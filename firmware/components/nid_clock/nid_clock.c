/*
 * nid_clock — horloge temps réel (DS3231 / DS1307) et heure système
 *
 * SQUELETTE : pilote DS3231/DS1307 à compléter.
 */
#include "nid_clock.h"

#include <stdlib.h>
#include <sys/time.h>
#include "esp_log.h"

static const char *TAG = "nid_clock";

esp_err_t nid_clock_init(i2c_master_bus_handle_t bus)
{
    (void)bus;
    ESP_LOGW(TAG, "horloge externe non implémentée");
    return ESP_ERR_NOT_SUPPORTED;
}

nid_rtc_model_t nid_clock_model(void)
{
    return NID_RTC_MODEL_NONE;
}

const char *nid_clock_model_str(void)
{
    return "aucune";
}

esp_err_t nid_clock_sync_from_rtc(bool *rtc_valid)
{
    *rtc_valid = false;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nid_clock_set_time(time_t utc)
{
    struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    return ESP_OK;
}

bool nid_clock_time_is_valid(void)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    return tm.tm_year + 1900 >= 2025;
}

void nid_clock_set_timezone(const char *posix_tz)
{
    setenv("TZ", posix_tz, 1);
    tzset();
}

size_t nid_clock_format_local(time_t t, const char *fmt, char *buf, size_t len)
{
    struct tm tm;
    localtime_r(&t, &tm);
    return strftime(buf, len, fmt, &tm);
}

esp_err_t nid_clock_set_alarm(time_t utc)
{
    (void)utc;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nid_clock_clear_alarm(bool *fired)
{
    *fired = false;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nid_clock_get_temperature(float *celsius)
{
    (void)celsius;
    return ESP_ERR_NOT_SUPPORTED;
}
