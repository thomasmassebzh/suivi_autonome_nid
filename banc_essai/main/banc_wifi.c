/*
 * banc_wifi — Wi-Fi du banc d'essai par le coprocesseur ESP32-C6
 *
 * Réutilise le composant commun du firmware nid_wifi (démarrage du C6,
 * ESP-Hosted, point d'accès 192.168.4.1 + DHCP, station, arrêt propre), le
 * même que le mode installation (nid_install).
 *
 * Tâche « banc_wifi » (priorité basse) : démarre le Wi-Fi sans bloquer le
 * démarrage du banc, puis le serveur web (banc_web.c) ; surveille la liaison
 * avec le C6 (en défaut si elle est perdue) et la puissance reçue en station ;
 * applique les demandes de la console (« wifi ap|sta|oublier|on|off »), les
 * seules à appeler nid_wifi_start() / nid_wifi_stop().
 *
 * Réglages : menu « Banc d'essai › Wi-Fi et tableau de bord web » ; le mode
 * station choisi par « wifi sta <ssid> <mdp> » est mémorisé en NVS (espace
 * « banc ») et prioritaire sur le menu ; « wifi oublier » revient au menu.
 * Le mot de passe n'est jamais affiché ni renvoyé par la page web.
 */
#include "banc.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_BANC_F_WIFI

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "banc_wifi";

#define TACHE_PILE          4096
#define TACHE_PRIO          2
#define PERIODE_MS          1000
#define RSSI_PERIODE_US     (10LL * 1000000LL)

#define NVS_ESPACE          "banc"
#define NVS_MODE            "wifi_mode"     /* u8 : 0 = menu, 1 = point d'accès, 2 = station */
#define NVS_SSID            "wifi_ssid"
#define NVS_MDP             "wifi_mdp"

#if CONFIG_BANC_WIFI_STA_REPLI
#define REPLI_TEXTE         "retour au point d'accès"
#else
#define REPLI_TEXTE         "nouveaux essais"
#endif

typedef enum {
    SOURCE_MENU = 0,
    SOURCE_AP,
    SOURCE_STA,
} source_t;

static TaskHandle_t s_tache;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static banc_wifi_etat_t s_etat;             /* sous s_mux (sauf st, lu chez nid_wifi) */
static volatile bool s_voulu = true;        /* « wifi on|off » */
static volatile bool s_reconfigurer;        /* mode ou réseau changé */
static uint32_t s_majeur_hote;              /* version majeure de l'hôte ESP-Hosted (P4) */

/* ------------------------------------------------------------------------- */
/* Réglages (menu, NVS)                                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    nid_wifi_mode_t mode;
    char            ssid[NID_WIFI_SSID_LEN];
    char            mdp[NID_WIFI_PASS_LEN];
    bool            nvs;                    /*!< réseau mémorisé par « wifi sta » */
} reglage_t;

