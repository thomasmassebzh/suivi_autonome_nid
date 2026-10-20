/*
 * banc_ui — tableau de bord du banc d'essai sur l'écran 7 pouces
 *
 * Onglets (barre du haut, tactile ou commande « ecran ») :
 *   DÉTECTION : PIR en temps réel, déclenchement (armé, chauffe, délai), REC,
 *               compteurs, latence front PIR → 1re image, dernier fichier ;
 *   PHOTO     : dernière photo, photo de suivi (compte à rebours) ;
 *   NUIT      : mode, LED IR, filtre IR-cut, monochrome, lever / coucher du soleil ;
 *   SON       : vumètre du micro, trames son de la dernière vidéo ;
 *   SYSTÈME   : batterie, température de la puce, carte SD, mémoire, endurance ;
 *   WI-FI     : mode, SSID, adresse, clients ou signal, firmware du C6, lien.
 * Boutons d'action (bas du panneau) : ARMER / DÉSARMER, VIDÉO (STOP pendant un
 * enregistrement), PHOTO, NUIT / JOUR.
 *
 * Tâche « banc_ui » (priorité basse) : lecture de la dalle toutes les 40 ms,
 * panneau toutes les 100 ms ; seules les zones dont le texte change sont
 * redessinées (quelques ko écrits en PSRAM), la vue caméra n'est pas touchée.
 */
#include "banc.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "nid_board.h"
#include "nid_detect.h"

static const char *TAG = "banc_ui";

#define UI_PILE             6144
#define UI_PRIO             2
#define UI_PERIODE_MS       40
#define PANNEAU_PERIODE_US  100000LL
#define ENTETE_PERIODE_US   1000000LL
#define MESURES_PERIODE_US  2000000LL
#define BOITE_H             62
#define ETIQUETTE_H         24
#define LIGNE_H             24
#define ESPACE              4
#define NB_BOITES           3
#define NB_LIGNES           6
#define NB_BOUTONS          4
#define TEXTE_ZONE          64

typedef struct {
    banc_rect_t    r;
    char           texte[TEXTE_ZONE];
    int8_t         echelle;
    banc_couleur_t encre, fond;
    bool           valide;
} zone_t;

typedef struct {
    zone_t etiquette;
    zone_t valeur;
    int    barre, repere;       /*!< vumètre (‰), -1 : pas de barre */
} boite_t;

static const char *const s_noms_onglets[BANC_NB_ONGLETS] = {
    [BANC_ONGLET_DETECTION] = "detection",
    [BANC_ONGLET_PHOTO] = "photo",
    [BANC_ONGLET_NUIT] = "nuit",
    [BANC_ONGLET_SON] = "son",
    [BANC_ONGLET_SYSTEME] = "systeme",
    [BANC_ONGLET_WIFI] = "wifi",
};

static const char *const s_titres_onglets[BANC_NB_ONGLETS] = {
    [BANC_ONGLET_DETECTION] = "DÉTECTION",
    [BANC_ONGLET_PHOTO] = "PHOTO",
    [BANC_ONGLET_NUIT] = "NUIT",
    [BANC_ONGLET_SON] = "SON",
    [BANC_ONGLET_SYSTEME] = "SYSTÈME",
    [BANC_ONGLET_WIFI] = "WI-FI",
};

static volatile banc_onglet_t s_onglet = BANC_ONGLET_DETECTION;
static volatile bool s_redessiner = true;
static banc_rect_t s_contenu;
static banc_rect_t s_r_onglets[BANC_NB_ONGLETS];
static zone_t s_z_boutons[NB_BOUTONS];
static boite_t s_boites[NB_BOITES];
static zone_t s_lignes[NB_LIGNES];

/* Mesures lentes (onglet Système), rafraîchies toutes les 2 s */
static struct {
    esp_err_t batt_err;
    uint32_t  batt_mv, batt_brut_mv;
    esp_err_t temp_err;
    float     temp_c;
} s_mes;

/* ------------------------------------------------------------------------- */
/* Zones                                                                      */
/* ------------------------------------------------------------------------- */

/** Redessine la zone si son contenu change. @return true si elle a été redessinée. */
static bool zone_ecrire(zone_t *z, int echelle, banc_couleur_t encre, banc_couleur_t fond, const char *texte)
{
    if (z->valide && z->echelle == echelle && z->encre == encre && z->fond == fond && strcmp(z->texte, texte) == 0) {
        return false;
    }
    banc_ecran_texte(&z->r, echelle, encre, fond, z->r.h <= LIGNE_H ? false : true, texte);
    strlcpy(z->texte, texte, sizeof(z->texte));
    z->echelle = (int8_t)echelle;
    z->encre = encre;
    z->fond = fond;
    z->valide = true;
    return true;
}

