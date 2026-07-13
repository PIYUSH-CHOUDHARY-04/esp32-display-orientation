/**
 * @file main.c
 * @ingroup pc_client
 * @brief PC-side daemon: discovers the ESP over UDP broadcast, receives orientation
 *        commands over TCP, and rotates the external display with @c xrandr.
 *
 * The ESP32 watches its accelerometer and, whenever the board is physically rotated,
 * sends an orientation command. This program is the other half of that link: it finds
 * the ESP on the LAN, holds a TCP session open, and applies each command by forking
 * @c xrandr.
 *
 * ### Lifecycle (the outer loop in ::main)
 * @verbatim
 *   detect external output (once)
 *     -> discover_esp()      UDP broadcast on each Wi-Fi iface until an ACK arrives
 *     -> start_netlink()     watcher thread: flags IP change / link down
 *     -> start_tcp_client()  connect, then loop: recv command -> rotate -> ACK
 *     -> (on any failure)    stop watcher, join it, rediscover
 * @endverbatim
 * Every failure path funnels back to rediscovery rather than exiting: the ESP may have
 * rebooted, the AP may have changed, the lease may have moved. The daemon is expected
 * to run forever and heal itself.
 *
 * ### Discovery
 * The ESP's IP is not known ahead of time, so a @c MSG_DISCOVERY_REQ is broadcast to
 * the directed broadcast address of each up, non-loopback, wireless interface
 * (@c ip | @c ~mask). The ESP replies with @c MSG_DISCOVERY_ACK carrying its TCP port;
 * @c recvfrom fills in its address for free. The interface that worked is cached in
 * @c gs.old_info so the netlink watcher knows which one to monitor.
 *
 * ### Orientation state: intent vs reality
 * Two variables, and the distinction between them is the whole point:
 * - @c gs.last_orient    -- what is actually applied on screen (reality). Written ONLY
 *   when @c xrandr returned success. It is the dedup guard.
 * - @c gs.desired_orient -- what the ESP last asked for (intent). Written always.
 *
 * Caching a failed rotation into @c last_orient would be self-defeating: that variable
 * is exactly what the dedup check consults to decide "already applied, skip". Storing a
 * failure there tells the program the screen is already rotated when it is not, and the
 * guard then suppresses every future retry, leaving the display permanently wrong. So a
 * failure is recorded in @c desired_orient only, leaving @c last_orient stale; the
 * mismatch between the two is what ::__heal_orientation__ later notices and retries.
 * This matters because the ESP only transmits when the board actually moves: if a
 * rotation fails while X is unreachable, nothing would ever retrigger it.
 *
 * ### Logging
 * Developer-facing trace via ::PRINT_MSG (see log.h). Every function logs
 * @c LOG_LVL_DEBUG on entry and on each exit path, @c LOG_LVL_ERROR at each failure
 * (naming the failing call and @c errno), @c LOG_LVL_INFO at operational milestones,
 * and @c LOG_LVL_WARN on benign/recoverable conditions. Stream split and color are
 * handled by log.h; under an init system the output is automatically plain text on
 * fd 1 / fd 2, so the service definition decides where it lands.
 *
 * @note This is the one translation unit that defines @c LOG_IMPLEMENTATION, so it owns
 *       the storage for @c _log_runtime_level.
 */

#define LOG_TAG "main"
#define LOG_IMPLEMENTATION

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdlib.h>
#include <netinet/tcp.h>
#include <sys/wait.h>

#include "main.h"
#include "common.h"
#include "netlink_monitor.h"
#include "log.h"

/**
 * @name Discovery parameters
 * @{
 */
#define DISCOVERY_RECV_TIMEOUT_MS   200   /**< SO_RCVTIMEO on the discovery socket. */
#define DISCOVERY_RECV_COUNTER_MAX  5     /**< ACK wait iterations per broadcast (~1 s total). */
#define MAX_WIFI_INTERFACES         4     /**< Cap on interfaces enumerated. */
#define IFACE_TRIAL_MAX             3     /**< Broadcast attempts per interface. */
/** @} */

/**
 * @name Command-session parameters
 * @{
 */
#define CMD_RECV_TIMEOUT_MS         500   /**< SO_RCVTIMEO on the TCP socket; also the heal tick. */
/** @} */

/**
 * @name TCP keepalive tuning
 * @brief Aggressive dead-peer detection: ~4 s to notice a silently vanished ESP,
 *        instead of the kernel default of hours.
 * @{
 */
#define KEEPALIVE_IDLE_S            1      /**< Start probing after 1 s idle. */
#define KEEPALIVE_INTVL_S           1      /**< Probe every 1 s. */
#define KEEPALIVE_CNT               3      /**< 3 unanswered probes -> dead: idle + (intvl * cnt) ~= 4 s. */
#define KEEPALIVE_USER_TIMEOUT_MS   5000   /**< Give up on unACKed in-flight data after ~5 s. */
/** @} */

/** @brief Process-wide state (single instance, shared with the netlink thread). */
struct global_state gs = {
    .esp_tcp_addr   = {0},
    .old_info       = {0},
    .quit_netlink   = false,
    .last_orient    = ORIENT_INVALID,
    .desired_orient = ORIENT_INVALID,
};

static int  __recv_exact__(int fd, void* buf, size_t len);
static int  __rotate_display__(display_orient_t orient);
static void __heal_orientation__(void);

/* ================================ discovery ================================ */

