/*
 * nid_writer — écriture de fichier asynchrone, avec tampon en PSRAM
 *
 * Producteur (tâche d'enregistrement) :
 *   nid_writer_write : copie dans le tampon circulaire → opération WRITE en file
 *   nid_writer_seek  : opération SEEK en file (ordre conservé)
 *   nid_writer_open / nid_writer_close : opérations synchrones (attente du résultat)
 *
 * Consommateur (tâche « nid_ecriture ») : traite les opérations dans l'ordre.
 *
 * Écritures alignées sur le secteur (correctif du 27/11) : l'octet de position P
 * du fichier est toujours rangé à l'adresse bounce + (P % 512) + n × 512 du
 * tampon de transfert. Lors d'un write(), FatFs complète le secteur entamé dans
 * son propre tampon, puis envoie les secteurs entiers DIRECTEMENT depuis notre
 * tampon (ff.c, f_write) : l'adresse est alors alignée sur 512, donc sur la
 * ligne de cache (64 o en interne, 128 o en PSRAM), et le pilote SDMMC fait un
 * DMA direct, sans copie ni allocation. Le reste (< 512 o) est gardé et ramené
 * en tête du tampon. Avant ce correctif, une vidange « au repos » laissait la
 * position fichier non alignée : le pilote réservait alors une copie en mémoire
 * interne à chaque écriture (allocate_dma_buf), qui manquait au banc une fois
 * l'encodeur H.264, l'écran et le Wi-Fi démarrés (sdmmc_cmd.c, « not enough mem »).
 *
 * Sur l'ESP32-P4, le contrôleur SDMMC sait faire du DMA depuis la PSRAM
 * (SOC_SDMMC_PSRAM_DMA_CAPABLE) si l'adresse et la longueur sont alignées sur
 * 128 o : le tampon de transfert (64 ko, aligné sur 128) est en PSRAM et ne
 * prend plus aucune mémoire interne. Il est réservé à l'ouverture du fichier,
 * comme le tampon circulaire, donc après l'encodeur. Option
 * CONFIG_NID_WRITER_BOUNCE_INTERNAL : tampon de transfert en mémoire interne
 * DMA, réservé à la création (pour une clé USB si son pilote l'exige).
 *
 * Comptabilité du tampon circulaire : `used` (octets en attente) n'est modifié
 * que sous la section critique `mux` ; `head` n'est touché que par le
 * producteur, `tail` que par le consommateur. Les copies se font hors section
 * critique (zones disjointes).
 */
#include "nid_writer.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
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

#define OP_QUEUE_LEN        1024            /* file en PSRAM : une opération par écriture du multiplexeur */
#define SECTOR              512u
#define BOUNCE_SIZE         (64 * 1024)     /* tampon de transfert : écritures de 128 secteurs */
#define BOUNCE_SIZE_INT     (16 * 1024)     /* variante en mémoire interne (option) */
#define BOUNCE_ALIGN        128             /* ≥ ligne de cache L2 PSRAM (128) et L1 interne (64) */
#define MAX_CHUNK           (64 * 1024)     /* morceau maximal copié d'un coup dans le tampon circulaire */
#define OPEN_TIMEOUT_MS     5000
#define QUIT_TIMEOUT_MS     2000
#define TASK_STACK          4096
#define PATH_MAX_LEN        192

_Static_assert(BOUNCE_SIZE % SECTOR == 0 && BOUNCE_SIZE_INT % SECTOR == 0, "tampon de transfert en secteurs entiers");
_Static_assert(SECTOR % BOUNCE_ALIGN == 0, "secteur multiple de l'alignement DMA");

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
    uint32_t seq;       /* opérations synchrones : numéro attendu en retour */
    uint64_t pos;
} op_t;

struct nid_writer {
    uint8_t          *ring;
    size_t            cap;
    size_t            want;         /* taille demandée du tampon circulaire (réservé à l'ouverture) */
    size_t            head;         /* producteur seulement */
    size_t            tail;         /* consommateur seulement */
    size_t            used;         /* sous mux */
    size_t            peak;         /* sous mux */
    portMUX_TYPE      mux;

    uint8_t          *bounce;
    size_t            bounce_size;
    size_t            lead;         /* consommateur : position fichier % 512 du premier octet en attente */
    size_t            bounce_fill;  /* consommateur : octets en attente dans le tampon de transfert */

    QueueHandle_t     ops;
    SemaphoreHandle_t space;        /* donné par le consommateur après chaque libération */
    SemaphoreHandle_t done;         /* donné après OPEN, CLOSE, QUIT */
    TaskHandle_t      task;
    volatile bool     task_alive;
    uint32_t          seq;          /* producteur : dernier numéro d'opération synchrone */
    volatile uint32_t done_seq;     /* consommateur : numéro de la dernière opération synchrone traitée */

