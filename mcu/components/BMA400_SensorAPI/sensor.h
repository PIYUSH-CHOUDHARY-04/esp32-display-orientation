#ifndef SENSOR_H
#define SENSOR_H

/**
 * @file sensor.h
 * @ingroup esp_firmware
 * @brief Interface to the BMA400 orientation sensor: init, teardown, and a direct read.
 *
 * The board's whole purpose runs through this header. The BMA400 reports which way up it is; this
 * module turns that into a @c display_orient_t and hands it to a callback, which sends it to the PC.
 *
 * ### How orientation actually arrives
 * Not by polling. The BMA400 is configured to raise an interrupt when the board STOPS moving, and a
 * worker task waits on it:
 * @verbatim
 *   board moved -> INT1 fires -> ISR notifies the worker task
 *                                     |
 *                                     v
 *                             worker reads the accelerometer,
 *                             works out the orientation, and
 *                             calls fptr_send_orient
 * @endverbatim
 * The interrupt is on ACTIVITY-then-INACTIVITY -- the board settling -- rather than on movement
 * itself. Reading mid-turn would report whatever transient angle the sensor happened to see, and the
 * display would flap through two or three rotations on the way to the one the user wanted.
 *
 * ### Contract
 * @verbatim
 *   sensor_init(my_handle, on_orientation, DIE_BIT)   once, at startup
 *   ...                                               callback fires on each settled change
 *   sensor_deinit()                                   before re-initialising, ever
 * @endverbatim
 *
 * @note The callback runs in the SENSOR TASK's context, not the caller's. It may block briefly, but
 *       it must not be heavy: while it runs, no further orientation change can be processed.
 *
 * @warning ::sensor_init installs a GPIO ISR and creates a task. Calling it twice without
 *          ::sensor_deinit in between leaks both.
 */

#include "common.h"

/**
 * @brief Initialise the BMA400: SPI, interrupts, ISR, and the worker task.
 *
 * Configures the accelerometer and its activity/inactivity interrupts, installs the GPIO ISR, and
 * starts the sensor task. From then on, every time the board settles into a new orientation,
 * @p fptr_send_orient is called with it.
 *
 * ### The die-bit, and why it exists
 * @p caller_handle and @p die_bit together are a death notification. If the worker task ever exits
 * -- an SPI failure, a sensor that stops responding -- it does @c xTaskNotify(caller_handle, @c 1
 * @c << @c die_bit) on its way out.
 *
 * Without it, a dead worker is INVISIBLE. The task simply vanishes; nothing polls it, nothing
 * notices, and the board carries on looking perfectly healthy while reporting no orientation change
 * ever again. The caller is then the only thing that can react, and this is how it finds out.
 *
 * @param caller_handle Task to notify if the worker exits. Pass the handle of whatever should hear
 *                      about it -- usually the task calling this. May be @c NULL to opt out, in
 *                      which case a dying worker really is silent.
 * @param fptr_send_orient Called with each new settled orientation. Must not be @c NULL. Runs in the
 *                      SENSOR TASK's context: it may block briefly, but must not be heavy, since no
 *                      further orientation is processed while it runs.
 * @param die_bit       Which notification bit to set on @p caller_handle. Only meaningful if
 *                      @p caller_handle is non-NULL.
 * @return 0 on success; a negative error code on failure.
 */
int sensor_init(TaskHandle_t caller_handle, void (*fptr_send_orient)(display_orient_t), uint8_t die_bit);

/**
 * @brief Tear the sensor down: stop the task, remove the ISR, release SPI.
 *
 * Safe to call when not initialised -- it does nothing. Must be called before ::sensor_init is
 * called a second time, or the ISR handler and the worker task are both leaked.
 */
void sensor_deinit(void);

/**
 * @brief Read the orientation RIGHT NOW, without waiting for an interrupt.
 *
 * The normal path is the callback: the sensor decides when the orientation has changed and pushes
 * it. This is the exception -- a synchronous read, for the case where something needs the current
 * orientation before any interrupt has had cause to fire. Chiefly at startup, when the board has
 * been sitting still since boot and there has been no settling event to report.
 *
 * @warning Reads the accelerometer directly, and returns whatever it says at that instant -- with
 *          none of the settling logic the interrupt path applies. Called while the board is being
 *          turned, it reports the transient mid-rotation angle rather than the one it is heading
 *          for.
 *
 * @return The current orientation, or @c ORIENT_INVALID if it cannot be read.
 */
display_orient_t sensor_get_immediate_orientation(void);

#endif  /* SENSOR_H */