/**
 * @brief Enumerate up, non-loopback, IPv4 Wi-Fi interfaces.
 *
 * Walks @c getifaddrs and keeps only interfaces that are up, not loopback, carry an
 * IPv4 address (the ESP protocol is IPv4 only), and are wireless. "Wireless" is decided
 * by the existence of @c /sys/class/net/\<if\>/wireless, which the kernel creates only for
 * 802.11 devices -- cheaper and more reliable than an ioctl probe. Each survivor's name,
 * address and netmask are recorded; the mask is needed to compute the directed broadcast
 * address. An interface with no netmask is skipped for that reason.
 *
 * @note The two address log lines are separate @c PRINT_MSG calls on purpose:
 *       @c inet_ntoa returns a pointer into one static buffer, so two calls in a single
 *       format would both render the second address.
 *
 * @param iface_arr Output array, at least @c MAX_WIFI_INTERFACES entries.
 * @return Number of interfaces filled in (may be 0), or -1 if @c getifaddrs failed.
 */
static int __get_wifi_interfaces__(struct wifi_iface_info* const iface_arr){
    PRINT_MSG(LOG_LVL_DEBUG, "__get_wifi_interfaces__: entry");
    struct ifaddrs *ifaddr;
    struct ifaddrs *ifa;
    uint8_t iface_count = 0;

    if(getifaddrs(&ifaddr) == -1){
        PRINT_MSG(LOG_LVL_ERROR, "__get_wifi_interfaces__: getifaddrs failed, errno %d (%s)", errno, strerror(errno));
        PRINT_MSG(LOG_LVL_DEBUG, "__get_wifi_interfaces__: exit (-1)");
        return -1;
    }

    for(ifa = ifaddr; ifa && iface_count < MAX_WIFI_INTERFACES; ifa = ifa->ifa_next){
        char path[256];

        if(!ifa->ifa_addr){
            continue;
        }
        if(ifa->ifa_addr->sa_family != AF_INET){
            continue;
        }
        if(!(ifa->ifa_flags & IFF_UP)){
            continue;
        }
        if(ifa->ifa_flags & IFF_LOOPBACK){
            continue;
        }

        snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", ifa->ifa_name);
        if(access(path, F_OK) != 0){
            continue;
        }

        struct wifi_iface_info *iface = &iface_arr[iface_count];
        memset(iface, 0, sizeof(*iface));
        strncpy(iface->ifname, ifa->ifa_name, IFNAMSIZ - 1);

        if(!ifa->ifa_netmask){
            PRINT_MSG(LOG_LVL_WARN, "__get_wifi_interfaces__: %s has no netmask, skipping", ifa->ifa_name);
            continue;
        }

        iface->ip   = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
        iface->mask = ((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr;

        PRINT_MSG(LOG_LVL_INFO,  "__get_wifi_interfaces__: found wifi interface %s", iface->ifname);
        PRINT_MSG(LOG_LVL_DEBUG, "__get_wifi_interfaces__:   ip   %s", inet_ntoa(iface->ip));
        PRINT_MSG(LOG_LVL_DEBUG, "__get_wifi_interfaces__:   mask %s", inet_ntoa(iface->mask));

        iface_count++;
    }

    freeifaddrs(ifaddr);
    PRINT_MSG(LOG_LVL_DEBUG, "__get_wifi_interfaces__: exit (%u interfaces)", iface_count);
    return iface_count;
}

/**
 * @brief Create the UDP socket used for ESP discovery.
 *
 * Enables @c SO_BROADCAST (required to send to a directed broadcast address) and sets
 * @c SO_RCVTIMEO so the ACK wait cannot block forever -- a bounded @c recvfrom is what
 * lets the caller give up on one interface and move to the next.
 *
 * @note No @c bind is done: an ephemeral source port is fine, since the ESP replies to
 *       whatever source address and port the request arrived from.
 *
 * @param[out] fd Receives the socket fd on success, or -1 on any failure.
 * @return 0 on success; -1 on failure (the fd is closed and @p fd set to -1).
 */
static int __create_udp_socket__(int* fd){
    PRINT_MSG(LOG_LVL_DEBUG, "__create_udp_socket__: entry");

    int udp_sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(udp_sock_fd < 0){
        PRINT_MSG(LOG_LVL_ERROR, "__create_udp_socket__: socket failed, errno %d (%s)", errno, strerror(errno));
        *fd = -1;
        PRINT_MSG(LOG_LVL_DEBUG, "__create_udp_socket__: exit (-1)");
        return -1;
    }

    int one = 1;
    if(setsockopt(udp_sock_fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "__create_udp_socket__: setsockopt SO_BROADCAST failed, errno %d (%s)", errno, strerror(errno));
        *fd = -1;
        close(udp_sock_fd);
        PRINT_MSG(LOG_LVL_DEBUG, "__create_udp_socket__: exit (-1)");
        return -1;
    }

    struct timeval tv = {
        .tv_sec  = DISCOVERY_RECV_TIMEOUT_MS / 1000,
        .tv_usec = (DISCOVERY_RECV_TIMEOUT_MS % 1000) * 1000,
    };

    if(setsockopt(udp_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "__create_udp_socket__: setsockopt SO_RCVTIMEO failed, errno %d (%s)", errno, strerror(errno));
        *fd = -1;
        close(udp_sock_fd);
        PRINT_MSG(LOG_LVL_DEBUG, "__create_udp_socket__: exit (-1)");
        return -1;
    }

    *fd = udp_sock_fd;
    PRINT_MSG(LOG_LVL_DEBUG, "__create_udp_socket__: exit (fd %d)", udp_sock_fd);
    return 0;
}

/**
 * @brief Locate the ESP on the LAN by UDP broadcast, on each Wi-Fi interface in turn.
 *
 * For every interface found by ::__get_wifi_interfaces__, broadcasts a
 * @c MSG_DISCOVERY_REQ to that interface's directed broadcast address (@c ip | @c ~mask,
 * i.e. host bits all ones) and waits for a @c MSG_DISCOVERY_ACK. Up to
 * @c IFACE_TRIAL_MAX broadcasts per interface, each followed by up to
 * @c DISCOVERY_RECV_COUNTER_MAX bounded receives.
 *
 * The ESP's IP arrives for free: @c recvfrom writes the sender's address straight into
 * @c gs.esp_tcp_addr. Only the port has to be taken from the ACK payload, since the
 * ESP's TCP listener sits on a different port than its UDP responder. @c sin_family is
 * reassigned afterwards -- redundant, but it leaves the address unambiguous.
 *
 * Received packets are validated on length, checksum, magic and message id; anything
 * failing those is a stale or foreign datagram and is skipped without ending the wait.
 * A @c sendto failure, by contrast, means the network went down between @c getifaddrs
 * and now, so that interface is abandoned and the next one tried.
 *
 * On success the winning interface is cached in @c gs.old_info: the netlink watcher
 * needs to know which interface to monitor.
 *
 * @note This function makes exactly one pass over the interfaces. Retrying the whole
 *       sweep is the outer @c while(1) in ::main's job, not this function's.
 *
 * @return 0 if the ESP was found (@c gs.esp_tcp_addr and @c gs.old_info populated);
 *         -1 if no interface yielded an ACK, or if enumeration failed.
 */
static int discover_esp(void){
    PRINT_MSG(LOG_LVL_DEBUG, "discover_esp: entry");
    PRINT_MSG(LOG_LVL_INFO, "discover_esp: discovering ESP-WROOM-32...");

    struct wifi_iface_info iface_arr[MAX_WIFI_INTERFACES];
    int8_t active_interfaces = __get_wifi_interfaces__(iface_arr);

    if(active_interfaces < 0){
        PRINT_MSG(LOG_LVL_ERROR, "discover_esp: __get_wifi_interfaces__ failed, retval %d", active_interfaces);
        PRINT_MSG(LOG_LVL_DEBUG, "discover_esp: exit (-1)");
        return -1;
    }
    PRINT_MSG(LOG_LVL_INFO, "discover_esp: %d wifi interface(s) found", active_interfaces);

    int udp_sock_fd = -1;

    struct sockaddr_in brdcst_addr = {0};
    brdcst_addr.sin_family = AF_INET;
    brdcst_addr.sin_port   = htons(ESP_COMM_PORT_UDP);
    socklen_t recv_socklen = sizeof(struct sockaddr_in);

    proto_packet_t pkt = {
        .header.magic  = MAGIC_BYTES,
        .header.msg_id = MSG_DISCOVERY_REQ,
        .header.length = sizeof(discovery_req_t),
        .header.chksum = 0,
    };
    strncpy(pkt.payload.discovery_req.name, NAME, NAME_SIZE);
    pkt.header.chksum = proto_generate_chksum(&pkt);

    proto_packet_t ack = {0};
    bool skip_interface = false;

    for(int8_t iface_iterator = 0; iface_iterator < active_interfaces; iface_iterator++){
        PRINT_MSG(LOG_LVL_INFO, "discover_esp: scanning over %s", iface_arr[iface_iterator].ifname);
        skip_interface = false;

        int retval = __create_udp_socket__(&udp_sock_fd);
        if(retval < 0){
            PRINT_MSG(LOG_LVL_ERROR, "discover_esp: __create_udp_socket__ failed, retval %d", retval);
            PRINT_MSG(LOG_LVL_DEBUG, "discover_esp: exit (-1)");
            return -1;
        }

        brdcst_addr.sin_addr.s_addr = (iface_arr[iface_iterator].ip.s_addr) | ~(iface_arr[iface_iterator].mask.s_addr);

        for(uint8_t iface_trial = 0; iface_trial < IFACE_TRIAL_MAX; iface_trial++){
            PRINT_MSG(LOG_LVL_DEBUG, "discover_esp: broadcast attempt %u on %s",
                      iface_trial, iface_arr[iface_iterator].ifname);

            if(sendto(udp_sock_fd, &pkt, sizeof(proto_hdr_t) + sizeof(discovery_req_t), 0,
                      (struct sockaddr*)&brdcst_addr, sizeof(struct sockaddr_in)) < 0){
                PRINT_MSG(LOG_LVL_WARN, "discover_esp: sendto failed on %s, errno %d (%s), skipping interface",
                          iface_arr[iface_iterator].ifname, errno, strerror(errno));
                close(udp_sock_fd);
                udp_sock_fd = -1;
                skip_interface = true;
                break;
            }

            for(uint8_t discov_counter = 0; discov_counter < DISCOVERY_RECV_COUNTER_MAX; discov_counter++){
                ssize_t n = recvfrom(udp_sock_fd, &ack, sizeof(proto_packet_t), 0,
                                     (struct sockaddr*)&(gs.esp_tcp_addr), &recv_socklen);
                if(n < 0){
                    int err = errno;
                    if(err == EINTR){
                        continue;
                    }
                    if(err == EAGAIN || err == EWOULDBLOCK){
                        PRINT_MSG(LOG_LVL_DEBUG, "discover_esp: no ack yet (attempt %u)", discov_counter);
                        continue;
                    }
                    PRINT_MSG(LOG_LVL_ERROR, "discover_esp: recvfrom failed on %s, errno %d (%s)",
                              iface_arr[iface_iterator].ifname, err, strerror(err));
                    memset(&(gs.esp_tcp_addr), 0, sizeof(struct sockaddr_in));
                    close(udp_sock_fd);
                    udp_sock_fd = -1;
                    skip_interface = true;
                    break;
                }

                if(n < (ssize_t)(sizeof(proto_hdr_t) + sizeof(discovery_ack_t))){
                    PRINT_MSG(LOG_LVL_WARN, "discover_esp: short packet (%zd bytes), discarding", n);
                    continue;
                }
                if(!proto_verify_chksum(&ack)){
                    PRINT_MSG(LOG_LVL_WARN, "discover_esp: bad checksum, discarding packet");
                    continue;
                }
                if(ack.header.magic != MAGIC_BYTES || ack.header.msg_id != MSG_DISCOVERY_ACK){
                    PRINT_MSG(LOG_LVL_WARN, "discover_esp: unexpected magic/msg_id, discarding packet");
                    continue;
                }

                gs.esp_tcp_addr.sin_family = AF_INET;
                gs.esp_tcp_addr.sin_port   = htons(ack.payload.discovery_ack.tcp_port);

                memcpy(&gs.old_info, &iface_arr[iface_iterator], sizeof(struct wifi_iface_info));

                PRINT_MSG(LOG_LVL_INFO, "discover_esp: ESP found on %s at %s:%u",
                          iface_arr[iface_iterator].ifname,
                          inet_ntoa(gs.esp_tcp_addr.sin_addr),
                          ack.payload.discovery_ack.tcp_port);

                close(udp_sock_fd);
                PRINT_MSG(LOG_LVL_DEBUG, "discover_esp: exit (0)");
                return 0;
            }

            if(skip_interface){
                break;
            }
        }

        if(skip_interface){
            continue;
        }

        close(udp_sock_fd);
        udp_sock_fd = -1;
    }

    PRINT_MSG(LOG_LVL_ERROR, "discover_esp: ESP-WROOM-32 not found on any wifi interface");
    if(udp_sock_fd >= 0){
        close(udp_sock_fd);
    }
    PRINT_MSG(LOG_LVL_DEBUG, "discover_esp: exit (-1)");
    return -1;
}

/* =============================== TCP session =============================== */

/**
 * @brief Configure aggressive TCP keepalive and user timeout on a connected socket.
 *
 * Two distinct failure modes need bounding, and they need different mechanisms:
 * - An @b idle connection whose peer silently vanished (ESP powered off, crashed): only
 *   keepalive probes detect this. Tuned to roughly
 *   @c KEEPALIVE_IDLE_S + (@c KEEPALIVE_INTVL_S * @c KEEPALIVE_CNT), about 4 s.
 * - A connection with @b data @b in @b flight that is never ACKed: keepalive does not
 *   fire here, because the socket is not idle. @c TCP_USER_TIMEOUT bounds retransmission
 *   to ~5 s instead of the kernel's default of minutes.
 *
 * Without both, a dead ESP would only be noticed after the default timeouts -- far too
 * long for a display currently rotated the wrong way.
 *
 * @param fd Connected TCP socket.
 * @return 0 on success; -1 if any @c setsockopt failed.
 */
static int __set_keepalive__(int fd){
    PRINT_MSG(LOG_LVL_DEBUG, "__set_keepalive__: entry (fd %d)", fd);

    int yes   = 1;
    int idle  = KEEPALIVE_IDLE_S;
    int intvl = KEEPALIVE_INTVL_S;
    int cnt   = KEEPALIVE_CNT;

    if(setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes)) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "__set_keepalive__: setsockopt SO_KEEPALIVE failed, errno %d (%s)", errno, strerror(errno));
        goto err;
    }
    if(setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "__set_keepalive__: setsockopt TCP_KEEPIDLE failed, errno %d (%s)", errno, strerror(errno));
        goto err;
    }
    if(setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl)) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "__set_keepalive__: setsockopt TCP_KEEPINTVL failed, errno %d (%s)", errno, strerror(errno));
        goto err;
    }
    if(setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt)) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "__set_keepalive__: setsockopt TCP_KEEPCNT failed, errno %d (%s)", errno, strerror(errno));
        goto err;
    }

    unsigned int user_timeout_ms = KEEPALIVE_USER_TIMEOUT_MS;
    if(setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout_ms, sizeof(user_timeout_ms)) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "__set_keepalive__: setsockopt TCP_USER_TIMEOUT failed, errno %d (%s)", errno, strerror(errno));
        goto err;
    }

    PRINT_MSG(LOG_LVL_DEBUG, "__set_keepalive__: exit (0)");
    return 0;

