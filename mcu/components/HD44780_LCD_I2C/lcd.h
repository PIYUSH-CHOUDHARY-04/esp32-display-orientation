#ifndef LCD_H
#define LCD_H

/**
 * @file lcd.h
 * @ingroup esp_firmware
 * @brief High-level HD44780 LCD driver wrapper for ESP-IDF using PCF8574 I2C backpack.
 *
 * This module provides:
 * - Formatted LCD printing
 * - Cursor state management
 * - Persistent row pinning
 * - Partial row clearing
 * - ESP-IDF I2C integration
 * - Thread-safe LCD runtime operations
 *
 * Built on top of the HD44780 low-level driver.
 *
 * -----------------------------------------------------------------------------
 * Usage Model
 * -----------------------------------------------------------------------------
 *
 * Typical lifecycle:
 *
 *      lcd_init(...)
 *          ->
 *      lcd_printf(), lcd_clear(), lcd_set_cursor(), ...
 *          ->
 *      lcd_deinit()
 *
 * -----------------------------------------------------------------------------
 * Threading Model
 * -----------------------------------------------------------------------------
 *
 * - Runtime LCD operations are internally protected using a recursive mutex.
 * - APIs such as lcd_printf(), lcd_clear(), lcd_row_pin(), etc are thread-safe.
 * - lcd_init() and lcd_deinit() are NOT thread-safe.
 *
 * @warning
 * lcd_init() must be called once from app_main() before any task starts using
 * the LCD APIs.
 *
 * -----------------------------------------------------------------------------
 * Design Notes
 * -----------------------------------------------------------------------------
 *
 * - Current implementation manages a singleton LCD device internally.
 * - I2C transfers are synchronous/blocking.
 * - Driver internally tracks:
 *      - cursor state
 *      - row pinning state
 *      - LCD initialization state
 *
 * -----------------------------------------------------------------------------
 * Hardware Assumptions
 * -----------------------------------------------------------------------------
 *
 * - HD44780-compatible LCD
 * - PCF8574 I2C backpack
 * - 16x2 LCD configuration (default current implementation)
 */

#include <stdint.h>
#include <esp_err.h>
#include "hd44780.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Base error code offset for LCD driver.
 */
#define ESP_ERR_LCD_BASE                 0x5000

/**
 * @brief LCD driver error codes.
 */
typedef enum {

    /* Generic */
    LCD_OK                              = ESP_OK,

    /* I2C operation failures */
    LCD_ERR_NEW_MASTER_BUS              = ESP_ERR_LCD_BASE + 1,
    LCD_ERR_MASTER_ADD_DEVICE           = ESP_ERR_LCD_BASE + 2,

    /* Invalid usage */
    LCD_ERR_INVALID_ARG                 = ESP_ERR_INVALID_ARG,
    LCD_ERR_INVALID_STATE               = ESP_ERR_INVALID_STATE,
    /* State warnings/errors */
    LCD_WARN_NOTHING_TO_CLEAR           = ESP_ERR_LCD_BASE + 3,

    /* HD44780 operation failures */
    LCD_ERR_PUTC_FAILED                 = ESP_ERR_LCD_BASE + 4,
    LCD_ERR_GOTOXY_FAILED               = ESP_ERR_LCD_BASE + 5,
    LCD_ERR_CLEAR_FAILED                = ESP_ERR_LCD_BASE + 6,
    LCD_ERR_INIT_FAILED                 = ESP_ERR_LCD_BASE + 7

} lcd_err_t;

/**
 * @brief User-provided ESP-IDF I2C configuration.
 *
 * Pass this structure to lcd_init() when custom I2C configuration
 * is required.
 *
 * If NULL is passed to lcd_init(), default configuration values
 * are used internally.
 */
struct lcd_user_handle {

    /**
     * @brief SDA GPIO pin number.
     */
    uint8_t esp_i2c_sda_pin_usr;

    /**
     * @brief SCL GPIO pin number.
     */
    uint8_t esp_i2c_scl_pin_usr;

    /**
     * @brief ESP-IDF I2C clock source.
     */
    uint8_t esp_i2c_clk_src_usr;

    /**
     * @brief ESP-IDF I2C controller port.
     */
    uint8_t esp_i2c_port_usr;
};

//=============================================================================
// Internals (exposed for hd44780)
//=============================================================================

/**
 * @brief Low-level HD44780 write callback.
 *
 * This callback is used internally by the HD44780 driver
 * to transmit one byte through the PCF8574 I2C expander.
 *
 * @note
 * This API is intended for internal driver usage only.
 *
 * @param lcd Pointer to HD44780 descriptor.
 * @param data Data byte to transmit.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP-IDF I2C error code on failure
 */
esp_err_t write_cb(const hd44780_t *lcd, uint8_t data);

//=============================================================================
// Public API
//=============================================================================

