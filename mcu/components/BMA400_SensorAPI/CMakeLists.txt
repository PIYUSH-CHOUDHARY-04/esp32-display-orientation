idf_component_register(
    SRCS
        "bma400.c"           # Bosch BMA400 driver
        "sensor.c"           # sensor logic (SPI glue, state machine, task)
    INCLUDE_DIRS
        "."
    REQUIRES
        driver               # driver/spi_master.h, driver/gpio.h
        esp_common           # esp_err.h / esp_err_t
        freertos             # tasks, vTaskNotifyGiveFromISR, notifications
        logging              # PRINT_MSG
        common               # common.h → display_orient_t / ORIENT_*
)
