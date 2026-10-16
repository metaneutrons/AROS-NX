#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIT(n) (1U << (n))
#include "common/defs.h"
#include "common/security_summary.h"

#include "sana2-assoc-failed-state-constants.inc"

#define ETH_ALEN 6
#define os_memset memset
#define MSG_DEBUG 0
typedef unsigned char u8;

struct wpa_ssid { int key_mgmt; };
static struct wpa_ssid fixture_ssid;
static void wpabuf_free(void *buffer) { free(buffer); }

struct wpa_supplicant {
	enum wpa_states wpa_state;
	u8 bssid[ETH_ALEN];
	u8 pending_bssid[ETH_ALEN];
	void *current_bss;
	struct wpa_ssid *current_ssid;
	void *eapol;
	void *wpa;
	int key_mgmt;
	int drv_flags;
	int ap_ies_from_associnfo;
	int interface_removed;
	int disconnected;
	unsigned int fullmac_profile, fullmac_pwe;
	struct wpa_ssid *fullmac_ssid;
	struct wpa_security_summary fullmac_summary;
	u8 fullmac_bssid[ETH_ALEN], fullmac_ssid_bytes[32];
	size_t fullmac_ssid_len;
	void *pending_eapol_rx;
	struct { long sec, usec; } pending_eapol_rx_time;
	u8 pending_eapol_rx_src[ETH_ALEN];
};

typedef void (*eloop_timeout_handler)(void *, void *);

struct effects {
	unsigned int timer_cancel_calls;
	unsigned int blacklist_delete_calls;
	unsigned int state_change_calls;
	unsigned int bssid_notify_calls;
	unsigned int port_enabled_false_calls;
	unsigned int port_valid_false_calls;
	unsigned int eap_success_false_calls;
	unsigned int wpa_disassoc_calls;
	unsigned int scan_calls;
	unsigned int scan_delay_sec;
	unsigned int scan_delay_usec;
	unsigned int disconnected_log_calls;
	unsigned int timeout_or_reject_log_calls;
	unsigned int driver_disassociate_calls;
	unsigned int driver_deauthenticate_calls;
	unsigned int mlme_disassociate_calls;
	unsigned int mlme_deauthenticate_calls;
	unsigned int clear_keys_calls;
	unsigned int sm_set_config_calls;
	unsigned int eapol_config_calls;
	unsigned int network_changed_calls;
	int last_driver_disassociate_null;
	int last_driver_deauthenticate_null;
	int last_mlme_disassociate_null;
	int last_mlme_deauthenticate_null;
	int last_clear_keys_null;
	int last_clear_keys_addr_zero;
	int last_sm_config_null;
	int last_eapol_config_null;
	int last_disassociate_reason;
	int last_deauthenticate_reason;
	u8 last_blacklist_delete_bssid[ETH_ALEN];
	u8 last_driver_disassociate_addr[ETH_ALEN];
	u8 last_driver_deauthenticate_addr[ETH_ALEN];
	u8 last_mlme_disassociate_addr[ETH_ALEN];
	u8 last_mlme_deauthenticate_addr[ETH_ALEN];
	u8 last_clear_keys_addr[ETH_ALEN];
};

static struct effects effects;
static char diagnostic_log[1024];

static void reset_effects(void)
{
	memset(&effects, 0, sizeof(effects));
	diagnostic_log[0] = '\0';
}

static void record_log(const char *format, va_list ap)
{
	size_t used = strlen(diagnostic_log);
	if (used < sizeof(diagnostic_log) - 1) {
		vsnprintf(diagnostic_log + used, sizeof(diagnostic_log) - used,
			  format, ap);
		strncat(diagnostic_log, "\n",
			sizeof(diagnostic_log) - strlen(diagnostic_log) - 1);
	}
	if (strstr(format, "WPA_EVENT_DISCONNECTED") != NULL)
		effects.disconnected_log_calls++;
	if (strstr(format, "timed out") != NULL ||
	    strstr(format, "rejected") != NULL)
		effects.timeout_or_reject_log_calls++;
}

static void wpa_printf(int level, const char *format, ...)
{
	va_list ap;
	(void) level;
	va_start(ap, format);
	record_log(format, ap);
	va_end(ap);
}

static void wpa_msg(struct wpa_supplicant *wpa_s, int level,
		    const char *format, ...)
{
	va_list ap;
	(void) wpa_s;
	(void) level;
	va_start(ap, format);
	record_log(format, ap);
	va_end(ap);
}

