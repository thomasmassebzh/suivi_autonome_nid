/*
 * nid_storage — carte microSD, nommage des fichiers, journal
 *
 * Organisation sur la carte (FP1.6, FP1.7, FP1.8) :
 *   /sdcard/config.ini                                   réglages (nid_config)
 *   /sdcard/<SYS>_<NID>/journal.csv                      journal des événements
 *   /sdcard/<SYS>_<NID>/<AAAAMMJJ>/<SYS>_<NID>_<AAAAMMJJ>_<HHMMSS>_V0042.mp4
 *   /sdcard/<SYS>_<NID>/<AAAAMMJJ>/<SYS>_<NID>_<AAAAMMJJ>_<HHMMSS>_P0123.jpg
 * Date et heure en heure locale (fuseau de config.ini), un dossier par jour.
 *
 * Intégrité (FP1.7, F0 « 0 fichier corrompu ») : chaque média est écrit sous
 * un nom temporaire « .tmp », puis renommé seulement après fermeture + fsync.
 * Au démarrage, un « .tmp » restant (coupure pendant l'écriture) est renommé
 * « ..._INCOMPLET.<ext> » et signalé dans le journal ; les fichiers déjà
 * finalisés ne sont jamais rouverts en écriture.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NID_SD_MOUNT_POINT  "/sdcard"
#define NID_CONFIG_FILE     NID_SD_MOUNT_POINT "/config.ini"
#define NID_PATH_MAX        160

/** Monte la carte microSD (SDMMC, alimentation par le LDO interne, FAT32, noms longs). Idempotent. */
esp_err_t nid_storage_mount(void);

/** Démonte proprement la carte (tous les fichiers doivent être fermés). Idempotent. */
esp_err_t nid_storage_unmount(void);

bool nid_storage_is_mounted(void);

/**
 * Capacité totale et espace libre du volume FAT monté, en octets (calcul 64 bits :
 * grappes × secteurs par grappe × taille de secteur). L'espace libre vient du
 * compteur FSINFO de la carte s'il est vraisemblable (lecture immédiate).
 */
esp_err_t nid_storage_get_space(uint64_t *total_bytes, uint64_t *free_bytes);

/** Description du volume FAT monté (diagnostic, banc d'essai). */
typedef struct {
    uint64_t card_bytes;       /*!< capacité de la carte (registre CSD) */
    uint64_t volume_bytes;     /*!< zone de données du volume FAT monté */
    uint64_t free_bytes;       /*!< espace libre */
    uint32_t cluster_bytes;    /*!< taille d'une grappe (« taille d'unité d'allocation ») */
    uint32_t first_sector;     /*!< premier secteur du volume (0 : carte sans table de partitions) */
    uint8_t  fat_type;         /*!< 12, 16 ou 32 */
} nid_storage_volume_t;

/**
 * Décrit le volume monté. full_scan : recompte les grappes libres dans la FAT
 * (quelques secondes sur une grande carte) au lieu de se fier au compteur
 * FSINFO, et corrige ce compteur sur la carte. Banc d'essai : à n'appeler
 * qu'en l'absence d'écriture en cours.
 */
esp_err_t nid_storage_get_volume(nid_storage_volume_t *out, bool full_scan);

/**
 * Définit les identifiants système/nid (assainis) utilisés dans les noms et
 * crée le dossier de base /sdcard/<SYS>_<NID>. À appeler après le montage.
 */
esp_err_t nid_storage_set_ids(const char *system_id, const char *nest_id);

/** Dossier de base « /sdcard/<SYS>_<NID> ». */
const char *nid_storage_base_dir(void);

typedef enum {
    NID_MEDIA_VIDEO = 0,   /*!< séquence vidéo MP4 (H.264 + AAC) */
    NID_MEDIA_PHOTO,       /*!< photo de suivi JPEG */
} nid_media_kind_t;

typedef struct {
    char final_path[NID_PATH_MAX];  /*!< chemin définitif */
    char tmp_path[NID_PATH_MAX];    /*!< chemin d'écriture (final_path + « .tmp ») */
    char name[64];                  /*!< nom de fichier définitif, sans dossier */
} nid_media_file_t;

/**
 * Prépare l'écriture d'un média : crée le dossier du jour (heure locale de ts)
 * et calcule les chemins. N'ouvre aucun fichier.
 *
 * @param kind  vidéo ou photo
 * @param ts    horodatage (UTC, time_t) du début de la séquence ou de la photo
 * @param seq   numéro de séquence (V0001..., P0001...)
 */
esp_err_t nid_storage_media_begin(nid_media_kind_t kind, time_t ts, uint32_t seq, nid_media_file_t *out);

/**
 * Finalise un média : le fichier tmp_path doit être fermé (et fsync) par
 * l'écrivain. Renomme tmp_path → final_path.
 */
esp_err_t nid_storage_media_commit(const nid_media_file_t *f);

/**
 * Abandonne un média : supprime le fichier temporaire, ou, si keep_partial,
 * le renomme en « ..._INCOMPLET.<ext> ».
 */
esp_err_t nid_storage_media_abort(const nid_media_file_t *f, bool keep_partial);

/**
 * Récupération au démarrage : renomme les « *.tmp » restants en
 * « *_INCOMPLET.<ext> » (dossier de base et dossiers du jour).
 * @return nombre de fichiers traités, ou -1 en cas d'erreur.
 */
int nid_storage_recover(void);

/**
 * Ajoute une ligne au journal CSV (séparateur « ; », UTF-8) :
 *   date_heure_locale;evenement;details;batterie_mV;temperature_C
 * Le fichier est ouvert en ajout, écrit, fsync puis fermé à chaque appel.
 * Sans effet (ESP_ERR_INVALID_STATE) si la carte n'est pas montée.
 */
esp_err_t nid_storage_journal(const char *event, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/** Valeurs des colonnes batterie/température des lignes suivantes (-1 / NAN = inconnues). */
void nid_storage_journal_context(int32_t batt_mv, float temp_c);

/** Écrit un fichier de façon atomique (temporaire + fsync + remplacement). */
esp_err_t nid_storage_write_atomic(const char *path, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
