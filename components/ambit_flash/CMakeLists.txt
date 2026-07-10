idf_component_register(
    SRCS "ambit_flash.c" "loader_port_ambyte.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES uart_sensors
    PRIV_REQUIRES driver esp_timer esp_serial_flasher sd_card device_commands nvs_flash
)
# esp_serial_flasher is vendored at components/esp_serial_flasher (source only — the
# registry tarball's deep test/ paths blow past Windows MAX_PATH). We use our own
# esp_loader_port_ops_t vtable (loader_port_ambyte.c), not its bundled esp32 port.
