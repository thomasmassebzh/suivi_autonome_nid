/*
 * nid_clock — heure système de l'ESP32-P4, sans horloge externe
 *
 * Base de temps : le compteur basse consommation (« RTC timer ») de l'ESP32-P4
 * tourne pendant la veille profonde ; l'heure système (time(), gettimeofday())
 * en découle et reste juste d'un réveil à l'autre. ESP-IDF étalonne l'horloge
 * lente sur le quartz principal (40 MHz) : l'écart fixe du quartz 32 kHz est
 * déjà corrigé ; il reste la dérive en température et l'erreur du quartz
 * principal (quelques ppm à 10 ppm : 10 à 15 s sur 14 jours).
 *
 * Sources possibles de l'horloge lente (relevées à l'exécution) :
 *  - quartz 32,768 kHz de la carte (CONFIG_RTC_CLK_SRC_EXT_CRYS) : P4X ;
 *  - oscillateur RC interne 150 kHz si le quartz est absent ou ne démarre pas
 *    (ESP-IDF bascule seul dessus) : dérive de plusieurs minutes par jour ;
 * et pile de sauvegarde (VBAT, CONFIG_ESP_VBAT_INIT_AUTO) : support de pile
 * rechargeable des cartes Waveshare. Sans pile, une coupure d'alimentation
 * (échange de batterie) fait perdre l'heure : elle se règle alors depuis le
 * téléphone (page d'installation, réglage automatique à l'ouverture).
 *
 * Correction de dérive : à chaque mise à l'heure depuis le téléphone, l'écart
 * constaté depuis la mise à l'heure précédente (au moins 24 h plus tôt, sans
 * perte d'heure entre les deux) donne la dérive propre de la carte, en
 * milliardièmes (ppb). Elle est enregistrée en NVS et compensée à chaque
 * réveil (nid_clock_sync_from_rtc). Résolution : heure du téléphone à la
 * milliseconde, soit ≈ 2 ppm sur 24 h ; erreur résiduelle visée ≤ 5 s sur
 * 14 jours avec le quartz 32 kHz (FP1.6 : ≤ 1 min).
 *
 * Le pilote DS3231 / DS1307 n'est pas écrit : nid_clock_init() n'utilise que
 * l'horloge interne (alarme et température non disponibles).
 */
#include "nid_clock.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "soc/rtc.h"
#if CONFIG_ESP_VBAT_INIT_AUTO
#include "esp_vbat.h"
#endif

#include "nid_clock_calc.h"

static const char *TAG = "nid_clock";

#define NVS_NAMESPACE       "nid_clock"
#define NVS_KEY_REF_MS      "ref_ms"        /* instant (UTC, ms) de la dernière mise à l'heure */
#define NVS_KEY_CORR        "corr_ppb"      /* dérive mesurée de la carte (ppb, > 0 : la carte avance) */
#define NVS_KEY_SAMPLES     "mesures"       /* nombre de mesures de dérive retenues */
#define ANCHOR_MAGIC        0x4E434C4Bu     /* « NCLK » */

/* Point de départ de la compensation, conservé pendant la veille profonde. */
static RTC_DATA_ATTR uint32_t s_anchor_magic;
static RTC_DATA_ATTR int64_t  s_anchor_us;  /* heure système (µs UTC) à la dernière compensation */

static bool     s_inited;
static int64_t  s_ref_ms;                   /* 0 : jamais mise à l'heure depuis le téléphone */
static int32_t  s_corr_ppb;
static uint32_t s_samples;
static nid_clock_source_t s_source = NID_CLOCK_SRC_UNKNOWN;

/* ------------------------------------------------------------------------- */
/* Outils                                                                     */
/* ------------------------------------------------------------------------- */

static int64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static void set_now_us(int64_t us)
{
    struct timeval tv = {
        .tv_sec = (time_t)(us / 1000000LL),
        .tv_usec = (suseconds_t)(us % 1000000LL),
    };
    if (tv.tv_usec < 0) {
        tv.tv_sec -= 1;
        tv.tv_usec += 1000000;
    }
    settimeofday(&tv, NULL);
}

static bool nvs_ready(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* nid_state fait de même : NVS réinitialisée seulement si illisible. */
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err == ESP_OK;
}

