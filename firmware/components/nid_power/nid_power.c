/*
 * nid_power — énergie : réveil, veille, batterie, chien de garde
 *
 * Veille profonde (ESP-IDF v6.1, ESP32-P4 rév. ≥ 3.0) :
 *   - réveil EXT1 par niveau, un niveau par broche (SOC_PM_SUPPORT_EXT1_WAKEUP_MODE_PER_PIN) :
 *     esp_sleep_enable_ext1_wakeup_io() appelée une fois par niveau (PIR actif haut,
 *     bouton actif bas) ; esp_sleep_enable_ext1_wakeup_with_level_mask() est dépréciée ;
 *   - les broches de réveil passent en E/S basse consommation (LP IO) avec leur tirage ;
 *     le domaine RTC_PERIPH reste coupé (EXT1 est géré par le PMU) et ESP-IDF maintient
 *     ces broches (hold) pendant la veille, tirages compris ; il libère ce maintien
 *     au réveil, nid_detect_init() les rend ensuite au GPIO numérique ;
 *   - ESP-IDF coupe la flash (VO1), la PSRAM (VO2), le domaine numérique et le
 *     quartz 40 MHz ; le quartz 32,768 kHz reste actif (minuterie précise) ;
 *   - ce module coupe en plus les LDO internes VO3 (PHY MIPI) et VO4 (domaine d'E/S
 *     de la carte SD), que l'ESP-IDF laisse dans leur état, et nid_board maintient
 *     les sorties au repos (LED IR, voyant, ampli, rétroéclairage, C6 en reset).
 *
 * Architecture « coupure matérielle » : chaque mise sous tension est un POWERON ;
 * la cause est déduite du drapeau d'alarme du DS3231 et des niveaux du PIR et du
 * bouton. En fin de cycle, l'alarme du DS3231 est programmée puis OFF est commandé ;
 * si l'alimentation reste présente (USB au banc), repli sur la veille profonde.
 */
#include "nid_power.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_bit_defs.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/ldo_ll.h"
#include "soc/soc_caps.h"
#include "nid_board.h"
#include "nid_clock.h"
#include "nid_detect.h"

static const char *TAG = "nid_power";

#define US_PER_S                 1000000ULL
#define MIN_SLEEP_US             (100ULL * 1000)  /* minuterie : durée minimale */
#define STUCK_BUTTON_RETRY_S     3600             /* bouton bloqué et aucune autre source de réveil */
#define BUTTON_STABLE_MS         50               /* bouton relâché de façon stable */
#define BUTTON_POLL_MS           10
#define BATT_LOCK_TIMEOUT_MS     500
#define ADC_UNCAL_FULL_SCALE_MV  4400             /* Vref 1100 mV / k 0,25 (atténuation 12 dB), sans étalonnage */
#define LDO_RAIL_MV              3300
#define SD_IO_DOMAIN_FIRST_GPIO  39               /* emplacement SD 0 : D0-D3 39-42, CLK 43, CMD 44, D4-D7 45-48 */
#define SD_IO_DOMAIN_LAST_GPIO   48
#define FIRST_FREE_LDO_CHAN      3                /* VO1 (flash) et VO2 (PSRAM) ne sont jamais touchés */

static TickType_t ticks_ms(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    return t > 0 ? t : 1;
}

/* ------------------------------------------------------------------------- */
/* Cause du démarrage                                                        */
/* ------------------------------------------------------------------------- */

static const char *reset_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:
        return "mise sous tension";
    case ESP_RST_EXT:
        return "broche de reset";
    case ESP_RST_SW:
        return "redémarrage logiciel";
    case ESP_RST_PANIC:
        return "panique";
    case ESP_RST_INT_WDT:
        return "chien de garde des interruptions";
    case ESP_RST_TASK_WDT:
        return "chien de garde des tâches";
    case ESP_RST_WDT:
        return "chien de garde matériel";
    case ESP_RST_DEEPSLEEP:
        return "réveil de veille profonde";
    case ESP_RST_BROWNOUT:
        return "chute de tension";
    case ESP_RST_SDIO:
        return "SDIO";
    case ESP_RST_USB:
        return "USB (programmation, moniteur)";
    case ESP_RST_JTAG:
        return "JTAG";
    case ESP_RST_EFUSE:
        return "erreur eFuse";
    case ESP_RST_PWR_GLITCH:
        return "perturbation de l'alimentation";
    case ESP_RST_CPU_LOCKUP:
        return "blocage du processeur";
    default:
        return "inconnue";
    }
}

