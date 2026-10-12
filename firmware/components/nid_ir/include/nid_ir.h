/*
 * nid_ir — éclairage infrarouge 940 nm, filtre IR-cut, décision jour/nuit
 *
 * FP1.2 (F0) : images exploitables 24 h/24, y compris en obscurité totale.
 * FC1.1 (F0) : aucune lumière visible (λ < 750 nm) → LED 940 nm uniquement,
 *   allumées seulement pendant une prise de vue de nuit, éteintes sinon.
 *
 * Décision jour/nuit (mode « auto ») : lever et coucher du soleil calculés
 * (algorithme NOAA) pour la latitude/longitude de config.ini, avec avance au
 * crépuscule et retard à l'aube pour tenir compte du couvert forestier.
 * Aucun capteur supplémentaire n'est nécessaire et la décision est immédiate
 * au réveil (pas de retard sur le début d'enregistrement).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "nid_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise la PWM des LED IR (éteintes) et la commande du filtre IR-cut. */
esp_err_t nid_ir_init(void);

/** Puissance des LED IR en % (0 = éteintes). */
void nid_ir_set_led_power(uint8_t percent);

/**
 * Filtre IR-cut : day = true → filtre en place (couleurs justes de jour),
 * false → filtre retiré (sensibilité à l'infrarouge la nuit).
 * Sans effet (ESP_ERR_NOT_SUPPORTED) si aucune commande n'est configurée.
 */
esp_err_t nid_ir_set_day_filter(bool day);

/**
 * Heures de lever et coucher du soleil (UTC) du jour civil UTC contenant
 * day_utc. @return ESP_ERR_INVALID_STATE pendant la nuit polaire / le jour polaire.
 */
esp_err_t nid_ir_sun_times(double latitude, double longitude, time_t day_utc,
                           time_t *sunrise_utc, time_t *sunset_utc);

/**
 * true s'il fait nuit à l'instant now_utc selon config (mode auto/jour/nuit,
 * position, avance au crépuscule, retard à l'aube).
 */
bool nid_ir_is_night(const nid_config_t *cfg, time_t now_utc);

/** Applique le mode : nuit → filtre retiré + LED à cfg->ir_power_pct ; jour → filtre en place + LED éteintes. */
void nid_ir_apply(bool night, const nid_config_t *cfg);

/** Éteint les LED IR (et laisse le filtre en position jour). */
void nid_ir_off(void);

#ifdef __cplusplus
}
#endif
