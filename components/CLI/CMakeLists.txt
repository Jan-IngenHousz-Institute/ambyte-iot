idf_component_register(
    SRCS "CLI.c"
    INCLUDE_DIRS "include"
    REQUIRES device_commands i2c_bus time_sync
    PRIV_REQUIRES console driver wifi_manager
)
