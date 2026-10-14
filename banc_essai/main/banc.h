/*
 * Banc d'essai « Suivi nid » — déclarations communes
 *
 *  - banc_main.c      : démarrage, tâche d'action (seule à utiliser la caméra),
 *                       bouton BOOT, état partagé ;
 *  - banc_video.c     : vidéos (manuelle, sur détection PIR, endurance), photos ;
 *  - banc_detection.c : surveillance PIR (logique du firmware : chauffe, durée,
 *                       délai, enchaînement), réglages, photos de suivi ;
 *  - banc_tests.c     : nuit (LED IR, IR-cut, monochrome, soleil), son, batterie,
 *                       température, carte SD (débit, latence) ;
 *  - banc_ui.c        : onglets de l'écran, panneau d'état, boutons tactiles ;
 *  - banc_ecran.c     : écran MIPI-DSI (dessin direct RGB565, vue caméra par le PPA) ;
 *  - banc_tactile.c   : dalle tactile GT911 ;
 *  - banc_console.c   : commandes de la console série.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "nid_capture.h"
#include "nid_config.h"
#include "nid_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BANC_ID_SYSTEME     "BANC"
#define BANC_ID_NID         "N00"
#define BANC_ETIQUETTE      "BANC N00"      /* incrusté après la date et l'heure */
#define BANC_DUREE_MAX_S    300

/* ========================================================================= */
/* Écran (banc_ecran.c)                                                       */
/* ========================================================================= */

/** Lignes du bandeau d'état, en bas de l'écran. */
typedef enum {
    BANC_LIGNE_ENTETE = 0,      /*!< capteur, réglages, carte SD, heure */
    BANC_LIGNE_ETAT,            /*!< prêt / enregistrement en cours / erreur */
    BANC_LIGNE_DETAIL1,         /*!< résultats, pistes de dépannage */
    BANC_LIGNE_DETAIL2,
    BANC_LIGNE_DETAIL3,
    BANC_NB_LIGNES,
} banc_ligne_t;

typedef enum {
    BANC_BLANC = 0,
    BANC_GRIS,
    BANC_VERT,
    BANC_JAUNE,
    BANC_ROUGE,
    BANC_NOIR,
    BANC_FOND,              /*!< fond du panneau et du bandeau */
    BANC_GRIS_FONCE,        /*!< pastille au repos, bouton */
    BANC_VERT_FONCE,        /*!< pastille « armé » */
    BANC_ROUGE_VIF,         /*!< pastille « PIR actif », « REC » */
    BANC_ORANGE,            /*!< pastille « chauffe », « délai » */
    BANC_BLEU,              /*!< onglet actif, barre du vumètre */
    BANC_NB_COULEURS,
} banc_couleur_t;

/** Rectangle à l'écran (pixels). */
typedef struct {
    uint16_t x, y, w, h;
} banc_rect_t;

/** Zones de l'écran, calculées à l'initialisation d'après sa taille. */
typedef struct {
    banc_rect_t onglets;    /*!< barre des onglets (haut) */
    banc_rect_t vue;        /*!< vue caméra */
    banc_rect_t panneau;    /*!< panneau d'état de l'onglet */
    banc_rect_t boutons;    /*!< rangée des boutons d'action (bas du panneau) */
    banc_rect_t bandeau;    /*!< lignes d'état (bas de l'écran) */
} banc_zones_t;

/**
 * Initialise l'écran choisi dans le menu « Banc d'essai » (LDO du PHY MIPI,
 * bus DSI, panneau, PPA, rétroéclairage). ESP_ERR_NOT_SUPPORTED : aucun écran
 * configuré. En cas d'échec, le banc continue sans écran.
 */
esp_err_t banc_ecran_init(void);

/** true si l'écran est en service. */
bool banc_ecran_ok(void);

/** Description de l'écran (« EK79007 1024x600 », « aucun »...). */
const char *banc_ecran_nom(void);

/** Zones de l'écran (toutes nulles sans écran). */
const banc_zones_t *banc_ecran_zones(void);

/** Largeur en pixels d'un caractère à l'échelle e (police 5x7 dans une cellule 6x9). */
uint16_t banc_ecran_car_l(int echelle);

