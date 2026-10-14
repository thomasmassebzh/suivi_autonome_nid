/*
 * nid_detect — détecteur PIR et bouton de mise en service
 *
 * Entrées : PIR avec tirage vers son niveau de repos (une sortie débranchée ne
 * flotte pas), bouton vers la masse avec tirage haut. Pendant la veille
 * profonde, nid_power reconfigure ces broches en E/S basse consommation (EXT1) ;
 * nid_detect_init() les rend ensuite au GPIO numérique (gpio_config).
 *
 * La validation d'un déclenchement échantillonne la sortie PIR toutes les 5 ms
 * en rendant la main au planificateur (vTaskDelay) : elle tourne en parallèle du
 * démarrage de la caméra sans le ralentir (FP1.4).
 */
#include "nid_detect.h"

#include <stdint.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "nid_board.h"

static const char *TAG = "nid_detect";

#define CONFIRM_POLL_MS     5       /* échantillonnage de la validation PIR */
#define IDLE_POLL_MS        10      /* échantillonnage de l'attente du repos */
#define IDLE_STABLE_US      (50 * 1000)
#define WDT_FEED_PERIOD_US  (1000 * 1000)
#define BUTTON_DEBOUNCE_US  (50 * 1000)

static gpio_num_t s_pir = GPIO_NUM_NC;
static gpio_num_t s_button = GPIO_NUM_NC;
static bool s_pir_active_high = true;

/* Événements du mode banc. */
static QueueHandle_t volatile s_evt_queue;
static esp_timer_handle_t s_btn_timer;
static volatile bool s_btn_pending;
static bool s_isr_service_ready;
static bool s_events_on;
static volatile int64_t s_pir_edge_us;   /* instant du dernier front actif (mode banc) */

static TickType_t ticks_ms(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    return t > 0 ? t : 1;
}

/* Nourrit le chien de garde au plus une fois par seconde, si la tâche y est abonnée. */
static void wdt_feed_periodic(int64_t *last_us, int64_t now_us)
{
#if CONFIG_ESP_TASK_WDT_EN && CONFIG_ESP_TASK_WDT_INIT
    if (now_us - *last_us >= WDT_FEED_PERIOD_US) {
        *last_us = now_us;
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            esp_task_wdt_reset();
        }
    }
#else
    (void)last_us;
    (void)now_us;
#endif
}

static esp_err_t config_input(gpio_num_t pin, bool pull_up)
{
    gpio_hold_dis(pin);  /* maintien éventuel posé avant la veille */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_up ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io);
}

esp_err_t nid_detect_init(void)
{
    const nid_board_pins_t *pins = nid_board_pins();
    esp_err_t ret = ESP_OK;

    s_pir_active_high = pins->pir_active_high;
    if (pins->pir != GPIO_NUM_NC) {
        /* Tirage vers le niveau de repos : bas pour une sortie active haute. */
        esp_err_t err = config_input(pins->pir, !s_pir_active_high);
        if (err == ESP_OK) {
            s_pir = pins->pir;
        } else {
            ESP_LOGE(TAG, "entrée PIR (GPIO%d) : %s", pins->pir, esp_err_to_name(err));
            ret = err;
        }
    }
    if (pins->button != GPIO_NUM_NC && pins->button != pins->pir) {
        esp_err_t err = config_input(pins->button, true);
        if (err == ESP_OK) {
            s_button = pins->button;
        } else {
            ESP_LOGE(TAG, "entrée bouton (GPIO%d) : %s", pins->button, esp_err_to_name(err));
            ret = err;
        }
    }
    return ret;
}

bool nid_detect_pir_active(void)
{
    if (s_pir == GPIO_NUM_NC) {
        return false;
    }
    return gpio_get_level(s_pir) == (s_pir_active_high ? 1 : 0);
}

bool nid_detect_button_pressed(void)
{
    if (s_button == GPIO_NUM_NC) {
        return false;
    }
    return gpio_get_level(s_button) == 0;
}

uint32_t nid_detect_min_active_ms(uint8_t sensitivity)
{
    switch (sensitivity) {
    case 5:
        return 0;
    case 4:
        return 50;
    case 3:
        return 150;
    case 2:
        return 300;
    default:
        return 500;
    }
}

bool nid_detect_pir_confirm(uint32_t min_active_ms, uint32_t timeout_ms)
{
    if (min_active_ms == 0) {
        return true;
    }
    if (s_pir == GPIO_NUM_NC) {
        return false;
    }
    const int64_t need_us = (int64_t)min_active_ms * 1000;
    const int64_t start = esp_timer_get_time();
    const int64_t deadline = start + (int64_t)timeout_ms * 1000;
    int64_t active_us = 0;
    int64_t prev_us = start;
    int64_t last_feed = start;
    bool prev_active = nid_detect_pir_active();

    for (;;) {
        vTaskDelay(ticks_ms(CONFIRM_POLL_MS));
        const int64_t now = esp_timer_get_time();
        const bool active = nid_detect_pir_active();
        /* Intervalle compté en entier si le PIR est actif aux deux échantillons,
         * pour moitié s'il ne l'est qu'à l'un des deux (front dans l'intervalle). */
        if (prev_active && active) {
            active_us += now - prev_us;
        } else if (prev_active || active) {
            active_us += (now - prev_us) / 2;
        }
        prev_us = now;
        prev_active = active;
        if (active_us >= need_us) {
            ESP_LOGD(TAG, "PIR validé en %lld ms", (long long)((now - start) / 1000));
            return true;
        }
        if (now >= deadline) {
            ESP_LOGI(TAG, "PIR non validé : actif %lld ms sur %lld ms (seuil %lu ms)",
                     (long long)(active_us / 1000), (long long)((now - start) / 1000),
                     (unsigned long)min_active_ms);
            return false;
        }
        wdt_feed_periodic(&last_feed, now);
    }
}

