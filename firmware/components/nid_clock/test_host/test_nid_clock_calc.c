/*
 * Tests sur PC de la correction de dérive de nid_clock (nid_clock_calc.h) :
 * mesure à la mise à l'heure, compensation à chaque réveil, et simulation de
 * 14 jours de veille avec réveils fréquents (carte qui avance de 23 ppm, réglée
 * depuis un téléphone à ±200 ms près).
 *
 * Usage : make -C firmware/components/nid_clock/test_host
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nid_clock_calc.h"

static int s_fail;
static int s_ok;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (cond) {                                       \
            s_ok++;                                       \
        } else {                                          \
            s_fail++;                                     \
            printf("ÉCHEC %s:%d : ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

#define DAY_MS (24LL * 3600LL * 1000LL)
#define T0_MS  (1795000000LL * 1000LL)      /* novembre 2026 */

static void test_measure(void)
{
    nid_clock_drift_t m;
    CHECK(nid_clock_measure(false, T0_MS, T0_MS + 2 * DAY_MS, T0_MS + 2 * DAY_MS, 0, NID_CLOCK_MAX_PPB_XTAL, &m)
          == NID_CLOCK_MEASURE_NO_REFERENCE, "heure perdue entre les deux réglages");
    CHECK(nid_clock_measure(true, 0, T0_MS, T0_MS, 0, NID_CLOCK_MAX_PPB_XTAL, &m) == NID_CLOCK_MEASURE_NO_REFERENCE,
          "pas de réglage précédent");
    CHECK(nid_clock_measure(true, T0_MS, T0_MS + DAY_MS / 2 + 500, T0_MS + DAY_MS / 2, 0, NID_CLOCK_MAX_PPB_XTAL, &m)
          == NID_CLOCK_MEASURE_TOO_SOON, "12 h : trop tôt");
    /* 14 jours, la carte avance de 28 s : 28 / 1 209 600 = 23,15 ppm. */
    const int64_t utc = T0_MS + 14 * DAY_MS;
    CHECK(nid_clock_measure(true, T0_MS, utc + 28000, utc, 0, NID_CLOCK_MAX_PPB_XTAL, &m) == NID_CLOCK_MEASURE_OK,
          "mesure sur 14 jours");
    CHECK(m.residual_ppb == 23148 && m.new_corr_ppb == 23148, "dérive 23 148 ppb (obtenu %d)", (int)m.residual_ppb);
    /* Correction déjà en place, reste -2 s : la correction diminue. */
    CHECK(nid_clock_measure(true, T0_MS, utc - 2000, utc, 23148, NID_CLOCK_MAX_PPB_XTAL, &m) == NID_CLOCK_MEASURE_OK &&
          m.new_corr_ppb == 23148 - 1653, "affinage (obtenu %d)", (int)m.new_corr_ppb);
    /* Heure réglée à la main avec 1 h d'erreur : rejetée. */
    CHECK(nid_clock_measure(true, T0_MS, utc + 3600000, utc, 0, NID_CLOCK_MAX_PPB_XTAL, &m) == NID_CLOCK_MEASURE_IMPLAUSIBLE,
          "1 h d'écart rejetée avec un quartz");
    CHECK(nid_clock_measure(true, T0_MS, utc + 3600000, utc, 0, NID_CLOCK_MAX_PPB_RC, &m) == NID_CLOCK_MEASURE_OK,
          "1 h sur 14 jours plausible avec l'oscillateur RC (0,3 %%)");
}

static void test_delta(void)
{
    CHECK(nid_clock_drift_delta_us(0, 23148) == 0, "rien d'écoulé");
    CHECK(nid_clock_drift_delta_us(10LL * 1000000, 23148) == 0, "10 s : moins d'1 ms, accumulé");
    CHECK(nid_clock_drift_delta_us(3600LL * 1000000, 23148) == -83332, "1 h : -83,3 ms (obtenu %lld)",
          (long long)nid_clock_drift_delta_us(3600LL * 1000000, 23148));
    CHECK(nid_clock_drift_delta_us(14 * DAY_MS * 1000, -NID_CLOCK_MAX_PPB_RC) > 0, "14 jours à -2 %% sans débordement");
    CHECK(nid_clock_error_bound_s(14 * 86400, 15) == 20, "majoration 14 j à 15 ppm (obtenu %u)",
          (unsigned)nid_clock_error_bound_s(14 * 86400, 15));
    char b[24];
    nid_clock_ppb_str(23148, b, sizeof(b));
    CHECK(strcmp(b, "+23,1 ppm") == 0, "texte %s", b);
    nid_clock_ppb_str(-950, b, sizeof(b));
    CHECK(strcmp(b, "-1,0 ppm") == 0, "texte %s", b);
}

/*
 * Simulation : la carte avance de 23 ppm. Mise à l'heure J0, J14 (mesure), puis
 * 14 jours de réveils toutes les 10 min avec compensation : écart final.
 */
static void test_simulation(void)
{
    const double drift = 23e-6;
    double board_err_ms = 0;        /* écart carte - vérité */
    int32_t corr = 0;
    /* Période 1 : sans correction. */
    board_err_ms += 14 * DAY_MS * drift;
    nid_clock_drift_t m;
    const int64_t utc = T0_MS + 14 * DAY_MS;
    const int64_t phone_err = 180;  /* téléphone en retard de 180 ms */
    CHECK(nid_clock_measure(true, T0_MS, utc + (int64_t)board_err_ms, utc - phone_err, corr, NID_CLOCK_MAX_PPB_XTAL, &m)
          == NID_CLOCK_MEASURE_OK, "mesure");
    corr = m.new_corr_ppb;
    board_err_ms = -phone_err;      /* réglée sur le téléphone */
    /* Période 2 : réveils toutes les 10 min, compensation à chaque fois. */
    double acc_us = 0;
    for (int i = 0; i < 14 * 24 * 6; i++) {
        const int64_t step_us = 600LL * 1000000;
        board_err_ms += step_us / 1000.0 * drift;
        acc_us += (double)step_us;
        const int64_t d = nid_clock_drift_delta_us((int64_t)acc_us, corr);
        if (d != 0) {
            board_err_ms += d / 1000.0;
            acc_us = 0;
        }
    }
    printf("Simulation : dérive 23 ppm, écart après 14 jours sans correction %.1f s, avec correction %.2f s\n",
           14 * DAY_MS * drift / 1000.0, board_err_ms / 1000.0);
    CHECK(board_err_ms > -1000 && board_err_ms < 1000, "écart final %.0f ms (attendu < 1 s)", board_err_ms);
}

int main(void)
{
    test_measure();
    test_delta();
    test_simulation();
    printf("%d vérification(s) réussie(s), %d échec(s)\n", s_ok, s_fail);
    return s_fail ? 1 : 0;
}
