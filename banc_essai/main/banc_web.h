/*
 * banc_web — déclarations internes au tableau de bord web (banc_web.c, banc_web_vue.c)
 */
#pragma once

#include <stdbool.h>
#include <sys/socket.h>
#include "esp_err.h"
#include "esp_http_server.h"

#define BANC_HTTP_400   "400 Bad Request"
#define BANC_HTTP_403   "403 Forbidden"
#define BANC_HTTP_404   "404 Not Found"
#define BANC_HTTP_409   "409 Conflict"
#define BANC_HTTP_413   "413 Content Too Large"
#define BANC_HTTP_500   "500 Internal Server Error"
#define BANC_HTTP_503   "503 Service Unavailable"

/** Réponse texte (UTF-8, jamais mise en cache). */
esp_err_t banc_web_texte(httpd_req_t *req, const char *statut, const char *texte);

/** Adresse de l'appareil à l'origine de la requête (zéros si inconnue). */
void banc_web_pair(httpd_req_t *req, struct sockaddr_storage *out);

/** true si les deux adresses désignent le même appareil (adresse IP). */
bool banc_web_meme_pair(const struct sockaddr_storage *a, const struct sockaddr_storage *b);

/** Compte une requête (état Wi-Fi). */
void banc_web_compter(void);

/* banc_web_vue.c */

/** Tampons, PPA, encodeur JPEG et tâches de la vue (idempotent). */
esp_err_t banc_vue_init(void);

/** GET /stream : flux MJPEG confié à la tâche de la vue (un seul appareil à la fois). */
esp_err_t banc_vue_get(httpd_req_t *req);

/** Images envoyées depuis le démarrage, cadence de la dernière session (dixièmes d'i/s). */
void banc_vue_stats(uint32_t *envoyees, uint32_t *ips10);