static void load_nvs(void)
{
    nvs_handle_t h;
    if (!nvs_ready() || nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int64_t ref = 0;
    int32_t corr = 0;
    uint32_t n = 0;
    if (nvs_get_i64(h, NVS_KEY_REF_MS, &ref) == ESP_OK) {
        s_ref_ms = ref;
    }
    if (nvs_get_i32(h, NVS_KEY_CORR, &corr) == ESP_OK && nid_clock_ppb_plausible(corr)) {
        s_corr_ppb = corr;
    }
    if (nvs_get_u32(h, NVS_KEY_SAMPLES, &n) == ESP_OK) {
        s_samples = n;
    }
    nvs_close(h);
}

static void save_nvs(void)
{
    nvs_handle_t h;
    if (!nvs_ready() || nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS indisponible : correction de dérive non enregistrée");
        return;
    }
    nvs_set_i64(h, NVS_KEY_REF_MS, s_ref_ms);
    nvs_set_i32(h, NVS_KEY_CORR, s_corr_ppb);
    nvs_set_u32(h, NVS_KEY_SAMPLES, s_samples);
    nvs_commit(h);
    nvs_close(h);
}

static nid_clock_source_t detect_source(void)
{
    if (rtc_clk_slow_src_get() == SOC_RTC_SLOW_CLK_SRC_XTAL32K) {
        return NID_CLOCK_SRC_XTAL32K;
    }
    return NID_CLOCK_SRC_RC;
}

/**
 * Dérive maximale estimée (ppm) : quartz 32 kHz ≈ 10 ppm (quartz principal de
 * référence) + 5 ppm (température), ramenés à 5 ppm après une mesure ;
 * oscillateur RC : 2000 ppm, 500 après une mesure (estimations prudentes).
 */
static uint32_t ppm_bound(void)
{
    if (s_source == NID_CLOCK_SRC_RC) {
        return s_samples > 0 ? 500u : 2000u;
    }
    return s_samples > 0 ? 5u : 15u;
}

static bool vbat_present(void)
{
#if CONFIG_ESP_VBAT_INIT_AUTO
    return true;
#else
    return false;
#endif
}

/* ------------------------------------------------------------------------- */
/* API                                                                        */
/* ------------------------------------------------------------------------- */

esp_err_t nid_clock_init(i2c_master_bus_handle_t bus)
{
    (void)bus;      /* DS3231 / DS1307 : pilote non écrit, horloge interne seule */
    if (s_inited) {
        return ESP_OK;
    }
    s_source = detect_source();
    load_nvs();
    if (s_anchor_magic != ANCHOR_MAGIC) {
        /* Démarrage à froid : pas de point de départ (la compensation repart d'ici). */
        s_anchor_magic = ANCHOR_MAGIC;
        s_anchor_us = 0;
    }
    s_inited = true;

    if (s_source == NID_CLOCK_SRC_RC) {
        ESP_LOGW(TAG, "horloge lente sur l'oscillateur RC interne (quartz 32 kHz absent ou arrêté) : "
                 "dérive de plusieurs minutes par jour, l'heure sera corrigée à chaque passage");
    }
    char corr[24];
    nid_clock_ppb_str(s_corr_ppb, corr, sizeof(corr));
    ESP_LOGI(TAG, "horloge %s%s, correction de dérive %s (%" PRIu32 " mesure(s))", nid_clock_model_str(),
             vbat_present() ? ", pile de sauvegarde" : "", corr, s_samples);
    return ESP_OK;
}

nid_rtc_model_t nid_clock_model(void)
{
    return NID_RTC_MODEL_NONE;
}

const char *nid_clock_model_str(void)
{
    switch (s_source) {
    case NID_CLOCK_SRC_XTAL32K:
        return "interne (quartz 32 kHz)";
    case NID_CLOCK_SRC_RC:
        return "interne (oscillateur RC)";
    default:
        return "interne";
    }
}

esp_err_t nid_clock_sync_from_rtc(bool *rtc_valid)
{
    if (!s_inited) {
        nid_clock_init(NULL);
    }
    const bool valid = nid_clock_time_is_valid();
    if (rtc_valid != NULL) {
        *rtc_valid = valid;
    }
    if (!valid) {
        s_anchor_us = 0;
        return ESP_ERR_INVALID_STATE;
    }
    /* Compensation de la dérive mesurée depuis le dernier point de départ. */
    const int64_t now = now_us();
    if (s_anchor_us == 0 || now < s_anchor_us) {
        s_anchor_us = now;
        return ESP_OK;
    }
    const int64_t delta = nid_clock_drift_delta_us(now - s_anchor_us, s_corr_ppb);
    if (delta != 0) {
        set_now_us(now + delta);
        s_anchor_us = now + delta;
        ESP_LOGD(TAG, "dérive compensée : %+lld µs", (long long)delta);
    }
    return ESP_OK;
}

esp_err_t nid_clock_set_time_ms(int64_t utc_ms)
{
    if (!s_inited) {
        nid_clock_init(NULL);
    }
    if (utc_ms < (int64_t)NID_CLOCK_VALID_MIN_UTC * 1000LL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Écart de la carte au moment du réglage (compensation à jour). */
    const bool was_valid = nid_clock_time_is_valid();
    if (was_valid) {
        nid_clock_sync_from_rtc(NULL);
    }
    const int64_t board_ms = now_us() / 1000;

    nid_clock_drift_t m;
    const int32_t max_ppb = (s_source == NID_CLOCK_SRC_RC) ? NID_CLOCK_MAX_PPB_RC : NID_CLOCK_MAX_PPB_XTAL;
    const nid_clock_measure_t r = nid_clock_measure(was_valid, s_ref_ms, board_ms, utc_ms, s_corr_ppb, max_ppb, &m);
    char a[24], b[24];
    switch (r) {
    case NID_CLOCK_MEASURE_OK:
        nid_clock_ppb_str(s_corr_ppb, a, sizeof(a));
        nid_clock_ppb_str(m.new_corr_ppb, b, sizeof(b));
        ESP_LOGI(TAG, "écart de %+lld ms en %.1f h : correction de dérive %s → %s",
                 (long long)(board_ms - utc_ms), (double)m.elapsed_ms / 3.6e6, a, b);
        s_corr_ppb = m.new_corr_ppb;
        s_samples++;
        break;
    case NID_CLOCK_MEASURE_TOO_SOON:
        ESP_LOGI(TAG, "écart de %+lld ms (moins de 24 h depuis le réglage précédent : dérive non mesurée)",
                 (long long)(board_ms - utc_ms));
        break;
    case NID_CLOCK_MEASURE_IMPLAUSIBLE:
        ESP_LOGW(TAG, "écart de %+lld ms en %.1f h invraisemblable (heure réglée à la main ?) : dérive non mise à jour",
                 (long long)(board_ms - utc_ms), (double)m.elapsed_ms / 3.6e6);
        break;
    case NID_CLOCK_MEASURE_NO_REFERENCE:
    default:
        break;
    }

    set_now_us(utc_ms * 1000LL);
    s_anchor_magic = ANCHOR_MAGIC;
    s_anchor_us = utc_ms * 1000LL;
    /* La prochaine mesure part d'ici (l'écart vient d'être remis à zéro). */
    s_ref_ms = utc_ms;
    save_nvs();
    return ESP_OK;
}

esp_err_t nid_clock_set_time(time_t utc)
{
    return nid_clock_set_time_ms((int64_t)utc * 1000LL);
}

bool nid_clock_time_is_valid(void)
{
    return (int64_t)time(NULL) >= NID_CLOCK_VALID_MIN_UTC;
}

void nid_clock_get_info(nid_clock_info_t *out)
{
    if (out == NULL) {
        return;
    }
    if (!s_inited) {
        nid_clock_init(NULL);
    }
    memset(out, 0, sizeof(*out));
    out->source = s_source;
    out->vbat = vbat_present();
    out->correction_ppb = s_corr_ppb;
    out->measurements = s_samples;
    out->last_set_utc = s_ref_ms / 1000;
    if (nid_clock_time_is_valid() && s_ref_ms > 0) {
        const int64_t since_s = (int64_t)time(NULL) - s_ref_ms / 1000;
        out->max_error_s = nid_clock_error_bound_s(since_s > 0 ? since_s : 0, ppm_bound());
    }
}

void nid_clock_set_timezone(const char *posix_tz)
{
    setenv("TZ", posix_tz, 1);
    tzset();
}

size_t nid_clock_format_local(time_t t, const char *fmt, char *buf, size_t len)
{
    struct tm tm;
    localtime_r(&t, &tm);
    return strftime(buf, len, fmt, &tm);
}

esp_err_t nid_clock_set_alarm(time_t utc)
{
    (void)utc;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nid_clock_clear_alarm(bool *fired)
{
    if (fired != NULL) {
        *fired = false;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nid_clock_get_temperature(float *celsius)
{
    (void)celsius;
    return ESP_ERR_NOT_SUPPORTED;
}
