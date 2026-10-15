/* Minimal host mock of the IDF flash declarations used by the C6 collector. */
#ifndef C6_DISCOVERY_FIXTURE_ESP_FLASH_H
#define C6_DISCOVERY_FIXTURE_ESP_FLASH_H

#include <stdint.h>
#include "esp_err.h"

#ifndef C6_DISCOVERY_FIXTURE_ESP_FLASH_T
#define C6_DISCOVERY_FIXTURE_ESP_FLASH_T
/* The real type is public, but this collector only observes chip identity. */
typedef struct esp_flash_t esp_flash_t;
#endif

extern esp_flash_t *esp_flash_default_chip;

esp_err_t esp_flash_get_physical_size(esp_flash_t *chip, uint32_t *flash_size);
esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *flash_size);
esp_err_t esp_flash_write(esp_flash_t *chip, const void *buffer,
                          uint32_t address, uint32_t length);
esp_err_t esp_flash_erase_region(esp_flash_t *chip, uint32_t start, uint32_t length);
esp_err_t esp_flash_write_encrypted(esp_flash_t *chip, uint32_t address,
                                    const void *buffer, uint32_t length);

#endif
