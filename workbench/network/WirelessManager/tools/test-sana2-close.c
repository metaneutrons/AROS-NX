#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sana2-close-constants.inc"

typedef uint32_t u32;
typedef uint32_t ULONG;
typedef void *APTR;
typedef void (*eloop_signal_handler)(int sig, void *signal_ctx);
typedef void (*eloop_timeout_handler)(void *eloop_ctx, void *timeout_ctx);

struct eloop_signal {
	int sig;
	void *user_data;
	eloop_signal_handler handler;
	int signaled;
};

struct eloop_data {
	int signal_count;
	struct eloop_signal *signals;
	int signaled;
	int signal_dispatch_depth;
	int pending_terminate;
};

static struct eloop_data eloop;

struct MsgPort {
	unsigned char mp_SigBit;
};

struct IOSana2Req {
	int id;
};

struct wpa_driver_sana2_data {
	void *ctx;
	char *device_name;
	struct MsgPort *port;
	struct IOSana2Req *request;
	struct IOSana2Req *event_request;
	struct IOSana2Req *eapol_request;
	struct IOSana2Req *mlme_requests[MLME_REQ_COUNT];
	int device_opened;
	int signal_registered;
};

enum log_kind {
	LOG_TIMEOUT_CANCEL,
	LOG_SIGNAL_UNREGISTER,
	LOG_ABORT_IO,
	LOG_WAIT_IO,
	LOG_FREEVEC,
	LOG_CLOSE_DEVICE,
	LOG_DELETE_IO_REQUEST,
	LOG_CLEAR_SIGNAL,
	LOG_DELETE_MSG_PORT,
	LOG_OS_FREE
};

struct log_record {
	enum log_kind kind;
	void *ptr;
};

#define LOG_CAPACITY 128
static struct log_record log_records[LOG_CAPACITY];
static size_t log_count;
static unsigned int scenario_count;
static unsigned int check_count;
static unsigned int failure_count;
static eloop_timeout_handler cancelled_timeout_handler;
static void *cancelled_eloop_data;
static void *cancelled_timeout_data;
static int event_callback_count;
static void *last_event_callback_data;
static int fail_next_realloc;

static void record(enum log_kind kind, void *ptr)
{
	if (log_count >= LOG_CAPACITY) {
		fprintf(stderr, "FAIL: event log overflow\n");
		++failure_count;
		return;
	}
	log_records[log_count].kind = kind;
	log_records[log_count].ptr = ptr;
	++log_count;
}

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
	check_value((scenario), (predicate), (int)(actual), (int)(expected))
#define SCENARIO() (++scenario_count)

static void eloop_unregister_signal_impl(int sig,
					 eloop_signal_handler handler,
					 void *user_data);
static void eloop_unregister_signal(int sig,
				    eloop_signal_handler handler,
				    void *user_data)
{
	record(LOG_SIGNAL_UNREGISTER, user_data);
	eloop_unregister_signal_impl(sig, handler, user_data);
}

static void wpa_driver_sana2_stay_alive(void *eloop_ctx, void *timeout_ctx);
static void wpa_driver_sana2_event_handler(int sig, void *sig_ctx);
static int eloop_cancel_timeout(eloop_timeout_handler handler,
				void *eloop_data, void *user_data);
static void AbortIO(APTR request);
static void WaitIO(APTR request);
static void FreeVec(APTR ptr);
static void CloseDevice(APTR request);
static void DeleteIORequest(APTR request);
static ULONG SetSignal(ULONG new_signals, ULONG signal_set);
static void DeleteMsgPort(struct MsgPort *port);
static void os_free(void *ptr);

static void *test_realloc(void *ptr, size_t size)
{
	if (fail_next_realloc) {
		fail_next_realloc = 0;
		return NULL;
	}
	return realloc(ptr, size);
}

#define realloc test_realloc

#include "eloop_signal_mask.inc"
#include "eloop_handle_signals.inc"
#include "eloop_compact_signals.inc"
#include "eloop_process_pending_signals.inc"
#include "eloop_register_signal.inc"
#include "eloop_unregister_signal.inc"
#include "wpa_driver_sana2_deinit.inc"

