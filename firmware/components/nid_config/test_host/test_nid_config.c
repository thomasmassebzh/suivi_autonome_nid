/*
 * Tests sur PC de nid_config : lecture INI tolérante, contrôle des valeurs,
 * validation, aller-retour INI et JSON, écriture atomique et reprise après
 * coupure (états intermédiaires reconstitués à la main).
 *
 * Usage : test_nid_config <dossier de travail>   (voir Makefile : make test)
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nid_config.h"
#include "esp_log.h"
#include "test_util.h"

static char s_dir[512];

/* ------------------------------------------------------------------------- */
/* Outils fichiers                                                            */
/* ------------------------------------------------------------------------- */

static void path_in_dir(char *out, size_t len, const char *name)
{
    snprintf(out, len, "%s/%s", s_dir, name);
}

static void write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "création de %s impossible\n", path);
        exit(2);
    }
    fwrite(data, 1, len, f);
    fclose(f);
}

static void write_text(const char *path, const char *text)
{
    write_file(path, text, strlen(text));
}

/* Lit un fichier entier (alloué, terminé par '\0'), NULL s'il n'existe pas. */
static char *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (len_out) {
        *len_out = got;
    }
    return buf;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Comparaison champ par champ (pas de memcmp : octets de remplissage). */
static bool cfg_equal(const nid_config_t *a, const nid_config_t *b)
{
    return strcmp(a->system_id, b->system_id) == 0 && strcmp(a->nest_id, b->nest_id) == 0 &&
           strcmp(a->timezone, b->timezone) == 0 && a->seq_duration_s == b->seq_duration_s &&
           a->trigger_delay_s == b->trigger_delay_s && a->video_width == b->video_width &&
           a->video_height == b->video_height && a->video_fps == b->video_fps &&
           a->video_bitrate_kbps == b->video_bitrate_kbps && a->audio_enabled == b->audio_enabled &&
           a->photo_interval_min == b->photo_interval_min && a->jpeg_quality == b->jpeg_quality &&
           a->pir_sensitivity == b->pir_sensitivity && a->pir_warmup_s == b->pir_warmup_s &&
           a->night_mode == b->night_mode && a->latitude == b->latitude && a->longitude == b->longitude &&
           a->dusk_advance_min == b->dusk_advance_min && a->dawn_delay_min == b->dawn_delay_min &&
           a->ir_power_pct == b->ir_power_pct && a->night_monochrome == b->night_monochrome &&
           a->batt_low_mv == b->batt_low_mv && a->batt_critical_mv == b->batt_critical_mv &&
           strcmp(a->wifi_ssid, b->wifi_ssid) == 0 && strcmp(a->wifi_password, b->wifi_password) == 0 &&
           a->install_timeout_min == b->install_timeout_min;
}

/* Jeu de réglages tous différents des valeurs par défaut. */
static void make_custom(nid_config_t *c)
{
    nid_config_set_defaults(c);
    CHECK_INT(nid_config_set(c, "id_systeme", "CAM-07_b"), ESP_OK);
    CHECK_INT(nid_config_set(c, "id_nid", "NID_Caravelle2"), ESP_OK);
    CHECK_INT(nid_config_set(c, "fuseau_horaire", "CET-1CEST,M3.5.0,M10.5.0/3"), ESP_OK);
    CHECK_INT(nid_config_set(c, "duree_sequence_s", "45"), ESP_OK);
    CHECK_INT(nid_config_set(c, "delai_entre_declenchements_s", "0"), ESP_OK);
    CHECK_INT(nid_config_set(c, "resolution", "1280x720"), ESP_OK);
    CHECK_INT(nid_config_set(c, "images_par_seconde", "25"), ESP_OK);
    CHECK_INT(nid_config_set(c, "debit_kbps", "4000"), ESP_OK);
    CHECK_INT(nid_config_set(c, "son", "non"), ESP_OK);
    CHECK_INT(nid_config_set(c, "intervalle_suivi_min", "0"), ESP_OK);
    CHECK_INT(nid_config_set(c, "qualite_jpeg", "95"), ESP_OK);
    CHECK_INT(nid_config_set(c, "sensibilite", "5"), ESP_OK);
    CHECK_INT(nid_config_set(c, "chauffe_pir_s", "0"), ESP_OK);
    CHECK_INT(nid_config_set(c, "mode", "nuit"), ESP_OK);
    CHECK_INT(nid_config_set(c, "latitude", "-14,5"), ESP_OK);
    CHECK_INT(nid_config_set(c, "longitude", "179.875"), ESP_OK);
    CHECK_INT(nid_config_set(c, "avance_crepuscule_min", "-180"), ESP_OK);
    CHECK_INT(nid_config_set(c, "retard_aube_min", "240"), ESP_OK);
    CHECK_INT(nid_config_set(c, "puissance_ir", "0"), ESP_OK);
    CHECK_INT(nid_config_set(c, "nuit_monochrome", "non"), ESP_OK);
    CHECK_INT(nid_config_set(c, "seuil_batterie_basse_mv", "3600"), ESP_OK);
    CHECK_INT(nid_config_set(c, "seuil_batterie_critique_mv", "3300"), ESP_OK);
    CHECK_INT(nid_config_set(c, "wifi_ssid", "Nid Forêt #2 ; \"test\""), ESP_OK);
    CHECK_INT(nid_config_set(c, "wifi_mot_de_passe", " a#b;c\"d e "), ESP_OK);
    CHECK_INT(nid_config_set(c, "duree_max_min", "60"), ESP_OK);
}

/* ------------------------------------------------------------------------- */
/* Mini-lecteur JSON (objet plat) pour vérifier la relecture par nid_config_set */
/* ------------------------------------------------------------------------- */

