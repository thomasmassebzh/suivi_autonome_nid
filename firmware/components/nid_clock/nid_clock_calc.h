/*
 * nid_clock — calculs purs de la correction de dérive (testés sur PC, test_host/)
 *
 * Conventions : dérive en milliardièmes (ppb ; 1 ppm = 1000 ppb), positive
 * quand la carte avance. Compensation = − durée écoulée × dérive.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Heure plausible : à partir du 2025-01-01 00:00:00 UTC. */
#define NID_CLOCK_VALID_MIN_UTC     1735689600LL

/** Durée minimale entre deux mises à l'heure pour mesurer la dérive (24 h). */
#define NID_CLOCK_MEASURE_MIN_MS    (24LL * 3600LL * 1000LL)

/** Dérive maximale retenue : 500 ppm avec un quartz 32 kHz, 2 % avec l'oscillateur RC. */
#define NID_CLOCK_MAX_PPB_XTAL      500000
#define NID_CLOCK_MAX_PPB_RC        20000000

typedef enum {
    NID_CLOCK_MEASURE_OK = 0,       /*!< dérive mesurée */
    NID_CLOCK_MEASURE_NO_REFERENCE, /*!< pas de mise à l'heure précédente valide, ou heure perdue depuis */
    NID_CLOCK_MEASURE_TOO_SOON,     /*!< moins de 24 h depuis la précédente : mesure trop imprécise */
    NID_CLOCK_MEASURE_IMPLAUSIBLE,  /*!< écart hors de portée d'une dérive (heure réglée à la main ?) */
} nid_clock_measure_t;

typedef struct {
    int64_t elapsed_ms;             /*!< durée depuis la mise à l'heure précédente */
    int32_t residual_ppb;           /*!< dérive restante malgré la correction en place */
    int32_t new_corr_ppb;           /*!< nouvelle correction */
} nid_clock_drift_t;

static inline int64_t nid_clock_abs64(int64_t v)
{
    return v < 0 ? -v : v;
}

static inline bool nid_clock_ppb_plausible(int32_t ppb)
{
    return nid_clock_abs64(ppb) <= NID_CLOCK_MAX_PPB_RC;
}

/**
 * Mesure de la dérive à une mise à l'heure.
 * @param was_valid  l'heure de la carte était valide juste avant (pas de perte d'heure)
 * @param ref_ms     instant (UTC, ms) de la mise à l'heure précédente, 0 si aucune
 * @param board_ms   heure de la carte (compensation à jour), ms
 * @param utc_ms     heure exacte (téléphone), ms
 * @param corr_ppb   correction en place
 * @param max_ppb    dérive maximale plausible
 */
static inline nid_clock_measure_t nid_clock_measure(bool was_valid, int64_t ref_ms, int64_t board_ms, int64_t utc_ms,
                                                    int32_t corr_ppb, int32_t max_ppb, nid_clock_drift_t *m)
{
    nid_clock_drift_t d = { 0, 0, corr_ppb };
    if (!was_valid || ref_ms <= 0) {
        if (m != NULL) {
            *m = d;
        }
        return NID_CLOCK_MEASURE_NO_REFERENCE;
    }
    d.elapsed_ms = utc_ms - ref_ms;
    if (m != NULL) {
        *m = d;
    }
    if (d.elapsed_ms < NID_CLOCK_MEASURE_MIN_MS) {
        return NID_CLOCK_MEASURE_TOO_SOON;
    }
    const int64_t err_ms = board_ms - utc_ms;
    /* err_ms × 10^6 : sans débordement tant que |err| < 9 × 10^12 ms. */
    const int64_t residual = err_ms * 1000000LL / d.elapsed_ms * 1000LL
                             + (err_ms * 1000000LL % d.elapsed_ms) * 1000LL / d.elapsed_ms;
    const int64_t updated = (int64_t)corr_ppb + residual;
    if (nid_clock_abs64(residual) > max_ppb || nid_clock_abs64(updated) > max_ppb) {
        if (m != NULL) {
            *m = d;
        }
        return NID_CLOCK_MEASURE_IMPLAUSIBLE;
    }
    d.residual_ppb = (int32_t)residual;
    d.new_corr_ppb = (int32_t)updated;
    if (m != NULL) {
        *m = d;
    }
    return NID_CLOCK_MEASURE_OK;
}

/**
 * Compensation à appliquer après elapsed_us de fonctionnement avec une dérive
 * corr_ppb : − elapsed × corr. Retourne 0 tant que la compensation fait moins
 * d'une milliseconde (elle s'accumule jusqu'au réveil suivant).
 */
static inline int64_t nid_clock_drift_delta_us(int64_t elapsed_us, int32_t corr_ppb)
{
    if (elapsed_us <= 0 || corr_ppb == 0) {
        return 0;
    }
    /* En ms × ppb : 14 jours × 2 % tiennent largement en 64 bits. */
    const int64_t delta = -(elapsed_us / 1000LL) * (int64_t)corr_ppb / 1000000LL;
    return nid_clock_abs64(delta) >= 1000 ? delta : 0;
}

/**
 * Majoration de l'écart (s) depuis la dernière mise à l'heure, pour une
 * dérive de ppm_bound ppm au plus : arrondi au-dessus, plus 1 s (réglage).
 */
static inline uint32_t nid_clock_error_bound_s(int64_t since_s, uint32_t ppm_bound)
{
    if (since_s <= 0) {
        return 1;
    }
    const int64_t e = (since_s * (int64_t)ppm_bound + 999999LL) / 1000000LL + 1;
    return e > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)e;
}

/** Dérive lisible : « +12,3 ppm » (virgule décimale). */
static inline void nid_clock_ppb_str(int32_t ppb, char *buf, size_t len)
{
    const int64_t a = nid_clock_abs64(ppb);
    const int64_t tenths = (a + 50) / 100;
    snprintf(buf, len, "%c%lld,%lld ppm", ppb < 0 ? '-' : '+', (long long)(tenths / 10), (long long)(tenths % 10));
}

#ifdef __cplusplus
}
#endif