#undef realloc

static int eloop_cancel_timeout(eloop_timeout_handler handler,
				void *eloop_data, void *user_data)
{
	record(LOG_TIMEOUT_CANCEL, user_data);
	cancelled_timeout_handler = handler;
	cancelled_eloop_data = eloop_data;
	cancelled_timeout_data = user_data;
	return 1;
}

static void AbortIO(APTR request)
{
	record(LOG_ABORT_IO, request);
}

static void WaitIO(APTR request)
{
	record(LOG_WAIT_IO, request);
}

static void FreeVec(APTR ptr)
{
	record(LOG_FREEVEC, ptr);
	free(ptr);
}

static void CloseDevice(APTR request)
{
	record(LOG_CLOSE_DEVICE, request);
}

static void DeleteIORequest(APTR request)
{
	record(LOG_DELETE_IO_REQUEST, request);
	free(request);
}

static ULONG SetSignal(ULONG new_signals, ULONG signal_set)
{
	(void)new_signals;
	record(LOG_CLEAR_SIGNAL, (void *)(uintptr_t)signal_set);
	return 0;
}

static void DeleteMsgPort(struct MsgPort *port)
{
	record(LOG_DELETE_MSG_PORT, port);
	free(port);
}

static void os_free(void *ptr)
{
	record(LOG_OS_FREE, ptr);
	free(ptr);
}

static void wpa_driver_sana2_stay_alive(void *eloop_ctx, void *timeout_ctx)
{
	(void)eloop_ctx;
	(void)timeout_ctx;
}

static void wpa_driver_sana2_event_handler(int sig, void *sig_ctx)
{
	(void)sig;
	++event_callback_count;
	last_event_callback_data = sig_ctx;
}

struct mutation_context {
	int is_other_bit;
};

static struct mutation_context mutation_a;
static struct mutation_context mutation_b;
static struct mutation_context mutation_other;
static int mutation_a_calls;
static int mutation_b_calls;
static int mutation_other_calls;
static int mutation_reentered;
static int registration_appended_during_dispatch;

static void mutation_handler(int sig, void *context)
{
	if (context == &mutation_a) {
		++mutation_a_calls;
		if (!mutation_reentered) {
			eloop_unregister_signal(sig, mutation_handler, &mutation_a);
			eloop_unregister_signal(6, (eloop_signal_handler)mutation_handler,
					&mutation_other);
			CHECK("mutation callback", "re-registers its tuple",
				eloop_register_signal(sig, mutation_handler,
					&mutation_a), 0);
			registration_appended_during_dispatch =
				eloop.signal_count == 4;
			mutation_reentered = 1;
		}
	} else if (context == &mutation_b) {
		++mutation_b_calls;
	} else if (context == &mutation_other) {
		++mutation_other_calls;
	}
}

static void high_bit_handler(int sig, void *context)
{
	int *calls = context;
	(void)sig;
	++*calls;
}

static int find_record(enum log_kind kind, void *ptr)
{
	size_t i;
	for (i = 0; i < log_count; i++)
		if (log_records[i].kind == kind && log_records[i].ptr == ptr)
			return (int)i;
	return -1;
}

static int count_records(enum log_kind kind, void *ptr)
{
	size_t i;
	int count = 0;
	for (i = 0; i < log_count; i++)
		if (log_records[i].kind == kind && log_records[i].ptr == ptr)
			++count;
	return count;
}

static struct IOSana2Req *new_request(int id)
{
	struct IOSana2Req *request = calloc(1, sizeof(*request));
	if (request != NULL)
		request->id = id;
	return request;
}

