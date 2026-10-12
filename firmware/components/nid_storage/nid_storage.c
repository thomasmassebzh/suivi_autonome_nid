/*
 * nid_storage — carte microSD, nommage des fichiers, journal
 *
 * SQUELETTE : implémentation à compléter (montage SDMMC + LDO, nommage, journal).
 */
#include "nid_storage.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "nid_storage";

esp_err_t nid_storage_mount(void)
{
    ESP_LOGW(TAG, "montage SD non implémenté");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nid_storage_unmount(void)
{
    return ESP_OK;
}

bool nid_storage_is_mounted(void)
{
    return false;
}

esp_err_t nid_storage_get_space(uint64_t *total_bytes, uint64_t *free_bytes)
{
    *total_bytes = 0;
    *free_bytes = 0;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t nid_storage_set_ids(const char *system_id, const char *nest_id)
{
    (void)system_id;
    (void)nest_id;
    return ESP_ERR_INVALID_STATE;
}

const char *nid_storage_base_dir(void)
{
    return NID_SD_MOUNT_POINT;
}

esp_err_t nid_storage_media_begin(nid_media_kind_t kind, time_t ts, uint32_t seq, nid_media_file_t *out)
{
    (void)kind;
    (void)ts;
    (void)seq;
    memset(out, 0, sizeof(*out));
    return ESP_ERR_INVALID_STATE;
}

esp_err_t nid_storage_media_commit(const nid_media_file_t *f)
{
    (void)f;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t nid_storage_media_abort(const nid_media_file_t *f, bool keep_partial)
{
    (void)f;
    (void)keep_partial;
    return ESP_ERR_INVALID_STATE;
}

int nid_storage_recover(void)
{
    return 0;
}

esp_err_t nid_storage_journal(const char *event, const char *fmt, ...)
{
    char details[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(details, sizeof(details), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "[journal] %s ; %s", event, details);
    return ESP_ERR_INVALID_STATE;
}

void nid_storage_journal_context(int32_t batt_mv, float temp_c)
{
    (void)batt_mv;
    (void)temp_c;
}

esp_err_t nid_storage_write_atomic(const char *path, const void *data, size_t len)
{
    (void)path;
    (void)data;
    (void)len;
    return ESP_ERR_INVALID_STATE;
}
