idf_component_register(
    SRCS "ota_manifest.c" "ota_policy.c" "ota_refusal.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_update bootloader_support esp_partition esp_app_format littlefs heap
)
# Keep the signed descriptor even when no runtime code reads the symbol itself.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ota_storage_manifest")
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
