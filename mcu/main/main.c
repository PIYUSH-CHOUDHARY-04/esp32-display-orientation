/**
 * @file main.c
 * @ingroup esp_firmware
 * @brief Application entry point and supervisor for the orientation monitor.
 *
 * Brings up the board-level prerequisites the components assume already exist
 * (NVS, netif, the default event loop, the shared GPIO ISR service), initialises
 * the LCD, Wi-Fi manager, and BMA400 sensor, then parks in a supervisor loop.
 *
 * ### Supervisor / restart model
 * The Wi-Fi manager task and the sensor task each take @c app_main's own task
 * handle and a distinct "died" notification bit. When either task exits (its own
 * fatal path), it notifies @c app_main via that bit; the loop below wakes, tears
 * the dead component down, waits briefly, and re-initialises just that component —
 * so one subsystem failing does not take the other down.
 *
 * ### Logging
 * All logging is developer-facing UART trace via ::PRINT_MSG (the "app" component
 * tag) and carries no end-user meaning. @c LOG_LVL_DEBUG marks entry/exit and
 * fine-grained bring-up steps, @c LOG_LVL_INFO marks milestones (boot, each
 * subsystem ready), @c LOG_LVL_WARN marks a task-death/restart, and
 * @c LOG_LVL_ERROR marks an init failure.
 */

#define LOG_COMPONENT_APP
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "wifi_mgr.h"
#include "logging.h"
#include "sensor.h"
#include "lcd.h"

/**
 * @name Task-death notification bits
 * @brief Distinct bits set on @c app_main's task handle when a component task exits.
 * @{
 */
#define NOTIFY_WIFI_MGR_DIED_BIT        0   /**< Bit 0: Wi-Fi manager task died. */
#define NOTIFY_SENSOR_DIED_BIT          1   /**< Bit 1: sensor task died. */
/** @} */

/**
 * @brief Initialise board-level prerequisites the components do not create themselves.
 *
 * Sets up, in order:
 * 1. NVS — required by @c _get_boot_epoch() inside @c wifi_mgr_init() (on a
 *    corrupt/version-mismatched partition it is erased and re-initialised);
 * 2. netif — required by @c esp_netif_create_default_wifi_sta()/_ap();
 * 3. the default event loop — required by @c esp_event_handler_register() for
 *    WIFI_EVENT / IP_EVENT inside @c wifi_mgr_driver_init();
 * 4. the shared GPIO ISR service — the sensor's @c _sensor_isr_init_ only adds a
 *    per-pin handler and assumes the global service already exists.
 *
 * The sensor needs no further prerequisites: it initialises SPI itself and only
 * requires SPI3 to be free and GPIO already up. All steps are wrapped in
 * @c ESP_ERROR_CHECK — a failure here aborts the boot.
 */
static void init_prerequisites(void){
    PRINT_MSG(LOG_LVL_DEBUG, "init_prerequisites: entry");

    /* 1) NVS — required by _get_boot_epoch() inside wifi_mgr_init(). */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        PRINT_MSG(LOG_LVL_WARN, "init_prerequisites: NVS needs erase (ret %d), erasing", ret);
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    PRINT_MSG(LOG_LVL_DEBUG, "init_prerequisites: NVS ready");

    /* 2) netif — required by esp_netif_create_default_wifi_sta/ap(). */
    ESP_ERROR_CHECK(esp_netif_init());
    PRINT_MSG(LOG_LVL_DEBUG, "init_prerequisites: netif ready");

    /* 3) default event loop — required for WIFI_EVENT / IP_EVENT registration. */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    PRINT_MSG(LOG_LVL_DEBUG, "init_prerequisites: default event loop ready");

    /* 4) shared GPIO ISR service — the sensor adds only a per-pin handler. */
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    PRINT_MSG(LOG_LVL_DEBUG, "init_prerequisites: GPIO ISR service installed");

    PRINT_MSG(LOG_LVL_INFO, "init_prerequisites: prerequisites ready");
    PRINT_MSG(LOG_LVL_DEBUG, "init_prerequisites: exit");
}

/** @brief Maximum Wi-Fi manager restarts (reserved for future cap on restarts). */
#define MAX_WIFI_MGR_RESTARTS       5

