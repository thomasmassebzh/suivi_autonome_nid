/*
 * banc_video — vidéos et photos du banc d'essai, par le vrai chemin du firmware
 *
 *   nid_storage_media_begin → nid_capture_record → nid_storage_media_commit
 *   (ou nid_storage_media_abort : fichier conservé en « _INCOMPLET »)
 *
 * Chaque enregistrement produit un bilan à l'écran, sur le journal série (ligne
 * « BILAN » à copier dans un compte rendu) et dans le journal CSV de la carte.
 * Appelé uniquement par la tâche d'action (banc_main.c).
 */
#include "banc.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "nid_capture.h"
#include "nid_storage.h"

static const char *TAG = "banc";

static char s_bilan[512] = "aucun";         /* dernière ligne de bilan (commande info) */
static uint32_t s_seq_video;
static uint32_t s_seq_photo;

/* Enregistrement en cours (rappel de progression) */
typedef struct {
    int64_t       t_debut_us;
    uint32_t      duree_s;
    int32_t       seconde_affichee;
    banc_rappel_t rappel;
    void         *arg;
    char          motif[24];
} enreg_t;

const char *banc_dernier_bilan(void)
{
    return s_bilan;
}

/* ------------------------------------------------------------------------- */
/* Crochet d'image                                                            */
/* ------------------------------------------------------------------------- */

void banc_crochet_image(const nid_capture_frame_t *f, void *arg)
{
    (void)arg;
    if (f->recording) {
        banc_etat_t *e = banc_etat_verrouiller();
        e->rec_images++;
        banc_etat_deverrouiller();
    }
    banc_ecran_image(f);
}

/* ------------------------------------------------------------------------- */
/* Vidéo                                                                      */
/* ------------------------------------------------------------------------- */

static const char *piste_video(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_TIMEOUT:
        return "Aucune image reçue de la caméra : nappe, alimentation ;\n"
               "voir le journal série (nid_capture)";
    case ESP_ERR_NO_MEM:
        return "Mémoire insuffisante (multiplexeur MP4)";
    case ESP_ERR_INVALID_RESPONSE:
        return "Encodeur H.264 : image codée invalide\n(voir le journal série)";
    case ESP_ERR_INVALID_STATE:
        return "Caméra non prête : recommencer";
    default:
        return "Écriture impossible : carte SD pleine, trop lente (A1/V30)\n"
               "ou retirée ? Voir le journal série";
    }
}

/** Rappel de nid_capture_record (~ toutes les 100 ms) : progression, arrêt anticipé, rappel de l'appelant. */
static bool rappel_enregistrement(void *arg)
{
    enreg_t *en = (enreg_t *)arg;
    const int32_t sec = (int32_t)((esp_timer_get_time() - en->t_debut_us) / 1000000LL);
    if (sec != en->seconde_affichee) {
        en->seconde_affichee = sec;
        banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_ROUGE, "REC %s %" PRId32 "/%" PRIu32 " s — BOOT pour arrêter",
                         en->motif, sec, en->duree_s);
    }
    if (banc_arret_demande()) {
        return true;
    }
    return en->rappel != NULL && en->rappel(en->arg);
}