static const char *json_string(const char *p, char *out, size_t len)
{
    size_t n = 0;
    if (*p++ != '"') {
        return NULL;
    }
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            char e = *p++;
            switch (e) {
            case '"':
            case '\\':
            case '/':
                c = e;
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'u': {
                char hex[5] = { p[0], p[1], p[2], p[3], 0 };
                long v = strtol(hex, NULL, 16);
                if (v > 0x7F) {
                    return NULL;
                }
                c = (char)v;
                p += 4;
                break;
            }
            default:
                return NULL;
            }
        } else if ((unsigned char)c < 0x20) {
            return NULL;  /* caractère de contrôle non échappé : JSON invalide */
        }
        if (n + 1 >= len) {
            return NULL;
        }
        out[n++] = c;
    }
    if (*p != '"') {
        return NULL;
    }
    out[n] = '\0';
    return p + 1;
}

/* Applique chaque paire de l'objet JSON avec nid_config_set. @return nombre de paires, -1 si JSON invalide. */
static int json_apply(nid_config_t *cfg, const char *json)
{
    const char *p = json;
    int count = 0;
    if (*p++ != '{') {
        return -1;
    }
    while (*p && *p != '}') {
        char key[64];
        char value[160];
        p = json_string(p, key, sizeof(key));
        if (p == NULL || *p++ != ':') {
            return -1;
        }
        if (*p == '"') {
            p = json_string(p, value, sizeof(value));
            if (p == NULL) {
                return -1;
            }
        } else {
            size_t n = 0;
            while (*p && *p != ',' && *p != '}') {
                if (n + 1 >= sizeof(value)) {
                    return -1;
                }
                value[n++] = *p++;
            }
            value[n] = '\0';
        }
        if (nid_config_set(cfg, key, value) != ESP_OK) {
            return -1;
        }
        count++;
        if (*p == ',') {
            p++;
        }
    }
    return (*p == '}' && p[1] == '\0') ? count : -1;
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                      */
/* ------------------------------------------------------------------------- */

static void test_defaults(void)
{
    nid_config_t c;
    nid_config_set_defaults(&c);
    CHECK_STR(c.system_id, "CAM01");
    CHECK_STR(c.nest_id, "NID01");
    CHECK_STR(c.timezone, "AST4");
    CHECK_INT(c.seq_duration_s, 30);
    CHECK_INT(c.trigger_delay_s, 60);
    CHECK_INT(c.video_width, 1920);
    CHECK_INT(c.video_height, 1080);
    CHECK_INT(c.video_fps, 15);
    CHECK_INT(c.video_bitrate_kbps, 2500);
    CHECK(c.audio_enabled);
    CHECK_INT(c.photo_interval_min, 15);
    CHECK_INT(c.jpeg_quality, 80);
    CHECK_INT(c.pir_sensitivity, 3);
    CHECK_INT(c.pir_warmup_s, 60);
    CHECK_INT(c.night_mode, NID_NIGHT_MODE_AUTO);
    CHECK(c.latitude == 14.77f);
    CHECK(c.longitude == -60.89f);
    CHECK_INT(c.dusk_advance_min, 30);
    CHECK_INT(c.dawn_delay_min, 30);
    CHECK_INT(c.ir_power_pct, 100);
    CHECK(c.night_monochrome);
    CHECK_INT(c.batt_low_mv, 3450);
    CHECK_INT(c.batt_critical_mv, 3350);
    CHECK_STR(c.wifi_ssid, "");
    CHECK_STR(c.wifi_password, "moqueur-nid");
    CHECK_INT(c.install_timeout_min, 15);
    int w = g_log_warnings;
    CHECK_INT(nid_config_validate(&c), ESP_OK);
    CHECK_INT(g_log_warnings, w);
}

static void test_set_ranges(void)
{
    static const struct {
        const char *key;
        const char *min_ok;
        const char *max_ok;
        const char *below;
        const char *above;
    } t[] = {
        { "duree_sequence_s", "1", "300", "0", "301" },
        { "delai_entre_declenchements_s", "0", "3600", "-1", "3601" },
        { "images_par_seconde", "10", "30", "9", "31" },
        { "debit_kbps", "500", "10000", "499", "10001" },
        { "intervalle_suivi_min", "0", "1440", "-1", "1441" },
        { "qualite_jpeg", "10", "100", "9", "101" },
        { "sensibilite", "1", "5", "0", "6" },
        { "chauffe_pir_s", "0", "120", "-1", "121" },
        { "avance_crepuscule_min", "-180", "240", "-181", "241" },
        { "retard_aube_min", "-180", "240", "-181", "241" },
        { "puissance_ir", "0", "100", "-1", "101" },
        { "seuil_batterie_basse_mv", "2800", "4500", "2799", "4501" },
        { "seuil_batterie_critique_mv", "2800", "4500", "2799", "4501" },
        { "duree_max_min", "1", "60", "0", "61" },
        { "latitude", "-90", "90", "-90.001", "90,5" },
        { "longitude", "-180", "180", "-180.5", "180.01" },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        nid_config_t c;
        nid_config_t ref;
        nid_config_set_defaults(&c);
        CHECK_INT(nid_config_set(&c, t[i].key, t[i].min_ok), ESP_OK);
        CHECK_INT(nid_config_set(&c, t[i].key, t[i].max_ok), ESP_OK);
        ref = c;
        CHECK_INT(nid_config_set(&c, t[i].key, t[i].below), ESP_ERR_INVALID_ARG);
        CHECK_INT(nid_config_set(&c, t[i].key, t[i].above), ESP_ERR_INVALID_ARG);
        CHECK_INT(nid_config_set(&c, t[i].key, ""), ESP_ERR_INVALID_ARG);
        CHECK_INT(nid_config_set(&c, t[i].key, "abc"), ESP_ERR_INVALID_ARG);
        CHECK(cfg_equal(&c, &ref));
    }

    nid_config_t c;
    nid_config_set_defaults(&c);
    CHECK_INT(nid_config_set(&c, "duree_sequence_s", "+45"), ESP_OK);
    CHECK_INT(c.seq_duration_s, 45);
    CHECK_INT(nid_config_set(&c, " Duree_Sequence_S ", "  46  "), ESP_OK);
    CHECK_INT(c.seq_duration_s, 46);
    CHECK_INT(nid_config_set(&c, "duree_sequence_s", "30s"), ESP_ERR_INVALID_ARG);
    CHECK_INT(nid_config_set(&c, "duree_sequence_s", "0x10"), ESP_ERR_INVALID_ARG);
    CHECK_INT(nid_config_set(&c, "duree_sequence_s", "3.5"), ESP_ERR_INVALID_ARG);
    CHECK_INT(nid_config_set(&c, "duree_sequence_s", "99999999999999"), ESP_ERR_INVALID_ARG);
    CHECK_INT(nid_config_set(&c, "duree_sequence_s", "-"), ESP_ERR_INVALID_ARG);
    CHECK_INT(c.seq_duration_s, 46);
    CHECK_INT(nid_config_set(&c, "avance_crepuscule_min", "-45"), ESP_OK);
    CHECK_INT(c.dusk_advance_min, -45);
}

static void test_set_unknown_key(void)
{
    nid_config_t c;
    nid_config_t ref;
    nid_config_set_defaults(&c);
    ref = c;
    int w = g_log_warnings;
    CHECK_INT(nid_config_set(&c, "duree", "30"), ESP_ERR_NOT_FOUND);
    CHECK_INT(nid_config_set(&c, "", "30"), ESP_ERR_NOT_FOUND);
    CHECK_INT(nid_config_set(&c, "une_cle_beaucoup_trop_longue_pour_exister_vraiment", "1"), ESP_ERR_NOT_FOUND);
    CHECK(g_log_warnings >= w + 3);
    CHECK_INT(nid_config_set(NULL, "son", "oui"), ESP_ERR_INVALID_ARG);
    CHECK_INT(nid_config_set(&c, NULL, "oui"), ESP_ERR_INVALID_ARG);
    CHECK_INT(nid_config_set(&c, "son", NULL), ESP_ERR_INVALID_ARG);
    CHECK(cfg_equal(&c, &ref));
}

static void test_booleans(void)
{
    static const char *const yes[] = { "oui", "OUI", "Oui", "1", "true", "TRUE", "yes", "on", "vrai" };
    static const char *const no[] = { "non", "NON", "0", "false", "False", "no", "off", "faux" };
    nid_config_t c;
    nid_config_set_defaults(&c);
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
        c.audio_enabled = true;
        CHECK_INT(nid_config_set(&c, "son", no[i]), ESP_OK);
        CHECK(!c.audio_enabled);
    }
    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
        c.night_monochrome = false;
        CHECK_INT(nid_config_set(&c, "nuit_monochrome", yes[i]), ESP_OK);
        CHECK(c.night_monochrome);
    }
    CHECK_INT(nid_config_set(&c, "son", "peut-être"), ESP_ERR_INVALID_ARG);
    CHECK_INT(nid_config_set(&c, "son", "2"), ESP_ERR_INVALID_ARG);
    CHECK_INT(nid_config_set(&c, "son", "ouii"), ESP_ERR_INVALID_ARG);
}

