/*
 * nid_capture — caméra MIPI-CSI, vidéo H.264 + son AAC en MP4, photo JPEG
 *
 * Adapté des exemples officiels d'Espressif (voir LICENSE_ESPRESSIF_EXAMPLES) :
 *
 *  - esp_video, examples/image_storage/sd_card/main/sd_card_main.c
 *    (SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD,
 *    SPDX-License-Identifier: ESPRESSIF MIT) : ouverture des périphériques V4L2,
 *    réglage des encodeurs mémoire à mémoire H.264 et JPEG (I-period, débit,
 *    QP, qualité), files de tampons MMAP / USERPTR, démarrage des flux, saut
 *    des premières images et boucle « DQBUF caméra → QBUF encodeur → DQBUF ».
 *  - esp_video, examples/common_components/example_video_common/example_init_video.c
 *    (même licence) : esp_video_init() avec un bus I2C créé par l'application
 *    (init_sccb = false, i2c_handle), broches reset / pwdn du capteur.
 *  - esp_video, examples/capture_stream/main/capture_stream_main.c (même
 *    licence) : identifiant du capteur (ESP_CAM_SENSOR_IOC_G_CHIP_ID), cadence
 *    par VIDIOC_S_PARM.
 *  - esp_muxer, examples/video_muxer/main/main.c (SPDX-FileCopyrightText: 2026
 *    Espressif Systems (Shanghai) CO., LTD, SPDX-License-Identifier: Apache-2.0)
 *    et test_apps/muxer_test (LicenseRef-Espressif-Modified-MIT) : configuration
 *    du multiplexeur MP4 (mp4_muxer_config_t, ram_cache_size, chemin par
 *    url_pattern_ex), ajout des flux puis des paquets, fermeture. Modifications :
 *    pas d'esp_capture, flux H.264 issu du périphérique V4L2 M2M, SPS/PPS de la
 *    première image IDR passés en codec_spec_info, écriture par un « file
 *    writer » qui fait fflush + fsync avant fclose.
 *
 * Chaîne : capteur (OV5647 ou SC2336) → MIPI-CSI → ISP (exposition, balance des
 * blancs : contrôleur de pipeline esp_ipa) → YUV 4:2:0 Espressif (O_UYY_E_VYY)
 * → incrustation date/heure/identifiants (nid_overlay) + esp_cache_msync C2M →
 * encodeur H.264 (vidéo) ou JPEG (photo, aperçu) → esp_muxer MP4 / fichier.
 *
 * Correspondance résolution réglée → format capteur (VIDIOC_S_SENSOR_FMT), voir
 * aussi la section « Caméra et vidéo » de sdkconfig.defaults :
 *
 *   réglage    | OV5647                          | SC2336
 *   1920x1080  | RAW10 1920x1080 30 i/s          | RAW10 1920x1080 30 i/s
 *   1280x960   | RAW10 1280x960 (binning) 45 i/s | 1920x1080 recadré au centre par l'ISP
 *   1280x720   | 1280x960 recadré au centre      | RAW8 1280x720 30 i/s
 *
 * Règle générale : format MIPI le plus petit couvrant la taille demandée, puis
 * recadrage centré par l'ISP (VIDIOC_S_SELECTION) s'il est plus grand. Cadence
 * effective : plus grand diviseur entier de la cadence du capteur ne dépassant
 * pas la cadence réglée (30 i/s → 30, 15, 10 ; 45 i/s → 45, 15, 9), saut
 * d'images fait par esp_video.
 *
 * Plage YUV : limitée (16-235) pour la vidéo H.264, pleine (0-255) pour le
 * JPEG ; l'ISP est reconfiguré lors du passage de l'une à l'autre.
 *
 * Concurrence : toutes les fonctions publiques (sauf is_ready, sensor_name et
 * set_night) sont sérialisées par un mutex ; l'enregistrement le garde pendant
 * toute la séquence.
 */
#include "nid_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "esp_muxer.h"
#include "mp4_muxer.h"

#include "nid_board.h"
#include "nid_capture_priv.h"
#include "nid_overlay.h"

#if !CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE || !CONFIG_ESP_VIDEO_ENABLE_HW_H264_VIDEO_DEVICE || \
    !CONFIG_ESP_VIDEO_ENABLE_HW_JPEG_ENC_VIDEO_DEVICE
#error "nid_capture : activer MIPI-CSI, l'encodeur H.264 et l'encodeur JPEG d'esp_video (sdkconfig.defaults)"
#endif

static const char *TAG = "nid_capture";

/* Périphériques V4L2 d'esp_video */
#define CAM_DEV_PATH            ESP_VIDEO_MIPI_CSI_DEVICE_NAME
#define H264_DEV_PATH           ESP_VIDEO_H264_DEVICE_NAME
#define JPEG_DEV_PATH           ESP_VIDEO_JPEG_DEVICE_NAME
#define ISP_DEV_PATH            ESP_VIDEO_ISP1_DEVICE_NAME

#define SCCB_FREQ_HZ            100000      /* bus I2C partagé (caméra, ES8311, DS3231) */
#define CAM_BUF_COUNT           3           /* exemple : 2 ; un de plus pour absorber les latences de la carte SD */
#define SKIP_STARTUP_FRAME_COUNT 2          /* exemple sd_card : images instables au démarrage du flux */
#define PHOTO_SKIP_FRAMES       8           /* photo : laisse l'exposition automatique converger */
#define CAM_DQBUF_TIMEOUT_MS    150         /* garantit l'appel du rappel d'arrêt toutes les ~200 ms */
#define CAM_STALL_US            3000000LL   /* plus d'image depuis 3 s : caméra en défaut */
#define M2M_DQBUF_TIMEOUT_MS    1000
#define STOP_CB_PERIOD_US       100000LL
#define PV_RELEASE_WAIT_US      1000000LL

/* Encodeur H.264 : bornes de l'encodeur matériel (esp_video_h264_device.c) */
#define H264_BITRATE_STEP       25000
#define H264_BITRATE_MIN        25000
#define H264_BITRATE_MAX        25000000
#define H264_MIN_QP             22
#define H264_MAX_QP             40
#define H264_SPEC_MAX           256         /* SPS + PPS avec codes de début */

#define MUXER_CACHE_SIZE        (32 * 1024)

/* Identifiants des capteurs (ov5647.h, sc2336.h d'esp_cam_sensor) */
#define OV5647_CHIP_PID         0x5647
#define SC2336_CHIP_PID         0xcb3a

#define ALIGN_UP(x, a)          ((((x) + (a) - 1) / (a)) * (a))

typedef struct {
    nid_overlay_t ov;
    time_t        sec;              /*!< seconde affichée, -1 : texte à rendre */
    bool          ok;
} overlay_ctx_t;

typedef struct {
    volatile bool ready;
    bool          video_inited;     /*!< esp_video_init() réussi */
    nid_capture_settings_t cfg;
    char          sensor_name[24];

    /* caméra */
    int           cam_fd;
    uint16_t      width, height;    /*!< image livrée (après recadrage éventuel) */
    uint8_t       sensor_fps;
    uint8_t       fps;              /*!< cadence effective */
    bool          crop_checked;
    bool          cam_configured;
    bool          cam_limited;      /*!< plage YUV limitée (vidéo) ou pleine (JPEG) */
    bool          cam_streaming;
    uint32_t      cam_timeout_ms;
    uint8_t      *cam_buf[CAM_BUF_COUNT];

    /* encodeur H.264 */
    int           h264_fd;
    uint8_t      *h264_buf;

    /* encodeur JPEG */
    int           jpeg_fd;
    uint8_t      *jpeg_buf;
    uint16_t      jpeg_w, jpeg_h;
    bool          jpeg_streaming;

    /* ISP : saturation (monochrome de nuit) */
    int           isp_fd;
    int32_t       day_saturation;

    /* incrustation */
    overlay_ctx_t ov_video;         /*!< image entière, plage limitée (H.264) */
    overlay_ctx_t ov_photo;         /*!< image entière, plage pleine (JPEG) */
    overlay_ctx_t ov_preview;       /*!< image réduite de l'aperçu, plage pleine */

    /* aperçu */
    bool          pv_on;            /*!< aperçu demandé (start .. stop) */
    bool          pv_active;        /*!< chaîne de l'aperçu configurée et caméra en marche */
    volatile bool pv_held;          /*!< image JPEG prêtée au lecteur */
    uint8_t       pv_quality;
    uint16_t      pv_max_width;
    uint16_t      pv_div;
    uint16_t      pv_w, pv_h;
    uint8_t      *pv_buf;
    size_t        pv_buf_len;

    bool          audio_ok;
} capture_t;

/** Contexte d'un enregistrement (rappels du multiplexeur et de l'audio). */
typedef struct {
    const char        *path;
    esp_muxer_handle_t muxer;
    int                video_idx;
    int                audio_idx;
    bool               streams_added;
    bool               with_audio;
    nid_capture_audio_info_t audio_info;
    volatile bool      write_error;
    volatile bool      closed_ok;
    uint8_t            spec[H264_SPEC_MAX];
    size_t             spec_len;
} rec_t;

