/*
 * nid_ir_sun — décision jour/nuit : lever et coucher du soleil (algorithme NOAA)
 *
 * Calcul pur : aucune dépendance à ESP-IDF (hormis les codes d'erreur), ni au
 * fuseau horaire (TZ), ni à la bibliothèque de dates. Le même fichier est compilé
 * par le firmware et par les tests sur PC (test_host/).
 *
 * Principe (NOAA Solar Calculator) : la déclinaison du soleil et l'équation du
 * temps, à un instant donné, fixent l'angle horaire HA du lever/coucher pour un
 * zénith de 90,833° (bord supérieur du disque + réfraction standard) :
 *
 *     cos(HA) = cos(90,833°) / (cos(lat) · cos(décl)) − tan(lat) · tan(décl)
 *     heure UTC = 720 − 4 · (longitude ± HA) − équation du temps   [min depuis 0 h UTC]
 *
 * La déclinaison varie de quelques centièmes de degré dans la journée : le
 * calcul est repris à l'heure trouvée (deux affinages suffisent, l'écart final
 * est de l'ordre de la seconde).
 *
 * Jour traité : le jour solaire local contenant l'instant demandé, c'est-à-dire
 * le jour civil en temps solaire moyen du lieu (UTC + longitude/15 h). Il est
 * choisi d'après la longitude, pas d'après la date UTC : en Martinique (UTC-4)
 * la soirée locale tombe déjà dans le jour UTC suivant, et à l'ouest du
 * méridien 90° O le coucher du soleil a lieu après minuit UTC.
 */
#include "nid_ir.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define PI_RAD            3.14159265358979323846
#define SECONDS_PER_DAY   86400
#define TIME_LIMIT_S      (INT64_C(1) << 50)   /* ≈ 35 millions d'années : au-delà, l'horloge est aberrante */
#define JD_UNIX_EPOCH     2440587.5   /* jour julien du 1970-01-01 à 0 h UTC */
#define JD_J2000          2451545.0   /* jour julien du 2000-01-01 à 12 h */
#define SUN_ZENITH_DEG    90.833      /* lever/coucher : bord supérieur du disque + réfraction */
#define LATITUDE_MAX_DEG  89.9        /* évite tan(±90°) ; la région est de toute façon polaire */
#define REFINE_PASSES     2           /* affinages après l'estimation initiale */
#define CONVERGED_MIN     0.005       /* écart (min) sous lequel l'affinage s'arrête (0,3 s) */

/** Régime du soleil sur le jour solaire considéré. */
typedef enum {
    SUN_REGIME_NORMAL = 0,   /*!< lever et coucher dans la journée */
    SUN_REGIME_ALWAYS_UP,    /*!< jour polaire : le soleil ne se couche pas */
    SUN_REGIME_ALWAYS_DOWN,  /*!< nuit polaire : le soleil ne se lève pas */
} sun_regime_t;

/** Termes ne dépendant que du lieu, calculés une fois (double logiciel sur l'ESP32-P4 : on économise les appels). */
typedef struct {
    double longitude;    /*!< degrés, est positif */
    double cos_lat;
    double tan_lat;
    double cos_zenith;
} sun_site_t;

static double deg_to_rad(double deg)
{
    return deg * (PI_RAD / 180.0);
}

static double rad_to_deg(double rad)
{
    return rad * (180.0 / PI_RAD);
}

/* Division entière arrondie vers −∞ (instants avant 1970 ou longitudes à l'ouest). */
static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        q--;
    }
    return q;
}

static bool coordinates_valid(double latitude, double longitude)
{
    return isfinite(latitude) && isfinite(longitude) && fabs(latitude) <= 90.0 && fabs(longitude) <= 180.0;
}

