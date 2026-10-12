idf_component_register(
    SRCS "nid_ir.c" "nid_ir_sun.c"
    INCLUDE_DIRS "include"
    REQUIRES nid_config
    PRIV_REQUIRES nid_board esp_driver_ledc esp_driver_gpio esp_timer
)
