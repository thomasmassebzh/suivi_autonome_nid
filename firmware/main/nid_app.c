/*
 * nid_app — logique du système : réveil → action → veille
 *
 * Mode terrain (veille profonde entre deux événements, FC3.1/FC3.2) :
 *
 *   mise sous tension ──► SD, réglages, horloge, journal ──► mode installation
 *        (ou RESET)          (aperçu Wi-Fi, réglages, heure) ──► photo de mise en
 *                            service ──► voyant « en fonctionnement » ──► veille
 *
 *   réveil PIR ──► caméra   ┐ en parallèle : validation PIR, SD, horloge,
 *                           ┘ batterie ──► séquence MP4 ──► délai ──► veille
 *
 *   réveil minuterie ──► photo de suivi si due / fin du délai ──► veille
 *
 *   réveil bouton ──► mode installation (cadrage, réglages, maintenance)
 *
 *   redémarrage sur défaut (chien de garde, panique, chute de tension)
 *        ──► journal ──► reprise de la surveillance sans installation (FC3.3)
 *
 * Mode banc d'essai : même logique, sans veille profonde (événements par
 * interruption, caméra maintenue prête).
 */
#include "nid_app.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "nid_board.h"
#include "nid_capture.h"
#include "nid_clock.h"
#include "nid_config.h"
#include "nid_detect.h"
#include "nid_install.h"
#include "nid_ir.h"
#include "nid_power.h"
#include "nid_state.h"
#include "nid_storage.h"

static const char *TAG = "nid_app";

#if CONFIG_NID_MODE_BENCH
#define BENCH_MODE 1
#else
#define BENCH_MODE 0
#endif

#define PREP_DONE_BIT            BIT0
#define PREP_TASK_STACK          6144
#define PREP_TIMEOUT_MS          5000
#define PIR_CONFIRM_TIMEOUT_MS   1500   /* fenêtre d'observation du PIR après le réveil */
#define PIR_IDLE_TIMEOUT_MS      15000  /* attente du retour au repos du PIR avant la veille */
#define PIR_REARM_RETRY_S        30
#define PIR_STUCK_PAUSE_S        60
#define PHOTO_EARLY_S            30     /* une photo due dans moins de 30 s est prise tout de suite */
#define BATT_CHECK_PERIOD_US     (5 * 1000 * 1000)
#define BATT_SAG_COUNT           2      /* mesures successives sous le seuil pendant l'enregistrement */

/* ------------------------------------------------------------------------- */
/* Outils                                                                    */
/* ------------------------------------------------------------------------- */

static int64_t now_utc(void)
{
    return (int64_t)time(NULL);
}

static const char *fw_version(void)
{
    return esp_app_get_description()->version;
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:
        return "mise sous tension";
    case ESP_RST_EXT:
        return "broche de reset";
    case ESP_RST_SW:
        return "logiciel";
    case ESP_RST_PANIC:
        return "panique";
    case ESP_RST_INT_WDT:
        return "chien de garde (interruptions)";
    case ESP_RST_TASK_WDT:
        return "chien de garde (tâches)";
    case ESP_RST_WDT:
        return "chien de garde";
    case ESP_RST_DEEPSLEEP:
        return "réveil de veille";
    case ESP_RST_BROWNOUT:
        return "chute de tension";
    default:
        return "autre";
    }
}

/** Durée de séquence (ms) : config.ini, sauf en mode banc si une durée est imposée. */
static uint32_t sequence_duration_ms(const nid_config_t *cfg)
{
#if CONFIG_NID_MODE_BENCH
    if (CONFIG_NID_BENCH_SEQ_DURATION_S > 0) {
        return CONFIG_NID_BENCH_SEQ_DURATION_S * 1000u;
    }
#endif
    return (uint32_t)cfg->seq_duration_s * 1000u;
}

static void capture_settings(const nid_config_t *cfg, bool night, bool with_audio, nid_capture_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->width = cfg->video_width;
    s->height = cfg->video_height;
    s->fps = cfg->video_fps;
    s->bitrate_kbps = cfg->video_bitrate_kbps;
    s->audio = with_audio && cfg->audio_enabled;
    s->night = night;
    s->night_mono = cfg->night_monochrome;
    s->jpeg_quality = cfg->jpeg_quality;
    snprintf(s->label, sizeof(s->label), "%s %s", cfg->system_id, cfg->nest_id);
}