static capture_t s_cap = {
    .cam_fd = -1, .h264_fd = -1, .jpeg_fd = -1, .isp_fd = -1,
    .sensor_name = "aucun",
};
static rec_t s_rec;

static SemaphoreHandle_t s_lock;            /* sérialise l'API */
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_ctrl_lock;       /* jour / nuit (ISP), indépendant d'un enregistrement en cours */
static StaticSemaphore_t s_ctrl_lock_buf;
static SemaphoreHandle_t s_mux_lock;        /* multiplexeur : tâche vidéo et tâche audio */
static StaticSemaphore_t s_mux_lock_buf;

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------------- */
/* Outils V4L2 (exemple sd_card)                                              */
/* ------------------------------------------------------------------------- */

/** Reprise de set_codec_control() de l'exemple sd_card. */
static esp_err_t set_ctrl(int fd, uint32_t ctrl_class, uint32_t id, int32_t value)
{
    struct v4l2_ext_controls controls;
    struct v4l2_ext_control control[1];

    memset(&controls, 0, sizeof(controls));
    memset(control, 0, sizeof(control));
    controls.ctrl_class = ctrl_class;
    controls.count = 1;
    controls.controls = control;
    control[0].id = id;
    control[0].value = value;

    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "réglage refusé : contrôle 0x%" PRIx32 " = %" PRId32, id, value);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t get_ctrl(int fd, uint32_t ctrl_class, uint32_t id, int32_t *value)
{
    struct v4l2_ext_controls controls;
    struct v4l2_ext_control control[1];

    memset(&controls, 0, sizeof(controls));
    memset(control, 0, sizeof(control));
    controls.ctrl_class = ctrl_class;
    controls.count = 1;
    controls.controls = control;
    control[0].id = id;

    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &controls) != 0) {
        return ESP_FAIL;
    }
    *value = control[0].value;
    return ESP_OK;
}

static esp_err_t set_dqbuf_timeout(int fd, uint32_t ms)
{
    struct timeval tv = {
        .tv_sec = (time_t)(ms / 1000),
        .tv_usec = (suseconds_t)((ms % 1000) * 1000),
    };
    return ioctl(fd, VIDIOC_S_DQBUF_TIMEOUT, &tv) == 0 ? ESP_OK : ESP_FAIL;
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/* ------------------------------------------------------------------------- */
/* Caméra (/dev/video0)                                                       */
/* ------------------------------------------------------------------------- */

static esp_err_t cam_qbuf(uint32_t index)
{
    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    return ioctl(s_cap.cam_fd, VIDIOC_QBUF, &buf) == 0 ? ESP_OK : ESP_FAIL;
}

/** Image suivante de la caméra, en ms d'attente au plus (0 : sans attente). */
static esp_err_t cam_dqbuf(struct v4l2_buffer *buf, uint32_t timeout_ms)
{
    if (timeout_ms != s_cap.cam_timeout_ms) {
        if (set_dqbuf_timeout(s_cap.cam_fd, timeout_ms) == ESP_OK) {
            s_cap.cam_timeout_ms = timeout_ms;
        }
    }
    memset(buf, 0, sizeof(*buf));
    buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf->memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_cap.cam_fd, VIDIOC_DQBUF, buf) != 0) {
        return ESP_ERR_TIMEOUT;
    }
    if ((buf->flags & V4L2_BUF_FLAG_ERROR) || buf->bytesused == 0 || buf->index >= CAM_BUF_COUNT) {
        if (buf->index < CAM_BUF_COUNT) {
            cam_qbuf(buf->index);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/** Dernière image disponible : les images plus anciennes déjà prêtes sont rendues à la caméra. */
static esp_err_t cam_dqbuf_latest(struct v4l2_buffer *buf, uint32_t timeout_ms)
{
    esp_err_t err = cam_dqbuf(buf, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        struct v4l2_buffer next;
        if (cam_dqbuf(&next, 0) != ESP_OK) {
            break;
        }
        cam_qbuf(buf->index);
        *buf = next;
    }
    return ESP_OK;
}

static esp_err_t cam_start(void)
{
    if (s_cap.cam_streaming) {
        return ESP_OK;
    }
    for (uint32_t i = 0; i < CAM_BUF_COUNT; i++) {
        ESP_RETURN_ON_ERROR(cam_qbuf(i), TAG, "file de la caméra refusée");
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(s_cap.cam_fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL, TAG, "démarrage du flux caméra impossible");
    s_cap.cam_streaming = true;
    return ESP_OK;
}

static void cam_stop(void)
{
    if (!s_cap.cam_streaming) {
        return;
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cap.cam_fd, VIDIOC_STREAMOFF, &type) != 0) {
        ESP_LOGW(TAG, "arrêt du flux caméra : erreur %d", errno);
    }
    s_cap.cam_streaming = false;
}

/** Saute n images (exposition automatique, stabilisation du flux). */
static void cam_skip(int n)
{
    int64_t t0 = esp_timer_get_time();
    while (n > 0 && esp_timer_get_time() - t0 < CAM_STALL_US) {
        struct v4l2_buffer buf;
        if (cam_dqbuf(&buf, CAM_DQBUF_TIMEOUT_MS) == ESP_OK) {
            cam_qbuf(buf.index);
            n--;
        }
    }
}

/** Nom du capteur détecté (exemple capture_stream : ESP_CAM_SENSOR_IOC_G_CHIP_ID). */
static void cam_read_sensor_name(void)
{
    esp_cam_sensor_id_t chip_id;
    struct v4l2_ext_controls controls;
    struct v4l2_ext_control control[1];

    memset(&chip_id, 0, sizeof(chip_id));
    memset(&controls, 0, sizeof(controls));
    memset(control, 0, sizeof(control));
    controls.ctrl_class = V4L2_CTRL_CLASS_ESP_CAM_IOCTL;
    controls.count = 1;
    controls.controls = control;
    control[0].id = ESP_CAM_SENSOR_IOC_G_CHIP_ID;
    control[0].p_u8 = (uint8_t *)&chip_id;
    control[0].size = sizeof(chip_id);

    const char *name = NULL;
    if (ioctl(s_cap.cam_fd, VIDIOC_G_EXT_CTRLS, &controls) != 0) {
        name = "inconnu";
    } else if (chip_id.pid == OV5647_CHIP_PID) {
        name = "OV5647";
    } else if (chip_id.pid == SC2336_CHIP_PID) {
        name = "SC2336";
    }
    if (name != NULL) {
        strlcpy(s_cap.sensor_name, name, sizeof(s_cap.sensor_name));
    } else {
        int n = snprintf(s_cap.sensor_name, sizeof(s_cap.sensor_name), "PID 0x%04x", (unsigned)chip_id.pid);
        if (n < 0) {
            strlcpy(s_cap.sensor_name, "inconnu", sizeof(s_cap.sensor_name));
        }
    }
}

/**
 * Choisit le format MIPI du capteur : le plus petit couvrant want_w x want_h
 * (cadence ≥ want_fps de préférence), sinon le plus grand disponible.
 */
static esp_err_t cam_select_sensor_format(uint16_t want_w, uint16_t want_h, uint8_t want_fps)
{
    esp_cam_sensor_format_t best, largest;
    bool have_best = false, have_largest = false, best_fps_ok = false;
    uint32_t best_area = 0, largest_area = 0;

    for (uint32_t i = 0; i < 32; i++) {
        struct v4l2_sensor_format_enum e;
        memset(&e, 0, sizeof(e));
        e.index = i;
        if (ioctl(s_cap.cam_fd, VIDIOC_ENUM_SENSOR_FMT, &e) != 0) {
            break;
        }
        const esp_cam_sensor_format_t *f = &e.format;
        if (f->port != ESP_CAM_SENSOR_MIPI_CSI) {
            continue;
        }
        ESP_LOGD(TAG, "format capteur %" PRIu32 " : %s", i, f->name ? f->name : "?");
        uint32_t area = (uint32_t)f->width * f->height;
        if (!have_largest || area > largest_area) {
            largest = *f;
            largest_area = area;
            have_largest = true;
        }
        if (f->width < want_w || f->height < want_h) {
            continue;
        }
        bool fps_ok = f->fps >= want_fps;
        if (!have_best || area < best_area || (area == best_area && fps_ok && !best_fps_ok)) {
            best = *f;
            best_area = area;
            best_fps_ok = fps_ok;
            have_best = true;
        }
    }
    if (!have_largest) {
        ESP_LOGE(TAG, "aucun format MIPI-CSI annoncé par le capteur");
        return ESP_ERR_NOT_FOUND;
    }
    if (!have_best) {
        ESP_LOGW(TAG, "aucun format capteur ≥ %ux%u : %ux%u utilisé", want_w, want_h, largest.width, largest.height);
        best = largest;
    }

    esp_cam_sensor_format_t cur;
    memset(&cur, 0, sizeof(cur));
    bool same = ioctl(s_cap.cam_fd, VIDIOC_G_SENSOR_FMT, &cur) == 0 &&
                cur.width == best.width && cur.height == best.height &&
                cur.fps == best.fps && cur.format == best.format;
    if (!same) {
        ESP_RETURN_ON_FALSE(ioctl(s_cap.cam_fd, VIDIOC_S_SENSOR_FMT, &best) == 0, ESP_FAIL, TAG,
                            "format capteur %s refusé", best.name ? best.name : "?");
    }
    s_cap.sensor_fps = best.fps;
    ESP_LOGI(TAG, "capteur %s : %s (%ux%u, %u i/s)", s_cap.sensor_name, best.name ? best.name : "?",
             best.width, best.height, best.fps);
    return ESP_OK;
}

/** Plus grand diviseur entier de la cadence du capteur ne dépassant pas la cadence demandée. */
static uint8_t pick_fps(uint8_t sensor_fps, uint8_t want)
{
    if (sensor_fps == 0) {
        return want;
    }
    for (uint32_t d = 1; d <= sensor_fps; d++) {
        if (sensor_fps % d == 0 && sensor_fps / d <= want) {
            return (uint8_t)(sensor_fps / d);
        }
    }
    return 1;
}

static esp_err_t cam_set_fps(uint8_t want)
{
    uint8_t fps = pick_fps(s_cap.sensor_fps, want);
    struct v4l2_streamparm sparm;

    memset(&sparm, 0, sizeof(sparm));
    sparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sparm.parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
    sparm.parm.capture.timeperframe.numerator = 1;
    sparm.parm.capture.timeperframe.denominator = fps;
    if (ioctl(s_cap.cam_fd, VIDIOC_S_PARM, &sparm) != 0) {
        ESP_LOGW(TAG, "cadence %u i/s refusée : cadence du capteur (%u i/s)", fps, s_cap.sensor_fps);
        fps = s_cap.sensor_fps ? s_cap.sensor_fps : want;
    }
    s_cap.fps = fps;
    if (fps != want) {
        ESP_LOGW(TAG, "cadence effective %u i/s (demandée %u, capteur %u)", fps, want, s_cap.sensor_fps);
    }
    return ESP_OK;
}

/**
 * Format YUV 4:2:0 de la caméra, recadrage éventuel (première fois) et tampons
 * MMAP. limited : plage 16-235 (vidéo) sinon 0-255 (JPEG).
 */
static esp_err_t cam_configure(bool limited)
{
    if (s_cap.cam_configured && s_cap.cam_limited == limited) {
        return ESP_OK;
    }
    cam_stop();
    s_cap.cam_configured = false;

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(s_cap.cam_fd, VIDIOC_G_FMT, &fmt) == 0, ESP_FAIL, TAG, "lecture du format impossible");
    uint32_t w = fmt.fmt.pix.width;
    uint32_t h = fmt.fmt.pix.height;

    /* Hauteur arrondie à 16 lignes dans la taille du tampon : l'encodeur H.264
     * matériel lit des macroblocs entiers (1080 → 1088 lignes). */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    fmt.fmt.pix.quantization = limited ? V4L2_QUANTIZATION_LIM_RANGE : V4L2_QUANTIZATION_FULL_RANGE;
    fmt.fmt.pix.sizeimage = w * ALIGN_UP(h, 16) * 3 / 2;
    ESP_RETURN_ON_FALSE(ioctl(s_cap.cam_fd, VIDIOC_S_FMT, &fmt) == 0, ESP_FAIL, TAG, "format YUV420 %" PRIu32 "x%" PRIu32 " refusé", w, h);

    if (!s_cap.crop_checked) {
        s_cap.crop_checked = true;
        uint32_t cw = s_cap.cfg.width, ch = s_cap.cfg.height;
        if (cw <= w && ch <= h && (cw < w || ch < h)) {
            /* Recadrage centré par l'ISP (fenêtre à coordonnées paires, voir isp_crop.c). */
            struct v4l2_selection sel;
            memset(&sel, 0, sizeof(sel));
            sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            sel.target = V4L2_SEL_TGT_CROP;
            sel.r.left = (int32_t)(((w - cw) / 2) & ~1u);
            sel.r.top = (int32_t)(((h - ch) / 2) & ~1u);
            sel.r.width = cw;
            sel.r.height = ch;
            if (ioctl(s_cap.cam_fd, VIDIOC_S_SELECTION, &sel) != 0) {
                ESP_LOGW(TAG, "recadrage %" PRIu32 "x%" PRIu32 " impossible : image %" PRIu32 "x%" PRIu32, cw, ch, w, h);
            } else {
                ESP_LOGI(TAG, "recadrage centré %" PRIu32 "x%" PRIu32 " dans %" PRIu32 "x%" PRIu32, cw, ch, w, h);
            }
        }
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(s_cap.cam_fd, VIDIOC_G_FMT, &fmt) == 0, ESP_FAIL, TAG, "lecture du format impossible");
    ESP_RETURN_ON_FALSE(fmt.fmt.pix.width >= 64 && fmt.fmt.pix.height >= 64 &&
                        !(fmt.fmt.pix.width & 1) && !(fmt.fmt.pix.height & 1),
                        ESP_ERR_INVALID_SIZE, TAG, "taille d'image inattendue");
    s_cap.width = (uint16_t)fmt.fmt.pix.width;
    s_cap.height = (uint16_t)fmt.fmt.pix.height;

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = CAM_BUF_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_RETURN_ON_FALSE(ioctl(s_cap.cam_fd, VIDIOC_REQBUFS, &req) == 0, ESP_ERR_NO_MEM, TAG, "tampons caméra refusés");

    for (uint32_t i = 0; i < CAM_BUF_COUNT; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        ESP_RETURN_ON_FALSE(ioctl(s_cap.cam_fd, VIDIOC_QUERYBUF, &buf) == 0, ESP_FAIL, TAG, "QUERYBUF caméra");
        s_cap.cam_buf[i] = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                                           s_cap.cam_fd, buf.m.offset);
        ESP_RETURN_ON_FALSE(s_cap.cam_buf[i] != NULL, ESP_ERR_NO_MEM, TAG, "mmap caméra");
    }
    s_cap.cam_limited = limited;
    s_cap.cam_configured = true;
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Encodeurs mémoire à mémoire (exemple sd_card)                              */
/* ------------------------------------------------------------------------- */

/** Rend le tampon de sortie de l'encodeur (index 0, MMAP) à sa file. */
static esp_err_t m2m_requeue(int fd)
{
    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof(buf));
    buf.index = 0;
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    return ioctl(fd, VIDIOC_QBUF, &buf) == 0 ? ESP_OK : ESP_FAIL;
}

