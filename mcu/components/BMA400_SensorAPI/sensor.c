/**
 * @file sensor.c
 * @ingroup esp_firmware
 * @brief BMA400 accelerometer driver: orientation detection via activity /
 *        inactivity interrupts (ESP-IDF, SPI).
 *
 * The BMA400 is wired over SPI and drives a single interrupt line (INT1 -> GPIO4).
 * Two of its "generic" interrupts form a small state machine:
 * - GEN1 = activity   (motion started),
 * - GEN2 = inactivity (board settled).
 *
 * A worker task (::sensor_task) toggles between the two: it arms activity, and on
 * activity it arms inactivity; when the board settles it averages a batch of
 * accelerometer samples, derives one of four upright orientations
 * (::_derive_orientation_), and reports it through a caller-supplied callback —
 * but only when the orientation actually changes. It then re-arms activity.
 *
 * ### Interrupt / ISR path
 * The GPIO ISR (::__sensor_int_isr__) does nothing but notify the task (no SPI,
 * no logging), keeping it ISR-safe. The task blocks on the notification.
 *
 * ### SPI concurrency
 * Every BMA400 SPI transfer goes through ::bma400_spi_read / ::bma400_spi_write,
 * which serialise the physical transfer under ::g_sensor::spi_mutex. This is what
 * lets another task (e.g. the Wi-Fi manager) call ::sensor_get_immediate_orientation
 * concurrently with the worker task without corrupting a transfer.
 *
 * ### Orientation model
 * Orientation is derived purely from the gravity vector's dominant axis. Pitch and
 * yaw are deliberately ignored (they don't change which axis gravity points along
 * in the way we care about). A Z-dominant reading means the board is lying flat,
 * where in-plane rotation is undefined, so the previous orientation is kept.
 *
 * ### Logging
 * All logging is developer-facing UART trace via ::PRINT_MSG and carries no
 * end-user meaning. Every function logs @c LOG_LVL_DEBUG on entry and on each exit
 * path, @c LOG_LVL_ERROR at each failure (naming the function and the failing
 * call/retval), @c LOG_LVL_INFO at operational milestones, and @c LOG_LVL_WARN on
 * benign/recoverable conditions. The ISR and the hot-path SPI hooks are the
 * exceptions: they must stay fast/ISR-safe and therefore are not entry/exit traced.
 *
 * @note FIFO mode is intentionally not used; the driver reads accel data directly.
 */

#define LOG_COMPONENT_SENSOR
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_err.h"

#include "bma400.h"
#include "bma400_defs.h"

#include "sensor.h"
#include "logging.h"

/**
 * @name SPI wiring and bus parameters
 * @{
 */
#define SENSOR_SPI_HOST     SPI3_HOST      /**< SPI host controller. */
#define SENSOR_PIN_SCLK     GPIO_NUM_18    /**< SPI clock. */
#define SENSOR_PIN_MOSI     GPIO_NUM_23    /**< SPI MOSI. */
#define SENSOR_PIN_MISO     GPIO_NUM_19    /**< SPI MISO. */
#define SENSOR_PIN_CS       GPIO_NUM_5     /**< SPI chip select. */
#define SENSOR_PIN_INT1     GPIO_NUM_4     /**< BMA400 INT1 interrupt line. */
#define SENSOR_SPI_CLK_HZ   (1*1000*1000)  /**< SPI clock; the BMA400 tolerates up to 10 MHz. */
/** @} */

/**
 * @name Worker task parameters
 * @{
 */
#define SENSOR_TASK_STACK_SIZE  4096   /**< Task stack size (bytes). */
#define SENSOR_TASK_PRIO        10     /**< Task priority. */
#define SENSOR_TASK_CORE        1      /**< Core the task is pinned to. */
/** @} */

/**
 * @name Sampling
 * @{
 */
#define SETTLE_SAMPLE_COUNT     10   /**< Samples averaged after the board settles. */
#define IMMEDIATE_SAMPLE_COUNT  4    /**< Samples averaged for an on-demand query. */
#define READ_WRITE_LEN          32   /**< Max BMA400 SPI payload length. */
/** @} */

static void sensor_task(void* arg);
static BMA400_INTF_RET_TYPE bma400_spi_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t length, void *intf_ptr);
static BMA400_INTF_RET_TYPE bma400_spi_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t length, void *intf_ptr);
static void bma400_delay_us(uint32_t period, void *intf_ptr);

/**
 * @brief All module state (single static instance ::gs).
 */
struct g_sensor {
    spi_device_handle_t     spidev;              /**< ESP-IDF SPI device handle. */
    struct bma400_dev       dev;                 /**< Vendor BMA400 descriptor. */
    TaskHandle_t            sensor_task_handle;  /**< Worker task handle. */
    TaskHandle_t            caller_handle;       /**< Task notified (via die_bit) if the worker exits. */
    uint8_t                 die_bit;             /**< Notification bit set on @c caller_handle at exit. */
    display_orient_t        last_orient;         /**< Most recently reported orientation (dedup). */
    void (*send_orient)(display_orient_t);       /**< Callback invoked when orientation changes. */
    SemaphoreHandle_t       spi_mutex;           /**< Serialises BMA400 SPI transfers across tasks. */
};

static struct g_sensor gs = {
    .spidev = NULL,
    .dev = {
        .intf = BMA400_SPI_INTF,
        .read = bma400_spi_read,
        .write = bma400_spi_write,
        .delay_us = bma400_delay_us,
        .intf_ptr = &gs.spidev,
        .read_write_len = READ_WRITE_LEN
    },
    .sensor_task_handle = NULL,
    .caller_handle = NULL,
    .die_bit = 0,
    .last_orient = ORIENT_INVALID,
    .send_orient = NULL,
    .spi_mutex = NULL
};

