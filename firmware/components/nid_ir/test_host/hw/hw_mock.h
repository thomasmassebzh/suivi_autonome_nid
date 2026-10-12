/*
 * Maquette d'ESP-IDF pour tester sur PC la logique matérielle de nid_ir.c
 * (LED PWM, filtre IR-cut) : broches avec maintien, LEDC, esp_timer, mutex.
 *
 * Le temps est virtuel : esp_timer_get_time() ne avance que par vTaskDelay() ou
 * mock_advance_us(), qui déclenchent alors les minuteries échues. Le modèle de broche
 * calcule le niveau réel du plot (maintien, routage LEDC, tirage) et consigne chaque
 * changement dans une trace datée, ce qui permet de vérifier la largeur des impulsions
 * et l'absence d'état intermédiaire.
 *
 * Ce fichier est inclus par tous les en-têtes factices de hw/ ; il ne reproduit que le
 * comportement de l'API utilisé par nid_ir.c (vérifié dans les sources ESP-IDF v6.1).
 */
#pragma once

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"

/* ------------------------------------------------------------------------- */
/* Temps virtuel, minuteries                                                 */
/* ------------------------------------------------------------------------- */

typedef void (*esp_timer_cb_t)(void *arg);

typedef enum { ESP_TIMER_TASK = 0 } esp_timer_dispatch_t;

typedef struct {
    esp_timer_cb_t callback;
    void *arg;
    esp_timer_dispatch_t dispatch_method;
    const char *name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;

struct mock_timer {
    esp_timer_cb_t callback;
    void *arg;
    bool active;
    int64_t due_us;
};
typedef struct mock_timer *esp_timer_handle_t;

#define MOCK_MAX_TIMERS 4
static struct mock_timer g_timers[MOCK_MAX_TIMERS];
static int g_timer_count;
static int64_t g_now_us;
static esp_err_t g_timer_create_error;   /* erreur injectée par esp_timer_create() */
static esp_err_t g_timer_start_error;    /* erreur injectée par esp_timer_start_once() */
static int g_timer_starts;

static void mock_sync_pads(void);

/* Avance le temps virtuel jusqu'à target_us en exécutant les minuteries échues, dans l'ordre. */
static void mock_advance_to(int64_t target_us)
{
    for (;;) {
        struct mock_timer *next = NULL;
        for (int i = 0; i < g_timer_count; i++) {
            if (g_timers[i].active && g_timers[i].due_us <= target_us && (next == NULL || g_timers[i].due_us < next->due_us)) {
                next = &g_timers[i];
            }
        }
        if (next == NULL) {
            break;
        }
        if (next->due_us > g_now_us) {
            g_now_us = next->due_us;
        }
        next->active = false;
        next->callback(next->arg);
    }
    if (target_us > g_now_us) {
        g_now_us = target_us;
    }
}

static void mock_advance_us(int64_t us)
{
    mock_advance_to(g_now_us + us);
}

static int64_t esp_timer_get_time(void)
{
    return g_now_us;
}

static esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    if (g_timer_create_error != ESP_OK) {
        return g_timer_create_error;
    }
    if (g_timer_count >= MOCK_MAX_TIMERS) {
        return ESP_ERR_NO_MEM;
    }
    struct mock_timer *t = &g_timers[g_timer_count++];
    memset(t, 0, sizeof(*t));
    t->callback = args->callback;
    t->arg = args->arg;
    *out = t;
    return ESP_OK;
}

static esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{
    if (g_timer_start_error != ESP_OK) {
        return g_timer_start_error;
    }
    if (timer->active) {
        return ESP_ERR_INVALID_STATE;   /* comme ESP-IDF : le minuteur doit être arrêté */
    }
    timer->active = true;
    timer->due_us = g_now_us + (int64_t)timeout_us;
    g_timer_starts++;
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* FreeRTOS : mutex et délais                                                */
/* ------------------------------------------------------------------------- */

typedef uint32_t TickType_t;
typedef struct { bool taken; } mock_sem_t;
typedef mock_sem_t *SemaphoreHandle_t;

#define portMAX_DELAY      0xffffffffU
#define pdMS_TO_TICKS(ms)  ((TickType_t)(ms))   /* CONFIG_FREERTOS_HZ = 1000 */

static mock_sem_t g_sems[4];
static int g_sem_count;
static int g_lock_violations;   /* prise d'un mutex déjà pris : blocage dans la réalité */
static bool g_sem_create_fail;

static SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    if (g_sem_create_fail || g_sem_count >= 4) {
        return NULL;
    }
    mock_sem_t *s = &g_sems[g_sem_count++];
    s->taken = false;
    return s;
}

