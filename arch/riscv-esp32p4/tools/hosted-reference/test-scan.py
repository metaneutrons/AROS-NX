#!/usr/bin/env python3
"""Compile focused host tests from the pinned ESP-Hosted scan RPC source."""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


WRAPPER_SIGNATURE = (
    "int rpc_wifi_scan_get_ap_records(uint16_t *number, "
    "wifi_ap_record_t *ap_records)"
)
PARSER_SIGNATURE = "int rpc_parse_rsp(Rpc *rpc_msg, ctrl_cmd_t *app_resp)"
MAC_CASE = "case RPC_ID__Resp_GetMACAddress"
SCAN_CASE = "case RPC_ID__Resp_WifiScanGetApRecords:"
DEFAULT_CASE = "default: {"


def find_braced_source(
    source: str, start: int, opening_search: int, description: str, path: Path
) -> str:
    opening = source.find("{", opening_search)
    if opening < 0:
        raise ValueError(f"body for {description} not found in {path}")

    closing = find_brace_end(source, opening, description, path)
    return source[start : closing + 1]


def find_brace_end(source: str, opening: int, description: str, path: Path) -> int:
    if opening >= len(source) or source[opening] != "{":
        raise ValueError(f"body for {description} not found in {path}")

    depth = 0
    state = "code"
    escaped = False
    i = opening
    while i < len(source):
        char = source[i]
        following = source[i + 1] if i + 1 < len(source) else ""

        if state == "line-comment":
            if char == "\n":
                state = "code"
        elif state == "block-comment":
            if char == "*" and following == "/":
                state = "code"
                i += 1
        elif state in ("string", "character"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        elif char == "/" and following == "/":
            state = "line-comment"
            i += 1
        elif char == "/" and following == "*":
            state = "block-comment"
            i += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "character"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1

    raise ValueError(f"unterminated body for {description} in {path}")


def extract_function(source: str, signature: str, path: Path) -> str:
    start = source.find(signature)
    if start < 0:
        raise ValueError(f"{signature} not found in {path}")
    return find_braced_source(
        source, start, start + len(signature), signature, path
    )


def extract_function_context(
    source: str, signature: str, switch_marker: str, path: Path
) -> tuple[str, str, str]:
    start = source.find(signature)
    if start < 0:
        raise ValueError(f"{signature} not found in {path}")
    body_open = source.find("{", start + len(signature))
    if body_open < 0:
        raise ValueError(f"body for {signature} not found in {path}")
    function_close = find_brace_end(source, body_open, signature, path)

    switch_start = source.find(switch_marker, body_open + 1, function_close)
    if switch_start < 0:
        raise ValueError(f"{switch_marker} not found in {signature} in {path}")
    switch_open = source.find("{", switch_start + len(switch_marker), function_close)
    if switch_open < 0:
        raise ValueError(f"switch body not found in {signature} in {path}")
    switch_close = find_brace_end(source, switch_open, "response switch", path)

    return (
        source[start:body_open].rstrip(),
        source[body_open + 1 : switch_start],
        source[switch_close + 1 : function_close],
    )


def extract_case(
    source: str, marker: str, path: Path, search_from: int = 0
) -> tuple[int, str]:
    start = source.find(marker, search_from)
    if start < 0:
        raise ValueError(f"{marker} not found in {path}")
    opening_search = start + len(marker)
    if marker.rstrip().endswith("{"):
        opening_search = start + marker.rfind("{")
    return start, find_braced_source(source, start, opening_search, marker, path)


def make_test_source(
    wrapper_source: str,
    parser_signature: str,
    parser_prefix: str,
    parser_suffix: str,
    mac_case: str,
    scan_case: str,
    default_case: str,
) -> str:
    template = r'''#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SUCCESS 0
#define FAILURE (-1)
#define REMOTE_ERROR 77
#define RPC_ERR_PROTOBUF_DECODE 1001
#define RPC_TYPE__Resp 2
#define RPC_TYPE__Req 1
#define RPC_ID__Resp_GetMACAddress 10
#define RPC_ID__Resp_WifiScanGetApRecords 20
#define TRANSPORT_TEST_UNKNOWN_ID 999
#define TAG "rpc-test"
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC2STR(mac) (mac)[0], (mac)[1], (mac)[2], (mac)[3], (mac)[4], (mac)[5]
#define ESP_LOGE(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGI(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGD(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGV(tag, ...) do { (void)(tag); } while (0)

#define WIFI_SCAN_AP_REC_phy_11b_BIT 0
#define WIFI_SCAN_AP_REC_phy_11g_BIT 1
#define WIFI_SCAN_AP_REC_phy_11n_BIT 2
#define WIFI_SCAN_AP_REC_phy_lr_BIT 3
#define WIFI_SCAN_AP_REC_phy_11ax_BIT 4
#define WIFI_SCAN_AP_REC_wps_BIT 5
#define WIFI_SCAN_AP_REC_ftm_responder_BIT 6
#define WIFI_SCAN_AP_REC_ftm_initiator_BIT 7
#define WIFI_HE_AP_INFO_partial_bss_color_BIT 6
#define WIFI_HE_AP_INFO_bss_color_disabled_BIT 7
#define H_GET_BIT(bit, mask) (((mask) >> (bit)) & 1u)
#define WIFI_SCAN_AP_GET_RESERVED_VAL(mask) ((mask) >> 16)

typedef struct {
    size_t len;
    uint8_t *data;
} rpc_bytes_t;

typedef struct {
    rpc_bytes_t cc;
    uint8_t schan;
    uint8_t nchan;
    int8_t max_tx_power;
    uint8_t policy;
} WifiCountry;

typedef struct {
    uint32_t bitmask;
} WifiHeApInfo;

typedef struct WifiApRecord {
    rpc_bytes_t ssid;
    rpc_bytes_t bssid;
    WifiCountry *country;
    WifiHeApInfo *he_ap;
    uint8_t primary;
    uint8_t second;
    int8_t rssi;
    uint8_t authmode;
    uint8_t pairwise_cipher;
    uint8_t group_cipher;
    uint8_t ant;
    uint32_t bitmask;
} WifiApRecord;

typedef struct {
    char cc[2];
    uint8_t schan;
    uint8_t nchan;
    int8_t max_tx_power;
    uint8_t policy;
} wifi_country_t;

typedef struct {
    uint8_t bss_color;
    uint8_t partial_bss_color;
    uint8_t bss_color_disabled;
} wifi_he_ap_info_t;

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    wifi_country_t country;
    wifi_he_ap_info_t he_ap;
    uint8_t primary;
    uint8_t second;
    int8_t rssi;
    uint8_t authmode;
    uint8_t pairwise_cipher;
    uint8_t group_cipher;
    uint8_t ant;
    uint8_t phy_11b;
    uint8_t phy_11g;
    uint8_t phy_11n;
    uint8_t phy_lr;
    uint8_t phy_11ax;
    uint8_t wps;
    uint8_t ftm_responder;
    uint8_t ftm_initiator;
    uint32_t reserved;
} wifi_ap_record_t;

typedef struct {
    uint16_t number;
    wifi_ap_record_t *out_list;
} wifi_scan_ap_list_t;

typedef struct {
    uint8_t mac[6];
} wifi_mac_t;

typedef struct {
    int msg_type;
    int msg_id;
    uint32_t uid;
    int resp_event_status;
    union {
        wifi_scan_ap_list_t wifi_scan_ap_list;
        wifi_mac_t wifi_mac;
    } u;
    void (*app_free_buff_func)(void *);
    void *app_free_buff_hdl;
} ctrl_cmd_t;

typedef struct {
    int resp;
    rpc_bytes_t mac;
} RespGetMacAddress;

typedef struct {
    int resp;
    int32_t number;
    size_t n_ap_records;
    WifiApRecord **ap_records;
} RespWifiScanGetApRecords;

typedef struct {
    int msg_id;
    uint32_t uid;
    RespGetMacAddress *resp_get_mac_address;
    RespWifiScanGetApRecords *resp_wifi_scan_get_ap_records;
} Rpc;

typedef struct {
    void *(*_h_calloc)(size_t count, size_t size);
    void (*_h_free)(void *ptr);
    void *(*_h_memcpy)(void *dst, const void *src, size_t size);
    void *(*_h_memset)(void *dst, int value, size_t size);
} hosted_functions_t;

typedef struct {
    hosted_functions_t *funcs;
} hosted_handle_t;

static hosted_handle_t g_h;
static unsigned allocation_calls;
static unsigned free_calls;
static unsigned hosted_memcpy_calls;
static unsigned hosted_memset_calls;
static unsigned scan_rpc_calls;
static unsigned response_callback_calls;
static unsigned request_alloc_calls;
static uint16_t observed_request_capacity;
static ctrl_cmd_t request_storage;
static ctrl_cmd_t *mock_scan_response;

static void check(int condition, const char *label);

static void *host_calloc(size_t count, size_t size)
{
    ++allocation_calls;
    return calloc(count, size);
}

static void host_free(void *ptr)
{
    ++free_calls;
    free(ptr);
}

static void *host_memcpy(void *dst, const void *src, size_t size)
{
    ++hosted_memcpy_calls;
    return memcpy(dst, src, size);
}

static void *host_memset(void *dst, int value, size_t size)
{
    ++hosted_memset_calls;
    return memset(dst, value, size);
}

static hosted_functions_t hosted_functions = {
    host_calloc,
    host_free,
    host_memcpy,
    host_memset
};

#define RPC_FAIL_ON_NULL(field) \
    do { if (!rpc_msg->field) goto fail_parse_rpc_msg; } while (0)
#define RPC_FAIL_ON_NULL_PRINT(ptr, message) \
    do { (void)(message); if (!(ptr)) goto fail_parse_rpc_msg; } while (0)
#define RPC_ERR_IN_RESP(field) \
    do { \
        if (rpc_msg->field->resp) { \
            app_resp->resp_event_status = rpc_msg->field->resp; \
            goto fail_parse_rpc_msg; \
        } \
    } while (0)
#define RPC_RSP_COPY_BYTES(dst, src) \
    do { \
        if ((src).data && (src).len) \
            g_h.funcs->_h_memcpy((dst), (src).data, (src).len); \
    } while (0)

static ctrl_cmd_t *mock_default_request(void)
{
    ++request_alloc_calls;
    memset(&request_storage, 0, sizeof(request_storage));
    request_storage.msg_type = RPC_TYPE__Req;
    return &request_storage;
}

#define RPC_DEFAULT_REQ() mock_default_request()

static ctrl_cmd_t *wifi_scan_get_ap_records(ctrl_cmd_t *request)
{
    ++scan_rpc_calls;
    observed_request_capacity = request->u.wifi_scan_ap_list.number;
    return mock_scan_response;
}

static int rpc_rsp_callback(ctrl_cmd_t *response)
{
    ++response_callback_calls;
    return response ? response->resp_event_status : FAILURE;
}

static unsigned checks;
static unsigned cases;
static unsigned failures;

static void check(int condition, const char *label)
{
    ++checks;
    if (!condition) {
        ++failures;
        fprintf(stderr, "FAIL: %s\n", label);
    }
}

@@PARSER_SIGNATURE@@
{
@@PARSER_PREFIX@@
    switch (rpc_msg->msg_id) {
@@MAC_CASE@@
@@SCAN_CASE@@
@@DEFAULT_CASE@@
    }
@@PARSER_SUFFIX@@
}

@@WRAPPER_FUNCTION@@

static Rpc test_rpc;
static ctrl_cmd_t test_app;
static RespWifiScanGetApRecords test_scan;
static RespGetMacAddress test_mac;
static WifiCountry fixture_country;
static WifiHeApInfo fixture_he_ap;
static uint8_t fixture_ssid[] = {'A', 'P'};
static uint8_t fixture_bssid[] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static uint8_t fixture_cc[] = {'U', 'S'};
static uint8_t fixture_cc_large[] = {'U', 'S', 'X'};
static uint8_t fixture_mac[] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
static WifiApRecord fixture_records[8];
static WifiApRecord *fixture_record_ptrs[8];
static wifi_ap_record_t wrapper_records[9];
static ctrl_cmd_t wrapper_response;

static void reset_runtime(void)
{
    allocation_calls = 0;
    free_calls = 0;
    hosted_memcpy_calls = 0;
    hosted_memset_calls = 0;
    scan_rpc_calls = 0;
    response_callback_calls = 0;
    request_alloc_calls = 0;
    observed_request_capacity = 0;
    mock_scan_response = NULL;
    memset(&request_storage, 0, sizeof(request_storage));
    g_h.funcs = &hosted_functions;
}

static void initialize_record_fixtures(void)
{
    size_t i;
    fixture_country.cc.len = sizeof(fixture_cc);
    fixture_country.cc.data = fixture_cc;
    fixture_country.schan = 1;
    fixture_country.nchan = 11;
    fixture_country.max_tx_power = 18;
    fixture_country.policy = 2;
    fixture_he_ap.bitmask = UINT32_C(0xC5);

    for (i = 0; i < 8; ++i) {
        memset(&fixture_records[i], 0, sizeof(fixture_records[i]));
        fixture_records[i].ssid.len = sizeof(fixture_ssid);
        fixture_records[i].ssid.data = fixture_ssid;
        fixture_records[i].bssid.len = sizeof(fixture_bssid);
        fixture_records[i].bssid.data = fixture_bssid;
        fixture_records[i].country = &fixture_country;
        fixture_records[i].he_ap = &fixture_he_ap;
        fixture_records[i].primary = (uint8_t)(i + 1);
        fixture_records[i].second = 2;
        fixture_records[i].rssi = (int8_t)(-30 - (int)i);
        fixture_records[i].authmode = 3;
        fixture_records[i].pairwise_cipher = 4;
        fixture_records[i].group_cipher = 5;
        fixture_records[i].ant = 1;
        fixture_records[i].bitmask = UINT32_C(0x1ff);
        fixture_record_ptrs[i] = &fixture_records[i];
    }
}

static void prepare_scan(int32_t number, size_t repeated_count,
                         WifiApRecord **records)
{
    reset_runtime();
    initialize_record_fixtures();
    memset(&test_rpc, 0, sizeof(test_rpc));
    memset(&test_app, 0, sizeof(test_app));
    memset(&test_scan, 0, sizeof(test_scan));
    test_rpc.msg_id = RPC_ID__Resp_WifiScanGetApRecords;
    test_rpc.uid = UINT32_C(1234);
    test_rpc.resp_wifi_scan_get_ap_records = &test_scan;
    test_scan.number = number;
    test_scan.n_ap_records = repeated_count;
    test_scan.ap_records = records;
    test_app.resp_event_status = SUCCESS;
}

static void expect_scan_decode(const char *label)
{
    (void)rpc_parse_rsp(&test_rpc, &test_app);
    check(test_app.resp_event_status == RPC_ERR_PROTOBUF_DECODE, label);
    check(allocation_calls == 0, "invalid scan is rejected before allocation");
    check(test_app.u.wifi_scan_ap_list.out_list == NULL,
          "invalid scan publishes no record list");
}

static void cleanup_app_response(ctrl_cmd_t *response)
{
    if (response->app_free_buff_hdl && response->app_free_buff_func) {
        response->app_free_buff_func(response->app_free_buff_hdl);
        response->app_free_buff_hdl = NULL;
    }
}

static void test_scan_success_count(uint16_t count)
{
    uint16_t i;
    ++cases;
    prepare_scan((int32_t)count, count, count ? fixture_record_ptrs : NULL);
    check(rpc_parse_rsp(&test_rpc, &test_app) == SUCCESS,
          "scan parser returns its normal completion status");
    check(test_app.resp_event_status == SUCCESS, "valid scan response succeeds");
    check(test_app.u.wifi_scan_ap_list.number == count,
          "valid scan response preserves its count");
    if (count == 0) {
        check(test_app.u.wifi_scan_ap_list.out_list == NULL,
              "zero AP response does not require a list");
        check(allocation_calls == 0, "zero AP response does not allocate");
        return;
    }

    check(allocation_calls == 1, "valid nonempty scan allocates one output list");
    check(test_app.u.wifi_scan_ap_list.out_list != NULL,
          "valid nonempty scan publishes its output list");
    for (i = 0; i < count; ++i) {
        wifi_ap_record_t *record = &test_app.u.wifi_scan_ap_list.out_list[i];
        check(strcmp(record->ssid, "AP") == 0, "SSID bytes are copied and terminated");
        check(memcmp(record->bssid, fixture_bssid, sizeof(fixture_bssid)) == 0,
              "BSSID bytes are copied");
        check(record->primary == (uint8_t)(i + 1), "AP primary channel is copied");
        check(record->country.cc[0] == 'U' && record->country.cc[1] == 'S',
              "country code bytes are copied");
        check(record->he_ap.bss_color == (fixture_he_ap.bitmask & 0x3f),
              "HE AP information is copied");
    }
    cleanup_app_response(&test_app);
    check(free_calls == 1, "valid scan output can be released through host callback");
}

static void test_scan_negative_count(void)
{
    ++cases;
    prepare_scan(-1, 0, NULL);
    expect_scan_decode("negative AP count is rejected");
}

static void test_scan_uint16_overflow(void)
{
    ++cases;
    prepare_scan(INT32_C(65536), (size_t)65536, fixture_record_ptrs);
    expect_scan_decode("AP count above UINT16_MAX is rejected");
}

static void test_scan_repeated_count_mismatch(void)
{
    ++cases;
    prepare_scan(2, 1, fixture_record_ptrs);
    expect_scan_decode("count different from repeated field length is rejected");
}

static void test_scan_null_response_list(void)
{
    ++cases;
    prepare_scan(1, 1, NULL);
    expect_scan_decode("positive count with NULL protobuf list is rejected");
}

static void test_scan_null_record(void)
{
    ++cases;
    prepare_scan(1, 1, fixture_record_ptrs);
    fixture_record_ptrs[0] = NULL;
    expect_scan_decode("NULL AP record is rejected");
}

static void test_scan_null_country(void)
{
    ++cases;
    prepare_scan(1, 1, fixture_record_ptrs);
    fixture_records[0].country = NULL;
    expect_scan_decode("NULL country structure is rejected");
}

static void test_scan_null_he_ap(void)
{
    ++cases;
    prepare_scan(1, 1, fixture_record_ptrs);
    fixture_records[0].he_ap = NULL;
    expect_scan_decode("NULL HE AP structure is rejected");
}

static void test_scan_oversized_ssid(void)
{
    ++cases;
    prepare_scan(1, 1, fixture_record_ptrs);
    fixture_records[0].ssid.len = sizeof(((wifi_ap_record_t *)0)->ssid);
    expect_scan_decode("SSID length that leaves no terminator is rejected");
}

static void test_scan_ssid_missing_data(void)
{
    ++cases;
    prepare_scan(1, 1, fixture_record_ptrs);
    fixture_records[0].ssid.len = 1;
    fixture_records[0].ssid.data = NULL;
    expect_scan_decode("nonempty SSID without data is rejected");
}

static void test_scan_wrong_bssid_length(void)
{
    ++cases;
    prepare_scan(1, 1, fixture_record_ptrs);
    fixture_records[0].bssid.len = 7;
    expect_scan_decode("BSSID length other than six is rejected");
}

static void test_scan_bssid_missing_data(void)
{
    ++cases;
    prepare_scan(1, 1, fixture_record_ptrs);
    fixture_records[0].bssid.data = NULL;
    expect_scan_decode("BSSID without data is rejected");
}

static void test_scan_oversized_country_code(void)
{
    ++cases;
    prepare_scan(1, 1, fixture_record_ptrs);
    fixture_records[0].country->cc.len = sizeof(fixture_cc_large);
    fixture_records[0].country->cc.data = fixture_cc_large;
    expect_scan_decode("country code longer than destination is rejected");
}

static void test_scan_country_code_missing_data(void)
{
    ++cases;
    prepare_scan(1, 1, fixture_record_ptrs);
    fixture_records[0].country->cc.len = 1;
    fixture_records[0].country->cc.data = NULL;
    expect_scan_decode("nonempty country code without data is rejected");
}

static void test_scan_remote_error(void)
{
    ++cases;
    prepare_scan(1, 1, fixture_record_ptrs);
    test_scan.resp = REMOTE_ERROR;
    (void)rpc_parse_rsp(&test_rpc, &test_app);
    check(test_app.resp_event_status == REMOTE_ERROR,
          "remote scan error is preserved");
    check(allocation_calls == 0, "remote scan error does not allocate records");
}

static void test_scan_null_payload(void)
{
    ++cases;
    prepare_scan(1, 1, fixture_record_ptrs);
    test_rpc.resp_wifi_scan_get_ap_records = NULL;
    (void)rpc_parse_rsp(&test_rpc, &test_app);
    check(test_app.resp_event_status == RPC_ERR_PROTOBUF_DECODE,
          "missing scan response payload keeps decode error status");
    check(allocation_calls == 0, "missing scan payload does not allocate");
}

static void prepare_mac(size_t length, uint8_t *data, int remote_status)
{
    reset_runtime();
    memset(&test_rpc, 0, sizeof(test_rpc));
    memset(&test_app, 0, sizeof(test_app));
    memset(&test_mac, 0, sizeof(test_mac));
    test_rpc.msg_id = RPC_ID__Resp_GetMACAddress;
    test_rpc.resp_get_mac_address = &test_mac;
    test_mac.resp = remote_status;
    test_mac.mac.len = length;
    test_mac.mac.data = data;
    test_app.resp_event_status = SUCCESS;
}

static void test_mac_success(void)
{
    ++cases;
    prepare_mac(sizeof(fixture_mac), fixture_mac, 0);
    (void)rpc_parse_rsp(&test_rpc, &test_app);
    check(test_app.resp_event_status == SUCCESS, "six-byte MAC response succeeds");
    check(memcmp(test_app.u.wifi_mac.mac, fixture_mac, sizeof(fixture_mac)) == 0,
          "valid MAC bytes are copied");
    check(hosted_memcpy_calls == 1, "valid MAC uses one bounded byte copy");
}

static void test_mac_wrong_length(size_t length)
{
    ++cases;
    prepare_mac(length, fixture_mac, 0);
    (void)rpc_parse_rsp(&test_rpc, &test_app);
    check(test_app.resp_event_status == RPC_ERR_PROTOBUF_DECODE,
          "MAC length other than six keeps decode error status");
    check(hosted_memcpy_calls == 0, "wrong-length MAC is rejected before copy");
}

static void test_mac_missing_data(void)
{
    ++cases;
    prepare_mac(6, NULL, 0);
    (void)rpc_parse_rsp(&test_rpc, &test_app);
    check(test_app.resp_event_status == RPC_ERR_PROTOBUF_DECODE,
          "MAC bytes missing data keeps decode error status");
    check(hosted_memcpy_calls == 0, "MAC with NULL bytes is rejected before copy");
}

static void test_default_decode_status(void)
{
    ++cases;
    reset_runtime();
    memset(&test_rpc, 0, sizeof(test_rpc));
    memset(&test_app, 0, sizeof(test_app));
    test_rpc.msg_id = TRANSPORT_TEST_UNKNOWN_ID;
    test_app.resp_event_status = SUCCESS;
    (void)rpc_parse_rsp(&test_rpc, &test_app);
    check(test_app.resp_event_status == RPC_ERR_PROTOBUF_DECODE,
          "unsupported response id retains default decode status");
}

static void initialize_wrapper_records(void)
{
    size_t i;
    for (i = 0; i < 9; ++i) {
        memset(&wrapper_records[i], 0, sizeof(wrapper_records[i]));
        (void)snprintf(wrapper_records[i].ssid, sizeof(wrapper_records[i].ssid),
                       "net%u", (unsigned)i);
        wrapper_records[i].primary = (uint8_t)(i + 1);
        wrapper_records[i].bssid[0] = 0x02;
        wrapper_records[i].bssid[5] = (uint8_t)i;
    }
}

static void prepare_wrapper(int response_status, uint16_t response_count,
                            wifi_ap_record_t *response_records)
{
    reset_runtime();
    initialize_wrapper_records();
    memset(&wrapper_response, 0, sizeof(wrapper_response));
    wrapper_response.resp_event_status = response_status;
    wrapper_response.u.wifi_scan_ap_list.number = response_count;
    wrapper_response.u.wifi_scan_ap_list.out_list = response_records;
    mock_scan_response = &wrapper_response;
}

static void test_wrapper_success(uint16_t capacity, uint16_t response_count)
{
    uint16_t number = capacity;
    wifi_ap_record_t output[8];
    uint16_t i;
    ++cases;
    prepare_wrapper(SUCCESS, response_count,
                    response_count ? wrapper_records : NULL);
    memset(output, 0xa5, sizeof(output));
    check(rpc_wifi_scan_get_ap_records(&number, output) == SUCCESS,
          "wrapper accepts response within caller capacity");
    check(number == response_count, "wrapper publishes returned AP count");
    check(observed_request_capacity == capacity,
          "wrapper sends original capacity to remote request");
    check(scan_rpc_calls == 1, "valid wrapper request calls remote once");
    check(response_callback_calls == 1, "valid wrapper request checks response once");
    check(hosted_memset_calls == 1, "wrapper clears caller capacity before copying");
    check(hosted_memcpy_calls == (response_count ? 1u : 0u),
          "wrapper copies only a nonempty validated result");
    for (i = 0; i < response_count; ++i) {
        check(output[i].primary == wrapper_records[i].primary,
              "wrapper copies each returned record");
        check(strcmp(output[i].ssid, wrapper_records[i].ssid) == 0,
              "wrapper preserves copied SSID");
    }
    if (response_count < capacity) {
        check(output[response_count].primary == 0 &&
              output[response_count].ssid[0] == '\0',
              "unused caller record remains cleared");
    }
}

static void test_wrapper_rejects_over_capacity(void)
{
    uint16_t number = 8;
    wifi_ap_record_t output[8];
    ++cases;
    prepare_wrapper(SUCCESS, 9, wrapper_records);
    memset(output, 0xa5, sizeof(output));
    check(rpc_wifi_scan_get_ap_records(&number, output) == RPC_ERR_PROTOBUF_DECODE,
          "wrapper rejects nine records for capacity eight");
    check(number == 8, "wrapper preserves capacity on malformed response");
    check(wrapper_response.resp_event_status == RPC_ERR_PROTOBUF_DECODE,
          "wrapper marks over-capacity response as decode error");
    check(hosted_memcpy_calls == 0, "over-capacity response is rejected before copy");
    check(output[7].primary == 0, "over-capacity response does not overrun output");
}

static void test_wrapper_rejects_missing_remote_list(void)
{
    uint16_t number = 8;
    wifi_ap_record_t output[8];
    ++cases;
    prepare_wrapper(SUCCESS, 1, NULL);
    memset(output, 0xa5, sizeof(output));
    check(rpc_wifi_scan_get_ap_records(&number, output) == RPC_ERR_PROTOBUF_DECODE,
          "wrapper rejects nonzero count with NULL remote list");
    check(number == 8, "wrapper preserves capacity when remote list is NULL");
    check(hosted_memcpy_calls == 0, "NULL remote list is rejected before copy");
}

static void test_wrapper_preserves_remote_error(void)
{
    uint16_t number = 8;
    wifi_ap_record_t output[8];
    ++cases;
    prepare_wrapper(REMOTE_ERROR, 1, wrapper_records);
    memset(output, 0xa5, sizeof(output));
    check(rpc_wifi_scan_get_ap_records(&number, output) == REMOTE_ERROR,
          "wrapper returns remote error unchanged");
    check(number == 8, "wrapper leaves capacity unchanged on remote error");
    check(hosted_memcpy_calls == 0, "remote error response is not copied");
}

static void test_wrapper_rejects_null_inputs(void)
{
    uint16_t zero = 0;
    uint16_t capacity = 8;
    wifi_ap_record_t output[8];
    int null_count_result;
    int zero_capacity_result;
    int null_output_result;
    ++cases;
    prepare_wrapper(SUCCESS, 0, NULL);
    null_count_result = rpc_wifi_scan_get_ap_records(NULL, output);
    zero_capacity_result = rpc_wifi_scan_get_ap_records(&zero, output);
    null_output_result = rpc_wifi_scan_get_ap_records(&capacity, NULL);
    check(null_count_result == FAILURE, "wrapper rejects NULL count pointer");
    check(zero_capacity_result == FAILURE, "wrapper rejects zero caller capacity");
    check(null_output_result == FAILURE, "wrapper rejects NULL caller record list");
    check(scan_rpc_calls == 0, "invalid wrapper inputs do not call remote");
    check(response_callback_calls == 0, "invalid wrapper inputs do not parse a response");
    check(request_alloc_calls == 3, "each wrapper invocation creates its request first");
}

int main(void)
{
    test_scan_success_count(0);
    test_scan_success_count(1);
    test_scan_success_count(8);
    test_scan_negative_count();
    test_scan_uint16_overflow();
    test_scan_repeated_count_mismatch();
    test_scan_null_response_list();
    test_scan_null_record();
    test_scan_null_country();
    test_scan_null_he_ap();
    test_scan_oversized_ssid();
    test_scan_ssid_missing_data();
    test_scan_wrong_bssid_length();
    test_scan_bssid_missing_data();
    test_scan_oversized_country_code();
    test_scan_country_code_missing_data();
    test_scan_remote_error();
    test_scan_null_payload();
    test_mac_success();
    test_mac_wrong_length(5);
    test_mac_wrong_length(7);
    test_mac_missing_data();
    test_default_decode_status();
    test_wrapper_success(1, 0);
    test_wrapper_success(1, 1);
    test_wrapper_success(8, 8);
    test_wrapper_rejects_over_capacity();
    test_wrapper_rejects_missing_remote_list();
    test_wrapper_preserves_remote_error();
    test_wrapper_rejects_null_inputs();

    if (failures != 0) {
        fprintf(stderr, "FAIL: %u of %u checks failed across %u cases\n",
                failures, checks, cases);
        return 1;
    }
    printf("PASS: %u cases, %u checks\n", cases, checks);
    return 0;
}
'''
    return (
        template.replace("@@PARSER_SIGNATURE@@", parser_signature)
        .replace("@@PARSER_PREFIX@@", parser_prefix)
        .replace("@@PARSER_SUFFIX@@", parser_suffix)
        .replace("@@MAC_CASE@@", mac_case)
        .replace("@@SCAN_CASE@@", scan_case)
        .replace("@@DEFAULT_CASE@@", default_case)
        .replace("@@WRAPPER_FUNCTION@@", wrapper_source)
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compile focused host tests from ESP-Hosted RPC scan sources"
    )
    parser.add_argument("wrapper", type=Path, help="path to rpc_wrap.c")
    parser.add_argument("response", type=Path, help="path to rpc_rsp.c")
    args = parser.parse_args()

    compiler_words = shlex.split(os.environ.get("CC", "cc"))
    if not compiler_words:
        parser.error("CC must name a C compiler")
    compiler = shutil.which(compiler_words[0])
    if compiler is None:
        parser.error(f"C compiler not found: {compiler_words[0]}")
    compiler_words[0] = compiler

    try:
        wrapper_source = extract_function(
            args.wrapper.read_text(encoding="utf-8"), WRAPPER_SIGNATURE, args.wrapper
        )
        response_text = args.response.read_text(encoding="utf-8")
        (
            parser_signature,
            parser_prefix,
            parser_suffix,
        ) = extract_function_context(
            response_text,
            PARSER_SIGNATURE,
            "switch (rpc_msg->msg_id)",
            args.response,
        )
        mac_offset, mac_case = extract_case(response_text, MAC_CASE, args.response)
        scan_offset, scan_case = extract_case(
            response_text, SCAN_CASE, args.response, mac_offset
        )
        _, default_case = extract_case(
            response_text, DEFAULT_CASE, args.response, scan_offset
        )
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="hosted-scan-test-") as tmp:
        source_path = Path(tmp) / "test_scan.c"
        source_path.write_text(
            make_test_source(
                wrapper_source,
                parser_signature,
                parser_prefix,
                parser_suffix,
                mac_case,
                scan_case,
                default_case,
            ),
            encoding="utf-8",
        )

        for optimization in ("-O0", "-O2"):
            binary_path = Path(tmp) / f"test_scan_{optimization[2:]}"
            command = [
                *compiler_words,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                optimization,
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                str(source_path),
                "-o",
                str(binary_path),
            ]
            print(f"{optimization}: compiling extracted scan RPC source with ASan and UBSan")
            built = subprocess.run(command, text=True, capture_output=True)
            if built.stdout:
                print(built.stdout, end="")
            if built.stderr:
                print(built.stderr, end="", file=sys.stderr)
            if built.returncode != 0:
                print(f"{optimization}: compiler exited {built.returncode}", file=sys.stderr)
                return built.returncode

            run = subprocess.run([str(binary_path)], text=True, capture_output=True)
            if run.stdout:
                print(f"{optimization}: {run.stdout}", end="")
            if run.stderr:
                print(run.stderr, end="", file=sys.stderr)
            if run.returncode != 0:
                print(f"{optimization}: test executable exited {run.returncode}",
                      file=sys.stderr)
                return run.returncode

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
