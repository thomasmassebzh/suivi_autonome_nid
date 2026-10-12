/*
 * Implémentations PC des substituts (tests sur PC uniquement).
 */
#include <stdlib.h>
#include "esp_err.h"
#include "esp_log.h"
#include "test_util.h"

int g_log_verbose;
int g_log_warnings;
int g_log_errors;
int g_checks;
int g_failures;

__attribute__((constructor)) static void stubs_init(void)
{
    const char *v = getenv("NID_TEST_VERBOSE");
    g_log_verbose = v ? atoi(v) : 0;
}

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK:
        return "ESP_OK";
    case ESP_FAIL:
        return "ESP_FAIL";
    case ESP_ERR_NO_MEM:
        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:
        return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:
        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:
        return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:
        return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED:
        return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT:
        return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE:
        return "ESP_ERR_INVALID_RESPONSE";
    case ESP_ERR_INVALID_CRC:
        return "ESP_ERR_INVALID_CRC";
    default:
        return "ESP_ERR_?";
    }
}