/** Mesure la batterie, met à jour l'état et le contexte du journal. @return mV (0 = inconnue). */
static uint32_t read_battery(void)
{
    nid_state_t *st = nid_state();
    uint32_t mv = 0;
    if (nid_power_battery_read_mv(&mv) != ESP_OK) {
        mv = 0;
    }
    st->last_batt_mv = mv;
    st->batt_level = (uint8_t)nid_power_battery_level(mv, st->cfg.batt_low_mv, st->cfg.batt_critical_mv);
    float temp = NAN;
    if (nid_clock_get_temperature(&temp) != ESP_OK) {
        temp = NAN;
    }
    nid_storage_journal_context(mv ? (int32_t)mv : -1, temp);
    return mv;
}

/** Monte la carte SD et pose les identifiants. */
static bool storage_up(void)
{
    if (nid_storage_mount() != ESP_OK) {
        ESP_LOGE(TAG, "carte SD indisponible");
        return false;
    }
    nid_storage_set_ids(nid_state()->cfg.system_id, nid_state()->cfg.nest_id);
    return true;
}

/**
 * Monte la SD et (re)lit config.ini ; crée le fichier avec les valeurs
 * par défaut s'il est absent. Applique le fuseau horaire.
 */
static bool storage_up_with_config(void)
{
    nid_state_t *st = nid_state();
    if (nid_storage_mount() != ESP_OK) {
        ESP_LOGE(TAG, "carte SD indisponible : réglages par défaut");
        nid_clock_set_timezone(st->cfg.timezone);
        return false;
    }
    nid_config_t cfg;
    nid_config_set_defaults(&cfg);
    esp_err_t err = nid_config_load(&cfg, NID_CONFIG_FILE);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "%s absent : création avec les valeurs par défaut", NID_CONFIG_FILE);
        nid_config_save(&cfg, NID_CONFIG_FILE);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "lecture de %s impossible (%s) : valeurs par défaut", NID_CONFIG_FILE, esp_err_to_name(err));
    }
    nid_config_validate(&cfg);
    st->cfg = cfg;
    st->cfg_loaded = (err == ESP_OK);
    nid_clock_set_timezone(st->cfg.timezone);
    nid_storage_set_ids(st->cfg.system_id, st->cfg.nest_id);
    return true;
}

/** Libère caméra, éclairage et carte SD (sauf en mode banc où tout reste prêt). */
static void release_resources(void)
{
    nid_ir_off();
#if !BENCH_MODE
    nid_capture_deinit();
    nid_storage_unmount();
#endif
}

/* ------------------------------------------------------------------------- */
/* Photos de suivi (FP1.5)                                                   */
/* ------------------------------------------------------------------------- */

static bool photo_due(int64_t now)
{
    const nid_state_t *st = nid_state();
    return st->monitoring && !st->maintenance && st->cfg.photo_interval_min > 0 &&
           st->next_photo_utc != 0 && now >= st->next_photo_utc - PHOTO_EARLY_S;
}

/** Planifie la prochaine photo sur la grille de l'intervalle (rattrape les créneaux manqués). */
static void schedule_next_photo(int64_t now)
{
    nid_state_t *st = nid_state();
    int64_t iv = (int64_t)st->cfg.photo_interval_min * 60;
    if (iv <= 0) {
        st->next_photo_utc = 0;
        return;
    }
    if (st->next_photo_utc == 0 || st->next_photo_utc > now + iv) {
        st->next_photo_utc = now + iv;
        return;
    }
    if (st->next_photo_utc <= now) {
        int64_t late = now - st->next_photo_utc;
        st->next_photo_utc += (late / iv + 1) * iv;
    }
}

