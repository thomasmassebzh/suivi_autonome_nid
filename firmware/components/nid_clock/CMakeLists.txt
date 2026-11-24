idf_component_register(
    SRCS "nid_clock.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES nvs_flash esp_hw_support
)
