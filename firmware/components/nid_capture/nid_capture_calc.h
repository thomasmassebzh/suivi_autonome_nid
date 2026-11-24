/*
 * nid_capture — calculs purs (sans matériel), testés sur PC (test_host/)
 *
 *  - AudioSpecificConfig AAC (ISO/IEC 14496-3, § 1.6.2.1) ;
 *  - en-tête ADTS d'une trame AAC (ISO/IEC 13818-7, § 6.2) : repérage et
 *    construction. Les trames sont remises au multiplexeur MP4 avec leur
 *    en-tête ADTS, comme le fait esp_capture d'Espressif ;
 *  - créneaux d'horodatage des images vidéo (images perdues, PTS).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NID_AAC_OBJECT_LC       2u      /* Audio Object Type « AAC LC » */
#define NID_AAC_ASC_LEN         2u

/**
 * Index de fréquence d'échantillonnage AAC (samplingFrequencyIndex), ou -1 si
 * la fréquence n'est pas dans la table normalisée.
 */
static inline int nid_aac_freq_index(uint32_t sample_rate)
{
    static const uint32_t table[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
    };
    for (int i = 0; i < (int)(sizeof(table) / sizeof(table[0])); i++) {
        if (table[i] == sample_rate) {
            return i;
        }
    }
    return -1;
}

/**
 * AudioSpecificConfig AAC-LC sur 2 octets :
 *   audioObjectType (5 bits) | samplingFrequencyIndex (4) | channelConfiguration (4)
 *   | frameLengthFlag = 0 (1024 échantillons) | dependsOnCoreCoder = 0 | extensionFlag = 0.
 * Exemple : 16 kHz mono → 0x14 0x08 ; 44,1 kHz stéréo → 0x12 0x10.
 * @return false si la fréquence est hors table ou le nombre de voies hors 1..7.
 */
static inline bool nid_aac_asc_lc(uint32_t sample_rate, uint8_t channels, uint8_t out[NID_AAC_ASC_LEN])
{
    const int fi = nid_aac_freq_index(sample_rate);
    if (fi < 0 || channels < 1 || channels > 7 || out == NULL) {
        return false;
    }
    const uint16_t v = (uint16_t)((NID_AAC_OBJECT_LC << 11) | ((uint16_t)fi << 7) | ((uint16_t)channels << 3));
    out[0] = (uint8_t)(v >> 8);
    out[1] = (uint8_t)(v & 0xFF);
    return true;
}

/**
 * Longueur de l'en-tête ADTS en tête de trame (7 octets, 9 avec CRC), ou 0 si
 * la trame ne commence pas par un en-tête ADTS (mot de synchro 0xFFF, couche 0).
 */
static inline size_t nid_aac_adts_header_len(const uint8_t *d, size_t len)
{
    if (d == NULL || len < 7 || d[0] != 0xFF || (d[1] & 0xF6) != 0xF0) {
        return 0;
    }
    const size_t hdr = (d[1] & 0x01) ? 7u : 9u;     /* protection_absent = 1 : pas de CRC */
    const size_t frame_len = ((size_t)(d[3] & 0x03) << 11) | ((size_t)d[4] << 3) | ((size_t)d[5] >> 5);
    if (frame_len < hdr || frame_len > len) {
        return 0;   /* incohérent : pas un en-tête ADTS */
    }
    return hdr;
}

#define NID_AAC_ADTS_LEN        7u      /* en-tête ADTS sans CRC */

/**
 * Construit un en-tête ADTS de 7 octets (MPEG-4, AAC-LC, sans CRC) pour une
 * trame brute de payload_len octets. Le champ frame_length inclut l'en-tête.
 * @return false si la fréquence est hors table, le nombre de voies hors 1..7
 *         ou la trame trop longue (13 bits).
 */
static inline bool nid_aac_adts_build(uint32_t sample_rate, uint8_t channels, size_t payload_len,
                                      uint8_t out[NID_AAC_ADTS_LEN])
{
    const int fi = nid_aac_freq_index(sample_rate);
    const size_t frame_len = payload_len + NID_AAC_ADTS_LEN;
    if (fi < 0 || channels < 1 || channels > 7 || frame_len > 0x1FFF || out == NULL) {
        return false;
    }
    const uint8_t profile = (uint8_t)(NID_AAC_OBJECT_LC - 1u);    /* 1 = LC */
    out[0] = 0xFF;                                                  /* synchro 0xFFF */
    out[1] = 0xF1;                                                  /* MPEG-4, couche 0, sans CRC */
    out[2] = (uint8_t)((profile << 6) | ((uint8_t)fi << 2) | (channels >> 2));
    out[3] = (uint8_t)(((channels & 0x3u) << 6) | ((frame_len >> 11) & 0x3u));
    out[4] = (uint8_t)((frame_len >> 3) & 0xFFu);
    out[5] = (uint8_t)(((frame_len & 0x7u) << 5) | 0x1Fu);          /* remplissage du tampon : 0x7FF (VBR) */
    out[6] = 0xFC;                                                  /* une seule trame brute */
    return true;
}

