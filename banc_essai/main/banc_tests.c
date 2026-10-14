/*
 * banc_tests — essais des autres fonctions du firmware sur le banc
 *
 *  - nuit : LED IR (PWM, nid_ir), filtre IR-cut, image monochrome (ISP,
 *    nid_capture), décision jour/nuit du firmware (lever / coucher du soleil,
 *    nid_ir_is_night) ;
 *  - son : vumètre du micro ES8311 (nid_capture_audio_level) ;
 *  - batterie (nid_power : pont diviseur sur l'ADC) et température de la puce
 *    (pilote temperature_sensor d'ESP-IDF) ;
 *  - carte SD : débit et latence d'écriture, recomptage de l'espace libre.
 */
#include "banc.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "sdkconfig.h"
#include "driver/temperature_sensor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "nid_board.h"
#include "nid_capture.h"
#include "nid_ir.h"
#include "nid_power.h"
#include "nid_storage.h"

static const char *TAG = "banc_tests";

#define SON_MESURE_MS           200
#define SD_BLOC                 (64 * 1024)
#define SD_LENT_MS              100
#define SD_TEST_FICHIER         "test_debit.bin"

static temperature_sensor_handle_t s_tsens;
static bool s_ir_ok;
static bool s_batt_ok;

/* ------------------------------------------------------------------------- */
/* Initialisation                                                             */
/* ------------------------------------------------------------------------- */

void banc_tests_init(void)
{
    esp_err_t err;
    if (banc_f_active(BANC_F_NUIT)) {
        err = nid_ir_init();
        s_ir_ok = (err == ESP_OK);
        if (!s_ir_ok) {
            banc_f_defaut(BANC_F_NUIT, err);
        }
    }
    if (banc_f_active(BANC_F_BATTERIE)) {
        err = nid_power_battery_init();
        s_batt_ok = (err == ESP_OK);
        if (err != ESP_OK) {
            banc_f_defaut(BANC_F_BATTERIE, err);    /* ESP_ERR_NOT_SUPPORTED : aucune broche configurée */
        }
    }
    if (banc_f_active(BANC_F_TEMPERATURE)) {
        const temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        err = temperature_sensor_install(&tcfg, &s_tsens);
        if (err == ESP_OK) {
            err = temperature_sensor_enable(s_tsens);
        }
        if (err != ESP_OK) {
            banc_f_defaut(BANC_F_TEMPERATURE, err);
            s_tsens = NULL;
        }
    }
    banc_etat_t *e = banc_etat_verrouiller();
#if CONFIG_BANC_NUIT_DEFAUT_AUTO
    e->mode_nuit = BANC_NUIT_AUTO;
#elif CONFIG_BANC_NUIT_DEFAUT_NUIT
    e->mode_nuit = BANC_NUIT_NUIT;
#else
    e->mode_nuit = BANC_NUIT_JOUR;
#endif
    banc_etat_deverrouiller();
}

/* ------------------------------------------------------------------------- */
/* Nuit                                                                       */
/* ------------------------------------------------------------------------- */

/** Applique jour ou nuit comme le firmware (nid_ir_apply), plus le monochrome de l'ISP. */
static void appliquer_nuit(bool nuit, const nid_config_t *cfg)
{
    if (s_ir_ok) {
        nid_ir_apply(nuit, cfg);
    }
    const bool mono = nuit && cfg->night_monochrome;
    const esp_err_t err_mono = nid_capture_set_night(mono);
    const esp_err_t err_filtre = s_ir_ok ? nid_ir_set_day_filter(!nuit) : ESP_ERR_INVALID_STATE;
    banc_etat_t *e = banc_etat_verrouiller();
    e->nuit_active = nuit;
    e->led_ir_pct = (nuit && s_ir_ok && nid_board_pins()->ir_led != GPIO_NUM_NC) ? cfg->ir_power_pct : 0;
    e->filtre_jour = (err_filtre == ESP_OK) ? (int8_t)!nuit : -1;
    e->mono = (err_mono == ESP_OK) && mono;
    banc_etat_deverrouiller();
}

