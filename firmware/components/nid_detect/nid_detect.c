/*
 * nid_detect — détecteur PIR et bouton de mise en service
 *
 * SQUELETTE : implémentation à compléter (entrées, validation, interruptions).
 */
#include "nid_detect.h"

#include "nid_board.h"

esp_err_t nid_detect_init(void)
{
    return ESP_OK;
}

bool nid_detect_pir_active(void)
{
    return false;
}

bool nid_detect_button_pressed(void)
{
    return false;
}

uint32_t nid_detect_min_active_ms(uint8_t sensitivity)
{
    switch (sensitivity) {
    case 5:
        return 0;
    case 4:
        return 50;
    case 3:
        return 150;
    case 2:
        return 300;
    default:
        return 500;
    }
}

bool nid_detect_pir_confirm(uint32_t min_active_ms, uint32_t timeout_ms)
{
    (void)timeout_ms;
    return min_active_ms == 0;
}

esp_err_t nid_detect_wait_pir_idle(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return ESP_OK;
}

esp_err_t nid_detect_enable_events(QueueHandle_t queue)
{
    (void)queue;
    return ESP_ERR_NOT_SUPPORTED;
}

void nid_detect_disable_events(void)
{
}