/** Hauteur en pixels d'un caractère à l'échelle e. */
uint16_t banc_ecran_car_h(int echelle);

/**
 * Affiche une image de la caméra dans la zone de vue (conversion YUV 4:2:0 →
 * RGB565 et mise à l'échelle par le PPA). Pendant un enregistrement, limitée à
 * quelques images par seconde (menu « Banc d'essai »), jamais bloquante : une
 * image est sautée si l'écran est occupé.
 */
void banc_ecran_image(const nid_capture_frame_t *f);

/** Écrit une ligne du bandeau (texte UTF-8, accents du français affichés). */
void banc_ecran_ligne(banc_ligne_t ligne, banc_couleur_t couleur, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/** Efface la zone de vue et y écrit un message (caméra absente...). */
void banc_ecran_message_vue(banc_couleur_t couleur, const char *texte);

/** Remplit un rectangle. */
void banc_ecran_rect(const banc_rect_t *r, banc_couleur_t couleur);

/**
 * Remplit le rectangle avec le fond puis y écrit le texte (lignes séparées par
 * « \n »), centré horizontalement et verticalement si centre, sinon en haut à
 * gauche avec une marge de 6 pixels. Le texte trop long est coupé.
 */
void banc_ecran_texte(const banc_rect_t *r, int echelle, banc_couleur_t encre, banc_couleur_t fond, bool centre,
                      const char *texte);

/** Barre horizontale (vumètre) : part remplie en ‰ (0..1000), repère facultatif en ‰ (-1 : aucun). */
void banc_ecran_barre(const banc_rect_t *r, int pour_mille, int repere_pour_mille, banc_couleur_t couleur,
                      banc_couleur_t fond);

/** Disque plein (voyant « REC »). */
void banc_ecran_disque(uint16_t cx, uint16_t cy, uint16_t rayon, banc_couleur_t couleur);

/* ========================================================================= */
/* Dalle tactile (banc_tactile.c)                                             */
/* ========================================================================= */

/** Détecte et initialise le GT911 sur le bus I2C partagé. ESP_ERR_NOT_FOUND : pas de dalle. */
esp_err_t banc_tactile_init(void);

/** true si la dalle est en service. */
bool banc_tactile_ok(void);

/** Description (« GT911 0x5D », « absente »...). */
const char *banc_tactile_nom(void);

/** Lit la dalle : true si un doigt est posé, coordonnées en pixels de l'écran. */
bool banc_tactile_lire(uint16_t *x, uint16_t *y);

/* ========================================================================= */
/* État partagé (banc_main.c)                                                 */
/* ========================================================================= */

typedef enum {
    BANC_ONGLET_DETECTION = 0,
    BANC_ONGLET_PHOTO,
    BANC_ONGLET_NUIT,
    BANC_ONGLET_SON,
    BANC_ONGLET_SYSTEME,
    BANC_NB_ONGLETS,
} banc_onglet_t;

typedef enum {
    BANC_NUIT_JOUR = 0,     /*!< forcé jour : filtre en place, LED éteintes, couleur */
    BANC_NUIT_NUIT,         /*!< forcé nuit : filtre retiré, LED à puissance_ir, monochrome */
    BANC_NUIT_AUTO,         /*!< comme le firmware : lever / coucher du soleil (réévalué chaque minute) */
} banc_mode_nuit_t;

/** Instantané de l'état du banc (lu par l'écran et la console). */
typedef struct {
    /* Surveillance PIR (banc_detection.c) */
    bool     arme;
    int64_t  chauffe_fin_us;        /*!< fin de la chauffe du PIR (esp_timer) */
    int64_t  delai_fin_us;          /*!< fin du délai entre déclenchements (esp_timer), 0 : aucun */
    uint32_t fronts;                /*!< fronts montants du PIR reçus */
    uint32_t detections;            /*!< fronts acceptés (enregistrement lancé) */
    uint32_t sequences;             /*!< séquences enregistrées et validées */
    uint32_t fausses;               /*!< fausses détections (PIR trop bref pour la sensibilité) */
    uint32_t ignorees;              /*!< fronts ignorés (désarmé, chauffe, délai, occupé) */
    char     ignoree_motif[24];     /*!< motif du dernier front ignoré */
    int32_t  latence_ms;            /*!< front PIR → 1re image de la dernière séquence (-1 : aucune) */
    char     dernier_fichier[64];   /*!< dernier média enregistré */

    /* Enregistrement en cours */
    bool     rec;
    int64_t  rec_debut_us;
    uint32_t rec_duree_s;
    char     rec_motif[24];         /*!< « PIR », « manuel », « boucle 3/10 »... */
    uint32_t rec_images;            /*!< images reçues par le crochet pendant l'enregistrement */

    /* Dernier bilan vidéo */
    uint32_t bilan_images;
    uint32_t bilan_perdues;
    uint32_t bilan_trames_son;
    uint32_t bilan_duree_ms;
    bool     bilan_ok;

    /* Photos */
    uint32_t photos;                /*!< photos prises (manuelles et de suivi) */
    uint32_t photos_suivi;
    char     photo_fichier[64];
    uint32_t photo_ko;
    uint32_t photo_ms;
    bool     photo_ok;
    int64_t  suivi_prochaine_us;    /*!< prochaine photo de suivi (esp_timer), 0 : aucune */

    /* Nuit */
    banc_mode_nuit_t mode_nuit;
    bool     nuit_active;           /*!< état appliqué (filtre, LED, monochrome) */
    uint8_t  led_ir_pct;            /*!< puissance appliquée aux LED IR */
    int8_t   filtre_jour;           /*!< 1 jour, 0 nuit, -1 inconnu / non câblé */
    bool     mono;

    /* Son */
    bool     son_ok;                /*!< micro prêt (ES8311) */
    nid_capture_audio_level_t son_niveau;
    bool     son_niveau_valide;

    /* Carte SD : test de débit */
    bool     sd_test_fait;
    uint32_t sd_debit_ko_s;         /*!< ko/s (1 ko = 1000 o) */
    uint32_t sd_latence_max_ms;
    uint32_t sd_blocs_lents;        /*!< écritures de plus de 100 ms */
    uint32_t sd_test_mo;

    /* Endurance */
    bool     boucle_en_cours;
    uint32_t boucle_n, boucle_i, boucle_echecs, boucle_perdues;
    int32_t  boucle_delta_interne_ko, boucle_delta_psram_ko;
    bool     boucle_faite;
} banc_etat_t;

/** Copie de l'état (section critique de quelques µs). */
void banc_etat_lire(banc_etat_t *copie);

/**
 * Accès en écriture à l'état, sous section critique : quelques affectations
 * seulement entre les deux appels (aucun appel bloquant, aucun journal).
 */
banc_etat_t *banc_etat_verrouiller(void);
void banc_etat_deverrouiller(void);

/* ========================================================================= */
/* Actions (banc_main.c) : une seule à la fois, dans la tâche « banc_action » */
/* ========================================================================= */

typedef enum {
    BANC_ACTION_VIDEO = 0,      /*!< vidéo manuelle (bouton, console, tactile) */
    BANC_ACTION_PHOTO,          /*!< photo manuelle */
    BANC_ACTION_PHOTO_SUIVI,    /*!< photo de suivi périodique */
    BANC_ACTION_DETECTION,      /*!< séquence sur front du PIR */
    BANC_ACTION_SON_VIDEO,      /*!< vidéo de test du son */
    BANC_ACTION_SD_TEST,        /*!< test de débit de la carte SD */
    BANC_ACTION_SD_SCAN,        /*!< recomptage de l'espace libre */
    BANC_ACTION_BOUCLE,         /*!< endurance : N vidéos d'affilée */
} banc_action_t;

typedef struct {
    banc_action_t type;
    uint32_t param;             /*!< durée (s), taille (Mo), nombre de vidéos */
    uint32_t param2;            /*!< durée des vidéos de la boucle (s) */
    int64_t  t_demande_us;      /*!< appui, commande ou front du PIR (esp_timer) */
    char     origine[12];       /*!< « bouton », « console », « tactile », « PIR »... */
} banc_demande_t;

/** Confie une action à la tâche d'action. ESP_ERR_INVALID_STATE si une action est déjà en cours. */
esp_err_t banc_demander(banc_action_t type, uint32_t param, uint32_t param2, const char *origine);

/** Variante avec l'instant de référence (front du PIR). */
esp_err_t banc_demander_a(banc_action_t type, uint32_t param, uint32_t param2, int64_t t_ref_us,
                          const char *origine);

/** true si une action est demandée ou en cours. */
bool banc_occupe(void);

/** true pendant un enregistrement vidéo. */
bool banc_enregistrement_en_cours(void);

/** Demande l'arrêt anticipé de l'enregistrement en cours (et de la boucle d'endurance). */
void banc_arreter(const char *origine);

/** true si un arrêt anticipé a été demandé (lu par les rappels d'enregistrement). */
bool banc_arret_demande(void);

/** Réglages de capture courants. */
const nid_capture_settings_t *banc_reglages_capture(void);

/** Prépare la carte SD et la caméra ; en cas d'échec, l'erreur est affichée. */
bool banc_preparer(void);

/** Monte la carte SD si besoin (identifiants, récupération des fichiers interrompus). */
esp_err_t banc_preparer_sd(void);

/** Description du volume de la carte (dernière lecture). */
void banc_volume_sd(nid_storage_volume_t *v);

/** Lit l'espace libre de la carte (après chaque écriture). */
void banc_lire_espace_sd(void);

/** « 29.1 Go libres » ou « absente ». */
void banc_texte_sd(char *out, size_t len);

/** Espace de la carte (dernière lecture). */
void banc_espace_sd(uint64_t *total, uint64_t *libre);

/** Affiche une erreur : étape et code, piste de dépannage (« \n » sépare deux lignes). */
void banc_afficher_erreur(const char *etape, esp_err_t err, const char *piste);

/** Efface les lignes de détail du bandeau. */
void banc_effacer_details(void);

/** Affiche sur la console l'état complet du banc (commande « info »). */
void banc_afficher_info(void);

/** Redessine la ligne d'en-tête (après un changement d'heure...). */
void banc_rafraichir_entete(void);

/** Heure locale au format strftime. */
void banc_heure_locale(char *out, size_t len, const char *fmt);

/* ========================================================================= */
/* Vidéo et photo (banc_video.c), appelées par la tâche d'action              */
/* ========================================================================= */

/** Rappel d'enregistrement supplémentaire (détection : validation du PIR). true = arrêter. */
typedef bool (*banc_rappel_t)(void *arg);

typedef struct {
    esp_err_t           err;
    bool                finalisation;   /*!< échec du renommage .tmp → nom définitif */
    bool                arrete;         /*!< arrêt anticipé */
    bool                annule;         /*!< fichier supprimé (fausse détection) */
    nid_capture_stats_t st;
    char                nom[64];
    int32_t             premiere_ms;    /*!< 1re image − instant de référence (ms), -1 : inconnue */
} banc_bilan_video_t;

/**
 * Enregistre une vidéo par le vrai chemin du firmware (media_begin → record →
 * commit / abort), affiche et journalise le bilan (ligne « BILAN »).
 * rappel (peut être NULL) : appelé ~10 fois par seconde, true → arrêt anticipé ;
 * annuler (peut être NULL) : appelé après l'enregistrement, avant la
 * finalisation, true → fichier supprimé (fausse détection).
 */
void banc_video_enregistrer(const banc_demande_t *d, uint32_t duree_s, const char *motif, banc_rappel_t rappel,
                            banc_rappel_t annuler, void *arg, banc_bilan_video_t *bilan);

/** Prend une photo JPEG (manuelle ou de suivi) et affiche son bilan. */
esp_err_t banc_photo_prendre(const banc_demande_t *d, bool suivi);

/** Endurance : N vidéos d'affilée, bilan mémoire et images perdues. */
void banc_boucle(const banc_demande_t *d);

/** Crochet de nid_capture : chaque image capturée (viseur, vidéo, photo). */
void banc_crochet_image(const nid_capture_frame_t *f, void *arg);

/** Dernière ligne de bilan (commande « info »). */
const char *banc_dernier_bilan(void);

/* ========================================================================= */
/* Surveillance PIR, réglages, photos de suivi (banc_detection.c)             */
/* ========================================================================= */

/** Réglages du firmware (nid_config) + valeurs du menu « Banc d'essai », interruptions du PIR. */
esp_err_t banc_detection_init(void);

/** Copie des réglages courants (nid_config_t). */
void banc_reglages(nid_config_t *out);

/**
 * Modifie un réglage (clé de config.ini, voir nid_config.h) à chaud.
 * ESP_ERR_NOT_FOUND : clé inconnue ou non modifiable sur le banc ;
 * ESP_ERR_INVALID_ARG : valeur invalide.
 */
esp_err_t banc_regler(const char *cle, const char *valeur);

/** Liste des clés modifiables à chaud (console « regle »). */
const char *const *banc_cles_reglables(void);

/** Arme ou désarme la surveillance (le délai en cours est annulé à l'armement, comme le firmware). */
void banc_armer(bool arme, const char *origine);

/** Séquence(s) sur détection : appelée par la tâche d'action. */
void banc_detection_sequence(const banc_demande_t *d);

/** Photo de suivi due (tâche d'action, entre deux images du viseur). */
bool banc_suivi_du(void);

/** Replanifie la photo de suivi (après une photo ou un changement d'intervalle). */
void banc_suivi_planifier(void);

/** Remet les compteurs de détection à zéro. */
void banc_compteurs_raz(void);

/** Libellé de l'état du déclenchement (« ON (armé) », « OFF : chauffe 42 s »...). */
void banc_texte_declenchement(const banc_etat_t *e, int64_t maintenant_us, char *out, size_t len,
                              banc_couleur_t *fond);

/* ========================================================================= */
/* Tests (banc_tests.c)                                                       */
/* ========================================================================= */

/** LED IR, IR-cut, capteur de température, batterie. */
void banc_tests_init(void);

/** Mode jour / nuit / auto du banc (filtre, LED IR, monochrome). */
esp_err_t banc_nuit_mode(banc_mode_nuit_t mode);

/** Réévalue le mode auto (chaque minute) et réapplique après un réglage. */
void banc_nuit_actualiser(bool forcer);

/** Puissance des LED IR (test direct, %). */
esp_err_t banc_ir_puissance(uint8_t pct);

/** Filtre IR-cut (test direct). */
esp_err_t banc_ircut(bool jour);

/** Image monochrome (test direct). */
esp_err_t banc_mono(bool mono);

/** Lever et coucher du soleil du jour (heure locale), décision « nuit » du firmware maintenant. */
esp_err_t banc_soleil(char *lever, char *coucher, size_t len, bool *nuit_maintenant);

/** Tension batterie : ESP_OK (mV), ESP_ERR_INVALID_RESPONSE (non câblée, *brut_mv lu), ESP_ERR_NOT_SUPPORTED. */
esp_err_t banc_batterie(uint32_t *mv, uint32_t *brut_mv);

/** Température de la puce (°C). */
esp_err_t banc_temperature(float *c);

/** Vumètre : mesure le niveau du micro (tâche d'action, hors enregistrement). */
void banc_son_mesurer(void);

/** Niveau en dBFS ×10 (valeur absolue 16 bits → dBFS ; -900 pour 0). */
int banc_dbfs10(uint16_t v);

/** Test de débit de la carte SD (tâche d'action). */
void banc_sd_test(const banc_demande_t *d);

/** Recomptage complet de l'espace libre (tâche d'action). */
void banc_sd_scan(void);

/* ========================================================================= */
/* Interface (banc_ui.c)                                                      */
/* ========================================================================= */

/** Dessine les onglets et démarre la tâche d'affichage (panneau, tactile). */
esp_err_t banc_ui_demarrer(void);

/** Onglet affiché. */
banc_onglet_t banc_ui_onglet(void);

/** Change d'onglet (console « ecran », tactile). */
void banc_ui_choisir(banc_onglet_t onglet);

/** Nom de l'onglet (« detection », « photo »...). */
const char *banc_ui_nom_onglet(banc_onglet_t onglet);

/* ========================================================================= */
/* Console (banc_console.c)                                                   */
/* ========================================================================= */

/** Enregistre les commandes et démarre la console interactive (REPL). */
esp_err_t banc_console_demarrer(void);

#ifdef __cplusplus
}
#endif
