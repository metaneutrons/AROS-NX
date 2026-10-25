#!/usr/bin/env python3
"""Host-test the source-extracted wpa_supplicant FullMAC SAE caller path."""

from __future__ import annotations

import os
import re
import shlex
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
WM_ROOT = ROOT / "workbench/network/WirelessManager"
WM_SRC = WM_ROOT / "src"
SUPPLICANT = WM_ROOT / "wpa_supplicant/wpa_supplicant.c"
EVENTS = WM_ROOT / "wpa_supplicant/events.c"


TEST_SOURCE = r'''#define CONFIG_IEEE80211W 1

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/common.h"
#include "utils/wpabuf.h"
#include "utils/wpa_debug.h"
#include "common/defs.h"
#include "common/ieee802_11_defs.h"
#include "common/security_summary.h"
#include "wpa_supplicant/config_ssid.h"
#include "drivers/driver.h"
#include "wpa_supplicant/wpa_supplicant_i.h"
#include "wpa_supplicant/bss.h"
#include "wpa_supplicant/driver_i.h"

#define REQUIRED_FLAGS (WPA_DRIVER_FLAGS_SECURITY_SUMMARY | \
                        WPA_DRIVER_FLAGS_FULLMAC_ASSOCIATION | \
                        WPA_DRIVER_FLAGS_CANCEL_PENDING)

struct call_observation {
	unsigned int calls;
	const u8 *bssid;
	const u8 *ssid;
	size_t ssid_len;
	const u8 *wpa_ie;
	size_t wpa_ie_len;
	const char *passphrase;
	const u8 *psk;
	const u8 *sae_password;
	size_t sae_password_len;
	unsigned int profile;
	unsigned int diagnostic;
	unsigned int pwe;
	enum wpa_key_mgmt key_mgmt;
	enum wpa_cipher pairwise;
	enum wpa_cipher group;
	int mode;
	int auth_alg;
	int freq;
	int drop_unencrypted;
	u8 sae_password_copy[64];
};

struct rsn_supp_config;
struct wpa_cred;

struct mock_state {
	struct wpa_driver_capa capa;
	int get_capa_result;
	int get_bssid_result;
	int get_bssid_leave_output_untouched;
	int associate_result;
	u8 driver_bssid[ETH_ALEN];
	unsigned int capa_calls;
	unsigned int bssid_calls;
	unsigned int associate_calls;
};

struct effects {
	unsigned int timeout_request_calls;
	unsigned int timeout_cancel_calls;
	unsigned int timeout_cancel_no_blacklist_calls;
	unsigned int cancel_scan_calls;
	unsigned int request_scan_calls;
	int request_scan_sec;
	int request_scan_usec;
	unsigned int failed_event_calls;
	unsigned int unexpected_event_calls;
	unsigned int state_changes;
	unsigned int notify_bssid_calls;
	unsigned int notify_network_calls;
	unsigned int wpabuf_free_calls;
	unsigned int sm_assoc_ie_clear_calls;
	unsigned int sm_ap_wpa_ie_clear_calls;
	unsigned int sm_ap_rsn_ie_clear_calls;
	unsigned int sm_config_clear_calls;
	unsigned int eapol_config_clear_calls;
	unsigned int port_enabled_true_calls;
	unsigned int port_enabled_false_calls;
	unsigned int port_valid_true_calls;
	unsigned int port_valid_false_calls;
	unsigned int eap_success_true_calls;
	unsigned int eap_success_false_calls;
	unsigned int sm_disassoc_calls;
	unsigned int auth_timeout_sec;
	unsigned int auth_timeout_usec;
};

static struct mock_state mock;
static struct effects effect;
static struct call_observation observed;
static int mock_vendor_ie;
static int mock_rsn_ie;
static const u8 fake_ie[] = { 0x30, 0x00 };
static u8 test_ssid_bytes[] = "synthetic-sae-network";
static const u8 test_sae_password[] = "synthetic-only-sae-password";
static const u8 test_bssid[ETH_ALEN] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static struct wpa_supplicant wpa_s;
static struct wpa_ssid ssid;
static struct wpa_bss bss;

static int mock_get_capa(void *priv, struct wpa_driver_capa *capa)
{
	struct mock_state *state = priv;
	++state->capa_calls;
	if (state->get_capa_result < 0)
		return state->get_capa_result;
	*capa = state->capa;
	return state->get_capa_result;
}

static int mock_get_bssid(void *priv, u8 *bssid)
{
	struct mock_state *state = priv;
	++state->bssid_calls;
	if (!state->get_bssid_leave_output_untouched)
		os_memcpy(bssid, state->driver_bssid, ETH_ALEN);
	return state->get_bssid_result;
}

static int mock_associate(void *priv,
			  struct wpa_driver_associate_params *params)
{
	struct mock_state *state = priv;
	++state->associate_calls;
	++observed.calls;
	observed.bssid = params->bssid;
	observed.ssid = params->ssid;
	observed.ssid_len = params->ssid_len;
	observed.wpa_ie = params->wpa_ie;
	observed.wpa_ie_len = params->wpa_ie_len;
	observed.passphrase = params->passphrase;
	observed.psk = params->psk;
	observed.sae_password = params->sae_password;
	observed.sae_password_len = params->sae_password_len;
	observed.profile = params->fullmac_profile;
	observed.diagnostic = params->fullmac_allow_diagnostic;
	observed.pwe = params->fullmac_sae_pwe_mode;
	observed.key_mgmt = params->key_mgmt_suite;
	observed.pairwise = params->pairwise_suite;
	observed.group = params->group_suite;
	observed.mode = params->mode;
	observed.auth_alg = params->auth_alg;
	observed.freq = params->freq;
	observed.drop_unencrypted = params->drop_unencrypted;
	if (params->sae_password && params->sae_password_len <=
	    sizeof(observed.sae_password_copy))
		os_memcpy(observed.sae_password_copy, params->sae_password,
			  params->sae_password_len);
	return state->associate_result;
}

static struct wpa_driver_ops mock_driver = {
	.name = "fullmac-caller-test",
	.associate = mock_associate,
	.get_bssid = mock_get_bssid,
	.get_capa = mock_get_capa
};

void wpa_supplicant_set_state(struct wpa_supplicant *wpa,
			      enum wpa_states state)
{
	++effect.state_changes;
	wpa->wpa_state = state;
}

void wpa_supplicant_req_auth_timeout(struct wpa_supplicant *wpa,
				     int sec, int usec)
{
	(void) wpa;
	++effect.timeout_request_calls;
	effect.auth_timeout_sec = (unsigned int) sec;
	effect.auth_timeout_usec = (unsigned int) usec;
}

void wpa_supplicant_cancel_auth_timeout(struct wpa_supplicant *wpa)
{
	(void) wpa;
	++effect.timeout_cancel_calls;
}

void wpa_supplicant_cancel_auth_timeout_no_blacklist(
	struct wpa_supplicant *wpa)
{
	(void) wpa;
	++effect.timeout_cancel_no_blacklist_calls;
}

static void wpa_supplicant_cancel_scan(struct wpa_supplicant *wpa)
{
	(void) wpa;
	++effect.cancel_scan_calls;
}

static void wpa_supplicant_req_scan(struct wpa_supplicant *wpa,
				    int sec, int usec)
{
	(void) wpa;
	++effect.request_scan_calls;
	effect.request_scan_sec = sec;
	effect.request_scan_usec = usec;
}

void wpa_supplicant_disassociate(struct wpa_supplicant *wpa,
				int reason_code)
{
	(void) wpa;
	(void) reason_code;
}

static void wpas_notify_bssid_changed(struct wpa_supplicant *wpa)
{
	(void) wpa;
	++effect.notify_bssid_calls;
}

static void wpas_notify_network_changed(struct wpa_supplicant *wpa)
{
	(void) wpa;
	++effect.notify_network_calls;
}

static void wpa_sm_set_assoc_wpa_ie(struct wpa_sm *sm, const u8 *ie,
				    size_t len)
{
	(void) sm;
	if (!ie && !len)
		++effect.sm_assoc_ie_clear_calls;
}

static void wpa_sm_set_ap_wpa_ie(struct wpa_sm *sm, const u8 *ie, size_t len)
{
	(void) sm;
	if (!ie && !len)
		++effect.sm_ap_wpa_ie_clear_calls;
}

static void wpa_sm_set_ap_rsn_ie(struct wpa_sm *sm, const u8 *ie, size_t len)
{
	(void) sm;
	if (!ie && !len)
		++effect.sm_ap_rsn_ie_clear_calls;
}

static void wpa_sm_set_config(struct wpa_sm *sm,
			      struct rsn_supp_config *config)
{
	(void) sm;
	if (!config)
		++effect.sm_config_clear_calls;
}

static void eapol_sm_notify_config(struct eapol_sm *sm,
				   struct wpa_ssid *config,
				   struct wpa_cred *cred)
{
	(void) sm;
	(void) cred;
	if (!config)
		++effect.eapol_config_clear_calls;
}

static void eapol_sm_notify_portEnabled(struct eapol_sm *sm, int enabled)
{
	(void) sm;
	if (enabled)
		++effect.port_enabled_true_calls;
	else
		++effect.port_enabled_false_calls;
}

static void eapol_sm_notify_portValid(struct eapol_sm *sm, int valid)
{
	(void) sm;
	if (valid)
		++effect.port_valid_true_calls;
	else
		++effect.port_valid_false_calls;
}

static void eapol_sm_notify_eap_success(struct eapol_sm *sm, int success)
{
	(void) sm;
	if (success)
		++effect.eap_success_true_calls;
	else
		++effect.eap_success_false_calls;
}

static void wpa_sm_notify_disassoc(struct wpa_sm *sm)
{
	(void) sm;
	++effect.sm_disassoc_calls;
}

void wpabuf_free(struct wpabuf *buf)
{
	if (buf)
		++effect.wpabuf_free_calls;
}

void wpa_printf(int level, const char *fmt, ...)
{
	(void) level;
	(void) fmt;
}

void wpa_msg(void *ctx, int level, const char *fmt, ...)
{
	(void) ctx;
	(void) level;
	(void) fmt;
}

const u8 * wpa_bss_get_vendor_ie(const struct wpa_bss *entry, u32 vendor_type)
{
	(void) entry;
	(void) vendor_type;
	return mock_vendor_ie ? fake_ie : NULL;
}

const u8 * wpa_bss_get_ie(const struct wpa_bss *entry, u8 ie)
{
	(void) entry;
	(void) ie;
	return mock_rsn_ie ? fake_ie : NULL;
}

static void reset_effects(void)
{
	os_memset(&effect, 0, sizeof(effect));
	os_memset(&observed, 0, sizeof(observed));
}

static void init_fixture(void)
{
	os_memset(&mock, 0, sizeof(mock));
	reset_effects();
	mock.capa.flags = REQUIRED_FLAGS;
	mock.capa.fullmac_profiles = WPA_FULLMAC_PROFILE_SAE_CCMP_PMF;
	mock.capa.fullmac_sae_pwe_modes = WPA_FULLMAC_SAE_PWE_H2E;
	mock.driver_bssid[0] = test_bssid[0];
	os_memcpy(mock.driver_bssid + 1, test_bssid + 1, ETH_ALEN - 1);
	mock_vendor_ie = 0;
	mock_rsn_ie = 0;

	os_memset(&wpa_s, 0, sizeof(wpa_s));
	os_memset(&ssid, 0, sizeof(ssid));
	os_memset(&bss, 0, sizeof(bss));
	wpa_s.driver = &mock_driver;
	wpa_s.drv_priv = &mock;
	wpa_s.drv_flags = REQUIRED_FLAGS; /* Intentionally no FOURWAY. */
	wpa_s.wpa_state = WPA_DISCONNECTED;
	wpa_s.wpa = (struct wpa_sm *) (uintptr_t) 0x101;
	wpa_s.eapol = (struct eapol_sm *) (uintptr_t) 0x202;

	ssid.mode = WPAS_MODE_INFRA;
	ssid.proto = WPA_PROTO_RSN;
	ssid.key_mgmt = WPA_KEY_MGMT_SAE;
	ssid.pairwise_cipher = WPA_CIPHER_CCMP;
	ssid.group_cipher = WPA_CIPHER_CCMP;
	ssid.auth_alg = WPA_AUTH_ALG_OPEN;
	ssid.ieee80211w = MGMT_FRAME_PROTECTION_REQUIRED;
	ssid.ssid = (u8 *) test_ssid_bytes;
	ssid.ssid_len = sizeof(test_ssid_bytes) - 1;
	ssid.sae_password = (u8 *) test_sae_password;
	ssid.sae_password_len = sizeof(test_sae_password) - 1;

	os_memcpy(bss.ssid, test_ssid_bytes, sizeof(test_ssid_bytes) - 1);
	bss.ssid_len = sizeof(test_ssid_bytes) - 1;
	os_memcpy(bss.bssid, test_bssid, ETH_ALEN);
	bss.freq = 2412;
	bss.security_summary.flags = WPA_SECURITY_SUMMARY_PRESENT |
		WPA_SECURITY_SUMMARY_KNOWN | WPA_SECURITY_SUMMARY_PRIVACY;
	bss.security_summary.proto = WPA_PROTO_RSN;
	bss.security_summary.key_mgmt = WPA_KEY_MGMT_SAE;
	bss.security_summary.pairwise_cipher = WPA_CIPHER_CCMP;
	bss.security_summary.group_cipher = WPA_CIPHER_CCMP;
}

static void check_value(const char *scenario, const char *predicate,
			int actual, int expected)
{
	static unsigned int checks;
	static unsigned int failures;
	++checks;
	if (actual != expected) {
		++failures;
		fprintf(stderr, "FAIL: %s: %s got %d expected %d\n",
			scenario, predicate, actual, expected);
	}
	if (strcmp(scenario, "__report__") == 0) {
		printf("PASS fullmac-caller checks=%u failures=%u ABI=host-mock-not-native-RV32\n",
		       checks, failures);
		if (failures)
			exit(1);
	}
}

#define CHECK(scenario, predicate, actual, expected) \
	check_value((scenario), (predicate), (int) (actual), (int) (expected))

/* EXTRACTED_FUNCTIONS */

void wpa_supplicant_event(void *ctx, enum wpa_event_type event,
			  union wpa_event_data *data)
{
	struct wpa_supplicant *wpa = ctx;
	(void) data;
	if (event == EVENT_ASSOC_FAILED) {
		++effect.failed_event_calls;
		wpa_supplicant_event_assoc_failed(wpa);
	} else
		++effect.unexpected_event_calls;
}

static int direct_fullmac_match(unsigned int *profile, unsigned int *pwe)
{
	return wpa_supplicant_fullmac_config_match(&wpa_s, &ssid,
		&mock.capa, &bss.security_summary, profile, pwe);
}

static void expect_match(const char *scenario, int expected,
			unsigned int expected_profile,
			unsigned int expected_pwe)
{
	unsigned int profile = 0xaaaaaaaaU, pwe = 0xbbbbbbbbU;
	int result = direct_fullmac_match(&profile, &pwe);
	CHECK(scenario, "config match result", result, expected);
	CHECK(scenario, "selected profile", profile,
	      expected ? expected_profile : 0);
	CHECK(scenario, "selected PWE", pwe, expected ? expected_pwe : 0);
}

static void test_match_and_policy(void)
{
	unsigned int profile, pwe;
	int result;

	init_fixture();
	CHECK("exact SAE summary", "pure predicate accepts exact RSN/SAE/CCMP",
	      wpa_security_summary_sae(&bss.security_summary), 1);
	CHECK("FullMAC without FOURWAY", "cached FOURWAY flag absent",
	      (wpa_s.drv_flags & WPA_DRIVER_FLAGS_4WAY_HANDSHAKE) != 0, 0);
	CHECK("FullMAC without FOURWAY", "capability FOURWAY flag absent",
	      (mock.capa.flags & WPA_DRIVER_FLAGS_4WAY_HANDSHAKE) != 0, 0);
	expect_match("FullMAC without FOURWAY", 1,
		     WPA_FULLMAC_PROFILE_SAE_CCMP_PMF,
		     WPA_FULLMAC_SAE_PWE_H2E);

	mock.capa.fullmac_sae_pwe_modes = WPA_FULLMAC_SAE_PWE_HNP;
	expect_match("single HNP mode", 1, WPA_FULLMAC_PROFILE_SAE_CCMP_PMF,
		     WPA_FULLMAC_SAE_PWE_HNP);
	mock.capa.fullmac_sae_pwe_modes = WPA_FULLMAC_SAE_PWE_BOTH;
	expect_match("single BOTH mode", 1, WPA_FULLMAC_PROFILE_SAE_CCMP_PMF,
		     WPA_FULLMAC_SAE_PWE_BOTH);
	mock.capa.fullmac_sae_pwe_modes = WPA_FULLMAC_SAE_PWE_HNP |
		WPA_FULLMAC_SAE_PWE_H2E;
	expect_match("one selected mode from multiple advertised", 1,
		     WPA_FULLMAC_PROFILE_SAE_CCMP_PMF,
		     WPA_FULLMAC_SAE_PWE_H2E);
	mock.capa.fullmac_profiles |= WPA_FULLMAC_PROFILE_WPA2_CCMP;
	expect_match("SAE profile among exact capabilities", 1,
		     WPA_FULLMAC_PROFILE_SAE_CCMP_PMF,
		     WPA_FULLMAC_SAE_PWE_H2E);

	init_fixture();
	mock.capa.fullmac_sae_pwe_modes = 0;
	expect_match("missing PWE mode", 0, 0, 0);
	init_fixture();
	mock.capa.fullmac_profiles = WPA_FULLMAC_PROFILE_WPA2_CCMP;
	mock.capa.fullmac_sae_pwe_modes = 0;
	expect_match("SAE profile not advertised", 0, 0, 0);
	init_fixture();
	mock.capa.fullmac_profiles |= 0x80000000U;
	expect_match("unknown profile bit", 0, 0, 0);
	init_fixture();
	mock.capa.fullmac_sae_pwe_modes |= 0x80000000U;
	expect_match("unknown PWE bit", 0, 0, 0);
	init_fixture();
	mock.capa.fullmac_profiles = WPA_FULLMAC_PROFILE_WPA2_CCMP;
	expect_match("PWE/profile capability incoherence", 0, 0, 0);

	init_fixture();
	mock.capa.fullmac_diagnostic_risk = 1;
	expect_match("diagnostic risk requires explicit opt-in", 0, 0, 0);
	ssid.fullmac_allow_diagnostic = 1;
	expect_match("diagnostic risk explicit opt-in", 1,
		     WPA_FULLMAC_PROFILE_SAE_CCMP_PMF,
		     WPA_FULLMAC_SAE_PWE_H2E);
	ssid.fullmac_allow_diagnostic = 2;
	expect_match("diagnostic opt-in must be boolean", 0, 0, 0);
	ssid.fullmac_allow_diagnostic = 1;
	mock.capa.fullmac_diagnostic_risk = 2;
	expect_match("invalid diagnostic risk value", 0, 0, 0);

	init_fixture();
	mock.capa.flags &= ~WPA_DRIVER_FLAGS_CANCEL_PENDING;
	expect_match("missing CANCEL_PENDING capability", 0, 0, 0);
	init_fixture();
	mock.capa.flags &= ~WPA_DRIVER_FLAGS_FULLMAC_ASSOCIATION;
	expect_match("missing FullMAC capability", 0, 0, 0);
	init_fixture();
	mock.capa.flags &= ~WPA_DRIVER_FLAGS_SECURITY_SUMMARY;
	expect_match("missing summary capability", 0, 0, 0);
	init_fixture();
	wpa_s.drv_flags &= ~WPA_DRIVER_FLAGS_CANCEL_PENDING;
	expect_match("missing cached CANCEL_PENDING capability", 0, 0, 0);
	init_fixture();
	wpa_s.drv_flags |= WPA_DRIVER_FLAGS_SME;
	expect_match("cached SME path refused", 0, 0, 0);
	init_fixture();
	mock.capa.flags |= WPA_DRIVER_FLAGS_USER_SPACE_MLME;
	expect_match("fresh user-space MLME path refused", 0, 0, 0);
	init_fixture();
	mock.capa.flags |= WPA_DRIVER_FLAGS_SME;
	expect_match("fresh SME path refused", 0, 0, 0);

	init_fixture();
	bss.security_summary.flags |= 0x08;
	CHECK("SAE summary rejects unknown flags", "predicate false",
	      wpa_security_summary_sae(&bss.security_summary), 0);
	expect_match("SAE summary rejects unknown flags", 0, 0, 0);
	init_fixture();
	bss.security_summary.proto |= WPA_PROTO_WPA;
	expect_match("SAE summary rejects mixed protocol", 0, 0, 0);
	init_fixture();
	bss.security_summary.key_mgmt |= WPA_KEY_MGMT_PSK;
	expect_match("transition AP selects SAE-only profile", 1,
		     WPA_FULLMAC_PROFILE_SAE_CCMP_PMF,
		     WPA_FULLMAC_SAE_PWE_H2E);
	ssid.key_mgmt |= WPA_KEY_MGMT_PSK;
	expect_match("transition AP cannot enable SAE/PSK fallback", 0, 0, 0);
	init_fixture();
	bss.security_summary.key_mgmt = WPA_KEY_MGMT_PSK;
	expect_match("SAE config cannot select PSK-only AP", 0, 0, 0);
	init_fixture();
	bss.security_summary.pairwise_cipher |= WPA_CIPHER_TKIP;
	expect_match("SAE summary rejects mixed pairwise cipher", 0, 0, 0);
	init_fixture();
	bss.security_summary.group_cipher = WPA_CIPHER_TKIP;
	expect_match("SAE summary rejects wrong group cipher", 0, 0, 0);
	init_fixture();
	bss.security_summary.flags &= ~WPA_SECURITY_SUMMARY_KNOWN;
	expect_match("SAE summary rejects unknown scan identity", 0, 0, 0);
	init_fixture();
	bss.security_summary.flags &= ~WPA_SECURITY_SUMMARY_PRIVACY;
	expect_match("SAE summary requires privacy flag", 0, 0, 0);

	init_fixture();
	ssid.key_mgmt |= WPA_KEY_MGMT_PSK;
	expect_match("mixed SAE/PSK config refused", 0, 0, 0);
	init_fixture();
	ssid.passphrase = (char *) "not-an-SAE-secret";
	expect_match("PSK passphrase cannot substitute for SAE password", 0, 0, 0);
	init_fixture();
	ssid.psk_set = 1;
	expect_match("PSK key cannot substitute for SAE password", 0, 0, 0);
	init_fixture();
	ssid.sae_password = NULL;
	expect_match("missing SAE password", 0, 0, 0);
	init_fixture();
	ssid.sae_password_len = 0;
	expect_match("empty SAE password", 0, 0, 0);
	init_fixture();
	ssid.sae_password_len = 64;
	expect_match("overlong SAE password", 0, 0, 0);
	init_fixture();
	{
		static u8 embedded_nul[] = { 's', 'a', 'e', 0, 'x' };
		ssid.sae_password = embedded_nul;
		ssid.sae_password_len = sizeof(embedded_nul);
		expect_match("SAE password with embedded NUL", 0, 0, 0);
	}
	init_fixture();
	ssid.key_mgmt = WPA_KEY_MGMT_PSK;
	expect_match("PSK does not match SAE summary", 0, 0, 0);

	init_fixture();
	ssid.disabled = 1;
	expect_match("disabled profile", 0, 0, 0);
	init_fixture();
	ssid.mode = WPAS_MODE_IBSS;
	expect_match("non-infrastructure profile", 0, 0, 0);
	init_fixture();
	ssid.mixed_cell = 1;
	expect_match("mixed cell profile", 0, 0, 0);
	init_fixture();
	ssid.auth_alg = WPA_AUTH_ALG_SHARED;
	expect_match("unsupported authentication algorithm", 0, 0, 0);
	init_fixture();
	ssid.proto = WPA_PROTO_WPA;
	expect_match("non-RSN profile", 0, 0, 0);
	init_fixture();
	ssid.pairwise_cipher = WPA_CIPHER_TKIP;
	expect_match("non-CCMP pairwise profile", 0, 0, 0);
	init_fixture();
	ssid.group_cipher = WPA_CIPHER_TKIP;
	expect_match("non-CCMP group profile", 0, 0, 0);
	init_fixture();
	ssid.wep_key_len[0] = 1;
	expect_match("WEP key in SAE config", 0, 0, 0);
	init_fixture();
	ssid.ssid = NULL;
	expect_match("null SSID", 0, 0, 0);
	init_fixture();
	ssid.ssid_len = 0;
	expect_match("empty SSID", 0, 0, 0);
	init_fixture();
	{
		static u8 too_long[33] = "12345678901234567890123456789012";
		ssid.ssid = too_long;
		ssid.ssid_len = sizeof(too_long);
		expect_match("SSID over 32 bytes", 0, 0, 0);
	}
	init_fixture();
	{
		static u8 nul_ssid[] = { 'a', 'p', 0, 'x' };
		ssid.ssid = nul_ssid;
		ssid.ssid_len = sizeof(nul_ssid);
		expect_match("SSID with embedded NUL", 0, 0, 0);
	}

	/* Keep local variable reads explicit so -Werror catches accidental test
	 * omissions as the production code evolves. */
	result = direct_fullmac_match(&profile, &pwe);
	CHECK("final negative state", "result rejected", result, 0);
}

static void test_typed_associate_and_ack_boundary(void)
{
	int result;

	init_fixture();
	wpa_s.pending_eapol_rx = (struct wpabuf *) (uintptr_t) 0x303;
	result = wpa_supplicant_associate_fullmac(&wpa_s, &bss, &ssid);
	CHECK("SAE typed association", "association accepted", result, 0);
	CHECK("SAE typed association", "one driver associate call",
	      mock.associate_calls, 1);
	CHECK("SAE typed association", "one callback record", observed.calls, 1);
	CHECK("SAE typed association", "selected SAE FullMAC profile",
	      observed.profile, WPA_FULLMAC_PROFILE_SAE_CCMP_PMF);
	CHECK("SAE typed association", "selected one PWE method",
	      observed.pwe, WPA_FULLMAC_SAE_PWE_H2E);
	CHECK("SAE typed association", "driver suite is SAE",
	      observed.key_mgmt, KEY_MGMT_SAE);
	CHECK("SAE typed association", "pairwise CCMP",
	      observed.pairwise, CIPHER_CCMP);
	CHECK("SAE typed association", "group CCMP",
	      observed.group, CIPHER_CCMP);
	CHECK("SAE typed association", "typed SAE password pointer",
	      observed.sae_password == ssid.sae_password, 1);
	CHECK("SAE typed association", "typed SAE password length",
	      observed.sae_password_len, sizeof(test_sae_password) - 1);
	CHECK("SAE typed association", "PSK passphrase field empty",
	      observed.passphrase == NULL, 1);
	CHECK("SAE typed association", "PSK field empty",
	      observed.psk == NULL, 1);
	CHECK("SAE typed association", "synthetic WPA/RSN IE absent",
	      observed.wpa_ie == NULL, 1);
	CHECK("SAE typed association", "IE length zero", observed.wpa_ie_len, 0);
	CHECK("SAE typed association", "SSID supplied exactly",
	      observed.ssid == bss.ssid && observed.ssid_len == bss.ssid_len, 1);
	CHECK("SAE typed association", "BSSID supplied exactly",
	      observed.bssid == bss.bssid, 1);
	CHECK("SAE typed association", "infra mode and open auth",
	      observed.mode == IEEE80211_MODE_INFRA &&
	      observed.auth_alg == WPA_AUTH_ALG_OPEN, 1);
	CHECK("SAE typed association", "selected AP frequency",
	      observed.freq, bss.freq);
	CHECK("SAE typed association", "drop unencrypted requested",
	      observed.drop_unencrypted, 1);
	CHECK("SAE typed association", "stored profile remains SAE",
	      wpa_s.fullmac_profile, WPA_FULLMAC_PROFILE_SAE_CCMP_PMF);
	CHECK("SAE typed association", "state remains associating after ACK",
	      wpa_s.wpa_state, WPA_ASSOCIATING);
	CHECK("SAE typed association", "ACK did not publish event",
	      effect.failed_event_calls + effect.unexpected_event_calls, 0);
	CHECK("SAE typed association", "20-second timeout armed",
	      effect.timeout_request_calls, 1);
	CHECK("SAE typed association", "timeout duration seconds",
	      effect.auth_timeout_sec, 20);
	CHECK("SAE typed association", "timeout duration microseconds",
	      effect.auth_timeout_usec, 0);
	CHECK("SAE typed association", "no EAPOL authorization at ACK",
	      effect.port_enabled_true_calls + effect.port_valid_true_calls +
	      effect.eap_success_true_calls, 0);
	CHECK("SAE typed association", "pending unauthenticated EAPOL discarded",
	      wpa_s.pending_eapol_rx == NULL && effect.wpabuf_free_calls == 1, 1);
	CHECK("SAE typed association", "WPA IE cleared, none installed",
	      effect.sm_assoc_ie_clear_calls == 1 &&
	      effect.sm_ap_wpa_ie_clear_calls == 1 &&
	      effect.sm_ap_rsn_ie_clear_calls == 1, 1);
	CHECK("SAE typed association", "software RSN configuration cleared",
	      effect.sm_config_clear_calls == 1 &&
	      effect.eapol_config_clear_calls == 1, 1);
	CHECK("SAE typed association", "no management cipher fabricated",
	      wpa_s.mgmt_group_cipher, 0);
}

static void reject_associate_without_dispatch(const char *scenario)
{
	int result = wpa_supplicant_associate_fullmac(&wpa_s, &bss, &ssid);
	CHECK(scenario, "invalid association refused", result, -1);
	CHECK(scenario, "no driver associate I/O", mock.associate_calls, 0);
	CHECK(scenario, "no auth timeout", effect.timeout_request_calls, 0);
	CHECK(scenario, "no failure event for preflight rejection",
	      effect.failed_event_calls, 0);
	CHECK(scenario, "state not advanced", wpa_s.wpa_state, WPA_DISCONNECTED);
}

static void test_preflight_refusals_do_not_dispatch(void)
{
	init_fixture();
	CHECK("missing BSS", "no AP passed", 1, 1);
	CHECK("missing BSS", "helper refuses", 
	      wpa_supplicant_associate_fullmac(&wpa_s, NULL, &ssid), -1);
	CHECK("missing BSS", "no associate call", mock.associate_calls, 0);

	init_fixture();
	os_memset(bss.bssid, 0, ETH_ALEN);
	reject_associate_without_dispatch("zero BSSID");
	init_fixture();
	bss.bssid[0] = 0x01;
	reject_associate_without_dispatch("multicast BSSID");
	init_fixture();
	bss.ssid[0] ^= 1;
	reject_associate_without_dispatch("selected SSID mismatch");
	init_fixture();
	ssid.bssid_set = 1;
	ssid.bssid[0] = 0x02;
	ssid.bssid[5] = 0x99;
	reject_associate_without_dispatch("configured BSSID mismatch");
	init_fixture();
	bss.ssid_len = 33;
	ssid.ssid_len = 33;
	reject_associate_without_dispatch("oversize selected SSID");
	init_fixture();
	mock_vendor_ie = 1;
	reject_associate_without_dispatch("genuine WPA vendor IE refused");
	init_fixture();
	mock_rsn_ie = 1;
	reject_associate_without_dispatch("genuine RSN IE refused");
	init_fixture();
	mock.get_capa_result = -1;
	reject_associate_without_dispatch("capability query failure");
	init_fixture();
	mock.capa.fullmac_sae_pwe_modes = 0;
	reject_associate_without_dispatch("unsupported SAE PWE");
	init_fixture();
	bss.security_summary.key_mgmt = WPA_KEY_MGMT_PSK;
	reject_associate_without_dispatch("non-SAE summary");
	init_fixture();
	ssid.sae_password_len = 64;
	reject_associate_without_dispatch("invalid SAE password length");
	init_fixture();
	ssid.psk_set = 1;
	reject_associate_without_dispatch("legacy PSK credential beside SAE");
}

static void test_driver_error_runs_real_failure_cleanup(void)
{
	int result;

	init_fixture();
	mock.associate_result = -1;
	wpa_s.pending_eapol_rx = (struct wpabuf *) (uintptr_t) 0x404;
	result = wpa_supplicant_associate_fullmac(&wpa_s, &bss, &ssid);
	CHECK("negative driver ACK", "association returns failure", result, -1);
	CHECK("negative driver ACK", "one associate attempt", mock.associate_calls, 1);
	CHECK("negative driver ACK", "EVENT_ASSOC_FAILED dispatched",
	      effect.failed_event_calls, 1);
	CHECK("negative driver ACK", "failure handler clears FullMAC profile",
	      wpa_s.fullmac_profile, 0);
	CHECK("negative driver ACK", "failure handler clears FullMAC config pointer",
	      wpa_s.fullmac_ssid == NULL, 1);
	CHECK("negative driver ACK", "failure state disconnected",
	      wpa_s.wpa_state, WPA_DISCONNECTED);
	CHECK("negative driver ACK", "does not become completed",
	      wpa_s.wpa_state == WPA_COMPLETED, 0);
	CHECK("negative driver ACK", "no timeout armed after failure",
	      effect.timeout_request_calls, 0);
	CHECK("negative driver ACK", "cause-free auth timeout canceled",
	      effect.timeout_cancel_no_blacklist_calls >= 2, 1);
	CHECK("negative driver ACK", "no EAPOL authorization",
	      effect.port_valid_true_calls + effect.eap_success_true_calls, 0);
	CHECK("negative driver ACK", "pending EAPOL released",
	      wpa_s.pending_eapol_rx == NULL && effect.wpabuf_free_calls >= 1, 1);
	CHECK("negative driver ACK", "retry scan scheduled after delay",
	      effect.request_scan_calls, 1);
	CHECK("negative driver ACK", "retry delay seconds",
	      effect.request_scan_sec, 5);
	CHECK("negative driver ACK", "no unexpected events",
	      effect.unexpected_event_calls, 0);
}

static void prepare_pending_event(union wpa_event_data *data)
{
	init_fixture();
	wpa_s.current_ssid = &ssid;
	wpa_s.current_bss = &bss;
	wpa_s.fullmac_ssid = &ssid;
	wpa_s.fullmac_profile = WPA_FULLMAC_PROFILE_SAE_CCMP_PMF;
	wpa_s.fullmac_pwe = WPA_FULLMAC_SAE_PWE_H2E;
	wpa_s.fullmac_summary = bss.security_summary;
	wpa_s.fullmac_ssid_len = ssid.ssid_len;
	os_memcpy(wpa_s.fullmac_ssid_bytes, ssid.ssid, ssid.ssid_len);
	os_memcpy(wpa_s.fullmac_bssid, bss.bssid, ETH_ALEN);
	os_memcpy(wpa_s.pending_bssid, bss.bssid, ETH_ALEN);
	wpa_s.key_mgmt = WPA_KEY_MGMT_SAE;
	wpa_s.wpa_state = WPA_ASSOCIATING;
	wpa_s.pending_eapol_rx = (struct wpabuf *) (uintptr_t) 0x505;
	os_memset(data, 0, sizeof(*data));
	data->assoc_info.fullmac_profile = WPA_FULLMAC_PROFILE_SAE_CCMP_PMF;
	data->assoc_info.fullmac_cookie = 0x65;
}

static void expect_event_rejected(const char *scenario,
				  union wpa_event_data *data)
{
	enum wpa_states initial_state = wpa_s.wpa_state;
	int result = wpa_supplicant_event_fullmac_assoc(&wpa_s, data);
	CHECK(scenario, "invalid completion refused", result, -1);
	CHECK(scenario, "state unchanged", wpa_s.wpa_state, initial_state);
	CHECK(scenario, "no BSSID publication", is_zero_ether_addr(wpa_s.bssid), 1);
	CHECK(scenario, "no EAPOL port authorization",
	      effect.port_enabled_true_calls + effect.port_valid_true_calls +
	      effect.eap_success_true_calls, 0);
	CHECK(scenario, "no timeout cancellation on rejected event",
	      effect.timeout_cancel_calls, 0);
	CHECK(scenario, "pending EAPOL remains untouched until real completion",
	      wpa_s.pending_eapol_rx != NULL && effect.wpabuf_free_calls == 0, 1);
}

static void test_event_completion_and_rejections(void)
{
	union wpa_event_data data;
	struct wpa_ssid other;
	int result;

	prepare_pending_event(&data);
	result = wpa_supplicant_event_fullmac_assoc(&wpa_s, &data);
	CHECK("valid typed CONNECT", "event accepted", result, 0);
	CHECK("valid typed CONNECT", "completion state",
	      wpa_s.wpa_state, WPA_COMPLETED);
	CHECK("valid typed CONNECT", "BSSID recorded",
	      os_memcmp(wpa_s.bssid, test_bssid, ETH_ALEN) == 0, 1);
	CHECK("valid typed CONNECT", "pending BSSID cleared",
	      is_zero_ether_addr(wpa_s.pending_bssid), 1);
	CHECK("valid typed CONNECT", "auth timeout canceled",
	      effect.timeout_cancel_calls, 1);
	CHECK("valid typed CONNECT", "EAPOL authorization occurs on event",
	      effect.port_enabled_true_calls == 1 &&
	      effect.port_valid_true_calls == 1 &&
	      effect.eap_success_true_calls == 1, 1);
	CHECK("valid typed CONNECT", "pre-association EAPOL discarded",
	      wpa_s.pending_eapol_rx == NULL && effect.wpabuf_free_calls == 1, 1);

	prepare_pending_event(&data);
	data.assoc_info.fullmac_cookie = 0;
	expect_event_rejected("missing event cookie", &data);
	prepare_pending_event(&data);
	data.assoc_info.fullmac_profile = 0;
	expect_event_rejected("missing event profile", &data);
	prepare_pending_event(&data);
	data.assoc_info.fullmac_profile = WPA_FULLMAC_PROFILE_WPA2_CCMP;
	expect_event_rejected("wrong event profile", &data);
	prepare_pending_event(&data);
	wpa_s.fullmac_profile = 0;
	expect_event_rejected("no active FullMAC attempt", &data);
	prepare_pending_event(&data);
	wpa_s.wpa_state = WPA_COMPLETED;
	expect_event_rejected("late event in completed state", &data);
	prepare_pending_event(&data);
	wpa_s.reassociate = 1;
	expect_event_rejected("event after cancellation request", &data);
	prepare_pending_event(&data);
	wpa_s.current_ssid = NULL;
	expect_event_rejected("selected configuration removed", &data);
	prepare_pending_event(&data);
	os_memcpy(&other, &ssid, sizeof(other));
	wpa_s.current_ssid = &other;
	expect_event_rejected("selected configuration identity changed", &data);
	prepare_pending_event(&data);
	ssid.disabled = 1;
	expect_event_rejected("configuration disabled before event", &data);
	prepare_pending_event(&data);
	wpa_s.disconnected = 1;
	expect_event_rejected("interface disconnected before event", &data);
	prepare_pending_event(&data);
	wpa_s.interface_removed = 1;
	expect_event_rejected("interface removed before event", &data);
	prepare_pending_event(&data);
	wpa_s.key_mgmt = WPA_KEY_MGMT_PSK;
	expect_event_rejected("active key management changed", &data);
	prepare_pending_event(&data);
	ssid.ssid_len--;
	expect_event_rejected("SSID length changed", &data);
	prepare_pending_event(&data);
	ssid.ssid[0] ^= 1;
	expect_event_rejected("SSID bytes changed", &data);
	prepare_pending_event(&data);
	ssid.bssid_set = 1;
	ssid.bssid[0] = 0x02;
	ssid.bssid[5] = 0x99;
	expect_event_rejected("configured BSSID changed", &data);
	prepare_pending_event(&data);
	os_memset(wpa_s.fullmac_bssid, 0, ETH_ALEN);
	expect_event_rejected("retained BSSID zero", &data);
	prepare_pending_event(&data);
	wpa_s.fullmac_bssid[0] = 0x01;
	expect_event_rejected("retained BSSID multicast", &data);
	prepare_pending_event(&data);
	data.assoc_info.req_ies = fake_ie;
	expect_event_rejected("request IE on typed event", &data);
	prepare_pending_event(&data);
	data.assoc_info.req_ies_len = 1;
	expect_event_rejected("request IE length on typed event", &data);
	prepare_pending_event(&data);
	data.assoc_info.resp_ies = fake_ie;
	expect_event_rejected("response IE on typed event", &data);
	prepare_pending_event(&data);
	data.assoc_info.resp_ies_len = 1;
	expect_event_rejected("response IE length on typed event", &data);
	prepare_pending_event(&data);
	data.assoc_info.beacon_ies = fake_ie;
	expect_event_rejected("beacon IE on typed event", &data);
	prepare_pending_event(&data);
	data.assoc_info.beacon_ies_len = 1;
	expect_event_rejected("beacon IE length on typed event", &data);
	prepare_pending_event(&data);
	mock.get_capa_result = -1;
	expect_event_rejected("capability query failed during completion", &data);
	prepare_pending_event(&data);
	mock.capa.flags |= WPA_DRIVER_FLAGS_SME;
	expect_event_rejected("capability changed to SME", &data);
	prepare_pending_event(&data);
	mock.capa.fullmac_sae_pwe_modes = WPA_FULLMAC_SAE_PWE_HNP;
	expect_event_rejected("capability PWE changed during attempt", &data);
	prepare_pending_event(&data);
	wpa_s.fullmac_summary.key_mgmt = WPA_KEY_MGMT_PSK;
	expect_event_rejected("retained security summary changed", &data);
	prepare_pending_event(&data);
	mock.driver_bssid[5] ^= 1;
	expect_event_rejected("driver reports a different BSSID", &data);
	prepare_pending_event(&data);
	os_memset(mock.driver_bssid, 0, ETH_ALEN);
	expect_event_rejected("driver reports a zero BSSID", &data);
	prepare_pending_event(&data);
	mock.get_bssid_result = -1;
	expect_event_rejected("driver BSSID query failure", &data);
	prepare_pending_event(&data);
	mock.get_bssid_result = 1; /* SANA-II uses positive for no current BSSID. */
	mock.get_bssid_leave_output_untouched = 1;
	expect_event_rejected("positive no-BSSID result leaves output untouched",
			      &data);

	prepare_pending_event(&data);
	expect_event_rejected("null event data", NULL);
}

static void test_disassociation_rejects_late_connect(void)
{
	union wpa_event_data data;
	int result;

	prepare_pending_event(&data);
	wpa_supplicant_mark_disassoc(&wpa_s);
	CHECK("disassociation retires FullMAC state", "profile cleared",
	      wpa_s.fullmac_profile, 0);
	CHECK("disassociation retires FullMAC state", "PWE cleared",
	      wpa_s.fullmac_pwe, 0);
	CHECK("disassociation retires FullMAC state", "config pointer cleared",
	      wpa_s.fullmac_ssid == NULL, 1);
	CHECK("disassociation retires FullMAC state", "summary cleared",
	      wpa_s.fullmac_summary.flags, 0);
	CHECK("disassociation retires FullMAC state", "SSID identity cleared",
	      wpa_s.fullmac_ssid_len, 0);
	CHECK("disassociation retires FullMAC state", "BSSID identity cleared",
	      is_zero_ether_addr(wpa_s.fullmac_bssid), 1);
	CHECK("disassociation retires FullMAC state", "state disconnected",
	      wpa_s.wpa_state, WPA_DISCONNECTED);
	CHECK("disassociation retires FullMAC state", "EAPOL authorization revoked",
	      effect.port_enabled_false_calls > 0 &&
	      effect.port_valid_false_calls > 0 &&
	      effect.eap_success_false_calls > 0, 1);
	result = wpa_supplicant_event_fullmac_assoc(&wpa_s, &data);
	CHECK("late CONNECT after disassociation", "stale completion refused",
	      result, -1);
	CHECK("late CONNECT after disassociation", "not completed",
	      wpa_s.wpa_state == WPA_COMPLETED, 0);
	CHECK("late CONNECT after disassociation", "no positive authorization",
	      effect.port_valid_true_calls + effect.eap_success_true_calls, 0);
}

int main(void)
{
	test_match_and_policy();
	test_typed_associate_and_ack_boundary();
	test_preflight_refusals_do_not_dispatch();
	test_driver_error_runs_real_failure_cleanup();
	test_event_completion_and_rejections();
	test_disassociation_rejects_late_connect();
	check_value("__report__", "summary", 0, 0);
	return 0;
}
'''


