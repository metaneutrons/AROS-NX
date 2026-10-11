/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef WIFI_VOLATILE_CONFIG_H
#define WIFI_VOLATILE_CONFIG_H

#include <exec/types.h>
#include <stddef.h>

/* Keep the security selection typed from the scan through preference output.
 * UNKNOWN is deliberately not a protected/PSK alias. */
enum WifiSecurityProfile
{
    WIFI_SECURITY_OPEN,
    WIFI_SECURITY_PSK,
    WIFI_SECURITY_SAE,
    WIFI_SECURITY_UNKNOWN
};

enum WifiSecurityProfile WifiSecurityProfileFromLabel(CONST_STRPTR label);
BOOL WifiValidateProfileCredential(enum WifiSecurityProfile profile,
    CONST_STRPTR key);
BOOL WifiConfigBlockMatchesSSID(CONST_STRPTR block, size_t length,
    CONST_STRPTR ssid);
BOOL WifiConfigBlockLength(CONST_STRPTR block, size_t available,
    size_t *block_length);
BOOL WifiFormatVolatileProfilePrefs(char *output, size_t capacity,
    CONST_STRPTR ssid, CONST_STRPTR key,
    enum WifiSecurityProfile profile);
BOOL WifiWriteVolatileProfilePrefs(CONST_STRPTR ssid, CONST_STRPTR key,
    enum WifiSecurityProfile profile);
BOOL WifiFormatPersistentProfileBlock(char *output, size_t capacity,
    CONST_STRPTR ssid, CONST_STRPTR key,
    enum WifiSecurityProfile profile);

/* Legacy PSK/open formatter entry points retained for existing callers. */
BOOL WifiSSIDHex(char *output, size_t capacity, CONST_STRPTR ssid);
BOOL WifiFormatVolatilePrefs(char *output, size_t capacity,
    CONST_STRPTR ssid, CONST_STRPTR key, BOOL protected);
BOOL WifiWriteVolatilePrefs(CONST_STRPTR ssid, CONST_STRPTR key,
    BOOL protected);
void WifiClearSecret(APTR buffer, size_t length);

#endif
