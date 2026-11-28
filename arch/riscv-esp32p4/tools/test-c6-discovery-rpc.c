/*
 * Host tests for the production C6 discovery RPC handler and wire codecs.
 * The inventory collector is the only stub: no ESP-IDF API or target hardware
 * is linked into this executable.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpc_discovery.h"
#include "discovery_wire.h"
#include "hosted_serial.h"
#include "hosted_wire.h"

/* The supplied minimal esp_err.h fixture omits this standard IDF error. */
#ifndef ESP_ERR_NO_MEM
#define ESP_ERR_NO_MEM 0x101
#endif

_Static_assert(BOARD_DISCOVERY_WIRE_MAX == 1064u,
    "maximum C6DI payload must hold one header plus 32 records");
_Static_assert(HOSTED_SERIAL_MESSAGE_OVERHEAD == 12u,
    "RPCRsp serial wrapper overhead changed");
_Static_assert(P4_HOSTED_HEADER_SIZE == 12u,
    "ESP-Hosted SDIO frame header changed");

#define MAX_TRACKED_ALLOCATIONS 32u
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

struct tracked_allocation {
    void *pointer;
    size_t size;
    int active;
};

static struct tracked_allocation allocations[MAX_TRACKED_ALLOCATIONS];
static size_t allocation_calls;
static size_t allocation_fail_call;
static size_t allocation_total;
static size_t free_total;
static size_t double_free_total;
static size_t foreign_free_total;
static const char *active_case = "setup";
static size_t checks_run;
static size_t cases_run;
static size_t failures;
static unsigned collector_calls;

enum collector_result {
    COLLECTOR_SUCCESS,
    COLLECTOR_FAILURE,
    COLLECTOR_INVALID_INVENTORY
};

static enum collector_result collector_result = COLLECTOR_SUCCESS;

static size_t live_allocations(void)
{
    size_t i, count = 0;
    for (i = 0; i < ARRAY_COUNT(allocations); ++i)
        if (allocations[i].active) ++count;
    return count;
}

static void track_allocation(void *pointer, size_t size)
{
    size_t i;
    if (!pointer) return;
    for (i = 0; i < ARRAY_COUNT(allocations); ++i) {
        if (!allocations[i].active) {
            allocations[i].pointer = pointer;
            allocations[i].size = size;
            allocations[i].active = 1;
            ++allocation_total;
            return;
        }
    }
    fprintf(stderr, "FAIL %s: allocation tracker is full\n", active_case);
    abort();
}

/* These three functions are substituted only while compiling rpc_discovery.c. */
void *c6_test_malloc(size_t size)
{
    void *pointer;
    ++allocation_calls;
    if (allocation_fail_call && allocation_calls == allocation_fail_call)
        return NULL;
    pointer = malloc(size);
    track_allocation(pointer, size);
    return pointer;
}

void *c6_test_calloc(size_t count, size_t size)
{
    void *pointer;
    ++allocation_calls;
    if (allocation_fail_call && allocation_calls == allocation_fail_call)
        return NULL;
    pointer = calloc(count, size);
    track_allocation(pointer, count * size);
    return pointer;
}

void c6_test_free(void *pointer)
{
    size_t i;
    if (pointer) {
        for (i = 0; i < ARRAY_COUNT(allocations); ++i) {
            if (allocations[i].pointer == pointer) {
                if (!allocations[i].active) {
                    ++double_free_total;
                    break;
                }
                allocations[i].active = 0;
                ++free_total;
                free(pointer);
                return;
            }
        }
        if (i == ARRAY_COUNT(allocations)) ++foreign_free_total;
    }
    free(pointer);
}

static void set_allocation_failure(size_t call_number)
{
    allocation_calls = 0;
    allocation_fail_call = call_number;
}

static int fail(const char *message)
{
    fprintf(stderr, "FAIL %s: %s\n", active_case, message);
    ++failures;
    return 0;
}

