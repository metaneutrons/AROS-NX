/* Explicit driver-reported scan security, never reconstructed AP IEs.
 * Copyright (c) 2026, The AROS Development Team.
 * Distributed under the same GPLv2/BSD terms as WirelessManager.
 */
#ifndef WPA_SECURITY_SUMMARY_H
#define WPA_SECURITY_SUMMARY_H

#include "defs.h"

#define WPA_SECURITY_SUMMARY_PRESENT 0x01u
#define WPA_SECURITY_SUMMARY_KNOWN   0x02u
#define WPA_SECURITY_SUMMARY_PRIVACY 0x04u

struct wpa_security_summary {
    unsigned int flags;
    int proto;
    int key_mgmt;
    int pairwise_cipher;
    int group_cipher;
};

/* A deliberately bounded initial offload profile. Other suites remain
 * distinguishable as unsupported, never downgraded to an open network.
 * This does not claim support for SAE, EAP, WEP, WPS or management frame
 * protection: the peer must supply additional truthful metadata first.
 */
static inline int wpa_security_summary_protected(
    const struct wpa_security_summary *summary)
{
    const int protocols = WPA_PROTO_WPA | WPA_PROTO_RSN;
    const int ciphers = WPA_CIPHER_TKIP | WPA_CIPHER_CCMP;
    return summary->flags == (WPA_SECURITY_SUMMARY_PRESENT |
        WPA_SECURITY_SUMMARY_KNOWN | WPA_SECURITY_SUMMARY_PRIVACY) &&
        summary->proto != 0 && !(summary->proto & ~protocols) &&
        !(summary->proto & (summary->proto - 1)) &&
        summary->key_mgmt == WPA_KEY_MGMT_PSK &&
        summary->pairwise_cipher != 0 &&
        !(summary->pairwise_cipher & ~ciphers) &&
        !(summary->pairwise_cipher & (summary->pairwise_cipher - 1)) &&
        summary->group_cipher != 0 && !(summary->group_cipher & ~ciphers) &&
        !(summary->group_cipher & (summary->group_cipher - 1));
}

static inline int wpa_security_summary_open(
    const struct wpa_security_summary *summary)
{
    return summary->flags == (WPA_SECURITY_SUMMARY_PRESENT |
        WPA_SECURITY_SUMMARY_KNOWN) && !summary->proto &&
        !summary->key_mgmt && !summary->pairwise_cipher &&
        !summary->group_cipher;
}

static inline int wpa_security_summary_match(
    const struct wpa_security_summary *summary, int offload,
    int proto, int key_mgmt, int pairwise, int group)
{
    return offload && wpa_security_summary_protected(summary) &&
        (summary->proto & proto) && (summary->key_mgmt & key_mgmt) &&
        (summary->pairwise_cipher & pairwise) &&
        (summary->group_cipher & group);
}

#endif
