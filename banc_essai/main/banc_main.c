/*
 * Banc d'essai « Suivi nid » — validation pas à pas sur l'ESP32-P4X-Function-EV-Board
 *
 * Étape 1 : enregistrer une vidéo sur appui du bouton BOOT par le vrai chemin
 * du firmware de terrain (nid_storage_media_begin → nid_capture_record →
 * nid_storage_media_commit, ou nid_storage_media_abort), puis afficher le
 * bilan à l'écran, sur le journal série (ligne « BILAN ») et dans le journal
 * CSV de la carte SD.
 *
 * Tâches :
 *  - « banc_action » : seule tâche qui utilise la caméra : enregistrements et
 *    photos demandés par le bouton ou la console (un seul à la fois), viseur
 *    (aperçu de nid_capture, images remises à l'écran par le crochet de
 *    débogage) entre deux actions ;
 *  - « banc_bouton » : scrute le bouton (anti-rebond), demande un
 *    enregistrement ou l'arrêt anticipé de celui en cours ;
 *  - console série (esp_console), voir banc_console.c.
 *
 * Aucune erreur ne redémarre la carte : elle est affichée (étape, code
 * d'erreur, piste de dépannage) et le banc reste utilisable : le prochain
 * appui réessaie le montage de la carte SD ou l'initialisation de la caméra.
 */
#include "banc.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "nid_board.h"
#include "nid_capture.h"
#include "nid_storage.h"

static const char *TAG = "banc";

#define ID_SYSTEME              "BANC"
#define ID_NID                  "N00"
#define ETIQUETTE               "BANC N00"      /* incrusté après la date et l'heure */

#define ACTION_PILE             (12 * 1024)
#define ACTION_PRIO             5
#define BOUTON_PILE             3072
#define BOUTON_PRIO             4
#define BOUTON_PERIODE_MS       10
#define BOUTON_STABLE           3               /* 3 lectures identiques : 30 ms d'anti-rebond */

#define VISEUR_PERIODE_US       (1000000LL / 12)  /* viseur : 12 images par seconde au plus */
#define VISEUR_QUALITE          30              /* qualité JPEG de l'aperçu (non utilisé par l'écran) */
#define VISEUR_ATTENTE_MS       300
#define VISEUR_ECHECS_MAX       10
#define ENTETE_PERIODE_US       1000000LL

#define HEURE_VALIDE_MIN        1704067200      /* 2024-01-01 : avant, l'heure n'est pas réglée */
#define DUREE_MAX_S             300

typedef enum {
    ACTION_VIDEO = 0,
    ACTION_PHOTO,
} action_t;

typedef struct {
    action_t type;
    uint32_t duree_s;
    int64_t  t_demande_us;      /*!< instant de l'appui ou de la commande (esp_timer) */
    char     origine[12];       /*!< « bouton » ou « console » */
} demande_t;

static QueueHandle_t s_file;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_occupe;                       /* action demandée ou en cours (sous s_mux) */
static volatile bool s_enregistre;          /* enregistrement vidéo en cours */
static volatile bool s_arret;               /* arrêt anticipé demandé par le bouton */

static nid_capture_settings_t s_reglages;
static uint32_t s_seq_video;
static uint32_t s_seq_photo;
static volatile uint16_t s_img_w, s_img_h;  /* dernière image vue par le crochet */
static uint64_t s_sd_libre;                 /* octets libres (mis à jour après chaque action) */
static uint64_t s_sd_total;
static int s_recuperes = -1;                /* fichiers renommés _INCOMPLET au dernier montage */
static char s_bilan[512] = "aucun";         /* dernière ligne de bilan (commande info) */

/* Enregistrement en cours (affichage de la progression) */
static int64_t s_t_debut_us;
static uint32_t s_duree_s;
static int32_t s_seconde_affichee;

/* Viseur */
static bool s_viseur_demarre;
static int s_viseur_echecs;

/* ------------------------------------------------------------------------- */
/* Outils                                                                     */
/* ------------------------------------------------------------------------- */

/** Heure locale au format fmt (strftime). */
static void heure_locale(char *out, size_t len, const char *fmt)
{
    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (strftime(out, len, fmt, &tm) == 0) {
        strlcpy(out, "?", len);
    }
}

