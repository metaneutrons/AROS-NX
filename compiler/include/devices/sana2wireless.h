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
#define S2INFO_WPAInfoLength   (TAG_USER + 19) /* readable WPAInfo extent */
#define S2INFO_AssociationCookie (TAG_USER + 20) /* caller-owned attempt ID */
#define S2INFO_FullMACAssociation (TAG_USER + 21) /* borrowed typed selection */
#define S2INFO_WirelessCredential (TAG_USER + 22) /* borrowed typed credential */

/* Send WPAInfoLength only when the actual device advertises IE_EXTENT with
 * FOURWAY. It is the caller's validated readable byte count, not a length
 * inferred by the device from WPAInfo[1]. Zero accompanies NULL WPAInfo (open);
 * otherwise one complete IE spans 2..257 bytes including its two-byte header.
 * The embedded length must agree. Both tags are borrowed through Reply; tag
 * order is irrelevant and devices must copy before acknowledging async work.
 * An extent does not attest arbitrary pointer validity: the caller guarantees
 * readable memory for the full count. No credential lifetime is extended. */

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
#define S2_GETFULLMACCAPABILITIES  0xc01a

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
/* IE_EXTENT requires FOURWAY and means WPAInfoLength is required/understood by
 * this device's S2_SETOPTIONS path, including zero for an open association.
 * Keep old/legacy devices' tag lists unchanged unless this flag is negotiated.
 * Older strict callers reject this new flag rather than supplying an inferred
 * span to an offload-only device. Capability structure size/version is unchanged. */
#define S2WIRELESS_CAPF_IE_EXTENT       (1UL << 3)
/* CONNECT_FAILED reports that a still-current acknowledged association
 * attempt ended unsuccessfully, without conveying its cause. It may follow
 * a local deadline or RPC error, but does not attest AP rejection, a proved
 * timeout or loss of a previously confirmed link. Independent of FOURWAY.
 * A caller negotiates this flag before arming S2_ONEVENT and supplies a nonzero
 * 32-bit AssociationCookie on each association SETOPTIONS. Cookies must never
 * be reused during that OpenDevice lifetime; refuse exhaustion rather than wrap.
 * The device copies the cookie before Reply and scopes it to that opener.
 * For failure of its still-current acknowledged attempt, S2_ONEVENT returns
 * io_Error=0, WireError=S2EVENT_CONNECT_FAILED, DataLength=the copied cookie.
 * Do not combine this edge with CONNECT/DISCONNECT or infer a remote cause.
 * The device retains an unconsumed failure for its originating opener only,
 * discards it on supersession/cancellation/leave/offline/close, and must not
 * report canceled or superseded work as a new attempt's failure. The caller
 * sends standalone S2INFO_Disassociate=1 for local cancellation/leave and
 * retires its active cookie even if that request fails. Advertising this
 * flag requires support for that leave tag; acknowledgment accepts the
 * cancellation but does not prove radio teardown or grant a DISCONNECT edge.
 * Neither a local reason code nor an AP rejection cause is conveyed by it.
 * The caller ignores cookies that do not match its active acknowledged attempt.
 * Legacy callers/devices receive neither a new tag nor an unsupported mask;
 * no capability grants new credential handling. Version/size remain v1/32B. */
#define S2WIRELESS_CAPF_CONNECT_FAILED  (1UL << 4)

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

/* Separate optional FullMAC contract, not a reinterpretation of v1 FOURWAY.
 * S2_GETFULLMACCAPABILITIES uses caller-owned ios2_StatData. Initialize
 * SizeAvailable/Version; require exact SizeSupplied/Version, known bits and
 * zero reserved fields. A failed/unsupported response grants no permission
 * to send the new tags. Keep legacy devices' requests unchanged.
 * Profiles name whole co-occurring suites, not Cartesian mask combinations.
 * Implementations must refuse unsupported profiles rather than falling back.
 * DIAGNOSTIC_RISK reports unqualified firmware/privacy/recovery: callers must
 * explicitly opt in per association. It is never a production verification
 * assertion. Absence of this flag requires the provider's production gates.
 * No credential/peer firmware identity is returned by this query.
 */
#define S2FULLMAC_VERSION 1
#define S2FULLMAC_CAPF_DIAGNOSTIC_RISK (1UL << 0)
#define S2FULLMAC_PROFILE_OPEN         (1UL << 0)
#define S2FULLMAC_PROFILE_WPA2_CCMP    (1UL << 1)
#define S2FULLMAC_PROFILE_SAE_CCMP_PMF (1UL << 2)
#define S2FULLMAC_SAE_PWE_HUNT_AND_PECK (1UL << 0)
#define S2FULLMAC_SAE_PWE_H2E           (1UL << 1)
#define S2FULLMAC_SAE_PWE_BOTH          (1UL << 2)