static void test_signal_dispatch_mutation(void)
{
	SCENARIO();
	mutation_a_calls = 0;
	mutation_b_calls = 0;
	mutation_other_calls = 0;
	mutation_reentered = 0;
	registration_appended_during_dispatch = 0;
	CHECK("signal mutation", "register A", eloop_register_signal(5,
		mutation_handler, &mutation_a), 0);
	CHECK("signal mutation", "register same-bit B",
		eloop_register_signal(5, mutation_handler, &mutation_b), 0);
	CHECK("signal mutation", "register other bit",
		eloop_register_signal(6, mutation_handler, &mutation_other), 0);
	CHECK("signal mutation", "same-bit handlers share a wait bit",
		eloop_signal_mask(), (1u << 5) | (1u << 6));

	eloop_handle_signals((1u << 5) | (1u << 6));
	eloop_process_pending_signals();
	CHECK("signal mutation", "A called once", mutation_a_calls, 1);
	CHECK("signal mutation", "B survives exact unregistration",
		mutation_b_calls, 1);
	CHECK("signal mutation", "other pending callback removed",
		mutation_other_calls, 0);
	CHECK("signal mutation", "callback registration appended",
		registration_appended_during_dispatch, 1);
	CHECK("signal mutation", "only live signal bit remains",
		eloop_signal_mask(), 1u << 5);

	eloop_handle_signals(1u << 5);
	eloop_process_pending_signals();
	CHECK("signal mutation", "re-registered A called on next pass",
		mutation_a_calls, 2);
	CHECK("signal mutation", "distinct userdata B remains registered",
		mutation_b_calls, 2);
	eloop_unregister_signal(5, mutation_handler, &mutation_a);
	eloop_unregister_signal(5, mutation_handler, &mutation_b);
	CHECK("signal mutation", "all callbacks removed",
		eloop_signal_mask(), 0);
}

static void test_signal_validation_and_unsigned_shift(void)
{
	SCENARIO();
	int calls = 0;
	CHECK("signal validation", "reject negative signal",
		eloop_register_signal(-1, high_bit_handler, &calls), -1);
	CHECK("signal validation", "reject out-of-range signal",
		eloop_register_signal(32, high_bit_handler, &calls), -1);
	CHECK("signal validation", "reject null handler",
		eloop_register_signal(1, NULL, &calls), -1);
	CHECK("signal validation", "accept highest ULONG bit",
		eloop_register_signal(31, high_bit_handler, &calls), 0);
	CHECK("signal validation", "unsigned high-bit mask",
		eloop_signal_mask(), 0x80000000u);
	fail_next_realloc = 1;
	CHECK("signal validation", "allocation failure is reported",
		eloop_register_signal(30, high_bit_handler, &calls), -1);
	CHECK("signal validation", "allocation failure preserves live entry",
		eloop_signal_mask(), 0x80000000u);
	eloop_handle_signals(0x80000000u);
	eloop_process_pending_signals();
	CHECK("signal validation", "highest bit dispatched", calls, 1);
	eloop_unregister_signal(31, high_bit_handler, &calls);
	CHECK("signal validation", "tombstone excluded from wait mask",
		eloop_signal_mask(), 0);
}

static void reset_log(void)
{
	log_count = 0;
	cancelled_timeout_handler = NULL;
	cancelled_eloop_data = NULL;
	cancelled_timeout_data = NULL;
}

