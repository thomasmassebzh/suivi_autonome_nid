/*
 * nid_power — énergie : réveil, veille, batterie, chien de garde
 *
 * FC3.1/FC3.2 : autonomie ≥ 14 j (cible 17 j) → veille profonde entre deux
 *   événements, réveil par le PIR, le bouton ou la minuterie (photo de suivi,
 *   fin du délai entre déclenchements).
 * FC3.3 : reprise automatique ≤ 5 min après un défaut → chien de garde des
 *   tâches (redémarrage), puis reprise de la surveillance sans intervention.
 * FC3.4 (F0) : arrêt propre sous le seuil critique de tension → l'application
 *   ferme les fichiers, puis nid_power_shutdown() met le système à l'arrêt.
 *
 * Deux architectures d'alimentation (menuconfig « Suivi nid — Application ») :
 *   - veille profonde de l'ESP32-P4 (preuve de concept, carte toujours alimentée) ;
 *   - coupure matérielle (interrupteur Pololu : ON par le PIR ou l'alarme du
 *     DS3231, OFF commandé par l'ESP32-P4 en fin de cycle).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Raison du démarrage, du point de vue de l'application. */
typedef enum {
    NID_WAKE_POWER_ON = 0,  /*!< mise sous tension, bouton RESET, flash (→ mode installation) */
    NID_WAKE_PIR,           /*!< détection PIR (réveil EXT1, ou PIR actif au démarrage en architecture « coupure ») */
    NID_WAKE_BUTTON,        /*!< bouton de mise en service */
    NID_WAKE_TIMER,         /*!< minuterie (photo de suivi, fin de délai, contrôle batterie) */
    NID_WAKE_RTC_ALARM,     /*!< alarme du DS3231 (architecture « coupure ») */
    NID_WAKE_FAULT,         /*!< chien de garde, panique, chute de tension : reprise sans installation */
} nid_wake_reason_t;

typedef struct {
    nid_wake_reason_t  reason;
    esp_reset_reason_t reset_reason;  /*!< esp_reset_reason() */
    uint32_t           wake_causes;   /*!< masque esp_sleep_get_wakeup_causes() */
    uint64_t           ext1_status;   /*!< broches à l'origine d'un réveil EXT1 */
} nid_boot_info_t;

/**
 * Analyse la cause du démarrage (à appeler tôt dans app_main, après
 * nid_detect_init() et, en architecture « coupure », après nid_clock_init()).
 *
 * - Réveil de veille profonde : EXT1 sur le bouton → BUTTON (prioritaire si les
 *   deux broches ont réveillé), EXT1 sur le PIR → PIR, minuterie → TIMER.
 * - esp_restart() (tâche bloquée), panique, chiens de garde, chute ou
 *   perturbation de tension, blocage du processeur, cause inconnue → FAULT.
 * - Mise sous tension, bouton RESET, USB, JTAG → POWER_ON.
 * - Architecture « coupure » (chaque allumage est une mise sous tension) :
 *   bouton appuyé → BUTTON, sinon PIR actif → PIR, sinon drapeau d'alarme du
 *   DS3231 → RTC_ALARM, sinon POWER_ON. Le drapeau est effacé à chaque démarrage
 *   (nid_clock_clear_alarm), ce qui libère la sortie INT du DS3231.
 */
void nid_power_get_boot_info(nid_boot_info_t *out);

/** Libellé français (« mise sous tension », « PIR »...). */
const char *nid_power_wake_str(nid_wake_reason_t reason);

/* ---------- Batterie ---------- */

typedef enum {
    NID_BATT_UNKNOWN = 0,  /*!< pas de mesure (broche non câblée ou erreur) */
    NID_BATT_OK,
    NID_BATT_LOW,          /*!< sous seuil_batterie_basse_mv : plus de vidéos, photos seules */
    NID_BATT_CRITICAL,     /*!< sous seuil_batterie_critique_mv : arrêt propre (FC3.4) */
} nid_batt_level_t;

