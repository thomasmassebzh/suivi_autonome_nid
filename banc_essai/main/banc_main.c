/*
 * Banc d'essai « Suivi nid » — validation pas à pas sur l'ESP32-P4X-Function-EV-Board
 *
 * Le banc compile les modules du firmware de terrain (nid_board, nid_capture,
 * nid_storage, nid_config, nid_detect, nid_ir, nid_power, nid_clock) sans les
 * copier, et les essaie une fonction après l'autre avec l'écran 7 pouces comme
 * tableau de bord (voir README.md).
 *
 * Tâches :
 *  - « banc_action » : seule tâche qui utilise la caméra et le micro : vidéos
 *    (manuelles, sur détection PIR, endurance), photos (manuelles, de suivi),
 *    tests de la carte SD, vumètre, viseur (aperçu de nid_capture, images
 *    remises à l'écran par le crochet de débogage) entre deux actions ;
 *  - « banc_detect » (banc_detection.c) : fronts du PIR, validation selon la
 *    sensibilité, en parallèle du démarrage de l'enregistrement ;
 *  - « banc_bouton » : bouton BOOT (appui court : armer / désarmer ; appui long :
 *    vidéo ; appui pendant un enregistrement : arrêt anticipé) ;
 *  - « banc_ui » (banc_ui.c) : panneau d'état, onglets, boutons tactiles ;
 *  - console série (esp_console), voir banc_console.c.
 *
 * Aucune erreur ne redémarre la carte : elle est affichée (étape, code
 * d'erreur, piste de dépannage) et le banc reste utilisable : la prochaine
 * action réessaie le montage de la carte SD ou l'initialisation de la caméra.
 */
#include "banc.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define ACTION_PILE             (12 * 1024)
#define ACTION_PRIO             5
#define BOUTON_PILE             3072
#define BOUTON_PRIO             4
#define BOUTON_PERIODE_MS       10
#define BOUTON_STABLE           3               /* 3 lectures identiques : 30 ms d'anti-rebond */
#define BOUTON_LONG_MS          1000            /* appui long : vidéo manuelle */

#define VISEUR_PERIODE_US       (1000000LL / 12)  /* viseur : 12 images par seconde au plus */
#define VISEUR_QUALITE          30              /* qualité JPEG de l'aperçu (non utilisé par l'écran) */
#define VISEUR_ATTENTE_MS       300
#define VISEUR_ECHECS_MAX       10
#define NUIT_AUTO_PERIODE_US    (60LL * 1000000LL)

#define HEURE_VALIDE_MIN        1704067200      /* 2024-01-01 : avant, l'heure n'est pas réglée */

static QueueHandle_t s_file;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_occupe;                       /* action demandée ou en cours (sous s_mux) */
static volatile bool s_arret;               /* arrêt anticipé demandé */

static banc_etat_t s_etat = {
    .latence_ms = -1,
    .filtre_jour = -1,
    .dernier_fichier = "aucun",
    .photo_fichier = "aucune",
};
static portMUX_TYPE s_etat_mux = portMUX_INITIALIZER_UNLOCKED;

static nid_capture_settings_t s_reglages;
static nid_storage_volume_t s_volume;       /* volume de la carte (mis à jour après chaque action, sous s_mux) */
static int s_recuperes = -1;                /* fichiers renommés _INCOMPLET au dernier montage */

/* Viseur */
static bool s_viseur_demarre;
static int s_viseur_echecs;

/* Copie du bandeau (page web, même sans écran), sous s_bandeau_mux */
#define BANDEAU_TEXTE   160
static char s_bandeau[BANC_NB_LIGNES][BANDEAU_TEXTE];
static banc_couleur_t s_bandeau_couleur[BANC_NB_LIGNES];
static portMUX_TYPE s_bandeau_mux = portMUX_INITIALIZER_UNLOCKED;

/* ------------------------------------------------------------------------- */
/* État partagé                                                               */
/* ------------------------------------------------------------------------- */

void banc_etat_lire(banc_etat_t *copie)
{
    portENTER_CRITICAL(&s_etat_mux);
    *copie = s_etat;
    portEXIT_CRITICAL(&s_etat_mux);
}

banc_etat_t *banc_etat_verrouiller(void)
{
    portENTER_CRITICAL(&s_etat_mux);
    return &s_etat;
}