/** Heure non réglée (pas d'horloge sauvegardée sur le banc) : heure de compilation de ce fichier. */
static void regler_heure_compilation(void)
{
    if (time(NULL) >= HEURE_VALIDE_MIN) {
        return;
    }
    static const char mois[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char m[4] = "";
    int j = 0, a = 0, h = 0, mi = 0, s = 0;
    if (sscanf(__DATE__, "%3s %d %d", m, &j, &a) != 3 || sscanf(__TIME__, "%d:%d:%d", &h, &mi, &s) != 3) {
        return;
    }
    const char *pos = strstr(mois, m);
    if (pos == NULL || m[0] == '\0') {
        return;
    }
    struct tm tm = {
        .tm_year = a - 1900,
        .tm_mon = (int)((pos - mois) / 3),
        .tm_mday = j,
        .tm_hour = h,
        .tm_min = mi,
        .tm_sec = s,
        .tm_isdst = -1,
    };
    const struct timeval tv = { .tv_sec = mktime(&tm) };
    settimeofday(&tv, NULL);
    ESP_LOGW(TAG, "heure non réglée : heure de compilation du banc utilisée (%s %s) ; "
             "la régler avec la commande « heure AAAA-MM-JJ HH:MM:SS »", __DATE__, __TIME__);
}

static void lire_espace_sd(void)
{
    if (nid_storage_get_space(&s_sd_total, &s_sd_libre) != ESP_OK) {
        s_sd_total = 0;
        s_sd_libre = 0;
    }
}

/** « 29.1 Go libres » ou « absente ». */
static void texte_sd(char *out, size_t len)
{
    if (!nid_storage_is_mounted()) {
        strlcpy(out, "absente", len);
        return;
    }
    const uint64_t dixiemes = s_sd_libre / 100000000ULL;    /* dixièmes de Go (1 Go = 10^9 o) */
    snprintf(out, len, "%" PRIu64 ".%" PRIu64 " Go libres", dixiemes / 10, dixiemes % 10);
}

void banc_rafraichir_entete(void)
{
    char sd[24];
    char heure[16];
    texte_sd(sd, sizeof(sd));
    heure_locale(heure, sizeof(heure), "%H:%M:%S");
    banc_ecran_ligne(BANC_LIGNE_ENTETE, BANC_GRIS, "%s | %ux%u %u i/s %" PRIu32 " kbit/s%s%s | SD %s | %s",
                     nid_capture_sensor_name(), s_reglages.width, s_reglages.height, s_reglages.fps,
                     s_reglages.bitrate_kbps, s_reglages.audio ? " son" : "", s_reglages.night ? " nuit" : "",
                     sd, heure);
}

static void effacer_details(void)
{
    banc_ecran_ligne(BANC_LIGNE_DETAIL1, BANC_BLANC, "%s", "");
    banc_ecran_ligne(BANC_LIGNE_DETAIL2, BANC_BLANC, "%s", "");
    banc_ecran_ligne(BANC_LIGNE_DETAIL3, BANC_BLANC, "%s", "");
}

static void afficher_pret(void)
{
    banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_VERT, "Prêt — appuyez sur BOOT pour enregistrer %d s", CONFIG_BANC_DUREE_S);
}

/**
 * Affiche une erreur : étape et code (ligne d'état), piste de dépannage (une
 * ou deux lignes séparées par « \n »), puis la marche à suivre.
 */
static void afficher_erreur(const char *etape, esp_err_t err, const char *piste)
{
    char l1[160];
    const char *l2 = "";
    const char *nl = strchr(piste, '\n');
    if (nl != NULL) {
        snprintf(l1, sizeof(l1), "%.*s", (int)(nl - piste), piste);
        l2 = nl + 1;
    } else {
        strlcpy(l1, piste, sizeof(l1));
    }
    ESP_LOGE(TAG, "ERREUR %s : %s — %s %s", etape, esp_err_to_name(err), l1, l2);
    banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_ROUGE, "ERREUR %s : %s", etape, esp_err_to_name(err));
    banc_ecran_ligne(BANC_LIGNE_DETAIL1, BANC_ROUGE, "%s", l1);
    banc_ecran_ligne(BANC_LIGNE_DETAIL2, BANC_ROUGE, "%s", l2);
    banc_ecran_ligne(BANC_LIGNE_DETAIL3, BANC_GRIS, "%s", "Corriger, puis appuyer sur BOOT pour réessayer");
}

