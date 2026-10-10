/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <dos/dos.h>
#include <proto/dos.h>
#include <stdio.h>
#include "volatile_config.h"

void WifiClearSecret(APTR buffer, size_t length)
{
    volatile UBYTE *p = buffer;
    if (p != NULL)
        while (length-- != 0)
            *p++ = 0;
}

BOOL WifiSSIDHex(char *output, size_t capacity, CONST_STRPTR ssid)
{
    static const char hex[] = "0123456789abcdef";
    size_t n;

    if (output == NULL || capacity == 0)
        return FALSE;
    WifiClearSecret(output, capacity);
    if (ssid == NULL)
        return FALSE;
    for (n = 0; n < 33 && ssid[n] != '\0'; ++n)
        if (n == 32)
            return FALSE;
    if (n == 0 || capacity <= 2 * n)
        return FALSE;
    for (n = 0; ssid[n] != '\0'; ++n)
    {
        output[2 * n] = hex[((UBYTE)ssid[n]) >> 4];
        output[2 * n + 1] = hex[((UBYTE)ssid[n]) & 15];
    }
    return TRUE;
}

/* A single station selection, not a persistent list of remembered networks.
 * SSID bytes are hex: quotes, backslashes and newlines cannot inject fields.
 * WPA passphrases are printable ASCII8..63. The manager's parser uses the
 * final quote as the delimiter, retaining internal quotes/#/backslashes. */
BOOL WifiFormatVolatilePrefs(char *output, size_t capacity,
    CONST_STRPTR ssid, CONST_STRPTR key, BOOL protected)
{
    char encoded[65];
    size_t key_length = 0;
    int length;

    if (output == NULL || capacity == 0)
        return FALSE;
    WifiClearSecret(output, capacity);
    if (!WifiSSIDHex(encoded, sizeof(encoded), ssid))
        return FALSE;
    if (protected)
    {
        if (key == NULL)
            return FALSE;
        for (; key_length < 64 && key[key_length] != '\0'; ++key_length)
            if ((UBYTE)key[key_length] < 32 || (UBYTE)key[key_length] > 126)
                return FALSE;
        if (key_length < 8 || key_length > 63)
            return FALSE;
    }
    length = protected
        ? snprintf(output, capacity,
            "ap_scan=1\nnetwork={\n\tssid=%s\n\tpsk=\"%s\"\n"
            "\tkey_mgmt=WPA-PSK\n}\n", encoded, (const char *)key)
        : snprintf(output, capacity,
            "ap_scan=1\nnetwork={\n\tssid=%s\n\tkey_mgmt=NONE\n}\n", encoded);
    if (length < 0 || (size_t)length >= capacity)
    {
        WifiClearSecret(output, capacity);
        return FALSE;
    }
    return TRUE;
}

BOOL WifiWriteVolatilePrefs(CONST_STRPTR ssid, CONST_STRPTR key, BOOL protected)
{
    char output[256];
    BPTR file;
    BOOL result = FALSE;

    if (!WifiFormatVolatilePrefs(output, sizeof(output), ssid, key, protected))
        return FALSE;
    file = Open((CONST_STRPTR)"ENV:Sys/Wireless.prefs", MODE_NEWFILE);
    if (file != BNULL)
    {
        result = FPuts(file, (CONST_STRPTR)output) == 0;
        if (!Close(file))
            result = FALSE;
    }
    WifiClearSecret(output, sizeof(output));
    return result;
}