/**
 * @brief Raise the per-component RUNTIME log levels so compiled-in DEBUG is visible.
 *
 * Each component's compile-time ceiling (in @c logging.h) only decides which levels
 * are built into flash; a second, runtime gate (@c esp_log_level_set, defaulting to
 * @c CONFIG_LOG_DEFAULT_LEVEL, typically INFO) still filters what actually prints.
 * Without this, the DEBUG entry/exit traces compiled into @c wifi_mgr and @c app
 * would be silently dropped at runtime.
 *
 * The strategy is: pin the wildcard low to silence ESP-IDF's own chatty tags
 * (@c wifi:, @c esp_netif:, @c phy:, ...), then raise only our own tags to match
 * the level we compiled in for each. @c lcd stays at WARN because its ceiling is
 * WARN (raising its runtime level higher would print nothing, since INFO/DEBUG
 * were never compiled for it).
 *
 * @note For the DEBUG lines to appear, @c CONFIG_LOG_MAXIMUM_LEVEL must be
 *       DEBUG or VERBOSE in sdkconfig; otherwise ESP-IDF strips DEBUG globally
 *       regardless of these calls.
 */
static void configure_log_levels(void){
    esp_log_level_set("*",        ESP_LOG_INFO);    /* silence ESP-IDF internal tags. */
    esp_log_level_set("wifi_mgr", ESP_LOG_DEBUG);   /* matches WIFI_MGR_LOG_LVL. */
    esp_log_level_set("sensor",   ESP_LOG_DEBUG);    /* matches SENSOR_LOG_LVL. */
    esp_log_level_set("lcd",      ESP_LOG_DEBUG);    /* matches LCD_LOG_LVL. */
    esp_log_level_set("app",      ESP_LOG_DEBUG);   /* matches APP_LOG_LVL. */
}

/**
 * @brief Application entry point: bring up all subsystems and supervise them.
 *
 * Logs a boot banner, initialises the prerequisites, then the LCD, Wi-Fi manager,
 * and sensor (an LCD or sensor init failure is logged but non-fatal — the loop
 * still runs). Then it blocks on a task notification, waking whenever a component
 * task sets its "died" bit, and restarts that component in isolation (deinit,
 * short delay, re-init).
 *
 * The @c xTaskNotifyWait mask semantics: clear no bits on entry (so a death
 * signalled while we were busy is not lost), clear all bits on exit (so each
 * notification is consumed once).
 */
void app_main(void){
    configure_log_levels();   /* raise runtime levels before the first log line. */
    PRINT_MSG(LOG_LVL_INFO, "app_main: *** ESP32 boot OK - wifi_mgr bring-up ***");
    PRINT_MSG(LOG_LVL_DEBUG, "app_main: entry");

    TaskHandle_t self = xTaskGetCurrentTaskHandle();

    init_prerequisites();

    int res;
    res = lcd_init(NULL);
    if(res != LCD_OK){
        PRINT_MSG(LOG_LVL_ERROR, "app_main: lcd_init failed : %d", res);
    }else{
        PRINT_MSG(LOG_LVL_INFO, "app_main: LCD initialised");
    }

    wifi_mgr_init(self, sensor_get_immediate_orientation, NOTIFY_WIFI_MGR_DIED_BIT);
    res = sensor_init(self, wifi_mgr_send_cmd, NOTIFY_SENSOR_DIED_BIT);
    if(res != 0){
        PRINT_MSG(LOG_LVL_ERROR, "app_main: sensor_init failed : %d", res);
    }else{
        PRINT_MSG(LOG_LVL_INFO, "app_main: sensor initialised");
    }

    PRINT_MSG(LOG_LVL_INFO, "app_main: entering supervisor loop");
    for(;;){
        uint32_t notify_bits = 0x00000000;
        xTaskNotifyWait(0x00,            /* clear no bits on entry (don't lose a death signalled while busy). */
                        0xFFFFFFFF,      /* clear all bits on exit (consume each notification once). */
                        &notify_bits,    /* receives which bits were set. */
                        portMAX_DELAY);
        PRINT_MSG(LOG_LVL_DEBUG, "app_main: woke, notify_bits 0x%08" PRIx32, notify_bits);

        if(notify_bits & (1 << NOTIFY_WIFI_MGR_DIED_BIT)){
            PRINT_MSG(LOG_LVL_WARN, "app_main: Wi-Fi task died, restarting");
            wifi_mgr_deinit();
            vTaskDelay(pdMS_TO_TICKS(2000));
            wifi_mgr_init(self, sensor_get_immediate_orientation, NOTIFY_WIFI_MGR_DIED_BIT);
            PRINT_MSG(LOG_LVL_INFO, "app_main: Wi-Fi manager restarted");
        }

        if(notify_bits & (1 << NOTIFY_SENSOR_DIED_BIT)){
            PRINT_MSG(LOG_LVL_WARN, "app_main: sensor task died, restarting");
            sensor_deinit();
            vTaskDelay(pdMS_TO_TICKS(1000));
            sensor_init(self, wifi_mgr_send_cmd, NOTIFY_SENSOR_DIED_BIT);
            PRINT_MSG(LOG_LVL_INFO, "app_main: sensor restarted");
        }
    }
}
