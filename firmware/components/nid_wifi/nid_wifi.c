/*
 * nid_wifi — Wi-Fi de l'ESP32-P4 par le coprocesseur ESP32-C6
 *
 * L'ESP32-P4 n'a pas de radio : le Wi-Fi passe par l'ESP32-C6 de la carte,
 * relié en SDIO (slot 1) et piloté par ESP-Hosted ; les appels esp_wifi_*
 * sont transmis au C6 (Wi-Fi « distant » intégré à esp_wifi dans IDF v6.1).
 *
 * Rien n'est démarré au boot (CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN
 * désactivé, vérifié ci-dessous) : le réveil de veille et le délai
 * détection → première image (≤ 1 s) ne sont pas affectés. Séquence :
 *   1. NVS, pile réseau, boucle d'événements par défaut ;
 *   2. C6 sorti du reset (nid_board), transport ESP-Hosted : impulsion de reset
 *      sur la même broche (GPIO 54), énumération SDIO, échange des capacités ;
 *   3. interface réseau (AP : 192.168.4.1 et serveur DHCP ; STA : client
 *      DHCP), esp_wifi_init / set_mode / set_config / start (RPC vers le C6),
 *      attente de AP_START (AP) ou de l'adresse IP (STA).
 * Changement de mode sans arrêter le C6 : esp_wifi_stop, nouvelle interface,
 * set_mode / set_config / start. L'arrêt suit l'ordre inverse et remet
 * toujours le C6 en reset.
 *
 * Compatibilité : hôte ESP-Hosted 3.x avec le firmware 2.x livré sur le C6
 * (rétrocompatibilité documentée par Espressif ; en-tête de trame V1, RPC sans
 * négociation de version, réception SDIO en mode « streaming »).
 */
#include "nid_wifi.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "nid_board.h"

/* --------------------------------------------------------------------------
 * Garde-fous de configuration (sdkconfig.defaults, section « Wi-Fi ») : une
 * configuration régénérée sans ces réglages ne doit pas compiler.
 * -------------------------------------------------------------------------- */
#if CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN
#error "ESP-Hosted ne doit pas démarrer avant app_main (réveil ≤ 1 s) : désactiver CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN"
#endif
#if CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE
#error "Une panne du transport ne doit pas redémarrer tout le système : désactiver CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE"
#endif
#if !CONFIG_ESP_HOSTED_HOST_CP_BRINGUP_ON_TIMEOUT_NONE
#error "Coprocesseur muet : ESP-Hosted doit rendre la main (CONFIG_ESP_HOSTED_HOST_CP_BRINGUP_ON_TIMEOUT_NONE), pas redémarrer le système"
#endif
#if CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO && !CONFIG_ESP_HOSTED_HOST_SDIO_RX_STREAMING_MODE
#error "Le firmware 2.x du C6 émet en mode streaming : CONFIG_ESP_HOSTED_HOST_SDIO_RX_STREAMING_MODE requis (sinon assert dans ESP-Hosted)"
#endif
#if defined(CONFIG_ESP_HOSTED_HOST_RESET_GPIO) && (CONFIG_NID_PIN_C6_RESET >= 0) && \
    (CONFIG_ESP_HOSTED_HOST_RESET_GPIO != CONFIG_NID_PIN_C6_RESET)
#error "Broche de reset du C6 différente entre nid_board (CONFIG_NID_PIN_C6_RESET) et ESP-Hosted (CONFIG_ESP_HOSTED_HOST_RESET_GPIO)"
#endif

static const char *TAG = "nid_wifi";

#define LINK_SETTLE_MS        200    /* enregistrement des événements Wi-Fi d'ESP-Hosted */
#define AP_START_TIMEOUT_MS   5000
#define WIFI_STOP_TIMEOUT_MS  1500
#define NETIF_UP_TIMEOUT_MS   2000
#define STA_DEFAULT_WAIT_MS   15000
#define POLL_MS               50

#define EV_AP_START   BIT0
#define EV_AP_STOP    BIT1
#define EV_STA_IP     BIT2
#define EV_STA_STOP   BIT3

static struct {
    EventGroupHandle_t           ev;
    esp_event_handler_instance_t h_wifi;
    esp_event_handler_instance_t h_ip;
    esp_event_handler_instance_t h_hosted;
    esp_netif_t                 *netif;
    nid_wifi_mode_t              netif_mode;   /*!< mode de l'interface netif */
    bool                         coproc_on;
    bool                         hosted_init;
    bool                         wifi_init;
    bool                         wifi_started;
    bool                         sta_reconnect;  /*!< station : nouvel essai après une déconnexion */
} s_w;

