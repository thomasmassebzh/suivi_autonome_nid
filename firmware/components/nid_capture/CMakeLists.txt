idf_component_register(
    SRCS "nid_capture.c" "nid_capture_audio.c" "nid_overlay.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES nid_board esp_timer esp_mm esp_driver_i2s esp_driver_i2c esp_driver_jpeg esp_driver_gpio
                  esp_video esp_cam_sensor esp_muxer esp_codec_dev esp_audio_codec
)
