#ifndef WIFI_MGR_H
#define WIFI_MGR_H

/**
 * @file wifi_mgr.h
 * @ingroup esp_firmware
 * @brief The Wi-Fi manager: provisioning, discovery, and the orientation link to the PC.
 *
 * The largest component on the board, because it owns everything between "the ESP has power" and
 * "the PC is listening". Three jobs, in order:
 *
 * -# **Provisioning.** On first boot the board has no credentials and no keyboard with which to be
 *    given any. It raises its own access point, waits for @c ap_provision to connect, and receives
 *    the SSID and password inside an AES-256-GCM session keyed on the pairing code shown on the LCD.
 * -# **Station.** With credentials in hand it joins the real network and tears the AP down.
 * -# **The link.** It answers the PC's discovery broadcast, accepts a TCP session, and sends an
 *    orientation command each time the sensor reports the board has been turned.
 *
 * ### Everything runs in one task
 * ::wifi_mgr_init creates a task and a queue and returns immediately -- it does NOT block on the
 * connection. Every subsequent operation is a message posted to that queue and handled inside the
 * task.
 *
 * That is not incidental. ESP-IDF's Wi-Fi event handlers run in the event-loop task, and the
 * connection state machine has to be driven from somewhere that can block; doing both from the
 * caller's context would mean either blocking @c app_main for the length of a Wi-Fi association, or
 * mutating connection state from two tasks at once. The queue is what keeps all of it single-
 * threaded.
 *
 * ### If the task dies
 * @p caller_handle and @p die_bit are a death notification: if the manager task ever exits, it
 * notifies the caller on its way out. Without it a dead manager is invisible -- the board keeps
 * running, the sensor keeps detecting rotations, and every one of them is queued into a task that no
 * longer exists. See @c app_main, which uses exactly this to restart the component.
 *
 * @warning ::wifi_mgr_deinit does NOT stop a running task. See its own warning: it must be called
 *          only after the task has already exited.
 *
 * @note There is no "is it connected yet" query, and none is needed. ::wifi_mgr_send_cmd may be
 *       called at any point, including before the radio is up: the command is queued, the send
 *       fails with @c WIFI_MGR_NOT_READY, the orientation is cached, and it is replayed the moment
 *       the PC connects. A caller that checked first would be doing the manager's job for it, and
 *       would still race -- the connection can drop between the check and the send.
 */

#include "esp_err.h"		/* esp_err_t (return type of public funcs) */
#include "esp_wifi.h"		/* WIFI_MODE_NULL / WIFI_MODE_STA / WIFI_MODE_AP (enum values) */
#include "freertos/task.h"	/* TaskHandle_t, xTaskCreate */
#include <stddef.h>		/* size_t */
#include "common.h"

/**
 * @brief Which Wi-Fi role the board is currently in.
 *
 * @note ::WIFI_MGR_MODE_NONE is @c 0x00FFFFFF rather than @c 0, deliberately. Zero is what an
 *       uninitialised struct field holds, so a zero sentinel cannot be distinguished from "never
 *       set" -- and "the radio is off" and "nobody has decided yet" are different states that must
 *       not be confused.
 */
typedef enum {
	WIFI_MGR_MODE_NONE = 0x00FFFFFF,  /**< Radio off, or not yet decided. See the note above. */
	WIFI_MGR_MODE_STA,                /**< Station: joined to the user's network. The normal state. */
	WIFI_MGR_MODE_AP,                 /**< Access point: the temporary provisioning network. */
} wifi_mgr_mode_t;

/**
 * @brief Start the Wi-Fi manager: create the queue, spawn the task, bump the boot epoch.
 *
 * Returns immediately. It does NOT wait for a connection -- the task handles association,
 * provisioning if there are no stored credentials, and everything after, on its own schedule.
 *
 * ### The die-bit
 * @p caller_handle and @p die_bit together are how a dying manager announces itself. If the task
 * exits, it does @c xTaskNotify(caller_handle, @c 1 @c << @c die_bit) before it goes.
 *
 * Without that, a dead manager is SILENT. The board keeps running, the sensor keeps detecting
 * rotations, and each one is queued into a task that no longer exists -- so the display simply stops
 * following the board, with nothing anywhere saying why. @c app_main listens for this bit and
 * restarts the component.
 *
 * @param caller_handle        Task to notify if the manager task exits. Usually the caller's own
 *                             handle. @c NULL opts out, and a dying task then really is silent.
 * @param get_immediate_orient Called to read the CURRENT orientation on demand -- used when the PC
 *                             connects and needs the state now, rather than waiting for the board to
 *                             next be moved. Normally ::sensor_get_immediate_orientation.
 * @param die_bit              Which notification bit to set on @p caller_handle. Only meaningful if
 *                             @p caller_handle is non-NULL.
 */
void wifi_mgr_init(TaskHandle_t caller_handle, display_orient_t (*get_immediate_orient)(void), uint8_t die_bit);

/**
 * @brief Clear the manager's bookkeeping and delete its queue.
 *
 * @warning This does NOT stop a running manager task. It only tears down the queue and the stored
 *          handles -- so calling it while the task is still alive leaves that task running against a
 *          DELETED queue, which is a use-after-free waiting to happen on the next command.
 *
 *          The task exits only through its own internal @c DRIVER_DEINIT path. In practice that
 *          means this is safe to call in exactly one situation: after the task has already died and
 *          said so via its die-bit. That is what @c app_main does.
 */
void wifi_mgr_deinit(void);

/**
 * @brief Queue an orientation command for the manager task to send to the PC.
 *
 * Asynchronous, and deliberately so: this is called from the SENSOR task, which must not block. The
 * command is posted to the queue and this returns at once -- the manager task does the sending.
 *
 * @note Posted with a zero timeout. If the queue is full the command is DROPPED rather than blocking
 *       the sensor task, which is the right trade: a full queue means the manager is already behind,
 *       and the orientation about to be dropped is stale anyway. The next rotation supersedes it.
 *
 * @param orient The orientation to send.
 */
void wifi_mgr_send_cmd(display_orient_t orient);

#endif  /* WIFI_MGR_H */
