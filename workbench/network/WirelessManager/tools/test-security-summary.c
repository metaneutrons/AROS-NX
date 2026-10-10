#ifdef SECURITY_SUMMARY_ABI_FIXTURE
#include <stddef.h>
#include <devices/sana2wireless.h>

typedef char abi_ulong_is_32_bits[(sizeof(ULONG) == 4) ? 1 : -1];
typedef char abi_capabilities_size[(sizeof(struct Sana2WirelessCapabilities) == 32) ? 1 : -1];
typedef char abi_cap_size_available[(offsetof(struct Sana2WirelessCapabilities, SizeAvailable) == 0) ? 1 : -1];
typedef char abi_cap_size_supplied[(offsetof(struct Sana2WirelessCapabilities, SizeSupplied) == 4) ? 1 : -1];
typedef char abi_cap_version[(offsetof(struct Sana2WirelessCapabilities, Version) == 8) ? 1 : -1];
typedef char abi_cap_flags[(offsetof(struct Sana2WirelessCapabilities, Flags) == 12) ? 1 : -1];
typedef char abi_cap_protocols[(offsetof(struct Sana2WirelessCapabilities, Protocols) == 16) ? 1 : -1];
typedef char abi_cap_key_management[(offsetof(struct Sana2WirelessCapabilities, KeyManagement) == 20) ? 1 : -1];
typedef char abi_cap_pairwise[(offsetof(struct Sana2WirelessCapabilities, PairwiseCiphers) == 24) ? 1 : -1];
typedef char abi_cap_group[(offsetof(struct Sana2WirelessCapabilities, GroupCiphers) == 28) ? 1 : -1];

typedef char abi_summary_size[(sizeof(struct Sana2WirelessSecuritySummary) == 28) ? 1 : -1];
typedef char abi_summary_size_field[(offsetof(struct Sana2WirelessSecuritySummary, Size) == 0) ? 1 : -1];
typedef char abi_summary_version[(offsetof(struct Sana2WirelessSecuritySummary, Version) == 4) ? 1 : -1];
typedef char abi_summary_flags[(offsetof(struct Sana2WirelessSecuritySummary, Flags) == 8) ? 1 : -1];
typedef char abi_summary_protocols[(offsetof(struct Sana2WirelessSecuritySummary, Protocols) == 12) ? 1 : -1];
typedef char abi_summary_key_management[(offsetof(struct Sana2WirelessSecuritySummary, KeyManagement) == 16) ? 1 : -1];
typedef char abi_summary_pairwise[(offsetof(struct Sana2WirelessSecuritySummary, PairwiseCiphers) == 20) ? 1 : -1];
typedef char abi_summary_group[(offsetof(struct Sana2WirelessSecuritySummary, GroupCiphers) == 24) ? 1 : -1];
#else
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* security_summary.h uses the same BIT definitions as WirelessManager. */
#include "utils/common.h"
#include "common/security_summary.h"

static unsigned int scenario_count;
static unsigned int predicate_check_count;
static unsigned int failure_count;

static void check_predicate(const char *scenario, const char *predicate,
                            int actual, int expected)
{
	++predicate_check_count;
	if (!!actual != !!expected) {
		fprintf(stderr, "FAIL: %s: %s returned %d, expected %d\n",
			scenario, predicate, actual, expected);
		++failure_count;
	}
}

/* One scenario may exercise all three helpers; assertions are not scenarios. */
static void check_scenario(const char *name,
			   const struct wpa_security_summary *summary,
			   int offload, int proto, int key_mgmt,
			   int pairwise, int group,
			   int expect_protected, int expect_open,
			   int expect_match)
{
	++scenario_count;
	check_predicate(name, "protected",
		wpa_security_summary_protected(summary), expect_protected);
	check_predicate(name, "open",
		wpa_security_summary_open(summary), expect_open);
	check_predicate(name, "match",
		wpa_security_summary_match(summary, offload, proto, key_mgmt,
					   pairwise, group), expect_match);
}

