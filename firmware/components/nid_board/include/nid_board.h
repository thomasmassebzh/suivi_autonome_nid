/*
 * nid_board — brochage et ressources partagées de la carte
 *
 * Centralise le brochage (menuconfig « Suivi nid — Carte et brochage ») et
 * les ressources communes : bus I2C partagé (caméra, codec ES8311, DS3231),
 * coprocesseur Wi-Fi ESP32-C6, voyant d'état, mise en sécurité des broches.
 *
 * Exigences couvertes : FC1.1 (aucune lumière visible : rétroéclairage LCD et
 * voyant éteints hors installation), FC1.2 (ampli audio coupé), FC3.2 (C6 en
 * reset, broches au repos avant la veille).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Brochage effectif (GPIO_NUM_NC = non câblé). */
typedef struct {
    gpio_num_t pir;            /*!< Sortie du PIR (LP GPIO 0-15) */
    bool       pir_active_high;
    gpio_num_t button;         /*!< Bouton de mise en service (LP GPIO, actif bas) */
    gpio_num_t rtc_int;        /*!< INT/SQW du DS3231 (LP GPIO, actif bas) */
    gpio_num_t status_led;     /*!< Voyant d'état (installation uniquement) */
    bool       status_led_active_high;
    gpio_num_t ir_led;         /*!< MOSFET des LED IR 940 nm (PWM) */
    gpio_num_t ircut_a;        /*!< IR-cut : fil unique ou entrée A du pont en H */
    gpio_num_t ircut_b;        /*!< IR-cut : entrée B du pont en H */
    gpio_num_t batt_adc;       /*!< Mesure de la tension batterie */
    gpio_num_t ldr_adc;        /*!< Photorésistance optionnelle */
    gpio_num_t power_off;      /*!< OFF de l'interrupteur d'alimentation (architecture « coupure ») */
    gpio_num_t i2c_sda;
    gpio_num_t i2c_scl;
    gpio_num_t c6_reset;       /*!< Reset du coprocesseur ESP32-C6 (actif bas) */
    gpio_num_t lcd_backlight;  /*!< Rétroéclairage LCD */
    gpio_num_t audio_pa;       /*!< Ampli audio NS4150B */
    gpio_num_t cam_reset;
    gpio_num_t cam_pwdn;
    gpio_num_t i2s_mclk, i2s_bclk, i2s_ws, i2s_dout, i2s_din;
    gpio_num_t sd_clk, sd_cmd, sd_d0, sd_d1, sd_d2, sd_d3;
} nid_board_pins_t;

/** Brochage issu du menuconfig. */
const nid_board_pins_t *nid_board_pins(void);

/**
 * Mise en sécurité immédiate des broches, à appeler en tout premier dans app_main :
 * LED IR éteintes, voyant éteint, rétroéclairage LCD éteint, ampli audio coupé,
 * ESP32-C6 maintenu en reset. Libère les maintiens (« hold ») posés avant la veille.
 */
esp_err_t nid_board_init_early(void);

/** Crée le bus I2C maître partagé (idempotent). */
esp_err_t nid_board_i2c_init(void);

/** Bus I2C partagé (NULL si nid_board_i2c_init() n'a pas réussi). */
i2c_master_bus_handle_t nid_board_i2c_bus(void);

/**
 * Coprocesseur Wi-Fi ESP32-C6 : true = sort du reset (mode installation),
 * false = maintenu en reset (et maintien conservé pendant la veille profonde).
 */
esp_err_t nid_board_coproc_enable(bool enable);

/** Voyant d'état (FS1.3). Sans effet si aucun voyant n'est configuré. */
void nid_board_status_led(bool on);

/** Fait clignoter le voyant d'état (bloquant) : n éclats de on_ms / off_ms. */
void nid_board_status_blink(int count, int on_ms, int off_ms);

/**
 * Prépare les broches avant la veille profonde : sorties au repos et
 * maintenues (LED IR éteintes, C6 en reset, rétroéclairage et ampli coupés).
 */
void nid_board_prepare_deep_sleep(void);

#ifdef __cplusplus
}
#endif
