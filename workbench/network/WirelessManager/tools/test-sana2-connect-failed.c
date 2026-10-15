#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sana2-connect-failed-source-constants.inc"

typedef uint32_t ULONG;
typedef uintptr_t UPINT;
typedef void *APTR;

#define os_memset memset

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

struct Sana2FullMACCapabilities {
	ULONG SizeAvailable;
	ULONG SizeSupplied;
	ULONG Version;
	ULONG Flags;
	ULONG Profiles;
	ULONG SAEPweMethods;
	ULONG Reserved[2];
};

struct IOSana2Req {
	struct {
		ULONG io_Command;
		int io_Error;
	} ios2_Req;
	ULONG ios2_WireError;
	ULONG ios2_DataLength;
	APTR ios2_Data;
	APTR ios2_StatData;
};

struct wpa_driver_sana2_data {
	void *ctx;
	struct IOSana2Req *request;
	int hard_mac;
	int wireless_caps_queried;
	int wireless_caps_valid;
	struct Sana2WirelessCapabilities wireless_caps;
	/* This fixture keeps old v1 behavior; the new typed query path is tested
	 * against the actual public contract in test-wireless-fullmac-driver.py. */
	int fullmac_caps_valid;
	struct Sana2FullMACCapabilities fullmac_caps;
	ULONG active_association_cookie;
	ULONG active_fullmac_profile;
};

static unsigned int scenario_count;
static unsigned int check_count;
static unsigned int failure_count;
static unsigned int query_calls;
static unsigned int unexpected_io_calls;
static unsigned int fullmac_validator_calls;
static int query_return;
static int query_error;
static ULONG reply_size_supplied;
static ULONG reply_version;
static ULONG reply_flags;
static ULONG reply_protocols;
static ULONG reply_key_management;
static ULONG reply_pairwise_ciphers;
static ULONG reply_group_ciphers;
static ULONG observed_size_available;
static ULONG observed_request_version;
static ULONG observed_command;
static unsigned int cancel_calls;
static unsigned int event_calls;
static void *last_cancel_eloop_data;
static void *last_cancel_timeout_data;
static void (*last_cancel_handler)(void *, void *);
static void *last_event_context;
static void *last_event_data;
static int last_event_type;

static void wpa_driver_sana2_stay_alive(void *eloop_ctx, void *timeout_ctx);
static void eloop_cancel_timeout(void (*handler)(void *, void *),
	void *eloop_data, void *timeout_data);
static void wpa_supplicant_event(void *ctx, int event, void *event_data);
static int DoIO(APTR request_arg);

/* Legacy fixtures answer only the v1 command and never validate an extension. */
static int Sana2FullMACCapabilitiesValid(
	const struct Sana2FullMACCapabilities *caps)
{
	(void) caps;
	++fullmac_validator_calls;
	return 0;
}

#include "sana2-caps-valid.inc"
#include "sana2-query-caps.inc"
#include "sana2-initial-event-mask.inc"
#include "sana2-connect-failed.inc"

static void check_value(const char *scenario, const char *predicate,
			int actual, int expected)
{
	++check_count;
	if (actual != expected) {
		fprintf(stderr, "FAIL: %s: %s got %d, expected %d\n",
			scenario, predicate, actual, expected);
		++failure_count;
	}
}

#define CHECK(scenario, predicate, actual, expected) \
	check_value((scenario), (predicate), (int) (actual), (int) (expected))

#define SCENARIO() (++scenario_count)

static void reset_observers(void)
{
	query_calls = 0;
	unexpected_io_calls = 0;
	fullmac_validator_calls = 0;
	query_return = 0;
	query_error = 0;
	reply_size_supplied = sizeof(struct Sana2WirelessCapabilities);
	reply_version = S2WIRELESS_CAPABILITIES_VERSION;
	reply_flags = S2WIRELESS_CAPF_CONNECT_FAILED;
	reply_protocols = 0;
	reply_key_management = 0;
	reply_pairwise_ciphers = 0;
	reply_group_ciphers = 0;
	observed_size_available = 0;
	observed_request_version = 0;
	observed_command = 0;
	cancel_calls = 0;
	event_calls = 0;
	last_cancel_eloop_data = NULL;
	last_cancel_timeout_data = NULL;
	last_cancel_handler = NULL;
	last_event_context = NULL;
	last_event_data = (void *) (uintptr_t) 1;
	last_event_type = -1;
}

static void init_fixture(struct wpa_driver_sana2_data *drv,
			 struct IOSana2Req *request, int hard_mac)
{
	memset(drv, 0, sizeof(*drv));
	memset(request, 0, sizeof(*request));
	reset_observers();
	drv->ctx = (void *) (uintptr_t) 0xabc;
	drv->request = request;
	drv->hard_mac = hard_mac;
}