err:
    PRINT_MSG(LOG_LVL_DEBUG, "__set_keepalive__: exit (-1)");
    return -1;
}

/**
 * @brief Receive exactly @p len bytes, or fail.
 *
 * TCP is a byte stream, not a message stream: one @c recv() may return only part of a
 * packet. This loops until the full fixed-size command has arrived.
 *
 * @c SO_RCVTIMEO makes each @c recv() return @c EAGAIN after @c CMD_RECV_TIMEOUT_MS
 * rather than blocking forever, and that timeout is load-bearing rather than incidental:
 * it is the tick on which the IP-change flag is polled and on which
 * ::__heal_orientation__ gets a chance to retry a previously failed rotation. A timeout
 * arriving mid-packet is therefore NOT an error -- it loops back and keeps waiting for
 * the remainder. @c EINTR is likewise benign.
 *
 * The IP-change check happens before each @c recv() because if our address moved, this
 * socket is bound to an address that is no longer ours; there is no point reading from
 * it, and the session must end so ::main can rediscover.
 *
 * @param fd  Connected socket.
 * @param buf Destination buffer, at least @p len bytes.
 * @param len Exact number of bytes to read.
 * @return 0 on success; -1 on a real socket error or a detected IP change; -2 if the
 *         peer closed the connection cleanly.
 */
