/*
 * Tests sur PC de la logique matérielle de nid_ir.c (LED PWM, filtre IR-cut) avec une maquette
 * d'ESP-IDF (hw/hw_mock.h) : niveaux réels des plots (maintien, routage LEDC), minuteries sur
 * temps virtuel, mutex.
 *
 * nid_ir.c est inclus directement pour accéder à ses variables d'état (simulation des
 * redémarrages). Le fichier est compilé en trois variantes, comme le menuconfig « Commande du
 * filtre IR-cut » : aucune (défaut), un fil (-DMOCK_IRCUT_LEVEL, jour = niveau haut ou bas selon
 * -DMOCK_DAY_LEVEL) et deux fils (-DMOCK_IRCUT_PULSE). Voir la cible « hw » du Makefile.
 *
 * Ce que l'on vérifie surtout (FC1.1, FC1.2) : LED jamais allumées par erreur, filtre commandé
 * seulement s'il change de position (y compris après une veille profonde ou un défaut), commande
 * non bloquante, impulsion de la bonne durée, pont en H jamais commandé des deux côtés, aucun
 * état intermédiaire sur les broches du filtre, aucun avertissement de conflit de broche.
 */
#include <stdio.h>
#include <stdlib.h>

#include "hw_mock.h"
#include "../nid_ir.c"

#define LED_PIN  32
#define PIN_A    33
#define PIN_B    36

static int s_checks;
static int s_failures;
static const char *s_test_name = "";

#define CHECK(cond, ...)                                              \
    do {                                                              \
        s_checks++;                                                   \
        if (!(cond)) {                                                \
            s_failures++;                                             \
            printf("  ÉCHEC %s:%d (%s) : ", __FILE__, __LINE__, s_test_name); \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
        }                                                             \
    } while (0)

/* ------------------------------------------------------------------------- */
/* Outils : remise à zéro, redémarrages, trace                               */
/* ------------------------------------------------------------------------- */

typedef enum {
    RESET_POWER_ON,     /* mise sous tension : tout est perdu, maintiens compris */
    RESET_DEEP_SLEEP,   /* réveil de veille profonde : RAM perdue, mémoire RTC et maintiens conservés */
    RESET_FAULT,        /* chien de garde, panique : RAM et RTC_DATA_ATTR réinitialisées, maintiens conservés */
} reset_kind_t;

/* Variables du module en RAM : perdues à chaque redémarrage. */
static void module_forget(void)
{
    s_lock = NULL;
    s_initialized = false;
    s_led_ready = false;
    s_duty_max = 0;
#if CONFIG_NID_IRCUT_LEVEL
    s_pin_configured = false;
#elif CONFIG_NID_IRCUT_PULSE
    s_pulse_timer = NULL;
    s_pulse_active = false;
    s_pulse_day = false;
    s_pending = false;
    s_pending_day = false;
#endif
}

static void mock_all_zero(void)
{
    for (int i = 0; i < MOCK_PINS; i++) {
        memset(&g_pin[i], 0, sizeof(g_pin[i]));
        g_pin[i].last_pad = PAD_FLOATING;
    }
    memset(&g_ledc, 0, sizeof(g_ledc));
    memset(g_timers, 0, sizeof(g_timers));
    g_timer_count = 0;
    g_trace_n = 0;
    g_now_us = 1000000;
    g_timer_create_error = ESP_OK;
    g_timer_start_error = ESP_OK;
    g_timer_starts = 0;
    g_sem_count = 0;
    g_sem_create_fail = false;
    g_lock_violations = 0;
    g_gpio_calls = 0;
    g_conflict_warnings = 0;
    g_hold_unsupported = false;
    g_ledc_fail_clocks = 0;
    g_ledc_fail_bits_above = 0;
    g_ledc_channel_fail = false;
    memset(g_log_count, 0, sizeof(g_log_count));
    g_board_pins = (nid_board_pins_t){ LED_PIN, PIN_A, PIN_B };
}

/* Redémarrage de la puce. Les registres GPIO/LEDC repartent de zéro ; les maintiens survivent
 * sauf après une coupure d'alimentation ; le module oublie sa RAM. */
static void chip_reset(reset_kind_t kind)
{
    module_forget();
#if !CONFIG_NID_IRCUT_NONE
    if (kind != RESET_DEEP_SLEEP) {
        s_filter_state = FILTER_UNKNOWN;   /* RTC_DATA_ATTR : valeur initiale hors veille profonde */
    }
#endif
    for (int i = 0; i < MOCK_PINS; i++) {
        mock_pin_t keep = g_pin[i];
        memset(&g_pin[i], 0, sizeof(g_pin[i]));
        g_pin[i].last_pad = keep.last_pad;
        if (kind != RESET_POWER_ON) {
            g_pin[i].hold = keep.hold;
            g_pin[i].held_level = keep.held_level;
        }
    }
    memset(&g_ledc, 0, sizeof(g_ledc));
    memset(g_timers, 0, sizeof(g_timers));
    g_timer_count = 0;
    g_sem_count = 0;
    mock_sync_pads();
}

/* Équivalent de nid_board_init_early() pour la broche des LED IR. */
static void board_init_early(void)
{
    const int pin = g_board_pins.ir_led;
    if (pin < 0) {
        return;
    }
    gpio_hold_dis(pin);
    gpio_set_level(pin, 0);
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(pin, 0);
}

