/*
 * nid_install — serveur web d'installation : page, API JSON, aperçu MJPEG
 *
 * http://192.168.4.1 :
 *   GET  /                 page d'installation (www/index.html, embarquée)
 *   GET  /stream           aperçu MJPEG (multipart/x-mixed-replace), un téléphone à la fois
 *   GET  /photo            dernière photo de test (JPEG)
 *   GET  /api/status       état (JSON)
 *   GET  /api/config       réglages (JSON, clés de config.ini)
 *   POST /api/config       réglages modifiés (application/x-www-form-urlencoded)
 *   POST /api/time         epoch=<s> : mise à l'heure depuis le téléphone
 *   POST /api/ir           on=1|0 : éclairage IR pour le cadrage de nuit
 *   POST /api/photo        photo de test enregistrée sur la carte SD
 *   POST /api/start        démarrer la surveillance
 *   POST /api/maintenance  pause maintenance (changement de carte ou de batterie)
 *
 * Le gestionnaire /stream confie la requête (httpd_req_async_handler_begin)
 * à la tâche d'aperçu : le serveur reste disponible pour la page. Un seul
 * téléphone reçoit l'aperçu ; le même téléphone (page rechargée) le reprend.
 * La photo de test suspend l'aperçu (arrêt du flux, prise de vue, reprise).
 */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "sdkconfig.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "nid_capture.h"
#include "nid_clock.h"
#include "nid_config.h"
#include "nid_install_priv.h"
#include "nid_ir.h"
#include "nid_power.h"
#include "nid_storage.h"

static const char *TAG = "nid_install";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

#define HTTP_400  "400 Bad Request"
#define HTTP_404  "404 Not Found"
#define HTTP_409  "409 Conflict"
#define HTTP_413  "413 Content Too Large"
#define HTTP_500  "500 Internal Server Error"
#define HTTP_503  "503 Service Unavailable"

#define HTTPD_SOCKETS        5       /* + 3 internes ≤ CONFIG_LWIP_MAX_SOCKETS */
#define FORM_MAX_LEN         2048
#define SMALL_BODY_MAX       128
#define STATUS_JSON_LEN      1536
#define CONFIG_JSON_LEN      2048
#define ERRORS_JSON_LEN      1024
#define RESPONSE_JSON_LEN    (CONFIG_JSON_LEN + ERRORS_JSON_LEN + 512)
#define MAX_REPORTED_ERRORS  10
#define MAX_KEY_LEN          40
#define PHOTO_CHUNK          4096
#define TIME_MIN_EPOCH       1735689600LL  /* 2025-01-01 */
#define TIME_MAX_EPOCH       4102444800LL  /* 2100-01-01 */

#define STREAM_TASK_STACK    6144
#define STREAM_TASK_PRIO     4
#define STREAM_BOUNDARY      "nidimage"
#define FRAME_TIMEOUT_MS     1000
#define FRAME_FAIL_MAX       5
#define PAUSE_WAIT_MS        3000
#define STREAM_STOP_WAIT_MS  8000
#define CAM_LOCK_WAIT_MS     3000

static httpd_handle_t    s_server;
static SemaphoreHandle_t s_cam_lock;       /* appels à nid_capture / nid_ir */
static StaticSemaphore_t s_cam_lock_buf;
static SemaphoreHandle_t s_stream_lock;    /* état de la tâche d'aperçu ci-dessous */
static StaticSemaphore_t s_stream_lock_buf;
static TaskHandle_t      s_stream_task;
static bool              s_stream_running;
static bool              s_session_on;
static httpd_req_t      *s_pending;
static struct sockaddr_storage s_pending_peer;
static struct sockaddr_storage s_session_peer;
static atomic_bool       s_quit;
static atomic_bool       s_pause_req;
static atomic_bool       s_paused;

static bool session_active(void);

/* -------------------------------------------------------------------------- */
/* Construction de JSON                                                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   full;
} sb_t;

static void sb_init(sb_t *b, char *buf, size_t cap)
{
    b->buf = buf;
    b->cap = cap;
    b->len = 0;
    b->full = (cap == 0);
    if (cap) {
        buf[0] = '\0';
    }
}

static void sb_printf(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(sb_t *b, const char *fmt, ...)
{
    if (b->full) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) {
        b->full = true;
        b->buf[b->len] = '\0';
        return;
    }
    b->len += (size_t)n;
}

static void sb_putc(sb_t *b, char c)
{
    if (b->full || b->len + 1 >= b->cap) {
        b->full = true;
        return;
    }
    b->buf[b->len++] = c;
    b->buf[b->len] = '\0';
}

/** Chaîne JSON entre guillemets (caractères spéciaux échappés). */
static void sb_json_str(sb_t *b, const char *s)
{
    sb_putc(b, '"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') {
            sb_putc(b, '\\');
            sb_putc(b, (char)*p);
        } else if (*p < 0x20) {
            sb_printf(b, "\\u%04x", (unsigned)*p);
        } else {
            sb_putc(b, (char)*p);
        }
    }
    sb_putc(b, '"');
}

