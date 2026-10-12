idf_component_register(
    SRCS "nid_install.c"
    INCLUDE_DIRS "include"
    REQUIRES nid_config
    PRIV_REQUIRES nid_board nid_detect nid_capture nid_storage nid_clock nid_power nid_ir
                  esp_http_server esp_netif esp_event nvs_flash esp_wifi
)
