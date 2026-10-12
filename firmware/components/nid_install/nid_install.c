/*
 * nid_install — mode installation : Wi-Fi local, aperçu et réglages sur smartphone
 *
 * Tâche principale (nid_install_run) : voyant, bouton, fin automatique et
 * mesures affichées par la page. Elle nourrit le chien de garde toutes les
 * 50 ms et ne bloque jamais longtemps. Le Wi-Fi (coprocesseur ESP32-C6 +
 * point d'accès) et le serveur web sont démarrés puis arrêtés par une tâche
 * dédiée aux délais bornés : le bouton et la fin automatique restent actifs
 * pendant le démarrage du Wi-Fi, même si le coprocesseur ne répond pas.
 *
 * Voyant (uniquement pendant l'installation, FS1.3 / FC1.1) :
 *   clignotement lent (1 Hz)      démarrage du Wi-Fi en cours
 *   allumé fixe                   Wi-Fi prêt : réseau « NID-… », http://192.168.4.1
 *   double éclat toutes les 2 s   Wi-Fi indisponible : bouton seulement
 *   éteint pendant l'appui        appui en cours (relâcher avant 3 s → démarrer)
 *   clignotement rapide           appui ≥ 3 s (relâcher → pause maintenance)
 */
#include "nid_install.h"

#include <inttypes.h>
#include <string.h>
#include <time.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "nid_board.h"
#include "nid_clock.h"
#include "nid_detect.h"
#include "nid_install_priv.h"
#include "nid_ir.h"
#include "nid_power.h"
#include "nid_storage.h"

static const char *TAG = "nid_install";

#define LOOP_MS              50
#define DEBOUNCE_SAMPLES     2       /* 2 × 50 ms */
#define SHORT_PRESS_MIN_MS   60
#define LONG_PRESS_MS        3000
#define EXIT_GRACE_MS        1500    /* la page affiche la confirmation avant l'arrêt du Wi-Fi */
#define WIFI_STOP_WAIT_MS    30000
#define WIFI_START_WAIT_MS   (CONFIG_NID_INSTALL_WIFI_START_TIMEOUT_S * 1000)
#define BATT_PERIOD_MS       5000
#define TEMP_PERIOD_MS       10000
#define SD_PERIOD_MS         30000
#define NIGHT_PERIOD_MS      5000
#define DEFAULT_TIMEOUT_S    (15 * 60)
#define WIFI_TASK_STACK      6144
#define WIFI_TASK_PRIO       4

#define EV_WIFI_UP      BIT0
#define EV_WIFI_FAILED  BIT1
#define EV_STOP_REQ     BIT2
#define EV_STOPPED      BIT3

typedef enum {
    LED_STARTING,
    LED_WIFI,
    LED_NO_WIFI,
    LED_PRESS,
    LED_LONG,
} led_mode_t;

typedef enum {
    BTN_NONE,
    BTN_SHORT,
    BTN_LONG,
} btn_event_t;

typedef struct {
    bool    armed;     /*!< relâché au moins une fois depuis l'entrée */
    bool    pressed;   /*!< état filtré */
    uint8_t count;     /*!< échantillons consécutifs différents de l'état filtré */
    int64_t since_ms;  /*!< début de l'appui en cours */
} button_t;

typedef struct {
    int64_t batt;
    int64_t temp;
    int64_t sd;
    int64_t night;
} sample_due_t;

static inst_shared_t     s_sh;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static char              s_ssid[NID_CONFIG_SSID_LEN];
static char              s_pass[NID_CONFIG_PASS_LEN];
static char              s_fail_reason[128];

inst_shared_t *inst_shared(void)
{
    return &s_sh;
}