static const char *json_bool(bool v)
{
    return v ? "true" : "false";
}

/* -------------------------------------------------------------------------- */
/* Réponses et lecture des requêtes                                          */
/* -------------------------------------------------------------------------- */

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    if (status) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/** Réponse {"ok":…,"message":…}. */
static esp_err_t send_result(httpd_req_t *req, const char *status, bool ok, const char *message)
{
    char buf[320];
    sb_t b;
    sb_init(&b, buf, sizeof(buf));
    sb_printf(&b, "{\"ok\":%s,\"message\":", json_bool(ok));
    sb_json_str(&b, message);
    sb_putc(&b, '}');
    if (b.full) {
        return send_json(req, HTTP_500, "{\"ok\":false,\"message\":\"Réponse trop longue\"}");
    }
    return send_json(req, status, buf);
}

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *text)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, text);
}

/** Corps de la requête (terminé par '\0'), NULL si trop long ou illisible. */
static char *recv_body(httpd_req_t *req, size_t max_len, bool *too_long)
{
    *too_long = req->content_len > max_len;
    if (*too_long) {
        return NULL;
    }
    size_t len = req->content_len;
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        return NULL;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += (size_t)r;
    }
    buf[len] = '\0';
    return buf;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/** Décodage URL en place (« + » → espace, %XX → octet). */
static void url_decode(char *s)
{
    char *out = s;
    for (const char *p = s; *p; p++) {
        if (*p == '+') {
            *out++ = ' ';
        } else if (p[0] == '%' && p[1] && p[2] && hex_digit(p[1]) >= 0 && hex_digit(p[2]) >= 0) {
            *out++ = (char)(hex_digit(p[1]) * 16 + hex_digit(p[2]));
            p += 2;
        } else {
            *out++ = *p;
        }
    }
    *out = '\0';
}

/** Valeur d'un champ d'un petit formulaire (« clé=valeur&… »), décodée. */
static bool form_value(httpd_req_t *req, const char *key, char *val, size_t len)
{
    bool too_long = false;
    char *body = recv_body(req, SMALL_BODY_MAX, &too_long);
    if (body == NULL) {
        return false;
    }
    bool ok = httpd_query_key_value(body, key, val, len) == ESP_OK;
    free(body);
    if (ok) {
        url_decode(val);
    }
    return ok;
}

/** true si la page peut encore agir (fin non décidée). */
static bool actions_allowed(void)
{
    inst_lock();
    bool ok = inst_shared()->active && inst_shared()->accept_actions;
    inst_unlock();
    return ok;
}

static esp_err_t send_closing(httpd_req_t *req)
{
    return send_result(req, HTTP_409, false, "Le mode installation se termine.");
}

/** Copie des réglages courants (false si le mode installation est terminé). */
static bool copy_cfg(nid_config_t *out)
{
    inst_lock();
    const inst_shared_t *sh = inst_shared();
    bool ok = sh->active && sh->cfg != NULL;
    if (ok) {
        *out = *sh->cfg;
    }
    inst_unlock();
    return ok;
}

static bool cam_lock(void)
{
    return s_cam_lock != NULL && xSemaphoreTake(s_cam_lock, pdMS_TO_TICKS(CAM_LOCK_WAIT_MS)) == pdTRUE;
}

static void cam_unlock(void)
{
    xSemaphoreGive(s_cam_lock);
}

/* -------------------------------------------------------------------------- */
/* Page                                                                       */
/* -------------------------------------------------------------------------- */

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* EMBED_TXTFILES ajoute un '\0' final, non envoyé. */
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

/* -------------------------------------------------------------------------- */
/* État                                                                       */
/* -------------------------------------------------------------------------- */

static const char *batt_level_str(int level)
{
    switch (level) {
    case NID_BATT_OK:
        return "ok";
    case NID_BATT_LOW:
        return "basse";
    case NID_BATT_CRITICAL:
        return "critique";
    default:
        return "inconnue";
    }
}

