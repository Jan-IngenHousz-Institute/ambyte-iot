idf_component_register(SRCS "sensor_inventory.c" "sensor_json.c"
    INCLUDE_DIRS "include")
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