/* ============================ SPI transfer ops ============================= */

/**
 * @brief BMA400 SPI read hook (registered as @c bma400_dev::read).
 *
 * Sends the register address then clocks in @p length bytes. The physical transfer
 * is serialised under ::g_sensor::spi_mutex so it can't interleave with another
 * task's transfer. The first received byte is a dummy byte inherent to BMA400 SPI
 * reads and is skipped when copying into @p reg_data.
 *
 * @note No entry/exit DEBUG trace here: this runs on every register access and is
 *       called at high frequency from the vendor driver; tracing it would flood the
 *       log and perturb timing. Only oversize/SPI failure is reported (as the
 *       vendor error code); the calling high-level function logs the context.
 *
 * @param reg_addr Register to read.
 * @param reg_data Output buffer for @p length bytes.
 * @param length   Number of bytes to read (must be <= @c READ_WRITE_LEN).
 * @param intf_ptr Points at the @c spi_device_handle_t.
 * @return @c BMA400_OK on success; @c BMA400_E_COM_FAIL on oversize or SPI error.
 */
static BMA400_INTF_RET_TYPE bma400_spi_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t length, void *intf_ptr){
    spi_device_handle_t spi = *(spi_device_handle_t*)intf_ptr;
    if(length > READ_WRITE_LEN){
        return BMA400_E_COM_FAIL;
    }
    uint8_t txbuf[1+32] = {0};
    uint8_t rxbuf[1+32] = {0};
    txbuf[0] = reg_addr;
    spi_transaction_t t = {0};
    t.length    = (1+length)*8;
    t.tx_buffer = txbuf;
    t.rx_buffer = rxbuf;

    xSemaphoreTake(gs.spi_mutex, portMAX_DELAY);
    esp_err_t retval = spi_device_polling_transmit(spi, &t);
    xSemaphoreGive(gs.spi_mutex);
    if(retval != ESP_OK){
        return BMA400_E_COM_FAIL;
    }
    memcpy(reg_data, &rxbuf[1], length);
    return BMA400_OK;
}

/**
 * @brief BMA400 SPI write hook (registered as @c bma400_dev::write).
 *
 * Prepends the register address and clocks out @p length bytes, serialised under
 * ::g_sensor::spi_mutex.
 *
 * @note As with ::bma400_spi_read, this hot-path hook is intentionally not
 *       entry/exit traced.
 *
 * @param reg_addr Register to write.
 * @param reg_data Source bytes.
 * @param length   Number of bytes (must be <= @c READ_WRITE_LEN).
 * @param intf_ptr Points at the @c spi_device_handle_t.
 * @return @c BMA400_OK on success; @c BMA400_E_COM_FAIL on oversize or SPI error.
 */
static BMA400_INTF_RET_TYPE bma400_spi_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t length, void *intf_ptr){
    spi_device_handle_t spi = *(spi_device_handle_t*)intf_ptr;
    if(length > READ_WRITE_LEN){
        return BMA400_E_COM_FAIL;
    }
    uint8_t txbuf[1+32] = {0};
    txbuf[0] = reg_addr;
    memcpy(&txbuf[1], reg_data, length);
    spi_transaction_t t = {0};
    t.length    = (1+length)*8;
    t.tx_buffer = txbuf;
    t.rx_buffer = NULL;
    xSemaphoreTake(gs.spi_mutex, portMAX_DELAY);
    esp_err_t retval = spi_device_polling_transmit(spi, &t);
    xSemaphoreGive(gs.spi_mutex);
    if(retval != ESP_OK){
        return BMA400_E_COM_FAIL;
    }
    return BMA400_OK;
}

/**
 * @brief BMA400 microsecond delay hook (registered as @c bma400_dev::delay_us).
 *
 * @note Not traced: it is invoked constantly by the vendor driver's timing loops.
 *
 * @param period Microseconds to busy-wait.
 * @param intf_ptr Unused.
 */
static void bma400_delay_us(uint32_t period, void *intf_ptr){
    esp_rom_delay_us(period);
    return;
}

/* ============================== bring-up steps ============================= */

/**
 * @brief Initialise the SPI bus and add the BMA400 as a device on it.
 *
 * First configures the shared SCLK/MOSI/MISO bus, then adds the BMA400 as a device
 * on it. Bus tuning notes, folded from the original inline comments:
 * - @c quadwp_io_num / @c quadhd_io_num are unused (they exist only for quad-SPI);
 * - @c max_transfer_sz is small because every read/write is a few bytes;
 * - SPI mode 0 (CPOL=0, CPHA=0) is the BMA400 default;
 * - @c spics_io_num lets the driver toggle CS automatically;
 * - @c queue_size is 1 because transfers are polling, one at a time;
 * - @c command_bits / @c address_bits / @c dummy_bits are all 0 because the
 *   register address is placed manually in the tx buffer and the read's leading
 *   dummy byte is stripped by hand in ::bma400_spi_read.
 * On device-add failure the bus init is unwound before returning.
 *
 * @return @c ESP_OK on success, or the failing @c esp_err_t.
 */
