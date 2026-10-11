/*
 * Regression harness for the exact data_transfer_handler body extracted from
 * the candidate ESP-Hosted source tree by run-c6-dispatch-cleanup-tests.sh.
 * Dispatcher and protobuf routines below are host stubs; no IDF or hardware
 * behavior is modeled.
 */
#ifdef C6_DISPATCH_HANDLER_SHIM

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "esp_err.h"
#include "esp_hosted_rpc.pb-c.h"
#include "adapter.h"

#ifndef ESP_ERR_NO_MEM
#define ESP_ERR_NO_MEM 0x101
#endif

#define TAG "dispatch_cleanup_test"
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define C6_BOARD_DISCOVERY_VERSION 2

void *c6_dispatch_outbuf_calloc(size_t count, size_t size);
esp_err_t esp_rpc_command_dispatcher(Rpc *request, Rpc *response,
    void *private_data);
void esp_rpc_cleanup(Rpc *response);
esp_err_t data_transfer_handler(uint32_t session_id, const uint8_t *inbuf,
    ssize_t inlen, uint8_t **outbuf, ssize_t *outlen, void *priv_data);

#include "data-transfer-handler.extracted.inc"

#else

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "esp_err.h"
#include "esp_hosted_rpc.pb-c.h"
#include "adapter.h"

#ifndef ESP_ERR_NO_MEM
#define ESP_ERR_NO_MEM 0x101
#endif

#define CHECK(condition, message) do { \
    ++checks_run; \
    if (!(condition)) return fail(message); \
} while (0)

enum dispatch_mode {
    DISPATCH_REJECT_WITH_PARTIAL_RESPONSE,
    DISPATCH_RESPONSE_ALLOCATION_FAILURE,
    DISPATCH_SUCCESS
};

struct observations {
    unsigned unpack_calls;
    unsigned request_allocations;
    unsigned request_body_allocations;
    unsigned request_free_calls;
    unsigned request_body_free_calls;
    unsigned dispatch_calls;
    unsigned response_allocation_attempts;
    unsigned response_child_allocations;
    unsigned response_data_allocations;
    unsigned response_child_frees;
    unsigned response_data_frees;
    unsigned response_cleanup_calls;
    unsigned packed_size_calls;
    unsigned pack_calls;
    unsigned outbuf_alloc_calls;
    unsigned outbuf_alloc_successes;
    size_t unpacked_length;
    uint32_t dispatched_uid;
};

static struct observations observed;
static const char *active_case = "setup";
static size_t checks_run;
static size_t cases_run;
static size_t failures;
static enum dispatch_mode dispatch_mode;
static int unpack_failure;
static int response_allocation_failure;
static int outbuf_allocation_failure;
static size_t packed_response_size = 8u;

esp_err_t data_transfer_handler(uint32_t session_id, const uint8_t *inbuf,
    ssize_t inlen, uint8_t **outbuf, ssize_t *outlen, void *priv_data);

static int fail(const char *message)
{
    fprintf(stderr, "FAIL %s: %s\n", active_case, message);
    ++failures;
    return 0;
}

static void begin_case(const char *name)
{
    active_case = name;
    ++cases_run;
    memset(&observed, 0, sizeof(observed));
    dispatch_mode = DISPATCH_SUCCESS;
    unpack_failure = 0;
    response_allocation_failure = 0;
    outbuf_allocation_failure = 0;
    packed_response_size = 8u;
}

/* Protobuf allocation/free stubs own one unpacked envelope and its body. */
Rpc *rpc__unpack(ProtobufCAllocator *allocator, size_t length,
    const uint8_t *data)
{
    Rpc *request;
    RpcReqBoardDiscovery *body;
    (void)allocator;
    ++observed.unpack_calls;
    observed.unpacked_length = length;
    if (unpack_failure || !data || !length || data[0] != 0x5au) return NULL;

    request = calloc(1u, sizeof(*request));
    body = calloc(1u, sizeof(*body));
    if (!request || !body) {
        free(request);
        free(body);
        return NULL;
    }
    ++observed.request_allocations;
    ++observed.request_body_allocations;
    request->msg_type = RPC_TYPE__Req;
    request->msg_id = RPC_ID__Req_BoardDiscovery;
    request->uid = 0x12345678u;
    request->payload_case = RPC__PAYLOAD_REQ_BOARD_DISCOVERY;
    request->req_board_discovery = body;
    return request;
}