    int               fd;
    char              path[PATH_MAX_LEN];
    volatile esp_err_t result;      /* résultat de la dernière opération synchrone */
    volatile bool     error;

    int64_t           stall_us;
    uint32_t          slowest_ms;
    uint64_t          bytes;
};

/* ------------------------------------------------------------------------- */
/* Tâche d'écriture                                                           */
/* ------------------------------------------------------------------------- */

static void set_error(nid_writer_t *w, const char *what, int err)
{
    if (!w->error) {
        ESP_LOGE(TAG, "%s : %s (errno %d)", what, w->path, err);
    }
    w->error = true;
}

static bool write_all(int fd, const uint8_t *p, size_t n, int *err)
{
    while (n > 0) {
        const ssize_t r = write(fd, p, n);
        if (r <= 0) {
            *err = errno;
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}

/**
 * Écrit ce qui attend dans le tampon de transfert.
 * all = false : jusqu'à la dernière frontière de secteur seulement ; le reste
 * (< 512 o) est ramené en tête du tampon, à l'adresse alignée correspondante.
 * all = true : tout (avant un déplacement ou la fermeture).
 */
static void flush_bounce(nid_writer_t *w, bool all)
{
    const size_t start = w->lead;
    const size_t end = w->lead + w->bounce_fill;
    const size_t stop = all ? end : (end & ~(size_t)(SECTOR - 1));
    if (stop <= start) {
        return;
    }
    if (!w->error && w->fd >= 0) {
        const int64_t t0 = esp_timer_get_time();
        int err = 0;
        if (!write_all(w->fd, w->bounce + start, stop - start, &err)) {
            set_error(w, "écriture impossible (support plein ou retiré ?)", err);
        } else {
            w->bytes += stop - start;
        }
        const uint32_t dt_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        if (dt_ms > w->slowest_ms) {
            w->slowest_ms = dt_ms;
        }
    }
    const size_t rest = end - stop;                 /* 0 si all */
    if (rest > 0) {
        memmove(w->bounce, w->bounce + stop, rest); /* stop multiple de 512 : le reste commence un secteur */
    }
    w->lead = all ? (stop % SECTOR) : 0;
    w->bounce_fill = rest;
}

static void consume_write(nid_writer_t *w, size_t len)
{
    while (len > 0) {
        const size_t room = w->bounce_size - (w->lead + w->bounce_fill);
        if (room == 0) {
            flush_bounce(w, false);
            continue;
        }
        const size_t m = len < room ? len : room;
        w->tail = nid_ring_copy_out(w->ring, w->cap, w->tail, w->bounce + w->lead + w->bounce_fill, m);
        portENTER_CRITICAL(&w->mux);
        w->used -= m;
        portEXIT_CRITICAL(&w->mux);
        xSemaphoreGive(w->space);       /* place libérée : le producteur peut continuer */
        w->bounce_fill += m;
        len -= m;
    }
}

static esp_err_t do_close(nid_writer_t *w)
{
    if (w->fd < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    flush_bounce(w, true);
    bool ok = true;
    int err = 0;
    if (fsync(w->fd) != 0) {
        ok = false;
        err = errno;
    }
    if (close(w->fd) != 0 && ok) {
        ok = false;
        err = errno;
    }
    w->fd = -1;
    if (!ok) {
        set_error(w, "fermeture impossible", err);
    }
    return (ok && !w->error) ? ESP_OK : ESP_FAIL;
}

static void sync_done(nid_writer_t *w, uint32_t seq)
{
    w->done_seq = seq;
    xSemaphoreGive(w->done);
}

static void writer_task(void *arg)
{
    nid_writer_t *w = (nid_writer_t *)arg;
    op_t op;
    for (;;) {
        if (xQueueReceive(w->ops, &op, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (op.type) {
        case OP_OPEN:
            if (w->fd >= 0) {
                close(w->fd);
            }
            w->error = false;
            w->lead = 0;
            w->bounce_fill = 0;
            w->fd = open(w->path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (w->fd < 0) {
                set_error(w, "création impossible", errno);
                w->result = ESP_FAIL;
            } else {
                w->result = ESP_OK;
            }
            sync_done(w, op.seq);
            break;
        case OP_WRITE:
            consume_write(w, op.len);
            break;
        case OP_SEEK:
            flush_bounce(w, true);      /* ordre des écritures et des déplacements conservé */
            if (!w->error && w->fd >= 0) {
                if (op.pos > (uint64_t)INT32_MAX || lseek(w->fd, (off_t)op.pos, SEEK_SET) < 0) {
                    set_error(w, "déplacement impossible", errno);
                }
            }
            w->lead = (size_t)(op.pos % SECTOR);
            break;
        case OP_CLOSE:
            w->result = do_close(w);
            sync_done(w, op.seq);
            break;
        case OP_QUIT:
        default:
            if (w->fd >= 0) {
                close(w->fd);
                w->fd = -1;
            }
            w->task_alive = false;
            w->result = ESP_OK;
            sync_done(w, op.seq);
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
    nid_writer_t *w = heap_caps_calloc(1, sizeof(*w), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (w == NULL) {
        return ESP_ERR_NO_MEM;
    }
    portMUX_INITIALIZE(&w->mux);
    w->want = buffer_bytes;
    w->fd = -1;

#if CONFIG_NID_WRITER_BOUNCE_INTERNAL
    /* Option : tampon de transfert en mémoire interne DMA, réservé maintenant (avant l'encodeur). */
    w->bounce = heap_caps_aligned_alloc(BOUNCE_ALIGN, BOUNCE_SIZE_INT, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    w->bounce_size = BOUNCE_SIZE_INT;
    if (w->bounce == NULL) {
        ESP_LOGE(TAG, "mémoire interne (DMA) insuffisante pour le tampon de transfert");
        free(w);
        return ESP_ERR_NO_MEM;
    }
#endif

    /* File d'opérations en PSRAM : 24 ko qui manqueraient en mémoire interne. */
    w->ops = xQueueCreateWithCaps(OP_QUEUE_LEN, sizeof(op_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    w->space = xSemaphoreCreateBinary();
    w->done = xSemaphoreCreateBinary();
    if (w->ops == NULL || w->space == NULL || w->done == NULL) {
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

/**
 * Opération synchrone : attend que la tâche ait traité CETTE opération (numéro
 * de séquence), pas seulement qu'un « done » ait été donné : une fermeture
 * tardive (délai dépassé) ne peut pas faire croire qu'un QUIT a abouti.
 */
static esp_err_t run_sync(nid_writer_t *w, uint8_t type, uint32_t timeout_ms)
{
    const op_t op = { .type = type, .seq = ++w->seq };
    if (xQueueSend(w->ops, &op, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        w->error = true;
        return ESP_ERR_TIMEOUT;
    }
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (w->done_seq != op.seq) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0 || xSemaphoreTake(w->done, deadline - now) != pdTRUE) {
            if (w->done_seq == op.seq) {
                break;
            }
            w->error = true;
            return ESP_ERR_TIMEOUT;
        }
    }
    return w->result;
}

/** Tampons en PSRAM, réservés à l'ouverture (après l'encodeur H.264). */
static esp_err_t alloc_buffers(nid_writer_t *w)
{
    if (w->bounce == NULL) {
        w->bounce = heap_caps_aligned_alloc(BOUNCE_ALIGN, BOUNCE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        w->bounce_size = BOUNCE_SIZE;
        if (w->bounce == NULL) {
            ESP_LOGE(TAG, "tampon de transfert : PSRAM insuffisante");
            return ESP_ERR_NO_MEM;
        }
    }
    if (w->ring == NULL) {
        size_t sz = w->want;
        w->ring = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        /* PSRAM fragmentée : tampon réduit par moitiés jusqu'à 512 ko (jamais en mémoire interne). */
        while (w->ring == NULL && sz > 512u * 1024u) {
            sz /= 2;
            w->ring = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (w->ring == NULL) {
            ESP_LOGE(TAG, "tampon d'écriture : PSRAM insuffisante");
            return ESP_ERR_NO_MEM;
        }
        if (sz < w->want) {
            ESP_LOGW(TAG, "tampon d'écriture réduit à %u ko (PSRAM fragmentée)", (unsigned)(sz / 1024));
        }
        w->cap = sz;
    }
    return ESP_OK;
}

esp_err_t nid_writer_open(nid_writer_t *w, const char *path)
{
    if (w == NULL || path == NULL || strlen(path) >= sizeof(w->path)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(w->path, path, sizeof(w->path));
    const esp_err_t aerr = alloc_buffers(w);
    if (aerr != ESP_OK) {
        w->error = true;
        return aerr;
    }
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

const char *nid_writer_path(const nid_writer_t *w)
{
    return (w != NULL && w->fd >= 0) ? w->path : NULL;
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
    esp_err_t err = run_sync(w, OP_CLOSE, timeout_ms);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "fermeture de %s : tampon non vidé après %" PRIu32 " ms", w->path, timeout_ms);
    }
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
            /* Tâche bloquée dans une écriture : elle référence les tampons, ils ne sont pas libérés. */
            ESP_LOGE(TAG, "tâche d'écriture bloquée : %u ko non libérés", (unsigned)(w->cap / 1024));
            return;
        }
    }
    free_writer(w);
}