static const char *piste_sd(esp_err_t err)
{
    if (err == ESP_FAIL) {
        return "Carte SD illisible : la formater en FAT32 sur un ordinateur\n"
               "(le banc ne formate jamais une carte)";
    }
    return "Carte SD absente ou mal insérée ? (microSD FAT32,\n"
           "enfoncée jusqu'au déclic, carte éteinte pour l'insérer)";
}

static const char *piste_camera(esp_err_t err)
{
    if (err == ESP_ERR_NO_MEM) {
        return "Mémoire insuffisante : la PSRAM est-elle activée ?";
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return "Bus I2C indisponible : voir le journal série (nid_board)";
    }
    return "Caméra non détectée : nappe (sens, clapets verrouillés),\n"
           "commande « i2cscan » (SC2336 = 0x30, OV5647 = 0x36)";
}

static const char *piste_video(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_TIMEOUT:
        return "Aucune image reçue de la caméra : nappe, alimentation ;\n"
               "voir le journal série (nid_capture)";
    case ESP_ERR_NO_MEM:
        return "Mémoire insuffisante (multiplexeur MP4)";
    case ESP_ERR_INVALID_RESPONSE:
        return "Encodeur H.264 : image codée invalide\n(voir le journal série)";
    case ESP_ERR_INVALID_STATE:
        return "Caméra non prête : appuyer de nouveau sur BOOT";
    default:
        return "Écriture impossible : carte SD pleine, trop lente (A1/V30)\n"
               "ou retirée ? Voir le journal série";
    }
}

/* ------------------------------------------------------------------------- */
/* Carte SD et caméra                                                         */
/* ------------------------------------------------------------------------- */

/**
 * Monte la carte, pose les identifiants puis renomme en « _INCOMPLET » les
 * fichiers interrompus (coupure pendant une écriture), comme le firmware.
 */
static esp_err_t preparer_sd(void)
{
    if (nid_storage_is_mounted()) {
        return ESP_OK;
    }
    esp_err_t err = nid_storage_mount();
    if (err == ESP_OK) {
        err = nid_storage_set_ids(ID_SYSTEME, ID_NID);
    }
    if (err != ESP_OK) {
        return err;
    }
    s_recuperes = nid_storage_recover();
    lire_espace_sd();
    if (s_recuperes > 0) {
        ESP_LOGW(TAG, "RECUPERATION fichiers_incomplets=%d (renommés _INCOMPLET dans %s)", s_recuperes,
                 nid_storage_base_dir());
        nid_storage_journal("fichiers_incomplets", "%d fichier(s) interrompu(s) renommé(s) en _INCOMPLET", s_recuperes);
    } else if (s_recuperes == 0) {
        ESP_LOGI(TAG, "RECUPERATION fichiers_incomplets=0");
    } else {
        ESP_LOGW(TAG, "RECUPERATION impossible (lecture du dossier %s)", nid_storage_base_dir());
    }
    return ESP_OK;
}

static void afficher_sd(banc_ligne_t ligne)
{
    char sd[24];
    texte_sd(sd, sizeof(sd));
    if (s_recuperes > 0) {
        banc_ecran_ligne(ligne, BANC_JAUNE, "Carte SD : %s ; %d fichier(s) interrompu(s) renommé(s) _INCOMPLET",
                         sd, s_recuperes);
    } else {
        banc_ecran_ligne(ligne, BANC_BLANC, "Carte SD : %s ; %s", sd,
                         s_recuperes == 0 ? "aucun fichier interrompu" : "récupération impossible");
    }
}

static esp_err_t preparer_camera(void)
{
    if (nid_capture_is_ready()) {
        return ESP_OK;
    }
    s_viseur_demarre = false;
    esp_err_t err = nid_capture_init(&s_reglages);
    if (err != ESP_OK) {
        banc_ecran_message_vue(BANC_ROUGE, "Caméra absente");
    }
    return err;
}