static int __recv_exact__(int fd, void* buf, size_t len){
    uint8_t* p   = (uint8_t*)buf;
    size_t   got = 0;

    while(got < len){
        if(netlink_ip_changed()){
            PRINT_MSG(LOG_LVL_WARN, "__recv_exact__: IP change detected, aborting session for rediscovery");
            return -1;
        }

        __heal_orientation__();

        ssize_t n = recv(fd, p + got, len - got, 0);
        if(n > 0){
            got += (size_t)n;
            continue;
        }
        if(n == 0){
            PRINT_MSG(LOG_LVL_WARN, "__recv_exact__: peer closed the connection");
            return -2;
        }
        if(errno == EINTR){
            continue;
        }
        if(errno == EAGAIN || errno == EWOULDBLOCK){
            continue;
        }

        PRINT_MSG(LOG_LVL_ERROR, "__recv_exact__: recv failed, errno %d (%s)", errno, strerror(errno));
        return -1;
    }
    return 0;
}

/* ============================= display rotation ============================ */

/**
 * @brief Apply an orientation to the external display by forking @c xrandr.
 *
 * Maps the protocol orientation onto an @c xrandr rotation keyword and execs
 * @c "xrandr --output \<monitor\> --rotate \<dir\>" in a child, waiting for it to finish.
 * @c DISPLAY is forced to @c ":0" in the child because a daemon inherits no X
 * environment from its supervisor.
 *
 * The @c ORIENT_90 and @c ORIENT_270 keyword mapping is the one thing here that must be
 * calibrated against the physical board: if the screen turns the wrong way, swap
 * @c "left" and @c "right".
 *
 * The distinct negative return codes let the caller tell a permanent configuration
 * problem (unknown orientation) from a transient one (no monitor resolved yet, X
 * unreachable). The caller nevertheless treats every non-zero value as "did not apply",
 * which is exactly what keeps @c gs.last_orient honest.
 *
 * @warning Run as a service rather than from a session terminal, @c DISPLAY alone is
 *          often not enough: X will also refuse the connection without @c XAUTHORITY
 *          pointing at the session owner's cookie. That is the single most likely cause
 *          of a rotation failing under systemd while working fine from a shell.
 *
 * @note The child's @c _exit(127) is only reached if @c execlp itself failed. It cannot
 *       log there (that would mean a @c PRINT_MSG from a half-execed child), so it uses
 *       the conventional "command not found" status and lets the parent report it from
 *       the exit code.
 *
 * @param orient Orientation to apply.
 * @return 0 on success; -1 unknown orientation; -2 no external monitor resolved;
 *         -3 @c fork failed; -4 @c xrandr exited non-zero.
 */
