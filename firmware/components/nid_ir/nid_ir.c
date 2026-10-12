/*
 * nid_ir — éclairage infrarouge 940 nm, filtre IR-cut, décision jour/nuit
 *
 * Partie matérielle : PWM des LED IR et commande du filtre IR-cut. Le calcul
 * jour/nuit (nid_ir_sun_times, nid_ir_is_night) est dans nid_ir_sun.c : code pur,
 * testé sur PC dans test_host/.
 *
 * LED IR : MOSFET sur la broche CONFIG_NID_PIN_IR_LED, PWM LEDC à
 * CONFIG_NID_IR_LED_PWM_FREQ_HZ (inaudible, FC1.2). « Éteintes » = canal arrêté
 * avec un niveau de repos bas (FC1.1) ; nid_board maintient la broche basse
 * pendant la veille profonde.
 *
 * Filtre IR-cut (menuconfig « Commande du filtre IR-cut ») :
 *   - aucune : nid_ir_set_day_filter() renvoie ESP_ERR_NOT_SUPPORTED ;
 *   - un fil : niveau logique permanent (CONFIG_NID_IRCUT_DAY_LEVEL = jour) ;
 *   - deux fils (pont en H, filtre bistable) : impulsion de CONFIG_NID_IRCUT_PULSE_MS,
 *     jour = A haut / B bas, nuit = A bas / B haut (inverser les deux broches du
 *     menuconfig si le montage est à l'envers). L'impulsion est terminée par une
 *     minuterie esp_timer : la commande ne retarde pas le démarrage de la caméra.
 *
 * FC1.2 : le filtre fait un « clic » à chaque changement. Il n'est donc commandé
 * que si la position change ; elle est mémorisée en mémoire RTC (conservée pendant
 * la veille profonde ; inconnue après une mise sous tension ou un redémarrage sur
 * défaut, d'où une commande « à vide » sans effet mécanique si la position est
 * déjà la bonne). Les broches du filtre sont maintenues (« hold ») au repos : pont
 * éteint, ou niveau mémorisé pour un fil unique, y compris pendant la veille.
 */
#include "nid_ir.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nid_board.h"

static const char *TAG = "nid_ir";

/* Derniers timer et canal LEDC : les premiers restent disponibles pour les autres modules. */
#define LED_SPEED_MODE   LEDC_LOW_SPEED_MODE
#define LED_TIMER        ((ledc_timer_t)(LEDC_TIMER_MAX - 1))
#define LED_CHANNEL      ((ledc_channel_t)(LEDC_CHANNEL_MAX - 1))
#define LED_BITS_MAX     10   /* 1024 niveaux : à 20 kHz, source d'horloge d'au moins 20,5 MHz */
#define LED_BITS_MIN     5

#define PULSE_US         ((uint64_t)CONFIG_NID_IRCUT_PULSE_MS * 1000ULL)
#define PULSE_WAIT_MARGIN_MS  200

static SemaphoreHandle_t s_lock;      /* sérialise la commande des LED et du filtre (tâche appelante, minuterie) */
static bool s_initialized;
static bool s_led_ready;
static uint32_t s_duty_max;           /* 2^résolution : rapport cyclique de 100 % */

/* ------------------------------------------------------------------------- */
/* LED IR (PWM LEDC)                                                         */
/* ------------------------------------------------------------------------- */

/*
 * Timer LEDC à la fréquence demandée, avec la meilleure résolution possible. Sur l'ESP32-P4
 * tous les timers partagent la même horloge source : si un autre module en utilise déjà une,
 * la sélection automatique peut échouer, d'où l'essai des sources une à une.
 */
static esp_err_t led_timer_config(void)
{
    static const ledc_clk_cfg_t clocks[] = {
        LEDC_AUTO_CLK, LEDC_USE_XTAL_CLK, LEDC_USE_PLL_DIV_CLK, LEDC_USE_RC_FAST_CLK,
    };
    esp_err_t err = ESP_FAIL;
    for (size_t c = 0; c < sizeof(clocks) / sizeof(clocks[0]) && err != ESP_OK; c++) {
        for (int bits = LED_BITS_MAX; bits >= LED_BITS_MIN && err != ESP_OK; bits--) {
            const ledc_timer_config_t timer = {
                .speed_mode = LED_SPEED_MODE,
                .duty_resolution = (ledc_timer_bit_t)bits,
                .timer_num = LED_TIMER,
                .freq_hz = (uint32_t)CONFIG_NID_IR_LED_PWM_FREQ_HZ,
                .clk_cfg = clocks[c],
            };
            err = ledc_timer_config(&timer);
            if (err == ESP_OK) {
                s_duty_max = 1u << bits;
            }
        }
    }
    return err;
}

