/*
 * Tests sur PC des calculs purs de nid_capture (nid_capture_calc.h) :
 *  - AudioSpecificConfig AAC-LC (piste son du MP4) ;
 *  - repérage d'un en-tête ADTS ;
 *  - créneaux d'horodatage des images : simulation d'une caméra à 3 tampons
 *    (comme CAM_BUF_COUNT) et d'une boucle d'enregistrement ralentie par les
 *    écritures sur la carte SD ; comparaison de l'ancien calcul (créneau
 *    recalculé sur l'heure de sortie de DQBUF pour toute image) et du nouveau
 *    (recalculé seulement pour une image fraîche) avec la vérité.
 *
 * Usage : make -C firmware/components/nid_capture/test_host
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nid_capture_calc.h"

static int s_fail;
static int s_ok;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (cond) {                                       \
            s_ok++;                                       \
        } else {                                          \
            s_fail++;                                     \
            printf("ÉCHEC %s:%d : ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

/* ------------------------------------------------------------------------- */
/* AAC                                                                        */
/* ------------------------------------------------------------------------- */

static void test_asc(void)
{
    uint8_t a[2];
    CHECK(nid_aac_asc_lc(16000, 1, a) && a[0] == 0x14 && a[1] == 0x08, "16 kHz mono : %02X %02X", a[0], a[1]);
    CHECK(nid_aac_asc_lc(44100, 2, a) && a[0] == 0x12 && a[1] == 0x10, "44,1 kHz stéréo : %02X %02X", a[0], a[1]);
    CHECK(nid_aac_asc_lc(48000, 2, a) && a[0] == 0x11 && a[1] == 0x90, "48 kHz stéréo : %02X %02X", a[0], a[1]);
    CHECK(nid_aac_asc_lc(8000, 1, a) && a[0] == 0x15 && a[1] == 0x88, "8 kHz mono : %02X %02X", a[0], a[1]);
    CHECK(!nid_aac_asc_lc(15000, 1, a), "15 kHz refusé");
    CHECK(!nid_aac_asc_lc(16000, 0, a), "0 voie refusée");
    CHECK(!nid_aac_asc_lc(16000, 8, a), "8 voies refusées");
    CHECK(nid_aac_freq_index(96000) == 0 && nid_aac_freq_index(7350) == 12, "table des fréquences");
}

