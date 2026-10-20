/*
 * banc_console — commandes de la console série du banc d'essai
 *
 * REPL esp_console sur la console configurée (CONFIG_ESP_CONSOLE_* : port USB
 * Serial/JTAG de la P4X par défaut, ou UART), comme l'exemple d'ESP-IDF
 * examples/system/console/basic (domaine public / CC0). La même table de
 * commandes sert la console de la page web (banc_console_executer). Les commandes qui
 * utilisent la caméra, le micro ou écrivent sur la carte sont confiées à la
 * tâche d'action du banc, partagée avec le bouton BOOT, l'écran tactile et le
 * PIR : jamais deux enregistrements en même temps.
 */
#include "banc.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "sdkconfig.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "nid_board.h"
#include "nid_detect.h"
#include "nid_power.h"
#include "nid_storage.h"

static const char *TAG = "banc_console";

#define I2C_ADRESSE_MIN     0x08
#define I2C_ADRESSE_MAX     0x77
#define I2C_DELAI_MS        50

static const char *const OCCUPE = "Occupé (enregistrement, photo ou test en cours) : réessayer à la fin\n";

/** Entier décimal dans [min, max] ; false si invalide. */
static bool lire_entier(const char *s, long min, long max, long *out)
{
    char *fin = NULL;
    const long v = strtol(s, &fin, 10);
    if (fin == s || *fin != '\0' || v < min || v > max) {
        return false;
    }
    *out = v;
    return true;
}

/* ------------------------------------------------------------------------- */
/* video, photo, info                                                         */
/* ------------------------------------------------------------------------- */

static int cmd_video(int argc, char **argv)
{
    long duree = CONFIG_BANC_DUREE_S;
    if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        banc_arreter("console");
        return 0;
    }
    if (argc > 2 || (argc == 2 && !lire_entier(argv[1], 1, BANC_DUREE_MAX_S, &duree))) {
        printf("Usage : video [durée_s]   (1 à %d s ; « video stop » : arrêt anticipé)\n", BANC_DUREE_MAX_S);
        return 1;
    }
    if (banc_demander(BANC_ACTION_VIDEO, (uint32_t)duree, 0, "console") != ESP_OK) {
        printf("%s", OCCUPE);
        return 1;
    }
    printf("Enregistrement de %ld s demandé : bilan à l'écran et sur la ligne « BILAN » du journal\n", duree);
    return 0;
}

static int cmd_photo(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (banc_demander(BANC_ACTION_PHOTO, 0, 0, "console") != ESP_OK) {
        printf("%s", OCCUPE);
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

/* ------------------------------------------------------------------------- */
/* Détection PIR                                                              */
/* ------------------------------------------------------------------------- */

static void afficher_detection(void)
{
    banc_etat_t e;
    banc_etat_lire(&e);
    char decl[40];
    banc_couleur_t fond;
    banc_texte_declenchement(&e, esp_timer_get_time(), decl, sizeof(decl), &fond);
    printf("PIR (GPIO%d) : %s ; déclenchement %s\n", (int)nid_board_pins()->pir,
           nid_detect_pir_active() ? "ACTIF" : "repos", decl);
    printf("Fronts %" PRIu32 ", détections %" PRIu32 ", séquences %" PRIu32 ", fausses %" PRIu32 ", ignorés %" PRIu32
           "%s%s%s\n", e.fronts, e.detections, e.sequences, e.fausses, e.ignorees, e.ignorees ? " (dernier : " : "",
           e.ignorees ? e.ignoree_motif : "", e.ignorees ? ")" : "");
    if (e.latence_ms >= 0) {
        printf("Latence front PIR → 1re image de la dernière séquence : %" PRId32 " ms\n", e.latence_ms);
    }
}

static int cmd_armer(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        banc_armer(true, "console");
    } else if (argc == 2 && strcmp(argv[1], "off") == 0) {
        banc_armer(false, "console");
    } else if (argc != 1) {
        printf("Usage : armer [on|off]   (sans argument : état)\n");
        return 1;
    }
    afficher_detection();
    return 0;
}

static int cmd_compteurs(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "raz") == 0) {
        banc_compteurs_raz();
        printf("Compteurs remis à zéro\n");
    } else if (argc != 1) {
        printf("Usage : compteurs [raz]\n");
        return 1;
    }
    afficher_detection();
    return 0;
}

