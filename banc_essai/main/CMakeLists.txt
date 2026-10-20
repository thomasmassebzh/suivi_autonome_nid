# Police 5x7 de l'incrustation : nid_overlay.h, en-tête interne de nid_capture.
get_filename_component(NID_CAPTURE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/nid_capture" ABSOLUTE)

set(banc_srcs "banc_main.c" "banc_fonctions.c" "banc_video.c" "banc_detection.c" "banc_tests.c" "banc_ui.c"
              "banc_ecran.c" "banc_tactile.c" "banc_console.c")
set(banc_embed "")
# Wi-Fi et tableau de bord web : compilés seulement si la fonction est choisie
# (menu « Banc d'essai › Fonctions activées »). Les dépendances ci-dessous
# restent déclarées (elles ne peuvent pas dépendre de sdkconfig) mais rien
# n'est lié sans la fonction.
if(CONFIG_BANC_F_WIFI)
    list(APPEND banc_srcs "banc_wifi.c" "banc_web.c" "banc_web_vue.c")
    list(APPEND banc_embed "www/banc.html")
endif()

idf_component_register(
    SRCS ${banc_srcs}
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "${NID_CAPTURE_DIR}"
    REQUIRES nid_capture nid_config nid_storage nid_wifi
    PRIV_REQUIRES nid_board nid_detect nid_ir nid_power
                  esp_lcd esp_driver_ppa esp_driver_jpeg esp_driver_gpio esp_driver_i2c esp_driver_tsens
                  esp_hw_support esp_timer esp_system esp_app_format esp_http_server lwip console freertos vfs
                  nvs_flash
    EMBED_TXTFILES ${banc_embed}
)
