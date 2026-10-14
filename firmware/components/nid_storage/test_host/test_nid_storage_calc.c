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

    printf("%d vérification(s) réussie(s), %d échec(s)\n", s_ok, s_fail);
    return s_fail ? 1 : 0;
}