static void test_driver_deinit_order(void)
{
	SCENARIO();
	const char scenario[] = "driver deinit";
	struct wpa_driver_sana2_data *drv = calloc(1, sizeof(*drv));
	struct MsgPort *port = calloc(1, sizeof(*port));
	struct IOSana2Req *request = new_request(1);
	struct IOSana2Req *event_request = new_request(2);
	struct IOSana2Req *eapol_request = new_request(3);
	struct IOSana2Req *mlme_requests[MLME_REQ_COUNT];
	char *device_name;
	void *other_userdata = (void *)(uintptr_t)0x55;
	int i;
	unsigned char port_bit = 7;
	int cancel_index, unregister_index, event_wait_index, event_free_index;
	int close_index, delete_request_index, clear_signal_index;
	int delete_port_index, free_name_index;
	int last_wait_index;
	int abort_total;

	if (drv == NULL || port == NULL || request == NULL ||
	    event_request == NULL || eapol_request == NULL) {
		fprintf(stderr, "FAIL: could not allocate deinit fixture\n");
		++failure_count;
		free(drv);
		free(port);
		free(request);
		free(event_request);
		free(eapol_request);
		return;
	}
	for (i = 0; i < MLME_REQ_COUNT; i++) {
		mlme_requests[i] = new_request(10 + i);
		if (mlme_requests[i] == NULL) {
			fprintf(stderr, "FAIL: could not allocate MLME fixture\n");
			++failure_count;
			return;
		}
	}
	port->mp_SigBit = port_bit;
	drv->ctx = (void *)(uintptr_t)0xabc;
	device_name = malloc(sizeof("Networks/test.device"));
	if (device_name == NULL) {
		fprintf(stderr, "FAIL: could not allocate device-name fixture\n");
		++failure_count;
		free(drv);
		free(port);
		free(request);
		free(event_request);
		free(eapol_request);
		for (i = 0; i < MLME_REQ_COUNT; i++)
			free(mlme_requests[i]);
		return;
	}
	strcpy(device_name, "Networks/test.device");
	drv->device_name = device_name;
	drv->port = port;
	drv->request = request;
	drv->event_request = event_request;
	drv->eapol_request = eapol_request;
	for (i = 0; i < MLME_REQ_COUNT; i++)
		drv->mlme_requests[i] = mlme_requests[i];
	drv->device_opened = 1;
	drv->signal_registered = 1;
	CHECK(scenario, "allocate device name", device_name != NULL, 1);
	CHECK(scenario, "register driver's event callback",
		eloop_register_signal(7, wpa_driver_sana2_event_handler, drv), 0);
	CHECK(scenario, "retain same-bit callback for other userdata",
		eloop_register_signal(7, wpa_driver_sana2_event_handler,
			other_userdata), 0);

	reset_log();
	event_callback_count = 0;
	last_event_callback_data = NULL;
	wpa_driver_sana2_deinit(drv);

	CHECK(scenario, "cancel keepalive handler",
		cancelled_timeout_handler == wpa_driver_sana2_stay_alive, 1);
	CHECK(scenario, "cancel keepalive with driver context",
		cancelled_eloop_data == (void *)(uintptr_t)0xabc, 1);
	CHECK(scenario, "cancel keepalive with exact driver userdata",
		cancelled_timeout_data == drv, 1);
	cancel_index = find_record(LOG_TIMEOUT_CANCEL, drv);
	unregister_index = find_record(LOG_SIGNAL_UNREGISTER, drv);
	event_wait_index = find_record(LOG_WAIT_IO, event_request);
	event_free_index = find_record(LOG_FREEVEC, event_request);
	close_index = find_record(LOG_CLOSE_DEVICE, request);
	delete_request_index = find_record(LOG_DELETE_IO_REQUEST, request);
	clear_signal_index = find_record(LOG_CLEAR_SIGNAL,
		(void *)(uintptr_t)(1u << port_bit));
	delete_port_index = find_record(LOG_DELETE_MSG_PORT, port);
	free_name_index = find_record(LOG_FREEVEC, device_name);
	last_wait_index = find_record(LOG_WAIT_IO, event_request);
	for (i = 0; i < MLME_REQ_COUNT; i++) {
		int wait_index = find_record(LOG_WAIT_IO, mlme_requests[i]);
		if (wait_index > last_wait_index)
			last_wait_index = wait_index;
	}
	if (find_record(LOG_WAIT_IO, eapol_request) > last_wait_index)
		last_wait_index = find_record(LOG_WAIT_IO, eapol_request);
	if (find_record(LOG_WAIT_IO, request) > last_wait_index)
		last_wait_index = find_record(LOG_WAIT_IO, request);
	CHECK(scenario, "cancel precedes unregister", cancel_index < unregister_index, 1);
	CHECK(scenario, "unregister precedes device close",
		unregister_index < close_index, 1);
	CHECK(scenario, "event request waited before free",
		event_wait_index < event_free_index, 1);
	CHECK(scenario, "event request freed exactly once",
		count_records(LOG_FREEVEC, event_request), 1);
	CHECK(scenario, "device close precedes request deletion",
		close_index < delete_request_index, 1);
	CHECK(scenario, "request deletion precedes port deletion",
		delete_request_index < delete_port_index, 1);
	CHECK(scenario, "all I/O waits precede signal-bit clear",
		last_wait_index < clear_signal_index, 1);
	CHECK(scenario, "owned signal bit cleared before port deletion",
		clear_signal_index < delete_port_index, 1);
	CHECK(scenario, "port deleted before device name release",
		delete_port_index < free_name_index, 1);
	abort_total = count_records(LOG_ABORT_IO, event_request) +
		count_records(LOG_ABORT_IO, eapol_request) +
		count_records(LOG_ABORT_IO, request);
	for (i = 0; i < MLME_REQ_COUNT; i++) {
		abort_total += count_records(LOG_ABORT_IO, mlme_requests[i]);
		CHECK(scenario, "MLME request aborted",
			count_records(LOG_ABORT_IO, mlme_requests[i]), 1);
		CHECK(scenario, "MLME request freed",
			count_records(LOG_FREEVEC, mlme_requests[i]), 1);
	}
	CHECK(scenario, "all outstanding requests aborted", abort_total,
		2 + MLME_REQ_COUNT + 1);

	eloop_handle_signals(1u << 7);
	eloop_process_pending_signals();
	CHECK(scenario, "driver callback removed before freeing its context",
		event_callback_count, 1);
	CHECK(scenario, "same-bit callback with other userdata retained",
		last_event_callback_data == other_userdata, 1);
	eloop_unregister_signal(7, wpa_driver_sana2_event_handler,
				other_userdata);
}

