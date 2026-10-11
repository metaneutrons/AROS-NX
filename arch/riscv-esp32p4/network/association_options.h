/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4WLAN_ASSOCIATION_OPTIONS_H
#define P4WLAN_ASSOCIATION_OPTIONS_H

#include "device_bridge.h"
#include <devices/sana2wireless.h>
#include <devices/sana2wireless_fullmac.h>

/* Private owned targets and public negotiated profiles intentionally share
 * these values. Fail compilation if either contract changes independently. */
typedef char p4wlan_fullmac_profile_values_match[
    P4_WIFI_FULLMAC_OPEN == S2FULLMAC_PROFILE_OPEN &&
    P4_WIFI_FULLMAC_WPA2_CCMP == S2FULLMAC_PROFILE_WPA2_CCMP &&
    P4_WIFI_FULLMAC_SAE_CCMP_PMF == S2FULLMAC_PROFILE_SAE_CCMP_PMF ? 1 : -1];
#include <utility/tagitem.h>

/* A leave request is a standalone command, never mixed with credentials or
 * a new profile. Effective tags and TAG_MORE/SKIP share one bounded budget. */
static BOOL p4wlan_disassociate_options(const struct TagItem *tags)
{
    ULONG visited = 0;
    BOOL seen = FALSE;
    while (tags && visited < 128u) {
        ULONG tag = tags->ti_Tag;
        IPTR data = tags->ti_Data;
        ++tags;
        ++visited;
        if (tag == TAG_DONE) return seen;
        if (tag == TAG_IGNORE) continue;
        if (tag == TAG_MORE) {
            tags = (const struct TagItem *)data;
            if (!tags) return seen;
            continue;
        }
        if (tag == TAG_SKIP) {
            IPTR address = (IPTR)tags;
            if (data > 128u - visited ||
                data > (~(IPTR)0 - address) / sizeof(*tags)) return FALSE;
            tags = (const struct TagItem *)(address + data * sizeof(*tags));
            visited += (ULONG)data;
            continue;
        }
        if (tag != S2INFO_Disassociate || data != TRUE || seen) return FALSE;
        seen = TRUE;
    }
    return FALSE;
}

/* Borrowed SANA pointers must be readable through their declared extent until
 * command Reply. The IE extent must come from a validated explicit caller
 * WPAInfoLength tag, not from ie[1]. Native binding remains disabled until the
 * queue/lifecycle and privacy gates pass. An arbitrary pointer's allocation cannot be
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
static BOOL p4wlan_copy_association_options_cookie(
    struct p4_wifi_association_target *target, ULONG *cookie,
    const struct TagItem *tags)
{
    ULONG visited = 0, seen = 0, i;
    ULONG wpa_ie_extent = 0;
    const UBYTE *wpa_ie = NULL;
    BOOL ended = FALSE, protected_profile = FALSE;
    enum { SSID = 1, BSSID = 2, PORT = 4, CHANNEL = 8, AUTH = 16,
           WPA = 32, PASSPHRASE = 64, PSK = 128, EXTENT = 256,
           COOKIE = 512 };
    if (cookie) *cookie = 0;
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
        case S2INFO_WPAInfoLength: bit = EXTENT; break;
        case S2INFO_Passphrase: bit = PASSPHRASE; break;
        case S2INFO_PSK: bit = PSK; break;
        case S2INFO_AssociationCookie: bit = COOKIE; break;
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
            /* Borrow only during this call; validate after both tags arrive.
             * Never inspect the IE before its independently supplied extent. */
            wpa_ie = (const UBYTE *)data;
        } else if (bit == EXTENT) {
            if (data > 257u) goto refused;
            wpa_ie_extent = (ULONG)data;
        } else if (bit == PASSPHRASE) {
            const UBYTE *s = (const UBYTE *)data;
            if (!s || (seen & PSK)) goto refused;
            for (i = 0; i < 64u && s[i]; ++i) {
                if (s[i] < 32 || s[i] > 126) goto refused;
                target->password[i] = s[i];
            }
            if (i < 8 || i > 63) goto refused;
            target->password_length = i;
        } else if (bit == COOKIE) {
            /* The cookie is request metadata, not part of the radio ABI.
             * Validate before narrowing IPTR on a wider host. */
            if (!cookie || !data || data > (IPTR)~(ULONG)0) goto refused;
            *cookie = (ULONG)data;
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
    if (!ended || (seen & (SSID | PORT | AUTH | WPA | EXTENT)) !=
                            (SSID | PORT | AUTH | WPA | EXTENT)) goto refused;
    if (wpa_ie) {
        if (!p4wlan_rsn_psk_ccmp(wpa_ie, wpa_ie_extent)) goto refused;
        protected_profile = TRUE;
    } else if (wpa_ie_extent) goto refused;
    if (protected_profile != (target->password_length != 0)) goto refused;
    target->authmode = protected_profile ? 3u : 0u;
    return TRUE;
refused:
    if (cookie) *cookie = 0;
    p4wlan_options_wipe(target);
    return FALSE;
}

