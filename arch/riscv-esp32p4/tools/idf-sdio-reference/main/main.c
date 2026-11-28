/*
 * SPDX-FileCopyrightText: 2026 Fabian Schmieder
 * SPDX-License-Identifier: MIT
 *
 * Bounded ESP-IDF SDIO host reference probe for ESP32-P4.
 * Sends only CMD0 and CMD5(arg=0); it deliberately does not initialize a card.
 * CMD0 changes protocol state; neither command programs C6 firmware.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "driver/sdmmc_default_configs.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_protocol_defs.h"

#define SDIO_SLOT              SDMMC_HOST_SLOT_1
#define SDIO_CMD_TIMEOUT_MS    500U
#define CMD5_MAX_ATTEMPTS      3U
#define CMD5_RETRY_DELAY_MS    100U

static void print_result(const char *stage, esp_err_t result)
{
    printf("[sdio-ref] stage=%s result=%s raw=0x%08" PRIx32 "\n",
           stage, esp_err_to_name(result), (uint32_t) result);
}

static esp_err_t send_command(const char *stage, uint32_t opcode, uint32_t arg,
                              int flags, sdmmc_command_t *command)
{
    *command = (sdmmc_command_t) {
        .opcode = opcode,
        .arg = arg,
        .flags = flags,
        .timeout_ms = SDIO_CMD_TIMEOUT_MS,
    };

    printf("[sdio-ref] stage=%s send opcode=%" PRIu32 " arg=0x%08" PRIx32
           " flags=0x%04x timeout_ms=%u\n",
           stage, opcode, arg, (unsigned) flags, (unsigned) SDIO_CMD_TIMEOUT_MS);

    const esp_err_t result = sdmmc_host_do_transaction(SDIO_SLOT, command);
    printf("[sdio-ref] stage=%s result=%s raw=0x%08" PRIx32
           " cmd_error=%s/0x%08" PRIx32 " response0=0x%08" PRIx32 "\n",
           stage, esp_err_to_name(result), (uint32_t) result,
           esp_err_to_name(command->error), (uint32_t) command->error,
           command->response[0]);
    /* Legacy IDF may report successful dispatch while the peripheral result
     * lives in command->error. Never decode an OCR after either failure. */
    return result != ESP_OK ? result : command->error;
}

static void idle_forever(void)
{
    printf("[sdio-ref] stage=idle\n");
    fflush(stdout);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    printf("[sdio-ref] boot: ESP-IDF SDIO host-driver reference; diagnostic only\n");
    fflush(stdout);

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = GPIO_NUM_18;
    slot_config.cmd = GPIO_NUM_19;
    slot_config.d0 = GPIO_NUM_14;
    slot_config.d1 = GPIO_NUM_15;
    slot_config.d2 = GPIO_NUM_16;
    slot_config.d3 = GPIO_NUM_17;
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.width = 1;
    slot_config.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    printf("[sdio-ref] pins slot=%d width=%u clk=%d cmd=%d d0=%d d1=%d d2=%d d3=%d"
           " cd=NC wp=NC clock_khz=%u\n",
           SDIO_SLOT, (unsigned) slot_config.width,
           (int) slot_config.clk, (int) slot_config.cmd,
           (int) slot_config.d0, (int) slot_config.d1,
           (int) slot_config.d2, (int) slot_config.d3,
           (unsigned) SDMMC_FREQ_PROBING);

    esp_err_t result = sdmmc_host_init();
    print_result("host-init", result);
    if (result != ESP_OK) {
        idle_forever();
    }

    result = sdmmc_host_init_slot(SDIO_SLOT, &slot_config);
    print_result("slot-init", result);
    if (result != ESP_OK) {
        idle_forever();
    }

    /* IDF 6.0.1 legacy API-order workaround: set_card_clk marks READY
     * before clk_src is initialized. Let the first transaction initialize
     * the default probing clock, then verify it before requesting an OCR.
     * This is pinned implementation behavior, not a documented API guarantee.
     */
    sdmmc_command_t command = {0};
    result = send_command("cmd0", MMC_GO_IDLE_STATE, 0,
                          SCF_CMD_BC | SCF_RSP_R0, &command);
    if (result != ESP_OK) {
        idle_forever();
    }

    int real_freq_khz = 0;
    result = sdmmc_host_get_real_freq(SDIO_SLOT, &real_freq_khz);
    print_result("clock-verify", result);
    printf("[sdio-ref] clock_actual_khz=%d maximum_khz=%u\n",
           real_freq_khz, (unsigned) SDMMC_FREQ_PROBING);
    if (result != ESP_OK || real_freq_khz <= 0 ||
        real_freq_khz > SDMMC_FREQ_PROBING) {
        printf("[sdio-ref] probing clock invalid; CMD5 withheld\n");
        idle_forever();
    }

    for (unsigned attempt = 1; attempt <= CMD5_MAX_ATTEMPTS; ++attempt) {
        char stage[20];
        snprintf(stage, sizeof(stage), "cmd5-%u", attempt);
        result = send_command(stage, SD_IO_SEND_OP_COND, 0,
                              SCF_CMD_BCR | SCF_RSP_R4, &command);

        if (result != ESP_OK) {
            printf("[sdio-ref] stage=%s response invalid; no OCR decoded\n", stage);
            if (attempt < CMD5_MAX_ATTEMPTS) {
                vTaskDelay(pdMS_TO_TICKS(CMD5_RETRY_DELAY_MS));
            }
            continue;
        }
        const uint32_t r4 = command.response[0];
        const uint32_t ocr = r4 & UINT32_C(0x00ffffff);
        const unsigned function_count = (unsigned) SD_IO_OCR_NUM_FUNCTIONS(r4);
        const bool ready = (r4 & SD_IO_OCR_MEM_READY) != 0;
        printf("[sdio-ref] stage=%s r4=0x%08" PRIx32 " ocr=0x%06" PRIx32
               " functions=%u ready=%u memory_present=%u\n",
               stage, r4, ocr, function_count, ready ? 1U : 0U,
               (r4 & SD_IO_OCR_MEM_PRESENT) != 0 ? 1U : 0U);

        if (result == ESP_OK && ready) {
            break;
        }
        if (attempt < CMD5_MAX_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(CMD5_RETRY_DELAY_MS));
        }
    }

    idle_forever();
}
