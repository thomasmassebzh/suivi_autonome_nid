/*
 * nid_clock — horloge temps réel (DS3231 / DS1307) et heure système
 *
 * FP1.6 (F0) : date et heure sur chaque fichier et incrustées dans l'image ;
 * dérive ≤ 1 min entre deux relevés. L'horloge externe est en UTC ; l'heure
 * locale (noms de fichiers, incrustation, journal) découle du fuseau POSIX
 * de config.ini (« AST4 » = Martinique, UTC-4 sans heure d'été).
 *
 * Au réveil, l'heure système (maintenue par l'ESP32-P4 pendant la veille) est
 * resynchronisée sur le DS3231 pour ne pas cumuler la dérive interne.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NID_RTC_MODEL_NONE = 0,
    NID_RTC_MODEL_DS3231,
    NID_RTC_MODEL_DS1307,
} nid_rtc_model_t;

/** Détecte et initialise l'horloge externe sur le bus I2C partagé. */
esp_err_t nid_clock_init(i2c_master_bus_handle_t bus);

nid_rtc_model_t nid_clock_model(void);

/** « DS3231 », « DS1307 » ou « aucune ». */
const char *nid_clock_model_str(void);

/**
 * Recopie l'heure de l'horloge externe dans l'heure système (settimeofday).
 * @param[out] rtc_valid  false si l'horloge a perdu l'heure (oscillateur arrêté,
 *                        pile vide) ou indique une date antérieure à 2025.
 */
esp_err_t nid_clock_sync_from_rtc(bool *rtc_valid);

/** Règle l'heure système ET l'horloge externe (UTC). */
esp_err_t nid_clock_set_time(time_t utc);

/** true si l'heure système est plausible (année ≥ 2025). */
bool nid_clock_time_is_valid(void);

/** Applique le fuseau horaire POSIX (setenv TZ + tzset). */
void nid_clock_set_timezone(const char *posix_tz);

/** Formate un instant en heure locale (strftime). */
size_t nid_clock_format_local(time_t t, const char *fmt, char *buf, size_t len);

/**
 * Programme l'alarme 1 du DS3231 (sortie INT/SQW active basse) à l'instant
 * donné. Architecture « coupure matérielle » : l'alarme rallume la carte.
 * @return ESP_ERR_NOT_SUPPORTED sans DS3231.
 */
esp_err_t nid_clock_set_alarm(time_t utc);

/** Lit puis efface le drapeau d'alarme du DS3231 (et désactive l'alarme). */
esp_err_t nid_clock_clear_alarm(bool *fired);

/** Température interne du DS3231 (°C, résolution 0,25). */
esp_err_t nid_clock_get_temperature(float *celsius);

#ifdef __cplusplus
}
#endif
