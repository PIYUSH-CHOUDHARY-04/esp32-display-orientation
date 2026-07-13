idf_component_register(
    SRCS "wifi_mgr.c"
    INCLUDE_DIRS "."
    REQUIRES
        common
        esp_wifi
        esp_event
        esp_netif
\       nvs_flash
    PRIV_REQUIRES
        lwip
        logging
        esp_timer
        mbedtls
        HD44780_LCD_I2C
)