static int cmd_regle(int argc, char **argv)
{
    if (argc == 3) {
        esp_err_t err = banc_regler(argv[1], argv[2]);
        if (err == ESP_ERR_NOT_FOUND) {
            printf("Clé inconnue ou non modifiable sur le banc : %s\n", argv[1]);
        } else if (err != ESP_OK) {
            printf("Valeur refusée pour %s : %s (voir les plages de config.ini)\n", argv[1], argv[2]);
        }
        if (err != ESP_OK) {
            return 1;
        }
    } else if (argc != 1) {
        printf("Usage : regle [clé valeur]   (clés de config.ini, ex. regle delai_entre_declenchements_s 20)\n");
        return 1;
    }
    nid_config_t c;
    banc_reglages(&c);
    printf("duree_sequence_s = %u\ndelai_entre_declenchements_s = %u   (compté depuis la fin de la séquence ; 0 = "
           "enchaînement tant que le PIR est actif)\nchauffe_pir_s = %u\nsensibilite = %u   (actif %" PRIu32
           " ms au moins)\nintervalle_suivi_min = %u   (0 = pas de photo de suivi)\nmode = %s\nlatitude = %.4f\n"
           "longitude = %.4f\navance_crepuscule_min = %d\nretard_aube_min = %d\npuissance_ir = %u\nnuit_monochrome = %s\n",
           c.seq_duration_s, c.trigger_delay_s, c.pir_warmup_s, c.pir_sensitivity,
           nid_detect_min_active_ms(c.pir_sensitivity), c.photo_interval_min, nid_config_night_mode_str(c.night_mode),
           (double)c.latitude, (double)c.longitude, c.dusk_advance_min, c.dawn_delay_min, c.ir_power_pct,
           c.night_monochrome ? "oui" : "non");
    return 0;
}

static int cmd_suivi(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage : suivi <minutes>   (0 = arrêt ; photos prises si la surveillance est armée)\n");
        return 1;
    }
    if (banc_regler("intervalle_suivi_min", argv[1]) != ESP_OK) {
        printf("Intervalle invalide : 0 à 1440 min\n");
        return 1;
    }
    banc_etat_t e;
    banc_etat_lire(&e);
    printf("Photo de suivi toutes les %s min%s\n", argv[1], e.arme ? "" : " (surveillance désarmée : « armer on »)");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Nuit                                                                       */
/* ------------------------------------------------------------------------- */

static void afficher_nuit(void)
{
    banc_etat_t e;
    banc_etat_lire(&e);
    static const char *const modes[] = { "jour forcé", "nuit forcée", "automatique (firmware)" };
    char lever[12], coucher[12];
    bool nuit = false;
    banc_soleil(lever, coucher, sizeof(lever), &nuit);
    nid_config_t c;
    banc_reglages(&c);
    printf("Mode %s : %s ; LED IR %u %% ; filtre IR-cut %s ; image %s\n", modes[e.mode_nuit],
           e.nuit_active ? "NUIT" : "JOUR", e.led_ir_pct,
           e.filtre_jour < 0 ? "non câblé" : (e.filtre_jour ? "jour" : "nuit"), e.mono ? "monochrome" : "couleur");
    printf("Soleil à %.2f N, %.2f E : lever %s, coucher %s (heure locale du banc) ; décision du firmware : %s\n",
           (double)c.latitude, (double)c.longitude, lever, coucher, nuit ? "NUIT" : "JOUR");
}

static int cmd_nuit(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        banc_nuit_mode(BANC_NUIT_NUIT);
    } else if (argc == 2 && strcmp(argv[1], "off") == 0) {
        banc_nuit_mode(BANC_NUIT_JOUR);
    } else if (argc == 2 && strcmp(argv[1], "auto") == 0) {
        banc_nuit_mode(BANC_NUIT_AUTO);
    } else if (argc != 1) {
        printf("Usage : nuit [on|off|auto]   (filtre, LED IR à puissance_ir, monochrome, comme le firmware)\n");
        return 1;
    }
    afficher_nuit();
    return 0;
}

