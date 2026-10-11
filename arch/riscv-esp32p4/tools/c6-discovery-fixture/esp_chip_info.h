#ifndef C6_DISCOVERY_FIXTURE_ESP_CHIP_INFO_H
#define C6_DISCOVERY_FIXTURE_ESP_CHIP_INFO_H

#include <stdint.h>

typedef enum {
    CHIP_ESP32C3 = 5,
    CHIP_ESP32C6 = 13,
    CHIP_ESP32P4 = 18
} esp_chip_model_t;

typedef struct {
    esp_chip_model_t model;
    uint32_t features;
    uint16_t revision;
    uint8_t cores;
} esp_chip_info_t;

void esp_chip_info(esp_chip_info_t *out_info);

#endif
