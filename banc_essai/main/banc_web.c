/*
 * banc_web — tableau de bord web du banc d'essai (banc utilisable sans écran)
 *
 * http://<adresse du banc> (192.168.4.1 en point d'accès) :
 *   GET  /                 page (www/banc.html, embarquée, sans dépendance externe)
 *   GET  /api/etat         état complet (JSON), interrogé chaque seconde par la page
 *                          (?son=1 : la section Son est affichée, le vumètre est mesuré)
 *   GET  /api/reglages     réglages modifiables à chaud (clés de config.ini)
 *   POST /api/console      c=<commande> : mêmes commandes que la console série,
 *                          sortie renvoyée (JSON) ; les boutons de la page s'en servent
 *   POST /api/heure        epoch=<s> : mise à l'heure depuis le navigateur
 *   GET  /stream           vue caméra MJPEG (banc_web_vue.c), un appareil à la fois
 *   GET  /api/fichiers     ?d=<dossier> : contenu d'un dossier de la carte SD (JSON)
 *   GET  /fichier          ?f=<fichier>[&vue=1] : téléchargement par blocs (tâche dédiée)
 *   POST /api/supprimer    f=<fichier> : suppression (si CONFIG_BANC_WEB_SUPPRESSION)
 *
 * Les actions passent par la même table de commandes que la console série,
 * donc par la même file d'action que le bouton BOOT (jamais deux
 * enregistrements à la fois). La sortie d'une commande est capturée : stdout
 * est remplacé, le temps de la commande, par un flux qui recopie tout sur la
 * console série et ne garde pour la page que ce qu'écrit la tâche du serveur.
 *
 * Défauts relevés pour nid_install (docs/revue_cdcf.md) non reproduits :
 * délais de réception courts (2 s, sans nouvel essai), entrées validées
 * (longueurs, caractères, chemins limités à la carte SD sans « .. »), tampon
 * de la vue protégé pendant son envoi, aucun mot de passe renvoyé.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE     /* fopencookie (newlib) ; déjà défini par ESP-IDF */
#endif
#include "banc.h"

#include "sdkconfig.h"

#if CONFIG_BANC_F_WIFI

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "nid_board.h"
#include "nid_detect.h"

#include "banc_web.h"

static const char *TAG = "banc_web";

extern const char banc_html_debut[] asm("_binary_banc_html_start");
extern const char banc_html_fin[] asm("_binary_banc_html_end");

#define HTTPD_PILE          10240
#define HTTPD_PRIO          3           /* sous la tâche d'action (5) : l'enregistrement passe avant */
#define HTTPD_SOCKETS       6           /* + 3 internes ≤ CONFIG_LWIP_MAX_SOCKETS (10) */
#define HTTPD_GESTIONNAIRES 12
#define CORPS_MAX           256
#define LIGNE_MAX           160
#define ETAT_JSON_LEN       6144
#define SORTIE_MAX          (6 * 1024)
#define REPONSE_LEN         (SORTIE_MAX + 512)
#define CHEMIN_MAX          160
#define REL_MAX             120
#define NOM_MAX             64
#define ENTREES_MAX         400
#define BLOC_FICHIER        (16 * 1024)
#define PAUSE_REC_MS        20          /* téléchargement pendant un enregistrement : ≤ 800 ko/s */
#define FIC_PILE            4096
#define FIC_PRIO            3
#define SON_DUREE_US        (3LL * 1000000LL)
#define MESURES_PERIODE_US  (2LL * 1000000LL)
#define CONSOLE_ATTENTE_MS  5000

#if CONFIG_BANC_WEB_SUPPRESSION
#define SUPPRESSION_POSSIBLE true
#else
#define SUPPRESSION_POSSIBLE false
#endif

static httpd_handle_t s_serveur;
static atomic_uint s_requetes;
static atomic_llong s_son_jusqua_us;

/* ------------------------------------------------------------------------- */
/* Construction de JSON                                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   plein;
} sb_t;

static void sb_init(sb_t *b, char *buf, size_t cap)
{
    b->buf = buf;
    b->cap = cap;
    b->len = 0;
    b->plein = (cap == 0);
    if (cap) {
        buf[0] = '\0';
    }
}

static void sb_printf(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(sb_t *b, const char *fmt, ...)
{
    if (b->plein) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) {
        b->plein = true;
        b->buf[b->len] = '\0';
        return;
    }
    b->len += (size_t)n;
}

static void sb_putc(sb_t *b, char c)
{
    if (b->plein || b->len + 1 >= b->cap) {
        b->plein = true;
        return;
    }
    b->buf[b->len++] = c;
    b->buf[b->len] = '\0';
}

/** Chaîne JSON entre guillemets (caractères spéciaux échappés). */
static void sb_str(sb_t *b, const char *s)
{
    sb_putc(b, '"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') {
            sb_putc(b, '\\');
            sb_putc(b, (char)*p);
        } else if (*p == '\n') {
            sb_putc(b, '\\');
            sb_putc(b, 'n');
        } else if (*p < 0x20) {
            if (*p != '\r') {
                sb_printf(b, "\\u%04x", (unsigned)*p);
            }
        } else {
            sb_putc(b, (char)*p);
        }
    }
    sb_putc(b, '"');
}

/** « ,"clé": » */
static void sb_cle(sb_t *b, const char *cle)
{
    sb_printf(b, ",\"%s\":", cle);
}

static const char *json_bool(bool v)
{
    return v ? "true" : "false";
}

static const char *nom_couleur(banc_couleur_t c)
{
    static const char *const noms[BANC_NB_COULEURS] = {
        [BANC_BLANC] = "blanc", [BANC_GRIS] = "gris", [BANC_VERT] = "vert", [BANC_JAUNE] = "jaune",
        [BANC_ROUGE] = "rouge", [BANC_NOIR] = "noir", [BANC_FOND] = "fond", [BANC_GRIS_FONCE] = "grisf",
        [BANC_VERT_FONCE] = "vertf", [BANC_ROUGE_VIF] = "rougev", [BANC_ORANGE] = "orange", [BANC_BLEU] = "bleu",
    };
    return (c < BANC_NB_COULEURS && noms[c]) ? noms[c] : "blanc";
}

