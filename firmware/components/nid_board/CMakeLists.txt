idf_component_register(
    SRCS "nid_board.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2c
)
