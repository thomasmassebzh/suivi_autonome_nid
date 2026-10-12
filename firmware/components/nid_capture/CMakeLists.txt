idf_component_register(
    SRCS "nid_capture.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nid_board esp_timer esp_driver_i2s esp_driver_jpeg esp_driver_gpio
)