/** Numéro du jour solaire local (jours depuis 1970-01-01) contenant l'instant t. */
static int64_t solar_day(double longitude, int64_t t)
{
    /* Horloge aberrante : borne l'instant pour éviter tout dépassement d'entier. */
    if (t > TIME_LIMIT_S) {
        t = TIME_LIMIT_S;
    } else if (t < -TIME_LIMIT_S) {
        t = -TIME_LIMIT_S;
    }
    /* 1° de longitude = 4 min = 240 s de temps solaire moyen. */
    return floor_div(t + llround(longitude * 240.0), SECONDS_PER_DAY);
}

/** Déclinaison du soleil (rad) et équation du temps (min) au jour julien jd. */
static void sun_position(double jd, double *declination, double *eq_time_min)
{
    const double t = (jd - JD_J2000) / 36525.0;   /* siècles juliens depuis J2000 */

    /* Longitude moyenne (°), anomalie moyenne (rad), excentricité de l'orbite. */
    double l0 = fmod(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0);
    if (l0 < 0.0) {
        l0 += 360.0;
    }
    const double m = deg_to_rad(357.52911 + t * (35999.05029 - 0.0001537 * t));
    const double e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
    const double sin_m = sin(m);
    const double sin_2m = sin(2.0 * m);

    /* Équation du centre → longitude vraie, puis apparente (nutation, aberration). */
    const double center = sin_m * (1.914602 - t * (0.004817 + 0.000014 * t)) +
                          sin_2m * (0.019993 - 0.000101 * t) +
                          sin(3.0 * m) * 0.000289;
    const double omega = deg_to_rad(125.04 - 1934.136 * t);
    const double apparent_long = deg_to_rad(l0 + center - 0.00569 - 0.00478 * sin(omega));

    /* Obliquité de l'écliptique, corrigée de la nutation. */
    const double arcsec = 21.448 - t * (46.8150 + t * (0.00059 - t * 0.001813));
    const double obliquity = deg_to_rad(23.0 + (26.0 + arcsec / 60.0) / 60.0 + 0.00256 * cos(omega));

    *declination = asin(sin(obliquity) * sin(apparent_long));

    /* Équation du temps (min). */
    const double half_tan = tan(obliquity / 2.0);
    const double y = half_tan * half_tan;
    const double two_l0 = 2.0 * deg_to_rad(l0);
    const double eq = y * sin(two_l0) -
                      2.0 * e * sin_m +
                      4.0 * e * y * sin_m * cos(two_l0) -
                      0.5 * y * y * sin(2.0 * two_l0) -
                      1.25 * e * e * sin_2m;
    *eq_time_min = 4.0 * rad_to_deg(eq);
}

/** cos(HA) du lever/coucher : > 1 le soleil ne se lève pas, < −1 il ne se couche pas. */
static double cos_hour_angle(const sun_site_t *site, double declination)
{
    return site->cos_zenith / (site->cos_lat * cos(declination)) - site->tan_lat * tan(declination);
}

/** Heure UTC (min depuis 0 h UTC) du lever (sign = +1) ou du coucher (sign = −1) pour un soleil donné. */
static double event_minutes(const sun_site_t *site, double declination, double eq_time_min, int sign)
{
    double c = cos_hour_angle(site, declination);
    c = fmin(1.0, fmax(-1.0, c));   /* l'affinage ne doit pas faire sortir d'une zone limite */
    return 720.0 - 4.0 * (site->longitude + sign * rad_to_deg(acos(c))) - eq_time_min;
}

/**
 * Lever et coucher (secondes Unix UTC) du jour solaire n° day. Hors régime
 * normal, les sorties ne sont pas modifiées.
 */