static void wpa_supplicant_timeout(void *eloop_ctx, void *timeout_ctx)
{
	(void) eloop_ctx;
	(void) timeout_ctx;
}

static void eloop_cancel_timeout(eloop_timeout_handler handler,
				 void *eloop_ctx, void *timeout_ctx)
{
	(void) timeout_ctx;
	if (handler == wpa_supplicant_timeout && eloop_ctx != NULL)
		effects.timer_cancel_calls++;
}

static int wpa_blacklist_del(struct wpa_supplicant *wpa_s, const u8 *bssid)
{
	(void) wpa_s;
	effects.blacklist_delete_calls++;
	memcpy(effects.last_blacklist_delete_bssid, bssid, ETH_ALEN);
	return 0;
}

static int is_zero_ether_addr(const u8 *addr)
{
	static const u8 zero[ETH_ALEN] = { 0 };
	return memcmp(addr, zero, ETH_ALEN) == 0;
}

static void wpas_notify_bssid_changed(struct wpa_supplicant *wpa_s)
{
	(void) wpa_s;
	effects.bssid_notify_calls++;
}

static void wpa_supplicant_set_state(struct wpa_supplicant *wpa_s,
				     enum wpa_states state)
{
	if (wpa_s->wpa_state != state)
		effects.state_change_calls++;
	wpa_s->wpa_state = state;
}

static void eapol_sm_notify_portEnabled(void *eapol, Boolean enabled)
{
	(void) eapol;
	if (!enabled)
		effects.port_enabled_false_calls++;
}

static void eapol_sm_notify_portValid(void *eapol, Boolean valid)
{
	(void) eapol;
	if (!valid)
		effects.port_valid_false_calls++;
}

static void eapol_sm_notify_eap_success(void *eapol, Boolean success)
{
	(void) eapol;
	if (!success)
		effects.eap_success_false_calls++;
}

static void wpa_sm_notify_disassoc(void *wpa)
{
	(void) wpa;
	effects.wpa_disassoc_calls++;
}

static void wpa_supplicant_req_scan(struct wpa_supplicant *wpa_s,
				    int sec, int usec)
{
	(void) wpa_s;
	effects.scan_calls++;
	effects.scan_delay_sec = sec;
	effects.scan_delay_usec = usec;
}

static int record_driver_callback(unsigned int *calls, int *addr_is_null,
				  u8 *addr_copy, const u8 *addr, int reason)
{
	(*calls)++;
	*addr_is_null = addr == NULL;
	if (addr)
		memcpy(addr_copy, addr, ETH_ALEN);
	return reason;
}

static int wpa_drv_disassociate(struct wpa_supplicant *wpa_s,
				const u8 *addr, int reason)
{
	(void) wpa_s;
	effects.last_disassociate_reason = record_driver_callback(
		&effects.driver_disassociate_calls,
		&effects.last_driver_disassociate_null,
		effects.last_driver_disassociate_addr, addr, reason);
	return 0;
}

static int wpa_drv_deauthenticate(struct wpa_supplicant *wpa_s,
				  const u8 *addr, int reason)
{
	(void) wpa_s;
	effects.last_deauthenticate_reason = record_driver_callback(
		&effects.driver_deauthenticate_calls,
		&effects.last_driver_deauthenticate_null,
		effects.last_driver_deauthenticate_addr, addr, reason);
	return 0;
}

static int ieee80211_sta_disassociate(struct wpa_supplicant *wpa_s,
				     int reason)
{
	(void) wpa_s;
	effects.last_disassociate_reason = record_driver_callback(
		&effects.mlme_disassociate_calls,
		&effects.last_mlme_disassociate_null,
		effects.last_mlme_disassociate_addr, NULL, reason);
	return 0;
}

static int ieee80211_sta_deauthenticate(struct wpa_supplicant *wpa_s,
					int reason)
{
	(void) wpa_s;
	effects.last_deauthenticate_reason = record_driver_callback(
		&effects.mlme_deauthenticate_calls,
		&effects.last_mlme_deauthenticate_null,
		effects.last_mlme_deauthenticate_addr, NULL, reason);
	return 0;
}

