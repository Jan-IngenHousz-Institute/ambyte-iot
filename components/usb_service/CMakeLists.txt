idf_component_register(
    SRCS "svc_wire.c" "svc_store.c" "svc_protocol.c" "svc_engine.c" "svc_session.c" "svc_usb.c" "usb_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES console driver esp_timer heap littlefs esp_wifi esp_hw_support mbedtls script_update wifi_manager
)

# These are defined in separate pinned-IDF translation units. The qualification
# records the final map/disassembly, so an SDK/LTO change cannot silently bypass
# the no-echo/no-unframed-output boundary.
foreach(symbol usb_serial_jtag_driver_install xRingbufferCreate
               xRingbufferSendFromISR xRingbufferReceiveUpTo vRingbufferReturnItem
               usb_serial_jtag_write_bytes)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${symbol}")
endforeach()
