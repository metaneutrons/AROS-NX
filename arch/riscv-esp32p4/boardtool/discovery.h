/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_BOARD_DISCOVERY_H
#define P4_BOARD_DISCOVERY_H
#include "layout.h"

#define BOARD_DISCOVERY_UNKNOWN_ROLE 255u
#define BOARD_DISCOVERY_SECURITY_ENCRYPTION 1u
#define BOARD_DISCOVERY_SECURITY_SECURE_BOOT 2u
#define BOARD_DISCOVERY_SECURITY_ANTI_ROLLBACK 4u
#define BOARD_DISCOVERY_SECURITY_ROLLBACK 8u
#define BOARD_DISCOVERY_SECURITY_ALL 15u
#define BOARD_DISCOVERY_PART_ENCRYPTED 1u
#define BOARD_DISCOVERY_PART_READONLY 2u

/* Normalized internal IDF registry inventory, not raw partition-table bytes
 * or write authority. Partition flags are normalized encryption bit0 and
 * readonly bit1, not all original on-flash flags. IDF's registry can include
 * runtime registrations; proving correspondence with the installed table is
 * an additional adapter/qualification requirement.
 * physical_bytes is detected chip capacity; layout.flash_bytes is the usable
 * image/driver limit. Unknown policy is represented by a clear known bit,
 * never by an assumed disabled value. Role indices refer to sorted entries.
 * State0=unknown,1=new,2=pending,3=valid,4=invalid,5=aborted,6=undefined.
 * No pointer or native struct is a wire format. */
struct board_discovery_snapshot {
    struct board_layout layout;
    uint32_t physical_bytes;
    uint8_t running, boot, update, running_state;
    uint8_t security_known, security_enabled;
};

/* Structural validation only. Caller must separately establish origin,
 * freshness, negotiated protocol, image compatibility and recovery. */
int board_discovery_validate(const struct board_discovery_snapshot *snapshot);
#endif
