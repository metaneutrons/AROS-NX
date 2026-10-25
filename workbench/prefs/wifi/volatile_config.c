/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <dos/dos.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
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

enum WifiSecurityProfile WifiSecurityProfileFromLabel(CONST_STRPTR label)
{
    if (label == NULL)
        return WIFI_SECURITY_UNKNOWN;
    if (strcmp((const char *)label, "open") == 0)
        return WIFI_SECURITY_OPEN;
    if (strcmp((const char *)label, "WPA") == 0 ||
        strcmp((const char *)label, "WPA2") == 0)
        return WIFI_SECURITY_PSK;
    /* Transition-mode availability is not permission to downgrade. */
    if (strcmp((const char *)label, "WPA3") == 0 ||
        strcmp((const char *)label, "WPA2/3") == 0)
        return WIFI_SECURITY_SAE;
    return WIFI_SECURITY_UNKNOWN;
}

BOOL WifiValidateProfileCredential(enum WifiSecurityProfile profile,
    CONST_STRPTR key)
{
    size_t length = 0;
    size_t minimum;

    if (profile == WIFI_SECURITY_OPEN)
        return key == NULL || key[0] == '\0';
    if (profile == WIFI_SECURITY_SAE)
        minimum = 1;
    else if (profile == WIFI_SECURITY_PSK)
        minimum = 8;
    else
        return FALSE;

    if (key == NULL)
        return FALSE;
    for (; length < 64 && key[length] != '\0'; ++length)
        if ((UBYTE)key[length] < 32 || (UBYTE)key[length] > 126)
            return FALSE;
    return length >= minimum && length <= 63;
}

static BOOL WifiHexEncode(char *output, size_t capacity,
    CONST_STRPTR input, size_t length)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    if (output == NULL || capacity == 0)
        return FALSE;
    WifiClearSecret(output, capacity);
    if (input == NULL || capacity <= length * 2)
        return FALSE;
    for (i = 0; i < length; ++i)
    {
        output[2 * i] = hex[((UBYTE)input[i]) >> 4];
        output[2 * i + 1] = hex[((UBYTE)input[i]) & 15];
    }
    return TRUE;
}

static size_t WifiKeyLength(CONST_STRPTR key)
{
    size_t length = 0;
    while (length < 64 && key[length] != '\0')
        length++;
    return length;
}

/* Emit one volatile selection. SSIDs and SAE credentials are hex encoded so
 * printable quotes and punctuation cannot become supplicant syntax. The
 * existing PSK format is retained for the WPA/WPA2 path. */
BOOL WifiFormatVolatileProfilePrefs(char *output, size_t capacity,
    CONST_STRPTR ssid, CONST_STRPTR key,
    enum WifiSecurityProfile profile)
{
    char encoded_ssid[65];
    char encoded_key[127];
    size_t key_length;
    int length;

    if (output == NULL || capacity == 0)
        return FALSE;
    WifiClearSecret(output, capacity);
    WifiClearSecret(encoded_ssid, sizeof(encoded_ssid));
    WifiClearSecret(encoded_key, sizeof(encoded_key));
    if (!WifiSSIDHex(encoded_ssid, sizeof(encoded_ssid), ssid) ||
        (profile != WIFI_SECURITY_OPEN && profile != WIFI_SECURITY_PSK &&
         profile != WIFI_SECURITY_SAE) ||
        !WifiValidateProfileCredential(profile, key))
        goto fail;

    key_length = profile == WIFI_SECURITY_OPEN ? 0 : WifiKeyLength(key);
    if (profile == WIFI_SECURITY_SAE &&
        !WifiHexEncode(encoded_key, sizeof(encoded_key), key, key_length))
        goto fail;

    if (profile == WIFI_SECURITY_SAE)
        length = snprintf(output, capacity,
            "ap_scan=1\nnetwork={\n\tssid=%s\n"
            "\tsae_password=%s\n\tkey_mgmt=SAE\n\tproto=RSN\n"
            "\tpairwise=CCMP\n\tgroup=CCMP\n}\n",
            encoded_ssid, encoded_key);
    else if (profile == WIFI_SECURITY_PSK)
        length = snprintf(output, capacity,
            "ap_scan=1\nnetwork={\n\tssid=%s\n"
            "\tpsk=\"%s\"\n\tkey_mgmt=WPA-PSK\n}\n",
            encoded_ssid, (const char *)key);
    else
        length = snprintf(output, capacity,
            "ap_scan=1\nnetwork={\n\tssid=%s\n\tkey_mgmt=NONE\n}\n",
            encoded_ssid);

