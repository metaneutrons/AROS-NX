#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_hosted_api.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define LIFECYCLE_BUDGET_US INT64_C(40000000)
#define EVENT_QUIET_US INT64_C(250000)
#define YIELD_MS 10
#define EXPECTED_EVENTS 7U

/* This observes local IDF events around the official Hosted API calls. It is
 * not a producer-drain, attempt-provenance, or peer-completion contract. */
static const int32_t expected_event_ids[EXPECTED_EVENTS] = {
    WIFI_EVENT_STA_START,
    WIFI_EVENT_STA_STOP,
    WIFI_EVENT_STA_START,
    WIFI_EVENT_STA_STOP,
    WIFI_EVENT_STA_START,
    WIFI_EVENT_STA_STOP,
    WIFI_EVENT_STA_START
};

static _Atomic(const char *) phase = "application startup";
static atomic_bool complete;
static atomic_bool event_violation;
static atomic_uint event_count;
static atomic_uint start_count;
static atomic_uint stop_count;
static atomic_uint connected_count;
static atomic_uint unexpected_count;
static atomic_int event_ids[EXPECTED_EVENTS];
static int64_t deadline;
static esp_timer_handle_t deadline_timer;

static void fail_stop(esp_err_t status)
{
    ESP_LOGE("reference",
        "REFERENCE result=failed phase=%s status=%d events=%u starts=%u stops=%u connected=%u unexpected=%u",
        atomic_load(&phase), status,
        atomic_load(&event_count), atomic_load(&start_count),
        atomic_load(&stop_count), atomic_load(&connected_count),
        atomic_load(&unexpected_count));
    esp_system_abort("REFERENCE fail-stop; no automatic retry");
}

static void expired(void *unused)
{
    (void)unused;
    if (!atomic_load(&complete))
        fail_stop(ESP_ERR_TIMEOUT);
}

static void check_observation(void)
{
    if (atomic_load(&event_violation))
        fail_stop(ESP_ERR_INVALID_RESPONSE);
    if (esp_timer_get_time() >= deadline)
        fail_stop(ESP_ERR_TIMEOUT);
}

static void check_api_return(const char *operation, esp_err_t status)
{
    ESP_LOGI("reference", "API_RETURN op=%s status=%d", operation, status);
    if (status != ESP_OK)
        fail_stop(status);
    check_observation();
}

static void check_rpc_ack(const char *operation, unsigned cycle,
                          esp_err_t status)
{
    ESP_LOGI("reference", "RPC_ACK op=%s cycle=%u status=%d",
        operation, cycle, status);
    if (status != ESP_OK)
        fail_stop(status);
    check_observation();
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    (void)event_data;

    unsigned sequence = atomic_load_explicit(&event_count, memory_order_relaxed);
    unsigned starts = atomic_load_explicit(&start_count, memory_order_relaxed);
    unsigned stops = atomic_load_explicit(&stop_count, memory_order_relaxed);
    unsigned connected = atomic_load_explicit(&connected_count, memory_order_relaxed);
    unsigned unexpected = atomic_load_explicit(&unexpected_count, memory_order_relaxed);
    bool known = event_base == WIFI_EVENT;

    if (known && event_id == WIFI_EVENT_STA_START)
        ++starts;
    else if (known && event_id == WIFI_EVENT_STA_STOP)
        ++stops;
    else {
        if (known && event_id == WIFI_EVENT_STA_CONNECTED)
            ++connected;
        ++unexpected;
    }

    if (!known || sequence >= EXPECTED_EVENTS ||
        event_id != expected_event_ids[sequence < EXPECTED_EVENTS ? sequence : 0])
        atomic_store_explicit(&event_violation, true, memory_order_relaxed);

    if (sequence < EXPECTED_EVENTS)
        atomic_store_explicit(&event_ids[sequence], event_id, memory_order_relaxed);
    atomic_store_explicit(&start_count, starts, memory_order_relaxed);
    atomic_store_explicit(&stop_count, stops, memory_order_relaxed);
    atomic_store_explicit(&connected_count, connected, memory_order_relaxed);
    atomic_store_explicit(&unexpected_count, unexpected, memory_order_relaxed);

    /* Publish the fully recorded event after its numeric evidence. The
     * default event loop dispatches these callbacks serially. */
    atomic_store_explicit(&event_count, sequence + 1, memory_order_release);
    ESP_LOGI("reference",
        "OBS event sequence=%u id=%ld starts=%u stops=%u connected=%u unexpected=%u",
        sequence, (long)event_id, starts, stops, connected, unexpected);
}

