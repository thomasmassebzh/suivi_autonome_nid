/*
 * nid_install — Wi-Fi d'installation : point d'accès par le coprocesseur ESP32-C6
 *
 * Le démarrage du C6, le transport ESP-Hosted, le point d'accès et l'arrêt
 * propre sont dans le composant commun nid_wifi (partagé avec le banc
 * d'essai) ; ce fichier n'ajoute que les réglages du mode installation
 * (canal, nombre de téléphones, menu « Suivi nid — Mode installation ») et
 * l'état affiché par la page.
 */
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"

#include "nid_install_priv.h"
#include "nid_wifi.h"

static const char *TAG = "nid_install";

#define DEFAULT_PASSWORD  "moqueur-nid"

esp_err_t inst_wifi_start(const char *ssid, const char *password)
{
    size_t pass_len = strlen(password);
    if (pass_len < 8 || pass_len > 63) {
        ESP_LOGW(TAG, "mot de passe Wi-Fi invalide (%u caractères) : mot de passe par défaut", (unsigned)pass_len);
        password = DEFAULT_PASSWORD;
    }
    char name[NID_WIFI_SSID_LEN];
    strlcpy(name, ssid, sizeof(name));     /* SSID tronqué à 32 caractères, comme avant */
    const nid_wifi_config_t cfg = {
        .mode = NID_WIFI_MODE_AP,
        .ssid = name,
        .password = password,
        .channel = CONFIG_NID_INSTALL_WIFI_CHANNEL,
        .max_clients = CONFIG_NID_INSTALL_WIFI_MAX_CLIENTS,
    };
    esp_err_t err = nid_wifi_start(&cfg);
    if (err == ESP_OK) {
        inst_lock();
        strlcpy(inst_shared()->ssid, ssid, sizeof(inst_shared()->ssid));
        inst_unlock();
    }
    return err;
}

void inst_wifi_query_cp_version(void)
{
    char txt[INST_CP_LEN];
    nid_wifi_query_cp_version(txt, sizeof(txt), NULL);
    inst_lock();
    strlcpy(inst_shared()->cp_version, txt, sizeof(inst_shared()->cp_version));
    inst_unlock();
}

void inst_wifi_stop(void)
{
    nid_wifi_stop();
}

bool inst_wifi_link_lost(void)
{
    return nid_wifi_link_lost();
}

const char *inst_wifi_error(void)
{
    return nid_wifi_error();
}

int inst_wifi_clients(void)
{
    nid_wifi_status_t st;
    nid_wifi_get_status(&st);
    return st.clients;
}
