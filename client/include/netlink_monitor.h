#ifndef NETLINK_MONITOR_H
#define NETLINK_MONITOR_H

/**
 * @file netlink_monitor.h
 * @brief Interface to the netlink watcher thread: IP-change and link-down detection.
 *
 * The PC discovers the ESP by broadcasting on one specific Wi-Fi interface and then
 * holds a TCP session to the address it learned. If the machine roams to another AP, the
 * DHCP lease moves, or the link drops, that address is stale and the session is dead --
 * but nothing in a blocking @c recv() would ever say so. The watcher thread exists to
 * notice, and this header is how the main loop talks to it.
 *
 * ### Contract
 * @verbatim
 *   main: netlink_ip_reset()            clear any flag left from the previous session
 *   main: pthread_create(netlink_monitor)
 *   ...
 *   main: netlink_ip_changed()          polled each recv timeout; true -> rediscover
 *   main: set_netlink_quit_status(true) ask it to stop
 *   main: pthread_join()                must join before starting another
 * @endverbatim
 *
 * The thread is one-shot: on the first relevant change it raises the flag and returns,
 * because the session is already invalid and there is nothing further to watch.
 *
 * @note The flag is @c atomic_bool internally, so these calls need no external locking.
 * @note The thread reads @c gs.old_info (which interface to watch, and its address) and
 *       @c gs.quit_netlink from main.h, so discovery must have succeeded before it is
 *       started.
 */

#include <stdbool.h>

/**
 * @brief Test and clear the IP-changed flag.
 *
 * Consumes the flag: a single atomic exchange, so even though the main loop polls it
 * from two places (before each @c recv, and again before each ACK send), one change is
 * reported exactly once.
 *
 * @return @c true if an IP change or link-down was seen since the last call, and clears
 *         it; @c false otherwise.
 */
bool netlink_ip_changed(void);

/**
 * @brief Clear the IP-changed flag unconditionally.
 *
 * Called before each rediscovery. Without it, a change flagged during the previous
 * session would still be pending and would immediately tear down the session about to be
 * established.
 */
void netlink_ip_reset(void);

/**
 * @brief Thread entry point for the netlink watcher (@c pthread_create signature).
 *
 * Watches the interface named in @c gs.old_info for @c RTM_NEWADDR (address changed) and
 * @c RTM_NEWLINK (carrier lost), raising the flag and exiting on the first one it sees.
 * Polls @c get_netlink_quit_status on each receive timeout so the main loop can stop it.
 *
 * @param arg Unused; required by the @c pthread_create signature.
 * @return Always @c NULL. Raises @c SIGTERM against the process on an unrecoverable
 *         socket error, since without this thread an IP change would go unnoticed and
 *         the daemon would silently talk to a stale address.
 */
void* netlink_monitor(void* arg);

#endif  /* NETLINK_MONITOR_H */