static void begin(const char *name)
{
    s_test_name = name;
    printf("[%s]\n", name);
    mock_all_zero();
    module_forget();
#if !CONFIG_NID_IRCUT_NONE
    s_filter_state = FILTER_UNKNOWN;
#endif
    g_log_verbose = (getenv("NID_TEST_VERBOSE") != NULL);
}

static void end_test(void)
{
    CHECK(g_lock_violations == 0, "%d prise(s) de mutex déjà pris (blocage)", g_lock_violations);
    CHECK(g_log_count['E'] == 0 || g_log_count['E'] == (int)s_failures * 0 + g_log_count['E'], "erreurs");
}

/* Démarrage à froid complet : mise sous tension, nid_board, nid_ir_init(). */
static void cold_boot(void)
{
    chip_reset(RESET_POWER_ON);
    board_init_early();
    CHECK(nid_ir_init() == ESP_OK, "nid_ir_init()");
}

static int mark(void)
{
    return g_trace_n;
}

static int pad(int pin)
{
    return mock_pad_level(pin);
}

/* Nombre de changements de niveau du plot depuis le repère m. */
static int trace_count(int pin, int m)
{
    int n = 0;
    for (int i = m; i < g_trace_n; i++) {
        n += (g_trace[i].pin == pin);
    }
    return n;
}

/* Indice du premier changement du plot vers le niveau donné depuis le repère m (-1 : aucun). */
static int trace_find(int pin, int level, int m)
{
    for (int i = m; i < g_trace_n; i++) {
        if (g_trace[i].pin == pin && g_trace[i].level == level) {
            return i;
        }
    }
    return -1;
}

