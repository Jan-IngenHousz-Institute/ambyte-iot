idf_component_register(
    SRCS "ambit_flash.c" "ambit_flash_preflight.c" "loader_port_ambyte.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES uart_sensors
    # event_log: only for evq_hil_io_w.h (verification-build fault wrappers).
    PRIV_REQUIRES driver esp_timer esp_serial_flasher sd_card device_commands nvs_flash event_log
)
# esp_serial_flasher is vendored at components/esp_serial_flasher (source only — the
# registry tarball's deep test/ paths blow past Windows MAX_PATH). We use our own
# esp_loader_port_ops_t vtable (loader_port_ambyte.c), not its bundled esp32 port.
# Cold SD-fault/diagnostic paths: compiled for size, not the project's -Og
# (keeps the write-integrity release inside its flash budget).
set_source_files_properties("ambit_flash_preflight.c" PROPERTIES COMPILE_OPTIONS "-Os")