static struct wpa_security_summary protected_summary(int proto,
						      int key_mgmt,
						      int pairwise,
						      int group)
{
	struct wpa_security_summary summary;

	summary.flags = WPA_SECURITY_SUMMARY_PRESENT |
		WPA_SECURITY_SUMMARY_KNOWN | WPA_SECURITY_SUMMARY_PRIVACY;
	summary.proto = proto;
	summary.key_mgmt = key_mgmt;
	summary.pairwise_cipher = pairwise;
	summary.group_cipher = group;
	return summary;
}

static struct wpa_security_summary open_summary(void)
{
	struct wpa_security_summary summary;

	summary.flags = WPA_SECURITY_SUMMARY_PRESENT |
		WPA_SECURITY_SUMMARY_KNOWN;
	summary.proto = 0;
	summary.key_mgmt = 0;
	summary.pairwise_cipher = 0;
	summary.group_cipher = 0;
	return summary;
}

static void check_protected_rejection(const char *name,
				     struct wpa_security_summary summary)
{
	check_scenario(name, &summary, 1, WPA_PROTO_WPA, WPA_KEY_MGMT_PSK,
			WPA_CIPHER_CCMP, WPA_CIPHER_TKIP, 0, 0, 0);
}

static void check_open_rejection(const char *name,
				 struct wpa_security_summary summary)
{
	check_scenario(name, &summary, 1, WPA_PROTO_WPA, WPA_KEY_MGMT_PSK,
			WPA_CIPHER_CCMP, WPA_CIPHER_CCMP, 0, 0, 0);
}

static void run_positive_singletons(void)
{
	static const int protocols[] = { WPA_PROTO_WPA, WPA_PROTO_RSN };
	static const int ciphers[] = { WPA_CIPHER_TKIP, WPA_CIPHER_CCMP };
	char name[80];
	size_t p, pair, group;
	struct wpa_security_summary summary;

	for (p = 0; p < sizeof(protocols) / sizeof(protocols[0]); ++p) {
		for (pair = 0; pair < sizeof(ciphers) / sizeof(ciphers[0]); ++pair) {
			for (group = 0; group < sizeof(ciphers) / sizeof(ciphers[0]);
			     ++group) {
				summary = protected_summary(protocols[p],
							   WPA_KEY_MGMT_PSK,
							   ciphers[pair],
							   ciphers[group]);
				(void) snprintf(name, sizeof(name),
						 "protected singleton p=%lu pair=%lu group=%lu",
						 (unsigned long) p, (unsigned long) pair,
						 (unsigned long) group);
				check_scenario(name, &summary, 1, protocols[p],
						WPA_KEY_MGMT_PSK, ciphers[pair],
						ciphers[group], 1, 0, 1);
			}
		}
	}

	summary = open_summary();
	check_scenario("known open summary", &summary, 1, 0, 0, 0, 0,
			0, 1, 0);
}

static void test_protected_flags(void)
{
	static const unsigned int invalid_flags[] = {
		0,
		WPA_SECURITY_SUMMARY_PRESENT,
		WPA_SECURITY_SUMMARY_PRESENT | WPA_SECURITY_SUMMARY_PRIVACY,
		WPA_SECURITY_SUMMARY_KNOWN | WPA_SECURITY_SUMMARY_PRIVACY,
		WPA_SECURITY_SUMMARY_PRESENT | WPA_SECURITY_SUMMARY_KNOWN |
			WPA_SECURITY_SUMMARY_PRIVACY | 0x08u,
		WPA_SECURITY_SUMMARY_PRESENT | WPA_SECURITY_SUMMARY_KNOWN |
			WPA_SECURITY_SUMMARY_PRIVACY | 0x80000000u
	};
	char name[64];
	size_t i;
	struct wpa_security_summary summary;

	for (i = 0; i < sizeof(invalid_flags) / sizeof(invalid_flags[0]); ++i) {
		summary = protected_summary(WPA_PROTO_RSN, WPA_KEY_MGMT_PSK,
					    WPA_CIPHER_CCMP, WPA_CIPHER_CCMP);
		summary.flags = invalid_flags[i];
		(void) snprintf(name, sizeof(name), "protected invalid flags %lu",
				(unsigned long) i);
		check_protected_rejection(name, summary);
	}
}

