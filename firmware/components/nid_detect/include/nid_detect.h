/*
 * nid_detect — détecteur PIR et bouton de mise en service
 *
 * FP1.1 : détecter l'arrivée d'un animal (gabarit ≥ rat noir, ≥ 90 % des
 * passages, ≤ 20 % de déclenchements intempestifs). La sensibilité matérielle
 * se règle sur le potentiomètre du HC-SR501 ; la « sensibilite » (1..5) de
 * config.ini fixe en plus une durée minimale de sortie haute qui rejette les
 * parasites électriques (appels de courant de la radio, de l'éclairage IR).
 *
 * La validation se fait EN PARALLÈLE du démarrage de la caméra : elle ne
 * retarde pas l'enregistrement (FP1.4 : ≤ 1 s entre détection et début).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configure les entrées PIR (sans tirage, la sortie du module est active) et bouton (pull-up). */
esp_err_t nid_detect_init(void);

/** Niveau logique actif du PIR (true = détection en cours). */
bool nid_detect_pir_active(void);

/** true si le bouton est appuyé. */
bool nid_detect_button_pressed(void);

/**
 * Durée minimale d'activité du PIR pour valider un déclenchement selon la
 * sensibilité : 5 → 0 ms, 4 → 50 ms, 3 → 150 ms, 2 → 300 ms, 1 → 500 ms.
 */
uint32_t nid_detect_min_active_ms(uint8_t sensitivity);

/**
 * Valide un déclenchement : true dès que la sortie PIR est restée active
 * min_active_ms (cumul depuis l'appel), false si timeout_ms est atteint avant.
 * min_active_ms = 0 → retourne immédiatement true.
 */
bool nid_detect_pir_confirm(uint32_t min_active_ms, uint32_t timeout_ms);

/**
 * Attend que la sortie PIR retombe au repos (à faire avant la veille profonde,
 * sinon réveil immédiat).
 * @return ESP_OK, ou ESP_ERR_TIMEOUT si toujours active après timeout_ms.
 */
esp_err_t nid_detect_wait_pir_idle(uint32_t timeout_ms);

/** Événements du mode banc d'essai (sans veille profonde). */
typedef enum {
    NID_DETECT_EVT_PIR = 1,     /*!< front actif du PIR */
    NID_DETECT_EVT_BUTTON = 2,  /*!< appui sur le bouton (anti-rebond 50 ms) */
} nid_detect_evt_t;

/**
 * Mode banc : publie les fronts du PIR et du bouton dans une file
 * (éléments de type nid_detect_evt_t) depuis une interruption.
 */
esp_err_t nid_detect_enable_events(QueueHandle_t queue);

/** Désactive les interruptions installées par nid_detect_enable_events(). */
void nid_detect_disable_events(void);

#ifdef __cplusplus
}
#endif
