#define CONFIG_WPS 1
#define CONFIG_IEEE80211W 1

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/common.h"
#include "common/security_summary.h"

#include "security-summary-source-constants.inc"

/* The production helpers are extracted verbatim by the runner. These small
 * host-side shapes supply only the fields those helpers consume. Constants
 * mirror driver.h and devices/sana2wireless.h; the public ABI is checked by
 * test-security-summary.c's separate RV32 fixture. */
typedef uint32_t ULONG;
typedef uintptr_t UPINT;
typedef void *APTR;

struct Sana2WirelessCapabilities {
	ULONG SizeAvailable;
	ULONG SizeSupplied;
	ULONG Version;
	ULONG Flags;
	ULONG Protocols;
	ULONG KeyManagement;
	ULONG PairwiseCiphers;
	ULONG GroupCiphers;
};

struct Sana2WirelessSecuritySummary {
	ULONG Size;
	ULONG Version;
	ULONG Flags;
	ULONG Protocols;
	ULONG KeyManagement;
	ULONG PairwiseCiphers;
	ULONG GroupCiphers;
};

struct TagItem {
	ULONG ti_Tag;
	UPINT ti_Data;
};

struct wpa_scan_res {
	unsigned int caps;
	struct wpa_security_summary security_summary;
};

struct IOSana2Req {
	struct {
		int io_Command;
		int io_Error;
	} ios2_Req;
	APTR ios2_Data;
	APTR ios2_StatData;
	ULONG ios2_DataLength;
};

struct wpa_driver_sana2_data {
	struct IOSana2Req *request;
	int hard_mac;
	int wireless_caps_queried;
	int wireless_caps_valid;
	struct Sana2WirelessCapabilities wireless_caps;
};

struct wpa_driver_capa {
	unsigned int key_mgmt;
	unsigned int enc;
	unsigned int flags;
	unsigned int summary_pairwise_enc;
	unsigned int summary_group_enc;
};

struct wpa_supplicant {
	unsigned int drv_flags;
};

struct wpa_ssid {
	int proto;
	int key_mgmt;
	int pairwise_cipher;
	int group_cipher;
	int mode;
	int mixed_cell;
	int auth_alg;
	int disabled;
	int ieee80211w;
	size_t wep_key_len[NUM_WEP_KEYS];
};

static unsigned int scenario_count;
static unsigned int check_count;
static unsigned int failure_count;

static void check(const char *scenario, const char *predicate,
		  int actual, int expected)
{
	++check_count;
	if (!!actual != !!expected) {
		fprintf(stderr, "FAIL: %s: %s got %d expected %d\n",
			scenario, predicate, actual, expected);
		++failure_count;
	}
}

#define CHECK(scenario, predicate, actual, expected) \
	check((scenario), (predicate), (actual), (expected))

static struct TagItem *FindTagItem(ULONG tag, struct TagItem *items)
{
	size_t i;

	if (items == NULL)
		return NULL;
	for (i = 0; i < 16; ++i) {
		if (items[i].ti_Tag == TAG_END)
			return NULL;
		if (items[i].ti_Tag == tag)
			return &items[i];
	}
	return NULL;
}

static unsigned char fake_crypt_types[] = { S2ENC_TKIP, S2ENC_CCMP };

static APTR CreatePool(ULONG flags, ULONG puddle, ULONG threshold)
{
	static int pool_token;
	(void) flags;
	(void) puddle;
	(void) threshold;
	return &pool_token;
}

static void DeletePool(APTR pool)
{
	(void) pool;
}

static int DoIO(APTR request_arg)
{
	struct IOSana2Req *request = request_arg;

	if (request->ios2_Req.io_Command != S2_GETCRYPTTYPES)
		return -1;
	request->ios2_StatData = fake_crypt_types;
	request->ios2_DataLength = sizeof(fake_crypt_types);
	request->ios2_Req.io_Error = 0;
	return 0;
}

static int wpa_driver_sana2_query_wireless_caps(
	struct wpa_driver_sana2_data *drv)
{
	(void) drv;
	return 0;
}

