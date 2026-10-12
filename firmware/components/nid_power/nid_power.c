/*
 * nid_power — énergie : réveil, veille, batterie, chien de garde
 *
 * SQUELETTE : implémentation à compléter (causes de réveil, ADC batterie,
 * veille profonde EXT1 + minuterie, architecture « coupure matérielle »).
 */
#include "nid_power.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "nid_board.h"

static const char *TAG = "nid_power";

void nid_power_get_boot_info(nid_boot_info_t *out)
{
    out->reset_reason = esp_reset_reason();
    out->wake_causes = 0;
    out->ext1_status = 0;
    out->reason = NID_WAKE_POWER_ON;
}

const char *nid_power_wake_str(nid_wake_reason_t reason)
{
    switch (reason) {
    case NID_WAKE_POWER_ON:
        return "mise sous tension";
    case NID_WAKE_PIR:
        return "PIR";
    case NID_WAKE_BUTTON:
        return "bouton";
    case NID_WAKE_TIMER:
        return "minuterie";
    case NID_WAKE_RTC_ALARM:
        return "alarme horloge";
    case NID_WAKE_FAULT:
        return "reprise après défaut";
    default:
        return "?";
    }
}

esp_err_t nid_power_battery_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nid_power_battery_read_mv(uint32_t *mv)
{
    *mv = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

nid_batt_level_t nid_power_battery_level(uint32_t mv, uint32_t low_mv, uint32_t critical_mv)
{
    if (mv == 0) {
        return NID_BATT_UNKNOWN;
    }
    if (mv < critical_mv) {
        return NID_BATT_CRITICAL;
    }
    if (mv < low_mv) {
        return NID_BATT_LOW;
    }
    return NID_BATT_OK;
}

esp_err_t nid_power_watchdog_start(uint32_t timeout_s)
{
    (void)timeout_s;
    return ESP_OK;
}

esp_err_t nid_power_watchdog_add_current_task(void)
{
    return ESP_OK;
}

esp_err_t nid_power_watchdog_remove_current_task(void)
{
    return ESP_OK;
}

void nid_power_watchdog_feed(void)
{
}

void nid_power_sleep(const nid_sleep_plan_t *plan)
{
    ESP_LOGW(TAG, "veille (squelette) : minuterie %llu µs", (unsigned long long)plan->sleep_us);
    nid_board_prepare_deep_sleep();
    if (plan->sleep_us) {
        esp_sleep_enable_timer_wakeup(plan->sleep_us);
    }
    esp_deep_sleep_start();
}

void nid_power_shutdown(uint32_t recheck_s)
{
    nid_sleep_plan_t plan = { .sleep_us = (uint64_t)recheck_s * 1000000ULL };
    nid_power_sleep(&plan);
}

void nid_power_restart(void)
{
    esp_restart();
}
