idf_component_register(
    SRCS "iot_jobs.c" "iot_jobs_plan.c"
    INCLUDE_DIRS "include"
    REQUIRES domain
    PRIV_REQUIRES json app_update esp_timer ota_update
)