void banc_etat_deverrouiller(void)
{
    portEXIT_CRITICAL(&s_etat_mux);
}

const nid_capture_settings_t *banc_reglages_capture(void)
{
    return &s_reglages;
}

void banc_bandeau_noter(banc_ligne_t ligne, banc_couleur_t couleur, const char *texte)
{
    if (ligne >= BANC_NB_LIGNES) {
        return;
    }
    portENTER_CRITICAL(&s_bandeau_mux);
    strlcpy(s_bandeau[ligne], texte, BANDEAU_TEXTE);
    s_bandeau_couleur[ligne] = couleur;
    portEXIT_CRITICAL(&s_bandeau_mux);
}

void banc_bandeau_lire(banc_ligne_t ligne, char *out, size_t len, banc_couleur_t *couleur)
{
    if (ligne >= BANC_NB_LIGNES || len == 0) {
        return;
    }
    char t[BANDEAU_TEXTE];
    portENTER_CRITICAL(&s_bandeau_mux);
    memcpy(t, s_bandeau[ligne], sizeof(t));
    *couleur = s_bandeau_couleur[ligne];
    portEXIT_CRITICAL(&s_bandeau_mux);
    strlcpy(out, t, len);
}

bool banc_vumetre_demande(void)
{
    return banc_ui_onglet() == BANC_ONGLET_SON || banc_web_son_demande();
}

/* ------------------------------------------------------------------------- */
/* Outils                                                                     */
/* ------------------------------------------------------------------------- */

void banc_heure_locale(char *out, size_t len, const char *fmt)
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

esp_err_t banc_regler_heure(time_t t, const char *origine)
{
    if (t < (time_t)HEURE_VALIDE_MIN || t >= (time_t)4102444800LL) {     /* 2024-01-01 .. 2100-01-01 */
        return ESP_ERR_INVALID_ARG;
    }
    const time_t avant = time(NULL);
    const struct timeval tv = { .tv_sec = t };
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }
    char txt[24];
    banc_heure_locale(txt, sizeof(txt), "%Y-%m-%d %H:%M:%S");
    ESP_LOGI(TAG, "heure réglée (%s) : %s, écart %+lld s", origine, txt, (long long)(t - avant));
    nid_storage_journal("heure_reglee", "%s (%s, écart %+lld s)", txt, origine, (long long)(t - avant));
    banc_rafraichir_entete();
    banc_nuit_actualiser(false);
    return ESP_OK;
}

void banc_lire_espace_sd(void)
{
    nid_storage_volume_t v;
    if (nid_storage_get_volume(&v, false) != ESP_OK) {
        memset(&v, 0, sizeof(v));
    }
    portENTER_CRITICAL(&s_mux);
    s_volume = v;
    portEXIT_CRITICAL(&s_mux);
}

void banc_volume_sd(nid_storage_volume_t *v)
{
    portENTER_CRITICAL(&s_mux);
    *v = s_volume;
    portEXIT_CRITICAL(&s_mux);
}

void banc_espace_sd(uint64_t *total, uint64_t *libre)
{
    portENTER_CRITICAL(&s_mux);
    *total = s_volume.volume_bytes;
    *libre = s_volume.free_bytes;
    portEXIT_CRITICAL(&s_mux);
}

void banc_texte_sd(char *out, size_t len)
{
    if (!nid_storage_is_mounted()) {
        strlcpy(out, "absente", len);
        return;
    }
    uint64_t total, libre;
    banc_espace_sd(&total, &libre);
    const uint64_t dixiemes = libre / 100000000ULL;    /* dixièmes de Go (1 Go = 10^9 o) */
    snprintf(out, len, "%" PRIu64 ".%" PRIu64 " Go libres", dixiemes / 10, dixiemes % 10);
}

void banc_rafraichir_entete(void)
{
    char sd[24];
    char heure[16];
    banc_texte_sd(sd, sizeof(sd));
    banc_heure_locale(heure, sizeof(heure), "%H:%M:%S");
    banc_ecran_ligne(BANC_LIGNE_ENTETE, BANC_GRIS, "%s | %ux%u %u i/s %" PRIu32 " kbit/s%s | SD %s | %s",
                     nid_capture_sensor_name(), s_reglages.width, s_reglages.height, s_reglages.fps,
                     s_reglages.bitrate_kbps, s_reglages.audio ? " son" : "", sd, heure);
}