static int xSemaphoreTake(SemaphoreHandle_t h, TickType_t timeout)
{
    (void)timeout;
    if (h->taken) {
        g_lock_violations++;
        return 0;
    }
    h->taken = true;
    return 1;
}

static int xSemaphoreGive(SemaphoreHandle_t h)
{
    h->taken = false;
    return 1;
}

static void vTaskDelay(TickType_t ticks)
{
    mock_advance_us((int64_t)ticks * 1000);
}

/* ------------------------------------------------------------------------- */
/* Journal                                                                   */
/* ------------------------------------------------------------------------- */

static int g_log_count[128];       /* nombre de messages par niveau ('E', 'W', 'I', 'D') */
static bool g_log_verbose;

static void mock_log(char level, const char *tag, const char *fmt, ...)
{
    g_log_count[(int)level]++;
    if (g_log_verbose) {
        va_list ap;
        va_start(ap, fmt);
        printf("      [%c %s] ", level, tag);
        vprintf(fmt, ap);
        printf("\n");
        va_end(ap);
    }
}

#define ESP_LOGE(tag, fmt, ...) mock_log('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) mock_log('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) mock_log('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) mock_log('D', tag, fmt, ##__VA_ARGS__)

static const char *esp_err_to_name(esp_err_t err)
{
    return err == ESP_OK ? "ESP_OK" : "ERREUR";
}

#define RTC_DATA_ATTR

/* ------------------------------------------------------------------------- */
/* GPIO : modèle de plot avec maintien                                       */
/* ------------------------------------------------------------------------- */

typedef int gpio_num_t;
#define GPIO_NUM_NC (-1)

typedef enum { GPIO_MODE_DISABLE = 0, GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2 } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE = 1 } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE = 1 } gpio_pulldown_t;
typedef enum { GPIO_INTR_DISABLE = 0 } gpio_int_type_t;
typedef enum { GPIO_FLOATING = 3 } gpio_pull_mode_t;

typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

/* Niveaux du plot observés (mock_pad_level) */
#define PAD_FLOATING   (-1)   /* sortie désactivée, sans tirage */
#define PAD_LOW        0
#define PAD_HIGH       1
#define PAD_PWM        2      /* rapport cyclique intermédiaire */
#define PAD_PULLED_UP  3      /* sortie désactivée, tirage vers le haut actif */

#define MOCK_PINS 64

typedef struct {
    bool reserved;      /* réservation du pilote (esp_gpio_reserve) */
    bool out_enabled;
    bool ledc_routed;   /* sortie reliée à un canal LEDC */
    bool pull_up;
    bool hold;
    int reg_level;      /* registre GPIO_OUT */
    int held_level;     /* niveau figé par le maintien */
    int last_pad;       /* dernier niveau consigné dans la trace */
} mock_pin_t;

static mock_pin_t g_pin[MOCK_PINS];
static int g_gpio_calls;              /* appels à l'API GPIO (hors consultation) */
static int g_conflict_warnings;       /* avertissements « GPIO not usable, maybe conflict » du pilote */
static bool g_hold_unsupported;       /* gpio_hold_en() renvoie ESP_ERR_NOT_SUPPORTED */

/* ------------------------------------------------------------------------- */
/* LEDC                                                                      */
/* ------------------------------------------------------------------------- */

typedef enum { LEDC_LOW_SPEED_MODE = 0, LEDC_SPEED_MODE_MAX } ledc_mode_t;
typedef enum {
    LEDC_TIMER_0 = 0, LEDC_TIMER_1, LEDC_TIMER_2, LEDC_TIMER_3, LEDC_TIMER_MAX,
} ledc_timer_t;
typedef enum {
    LEDC_CHANNEL_0 = 0, LEDC_CHANNEL_1, LEDC_CHANNEL_2, LEDC_CHANNEL_3,
    LEDC_CHANNEL_4, LEDC_CHANNEL_5, LEDC_CHANNEL_6, LEDC_CHANNEL_7, LEDC_CHANNEL_MAX,
} ledc_channel_t;
typedef int ledc_timer_bit_t;
typedef enum {
    LEDC_AUTO_CLK = 0, LEDC_USE_XTAL_CLK = 1, LEDC_USE_PLL_DIV_CLK = 2, LEDC_USE_RC_FAST_CLK = 3,
} ledc_clk_cfg_t;

