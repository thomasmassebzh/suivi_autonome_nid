idf_component_register(
    SRCS "nid_power.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_system
    PRIV_REQUIRES nid_board nid_clock esp_adc esp_driver_gpio esp_hw_support
)