/* Broche en sortie simple au niveau demandé, sans résistance de tirage (même réglage que nid_board). */
static esp_err_t pin_to_output(gpio_num_t pin, int level)
{
    gpio_set_level(pin, level);
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

static esp_err_t led_init(gpio_num_t pin)
{
    if (pin == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "LED IR : broche non câblée, pas d'éclairage infrarouge");
        return ESP_OK;
    }
    esp_err_t err = led_timer_config();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED IR : timer PWM impossible (%s)", esp_err_to_name(err));
        return err;
    }

    /* nid_board_init_early() a réservé la broche comme sortie basse : on la libère pour que le pilote
     * LEDC la prenne sans avertissement de conflit. Pendant ces quelques microsecondes la grille du
     * MOSFET n'est tenue que par sa résistance de rappel vers la masse. */
    gpio_hold_dis(pin);
    gpio_reset_pin(pin);
    const ledc_channel_config_t channel = {
        .gpio_num = pin,
        .speed_mode = LED_SPEED_MODE,
        .channel = LED_CHANNEL,
        .timer_sel = LED_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED IR : canal PWM impossible (%s)", esp_err_to_name(err));
        pin_to_output(pin, 0);   /* LED éteintes quoi qu'il arrive */
        return err;
    }
    gpio_set_pull_mode(pin, GPIO_FLOATING);   /* gpio_reset_pin() a activé le tirage vers le haut */
    ledc_stop(LED_SPEED_MODE, LED_CHANNEL, 0);
    s_led_ready = true;
    ESP_LOGD(TAG, "LED IR : GPIO %d, PWM %d Hz, %" PRIu32 " niveaux", (int)pin, CONFIG_NID_IR_LED_PWM_FREQ_HZ, s_duty_max);
    return ESP_OK;
}

void nid_ir_set_led_power(uint8_t percent)
{
    if (!s_led_ready) {
        return;
    }
    if (percent > 100) {
        percent = 100;
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (percent == 0) {
        err = ledc_stop(LED_SPEED_MODE, LED_CHANNEL, 0);
    } else {
        /* PWM linéaire ; 100 % = sortie fixe à l'état haut ; jamais 0 tant qu'une puissance est demandée. */
        uint32_t duty = (uint32_t)(((uint64_t)percent * s_duty_max + 50) / 100);
        if (duty == 0) {
            duty = 1;
        }
        err = ledc_set_duty(LED_SPEED_MODE, LED_CHANNEL, duty);
        if (err == ESP_OK) {
            err = ledc_update_duty(LED_SPEED_MODE, LED_CHANNEL);
        }
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED IR : réglage de la puissance impossible (%s)", esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "LED IR : %u %%", percent);
    }
}

/* ------------------------------------------------------------------------- */
/* Filtre IR-cut                                                             */
/* ------------------------------------------------------------------------- */

#if !CONFIG_NID_IRCUT_NONE

typedef enum {
    FILTER_UNKNOWN = 0,   /* mise sous tension, défaut ou impulsion interrompue : la prochaine commande sera envoyée */
    FILTER_DAY,           /* filtre en place */
    FILTER_NIGHT,         /* filtre retiré */
} filter_state_t;

/* Position du filtre, conservée pendant la veille profonde (initialisée à FILTER_UNKNOWN au démarrage à froid). */
static RTC_DATA_ATTR uint8_t s_filter_state = FILTER_UNKNOWN;

static filter_state_t filter_state_of(bool day)
{
    return day ? FILTER_DAY : FILTER_NIGHT;
}

/* Maintient le niveau de la broche pendant la veille profonde et les redémarrages. */
static void pin_hold(gpio_num_t pin)
{
    esp_err_t err = gpio_hold_en(pin);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO %d : maintien impossible (%s), le niveau ne survivra pas à la veille",
                 (int)pin, esp_err_to_name(err));
    }
}

#if CONFIG_NID_IRCUT_LEVEL

/* ----- Un fil : niveau logique permanent ----- */

static bool s_pin_configured;

static gpio_num_t ircut_pin(void)
{
    return nid_board_pins()->ircut_a;
}

