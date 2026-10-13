/*
 * nid_capture — piste son : micro de la carte (codec ES8311) → AAC
 *
 * Adapté des exemples et tests d'Espressif :
 *  - esp_codec_dev, test_apps/codec_dev_test/main/test_board.c (Apache-2.0) :
 *    canal I2S STD maître, interfaces de données I2S, de contrôle I2C et GPIO,
 *    codec ES8311 (es8311_codec_new) et périphérique d'entrée esp_codec_dev ;
 *  - esp_muxer, test_apps/muxer_test/main/muxer_test.c
 *    (LicenseRef-Espressif-Modified-MIT, voir LICENSE_ESPRESSIF_EXAMPLES) :
 *    ouverture de l'encodeur AAC (esp_audio_enc), taille des trames,
 *    AudioSpecificConfig (codec_spec_info) et horodatage des trames ;
 *  - esp_muxer, examples/video_muxer (Apache-2.0) : gain d'entrée du micro.
 * Modifications : réception seule, ampli du haut-parleur jamais piloté
 * (pa_pin = -1, FC1.2), lecture dans une tâche dédiée pendant la séquence.
 */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_audio_enc.h"
#include "esp_aac_enc.h"

#include "nid_board.h"
#include "nid_capture_priv.h"

static const char *TAG = "nid_capture_son";

#define AUDIO_SAMPLE_RATE       16000
#define AUDIO_CHANNELS          1
#define AUDIO_BITS              16
#define AUDIO_BITRATE           48000       /* AAC-LC mono 16 kHz : 22 000 à 96 000 bit/s */
#define AUDIO_IN_GAIN_DB        30.0f       /* exemple video_muxer : 32 dB */
#define AUDIO_TASK_STACK        (16 * 1024) /* encodeur AAC */
#define AUDIO_TASK_PRIO         6
#define AUDIO_STOP_TIMEOUT_MS   1000
#define I2S_DMA_DESC_NUM        8           /* 8 x 512 trames = 256 ms de marge pendant les écritures SD */
#define I2S_DMA_FRAME_NUM       512

static struct {
    bool                          inited;
    i2s_chan_handle_t             rx;
    const audio_codec_data_if_t  *data_if;
    const audio_codec_ctrl_if_t  *ctrl_if;
    const audio_codec_gpio_if_t  *gpio_if;
    const audio_codec_if_t       *codec_if;
    esp_codec_dev_handle_t        dev;

    /* séquence en cours */
    esp_audio_enc_handle_t        enc;
    bool                          dev_open;
    uint8_t                      *pcm;
    int                           pcm_size;
    uint8_t                      *out;
    int                           out_size;
    TaskHandle_t                  task;
    SemaphoreHandle_t             done;
    StaticSemaphore_t             done_buf;
    atomic_bool                   run;
    atomic_bool                   deliver;
    int64_t                       t0_us;
    uint32_t                      frames;
    nid_capture_audio_sink_t      sink;
    void                         *ctx;
} s_au;

static void free_codec(void)
{
    if (s_au.dev) {
        esp_codec_dev_delete(s_au.dev);
        s_au.dev = NULL;
    }
    if (s_au.codec_if) {
        audio_codec_delete_codec_if(s_au.codec_if);
        s_au.codec_if = NULL;
    }
    if (s_au.ctrl_if) {
        audio_codec_delete_ctrl_if(s_au.ctrl_if);
        s_au.ctrl_if = NULL;
    }
    if (s_au.gpio_if) {
        audio_codec_delete_gpio_if(s_au.gpio_if);
        s_au.gpio_if = NULL;
    }
    if (s_au.data_if) {
        audio_codec_delete_data_if(s_au.data_if);
        s_au.data_if = NULL;
    }
    if (s_au.rx) {
        i2s_del_channel(s_au.rx);
        s_au.rx = NULL;
    }
}