void banc_effacer_details(void)
{
    banc_ecran_ligne(BANC_LIGNE_DETAIL1, BANC_BLANC, "%s", "");
    banc_ecran_ligne(BANC_LIGNE_DETAIL2, BANC_BLANC, "%s", "");
    banc_ecran_ligne(BANC_LIGNE_DETAIL3, BANC_BLANC, "%s", "");
}

static void afficher_pret(void)
{
    banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_VERT, "%s",
                     "Prêt — BOOT court : armer/désarmer, BOOT long : vidéo ; « help » sur la console");
}

void banc_afficher_erreur(const char *etape, esp_err_t err, const char *piste)
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
    banc_ecran_ligne(BANC_LIGNE_DETAIL3, BANC_GRIS, "%s", "Corriger, puis recommencer (BOOT, console ou écran)");
}

static const char *piste_sd(esp_err_t err)
{
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return "Fonction « carte SD » désactivée\n(écran de démarrage, « fonctions »)";
    }
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

/* ------------------------------------------------------------------------- */
/* Carte SD et caméra                                                         */
/* ------------------------------------------------------------------------- */

/**
 * Monte la carte, pose les identifiants puis renomme en « _INCOMPLET » les
 * fichiers interrompus (coupure pendant une écriture), comme le firmware.
 */
esp_err_t banc_preparer_sd(void)
{
    if (nid_storage_is_mounted()) {
        return ESP_OK;
    }
    if (!banc_f_choisie(BANC_F_SD)) {
        return ESP_ERR_NOT_SUPPORTED;   /* fonction désactivée : la carte n'est jamais montée */
    }
    esp_err_t err = nid_storage_mount();
    if (err == ESP_OK) {
        err = nid_storage_set_ids(BANC_ID_SYSTEME, BANC_ID_NID);
    }
    if (err != ESP_OK) {
        return err;
    }
    s_recuperes = nid_storage_recover();
    banc_lire_espace_sd();
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
    banc_texte_sd(sd, sizeof(sd));
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
    banc_etat_t *e = banc_etat_verrouiller();
    e->son_ok = (err == ESP_OK) && s_reglages.audio;
    banc_etat_deverrouiller();
    return err;
}