static int level_of(bool day)
{
    return day ? CONFIG_NID_IRCUT_DAY_LEVEL : !CONFIG_NID_IRCUT_DAY_LEVEL;
}

/* Programme le niveau du fil et le maintient (verrou pris). */
static void level_drive(bool day)
{
    const gpio_num_t pin = ircut_pin();
    /* Le registre reçoit d'abord le niveau voulu : la broche, encore maintenue, ne bouge pas ; en
     * relâchant le maintien elle prend ce niveau d'un seul coup, sans passage par un état intermédiaire. */
    if (!s_pin_configured) {
        pin_to_output(pin, level_of(day));
        s_pin_configured = true;
    } else {
        gpio_set_level(pin, (uint32_t)level_of(day));
    }
    gpio_hold_dis(pin);
    pin_hold(pin);   /* le niveau survit à la veille profonde et aux redémarrages */
}

static esp_err_t ircut_init(void)
{
    if (ircut_pin() == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "IR-cut : broche non câblée, filtre non commandé");
        return ESP_OK;
    }
    if (s_filter_state != FILTER_UNKNOWN) {
        /* Réveil de veille profonde : la broche est restée maintenue au bon niveau, on remet seulement le
         * registre d'accord avec elle (aucun mouvement du filtre). */
        pin_to_output(ircut_pin(), level_of(s_filter_state == FILTER_DAY));
        s_pin_configured = true;
    }
    return ESP_OK;
}

static bool ircut_available(void)
{
    return ircut_pin() != GPIO_NUM_NC;
}

/* Commande le filtre (verrou pris). */
static esp_err_t ircut_command_locked(bool day)
{
    if (s_filter_state == filter_state_of(day)) {
        return ESP_OK;   /* déjà en place : aucun bruit */
    }
    level_drive(day);
    s_filter_state = filter_state_of(day);
    ESP_LOGI(TAG, "filtre IR-cut : %s", day ? "en place (jour)" : "retiré (nuit)");
    return ESP_OK;
}

#else  /* CONFIG_NID_IRCUT_PULSE */

/* ----- Deux fils : impulsion sur un pont en H ----- */

static esp_timer_handle_t s_pulse_timer;
static bool s_pulse_active;      /* impulsion en cours */
static bool s_pulse_day;         /* position visée par l'impulsion en cours */
static bool s_pending;           /* demande contraire reçue pendant l'impulsion */
static bool s_pending_day;

static esp_err_t pulse_pins(gpio_num_t *a, gpio_num_t *b)
{
    *a = nid_board_pins()->ircut_a;
    *b = nid_board_pins()->ircut_b;
    return (*a == GPIO_NUM_NC || *b == GPIO_NUM_NC) ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}

static bool ircut_available(void)
{
    gpio_num_t a, b;
    return pulse_pins(&a, &b) == ESP_OK;
}

static void pulse_finish_locked(void);

/* Début d'impulsion (verrou pris) : la minuterie la termine, sans bloquer l'appelant. */
static void pulse_begin_locked(bool day)
{
    gpio_num_t a, b;
    pulse_pins(&a, &b);

    s_filter_state = FILTER_UNKNOWN;   /* en mouvement : si l'impulsion est interrompue, elle sera renvoyée */
    s_pulse_active = true;
    s_pulse_day = day;
    ESP_LOGI(TAG, "filtre IR-cut : %s", day ? "mise en place (jour)" : "retrait (nuit)");

    /* Registres d'abord (broches encore maintenues au repos), puis relâche : A et B changent ensemble. */
    gpio_set_level(a, day ? 1 : 0);
    gpio_set_level(b, day ? 0 : 1);
    gpio_hold_dis(a);
    gpio_hold_dis(b);

    if (s_pulse_timer == NULL || esp_timer_start_once(s_pulse_timer, PULSE_US) != ESP_OK) {
        /* Cas dégradé (minuterie indisponible) : mieux vaut une impulsion bloquante que pas de filtre. */
        ESP_LOGW(TAG, "IR-cut : minuterie indisponible, impulsion bloquante de %d ms", CONFIG_NID_IRCUT_PULSE_MS);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_NID_IRCUT_PULSE_MS));
        pulse_finish_locked();
    }
}

