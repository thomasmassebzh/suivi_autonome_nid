/*
 * Mini-cadre de tests sur PC (aucune dépendance) : CHECK*, RUN, résumé.
 */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int g_checks;
extern int g_failures;

#define CHECK(cond) do { \
        g_checks++; \
        if (!(cond)) { \
            g_failures++; \
            fprintf(stderr, "%s:%d: ÉCHEC : %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define CHECK_INT(a, b) do { \
        long long _a = (long long)(a), _b = (long long)(b); \
        g_checks++; \
        if (_a != _b) { \
            g_failures++; \
            fprintf(stderr, "%s:%d: ÉCHEC : %s == %lld, attendu %s == %lld\n", \
                    __FILE__, __LINE__, #a, _a, #b, _b); \
        } \
    } while (0)

#define CHECK_STR(a, b) do { \
        const char *_a = (a), *_b = (b); \
        g_checks++; \
        if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) { \
            g_failures++; \
            fprintf(stderr, "%s:%d: ÉCHEC : %s = « %s », attendu « %s »\n", \
                    __FILE__, __LINE__, #a, _a ? _a : "(null)", _b ? _b : "(null)"); \
        } \
    } while (0)

#define RUN(test) do { \
        int _before = g_failures; \
        test(); \
        printf("%-48s %s\n", #test, g_failures == _before ? "ok" : "ÉCHEC"); \
    } while (0)

/* Code de sortie du programme de test. */
static inline int test_summary(const char *name)
{
    printf("%s : %d vérifications, %d échec(s)\n", name, g_checks, g_failures);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