/** Prend une photo (capture déjà initialisée), la finalise et planifie la suivante. */
static void take_photo(bool night, const char *reason)
{
    nid_state_t *st = nid_state();
    int64_t now = now_utc();
    nid_media_file_t f;
    esp_err_t err = nid_storage_media_begin(NID_MEDIA_PHOTO, (time_t)now, ++st->photo_seq, &f);
    if (err == ESP_OK) {
        err = nid_capture_photo(f.tmp_path);
        if (err == ESP_OK) {
            err = nid_storage_media_commit(&f);
        } else {
            nid_storage_media_abort(&f, false);
        }
    }
    if (err == ESP_OK) {
        nid_storage_journal("photo", "%s;%s;%s", f.name, reason, night ? "nuit" : "jour");
    } else {
        nid_storage_journal("erreur", "photo (%s) : %s", reason, esp_err_to_name(err));
    }
    schedule_next_photo(now);
}

/* ------------------------------------------------------------------------- */
/* Arrêt sur batterie critique (FC3.4)                                       */
/* ------------------------------------------------------------------------- */

static void critical_shutdown(const char *where) __attribute__((noreturn));
static void critical_shutdown(const char *where)
{
    nid_state_t *st = nid_state();
    ESP_LOGW(TAG, "batterie critique (%" PRIu32 " mV) : arrêt propre", st->last_batt_mv);
    nid_ir_off();
    nid_capture_deinit();
    nid_storage_journal("batterie_critique", "%s : %" PRIu32 " mV, arrêt propre", where, st->last_batt_mv);
    nid_storage_unmount();
    st->batt_level = NID_BATT_CRITICAL;
    nid_state_commit();
    nid_power_shutdown(CONFIG_NID_BATT_RECHECK_S);
}

/* ------------------------------------------------------------------------- */
/* Préparation parallèle au démarrage de la caméra (réveil PIR)              */
/* ------------------------------------------------------------------------- */

typedef struct {
    EventGroupHandle_t done;
    uint32_t pir_min_ms;
    bool     pir_ok;
    bool     sd_ok;
    bool     rtc_valid;
} prep_ctx_t;

