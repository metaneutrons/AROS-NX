/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef C6_BOARD_DISCOVERY_H
#define C6_BOARD_DISCOVERY_H
#include "../../boardtool/discovery.h"
/* C6 IDF5.4.3 adapter. Must run in the serialized RPC/OTA dispatcher, with no
 * concurrent slot selection or registry mutation (registration,
 * deregistration or unload). Capacity-bounded normalized registry metadata;
 * reads chip model/revision and the full non-secret 16-bit eFuse secure-version
 * burned-bit count. IDF's field-count API may retry while an eFuse burn is in
 * progress, so whole-call runtime is not bounded until concurrent eFuse writes
 * are excluded or qualified. This count does not establish bootloader policy.
 * Encryption/secure-boot APIs expose status predicates only. No eFuse key
 * material or raw eFuse blocks, NVS or app contents are read; no erase/write,
 * boot selection, reset or Wi-Fi call occurs.
 * Output unchanged on failure and never grants update permission. RPC wiring,
 * actual-table correspondence and dispatcher stack budget remain to qualify. */
int c6_board_discovery_collect(struct board_discovery_snapshot *output);
#endif
