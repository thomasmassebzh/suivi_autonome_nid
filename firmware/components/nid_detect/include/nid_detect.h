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

/**
 * Configure les entrées : PIR avec tirage interne vers son niveau de repos
 * (tirage bas si la sortie est active haute : un PIR débranché ne flotte pas)
 * et bouton vers la masse avec tirage haut. Rend les broches au GPIO numérique
 * après une veille profonde (elles étaient routées vers les E/S basse
 * consommation pour le réveil EXT1). Idempotent. À appeler avant les autres
 * fonctions du module (et avant nid_power_get_boot_info()).
 */
esp_err_t nid_detect_init(void);

/** Niveau logique actif du PIR (true = détection en cours). */
bool nid_detect_pir_active(void);

/** true si le bouton est appuyé (niveau bas). */
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
 *
 * Échantillonnage toutes les 5 ms par vTaskDelay (jamais d'attente active) :
 * prévu pour tourner dans une tâche de priorité supérieure, en parallèle du
 * démarrage de la caméra. Nourrit le chien de garde si la tâche y est abonnée.
 */
bool nid_detect_pir_confirm(uint32_t min_active_ms, uint32_t timeout_ms);

/**
 * Attend que la sortie PIR retombe au repos, de façon stable pendant 50 ms (à
 * faire avant la veille profonde, sinon réveil immédiat). Nourrit le chien de
 * garde si la tâche appelante y est abonnée.
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
 *
 * PIR : front vers le niveau actif, publié directement par l'interruption.
 * Bouton : front descendant, confirmé 50 ms plus tard (bouton toujours appuyé)
 * par une minuterie esp_timer : un seul événement par appui, aucun au
 * relâchement. Utilise le service d'interruptions GPIO partagé (installé s'il
 * ne l'est pas déjà, jamais désinstallé) : compatible avec les autres
 * utilisateurs (ESP-Hosted...). Un nouvel appel remplace la file.
 */
esp_err_t nid_detect_enable_events(QueueHandle_t queue);

/** Désactive les interruptions installées par nid_detect_enable_events() (le service GPIO reste installé). */
void nid_detect_disable_events(void);

#ifdef __cplusplus
}
#endif