esp_err_t nid_capture_audio_init(void)
{
    if (s_au.inited) {
        return ESP_OK;
    }
    const nid_board_pins_t *pins = nid_board_pins();
    i2c_master_bus_handle_t bus = nid_board_i2c_bus();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "bus I2C absent");
    ESP_RETURN_ON_FALSE(pins->i2s_bclk != GPIO_NUM_NC && pins->i2s_ws != GPIO_NUM_NC && pins->i2s_din != GPIO_NUM_NC,
                        ESP_ERR_NOT_SUPPORTED, TAG, "broches I2S du micro non configurées");
    if (s_au.done == NULL) {
        s_au.done = xSemaphoreCreateBinaryStatic(&s_au.done_buf);
    }

    /* Canal I2S en réception seule, maître (MCLK fourni à l'ES8311). */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_au.rx);
    ESP_RETURN_ON_ERROR(err, TAG, "canal I2S");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = pins->i2s_mclk,
            .bclk = pins->i2s_bclk,
            .ws = pins->i2s_ws,
            .dout = I2S_GPIO_UNUSED,
            .din = pins->i2s_din,
        },
    };
    err = i2s_channel_init_std_mode(s_au.rx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mode I2S STD : %s", esp_err_to_name(err));
        free_codec();
        return err;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .rx_handle = s_au.rx,
        .tx_handle = NULL,
    };
    s_au.data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    s_au.ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    s_au.gpio_if = audio_codec_new_gpio();
    if (s_au.data_if == NULL || s_au.ctrl_if == NULL || s_au.gpio_if == NULL) {
        ESP_LOGE(TAG, "interfaces du codec");
        free_codec();
        return ESP_ERR_NO_MEM;
    }

    /* ES8311 en ADC seul ; pa_pin = -1 : l'ampli du haut-parleur n'est jamais activé. */
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = s_au.ctrl_if,
        .gpio_if = s_au.gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin = -1,
        .master_mode = false,
        .use_mclk = pins->i2s_mclk != GPIO_NUM_NC,
    };
    s_au.codec_if = es8311_codec_new(&es8311_cfg);
    if (s_au.codec_if == NULL) {
        ESP_LOGE(TAG, "ES8311 absent ou muet sur le bus I2C");
        free_codec();
        return ESP_ERR_NOT_FOUND;
    }
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_au.codec_if,
        .data_if = s_au.data_if,
    };
    s_au.dev = esp_codec_dev_new(&dev_cfg);
    if (s_au.dev == NULL) {
        ESP_LOGE(TAG, "périphérique d'entrée audio");
        free_codec();
        return ESP_ERR_NO_MEM;
    }

    if (esp_aac_enc_register() != ESP_AUDIO_ERR_OK) {
        ESP_LOGD(TAG, "encodeur AAC déjà enregistré");
    }
    s_au.inited = true;
    ESP_LOGI(TAG, "micro prêt (ES8311, %d Hz, AAC %d bit/s)", AUDIO_SAMPLE_RATE, AUDIO_BITRATE);
    return ESP_OK;
}

bool nid_capture_audio_ready(void)
{
    return s_au.inited;
}

static void audio_task(void *arg)
{
    (void)arg;
    const int64_t chunk_us = (int64_t)s_au.pcm_size * 1000000 / (AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BITS / 8));
    bool have_base = false;
    int64_t base_ms = 0;
    uint32_t read_errors = 0;

    while (atomic_load(&s_au.run)) {
        int ret = esp_codec_dev_read(s_au.dev, s_au.pcm, s_au.pcm_size);
        int64_t t_end = esp_timer_get_time();
        if (ret != ESP_CODEC_DEV_OK) {
            if (read_errors++ == 0) {
                ESP_LOGW(TAG, "lecture du micro : erreur %d", ret);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!atomic_load(&s_au.deliver)) {
            continue;   /* avant la première image vidéo : micro en route, trames ignorées */
        }
        esp_audio_enc_in_frame_t in_frame = {
            .buffer = s_au.pcm,
            .len = (uint32_t)s_au.pcm_size,
        };
        esp_audio_enc_out_frame_t out_frame = {
            .buffer = s_au.out,
            .len = (uint32_t)s_au.out_size,
        };
        if (esp_audio_enc_process(s_au.enc, &in_frame, &out_frame) != ESP_AUDIO_ERR_OK) {
            continue;
        }
        if (!have_base) {
            /* Début du premier bloc remis, relatif à la première image vidéo. */
            base_ms = (t_end - chunk_us - s_au.t0_us) / 1000;
            if (base_ms < 0) {
                base_ms = 0;
            }
            have_base = true;
        }
        if (out_frame.encoded_bytes > 0) {
            s_au.sink(out_frame.buffer, out_frame.encoded_bytes, (uint32_t)(base_ms + (int64_t)out_frame.pts), s_au.ctx);
            s_au.frames++;
        }
    }
    xSemaphoreGive(s_au.done);
    vTaskDelete(NULL);
}

static void release_session(void)
{
    if (s_au.dev_open) {
        esp_codec_dev_close(s_au.dev);
        s_au.dev_open = false;
    }
    if (s_au.enc) {
        esp_audio_enc_close(s_au.enc);
        s_au.enc = NULL;
    }
    heap_caps_free(s_au.pcm);
    s_au.pcm = NULL;
    heap_caps_free(s_au.out);
    s_au.out = NULL;
}

