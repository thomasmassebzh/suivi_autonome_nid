/*
 * nid_board — brochage et ressources partagées de la carte
 */
#include "nid_board.h"

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "nid_board";

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

/* Configure une sortie et la place immédiatement à un niveau donné. */
static void output_set(gpio_num_t pin, int level)
{
    if (pin == GPIO_NUM_NC) {
        return;
    }
    gpio_hold_dis(pin);
    gpio_set_level(pin, level);
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(pin, level);
}

static int led_level(bool on)
{
    return (on == s_pins.status_led_active_high) ? 1 : 0;
}

esp_err_t nid_board_init_early(void)
{
    /* Ordre : d'abord tout ce qui émet de la lumière ou du bruit (FC1.1, FC1.2). */
    output_set(s_pins.ir_led, 0);
    output_set(s_pins.status_led, led_level(false));
    output_set(s_pins.lcd_backlight, 0);
    output_set(s_pins.audio_pa, 0);
    /* Le C6 reste en reset tant que le mode installation ne le réveille pas (FC3.2). */
    output_set(s_pins.c6_reset, 0);
    /* Interrupteur d'alimentation : OFF au repos (niveau bas = pas de coupure). */
    output_set(s_pins.power_off, 0);
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
    if (s_pins.c6_reset == GPIO_NUM_NC) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    gpio_hold_dis(s_pins.c6_reset);
    if (enable) {
        /* Impulsion de reset propre puis libération. */
        gpio_set_level(s_pins.c6_reset, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(s_pins.c6_reset, 1);
    } else {
        gpio_set_level(s_pins.c6_reset, 0);
    }
    return ESP_OK;
}

void nid_board_status_led(bool on)
{
    if (s_pins.status_led == GPIO_NUM_NC) {
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

static void hold_low(gpio_num_t pin, int level)
{
    if (pin == GPIO_NUM_NC) {
        return;
    }
    gpio_set_level(pin, level);
    gpio_hold_en(pin);
}

void nid_board_prepare_deep_sleep(void)
{
    /* Sorties au repos, maintenues pendant la veille (ESP32-P4 rév. ≥ 3.0 :
     * maintien individuel des broches en veille profonde). */
    hold_low(s_pins.ir_led, 0);
    hold_low(s_pins.status_led, led_level(false));
    hold_low(s_pins.lcd_backlight, 0);
    hold_low(s_pins.audio_pa, 0);
    hold_low(s_pins.c6_reset, 0);
    hold_low(s_pins.power_off, 0);
}