static int cmd_ir(int argc, char **argv)
{
    long pct = 0;
    if (argc != 2 || !lire_entier(argv[1], 0, 100, &pct)) {
        printf("Usage : ir <0-100>   (puissance des LED IR en %%, PWM)\n");
        return 1;
    }
    esp_err_t err = banc_ir_puissance((uint8_t)pct);
    if (err != ESP_OK) {
        printf("LED IR non configurées (menu « Suivi nid — Carte et brochage ») : %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("LED IR à %ld %% (GPIO%d)\n", pct, (int)nid_board_pins()->ir_led);
    return 0;
}

static int cmd_ircut(int argc, char **argv)
{
    bool jour;
    if (argc == 2 && strcmp(argv[1], "jour") == 0) {
        jour = true;
    } else if (argc == 2 && strcmp(argv[1], "nuit") == 0) {
        jour = false;
    } else {
        printf("Usage : ircut jour|nuit   (jour : filtre en place)\n");
        return 1;
    }
    esp_err_t err = banc_ircut(jour);
    if (err != ESP_OK) {
        printf("Filtre IR-cut non commandé : %s (menu « Commande du filtre IR-cut »)\n", esp_err_to_name(err));
        return 1;
    }
    printf("Filtre IR-cut : %s\n", jour ? "jour (en place)" : "nuit (retiré)");
    return 0;
}

static int cmd_mono(int argc, char **argv)
{
    bool mono;
    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        mono = true;
    } else if (argc == 2 && strcmp(argv[1], "off") == 0) {
        mono = false;
    } else {
        printf("Usage : mono on|off   (image en niveaux de gris par l'ISP)\n");
        return 1;
    }
    esp_err_t err = banc_mono(mono);
    if (err != ESP_OK) {
        printf("Non appliqué : %s%s\n", esp_err_to_name(err), err == ESP_ERR_INVALID_STATE ? " (caméra non prête)" : "");
        return 1;
    }
    printf("Image %s\n", mono ? "monochrome" : "couleur");
    return 0;
}

static int cmd_soleil(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    afficher_nuit();
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Son, batterie, température                                                 */
/* ------------------------------------------------------------------------- */

static int cmd_son(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "video") == 0) {
        long duree = CONFIG_BANC_DUREE_S;
        if (argc == 3 && !lire_entier(argv[2], 1, BANC_DUREE_MAX_S, &duree)) {
            printf("Durée invalide : 1 à %d s\n", BANC_DUREE_MAX_S);
            return 1;
        }
        if (banc_demander(BANC_ACTION_SON_VIDEO, (uint32_t)duree, 0, "console") != ESP_OK) {
            printf("%s", OCCUPE);
            return 1;
        }
        printf("Vidéo de test du son (%ld s) : parler près du micro ; vérifier trames_son dans le BILAN\n", duree);
        return 0;
    }
    if (argc != 1) {
        printf("Usage : son [video [durée_s]]\n");
        return 1;
    }
    if (!banc_reglages_capture()->audio) {
        printf("Piste son désactivée (menu « Banc d'essai »)\n");
        return 1;
    }
    nid_capture_audio_level_t l;
    bool valide;
    if (banc_enregistrement_en_cours()) {
        valide = nid_capture_audio_last_level(&l);
    } else {
        esp_err_t err = nid_capture_audio_level(1000, &l);
        valide = (err == ESP_OK);
        if (!valide) {
            printf("Mesure impossible : %s (ES8311 : « i2cscan » doit montrer 0x18)\n", esp_err_to_name(err));
            return 1;
        }
    }
    if (!valide) {
        printf("Pas de niveau disponible\n");
        return 1;
    }
    const int rms = banc_dbfs10(l.rms);
    const int crete = banc_dbfs10(l.peak);
    printf("Micro : efficace %d.%d dBFS, crête %d.%d dBFS (%u / %u sur 32767)\n", rms / 10, abs(rms % 10),
           crete / 10, abs(crete % 10), l.rms, l.peak);
    return 0;
}

static int cmd_batterie(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint32_t mv = 0, brut = 0;
    esp_err_t err = banc_batterie(&mv, &brut);
    if (err == ESP_OK) {
        printf("Batterie : %" PRIu32 " mV (GPIO%d, rapport du pont %d/1000)\n", mv, (int)nid_board_pins()->batt_adc,
               CONFIG_NID_BATT_DIVIDER_X1000);
    } else if (err == ESP_ERR_INVALID_RESPONSE) {
        printf("Batterie : non câblée — %" PRIu32 " mV lus, hors plage %d à %d mV : mesure ignorée, jamais d'arrêt\n",
               brut, NID_BATT_PLAUSIBLE_MIN_MV, NID_BATT_PLAUSIBLE_MAX_MV);
    } else {
        printf("Batterie : pas de mesure (%s)\n", esp_err_to_name(err));
    }
    return 0;
}

