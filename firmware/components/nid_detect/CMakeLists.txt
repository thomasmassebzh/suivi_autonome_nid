idf_component_register(
    SRCS "nid_detect.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos
    PRIV_REQUIRES nid_board esp_driver_gpio esp_timer
)