static int DoIO(APTR request_arg)
{
	struct IOSana2Req *request = request_arg;
	struct Sana2WirelessCapabilities *caps;

	++query_calls;
	observed_command = request->ios2_Req.io_Command;
	if (request->ios2_Req.io_Command != S2_GETWIRELESSCAPABILITIES) {
		++unexpected_io_calls;
		return -1;
	}
	caps = request->ios2_StatData;
	if (caps == NULL) {
		++unexpected_io_calls;
		return -1;
	}
	observed_size_available = caps->SizeAvailable;
	observed_request_version = caps->Version;
	caps->SizeSupplied = reply_size_supplied;
	caps->Version = reply_version;
	caps->Flags = reply_flags;
	caps->Protocols = reply_protocols;
	caps->KeyManagement = reply_key_management;
	caps->PairwiseCiphers = reply_pairwise_ciphers;
	caps->GroupCiphers = reply_group_ciphers;
	request->ios2_Req.io_Error = query_error;
	return query_return;
}

static void wpa_driver_sana2_stay_alive(void *eloop_ctx, void *timeout_ctx)
{
	(void) eloop_ctx;
	(void) timeout_ctx;
}

static void eloop_cancel_timeout(void (*handler)(void *, void *),
	void *eloop_data, void *timeout_data)
{
	++cancel_calls;
	last_cancel_handler = handler;
	last_cancel_eloop_data = eloop_data;
	last_cancel_timeout_data = timeout_data;
}

static void wpa_supplicant_event(void *ctx, int event, void *event_data)
{
	++event_calls;
	last_event_context = ctx;
	last_event_type = event;
	last_event_data = event_data;
}

static void test_initial_event_mask_negotiation(void)
{
	struct wpa_driver_sana2_data drv;
	struct IOSana2Req request;
	ULONG mask;

	init_fixture(&drv, &request, 1);
	SCENARIO();
	mask = wpa_driver_sana2_initial_event_mask(&drv);
	CHECK("valid independent CONNECT_FAILED capability", "event mask includes failure",
	      mask, S2EVENT_OFFLINE | S2EVENT_CONNECT | S2EVENT_DISCONNECT |
		S2EVENT_CONNECT_FAILED);
	CHECK("valid independent CONNECT_FAILED capability", "query occurs once",
	      query_calls, 1);
	CHECK("valid independent CONNECT_FAILED capability", "query command",
	      observed_command, S2_GETWIRELESSCAPABILITIES);
	CHECK("valid independent CONNECT_FAILED capability", "caller advertises full output span",
	      observed_size_available, sizeof(struct Sana2WirelessCapabilities));
	CHECK("valid independent CONNECT_FAILED capability", "caller selects v1",
	      observed_request_version, S2WIRELESS_CAPABILITIES_VERSION);
	CHECK("valid independent CONNECT_FAILED capability", "capability retained",
	      drv.wireless_caps_valid, 1);
	CHECK("valid independent CONNECT_FAILED capability", "query marked complete",
	      drv.wireless_caps_queried, 1);
	CHECK("valid independent CONNECT_FAILED capability", "query statdata released",
	      request.ios2_StatData == NULL, 1);
	CHECK("valid independent CONNECT_FAILED capability", "no unrelated I/O",
	      unexpected_io_calls, 0);
	CHECK("legacy v1 fixture", "no FullMAC validation attempted",
	      fullmac_validator_calls, 0);

	mask = wpa_driver_sana2_initial_event_mask(&drv);
	CHECK("capability query is once per open", "second mask unchanged",
	      mask, S2EVENT_OFFLINE | S2EVENT_CONNECT | S2EVENT_DISCONNECT |
		S2EVENT_CONNECT_FAILED);
	CHECK("capability query is once per open", "no second capability query",
	      query_calls, 1);
}

static void test_valid_but_unadvertised_and_soft_mac(void)
{
	struct wpa_driver_sana2_data drv;
	struct IOSana2Req request;
	ULONG mask;

	init_fixture(&drv, &request, 1);
	reply_flags = 0;
	SCENARIO();
	mask = wpa_driver_sana2_initial_event_mask(&drv);
	CHECK("valid capability without flag", "legacy hard-MAC mask",
	      mask, S2EVENT_OFFLINE | S2EVENT_CONNECT | S2EVENT_DISCONNECT);
	CHECK("valid capability without flag", "response remains valid",
	      drv.wireless_caps_valid, 1);
	CHECK("valid capability without flag", "query count",
	      query_calls, 1);

	init_fixture(&drv, &request, 0);
	SCENARIO();
	mask = wpa_driver_sana2_initial_event_mask(&drv);
	CHECK("soft MAC with capability", "only offline event armed",
	      mask, S2EVENT_OFFLINE);
	CHECK("soft MAC with capability", "capability still queried",
	      query_calls, 1);
	CHECK("soft MAC with capability", "capability remains valid",
	      drv.wireless_caps_valid, 1);
}

