idf_component_register(
    SRCS "hello_world_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        spi_flash
        esp_timer
        esp_driver_gpio
        driver
)