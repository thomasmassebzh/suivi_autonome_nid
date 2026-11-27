/*
 * nid_writer — écriture de fichier asynchrone, avec tampon en PSRAM
 *
 * Producteur (tâche d'enregistrement) :
 *   nid_writer_write : copie dans le tampon circulaire → opération WRITE en file
 *   nid_writer_seek  : opération SEEK en file (ordre conservé)
 *   nid_writer_open / nid_writer_close : opérations synchrones (attente du résultat)
 *
 * Consommateur (tâche « nid_ecriture ») : traite les opérations dans l'ordre.
 * Les écritures sont regroupées dans un tampon de transfert en mémoire interne
 * alignée (DMA de la carte SD, transferts USB) : la place est libérée dans le
 * tampon circulaire dès la copie, et le support ne reçoit que des blocs de
 * 16 ko (ou ce qui est en attente avant un déplacement, une fermeture, ou après
 * FLUSH_IDLE_MS sans nouvelle donnée). Le multiplexeur MP4 n'a donc plus besoin
 * de son propre cache en mémoire interne (32 ko qui manquaient au banc).
 *
 * Comptabilité du tampon : `used` (octets en attente) n'est modifié que sous la
 * section critique `mux` ; `head` n'est touché que par le producteur, `tail`
 * que par le consommateur. Un producteur ne copie que dans la zone libre, le
 * consommateur ne lit que la zone occupée : les copies se font hors section
 * critique.
 */
#include "nid_writer.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "nid_writer_ring.h"

static const char *TAG = "nid_writer";

#define OP_QUEUE_LEN        1024            /* file en PSRAM : sans multiplexeur caché, une écriture par paquet */
#define BOUNCE_SIZE         (16 * 1024)     /* tampon de transfert, mémoire interne DMA alignée */
#define BOUNCE_SIZE_MIN     (4 * 1024)
#define FLUSH_IDLE_MS       50              /* données en attente écrites après ce délai sans nouvelle donnée */
#define BOUNCE_ALIGN        64
#define MAX_CHUNK           (64 * 1024)     /* morceau maximal copié d'un coup dans le tampon */
#define FALLBACK_RING_SIZE  (256 * 1024)    /* sans PSRAM */
#define OPEN_TIMEOUT_MS     5000
#define QUIT_TIMEOUT_MS     2000
#define TASK_STACK          4096
#define PATH_MAX_LEN        192

typedef enum {
    OP_OPEN = 0,
    OP_WRITE,
    OP_SEEK,
    OP_CLOSE,
    OP_QUIT,
} op_type_t;

typedef struct {
    uint8_t  type;
    uint32_t len;
    uint64_t pos;
} op_t;

struct nid_writer {
    uint8_t          *ring;
    size_t            cap;
    size_t            want;         /* taille demandée du tampon circulaire (alloué à l'ouverture) */
    size_t            head;         /* producteur seulement */
    size_t            tail;         /* consommateur seulement */
    size_t            used;         /* sous mux */
    size_t            peak;         /* sous mux */
    portMUX_TYPE      mux;

    uint8_t          *bounce;
    size_t            bounce_size;
    size_t            bounce_fill;  /* consommateur seulement : octets en attente d'écriture */

    QueueHandle_t     ops;
    SemaphoreHandle_t space;        /* donné par le consommateur après chaque libération */
    SemaphoreHandle_t done;         /* donné après OPEN, CLOSE, QUIT */
    TaskHandle_t      task;
    bool              task_alive;

    FILE             *f;
    char              path[PATH_MAX_LEN];
    esp_err_t         result;       /* résultat de la dernière opération synchrone */
    volatile bool     error;

    int64_t           stall_us;
    uint32_t          slowest_ms;
    uint64_t          bytes;
};

/* ------------------------------------------------------------------------- */
/* Tâche d'écriture                                                           */
/* ------------------------------------------------------------------------- */

static void set_error(nid_writer_t *w, const char *what)
{
    if (!w->error) {
        ESP_LOGE(TAG, "%s : %s (errno %d)", what, w->path, errno);
    }
    w->error = true;
}

