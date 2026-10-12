/*
 * Tests sur PC du calcul jour/nuit de nid_ir (nid_ir_sun.c) — hors compilation ESP-IDF.
 *
 *   make -C firmware/components/nid_ir/test_host        (compile et exécute)
 *
 * Ils couvrent nid_ir_sun_times() et nid_ir_is_night() :
 *   - lever/coucher comparés à astral (refs_data.h, généré par gen_refs.py) pour la
 *     Caravelle, Fort-de-France et d'autres lieux, sur toute l'année, avec ±2 min
 *     contre l'API publique d'astral et quelques secondes contre astral réglé sur le
 *     zénith NOAA exact (90,833°) ;
 *   - choix du jour d'après la longitude (minuit local, minuit UTC, coucher après
 *     minuit UTC à l'ouest, lever la veille en UTC à l'est) ;
 *   - avance au crépuscule, retard à l'aube (positifs, négatifs, fenêtre vide) ;
 *   - modes jour/nuit, jour et nuit polaires, arguments invalides.
 */
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nid_ir.h"
#include "refs_data.h"

#define TOL_ASTRAL_S        120   /* ±2 min : exigence du cahier des charges (modèle de réfraction d'astral) */
#define TOL_NOAA_S          5     /* même algorithme et même zénith : écart de quelques secondes */
#define TOL_NOAA_ARCTIC_S   60    /* au-delà de 66° : astral n'itère que deux fois (mauvais conditionnement) */
#define ARCTIC_LAT          66.0

static int s_checks;
static int s_failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        s_checks++;                                                   \
        if (!(cond)) {                                                \
            s_failures++;                                             \
            printf("  ÉCHEC %s:%d : ", __FILE__, __LINE__);           \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
        }                                                             \
    } while (0)

/* ------------------------------------------------------------------------- */
/* Outils                                                                    */
/* ------------------------------------------------------------------------- */

/* Jours depuis 1970-01-01 d'une date civile (H. Hinnant) : indépendant de TZ et de timegm(). */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static time_t utc_time(int y, int mo, int d, int h, int mi, int s)
{
    return (time_t)(days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s);
}

/* Début (minuit en temps solaire moyen) du jour solaire local de date civile y-m-d. */
static time_t solar_day_start(int y, int mo, int d, double longitude)
{
    return (time_t)(days_from_civil(y, mo, d) * 86400 - llround(longitude * 240.0));
}

static nid_config_t make_cfg(double latitude, double longitude, nid_night_mode_t mode,
                             int advance_min, int delay_min)
{
    nid_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.night_mode = mode;
    cfg.latitude = (float)latitude;
    cfg.longitude = (float)longitude;
    cfg.dusk_advance_min = (int16_t)advance_min;
    cfg.dawn_delay_min = (int16_t)delay_min;
    cfg.ir_power_pct = 100;
    return cfg;
}

/* Lever/coucher avec exactement les coordonnées (float → double) que voit nid_ir_is_night(). */
static esp_err_t cfg_sun_times(const nid_config_t *cfg, time_t t, time_t *rise, time_t *set)
{
    return nid_ir_sun_times((double)cfg->latitude, (double)cfg->longitude, t, rise, set);
}

static long long abs_diff(time_t a, long long b)
{
    return llabs((long long)a - b);
}

static long long max_ll(long long a, long long b)
{
    return a > b ? a : b;
}

static const char *place_name(int place)
{
    return REF_PLACES[place].id;
}

/* ------------------------------------------------------------------------- */
/* Lever et coucher comparés aux références astral                           */
/* ------------------------------------------------------------------------- */

