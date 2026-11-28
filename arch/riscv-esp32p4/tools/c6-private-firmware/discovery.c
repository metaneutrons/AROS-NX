/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "discovery.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_flash_encrypt.h"
#include "esp_secure_boot.h"
#include "esp_chip_info.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include <string.h>

/* Pinned IDF 5.4.3 ESP32-C6 table: SECURE_VERSION is 16 bits at bit 142. */
enum { C6_SECURE_VERSION_FIELD_BITS = 16u };

static void le32(uint8_t *bytes, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (8u * i));
}

static int same(const esp_partition_t *left, const esp_partition_t *right)
{
    if (!left || !right) return left == right;
    return left->flash_chip == right->flash_chip && left->address == right->address &&
        left->size == right->size && left->type == right->type &&
        left->subtype == right->subtype && left->encrypted == right->encrypted &&
        left->readonly == right->readonly &&
        !memcmp(left->label, right->label, sizeof(left->label));
}

static int role_index(const struct board_layout *layout,
    const esp_partition_t *role, uint8_t *index)
{
    size_t i;
    *index = BOARD_DISCOVERY_UNKNOWN_ROLE;
    if (!role) return 1;
    if (role->flash_chip != esp_flash_default_chip ||
        !memchr(role->label, 0, sizeof(role->label))) return 0;
    for (i = 0; i < layout->count; ++i) {
        const struct board_partition *part = &layout->partitions[i];
        if (role->address == part->offset && role->size == part->size &&
            (unsigned)role->type == part->type && (unsigned)role->subtype == part->subtype &&
            !strcmp(role->label, part->label) &&
            part->flags == ((role->encrypted ? 1u : 0u) | (role->readonly ? 2u : 0u))) {
            *index = (uint8_t)i;
            return 1;
        }
    }
    return 0;
}

int c6_board_discovery_collect(struct board_discovery_snapshot *output)
{
    struct board_discovery_snapshot snapshot = {0};
    uint8_t table[(BOARD_LAYOUT_MAX_PARTITIONS + 1u) * 32u];
    esp_partition_iterator_t iterator;
    const esp_partition_t *running, *boot, *update;
    esp_ota_img_states_t state;
    esp_err_t state_result;
#ifndef CONFIG_EFUSE_VIRTUAL
    esp_chip_info_t chip_info = {0};
    size_t secure_version_count = 0;
#endif
    size_t count = 0, i;
    if (!output || !esp_flash_default_chip) return 0;
#ifndef CONFIG_EFUSE_VIRTUAL
    esp_chip_info(&chip_info);
    if (chip_info.model != CHIP_ESP32C6) return 0;
    snapshot.target_known = BOARD_DISCOVERY_TARGET_CHIP |
                            BOARD_DISCOVERY_TARGET_REVISION;
    snapshot.chip_id = BOARD_DISCOVERY_CHIP_C6;
    snapshot.chip_revision = chip_info.revision;
    /* Read the complete physical field's set-bit count. This is not proof of
     * the installed bootloader's anti-rollback enforcement or its policy. */
    if (esp_efuse_read_field_cnt(ESP_EFUSE_SECURE_VERSION,
            &secure_version_count) == ESP_OK &&
        secure_version_count <= C6_SECURE_VERSION_FIELD_BITS) {
        snapshot.target_known |= BOARD_DISCOVERY_TARGET_SECURE_VERSION;
        snapshot.secure_version = (uint32_t)secure_version_count;
    }
#endif
    if (esp_flash_get_physical_size(NULL, &snapshot.physical_bytes) != ESP_OK ||
        esp_flash_get_size(NULL, &snapshot.layout.flash_bytes) != ESP_OK) return 0;
    running = esp_ota_get_running_partition();
    boot = esp_ota_get_boot_partition();
    update = esp_ota_get_next_update_partition(NULL);
    if (!running) return 0;
    memset(table, 0xff, sizeof(table));
    iterator = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (iterator) {
        const esp_partition_t *part = esp_partition_get(iterator);
        uint8_t *record = table + count * 32u;
        if (!part || count == BOARD_LAYOUT_MAX_PARTITIONS ||
            part->flash_chip != esp_flash_default_chip ||
            (unsigned)part->type > 255u || (unsigned)part->subtype > 255u) goto failed;
        memset(record, 0, 32u);
        record[0] = 0xaa; record[1] = 0x50;
        record[2] = (uint8_t)part->type; record[3] = (uint8_t)part->subtype;
        le32(record + 4, part->address); le32(record + 8, part->size);
        for (i = 0; i < sizeof(part->label); ++i) if (!part->label[i]) break;
        if (!i || i > 16u) goto failed;
        memcpy(record + 12, part->label, i);
        le32(record + 28, (part->encrypted ? 1u : 0u) | (part->readonly ? 2u : 0u));
        ++count;
        iterator = esp_partition_next(iterator); /* consumes current iterator */
    }
    if (!board_layout_parse(table, (count + 1u) * 32u,
            snapshot.layout.flash_bytes, &snapshot.layout) ||
        !role_index(&snapshot.layout, running, &snapshot.running) ||
        !role_index(&snapshot.layout, boot, &snapshot.boot) ||
        !role_index(&snapshot.layout, update, &snapshot.update)) return 0;
    state_result = esp_ota_get_state_partition(running, &state);
    if (state_result == ESP_OK) {
        switch (state) {
        case ESP_OTA_IMG_NEW: snapshot.running_state = 1; break;
        case ESP_OTA_IMG_PENDING_VERIFY: snapshot.running_state = 2; break;
        case ESP_OTA_IMG_VALID: snapshot.running_state = 3; break;
        case ESP_OTA_IMG_INVALID: snapshot.running_state = 4; break;
        case ESP_OTA_IMG_ABORTED: snapshot.running_state = 5; break;
        case ESP_OTA_IMG_UNDEFINED: snapshot.running_state = 6; break;
        default: return 0;
        }
    } else if (state_result != ESP_ERR_NOT_SUPPORTED && state_result != ESP_ERR_NOT_FOUND)
        return 0;
#ifndef CONFIG_EFUSE_VIRTUAL
    snapshot.security_known = BOARD_DISCOVERY_SECURITY_ENCRYPTION |
                              BOARD_DISCOVERY_SECURITY_SECURE_BOOT;
    if (esp_flash_encryption_enabled()) snapshot.security_enabled |= BOARD_DISCOVERY_SECURITY_ENCRYPTION;
    if (esp_secure_boot_enabled()) snapshot.security_enabled |= BOARD_DISCOVERY_SECURITY_SECURE_BOOT;
#endif
    /* App sdkconfig cannot establish the installed bootloader's rollback or
     * anti-rollback enforcement. Both stay explicitly unknown. */
    if (!same(running, esp_ota_get_running_partition()) ||
        !same(boot, esp_ota_get_boot_partition()) ||
        !same(update, esp_ota_get_next_update_partition(NULL)) ||
        !board_discovery_validate(&snapshot)) return 0;
    *output = snapshot;
    return 1;
failed:
    esp_partition_iterator_release(iterator);
    return 0;
}