def extract_function(source: str, name: str, return_type: str) -> str:
	pattern = re.compile(
		r"(?m)^(?:static\s+)?" + re.escape(return_type) + r"\s+" +
		re.escape(name) + r"\s*\("
	)
	matches = list(pattern.finditer(source))
	if len(matches) != 1:
		raise SystemExit(
			f"error: expected one production definition for {name}, found {len(matches)}"
		)
	start = matches[0].start()
	state = "code"
	depth = 0
	opened = False
	index = start
	while index < len(source):
		char = source[index]
		following = source[index + 1] if index + 1 < len(source) else ""
		if state == "code":
			if char == "/" and following == "*":
				state = "comment"
				index += 2
				continue
			if char == "/" and following == "/":
				state = "line-comment"
				index += 2
				continue
			if char == '"':
				state = "string"
			elif char == "'":
				state = "character"
			elif char == "{":
				opened = True
				depth += 1
			elif char == "}":
				depth -= 1
				if opened and depth == 0:
					return source[start : index + 1]
		elif state == "comment":
			if char == "*" and following == "/":
				state = "code"
				index += 2
				continue
		elif state == "line-comment":
			if char == "\n":
				state = "code"
		elif state in ("string", "character"):
			if char == "\\":
				index += 2
				continue
			if (state == "string" and char == '"') or (
				state == "character" and char == "'"
			):
				state = "code"
		index += 1
	raise SystemExit(f"error: incomplete production function {name}")


