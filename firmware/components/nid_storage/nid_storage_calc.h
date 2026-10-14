/*
 * nid_storage — calculs purs sur le volume FAT (testés sur PC, test_host/)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Octets de n grappes : tout le calcul en 64 bits. Une carte de 128 Go compte
 * environ 2,5 × 10^11 octets : un produit intermédiaire en 32 bits déborde
 * (au-delà de 4 Gio) et donne une valeur fausse.
 */
static inline uint64_t nid_fat_bytes(uint32_t clusters, uint32_t sectors_per_cluster, uint32_t sector_size)
{
    return (uint64_t)clusters * (uint64_t)sectors_per_cluster * (uint64_t)sector_size;
}

/**
 * Nombre de grappes de données d'un volume FAT (FATFS.n_fatent = grappes + 2).
 */
static inline uint32_t nid_fat_clusters(uint32_t n_fatent)
{
    return n_fatent >= 2 ? n_fatent - 2 : 0;
}

/**
 * true si le volume FAT monté couvre moins de 90 % de la carte : carte à
 * plusieurs partitions (seule la première partition FAT est montée), ou
 * volume créé plus petit que la carte.
 */
static inline bool nid_fat_volume_too_small(uint64_t volume_bytes, uint64_t card_bytes)
{
    return card_bytes > 0 && volume_bytes < card_bytes / 10 * 9;
}

#ifdef __cplusplus
}
#endif
