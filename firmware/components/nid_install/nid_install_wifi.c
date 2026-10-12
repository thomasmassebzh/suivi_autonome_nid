/*
 * nid_install — Wi-Fi d'installation : coprocesseur ESP32-C6 et point d'accès
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
 *   3. interface réseau AP (192.168.4.1, serveur DHCP), esp_wifi_init /
 *      set_mode / set_config / start (RPC vers le C6), attente de AP_START.
 * L'arrêt suit l'ordre inverse et remet toujours le C6 en reset.
 *
 * Compatibilité : hôte ESP-Hosted 3.x avec le firmware 2.x livré sur le C6
 * (rétrocompatibilité documentée par Espressif ; en-tête de trame V1, RPC sans
 * négociation de version, réception SDIO en mode « streaming »).
 */
#include <inttypes.h>
#include <stdatomic.h>
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
#include "nid_install_priv.h"

/* --------------------------------------------------------------------------
 * Garde-fous de configuration (sdkconfig.defaults, section « Wi-Fi
 * d'installation ») : une configuration régénérée sans ces réglages ne doit
 * pas compiler.
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

static const char *TAG = "nid_install";

#define DEFAULT_PASSWORD      "moqueur-nid"
#define LINK_SETTLE_MS        200    /* enregistrement des événements Wi-Fi d'ESP-Hosted */
#define AP_START_TIMEOUT_MS   5000
#define AP_STOP_TIMEOUT_MS    1500
#define NETIF_UP_TIMEOUT_MS   2000
#define POLL_MS               50

#define EV_AP_START  BIT0
#define EV_AP_STOP   BIT1

static struct {
    EventGroupHandle_t           ev;
    esp_event_handler_instance_t h_wifi;
    esp_event_handler_instance_t h_ip;
    esp_event_handler_instance_t h_hosted;
    esp_netif_t                 *netif;
    bool                         coproc_on;
    bool                         hosted_init;
    bool                         wifi_init;
    bool                         wifi_started;
    char                         error[96];
} s_w;

static atomic_bool s_link_lost;

/* -------------------------------------------------------------------------- */

static void set_error(const char *what, esp_err_t err)
{
    snprintf(s_w.error, sizeof(s_w.error), "%s (%s)", what, esp_err_to_name(err));
    ESP_LOGE(TAG, "Wi-Fi d'installation : %s", s_w.error);
}

const char *inst_wifi_error(void)
{
    return s_w.error[0] ? s_w.error : "cause inconnue";
}

bool inst_wifi_link_lost(void)
{
    return atomic_load(&s_link_lost);
}

static void clients_add(int delta)
{
    inst_lock();
    inst_shared_t *sh = inst_shared();
    sh->clients += delta;
    if (sh->clients < 0) {
        sh->clients = 0;
    }
    inst_unlock();
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
            ESP_LOGI(TAG, "téléphone associé " MACSTR " (AID %u)", MAC2STR(e->mac), (unsigned)e->aid);
            clients_add(1);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            const wifi_event_ap_stadisconnected_t *e = data;
            ESP_LOGI(TAG, "téléphone déconnecté " MACSTR " (raison %u)", MAC2STR(e->mac), (unsigned)e->reason);
            clients_add(-1);
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        const ip_event_assigned_ip_to_client_t *e = data;
        ESP_LOGI(TAG, "adresse " IPSTR " attribuée à " MACSTR, IP2STR(&e->ip), MAC2STR(e->mac));
    } else if (base == ESP_HOSTED_EVENT) {
        if (id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE) {
            ESP_LOGE(TAG, "liaison SDIO avec le coprocesseur en panne");
            atomic_store(&s_link_lost, true);
        } else if (id == ESP_HOSTED_EVENT_CP_INIT) {
            ESP_LOGI(TAG, "coprocesseur initialisé");
        }
    }
}

static esp_err_t register_handlers(void)
{
    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, &s_w.h_wifi);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, on_event, NULL, &s_w.h_ip);
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
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, s_w.h_ip);
        s_w.h_ip = NULL;
    }
    if (s_w.h_hosted) {
        esp_event_handler_instance_unregister(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, s_w.h_hosted);
        s_w.h_hosted = NULL;
    }
}

/* -------------------------------------------------------------------------- */

