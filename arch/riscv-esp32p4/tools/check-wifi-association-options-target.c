/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Compile with the configured RV32 generated headers and Developer sysroot.
 * This exports a call to the actual parser to retain its target code; no host
 * ABI substitution and no runtime/hardware qualification is implied. */
#include "../network/association_options.h"
#include <stddef.h>

typedef char p4wlan_options_iptr_is_rv32[sizeof(IPTR) == 4 ? 1 : -1];
typedef char p4wlan_options_target_layout[
    sizeof(BOOL) == 2 &&
    sizeof(struct p4_wifi_association_target) == 120 &&
    offsetof(struct p4_wifi_association_target, password) == 36 &&
    offsetof(struct p4_wifi_association_target, bssid_set) == 110 ? 1 : -1];

BOOL p4wlan_check_options_target(struct p4_wifi_association_target *target,
                                const struct TagItem *tags)
{
    return p4wlan_copy_association_options(target, tags);
}
