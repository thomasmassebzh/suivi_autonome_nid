/*
 * nid_wifi — Wi-Fi de l'ESP32-P4 par le coprocesseur ESP32-C6 (ESP-Hosted)
 *
 * Composant commun au firmware de terrain (mode installation, nid_install) et
 * au banc d'essai : démarrage du C6 et du transport ESP-Hosted (SDIO), point
 * d'accès WPA2 (192.168.4.1, serveur DHCP) ou station (rejoindre un réseau
 * existant, adresse obtenue par DHCP), arrêt propre (C6 remis en reset).
 *
 * Rien n'est démarré au boot : le C6 reste en reset jusqu'à nid_wifi_start()
 * (réveil de veille et délai détection → première image non affectés).
 * Toutes les attentes sont bornées ; un C6 muet ou une liaison perdue ne
 * redémarre jamais le système.
 *
 * Appels non réentrants : nid_wifi_start() et nid_wifi_stop() sont appelés par
 * une seule tâche à la fois (tâche Wi-Fi de l'application). nid_wifi_get_status(),
 * nid_wifi_link_lost() et nid_wifi_host_version() peuvent être appelés de
 * n'importe quelle tâche.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NID_WIFI_SSID_LEN     33    /*!< SSID : 1 à 32 caractères + '\0' */
#define NID_WIFI_PASS_LEN     65    /*!< mot de passe WPA2 : 8 à 63 caractères + '\0' */
#define NID_WIFI_IP_LEN       16
#define NID_WIFI_VERSION_LEN  24
#define NID_WIFI_ERROR_LEN    96

typedef enum {
    NID_WIFI_MODE_AP = 0,           /*!< point d'accès (192.168.4.1, DHCP) */
    NID_WIFI_MODE_STA,              /*!< station : rejoint un réseau existant */
} nid_wifi_mode_t;

typedef struct {
    nid_wifi_mode_t mode;
    const char     *ssid;           /*!< AP : nom diffusé ; STA : réseau à rejoindre (1 à 32 caractères) */
    const char     *password;       /*!< AP : 8 à 63 caractères (WPA2) ; STA : "" (réseau ouvert) ou 8 à 63 */
    uint8_t         channel;        /*!< AP : canal 1 à 13 */
    uint8_t         max_clients;    /*!< AP : 1 à 10 */
    uint32_t        sta_timeout_ms; /*!< STA : attente de l'adresse IP (0 : 15 s) */
} nid_wifi_config_t;

typedef enum {
    NID_WIFI_STATE_OFF = 0,         /*!< arrêté, C6 en reset */
    NID_WIFI_STATE_STARTING,        /*!< démarrage du C6, du transport ou du Wi-Fi */
    NID_WIFI_STATE_UP,              /*!< AP démarré, ou station connectée avec une adresse IP */
    NID_WIFI_STATE_CONNECTING,      /*!< station : réseau absent ou adresse en attente (nouvel essai automatique) */
    NID_WIFI_STATE_FAILED,          /*!< démarrage en échec (voir error) */
    NID_WIFI_STATE_LINK_LOST,       /*!< liaison SDIO avec le C6 perdue */
} nid_wifi_state_t;

/** État courant (copie). */
typedef struct {
    nid_wifi_state_t state;
    nid_wifi_mode_t  mode;
    char             ssid[NID_WIFI_SSID_LEN];
    char             ip[NID_WIFI_IP_LEN];       /*!< "" tant qu'aucune adresse */
    int              clients;                   /*!< AP : appareils associés */
    uint8_t          channel;                   /*!< AP : canal ; STA : 0 */
    uint32_t         sta_disconnects;           /*!< STA : déconnexions depuis le démarrage */
    uint8_t          sta_last_reason;           /*!< STA : dernier motif de déconnexion (wifi_err_reason_t) */
    char             error[NID_WIFI_ERROR_LEN]; /*!< cause du dernier échec, "" sinon */
} nid_wifi_status_t;

/**
 * Démarre le Wi-Fi (bloquant, délais bornés) :
 *  1. NVS, pile réseau, boucle d'événements par défaut (créées si absentes) ;
 *  2. C6 sorti du reset, transport ESP-Hosted (énumération SDIO) ;
 *  3. interface réseau et Wi-Fi du C6 dans le mode demandé.
 * Si le Wi-Fi est déjà démarré, seul le mode est changé (le C6 et le
 * transport restent en service).
 *
 * @return ESP_OK : AP actif, ou station connectée avec une adresse ;
 *         ESP_ERR_INVALID_ARG : SSID ou mot de passe invalide (rien n'est démarré) ;
 *         ESP_ERR_TIMEOUT en station : réseau introuvable ou pas d'adresse dans
 *         le délai, le Wi-Fi reste démarré et réessaie (état CONNECTING) ;
 *         autre : échec, l'état partiel est à arrêter par nid_wifi_stop().
 */
esp_err_t nid_wifi_start(const nid_wifi_config_t *cfg);

/** Arrête le Wi-Fi et le transport, remet le C6 en reset (idempotent). */
void nid_wifi_stop(void);

/** Copie de l'état courant. */
void nid_wifi_get_status(nid_wifi_status_t *out);

/** true si la liaison avec le coprocesseur a été perdue en cours de route. */
bool nid_wifi_link_lost(void);

/** Cause du dernier échec de nid_wifi_start() (texte français, « cause inconnue » sinon). */
const char *nid_wifi_error(void);

/**
 * Version du firmware ESP-Hosted du C6 (RPC, après un démarrage réussi), au
 * format « x.y.z ». ESP_FAIL si le C6 ne la donne pas (out = « inconnue »).
 */
esp_err_t nid_wifi_query_cp_version(char *out, size_t len, uint32_t *major);

/** Version de l'hôte ESP-Hosted compilée dans le P4 (« 3.0.9 ») ; renvoie la version majeure. */
uint32_t nid_wifi_host_version(char *out, size_t len);

/** Puissance reçue du point d'accès en station (RPC, dBm). */
esp_err_t nid_wifi_sta_rssi(int *rssi);

/** Libellé court d'un état (« arrêté », « prêt »...). */
const char *nid_wifi_state_str(nid_wifi_state_t state);

#ifdef __cplusplus
}
#endif
