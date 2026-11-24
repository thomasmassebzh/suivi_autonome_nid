/*
 * nid_storage — calculs purs sur le volume FAT (testés sur PC, test_host/)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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

/* ------------------------------------------------------------------------- */
/* Table de partitions MBR (diagnostic d'une carte mal formatée)              */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t  type;          /*!< type de partition (0x0B/0x0C FAT32, 0x07 exFAT/NTFS, 0x83 Linux...) */
    uint32_t lba_start;     /*!< premier secteur */
    uint32_t sectors;       /*!< nombre de secteurs */
} nid_mbr_part_t;

static inline uint32_t nid_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * Lit la table de partitions du secteur 0 (512 octets).
 * @return nombre d'entrées non vides (0 à 4), copiées dans out dans l'ordre ;
 *         -1 si le secteur n'a pas de table de partitions : signature absente,
 *         ou secteur de démarrage FAT/exFAT (support formaté sans partition).
 */
static inline int nid_mbr_parse(const uint8_t *s0, nid_mbr_part_t out[4])
{
    if (s0 == NULL || s0[510] != 0x55 || s0[511] != 0xAA) {
        return -1;
    }
    const bool jump = (s0[0] == 0xEB && s0[2] == 0x90) || s0[0] == 0xE9;
    if (jump && (memcmp(s0 + 82, "FAT32", 5) == 0 || memcmp(s0 + 54, "FAT", 3) == 0 ||
                 memcmp(s0 + 3, "EXFAT   ", 8) == 0)) {
        return -1;      /* secteur de démarrage d'un volume : pas de table */
    }
    int n = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = s0 + 446 + 16 * i;
        if (e[0] != 0x00 && e[0] != 0x80) {
            return -1;  /* indicateur d'amorçage invalide : pas une table MBR */
        }
        const uint8_t type = e[4];
        const uint32_t start = nid_le32(e + 8);
        const uint32_t count = nid_le32(e + 12);
        if (type == 0 || count == 0) {
            continue;
        }
        if (out != NULL) {
            out[n].type = type;
            out[n].lba_start = start;
            out[n].sectors = count;
        }
        n++;
    }
    return n;
}

/** Nom lisible d'un type de partition MBR. */
static inline const char *nid_mbr_type_name(uint8_t type)
{
    switch (type) {
    case 0x01:
        return "FAT12";
    case 0x04:
    case 0x06:
    case 0x0E:
        return "FAT16";
    case 0x0B:
    case 0x0C:
        return "FAT32";
    case 0x07:
        return "exFAT ou NTFS";
    case 0x82:
        return "Linux swap";
    case 0x83:
        return "Linux";
    case 0xEE:
        return "GPT";
    default:
        return "autre";
    }
}

/** true si le type est une partition FAT lisible par FatFs (sans exFAT). */
static inline bool nid_mbr_is_fat(uint8_t type)
{
    return type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0B || type == 0x0C || type == 0x0E;
}

#ifdef __cplusplus
}
#endif