/** Carte SD puis caméra ; en cas d'échec, l'erreur est affichée. */
static bool preparer_tout(void)
{
    esp_err_t err = preparer_sd();
    if (err != ESP_OK) {
        afficher_erreur("carte SD", err, piste_sd(err));
        return false;
    }
    err = preparer_camera();
    if (err != ESP_OK) {
        afficher_erreur("caméra", err, piste_camera(err));
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Vidéo                                                                      */
/* ------------------------------------------------------------------------- */

/** Rappel de nid_capture_record (~ toutes les 100 ms) : progression, arrêt anticipé. */
static bool rappel_enregistrement(void *arg)
{
    (void)arg;
    const int32_t sec = (int32_t)((esp_timer_get_time() - s_t_debut_us) / 1000000LL);
    if (sec != s_seconde_affichee) {
        s_seconde_affichee = sec;
        banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_ROUGE, "Enregistrement %" PRId32 "/%" PRIu32 " s — BOOT pour arrêter",
                         sec, s_duree_s);
    }
    return s_arret;
}

static void bilan_video(const demande_t *d, const nid_media_file_t *f, const nid_capture_stats_t *st,
                        esp_err_t err, bool finalisation, bool arrete)
{
    const uint32_t ms = st->duration_ms;
    const uint32_t ips10 = ms ? (uint32_t)((uint64_t)st->video_frames * 10000ULL / ms) : 0;
    const uint32_t mo100 = (uint32_t)(st->bytes / 10000ULL);    /* centièmes de Mo (1 Mo = 10^6 o) */
    const uint32_t kbps = ms ? (uint32_t)(st->bytes * 8ULL / ms) : 0;
    const int32_t premiere = st->first_frame_ms ? (int32_t)((int64_t)st->first_frame_ms - d->t_demande_us / 1000) : -1;
    const bool ok = (err == ESP_OK);
    const char *resultat = ok ? (arrete ? "OK_ARRET_ANTICIPE" : "OK") : "ECHEC";

    snprintf(s_bilan, sizeof(s_bilan),
             "BILAN video fichier=%s resultat=%s erreur=%s duree=%" PRIu32 ".%02" PRIu32 "s images=%" PRIu32
             " cadence=%" PRIu32 ".%" PRIu32 "ips perdues=%" PRIu32 " trames_son=%" PRIu32 " taille=%" PRIu32
             ".%02" PRIu32 "Mo debit=%" PRIu32 "kbit/s premiere_image=%" PRId32 "ms reglages=%ux%u,%uips,%" PRIu32
             "kbit/s,son=%s capteur=%s",
             f->name, resultat, esp_err_to_name(err), ms / 1000, (ms % 1000) / 10, st->video_frames,
             ips10 / 10, ips10 % 10, st->dropped_frames, st->audio_frames, mo100 / 100, mo100 % 100, kbps,
             premiere, s_reglages.width, s_reglages.height, s_reglages.fps, s_reglages.bitrate_kbps,
             s_reglages.audio ? "oui" : "non", nid_capture_sensor_name());
    ESP_LOGI(TAG, "%s", s_bilan);

    if (ok) {
        nid_storage_journal("video_banc", "%s;%" PRIu32 " ms;%" PRIu32 " images;%" PRIu32 ".%" PRIu32 " i/s;%" PRIu32
                            " perdues;%" PRIu32 " trames son;%" PRIu32 ".%02" PRIu32 " Mo;%" PRIu32 " kbit/s;"
                            "1re image %" PRId32 " ms;%s",
                            f->name, ms, st->video_frames, ips10 / 10, ips10 % 10, st->dropped_frames,
                            st->audio_frames, mo100 / 100, mo100 % 100, kbps, premiere, d->origine);
        banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_VERT, "Vidéo enregistrée%s — BOOT : nouvel essai",
                         arrete ? " (arrêt anticipé)" : "");
        banc_ecran_ligne(BANC_LIGNE_DETAIL1, BANC_BLANC, "%s", f->name);
    } else {
        nid_storage_journal("erreur", "video_banc %s : %s (%s)", f->name, esp_err_to_name(err),
                            finalisation ? "finalisation" : "enregistrement");
        if (finalisation) {
            afficher_erreur("finalisation", err, "Renommage .tmp → .mp4 impossible : carte SD retirée ?\n"
                            "Le .tmp sera renommé _INCOMPLET au prochain démarrage");
        } else {
            afficher_erreur("enregistrement", err, piste_video(err));
            banc_ecran_ligne(BANC_LIGNE_DETAIL3, BANC_GRIS, "%s", "Fichier partiel conservé sous le nom ..._INCOMPLET.mp4");
        }
        return;
    }
    banc_ecran_ligne(BANC_LIGNE_DETAIL2, BANC_BLANC,
                     "%" PRIu32 ".%02" PRIu32 " s | %" PRIu32 " images | %" PRIu32 ".%" PRIu32 " i/s | %" PRIu32
                     " perdue(s) | son : %" PRIu32 " trames",
                     ms / 1000, (ms % 1000) / 10, st->video_frames, ips10 / 10, ips10 % 10, st->dropped_frames,
                     st->audio_frames);
    banc_ecran_ligne(BANC_LIGNE_DETAIL3, st->dropped_frames ? BANC_JAUNE : BANC_BLANC,
                     "%" PRIu32 ".%02" PRIu32 " Mo | %" PRIu32 " kbit/s | 1re image à %" PRId32 " ms de l'appui",
                     mo100 / 100, mo100 % 100, kbps, premiere);
}

