# Bench-only (env bench-ble). Every release env leaves CONFIG_AMBYTE_BLE_DIAG
# unset (it depends on BT, which sdkconfig.defaults keeps off), so this
# component compiles to nothing and links no Bluetooth code there.
# The requirements are unconditional on purpose: IDF expands component
# requirements before sdkconfig values exist, so a requirement list gated on
# CONFIG_AMBYTE_BLE_DIAG comes out empty. `bt` is in every env's component
# graph anyway; with BT off it only contributes include paths.
set(ble_diag_srcs "")
if(CONFIG_AMBYTE_BLE_DIAG)
    list(APPEND ble_diag_srcs "ble_diag.c" "ble_diag_core.c")
endif()

idf_component_register(
    SRCS ${ble_diag_srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES bt console esp_timer esp_app_format esp_hw_support mp2731 domain
)