static void test_failed_unsupported_queries(void)
{
	struct wpa_driver_sana2_data drv;
	struct IOSana2Req request;
	int kind;
	ULONG mask;

	for (kind = 0; kind < 2; ++kind) {
		char scenario[64];
		init_fixture(&drv, &request, 1);
		query_return = kind == 0 ? -1 : 0;
		query_error = kind == 0 ? 0 : S2ERR_NOT_SUPPORTED;
		SCENARIO();
		mask = wpa_driver_sana2_initial_event_mask(&drv);
		(void) snprintf(scenario, sizeof(scenario),
				"unsupported device query %s",
				kind == 0 ? "DoIO failure" : "io_Error");
		CHECK(scenario, "failure event not armed", mask & S2EVENT_CONNECT_FAILED, 0);
		CHECK(scenario, "legacy hard-MAC events retained",
		      mask, S2EVENT_OFFLINE | S2EVENT_CONNECT | S2EVENT_DISCONNECT);
		CHECK(scenario, "invalid capabilities not retained",
		      drv.wireless_caps_valid, 0);
		CHECK(scenario, "query marked complete after failure",
		      drv.wireless_caps_queried, 1);
		CHECK(scenario, "failure clears borrowed statdata",
		      request.ios2_StatData == NULL, 1);
		CHECK(scenario, "exactly one query attempted", query_calls, 1);
	}
}

static void test_short_and_unknown_capabilities(void)
{
	struct wpa_driver_sana2_data drv;
	struct IOSana2Req request;
	int kind;
	ULONG mask;

	for (kind = 0; kind < 4; ++kind) {
		char scenario[64];
		init_fixture(&drv, &request, 1);
		switch (kind) {
		case 0:
			reply_size_supplied = sizeof(struct Sana2WirelessCapabilities) - sizeof(ULONG);
			break;
		case 1:
			reply_version = S2WIRELESS_CAPABILITIES_VERSION + 1;
			break;
		case 2:
			reply_flags |= (1UL << 5);
			break;
		default:
			reply_protocols = (1UL << 5);
			break;
		}
		SCENARIO();
		mask = wpa_driver_sana2_initial_event_mask(&drv);
		(void) snprintf(scenario, sizeof(scenario), "invalid capability response %d", kind);
		CHECK(scenario, "failure event not armed", mask & S2EVENT_CONNECT_FAILED, 0);
		CHECK(scenario, "invalid structure not retained", drv.wireless_caps_valid, 0);
		CHECK(scenario, "request marked queried", drv.wireless_caps_queried, 1);
		CHECK(scenario, "statdata cleared after rejection",
		      request.ios2_StatData == NULL, 1);
	}
}

static void check_failure_ignored(const char *scenario, int hard_mac,
				  int caps_valid, ULONG flags,
				  int io_error, ULONG wire_error,
				  ULONG data_length, ULONG active_cookie)
{
	struct wpa_driver_sana2_data drv;
	struct IOSana2Req request;

	init_fixture(&drv, &request, hard_mac);
	drv.wireless_caps_valid = caps_valid;
	drv.wireless_caps.Flags = flags;
	drv.active_association_cookie = active_cookie;
	request.ios2_Req.io_Error = io_error;
	request.ios2_WireError = wire_error;
	request.ios2_DataLength = data_length;
	SCENARIO();
	wpa_driver_sana2_connect_failed(&drv, &request);
	CHECK(scenario, "active cookie preserved", drv.active_association_cookie,
	      active_cookie);
	CHECK(scenario, "stay-alive cancellation omitted", cancel_calls, 0);
	CHECK(scenario, "association failure omitted", event_calls, 0);
}