#include "driver_caps_valid.inc"
#include "driver_set_security_summary.inc"
#include "driver_get_capa.inc"
#include "supplicant_summary_profile_common.inc"
#include "supplicant_summary_open_profile.inc"
#include "supplicant_summary_capa_match.inc"
#include "supplicant_summary_config_match.inc"
#include "events_summary_offload_enabled.inc"
#include "events_summary_matches_ssid.inc"
#include "events_summary_open_profile.inc"

static struct Sana2WirelessCapabilities valid_caps(void)
{
	struct Sana2WirelessCapabilities caps;

	memset(&caps, 0, sizeof(caps));
	caps.SizeAvailable = sizeof(caps);
	caps.SizeSupplied = sizeof(caps);
	caps.Version = S2WIRELESS_CAPABILITIES_VERSION;
	caps.Flags = S2WIRELESS_CAPF_SECURITY_SUMMARY |
		S2WIRELESS_CAPF_FOURWAY;
	caps.Protocols = S2WIRELESS_PROTO_RSN;
	caps.KeyManagement = S2WIRELESS_KEY_PSK;
	caps.PairwiseCiphers = S2WIRELESS_CIPHER_CCMP;
	caps.GroupCiphers = S2WIRELESS_CIPHER_TKIP;
	return caps;
}

static struct Sana2WirelessSecuritySummary valid_wire_summary(void)
{
	struct Sana2WirelessSecuritySummary summary;

	memset(&summary, 0, sizeof(summary));
	summary.Size = sizeof(summary);
	summary.Version = S2WIRELESS_SUMMARY_VERSION;
	summary.Flags = S2WIRELESS_SUMF_KNOWN | S2WIRELESS_SUMF_PRIVACY;
	summary.Protocols = S2WIRELESS_PROTO_RSN;
	summary.KeyManagement = S2WIRELESS_KEY_PSK;
	summary.PairwiseCiphers = S2WIRELESS_CIPHER_CCMP;
	summary.GroupCiphers = S2WIRELESS_CIPHER_TKIP;
	return summary;
}

static struct wpa_security_summary mapped_summary(
	struct Sana2WirelessSecuritySummary *wire_summary,
	int summary_capability, unsigned int *bss_caps, int include_tag)
{
	struct wpa_driver_sana2_data drv;
	struct wpa_scan_res res;
	struct TagItem tags[2];
	struct TagItem *tag_lists[1];

	memset(&drv, 0, sizeof(drv));
	memset(&res, 0, sizeof(res));
	memset(tags, 0, sizeof(tags));
	drv.wireless_caps_valid = 1;
	drv.wireless_caps.Flags = summary_capability ?
		S2WIRELESS_CAPF_SECURITY_SUMMARY : 0;
	res.caps = *bss_caps;
	if (include_tag) {
		tags[0].ti_Tag = S2INFO_SecuritySummary;
		tags[0].ti_Data = (UPINT) wire_summary;
		tags[1].ti_Tag = TAG_END;
	} else {
		tags[0].ti_Tag = TAG_END;
	}
	tag_lists[0] = tags;
	wpa_driver_sana2_set_security_summary(&drv, &res, tag_lists, 0);
	*bss_caps = res.caps;
	return res.security_summary;
}

