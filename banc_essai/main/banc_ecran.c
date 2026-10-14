/*
 * banc_ecran — écran de débogage du banc d'essai (MIPI-DSI, sans BSP ni LVGL)
 *
 * Adapté des exemples d'ESP-IDF v6.1 :
 *  - examples/peripherals/camera/common_components/dsi_init (Apache-2.0) : bus
 *    DSI 2 voies, interface DBI des commandes, panneau DPI EK79007 / ILI9881C,
 *    un seul tampon d'image ;
 *  - examples/peripherals/lcd/mipi_dsi (CC0) : alimentation du PHY MIPI par le
 *    LDO interne VO3 (2,5 V), rétroéclairage ;
 * et du BSP esp32_p4_function_ev_board d'esp-bsp (Apache-2.0) : débit des voies
 * (1 Gbit/s), temporisations des panneaux (macros des pilotes), broches de
 * rétroéclairage (GPIO26) et de reset (GPIO27) de l'écran 7 pouces du kit.
 *
 * Mise en page (écran 1024x600 en paysage ; en portrait, panneau sous la vue) :
 *
 *   +----------------------------------------------------------------+
 *   | DÉTECTION | PHOTO | NUIT | SON | SYSTÈME            (onglets)   |
 *   +--------------------------------------+-------------------------+
 *   |                                      | panneau de l'onglet      |
 *   |   vue caméra (PPA : YUV → RGB565)    | (pastilles, compteurs)   |
 *   |                                      +-------------------------+
 *   |                                      | ARMER VIDÉO PHOTO NUIT  |
 *   +--------------------------------------+-------------------------+
 *   | bandeau : en-tête, état, 3 lignes de détail                     |
 *   +----------------------------------------------------------------+
 *
 * Texte : police 5x7 de l'incrustation (nid_overlay), agrandie ×2 à ×4,
 * dessinée par le processeur directement dans le tampon d'image (PSRAM), que
 * le contrôleur DPI balaie en continu : une image peut se déchirer, sans
 * importance pour un écran de contrôle.
 *
 * LDO VO3 : la caméra l'acquiert aussi (esp_video, même canal, même tension
 * 2,5 V, non réglable). Le pilote LDO compte les références : l'arrêt de la
 * caméra ne coupe pas l'écran, et inversement.
 *
 * Concurrence : un mutex protège le tampon d'image (texte écrit par le
 * processeur, images écrites par le PPA). Le texte est réécrit en mémoire
 * (cache C2M) avant de rendre le mutex : le PPA, qui invalide le cache sur sa
 * zone de sortie, ne peut pas effacer une écriture du processeur en attente.
 * Pendant un enregistrement, la vue ne prend le mutex que s'il est libre : la
 * tâche d'enregistrement n'attend jamais l'écran.
 */
#include "banc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"

#if !CONFIG_BANC_ECRAN_AUCUN

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nid_overlay.h"
#if CONFIG_BANC_ECRAN_EK79007
#include "esp_lcd_ek79007.h"
#else
#include "esp_lcd_ili9881c.h"
#endif

static const char *TAG = "banc_ecran";

#define DSI_VOIES               2
#define DSI_DEBIT_MBPS          1000    /* BSP : BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS */
#define PHY_LDO_CANAL           3       /* LDO_VO3 → VDD_MIPI_DPHY (schéma de la P4X) */
#define PHY_LDO_MV              2500

#define ECHELLE_TEXTE           2
#define ECHELLE_MESSAGE         3
#define CAR_L(e)                (NID_OVERLAY_CELL_W * (e))
#define CAR_H(e)                (NID_OVERLAY_CELL_H * (e))
#define LIGNE_H                 (CAR_H(ECHELLE_TEXTE) + 4)
#define MARGE_X                 8
#define MARGE_TEXTE             6
#define SEPARATION_H            2
#define BANDEAU_H               (SEPARATION_H + 8 + BANC_NB_LIGNES * LIGNE_H)
#define ONGLETS_H               44
#define BOUTONS_H               76
#define PANNEAU_L               420     /* largeur du panneau en paysage */
#define ESPACE                  4
#define ECHELLE_VUE_MAX         32      /* agrandissement max de la vue : 32/16 = ×2 */
#define TEXTE_MAX               192

