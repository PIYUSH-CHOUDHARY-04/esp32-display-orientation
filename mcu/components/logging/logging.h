#ifndef LOGGING_H
#define LOGGING_H

/**
 * @file logging.h
 * @ingroup esp_firmware
 * @brief Thin ESP-IDF logging front end.
 *
 * A single call style, @c PRINT_MSG(level, fmt, ...), that dispatches to the
 * matching @c ESP_LOGx macro. This keeps ESP-IDF's timestamps, per-tag runtime
 * control (@c esp_log_level_set), thread safety, and compile-time elision of
 * levels above a component's ceiling (format strings above the ceiling are not
 * emitted into flash).
 *
 * ### Per-component control
 * Each translation unit declares its component by defining one @c LOG_COMPONENT_*
 * flag BEFORE including this header:
 *
 * @code
 * #define LOG_COMPONENT_WIFI_MGR
 * #include "logging.h"
 * @endcode
 *
 * The flag selects that file's compile-time ceiling (@c LOG_LOCAL_LEVEL, from the
 * matching @c *_LOG_LVL below) and its @c TAG string. A file with no flag falls
 * back to the @c "app" tag and @c APP_LOG_LVL, so it still compiles and stays
 * visible.
 *
 * ### Cutting ESP-IDF internal noise at runtime
 * Keep the global runtime level low and raise only your own tags, once, early in
 * app_main():
 * @code
 * esp_log_level_set("*",        ESP_LOG_WARN);   // silence wifi:/esp_netif:/phy:
 * esp_log_level_set("wifi_mgr", ESP_LOG_DEBUG);
 * esp_log_level_set("sensor",   ESP_LOG_DEBUG);
 * @endcode
 * (Compile-time ceilings below must admit DEBUG for those components, and
 * @c CONFIG_LOG_MAXIMUM_LEVEL must be DEBUG/VERBOSE.)
 *
 * @note @c LOG_LOCAL_LEVEL and @c TAG are set from the component flag BEFORE
 *       @c esp_log.h is included, so the flag must be defined before this header.
 */

/**
 * @name Severity levels
 * @brief Values accepted by ::PRINT_MSG, mirroring the five ESP-IDF levels.
 * @{
 */
#define LOG_LVL_ERROR   0   /**< Error: an operation failed. */
#define LOG_LVL_WARN    1   /**< Warning: unexpected but recoverable. */
#define LOG_LVL_INFO    2   /**< Info: normal operational milestone. */
#define LOG_LVL_DEBUG   3   /**< Debug: developer-level detail. */
#define LOG_LVL_VERBOSE 4   /**< Verbose: ultra-fine tracing. */
/** @} */

/**
 * @name Per-component compile-time ceilings
 * @brief Edit here to tune build verbosity; levels above a component's ceiling
 *        are compiled out entirely.
 * @{
 */
#define WIFI_MGR_LOG_LVL   ESP_LOG_DEBUG   /**< wifi_mgr ceiling. */
#define LCD_LOG_LVL        ESP_LOG_WARN    /**< lcd ceiling. */
#define SENSOR_LOG_LVL     ESP_LOG_INFO    /**< sensor ceiling. */
#define APP_LOG_LVL        ESP_LOG_DEBUG   /**< app / no-flag fallback ceiling. */
/** @} */

/*
 * Establish LOG_LOCAL_LEVEL and TAG from the component flag BEFORE esp_log.h is
 * included, so ESP-IDF honours the per-component compile-time ceiling.
 */
#ifdef  LOG_COMPONENT_WIFI_MGR
#define LOG_LOCAL_LEVEL     WIFI_MGR_LOG_LVL
#define _LOG_TAG_STR        "wifi_mgr"
#endif

#ifdef  LOG_COMPONENT_LCD
#define LOG_LOCAL_LEVEL     LCD_LOG_LVL
#define _LOG_TAG_STR        "lcd"
#endif

#ifdef  LOG_COMPONENT_SENSOR
#define LOG_LOCAL_LEVEL     SENSOR_LOG_LVL
#define _LOG_TAG_STR        "sensor"
#endif

#ifdef  LOG_COMPONENT_APP
#define LOG_LOCAL_LEVEL     APP_LOG_LVL
#define _LOG_TAG_STR        "app"
#endif

/* Fallback: no component flag -> still compiles, logs stay visible. */
#ifndef _LOG_TAG_STR
#define LOG_LOCAL_LEVEL     APP_LOG_LVL
#define _LOG_TAG_STR        "app"
#endif

#include "esp_log.h"

/**
 * @brief Per-file ESP-IDF log tag (from the component flag, or "app" fallback).
 *        Used by ::PRINT_MSG and by @c esp_log_level_set() for runtime control.
 */
static const char *TAG __attribute__((unused)) = _LOG_TAG_STR;

/**
 * @brief Emit a log line at @p lvl through the matching @c ESP_LOGx macro.
 *
 * Compile-time stripping is preserved: each branch is a distinct @c ESP_LOGx, so
 * levels above the component ceiling are elided.
 *
 * @param lvl One of the @c LOG_LVL_* severities.
 * @param fmt printf-style format string (no trailing newline; ESP-IDF adds one).
 * @param ... Arguments for @p fmt.
 */
#define PRINT_MSG(lvl, fmt, ...) \
    do { \
        switch (lvl) { \
            case LOG_LVL_ERROR:   ESP_LOGE(TAG, fmt, ##__VA_ARGS__); break; \
            case LOG_LVL_WARN:    ESP_LOGW(TAG, fmt, ##__VA_ARGS__); break; \
            case LOG_LVL_INFO:    ESP_LOGI(TAG, fmt, ##__VA_ARGS__); break; \
            case LOG_LVL_DEBUG:   ESP_LOGD(TAG, fmt, ##__VA_ARGS__); break; \
            default:              ESP_LOGV(TAG, fmt, ##__VA_ARGS__); break; \
        } \
    } while (0)

#endif /* LOGGING_H */
