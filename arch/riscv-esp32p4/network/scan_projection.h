#ifndef P4WLAN_SCAN_PROJECTION_H
#define P4WLAN_SCAN_PROJECTION_H

/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include "hosted_scan.h"
#include <devices/sana2wireless.h>

/*
 * Security enum values follow the pinned ESP-Hosted IDF 5.4.3 source:
 * idf-5.4.3/components/esp_wifi/include/esp_wifi_types_generic.h:84-104
 * (WIFI_AUTH_*) and :261-275 (WIFI_CIPHER_TYPE_*). The slave copies these
 * wifi_ap_record_t fields without translation to WifiApRecord in
 * esp_hosted/examples/slave/main/slave_control.c:1156-1163.
 *
 * This projects summary metadata only. It does not construct Beacon/Probe
 * Response IEs. Unsupported, mixed, or incomplete profiles remain unknown
 * and private; only exact singleton suite tuples are marked known. Pure
 * WPA3-Personal/CCMP reports SAE metadata, not PMF/H2E capability, negotiated
 * authentication, firmware offload support or permission to associate.
 */
static void p4wlan_scan_security(
    const struct p4_hosted_scan_record *record,
    struct Sana2WirelessSecuritySummary *summary)
{
    int32_t authmode;
    int32_t pairwise_cipher;
    int32_t group_cipher;
    ULONG protocol;
    ULONG pairwise;
    ULONG group;

    if (summary == NULL)
        return;

    /* Snapshot before writing output so overlapping arguments remain safe. */
    if (record != NULL) {
        authmode = record->authmode;
        pairwise_cipher = record->pairwise_cipher;
        group_cipher = record->group_cipher;
    }

    summary->Size = (ULONG)sizeof(*summary);
    summary->Version = S2WIRELESS_SUMMARY_VERSION;
    summary->Flags = S2WIRELESS_SUMF_PRIVACY;
    summary->Protocols = 0;
    summary->KeyManagement = 0;
    summary->PairwiseCiphers = 0;
    summary->GroupCiphers = 0;

    if (record == NULL)
        return;

    /* WIFI_AUTH_OPEN=0, WIFI_CIPHER_TYPE_NONE=0. */
    if (authmode == 0 && pairwise_cipher == 0 && group_cipher == 0) {
        summary->Flags = S2WIRELESS_SUMF_KNOWN;
        return;
    }

    if (authmode == 2) {
        protocol = S2WIRELESS_PROTO_WPA;
    } else if (authmode == 3) {
        protocol = S2WIRELESS_PROTO_RSN;
    } else if (authmode == 6 && pairwise_cipher == 4 && group_cipher == 4) {
        /* WIFI_AUTH_WPA3_PSK: pure SAE, not WPA2/WPA3 transition mode7.
         * SAE never inherits the WPA1/WPA2 TKIP allowance below. */
        protocol = S2WIRELESS_PROTO_RSN_SAE;
    } else {
        return;
    }

    /* Accept only singleton data ciphers: TKIP=3 or CCMP=4. In particular,
     * TKIP_CCMP=5 denotes alternatives and must not be expanded into a union. */
    if (pairwise_cipher == 3)
        pairwise = S2WIRELESS_CIPHER_TKIP;
    else if (pairwise_cipher == 4)
        pairwise = S2WIRELESS_CIPHER_CCMP;
    else
        return;

    if (group_cipher == 3)
        group = S2WIRELESS_CIPHER_TKIP;
    else if (group_cipher == 4)
        group = S2WIRELESS_CIPHER_CCMP;
    else
        return;

    summary->Flags = S2WIRELESS_SUMF_KNOWN | S2WIRELESS_SUMF_PRIVACY;
    summary->Protocols = protocol;
    summary->KeyManagement = protocol == S2WIRELESS_PROTO_RSN_SAE ?
        S2WIRELESS_KEY_SAE : S2WIRELESS_KEY_PSK;
    summary->PairwiseCiphers = pairwise;
    summary->GroupCiphers = group;
}

#endif /* P4WLAN_SCAN_PROJECTION_H */