static void test_mode_resolution(void)
{
    nid_config_t c;
    nid_config_set_defaults(&c);
    CHECK_INT(nid_config_set(&c, "mode", "NUIT"), ESP_OK);
    CHECK_INT(c.night_mode, NID_NIGHT_MODE_NIGHT);
    CHECK_INT(nid_config_set(&c, "mode", "jour"), ESP_OK);
    CHECK_INT(c.night_mode, NID_NIGHT_MODE_DAY);
    CHECK_INT(nid_config_set(&c, "mode", "Auto"), ESP_OK);
    CHECK_INT(c.night_mode, NID_NIGHT_MODE_AUTO);
    CHECK_INT(nid_config_set(&c, "mode", "soir"), ESP_ERR_INVALID_ARG);
    CHECK_STR(nid_config_night_mode_str(NID_NIGHT_MODE_NIGHT), "nuit");
    CHECK_STR(nid_config_night_mode_str(NID_NIGHT_MODE_DAY), "jour");
    CHECK_STR(nid_config_night_mode_str((nid_night_mode_t)9), "auto");

    static const struct {
        const char *v;
        int w;
        int h;
    } ok[] = {
        { "1920x1080", 1920, 1080 }, { "1280X960", 1280, 960 }, { " 1280 x 720 ", 1280, 720 },
        { "1920\xC3\x97" "1080", 1920, 1080 }, { "720p", 1280, 720 }, { "960P", 1280, 960 },
        { "1080p", 1920, 1080 },
    };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        c.video_width = 1;
        c.video_height = 1;
        CHECK_INT(nid_config_set(&c, "resolution", ok[i].v), ESP_OK);
        CHECK_INT(c.video_width, ok[i].w);
        CHECK_INT(c.video_height, ok[i].h);
    }
    static const char *const bad[] = { "640x480", "1920x", "x1080", "1920x1080x3", "1920*1080", "", "4k" };
    nid_config_set_defaults(&c);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK_INT(nid_config_set(&c, "resolution", bad[i]), ESP_ERR_INVALID_ARG);
        CHECK_INT(c.video_width, 1920);
        CHECK_INT(c.video_height, 1080);
    }
}

static void test_floats(void)
{
    nid_config_t c;
    nid_config_set_defaults(&c);
    CHECK_INT(nid_config_set(&c, "latitude", "14,5"), ESP_OK);
    CHECK(c.latitude == 14.5f);
    CHECK_INT(nid_config_set(&c, "latitude", "-60.89"), ESP_OK);
    CHECK(c.latitude == -60.89f);
    CHECK_INT(nid_config_set(&c, "latitude", ".5"), ESP_OK);
    CHECK(c.latitude == 0.5f);
    CHECK_INT(nid_config_set(&c, "latitude", "5."), ESP_OK);
    CHECK(c.latitude == 5.0f);
    CHECK_INT(nid_config_set(&c, "latitude", "+12"), ESP_OK);
    CHECK(c.latitude == 12.0f);
    static const char *const bad[] = { "1e3", "nan", "inf", "-inf", "14.7.7", "14,7.7", "", "-", ".", "1 2", "0x1p3" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK_INT(nid_config_set(&c, "longitude", bad[i]), ESP_ERR_INVALID_ARG);
    }
    CHECK(c.longitude == -60.89f);
}