static void wpa_clear_keys(struct wpa_supplicant *wpa_s, const u8 *addr)
{
	(void) wpa_s;
	effects.clear_keys_calls++;
	effects.last_clear_keys_null = addr == NULL;
	if (addr) {
		memcpy(effects.last_clear_keys_addr, addr, ETH_ALEN);
		effects.last_clear_keys_addr_zero = is_zero_ether_addr(addr);
	}
}

static void wpa_sm_set_config(void *wpa, void *config)
{
	(void) wpa;
	effects.sm_set_config_calls++;
	effects.last_sm_config_null = config == NULL;
}

static void eapol_sm_notify_config(void *eapol, void *eap_conf,
				   void *eapol_conf)
{
	(void) eapol;
	effects.eapol_config_calls++;
	effects.last_eapol_config_null = eap_conf == NULL && eapol_conf == NULL;
}

static void wpas_notify_network_changed(struct wpa_supplicant *wpa_s)
{
	(void) wpa_s;
	effects.network_changed_calls++;
}

#include "sana2-assoc-failed-state-functions.inc"

static unsigned int check_count;
static unsigned int failure_count;

#define CHECK(condition) do { \
	check_count++; \
	if (!(condition)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
			#condition); \
		failure_count++; \
	} \
} while (0)

static void fill_address(u8 *addr, u8 first)
{
	addr[0] = first;
	addr[1] = 0x11;
	addr[2] = 0x22;
	addr[3] = 0x33;
	addr[4] = 0x44;
	addr[5] = 0x55;
}

static void init_supplicant(struct wpa_supplicant *wpa_s,
			    enum wpa_states state)
{
	memset(wpa_s, 0, sizeof(*wpa_s));
	wpa_s->wpa_state = state;
	wpa_s->pending_bssid[0] = 0xaa;
	wpa_s->pending_bssid[1] = 0xbb;
	wpa_s->pending_bssid[2] = 0xcc;
	wpa_s->pending_bssid[3] = 0xdd;
	wpa_s->pending_bssid[4] = 0xee;
	wpa_s->pending_bssid[5] = 0xff;
	wpa_s->current_bss = (void *) (uintptr_t) 0x1234;
	wpa_s->current_ssid = &fixture_ssid;
	wpa_s->eapol = (void *) (uintptr_t) 0x2345;
	wpa_s->wpa = (void *) (uintptr_t) 0x3456;
	wpa_s->key_mgmt = WPA_KEY_MGMT_PSK;
	wpa_s->ap_ies_from_associnfo = 1;
}

static void test_active_failure(enum wpa_states state, int expect_bssid_notice)
{
	struct wpa_supplicant wpa_s;
	init_supplicant(&wpa_s, state);
	if (expect_bssid_notice)
		fill_address(wpa_s.bssid, 0x66);
	reset_effects();

	wpa_supplicant_event_assoc_failed(&wpa_s);

	CHECK(wpa_s.wpa_state == WPA_DISCONNECTED);
	CHECK(is_zero_ether_addr(wpa_s.bssid));
	CHECK(is_zero_ether_addr(wpa_s.pending_bssid));
	CHECK(wpa_s.current_bss == NULL);
	CHECK(wpa_s.ap_ies_from_associnfo == 0);
	CHECK(effects.state_change_calls == 1);
	CHECK(effects.bssid_notify_calls == (unsigned int) expect_bssid_notice);
	CHECK(effects.timer_cancel_calls == 1);
	CHECK(effects.wpa_disassoc_calls == 1);
	CHECK(effects.port_enabled_false_calls == 1);
	CHECK(effects.port_valid_false_calls == 1);
	CHECK(effects.eap_success_false_calls == 1);
	CHECK(effects.scan_calls == 1);
	CHECK(effects.scan_delay_sec == 5);
	CHECK(effects.scan_delay_usec == 0);
	CHECK(effects.blacklist_delete_calls == 0);
	CHECK(effects.disconnected_log_calls == 0);
	CHECK(effects.timeout_or_reject_log_calls == 0);
	CHECK(effects.driver_disassociate_calls == 0);
	CHECK(effects.driver_deauthenticate_calls == 0);
	CHECK(effects.mlme_disassociate_calls == 0);
	CHECK(effects.mlme_deauthenticate_calls == 0);
	CHECK(strstr(diagnostic_log, "cause unspecified") != NULL);
}

