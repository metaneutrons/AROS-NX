#ifndef DEVICES_SANA2WIRELESS_H
#define DEVICES_SANA2WIRELESS_H

/*
    Copyright (C) 2011-2013 Neil Cafferkey
    $Id$

    Desc: Definitions for SANA-II wireless devices
    Lang: english
*/

#include <exec/types.h>
#include <utility/tagitem.h>


/* Constants */
/* ========= */

/* Tags to get and set information */

#define S2INFO_SSID           (TAG_USER + 0)
#define S2INFO_BSSID          (TAG_USER + 1)
#define S2INFO_AuthTypes      (TAG_USER + 2)
#define S2INFO_AssocID        (TAG_USER + 3)
#define S2INFO_Encryption     (TAG_USER + 4)
#define S2INFO_PortType       (TAG_USER + 5)
#define S2INFO_BeaconInterval (TAG_USER + 6)
#define S2INFO_Channel        (TAG_USER + 7)
#define S2INFO_Signal         (TAG_USER + 8)
#define S2INFO_Noise          (TAG_USER + 9)
#define S2INFO_Capabilities   (TAG_USER + 10)
#define S2INFO_InfoElements   (TAG_USER + 11)
#define S2INFO_WPAInfo        (TAG_USER + 12)
#define S2INFO_Band           (TAG_USER + 13)
#define S2INFO_DefaultKeyNo   (TAG_USER + 14)
#define S2INFO_Passphrase     (TAG_USER + 15)  /* PSK passphrase (driver-side 4-way) */
#define S2INFO_Disassociate   (TAG_USER + 16)  /* S2_SETOPTIONS: leave the network */
#define S2INFO_SecuritySummary (TAG_USER + 17) /* owned Sana2WirelessSecuritySummary */
#define S2INFO_PSK             (TAG_USER + 18) /* borrowed binary WPA PSK */

/* S2INFO_PSK is exactly 32 binary bytes, never a string or passphrase.
 * Send it only with FOURWAY and RAW_PSK capabilities on the actual device.
 * S2_SETOPTIONS borrows the bytes until Reply; the device must copy them
 * before replying and protect/wipe any retained credential state. A caller
 * sends either Passphrase or PSK, not both. Neither credential belongs in
 * scan results, logs or capability responses. */
#define S2WIRELESS_PSK_SIZE 32

/* Wireless Commands */

#define S2_GETSIGNALQUALITY 0xc010
#define S2_GETNETWORKS      0xc011
#define S2_SETOPTIONS       0xc012
/* S2_SETKEY: ios2_WireError = key index, ios2_PacketType = S2ENC_*,
 * ios2_Data/ios2_DataLength = the key, ios2_StatData = RX sequence counter,
 * ios2_DstAddr = the peer this key is for (all-zero or multicast = group key) */
#define S2_SETKEY           0xc013
#define S2_GETNETWORKINFO   0xc014
#define S2_READMGMT         0xc015
#define S2_WRITEMGMT        0xc016
#define S2_GETRADIOBANDS    0xc017
#define S2_GETCRYPTTYPES    0xc018
#define S2_GETWIRELESSCAPABILITIES 0xc019

/* Optional, versioned per-device contract. S2_GETWIRELESSCAPABILITIES uses
 * ios2_StatData to point to a caller-owned structure. The caller initializes
 * SizeAvailable and Version; the device returns SizeSupplied and the same
 * supported Version, without writing beyond SizeAvailable. Unsupported
 * devices return S2ERR_NOT_SUPPORTED and retain legacy handshake behavior.
 * For v1 the caller requires SizeSupplied == sizeof this structure,
 * Version == 1, known Flags and known mask bits only. A failed, short,
 * oversized or unknown-version response grants no new capability.
 * Flags apply to this device instance, never to all FullMAC devices.
 * FOURWAY means that the device accepts credentials through S2_SETOPTIONS
 * and completes the handshake in firmware; it must not be advertised until
 * credential handling and firmware identity/privacy are safe for that device.
 * The masks restrict supported offload suites; they do not claim EAP support.
 */