/**
 * Encode une image brute (motif de example_video_fb_get() de l'exemple
 * sd_card). En cas de succès, le tampon codé (index 0) est à rendre par
 * m2m_requeue() après usage.
 */
static esp_err_t m2m_encode(int fd, uint8_t *src, uint32_t src_len, uint32_t *out_len)
{
    struct v4l2_buffer out_buf;
    struct v4l2_buffer cap_buf;

    memset(&out_buf, 0, sizeof(out_buf));
    out_buf.index = 0;
    out_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    out_buf.memory = V4L2_MEMORY_USERPTR;
    out_buf.m.userptr = (unsigned long)src;
    out_buf.length = src_len;
    ESP_RETURN_ON_FALSE(ioctl(fd, VIDIOC_QBUF, &out_buf) == 0, ESP_FAIL, TAG, "QBUF encodeur");

    memset(&cap_buf, 0, sizeof(cap_buf));
    cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cap_buf.memory = V4L2_MEMORY_MMAP;
    bool coded = ioctl(fd, VIDIOC_DQBUF, &cap_buf) == 0;

    memset(&out_buf, 0, sizeof(out_buf));
    out_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    out_buf.memory = V4L2_MEMORY_USERPTR;
    if (ioctl(fd, VIDIOC_DQBUF, &out_buf) != 0) {
        ESP_LOGD(TAG, "DQBUF de l'image source");
    }

    if (!coded) {
        ESP_LOGE(TAG, "encodeur : pas d'image codée");
        return ESP_ERR_TIMEOUT;
    }
    if ((cap_buf.flags & V4L2_BUF_FLAG_ERROR) || cap_buf.bytesused == 0) {
        m2m_requeue(fd);
        ESP_LOGE(TAG, "encodeur : image codée vide");
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_len = cap_buf.bytesused;
    return ESP_OK;
}

static void m2m_stop(int fd)
{
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMOFF, &type);
}

static esp_err_t m2m_start(int fd)
{
    ESP_RETURN_ON_ERROR(m2m_requeue(fd), TAG, "QBUF sortie encodeur");
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL, TAG, "STREAMON sortie encodeur");
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd, VIDIOC_STREAMOFF, &type);
        ESP_LOGE(TAG, "STREAMON entrée encodeur");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * Formats et tampons d'un encodeur M2M : entrée YUV420 w x h en USERPTR (les
 * tampons de la caméra), sortie codée en MMAP, comme example_video_start().
 */
