/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef ESP32P4_SDMMC_CLOCK_H
#define ESP32P4_SDMMC_CLOCK_H
#include "host.h"
BOOL p4host_reset(struct P4SDMMCHost *host);
BOOL p4host_set_clock(struct P4SDMMCHost *host, unsigned slot,
                       ULONG requested, BOOL low_power, ULONG *actual);
#endif