#define RGB565(r, g, b)         ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define FOND_VUE                RGB565(0, 0, 0)
#define COULEUR_SEPARATION      RGB565(90, 90, 90)

static const uint16_t s_couleurs[BANC_NB_COULEURS] = {
    [BANC_BLANC]      = RGB565(255, 255, 255),
    [BANC_GRIS]       = RGB565(150, 150, 150),
    [BANC_VERT]       = RGB565(70, 230, 70),
    [BANC_JAUNE]      = RGB565(255, 200, 0),
    [BANC_ROUGE]      = RGB565(255, 70, 70),
    [BANC_NOIR]       = RGB565(0, 0, 0),
    [BANC_FOND]       = RGB565(16, 16, 28),
    [BANC_GRIS_FONCE] = RGB565(60, 60, 70),
    [BANC_VERT_FONCE] = RGB565(0, 120, 40),
    [BANC_ROUGE_VIF]  = RGB565(220, 0, 0),
    [BANC_ORANGE]     = RGB565(230, 120, 0),
    [BANC_BLEU]       = RGB565(30, 90, 200),
};

static struct {
    bool                      ok;
    esp_ldo_channel_handle_t  ldo;
    esp_lcd_dsi_bus_handle_t  bus;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t    panel;
    ppa_client_handle_t       ppa;
    SemaphoreHandle_t         mutex;
    StaticSemaphore_t         mutex_buf;
    uint16_t                 *fb;           /*!< tampon d'image RGB565 (PSRAM) */
    size_t                    fb_size;
    uint16_t                  w, h;         /*!< taille de l'écran */
    banc_zones_t              zones;
    uint16_t                  img_w, img_h; /*!< taille de la dernière image affichée */
    bool                      vue_sale;     /*!< zone de vue à effacer avant la prochaine image */
    bool                      erreur_ppa;   /*!< erreur PPA déjà signalée */
    int64_t                   t_vue_us;     /*!< dernière image affichée pendant un enregistrement */
    char                      nom[32];
} s_ec = {
    .nom = "aucun",
};

/* ------------------------------------------------------------------------- */
/* Texte : police 5x7 de l'incrustation, accents du français composés         */
/* ------------------------------------------------------------------------- */

typedef enum { ACC_AUCUN = 0, ACC_AIGU, ACC_GRAVE, ACC_CIRC, ACC_TREMA, ACC_CEDILLE } accent_t;

typedef struct {
    uint16_t code;      /*!< point de code Unicode */
    char     base;      /*!< caractère ASCII de base */
    uint8_t  accent;
} translit_t;

/* Lettres accentuées du français et quelques signes typographiques. Les
 * capitales accentuées sont affichées sans accent (place insuffisante). */
static const translit_t s_translit[] = {
    { 0x00E0, 'a', ACC_GRAVE }, { 0x00E2, 'a', ACC_CIRC }, { 0x00E4, 'a', ACC_TREMA },
    { 0x00E7, 'c', ACC_CEDILLE }, { 0x00E8, 'e', ACC_GRAVE }, { 0x00E9, 'e', ACC_AIGU },
    { 0x00EA, 'e', ACC_CIRC }, { 0x00EB, 'e', ACC_TREMA }, { 0x00EE, 'i', ACC_CIRC },
    { 0x00EF, 'i', ACC_TREMA }, { 0x00F4, 'o', ACC_CIRC }, { 0x00F6, 'o', ACC_TREMA },
    { 0x00F9, 'u', ACC_GRAVE }, { 0x00FB, 'u', ACC_CIRC }, { 0x00FC, 'u', ACC_TREMA },
    { 0x00FF, 'y', ACC_TREMA }, { 0x00C0, 'A', 0 }, { 0x00C2, 'A', 0 }, { 0x00C7, 'C', ACC_CEDILLE },
    { 0x00C8, 'E', 0 }, { 0x00C9, 'E', 0 }, { 0x00CA, 'E', 0 }, { 0x00CB, 'E', 0 },
    { 0x00CE, 'I', 0 }, { 0x00CF, 'I', 0 }, { 0x00D4, 'O', 0 }, { 0x00D9, 'U', 0 },
    { 0x00DB, 'U', 0 }, { 0x00DC, 'U', 0 }, { 0x0153, 'o', 0 }, { 0x00A0, ' ', 0 },
    { 0x202F, ' ', 0 }, { 0x2013, '-', 0 }, { 0x2014, '-', 0 }, { 0x2192, '>', 0 },
    { 0x00AB, '"', 0 }, { 0x00BB, '"', 0 }, { 0x2019, '\'', 0 }, { 0x2026, '.', 0 },
    { 0x00B0, 'o', 0 }, { 0x00D7, 'x', 0 }, { 0x2265, '>', 0 }, { 0x2264, '<', 0 },
    { 0x00B5, 'u', 0 },
};

