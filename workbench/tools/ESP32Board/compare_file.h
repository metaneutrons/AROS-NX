/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef ESP32BOARD_COMPARE_FILE_H
#define ESP32BOARD_COMPARE_FILE_H
#include "target.h"
#include "../../../arch/riscv-esp32p4/boardtool/discovery_client.h"
/* Tool-private synchronous read-only operation. All inputs/outputs must be
 * disjoint. Report remains unchanged on failure; an observation is historical
 * after query release. Return1 means report produced, never admission.
 * Native query stops WLAN and leaves it offline. */
int boardtool_compare_c6(const char *, const char *,
    struct board_client_observation *, struct board_c6_target_report *, char *, size_t);
#endif
