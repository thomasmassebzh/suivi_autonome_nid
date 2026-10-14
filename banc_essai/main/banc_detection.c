/*
 * banc_detection — surveillance PIR du banc d'essai, avec la logique du firmware
 *
 * Reprend nid_app.c (mode terrain), sans la veille profonde :
 *  - réglages : valeurs par défaut de nid_config (durée de séquence 30 s, délai
 *    60 s, chauffe 60 s, sensibilité 3, photo de suivi toutes les 15 min),
 *    remplacées par celles du menu « Banc d'essai » si elles y sont fixées,
 *    modifiables à chaud par la console (« regle cle valeur », même syntaxe
 *    que config.ini) ;
 *  - chauffe du PIR : aucun déclenchement pendant chauffe_pir_s après la mise
 *    sous tension (le PIR est alimenté depuis le démarrage de la carte) ;
 *  - front du PIR (interruption de nid_detect) : enregistrement lancé tout de
 *    suite ; en parallèle, validation selon la sensibilité
 *    (nid_detect_pir_confirm : durée minimale d'activité). Refusée : la vidéo
 *    est arrêtée et supprimée (« fausse détection »), comme le firmware qui
 *    n'enregistre pas ;
 *  - délai entre déclenchements compté depuis la FIN de la séquence ;
 *  - délai désactivé (0) : séquences enchaînées tant que le PIR reste actif
 *    (au plus CONFIG_BANC_PIR_ENCHAINEES_MAX, puis pause de 60 s) ;
 *  - photo de suivi périodique tant que la surveillance est armée.
 * Les fronts reçus pendant la chauffe, le délai, un enregistrement ou une
 * autre action sont comptés comme « ignorés » (motif affiché).
 *
 * La latence affichée est mesurée depuis le front montant du PIR (horodaté
 * dans l'interruption) jusqu'à la première image encodée de la séquence. Elle
 * comprend ici l'arrêt du viseur et la reconfiguration de la caméra ; sur le
 * terrain s'y ajoute le réveil et l'initialisation de la caméra.
 */
#include "banc.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "nid_board.h"
#include "nid_detect.h"
#include "nid_storage.h"

static const char *TAG = "banc_pir";

#define DETECT_PILE             4096
#define DETECT_PRIO             6               /* au-dessus de la tâche d'action : validation en parallèle */
#define PIR_CONFIRM_TIMEOUT_MS  1500            /* comme nid_app.c */
#define PIR_STUCK_PAUSE_S       60
#define VALIDATION_ATTENTE_MS   (PIR_CONFIRM_TIMEOUT_MS + 500)

#ifdef CONFIG_BANC_ARME_DEMARRAGE
#define ARME_DEMARRAGE          true
#else
#define ARME_DEMARRAGE          false
#endif

typedef enum {
    VALIDATION_EN_COURS = 0,
    VALIDATION_OK,
    VALIDATION_REFUSEE,
} validation_t;

static nid_config_t s_cfg;
static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_evt;
static volatile validation_t s_validation = VALIDATION_OK;
static volatile uint32_t s_min_actif_ms;
static int64_t s_suivi_prochaine_us;

/* Clés de config.ini modifiables à chaud sur le banc. */
static const char *const s_cles[] = {
    "duree_sequence_s", "delai_entre_declenchements_s", "chauffe_pir_s", "sensibilite", "intervalle_suivi_min",
    "mode", "latitude", "longitude", "avance_crepuscule_min", "retard_aube_min", "puissance_ir", "nuit_monochrome",
    NULL,
};

/* ------------------------------------------------------------------------- */
/* Réglages                                                                   */
/* ------------------------------------------------------------------------- */

void banc_reglages(nid_config_t *out)
{
    portENTER_CRITICAL(&s_cfg_mux);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_cfg_mux);
}

const char *const *banc_cles_reglables(void)
{
    return s_cles;
}

static int64_t chauffe_fin_us(const nid_config_t *cfg)
{
    return (int64_t)cfg->pir_warmup_s * 1000000LL;  /* le PIR est alimenté depuis le démarrage */
}

esp_err_t banc_regler(const char *cle, const char *valeur)
{
    bool connue = false;
    for (size_t i = 0; s_cles[i] != NULL; i++) {
        if (strcasecmp(cle, s_cles[i]) == 0) {
            connue = true;
            break;
        }
    }
    if (!connue) {
        return ESP_ERR_NOT_FOUND;
    }
    nid_config_t cfg;
    banc_reglages(&cfg);
    esp_err_t err = nid_config_set(&cfg, cle, valeur);  /* syntaxe et plages de config.ini */
    if (err != ESP_OK) {
        return err;
    }
    portENTER_CRITICAL(&s_cfg_mux);
    s_cfg = cfg;
    portEXIT_CRITICAL(&s_cfg_mux);

    banc_etat_t *e = banc_etat_verrouiller();
    e->chauffe_fin_us = chauffe_fin_us(&cfg);
    banc_etat_deverrouiller();
    if (strcasecmp(cle, "intervalle_suivi_min") == 0) {
        banc_suivi_planifier();
    }
    banc_nuit_actualiser(true);     /* mode, coordonnées, décalages, puissance IR, monochrome */
    nid_storage_journal("reglage_banc", "%s = %s", cle, valeur);
    ESP_LOGI(TAG, "réglage %s = %s", cle, valeur);
    return ESP_OK;
}

