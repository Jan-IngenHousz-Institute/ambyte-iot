idf_component_register(
    SRCS "CLI.c" "uart_stream_cli_support.c"
    INCLUDE_DIRS "include"
    REQUIRES device_commands uart_sensors uart_stream ambit_flash i2c_bus time_sync timezone clock_trust ambit_ota sched_runner sched_spec script_update sync_runner event_log
    PRIV_REQUIRES console driver wifi_manager sd_logger pcf2131tfy_rtc app_update esp_partition device_config json sd_card mbedtls littlefs nvs_flash spi_flash mp2731
)