/* État publié (nid_wifi_get_status), sous s_mux. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static nid_wifi_status_t s_st;
static atomic_bool s_link_lost;

/* -------------------------------------------------------------------------- */

static void set_state(nid_wifi_state_t state)
{
    portENTER_CRITICAL(&s_mux);
    s_st.state = state;
    portEXIT_CRITICAL(&s_mux);
}

static void set_ip(const char *ip)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_st.ip, ip, sizeof(s_st.ip));
    portEXIT_CRITICAL(&s_mux);
}

static void set_error(const char *what, esp_err_t err)
{
    char txt[NID_WIFI_ERROR_LEN];
    snprintf(txt, sizeof(txt), "%s (%s)", what, esp_err_to_name(err));
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_st.error, txt, sizeof(s_st.error));
    s_st.state = NID_WIFI_STATE_FAILED;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGE(TAG, "Wi-Fi : %s", txt);
}

const char *nid_wifi_error(void)
{
    /* Lu par la tâche qui a appelé nid_wifi_start(), après son retour. */
    return s_st.error[0] ? s_st.error : "cause inconnue";
}

bool nid_wifi_link_lost(void)
{
    return atomic_load(&s_link_lost);
}

void nid_wifi_get_status(nid_wifi_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

const char *nid_wifi_state_str(nid_wifi_state_t state)
{
    switch (state) {
    case NID_WIFI_STATE_STARTING:
        return "démarrage";
    case NID_WIFI_STATE_UP:
        return "prêt";
    case NID_WIFI_STATE_CONNECTING:
        return "connexion en cours";
    case NID_WIFI_STATE_FAILED:
        return "en échec";
    case NID_WIFI_STATE_LINK_LOST:
        return "liaison avec le C6 perdue";
    default:
        return "arrêté";
    }
}

static void clients_add(int delta)
{
    portENTER_CRITICAL(&s_mux);
    s_st.clients += delta;
    if (s_st.clients < 0) {
        s_st.clients = 0;
    }
    portEXIT_CRITICAL(&s_mux);
}

/* Boucle d'événements par défaut : Wi-Fi (relayé par ESP-Hosted), IP, ESP-Hosted. */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "point d'accès démarré");
            if (s_w.ev) {
                xEventGroupSetBits(s_w.ev, EV_AP_START);
            }
            break;
        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG, "point d'accès arrêté");
            if (s_w.ev) {
                xEventGroupSetBits(s_w.ev, EV_AP_STOP);
            }
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            const wifi_event_ap_staconnected_t *e = data;
            ESP_LOGI(TAG, "appareil associé " MACSTR " (AID %u)", MAC2STR(e->mac), (unsigned)e->aid);
            clients_add(1);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            const wifi_event_ap_stadisconnected_t *e = data;
            ESP_LOGI(TAG, "appareil déconnecté " MACSTR " (raison %u)", MAC2STR(e->mac), (unsigned)e->reason);
            clients_add(-1);
            break;
        }
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "station démarrée : connexion au réseau");
            if (s_w.sta_reconnect) {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "station associée : attente de l'adresse (DHCP)");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *e = data;
            portENTER_CRITICAL(&s_mux);
            s_st.sta_disconnects++;
            s_st.sta_last_reason = e->reason;
            s_st.ip[0] = '\0';
            if (s_st.state == NID_WIFI_STATE_UP) {
                s_st.state = NID_WIFI_STATE_CONNECTING;
            }
            portEXIT_CRITICAL(&s_mux);
            if (s_w.ev) {
                xEventGroupClearBits(s_w.ev, EV_STA_IP);
            }
            ESP_LOGW(TAG, "station déconnectée (raison %u)%s", (unsigned)e->reason,
                     s_w.sta_reconnect ? " : nouvel essai" : "");
            if (s_w.sta_reconnect) {
                /* Un essai dure quelques secondes (recherche du réseau) : pas de boucle serrée. */
                esp_wifi_connect();
            }
            break;
        }
        case WIFI_EVENT_STA_STOP:
            if (s_w.ev) {
                xEventGroupSetBits(s_w.ev, EV_STA_STOP);
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
            const ip_event_assigned_ip_to_client_t *e = data;
            ESP_LOGI(TAG, "adresse " IPSTR " attribuée à " MACSTR, IP2STR(&e->ip), MAC2STR(e->mac));
        } else if (id == IP_EVENT_STA_GOT_IP) {
            const ip_event_got_ip_t *e = data;
            char ip[NID_WIFI_IP_LEN];
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
            portENTER_CRITICAL(&s_mux);
            strlcpy(s_st.ip, ip, sizeof(s_st.ip));
            s_st.state = NID_WIFI_STATE_UP;
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "station connectée : adresse %s", ip);
            if (s_w.ev) {
                xEventGroupSetBits(s_w.ev, EV_STA_IP);
            }
        } else if (id == IP_EVENT_STA_LOST_IP) {
            set_ip("");
            ESP_LOGW(TAG, "station : adresse perdue");
        }
    } else if (base == ESP_HOSTED_EVENT) {
        if (id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE) {
            ESP_LOGE(TAG, "liaison SDIO avec le coprocesseur en panne");
            atomic_store(&s_link_lost, true);
            set_state(NID_WIFI_STATE_LINK_LOST);
        } else if (id == ESP_HOSTED_EVENT_CP_INIT) {
            ESP_LOGI(TAG, "coprocesseur initialisé");
        }
    }
}