static void prep_task(void *arg)
{
    prep_ctx_t *p = (prep_ctx_t *)arg;
    /* 1. Validation du déclenchement : le signal est observé tout de suite. */
    p->pir_ok = nid_detect_pir_confirm(p->pir_min_ms, PIR_CONFIRM_TIMEOUT_MS);
    /* 2. Carte SD, heure exacte (DS3231), batterie. */
    p->sd_ok = storage_up();
    p->rtc_valid = false;
    nid_clock_sync_from_rtc(&p->rtc_valid);
    read_battery();
    xEventGroupSetBits(p->done, PREP_DONE_BIT);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------------- */
/* Séquence vidéo sur détection (FP1.1, FP1.4)                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    int64_t last_check_us;
    int     low_count;
    bool    critical;
} rec_ctx_t;

/** Rappelé ~5 fois par seconde pendant l'enregistrement. */
static bool record_stop_cb(void *arg)
{
    rec_ctx_t *rc = (rec_ctx_t *)arg;
    nid_power_watchdog_feed();
    int64_t t = esp_timer_get_time();
    if (t - rc->last_check_us < BATT_CHECK_PERIOD_US) {
        return false;
    }
    rc->last_check_us = t;
    uint32_t mv = 0;
    if (nid_power_battery_read_mv(&mv) == ESP_OK && mv != 0) {
        nid_state()->last_batt_mv = mv;
        if (mv < nid_state()->cfg.batt_critical_mv) {
            rc->low_count++;
        } else {
            rc->low_count = 0;
        }
        if (rc->low_count >= BATT_SAG_COUNT) {
            rc->critical = true;
            return true;
        }
    }
    return false;
}

static void record_sequences(bool night)
{
    nid_state_t *st = nid_state();
    const nid_config_t *cfg = &st->cfg;
    int chained = 0;
    esp_err_t err;
    bool critical = false;

    do {
        int64_t start = now_utc();
        nid_media_file_t f;
        err = nid_storage_media_begin(NID_MEDIA_VIDEO, (time_t)start, ++st->video_seq, &f);
        if (err != ESP_OK) {
            nid_storage_journal("erreur", "préparation du fichier vidéo : %s", esp_err_to_name(err));
            break;
        }
        rec_ctx_t rc = { .last_check_us = esp_timer_get_time() };
        nid_capture_stats_t stats = { 0 };
        err = nid_capture_record(f.tmp_path, sequence_duration_ms(cfg), record_stop_cb, &rc, &stats);
        if (err == ESP_OK) {
            err = nid_storage_media_commit(&f);
        }
        if (err == ESP_OK) {
            nid_storage_journal("video", "%s;%" PRIu32 " ms;%" PRIu32 " images;%" PRIu32 " perdues;%s;image 1 à %" PRIu32 " ms du réveil",
                                f.name, stats.duration_ms, stats.video_frames, stats.dropped_frames,
                                night ? "nuit" : "jour", stats.first_frame_ms);
        } else {
            nid_storage_media_abort(&f, true);
            nid_storage_journal("erreur", "vidéo %s : %s", f.name, esp_err_to_name(err));
        }
        chained++;
        if (rc.critical) {
            critical = true;
            break;
        }
        if (cfg->trigger_delay_s > 0) {
            /* FP1.4 : délai minimal entre deux déclenchements. */
            st->cooldown_until_utc = now_utc() + cfg->trigger_delay_s;
            break;
        }
        /* Délai désactivé : on enchaîne tant que l'animal est détecté. */
    } while (err == ESP_OK && nid_detect_pir_active() && chained < CONFIG_NID_MAX_CHAINED_SEQUENCES);

    if (critical) {
        read_battery();
        critical_shutdown("enregistrement");
    }
    if (cfg->trigger_delay_s == 0 && chained >= CONFIG_NID_MAX_CHAINED_SEQUENCES && nid_detect_pir_active()) {
        nid_storage_journal("alerte", "PIR toujours actif après %d séquences : pause de %d s", chained, PIR_STUCK_PAUSE_S);
        st->cooldown_until_utc = now_utc() + PIR_STUCK_PAUSE_S;
    }
}

static void handle_pir(void)
{
    nid_state_t *st = nid_state();
    const nid_config_t *cfg = &st->cfg;
    int64_t now = now_utc();

    if (!st->monitoring || st->maintenance) {
        ESP_LOGW(TAG, "détection ignorée : surveillance inactive");
        return;
    }
    if (now < st->cooldown_until_utc || now < st->pir_armed_utc) {
        ESP_LOGI(TAG, "détection ignorée : délai entre déclenchements ou chauffe du PIR");
        return;
    }
    const bool low_batt = (st->batt_level == NID_BATT_LOW);

    /* Préparation en parallèle : validation PIR, SD, horloge, batterie. */
    prep_ctx_t prep = {
        .done = xEventGroupCreate(),
        .pir_min_ms = nid_detect_min_active_ms(cfg->pir_sensitivity),
    };
    if (prep.done == NULL ||
        xTaskCreate(prep_task, "nid_prep", PREP_TASK_STACK, &prep, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "tâche de préparation impossible");
        if (prep.done) {
            vEventGroupDelete(prep.done);
        }
        return;
    }

    /* Pendant ce temps : décision jour/nuit sur l'heure système et démarrage de la caméra. */
    bool night = nid_ir_is_night(cfg, (time_t)now);
    esp_err_t cam_err = ESP_ERR_INVALID_STATE;
    if (!low_batt) {
        nid_ir_apply(night, cfg);
        nid_capture_settings_t cs;
        capture_settings(cfg, night, true, &cs);
        cam_err = nid_capture_init(&cs);
    }

    EventBits_t bits = xEventGroupWaitBits(prep.done, PREP_DONE_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(PREP_TIMEOUT_MS));
    if (!(bits & PREP_DONE_BIT)) {
        /* La tâche référence `prep` : on ne peut pas sortir de la fonction sans elle. */
        ESP_LOGE(TAG, "préparation bloquée : redémarrage");
        nid_power_restart();
    }
    vEventGroupDelete(prep.done);

    if (st->batt_level == NID_BATT_CRITICAL) {
        critical_shutdown("détection");
    }
    if (!prep.sd_ok) {
        ESP_LOGE(TAG, "pas de carte SD : séquence impossible");
        nid_ir_off();
        return;
    }
    if (!prep.pir_ok) {
        nid_storage_journal("fausse_detection", "PIR actif moins de %" PRIu32 " ms (sensibilité %u)",
                            prep.pir_min_ms, cfg->pir_sensitivity);
        nid_ir_off();
        return;
    }
    if (low_batt) {
        nid_storage_journal("detection", "batterie basse (%" PRIu32 " mV) : pas de vidéo", st->last_batt_mv);
        return;
    }
    if (cam_err != ESP_OK) {
        nid_storage_journal("erreur", "caméra : %s", esp_err_to_name(cam_err));
        nid_ir_off();
        return;
    }

    record_sequences(night);

    /* Photo de suivi due : la caméra est déjà prête. */
    if (photo_due(now_utc())) {
        take_photo(night, "suivi");
    }
    nid_ir_off();
}

/* ------------------------------------------------------------------------- */
/* Réveil par minuterie : photo de suivi, fin de délai, contrôle batterie     */
/* ------------------------------------------------------------------------- */

static void handle_timer(void)
{
    nid_state_t *st = nid_state();

    if (st->batt_level == NID_BATT_CRITICAL) {
        /* Contrôle après un arrêt critique (rechargement solaire possible). */
        bool sd = storage_up();
        uint32_t mv = read_battery();
        /* Hystérésis : reprise seulement au-dessus du seuil bas. */
        if (mv != 0 && mv < st->cfg.batt_low_mv) {
            critical_shutdown("contrôle périodique");
        }
        st->batt_level = NID_BATT_OK;
        if (sd) {
            nid_storage_journal("reprise_batterie", "%" PRIu32 " mV : reprise de la surveillance", mv);
        }
    }
    if (!st->monitoring || st->maintenance) {
        return;
    }
    int64_t now = now_utc();
    if (!photo_due(now)) {
        return; /* fin de délai entre déclenchements : rien d'autre à faire */
    }
    if (!storage_up()) {
        schedule_next_photo(now);
        return;
    }
    bool rtc_valid = false;
    nid_clock_sync_from_rtc(&rtc_valid);
    read_battery();
    if (st->batt_level == NID_BATT_CRITICAL) {
        critical_shutdown("photo de suivi");
    }
    now = now_utc();
    bool night = nid_ir_is_night(&st->cfg, (time_t)now);
    nid_ir_apply(night, &st->cfg);
    nid_capture_settings_t cs;
    capture_settings(&st->cfg, night, false, &cs);
    esp_err_t err = nid_capture_init(&cs);
    if (err == ESP_OK) {
        take_photo(night, "suivi");
    } else {
        nid_storage_journal("erreur", "caméra (photo de suivi) : %s", esp_err_to_name(err));
        schedule_next_photo(now);
    }
    nid_ir_off();
}

/* ------------------------------------------------------------------------- */
/* Installation et mise en service (FS1.2, FS1.3, FS1.4)                     */
/* ------------------------------------------------------------------------- */

static void start_monitoring(bool from_power_on)
{
    nid_state_t *st = nid_state();
    int64_t now = now_utc();
    st->monitoring = 1;
    st->maintenance = 0;
    st->monitoring_since_utc = now;
    st->cooldown_until_utc = 0;
    st->next_photo_utc = 0;
    /* Le PIR est alimenté depuis la mise sous tension : sa chauffe court depuis le démarrage. */
    int64_t since_boot_s = esp_timer_get_time() / 1000000;
    int64_t warmup_left = from_power_on ? (int64_t)st->cfg.pir_warmup_s - since_boot_s : 0;
    st->pir_armed_utc = now + (warmup_left > 0 ? warmup_left : 0);

    /* Photo de mise en service : référence du cadrage dans les données. */
    if (nid_storage_is_mounted() && nid_capture_is_ready()) {
        bool night = nid_ir_is_night(&st->cfg, (time_t)now);
        nid_ir_apply(night, &st->cfg);
        nid_capture_set_night(night);
        take_photo(night, "mise_en_service");
        nid_ir_off();
    } else {
        schedule_next_photo(now);
    }
    nid_storage_journal("surveillance", "démarrée : séquence %u s, délai %u s, photo %u min, son %s, sensibilité %u, %ux%u à %u img/s",
                        st->cfg.seq_duration_s, st->cfg.trigger_delay_s, st->cfg.photo_interval_min,
                        st->cfg.audio_enabled ? "oui" : "non", st->cfg.pir_sensitivity,
                        st->cfg.video_width, st->cfg.video_height, st->cfg.video_fps);
    nid_state_commit();
    /* FS1.3 (F0) : indication claire « en fonctionnement » avant de quitter le nid. */
    nid_board_status_blink(5, 150, 150);
}

static void run_installation(bool from_power_on)
{
    nid_state_t *st = nid_state();

    /* Caméra prête pour l'aperçu (de jour ; la page permet d'allumer l'IR). */
    nid_capture_settings_t cs;
    capture_settings(&st->cfg, false, false, &cs);
    esp_err_t cam_err = nid_capture_init(&cs);
    if (cam_err != ESP_OK) {
        nid_storage_journal("erreur", "caméra (installation) : %s", esp_err_to_name(cam_err));
    }

    nid_install_ctx_t ctx = {
        .cfg = &st->cfg,
        .config_path = NID_CONFIG_FILE,
        .fw_version = fw_version(),
        .timeout_s = (uint32_t)st->cfg.install_timeout_min * 60u,
    };
    char ssid[NID_CONFIG_SSID_LEN];
    nid_config_effective_ssid(&st->cfg, ssid, sizeof(ssid));
    nid_storage_journal("installation", "début (Wi-Fi « %s »)", ssid);
    nid_install_result_t res = nid_install_run(&ctx);
    nid_power_watchdog_feed();
    nid_ir_off();

    if (ctx.time_changed) {
        char buf[32];
        nid_clock_format_local((time_t)now_utc(), "%Y-%m-%d %H:%M:%S", buf, sizeof(buf));
        nid_storage_journal("heure_reglee", "%s (depuis le téléphone)", buf);
    }
    if (ctx.config_changed) {
        nid_config_validate(&st->cfg);
        nid_clock_set_timezone(st->cfg.timezone);
        nid_storage_set_ids(st->cfg.system_id, st->cfg.nest_id);
        nid_storage_journal("config_modifiee", "réglages enregistrés dans %s", NID_CONFIG_FILE);
    }

    switch (res) {
    case NID_INSTALL_EXIT_MAINTENANCE:
        st->monitoring = 0;
        st->maintenance = 1;
        nid_storage_journal("maintenance", "pause : la carte SD et la batterie peuvent être retirées");
        nid_capture_deinit();
        nid_storage_unmount();
        nid_state_commit();
        nid_board_status_blink(2, 600, 300);
        return;
    case NID_INSTALL_EXIT_TIMEOUT:
        nid_storage_journal("installation", "fin automatique après %u min", st->cfg.install_timeout_min);
        break;
    case NID_INSTALL_EXIT_NO_WIFI:
        nid_storage_journal("installation", "Wi-Fi indisponible : mise en service sans aperçu");
        break;
    default:
        nid_storage_journal("installation", "terminée par l'opérateur");
        break;
    }
    /* Une résolution modifiée ne s'applique qu'à la prochaine initialisation. */
    if (ctx.config_changed) {
        nid_capture_deinit();
        capture_settings(&st->cfg, false, false, &cs);
        nid_capture_init(&cs);
    }
    start_monitoring(from_power_on);
}

/* ------------------------------------------------------------------------- */
/* Démarrages                                                                 */
/* ------------------------------------------------------------------------- */

static void log_startup(const nid_boot_info_t *boot, bool rtc_valid, int recovered)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint64_t total = 0, free_b = 0;
    nid_storage_get_space(&total, &free_b);
    nid_storage_journal("demarrage", "firmware %s;puce rév. %d.%d;%s;horloge %s%s;SD %" PRIu64 "/%" PRIu64 " Mo libres",
                        fw_version(), chip.revision / 100, chip.revision % 100,
                        reset_reason_str(boot->reset_reason), nid_clock_model_str(),
                        rtc_valid ? "" : " (heure perdue)",
                        free_b / (1024 * 1024), total / (1024 * 1024));
    if (recovered > 0) {
        nid_storage_journal("fichiers_incomplets", "%d fichier(s) interrompu(s) renommé(s) en _INCOMPLET", recovered);
    }
    if (!nid_clock_time_is_valid()) {
        nid_storage_journal("alerte", "heure non réglée : la régler depuis la page d'installation");
    }
}

