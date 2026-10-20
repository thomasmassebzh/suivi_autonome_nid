/*
 * banc_web_vue — vue caméra en direct de la page web (MJPEG)
 *
 * Chemin d'une image : crochet de nid_capture (banc_crochet_image : viseur,
 * vidéo, photo) → banc_web_image() : réduction et conversion YUV 4:2:0 →
 * RGB565 par le PPA dans un tampon dédié (quelques ms, seulement si une page
 * regarde, à cadence limitée, image sautée si le tampon est encore occupé :
 * l'enregistrement n'attend jamais) → tâche « banc_vue_jpeg » : encodeur JPEG
 * matériel (esp_driver_jpeg, partagé avec esp_video sous son verrou) dans l'un
 * des deux tampons JPEG → tâche « banc_vue_flux » : envoi multipart.
 *
 * Cadence : CONFIG_BANC_WEB_VUE_IPS hors enregistrement, réduite à
 * CONFIG_BANC_WEB_VUE_ENREG_IPS pendant un enregistrement (0 : aucune image),
 * comme l'écran, pour ne pas faire perdre d'images à la vidéo.
 *
 * Tampons JPEG protégés (défaut 25 de docs/revue_cdcf.md) : l'image en cours
 * d'envoi (« envoi ») n'est jamais réécrite ; l'encodeur écrit dans l'autre,
 * et retire « dernier » s'il le réécrit, sous la section critique s_mux.
 *
 * Un seul appareil à la fois : un second reçoit « 503 vue déjà ouverte » ;
 * le même appareil (page rechargée) reprend la vue. Sans image pendant
 * FLUX_SANS_IMAGE_US, le flux se termine (la page propose de réessayer).
 */
#include "banc.h"

#include "sdkconfig.h"

#if CONFIG_BANC_F_WIFI

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "banc_web.h"

static const char *TAG = "banc_vue";

#define JPEG_MAX            (192 * 1024)
#define ENC_PILE            3072
#define ENC_PRIO            3
#define FLUX_PILE           4096
#define FLUX_PRIO           3
#define FRONTIERE           "bancimage"
#define ATTENTE_IMAGE_MS    500
#define FLUX_SANS_IMAGE_US  (10LL * 1000000LL)
#define JPEG_DELAI_MS       200

static struct {
    bool                  pret;
    bool                  init_faite;
    ppa_client_handle_t   ppa;
    jpeg_encoder_handle_t jpeg;
    uint8_t              *rgb;          /*!< image réduite RGB565 (sortie du PPA, entrée du JPEG) */
    size_t                rgb_taille;
    uint16_t              rgb_w, rgb_h;
    atomic_bool           rgb_plein;    /*!< image à encoder (le crochet n'y écrit plus) */
    uint8_t              *jpg[2];
    size_t                jpg_taille;
    size_t                jpg_len[2];
    int                   envoi;        /*!< tampon en cours d'envoi, -1 : aucun (sous s_mux) */
    int                   dernier;      /*!< dernière image complète, -1 : aucune (sous s_mux) */
    uint32_t              seq;          /*!< numéro de la dernière image (sous s_mux) */
    int64_t               t_image_us;   /*!< dernière image prise par le crochet */
    bool                  erreur_ppa, erreur_jpeg;
    TaskHandle_t          enc;
    SemaphoreHandle_t     nouvelle;     /*!< une image JPEG est prête */
    atomic_bool           client;       /*!< une page regarde */
    /* Session (sous verrou) */
    SemaphoreHandle_t     verrou;
    TaskHandle_t          flux;
    httpd_req_t          *attente;
    struct sockaddr_storage pair_attente, pair_session;
    bool                  session;
    uint32_t              envoyees;
    uint32_t              ips10;
} s_v = {
    .envoi = -1,
    .dernier = -1,
};
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ------------------------------------------------------------------------- */
/* Crochet d'image (tâche d'action ou d'enregistrement)                        */
/* ------------------------------------------------------------------------- */

bool banc_web_vue_active(void)
{
    return atomic_load(&s_v.client);
}

