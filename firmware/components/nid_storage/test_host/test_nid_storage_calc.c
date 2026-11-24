/*
 * Tests sur PC des calculs d'espace de nid_storage (nid_storage_calc.h).
 *
 * Valeurs de référence : volume FAT32 de 120 Go (120 × 10^9 octets) formaté par
 * FatFs (f_mkfs, grappes de 32 ko) puis relu par f_getfree, avec la
 * configuration FatFs d'ESP-IDF v6.1 (secteurs de 512 octets) :
 * n_fatent = 3 661 599, csize = 64, grappes libres = 3 661 596.
 *
 * Usage : make -C firmware/components/nid_storage/test_host
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nid_storage_calc.h"

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

int main(void)
{
    /* Carte de 120 Go en FAT32 */
    const uint32_t n_fatent = 3661599, csize = 64, ssize = 512, free_clst = 3661596;
    const uint64_t total = nid_fat_bytes(nid_fat_clusters(n_fatent), csize, ssize);
    const uint64_t libre = nid_fat_bytes(free_clst, csize, ssize);
    CHECK(total == 119983210496ULL, "total 120 Go : %llu", (unsigned long long)total);
    CHECK(libre == 119983177728ULL, "libre 120 Go : %llu", (unsigned long long)libre);
    /* Le même produit en 32 bits déborde : c'est ce que le calcul 64 bits évite. */
    const uint32_t faux = nid_fat_clusters(n_fatent) * csize * ssize;
    CHECK((uint64_t)faux != total, "le produit 32 bits déborde (%u)", faux);

    /* Grappes de 64 ko (128 secteurs), carte de 256 Go */
    CHECK(nid_fat_bytes(3906000, 128, 512) == 255983616000ULL, "256 Go");
    /* Secteurs de 4096 octets (FF_MAX_SS) */
    CHECK(nid_fat_bytes(1000000, 8, 4096) == 32768000000ULL, "secteurs de 4 ko");
    CHECK(nid_fat_clusters(1) == 0 && nid_fat_clusters(2) == 0 && nid_fat_clusters(10) == 8, "grappes");

    /* Volume plus petit que la carte : première partition de 42 Mo sur une carte de 128 Go */
    CHECK(nid_fat_volume_too_small(44040192ULL, 127865454592ULL), "petite partition détectée");
    CHECK(!nid_fat_volume_too_small(119983210496ULL, 120000000000ULL), "volume = carte");
    CHECK(!nid_fat_volume_too_small(1000, 0), "capacité de la carte inconnue");

    /* Table MBR d'une carte de Raspberry Pi : petite partition FAT + partition Linux. */
    uint8_t s0[512];
    memset(s0, 0, sizeof(s0));
    s0[510] = 0x55;
    s0[511] = 0xAA;
    uint8_t *e = s0 + 446;
    e[4] = 0x0C;                                    /* FAT32 LBA, 42 Mo à partir du secteur 8192 */
    e[8] = 0x00; e[9] = 0x20;                       /* 8192 */
    e[12] = 0x00; e[13] = 0x50; e[14] = 0x01;       /* 86016 secteurs */
    e += 16;
    e[4] = 0x83;                                    /* Linux, 119 Go */
    e[8] = 0x00; e[9] = 0x70; e[10] = 0x01;         /* 94208 */
    e[12] = 0x00; e[13] = 0x00; e[14] = 0x00; e[15] = 0x0E;   /* 234881024 secteurs */
    nid_mbr_part_t p[4];
    const int n = nid_mbr_parse(s0, p);
    CHECK(n == 2, "deux partitions (obtenu %d)", n);
    CHECK(p[0].type == 0x0C && p[0].lba_start == 8192 && p[0].sectors == 86016, "partition FAT");
    CHECK(p[1].type == 0x83 && p[1].sectors == 234881024u, "partition Linux");
    CHECK(nid_mbr_is_fat(p[0].type) && !nid_mbr_is_fat(p[1].type) && !nid_mbr_is_fat(0x07), "types FAT");
    CHECK(strcmp(nid_mbr_type_name(0x07), "exFAT ou NTFS") == 0, "nom exFAT");
    /* Secteur de démarrage FAT32 (carte formatée sans table de partitions) */
    memset(s0, 0, sizeof(s0));
    s0[0] = 0xEB; s0[1] = 0x58; s0[2] = 0x90;
    memcpy(s0 + 82, "FAT32   ", 8);
    s0[510] = 0x55;
    s0[511] = 0xAA;
    CHECK(nid_mbr_parse(s0, p) == -1, "volume sans table de partitions");
    s0[511] = 0x00;
    CHECK(nid_mbr_parse(s0, p) == -1, "signature absente");

    printf("%d vérification(s) réussie(s), %d échec(s)\n", s_ok, s_fail);
    return s_fail ? 1 : 0;
}
