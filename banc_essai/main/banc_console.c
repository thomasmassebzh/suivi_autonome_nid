/*
 * banc_console — commandes de la console série du banc d'essai
 *
 * REPL esp_console sur la console configurée (CONFIG_ESP_CONSOLE_* : port USB
 * Serial/JTAG de la P4X par défaut, ou UART), comme l'exemple d'ESP-IDF
 * examples/system/console/basic (domaine public / CC0). Les commandes qui
 * utilisent la caméra (video, photo) sont confiées à la tâche d'action du banc,
 * partagée avec le bouton BOOT : jamais deux enregistrements en même temps.
 */
#include "banc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "sdkconfig.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_log.h"

#include "nid_board.h"
#include "nid_storage.h"

static const char *TAG = "banc_console";

#define I2C_ADRESSE_MIN     0x08
#define I2C_ADRESSE_MAX     0x77
#define I2C_DELAI_MS        50

/* ------------------------------------------------------------------------- */
/* video, photo, info, nuit                                                   */
/* ------------------------------------------------------------------------- */

static int cmd_video(int argc, char **argv)
{
    long duree = CONFIG_BANC_DUREE_S;
    if (argc > 2) {
        printf("Usage : video [durée_s]\n");
        return 1;
    }
    if (argc == 2) {
        char *fin = NULL;
        duree = strtol(argv[1], &fin, 10);
        if (fin == argv[1] || *fin != '\0' || duree < 1 || duree > 300) {
            printf("Durée invalide : 1 à 300 s\n");
            return 1;
        }
    }
    esp_err_t err = banc_demander_video((uint32_t)duree, "console");
    if (err != ESP_OK) {
        printf("Occupé (enregistrement ou photo en cours) : réessayer à la fin\n");
        return 1;
    }
    printf("Enregistrement de %ld s demandé : bilan à l'écran et sur la ligne « BILAN » du journal\n", duree);
    return 0;
}

static int cmd_photo(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (banc_demander_photo("console") != ESP_OK) {
        printf("Occupé (enregistrement ou photo en cours) : réessayer à la fin\n");
        return 1;
    }
    printf("Photo demandée : bilan à l'écran et sur la ligne « BILAN » du journal\n");
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    banc_afficher_info();
    return 0;
}

static int cmd_nuit(int argc, char **argv)
{
    bool nuit;
    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        nuit = true;
    } else if (argc == 2 && strcmp(argv[1], "off") == 0) {
        nuit = false;
    } else {
        printf("Usage : nuit on|off\n");
        return 1;
    }
    esp_err_t err = banc_regler_nuit(nuit);
    banc_rafraichir_entete();
    if (err == ESP_OK) {
        printf("Mode %s : image %s (les LED IR ne sont pas pilotées par le banc)\n", nuit ? "nuit" : "jour",
               nuit ? "en niveaux de gris" : "en couleur");
        return 0;
    }
    printf("Mode %s mémorisé, non appliqué : %s%s\n", nuit ? "nuit" : "jour", esp_err_to_name(err),
           err == ESP_ERR_INVALID_STATE ? " (caméra non prête)" : "");
    return 1;
}

/* ------------------------------------------------------------------------- */
/* i2cscan                                                                    */
/* ------------------------------------------------------------------------- */

static const char *nom_i2c(uint16_t adresse)
{
    switch (adresse) {
    case 0x30: return "SC2336 (caméra 2 MP du kit)";
    case 0x36: return "OV5647 (caméra IR-cut)";
    case 0x18: return "ES8311 (codec audio de la carte)";
    case 0x5D: return "GT911 (dalle tactile de l'écran)";
    case 0x14: return "GT911 (dalle tactile, autre adresse)";
    case 0x68: return "DS3231 / DS1307 (horloge temps réel)";
    case 0x57: return "AT24C32 (mémoire du module DS3231)";
    default:   return "inconnu";
    }
}