void banc_web_image(const nid_capture_frame_t *f)
{
    if (!s_v.pret || !atomic_load(&s_v.client) || f == NULL || f->data == NULL || f->width < 32 || f->height < 32) {
        return;
    }
    const int ips = f->recording ? CONFIG_BANC_WEB_VUE_ENREG_IPS : CONFIG_BANC_WEB_VUE_IPS;
    if (ips <= 0) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (now - s_v.t_image_us < 1000000LL / ips || atomic_load(&s_v.rgb_plein)) {
        return;     /* cadence atteinte, ou image précédente pas encore encodée : image sautée */
    }
    /* Échelle k/16 (pas du PPA), image réduite aux multiples de 16 pixels (blocs JPEG). */
    uint32_t k = 16u * CONFIG_BANC_WEB_VUE_LARGEUR / f->width;
    if (k > 16u) {
        k = 16u;
    }
    if (k == 0) {
        return;
    }
    const uint32_t ow = (f->width * k / 16u) & ~15u;
    const uint32_t oh = (f->height * k / 16u) & ~15u;
    if (ow < 16u || oh < 16u || (size_t)ow * oh * 2u > s_v.rgb_taille) {
        return;
    }
    /* Bloc source centré (dimensions paires) dont la réduction tient dans ow x oh. */
    uint32_t bw = (ow * 16u + k - 1u) / k;
    uint32_t bh = (oh * 16u + k - 1u) / k;
    bw = (bw > f->width ? f->width : bw) & ~1u;
    bh = (bh > f->height ? f->height : bh) & ~1u;
    const ppa_srm_oper_config_t op = {
        .in = {
            .buffer = f->data,
            .pic_w = f->width,
            .pic_h = f->height,
            .block_w = bw,
            .block_h = bh,
            .block_offset_x = ((f->width - bw) / 2u) & ~1u,
            .block_offset_y = ((f->height - bh) / 2u) & ~1u,
            .srm_cm = PPA_SRM_COLOR_MODE_YUV420,    /* O_UYY_E_VYY, format de l'ISP */
            .yuv_range = f->full_range ? PPA_COLOR_RANGE_FULL : PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = s_v.rgb,
            .buffer_size = (uint32_t)s_v.rgb_taille,
            .pic_w = ow,
            .pic_h = oh,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = (float)k / 16.0f,
        .scale_y = (float)k / 16.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    const esp_err_t err = ppa_do_scale_rotate_mirror(s_v.ppa, &op);
    if (err != ESP_OK) {
        if (!s_v.erreur_ppa) {
            s_v.erreur_ppa = true;
            ESP_LOGW(TAG, "réduction PPA %ux%u → %" PRIu32 "x%" PRIu32 " refusée : %s", f->width, f->height, ow, oh,
                     esp_err_to_name(err));
        }
        return;
    }
    s_v.rgb_w = (uint16_t)ow;
    s_v.rgb_h = (uint16_t)oh;
    s_v.t_image_us = now;
    atomic_store(&s_v.rgb_plein, true);
    xTaskNotifyGive(s_v.enc);
}

/* ------------------------------------------------------------------------- */
/* Encodage JPEG                                                              */
/* ------------------------------------------------------------------------- */

static void tache_encodage(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!atomic_load(&s_v.rgb_plein)) {
            continue;
        }
        /* Tampon de sortie : jamais celui en cours d'envoi. */
        portENTER_CRITICAL(&s_mux);
        const int i = (s_v.envoi == 0) ? 1 : (s_v.envoi == 1) ? 0 : (s_v.dernier == 0 ? 1 : 0);
        if (s_v.dernier == i) {
            s_v.dernier = -1;
        }
        portEXIT_CRITICAL(&s_mux);

        const jpeg_encode_cfg_t cfg = {
            .width = s_v.rgb_w,
            .height = s_v.rgb_h,
            .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
            .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
            .image_quality = CONFIG_BANC_WEB_VUE_QUALITE,
        };
        uint32_t taille = 0;
        const esp_err_t err = jpeg_encoder_process(s_v.jpeg, &cfg, s_v.rgb, (uint32_t)s_v.rgb_w * s_v.rgb_h * 2u,
                                                   s_v.jpg[i], (uint32_t)s_v.jpg_taille, &taille);
        atomic_store(&s_v.rgb_plein, false);
        if (err != ESP_OK || taille == 0) {
            if (!s_v.erreur_jpeg) {
                s_v.erreur_jpeg = true;
                ESP_LOGW(TAG, "encodage JPEG de la vue %ux%u : %s", s_v.rgb_w, s_v.rgb_h, esp_err_to_name(err));
            }
            continue;
        }
        portENTER_CRITICAL(&s_mux);
        s_v.jpg_len[i] = taille;
        s_v.dernier = i;
        s_v.seq++;
        portEXIT_CRITICAL(&s_mux);
        xSemaphoreGive(s_v.nouvelle);
    }
}

