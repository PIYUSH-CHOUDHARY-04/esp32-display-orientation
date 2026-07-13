#ifndef MAIN_H
#define MAIN_H

/**
 * @file main.h
 * @ingroup pc_client
 * @brief Shared state and types for the PC-side orientation daemon.
 *
 * Declares the single process-wide ::global_state instance (@c gs) that main.c defines
 * and netlink_monitor.c reads, plus the interface-description type used during
 * discovery.
 *
 * ### Why this state is global rather than passed around
 * Two threads need it: the main thread runs discovery and the TCP command loop, while
 * the netlink watcher thread needs to know which interface to monitor
 * (::global_state::old_info) and how to signal that it should stop
 * (::global_state::quit_netlink). Only the fields that genuinely cross the thread
 * boundary are synchronised; see the per-field notes below.
 *
 * @note @c common.h supplies @c display_orient_t and the wire protocol types shared with
 *       the ESP32 firmware.
 */

#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "common.h"

/**
 * @brief One Wi-Fi interface as seen during discovery.
 *
 * Filled in by @c __get_wifi_interfaces__ for each up, non-loopback, IPv4, wireless
 * interface. The mask is carried alongside the address because discovery needs both: the
 * directed broadcast address it sends to is computed as @c ip | @c ~mask.
 *
 * The interface that discovery eventually succeeded on is copied into
 * ::global_state::old_info, and the netlink watcher then monitors that one for changes.
 */
struct wifi_iface_info {
    char ifname[IFNAMSIZ];   /**< Interface name (e.g. "wlan0"), sized by the kernel's own limit. */
    struct in_addr ip;       /**< The interface's IPv4 address; compared against later netlink events. */
    struct in_addr mask;     /**< Netmask; needed to derive the directed broadcast address. */
};

#define EXTERNAL_CONNECTED_MONITOR_NAMESIZE  32   /**< Max length of an xrandr output name (e.g. "HDMI1"). */

/**
 * @brief Process-wide state, defined once in main.c and shared with the netlink thread.
 *
 * ### Orientation: intent vs reality
 * The two orientation fields are not redundant, and confusing them is the subtle bug
 * this design exists to prevent:
 * - ::global_state::last_orient is @b reality -- what is actually applied on screen. It
 *   is the dedup guard, and must be written ONLY when @c xrandr actually succeeded.
 * - ::global_state::desired_orient is @b intent -- what the ESP last asked for. Written
 *   unconditionally, success or failure.
 *
 * Writing a failed rotation into @c last_orient would be self-defeating: it is the very
 * field the dedup check reads to decide "already applied, skip". Recording a failure
 * there tells the program the screen is already rotated when it is not, and the guard
 * then suppresses every future retry -- leaving the display permanently wrong with no
 * path back. Keeping the failed request in @c desired_orient instead leaves the two
 * fields mismatched, and that mismatch is precisely what the healer in main.c detects
 * and retries.
 *
 * This separation is necessary because the ESP only transmits when the board physically
 * moves. If a rotation fails while X is unreachable, nothing on the wire would ever ask
 * again; the daemon has to remember the request and reapply it itself.
 */
struct global_state {
    /**
     * @brief The ESP's TCP endpoint, learned during discovery.
     *
     * The address half is filled in by @c recvfrom (the ESP replies from its own IP, so
     * it comes for free); only the port is taken from the discovery ACK payload, since
     * the ESP's TCP listener is on a different port than its UDP responder.
     */
    struct sockaddr_in esp_tcp_addr;

    /**
     * @brief The interface discovery succeeded on.
     *
     * Cached so the netlink watcher knows which interface to monitor and what address to
     * compare incoming @c RTM_NEWADDR events against.
     *
     * @warning Must be populated before the watcher thread starts, or its
     *          @c if_nametoindex on an empty name fails and it kills the process.
     */
    struct wifi_iface_info old_info;

    /**
     * @brief Set by the main thread to ask the netlink watcher to exit.
     *
     * Atomic because it crosses the thread boundary with no mutex: written by main,
     * polled by the watcher on each of its receive timeouts.
     */
    atomic_bool quit_netlink;

    /**
     * @brief xrandr output name of the external display (e.g. "HDMI1").
     *
     * Resolved once at startup. If X is not up yet and it cannot be resolved, main.c
     * falls back to a default rather than refusing to start; the rotation then fails
     * loudly and the healer reapplies it once the display appears.
     */
    char monitor_ext[EXTERNAL_CONNECTED_MONITOR_NAMESIZE];

    /**
     * @brief Reality: the orientation currently applied on screen.
     *
     * The dedup guard. Starts at @c ORIENT_INVALID (nothing applied yet).
     *
     * @warning Write to this ONLY when @c xrandr returned success. Caching a failed
     *          rotation here poisons the dedup check and permanently blocks the retry --
     *          see the struct-level notes.
     */
    display_orient_t last_orient;

    /**
     * @brief Intent: the orientation the ESP last asked for.
     *
     * Written on every command, whether or not the rotation succeeded. When this differs
     * from ::global_state::last_orient, a requested rotation has not been applied and the
     * healer retries it on the receive-timeout tick. Starts at @c ORIENT_INVALID (nothing
     * requested yet), which the healer treats as "nothing to do".
     */
    display_orient_t desired_orient;
};

/** @brief The single instance; defined in main.c, read by the netlink thread. */
extern struct global_state gs;

/**
 * @brief Read the netlink watcher's quit request.
 *
 * Exposed here rather than kept private to main.c because the watcher thread lives in
 * netlink_monitor.c and polls it from its receive loop.
 *
 * @return @c true if the watcher has been asked to exit.
 */
bool get_netlink_quit_status(void);

#endif  /* MAIN_H */