/*
 * Redémarrage non voulu par un opérateur : reprise de la surveillance sans
 * installation (FC3.3). esp_restart() n'est appelé que par nid_power_restart()
 * après une tâche bloquée, d'où ESP_RST_SW classé en défaut.
 */
static bool is_fault_reset(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_SW:
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
    case ESP_RST_CPU_LOCKUP:
    case ESP_RST_PWR_GLITCH:
    case ESP_RST_EFUSE:
    case ESP_RST_UNKNOWN:
        return true;
    default:
        return false;  /* mise sous tension, bouton RESET (POWERON sur l'ESP32-P4), USB, JTAG */
    }
}

static nid_wake_reason_t wake_from_deep_sleep(uint32_t causes, uint64_t ext1)
{
    const nid_board_pins_t *pins = nid_board_pins();
    if (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) {
        /* Le bouton l'emporte : l'opérateur attend le mode installation. */
        if (pins->button != GPIO_NUM_NC && (ext1 & (1ULL << pins->button))) {
            return NID_WAKE_BUTTON;
        }
        if (pins->pir != GPIO_NUM_NC && (ext1 & (1ULL << pins->pir))) {
            return NID_WAKE_PIR;
        }
        /* Statut EXT1 vide (ne devrait pas arriver) : niveaux actuels. */
        return nid_detect_button_pressed() ? NID_WAKE_BUTTON : NID_WAKE_PIR;
    }
    if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
        return NID_WAKE_TIMER;
    }
    ESP_LOGW(TAG, "réveil de veille sans cause connue (0x%" PRIx32 ") : traité comme la minuterie", causes);
    return NID_WAKE_TIMER;
}

#if CONFIG_NID_POWER_LATCH
typedef struct {
    bool alarm;   /* drapeau d'alarme 1 du DS3231 */
    bool pir;     /* sortie PIR active */
    bool button;  /* bouton appuyé */
} latch_sources_t;

static latch_sources_t latch_sources_read(void)
{
    latch_sources_t s = { 0 };
    /* Lu puis effacé à CHAQUE démarrage : libère aussi la sortie INT du DS3231,
     * qui commande l'interrupteur d'alimentation. Suppose nid_clock_init() fait
     * (bus I2C) et que nid_clock_init() n'efface pas lui-même ce drapeau. */
    esp_err_t err = nid_clock_clear_alarm(&s.alarm);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "drapeau d'alarme du DS3231 illisible : %s", esp_err_to_name(err));
        }
        s.alarm = false;
    }
    s.pir = nid_detect_pir_active();
    s.button = nid_detect_button_pressed();
    return s;
}
#endif