/** Point de code UTF-8 suivant ; avance *p (séquence invalide : « ? »). */
static uint32_t utf8_suivant(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = s[0];
    int n;
    if (c < 0x80) {
        n = 0;
    } else if ((c & 0xE0) == 0xC0) {
        c &= 0x1F;
        n = 1;
    } else if ((c & 0xF0) == 0xE0) {
        c &= 0x0F;
        n = 2;
    } else if ((c & 0xF8) == 0xF0) {
        c &= 0x07;
        n = 3;
    } else {
        *p += 1;
        return '?';
    }
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *p += i;
            return '?';
        }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *p += n + 1;
    return c;
}

/** Glyphe 6x9 du caractère (bit 4 = colonne de gauche), accent composé au besoin. */
static void glyphe(uint32_t code, uint8_t g[NID_OVERLAY_CELL_H])
{
    char base = '?';
    uint8_t accent = ACC_AUCUN;
    if (code >= 0x20 && code <= 0x7E) {
        base = (char)code;
    } else {
        for (size_t i = 0; i < sizeof(s_translit) / sizeof(s_translit[0]); i++) {
            if (s_translit[i].code == code) {
                base = s_translit[i].base;
                accent = s_translit[i].accent;
                break;
            }
        }
    }
    memcpy(g, nid_overlay_glyph(base), NID_OVERLAY_CELL_H);
    /* Minuscules : corps à partir de la ligne 2, accent sur les lignes 0 et 1 ;
     * cédille sur les deux lignes de jambage. */
    switch (accent) {
    case ACC_AIGU:    g[0] = 0x02; g[1] = 0x04; break;
    case ACC_GRAVE:   g[0] = 0x08; g[1] = 0x04; break;
    case ACC_CIRC:    g[0] = 0x04; g[1] = 0x0A; break;
    case ACC_TREMA:   g[0] = 0x00; g[1] = 0x0A; break;
    case ACC_CEDILLE: g[7] = 0x04; g[8] = 0x08; break;
    default: break;
    }
}

/** Nombre de caractères affichés (et non d'octets) jusqu'à la fin ou au premier « \n ». */
static size_t longueur_ligne(const char *p)
{
    size_t n = 0;
    while (*p != '\0' && *p != '\n') {
        utf8_suivant(&p);
        n++;
    }
    return n;
}

static void remplir(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t couleur)
{
    for (uint32_t r = y; r < y + h && r < s_ec.h; r++) {
        uint16_t *p = s_ec.fb + (size_t)r * s_ec.w + x;
        for (uint32_t c = 0; c < w && x + c < s_ec.w; c++) {
            p[c] = couleur;
        }
    }
}

/**
 * Écrit une ligne de texte à partir de (x, y), sans dépasser x_max ni y_max
 * (caractères coupés) ; s'arrête à la fin ou au premier « \n ».
 */
static void dessiner_ligne(uint32_t x, uint32_t y, const char *texte, uint16_t encre, uint16_t fond,
                           uint32_t x_max, uint32_t y_max, int e)
{
    const char *p = texte;
    uint32_t cx = x;
    while (*p != '\0' && *p != '\n' && cx + CAR_L(e) <= x_max) {
        uint8_t g[NID_OVERLAY_CELL_H];
        glyphe(utf8_suivant(&p), g);
        for (int r = 0; r < NID_OVERLAY_CELL_H; r++) {
            for (int sy = 0; sy < e; sy++) {
                const uint32_t py = y + (uint32_t)(r * e + sy);
                if (py >= y_max || py >= s_ec.h) {
                    break;
                }
                uint16_t *px = s_ec.fb + (size_t)py * s_ec.w + cx;
                for (int c = 0; c < NID_OVERLAY_CELL_W; c++) {
                    const uint16_t v = (c < 5 && (g[r] & (0x10 >> c))) ? encre : fond;
                    for (int sx = 0; sx < e; sx++) {
                        *px++ = v;
                    }
                }
            }
        }
        cx += CAR_L(e);
    }
}

