/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 *
 * Desc: Strict display-only projection of SANA-II security metadata.
 */

#ifndef WIFI_SECURITY_DISPLAY_H
#define WIFI_SECURITY_DISPLAY_H

#include <devices/sana2wireless.h>

/* This validates the whole per-device contract before allowing the caller to
 * use summaries. It never grants association, credential or cipher-offload
 * policy; those decisions remain in WirelessManager's driver contract. */
static int WifiSummaryCapability(
	const struct Sana2WirelessCapabilities *caps)
{
	const ULONG known_flags = S2WIRELESS_CAPF_FOURWAY |
		S2WIRELESS_CAPF_SECURITY_SUMMARY | S2WIRELESS_CAPF_RAW_PSK |
		S2WIRELESS_CAPF_IE_EXTENT | S2WIRELESS_CAPF_CONNECT_FAILED;
	const ULONG known_protocols = S2WIRELESS_PROTO_WPA |
		S2WIRELESS_PROTO_RSN | S2WIRELESS_PROTO_RSN_SAE;
	const ULONG known_key_mgmt = S2WIRELESS_KEY_PSK |
		S2WIRELESS_KEY_EAP | S2WIRELESS_KEY_SAE;
	const ULONG known_ciphers = S2WIRELESS_CIPHER_TKIP |
		S2WIRELESS_CIPHER_CCMP | S2WIRELESS_CIPHER_GCMP;
	const ULONG fourway_protocols = S2WIRELESS_PROTO_WPA |
		S2WIRELESS_PROTO_RSN;
	const ULONG fourway_ciphers = S2WIRELESS_CIPHER_TKIP |
		S2WIRELESS_CIPHER_CCMP;

	if (caps == NULL ||
	    caps->SizeAvailable != (ULONG)sizeof(*caps) ||
	    caps->SizeSupplied != (ULONG)sizeof(*caps) ||
	    caps->Version != S2WIRELESS_CAPABILITIES_VERSION ||
	    (caps->Flags & ~known_flags) != 0 ||
	    (caps->Protocols & ~known_protocols) != 0 ||
	    (caps->KeyManagement & ~known_key_mgmt) != 0 ||
	    (caps->PairwiseCiphers & ~known_ciphers) != 0 ||
	    (caps->GroupCiphers & ~known_ciphers) != 0)
		return 0;

	/* Mirror WirelessManager's strict v1 validation. Offload declarations must
	 * be complete and representable; without FOURWAY every suite mask is zero. */
	if (caps->Flags & S2WIRELESS_CAPF_FOURWAY) {
		if (caps->Protocols == 0 ||
		    (caps->Protocols & ~fourway_protocols) != 0 ||
		    caps->KeyManagement != S2WIRELESS_KEY_PSK ||
		    caps->PairwiseCiphers == 0 ||
		    (caps->PairwiseCiphers & ~fourway_ciphers) != 0 ||
		    caps->GroupCiphers == 0 ||
		    (caps->GroupCiphers & ~fourway_ciphers) != 0)
			return 0;
	} else if ((caps->Flags & (S2WIRELESS_CAPF_RAW_PSK |
				  S2WIRELESS_CAPF_IE_EXTENT)) != 0 ||
		   caps->Protocols != 0 || caps->KeyManagement != 0 ||
		   caps->PairwiseCiphers != 0 || caps->GroupCiphers != 0) {
		return 0;
	}

	return (caps->Flags & S2WIRELESS_CAPF_SECURITY_SUMMARY) != 0;
}

/* Summaries are independent from authentic WPA/RSN information elements.
 * Only exact, known singleton tuples have a human-readable projection. */
static const char *WifiSummaryProtection(
	const struct Sana2WirelessSecuritySummary *summary)
{
	const ULONG known_flags = S2WIRELESS_SUMF_KNOWN |
		S2WIRELESS_SUMF_PRIVACY;
	const ULONG ciphers = S2WIRELESS_CIPHER_TKIP |
		S2WIRELESS_CIPHER_CCMP;

	if (summary == NULL ||
	    summary->Size != (ULONG)sizeof(*summary) ||
	    summary->Version != S2WIRELESS_SUMMARY_VERSION ||
	    (summary->Flags & ~known_flags) != 0 ||
	    (summary->Flags & S2WIRELESS_SUMF_KNOWN) == 0)
		return "?";

	if (summary->Flags == S2WIRELESS_SUMF_KNOWN) {
		if (summary->Protocols == 0 && summary->KeyManagement == 0 &&
		    summary->PairwiseCiphers == 0 && summary->GroupCiphers == 0)
			return "open";
		return "?";
	}

	if (summary->Flags != known_flags ||
	    summary->KeyManagement != S2WIRELESS_KEY_PSK ||
	    (summary->Protocols != S2WIRELESS_PROTO_WPA &&
	     summary->Protocols != S2WIRELESS_PROTO_RSN) ||
	    (summary->PairwiseCiphers != S2WIRELESS_CIPHER_TKIP &&
	     summary->PairwiseCiphers != S2WIRELESS_CIPHER_CCMP) ||
	    (summary->GroupCiphers != S2WIRELESS_CIPHER_TKIP &&
	     summary->GroupCiphers != S2WIRELESS_CIPHER_CCMP) ||
	    (summary->PairwiseCiphers & ~ciphers) != 0 ||
	    (summary->GroupCiphers & ~ciphers) != 0)
		return "?";

	return summary->Protocols == S2WIRELESS_PROTO_WPA ? "WPA" : "WPA2";
}

#endif /* WIFI_SECURITY_DISPLAY_H */