static esp_err_t m2m_setup(int fd, uint32_t w, uint32_t h, uint32_t out_format, uint8_t **out_ptr)
{
    struct v4l2_format format;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width = w;
    format.fmt.pix.height = h;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    ESP_RETURN_ON_FALSE(ioctl(fd, VIDIOC_S_FMT, &format) == 0, ESP_FAIL, TAG, "format d'entrée de l'encodeur refusé");

    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_USERPTR;
    ESP_RETURN_ON_FALSE(ioctl(fd, VIDIOC_REQBUFS, &req) == 0, ESP_FAIL, TAG, "REQBUFS entrée encodeur");

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = w;
    format.fmt.pix.height = h;
    format.fmt.pix.pixelformat = out_format;
    ESP_RETURN_ON_FALSE(ioctl(fd, VIDIOC_S_FMT, &format) == 0, ESP_FAIL, TAG, "format de sortie de l'encodeur refusé");

    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_RETURN_ON_FALSE(ioctl(fd, VIDIOC_REQBUFS, &req) == 0, ESP_ERR_NO_MEM, TAG, "REQBUFS sortie encodeur");

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    ESP_RETURN_ON_FALSE(ioctl(fd, VIDIOC_QUERYBUF, &buf) == 0, ESP_FAIL, TAG, "QUERYBUF sortie encodeur");
    *out_ptr = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
    ESP_RETURN_ON_FALSE(*out_ptr != NULL, ESP_ERR_NO_MEM, TAG, "mmap sortie encodeur");
    return ESP_OK;
}

static void m2m_release(int fd)
{
    struct v4l2_requestbuffers req;

    memset(&req, 0, sizeof(req));
    req.count = 0;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ioctl(fd, VIDIOC_REQBUFS, &req);
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_USERPTR;
    ioctl(fd, VIDIOC_REQBUFS, &req);
}

/** Réglages de l'encodeur H.264 (exemple sd_card : I-period, débit, QP) et cadence. */
static void h264_apply_rate(void)
{
    int fd = s_cap.h264_fd;
    int32_t gop = s_cap.fps < 1 ? 1 : (s_cap.fps > 120 ? 120 : s_cap.fps);
    int64_t bitrate = ((int64_t)s_cap.cfg.bitrate_kbps * 1000 + H264_BITRATE_STEP / 2) / H264_BITRATE_STEP * H264_BITRATE_STEP;
    if (bitrate < H264_BITRATE_MIN) {
        bitrate = H264_BITRATE_MIN;
    } else if (bitrate > H264_BITRATE_MAX) {
        bitrate = H264_BITRATE_MAX;
    }

    set_ctrl(fd, V4L2_CID_CODEC_CLASS, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, gop);
    set_ctrl(fd, V4L2_CID_CODEC_CLASS, V4L2_CID_MPEG_VIDEO_BITRATE, (int32_t)bitrate);
    set_ctrl(fd, V4L2_CID_CODEC_CLASS, V4L2_CID_MPEG_VIDEO_H264_MIN_QP, H264_MIN_QP);
    set_ctrl(fd, V4L2_CID_CODEC_CLASS, V4L2_CID_MPEG_VIDEO_H264_MAX_QP, H264_MAX_QP);

    struct v4l2_streamparm sparm;
    memset(&sparm, 0, sizeof(sparm));
    sparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sparm.parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
    sparm.parm.capture.timeperframe.numerator = 1;
    sparm.parm.capture.timeperframe.denominator = s_cap.fps;
    if (ioctl(fd, VIDIOC_S_PARM, &sparm) != 0) {
        ESP_LOGW(TAG, "cadence de l'encodeur H.264 refusée");
    }
    ESP_LOGI(TAG, "H.264 %ux%u, %u i/s, GOP %" PRId32 ", %" PRId64 " bit/s", s_cap.width, s_cap.height,
             s_cap.fps, gop, bitrate);
}

/** Prépare l'encodeur JPEG pour des images w x h (reconfiguré seulement si la taille change). */
static esp_err_t jpeg_setup(uint16_t w, uint16_t h, uint8_t quality)
{
    set_ctrl(s_cap.jpeg_fd, V4L2_CID_JPEG_CLASS, V4L2_CID_JPEG_COMPRESSION_QUALITY, quality);
    if (s_cap.jpeg_streaming && s_cap.jpeg_w == w && s_cap.jpeg_h == h) {
        return ESP_OK;
    }
    if (s_cap.jpeg_streaming) {
        m2m_stop(s_cap.jpeg_fd);
        s_cap.jpeg_streaming = false;
    }
    ESP_RETURN_ON_ERROR(m2m_setup(s_cap.jpeg_fd, w, h, V4L2_PIX_FMT_JPEG, &s_cap.jpeg_buf), TAG, "encodeur JPEG");
    ESP_RETURN_ON_ERROR(m2m_start(s_cap.jpeg_fd), TAG, "encodeur JPEG");
    s_cap.jpeg_w = w;
    s_cap.jpeg_h = h;
    s_cap.jpeg_streaming = true;
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Incrustation                                                               */
/* ------------------------------------------------------------------------- */

static void overlay_setup(overlay_ctx_t *ctx, uint16_t w, uint16_t h, bool full_range)
{
    ctx->ok = nid_overlay_init(&ctx->ov, w, h, 0, true, full_range);
    ctx->sec = (time_t)-1;
    if (!ctx->ok) {
        ESP_LOGE(TAG, "incrustation impossible pour %ux%u", w, h);
    }
}

/** Texte « AAAA-MM-JJ HH:MM:SS  <identifiants> », heure locale. */
static void overlay_text(time_t now, char *out, size_t len)
{
    struct tm tm;
    char ts[24];

    localtime_r(&now, &tm);
    if (strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm) == 0) {
        strlcpy(ts, "0000-00-00 00:00:00", sizeof(ts));
    }
    int n = snprintf(out, len, "%s  %s", ts, s_cap.cfg.label);
    if (n < 0) {
        strlcpy(out, ts, len);
    }
}

/**
 * Incruste date, heure et identifiants dans l'image brute (texte rendu une
 * fois par seconde), puis écrit le cache vers la PSRAM (C2M) avant la lecture
 * de l'image par un encodeur matériel.
 */
static void overlay_apply(overlay_ctx_t *ctx, uint8_t *frame)
{
    if (!ctx->ok) {
        return;
    }
    time_t now = time(NULL);
    if (now != ctx->sec) {
        char text[NID_OVERLAY_MAX_CHARS + 1];
        overlay_text(now, text, sizeof(text));
        if (nid_overlay_set_text(&ctx->ov, text)) {
            ctx->sec = now;
        }
    }
    nid_overlay_draw(&ctx->ov, frame);
    size_t off, len;
    if (nid_overlay_dirty_range(&ctx->ov, &off, &len)) {
        esp_err_t err = esp_cache_msync(frame + off, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_cache_msync : %s", esp_err_to_name(err));
        }
    }
}

static void overlays_invalidate(void)
{
    s_cap.ov_video.sec = (time_t)-1;
    s_cap.ov_photo.sec = (time_t)-1;
    s_cap.ov_preview.sec = (time_t)-1;
}

/* ------------------------------------------------------------------------- */
/* Jour / nuit                                                                */
/* ------------------------------------------------------------------------- */

/** Saturation de l'ISP : 0 la nuit si l'image monochrome est demandée. Sous s_ctrl_lock. */
static esp_err_t apply_night(void)
{
    bool mono = s_cap.cfg.night && s_cap.cfg.night_mono;
    if (s_cap.isp_fd < 0) {
        return mono ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
    }
    return set_ctrl(s_cap.isp_fd, V4L2_CID_USER_CLASS, V4L2_CID_SATURATION, mono ? 0 : s_cap.day_saturation);
}

/* ------------------------------------------------------------------------- */
/* Aperçu                                                                     */
/* ------------------------------------------------------------------------- */

/**
 * Réduction d'un facteur entier d d'une image YUV 4:2:0 Espressif par
 * décimation : chaque ligne de sortie reprend une ligne source de même parité
 * (U sur les lignes paires, V sur les impaires).
 */
static void downscale_yuv420(const uint8_t *src, uint16_t sw, uint8_t *dst, uint16_t dw, uint16_t dh, uint16_t d)
{
    const size_t sstride = (size_t)sw / 2 * 3;
    const size_t dstride = (size_t)dw / 2 * 3;
    for (uint32_t r = 0; r < dh; r++) {
        uint32_t sr = ((r * d) & ~1u) | (r & 1u);
        const uint8_t *s = src + sr * sstride;
        uint8_t *o = dst + r * dstride;
        for (uint32_t k = 0; k < dw / 2u; k++) {
            uint32_t x0 = 2u * k * d;
            uint32_t x1 = x0 + d;
            o[0] = s[3u * (x0 >> 1)];
            o[1] = s[3u * (x0 >> 1) + 1u + (x0 & 1u)];
            o[2] = s[3u * (x1 >> 1) + 1u + (x1 & 1u)];
            o += 3;
        }
    }
}

/** Arrête la chaîne de l'aperçu (caméra) ; l'aperçu reste demandé et redémarre à la prochaine image. */
static void preview_deactivate(void)
{
    if (s_cap.pv_active) {
        cam_stop();
        s_cap.pv_active = false;
    }
}