/** Écrit en mémoire (cache → PSRAM) les lignes y0..y1 du tampon, lues par le contrôleur DPI. */
static void publier(uint32_t y0, uint32_t y1)
{
    if (y1 > s_ec.h) {
        y1 = s_ec.h;
    }
    if (y0 >= y1) {
        return;
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_ec.panel, 0, (int)y0, s_ec.w, (int)y1, s_ec.fb + (size_t)y0 * s_ec.w);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rafraîchissement de l'écran : %s", esp_err_to_name(err));
    }
}

static void effacer_vue(void)
{
    const banc_rect_t *v = &s_ec.zones.vue;
    remplir(v->x, v->y, v->w, v->h, FOND_VUE);
    publier(v->y, (uint32_t)v->y + v->h);
}

/* ------------------------------------------------------------------------- */
/* Initialisation                                                             */
/* ------------------------------------------------------------------------- */

static void liberer(void)
{
    if (s_ec.ppa != NULL) {
        ppa_unregister_client(s_ec.ppa);
        s_ec.ppa = NULL;
    }
    if (s_ec.panel != NULL) {
        esp_lcd_panel_del(s_ec.panel);
        s_ec.panel = NULL;
    }
    if (s_ec.io != NULL) {
        esp_lcd_panel_io_del(s_ec.io);
        s_ec.io = NULL;
    }
    if (s_ec.bus != NULL) {
        esp_lcd_del_dsi_bus(s_ec.bus);
        s_ec.bus = NULL;
    }
    if (s_ec.ldo != NULL) {
        esp_ldo_release_channel(s_ec.ldo);
        s_ec.ldo = NULL;
    }
    s_ec.fb = NULL;
}

/** Bus DSI, interface de commande DBI et panneau DPI (exemple dsi_init, BSP). */
static esp_err_t creer_panneau(void)
{
    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = DSI_VOIES,
        .lane_bit_rate_mbps = DSI_DEBIT_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &s_ec.bus), TAG, "bus MIPI-DSI");

    const esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_ec.bus, &dbi_cfg, &s_ec.io), TAG, "interface de commande DBI");

#if CONFIG_BANC_ECRAN_EK79007
    esp_lcd_dpi_panel_config_t dpi_cfg = EK79007_1024_600_PANEL_60HZ_CONFIG_CF(LCD_COLOR_FMT_RGB565);
    dpi_cfg.num_fbs = 1;
    ek79007_vendor_config_t vendor = {
        .mipi_config = {
            .dsi_bus = s_ec.bus,
            .dpi_config = &dpi_cfg,
            .lane_num = DSI_VOIES,
        },
    };
    const esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = CONFIG_BANC_ECRAN_RESET_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ek79007(s_ec.io, &dev_cfg, &s_ec.panel), TAG, "panneau EK79007");
    snprintf(s_ec.nom, sizeof(s_ec.nom), "EK79007 %ux%u", (unsigned)dpi_cfg.video_timing.h_size,
             (unsigned)dpi_cfg.video_timing.v_size);
#else
    esp_lcd_dpi_panel_config_t dpi_cfg = ILI9881C_800_1280_PANEL_60HZ_DPI_CONFIG_CF(LCD_COLOR_FMT_RGB565);
    dpi_cfg.num_fbs = 1;
    ili9881c_vendor_config_t vendor = {
        .mipi_config = {
            .dsi_bus = s_ec.bus,
            .dpi_config = &dpi_cfg,
            .lane_num = DSI_VOIES,
        },
    };
    const esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = CONFIG_BANC_ECRAN_RESET_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9881c(s_ec.io, &dev_cfg, &s_ec.panel), TAG, "panneau ILI9881C");
    snprintf(s_ec.nom, sizeof(s_ec.nom), "ILI9881C %ux%u", (unsigned)dpi_cfg.video_timing.h_size,
             (unsigned)dpi_cfg.video_timing.v_size);