#define CHECK(condition, message) do { \
    ++checks_run; \
    if (!(condition)) return fail(message); \
} while (0)

static void begin_case(const char *name)
{
    active_case = name;
    ++cases_run;
}

static void fill_valid_snapshot(struct board_discovery_snapshot *snapshot)
{
    size_t i;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->physical_bytes = 0x01000000u;
    snapshot->layout.flash_bytes = 0x00800000u;
    snapshot->layout.count = BOARD_LAYOUT_MAX_PARTITIONS;
    snapshot->running = 0;
    snapshot->boot = 1;
    snapshot->update = 0;
    snapshot->running_state = 3;
    snapshot->security_known = BOARD_DISCOVERY_SECURITY_ALL;
    snapshot->security_enabled = BOARD_DISCOVERY_SECURITY_ENCRYPTION |
        BOARD_DISCOVERY_SECURITY_ANTI_ROLLBACK;
    snapshot->target_known = BOARD_DISCOVERY_TARGET_ALL;
    snapshot->chip_id = BOARD_DISCOVERY_CHIP_C6;
    snapshot->chip_revision = 0x1234u;
    snapshot->secure_version = 0x89abcdefu;

    for (i = 0; i < snapshot->layout.count; ++i) {
        struct board_partition *part = &snapshot->layout.partitions[i];
        uint32_t offset;

        if (i == 0) offset = 0x00010000u;
        else if (i == 1) offset = 0x00020000u;
        else if (i == 2) offset = 0x00030000u;
        else offset = 0x00031000u + (uint32_t)(i - 3u) * 0x1000u;

        part->offset = offset;
        part->size = 0x1000u;
        part->flags = (uint32_t)(i & 3u);
        if (i < 3) {
            part->type = 0;
            part->subtype = (uint8_t)(0x10u + i);
        } else {
            part->type = 1;
            part->subtype = 2;
        }
        (void)snprintf(part->label, sizeof(part->label), "part%02u", (unsigned)i);
    }
}

/* Minimal substitute for the IDF-backed inventory adapter. */
int c6_board_discovery_collect(struct board_discovery_snapshot *output)
{
    ++collector_calls;
    if (collector_result == COLLECTOR_FAILURE) return 0;
    if (collector_result == COLLECTOR_INVALID_INVENTORY) {
        memset(output, 0, sizeof(*output));
        return 1;
    }
    fill_valid_snapshot(output);
    return 1;
}

static void init_request(Rpc *request, RpcReqBoardDiscovery *payload,
    RpcType type, RpcId id, uint32_t uid, Rpc__PayloadCase payload_case,
    uint32_t version, uint32_t nonce)
{
    rpc__init(request);
    rpc__req__board_discovery__init(payload);
    payload->version = version;
    payload->nonce = nonce;
    request->msg_type = type;
    request->msg_id = id;
    request->uid = uid;
    request->payload_case = payload_case;
    request->req_board_discovery = payload;
}

static void init_response(Rpc *response, uint32_t uid)
{
    rpc__init(response);
    response->msg_type = RPC_TYPE__Resp;
    response->msg_id = RPC_ID__Resp_BoardDiscovery;
    response->uid = uid;
    response->payload_case = RPC__PAYLOAD_RESP_BOARD_DISCOVERY;
}

static int reject_preflight(Rpc *request, Rpc *response,
    RpcRespBoardDiscovery *expected_child)
{
    unsigned calls_before = collector_calls;
    set_allocation_failure(0);
    CHECK(c6_rpc_board_discovery(request, response, NULL) == ESP_ERR_INVALID_ARG,
        "invalid envelope was not rejected");
    CHECK(!response || response->resp_board_discovery == expected_child,
        "invalid envelope changed response ownership");
    CHECK(allocation_calls == 0 && live_allocations() == 0,
        "invalid envelope allocated handler memory");
    CHECK(collector_calls == calls_before,
        "invalid envelope reached the inventory collector");
    return 1;
}