static void test_ids_tz_wifi(void)
{
    nid_config_t c;
    nid_config_set_defaults(&c);
    CHECK_INT(nid_config_set(&c, "id_systeme", "CAM-01_a"), ESP_OK);
    CHECK_STR(c.system_id, "CAM-01_a");
    CHECK_INT(nid_config_set(&c, "id_nid", "ABCDEFGHIJKLMNO"), ESP_OK);  /* 15 caractères */
    CHECK_STR(c.nest_id, "ABCDEFGHIJKLMNO");
    static const char *const bad_id[] = { "CAM 01", "", "ABCDEFGHIJKLMNOP", "Forêt", "a/b", "a.b", "a;b" };
    for (size_t i = 0; i < sizeof(bad_id) / sizeof(bad_id[0]); i++) {
        CHECK_INT(nid_config_set(&c, "id_nid", bad_id[i]), ESP_ERR_INVALID_ARG);
    }
    CHECK_STR(c.nest_id, "ABCDEFGHIJKLMNO");

    static const char *const good_tz[] = { "AST4", "UTC0", "CET-1CEST,M3.5.0,M10.5.0/3", "<-04>4", "EST5EDT", "GMT+0" };
    for (size_t i = 0; i < sizeof(good_tz) / sizeof(good_tz[0]); i++) {
        CHECK_INT(nid_config_set(&c, "fuseau_horaire", good_tz[i]), ESP_OK);
        CHECK_STR(c.timezone, good_tz[i]);
    }
    static const char *const bad_tz[] = {
        "UTC", "AS4", "AST", "AST4;x", "AST4 #", "", "<-0>4",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUV4",
    };
    for (size_t i = 0; i < sizeof(bad_tz) / sizeof(bad_tz[0]); i++) {
        CHECK_INT(nid_config_set(&c, "fuseau_horaire", bad_tz[i]), ESP_ERR_INVALID_ARG);
    }
    CHECK_STR(c.timezone, "GMT+0");

    CHECK_INT(nid_config_set(&c, "wifi_ssid", ""), ESP_OK);
    CHECK_STR(c.wifi_ssid, "");
    CHECK_INT(nid_config_set(&c, "wifi_ssid", "12345678901234567890123456789012"), ESP_OK);
    CHECK_INT(nid_config_set(&c, "wifi_ssid", "123456789012345678901234567890123"), ESP_ERR_INVALID_ARG);
    CHECK_INT(nid_config_set(&c, "wifi_ssid", "a\tb"), ESP_ERR_INVALID_ARG);
    CHECK_INT(nid_config_set(&c, "wifi_ssid", "abc\xC3"), ESP_ERR_INVALID_ARG);  /* UTF-8 tronqué */
    CHECK_INT(nid_config_set(&c, "wifi_ssid", "Nid Forêt"), ESP_OK);
    CHECK_STR(c.wifi_ssid, "Nid Forêt");

    CHECK_INT(nid_config_set(&c, "wifi_mot_de_passe", "1234567"), ESP_ERR_INVALID_ARG);
    CHECK_INT(nid_config_set(&c, "wifi_mot_de_passe", "12345678"), ESP_OK);
    CHECK_INT(nid_config_set(&c, "wifi_mot_de_passe",
                             "123456789012345678901234567890123456789012345678901234567890123"), ESP_OK);
    CHECK_INT(nid_config_set(&c, "wifi_mot_de_passe",
                             "1234567890123456789012345678901234567890123456789012345678901234"), ESP_ERR_INVALID_ARG);
    CHECK_INT(nid_config_set(&c, "wifi_mot_de_passe", "motdepasseé"), ESP_ERR_INVALID_ARG);
    CHECK_INT(nid_config_set(&c, "wifi_mot_de_passe", " avec espaces "), ESP_OK);
    CHECK_STR(c.wifi_password, " avec espaces ");

    char ssid[NID_CONFIG_SSID_LEN];
    c.wifi_ssid[0] = '\0';
    nid_config_effective_ssid(&c, ssid, sizeof(ssid));
    CHECK_STR(ssid, "NID-CAM-01_a");
    CHECK_INT(nid_config_set(&c, "wifi_ssid", "MonNid"), ESP_OK);
    nid_config_effective_ssid(&c, ssid, sizeof(ssid));
    CHECK_STR(ssid, "MonNid");
}

static void test_validate(void)
{
    nid_config_t c;
    nid_config_set_defaults(&c);
    c.video_fps = 200;
    c.seq_duration_s = 0;
    c.video_width = 640;
    c.video_height = 480;
    c.night_mode = (nid_night_mode_t)7;
    c.latitude = NAN;
    c.longitude = 500.0f;
    c.dusk_advance_min = -500;
    memset(&c.audio_enabled, 5, 1);  /* booléen restauré corrompu */
    strcpy(c.system_id, " CAM 01 ");
    c.nest_id[0] = '\0';
    strcpy(c.wifi_password, "court");
    c.timezone[0] = '\0';
    strcpy(c.wifi_ssid, "a\x01" "b");
    c.batt_low_mv = 3000;
    c.batt_critical_mv = 3500;
    CHECK_INT(nid_config_validate(&c), ESP_ERR_INVALID_ARG);
    CHECK_INT(c.video_fps, 30);
    CHECK_INT(c.seq_duration_s, 1);
    CHECK_INT(c.video_width, 1920);
    CHECK_INT(c.video_height, 1080);
    CHECK_INT(c.night_mode, NID_NIGHT_MODE_AUTO);
    CHECK(c.latitude == 14.77f);
    CHECK(c.longitude == 180.0f);
    CHECK_INT(c.dusk_advance_min, -180);
    CHECK(c.audio_enabled);
    CHECK_STR(c.system_id, "CAM_01");
    CHECK_STR(c.nest_id, "NID01");
    CHECK_STR(c.wifi_password, "moqueur-nid");
    CHECK_STR(c.timezone, "AST4");
    CHECK_STR(c.wifi_ssid, "");
    CHECK_INT(c.batt_low_mv, 3000);
    CHECK_INT(c.batt_critical_mv, 2900);
    /* Une seconde validation ne corrige plus rien. */
    CHECK_INT(nid_config_validate(&c), ESP_OK);

    nid_config_set_defaults(&c);
    c.batt_low_mv = 2800;
    c.batt_critical_mv = 2800;
    CHECK_INT(nid_config_validate(&c), ESP_ERR_INVALID_ARG);
    CHECK_INT(c.batt_critical_mv, 2800);
    CHECK_INT(c.batt_low_mv, 2900);

    /* Chaînes non terminées (mémoire restaurée corrompue). */
    nid_config_set_defaults(&c);
    memset(c.system_id, 'A', sizeof(c.system_id));
    memset(c.timezone, 'B', sizeof(c.timezone));
    CHECK_INT(nid_config_validate(&c), ESP_ERR_INVALID_ARG);
    CHECK_STR(c.system_id, "AAAAAAAAAAAAAAA");
    CHECK_STR(c.timezone, "AST4");
    CHECK_INT(nid_config_validate(NULL), ESP_ERR_INVALID_ARG);
}