#endif
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_ec.panel), TAG, "reset du panneau");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_ec.panel), TAG, "initialisation du panneau");
#if CONFIG_BANC_ECRAN_ILI9881C
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_ec.panel, true), TAG, "allumage du panneau");
#endif

    void *fb = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_ec.panel, 1, &fb), TAG, "tampon d'image");
    s_ec.fb = (uint16_t *)fb;
    s_ec.w = (uint16_t)dpi_cfg.video_timing.h_size;
    s_ec.h = (uint16_t)dpi_cfg.video_timing.v_size;
    s_ec.fb_size = (size_t)s_ec.w * s_ec.h * sizeof(uint16_t);
    return ESP_OK;
}

/** Zones de l'écran : paysage (panneau à droite de la vue) ou portrait (panneau sous la vue). */
static void calculer_zones(void)
{
    banc_zones_t *z = &s_ec.zones;
    const uint16_t w = s_ec.w;
    const uint16_t h = s_ec.h;
    z->onglets = (banc_rect_t){ 0, 0, w, ONGLETS_H };
    z->bandeau = (banc_rect_t){ 0, (uint16_t)(h - BANDEAU_H), w, BANDEAU_H };
    const uint16_t haut = ONGLETS_H + ESPACE;
    const uint16_t bas = (uint16_t)(z->bandeau.y - ESPACE);
    if (w >= h) {
        z->panneau = (banc_rect_t){ (uint16_t)(w - PANNEAU_L), haut, PANNEAU_L, (uint16_t)(bas - haut) };
        z->vue = (banc_rect_t){ 0, haut, (uint16_t)(w - PANNEAU_L - ESPACE), (uint16_t)(bas - haut) };
    } else {
        const uint16_t vh = (uint16_t)(w * 9u / 16u);
        z->vue = (banc_rect_t){ 0, haut, w, vh };
        const uint16_t py = (uint16_t)(haut + vh + ESPACE);
        z->panneau = (banc_rect_t){ 0, py, w, (uint16_t)(bas - py) };
    }
    z->boutons = (banc_rect_t){ z->panneau.x, (uint16_t)(z->panneau.y + z->panneau.h - BOUTONS_H), z->panneau.w,
                                BOUTONS_H };
}

esp_err_t banc_ecran_init(void)
{
    if (s_ec.ok) {
        return ESP_OK;
    }
    if (s_ec.mutex == NULL) {
        s_ec.mutex = xSemaphoreCreateMutexStatic(&s_ec.mutex_buf);
    }

    /* Rétroéclairage éteint pendant l'initialisation (nid_board_init_early l'a déjà mis au repos). */
#if CONFIG_BANC_ECRAN_RETRO_GPIO >= 0
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_BANC_ECRAN_RETRO_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "GPIO du rétroéclairage");
    gpio_set_level(CONFIG_BANC_ECRAN_RETRO_GPIO, 0);
#endif

    /* Alimentation du PHY MIPI D-PHY (partagée avec la caméra, voir l'en-tête). */
    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = PHY_LDO_CANAL,
        .voltage_mv = PHY_LDO_MV,
    };
    esp_err_t err = esp_ldo_acquire_channel(&ldo_cfg, &s_ec.ldo);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LDO VO3 du PHY MIPI : %s", esp_err_to_name(err));
        s_ec.ldo = NULL;
        return err;
    }
    err = creer_panneau();
    if (err == ESP_OK) {
        const ppa_client_config_t ppa_cfg = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        err = ppa_register_client(&ppa_cfg, &s_ec.ppa);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "PPA (conversion des images) : %s", esp_err_to_name(err));
            s_ec.ppa = NULL;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "écran indisponible : %s (le banc continue sans écran)", esp_err_to_name(err));
        liberer();
        strlcpy(s_ec.nom, "aucun (échec)", sizeof(s_ec.nom));
        return err;
    }

    calculer_zones();
    const banc_zones_t *z = &s_ec.zones;
    remplir(0, 0, s_ec.w, s_ec.h, s_couleurs[BANC_FOND]);
    remplir(z->vue.x, z->vue.y, z->vue.w, z->vue.h, FOND_VUE);
    remplir(0, z->bandeau.y, s_ec.w, SEPARATION_H, COULEUR_SEPARATION);
    publier(0, s_ec.h);
    s_ec.ok = true;

#if CONFIG_BANC_ECRAN_RETRO_GPIO >= 0
    gpio_set_level(CONFIG_BANC_ECRAN_RETRO_GPIO, 1);
#endif
    ESP_LOGI(TAG, "écran %s prêt (vue %ux%u, panneau %ux%u, bandeau de %d lignes)", s_ec.nom, z->vue.w, z->vue.h,
             z->panneau.w, z->panneau.h, BANC_NB_LIGNES);
    return ESP_OK;
}