static void test_protected_suite_rejections(void)
{
	static const int invalid_protocols[] = {
		0,
		WPA_PROTO_WPA | WPA_PROTO_RSN,
		BIT(2),
		WPA_PROTO_RSN | BIT(2),
		-1
	};
	static const int invalid_key_mgmt[] = {
		0,
		WPA_KEY_MGMT_IEEE8021X,
		WPA_KEY_MGMT_NONE,
		WPA_KEY_MGMT_IEEE8021X_NO_WPA,
		WPA_KEY_MGMT_WPA_NONE,
		WPA_KEY_MGMT_FT_IEEE8021X,
		WPA_KEY_MGMT_FT_PSK,
		WPA_KEY_MGMT_IEEE8021X_SHA256,
		WPA_KEY_MGMT_PSK_SHA256,
		WPA_KEY_MGMT_WPS,
		WPA_KEY_MGMT_PSK | WPA_KEY_MGMT_IEEE8021X,
		BIT(10)
	};
	static const int unsupported_ciphers[] = {
		0,
		WPA_CIPHER_NONE,
		WPA_CIPHER_WEP40,
		WPA_CIPHER_WEP104,
		WPA_CIPHER_TKIP | WPA_CIPHER_CCMP,
		WPA_CIPHER_CCMP | WPA_CIPHER_WEP104,
		BIT(9),
#ifdef CONFIG_IEEE80211W
		WPA_CIPHER_AES_128_CMAC,
#endif
	};
	char name[80];
	size_t i;
	struct wpa_security_summary summary;

	for (i = 0; i < sizeof(invalid_protocols) / sizeof(invalid_protocols[0]);
	     ++i) {
		summary = protected_summary(invalid_protocols[i], WPA_KEY_MGMT_PSK,
					    WPA_CIPHER_CCMP, WPA_CIPHER_CCMP);
		(void) snprintf(name, sizeof(name), "unsupported protocol %lu",
				(unsigned long) i);
		check_protected_rejection(name, summary);
	}
	for (i = 0; i < sizeof(invalid_key_mgmt) / sizeof(invalid_key_mgmt[0]);
	     ++i) {
		summary = protected_summary(WPA_PROTO_RSN, invalid_key_mgmt[i],
					    WPA_CIPHER_CCMP, WPA_CIPHER_CCMP);
		(void) snprintf(name, sizeof(name), "unsupported key management %lu",
				(unsigned long) i);
		check_protected_rejection(name, summary);
	}
	for (i = 0; i < sizeof(unsupported_ciphers) /
			     sizeof(unsupported_ciphers[0]); ++i) {
		summary = protected_summary(WPA_PROTO_RSN, WPA_KEY_MGMT_PSK,
					    unsupported_ciphers[i], WPA_CIPHER_CCMP);
		(void) snprintf(name, sizeof(name), "unsupported pairwise cipher %lu",
				(unsigned long) i);
		check_protected_rejection(name, summary);

		summary = protected_summary(WPA_PROTO_RSN, WPA_KEY_MGMT_PSK,
					    WPA_CIPHER_CCMP, unsupported_ciphers[i]);
		(void) snprintf(name, sizeof(name), "unsupported group cipher %lu",
				(unsigned long) i);
		check_protected_rejection(name, summary);
	}
}

static void test_open_rejections(void)
{
	static const unsigned int invalid_flags[] = {
		0,
		WPA_SECURITY_SUMMARY_PRESENT,
		WPA_SECURITY_SUMMARY_KNOWN,
		WPA_SECURITY_SUMMARY_PRESENT | WPA_SECURITY_SUMMARY_KNOWN |
			WPA_SECURITY_SUMMARY_PRIVACY,
		WPA_SECURITY_SUMMARY_PRESENT | WPA_SECURITY_SUMMARY_KNOWN | 0x08u
	};
	char name[64];
	size_t i;
	struct wpa_security_summary summary;

	for (i = 0; i < sizeof(invalid_flags) / sizeof(invalid_flags[0]); ++i) {
		summary = open_summary();
		summary.flags = invalid_flags[i];
		(void) snprintf(name, sizeof(name), "open invalid flags %lu",
				(unsigned long) i);
		check_open_rejection(name, summary);
	}

	summary = open_summary();
	summary.proto = WPA_PROTO_WPA;
	check_open_rejection("open with protocol", summary);
	summary = open_summary();
	summary.key_mgmt = WPA_KEY_MGMT_NONE;
	check_open_rejection("open with key management", summary);
	summary = open_summary();
	summary.pairwise_cipher = WPA_CIPHER_CCMP;
	check_open_rejection("open with pairwise cipher", summary);
	summary = open_summary();
	summary.group_cipher = WPA_CIPHER_CCMP;
	check_open_rejection("open with group cipher", summary);
}