static void test_caps_validator(void)
{
	struct Sana2WirelessCapabilities caps, absent;

	++scenario_count;
	caps = valid_caps();
	CHECK("valid disjoint role masks", "fourway+summary accepted",
		wpa_driver_sana2_wireless_caps_valid(&caps), 1);
	caps.PairwiseCiphers = S2WIRELESS_CIPHER_TKIP;
	caps.GroupCiphers = S2WIRELESS_CIPHER_CCMP;
	CHECK("valid swapped role masks", "fourway+summary accepted",
		wpa_driver_sana2_wireless_caps_valid(&caps), 1);

	++scenario_count;
	memset(&absent, 0, sizeof(absent));
	absent.SizeAvailable = sizeof(absent);
	absent.SizeSupplied = sizeof(absent);
	absent.Version = S2WIRELESS_CAPABILITIES_VERSION;
	CHECK("valid empty capability response", "structurally valid",
		wpa_driver_sana2_wireless_caps_valid(&absent), 1);

	++scenario_count;
	caps = valid_caps();
	caps.Flags = S2WIRELESS_CAPF_FOURWAY | S2WIRELESS_CAPF_RAW_PSK;
	CHECK("raw PSK capability contract", "fourway+raw without summary accepted",
		wpa_driver_sana2_wireless_caps_valid(&caps), 1);
	caps.Flags |= S2WIRELESS_CAPF_SECURITY_SUMMARY;
	CHECK("raw PSK capability contract", "fourway+raw with summary accepted",
		wpa_driver_sana2_wireless_caps_valid(&caps), 1);
	absent.Flags = S2WIRELESS_CAPF_RAW_PSK;
	CHECK("raw PSK capability contract", "raw-only rejected with zero masks",
		wpa_driver_sana2_wireless_caps_valid(&absent), 0);
	absent.Flags |= S2WIRELESS_CAPF_SECURITY_SUMMARY;
	CHECK("raw PSK capability contract",
		"raw+summary without fourway rejected with zero masks",
		wpa_driver_sana2_wireless_caps_valid(&absent), 0);

	caps = valid_caps();
	caps.SizeAvailable--;
	++scenario_count;
	CHECK("short capability buffer", "rejected",
		wpa_driver_sana2_wireless_caps_valid(&caps), 0);
	caps = valid_caps();
	caps.SizeSupplied--;
	++scenario_count;
	CHECK("short capability response", "rejected",
		wpa_driver_sana2_wireless_caps_valid(&caps), 0);
	caps = valid_caps();
	caps.Version++;
	++scenario_count;
	CHECK("wrong capability version", "rejected",
		wpa_driver_sana2_wireless_caps_valid(&caps), 0);
	caps = valid_caps();
	caps.Flags |= 1UL << 7;
	++scenario_count;
	CHECK("unknown capability flag", "rejected",
		wpa_driver_sana2_wireless_caps_valid(&caps), 0);
	caps = valid_caps();
	caps.Protocols = S2WIRELESS_PROTO_RSN_SAE;
	++scenario_count;
	CHECK("unsupported SAE offload protocol", "rejected",
		wpa_driver_sana2_wireless_caps_valid(&caps), 0);
	caps = valid_caps();
	caps.KeyManagement = S2WIRELESS_KEY_EAP;
	++scenario_count;
	CHECK("unsupported EAP offload key management", "rejected",
		wpa_driver_sana2_wireless_caps_valid(&caps), 0);
	caps = valid_caps();
	caps.PairwiseCiphers = S2WIRELESS_CIPHER_GCMP;
	++scenario_count;
	CHECK("unsupported GCMP offload cipher", "rejected",
		wpa_driver_sana2_wireless_caps_valid(&caps), 0);
	caps = valid_caps();
	caps.Flags &= ~S2WIRELESS_CAPF_FOURWAY;
	++scenario_count;
	CHECK("masks without fourway declaration", "rejected",
		wpa_driver_sana2_wireless_caps_valid(&caps), 0);
}

static struct wpa_driver_capa get_driver_capa_from_actual_producer(
	struct Sana2WirelessCapabilities caps)
{
	struct wpa_driver_sana2_data drv;
	struct IOSana2Req request;
	struct wpa_driver_capa capa;

	memset(&drv, 0, sizeof(drv));
	memset(&request, 0, sizeof(request));
	memset(&capa, 0xa5, sizeof(capa));
	drv.request = &request;
	drv.hard_mac = 1;
	drv.wireless_caps_queried = 1;
	drv.wireless_caps_valid = 1;
	drv.wireless_caps = caps;
	wpa_driver_sana2_get_capa(&drv, &capa);
	return capa;
}