/**
 * @brief Initialize LCD driver and underlying I2C bus/device.
 *
 * This function:
 * - Creates I2C master bus
 * - Adds PCF8574 device onto the bus
 * - Initializes the HD44780 LCD
 * - Creates internal synchronization primitives
 *
 * If @p luh is NULL, default I2C configuration values are used.
 *
 * @warning
 * Must be called once from app_main() before any task uses LCD APIs.
 *
 * @note
 * This function is NOT thread-safe.
 *
 * @param luh Pointer to user I2C configuration.
 *            Pass NULL to use default configuration.
 *
 * @return
 *      - LCD_OK on success
 *      - LCD_ERR_** on failure
 *      - ESP_ERR_INVALID_STATE on invalid initialization sequence
 */
esp_err_t lcd_init(struct lcd_user_handle* luh);

/**
 * @brief Clear entire LCD and reset internal state.
 *
 * This function:
 * - Clears LCD display
 * - Resets cursor position
 * - Clears row pinning state
 *
 * @note
 * This API is thread-safe.
 *
 * @return
 *      - LCD_OK on success
 *      - LCD_ERR_CLEAR_FAILED on LCD clear failure
 *      - ESP_ERR_INVALID_STATE if LCD is not initialized
 */
esp_err_t lcd_clear(void);

/**
 * @brief Clear only unpinned LCD rows.
 *
 * Behavior:
 * - No pinned rows:
 *      clears entire display
 * - One pinned row:
 *      clears only the unpinned row
 * - Both rows pinned:
 *      nothing is cleared
 *
 * @note
 * Cursor positioning assumptions are maintained internally
 * by row pinning and cursor management APIs.
 *
 * @note
 * This API is thread-safe.
 *
 * @return
 *      - LCD_OK on success
 *      - LCD_WARN_NOTHING_TO_CLEAR if both rows are pinned
 *      - LCD_ERR_** on internal operation failure
 *      - ESP_ERR_INVALID_STATE if LCD is not initialized
 */
esp_err_t lcd_clear_row(void);

/**
 * @brief Print formatted text to LCD.
 *
 * Supports standard printf-style formatting using variadic arguments.
 *
 * Behavior depends on current row pinning state.
 *
 * The driver internally:
 * - clears writable regions
 * - preserves pinned rows
 * - restores cursor position after printing
 *
 * @note
 * Maximum printable length is bounded by LCD dimensions.
 *
 * @note
 * This API is thread-safe.
 *
 * @param fmt printf-style format string.
 * @param ... Variable arguments.
 *
 * @return
 *      - Number of characters formatted
 *      - Negative value on failure
 */
int lcd_printf(const char *fmt, ...);

/**
 * @brief Pin an LCD row to preserve its contents.
 *
 * Pinned rows are protected from:
 * - lcd_printf()
 * - lcd_clear_row()
 *
 * Cursor position is automatically updated to the next
 * writable region.
 *
 * @note
 * This API is thread-safe.
 *
 * @param row LCD row number.
 *
 * @return
 *      - LCD_OK on success
 *      - LCD_ERR_INVALID_ARG on invalid row
 *      - LCD_ERR_GOTOXY_FAILED on cursor positioning failure
 *      - ESP_ERR_INVALID_STATE if LCD is not initialized
 */
esp_err_t lcd_row_pin(uint8_t row);

/**
 * @brief Remove row pinning.
 *
 * After unpinning, the row becomes writable again.
 *
 * Internal cursor state is also updated.
 *
 * @note
 * This API is thread-safe.
 *
 * @param row LCD row number.
 *
 * @return
 *      - LCD_OK on success
 *      - LCD_ERR_INVALID_ARG on invalid row
 *      - LCD_ERR_GOTOXY_FAILED on cursor positioning failure
 *      - ESP_ERR_INVALID_STATE if LCD is not initialized
 */
esp_err_t lcd_row_unpin(uint8_t row);

/**
 * @brief Set LCD cursor position.
 *
 * Updates both:
 * - LCD hardware cursor
 * - Internal wrapper cursor state
 *
 * @note
 * This API is thread-safe.
 *
 * @param row Target row.
 * @param col Target column.
 *
 * @return
 *      - LCD_OK on success
 *      - LCD_ERR_INVALID_ARG on invalid row/column
 *      - LCD_ERR_GOTOXY_FAILED on LCD cursor update failure
 *      - ESP_ERR_INVALID_STATE if LCD is not initialized
 */
esp_err_t lcd_set_cursor(uint8_t row, uint8_t col);

/**
 * @brief Deinitialize LCD driver and underlying I2C resources.
 *
 * This function:
 * - Removes I2C device from bus
 * - Deletes I2C master bus
 * - Resets internal LCD state
 * - Deletes synchronization primitives
 *
 * @warning
 * No task should access LCD APIs after this function returns.
 *
 * @note
 * This function is NOT thread-safe.
 *
 * @return
 *      - LCD_OK on success
 *      - ESP_ERR_INVALID_STATE if LCD is not initialized
 */
esp_err_t lcd_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