static void test_match_gates_and_tuple_mismatches(void)
{
	static const int protocols[] = { WPA_PROTO_WPA, WPA_PROTO_RSN };
	static const int ciphers[] = { WPA_CIPHER_TKIP, WPA_CIPHER_CCMP };
	char name[80];
	size_t p, pair, group;
	struct wpa_security_summary summary;

	summary = protected_summary(WPA_PROTO_RSN, WPA_KEY_MGMT_PSK,
				    WPA_CIPHER_CCMP, WPA_CIPHER_CCMP);
	check_scenario("offload unavailable", &summary, 0, WPA_PROTO_RSN,
			WPA_KEY_MGMT_PSK, WPA_CIPHER_CCMP, WPA_CIPHER_CCMP,
			1, 0, 0);

	for (p = 0; p < sizeof(protocols) / sizeof(protocols[0]); ++p) {
		for (pair = 0; pair < sizeof(ciphers) / sizeof(ciphers[0]); ++pair) {
			for (group = 0; group < sizeof(ciphers) / sizeof(ciphers[0]);
			     ++group) {
				summary = protected_summary(protocols[p],
							   WPA_KEY_MGMT_PSK,
							   ciphers[pair],
							   ciphers[group]);
				(void) snprintf(name, sizeof(name),
						 "protocol mismatch p=%lu pair=%lu group=%lu",
						 (unsigned long) p, (unsigned long) pair,
						 (unsigned long) group);
				check_scenario(name, &summary, 1,
						protocols[p] == WPA_PROTO_WPA ?
						WPA_PROTO_RSN : WPA_PROTO_WPA,
						WPA_KEY_MGMT_PSK, ciphers[pair],
						ciphers[group], 1, 0, 0);

				(void) snprintf(name, sizeof(name),
						 "key-management mismatch p=%lu pair=%lu group=%lu",
						 (unsigned long) p, (unsigned long) pair,
						 (unsigned long) group);
				check_scenario(name, &summary, 1, protocols[p],
						WPA_KEY_MGMT_IEEE8021X,
						ciphers[pair], ciphers[group], 1, 0, 0);

				(void) snprintf(name, sizeof(name),
						 "pairwise mismatch p=%lu pair=%lu group=%lu",
						 (unsigned long) p, (unsigned long) pair,
						 (unsigned long) group);
				check_scenario(name, &summary, 1, protocols[p],
						WPA_KEY_MGMT_PSK,
						ciphers[pair] == WPA_CIPHER_TKIP ?
						WPA_CIPHER_CCMP : WPA_CIPHER_TKIP,
						ciphers[group], 1, 0, 0);

				(void) snprintf(name, sizeof(name),
						 "group mismatch p=%lu pair=%lu group=%lu",
						 (unsigned long) p, (unsigned long) pair,
						 (unsigned long) group);
				check_scenario(name, &summary, 1, protocols[p],
						WPA_KEY_MGMT_PSK, ciphers[pair],
						ciphers[group] == WPA_CIPHER_TKIP ?
						WPA_CIPHER_CCMP : WPA_CIPHER_TKIP,
						1, 0, 0);
			}
		}
	}
}

int main(void)
{
	run_positive_singletons();
	test_protected_flags();
	test_protected_suite_rejections();
	test_open_rejections();
	test_match_gates_and_tuple_mismatches();

	if (failure_count != 0) {
		fprintf(stderr, "%u failures across %u scenarios (%u predicate checks)\n",
			failure_count, scenario_count, predicate_check_count);
		return EXIT_FAILURE;
	}
	printf("security-summary: %u scenarios, %u predicate checks passed\n",
	       scenario_count, predicate_check_count);
	return EXIT_SUCCESS;
}
#endif /* SECURITY_SUMMARY_ABI_FIXTURE */