static esp_err_t preview_activate(void)
{
    if (s_cap.pv_active) {
        return ESP_OK;
    }
    uint16_t d = 1;
    if (s_cap.pv_max_width > 0 && s_cap.width > s_cap.pv_max_width) {
        d = (uint16_t)((s_cap.width + s_cap.pv_max_width - 1) / s_cap.pv_max_width);
    }
    uint16_t pw = (uint16_t)((s_cap.width / d) & ~1u);
    uint16_t ph = (uint16_t)((s_cap.height / d) & ~1u);
    ESP_RETURN_ON_FALSE(pw >= 64 && ph >= 64, ESP_ERR_INVALID_SIZE, TAG, "aperçu trop petit");

    if (d > 1) {
        size_t need = ALIGN_UP((size_t)pw * ph * 3 / 2, 256);
        if (s_cap.pv_buf == NULL || s_cap.pv_buf_len != need) {
            heap_caps_free(s_cap.pv_buf);
            s_cap.pv_buf = heap_caps_calloc(1, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_CACHE_ALIGNED);
            s_cap.pv_buf_len = s_cap.pv_buf ? need : 0;
            ESP_RETURN_ON_FALSE(s_cap.pv_buf != NULL, ESP_ERR_NO_MEM, TAG, "mémoire de l'aperçu");
        }
        if (!s_cap.ov_preview.ok || s_cap.pv_w != pw || s_cap.pv_h != ph) {
            overlay_setup(&s_cap.ov_preview, pw, ph, true);
        }
    }
    s_cap.pv_div = d;
    s_cap.pv_w = pw;
    s_cap.pv_h = ph;

    ESP_RETURN_ON_ERROR(cam_configure(false), TAG, "caméra (aperçu)");
    ESP_RETURN_ON_ERROR(jpeg_setup(pw, ph, s_cap.pv_quality), TAG, "JPEG (aperçu)");
    ESP_RETURN_ON_ERROR(cam_start(), TAG, "caméra (aperçu)");
    cam_skip(SKIP_STARTUP_FRAME_COUNT);
    s_cap.pv_active = true;
    ESP_LOGI(TAG, "aperçu %ux%u (1/%u), qualité %u", pw, ph, d, s_cap.pv_quality);
    return ESP_OK;
}

/**
 * Attend que le lecteur de l'aperçu rende son image (le tampon JPEG va être
 * réutilisé). Sous verrou ; le verrou est relâché pendant l'attente.
 */
static esp_err_t wait_preview_released(void)
{
    int64_t t0 = esp_timer_get_time();
    while (s_cap.pv_held && esp_timer_get_time() - t0 < PV_RELEASE_WAIT_US) {
        unlock();
        vTaskDelay(pdMS_TO_TICKS(10));
        lock();
    }
    if (s_cap.pv_held) {
        ESP_LOGW(TAG, "image d'aperçu non rendue : reprise forcée");
        m2m_requeue(s_cap.jpeg_fd);
        s_cap.pv_held = false;
    }
    return s_cap.ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* ------------------------------------------------------------------------- */
/* Initialisation / arrêt                                                     */
/* ------------------------------------------------------------------------- */

static void teardown(void)
{
    s_cap.ready = false;

    s_cap.pv_on = false;
    s_cap.pv_active = false;
    s_cap.pv_held = false;
    heap_caps_free(s_cap.pv_buf);
    s_cap.pv_buf = NULL;
    s_cap.pv_buf_len = 0;

    if (s_cap.cam_fd >= 0) {
        cam_stop();
        struct v4l2_requestbuffers req;
        memset(&req, 0, sizeof(req));
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        ioctl(s_cap.cam_fd, VIDIOC_REQBUFS, &req);
    }
    memset(s_cap.cam_buf, 0, sizeof(s_cap.cam_buf));
    s_cap.cam_configured = false;
    s_cap.crop_checked = false;
    close_fd(&s_cap.cam_fd);

    if (s_cap.h264_fd >= 0) {
        m2m_stop(s_cap.h264_fd);
        m2m_release(s_cap.h264_fd);
    }
    close_fd(&s_cap.h264_fd);
    s_cap.h264_buf = NULL;

    if (s_cap.jpeg_fd >= 0) {
        m2m_stop(s_cap.jpeg_fd);
        m2m_release(s_cap.jpeg_fd);
    }
    close_fd(&s_cap.jpeg_fd);
    s_cap.jpeg_buf = NULL;
    s_cap.jpeg_streaming = false;
    s_cap.jpeg_w = s_cap.jpeg_h = 0;

    xSemaphoreTake(s_ctrl_lock, portMAX_DELAY);
    close_fd(&s_cap.isp_fd);
    xSemaphoreGive(s_ctrl_lock);

    if (s_cap.audio_ok) {
        nid_capture_audio_deinit();
        s_cap.audio_ok = false;
    }

    nid_overlay_deinit(&s_cap.ov_video.ov);
    nid_overlay_deinit(&s_cap.ov_photo.ov);
    nid_overlay_deinit(&s_cap.ov_preview.ov);
    s_cap.ov_video.ok = s_cap.ov_photo.ok = s_cap.ov_preview.ok = false;

    if (s_cap.video_inited) {
        /* Détruit capteur, CSI, ISP, encodeurs ; le bus I2C partagé est conservé. */
        esp_err_t err = esp_video_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_video_deinit : %s", esp_err_to_name(err));
        }
        s_cap.video_inited = false;
    }
    strlcpy(s_cap.sensor_name, "aucun", sizeof(s_cap.sensor_name));
}

static void audio_setup(bool wanted)
{
    if (!wanted || s_cap.audio_ok) {
        return;
    }
    esp_err_t err = nid_capture_audio_init();
    s_cap.audio_ok = err == ESP_OK;
    if (!s_cap.audio_ok) {
        ESP_LOGW(TAG, "son indisponible (%s) : vidéo seule", esp_err_to_name(err));
    }
}

/** Initialisation complète (adaptée de example_video_init() et init_capture_video()). */
static esp_err_t setup(const nid_capture_settings_t *s)
{
    s_cap.cfg = *s;

    i2c_master_bus_handle_t bus = nid_board_i2c_bus();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "bus I2C absent (nid_board_i2c_init)");
    const nid_board_pins_t *pins = nid_board_pins();

    /* Bus I2C créé par l'application : CONFIG_EXAMPLE_SCCB_I2C_INIT_BY_APP de l'exemple. */
    esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = bus,
            .freq = SCCB_FREQ_HZ,
        },
        .reset_pin = pins->cam_reset,
        .pwdn_pin = pins->cam_pwdn,
    };
    esp_video_init_config_t cam_config = {
        .csi = &csi_config,
    };
    esp_err_t err = esp_video_init(&cam_config);
    ESP_RETURN_ON_ERROR(err, TAG, "esp_video_init (caméra absente ?)");
    s_cap.video_inited = true;

    /* Caméra (init_capture_video() de l'exemple) */
    s_cap.cam_fd = open(CAM_DEV_PATH, O_RDONLY);
    ESP_RETURN_ON_FALSE(s_cap.cam_fd >= 0, ESP_FAIL, TAG, "ouverture de %s impossible", CAM_DEV_PATH);
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    ESP_RETURN_ON_FALSE(ioctl(s_cap.cam_fd, VIDIOC_QUERYCAP, &cap) == 0, ESP_FAIL, TAG, "QUERYCAP caméra");
    cam_read_sensor_name();

    ESP_RETURN_ON_ERROR(cam_select_sensor_format(s->width, s->height, s->fps), TAG, "format capteur");
    ESP_RETURN_ON_ERROR(cam_set_fps(s->fps), TAG, "cadence");
    ESP_RETURN_ON_ERROR(cam_configure(true), TAG, "caméra");
    s_cap.cam_timeout_ms = 0;
    ESP_RETURN_ON_ERROR(set_dqbuf_timeout(s_cap.cam_fd, CAM_DQBUF_TIMEOUT_MS), TAG, "délai caméra");
    s_cap.cam_timeout_ms = CAM_DQBUF_TIMEOUT_MS;
    if (s_cap.width != s->width || s_cap.height != s->height) {
        ESP_LOGW(TAG, "image %ux%u au lieu de %ux%u", s_cap.width, s_cap.height, s->width, s->height);
    }

    /* Encodeur H.264 (init_codec_video() de l'exemple) */
    s_cap.h264_fd = open(H264_DEV_PATH, O_RDONLY);
    ESP_RETURN_ON_FALSE(s_cap.h264_fd >= 0, ESP_FAIL, TAG, "ouverture de %s impossible", H264_DEV_PATH);
    h264_apply_rate();
    ESP_RETURN_ON_ERROR(m2m_setup(s_cap.h264_fd, s_cap.width, s_cap.height, V4L2_PIX_FMT_H264, &s_cap.h264_buf),
                        TAG, "encodeur H.264");
    set_dqbuf_timeout(s_cap.h264_fd, M2M_DQBUF_TIMEOUT_MS);

    /* Encodeur JPEG, configuré à chaque changement de taille (photo / aperçu) */
    s_cap.jpeg_fd = open(JPEG_DEV_PATH, O_RDONLY);
    ESP_RETURN_ON_FALSE(s_cap.jpeg_fd >= 0, ESP_FAIL, TAG, "ouverture de %s impossible", JPEG_DEV_PATH);
    set_dqbuf_timeout(s_cap.jpeg_fd, M2M_DQBUF_TIMEOUT_MS);

    /* ISP : saturation pour le monochrome de nuit (facultatif) */
    int isp_fd = open(ISP_DEV_PATH, O_RDWR);
    int32_t sat = 128;
    if (isp_fd < 0) {
        ESP_LOGW(TAG, "ISP inaccessible : pas de monochrome de nuit");
    } else if (get_ctrl(isp_fd, V4L2_CID_USER_CLASS, V4L2_CID_SATURATION, &sat) != ESP_OK) {
        sat = 128;
    }
    xSemaphoreTake(s_ctrl_lock, portMAX_DELAY);
    s_cap.isp_fd = isp_fd;
    s_cap.day_saturation = sat;
    apply_night();
    xSemaphoreGive(s_ctrl_lock);

    overlay_setup(&s_cap.ov_video, s_cap.width, s_cap.height, false);
    overlay_setup(&s_cap.ov_photo, s_cap.width, s_cap.height, true);

    if (mp4_muxer_register() != ESP_MUXER_ERR_OK) {
        ESP_LOGE(TAG, "multiplexeur MP4 indisponible");
        return ESP_FAIL;
    }

    audio_setup(s->audio);
    if (s->audio && !s_cap.audio_ok) {
        ESP_LOGW(TAG, "piste son demandée mais indisponible : vidéo seule");
    }

    s_cap.ready = true;
    ESP_LOGI(TAG, "caméra prête : %s, %ux%u à %u i/s, %" PRIu32 " kbit/s, son %s", s_cap.sensor_name,
             s_cap.width, s_cap.height, s_cap.fps, s_cap.cfg.bitrate_kbps, s_cap.audio_ok ? "oui" : "non");
    return ESP_OK;
}