static int cmd_temp(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    float c = 0.0f;
    esp_err_t err = banc_temperature(&c);
    if (err != ESP_OK) {
        printf("Capteur de température : %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Température de la puce : %.1f °C\n", (double)c);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Carte SD, endurance                                                        */
/* ------------------------------------------------------------------------- */

static int cmd_sd(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "test") == 0) {
        long mo = CONFIG_BANC_SD_TEST_MO;
        if (argc == 3 && !lire_entier(argv[2], 1, 1024, &mo)) {
            printf("Taille invalide : 1 à 1024 Mo\n");
            return 1;
        }
        if (banc_demander(BANC_ACTION_SD_TEST, (uint32_t)mo, 0, "console") != ESP_OK) {
            printf("%s", OCCUPE);
            return 1;
        }
        printf("Test d'écriture de %ld Mo lancé (bilan : ligne « BILAN sd_test »)\n", mo);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "scan") == 0) {
        if (banc_demander(BANC_ACTION_SD_SCAN, 0, 0, "console") != ESP_OK) {
            printf("%s", OCCUPE);
            return 1;
        }
        printf("Recomptage de l'espace libre lancé (lecture de toute la FAT)\n");
        return 0;
    }
    if (argc != 1) {
        printf("Usage : sd [test [Mo] | scan]\n");
        return 1;
    }
    nid_storage_volume_t v;
    banc_volume_sd(&v);
    if (!nid_storage_is_mounted()) {
        printf("Carte SD non montée\n");
        return 1;
    }
    printf("Volume FAT%u : %" PRIu64 " Mo libres sur %" PRIu64 " Mo ; carte %" PRIu64 " Mo ; grappes de %" PRIu32
           " ko ; début au secteur %" PRIu32 "\n", v.fat_type, v.free_bytes / 1000000ULL, v.volume_bytes / 1000000ULL,
           v.card_bytes / 1000000ULL, v.cluster_bytes / 1024u, v.first_sector);
    if (v.card_bytes > 0 && v.volume_bytes < v.card_bytes / 10 * 9) {
        printf("ATTENTION : le volume ne couvre pas la carte (plusieurs partitions ?) : reformater en une seule FAT32\n");
    }
    return 0;
}

static int cmd_boucle(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        banc_arreter("console");
        return 0;
    }
    long n = 0, duree = CONFIG_BANC_DUREE_S;
    if (argc < 2 || argc > 3 || !lire_entier(argv[1], 1, 1000, &n) ||
        (argc == 3 && !lire_entier(argv[2], 1, BANC_DUREE_MAX_S, &duree))) {
        printf("Usage : boucle N [durée_s] | boucle stop   (N vidéos d'affilée, bilan mémoire et images perdues)\n");
        return 1;
    }
    if (banc_demander(BANC_ACTION_BOUCLE, (uint32_t)n, (uint32_t)duree, "console") != ESP_OK) {
        printf("%s", OCCUPE);
        return 1;
    }
    printf("Endurance : %ld vidéo(s) de %ld s ; arrêt par BOOT ou « boucle stop » ; bilan : « BILAN boucle »\n", n,
           duree);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Écran                                                                      */
/* ------------------------------------------------------------------------- */

static int cmd_ecran(int argc, char **argv)
{
    if (argc == 2) {
        for (int i = 0; i < BANC_NB_ONGLETS; i++) {
            if (strcasecmp(argv[1], banc_ui_nom_onglet((banc_onglet_t)i)) == 0) {
                banc_ui_choisir((banc_onglet_t)i);
                printf("Onglet %s\n", argv[1]);
                return 0;
            }
        }
    }
    printf("Usage : ecran detection|photo|nuit|son|systeme|wifi   (onglet affiché : %s)\n",
           banc_ui_nom_onglet(banc_ui_onglet()));
    return argc == 1 ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* Fonctions à tester                                                         */
/* ------------------------------------------------------------------------- */

static int cmd_fonctions(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "go") == 0) {
        if (!banc_fonctions_en_selection()) {
            printf("Le banc a déjà démarré\n");
            return 1;
        }
        banc_fonctions_valider();
        return 0;
    }
    if (argc == 3 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        const bool on = (strcmp(argv[1], "on") == 0);
        int trouve = -1;
        for (int f = 0; f < BANC_NB_F; f++) {
            if (strcasecmp(argv[2], banc_f_nom((banc_fonction_t)f)) == 0) {
                trouve = f;
            }
        }
        if (trouve < 0) {
            printf("Fonction inconnue : %s\n", argv[2]);
            return 1;
        }
        if (banc_f_choisir((banc_fonction_t)trouve, on) != ESP_OK) {
            printf("Fonction non compilée (menu « Banc d'essai › Fonctions activées »)\n");
            return 1;
        }
        printf("%s : %s%s\n", banc_f_libelle((banc_fonction_t)trouve), on ? "activée" : "désactivée",
               banc_fonctions_en_selection() ? "" : " au prochain démarrage (commande « redemarrer » ou RESET)");
    } else if (argc != 1) {
        printf("Usage : fonctions [on|off <nom> | go]\n");
        return 1;
    }
    printf("%-12s %-30s %s\n", "nom", "fonction", "état");
    for (int f = 0; f < BANC_NB_F; f++) {
        const char *t = banc_f_texte((banc_fonction_t)f);
        const bool choisie = banc_f_choisie((banc_fonction_t)f);
        printf("%-12s %-30s %s%s\n", banc_f_nom((banc_fonction_t)f), banc_f_libelle((banc_fonction_t)f),
               t ? t : "active", (!banc_fonctions_en_selection() && t != NULL && choisie &&
                                  banc_f_etat((banc_fonction_t)f) == BANC_FE_DESACTIVEE) ? " (dépendance)" : "");
    }
    return 0;
}

