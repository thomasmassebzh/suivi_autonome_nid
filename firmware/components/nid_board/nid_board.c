/*
 * nid_board — brochage et ressources partagées de la carte
 *
 * Maintien des broches (ESP-IDF v6.1, ESP32-P4 rév. ≥ 3.0) : gpio_hold_en()
 * écrit LP_SYS.pad_rtc_hold_ctrl, dans le domaine basse consommation : chaque
 * broche (0-54) garde son état pendant la veille profonde et APRÈS le réveil,
 * jusqu'à gpio_hold_dis(). gpio_deep_sleep_hold_en() n'existe pas sur cette
 * puce (SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP) et n'est pas nécessaire.
 *
 * Les sorties sont configurées par gpio_output_enable() plutôt que gpio_config() :
 * pas de « réservation » de la broche, donc pas d'avertissement de conflit quand
 * le pilote propriétaire la reprend (LEDC des LED IR, codec audio, ESP-Hosted).
 */
#include "nid_board.h"

#include "sdkconfig.h"
#include "driver/rtc_io.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "nid_board";

#define C6_RESET_PULSE_MS 10

#define PIN_OR_NC(x) ((x) < 0 ? GPIO_NUM_NC : (gpio_num_t)(x))

static const nid_board_pins_t s_pins = {
    .pir = PIN_OR_NC(CONFIG_NID_PIN_PIR),
#if CONFIG_NID_PIR_ACTIVE_HIGH
    .pir_active_high = true,
#else
    .pir_active_high = false,
#endif
    .button = PIN_OR_NC(CONFIG_NID_PIN_BUTTON),
    .rtc_int = PIN_OR_NC(CONFIG_NID_PIN_RTC_INT),
    .status_led = PIN_OR_NC(CONFIG_NID_PIN_STATUS_LED),
#if CONFIG_NID_STATUS_LED_ACTIVE_HIGH
    .status_led_active_high = true,
#else
    .status_led_active_high = false,
#endif
    .ir_led = PIN_OR_NC(CONFIG_NID_PIN_IR_LED),
#if CONFIG_NID_IRCUT_NONE
    .ircut_a = GPIO_NUM_NC,
    .ircut_b = GPIO_NUM_NC,
#elif CONFIG_NID_IRCUT_LEVEL
    .ircut_a = PIN_OR_NC(CONFIG_NID_PIN_IRCUT_A),
    .ircut_b = GPIO_NUM_NC,
#else
    .ircut_a = PIN_OR_NC(CONFIG_NID_PIN_IRCUT_A),
    .ircut_b = PIN_OR_NC(CONFIG_NID_PIN_IRCUT_B),
#endif
    .batt_adc = PIN_OR_NC(CONFIG_NID_PIN_BATT_ADC),
    .ldr_adc = PIN_OR_NC(CONFIG_NID_PIN_LDR_ADC),
    .power_off = PIN_OR_NC(CONFIG_NID_PIN_POWER_OFF),
    .i2c_sda = PIN_OR_NC(CONFIG_NID_PIN_I2C_SDA),
    .i2c_scl = PIN_OR_NC(CONFIG_NID_PIN_I2C_SCL),
    .c6_reset = PIN_OR_NC(CONFIG_NID_PIN_C6_RESET),
    .lcd_backlight = PIN_OR_NC(CONFIG_NID_PIN_LCD_BACKLIGHT),
    .audio_pa = PIN_OR_NC(CONFIG_NID_PIN_AUDIO_PA),
    .cam_reset = PIN_OR_NC(CONFIG_NID_PIN_CAM_RESET),
    .cam_pwdn = PIN_OR_NC(CONFIG_NID_PIN_CAM_PWDN),
    .i2s_mclk = PIN_OR_NC(CONFIG_NID_PIN_I2S_MCLK),
    .i2s_bclk = PIN_OR_NC(CONFIG_NID_PIN_I2S_BCLK),
    .i2s_ws = PIN_OR_NC(CONFIG_NID_PIN_I2S_WS),
    .i2s_dout = PIN_OR_NC(CONFIG_NID_PIN_I2S_DOUT),
    .i2s_din = PIN_OR_NC(CONFIG_NID_PIN_I2S_DIN),
    .sd_clk = PIN_OR_NC(CONFIG_NID_PIN_SD_CLK),
    .sd_cmd = PIN_OR_NC(CONFIG_NID_PIN_SD_CMD),
    .sd_d0 = PIN_OR_NC(CONFIG_NID_PIN_SD_D0),
    .sd_d1 = PIN_OR_NC(CONFIG_NID_PIN_SD_D1),
    .sd_d2 = PIN_OR_NC(CONFIG_NID_PIN_SD_D2),
    .sd_d3 = PIN_OR_NC(CONFIG_NID_PIN_SD_D3),
};

static i2c_master_bus_handle_t s_i2c_bus;

const nid_board_pins_t *nid_board_pins(void)
{
    return &s_pins;
}

/* Entrée de réveil : jamais pilotée ni maintenue en sortie par ce module. */
static bool is_wake_input(gpio_num_t pin)
{
    return pin != GPIO_NUM_NC &&
           (pin == s_pins.pir || pin == s_pins.button || pin == s_pins.rtc_int);
}