void banc_nuit_actualiser(bool forcer)
{
    if (!banc_f_active(BANC_F_NUIT)) {
        return;
    }
    nid_config_t cfg;
    banc_reglages(&cfg);
    banc_etat_t e;
    banc_etat_lire(&e);
    bool nuit;
    switch (e.mode_nuit) {
    case BANC_NUIT_NUIT:
        nuit = true;
        break;
    case BANC_NUIT_AUTO:
        nuit = nid_ir_is_night(&cfg, time(NULL));
        break;
    default:
        nuit = false;
        break;
    }
    if (!forcer && nuit == e.nuit_active) {
        return;
    }
    appliquer_nuit(nuit, &cfg);
    if (nuit != e.nuit_active) {
        ESP_LOGI(TAG, "passage en mode %s (%s)", nuit ? "NUIT" : "JOUR",
                 e.mode_nuit == BANC_NUIT_AUTO ? "automatique" : "forcé");
        nid_storage_journal("nuit_banc", "%s (%s)", nuit ? "nuit" : "jour",
                            e.mode_nuit == BANC_NUIT_AUTO ? "auto" : "forcé");
    }
}

esp_err_t banc_nuit_mode(banc_mode_nuit_t mode)
{
    if (!banc_f_active(BANC_F_NUIT)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    banc_etat_t *e = banc_etat_verrouiller();
    e->mode_nuit = mode;
    banc_etat_deverrouiller();
    banc_nuit_actualiser(true);
    return ESP_OK;
}

esp_err_t banc_ir_puissance(uint8_t pct)
{
    if (!s_ir_ok || nid_board_pins()->ir_led == GPIO_NUM_NC) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (pct > 100) {
        pct = 100;
    }
    nid_ir_set_led_power(pct);
    banc_etat_t *e = banc_etat_verrouiller();
    e->led_ir_pct = pct;
    banc_etat_deverrouiller();
    ESP_LOGI(TAG, "LED IR à %u %% (GPIO%d)", pct, (int)nid_board_pins()->ir_led);
    return ESP_OK;
}

esp_err_t banc_ircut(bool jour)
{
    const esp_err_t err = s_ir_ok ? nid_ir_set_day_filter(jour) : ESP_ERR_INVALID_STATE;
    banc_etat_t *e = banc_etat_verrouiller();
    e->filtre_jour = (err == ESP_OK) ? (int8_t)jour : -1;
    banc_etat_deverrouiller();
    return err;
}

esp_err_t banc_mono(bool mono)
{
    if (!banc_f_active(BANC_F_NUIT)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_err_t err = nid_capture_set_night(mono);
    banc_etat_t *e = banc_etat_verrouiller();
    e->mono = (err == ESP_OK) && mono;
    banc_etat_deverrouiller();
    return err;
}

esp_err_t banc_soleil(char *lever, char *coucher, size_t len, bool *nuit_maintenant)
{
    nid_config_t cfg;
    banc_reglages(&cfg);
    const time_t now = time(NULL);
    time_t lv = 0, co = 0;
    const esp_err_t err = nid_ir_sun_times(cfg.latitude, cfg.longitude, now, &lv, &co);
    if (nuit_maintenant != NULL) {
        *nuit_maintenant = nid_ir_is_night(&cfg, now);
    }
    if (err != ESP_OK) {
        strlcpy(lever, err == ESP_ERR_INVALID_STATE ? "polaire" : "?", len);
        strlcpy(coucher, lever, len);
        return err;
    }
    struct tm tm;
    localtime_r(&lv, &tm);
    strftime(lever, len, "%H:%M", &tm);
    localtime_r(&co, &tm);
    strftime(coucher, len, "%H:%M", &tm);
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Batterie, température                                                      */
/* ------------------------------------------------------------------------- */

esp_err_t banc_batterie(uint32_t *mv, uint32_t *brut_mv)
{
    *mv = 0;
    *brut_mv = 0;
    if (!s_batt_ok) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_err_t err = nid_power_battery_read_mv(mv);
    *brut_mv = nid_power_battery_last_raw_mv();
    return err;
}

esp_err_t banc_temperature(float *c)
{
    if (s_tsens == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return temperature_sensor_get_celsius(s_tsens, c);
}

/* ------------------------------------------------------------------------- */
/* Son                                                                        */
/* ------------------------------------------------------------------------- */

int banc_dbfs10(uint16_t v)
{
    if (v == 0) {
        return -900;
    }
    return (int)lroundf(200.0f * log10f((float)v / 32767.0f));
}

void banc_son_mesurer(void)
{
    if (!banc_f_active(BANC_F_SON)) {
        return;
    }
    nid_capture_audio_level_t l;
    const esp_err_t err = nid_capture_audio_level(SON_MESURE_MS, &l);
    banc_etat_t *e = banc_etat_verrouiller();
    e->son_niveau_valide = (err == ESP_OK);
    if (err == ESP_OK) {
        e->son_niveau = l;
    }
    e->son_ok = (err == ESP_OK);
    banc_etat_deverrouiller();
}

/* ------------------------------------------------------------------------- */
/* Carte SD                                                                   */
/* ------------------------------------------------------------------------- */

void banc_sd_test(const banc_demande_t *d)
{
    uint32_t mo = d->param ? d->param : CONFIG_BANC_SD_TEST_MO;
    if (mo > 1024) {
        mo = 1024;
    }
    banc_effacer_details();
    banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_JAUNE, "Test d'écriture de la carte SD : %" PRIu32 " Mo...", mo);
    esp_err_t err = banc_preparer_sd();
    if (err != ESP_OK) {
        banc_afficher_erreur("carte SD", err, "Carte SD absente ou illisible (FAT32)");
        return;
    }
    char chemin[NID_PATH_MAX];
    snprintf(chemin, sizeof(chemin), "%s/" SD_TEST_FICHIER, nid_storage_base_dir());

    uint8_t *bloc = heap_caps_malloc(SD_BLOC, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (bloc == NULL) {
        bloc = heap_caps_malloc(SD_BLOC, MALLOC_CAP_SPIRAM);
    }
    if (bloc == NULL) {
        banc_afficher_erreur("test SD", ESP_ERR_NO_MEM, "Mémoire insuffisante pour le tampon de 64 ko");
        return;
    }
    for (size_t i = 0; i < SD_BLOC; i++) {
        bloc[i] = (uint8_t)(i * 7u + 3u);
    }
    const int fd = open(chemin, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd < 0) {
        heap_caps_free(bloc);
        banc_afficher_erreur("test SD", ESP_FAIL, "Création du fichier de test impossible (carte pleine ou protégée ?)");
        return;
    }
    const uint32_t n = mo * 1000000u / SD_BLOC;
    uint32_t max_us = 0, lents = 0, ecrits = 0;
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t i = 0; i < n && !banc_arret_demande(); i++) {
        const int64_t a = esp_timer_get_time();
        const ssize_t w = write(fd, bloc, SD_BLOC);
        const uint32_t us = (uint32_t)(esp_timer_get_time() - a);
        if (w != SD_BLOC) {
            err = ESP_FAIL;
            ESP_LOGE(TAG, "écriture du bloc %" PRIu32 " : errno %d", i, errno);
            break;
        }
        ecrits++;
        if (us > max_us) {
            max_us = us;
        }
        if (us > SD_LENT_MS * 1000u) {
            lents++;
        }
    }
    const int64_t a = esp_timer_get_time();
    if (fsync(fd) != 0) {
        err = ESP_FAIL;
    }
    const uint32_t fsync_us = (uint32_t)(esp_timer_get_time() - a);
    close(fd);
    const int64_t total_us = esp_timer_get_time() - t0;
    unlink(chemin);
    heap_caps_free(bloc);
    banc_lire_espace_sd();

    const uint64_t octets = (uint64_t)ecrits * SD_BLOC;
    const uint32_t ko_s = total_us > 0 ? (uint32_t)(octets * 1000ULL / (uint64_t)total_us) : 0;  /* ko/s (10^3) */
    const nid_capture_settings_t *r = banc_reglages_capture();
    const uint32_t video_ko_s = r->bitrate_kbps / 8u + 8u;     /* vidéo + son */
    const uint32_t marge = video_ko_s ? ko_s / video_ko_s : 0;
    ESP_LOGI(TAG, "BILAN sd_test taille=%" PRIu64 "Mo resultat=%s debit=%" PRIu32 ".%02" PRIu32 "Mo/s latence_max=%"
             PRIu32 "ms blocs_lents(>%dms)=%" PRIu32 " fsync=%" PRIu32 "ms bloc=64ko marge_video=x%" PRIu32,
             octets / 1000000ULL, err == ESP_OK ? "OK" : "ECHEC", ko_s / 1000, (ko_s % 1000) / 10, max_us / 1000,
             SD_LENT_MS, lents, fsync_us / 1000, marge);
    nid_storage_journal("test_sd_banc", "%" PRIu64 " Mo;%" PRIu32 " ko/s;latence max %" PRIu32 " ms;%" PRIu32
                        " blocs > %d ms", octets / 1000000ULL, ko_s, max_us / 1000, lents, SD_LENT_MS);

    banc_etat_t *e = banc_etat_verrouiller();
    e->sd_test_fait = true;
    e->sd_debit_ko_s = ko_s;
    e->sd_latence_max_ms = max_us / 1000;
    e->sd_blocs_lents = lents;
    e->sd_test_mo = (uint32_t)(octets / 1000000ULL);
    banc_etat_deverrouiller();

    if (err != ESP_OK) {
        banc_afficher_erreur("test SD", err, "Écriture interrompue : carte pleine, retirée ou défectueuse ?");
        return;
    }
    const bool bon = (marge >= 4 && max_us / 1000 < 500);
    banc_ecran_ligne(BANC_LIGNE_ETAT, bon ? BANC_VERT : BANC_JAUNE, "Carte SD : %" PRIu32 ".%02" PRIu32
                     " Mo/s en écriture, latence max %" PRIu32 " ms", ko_s / 1000, (ko_s % 1000) / 10, max_us / 1000);
    banc_ecran_ligne(BANC_LIGNE_DETAIL1, BANC_BLANC, "%" PRIu64 " Mo en blocs de 64 ko, %" PRIu32
                     " bloc(s) de plus de %d ms, fsync %" PRIu32 " ms", octets / 1000000ULL, lents, SD_LENT_MS,
                     fsync_us / 1000);
    banc_ecran_ligne(BANC_LIGNE_DETAIL2, marge >= 4 ? BANC_BLANC : BANC_JAUNE, "Marge : %" PRIu32
                     " fois le débit de la vidéo (%" PRIu32 " ko/s) ; viser au moins 4", marge, video_ko_s);
}

void banc_sd_scan(void)
{
    banc_effacer_details();
    banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_JAUNE, "%s", "Recomptage de l'espace libre (lecture de toute la FAT)...");
    esp_err_t err = banc_preparer_sd();
    nid_storage_volume_t v;
    const int64_t t0 = esp_timer_get_time();
    if (err == ESP_OK) {
        err = nid_storage_get_volume(&v, true);
    }
    const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (err != ESP_OK) {
        banc_afficher_erreur("carte SD", err, "Carte SD absente ou illisible (FAT32)");
        return;
    }
    banc_lire_espace_sd();
    ESP_LOGI(TAG, "BILAN sd_volume fat=%u volume=%" PRIu64 "Mo libre=%" PRIu64 "Mo carte=%" PRIu64 "Mo grappe=%" PRIu32
             "ko debut=%" PRIu32 " recomptage=%" PRIu32 "ms", v.fat_type, v.volume_bytes / 1000000ULL,
             v.free_bytes / 1000000ULL, v.card_bytes / 1000000ULL, v.cluster_bytes / 1024u, v.first_sector, ms);
    const bool petit = v.card_bytes > 0 && v.volume_bytes < v.card_bytes / 10 * 9;
    banc_ecran_ligne(BANC_LIGNE_ETAT, petit ? BANC_JAUNE : BANC_VERT, "Volume FAT%u : %" PRIu64 " Mo libres sur %"
                     PRIu64 " Mo (recompté en %" PRIu32 " ms)", v.fat_type, v.free_bytes / 1000000ULL,
                     v.volume_bytes / 1000000ULL, ms);
    banc_ecran_ligne(BANC_LIGNE_DETAIL1, BANC_BLANC, "Carte %" PRIu64 " Mo, grappes de %" PRIu32
                     " ko, volume au secteur %" PRIu32, v.card_bytes / 1000000ULL, v.cluster_bytes / 1024u,
                     v.first_sector);
    if (petit) {
        banc_ecran_ligne(BANC_LIGNE_DETAIL2, BANC_JAUNE, "%s",
                         "Le volume ne couvre pas la carte : plusieurs partitions ? Reformater en une seule FAT32");
    }
}