static int __rotate_display__(display_orient_t orient){
    PRINT_MSG(LOG_LVL_DEBUG, "__rotate_display__: entry (orient %d)", (int)orient);

    const char *dir = NULL;
    switch(orient){
        case ORIENT_0:   dir = "normal";   break;
        case ORIENT_90:  dir = "left";     break;
        case ORIENT_180: dir = "inverted"; break;
        case ORIENT_270: dir = "right";    break;
        default:
            PRINT_MSG(LOG_LVL_ERROR, "__rotate_display__: unknown orientation %d, ignoring", (int)orient);
            PRINT_MSG(LOG_LVL_DEBUG, "__rotate_display__: exit (-1)");
            return -1;
    }

    if(gs.monitor_ext[0] == '\0'){
        PRINT_MSG(LOG_LVL_WARN, "__rotate_display__: no external monitor name set, skipping rotation");
        PRINT_MSG(LOG_LVL_DEBUG, "__rotate_display__: exit (-2)");
        return -2;
    }

    pid_t pid = fork();
    if(pid < 0){
        PRINT_MSG(LOG_LVL_ERROR, "__rotate_display__: fork failed, errno %d (%s)", errno, strerror(errno));
        PRINT_MSG(LOG_LVL_DEBUG, "__rotate_display__: exit (-3)");
        return -3;
    }

    if(pid == 0){
        setenv("DISPLAY", ":0", 1);
        execlp("xrandr", "xrandr",
               "--output", gs.monitor_ext,
               "--rotate", dir, (char*)NULL);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if(WIFEXITED(status) && WEXITSTATUS(status) != 0){
        PRINT_MSG(LOG_LVL_ERROR, "__rotate_display__: xrandr exited %d, rotation not applied", WEXITSTATUS(status));
        PRINT_MSG(LOG_LVL_DEBUG, "__rotate_display__: exit (-4)");
        return -4;
    }

    PRINT_MSG(LOG_LVL_INFO, "__rotate_display__: display rotated to %s", dir);
    PRINT_MSG(LOG_LVL_DEBUG, "__rotate_display__: exit (0)");
    return 0;
}

/**
 * @brief Retry a rotation that was requested but never successfully applied.
 *
 * Called on the @c recv() timeout tick (roughly every @c CMD_RECV_TIMEOUT_MS). Does
 * nothing unless intent and reality disagree, i.e. @c gs.desired_orient differs from
 * @c gs.last_orient, and nothing has been requested yet when @c desired_orient is still
 * @c ORIENT_INVALID. The equal case is the common one, so this is a cheap no-op on
 * almost every tick.
 *
 * This closes the gap left by the ESP's send policy: the firmware transmits only when
 * the board physically moves. If a rotation failed because X was unreachable or the
 * monitor was unplugged, nothing would ever ask again -- the display would stay wrong
 * until someone picked the board up. Here the daemon reapplies it by itself the moment
 * the environment recovers.
 *
 * @c gs.last_orient is updated only on success, so a still-failing retry simply leaves
 * the mismatch in place and the next tick tries again.
 *
 * @note Fires on the receive timeout path; intentionally not entry/exit traced, since
 *       the no-op case would otherwise log twice a second forever.
 */
static void __heal_orientation__(void){
    if(gs.desired_orient == gs.last_orient){
        return;
    }
    if(gs.desired_orient == ORIENT_INVALID){
        return;
    }

    PRINT_MSG(LOG_LVL_DEBUG, "__heal_orientation__: retrying unapplied orientation %d (on screen %d)",
              (int)gs.desired_orient, (int)gs.last_orient);

    if(__rotate_display__(gs.desired_orient) == 0){
        gs.last_orient = gs.desired_orient;
        PRINT_MSG(LOG_LVL_INFO, "__heal_orientation__: recovered, orientation %d now applied", (int)gs.last_orient);
    }
}

/**
 * @brief Connect to the ESP and run the command loop until the session breaks.
 *
 * Connects to the address discovered by ::discover_esp, arms keepalive, then loops:
 * receive one @c MSG_ORIENT_CMD, validate it, apply the rotation if it differs from what
 * is on screen, and ACK it back echoing the command's sequence number and boot epoch.
 * That pair is what lets the ESP match an ACK to its command and reject stale ones from
 * before a reboot.
 *
 * The ACK header (@c magic, @c msg_id, @c length) is constant for the whole session and
 * is therefore built once, outside the loop; only the sequence fields and the checksum
 * change per packet. @c MSG_NOSIGNAL on the send makes a dead peer return @c EPIPE
 * rather than killing the process with @c SIGPIPE.
 *
 * ### Why the dedup cache is written only on success
 * @c gs.last_orient means "what is actually on screen". The guard
 * @c "if(desired != last_orient)" reads it to decide whether the rotation is already
 * applied. Writing a @b failed orientation into it would therefore be actively harmful:
 * the program would believe the screen is already rotated, and the guard would suppress
 * every subsequent attempt, leaving the display permanently wrong. So the write is gated
 * on @c "== 0", and the ESP's request is retained in @c gs.desired_orient for
 * ::__heal_orientation__ to retry.
 *
 * The IP flag is re-checked after the rotation and before the ACK send, because it may
 * have flipped while the process was busy forking @c xrandr; sending on a socket bound
 * to an address that is no longer ours is pointless.
 *
 * A malformed or foreign packet is skipped without tearing down the session. Only a
 * socket-level failure or an IP change ends it.
 *
 * @return -1 on any session failure (socket error, peer close, IP change). Never returns
 *         0: the loop exits only on failure, and ::main rediscovers.
 */
static int start_tcp_client(void){
    PRINT_MSG(LOG_LVL_DEBUG, "start_tcp_client: entry");

    int tcp_sock_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(tcp_sock_fd < 0){
        PRINT_MSG(LOG_LVL_ERROR, "start_tcp_client: socket failed, errno %d (%s)", errno, strerror(errno));
        PRINT_MSG(LOG_LVL_DEBUG, "start_tcp_client: exit (-1)");
        return -1;
    }

    struct timeval tv = {
        .tv_sec  = CMD_RECV_TIMEOUT_MS / 1000,
        .tv_usec = (CMD_RECV_TIMEOUT_MS % 1000) * 1000,
    };

    if(setsockopt(tcp_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "start_tcp_client: setsockopt SO_RCVTIMEO failed, errno %d (%s)", errno, strerror(errno));
        close(tcp_sock_fd);
        PRINT_MSG(LOG_LVL_DEBUG, "start_tcp_client: exit (-1)");
        return -1;
    }

    if(connect(tcp_sock_fd, (struct sockaddr*)&gs.esp_tcp_addr, sizeof(struct sockaddr_in)) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "start_tcp_client: connect failed, errno %d (%s), ESP server not responding",
                  errno, strerror(errno));
        close(tcp_sock_fd);
        PRINT_MSG(LOG_LVL_DEBUG, "start_tcp_client: exit (-1)");
        return -1;
    }

    if(__set_keepalive__(tcp_sock_fd) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "start_tcp_client: __set_keepalive__ failed");
        close(tcp_sock_fd);
        PRINT_MSG(LOG_LVL_DEBUG, "start_tcp_client: exit (-1)");
        return -1;
    }

    PRINT_MSG(LOG_LVL_INFO, "start_tcp_client: connected to ESP, waiting for orientation commands");

    proto_packet_t rx_pkt = {0};
    proto_packet_t tx_pkt = {0};

    tx_pkt.header.magic  = MAGIC_BYTES;
    tx_pkt.header.msg_id = MSG_ORIENT_ACK;
    tx_pkt.header.length = sizeof(orient_ack_t);

    while(1){
        int retval = __recv_exact__(tcp_sock_fd, &rx_pkt, sizeof(proto_hdr_t) + sizeof(orient_cmd_t));
        if(retval < 0){
            PRINT_MSG(LOG_LVL_ERROR, "start_tcp_client: __recv_exact__ failed, retval %d, ending session", retval);
            close(tcp_sock_fd);
            PRINT_MSG(LOG_LVL_DEBUG, "start_tcp_client: exit (-1)");
            return -1;
        }

        if(!proto_verify_chksum(&rx_pkt)){
            PRINT_MSG(LOG_LVL_WARN, "start_tcp_client: bad checksum, discarding packet");
            continue;
        }
        if(rx_pkt.header.magic != MAGIC_BYTES || rx_pkt.header.msg_id != MSG_ORIENT_CMD){
            PRINT_MSG(LOG_LVL_WARN, "start_tcp_client: unexpected magic/msg_id, discarding packet");
            continue;
        }

        display_orient_t orient = rx_pkt.payload.orient_cmd.orientation;
        PRINT_MSG(LOG_LVL_INFO, "start_tcp_client: orientation command received: %d", (int)orient);

        gs.desired_orient = orient;

        if(gs.desired_orient != gs.last_orient){
            PRINT_MSG(LOG_LVL_DEBUG, "start_tcp_client: applying orientation %d (on screen %d)",
                      (int)gs.desired_orient, (int)gs.last_orient);
            if(__rotate_display__(gs.desired_orient) == 0){
                gs.last_orient = gs.desired_orient;
            }else{
                PRINT_MSG(LOG_LVL_WARN, "start_tcp_client: rotation failed, will retry (desired %d, on screen %d)",
                          (int)gs.desired_orient, (int)gs.last_orient);
            }
        }else{
            PRINT_MSG(LOG_LVL_DEBUG, "start_tcp_client: orientation %d already applied, skipping", (int)orient);
        }

        tx_pkt.payload.orient_ack.seq.seq_us     = rx_pkt.payload.orient_cmd.seq.seq_us;
        tx_pkt.payload.orient_ack.seq.boot_epoch = rx_pkt.payload.orient_cmd.seq.boot_epoch;
        tx_pkt.header.chksum = proto_generate_chksum(&tx_pkt);

        if(netlink_ip_changed()){
            PRINT_MSG(LOG_LVL_WARN, "start_tcp_client: IP change detected, ending session for rediscovery");
            close(tcp_sock_fd);
            PRINT_MSG(LOG_LVL_DEBUG, "start_tcp_client: exit (-1)");
            return -1;
        }

        PRINT_MSG(LOG_LVL_DEBUG, "start_tcp_client: sending ACK (seq_us %llu)",
                  (unsigned long long)tx_pkt.payload.orient_ack.seq.seq_us);

        ssize_t sent = send(tcp_sock_fd, &tx_pkt, sizeof(proto_hdr_t) + sizeof(orient_ack_t), MSG_NOSIGNAL);
        if(sent < 0){
            PRINT_MSG(LOG_LVL_ERROR, "start_tcp_client: send failed, errno %d (%s), ending session",
                      errno, strerror(errno));
            close(tcp_sock_fd);
            PRINT_MSG(LOG_LVL_DEBUG, "start_tcp_client: exit (-1)");
            return -1;
        }
        PRINT_MSG(LOG_LVL_DEBUG, "start_tcp_client: ACK sent (%zd bytes)", sent);
    }
}