static void handle_power_on(const nid_boot_info_t *boot)
{
    nid_state_t *st = nid_state();
    st->monitoring = 0;
    st->maintenance = 0;

    bool sd = storage_up_with_config();
    bool rtc_valid = false;
    nid_clock_sync_from_rtc(&rtc_valid);
    int recovered = sd ? nid_storage_recover() : 0;
    read_battery();
    log_startup(boot, rtc_valid, recovered);
    if (st->batt_level == NID_BATT_CRITICAL) {
        critical_shutdown("démarrage");
    }
#if CONFIG_NID_INSTALL_ON_POWER_ON
    run_installation(true);
#else
    start_monitoring(true);
#endif
}

static void handle_button(const nid_boot_info_t *boot)
{
    nid_state_t *st = nid_state();
    /* La carte a pu être changée ou config.ini modifié sur un ordinateur. */
    bool sd = storage_up_with_config();
    bool rtc_valid = false;
    nid_clock_sync_from_rtc(&rtc_valid);
    int recovered = sd ? nid_storage_recover() : 0;
    read_battery();
    if (st->maintenance) {
        log_startup(boot, rtc_valid, recovered);
        nid_storage_journal("maintenance", "fin de la pause");
    } else if (recovered > 0) {
        nid_storage_journal("fichiers_incomplets", "%d fichier(s) renommé(s) en _INCOMPLET", recovered);
    }
    if (st->batt_level == NID_BATT_CRITICAL) {
        critical_shutdown("bouton");
    }
    run_installation(false);
}