static const char *end_str(inst_end_t end)
{
    switch (end) {
    case INST_END_START:
        return "demarrage";
    case INST_END_MAINTENANCE:
        return "maintenance";
    case INST_END_TIMEOUT:
        return "delai";
    default:
        return "";
    }
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static esp_err_t status_get(httpd_req_t *req)
{
    inst_shared_t sh;
    nid_night_mode_t mode = NID_NIGHT_MODE_AUTO;
    uint8_t ir_pct = 0;
    char ident[2 * NID_CONFIG_ID_LEN + 4] = "";

    inst_lock();
    sh = *inst_shared();
    if (sh.cfg != NULL) {
        mode = sh.cfg->night_mode;
        ir_pct = sh.cfg->ir_power_pct;
        snprintf(ident, sizeof(ident), "%s / %s", sh.cfg->system_id, sh.cfg->nest_id);
    }
    inst_unlock();

    time_t now = time(NULL);
    char heure[32];
    if (nid_clock_format_local(now, "%Y-%m-%d %H:%M:%S", heure, sizeof(heure)) == 0) {
        strlcpy(heure, "?", sizeof(heure));
    }
    int64_t elapsed_s = (esp_timer_get_time() - sh.start_us) / 1000000;
    int64_t left_s = (int64_t)sh.timeout_s - elapsed_s;
    if (left_s < 0) {
        left_s = 0;
    }

    char *buf = malloc(STATUS_JSON_LEN);
    if (buf == NULL) {
        return send_result(req, HTTP_503, false, "Mémoire insuffisante");
    }
    sb_t b;
    sb_init(&b, buf, STATUS_JSON_LEN);
    sb_printf(&b, "{\"heure\":");
    sb_json_str(&b, heure);
    sb_printf(&b, ",\"epoch\":%lld,\"heure_valide\":%s", (long long)now, json_bool(nid_clock_time_is_valid()));
    sb_printf(&b, ",\"batt_mv\":%" PRIu32 ",\"batt_niveau\":\"%s\"", sh.batt_mv, batt_level_str(sh.batt_level));
    sb_printf(&b, ",\"sd\":%s,\"sd_total\":%" PRIu64 ",\"sd_libre\":%" PRIu64,
              json_bool(sh.sd_mounted), sh.sd_total, sh.sd_free);
    sb_printf(&b, ",\"camera\":");
    sb_json_str(&b, nid_capture_sensor_name());
    sb_printf(&b, ",\"camera_prete\":%s,\"rtc\":", json_bool(nid_capture_is_ready()));
    sb_json_str(&b, nid_clock_model_str());
    if (sh.temp_ok) {
        int t10 = (int)(sh.temp_c * 10.0f + (sh.temp_c >= 0.0f ? 0.5f : -0.5f));
        int a10 = t10 < 0 ? -t10 : t10;
        sb_printf(&b, ",\"temp\":%s%d.%d", t10 < 0 ? "-" : "", a10 / 10, a10 % 10);
    } else {
        sb_printf(&b, ",\"temp\":null");
    }
    sb_printf(&b, ",\"pir\":%s,\"nuit\":%s,\"mode_nuit\":\"%s\",\"ir\":%s,\"puissance_ir\":%u",
              json_bool(sh.pir), json_bool(sh.night), nid_config_night_mode_str(mode), json_bool(sh.ir_on),
              (unsigned)ir_pct);
    sb_printf(&b, ",\"clients\":%d,\"ssid\":", sh.clients);
    sb_json_str(&b, sh.ssid);
    sb_printf(&b, ",\"ident\":");
    sb_json_str(&b, ident);
    sb_printf(&b, ",\"firmware\":");
    sb_json_str(&b, sh.fw_version);
    sb_printf(&b, ",\"coproc\":");
    sb_json_str(&b, sh.cp_version);
    sb_printf(&b, ",\"apercu\":%s,\"reste_s\":%lld,\"duree_s\":%" PRIu32 ",\"fin\":\"%s\",\"photo\":",
              json_bool(session_active()), (long long)left_s, sh.timeout_s, end_str(sh.ending));
    sb_json_str(&b, sh.last_photo[0] ? base_name(sh.last_photo) : "");
    sb_putc(&b, '}');

    esp_err_t err = b.full ? send_result(req, HTTP_500, false, "État trop long") : send_json(req, NULL, buf);
    free(buf);
    return err;
}

/* -------------------------------------------------------------------------- */
/* Réglages                                                                   */
/* -------------------------------------------------------------------------- */

static esp_err_t config_get(httpd_req_t *req)
{
    nid_config_t cfg;
    if (!copy_cfg(&cfg)) {
        return send_closing(req);
    }
    char *buf = malloc(CONFIG_JSON_LEN);
    if (buf == NULL) {
        return send_result(req, HTTP_503, false, "Mémoire insuffisante");
    }
    size_t n = nid_config_to_json(&cfg, buf, CONFIG_JSON_LEN);
    esp_err_t err = n ? send_json(req, NULL, buf) : send_result(req, HTTP_500, false, "Réglages illisibles");
    free(buf);
    return err;
}

/** Ajoute « "clé":"message" » à la liste des erreurs (entrée retirée si elle ne tient pas). */
static void add_error(sb_t *errs, int *count, const char *key, const char *message)
{
    if (*count < MAX_REPORTED_ERRORS && !errs->full) {
        size_t mark = errs->len;
        char k[MAX_KEY_LEN + 1];
        strlcpy(k, key, sizeof(k));
        if (mark > 0) {
            sb_putc(errs, ',');
        }
        sb_json_str(errs, k);
        sb_putc(errs, ':');
        sb_json_str(errs, message);
        if (errs->full) {
            errs->len = mark;  /* reste « plein » : plus aucune entrée */
            errs->buf[mark] = '\0';
        }
    }
    (*count)++;
}

static esp_err_t config_post(httpd_req_t *req)
{
    if (!actions_allowed()) {
        return send_closing(req);
    }
    bool too_long = false;
    char *body = recv_body(req, FORM_MAX_LEN, &too_long);
    if (body == NULL) {
        return send_result(req, too_long ? HTTP_413 : HTTP_400, false,
                           too_long ? "Formulaire trop long." : "Formulaire illisible.");
    }

    nid_config_t before;
    nid_config_t work;
    if (!copy_cfg(&before)) {
        free(body);
        return send_closing(req);
    }
    work = before;

    char *errs_buf = malloc(ERRORS_JSON_LEN);
    char *resp = malloc(RESPONSE_JSON_LEN);
    if (errs_buf == NULL || resp == NULL) {
        free(body);
        free(errs_buf);
        free(resp);
        return send_result(req, HTTP_503, false, "Mémoire insuffisante");
    }
    sb_t errs;
    sb_init(&errs, errs_buf, ERRORS_JSON_LEN);
    int nerr = 0;
    int applied = 0;

    char *save = NULL;
    for (char *pair = strtok_r(body, "&", &save); pair != NULL; pair = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(pair, '=');
        char *val = pair + strlen(pair);  /* « clé » seule : valeur vide */
        if (eq != NULL) {
            *eq = '\0';
            val = eq + 1;
        }
        url_decode(pair);
        url_decode(val);
        if (pair[0] == '\0') {
            continue;
        }
        esp_err_t e = nid_config_set(&work, pair, val);
        if (e == ESP_OK) {
            applied++;
        } else {
            add_error(&errs, &nerr, pair, e == ESP_ERR_NOT_FOUND ? "clé inconnue" : "valeur refusée");
        }
    }
    free(body);

    bool corrected = nid_config_validate(&work) != ESP_OK;
    bool changed = memcmp(&work, &before, sizeof(work)) != 0;
    bool closing = false;
    const char *path = NID_CONFIG_FILE;
    esp_err_t save_err = ESP_OK;

    if (changed) {
        inst_lock();
        inst_shared_t *sh = inst_shared();
        if (sh->active && sh->accept_actions && sh->cfg != NULL) {
            *sh->cfg = work;
            sh->config_changed = true;
            sh->sd_refresh = true;
            path = sh->config_path;
        } else {
            closing = true;
        }
        inst_unlock();
    }
    if (closing) {
        free(errs_buf);
        free(resp);
        return send_closing(req);
    }
    if (changed) {
        if (strcmp(work.timezone, before.timezone) != 0) {
            nid_clock_set_timezone(work.timezone);  /* heure locale de la page à jour */
        }
        save_err = nid_config_save(&work, path);
        if (save_err != ESP_OK) {
            nid_storage_journal("erreur", "enregistrement des réglages depuis la page : %s", esp_err_to_name(save_err));
        }
        ESP_LOGI(TAG, "réglages modifiés depuis la page (%d valeur(s), enregistrement : %s)",
                 applied, esp_err_to_name(save_err));
    }

    char message[160];
    if (!changed && nerr == 0) {
        strlcpy(message, "Aucune modification.", sizeof(message));
    } else if (changed && save_err != ESP_OK) {
        snprintf(message, sizeof(message), "Réglages appliqués mais non enregistrés sur la carte SD (%s).",
                 esp_err_to_name(save_err));
    } else if (nerr > 0) {
        strlcpy(message, changed ? "Réglages enregistrés, sauf les valeurs refusées."
                                 : "Valeurs refusées : rien n'a été modifié.", sizeof(message));
    } else if (corrected) {
        strlcpy(message, "Réglages enregistrés ; certaines valeurs ont été corrigées.", sizeof(message));
    } else {
        strlcpy(message, "Réglages enregistrés.", sizeof(message));
    }

    sb_t b;
    sb_init(&b, resp, RESPONSE_JSON_LEN);
    sb_printf(&b, "{\"ok\":%s,\"appliques\":%d,\"enregistre\":%s,\"corrige\":%s,\"message\":",
              json_bool(nerr == 0 && save_err == ESP_OK), applied, json_bool(changed && save_err == ESP_OK),
              json_bool(corrected));
    sb_json_str(&b, message);
    sb_printf(&b, ",\"erreurs\":{%s},\"config\":", errs_buf);
    free(errs_buf);
    if (!b.full) {
        size_t n = nid_config_to_json(&work, resp + b.len, b.cap - b.len - 1);
        if (n == 0) {
            sb_printf(&b, "{}");
        } else {
            b.len += n;
        }
    }
    sb_putc(&b, '}');
    esp_err_t err = b.full ? send_result(req, HTTP_500, false, message) : send_json(req, NULL, resp);
    free(resp);
    return err;
}

/* -------------------------------------------------------------------------- */
/* Heure, éclairage IR                                                        */
/* -------------------------------------------------------------------------- */

static esp_err_t time_post(httpd_req_t *req)
{
    if (!actions_allowed()) {
        return send_closing(req);
    }
    char val[24] = "";
    if (!form_value(req, "epoch", val, sizeof(val))) {
        return send_result(req, HTTP_400, false, "Heure du téléphone absente.");
    }
    char *end = NULL;
    long long epoch = strtoll(val, &end, 10);
    if (end == val || epoch < TIME_MIN_EPOCH || epoch > TIME_MAX_EPOCH) {
        return send_result(req, HTTP_400, false, "Heure du téléphone invalide.");
    }

    esp_err_t err = nid_clock_set_time((time_t)epoch);
    long long diff = (long long)time(NULL) - epoch;
    bool system_ok = diff >= -5 && diff <= 5;
    if (system_ok) {
        inst_lock();
        inst_shared()->time_changed = true;
        inst_unlock();
    }

    char heure[32];
    if (nid_clock_format_local(time(NULL), "%d/%m/%Y %H:%M:%S", heure, sizeof(heure)) == 0) {
        strlcpy(heure, "?", sizeof(heure));
    }
    char message[160];
    if (!system_ok) {
        snprintf(message, sizeof(message), "Réglage de l'heure impossible (%s).", esp_err_to_name(err));
    } else if (err != ESP_OK) {
        snprintf(message, sizeof(message), "Heure réglée : %s (horloge RTC non mise à jour : %s).",
                 heure, esp_err_to_name(err));
    } else {
        snprintf(message, sizeof(message), "Heure réglée : %s.", heure);
    }
    ESP_LOGI(TAG, "%s", message);
    return send_result(req, NULL, system_ok, message);
}

static esp_err_t ir_post(httpd_req_t *req)
{
    if (!actions_allowed()) {
        return send_closing(req);
    }
    char val[8] = "";
    if (!form_value(req, "on", val, sizeof(val))) {
        return send_result(req, HTTP_400, false, "Paramètre « on » absent.");
    }
    bool on = strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0 || strcasecmp(val, "oui") == 0;
    nid_config_t cfg;
    if (!copy_cfg(&cfg)) {
        return send_closing(req);
    }
    if (!cam_lock()) {
        return send_result(req, HTTP_503, false, "Caméra occupée, réessayer.");
    }
    nid_ir_apply(on, &cfg);
    esp_err_t err = nid_capture_set_night(on);
    cam_unlock();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bascule jour/nuit de la caméra : %s", esp_err_to_name(err));
    }
    inst_lock();
    inst_shared()->ir_on = on;
    inst_unlock();

    const char *message;
    if (!on) {
        message = "Éclairage infrarouge éteint.";
    } else if (cfg.ir_power_pct == 0) {
        message = "Puissance IR réglée à 0 % (réglage « puissance_ir ») : aucun éclairage.";
    } else {
        message = "Éclairage infrarouge allumé (invisible à l'œil).";
    }
    char buf[256];
    sb_t b;
    sb_init(&b, buf, sizeof(buf));
    sb_printf(&b, "{\"ok\":true,\"ir\":%s,\"message\":", json_bool(on));
    sb_json_str(&b, message);
    sb_putc(&b, '}');
    return send_json(req, NULL, buf);
}