static void test_matching_stale_and_duplicate_failures(void)
{
	struct wpa_driver_sana2_data drv;
	struct IOSana2Req request;
	ULONG cookie = 0x89abcdefUL;

	init_fixture(&drv, &request, 1);
	drv.wireless_caps_valid = 1;
	drv.wireless_caps.Flags = S2WIRELESS_CAPF_CONNECT_FAILED;
	drv.active_association_cookie = cookie;
	request.ios2_Req.io_Error = 0;
	request.ios2_WireError = S2EVENT_CONNECT_FAILED;
	request.ios2_DataLength = cookie;
	SCENARIO();
	wpa_driver_sana2_connect_failed(&drv, &request);
	CHECK("matching failure", "active cookie cleared", drv.active_association_cookie, 0);
	CHECK("matching failure", "one stay-alive cancellation", cancel_calls, 1);
	CHECK("matching failure", "cancels SANA stay-alive handler",
	      last_cancel_handler == wpa_driver_sana2_stay_alive, 1);
	CHECK("matching failure", "cancel context is supplicant context",
	      last_cancel_eloop_data == drv.ctx, 1);
	CHECK("matching failure", "cancel user data is driver context",
	      last_cancel_timeout_data == &drv, 1);
	CHECK("matching failure", "one failure event", event_calls, 1);
	CHECK("matching failure", "event context exact",
	      last_event_context == drv.ctx, 1);
	CHECK("matching failure", "event type is association failed",
	      last_event_type, EVENT_ASSOC_FAILED);
	CHECK("matching failure", "event data is NULL", last_event_data == NULL, 1);

	SCENARIO();
	wpa_driver_sana2_connect_failed(&drv, &request);
	CHECK("duplicate failure", "cookie remains cleared",
	      drv.active_association_cookie, 0);
	CHECK("duplicate failure", "no duplicate cancellation", cancel_calls, 1);
	CHECK("duplicate failure", "no duplicate event", event_calls, 1);

	check_failure_ignored("stale failure cookie", 1, 1,
		S2WIRELESS_CAPF_CONNECT_FAILED, 0, S2EVENT_CONNECT_FAILED,
		cookie - 1, cookie);
	check_failure_ignored("zero failure cookie", 1, 1,
		S2WIRELESS_CAPF_CONNECT_FAILED, 0, S2EVENT_CONNECT_FAILED, 0, cookie);
	check_failure_ignored("no active association cookie", 1, 1,
		S2WIRELESS_CAPF_CONNECT_FAILED, 0, S2EVENT_CONNECT_FAILED, 0, 0);
	check_failure_ignored("unnegotiated failure flag", 1, 0,
		S2WIRELESS_CAPF_CONNECT_FAILED, 0, S2EVENT_CONNECT_FAILED,
		cookie, cookie);
	check_failure_ignored("missing failure capability", 1, 1, 0,
		0, S2EVENT_CONNECT_FAILED, cookie, cookie);
	check_failure_ignored("soft MAC failure", 0, 1,
		S2WIRELESS_CAPF_CONNECT_FAILED, 0, S2EVENT_CONNECT_FAILED,
		cookie, cookie);
	check_failure_ignored("request error", 1, 1,
		S2WIRELESS_CAPF_CONNECT_FAILED, S2ERR_NOT_SUPPORTED,
		S2EVENT_CONNECT_FAILED, cookie, cookie);
	check_failure_ignored("missing failure event", 1, 1,
		S2WIRELESS_CAPF_CONNECT_FAILED, 0, 0, cookie, cookie);
	check_failure_ignored("CONNECT mixed with failure", 1, 1,
		S2WIRELESS_CAPF_CONNECT_FAILED, 0,
		S2EVENT_CONNECT_FAILED | S2EVENT_CONNECT, cookie, cookie);
	check_failure_ignored("DISCONNECT mixed with failure", 1, 1,
		S2WIRELESS_CAPF_CONNECT_FAILED, 0,
		S2EVENT_CONNECT_FAILED | S2EVENT_DISCONNECT, cookie, cookie);
	check_failure_ignored("OFFLINE mixed with failure", 1, 1,
		S2WIRELESS_CAPF_CONNECT_FAILED, 0,
		S2EVENT_CONNECT_FAILED | S2EVENT_OFFLINE, cookie, cookie);
	check_failure_ignored("unknown event mixed with failure", 1, 1,
		S2WIRELESS_CAPF_CONNECT_FAILED, 0,
		S2EVENT_CONNECT_FAILED | (1UL << 7), cookie, cookie);
}

int main(void)
{
	test_initial_event_mask_negotiation();
	test_valid_but_unadvertised_and_soft_mac();
	test_failed_unsupported_queries();
	test_short_and_unknown_capabilities();
	test_matching_stale_and_duplicate_failures();
	if (failure_count != 0) {
		fprintf(stderr, "FAIL: %u/%u checks failed across %u scenarios\n",
			failure_count, check_count, scenario_count);
		return 1;
	}
	printf("PASS: %u scenarios, %u checks\n", scenario_count, check_count);
	return 0;
}
