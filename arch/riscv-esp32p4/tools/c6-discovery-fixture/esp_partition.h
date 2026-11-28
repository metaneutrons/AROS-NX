/* Minimal host mirror of the IDF 5.4.3 partition API types and signatures. */
#ifndef C6_DISCOVERY_FIXTURE_ESP_PARTITION_H
#define C6_DISCOVERY_FIXTURE_ESP_PARTITION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifndef C6_DISCOVERY_FIXTURE_ESP_FLASH_T
#define C6_DISCOVERY_FIXTURE_ESP_FLASH_T
typedef struct esp_flash_t esp_flash_t;
#endif

typedef enum {
    ESP_PARTITION_TYPE_APP = 0x00,
    ESP_PARTITION_TYPE_DATA = 0x01,
    ESP_PARTITION_TYPE_BOOTLOADER = 0x02,
    ESP_PARTITION_TYPE_PARTITION_TABLE = 0x03,
    ESP_PARTITION_TYPE_ANY = 0xff
} esp_partition_type_t;

typedef enum {
    ESP_PARTITION_SUBTYPE_APP_FACTORY = 0x00,
    ESP_PARTITION_SUBTYPE_APP_OTA_MIN = 0x10,
    ESP_PARTITION_SUBTYPE_APP_OTA_0 = 0x10,
    ESP_PARTITION_SUBTYPE_APP_OTA_1 = 0x11,
    ESP_PARTITION_SUBTYPE_APP_OTA_MAX = 0x20,
    ESP_PARTITION_SUBTYPE_DATA_OTA = 0x00,
    ESP_PARTITION_SUBTYPE_DATA_NVS = 0x02,
    ESP_PARTITION_SUBTYPE_ANY = 0xff
} esp_partition_subtype_t;

typedef struct esp_partition_iterator_opaque_ *esp_partition_iterator_t;

typedef struct {
    esp_flash_t *flash_chip;
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
    uint32_t address;
    uint32_t size;
    uint32_t erase_size;
    char label[17];
    bool encrypted;
    bool readonly;
} esp_partition_t;

esp_partition_iterator_t esp_partition_find(esp_partition_type_t type,
    esp_partition_subtype_t subtype, const char *label);
const esp_partition_t *esp_partition_get(esp_partition_iterator_t iterator);
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t iterator);
void esp_partition_iterator_release(esp_partition_iterator_t iterator);
esp_err_t esp_partition_write(const esp_partition_t *partition,
    size_t dst_offset, const void *src, size_t size);
esp_err_t esp_partition_erase_range(const esp_partition_t *partition,
    size_t offset, size_t size);

#endif