esp_err_t inst_wifi_start(const char *ssid, const char *password)
{
    s_w.error[0] = '\0';
    atomic_store(&s_link_lost, false);
    int64_t t0 = esp_timer_get_time();

    s_w.ev = xEventGroupCreate();
    if (s_w.ev == NULL) {
        set_error("mémoire insuffisante", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    /* 1. NVS (le Wi-Fi distant n'en dépend pas : jamais effacée ici, la NVS
     *    peut contenir l'état sauvegardé de l'application). */
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS indisponible (%s) : sans effet sur le Wi-Fi du coprocesseur", esp_err_to_name(err));
    }

    /* 2. Pile réseau et boucle d'événements (créée seulement si absente). */
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

    /* 3. Coprocesseur : sortie du reset, puis transport ESP-Hosted (qui
     *    réinitialise lui-même le C6 par la même broche avant l'énumération). */
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

    /* 4. Interface réseau du point d'accès (clé « WIFI_AP_DEF » attendue par
     *    ESP-Hosted, 192.168.4.1/24, serveur DHCP). */
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_WIFI_AP();
    s_w.netif = esp_netif_new(&ncfg);
    if (s_w.netif == NULL) {
        set_error("création de l'interface réseau", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    err = esp_netif_attach_wifi_ap(s_w.netif);
    if (err == ESP_OK) {
        err = esp_wifi_set_default_wifi_ap_handlers();
    }
    if (err != ESP_OK) {
        set_error("liaison de l'interface réseau", err);
        return err;
    }

    /* 5. Wi-Fi du C6 en point d'accès WPA2. */
    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&icfg);
    if (err != ESP_OK) {
        set_error("esp_wifi_init", err);
        return err;
    }
    s_w.wifi_init = true;

    wifi_config_t ap = { 0 };
    size_t ssid_len = strnlen(ssid, sizeof(ap.ap.ssid));
    memcpy(ap.ap.ssid, ssid, ssid_len);
    ap.ap.ssid_len = (uint8_t)ssid_len;
    size_t pass_len = strlen(password);
    if (pass_len < 8 || pass_len > 63) {
        ESP_LOGW(TAG, "mot de passe Wi-Fi invalide (%u caractères) : mot de passe par défaut", (unsigned)pass_len);
        password = DEFAULT_PASSWORD;
        pass_len = strlen(password);
    }
    memcpy(ap.ap.password, password, pass_len);
    ap.ap.channel = CONFIG_NID_INSTALL_WIFI_CHANNEL;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = CONFIG_NID_INSTALL_WIFI_MAX_CLIENTS;
    ap.ap.pmf_cfg.required = false;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
    if (err != ESP_OK) {
        set_error("configuration du point d'accès", err);
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        set_error("esp_wifi_start", err);
        return err;
    }
    s_w.wifi_started = true;

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

    inst_lock();
    strlcpy(inst_shared()->ssid, ssid, sizeof(inst_shared()->ssid));
    inst_unlock();
    ESP_LOGI(TAG, "Wi-Fi « %s » prêt (canal %d, WPA2) en %" PRId64 " ms : http://192.168.4.1",
             ssid, CONFIG_NID_INSTALL_WIFI_CHANNEL, (esp_timer_get_time() - t0) / 1000);
    return ESP_OK;
}

void inst_wifi_query_cp_version(void)
{
    esp_hosted_coprocessor_fwver_t ver = { 0 };
    char txt[INST_CP_LEN];
    if (esp_hosted_get_coprocessor_fwversion(&ver) == 0) {
        snprintf(txt, sizeof(txt), "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 (uint32_t)ver.major1, (uint32_t)ver.minor1, (uint32_t)ver.patch1);
        ESP_LOGI(TAG, "firmware ESP-Hosted du coprocesseur : %s", txt);
    } else {
        strlcpy(txt, "inconnue", sizeof(txt));
        ESP_LOGW(TAG, "version du firmware du coprocesseur inconnue");
    }
    inst_lock();
    strlcpy(inst_shared()->cp_version, txt, sizeof(inst_shared()->cp_version));
    inst_unlock();
}

void inst_wifi_stop(void)
{
    const bool lost = atomic_load(&s_link_lost);

    if (s_w.wifi_started) {
        /* Liaison perdue : inutile d'attendre la réponse du C6. */
        if (!lost && esp_wifi_stop() == ESP_OK && s_w.ev) {
            xEventGroupWaitBits(s_w.ev, EV_AP_STOP, pdFALSE, pdTRUE, pdMS_TO_TICKS(AP_STOP_TIMEOUT_MS));
        }
        s_w.wifi_started = false;
    }
    if (s_w.netif && esp_netif_is_netif_up(s_w.netif)) {
        /* AP_STOP non reçu : arrêt du serveur DHCP et de l'interface. */
        esp_netif_action_stop(s_w.netif, WIFI_EVENT, WIFI_EVENT_AP_STOP, NULL);
    }
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
    inst_lock();
    inst_shared()->clients = 0;
    inst_unlock();
}
