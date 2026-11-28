#!/usr/bin/env python3
"""Compile and exercise the complete lifecycle app source with IDF mocks."""

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
typedef const char *esp_event_base_t;
typedef void *esp_timer_handle_t;
typedef uint32_t TickType_t;
typedef void (*esp_timer_cb_t)(void *);
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);

enum {
    ESP_OK = 0,
    ESP_FAIL = -1,
    ESP_ERR_TIMEOUT = 263,
    ESP_ERR_INVALID_RESPONSE = 105,
    WIFI_MODE_STA = 1,
    WIFI_EVENT_STA_START = 2,
    WIFI_EVENT_STA_STOP = 3,
    WIFI_EVENT_STA_CONNECTED = 4
};

static const char wifi_event_base[] = "WIFI_EVENT";
#define WIFI_EVENT ((esp_event_base_t)wifi_event_base)
#define ESP_EVENT_ANY_ID (-1)
#define MOCK_TICK_PERIOD_US UINT64_C(1000)
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * UINT64_C(1000)) / \
                                        MOCK_TICK_PERIOD_US))

typedef struct {
    int nvs_enable;
} wifi_init_config_t;

typedef struct {
    esp_timer_cb_t callback;
    const char *name;
} esp_timer_create_args_t;

#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){ .nvs_enable = 1 })

enum mock_api {
    API_TIMER_CREATE,
    API_TIMER_START,
    API_EVENT_LOOP,
    API_EVENT_REGISTER,
    API_EVENT_UNREGISTER,
    API_HOSTED_INIT,
    API_WIFI_INIT,
    API_SET_MODE,
    API_WIFI_START,
    API_WIFI_STOP,
    API_TIMER_STOP,
    API_COUNT
};

enum event_behavior {
    EVENTS_NORMAL,
    EVENTS_DELAYED,
    EVENTS_BEFORE_ACK,
    EVENTS_NONE,
    EVENTS_DUPLICATE_START,
    EVENTS_CONNECTED,
    EVENTS_WRONG_ORDER
};

struct pending_event {
    int32_t event_id;
    uint64_t due_tick;
};

static unsigned checks;
static unsigned cases;
static unsigned failures;
static unsigned api_calls[API_COUNT];
static esp_err_t api_results[API_COUNT];
static unsigned contract_violations;
static unsigned abort_calls;
static int abort_armed;
static char abort_reason[128];
static char error_log[4096];
static char info_log[16384];
static jmp_buf abort_environment;

static esp_event_handler_t registered_event_handler;
static void *registered_event_argument;
static unsigned event_loop_created;
static unsigned observer_registered;
static unsigned hosted_initializations;
static unsigned wifi_initializations;
static unsigned station_mode_calls;
static unsigned successful_starts;
static unsigned successful_stops;
static enum event_behavior configured_event_behavior;
static int inject_event_during_unregister;
static struct pending_event pending_events[16];
static unsigned pending_event_count;
static uint64_t mock_ticks;
static int64_t mock_time_us;
static unsigned yield_calls;
static unsigned zero_or_long_yields;

static esp_timer_cb_t registered_timer_callback;
static int timer_identity;
static int timer_armed;
static int timer_fired;
static int64_t timer_due_us;
static uint64_t timer_timeout_us;
static int64_t timer_create_cost_us;

static unsigned api_call(enum mock_api api)
{
    return ++api_calls[api];
}

static esp_err_t api_result(enum mock_api api)
{
    (void)api_call(api);
    return api_results[api];
}

static void append_log(char *buffer, size_t capacity, const char *format,
                       va_list arguments)
{
    size_t used = strlen(buffer);
    if (used >= capacity)
        return;
    (void)vsnprintf(buffer + used, capacity - used, format, arguments);
    used = strlen(buffer);
    if (used + 1 < capacity) {
        buffer[used++] = '\n';
        buffer[used] = '\0';
    }
}