static void sanitize(nid_capture_settings_t *s)
{
    if (s->fps == 0) {
        s->fps = 15;
    } else if (s->fps > 60) {
        s->fps = 60;
    }
    if (s->jpeg_quality == 0) {
        s->jpeg_quality = 80;
    } else if (s->jpeg_quality > 100) {
        s->jpeg_quality = 100;
    }
    if (s->bitrate_kbps == 0) {
        s->bitrate_kbps = 2500;
    }
    s->label[sizeof(s->label) - 1] = '\0';
}

esp_err_t nid_capture_init(const nid_capture_settings_t *s)
{
    ESP_RETURN_ON_FALSE(s != NULL, ESP_ERR_INVALID_ARG, TAG, "réglages absents");
    ESP_RETURN_ON_FALSE(s->width >= 64 && s->height >= 64 && !(s->width & 1) && !(s->height & 1),
                        ESP_ERR_INVALID_ARG, TAG, "résolution %ux%u invalide", s->width, s->height);
    if (s_lock == NULL) {
        /* Premier appel, depuis la tâche principale (avant toute autre fonction de l'API). */
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        s_ctrl_lock = xSemaphoreCreateMutexStatic(&s_ctrl_lock_buf);
        s_mux_lock = xSemaphoreCreateMutexStatic(&s_mux_lock_buf);
    }

    nid_capture_settings_t want = *s;
    sanitize(&want);

    lock();
    esp_err_t err = ESP_OK;
    if (s_cap.ready && want.width == s_cap.cfg.width && want.height == s_cap.cfg.height) {
        /* Mode banc : caméra conservée, seuls les éléments modifiés sont repris. */
        err = wait_preview_released();
        if (err == ESP_OK) {
            bool rate = want.fps != s_cap.cfg.fps || want.bitrate_kbps != s_cap.cfg.bitrate_kbps;
            bool label = strcmp(want.label, s_cap.cfg.label) != 0;
            if (rate) {
                preview_deactivate();
            }
            xSemaphoreTake(s_ctrl_lock, portMAX_DELAY);
            s_cap.cfg = want;
            apply_night();
            xSemaphoreGive(s_ctrl_lock);
            if (rate) {
                cam_set_fps(want.fps);
                h264_apply_rate();
            }
            if (label) {
                overlays_invalidate();
            }
            audio_setup(want.audio);
        }
        unlock();
        return err;
    }

    if (s_cap.ready || s_cap.video_inited) {
        ESP_LOGI(TAG, "réinitialisation de la caméra (%ux%u → %ux%u)", s_cap.cfg.width, s_cap.cfg.height,
                 want.width, want.height);
        wait_preview_released();
        teardown();
    }
    int64_t t0 = esp_timer_get_time();
    err = setup(&want);
    if (err != ESP_OK) {
        teardown();
    } else {
        ESP_LOGI(TAG, "initialisation en %" PRId64 " ms", (esp_timer_get_time() - t0) / 1000);
    }
    unlock();
    return err;
}

bool nid_capture_is_ready(void)
{
    return s_cap.ready;
}

const char *nid_capture_sensor_name(void)
{
    return s_cap.ready ? s_cap.sensor_name : "aucun";
}

esp_err_t nid_capture_set_night(bool night)
{
    if (s_ctrl_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ctrl_lock, portMAX_DELAY);
    s_cap.cfg.night = night;
    esp_err_t err = s_cap.ready ? apply_night() : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_ctrl_lock);
    return err;
}

esp_err_t nid_capture_deinit(void)
{
    if (s_lock == NULL) {
        return ESP_OK;
    }
    lock();
    if (s_cap.ready || s_cap.video_inited) {
        wait_preview_released();
        teardown();
        ESP_LOGI(TAG, "caméra arrêtée");
    }
    unlock();
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Vidéo : H.264 → MP4 (esp_muxer)                                            */
/* ------------------------------------------------------------------------- */

/** Début d'unité NAL suivante (après un code de début 00 00 01) ; *sc_pos = position du code. */
static size_t nal_next(const uint8_t *d, size_t len, size_t pos, size_t *sc_pos)
{
    for (size_t i = pos; i + 3 <= len; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            *sc_pos = (i > pos && d[i - 1] == 0) ? i - 1 : i;
            return i + 3;
        }
    }
    *sc_pos = len;
    return len;
}

/**
 * Parcourt les unités NAL d'une image codée : drapeau d'image IDR et, si spec
 * n'est pas NULL, construction de « code + SPS + code + PPS » (codec_spec_info
 * attendu par esp_muxer pour le H.264).
 */
static bool h264_parse(const uint8_t *d, size_t len, uint8_t *spec, size_t spec_cap, size_t *spec_len)
{
    static const uint8_t start_code[4] = { 0, 0, 0, 1 };
    const uint8_t *sps = NULL, *pps = NULL;
    size_t sps_len = 0, pps_len = 0, sc;
    bool idr = false;

    size_t p = nal_next(d, len, 0, &sc);
    while (p < len) {
        size_t next = nal_next(d, len, p, &sc);
        size_t end = sc;
        while (end > p && d[end - 1] == 0) {
            end--;
        }
        uint8_t type = d[p] & 0x1f;
        if (type == 5) {
            idr = true;
        } else if (type == 7 && sps == NULL) {
            sps = d + p;
            sps_len = end - p;
        } else if (type == 8 && pps == NULL) {
            pps = d + p;
            pps_len = end - p;
        }
        p = next;
    }
    if (spec != NULL) {
        *spec_len = 0;
        if (sps == NULL || pps == NULL || 8 + sps_len + pps_len > spec_cap) {
            return false;
        }
        size_t n = 0;
        memcpy(spec + n, start_code, 4);
        n += 4;
        memcpy(spec + n, sps, sps_len);
        n += sps_len;
        memcpy(spec + n, start_code, 4);
        n += 4;
        memcpy(spec + n, pps, pps_len);
        n += pps_len;
        *spec_len = n;
    }
    return idr;
}

/* Écriture du fichier MP4 : fopen / fwrite / fseek, fflush + fsync avant fclose. */
static void *mp4_on_open(char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "création de %s impossible (errno %d)", path, errno);
        s_rec.write_error = true;
    }
    return f;
}

static int mp4_on_write(void *writer, void *buffer, int len)
{
    if (len <= 0) {
        return 0;
    }
    size_t n = fwrite(buffer, 1, (size_t)len, (FILE *)writer);
    if (n != (size_t)len) {
        s_rec.write_error = true;
        return -1;
    }
    return len;
}

static int mp4_on_seek(void *writer, uint64_t pos)
{
    if (pos > (uint64_t)INT32_MAX || fseek((FILE *)writer, (long)pos, SEEK_SET) != 0) {
        s_rec.write_error = true;
        return -1;
    }
    return 0;
}

static int mp4_on_close(void *writer)
{
    FILE *f = (FILE *)writer;
    int ret = 0;
    if (fflush(f) != 0) {
        ret = -1;
    }
    if (fsync(fileno(f)) != 0) {
        ret = -1;
    }
    if (fclose(f) != 0) {
        ret = -1;
    }
    if (ret == 0) {
        s_rec.closed_ok = true;
    } else {
        s_rec.write_error = true;
        ESP_LOGE(TAG, "fermeture du MP4 : erreur (errno %d)", errno);
    }
    return ret;
}

static esp_muxer_file_writer_t s_mp4_writer = {
    .on_open = mp4_on_open,
    .on_write = mp4_on_write,
    .on_seek = mp4_on_seek,
    .on_close = mp4_on_close,
};