static void bilan_video(const banc_demande_t *d, const char *motif, const nid_media_file_t *f,
                        const banc_bilan_video_t *b)
{
    const nid_capture_stats_t *st = &b->st;
    const uint32_t ms = st->duration_ms;
    const uint32_t ips10 = ms ? (uint32_t)((uint64_t)st->video_frames * 10000ULL / ms) : 0;
    const uint32_t mo100 = (uint32_t)(st->bytes / 10000ULL);    /* centièmes de Mo (1 Mo = 10^6 o) */
    const uint32_t kbps = ms ? (uint32_t)(st->bytes * 8ULL / ms) : 0;
    const bool ok = (b->err == ESP_OK);
    const char *resultat = b->annule ? "ANNULE" : (ok ? (b->arrete ? "OK_ARRET_ANTICIPE" : "OK") : "ECHEC");
    const nid_capture_settings_t *r = banc_reglages_capture();

    snprintf(s_bilan, sizeof(s_bilan),
             "BILAN video fichier=%s motif=%s resultat=%s erreur=%s duree=%" PRIu32 ".%02" PRIu32 "s images=%" PRIu32
             " cadence=%" PRIu32 ".%" PRIu32 "ips perdues=%" PRIu32 " trames_son=%" PRIu32 " taille=%" PRIu32
             ".%02" PRIu32 "Mo debit=%" PRIu32 "kbit/s premiere_image=%" PRId32 "ms reglages=%ux%u,%uips,%" PRIu32
             "kbit/s,son=%s capteur=%s",
             f->name, motif, resultat, esp_err_to_name(b->err), ms / 1000, (ms % 1000) / 10, st->video_frames,
             ips10 / 10, ips10 % 10, st->dropped_frames, st->audio_frames, mo100 / 100, mo100 % 100, kbps,
             b->premiere_ms, r->width, r->height, r->fps, r->bitrate_kbps, r->audio ? "oui" : "non",
             nid_capture_sensor_name());
    ESP_LOGI(TAG, "%s", s_bilan);

    if (b->annule) {
        return;     /* fausse détection : bilan affiché par l'appelant */
    }
    if (ok) {
        nid_storage_journal("video_banc", "%s;%s;%" PRIu32 " ms;%" PRIu32 " images;%" PRIu32 ".%" PRIu32 " i/s;%" PRIu32
                            " perdues;%" PRIu32 " trames son;%" PRIu32 ".%02" PRIu32 " Mo;%" PRIu32 " kbit/s;"
                            "1re image %" PRId32 " ms;%s",
                            f->name, motif, ms, st->video_frames, ips10 / 10, ips10 % 10, st->dropped_frames,
                            st->audio_frames, mo100 / 100, mo100 % 100, kbps, b->premiere_ms, d->origine);
        banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_VERT, "Vidéo enregistrée (%s)%s", motif,
                         b->arrete ? " — arrêt anticipé" : "");
        banc_ecran_ligne(BANC_LIGNE_DETAIL1, BANC_BLANC, "%s", f->name);
    } else {
        nid_storage_journal("erreur", "video_banc %s : %s (%s)", f->name, esp_err_to_name(b->err),
                            b->finalisation ? "finalisation" : "enregistrement");
        if (b->finalisation) {
            banc_afficher_erreur("finalisation", b->err, "Renommage .tmp → .mp4 impossible : carte SD retirée ?\n"
                                 "Le .tmp sera renommé _INCOMPLET au prochain démarrage");
        } else {
            banc_afficher_erreur("enregistrement", b->err, piste_video(b->err));
            banc_ecran_ligne(BANC_LIGNE_DETAIL3, BANC_GRIS, "%s", "Fichier partiel conservé sous le nom ..._INCOMPLET.mp4");
        }
        return;
    }
    banc_ecran_ligne(BANC_LIGNE_DETAIL2, BANC_BLANC,
                     "%" PRIu32 ".%02" PRIu32 " s | %" PRIu32 " images | %" PRIu32 ".%" PRIu32 " i/s | %" PRIu32
                     " perdue(s) | son : %" PRIu32 " trames",
                     ms / 1000, (ms % 1000) / 10, st->video_frames, ips10 / 10, ips10 % 10, st->dropped_frames,
                     st->audio_frames);
    banc_ecran_ligne(BANC_LIGNE_DETAIL3, (st->dropped_frames || (r->audio && st->audio_frames == 0)) ? BANC_JAUNE : BANC_BLANC,
                     "%" PRIu32 ".%02" PRIu32 " Mo | %" PRIu32 " kbit/s | 1re image à %" PRId32 " ms %s%s",
                     mo100 / 100, mo100 % 100, kbps, b->premiere_ms,
                     strcmp(d->origine, "PIR") == 0 ? "du front PIR" : "de la demande",
                     (r->audio && st->audio_frames == 0) ? " | SANS SON" : "");
}