static void test_actual_driver_capability_mapping(void)
{
	struct Sana2WirelessCapabilities caps;
	struct wpa_driver_capa capa;

	++scenario_count;
	caps = valid_caps();
	capa = get_driver_capa_from_actual_producer(caps);
	CHECK("actual capability producer disjoint roles", "pairwise CCMP",
		capa.summary_pairwise_enc == WPA_DRIVER_CAPA_ENC_CCMP, 1);
	CHECK("actual capability producer disjoint roles", "group TKIP",
		capa.summary_group_enc == WPA_DRIVER_CAPA_ENC_TKIP, 1);
	CHECK("actual capability producer disjoint roles", "union encoding mask",
		capa.enc == (WPA_DRIVER_CAPA_ENC_CCMP |
			WPA_DRIVER_CAPA_ENC_TKIP), 1);
	CHECK("actual capability producer disjoint roles", "PSK and flags",
		(capa.key_mgmt & WPA_DRIVER_CAPA_KEY_MGMT_WPA2_PSK) &&
			(capa.flags & WPA_DRIVER_FLAGS_SECURITY_SUMMARY) &&
			(capa.flags & WPA_DRIVER_FLAGS_4WAY_HANDSHAKE), 1);

	++scenario_count;
	caps = valid_caps();
	caps.PairwiseCiphers = S2WIRELESS_CIPHER_TKIP;
	caps.GroupCiphers = S2WIRELESS_CIPHER_CCMP;
	capa = get_driver_capa_from_actual_producer(caps);
	CHECK("actual capability producer swapped roles", "role masks remain distinct",
		capa.summary_pairwise_enc == WPA_DRIVER_CAPA_ENC_TKIP &&
			capa.summary_group_enc == WPA_DRIVER_CAPA_ENC_CCMP, 1);

	++scenario_count;
	caps = valid_caps();
	caps.Flags = S2WIRELESS_CAPF_FOURWAY;
	capa = get_driver_capa_from_actual_producer(caps);
	CHECK("fourway without summary capability", "summary role fields stay zero",
		capa.summary_pairwise_enc == 0 && capa.summary_group_enc == 0, 1);
	CHECK("fourway without summary capability", "legacy union remains available",
		capa.enc == (WPA_DRIVER_CAPA_ENC_CCMP |
			WPA_DRIVER_CAPA_ENC_TKIP), 1);

	++scenario_count;
	memset(&caps, 0, sizeof(caps));
	capa = get_driver_capa_from_actual_producer(caps);
	CHECK("legacy capability path", "no summary role masks or offload flags",
		capa.summary_pairwise_enc == 0 && capa.summary_group_enc == 0 &&
			!(capa.flags & WPA_DRIVER_FLAGS_SECURITY_SUMMARY) &&
			!(capa.flags & WPA_DRIVER_FLAGS_4WAY_HANDSHAKE), 1);
	CHECK("legacy capability path", "legacy GETCRYPTTYPES remains intact",
		capa.enc == (WPA_DRIVER_CAPA_ENC_TKIP |
			WPA_DRIVER_CAPA_ENC_CCMP) &&
			(capa.key_mgmt & WPA_DRIVER_CAPA_KEY_MGMT_WPA_PSK) &&
			(capa.key_mgmt & WPA_DRIVER_CAPA_KEY_MGMT_WPA2_PSK), 1);

	++scenario_count;
	memset(&caps, 0, sizeof(caps));
	caps.Flags = S2WIRELESS_CAPF_SECURITY_SUMMARY;
	capa = get_driver_capa_from_actual_producer(caps);
	CHECK("summary only without fourway", "summary flag but no role masks",
		(capa.flags & WPA_DRIVER_FLAGS_SECURITY_SUMMARY) &&
			!(capa.flags & WPA_DRIVER_FLAGS_4WAY_HANDSHAKE) &&
			capa.summary_pairwise_enc == 0 &&
			capa.summary_group_enc == 0, 1);
	CHECK("summary only without fourway", "legacy encryption remains intact",
		capa.enc == (WPA_DRIVER_CAPA_ENC_TKIP |
			WPA_DRIVER_CAPA_ENC_CCMP), 1);
}

