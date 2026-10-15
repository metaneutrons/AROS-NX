/* Development-only trace in one retained app sector, not an updater.
 * Fresh trace before USB init; later boots never alter an earlier capture. */
#include "trace.h"
#include "flash_window.h"
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_mac.h"
#include "esp_secure_boot.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#if defined(CONFIG_SECURE_BOOT) || defined(CONFIG_SECURE_SIGNED_APPS)
#error Persistent reference does not support secure padding or signed images
#endif
#if defined(CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED)
#error Staged reference must retain SDK dangerous-write protection
#endif

static struct hs_journal_store store;
static struct hs_trace_state state;
static uint8_t *work;
static SemaphoreHandle_t trace_lock;
uintptr_t hs_flash_current_owner(void)
{
    return xPortInIsrContext()?0:(uintptr_t)xTaskGetCurrentTaskHandle();
}
static bool rd(void *ctx,uint32_t off,void *p,size_t n)
{ return esp_partition_read(ctx,off,p,n)==ESP_OK; }
static bool erase(void *ctx,uint32_t off,size_t n)
{
    if(off!=HS_JOURNAL_OFFSET || n!=HS_JOURNAL_SECTOR ||
       !hs_flash_window_begin(HS_FLASH_ERASE))return false;
    esp_err_t result=esp_partition_erase_range(ctx,off,n);
    hs_flash_window_end();
    return result==ESP_OK;
}
static bool wr(void *ctx,uint32_t off,const void *p,size_t n)
{
    if(off<HS_JOURNAL_OFFSET || n!=HS_TRACE_RECORD ||
       off-HS_JOURNAL_OFFSET>HS_JOURNAL_SECTOR-HS_TRACE_RECORD ||
       (off-HS_JOURNAL_OFFSET)%HS_TRACE_RECORD!=0 ||
       !hs_flash_window_begin(HS_FLASH_WRITE))return false;
    esp_err_t result=esp_partition_write(ctx,off,p,n);
    hs_flash_window_end();
    return result==ESP_OK;
}
static void report(int result)
{
    ESP_LOGI("HSTrace","result=%d %s next=%u active=%u",result,
        hs_trace_result_name(result),state.next,(unsigned)state.active);
}
void hs_reference_trace_start(void)
{
    if(esp_secure_boot_enabled()) {
        ESP_LOGE("HSTrace","refused: hardware secure boot");return;
    }
    const esp_partition_t *part=esp_partition_find_first(ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_APP_OTA_0,"ota_0");
    if(!part){ESP_LOGE("HSTrace","refused: ota_0 absent");return;}
    trace_lock=xSemaphoreCreateMutex();
    work=malloc(HS_JOURNAL_SECTOR);
    if(!work || !trace_lock) {
        ESP_LOGE("HSTrace","refused: memory/mutex allocation");
        free(work);work=NULL;return;
    }
    store=(struct hs_journal_store){.ctx=(void *)part,.address=part->address,
        .size=part->size,.encrypted=part->encrypted,
        .debug_connected=usb_serial_jtag_is_connected(),
        .preimage_crc=HS_REFERENCE_PREIMAGE_CRC32,.read=rd,.erase=erase,.write=wr};
    if(esp_efuse_mac_get_default(store.mac)!=ESP_OK) {
        ESP_LOGE("HSTrace","refused: MAC unavailable");return;
    }
    char text[HS_TRACE_TEXT_MAX+1];
    int n=snprintf(text,sizeof(text),
        "stage=app_main_enter us=%lld reset=%d debug_sof=%u\n",
        (long long)esp_timer_get_time(),(int)esp_reset_reason(),
        (unsigned)store.debug_connected);
    if(n<=0 || (size_t)n>=sizeof(text))return;
    report(hs_trace_begin(&store,&state,text,(size_t)n,work,HS_JOURNAL_SECTOR));
}
void hs_reference_trace_event(const char *stage,int error,int line)
{
    char text[HS_TRACE_TEXT_MAX+1];
    int n=snprintf(text,sizeof(text),
        "stage=%s us=%lld error=%d line=%d debug_sof=%u\n",stage,
        (long long)esp_timer_get_time(),error,line,
        (unsigned)usb_serial_jtag_is_connected());
    if(n<=0 || (size_t)n>=sizeof(text) || !work || !trace_lock)return;
    if(xSemaphoreTake(trace_lock,pdMS_TO_TICKS(100))!=pdTRUE) {
        ESP_LOGE("HSTrace","event omitted: mutex timeout");return;
    }
    if(state.active)
        report(hs_trace_append(&store,&state,text,(size_t)n,work,HS_JOURNAL_SECTOR));
    xSemaphoreGive(trace_lock);
}
void hs_reference_trace_sample(const char *text,size_t n)
{
    static bool attempted;
    if(attempted)return;
    attempted=true;
    if(!work || !trace_lock)return;
    if(xSemaphoreTake(trace_lock,pdMS_TO_TICKS(100))!=pdTRUE) {
        ESP_LOGE("HSTrace","sample omitted: mutex timeout");return;
    }
    unsigned part=0;
    for(;n && state.active;++part) {
        char record[HS_TRACE_TEXT_MAX+1];
        size_t take=n>180?180:n;
        int bytes=snprintf(record,sizeof(record),"stage=sample6 part=%u\n%.*s",
            part,(int)take,text);
        if(bytes<=0 || (size_t)bytes>=sizeof(record))break;
        report(hs_trace_append(&store,&state,record,(size_t)bytes,work,HS_JOURNAL_SECTOR));
        text+=take;n-=take;
    }
    if(n==0 && state.active) {
        static const char done[]="stage=sample6_complete ok=1\n";
        report(hs_trace_append(&store,&state,done,sizeof(done)-1,work,HS_JOURNAL_SECTOR));
    }
    xSemaphoreGive(trace_lock);
}
