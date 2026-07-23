idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES i2c_bus wifi_manager nvs_flash ambyte_status lua_runner pcf2131tfy_rtc bme280 mp2731 CLI device_commands command_router event_log sd_card sd_logger ota_update ambit_ota ambit_flash script_update domain littlefs mqtt_client device_config timezone certs uart_sensors sync_runner esp_event esp_netif esp_wifi esp_pm
)