static void test_adts(void)
{
    /* En-tête ADTS d'une trame de 100 octets, MPEG-4, AAC-LC, 16 kHz, mono, sans CRC. */
    uint8_t f[100] = { 0xFF, 0xF1, 0x60, 0x40, 0x0C, 0x9F, 0xFC };
    CHECK(nid_aac_adts_header_len(f, sizeof(f)) == 7, "ADTS sans CRC : 7 octets");
    f[1] = 0xF0;    /* protection_absent = 0 : CRC */
    CHECK(nid_aac_adts_header_len(f, sizeof(f)) == 9, "ADTS avec CRC : 9 octets");
    f[1] = 0xF1;
    CHECK(nid_aac_adts_header_len(f, 50) == 0, "longueur incohérente : pas un en-tête");
    const uint8_t raw[8] = { 0x21, 0x10, 0x05, 0x00, 0xA0, 0x19, 0x33, 0x87 };
    CHECK(nid_aac_adts_header_len(raw, sizeof(raw)) == 0, "trame brute");
    const uint8_t ff[8] = { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    CHECK(nid_aac_adts_header_len(ff, sizeof(ff)) == 0, "0xFFFF (couche ≠ 0) : pas un en-tête");
}

/* ------------------------------------------------------------------------- */
/* Créneaux d'horodatage                                                      */
/* ------------------------------------------------------------------------- */

#define BUFS        3       /* CAM_BUF_COUNT */
#define DQ_COST_US  50      /* sortie de DQBUF quand une image attend déjà */

typedef struct {
    int     frames;         /* images enregistrées */
    int     true_drops;     /* images non capturées faute de tampon libre */
    int64_t true_last;      /* créneau réel de la dernière image */
    int     drops_old, drops_new;
    int64_t last_old, last_new;
    int64_t max_err_new;    /* écart max |créneau calculé - créneau réel| */
} sim_t;

/**
 * Caméra : une image toutes les frame_us ; elle remplit un tampon libre ou est
 * perdue. Boucle : DQBUF (immédiat si une image attend, sinon attente de la
 * suivante), traitement de proc_us, et une écriture SD de stall_us toutes les
 * stall_every images.
 */
static sim_t simulate(int64_t frame_us, int64_t proc_us, int64_t stall_us, int stall_every, int64_t duration_us)
{
    sim_t r;
    memset(&r, 0, sizeof(r));
    int64_t queue[64];      /* indices des images en file (capturées, pas encore lues) */
    int qn = 0;
    int free_bufs = BUFS;
    int64_t next_frame = 0; /* indice de la prochaine image produite */
    int64_t now = 0;
    int64_t first = -1;
    int64_t prev_old = -1, prev_new = -1, prev_true = -1;
    int64_t t0 = 0;
    int64_t qbuf[BUFS];     /* instants des BUFS dernières remises (QBUF) */
    int nq = 0;

    /* Produit les images jusqu'à l'instant t. */
#define PRODUCE(t)                                         \
    while (next_frame * frame_us <= (t)) {                 \
        if (free_bufs > 0) {                               \
            free_bufs--;                                   \
            queue[qn++] = next_frame;                      \
        }                                                  \
        next_frame++;                                      \
    }

    while (first < 0 || now - t0 < duration_us) {
        PRODUCE(now);
        const int64_t t_call = now;
        if (qn == 0) {
            now = next_frame * frame_us;    /* attente de l'image suivante */
            PRODUCE(now);
        } else {
            now += DQ_COST_US;
            PRODUCE(now);
        }
        const int64_t idx = queue[0];
        memmove(queue, queue + 1, (size_t)(qn - 1) * sizeof(queue[0]));
        qn--;
        const int64_t t = now;
        const bool fresh = nid_capture_frame_is_fresh(t - t_call, frame_us);
        if (first < 0) {
            first = idx;
            t0 = t;
        }
        /* Vérité */
        const int64_t truth = idx - first;
        /* Ancien calcul (avant correctif) */
        int64_t s_old = (t - t0 + frame_us / 2) / frame_us;
        if (s_old <= prev_old) {
            s_old = prev_old + 1;
        }
        if (prev_old >= 0 && s_old > prev_old + 1) {
            r.drops_old += (int)(s_old - prev_old - 1);
        }
        prev_old = s_old;
        /* Nouveau calcul */
        const int64_t release = nq >= BUFS ? qbuf[nq % BUFS] - t0 : NID_SLOT_NO_RELEASE;
        const int64_t s_new = nid_capture_next_slot(prev_new, t - t0, frame_us, fresh, release);
        if (prev_new >= 0 && s_new > prev_new + 1) {
            r.drops_new += (int)(s_new - prev_new - 1);
        }
        prev_new = s_new;
        const int64_t err = s_new > truth ? s_new - truth : truth - s_new;
        if (err > r.max_err_new) {
            r.max_err_new = err;
        }
        prev_true = truth;
        r.frames++;

        /* Traitement (incrustation, encodage), remise du tampon (QBUF), puis écriture
         * sur la carte SD de temps en temps (même ordre que record_locked). */
        now += proc_us;
        PRODUCE(now);
        free_bufs++;    /* QBUF */
        qbuf[nq % BUFS] = now;
        nq++;
        if (stall_every > 0 && r.frames % stall_every == 0) {
            now += stall_us;
            PRODUCE(now);
        }
    }
#undef PRODUCE
    r.true_last = prev_true;
    /* Images perdues entre la première et la dernière image enregistrées (celles perdues
     * après la dernière, pendant l'arrêt de la boucle, ne sont pas dans la vidéo). */
    r.true_drops = (int)(prev_true + 1 - r.frames);
    r.last_old = prev_old;
    r.last_new = prev_new;
    return r;
}

static void report(const char *name, const sim_t *r)
{
    printf("  %-44s images %4d | perdues réelles %3d, ancien calcul %3d, nouveau %3d | "
           "dernier créneau réel %4lld, ancien %4lld, nouveau %4lld\n",
           name, r->frames, r->true_drops, r->drops_old, r->drops_new, (long long)r->true_last,
           (long long)r->last_old, (long long)r->last_new);
}

static void test_slots(void)
{
    printf("Créneaux d'horodatage (simulation, 3 tampons caméra) :\n");
    struct {
        const char *name;
        int64_t frame_us, proc_us, stall_us;
        int every;
    } cases[] = {
        { "30 i/s, 20 ms/image, écriture SD 40 ms /2",   33333, 20000, 40000, 2 },
        { "30 i/s, 45 ms/image (surcharge)",             33333, 45000, 0, 0 },
        { "30 i/s, 30 ms/image, écriture SD 60 ms /3",   33333, 30000, 60000, 3 },
        { "30 i/s, 20 ms/image, écriture SD 25 ms /3",   33333, 20000, 25000, 3 },
        { "30 i/s, 25 ms/image, sans à-coup",            33333, 25000, 0, 0 },
        { "30 i/s, 15 ms/image, écriture SD 300 ms /60", 33333, 15000, 300000, 60 },
        { "15 i/s, 20 ms/image, écriture SD 80 ms /4",   66667, 20000, 80000, 4 },
        { "15 i/s, 30 ms/image, écriture SD 500 ms /30", 66667, 30000, 500000, 30 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const sim_t r = simulate(cases[i].frame_us, cases[i].proc_us, cases[i].stall_us, cases[i].every, 10000000);
        report(cases[i].name, &r);
        CHECK(r.drops_new == r.true_drops, "%s : perdues %d calculées, %d réelles", cases[i].name, r.drops_new,
              r.true_drops);
        CHECK(r.last_new == r.true_last, "%s : dernier créneau %lld, réel %lld", cases[i].name,
              (long long)r.last_new, (long long)r.true_last);
        CHECK(r.max_err_new <= BUFS, "%s : écart max %lld créneaux", cases[i].name, (long long)r.max_err_new);
    }
    /* Cas élémentaires de la fonction */
    const int64_t nr = NID_SLOT_NO_RELEASE;
    CHECK(nid_capture_next_slot(-1, 123456, 33333, true, nr) == 0, "première image : créneau 0");
    CHECK(nid_capture_next_slot(5, 1000000, 33333, false, nr) == 6, "image en retard : créneau suivant");
    CHECK(nid_capture_next_slot(5, 1000000, 33333, true, nr) == 30, "image fraîche après un trou : créneau de l'heure");
    CHECK(nid_capture_next_slot(40, 1000000, 33333, true, nr) == 41, "jamais de retour en arrière");
    CHECK(nid_capture_next_slot(5, 1000000, 33333, false, 400000) == 12, "image en retard : après la remise du tampon");
    CHECK(nid_capture_next_slot(5, 1000000, 33333, false, 50000) == 6, "remise ancienne : créneau suivant");
    CHECK(!nid_capture_frame_is_fresh(50, 33333) && nid_capture_frame_is_fresh(9000, 33333), "seuil d'attente 30 i/s");
    CHECK(!nid_capture_frame_is_fresh(1500, 4000) && nid_capture_frame_is_fresh(2000, 4000), "seuil minimal 2 ms");
}

int main(void)
{
    test_asc();
    test_adts();
    test_slots();
    printf("%d vérification(s) réussie(s), %d échec(s)\n", s_ok, s_fail);
    return s_fail ? 1 : 0;
}
