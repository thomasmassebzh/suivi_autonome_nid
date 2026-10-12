/*
 * nid_config — réglages du système (FS1.4 : paramétrage sans reprogrammation)
 *
 * Une table unique décrit chaque clé (type, champ, plage, unité, exigence du
 * CdCF, explication) ; elle sert à la lecture de config.ini, au contrôle des
 * valeurs, à la production du fichier commenté et au JSON de la page
 * d'installation.
 *
 * Lecture tolérante : BOM UTF-8, fins de ligne CRLF, espaces autour du « = »,
 * commentaires « # » / « ; » (y compris en fin de ligne), sections ignorées,
 * clés insensibles à la casse, valeurs entre guillemets. Une ligne erronée est
 * signalée (numéro de ligne) et ignorée, la lecture continue.
 *
 * Écriture sûre sur FAT (FP1.7) : voir nid_config_write_file_atomic().
 *
 * Ce fichier ne dépend que de la bibliothèque C (stdio, fsync) et de esp_log :
 * il est aussi compilé sur PC pour les tests (test_host/).
 */
#include "nid_config.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"

static const char *TAG = "nid_config";

#define LINE_MAX_LEN       256          /* longueur maximale d'une ligne de config.ini */
#define VALUE_MAX_LEN      128          /* longueur maximale d'une valeur */
#define LOAD_MAX_BYTES     (64 * 1024)  /* au-delà, ce n'est plus un fichier de réglages */
#define LOAD_MAX_WARNINGS  20           /* avertissements détaillés par lecture */
#define FILE_PATH_LEN      256          /* chemins des fichiers (écriture atomique) */

/* ------------------------------------------------------------------------- */
/* Outils texte (ASCII, indépendants de la locale)                            */
/* ------------------------------------------------------------------------- */