    if (length < 0 || (size_t)length >= capacity)
        goto fail;
    WifiClearSecret(encoded_ssid, sizeof(encoded_ssid));
    WifiClearSecret(encoded_key, sizeof(encoded_key));
    return TRUE;

fail:
    WifiClearSecret(output, capacity);
    WifiClearSecret(encoded_ssid, sizeof(encoded_ssid));
    WifiClearSecret(encoded_key, sizeof(encoded_key));
    return FALSE;
}

/* Canonical block used by both ENVARC and ENV persistent configuration. */
BOOL WifiFormatPersistentProfileBlock(char *output, size_t capacity,
    CONST_STRPTR ssid, CONST_STRPTR key,
    enum WifiSecurityProfile profile)
{
    char encoded_ssid[65];
    char encoded_key[127];
    size_t key_length;
    int length;

    if (output == NULL || capacity == 0)
        return FALSE;
    WifiClearSecret(output, capacity);
    WifiClearSecret(encoded_ssid, sizeof(encoded_ssid));
    WifiClearSecret(encoded_key, sizeof(encoded_key));
    if (!WifiSSIDHex(encoded_ssid, sizeof(encoded_ssid), ssid) ||
        (profile != WIFI_SECURITY_OPEN && profile != WIFI_SECURITY_PSK &&
         profile != WIFI_SECURITY_SAE) ||
        !WifiValidateProfileCredential(profile, key))
        goto fail;

    key_length = profile == WIFI_SECURITY_OPEN ? 0 : WifiKeyLength(key);
    if (profile == WIFI_SECURITY_SAE &&
        !WifiHexEncode(encoded_key, sizeof(encoded_key), key, key_length))
        goto fail;

    if (profile == WIFI_SECURITY_SAE)
        length = snprintf(output, capacity,
            "network={\n\tssid=%s\n\tsae_password=%s\n"
            "\tkey_mgmt=SAE\n\tproto=RSN\n\tpairwise=CCMP\n"
            "\tgroup=CCMP\n}\n\n", encoded_ssid, encoded_key);
    else if (profile == WIFI_SECURITY_PSK)
        length = snprintf(output, capacity,
            "network={\n\tssid=%s\n\tpsk=\"%s\"\n"
            "\tkey_mgmt=WPA-PSK\n}\n\n",
            encoded_ssid, (const char *)key);
    else
        length = snprintf(output, capacity,
            "network={\n\tssid=%s\n\tkey_mgmt=NONE\n}\n\n",
            encoded_ssid);

    if (length < 0 || (size_t)length >= capacity)
        goto fail;
    WifiClearSecret(encoded_ssid, sizeof(encoded_ssid));
    WifiClearSecret(encoded_key, sizeof(encoded_key));
    return TRUE;

fail:
    WifiClearSecret(output, capacity);
    WifiClearSecret(encoded_ssid, sizeof(encoded_ssid));
    WifiClearSecret(encoded_key, sizeof(encoded_key));
    return FALSE;
}

/* Minimal bounded matcher for merge replacement. It recognizes only the
 * quoted form previously emitted by this panel and the new hex SSID form. */