/* Fin d'impulsion (verrou pris) : pont éteint et maintenu, position mémorisée. */
static void pulse_finish_locked(void)
{
    gpio_num_t a, b;
    pulse_pins(&a, &b);

    gpio_set_level(a, 0);
    gpio_set_level(b, 0);
    pin_hold(a);
    pin_hold(b);
    s_filter_state = filter_state_of(s_pulse_day);
    s_pulse_active = false;
    ESP_LOGD(TAG, "IR-cut : impulsion terminée");

    const bool again = s_pending && (s_pending_day != s_pulse_day);
    s_pending = false;
    if (again) {
        pulse_begin_locked(s_pending_day);
    }
}

static void pulse_timer_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pulse_finish_locked();
    xSemaphoreGive(s_lock);
}

static esp_err_t ircut_init(void)
{
    gpio_num_t a, b;
    if (pulse_pins(&a, &b) != ESP_OK) {
        ESP_LOGW(TAG, "IR-cut : broches A/B non câblées, filtre non commandé");
        return ESP_OK;
    }
    const esp_timer_create_args_t args = {
        .callback = pulse_timer_cb,
        .name = "nid_ircut",
    };
    esp_err_t err = esp_timer_create(&args, &s_pulse_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IR-cut : minuterie impossible (%s), impulsions bloquantes", esp_err_to_name(err));
        s_pulse_timer = NULL;
    }
    /* Repos : pont éteint (A = B = bas) et maintenu, y compris pendant la veille profonde. */
    pin_to_output(a, 0);
    pin_to_output(b, 0);
    gpio_hold_dis(a);
    gpio_hold_dis(b);
    pin_hold(a);
    pin_hold(b);
    return ESP_OK;
}

/* Commande le filtre (verrou pris). */
static esp_err_t ircut_command_locked(bool day)
{
    if (s_pulse_active) {
        /* Mouvement en cours : la dernière demande l'emporte, elle est traitée à la fin de l'impulsion. */
        s_pending = (day != s_pulse_day);
        s_pending_day = day;
    } else if (s_filter_state != filter_state_of(day)) {
        pulse_begin_locked(day);
    }
    return ESP_OK;
}

/* Attend la fin d'une impulsion en cours (au plus sa durée), pour ne pas la couper par la veille. */
static void pulse_wait_end(void)
{
    const int64_t deadline_us = esp_timer_get_time() + ((int64_t)CONFIG_NID_IRCUT_PULSE_MS + PULSE_WAIT_MARGIN_MS) * 1000;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool active = s_pulse_active;
        xSemaphoreGive(s_lock);
        if (!active) {
            return;
        }
        if (esp_timer_get_time() > deadline_us) {
            ESP_LOGW(TAG, "IR-cut : impulsion non terminée");
            return;
        }
        vTaskDelay(1);
    }
}

#endif  /* CONFIG_NID_IRCUT_LEVEL / PULSE */

#endif  /* !CONFIG_NID_IRCUT_NONE */

esp_err_t nid_ir_set_day_filter(bool day)
{
#if CONFIG_NID_IRCUT_NONE
    (void)day;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ircut_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ircut_command_locked(day);
    xSemaphoreGive(s_lock);
    return err;
#endif
}

/* ------------------------------------------------------------------------- */
/* Initialisation et commandes groupées                                      */
/* ------------------------------------------------------------------------- */

esp_err_t nid_ir_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "mémoire insuffisante");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = led_init(nid_board_pins()->ir_led);
#if !CONFIG_NID_IRCUT_NONE
    esp_err_t err_cut = ircut_init();
    if (err == ESP_OK) {
        err = err_cut;
    }
#endif
    s_initialized = true;
    return err;
}

void nid_ir_apply(bool night, const nid_config_t *cfg)
{
    const uint8_t power = (cfg != NULL) ? cfg->ir_power_pct : 100;
    esp_err_t err;

    if (night) {
        /* Filtre retiré d'abord (la commande ne bloque pas), puis éclairage. */
        err = nid_ir_set_day_filter(false);
        nid_ir_set_led_power(power);
    } else {
        nid_ir_set_led_power(0);
        err = nid_ir_set_day_filter(true);
    }
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "filtre IR-cut non commandé (%s)", esp_err_to_name(err));
    }
    ESP_LOGD(TAG, "%s : LED IR %u %%", night ? "nuit" : "jour", (unsigned)(night ? power : 0));
}

void nid_ir_off(void)
{
    nid_ir_set_led_power(0);
#if CONFIG_NID_IRCUT_PULSE
    if (s_initialized) {
        pulse_wait_end();
    }
#endif
}
