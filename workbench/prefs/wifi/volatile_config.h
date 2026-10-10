/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef WIFI_VOLATILE_CONFIG_H
#define WIFI_VOLATILE_CONFIG_H

#include <exec/types.h>
#include <stddef.h>

BOOL WifiSSIDHex(char *output, size_t capacity, CONST_STRPTR ssid);
BOOL WifiFormatVolatilePrefs(char *output, size_t capacity,
    CONST_STRPTR ssid, CONST_STRPTR key, BOOL protected);
BOOL WifiWriteVolatilePrefs(CONST_STRPTR ssid, CONST_STRPTR key,
    BOOL protected);
void WifiClearSecret(APTR buffer, size_t length);

#endif
