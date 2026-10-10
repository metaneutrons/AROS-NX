/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4WLAN_ASSOCIATION_OPTIONS_H
#define P4WLAN_ASSOCIATION_OPTIONS_H

#include "device_bridge.h"
#include <devices/sana2wireless.h>
#include <utility/tagitem.h>

/* Borrowed SANA pointers must be readable through their declared extent until
 * command Reply. The IE extent must come from a validated explicit caller
 * length, not from ie[1]. Native binding remains disabled until the shared
 * caller can convey that extent. An arbitrary pointer's allocation cannot be
 * established here; the caller must supply a readable span of that length.
 * Output is a fresh device-owned allocation and must not alias borrowed input.
 * No borrowed pointer is retained. This first profile deliberately refuses
 * WPA1, mixed suites, enterprise, FT, SAE, PMF and cached PMKIDs. */
static BOOL p4wlan_rsn_psk_ccmp(const UBYTE *ie, ULONG extent)
{
    static const UBYTE prefix[] = {
        48, 0, 1, 0,             /* RSN element, length checked separately */
        0, 15, 172, 4,          /* group CCMP */
        1, 0, 0, 15, 172, 4,    /* one pairwise CCMP */
        1, 0, 0, 15, 172, 2     /* one AKM PSK */
    };
    ULONG i;
    if (!ie || (extent != 20 && extent != 22 && extent != 24) ||
        (ULONG)ie[1] + 2u != extent)
        return FALSE;
    for (i = 0; i < sizeof(prefix); ++i)
        if (i != 1 && ie[i] != prefix[i])
            return FALSE;
    /* Supported RSN capabilities: preauthentication and replay counters only.
     * No no-pairwise, peerkey, PMF, SPP, extended key IDs or reserved bits. */
    if (ie[1] >= 20 && ((ie[20] & ~0x3du) || ie[21]))
        return FALSE;
    /* A trailing, explicitly empty PMKID list is representable; keys aren't. */
    if (ie[1] == 22 && (ie[22] || ie[23]))
        return FALSE;
    return TRUE;
}

static void p4wlan_options_wipe(struct p4_wifi_association_target *target)
{
    volatile UBYTE *p = (volatile UBYTE *)target;
    ULONG n = sizeof(*target);
    while (n--)
        *p++ = 0;
}

/* The shared caller passes WPA_AUTH_ALG_OPEN (bit0), not an S2-auth enum.
 * Insist on that exact value; never infer a cipher from auth-alg alone.
 * Success produces one owned target. Failure leaves the entire output zero.
 * TAG_MORE cycles and TAG_SKIP are bounded by one 128-item traversal budget. */
static BOOL p4wlan_copy_association_options(
    struct p4_wifi_association_target *target, const struct TagItem *tags,
    ULONG wpa_ie_extent)
{
    ULONG visited = 0, seen = 0, i;
    BOOL ended = FALSE, protected_profile = FALSE;
    enum { SSID = 1, BSSID = 2, PORT = 4, CHANNEL = 8, AUTH = 16,
           WPA = 32, PASSPHRASE = 64, PSK = 128 };
    if (!target)
        return FALSE;
    p4wlan_options_wipe(target);
    if (!tags)
        return FALSE;
    while (tags && visited < 128u) {
        ULONG tag = tags->ti_Tag, bit = 0;
        IPTR data = tags->ti_Data;
        ++tags;
        ++visited;
        if (tag == TAG_DONE) { ended = TRUE; break; }
        if (tag == TAG_IGNORE) continue;
        if (tag == TAG_MORE) {
            tags = (const struct TagItem *)data;
            if (!tags) ended = TRUE; /* standard TAG_MORE(NULL) termination */
            continue;
        }
        if (tag == TAG_SKIP) {
            IPTR address = (IPTR)tags, skip = data;
            if (skip > 128u - visited ||
                skip > (~(IPTR)0 - address) / sizeof(*tags))
                goto refused;
            tags = (const struct TagItem *)(address + skip * sizeof(*tags));
            visited += (ULONG)skip;
            continue;
        }
        switch (tag) {
        case S2INFO_SSID: bit = SSID; break;
        case S2INFO_BSSID: bit = BSSID; break;
        case S2INFO_PortType: bit = PORT; break;
        case S2INFO_Channel: bit = CHANNEL; break;
        case S2INFO_AuthTypes: bit = AUTH; break;
        case S2INFO_WPAInfo: bit = WPA; break;
        case S2INFO_Passphrase: bit = PASSPHRASE; break;
        case S2INFO_PSK: bit = PSK; break;
        default: goto refused;
        }
        if (seen & bit) goto refused;
        seen |= bit;
        if (bit == SSID) {
            const UBYTE *s = (const UBYTE *)data;
            if (!s) goto refused;
            for (i = 0; i < 32u && s[i]; ++i) target->ssid[i] = s[i];
            if (!i || (i == 32u && s[i])) goto refused;
            target->ssid_length = i;
        } else if (bit == BSSID) {
            const UBYTE *b = (const UBYTE *)data;
            UBYTE nonzero = 0;
            if (!b) goto refused;
            for (i = 0; i < 6; ++i) { target->bssid[i] = b[i]; nonzero |= b[i]; }
            if (!nonzero || (b[0] & 1u)) goto refused;
            target->bssid_set = TRUE;
        } else if (bit == PORT) {
            if (data != S2PORT_MANAGED) goto refused;
        } else if (bit == CHANNEL) {
            if (data > 14u) goto refused;
            target->channel = (ULONG)data;
        } else if (bit == AUTH) {
            if (data != 1u) goto refused;
        } else if (bit == WPA) {
            if (data && !p4wlan_rsn_psk_ccmp((const UBYTE *)data,
                                           wpa_ie_extent)) goto refused;
            if (!data && wpa_ie_extent) goto refused;
            protected_profile = data != 0;
        } else if (bit == PASSPHRASE) {
            const UBYTE *s = (const UBYTE *)data;
            if (!s || (seen & PSK)) goto refused;
            for (i = 0; i < 64u && s[i]; ++i) {
                if (s[i] < 32 || s[i] > 126) goto refused;
                target->password[i] = s[i];
            }
            if (i < 8 || i > 63) goto refused;
            target->password_length = i;
        } else if (bit == PSK) {
            const UBYTE *s = (const UBYTE *)data;
            static const UBYTE hex[] = "0123456789abcdef";
            if (!s || (seen & PASSPHRASE)) goto refused;
            for (i = 0; i < 32; ++i) {
                target->password[i * 2] = hex[s[i] >> 4];
                target->password[i * 2 + 1] = hex[s[i] & 15];
            }
            target->password_length = 64;
        }
    }
    if (!ended || (seen & (SSID | PORT | AUTH | WPA)) !=
                            (SSID | PORT | AUTH | WPA)) goto refused;
    if (protected_profile != (target->password_length != 0)) goto refused;
    target->authmode = protected_profile ? 3u : 0u;
    return TRUE;
refused:
    p4wlan_options_wipe(target);
    return FALSE;
}

#endif
