#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_hosted_api.h"
#include "esp_timer.h"
#include "esp_system.h"
#include <stdatomic.h>

/* Full official control path, not a replacement SDIO/RPC implementation.
 * Deployment requires the retained safety patch and independent review. */
static _Atomic(const char *) phase = "application startup";
static atomic_bool complete;
static int64_t deadline;
static esp_timer_handle_t deadline_timer;

static void fail_stop(esp_err_t status)
{
    ESP_LOGE("reference", "REFERENCE result=failed phase=%s status=%d",
        atomic_load(&phase), status);
    esp_system_abort("REFERENCE fail-stop; no automatic retry");
}

static void expired(void *unused)
{
    (void)unused;
    if (!atomic_load(&complete))
        fail_stop(ESP_ERR_TIMEOUT);
}

static void checked(esp_err_t status)
{
    if (status != ESP_OK)
        fail_stop(status);
    if (esp_timer_get_time() >= deadline)
        fail_stop(ESP_ERR_TIMEOUT);
}

#define STEP(name, operation) do { \
    atomic_store(&phase, name); \
    checked(ESP_OK); \
    checked(operation); \
} while (0)

void app_main(void)
{
    deadline = esp_timer_get_time() + INT64_C(40000000);
    const esp_timer_create_args_t timer_args = {
        .callback = expired, .name = "reference_deadline"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &deadline_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(deadline_timer, 40000000));
    ESP_LOGI("reference", "REFERENCE begin budget_ms=40000 reset_gpio=-1");
    STEP("event loop", esp_event_loop_create_default());
    STEP("host initialization", esp_hosted_init(NULL));
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    config.nvs_enable = 0;
    STEP("wifi initialization", esp_wifi_init(&config));
    STEP("station mode", esp_wifi_set_mode(WIFI_MODE_STA));
    STEP("wifi start", esp_wifi_start());
    uint8_t mac[6];
    STEP("MAC query", esp_wifi_get_mac(WIFI_IF_STA, mac));
    bool nonzero = false;
    bool all_ff = true;
    for (unsigned i = 0; i < sizeof(mac); ++i) {
        nonzero |= mac[i] != 0;
        all_ff &= mac[i] == 0xff;
    }
    if (!nonzero || all_ff || (mac[0] & 1))
        fail_stop(ESP_ERR_INVALID_RESPONSE);
    ESP_LOGI("reference", "REFERENCE MAC=%02x:%02x:%02x:%02x:%02x:%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    STEP("blocking scan", esp_wifi_scan_start(NULL, true));
    uint16_t ap_count = 0;
    STEP("scan count", esp_wifi_scan_get_ap_num(&ap_count));
    wifi_ap_record_t records[8];
    uint16_t copied = 0;
    /* The pinned slave rejects get_records for an empty scan. Preserve its
     * firmware and accept the successful scan/count instead of calling it. */
    if (ap_count) {
        copied = 8;
        STEP("scan records", esp_wifi_scan_get_ap_records(&copied, records));
    }
    if (copied != (ap_count < 8 ? ap_count : 8))
        fail_stop(ESP_ERR_INVALID_RESPONSE);
    /* Do not print SSIDs/passwords; no association or network traffic yet. */
    STEP("wifi stop", esp_wifi_stop());
    checked(ESP_OK);
    atomic_store(&complete, true);
    checked(esp_timer_stop(deadline_timer));
    ESP_LOGI("reference", "REFERENCE result=ok scope=MAC_SCAN aps=%u records=%u",
        ap_count, copied);
}