static void mock_log_error(const char *tag, const char *format, ...)
{
    (void)tag;
    va_list arguments;
    va_start(arguments, format);
    append_log(error_log, sizeof(error_log), format, arguments);
    va_end(arguments);
}

static void mock_log_info(const char *tag, const char *format, ...)
{
    (void)tag;
    va_list arguments;
    va_start(arguments, format);
    append_log(info_log, sizeof(info_log), format, arguments);
    va_end(arguments);
}

#define ESP_LOGE(...) mock_log_error(__VA_ARGS__)
#define ESP_LOGI(...) mock_log_info(__VA_ARGS__)

_Noreturn void esp_system_abort(const char *reason);

esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_register(esp_event_base_t event_base,
                                     int32_t event_id,
                                     esp_event_handler_t event_handler,
                                     void *event_handler_arg);
esp_err_t esp_event_handler_unregister(esp_event_base_t event_base,
                                       int32_t event_id,
                                       esp_event_handler_t event_handler);
esp_err_t esp_hosted_init(void *configuration);
esp_err_t esp_wifi_init(const wifi_init_config_t *configuration);
esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *arguments,
                           esp_timer_handle_t *handle);
esp_err_t esp_timer_start_once(esp_timer_handle_t handle, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t handle);
void vTaskDelay(TickType_t ticks);

static void schedule_event(int32_t event_id, uint64_t delay_ticks)
{
    if (pending_event_count >= sizeof(pending_events) / sizeof(pending_events[0])) {
        ++contract_violations;
        return;
    }
    pending_events[pending_event_count++] = (struct pending_event) {
        .event_id = event_id,
        .due_tick = mock_ticks + delay_ticks
    };
}

static void deliver_due_events(void)
{
    for (unsigned index = 0; index < pending_event_count;) {
        if (pending_events[index].due_tick > mock_ticks) {
            ++index;
            continue;
        }
        struct pending_event event = pending_events[index];
        for (unsigned move = index + 1; move < pending_event_count; ++move)
            pending_events[move - 1] = pending_events[move];
        --pending_event_count;
        if (registered_event_handler == NULL && observer_registered == 0)
            continue;
        if (registered_event_handler == NULL) {
            ++contract_violations;
            continue;
        }
        registered_event_handler(registered_event_argument, WIFI_EVENT,
                                 event.event_id, NULL);
    }
}

esp_err_t esp_event_loop_create_default(void)
{
    esp_err_t status = api_result(API_EVENT_LOOP);
    if (!timer_armed)
        ++contract_violations;
    if (status == ESP_OK)
        ++event_loop_created;
    return status;
}

esp_err_t esp_event_handler_register(esp_event_base_t event_base,
                                     int32_t event_id,
                                     esp_event_handler_t event_handler,
                                     void *event_handler_arg)
{
    esp_err_t status = api_result(API_EVENT_REGISTER);
    if (event_base != WIFI_EVENT || event_id != ESP_EVENT_ANY_ID ||
        event_handler == NULL || event_handler_arg != NULL ||
        event_loop_created != 1 || observer_registered != 0)
        ++contract_violations;
    if (status == ESP_OK) {
        registered_event_handler = event_handler;
        registered_event_argument = event_handler_arg;
        ++observer_registered;
    }
    return status;
}

esp_err_t esp_event_handler_unregister(esp_event_base_t event_base,
                                       int32_t event_id,
                                       esp_event_handler_t event_handler)
{
    esp_err_t status = api_result(API_EVENT_UNREGISTER);
    if (event_base != WIFI_EVENT || event_id != ESP_EVENT_ANY_ID ||
        event_handler != registered_event_handler || observer_registered != 1)
        ++contract_violations;
    if (inject_event_during_unregister && registered_event_handler != NULL)
        registered_event_handler(registered_event_argument, WIFI_EVENT,
                                 WIFI_EVENT_STA_START, NULL);
    if (status == ESP_OK) {
        registered_event_handler = NULL;
        registered_event_argument = NULL;
        observer_registered = 0;
    }
    return status;
}