static void lire_reglage(reglage_t *r)
{
    memset(r, 0, sizeof(*r));
#if CONFIG_BANC_WIFI_MODE_STA
    r->mode = NID_WIFI_MODE_STA;
    strlcpy(r->ssid, CONFIG_BANC_WIFI_STA_SSID, sizeof(r->ssid));
    strlcpy(r->mdp, CONFIG_BANC_WIFI_STA_MDP, sizeof(r->mdp));
#else
    r->mode = NID_WIFI_MODE_AP;
#endif
    nvs_handle_t h;
    if (nvs_open(NVS_ESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t source = SOURCE_MENU;
    if (nvs_get_u8(h, NVS_MODE, &source) == ESP_OK) {
        if (source == SOURCE_AP) {
            r->mode = NID_WIFI_MODE_AP;
            r->nvs = true;
        } else if (source == SOURCE_STA) {
            size_t l1 = sizeof(r->ssid), l2 = sizeof(r->mdp);
            char ssid[NID_WIFI_SSID_LEN] = "", mdp[NID_WIFI_PASS_LEN] = "";
            if (nvs_get_str(h, NVS_SSID, ssid, &l1) == ESP_OK && ssid[0] != '\0') {
                nvs_get_str(h, NVS_MDP, mdp, &l2);
                r->mode = NID_WIFI_MODE_STA;
                strlcpy(r->ssid, ssid, sizeof(r->ssid));
                strlcpy(r->mdp, mdp, sizeof(r->mdp));
                r->nvs = true;
            }
        }
    }
    nvs_close(h);
}

static esp_err_t memoriser(source_t source, const char *ssid, const char *mdp)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_ESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (source == SOURCE_MENU) {
        nvs_erase_key(h, NVS_SSID);
        nvs_erase_key(h, NVS_MDP);
        err = nvs_erase_key(h, NVS_MODE);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_u8(h, NVS_MODE, (uint8_t)source);
        if (err == ESP_OK && source == SOURCE_STA) {
            err = nvs_set_str(h, NVS_SSID, ssid);
            if (err == ESP_OK) {
                err = nvs_set_str(h, NVS_MDP, mdp);
            }
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/** SSID 1 à 32 caractères ; mot de passe vide (réseau ouvert) ou 8 à 63 caractères ASCII imprimables. */
static bool reglage_valide(const char *ssid, const char *mdp)
{
    const size_t ls = strlen(ssid), lm = strlen(mdp);
    if (ls == 0 || ls > 32 || (lm != 0 && (lm < 8 || lm > 63))) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)mdp; *p; p++) {
        if (*p < 0x20 || *p > 0x7E) {
            return false;
        }
    }
    for (const unsigned char *p = (const unsigned char *)ssid; *p; p++) {
        if (*p < 0x20) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Version du C6                                                              */
/* ------------------------------------------------------------------------- */

static void lire_version_c6(void)
{
    char v[NID_WIFI_VERSION_LEN];
    uint32_t majeur = 0;
    const esp_err_t err = nid_wifi_query_cp_version(v, sizeof(v), &majeur);
    char compat[sizeof(s_etat.compat)];
    bool ok = false;
    if (err != ESP_OK) {
        strlcpy(compat, "version du C6 inconnue : firmware trop ancien ? (voir README)", sizeof(compat));
    } else if (majeur >= s_majeur_hote) {
        snprintf(compat, sizeof(compat), "compatible (même génération que l'hôte %" PRIu32 ".x)", s_majeur_hote);
        ok = true;
    } else if (majeur == 2) {
        strlcpy(compat, "2.x : rétrocompatible (hôte 3.x), à valider sur la carte", sizeof(compat));
        ok = true;
    } else {
        strlcpy(compat, "trop ancien : mettre à jour le firmware ESP-Hosted du C6", sizeof(compat));
    }
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_etat.version_c6, v, sizeof(s_etat.version_c6));
    strlcpy(s_etat.compat, compat, sizeof(s_etat.compat));
    s_etat.compat_ok = ok;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "WIFI firmware_c6=%s hote=%s : %s", v, s_etat.version_hote, compat);
}

/* ------------------------------------------------------------------------- */
/* Tâche                                                                      */
/* ------------------------------------------------------------------------- */

static void config_ap(nid_wifi_config_t *c)
{
    *c = (nid_wifi_config_t) {
        .mode = NID_WIFI_MODE_AP,
        .ssid = CONFIG_BANC_WIFI_AP_SSID,
        .password = CONFIG_BANC_WIFI_AP_MDP,
        .channel = CONFIG_BANC_WIFI_AP_CANAL,
        .max_clients = CONFIG_BANC_WIFI_AP_CLIENTS,
    };
}

static void journal_etat(void)
{
    nid_wifi_status_t st;
    nid_wifi_get_status(&st);
    if (st.state == NID_WIFI_STATE_UP) {
        ESP_LOGI(TAG, "WIFI mode=%s ssid=%s ip=%s : ouvrir http://%s", st.mode == NID_WIFI_MODE_AP ? "AP" : "station",
                 st.ssid, st.ip, st.ip);
        nid_storage_journal("wifi_banc", "%s « %s » prêt, adresse %s", st.mode == NID_WIFI_MODE_AP ? "point d'accès" :
                            "station", st.ssid, st.ip);
    } else {
        ESP_LOGW(TAG, "WIFI mode=%s ssid=%s etat=%s erreur=%s", st.mode == NID_WIFI_MODE_AP ? "AP" : "station", st.ssid,
                 nid_wifi_state_str(st.state), st.error[0] ? st.error : "-");
    }
}

/** Démarre (ou change de mode). @return true si le Wi-Fi est en service (prêt ou station en connexion). */
static bool demarrer(bool premier)
{
    reglage_t r;
    lire_reglage(&r);
    nid_wifi_config_t c;
    config_ap(&c);
    bool repli = false;
    esp_err_t err;
    if (r.mode == NID_WIFI_MODE_STA) {
        if (!reglage_valide(r.ssid, r.mdp)) {
            ESP_LOGW(TAG, "réseau de la station non réglé ou invalide (menu ou « wifi sta <ssid> <mdp> ») : "
                     "point d'accès");
            err = nid_wifi_start(&c);
            repli = true;
        } else {
            c = (nid_wifi_config_t) {
                .mode = NID_WIFI_MODE_STA,
                .ssid = r.ssid,
                .password = r.mdp,
                .sta_timeout_ms = CONFIG_BANC_WIFI_STA_DELAI_S * 1000u,
            };
            err = nid_wifi_start(&c);
#if CONFIG_BANC_WIFI_STA_REPLI
            if (err == ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "réseau « %s » introuvable en %d s : repli sur le point d'accès « %s »", r.ssid,
                         CONFIG_BANC_WIFI_STA_DELAI_S, CONFIG_BANC_WIFI_AP_SSID);
                config_ap(&c);
                err = nid_wifi_start(&c);
                repli = true;
            }
#endif
        }
    } else {
        err = nid_wifi_start(&c);
    }
    memset(r.mdp, 0, sizeof(r.mdp));
    portENTER_CRITICAL(&s_mux);
    s_etat.repli_ap = repli;
    s_etat.rssi_ok = false;
    portEXIT_CRITICAL(&s_mux);

    const bool en_service = (err == ESP_OK) || (err == ESP_ERR_TIMEOUT && !repli && r.mode == NID_WIFI_MODE_STA);
    journal_etat();
    if (!en_service) {
        nid_wifi_status_t st;
        nid_wifi_get_status(&st);
        nid_storage_journal("erreur", "Wi-Fi du banc indisponible : %s", st.error[0] ? st.error : esp_err_to_name(err));
        banc_f_defaut(BANC_F_WIFI, err);
        nid_wifi_stop();
        return false;
    }
    banc_f_retablir(BANC_F_WIFI);
    if (premier) {
        lire_version_c6();
    }
    err = banc_web_demarrer();
    portENTER_CRITICAL(&s_mux);
    s_etat.web = (err == ESP_OK);
    portEXIT_CRITICAL(&s_mux);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "serveur web indisponible : %s", esp_err_to_name(err));
    }
    return true;
}