static int led_level(bool on)
{
    return (on == s_pins.status_led_active_high) ? 1 : 0;
}

/*
 * Sortie GPIO simple au niveau demandé, sans tirage. gpio_output_enable() rend
 * aussi la broche au GPIO si un périphérique l'utilisait (signal LEDC des LED IR).
 * Si la broche est maintenue, l'état n'atteint la broche qu'à la libération.
 */
static void drive_output(gpio_num_t pin, int level)
{
    if (rtc_gpio_is_valid_gpio(pin)) {
        rtc_gpio_deinit(pin);  /* broche LP : retour au GPIO numérique */
    }
    gpio_set_level(pin, level);
    gpio_output_enable(pin);
    gpio_set_level(pin, level);
    gpio_pullup_dis(pin);    /* un tirage opposé au niveau consommerait ~70 µA */
    gpio_pulldown_dis(pin);
}

/* Démarrage : niveau de repos écrit pendant que la broche est encore maintenue, puis libération. */
static void park_output(gpio_num_t pin, int level)
{
    if (pin == GPIO_NUM_NC || is_wake_input(pin)) {
        return;
    }
    drive_output(pin, level);
    gpio_hold_dis(pin);
}

/* Avant la veille : niveau de repos puis maintien. */
static void hold_output(gpio_num_t pin, int level)
{
    if (pin == GPIO_NUM_NC || is_wake_input(pin)) {
        return;
    }
    drive_output(pin, level);
    gpio_hold_dis(pin);  /* un maintien antérieur figerait l'ancien état */
    gpio_hold_en(pin);
}

esp_err_t nid_board_init_early(void)
{
    /* Ordre : d'abord tout ce qui émet de la lumière ou du bruit (FC1.1, FC1.2). */
    park_output(s_pins.ir_led, 0);
    park_output(s_pins.status_led, led_level(false));
    park_output(s_pins.lcd_backlight, 0);
    park_output(s_pins.audio_pa, 0);
    /* Le C6 reste en reset tant que le mode installation ne le réveille pas (FC3.2). */
    park_output(s_pins.c6_reset, 0);
    /* Interrupteur d'alimentation : OFF inactif (niveau bas = pas de coupure). */
    park_output(s_pins.power_off, 0);
    /* Entrées de réveil maintenues avant la veille (configurées ensuite par nid_detect). */
    const gpio_num_t inputs[] = { s_pins.pir, s_pins.button, s_pins.rtc_int };
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        if (inputs[i] != GPIO_NUM_NC) {
            gpio_hold_dis(inputs[i]);
        }
    }
    return ESP_OK;
}

esp_err_t nid_board_i2c_init(void)
{
    if (s_i2c_bus) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(s_pins.i2c_sda != GPIO_NUM_NC && s_pins.i2c_scl != GPIO_NUM_NC,
                        ESP_ERR_INVALID_STATE, TAG, "broches I2C non configurées");
    i2c_master_bus_config_t cfg = {
        .i2c_port = CONFIG_NID_I2C_PORT,
        .sda_io_num = s_pins.i2c_sda,
        .scl_io_num = s_pins.i2c_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "création du bus I2C impossible : %s", esp_err_to_name(err));
        s_i2c_bus = NULL;
    }
    return err;
}

i2c_master_bus_handle_t nid_board_i2c_bus(void)
{
    return s_i2c_bus;
}

esp_err_t nid_board_coproc_enable(bool enable)
{
    const gpio_num_t pin = s_pins.c6_reset;
    if (pin == GPIO_NUM_NC || is_wake_input(pin)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Reset actif (niveau bas) ; reprend la broche même si ESP-Hosted l'a configurée. */
    drive_output(pin, 0);
    gpio_hold_dis(pin);
    if (enable) {
        /* Impulsion de reset propre, puis reset relâché : sortie haute, non maintenue. */
        vTaskDelay(pdMS_TO_TICKS(C6_RESET_PULSE_MS));
        gpio_set_level(pin, 1);
    }
    ESP_LOGI(TAG, "coprocesseur ESP32-C6 %s", enable ? "démarré" : "maintenu en reset");
    return ESP_OK;
}

void nid_board_status_led(bool on)
{
    if (s_pins.status_led == GPIO_NUM_NC || is_wake_input(s_pins.status_led)) {
        return;
    }
    gpio_set_level(s_pins.status_led, led_level(on));
}

void nid_board_status_blink(int count, int on_ms, int off_ms)
{
    for (int i = 0; i < count; i++) {
        nid_board_status_led(true);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        nid_board_status_led(false);
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}

void nid_board_prepare_deep_sleep(void)
{
    /* Sorties au repos et maintenues pendant la veille (maintien individuel des
     * broches en veille profonde, ESP32-P4 rév. ≥ 3.0). */
    hold_output(s_pins.ir_led, 0);
    hold_output(s_pins.status_led, led_level(false));
    hold_output(s_pins.lcd_backlight, 0);
    hold_output(s_pins.audio_pa, 0);
    hold_output(s_pins.c6_reset, 0);
    hold_output(s_pins.power_off, 0);
}
