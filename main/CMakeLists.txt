idf_component_register(
    SRCS "hello_world_main.c" "m2.c"
    INCLUDE_DIRS "."
    REQUIRES
        spi_flash
        esp_timer
        esp_driver_gpio
        driver
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        lwip
)