bool banc_ecran_ok(void)
{
    return s_ec.ok;
}

void banc_ecran_couper(void)
{
    if (!s_ec.ok) {
        return;
    }
    xSemaphoreTake(s_ec.mutex, portMAX_DELAY);
    remplir(0, 0, s_ec.w, s_ec.h, FOND_VUE);
    publier(0, s_ec.h);
    s_ec.ok = false;        /* plus aucun dessin ; le panneau reste initialisé */
    xSemaphoreGive(s_ec.mutex);
#if CONFIG_BANC_ECRAN_RETRO_GPIO >= 0
    gpio_set_level(CONFIG_BANC_ECRAN_RETRO_GPIO, 0);
#endif
    strlcpy(s_ec.nom, "désactivé", sizeof(s_ec.nom));
}

const char *banc_ecran_nom(void)
{
    return s_ec.nom;
}

const banc_zones_t *banc_ecran_zones(void)
{
    return &s_ec.zones;
}

uint16_t banc_ecran_car_l(int echelle)
{
    return (uint16_t)CAR_L(echelle);
}

uint16_t banc_ecran_car_h(int echelle)
{
    return (uint16_t)CAR_H(echelle);
}

/* ------------------------------------------------------------------------- */
/* Vue caméra                                                                 */
/* ------------------------------------------------------------------------- */

