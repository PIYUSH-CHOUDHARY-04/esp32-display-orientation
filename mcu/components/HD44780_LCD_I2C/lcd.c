/**
 * @file lcd.c
 * @ingroup esp_firmware
 * @brief HD44780 character-LCD driver over a PCF8574 I2C backpack (ESP-IDF).
 *
 * This is a thin, thread-safe wrapper around the vendor @c hd44780 driver. It
 * owns the ESP-IDF I2C master bus/device, tracks a shadow cursor position, and
 * adds a "row pinning" feature that protects chosen rows from being cleared or
 * overwritten by ::lcd_printf and ::lcd_clear_row.
 *
 * ### HD44780 <-> PCF8574 pin mapping
 * The HD44780 is driven in 4-bit mode through the PCF8574's 8-bit port. The port
 * bits (P7..P0) map to LCD control/data lines as:
 * - P0 -> RS
 * - P1 -> RW
 * - P2 -> E
 * - P3 -> BL (backlight)
 * - P4 -> D4
 * - P5 -> D5
 * - P6 -> D6
 * - P7 -> D7
 *
 * ### Concurrency
 * All public state transitions are serialised by a single @b recursive mutex
 * (::lcd_i2c::lcd_mutex). It is recursive because higher-level calls nest lower
 * ones while already holding the lock — e.g. ::lcd_printf calls ::lcd_clear /
 * ::lcd_clear_row, which take the same lock.
 *
 * ### Return-value convention
 * Functions return @c LCD_OK (0) on success, a negative @c LCD_ERR_* on failure,
 * and a positive @c LCD_WARN_* for benign "nothing to do" outcomes.
 */

#define LOG_COMPONENT_LCD
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lcd.h"
#include "logging.h"

/**
 * @name I2C bus configuration (ESP-IDF master bus)
 * @{
 */
#define ESP_I2C_PORT_DEFAULT        I2C_NUM_0            /**< Default I2C controller port. */
#define ESP_SDA_GPIO_DEFAULT        GPIO_NUM_21          /**< Default SDA GPIO. */
#define ESP_SCL_GPIO_DEFAULT        GPIO_NUM_22          /**< Default SCL GPIO. */
#define ESP_I2C_CLK_SRC_DEFAULT     I2C_CLK_SRC_DEFAULT  /**< Default I2C clock source. */
/** @} */

/**
 * @name Fixed I2C bus tuning — do not change
 * @{
 */
#define GLITCH_IGN_CNT              7      /**< Glitch-ignore count for the bus. */
#define INTR_PRIOR                  0      /**< Interrupt priority (0 = driver default). */
#define TQUEUE_DEPTH                0      /**< Transaction queue depth; 0 puts I2C in synchronous mode, which is fine for simple LCDs. */
#define EN_INTR_PULLUP              true   /**< Enable internal pull-ups on SDA/SCL. */
/** @} */

/**
 * @name PCF8574 device configuration — modify if your backpack differs
 * @{
 */
#define PCF8574_I2C_ADDR_DEFAULT        0x27                /**< PCF8574 I2C address (change if it differs). */
#define PCF8574_I2C_CLK_FREQ_DEFAULT    100000              /**< SCL frequency in Hz (change if it differs). */
#define PCF8574_ADDR_LEN_DEFAULT        I2C_ADDR_BIT_LEN_7  /**< Address length (change if it differs). */
/** @} */

/**
 * @name PCF8574 port-bit positions
 * @brief Bit index within the PCF8574 byte for each HD44780 line.
 * @{
 */
#define PIN_RS_BIT      0   /**< RS line bit. */
#define PIN_RW_BIT      1   /**< RW line bit. */
#define PIN_E_BIT       2   /**< E (enable) line bit. */
#define PIN_BL_BIT      3   /**< Backlight control bit. */
#define PIN_D4_BIT      4   /**< Data D4 bit. */
#define PIN_D5_BIT      5   /**< Data D5 bit. */
#define PIN_D6_BIT      6   /**< Data D6 bit. */
#define PIN_D7_BIT      7   /**< Data D7 bit. */
/** @} */

/**
 * @name HD44780 geometry
 * @{
 */
#define LCD_COL_CNT     16          /**< Columns per row. */
#define LCD_ROW_CNT     2           /**< Row count. */
#define ROW(x)          (x)         /**< Row selector, @p x in [0, LCD_ROW_CNT-1]. */
#define COL(x)          (x)         /**< Column selector, @p x in [0, LCD_COL_CNT-1]. */
/** @} */