void rpc__free_unpacked(Rpc *request, ProtobufCAllocator *allocator)
{
    (void)allocator;
    if (!request) return;
    if (request->req_board_discovery) {
        free(request->req_board_discovery);
        request->req_board_discovery = NULL;
        ++observed.request_body_free_calls;
    }
    free(request);
    ++observed.request_free_calls;
}

void rpc__init(Rpc *message)
{
    memset(message, 0, sizeof(*message));
}

size_t rpc__get_packed_size(const Rpc *message)
{
    ++observed.packed_size_calls;
    if (message->msg_id != RPC_ID__Resp_BoardDiscovery ||
        message->uid != 0x12345678u) return 0;
    return packed_response_size;
}

size_t rpc__pack(const Rpc *message, uint8_t *output)
{
    size_t i;
    if (message->msg_id != RPC_ID__Resp_BoardDiscovery || !output) return 0;
    ++observed.pack_calls;
    for (i = 0; i < packed_response_size; ++i)
        output[i] = (uint8_t)(0xa0u + i);
    return packed_response_size;
}

static int allocate_response(Rpc *response)
{
    RpcRespBoardDiscovery *body;
    uint8_t *snapshot;

    ++observed.response_allocation_attempts;
    if (response_allocation_failure) return 0;
    body = calloc(1u, sizeof(*body));
    if (!body) return 0;
    ++observed.response_child_allocations;
    snapshot = calloc(1u, 4u);
    if (!snapshot) {
        free(body);
        ++observed.response_child_frees;
        return 0;
    }
    ++observed.response_data_allocations;
    body->resp = ESP_OK;
    body->snapshot.data = snapshot;
    body->snapshot.len = 4u;
    response->resp_board_discovery = body;
    return 1;
}

esp_err_t esp_rpc_command_dispatcher(Rpc *request, Rpc *response,
    void *private_data)
{
    (void)private_data;
    ++observed.dispatch_calls;
    observed.dispatched_uid = request->uid;

    if (dispatch_mode == DISPATCH_RESPONSE_ALLOCATION_FAILURE) {
        response_allocation_failure = 1;
        (void)allocate_response(response);
        return ESP_ERR_NO_MEM;
    }
    if (!allocate_response(response)) return ESP_ERR_NO_MEM;
    if (dispatch_mode == DISPATCH_REJECT_WITH_PARTIAL_RESPONSE) return ESP_FAIL;
    return ESP_OK;
}

void esp_rpc_cleanup(Rpc *response)
{
    ++observed.response_cleanup_calls;
    if (!response || response->msg_id != RPC_ID__Resp_BoardDiscovery ||
        !response->resp_board_discovery) return;
    if (response->resp_board_discovery->snapshot.data) {
        free(response->resp_board_discovery->snapshot.data);
        response->resp_board_discovery->snapshot.data = NULL;
        ++observed.response_data_frees;
    }
    free(response->resp_board_discovery);
    response->resp_board_discovery = NULL;
    ++observed.response_child_frees;
}

void *c6_dispatch_outbuf_calloc(size_t count, size_t size)
{
    ++observed.outbuf_alloc_calls;
    if (outbuf_allocation_failure) return NULL;
    void *result = calloc(count, size);
    if (result) ++observed.outbuf_alloc_successes;
    return result;
}

static esp_err_t invoke_handler(ssize_t length, uint8_t **outbuf,
    ssize_t *outlen, const uint8_t *input)
{
    return data_transfer_handler(0x55u, input, length, outbuf, outlen, NULL);
}

static int test_unpack_failure(void)
{
    const uint8_t input[4] = { 0x5au, 0u, 0u, 0u };
    uint8_t *outbuf = NULL;
    ssize_t outlen = -7;

    begin_case("unpack-failure");
    unpack_failure = 1;
    CHECK(invoke_handler((ssize_t)sizeof(input), &outbuf, &outlen, input) == ESP_FAIL,
        "unpack failure returned the wrong status");
    CHECK(observed.unpack_calls == 1u && observed.unpacked_length == sizeof(input),
        "unpack failure did not call the protobuf boundary exactly once");
    CHECK(observed.request_allocations == 0u &&
        observed.request_free_calls == 0u && observed.dispatch_calls == 0u,
        "unpack failure created or freed a request unexpectedly");
    CHECK(observed.response_cleanup_calls == 0u &&
        observed.outbuf_alloc_calls == 0u && observed.pack_calls == 0u,
        "unpack failure reached response handling");
    CHECK(outbuf == NULL && outlen == -7,
        "unpack failure changed caller output values");
    return 1;
}