static int cmd_i2cscan(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    i2c_master_bus_handle_t bus = nid_board_i2c_bus();
    if (bus == NULL) {
        printf("Bus I2C non initialisé (voir le journal de démarrage)\n");
        return 1;
    }
    if (banc_enregistrement_en_cours()) {
        printf("Enregistrement en cours : réessayer à la fin\n");
        return 1;
    }
    const nid_board_pins_t *pins = nid_board_pins();
    printf("Bus I2C partagé (SDA GPIO%d, SCL GPIO%d) :\n", (int)pins->i2c_sda, (int)pins->i2c_scl);
    int trouves = 0;
    for (uint16_t a = I2C_ADRESSE_MIN; a <= I2C_ADRESSE_MAX; a++) {
        esp_err_t err = i2c_master_probe(bus, a, I2C_DELAI_MS);
        if (err == ESP_OK) {
            printf("  0x%02X  %s\n", a, nom_i2c(a));
            trouves++;
        } else if (err == ESP_ERR_TIMEOUT) {
            printf("  0x%02X  délai dépassé : bus bloqué ou rappels au 3,3 V absents ?\n", a);
            break;
        }
    }
    printf("%d périphérique(s). Attendus : ES8311 0x18 ; caméra 0x30 (SC2336) ou 0x36 (OV5647) ;\n"
           "GT911 0x5D ou 0x14 si l'écran tactile est branché ; DS3231 0x68 si l'horloge est câblée.\n", trouves);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* heure                                                                      */
/* ------------------------------------------------------------------------- */

static void afficher_heure(void)
{
    const time_t now = time(NULL);
    struct tm tm;
    char txt[32];
    localtime_r(&now, &tm);
    strftime(txt, sizeof(txt), "%Y-%m-%d %H:%M:%S", &tm);
    printf("Heure locale : %s (fuseau %s)\n", txt, CONFIG_BANC_FUSEAU);
}

static int cmd_heure(int argc, char **argv)
{
    if (argc == 1) {
        afficher_heure();
        return 0;
    }
    int a = 0, mo = 0, j = 0, h = 0, mi = 0, s = 0;
    char reste = '\0';
    if (argc != 3 || sscanf(argv[1], "%d-%d-%d%c", &a, &mo, &j, &reste) != 3 ||
        sscanf(argv[2], "%d:%d:%d%c", &h, &mi, &s, &reste) != 3 || a < 2024 || a > 2099 || mo < 1 || mo > 12 ||
        j < 1 || j > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) {
        printf("Usage : heure AAAA-MM-JJ HH:MM:SS (heure locale, ex. heure 2026-10-14 09:30:00)\n");
        return 1;
    }
    struct tm tm = {
        .tm_year = a - 1900,
        .tm_mon = mo - 1,
        .tm_mday = j,
        .tm_hour = h,
        .tm_min = mi,
        .tm_sec = s,
        .tm_isdst = -1,
    };
    const time_t t = mktime(&tm);
    if (t == (time_t)-1) {
        printf("Date invalide\n");
        return 1;
    }
    const struct timeval tv = { .tv_sec = t };
    if (settimeofday(&tv, NULL) != 0) {
        printf("Réglage de l'heure refusé\n");
        return 1;
    }
    afficher_heure();
    nid_storage_journal("heure_reglee", "%s %s (console du banc)", argv[1], argv[2]);
    banc_rafraichir_entete();
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Démarrage                                                                  */
/* ------------------------------------------------------------------------- */

esp_err_t banc_console_demarrer(void)
{
    static const esp_console_cmd_t cmds[] = {
        {
            .command = "video",
            .help = "Enregistre une vidéo MP4 (durée par défaut : menu « Banc d'essai »), comme le bouton BOOT",
            .hint = "[durée_s]",
            .func = cmd_video,
        },
        {
            .command = "photo",
            .help = "Prend une photo JPEG sur la carte SD",
            .func = cmd_photo,
        },
        {
            .command = "info",
            .help = "Capteur, réglages, carte SD, mémoire libre, version, dernier bilan",
            .func = cmd_info,
        },
        {
            .command = "nuit",
            .help = "Mode nuit (image monochrome par l'ISP) ou jour",
            .hint = "on|off",
            .func = cmd_nuit,
        },
        {
            .command = "i2cscan",
            .help = "Liste les périphériques du bus I2C partagé (caméra, codec, tactile, horloge)",
            .func = cmd_i2cscan,
        },
        {
            .command = "heure",
            .help = "Affiche ou règle l'heure locale",
            .hint = "[AAAA-MM-JJ HH:MM:SS]",
            .func = cmd_heure,
        },
    };

    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "commande help");
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), TAG, "commande %s", cmds[i].command);
    }

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "banc>";
    cfg.max_cmdline_length = 128;
    cfg.task_stack_size = 6144;
    ESP_RETURN_ON_ERROR(esp_console_new_repl_stdio(&cfg, &repl), TAG, "console");
    return esp_console_start_repl(repl);
}