def extract_body(source: str, name: str, return_type: str) -> str:
	return extract_function(source, name, return_type)


def source_guards() -> None:
	supplicant = SUPPLICANT.read_text(encoding="utf-8")
	events = EVENTS.read_text(encoding="utf-8")
	associate = extract_body(supplicant, "wpa_supplicant_associate", "void")
	rx_eapol = extract_body(supplicant, "wpa_supplicant_rx_eapol", "void")
	event_assoc = extract_body(events, "wpa_supplicant_event_assoc", "void")
	reload_config = extract_body(supplicant, "wpa_supplicant_reload_configuration", "int")
	retire = reload_config.find("if (wpa_s->fullmac_profile)")
	cancel = reload_config.find("wpa_supplicant_disassociate", retire)
	free_config = reload_config.find("wpa_config_free", cancel)
	if min(retire, cancel, free_config) < 0 or not retire < cancel < free_config:
		raise SystemExit("error: configuration reload must retire typed state before freeing networks")

	sae_route = associate.find("(ssid->key_mgmt & WPA_KEY_MGMT_SAE)")
	helper_call = associate.find("wpa_supplicant_associate_fullmac(wpa_s, bss, ssid)")
	helper_return = associate.find("return;", helper_call)
	legacy_path = associate.find("wpa_supplicant_set_suites", helper_return)
	if min(sae_route, helper_call, helper_return) < 0 or not (
		sae_route < helper_call < helper_return < legacy_path
	):
		raise SystemExit(
			"error: SAE association must take the typed helper and return before legacy suite setup"
		)
	if "ssid->key_mgmt == WPA_KEY_MGMT_PSK" not in associate[sae_route:helper_call]:
		raise SystemExit("error: typed WPA2 dispatch guard is missing")

	eapol_guard = rx_eapol.find(
		"if (wpa_s->fullmac_profile || wpa_s->key_mgmt == WPA_KEY_MGMT_SAE)"
	)
	guard_return = rx_eapol.find("return;", eapol_guard)
	first_processing = rx_eapol.find("wpa_printf", guard_return)
	if min(eapol_guard, guard_return, first_processing) < 0 or not (
		eapol_guard < guard_return < first_processing
	):
		raise SystemExit("error: SAE EAPOL early-ignore guard is missing or late")

	typed_dispatch = event_assoc.find("wpa_supplicant_event_fullmac_assoc(wpa_s, data)")
	typed_return = event_assoc.find("return;", typed_dispatch)
	generic_assoc = event_assoc.find("ft_completed =", typed_return)
	if min(typed_dispatch, typed_return, generic_assoc) < 0 or not (
		typed_dispatch < typed_return < generic_assoc
	):
		raise SystemExit(
			"error: typed association events must return before generic IE/handshake processing"
		)


