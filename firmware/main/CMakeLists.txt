idf_component_register(
    SRCS "main.c" "nid_app.c" "nid_state.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nid_board nid_config nid_storage nid_clock nid_power nid_detect
                  nid_ir nid_capture nid_install
                  esp_timer esp_system esp_hw_support esp_app_format nvs_flash freertos
)
