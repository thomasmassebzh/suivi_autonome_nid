/*
 * nid_ir — éclairage infrarouge 940 nm, filtre IR-cut, décision jour/nuit
 *
 * FP1.2 (F0) : images exploitables 24 h/24, y compris en obscurité totale.
 * FC1.1 (F0) : aucune lumière visible (λ < 750 nm) → LED 940 nm uniquement,
 *   allumées seulement pendant une prise de vue de nuit, éteintes sinon.
 * FC1.2 (F0) : aucun bruit audible → le filtre IR-cut n'est commandé que
 *   lorsque l'état jour/nuit change réellement (sa position est mémorisée en
 *   mémoire RTC, y compris pendant la veille profonde).
 *
 * Décision jour/nuit (mode « auto ») : lever et coucher du soleil calculés
 * (algorithme NOAA) pour la latitude/longitude de config.ini, avec avance au
 * crépuscule et retard à l'aube pour tenir compte du couvert forestier.
 * Aucun capteur supplémentaire n'est nécessaire et la décision est immédiate
 * au réveil (pas de retard sur le début d'enregistrement).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "nid_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise la PWM des LED IR (éteintes, sortie à l'état bas) et la commande
 * du filtre IR-cut selon le menuconfig. Sans danger si les broches ne sont pas
 * câblées (-1). À appeler une fois au démarrage, après nid_board_init_early().
 */
esp_err_t nid_ir_init(void);

/** Puissance des LED IR en % (0 = éteintes, 100 = pleine puissance ; PWM linéaire). Sans effet sans broche LED. */
void nid_ir_set_led_power(uint8_t percent);

/**
 * Filtre IR-cut : day = true → filtre en place (couleurs justes de jour),
 * false → filtre retiré (sensibilité à l'infrarouge la nuit).
 *
 * Ne fait rien si le filtre est déjà dans l'état demandé (position mémorisée en
 * mémoire RTC ; inconnue après une mise sous tension : une commande est alors
 * envoyée). Ne bloque pas : avec un filtre à impulsion, l'impulsion est terminée
 * par une minuterie.
 *
 * @return ESP_OK ; ESP_ERR_NOT_SUPPORTED si aucune commande n'est configurée
 *         (menuconfig « aucune » ou broche non câblée) ; ESP_ERR_INVALID_STATE
 *         si nid_ir_init() n'a pas été appelée.
 */
esp_err_t nid_ir_set_day_filter(bool day);

/**
 * Heures de lever et coucher du soleil (UTC) du jour solaire local contenant
 * l'instant day_utc, pour un zénith de 90,833° (algorithme NOAA, précision de
 * l'ordre de la minute entre 1900 et 2100).
 *
 * Le jour solaire local commence à minuit en temps solaire moyen du lieu
 * (UTC + longitude/15 h) : il est choisi d'après la longitude et non d'après la
 * date UTC. En Martinique (UTC-4), la soirée locale appartient déjà au jour UTC
 * suivant ; ici elle reste dans le même jour que l'après-midi qui précède.
 * Ne dépend ni du fuseau horaire (TZ) ni de l'heure d'été.
 *
 * @param latitude  degrés, nord positif (−90..90)
 * @param longitude degrés, est positif (−180..180)
 * @return ESP_OK ; ESP_ERR_INVALID_STATE pendant le jour polaire / la nuit
 *         polaire (le soleil ne se couche pas / ne se lève pas : sorties = day_utc) ;
 *         ESP_ERR_INVALID_ARG si un pointeur est NULL ou une coordonnée invalide.
 */
esp_err_t nid_ir_sun_times(double latitude, double longitude, time_t day_utc,
                           time_t *sunrise_utc, time_t *sunset_utc);

/**
 * true s'il fait nuit à l'instant now_utc selon config.
 *  - mode « jour » : toujours false ; mode « nuit » : toujours true ;
 *  - mode « auto » : true sauf si now_utc ∈ [lever + retard_aube, coucher − avance_crépuscule]
 *    du jour solaire local contenant now_utc (voir nid_ir_sun_times()). Jour
 *    polaire → false, nuit polaire → true. Coordonnées inutilisables → true
 *    (mieux vaut éclairer que filmer dans le noir).
 * Calcul en quelques millisecondes, sans accès au matériel.
 */
bool nid_ir_is_night(const nid_config_t *cfg, time_t now_utc);

/**
 * Applique le mode : nuit → filtre retiré + LED à cfg->ir_power_pct ;
 * jour → filtre en place + LED éteintes. Ne bloque pas (chemin critique avant
 * le démarrage de la caméra) ; le filtre n'est commandé que s'il change d'état.
 */
void nid_ir_apply(bool night, const nid_config_t *cfg);

/**
 * Éteint les LED IR. Ne touche pas au filtre IR-cut, qui reste dans sa position
 * (aucun bruit entre deux prises de vue ; il sera commandé seulement si
 * l'état jour/nuit change). Si une impulsion IR-cut est en cours, attend sa fin
 * (au plus CONFIG_NID_IRCUT_PULSE_MS) pour ne pas interrompre le mouvement.
 */
void nid_ir_off(void);

#ifdef __cplusplus
}
#endif
