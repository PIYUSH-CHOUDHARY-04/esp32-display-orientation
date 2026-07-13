/**
 * @file netlink_monitor.c
 * @ingroup pc_client
 * @brief Netlink watcher thread: detects when the Wi-Fi interface's IP changes or
 *        the link drops, so the main loop can rediscover the ESP.
 *
 * The PC finds the ESP by UDP broadcast on a specific Wi-Fi interface and caches that
 * interface's IP in @c gs.old_info. If the machine roams to another AP, the DHCP lease
 * changes, or the link goes down, the cached address becomes wrong and the TCP session
 * to the ESP is dead in the water. This thread exists purely to notice that and raise
 * a flag.
 *
 * ### Mechanism
 * A @c NETLINK_ROUTE socket subscribed to @c RTMGRP_LINK and @c RTMGRP_IPV4_IFADDR
 * receives kernel notifications for link and IPv4 address changes. Two message types
 * matter, both filtered down to the single interface discovery succeeded on
 * (@c monitored_ifindex):
 * - @c RTM_NEWADDR -- a new IPv4 address on the interface. If it differs from the
 *   cached @c gs.old_info.ip, our address moved: flag and exit.
 * - @c RTM_NEWLINK -- link state changed. If @c IFF_RUNNING is clear the carrier is
 *   gone: flag and exit.
 *
 * ### Multiple messages per recv()
 * A netlink socket is a datagram socket: it preserves message boundaries, but a single
 * @c recv() may still carry several @c nlmsghdr + payload records back to back:
 * @verbatim
 *   nlmsghdr #1 | payload #1 | nlmsghdr #2 | payload #2 | ... | nlmsghdr #n
 * @endverbatim
 * Hence the @c NLMSG_OK / @c NLMSG_NEXT walk rather than casting the buffer to one
 * message. Attributes within a message are walked the same way with @c RTA_OK /
 * @c RTA_NEXT.
 *
 * ### Shutdown
 * @c SO_RCVTIMEO is set to @c NL_RECV_TIMEOUT_US so @c recv() cannot block forever.
 * Each timeout is a chance to poll ::get_netlink_quit_status, which the main loop sets
 * when it wants the thread gone. Without the timeout the thread would sit in @c recv()
 * until the next kernel event, which on a quiet network might never come.
 *
 * ### Failure policy
 * Any unrecoverable socket error raises @c SIGTERM against the whole process. This
 * thread cannot repair a broken netlink socket and has no useful degraded mode:
 * without it, IP changes go unnoticed and the program silently talks to a stale
 * address. Signalling the process is deliberate -- the supervisor (systemd et al.)
 * restarts it clean.
 *
 * ### Logging
 * Developer-facing trace via ::PRINT_MSG (see log.h). Every function logs
 * @c LOG_LVL_DEBUG on entry and on each exit path, @c LOG_LVL_ERROR at each failure
 * (naming the failing call and @c errno), @c LOG_LVL_INFO at operational milestones,
 * and @c LOG_LVL_WARN on benign/recoverable conditions. The accessors
 * ::netlink_ip_changed and ::netlink_ip_reset are the exception: they sit on the
 * receive hot path and are not entry/exit traced.
 *
 * @note The @c ip_changed flag is @c atomic_bool: written here, read by the main
 *       thread, with no mutex.
 */

#define LOG_TAG "netlink"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include "netlink_monitor.h"
#include "main.h"
#include "log.h"

/**
 * @name Netlink parameters
 * @{
 */
#define NL_OUTPUT_SIZE      4096    /**< Receive buffer (bytes); one recv() may hold several messages. */
#define NL_RECV_TIMEOUT_US  400000  /**< recv() timeout (~400 ms); bounds the quit-flag poll interval. */
/** @} */

/**
 * @brief Set when an IP change or link-down is seen on the monitored interface.
 *
 * Atomic because it crosses the thread boundary: written here, read by the main thread
 * from ::netlink_ip_changed with no lock held.
 */
static atomic_bool ip_changed = false;

/**
 * @brief Test-and-clear the IP-changed flag.
 *
 * A single atomic exchange, so the flag is consumed exactly once even though the main
 * loop polls it from two places (before each @c recv() and again before each ACK
 * send).
 *
 * @note Called from the receive hot path; intentionally not entry/exit traced.
 *
 * @return @c true if a change was pending (and clears it); @c false otherwise.
 */