/* Copie bornée, toujours terminée. @return true si la source a été tronquée. */
static bool copy_str(char *dst, size_t len, const char *src)
{
    if (len == 0) {
        return true;
    }
    size_t n = strlen(src);
    bool truncated = (n >= len);
    if (truncated) {
        n = len - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return truncated;
}

static bool is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char to_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (to_lower(*a) != to_lower(*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

/* Retire les blancs de début et de fin (modifie la chaîne). */
static char *trim(char *s)
{
    while (is_blank(*s)) {
        s++;
    }
    char *e = s + strlen(s);
    while (e > s && is_blank(e[-1])) {
        e--;
    }
    *e = '\0';
    return s;
}

/* Vérifie une séquence UTF-8 (sans surlongueur ni caractère de contrôle). */
static bool utf8_text_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned char c = *p;
        int follow;
        if (c < 0x20 || c == 0x7F) {
            return false;
        } else if (c < 0x80) {
            follow = 0;
        } else if (c >= 0xC2 && c <= 0xDF) {
            follow = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            follow = 2;
        } else if (c >= 0xF0 && c <= 0xF4) {
            follow = 3;
        } else {
            return false;
        }
        p++;
        for (int i = 0; i < follow; i++, p++) {
            if ((*p & 0xC0) != 0x80) {
                return false;
            }
        }
    }
    return true;
}

/* Nombre décimal sans exposant, 5 décimales au plus, zéros inutiles retirés. */
static void format_float(float v, char *out, size_t len)
{
    if (!isfinite(v)) {
        copy_str(out, len, "0");
        return;
    }
    int n = snprintf(out, len, "%.5f", (double)v);
    if (n <= 0 || (size_t)n >= len) {
        copy_str(out, len, "0");
        return;
    }
    char *dot = strchr(out, '.');
    if (dot) {
        char *e = out + n;
        while (e > dot + 1 && e[-1] == '0') {
            e--;
        }
        if (e == dot + 1) {
            e = dot;
        }
        *e = '\0';
    }
    if (strcmp(out, "-0") == 0) {
        copy_str(out, len, "0");
    }
}

/* ------------------------------------------------------------------------- */
/* Tampon de sortie borné                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t len;
    size_t pos;
    bool   overflow;
} out_t;

static void out_write(out_t *o, const char *s, size_t n)
{
    if (o->overflow) {
        return;
    }
    if (o->pos + n >= o->len) {
        o->overflow = true;
        return;
    }
    memcpy(o->buf + o->pos, s, n);
    o->pos += n;
    o->buf[o->pos] = '\0';
}

static void out_str(out_t *o, const char *s)
{
    out_write(o, s, strlen(s));
}

static void out_fmt(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void out_fmt(out_t *o, const char *fmt, ...)
{
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        o->overflow = true;
        return;
    }
    out_write(o, tmp, (size_t)n);
}

/* ------------------------------------------------------------------------- */
/* Table des clés                                                             */
/* ------------------------------------------------------------------------- */

typedef enum {
    KT_ID = 0,  /* identifiant : 1 à 15 caractères [A-Za-z0-9_-] */
    KT_TZ,      /* fuseau horaire POSIX */
    KT_UINT,    /* entier non signé borné */
    KT_INT,     /* entier signé borné */
    KT_BOOL,    /* oui / non */
    KT_RES,     /* résolution vidéo (deux champs) */
    KT_MODE,    /* auto / jour / nuit */
    KT_FLOAT,   /* nombre décimal borné */
    KT_SSID,    /* nom de réseau Wi-Fi : 0 à 32 octets UTF-8 */
    KT_PASS,    /* mot de passe WPA2 : 8 à 63 caractères ASCII imprimables */
} key_type_t;

typedef struct {
    const char *key;      /* clé INI (minuscules) */
    const char *section;  /* section INI (lisibilité uniquement) */
    key_type_t  type;
    uint16_t    offset;   /* position du champ dans nid_config_t */
    uint8_t     size;     /* taille du champ en octets */
    int32_t     min;      /* bornes des entiers et des décimaux */
    int32_t     max;
    const char *unit;     /* unité affichée dans config.ini */
    const char *ref;      /* exigence du cahier des charges */
    const char *doc;      /* explication (lignes séparées par \n) */
} key_def_t;

#define FIELD(f) (uint16_t)offsetof(nid_config_t, f), (uint8_t)sizeof(((nid_config_t *)0)->f)

static const key_def_t s_keys[] = {
    /* [systeme] */
    {
        "id_systeme", "systeme", KT_ID, FIELD(system_id), 1, NID_CONFIG_ID_LEN - 1, "", "FP1.6",
        "Identifiant du système (boîtier caméra) : repris dans le nom de chaque\n"
        "fichier et incrusté dans les images.\n"
        "1 à 15 caractères parmi : lettres sans accent, chiffres, _ et -."
    },
    {
        "id_nid", "systeme", KT_ID, FIELD(nest_id), 1, NID_CONFIG_ID_LEN - 1, "", "FP1.6",
        "Identifiant du nid surveillé : repris dans le nom de chaque fichier et\n"
        "incrusté dans les images. À changer pour chaque nouveau nid.\n"
        "1 à 15 caractères parmi : lettres sans accent, chiffres, _ et -."
    },
    {
        "fuseau_horaire", "systeme", KT_TZ, FIELD(timezone), 4, NID_CONFIG_TZ_LEN - 1, "", "FP1.6",
        "Fuseau horaire (format POSIX « TZ ») de l'heure locale utilisée dans les\n"
        "noms de fichiers, les images et le journal. L'horloge interne reste en\n"
        "temps universel (UTC).\n"
        "AST4 = Martinique et Guadeloupe (UTC-4, sans heure d'été). Attention :\n"
        "en notation POSIX le signe est inversé (AST4 = 4 h de moins que UTC).\n"
        "Autres exemples : UTC0 ; CET-1CEST,M3.5.0,M10.5.0/3 (France métropolitaine)."
    },
    /* [video] */
    {
        "duree_sequence_s", "video", KT_UINT, FIELD(seq_duration_s), 1, 300, "s", "FP1.4",
        "Durée d'une séquence vidéo enregistrée après une détection."
    },
    {
        "delai_entre_declenchements_s", "video", KT_UINT, FIELD(trigger_delay_s), 0, 3600, "s", "FP1.4",
        "Délai minimal entre deux déclenchements : aucune nouvelle vidéo pendant\n"
        "ce délai. 0 = désactivé : les séquences s'enchaînent tant que l'animal\n"
        "est détecté."
    },
    {
        "resolution", "video", KT_RES, FIELD(video_width), 0, 0, "", "FP1.2",
        "Définition de la vidéo, en pixels (largeur x hauteur)."
    },
    {
        "images_par_seconde", "video", KT_UINT, FIELD(video_fps), 10, 30, "img/s", "FP1.2",
        "Cadence de la vidéo (le cahier des charges demande au moins 15 img/s)."
    },
    {
        "debit_kbps", "video", KT_UINT, FIELD(video_bitrate_kbps), 500, 10000, "kbit/s", "FP1.2",
        "Débit de la vidéo H.264 : plus élevé = meilleure qualité mais fichiers\n"
        "plus gros (2500 kbit/s = environ 19 Mo par minute de vidéo)."
    },
    {
        "son", "video", KT_BOOL, FIELD(audio_enabled), 0, 1, "", "FP1.3",
        "Enregistrement du son (piste audio AAC) dans les vidéos."
    },
    /* [photo] */
    {
        "intervalle_suivi_min", "photo", KT_UINT, FIELD(photo_interval_min), 0, 1440, "min", "FP1.5",
        "Intervalle entre deux photos de suivi du nid. 0 = pas de photo de suivi."
    },
    {
        "qualite_jpeg", "photo", KT_UINT, FIELD(jpeg_quality), 10, 100, "", "FP1.5",
        "Qualité des photos JPEG : 100 = meilleure qualité, fichiers plus gros."
    },
    /* [detection] */
    {
        "sensibilite", "detection", KT_UINT, FIELD(pir_sensitivity), 1, 5, "", "FP1.1",
        "Sensibilité de la détection de mouvement (capteur PIR) : 1 = la moins\n"
        "sensible (moins de fausses détections), 5 = la plus sensible.\n"
        "À ajuster selon l'espèce suivie et le site."
    },
    {
        "chauffe_pir_s", "detection", KT_UINT, FIELD(pir_warmup_s), 0, 120, "s", "FP1.1",
        "Temps de stabilisation du capteur PIR après la mise sous tension :\n"
        "aucune détection n'est prise en compte pendant ce temps."
    },
    /* [nuit] */
    {
        "mode", "nuit", KT_MODE, FIELD(night_mode), 0, 2, "", "FP1.2",
        "Mode jour/nuit. auto : selon le lever et le coucher du soleil calculés\n"
        "pour le lieu ci-dessous ; jour : toujours en mode jour (filtre\n"
        "infrarouge en place, LED infrarouges éteintes) ; nuit : toujours en mode\n"
        "nuit (LED infrarouges allumées pendant les prises de vue)."
    },
    {
        "latitude", "nuit", KT_FLOAT, FIELD(latitude), -90, 90, "degrés", "FP1.2",
        "Latitude du nid en degrés décimaux (nord positif), pour le calcul du\n"
        "lever et du coucher du soleil."
    },
    {
        "longitude", "nuit", KT_FLOAT, FIELD(longitude), -180, 180, "degrés", "FP1.2",
        "Longitude du nid en degrés décimaux (est positif, ouest négatif :\n"
        "Martinique = environ -61)."
    },
    {
        "avance_crepuscule_min", "nuit", KT_INT, FIELD(dusk_advance_min), -180, 240, "min", "FP1.2",
        "Le mode nuit commence N minutes avant le coucher du soleil (valeur\n"
        "négative : après le coucher)."
    },
    {
        "retard_aube_min", "nuit", KT_INT, FIELD(dawn_delay_min), -180, 240, "min", "FP1.2",
        "Le mode nuit se termine N minutes après le lever du soleil (valeur\n"
        "négative : avant le lever)."
    },
    {
        "puissance_ir", "nuit", KT_UINT, FIELD(ir_power_pct), 0, 100, "%", "FP1.2, FC1.1",
        "Puissance des LED infrarouges 940 nm (invisibles pour l'oiseau) pendant\n"
        "les prises de vue de nuit. 0 = LED éteintes."
    },
    {
        "nuit_monochrome", "nuit", KT_BOOL, FIELD(night_monochrome), 0, 1, "", "FP1.2",
        "Images en noir et blanc la nuit (recommandé avec l'éclairage infrarouge)."
    },
    /* [energie] */
    {
        "seuil_batterie_basse_mv", "energie", KT_UINT, FIELD(batt_low_mv), 2800, 4500, "mV", "FC3.4",
        "Tension de batterie basse : en dessous, plus de vidéo, seulement les\n"
        "photos de suivi (économie d'énergie)."
    },
    {
        "seuil_batterie_critique_mv", "energie", KT_UINT, FIELD(batt_critical_mv), 2800, 4500, "mV", "FC3.4",
        "Tension de batterie critique : en dessous, arrêt propre du système\n"
        "(fichiers fermés) jusqu'à la recharge. Doit être inférieure au seuil bas."
    },
    /* [installation] */
    {
        "wifi_ssid", "installation", KT_SSID, FIELD(wifi_ssid), 0, NID_CONFIG_SSID_LEN - 1, "", "FS1.2",
        "Nom du réseau Wi-Fi temporaire créé par la caméra en mode installation.\n"
        "Vide = « NID- » suivi de id_systeme (ex. NID-CAM01). 0 à 32 caractères."
    },
    {
        "wifi_mot_de_passe", "installation", KT_PASS, FIELD(wifi_password), 8, NID_CONFIG_PASS_LEN - 2, "", "FS1.2",
        "Mot de passe de ce réseau Wi-Fi (WPA2) : 8 à 63 caractères sans accent.\n"
        "Le mettre entre guillemets s'il contient # ou ; ou des espaces au début\n"
        "ou à la fin."
    },
    {
        "duree_max_min", "installation", KT_UINT, FIELD(install_timeout_min), 1, 60, "min", "FC1.6",
        "Durée maximale du mode installation : la surveillance démarre d'office\n"
        "ensuite."
    },
};

#define KEY_COUNT (sizeof(s_keys) / sizeof(s_keys[0]))
_Static_assert(KEY_COUNT <= 32, "le masque des clés lues tient sur 32 bits");

typedef struct {
    const char *title;    /* titre affiché dans config.ini */
    const char *section;
} section_doc_t;

static const section_doc_t s_sections[] = {
    { "Système : identifiants et heure", "systeme" },
    { "Vidéo sur détection", "video" },
    { "Photos de suivi du nid", "photo" },
    { "Détection de mouvement", "detection" },
    { "Prises de vue de nuit (éclairage infrarouge)", "nuit" },
    { "Énergie", "energie" },
    { "Mode installation (Wi-Fi et réglages depuis un smartphone)", "installation" },
};

typedef struct {
    uint16_t w;
    uint16_t h;
} resolution_t;

static const resolution_t s_resolutions[] = {
    { 1920, 1080 },
    { 1280, 960 },
    { 1280, 720 },
};

static int find_key(const char *key)
{
    for (size_t i = 0; i < KEY_COUNT; i++) {
        if (str_ieq(key, s_keys[i].key)) {
            return (int)i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Accès aux champs                                                           */
/* ------------------------------------------------------------------------- */

static void *field_ptr(nid_config_t *cfg, const key_def_t *k)
{
    return (uint8_t *)cfg + k->offset;
}

static const void *field_cptr(const nid_config_t *cfg, const key_def_t *k)
{
    return (const uint8_t *)cfg + k->offset;
}

static int32_t field_get_int(const nid_config_t *cfg, const key_def_t *k)
{
    const uint8_t *p = field_cptr(cfg, k);
    if (k->size == 1) {
        return (k->type == KT_INT) ? (int32_t)(int8_t)p[0] : (int32_t)p[0];
    }
    if (k->size == 2) {
        uint16_t v;
        memcpy(&v, p, sizeof(v));
        return (k->type == KT_INT) ? (int32_t)(int16_t)v : (int32_t)v;
    }
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void field_set_int(nid_config_t *cfg, const key_def_t *k, int32_t v)
{
    uint8_t *p = field_ptr(cfg, k);
    if (k->size == 1) {
        p[0] = (uint8_t)v;
    } else if (k->size == 2) {
        uint16_t u = (uint16_t)v;
        memcpy(p, &u, sizeof(u));
    } else {
        memcpy(p, &v, sizeof(v));
    }
}

/* Booléen lu octet par octet : une valeur restaurée invalide reste détectable. */
static uint8_t field_get_bool_raw(const nid_config_t *cfg, const key_def_t *k)
{
    return *(const uint8_t *)field_cptr(cfg, k);
}

static void field_set_bool(nid_config_t *cfg, const key_def_t *k, bool v)
{
    *(uint8_t *)field_ptr(cfg, k) = v ? 1 : 0;
}

static float field_get_float(const nid_config_t *cfg, const key_def_t *k)
{
    float v;
    memcpy(&v, field_cptr(cfg, k), sizeof(v));
    return v;
}

static void field_set_float(nid_config_t *cfg, const key_def_t *k, float v)
{
    memcpy(field_ptr(cfg, k), &v, sizeof(v));
}

static const char *field_get_str(const nid_config_t *cfg, const key_def_t *k)
{
    return (const char *)field_cptr(cfg, k);
}

/* ------------------------------------------------------------------------- */
/* Contrôle et conversion des valeurs                                         */
/* ------------------------------------------------------------------------- */

static bool id_valid(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > NID_CONFIG_ID_LEN - 1) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!is_alpha(c) && !is_digit(c) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

/*
 * Fuseau horaire POSIX : nom de 3 lettres au moins (ou <...>), puis décalage
 * [+-]h[h][:mm[:ss]], puis éventuellement l'heure d'été et ses règles. Seuls
 * les caractères de cette syntaxe sont acceptés (pas de « # » ni de « ; »).
 */
static bool tz_valid(const char *s)
{
    size_t n = strlen(s);
    if (n < 4 || n > NID_CONFIG_TZ_LEN - 1) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!is_alpha(c) && !is_digit(c) && !strchr("+-:,./<>", c)) {
            return false;
        }
    }
    const char *p = s;
    if (*p == '<') {
        const char *q = strchr(p, '>');
        if (!q || q - p - 1 < 3) {
            return false;
        }
        p = q + 1;
    } else {
        int letters = 0;
        while (is_alpha(*p)) {
            p++;
            letters++;
        }
        if (letters < 3) {
            return false;
        }
    }
    if (*p == '+' || *p == '-') {
        p++;
    }
    return is_digit(*p);
}

static bool ssid_valid(const char *s)
{
    return strlen(s) <= NID_CONFIG_SSID_LEN - 1 && utf8_text_valid(s);
}

static bool pass_valid(const char *s)
{
    size_t n = strlen(s);
    if (n < 8 || n > 63) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

static bool res_supported(uint16_t w, uint16_t h)
{
    for (size_t i = 0; i < sizeof(s_resolutions) / sizeof(s_resolutions[0]); i++) {
        if (s_resolutions[i].w == w && s_resolutions[i].h == h) {
            return true;
        }
    }
    return false;
}

/* Entier décimal signé, sans caractère parasite. */
static bool parse_int(const char *s, int32_t *out)
{
    const char *p = s;
    bool neg = false;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }
    if (!is_digit(*p)) {
        return false;
    }
    int64_t v = 0;
    for (; is_digit(*p); p++) {
        v = v * 10 + (*p - '0');
        if (v > 100000000) {
            return false;
        }
    }
    if (*p != '\0') {
        return false;
    }
    *out = (int32_t)(neg ? -v : v);
    return true;
}

/* Décimal avec point ou virgule, sans exposant ni « nan »/« inf ». */
static bool parse_float(const char *s, float *out)
{
    char buf[32];
    size_t n = 0;
    bool digit = false;
    bool sep = false;
    const char *p = s;
    if (*p == '+' || *p == '-') {
        buf[n++] = *p++;
    }
    for (; *p; p++) {
        char c = *p;
        if (is_digit(c)) {
            digit = true;
        } else if ((c == '.' || c == ',') && !sep) {
            sep = true;
            c = '.';
        } else {
            return false;
        }
        if (n + 1 >= sizeof(buf)) {
            return false;
        }
        buf[n++] = c;
    }
    if (!digit) {
        return false;
    }
    buf[n] = '\0';
    char *end = NULL;
    float v = strtof(buf, &end);
    if (end == NULL || *end != '\0' || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

static bool parse_bool(const char *s, bool *out)
{
    static const char *const yes[] = { "oui", "1", "true", "yes", "on", "vrai" };
    static const char *const no[] = { "non", "0", "false", "no", "off", "faux" };
    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
        if (str_ieq(s, yes[i])) {
            *out = true;
            return true;
        }
        if (str_ieq(s, no[i])) {
            *out = false;
            return true;
        }
    }
    return false;
}

static bool parse_mode(const char *s, nid_night_mode_t *out)
{
    if (str_ieq(s, "auto")) {
        *out = NID_NIGHT_MODE_AUTO;
    } else if (str_ieq(s, "jour")) {
        *out = NID_NIGHT_MODE_DAY;
    } else if (str_ieq(s, "nuit")) {
        *out = NID_NIGHT_MODE_NIGHT;
    } else {
        return false;
    }
    return true;
}

/* « 1920x1080 » (x, X ou ×, espaces tolérés) ou « 1080p » / « 960p » / « 720p ». */
static bool parse_res(const char *s, uint16_t *w, uint16_t *h)
{
    if (str_ieq(s, "1080p")) {
        *w = 1920;
        *h = 1080;
        return true;
    }
    if (str_ieq(s, "960p")) {
        *w = 1280;
        *h = 960;
        return true;
    }
    if (str_ieq(s, "720p")) {
        *w = 1280;
        *h = 720;
        return true;
    }
    char a[12];
    char b[12];
    size_t na = 0;
    size_t nb = 0;
    bool second = false;
    for (const char *p = s; *p; p++) {
        if (is_blank(*p)) {
            continue;
        }
        if (!second && (*p == 'x' || *p == 'X')) {
            second = true;
            continue;
        }
        if (!second && (unsigned char)p[0] == 0xC3 && (unsigned char)p[1] == 0x97) { /* « × » */
            second = true;
            p++;
            continue;
        }
        if (!is_digit(*p)) {
            return false;
        }
        if (second) {
            if (nb + 1 >= sizeof(b)) {
                return false;
            }
            b[nb++] = *p;
        } else {
            if (na + 1 >= sizeof(a)) {
                return false;
            }
            a[na++] = *p;
        }
    }
    if (!second || na == 0 || nb == 0) {
        return false;
    }
    a[na] = '\0';
    b[nb] = '\0';
    int32_t vw;
    int32_t vh;
    if (!parse_int(a, &vw) || !parse_int(b, &vh) || vw > 65535 || vh > 65535) {
        return false;
    }
    if (!res_supported((uint16_t)vw, (uint16_t)vh)) {
        return false;
    }
    *w = (uint16_t)vw;
    *h = (uint16_t)vh;
    return true;
}

/* Valeur courante sous forme de texte (INI, journaux, JSON). */
static void format_value(const nid_config_t *cfg, const key_def_t *k, char *out, size_t len)
{
    switch (k->type) {
    case KT_ID:
    case KT_TZ:
    case KT_SSID:
    case KT_PASS:
        copy_str(out, len, field_get_str(cfg, k));
        break;
    case KT_UINT:
    case KT_INT:
        snprintf(out, len, "%ld", (long)field_get_int(cfg, k));
        break;
    case KT_BOOL:
        copy_str(out, len, field_get_bool_raw(cfg, k) ? "oui" : "non");
        break;
    case KT_RES:
        snprintf(out, len, "%ux%u", (unsigned)cfg->video_width, (unsigned)cfg->video_height);
        break;
    case KT_MODE:
        copy_str(out, len, nid_config_night_mode_str(cfg->night_mode));
        break;
    case KT_FLOAT:
        format_float(field_get_float(cfg, k), out, len);
        break;
    }
}

/* Valeur montrée dans les journaux (le mot de passe n'y apparaît pas). */
static void format_value_log(const nid_config_t *cfg, const key_def_t *k, char *out, size_t len)
{
    if (k->type == KT_PASS) {
        copy_str(out, len, "(masqué)");
    } else {
        format_value(cfg, k, out, len);
    }
}

/* Ce qui est attendu pour une clé (messages d'erreur, config.ini). */
static void expected_text(const key_def_t *k, char *out, size_t len)
{
    switch (k->type) {
    case KT_ID:
        copy_str(out, len, "1 à 15 caractères : lettres sans accent, chiffres, _ ou -");
        break;
    case KT_TZ:
        copy_str(out, len, "fuseau horaire POSIX, ex. AST4");
        break;
    case KT_UINT:
    case KT_INT:
        snprintf(out, len, "entier de %ld à %ld%s%s", (long)k->min, (long)k->max,
                 k->unit[0] ? " " : "", k->unit);
        break;
    case KT_BOOL:
        copy_str(out, len, "oui ou non");
        break;
    case KT_RES:
        copy_str(out, len, "1920x1080, 1280x960 ou 1280x720");
        break;
    case KT_MODE:
        copy_str(out, len, "auto, jour ou nuit");
        break;
    case KT_FLOAT:
        snprintf(out, len, "nombre de %ld à %ld %s", (long)k->min, (long)k->max, k->unit);
        break;
    case KT_SSID:
        copy_str(out, len, "0 à 32 caractères");
        break;
    case KT_PASS:
        copy_str(out, len, "8 à 63 caractères sans accent");
        break;
    }
}

/*
 * Analyse « value » pour la clé k et l'applique à cfg (inchangé en cas
 * d'erreur). line > 0 : numéro de ligne de config.ini pour les messages.
 */
static esp_err_t apply_value(nid_config_t *cfg, const key_def_t *k, const char *value, int line, bool quiet)
{
    bool ok = false;
    switch (k->type) {
    case KT_ID:
        ok = id_valid(value);
        if (ok) {
            copy_str(field_ptr(cfg, k), k->size, value);
        }
        break;
    case KT_TZ:
        ok = tz_valid(value);
        if (ok) {
            copy_str(field_ptr(cfg, k), k->size, value);
        }
        break;
    case KT_SSID:
        ok = ssid_valid(value);
        if (ok) {
            copy_str(field_ptr(cfg, k), k->size, value);
        }
        break;
    case KT_PASS:
        ok = pass_valid(value);
        if (ok) {
            copy_str(field_ptr(cfg, k), k->size, value);
        }
        break;
    case KT_UINT:
    case KT_INT: {
        int32_t v;
        ok = parse_int(value, &v) && v >= k->min && v <= k->max;
        if (ok) {
            field_set_int(cfg, k, v);
        }
        break;
    }
    case KT_BOOL: {
        bool v;
        ok = parse_bool(value, &v);
        if (ok) {
            field_set_bool(cfg, k, v);
        }
        break;
    }
    case KT_RES: {
        uint16_t w;
        uint16_t h;
        ok = parse_res(value, &w, &h);
        if (ok) {
            cfg->video_width = w;
            cfg->video_height = h;
        }
        break;
    }
    case KT_MODE: {
        nid_night_mode_t m;
        ok = parse_mode(value, &m);
        if (ok) {
            cfg->night_mode = m;
        }
        break;
    }
    case KT_FLOAT: {
        float v;
        ok = parse_float(value, &v) && v >= (float)k->min && v <= (float)k->max;
        if (ok) {
            field_set_float(cfg, k, v);
        }
        break;
    }
    }
    if (ok) {
        return ESP_OK;
    }
    if (!quiet) {
        char expected[96];
        char current[NID_CONFIG_PASS_LEN];
        char where[24] = "";
        expected_text(k, expected, sizeof(expected));
        format_value_log(cfg, k, current, sizeof(current));
        if (line > 0) {
            snprintf(where, sizeof(where), "ligne %d : ", line);
        }
        ESP_LOGW(TAG, "%svaleur invalide pour « %s » : « %s » (attendu : %s) ; valeur conservée : %s",
                 where, k->key, (k->type == KT_PASS) ? "(masqué)" : value, expected, current);
    }
    return ESP_ERR_INVALID_ARG;
}

/* ------------------------------------------------------------------------- */
/* API : valeurs par défaut, clé/valeur, validation                           */
/* ------------------------------------------------------------------------- */

void nid_config_set_defaults(nid_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    copy_str(cfg->system_id, sizeof(cfg->system_id), "CAM01");
    copy_str(cfg->nest_id, sizeof(cfg->nest_id), "NID01");
    copy_str(cfg->timezone, sizeof(cfg->timezone), "AST4");
    cfg->seq_duration_s = 30;
    cfg->trigger_delay_s = 60;
    cfg->video_width = 1920;
    cfg->video_height = 1080;
    cfg->video_fps = 15;
    cfg->video_bitrate_kbps = 2500;
    cfg->audio_enabled = true;
    cfg->photo_interval_min = 15;
    cfg->jpeg_quality = 80;
    cfg->pir_sensitivity = 3;
    cfg->pir_warmup_s = 60;
    cfg->night_mode = NID_NIGHT_MODE_AUTO;
    cfg->latitude = 14.77f;
    cfg->longitude = -60.89f;
    cfg->dusk_advance_min = 30;
    cfg->dawn_delay_min = 30;
    cfg->ir_power_pct = 100;
    cfg->night_monochrome = true;
    cfg->batt_low_mv = 3450;
    cfg->batt_critical_mv = 3350;
    cfg->wifi_ssid[0] = '\0';
    copy_str(cfg->wifi_password, sizeof(cfg->wifi_password), "moqueur-nid");
    cfg->install_timeout_min = 15;
}

esp_err_t nid_config_set(nid_config_t *cfg, const char *key, const char *value)
{
    if (cfg == NULL || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char k[40];
    char v[VALUE_MAX_LEN];
    if (copy_str(k, sizeof(k), key)) {
        ESP_LOGW(TAG, "clé inconnue « %.39s… » : ignorée", key);
        return ESP_ERR_NOT_FOUND;
    }
    int idx = find_key(trim(k));
    if (idx < 0) {
        ESP_LOGW(TAG, "clé inconnue « %s » : ignorée", key);
        return ESP_ERR_NOT_FOUND;
    }
    if (copy_str(v, sizeof(v), value)) {
        ESP_LOGW(TAG, "valeur trop longue pour « %s » : ignorée", s_keys[idx].key);
        return ESP_ERR_INVALID_ARG;
    }
    /* Espaces de début/fin non significatifs, sauf dans le mot de passe et le SSID. */
    const char *val = (s_keys[idx].type == KT_PASS || s_keys[idx].type == KT_SSID) ? v : trim(v);
    return apply_value(cfg, &s_keys[idx], val, 0, false);
}

esp_err_t nid_config_validate(nid_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nid_config_t def;
    nid_config_set_defaults(&def);
    int fixes = 0;

    for (size_t i = 0; i < KEY_COUNT; i++) {
        const key_def_t *k = &s_keys[i];
        switch (k->type) {
        case KT_ID: {
            char *s = field_ptr(cfg, k);
            s[k->size - 1] = '\0';
            char clean[NID_CONFIG_ID_LEN];
            if (nid_config_sanitize_id(s, clean, sizeof(clean)) == 0) {
                copy_str(clean, sizeof(clean), field_get_str(&def, k));
            }
            if (strcmp(clean, s) != 0) {
                ESP_LOGW(TAG, "%s « %s » corrigé en « %s »", k->key, s, clean);
                copy_str(s, k->size, clean);
                fixes++;
            }
            break;
        }
        case KT_TZ: {
            char *s = field_ptr(cfg, k);
            s[k->size - 1] = '\0';
            if (!tz_valid(s)) {
                ESP_LOGW(TAG, "%s « %s » invalide : %s", k->key, s, field_get_str(&def, k));
                copy_str(s, k->size, field_get_str(&def, k));
                fixes++;
            }
            break;
        }
        case KT_SSID: {
            char *s = field_ptr(cfg, k);
            s[k->size - 1] = '\0';
            if (!ssid_valid(s)) {
                ESP_LOGW(TAG, "%s invalide : nom par défaut (NID-<id_systeme>)", k->key);
                s[0] = '\0';
                fixes++;
            }
            break;
        }
        case KT_PASS: {
            char *s = field_ptr(cfg, k);
            s[k->size - 1] = '\0';
            if (!pass_valid(s)) {
                ESP_LOGW(TAG, "%s invalide (8 à 63 caractères sans accent) : valeur par défaut", k->key);
                copy_str(s, k->size, field_get_str(&def, k));
                fixes++;
            }
            break;
        }
        case KT_UINT:
        case KT_INT: {
            int32_t v = field_get_int(cfg, k);
            int32_t c = (v < k->min) ? k->min : (v > k->max) ? k->max : v;
            if (c != v) {
                ESP_LOGW(TAG, "%s = %ld hors plage (%ld à %ld) : ramené à %ld",
                         k->key, (long)v, (long)k->min, (long)k->max, (long)c);
                field_set_int(cfg, k, c);
                fixes++;
            }
            break;
        }
        case KT_BOOL:
            if (field_get_bool_raw(cfg, k) > 1) {
                bool d = field_get_bool_raw(&def, k) != 0;
                ESP_LOGW(TAG, "%s invalide : %s", k->key, d ? "oui" : "non");
                field_set_bool(cfg, k, d);
                fixes++;
            }
            break;
        case KT_RES:
            if (!res_supported(cfg->video_width, cfg->video_height)) {
                ESP_LOGW(TAG, "résolution %ux%u non prise en charge : %ux%u",
                         (unsigned)cfg->video_width, (unsigned)cfg->video_height,
                         (unsigned)def.video_width, (unsigned)def.video_height);
                cfg->video_width = def.video_width;
                cfg->video_height = def.video_height;
                fixes++;
            }
            break;
        case KT_MODE:
            if ((unsigned)cfg->night_mode > (unsigned)NID_NIGHT_MODE_NIGHT) {
                ESP_LOGW(TAG, "mode jour/nuit invalide : auto");
                cfg->night_mode = def.night_mode;
                fixes++;
            }
            break;
        case KT_FLOAT: {
            float v = field_get_float(cfg, k);
            float c = v;
            if (!isfinite(v)) {
                c = field_get_float(&def, k);
            } else if (v < (float)k->min) {
                c = (float)k->min;
            } else if (v > (float)k->max) {
                c = (float)k->max;
            }
            if (c != v || !isfinite(v)) {
                char sc[24];
                format_float(c, sc, sizeof(sc));
                ESP_LOGW(TAG, "%s hors plage (%ld à %ld) : ramené à %s", k->key, (long)k->min, (long)k->max, sc);
                field_set_float(cfg, k, c);
                fixes++;
            }
            break;
        }
        }
    }

    /* Cohérence : le seuil critique doit rester sous le seuil bas. */
    if (cfg->batt_critical_mv >= cfg->batt_low_mv) {
        const int32_t floor_mv = 2800;
        unsigned old_crit = cfg->batt_critical_mv;
        if (cfg->batt_low_mv >= floor_mv + 100) {
            cfg->batt_critical_mv = (uint16_t)(cfg->batt_low_mv - 100);
        } else {
            cfg->batt_critical_mv = (uint16_t)floor_mv;
            cfg->batt_low_mv = (uint16_t)(floor_mv + 100);
        }
        ESP_LOGW(TAG, "seuil_batterie_critique_mv (%u) doit être inférieur à seuil_batterie_basse_mv : "
                 "seuils ramenés à %u / %u mV", old_crit, (unsigned)cfg->batt_critical_mv, (unsigned)cfg->batt_low_mv);
        fixes++;
    }
    return fixes ? ESP_ERR_INVALID_ARG : ESP_OK;
}

const char *nid_config_night_mode_str(nid_night_mode_t mode)
{
    switch (mode) {
    case NID_NIGHT_MODE_DAY:
        return "jour";
    case NID_NIGHT_MODE_NIGHT:
        return "nuit";
    default:
        return "auto";
    }
}

void nid_config_effective_ssid(const nid_config_t *cfg, char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return;
    }
    if (cfg->wifi_ssid[0]) {
        copy_str(out, len, cfg->wifi_ssid);
    } else {
        snprintf(out, len, "NID-%.15s", cfg->system_id);
    }
}

size_t nid_config_sanitize_id(const char *in, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return 0;
    }
    out[0] = '\0';
    if (in == NULL) {
        return 0;
    }
    while (is_blank(*in)) {
        in++;
    }
    size_t end = strlen(in);
    while (end > 0 && is_blank(in[end - 1])) {
        end--;
    }
    size_t n = 0;
    size_t i = 0;
    while (i < end && n + 1 < out_len) {
        unsigned char c = (unsigned char)in[i];
        if (is_alpha((char)c) || is_digit((char)c) || c == '_' || c == '-') {
            out[n++] = (char)c;
            i++;
        } else if (c >= 0xC0) {
            /* Caractère UTF-8 multi-octet (lettre accentuée...) : un seul « _ ». */
            out[n++] = '_';
            i++;
            while (i < end && ((unsigned char)in[i] & 0xC0) == 0x80) {
                i++;
            }
        } else {
            out[n++] = '_';
            i++;
        }
    }
    out[n] = '\0';
    return n;
}

/* ------------------------------------------------------------------------- */
/* Lecture de config.ini                                                      */
/* ------------------------------------------------------------------------- */

typedef enum {
    VAL_OK = 0,
    VAL_UNTERMINATED,   /* guillemet fermant manquant */
    VAL_TOO_LONG,
} value_status_t;

/*
 * Extrait la valeur après « = » : texte entre guillemets (« "" » = un
 * guillemet), ou texte brut jusqu'à un commentaire « # » / « ; » placé en
 * début de valeur ou précédé d'un blanc. Les blancs de début/fin sont retirés
 * (hors guillemets).
 */
static value_status_t extract_value(const char *raw, char *out, size_t out_len, bool *trailing_text)
{
    const char *p = raw;
    size_t n = 0;
    *trailing_text = false;
    while (is_blank(*p)) {
        p++;
    }
    if (*p == '"') {
        p++;
        for (;;) {
            if (*p == '\0') {
                return VAL_UNTERMINATED;
            }
            if (*p == '"') {
                if (p[1] == '"') {
                    p++;
                } else {
                    p++;
                    break;
                }
            }
            if (n + 1 >= out_len) {
                return VAL_TOO_LONG;
            }
            out[n++] = *p++;
        }
        out[n] = '\0';
        while (is_blank(*p)) {
            p++;
        }
        *trailing_text = (*p != '\0' && *p != '#' && *p != ';');
        return VAL_OK;
    }
    for (size_t i = 0; p[i]; i++) {
        if ((p[i] == '#' || p[i] == ';') && (i == 0 || is_blank(p[i - 1]))) {
            break;
        }
        if (n + 1 >= out_len) {
            return VAL_TOO_LONG;
        }
        out[n++] = p[i];
    }
    while (n > 0 && is_blank(out[n - 1])) {
        n--;
    }
    out[n] = '\0';
    return VAL_OK;
}

typedef struct {
    const char *path;
    int         warnings;
} load_ctx_t;

static void load_warn(load_ctx_t *lc, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void load_warn(load_ctx_t *lc, int line, const char *fmt, ...)
{
    lc->warnings++;
    if (lc->warnings > LOAD_MAX_WARNINGS) {
        return;
    }
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "%s ligne %d : %s", lc->path, line, msg);
}

esp_err_t nid_config_load(nid_config_t *cfg, const char *path)
{
    if (cfg == NULL || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Écriture précédente interrompue par une coupure : la terminer ou l'annuler. */
    nid_config_recover_file(path);

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        if (errno == ENOENT) {
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGE(TAG, "ouverture de %s impossible (errno %d)", path, errno);
        return ESP_FAIL;
    }

    nid_config_t work = *cfg;
    load_ctx_t lc = { .path = path, .warnings = 0 };
    char line[LINE_MAX_LEN];
    int lineno = 0;
    int applied = 0;
    size_t total = 0;
    uint32_t seen = 0;
    bool too_big = false;

    while (fgets(line, sizeof(line), f) != NULL) {
        lineno++;
        size_t n = strlen(line);
        total += n;
        if (total > LOAD_MAX_BYTES) {
            too_big = true;
            break;
        }
        if (n == sizeof(line) - 1 && line[n - 1] != '\n') {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {
                total++;
            }
            load_warn(&lc, lineno, "ligne trop longue (plus de %d caractères) : ignorée", LINE_MAX_LEN - 2);
            continue;
        }
        char *s = line;
        if (lineno == 1 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
            s += 3;  /* BOM UTF-8 (Bloc-notes) */
        }
        s = trim(s);
        if (*s == '\0' || *s == '#' || *s == ';') {
            continue;
        }
        if (*s == '[') {
            if (strchr(s, ']') == NULL) {
                load_warn(&lc, lineno, "section mal formée « %.40s » : ignorée", s);
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (eq == NULL) {
            load_warn(&lc, lineno, "« clé = valeur » attendu : « %.40s » ignoré", s);
            continue;
        }
        *eq = '\0';
        char *key = trim(s);
        for (char *p = key; *p; p++) {
            *p = to_lower(*p);
        }
        char value[VALUE_MAX_LEN];
        bool trailing = false;
        value_status_t vs = extract_value(eq + 1, value, sizeof(value), &trailing);
        if (vs == VAL_UNTERMINATED) {
            load_warn(&lc, lineno, "guillemet fermant manquant pour « %.40s » : ignoré", key);
            continue;
        }
        if (vs == VAL_TOO_LONG) {
            load_warn(&lc, lineno, "valeur trop longue pour « %.40s » : ignorée", key);
            continue;
        }
        int idx = find_key(key);
        if (idx < 0) {
            load_warn(&lc, lineno, "clé inconnue « %.40s » : ignorée", key);
            continue;
        }
        if (trailing) {
            load_warn(&lc, lineno, "texte après le guillemet fermant de « %s » : ignoré", s_keys[idx].key);
        }
        if (seen & (1u << idx)) {
            load_warn(&lc, lineno, "clé « %s » répétée : la dernière valeur s'applique", s_keys[idx].key);
        }
        seen |= 1u << idx;
        if (apply_value(&work, &s_keys[idx], value, lineno, lc.warnings >= LOAD_MAX_WARNINGS) == ESP_OK) {
            applied++;
        } else {
            lc.warnings++;
        }
    }
    bool read_error = ferror(f) != 0;
    fclose(f);

    if (read_error) {
        ESP_LOGE(TAG, "erreur de lecture de %s : réglages inchangés", path);
        return ESP_FAIL;
    }
    if (total == 0) {
        ESP_LOGW(TAG, "%s est vide", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (too_big) {
        load_warn(&lc, lineno, "fichier trop grand : lecture arrêtée après %d Ko", LOAD_MAX_BYTES / 1024);
    }
    if (lc.warnings > LOAD_MAX_WARNINGS) {
        ESP_LOGW(TAG, "%s : %d avertissement(s) au total (les premiers seulement sont détaillés)", path, lc.warnings);
    }
    nid_config_validate(&work);
    *cfg = work;
    ESP_LOGI(TAG, "%s lu : %d réglage(s) appliqué(s), %d avertissement(s)", path, applied, lc.warnings);
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Production de config.ini et du JSON                                        */
/* ------------------------------------------------------------------------- */

#define EOL "\r\n"

/* Écrit chaque ligne de « text » précédée de « # ». */
static void out_comment(out_t *o, const char *text)
{
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        out_str(o, n ? "# " : "#");
        out_write(o, p, n);
        out_str(o, EOL);
        p += n;
        if (*p == '\n') {
            p++;
        }
    }
}

/* Valeur INI : texte entre guillemets si nécessaire (« # », « ; », guillemet, espaces). */
static void out_ini_value(out_t *o, const char *s, bool may_quote)
{
    size_t n = strlen(s);
    bool quote = may_quote && n > 0 &&
                 (strpbrk(s, "#;\"") != NULL || is_blank(s[0]) || is_blank(s[n - 1]));
    if (!quote) {
        out_str(o, s);
        return;
    }
    out_str(o, "\"");
    for (const char *p = s; *p; p++) {
        if (*p == '"') {
            out_str(o, "\"\"");
        } else {
            out_write(o, p, 1);
        }
    }
    out_str(o, "\"");
}

static void out_range_line(out_t *o, const key_def_t *k, const nid_config_t *def)
{
    char dv[NID_CONFIG_PASS_LEN];
    format_value(def, k, dv, sizeof(dv));
    switch (k->type) {
    case KT_UINT:
    case KT_INT:
    case KT_FLOAT:
        out_fmt(o, "# Plage : %ld à %ld%s%s. Défaut : %s. (CdCF %s)" EOL, (long)k->min, (long)k->max,
                k->unit[0] ? " " : "", k->unit, dv, k->ref);
        break;
    case KT_BOOL:
        out_fmt(o, "# Valeurs : oui ou non. Défaut : %s. (CdCF %s)" EOL, dv, k->ref);
        break;
    case KT_RES:
        out_fmt(o, "# Valeurs : 1920x1080, 1280x960 ou 1280x720. Défaut : %s. (CdCF %s)" EOL, dv, k->ref);
        break;
    case KT_MODE:
        out_fmt(o, "# Valeurs : auto, jour ou nuit. Défaut : %s. (CdCF %s)" EOL, dv, k->ref);
        break;
    default:
        out_fmt(o, "# Défaut : %s. (CdCF %s)" EOL, dv[0] ? dv : "vide", k->ref);
        break;
    }
}

static const char *section_title(const char *section)
{
    for (size_t i = 0; i < sizeof(s_sections) / sizeof(s_sections[0]); i++) {
        if (strcmp(s_sections[i].section, section) == 0) {
            return s_sections[i].title;
        }
    }
    return section;
}

static const char s_ini_header[] =
    "=============================================================================\n"
    " config.ini : réglages de la caméra de suivi de nid\n"
    " (moqueur gorge-blanche, Martinique ; utilisable pour d'autres espèces)\n"
    "=============================================================================\n"
    " Ce fichier se modifie avec un éditeur de texte (Bloc-notes, TextEdit...)\n"
    " ou depuis la page d'installation (smartphone). Il est relu à chaque\n"
    " démarrage et à chaque ouverture du mode installation (bouton).\n"
    "\n"
    " Règles :\n"
    "  - une ligne « clé = valeur » par réglage ; ne pas modifier les clés ;\n"
    "  - les lignes qui commencent par # ou ; sont des commentaires ;\n"
    "  - nombres décimaux avec un point ou une virgule (14.77 ou 14,77) ;\n"
    "  - choix : oui ou non (1/0 et true/false sont aussi acceptés) ;\n"
    "  - une valeur invalide ou hors plage est ignorée : la valeur par défaut\n"
    "    indiquée s'applique ;\n"
    "  - pour revenir aux réglages d'usine, supprimer ce fichier : il sera\n"
    "    recréé au prochain démarrage.\n"
    " CdCF = cahier des charges fonctionnel (exigences FP, FS et FC).\n"
    "=============================================================================";

size_t nid_config_to_ini(const nid_config_t *cfg, char *buf, size_t len)
{
    if (cfg == NULL || buf == NULL || len == 0) {
        return 0;
    }
    nid_config_t def;
    nid_config_set_defaults(&def);
    out_t o = { .buf = buf, .len = len, .pos = 0, .overflow = false };
    buf[0] = '\0';

    out_str(&o, "\xEF\xBB\xBF");  /* BOM : reconnu comme UTF-8 par le Bloc-notes */
    out_comment(&o, s_ini_header);

    const char *section = NULL;
    for (size_t i = 0; i < KEY_COUNT; i++) {
        const key_def_t *k = &s_keys[i];
        if (section == NULL || strcmp(section, k->section) != 0) {
            section = k->section;
            out_str(&o, EOL "# -----------------------------------------------------------------------------" EOL);
            out_fmt(&o, "# %s" EOL, section_title(section));
            out_str(&o, "# -----------------------------------------------------------------------------" EOL);
            out_fmt(&o, "[%s]" EOL, section);
        }
        out_str(&o, EOL);
        out_comment(&o, k->doc);
        out_range_line(&o, k, &def);
        char v[NID_CONFIG_PASS_LEN];
        format_value(cfg, k, v, sizeof(v));
        out_str(&o, k->key);
        out_str(&o, v[0] ? " = " : " =");
        out_ini_value(&o, v, k->type == KT_SSID || k->type == KT_PASS);
        out_str(&o, EOL);
    }
    out_str(&o, EOL "# Fin du fichier." EOL);
    if (o.overflow) {
        buf[0] = '\0';
        return 0;
    }
    return o.pos;
}

static void out_json_str(out_t *o, const char *s)
{
    out_str(o, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':
            out_str(o, "\\\"");
            break;
        case '\\':
            out_str(o, "\\\\");
            break;
        case '\n':
            out_str(o, "\\n");
            break;
        case '\r':
            out_str(o, "\\r");
            break;
        case '\t':
            out_str(o, "\\t");
            break;
        case '\b':
            out_str(o, "\\b");
            break;
        case '\f':
            out_str(o, "\\f");
            break;
        case '<':
        case '>':
        case '&':
            /* Sûr même inséré dans une page HTML. */
            out_fmt(o, "\\u%04x", (unsigned)*p);
            break;
        default:
            if (*p < 0x20 || *p == 0x7F) {
                out_fmt(o, "\\u%04x", (unsigned)*p);
            } else {
                out_write(o, (const char *)p, 1);
            }
            break;
        }
    }
    out_str(o, "\"");
}

size_t nid_config_to_json(const nid_config_t *cfg, char *buf, size_t len)
{
    if (cfg == NULL || buf == NULL || len == 0) {
        return 0;
    }
    out_t o = { .buf = buf, .len = len, .pos = 0, .overflow = false };
    buf[0] = '\0';
    out_str(&o, "{");
    for (size_t i = 0; i < KEY_COUNT; i++) {
        const key_def_t *k = &s_keys[i];
        char v[NID_CONFIG_PASS_LEN];
        if (i > 0) {
            out_str(&o, ",");
        }
        out_json_str(&o, k->key);
        out_str(&o, ":");
        switch (k->type) {
        case KT_UINT:
        case KT_INT:
            out_fmt(&o, "%ld", (long)field_get_int(cfg, k));
            break;
        case KT_BOOL:
            out_str(&o, field_get_bool_raw(cfg, k) ? "true" : "false");
            break;
        case KT_FLOAT:
            format_float(field_get_float(cfg, k), v, sizeof(v));
            out_str(&o, v);
            break;
        default:
            format_value(cfg, k, v, sizeof(v));
            out_json_str(&o, v);
            break;
        }
    }
    out_str(&o, "}");
    if (o.overflow) {
        buf[0] = '\0';
        return 0;
    }
    return o.pos;
}

/* ------------------------------------------------------------------------- */
/* Écriture sûre sur FAT, sans renommage                                      */
/* ------------------------------------------------------------------------- */
/*
 * Pourquoi pas « écrire .tmp puis renommer » : sur FAT, f_rename crée la
 * nouvelle entrée de répertoire puis efface l'ancienne. Si les deux entrées
 * sont dans des secteurs différents, une coupure entre les deux écritures
 * laisse deux noms sur la même chaîne de clusters ; supprimer ou réécrire
 * l'un des deux libère alors les clusters de l'autre (corruption différée).
 *
 * Protocole retenu (aucun renommage) :
 *   1. path.tmp = données + ligne de contrôle « \n#NIDTMP <longueur> <crc32>\n »,
 *      fflush + fsync + fermeture ;
 *   2. path réécrit avec les données seules, fflush + fsync + fermeture ;
 *   3. suppression de path.tmp.
 * Coupure pendant 1 : path intact, path.tmp sans ligne de contrôle valide →
 * supprimé à la reprise. Coupure pendant 2 : path.tmp complet et vérifié par
 * CRC → path réécrit à la reprise. Coupure pendant 3 : path complet ; la
 * reprise constate l'égalité et supprime path.tmp.
 */

#define TMP_SUFFIX     ".tmp"
#define TRAILER_MAGIC  "\n#NIDTMP "
#define TRAILER_LEN    29   /* "\n#NIDTMP " (9) + 10 chiffres + " " + 8 hexa + "\n" */

static uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    static const uint32_t table[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ table[crc & 0x0F];
        crc = (crc >> 4) ^ table[crc & 0x0F];
    }
    return ~crc;
}

static bool tmp_path_of(const char *path, char *out, size_t len)
{
    int n = snprintf(out, len, "%s" TMP_SUFFIX, path);
    return n > 0 && (size_t)n < len;
}

/* Écrit data (+ trailer) dans path, fflush + fsync + fermeture. */
static esp_err_t write_whole(const char *path, const void *data, size_t len, const char *trailer)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "création de %s impossible (errno %d)", path, errno);
        return ESP_FAIL;
    }
    bool ok = (len == 0) || fwrite(data, 1, len, f) == len;
    if (ok && trailer != NULL) {
        ok = fwrite(trailer, 1, TRAILER_LEN, f) == TRAILER_LEN;
    }
    if (ok) {
        ok = fflush(f) == 0;
    }
    if (ok) {
        ok = fsync(fileno(f)) == 0;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        ESP_LOGE(TAG, "écriture de %s impossible (errno %d)", path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Lit et vérifie la ligne de contrôle d'un fichier temporaire (sans le CRC). */
static bool read_trailer(const char *tmp, uint32_t *data_len, uint32_t *crc)
{
    struct stat st;
    if (stat(tmp, &st) != 0 || st.st_size < TRAILER_LEN) {
        return false;
    }
    /* off_t peut être signé sur 32 bits : comparaisons en 64 bits non signés. */
    uint64_t size = (uint64_t)st.st_size;
    FILE *f = fopen(tmp, "rb");
    if (f == NULL) {
        return false;
    }
    char t[TRAILER_LEN];
    bool ok = fseek(f, (long)(size - TRAILER_LEN), SEEK_SET) == 0 &&
              fread(t, 1, TRAILER_LEN, f) == TRAILER_LEN;
    fclose(f);
    if (!ok || memcmp(t, TRAILER_MAGIC, 9) != 0 || t[19] != ' ' || t[28] != '\n') {
        return false;
    }
    uint32_t n = 0;
    for (int i = 9; i < 19; i++) {
        if (!is_digit(t[i])) {
            return false;
        }
        n = n * 10 + (uint32_t)(t[i] - '0');
    }
    uint32_t c = 0;
    for (int i = 20; i < 28; i++) {
        char h = t[i];
        uint32_t d;
        if (is_digit(h)) {
            d = (uint32_t)(h - '0');
        } else if (h >= 'A' && h <= 'F') {
            d = (uint32_t)(h - 'A' + 10);
        } else {
            return false;
        }
        c = (c << 4) | d;
    }
    if ((uint64_t)n + TRAILER_LEN != size) {
        return false;
    }
    *data_len = n;
    *crc = c;
    return true;
}

/* CRC-32 des len premiers octets d'un fichier (false si plus court ou illisible). */
static bool file_crc(const char *path, uint32_t len, uint32_t *crc_out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    uint8_t buf[512];
    uint32_t crc = 0;
    uint32_t left = len;
    bool ok = true;
    while (left > 0) {
        size_t chunk = left < sizeof(buf) ? left : sizeof(buf);
        if (fread(buf, 1, chunk, f) != chunk) {
            ok = false;
            break;
        }
        crc = crc32_update(crc, buf, chunk);
        left -= (uint32_t)chunk;
    }
    fclose(f);
    *crc_out = crc;
    return ok;
}

/* Recopie les len premiers octets de src dans dst (fflush + fsync). */
static esp_err_t copy_prefix(const char *src, const char *dst, uint32_t len)
{
    FILE *in = fopen(src, "rb");
    if (in == NULL) {
        return ESP_FAIL;
    }
    FILE *out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        return ESP_FAIL;
    }
    uint8_t buf[512];
    uint32_t left = len;
    bool ok = true;
    while (ok && left > 0) {
        size_t chunk = left < sizeof(buf) ? left : sizeof(buf);
        ok = fread(buf, 1, chunk, in) == chunk && fwrite(buf, 1, chunk, out) == chunk;
        left -= (uint32_t)chunk;
    }
    fclose(in);
    if (ok) {
        ok = fflush(out) == 0 && fsync(fileno(out)) == 0;
    }
    if (fclose(out) != 0) {
        ok = false;
    }
    return ok ? ESP_OK : ESP_FAIL;
}

bool nid_config_tmp_is_complete(const char *tmp_path)
{
    uint32_t len;
    uint32_t crc;
    return tmp_path != NULL && read_trailer(tmp_path, &len, &crc);
}

esp_err_t nid_config_recover_file(const char *path)
{
    char tmp[FILE_PATH_LEN];
    if (path == NULL || !tmp_path_of(path, tmp, sizeof(tmp))) {
        return ESP_ERR_INVALID_ARG;
    }
    struct stat st;
    if (stat(tmp, &st) != 0) {
        return ESP_OK;  /* aucune écriture en cours */
    }
    uint32_t len = 0;
    uint32_t crc = 0;
    uint32_t tmp_crc = 0;
    if (!read_trailer(tmp, &len, &crc) || !file_crc(tmp, len, &tmp_crc) || tmp_crc != crc) {
        /* Coupure pendant l'écriture du temporaire : la version précédente fait foi. */
        ESP_LOGW(TAG, "%s : écriture interrompue avant la fin, version précédente conservée", path);
        if (remove(tmp) != 0) {
            ESP_LOGE(TAG, "suppression de %s impossible (errno %d)", tmp, errno);
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    struct stat sp;
    uint32_t cur_crc = 0;
    bool up_to_date = stat(path, &sp) == 0 && sp.st_size >= 0 && (uint64_t)sp.st_size == len &&
                      file_crc(path, len, &cur_crc) && cur_crc == crc;
    if (!up_to_date) {
        if (copy_prefix(tmp, path, len) != ESP_OK) {
            ESP_LOGE(TAG, "%s : reprise de l'écriture impossible (errno %d), %s conservé", path, errno, tmp);
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "%s : écriture interrompue terminée depuis %s", path, tmp);
    }
    if (remove(tmp) != 0) {
        ESP_LOGW(TAG, "suppression de %s impossible (errno %d)", tmp, errno);
    }
    return ESP_OK;
}

esp_err_t nid_config_write_file_atomic(const char *path, const void *data, size_t len)
{
    char tmp[FILE_PATH_LEN];
    if (path == NULL || (data == NULL && len > 0) || len > 0x7FFFFFFFu) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tmp_path_of(path, tmp, sizeof(tmp))) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* Une écriture précédente interrompue doit d'abord être terminée. */
    nid_config_recover_file(path);

    char trailer[TRAILER_LEN + 1];
    uint32_t crc = crc32_update(0, data, len);
    int n = snprintf(trailer, sizeof(trailer), TRAILER_MAGIC "%010lu %08lX\n", (unsigned long)len, (unsigned long)crc);
    if (n != TRAILER_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = write_whole(tmp, data, len, trailer);
    if (err != ESP_OK) {
        remove(tmp);
        return err;
    }
    err = write_whole(path, data, len, NULL);
    if (err != ESP_OK) {
        /* path peut être incomplet : path.tmp, complet, sera recopié à la reprise. */
        ESP_LOGE(TAG, "%s non mis à jour : la nouvelle version reste dans %s", path, tmp);
        return err;
    }
    if (remove(tmp) != 0) {
        ESP_LOGW(TAG, "suppression de %s impossible (errno %d)", tmp, errno);
    }
    return ESP_OK;
}

esp_err_t nid_config_save(const nid_config_t *cfg, const char *path)
{
    if (cfg == NULL || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char *buf = malloc(NID_CONFIG_INI_MAX_LEN);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t n = nid_config_to_ini(cfg, buf, NID_CONFIG_INI_MAX_LEN);
    esp_err_t err = (n > 0) ? nid_config_write_file_atomic(path, buf, n) : ESP_ERR_INVALID_SIZE;
    free(buf);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s enregistré (%u octets)", path, (unsigned)n);
    } else {
        ESP_LOGE(TAG, "enregistrement de %s impossible (%s)", path, esp_err_to_name(err));
    }
    return err;
}