/* SAEPweMethods advertises selectable modes, not a set of AP observations.
 * BOTH is one selectable mode enabling HNP and H2E negotiation, not the
 * bitwise union of the two concrete mode values. */

struct Sana2FullMACCapabilities
{
    ULONG SizeAvailable;
    ULONG SizeSupplied;
    ULONG Version;
    ULONG Flags;
    ULONG Profiles;
    ULONG SAEPweMethods;
    ULONG Reserved[2];
};

/* Typed selection supplied only after successful per-device negotiation.
 * S2_SETOPTIONS receives exactly one FullMACAssociation and, for protected
 * profiles, exactly one WirelessCredential. Do not combine them with WPAInfo,
 * WPAInfoLength, Passphrase, PSK or an alternative authentication tag. SSID,
 * optional BSSID/channel and existing opener-scoped AssociationCookie retain
 * their meaning; PortType must be managed. OPEN has no credential, disabled
 * PMF and no SAE PWE selector. WPA2_CCMP is specifically PSK authentication:
 * it accepts WPA2 passphrase or binary PSK, disabled PMF, no SAE PWE.
 * SAE_CCMP_PMF uses a distinct SAE password, required PMF and one advertised
 * PWE method. No fallback to WPA2/open, no raw PSK/PBKDF2 and no synthetic IE.
 * This initial SAE profile does not support a password identifier or FT/EAP.
 * Required PMF is client intent, never a claim of observed AP PMF metadata.
 * Callers must not infer observed AP PMF/PWE from the SAE label or this
 * selection; policies requiring those facts must refuse missing evidence.
 * A transition BSS needs separate unambiguous suite/negotiation evidence;
 * an AP-info authmode is not proof of the negotiated AKM.
 * Flags permits explicit diagnostic consent, not policy inferred from a
 * credential or from a generic FullMAC classification. Reserved must be zero.
 */
#define S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK (1UL << 0)
#define S2FULLMAC_PMF_DISABLED 0
#define S2FULLMAC_PMF_REQUIRED 1

struct Sana2FullMACAssociation
{
    ULONG Size;
    ULONG Version;
    ULONG Profile;
    ULONG Flags;
    ULONG PMFPolicy;
    ULONG SAEPweMethod;
    ULONG Reserved;
};

#define S2WIRELESS_CREDENTIAL_VERSION 1
#define S2WIRELESS_CRED_WPA2_PASSPHRASE 1
#define S2WIRELESS_CRED_WPA2_PSK        2
#define S2WIRELESS_CRED_SAE_PASSWORD    3

/* Explicit bytes, not NUL-terminated strings: WPA2 printable passphrase8..63,
 * WPA2 binary PSK exactly32, SAE password1..63 non-NUL bytes. These are the
 * bounded initial contract's limits, not all possible SAE passwords.
 * Caller guarantees readable objects/Data for Length bytes until Reply.
 * Provider copies to owned state before acknowledging asynchronous work;
 * no borrowed pointer may survive Reply. Wipe owned credentials on failure,
 * cancellation, supersession, offline/close and after the bounded operation.
 * Never log, publish in scan/capabilities or persist credentials implicitly.
 * Association acknowledgment is not link-up. CONNECT requires current-target
 * authentication/keying/port authorization and the live attempt/epoch fence;
 * matching AP-info or successful SetConfig alone cannot establish success.
 * For a negotiated typed operation, CONNECT is opener-scoped and carries its
 * acknowledged AssociationCookie in ios2_DataLength. A subsequent DISCONNECT
 * carries that same committed-link cookie, never an older attempt's cookie.
 * Deliver each as an exact terminal event, not a combined terminal mask. This
 * additional identity rule applies only to FullMAC operations; legacy v1
 * CONNECT/DISCONNECT behavior and failure-event layouts are unchanged.
 * The v1 S2WIRELESS_CAPF_CONNECT_FAILED is the sole failure-event authority:
 * query v1 capabilities too and arm its existing opener/cookie ONEVENT before
 * the attempt. FullMAC does not imply that flag or alter Disassociate support.
 * A caller requiring correlated failure/cancellation must refuse its absence.
 */
struct Sana2WirelessCredential
{
    ULONG Size;
    ULONG Version;
    ULONG Kind;
    ULONG Length;
    const UBYTE *Data;
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