bool netlink_ip_changed(void){
    return atomic_exchange(&ip_changed, false);
}

/**
 * @brief Clear the IP-changed flag unconditionally.
 *
 * Called by the main loop before each rediscovery, so a change flagged during the
 * previous session cannot immediately tear down the new one.
 *
 * @note Called on the restart path; intentionally not entry/exit traced.
 */
void netlink_ip_reset(void){
    atomic_store(&ip_changed, false);
    return;
}

/**
 * @brief Thread entry: watch the discovered interface for IP/link changes.
 *
 * Opens and binds a @c NETLINK_ROUTE socket subscribed to the link and IPv4-address
 * groups, resolves the monitored interface index from @c gs.old_info.ifname, then
 * loops receiving and parsing kernel notifications.
 *
 * The thread is one-shot by design: on the first relevant change it sets the flag,
 * closes the socket and returns, letting the main loop join it and rediscover. There
 * is nothing to gain from watching further, since the session is already invalid.
 *
 * Inside the receive loop, @c EINTR and @c EAGAIN / @c EWOULDBLOCK are both benign:
 * the former is a signal interruption, the latter is @c SO_RCVTIMEO firing on an idle
 * network. Both simply loop back, and the timeout path is what gives the quit flag its
 * polling opportunity. Any other @c errno is unrecoverable and takes the @c SIGTERM
 * path.
 *
 * Within an @c RTM_NEWADDR message the attribute walk looks for @c IFA_LOCAL
 * specifically: that is the interface's own address, whereas @c IFA_ADDRESS would be
 * the peer address on a point-to-point link and is not what should be compared against
 * the cached IP. An address matching the cached one is a re-announcement (e.g. a DHCP
 * lease renewal) and is ignored, since nothing has actually moved.
 *
 * A link-down (@c IFF_RUNNING clear) is treated exactly like an IP change: the TCP
 * session is equally dead either way, and the recovery is identical.
 *
 * @warning @c gs.old_info must be populated (i.e. discovery must have succeeded)
 *          before this thread is started; @c if_nametoindex would otherwise fail on an
 *          empty name and the thread would @c SIGTERM the process.
 *
 * @note The parameter and return contract lives with the DECLARATION, in
 *       netlink_monitor.h -- not here. That is where a caller looks, and duplicating it
 *       would mean one contract with two copies free to drift apart.
 */