static int test_envelope_preflight(void)
{
    RpcReqBoardDiscovery body;
    Rpc request, response;
    RpcRespBoardDiscovery occupied;
    RpcReqBoardDiscovery *saved_request_body;

    begin_case("envelope-preflight-type-id-uid-payload-and-output");
    init_request(&request, &body, RPC_TYPE__Req,
        RPC_ID__Req_BoardDiscovery, 7u, RPC__PAYLOAD_REQ_BOARD_DISCOVERY,
        BOARD_DISCOVERY_WIRE_VERSION, 0x12345678u);
    init_response(&response, request.uid);

    request.msg_type = RPC_TYPE__Event;
    CHECK(reject_preflight(&request, &response, NULL), "request type case failed");
    init_request(&request, &body, RPC_TYPE__Req,
        RPC_ID__Req_BoardDiscovery, 7u, RPC__PAYLOAD_REQ_BOARD_DISCOVERY,
        BOARD_DISCOVERY_WIRE_VERSION, 0x12345678u);
    request.msg_id = RPC_ID__Req_Base;
    CHECK(reject_preflight(&request, &response, NULL), "request id case failed");
    init_request(&request, &body, RPC_TYPE__Req,
        RPC_ID__Req_BoardDiscovery, 0u, RPC__PAYLOAD_REQ_BOARD_DISCOVERY,
        BOARD_DISCOVERY_WIRE_VERSION, 0x12345678u);
    CHECK(reject_preflight(&request, &response, NULL), "zero uid case failed");
    init_request(&request, &body, RPC_TYPE__Req,
        RPC_ID__Req_BoardDiscovery, 7u, RPC__PAYLOAD_REQ_BOARD_DISCOVERY,
        BOARD_DISCOVERY_WIRE_VERSION, 0x12345678u);
    request.payload_case = (Rpc__PayloadCase)(RPC__PAYLOAD_REQ_BOARD_DISCOVERY - 1);
    CHECK(reject_preflight(&request, &response, NULL), "request payload case failed");
    init_request(&request, &body, RPC_TYPE__Req,
        RPC_ID__Req_BoardDiscovery, 7u, RPC__PAYLOAD_REQ_BOARD_DISCOVERY,
        BOARD_DISCOVERY_WIRE_VERSION, 0x12345678u);
    saved_request_body = request.req_board_discovery;
    request.req_board_discovery = NULL;
    CHECK(reject_preflight(&request, &response, NULL), "null request body case failed");
    request.req_board_discovery = saved_request_body;

    init_request(&request, &body, RPC_TYPE__Req,
        RPC_ID__Req_BoardDiscovery, 7u, RPC__PAYLOAD_REQ_BOARD_DISCOVERY,
        BOARD_DISCOVERY_WIRE_VERSION, 0x12345678u);
    response.msg_type = RPC_TYPE__Event;
    CHECK(reject_preflight(&request, &response, NULL), "response type case failed");
    init_response(&response, request.uid);
    response.msg_id = RPC_ID__Resp_Base;
    CHECK(reject_preflight(&request, &response, NULL), "response id case failed");
    init_response(&response, request.uid);
    response.uid += 1u;
    CHECK(reject_preflight(&request, &response, NULL), "response uid case failed");
    init_response(&response, request.uid);
    response.payload_case = (Rpc__PayloadCase)(RPC__PAYLOAD_RESP_BOARD_DISCOVERY - 1);
    CHECK(reject_preflight(&request, &response, NULL), "response payload case failed");
    init_response(&response, request.uid);
    rpc__resp__board_discovery__init(&occupied);
    response.resp_board_discovery = &occupied;
    CHECK(reject_preflight(&request, &response, &occupied),
        "pre-owned response case failed");

    CHECK(c6_rpc_board_discovery(NULL, &response, NULL) == ESP_ERR_INVALID_ARG,
        "null request was not rejected");
    CHECK(c6_rpc_board_discovery(&request, NULL, NULL) == ESP_ERR_INVALID_ARG,
        "null response was not rejected");
    CHECK(live_allocations() == 0, "preflight test leaked handler memory");
    return 1;
}

