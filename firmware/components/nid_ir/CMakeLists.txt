idf_component_register(
    SRCS "nid_ir.c"
    INCLUDE_DIRS "include"
    REQUIRES nid_config
    PRIV_REQUIRES nid_board esp_driver_ledc esp_driver_gpio
)