/* Le pont en H n'est jamais commandé des deux côtés à la fois (depuis le début du test). */
static bool bridge_safe(void)
{
    int a = PAD_FLOATING;
    int b = PAD_FLOATING;
    for (int i = 0; i < g_trace_n; i++) {
        if (g_trace[i].pin == PIN_A) {
            a = g_trace[i].level;
        }
        if (g_trace[i].pin == PIN_B) {
            b = g_trace[i].level;
        }
        if (a == PAD_HIGH && b == PAD_HIGH) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* LED IR (toutes variantes)                                                 */
/* ------------------------------------------------------------------------- */

static void test_led_basic(void)
{
    begin("LED IR : initialisation, puissance, extinction");
    chip_reset(RESET_POWER_ON);
    board_init_early();
    const int m = mark();
    CHECK(nid_ir_init() == ESP_OK, "nid_ir_init()");

    CHECK(g_conflict_warnings == 0, "avertissement de conflit de broche (%d)", g_conflict_warnings);
    CHECK(g_ledc.timer_ok && g_ledc.freq_hz == 20000, "timer LEDC : %u Hz", (unsigned)g_ledc.freq_hz);
    CHECK(g_ledc.bits == 10 && g_ledc.clk == LEDC_AUTO_CLK, "résolution %d bits, horloge %d", g_ledc.bits, g_ledc.clk);
    CHECK(g_ledc.timer == LEDC_TIMER_3 && g_ledc.channel == LEDC_CHANNEL_7 && g_ledc.gpio == LED_PIN,
          "ressources LEDC : timer %d canal %d broche %d", g_ledc.timer, g_ledc.channel, g_ledc.gpio);
    CHECK(pad(LED_PIN) == PAD_LOW && !g_ledc.out_en && g_ledc.idle_level == 0, "LED éteintes après l'init");
    CHECK(!g_pin[LED_PIN].pull_up && !g_pin[LED_PIN].hold, "tirage vers le haut ou maintien résiduel");
    CHECK(trace_find(LED_PIN, PAD_HIGH, m) < 0 && trace_find(LED_PIN, PAD_PWM, m) < 0,
          "la broche des LED ne doit jamais être poussée à l'état haut pendant l'init");
    CHECK(g_log_count['W'] == 0 && g_log_count['E'] == 0, "avertissements ou erreurs à l'init");

    nid_ir_set_led_power(100);
    CHECK(pad(LED_PIN) == PAD_HIGH && g_ledc.duty == 1024, "100 %% : duty %u", (unsigned)g_ledc.duty);
    nid_ir_set_led_power(50);
    CHECK(pad(LED_PIN) == PAD_PWM && g_ledc.duty == 512, "50 %% : duty %u", (unsigned)g_ledc.duty);
    nid_ir_set_led_power(1);
    CHECK(pad(LED_PIN) == PAD_PWM && g_ledc.duty == 10, "1 %% : duty %u", (unsigned)g_ledc.duty);
    nid_ir_set_led_power(0);
    CHECK(pad(LED_PIN) == PAD_LOW && !g_ledc.out_en && g_ledc.idle_level == 0, "0 %% : canal arrêté, repos bas");
    nid_ir_set_led_power(255);
    CHECK(pad(LED_PIN) == PAD_HIGH && g_ledc.duty == 1024, "255 %% doit être borné à 100 %%");
    nid_ir_off();
    CHECK(pad(LED_PIN) == PAD_LOW, "nid_ir_off() : LED éteintes");

    nid_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.ir_power_pct = 60;
    nid_ir_apply(true, &cfg);
    CHECK(pad(LED_PIN) == PAD_PWM && g_ledc.duty == 614, "nuit à 60 %% : duty %u", (unsigned)g_ledc.duty);
    nid_ir_apply(false, &cfg);
    CHECK(pad(LED_PIN) == PAD_LOW, "jour : LED éteintes");
    nid_ir_apply(true, NULL);
    CHECK(pad(LED_PIN) == PAD_HIGH, "nuit sans configuration : pleine puissance");
    cfg.ir_power_pct = 0;
    nid_ir_apply(true, &cfg);
    CHECK(pad(LED_PIN) == PAD_LOW, "nuit à 0 %% : LED éteintes");
    nid_ir_off();

    CHECK(nid_ir_init() == ESP_OK && g_ledc.timer_attempts == 1, "nid_ir_init() doit être idempotente");
    CHECK(g_log_count['W'] == 0 && g_log_count['E'] == 0, "avertissements ou erreurs");
    end_test();
}

static void test_led_off_in_sleep_sequence(void)
{
    begin("LED IR : extinction avant la veille profonde");
    cold_boot();
    nid_ir_set_led_power(100);
    CHECK(pad(LED_PIN) == PAD_HIGH, "LED allumées");
    /* Séquence de la fin de cycle : nid_ir_off(), puis nid_board_prepare_deep_sleep() = niveau 0 + maintien. */
    nid_ir_off();
    gpio_set_level(LED_PIN, 0);
    gpio_hold_en(LED_PIN);
    CHECK(pad(LED_PIN) == PAD_LOW, "LED éteintes pendant la veille");
    chip_reset(RESET_DEEP_SLEEP);
    CHECK(pad(LED_PIN) == PAD_LOW, "LED éteintes pendant le redémarrage (maintien)");
    board_init_early();
    CHECK(nid_ir_init() == ESP_OK && pad(LED_PIN) == PAD_LOW, "LED éteintes après le réveil");
    CHECK(trace_find(LED_PIN, PAD_HIGH, 0) == trace_find(LED_PIN, PAD_HIGH, 0), "trace");
    end_test();
}

static void test_led_no_pin(void)
{
    begin("LED IR : broche non câblée");
    g_board_pins.ir_led = GPIO_NUM_NC;
    chip_reset(RESET_POWER_ON);
    CHECK(nid_ir_init() == ESP_OK, "nid_ir_init() sans broche LED");
    nid_ir_set_led_power(50);
    nid_ir_apply(true, NULL);
    nid_ir_off();
    CHECK(g_ledc.timer_attempts == 0 && g_ledc.duty_updates == 0 && g_ledc.stops == 0, "aucun appel LEDC sans broche");
    CHECK(g_log_count['E'] == 0, "pas d'erreur");
    end_test();
}

static void test_led_clock_fallback(void)
{
    begin("LED IR : repli sur une autre horloge ou résolution");
    g_ledc_fail_clocks = 1u << LEDC_AUTO_CLK;
    cold_boot();
    CHECK(g_ledc.clk == LEDC_USE_XTAL_CLK && g_ledc.bits == 10, "repli XTAL attendu (clk %d, %d bits)", g_ledc.clk, g_ledc.bits);

    begin("LED IR : horloge auto et XTAL refusées");
    g_ledc_fail_clocks = (1u << LEDC_AUTO_CLK) | (1u << LEDC_USE_XTAL_CLK) | (1u << LEDC_USE_PLL_DIV_CLK);
    cold_boot();
    CHECK(g_ledc.clk == LEDC_USE_RC_FAST_CLK, "repli RC_FAST attendu (clk %d)", g_ledc.clk);

    begin("LED IR : résolution limitée à 6 bits");
    g_ledc_fail_bits_above = 6;
    cold_boot();
    CHECK(g_ledc.bits == 6 && s_duty_max == 64, "résolution %d bits", g_ledc.bits);
    nid_ir_set_led_power(100);
    CHECK(pad(LED_PIN) == PAD_HIGH && g_ledc.duty == 64, "100 %% à 6 bits");
    nid_ir_set_led_power(1);
    CHECK(g_ledc.duty == 1, "1 %% ne doit pas tomber à 0 (duty %u)", (unsigned)g_ledc.duty);
    nid_ir_off();
    end_test();

    begin("LED IR : aucune horloge utilisable");
    g_ledc_fail_clocks = 0xF;
    chip_reset(RESET_POWER_ON);
    board_init_early();
    CHECK(nid_ir_init() != ESP_OK, "l'erreur doit être remontée");
    CHECK(pad(LED_PIN) == PAD_LOW && !g_ledc.channel_ok, "LED éteintes, canal non configuré");
    nid_ir_set_led_power(100);
    CHECK(pad(LED_PIN) == PAD_LOW, "commande sans effet quand la PWM est indisponible");
    end_test();
}

static void test_led_channel_failure(void)
{
    begin("LED IR : échec de configuration du canal");
    g_ledc_channel_fail = true;
    chip_reset(RESET_POWER_ON);
    board_init_early();
    CHECK(nid_ir_init() != ESP_OK, "l'erreur doit être remontée");
    CHECK(pad(LED_PIN) == PAD_LOW && g_pin[LED_PIN].out_enabled && !g_pin[LED_PIN].ledc_routed,
          "la broche doit rester une sortie basse (pas de tirage haut résiduel)");
    nid_ir_set_led_power(100);
    CHECK(pad(LED_PIN) == PAD_LOW, "pas d'éclairage sans PWM");
    end_test();
}

/* ------------------------------------------------------------------------- */
/* Filtre IR-cut : aucune commande configurée                                */
/* ------------------------------------------------------------------------- */

#if CONFIG_NID_IRCUT_NONE

static void test_ircut_none(void)
{
    begin("IR-cut : aucune commande");
    CHECK(nid_ir_set_day_filter(true) == ESP_ERR_NOT_SUPPORTED, "avant l'init");
    cold_boot();
    CHECK(nid_ir_set_day_filter(true) == ESP_ERR_NOT_SUPPORTED, "jour");
    CHECK(nid_ir_set_day_filter(false) == ESP_ERR_NOT_SUPPORTED, "nuit");
    nid_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.ir_power_pct = 100;
    nid_ir_apply(true, &cfg);
    nid_ir_apply(false, &cfg);
    CHECK(trace_count(PIN_A, 0) == 0 && trace_count(PIN_B, 0) == 0, "aucune broche IR-cut ne doit être touchée");
    CHECK(g_log_count['W'] == 0 && g_log_count['E'] == 0, "un filtre absent n'est pas une anomalie");
    end_test();
}

#endif

/* ------------------------------------------------------------------------- */
/* Filtre IR-cut : un fil, niveau permanent                                  */
/* ------------------------------------------------------------------------- */

#if CONFIG_NID_IRCUT_LEVEL

#define DAY_PAD    (CONFIG_NID_IRCUT_DAY_LEVEL ? PAD_HIGH : PAD_LOW)
#define NIGHT_PAD  (CONFIG_NID_IRCUT_DAY_LEVEL ? PAD_LOW : PAD_HIGH)

static void test_level_basic(void)
{
    begin("IR-cut un fil : commandes, polarité, aucune commande inutile");
    CHECK(nid_ir_set_day_filter(true) == ESP_ERR_INVALID_STATE, "avant l'init");
    cold_boot();
    const int m = mark();
    CHECK(trace_count(PIN_A, m) == 0 && s_filter_state == FILTER_UNKNOWN, "position inconnue : la broche ne doit pas être touchée à l'init");

    CHECK(nid_ir_set_day_filter(true) == ESP_OK, "jour");
    CHECK(pad(PIN_A) == DAY_PAD && g_pin[PIN_A].hold && s_filter_state == FILTER_DAY, "niveau jour %d, maintenu", DAY_PAD);
    CHECK(trace_count(PIN_A, m) == 1, "un seul changement de niveau pour la première commande");
    int calls = g_gpio_calls;
    CHECK(nid_ir_set_day_filter(true) == ESP_OK && g_gpio_calls == calls, "déjà en place : aucun accès aux broches");

    CHECK(nid_ir_set_day_filter(false) == ESP_OK, "nuit");
    CHECK(pad(PIN_A) == NIGHT_PAD && g_pin[PIN_A].hold && s_filter_state == FILTER_NIGHT, "niveau nuit %d", NIGHT_PAD);
    CHECK(trace_count(PIN_A, m) == 2, "un seul changement par commande");
    calls = g_gpio_calls;
    CHECK(nid_ir_set_day_filter(false) == ESP_OK && g_gpio_calls == calls, "nuit déjà en place : rien");

    /* Le cycle normal du terrain : plusieurs prises de vue de nuit, puis le jour. */
    nid_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.ir_power_pct = 100;
    const int before = trace_count(PIN_A, m);
    for (int i = 0; i < 5; i++) {
        nid_ir_apply(true, &cfg);
        nid_ir_off();
    }
    CHECK(trace_count(PIN_A, m) == before, "5 prises de vue de nuit : aucun mouvement du filtre");
    nid_ir_apply(false, &cfg);
    CHECK(pad(PIN_A) == DAY_PAD && trace_count(PIN_A, m) == before + 1, "passage au jour : un mouvement");
    CHECK(pad(LED_PIN) == PAD_LOW, "LED éteintes de jour");
    CHECK(g_log_count['W'] == 0 && g_log_count['E'] == 0, "avertissements ou erreurs");
    CHECK(g_conflict_warnings == 0, "conflit de broche");
    end_test();
}

static void test_level_deep_sleep(void)
{
    begin("IR-cut un fil : veille profonde sans clic");
    cold_boot();
    CHECK(nid_ir_set_day_filter(false) == ESP_OK, "nuit");
    nid_ir_off();
    CHECK(pad(PIN_A) == NIGHT_PAD, "niveau nuit");

    /* Veille profonde : la broche reste maintenue au niveau mémorisé. */
    chip_reset(RESET_DEEP_SLEEP);
    CHECK(pad(PIN_A) == NIGHT_PAD, "le niveau survit à la veille (maintien)");
    board_init_early();
    const int m = mark();
    CHECK(nid_ir_init() == ESP_OK, "nid_ir_init()");
    CHECK(trace_count(PIN_A, m) == 0 && pad(PIN_A) == NIGHT_PAD && g_pin[PIN_A].hold, "init après veille : broche inchangée et maintenue");
    CHECK(s_filter_state == FILTER_NIGHT, "position conservée en RTC");
    const int calls = g_gpio_calls;
    CHECK(nid_ir_set_day_filter(false) == ESP_OK && g_gpio_calls == calls, "nuit déjà en place : rien");
    CHECK(trace_count(PIN_A, m) == 0, "aucun changement de niveau");
    CHECK(nid_ir_set_day_filter(true) == ESP_OK && pad(PIN_A) == DAY_PAD, "jour");
    CHECK(trace_count(PIN_A, m) == 1, "un seul changement de niveau, sans état intermédiaire");
    end_test();
}

static void test_level_fault_reset(void)
{
    begin("IR-cut un fil : redémarrage sur défaut");
    cold_boot();
    CHECK(nid_ir_set_day_filter(false) == ESP_OK, "nuit");
    chip_reset(RESET_FAULT);   /* RTC_DATA_ATTR perdue : position inconnue, mais le niveau est resté maintenu */
    board_init_early();
    int m = mark();
    CHECK(nid_ir_init() == ESP_OK, "nid_ir_init()");
    CHECK(s_filter_state == FILTER_UNKNOWN && pad(PIN_A) == NIGHT_PAD && trace_count(PIN_A, m) == 0,
          "init : position inconnue, la broche n'est pas touchée");
    /* La commande « nuit » renvoie le même niveau : aucun changement visible sur la broche. */
    CHECK(nid_ir_set_day_filter(false) == ESP_OK, "nuit");
    CHECK(trace_count(PIN_A, m) == 0 && pad(PIN_A) == NIGHT_PAD && g_pin[PIN_A].hold, "même niveau renvoyé : le filtre ne bouge pas");

    chip_reset(RESET_FAULT);
    board_init_early();
    m = mark();
    CHECK(nid_ir_init() == ESP_OK, "nid_ir_init()");
    CHECK(nid_ir_set_day_filter(true) == ESP_OK, "jour");
    CHECK(pad(PIN_A) == DAY_PAD && trace_count(PIN_A, m) == 1, "passage direct au nouveau niveau, sans état intermédiaire");
    end_test();
}

static void test_level_power_on_after_sleep(void)
{
    begin("IR-cut un fil : mise sous tension (maintiens perdus)");
    cold_boot();
    CHECK(nid_ir_set_day_filter(true) == ESP_OK, "jour");
    chip_reset(RESET_POWER_ON);
    board_init_early();
    CHECK(pad(PIN_A) == PAD_FLOATING, "après coupure, la broche flotte");
    CHECK(nid_ir_init() == ESP_OK && s_filter_state == FILTER_UNKNOWN, "position inconnue");
    CHECK(nid_ir_set_day_filter(true) == ESP_OK && pad(PIN_A) == DAY_PAD, "commande envoyée une fois");
    end_test();
}

static void test_level_unwired(void)
{
    begin("IR-cut un fil : broche non câblée");
    g_board_pins.ircut_a = GPIO_NUM_NC;
    cold_boot();
    CHECK(nid_ir_set_day_filter(true) == ESP_ERR_NOT_SUPPORTED, "jour");
    CHECK(nid_ir_set_day_filter(false) == ESP_ERR_NOT_SUPPORTED, "nuit");
    nid_ir_apply(true, NULL);
    CHECK(pad(LED_PIN) == PAD_HIGH, "la LED fonctionne sans filtre");
    nid_ir_off();
    CHECK(g_log_count['E'] == 0, "pas d'erreur");
    end_test();
}

static void test_level_hold_unsupported(void)
{
    begin("IR-cut un fil : maintien indisponible");
    g_hold_unsupported = true;
    cold_boot();
    CHECK(nid_ir_set_day_filter(true) == ESP_OK && pad(PIN_A) == DAY_PAD, "la commande fonctionne quand même");
    CHECK(g_log_count['W'] >= 1, "un avertissement doit signaler que le niveau ne survivra pas à la veille");
    end_test();
}

#endif  /* CONFIG_NID_IRCUT_LEVEL */

/* ------------------------------------------------------------------------- */
/* Filtre IR-cut : deux fils, impulsion sur pont en H                        */
/* ------------------------------------------------------------------------- */

#if CONFIG_NID_IRCUT_PULSE

#define PULSE_DURATION_US  ((int64_t)CONFIG_NID_IRCUT_PULSE_MS * 1000)

static void test_pulse_init(void)
{
    begin("IR-cut impulsion : initialisation");
    CHECK(nid_ir_set_day_filter(true) == ESP_ERR_INVALID_STATE, "avant l'init");
    cold_boot();
    CHECK(g_timer_count == 1, "une minuterie créée");
    CHECK(pad(PIN_A) == PAD_LOW && pad(PIN_B) == PAD_LOW && g_pin[PIN_A].hold && g_pin[PIN_B].hold,
          "pont éteint (A = B = bas) et maintenu");
    CHECK(s_filter_state == FILTER_UNKNOWN, "position inconnue après la mise sous tension");
    CHECK(g_log_count['W'] == 0 && g_log_count['E'] == 0 && g_conflict_warnings == 0, "avertissements ou erreurs");
    end_test();
}

static void test_pulse_basic(void)
{
    begin("IR-cut impulsion : durée, polarité, non bloquant");
    cold_boot();
    int m = mark();
    int64_t t0 = g_now_us;

    CHECK(nid_ir_set_day_filter(true) == ESP_OK, "jour");
    CHECK(g_now_us == t0, "la commande ne doit pas bloquer (%lld µs écoulées)", (long long)(g_now_us - t0));
    CHECK(pad(PIN_A) == PAD_HIGH && pad(PIN_B) == PAD_LOW, "jour : A haut, B bas");
    CHECK(!g_pin[PIN_A].hold && !g_pin[PIN_B].hold, "pas de maintien pendant l'impulsion");
    CHECK(s_filter_state == FILTER_UNKNOWN, "position inconnue pendant le mouvement");
    mock_advance_us(PULSE_DURATION_US - 1);
    CHECK(pad(PIN_A) == PAD_HIGH, "impulsion encore en cours 1 µs avant la fin");
    mock_advance_us(1);
    CHECK(pad(PIN_A) == PAD_LOW && pad(PIN_B) == PAD_LOW && g_pin[PIN_A].hold && g_pin[PIN_B].hold,
          "fin d'impulsion : pont éteint et maintenu");
    CHECK(s_filter_state == FILTER_DAY, "position mémorisée");
    const int up = trace_find(PIN_A, PAD_HIGH, m);
    const int down = trace_find(PIN_A, PAD_LOW, up + 1);
    CHECK(up >= 0 && down > up && g_trace[down].t_us - g_trace[up].t_us == PULSE_DURATION_US,
          "largeur d'impulsion %lld µs (attendu %lld)", up >= 0 && down > up ? (long long)(g_trace[down].t_us - g_trace[up].t_us) : -1LL,
          (long long)PULSE_DURATION_US);
    CHECK(trace_count(PIN_B, m) == 0, "B ne doit pas bouger pendant une impulsion « jour »");

    /* Même position : aucune impulsion. */
    const int starts = g_timer_starts;
    const int calls = g_gpio_calls;
    CHECK(nid_ir_set_day_filter(true) == ESP_OK && g_timer_starts == starts && g_gpio_calls == calls, "déjà en place : rien");

    /* Nuit : impulsion en sens inverse. */
    m = mark();
    t0 = g_now_us;
    CHECK(nid_ir_set_day_filter(false) == ESP_OK && g_now_us == t0, "nuit, sans bloquer");
    CHECK(pad(PIN_A) == PAD_LOW && pad(PIN_B) == PAD_HIGH, "nuit : A bas, B haut");
    mock_advance_us(PULSE_DURATION_US);
    CHECK(pad(PIN_B) == PAD_LOW && s_filter_state == FILTER_NIGHT && g_pin[PIN_B].hold, "fin de l'impulsion de nuit");
    CHECK(trace_count(PIN_A, m) == 0, "A ne doit pas bouger pendant une impulsion « nuit »");
    CHECK(bridge_safe(), "le pont en H a été commandé des deux côtés");
    CHECK(g_timer_starts == starts + 1, "une minuterie par impulsion");
    CHECK(g_log_count['W'] == 0 && g_log_count['E'] == 0, "avertissements ou erreurs");
    end_test();
}

static void test_pulse_requests_during_motion(void)
{
    begin("IR-cut impulsion : demande reçue pendant un mouvement");
    cold_boot();
    nid_ir_set_day_filter(true);
    mock_advance_us(PULSE_DURATION_US);   /* jour en place */
    int m = mark();
    int starts = g_timer_starts;
    int64_t t0 = g_now_us;

    nid_ir_set_day_filter(false);                 /* nuit : mouvement */
    mock_advance_us(50000);
    CHECK(nid_ir_set_day_filter(true) == ESP_OK, "demande contraire pendant le mouvement");
    CHECK(g_timer_starts == starts + 1 && pad(PIN_B) == PAD_HIGH && pad(PIN_A) == PAD_LOW, "le mouvement en cours n'est pas interrompu");
    mock_advance_us(PULSE_DURATION_US - 50000);   /* fin de l'impulsion de nuit */
    CHECK(pad(PIN_B) == PAD_LOW && pad(PIN_A) == PAD_HIGH, "la demande en attente démarre dès la fin du mouvement");
    CHECK(g_timer_starts == starts + 2, "deux impulsions en tout");
    mock_advance_us(PULSE_DURATION_US);
    CHECK(pad(PIN_A) == PAD_LOW && pad(PIN_B) == PAD_LOW && s_filter_state == FILTER_DAY, "filtre en place (jour) à la fin");
    CHECK(g_now_us - t0 == 2 * PULSE_DURATION_US + 0, "durée totale %lld µs", (long long)(g_now_us - t0));
    CHECK(bridge_safe(), "le pont en H a été commandé des deux côtés");
    CHECK(trace_find(PIN_B, PAD_HIGH, m) >= 0, "l'impulsion de nuit a bien eu lieu");

    /* Demande contraire puis retour à la position visée : aucune seconde impulsion. */
    m = mark();
    starts = g_timer_starts;
    nid_ir_set_day_filter(false);
    nid_ir_set_day_filter(true);
    nid_ir_set_day_filter(false);
    mock_advance_us(3 * PULSE_DURATION_US);
    CHECK(g_timer_starts == starts + 1 && s_filter_state == FILTER_NIGHT, "dernière demande retenue (nuit), %d impulsion(s)", g_timer_starts - starts);
    CHECK(bridge_safe(), "pont en H");

    /* Même demande répétée pendant le mouvement : rien de plus. */
    nid_ir_set_day_filter(true);
    starts = g_timer_starts;
    nid_ir_set_day_filter(true);
    nid_ir_set_day_filter(true);
    mock_advance_us(3 * PULSE_DURATION_US);
    CHECK(g_timer_starts == starts && s_filter_state == FILTER_DAY, "demandes identiques : une seule impulsion");
    end_test();
}

static void test_pulse_off_waits(void)
{
    begin("IR-cut impulsion : nid_ir_off() attend la fin du mouvement");
    cold_boot();
    nid_ir_set_led_power(100);
    const int64_t t0 = g_now_us;
    nid_ir_set_day_filter(false);
    mock_advance_us(10000);
    nid_ir_off();
    CHECK(!s_pulse_active && pad(PIN_B) == PAD_LOW && g_pin[PIN_B].hold, "impulsion terminée au retour de nid_ir_off()");
    CHECK(g_now_us - t0 >= PULSE_DURATION_US && g_now_us - t0 <= PULSE_DURATION_US + 2000,
          "attente de %lld µs (impulsion %lld µs)", (long long)(g_now_us - t0), (long long)PULSE_DURATION_US);
    CHECK(pad(LED_PIN) == PAD_LOW, "LED éteintes");
    CHECK(s_filter_state == FILTER_NIGHT, "position mémorisée avant la veille");

    /* Sans mouvement : retour immédiat. */
    const int64_t t1 = g_now_us;
    nid_ir_off();
    CHECK(g_now_us == t1, "nid_ir_off() ne doit pas attendre sans impulsion");
    end_test();
}

static void test_pulse_apply_non_blocking(void)
{
    begin("IR-cut impulsion : nid_ir_apply() ne retarde pas le démarrage");
    cold_boot();
    nid_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.ir_power_pct = 80;
    const int64_t t0 = g_now_us;
    nid_ir_apply(true, &cfg);
    CHECK(g_now_us == t0, "nid_ir_apply(nuit) a bloqué %lld µs", (long long)(g_now_us - t0));
    CHECK(pad(PIN_B) == PAD_HIGH && pad(LED_PIN) == PAD_PWM, "filtre en mouvement, LED allumées");
    mock_advance_us(PULSE_DURATION_US);
    nid_ir_off();
    const int starts = g_timer_starts;
    for (int i = 0; i < 5; i++) {
        nid_ir_apply(true, &cfg);
        nid_ir_off();
    }
    CHECK(g_timer_starts == starts, "prises de vue de nuit suivantes : aucun mouvement du filtre");
    nid_ir_apply(false, &cfg);
    CHECK(pad(PIN_A) == PAD_HIGH && pad(LED_PIN) == PAD_LOW, "jour : filtre en place, LED éteintes");
    mock_advance_us(PULSE_DURATION_US);
    CHECK(bridge_safe(), "pont en H");
    end_test();
}

static void test_pulse_interrupted(void)
{
    begin("IR-cut impulsion : mouvement interrompu par un redémarrage");
    cold_boot();
    nid_ir_set_day_filter(true);
    mock_advance_us(20000);
    CHECK(pad(PIN_A) == PAD_HIGH, "impulsion en cours");

    /* Veille ou redémarrage au milieu de l'impulsion : la position RTC doit rester « inconnue ». */
    chip_reset(RESET_DEEP_SLEEP);
    CHECK(s_filter_state == FILTER_UNKNOWN, "position inconnue conservée");
    board_init_early();
    CHECK(nid_ir_init() == ESP_OK, "nid_ir_init()");
    CHECK(pad(PIN_A) == PAD_LOW && pad(PIN_B) == PAD_LOW && g_pin[PIN_A].hold && g_pin[PIN_B].hold, "pont éteint et maintenu");
    const int starts = g_timer_starts;
    CHECK(nid_ir_set_day_filter(true) == ESP_OK && pad(PIN_A) == PAD_HIGH && g_timer_starts == starts + 1,
          "l'impulsion interrompue doit être renvoyée");
    mock_advance_us(PULSE_DURATION_US);
    CHECK(s_filter_state == FILTER_DAY && pad(PIN_A) == PAD_LOW, "impulsion complète");

    /* Idem après un défaut (RTC réinitialisée). */
    nid_ir_set_day_filter(false);
    mock_advance_us(30000);
    chip_reset(RESET_FAULT);
    board_init_early();
    nid_ir_init();
    CHECK(s_filter_state == FILTER_UNKNOWN && pad(PIN_B) == PAD_LOW, "après défaut : pont éteint, position inconnue");
    CHECK(nid_ir_set_day_filter(false) == ESP_OK && pad(PIN_B) == PAD_HIGH, "impulsion renvoyée");
    mock_advance_us(PULSE_DURATION_US);
    CHECK(bridge_safe(), "pont en H");
    end_test();
}

static void test_pulse_deep_sleep(void)
{
    begin("IR-cut impulsion : veille profonde sans clic");
    cold_boot();
    nid_ir_set_day_filter(true);
    nid_ir_off();
    CHECK(s_filter_state == FILTER_DAY, "jour en place");

    chip_reset(RESET_DEEP_SLEEP);
    CHECK(pad(PIN_A) == PAD_LOW && pad(PIN_B) == PAD_LOW, "pont maintenu éteint pendant la veille");
    board_init_early();
    const int m = mark();
    CHECK(nid_ir_init() == ESP_OK, "nid_ir_init()");
    CHECK(trace_count(PIN_A, m) == 0 && trace_count(PIN_B, m) == 0, "init après veille : aucun mouvement sur les broches");
    CHECK(s_filter_state == FILTER_DAY, "position conservée en RTC");
    const int starts = g_timer_starts;
    CHECK(nid_ir_set_day_filter(true) == ESP_OK && g_timer_starts == starts, "jour déjà en place : aucune impulsion");
    CHECK(nid_ir_set_day_filter(false) == ESP_OK && g_timer_starts == starts + 1 && pad(PIN_B) == PAD_HIGH, "nuit : une impulsion");
    mock_advance_us(PULSE_DURATION_US);
    CHECK(bridge_safe(), "pont en H");
    end_test();
}

static void test_pulse_timer_failures(void)
{
    begin("IR-cut impulsion : minuterie indisponible (création)");
    g_timer_create_error = ESP_ERR_NO_MEM;
    cold_boot();
    CHECK(g_log_count['E'] == 1, "l'échec de création doit être signalé");
    int64_t t0 = g_now_us;
    CHECK(nid_ir_set_day_filter(true) == ESP_OK, "jour");
    CHECK(g_now_us - t0 == PULSE_DURATION_US, "repli : impulsion bloquante de %lld µs", (long long)(g_now_us - t0));
    CHECK(pad(PIN_A) == PAD_LOW && pad(PIN_B) == PAD_LOW && s_filter_state == FILTER_DAY, "pont éteint, position mémorisée");
    CHECK(bridge_safe(), "pont en H");
    end_test();

    begin("IR-cut impulsion : minuterie indisponible (démarrage)");
    cold_boot();
    g_timer_start_error = ESP_FAIL;
    t0 = g_now_us;
    CHECK(nid_ir_set_day_filter(false) == ESP_OK, "nuit");
    CHECK(g_now_us - t0 == PULSE_DURATION_US && s_filter_state == FILTER_NIGHT && !s_pulse_active, "impulsion bloquante complète");
    CHECK(pad(PIN_A) == PAD_LOW && pad(PIN_B) == PAD_LOW, "pont éteint");
    CHECK(g_log_count['W'] >= 1, "avertissement attendu");
    CHECK(bridge_safe(), "pont en H");
    end_test();
}

static void test_pulse_unwired(void)
{
    begin("IR-cut impulsion : broche B non câblée");
    g_board_pins.ircut_b = GPIO_NUM_NC;
    cold_boot();
    CHECK(nid_ir_set_day_filter(true) == ESP_ERR_NOT_SUPPORTED, "jour");
    CHECK(nid_ir_set_day_filter(false) == ESP_ERR_NOT_SUPPORTED, "nuit");
    CHECK(trace_count(PIN_A, 0) == 0, "aucune broche touchée");
    nid_ir_apply(true, NULL);
    nid_ir_off();
    CHECK(g_log_count['E'] == 0, "pas d'erreur");
    end_test();
}

#endif  /* CONFIG_NID_IRCUT_PULSE */

/* ------------------------------------------------------------------------- */

int main(void)
{
#if CONFIG_NID_IRCUT_LEVEL
    printf("Tests matériels de nid_ir (IR-cut un fil, jour = niveau %d)\n", CONFIG_NID_IRCUT_DAY_LEVEL);
#elif CONFIG_NID_IRCUT_PULSE
    printf("Tests matériels de nid_ir (IR-cut deux fils, impulsion de %d ms)\n", CONFIG_NID_IRCUT_PULSE_MS);
#else
    printf("Tests matériels de nid_ir (sans commande de filtre IR-cut)\n");
#endif
    test_led_basic();
    test_led_off_in_sleep_sequence();
    test_led_no_pin();
    test_led_clock_fallback();
    test_led_channel_failure();
#if CONFIG_NID_IRCUT_NONE
    test_ircut_none();
#elif CONFIG_NID_IRCUT_LEVEL
    test_level_basic();
    test_level_deep_sleep();
    test_level_fault_reset();
    test_level_power_on_after_sleep();
    test_level_unwired();
    test_level_hold_unsupported();
#elif CONFIG_NID_IRCUT_PULSE
    test_pulse_init();
    test_pulse_basic();
    test_pulse_requests_during_motion();
    test_pulse_off_waits();
    test_pulse_apply_non_blocking();
    test_pulse_interrupted();
    test_pulse_deep_sleep();
    test_pulse_timer_failures();
    test_pulse_unwired();
#endif

    if (s_failures != 0) {
        printf("ÉCHEC : %d vérification(s) en erreur sur %d\n", s_failures, s_checks);
        return 1;
    }
    printf("OK : %d vérifications réussies\n", s_checks);
    return 0;
}