static void test_ignored_states(void)
{
	int state;
	for (state = WPA_DISCONNECTED; state <= WPA_COMPLETED; state++) {
		struct wpa_supplicant wpa_s;
		if (state == WPA_AUTHENTICATING || state == WPA_ASSOCIATING)
			continue;
		init_supplicant(&wpa_s, (enum wpa_states) state);
		reset_effects();

		wpa_supplicant_event_assoc_failed(&wpa_s);

		CHECK(wpa_s.wpa_state == (enum wpa_states) state);
		CHECK(!is_zero_ether_addr(wpa_s.pending_bssid));
		CHECK(wpa_s.current_bss != NULL);
		CHECK(effects.timer_cancel_calls == 0);
		CHECK(effects.wpa_disassoc_calls == 0);
		CHECK(effects.state_change_calls == 0);
		CHECK(effects.scan_calls == 0);
		CHECK(effects.blacklist_delete_calls == 0);
	}
}

static void test_duplicate_failure(void)
{
	struct wpa_supplicant wpa_s;
	init_supplicant(&wpa_s, WPA_ASSOCIATING);
	reset_effects();

	wpa_supplicant_event_assoc_failed(&wpa_s);
	wpa_supplicant_event_assoc_failed(&wpa_s);

	CHECK(wpa_s.wpa_state == WPA_DISCONNECTED);
	CHECK(effects.timer_cancel_calls == 1);
	CHECK(effects.wpa_disassoc_calls == 1);
	CHECK(effects.state_change_calls == 1);
	CHECK(effects.scan_calls == 1);
	CHECK(effects.blacklist_delete_calls == 0);
}

static void test_retry_suppressed_but_state_cleared(int removed,
						    int disconnected)
{
	struct wpa_supplicant wpa_s;
	init_supplicant(&wpa_s, WPA_AUTHENTICATING);
	wpa_s.interface_removed = removed;
	wpa_s.disconnected = disconnected;
	reset_effects();

	wpa_supplicant_event_assoc_failed(&wpa_s);

	CHECK(wpa_s.wpa_state == WPA_DISCONNECTED);
	CHECK(is_zero_ether_addr(wpa_s.pending_bssid));
	CHECK(wpa_s.current_bss == NULL);
	CHECK(effects.timer_cancel_calls == 1);
	CHECK(effects.wpa_disassoc_calls == 1);
	CHECK(effects.scan_calls == 0);
	CHECK(effects.blacklist_delete_calls == 0);
}

static void test_timer_cancellation_blacklist_semantics(void)
{
	struct wpa_supplicant wpa_s;
	static const u8 expected_bssid[ETH_ALEN] = {
		0x02, 0x10, 0x20, 0x30, 0x40, 0x50
	};
	init_supplicant(&wpa_s, WPA_ASSOCIATING);
	fill_address(wpa_s.bssid, expected_bssid[0]);
	memcpy(wpa_s.bssid, expected_bssid, ETH_ALEN);
	reset_effects();

	wpa_supplicant_cancel_auth_timeout_no_blacklist(&wpa_s);
	CHECK(effects.timer_cancel_calls == 1);
	CHECK(effects.blacklist_delete_calls == 0);

	reset_effects();
	wpa_supplicant_cancel_auth_timeout(&wpa_s);
	CHECK(effects.timer_cancel_calls == 1);
	CHECK(effects.blacklist_delete_calls == 1);
	CHECK(memcmp(effects.last_blacklist_delete_bssid, expected_bssid,
		     ETH_ALEN) == 0);
}

#define TEST_REASON_CODE 17

static void invoke_local_disconnect(struct wpa_supplicant *wpa_s, int deauth)
{
	if (deauth)
		wpa_supplicant_deauthenticate(wpa_s, TEST_REASON_CODE);
	else
		wpa_supplicant_disassociate(wpa_s, TEST_REASON_CODE);
}

static unsigned int local_driver_callback_count(int deauth)
{
	return deauth ? effects.driver_deauthenticate_calls :
		effects.driver_disassociate_calls;
}

static unsigned int local_mlme_callback_count(int deauth)
{
	return deauth ? effects.mlme_deauthenticate_calls :
		effects.mlme_disassociate_calls;
}

static int local_driver_callback_was_null(int deauth)
{
	return deauth ? effects.last_driver_deauthenticate_null :
		effects.last_driver_disassociate_null;
}

static const u8 *local_driver_callback_addr(int deauth)
{
	return deauth ? effects.last_driver_deauthenticate_addr :
		effects.last_driver_disassociate_addr;
}