/* ============================== netlink thread ============================= */

/**
 * @brief Start the netlink watcher thread.
 *
 * @note @c pthread_create returns the error number directly and does not set @c errno,
 *       hence @c strerror(retval) rather than @c strerror(errno).
 *
 * @param[out] tid Receives the thread id on success.
 * @return 0 on success; -1 if @c pthread_create failed.
 */
static int start_netlink(pthread_t* tid){
    PRINT_MSG(LOG_LVL_DEBUG, "start_netlink: entry");

    int retval = pthread_create(tid, NULL, netlink_monitor, NULL);
    if(retval != 0){
        PRINT_MSG(LOG_LVL_ERROR, "start_netlink: pthread_create failed, retval %d (%s)", retval, strerror(retval));
        PRINT_MSG(LOG_LVL_DEBUG, "start_netlink: exit (-1)");
        return -1;
    }

    PRINT_MSG(LOG_LVL_INFO, "start_netlink: netlink watcher started");
    PRINT_MSG(LOG_LVL_DEBUG, "start_netlink: exit (0)");
    return 0;
}

/**
 * @brief Join the netlink watcher thread.
 *
 * Must be called before starting another, or every rediscovery cycle would leak a
 * thread's resources.
 *
 * @note As with ::start_netlink, @c pthread_join returns the error number directly.
 *
 * @param tid Thread id from ::start_netlink.
 * @return 0 on success; -1 if @c pthread_join failed.
 */