/* -------------------------------------------------------------------------- */
/* Photo de test                                                              */
/* -------------------------------------------------------------------------- */

static bool session_active(void)
{
    xSemaphoreTake(s_stream_lock, portMAX_DELAY);
    bool on = s_session_on;
    xSemaphoreGive(s_stream_lock);
    return on;
}

static esp_err_t photo_post(httpd_req_t *req)
{
    if (!actions_allowed()) {
        return send_closing(req);
    }
    if (!nid_capture_is_ready()) {
        return send_result(req, HTTP_503, false, "Caméra indisponible : photo impossible.");
    }
    if (!nid_storage_is_mounted()) {
        return send_result(req, HTTP_503, false, "Carte SD absente : photo impossible.");
    }

    /* Suspension de l'aperçu pendant la prise de vue. */
    atomic_store(&s_pause_req, true);
    int64_t t0 = esp_timer_get_time();
    while (session_active() && !atomic_load(&s_paused) &&
           esp_timer_get_time() - t0 < (int64_t)PAUSE_WAIT_MS * 1000) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    nid_media_file_t f;
    esp_err_t err = nid_storage_media_begin(NID_MEDIA_PHOTO, time(NULL), 0, &f);
    if (err == ESP_OK) {
        if (cam_lock()) {
            err = nid_capture_photo(f.tmp_path);
            cam_unlock();
        } else {
            err = ESP_ERR_TIMEOUT;
        }
        if (err == ESP_OK) {
            err = nid_storage_media_commit(&f);
        } else {
            nid_storage_media_abort(&f, false);
        }
    }
    atomic_store(&s_pause_req, false);

    char message[160];
    if (err == ESP_OK) {
        inst_lock();
        strlcpy(inst_shared()->last_photo, f.final_path, sizeof(inst_shared()->last_photo));
        inst_shared()->sd_refresh = true;
        inst_unlock();
        nid_storage_journal("photo_test", "%s", f.name);
        snprintf(message, sizeof(message), "Photo enregistrée : %s.", f.name);
        ESP_LOGI(TAG, "photo de test : %s", f.final_path);
    } else {
        nid_storage_journal("erreur", "photo de test : %s", esp_err_to_name(err));
        snprintf(message, sizeof(message), "Échec de la photo de test (%s).", esp_err_to_name(err));
    }
    return send_result(req, err == ESP_OK ? NULL : HTTP_500, err == ESP_OK, message);
}

