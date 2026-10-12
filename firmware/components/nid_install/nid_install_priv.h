/*
 * nid_install — définitions internes au composant
 *
 * Trois contextes d'exécution partagent l'état ci-dessous :
 *   - la tâche principale (nid_install_run : bouton, voyant, délai, mesures) ;
 *   - la tâche Wi-Fi (démarrage/arrêt du coprocesseur, du point d'accès et du
 *     serveur web) et la boucle d'événements par défaut ;
 *   - la tâche du serveur web et la tâche d'aperçu MJPEG.
 * Toute lecture/écriture de inst_shared_t (y compris *cfg) se fait sous
 * inst_lock(). Ne jamais appeler un autre module en tenant ce verrou.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "nid_config.h"
#include "nid_install.h"
#include "nid_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INST_FW_LEN  32
#define INST_CP_LEN  32

/* Fin du mode installation, telle qu'affichée par la page. */
typedef enum {
    INST_END_NONE = 0,
    INST_END_START,        /*!< démarrage de la surveillance (page ou appui court) */
    INST_END_MAINTENANCE,  /*!< pause maintenance (page ou appui long) */
    INST_END_TIMEOUT,      /*!< durée maximale atteinte */
} inst_end_t;

typedef struct {
    /* Cycle de vie */
    bool          active;          /*!< false dès que nid_install_run() a rendu la main */
    bool          accept_actions;  /*!< false dès que la fin est décidée */
    inst_end_t    web_request;     /*!< fin demandée depuis la page */
    inst_end_t    ending;          /*!< fin décidée (affichée par la page) */
    /* Contexte */
    nid_config_t *cfg;             /*!< réglages de l'application (modifiés par la page) */
    const char   *config_path;
    char          fw_version[INST_FW_LEN];
    int64_t       start_us;        /*!< esp_timer_get_time() à l'entrée */
    uint32_t      timeout_s;
    /* Sorties */
    bool          config_changed;
    bool          time_changed;
    /* Mesures de la tâche principale */
    uint32_t      batt_mv;         /*!< 0 = inconnue */
    int           batt_level;      /*!< nid_batt_level_t */
    bool          temp_ok;
    float         temp_c;
    bool          sd_mounted;
    uint64_t      sd_total;
    uint64_t      sd_free;
    bool          sd_refresh;      /*!< demande de nouvelle mesure de l'espace libre */
    bool          pir;
    bool          night;
    /* Page */
    bool          ir_on;
    char          last_photo[NID_PATH_MAX];
    /* Wi-Fi */
    int           clients;
    char          ssid[NID_CONFIG_SSID_LEN];
    char          cp_version[INST_CP_LEN];
} inst_shared_t;

inst_shared_t *inst_shared(void);
void inst_lock(void);
void inst_unlock(void);

/* ---- nid_install_wifi.c : coprocesseur ESP32-C6 et point d'accès ---- */

/**
 * Démarre le coprocesseur, le transport ESP-Hosted et le point d'accès
 * (bloquant, délais bornés). En cas d'échec, l'état partiel reste à arrêter
 * par inst_wifi_stop(). @return ESP_OK si le point d'accès est actif.
 */
esp_err_t inst_wifi_start(const char *ssid, const char *password);

/** Interroge la version du firmware du coprocesseur (RPC, après le démarrage). */
void inst_wifi_query_cp_version(void);

/** Arrête le point d'accès et le transport, remet le C6 en reset (idempotent). */
void inst_wifi_stop(void);

/** true si la liaison avec le coprocesseur a été perdue en cours de route. */
bool inst_wifi_link_lost(void);

/** Cause du dernier échec de inst_wifi_start() (texte français). */
const char *inst_wifi_error(void);

/* ---- nid_install_web.c : serveur web, page, aperçu MJPEG ---- */

esp_err_t inst_web_start(void);

/** Arrête l'aperçu puis le serveur web (idempotent). */
void inst_web_stop(void);

/** Arrête l'aperçu et éteint l'éclairage IR si la page l'avait allumé. */
void inst_web_camera_idle(void);

#ifdef __cplusplus
}
#endif
