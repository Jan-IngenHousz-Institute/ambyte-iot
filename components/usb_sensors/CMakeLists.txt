idf_component_register(
    SRCS "usb_sensors.c"
    INCLUDE_DIRS "include"
    REQUIRES domain
    PRIV_REQUIRES usb esp_timer freertos
)
