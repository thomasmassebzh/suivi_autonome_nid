/*
 * nid_install — mode installation : Wi-Fi local, aperçu et réglages sur smartphone
 *
 * SQUELETTE : implémentation à compléter (C6 + point d'accès, serveur web,
 * aperçu MJPEG, réglages, mise à l'heure).
 */
#include "nid_install.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nid_board.h"
#include "nid_detect.h"

static const char *TAG = "nid_install";

nid_install_result_t nid_install_run(nid_install_ctx_t *ctx)
{
    ESP_LOGW(TAG, "mode installation non implémenté : appui bouton ou %lu s", (unsigned long)ctx->timeout_s);
    ctx->config_changed = false;
    ctx->time_changed = false;
    nid_board_status_led(true);
    for (uint32_t t = 0; t < ctx->timeout_s * 10; t++) {
        if (nid_detect_button_pressed()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    nid_board_status_led(false);
    return NID_INSTALL_EXIT_NO_WIFI;
}