def make_source() -> str:
	source_guards()
	supplicant = SUPPLICANT.read_text(encoding="utf-8")
	events = EVENTS.read_text(encoding="utf-8")
	blocks = [
		extract_function(supplicant, "key_mgmt2driver", "enum wpa_key_mgmt"),
		extract_function(supplicant, "wpa_supplicant_fullmac_config_match", "int"),
		extract_function(events, "wpa_supplicant_mark_disassoc", "void"),
		extract_function(events, "wpa_supplicant_event_assoc_failed", "void"),
		extract_function(supplicant, "wpa_supplicant_associate_fullmac", "int"),
		extract_function(events, "wpa_supplicant_event_fullmac_assoc", "int"),
	]
	result = TEST_SOURCE.replace(
		"/* EXTRACTED_FUNCTIONS */", "\n\n".join(blocks)
	)
	if "/* EXTRACTED_FUNCTIONS */" in result:
		raise SystemExit("error: failed to insert extracted production functions")
	return result


def main() -> int:
	compiler = shlex.split(os.environ.get("CC", "cc"))
	source = make_source()
	with tempfile.TemporaryDirectory(prefix="wireless-fullmac-caller-") as temp:
		temp_dir = Path(temp)
		c_file = temp_dir / "test_wireless_fullmac_caller.c"
		c_file.write_text(source, encoding="utf-8")
		for optimization in (0, 2):
			binary = temp_dir / f"test-wireless-fullmac-caller-O{optimization}"
			command = compiler + [
				"-std=c99",
				f"-O{optimization}",
				"-g",
				"-Wall",
				"-Wextra",
				"-Werror",
				"-fsanitize=address,undefined",
				"-fno-omit-frame-pointer",
				"-I",
				str(WM_SRC),
				"-I",
				str(WM_ROOT),
				"-I",
				str(ROOT / "compiler/include"),
				str(c_file),
				"-o",
				str(binary),
			]
			print(f"== O{optimization} ASan/UBSan ==", flush=True)
			subprocess.run(command, check=True)
			run_env = os.environ.copy()
			run_env["ASAN_OPTIONS"] = (
				"halt_on_error=1:detect_stack_use_after_return=1"
			)
			run_env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
			subprocess.run([str(binary)], env=run_env, check=True)
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
