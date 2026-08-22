/*
 * Read-only ESP-IDF control experiment for the D1001 SD socket.
 *
 * This deliberately bypasses every AROS storage component.  It brings up the
 * official ESP-IDF P4 SDMMC host at 1-bit/400 kHz and compares CMD18 data
 * against independently issued CMD17 reads.  It contains no filesystem,
 * write, erase or formatting operation.
 */

#include <inttypes.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

#define D1001_SD_POWER_GPIO     GPIO_NUM_46
#define D1001_SD_LBA            2048U
#define D1001_SECTOR_SIZE       512U

static const char *TAG = "idf-sdctrl";

static uint32_t fnv1a32(const uint8_t *data, size_t length)
{
    uint32_t hash = 2166136261U;

    while (length-- != 0) {
        hash ^= *data++;
        hash *= 16777619U;
    }
    return hash;
}

static void compare_transfer(sdmmc_card_t *card, size_t sectors)
{
    const size_t bytes = sectors * D1001_SECTOR_SIZE;
    uint8_t *expected = heap_caps_aligned_alloc(4, bytes, MALLOC_CAP_DMA);
    uint8_t *actual = heap_caps_aligned_alloc(4, bytes, MALLOC_CAP_DMA);
    esp_err_t err;
    size_t index;

    if (expected == NULL || actual == NULL) {
        ESP_LOGE(TAG, "allocation failed for %u sectors", (unsigned)sectors);
        goto out;
    }

    for (index = 0; index < sectors; ++index) {
        err = sdmmc_read_sectors(card, expected + index * D1001_SECTOR_SIZE,
                                 D1001_SD_LBA + index, 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "CMD17 LBA %u failed: %s",
                     (unsigned)(D1001_SD_LBA + index), esp_err_to_name(err));
            goto out;
        }
    }

    memset(actual, 0xa5, bytes);
    err = sdmmc_read_sectors(card, actual, D1001_SD_LBA, sectors);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CMD18 LBA %u sectors %u failed: %s",
                 D1001_SD_LBA, (unsigned)sectors, esp_err_to_name(err));
        goto out;
    }

    if (memcmp(expected, actual, bytes) == 0) {
        ESP_LOGI(TAG, "CMD18 LBA %u sectors %u PASS hash=%08" PRIx32,
                 D1001_SD_LBA, (unsigned)sectors, fnv1a32(actual, bytes));
        goto out;
    }

    for (index = 0; index < bytes; ++index) {
        if (expected[index] != actual[index]) {
            ESP_LOGE(TAG, "CMD18 LBA %u sectors %u MISMATCH byte %u exp=%02x got=%02x",
                     D1001_SD_LBA, (unsigned)sectors, (unsigned)index,
                     expected[index], actual[index]);
            break;
        }
    }

out:
    heap_caps_free(actual);
    heap_caps_free(expected);
}

void app_main(void)
{
    sd_pwr_ctrl_handle_t power = NULL;
    sd_pwr_ctrl_ldo_config_t power_config = { .ldo_chan_id = 4 };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    sdmmc_card_t card;
    esp_err_t err;
    const size_t tests[] = { 2, 32, 128 };
    size_t index;

    ESP_LOGI(TAG, "read-only D1001 ESP-IDF SDMMC control test");
    ESP_ERROR_CHECK(gpio_set_direction(D1001_SD_POWER_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(D1001_SD_POWER_GPIO, 1));

    ESP_ERROR_CHECK(sd_pwr_ctrl_new_on_chip_ldo(&power_config, &power));
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_PROBING;
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.pwr_ctrl_handle = power;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    err = host.init();
    if (err == ESP_OK)
        err = sdmmc_host_init_slot(host.slot, &slot);
    if (err == ESP_OK)
        err = sdmmc_card_init(&host, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD initialization failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "card sectors=%" PRIu32 ", width=%u, max=%u kHz",
             card.csd.capacity, (unsigned)slot.width, (unsigned)host.max_freq_khz);
    for (index = 0; index < sizeof(tests) / sizeof(tests[0]); ++index)
        compare_transfer(&card, tests[index]);

    ESP_LOGI(TAG, "control test complete; no SD write was issued");
}
