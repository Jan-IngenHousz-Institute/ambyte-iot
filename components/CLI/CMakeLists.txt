idf_component_register(
    SRCS "CLI.c"
    INCLUDE_DIRS "include"
    REQUIRES device_commands uart_sensors ambit_flash i2c_bus time_sync ambit_ota lua_runner sync_runner event_log
    PRIV_REQUIRES console driver wifi_manager sd_logger pcf2131tfy_rtc app_update esp_partition
)
