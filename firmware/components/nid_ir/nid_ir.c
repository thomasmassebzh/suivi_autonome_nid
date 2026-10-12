/*
 * nid_ir — éclairage infrarouge 940 nm, filtre IR-cut, décision jour/nuit
 *
 * SQUELETTE : implémentation à compléter (PWM LEDC, IR-cut, calcul solaire).
 */
#include "nid_ir.h"

esp_err_t nid_ir_init(void)
{
    return ESP_OK;
}

void nid_ir_set_led_power(uint8_t percent)
{
    (void)percent;
}

esp_err_t nid_ir_set_day_filter(bool day)
{
    (void)day;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nid_ir_sun_times(double latitude, double longitude, time_t day_utc,
                           time_t *sunrise_utc, time_t *sunset_utc)
{
    (void)latitude;
    (void)longitude;
    *sunrise_utc = day_utc;
    *sunset_utc = day_utc;
    return ESP_ERR_NOT_SUPPORTED;
}

bool nid_ir_is_night(const nid_config_t *cfg, time_t now_utc)
{
    (void)now_utc;
    return cfg->night_mode == NID_NIGHT_MODE_NIGHT;
}

void nid_ir_apply(bool night, const nid_config_t *cfg)
{
    (void)night;
    (void)cfg;
}

void nid_ir_off(void)
{
}
