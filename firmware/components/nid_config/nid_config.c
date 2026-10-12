/*
 * nid_config — réglages du système
 *
 * SQUELETTE : implémentation à compléter (lecture/écriture INI, validation, JSON).
 */
#include "nid_config.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "nid_config";

void nid_config_set_defaults(nid_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->system_id, "CAM01", sizeof(cfg->system_id));
    strlcpy(cfg->nest_id, "NID01", sizeof(cfg->nest_id));
    strlcpy(cfg->timezone, "AST4", sizeof(cfg->timezone));
    cfg->seq_duration_s = 30;
    cfg->trigger_delay_s = 60;
    cfg->video_width = 1920;
    cfg->video_height = 1080;
    cfg->video_fps = 15;
    cfg->video_bitrate_kbps = 2500;
    cfg->audio_enabled = true;
    cfg->photo_interval_min = 15;
    cfg->jpeg_quality = 80;
    cfg->pir_sensitivity = 3;
    cfg->pir_warmup_s = 60;
    cfg->night_mode = NID_NIGHT_MODE_AUTO;
    cfg->latitude = 14.77f;
    cfg->longitude = -60.89f;
    cfg->dusk_advance_min = 30;
    cfg->dawn_delay_min = 30;
    cfg->ir_power_pct = 100;
    cfg->night_monochrome = true;
    cfg->batt_low_mv = 3450;
    cfg->batt_critical_mv = 3350;
    cfg->wifi_ssid[0] = '\0';
    strlcpy(cfg->wifi_password, "moqueur-nid", sizeof(cfg->wifi_password));
    cfg->install_timeout_min = 15;
}

esp_err_t nid_config_load(nid_config_t *cfg, const char *path)
{
    (void)cfg;
    ESP_LOGW(TAG, "lecture de %s non implémentée", path);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nid_config_save(const nid_config_t *cfg, const char *path)
{
    (void)cfg;
    ESP_LOGW(TAG, "écriture de %s non implémentée", path);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nid_config_set(nid_config_t *cfg, const char *key, const char *value)
{
    (void)cfg;
    (void)key;
    (void)value;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nid_config_validate(nid_config_t *cfg)
{
    (void)cfg;
    return ESP_OK;
}

size_t nid_config_to_json(const nid_config_t *cfg, char *buf, size_t len)
{
    (void)cfg;
    int n = snprintf(buf, len, "{}");
    return (n > 0 && (size_t)n < len) ? (size_t)n : 0;
}

size_t nid_config_to_ini(const nid_config_t *cfg, char *buf, size_t len)
{
    (void)cfg;
    if (len) {
        buf[0] = '\0';
    }
    return 0;
}

const char *nid_config_night_mode_str(nid_night_mode_t mode)
{
    switch (mode) {
    case NID_NIGHT_MODE_DAY:
        return "jour";
    case NID_NIGHT_MODE_NIGHT:
        return "nuit";
    default:
        return "auto";
    }
}

void nid_config_effective_ssid(const nid_config_t *cfg, char *out, size_t len)
{
    if (cfg->wifi_ssid[0]) {
        strlcpy(out, cfg->wifi_ssid, len);
    } else {
        snprintf(out, len, "NID-%s", cfg->system_id);
    }
}
