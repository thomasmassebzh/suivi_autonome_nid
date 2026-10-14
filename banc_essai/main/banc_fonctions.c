/*
 * banc_fonctions — choix des fonctions à tester
 *
 * Trois niveaux :
 *  1. compilation : menu « Banc d'essai › Fonctions activées » (une option par
 *     fonction) ; une fonction retirée n'est jamais proposée ni initialisée ;
 *  2. démarrage : écran de sélection (tactile, bouton BOOT, console
 *     « fonctions »), démarrage automatique après CONFIG_BANC_SELECTION_S
 *     secondes sans action, sélection mémorisée en NVS (espace « banc ») ;
 *  3. initialisation : une fonction choisie dont l'initialisation échoue est
 *     marquée « en défaut » ; les autres continuent.
 * Une fonction dont dépend une autre (caméra pour le son, caméra et carte SD
 * pour la photo de suivi, écran pour le tactile) la désactive avec elle.
 *
 * L'écran et le tactile servent à la sélection : ils sont initialisés avant
 * elle s'ils étaient choisis au démarrage précédent ; désactivés pendant la
 * sélection, ils sont éteints ensuite. La console, qui sert aussi à la
 * sélection, est démarrée avant : la désactiver prend effet au démarrage suivant.
 */
#include "banc.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "banc_fonctions";

#define NVS_ESPACE          "banc"
#define NVS_CLE             "fonctions"
#define PERIODE_MS          20
#define APPUI_LONG_US       (1000LL * 1000LL)
#define PROLONGATION_US     (10LL * 1000000LL)  /* après une action : 10 s avant le démarrage automatique */
#define LIGNE_SEL_H         34

#ifdef CONFIG_BANC_F_ECRAN
#define C_ECRAN 1
#else
#define C_ECRAN 0
#endif
#ifdef CONFIG_BANC_TACTILE
#define C_TACTILE 1
#else
#define C_TACTILE 0
#endif
#ifdef CONFIG_BANC_F_CAMERA
#define C_CAMERA 1
#else
#define C_CAMERA 0
#endif
#ifdef CONFIG_BANC_SON
#define C_SON 1
#else
#define C_SON 0
#endif
#ifdef CONFIG_BANC_F_SD
#define C_SD 1
#else
#define C_SD 0
#endif
#ifdef CONFIG_BANC_F_PIR
#define C_PIR 1
#else
#define C_PIR 0
#endif
#ifdef CONFIG_BANC_F_NUIT
#define C_NUIT 1
#else
#define C_NUIT 0
#endif
#ifdef CONFIG_BANC_F_BATTERIE
#define C_BATTERIE 1
#else
#define C_BATTERIE 0
#endif
#ifdef CONFIG_BANC_F_TEMPERATURE
#define C_TEMPERATURE 1
#else
#define C_TEMPERATURE 0
#endif
#ifdef CONFIG_BANC_F_SUIVI
#define C_SUIVI 1
#else
#define C_SUIVI 0
#endif
#ifdef CONFIG_BANC_F_CONSOLE
#define C_CONSOLE 1
#else
#define C_CONSOLE 0
#endif

#define FBIT(f)              (1u << (f))

typedef struct {
    const char *nom;            /*!< console */
    const char *libelle;        /*!< écran */
    bool        compilee;
    uint32_t    depend;         /*!< fonctions requises */
} fonction_t;

static const fonction_t s_f[BANC_NB_F] = {
    [BANC_F_ECRAN]       = { "ecran", "Écran (tableau de bord)", C_ECRAN, 0 },
    [BANC_F_TACTILE]     = { "tactile", "Dalle tactile", C_TACTILE, FBIT(BANC_F_ECRAN) },
    [BANC_F_CAMERA]      = { "camera", "Caméra, vidéo, photo", C_CAMERA, 0 },
    [BANC_F_SON]         = { "son", "Son (micro, AAC, vumètre)", C_SON, FBIT(BANC_F_CAMERA) },
    [BANC_F_SD]          = { "sd", "Carte SD", C_SD, 0 },
    [BANC_F_PIR]         = { "pir", "PIR et détection", C_PIR, 0 },
    [BANC_F_NUIT]        = { "nuit", "Nuit (LED IR, IR-cut, mono)", C_NUIT, 0 },
    [BANC_F_BATTERIE]    = { "batterie", "Batterie (ADC)", C_BATTERIE, 0 },
    [BANC_F_TEMPERATURE] = { "temperature", "Température de la puce", C_TEMPERATURE, 0 },
    [BANC_F_SUIVI]       = { "suivi", "Photo de suivi", C_SUIVI, FBIT(BANC_F_CAMERA) | FBIT(BANC_F_SD) },
    [BANC_F_CONSOLE]     = { "console", "Console série", C_CONSOLE, 0 },
};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_choix;            /* fonctions sélectionnées */
static uint32_t s_defaut;           /* fonctions en défaut */
static volatile bool s_selection;   /* écran de sélection en cours */
static volatile bool s_valider;
static volatile bool s_change;      /* sélection modifiée (console) : redessiner, prolonger */