static int check_error_response(uint32_t version, uint32_t nonce,
    int expected_error, const char *name)
{
    RpcReqBoardDiscovery body;
    Rpc request, response;
    unsigned calls_before = collector_calls;
    size_t allocations_before = allocation_total;
    size_t frees_before = free_total;

    begin_case(name);
    init_request(&request, &body, RPC_TYPE__Req,
        RPC_ID__Req_BoardDiscovery, 0x10203040u,
        RPC__PAYLOAD_REQ_BOARD_DISCOVERY, version, nonce);
    init_response(&response, request.uid);
    set_allocation_failure(0);
    CHECK(c6_rpc_board_discovery(&request, &response, NULL) == ESP_OK,
        "well-formed envelope did not receive an RPC-level error");
    CHECK(response.resp_board_discovery != NULL,
        "error response child was not allocated");
    CHECK(response.resp_board_discovery->resp == expected_error,
        "unexpected response error value");
    CHECK(response.resp_board_discovery->snapshot.data == NULL &&
        response.resp_board_discovery->snapshot.len == 0,
        "rejected request returned snapshot bytes");
    CHECK(collector_calls == calls_before,
        "bad version or nonce reached the inventory collector");
    CHECK(allocation_total == allocations_before + 1u && live_allocations() == 1u,
        "error response ownership is incorrect");
    c6_rpc_board_discovery_cleanup(&response);
    CHECK(live_allocations() == 0 && free_total == frees_before + 1u,
        "error response cleanup leaked or freed the wrong number of objects");
    return 1;
}

static int test_version_and_nonce_refusal(void)
{
    CHECK(check_error_response(BOARD_DISCOVERY_WIRE_VERSION + 1u,
        0x12345678u, ESP_ERR_INVALID_ARG, "unsupported-version"),
        "unsupported version case failed");
    CHECK(check_error_response(BOARD_DISCOVERY_WIRE_VERSION, 0u,
        ESP_ERR_INVALID_ARG, "zero-nonce"), "zero nonce case failed");
    return 1;
}

static int test_allocation_failures(void)
{
    size_t fail_at;
    for (fail_at = 1u; fail_at <= 3u; ++fail_at) {
        RpcReqBoardDiscovery body;
        Rpc request, response;
        size_t allocations_before = allocation_total;
        size_t frees_before = free_total;
        unsigned calls_before = collector_calls;
        esp_err_t result;

        begin_case(fail_at == 1u ? "allocation-failure-response" :
            (fail_at == 2u ? "allocation-failure-snapshot" :
                             "allocation-failure-wire-buffer"));
        init_request(&request, &body, RPC_TYPE__Req,
            RPC_ID__Req_BoardDiscovery, 9u,
            RPC__PAYLOAD_REQ_BOARD_DISCOVERY, BOARD_DISCOVERY_WIRE_VERSION,
            0x87654321u);
        init_response(&response, request.uid);
        set_allocation_failure(fail_at);
        result = c6_rpc_board_discovery(&request, &response, NULL);
        CHECK(allocation_calls == (fail_at == 1u ? 1u : 3u),
            "handler allocation count changed");
        CHECK(collector_calls == calls_before,
            "allocation failure reached the inventory collector");
        CHECK(allocation_total - allocations_before ==
            (fail_at == 1u ? 0u : 2u), "unexpected successful allocations");

        if (fail_at == 1u) {
            CHECK(result == ESP_ERR_NO_MEM,
                "response allocation failure returned wrong status");
            CHECK(response.resp_board_discovery == NULL,
                "failed response allocation left a child pointer");
            CHECK(live_allocations() == 0 && free_total == frees_before,
                "response allocation failure leaked or freed memory");
        } else {
            CHECK(result == ESP_OK,
                "nested allocation failure did not return an RPC response");
            CHECK(response.resp_board_discovery != NULL &&
                response.resp_board_discovery->resp == ESP_ERR_NO_MEM,
                "nested allocation failure response status is wrong");
            CHECK(response.resp_board_discovery->snapshot.data == NULL &&
                response.resp_board_discovery->snapshot.len == 0,
                "nested allocation failure exposed snapshot data");
            CHECK(live_allocations() == 1u,
                "nested allocation failure retained unexpected allocations");
            c6_rpc_board_discovery_cleanup(&response);
            CHECK(live_allocations() == 0 &&
                free_total - frees_before == 2u,
                "nested allocation cleanup did not release all handler memory");
        }
    }
    CHECK(double_free_total == 0 && foreign_free_total == 0,
        "allocation failures triggered an invalid or duplicate free");
    return 1;
}

