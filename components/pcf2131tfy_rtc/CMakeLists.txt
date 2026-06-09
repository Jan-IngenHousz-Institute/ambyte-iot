idf_component_register(
    SRCS
        "PCF2131_base.cpp"
        "PCF2131_I2C.cpp"
        "RTC_NXP.cpp"
        "pcf2131tfy_rtc_api.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES domain
    PRIV_REQUIRES driver i2c_bus esp_timer
)