esp_err_t esp_hosted_init(void *configuration)
{
    esp_err_t status = api_result(API_HOSTED_INIT);
    if (configuration != NULL || observer_registered != 1 ||
        hosted_initializations != 0)
        ++contract_violations;
    if (status == ESP_OK)
        ++hosted_initializations;
    return status;
}

esp_err_t esp_wifi_init(const wifi_init_config_t *configuration)
{
    esp_err_t status = api_result(API_WIFI_INIT);
    if (configuration == NULL || configuration->nvs_enable != 0 ||
        hosted_initializations != 1 || observer_registered != 1 ||
        wifi_initializations != 0)
        ++contract_violations;
    if (status == ESP_OK)
        ++wifi_initializations;
    return status;
}

esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{
    esp_err_t status = api_result(API_SET_MODE);
    if (mode != WIFI_MODE_STA || wifi_initializations != 1 ||
        station_mode_calls != 0)
        ++contract_violations;
    if (status == ESP_OK)
        ++station_mode_calls;
    return status;
}

esp_err_t esp_wifi_start(void)
{
    esp_err_t status = api_result(API_WIFI_START);
    if (observer_registered != 1 || station_mode_calls != 1)
        ++contract_violations;
    if (status == ESP_OK) {
        ++successful_starts;
        if (configured_event_behavior != EVENTS_NONE) {
            int32_t event_id = WIFI_EVENT_STA_START;
            uint64_t delay = configured_event_behavior == EVENTS_DELAYED ? 12 : 2;
            if (configured_event_behavior == EVENTS_WRONG_ORDER && successful_starts == 1)
                event_id = WIFI_EVENT_STA_STOP;
            if (configured_event_behavior == EVENTS_BEFORE_ACK)
                delay = 0;
            schedule_event(event_id, delay);
            if (configured_event_behavior == EVENTS_DUPLICATE_START &&
                successful_starts == 1)
                schedule_event(WIFI_EVENT_STA_START, 12);
            if (configured_event_behavior == EVENTS_CONNECTED &&
                successful_starts == 1)
                schedule_event(WIFI_EVENT_STA_CONNECTED, 12);
            if (configured_event_behavior == EVENTS_BEFORE_ACK)
                deliver_due_events();
        }
    }
    return status;
}

esp_err_t esp_wifi_stop(void)
{
    esp_err_t status = api_result(API_WIFI_STOP);
    if (observer_registered != 1 || station_mode_calls != 1)
        ++contract_violations;
    if (status == ESP_OK) {
        ++successful_stops;
        if (configured_event_behavior != EVENTS_NONE) {
            uint64_t delay = configured_event_behavior == EVENTS_DELAYED ? 12 : 2;
            if (configured_event_behavior == EVENTS_BEFORE_ACK)
                delay = 0;
            schedule_event(WIFI_EVENT_STA_STOP, delay);
            if (configured_event_behavior == EVENTS_BEFORE_ACK)
                deliver_due_events();
        }
    }
    return status;
}

int64_t esp_timer_get_time(void)
{
    return mock_time_us;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *arguments,
                           esp_timer_handle_t *handle)
{
    esp_err_t status = api_result(API_TIMER_CREATE);
    if (arguments == NULL || arguments->callback == NULL ||
        arguments->name == NULL ||
        strcmp(arguments->name, "reference_lifecycle_deadline") != 0 ||
        handle == NULL || registered_timer_callback != NULL)
        ++contract_violations;
    if (arguments != NULL)
        registered_timer_callback = arguments->callback;
    mock_time_us += timer_create_cost_us;
    if (status == ESP_OK && handle != NULL)
        *handle = &timer_identity;
    return status;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t handle, uint64_t timeout_us)
{
    esp_err_t status = api_result(API_TIMER_START);
    if (handle != &timer_identity || timeout_us == 0 ||
        timeout_us > UINT64_C(40000000) || timer_armed)
        ++contract_violations;
    timer_timeout_us = timeout_us;
    if (status == ESP_OK) {
        timer_armed = 1;
        timer_due_us = mock_time_us + (int64_t)timeout_us;
    }
    return status;
}

