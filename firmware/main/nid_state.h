/*
 * nid_state — état conservé entre deux réveils
 *
 * En veille profonde, seule la mémoire RTC (LP SRAM) de l'ESP32-P4 est
 * conservée : l'état y est stocké avec un CRC. Il contient aussi une copie
 * des réglages, ce qui évite de relire config.ini à chaque réveil (délai
 * détection → enregistrement, FP1.4).
 *
 * Architecture « coupure matérielle » : la mémoire RTC est perdue à chaque
 * coupure ; l'état est alors aussi sauvegardé en NVS (flash).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "nid_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t     magic;
    uint32_t     layout;              /*!< version de la structure */
    uint32_t     boot_count;
    uint32_t     video_seq;           /*!< dernier numéro de séquence vidéo utilisé */
    uint32_t     photo_seq;           /*!< dernier numéro de photo utilisé */
    int64_t      next_photo_utc;      /*!< prochaine photo de suivi (0 = aucune) */
    int64_t      cooldown_until_utc;  /*!< fin du délai entre deux déclenchements */
    int64_t      pir_armed_utc;       /*!< fin de la chauffe du PIR */
    int64_t      monitoring_since_utc;/*!< début de la surveillance */
    uint32_t     last_batt_mv;        /*!< dernière tension mesurée (0 = inconnue) */
    uint8_t      monitoring;          /*!< 1 = surveillance active (installation terminée) */
    uint8_t      batt_level;          /*!< nid_batt_level_t de la dernière mesure */
    uint8_t      maintenance;         /*!< 1 = pause maintenance (aucun enregistrement) */
    uint8_t      cfg_loaded;          /*!< 1 = réglages lus sur la carte SD */
    nid_config_t cfg;                 /*!< copie des réglages */
    uint32_t     crc;
} nid_state_t;

/** État courant (mémoire RTC). */
nid_state_t *nid_state(void);

/**
 * Restaure l'état depuis la mémoire RTC (CRC), sinon depuis la NVS
 * (architecture « coupure »). @return false si aucun état valide (état remis à zéro).
 */
bool nid_state_restore(void);

/** Remet l'état à zéro (réglages par défaut, surveillance inactive). */
void nid_state_reset(void);

/** Recalcule le CRC ; en architecture « coupure », sauvegarde aussi en NVS. */
void nid_state_commit(void);

#ifdef __cplusplus
}
#endif
