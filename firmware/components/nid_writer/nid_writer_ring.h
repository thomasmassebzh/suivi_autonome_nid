/*
 * nid_writer — copies dans un tampon circulaire (calculs purs, testés sur PC)
 *
 * Le tampon circulaire de nid_writer a un seul producteur (la boucle
 * d'enregistrement, via le multiplexeur MP4) et un seul consommateur (la tâche
 * d'écriture). Le producteur écrit à la position « tête », le consommateur lit
 * à la position « queue » ; la comptabilité (octets occupés) est tenue par
 * nid_writer.c sous section critique. Ces fonctions ne font que les copies avec
 * retour au début du tampon.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Copie n octets de src dans le tampon circulaire buf (capacité cap) à partir
 * de la position pos. @return nouvelle position (après les n octets).
 * Précondition : n <= cap, pos < cap.
 */
static inline size_t nid_ring_copy_in(uint8_t *buf, size_t cap, size_t pos, const void *src, size_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    const size_t first = (n <= cap - pos) ? n : cap - pos;
    memcpy(buf + pos, s, first);
    if (n > first) {
        memcpy(buf, s + first, n - first);
    }
    pos += n;
    return pos >= cap ? pos - cap : pos;
}

/**
 * Copie n octets du tampon circulaire buf (capacité cap), à partir de la
 * position pos, vers dst. @return nouvelle position.
 * Précondition : n <= cap, pos < cap.
 */
static inline size_t nid_ring_copy_out(const uint8_t *buf, size_t cap, size_t pos, void *dst, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const size_t first = (n <= cap - pos) ? n : cap - pos;
    memcpy(d, buf + pos, first);
    if (n > first) {
        memcpy(d + first, buf, n - first);
    }
    pos += n;
    return pos >= cap ? pos - cap : pos;
}

/**
 * Découpe d'une écriture : taille du prochain morceau à copier dans le tampon.
 * Un morceau ne dépasse jamais max_chunk (moitié du tampon au plus), pour que
 * le producteur n'attende jamais un espace libre impossible à obtenir.
 */
static inline size_t nid_ring_next_chunk(size_t remaining, size_t cap, size_t max_chunk)
{
    size_t limit = cap / 2;
    if (max_chunk != 0 && max_chunk < limit) {
        limit = max_chunk;
    }
    if (limit == 0) {
        limit = 1;
    }
    return remaining < limit ? remaining : limit;
}

#ifdef __cplusplus
}
#endif