static uint32_t masque_compile(void)
{
    uint32_t m = 0;
    for (int f = 0; f < BANC_NB_F; f++) {
        if (s_f[f].compilee) {
            m |= FBIT(f);
        }
    }
    return m;
}

/* ------------------------------------------------------------------------- */
/* État                                                                       */
/* ------------------------------------------------------------------------- */

void banc_fonctions_charger(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    uint32_t m = masque_compile();
    nvs_handle_t h;
    if (err == ESP_OK && nvs_open(NVS_ESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint32_t lu = 0;
        if (nvs_get_u32(h, NVS_CLE, &lu) == ESP_OK) {
            m = lu;
        }
        nvs_close(h);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS indisponible (%s) : toutes les fonctions compilées", esp_err_to_name(err));
    }
    portENTER_CRITICAL(&s_mux);
    s_choix = m & masque_compile();
    portEXIT_CRITICAL(&s_mux);
}

static void enregistrer(void)
{
    portENTER_CRITICAL(&s_mux);
    const uint32_t m = s_choix;
    portEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    if (nvs_open(NVS_ESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    uint32_t avant = 0;
    if (nvs_get_u32(h, NVS_CLE, &avant) != ESP_OK || avant != m) {
        if (nvs_set_u32(h, NVS_CLE, m) == ESP_OK) {
            nvs_commit(h);
        }
    }
    nvs_close(h);
}

bool banc_f_choisie(banc_fonction_t f)
{
    if (f >= BANC_NB_F) {
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    const bool c = (s_choix & FBIT(f)) != 0;
    portEXIT_CRITICAL(&s_mux);
    return c && s_f[f].compilee;
}

banc_fonction_etat_t banc_f_etat(banc_fonction_t f)
{
    if (f >= BANC_NB_F || !s_f[f].compilee) {
        return BANC_FE_NON_COMPILEE;
    }
    portENTER_CRITICAL(&s_mux);
    const uint32_t choix = s_choix;
    const uint32_t defaut = s_defaut;
    portEXIT_CRITICAL(&s_mux);
    if (!(choix & FBIT(f))) {
        return BANC_FE_DESACTIVEE;
    }
    for (int d = 0; d < BANC_NB_F; d++) {
        if ((s_f[f].depend & FBIT(d)) && banc_f_etat((banc_fonction_t)d) != BANC_FE_ACTIVE) {
            return BANC_FE_DESACTIVEE;      /* fonction requise absente */
        }
    }
    return (defaut & FBIT(f)) ? BANC_FE_DEFAUT : BANC_FE_ACTIVE;
}

bool banc_f_active(banc_fonction_t f)
{
    return banc_f_etat(f) == BANC_FE_ACTIVE;
}

void banc_f_defaut(banc_fonction_t f, esp_err_t err)
{
    if (f >= BANC_NB_F) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_defaut |= FBIT(f);
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "fonction « %s » EN DÉFAUT (%s) : les autres fonctions continuent", s_f[f].libelle,
             esp_err_to_name(err));
}

void banc_f_retablir(banc_fonction_t f)
{
    if (f >= BANC_NB_F) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    const bool etait = (s_defaut & FBIT(f)) != 0;
    s_defaut &= ~FBIT(f);
    portEXIT_CRITICAL(&s_mux);
    if (etait) {
        ESP_LOGI(TAG, "fonction « %s » rétablie", s_f[f].libelle);
    }
}

const char *banc_f_texte(banc_fonction_t f)
{
    switch (banc_f_etat(f)) {
    case BANC_FE_ACTIVE:
        return NULL;
    case BANC_FE_DEFAUT:
        return "en défaut";
    case BANC_FE_NON_COMPILEE:
        return "non compilé";
    default:
        return "désactivé";
    }
}

const char *banc_f_nom(banc_fonction_t f)
{
    return f < BANC_NB_F ? s_f[f].nom : "?";
}

const char *banc_f_libelle(banc_fonction_t f)
{
    return f < BANC_NB_F ? s_f[f].libelle : "?";
}

esp_err_t banc_f_choisir(banc_fonction_t f, bool choisie)
{
    if (f >= BANC_NB_F) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_f[f].compilee) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    portENTER_CRITICAL(&s_mux);
    if (choisie) {
        s_choix |= FBIT(f);
    } else {
        s_choix &= ~FBIT(f);
    }
    portEXIT_CRITICAL(&s_mux);
    s_change = true;
    if (!s_selection) {
        enregistrer();      /* après le démarrage : pris en compte au prochain démarrage */
    }
    return ESP_OK;
}

void banc_fonctions_valider(void)
{
    s_valider = true;
}

bool banc_fonctions_en_selection(void)
{
    return s_selection;
}

/* ------------------------------------------------------------------------- */
/* Écran de sélection                                                         */
/* ------------------------------------------------------------------------- */

/** Éléments navigables : les fonctions compilées, puis « DÉMARRER ». */
static int s_elements[BANC_NB_F + 1];
static int s_nb_elements;

#define ELEMENT_DEMARRER    (-1)

static void texte_element(int f, char *out, size_t len)
{
    const banc_fonction_etat_t e = banc_f_etat((banc_fonction_t)f);
    const bool choisie = banc_f_choisie((banc_fonction_t)f);
    snprintf(out, len, "[%c] %s%s", choisie ? 'X' : ' ', s_f[f].libelle,
             (choisie && e == BANC_FE_DESACTIVEE) ? " (dépendance)" : "");
}

static void journal_liste(int curseur)
{
    printf("\nFonctions à tester (BOOT court : suivante, BOOT long : basculer ; console : fonctions on|off <nom>, "
           "fonctions go) :\n");
    for (int i = 0; i < s_nb_elements; i++) {
        const int f = s_elements[i];
        if (f == ELEMENT_DEMARRER) {
            printf(" %s DÉMARRER\n", i == curseur ? ">" : " ");
            continue;
        }
        char t[64];
        texte_element(f, t, sizeof(t));
        printf(" %s %-12s %s\n", i == curseur ? ">" : " ", s_f[f].nom, t);
    }
}

/** tout : cadre et consignes ; lignes : liste des fonctions ; toujours : bouton DÉMARRER (compte à rebours). */
static void dessiner(int curseur, int64_t reste_us, bool tout, bool lignes)
{
    if (!banc_ecran_ok() || !banc_f_choisie(BANC_F_ECRAN)) {
        return;
    }
    const banc_zones_t *z = banc_ecran_zones();
    if (tout) {
        banc_ecran_texte(&z->onglets, 2, BANC_BLANC, BANC_BLEU, true, "BANC D'ESSAI — FONCTIONS À TESTER");
        banc_ecran_rect(&z->vue, BANC_FOND);
        banc_ecran_rect(&z->panneau, BANC_FOND);
        banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_BLANC, "%s", "Toucher une ligne pour la basculer, puis DÉMARRER");
        banc_ecran_ligne(BANC_LIGNE_DETAIL1, BANC_GRIS, "%s",
                         "Bouton BOOT : appui court = ligne suivante, appui long = basculer / démarrer");
        banc_ecran_ligne(BANC_LIGNE_DETAIL2, BANC_GRIS, "%s",
                         "Console : fonctions | fonctions on|off <nom> | fonctions go");
        banc_ecran_ligne(BANC_LIGNE_DETAIL3, BANC_GRIS, "%s", "Sélection mémorisée pour les démarrages suivants");
    }
    for (int i = 0; i < s_nb_elements && (tout || lignes); i++) {
        const int f = s_elements[i];
        if (f == ELEMENT_DEMARRER) {
            continue;
        }
        const banc_rect_t r = { z->vue.x, (uint16_t)(z->vue.y + i * LIGNE_SEL_H), z->vue.w, LIGNE_SEL_H - 2 };
        char t[72];
        texte_element(f, t, sizeof(t));
        const bool choisie = banc_f_choisie((banc_fonction_t)f);
        banc_ecran_texte(&r, 2, choisie ? BANC_BLANC : BANC_GRIS,
                         i == curseur ? BANC_BLEU : (choisie ? BANC_VERT_FONCE : BANC_GRIS_FONCE), false, t);
    }
    const banc_rect_t b = { z->panneau.x, (uint16_t)(z->panneau.y + 40), z->panneau.w, 140 };
    char t[48];
    snprintf(t, sizeof(t), "DÉMARRER\n(%" PRId64 " s)", (reste_us + 999999) / 1000000);
    const bool sur_demarrer = (s_elements[curseur] == ELEMENT_DEMARRER);
    banc_ecran_texte(&b, 3, BANC_BLANC, sur_demarrer ? BANC_BLEU : BANC_VERT_FONCE, true, t);
}

/** Élément touché : index dans s_elements, ou -1. */
static int element_touche(uint16_t x, uint16_t y)
{
    const banc_zones_t *z = banc_ecran_zones();
    const banc_rect_t b = { z->panneau.x, (uint16_t)(z->panneau.y + 40), z->panneau.w, 140 };
    if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
        return s_nb_elements - 1;
    }
    if (x >= z->vue.x && x < z->vue.x + z->vue.w && y >= z->vue.y) {
        const int i = (y - z->vue.y) / LIGNE_SEL_H;
        if (i < s_nb_elements - 1) {
            return i;
        }
    }
    return -1;
}

