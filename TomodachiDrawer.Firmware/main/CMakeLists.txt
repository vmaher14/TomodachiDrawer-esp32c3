idf_component_register(
    SRCS
        "TomodachiDrawer.Firmware.c"
        "usb_descriptors.c"
    INCLUDE_DIRS "."
    REQUIRES
        tinyusb
        driver
        esp_partition
        esp_timer
        spi_flash
        esp_driver_gpio
)