static int wait_for_thread(pthread_t tid){
    PRINT_MSG(LOG_LVL_DEBUG, "wait_for_thread: entry");

    int retval = pthread_join(tid, NULL);
    if(retval != 0){
        PRINT_MSG(LOG_LVL_ERROR, "wait_for_thread: pthread_join failed, retval %d (%s)", retval, strerror(retval));
        PRINT_MSG(LOG_LVL_DEBUG, "wait_for_thread: exit (-1)");
        return -1;
    }

    PRINT_MSG(LOG_LVL_DEBUG, "wait_for_thread: exit (0)");
    return 0;
}

/**
 * @brief Request the netlink watcher to stop, or clear a previous request.
 *
 * @note Trivial atomic store on the restart path; intentionally not entry/exit traced.
 *
 * @param status @c true to ask the thread to exit; @c false to clear the request.
 */
static void set_netlink_quit_status(bool status){
    atomic_store(&gs.quit_netlink, status);
}

/**
 * @brief Read the quit request. Polled by the watcher on each of its recv timeouts.
 *
 * @note Sits on the watcher's receive loop; intentionally not entry/exit traced.
 *
 * @return @c true if the thread has been asked to exit.
 */
bool get_netlink_quit_status(void){
    return atomic_load(&gs.quit_netlink);
}

/* ============================= display discovery =========================== */

/**
 * @brief Resolve the name of the first connected external output (e.g. "HDMI1").
 *
 * Runs @c "xrandr --query" in a child with its stdout redirected onto a pipe, then scans
 * the output for a line of the form @c "NAME connected ...". Internal laptop panels
 * (@c eDP*, @c LVDS*) are skipped: rotating those is never what is wanted here.
 *
 * The parent closes the write end of the pipe before reading, or the @c read loop would
 * never see EOF -- it would still hold a writer open itself.
 *
 * @note The leading space in the @c " connected" match is deliberate and load-bearing:
 *       without it, the string @c "disconnected" would also match, and the first
 *       unplugged output would be selected.
 *
 * @param[out] out_name Receives the output name, NUL-terminated.
 * @param out_sz Size of @p out_name.
 * @return 0 if an external output was found; -1 if none was, or if @c pipe / @c fork
 *         failed.
 */