static sun_regime_t sun_day_events(int64_t day, double latitude, double longitude,
                                   int64_t *sunrise, int64_t *sunset)
{
    const double jd0 = JD_UNIX_EPOCH + (double)day;   /* 0 h UTC du jour considéré */
    const double lat = deg_to_rad(fmin(LATITUDE_MAX_DEG, fmax(-LATITUDE_MAX_DEG, latitude)));
    const sun_site_t site = {
        .longitude = longitude,
        .cos_lat = cos(lat),
        .tan_lat = tan(lat),
        .cos_zenith = cos(deg_to_rad(SUN_ZENITH_DEG)),
    };

    /* Estimation à midi solaire moyen : donne aussi le régime (polaire ou non). */
    const double noon = 720.0 - 4.0 * longitude;
    double declination;
    double eq_time;
    sun_position(jd0 + noon / 1440.0, &declination, &eq_time);
    const double c = cos_hour_angle(&site, declination);
    if (c > 1.0) {
        return SUN_REGIME_ALWAYS_DOWN;
    }
    if (c < -1.0) {
        return SUN_REGIME_ALWAYS_UP;
    }
    double rise = event_minutes(&site, declination, eq_time, +1);
    double set = event_minutes(&site, declination, eq_time, -1);

    /* Affinage : soleil évalué à l'heure de chaque événement. */
    for (int pass = 0; pass < REFINE_PASSES; pass++) {
        const double prev_rise = rise;
        const double prev_set = set;
        sun_position(jd0 + prev_rise / 1440.0, &declination, &eq_time);
        rise = event_minutes(&site, declination, eq_time, +1);
        sun_position(jd0 + prev_set / 1440.0, &declination, &eq_time);
        set = event_minutes(&site, declination, eq_time, -1);
        if (fabs(rise - prev_rise) < CONVERGED_MIN && fabs(set - prev_set) < CONVERGED_MIN) {
            break;
        }
    }

    *sunrise = day * SECONDS_PER_DAY + llround(rise * 60.0);
    *sunset = day * SECONDS_PER_DAY + llround(set * 60.0);
    return SUN_REGIME_NORMAL;
}

esp_err_t nid_ir_sun_times(double latitude, double longitude, time_t day_utc,
                           time_t *sunrise_utc, time_t *sunset_utc)
{
    if (sunrise_utc == NULL || sunset_utc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *sunrise_utc = day_utc;
    *sunset_utc = day_utc;
    if (!coordinates_valid(latitude, longitude)) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t sunrise;
    int64_t sunset;
    if (sun_day_events(solar_day(longitude, (int64_t)day_utc), latitude, longitude,
                       &sunrise, &sunset) != SUN_REGIME_NORMAL) {
        return ESP_ERR_INVALID_STATE;
    }
    *sunrise_utc = (time_t)sunrise;
    *sunset_utc = (time_t)sunset;
    return ESP_OK;
}

bool nid_ir_is_night(const nid_config_t *cfg, time_t now_utc)
{
    if (cfg == NULL) {
        return true;
    }
    switch (cfg->night_mode) {
    case NID_NIGHT_MODE_DAY:
        return false;
    case NID_NIGHT_MODE_NIGHT:
        return true;
    default:   /* NID_NIGHT_MODE_AUTO */
        break;
    }

    const double latitude = cfg->latitude;
    const double longitude = cfg->longitude;
    if (!coordinates_valid(latitude, longitude)) {
        return true;   /* position inutilisable : éclairer plutôt que filmer dans le noir */
    }

    const int64_t now = (int64_t)now_utc;
    int64_t sunrise;
    int64_t sunset;
    switch (sun_day_events(solar_day(longitude, now), latitude, longitude, &sunrise, &sunset)) {
    case SUN_REGIME_ALWAYS_UP:
        return false;
    case SUN_REGIME_ALWAYS_DOWN:
        return true;
    default:
        break;
    }

    /* Jour = [lever + retard à l'aube, coucher − avance au crépuscule] ; fenêtre vide si début > fin. */
    const int64_t day_start = sunrise + (int64_t)cfg->dawn_delay_min * 60;
    const int64_t day_end = sunset - (int64_t)cfg->dusk_advance_min * 60;
    return !(now >= day_start && now <= day_end);
}