static void bounded_yield(void)
{
    TickType_t ticks = pdMS_TO_TICKS(YIELD_MS);
    if (ticks == 0)
        ticks = 1;
    vTaskDelay(ticks);
}

static void observe_event(unsigned sequence)
{
    if (sequence >= EXPECTED_EVENTS)
        fail_stop(ESP_ERR_INVALID_RESPONSE);

    atomic_store(&phase, "event observation");
    const unsigned target_count = sequence + 1;
    while (atomic_load_explicit(&event_count, memory_order_acquire) < target_count) {
        check_observation();
        bounded_yield();
        check_observation();
    }

    check_observation();
    if (atomic_load_explicit(&event_count, memory_order_acquire) != target_count ||
        atomic_load_explicit(&event_ids[sequence], memory_order_relaxed) !=
            expected_event_ids[sequence])
        fail_stop(ESP_ERR_INVALID_RESPONSE);

    /* A bounded quiet interval catches a delayed duplicate in the observed
     * window; it does not establish that the producer has drained. */
    const int64_t quiet_deadline = esp_timer_get_time() + EVENT_QUIET_US;
    while (esp_timer_get_time() < quiet_deadline) {
        bounded_yield();
        check_observation();
        if (atomic_load_explicit(&event_count, memory_order_acquire) != target_count)
            fail_stop(ESP_ERR_INVALID_RESPONSE);
    }
}

void app_main(void)
{
    deadline = esp_timer_get_time() + LIFECYCLE_BUDGET_US;
    const esp_timer_create_args_t timer_args = {
        .callback = expired,
        .name = "reference_lifecycle_deadline"
    };
    check_api_return("TIMER_CREATE", esp_timer_create(&timer_args, &deadline_timer));

    const int64_t remaining = deadline - esp_timer_get_time();
    if (remaining <= 0)
        fail_stop(ESP_ERR_TIMEOUT);
    esp_err_t timer_status = esp_timer_start_once(deadline_timer,
        (uint64_t)remaining);
    ESP_LOGI("reference", "API_RETURN op=TIMER_START status=%d", timer_status);
    if (timer_status != ESP_OK)
        fail_stop(timer_status);

    ESP_LOGI("reference",
        "REFERENCE begin budget_ms=40000 quiet_ms=250 cycles=3 mode=STA");
    atomic_store(&phase, "event loop");
    check_api_return("EVENT_LOOP", esp_event_loop_create_default());
    check_api_return("EVENT_REGISTER",
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
            wifi_event_handler, NULL));

    atomic_store(&phase, "host initialization");
    check_api_return("HOSTED_INIT", esp_hosted_init(NULL));
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    config.nvs_enable = 0;
    check_rpc_ack("INIT", 0, esp_wifi_init(&config));
    check_rpc_ack("SET_MODE_STA", 0, esp_wifi_set_mode(WIFI_MODE_STA));

    check_rpc_ack("START", 0, esp_wifi_start());
    observe_event(0);

    for (unsigned cycle = 1; cycle <= 3; ++cycle) {
        check_rpc_ack("STOP", cycle, esp_wifi_stop());
        observe_event((cycle * 2) - 1);
        check_rpc_ack("START", cycle, esp_wifi_start());
        observe_event(cycle * 2);
    }

    /* Close the local callback window before taking final counts. IDF waits
     * for callbacks already dispatching; queued peer events are not drained. */
    atomic_store(&phase, "observer close");
    check_api_return("EVENT_UNREGISTER",
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
            wifi_event_handler));

    atomic_store(&phase, "final counts");
    check_observation();
    if (atomic_load_explicit(&event_count, memory_order_acquire) != EXPECTED_EVENTS ||
        atomic_load(&start_count) != 4 || atomic_load(&stop_count) != 3 ||
        atomic_load(&connected_count) != 0 || atomic_load(&unexpected_count) != 0)
        fail_stop(ESP_ERR_INVALID_RESPONSE);

    atomic_store(&phase, "deadline stop");
    check_api_return("TIMER_STOP", esp_timer_stop(deadline_timer));
    atomic_store(&complete, true);
    ESP_LOGI("reference",
        "REFERENCE result=observed scope=LIFECYCLE_OBSERVATION starts=%u stops=%u connected=%u unexpected=%u events=%u rpc_start=%u rpc_stop=%u quiet_ms=250",
        atomic_load(&start_count), atomic_load(&stop_count),
        atomic_load(&connected_count), atomic_load(&unexpected_count),
        atomic_load(&event_count), 4U, 3U);
}
