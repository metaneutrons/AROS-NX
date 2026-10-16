/* Minimal host mock of the ESP-IDF 5.4.3 error type and values used here. */
#ifndef C6_DISCOVERY_FIXTURE_ESP_ERR_H
#define C6_DISCOVERY_FIXTURE_ESP_ERR_H

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106

#endif
