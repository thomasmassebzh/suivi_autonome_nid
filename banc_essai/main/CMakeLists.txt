# Police 5x7 de l'incrustation : nid_overlay.h, en-tête interne de nid_capture.
get_filename_component(NID_CAPTURE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/nid_capture" ABSOLUTE)

idf_component_register(
    SRCS "banc_main.c" "banc_video.c" "banc_detection.c" "banc_tests.c" "banc_ui.c" "banc_ecran.c"
         "banc_tactile.c" "banc_console.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "${NID_CAPTURE_DIR}"
    REQUIRES nid_capture nid_config nid_storage
    PRIV_REQUIRES nid_board nid_detect nid_ir nid_power
                  esp_lcd esp_driver_ppa esp_driver_gpio esp_driver_i2c esp_driver_tsens esp_hw_support
                  esp_timer esp_system esp_app_format console freertos vfs
)
