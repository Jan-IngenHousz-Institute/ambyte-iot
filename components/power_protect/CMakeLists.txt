# power_protect — brownout protection: a persisted normal/no_flash/hold state
# machine (pure core, host-tested by tests/test_power_protect.py) plus the NVS /
# log / counter glue. Consumers: device_commands (feeds charger reads, renders
# health.power_protect), sched_runner (sheds jobs), ota_update / ambit_ota /
# ambit_flash (refuse updates), command_router (power_protect command, pong).
idf_component_register(
    SRCS "power_protect_core.c" "power_protect.c"
    INCLUDE_DIRS "include"
    REQUIRES domain
    PRIV_REQUIRES clock_trust diag esp_system esp_timer freertos nvs_flash log
)