static void test_sanitize_id(void)
{
    char out[NID_CONFIG_ID_LEN];
    CHECK_INT(nid_config_sanitize_id(" CAM 01 ", out, sizeof(out)), 6);
    CHECK_STR(out, "CAM_01");
    CHECK_INT(nid_config_sanitize_id("Forêt", out, sizeof(out)), 5);
    CHECK_STR(out, "For_t");
    CHECK_INT(nid_config_sanitize_id("a/b.c;d", out, sizeof(out)), 7);
    CHECK_STR(out, "a_b_c_d");
    CHECK_INT(nid_config_sanitize_id("", out, sizeof(out)), 0);
    CHECK_STR(out, "");
    CHECK_INT(nid_config_sanitize_id("   ", out, sizeof(out)), 0);
    CHECK_INT(nid_config_sanitize_id(NULL, out, sizeof(out)), 0);
    CHECK_INT(nid_config_sanitize_id("ABCDEFGHIJKLMNOPQRSTUVWXYZ", out, sizeof(out)), 15);
    CHECK_STR(out, "ABCDEFGHIJKLMNO");
    char small[2];
    CHECK_INT(nid_config_sanitize_id("xyz", small, sizeof(small)), 1);
    CHECK_STR(small, "x");
    CHECK_INT(nid_config_sanitize_id("ok", NULL, 4), 0);
}

static void test_ini_text(void)
{
    nid_config_t c;
    nid_config_set_defaults(&c);
    char *buf = malloc(NID_CONFIG_INI_MAX_LEN);
    size_t n = nid_config_to_ini(&c, buf, NID_CONFIG_INI_MAX_LEN);
    CHECK(n > 4000);
    CHECK(n < NID_CONFIG_INI_MAX_LEN / 2);  /* marge confortable */
    CHECK_INT(strlen(buf), n);
    CHECK(memcmp(buf, "\xEF\xBB\xBF", 3) == 0);
    static const char *const expected[] = {
        "[systeme]\r\n", "[video]\r\n", "[photo]\r\n", "[detection]\r\n", "[nuit]\r\n", "[energie]\r\n",
        "[installation]\r\n", "\r\nid_systeme = CAM01\r\n", "\r\nid_nid = NID01\r\n",
        "\r\nfuseau_horaire = AST4\r\n", "\r\nduree_sequence_s = 30\r\n",
        "\r\ndelai_entre_declenchements_s = 60\r\n", "\r\nresolution = 1920x1080\r\n",
        "\r\nimages_par_seconde = 15\r\n", "\r\ndebit_kbps = 2500\r\n", "\r\nson = oui\r\n",
        "\r\nintervalle_suivi_min = 15\r\n", "\r\nqualite_jpeg = 80\r\n", "\r\nsensibilite = 3\r\n",
        "\r\nchauffe_pir_s = 60\r\n", "\r\nmode = auto\r\n", "\r\nlatitude = 14.77\r\n",
        "\r\nlongitude = -60.89\r\n", "\r\navance_crepuscule_min = 30\r\n", "\r\nretard_aube_min = 30\r\n",
        "\r\npuissance_ir = 100\r\n", "\r\nnuit_monochrome = oui\r\n", "\r\nseuil_batterie_basse_mv = 3450\r\n",
        "\r\nseuil_batterie_critique_mv = 3350\r\n", "\r\nwifi_ssid =\r\n",
        "\r\nwifi_mot_de_passe = moqueur-nid\r\n", "\r\nduree_max_min = 15\r\n",
        "# Plage : 1 à 300 s. Défaut : 30. (CdCF FP1.4)\r\n",
        "# Valeurs : oui ou non. Défaut : oui. (CdCF FP1.3)\r\n",
        "# Valeurs : auto, jour ou nuit. Défaut : auto. (CdCF FP1.2)\r\n",
        "# Plage : -90 à 90 degrés. Défaut : 14.77. (CdCF FP1.2)\r\n",
        "# Défaut : vide. (CdCF FS1.2)\r\n",
        "# Fin du fichier.\r\n",
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        if (strstr(buf, expected[i]) == NULL) {
            fprintf(stderr, "texte INI attendu absent : « %s »\n", expected[i]);
            CHECK(0);
        }
    }
    /* Aucune fin de ligne LF isolée (CRLF partout). */
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\n' && (i == 0 || buf[i - 1] != '\r')) {
            CHECK(0);
            break;
        }
    }
    /* Tampon trop petit : 0, chaîne vide. */
    char small[100];
    CHECK_INT(nid_config_to_ini(&c, small, sizeof(small)), 0);
    CHECK_STR(small, "");
    CHECK_INT(nid_config_to_ini(&c, small, 0), 0);
    CHECK_INT(nid_config_to_ini(&c, NULL, 100), 0);
    if (g_log_verbose >= 2) {
        fwrite(buf, 1, n, stdout);
    }
    free(buf);
}