static void tache_wifi(void *arg)
{
    (void)arg;
    bool en_service = false;
    bool version_lue = false;
    int64_t prochain_rssi = 0;
    for (;;) {
        if (s_voulu && (!en_service || s_reconfigurer)) {
            s_reconfigurer = false;
            en_service = demarrer(!version_lue);
            version_lue = version_lue || en_service;
            if (!en_service) {
                s_voulu = false;    /* pas de nouvel essai automatique : « wifi on » */
            }
        } else if (!s_voulu && en_service) {
            ESP_LOGI(TAG, "arrêt du Wi-Fi demandé : C6 remis en reset");
            nid_wifi_stop();
            en_service = false;
        }

        if (en_service && nid_wifi_link_lost()) {
            ESP_LOGE(TAG, "liaison avec le C6 perdue : Wi-Fi arrêté (« wifi on » pour réessayer)");
            nid_storage_journal("erreur", "liaison SDIO avec le coprocesseur Wi-Fi perdue");
            banc_f_defaut(BANC_F_WIFI, ESP_ERR_INVALID_RESPONSE);
            nid_wifi_stop();
            en_service = false;
            s_voulu = false;
        }

        const int64_t now = esp_timer_get_time();
        if (en_service && now >= prochain_rssi) {
            prochain_rssi = now + RSSI_PERIODE_US;
            int rssi = 0;
            const bool ok = (nid_wifi_sta_rssi(&rssi) == ESP_OK);
            portENTER_CRITICAL(&s_mux);
            s_etat.rssi_ok = ok;
            s_etat.rssi = rssi;
            portEXIT_CRITICAL(&s_mux);
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PERIODE_MS));
    }
}