static esp_err_t photo_get(httpd_req_t *req)
{
    char path[NID_PATH_MAX];
    inst_lock();
    strlcpy(path, inst_shared()->last_photo, sizeof(path));
    inst_unlock();
    if (path[0] == '\0') {
        return send_text(req, HTTP_404, "Aucune photo de test.");
    }
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return send_text(req, HTTP_404, "Photo introuvable sur la carte SD.");
    }
    char *chunk = malloc(PHOTO_CHUNK);
    if (chunk == NULL) {
        fclose(fp);
        return send_text(req, HTTP_503, "Mémoire insuffisante.");
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = ESP_OK;
    size_t n;
    while (err == ESP_OK && (n = fread(chunk, 1, PHOTO_CHUNK, fp)) > 0) {
        err = httpd_resp_send_chunk(req, chunk, (ssize_t)n);
    }
    fclose(fp);
    free(chunk);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

/* -------------------------------------------------------------------------- */
/* Démarrage de la surveillance, pause maintenance                            */
/* -------------------------------------------------------------------------- */

static esp_err_t request_end(httpd_req_t *req, inst_end_t end)
{
    inst_lock();
    inst_shared_t *sh = inst_shared();
    bool ok = sh->active && sh->accept_actions;
    if (ok && sh->web_request == INST_END_NONE) {
        sh->web_request = end;
    }
    inst_unlock();
    if (!ok) {
        return send_closing(req);
    }
    ESP_LOGI(TAG, "%s demandé depuis la page", end == INST_END_MAINTENANCE ? "pause maintenance" : "démarrage");
    return send_result(req, NULL, true, end == INST_END_MAINTENANCE ? "Pause maintenance." : "Surveillance démarrée.");
}

static esp_err_t start_post(httpd_req_t *req)
{
    return request_end(req, INST_END_START);
}

static esp_err_t maintenance_post(httpd_req_t *req)
{
    return request_end(req, INST_END_MAINTENANCE);
}

/* -------------------------------------------------------------------------- */
/* Aperçu MJPEG                                                               */
/* -------------------------------------------------------------------------- */

static void peer_of(httpd_req_t *req, struct sockaddr_storage *out)
{
    memset(out, 0, sizeof(*out));
    socklen_t len = sizeof(*out);
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0 || getpeername(fd, (struct sockaddr *)out, &len) != 0) {
        memset(out, 0, sizeof(*out));
    }
}

