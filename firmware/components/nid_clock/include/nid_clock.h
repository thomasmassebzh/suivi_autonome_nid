/*
 * nid_clock — heure système (horloge interne de l'ESP32-P4)
 *
 * FP1.6 (F0) : date et heure sur chaque fichier et incrustées dans l'image ;
 * dérive ≤ 1 min entre deux relevés. L'heure système est en UTC ; l'heure
 * locale (noms de fichiers, incrustation, journal) découle du fuseau POSIX
 * de config.ini (« AST4 » = Martinique, UTC-4 sans heure d'été).
 *
 * Horloge interne seule (les deux cartes de développement, sans module
 * externe) : le compteur basse consommation de l'ESP32-P4, cadencé par le
 * quartz 32 kHz de la carte, garde l'heure pendant la veille profonde. Sa
 * dérive propre est mesurée à chaque mise à l'heure depuis le téléphone et
 * compensée à chaque réveil (voir nid_clock.c). L'heure n'est perdue qu'en
 * cas de coupure d'alimentation, sauf avec une pile de sauvegarde sur VBAT
 * (cartes Waveshare, CONFIG_ESP_VBAT_INIT_AUTO).
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
    NID_RTC_MODEL_NONE = 0,     /*!< pas d'horloge externe : horloge interne de l'ESP32-P4 */
    NID_RTC_MODEL_DS3231,
    NID_RTC_MODEL_DS1307,
} nid_rtc_model_t;

/** Source de l'horloge lente de l'ESP32-P4 (relevée au démarrage). */
typedef enum {
    NID_CLOCK_SRC_UNKNOWN = 0,
    NID_CLOCK_SRC_XTAL32K,      /*!< quartz 32,768 kHz de la carte : quelques ppm */
    NID_CLOCK_SRC_RC,           /*!< oscillateur RC interne (quartz absent) : dérive forte */
} nid_clock_source_t;

/** État de l'horloge (page d'installation, journal). */
typedef struct {
    nid_clock_source_t source;
    bool     vbat;              /*!< pile de sauvegarde configurée (heure conservée hors tension) */
    int32_t  correction_ppb;    /*!< dérive mesurée et compensée (ppb, > 0 : la carte avance) */
    uint32_t measurements;      /*!< nombre de mesures de dérive retenues */
    int64_t  last_set_utc;      /*!< dernière mise à l'heure depuis le téléphone (0 : jamais) */
    uint32_t max_error_s;       /*!< écart maximal estimé depuis (0 : inconnu) */
} nid_clock_info_t;

/**
 * Initialise l'horloge : source de l'horloge lente, correction de dérive
 * enregistrée (NVS). Le bus I2C est réservé à un futur pilote DS3231.
 */
esp_err_t nid_clock_init(i2c_master_bus_handle_t bus);

nid_rtc_model_t nid_clock_model(void);

/** « interne (quartz 32 kHz) » ou « interne (oscillateur RC) ». */
const char *nid_clock_model_str(void);

/**
 * À chaque réveil : compense la dérive mesurée depuis la compensation
 * précédente (settimeofday si l'écart atteint 1 ms).
 * @param[out] rtc_valid  false si l'heure est perdue (date antérieure à 2025).
 */
esp_err_t nid_clock_sync_from_rtc(bool *rtc_valid);

/**
 * Règle l'heure système (UTC, en ms : heure du téléphone). Si la précédente
 * mise à l'heure date d'au moins 24 h et que l'heure n'a pas été perdue entre
 * les deux, l'écart constaté met à jour la correction de dérive.
 */
esp_err_t nid_clock_set_time_ms(int64_t utc_ms);

/** Comme nid_clock_set_time_ms(), à la seconde. */
esp_err_t nid_clock_set_time(time_t utc);

/** État de l'horloge. */
void nid_clock_get_info(nid_clock_info_t *out);

/** true si l'heure système est plausible (année ≥ 2025). */
bool nid_clock_time_is_valid(void);

/** Applique le fuseau horaire POSIX (setenv TZ + tzset). */
void nid_clock_set_timezone(const char *posix_tz);

/** Formate un instant en heure locale (strftime). */
size_t nid_clock_format_local(time_t t, const char *fmt, char *buf, size_t len);

/**
 * Sans DS3231 (cas actuel), les trois fonctions suivantes retournent
 * ESP_ERR_NOT_SUPPORTED.
 *
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
