idf_component_register(
    SRCS
        "lcd.c"
        "hd44780.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        esp_driver_i2c
        esp_driver_gpio
        logging
)