/**
 * @brief LCD device descriptor: HD44780 state, I2C configuration, and handles.
 *
 * A single static instance (::lcd_dev) backs the whole module. It aggregates the
 * vendor HD44780 descriptor, the resolved I2C configuration, the shadow cursor
 * the wrapper maintains, the row-pinning bitmap, and the ESP-IDF bus/device
 * handles.
 */
struct lcd_i2c {
    hd44780_t lcd;                      /**< Underlying vendor HD44780 descriptor. */
    uint8_t pcf8574_i2c_addr;           /**< PCF8574 I2C device address. */
    uint8_t pcf8574_i2c_addr_len;       /**< PCF8574 address length. */
    uint8_t esp_i2c_sda_pin;            /**< Resolved SDA GPIO. */
    uint8_t esp_i2c_scl_pin;            /**< Resolved SCL GPIO. */
    uint32_t esp_i2c_freq;              /**< I2C clock frequency in Hz. */
    uint8_t esp_i2c_clk_src;            /**< ESP-IDF I2C clock source (e.g. APB). */
    uint8_t esp_i2c_port;               /**< ESP-IDF I2C controller port. */
    const uint8_t lcd_cols;             /**< Number of LCD columns. */

    /**
     * @brief Row-pinning bitmap (2 bits).
     *
     * bit0 -> row 0 pinned, bit1 -> row 1 pinned. A pinned row cannot be written
     * or cleared: both ::lcd_printf and ::lcd_clear_row skip pinned rows. This is
     * used to keep a fixed label (e.g. "STATION IP:") on one row while the other
     * row updates.
     */
    uint8_t lcd_row_pinning     : 2;

    uint8_t lcd_cursor_pos_row;         /**< Shadow cursor row tracked by the wrapper. */
    uint8_t lcd_cursor_pos_col;         /**< Shadow cursor column tracked by the wrapper. */
    bool lcd_init_state;                /**< True once the device is initialised. */
    SemaphoreHandle_t lcd_mutex;        /**< Recursive mutex serialising all LCD operations (safe across cores and nested calls). */
    i2c_master_bus_handle_t bus_handle; /**< ESP-IDF I2C master bus handle. */
    i2c_master_dev_handle_t dev_handle; /**< ESP-IDF I2C device handle. */
};

/**
 * @brief The single module-wide LCD instance.
 *
 * The HD44780 descriptor is pre-wired with the PCF8574 pin mapping, 5x8 font,
 * row count, and backlight initially on. All fd/handle fields start NULL and the
 * device starts uninitialised until ::lcd_init runs.
 */
struct lcd_i2c lcd_dev = {
    .lcd = { .write_cb = write_cb, 
             .pins = { .rs = PIN_RS_BIT , .e = PIN_E_BIT , .d4 = PIN_D4_BIT , .d5 = PIN_D5_BIT , .d6 = PIN_D6_BIT , .d7 = PIN_D7_BIT , .bl = PIN_BL_BIT  },
             .font = HD44780_FONT_5X8,
             .lines = LCD_ROW_CNT,
             .backlight = true    /* backlight initially ON */
    },
    .pcf8574_i2c_addr = PCF8574_I2C_ADDR_DEFAULT,
    .pcf8574_i2c_addr_len = PCF8574_ADDR_LEN_DEFAULT,
    .esp_i2c_sda_pin = 0,
    .esp_i2c_scl_pin = 0,    
    .esp_i2c_freq = PCF8574_I2C_CLK_FREQ_DEFAULT,
    .esp_i2c_clk_src = 0,
    .esp_i2c_port = 0,
    .lcd_cols = LCD_COL_CNT,
    .lcd_row_pinning = 0,
    .lcd_cursor_pos_row = 0,
    .lcd_cursor_pos_col = 0,
    .lcd_init_state = false,
    .lcd_mutex = NULL,
    .bus_handle = NULL,
    .dev_handle = NULL
};

/**
 * @brief Load the built-in default ESP I2C configuration into @p dev.
 *
 * Used by ::lcd_init when the caller passes no user handle. Sets the default SDA,
 * SCL, clock source, and port.
 *
 * @param dev The ::lcd_i2c instance (by value name) to populate.
 */