static void handle_fault(const nid_boot_info_t *boot, bool state_restored)
{
    nid_state_t *st = nid_state();
    bool sd = state_restored ? storage_up() : storage_up_with_config();
    bool rtc_valid = false;
    nid_clock_sync_from_rtc(&rtc_valid);
    int recovered = sd ? nid_storage_recover() : 0;
    read_battery();
    nid_storage_journal("reprise_defaut", "redémarrage après : %s", reset_reason_str(boot->reset_reason));
    if (recovered > 0) {
        nid_storage_journal("fichiers_incomplets", "%d fichier(s) interrompu(s) renommé(s) en _INCOMPLET", recovered);
    }
    if (st->batt_level == NID_BATT_CRITICAL) {
        critical_shutdown("reprise");
    }
    /* FC3.3 : reprise ≤ 5 min, sans attendre d'opérateur. */
    if (!state_restored || (!st->monitoring && !st->maintenance)) {
        start_monitoring(false);
    } else if (photo_due(now_utc())) {
        handle_timer();
    }
}

/* ------------------------------------------------------------------------- */
/* Veille                                                                     */
/* ------------------------------------------------------------------------- */

static void go_to_sleep(void) __attribute__((noreturn));
static void go_to_sleep(void)
{
    nid_state_t *st = nid_state();
    nid_sleep_plan_t plan = { .wake_on_button = true };
    int64_t now = now_utc();
    int64_t next = INT64_MAX;

    bool active = st->monitoring && !st->maintenance && st->batt_level != NID_BATT_CRITICAL;
    if (active) {
        if (st->next_photo_utc) {
            next = st->next_photo_utc;
        }
        bool pir_blocked = false;
        if (now < st->cooldown_until_utc) {
            next = (st->cooldown_until_utc < next) ? st->cooldown_until_utc : next;
            pir_blocked = true;
        }
        if (now < st->pir_armed_utc) {
            next = (st->pir_armed_utc < next) ? st->pir_armed_utc : next;
            pir_blocked = true;
        }
        if (!pir_blocked) {
            /* Sortie PIR active pendant la veille = réveil immédiat : attendre le repos. */
            if (nid_detect_wait_pir_idle(PIR_IDLE_TIMEOUT_MS) == ESP_OK) {
                plan.wake_on_pir = true;
            } else {
                ESP_LOGW(TAG, "PIR toujours actif : réarmement dans %d s", PIR_REARM_RETRY_S);
                int64_t retry = now_utc() + PIR_REARM_RETRY_S;
                next = (retry < next) ? retry : next;
            }
        }
    }
    if (next != INT64_MAX) {
        int64_t delta = next - now_utc();
        plan.sleep_us = (uint64_t)(delta < 1 ? 1 : delta) * 1000000ULL;
    }

    release_resources();
    nid_state_commit();
    ESP_LOGI(TAG, "veille : PIR %s, bouton %s, minuterie %" PRIu64 " s",
             plan.wake_on_pir ? "oui" : "non", plan.wake_on_button ? "oui" : "non", plan.sleep_us / 1000000ULL);
    nid_power_sleep(&plan);
}