typedef struct {
    ledc_mode_t speed_mode;
    ledc_timer_bit_t duty_resolution;
    ledc_timer_t timer_num;
    uint32_t freq_hz;
    ledc_clk_cfg_t clk_cfg;
    bool deconfigure;
} ledc_timer_config_t;

typedef struct {
    int gpio_num;
    ledc_mode_t speed_mode;
    ledc_channel_t channel;
    ledc_timer_t timer_sel;
    uint32_t duty;
    int hpoint;
    bool deconfigure;
} ledc_channel_config_t;

typedef struct {
    bool timer_ok;
    ledc_timer_t timer;
    uint32_t freq_hz;
    int bits;
    ledc_clk_cfg_t clk;
    bool channel_ok;
    int gpio;
    ledc_channel_t channel;
    bool out_en;          /* sortie du canal active */
    uint32_t duty;        /* rapport cyclique appliqué */
    uint32_t pending;     /* rapport cyclique programmé, pas encore appliqué */
    int idle_level;
    int timer_attempts;
    int duty_updates;
    int stops;
} mock_ledc_t;

static mock_ledc_t g_ledc;
static unsigned g_ledc_fail_clocks;    /* masque des horloges refusées (bit = ledc_clk_cfg_t) */
static int g_ledc_fail_bits_above;     /* résolutions supérieures refusées (0 = aucune limite) */
static bool g_ledc_channel_fail;

/* ------------------------------------------------------------------------- */
/* Niveau réel des plots et trace                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    int64_t t_us;
    int pin;
    int level;
} mock_event_t;

#define MOCK_TRACE_MAX 4096
static mock_event_t g_trace[MOCK_TRACE_MAX];
static int g_trace_n;

static int mock_pad_free(int pin)
{
    const mock_pin_t *p = &g_pin[pin];
    if (!p->out_enabled) {
        return p->pull_up ? PAD_PULLED_UP : PAD_FLOATING;
    }
    if (p->ledc_routed) {
        if (!g_ledc.out_en) {
            return g_ledc.idle_level ? PAD_HIGH : PAD_LOW;
        }
        if (g_ledc.duty == 0) {
            return PAD_LOW;
        }
        const uint32_t max = 1u << g_ledc.bits;
        return g_ledc.duty >= max ? PAD_HIGH : PAD_PWM;
    }
    return p->reg_level ? PAD_HIGH : PAD_LOW;
}

static int mock_pad_level(int pin)
{
    return g_pin[pin].hold ? g_pin[pin].held_level : mock_pad_free(pin);
}

/* Consigne dans la trace tout changement de niveau d'un plot. */
static void mock_sync_pads(void)
{
    for (int i = 0; i < MOCK_PINS; i++) {
        const int level = mock_pad_level(i);
        if (level != g_pin[i].last_pad) {
            g_pin[i].last_pad = level;
            if (g_trace_n < MOCK_TRACE_MAX) {
                g_trace[g_trace_n++] = (mock_event_t){ g_now_us, i, level };
            }
        }
    }
}

/* ----- API GPIO ----- */

static esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    g_gpio_calls++;
    g_pin[pin].reg_level = level ? 1 : 0;
    mock_sync_pads();
    return ESP_OK;
}

static esp_err_t gpio_config(const gpio_config_t *cfg)
{
    g_gpio_calls++;
    for (int i = 0; i < MOCK_PINS; i++) {
        if (!((cfg->pin_bit_mask >> i) & 1ULL)) {
            continue;
        }
        mock_pin_t *p = &g_pin[i];
        if ((cfg->mode & GPIO_MODE_OUTPUT) != 0) {
            if (p->reserved) {
                g_conflict_warnings++;
            }
            p->reserved = true;
        }
        p->out_enabled = (cfg->mode & GPIO_MODE_OUTPUT) != 0;
        p->ledc_routed = false;
        p->pull_up = (cfg->pull_up_en == GPIO_PULLUP_ENABLE);
    }
    mock_sync_pads();
    return ESP_OK;
}

