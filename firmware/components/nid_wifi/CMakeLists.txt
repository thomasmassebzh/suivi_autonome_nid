# Wi-Fi par le coprocesseur ESP32-C6 (ESP-Hosted, déclaré dans idf_component.yml :
# le gestionnaire de composants l'ajoute aux dépendances privées). Commun au
# mode installation du firmware (nid_install) et au banc d'essai.
idf_component_register(
    SRCS "nid_wifi.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nid_board esp_netif esp_event esp_wifi esp_timer nvs_flash
)