static void ligne(int i, banc_couleur_t encre, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void ligne(int i, banc_couleur_t encre, const char *fmt, ...)
{
    char t[TEXTE_ZONE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    zone_ecrire(&s_lignes[i], 2, encre, BANC_FOND, t);
}

/**
 * Boîte : étiquette en petit (haut gauche) et valeur en grand (centrée).
 * @return true si la valeur a été redessinée.
 */
static bool boite(int i, const char *etiquette, int echelle, banc_couleur_t encre, banc_couleur_t fond,
                  const char *fmt, ...) __attribute__((format(printf, 6, 7)));
static bool boite(int i, const char *etiquette, int echelle, banc_couleur_t encre, banc_couleur_t fond,
                  const char *fmt, ...)
{
    boite_t *b = &s_boites[i];
    char t[TEXTE_ZONE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    zone_ecrire(&b->etiquette, 2, encre == BANC_NOIR ? BANC_NOIR : BANC_GRIS, fond, etiquette);
    if (b->barre >= 0) {
        b->valeur.valide = false;     /* barre du vumètre à remplacer par le texte */
        b->barre = -1;
    }
    return zone_ecrire(&b->valeur, echelle, encre, fond, t);
}

/** Boîte « vumètre » : barre (‰) et repère de crête (‰, -1 : aucun). */
static void boite_barre(int i, const char *etiquette, int pour_mille, int repere)
{
    boite_t *b = &s_boites[i];
    zone_ecrire(&b->etiquette, 2, BANC_GRIS, BANC_FOND, etiquette);
    if (b->barre == pour_mille && b->repere == repere) {
        return;
    }
    if (b->barre < 0) {
        banc_ecran_rect(&b->valeur.r, BANC_FOND);     /* texte précédent effacé */
    }
    banc_rect_t r = b->valeur.r;
    r.x += 6;
    r.w -= 12;
    r.y += 4;
    r.h -= 10;
    banc_ecran_barre(&r, pour_mille, repere, pour_mille > 900 ? BANC_ROUGE_VIF : BANC_BLEU, BANC_GRIS_FONCE);
    b->barre = pour_mille;
    b->repere = repere;
    b->valeur.valide = false;       /* un texte ultérieur repeindra la zone */
}

static void invalider_tout(void)
{
    for (int i = 0; i < NB_BOITES; i++) {
        s_boites[i].etiquette.valide = false;
        s_boites[i].valeur.valide = false;
        s_boites[i].barre = -1;
    }
    for (int i = 0; i < NB_LIGNES; i++) {
        s_lignes[i].valide = false;
    }
    for (int i = 0; i < NB_BOUTONS; i++) {
        s_z_boutons[i].valide = false;
    }
}

static void disposer(void)
{
    const banc_zones_t *z = banc_ecran_zones();
    s_contenu = (banc_rect_t){ z->panneau.x, z->panneau.y, z->panneau.w,
                               (uint16_t)(z->boutons.y - z->panneau.y - ESPACE) };
    for (int i = 0; i < NB_BOITES; i++) {
        const uint16_t y = (uint16_t)(s_contenu.y + i * (BOITE_H + ESPACE));
        s_boites[i].etiquette.r = (banc_rect_t){ s_contenu.x, y, s_contenu.w, ETIQUETTE_H };
        s_boites[i].valeur.r = (banc_rect_t){ s_contenu.x, (uint16_t)(y + ETIQUETTE_H), s_contenu.w,
                                              BOITE_H - ETIQUETTE_H };
    }
    const uint16_t y0 = (uint16_t)(s_contenu.y + NB_BOITES * (BOITE_H + ESPACE) + 2);
    for (int i = 0; i < NB_LIGNES; i++) {
        s_lignes[i].r = (banc_rect_t){ s_contenu.x, (uint16_t)(y0 + i * LIGNE_H), s_contenu.w, LIGNE_H };
    }
    const uint16_t bw = (uint16_t)((z->boutons.w - (NB_BOUTONS - 1) * ESPACE) / NB_BOUTONS);
    for (int i = 0; i < NB_BOUTONS; i++) {
        s_z_boutons[i].r = (banc_rect_t){ (uint16_t)(z->boutons.x + i * (bw + ESPACE)), z->boutons.y, bw,
                                          z->boutons.h };
    }
    const uint16_t ow = (uint16_t)(z->onglets.w / BANC_NB_ONGLETS);
    for (int i = 0; i < BANC_NB_ONGLETS; i++) {
        s_r_onglets[i] = (banc_rect_t){ (uint16_t)(z->onglets.x + i * ow), z->onglets.y, (uint16_t)(ow - 2),
                                        z->onglets.h };
    }
}

static void dessiner_onglets(void)
{
    for (int i = 0; i < BANC_NB_ONGLETS; i++) {
        const bool actif = (i == (int)s_onglet);
        banc_ecran_texte(&s_r_onglets[i], 2, actif ? BANC_BLANC : BANC_GRIS, actif ? BANC_BLEU : BANC_GRIS_FONCE, true,
                         s_titres_onglets[i]);
    }
}

/* ------------------------------------------------------------------------- */
/* Onglets                                                                    */
/* ------------------------------------------------------------------------- */

/** Nom court d'un média : sans le préfixe « BANC_N00_ » ni l'extension. */
static void nom_court(const char *nom, char *out, size_t len)
{
    const char *p = strchr(nom, '_');
    p = (p != NULL) ? strchr(p + 1, '_') : NULL;
    strlcpy(out, (p != NULL) ? p + 1 : nom, len);
    char *pt = strrchr(out, '.');
    if (pt != NULL) {
        *pt = '\0';
    }
}

static void onglet_detection(const banc_etat_t *e, int64_t now)
{
    char t[48];
    snprintf(t, sizeof(t), "PIR (GPIO%d)", (int)nid_board_pins()->pir);
    if (!banc_f_active(BANC_F_PIR)) {
        boite(0, t, 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", banc_f_texte(BANC_F_PIR));
    } else {
        const bool pir = nid_detect_pir_active();
        boite(0, t, 4, pir ? BANC_BLANC : BANC_GRIS, pir ? BANC_ROUGE_VIF : BANC_GRIS_FONCE, "%s",
              pir ? "ACTIF" : "REPOS");
    }

    banc_couleur_t fond;
    banc_texte_declenchement(e, now, t, sizeof(t), &fond);
    boite(1, "DÉCLENCHEMENT", 3, fond == BANC_ORANGE ? BANC_NOIR : BANC_BLANC, fond, "%s", t);

    if (e->rec) {
        const int64_t s = (now - e->rec_debut_us) / 1000000LL;
        if (boite(2, "ENREGISTREMENT", 4, BANC_BLANC, BANC_ROUGE_VIF, "REC %" PRId64 "/%" PRIu32 " s", s,
                  e->rec_duree_s)) {
            /* Voyant blanc à gauche du texte, redessiné avec la zone. */
            const banc_rect_t *v = &s_boites[2].valeur.r;
            banc_ecran_disque((uint16_t)(v->x + 22), (uint16_t)(v->y + v->h / 2), 12, BANC_BLANC);
        }
    } else {
        boite(2, "ENREGISTREMENT", 4, BANC_GRIS, BANC_GRIS_FONCE, "%s", "REC OFF");
    }

    ligne(0, BANC_BLANC, "Fronts %" PRIu32 "  Détections %" PRIu32, e->fronts, e->detections);
    ligne(1, e->fausses ? BANC_JAUNE : BANC_BLANC, "Séquences %" PRIu32 "  Fausses %" PRIu32, e->sequences, e->fausses);
    if (e->ignorees) {
        ligne(2, BANC_GRIS, "Ignorés %" PRIu32 " (%s)", e->ignorees, e->ignoree_motif);
    } else {
        ligne(2, BANC_GRIS, "%s", "Ignorés 0");
    }
    if (e->latence_ms >= 0) {
        ligne(3, e->latence_ms <= 1000 ? BANC_VERT : BANC_JAUNE, "Latence front→image %" PRId32 " ms", e->latence_ms);
    } else {
        ligne(3, BANC_GRIS, "%s", "Latence front→image : -");
    }
    char nom[48];
    nom_court(e->dernier_fichier, nom, sizeof(nom));
    ligne(4, BANC_BLANC, "Dernier %s", nom);
    if (e->rec) {
        const int64_t ms = (now - e->rec_debut_us) / 1000;
        const uint32_t ips10 = ms > 500 ? (uint32_t)((int64_t)e->rec_images * 10000 / ms) : 0;
        ligne(5, BANC_BLANC, "Images reçues %" PRIu32 " (%" PRIu32 ".%" PRIu32 " i/s)", e->rec_images, ips10 / 10,
              ips10 % 10);
    } else {
        nid_config_t cfg;
        banc_reglages(&cfg);
        ligne(5, BANC_GRIS, "Séq. %u s, délai %u s, sens. %u", cfg.seq_duration_s, cfg.trigger_delay_s,
              cfg.pir_sensitivity);
    }
}

static void onglet_photo(const banc_etat_t *e, int64_t now)
{
    char nom[48];
    nom_court(e->photo_fichier, nom, sizeof(nom));
    if (!banc_f_active(BANC_F_CAMERA)) {
        boite(0, "DERNIÈRE PHOTO", 3, BANC_GRIS, BANC_GRIS_FONCE, "caméra %s", banc_f_texte(BANC_F_CAMERA));
    } else {
        boite(0, "DERNIÈRE PHOTO", 3, e->photo_ok ? BANC_BLANC : BANC_GRIS, BANC_GRIS_FONCE, "%s", nom);
    }
    nid_config_t cfg;
    banc_reglages(&cfg);
    if (!banc_f_active(BANC_F_SUIVI)) {
        boite(1, "PHOTO DE SUIVI", 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", banc_f_texte(BANC_F_SUIVI));
    } else if (cfg.photo_interval_min == 0) {
        boite(1, "PHOTO DE SUIVI", 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", "arrêtée");
    } else if (!e->arme || e->suivi_prochaine_us == 0) {
        boite(1, "PHOTO DE SUIVI", 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", "désarmé");
    } else {
        int64_t s = (e->suivi_prochaine_us - now + 999999) / 1000000;
        if (s < 0) {
            s = 0;
        }
        boite(1, "PHOTO DE SUIVI", 3, BANC_BLANC, BANC_VERT_FONCE, "dans %" PRId64 ":%02" PRId64, s / 60, s % 60);
    }
    boite(2, "PHOTOS PRISES", 3, BANC_BLANC, BANC_GRIS_FONCE, "%" PRIu32 " (suivi %" PRIu32 ")", e->photos,
          e->photos_suivi);
    if (e->photo_ko) {
        ligne(0, BANC_BLANC, "Taille %" PRIu32 " ko, %" PRIu32 " ms", e->photo_ko, e->photo_ms);
    } else {
        ligne(0, BANC_GRIS, "%s", "Bouton PHOTO ou « photo »");
    }
    ligne(1, BANC_BLANC, "Intervalle de suivi %u min", cfg.photo_interval_min);
    ligne(2, BANC_GRIS, "%s", "Prise si la surveillance est");
    ligne(3, BANC_GRIS, "%s", "armée (comme le firmware)");
    ligne(4, BANC_GRIS, "%s", "Console : suivi <min> (0 = arrêt)");
    const nid_capture_settings_t *r = banc_reglages_capture();
    ligne(5, BANC_GRIS, "JPEG qualité %u, %ux%u", r->jpeg_quality, r->width, r->height);
}

static void onglet_nuit(const banc_etat_t *e)
{
    static const char *const modes[] = { "forcé", "forcé", "auto" };
    if (!banc_f_active(BANC_F_NUIT)) {
        const char *t = banc_f_texte(BANC_F_NUIT);
        boite(0, "MODE", 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", t);
        boite(1, "LED IR 940 nm", 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", t);
        boite(2, "FILTRE IR-CUT", 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", t);
        for (int i = 0; i < NB_LIGNES; i++) {
            ligne(i, BANC_GRIS, "%s", i == 0 ? "Fonction « nuit » non utilisée" : "");
        }
        return;
    }
    boite(0, "MODE", 3, BANC_BLANC, e->nuit_active ? BANC_BLEU : BANC_GRIS_FONCE, "%s (%s)",
          e->nuit_active ? "NUIT" : "JOUR", modes[e->mode_nuit]);
    if (nid_board_pins()->ir_led == GPIO_NUM_NC) {
        boite(1, "LED IR 940 nm", 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", "non câblée");
    } else {
        boite(1, "LED IR 940 nm", 3, BANC_BLANC, e->led_ir_pct ? BANC_ROUGE_VIF : BANC_GRIS_FONCE, "%u %% (GPIO%d)",
              e->led_ir_pct, (int)nid_board_pins()->ir_led);
    }
    boite(2, "FILTRE IR-CUT", 3, e->filtre_jour < 0 ? BANC_GRIS : BANC_BLANC, BANC_GRIS_FONCE, "%s",
          e->filtre_jour < 0 ? "non câblé" : (e->filtre_jour ? "jour (en place)" : "nuit (retiré)"));
    ligne(0, BANC_BLANC, "Image : %s", e->mono ? "monochrome" : "couleur");
    nid_config_t cfg;
    banc_reglages(&cfg);
    ligne(1, BANC_GRIS, "Lieu %.2f N, %.2f E", (double)cfg.latitude, (double)cfg.longitude);
    char lever[12], coucher[12];
    bool nuit = false;
    banc_soleil(lever, coucher, sizeof(lever), &nuit);
    ligne(2, BANC_BLANC, "Lever %s  coucher %s", lever, coucher);
    ligne(3, nuit ? BANC_BLEU : BANC_JAUNE, "Firmware : %s maintenant", nuit ? "NUIT" : "JOUR");
    ligne(4, BANC_GRIS, "Nuit dès coucher-%d, aube+%d min", cfg.dusk_advance_min, cfg.dawn_delay_min);
    ligne(5, BANC_GRIS, "%s", "nuit on|off|auto, ir, ircut, mono");
}

static void onglet_son(const banc_etat_t *e)
{
    nid_capture_audio_level_t l = e->son_niveau;
    bool valide = e->son_niveau_valide;
    if (e->rec) {
        valide = nid_capture_audio_last_level(&l);     /* niveau pendant l'enregistrement */
    }
    const nid_capture_settings_t *r = banc_reglages_capture();
    if (!banc_f_active(BANC_F_SON)) {
        valide = false;
        boite(0, "MICRO ES8311", 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", banc_f_texte(BANC_F_SON));
    } else {
        boite(0, "MICRO ES8311", 3, BANC_BLANC, valide ? BANC_VERT_FONCE : BANC_GRIS_FONCE, "%s",
              e->rec ? "enregistrement" : (valide ? "en écoute" : "absent ?"));
    }
    /* Échelle du vumètre : -60 dBFS (vide) à 0 dBFS (plein). */
    const int rms = banc_dbfs10(l.rms);
    const int crete = banc_dbfs10(l.peak);
    const int pm = valide ? (rms + 600) * 1000 / 600 : 0;
    const int rp = valide ? (crete + 600) * 1000 / 600 : -1;
    boite_barre(1, "NIVEAU (-60 à 0 dBFS)", pm < 0 ? 0 : pm, rp < 0 ? -1 : rp);
    if (valide) {
        boite(2, "EFFICACE / CRÊTE", 3, BANC_BLANC, BANC_GRIS_FONCE, "%d / %d dB", rms / 10, crete / 10);
    } else {
        boite(2, "EFFICACE / CRÊTE", 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", "-");
    }
    if (e->bilan_duree_ms) {
        ligne(0, (r->audio && e->bilan_trames_son == 0) ? BANC_JAUNE : BANC_BLANC, "Dernière vidéo : %" PRIu32 " trames",
              e->bilan_trames_son);
        ligne(1, BANC_GRIS, "Attendu ~%" PRIu32 " (64 ms chacune)", e->bilan_duree_ms / 64);
    } else {
        ligne(0, BANC_GRIS, "%s", "Pas encore de vidéo");
        ligne(1, BANC_GRIS, "%s", "");
    }
    ligne(2, BANC_GRIS, "%s", "Test : bouton VIDÉO ou « son »");
    ligne(3, BANC_GRIS, "%s", "Parler : la barre doit monter");
    ligne(4, BANC_GRIS, "%s", "Silence : moins de -50 dBFS");
    ligne(5, BANC_GRIS, "%s", "");
}

static void onglet_systeme(const banc_etat_t *e)
{
    char t[48];
    snprintf(t, sizeof(t), "BATTERIE (GPIO%d)", (int)nid_board_pins()->batt_adc);
    if (!banc_f_active(BANC_F_BATTERIE)) {
        boite(0, t, 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", banc_f_texte(BANC_F_BATTERIE));
    } else if (s_mes.batt_err == ESP_OK) {
        boite(0, t, 3, BANC_BLANC, BANC_VERT_FONCE, "%" PRIu32 ".%02" PRIu32 " V", s_mes.batt_mv / 1000,
              (s_mes.batt_mv % 1000) / 10);
    } else if (s_mes.batt_err == ESP_ERR_INVALID_RESPONSE) {
        boite(0, t, 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", "non câblée");
    } else {
        boite(0, t, 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", "pas de mesure");
    }
    if (!banc_f_active(BANC_F_TEMPERATURE)) {
        boite(1, "TEMPÉRATURE PUCE", 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", banc_f_texte(BANC_F_TEMPERATURE));
    } else if (s_mes.temp_err == ESP_OK) {
        boite(1, "TEMPÉRATURE PUCE", 3, BANC_BLANC, s_mes.temp_c > 70.0f ? BANC_ORANGE : BANC_GRIS_FONCE, "%.1f °C",
              (double)s_mes.temp_c);
    } else {
        boite(1, "TEMPÉRATURE PUCE", 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", "indisponible");
    }
    char sd[24];
    banc_texte_sd(sd, sizeof(sd));
    if (!banc_f_choisie(BANC_F_SD)) {
        boite(2, "CARTE SD", 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", banc_f_texte(BANC_F_SD));
    } else {
        boite(2, "CARTE SD", 3, BANC_BLANC, BANC_GRIS_FONCE, "%s", sd);
    }

    if (s_mes.batt_err == ESP_ERR_INVALID_RESPONSE) {
        ligne(0, BANC_JAUNE, "Lu %" PRIu32 " mV : ignoré (pas de pont)", s_mes.batt_brut_mv);
    } else {
        nid_config_t cfg;
        banc_reglages(&cfg);
        ligne(0, BANC_GRIS, "Seuils %u / %u mV", cfg.batt_low_mv, cfg.batt_critical_mv);
    }
    nid_storage_volume_t v;
    banc_volume_sd(&v);
    if (v.volume_bytes) {
        const bool petit = v.card_bytes > 0 && v.volume_bytes < v.card_bytes / 10 * 9;
        ligne(1, petit ? BANC_JAUNE : BANC_BLANC, "FAT%u %" PRIu64 " Go/carte %" PRIu64 " Go", v.fat_type,
              v.volume_bytes / 1000000000ULL, v.card_bytes / 1000000000ULL);
    } else {
        ligne(1, BANC_GRIS, "%s", "Volume : -");
    }
    if (e->sd_test_fait) {
        ligne(2, e->sd_latence_max_ms < 500 ? BANC_BLANC : BANC_JAUNE, "%" PRIu32 ".%02" PRIu32 " Mo/s, max %" PRIu32
              " ms", e->sd_debit_ko_s / 1000, (e->sd_debit_ko_s % 1000) / 10, e->sd_latence_max_ms);
    } else {
        ligne(2, BANC_GRIS, "%s", "Débit : « sd test »");
    }
    ligne(3, BANC_GRIS, "Mém. %u ko, PSRAM %u ko",
          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    if (e->boucle_en_cours) {
        ligne(4, BANC_JAUNE, "Boucle %" PRIu32 "/%" PRIu32 ", %" PRIu32 " échec(s)", e->boucle_i, e->boucle_n,
              e->boucle_echecs);
    } else if (e->boucle_faite) {
        ligne(4, (e->boucle_echecs || e->boucle_perdues) ? BANC_JAUNE : BANC_VERT, "Boucle %" PRIu32 " vid., %" PRIu32
              " perdues, %+" PRId32 " ko", e->boucle_n, e->boucle_perdues, e->boucle_delta_interne_ko);
    } else {
        ligne(4, BANC_GRIS, "%s", "Endurance : « boucle N »");
    }
    banc_heure_locale(t, sizeof(t), "%Y-%m-%d %H:%M:%S");
    ligne(5, BANC_GRIS, "%s", t);
}

#if CONFIG_BANC_F_WIFI
static void onglet_wifi_actif(const banc_wifi_etat_t *w)
{
    const bool ap = (w->st.mode == NID_WIFI_MODE_AP);
    const bool defaut = (banc_f_etat(BANC_F_WIFI) == BANC_FE_DEFAUT);
    const bool perdu = (w->st.state == NID_WIFI_STATE_LINK_LOST);
    banc_couleur_t fond = BANC_GRIS_FONCE;
    if (w->st.state == NID_WIFI_STATE_UP) {
        fond = BANC_VERT_FONCE;
    } else if (w->st.state == NID_WIFI_STATE_STARTING || w->st.state == NID_WIFI_STATE_CONNECTING) {
        fond = BANC_ORANGE;
    } else if (defaut) {
        fond = BANC_ROUGE_VIF;
    }
    boite(0, ap ? "POINT D'ACCÈS" : "STATION", 3, fond == BANC_ORANGE ? BANC_NOIR : BANC_BLANC, fond, "%s",
          defaut ? "en défaut" : nid_wifi_state_str(w->st.state));
    boite(1, "ADRESSE", 3, w->st.ip[0] ? BANC_BLANC : BANC_GRIS, BANC_GRIS_FONCE, "%s", w->st.ip[0] ? w->st.ip : "-");
    if (ap) {
        boite(2, "CLIENTS", 3, BANC_BLANC, BANC_GRIS_FONCE, "%d connecté(s)", w->st.clients);
    } else if (w->rssi_ok) {
        boite(2, "SIGNAL", 3, BANC_BLANC, BANC_GRIS_FONCE, "%d dBm", w->rssi);
    } else {
        boite(2, "SIGNAL", 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", "-");
    }
    ligne(0, BANC_BLANC, "SSID %s%s", w->st.ssid[0] ? w->st.ssid : "-", w->repli_ap ? " (repli)" : "");
    if (w->st.ip[0]) {
        ligne(1, BANC_BLANC, "http://%s", w->st.ip);
    } else {
        ligne(1, BANC_GRIS, "%s", "Pas encore d'adresse");
    }
    ligne(2, w->compat_ok ? BANC_GRIS : BANC_JAUNE, "C6 %s, hôte %s", w->version_c6, w->version_hote);
    ligne(3, perdu ? BANC_ROUGE : BANC_GRIS, "Lien C6 %s ; vue %s", perdu ? "PERDU" : "ok",
          w->vue_client ? "ouverte" : "fermée");
    if (w->st.error[0]) {
        ligne(4, BANC_JAUNE, "%s", w->st.error);
    } else {
        ligne(4, BANC_GRIS, "Page web : %" PRIu32 " requête(s)", w->requetes);
    }
    ligne(5, BANC_GRIS, "%s", "wifi | wifi sta <ssid> <mdp> | wifi ap");
}
#endif

static void onglet_wifi(void)
{
#if CONFIG_BANC_F_WIFI
    banc_wifi_etat_t w;
    banc_wifi_etat(&w);
    if (w.demarre) {
        onglet_wifi_actif(&w);
        return;
    }
#endif
    const char *t = banc_f_texte(BANC_F_WIFI);
    boite(0, "WI-FI (ESP32-C6)", 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", t ? t : "arrêté");
    boite(1, "ADRESSE", 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", "-");
    boite(2, "CLIENTS / SIGNAL", 3, BANC_GRIS, BANC_GRIS_FONCE, "%s", "-");
    ligne(0, BANC_GRIS, "%s", "Fonction « wifi » non utilisée");
    ligne(1, BANC_GRIS, "%s", "Écran de sélection ou console :");
    ligne(2, BANC_GRIS, "%s", "fonctions on wifi, puis redemarrer");
    for (int i = 3; i < NB_LIGNES; i++) {
        ligne(i, BANC_GRIS, "%s", "");
    }
}


static void dessiner_boutons(const banc_etat_t *e)
{
    zone_ecrire(&s_z_boutons[0], 2, BANC_BLANC, e->arme ? BANC_GRIS_FONCE : BANC_VERT_FONCE,
                e->arme ? "DÉSARMER" : "ARMER");
    zone_ecrire(&s_z_boutons[1], 2, BANC_BLANC, e->rec ? BANC_ROUGE_VIF : BANC_BLEU, e->rec ? "STOP" : "VIDÉO");
    zone_ecrire(&s_z_boutons[2], 2, BANC_BLANC, BANC_BLEU, "PHOTO");
    zone_ecrire(&s_z_boutons[3], 2, BANC_BLANC, BANC_GRIS_FONCE, e->nuit_active ? "JOUR" : "NUIT");
}

static void rafraichir_panneau(int64_t now)
{
    banc_etat_t e;
    banc_etat_lire(&e);
    switch (s_onglet) {
    case BANC_ONGLET_PHOTO:
        onglet_photo(&e, now);
        break;
    case BANC_ONGLET_NUIT:
        onglet_nuit(&e);
        break;
    case BANC_ONGLET_SON:
        onglet_son(&e);
        break;
    case BANC_ONGLET_SYSTEME:
        onglet_systeme(&e);
        break;
    case BANC_ONGLET_WIFI:
        onglet_wifi();
        break;
    default:
        onglet_detection(&e, now);
        break;
    }
    dessiner_boutons(&e);
}

/* ------------------------------------------------------------------------- */
/* Dalle tactile                                                              */
/* ------------------------------------------------------------------------- */

static bool dans(const banc_rect_t *r, uint16_t x, uint16_t y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static void bouton(int i)
{
    banc_etat_t e;
    banc_etat_lire(&e);
    esp_err_t err = ESP_OK;
    switch (i) {
    case 0:
        banc_armer(!e.arme, "tactile");
        break;
    case 1:
        if (e.rec || e.boucle_en_cours) {
            banc_arreter("tactile");
        } else {
            err = banc_demander(BANC_ACTION_VIDEO, CONFIG_BANC_DUREE_S, 0, "tactile");
        }
        break;
    case 2:
        err = banc_demander(BANC_ACTION_PHOTO, 0, 0, "tactile");
        break;
    default:
        banc_nuit_mode(e.nuit_active ? BANC_NUIT_JOUR : BANC_NUIT_NUIT);
        break;
    }
    if (err != ESP_OK) {
        banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_JAUNE, "%s", "Occupé : action en cours, réessayer à la fin");
    }
}

static void toucher(uint16_t x, uint16_t y)
{
    for (int i = 0; i < BANC_NB_ONGLETS; i++) {
        if (dans(&s_r_onglets[i], x, y)) {
            banc_ui_choisir((banc_onglet_t)i);
            return;
        }
    }
    for (int i = 0; i < NB_BOUTONS; i++) {
        if (dans(&s_z_boutons[i].r, x, y)) {
            /* Retour visuel : bouton blanc jusqu'au prochain rafraîchissement du panneau. */
            zone_ecrire(&s_z_boutons[i], 2, BANC_NOIR, BANC_BLANC, s_z_boutons[i].texte);
            s_z_boutons[i].valide = false;
            ESP_LOGI(TAG, "bouton tactile « %s »", s_z_boutons[i].texte);
            bouton(i);
            return;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Tâche                                                                      */
/* ------------------------------------------------------------------------- */

static void mesures_lentes(void)
{
    s_mes.batt_err = banc_batterie(&s_mes.batt_mv, &s_mes.batt_brut_mv);
    s_mes.temp_err = banc_temperature(&s_mes.temp_c);
}

static void tache_ui(void *arg)
{
    (void)arg;
    int64_t prochain_panneau = 0, prochain_entete = 0, prochaines_mesures = 0;
    bool appui = false;
    for (;;) {
        if (s_redessiner) {
            s_redessiner = false;
            dessiner_onglets();
            banc_ecran_rect(&s_contenu, BANC_FOND);
            invalider_tout();
            prochain_panneau = 0;
        }
        uint16_t x = 0, y = 0;
        const bool touche = banc_tactile_lire(&x, &y);
        if (touche && !appui) {
            toucher(x, y);
        }
        appui = touche;

        const int64_t now = esp_timer_get_time();
        if (now >= prochaines_mesures && s_onglet == BANC_ONGLET_SYSTEME) {
            prochaines_mesures = now + MESURES_PERIODE_US;
            mesures_lentes();
        }
        if (now >= prochain_panneau) {
            prochain_panneau = now + PANNEAU_PERIODE_US;
            rafraichir_panneau(now);
        }
        if (now >= prochain_entete) {
            prochain_entete = now + ENTETE_PERIODE_US;
            banc_rafraichir_entete();
        }
        vTaskDelay(pdMS_TO_TICKS(UI_PERIODE_MS));
    }
}

esp_err_t banc_ui_demarrer(void)
{
    if (!banc_ecran_ok()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    disposer();
    mesures_lentes();
    if (xTaskCreate(tache_ui, "banc_ui", UI_PILE, NULL, UI_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

banc_onglet_t banc_ui_onglet(void)
{
    return s_onglet;
}

void banc_ui_choisir(banc_onglet_t onglet)
{
    if (onglet >= BANC_NB_ONGLETS || onglet == s_onglet) {
        return;
    }
    s_onglet = onglet;
    s_redessiner = true;
}

const char *banc_ui_nom_onglet(banc_onglet_t onglet)
{
    return onglet < BANC_NB_ONGLETS ? s_noms_onglets[onglet] : "?";
}
