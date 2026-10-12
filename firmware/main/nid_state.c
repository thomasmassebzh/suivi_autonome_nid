/*
 * nid_state — état conservé entre deux réveils
 */
#include "nid_state.h"

#include <stddef.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "nvs_flash.h"

#define STATE_MAGIC   0x4E494453u  /* « NIDS » */
#define STATE_LAYOUT  1u
#define NVS_NAMESPACE "nid"
#define NVS_KEY       "state"

/* LP SRAM, non initialisée au démarrage : conservée pendant la veille profonde
 * ET après un redémarrage sur défaut (chien de garde, panique), FC3.3.
 * Après une coupure d'alimentation, le CRC est faux : état remis à zéro. */
RTC_NOINIT_ATTR static nid_state_t s_state;

static uint32_t state_crc(const nid_state_t *st)
{
    return esp_rom_crc32_le(0, (const uint8_t *)st, offsetof(nid_state_t, crc));
}

static bool state_valid(const nid_state_t *st)
{
    return st->magic == STATE_MAGIC && st->layout == STATE_LAYOUT && st->crc == state_crc(st);
}

nid_state_t *nid_state(void)
{
    return &s_state;
}

#if CONFIG_NID_POWER_LATCH
static const char *TAG = "nid_state";

static bool nvs_ready(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err == ESP_OK;
}

static bool load_from_nvs(void)
{
    if (!nvs_ready()) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(s_state);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, &s_state, &len);
    nvs_close(h);
    return err == ESP_OK && len == sizeof(s_state) && state_valid(&s_state);
}

static void save_to_nvs(void)
{
    if (!nvs_ready()) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, NVS_KEY, &s_state, sizeof(s_state)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}
#endif

bool nid_state_restore(void)
{
    if (state_valid(&s_state)) {
        return true;
    }
#if CONFIG_NID_POWER_LATCH
    if (load_from_nvs()) {
        ESP_LOGI(TAG, "état restauré depuis la NVS");
        return true;
    }
#endif
    nid_state_reset();
    return false;
}

void nid_state_reset(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.magic = STATE_MAGIC;
    s_state.layout = STATE_LAYOUT;
    nid_config_set_defaults(&s_state.cfg);
    s_state.crc = state_crc(&s_state);
}

void nid_state_commit(void)
{
    s_state.magic = STATE_MAGIC;
    s_state.layout = STATE_LAYOUT;
    s_state.crc = state_crc(&s_state);
#if CONFIG_NID_POWER_LATCH
    save_to_nvs();
#endif
}
