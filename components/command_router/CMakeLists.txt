idf_component_register(
    SRCS "command_router.c" "command_router_pong.c" "timezone_command.c" "log_command.c"
    INCLUDE_DIRS "include"
    REQUIRES domain json nvs_flash esp_timer ota_update ota_policy ambit_ota script_update device_commands sched_runner device_config time_sync timezone event_log evlog_replay sd_card sd_logger diag
)
