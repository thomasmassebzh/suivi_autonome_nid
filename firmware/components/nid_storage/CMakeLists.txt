idf_component_register(
    SRCS "nid_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs sdmmc esp_driver_sdmmc vfs
    PRIV_REQUIRES nid_board
)
