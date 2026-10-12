/*
 * nid_install — mode installation : Wi-Fi local, aperçu et réglages sur smartphone
 *
 * FS1.2 : orienter et cadrer → aperçu vidéo en direct sur smartphone via un
 *         point d'accès Wi-Fi local temporaire (coprocesseur ESP32-C6).
 * FS1.3 (F0) : mettre en service et confirmer le fonctionnement avant de
 *         quitter le nid → bouton « Démarrer la surveillance » (page web) ou
 *         appui sur le bouton du boîtier ; voyant allumé seulement pendant
 *         l'installation.
 * FS1.4 : paramétrer sans reprogrammer → formulaire des réglages (écrit
 *         /sdcard/config.ini) ; mise à l'heure depuis le téléphone.
 * FC1.6 : intervention courte → fin automatique après duree_max_min.
 *
 * Accès : se connecter au Wi-Fi « NID-<id_systeme> » puis ouvrir
 * http://192.168.4.1 dans le navigateur.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "nid_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bouton du boîtier pendant le mode installation :
 *   appui court (< 3 s)  → « Démarrer la surveillance »
 *   appui long  (≥ 3 s)  → « Pause maintenance » (changement de carte ou de batterie)
 */
typedef enum {
    NID_INSTALL_EXIT_START = 0,   /*!< l'opérateur a lancé la surveillance (page web ou appui court) */
    NID_INSTALL_EXIT_TIMEOUT,     /*!< durée maximale atteinte : la surveillance démarre d'office */
    NID_INSTALL_EXIT_NO_WIFI,     /*!< Wi-Fi indisponible : surveillance démarrée sur appui court ou délai */
    NID_INSTALL_EXIT_MAINTENANCE, /*!< pause maintenance demandée (page web ou appui long) : carte SD
                                       démontée, plus aucun enregistrement jusqu'au prochain appui bouton */
} nid_install_result_t;

typedef struct {
    nid_config_t *cfg;            /*!< [in/out] réglages courants, modifiés depuis la page web */
    const char   *config_path;    /*!< fichier où sauvegarder les réglages (/sdcard/config.ini) */
    const char   *fw_version;     /*!< version du firmware affichée */
    uint32_t      timeout_s;      /*!< fin automatique du mode installation */
    bool          config_changed; /*!< [out] réglages modifiés et sauvegardés */
    bool          time_changed;   /*!< [out] heure réglée depuis le téléphone */
} nid_install_ctx_t;

/**
 * Lance le mode installation (bloquant) : sort le C6 du reset, démarre le
 * point d'accès et le serveur web, allume le voyant. À la sortie : serveur et
 * Wi-Fi arrêtés, C6 remis en reset, voyant éteint.
 *
 * Prérequis : SD montée, nid_capture_init() appelé (sinon pas d'aperçu).
 */
nid_install_result_t nid_install_run(nid_install_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
