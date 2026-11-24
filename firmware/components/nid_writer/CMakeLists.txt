idf_component_register(
    SRCS "nid_writer.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES esp_timer
)