static int test_collector_failures(void)
{
    size_t mode;
    for (mode = COLLECTOR_FAILURE; mode <= COLLECTOR_INVALID_INVENTORY; ++mode) {
        RpcReqBoardDiscovery body;
        Rpc request, response;
        size_t frees_before = free_total;
        size_t allocations_before = allocation_total;
        unsigned calls_before = collector_calls;

        begin_case(mode == COLLECTOR_FAILURE ? "collector-failure" :
            "invalid-collected-inventory");
        init_request(&request, &body, RPC_TYPE__Req,
            RPC_ID__Req_BoardDiscovery, 10u,
            RPC__PAYLOAD_REQ_BOARD_DISCOVERY, BOARD_DISCOVERY_WIRE_VERSION,
            0xaabbccddu);
        init_response(&response, request.uid);
        collector_result = (enum collector_result)mode;
        set_allocation_failure(0);
        CHECK(c6_rpc_board_discovery(&request, &response, NULL) == ESP_OK,
            "collector failure did not return an RPC response");
        CHECK(collector_calls == calls_before + 1u,
            "collector was not called exactly once");
        CHECK(response.resp_board_discovery != NULL &&
            response.resp_board_discovery->resp == ESP_FAIL,
            "collector failure or invalid inventory did not map to ESP_FAIL");
        CHECK(response.resp_board_discovery->snapshot.data == NULL &&
            response.resp_board_discovery->snapshot.len == 0,
            "collector failure exposed snapshot data");
        CHECK(allocation_total - allocations_before == 3u &&
            free_total - frees_before == 2u && live_allocations() == 1u,
            "collector failure ownership accounting is wrong");
        c6_rpc_board_discovery_cleanup(&response);
        CHECK(live_allocations() == 0 && free_total - frees_before == 3u,
            "collector failure cleanup leaked handler memory");
    }
    collector_result = COLLECTOR_SUCCESS;
    CHECK(double_free_total == 0 && foreign_free_total == 0,
        "collector failures triggered an invalid or duplicate free");
    return 1;
}

static int same_snapshot(const struct board_discovery_snapshot *left,
    const struct board_discovery_snapshot *right)
{
    size_t i;
    if (left->physical_bytes != right->physical_bytes ||
        left->layout.flash_bytes != right->layout.flash_bytes ||
        left->layout.count != right->layout.count ||
        left->layout.md5_record_present != right->layout.md5_record_present ||
        left->running != right->running || left->boot != right->boot ||
        left->update != right->update ||
        left->running_state != right->running_state ||
        left->security_known != right->security_known ||
        left->security_enabled != right->security_enabled ||
        left->target_known != right->target_known ||
        left->chip_id != right->chip_id ||
        left->chip_revision != right->chip_revision ||
        left->secure_version != right->secure_version) return 0;
    for (i = 0; i < left->layout.count; ++i) {
        const struct board_partition *a = &left->layout.partitions[i];
        const struct board_partition *b = &right->layout.partitions[i];
        if (a->offset != b->offset || a->size != b->size ||
            a->flags != b->flags || a->type != b->type ||
            a->subtype != b->subtype ||
            memcmp(a->label, b->label, sizeof(a->label)) != 0) return 0;
    }
    return 1;
}