/** Chemin du fichier (un seul segment). */
static int mp4_slice_path(esp_muxer_slice_info_t *info, void *ctx)
{
    rec_t *rc = (rec_t *)ctx;
    if (info->slice_index != 0) {
        ESP_LOGE(TAG, "segment MP4 inattendu");
        return -1;
    }
    int n = snprintf(info->file_path, (size_t)info->len, "%s", rc->path);
    return (n > 0 && n < info->len) ? 0 : -1;
}

/** Trame AAC remise par la tâche audio. */
static void rec_audio_sink(const uint8_t *data, uint32_t len, uint32_t pts_ms, void *ctx)
{
    rec_t *rc = (rec_t *)ctx;
    esp_muxer_audio_packet_t pkt = {
        .data = (void *)data,
        .len = (int)len,
        .pts = pts_ms,
    };
    xSemaphoreTake(s_mux_lock, portMAX_DELAY);
    if (rc->muxer != NULL && rc->audio_idx >= 0) {
        if (esp_muxer_add_audio_packet(rc->muxer, rc->audio_idx, &pkt) != ESP_MUXER_ERR_OK) {
            ESP_LOGD(TAG, "trame audio refusée");
        }
    }
    xSemaphoreGive(s_mux_lock);
}

/** Déclaration des flux à la première image (IDR) : SPS/PPS en codec_spec_info. */
static esp_err_t rec_add_streams(rec_t *rc, const uint8_t *frame, size_t len)
{
    h264_parse(frame, len, rc->spec, sizeof(rc->spec), &rc->spec_len);
    ESP_RETURN_ON_FALSE(rc->spec_len > 0, ESP_ERR_INVALID_RESPONSE, TAG, "première image sans SPS/PPS");

    esp_muxer_video_stream_info_t vi = {
        .codec = ESP_MUXER_VDEC_H264,
        .width = s_cap.width,
        .height = s_cap.height,
        .fps = s_cap.fps,
        .min_packet_duration = 1000u / s_cap.fps,
        .codec_spec_info = rc->spec,
        .spec_info_len = (int)rc->spec_len,
    };
    xSemaphoreTake(s_mux_lock, portMAX_DELAY);
    esp_muxer_err_t me = esp_muxer_add_video_stream(rc->muxer, &vi, &rc->video_idx);
    if (me == ESP_MUXER_ERR_OK && rc->with_audio) {
        esp_muxer_audio_stream_info_t ai = {
            .codec = ESP_MUXER_ADEC_AAC,
            .channel = rc->audio_info.channels,
            .bits_per_sample = rc->audio_info.bits_per_sample,
            .sample_rate = rc->audio_info.sample_rate,
            .min_packet_duration = rc->audio_info.frame_ms,
            .codec_spec_info = (void *)rc->audio_info.spec_info,
            .spec_info_len = (int)rc->audio_info.spec_info_len,
        };
        int idx = -1;
        if (esp_muxer_add_audio_stream(rc->muxer, &ai, &idx) == ESP_MUXER_ERR_OK) {
            rc->audio_idx = idx;
        } else {
            ESP_LOGW(TAG, "flux audio refusé par le multiplexeur : vidéo seule");
        }
    }
    xSemaphoreGive(s_mux_lock);
    ESP_RETURN_ON_FALSE(me == ESP_MUXER_ERR_OK, ESP_FAIL, TAG, "flux vidéo refusé par le multiplexeur (%d)", (int)me);
    rc->streams_added = true;
    return ESP_OK;
}

static esp_err_t record_locked(const char *path, uint32_t duration_ms, nid_capture_stop_cb_t stop_cb, void *arg,
                               nid_capture_stats_t *st)
{
    rec_t *rc = &s_rec;
    memset(rc, 0, sizeof(*rc));
    rc->path = path;
    rc->video_idx = -1;
    rc->audio_idx = -1;

    ESP_RETURN_ON_ERROR(cam_configure(true), TAG, "caméra (vidéo)");

    /* Multiplexeur MP4 (configuration de l'exemple video_muxer, un seul segment) */
    mp4_muxer_config_t mp4_cfg;
    memset(&mp4_cfg, 0, sizeof(mp4_cfg));
    mp4_cfg.base_config.muxer_type = ESP_MUXER_TYPE_MP4;
    mp4_cfg.base_config.slice_duration = ESP_MUXER_MAX_SLICE_DURATION;
    mp4_cfg.base_config.url_pattern_ex = mp4_slice_path;
    mp4_cfg.base_config.ctx = rc;
    mp4_cfg.base_config.ram_cache_size = MUXER_CACHE_SIZE;
    mp4_cfg.display_in_order = true;     /* pas d'images B : dts = pts */
    mp4_cfg.moov_before_mdat = false;
    esp_muxer_handle_t muxer = esp_muxer_open(&mp4_cfg.base_config, sizeof(mp4_cfg));
    ESP_RETURN_ON_FALSE(muxer != NULL, ESP_ERR_NO_MEM, TAG, "ouverture du multiplexeur MP4 impossible");
    if (esp_muxer_set_file_writer(muxer, &s_mp4_writer) != ESP_MUXER_ERR_OK) {
        esp_muxer_close(muxer);
        ESP_LOGE(TAG, "écriture du MP4 : configuration refusée");
        return ESP_FAIL;
    }
    xSemaphoreTake(s_mux_lock, portMAX_DELAY);
    rc->muxer = muxer;
    xSemaphoreGive(s_mux_lock);

    /* Son : démarré avant la caméra (le micro se stabilise), trames remises après la 1re image. */
    if (s_cap.cfg.audio && s_cap.audio_ok) {
        esp_err_t aerr = nid_capture_audio_start(rec_audio_sink, rc, &rc->audio_info);
        rc->with_audio = aerr == ESP_OK;
        if (!rc->with_audio) {
            ESP_LOGW(TAG, "son indisponible (%s) : vidéo seule", esp_err_to_name(aerr));
        }
    } else if (s_cap.cfg.audio) {
        ESP_LOGW(TAG, "son demandé mais indisponible : vidéo seule");
    }

    esp_err_t err = m2m_start(s_cap.h264_fd);
    bool h264_on = err == ESP_OK;
    if (err == ESP_OK) {
        err = cam_start();
    }

    const int64_t frame_us = 1000000LL / s_cap.fps;
    const int64_t limit_us = (int64_t)duration_ms * 1000;
    int64_t t_begin = esp_timer_get_time();
    int64_t last_cb = 0;
    int64_t last_frame = t_begin;
    int64_t t0 = 0;
    int64_t prev_slot = -1;
    uint32_t last_pts = 0;
    int skip = SKIP_STARTUP_FRAME_COUNT;
    bool stalled = false;

    while (err == ESP_OK) {
        int64_t now = esp_timer_get_time();
        if (stop_cb != NULL && now - last_cb >= STOP_CB_PERIOD_US) {
            last_cb = now;
            if (stop_cb(arg)) {
                ESP_LOGW(TAG, "arrêt anticipé demandé");
                break;
            }
        }
        if (rc->streams_added && now - t0 >= limit_us) {
            break;
        }
        if (now - last_frame > CAM_STALL_US) {
            ESP_LOGE(TAG, "plus d'image de la caméra depuis %lld ms", (long long)((now - last_frame) / 1000));
            stalled = true;
            break;
        }

        struct v4l2_buffer cb;
        if (cam_dqbuf(&cb, CAM_DQBUF_TIMEOUT_MS) != ESP_OK) {
            continue;
        }
        int64_t t = esp_timer_get_time();
        last_frame = t;
        if (skip > 0) {
            skip--;
            cam_qbuf(cb.index);
            continue;
        }

        /* Incrustation (obligatoire) puis encodage H.264 */
        uint8_t *frame = s_cap.cam_buf[cb.index];
        overlay_apply(&s_cap.ov_video, frame);
        uint32_t enc_len = 0;
        err = m2m_encode(s_cap.h264_fd, frame, cb.bytesused, &enc_len);
        cam_qbuf(cb.index);
        if (err != ESP_OK) {
            break;
        }

        if (!rc->streams_added) {
            err = rec_add_streams(rc, s_cap.h264_buf, enc_len);
            if (err != ESP_OK) {
                m2m_requeue(s_cap.h264_fd);
                break;
            }
            t0 = t;
            st->first_frame_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (rc->with_audio && rc->audio_idx >= 0) {
                nid_capture_audio_deliver(t0);
            }
        }

        /* Horodatage sur la grille des images : les trous comptent comme images perdues. */
        int64_t slot = (t - t0 + frame_us / 2) / frame_us;
        if (slot <= prev_slot) {
            slot = prev_slot + 1;
        }
        if (prev_slot >= 0 && slot > prev_slot + 1) {
            st->dropped_frames += (uint32_t)(slot - prev_slot - 1);
        }
        prev_slot = slot;
        uint32_t pts = (uint32_t)(slot * 1000 / s_cap.fps);

        esp_muxer_video_packet_t pkt = {
            .data = s_cap.h264_buf,
            .len = (int)enc_len,
            .pts = pts,
            .dts = pts,
            .key_frame = h264_parse(s_cap.h264_buf, enc_len, NULL, 0, NULL),
        };
        xSemaphoreTake(s_mux_lock, portMAX_DELAY);
        esp_muxer_err_t me = esp_muxer_add_video_packet(rc->muxer, rc->video_idx, &pkt);
        xSemaphoreGive(s_mux_lock);
        m2m_requeue(s_cap.h264_fd);
        if (me != ESP_MUXER_ERR_OK || rc->write_error) {
            ESP_LOGE(TAG, "écriture de la vidéo impossible (%d) : carte pleine ou retirée ?", (int)me);
            err = ESP_FAIL;
            break;
        }
        st->video_frames++;
        last_pts = pts;
    }

    /* Arrêt : caméra, encodeur, son, puis fermeture du MP4 (moov, fflush, fsync). */
    cam_stop();
    if (h264_on) {
        m2m_stop(s_cap.h264_fd);
    }
    if (rc->with_audio) {
        st->audio_frames = nid_capture_audio_stop();
    }
    xSemaphoreTake(s_mux_lock, portMAX_DELAY);
    esp_muxer_err_t ce = esp_muxer_close(muxer);
    rc->muxer = NULL;
    xSemaphoreGive(s_mux_lock);

    struct stat sb;
    if (stat(path, &sb) == 0) {
        st->bytes = (uint64_t)sb.st_size;
    }
    if (rc->streams_added) {
        st->duration_ms = last_pts + 1000u / s_cap.fps;
    }
    ESP_LOGI(TAG, "vidéo %s : %" PRIu32 " images (%" PRIu32 " perdues), %" PRIu32 " trames son, %" PRIu32 " ms, %" PRIu64 " octets, %lld ms",
             path, st->video_frames, st->dropped_frames, st->audio_frames, st->duration_ms, st->bytes,
             (long long)((esp_timer_get_time() - t_begin) / 1000));

    if (err != ESP_OK) {
        return err;
    }
    if (!rc->streams_added) {
        ESP_LOGE(TAG, "aucune image enregistrée");
        return ESP_ERR_TIMEOUT;
    }
    if (ce != ESP_MUXER_ERR_OK || rc->write_error || !rc->closed_ok) {
        ESP_LOGE(TAG, "fichier MP4 incomplet (fermeture %d)", (int)ce);
        return ESP_FAIL;
    }
    if (stalled) {
        /* Fichier fermé et lisible : la séquence est conservée, l'incident journalisé par l'appelant. */
        ESP_LOGW(TAG, "séquence écourtée par un arrêt de la caméra");
    }
    return ESP_OK;
}