/** Écrit sur le support ce qui attend dans le tampon de transfert. */
static void flush_bounce(nid_writer_t *w)
{
    const size_t m = w->bounce_fill;
    w->bounce_fill = 0;
    if (m == 0 || w->error || w->f == NULL) {
        return;     /* après une erreur : données consommées, plus écrites */
    }
    const int64_t t0 = esp_timer_get_time();
    const size_t n = fwrite(w->bounce, 1, m, w->f);
    const uint32_t dt_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (dt_ms > w->slowest_ms) {
        w->slowest_ms = dt_ms;
    }
    if (n != m) {
        set_error(w, "écriture impossible (support plein ou retiré ?)");
    } else {
        w->bytes += m;
    }
}

static void consume_write(nid_writer_t *w, size_t len)
{
    while (len > 0) {
        const size_t room = w->bounce_size - w->bounce_fill;
        const size_t m = len < room ? len : room;
        w->tail = nid_ring_copy_out(w->ring, w->cap, w->tail, w->bounce + w->bounce_fill, m);
        portENTER_CRITICAL(&w->mux);
        w->used -= m;
        portEXIT_CRITICAL(&w->mux);
        xSemaphoreGive(w->space);       /* place libérée : le producteur peut continuer */
        w->bounce_fill += m;
        len -= m;
        if (w->bounce_fill == w->bounce_size) {
            flush_bounce(w);
        }
    }
}