static int test_protobuf_roundtrip_maximum_response(void)
{
    const uint32_t uid = 0x89abcdefu;
    const uint32_t nonce = 0x76543210u;
    RpcReqBoardDiscovery request_body;
    Rpc request, response;
    Rpc *unpacked_request = NULL, *unpacked_response = NULL;
    struct board_discovery_snapshot expected, decoded;
    uint8_t request_wire[64];
    uint8_t response_wire[BOARD_DISCOVERY_WIRE_MAX + 64u];
    uint8_t serial_wire[HOSTED_SERIAL_MESSAGE_OVERHEAD +
        BOARD_DISCOVERY_WIRE_MAX + 64u];
    uint8_t sdio_frame[P4_HOSTED_FRAME_MAX];
    size_t request_size, response_size, serial_size, frame_size;
    size_t allocation_before = allocation_total, free_before = free_total;
    struct hosted_serial_message serial_message;
    struct hosted_serial_message decoded_serial;
    struct p4_hosted_frame host_frame, decoded_frame;
    unsigned calls_before = collector_calls;

    begin_case("protobuf-request-response-maximum-rpcrsp-sdio-envelope");
    fill_valid_snapshot(&expected);
    CHECK(board_discovery_validate(&expected),
        "maximum fixture inventory is invalid");
    CHECK(expected.layout.count == BOARD_LAYOUT_MAX_PARTITIONS,
        "fixture does not use the maximum partition count");

    init_request(&request, &request_body, RPC_TYPE__Req,
        RPC_ID__Req_BoardDiscovery, uid, RPC__PAYLOAD_REQ_BOARD_DISCOVERY,
        BOARD_DISCOVERY_WIRE_VERSION, nonce);
    request_size = rpc__get_packed_size(&request);
    CHECK(request_size <= sizeof(request_wire),
        "packed request exceeded its bounded test buffer");
    CHECK(rpc__pack(&request, request_wire) == request_size,
        "generated protobuf request pack length changed");
    unpacked_request = rpc__unpack(NULL, request_size, request_wire);
    CHECK(unpacked_request != NULL,
        "generated protobuf request could not be unpacked");
    CHECK(unpacked_request->msg_type == RPC_TYPE__Req &&
        unpacked_request->msg_id == RPC_ID__Req_BoardDiscovery &&
        unpacked_request->uid == uid &&
        unpacked_request->payload_case == RPC__PAYLOAD_REQ_BOARD_DISCOVERY,
        "request envelope fields changed through protobuf roundtrip");
    CHECK(unpacked_request->req_board_discovery->version ==
        BOARD_DISCOVERY_WIRE_VERSION &&
        unpacked_request->req_board_discovery->nonce == nonce,
        "request version or nonce changed through protobuf roundtrip");

    init_response(&response, unpacked_request->uid);
    collector_result = COLLECTOR_SUCCESS;
    set_allocation_failure(0);
    CHECK(c6_rpc_board_discovery(unpacked_request, &response, NULL) == ESP_OK,
        "production handler rejected a protobuf-decoded request");
    CHECK(collector_calls == calls_before + 1u,
        "successful RPC did not call the collector exactly once");
    CHECK(response.resp_board_discovery != NULL &&
        response.resp_board_discovery->resp == ESP_OK,
        "successful RPC response status is wrong");
    CHECK(response.resp_board_discovery->snapshot.len ==
        BOARD_DISCOVERY_WIRE_MAX,
        "maximum inventory did not produce the maximum C6DI wire payload");
    CHECK(board_discovery_decode(response.resp_board_discovery->snapshot.data,
        response.resp_board_discovery->snapshot.len, nonce, &decoded),
        "handler snapshot did not decode with the request nonce");
    CHECK(same_snapshot(&expected, &decoded),
        "handler snapshot changed maximum inventory fields");

    response_size = rpc__get_packed_size(&response);
    CHECK(response_size <= sizeof(response_wire),
        "packed response envelope exceeded its bounded test buffer");
    CHECK(rpc__pack(&response, response_wire) == response_size,
        "generated protobuf response pack length changed");
    unpacked_response = rpc__unpack(NULL, response_size, response_wire);
    CHECK(unpacked_response != NULL,
        "generated protobuf response could not be unpacked");
    CHECK(unpacked_response->msg_type == RPC_TYPE__Resp &&
        unpacked_response->msg_id == RPC_ID__Resp_BoardDiscovery &&
        unpacked_response->uid == uid &&
        unpacked_response->payload_case == RPC__PAYLOAD_RESP_BOARD_DISCOVERY,
        "response type, id, UID or payload case changed through protobuf");
    CHECK(unpacked_response->resp_board_discovery->resp == ESP_OK &&
        unpacked_response->resp_board_discovery->snapshot.len ==
            BOARD_DISCOVERY_WIRE_MAX,
        "unpacked protobuf response has wrong result or snapshot length");
    CHECK(board_discovery_decode(
        unpacked_response->resp_board_discovery->snapshot.data,
        unpacked_response->resp_board_discovery->snapshot.len, nonce, &decoded),
        "protobuf response snapshot did not decode with the request nonce");
    CHECK(same_snapshot(&expected, &decoded),
        "protobuf response snapshot changed maximum inventory fields");

    serial_message.endpoint = HOSTED_SERIAL_ENDPOINT_RPCRSP;
    serial_message.protobuf = response_wire;
    serial_message.protobuf_size = response_size;
    serial_message.wire_size = 0;
    CHECK(hosted_serial_encode(serial_wire, sizeof(serial_wire),
        &serial_message, &serial_size),
        "actual RPCRsp serial wrapper rejected the maximum response");
    CHECK(serial_size == HOSTED_SERIAL_MESSAGE_OVERHEAD + response_size,
        "RPCRsp wrapper size is not protobuf plus its real TLV overhead");
    CHECK(hosted_serial_decode(serial_wire, serial_size, &decoded_serial),
        "actual RPCRsp serial wrapper did not decode");
    CHECK(decoded_serial.endpoint == HOSTED_SERIAL_ENDPOINT_RPCRSP &&
        decoded_serial.protobuf_size == response_size &&
        memcmp(decoded_serial.protobuf, response_wire, response_size) == 0,
        "RPCRsp wrapper changed the packed protobuf envelope");

    memset(&host_frame, 0, sizeof(host_frame));
    host_frame.interface_type = P4_HOSTED_SERIAL;
    host_frame.payload = serial_wire;
    host_frame.payload_size = serial_size;
    CHECK(p4_hosted_encode(sdio_frame, sizeof(sdio_frame), 0u,
        &host_frame, &frame_size),
        "actual 12-byte SDIO frame encoder rejected the maximum response");
    CHECK(frame_size == P4_HOSTED_HEADER_SIZE + serial_size,
        "SDIO frame size is not serial payload plus its real header");
    CHECK(frame_size == response_size + HOSTED_SERIAL_MESSAGE_OVERHEAD +
        P4_HOSTED_HEADER_SIZE,
        "full envelope omitted a wrapper or the SDIO header");
    CHECK(frame_size == 1106u,
        "maximum generated RPC plus serial/SDIO wrappers is not 1106 bytes");
    CHECK(frame_size <= P4_HOSTED_FRAME_MAX,
        "maximum response exceeds the 1536-byte Hosted frame limit");
    CHECK(p4_hosted_decode(sdio_frame, frame_size, 0u, &decoded_frame),
        "actual SDIO frame encoder output did not decode");
    CHECK(decoded_frame.interface_type == P4_HOSTED_SERIAL &&
        decoded_frame.payload_size == serial_size &&
        memcmp(decoded_frame.payload, serial_wire, serial_size) == 0,
        "SDIO frame changed the RPCRsp wrapper bytes");

    CHECK(allocation_total - allocation_before == 3u &&
        free_total - free_before == 1u && live_allocations() == 2u,
        "successful handler response allocation ownership is wrong");
    rpc__free_unpacked(unpacked_request, NULL);
    unpacked_request = NULL;
    c6_rpc_board_discovery_cleanup(&response);
    CHECK(live_allocations() == 0 && free_total - free_before == 3u,
        "successful response cleanup did not release its reply and payload");
    rpc__free_unpacked(unpacked_response, NULL);
    unpacked_response = NULL;

    CHECK(double_free_total == 0 && foreign_free_total == 0,
        "protobuf roundtrip triggered an invalid or duplicate handler free");
    return 1;
}