/** Valeurs du menu « Banc d'essai » (-1 : valeur par défaut du firmware). */
static void appliquer_menu(nid_config_t *cfg)
{
#if CONFIG_BANC_DET_DUREE_S >= 0
    cfg->seq_duration_s = CONFIG_BANC_DET_DUREE_S;
#endif
#if CONFIG_BANC_DET_DELAI_S >= 0
    cfg->trigger_delay_s = CONFIG_BANC_DET_DELAI_S;
#endif
#if CONFIG_BANC_DET_CHAUFFE_S >= 0
    cfg->pir_warmup_s = CONFIG_BANC_DET_CHAUFFE_S;
#endif
#if CONFIG_BANC_DET_SENSIBILITE > 0
    cfg->pir_sensitivity = CONFIG_BANC_DET_SENSIBILITE;
#endif
#if CONFIG_BANC_SUIVI_MIN >= 0
    cfg->photo_interval_min = CONFIG_BANC_SUIVI_MIN;
#endif
    if (nid_config_validate(cfg) != ESP_OK) {
        ESP_LOGW(TAG, "valeurs du menu « Banc d'essai » corrigées (voir le journal nid_config)");
    }
}

/* ------------------------------------------------------------------------- */
/* Armement, compteurs, textes                                                */
/* ------------------------------------------------------------------------- */

void banc_armer(bool arme, const char *origine)
{
    banc_etat_t *e = banc_etat_verrouiller();
    const bool avant = e->arme;
    e->arme = arme;
    if (arme) {
        e->delai_fin_us = 0;    /* comme start_monitoring() : délai en cours annulé */
    }
    const int64_t chauffe = e->chauffe_fin_us;
    banc_etat_deverrouiller();
    banc_suivi_planifier();
    if (avant == arme) {
        return;
    }
    const int64_t reste_s = (chauffe - esp_timer_get_time()) / 1000000LL;
    ESP_LOGI(TAG, "surveillance %s (%s)%s", arme ? "ARMÉE" : "DÉSARMÉE", origine,
             (arme && reste_s > 0) ? " : chauffe du PIR en cours" : "");
    nid_storage_journal("surveillance_banc", "%s (%s)", arme ? "armée" : "désarmée", origine);
    if (arme) {
        banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_VERT, "Surveillance ARMÉE (%s) : un front du PIR lance une séquence", origine);
    } else {
        banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_GRIS, "Surveillance DÉSARMÉE (%s) : BOOT court ou ARMER pour réarmer",
                         origine);
    }
}

void banc_compteurs_raz(void)
{
    banc_etat_t *e = banc_etat_verrouiller();
    e->fronts = 0;
    e->detections = 0;
    e->sequences = 0;
    e->fausses = 0;
    e->ignorees = 0;
    e->ignoree_motif[0] = '\0';
    e->latence_ms = -1;
    banc_etat_deverrouiller();
}

void banc_texte_declenchement(const banc_etat_t *e, int64_t maintenant_us, char *out, size_t len,
                              banc_couleur_t *fond)
{
    if (!e->arme) {
        snprintf(out, len, "OFF : désarmé");
        *fond = BANC_GRIS_FONCE;
    } else if (e->rec) {
        snprintf(out, len, "ON : séquence");
        *fond = BANC_VERT_FONCE;
    } else if (maintenant_us < e->chauffe_fin_us) {
        snprintf(out, len, "OFF : chauffe %" PRId64 " s", (e->chauffe_fin_us - maintenant_us + 999999) / 1000000);
        *fond = BANC_ORANGE;
    } else if (e->delai_fin_us != 0 && maintenant_us < e->delai_fin_us) {
        snprintf(out, len, "OFF : délai %" PRId64 " s", (e->delai_fin_us - maintenant_us + 999999) / 1000000);
        *fond = BANC_ORANGE;
    } else {
        snprintf(out, len, "ON : armé");
        *fond = BANC_VERT_FONCE;
    }
}

/* ------------------------------------------------------------------------- */
/* Photos de suivi                                                            */
/* ------------------------------------------------------------------------- */

