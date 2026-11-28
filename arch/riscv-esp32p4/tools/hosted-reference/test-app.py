#!/usr/bin/env python3
"""Compile host tests around the complete bounded Hosted reference app source."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


MOCKS = r'''#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
typedef int wifi_mode_t;
typedef int wifi_interface_t;
typedef void *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *);

enum {
    ESP_OK = 0,
    ESP_ERR_TIMEOUT = 263,
    ESP_ERR_INVALID_RESPONSE = 105,
    ESP_FAIL = -1,
    WIFI_MODE_STA = 1,
    WIFI_IF_STA = 0
};

typedef struct {
    int nvs_enable;
} wifi_init_config_t;

typedef struct {
    esp_timer_cb_t callback;
    const char *name;
} esp_timer_create_args_t;

typedef struct {
    uint8_t opaque[32];
} wifi_ap_record_t;

#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){ .nvs_enable = 1 })

_Noreturn void esp_system_abort(const char *reason);

enum mock_api {
    API_EVENT_LOOP,
    API_HOSTED_INIT,
    API_WIFI_INIT,
    API_SET_MODE,
    API_WIFI_START,
    API_GET_MAC,
    API_SCAN_START,
    API_SCAN_GET_NUM,
    API_SCAN_GET_RECORDS,
    API_WIFI_STOP,
    API_TIMER_CREATE,
    API_TIMER_START,
    API_TIMER_STOP,
    API_COUNT
};

static unsigned checks;
static unsigned cases;
static unsigned failures;
static unsigned api_calls[API_COUNT];
static esp_err_t api_results[API_COUNT];
static uint8_t configured_mac[6];
static uint16_t configured_ap_count;
static uint16_t configured_record_count;
static uint16_t requested_record_count;
static unsigned mock_contract_violations;
static unsigned abort_calls;
static int abort_armed;
static int sdk_check_status;
static char abort_reason[128];
static char error_log[256];
static char info_log[256];
static int64_t clock_values[32];
static unsigned clock_value_count;
static unsigned clock_value_index;
static int64_t fallback_time;
static esp_timer_cb_t registered_timer_callback;
static int timer_identity;
static int fire_timer_during_start;
static jmp_buf abort_environment;

static unsigned api_call(enum mock_api api)
{
    return ++api_calls[api];
}

static esp_err_t api_result(enum mock_api api)
{
    api_call(api);
    return api_results[api];
}

static void mock_log_error(const char *tag, const char *format, ...)
{
    va_list arguments;
    (void)tag;
    va_start(arguments, format);
    (void)vsnprintf(error_log, sizeof(error_log), format, arguments);
    va_end(arguments);
}

static void mock_log_info(const char *tag, const char *format, ...)
{
    va_list arguments;
    (void)tag;
    va_start(arguments, format);
    (void)vsnprintf(info_log, sizeof(info_log), format, arguments);
    va_end(arguments);
}

#define ESP_LOGE(...) mock_log_error(__VA_ARGS__)
#define ESP_LOGI(...) mock_log_info(__VA_ARGS__)
#define ESP_ERROR_CHECK(expression) do { \
    esp_err_t mock_status = (expression); \
    if (mock_status != ESP_OK) { \
        sdk_check_status = mock_status; \
        esp_system_abort("ESP_ERROR_CHECK"); \
    } \
} while (0)

esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_hosted_init(void *configuration);
esp_err_t esp_wifi_init(const wifi_init_config_t *configuration);
esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_get_mac(wifi_interface_t interface, uint8_t mac[6]);
esp_err_t esp_wifi_scan_start(const void *configuration, bool block);
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *count);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records);
esp_err_t esp_wifi_stop(void);
int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *arguments,
                           esp_timer_handle_t *handle);
esp_err_t esp_timer_start_once(esp_timer_handle_t handle, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t handle);

esp_err_t esp_event_loop_create_default(void)
{
    return api_result(API_EVENT_LOOP);
}

esp_err_t esp_hosted_init(void *configuration)
{
    if (configuration != NULL)
        ++mock_contract_violations;
    return api_result(API_HOSTED_INIT);
}

esp_err_t esp_wifi_init(const wifi_init_config_t *configuration)
{
    if (configuration == NULL || configuration->nvs_enable != 0)
        ++mock_contract_violations;
    return api_result(API_WIFI_INIT);
}

esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{
    if (mode != WIFI_MODE_STA)
        ++mock_contract_violations;
    return api_result(API_SET_MODE);
}

esp_err_t esp_wifi_start(void)
{
    return api_result(API_WIFI_START);
}

esp_err_t esp_wifi_get_mac(wifi_interface_t interface, uint8_t mac[6])
{
    esp_err_t status = api_result(API_GET_MAC);
    if (interface != WIFI_IF_STA || mac == NULL)
        ++mock_contract_violations;
    if (status == ESP_OK)
        memcpy(mac, configured_mac, sizeof(configured_mac));
    return status;
}

esp_err_t esp_wifi_scan_start(const void *configuration, bool block)
{
    if (configuration != NULL || !block)
        ++mock_contract_violations;
    return api_result(API_SCAN_START);
}

esp_err_t esp_wifi_scan_get_ap_num(uint16_t *count)
{
    esp_err_t status = api_result(API_SCAN_GET_NUM);
    if (count == NULL)
        ++mock_contract_violations;
    if (status == ESP_OK)
        *count = configured_ap_count;
    return status;
}

esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records)
{
    esp_err_t status = api_result(API_SCAN_GET_RECORDS);
    if (count == NULL || records == NULL) {
        ++mock_contract_violations;
    } else {
        requested_record_count = *count;
        if (status == ESP_OK)
            *count = configured_record_count;
    }
    return status;
}

esp_err_t esp_wifi_stop(void)
{
    return api_result(API_WIFI_STOP);
}

int64_t esp_timer_get_time(void)
{
    if (clock_value_index < clock_value_count)
        return clock_values[clock_value_index++];
    return fallback_time;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *arguments,
                           esp_timer_handle_t *handle)
{
    esp_err_t status = api_result(API_TIMER_CREATE);
    if (arguments == NULL || arguments->callback == NULL ||
        arguments->name == NULL || strcmp(arguments->name, "reference_deadline") != 0 ||
        handle == NULL)
        ++mock_contract_violations;
    if (arguments != NULL)
        registered_timer_callback = arguments->callback;
    if (status == ESP_OK && handle != NULL)
        *handle = &timer_identity;
    return status;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t handle, uint64_t timeout_us)
{
    esp_err_t status = api_result(API_TIMER_START);
    if (handle != &timer_identity || timeout_us != UINT64_C(40000000))
        ++mock_contract_violations;
    if (status == ESP_OK && fire_timer_during_start &&
        registered_timer_callback != NULL)
        registered_timer_callback(NULL);
    return status;
}

esp_err_t esp_timer_stop(esp_timer_handle_t handle)
{
    if (handle != &timer_identity)
        ++mock_contract_violations;
    return api_result(API_TIMER_STOP);
}

_Noreturn void esp_system_abort(const char *reason)
{
    ++abort_calls;
    (void)snprintf(abort_reason, sizeof(abort_reason), "%s", reason);
    if (abort_armed)
        longjmp(abort_environment, 1);
    (void)fprintf(stderr, "unexpected abort outside test harness: %s\n", reason);
    abort();
}
'''


HARNESS = r'''
static void check(int condition, const char *description)
{
    ++checks;
    if (!condition) {
        ++failures;
        (void)fprintf(stderr, "FAIL: %s\n", description);
    }
}

static void reset_mocks(void)
{
    memset(api_calls, 0, sizeof(api_calls));
    memset(api_results, 0, sizeof(api_results));
    memset(configured_mac, 0, sizeof(configured_mac));
    configured_mac[0] = 0x02;
    configured_mac[5] = 0x01;
    configured_ap_count = 0;
    configured_record_count = 0;
    requested_record_count = 0;
    mock_contract_violations = 0;
    abort_calls = 0;
    abort_armed = 0;
    sdk_check_status = ESP_OK;
    abort_reason[0] = '\0';
    error_log[0] = '\0';
    info_log[0] = '\0';
    memset(clock_values, 0, sizeof(clock_values));
    clock_value_count = 0;
    clock_value_index = 0;
    fallback_time = 100;
    registered_timer_callback = NULL;
    fire_timer_during_start = 0;
    atomic_store(&phase, "application startup");
    atomic_store(&complete, false);
    deadline = 0;
    deadline_timer = NULL;
}

static int run_app_catching_abort(void)
{
    abort_armed = 1;
    if (setjmp(abort_environment) == 0) {
        app_main();
        abort_armed = 0;
        return 0;
    }
    abort_armed = 0;
    return 1;
}

static int run_callback_catching_abort(esp_timer_cb_t callback)
{
    abort_armed = 1;
    if (setjmp(abort_environment) == 0) {
        callback(NULL);
        abort_armed = 0;
        return 0;
    }
    abort_armed = 0;
    return 1;
}

static void test_successful_scan(const char *name, uint16_t aps,
                                 uint16_t returned, unsigned records_calls,
                                 const char *expected_summary)
{
    int aborted;
    ++cases;
    reset_mocks();
    configured_ap_count = aps;
    configured_record_count = returned;
    aborted = run_app_catching_abort();
    check(aborted == 0, name);
    check(abort_calls == 0, "successful app does not abort");
    check(api_calls[API_SCAN_GET_RECORDS] == records_calls,
          "record query is omitted for zero APs and used otherwise");
    check(api_calls[API_WIFI_STOP] == 1, "successful scan stops Wi-Fi");
    check(api_calls[API_TIMER_STOP] == 1, "successful scan stops deadline timer");
    check(mock_contract_violations == 0, "app uses expected API arguments");
    check(strstr(info_log, expected_summary) != NULL,
          "success summary reports AP and copied record counts");
    if (records_calls != 0)
        check(requested_record_count == 8, "record query is capped at eight entries");
}

static void test_invalid_mac(const char *name, const uint8_t mac[6])
{
    int aborted;
    ++cases;
    reset_mocks();
    memcpy(configured_mac, mac, sizeof(configured_mac));
    aborted = run_app_catching_abort();
    check(aborted == 1, name);
    check(abort_calls == 1, "invalid MAC fail-stops once");
    check(strstr(error_log, "phase=MAC query") != NULL,
          "invalid MAC failure identifies query phase");
    check(strstr(error_log, "status=105") != NULL,
          "invalid MAC uses invalid-response status");
    check(api_calls[API_SCAN_START] == 0, "invalid MAC prevents scan");
    check(api_calls[API_WIFI_STOP] == 0, "invalid MAC stops before Wi-Fi cleanup path");
}

static void test_api_failure(enum mock_api api, enum mock_api next_api,
                             const char *phase_name, int sdk_check)
{
    int aborted;
    ++cases;
    reset_mocks();
    configured_ap_count = 1;
    configured_record_count = 1;
    api_results[api] = -37;
    aborted = run_app_catching_abort();
    check(aborted == 1, "API error fail-stops");
    check(abort_calls == 1, "API error aborts once");
    check(api_calls[api] == 1, "failing API is called once");
    if (next_api < API_COUNT)
        check(api_calls[next_api] == 0, "later API is not called after failure");
    if (sdk_check) {
        check(strcmp(abort_reason, "ESP_ERROR_CHECK") == 0,
              "timer setup error uses ESP_ERROR_CHECK");
        check(sdk_check_status == -37, "ESP_ERROR_CHECK receives timer error");
    } else {
        check(strstr(error_log, phase_name) != NULL,
              "fail-stop log identifies failing phase");
        check(strstr(error_log, "status=-37") != NULL,
              "fail-stop log preserves API error status");
    }
}

static void test_malformed_record_count(const char *name, uint16_t aps,
                                        uint16_t returned)
{
    int aborted;
    ++cases;
    reset_mocks();
    configured_ap_count = aps;
    configured_record_count = returned;
    aborted = run_app_catching_abort();
    check(aborted == 1, name);
    check(abort_calls == 1, "malformed count fail-stops once");
    check(api_calls[API_SCAN_GET_RECORDS] == 1,
          "malformed count is observed after record query");
    check(api_calls[API_WIFI_STOP] == 0,
          "malformed count prevents success cleanup path");
    check(strstr(error_log, "phase=scan records") != NULL,
          "malformed count identifies record phase");
    check(strstr(error_log, "status=105") != NULL,
          "malformed count uses invalid-response status");
}

static void test_deadline(const char *name, int64_t check_time,
                          unsigned clock_samples, unsigned event_calls)
{
    int aborted;
    ++cases;
    reset_mocks();
    clock_values[0] = 100;
    clock_values[1] = check_time;
    if (clock_samples == 3)
        clock_values[2] = INT64_C(40000100);
    clock_value_count = clock_samples;
    aborted = run_app_catching_abort();
    check(aborted == 1, name);
    check(abort_calls == 1, "deadline expiry fail-stops once");
    check(api_calls[API_EVENT_LOOP] == event_calls,
          "deadline check occurs on expected side of event-loop call");
    check(strstr(error_log, "phase=event loop") != NULL,
          "deadline failure identifies event-loop phase");
    check(strstr(error_log, "status=263") != NULL,
          "deadline expiry uses timeout status");
}

static void test_timer_callback_failstop(void)
{
    int aborted;
    ++cases;
    reset_mocks();
    fire_timer_during_start = 1;
    aborted = run_app_catching_abort();
    check(aborted == 1, "expired timer callback aborts the app");
    check(registered_timer_callback != NULL,
          "deadline timer registers its callback");
    check(abort_calls == 1, "expired timer callback aborts once");
    check(strcmp(abort_reason, "REFERENCE fail-stop; no automatic retry") == 0,
          "timer callback uses fail-stop policy");
    check(strstr(error_log, "phase=application startup") != NULL,
          "timer callback reports current phase");
    check(strstr(error_log, "status=263") != NULL,
          "timer callback reports timeout status");
    check(api_calls[API_EVENT_LOOP] == 0,
          "timer expiry prevents later initialization");
}

static void test_callback_after_completion(void)
{
    int aborted;
    ++cases;
    reset_mocks();
    aborted = run_app_catching_abort();
    check(aborted == 0, "setup app for completed timer callback");
    check(registered_timer_callback != NULL,
          "successful app registers the deadline callback");
    check(atomic_load(&complete), "successful app marks work complete");
    if (registered_timer_callback != NULL) {
        aborted = run_callback_catching_abort(registered_timer_callback);
        check(aborted == 0, "deadline callback ignores completed app");
        check(abort_calls == 0, "completed callback does not abort");
    }
}

int main(void)
{
    static const uint8_t zero_mac[6] = { 0, 0, 0, 0, 0, 0 };
    static const uint8_t all_ff_mac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t multicast_mac[6] = { 0x01, 0, 0, 0, 0, 1 };

    test_successful_scan("zero-AP scan succeeds", 0, 0, 0,
                         "aps=0 records=0");
    test_successful_scan("one-AP scan copies one record", 1, 1, 1,
                         "aps=1 records=1");
    test_successful_scan("eight-AP scan copies eight records", 8, 8, 1,
                         "aps=8 records=8");
    test_successful_scan("larger scan is limited to eight records", 12, 8, 1,
                         "aps=12 records=8");

    test_invalid_mac("zero MAC is rejected", zero_mac);
    test_invalid_mac("all-FF MAC is rejected", all_ff_mac);
    test_invalid_mac("multicast MAC is rejected", multicast_mac);

    test_api_failure(API_EVENT_LOOP, API_HOSTED_INIT, "event loop", 0);
    test_api_failure(API_HOSTED_INIT, API_WIFI_INIT, "host initialization", 0);
    test_api_failure(API_WIFI_INIT, API_SET_MODE, "wifi initialization", 0);
    test_api_failure(API_SET_MODE, API_WIFI_START, "station mode", 0);
    test_api_failure(API_WIFI_START, API_GET_MAC, "wifi start", 0);
    test_api_failure(API_GET_MAC, API_SCAN_START, "MAC query", 0);
    test_api_failure(API_SCAN_START, API_SCAN_GET_NUM, "blocking scan", 0);
    test_api_failure(API_SCAN_GET_NUM, API_SCAN_GET_RECORDS, "scan count", 0);
    test_api_failure(API_SCAN_GET_RECORDS, API_WIFI_STOP, "scan records", 0);
    test_api_failure(API_WIFI_STOP, API_TIMER_STOP, "wifi stop", 0);
    test_api_failure(API_TIMER_CREATE, API_TIMER_START, "", 1);
    test_api_failure(API_TIMER_START, API_EVENT_LOOP, "", 1);
    test_api_failure(API_TIMER_STOP, API_COUNT, "wifi stop", 0);

    test_malformed_record_count("returned records exceed AP count", 1, 2);
    test_malformed_record_count("returned records exceed capacity", 12, 9);
    test_malformed_record_count("returned records are short by four", 4, 0);
    test_malformed_record_count("returned records are short by three", 4, 1);
    test_malformed_record_count("returned records are short by one", 4, 3);
    test_malformed_record_count("large scan returns fewer than capacity", 12, 7);

    test_deadline("pre-operation clock at deadline fail-stops",
                  INT64_C(40000100), 2, 0);
    test_deadline("post-operation clock at deadline fail-stops",
                  INT64_C(40000099), 3, 1);
    test_timer_callback_failstop();
    test_callback_after_completion();

    if (failures != 0) {
        (void)fprintf(stderr, "FAIL: %u of %u checks failed across %u cases\n",
                      failures, checks, cases);
        return 1;
    }
    (void)printf("PASS: %u cases, %u checks\n", cases, checks);
    return 0;
}
'''


def strip_includes(source_path: Path) -> str:
    source = source_path.read_text(encoding="utf-8")
    source_lines = source.splitlines()
    app_source = "\n".join(
        line for line in source_lines
        if not line.lstrip().startswith("#include ")
    )
    if "void app_main(void)" not in app_source:
        raise ValueError(f"app_main not found in {source_path}")
    return app_source + "\n"


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(
        description="Test the complete reference.c app with host API mocks"
    )
    parser.add_argument("source", type=Path, help="path to main/reference.c")
    args = parser.parse_args()

    compiler_words = shlex.split(os.environ.get("CC", "cc"))
    if not compiler_words:
        parser.error("CC must name a C compiler")
    compiler = shutil.which(compiler_words[0])
    if compiler is None:
        parser.error(f"C compiler not found: {compiler_words[0]}")
    compiler_words[0] = compiler

    try:
        app_source = strip_includes(args.source)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="hosted-app-test-") as tmp:
        generated_source = Path(tmp) / "test_app.c"
        generated_source.write_text(MOCKS + app_source + HARNESS, encoding="utf-8")

        for optimization in ("-O0", "-O2"):
            binary = Path(tmp) / f"test_app_{optimization[2:]}"
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
                str(generated_source),
                "-o",
                str(binary),
            ]
            print(f"{optimization}: compiling extracted app with ASan and UBSan")
            built = subprocess.run(command, text=True, capture_output=True)
            if built.stdout:
                print(built.stdout, end="")
            if built.stderr:
                print(built.stderr, end="", file=sys.stderr)
            if built.returncode != 0:
                print(f"{optimization}: compiler exited {built.returncode}",
                      file=sys.stderr)
                return built.returncode

            run = subprocess.run([str(binary)], text=True, capture_output=True)
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