static int WifiHexNibble(UBYTE value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

static BOOL WifiSSIDValueMatches(CONST_STRPTR value, size_t value_length,
    CONST_STRPTR ssid, size_t ssid_length)
{
    size_t i;

    if (value_length >= 2 && value[0] == '"' &&
        value[value_length - 1] == '"')
        return value_length - 2 == ssid_length &&
            memcmp(value + 1, ssid, ssid_length) == 0;
    if (value_length != ssid_length * 2)
        return FALSE;
    for (i = 0; i < ssid_length; ++i)
    {
        int high = WifiHexNibble((UBYTE)value[2 * i]);
        int low = WifiHexNibble((UBYTE)value[2 * i + 1]);
        if (high < 0 || low < 0 ||
            (UBYTE)ssid[i] != (UBYTE)((high << 4) | low))
            return FALSE;
    }
    return TRUE;
}

/* Find the end of a network block without treating a quoted brace or a
 * comment as its delimiter. This is deliberately only a bounded delimiter
 * scan, not a second WirelessManager configuration parser. */
BOOL WifiConfigBlockLength(CONST_STRPTR block, size_t available,
    size_t *block_length)
{
    BOOL comment = FALSE;
    size_t i;

    if (block_length != NULL)
        *block_length = 0;
    if (block == NULL || block_length == NULL)
        return FALSE;

    for (i = 0; i < available; ++i)
    {
        UBYTE ch = (UBYTE)block[i];
        if (comment)
        {
            if (ch == '\n')
                comment = FALSE;
            continue;
        }
        if (ch == '#')
            comment = TRUE;
        else if (ch == '"')
        {
            /* WirelessManager's quoted-value reader uses the last quote on
             * the line as the delimiter. Match it here: legacy printable
             * PSKs could contain raw quotes and braces. */
            size_t line_end = i;
            size_t quote_end;
            while (line_end < available && block[line_end] != '\n')
                line_end++;
            quote_end = line_end;
            while (quote_end > i + 1 && block[quote_end - 1] != '"')
                quote_end--;
            if (quote_end <= i + 1)
                return FALSE;
            i = quote_end - 1;
        }
        else if (ch == '}')
        {
            *block_length = i + 1;
            return TRUE;
        }
    }
    return FALSE;
}

BOOL WifiConfigBlockMatchesSSID(CONST_STRPTR block, size_t length,
    CONST_STRPTR ssid)
{
    size_t ssid_length = 0, offset = 0;

    if (block == NULL || ssid == NULL)
        return FALSE;
    while (ssid_length < 33 && ssid[ssid_length] != '\0')
        ssid_length++;
    if (ssid_length == 0 || ssid_length > 32)
        return FALSE;

    while (offset < length)
    {
        size_t start = offset, line_length, p, equal, key_end;
        CONST_STRPTR value;
        size_t value_length;

        while (offset < length && block[offset] != '\n')
            offset++;
        line_length = offset - start;
        if (offset < length)
            offset++;
        while (line_length != 0 && block[start + line_length - 1] == '\r')
            line_length--;
        p = start;
        while (p < start + line_length &&
               (block[p] == ' ' || block[p] == '\t'))
            p++;
        for (equal = p; equal < start + line_length && block[equal] != '=';
             ++equal)
            ;
        if (equal == start + line_length)
            continue;
        key_end = equal;
        while (key_end > p && (block[key_end - 1] == ' ' ||
                               block[key_end - 1] == '\t'))
            key_end--;
        if (key_end - p != 4 || memcmp(block + p, "ssid", 4) != 0)
            continue;
        p = equal + 1;
        while (p < start + line_length &&
               (block[p] == ' ' || block[p] == '\t'))
            p++;
        value = block + p;
        value_length = start + line_length - p;
        while (value_length != 0 &&
               (value[value_length - 1] == ' ' ||
                value[value_length - 1] == '\t'))
            value_length--;
        if (WifiSSIDValueMatches(value, value_length, ssid, ssid_length))
            return TRUE;
    }
    return FALSE;
}

/* Legacy interface retained for the existing volatile configuration tests. */
BOOL WifiFormatVolatilePrefs(char *output, size_t capacity,
    CONST_STRPTR ssid, CONST_STRPTR key, BOOL protected)
{
    return WifiFormatVolatileProfilePrefs(output, capacity, ssid, key,
        protected ? WIFI_SECURITY_PSK : WIFI_SECURITY_OPEN);
}

static BOOL WifiWriteVolatileText(CONST_STRPTR text)
{
    BPTR file;
    BOOL result = FALSE;

    file = Open((CONST_STRPTR)"ENV:Sys/Wireless.prefs", MODE_NEWFILE);
    if (file != BNULL)
    {
        result = FPuts(file, text) == 0;
        if (!Close(file))
            result = FALSE;
    }
    return result;
}

BOOL WifiWriteVolatileProfilePrefs(CONST_STRPTR ssid, CONST_STRPTR key,
    enum WifiSecurityProfile profile)
{
    char output[384];
    BOOL result;

    if (!WifiFormatVolatileProfilePrefs(output, sizeof(output), ssid, key,
            profile))
        return FALSE;
    result = WifiWriteVolatileText((CONST_STRPTR)output);
    WifiClearSecret(output, sizeof(output));
    return result;
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