void banc_suivi_planifier(void)
{
    nid_config_t cfg;
    banc_reglages(&cfg);
    banc_etat_t *e = banc_etat_verrouiller();
    const bool arme = e->arme;
    banc_etat_deverrouiller();
    int64_t prochaine = 0;
    if (arme && cfg.photo_interval_min > 0) {
        prochaine = esp_timer_get_time() + (int64_t)cfg.photo_interval_min * 60LL * 1000000LL;
    }
    s_suivi_prochaine_us = prochaine;
    e = banc_etat_verrouiller();
    e->suivi_prochaine_us = prochaine;
    banc_etat_deverrouiller();
}

bool banc_suivi_du(void)
{
    const int64_t p = s_suivi_prochaine_us;
    return p != 0 && esp_timer_get_time() >= p && !banc_enregistrement_en_cours();
}

/* ------------------------------------------------------------------------- */
/* Fronts du PIR                                                              */
/* ------------------------------------------------------------------------- */

static void ignorer(const char *motif)
{
    banc_etat_t *e = banc_etat_verrouiller();
    e->ignorees++;
    strlcpy(e->ignoree_motif, motif, sizeof(e->ignoree_motif));
    banc_etat_deverrouiller();
    ESP_LOGI(TAG, "front PIR ignoré : %s", motif);
}

static void traiter_front(int64_t t_front_us)
{
    const int64_t now = esp_timer_get_time();
    banc_etat_t *e = banc_etat_verrouiller();
    e->fronts++;
    const bool arme = e->arme;
    const bool rec = e->rec;
    const int64_t chauffe = e->chauffe_fin_us;
    const int64_t delai = e->delai_fin_us;
    banc_etat_deverrouiller();

    if (!arme) {
        ignorer("désarmé");
        return;
    }
    if (now < chauffe) {
        ignorer("chauffe");
        return;
    }
    if (rec) {
        ignorer("enregistrement");
        return;
    }
    if (delai != 0 && now < delai) {
        ignorer("délai");
        return;
    }
    nid_config_t cfg;
    banc_reglages(&cfg);
    s_min_actif_ms = nid_detect_min_active_ms(cfg.pir_sensitivity);
    s_validation = VALIDATION_EN_COURS;
    if (banc_demander_a(BANC_ACTION_DETECTION, 0, 0, t_front_us, "PIR") != ESP_OK) {
        s_validation = VALIDATION_OK;
        ignorer("occupé");
        return;
    }
    e = banc_etat_verrouiller();
    e->detections++;
    banc_etat_deverrouiller();
    ESP_LOGI(TAG, "front PIR à %" PRId64 " ms : séquence lancée, validation (actif %" PRIu32 " ms au moins)",
             t_front_us / 1000, s_min_actif_ms);

    /* Validation en parallèle du démarrage de l'enregistrement (comme prep_task du firmware). */
    const bool ok = nid_detect_pir_confirm(s_min_actif_ms, PIR_CONFIRM_TIMEOUT_MS);
    s_validation = ok ? VALIDATION_OK : VALIDATION_REFUSEE;
    if (!ok) {
        ESP_LOGW(TAG, "front PIR non validé (actif moins de %" PRIu32 " ms) : fausse détection", s_min_actif_ms);
    }
}