static void test_driver_mapping(void)
{
	struct Sana2WirelessSecuritySummary wire_summary;
	struct wpa_security_summary summary;
	unsigned int bss_caps;

	++scenario_count;
	wire_summary = valid_wire_summary();
	bss_caps = 0;
	summary = mapped_summary(&wire_summary, 1, &bss_caps, 1);
	CHECK("protected tuple mapping", "exact protected flags",
		summary.flags == (WPA_SECURITY_SUMMARY_PRESENT |
			WPA_SECURITY_SUMMARY_KNOWN | WPA_SECURITY_SUMMARY_PRIVACY), 1);
	CHECK("protected tuple mapping", "RSN PSK",
		summary.proto == WPA_PROTO_RSN &&
			summary.key_mgmt == WPA_KEY_MGMT_PSK, 1);
	CHECK("protected tuple mapping", "pairwise CCMP and group TKIP",
		summary.pairwise_cipher == WPA_CIPHER_CCMP &&
			summary.group_cipher == WPA_CIPHER_TKIP, 1);
	CHECK("protected tuple mapping", "privacy bit set",
		(bss_caps & IEEE80211_CAP_PRIVACY) != 0, 1);

	++scenario_count;
	memset(&wire_summary, 0, sizeof(wire_summary));
	wire_summary.Size = sizeof(wire_summary);
	wire_summary.Version = S2WIRELESS_SUMMARY_VERSION;
	wire_summary.Flags = S2WIRELESS_SUMF_KNOWN;
	bss_caps = IEEE80211_CAP_PRIVACY;
	summary = mapped_summary(&wire_summary, 1, &bss_caps, 1);
	CHECK("known open mapping", "known-open exact flags and zero fields",
		wpa_security_summary_open(&summary), 1);
	CHECK("known open mapping", "privacy bit cleared",
		(bss_caps & IEEE80211_CAP_PRIVACY) == 0, 1);

	++scenario_count;
	wire_summary = valid_wire_summary();
	bss_caps = 0;
	summary = mapped_summary(&wire_summary, 1, &bss_caps, 0);
	CHECK("missing tag is unknown", "not open and privacy set",
		summary.flags == (WPA_SECURITY_SUMMARY_PRESENT |
			WPA_SECURITY_SUMMARY_PRIVACY) &&
			(bss_caps & IEEE80211_CAP_PRIVACY), 1);
	CHECK("missing tag is unknown", "suite fields remain zero",
		!summary.proto && !summary.key_mgmt &&
			!summary.pairwise_cipher && !summary.group_cipher, 1);

	++scenario_count;
	wire_summary = valid_wire_summary();
	wire_summary.Size--;
	bss_caps = 0;
	summary = mapped_summary(&wire_summary, 1, &bss_caps, 1);
	CHECK("short summary tag is unknown", "unknown privacy-safe fallback",
		summary.flags == (WPA_SECURITY_SUMMARY_PRESENT |
			WPA_SECURITY_SUMMARY_PRIVACY) &&
			(bss_caps & IEEE80211_CAP_PRIVACY), 1);

	++scenario_count;
	wire_summary = valid_wire_summary();
	wire_summary.Version++;
	bss_caps = 0;
	summary = mapped_summary(&wire_summary, 1, &bss_caps, 1);
	CHECK("wrong summary version is unknown", "unknown privacy-safe fallback",
		summary.flags == (WPA_SECURITY_SUMMARY_PRESENT |
			WPA_SECURITY_SUMMARY_PRIVACY) &&
			(bss_caps & IEEE80211_CAP_PRIVACY), 1);

	++scenario_count;
	wire_summary = valid_wire_summary();
	wire_summary.Flags |= 1UL << 5;
	bss_caps = 0;
	summary = mapped_summary(&wire_summary, 1, &bss_caps, 1);
	CHECK("unknown summary flag is unknown", "unknown privacy-safe fallback",
		summary.flags == (WPA_SECURITY_SUMMARY_PRESENT |
			WPA_SECURITY_SUMMARY_PRIVACY), 1);

	++scenario_count;
	wire_summary = valid_wire_summary();
	wire_summary.Protocols |= S2WIRELESS_PROTO_WPA;
	bss_caps = 0;
	summary = mapped_summary(&wire_summary, 1, &bss_caps, 1);
	CHECK("ambiguous protocol summary is unknown", "unknown fallback",
		summary.flags == (WPA_SECURITY_SUMMARY_PRESENT |
			WPA_SECURITY_SUMMARY_PRIVACY), 1);

	++scenario_count;
	wire_summary = valid_wire_summary();
	bss_caps = 0;
	summary = mapped_summary(&wire_summary, 0, &bss_caps, 1);
	CHECK("summary capability absent", "mapping leaves zeroed output",
		!summary.flags && !summary.proto &&
			!(bss_caps & IEEE80211_CAP_PRIVACY), 1);
}

