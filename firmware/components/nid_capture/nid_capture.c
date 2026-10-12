/*
 * nid_capture — caméra MIPI-CSI, vidéo H.264 + son AAC en MP4, photo JPEG
 *
 * SQUELETTE : implémentation à compléter.
 */
#include "nid_capture.h"

#include "esp_log.h"

static const char *TAG = "nid_capture";

esp_err_t nid_capture_init(const nid_capture_settings_t *s)
{
    (void)s;
    ESP_LOGW(TAG, "capture non implémentée");
    return ESP_ERR_NOT_SUPPORTED;
}

bool nid_capture_is_ready(void)
{
    return false;
}

const char *nid_capture_sensor_name(void)
{
    return "aucun";
}

esp_err_t nid_capture_set_night(bool night)
{
    (void)night;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nid_capture_record(const char *path, uint32_t duration_ms,
                             nid_capture_stop_cb_t stop_cb, void *arg,
                             nid_capture_stats_t *stats)
{
    (void)path;
    (void)duration_ms;
    (void)stop_cb;
    (void)arg;
    (void)stats;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nid_capture_photo(const char *path)
{
    (void)path;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nid_capture_preview_start(uint8_t quality, uint16_t max_width)
{
    (void)quality;
    (void)max_width;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nid_capture_preview_frame(const uint8_t **jpeg, size_t *len, uint32_t timeout_ms)
{
    (void)timeout_ms;
    *jpeg = NULL;
    *len = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

void nid_capture_preview_release(void)
{
}

esp_err_t nid_capture_preview_stop(void)
{
    return ESP_OK;
}

esp_err_t nid_capture_deinit(void)
{
    return ESP_OK;
}
