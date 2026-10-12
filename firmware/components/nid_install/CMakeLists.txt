# Mode installation : Wi-Fi via le coprocesseur ESP32-C6 (ESP-Hosted, déclaré dans
# idf_component.yml : le gestionnaire de composants l'ajoute aux dépendances
# privées), serveur web et page embarquée.
idf_component_register(
    SRCS "nid_install.c" "nid_install_wifi.c" "nid_install_web.c"
    INCLUDE_DIRS "include"
    REQUIRES nid_config
    PRIV_REQUIRES nid_board nid_detect nid_capture nid_storage nid_clock nid_power nid_ir
                  esp_http_server esp_netif esp_event esp_wifi esp_timer nvs_flash lwip
    EMBED_TXTFILES "www/index.html"
)