static int test_cleanup_guards_and_idempotence(void)
{
    RpcReqBoardDiscovery body;
    Rpc request, response;
    RpcRespBoardDiscovery *owned;
    size_t frees_before = free_total;

    begin_case("cleanup-wrong-case-and-idempotence");
    init_request(&request, &body, RPC_TYPE__Req,
        RPC_ID__Req_BoardDiscovery, 11u,
        RPC__PAYLOAD_REQ_BOARD_DISCOVERY, BOARD_DISCOVERY_WIRE_VERSION,
        0x11223344u);
    init_response(&response, request.uid);
    collector_result = COLLECTOR_SUCCESS;
    set_allocation_failure(0);
    CHECK(c6_rpc_board_discovery(&request, &response, NULL) == ESP_OK,
        "setup discovery request failed");
    owned = response.resp_board_discovery;
    CHECK(owned != NULL && owned->snapshot.data != NULL,
        "setup response did not own payload memory");

    c6_rpc_board_discovery_cleanup(NULL);
    response.msg_id = RPC_ID__Resp_Base;
    c6_rpc_board_discovery_cleanup(&response);
    CHECK(response.resp_board_discovery == owned &&
        live_allocations() == 2u && free_total == frees_before + 1u,
        "wrong response id cleanup freed owned memory");
    response.msg_id = RPC_ID__Resp_BoardDiscovery;
    response.payload_case = (Rpc__PayloadCase)(
        RPC__PAYLOAD_RESP_BOARD_DISCOVERY - 1);
    c6_rpc_board_discovery_cleanup(&response);
    CHECK(response.resp_board_discovery == owned &&
        live_allocations() == 2u && free_total == frees_before + 1u,
        "wrong payload case cleanup freed owned memory");
    response.payload_case = RPC__PAYLOAD_RESP_BOARD_DISCOVERY;
    c6_rpc_board_discovery_cleanup(&response);
    CHECK(response.resp_board_discovery == NULL && live_allocations() == 0u &&
        free_total == frees_before + 3u,
        "valid cleanup did not release nested data and response child");
    c6_rpc_board_discovery_cleanup(&response);
    CHECK(live_allocations() == 0u && free_total == frees_before + 3u,
        "repeated cleanup was not idempotent");
    CHECK(double_free_total == 0 && foreign_free_total == 0,
        "cleanup guards triggered an invalid or duplicate free");
    return 1;
}

int main(int argc, char **argv)
{
    const char *optimization = argc > 1 ? argv[1] : "host";
    if (!test_envelope_preflight() ||
        !test_version_and_nonce_refusal() ||
        !test_allocation_failures() ||
        !test_collector_failures() ||
        !test_protobuf_roundtrip_maximum_response() ||
        !test_cleanup_guards_and_idempotence()) return 1;
    if (live_allocations() != 0 || double_free_total || foreign_free_total) {
        fprintf(stderr, "FAIL final: live=%zu double-free=%zu foreign-free=%zu\n",
            live_allocations(), double_free_total, foreign_free_total);
        return 1;
    }
    printf("PASS %s: %zu RPC discovery checks across %zu cases; "
        "%zu handler allocations, %zu frees\n", optimization, checks_run,
        cases_run, allocation_total, free_total);
    return 0;
}
