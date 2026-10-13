/*
 * Banc d'essai « Suivi nid » — déclarations communes
 *
 *  - banc_main.c    : démarrage, tâche d'action (vidéo, photo, viseur), bouton BOOT ;
 *  - banc_ecran.c   : écran MIPI-DSI (vue caméra + bandeau d'état) ;
 *  - banc_console.c : commandes de la console série.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "nid_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Écran (banc_ecran.c)                                                       */
/* ------------------------------------------------------------------------- */

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
} banc_couleur_t;

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

/**
 * Affiche une image de la caméra dans la zone de vue (conversion YUV 4:2:0 →
 * RGB565 et mise à l'échelle par le PPA). Pendant un enregistrement, limitée à
 * quelques images par seconde (menu « Banc d'essai ») : les autres sont sautées.
 */
void banc_ecran_image(const nid_capture_frame_t *f);

/** Écrit une ligne du bandeau (texte UTF-8, accents du français affichés). */
void banc_ecran_ligne(banc_ligne_t ligne, banc_couleur_t couleur, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/** Efface la zone de vue et y écrit un message (caméra absente...). */
void banc_ecran_message_vue(banc_couleur_t couleur, const char *texte);

/* ------------------------------------------------------------------------- */
/* Actions (banc_main.c)                                                      */
/* ------------------------------------------------------------------------- */

/**
 * Demande un enregistrement vidéo à la tâche d'action.
 * @return ESP_ERR_INVALID_STATE si une action est déjà en cours.
 */
esp_err_t banc_demander_video(uint32_t duree_s, const char *origine);

/** Demande une photo JPEG. @return ESP_ERR_INVALID_STATE si une action est en cours. */
esp_err_t banc_demander_photo(const char *origine);

/** true pendant un enregistrement vidéo. */
bool banc_enregistrement_en_cours(void);

/** Mode nuit (image monochrome par l'ISP) ; mémorisé pour les réinitialisations. */
esp_err_t banc_regler_nuit(bool nuit);

/** Affiche sur la console l'état complet du banc (commande « info »). */
void banc_afficher_info(void);

/** Redessine la ligne d'en-tête (après un changement d'heure...). */
void banc_rafraichir_entete(void);

/* ------------------------------------------------------------------------- */
/* Console (banc_console.c)                                                   */
/* ------------------------------------------------------------------------- */

/** Enregistre les commandes et démarre la console interactive (REPL). */
esp_err_t banc_console_demarrer(void);

#ifdef __cplusplus
}
#endif