esp_err_t nid_capture_record(const char *path, uint32_t duration_ms,
                             nid_capture_stop_cb_t stop_cb, void *arg,
                             nid_capture_stats_t *stats)
{
    nid_capture_stats_t st;
    memset(&st, 0, sizeof(st));
    if (stats != NULL) {
        *stats = st;
    }
    ESP_RETURN_ON_FALSE(path != NULL && path[0] != '\0' && duration_ms > 0, ESP_ERR_INVALID_ARG, TAG, "paramètres invalides");
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    esp_err_t err = s_cap.ready ? wait_preview_released() : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        preview_deactivate();
        err = record_locked(path, duration_ms, stop_cb, arg, &st);
    }
    unlock();
    if (stats != NULL) {
        *stats = st;
    }
    return err;
}

/* ------------------------------------------------------------------------- */
/* Photo JPEG                                                                 */
/* ------------------------------------------------------------------------- */

static esp_err_t write_file_sync(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    ESP_RETURN_ON_FALSE(f != NULL, ESP_FAIL, TAG, "création de %s impossible (errno %d)", path, errno);
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fflush(f) == 0) && ok;
    ok = (fsync(fileno(f)) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    ESP_RETURN_ON_FALSE(ok, ESP_FAIL, TAG, "écriture de %s impossible (errno %d)", path, errno);
    return ESP_OK;
}

static esp_err_t photo_locked(const char *path)
{
    ESP_RETURN_ON_ERROR(cam_configure(false), TAG, "caméra (photo)");
    ESP_RETURN_ON_ERROR(jpeg_setup(s_cap.width, s_cap.height, s_cap.cfg.jpeg_quality), TAG, "JPEG (photo)");
    ESP_RETURN_ON_ERROR(cam_start(), TAG, "caméra (photo)");

    cam_skip(PHOTO_SKIP_FRAMES);
    struct v4l2_buffer cb;
    esp_err_t err = ESP_ERR_TIMEOUT;
    int64_t t0 = esp_timer_get_time();
    while (err != ESP_OK && esp_timer_get_time() - t0 < CAM_STALL_US) {
        err = cam_dqbuf(&cb, CAM_DQBUF_TIMEOUT_MS);
    }
    if (err != ESP_OK) {
        cam_stop();
        ESP_LOGE(TAG, "photo : pas d'image de la caméra");
        return err;
    }

    uint8_t *frame = s_cap.cam_buf[cb.index];
    overlay_apply(&s_cap.ov_photo, frame);
    uint32_t len = 0;
    err = m2m_encode(s_cap.jpeg_fd, frame, cb.bytesused, &len);
    cam_qbuf(cb.index);
    cam_stop();
    if (err != ESP_OK) {
        return err;
    }
    err = write_file_sync(path, s_cap.jpeg_buf, len);
    m2m_requeue(s_cap.jpeg_fd);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "photo %s : %ux%u, %" PRIu32 " octets", path, s_cap.width, s_cap.height, len);
    }
    return err;
}

esp_err_t nid_capture_photo(const char *path)
{
    ESP_RETURN_ON_FALSE(path != NULL && path[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "chemin absent");
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    esp_err_t err = s_cap.ready ? wait_preview_released() : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        preview_deactivate();
        err = photo_locked(path);
    }
    unlock();
    return err;
}

/* ------------------------------------------------------------------------- */
/* Aperçu JPEG (installation)                                                 */
/* ------------------------------------------------------------------------- */

esp_err_t nid_capture_preview_start(uint8_t quality, uint16_t max_width)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    esp_err_t err = s_cap.ready ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        uint8_t q = quality == 0 ? 60 : (quality > 100 ? 100 : quality);
        if (s_cap.pv_on && (q != s_cap.pv_quality || max_width != s_cap.pv_max_width)) {
            err = wait_preview_released();
            preview_deactivate();
        }
        s_cap.pv_quality = q;
        s_cap.pv_max_width = max_width;
        s_cap.pv_on = true;
        if (err == ESP_OK) {
            err = preview_activate();
        }
        if (err != ESP_OK) {
            preview_deactivate();
            s_cap.pv_on = false;
        }
    }
    unlock();
    return err;
}

esp_err_t nid_capture_preview_frame(const uint8_t **jpeg, size_t *len, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(jpeg != NULL && len != NULL, ESP_ERR_INVALID_ARG, TAG, "paramètres absents");
    *jpeg = NULL;
    *len = 0;
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    esp_err_t err = ESP_OK;
    if (!s_cap.ready || !s_cap.pv_on) {
        err = ESP_ERR_INVALID_STATE;
    } else if (s_cap.pv_held) {
        ESP_LOGW(TAG, "aperçu : image précédente non rendue (un seul lecteur)");
        err = ESP_ERR_INVALID_STATE;
    } else {
        err = preview_activate();
    }

    struct v4l2_buffer cb;
    if (err == ESP_OK) {
        err = cam_dqbuf_latest(&cb, timeout_ms);
    }
    uint32_t out_len = 0;
    if (err == ESP_OK) {
        uint8_t *frame = s_cap.cam_buf[cb.index];
        if (s_cap.pv_div > 1) {
            /* Réduction par le processeur dans un tampon dédié, puis incrustation à l'échelle de l'aperçu. */
            downscale_yuv420(frame, s_cap.width, s_cap.pv_buf, s_cap.pv_w, s_cap.pv_h, s_cap.pv_div);
            cam_qbuf(cb.index);
            overlay_apply(&s_cap.ov_preview, s_cap.pv_buf);
            size_t used = (size_t)s_cap.pv_w * s_cap.pv_h * 3 / 2;
            esp_cache_msync(s_cap.pv_buf, s_cap.pv_buf_len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            err = m2m_encode(s_cap.jpeg_fd, s_cap.pv_buf, (uint32_t)used, &out_len);
        } else {
            overlay_apply(&s_cap.ov_photo, frame);
            err = m2m_encode(s_cap.jpeg_fd, frame, cb.bytesused, &out_len);
            cam_qbuf(cb.index);
        }
    }
    if (err == ESP_OK) {
        *jpeg = s_cap.jpeg_buf;
        *len = out_len;
        s_cap.pv_held = true;
    }
    unlock();
    return err;
}

void nid_capture_preview_release(void)
{
    if (s_lock == NULL) {
        return;
    }
    lock();
    if (s_cap.pv_held) {
        m2m_requeue(s_cap.jpeg_fd);
        s_cap.pv_held = false;
    }
    unlock();
}

esp_err_t nid_capture_preview_stop(void)
{
    if (s_lock == NULL) {
        return ESP_OK;
    }
    lock();
    if (s_cap.pv_held) {
        m2m_requeue(s_cap.jpeg_fd);
        s_cap.pv_held = false;
    }
    preview_deactivate();
    s_cap.pv_on = false;
    heap_caps_free(s_cap.pv_buf);
    s_cap.pv_buf = NULL;
    s_cap.pv_buf_len = 0;
    unlock();
    return ESP_OK;
}