/* ------------------------------------------------------------------------- */
/* Flux MJPEG                                                                 */
/* ------------------------------------------------------------------------- */

static bool nouvelle_demande(void)
{
    xSemaphoreTake(s_v.verrou, portMAX_DELAY);
    const bool d = (s_v.attente != NULL);
    xSemaphoreGive(s_v.verrou);
    return d;
}

static void session(httpd_req_t *req)
{
    atomic_store(&s_v.client, true);
    xSemaphoreTake(s_v.nouvelle, 0);
    portENTER_CRITICAL(&s_mux);
    uint32_t vu = s_v.seq;
    portEXIT_CRITICAL(&s_mux);
    bool entete = false;
    bool vivant = true;
    int64_t derniere = esp_timer_get_time();
    const int64_t debut = derniere;
    uint32_t envoyees = 0;
    char part[96];

    while (vivant && !nouvelle_demande()) {
        if (xSemaphoreTake(s_v.nouvelle, pdMS_TO_TICKS(ATTENTE_IMAGE_MS)) != pdTRUE) {
            if (esp_timer_get_time() - derniere > FLUX_SANS_IMAGE_US) {
                ESP_LOGW(TAG, "vue web : aucune image depuis %lld s (caméra ?), flux terminé",
                         (long long)(FLUX_SANS_IMAGE_US / 1000000LL));
                break;
            }
            continue;
        }
        /* Image la plus récente, protégée pendant son envoi. */
        portENTER_CRITICAL(&s_mux);
        const int i = s_v.dernier;
        size_t len = 0;
        if (i >= 0 && s_v.seq != vu) {
            s_v.envoi = i;
            len = s_v.jpg_len[i];
            vu = s_v.seq;
        }
        portEXIT_CRITICAL(&s_mux);
        if (len == 0) {
            continue;
        }
        if (!entete) {
            httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" FRONTIERE);
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            entete = true;
        }
        const int n = snprintf(part, sizeof(part),
                               "\r\n--" FRONTIERE "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                               (unsigned)len);
        esp_err_t err = (n > 0 && (size_t)n < sizeof(part)) ? httpd_resp_send_chunk(req, part, n) : ESP_FAIL;
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, (const char *)s_v.jpg[i], (ssize_t)len);
        }
        portENTER_CRITICAL(&s_mux);
        s_v.envoi = -1;
        portEXIT_CRITICAL(&s_mux);
        if (err != ESP_OK) {
            vivant = false;     /* page fermée ou appareil parti */
            break;
        }
        derniere = esp_timer_get_time();
        envoyees++;
        s_v.envoyees++;
        const int64_t ms = (derniere - debut) / 1000;
        if (ms > 1000) {
            s_v.ips10 = (uint32_t)((int64_t)envoyees * 10000 / ms);
        }
    }
    atomic_store(&s_v.client, false);
    if (!entete) {
        banc_web_texte(req, BANC_HTTP_503, "Pas d'image de la caméra (fonction caméra désactivée ou en défaut ?)");
    } else if (vivant) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    httpd_req_async_handler_complete(req);
    ESP_LOGI(TAG, "vue web fermée : %" PRIu32 " image(s) envoyée(s)", envoyees);
}

static void tache_flux(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_v.verrou, portMAX_DELAY);
        httpd_req_t *req = s_v.attente;
        s_v.attente = NULL;
        if (req != NULL) {
            s_v.session = true;
            s_v.pair_session = s_v.pair_attente;
        }
        xSemaphoreGive(s_v.verrou);
        if (req == NULL) {
            continue;
        }
        ESP_LOGI(TAG, "vue web ouverte");
        session(req);
        xSemaphoreTake(s_v.verrou, portMAX_DELAY);
        s_v.session = false;
        const bool encore = (s_v.attente != NULL);
        xSemaphoreGive(s_v.verrou);
        if (encore) {
            xTaskNotifyGive(s_v.flux);  /* même appareil : reprise immédiate */
        }
    }
}

