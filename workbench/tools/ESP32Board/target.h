/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef ESP32BOARD_TARGET_H
#define ESP32BOARD_TARGET_H
#include "image.h"
#include "../../../arch/riscv-esp32p4/boardtool/discovery.h"

#define BOARD_C6_TARGET_CHIP       1u
#define BOARD_C6_TARGET_REVISION   2u
#define BOARD_C6_TARGET_COUNTER    4u
#define BOARD_C6_TARGET_SECURITY   8u
#define BOARD_C6_TARGET_SLOT       16u
#define BOARD_C6_TARGET_BLOCK_REV  32u
#define BOARD_C6_TARGET_POLICY     64u
#define BOARD_C6_TARGET_PROVENANCE 128u
#define BOARD_C6_TARGET_MMU        256u

struct board_c6_target_report {
    uint32_t rejected, unknown, risk;
};

/* Read-only comparison of inspected image metadata and a structurally valid
 * historical discovery-v2 snapshot. No device authentication/freshness, raw
 * installed-table provenance or bootloader policy is supplied by either input.
 * Therefore POLICY and PROVENANCE remain unknown even if other facts match.
 * Maximum chip revision is conservatively enforced without assuming IDF's
 * device-specific disable-wafer-version override. A lower image secure_version
 * than the full physical counter is a risk, NOT a proven effective-floor
 * violation: installed bootloader width/enforcement are unknown.
 * Block revision/MMU facts are absent from v2; constraints stay unknown.
 * Return1 means a report was produced, NEVER update compatibility/admission.
 * Inputs immutable/disjoint from report; output unchanged on invalid input. */
int board_c6_target_compare(const struct board_image_info *,
    const struct board_discovery_snapshot *, struct board_c6_target_report *);
#endif