static void faire_video(const demande_t *d)
{
    effacer_details();
    banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_JAUNE, "Préparation de l'enregistrement (%s)...", d->origine);
    if (!preparer_tout()) {
        return;
    }
    nid_media_file_t f;
    esp_err_t err = nid_storage_media_begin(NID_MEDIA_VIDEO, time(NULL), ++s_seq_video, &f);
    if (err != ESP_OK) {
        afficher_erreur("fichier", err, "Carte SD pleine, protégée en écriture ou retirée ?");
        return;
    }

    ESP_LOGI(TAG, "enregistrement de %" PRIu32 " s (%s) : %s", d->duree_s, d->origine, f.tmp_path);
#if !CONFIG_BANC_VUE_ENREG
    banc_ecran_message_vue(BANC_GRIS, "Vue coupée pendant l'enregistrement");
#endif
    s_duree_s = d->duree_s;
    s_t_debut_us = esp_timer_get_time();
    s_seconde_affichee = -1;
    s_arret = false;
    s_enregistre = true;
    nid_capture_stats_t st;
    err = nid_capture_record(f.tmp_path, d->duree_s * 1000u, rappel_enregistrement, NULL, &st);
    s_enregistre = false;
    const bool arrete = s_arret;
    s_arret = false;

    bool finalisation = false;
    if (err == ESP_OK) {
        err = nid_storage_media_commit(&f);       /* .tmp → nom définitif */
        finalisation = (err != ESP_OK);
    } else {
        nid_storage_media_abort(&f, true);        /* conservé en _INCOMPLET, comme le firmware */
    }
    lire_espace_sd();
    bilan_video(d, &f, &st, err, finalisation, arrete);
    banc_rafraichir_entete();
}

/* ------------------------------------------------------------------------- */
/* Photo                                                                      */
/* ------------------------------------------------------------------------- */