static struct wpa_ssid protected_profile(void)
{
	struct wpa_ssid ssid;

	memset(&ssid, 0, sizeof(ssid));
	ssid.proto = WPA_PROTO_RSN;
	ssid.key_mgmt = WPA_KEY_MGMT_PSK;
	ssid.pairwise_cipher = WPA_CIPHER_CCMP;
	ssid.group_cipher = WPA_CIPHER_TKIP;
	ssid.mode = WPAS_MODE_INFRA;
	return ssid;
}

static struct wpa_security_summary protected_summary(void)
{
	struct wpa_security_summary summary;

	memset(&summary, 0, sizeof(summary));
	summary.flags = WPA_SECURITY_SUMMARY_PRESENT |
		WPA_SECURITY_SUMMARY_KNOWN | WPA_SECURITY_SUMMARY_PRIVACY;
	summary.proto = WPA_PROTO_RSN;
	summary.key_mgmt = WPA_KEY_MGMT_PSK;
	summary.pairwise_cipher = WPA_CIPHER_CCMP;
	summary.group_cipher = WPA_CIPHER_TKIP;
	return summary;
}

static struct wpa_driver_capa protected_capa(void)
{
	struct wpa_driver_capa capa;

	memset(&capa, 0, sizeof(capa));
	capa.key_mgmt = WPA_DRIVER_CAPA_KEY_MGMT_WPA2_PSK;
	capa.flags = WPA_DRIVER_FLAGS_SECURITY_SUMMARY |
		WPA_DRIVER_FLAGS_4WAY_HANDSHAKE;
	capa.enc = WPA_DRIVER_CAPA_ENC_CCMP | WPA_DRIVER_CAPA_ENC_TKIP;
	capa.summary_pairwise_enc = WPA_DRIVER_CAPA_ENC_CCMP;
	capa.summary_group_enc = WPA_DRIVER_CAPA_ENC_TKIP;
	return capa;
}