/* Legacy consumers have not negotiated failure publication. Do not silently
 * discard a supplied correlation cookie on those paths. */
static BOOL p4wlan_copy_association_options(
    struct p4_wifi_association_target *target, const struct TagItem *tags)
{
    return p4wlan_copy_association_options_cookie(target, NULL, tags);
}

/* Separate negotiated entry: no legacy credential/IE/auth tags can enter it.
 * Both tag objects and declared credential extents must remain readable and
 * unchanged until Reply. Copy all accepted state into owned storage; on every
 * refusal volatile-wipe the complete target and retire the output cookie.
 * This helper does not advertise a profile or qualify firmware semantics. */
static __inline__ BOOL p4wlan_copy_fullmac_association_options(
    struct p4_wifi_association_target *target, ULONG *cookie,
    const struct TagItem *tags, const struct Sana2FullMACCapabilities *caps)
{
    const struct Sana2FullMACAssociation *selection = NULL;
    const struct Sana2WirelessCredential *credential = NULL;
    ULONG seen = 0, visited = 0, i;
    BOOL ended = FALSE;
    enum { SSID = 1, BSSID = 2, PORT = 4, CHANNEL = 8,
           PROFILE = 16, CREDENTIAL = 32, COOKIE = 64 };
    if (cookie) *cookie = 0;
    if (!target) return FALSE;
    p4wlan_options_wipe(target);
    if (!cookie || !tags || !Sana2FullMACCapabilitiesValid(caps)) return FALSE;
    while (tags && visited < 128u) {
        ULONG tag = tags->ti_Tag, bit;
        IPTR data = tags->ti_Data;
        ++tags;
        ++visited;
        if (tag == TAG_DONE) { ended = TRUE; break; }
        if (tag == TAG_IGNORE) continue;
        if (tag == TAG_MORE) {
            tags = (const struct TagItem *)data;
            if (!tags) ended = TRUE;
            continue;
        }
        if (tag == TAG_SKIP) {
            IPTR address = (IPTR)tags;
            if (data > 128u - visited ||
                data > (~(IPTR)0 - address) / sizeof(*tags)) goto refused;
            tags = (const struct TagItem *)(address + data * sizeof(*tags));
            visited += (ULONG)data;
            continue;
        }
        switch (tag) {
        case S2INFO_SSID: bit = SSID; break;
        case S2INFO_BSSID: bit = BSSID; break;
        case S2INFO_PortType: bit = PORT; break;
        case S2INFO_Channel: bit = CHANNEL; break;
        case S2INFO_FullMACAssociation: bit = PROFILE; break;
        case S2INFO_WirelessCredential: bit = CREDENTIAL; break;
        case S2INFO_AssociationCookie: bit = COOKIE; break;
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
        } else if (bit == PROFILE) {
            selection = (const struct Sana2FullMACAssociation *)data;
        } else if (bit == CREDENTIAL) {
            credential = (const struct Sana2WirelessCredential *)data;
            if (!credential) goto refused;
        } else {
            if (!data || data > (IPTR)~(ULONG)0) goto refused;
            *cookie = (ULONG)data;
        }
    }
    if (!ended || (seen & (SSID | PORT | PROFILE | COOKIE)) !=
                             (SSID | PORT | PROFILE | COOKIE) ||
        !Sana2FullMACAssociationValid(caps, selection, credential)) goto refused;
    target->fullmac_profile = selection->Profile;
    target->diagnostic_risk_accepted =
        !!(selection->Flags & S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK);
    target->pmf_required = selection->PMFPolicy == S2FULLMAC_PMF_REQUIRED;
    if (selection->Profile == S2FULLMAC_PROFILE_OPEN) return TRUE;
    if (selection->Profile == S2FULLMAC_PROFILE_SAE_CCMP_PMF) {
        target->authmode = 6u;
        if (selection->SAEPweMethod == S2FULLMAC_SAE_PWE_HUNT_AND_PECK)
            target->sae_pwe_h2e = 1u;
        else if (selection->SAEPweMethod == S2FULLMAC_SAE_PWE_H2E)
            target->sae_pwe_h2e = 2u;
        else target->sae_pwe_h2e = 3u;
    } else target->authmode = 3u;
    if (credential->Kind == S2WIRELESS_CRED_WPA2_PSK) {
        static const UBYTE hex[] = "0123456789abcdef";
        for (i = 0; i < S2WIRELESS_PSK_SIZE; ++i) {
            target->password[i * 2] = hex[credential->Data[i] >> 4];
            target->password[i * 2 + 1] = hex[credential->Data[i] & 15];
        }
        target->password_length = 64u;
    } else {
        for (i = 0; i < credential->Length; ++i)
            target->password[i] = credential->Data[i];
        target->password_length = credential->Length;
    }
    return TRUE;
refused:
    *cookie = 0;
    p4wlan_options_wipe(target);
    return FALSE;
}

#endif
