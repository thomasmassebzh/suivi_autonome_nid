/*
 * nid_capture — caméra MIPI-CSI, vidéo H.264 + son AAC en MP4, photo JPEG
 *
 * Exigences couvertes :
 *   FP1.2 : ≥ 720p, ≥ 15 img/s, couleur de jour, monochrome accepté la nuit.
 *   FP1.3 : piste son AAC (micro + codec ES8311 de la carte), désactivable.
 *   FP1.4 : enregistrement dès la détection (≤ 1 s), durée réglable 1 s – 5 min.
 *   FP1.5 : photo JPEG de suivi.
 *   FP1.6 (F0) : date, heure et identifiants incrustés dans chaque image.
 *   FP1.7 (F0) : le fichier est fermé et synchronisé avant le retour ; arrêt
 *                anticipé propre sur demande (batterie critique).
 *   FP1.8 (F0) : MP4 (H.264 + AAC) et JPEG lisibles sans logiciel propriétaire.
 *   FS1.2 : flux d'aperçu JPEG pour le cadrage depuis le smartphone.
 *
 * Chaîne : capteur (OV5647 ou SC2336, détection automatique) → MIPI-CSI → ISP
 * (exposition et balance des blancs automatiques) → incrustation → encodeur
 * H.264 matériel → multiplexeur MP4 (esp_muxer) ; audio I2S → AAC (esp_audio_codec).
 * Photo et aperçu : encodeur JPEG matériel.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Réglages de capture (issus de nid_config_t). */
typedef struct {
    uint16_t width;            /*!< 1920x1080, 1280x960 ou 1280x720 */
    uint16_t height;
    uint8_t  fps;              /*!< images par seconde (10..30) */
    uint32_t bitrate_kbps;     /*!< débit vidéo cible */
    bool     audio;            /*!< piste son AAC */
    bool     night;            /*!< prise de vue de nuit (éclairage IR) */
    bool     night_mono;       /*!< la nuit : image en niveaux de gris */
    uint8_t  jpeg_quality;     /*!< qualité des photos (10..100) */
    char     label[40];        /*!< texte incrusté après la date et l'heure, ex. « CAM01 NID01 » */
} nid_capture_settings_t;

/** Bilan d'un enregistrement (journal, mesures du CdCF). */
typedef struct {
    uint32_t video_frames;     /*!< images encodées */
    uint32_t audio_frames;     /*!< trames AAC */
    uint32_t dropped_frames;   /*!< images perdues (file pleine, carte trop lente) */
    uint64_t bytes;            /*!< taille du fichier */
    uint32_t duration_ms;      /*!< durée effective */
    uint32_t first_frame_ms;   /*!< instant de la 1re image encodée, en ms depuis le démarrage (esp_timer) */
} nid_capture_stats_t;

/**
 * Rappel d'arrêt anticipé, appelé par la tâche d'enregistrement environ toutes
 * les 200 ms. Retourner true arrête proprement la séquence (fichier fermé).
 * L'application s'en sert aussi pour nourrir le chien de garde.
 */
typedef bool (*nid_capture_stop_cb_t)(void *arg);

/**
 * Initialise la caméra, l'ISP, les encodeurs et (si s->audio) le micro.
 * À appeler le plus tôt possible après le réveil : c'est l'étape la plus
 * longue avant la première image. Nécessite nid_board_i2c_init().
 *
 * Idempotent : si la capture est déjà initialisée avec des réglages
 * identiques, retour immédiat ; sinon les éléments concernés sont
 * réinitialisés (mode banc : la caméra reste prête entre deux séquences).
 */
esp_err_t nid_capture_init(const nid_capture_settings_t *s);

/** true si nid_capture_init() a réussi et que nid_capture_deinit() n'a pas été appelé. */
bool nid_capture_is_ready(void);

/** Nom du capteur détecté (« OV5647 », « SC2336 »...) ou « aucun ». */
const char *nid_capture_sensor_name(void);

/** Bascule jour/nuit à chaud (monochrome selon night_mono). */
esp_err_t nid_capture_set_night(bool night);

/**
 * Enregistre une séquence MP4 (bloquant). Le fichier `path` est créé, écrit,
 * fermé et synchronisé (fsync) avant le retour.
 *
 * @param path         chemin du fichier à écrire (nom temporaire fourni par nid_storage)
 * @param duration_ms  durée demandée
 * @param stop_cb      rappel d'arrêt anticipé (peut être NULL)
 * @param arg          argument du rappel
 * @param[out] stats   bilan (peut être NULL)
 * @return ESP_OK si le fichier est complet et lisible (y compris après arrêt anticipé).
 */
esp_err_t nid_capture_record(const char *path, uint32_t duration_ms,
                             nid_capture_stop_cb_t stop_cb, void *arg,
                             nid_capture_stats_t *stats);

/** Prend une photo JPEG avec incrustation, écrite et synchronisée dans `path` (bloquant). */
esp_err_t nid_capture_photo(const char *path);

/**
 * Démarre le flux d'aperçu JPEG (mode installation) : images réduites à
 * max_width pixels de large au plus, qualité quality.
 */
esp_err_t nid_capture_preview_start(uint8_t quality, uint16_t max_width);

/**
 * Récupère la dernière image d'aperçu. Le tampon reste valide jusqu'à
 * nid_capture_preview_release(). Un seul lecteur à la fois.
 */
esp_err_t nid_capture_preview_frame(const uint8_t **jpeg, size_t *len, uint32_t timeout_ms);

void nid_capture_preview_release(void);

esp_err_t nid_capture_preview_stop(void);

/**
 * Image remise au crochet de débogage : YUV 4:2:0 « Espressif » (O_UYY_E_VYY,
 * format de l'ISP, voir nid_overlay.h), incrustation date/heure comprise.
 */
typedef struct {
    const uint8_t *data;       /*!< image, en lecture seule, valide pendant l'appel seulement */
    size_t   len;              /*!< octets utiles */
    uint16_t width;
    uint16_t height;
    bool     full_range;       /*!< plage YUV pleine 0-255 (photo, aperçu), sinon limitée 16-235 (vidéo) */
    bool     recording;        /*!< image d'un enregistrement en cours */
} nid_capture_frame_t;

typedef void (*nid_capture_frame_hook_t)(const nid_capture_frame_t *frame, void *arg);

/**
 * Crochet de débogage, pour le banc d'essai (écran local) ; le firmware de
 * terrain ne l'utilise pas. Appelé avec chaque image capturée, une fois
 * incrustée et encodée, par nid_capture_record(), nid_capture_photo() et
 * nid_capture_preview_frame(), dans la tâche appelante et sous le verrou du
 * module : le crochet ne doit appeler aucune fonction de nid_capture.
 *
 * Pendant un enregistrement, le temps passé dans le crochet retarde la lecture
 * de l'image suivante (trois tampons caméra) : il doit rester bref (quelques ms)
 * et sauter lui-même des images (quelques i/s suffisent à un écran de contrôle).
 * NULL (par défaut) : désactivé, sans coût. Un appel déjà commencé peut se
 * terminer après le retour de nid_capture_set_frame_hook(NULL, NULL).
 */
void nid_capture_set_frame_hook(nid_capture_frame_hook_t hook, void *arg);

/** Arrête la caméra (capteur en veille si possible) et libère la mémoire. */
esp_err_t nid_capture_deinit(void);

#ifdef __cplusplus
}
#endif
