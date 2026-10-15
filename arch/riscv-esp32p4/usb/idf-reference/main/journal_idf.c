/* Explicitly selected development persistence; one attempt after30s/boot.
 * Writes only approved ota_0 relative0x37000 (absolute0x57000), one sector
 * already retained in aros-restore-exact.bin. Never touches metadata/NVS.
 */
#include "journal.h"
#include <stdlib.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_mac.h"
#include "esp_secure_boot.h"
#include "driver/usb_serial_jtag.h"
#include "sdkconfig.h"

#if defined(CONFIG_SECURE_BOOT) || defined(CONFIG_SECURE_SIGNED_APPS)
#error Persistent reference does not support secure padding or signed images
#endif

static bool rd(void *ctx,uint32_t off,void *p,size_t n)
{ return esp_partition_read(ctx,off,p,n)==ESP_OK; }
static bool erase(void *ctx,uint32_t off,size_t n)
{
    return off==HS_JOURNAL_OFFSET && n==HS_JOURNAL_SECTOR &&
        esp_partition_erase_range(ctx,off,n)==ESP_OK;
}
static bool wr(void *ctx,uint32_t off,const void *p,size_t n)
{
    return off==HS_JOURNAL_OFFSET && n==HS_JOURNAL_SECTOR &&
        esp_partition_write(ctx,off,p,n)==ESP_OK;
}
void hs_journal_observe(const char *text,size_t n)
{
    static bool attempted;
    if(attempted)return;
    attempted=true;
    /* Signed images may extend beyond their ordinary checksum/SHA footer.
     * Never infer a spare sector from that footer on a secure-boot board. */
    if(esp_secure_boot_enabled()) {
        ESP_LOGE("HSJournal","secure boot enabled; refused");return;
    }
    const esp_partition_t *part=esp_partition_find_first(ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_APP_OTA_0,"ota_0");
    if(!part){ESP_LOGE("HSJournal","ota_0 absent; refused");return;}
    uint8_t *work=malloc(HS_JOURNAL_SECTOR);
    if(!work){ESP_LOGE("HSJournal","no memory; not retried");return;}
    struct hs_journal_store store={.ctx=(void *)part,.address=part->address,
        .size=part->size,.encrypted=part->encrypted,
        .debug_connected=usb_serial_jtag_is_connected(),
        .preimage_crc=HS_REFERENCE_PREIMAGE_CRC32,.read=rd,.erase=erase,.write=wr};
    if(esp_efuse_mac_get_default(store.mac)!=ESP_OK) {
        ESP_LOGE("HSJournal","MAC unavailable; refused");free(work);return;
    }
    int result=hs_journal_save(&store,text,n,work,HS_JOURNAL_SECTOR);
    ESP_LOGI("HSJournal","result=%d; no retry/overwrite",result);
    size_t saved=(result==HS_JOURNAL_WRITTEN || result==HS_JOURNAL_PRESERVED)
        ? hs_journal_length(work,HS_JOURNAL_SECTOR):0;
    if((result==HS_JOURNAL_WRITTEN || result==HS_JOURNAL_PRESERVED) && saved)
        ESP_LOGI("HSJournal","preserved capture: %.*s",(int)saved,(char *)work+16);
    free(work);
}