void banc_ecran_image(const nid_capture_frame_t *f)
{
    if (!s_ec.ok || f == NULL || f->data == NULL || f->width < 2 || f->height < 2) {
        return;
    }
    if (f->recording) {
#if CONFIG_BANC_VUE_ENREG
        /* Quelques images par seconde seulement : le temps passé ici retarde l'enregistrement. */
        const int64_t now = esp_timer_get_time();
        if (now - s_ec.t_vue_us < 1000000LL / CONFIG_BANC_VUE_ENREG_IPS) {
            return;
        }
#else
        return;
#endif
    }

    /* Plus grand facteur k/16 (pas du PPA) qui fait tenir l'image dans la zone de vue. */
    const banc_rect_t *v = &s_ec.zones.vue;
    uint32_t k = ECHELLE_VUE_MAX;
    const uint32_t kx = 16u * v->w / f->width;
    const uint32_t ky = 16u * v->h / f->height;
    if (kx < k) {
        k = kx;
    }
    if (ky < k) {
        k = ky;
    }
    if (k == 0) {
        return;
    }
    const uint32_t ow = f->width * k / 16u;
    const uint32_t oh = f->height * k / 16u;

    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = f->data,
            .pic_w = f->width,
            .pic_h = f->height,
            .block_w = f->width,
            .block_h = f->height,
            .srm_cm = PPA_SRM_COLOR_MODE_YUV420,    /* O_UYY_E_VYY, format de l'ISP */
            .yuv_range = f->full_range ? PPA_COLOR_RANGE_FULL : PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = s_ec.fb,
            .buffer_size = (uint32_t)s_ec.fb_size,
            .pic_w = s_ec.w,
            .pic_h = s_ec.h,
            .block_offset_x = v->x + (v->w - ow) / 2u,
            .block_offset_y = v->y + (v->h - oh) / 2u,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = (float)k / 16.0f,
        .scale_y = (float)k / 16.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    /* Pendant un enregistrement : image sautée si l'écran est occupé (jamais d'attente). */
    if (xSemaphoreTake(s_ec.mutex, f->recording ? 0 : portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (f->recording) {
        s_ec.t_vue_us = esp_timer_get_time();
    }
    if (s_ec.vue_sale || ow != s_ec.img_w || oh != s_ec.img_h) {
        effacer_vue();      /* changement de taille ou message : restes à effacer */
        s_ec.vue_sale = false;
        s_ec.img_w = (uint16_t)ow;
        s_ec.img_h = (uint16_t)oh;
    }
    const esp_err_t err = ppa_do_scale_rotate_mirror(s_ec.ppa, &op);
    xSemaphoreGive(s_ec.mutex);
    if (err != ESP_OK && !s_ec.erreur_ppa) {
        s_ec.erreur_ppa = true;
        ESP_LOGW(TAG, "conversion PPA d'une image %ux%u refusée : %s", f->width, f->height, esp_err_to_name(err));
    }
}

void banc_ecran_message_vue(banc_couleur_t couleur, const char *texte)
{
    if (!s_ec.ok || texte == NULL) {
        return;
    }
    /* La vue est marquée « à effacer » : la prochaine image remplace le message. */
    banc_ecran_texte(&s_ec.zones.vue, ECHELLE_MESSAGE, couleur, BANC_NOIR, true, texte);
}

/* ------------------------------------------------------------------------- */
/* Dessin                                                                     */
/* ------------------------------------------------------------------------- */

void banc_ecran_rect(const banc_rect_t *r, banc_couleur_t couleur)
{
    if (!s_ec.ok || r == NULL || couleur >= BANC_NB_COULEURS) {
        return;
    }
    xSemaphoreTake(s_ec.mutex, portMAX_DELAY);
    remplir(r->x, r->y, r->w, r->h, s_couleurs[couleur]);
    publier(r->y, (uint32_t)r->y + r->h);
    xSemaphoreGive(s_ec.mutex);
}

void banc_ecran_texte(const banc_rect_t *r, int echelle, banc_couleur_t encre, banc_couleur_t fond, bool centre,
                      const char *texte)
{
    if (!s_ec.ok || r == NULL || texte == NULL || encre >= BANC_NB_COULEURS || fond >= BANC_NB_COULEURS) {
        return;
    }
    if (echelle < 1) {
        echelle = 1;
    }
    size_t lignes = 1;
    for (const char *p = texte; *p != '\0'; p++) {
        lignes += (*p == '\n');
    }
    const uint32_t interligne = CAR_H(echelle) + (uint32_t)echelle * 2u;
    const uint32_t haut_texte = (uint32_t)lignes * interligne;
    uint32_t y = r->y + MARGE_TEXTE;
    if (centre) {
        y = r->y + (haut_texte < r->h ? (r->h - haut_texte) / 2u + (uint32_t)echelle : 0);
    }
    const uint32_t x_max = (uint32_t)r->x + r->w - (centre ? 0 : MARGE_TEXTE);
    const uint32_t y_max = (uint32_t)r->y + r->h;

    xSemaphoreTake(s_ec.mutex, portMAX_DELAY);
    remplir(r->x, r->y, r->w, r->h, s_couleurs[fond]);
    const char *p = texte;
    while (y < y_max) {
        uint32_t x = (uint32_t)r->x + MARGE_TEXTE;
        if (centre) {
            const uint32_t largeur = (uint32_t)longueur_ligne(p) * CAR_L(echelle);
            x = r->x + (largeur < r->w ? (r->w - largeur) / 2u : 0);
        }
        dessiner_ligne(x, y, p, s_couleurs[encre], s_couleurs[fond], x_max, y_max, echelle);
        const char *nl = strchr(p, '\n');
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
        y += interligne;
    }
    if (r == &s_ec.zones.vue) {
        s_ec.vue_sale = true;
    }
    publier(r->y, y_max);
    xSemaphoreGive(s_ec.mutex);
}

void banc_ecran_barre(const banc_rect_t *r, int pour_mille, int repere_pour_mille, banc_couleur_t couleur,
                      banc_couleur_t fond)
{
    if (!s_ec.ok || r == NULL || couleur >= BANC_NB_COULEURS || fond >= BANC_NB_COULEURS) {
        return;
    }
    if (pour_mille < 0) {
        pour_mille = 0;
    } else if (pour_mille > 1000) {
        pour_mille = 1000;
    }
    const uint32_t plein = (uint32_t)r->w * (uint32_t)pour_mille / 1000u;
    xSemaphoreTake(s_ec.mutex, portMAX_DELAY);
    remplir(r->x, r->y, plein, r->h, s_couleurs[couleur]);
    remplir(r->x + plein, r->y, r->w - plein, r->h, s_couleurs[fond]);
    if (repere_pour_mille >= 0 && repere_pour_mille <= 1000) {
        uint32_t xr = (uint32_t)r->w * (uint32_t)repere_pour_mille / 1000u;
        if (xr + 3 > r->w) {
            xr = r->w > 3 ? r->w - 3u : 0;
        }
        remplir(r->x + xr, r->y, 3, r->h, s_couleurs[BANC_BLANC]);
    }
    publier(r->y, (uint32_t)r->y + r->h);
    xSemaphoreGive(s_ec.mutex);
}

void banc_ecran_disque(uint16_t cx, uint16_t cy, uint16_t rayon, banc_couleur_t couleur)
{
    if (!s_ec.ok || couleur >= BANC_NB_COULEURS || cx < rayon || cy < rayon) {
        return;
    }
    const int32_t r2 = (int32_t)rayon * rayon;
    xSemaphoreTake(s_ec.mutex, portMAX_DELAY);
    for (int32_t dy = -(int32_t)rayon; dy <= (int32_t)rayon; dy++) {
        int32_t dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r2) {
            dx++;
        }
        remplir((uint32_t)(cx - dx), (uint32_t)(cy + dy), (uint32_t)(2 * dx + 1), 1, s_couleurs[couleur]);
    }
    publier((uint32_t)(cy - rayon), (uint32_t)(cy + rayon + 1));
    xSemaphoreGive(s_ec.mutex);
}

/* ------------------------------------------------------------------------- */
/* Bandeau d'état                                                             */
/* ------------------------------------------------------------------------- */

void banc_ecran_ligne(banc_ligne_t ligne, banc_couleur_t couleur, const char *fmt, ...)
{
    if (!s_ec.ok || ligne >= BANC_NB_LIGNES || couleur >= BANC_NB_COULEURS) {
        return;
    }
    char texte[TEXTE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(texte, sizeof(texte), fmt, ap);
    va_end(ap);

    const banc_rect_t *b = &s_ec.zones.bandeau;
    const uint32_t y = b->y + SEPARATION_H + 4u + (uint32_t)ligne * LIGNE_H;
    const uint16_t fond = s_couleurs[BANC_FOND];
    xSemaphoreTake(s_ec.mutex, portMAX_DELAY);
    remplir(0, y, s_ec.w, CAR_H(ECHELLE_TEXTE), fond);
    dessiner_ligne(MARGE_X, y, texte, s_couleurs[couleur], fond, (uint32_t)s_ec.w - MARGE_X, y + CAR_H(ECHELLE_TEXTE),
                   ECHELLE_TEXTE);
    publier(y, y + CAR_H(ECHELLE_TEXTE));
    xSemaphoreGive(s_ec.mutex);
}

#else /* CONFIG_BANC_ECRAN_AUCUN */

static const banc_zones_t s_zones_vides;

esp_err_t banc_ecran_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool banc_ecran_ok(void)
{
    return false;
}

void banc_ecran_couper(void)
{
}

const char *banc_ecran_nom(void)
{
    return "aucun";
}

const banc_zones_t *banc_ecran_zones(void)
{
    return &s_zones_vides;
}

uint16_t banc_ecran_car_l(int echelle)
{
    return (uint16_t)(6 * echelle);
}

uint16_t banc_ecran_car_h(int echelle)
{
    return (uint16_t)(9 * echelle);
}

void banc_ecran_image(const nid_capture_frame_t *f)
{
    (void)f;
}

void banc_ecran_ligne(banc_ligne_t ligne, banc_couleur_t couleur, const char *fmt, ...)
{
    (void)ligne;
    (void)couleur;
    (void)fmt;
}

void banc_ecran_message_vue(banc_couleur_t couleur, const char *texte)
{
    (void)couleur;
    (void)texte;
}

void banc_ecran_rect(const banc_rect_t *r, banc_couleur_t couleur)
{
    (void)r;
    (void)couleur;
}

void banc_ecran_texte(const banc_rect_t *r, int echelle, banc_couleur_t encre, banc_couleur_t fond, bool centre,
                      const char *texte)
{
    (void)r;
    (void)echelle;
    (void)encre;
    (void)fond;
    (void)centre;
    (void)texte;
}

void banc_ecran_barre(const banc_rect_t *r, int pour_mille, int repere_pour_mille, banc_couleur_t couleur,
                      banc_couleur_t fond)
{
    (void)r;
    (void)pour_mille;
    (void)repere_pour_mille;
    (void)couleur;
    (void)fond;
}

void banc_ecran_disque(uint16_t cx, uint16_t cy, uint16_t rayon, banc_couleur_t couleur)
{
    (void)cx;
    (void)cy;
    (void)rayon;
    (void)couleur;
}

#endif /* CONFIG_BANC_ECRAN_AUCUN */