static void test_ini_roundtrip(void)
{
    nid_config_t custom;
    make_custom(&custom);
    char path[600];
    path_in_dir(path, sizeof(path), "roundtrip.ini");
    CHECK_INT(nid_config_save(&custom, path), ESP_OK);
    char tmp[620];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    CHECK(!file_exists(tmp));

    char *text = read_file(path, NULL);
    CHECK(text != NULL && strstr(text, "wifi_mot_de_passe = \" a#b;c\"\"d e \"\r\n") != NULL);
    CHECK(text != NULL && strstr(text, "wifi_ssid = \"Nid Forêt #2 ; \"\"test\"\"\"\r\n") != NULL);
    CHECK(text != NULL && strstr(text, "latitude = -14.5\r\n") != NULL);
    CHECK(text != NULL && strstr(text, "longitude = 179.875\r\n") != NULL);
    free(text);

    nid_config_t back;
    nid_config_set_defaults(&back);
    int w = g_log_warnings;
    CHECK_INT(nid_config_load(&back, path), ESP_OK);
    CHECK_INT(g_log_warnings, w);
    CHECK(cfg_equal(&back, &custom));

    /* Les valeurs par défaut aussi. */
    nid_config_t def;
    nid_config_set_defaults(&def);
    CHECK_INT(nid_config_save(&def, path), ESP_OK);
    CHECK_INT(nid_config_load(&back, path), ESP_OK);
    CHECK(cfg_equal(&back, &def));
}

static void test_ini_tolerant_parser(void)
{
    char path[600];
    path_in_dir(path, sizeof(path), "tolerant.ini");
    char longline[400];
    memset(longline, 'x', sizeof(longline) - 3);
    longline[sizeof(longline) - 3] = '\r';
    longline[sizeof(longline) - 2] = '\n';
    longline[sizeof(longline) - 1] = '\0';
    char text[2048];
    snprintf(text, sizeof(text),
             "\xEF\xBB\xBF# commentaire\r\n"
             "; autre commentaire\r\n"
             "[SYSTEME]\r\n"
             "  ID_Systeme   =   BOIS-03   # identifiant\r\n"
             "id_nid=N7;commentaire collé\r\n"
             "\tfuseau_horaire\t=\tUTC0\t; UTC\r\n"
             "[video]   \r\n"
             "duree_sequence_s = 999\r\n"           /* hors plage : ignoré */
             "delai_entre_declenchements_s = 10\r\n"
             "delai_entre_declenchements_s = 20\r\n" /* répétée : la dernière s'applique */
             "resolution = 1280 X 960\r\n"
             "son = NON\r\n"
             "cle_inconnue = 4\r\n"
             "ligne sans egal\r\n"
             "%s"
             "[nuit\r\n"
             "latitude = 16,25\r\n"
             "longitude = -61.5   ; ouest\r\n"
             "mode = \"nuit\"   # entre guillemets\r\n"
             "wifi_mot_de_passe = \"mot#de;passe\"\"x\"\r\n"
             "wifi_ssid = \"non fermé\r\n"
             "qualite_jpeg = 70 trop\r\n"
             "puissance_ir = 50\n"                  /* fin de ligne LF seule */
             "sensibilite = 4",                     /* dernière ligne sans fin de ligne */
             longline);
    write_text(path, text);

    nid_config_t c;
    nid_config_set_defaults(&c);
    int w = g_log_warnings;
    CHECK_INT(nid_config_load(&c, path), ESP_OK);
    int warnings = g_log_warnings - w;
    CHECK(warnings >= 7);
    CHECK_STR(c.system_id, "BOIS-03");
    CHECK_STR(c.nest_id, "N7");
    CHECK_STR(c.timezone, "UTC0");
    CHECK_INT(c.seq_duration_s, 30);
    CHECK_INT(c.trigger_delay_s, 20);
    CHECK_INT(c.video_width, 1280);
    CHECK_INT(c.video_height, 960);
    CHECK(!c.audio_enabled);
    CHECK(c.latitude == 16.25f);
    CHECK(c.longitude == -61.5f);
    CHECK_INT(c.night_mode, NID_NIGHT_MODE_NIGHT);
    CHECK_STR(c.wifi_password, "mot#de;passe\"x");
    CHECK_STR(c.wifi_ssid, "");
    CHECK_INT(c.jpeg_quality, 80);
    CHECK_INT(c.ir_power_pct, 50);
    CHECK_INT(c.pir_sensitivity, 4);
}

static void test_load_missing_empty_invalid(void)
{
    char path[600];
    nid_config_t c;
    nid_config_t ref;
    nid_config_set_defaults(&c);
    c.seq_duration_s = 77;
    ref = c;
    path_in_dir(path, sizeof(path), "absent.ini");
    CHECK_INT(nid_config_load(&c, path), ESP_ERR_NOT_FOUND);
    CHECK(cfg_equal(&c, &ref));
    path_in_dir(path, sizeof(path), "vide.ini");
    write_text(path, "");
    CHECK_INT(nid_config_load(&c, path), ESP_ERR_NOT_FOUND);
    CHECK(cfg_equal(&c, &ref));
    /* Fichier binaire quelconque : lu sans planter, réglages inchangés. */
    path_in_dir(path, sizeof(path), "binaire.ini");
    char junk[3000];
    for (size_t i = 0; i < sizeof(junk); i++) {
        junk[i] = (char)((i * 7919u + 13u) & 0xFF);
    }
    write_file(path, junk, sizeof(junk));
    CHECK_INT(nid_config_load(&c, path), ESP_OK);
    CHECK_INT(c.seq_duration_s, 77);
    CHECK_INT(nid_config_load(NULL, path), ESP_ERR_INVALID_ARG);
    CHECK_INT(nid_config_load(&c, NULL), ESP_ERR_INVALID_ARG);
}

