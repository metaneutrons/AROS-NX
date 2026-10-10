/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef C6_BOARD_DISCOVERY_H
#define C6_BOARD_DISCOVERY_H
#include "../../boardtool/discovery.h"
/* C6 IDF5.4.3 adapter. Must run in the serialized RPC/OTA dispatcher, with no
 * concurrent slot selection or registry mutation (registration,
 * deregistration or unload). Bounded normalized
 * internal registry metadata only; no flash read
 * of app/NVS/eFuse contents, erase, write, boot selection, reset or Wi-Fi call.
 * Output unchanged on failure and never grants update permission. RPC wiring,
 * actual-table correspondence and dispatcher stack budget remain to qualify. */
int c6_board_discovery_collect(struct board_discovery_snapshot *output);
#endif