static void basculer(int f)
{
    banc_f_choisir((banc_fonction_t)f, !banc_f_choisie((banc_fonction_t)f));
}

void banc_fonctions_selection(void)
{
    s_nb_elements = 0;
    for (int f = 0; f < BANC_NB_F; f++) {
        if (s_f[f].compilee) {
            s_elements[s_nb_elements++] = f;
        }
    }
    s_elements[s_nb_elements++] = ELEMENT_DEMARRER;

    if (CONFIG_BANC_SELECTION_S > 0) {
        s_selection = true;
        int curseur = s_nb_elements - 1;    /* sur « DÉMARRER » */
        int64_t fin = esp_timer_get_time() + CONFIG_BANC_SELECTION_S * 1000000LL;
        int64_t prochain_dessin = 0;
        int64_t t_appui = -1;
        bool long_traite = false;
        bool touche_avant = false;
        int stable = gpio_get_level(CONFIG_BANC_BOUTON_GPIO);
        int compte = 0;
        dessiner(curseur, fin - esp_timer_get_time(), true, true);
        journal_liste(curseur);
        while (!s_valider && esp_timer_get_time() < fin) {
            vTaskDelay(pdMS_TO_TICKS(PERIODE_MS));
            const int64_t now = esp_timer_get_time();
            bool action = false;

            /* Bouton BOOT (anti-rebond de 3 lectures) */
            const int niveau = gpio_get_level(CONFIG_BANC_BOUTON_GPIO);
            if (niveau != stable) {
                if (++compte >= 3) {
                    stable = niveau;
                    compte = 0;
                    if (stable == 0) {
                        t_appui = now;
                        long_traite = false;
                    } else if (t_appui >= 0 && !long_traite) {
                        curseur = (curseur + 1) % s_nb_elements;   /* appui court : suivant */
                        action = true;
                    }
                    if (stable != 0) {
                        t_appui = -1;
                    }
                }
            } else {
                compte = 0;
            }
            if (stable == 0 && t_appui >= 0 && !long_traite && now - t_appui >= APPUI_LONG_US) {
                long_traite = true;
                action = true;
                if (s_elements[curseur] == ELEMENT_DEMARRER) {
                    s_valider = true;
                } else {
                    basculer(s_elements[curseur]);
                }
            }

            /* Dalle tactile */
            uint16_t x = 0, y = 0;
            const bool touche = banc_f_choisie(BANC_F_TACTILE) && banc_tactile_lire(&x, &y);
            if (touche && !touche_avant) {
                const int i = element_touche(x, y);
                if (i >= 0) {
                    curseur = i;
                    action = true;
                    if (s_elements[i] == ELEMENT_DEMARRER) {
                        s_valider = true;
                    } else {
                        basculer(s_elements[i]);
                    }
                }
            }
            touche_avant = touche;

            /* Console (« fonctions on|off <nom> ») */
            if (s_change) {
                s_change = false;
                action = true;
            }
            if (action) {
                fin = now + PROLONGATION_US;
                journal_liste(curseur);
                dessiner(curseur, fin - now, false, true);
                prochain_dessin = now + 250000;
            } else if (now >= prochain_dessin) {
                prochain_dessin = now + 250000;
                dessiner(curseur, fin - now, false, false);
            }
        }
        /* Bouton relâché avant de rendre la main (la tâche du bouton le verrait comme un appui). */
        while (gpio_get_level(CONFIG_BANC_BOUTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(PERIODE_MS));
        }
        s_selection = false;
    }
    enregistrer();
    printf("\nFonctions :");
    for (int f = 0; f < BANC_NB_F; f++) {
        const char *t = banc_f_texte((banc_fonction_t)f);
        if (s_f[f].compilee) {
            printf(" %s=%s", s_f[f].nom, t ? t : "oui");
        }
    }
    printf("\n");
}
