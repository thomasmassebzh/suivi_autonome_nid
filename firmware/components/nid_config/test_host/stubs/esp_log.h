/*
 * Substitut PC de esp_log.h (tests sur PC uniquement).
 * Les messages ne sont affichés qu'avec NID_TEST_VERBOSE=1 (ou 2 pour les
 * informations) ; avertissements et erreurs sont comptés pour les tests.
 */
#pragma once

#include <stdio.h>

extern int g_log_verbose;   /* 0 : muet, 1 : avertissements et erreurs, 2 : tout */
extern int g_log_warnings;  /* nombre de ESP_LOGW depuis le début */
extern int g_log_errors;    /* nombre de ESP_LOGE depuis le début */

#define ESP_LOGE(tag, fmt, ...) do { \
        g_log_errors++; \
        if (g_log_verbose >= 1) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__); \
    } while (0)
#define ESP_LOGW(tag, fmt, ...) do { \
        g_log_warnings++; \
        if (g_log_verbose >= 1) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__); \
    } while (0)
#define ESP_LOGI(tag, fmt, ...) do { \
        if (g_log_verbose >= 2) fprintf(stderr, "I %s: " fmt "\n", tag, ##__VA_ARGS__); \
    } while (0)
#define ESP_LOGD(tag, fmt, ...) do { \
        if (0) fprintf(stderr, "D %s: " fmt "\n", tag, ##__VA_ARGS__); \
    } while (0)
#define ESP_LOGV(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