void banc_video_enregistrer(const banc_demande_t *d, uint32_t duree_s, const char *motif, banc_rappel_t rappel,
                            banc_rappel_t annuler, void *arg, banc_bilan_video_t *b)
{
    memset(b, 0, sizeof(*b));
    b->premiere_ms = -1;
    b->err = ESP_ERR_INVALID_STATE;
    banc_effacer_details();
    banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_JAUNE, "Préparation de l'enregistrement (%s, %s)...", motif, d->origine);
    if (!banc_preparer()) {
        return;
    }
    nid_media_file_t f;
    esp_err_t err = nid_storage_media_begin(NID_MEDIA_VIDEO, time(NULL), ++s_seq_video, &f);
    if (err != ESP_OK) {
        b->err = err;
        banc_afficher_erreur("fichier", err, "Carte SD pleine, protégée en écriture ou retirée ?");
        return;
    }
    strlcpy(b->nom, f.name, sizeof(b->nom));

    ESP_LOGI(TAG, "enregistrement de %" PRIu32 " s (%s, %s) : %s", duree_s, motif, d->origine, f.tmp_path);
#if !CONFIG_BANC_VUE_ENREG
    banc_ecran_message_vue(BANC_GRIS, "Vue coupée pendant l'enregistrement");
#endif
    enreg_t en = {
        .t_debut_us = esp_timer_get_time(),
        .duree_s = duree_s,
        .seconde_affichee = -1,
        .rappel = rappel,
        .arg = arg,
    };
    strlcpy(en.motif, motif, sizeof(en.motif));
    banc_etat_t *e = banc_etat_verrouiller();
    e->rec = true;
    e->rec_debut_us = en.t_debut_us;
    e->rec_duree_s = duree_s;
    e->rec_images = 0;
    strlcpy(e->rec_motif, motif, sizeof(e->rec_motif));
    banc_etat_deverrouiller();

    err = nid_capture_record(f.tmp_path, duree_s * 1000u, rappel_enregistrement, &en, &b->st);

    e = banc_etat_verrouiller();
    e->rec = false;
    banc_etat_deverrouiller();
    b->arrete = banc_arret_demande();
    b->annule = (annuler != NULL && annuler(arg));

    if (b->annule) {
        nid_storage_media_abort(&f, false);         /* fausse détection : rien n'est conservé */
        b->err = err;
    } else if (err == ESP_OK) {
        err = nid_storage_media_commit(&f);       /* .tmp → nom définitif */
        b->finalisation = (err != ESP_OK);
        b->err = err;
    } else {
        nid_storage_media_abort(&f, true);        /* conservé en _INCOMPLET, comme le firmware */
        b->err = err;
    }
    if (b->st.first_frame_ms != 0) {
        b->premiere_ms = (int32_t)((int64_t)b->st.first_frame_ms - d->t_demande_us / 1000);
    }
    banc_lire_espace_sd();
    bilan_video(d, motif, &f, b);

    e = banc_etat_verrouiller();
    if (!b->annule) {
        e->bilan_images = b->st.video_frames;
        e->bilan_perdues = b->st.dropped_frames;
        e->bilan_trames_son = b->st.audio_frames;
        e->bilan_duree_ms = b->st.duration_ms;
        e->bilan_ok = (b->err == ESP_OK);
        if (b->err == ESP_OK) {
            strlcpy(e->dernier_fichier, f.name, sizeof(e->dernier_fichier));
        }
    }
    banc_etat_deverrouiller();
    banc_rafraichir_entete();
}

/* ------------------------------------------------------------------------- */
/* Photo                                                                      */
/* ------------------------------------------------------------------------- */