static bool same_peer(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
    if (a->ss_family != b->ss_family) {
        return false;
    }
    if (a->ss_family == AF_INET) {
        struct sockaddr_in x;
        struct sockaddr_in y;
        memcpy(&x, a, sizeof(x));
        memcpy(&y, b, sizeof(y));
        return x.sin_addr.s_addr == y.sin_addr.s_addr;
    }
#if CONFIG_LWIP_IPV6
    if (a->ss_family == AF_INET6) {
        struct sockaddr_in6 x;
        struct sockaddr_in6 y;
        memcpy(&x, a, sizeof(x));
        memcpy(&y, b, sizeof(y));
        return memcmp(&x.sin6_addr, &y.sin6_addr, sizeof(x.sin6_addr)) == 0;
    }
#endif
    return false;
}

/** Refuse une requête d'aperçu confiée à la tâche (réponse puis libération). */
static void refuse_async(httpd_req_t *r, const char *why)
{
    send_text(r, HTTP_503, why);
    httpd_req_async_handler_complete(r);
}

static bool has_pending(void)
{
    xSemaphoreTake(s_stream_lock, portMAX_DELAY);
    bool pending = s_pending != NULL;
    xSemaphoreGive(s_stream_lock);
    return pending;
}

static void preview_stop_locked(bool *on)
{
    if (*on && cam_lock()) {
        nid_capture_preview_stop();
        cam_unlock();
    }
    *on = false;
}

