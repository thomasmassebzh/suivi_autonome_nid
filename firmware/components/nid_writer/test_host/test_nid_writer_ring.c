/*
 * Tests sur PC du tampon circulaire de nid_writer (nid_writer_ring.h) :
 * simulation d'un producteur (écritures de tailles variées, comme le
 * multiplexeur MP4) et d'un consommateur lent (morceaux de taille fixe, comme
 * la tâche d'écriture), avec retours au début du tampon ; le flux relu doit
 * être identique au flux écrit, octet pour octet.
 *
 * Usage : make -C firmware/components/nid_writer/test_host
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nid_writer_ring.h"

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

static uint32_t s_rng = 12345;
static uint32_t rnd(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return s_rng >> 8;
}

static void test_copy_wrap(void)
{
    uint8_t buf[10];
    uint8_t out[10];
    const uint8_t src[7] = { 1, 2, 3, 4, 5, 6, 7 };
    memset(buf, 0, sizeof(buf));
    size_t pos = nid_ring_copy_in(buf, sizeof(buf), 6, src, 7);
    CHECK(pos == 3, "position après retour : %zu", pos);
    CHECK(buf[6] == 1 && buf[9] == 4 && buf[0] == 5 && buf[2] == 7, "contenu après retour");
    pos = nid_ring_copy_out(buf, sizeof(buf), 6, out, 7);
    CHECK(pos == 3 && memcmp(out, src, 7) == 0, "relecture après retour");
    pos = nid_ring_copy_in(buf, sizeof(buf), 3, src, 7);
    CHECK(pos == 0, "fin exacte du tampon : position %zu", pos);
}

static void test_chunk(void)
{
    CHECK(nid_ring_next_chunk(1000, 4096, 65536) == 1000, "petit morceau");
    CHECK(nid_ring_next_chunk(100000, 4096, 65536) == 2048, "au plus la moitié du tampon");
    CHECK(nid_ring_next_chunk(200000, 4 * 1024 * 1024, 65536) == 65536, "au plus max_chunk");
    CHECK(nid_ring_next_chunk(5, 1, 0) == 1, "tampon minuscule");
}

/* Producteur et consommateur entrelacés, tailles aléatoires, flux vérifié. */
static void test_stream(size_t cap, size_t consumer_chunk, size_t total)
{
    uint8_t *ring = malloc(cap);
    uint8_t *in = malloc(total);
    uint8_t *out = malloc(total);
    uint8_t *tmp = malloc(consumer_chunk);
    for (size_t i = 0; i < total; i++) {
        in[i] = (uint8_t)(rnd() & 0xFF);
    }
    size_t head = 0, tail = 0, used = 0, written = 0, read = 0;
    size_t pending_ops[4096];
    size_t n_ops = 0, op_rd = 0;
    while (read < total) {
        /* Producteur : une écriture de taille aléatoire (paquet MP4), découpée. */
        if (written < total && (rnd() % 3) != 0) {
            size_t want = 1 + rnd() % (cap / 3 + 1);
            if (want > total - written) {
                want = total - written;
            }
            size_t n = nid_ring_next_chunk(want, cap, 65536);
            if (cap - used >= n && n_ops < 4096) {
                head = nid_ring_copy_in(ring, cap, head, in + written, n);
                used += n;
                written += n;
                pending_ops[n_ops++] = n;
            }
        }
        /* Consommateur : traite une opération en morceaux de consumer_chunk. */
        if (op_rd < n_ops && (rnd() % 2) == 0) {
            size_t len = pending_ops[op_rd++];
            while (len > 0) {
                size_t m = len < consumer_chunk ? len : consumer_chunk;
                tail = nid_ring_copy_out(ring, cap, tail, tmp, m);
                used -= m;
                memcpy(out + read, tmp, m);
                read += m;
                len -= m;
            }
        }
    }
    CHECK(used == 0, "tampon vide à la fin (%zu)", used);
    CHECK(head == tail, "tête et queue alignées (%zu / %zu)", head, tail);
    CHECK(memcmp(in, out, total) == 0, "flux identique (capacité %zu, morceaux %zu)", cap, consumer_chunk);
    free(ring);
    free(in);
    free(out);
    free(tmp);
}

int main(void)
{
    test_copy_wrap();
    test_chunk();
    test_stream(4096, 512, 1000000);
    test_stream(4099, 1000, 777777);       /* capacité non multiple des morceaux */
    test_stream(1 << 20, 32768, 20000000); /* proche de l'usage réel */
    printf("%d vérification(s) réussie(s), %d échec(s)\n", s_ok, s_fail);
    return s_fail ? 1 : 0;
}