esp_err_t nid_capture_audio_start(nid_capture_audio_sink_t sink, void *ctx, nid_capture_audio_info_t *info)
{
    ESP_RETURN_ON_FALSE(s_au.inited, ESP_ERR_INVALID_STATE, TAG, "micro non initialisé");
    ESP_RETURN_ON_FALSE(sink != NULL && info != NULL, ESP_ERR_INVALID_ARG, TAG, "paramètres absents");
    ESP_RETURN_ON_FALSE(s_au.task == NULL, ESP_ERR_INVALID_STATE, TAG, "séquence audio déjà en cours");

    /* Encodeur AAC brut (sans ADTS) : AudioSpecificConfig pour le MP4 (motif de muxer_test). */
    esp_aac_enc_config_t aac_cfg = ESP_AAC_ENC_CONFIG_DEFAULT();
    aac_cfg.sample_rate = AUDIO_SAMPLE_RATE;
    aac_cfg.channel = AUDIO_CHANNELS;
    aac_cfg.bits_per_sample = AUDIO_BITS;
    aac_cfg.bitrate = AUDIO_BITRATE;
    aac_cfg.adts_used = false;
    esp_audio_enc_config_t enc_cfg = {
        .type = ESP_AUDIO_TYPE_AAC,
        .cfg = &aac_cfg,
        .cfg_sz = sizeof(aac_cfg),
    };
    esp_audio_err_t aerr = esp_audio_enc_open(&enc_cfg, &s_au.enc);
    ESP_RETURN_ON_FALSE(aerr == ESP_AUDIO_ERR_OK, ESP_FAIL, TAG, "encodeur AAC (%d)", (int)aerr);

    esp_audio_enc_info_t enc_info;
    memset(&enc_info, 0, sizeof(enc_info));
    esp_audio_enc_get_info(s_au.enc, &enc_info);
    int in_size = 0, out_size = 0;
    esp_audio_enc_get_frame_size(s_au.enc, &in_size, &out_size);
    if (enc_info.spec_info_len == 0 || in_size <= 0 || out_size <= 0) {
        ESP_LOGE(TAG, "encodeur AAC : informations absentes");
        release_session();
        return ESP_FAIL;
    }
    s_au.pcm_size = in_size;
    s_au.out_size = out_size;
    s_au.pcm = heap_caps_malloc((size_t)in_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_au.out = heap_caps_malloc((size_t)out_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_au.pcm == NULL || s_au.out == NULL) {
        release_session();
        return ESP_ERR_NO_MEM;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BITS,
    };
    if (esp_codec_dev_open(s_au.dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "ouverture du micro impossible");
        release_session();
        return ESP_FAIL;
    }
    s_au.dev_open = true;
    esp_codec_dev_set_in_gain(s_au.dev, AUDIO_IN_GAIN_DB);

    s_au.sink = sink;
    s_au.ctx = ctx;
    s_au.frames = 0;
    s_au.t0_us = 0;
    atomic_store(&s_au.deliver, false);
    atomic_store(&s_au.run, true);
    xSemaphoreTake(s_au.done, 0);
    if (xTaskCreate(audio_task, "nid_son", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIO, &s_au.task) != pdPASS) {
        s_au.task = NULL;
        atomic_store(&s_au.run, false);
        release_session();
        return ESP_ERR_NO_MEM;
    }

    info->sample_rate = AUDIO_SAMPLE_RATE;
    info->channels = AUDIO_CHANNELS;
    info->bits_per_sample = AUDIO_BITS;
    info->frame_ms = (uint32_t)((int64_t)in_size * 1000 / (AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BITS / 8)));
    info->spec_info = enc_info.codec_spec_info;
    info->spec_info_len = enc_info.spec_info_len;
    return ESP_OK;
}

void nid_capture_audio_deliver(int64_t t0_us)
{
    s_au.t0_us = t0_us;
    atomic_store(&s_au.deliver, true);
}

uint32_t nid_capture_audio_stop(void)
{
    if (s_au.task != NULL) {
        atomic_store(&s_au.run, false);
        if (xSemaphoreTake(s_au.done, pdMS_TO_TICKS(AUDIO_STOP_TIMEOUT_MS)) != pdTRUE) {
            /* Lecture bloquée : la tâche référence les tampons, ils ne sont pas libérés. */
            ESP_LOGE(TAG, "tâche audio bloquée");
            s_au.task = NULL;
            return s_au.frames;
        }
        s_au.task = NULL;
    }
    atomic_store(&s_au.deliver, false);
    release_session();
    return s_au.frames;
}

void nid_capture_audio_deinit(void)
{
    nid_capture_audio_stop();
    if (!s_au.inited) {
        return;
    }
    free_codec();
    s_au.inited = false;
}