static void check_disconnect_cleanup(struct wpa_supplicant *wpa_s,
				     int old_state,
				     const u8 *expected_clear_addr)
{
	CHECK(wpa_s->wpa_state == WPA_DISCONNECTED);
	CHECK(is_zero_ether_addr(wpa_s->bssid));
	CHECK(is_zero_ether_addr(wpa_s->pending_bssid));
	CHECK(wpa_s->current_ssid == NULL);
	CHECK(wpa_s->current_bss == NULL);
	CHECK(effects.state_change_calls == (old_state == WPA_DISCONNECTED ? 0 : 1));
	CHECK(effects.clear_keys_calls == 1);
	CHECK(effects.last_clear_keys_null == (expected_clear_addr == NULL));
	if (expected_clear_addr) {
		CHECK(memcmp(effects.last_clear_keys_addr, expected_clear_addr,
			     ETH_ALEN) == 0);
		CHECK(!effects.last_clear_keys_addr_zero);
	}
	CHECK(effects.sm_set_config_calls == 1);
	CHECK(effects.last_sm_config_null);
	CHECK(effects.eapol_config_calls == 1);
	CHECK(effects.last_eapol_config_null);
	CHECK(effects.network_changed_calls == 1);
	CHECK(effects.timer_cancel_calls == 1);
	CHECK(effects.port_enabled_false_calls == 1);
	CHECK(effects.port_valid_false_calls == 1);
	CHECK(effects.eap_success_false_calls == 1);
}

static void test_pending_cancel_route(int deauth, enum wpa_states state)
{
	struct wpa_supplicant wpa_s;
	u8 expected[ETH_ALEN];
	init_supplicant(&wpa_s, state);
	wpa_s.drv_flags = WPA_DRIVER_FLAGS_CANCEL_PENDING;
	memcpy(expected, wpa_s.pending_bssid, ETH_ALEN);
	reset_effects();

	invoke_local_disconnect(&wpa_s, deauth);

	CHECK(local_driver_callback_count(deauth) == 1);
	CHECK(local_mlme_callback_count(deauth) == 0);
	CHECK(!local_driver_callback_was_null(deauth));
	CHECK(memcmp(local_driver_callback_addr(deauth), expected, ETH_ALEN) == 0);
	CHECK((deauth ? effects.last_deauthenticate_reason :
	       effects.last_disassociate_reason) == TEST_REASON_CODE);
	CHECK(effects.bssid_notify_calls == 0);
	check_disconnect_cleanup(&wpa_s, state, NULL);
}

static void test_unknown_pending_cancel_route(int deauth)
{
	struct wpa_supplicant wpa_s;
	init_supplicant(&wpa_s, WPA_ASSOCIATING);
	wpa_s.drv_flags = WPA_DRIVER_FLAGS_CANCEL_PENDING;
	memset(wpa_s.pending_bssid, 0, ETH_ALEN);
	reset_effects();

	invoke_local_disconnect(&wpa_s, deauth);

	CHECK(local_driver_callback_count(deauth) == 1);
	CHECK(local_mlme_callback_count(deauth) == 0);
	CHECK(local_driver_callback_was_null(deauth));
	CHECK((deauth ? effects.last_deauthenticate_reason :
	       effects.last_disassociate_reason) == TEST_REASON_CODE);
	check_disconnect_cleanup(&wpa_s, WPA_ASSOCIATING, NULL);
}

static void test_cancel_flag_gating(int deauth)
{
	struct wpa_supplicant wpa_s;
	init_supplicant(&wpa_s, WPA_AUTHENTICATING);
	reset_effects();

	invoke_local_disconnect(&wpa_s, deauth);

	CHECK(local_driver_callback_count(deauth) == 0);
	CHECK(local_mlme_callback_count(deauth) == 0);
	check_disconnect_cleanup(&wpa_s, WPA_AUTHENTICATING, NULL);
}

static void test_nonpending_state_no_callback(int deauth)
{
	struct wpa_supplicant wpa_s;
	init_supplicant(&wpa_s, WPA_DISCONNECTED);
	wpa_s.drv_flags = WPA_DRIVER_FLAGS_CANCEL_PENDING;
	reset_effects();

	invoke_local_disconnect(&wpa_s, deauth);

	CHECK(local_driver_callback_count(deauth) == 0);
	CHECK(local_mlme_callback_count(deauth) == 0);
	check_disconnect_cleanup(&wpa_s, WPA_DISCONNECTED, NULL);
}