static int cmd_redemarrer(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (banc_enregistrement_en_cours()) {
        printf("Enregistrement en cours : réessayer à la fin\n");
        return 1;
    }
    printf("Redémarrage...\n");
    fflush(stdout);
    nid_storage_unmount();
    esp_restart();
    return 0;
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
    char txt[32];
    banc_heure_locale(txt, sizeof(txt), "%Y-%m-%d %H:%M:%S");
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
    if (banc_regler_heure(t, "console du banc") != ESP_OK) {
        printf("Réglage de l'heure refusé\n");
        return 1;
    }
    afficher_heure();
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Table des commandes (console série et console web)                         */
/* ------------------------------------------------------------------------- */

#define LIGNE_MAX           160
#define ARGS_MAX            12

static const esp_console_cmd_t s_cmds[] = {
    { .command = "video", .help = "Vidéo MP4 manuelle (durée par défaut : menu « Banc d'essai ») ; « video stop »",
      .hint = "[durée_s|stop]", .func = cmd_video },
    { .command = "photo", .help = "Photo JPEG sur la carte SD", .func = cmd_photo },
    { .command = "armer", .help = "Arme / désarme la surveillance PIR (sans argument : état du PIR et compteurs)",
      .hint = "[on|off]", .func = cmd_armer },
    { .command = "compteurs", .help = "Compteurs de détection, latence ; « raz » pour les remettre à zéro",
      .hint = "[raz]", .func = cmd_compteurs },
    { .command = "regle", .help = "Réglages de détection, suivi et nuit à chaud (clés de config.ini)",
      .hint = "[clé valeur]", .func = cmd_regle },
    { .command = "suivi", .help = "Intervalle des photos de suivi en minutes (0 = arrêt)", .hint = "<min>",
      .func = cmd_suivi },
    { .command = "nuit", .help = "Mode jour / nuit / automatique (filtre, LED IR, monochrome)",
      .hint = "[on|off|auto]", .func = cmd_nuit },
    { .command = "ir", .help = "Puissance des LED IR en % (test direct)", .hint = "<0-100>", .func = cmd_ir },
    { .command = "ircut", .help = "Filtre IR-cut (test direct)", .hint = "jour|nuit", .func = cmd_ircut },
    { .command = "mono", .help = "Image monochrome par l'ISP (test direct)", .hint = "on|off", .func = cmd_mono },
    { .command = "soleil", .help = "Lever et coucher du soleil du jour, décision jour/nuit du firmware",
      .func = cmd_soleil },
    { .command = "son", .help = "Niveau du micro (1 s) ; « son video [s] » : vidéo de test du son",
      .hint = "[video [durée_s]]", .func = cmd_son },
    { .command = "batterie", .help = "Tension de la batterie (ou « non câblée »)", .func = cmd_batterie },
    { .command = "temp", .help = "Température de la puce", .func = cmd_temp },
    { .command = "sd", .help = "Volume de la carte ; « sd test [Mo] » : débit ; « sd scan » : recompte le libre",
      .hint = "[test [Mo]|scan]", .func = cmd_sd },
    { .command = "boucle", .help = "Endurance : N vidéos d'affilée, bilan mémoire et images perdues",
      .hint = "N [durée_s] | stop", .func = cmd_boucle },
    { .command = "ecran", .help = "Onglet de l'écran", .hint = "detection|photo|nuit|son|systeme|wifi",
      .func = cmd_ecran },
    { .command = "fonctions", .help = "Fonctions à tester : liste ; on|off <nom> (mémorisé) ; go (fin de la sélection)",
      .hint = "[on|off <nom> | go]", .func = cmd_fonctions },
#if CONFIG_BANC_F_WIFI
    { .command = "wifi", .help = "Wi-Fi : état (mode, SSID, adresse, clients, firmware du C6) ; ap ; sta <ssid> [mdp] "
                              "(mémorisé) ; oublier ; on|off",
      .hint = "[ap | sta <ssid> [mdp] | oublier | on | off]", .func = banc_wifi_commande },
#endif
    { .command = "redemarrer", .help = "Redémarre la carte (carte SD démontée proprement)", .func = cmd_redemarrer },
    { .command = "info", .help = "Capteur, réglages, compteurs, carte SD, mémoire, version, dernier bilan",
      .func = cmd_info },
    { .command = "i2cscan", .help = "Liste les périphériques du bus I2C partagé (caméra, codec, tactile, horloge)",
      .func = cmd_i2cscan },
    { .command = "heure", .help = "Affiche ou règle l'heure locale", .hint = "[AAAA-MM-JJ HH:MM:SS]",
      .func = cmd_heure },
};

#define NB_CMDS (sizeof(s_cmds) / sizeof(s_cmds[0]))

/** « help » de la console web (la console série garde celui d'esp_console). */
static void aide(void)
{
    for (size_t i = 0; i < NB_CMDS; i++) {
        printf("%s %s\n  %s\n", s_cmds[i].command, s_cmds[i].hint ? s_cmds[i].hint : "", s_cmds[i].help);
    }
}

esp_err_t banc_console_executer(const char *ligne, int *code)
{
    *code = 1;
    const size_t n = strlen(ligne);
    if (n == 0 || n >= LIGNE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Copie locale : esp_console_run() partage un tampon global avec la console série. */
    char buf[LIGNE_MAX];
    strlcpy(buf, ligne, sizeof(buf));
    char *argv[ARGS_MAX + 1] = { 0 };
    const size_t argc = esp_console_split_argv(buf, argv, ARGS_MAX + 1);
    if (argc == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (argc > ARGS_MAX) {
        printf("Trop d'arguments (%d au plus)\n", ARGS_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "aide") == 0) {
        aide();
        *code = 0;
        return ESP_OK;
    }
    for (size_t i = 0; i < NB_CMDS; i++) {
        if (strcmp(argv[0], s_cmds[i].command) == 0) {
            *code = s_cmds[i].func((int)argc, argv);
            return ESP_OK;
        }
    }
    printf("Commande inconnue : %s (« help » pour la liste)\n", argv[0]);
    return ESP_ERR_NOT_FOUND;
}

void banc_console_masquer(const char *ligne, char *out, size_t len)
{
    strlcpy(out, ligne, len);
    /* « wifi sta <ssid> <mdp> » : le mot de passe n'apparaît dans aucun journal. */
    char *p = out;
    while (*p == ' ') {
        p++;
    }
    if (strncmp(p, "wifi", 4) != 0 || (p[4] != ' ' && p[4] != '\0')) {
        return;
    }
    p += 4;
    while (*p == ' ') {
        p++;
    }
    if (strncmp(p, "sta ", 4) != 0) {
        return;
    }
    p += 4;
    while (*p == ' ') {
        p++;
    }
    /* SSID (éventuellement entre guillemets), puis tout le reste masqué. */
    if (*p == '"') {
        char *fin = strchr(p + 1, '"');
        p = (fin != NULL) ? fin + 1 : p + strlen(p);
    } else {
        while (*p != '\0' && *p != ' ') {
            p++;
        }
    }
    if (*p != '\0' && (size_t)(p - out) + 5 < len) {
        strcpy(p, " ****");
    }
}

/* ------------------------------------------------------------------------- */
/* Démarrage                                                                  */
/* ------------------------------------------------------------------------- */

esp_err_t banc_console_demarrer(void)
{
    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "commande help");
    for (size_t i = 0; i < NB_CMDS; i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&s_cmds[i]), TAG, "commande %s", s_cmds[i].command);
    }

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "banc>";
    cfg.max_cmdline_length = 128;
    cfg.task_stack_size = 6144;
    ESP_RETURN_ON_ERROR(esp_console_new_repl_stdio(&cfg, &repl), TAG, "console");
    return esp_console_start_repl(repl);
}