static void test_role_specific_selector(void)
{
	struct wpa_supplicant wpa_s;
	struct wpa_ssid ssid;
	struct wpa_driver_capa capa;
	struct wpa_security_summary summary;

	++scenario_count;
	memset(&wpa_s, 0, sizeof(wpa_s));
	wpa_s.drv_flags = WPA_DRIVER_FLAGS_SECURITY_SUMMARY |
		WPA_DRIVER_FLAGS_4WAY_HANDSHAKE;
	ssid = protected_profile();
	capa = protected_capa();
	summary = protected_summary();
	CHECK("disjoint pair/group supported", "role-aware capa match",
		wpa_supplicant_summary_capa_match(&capa, &summary), 1);
	CHECK("disjoint pair/group supported", "complete config match",
		wpa_supplicant_summary_config_match(&wpa_s, &ssid, &capa,
			&summary), 1);
	CHECK("disjoint pair/group supported", "event selector tuple match",
		wpa_supplicant_summary_matches_ssid(&wpa_s, &ssid, &summary), 1);

	++scenario_count;
	capa = protected_capa();
	capa.summary_pairwise_enc = WPA_DRIVER_CAPA_ENC_TKIP;
	capa.summary_group_enc = WPA_DRIVER_CAPA_ENC_CCMP;
	CHECK("role-swapped capability rejected", "union alone insufficient",
		(capa.enc & (WPA_DRIVER_CAPA_ENC_CCMP |
			WPA_DRIVER_CAPA_ENC_TKIP)) ==
			(WPA_DRIVER_CAPA_ENC_CCMP | WPA_DRIVER_CAPA_ENC_TKIP), 1);
	CHECK("role-swapped capability rejected", "capa match fails",
		wpa_supplicant_summary_capa_match(&capa, &summary), 0);
	CHECK("role-swapped capability rejected", "config match fails",
		wpa_supplicant_summary_config_match(&wpa_s, &ssid, &capa,
			&summary), 0);

	++scenario_count;
	capa = protected_capa();
	capa.summary_pairwise_enc = 0;
	CHECK("missing pairwise capability", "capa match fails",
		wpa_supplicant_summary_capa_match(&capa, &summary), 0);
	capa = protected_capa();
	capa.summary_group_enc = 0;
	CHECK("missing group capability", "capa match fails",
		wpa_supplicant_summary_capa_match(&capa, &summary), 0);

	++scenario_count;
	capa = protected_capa();
	capa.flags &= ~WPA_DRIVER_FLAGS_SECURITY_SUMMARY;
	CHECK("missing capability flag", "config match fails",
		wpa_supplicant_summary_config_match(&wpa_s, &ssid, &capa,
			&summary), 0);
	capa = protected_capa();
	wpa_s.drv_flags &= ~WPA_DRIVER_FLAGS_4WAY_HANDSHAKE;
	CHECK("missing supplicant offload flag", "config match fails",
		wpa_supplicant_summary_config_match(&wpa_s, &ssid, &capa,
			&summary), 0);
	wpa_s.drv_flags |= WPA_DRIVER_FLAGS_4WAY_HANDSHAKE;

	++scenario_count;
	ssid = protected_profile();
	ssid.group_cipher = WPA_CIPHER_CCMP;
	CHECK("configuration tuple mismatch", "config match fails",
		wpa_supplicant_summary_config_match(&wpa_s, &ssid, &capa,
			&summary), 0);
	ssid = protected_profile();
	ssid.proto = WPA_PROTO_WPA;
	CHECK("protocol mismatch", "event selector fails",
		wpa_supplicant_summary_matches_ssid(&wpa_s, &ssid, &summary), 0);

	++scenario_count;
	ssid = protected_profile();
	ssid.mixed_cell = 1;
	CHECK("mixed-cell profile refused", "config and event paths fail",
		wpa_supplicant_summary_config_match(&wpa_s, &ssid, &capa,
			&summary) == 0 &&
		wpa_supplicant_summary_matches_ssid(&wpa_s, &ssid,
			&summary) == 0, 1);
	ssid = protected_profile();
	ssid.mode = 1;
	CHECK("non-infrastructure profile refused", "config and event paths fail",
		wpa_supplicant_summary_config_match(&wpa_s, &ssid, &capa,
			&summary) == 0 &&
		wpa_supplicant_summary_matches_ssid(&wpa_s, &ssid,
			&summary) == 0, 1);
	ssid = protected_profile();
	ssid.auth_alg = WPA_AUTH_ALG_SHARED;
	CHECK("shared auth profile refused", "config and event paths fail",
		wpa_supplicant_summary_config_match(&wpa_s, &ssid, &capa,
			&summary) == 0 &&
		wpa_supplicant_summary_matches_ssid(&wpa_s, &ssid,
			&summary) == 0, 1);

	++scenario_count;
	ssid = protected_profile();
	ssid.key_mgmt |= WPA_KEY_MGMT_WPS;
	CHECK("WPS profile refused", "config and event paths fail",
		wpa_supplicant_summary_config_match(&wpa_s, &ssid, &capa,
			&summary) == 0 &&
		wpa_supplicant_summary_matches_ssid(&wpa_s, &ssid,
			&summary) == 0, 1);
	ssid = protected_profile();
	ssid.ieee80211w = 1;
	CHECK("PMF profile refused", "config and event paths fail",
		wpa_supplicant_summary_config_match(&wpa_s, &ssid, &capa,
			&summary) == 0 &&
		wpa_supplicant_summary_matches_ssid(&wpa_s, &ssid,
			&summary) == 0, 1);

	++scenario_count;
	memset(&wpa_s, 0, sizeof(wpa_s));
	CHECK("missing offload flags refused", "event selector fails",
		wpa_supplicant_summary_matches_ssid(&wpa_s, &ssid, &summary), 0);
	wpa_s.drv_flags = WPA_DRIVER_FLAGS_SECURITY_SUMMARY |
		WPA_DRIVER_FLAGS_4WAY_HANDSHAKE;
	ssid = protected_profile();
	ssid.disabled = 1;
	CHECK("disabled profile refused", "event selector fails",
		wpa_supplicant_summary_matches_ssid(&wpa_s, &ssid, &summary), 0);
}