esp_err_t banc_wifi_demarrer(void)
{
    if (s_tache != NULL) {
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_mux);
    s_etat.demarre = true;
    s_etat.voulu = true;
    strlcpy(s_etat.version_c6, "?", sizeof(s_etat.version_c6));
    strlcpy(s_etat.compat, "C6 non interrogé", sizeof(s_etat.compat));
    portEXIT_CRITICAL(&s_mux);
    s_majeur_hote = nid_wifi_host_version(s_etat.version_hote, sizeof(s_etat.version_hote));
    if (xTaskCreate(tache_wifi, "banc_wifi", TACHE_PILE, NULL, TACHE_PRIO, &s_tache) != pdPASS) {
        s_tache = NULL;
        portENTER_CRITICAL(&s_mux);
        s_etat.demarre = false;
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void banc_wifi_etat(banc_wifi_etat_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_etat;
    portEXIT_CRITICAL(&s_mux);
    out->voulu = s_voulu;
    nid_wifi_get_status(&out->st);
    out->requetes = banc_web_requetes();
    out->vue_client = banc_web_vue_active();
}

/* ------------------------------------------------------------------------- */
/* Console « wifi »                                                           */
/* ------------------------------------------------------------------------- */

static void afficher(void)
{
    banc_wifi_etat_t w;
    banc_wifi_etat(&w);
    if (!w.demarre) {
        const char *t = banc_f_texte(BANC_F_WIFI);
        printf("Wi-Fi : fonction %s (écran de sélection ou « fonctions on wifi » puis « redemarrer »)\n",
               t ? t : "non démarrée");
        return;
    }
    reglage_t r;
    lire_reglage(&r);
    memset(r.mdp, 0, sizeof(r.mdp));
    printf("Mode      : %s%s ; état : %s%s\n", w.st.mode == NID_WIFI_MODE_AP ? "point d'accès" : "station",
           w.repli_ap ? " (repli : réseau de la station introuvable)" : "", nid_wifi_state_str(w.st.state),
           w.voulu ? "" : " (arrêt demandé : « wifi on »)");
    printf("SSID      : %s%s\n", w.st.ssid[0] ? w.st.ssid : "-",
           w.st.mode == NID_WIFI_MODE_AP ? " (WPA2, mot de passe : menu « Banc d'essai › Wi-Fi »)" : "");
    printf("Adresse   : %s%s%s\n", w.st.ip[0] ? "http://" : "", w.st.ip[0] ? w.st.ip : "aucune",
           w.web ? "" : " (serveur web non démarré)");
    if (w.st.mode == NID_WIFI_MODE_AP) {
        printf("Clients   : %d connecté(s), canal %u\n", w.st.clients, w.st.channel);
    } else {
        char signal[16] = "inconnu";
        if (w.rssi_ok) {
            snprintf(signal, sizeof(signal), "%d dBm", w.rssi);
        }
        printf("Signal    : %s ; %" PRIu32 " déconnexion(s), dernier motif %u\n", signal, w.st.sta_disconnects,
               w.st.sta_last_reason);
    }
    printf("Lien C6   : %s\n", nid_wifi_link_lost() ? "PERDU (« wifi on » pour réessayer)" : "ok (SDIO)");
    printf("ESP-Hosted: C6 %s, hôte %s : %s\n", w.version_c6, w.version_hote, w.compat);
    printf("Page web  : %" PRIu32 " requête(s), vue caméra %s\n", w.requetes, w.vue_client ? "ouverte" : "fermée");
    printf("Réglage   : %s%s\n", r.mode == NID_WIFI_MODE_AP ? "point d'accès" : "station",
           r.nvs ? " (mémorisé par « wifi », « wifi oublier » pour revenir au menu)" : " (menu « Banc d'essai »)");
    if (w.st.error[0]) {
        printf("Erreur    : %s\n", w.st.error);
    }
}

static void relancer(void)
{
    s_reconfigurer = true;
    s_voulu = true;
    if (s_tache != NULL) {
        xTaskNotifyGive(s_tache);
    }
}

int banc_wifi_commande(int argc, char **argv)
{
    if (argc == 1) {
        afficher();
        return 0;
    }
    if (s_tache == NULL) {
        afficher();
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "ap") == 0) {
        if (memoriser(SOURCE_AP, NULL, NULL) != ESP_OK) {
            printf("NVS indisponible : réglage non mémorisé\n");
        }
        relancer();
        printf("Passage en point d'accès « %s » (adresse 192.168.4.1) : quelques secondes\n", CONFIG_BANC_WIFI_AP_SSID);
        return 0;
    }
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "sta") == 0) {
        const char *mdp = (argc == 4) ? argv[3] : "";
        if (!reglage_valide(argv[2], mdp)) {
            printf("Réglage refusé : SSID de 1 à 32 caractères, mot de passe vide (réseau ouvert) ou de 8 à 63 "
                   "caractères ASCII ; SSID avec espaces entre guillemets\n");
            return 1;
        }
        if (memoriser(SOURCE_STA, argv[2], mdp) != ESP_OK) {
            printf("NVS indisponible : réglage non mémorisé\n");
            return 1;
        }
        relancer();
        printf("Connexion au réseau « %s » (mémorisé) : adresse affichée par « wifi » et à l'écran ; "
               "introuvable en %d s : %s\n", argv[2], CONFIG_BANC_WIFI_STA_DELAI_S, REPLI_TEXTE);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "oublier") == 0) {
        if (memoriser(SOURCE_MENU, NULL, NULL) != ESP_OK) {
            printf("NVS indisponible\n");
            return 1;
        }
        relancer();
        printf("Réglage mémorisé effacé : mode du menu « Banc d'essai »\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        relancer();
        printf("Démarrage du Wi-Fi demandé\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        s_voulu = false;
        xTaskNotifyGive(s_tache);
        printf("Arrêt du Wi-Fi demandé (C6 remis en reset) ; la page web ne répondra plus\n");
        return 0;
    }
    printf("Usage : wifi [ap | sta <ssid> [mdp] | oublier | on | off]\n");
    return 1;
}

#endif /* CONFIG_BANC_F_WIFI */
