#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"

/* Build-only integration front end. Not approved for deployment: the pinned
 * component still has implicit startup and unadapted reset/retry policies. */
void app_main(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    config.nvs_enable = 0;
    esp_err_t status = esp_wifi_init(&config);
    ESP_LOGI("reference", "wifi_init status=%d", status);
}
