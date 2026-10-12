/*
 * nid_app — logique du système : réveil → action → veille
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Exécute le cycle de l'application. En mode terrain, ne revient pas (veille). */
void nid_app_run(void);

#ifdef __cplusplus
}
#endif
