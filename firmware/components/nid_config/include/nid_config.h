/*
 * nid_config — réglages du système (FS1.4 : paramétrage sans reprogrammation)
 *
 * Les réglages sont lus dans le fichier texte /sdcard/config.ini, éditable sur
 * un ordinateur ou depuis la page d'installation (smartphone). Le fichier est
 * créé avec les valeurs par défaut s'il est absent.
 *
 * Format INI : sections [systeme], [video], [photo], [detection], [nuit],
 * [energie], [installation] ; lignes « cle = valeur » ; commentaires « # » ou
 * « ; » ; booléens « oui/non » (ou 1/0, true/false). Les clés sont uniques :
 * la section sert seulement à la lisibilité. Clé inconnue ou valeur hors plage :
 * avertissement dans le journal, valeur bornée ou défaut conservé.
 *
 * Exemple :
 *   [systeme]
 *   id_systeme = CAM01
 *   id_nid = NID01
 *   fuseau_horaire = AST4
 *   [video]
 *   duree_sequence_s = 30
 *   delai_entre_declenchements_s = 60
 *   resolution = 1920x1080
 *   images_par_seconde = 15
 *   debit_kbps = 2500
 *   son = oui
 *   [photo]
 *   intervalle_suivi_min = 15
 *   qualite_jpeg = 80
 *   [detection]
 *   sensibilite = 3
 *   chauffe_pir_s = 60
 *   [nuit]
 *   mode = auto
 *   latitude = 14.77
 *   longitude = -60.89
 *   avance_crepuscule_min = 30
 *   retard_aube_min = 30
 *   puissance_ir = 100
 *   nuit_monochrome = oui
 *   [energie]
 *   seuil_batterie_basse_mv = 3450
 *   seuil_batterie_critique_mv = 3350
 *   [installation]
 *   wifi_ssid =
 *   wifi_mot_de_passe = moqueur-nid
 *   duree_max_min = 15
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NID_CONFIG_ID_LEN    16   /*!< id_systeme / id_nid : 1 à 15 caractères [A-Za-z0-9_-] */
#define NID_CONFIG_TZ_LEN    48   /*!< Chaîne de fuseau horaire POSIX */
#define NID_CONFIG_SSID_LEN  33   /*!< SSID Wi-Fi : 0 à 32 caractères */
#define NID_CONFIG_PASS_LEN  65   /*!< Mot de passe WPA2 : 8 à 63 caractères */

/** Mode jour/nuit (FP1.2). */
typedef enum {
    NID_NIGHT_MODE_AUTO = 0,  /*!< « auto » : lever/coucher du soleil calculés + décalages */
    NID_NIGHT_MODE_DAY,       /*!< « jour » : toujours jour (filtre IR en place, LED IR éteintes) */
    NID_NIGHT_MODE_NIGHT,     /*!< « nuit » : toujours nuit (filtre IR retiré, LED IR allumées) */
} nid_night_mode_t;

