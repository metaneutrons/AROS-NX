#ifndef C6_DISCOVERY_FIXTURE_ESP_EFUSE_H
#define C6_DISCOVERY_FIXTURE_ESP_EFUSE_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t efuse_block;
    uint8_t bit_start;
    uint16_t bit_count;
} esp_efuse_desc_t;

esp_err_t esp_efuse_read_field_cnt(const esp_efuse_desc_t *field[], size_t *out_cnt);

#endif
