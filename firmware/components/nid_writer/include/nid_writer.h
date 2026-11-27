/*
 * nid_writer — écriture de fichier asynchrone, avec tampon en PSRAM
 *
 * Pendant un enregistrement, la boucle caméra → encodeur → multiplexeur ne doit
 * jamais attendre le support de stockage : une carte microSD (nettoyage interne,
 * allocation FAT) ou une clé USB peut bloquer une écriture plusieurs centaines
 * de millisecondes. Avec 3 tampons caméra, une seule attente de ce genre faisait
 * perdre des images (≈ 140 sur 300 à 1080p30 au banc du 14/10) et retardait le
 * son (verrou du multiplexeur tenu pendant l'écriture).
 *
 * nid_writer copie les données dans un grand tampon circulaire (PSRAM, 4 Mo par
 * défaut, soit ≈ 13 s de vidéo à 2,5 Mbit/s) ; une tâche dédiée les écrit sur le
 * support dans l'ordre (écritures et déplacements). Le producteur n'attend que
 * si le tampon est plein, c'est-à-dire si le support est durablement plus lent
 * que le flux vidéo.
 *
 * Un seul fichier à la fois par instance ; un seul producteur (non réentrant).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nid_writer nid_writer_t;

/** Bilan d'écriture (journal, banc d'essai). */
typedef struct {
    size_t   buffer_bytes;     /*!< capacité du tampon */
    size_t   peak_bytes;       /*!< remplissage maximal atteint */
    uint32_t stall_ms;         /*!< temps total d'attente du producteur (tampon plein) */
    uint32_t slowest_write_ms; /*!< écriture la plus lente sur le support */
    uint64_t bytes;            /*!< octets écrits sur le support */
    bool     error;            /*!< une écriture, un déplacement ou la fermeture a échoué */
} nid_writer_stats_t;

/**
 * Crée un écrivain : tampon de transfert de 16 ko en mémoire interne DMA et
 * tâche d'écriture. Le tampon circulaire de buffer_bytes octets (PSRAM) n'est
 * réservé qu'à nid_writer_open(). buffer_bytes = 0 : taille de menuconfig
 * (CONFIG_NID_WRITER_BUFFER_KB).
 *
 * Ordre conseillé avec l'encodeur H.264 : créer l'écrivain avant de démarrer
 * l'encodeur (mémoire interne), ouvrir le fichier après (PSRAM).
 */
esp_err_t nid_writer_create(size_t buffer_bytes, nid_writer_t **out);

/** Ouvre (crée ou tronque) le fichier, de façon synchrone. */
esp_err_t nid_writer_open(nid_writer_t *w, const char *path);

/**
 * Ajoute len octets à écrire. Retourne immédiatement si le tampon a la place ;
 * sinon attend au plus CONFIG_NID_WRITER_STALL_TIMEOUT_MS que la tâche
 * d'écriture en libère. @return len, ou -1 si une erreur est survenue (support
 * plein ou retiré, délai dépassé) : les écritures suivantes échouent aussi.
 */
int nid_writer_write(nid_writer_t *w, const void *data, size_t len);

/** Déplace la position d'écriture (dans l'ordre des écritures). @return 0 ou -1. */
int nid_writer_seek(nid_writer_t *w, uint64_t pos);

/**
 * Vide le tampon, puis fflush + fsync + fclose. Bloquant, au plus timeout_ms.
 * @return ESP_OK si toutes les données sont sur le support.
 */
esp_err_t nid_writer_close(nid_writer_t *w, uint32_t timeout_ms);

/** true si une erreur a été rencontrée depuis l'ouverture. */
bool nid_writer_failed(const nid_writer_t *w);

void nid_writer_get_stats(const nid_writer_t *w, nid_writer_stats_t *out);

/**
 * Arrête la tâche et libère le tampon. Un fichier encore ouvert est fermé sans
 * garantie (appeler nid_writer_close() avant).
 */
void nid_writer_destroy(nid_writer_t *w);

#ifdef __cplusplus
}
#endif