/** Ensemble des réglages. Nom de clé INI entre parenthèses ; [défaut ; plage]. */
typedef struct {
    /* [systeme] */
    char     system_id[NID_CONFIG_ID_LEN];      /*!< (id_systeme) [« CAM01 »] identifiant du système (FP1.6) */
    char     nest_id[NID_CONFIG_ID_LEN];        /*!< (id_nid) [« NID01 »] identifiant du nid (FP1.6) */
    char     timezone[NID_CONFIG_TZ_LEN];       /*!< (fuseau_horaire) [« AST4 » = Martinique, UTC-4] TZ POSIX */
    /* [video] */
    uint16_t seq_duration_s;                    /*!< (duree_sequence_s) [30 ; 1..300] (FP1.4) */
    uint16_t trigger_delay_s;                   /*!< (delai_entre_declenchements_s) [60 ; 0..3600], 0 = désactivé */
    uint16_t video_width;                       /*!< (resolution) [1920x1080 ; 1920x1080 | 1280x960 | 1280x720] */
    uint16_t video_height;
    uint8_t  video_fps;                         /*!< (images_par_seconde) [15 ; 10..30] (FP1.2 ≥ 15) */
    uint16_t video_bitrate_kbps;                /*!< (debit_kbps) [2500 ; 500..10000] */
    bool     audio_enabled;                     /*!< (son) [oui] piste son AAC (FP1.3) */
    /* [photo] */
    uint16_t photo_interval_min;                /*!< (intervalle_suivi_min) [15 ; 0..1440], 0 = désactivé (FP1.5) */
    uint8_t  jpeg_quality;                      /*!< (qualite_jpeg) [80 ; 10..100] */
    /* [detection] */
    uint8_t  pir_sensitivity;                   /*!< (sensibilite) [3 ; 1..5] 5 = la plus sensible (FP1.1) */
    uint16_t pir_warmup_s;                      /*!< (chauffe_pir_s) [60 ; 0..120] stabilisation du PIR après mise sous tension */
    /* [nuit] */
    nid_night_mode_t night_mode;                /*!< (mode) [auto ; auto | jour | nuit] */
    float    latitude;                          /*!< (latitude) [14.77 ; -90..90] degrés, nord positif */
    float    longitude;                         /*!< (longitude) [-60.89 ; -180..180] degrés, est positif */
    int16_t  dusk_advance_min;                  /*!< (avance_crepuscule_min) [30 ; -180..240] nuit commence N min avant le coucher */
    int16_t  dawn_delay_min;                    /*!< (retard_aube_min) [30 ; -180..240] nuit finit N min après le lever */
    uint8_t  ir_power_pct;                      /*!< (puissance_ir) [100 ; 0..100] puissance des LED IR 940 nm */
    bool     night_monochrome;                  /*!< (nuit_monochrome) [oui] image en niveaux de gris la nuit */
    /* [energie] */
    uint16_t batt_low_mv;                       /*!< (seuil_batterie_basse_mv) [3450 ; 2800..4500] plus de vidéo, photos seules */
    uint16_t batt_critical_mv;                  /*!< (seuil_batterie_critique_mv) [3350 ; 2800..4500] arrêt propre (FC3.4) */
    /* [installation] */
    char     wifi_ssid[NID_CONFIG_SSID_LEN];    /*!< (wifi_ssid) [vide = « NID-<id_systeme> »] */
    char     wifi_password[NID_CONFIG_PASS_LEN];/*!< (wifi_mot_de_passe) [« moqueur-nid »] */
    uint16_t install_timeout_min;               /*!< (duree_max_min) [15 ; 1..60] fin automatique du mode installation */
} nid_config_t;

/** Remplit la structure avec les valeurs par défaut (celles du CdCF). */
void nid_config_set_defaults(nid_config_t *cfg);

/**
 * Lit un fichier INI. La structure doit être pré-remplie (défauts ou valeurs
 * courantes) : seules les clés présentes sont modifiées, puis la validation
 * (nid_config_validate) est appliquée.
 *
 * @return ESP_OK ; ESP_ERR_NOT_FOUND si le fichier n'existe pas (cfg inchangé) ;
 *         ESP_FAIL si lecture impossible.
 */
esp_err_t nid_config_load(nid_config_t *cfg, const char *path);

/**
 * Écrit le fichier INI complet, commenté en français (plages et effets de
 * chaque clé), de façon atomique : fichier temporaire, fsync, puis remplacement.
 */
esp_err_t nid_config_save(const nid_config_t *cfg, const char *path);

/**
 * Applique une paire clé/valeur (clé INI, ex. « duree_sequence_s »).
 * Utilisé par le lecteur INI et par le formulaire de la page d'installation.
 *
 * @return ESP_OK ; ESP_ERR_NOT_FOUND clé inconnue ; ESP_ERR_INVALID_ARG valeur invalide (cfg inchangé).
 */
esp_err_t nid_config_set(nid_config_t *cfg, const char *key, const char *value);

/**
 * Borne les valeurs hors plage, assainit les identifiants, vérifie la
 * cohérence (seuil critique < seuil bas, résolution supportée...).
 *
 * @return ESP_OK si rien n'a été corrigé ; ESP_ERR_INVALID_ARG si au moins une
 *         valeur a été corrigée (détails dans les logs).
 */
esp_err_t nid_config_validate(nid_config_t *cfg);

/** Sérialise en JSON (objet à plat, clés INI). @return longueur écrite (hors '\0'), 0 si trop petit. */
size_t nid_config_to_json(const nid_config_t *cfg, char *buf, size_t len);

/** Sérialise au format INI commenté. @return longueur écrite (hors '\0'), 0 si trop petit. */
size_t nid_config_to_ini(const nid_config_t *cfg, char *buf, size_t len);

/** « auto » / « jour » / « nuit ». */
const char *nid_config_night_mode_str(nid_night_mode_t mode);

/** SSID effectif : wifi_ssid, ou « NID-<id_systeme> » s'il est vide. */
void nid_config_effective_ssid(const nid_config_t *cfg, char *out, size_t len);

#ifdef __cplusplus
}
#endif