/* ------------------------------------------------------------------------- */
/* Réponses, lecture des requêtes                                             */
/* ------------------------------------------------------------------------- */

void banc_web_compter(void)
{
    atomic_fetch_add(&s_requetes, 1);
}

uint32_t banc_web_requetes(void)
{
    return atomic_load(&s_requetes);
}

bool banc_web_son_demande(void)
{
    return esp_timer_get_time() < atomic_load(&s_son_jusqua_us);
}

esp_err_t banc_web_texte(httpd_req_t *req, const char *statut, const char *texte)
{
    httpd_resp_set_status(req, statut);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, texte);
}

static esp_err_t envoyer_json(httpd_req_t *req, const char *statut, const char *json)
{
    if (statut != NULL) {
        httpd_resp_set_status(req, statut);
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/** Réponse {"ok":…,"message":…}. */
static esp_err_t envoyer_resultat(httpd_req_t *req, const char *statut, bool ok, const char *message)
{
    char buf[320];
    sb_t b;
    sb_init(&b, buf, sizeof(buf));
    sb_printf(&b, "{\"ok\":%s,\"message\":", json_bool(ok));
    sb_str(&b, message);
    sb_putc(&b, '}');
    if (b.plein) {
        return envoyer_json(req, BANC_HTTP_500, "{\"ok\":false,\"message\":\"Réponse trop longue\"}");
    }
    return envoyer_json(req, statut, buf);
}

void banc_web_pair(httpd_req_t *req, struct sockaddr_storage *out)
{
    memset(out, 0, sizeof(*out));
    socklen_t len = sizeof(*out);
    const int fd = httpd_req_to_sockfd(req);
    if (fd < 0 || getpeername(fd, (struct sockaddr *)out, &len) != 0) {
        memset(out, 0, sizeof(*out));
    }
}

bool banc_web_meme_pair(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
    if (a->ss_family != b->ss_family) {
        return false;
    }
    if (a->ss_family == AF_INET) {
        struct sockaddr_in x, y;
        memcpy(&x, a, sizeof(x));
        memcpy(&y, b, sizeof(y));
        return x.sin_addr.s_addr == y.sin_addr.s_addr;
    }
#if CONFIG_LWIP_IPV6
    if (a->ss_family == AF_INET6) {
        struct sockaddr_in6 x, y;
        memcpy(&x, a, sizeof(x));
        memcpy(&y, b, sizeof(y));
        return memcmp(&x.sin6_addr, &y.sin6_addr, sizeof(x.sin6_addr)) == 0;
    }
#endif
    return false;
}

/**
 * Corps de la requête (≤ CORPS_MAX, terminé par '\0') dans buf. Délai de
 * réception borné par la configuration du serveur (2 s), sans nouvel essai.
 */
static bool lire_corps(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len == 0 || req->content_len >= cap || req->content_len > CORPS_MAX) {
        return false;
    }
    size_t recu = 0;
    while (recu < req->content_len) {
        const int r = httpd_req_recv(req, buf + recu, req->content_len - recu);
        if (r <= 0) {
            return false;   /* délai dépassé ou connexion fermée */
        }
        recu += (size_t)r;
    }
    buf[recu] = '\0';
    return true;
}

static int hex(char c)
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
static void url_decoder(char *s)
{
    char *o = s;
    for (const char *p = s; *p; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (p[0] == '%' && hex(p[1]) >= 0 && hex(p[2]) >= 0) {
            *o++ = (char)(hex(p[1]) * 16 + hex(p[2]));
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

/** Champ d'un petit formulaire POST (« clé=valeur&… »), décodé. */
static bool champ_formulaire(httpd_req_t *req, const char *cle, char *val, size_t len)
{
    char corps[CORPS_MAX + 1];
    if (!lire_corps(req, corps, sizeof(corps))) {
        return false;
    }
    if (httpd_query_key_value(corps, cle, val, len) != ESP_OK) {
        return false;
    }
    url_decoder(val);
    return true;
}

/** Paramètre de l'URL, décodé. */
static bool parametre(httpd_req_t *req, const char *cle, char *val, size_t len)
{
    char q[CHEMIN_MAX + 32];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, cle, val, len) != ESP_OK) {
        return false;
    }
    url_decoder(val);
    return true;
}

/** Texte UTF-8 sans caractère de contrôle. */
static bool texte_imprimable(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7F) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Page                                                                       */
/* ------------------------------------------------------------------------- */

static esp_err_t page_get(httpd_req_t *req)
{
    banc_web_compter();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* EMBED_TXTFILES ajoute un '\0' final, non envoyé. */
    return httpd_resp_send(req, banc_html_debut, banc_html_fin - banc_html_debut - 1);
}

/* ------------------------------------------------------------------------- */
/* État                                                                       */
/* ------------------------------------------------------------------------- */

/* Mesures lentes (ADC, capteur de température), au plus toutes les 2 s. */
static struct {
    int64_t   t_us;
    esp_err_t batt_err;
    uint32_t  batt_mv, batt_brut_mv;
    esp_err_t temp_err;
    float     temp_c;
} s_mes = { .batt_err = ESP_ERR_NOT_SUPPORTED, .temp_err = ESP_ERR_NOT_SUPPORTED };

static void mesures_lentes(int64_t now)
{
    if (s_mes.t_us != 0 && now - s_mes.t_us < MESURES_PERIODE_US) {
        return;
    }
    s_mes.t_us = now;
    s_mes.batt_err = banc_f_active(BANC_F_BATTERIE) ? banc_batterie(&s_mes.batt_mv, &s_mes.batt_brut_mv)
                                                    : ESP_ERR_NOT_SUPPORTED;
    s_mes.temp_err = banc_f_active(BANC_F_TEMPERATURE) ? banc_temperature(&s_mes.temp_c) : ESP_ERR_NOT_SUPPORTED;
}

/** « active », « désactivé », « en défaut », « non compilé ». */
static const char *texte_fonction(banc_fonction_t f)
{
    const char *t = banc_f_texte(f);
    return t ? t : "active";
}

static void json_fonctions(sb_t *b)
{
    sb_cle(b, "fonctions");
    sb_putc(b, '[');
    for (int f = 0; f < BANC_NB_F; f++) {
        sb_printf(b, "%s{\"nom\":", f ? "," : "");
        sb_str(b, banc_f_nom((banc_fonction_t)f));
        sb_printf(b, ",\"libelle\":");
        sb_str(b, banc_f_libelle((banc_fonction_t)f));
        sb_printf(b, ",\"etat\":");
        sb_str(b, texte_fonction((banc_fonction_t)f));
        sb_printf(b, ",\"choisie\":%s}", json_bool(banc_f_choisie((banc_fonction_t)f)));
    }
    sb_putc(b, ']');
}

static void json_bandeau(sb_t *b)
{
    sb_cle(b, "bandeau");
    sb_putc(b, '[');
    for (int l = 0; l < BANC_NB_LIGNES; l++) {
        char t[160];
        banc_couleur_t c = BANC_BLANC;
        banc_bandeau_lire((banc_ligne_t)l, t, sizeof(t), &c);
        sb_printf(b, "%s{\"t\":", l ? "," : "");
        sb_str(b, t);
        sb_printf(b, ",\"c\":\"%s\"}", nom_couleur(c));
    }
    sb_putc(b, ']');
}

static void json_detection(sb_t *b, const banc_etat_t *e, int64_t now)
{
    sb_printf(b, ",\"pir\":{\"gpio\":%d,\"etat\":", (int)nid_board_pins()->pir);
    if (banc_f_active(BANC_F_PIR)) {
        sb_str(b, nid_detect_pir_active() ? "ACTIF" : "REPOS");
    } else {
        sb_str(b, texte_fonction(BANC_F_PIR));
    }
    char decl[48];
    banc_couleur_t fond = BANC_GRIS_FONCE;
    banc_texte_declenchement(e, now, decl, sizeof(decl), &fond);
    sb_printf(b, "},\"arme\":%s,\"declenchement\":{\"texte\":", json_bool(e->arme));
    sb_str(b, decl);
    sb_printf(b, ",\"c\":\"%s\"}", nom_couleur(fond));

    const int64_t ecoule_ms = e->rec ? (now - e->rec_debut_us) / 1000 : 0;
    const uint32_t ips10 = ecoule_ms > 500 ? (uint32_t)((int64_t)e->rec_images * 10000 / ecoule_ms) : 0;
    sb_printf(b, ",\"rec\":{\"on\":%s,\"ecoule_s\":%lld,\"duree_s\":%" PRIu32 ",\"images\":%" PRIu32
              ",\"ips10\":%" PRIu32 ",\"motif\":", json_bool(e->rec), (long long)(ecoule_ms / 1000), e->rec_duree_s,
              e->rec_images, ips10);
    sb_str(b, e->rec_motif);
    sb_printf(b, "},\"occupe\":%s", json_bool(banc_occupe()));
    sb_printf(b, ",\"compteurs\":{\"fronts\":%" PRIu32 ",\"detections\":%" PRIu32 ",\"sequences\":%" PRIu32
              ",\"fausses\":%" PRIu32 ",\"ignorees\":%" PRIu32 ",\"motif\":", e->fronts, e->detections, e->sequences,
              e->fausses, e->ignorees);
    sb_str(b, e->ignoree_motif);
    sb_printf(b, "},\"latence_ms\":%" PRId32 ",\"dernier\":", e->latence_ms);
    sb_str(b, e->dernier_fichier);
    sb_printf(b, ",\"video\":{\"images\":%" PRIu32 ",\"perdues\":%" PRIu32 ",\"trames_son\":%" PRIu32
              ",\"duree_ms\":%" PRIu32 ",\"ok\":%s}", e->bilan_images, e->bilan_perdues, e->bilan_trames_son,
              e->bilan_duree_ms, json_bool(e->bilan_ok));
    sb_cle(b, "bilan");
    sb_str(b, banc_dernier_bilan());
    nid_config_t cfg;
    banc_reglages(&cfg);
    sb_printf(b, ",\"reglages\":{\"sequence_s\":%u,\"delai_s\":%u,\"chauffe_s\":%u,\"sensibilite\":%u,"
              "\"suivi_min\":%u}", cfg.seq_duration_s, cfg.trigger_delay_s, cfg.pir_warmup_s, cfg.pir_sensitivity,
              cfg.photo_interval_min);

    int64_t suivi_s = -1;
    if (banc_f_active(BANC_F_SUIVI) && cfg.photo_interval_min > 0 && e->arme && e->suivi_prochaine_us != 0) {
        suivi_s = (e->suivi_prochaine_us - now + 999999) / 1000000;
        if (suivi_s < 0) {
            suivi_s = 0;
        }
    }
    sb_printf(b, ",\"photo\":{\"fichier\":");
    sb_str(b, e->photo_fichier);
    sb_printf(b, ",\"ok\":%s,\"ko\":%" PRIu32 ",\"ms\":%" PRIu32 ",\"nb\":%" PRIu32 ",\"suivi_nb\":%" PRIu32
              ",\"suivi_reste_s\":%lld,\"suivi\":", json_bool(e->photo_ok), e->photo_ko, e->photo_ms, e->photos,
              e->photos_suivi, (long long)suivi_s);
    sb_str(b, texte_fonction(BANC_F_SUIVI));
    sb_putc(b, '}');
}

static void json_nuit_son(sb_t *b, const banc_etat_t *e)
{
    static const char *const modes[] = { "jour", "nuit", "auto" };
    char lever[12] = "-", coucher[12] = "-";
    bool nuit_fw = false;
    banc_soleil(lever, coucher, sizeof(lever), &nuit_fw);
    sb_printf(b, ",\"nuit\":{\"fonction\":");
    sb_str(b, texte_fonction(BANC_F_NUIT));
    sb_printf(b, ",\"mode\":\"%s\",\"active\":%s,\"ir\":%u,\"ir_cable\":%s,\"filtre\":%d,\"mono\":%s,\"lever\":",
              modes[e->mode_nuit <= BANC_NUIT_AUTO ? e->mode_nuit : 0], json_bool(e->nuit_active), e->led_ir_pct,
              json_bool(nid_board_pins()->ir_led != GPIO_NUM_NC), e->filtre_jour, json_bool(e->mono));
    sb_str(b, lever);
    sb_cle(b, "coucher");
    sb_str(b, coucher);
    sb_printf(b, ",\"firmware_nuit\":%s}", json_bool(nuit_fw));

    nid_capture_audio_level_t l = e->son_niveau;
    bool valide = e->son_niveau_valide;
    if (e->rec) {
        valide = nid_capture_audio_last_level(&l);
    }
    if (!banc_f_active(BANC_F_SON)) {
        valide = false;
    }
    sb_printf(b, ",\"son\":{\"fonction\":");
    sb_str(b, texte_fonction(BANC_F_SON));
    sb_printf(b, ",\"valide\":%s,\"rms_db10\":%d,\"crete_db10\":%d}", json_bool(valide), banc_dbfs10(l.rms),
              banc_dbfs10(l.peak));
}

static void json_systeme(sb_t *b, const banc_etat_t *e, int64_t now)
{
    mesures_lentes(now);
    sb_printf(b, ",\"batterie\":{\"fonction\":");
    sb_str(b, texte_fonction(BANC_F_BATTERIE));
    sb_printf(b, ",\"etat\":\"%s\",\"mv\":%" PRIu32 ",\"brut_mv\":%" PRIu32 "}",
              s_mes.batt_err == ESP_OK ? "ok" : (s_mes.batt_err == ESP_ERR_INVALID_RESPONSE ? "non_cablee" : "absente"),
              s_mes.batt_mv, s_mes.batt_brut_mv);
    sb_printf(b, ",\"temperature\":{\"fonction\":");
    sb_str(b, texte_fonction(BANC_F_TEMPERATURE));
    if (s_mes.temp_err == ESP_OK) {
        sb_printf(b, ",\"c10\":%d}", (int)(s_mes.temp_c * 10.0f));
    } else {
        sb_printf(b, ",\"c10\":null}");
    }

    nid_storage_volume_t v;
    banc_volume_sd(&v);
    sb_printf(b, ",\"sd\":{\"fonction\":");
    sb_str(b, texte_fonction(BANC_F_SD));
    sb_printf(b, ",\"montee\":%s,\"libre\":%" PRIu64 ",\"volume\":%" PRIu64 ",\"carte\":%" PRIu64 ",\"fat\":%u"
              ",\"test\":%s,\"debit_ko_s\":%" PRIu32 ",\"latence_max_ms\":%" PRIu32 ",\"dossier\":",
              json_bool(nid_storage_is_mounted()), v.free_bytes, v.volume_bytes, v.card_bytes, v.fat_type,
              json_bool(e->sd_test_fait), e->sd_debit_ko_s, e->sd_latence_max_ms);
    sb_str(b, nid_storage_is_mounted() ? nid_storage_base_dir() : "");
    sb_putc(b, '}');

    sb_printf(b, ",\"memoire\":{\"interne_ko\":%u,\"interne_min_ko\":%u,\"psram_ko\":%u}",
              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
              (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    sb_printf(b, ",\"boucle\":{\"en_cours\":%s,\"faite\":%s,\"n\":%" PRIu32 ",\"i\":%" PRIu32 ",\"echecs\":%" PRIu32
              ",\"perdues\":%" PRIu32 ",\"delta_interne_ko\":%" PRId32 ",\"delta_psram_ko\":%" PRId32 "}",
              json_bool(e->boucle_en_cours), json_bool(e->boucle_faite), e->boucle_n, e->boucle_i, e->boucle_echecs,
              e->boucle_perdues, e->boucle_delta_interne_ko, e->boucle_delta_psram_ko);

    const nid_capture_settings_t *r = banc_reglages_capture();
    char reg[64];
    snprintf(reg, sizeof(reg), "%ux%u, %u i/s, %" PRIu32 " kbit/s, son %s", r->width, r->height, r->fps,
             r->bitrate_kbps, r->audio ? "oui" : "non");
    sb_printf(b, ",\"camera\":{\"fonction\":");
    sb_str(b, texte_fonction(BANC_F_CAMERA));
    sb_cle(b, "capteur");
    sb_str(b, nid_capture_sensor_name());
    sb_cle(b, "reglages");
    sb_str(b, reg);
    sb_putc(b, '}');

    banc_wifi_etat_t w;
    banc_wifi_etat(&w);
    uint32_t vues = 0, vue_ips10 = 0;
    banc_vue_stats(&vues, &vue_ips10);
    sb_printf(b, ",\"wifi\":{\"mode\":\"%s\",\"etat\":", w.st.mode == NID_WIFI_MODE_AP ? "point d'accès" : "station");
    sb_str(b, nid_wifi_state_str(w.st.state));
    sb_cle(b, "ssid");
    sb_str(b, w.st.ssid);
    sb_cle(b, "ip");
    sb_str(b, w.st.ip);
    sb_printf(b, ",\"clients\":%d,\"canal\":%u,\"lien\":\"%s\",\"repli\":%s,\"c6\":", w.st.clients, w.st.channel,
              nid_wifi_link_lost() ? "perdu" : "ok", json_bool(w.repli_ap));
    sb_str(b, w.version_c6);
    sb_cle(b, "hote");
    sb_str(b, w.version_hote);
    sb_cle(b, "compat");
    sb_str(b, w.compat);
    sb_printf(b, ",\"compat_ok\":%s", json_bool(w.compat_ok));
    if (w.rssi_ok) {
        sb_printf(b, ",\"rssi\":%d", w.rssi);
    } else {
        sb_printf(b, ",\"rssi\":null");
    }
    sb_printf(b, ",\"requetes\":%" PRIu32 ",\"vue\":%s,\"vue_ips10\":%" PRIu32 ",\"erreur\":", w.requetes,
              json_bool(w.vue_client), vue_ips10);
    sb_str(b, w.st.error);
    sb_putc(b, '}');
}

static esp_err_t etat_get(httpd_req_t *req)
{
    banc_web_compter();
    const int64_t now = esp_timer_get_time();
    char v[4];
    if (parametre(req, "son", v, sizeof(v)) && v[0] == '1') {
        atomic_store(&s_son_jusqua_us, now + SON_DUREE_US);
    }
    char *buf = heap_caps_malloc(ETAT_JSON_LEN, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        buf = malloc(ETAT_JSON_LEN);
    }
    if (buf == NULL) {
        return envoyer_resultat(req, BANC_HTTP_503, false, "Mémoire insuffisante");
    }
    banc_etat_t e;
    banc_etat_lire(&e);
    sb_t b;
    sb_init(&b, buf, ETAT_JSON_LEN);
    char heure[24];
    banc_heure_locale(heure, sizeof(heure), "%Y-%m-%d %H:%M:%S");
    sb_printf(&b, "{\"epoch\":%lld,\"heure\":", (long long)time(NULL));
    sb_str(&b, heure);
    sb_cle(&b, "version");
    sb_str(&b, esp_app_get_description()->version);
    sb_printf(&b, ",\"uptime_s\":%lld", (long long)(now / 1000000LL));
    json_bandeau(&b);
    json_fonctions(&b);
    json_detection(&b, &e, now);
    json_nuit_son(&b, &e);
    json_systeme(&b, &e, now);
    sb_putc(&b, '}');
    const esp_err_t err = b.plein ? envoyer_resultat(req, BANC_HTTP_500, false, "État trop long")
                                  : envoyer_json(req, NULL, buf);
    free(buf);
    return err;
}

static esp_err_t reglages_get(httpd_req_t *req)
{
    banc_web_compter();
    nid_config_t c;
    banc_reglages(&c);
    char buf[640];
    sb_t b;
    sb_init(&b, buf, sizeof(buf));
    sb_printf(&b, "{\"duree_sequence_s\":\"%u\",\"delai_entre_declenchements_s\":\"%u\",\"chauffe_pir_s\":\"%u\","
              "\"sensibilite\":\"%u\",\"intervalle_suivi_min\":\"%u\",\"mode\":\"%s\",\"latitude\":\"%.4f\","
              "\"longitude\":\"%.4f\",\"avance_crepuscule_min\":\"%d\",\"retard_aube_min\":\"%d\","
              "\"puissance_ir\":\"%u\",\"nuit_monochrome\":\"%s\"}",
              c.seq_duration_s, c.trigger_delay_s, c.pir_warmup_s, c.pir_sensitivity, c.photo_interval_min,
              nid_config_night_mode_str(c.night_mode), (double)c.latitude, (double)c.longitude, c.dusk_advance_min,
              c.dawn_delay_min, c.ir_power_pct, c.night_monochrome ? "oui" : "non");
    if (b.plein) {
        return envoyer_resultat(req, BANC_HTTP_500, false, "Réglages illisibles");
    }
    return envoyer_json(req, NULL, buf);
}

/* ------------------------------------------------------------------------- */
/* Console web : capture de la sortie des commandes                           */
/* ------------------------------------------------------------------------- */

#if CONFIG_LIBC_NEWLIB || CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY
#define CAPTURE_POSSIBLE 1
#else
#define CAPTURE_POSSIBLE 0
#endif

static SemaphoreHandle_t s_console_mutex;
static char *s_sortie;
static size_t s_sortie_len;
static bool s_sortie_tronquee;
static TaskHandle_t s_capture_tache;    /* tâche dont la sortie est gardée pour la page */
static FILE *s_tee;
static FILE *s_stdout_avant;

#if CAPTURE_POSSIBLE
/* Flux « tee » : tout est recopié sur la console série ; seule la sortie de la
 * tâche du serveur web, pendant une commande, est gardée pour la page. */
static ssize_t tee_ecrire(void *cookie, const char *buf, size_t n)
{
    (void)cookie;
    if (s_capture_tache != NULL && xTaskGetCurrentTaskHandle() == s_capture_tache && s_sortie != NULL) {
        size_t libre = SORTIE_MAX - 1 - s_sortie_len;
        size_t k = n < libre ? n : libre;
        memcpy(s_sortie + s_sortie_len, buf, k);
        s_sortie_len += k;
        s_sortie[s_sortie_len] = '\0';
        if (k < n) {
            s_sortie_tronquee = true;
        }
    }
    FILE *o = s_stdout_avant;
    if (o != NULL) {
        fwrite(buf, 1, n, o);
    }
    return (ssize_t)n;
}
#endif

static void capture_debut(void)
{
    s_sortie_len = 0;
    s_sortie[0] = '\0';
    s_sortie_tronquee = false;
#if CAPTURE_POSSIBLE
    if (s_tee == NULL) {
        const cookie_io_functions_t f = { .write = tee_ecrire };
        s_tee = fopencookie(NULL, "w", f);
        if (s_tee != NULL) {
            setvbuf(s_tee, NULL, _IONBF, 0);
        }
    }
    if (s_tee != NULL) {
        fflush(stdout);
        s_stdout_avant = stdout;
        s_capture_tache = xTaskGetCurrentTaskHandle();
        stdout = s_tee;     /* picolibc : flux global, le temps de la commande ; newlib : flux de cette tâche */
    }
#endif
}

static void capture_fin(void)
{
#if CAPTURE_POSSIBLE
    if (s_tee != NULL && s_stdout_avant != NULL) {
        fflush(s_tee);
        stdout = s_stdout_avant;
        s_capture_tache = NULL;
        fflush(stdout);
    }
#endif
}

static esp_err_t console_post(httpd_req_t *req)
{
    banc_web_compter();
    char ligne[LIGNE_MAX];
    if (!champ_formulaire(req, "c", ligne, sizeof(ligne)) || !texte_imprimable(ligne) || ligne[0] == '\0') {
        return envoyer_resultat(req, BANC_HTTP_400, false, "Commande vide, illisible ou trop longue (159 caractères au plus)");
    }
    char masque[LIGNE_MAX];
    banc_console_masquer(ligne, masque, sizeof(masque));
    ESP_LOGI(TAG, "console web : %s", masque);
    if (s_console_mutex == NULL || xSemaphoreTake(s_console_mutex, pdMS_TO_TICKS(CONSOLE_ATTENTE_MS)) != pdTRUE) {
        return envoyer_resultat(req, BANC_HTTP_503, false, "Une autre commande est en cours : réessayer");
    }
    int code = 1;
    capture_debut();
    const esp_err_t err = banc_console_executer(ligne, &code);
    capture_fin();

    char *rep = heap_caps_malloc(REPONSE_LEN, MALLOC_CAP_SPIRAM);
    if (rep == NULL) {
        rep = malloc(REPONSE_LEN);
    }
    esp_err_t res;
    if (rep == NULL) {
        res = envoyer_resultat(req, BANC_HTTP_503, false, "Mémoire insuffisante");
    } else {
        sb_t b;
        sb_init(&b, rep, REPONSE_LEN);
        sb_printf(&b, "{\"ok\":%s,\"code\":%d,\"inconnue\":%s,\"tronquee\":%s,\"sortie\":",
                  json_bool(err == ESP_OK && code == 0), code, json_bool(err == ESP_ERR_NOT_FOUND),
                  json_bool(s_sortie_tronquee));
        sb_str(&b, CAPTURE_POSSIBLE ? s_sortie : "(sortie sur la console série seulement)");
        sb_putc(&b, '}');
        res = b.plein ? envoyer_resultat(req, BANC_HTTP_500, false, "Sortie trop longue") : envoyer_json(req, NULL, rep);
        free(rep);
    }
    xSemaphoreGive(s_console_mutex);
    return res;
}

static esp_err_t heure_post(httpd_req_t *req)
{
    banc_web_compter();
    char v[24];
    if (!champ_formulaire(req, "epoch", v, sizeof(v))) {
        return envoyer_resultat(req, BANC_HTTP_400, false, "Heure absente");
    }
    char *fin = NULL;
    errno = 0;
    const long long t = strtoll(v, &fin, 10);
    if (errno != 0 || fin == v || *fin != '\0') {
        return envoyer_resultat(req, BANC_HTTP_400, false, "Heure illisible");
    }
    const esp_err_t err = banc_regler_heure((time_t)t, "page web");
    if (err != ESP_OK) {
        return envoyer_resultat(req, BANC_HTTP_400, false, "Heure refusée (2024 à 2099)");
    }
    char txt[48];
    banc_heure_locale(txt, sizeof(txt), "Heure réglée : %Y-%m-%d %H:%M:%S");
    return envoyer_resultat(req, NULL, true, txt);
}

/* ------------------------------------------------------------------------- */
/* Fichiers de la carte SD                                                    */
/* ------------------------------------------------------------------------- */

/** Nom autorisé : 1 à NOM_MAX caractères [A-Za-z0-9_.-], ni « . » ni « .. ». */
static bool nom_valide(const char *s, size_t n)
{
    if (n == 0 || n > NOM_MAX || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.')) {
            return false;
        }
    }
    return true;
}

/** Chemin relatif à la carte (« BANC_N00/20261020 », "" : racine) → chemin absolu. */
static bool chemin_valide(const char *rel, char *abs, size_t len)
{
    const size_t n = strlen(rel);
    if (n > REL_MAX || rel[0] == '/' || (n > 0 && rel[n - 1] == '/')) {
        return false;
    }
    const char *p = rel;
    while (*p) {
        const char *s = strchr(p, '/');
        const size_t l = s ? (size_t)(s - p) : strlen(p);
        if (!nom_valide(p, l)) {
            return false;
        }
        p += l + (s ? 1 : 0);
    }
    const int k = snprintf(abs, len, "%s%s%s", NID_SD_MOUNT_POINT, n ? "/" : "", rel);
    return k > 0 && (size_t)k < len;
}

/** Dossier par défaut : celui du jour, sinon le dossier du banc, sinon la racine. */
static void dossier_par_defaut(char *rel, size_t len)
{
    rel[0] = '\0';
    const char *base = nid_storage_base_dir();
    const size_t lm = strlen(NID_SD_MOUNT_POINT);
    if (strncmp(base, NID_SD_MOUNT_POINT "/", lm + 1) != 0) {
        return;
    }
    char jour[12];
    banc_heure_locale(jour, sizeof(jour), "%Y%m%d");
    char abs[CHEMIN_MAX];
    struct stat st;
    snprintf(abs, sizeof(abs), "%s/%s", base, jour);
    if (stat(abs, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(rel, len, "%s/%s", base + lm + 1, jour);
    } else if (stat(base, &st) == 0 && S_ISDIR(st.st_mode)) {
        strlcpy(rel, base + lm + 1, len);
    }
}

static esp_err_t fichiers_get(httpd_req_t *req)
{
    banc_web_compter();
    if (!nid_storage_is_mounted()) {
        return envoyer_resultat(req, BANC_HTTP_503, false, "Carte SD non montée");
    }
    char rel[REL_MAX + 1];
    if (!parametre(req, "d", rel, sizeof(rel))) {
        dossier_par_defaut(rel, sizeof(rel));
    }
    char abs[CHEMIN_MAX];
    if (!chemin_valide(rel, abs, sizeof(abs))) {
        return envoyer_resultat(req, BANC_HTTP_400, false, "Dossier invalide");
    }
    DIR *d = opendir(abs);
    if (d == NULL) {
        return envoyer_resultat(req, BANC_HTTP_404, false, "Dossier introuvable");
    }
    char parent[REL_MAX + 1];
    strlcpy(parent, rel, sizeof(parent));
    char *sl = strrchr(parent, '/');
    if (sl != NULL) {
        *sl = '\0';
    } else {
        parent[0] = '\0';
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char buf[1024];
    sb_t b;
    sb_init(&b, buf, sizeof(buf));
    sb_printf(&b, "{\"ok\":true,\"dossier\":");
    sb_str(&b, rel);
    sb_cle(&b, "parent");
    sb_str(&b, parent);
    sb_printf(&b, ",\"racine\":%s,\"suppression\":%s,\"entrees\":[", json_bool(rel[0] == '\0'),
              json_bool(SUPPRESSION_POSSIBLE));
    esp_err_t err = ESP_OK;
    int n = 0;
    bool tronque = false;
    struct dirent *ent;
    while (err == ESP_OK && (ent = readdir(d)) != NULL) {
        const char *nom = ent->d_name;
        if (strcmp(nom, ".") == 0 || strcmp(nom, "..") == 0) {
            continue;
        }
        if (n >= ENTREES_MAX) {
            tronque = true;
            break;
        }
        char chemin[CHEMIN_MAX + sizeof(ent->d_name) + 2];
        snprintf(chemin, sizeof(chemin), "%s/%s", abs, nom);
        struct stat st;
        const bool ok_stat = (stat(chemin, &st) == 0);
        char date[20] = "";
        if (ok_stat) {
            struct tm tm;
            localtime_r(&st.st_mtime, &tm);
            strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &tm);
        }
        sb_printf(&b, "%s{\"n\":", n ? "," : "");
        sb_str(&b, nom);
        sb_printf(&b, ",\"r\":%s,\"t\":%lld,\"m\":\"%s\",\"v\":%s}",
                  json_bool(ok_stat ? S_ISDIR(st.st_mode) : ent->d_type == DT_DIR),
                  ok_stat ? (long long)st.st_size : 0LL, date, json_bool(nom_valide(nom, strlen(nom))));
        n++;
        if (b.plein) {
            err = ESP_FAIL;
        } else if (b.len > sizeof(buf) - 256) {
            err = httpd_resp_send_chunk(req, buf, (ssize_t)b.len);
            sb_init(&b, buf, sizeof(buf));
        }
    }
    closedir(d);
    if (err == ESP_OK) {
        sb_printf(&b, "],\"tronque\":%s}", json_bool(tronque));
        err = httpd_resp_send_chunk(req, buf, (ssize_t)b.len);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

/* Téléchargement : confié à une tâche dédiée (le serveur reste disponible). */
typedef struct {
    httpd_req_t *req;
    char         chemin[CHEMIN_MAX];
    char         nom[NOM_MAX + 1];
    bool         en_ligne;
} envoi_t;

static QueueHandle_t s_fic_file;
static atomic_bool s_fic_occupe;
static char s_fic_chemin[CHEMIN_MAX];       /* fichier en cours d'envoi (sous s_fic_mux) */
static portMUX_TYPE s_fic_mux = portMUX_INITIALIZER_UNLOCKED;

static const char *type_mime(const char *nom)
{
    const char *pt = strrchr(nom, '.');
    if (pt == NULL) {
        return "application/octet-stream";
    }
    if (strcasecmp(pt, ".mp4") == 0) {
        return "video/mp4";
    }
    if (strcasecmp(pt, ".jpg") == 0 || strcasecmp(pt, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(pt, ".csv") == 0) {
        return "text/csv; charset=utf-8";
    }
    if (strcasecmp(pt, ".txt") == 0 || strcasecmp(pt, ".ini") == 0 || strcasecmp(pt, ".log") == 0) {
        return "text/plain; charset=utf-8";
    }
    return "application/octet-stream";
}

static void envoyer_fichier(const envoi_t *e)
{
    httpd_req_t *req = e->req;
    const int fd = open(e->chemin, O_RDONLY);
    if (fd < 0) {
        banc_web_texte(req, BANC_HTTP_404, "Fichier introuvable");
        return;
    }
    uint8_t *bloc = heap_caps_malloc(BLOC_FICHIER, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (bloc == NULL) {
        bloc = malloc(BLOC_FICHIER);
    }
    if (bloc == NULL) {
        close(fd);
        banc_web_texte(req, BANC_HTTP_503, "Mémoire insuffisante");
        return;
    }
    char dispo[NOM_MAX + 40];
    snprintf(dispo, sizeof(dispo), "%s; filename=\"%s\"", e->en_ligne ? "inline" : "attachment", e->nom);
    httpd_resp_set_type(req, type_mime(e->nom));
    httpd_resp_set_hdr(req, "Content-Disposition", dispo);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const int64_t t0 = esp_timer_get_time();
    uint64_t total = 0;
    esp_err_t err = ESP_OK;
    for (;;) {
        const ssize_t lu = read(fd, bloc, BLOC_FICHIER);
        if (lu < 0) {
            err = ESP_FAIL;
            break;
        }
        if (lu == 0) {
            break;
        }
        err = httpd_resp_send_chunk(req, (const char *)bloc, lu);
        if (err != ESP_OK) {
            break;
        }
        total += (uint64_t)lu;
        if (banc_enregistrement_en_cours()) {
            vTaskDelay(pdMS_TO_TICKS(PAUSE_REC_MS));    /* la carte SD sert d'abord la vidéo */
        }
    }
    close(fd);
    free(bloc);
    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    const int64_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "téléchargement %s : %s, %" PRIu64 " ko en %lld ms", e->nom, err == ESP_OK ? "terminé" : "interrompu",
             total / 1000ULL, (long long)ms);
}

static void tache_fichiers(void *arg)
{
    (void)arg;
    envoi_t e;
    for (;;) {
        if (xQueueReceive(s_fic_file, &e, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        portENTER_CRITICAL(&s_fic_mux);
        strlcpy(s_fic_chemin, e.chemin, sizeof(s_fic_chemin));
        portEXIT_CRITICAL(&s_fic_mux);
        envoyer_fichier(&e);
        httpd_req_async_handler_complete(e.req);
        portENTER_CRITICAL(&s_fic_mux);
        s_fic_chemin[0] = '\0';
        portEXIT_CRITICAL(&s_fic_mux);
        atomic_store(&s_fic_occupe, false);
    }
}

static bool en_cours_d_envoi(const char *chemin)
{
    portENTER_CRITICAL(&s_fic_mux);
    const bool oui = strcmp(s_fic_chemin, chemin) == 0;
    portEXIT_CRITICAL(&s_fic_mux);
    return oui;
}

/** Fichier (pas un dossier) de la carte, chemin validé. */
static esp_err_t fichier_cible(httpd_req_t *req, const char *rel, envoi_t *e)
{
    if (!nid_storage_is_mounted()) {
        envoyer_resultat(req, BANC_HTTP_503, false, "Carte SD non montée");
        return ESP_FAIL;
    }
    if (!chemin_valide(rel, e->chemin, sizeof(e->chemin)) || rel[0] == '\0') {
        envoyer_resultat(req, BANC_HTTP_400, false, "Nom de fichier invalide");
        return ESP_FAIL;
    }
    struct stat st;
    if (stat(e->chemin, &st) != 0 || !S_ISREG(st.st_mode)) {
        envoyer_resultat(req, BANC_HTTP_404, false, "Fichier introuvable");
        return ESP_FAIL;
    }
    const char *sl = strrchr(rel, '/');
    strlcpy(e->nom, sl ? sl + 1 : rel, sizeof(e->nom));
    return ESP_OK;
}

static esp_err_t fichier_get(httpd_req_t *req)
{
    banc_web_compter();
    char rel[REL_MAX + 1];
    envoi_t e = { 0 };
    if (!parametre(req, "f", rel, sizeof(rel))) {
        return envoyer_resultat(req, BANC_HTTP_400, false, "Fichier absent");
    }
    if (fichier_cible(req, rel, &e) != ESP_OK) {
        return ESP_OK;
    }
    char v[4];
    e.en_ligne = parametre(req, "vue", v, sizeof(v)) && v[0] == '1';
    bool attendu = false;
    if (!atomic_compare_exchange_strong(&s_fic_occupe, &attendu, true)) {
        return banc_web_texte(req, BANC_HTTP_503, "Un téléchargement est déjà en cours : réessayer à la fin");
    }
    if (httpd_req_async_handler_begin(req, &e.req) != ESP_OK) {
        atomic_store(&s_fic_occupe, false);
        return banc_web_texte(req, BANC_HTTP_503, "Serveur occupé : réessayer");
    }
    if (xQueueSend(s_fic_file, &e, 0) != pdTRUE) {
        banc_web_texte(e.req, BANC_HTTP_503, "Un téléchargement est déjà en cours : réessayer à la fin");
        httpd_req_async_handler_complete(e.req);
        atomic_store(&s_fic_occupe, false);
    }
    return ESP_OK;
}

static esp_err_t supprimer_post(httpd_req_t *req)
{
    banc_web_compter();
#if !CONFIG_BANC_WEB_SUPPRESSION
    return envoyer_resultat(req, BANC_HTTP_403, false, "Suppression désactivée (menu « Banc d'essai › Wi-Fi »)");
#else
    char rel[REL_MAX + 1];
    envoi_t e = { 0 };
    if (!champ_formulaire(req, "f", rel, sizeof(rel))) {
        return envoyer_resultat(req, BANC_HTTP_400, false, "Fichier absent");
    }
    if (fichier_cible(req, rel, &e) != ESP_OK) {
        return ESP_OK;
    }
    const size_t n = strlen(e.nom);
    if (n > 4 && strcasecmp(e.nom + n - 4, ".tmp") == 0 && banc_occupe()) {
        return envoyer_resultat(req, BANC_HTTP_409, false, "Fichier en cours d'écriture : réessayer à la fin");
    }
    if (en_cours_d_envoi(e.chemin)) {
        return envoyer_resultat(req, BANC_HTTP_409, false, "Fichier en cours de téléchargement : réessayer à la fin");
    }
    if (unlink(e.chemin) != 0) {
        return envoyer_resultat(req, BANC_HTTP_500, false, "Suppression impossible (carte protégée ou retirée ?)");
    }
    ESP_LOGI(TAG, "fichier supprimé depuis la page web : %s", rel);
    nid_storage_journal("suppression_web", "%s", rel);
    banc_lire_espace_sd();
    return envoyer_resultat(req, NULL, true, "Fichier supprimé");
#endif
}

/* ------------------------------------------------------------------------- */
/* Serveur                                                                    */
/* ------------------------------------------------------------------------- */

static const httpd_uri_t s_uris[] = {
    { .uri = "/",              .method = HTTP_GET,  .handler = page_get },
    { .uri = "/api/etat",      .method = HTTP_GET,  .handler = etat_get },
    { .uri = "/api/reglages",  .method = HTTP_GET,  .handler = reglages_get },
    { .uri = "/api/console",   .method = HTTP_POST, .handler = console_post },
    { .uri = "/api/heure",     .method = HTTP_POST, .handler = heure_post },
    { .uri = "/stream",        .method = HTTP_GET,  .handler = banc_vue_get },
    { .uri = "/api/fichiers",  .method = HTTP_GET,  .handler = fichiers_get },
    { .uri = "/fichier",       .method = HTTP_GET,  .handler = fichier_get },
    { .uri = "/api/supprimer", .method = HTTP_POST, .handler = supprimer_post },
};

_Static_assert(sizeof(s_uris) / sizeof(s_uris[0]) <= HTTPD_GESTIONNAIRES, "HTTPD_GESTIONNAIRES trop petit");

esp_err_t banc_web_demarrer(void)
{
    if (s_serveur != NULL) {
        return ESP_OK;
    }
    if (s_console_mutex == NULL) {
        s_console_mutex = xSemaphoreCreateMutex();
        s_sortie = heap_caps_malloc(SORTIE_MAX, MALLOC_CAP_SPIRAM);
        if (s_sortie == NULL) {
            s_sortie = malloc(SORTIE_MAX);
        }
        s_fic_file = xQueueCreate(1, sizeof(envoi_t));
        if (s_console_mutex == NULL || s_sortie == NULL || s_fic_file == NULL ||
            xTaskCreate(tache_fichiers, "banc_web_fic", FIC_PILE, NULL, FIC_PRIO, NULL) != pdPASS) {
            ESP_LOGE(TAG, "serveur web : mémoire insuffisante");
            return ESP_ERR_NO_MEM;
        }
    }
    const esp_err_t err_vue = banc_vue_init();
    if (err_vue != ESP_OK) {
        ESP_LOGW(TAG, "vue caméra indisponible (%s) : page sans vue", esp_err_to_name(err_vue));
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = HTTPD_PILE;
    cfg.task_priority = HTTPD_PRIO;
    cfg.max_uri_handlers = HTTPD_GESTIONNAIRES;
    cfg.max_open_sockets = HTTPD_SOCKETS;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 2;      /* délais courts : un appareil muet ne bloque pas le serveur */
    cfg.send_wait_timeout = 3;
    esp_err_t err = httpd_start(&s_serveur, &cfg);
    if (err != ESP_OK) {
        s_serveur = NULL;
        ESP_LOGE(TAG, "démarrage du serveur web impossible : %s", esp_err_to_name(err));
        return err;
    }
    for (size_t i = 0; i < sizeof(s_uris) / sizeof(s_uris[0]); i++) {
        err = httpd_register_uri_handler(s_serveur, &s_uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "gestionnaire %s : %s", s_uris[i].uri, esp_err_to_name(err));
            httpd_stop(s_serveur);
            s_serveur = NULL;
            return err;
        }
    }
    ESP_LOGI(TAG, "tableau de bord web démarré (port 80)");
    return ESP_OK;
}

#endif /* CONFIG_BANC_F_WIFI */