static void faire_photo(const demande_t *d)
{
    effacer_details();
    banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_JAUNE, "Photo (%s)...", d->origine);
    if (!preparer_tout()) {
        return;
    }
    nid_media_file_t f;
    esp_err_t err = nid_storage_media_begin(NID_MEDIA_PHOTO, time(NULL), ++s_seq_photo, &f);
    if (err != ESP_OK) {
        afficher_erreur("fichier", err, "Carte SD pleine, protégée en écriture ou retirée ?");
        return;
    }
    err = nid_capture_photo(f.tmp_path);
    if (err == ESP_OK) {
        err = nid_storage_media_commit(&f);
    } else {
        nid_storage_media_abort(&f, false);
    }
    const uint32_t ms = (uint32_t)((esp_timer_get_time() - d->t_demande_us) / 1000);
    struct stat sb;
    const uint32_t ko = (err == ESP_OK && stat(f.final_path, &sb) == 0) ? (uint32_t)(sb.st_size / 1000) : 0;
    lire_espace_sd();

    snprintf(s_bilan, sizeof(s_bilan), "BILAN photo fichier=%s resultat=%s erreur=%s taille=%" PRIu32 "ko duree=%" PRIu32
             "ms reglages=%ux%u,qualite=%u capteur=%s", f.name, err == ESP_OK ? "OK" : "ECHEC", esp_err_to_name(err),
             ko, ms, s_reglages.width, s_reglages.height, s_reglages.jpeg_quality, nid_capture_sensor_name());
    ESP_LOGI(TAG, "%s", s_bilan);
    if (err == ESP_OK) {
        nid_storage_journal("photo_banc", "%s;%" PRIu32 " ko;%" PRIu32 " ms;%s", f.name, ko, ms, d->origine);
        banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_VERT, "%s", "Photo enregistrée — BOOT : vidéo");
        banc_ecran_ligne(BANC_LIGNE_DETAIL1, BANC_BLANC, "%s", f.name);
        banc_ecran_ligne(BANC_LIGNE_DETAIL2, BANC_BLANC, "%" PRIu32 " ko | %" PRIu32 " ms depuis la commande", ko, ms);
    } else {
        nid_storage_journal("erreur", "photo_banc %s : %s", f.name, esp_err_to_name(err));
        afficher_erreur("photo", err, piste_video(err));
    }
    banc_rafraichir_entete();
}

/* ------------------------------------------------------------------------- */
/* Viseur et tâche d'action                                                   */
/* ------------------------------------------------------------------------- */

/** Crochet de nid_capture : chaque image capturée (viseur, vidéo, photo). */
static void crochet_image(const nid_capture_frame_t *f, void *arg)
{
    (void)arg;
    s_img_w = f->width;
    s_img_h = f->height;
    banc_ecran_image(f);
}

/** Une image du viseur : l'aperçu de nid_capture la remet au crochet, donc à l'écran. */
static void viseur_image(void)
{
    if (s_viseur_echecs >= VISEUR_ECHECS_MAX) {
        return;     /* suspendu jusqu'à la prochaine action (pas de journal en boucle) */
    }
    if (!s_viseur_demarre) {
        /* Images en pleine taille (incrustation identique à la photo) ; le JPEG produit n'est pas utilisé. */
        if (nid_capture_preview_start(VISEUR_QUALITE, 0) != ESP_OK) {
            s_viseur_echecs++;
            return;
        }
        s_viseur_demarre = true;
        s_viseur_echecs = 0;
    }
    const uint8_t *jpeg = NULL;
    size_t len = 0;
    esp_err_t err = nid_capture_preview_frame(&jpeg, &len, VISEUR_ATTENTE_MS);
    if (err == ESP_OK) {
        nid_capture_preview_release();
        s_viseur_echecs = 0;
        return;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        s_viseur_demarre = false;       /* caméra réinitialisée : aperçu à redemander */
    }
    if (++s_viseur_echecs == VISEUR_ECHECS_MAX) {
        ESP_LOGW(TAG, "viseur : pas d'image de la caméra (%s)", esp_err_to_name(err));
        banc_ecran_message_vue(BANC_ROUGE, "Viseur : pas d'image de la caméra");
    }
}