static void test_confirmed_bssid_precedence(int deauth,
					    enum wpa_states state,
					    int user_space_mlme)
{
	struct wpa_supplicant wpa_s;
	u8 expected[ETH_ALEN];
	init_supplicant(&wpa_s, state);
	wpa_s.drv_flags = WPA_DRIVER_FLAGS_CANCEL_PENDING;
	if (user_space_mlme)
		wpa_s.drv_flags |= WPA_DRIVER_FLAGS_USER_SPACE_MLME;
	fill_address(wpa_s.bssid, 0x61);
	memcpy(expected, wpa_s.bssid, ETH_ALEN);
	reset_effects();

	invoke_local_disconnect(&wpa_s, deauth);

	if (user_space_mlme) {
		CHECK(local_driver_callback_count(deauth) == 0);
		CHECK(local_mlme_callback_count(deauth) == 1);
	} else {
		CHECK(local_driver_callback_count(deauth) == 1);
		CHECK(local_mlme_callback_count(deauth) == 0);
		CHECK(!local_driver_callback_was_null(deauth));
		CHECK(memcmp(local_driver_callback_addr(deauth), expected,
			     ETH_ALEN) == 0);
	}
	CHECK((deauth ? effects.last_deauthenticate_reason :
	       effects.last_disassociate_reason) == TEST_REASON_CODE);
	CHECK(effects.bssid_notify_calls == 1);
	check_disconnect_cleanup(&wpa_s, state, expected);
}

static void test_legacy_confirmed_route(int deauth)
{
	struct wpa_supplicant wpa_s;
	u8 expected[ETH_ALEN];
	init_supplicant(&wpa_s, WPA_COMPLETED);
	fill_address(wpa_s.bssid, 0x71);
	memcpy(expected, wpa_s.bssid, ETH_ALEN);
	reset_effects();

	invoke_local_disconnect(&wpa_s, deauth);

	CHECK(local_driver_callback_count(deauth) == 1);
	CHECK(local_mlme_callback_count(deauth) == 0);
	CHECK(!local_driver_callback_was_null(deauth));
	CHECK(memcmp(local_driver_callback_addr(deauth), expected, ETH_ALEN) == 0);
	CHECK((deauth ? effects.last_deauthenticate_reason :
	       effects.last_disassociate_reason) == TEST_REASON_CODE);
	check_disconnect_cleanup(&wpa_s, WPA_COMPLETED, expected);
}

int main(void)
{
	test_active_failure(WPA_AUTHENTICATING, 0);
	test_active_failure(WPA_ASSOCIATING, 0);
	test_active_failure(WPA_ASSOCIATING, 1);
	test_ignored_states();
	test_duplicate_failure();
	test_retry_suppressed_but_state_cleared(1, 0);
	test_retry_suppressed_but_state_cleared(0, 1);
	test_timer_cancellation_blacklist_semantics();
	test_pending_cancel_route(0, WPA_AUTHENTICATING);
	test_pending_cancel_route(0, WPA_ASSOCIATING);
	test_pending_cancel_route(1, WPA_AUTHENTICATING);
	test_pending_cancel_route(1, WPA_ASSOCIATING);
	test_unknown_pending_cancel_route(0);
	test_unknown_pending_cancel_route(1);
	test_cancel_flag_gating(0);
	test_cancel_flag_gating(1);
	test_nonpending_state_no_callback(0);
	test_nonpending_state_no_callback(1);
	test_confirmed_bssid_precedence(0, WPA_ASSOCIATING, 0);
	test_confirmed_bssid_precedence(1, WPA_ASSOCIATING, 0);
	test_confirmed_bssid_precedence(0, WPA_COMPLETED, 0);
	test_confirmed_bssid_precedence(1, WPA_COMPLETED, 0);
	test_confirmed_bssid_precedence(0, WPA_COMPLETED, 1);
	test_confirmed_bssid_precedence(1, WPA_COMPLETED, 1);
	test_legacy_confirmed_route(0);
	test_legacy_confirmed_route(1);

	if (failure_count) {
		fprintf(stderr, "%u of %u checks failed\n", failure_count,
			check_count);
		return 1;
	}
	printf("SANA-II generic association failure state: %u checks passed\n",
	       check_count);
	return 0;
}