static int test_dispatch_rejection(void)
{
    const uint8_t input[4] = { 0x5au, 0u, 0u, 0u };
    uint8_t *outbuf = NULL;
    ssize_t outlen = -7;

    begin_case("dispatch-rejection-with-partial-response");
    dispatch_mode = DISPATCH_REJECT_WITH_PARTIAL_RESPONSE;
    CHECK(invoke_handler((ssize_t)sizeof(input), &outbuf, &outlen, input) == ESP_FAIL,
        "dispatcher rejection returned the wrong status");
    CHECK(observed.request_allocations == 1u &&
        observed.request_body_allocations == 1u &&
        observed.request_free_calls == 1u &&
        observed.request_body_free_calls == 1u,
        "dispatcher rejection did not free the unpacked request exactly once");
    CHECK(observed.dispatch_calls == 1u &&
        observed.dispatched_uid == 0x12345678u,
        "dispatcher did not receive the unpacked request");
    CHECK(observed.response_cleanup_calls == 1u &&
        observed.response_child_allocations == 1u &&
        observed.response_data_allocations == 1u &&
        observed.response_child_frees == 1u &&
        observed.response_data_frees == 1u,
        "dispatcher rejection did not clean partial response ownership exactly once");
    CHECK(observed.packed_size_calls == 0u && observed.outbuf_alloc_calls == 0u &&
        observed.pack_calls == 0u && outbuf == NULL && outlen == -7,
        "dispatcher rejection continued into response serialization");
    return 1;
}

static int test_response_allocation_failure(void)
{
    const uint8_t input[4] = { 0x5au, 0u, 0u, 0u };
    uint8_t *outbuf = NULL;
    ssize_t outlen = -7;

    begin_case("dispatcher-response-allocation-failure");
    dispatch_mode = DISPATCH_RESPONSE_ALLOCATION_FAILURE;
    CHECK(invoke_handler((ssize_t)sizeof(input), &outbuf, &outlen, input) == ESP_FAIL,
        "response allocation failure returned the wrong handler status");
    CHECK(observed.request_free_calls == 1u &&
        observed.request_body_free_calls == 1u,
        "response allocation failure did not free the request exactly once");
    CHECK(observed.response_allocation_attempts == 1u &&
        observed.response_child_allocations == 0u &&
        observed.response_child_frees == 0u &&
        observed.response_cleanup_calls == 1u,
        "response allocation failure ownership accounting is wrong");
    CHECK(observed.outbuf_alloc_calls == 0u && observed.pack_calls == 0u &&
        outbuf == NULL && outlen == -7,
        "response allocation failure reached output serialization");
    return 1;
}

static int test_outbuf_allocation_failure(void)
{
    const uint8_t input[4] = { 0x5au, 0u, 0u, 0u };
    uint8_t *outbuf = NULL;
    ssize_t outlen = -7;

    begin_case("outbuf-allocation-failure");
    outbuf_allocation_failure = 1;
    CHECK(invoke_handler((ssize_t)sizeof(input), &outbuf, &outlen, input) == ESP_ERR_NO_MEM,
        "outbuf allocation failure returned the wrong status");
    CHECK(observed.request_free_calls == 1u &&
        observed.request_body_free_calls == 1u,
        "outbuf allocation failure freed the request incorrectly");
    CHECK(observed.response_cleanup_calls == 1u &&
        observed.response_child_allocations == 1u &&
        observed.response_child_frees == 1u &&
        observed.response_data_allocations == 1u &&
        observed.response_data_frees == 1u,
        "outbuf allocation failure did not clean response ownership exactly once");
    CHECK(observed.packed_size_calls == 1u && observed.outbuf_alloc_calls == 1u &&
        observed.outbuf_alloc_successes == 0u && observed.pack_calls == 0u,
        "outbuf failure reached the wrong serialization stages");
    CHECK(outbuf == NULL && outlen == (ssize_t)packed_response_size,
        "outbuf failure output values are wrong");
    return 1;
}