static esp_err_t _sensor_spi_init_(void){
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_spi_init_: entry");
    esp_err_t err;
    spi_bus_config_t buscfg = {
        .mosi_io_num     = SENSOR_PIN_MOSI,
        .miso_io_num     = SENSOR_PIN_MISO,
        .sclk_io_num     = SENSOR_PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 64,
    };

    err = spi_bus_initialize(SENSOR_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if(err != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_spi_init_: spi_bus_initialize failed, retval %d", err);
        return err;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SENSOR_SPI_CLK_HZ,
        .mode           = 0,
        .spics_io_num   = SENSOR_PIN_CS,
        .queue_size     = 1,
        .command_bits   = 0,
        .address_bits   = 0,
        .dummy_bits     = 0,
    };

    err = spi_bus_add_device(SENSOR_SPI_HOST, &devcfg, &gs.spidev);
    if(err != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_spi_init_: spi_bus_add_device failed, retval %d", err);
        spi_bus_free(SENSOR_SPI_HOST);
        return err;
    }
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_spi_init_: exit (SPI ready)");
    return ESP_OK;
}

/**
 * @brief Probe the BMA400: run vendor init and verify the chip id.
 * @return @c ESP_OK if init succeeds and the chip id is @c BMA400_CHIP_ID (0x90);
 *         @c ESP_FAIL otherwise.
 */
static esp_err_t _sensor_probe_(void){
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_probe_: entry");
    int rslt = bma400_init(&gs.dev);
    if(rslt != BMA400_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_probe_: bma400_init failed, retval %d", rslt);
        return ESP_FAIL;
    }
    if(gs.dev.chip_id != BMA400_CHIP_ID){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_probe_: unexpected chip id 0x%02x (expected 0x90)", gs.dev.chip_id);
        return ESP_FAIL;
    }
    PRINT_MSG(LOG_LVL_INFO, "_sensor_probe_: BMA400 detected (chip id 0x%02x)", gs.dev.chip_id);
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_probe_: exit");
    return ESP_OK;
}

/**
 * @brief Soft-reset the BMA400.
 * @return @c ESP_OK on success; @c ESP_FAIL if the reset fails.
 */
static esp_err_t _sensor_reset_soft_(void){
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_reset_soft_: entry");
    if(bma400_soft_reset(&gs.dev) != BMA400_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_reset_soft_: bma400_soft_reset failed");
        return ESP_FAIL;
    }
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_reset_soft_: exit");
    return ESP_OK;
}

/**
 * @brief Put the BMA400 into normal power mode.
 * @return @c ESP_OK on success; @c ESP_FAIL if the mode set fails.
 */
static esp_err_t _sensor_power_normal_(void){
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_power_normal_: entry");
    if(bma400_set_power_mode(BMA400_MODE_NORMAL, &gs.dev) != BMA400_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_power_normal_: bma400_set_power_mode failed");
        return ESP_FAIL;
    }
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_power_normal_: exit");
    return ESP_OK;
}

/**
 * @name Accelerometer configuration
 * @{
 */
#define SENSOR_ODR          BMA400_ODR_100HZ              /**< Output data rate. */
#define SENSOR_RANGE        BMA400_RANGE_2G              /**< Measurement range. */
#define SENSOR_DATA_SRC     BMA400_DATA_SRC_ACCEL_FILT_1 /**< Data source filter. */
#define SENSOR_OSR          BMA400_ACCEL_OSR_SETTING_3   /**< Oversampling. */
#define SENSOR_OSR_LP       BMA400_ACCEL_OSR_SETTING_3   /**< Low-power oversampling. */
#define SENSOR_FILT1_BW     BMA400_ACCEL_FILT1_BW_0      /**< Filter-1 bandwidth. */
/** @} */

/**
 * @brief Configure the accelerometer (ODR, range, data source, oversampling).
 *
 * @c int_chan is set to @c BMA400_UNMAP_INT_PIN so the accelerometer data path
 * itself does not drive an interrupt line — only the GEN1/GEN2 detectors do.
 *
 * @return @c ESP_OK on success; @c ESP_FAIL if the config write fails.
 */
static esp_err_t _sensor_config_accel_(void){
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_config_accel_: entry");
    struct bma400_sensor_conf conf_accel = {0};
    conf_accel.type = BMA400_ACCEL;
    conf_accel.param.accel.odr = BMA400_ODR_100HZ;
    conf_accel.param.accel.range = BMA400_RANGE_2G;
    conf_accel.param.accel.data_src = BMA400_DATA_SRC_ACCEL_FILT_1;
    conf_accel.param.accel.osr = BMA400_ACCEL_OSR_SETTING_3;
    conf_accel.param.accel.osr_lp = BMA400_ACCEL_OSR_SETTING_3;
    conf_accel.param.accel.filt1_bw = BMA400_ACCEL_FILT1_BW_0;
    conf_accel.param.accel.int_chan = BMA400_UNMAP_INT_PIN;

    if(bma400_set_sensor_conf(&conf_accel, 1, &gs.dev) != BMA400_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_config_accel_: bma400_set_sensor_conf failed");
        return ESP_FAIL;
    }
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_config_accel_: exit");
    return ESP_OK;
}

/**
 * @name Activity (GEN1) interrupt configuration
 * @brief "Motion started" detector. Threshold is ~64 mg; duration ~10 ms so it
 *        reacts quickly to any axis moving.
 * @{
 */
#define SENSOR_ACTIVITY_THRES           8                          /**< ~64 mg motion threshold (tune). */
#define SENSOR_ACTIVITY_INTERVAL_DUR    1                          /**< 1 sample (~10 ms) — react fast. */
#define SENSOR_ACTIVITY_AXIS_SEL        BMA400_AXIS_XYZ_EN         /**< Evaluate all three axes. */
#define SENSOR_ACTIVITY_DATA_SRC        BMA400_DATA_SRC_ACC_FILT1  /**< Data source. */
#define SENSOR_ACTIVITY_EVAL_AXES       BMA400_ANY_AXES_INT        /**< Fire if ANY axis moves. */
#define SENSOR_ACTIVITY_REF_UPDATE      BMA400_UPDATE_EVERY_TIME   /**< Reference update policy. */
#define SENSOR_ACTIVITY_HYST            BMA400_HYST_48_MG          /**< Hysteresis. */
#define SENSOR_ACTIVITY_INT_PIN         BMA400_INT_CHANNEL_1       /**< Routed to INT1. */
/** @} */

/**
 * @brief Configure the GEN1 (activity) interrupt: fires when motion begins.
 *
 * Reads the current GEN1 config first, overlays the tuning above, then writes it
 * back. @c criterion_sel is @c BMA400_ACTIVITY_INT, meaning "above threshold =
 * moving"; @c evaluate_axes fires on ANY axis so motion in any direction wakes the
 * detector.
 *
 * @return @c ESP_OK on success; @c ESP_FAIL if a get/set config call fails.
 */
static esp_err_t _sensor_config_activity_(void){
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_config_activity_: entry");
    struct bma400_sensor_conf conf_activity = {0};
    conf_activity.type = BMA400_GEN1_INT;

    if(bma400_get_sensor_conf(&conf_activity, 1, &gs.dev) != BMA400_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_config_activity_: bma400_get_sensor_conf failed");
        return ESP_FAIL;
    }

    conf_activity.param.gen_int.gen_int_thres   = SENSOR_ACTIVITY_THRES;
    conf_activity.param.gen_int.gen_int_dur     = SENSOR_ACTIVITY_INTERVAL_DUR;
    conf_activity.param.gen_int.axes_sel        = SENSOR_ACTIVITY_AXIS_SEL;
    conf_activity.param.gen_int.data_src        = SENSOR_ACTIVITY_DATA_SRC;
    conf_activity.param.gen_int.criterion_sel   = BMA400_ACTIVITY_INT;
    conf_activity.param.gen_int.evaluate_axes   = SENSOR_ACTIVITY_EVAL_AXES;
    conf_activity.param.gen_int.ref_update      = SENSOR_ACTIVITY_REF_UPDATE;
    conf_activity.param.gen_int.hysteresis      = SENSOR_ACTIVITY_HYST;
    conf_activity.param.gen_int.int_thres_ref_x = 0;
    conf_activity.param.gen_int.int_thres_ref_y = 0;
    conf_activity.param.gen_int.int_thres_ref_z = 0;
    conf_activity.param.gen_int.int_chan        = SENSOR_ACTIVITY_INT_PIN;

    if(bma400_set_sensor_conf(&conf_activity, 1, &gs.dev) != BMA400_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_config_activity_: bma400_set_sensor_conf failed");
        return ESP_FAIL;
    }
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_config_activity_: exit");
    return ESP_OK;
}

/**
 * @name Inactivity (GEN2) interrupt configuration
 * @brief "Settled" detector. After motion drops below threshold it waits
 *        @c SENSOR_INACTIVITY_INTERVAL_DUR samples (~80 ms at the configured ODR)
 *        before declaring the board settled.
 * @{
 */
#define SENSOR_INACTIVITY_THRES             8                          /**< ~64 mg threshold (tune). */
#define SENSOR_INACTIVITY_INTERVAL_DUR      8                          /**< Wait ~80 ms below threshold before "settled". */
#define SENSOR_INACTIVITY_AXIS_SEL          BMA400_AXIS_XYZ_EN         /**< All three axes. */
#define SENSOR_INACTIVITY_DATA_SRC          BMA400_DATA_SRC_ACC_FILT1  /**< Data source. */
#define SENSOR_INACTIVITY_EVAL_AXES         BMA400_ALL_AXES_INT        /**< Fire only when ALL axes inactive. */
#define SENSOR_INACTIVITY_REF_UPDATE        BMA400_UPDATE_EVERY_TIME   /**< Reference update policy. */
#define SENSOR_INACTIVITY_HYST              BMA400_HYST_48_MG          /**< Hysteresis. */
#define SENSOR_INACTIVITY_INT_PIN           BMA400_INT_CHANNEL_1       /**< Routed to INT1 -> GPIO4. */
/** @} */

/**
 * @brief Configure the GEN2 (inactivity) interrupt: fires when the board settles.
 *
 * Mirror of ::_sensor_config_activity_ with inverted criterion: @c criterion_sel
 * is @c BMA400_INACTIVITY_INT ("below threshold = settled") and @c evaluate_axes
 * is ALL axes, so it fires only once every axis has gone quiet — avoiding a false
 * "settled" while one axis is still moving.
 *
 * @return @c ESP_OK on success; @c ESP_FAIL if a get/set config call fails.
 */
static esp_err_t _sensor_config_inactivity_(void){
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_config_inactivity_: entry");
    struct bma400_sensor_conf conf_inactivity = {0};
    conf_inactivity.type = BMA400_GEN2_INT;

    if(bma400_get_sensor_conf(&conf_inactivity, 1, &gs.dev) != BMA400_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_config_inactivity_: bma400_get_sensor_conf failed");
        return ESP_FAIL;
    }

    conf_inactivity.param.gen_int.gen_int_thres = SENSOR_INACTIVITY_THRES;
    conf_inactivity.param.gen_int.gen_int_dur = SENSOR_INACTIVITY_INTERVAL_DUR;
    conf_inactivity.param.gen_int.axes_sel = SENSOR_INACTIVITY_AXIS_SEL;
    conf_inactivity.param.gen_int.data_src = SENSOR_INACTIVITY_DATA_SRC;
    conf_inactivity.param.gen_int.criterion_sel = BMA400_INACTIVITY_INT;
    conf_inactivity.param.gen_int.evaluate_axes = SENSOR_INACTIVITY_EVAL_AXES;
    conf_inactivity.param.gen_int.ref_update = SENSOR_INACTIVITY_REF_UPDATE;
    conf_inactivity.param.gen_int.hysteresis = SENSOR_INACTIVITY_HYST;
    conf_inactivity.param.gen_int.int_thres_ref_x = 0;
    conf_inactivity.param.gen_int.int_thres_ref_y = 0;
    conf_inactivity.param.gen_int.int_thres_ref_z = 0;
    conf_inactivity.param.gen_int.int_chan = SENSOR_INACTIVITY_INT_PIN;

    if(bma400_set_sensor_conf(&conf_inactivity, 1, &gs.dev) != BMA400_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_config_inactivity_: bma400_set_sensor_conf failed");
        return ESP_FAIL;
    }
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_config_inactivity_: exit");
    return ESP_OK;
}

/**
 * @brief Route interrupts to the INT1 pin and enable latched interrupts.
 *
 * First sets INT1 to push-pull / active-high, then enables interrupt latching so a
 * fired interrupt stays asserted until its status is read. Latching matters because
 * the worker reads the status register to learn which interrupt fired; without it a
 * brief pulse could be missed between the ISR notification and the status read.
 *
 * @return @c ESP_OK on success; @c ESP_FAIL if a config/enable call fails.
 */
static esp_err_t _sensor_map_interrupts_(void){
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_map_interrupts_: entry");
    struct bma400_device_conf conf_map_int = {0};
    conf_map_int.type = BMA400_INT_PIN_CONF;
    conf_map_int.param.int_conf.int_chan = BMA400_INT_CHANNEL_1;
    conf_map_int.param.int_conf.pin_conf = BMA400_INT_PUSH_PULL_ACTIVE_1;
    if(bma400_set_device_conf(&conf_map_int, 1, &gs.dev) != BMA400_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_map_interrupts_: bma400_set_device_conf failed");
        return ESP_FAIL;
    }

    struct bma400_int_enable int_en;
    int_en.type = BMA400_LATCH_INT_EN;
    int_en.conf = BMA400_ENABLE;
    if(bma400_enable_interrupt(&int_en, 1, &gs.dev) != BMA400_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_map_interrupts_: bma400_enable_interrupt(latch) failed");
        return ESP_FAIL;
    }
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_map_interrupts_: exit");
    return ESP_OK;
}

/* =============================== ISR / GPIO =============================== */

/**
 * @brief GPIO ISR for INT1: notify the worker task and yield if it woke.
 *
 * Deliberately minimal and ISR-safe: no SPI, no logging, no blocking — just a task
 * notification. The @p arg is the worker task handle captured at handler-add time.
 *
 * @warning This function must never log or take a mutex; doing either from ISR
 *          context would be unsafe. It is the one function here without DEBUG trace.
 *
 * @param arg The worker @c TaskHandle_t to notify.
 */
static void IRAM_ATTR __sensor_int_isr__(void* arg){
    TaskHandle_t handle = (TaskHandle_t)arg;
    BaseType_t task_awake = pdFALSE;
    vTaskNotifyGiveFromISR(handle, &task_awake);
    if(task_awake == pdTRUE){
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief Configure the INT1 GPIO and attach the ISR handler.
 *
 * INT1 is an input with pull-down (idle low; the BMA400 drives it high),
 * rising-edge triggered.
 *
 * @note The global GPIO ISR service is assumed to be installed elsewhere (once, at
 *       app start); only the per-pin handler is added here. This lets the sensor be
 *       de-inited and re-inited without tearing down a service other GPIOs share.
 *
 * @return @c ESP_OK on success; @c ESP_FAIL if pin config or handler-add fails.
 */
static esp_err_t _sensor_isr_init_(void){
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_isr_init_: entry");
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << SENSOR_PIN_INT1),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    if(gpio_config(&io) != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_isr_init_: gpio_config failed");
        return ESP_FAIL;
    }
    if(gpio_isr_handler_add(SENSOR_PIN_INT1, __sensor_int_isr__, (void*)gs.sensor_task_handle) != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_isr_init_: gpio_isr_handler_add failed");
        return ESP_FAIL;
    }
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_isr_init_: exit");
    return ESP_OK;
}

/* ===================== interrupt arm / disarm helpers ===================== */

/**
 * @brief Enable the GEN1 (activity) interrupt.
 * @return @c ESP_OK on success; @c ESP_FAIL otherwise.
 */
static esp_err_t _sensor_activity_enable_(void){
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_activity_enable_: entry");
    struct bma400_int_enable en = { .type = BMA400_GEN1_INT_EN, .conf = BMA400_ENABLE };
    if(bma400_enable_interrupt(&en, 1, &gs.dev) != BMA400_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_activity_enable_: bma400_enable_interrupt failed");
        return ESP_FAIL;
    }
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_activity_enable_: exit");
    return ESP_OK;
}

/**
 * @brief Disable the GEN1 (activity) interrupt.
 * @return @c ESP_OK on success; @c ESP_FAIL otherwise.
 */
static esp_err_t _sensor_activity_disable_(void){
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_activity_disable_: entry");
    struct bma400_int_enable en = { .type = BMA400_GEN1_INT_EN, .conf = BMA400_DISABLE };
    if(bma400_enable_interrupt(&en, 1, &gs.dev) != BMA400_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_activity_disable_: bma400_enable_interrupt failed");
        return ESP_FAIL;
    }
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_activity_disable_: exit");
    return ESP_OK;
}

/**
 * @brief Enable the GEN2 (inactivity) interrupt.
 * @return @c ESP_OK on success; @c ESP_FAIL otherwise.
 */
static esp_err_t _sensor_inactivity_enable_(void){
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_inactivity_enable_: entry");
    struct bma400_int_enable en = { .type = BMA400_GEN2_INT_EN, .conf = BMA400_ENABLE };
    if(bma400_enable_interrupt(&en, 1, &gs.dev) != BMA400_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_inactivity_enable_: bma400_enable_interrupt failed");
        return ESP_FAIL;
    }
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_inactivity_enable_: exit");
    return ESP_OK;
}

/**
 * @brief Disable the GEN2 (inactivity) interrupt.
 * @return @c ESP_OK on success; @c ESP_FAIL otherwise.
 */
static esp_err_t _sensor_inactivity_disable_(void){
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_inactivity_disable_: entry");
    struct bma400_int_enable en = { .type = BMA400_GEN2_INT_EN, .conf = BMA400_DISABLE };
    if(bma400_enable_interrupt(&en, 1, &gs.dev) != BMA400_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_sensor_inactivity_disable_: bma400_enable_interrupt failed");
        return ESP_FAIL;
    }
    PRINT_MSG(LOG_LVL_DEBUG, "_sensor_inactivity_disable_: exit");
    return ESP_OK;
}

/* ================================ public API ============================== */

/** @brief FreeRTOS name of the worker task. */
#define TASK_NAME   "sensor_task"

/**
 * @brief Initialise the sensor: SPI, BMA400, interrupt config, worker task, ISR.
 *
 * Bring-up order: create the SPI mutex, init SPI, probe the chip, soft-reset,
 * enter normal power, configure the accelerometer, configure the activity (GEN1)
 * and inactivity (GEN2) interrupts, map them to INT1, then disable both at the
 * BMA400 before the worker arms them itself. Finally the worker task is created
 * and the GPIO ISR attached.
 *
 * Both GEN interrupts are disabled at the BMA400 before the MCU-side ISR is
 * enabled, because the worker task arms activity itself once it starts; this avoids
 * an interrupt firing into a not-yet-ready task. Each step returns a distinct
 * negative code so a failure is identifiable at a glance.
 *
 * @note The returned error codes intentionally skip -7 (that value belonged to a
 *       now-removed FIFO-config step); the gap is not a bug.
 *
 * @note The parameter and return contract lives with the DECLARATION, in sensor.h -- not here. That
 *       is where a caller looks, and duplicating it would mean one contract with two copies free to
 *       drift apart.
 */
int sensor_init(TaskHandle_t caller_handle, void (*fptr_send_orient)(display_orient_t), uint8_t die_bit){
    PRINT_MSG(LOG_LVL_DEBUG, "sensor_init: entry");
    if(!fptr_send_orient){
        PRINT_MSG(LOG_LVL_ERROR, "sensor_init: NULL send callback, returning -1");
        return -1;
    }
    gs.send_orient = fptr_send_orient;

    gs.spi_mutex = xSemaphoreCreateMutex();
    if(!gs.spi_mutex){
        PRINT_MSG(LOG_LVL_ERROR, "sensor_init: xSemaphoreCreateMutex failed, returning -2");
        return -2;
    }

    esp_err_t retval;
    retval = _sensor_spi_init_();
    if(retval < 0){
        PRINT_MSG(LOG_LVL_ERROR, "sensor_init: _sensor_spi_init_ failed, retval %d, returning -2", retval);
        return -2;
    }

    retval = _sensor_probe_();
    if(retval < 0){
        PRINT_MSG(LOG_LVL_ERROR, "sensor_init: _sensor_probe_ failed, retval %d, returning -3", retval);
        return -3;
    }

    retval = _sensor_reset_soft_();
    if(retval < 0){
        PRINT_MSG(LOG_LVL_ERROR, "sensor_init: _sensor_reset_soft_ failed, retval %d, returning -4", retval);
        return -4;
    }

    retval = _sensor_power_normal_();
    if(retval < 0){
        PRINT_MSG(LOG_LVL_ERROR, "sensor_init: _sensor_power_normal_ failed, retval %d, returning -5", retval);
        return -5;
    }

    retval = _sensor_config_accel_();
    if(retval < 0){
        PRINT_MSG(LOG_LVL_ERROR, "sensor_init: _sensor_config_accel_ failed, retval %d, returning -6", retval);
        return -6;
    }

    retval = _sensor_config_activity_();
    if(retval < 0){
        PRINT_MSG(LOG_LVL_ERROR, "sensor_init: _sensor_config_activity_ failed, retval %d, returning -8", retval);
        return -8;
    }

    retval = _sensor_config_inactivity_();
    if(retval < 0){
        PRINT_MSG(LOG_LVL_ERROR, "sensor_init: _sensor_config_inactivity_ failed, retval %d, returning -9", retval);
        return -9;
    }

    retval = _sensor_map_interrupts_();
    if(retval < 0){
        PRINT_MSG(LOG_LVL_ERROR, "sensor_init: _sensor_map_interrupts_ failed, retval %d, returning -10", retval);
        return -10;
    }

    retval = _sensor_inactivity_disable_();
    if(retval < 0){
        PRINT_MSG(LOG_LVL_ERROR, "sensor_init: _sensor_inactivity_disable_ failed, retval %d, returning -11", retval);
        return -11;
    }

    retval = _sensor_activity_disable_();
    if(retval < 0){
        PRINT_MSG(LOG_LVL_ERROR, "sensor_init: _sensor_activity_disable_ failed, retval %d, returning -12", retval);
        return -12;
    }

    gs.caller_handle = caller_handle;
    gs.die_bit = die_bit;
    BaseType_t ok = xTaskCreatePinnedToCore(sensor_task, TASK_NAME, SENSOR_TASK_STACK_SIZE, (void*)gs.caller_handle, SENSOR_TASK_PRIO, &gs.sensor_task_handle, SENSOR_TASK_CORE);
    if(ok != pdPASS){
        PRINT_MSG(LOG_LVL_ERROR, "sensor_init: xTaskCreatePinnedToCore failed, returning -13");
        return -13;
    }

    retval = _sensor_isr_init_();
    if(retval < 0){
        PRINT_MSG(LOG_LVL_ERROR, "sensor_init: _sensor_isr_init_ failed, retval %d, returning -14", retval);
        return -14;
    }
    PRINT_MSG(LOG_LVL_INFO, "sensor_init: sensor ready");
    PRINT_MSG(LOG_LVL_DEBUG, "sensor_init: exit");
    return 0;
}

/**
 * @brief Tear down the sensor: detach ISR, sleep the chip, free SPI, reset state.
 *
 * Ordering is deliberate: remove the INT1 GPIO handler first (without uninstalling
 * the shared global ISR service, which other GPIOs may use), reset the pin, put the
 * BMA400 to sleep (best-effort — failure is ignored since we are tearing down
 * anyway) so it stops driving INT1 and drawing current, then remove the SPI device
 * and free the bus so a later ::sensor_init can re-initialise it, and finally clear
 * all bookkeeping.
 *
 * @note The worker task is expected to have already deleted itself via its error
 *       path, so only its stale handle is cleared here — this function does not stop
 *       a running task.
 */
void sensor_deinit(void){
    PRINT_MSG(LOG_LVL_DEBUG, "sensor_deinit: entry");

    gpio_isr_handler_remove(SENSOR_PIN_INT1);
    gpio_reset_pin(SENSOR_PIN_INT1);
    bma400_set_power_mode(BMA400_MODE_SLEEP, &gs.dev);

    if(gs.spidev){
        spi_bus_remove_device(gs.spidev);
        gs.spidev = NULL;
    }
    spi_bus_free(SENSOR_SPI_HOST);

    memset(&gs.dev, 0, sizeof(struct bma400_dev));
    gs.sensor_task_handle = NULL;
    gs.caller_handle = NULL;
    gs.die_bit = 0;
    gs.last_orient = ORIENT_INVALID;
    gs.send_orient = NULL;
    if(gs.spi_mutex){
        vSemaphoreDelete(gs.spi_mutex);
    }
    gs.spi_mutex = NULL;
    PRINT_MSG(LOG_LVL_INFO, "sensor_deinit: sensor torn down");
    PRINT_MSG(LOG_LVL_DEBUG, "sensor_deinit: exit");
    return;
}

/**
 * @brief Reduce an averaged gravity vector to one of four upright orientations.
 *
 * Uses the dominant axis of the gravity vector. The board's mounting is fixed, so
 * the effective gravity vector lands in the set {(-g,0,0),(g,0,0),(0,-g,0),(0,g,0)}
 * for the four upright poses. Pitch and yaw are ignored deliberately: under yaw the
 * gravity vector doesn't change at all, and under pitch the Y component doesn't
 * change. A dominant Z axis means the board is lying flat (face up/down), where
 * in-plane rotation is undefined, so the previous orientation is kept rather than
 * guessed. Otherwise the dominant horizontal axis and its sign select the pose.
 *
 * @note Pure arithmetic decision with no I/O; intentionally not entry/exit traced to
 *       keep the settle-loop log readable. The chosen orientation is logged by the
 *       caller.
 *
 * @param gx Averaged X acceleration.
 * @param gy Averaged Y acceleration.
 * @param gz Averaged Z acceleration.
 * @return @c ORIENT_0 (+X up, upright), @c ORIENT_180 (-X up, inverted),
 *         @c ORIENT_90 (+Y up, 90 deg CW), @c ORIENT_270 (-Y up, 90 deg CCW), or
 *         @c gs.last_orient when the board is flat.
 */
static display_orient_t _derive_orientation_(int gx, int gy, int gz){
    int agx = abs(gx);
    int agy = abs(gy);
    int agz = abs(gz);

    if(agz >= agx && agz >= agy){
        return gs.last_orient;
    }

    if(agx >= agy){
        return (gx > 0) ? ORIENT_0 : ORIENT_180;
    } else {
        return (gy > 0) ? ORIENT_90 : ORIENT_270;
    }
}

/**
 * @brief Read the board's current orientation on demand (thread-safe).
 *
 * Averages @c IMMEDIATE_SAMPLE_COUNT accelerometer samples and derives the
 * orientation. Intended for another task (e.g. the Wi-Fi manager on a new PC
 * connection) to obtain orientation without waiting for a movement/settle event.
 * SPI transfers are serialised by ::g_sensor::spi_mutex inside the read hooks, so
 * this may run concurrently with the worker task.
 *
 * If no sample could be read it treats this as a fatal sensor fault: it notifies the
 * caller via the die-bit and deletes the worker task.
 *
 * @warning The no-sample path calls @c vTaskDelete on the worker task, which may be
 *          holding the SPI mutex at that instant; deleting it there can strand the
 *          mutex. Preserved as original behaviour, but worth revisiting.
 *
 * @note Return contract lives with the declaration, in sensor.h.
 */
display_orient_t sensor_get_immediate_orientation(void){
    PRINT_MSG(LOG_LVL_DEBUG, "sensor_get_immediate_orientation: entry");
    int32_t gx = 0, gy = 0, gz = 0;
    uint8_t recvd_pkts = 0;
    int retval;
    for(int i=0; i < IMMEDIATE_SAMPLE_COUNT; i++){
        struct bma400_sensor_data bsd;
        retval = bma400_get_accel_data(BMA400_DATA_ONLY, &bsd, &gs.dev);
        if(retval == BMA400_OK){
            gx += bsd.x;
            gy += bsd.y;
            gz += bsd.z;
            recvd_pkts++;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if(recvd_pkts > 0){
        display_orient_t orient = _derive_orientation_((int)(gx/recvd_pkts), (int)(gy/recvd_pkts), (int)(gz/recvd_pkts));
        PRINT_MSG(LOG_LVL_INFO, "sensor_get_immediate_orientation: orientation %d", orient);
        PRINT_MSG(LOG_LVL_DEBUG, "sensor_get_immediate_orientation: exit (orient %d)", orient);
        return orient;
    }else{
        PRINT_MSG(LOG_LVL_ERROR, "sensor_get_immediate_orientation: no samples read, notifying caller and deleting task");
        if(gs.caller_handle){
            xTaskNotify((TaskHandle_t)gs.caller_handle, 1 << gs.die_bit, eSetBits);
        }
        vTaskDelete(gs.sensor_task_handle);
        PRINT_MSG(LOG_LVL_DEBUG, "sensor_get_immediate_orientation: exit (ORIENT_INVALID)");
        return ORIENT_INVALID;
    }
}

/**
 * @brief Worker task: activity/inactivity state machine driving orientation.
 *
 * Arms the activity interrupt, then loops waiting on the ISR notification
 * (@c ulTaskNotifyTake). Each wake reads the BMA400 interrupt-status register, which
 * also clears the latch, then acts on which interrupt fired:
 * - GEN1 (activity / motion started): disarm activity, arm inactivity;
 * - GEN2 (inactivity / settled): average @c SETTLE_SAMPLE_COUNT samples, derive the
 *   orientation, report it through the callback only if it changed (dedup against
 *   @c gs.last_orient), then disarm inactivity and re-arm activity.
 *
 * Any interrupt-arming failure jumps to the error path, which notifies the caller
 * via the die-bit and self-deletes the task.
 *
 * @param caller_handle Task to notify (die-bit) if the task exits on error.
 */
static void sensor_task(void* caller_handle){
    PRINT_MSG(LOG_LVL_INFO, "sensor_task: started");
    PRINT_MSG(LOG_LVL_DEBUG, "sensor_task: entry");
    esp_err_t retval;

    retval = _sensor_activity_enable_();
    if(retval < 0){
        PRINT_MSG(LOG_LVL_ERROR, "sensor_task: _sensor_activity_enable_ failed, retval %d", retval);
        goto err;
    }
    uint16_t interrupts;
    for(;;){
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bma400_get_interrupt_status(&interrupts, &gs.dev);

        if(interrupts & BMA400_ASSERTED_GEN1_INT){
            PRINT_MSG(LOG_LVL_DEBUG, "sensor_task: GEN1 (activity) asserted, arming inactivity");
            retval = _sensor_activity_disable_();
            if(retval < 0){
                PRINT_MSG(LOG_LVL_ERROR, "sensor_task: _sensor_activity_disable_ failed, retval %d", retval);
                goto err;
            }
            retval = _sensor_inactivity_enable_();
            if(retval < 0){
                PRINT_MSG(LOG_LVL_ERROR, "sensor_task: _sensor_inactivity_enable_ failed, retval %d", retval);
                goto err;
            }
        }else
        if(interrupts & BMA400_ASSERTED_GEN2_INT){
            PRINT_MSG(LOG_LVL_DEBUG, "sensor_task: GEN2 (settled) asserted, sampling orientation");
            int32_t gx = 0, gy = 0, gz = 0;
            uint8_t recvd_pkts = 0;
            for(int i=0; i<SETTLE_SAMPLE_COUNT; i++){
                struct bma400_sensor_data bsd;
                if(bma400_get_accel_data(BMA400_DATA_ONLY, &bsd, &gs.dev) == BMA400_OK){
                    gx += bsd.x;
                    gy += bsd.y;
                    gz += bsd.z;
                    recvd_pkts++;
                }else{
                    PRINT_MSG(LOG_LVL_WARN, "sensor_task: bma400_get_accel_data failed for one sample");
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if(recvd_pkts > 0){
                display_orient_t orient = _derive_orientation_((int)(gx/recvd_pkts), (int)(gy/recvd_pkts), (int)(gz/recvd_pkts));
                if(orient != gs.last_orient){
                    PRINT_MSG(LOG_LVL_INFO, "sensor_task: orientation changed to %d, sending", orient);
                    gs.send_orient(orient);
                    gs.last_orient = orient;
                }else{
                    PRINT_MSG(LOG_LVL_DEBUG, "sensor_task: orientation unchanged (%d), not sending", orient);
                }
            }

            retval = _sensor_inactivity_disable_();
            if(retval < 0){
                PRINT_MSG(LOG_LVL_ERROR, "sensor_task: _sensor_inactivity_disable_ failed, retval %d", retval);
                goto err;
            }
            retval = _sensor_activity_enable_();
            if(retval < 0){
                PRINT_MSG(LOG_LVL_ERROR, "sensor_task: _sensor_activity_enable_ failed, retval %d", retval);
                goto err;
            }
        }
    }

err:
    PRINT_MSG(LOG_LVL_ERROR, "sensor_task: exiting on error, notifying caller and self-deleting");
    if(caller_handle){
        xTaskNotify((TaskHandle_t)caller_handle, 1 << gs.die_bit, eSetBits);
    }
    PRINT_MSG(LOG_LVL_DEBUG, "sensor_task: exit (deleted)");
    vTaskDelete(NULL);
}
