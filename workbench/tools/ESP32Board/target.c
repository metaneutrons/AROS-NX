/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "target.h"

static int disjoint(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t ab = (uintptr_t)a, bb = (uintptr_t)b;
    return a && b && ab <= UINTPTR_MAX - an && bb <= UINTPTR_MAX - bn &&
        (ab + an <= bb || bb + bn <= ab);
}

static int revision_bound(uint16_t value)
{
    return value != 0u && value != UINT16_MAX;
}

int board_c6_target_compare(const struct board_image_info *image,
    const struct board_discovery_snapshot *target,
    struct board_c6_target_report *report)
{
    struct board_c6_target_report result = {0};
    const struct board_partition *slot;
    const unsigned security = BOARD_DISCOVERY_SECURITY_ENCRYPTION |
                              BOARD_DISCOVERY_SECURITY_SECURE_BOOT;
    if (!image || !target || !report || image->bytes < 288u ||
        image->bytes > BOARD_C6_IMAGE_MAX ||
        !disjoint(image, sizeof(*image), report, sizeof(*report)) ||
        !disjoint(target, sizeof(*target), report, sizeof(*report)) ||
        !board_discovery_validate(target)) return 0;
    /* Format preflight supports C6 only, so there is no inferred image chip. */
    if (!(target->target_known & BOARD_DISCOVERY_TARGET_CHIP))
        result.unknown |= BOARD_C6_TARGET_CHIP;
    if (!(target->target_known & BOARD_DISCOVERY_TARGET_REVISION))
        result.unknown |= BOARD_C6_TARGET_REVISION;
    else if (target->chip_revision < image->min_revision ||
        (revision_bound(image->max_revision) &&
         target->chip_revision > image->max_revision))
        result.rejected |= BOARD_C6_TARGET_REVISION;
    if (!(target->target_known & BOARD_DISCOVERY_TARGET_SECURE_VERSION))
        result.unknown |= BOARD_C6_TARGET_COUNTER;
    else if (target->secure_version > 16u)
        return 0; /* Impossible full physical C6 counter. */
    else if (image->secure_version < target->secure_version)
        result.risk |= BOARD_C6_TARGET_COUNTER;
    if ((target->security_known & security) != security)
        result.unknown |= BOARD_C6_TARGET_SECURITY;
    /* Current backend deliberately supports unsigned/unencrypted images only.
     * An IDF OTA encryption capability must not be inferred from this status. */
    if (target->security_enabled & security)
        result.rejected |= BOARD_C6_TARGET_SECURITY;
    if (target->update == BOARD_DISCOVERY_UNKNOWN_ROLE ||
        target->running == BOARD_DISCOVERY_UNKNOWN_ROLE ||
        target->boot == BOARD_DISCOVERY_UNKNOWN_ROLE)
        result.unknown |= BOARD_C6_TARGET_SLOT;
    else {
        slot = &target->layout.partitions[target->update];
        if (target->update == target->running || target->update == target->boot ||
            slot->flags || image->bytes > slot->size ||
            target->running_state == 1u || target->running_state == 2u ||
            target->running_state == 4u || target->running_state == 5u)
            result.rejected |= BOARD_C6_TARGET_SLOT;
        if (!target->running_state)
            result.unknown |= BOARD_C6_TARGET_SLOT;
    }
    if (revision_bound(image->min_efuse_revision) ||
        revision_bound(image->max_efuse_revision))
        result.unknown |= BOARD_C6_TARGET_BLOCK_REV;
    if (image->mmu_page_size)
        result.unknown |= BOARD_C6_TARGET_MMU;
    /* Even zero/range-compatible metadata cannot fabricate these proofs. */
    result.unknown |= BOARD_C6_TARGET_POLICY | BOARD_C6_TARGET_PROVENANCE;
    *report = result;
    return 1;
}