static int detect_external_output(char *out_name, size_t out_sz){
    PRINT_MSG(LOG_LVL_DEBUG, "detect_external_output: entry");

    int pipefd[2];
    if(pipe(pipefd) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "detect_external_output: pipe failed, errno %d (%s)", errno, strerror(errno));
        PRINT_MSG(LOG_LVL_DEBUG, "detect_external_output: exit (-1)");
        return -1;
    }

    pid_t pid = fork();
    if(pid < 0){
        PRINT_MSG(LOG_LVL_ERROR, "detect_external_output: fork failed, errno %d (%s)", errno, strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        PRINT_MSG(LOG_LVL_DEBUG, "detect_external_output: exit (-1)");
        return -1;
    }

    if(pid == 0){
        setenv("DISPLAY", ":0", 1);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execlp("xrandr", "xrandr", "--query", (char*)NULL);
        _exit(127);
    }

    close(pipefd[1]);

    char buf[8192];
    ssize_t total = 0, n;
    while((n = read(pipefd[0], buf + total, sizeof(buf) - 1 - total)) > 0){
        total += n;
        if((size_t)total >= sizeof(buf) - 1){
            PRINT_MSG(LOG_LVL_WARN, "detect_external_output: xrandr output truncated at %zu bytes", sizeof(buf) - 1);
            break;
        }
    }
    buf[total] = '\0';
    close(pipefd[0]);
    waitpid(pid, NULL, 0);

    char *save = NULL;
    for(char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)){
        if(!strstr(line, " connected")){
            continue;
        }

        char name[64] = {0};
        if(sscanf(line, "%63s", name) != 1){
            continue;
        }

        if(strncmp(name, "eDP", 3) == 0 || strncmp(name, "LVDS", 4) == 0){
            PRINT_MSG(LOG_LVL_DEBUG, "detect_external_output: skipping internal panel %s", name);
            continue;
        }

        strncpy(out_name, name, out_sz - 1);
        out_name[out_sz - 1] = '\0';
        PRINT_MSG(LOG_LVL_DEBUG, "detect_external_output: exit (found %s)", out_name);
        return 0;
    }

    PRINT_MSG(LOG_LVL_WARN, "detect_external_output: no external output found in xrandr output");
    PRINT_MSG(LOG_LVL_DEBUG, "detect_external_output: exit (-1)");
    return -1;
}

/* ================================== main =================================== */

/**
 * @brief Entry point: resolve the display, then loop discover -> serve -> rediscover.
 *
 * ::log_init runs first so that every later line honours the requested verbosity;
 * precedence is CLI flag, then the @c LOG_LEVEL environment variable, then the context
 * default chosen from whether a TTY is attached.
 *
 * The external output is resolved once at startup, since it does not move at runtime for
 * this use case. If it cannot be resolved (X may not be up yet) the common default is
 * used rather than refusing to start: ::__rotate_display__ will then fail loudly and
 * ::__heal_orientation__ will reapply the orientation once the display appears.
 *
 * The outer loop then runs forever:
 * -# join any previous watcher thread, since skipping this would leak one per cycle;
 * -# clear the quit request and the stale IP-change flag, so a flag raised during the
 *    @b previous session cannot immediately kill the new one;
 * -# discover the ESP; on failure just loop and try again, which is the normal state
 *    whenever the ESP is powered off or out of range;
 * -# start the watcher and serve commands until the session breaks;
 * -# on a broken session, ask the watcher to stop and go round again.
 *
 * Only a @c pthread failure is fatal: it means the process is in a state that cannot be
 * reasoned about, so it exits and lets the supervisor restart it clean.
 *
 * @param argc Argument count, forwarded to ::log_init.
 * @param argv Argument vector, forwarded to ::log_init.
 * @return 1 if a thread join failed; 2 if the watcher could not be started. Otherwise
 *         does not return.
 */
int main(int argc, char **argv){
    log_init(argc, argv);

    PRINT_MSG(LOG_LVL_DEBUG, "main: entry");
    PRINT_MSG(LOG_LVL_INFO, "main: starting orientation daemon");

    int retval = 0;

    if(detect_external_output(gs.monitor_ext, sizeof(gs.monitor_ext)) == 0){
        PRINT_MSG(LOG_LVL_INFO, "main: external display output: %s", gs.monitor_ext);
    }else{
        strncpy(gs.monitor_ext, "HDMI1", sizeof(gs.monitor_ext) - 1);
        PRINT_MSG(LOG_LVL_WARN, "main: no external output detected, defaulting to %s", gs.monitor_ext);
    }

    bool is_netlink_running = false;
    pthread_t tid;

    while(1){
        if(is_netlink_running){
            if(wait_for_thread(tid) != 0){
                PRINT_MSG(LOG_LVL_ERROR, "main: wait_for_thread failed, exiting");
                PRINT_MSG(LOG_LVL_DEBUG, "main: exit (1)");
                return 1;
            }
            is_netlink_running = false;
        }

        set_netlink_quit_status(false);
        netlink_ip_reset();

        if((retval = discover_esp()) != 0){
            PRINT_MSG(LOG_LVL_WARN, "main: discovery failed (retval %d), retrying", retval);
            continue;
        }

        if(start_netlink(&tid) < 0){
            PRINT_MSG(LOG_LVL_ERROR, "main: start_netlink failed, exiting");
            PRINT_MSG(LOG_LVL_DEBUG, "main: exit (2)");
            return 2;
        }
        is_netlink_running = true;

        if((retval = start_tcp_client()) != 0){
            PRINT_MSG(LOG_LVL_WARN, "main: session ended (retval %d), stopping watcher and rediscovering", retval);
            set_netlink_quit_status(true);
            continue;
        }
    }
}
