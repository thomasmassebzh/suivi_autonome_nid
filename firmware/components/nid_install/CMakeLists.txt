# Mode installation : Wi-Fi via le coprocesseur ESP32-C6 (composant commun
# nid_wifi, qui déclare ESP-Hosted), serveur web et page embarquée.
idf_component_register(
    SRCS "nid_install.c" "nid_install_wifi.c" "nid_install_web.c"
    INCLUDE_DIRS "include"
    REQUIRES nid_config
    PRIV_REQUIRES nid_board nid_detect nid_capture nid_storage nid_clock nid_power nid_ir nid_wifi
                  esp_http_server esp_timer lwip
    EMBED_TXTFILES "www/index.html"
)