static void tache_action(void *arg)
{
    (void)arg;
    int64_t prochain_viseur = 0;
    int64_t prochain_entete = 0;
    for (;;) {
        const bool viseur = banc_ecran_ok() && nid_capture_is_ready();
        TickType_t attente = pdMS_TO_TICKS(200);
        if (viseur) {
            const int64_t reste_us = prochain_viseur - esp_timer_get_time();
            attente = (reste_us > 0) ? pdMS_TO_TICKS(reste_us / 1000) : 0;
        }
        demande_t d;
        if (xQueueReceive(s_file, &d, attente) == pdTRUE) {
            if (d.type == ACTION_VIDEO) {
                faire_video(&d);
            } else {
                faire_photo(&d);
            }
            s_viseur_echecs = 0;    /* le viseur reprend (ou réessaie) après chaque action */
            portENTER_CRITICAL(&s_mux);
            s_occupe = false;
            portEXIT_CRITICAL(&s_mux);
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (now >= prochain_entete) {
            prochain_entete = now + ENTETE_PERIODE_US;
            banc_rafraichir_entete();
        }
        if (viseur) {
            prochain_viseur = now + VISEUR_PERIODE_US;
            viseur_image();
        }
    }
}

static esp_err_t demander(action_t type, uint32_t duree_s, const char *origine)
{
    bool libre;
    portENTER_CRITICAL(&s_mux);
    libre = !s_occupe && s_file != NULL;
    if (libre) {
        s_occupe = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!libre) {
        return ESP_ERR_INVALID_STATE;
    }
    demande_t d = {
        .type = type,
        .duree_s = duree_s,
        .t_demande_us = esp_timer_get_time(),
    };
    strlcpy(d.origine, origine, sizeof(d.origine));
    if (xQueueSend(s_file, &d, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_mux);
        s_occupe = false;
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t banc_demander_video(uint32_t duree_s, const char *origine)
{
    if (duree_s < 1 || duree_s > DUREE_MAX_S) {
        return ESP_ERR_INVALID_ARG;
    }
    return demander(ACTION_VIDEO, duree_s, origine);
}

esp_err_t banc_demander_photo(const char *origine)
{
    return demander(ACTION_PHOTO, 0, origine);
}

bool banc_enregistrement_en_cours(void)
{
    return s_enregistre;
}

esp_err_t banc_regler_nuit(bool nuit)
{
    s_reglages.night = nuit;    /* repris à la prochaine initialisation de la caméra */
    return nid_capture_set_night(nuit);
}

/* ------------------------------------------------------------------------- */
/* Bouton BOOT                                                                */
/* ------------------------------------------------------------------------- */

static void appui_bouton(void)
{
    if (s_enregistre) {
        s_arret = true;
        ESP_LOGI(TAG, "bouton : arrêt anticipé demandé");
        return;
    }
    esp_err_t err = banc_demander_video(CONFIG_BANC_DUREE_S, "bouton");
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "bouton : enregistrement de %d s demandé", CONFIG_BANC_DUREE_S);
    } else {
        ESP_LOGW(TAG, "bouton : action déjà en cours, appui ignoré");
    }
}

static void tache_bouton(void *arg)
{
    (void)arg;
    int stable = 1;     /* relâché (actif bas) */
    int compte = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BOUTON_PERIODE_MS));
        const int niveau = gpio_get_level(CONFIG_BANC_BOUTON_GPIO);
        if (niveau == stable) {
            compte = 0;
            continue;
        }
        if (++compte >= BOUTON_STABLE) {
            stable = niveau;
            compte = 0;
            if (stable == 0) {
                appui_bouton();
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Informations (commande « info »)                                           */
/* ------------------------------------------------------------------------- */

void banc_afficher_info(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char heure[24];
    heure_locale(heure, sizeof(heure), "%Y-%m-%d %H:%M:%S");
    if (nid_storage_is_mounted() && !s_enregistre) {
        lire_espace_sd();
    }

    printf("Banc d'essai %s (ESP-IDF %s), puce ESP32-P4 rév. %d.%d\n", app->version, app->idf_ver,
           chip.revision / 100, chip.revision % 100);
    printf("Heure      : %s (fuseau %s)\n", heure, CONFIG_BANC_FUSEAU);
    printf("Capteur    : %s ; dernière image reçue : %ux%u\n", nid_capture_sensor_name(), s_img_w, s_img_h);
    printf("Réglages   : %ux%u, %u i/s, %" PRIu32 " kbit/s, son %s, nuit %s, %d s par vidéo\n", s_reglages.width,
           s_reglages.height, s_reglages.fps, s_reglages.bitrate_kbps, s_reglages.audio ? "oui" : "non",
           s_reglages.night ? "oui" : "non", CONFIG_BANC_DUREE_S);
    if (nid_storage_is_mounted()) {
        printf("Carte SD   : %" PRIu64 " / %" PRIu64 " Mo libres, dossier %s, %d fichier(s) _INCOMPLET au montage\n",
               s_sd_libre / 1000000ULL, s_sd_total / 1000000ULL, nid_storage_base_dir(), s_recuperes);
    } else {
        printf("Carte SD   : non montée\n");
    }
    printf("Mémoire    : interne %u ko libres (plus grand bloc %u ko), PSRAM %u ko libres\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    printf("Écran      : %s\n", banc_ecran_nom());
    printf("État       : %s\n", s_enregistre ? "enregistrement en cours" : (s_occupe ? "action en cours" : "prêt"));
    printf("Dernier    : %s\n", s_bilan);
}

/* ------------------------------------------------------------------------- */
/* Démarrage                                                                  */
/* ------------------------------------------------------------------------- */

void app_main(void)
{
    /* Sorties au repos (LED IR, ampli, C6 en reset...), comme le firmware. */
    nid_board_init_early();

    setenv("TZ", CONFIG_BANC_FUSEAU, 1);
    tzset();
    regler_heure_compilation();

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "Banc d'essai Suivi nid %s", app->version);

    s_reglages = (nid_capture_settings_t) {
        .width = CONFIG_BANC_LARGEUR,
        .height = CONFIG_BANC_HAUTEUR,
        .fps = CONFIG_BANC_IPS,
        .bitrate_kbps = CONFIG_BANC_DEBIT_KBPS,
#if CONFIG_BANC_SON
        .audio = true,
#endif
        .night = false,
        .night_mono = true,
        .jpeg_quality = CONFIG_BANC_QUALITE_JPEG,
        .label = ETIQUETTE,
    };

    if (banc_ecran_init() == ESP_OK) {
        banc_ecran_ligne(BANC_LIGNE_ENTETE, BANC_GRIS, "Banc d'essai Suivi nid %s — démarrage...", app->version);
    }

    /* Bouton BOOT (actif bas, rappel sur la carte ; tirage interne en plus). */
    const gpio_config_t bouton = {
        .pin_bit_mask = 1ULL << CONFIG_BANC_BOUTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&bouton) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d du bouton invalide (menu « Banc d'essai ») : console seule", CONFIG_BANC_BOUTON_GPIO);
    }

    esp_err_t err = nid_board_i2c_init();
    if (err != ESP_OK) {
        afficher_erreur("bus I2C", err, "Broches SDA/SCL : menu « Suivi nid — Carte et brochage »");
    }

    bool pret = true;
    err = preparer_sd();
    if (err != ESP_OK) {
        afficher_erreur("carte SD", err, piste_sd(err));
        pret = false;
    } else {
        afficher_sd(BANC_LIGNE_DETAIL1);
    }

    banc_ecran_message_vue(BANC_GRIS, "Initialisation de la caméra...");
    nid_capture_set_frame_hook(crochet_image, NULL);
    if (nid_board_i2c_bus() != NULL) {
        err = preparer_camera();
        if (err != ESP_OK) {
            afficher_erreur("caméra", err, piste_camera(err));
            pret = false;
        } else if (pret) {
            banc_ecran_ligne(BANC_LIGNE_DETAIL2, BANC_BLANC, "Caméra : %s prête", nid_capture_sensor_name());
        }
    }

    char sd[24];
    texte_sd(sd, sizeof(sd));
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    nid_storage_journal("demarrage_banc", "banc %s;puce rév. %d.%d;capteur %s;%ux%u %u i/s %" PRIu32 " kbit/s son %s;SD %s",
                        app->version, chip.revision / 100, chip.revision % 100, nid_capture_sensor_name(),
                        s_reglages.width, s_reglages.height, s_reglages.fps, s_reglages.bitrate_kbps,
                        s_reglages.audio ? "oui" : "non", sd);

    s_file = xQueueCreate(1, sizeof(demande_t));
    if (s_file == NULL ||
        xTaskCreate(tache_action, "banc_action", ACTION_PILE, NULL, ACTION_PRIO, NULL) != pdPASS ||
        xTaskCreate(tache_bouton, "banc_bouton", BOUTON_PILE, NULL, BOUTON_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "création des tâches impossible (mémoire)");
        banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_ROUGE, "%s", "ERREUR : création des tâches impossible (mémoire)");
        return;
    }
    if (pret) {
        afficher_pret();
    }
    banc_rafraichir_entete();

    err = banc_console_demarrer();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "console série indisponible : %s (le bouton BOOT reste utilisable)", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "prêt : bouton BOOT (GPIO%d) ou commande « video » ; « help » pour la liste des commandes",
             CONFIG_BANC_BOUTON_GPIO);
}