static esp_err_t register_handlers(void)
{
    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, &s_w.h_wifi);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, &s_w.h_ip);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, &s_w.h_hosted);
    }
    return err;
}

static void unregister_handlers(void)
{
    if (s_w.h_wifi) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_w.h_wifi);
        s_w.h_wifi = NULL;
    }
    if (s_w.h_ip) {
        esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_w.h_ip);
        s_w.h_ip = NULL;
    }
    if (s_w.h_hosted) {
        esp_event_handler_instance_unregister(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, s_w.h_hosted);
        s_w.h_hosted = NULL;
    }
}

/* -------------------------------------------------------------------------- */
/* Transport et interface                                                     */
/* -------------------------------------------------------------------------- */

/** Étapes 1 et 2 : pile réseau, événements, C6 et transport ESP-Hosted. */
static esp_err_t transport_start(int64_t t0)
{
    s_w.ev = xEventGroupCreate();
    if (s_w.ev == NULL) {
        set_error("mémoire insuffisante", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    /* NVS (le Wi-Fi distant n'en dépend pas : jamais effacée ici, la NVS
     * peut contenir l'état sauvegardé de l'application). */
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS indisponible (%s) : sans effet sur le Wi-Fi du coprocesseur", esp_err_to_name(err));
    }

    /* Pile réseau et boucle d'événements (créée seulement si absente). */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        set_error("pile réseau", err);
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        set_error("boucle d'événements", err);
        return err;
    }
    err = register_handlers();
    if (err != ESP_OK) {
        set_error("abonnement aux événements", err);
        return err;
    }

    /* Coprocesseur : sortie du reset, puis transport ESP-Hosted (qui
     * réinitialise lui-même le C6 par la même broche avant l'énumération). */
    err = nid_board_coproc_enable(true);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "sortie du reset du C6 : %s", esp_err_to_name(err));
    }
    s_w.coproc_on = true;

    int rc = esp_hosted_init();
    if (rc != 0) {
        set_error("initialisation du transport ESP-Hosted", ESP_FAIL);
        return ESP_FAIL;
    }
    s_w.hosted_init = true;

    rc = esp_hosted_connect_to_slave();
    if (rc != 0) {
        set_error("le coprocesseur ESP32-C6 ne répond pas", ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "coprocesseur ESP32-C6 prêt en %" PRId64 " ms", (esp_timer_get_time() - t0) / 1000);
    vTaskDelay(pdMS_TO_TICKS(LINK_SETTLE_MS));
    return ESP_OK;
}