esp_err_t nid_detect_wait_pir_idle(uint32_t timeout_ms)
{
    if (s_pir == GPIO_NUM_NC) {
        return ESP_OK;
    }
    const int64_t start = esp_timer_get_time();
    const int64_t deadline = start + (int64_t)timeout_ms * 1000;
    int64_t idle_since = -1;  /* début du repos en cours (-1 : PIR actif) */
    int64_t last_feed = start;

    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (nid_detect_pir_active()) {
            idle_since = -1;
        } else if (idle_since < 0) {
            idle_since = now;
        }
        if (idle_since >= 0 && now - idle_since >= IDLE_STABLE_US) {
            return ESP_OK;
        }
        if (now >= deadline) {
            /* Délai écoulé : accepté si le PIR est au repos à cet instant. */
            return idle_since >= 0 ? ESP_OK : ESP_ERR_TIMEOUT;
        }
        wdt_feed_periodic(&last_feed, now);
        vTaskDelay(ticks_ms(IDLE_POLL_MS));
    }
}

/* ------------------------------------------------------------------------- */
/* Mode banc : interruptions                                                 */
/* ------------------------------------------------------------------------- */

static void IRAM_ATTR pir_isr(void *arg)
{
    (void)arg;
    QueueHandle_t q = s_evt_queue;
    if (q == NULL) {
        return;
    }
    s_pir_edge_us = esp_timer_get_time();
    const nid_detect_evt_t evt = NID_DETECT_EVT_PIR;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(q, &evt, &woken);
    portYIELD_FROM_ISR(woken);
}

static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;
    if (s_btn_pending || s_evt_queue == NULL) {
        return;  /* rebond pendant la confirmation */
    }
    s_btn_pending = true;
    if (esp_timer_start_once(s_btn_timer, BUTTON_DEBOUNCE_US) != ESP_OK) {
        s_btn_pending = false;
    }
}

/* Tâche esp_timer, 50 ms après le front : un appui réel tient le bouton enfoncé. */
static void button_confirm_cb(void *arg)
{
    (void)arg;
    QueueHandle_t q = s_evt_queue;
    if (q != NULL && nid_detect_button_pressed()) {
        const nid_detect_evt_t evt = NID_DETECT_EVT_BUTTON;
        if (xQueueSend(q, &evt, 0) != pdTRUE) {
            ESP_LOGW(TAG, "file d'événements pleine : appui bouton perdu");
        }
    }
    s_btn_pending = false;
}

esp_err_t nid_detect_enable_events(QueueHandle_t queue)
{
    if (queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_pir == GPIO_NUM_NC && s_button == GPIO_NUM_NC) {
        return ESP_ERR_INVALID_STATE;  /* nid_detect_init() non appelé ou aucune entrée */
    }
    if (s_events_on) {
        nid_detect_disable_events();
    }
    if (!s_isr_service_ready) {
        /* Service partagé : ESP_ERR_INVALID_STATE = déjà installé par un autre module. */
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "service d'interruptions GPIO : %s", esp_err_to_name(err));
            return err;
        }
        s_isr_service_ready = true;
    }
    if (s_button != GPIO_NUM_NC && s_btn_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = button_confirm_cb,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "nid_bouton",
        };
        esp_err_t err = esp_timer_create(&args, &s_btn_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "minuterie anti-rebond : %s", esp_err_to_name(err));
            return err;
        }
    }

    s_btn_pending = false;
    s_evt_queue = queue;
    esp_err_t ret = ESP_OK;
    if (s_pir != GPIO_NUM_NC) {
        gpio_set_intr_type(s_pir, s_pir_active_high ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE);
        ret = gpio_isr_handler_add(s_pir, pir_isr, NULL);
    }
    if (ret == ESP_OK && s_button != GPIO_NUM_NC) {
        gpio_set_intr_type(s_button, GPIO_INTR_NEGEDGE);
        ret = gpio_isr_handler_add(s_button, button_isr, NULL);
    }
    s_events_on = true;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "interruptions PIR/bouton : %s", esp_err_to_name(ret));
        nid_detect_disable_events();
        return ret;
    }
    ESP_LOGI(TAG, "événements actifs : PIR GPIO%d, bouton GPIO%d", s_pir, s_button);
    return ESP_OK;
}

int64_t nid_detect_last_pir_edge_us(void)
{
    return s_pir_edge_us;
}

void nid_detect_disable_events(void)
{
    if (!s_events_on) {
        return;
    }
    if (s_pir != GPIO_NUM_NC) {
        gpio_isr_handler_remove(s_pir);
        gpio_set_intr_type(s_pir, GPIO_INTR_DISABLE);
    }
    if (s_button != GPIO_NUM_NC) {
        gpio_isr_handler_remove(s_button);
        gpio_set_intr_type(s_button, GPIO_INTR_DISABLE);
    }
    if (s_btn_timer != NULL) {
        esp_timer_stop(s_btn_timer);  /* ESP_ERR_INVALID_STATE si déjà arrêtée : sans importance */
    }
    s_evt_queue = NULL;
    s_btn_pending = false;
    s_events_on = false;
}