static void tache_detect(void *arg)
{
    (void)arg;
    for (;;) {
        nid_detect_evt_t evt;
        if (xQueueReceive(s_evt, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (evt == NID_DETECT_EVT_BUTTON) {
            ESP_LOGI(TAG, "bouton de mise en service du firmware (GPIO%d) : sans effet sur le banc",
                     (int)nid_board_pins()->button);
            continue;
        }
        int64_t t = nid_detect_last_pir_edge_us();
        if (t == 0) {
            t = esp_timer_get_time();
        }
        traiter_front(t);
    }
}

/* ------------------------------------------------------------------------- */
/* Séquence (tâche d'action)                                                  */
/* ------------------------------------------------------------------------- */

/** Rappel pendant l'enregistrement : arrêt dès que la validation est refusée. */
static bool rappel_validation(void *arg)
{
    (void)arg;
    return s_validation == VALIDATION_REFUSEE;
}

/** Avant la finalisation : attend la fin de la validation (séquence très courte). */
static bool annuler_si_refusee(void *arg)
{
    (void)arg;
    for (int i = 0; i < VALIDATION_ATTENTE_MS / 10 && s_validation == VALIDATION_EN_COURS; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return s_validation == VALIDATION_REFUSEE;
}

void banc_detection_sequence(const banc_demande_t *d)
{
    nid_config_t cfg;
    banc_reglages(&cfg);
    uint32_t enchainees = 0;
    banc_demande_t di = *d;
    banc_bilan_video_t b;
    do {
        const bool premiere = (enchainees == 0);
        banc_video_enregistrer(&di, cfg.seq_duration_s, premiere ? "PIR" : "PIR enchaînée", rappel_validation,
                               annuler_si_refusee, NULL, &b);
        if (b.annule) {
            banc_etat_t *e = banc_etat_verrouiller();
            e->fausses++;
            banc_etat_deverrouiller();
            nid_storage_journal("fausse_detection", "PIR actif moins de %" PRIu32 " ms (sensibilité %u)",
                                s_min_actif_ms, cfg.pir_sensitivity);
            banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_JAUNE, "Fausse détection : PIR actif moins de %" PRIu32
                             " ms (sensibilité %u) — vidéo supprimée", s_min_actif_ms, cfg.pir_sensitivity);
            break;
        }
        enchainees++;
        if (b.err == ESP_OK) {
            banc_etat_t *e = banc_etat_verrouiller();
            e->sequences++;
            if (premiere) {
                e->latence_ms = b.premiere_ms;
            }
            banc_etat_deverrouiller();
            nid_storage_journal("detection_banc", "%s;%" PRIu32 " ms;%" PRIu32 " images;%" PRIu32
                                " perdues;front PIR → 1re image %" PRId32 " ms",
                                b.nom, b.st.duration_ms, b.st.video_frames, b.st.dropped_frames,
                                premiere ? b.premiere_ms : -1);
            if (premiere) {
                ESP_LOGI(TAG, "LATENCE front_pir_premiere_image=%" PRId32 "ms fichier=%s", b.premiere_ms, b.nom);
            }
        }
        if (cfg.trigger_delay_s > 0) {
            /* FP1.4 : délai minimal entre deux déclenchements, compté depuis la fin de la séquence. */
            banc_etat_t *e = banc_etat_verrouiller();
            e->delai_fin_us = esp_timer_get_time() + (int64_t)cfg.trigger_delay_s * 1000000LL;
            banc_etat_deverrouiller();
            break;
        }
        di.t_demande_us = esp_timer_get_time();
        banc_etat_t e;
        banc_etat_lire(&e);
        if (!e.arme) {
            break;
        }
        /* Délai désactivé : on enchaîne tant que l'animal est détecté. */
    } while (b.err == ESP_OK && nid_detect_pir_active() && !banc_arret_demande() &&
             enchainees < CONFIG_BANC_PIR_ENCHAINEES_MAX);

    if (cfg.trigger_delay_s == 0 && enchainees >= CONFIG_BANC_PIR_ENCHAINEES_MAX && nid_detect_pir_active()) {
        nid_storage_journal("alerte", "PIR toujours actif après %" PRIu32 " séquences : pause de %d s", enchainees,
                            PIR_STUCK_PAUSE_S);
        banc_etat_t *e = banc_etat_verrouiller();
        e->delai_fin_us = esp_timer_get_time() + PIR_STUCK_PAUSE_S * 1000000LL;
        banc_etat_deverrouiller();
    }
    s_validation = VALIDATION_OK;
}

/* ------------------------------------------------------------------------- */
/* Initialisation                                                             */
/* ------------------------------------------------------------------------- */

esp_err_t banc_detection_init(void)
{
    nid_config_t cfg;
    nid_config_set_defaults(&cfg);
    appliquer_menu(&cfg);
    portENTER_CRITICAL(&s_cfg_mux);
    s_cfg = cfg;
    portEXIT_CRITICAL(&s_cfg_mux);

    banc_etat_t *e = banc_etat_verrouiller();
    e->arme = ARME_DEMARRAGE;
    e->chauffe_fin_us = chauffe_fin_us(&cfg);
    e->delai_fin_us = 0;
    banc_etat_deverrouiller();
    banc_suivi_planifier();
    ESP_LOGI(TAG, "surveillance %s : séquence %u s, délai %u s, chauffe %u s, sensibilité %u (%" PRIu32
             " ms), photo de suivi %u min", ARME_DEMARRAGE ? "armée" : "désarmée", cfg.seq_duration_s,
             cfg.trigger_delay_s, cfg.pir_warmup_s, cfg.pir_sensitivity,
             nid_detect_min_active_ms(cfg.pir_sensitivity), cfg.photo_interval_min);

    esp_err_t err = nid_detect_init();
    if (err != ESP_OK) {
        return err;
    }
    if (nid_board_pins()->pir == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "aucun PIR configuré (menu « Suivi nid — Carte et brochage »)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_evt = xQueueCreate(8, sizeof(nid_detect_evt_t));
    if (s_evt == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = nid_detect_enable_events(s_evt);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(tache_detect, "banc_detect", DETECT_PILE, NULL, DETECT_PRIO, NULL) != pdPASS) {
        nid_detect_disable_events();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "PIR sur GPIO%d (actif %s), niveau actuel : %s", (int)nid_board_pins()->pir,
             nid_board_pins()->pir_active_high ? "haut" : "bas", nid_detect_pir_active() ? "ACTIF" : "repos");
    return ESP_OK;
}