static void test_json(void)
{
    nid_config_t c;
    nid_config_set_defaults(&c);
    char buf[NID_CONFIG_JSON_MAX_LEN];
    size_t n = nid_config_to_json(&c, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK_STR(buf,
              "{\"id_systeme\":\"CAM01\",\"id_nid\":\"NID01\",\"fuseau_horaire\":\"AST4\","
              "\"duree_sequence_s\":30,\"delai_entre_declenchements_s\":60,\"resolution\":\"1920x1080\","
              "\"images_par_seconde\":15,\"debit_kbps\":2500,\"son\":true,\"intervalle_suivi_min\":15,"
              "\"qualite_jpeg\":80,\"sensibilite\":3,\"chauffe_pir_s\":60,\"mode\":\"auto\","
              "\"latitude\":14.77,\"longitude\":-60.89,\"avance_crepuscule_min\":30,\"retard_aube_min\":30,"
              "\"puissance_ir\":100,\"nuit_monochrome\":true,\"seuil_batterie_basse_mv\":3450,"
              "\"seuil_batterie_critique_mv\":3350,\"wifi_ssid\":\"\",\"wifi_mot_de_passe\":\"moqueur-nid\","
              "\"duree_max_min\":15}");

    /* Relecture complète par nid_config_set (page d'installation). */
    nid_config_t custom;
    make_custom(&custom);
    n = nid_config_to_json(&custom, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "\"wifi_ssid\":\"Nid Forêt #2 ; \\\"test\\\"\"") != NULL);
    CHECK(strstr(buf, "\"wifi_mot_de_passe\":\" a#b;c\\\"d e \"") != NULL);
    CHECK(strstr(buf, "\"son\":false") != NULL);
    nid_config_t back;
    nid_config_set_defaults(&back);
    CHECK_INT(json_apply(&back, buf), 25);
    CHECK(cfg_equal(&back, &custom));

    /* Échappement de caractères qui ne passent pas par nid_config_set. */
    strcpy(c.wifi_ssid, "a\"b\\c<d>&e\nf\x01");
    n = nid_config_to_json(&c, buf, sizeof(buf));
    CHECK(strstr(buf, "\"wifi_ssid\":\"a\\\"b\\\\c\\u003cd\\u003e\\u0026e\\nf\\u0001\"") != NULL);
    char key[64];
    char val[64];
    const char *p = strstr(buf, "\"wifi_ssid\"");
    CHECK(p != NULL);
    if (p != NULL) {
        p = json_string(p, key, sizeof(key));
        CHECK(p != NULL && *p == ':');
        if (p != NULL) {
            CHECK(json_string(p + 1, val, sizeof(val)) != NULL);
            CHECK_STR(val, "a\"b\\c<d>&e\nf\x01");
        }
    }
    /* Tampon trop petit. */
    char small[64];
    CHECK_INT(nid_config_to_json(&c, small, sizeof(small)), 0);
    CHECK_STR(small, "");
}

/* --- Écriture atomique et reprise après coupure --- */

static void test_atomic_write(void)
{
    char path[600];
    char tmp[620];
    path_in_dir(path, sizeof(path), "atomique.txt");
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    CHECK_INT(nid_config_write_file_atomic(path, "version 1\n", 10), ESP_OK);
    char *s = read_file(path, NULL);
    CHECK_STR(s, "version 1\n");
    free(s);
    CHECK(!file_exists(tmp));
    CHECK_INT(nid_config_write_file_atomic(path, "v2", 2), ESP_OK);
    s = read_file(path, NULL);
    CHECK_STR(s, "v2");
    free(s);
    CHECK_INT(nid_config_write_file_atomic(path, NULL, 0), ESP_OK);
    size_t len = 99;
    s = read_file(path, &len);
    CHECK_INT(len, 0);
    free(s);
    CHECK_INT(nid_config_write_file_atomic(path, NULL, 3), ESP_ERR_INVALID_ARG);
    char missing_dir[640];
    snprintf(missing_dir, sizeof(missing_dir), "%s/absent/x.txt", s_dir);
    CHECK(nid_config_write_file_atomic(missing_dir, "x", 1) != ESP_OK);
}

/* CRC-32 de « 123456789 » = CBF43926 (valeur de référence IEEE 802.3). */
static void test_recover_reference_crc(void)
{
    char path[600];
    char tmp[620];
    path_in_dir(path, sizeof(path), "crc.txt");
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    write_text(tmp, "123456789\n#NIDTMP 0000000009 CBF43926\n");
    CHECK(nid_config_tmp_is_complete(tmp));
    CHECK_INT(nid_config_recover_file(path), ESP_OK);
    char *s = read_file(path, NULL);
    CHECK_STR(s, "123456789");
    free(s);
    CHECK(!file_exists(tmp));
}

static void test_recover_states(void)
{
    char path[600];
    char tmp[620];
    path_in_dir(path, sizeof(path), "etat.txt");
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    /* a. Coupure pendant l'écriture du temporaire : ancienne version conservée. */
    write_text(path, "ancien");
    write_text(tmp, "nouv");  /* pas de ligne de contrôle */
    CHECK(!nid_config_tmp_is_complete(tmp));
    CHECK_INT(nid_config_recover_file(path), ESP_OK);
    char *s = read_file(path, NULL);
    CHECK_STR(s, "ancien");
    free(s);
    CHECK(!file_exists(tmp));

    /* Temporaire vide (créé, rien écrit) : annulé aussi. */
    write_text(tmp, "");
    CHECK(!nid_config_tmp_is_complete(tmp));
    CHECK_INT(nid_config_recover_file(path), ESP_OK);
    s = read_file(path, NULL);
    CHECK_STR(s, "ancien");
    free(s);
    CHECK(!file_exists(tmp));
}