void nid_power_get_boot_info(nid_boot_info_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->reset_reason = esp_reset_reason();
    out->wake_causes = esp_sleep_get_wakeup_causes();
    out->ext1_status = esp_sleep_get_ext1_wakeup_status();

    char detail[112];
    if (out->reset_reason == ESP_RST_DEEPSLEEP) {
        out->reason = wake_from_deep_sleep(out->wake_causes, out->ext1_status);
        snprintf(detail, sizeof(detail), "réveil de veille profonde, causes 0x%" PRIx32 ", EXT1 0x%" PRIx64,
                 out->wake_causes, out->ext1_status);
    } else if (is_fault_reset(out->reset_reason)) {
        out->reason = NID_WAKE_FAULT;
        snprintf(detail, sizeof(detail), "reset : %s", reset_str(out->reset_reason));
    } else {
        out->reason = NID_WAKE_POWER_ON;
        snprintf(detail, sizeof(detail), "reset : %s", reset_str(out->reset_reason));
    }

#if CONFIG_NID_POWER_LATCH
    const latch_sources_t src = latch_sources_read();
    if (out->reset_reason == ESP_RST_POWERON) {
        /* Interrupteur rallumé : bouton, puis PIR (une détection ne doit pas être
         * perdue ; la photo due sera prise après), puis alarme, sinon vraie mise
         * sous tension. */
        if (src.button) {
            out->reason = NID_WAKE_BUTTON;
        } else if (src.pir) {
            out->reason = NID_WAKE_PIR;
        } else if (src.alarm) {
            out->reason = NID_WAKE_RTC_ALARM;
        } else {
            out->reason = NID_WAKE_POWER_ON;
        }
    }
    ESP_LOGI(TAG, "démarrage : %s (%s ; coupure matérielle : alarme DS3231 %s, PIR %s, bouton %s)",
             nid_power_wake_str(out->reason), detail, src.alarm ? "oui" : "non",
             src.pir ? "actif" : "au repos", src.button ? "appuyé" : "relâché");
#else
    ESP_LOGI(TAG, "démarrage : %s (%s)", nid_power_wake_str(out->reason), detail);
#endif
}

const char *nid_power_wake_str(nid_wake_reason_t reason)
{
    switch (reason) {
    case NID_WAKE_POWER_ON:
        return "mise sous tension";
    case NID_WAKE_PIR:
        return "PIR";
    case NID_WAKE_BUTTON:
        return "bouton";
    case NID_WAKE_TIMER:
        return "minuterie";
    case NID_WAKE_RTC_ALARM:
        return "alarme horloge";
    case NID_WAKE_FAULT:
        return "reprise après défaut";
    default:
        return "?";
    }
}

/* ------------------------------------------------------------------------- */
/* Batterie                                                                  */
/* ------------------------------------------------------------------------- */

static SemaphoreHandle_t s_adc_lock;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static adc_unit_t s_adc_unit;
static adc_channel_t s_adc_chan;
static volatile bool s_batt_ready;