esp_err_t esp_timer_stop(esp_timer_handle_t handle)
{
    esp_err_t status = api_result(API_TIMER_STOP);
    if (handle != &timer_identity || !timer_armed || timer_fired)
        ++contract_violations;
    if (status == ESP_OK)
        timer_armed = 0;
    return status;
}

void vTaskDelay(TickType_t ticks)
{
    ++yield_calls;
    if (ticks == 0 || ticks > pdMS_TO_TICKS(10))
        ++zero_or_long_yields;
    mock_ticks += ticks;
    mock_time_us += (int64_t)(ticks * MOCK_TICK_PERIOD_US);
    deliver_due_events();
    if (timer_armed && !timer_fired && mock_time_us >= timer_due_us) {
        timer_fired = 1;
        registered_timer_callback(NULL);
    }
}

_Noreturn void esp_system_abort(const char *reason)
{
    ++abort_calls;
    (void)snprintf(abort_reason, sizeof(abort_reason), "%s", reason);
    if (abort_armed)
        longjmp(abort_environment, 1);
    (void)fprintf(stderr, "unexpected abort outside harness: %s\n", reason);
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
    contract_violations = 0;
    abort_calls = 0;
    abort_armed = 0;
    abort_reason[0] = '\0';
    error_log[0] = '\0';
    info_log[0] = '\0';
    registered_event_handler = NULL;
    registered_event_argument = NULL;
    event_loop_created = 0;
    observer_registered = 0;
    hosted_initializations = 0;
    wifi_initializations = 0;
    station_mode_calls = 0;
    successful_starts = 0;
    successful_stops = 0;
    configured_event_behavior = EVENTS_NORMAL;
    inject_event_during_unregister = 0;
    memset(pending_events, 0, sizeof(pending_events));
    pending_event_count = 0;
    mock_ticks = 0;
    mock_time_us = 0;
    yield_calls = 0;
    zero_or_long_yields = 0;
    registered_timer_callback = NULL;
    timer_armed = 0;
    timer_fired = 0;
    timer_due_us = 0;
    timer_timeout_us = 0;
    timer_create_cost_us = 0;
    atomic_store(&phase, "application startup");
    atomic_store(&complete, false);
    atomic_store(&event_violation, false);
    atomic_store(&event_count, 0);
    atomic_store(&start_count, 0);
    atomic_store(&stop_count, 0);
    atomic_store(&connected_count, 0);
    atomic_store(&unexpected_count, 0);
    for (unsigned index = 0; index < EXPECTED_EVENTS; ++index)
        atomic_store(&event_ids[index], -1);
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

static void test_successful_observation(void)
{
    int aborted;
    ++cases;
    reset_mocks();
    timer_create_cost_us = 6500;
    configured_event_behavior = EVENTS_DELAYED;
    aborted = run_app_catching_abort();
    check(aborted == 0, "expected lifecycle observations complete");
    check(abort_calls == 0, "successful observation does not abort");
    check(api_calls[API_EVENT_LOOP] == 1 && api_calls[API_EVENT_REGISTER] == 1 &&
          api_calls[API_EVENT_UNREGISTER] == 1 && observer_registered == 0,
          "one observer is registered before host init and closed before counts");
    check(api_calls[API_HOSTED_INIT] == 1 && api_calls[API_WIFI_INIT] == 1 &&
          api_calls[API_SET_MODE] == 1,
          "official host and one NVS-off STA init are used");
    check(api_calls[API_WIFI_START] == 4, "initial start and three cycle starts are acknowledged");
    check(api_calls[API_WIFI_STOP] == 3, "three cycle stops are acknowledged");
    check(api_calls[API_TIMER_CREATE] == 1 && api_calls[API_TIMER_START] == 1 &&
          api_calls[API_TIMER_STOP] == 1, "one aggregate timer is created, armed, and stopped");
    check(atomic_load(&event_count) == 7, "exactly seven lifecycle events are observed");
    check(atomic_load(&start_count) == 4 && atomic_load(&stop_count) == 3,
          "observed START and STOP counts are exact");
    check(atomic_load(&connected_count) == 0 && atomic_load(&unexpected_count) == 0,
          "no connected or other Wi-Fi event is observed");
    check(atomic_load(&event_violation) == false, "event sequence has no violation");
    check(atomic_load(&complete), "successful observation marks completion");
    check(contract_violations == 0, "mocked IDF boundary arguments satisfy contract");
    check(zero_or_long_yields == 0 && yield_calls > 0,
          "event waits use bounded nonzero task yields");
    check(timer_timeout_us == UINT64_C(39993500) && timer_due_us == INT64_C(40000000),
          "timer uses remaining aggregate budget after timer-creation time");
    check(strstr(info_log,
          "REFERENCE result=observed scope=LIFECYCLE_OBSERVATION starts=4 stops=3 connected=0 unexpected=0 events=7 rpc_start=4 rpc_stop=3 quiet_ms=250") != NULL,
          "summary labels the narrow observation scope and exact counts");
    const char *start_ack = strstr(info_log, "RPC_ACK op=START cycle=0 status=0");
    const char *start_event = strstr(info_log, "OBS event sequence=0 id=2 starts=1 stops=0");
    check(start_ack != NULL && start_event != NULL && start_ack < start_event,
          "RPC acknowledgment is distinct from the later delivered event");
    for (unsigned sequence = 0; sequence < EXPECTED_EVENTS; ++sequence) {
        int expected_id = (sequence % 2 == 0) ? WIFI_EVENT_STA_START : WIFI_EVENT_STA_STOP;
        char evidence[128];
        (void)snprintf(evidence, sizeof(evidence),
            "OBS event sequence=%u id=%d", sequence, expected_id);
        check(strstr(info_log, evidence) != NULL, "numeric event ID and order are logged");
        check(atomic_load(&event_ids[sequence]) == expected_id,
              "observed event sequence matches expected lifecycle order");
    }
    if (registered_timer_callback != NULL) {
        aborted = run_callback_catching_abort(registered_timer_callback);
        check(aborted == 0 && abort_calls == 0,
              "expired timer callback ignores completed observation");
    }
}

static void test_no_events_reaches_deadline(void)
{
    int aborted;
    ++cases;
    reset_mocks();
    configured_event_behavior = EVENTS_NONE;
    aborted = run_app_catching_abort();
    check(aborted == 1, "missing event reaches aggregate fail-stop");
    check(abort_calls == 1 && timer_fired == 1, "deadline timer fail-stops once");
    check(mock_time_us == INT64_C(40000000), "no-event run stops at the 40-second budget");
    check(api_calls[API_WIFI_START] == 1 && api_calls[API_WIFI_STOP] == 0,
          "no-event path does not continue or retry lifecycle operations");
    check(strstr(error_log, "status=263") != NULL,
          "deadline failure reports numeric timeout status");
    check(strstr(info_log, "REFERENCE result=observed") == NULL,
          "no-event path emits no observation result");
    check(zero_or_long_yields == 0, "deadline polling uses bounded yields");
}

static void test_late_duplicate_rejected(void)
{
    int aborted;
    ++cases;
    reset_mocks();
    configured_event_behavior = EVENTS_DUPLICATE_START;
    aborted = run_app_catching_abort();
    check(aborted == 1, "delayed duplicate during quiet interval is rejected");
    check(abort_calls == 1, "duplicate event fail-stops once");
    check(atomic_load(&event_count) == 2 && atomic_load(&start_count) == 2,
          "duplicate is counted as the second observed event");
    check(atomic_load(&event_violation), "duplicate violates expected event order");
    check(api_calls[API_WIFI_START] == 1 && api_calls[API_WIFI_STOP] == 0,
          "duplicate fails before the next lifecycle RPC");
    check(strstr(error_log, "status=105") != NULL,
          "duplicate reports numeric invalid-response status");
}

static void test_unexpected_connected_rejected(void)
{
    int aborted;
    ++cases;
    reset_mocks();
    configured_event_behavior = EVENTS_CONNECTED;
    aborted = run_app_catching_abort();
    check(aborted == 1, "unexpected connected event is rejected");
    check(abort_calls == 1, "unexpected connected event fail-stops once");
    check(atomic_load(&event_count) == 2 && atomic_load(&start_count) == 1 &&
          atomic_load(&connected_count) == 1 && atomic_load(&unexpected_count) == 1,
          "connected event appears in numeric counters");
    check(api_calls[API_WIFI_START] == 1 && api_calls[API_WIFI_STOP] == 0,
          "unexpected connected event fails before the next lifecycle RPC");
    check(strstr(error_log, "status=105") != NULL,
          "unexpected connected event reports numeric invalid-response status");
}

static void test_wrong_order_rejected(void)
{
    int aborted;
    ++cases;
    reset_mocks();
    configured_event_behavior = EVENTS_WRONG_ORDER;
    aborted = run_app_catching_abort();
    check(aborted == 1, "out-of-order STOP is rejected while START is expected");
    check(atomic_load(&event_count) == 1 && atomic_load(&stop_count) == 1 &&
          atomic_load(&event_violation), "wrong-order event is counted and rejected");
    check(api_calls[API_WIFI_START] == 1 && api_calls[API_WIFI_STOP] == 0,
          "wrong-order event fails before the next lifecycle RPC");
}

static void test_event_before_ack(void)
{
    int aborted;
    ++cases;
    reset_mocks();
    configured_event_behavior = EVENTS_BEFORE_ACK;
    aborted = run_app_catching_abort();
    check(aborted == 0 && atomic_load(&event_count) == 7,
          "delivered events can precede their RPC acknowledgments");
    const char *event = strstr(info_log, "OBS event sequence=0 id=2");
    const char *ack = strstr(info_log, "RPC_ACK op=START cycle=0 status=0");
    check(event != NULL && ack != NULL && event < ack,
          "event and later RPC acknowledgment remain separately logged");
}

static void test_budget_charges_timer_creation(void)
{
    int aborted;
    ++cases;
    reset_mocks();
    timer_create_cost_us = INT64_C(40000000);
    aborted = run_app_catching_abort();
    check(aborted == 1 && abort_calls == 1,
          "timer creation consuming the budget fails immediately");
    check(api_calls[API_TIMER_CREATE] == 1 && api_calls[API_TIMER_START] == 0,
          "expired setup does not arm a refreshed timer");
    check(api_calls[API_EVENT_LOOP] == 0 && api_calls[API_HOSTED_INIT] == 0,
          "expired setup performs no host or Wi-Fi initialization");
    check(strstr(error_log, "status=263") != NULL,
          "setup budget failure reports numeric timeout status");
}

static void test_callback_during_unregister_is_rejected(void)
{
    int aborted;
    ++cases;
    reset_mocks();
    inject_event_during_unregister = 1;
    aborted = run_app_catching_abort();
    check(aborted == 1 && abort_calls == 1,
          "event observed while unregistering prevents success");
    check(api_calls[API_EVENT_UNREGISTER] == 1 &&
          atomic_load(&event_count) == 8 && atomic_load(&event_violation),
          "callback in progress is included before unregister returns");
    check(api_calls[API_TIMER_STOP] == 0 && !atomic_load(&complete),
          "final validation and timer completion do not follow an in-flight duplicate");
}

static void test_queued_event_after_unregister_is_ignored(void)
{
    int aborted;
    ++cases;
    reset_mocks();
    aborted = run_app_catching_abort();
    check(aborted == 0 && observer_registered == 0,
          "successful observation unregisters its local handler");
    schedule_event(WIFI_EVENT_STA_START, 0);
    deliver_due_events();
    check(atomic_load(&event_count) == 7 && atomic_load(&start_count) == 4 &&
          !atomic_load(&event_violation),
          "queued delivery after unregister cannot mutate the closed observation");
    check(contract_violations == 0, "closed observer safely skips queued delivery");
}

static void test_api_failure(enum mock_api failing_api, enum mock_api blocked_api,
                             const char *label)
{
    int aborted;
    ++cases;
    reset_mocks();
    api_results[failing_api] = -37;
    aborted = run_app_catching_abort();
    check(aborted == 1, label);
    check(abort_calls == 1, "API error fail-stops once");
    check(api_calls[failing_api] == 1, "failed API is not retried");
    if (blocked_api < API_COUNT)
        check(api_calls[blocked_api] == 0, "later API is skipped after failure");
    check(strstr(error_log, "status=-37") != NULL,
          "API failure preserves numeric return status");
    check(strstr(info_log, "REFERENCE result=observed") == NULL,
          "API failure emits no observation result");
}

int main(void)
{
    test_successful_observation();
    test_no_events_reaches_deadline();
    test_late_duplicate_rejected();
    test_unexpected_connected_rejected();
    test_api_failure(API_HOSTED_INIT, API_WIFI_INIT, "Hosted init error fails");
    test_api_failure(API_WIFI_START, API_WIFI_STOP, "start acknowledgment error fails");
    test_api_failure(API_WIFI_STOP, API_TIMER_STOP, "stop acknowledgment error fails");
    test_api_failure(API_EVENT_UNREGISTER, API_TIMER_STOP, "observer unregister error fails");
    test_api_failure(API_TIMER_STOP, API_COUNT, "timer stop error fails");
    test_wrong_order_rejected();
    test_event_before_ack();
    test_budget_charges_timer_creation();
    test_callback_during_unregister_is_rejected();
    test_queued_event_after_unregister_is_ignored();

    if (failures != 0) {
        (void)fprintf(stderr, "FAIL: %u of %u checks failed across %u cases\n",
                      failures, checks, cases);
        return 1;
    }
    (void)printf("PASS: %u cases, %u checks\n", cases, checks);
    return 0;
}
'''


FORBIDDEN_SOURCE_TOKENS = (
    "esp_wifi_set_config",
    "esp_wifi_connect",
    "esp_wifi_disconnect",
    "esp_wifi_scan_start",
    "nvs_flash_init",
    "esp_wifi_deinit",
    "esp_hosted_deinit",
    "password",
    "ssid",
)


def strip_includes(source_path: Path) -> str:
    source = source_path.read_text(encoding="utf-8")
    app_source = "\n".join(
        line for line in source.splitlines()
        if not line.lstrip().startswith("#include ")
    )
    if "void app_main(void)" not in app_source:
        raise ValueError(f"app_main not found in {source_path}")
    for token in FORBIDDEN_SOURCE_TOKENS:
        if token in app_source.lower():
            raise ValueError(f"forbidden API or secret-related token in source: {token}")
    return app_source + "\n"


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(
        description="Test complete lifecycle.c with mocked IDF APIs"
    )
    parser.add_argument("source", type=Path, help="path to main/lifecycle.c")
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

    with tempfile.TemporaryDirectory(prefix="hosted-lifecycle-test-") as tmp:
        generated_source = Path(tmp) / "test_lifecycle.c"
        generated_source.write_text(MOCKS + app_source + HARNESS, encoding="utf-8")

        for optimization in ("-O0", "-O2"):
            binary = Path(tmp) / f"test_lifecycle_{optimization[2:]}"
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
            print(f"{optimization}: compiling actual app source with ASan and UBSan")
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