void inst_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void inst_unlock(void)
{
    xSemaphoreGive(s_lock);
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *end_name(inst_end_t end)
{
    switch (end) {
    case INST_END_MAINTENANCE:
        return "pause maintenance";
    case INST_END_TIMEOUT:
        return "durée maximale atteinte";
    default:
        return "démarrage de la surveillance";
    }
}

/* -------------------------------------------------------------------------- */
/* Tâche Wi-Fi : démarrage, attente de la fin, arrêt                          */
/* -------------------------------------------------------------------------- */

static void wifi_task(void *arg)
{
    EventGroupHandle_t ev = (EventGroupHandle_t)arg;
    bool up = false;

    esp_err_t err = inst_wifi_start(s_ssid, s_pass);
    if (err != ESP_OK) {
        strlcpy(s_fail_reason, inst_wifi_error(), sizeof(s_fail_reason));
    } else if (xEventGroupGetBits(ev) & EV_STOP_REQ) {
        strlcpy(s_fail_reason, "point d'accès prêt après le délai maximal", sizeof(s_fail_reason));
    } else if ((err = inst_web_start()) != ESP_OK) {
        snprintf(s_fail_reason, sizeof(s_fail_reason), "serveur web indisponible (%s)", esp_err_to_name(err));
    } else {
        up = true;
    }
    xEventGroupSetBits(ev, up ? EV_WIFI_UP : EV_WIFI_FAILED);

    if (up) {
        inst_wifi_query_cp_version();
        char cp[INST_CP_LEN];
        inst_lock();
        strlcpy(cp, s_sh.cp_version, sizeof(cp));
        inst_unlock();
        nid_storage_journal("wifi", "point d'accès « %s » prêt (canal %d), coprocesseur ESP-Hosted %s",
                            s_ssid, CONFIG_NID_INSTALL_WIFI_CHANNEL, cp);
        xEventGroupWaitBits(ev, EV_STOP_REQ, pdFALSE, pdTRUE, portMAX_DELAY);
    }

    /* Arrêt dans l'ordre inverse ; le C6 est remis en reset dans tous les cas. */
    inst_web_stop();
    inst_wifi_stop();
    xEventGroupSetBits(ev, EV_STOPPED);
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/* Voyant, bouton, mesures                                                    */
/* -------------------------------------------------------------------------- */

static bool led_level(led_mode_t mode, int64_t t_ms)
{
    switch (mode) {
    case LED_STARTING:
        return (t_ms % 1000) < 500;
    case LED_WIFI:
        return true;
    case LED_NO_WIFI: {
        int64_t p = t_ms % 2000;
        return p < 150 || (p >= 300 && p < 450);
    }
    case LED_LONG:
        return (t_ms % 200) < 100;
    case LED_PRESS:
    default:
        return false;
    }
}

/**
 * Filtre le bouton (anti-rebond 100 ms) et détecte la fin d'un appui.
 * Un appui en cours à l'entrée (réveil par le bouton) est ignoré : le bouton
 * n'est actif qu'après avoir été relâché.
 */
static btn_event_t button_poll(button_t *b, int64_t t_ms, bool *long_held)
{
    btn_event_t ev = BTN_NONE;
    bool raw = nid_detect_button_pressed();

    if (raw != b->pressed) {
        if (++b->count >= DEBOUNCE_SAMPLES) {
            b->count = 0;
            b->pressed = raw;
            if (raw) {
                b->since_ms = t_ms;
            } else if (!b->armed) {
                b->armed = true;
            } else {
                int64_t d = t_ms - b->since_ms;
                if (d >= LONG_PRESS_MS) {
                    ev = BTN_LONG;
                } else if (d >= SHORT_PRESS_MIN_MS) {
                    ev = BTN_SHORT;
                }
            }
        }
    } else {
        b->count = 0;
    }
    *long_held = b->armed && b->pressed && (t_ms - b->since_ms) >= LONG_PRESS_MS;
    return ev;
}

/** Mesures affichées par la page, à des cadences adaptées à chaque capteur. */
static void sample_status(sample_due_t *due, int64_t t, bool force)
{
    uint32_t low_mv = 0;
    uint32_t crit_mv = 0;
    nid_config_t cfg = { 0 };
    bool have_cfg = false;
    bool do_night = force || t >= due->night;

    inst_lock();
    if (s_sh.cfg != NULL) {
        low_mv = s_sh.cfg->batt_low_mv;
        crit_mv = s_sh.cfg->batt_critical_mv;
        if (do_night) {
            cfg = *s_sh.cfg;
            have_cfg = true;
        }
    }
    bool do_sd = force || s_sh.sd_refresh || t >= due->sd;
    s_sh.sd_refresh = false;
    inst_unlock();

    bool pir = nid_detect_pir_active();

    bool do_batt = force || t >= due->batt;
    uint32_t mv = 0;
    if (do_batt) {
        if (nid_power_battery_read_mv(&mv) != ESP_OK) {
            mv = 0;
        }
        due->batt = t + BATT_PERIOD_MS;
    }

    bool do_temp = force || t >= due->temp;
    float temp = 0.0f;
    bool temp_ok = false;
    if (do_temp) {
        temp_ok = (nid_clock_get_temperature(&temp) == ESP_OK);
        due->temp = t + TEMP_PERIOD_MS;
    }

    bool mounted = false;
    uint64_t total = 0;
    uint64_t free_b = 0;
    if (do_sd) {
        mounted = nid_storage_is_mounted();
        if (mounted && nid_storage_get_space(&total, &free_b) != ESP_OK) {
            total = 0;
            free_b = 0;
        }
        due->sd = t + SD_PERIOD_MS;
    }

    bool night = false;
    if (do_night) {
        night = have_cfg && nid_ir_is_night(&cfg, time(NULL));
        due->night = t + NIGHT_PERIOD_MS;
    }

    inst_lock();
    s_sh.pir = pir;
    if (do_batt) {
        s_sh.batt_mv = mv;
        s_sh.batt_level = (int)nid_power_battery_level(mv, low_mv, crit_mv);
    }
    if (do_temp) {
        s_sh.temp_ok = temp_ok;
        s_sh.temp_c = temp;
    }
    if (do_sd) {
        s_sh.sd_mounted = mounted;
        s_sh.sd_total = total;
        s_sh.sd_free = free_b;
    }
    if (do_night) {
        s_sh.night = night;
    }
    inst_unlock();
}

static void wait_feeding(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += LOOP_MS) {
        nid_power_watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}

static bool wait_bits_feeding(EventGroupHandle_t ev, EventBits_t bits, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += LOOP_MS) {
        nid_power_watchdog_feed();
        if ((xEventGroupWaitBits(ev, bits, pdFALSE, pdTRUE, pdMS_TO_TICKS(LOOP_MS)) & bits) == bits) {
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/* Point d'entrée                                                             */
/* -------------------------------------------------------------------------- */

nid_install_result_t nid_install_run(nid_install_ctx_t *ctx)
{
    if (ctx == NULL || ctx->cfg == NULL) {
        ESP_LOGE(TAG, "contexte d'installation invalide");
        nid_board_coproc_enable(false);
        return NID_INSTALL_EXIT_NO_WIFI;
    }
    ctx->config_changed = false;
    ctx->time_changed = false;
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }

    const int64_t start_ms = now_ms();
    const uint32_t timeout_s = ctx->timeout_s ? ctx->timeout_s : DEFAULT_TIMEOUT_S;

    nid_config_effective_ssid(ctx->cfg, s_ssid, sizeof(s_ssid));
    if (s_ssid[0] == '\0') {
        strlcpy(s_ssid, "NID-CAM", sizeof(s_ssid));
    }
    strlcpy(s_pass, ctx->cfg->wifi_password, sizeof(s_pass));
    size_t pass_len = strlen(s_pass);
    if (pass_len < 8 || pass_len > 63) {
        strlcpy(s_pass, "moqueur-nid", sizeof(s_pass));
        nid_storage_journal("alerte", "mot de passe Wi-Fi invalide (%u caractères) : « moqueur-nid » utilisé",
                            (unsigned)pass_len);
    }
    s_fail_reason[0] = '\0';

    inst_lock();
    memset(&s_sh, 0, sizeof(s_sh));
    s_sh.active = true;
    s_sh.accept_actions = true;
    s_sh.cfg = ctx->cfg;
    s_sh.config_path = ctx->config_path ? ctx->config_path : NID_CONFIG_FILE;
    strlcpy(s_sh.fw_version, ctx->fw_version ? ctx->fw_version : "?", sizeof(s_sh.fw_version));
    s_sh.start_us = start_ms * 1000;
    s_sh.timeout_s = timeout_s;
    s_sh.batt_level = NID_BATT_UNKNOWN;
    strlcpy(s_sh.ssid, s_ssid, sizeof(s_sh.ssid));
    strlcpy(s_sh.cp_version, "?", sizeof(s_sh.cp_version));
    inst_unlock();

    ESP_LOGI(TAG, "mode installation : Wi-Fi « %s », fin automatique dans %" PRIu32 " s", s_ssid, timeout_s);
    nid_board_status_led(true);

    /* Wi-Fi et serveur web dans leur propre tâche. */
    EventGroupHandle_t ev = xEventGroupCreate();
    bool wifi_task_ok = ev != NULL &&
                        xTaskCreate(wifi_task, "nid_wifi", WIFI_TASK_STACK, ev, WIFI_TASK_PRIO, NULL) == pdPASS;
    bool wifi_up = false;
    bool no_wifi = !wifi_task_ok;
    if (!wifi_task_ok) {
        if (ev != NULL) {
            vEventGroupDelete(ev);
            ev = NULL;
        }
        ESP_LOGE(TAG, "tâche Wi-Fi impossible : installation sans Wi-Fi");
        nid_storage_journal("erreur", "Wi-Fi d'installation indisponible : mémoire insuffisante");
    }

    sample_due_t due = { 0 };
    sample_status(&due, now_ms(), true);

    button_t btn = { .pressed = nid_detect_button_pressed() };
    btn.armed = !btn.pressed;
    bool led_on = true;
    inst_end_t end = INST_END_NONE;

    while (end == INST_END_NONE) {
        nid_power_watchdog_feed();
        const int64_t t = now_ms();
        const int64_t elapsed = t - start_ms;
        sample_status(&due, t, false);

        /* Évolution du Wi-Fi */
        if (wifi_task_ok) {
            EventBits_t bits = xEventGroupGetBits(ev);
            if (!wifi_up && !no_wifi) {
                if (bits & EV_WIFI_UP) {
                    wifi_up = true;
                } else if (bits & EV_WIFI_FAILED) {
                    no_wifi = true;
                    nid_storage_journal("erreur", "Wi-Fi d'installation indisponible : %s", s_fail_reason);
                } else if (elapsed >= WIFI_START_WAIT_MS) {
                    no_wifi = true;
                    xEventGroupSetBits(ev, EV_STOP_REQ);
                    nid_storage_journal("erreur", "Wi-Fi d'installation : point d'accès non prêt après %d s",
                                        CONFIG_NID_INSTALL_WIFI_START_TIMEOUT_S);
                }
                if (no_wifi) {
                    ESP_LOGW(TAG, "installation sans Wi-Fi : appui court = démarrer, appui long = maintenance");
                }
            } else if (wifi_up && inst_wifi_link_lost()) {
                wifi_up = false;
                no_wifi = true;
                xEventGroupSetBits(ev, EV_STOP_REQ);
                nid_storage_journal("erreur", "liaison avec le coprocesseur Wi-Fi perdue pendant l'installation");
                ESP_LOGW(TAG, "Wi-Fi perdu : appui court = démarrer, appui long = maintenance");
            }
        }

        /* Bouton du boîtier */
        bool long_held = false;
        btn_event_t be = button_poll(&btn, t, &long_held);
        if (be == BTN_SHORT) {
            end = INST_END_START;
        } else if (be == BTN_LONG) {
            end = INST_END_MAINTENANCE;
        }

        /* Page web */
        if (end == INST_END_NONE) {
            inst_lock();
            end = s_sh.web_request;
            inst_unlock();
        }

        /* Fin automatique (FC1.6) */
        if (end == INST_END_NONE && elapsed >= (int64_t)timeout_s * 1000) {
            end = INST_END_TIMEOUT;
        }

        /* Voyant */
        led_mode_t mode;
        if (btn.armed && btn.pressed) {
            mode = long_held ? LED_LONG : LED_PRESS;
        } else if (wifi_up) {
            mode = LED_WIFI;
        } else if (no_wifi) {
            mode = LED_NO_WIFI;
        } else {
            mode = LED_STARTING;
        }
        bool level = led_level(mode, elapsed);
        if (level != led_on) {
            nid_board_status_led(level);
            led_on = level;
        }

        if (end == INST_END_NONE) {
            vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
        }
    }

    /* ---- Sortie : la page affiche la fin, puis tout est arrêté ---- */
    ESP_LOGI(TAG, "fin du mode installation : %s", end_name(end));
    inst_lock();
    s_sh.ending = end;
    s_sh.accept_actions = false;
    inst_unlock();
    nid_board_status_led(true);

    if (wifi_up && !inst_wifi_link_lost()) {
        wait_feeding(EXIT_GRACE_MS);
    }
    if (wifi_task_ok) {
        xEventGroupSetBits(ev, EV_STOP_REQ);
        if (wait_bits_feeding(ev, EV_STOPPED, WIFI_STOP_WAIT_MS)) {
            vEventGroupDelete(ev);
        } else {
            /* Tâche bloquée dans un appel au coprocesseur : son groupe
             * d'événements est laissé en place, le C6 est forcé en reset. */
            ESP_LOGE(TAG, "arrêt du Wi-Fi bloqué : coprocesseur forcé en reset");
            nid_storage_journal("erreur", "arrêt du Wi-Fi d'installation bloqué : coprocesseur forcé en reset");
        }
    }

    inst_web_camera_idle();          /* IR éteint, caméra remise en mode jour */
    nid_board_coproc_enable(false);  /* garantie : C6 en reset quel que soit le chemin */

    inst_lock();
    s_sh.active = false;
    ctx->config_changed = s_sh.config_changed;
    ctx->time_changed = s_sh.time_changed;
    inst_unlock();

    nid_board_status_led(false);
    nid_power_watchdog_feed();

    switch (end) {
    case INST_END_MAINTENANCE:
        return NID_INSTALL_EXIT_MAINTENANCE;
    case INST_END_TIMEOUT:
        return wifi_up ? NID_INSTALL_EXIT_TIMEOUT : NID_INSTALL_EXIT_NO_WIFI;
    default:
        return wifi_up ? NID_INSTALL_EXIT_START : NID_INSTALL_EXIT_NO_WIFI;
    }
}