static int test_normal_success(void)
{
    const uint8_t input[4] = { 0x5au, 0u, 0u, 0u };
    uint8_t *outbuf = NULL;
    ssize_t outlen = -7;
    size_t i;

    begin_case("normal-success");
    CHECK(invoke_handler((ssize_t)sizeof(input), &outbuf, &outlen, input) == ESP_OK,
        "normal handler call failed");
    CHECK(observed.request_free_calls == 1u &&
        observed.request_body_free_calls == 1u,
        "normal path did not free the request exactly once");
    CHECK(observed.dispatch_calls == 1u &&
        observed.dispatched_uid == 0x12345678u,
        "normal path dispatched the wrong request");
    CHECK(observed.response_cleanup_calls == 1u &&
        observed.response_child_allocations == 1u &&
        observed.response_child_frees == 1u &&
        observed.response_data_allocations == 1u &&
        observed.response_data_frees == 1u,
        "normal path did not clean response ownership exactly once");
    CHECK(observed.packed_size_calls == 1u && observed.outbuf_alloc_calls == 1u &&
        observed.outbuf_alloc_successes == 1u && observed.pack_calls == 1u,
        "normal path serialization call counts are wrong");
    CHECK(outbuf != NULL && outlen == (ssize_t)packed_response_size,
        "normal path did not return a packed output buffer");
    for (i = 0; i < packed_response_size; ++i)
        CHECK(outbuf[i] == (uint8_t)(0xa0u + i),
            "normal path output does not contain the packed response");
    free(outbuf);
    return 1;
}

static int check_bound(ssize_t length, esp_err_t expected_status,
    unsigned expected_unpack_calls, const char *name)
{
    uint8_t input[ESP_SERIAL_REASSEMBLY_MAX + 1u] = { 0x5au };
    uint8_t *outbuf = NULL;
    ssize_t outlen = -9;

    begin_case(name);
    unpack_failure = 1;
    CHECK(invoke_handler(length, &outbuf, &outlen, input) == expected_status,
        "length returned the wrong status");
    CHECK(observed.unpack_calls == expected_unpack_calls,
        "length crossed the protobuf boundary unexpectedly");
    CHECK(observed.request_allocations == 0u && observed.request_free_calls == 0u &&
        observed.dispatch_calls == 0u && observed.response_cleanup_calls == 0u,
        "length boundary test entered request or response ownership");
    CHECK(outbuf == NULL && outlen == -9,
        "length boundary test changed caller output values");
    if (expected_unpack_calls)
        CHECK(observed.unpacked_length == (size_t)length,
            "accepted maximum length changed at protobuf boundary");
    return 1;
}

static int test_serial_reassembly_bounds(void)
{
    CHECK(check_bound(0, ESP_ERR_INVALID_ARG, 0u, "zero-length-input"),
        "zero-length bound failed");
    CHECK(check_bound(-1, ESP_ERR_INVALID_ARG, 0u, "negative-length-input"),
        "negative-length bound failed");
    CHECK(check_bound((ssize_t)ESP_SERIAL_REASSEMBLY_MAX, ESP_FAIL, 1u,
        "maximum-4096-byte-input"), "maximum length bound failed");
    CHECK(check_bound((ssize_t)ESP_SERIAL_REASSEMBLY_MAX + 1, ESP_ERR_INVALID_ARG,
        0u, "over-maximum-4097-byte-input"), "oversized length bound failed");
    return 1;
}

typedef int (*test_fn)(void);

static void run_case(const char *name, test_fn test)
{
    (void)name;
    (void)test();
}

int main(void)
{
    run_case("unpack-failure", test_unpack_failure);
    run_case("dispatch-rejection", test_dispatch_rejection);
    run_case("response-allocation-failure", test_response_allocation_failure);
    run_case("outbuf-allocation-failure", test_outbuf_allocation_failure);
    run_case("normal-success", test_normal_success);
    run_case("serial-reassembly-bounds", test_serial_reassembly_bounds);
    if (failures) {
        fprintf(stderr, "FAIL total: %zu failed assertions across %zu checks\n",
            failures, checks_run);
        return 1;
    }
    printf("PASS: %zu dispatch cleanup checks across %zu cases; "
        "request and response ownership counters balanced\n", checks_run, cases_run);
    return 0;
}

#endif