static esp_err_t do_close(nid_writer_t *w)
{
    if (w->f == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    bool ok = true;
    if (fflush(w->f) != 0) {
        ok = false;
    }
    if (fsync(fileno(w->f)) != 0) {
        ok = false;
    }
    if (fclose(w->f) != 0) {
        ok = false;
    }
    w->f = NULL;
    if (!ok) {
        set_error(w, "fermeture impossible");
    }
    return (ok && !w->error) ? ESP_OK : ESP_FAIL;
}

static void writer_task(void *arg)
{
    nid_writer_t *w = (nid_writer_t *)arg;
    op_t op;
    for (;;) {
        const TickType_t wait = w->bounce_fill > 0 ? pdMS_TO_TICKS(FLUSH_IDLE_MS) : portMAX_DELAY;
        if (xQueueReceive(w->ops, &op, wait) != pdTRUE) {
            flush_bounce(w);            /* plus rien n'arrive : ce qui attend part sur le support */
            continue;
        }
        if (op.type != OP_WRITE) {
            flush_bounce(w);            /* l'ordre des écritures et des déplacements est conservé */
        }
        switch (op.type) {
        case OP_OPEN:
            if (w->f != NULL) {
                fclose(w->f);
            }
            w->error = false;
            w->bounce_fill = 0;
            w->f = fopen(w->path, "wb");
            if (w->f == NULL) {
                set_error(w, "création impossible");
                w->result = ESP_FAIL;
            } else {
                /* Écritures par blocs de BOUNCE_SIZE : pas de tampon newlib supplémentaire. */
                setvbuf(w->f, NULL, _IONBF, 0);
                w->result = ESP_OK;
            }
            xSemaphoreGive(w->done);
            break;
        case OP_WRITE:
            consume_write(w, op.len);
            break;
        case OP_SEEK:
            if (!w->error && w->f != NULL) {
                if (op.pos > (uint64_t)INT32_MAX || fseek(w->f, (long)op.pos, SEEK_SET) != 0) {
                    set_error(w, "déplacement impossible");
                }
            }
            break;
        case OP_CLOSE:
            w->result = do_close(w);
            xSemaphoreGive(w->done);
            break;
        case OP_QUIT:
        default:
            if (w->f != NULL) {
                fclose(w->f);
                w->f = NULL;
            }
            w->task_alive = false;
            xSemaphoreGive(w->done);
            vTaskDelete(NULL);
            return;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* API                                                                        */
/* ------------------------------------------------------------------------- */

static void free_writer(nid_writer_t *w)
{
    if (w->ops) {
        vQueueDeleteWithCaps(w->ops);
    }
    if (w->space) {
        vSemaphoreDelete(w->space);
    }
    if (w->done) {
        vSemaphoreDelete(w->done);
    }
    heap_caps_free(w->ring);
    heap_caps_free(w->bounce);
    free(w);
}

esp_err_t nid_writer_create(size_t buffer_bytes, nid_writer_t **out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (buffer_bytes == 0) {
        buffer_bytes = (size_t)CONFIG_NID_WRITER_BUFFER_KB * 1024u;
    }
    nid_writer_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return ESP_ERR_NO_MEM;
    }
    portMUX_INITIALIZE(&w->mux);
    w->want = buffer_bytes;

    /* Tampon de transfert en mémoire interne capable de DMA : le pilote SD (ou USB)
     * écrit directement depuis lui. En PSRAM, le pilote SD alloue à chaque écriture
     * une copie en mémoire interne, qui manquait au banc une fois l'encodeur H.264
     * démarré (« allocate_dma_buf: not enough mem »). À créer AVANT l'encodeur ;
     * le grand tampon circulaire en PSRAM n'est réservé qu'à l'ouverture du
     * fichier, APRÈS l'encodeur (ses images de référence d'abord). */
    for (size_t sz = BOUNCE_SIZE; sz >= BOUNCE_SIZE_MIN && w->bounce == NULL; sz /= 2) {
        w->bounce = heap_caps_aligned_alloc(BOUNCE_ALIGN, sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        w->bounce_size = sz;
    }
    if (w->bounce == NULL) {
        ESP_LOGE(TAG, "mémoire interne (DMA) insuffisante pour le tampon de transfert");
        free(w);
        return ESP_ERR_NO_MEM;
    }
    if (w->bounce_size < BOUNCE_SIZE) {
        ESP_LOGW(TAG, "tampon de transfert réduit à %u ko (mémoire interne)", (unsigned)(w->bounce_size / 1024));
    }
    /* File d'opérations en PSRAM : 16 ko qui manqueraient en mémoire interne. */
    w->ops = xQueueCreateWithCaps(OP_QUEUE_LEN, sizeof(op_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (w->ops == NULL) {
        w->ops = xQueueCreateWithCaps(OP_QUEUE_LEN / 8, sizeof(op_t), MALLOC_CAP_8BIT);
    }
    w->space = xSemaphoreCreateBinary();
    w->done = xSemaphoreCreateBinary();
    if (w->bounce == NULL || w->ops == NULL || w->space == NULL || w->done == NULL) {
        free_writer(w);
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t core = (CONFIG_NID_WRITER_TASK_CORE < 0) ? tskNO_AFFINITY : CONFIG_NID_WRITER_TASK_CORE;
    w->task_alive = true;
    if (xTaskCreatePinnedToCore(writer_task, "nid_ecriture", TASK_STACK, w, CONFIG_NID_WRITER_TASK_PRIO,
                                &w->task, core) != pdPASS) {
        w->task_alive = false;
        free_writer(w);
        return ESP_ERR_NO_MEM;
    }
    *out = w;
    return ESP_OK;
}

static esp_err_t run_sync(nid_writer_t *w, uint8_t type, uint32_t timeout_ms)
{
    const op_t op = { .type = type };
    xSemaphoreTake(w->done, 0);
    if (xQueueSend(w->ops, &op, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        w->error = true;
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(w->done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        w->error = true;
        return ESP_ERR_TIMEOUT;
    }
    return w->result;
}

/** Tampon circulaire en PSRAM, réduit par moitiés si la PSRAM est fragmentée. */
static esp_err_t alloc_ring(nid_writer_t *w)
{
    if (w->ring != NULL) {
        return ESP_OK;
    }
    size_t sz = w->want;
    w->ring = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    while (w->ring == NULL && sz > 512u * 1024u) {
        sz /= 2;
        w->ring = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (w->ring == NULL) {
        sz = FALLBACK_RING_SIZE;
        w->ring = heap_caps_malloc(sz, MALLOC_CAP_8BIT);
    }
    if (w->ring == NULL) {
        ESP_LOGE(TAG, "tampon d'écriture : mémoire insuffisante");
        return ESP_ERR_NO_MEM;
    }
    if (sz < w->want) {
        ESP_LOGW(TAG, "tampon d'écriture réduit à %u ko (mémoire fragmentée)", (unsigned)(sz / 1024));
    }
    w->cap = sz;
    return ESP_OK;
}

esp_err_t nid_writer_open(nid_writer_t *w, const char *path)
{
    if (w == NULL || path == NULL || strlen(path) >= sizeof(w->path)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t aerr = alloc_ring(w);
    if (aerr != ESP_OK) {
        w->error = true;
        return aerr;
    }
    strlcpy(w->path, path, sizeof(w->path));
    w->head = w->tail = 0;
    portENTER_CRITICAL(&w->mux);
    w->used = 0;
    w->peak = 0;
    portEXIT_CRITICAL(&w->mux);
    w->stall_us = 0;
    w->slowest_ms = 0;
    w->bytes = 0;
    w->result = ESP_FAIL;
    return run_sync(w, OP_OPEN, OPEN_TIMEOUT_MS);
}

int nid_writer_write(nid_writer_t *w, const void *data, size_t len)
{
    if (w == NULL || w->error || w->ring == NULL) {
        return -1;
    }
    const uint8_t *p = (const uint8_t *)data;
    size_t remaining = len;
    const int64_t timeout_us = (int64_t)CONFIG_NID_WRITER_STALL_TIMEOUT_MS * 1000;

    while (remaining > 0) {
        const size_t n = nid_ring_next_chunk(remaining, w->cap, MAX_CHUNK);

        /* Attente de place : seulement si le support est durablement plus lent que le flux. */
        int64_t t_wait = 0;
        for (;;) {
            portENTER_CRITICAL(&w->mux);
            const size_t used = w->used;
            portEXIT_CRITICAL(&w->mux);
            if (w->cap - used >= n) {
                break;
            }
            if (w->error) {
                return -1;
            }
            const int64_t now = esp_timer_get_time();
            if (t_wait == 0) {
                t_wait = now;
            } else if (now - t_wait > timeout_us) {
                ESP_LOGE(TAG, "tampon plein depuis %d ms : support trop lent ou bloqué",
                         CONFIG_NID_WRITER_STALL_TIMEOUT_MS);
                w->error = true;
                return -1;
            }
            xSemaphoreTake(w->space, pdMS_TO_TICKS(50));
        }
        if (t_wait != 0) {
            w->stall_us += esp_timer_get_time() - t_wait;
        }

        w->head = nid_ring_copy_in(w->ring, w->cap, w->head, p, n);
        portENTER_CRITICAL(&w->mux);
        w->used += n;
        if (w->used > w->peak) {
            w->peak = w->used;
        }
        portEXIT_CRITICAL(&w->mux);

        const op_t op = { .type = OP_WRITE, .len = (uint32_t)n };
        if (xQueueSend(w->ops, &op, pdMS_TO_TICKS(CONFIG_NID_WRITER_STALL_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "file d'opérations pleine");
            w->error = true;
            return -1;
        }
        p += n;
        remaining -= n;
    }
    return (int)len;
}

int nid_writer_seek(nid_writer_t *w, uint64_t pos)
{
    if (w == NULL || w->error) {
        return -1;
    }
    const op_t op = { .type = OP_SEEK, .pos = pos };
    if (xQueueSend(w->ops, &op, pdMS_TO_TICKS(CONFIG_NID_WRITER_STALL_TIMEOUT_MS)) != pdTRUE) {
        w->error = true;
        return -1;
    }
    return 0;
}

esp_err_t nid_writer_close(nid_writer_t *w, uint32_t timeout_ms)
{
    if (w == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = run_sync(w, OP_CLOSE, timeout_ms);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "fermeture de %s : tampon non vidé après %" PRIu32 " ms", w->path, timeout_ms);
    }
    ESP_LOGD(TAG, "fermeture en %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
    return err;
}

bool nid_writer_failed(const nid_writer_t *w)
{
    return w == NULL || w->error;
}

void nid_writer_get_stats(const nid_writer_t *w, nid_writer_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (w == NULL) {
        return;
    }
    nid_writer_t *mw = (nid_writer_t *)w;
    portENTER_CRITICAL(&mw->mux);
    out->peak_bytes = w->peak;
    portEXIT_CRITICAL(&mw->mux);
    out->buffer_bytes = w->cap;
    out->stall_ms = (uint32_t)(w->stall_us / 1000);
    out->slowest_write_ms = w->slowest_ms;
    out->bytes = w->bytes;
    out->error = w->error;
}

void nid_writer_destroy(nid_writer_t *w)
{
    if (w == NULL) {
        return;
    }
    if (w->task_alive) {
        if (run_sync(w, OP_QUIT, QUIT_TIMEOUT_MS) == ESP_ERR_TIMEOUT && w->task_alive) {
            /* Tâche bloquée dans une écriture : elle référence le tampon, il n'est pas libéré. */
            ESP_LOGE(TAG, "tâche d'écriture bloquée : %u ko non libérés", (unsigned)(w->cap / 1024));
            return;
        }
    }
    free_writer(w);
}