void* netlink_monitor(void* arg){
    PRINT_MSG(LOG_LVL_DEBUG, "netlink_monitor: entry");

    int nl_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if(nl_fd < 0){
        PRINT_MSG(LOG_LVL_ERROR, "netlink_monitor: socket failed, errno %d (%s)", errno, strerror(errno));
        raise(SIGTERM);
        PRINT_MSG(LOG_LVL_DEBUG, "netlink_monitor: exit (socket failed)");
        return NULL;
    }

    struct sockaddr_nl nls = {0};
    nls.nl_family = AF_NETLINK;
    nls.nl_pid    = getpid();
    nls.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR;

    if(bind(nl_fd, (struct sockaddr*)&nls, sizeof(struct sockaddr_nl)) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "netlink_monitor: bind failed, errno %d (%s)", errno, strerror(errno));
        goto err_cleanup;
    }

    struct timeval tv = {
        .tv_sec  = 0,
        .tv_usec = NL_RECV_TIMEOUT_US
    };

    if(setsockopt(nl_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "netlink_monitor: setsockopt SO_RCVTIMEO failed, errno %d (%s)", errno, strerror(errno));
        goto err_cleanup;
    }

    ssize_t recv_retval = 0;
    char nl_output[NL_OUTPUT_SIZE] = {0};

    unsigned int monitored_ifindex = if_nametoindex(gs.old_info.ifname);
    if(monitored_ifindex == 0){
        PRINT_MSG(LOG_LVL_ERROR, "netlink_monitor: if_nametoindex failed for '%s', errno %d (%s)",
                  gs.old_info.ifname, errno, strerror(errno));
        goto err_cleanup;
    }

    PRINT_MSG(LOG_LVL_INFO, "netlink_monitor: listening for IP/link events on %s (ifindex %u)",
              gs.old_info.ifname, monitored_ifindex);

    while(1){

        while(1){
            if(get_netlink_quit_status()){
                PRINT_MSG(LOG_LVL_INFO, "netlink_monitor: quit requested, stopping");
                close(nl_fd);
                PRINT_MSG(LOG_LVL_DEBUG, "netlink_monitor: exit (quit requested)");
                return NULL;
            }

            recv_retval = recv(nl_fd, nl_output, sizeof(char)*NL_OUTPUT_SIZE, 0);
            if(recv_retval < 0){
                if(errno == EINTR){
                    continue;
                }
                if(errno == EAGAIN || errno == EWOULDBLOCK){
                    continue;
                }
                PRINT_MSG(LOG_LVL_ERROR, "netlink_monitor: recv failed, errno %d (%s)", errno, strerror(errno));
                goto err_cleanup;
            }else{
                break;
            }
        }

        PRINT_MSG(LOG_LVL_DEBUG, "netlink_monitor: received %zd bytes, parsing messages", recv_retval);

        struct nlmsghdr *nlh;
        int remaining = (int)recv_retval;

        for(nlh = (struct nlmsghdr *)nl_output; NLMSG_OK(nlh, remaining); nlh = NLMSG_NEXT(nlh, remaining)){

            switch(nlh->nlmsg_type){
                case RTM_NEWADDR:
                {
                    struct ifaddrmsg *ifa = NLMSG_DATA(nlh);

                    if(ifa->ifa_index != monitored_ifindex){
                        break;
                    }

                    if(ifa->ifa_family != AF_INET){
                        break;
                    }

                    struct rtattr *rta;
                    int len = IFA_PAYLOAD(nlh);

                    for(rta = IFA_RTA(ifa); RTA_OK(rta, len); rta = RTA_NEXT(rta, len)){
                        if(rta->rta_type != IFA_LOCAL){
                            continue;
                        }

                        struct in_addr new_ip;
                        memcpy(&new_ip, RTA_DATA(rta), sizeof(new_ip));

                        if(memcmp(&new_ip, &gs.old_info.ip, sizeof(struct in_addr)) != 0){
                            PRINT_MSG(LOG_LVL_INFO, "netlink_monitor: IP changed on %s, flagging for rediscovery",
                                      gs.old_info.ifname);
                            atomic_store(&ip_changed, true);
                            close(nl_fd);
                            PRINT_MSG(LOG_LVL_DEBUG, "netlink_monitor: exit (IP changed)");
                            return NULL;
                        }

                        PRINT_MSG(LOG_LVL_DEBUG, "netlink_monitor: RTM_NEWADDR with unchanged IP, ignoring");
                        break;
                    }
                    break;
                }
                case RTM_NEWLINK:
                {
                    struct ifinfomsg *ifi = NLMSG_DATA(nlh);

                    if(ifi->ifi_index != monitored_ifindex){
                        break;
                    }

                    if(!(ifi->ifi_flags & IFF_RUNNING)){
                        PRINT_MSG(LOG_LVL_WARN, "netlink_monitor: link %s went down, flagging for rediscovery",
                                  gs.old_info.ifname);
                        atomic_store(&ip_changed, true);
                        close(nl_fd);
                        PRINT_MSG(LOG_LVL_DEBUG, "netlink_monitor: exit (link down)");
                        return NULL;
                    }

                    PRINT_MSG(LOG_LVL_DEBUG, "netlink_monitor: RTM_NEWLINK, %s still running", gs.old_info.ifname);
                    break;
                }
                default:
                    PRINT_MSG(LOG_LVL_DEBUG, "netlink_monitor: ignoring nlmsg_type %u", nlh->nlmsg_type);
                    break;
            }
        }
    }

err_cleanup:
    if(nl_fd >= 0){
        close(nl_fd);
    }
    PRINT_MSG(LOG_LVL_ERROR, "netlink_monitor: unrecoverable error, raising SIGTERM");
    raise(SIGTERM);
    PRINT_MSG(LOG_LVL_DEBUG, "netlink_monitor: exit (error)");
    return NULL;
}