static void test_open_profiles(void)
{
	struct wpa_ssid ssid;

	++scenario_count;
	memset(&ssid, 0, sizeof(ssid));
	ssid.key_mgmt = WPA_KEY_MGMT_NONE;
	ssid.mode = WPAS_MODE_INFRA;
	CHECK("plain open profile", "supplicant open helper accepts",
		wpa_supplicant_summary_open_profile(&ssid), 1);
	CHECK("plain open profile", "event open helper accepts",
		wpa_events_summary_open_profile(&ssid), 1);

	++scenario_count;
	ssid.key_mgmt = WPA_KEY_MGMT_PSK;
	CHECK("protected profile is not open", "both open helpers reject",
		!wpa_supplicant_summary_open_profile(&ssid) &&
		!wpa_events_summary_open_profile(&ssid), 1);

	++scenario_count;
	memset(&ssid, 0, sizeof(ssid));
	ssid.key_mgmt = WPA_KEY_MGMT_NONE;
	ssid.mode = WPAS_MODE_INFRA;
	ssid.wep_key_len[2] = 5;
	CHECK("WEP profile is not open", "both open helpers reject",
		!wpa_supplicant_summary_open_profile(&ssid) &&
		!wpa_events_summary_open_profile(&ssid), 1);
	ssid.wep_key_len[2] = 0;
	ssid.mixed_cell = 1;
	CHECK("mixed-cell open profile refused", "both open helpers reject",
		!wpa_supplicant_summary_open_profile(&ssid) &&
		!wpa_events_summary_open_profile(&ssid), 1);
	ssid.mixed_cell = 0;
	ssid.auth_alg = WPA_AUTH_ALG_SHARED;
	CHECK("shared-auth open profile refused", "both open helpers reject",
		!wpa_supplicant_summary_open_profile(&ssid) &&
		!wpa_events_summary_open_profile(&ssid), 1);
	ssid.auth_alg = 0;
	ssid.mode = 1;
	CHECK("non-infrastructure open profile refused", "both open helpers reject",
		!wpa_supplicant_summary_open_profile(&ssid) &&
		!wpa_events_summary_open_profile(&ssid), 1);
	ssid.mode = WPAS_MODE_INFRA;
	ssid.ieee80211w = 1;
	CHECK("PMF open profile refused", "both open helpers reject",
		!wpa_supplicant_summary_open_profile(&ssid) &&
		!wpa_events_summary_open_profile(&ssid), 1);
	ssid.ieee80211w = 0;
	ssid.key_mgmt |= WPA_KEY_MGMT_WPS;
	CHECK("WPS open profile refused", "both open helpers reject",
		!wpa_supplicant_summary_open_profile(&ssid) &&
		!wpa_events_summary_open_profile(&ssid), 1);
}

int main(void)
{
	test_caps_validator();
	test_actual_driver_capability_mapping();
	test_driver_mapping();
	test_role_specific_selector();
	test_open_profiles();

	if (failure_count != 0) {
		fprintf(stderr, "security-summary integration: %u failures in %u checks\n",
			failure_count, check_count);
		return EXIT_FAILURE;
	}
	printf("security-summary integration: %u scenarios, %u checks passed\n",
		scenario_count, check_count);
	return EXIT_SUCCESS;
}