#define S2WIRELESS_CAPABILITIES_VERSION 1
#define S2WIRELESS_CAPF_FOURWAY         (1UL << 0)
#define S2WIRELESS_CAPF_SECURITY_SUMMARY (1UL << 1)
/* RAW_PSK is valid only together with FOURWAY; FOURWAY alone promises
 * passphrase handling, not support for S2INFO_PSK. Older strict v1 callers
 * reject this new flag and retain their legacy host-handshake path. This
 * does not guarantee connectivity to devices without legacy key support;
 * an offload-only device and caller must use matching contract support. */
#define S2WIRELESS_CAPF_RAW_PSK         (1UL << 2)

struct Sana2WirelessCapabilities
{
    ULONG SizeAvailable;
    ULONG SizeSupplied;
    ULONG Version;
    ULONG Flags;
    ULONG Protocols;
    ULONG KeyManagement;
    ULONG PairwiseCiphers;
    ULONG GroupCiphers;
};

/* Explicit driver-reported scan summaries, not reconstructed Beacon/Probe
 * Response IEs. S2INFO_SecuritySummary points to this pool-owned structure;
 * it has the same lifetime as the scan result's other pool-owned data.
 * For v1 Size must equal sizeof this structure and Version must equal 1;
 * unknown flag/mask bits are refused. Unknown summaries have KNOWN clear
 * and PRIVACY set; they must never match an open profile. A known open BSS
 * has KNOWN set, PRIVACY clear and all suite masks zero. A known protected BSS
 * has both flags set and one bit in each suite mask. A tuple of singleton
 * protocol, key-management, pairwise and group values describes one known
 * co-occurring suite; independent union masks would invent combinations.
 * Multi-suite/ambiguous APs must be reported as unknown until a future version
 * can preserve per-suite co-occurrence. Missing or malformed summary
 * data must not downgrade a summary-capable device's BSS to an open network.
 * Authentic IEs retain their existing meaning and are not manufactured from
 * these masks. Summary selection requires explicit per-device capability.
 */
#define S2WIRELESS_SUMMARY_VERSION 1
#define S2WIRELESS_SUMF_KNOWN       (1UL << 0)
#define S2WIRELESS_SUMF_PRIVACY     (1UL << 1)
#define S2WIRELESS_PROTO_WPA       (1UL << 0)
#define S2WIRELESS_PROTO_RSN       (1UL << 1)
#define S2WIRELESS_PROTO_RSN_SAE   (1UL << 2)
#define S2WIRELESS_KEY_PSK         (1UL << 0)
#define S2WIRELESS_KEY_EAP         (1UL << 1)
#define S2WIRELESS_KEY_SAE         (1UL << 2)
#define S2WIRELESS_CIPHER_TKIP     (1UL << 0)
#define S2WIRELESS_CIPHER_CCMP     (1UL << 1)
#define S2WIRELESS_CIPHER_GCMP     (1UL << 2)

struct Sana2WirelessSecuritySummary
{
    ULONG Size;
    ULONG Version;
    ULONG Flags;
    ULONG Protocols;
    ULONG KeyManagement;
    ULONG PairwiseCiphers;
    ULONG GroupCiphers;
};

/* Encryption types */

#define S2ENC_NONE 0
#define S2ENC_WEP  1
#define S2ENC_TKIP 2
#define S2ENC_CCMP 3

/* Radio modes */

#define S2BAND_A 0
#define S2BAND_B 1
#define S2BAND_G 2
#define S2BAND_N 3

/* Network topologies */

#define S2PORT_MANAGED 7
#define S2PORT_ADHOC   8


/* Structures */
/* ========== */

/* Structure for returning signal quality */

struct Sana2SignalQuality
{
   LONG SignalLevel;   /* signal level in dBm */
   LONG NoiseLevel;   /* noise level in dBm */
};

#endif
