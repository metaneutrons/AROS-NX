/* Diagnostic-only workaround for the SDK's whole-running-partition veto.
 * Does NOT disable flash protection: only an adapter-authorized exact erase
 * or aligned record write in the retained sector is exempt. All other calls
 * retain the real SDK decision; read-only checks and locking remain in IDF. */
#include "flash_window.h"
#include "trace.h"
#include <stdatomic.h>

static atomic_uint operation;
static atomic_uintptr_t owner;
extern bool __real_esp_partition_main_flash_region_safe(size_t,size_t);

bool hs_flash_window_begin(enum hs_flash_operation requested)
{
    if(requested!=HS_FLASH_ERASE && requested!=HS_FLASH_WRITE)return false;
    uintptr_t caller=hs_flash_current_owner(),expected=0;
    if(!caller || !atomic_compare_exchange_strong(&owner,&expected,caller))return false;
    atomic_store(&operation,(unsigned)requested);
    return true;
}
void hs_flash_window_end(void)
{
    uintptr_t caller=hs_flash_current_owner();
    if(!caller || caller!=atomic_load(&owner))return;
    atomic_store(&operation,HS_FLASH_IDLE);
    atomic_store(&owner,0);
}
bool __wrap_esp_partition_main_flash_region_safe(size_t address,size_t size)
{
    const size_t start=0x20000u+HS_JOURNAL_OFFSET;
    unsigned current=atomic_load(&operation);
    uintptr_t caller=hs_flash_current_owner();
    if(!caller || caller!=atomic_load(&owner))
        return __real_esp_partition_main_flash_region_safe(address,size);
    if(current==HS_FLASH_ERASE && address==start && size==HS_JOURNAL_SECTOR)
        return true;
    if(current==HS_FLASH_WRITE && size==HS_TRACE_RECORD && address>=start &&
       address-start<=HS_JOURNAL_SECTOR-HS_TRACE_RECORD &&
       (address-start)%HS_TRACE_RECORD==0)
        return true;
    return __real_esp_partition_main_flash_region_safe(address,size);
}