bool banc_preparer(void)
{
    /* Fonction désactivée : refus ; en défaut (échec au démarrage) : nouvel essai. */
    if (!banc_f_choisie(BANC_F_SD) || !banc_f_choisie(BANC_F_CAMERA)) {
        const bool sd = !banc_f_choisie(BANC_F_SD);
        banc_afficher_erreur(sd ? "carte SD" : "caméra", ESP_ERR_NOT_SUPPORTED,
                             sd ? "Fonction « carte SD » désactivée\n(écran de démarrage, « fonctions »)"
                                : "Fonction « caméra » désactivée\n(écran de démarrage, « fonctions »)");
        return false;
    }
    esp_err_t err = banc_preparer_sd();
    if (err != ESP_OK) {
        banc_afficher_erreur("carte SD", err, piste_sd(err));
        return false;
    }
    banc_f_retablir(BANC_F_SD);
    err = preparer_camera();
    if (err != ESP_OK) {
        banc_afficher_erreur("caméra", err, piste_camera(err));
        return false;
    }
    banc_f_retablir(BANC_F_CAMERA);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Viseur et tâche d'action                                                   */
/* ------------------------------------------------------------------------- */

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

static void executer(const banc_demande_t *d)
{
    switch (d->type) {
    case BANC_ACTION_VIDEO:
    case BANC_ACTION_SON_VIDEO: {
        banc_bilan_video_t b;
        banc_video_enregistrer(d, d->param, d->type == BANC_ACTION_SON_VIDEO ? "test son" : "manuel", NULL, NULL,
                               NULL, &b);
        break;
    }
    case BANC_ACTION_PHOTO:
        banc_photo_prendre(d, false);
        break;
    case BANC_ACTION_PHOTO_SUIVI:
        banc_photo_prendre(d, true);
        break;
    case BANC_ACTION_DETECTION:
        banc_detection_sequence(d);
        break;
    case BANC_ACTION_SD_TEST:
        banc_sd_test(d);
        break;
    case BANC_ACTION_SD_SCAN:
        banc_sd_scan();
        break;
    case BANC_ACTION_BOUCLE:
        banc_boucle(d);
        break;
    }
}

static void tache_action(void *arg)
{
    (void)arg;
    int64_t prochain_viseur = 0;
    int64_t prochain_nuit = 0;
    for (;;) {
        /* Viseur : pour l'écran, ou pour la vue de la page web (banc sans écran). */
        const bool viseur = (banc_ecran_ok() || banc_web_vue_active()) && nid_capture_is_ready();
        TickType_t attente = pdMS_TO_TICKS(200);
        if (viseur) {
            const int64_t reste_us = prochain_viseur - esp_timer_get_time();
            attente = (reste_us > 0) ? pdMS_TO_TICKS(reste_us / 1000) : 0;
        }
        banc_demande_t d;
        if (xQueueReceive(s_file, &d, attente) == pdTRUE) {
            s_arret = false;
            executer(&d);
            s_arret = false;
            s_viseur_echecs = 0;    /* le viseur reprend (ou réessaie) après chaque action */
            portENTER_CRITICAL(&s_mux);
            s_occupe = false;
            portEXIT_CRITICAL(&s_mux);
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (now >= prochain_nuit) {
            prochain_nuit = now + NUIT_AUTO_PERIODE_US;
            banc_nuit_actualiser(false);
        }
        /* Photo de suivi due : prise comme une action (le bouton et la console attendent). */
        if (banc_suivi_du() && banc_demander(BANC_ACTION_PHOTO_SUIVI, 0, 0, "suivi") == ESP_OK) {
            continue;
        }
        /* Onglet « Son » ou section Son de la page web : vumètre (~200 ms par mesure, le viseur ralentit). */
        if (banc_vumetre_demande() && nid_capture_is_ready() && banc_f_active(BANC_F_SON)) {
            banc_son_mesurer();
        }
        if (viseur) {
            prochain_viseur = esp_timer_get_time() + VISEUR_PERIODE_US;
            viseur_image();
        }
    }
}

esp_err_t banc_demander_a(banc_action_t type, uint32_t param, uint32_t param2, int64_t t_ref_us, const char *origine)
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
    banc_demande_t d = {
        .type = type,
        .param = param,
        .param2 = param2,
        .t_demande_us = t_ref_us,
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

esp_err_t banc_demander(banc_action_t type, uint32_t param, uint32_t param2, const char *origine)
{
    if ((type == BANC_ACTION_VIDEO || type == BANC_ACTION_SON_VIDEO) && (param < 1 || param > BANC_DUREE_MAX_S)) {
        return ESP_ERR_INVALID_ARG;
    }
    return banc_demander_a(type, param, param2, esp_timer_get_time(), origine);
}

bool banc_occupe(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool o = s_occupe;
    portEXIT_CRITICAL(&s_mux);
    return o;
}

bool banc_enregistrement_en_cours(void)
{
    banc_etat_t *e = banc_etat_verrouiller();
    const bool rec = e->rec;
    banc_etat_deverrouiller();
    return rec;
}

void banc_arreter(const char *origine)
{
    s_arret = true;
    ESP_LOGI(TAG, "%s : arrêt anticipé demandé", origine);
}

bool banc_arret_demande(void)
{
    return s_arret;
}

/* ------------------------------------------------------------------------- */
/* Bouton BOOT                                                                */
/* ------------------------------------------------------------------------- */

static void appui_court(void)
{
    banc_etat_t e;
    banc_etat_lire(&e);
    banc_armer(!e.arme, "bouton");
}

static void appui_long(void)
{
    esp_err_t err = banc_demander(BANC_ACTION_VIDEO, CONFIG_BANC_DUREE_S, 0, "bouton");
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "bouton (appui long) : vidéo de %d s demandée", CONFIG_BANC_DUREE_S);
    } else {
        ESP_LOGW(TAG, "bouton : action déjà en cours, appui ignoré");
    }
}

/**
 * Appui pendant un enregistrement (ou une boucle d'endurance) : arrêt anticipé.
 * Sinon : appui court (relâché avant 1 s) → armer / désarmer ; appui long → vidéo.
 */
static void tache_bouton(void *arg)
{
    (void)arg;
    int stable = 1;     /* relâché (actif bas) */
    int compte = 0;
    int64_t t_appui = 0;
    bool traite = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BOUTON_PERIODE_MS));
        const int niveau = gpio_get_level(CONFIG_BANC_BOUTON_GPIO);
        if (stable == 0 && !traite && esp_timer_get_time() - t_appui >= BOUTON_LONG_MS * 1000LL) {
            traite = true;
            appui_long();
        }
        if (niveau == stable) {
            compte = 0;
            continue;
        }
        if (++compte < BOUTON_STABLE) {
            continue;
        }
        stable = niveau;
        compte = 0;
        if (stable == 0) {
            t_appui = esp_timer_get_time();
            traite = false;
            banc_etat_t e;
            banc_etat_lire(&e);
            if (e.rec || e.boucle_en_cours) {
                traite = true;
                banc_arreter("bouton");
            }
        } else if (!traite) {
            appui_court();
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
    banc_heure_locale(heure, sizeof(heure), "%Y-%m-%d %H:%M:%S");
    banc_etat_t e;
    banc_etat_lire(&e);
    if (nid_storage_is_mounted() && !e.rec) {
        banc_lire_espace_sd();
    }
    uint64_t total, libre;
    banc_espace_sd(&total, &libre);
    nid_config_t cfg;
    banc_reglages(&cfg);

    printf("Banc d'essai %s (ESP-IDF %s), puce ESP32-P4 rév. %d.%d\n", app->version, app->idf_ver,
           chip.revision / 100, chip.revision % 100);
    printf("Heure      : %s (fuseau %s)\n", heure, CONFIG_BANC_FUSEAU);
    printf("Capteur    : %s\n", nid_capture_sensor_name());
    printf("Réglages   : %ux%u, %u i/s, %" PRIu32 " kbit/s, son %s, %d s par vidéo manuelle\n", s_reglages.width,
           s_reglages.height, s_reglages.fps, s_reglages.bitrate_kbps, s_reglages.audio ? "oui" : "non",
           CONFIG_BANC_DUREE_S);
    printf("Détection  : %s, séquence %u s, délai %u s, chauffe %u s, sensibilité %u, suivi %u min\n",
           e.arme ? "armée" : "désarmée", cfg.seq_duration_s, cfg.trigger_delay_s, cfg.pir_warmup_s,
           cfg.pir_sensitivity, cfg.photo_interval_min);
    printf("Compteurs  : %" PRIu32 " fronts, %" PRIu32 " détections, %" PRIu32 " séquences, %" PRIu32
           " fausses, %" PRIu32 " ignorées ; latence %" PRId32 " ms\n", e.fronts, e.detections, e.sequences,
           e.fausses, e.ignorees, e.latence_ms);
    if (nid_storage_is_mounted()) {
        printf("Carte SD   : %" PRIu64 " / %" PRIu64 " Mo libres, dossier %s, %d fichier(s) _INCOMPLET au montage\n",
               libre / 1000000ULL, total / 1000000ULL, nid_storage_base_dir(), s_recuperes);
    } else {
        printf("Carte SD   : non montée\n");
    }
    printf("Mémoire    : interne %u ko libres (plus grand bloc %u ko, minimum %u ko), PSRAM %u ko libres\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    printf("Écran      : %s ; tactile : %s ; onglet %s\n", banc_ecran_nom(), banc_tactile_nom(),
           banc_ui_nom_onglet(banc_ui_onglet()));
#if CONFIG_BANC_F_WIFI
    banc_wifi_etat_t w;
    banc_wifi_etat(&w);
    if (w.demarre) {
        printf("Wi-Fi      : %s « %s » %s, adresse %s ; C6 %s (« wifi » pour le détail)\n",
               w.st.mode == NID_WIFI_MODE_AP ? "point d'accès" : "station", w.st.ssid, nid_wifi_state_str(w.st.state),
               w.st.ip[0] ? w.st.ip : "-", w.version_c6);
    } else
#endif
    {
        const char *t = banc_f_texte(BANC_F_WIFI);
        printf("Wi-Fi      : %s\n", t ? t : "arrêté");
    }
    printf("État       : %s\n", e.rec ? "enregistrement en cours" : (banc_occupe() ? "action en cours" : "prêt"));
    printf("Dernier    : %s\n", banc_dernier_bilan());
}