/**
 * Prépare l'ADC (oneshot, atténuation 12 dB, étalonnage par courbe si les
 * eFuses le permettent). ESP_ERR_NOT_SUPPORTED si aucune broche n'est configurée.
 * L'unité ADC de la broche (ADC1 pour GPIO 16-23) est réservée par ce module.
 */
esp_err_t nid_power_battery_init(void);

/**
 * Tension batterie en mV : moyenne tronquée de CONFIG_NID_BATT_SAMPLES
 * échantillons (< 1 ms), pont diviseur compensé. Utilisable depuis plusieurs
 * tâches à la fois (verrou interne). *mv = 0 en cas d'erreur ;
 * ESP_ERR_INVALID_STATE si nid_power_battery_init() n'a pas réussi.
 */
esp_err_t nid_power_battery_read_mv(uint32_t *mv);

/** Classe une tension par rapport aux seuils (mv = 0 → NID_BATT_UNKNOWN). */
nid_batt_level_t nid_power_battery_level(uint32_t mv, uint32_t low_mv, uint32_t critical_mv);

/* ---------- Chien de garde (FC3.3) ---------- */

/**
 * Configure le chien de garde des tâches (panique → redémarrage) et y abonne la
 * tâche appelante. Les tâches « idle » restent surveillées selon le menuconfig.
 * Une tâche abonnée qui reste bloquée plus de timeout_s sans appeler
 * nid_power_watchdog_feed() provoque un redémarrage (reprise FAULT, FC3.3).
 */
esp_err_t nid_power_watchdog_start(uint32_t timeout_s);

/** Abonne / désabonne la tâche appelante (idempotent). */
esp_err_t nid_power_watchdog_add_current_task(void);
esp_err_t nid_power_watchdog_remove_current_task(void);

/** Nourrit le chien de garde pour la tâche appelante ; sans effet si elle n'est pas abonnée. */
void nid_power_watchdog_feed(void);

/* ---------- Veille ---------- */

typedef struct {
    uint64_t sleep_us;        /*!< réveil par minuterie dans sleep_us µs (0 = pas de minuterie) */
    bool     wake_on_pir;     /*!< réveil par le PIR (EXT1, niveau actif) */
    bool     wake_on_button;  /*!< réveil par le bouton de mise en service */
} nid_sleep_plan_t;

/**
 * Met le système en veille jusqu'au prochain événement. Ne revient pas :
 * le réveil est un redémarrage complet (seule la mémoire RTC est conservée).
 *
 * Avant l'appel, l'application doit avoir fermé ses fichiers et démonté la SD.
 * Si le PIR doit réveiller le système, sa sortie doit être retombée au repos
 * (sinon réveil immédiat) : utiliser nid_detect_wait_pir_idle().
 * Le bouton est attendu relâché (CONFIG_NID_BUTTON_RELEASE_WAIT_MS) ; s'il
 * reste appuyé, il n'est pas utilisé comme source de réveil pour cette veille.
 * Si wake_on_pir est faux, la broche du PIR est isolée : il ne réveille pas.
 *
 * Architecture « coupure matérielle » : programme l'alarme du DS3231 (si
 * sleep_us) puis commande l'OFF de l'interrupteur ; si l'alimentation reste
 * présente (USB au banc), si l'heure n'est pas valide ou si l'alarme ne peut
 * être programmée, repli sur la veille profonde avec le même plan. Le PIR,
 * relié matériellement à l'interrupteur, rallume la carte même si wake_on_pir
 * est faux.
 */
void nid_power_sleep(const nid_sleep_plan_t *plan) __attribute__((noreturn));

/**
 * Arrêt sur batterie critique (FC3.4) : plus aucun réveil par le PIR ni la
 * minuterie de photo ; réveil par le bouton seulement, et contrôle de la
 * tension toutes les recheck_s secondes (0 = jamais) pour reprendre après un
 * rechargement par panneau solaire.
 */
void nid_power_shutdown(uint32_t recheck_s) __attribute__((noreturn));

/**
 * Redémarrage logiciel (après un défaut détecté par l'application) : sorties au
 * repos maintenues pendant le redémarrage ; au démarrage suivant, la cause est
 * NID_WAKE_FAULT (reprise sans installation).
 */
void nid_power_restart(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
