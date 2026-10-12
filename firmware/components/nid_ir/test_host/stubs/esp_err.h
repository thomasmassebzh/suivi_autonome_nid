/*
 * Remplaçant minimal de esp_err.h pour compiler nid_ir_sun.c sur PC
 * (mêmes valeurs que ESP-IDF v6.1, components/esp_common/include/esp_err.h).
 */
#pragma once

typedef int esp_err_t;

#define ESP_OK                  0
#define ESP_FAIL                -1
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERR_TIMEOUT         0x107