static esp_err_t battery_cali_create(void)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cfg = {
        .unit_id = s_adc_unit,
        .chan = s_adc_chan,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    return adc_cali_create_scheme_curve_fitting(&cfg, &s_adc_cali);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    const adc_cali_line_fitting_config_t cfg = {
        .unit_id = s_adc_unit,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    return adc_cali_create_scheme_line_fitting(&cfg, &s_adc_cali);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t nid_power_battery_init(void)
{
    const gpio_num_t pin = nid_board_pins()->batt_adc;
    if (pin == GPIO_NUM_NC) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_batt_ready) {
        return ESP_OK;
    }
    if (s_adc_lock == NULL) {
        s_adc_lock = xSemaphoreCreateMutex();
        if (s_adc_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    esp_err_t err = adc_oneshot_io_to_channel(pin, &s_adc_unit, &s_adc_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d n'est pas une entrée ADC : mesure de la batterie impossible", pin);
        return err;
    }
    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = s_adc_unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC%d indisponible : %s", (int)s_adc_unit + 1, esp_err_to_name(err));
        return err;
    }
    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc, s_adc_chan, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configuration de l'ADC : %s", esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return err;
    }
    if (battery_cali_create() != ESP_OK) {
        s_adc_cali = NULL;
        ESP_LOGW(TAG, "ADC non étalonné : mesure de la batterie approximative (±10 %%)");
    }
    s_batt_ready = true;
    ESP_LOGD(TAG, "batterie : GPIO%d (ADC%d, canal %d), rapport %d/1000",
             pin, (int)s_adc_unit + 1, (int)s_adc_chan, CONFIG_NID_BATT_DIVIDER_X1000);
    return ESP_OK;
}

static void sort_ints(int *v, int n)
{
    for (int i = 1; i < n; i++) {
        int x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = x;
    }
}

esp_err_t nid_power_battery_read_mv(uint32_t *mv)
{
    if (mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *mv = 0;
    if (nid_board_pins()->batt_adc == GPIO_NUM_NC) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_batt_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Deux tâches peuvent mesurer en même temps (préparation au réveil, arrêt de
     * l'enregistrement) : adc_oneshot_read() échouerait pour l'une d'elles. */
    if (xSemaphoreTake(s_adc_lock, pdMS_TO_TICKS(BATT_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    int raw[CONFIG_NID_BATT_SAMPLES];
    int n = 0;
    for (int i = 0; i < CONFIG_NID_BATT_SAMPLES; i++) {
        if (adc_oneshot_read(s_adc, s_adc_chan, &raw[n]) == ESP_OK) {
            n++;
        }
    }
    xSemaphoreGive(s_adc_lock);
    if (n == 0) {
        return ESP_FAIL;
    }

    /* Moyenne tronquée : écarte les 1/8 extrêmes (creux dus aux appels de courant). */
    sort_ints(raw, n);
    const int drop = n / 8;
    int32_t sum = 0;
    for (int i = drop; i < n - drop; i++) {
        sum += raw[i];
    }
    const int count = n - 2 * drop;
    const int avg = (int)((sum + count / 2) / count);

    int adc_mv = 0;
    if (s_adc_cali == NULL || adc_cali_raw_to_voltage(s_adc_cali, avg, &adc_mv) != ESP_OK) {
        adc_mv = (int)(((int64_t)avg * ADC_UNCAL_FULL_SCALE_MV) / 4095);
    }
    if (adc_mv < 0) {
        adc_mv = 0;
    }
    *mv = (uint32_t)(((uint64_t)adc_mv * CONFIG_NID_BATT_DIVIDER_X1000 + 500) / 1000);
    return ESP_OK;
}

nid_batt_level_t nid_power_battery_level(uint32_t mv, uint32_t low_mv, uint32_t critical_mv)
{
    if (mv == 0) {
        return NID_BATT_UNKNOWN;
    }
    if (mv < critical_mv) {
        return NID_BATT_CRITICAL;
    }
    if (mv < low_mv) {
        return NID_BATT_LOW;
    }
    return NID_BATT_OK;
}

/* ------------------------------------------------------------------------- */
/* Chien de garde des tâches (FC3.3)                                         */
/* ------------------------------------------------------------------------- */

#if CONFIG_ESP_TASK_WDT_EN

/* Avec ESP_TASK_WDT_INIT, ESP-IDF initialise le chien de garde au démarrage. */
#if CONFIG_ESP_TASK_WDT_INIT
static volatile bool s_wdt_ready = true;
#else
static volatile bool s_wdt_ready = false;
#endif

static uint32_t wdt_idle_core_mask(void)
{
    uint32_t mask = 0;
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    mask |= BIT(0);
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1 && !CONFIG_FREERTOS_UNICORE
    mask |= BIT(1);
#endif
    return mask;
}

esp_err_t nid_power_watchdog_start(uint32_t timeout_s)
{
    if (timeout_s == 0) {
        timeout_s = 1;
    }
    const esp_task_wdt_config_t cfg = {
        .timeout_ms = timeout_s * 1000U,
        .idle_core_mask = wdt_idle_core_mask(),
        .trigger_panic = true,  /* panique → redémarrage → reprise (FC3.3) */
    };
    esp_err_t err = s_wdt_ready ? esp_task_wdt_reconfigure(&cfg) : esp_task_wdt_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "chien de garde : configuration impossible (%s)", esp_err_to_name(err));
        return err;
    }
    s_wdt_ready = true;
    err = nid_power_watchdog_add_current_task();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "chien de garde : %" PRIu32 " s, redémarrage en cas de blocage", timeout_s);
    }
    return err;
}

esp_err_t nid_power_watchdog_add_current_task(void)
{
    if (!s_wdt_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        return ESP_OK;  /* déjà abonnée */
    }
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "abonnement au chien de garde impossible : %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t nid_power_watchdog_remove_current_task(void)
{
    if (!s_wdt_ready || esp_task_wdt_status(NULL) != ESP_OK) {
        return ESP_OK;  /* pas abonnée */
    }
    return esp_task_wdt_delete(NULL);
}

void nid_power_watchdog_feed(void)
{
    /* Sans effet (et sans message d'erreur) pour une tâche non abonnée. */
    if (s_wdt_ready && esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
}

#else  /* !CONFIG_ESP_TASK_WDT_EN */

esp_err_t nid_power_watchdog_start(uint32_t timeout_s)
{
    (void)timeout_s;
    ESP_LOGW(TAG, "chien de garde des tâches désactivé dans la configuration (ESP_TASK_WDT_EN)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nid_power_watchdog_add_current_task(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nid_power_watchdog_remove_current_task(void)
{
    return ESP_OK;
}

void nid_power_watchdog_feed(void)
{
}

#endif  /* CONFIG_ESP_TASK_WDT_EN */

/* ------------------------------------------------------------------------- */
/* Veille                                                                    */
/* ------------------------------------------------------------------------- */

/* Attend que le bouton soit relâché de façon stable. @return false s'il reste appuyé. */
static bool wait_button_released(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    uint32_t released = 0;
    while (released < BUTTON_STABLE_MS) {
        if (nid_detect_button_pressed()) {
            released = 0;
            if (waited >= timeout_ms) {
                return false;
            }
        } else {
            released += BUTTON_POLL_MS;
        }
        vTaskDelay(ticks_ms(BUTTON_POLL_MS));
        waited += BUTTON_POLL_MS;
        if (waited % 1000 == 0) {
            nid_power_watchdog_feed();
        }
    }
    return true;
}

/* Entrée de réveil : E/S basse consommation, entrée seule, tirage vers le niveau de repos. */
static void sleep_wake_input(gpio_num_t pin, bool pull_up)
{
    rtc_gpio_hold_dis(pin);
    rtc_gpio_init(pin);
    rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
    if (pull_up) {
        rtc_gpio_pulldown_dis(pin);
        rtc_gpio_pullup_en(pin);
    } else {
        rtc_gpio_pullup_dis(pin);
        rtc_gpio_pulldown_en(pin);
    }
}

/* Entrée non utilisée pour ce réveil : isolée (ni entrée, ni sortie, ni tirage) et maintenue. */
static void sleep_isolate_input(gpio_num_t pin)
{
    rtc_gpio_hold_dis(pin);
    rtc_gpio_init(pin);
    rtc_gpio_isolate(pin);
}

#if CONFIG_NID_SLEEP_LDO_OFF
/* Coupe un LDO interne sans passer par son pilote (le système va dormir puis redémarrer). */
static void ldo_force_off(int chan)
{
    if (chan < FIRST_FREE_LDO_CHAN || chan > SOC_GP_LDO_NUM_UNITS) {
        return;
    }
#if CONFIG_ESP_LDO_RESERVE_SPI_NOR_FLASH
    if (chan == CONFIG_ESP_LDO_CHAN_SPI_NOR_FLASH_DOMAIN) {
        return;
    }
#endif
#if CONFIG_ESP_LDO_RESERVE_PSRAM
    if (chan == CONFIG_ESP_LDO_CHAN_PSRAM_DOMAIN) {
        return;
    }
#endif
    ldo_ll_enable(LDO_ID2UNIT(chan), false);
}
#endif

static void enter_deep_sleep(uint64_t sleep_us, bool wake_pir, bool wake_button) __attribute__((noreturn));
static void enter_deep_sleep(uint64_t sleep_us, bool wake_pir, bool wake_button)
{
    const nid_board_pins_t *pins = nid_board_pins();
    const gpio_num_t pir = pins->pir;
    const gpio_num_t button = (pins->button != pins->pir) ? pins->button : GPIO_NUM_NC;

    /* Réveil sur niveau bas : un bouton encore appuyé réveillerait aussitôt. */
    if (wake_button && button != GPIO_NUM_NC &&
            !wait_button_released(CONFIG_NID_BUTTON_RELEASE_WAIT_MS)) {
        ESP_LOGW(TAG, "bouton toujours appuyé : il ne réveillera pas le système cette fois");
        wake_button = false;
        if (sleep_us == 0 && !(wake_pir && pir != GPIO_NUM_NC)) {
            sleep_us = (uint64_t)STUCK_BUTTON_RETRY_S * US_PER_S;
        }
    }

    /* Sources de réveil : tout est redéfini (aussi les masques EXT1, non effacés par ALL). */
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_disable_ext1_wakeup_io(0);

    bool use_pir = false;
    if (pir != GPIO_NUM_NC) {
        if (wake_pir) {
            sleep_wake_input(pir, !pins->pir_active_high);
            const esp_sleep_ext1_wakeup_mode_t mode =
                pins->pir_active_high ? ESP_EXT1_WAKEUP_ANY_HIGH : ESP_EXT1_WAKEUP_ANY_LOW;
            esp_err_t err = esp_sleep_enable_ext1_wakeup_io(1ULL << pir, mode);
            use_pir = (err == ESP_OK);
            if (!use_pir) {
                ESP_LOGE(TAG, "réveil par le PIR (GPIO%d) impossible : %s", pir, esp_err_to_name(err));
            }
        }
        if (!use_pir) {
            sleep_isolate_input(pir);  /* le PIR ne doit pas réveiller (délai, chauffe, arrêt) */
        }
    }
    bool use_button = false;
    if (button != GPIO_NUM_NC) {
        if (wake_button) {
            sleep_wake_input(button, true);  /* tirage haut maintenu pendant la veille */
            esp_err_t err = esp_sleep_enable_ext1_wakeup_io(1ULL << button, ESP_EXT1_WAKEUP_ANY_LOW);
            use_button = (err == ESP_OK);
            if (!use_button) {
                ESP_LOGE(TAG, "réveil par le bouton (GPIO%d) impossible : %s", button, esp_err_to_name(err));
            }
        }
        if (!use_button) {
            sleep_isolate_input(button);
        }
    }
    if (sleep_us > 0) {
        if (sleep_us < MIN_SLEEP_US) {
            sleep_us = MIN_SLEEP_US;
        }
        esp_err_t err = esp_sleep_enable_timer_wakeup(sleep_us);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "minuterie de réveil impossible : %s", esp_err_to_name(err));
            sleep_us = 0;
        }
    }

    if (!use_pir && !use_button && sleep_us == 0) {
        ESP_LOGW(TAG, "aucune source de réveil : arrêt jusqu'à la prochaine mise sous tension");
    }
    ESP_LOGI(TAG, "veille profonde : PIR %s, bouton %s, minuterie %" PRIu64 " ms",
             use_pir ? "oui" : "non", use_button ? "oui" : "non", sleep_us / 1000ULL);

    /* Sorties au repos maintenues (LED IR, voyant, ampli, rétroéclairage, C6 en reset). */
    nid_board_prepare_deep_sleep();
#if CONFIG_NID_SLEEP_LDO_OFF
    /* LDO laissés actifs par un pilote : PHY MIPI (VO3), domaine d'E/S de la carte SD (VO4). */
    ldo_force_off(CONFIG_NID_MIPI_LDO_CHAN);
    ldo_force_off(CONFIG_NID_SD_LDO_CHAN);
#endif
#if CONFIG_NID_SLEEP_SILENT_ROM
    esp_deep_sleep_disable_rom_logging();
#endif
    esp_deep_sleep_start();
}

#if CONFIG_NID_POWER_LATCH
static bool in_sd_io_domain(gpio_num_t pin)
{
    return pin >= SD_IO_DOMAIN_FIRST_GPIO && pin <= SD_IO_DOMAIN_LAST_GPIO;
}

/* Attente qui nourrit le chien de garde (tâche abonnée). */
static void wait_ms_fed(uint32_t ms)
{
    while (ms > 0) {
        const uint32_t step = ms < 100 ? ms : 100;
        vTaskDelay(ticks_ms(step));
        ms -= step;
        nid_power_watchdog_feed();
    }
}

/*
 * Coupe l'alimentation par l'interrupteur (OFF au niveau haut). Ne revient que
 * si la coupure n'a pas eu lieu, ou n'est pas possible : l'appelant passe alors
 * en veille profonde avec le même plan.
 */
static void latch_power_off(const nid_sleep_plan_t *plan)
{
    const gpio_num_t off = nid_board_pins()->power_off;
    if (off == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "broche OFF non configurée : veille profonde à la place de la coupure");
        return;
    }
    if (plan->sleep_us > 0) {
        /* Le réveil programmé repose sur l'alarme du DS3231, réglée en heure UTC. */
        if (!nid_clock_time_is_valid()) {
            ESP_LOGW(TAG, "heure non valide : alarme du DS3231 impossible, veille profonde");
            return;
        }
        const uint64_t secs = (plan->sleep_us + US_PER_S - 1) / US_PER_S;
        esp_err_t err = nid_clock_set_alarm(time(NULL) + (time_t)secs);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "alarme du DS3231 impossible (%s) : veille profonde", esp_err_to_name(err));
            return;
        }
    }

    /* OFF dans le domaine d'E/S de la carte SD (GPIO 39-48) : ce domaine doit être
     * alimenté par le LDO VO4. Échec = canal déjà utilisé, donc déjà actif. */
    esp_ldo_channel_handle_t ldo = NULL;
    if (in_sd_io_domain(off) && CONFIG_NID_SD_LDO_CHAN >= FIRST_FREE_LDO_CHAN) {
        const esp_ldo_channel_config_t cfg = {
            .chan_id = CONFIG_NID_SD_LDO_CHAN,
            .voltage_mv = LDO_RAIL_MV,
        };
        if (esp_ldo_acquire_channel(&cfg, &ldo) != ESP_OK) {
            ldo = NULL;
        }
    }

    ESP_LOGI(TAG, "coupure de l'alimentation (OFF, GPIO%d) ; rallumage par le PIR, le bouton%s",
             off, plan->sleep_us > 0 ? " ou l'alarme du DS3231" : "");
    if (!plan->wake_on_pir) {
        ESP_LOGW(TAG, "le PIR rallumera quand même la carte (commande matérielle de l'interrupteur)");
    }
    vTaskDelay(ticks_ms(20));  /* laisse partir les derniers messages */
    gpio_hold_dis(off);
    gpio_set_level(off, 1);
    gpio_output_enable(off);
    gpio_set_level(off, 1);
    wait_ms_fed(CONFIG_NID_LATCH_OFF_TIMEOUT_MS);

    /* Toujours alimentée : alimentation USB au banc, ou interrupteur absent. */
    gpio_set_level(off, 0);
    if (ldo != NULL) {
        esp_ldo_release_channel(ldo);
    }
    ESP_LOGW(TAG, "alimentation toujours présente après %d ms (USB ?) : repli sur la veille profonde",
             CONFIG_NID_LATCH_OFF_TIMEOUT_MS);
}
#endif

void nid_power_sleep(const nid_sleep_plan_t *plan)
{
    nid_sleep_plan_t p = { .wake_on_button = true };
    if (plan != NULL) {
        p = *plan;
    }
#if CONFIG_NID_POWER_LATCH
    latch_power_off(&p);
#endif
    enter_deep_sleep(p.sleep_us, p.wake_on_pir, p.wake_on_button);
}

void nid_power_shutdown(uint32_t recheck_s)
{
    if (recheck_s > 0) {
        ESP_LOGW(TAG, "arrêt : réveil par le bouton seulement, contrôle de la batterie dans %" PRIu32 " s",
                 recheck_s);
    } else {
        ESP_LOGW(TAG, "arrêt : réveil par le bouton seulement");
    }
    const nid_sleep_plan_t plan = {
        .sleep_us = (uint64_t)recheck_s * US_PER_S,
        .wake_on_pir = false,
        .wake_on_button = true,
    };
    nid_power_sleep(&plan);
}

void nid_power_restart(void)
{
    ESP_LOGW(TAG, "redémarrage logiciel");
    /* Sorties au repos maintenues pendant le redémarrage (aucune lumière, aucun
     * bruit) ; nid_board_init_early() les libère au démarrage suivant. */
    nid_board_prepare_deep_sleep();
    esp_restart();
}