/** Envoie le flux à un téléphone jusqu'à son départ, une reprise ou la fin. */
static void run_session(httpd_req_t *req)
{
    const int64_t min_interval_us = 1000000LL / CONFIG_NID_INSTALL_PREVIEW_MAX_FPS;
    bool preview_on = false;
    bool headers_sent = false;
    bool alive = true;
    int fails = 0;
    int64_t last_us = 0;
    char part[96];

    while (alive && !atomic_load(&s_quit) && !has_pending()) {
        if (atomic_load(&s_pause_req)) {
            preview_stop_locked(&preview_on);
            atomic_store(&s_paused, true);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        atomic_store(&s_paused, false);

        if (!preview_on) {
            esp_err_t err = ESP_ERR_TIMEOUT;
            if (cam_lock()) {
                err = nid_capture_preview_start(CONFIG_NID_INSTALL_PREVIEW_QUALITY, CONFIG_NID_INSTALL_PREVIEW_MAX_WIDTH);
                cam_unlock();
            }
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "aperçu impossible : %s", esp_err_to_name(err));
                break;
            }
            preview_on = true;
        }

        int64_t wait_us = last_us + min_interval_us - esp_timer_get_time();
        if (wait_us > 0) {
            vTaskDelay(pdMS_TO_TICKS(wait_us > 50000 ? 50 : wait_us / 1000 + 1));
            continue;
        }

        const uint8_t *jpeg = NULL;
        size_t len = 0;
        esp_err_t err = ESP_ERR_TIMEOUT;
        if (cam_lock()) {
            err = nid_capture_preview_frame(&jpeg, &len, FRAME_TIMEOUT_MS);
            cam_unlock();
        }
        if (err != ESP_OK || jpeg == NULL || len == 0) {
            if (++fails >= FRAME_FAIL_MAX) {
                ESP_LOGW(TAG, "aperçu : plus d'image (%s)", esp_err_to_name(err));
                break;
            }
            continue;
        }
        fails = 0;

        if (!headers_sent) {
            httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY);
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            headers_sent = true;
        }
        int n = snprintf(part, sizeof(part),
                         "\r\n--" STREAM_BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                         (unsigned)len);
        err = (n > 0 && (size_t)n < sizeof(part)) ? httpd_resp_send_chunk(req, part, n) : ESP_FAIL;
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, (const char *)jpeg, (ssize_t)len);
        }
        if (cam_lock()) {
            nid_capture_preview_release();
            cam_unlock();
        }
        last_us = esp_timer_get_time();
        if (err != ESP_OK) {
            alive = false;  /* téléphone parti ou page fermée */
        }
    }

    preview_stop_locked(&preview_on);
    if (!headers_sent) {
        send_text(req, HTTP_503, "Aperçu indisponible.");
    } else if (alive) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    httpd_req_async_handler_complete(req);
    atomic_store(&s_paused, false);
}

static void stream_task(void *arg)
{
    (void)arg;
    while (!atomic_load(&s_quit)) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        xSemaphoreTake(s_stream_lock, portMAX_DELAY);
        httpd_req_t *req = atomic_load(&s_quit) ? NULL : s_pending;
        if (req != NULL) {
            s_pending = NULL;
            s_session_on = true;
            s_session_peer = s_pending_peer;
        }
        xSemaphoreGive(s_stream_lock);
        if (req != NULL) {
            run_session(req);
            xSemaphoreTake(s_stream_lock, portMAX_DELAY);
            s_session_on = false;
            xSemaphoreGive(s_stream_lock);
        }
    }
    /* Fin : refus des demandes encore en attente, puis plus aucun accès au serveur. */
    for (;;) {
        xSemaphoreTake(s_stream_lock, portMAX_DELAY);
        httpd_req_t *req = s_pending;
        s_pending = NULL;
        if (req == NULL) {
            s_stream_running = false;
            xSemaphoreGive(s_stream_lock);
            break;
        }
        xSemaphoreGive(s_stream_lock);
        refuse_async(req, "Fin du mode installation.");
    }
    vTaskDelete(NULL);
}