static void test_partial_init_cleanup(void)
{
	SCENARIO();
	const char scenario[] = "partial init cleanup";
	struct wpa_driver_sana2_data *drv = calloc(1, sizeof(*drv));
	struct MsgPort *port = calloc(1, sizeof(*port));
	struct IOSana2Req *request = new_request(20);
	char *device_name = malloc(sizeof("Networks/failure.device"));
	int close_index, delete_request_index, clear_signal_index;
	int delete_port_index;
	unsigned char port_bit = 9;

	if (drv == NULL || port == NULL || request == NULL || device_name == NULL) {
		fprintf(stderr, "FAIL: could not allocate partial-init fixture\n");
		++failure_count;
		free(drv);
		free(port);
		free(request);
		free(device_name);
		return;
	}
	strcpy(device_name, "Networks/failure.device");
	port->mp_SigBit = port_bit;
	drv->device_name = device_name;
	drv->port = port;
	drv->request = request;
	drv->device_opened = 1;
	/* This is the failed signal-registration state: cleanup must not unregister
	 * a tuple the event loop never accepted. */
	drv->signal_registered = 0;
	reset_log();
	wpa_driver_sana2_deinit(drv);
	close_index = find_record(LOG_CLOSE_DEVICE, request);
	delete_request_index = find_record(LOG_DELETE_IO_REQUEST, request);
	clear_signal_index = find_record(LOG_CLEAR_SIGNAL,
		(void *)(uintptr_t)(1u << port_bit));
	delete_port_index = find_record(LOG_DELETE_MSG_PORT, port);
	CHECK(scenario, "failed registration state skips unregister",
		find_record(LOG_SIGNAL_UNREGISTER, NULL), -1);
	CHECK(scenario, "opened device closed on init failure",
		close_index >= 0, 1);
	CHECK(scenario, "failed-init request deleted after close",
		close_index < delete_request_index, 1);
	CHECK(scenario, "failed-init port deleted after request",
		delete_request_index < delete_port_index, 1);
	CHECK(scenario, "owned bit cleared after failed registration",
		clear_signal_index >= 0 && clear_signal_index < delete_port_index, 1);
}

int main(void)
{
	test_signal_dispatch_mutation();
	test_signal_validation_and_unsigned_shift();
	test_driver_deinit_order();
	test_partial_init_cleanup();
	if (failure_count != 0) {
		fprintf(stderr, "sana2 close: %u scenarios, %u checks, %u failures\n",
			scenario_count, check_count, failure_count);
		return 1;
	}
	printf("sana2 close: %u scenarios, %u checks passed\n",
		scenario_count, check_count);
	return 0;
}
