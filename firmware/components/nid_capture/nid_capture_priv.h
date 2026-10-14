/*
 * nid_capture — interfaces internes au composant
 *
 * Piste son : micro de la carte via le codec ES8311 (esp_codec_dev), encodage
 * AAC (esp_audio_codec), voir nid_capture_audio.c. Les trames AAC sont remises
 * au multiplexeur MP4 par un rappel, depuis la tâche audio.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "nid_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Rappel de remise d'une trame AAC brute (sans en-tête ADTS), appelé depuis la tâche audio. */
typedef void (*nid_capture_audio_sink_t)(const uint8_t *data, uint32_t len, uint32_t pts_ms, void *ctx);

/** Caractéristiques de la piste son, pour la déclaration du flux MP4. */
typedef struct {
    uint32_t       sample_rate;      /*!< fréquence d'échantillonnage (Hz) */
    uint8_t        channels;         /*!< nombre de voies */
    uint8_t        bits_per_sample;  /*!< bits par échantillon */
    uint32_t       frame_ms;         /*!< durée d'une trame AAC (ms, arrondie) */
    const uint8_t *spec_info;        /*!< AudioSpecificConfig (calculée, 2 octets), valide jusqu'à nid_capture_audio_stop() */
    uint32_t       spec_info_len;
} nid_capture_audio_info_t;

/** Prépare le canal I2S (réception seule) et le codec ES8311 en mode ADC. Idempotent. */
esp_err_t nid_capture_audio_init(void);

/** true si nid_capture_audio_init() a réussi. */
bool nid_capture_audio_ready(void);

/**
 * Ouvre l'encodeur AAC et le micro, puis démarre la tâche de lecture. Les
 * trames ne sont remises au rappel qu'après nid_capture_audio_deliver().
 */
esp_err_t nid_capture_audio_start(nid_capture_audio_sink_t sink, void *ctx, nid_capture_audio_info_t *info);

/**
 * Autorise la remise des trames. t0_us (esp_timer) est l'instant de la
 * première image vidéo : les horodatages audio sont relatifs à cet instant.
 */
void nid_capture_audio_deliver(int64_t t0_us);

/** Arrête la tâche, ferme le micro et l'encodeur. @return nombre de trames AAC remises. */
uint32_t nid_capture_audio_stop(void);

/**
 * Vumètre (banc d'essai) : ouvre le micro, mesure le niveau pendant
 * duration_ms, le referme. ESP_ERR_INVALID_STATE pendant une séquence.
 */
esp_err_t nid_capture_audio_measure(uint32_t duration_ms, nid_capture_audio_level_t *out);

/** Niveau du dernier bloc lu (séquence ou mesure) ; false si aucun depuis 1 s. */
bool nid_capture_audio_get_last_level(nid_capture_audio_level_t *out);

/** Libère le codec et le canal I2S (l'ampli du haut-parleur n'est jamais piloté). */
void nid_capture_audio_deinit(void);

#ifdef __cplusplus
}
#endif