esp_err_t banc_vue_get(httpd_req_t *req)
{
    banc_web_compter();
    if (!banc_f_active(BANC_F_CAMERA)) {
        const char *t = banc_f_texte(BANC_F_CAMERA);
        char msg[64];
        snprintf(msg, sizeof(msg), "Caméra %s : pas de vue", t ? t : "indisponible");
        return banc_web_texte(req, BANC_HTTP_503, msg);
    }
    if (!s_v.pret) {
        return banc_web_texte(req, BANC_HTTP_503, "Vue indisponible (mémoire, PPA ou encodeur JPEG : voir le journal)");
    }
    struct sockaddr_storage pair;
    banc_web_pair(req, &pair);
    xSemaphoreTake(s_v.verrou, portMAX_DELAY);
    if (s_v.session && !banc_web_meme_pair(&pair, &s_v.pair_session)) {
        xSemaphoreGive(s_v.verrou);
        return banc_web_texte(req, BANC_HTTP_503, "Vue déjà ouverte sur un autre appareil : une seule à la fois. "
                              "La fermer, puis réessayer.");
    }
    httpd_req_t *copie = NULL;
    if (httpd_req_async_handler_begin(req, &copie) != ESP_OK) {
        xSemaphoreGive(s_v.verrou);
        return banc_web_texte(req, BANC_HTTP_503, "Serveur occupé : réessayer");
    }
    httpd_req_t *ancienne = s_v.attente;
    s_v.attente = copie;
    s_v.pair_attente = pair;
    xSemaphoreGive(s_v.verrou);
    if (ancienne != NULL) {
        banc_web_texte(ancienne, BANC_HTTP_503, "Remplacée par une demande plus récente");
        httpd_req_async_handler_complete(ancienne);
    }
    xTaskNotifyGive(s_v.flux);
    return ESP_OK;
}

void banc_vue_stats(uint32_t *envoyees, uint32_t *ips10)
{
    *envoyees = s_v.envoyees;
    *ips10 = atomic_load(&s_v.client) ? s_v.ips10 : 0;
}

/* ------------------------------------------------------------------------- */
/* Initialisation                                                             */
/* ------------------------------------------------------------------------- */

esp_err_t banc_vue_init(void)
{
    if (s_v.init_faite) {
        return s_v.pret ? ESP_OK : ESP_FAIL;
    }
    s_v.init_faite = true;
    s_v.verrou = xSemaphoreCreateMutex();
    s_v.nouvelle = xSemaphoreCreateBinary();
    if (s_v.verrou == NULL || s_v.nouvelle == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* Image réduite : au plus LARGEUR x 3/4 LARGEUR (capteurs 16:9 et 4:3). */
    const uint32_t l = CONFIG_BANC_WEB_VUE_LARGEUR;
    const jpeg_encode_memory_alloc_cfg_t entree = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    const jpeg_encode_memory_alloc_cfg_t sortie = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t taille = 0;
    s_v.rgb = jpeg_alloc_encoder_mem((size_t)l * (l * 3u / 4u) * 2u, &entree, &taille);
    s_v.rgb_taille = taille;
    s_v.jpg[0] = jpeg_alloc_encoder_mem(JPEG_MAX, &sortie, &s_v.jpg_taille);
    s_v.jpg[1] = jpeg_alloc_encoder_mem(JPEG_MAX, &sortie, &taille);
    if (s_v.rgb == NULL || s_v.jpg[0] == NULL || s_v.jpg[1] == NULL) {
        ESP_LOGE(TAG, "vue web : mémoire insuffisante (PSRAM)");
        return ESP_ERR_NO_MEM;
    }
    if (taille < s_v.jpg_taille) {
        s_v.jpg_taille = taille;
    }
    const ppa_client_config_t pcfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    esp_err_t err = ppa_register_client(&pcfg, &s_v.ppa);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "vue web : PPA %s", esp_err_to_name(err));
        return err;
    }
    const jpeg_encode_engine_cfg_t jcfg = { .timeout_ms = JPEG_DELAI_MS };
    err = jpeg_new_encoder_engine(&jcfg, &s_v.jpeg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "vue web : encodeur JPEG %s", esp_err_to_name(err));
        return err;
    }
    if (xTaskCreate(tache_encodage, "banc_vue_jpeg", ENC_PILE, NULL, ENC_PRIO, &s_v.enc) != pdPASS ||
        xTaskCreate(tache_flux, "banc_vue_flux", FLUX_PILE, NULL, FLUX_PRIO, &s_v.flux) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_v.pret = true;
    ESP_LOGI(TAG, "vue web prête : %" PRIu32 " px de large, %d i/s (%d pendant un enregistrement), qualité %d", l,
             CONFIG_BANC_WEB_VUE_IPS, CONFIG_BANC_WEB_VUE_ENREG_IPS, CONFIG_BANC_WEB_VUE_QUALITE);
    return ESP_OK;
}

#endif /* CONFIG_BANC_F_WIFI */