static void test_references(void)
{
    const int count = (int)(sizeof(REF_ENTRIES) / sizeof(REF_ENTRIES[0]));
    const int places = (int)(sizeof(REF_PLACES) / sizeof(REF_PLACES[0]));
    long long max_astral[sizeof(REF_PLACES) / sizeof(REF_PLACES[0])] = { 0 };
    long long max_noaa[sizeof(REF_PLACES) / sizeof(REF_PLACES[0])] = { 0 };
    int used[sizeof(REF_PLACES) / sizeof(REF_PLACES[0])] = { 0 };

    printf("[références astral] %d lieux/dates\n", count);
    for (int i = 0; i < count; i++) {
        const ref_entry_t *e = &REF_ENTRIES[i];
        const ref_place_t *p = &REF_PLACES[e->place];
        const time_t start = solar_day_start(e->year, e->month, e->day, p->longitude);
        /* Tout instant du jour solaire donne le même résultat : début, midi, dernière seconde. */
        const time_t probes[] = { start, start + 43200, start + 86399 };
        time_t first_rise = 0;
        time_t first_set = 0;

        for (int k = 0; k < 3; k++) {
            time_t rise = 0;
            time_t set = 0;
            const esp_err_t err = nid_ir_sun_times(p->latitude, p->longitude, probes[k], &rise, &set);

            if (e->polar != 0) {
                CHECK(err == ESP_ERR_INVALID_STATE, "%s %04d-%02d-%02d : jour/nuit polaire attendu, err=%d",
                      p->id, e->year, e->month, e->day, err);
                CHECK(rise == probes[k] && set == probes[k], "%s : sorties non neutralisées en cas polaire", p->id);
                continue;
            }
            CHECK(err == ESP_OK, "%s %04d-%02d-%02d : err=%d", p->id, e->year, e->month, e->day, err);
            if (err != ESP_OK) {
                continue;
            }
            if (k == 0) {
                first_rise = rise;
                first_set = set;
            } else {
                CHECK(rise == first_rise && set == first_set,
                      "%s %04d-%02d-%02d : résultat différent à l'intérieur du même jour solaire",
                      p->id, e->year, e->month, e->day);
            }
            CHECK(rise < set, "%s %04d-%02d-%02d : lever après coucher", p->id, e->year, e->month, e->day);

            const long long da = max_ll(abs_diff(rise, e->astral_rise), abs_diff(set, e->astral_set));
            const long long dn = max_ll(abs_diff(rise, e->noaa_rise), abs_diff(set, e->noaa_set));
            const long long tol_noaa = fabs(p->latitude) >= ARCTIC_LAT ? TOL_NOAA_ARCTIC_S : TOL_NOAA_S;
            CHECK(da <= TOL_ASTRAL_S, "%s %04d-%02d-%02d : écart %lld s avec astral (max %d)",
                  p->id, e->year, e->month, e->day, da, TOL_ASTRAL_S);
            CHECK(dn <= tol_noaa, "%s %04d-%02d-%02d : écart %lld s avec astral/NOAA 90,833° (max %lld)",
                  p->id, e->year, e->month, e->day, dn, tol_noaa);
            if (da > max_astral[e->place]) {
                max_astral[e->place] = da;
            }
            if (dn > max_noaa[e->place]) {
                max_noaa[e->place] = dn;
            }
            used[e->place] = 1;
        }
    }
    for (int p = 0; p < places; p++) {
        if (used[p]) {
            printf("  %-15s écart max : %3lld s (astral), %2lld s (NOAA 90,833°)\n",
                   place_name(p), max_astral[p], max_noaa[p]);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Décision jour/nuit comparée aux heures de référence                       */
/* ------------------------------------------------------------------------- */

static void test_is_night_references(void)
{
    const int count = (int)(sizeof(REF_ENTRIES) / sizeof(REF_ENTRIES[0]));
    int polar_cases = 0;
    int normal_cases = 0;

    printf("[décision jour/nuit vs références]\n");
    for (int i = 0; i < count; i++) {
        const ref_entry_t *e = &REF_ENTRIES[i];
        const ref_place_t *p = &REF_PLACES[e->place];
        const time_t start = solar_day_start(e->year, e->month, e->day, p->longitude);

        if (e->polar != 0) {
            /* Jour polaire : jamais nuit ; nuit polaire : toujours nuit (une mesure par heure). */
            const nid_config_t cfg = make_cfg(p->latitude, p->longitude, NID_NIGHT_MODE_AUTO, 30, 30);
            for (int h = 0; h < 24; h++) {
                const bool night = nid_ir_is_night(&cfg, start + h * 3600);
                CHECK(night == (e->polar < 0), "%s %04d-%02d-%02d +%d h : %s au lieu de %s", p->id,
                      e->year, e->month, e->day, h, night ? "nuit" : "jour", e->polar < 0 ? "nuit" : "jour");
            }
            polar_cases++;
            continue;
        }

        const long long margin = fabs(p->latitude) >= ARCTIC_LAT ? 300 : 180;   /* > tolérance sur les références */
        const long long rise = e->astral_rise;
        const long long set = e->astral_set;

        /* Sans décalage : nuit avant le lever, jour entre lever et coucher, nuit après. */
        const nid_config_t cfg0 = make_cfg(p->latitude, p->longitude, NID_NIGHT_MODE_AUTO, 0, 0);
        CHECK(nid_ir_is_night(&cfg0, (time_t)(rise - margin)), "%s %04d-%02d-%02d : jour avant le lever",
              p->id, e->year, e->month, e->day);
        CHECK(!nid_ir_is_night(&cfg0, (time_t)(rise + margin)), "%s %04d-%02d-%02d : nuit après le lever",
              p->id, e->year, e->month, e->day);
        CHECK(!nid_ir_is_night(&cfg0, (time_t)(set - margin)), "%s %04d-%02d-%02d : nuit avant le coucher",
              p->id, e->year, e->month, e->day);
        CHECK(nid_ir_is_night(&cfg0, (time_t)(set + margin)), "%s %04d-%02d-%02d : jour après le coucher",
              p->id, e->year, e->month, e->day);
        CHECK(!nid_ir_is_night(&cfg0, (time_t)((rise + set) / 2)), "%s %04d-%02d-%02d : nuit à midi solaire",
              p->id, e->year, e->month, e->day);
        CHECK(nid_ir_is_night(&cfg0, start), "%s %04d-%02d-%02d : jour à minuit solaire",
              p->id, e->year, e->month, e->day);
        CHECK(nid_ir_is_night(&cfg0, start + 86399), "%s %04d-%02d-%02d : jour à la fin du jour solaire",
              p->id, e->year, e->month, e->day);

        /* Avec 30 min d'avance au crépuscule et de retard à l'aube : jour = [lever + 30 min, coucher − 30 min]. */
        const nid_config_t cfg30 = make_cfg(p->latitude, p->longitude, NID_NIGHT_MODE_AUTO, 30, 30);
        CHECK(nid_ir_is_night(&cfg30, (time_t)(rise + 1800 - margin)), "%s %04d-%02d-%02d : retard à l'aube (avant)",
              p->id, e->year, e->month, e->day);
        CHECK(!nid_ir_is_night(&cfg30, (time_t)(rise + 1800 + margin)), "%s %04d-%02d-%02d : retard à l'aube (après)",
              p->id, e->year, e->month, e->day);
        CHECK(!nid_ir_is_night(&cfg30, (time_t)(set - 1800 - margin)), "%s %04d-%02d-%02d : avance au crépuscule (avant)",
              p->id, e->year, e->month, e->day);
        CHECK(nid_ir_is_night(&cfg30, (time_t)(set - 1800 + margin)), "%s %04d-%02d-%02d : avance au crépuscule (après)",
              p->id, e->year, e->month, e->day);
        normal_cases++;
    }
    printf("  %d dates avec lever/coucher, %d dates polaires\n", normal_cases, polar_cases);
}

/* ------------------------------------------------------------------------- */
/* Choix du jour d'après la longitude                                        */
/* ------------------------------------------------------------------------- */

static void test_solar_day_selection(void)
{
    printf("[choix du jour solaire]\n");

    /* Caravelle (UTC-4) : le jour solaire change à 04:03:34 UTC (minuit en temps solaire moyen), pas à minuit UTC. */
    {
        const double lat = 14.77;
        const double lon = -60.89;
        time_t rise21, set21, rise22, set22, a_rise, a_set;
        const time_t changeover = utc_time(2026, 6, 22, 4, 3, 34);

        CHECK(nid_ir_sun_times(lat, lon, utc_time(2026, 6, 21, 16, 0, 0), &rise21, &set21) == ESP_OK, "21 juin");
        CHECK(nid_ir_sun_times(lat, lon, utc_time(2026, 6, 22, 16, 0, 0), &rise22, &set22) == ESP_OK, "22 juin");
        /* Soir local (00:00 à 04:03 UTC le 22) : encore le 21 juin. */
        CHECK(nid_ir_sun_times(lat, lon, utc_time(2026, 6, 22, 0, 0, 0), &a_rise, &a_set) == ESP_OK &&
              a_rise == rise21 && a_set == set21, "minuit UTC : doit rester le jour solaire du 21");
        CHECK(nid_ir_sun_times(lat, lon, changeover - 1, &a_rise, &a_set) == ESP_OK &&
              a_rise == rise21 && a_set == set21, "juste avant minuit solaire : jour du 21");
        CHECK(nid_ir_sun_times(lat, lon, changeover, &a_rise, &a_set) == ESP_OK &&
              a_rise == rise22 && a_set == set22, "minuit solaire : jour du 22");
        CHECK(rise22 - rise21 > 86400 - 60 && rise22 - rise21 < 86400 + 60, "lever : +1 jour à la minute près");
        /* Le coucher du 21 (≈ 22:33 UTC) est bien avant minuit UTC. */
        CHECK(set21 < utc_time(2026, 6, 22, 0, 0, 0) && rise21 > utc_time(2026, 6, 21, 0, 0, 0),
              "Caravelle : lever et coucher dans la même date UTC");

        const nid_config_t cfg = make_cfg(lat, lon, NID_NIGHT_MODE_AUTO, 30, 30);
        /* Chaque instant du soir et de la nuit locale est « nuit », de part et d'autre de minuit UTC et de minuit local. */
        const time_t evening[] = {
            utc_time(2026, 6, 21, 22, 30, 0),   /* 18:30 locale, après le coucher − 30 min */
            utc_time(2026, 6, 21, 23, 59, 59),  /* 19:59:59 locale */
            utc_time(2026, 6, 22, 0, 0, 0),     /* 20:00 locale = minuit UTC */
            utc_time(2026, 6, 22, 2, 0, 0),     /* 22:00 locale */
            utc_time(2026, 6, 22, 3, 59, 59),   /* 23:59:59 locale */
            utc_time(2026, 6, 22, 4, 0, 0),     /* minuit local */
            utc_time(2026, 6, 22, 4, 3, 34),    /* minuit solaire */
            utc_time(2026, 6, 22, 8, 0, 0),     /* 04:00 locale */
        };
        for (size_t i = 0; i < sizeof(evening) / sizeof(evening[0]); i++) {
            CHECK(nid_ir_is_night(&cfg, evening[i]), "Caravelle : jour à t=%lld", (long long)evening[i]);
        }
        /* Le matin et l'après-midi sont « jour ». */
        const time_t daytime[] = {
            utc_time(2026, 6, 22, 11, 0, 0),    /* 07:00 locale */
            utc_time(2026, 6, 22, 16, 0, 0),    /* midi local */
            utc_time(2026, 6, 22, 21, 0, 0),    /* 17:00 locale */
        };
        for (size_t i = 0; i < sizeof(daytime) / sizeof(daytime[0]); i++) {
            CHECK(!nid_ir_is_night(&cfg, daytime[i]), "Caravelle : nuit à t=%lld", (long long)daytime[i]);
        }
    }

    /* San Francisco : en été le coucher (≈ 03:34 UTC) tombe après minuit UTC, mais appartient au jour local. */
    {
        const double lat = 37.77;
        const double lon = -122.42;
        time_t rise, set;
        const nid_config_t cfg = make_cfg(lat, lon, NID_NIGHT_MODE_AUTO, 0, 0);
        CHECK(nid_ir_sun_times(lat, lon, utc_time(2026, 6, 21, 20, 0, 0), &rise, &set) == ESP_OK, "San Francisco");
        CHECK(set > utc_time(2026, 6, 22, 3, 0, 0) && set < utc_time(2026, 6, 22, 4, 0, 0),
              "coucher à San Francisco attendu vers 03:34 UTC le 22, obtenu %lld", (long long)set);
        CHECK(!nid_ir_is_night(&cfg, utc_time(2026, 6, 22, 0, 30, 0)), "17:30 PDT = 00:30 UTC : jour");
        CHECK(!nid_ir_is_night(&cfg, utc_time(2026, 6, 22, 2, 0, 0)), "19:00 PDT = 02:00 UTC : jour");
        CHECK(!nid_ir_is_night(&cfg, utc_time(2026, 6, 22, 3, 0, 0)), "20:00 PDT = 03:00 UTC : jour");
        CHECK(nid_ir_is_night(&cfg, utc_time(2026, 6, 22, 4, 30, 0)), "21:30 PDT = 04:30 UTC : nuit");
        /* Même résultat avant et après minuit UTC (le jour solaire ne change qu'à 08:09:41 UTC). */
        time_t r2, s2;
        CHECK(nid_ir_sun_times(lat, lon, utc_time(2026, 6, 22, 1, 0, 0), &r2, &s2) == ESP_OK && r2 == rise && s2 == set,
              "minuit UTC ne doit pas changer le jour solaire");
        CHECK(nid_ir_sun_times(lat, lon, utc_time(2026, 6, 22, 8, 9, 40), &r2, &s2) == ESP_OK && r2 == rise && s2 == set,
              "dernière seconde du jour solaire");
        CHECK(nid_ir_sun_times(lat, lon, utc_time(2026, 6, 22, 8, 9, 41), &r2, &s2) == ESP_OK && r2 > rise + 86000,
              "jour solaire suivant à 08:09:41 UTC");
    }

    /* Sydney : le lever (05:41 locale) tombe la veille en UTC. */
    {
        const double lat = -33.87;
        const double lon = 151.21;
        time_t rise, set;
        const nid_config_t cfg = make_cfg(lat, lon, NID_NIGHT_MODE_AUTO, 0, 0);
        CHECK(nid_ir_sun_times(lat, lon, utc_time(2026, 12, 21, 0, 0, 0), &rise, &set) == ESP_OK, "Sydney");
        CHECK(rise < utc_time(2026, 12, 21, 0, 0, 0) && rise > utc_time(2026, 12, 20, 12, 0, 0),
              "lever à Sydney attendu le 20 décembre en UTC, obtenu %lld", (long long)rise);
        CHECK(set > utc_time(2026, 12, 21, 0, 0, 0) && set < utc_time(2026, 12, 21, 12, 0, 0),
              "coucher à Sydney attendu le 21 décembre en UTC, obtenu %lld", (long long)set);
        CHECK(nid_ir_is_night(&cfg, utc_time(2026, 12, 20, 17, 0, 0)), "04:00 AEDT : nuit");
        CHECK(!nid_ir_is_night(&cfg, utc_time(2026, 12, 20, 20, 0, 0)), "07:00 AEDT : jour");
        CHECK(!nid_ir_is_night(&cfg, utc_time(2026, 12, 21, 0, 0, 0)), "11:00 AEDT (minuit UTC) : jour");
        CHECK(nid_ir_is_night(&cfg, utc_time(2026, 12, 21, 10, 0, 0)), "21:00 AEDT : nuit");
        CHECK(nid_ir_is_night(&cfg, utc_time(2026, 12, 21, 13, 0, 0)), "00:00 AEDT du 22 : nuit");
    }

    /* Avant 1970 (jours négatifs) : 1969-12-31 à la Caravelle, instant 0 = 19:56 solaire du 31 décembre. */
    {
        time_t rise_a, set_a, rise_b, set_b;
        CHECK(nid_ir_sun_times(14.77, -60.89, 0, &rise_a, &set_a) == ESP_OK, "t=0");
        CHECK(nid_ir_sun_times(14.77, -60.89, -14000, &rise_b, &set_b) == ESP_OK && rise_a == rise_b && set_a == set_b,
              "même jour solaire avant et après t=0");
        CHECK(rise_a < 0 && set_a < 0, "lever/coucher du 31 décembre 1969 (négatifs)");
        CHECK(rise_a > -86400 && set_a > -86400, "lever/coucher du 31 décembre 1969 (dans la bonne journée)");
    }
}

/* ------------------------------------------------------------------------- */
/* Décalages : avance au crépuscule, retard à l'aube                         */
/* ------------------------------------------------------------------------- */

static void test_offsets(void)
{
    static const struct {
        const char *name;
        double lat, lon;
        int y, m, d;
    } sites[] = {
        { "Caravelle 21 juin", 14.77, -60.89, 2026, 6, 21 },
        { "Caravelle 21 déc.", 14.77, -60.89, 2026, 12, 21 },
        { "Paris 20 mars", 48.85, 2.35, 2026, 3, 20 },
        { "Reykjavik 21 juin", 64.15, -21.94, 2026, 6, 21 },
        { "Sydney 21 déc.", -33.87, 151.21, 2026, 12, 21 },
    };
    static const int advances[] = { -60, 0, 30, 240 };
    static const int delays[] = { -60, 0, 30, 240 };

    printf("[décalages]\n");
    for (size_t s = 0; s < sizeof(sites) / sizeof(sites[0]); s++) {
        for (size_t a = 0; a < sizeof(advances) / sizeof(advances[0]); a++) {
            for (size_t d = 0; d < sizeof(delays) / sizeof(delays[0]); d++) {
                const nid_config_t cfg = make_cfg(sites[s].lat, sites[s].lon, NID_NIGHT_MODE_AUTO, advances[a], delays[d]);
                const time_t noon = solar_day_start(sites[s].y, sites[s].m, sites[s].d, sites[s].lon) + 43200;
                time_t rise, set;
                if (cfg_sun_times(&cfg, noon, &rise, &set) != ESP_OK) {
                    CHECK(0, "%s : lever/coucher introuvables", sites[s].name);
                    continue;
                }
                const long long start = (long long)rise + delays[d] * 60LL;    /* début du jour */
                const long long end = (long long)set - advances[a] * 60LL;     /* fin du jour */
                if (start <= end) {
                    CHECK(nid_ir_is_night(&cfg, (time_t)(start - 1)), "%s adv=%d ret=%d : jour 1 s avant le début",
                          sites[s].name, advances[a], delays[d]);
                    CHECK(!nid_ir_is_night(&cfg, (time_t)start), "%s adv=%d ret=%d : nuit au début (borne incluse)",
                          sites[s].name, advances[a], delays[d]);
                    CHECK(!nid_ir_is_night(&cfg, (time_t)((start + end) / 2)), "%s adv=%d ret=%d : nuit en milieu de journée",
                          sites[s].name, advances[a], delays[d]);
                    CHECK(!nid_ir_is_night(&cfg, (time_t)end), "%s adv=%d ret=%d : nuit à la fin (borne incluse)",
                          sites[s].name, advances[a], delays[d]);
                    CHECK(nid_ir_is_night(&cfg, (time_t)(end + 1)), "%s adv=%d ret=%d : jour 1 s après la fin",
                          sites[s].name, advances[a], delays[d]);
                } else {
                    /* Fenêtre de jour vide : toujours nuit. */
                    CHECK(nid_ir_is_night(&cfg, (time_t)start) && nid_ir_is_night(&cfg, (time_t)end) &&
                          nid_ir_is_night(&cfg, (time_t)((start + end) / 2)),
                          "%s adv=%d ret=%d : fenêtre vide, nuit attendue", sites[s].name, advances[a], delays[d]);
                }
            }
        }
    }

    /* Fenêtre vide : à 60° N en décembre (≈ 5 h 40 de jour), 240 min d'avance + 240 min de retard → toujours nuit. */
    {
        const nid_config_t cfg = make_cfg(60.0, 10.0, NID_NIGHT_MODE_AUTO, 240, 240);
        const time_t start = solar_day_start(2026, 12, 21, 10.0);
        int day_seen = 0;
        for (int t = 0; t < 86400; t += 900) {
            day_seen += !nid_ir_is_night(&cfg, start + t);
        }
        CHECK(day_seen == 0, "60° N en décembre avec 240/240 min : %d instants de jour (aucun attendu)", day_seen);
        const nid_config_t cfg0 = make_cfg(60.0, 10.0, NID_NIGHT_MODE_AUTO, 0, 0);
        day_seen = 0;
        for (int t = 0; t < 86400; t += 900) {
            day_seen += !nid_ir_is_night(&cfg0, start + t);
        }
        CHECK(day_seen > 16 && day_seen < 28, "60° N en décembre sans décalage : %d quarts d'heure de jour", day_seen);
    }

    /* Caravelle, décalage 30/30 : exemple lisible en heure locale (UTC-4), 21 juin ≈ lever 05:33, coucher 18:32. */
    {
        const nid_config_t cfg = make_cfg(14.77, -60.89, NID_NIGHT_MODE_AUTO, 30, 30);
        CHECK(nid_ir_is_night(&cfg, utc_time(2026, 6, 21, 9, 45, 0)), "05:45 locale : nuit (retard à l'aube 30 min)");
        CHECK(!nid_ir_is_night(&cfg, utc_time(2026, 6, 21, 10, 15, 0)), "06:15 locale : jour");
        CHECK(!nid_ir_is_night(&cfg, utc_time(2026, 6, 21, 21, 45, 0)), "17:45 locale : jour");
        CHECK(nid_ir_is_night(&cfg, utc_time(2026, 6, 21, 22, 15, 0)), "18:15 locale : nuit (avance au crépuscule 30 min)");
    }
}

/* ------------------------------------------------------------------------- */
/* Modes, jour et nuit polaires, balayage de l'année                          */
/* ------------------------------------------------------------------------- */

static void test_modes(void)
{
    printf("[modes jour / nuit]\n");
    const nid_config_t day = make_cfg(14.77, -60.89, NID_NIGHT_MODE_DAY, 30, 30);
    const nid_config_t night = make_cfg(14.77, -60.89, NID_NIGHT_MODE_NIGHT, 30, 30);
    nid_config_t bad_day = make_cfg(NAN, NAN, NID_NIGHT_MODE_DAY, 0, 0);
    nid_config_t bad_night = make_cfg(NAN, NAN, NID_NIGHT_MODE_NIGHT, 0, 0);
    const time_t start = utc_time(2026, 6, 21, 0, 0, 0);
    for (int h = 0; h < 48; h++) {
        CHECK(!nid_ir_is_night(&day, start + h * 3600), "mode jour : nuit à +%d h", h);
        CHECK(nid_ir_is_night(&night, start + h * 3600), "mode nuit : jour à +%d h", h);
        CHECK(!nid_ir_is_night(&bad_day, start + h * 3600), "mode jour avec coordonnées invalides");
        CHECK(nid_ir_is_night(&bad_night, start + h * 3600), "mode nuit avec coordonnées invalides");
    }
}

static void test_polar(void)
{
    printf("[jour et nuit polaires]\n");
    static const struct {
        const char *name;
        double lat, lon;
    } sites[] = {
        { "Tromsø", 69.65, 18.96 },
        { "Longyearbyen", 78.22, 15.65 },
        { "Antarctique 70° S", -70.0, 20.0 },
    };

    for (size_t s = 0; s < sizeof(sites) / sizeof(sites[0]); s++) {
        const nid_config_t cfg = make_cfg(sites[s].lat, sites[s].lon, NID_NIGHT_MODE_AUTO, 30, 30);
        const bool north = sites[s].lat > 0;
        /* Autour du solstice de juin (jour polaire au nord, nuit polaire au sud) puis de décembre (l'inverse) :
         * trois jours consécutifs, une mesure par heure. */
        for (int pass = 0; pass < 2; pass++) {
            const bool june = (pass == 0);
            const bool expect_night = (north != june);
            const time_t t0 = utc_time(2026, june ? 6 : 12, 20, 0, 0, 0);
            int wrong = 0;
            for (int h = 0; h < 72; h++) {
                if (nid_ir_is_night(&cfg, t0 + h * 3600) != expect_night) {
                    wrong++;
                }
            }
            CHECK(wrong == 0, "%s %s : %d instants incohérents", sites[s].name,
                  june ? "juin" : "décembre", wrong);
            time_t rise, set;
            CHECK(cfg_sun_times(&cfg, t0 + 40000, &rise, &set) == ESP_ERR_INVALID_STATE,
                  "%s : nid_ir_sun_times() doit signaler le régime polaire", sites[s].name);
        }
    }

    /* Balayage de l'année : le régime ne change que quatre fois (nuit → normal → jour → normal → nuit), sans oscillation. */
    for (size_t s = 0; s < sizeof(sites) / sizeof(sites[0]); s++) {
        int transitions = 0;
        int prev = -2;
        for (int day = 0; day < 365; day++) {
            time_t rise, set;
            const time_t noon = solar_day_start(2026, 1, 1, sites[s].lon) + day * 86400LL + 43200;
            const esp_err_t err = nid_ir_sun_times(sites[s].lat, sites[s].lon, noon, &rise, &set);
            int regime = 0;
            if (err == ESP_ERR_INVALID_STATE) {
                const nid_config_t cfg = make_cfg(sites[s].lat, sites[s].lon, NID_NIGHT_MODE_AUTO, 0, 0);
                regime = nid_ir_is_night(&cfg, noon) ? -1 : 1;
            }
            if (prev != -2 && regime != prev) {
                transitions++;
            }
            prev = regime;
        }
        CHECK(transitions == 4, "%s : %d changements de régime sur l'année (4 attendus)", sites[s].name, transitions);
    }

    /* Régime à midi solaire par jour pour la Caravelle : jamais polaire. */
    for (int day = 0; day < 366; day++) {
        time_t rise, set;
        const time_t noon = solar_day_start(2028, 1, 1, -60.89) + day * 86400LL + 43200;
        CHECK(nid_ir_sun_times(14.77, -60.89, noon, &rise, &set) == ESP_OK, "Caravelle jour %d de 2028", day);
    }
}

static void test_year_scan(void)
{
    printf("[balayage de l'année à la Caravelle]\n");
    const double lat = 14.77;
    const double lon = -60.89;
    for (int year = 2026; year <= 2028; year += 2) {
        time_t prev_rise = 0;
        time_t prev_set = 0;
        long long min_rise = 1 << 30, max_rise = -(1 << 30), min_set = 1 << 30, max_set = -(1 << 30);
        const int days = (year % 4 == 0) ? 366 : 365;
        for (int day = 0; day < days; day++) {
            const time_t start = solar_day_start(year, 1, 1, lon) + day * 86400LL;
            time_t rise, set;
            CHECK(nid_ir_sun_times(lat, lon, start + 43200, &rise, &set) == ESP_OK, "%d jour %d", year, day);
            /* Heure solaire du lever et du coucher (secondes depuis minuit solaire). */
            const long long sr = (long long)rise - start;
            const long long ss = (long long)set - start;
            if (sr < min_rise) { min_rise = sr; }
            if (sr > max_rise) { max_rise = sr; }
            if (ss < min_set) { min_set = ss; }
            if (ss > max_set) { max_set = ss; }
            if (day > 0) {
                /* Au plus ≈ 46 s d'un jour à l'autre (équation du temps + déclinaison, en septembre) ;
                 * une erreur de choix du jour donnerait des sauts de plusieurs milliers de secondes. */
                const long long dr = (long long)rise - prev_rise - 86400;
                const long long ds = (long long)set - prev_set - 86400;
                CHECK(llabs(dr) <= 60 && llabs(ds) <= 60, "%d jour %d : saut de %lld/%lld s d'un jour à l'autre", year, day, dr, ds);
            }
            prev_rise = rise;
            prev_set = set;
        }
        /* Heure locale (UTC-4) : lever entre 05:15 et 06:45, coucher entre 17:30 et 19:00 (aux dérives du temps solaire près). */
        CHECK(min_rise > 5 * 3600 && max_rise < 7 * 3600, "%d : lever solaire hors de 05h-07h (%lld..%lld)", year, min_rise, max_rise);
        CHECK(min_set > 17 * 3600 && max_set < 19 * 3600, "%d : coucher solaire hors de 17h-19h (%lld..%lld)", year, min_set, max_set);
        printf("  %d : lever solaire %02lld:%02lld..%02lld:%02lld, coucher solaire %02lld:%02lld..%02lld:%02lld\n", year,
               min_rise / 3600, min_rise % 3600 / 60, max_rise / 3600, max_rise % 3600 / 60,
               min_set / 3600, min_set % 3600 / 60, max_set / 3600, max_set % 3600 / 60);
    }
}

/* ------------------------------------------------------------------------- */
/* Arguments invalides et valeurs extrêmes                                   */
/* ------------------------------------------------------------------------- */

static void test_invalid_inputs(void)
{
    printf("[arguments invalides et valeurs extrêmes]\n");
    time_t rise = 123, set = 456;
    const time_t now = utc_time(2026, 6, 21, 12, 0, 0);

    CHECK(nid_ir_sun_times(14.77, -60.89, now, NULL, &set) == ESP_ERR_INVALID_ARG, "sunrise NULL");
    CHECK(nid_ir_sun_times(14.77, -60.89, now, &rise, NULL) == ESP_ERR_INVALID_ARG, "sunset NULL");
    CHECK(nid_ir_sun_times(90.5, 0.0, now, &rise, &set) == ESP_ERR_INVALID_ARG, "latitude 90,5");
    CHECK(rise == now && set == now, "sorties neutralisées sur argument invalide");
    CHECK(nid_ir_sun_times(0.0, 180.5, now, &rise, &set) == ESP_ERR_INVALID_ARG, "longitude 180,5");
    CHECK(nid_ir_sun_times(NAN, 0.0, now, &rise, &set) == ESP_ERR_INVALID_ARG, "latitude NaN");
    CHECK(nid_ir_sun_times(0.0, INFINITY, now, &rise, &set) == ESP_ERR_INVALID_ARG, "longitude infinie");

    /* Coordonnées inutilisables en mode auto : nuit (on éclaire plutôt que de filmer dans le noir). */
    const nid_config_t nan_cfg = make_cfg(NAN, 0.0, NID_NIGHT_MODE_AUTO, 30, 30);
    const nid_config_t big_cfg = make_cfg(0.0, 400.0, NID_NIGHT_MODE_AUTO, 30, 30);
    CHECK(nid_ir_is_night(&nan_cfg, now), "latitude NaN : nuit attendue");
    CHECK(nid_ir_is_night(&big_cfg, now), "longitude 400 : nuit attendue");
    CHECK(nid_ir_is_night(NULL, now), "config NULL : nuit attendue");

    /* Pôles, méridien opposé, instants extrêmes : aucun plantage, résultat cohérent. */
    static const double lats[] = { -90.0, -89.95, 0.0, 89.95, 90.0 };
    static const double lons[] = { -180.0, -179.99, 0.0, 179.99, 180.0 };
    for (size_t i = 0; i < sizeof(lats) / sizeof(lats[0]); i++) {
        for (size_t j = 0; j < sizeof(lons) / sizeof(lons[0]); j++) {
            for (int month = 1; month <= 12; month += 3) {
                const time_t t = utc_time(2026, month, 15, 12, 0, 0);
                const esp_err_t err = nid_ir_sun_times(lats[i], lons[j], t, &rise, &set);
                CHECK(err == ESP_OK || err == ESP_ERR_INVALID_STATE, "(%.2f, %.2f) mois %d : err=%d", lats[i], lons[j], month, err);
                if (err == ESP_OK) {
                    CHECK(rise < set && set - rise < 86400 && llabs((long long)rise - t) < 86400 && llabs((long long)set - t) < 86400,
                          "(%.2f, %.2f) mois %d : lever/coucher incohérents", lats[i], lons[j], month);
                }
            }
        }
    }
    const nid_config_t cfg = make_cfg(14.77, -60.89, NID_NIGHT_MODE_AUTO, 30, 30);
    (void)nid_ir_is_night(&cfg, 0);
    (void)nid_ir_is_night(&cfg, (time_t)INT32_MAX);
    (void)nid_ir_is_night(&cfg, (time_t)INT32_MIN);
    (void)nid_ir_is_night(&cfg, (time_t)INT64_MAX);
    (void)nid_ir_is_night(&cfg, (time_t)INT64_MIN);
    CHECK(nid_ir_sun_times(14.77, -60.89, (time_t)INT64_MAX, &rise, &set) == ESP_OK ||
          nid_ir_sun_times(14.77, -60.89, (time_t)INT64_MAX, &rise, &set) == ESP_ERR_INVALID_STATE,
          "instant maximal");
}

/* ------------------------------------------------------------------------- */

int main(void)
{
    printf("Tests nid_ir (calcul jour/nuit)\n");
    test_references();
    test_is_night_references();
    test_solar_day_selection();
    test_offsets();
    test_modes();
    test_polar();
    test_year_scan();
    test_invalid_inputs();

    if (s_failures != 0) {
        printf("ÉCHEC : %d vérification(s) en erreur sur %d\n", s_failures, s_checks);
        return 1;
    }
    printf("OK : %d vérifications réussies\n", s_checks);
    return 0;
}