/* ------------------------------------------------------------------------- */
/* Démarrage                                                                  */
/* ------------------------------------------------------------------------- */

/** Bilan des fonctions (bandeau et journal série). */
static void afficher_fonctions(void)
{
    char liste[160] = "";
    int actives = 0;
    for (int f = 0; f < BANC_NB_F; f++) {
        const banc_fonction_etat_t e = banc_f_etat((banc_fonction_t)f);
        if (e == BANC_FE_ACTIVE) {
            actives++;
        } else if (e != BANC_FE_NON_COMPILEE) {
            char t[40];
            snprintf(t, sizeof(t), "%s%s %s", liste[0] ? ", " : "", banc_f_nom((banc_fonction_t)f),
                     banc_f_texte((banc_fonction_t)f));
            strlcat(liste, t, sizeof(liste));
        }
    }
    ESP_LOGI(TAG, "FONCTIONS actives=%d %s", actives, liste);
    banc_ecran_ligne(BANC_LIGNE_DETAIL3, liste[0] ? BANC_JAUNE : BANC_BLANC, "Fonctions : %d active(s)%s%s", actives,
                     liste[0] ? " ; " : "", liste);
}

void app_main(void)
{
    /* Sorties au repos (LED IR, ampli, C6 en reset...), comme le firmware. */
    nid_board_init_early();

    setenv("TZ", CONFIG_BANC_FUSEAU, 1);
    tzset();
    regler_heure_compilation();

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "Banc d'essai Suivi nid %s", app->version);

    /* 1. Fonctions choisies au démarrage précédent (NVS). */
    banc_fonctions_charger();

    /* Bouton BOOT (actif bas, rappel sur la carte ; tirage interne en plus) : sert aussi à la sélection. */
    const gpio_config_t bouton = {
        .pin_bit_mask = 1ULL << CONFIG_BANC_BOUTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&bouton) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d du bouton invalide (menu « Banc d'essai ») : console seule", CONFIG_BANC_BOUTON_GPIO);
    }
    esp_err_t err_i2c = nid_board_i2c_init();
    esp_err_t err;

    /* 2. Écran, tactile et console : ils servent à l'écran de sélection. */
    if (banc_f_choisie(BANC_F_ECRAN)) {
        err = banc_ecran_init();
        if (err != ESP_OK) {
            banc_f_defaut(BANC_F_ECRAN, err);
        }
    }
    if (banc_f_active(BANC_F_TACTILE)) {
        err = banc_tactile_init();
        if (err != ESP_OK) {
            banc_f_defaut(BANC_F_TACTILE, err);
        }
    }
    bool console_lancee = false;
    if (banc_f_choisie(BANC_F_CONSOLE)) {
        err = banc_console_demarrer();
        console_lancee = true;
        if (err != ESP_OK) {
            banc_f_defaut(BANC_F_CONSOLE, err);
        }
    }

    /* 3. Sélection (démarrage automatique sur la dernière sélection, mémorisée en NVS). */
    banc_fonctions_selection();
    if (banc_ecran_ok() && !banc_f_active(BANC_F_ECRAN)) {
        banc_ecran_couper();
    }
    /* Fonctions d'interface réactivées pendant la sélection : initialisées maintenant. */
    if (banc_f_active(BANC_F_ECRAN) && !banc_ecran_ok()) {
        err = banc_ecran_init();
        if (err != ESP_OK) {
            banc_f_defaut(BANC_F_ECRAN, err);
        }
    }
    if (banc_f_active(BANC_F_TACTILE) && !banc_tactile_ok()) {
        err = banc_tactile_init();
        if (err != ESP_OK) {
            banc_f_defaut(BANC_F_TACTILE, err);
        }
    }
    if (banc_f_choisie(BANC_F_CONSOLE) && !console_lancee) {
        err = banc_console_demarrer();
        if (err != ESP_OK) {
            banc_f_defaut(BANC_F_CONSOLE, err);
        }
    }
    if (banc_ecran_ok()) {
        banc_ecran_rect(&banc_ecran_zones()->vue, BANC_NOIR);
        banc_ecran_rect(&banc_ecran_zones()->panneau, BANC_FOND);
        banc_ecran_ligne(BANC_LIGNE_ENTETE, BANC_GRIS, "Banc d'essai Suivi nid %s — démarrage...", app->version);
        banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_JAUNE, "%s", "Initialisation des fonctions choisies...");
        banc_effacer_details();
    }
    if (err_i2c != ESP_OK) {
        banc_afficher_erreur("bus I2C", err_i2c, "Broches SDA/SCL : menu « Suivi nid — Carte et brochage »");
    }

    s_reglages = (nid_capture_settings_t) {
        .width = CONFIG_BANC_LARGEUR,
        .height = CONFIG_BANC_HAUTEUR,
        .fps = CONFIG_BANC_IPS,
        .bitrate_kbps = CONFIG_BANC_DEBIT_KBPS,
        .audio = banc_f_active(BANC_F_SON),
        .night = false,
        .night_mono = true,
        .jpeg_quality = CONFIG_BANC_QUALITE_JPEG,
        .label = BANC_ETIQUETTE,
    };

    /* 4. Chaque fonction choisie est initialisée ; un échec la met « en défaut » sans bloquer les autres. */
    banc_tests_init();      /* nuit (LED IR, IR-cut), batterie, température selon la sélection */
    err = banc_detection_init();    /* réglages ; PIR et interruptions si la fonction est active */
    if (err != ESP_OK && banc_f_active(BANC_F_PIR)) {
        banc_f_defaut(BANC_F_PIR, err);
    }

    bool pret = true;
    if (banc_f_active(BANC_F_SD)) {
        err = banc_preparer_sd();
        if (err != ESP_OK) {
            banc_f_defaut(BANC_F_SD, err);
            banc_afficher_erreur("carte SD", err, piste_sd(err));
            pret = false;
        } else {
            afficher_sd(BANC_LIGNE_DETAIL1);
        }
    }

    nid_capture_set_frame_hook(banc_crochet_image, NULL);
    if (banc_f_active(BANC_F_CAMERA)) {
        banc_ecran_message_vue(BANC_GRIS, "Initialisation de la caméra...");
        err = (nid_board_i2c_bus() != NULL) ? preparer_camera() : ESP_ERR_INVALID_STATE;
        if (err != ESP_OK) {
            banc_f_defaut(BANC_F_CAMERA, err);
            banc_afficher_erreur("caméra", err, piste_camera(err));
            pret = false;
        } else {
            banc_ecran_ligne(BANC_LIGNE_DETAIL2, BANC_BLANC, "Caméra : %s prête", nid_capture_sensor_name());
            nid_capture_audio_level_t niveau;
            if (s_reglages.audio && nid_capture_audio_level(100, &niveau) == ESP_ERR_NOT_SUPPORTED) {
                banc_f_defaut(BANC_F_SON, ESP_ERR_NOT_FOUND);   /* ES8311 absent ou muet : vidéo seule */
            }
        }
    } else {
        banc_ecran_message_vue(BANC_GRIS, "Caméra désactivée");
    }
    banc_nuit_actualiser(true);

    char sd[24];
    banc_texte_sd(sd, sizeof(sd));
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    nid_storage_journal("demarrage_banc", "banc %s;puce rév. %d.%d;capteur %s;%ux%u %u i/s %" PRIu32 " kbit/s son %s;SD %s",
                        app->version, chip.revision / 100, chip.revision % 100, nid_capture_sensor_name(),
                        s_reglages.width, s_reglages.height, s_reglages.fps, s_reglages.bitrate_kbps,
                        banc_f_active(BANC_F_SON) ? "oui" : "non", sd);

    s_file = xQueueCreate(1, sizeof(banc_demande_t));
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
    afficher_fonctions();
    banc_rafraichir_entete();
    if (banc_f_active(BANC_F_ECRAN)) {
        err = banc_ui_demarrer();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "tâche d'affichage : %s", esp_err_to_name(err));
        }
    }
    /* Wi-Fi en dernier (comme le firmware : carte SD montée avant le transport
     * SDIO du C6) ; démarrage en tâche de fond, un échec le met « en défaut ». */
    if (banc_f_active(BANC_F_WIFI)) {
        err = banc_wifi_demarrer();
        if (err != ESP_OK) {
            banc_f_defaut(BANC_F_WIFI, err);
        }
    }
    ESP_LOGI(TAG, "prêt : BOOT court = armer/désarmer, BOOT long = vidéo (GPIO%d) ; « help » pour les commandes",
             CONFIG_BANC_BOUTON_GPIO);
}