/* ------------------------------------------------------------------------- */
/* Mode banc d'essai                                                          */
/* ------------------------------------------------------------------------- */

#if BENCH_MODE
static void bench_loop(void)
{
    QueueHandle_t q = xQueueCreate(8, sizeof(nid_detect_evt_t));
    if (q == NULL || nid_detect_enable_events(q) != ESP_OK) {
        ESP_LOGE(TAG, "mode banc : interruptions indisponibles");
    }
    ESP_LOGI(TAG, "mode banc : en attente d'événements (PIR, bouton, photo de suivi)");
    for (;;) {
        nid_detect_evt_t evt;
        nid_power_watchdog_feed();
        if (q && xQueueReceive(q, &evt, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (evt == NID_DETECT_EVT_PIR) {
                ESP_LOGI(TAG, "front PIR");
                handle_pir();
            } else if (evt == NID_DETECT_EVT_BUTTON) {
                nid_detect_disable_events();
                run_installation(false);
                nid_detect_enable_events(q);
            }
            nid_state_commit();
        }
        if (photo_due(now_utc())) {
            handle_timer();
            nid_state_commit();
        }
    }
}
#endif

/* ------------------------------------------------------------------------- */
/* Point d'entrée                                                             */
/* ------------------------------------------------------------------------- */

void nid_app_run(void)
{
    /* 1. Rien ne doit éclairer ni bruiter (FC1.1, FC1.2). */
    nid_board_init_early();

    /* 2. État conservé et réglages en mémoire (pas de lecture SD sur le chemin critique). */
    bool restored = nid_state_restore();
    nid_state_t *st = nid_state();
    st->boot_count++;
    nid_clock_set_timezone(st->cfg.timezone);

    /* 3. Services de base. */
    nid_power_watchdog_start(CONFIG_NID_WDT_TIMEOUT_S);
    nid_detect_init();
    nid_ir_init();
    nid_power_battery_init();
    if (nid_board_i2c_init() == ESP_OK) {
        nid_clock_init(nid_board_i2c_bus());
    }

    /* 4. Cause du démarrage. */
    nid_boot_info_t boot;
    nid_power_get_boot_info(&boot);
    ESP_LOGI(TAG, "firmware %s, démarrage n°%" PRIu32 " : %s", fw_version(), st->boot_count, nid_power_wake_str(boot.reason));

    switch (boot.reason) {
    case NID_WAKE_PIR:
        if (restored) {
            handle_pir();
        } else {
            handle_power_on(&boot);
        }
        break;
    case NID_WAKE_TIMER:
    case NID_WAKE_RTC_ALARM:
        if (restored) {
            handle_timer();
        } else {
            handle_fault(&boot, false);
        }
        break;
    case NID_WAKE_BUTTON:
        handle_button(&boot);
        break;
    case NID_WAKE_FAULT:
        handle_fault(&boot, restored);
        break;
    case NID_WAKE_POWER_ON:
    default:
        handle_power_on(&boot);
        break;
    }

#if BENCH_MODE
    bench_loop();
#else
    go_to_sleep();
#endif
}