esp_err_t banc_photo_prendre(const banc_demande_t *d, bool suivi)
{
    banc_effacer_details();
    banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_JAUNE, "Photo%s (%s)...", suivi ? " de suivi" : "", d->origine);
    if (suivi) {
        banc_suivi_planifier();     /* prochaine échéance avancée avant la prise (pas de boucle sur défaut) */
    }
    if (!banc_preparer()) {
        return ESP_ERR_INVALID_STATE;
    }
    nid_media_file_t f;
    esp_err_t err = nid_storage_media_begin(NID_MEDIA_PHOTO, time(NULL), ++s_seq_photo, &f);
    if (err != ESP_OK) {
        banc_afficher_erreur("fichier", err, "Carte SD pleine, protégée en écriture ou retirée ?");
        return err;
    }
    err = nid_capture_photo(f.tmp_path);
    if (err == ESP_OK) {
        err = nid_storage_media_commit(&f);
    } else {
        nid_storage_media_abort(&f, false);
    }
    const uint32_t ms = (uint32_t)((esp_timer_get_time() - d->t_demande_us) / 1000);
    struct stat sb;
    const uint32_t ko = (err == ESP_OK && stat(f.final_path, &sb) == 0) ? (uint32_t)(sb.st_size / 1000) : 0;
    banc_lire_espace_sd();
    const nid_capture_settings_t *r = banc_reglages_capture();

    snprintf(s_bilan, sizeof(s_bilan), "BILAN photo fichier=%s motif=%s resultat=%s erreur=%s taille=%" PRIu32 "ko duree=%" PRIu32
             "ms reglages=%ux%u,qualite=%u capteur=%s", f.name, suivi ? "suivi" : "manuel", err == ESP_OK ? "OK" : "ECHEC",
             esp_err_to_name(err), ko, ms, r->width, r->height, r->jpeg_quality, nid_capture_sensor_name());
    ESP_LOGI(TAG, "%s", s_bilan);
    if (err == ESP_OK) {
        nid_storage_journal(suivi ? "photo" : "photo_banc", "%s;%s;%" PRIu32 " ko;%" PRIu32 " ms;%s", f.name,
                            suivi ? "suivi" : "manuelle", ko, ms, d->origine);
        banc_ecran_ligne(BANC_LIGNE_ETAT, BANC_VERT, "Photo%s enregistrée", suivi ? " de suivi" : "");
        banc_ecran_ligne(BANC_LIGNE_DETAIL1, BANC_BLANC, "%s", f.name);
        banc_ecran_ligne(BANC_LIGNE_DETAIL2, BANC_BLANC, "%" PRIu32 " ko | %" PRIu32 " ms depuis la demande", ko, ms);
    } else {
        nid_storage_journal("erreur", "photo_banc %s : %s", f.name, esp_err_to_name(err));
        banc_afficher_erreur("photo", err, piste_video(err));
    }
    banc_etat_t *e = banc_etat_verrouiller();
    e->photo_ok = (err == ESP_OK);
    if (err == ESP_OK) {
        e->photos++;
        if (suivi) {
            e->photos_suivi++;
        }
        strlcpy(e->photo_fichier, f.name, sizeof(e->photo_fichier));
        strlcpy(e->dernier_fichier, f.name, sizeof(e->dernier_fichier));
    }
    e->photo_ko = ko;
    e->photo_ms = ms;
    banc_etat_deverrouiller();
    banc_rafraichir_entete();
    return err;
}

/* ------------------------------------------------------------------------- */
/* Endurance                                                                  */
/* ------------------------------------------------------------------------- */

