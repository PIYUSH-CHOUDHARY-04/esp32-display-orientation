idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        logging
        HD44780_LCD_I2C
        wifi_manager
        BMA400_SensorAPI
        common
        nvs_flash
    PRIV_REQUIRES
        spi_flash
        esp_timer
)