/** Interface réseau du mode demandé (clés « WIFI_AP_DEF » / « WIFI_STA_DEF » attendues par ESP-Hosted). */
static esp_err_t netif_create(nid_wifi_mode_t mode)
{
    esp_netif_config_t ap_cfg = ESP_NETIF_DEFAULT_WIFI_AP();
    esp_netif_config_t sta_cfg = ESP_NETIF_DEFAULT_WIFI_STA();
    s_w.netif = esp_netif_new(mode == NID_WIFI_MODE_AP ? &ap_cfg : &sta_cfg);
    if (s_w.netif == NULL) {
        set_error("création de l'interface réseau", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    s_w.netif_mode = mode;
    esp_err_t err;
    if (mode == NID_WIFI_MODE_AP) {
        err = esp_netif_attach_wifi_ap(s_w.netif);
        if (err == ESP_OK) {
            err = esp_wifi_set_default_wifi_ap_handlers();
        }
    } else {
        err = esp_netif_attach_wifi_station(s_w.netif);
        if (err == ESP_OK) {
            err = esp_wifi_set_default_wifi_sta_handlers();
        }
    }
    if (err != ESP_OK) {
        set_error("liaison de l'interface réseau", err);
    }
    return err;
}

/**
 * Arrête le Wi-Fi du C6 sans le désinitialiser. replace_netif : changement de
 * mode, l'interface est détruite tout de suite (sinon nid_wifi_stop() la
 * détruit après esp_wifi_deinit, dans l'ordre d'origine).
 */
static void wifi_mode_stop(bool replace_netif)
{
    const bool lost = atomic_load(&s_link_lost);
    s_w.sta_reconnect = false;
    if (s_w.wifi_started) {
        /* Liaison perdue : inutile d'attendre la réponse du C6. */
        const EventBits_t bit = (s_w.netif_mode == NID_WIFI_MODE_AP) ? EV_AP_STOP : EV_STA_STOP;
        if (s_w.ev) {
            xEventGroupClearBits(s_w.ev, bit);
        }
        if (!lost && esp_wifi_stop() == ESP_OK && s_w.ev) {
            xEventGroupWaitBits(s_w.ev, bit, pdFALSE, pdTRUE, pdMS_TO_TICKS(WIFI_STOP_TIMEOUT_MS));
        }
        s_w.wifi_started = false;
    }
    if (s_w.netif && esp_netif_is_netif_up(s_w.netif)) {
        /* Événement d'arrêt non reçu : arrêt du DHCP et de l'interface. */
        if (s_w.netif_mode == NID_WIFI_MODE_AP) {
            esp_netif_action_stop(s_w.netif, WIFI_EVENT, WIFI_EVENT_AP_STOP, NULL);
        } else {
            esp_netif_action_stop(s_w.netif, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
        }
    }
    if (replace_netif && s_w.netif) {
        /* Changement de mode : l'interface est remplacée, le Wi-Fi reste initialisé. */
        esp_netif_destroy_default_wifi(s_w.netif);
        s_w.netif = NULL;
    }
    portENTER_CRITICAL(&s_mux);
    s_st.clients = 0;
    s_st.ip[0] = '\0';
    portEXIT_CRITICAL(&s_mux);
}

/** Étape 3 : Wi-Fi du C6 dans le mode demandé. */
static esp_err_t wifi_mode_start(const nid_wifi_config_t *cfg, int64_t t0)
{
    esp_err_t err;
    if (s_w.netif == NULL) {
        err = netif_create(cfg->mode);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (!s_w.wifi_init) {
        wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&icfg);
        if (err != ESP_OK) {
            set_error("esp_wifi_init", err);
            return err;
        }
        s_w.wifi_init = true;
    }

    wifi_config_t wc = { 0 };
    const size_t ssid_len = strlen(cfg->ssid);
    const size_t pass_len = strlen(cfg->password);
    if (cfg->mode == NID_WIFI_MODE_AP) {
        memcpy(wc.ap.ssid, cfg->ssid, ssid_len);
        wc.ap.ssid_len = (uint8_t)ssid_len;
        memcpy(wc.ap.password, cfg->password, pass_len);
        wc.ap.channel = cfg->channel;
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
        wc.ap.max_connection = cfg->max_clients;
        wc.ap.pmf_cfg.required = false;
        err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (err == ESP_OK) {
            err = esp_wifi_set_config(WIFI_IF_AP, &wc);
        }
    } else {
        memcpy(wc.sta.ssid, cfg->ssid, ssid_len);
        memcpy(wc.sta.password, cfg->password, pass_len);
        /* WPA2 au moins (WPA3 accepté) ; réseau ouvert si aucun mot de passe. */
        wc.sta.threshold.authmode = pass_len ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        wc.sta.pmf_cfg.capable = true;
        wc.sta.pmf_cfg.required = false;
        wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) {
            err = esp_wifi_set_config(WIFI_IF_STA, &wc);
        }
    }
    if (err != ESP_OK) {
        set_error(cfg->mode == NID_WIFI_MODE_AP ? "configuration du point d'accès" : "configuration de la station", err);
        return err;
    }

    xEventGroupClearBits(s_w.ev, EV_AP_START | EV_AP_STOP | EV_STA_IP | EV_STA_STOP);
    s_w.sta_reconnect = (cfg->mode == NID_WIFI_MODE_STA);
    err = esp_wifi_start();
    if (err != ESP_OK) {
        s_w.sta_reconnect = false;
        set_error("esp_wifi_start", err);
        return err;
    }
    s_w.wifi_started = true;

    if (cfg->mode == NID_WIFI_MODE_AP) {
        EventBits_t bits = xEventGroupWaitBits(s_w.ev, EV_AP_START, pdFALSE, pdTRUE, pdMS_TO_TICKS(AP_START_TIMEOUT_MS));
        if (!(bits & EV_AP_START)) {
            set_error("le point d'accès n'a pas démarré", ESP_ERR_TIMEOUT);
            return ESP_ERR_TIMEOUT;
        }
        /* Le gestionnaire par défaut démarre l'interface (DHCP) sur AP_START. */
        for (int t = 0; t < NETIF_UP_TIMEOUT_MS && !esp_netif_is_netif_up(s_w.netif); t += POLL_MS) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        }
        if (!esp_netif_is_netif_up(s_w.netif)) {
            ESP_LOGW(TAG, "interface du point d'accès pas encore active");
        }
        esp_netif_ip_info_t ipi = { 0 };
        char ip[NID_WIFI_IP_LEN] = "192.168.4.1";
        if (esp_netif_get_ip_info(s_w.netif, &ipi) == ESP_OK && ipi.ip.addr != 0) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ipi.ip));
        }
        portENTER_CRITICAL(&s_mux);
        strlcpy(s_st.ip, ip, sizeof(s_st.ip));
        s_st.state = NID_WIFI_STATE_UP;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "Wi-Fi « %s » prêt (canal %u, WPA2) en %" PRId64 " ms : http://%s", cfg->ssid,
                 (unsigned)cfg->channel, (esp_timer_get_time() - t0) / 1000, ip);
        return ESP_OK;
    }

    set_state(NID_WIFI_STATE_CONNECTING);
    const uint32_t wait_ms = cfg->sta_timeout_ms ? cfg->sta_timeout_ms : STA_DEFAULT_WAIT_MS;
    EventBits_t bits = xEventGroupWaitBits(s_w.ev, EV_STA_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(wait_ms));
    if (!(bits & EV_STA_IP)) {
        char txt[NID_WIFI_ERROR_LEN];
        nid_wifi_status_t st;
        nid_wifi_get_status(&st);
        snprintf(txt, sizeof(txt), "réseau « %s » introuvable ou refusé (motif %u)", cfg->ssid,
                 (unsigned)st.sta_last_reason);
        portENTER_CRITICAL(&s_mux);
        strlcpy(s_st.error, txt, sizeof(s_st.error));
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "Wi-Fi : %s ; nouveaux essais en arrière-plan", txt);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "station connectée à « %s » en %" PRId64 " ms", cfg->ssid, (esp_timer_get_time() - t0) / 1000);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */

esp_err_t nid_wifi_start(const nid_wifi_config_t *cfg)
{
    if (cfg == NULL || cfg->ssid == NULL || cfg->password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t ssid_len = strlen(cfg->ssid);
    const size_t pass_len = strlen(cfg->password);
    const bool pass_ok = (pass_len >= 8 && pass_len <= 63) || (cfg->mode == NID_WIFI_MODE_STA && pass_len == 0);
    if (ssid_len == 0 || ssid_len > 32 || !pass_ok ||
        (cfg->mode == NID_WIFI_MODE_AP && (cfg->channel < 1 || cfg->channel > 13 || cfg->max_clients < 1 ||
                                           cfg->max_clients > 10))) {
        char txt[NID_WIFI_ERROR_LEN];
        snprintf(txt, sizeof(txt), "réglages invalides (SSID %u car., mot de passe %u car.)", (unsigned)ssid_len,
                 (unsigned)pass_len);
        portENTER_CRITICAL(&s_mux);
        strlcpy(s_st.error, txt, sizeof(s_st.error));
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGE(TAG, "Wi-Fi : %s", txt);
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t t0 = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    s_st.state = NID_WIFI_STATE_STARTING;
    s_st.mode = cfg->mode;
    strlcpy(s_st.ssid, cfg->ssid, sizeof(s_st.ssid));
    s_st.ip[0] = '\0';
    s_st.clients = 0;
    s_st.channel = (cfg->mode == NID_WIFI_MODE_AP) ? cfg->channel : 0;
    s_st.sta_disconnects = 0;
    s_st.sta_last_reason = 0;
    s_st.error[0] = '\0';
    portEXIT_CRITICAL(&s_mux);

    esp_err_t err;
    if (!s_w.hosted_init) {
        if (s_w.ev != NULL || s_w.coproc_on) {
            nid_wifi_stop();    /* reste d'un démarrage en échec */
            set_state(NID_WIFI_STATE_STARTING);
        }
        atomic_store(&s_link_lost, false);
        err = transport_start(t0);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        if (atomic_load(&s_link_lost)) {
            set_error("liaison avec le coprocesseur perdue : arrêter puis redémarrer le Wi-Fi", ESP_ERR_INVALID_STATE);
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGI(TAG, "changement de mode : %s", cfg->mode == NID_WIFI_MODE_AP ? "point d'accès" : "station");
        wifi_mode_stop(true);
    }
    return wifi_mode_start(cfg, t0);
}

esp_err_t nid_wifi_query_cp_version(char *out, size_t len, uint32_t *major)
{
    esp_hosted_coprocessor_fwver_t ver = { 0 };
    if (major != NULL) {
        *major = 0;
    }
    if (!s_w.hosted_init || atomic_load(&s_link_lost) || esp_hosted_get_coprocessor_fwversion(&ver) != 0) {
        strlcpy(out, "inconnue", len);
        ESP_LOGW(TAG, "version du firmware du coprocesseur inconnue");
        return ESP_FAIL;
    }
    snprintf(out, len, "%" PRIu32 ".%" PRIu32 ".%" PRIu32, (uint32_t)ver.major1, (uint32_t)ver.minor1,
             (uint32_t)ver.patch1);
    if (major != NULL) {
        *major = (uint32_t)ver.major1;
    }
    ESP_LOGI(TAG, "firmware ESP-Hosted du coprocesseur : %s", out);
    return ESP_OK;
}

uint32_t nid_wifi_host_version(char *out, size_t len)
{
    snprintf(out, len, "%d.%d.%d", ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1);
    return (uint32_t)ESP_HOSTED_VERSION_MAJOR_1;
}

esp_err_t nid_wifi_sta_rssi(int *rssi)
{
    *rssi = 0;
    nid_wifi_status_t st;
    nid_wifi_get_status(&st);
    if (!s_w.wifi_started || st.mode != NID_WIFI_MODE_STA || st.state != NID_WIFI_STATE_UP ||
        atomic_load(&s_link_lost)) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_wifi_sta_get_rssi(rssi);
}

void nid_wifi_stop(void)
{
    wifi_mode_stop(false);
    if (s_w.wifi_init) {
        /* Libère aussi les canaux de données d'ESP-Hosted, même sans réponse du C6. */
        esp_err_t err = esp_wifi_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_deinit : %s", esp_err_to_name(err));
        }
        s_w.wifi_init = false;
    }
    if (s_w.netif) {
        esp_netif_destroy_default_wifi(s_w.netif);
        s_w.netif = NULL;
    }
    if (s_w.hosted_init) {
        /* Arrête le transport : tâches, slot SDMMC 1 (le slot 0 de la carte SD
         * n'est pas touché). */
        int rc = esp_hosted_deinit();
        if (rc != 0) {
            ESP_LOGW(TAG, "arrêt du transport ESP-Hosted : %d", rc);
        }
        s_w.hosted_init = false;
    }
    if (s_w.coproc_on) {
        nid_board_coproc_enable(false);
        s_w.coproc_on = false;
        ESP_LOGI(TAG, "coprocesseur ESP32-C6 remis en reset");
    }
    unregister_handlers();
    if (s_w.ev) {
        vEventGroupDelete(s_w.ev);
        s_w.ev = NULL;
    }
    portENTER_CRITICAL(&s_mux);
    s_st.clients = 0;
    s_st.ip[0] = '\0';
    if (s_st.state != NID_WIFI_STATE_FAILED) {
        s_st.state = NID_WIFI_STATE_OFF;
    }
    portEXIT_CRITICAL(&s_mux);
}