#define LCD_DEV_DEFAULT_ESP_CONFIG_INIT(dev)    do{ dev.esp_i2c_sda_pin = ESP_SDA_GPIO_DEFAULT; \
                                                    dev.esp_i2c_scl_pin = ESP_SCL_GPIO_DEFAULT; \
                                                    dev.esp_i2c_clk_src = ESP_I2C_CLK_SRC_DEFAULT; \
                                                    dev.esp_i2c_port = ESP_I2C_PORT_DEFAULT; \
                                                  }while(0)

/* ===================== internals (exposed for hd44780) ===================== */

/**
 * @brief HD44780 write callback: push one byte to the PCF8574 over I2C.
 *
 * Registered as @c hd44780_t::write_cb. The vendor driver calls this for every
 * byte it needs to place on the PCF8574 port. The device context is recovered
 * from @c lcd->user_ctx (set in ::lcd_init).
 */
esp_err_t write_cb(const hd44780_t *lcd, uint8_t data){
    struct lcd_i2c* ctx = (struct lcd_i2c*)(lcd->user_ctx);
    return i2c_master_transmit(ctx->dev_handle, &data, 1, 1000);
}

/* ==================== internals (this translation unit) ==================== */

/**
 * @brief Acquire the recursive LCD mutex (blocks indefinitely).
 * @note Recursive: the same task may take it while already holding it, which is
 *       what lets ::lcd_printf call ::lcd_clear / ::lcd_clear_row safely.
 */
static inline void lcd_ops_lock(void){
    xSemaphoreTakeRecursive(lcd_dev.lcd_mutex, portMAX_DELAY);
}

/**
 * @brief Release one level of the recursive LCD mutex.
 */
static inline void lcd_ops_unlock(void){
    xSemaphoreGiveRecursive(lcd_dev.lcd_mutex);
}

/* ================================== API ==================================== */

/**
 * @brief Initialise the LCD: create the mutex, bring up I2C, and init the HD44780.
 *
 * Must be called once (typically from @c app_main) before any other LCD call. If
 * @p luh is NULL the built-in default ESP config is used; otherwise the caller's
 * SDA/SCL/clock-source/port are applied. On any failure every resource created so
 * far is unwound before returning.
 *
 *         @c LCD_ERR_INVALID_STATE if already initialised;
 *         @c LCD_ERR_NEW_MASTER_BUS, @c LCD_ERR_MASTER_ADD_DEVICE, or
 *         @c LCD_ERR_INIT_FAILED if the corresponding bring-up step fails.
 */