/* Recrée un temporaire complet comme l'étape 1 du module (format documenté). */
static void make_complete_tmp(const char *tmp, const char *data)
{
    /* CRC-32 IEEE calculé ici indépendamment du module. */
    uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char *p = (const unsigned char *)data; *p; p++) {
        crc ^= *p;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    crc = ~crc;
    FILE *f = fopen(tmp, "wb");
    fprintf(f, "%s\n#NIDTMP %010lu %08lX\n", data, (unsigned long)strlen(data), (unsigned long)crc);
    fclose(f);
}

static void test_recover_complete_tmp(void)
{
    char path[600];
    char tmp[620];
    path_in_dir(path, sizeof(path), "reprise.txt");
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    /* b. path tronqué, temporaire complet → path réécrit. */
    write_text(path, "nouv");
    make_complete_tmp(tmp, "nouveau contenu");
    CHECK(nid_config_tmp_is_complete(tmp));
    CHECK_INT(nid_config_recover_file(path), ESP_OK);
    char *s = read_file(path, NULL);
    CHECK_STR(s, "nouveau contenu");
    free(s);
    CHECK(!file_exists(tmp));

    /* c. path absent, temporaire complet → path créé. */
    remove(path);
    make_complete_tmp(tmp, "recréé");
    CHECK_INT(nid_config_recover_file(path), ESP_OK);
    s = read_file(path, NULL);
    CHECK_STR(s, "recréé");
    free(s);
    CHECK(!file_exists(tmp));

    /* d. path déjà à jour (coupure avant la suppression du temporaire). */
    make_complete_tmp(tmp, "recréé");
    CHECK_INT(nid_config_recover_file(path), ESP_OK);
    s = read_file(path, NULL);
    CHECK_STR(s, "recréé");
    free(s);
    CHECK(!file_exists(tmp));

    /* e. Ligne de contrôle présente mais CRC faux (secteur abîmé) → annulé. */
    write_text(path, "intact");
    write_text(tmp, "abcdef\n#NIDTMP 0000000006 12345678\n");
    CHECK(nid_config_tmp_is_complete(tmp));  /* longueur cohérente */
    CHECK_INT(nid_config_recover_file(path), ESP_OK);
    s = read_file(path, NULL);
    CHECK_STR(s, "intact");
    free(s);
    CHECK(!file_exists(tmp));

    /* f. Longueur annoncée incohérente → incomplet. */
    write_text(tmp, "abc\n#NIDTMP 0000000009 CBF43926\n");
    CHECK(!nid_config_tmp_is_complete(tmp));
    CHECK_INT(nid_config_recover_file(path), ESP_OK);
    CHECK(!file_exists(tmp));

    /* g. Sans temporaire : rien à faire. */
    CHECK_INT(nid_config_recover_file(path), ESP_OK);
    CHECK(!nid_config_tmp_is_complete(tmp));
    CHECK_INT(nid_config_recover_file(NULL), ESP_ERR_INVALID_ARG);

    /* h. Écriture atomique après une coupure : le temporaire complet est d'abord appliqué. */
    write_text(path, "vieux");
    make_complete_tmp(tmp, "intermédiaire");
    CHECK_INT(nid_config_write_file_atomic(path, "final", 5), ESP_OK);
    s = read_file(path, NULL);
    CHECK_STR(s, "final");
    free(s);
    CHECK(!file_exists(tmp));
}

/* config.ini tronqué par une coupure, config.ini.tmp complet : la lecture reprend la bonne version. */
static void test_load_after_power_cut(void)
{
    nid_config_t custom;
    make_custom(&custom);
    char path[600];
    char tmp[620];
    path_in_dir(path, sizeof(path), "config.ini");
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    char *ini = malloc(NID_CONFIG_INI_MAX_LEN);
    size_t n = nid_config_to_ini(&custom, ini, NID_CONFIG_INI_MAX_LEN);
    CHECK(n > 0);
    make_complete_tmp(tmp, ini);
    write_file(path, ini, n / 3);  /* path à moitié réécrit */
    nid_config_t c;
    nid_config_set_defaults(&c);
    CHECK_INT(nid_config_load(&c, path), ESP_OK);
    CHECK(cfg_equal(&c, &custom));
    CHECK(!file_exists(tmp));
    size_t len = 0;
    char *s = read_file(path, &len);
    CHECK_INT(len, n);
    CHECK(s != NULL && memcmp(s, ini, n) == 0);
    free(s);

    /* Coupure pendant l'écriture du temporaire : l'ancienne version est lue. */
    write_file(tmp, ini, n / 2);
    nid_config_set_defaults(&c);
    CHECK_INT(nid_config_load(&c, path), ESP_OK);
    CHECK(cfg_equal(&c, &custom));
    CHECK(!file_exists(tmp));

    /* Première écriture interrompue (pas encore de config.ini) : réglages par défaut, fichier à recréer. */
    remove(path);
    write_file(tmp, ini, n / 2);
    nid_config_t def;
    nid_config_set_defaults(&def);
    nid_config_set_defaults(&c);
    CHECK_INT(nid_config_load(&c, path), ESP_ERR_NOT_FOUND);
    CHECK(cfg_equal(&c, &def));
    CHECK(!file_exists(tmp));
    free(ini);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage : %s <dossier de travail>\n", argv[0]);
        return 2;
    }
    snprintf(s_dir, sizeof(s_dir), "%s", argv[1]);
    mkdir(s_dir, 0755);

    RUN(test_defaults);
    RUN(test_set_ranges);
    RUN(test_set_unknown_key);
    RUN(test_booleans);
    RUN(test_mode_resolution);
    RUN(test_floats);
    RUN(test_ids_tz_wifi);
    RUN(test_validate);
    RUN(test_sanitize_id);
    RUN(test_ini_text);
    RUN(test_ini_roundtrip);
    RUN(test_ini_tolerant_parser);
    RUN(test_load_missing_empty_invalid);
    RUN(test_json);
    RUN(test_atomic_write);
    RUN(test_recover_reference_crc);
    RUN(test_recover_states);
    RUN(test_recover_complete_tmp);
    RUN(test_load_after_power_cut);
    return test_summary("nid_config");
}