static esp_err_t stream_get(httpd_req_t *req)
{
    if (!nid_capture_is_ready()) {
        return send_text(req, HTTP_503, "Caméra indisponible.");
    }
    struct sockaddr_storage peer;
    peer_of(req, &peer);

    httpd_req_t *copy = NULL;
    httpd_req_t *old = NULL;
    const char *refused = NULL;
    xSemaphoreTake(s_stream_lock, portMAX_DELAY);
    if (!s_stream_running || atomic_load(&s_quit)) {
        refused = "Aperçu arrêté.";
    } else if (s_session_on && !same_peer(&peer, &s_session_peer)) {
        refused = "Aperçu déjà affiché sur un autre téléphone.";
    } else if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        refused = "Mémoire insuffisante.";
    } else {
        old = s_pending;
        s_pending = copy;
        s_pending_peer = peer;
        xTaskNotifyGive(s_stream_task);
    }
    xSemaphoreGive(s_stream_lock);

    if (old != NULL) {
        refuse_async(old, "Demande remplacée.");
    }
    if (refused != NULL) {
        return send_text(req, HTTP_503, refused);
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Démarrage / arrêt                                                          */
/* -------------------------------------------------------------------------- */

static const httpd_uri_t s_uris[] = {
    { .uri = "/",                .method = HTTP_GET,  .handler = index_get },
    { .uri = "/stream",          .method = HTTP_GET,  .handler = stream_get },
    { .uri = "/photo",           .method = HTTP_GET,  .handler = photo_get },
    { .uri = "/api/status",      .method = HTTP_GET,  .handler = status_get },
    { .uri = "/api/config",      .method = HTTP_GET,  .handler = config_get },
    { .uri = "/api/config",      .method = HTTP_POST, .handler = config_post },
    { .uri = "/api/time",        .method = HTTP_POST, .handler = time_post },
    { .uri = "/api/ir",          .method = HTTP_POST, .handler = ir_post },
    { .uri = "/api/photo",       .method = HTTP_POST, .handler = photo_post },
    { .uri = "/api/start",       .method = HTTP_POST, .handler = start_post },
    { .uri = "/api/maintenance", .method = HTTP_POST, .handler = maintenance_post },
};

_Static_assert(sizeof(s_uris) / sizeof(s_uris[0]) <= CONFIG_NID_INSTALL_HTTPD_MAX_URI_HANDLERS,
               "CONFIG_NID_INSTALL_HTTPD_MAX_URI_HANDLERS trop petit");

esp_err_t inst_web_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }
    if (s_cam_lock == NULL) {
        s_cam_lock = xSemaphoreCreateMutexStatic(&s_cam_lock_buf);
    }
    if (s_stream_lock == NULL) {
        s_stream_lock = xSemaphoreCreateMutexStatic(&s_stream_lock_buf);
    }
    atomic_store(&s_quit, false);
    atomic_store(&s_pause_req, false);
    atomic_store(&s_paused, false);
    s_pending = NULL;
    s_session_on = false;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = CONFIG_NID_INSTALL_HTTPD_STACK_SIZE;
    cfg.max_uri_handlers = CONFIG_NID_INSTALL_HTTPD_MAX_URI_HANDLERS;
    cfg.max_open_sockets = HTTPD_SOCKETS;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 3;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        s_server = NULL;
        ESP_LOGE(TAG, "démarrage du serveur web impossible : %s", esp_err_to_name(err));
        return err;
    }
    for (size_t i = 0; i < sizeof(s_uris) / sizeof(s_uris[0]); i++) {
        err = httpd_register_uri_handler(s_server, &s_uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "gestionnaire %s : %s", s_uris[i].uri, esp_err_to_name(err));
            httpd_stop(s_server);
            s_server = NULL;
            return err;
        }
    }

    xSemaphoreTake(s_stream_lock, portMAX_DELAY);
    s_stream_running = true;
    xSemaphoreGive(s_stream_lock);
    if (xTaskCreate(stream_task, "nid_apercu", STREAM_TASK_STACK, NULL, STREAM_TASK_PRIO, &s_stream_task) != pdPASS) {
        xSemaphoreTake(s_stream_lock, portMAX_DELAY);
        s_stream_running = false;
        xSemaphoreGive(s_stream_lock);
        s_stream_task = NULL;
        ESP_LOGE(TAG, "tâche d'aperçu impossible : page sans aperçu");
    }
    ESP_LOGI(TAG, "serveur web démarré");
    return ESP_OK;
}

void inst_web_stop(void)
{
    atomic_store(&s_quit, true);
    bool running = false;
    if (s_stream_lock != NULL) {
        xSemaphoreTake(s_stream_lock, portMAX_DELAY);
        running = s_stream_running;
        if (running) {
            xTaskNotifyGive(s_stream_task);
        }
        xSemaphoreGive(s_stream_lock);
        int64_t t0 = esp_timer_get_time();
        while (running && esp_timer_get_time() - t0 < (int64_t)STREAM_STOP_WAIT_MS * 1000) {
            vTaskDelay(pdMS_TO_TICKS(20));
            xSemaphoreTake(s_stream_lock, portMAX_DELAY);
            running = s_stream_running;
            xSemaphoreGive(s_stream_lock);
        }
    }
    if (running) {
        /* Ne pas libérer le serveur sous une requête encore utilisée. */
        ESP_LOGE(TAG, "tâche d'aperçu bloquée : serveur web laissé actif");
        return;
    }
    s_stream_task = NULL;
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "serveur web arrêté");
    }
}

void inst_web_camera_idle(void)
{
    inst_lock();
    bool ir = inst_shared()->ir_on;
    inst_shared()->ir_on = false;
    inst_unlock();

    bool locked = cam_lock();
    if (ir) {
        nid_capture_set_night(false);
    }
    nid_ir_off();
    if (locked) {
        cam_unlock();
    }
}