esp_err_t lcd_init(struct lcd_user_handle* luh){
    PRINT_MSG(LOG_LVL_DEBUG, "lcd_init: entry");

    if(!(lcd_dev.lcd_mutex = xSemaphoreCreateRecursiveMutex())){
        PRINT_MSG(LOG_LVL_ERROR, "lcd_init: xSemaphoreCreateRecursiveMutex failed, returning %d", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    int retval = ESP_OK;
    if(lcd_dev.lcd_init_state){
        vSemaphoreDelete(lcd_dev.lcd_mutex);
        PRINT_MSG(LOG_LVL_ERROR, "lcd_init: already initialized, returning %d", LCD_ERR_INVALID_STATE);
        return LCD_ERR_INVALID_STATE;
    }    

    /* No user handle -> use the built-in default ESP config; else copy the caller's. */
    if(!luh){
        LCD_DEV_DEFAULT_ESP_CONFIG_INIT(lcd_dev);
    }else{
        lcd_dev.esp_i2c_sda_pin = luh->esp_i2c_sda_pin_usr;
        lcd_dev.esp_i2c_scl_pin = luh->esp_i2c_scl_pin_usr;
        lcd_dev.esp_i2c_clk_src = luh->esp_i2c_clk_src_usr;
        lcd_dev.esp_i2c_port = luh->esp_i2c_port_usr;
    }

    /* Create the I2C master bus. */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = lcd_dev.esp_i2c_port,
        .sda_io_num = lcd_dev.esp_i2c_sda_pin,
        .scl_io_num = lcd_dev.esp_i2c_scl_pin,
        .clk_source = lcd_dev.esp_i2c_clk_src,
        .glitch_ignore_cnt = GLITCH_IGN_CNT,
        .intr_priority = INTR_PRIOR,
        .trans_queue_depth = TQUEUE_DEPTH,
        .flags.enable_internal_pullup = EN_INTR_PULLUP,
    }; 

    retval = i2c_new_master_bus(&bus_cfg, &(lcd_dev.bus_handle));
    if(retval != ESP_OK){
        vSemaphoreDelete(lcd_dev.lcd_mutex);
        PRINT_MSG(LOG_LVL_ERROR, "lcd_init: i2c_new_master_bus failed, retval %d", retval);
        return LCD_ERR_NEW_MASTER_BUS;     
    }

    /* Add the PCF8574 device on the bus. */
    i2c_device_config_t dev_cfg = {
        .device_address = PCF8574_I2C_ADDR_DEFAULT ,
        .scl_speed_hz = PCF8574_I2C_CLK_FREQ_DEFAULT,
        .dev_addr_length = PCF8574_ADDR_LEN_DEFAULT,
    };

    retval = i2c_master_bus_add_device((lcd_dev.bus_handle), &dev_cfg, &(lcd_dev.dev_handle));
    if(retval != ESP_OK){
        i2c_del_master_bus(lcd_dev.bus_handle);
        lcd_dev.bus_handle = NULL;
        vSemaphoreDelete(lcd_dev.lcd_mutex);
        PRINT_MSG(LOG_LVL_ERROR, "lcd_init: i2c_master_bus_add_device failed, retval %d", retval);
        return LCD_ERR_MASTER_ADD_DEVICE;
    }

    /* Initialise the HD44780; user_ctx is the device so write_cb can find it. */
    retval = hd44780_init(&(lcd_dev.lcd), (void*)&lcd_dev);
    if(retval != ESP_OK){
        i2c_master_bus_rm_device(lcd_dev.dev_handle);
        lcd_dev.dev_handle = NULL;
        i2c_del_master_bus(lcd_dev.bus_handle);
        lcd_dev.bus_handle = NULL;
        vSemaphoreDelete(lcd_dev.lcd_mutex);       
        PRINT_MSG(LOG_LVL_ERROR, "lcd_init: hd44780_init failed, retval %d", retval);
        return LCD_ERR_INIT_FAILED;
    }
    lcd_dev.lcd_init_state = true;
    PRINT_MSG(LOG_LVL_INFO, "lcd_init: LCD initialized (SDA=%d SCL=%d port=%d)", lcd_dev.esp_i2c_sda_pin, lcd_dev.esp_i2c_scl_pin, lcd_dev.esp_i2c_port);
    PRINT_MSG(LOG_LVL_DEBUG, "lcd_init: exit");
    return LCD_OK;
}

/**
 * @brief Clear the entire display and reset cursor and pinning state.
 *
 * Clears both rows, moves the shadow cursor to (0,0), and drops all row pinning.
 *
 *         @c LCD_ERR_CLEAR_FAILED if the HD44780 clear fails.
 */
esp_err_t lcd_clear(void){
    PRINT_MSG(LOG_LVL_DEBUG, "lcd_clear: entry");
    esp_err_t retval = ESP_OK;      
    lcd_ops_lock();
    if(!(lcd_dev.lcd_init_state)){
        lcd_ops_unlock();
        PRINT_MSG(LOG_LVL_ERROR, "lcd_clear: LCD not initialized, returning %d", LCD_ERR_INVALID_STATE);
        return LCD_ERR_INVALID_STATE;
    }
    lcd_dev.lcd_cursor_pos_row = 0;
    lcd_dev.lcd_cursor_pos_col = 0;
    lcd_dev.lcd_row_pinning = 0;
    retval = hd44780_clear(&(lcd_dev.lcd));
    if(retval != ESP_OK){
        lcd_ops_unlock();
        PRINT_MSG(LOG_LVL_ERROR, "lcd_clear: hd44780_clear failed, retval %d", retval);
        return LCD_ERR_CLEAR_FAILED;
    }
    lcd_ops_unlock();
    PRINT_MSG(LOG_LVL_DEBUG, "lcd_clear: exit");
    return LCD_OK;
}

/**
 * @brief Clear only the non-pinned row(s).
 *
 * Behaviour by pinning state:
 * - no row pinned  -> clears both rows via ::lcd_clear (cursor returns to (0,0));
 * - row 0 pinned   -> clears row 1 only, leaving row 0 intact;
 * - row 1 pinned   -> clears row 0 only, leaving row 1 intact;
 * - both pinned    -> nothing to clear (benign warning).
 *
 * A pinned row is cleared by overwriting its columns with spaces, then parking
 * the cursor at the start of the just-cleared writable row.
 *
 *         @c LCD_ERR_PUTC_FAILED / @c LCD_ERR_GOTOXY_FAILED / @c LCD_ERR_CLEAR_FAILED
 *         on the corresponding HD44780 failure; @c LCD_WARN_NOTHING_TO_CLEAR when
 *         both rows are pinned.
 */
esp_err_t lcd_clear_row(void){
    PRINT_MSG(LOG_LVL_DEBUG, "lcd_clear_row: entry");
    esp_err_t retval = ESP_OK;
    lcd_ops_lock();
    if(!(lcd_dev.lcd_init_state)){
        lcd_ops_unlock();
        PRINT_MSG(LOG_LVL_ERROR, "lcd_clear_row: LCD not initialized, returning %d", LCD_ERR_INVALID_STATE);
        return LCD_ERR_INVALID_STATE;
    }
    switch(lcd_dev.lcd_row_pinning){
        case 0:         /* no row pinned: clear both (lcd_clear resets cursor to (0,0)) */
            retval = lcd_clear();
            if(retval != ESP_OK){
                lcd_ops_unlock();
                PRINT_MSG(LOG_LVL_ERROR, "lcd_clear_row: lcd_clear failed, retval %d", retval);
                return LCD_ERR_CLEAR_FAILED;
            }           
            break;
        case 1:         /* row 0 pinned: clear row 1 only */
             for(uint8_t i = 0; i < LCD_COL_CNT; i++){
                retval = hd44780_putc(&(lcd_dev.lcd), ' ');          
                if(retval != ESP_OK){
                    lcd_ops_unlock();
                    PRINT_MSG(LOG_LVL_ERROR, "lcd_clear_row: hd44780_putc failed, retval %d", retval);
                    return LCD_ERR_PUTC_FAILED;
                }
            }
            retval = hd44780_gotoxy(&(lcd_dev.lcd), COL(0), ROW(1)); 
            if(retval != ESP_OK){
                lcd_ops_unlock();
                PRINT_MSG(LOG_LVL_ERROR, "lcd_clear_row: hd44780_gotoxy failed, retval %d", retval);
                return LCD_ERR_GOTOXY_FAILED;    
            } 
            break;
        case 2:         /* row 1 pinned: clear row 0 only */
            for(uint8_t i = 0; i < LCD_COL_CNT; i++){
                retval = hd44780_putc(&(lcd_dev.lcd), ' ');          
                if(retval != ESP_OK){
                    lcd_ops_unlock();
                    PRINT_MSG(LOG_LVL_ERROR, "lcd_clear_row: hd44780_putc failed, retval %d", retval);
                    return LCD_ERR_PUTC_FAILED;
                }
            }
            retval = hd44780_gotoxy(&(lcd_dev.lcd), COL(0), ROW(0));
            if(retval != ESP_OK){
                lcd_ops_unlock();
                PRINT_MSG(LOG_LVL_ERROR, "lcd_clear_row: hd44780_gotoxy failed, retval %d", retval);
                return LCD_ERR_GOTOXY_FAILED;    
            } 
            break;
        case 3:         /* both rows pinned: nothing to clear */
        default:
            lcd_ops_unlock();
            PRINT_MSG(LOG_LVL_WARN, "lcd_clear_row: both rows pinned, nothing to clear");
            return LCD_WARN_NOTHING_TO_CLEAR;   
    }
    lcd_ops_unlock();  
    PRINT_MSG(LOG_LVL_DEBUG, "lcd_clear_row: exit");  
    return LCD_OK;
}

/**
 * @brief Formatted write to the LCD, respecting row pinning.
 *
 * Formats @p fmt into an internal buffer sized to the whole display
 * (@c LCD_ROW_CNT * @c LCD_COL_CNT + 1) and writes it, overwriting the previous
 * ::lcd_printf output. Semantics:
 * - pin/unpin decide where the cursor rests, and this function writes into the
 *   writable region only:
 *     - row 0 pinned  -> writing starts at (1,0);
 *     - row 1 pinned  -> writing starts at (0,0);
 *     - both pinned   -> nothing writable, returns an error;
 * - writing wraps from the end of row 0 to the start of row 1 (at column
 *   @c LCD_COL_CNT-1);
 * - on exit the shadow cursor is restored to where it was on entry, so the next
 *   call overwrites consistently.
 */
int lcd_printf(const char *fmt, ...){
    PRINT_MSG(LOG_LVL_DEBUG, "lcd_printf: entry");
    if(!fmt){
        PRINT_MSG(LOG_LVL_ERROR, "lcd_printf: NULL format, returning %d", LCD_ERR_INVALID_ARG);
        return LCD_ERR_INVALID_ARG;
    }
    esp_err_t retval = ESP_OK;
    char buff[LCD_ROW_CNT*LCD_COL_CNT + 1] = {0};       /* +1 for NUL terminator */

    va_list args;
    va_start(args, fmt);
    vsnprintf(buff, sizeof(buff), fmt, args);
    va_end(args);
    int written = strlen(buff);
    lcd_ops_lock();
    if(!(lcd_dev.lcd_init_state)){
        lcd_ops_unlock();
        PRINT_MSG(LOG_LVL_ERROR, "lcd_printf: LCD not initialized, returning %d", LCD_ERR_INVALID_STATE);
        return LCD_ERR_INVALID_STATE;
    }
    if(lcd_dev.lcd_row_pinning == ((1<<ROW(1))|(1<<ROW(0)))){
        lcd_ops_unlock();
        PRINT_MSG(LOG_LVL_WARN, "lcd_printf: both rows pinned, nothing writable");
        return -1;
    }
    PRINT_MSG(LOG_LVL_DEBUG, "lcd_printf: printing \"%s\"", buff);

    /* Remember the entry cursor so it can be restored before returning. */
    uint8_t old_pos_row = lcd_dev.lcd_cursor_pos_row;
    uint8_t old_pos_col = lcd_dev.lcd_cursor_pos_col;

    if(lcd_dev.lcd_row_pinning){
        /* A row is pinned: clear only the writable row and let its cursor stand. */
        retval = lcd_clear_row(); 
        if(retval != ESP_OK){
            lcd_ops_unlock();
            PRINT_MSG(LOG_LVL_ERROR, "lcd_printf: lcd_clear_row failed, retval %d", retval);
            return LCD_ERR_CLEAR_FAILED;
        }
    }else{
        /* Nothing pinned: clear the whole display, then restore the entry cursor. */
        retval = lcd_clear();
        if(retval != ESP_OK){
            lcd_ops_unlock();
            PRINT_MSG(LOG_LVL_ERROR, "lcd_printf: lcd_clear failed, retval %d", retval);
            return LCD_ERR_CLEAR_FAILED;
        }
        retval = hd44780_gotoxy(&(lcd_dev.lcd), old_pos_col, old_pos_row); 
        if(retval != ESP_OK){
            lcd_ops_unlock();
            PRINT_MSG(LOG_LVL_ERROR, "lcd_printf: hd44780_gotoxy failed, retval %d", retval);
            return LCD_ERR_GOTOXY_FAILED;    
        }    
    }

    /* Emit characters; wrap from end of row 0 to start of row 1. */
    for(uint8_t i = 0;i < written; i++ ){
        retval = hd44780_putc(&(lcd_dev.lcd), buff[i]); 
        if(retval != ESP_OK){
            lcd_ops_unlock();
            PRINT_MSG(LOG_LVL_ERROR, "lcd_printf: hd44780_putc failed, retval %d", retval);
            return LCD_ERR_PUTC_FAILED;
        }
        if(i == LCD_COL_CNT-1){
            retval = hd44780_gotoxy(&(lcd_dev.lcd), COL(0), ROW(1)); 
            if(retval != ESP_OK){
                lcd_ops_unlock();
                PRINT_MSG(LOG_LVL_ERROR, "lcd_printf: hd44780_gotoxy failed, retval %d", retval);
                return LCD_ERR_GOTOXY_FAILED;
            }
        }
    }

    /* Restore the shadow cursor to the entry position for consistent overwrites. */
    lcd_dev.lcd_cursor_pos_row = old_pos_row;
    lcd_dev.lcd_cursor_pos_col = old_pos_col;
    retval = hd44780_gotoxy(&(lcd_dev.lcd), old_pos_col, old_pos_row); 
    if(retval != ESP_OK){
        lcd_ops_unlock();
        PRINT_MSG(LOG_LVL_ERROR, "lcd_printf: hd44780_gotoxy failed, retval %d", retval);
        return LCD_ERR_GOTOXY_FAILED;    
    }    
    lcd_ops_unlock();
    PRINT_MSG(LOG_LVL_DEBUG, "lcd_printf: exit (%d chars)", written);
    return written;
}

/**
 * @brief Pin a row so it is protected from clears/overwrites, and park the cursor.
 *
 * After pinning, the cursor is positioned in the remaining writable region:
 * - pin row 0  -> cursor -> (1, 0);
 * - pin row 1  -> cursor -> (0, 0);
 * - both pinned -> cursor -> (1, 15) (no writable cells remain).
 *
 *         @c LCD_ERR_INVALID_ARG for an out-of-range row;
 *         @c LCD_ERR_GOTOXY_FAILED if cursor positioning fails.
 */
esp_err_t lcd_row_pin(uint8_t row){
    PRINT_MSG(LOG_LVL_DEBUG, "lcd_row_pin: entry (row=%d)", row);
    esp_err_t retval = ESP_OK;
    uint8_t _row = 0;
    uint8_t _col = 0;
    lcd_ops_lock();
    if(!(lcd_dev.lcd_init_state)){
        lcd_ops_unlock();
        PRINT_MSG(LOG_LVL_ERROR, "lcd_row_pin: LCD not initialized, returning %d", LCD_ERR_INVALID_STATE);
        return LCD_ERR_INVALID_STATE;
    }
    if(row == ROW(0) || row == ROW(1)){
        (lcd_dev.lcd_row_pinning)|=(1<<row);
        lcd_dev.lcd_cursor_pos_col = COL(0);
        _col = COL(0);
        if(lcd_dev.lcd_row_pinning == ((1<<ROW(1))|(1<<ROW(0)))){
            /* both pinned -> park at (1,15) */
            lcd_dev.lcd_cursor_pos_col = COL(15);
            lcd_dev.lcd_cursor_pos_row = ROW(1);
            _col = COL(15);            
            _row = ROW(1);
        }else 
        if(row == ROW(0)){
            lcd_dev.lcd_cursor_pos_row = ROW(1);
            _row = ROW(1);    
        }else{
            lcd_dev.lcd_cursor_pos_row = ROW(0);
            _row = ROW(0);
        }
        retval = hd44780_gotoxy(&(lcd_dev.lcd), _col, _row);
        if(retval != ESP_OK){
            lcd_ops_unlock();
            PRINT_MSG(LOG_LVL_ERROR, "lcd_row_pin: hd44780_gotoxy failed, retval %d", retval);
            return LCD_ERR_GOTOXY_FAILED;
        }
    }else{ 
        lcd_ops_unlock();
        PRINT_MSG(LOG_LVL_ERROR, "lcd_row_pin: invalid row %d, returning %d", row, LCD_ERR_INVALID_ARG);
        return LCD_ERR_INVALID_ARG; 
    }
    lcd_ops_unlock();
    PRINT_MSG(LOG_LVL_DEBUG, "lcd_row_pin: exit (pinning=%d cursor r%d c%d)", lcd_dev.lcd_row_pinning, lcd_dev.lcd_cursor_pos_row, lcd_dev.lcd_cursor_pos_col);
    return LCD_OK;
}

/**
 * @brief Unpin a row and move the cursor to (0,0).
 *
 * Clearing either row's pin bit resets the shadow cursor to (0,0) (valid whether
 * row 0 or row 1 was unpinned).
 *
 *         @c LCD_ERR_INVALID_ARG for an out-of-range row;
 *         @c LCD_ERR_GOTOXY_FAILED if cursor positioning fails.
 */
esp_err_t lcd_row_unpin(uint8_t row){
    PRINT_MSG(LOG_LVL_DEBUG, "lcd_row_unpin: entry (row=%d)", row);
    esp_err_t retval = ESP_OK;
    lcd_ops_lock();
    if(!(lcd_dev.lcd_init_state)){
        lcd_ops_unlock();
        PRINT_MSG(LOG_LVL_ERROR, "lcd_row_unpin: LCD not initialized, returning %d", LCD_ERR_INVALID_STATE);
        return LCD_ERR_INVALID_STATE;
    }
    if(row == ROW(0) || row == ROW(1)){
        (lcd_dev.lcd_row_pinning)&=(~(1<<row));
        lcd_dev.lcd_cursor_pos_row = ROW(0);   /* (0,0) is valid for unpinning either row */
        lcd_dev.lcd_cursor_pos_col = COL(0);
        retval = hd44780_gotoxy(&(lcd_dev.lcd), COL(0), ROW(0)); 
        if(retval != ESP_OK){
            lcd_ops_unlock();
            PRINT_MSG(LOG_LVL_ERROR, "lcd_row_unpin: hd44780_gotoxy failed, retval %d", retval);
            return LCD_ERR_GOTOXY_FAILED;
        }
    }else{
        lcd_ops_unlock();
        PRINT_MSG(LOG_LVL_ERROR, "lcd_row_unpin: invalid row %d, returning %d", row, LCD_ERR_INVALID_ARG);
        return LCD_ERR_INVALID_ARG;
    }
    lcd_ops_unlock();
    PRINT_MSG(LOG_LVL_DEBUG, "lcd_row_unpin: exit");
    return LCD_OK;
}

/**
 * @brief Move the shadow cursor and hardware cursor to (@p row, @p col).
 *
 *         @c LCD_ERR_INVALID_STATE if not initialised;
 *         @c LCD_ERR_GOTOXY_FAILED if the HD44780 positioning fails.
 */
esp_err_t lcd_set_cursor(uint8_t row, uint8_t col){
    PRINT_MSG(LOG_LVL_DEBUG, "lcd_set_cursor: entry (row=%d col=%d)", row, col);
    if (row >= LCD_ROW_CNT || col >= LCD_COL_CNT){
        PRINT_MSG(LOG_LVL_ERROR, "lcd_set_cursor: out of range (row=%d col=%d), returning %d", row, col, LCD_ERR_INVALID_ARG);
        return LCD_ERR_INVALID_ARG;
    }

    esp_err_t retval = ESP_OK;
    lcd_ops_lock();
    if(!(lcd_dev.lcd_init_state)){
        lcd_ops_unlock();
        PRINT_MSG(LOG_LVL_ERROR, "lcd_set_cursor: LCD not initialized, returning %d", LCD_ERR_INVALID_STATE);
        return LCD_ERR_INVALID_STATE;
    }
    lcd_dev.lcd_cursor_pos_row = row;
    lcd_dev.lcd_cursor_pos_col = col;
    retval = hd44780_gotoxy(&(lcd_dev.lcd), col, row); 
    if(retval != ESP_OK){
        lcd_ops_unlock();
        PRINT_MSG(LOG_LVL_ERROR, "lcd_set_cursor: hd44780_gotoxy failed, retval %d", retval);
        return LCD_ERR_GOTOXY_FAILED;
    }
    lcd_ops_unlock();
    PRINT_MSG(LOG_LVL_DEBUG, "lcd_set_cursor: exit");
    return LCD_OK;
}

/**
 * @brief Tear down the LCD: remove the I2C device/bus and delete the mutex.
 *
 * Resets all cursor/pinning state and marks the device uninitialised. After this
 * call no other LCD function may run until ::lcd_init is called again.
 *
 * @warning Not thread-safe against concurrent LCD use: the mutex is released and
 *          then deleted, so no other task may be calling into the LCD when this
 *          runs.
 */
esp_err_t lcd_deinit(void){
    PRINT_MSG(LOG_LVL_DEBUG, "lcd_deinit: entry");
    lcd_ops_lock();
    if(!(lcd_dev.lcd_init_state)){
        lcd_ops_unlock();
        PRINT_MSG(LOG_LVL_ERROR, "lcd_deinit: LCD not initialized, returning %d", LCD_ERR_INVALID_STATE);
        return LCD_ERR_INVALID_STATE;
    }
    i2c_master_bus_rm_device(lcd_dev.dev_handle);
    i2c_del_master_bus(lcd_dev.bus_handle);
    lcd_dev.dev_handle = NULL;
    lcd_dev.bus_handle = NULL;
    lcd_dev.lcd_row_pinning = 0;
    lcd_dev.lcd_cursor_pos_row = 0;
    lcd_dev.lcd_cursor_pos_col = 0;
    lcd_dev.lcd_init_state = false;
    lcd_ops_unlock();    
    vSemaphoreDelete(lcd_dev.lcd_mutex);    
    lcd_dev.lcd_mutex = NULL;
    PRINT_MSG(LOG_LVL_INFO, "lcd_deinit: LCD torn down");
    PRINT_MSG(LOG_LVL_DEBUG, "lcd_deinit: exit");
    return LCD_OK;
}
