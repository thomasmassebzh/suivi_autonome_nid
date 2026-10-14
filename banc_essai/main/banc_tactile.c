/*
 * banc_tactile — dalle tactile GT911 de l'écran 7 pouces du kit
 *
 * Adapté du BSP esp32_p4_function_ev_board d'esp-bsp (Apache-2.0),
 * bsp_touch_new() : GT911 sur le bus I2C partagé de la carte, broches de reset
 * et d'interruption non câblées (adresse 0x5D, ou 0x14 selon l'état de la
 * broche INT au démarrage de la dalle), coordonnées en miroir sur les deux
 * axes pour l'écran EK79007 du kit (réglable dans le menu « Banc d'essai »).
 *
 * La dalle est lue par scrutation (tâche d'affichage, toutes les ~40 ms) : sans
 * broche d'interruption, quelques transactions I2C par lecture sur le bus
 * partagé avec la caméra et le codec audio (le pilote I2C les sérialise).
 */
#include "banc.h"

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"

#if CONFIG_BANC_TACTILE

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "nid_board.h"

static const char *TAG = "banc_tactile";

#define SONDE_DELAI_MS      50

/* Options booléennes du menu : absentes de sdkconfig.h quand elles valent « n ». */
#ifdef CONFIG_BANC_TACTILE_ECHANGE_XY
#define ECHANGE_XY          1
#else
#define ECHANGE_XY          0
#endif
#ifdef CONFIG_BANC_TACTILE_MIROIR_X
#define MIROIR_X            1
#else
#define MIROIR_X            0
#endif
#ifdef CONFIG_BANC_TACTILE_MIROIR_Y
#define MIROIR_Y            1
#else
#define MIROIR_Y            0
#endif

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static char s_nom[24] = "absente";

esp_err_t banc_tactile_init(void)
{
    if (s_tp != NULL) {
        return ESP_OK;
    }
    i2c_master_bus_handle_t bus = nid_board_i2c_bus();
    if (bus == NULL || !banc_ecran_ok()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Adresse : 0x5D, ou 0x14 (selon l'état de INT au démarrage de la dalle). */
    uint8_t adresse = 0;
    if (i2c_master_probe(bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, SONDE_DELAI_MS) == ESP_OK) {
        adresse = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    } else if (i2c_master_probe(bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, SONDE_DELAI_MS) == ESP_OK) {
        adresse = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
    } else {
        ESP_LOGW(TAG, "GT911 absent (0x5D, 0x14) : boutons tactiles indisponibles (bouton BOOT et console)");
        return ESP_ERR_NOT_FOUND;
    }

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.dev_addr = adresse;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &s_io), TAG, "interface I2C du GT911");

    const banc_zones_t *z = banc_ecran_zones();
    esp_lcd_touch_io_gt911_config_t gt911_cfg = {
        .dev_addr = adresse,
    };
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = (uint16_t)(z->onglets.w),
        .y_max = (uint16_t)(z->bandeau.y + z->bandeau.h),
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = ECHANGE_XY,
            .mirror_x = MIROIR_X,
            .mirror_y = MIROIR_Y,
        },
        .driver_data = &gt911_cfg,
    };
    esp_err_t err = esp_lcd_touch_new_i2c_gt911(s_io, &tp_cfg, &s_tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT911 (0x%02X) : %s", adresse, esp_err_to_name(err));
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
        s_tp = NULL;
        return err;
    }
    snprintf(s_nom, sizeof(s_nom), "GT911 0x%02X", adresse);
    ESP_LOGI(TAG, "dalle tactile %s prête", s_nom);
    return ESP_OK;
}

bool banc_tactile_ok(void)
{
    return s_tp != NULL;
}

const char *banc_tactile_nom(void)
{
    return s_nom;
}

bool banc_tactile_lire(uint16_t *x, uint16_t *y)
{
    if (s_tp == NULL) {
        return false;
    }
    if (esp_lcd_touch_read_data(s_tp) != ESP_OK) {
        return false;
    }
    esp_lcd_touch_point_data_t pt[1];
    uint8_t n = 0;
    if (esp_lcd_touch_get_data(s_tp, pt, &n, 1) != ESP_OK || n == 0) {
        return false;
    }
    *x = pt[0].x;
    *y = pt[0].y;
    return true;
}

#else /* !CONFIG_BANC_TACTILE */

esp_err_t banc_tactile_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool banc_tactile_ok(void)
{
    return false;
}

const char *banc_tactile_nom(void)
{
    return "désactivée (menu)";
}

bool banc_tactile_lire(uint16_t *x, uint16_t *y)
{
    (void)x;
    (void)y;
    return false;
}

#endif /* CONFIG_BANC_TACTILE */