void banc_boucle(const banc_demande_t *d)
{
    const uint32_t n = d->param;
    const uint32_t duree = d->param2 ? d->param2 : CONFIG_BANC_DUREE_S;
    const size_t int0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t ps0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t intmin0 = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "ENDURANCE debut n=%" PRIu32 " duree=%" PRIu32 "s interne=%uko psram=%uko interne_min=%uko", n, duree,
             (unsigned)(int0 / 1024), (unsigned)(ps0 / 1024), (unsigned)(intmin0 / 1024));
    banc_etat_t *e = banc_etat_verrouiller();
    e->boucle_en_cours = true;
    e->boucle_faite = false;
    e->boucle_n = n;
    e->boucle_i = 0;
    e->boucle_echecs = 0;
    e->boucle_perdues = 0;
    banc_etat_deverrouiller();

    uint32_t faites = 0, echecs = 0, perdues = 0, images = 0;
    uint64_t octets = 0;
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t i = 1; i <= n && !banc_arret_demande(); i++) {
        char motif[24];
        snprintf(motif, sizeof(motif), "boucle %" PRIu32 "/%" PRIu32, i, n);
        e = banc_etat_verrouiller();
        e->boucle_i = i;
        banc_etat_deverrouiller();
        banc_demande_t di = *d;
        di.t_demande_us = esp_timer_get_time();
        banc_bilan_video_t b;
        banc_video_enregistrer(&di, duree, motif, NULL, NULL, NULL, &b);
        faites++;
        if (b.err != ESP_OK) {
            echecs++;
        }
        perdues += b.st.dropped_frames;
        images += b.st.video_frames;
        octets += b.st.bytes;
        e = banc_etat_verrouiller();
        e->boucle_echecs = echecs;
        e->boucle_perdues = perdues;
        banc_etat_deverrouiller();
    }
    const bool interrompue = banc_arret_demande();
    const size_t int1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t ps1 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t intmin1 = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const int32_t d_int = (int32_t)(((int64_t)int1 - (int64_t)int0) / 1024);
    const int32_t d_ps = (int32_t)(((int64_t)ps1 - (int64_t)ps0) / 1024);
    const uint32_t s = (uint32_t)((esp_timer_get_time() - t0) / 1000000LL);

    snprintf(s_bilan, sizeof(s_bilan),
             "BILAN boucle videos=%" PRIu32 "/%" PRIu32 " echecs=%" PRIu32 " images=%" PRIu32 " perdues=%" PRIu32
             " taille=%" PRIu64 "Mo duree=%" PRIu32 "s interne=%uko->%uko(%+" PRId32 ") psram=%uko->%uko(%+" PRId32
             ") interne_min=%uko->%uko%s",
             faites, n, echecs, images, perdues, octets / 1000000ULL, s, (unsigned)(int0 / 1024), (unsigned)(int1 / 1024),
             d_int, (unsigned)(ps0 / 1024), (unsigned)(ps1 / 1024), d_ps, (unsigned)(intmin0 / 1024),
             (unsigned)(intmin1 / 1024), interrompue ? " interrompue" : "");
    ESP_LOGI(TAG, "%s", s_bilan);
    nid_storage_journal("endurance_banc", "%" PRIu32 "/%" PRIu32 " vidéos;%" PRIu32 " échecs;%" PRIu32 " perdues;"
                        "mémoire interne %+" PRId32 " ko;PSRAM %+" PRId32 " ko%s", faites, n, echecs, perdues, d_int, d_ps,
                        interrompue ? ";interrompue" : "");

    const bool ok = (echecs == 0 && perdues == 0 && d_int > -8 && d_ps > -64);
    banc_ecran_ligne(BANC_LIGNE_ETAT, ok ? BANC_VERT : BANC_JAUNE, "Endurance : %" PRIu32 "/%" PRIu32 " vidéos, %" PRIu32
                     " échec(s), %" PRIu32 " image(s) perdue(s)%s", faites, n, echecs, perdues,
                     interrompue ? " (interrompue)" : "");
    banc_ecran_ligne(BANC_LIGNE_DETAIL1, BANC_BLANC, "Mémoire interne %u → %u ko (%+" PRId32 " ko), plus bas %u ko",
                     (unsigned)(int0 / 1024), (unsigned)(int1 / 1024), d_int, (unsigned)(intmin1 / 1024));
    banc_ecran_ligne(BANC_LIGNE_DETAIL2, BANC_BLANC, "PSRAM %u → %u ko (%+" PRId32 " ko) | %" PRIu64 " Mo en %" PRIu32 " s",
                     (unsigned)(ps0 / 1024), (unsigned)(ps1 / 1024), d_ps, octets / 1000000ULL, s);
    banc_ecran_ligne(BANC_LIGNE_DETAIL3, BANC_GRIS, "%s", "Fuite : mémoire qui baisse à chaque vidéo (relancer « boucle » pour confirmer)");

    e = banc_etat_verrouiller();
    e->boucle_en_cours = false;
    e->boucle_faite = true;
    e->boucle_delta_interne_ko = d_int;
    e->boucle_delta_psram_ko = d_ps;
    banc_etat_deverrouiller();
}