/**
 * Horodatage (ms) de la trame AAC n° index, relatif à la première trame :
 * index × échantillons par trame / fréquence, arrondi à la ms inférieure.
 * Calculé sur le nombre de trames et non sur l'horloge : jamais de trou ni
 * de doublon, même si la tâche son a été retardée.
 */
static inline uint32_t nid_aac_frame_pts_ms(uint32_t index, uint32_t samples_per_frame, uint32_t sample_rate)
{
    if (sample_rate == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)index * samples_per_frame * 1000u) / sample_rate);
}

/** Pas de borne de libération connue (voir nid_capture_next_slot). */
#define NID_SLOT_NO_RELEASE     INT64_MIN

/**
 * Créneau d'horodatage de l'image suivante sur la grille des images (période
 * frame_us), le créneau 0 étant la première image enregistrée.
 *
 * L'instant de sortie de DQBUF n'est l'instant d'arrivée de l'image que si
 * DQBUF a réellement attendu (file vide : l'image vient d'arriver, elle est
 * « fraîche ») : son créneau est alors recalculé sur cet instant. Une image
 * déjà en attente dans la file (retard pris pendant une écriture sur la carte
 * SD) a été capturée plus tôt : son créneau est le suivant du précédent.
 * Recalculer le créneau sur l'heure de sortie de DQBUF pour ces images en
 * retard comptait de fausses images perdues et décalait la vidéo
 * (horodatages en avance sur le son, sans retour possible).
 *
 * Borne basse pour une image en retard : avec N tampons caméra traités dans
 * l'ordre, l'image k n'a pu être capturée qu'après la remise (QBUF) du tampon
 * de l'image k − N (sinon les N tampons étaient tous occupés). Si la boucle
 * est durablement plus lente que la caméra (DQBUF n'attend jamais), cette
 * borne place l'image sur la première arrivée possible après cette remise :
 * les images réellement perdues restent comptées et la durée reste juste.
 * L'arrondi est prudent (jamais en avance d'un créneau).
 *
 * @param prev_slot     créneau de l'image précédente (-1 : première image)
 * @param t_rel_us      instant de sortie de DQBUF, relatif à la première image
 * @param frame_us      période des images
 * @param fresh         true si DQBUF a attendu l'image (voir nid_capture_frame_is_fresh)
 * @param release_rel_us instant de la remise du tampon de l'image k − N, relatif
 *                      à la première image, ou NID_SLOT_NO_RELEASE
 */
static inline int64_t nid_capture_next_slot(int64_t prev_slot, int64_t t_rel_us, int64_t frame_us, bool fresh,
                                            int64_t release_rel_us)
{
    if (prev_slot < 0) {
        return 0;
    }
    int64_t slot = prev_slot + 1;
    if (frame_us <= 0) {
        return slot;
    }
    int64_t bound = 0;
    if (fresh) {
        if (t_rel_us > 0) {
            bound = (t_rel_us + frame_us / 2) / frame_us;   /* arrivée = sortie de DQBUF */
        }
    } else if (release_rel_us != NID_SLOT_NO_RELEASE) {
        const int64_t r = release_rel_us - frame_us / 4;    /* marge : phase de la grille */
        if (r > 0) {
            bound = (r + frame_us - 1) / frame_us;          /* première arrivée après la remise */
        }
    }
    return bound > slot ? bound : slot;     /* un trou = images non capturées faute de tampon libre */
}

/**
 * true si l'attente dans DQBUF (wait_us) montre que l'image vient d'arriver :
 * au moins un quart de période (2 ms au minimum). Une image déjà en file sort
 * en quelques dizaines de µs ; la marge écarte une préemption brève (tâche son).
 */
static inline bool nid_capture_frame_is_fresh(int64_t wait_us, int64_t frame_us)
{
    int64_t threshold = frame_us / 4;
    if (threshold < 2000) {
        threshold = 2000;
    }
    return wait_us >= threshold;
}

#ifdef __cplusplus
}
#endif
