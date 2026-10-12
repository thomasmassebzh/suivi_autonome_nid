/*
 * nid_overlay — incrustation de texte (date, heure, identifiants) dans les images
 *
 * Police bitmap 5x7 intégrée (ASCII imprimable 0x20–0x7E, jambages sur deux
 * lignes supplémentaires), agrandie d'un facteur entier, écrite en blanc sur
 * un cadre opaque sombre directement dans un tampon au format YUV 4:2:0
 * « Espressif » (O_UYY_E_VYY), celui de l'ISP, de l'encodeur H.264 et de
 * l'encodeur JPEG de l'ESP32-P4 :
 *
 *   ligne paire   : U00 Y00 Y01  U02 Y02 Y03 ...
 *   ligne impaire : V10 Y10 Y11  V12 Y12 Y13 ...
 *
 * (3 octets pour 2 pixels, pas = largeur × 3 / 2). Le cadre est aligné sur
 * des coordonnées paires : chaque couple U/V du cadre est mis à 128 (gris).
 *
 * Le rendu de la chaîne est fait une fois par changement de texte (une fois
 * par seconde) dans des lignes « modèles » ; l'incrustation d'une image se
 * réduit ensuite à une copie mémoire par ligne du cadre.
 *
 * Module sans dépendance matérielle : testable sur la machine hôte.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NID_OVERLAY_MAX_CHARS   64      /*!< longueur maximale du texte incrusté */
#define NID_OVERLAY_CELL_W      6       /*!< cellule d'un caractère : 5 colonnes + 1 d'espacement */
#define NID_OVERLAY_CELL_H      9       /*!< 7 lignes de corps + 2 lignes de jambage */

/** Contexte d'incrustation pour une taille d'image donnée. */
typedef struct {
    uint16_t frame_w;           /*!< largeur de l'image (paire) */
    uint16_t frame_h;           /*!< hauteur de l'image (paire) */
    uint8_t  scale_max;         /*!< facteur d'agrandissement demandé */
    uint8_t  scale;             /*!< facteur effectif (réduit si le texte est trop long) */
    bool     bottom;            /*!< cadre en bas à gauche (sinon en haut à gauche) */
    uint8_t  y_fg;              /*!< luminance du texte */
    uint8_t  y_bg;              /*!< luminance du cadre */
    char     text[NID_OVERLAY_MAX_CHARS + 1];
    uint16_t box_x, box_y;      /*!< coin supérieur gauche du cadre (pairs) */
    uint16_t box_w, box_h;      /*!< dimensions du cadre (paires), 0 = rien à dessiner */
    uint8_t *tpl;               /*!< lignes modèles : NID_OVERLAY_CELL_H + 1 lignes de box_w × 3 / 2 octets */
    size_t   tpl_cap;           /*!< capacité allouée pour tpl */
} nid_overlay_t;

/**
 * Facteur d'agrandissement automatique selon la hauteur de l'image :
 * 1080 → 5, 960 → 4, 720 → 3, 480 → 2, ≤ 360 → 1.
 */
uint8_t nid_overlay_auto_scale(uint16_t frame_h);

/**
 * Prépare un contexte. full_range : niveaux 0/255 (JPEG, JFIF) au lieu de
 * 16/235 (vidéo H.264 en plage limitée).
 * @return false si les dimensions sont invalides.
 */
bool nid_overlay_init(nid_overlay_t *ov, uint16_t frame_w, uint16_t frame_h,
                      uint8_t scale, bool bottom, bool full_range);

/** Libère les lignes modèles. */
void nid_overlay_deinit(nid_overlay_t *ov);

/**
 * Change le texte (rendu uniquement s'il diffère du texte courant).
 * Les caractères hors ASCII imprimable sont remplacés par « ? ».
 * @return false en cas de manque de mémoire (rien ne sera dessiné).
 */
bool nid_overlay_set_text(nid_overlay_t *ov, const char *text);

/** Incruste le cadre dans une image YUV 4:2:0 Espressif de frame_w × frame_h. */
void nid_overlay_draw(const nid_overlay_t *ov, uint8_t *frame);

/**
 * Zone d'octets modifiée par nid_overlay_draw() (lignes complètes du cadre),
 * pour la synchronisation du cache avant lecture par un encodeur matériel.
 * @return false si rien n'est dessiné.
 */
bool nid_overlay_dirty_range(const nid_overlay_t *ov, size_t *offset, size_t *len);

/** Bitmap d'un caractère (9 lignes, bit 4 = colonne de gauche). */
const uint8_t *nid_overlay_glyph(char c);

#ifdef __cplusplus
}
#endif