static esp_err_t gpio_reset_pin(gpio_num_t pin)
{
    g_gpio_calls++;
    mock_pin_t *p = &g_pin[pin];
    p->out_enabled = false;
    p->ledc_routed = false;
    p->pull_up = true;     /* comme ESP-IDF : sortie coupée, tirage vers le haut */
    p->reserved = false;   /* esp_gpio_revoke() */
    mock_sync_pads();
    return ESP_OK;
}

static esp_err_t gpio_set_pull_mode(gpio_num_t pin, gpio_pull_mode_t mode)
{
    g_gpio_calls++;
    if (mode == GPIO_FLOATING) {
        g_pin[pin].pull_up = false;
    }
    mock_sync_pads();
    return ESP_OK;
}

static esp_err_t gpio_hold_en(gpio_num_t pin)
{
    g_gpio_calls++;
    if (g_hold_unsupported) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    mock_pin_t *p = &g_pin[pin];
    if (!p->hold) {
        p->held_level = mock_pad_free(pin);   /* le maintien fige l'état du plot à cet instant */
        p->hold = true;
    }
    mock_sync_pads();
    return ESP_OK;
}

static esp_err_t gpio_hold_dis(gpio_num_t pin)
{
    g_gpio_calls++;
    g_pin[pin].hold = false;
    mock_sync_pads();
    return ESP_OK;
}

/* ----- API LEDC ----- */

static esp_err_t ledc_timer_config(const ledc_timer_config_t *cfg)
{
    g_ledc.timer_attempts++;
    if (((g_ledc_fail_clocks >> cfg->clk_cfg) & 1u) != 0 ||
        (g_ledc_fail_bits_above != 0 && cfg->duty_resolution > g_ledc_fail_bits_above)) {
        return ESP_FAIL;
    }
    g_ledc.timer_ok = true;
    g_ledc.timer = cfg->timer_num;
    g_ledc.freq_hz = cfg->freq_hz;
    g_ledc.bits = cfg->duty_resolution;
    g_ledc.clk = cfg->clk_cfg;
    return ESP_OK;
}

static esp_err_t ledc_channel_config(const ledc_channel_config_t *cfg)
{
    if (g_ledc_channel_fail) {
        return ESP_FAIL;
    }
    mock_pin_t *p = &g_pin[cfg->gpio_num];
    if (p->reserved) {
        g_conflict_warnings++;   /* ESP_LOGW(LEDC_TAG, "GPIO %d is not usable, maybe conflict with others") */
    }
    p->reserved = true;
    p->out_enabled = true;
    p->ledc_routed = true;
    g_ledc.channel_ok = true;
    g_ledc.gpio = cfg->gpio_num;
    g_ledc.channel = cfg->channel;
    g_ledc.duty = cfg->duty;       /* ledc_update_duty() est appelée par la configuration : sortie active */
    g_ledc.out_en = true;
    mock_sync_pads();
    return ESP_OK;
}

static esp_err_t ledc_set_duty(ledc_mode_t mode, ledc_channel_t channel, uint32_t duty)
{
    (void)mode;
    (void)channel;
    g_ledc.pending = duty;
    return ESP_OK;
}

static esp_err_t ledc_update_duty(ledc_mode_t mode, ledc_channel_t channel)
{
    (void)mode;
    (void)channel;
    g_ledc.duty = g_ledc.pending;
    g_ledc.out_en = true;
    g_ledc.duty_updates++;
    mock_sync_pads();
    return ESP_OK;
}

static esp_err_t ledc_stop(ledc_mode_t mode, ledc_channel_t channel, uint32_t idle_level)
{
    (void)mode;
    (void)channel;
    g_ledc.idle_level = (int)idle_level;
    g_ledc.out_en = false;
    g_ledc.stops++;
    mock_sync_pads();
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* nid_board                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    gpio_num_t ir_led;
    gpio_num_t ircut_a;
    gpio_num_t ircut_b;
} nid_board_pins_t;

static nid_board_pins_t g_board_pins = { 32, 33, 36 };

static const nid_board_pins_t *nid_board_pins(void)
{
    return &g_board_pins;
}
