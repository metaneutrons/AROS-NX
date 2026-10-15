#include "flash_window.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned calls;
static bool decision;
static uintptr_t current_owner=1;
uintptr_t hs_flash_current_owner(void) { return current_owner; }
bool __real_esp_partition_main_flash_region_safe(size_t address,size_t size)
{ (void)address;(void)size;++calls;return decision; }
extern bool __wrap_esp_partition_main_flash_region_safe(size_t,size_t);
#define CHECK(x) do {if(!(x)){fprintf(stderr,"failure at %d\n",__LINE__);exit(1);}}while(0)
static bool safe(size_t address,size_t size)
{ return __wrap_esp_partition_main_flash_region_safe(address,size); }
int main(void)
{
    CHECK(!safe(0x57000,4096) && calls==1);
    CHECK(!hs_flash_window_begin(HS_FLASH_IDLE));
    CHECK(!hs_flash_window_begin((enum hs_flash_operation)99));
    CHECK(hs_flash_window_begin(HS_FLASH_ERASE));
    CHECK(!hs_flash_window_begin(HS_FLASH_WRITE));
    unsigned before=calls;
    CHECK(safe(0x57000,4096) && calls==before);
    current_owner=2;
    CHECK(!safe(0x57000,4096));
    CHECK(!hs_flash_window_begin(HS_FLASH_WRITE));
    hs_flash_window_end();
    current_owner=0;
    CHECK(!safe(0x57000,4096));
    CHECK(!hs_flash_window_begin(HS_FLASH_WRITE));
    hs_flash_window_end();
    current_owner=1;
    CHECK(safe(0x57000,4096));
    CHECK(!safe(0x57000,256));
    CHECK(!safe(0x56000,4096));
    CHECK(!safe(0x57000,8192));
    hs_flash_window_end();
    CHECK(!safe(0x57000,4096));
    CHECK(hs_flash_window_begin(HS_FLASH_WRITE));
    for(unsigned i=0;i<16;++i)CHECK(safe(0x57000+i*256,256));
    current_owner=2;
    CHECK(!safe(0x57000,256));
    hs_flash_window_end();current_owner=1;
    CHECK(safe(0x57000,256));
    CHECK(!safe(0x56f00,256));
    CHECK(!safe(0x58000,256));
    CHECK(!safe(0x57001,256));
    CHECK(!safe(0x57000,4096));
    CHECK(!safe(0x57000,0));
    CHECK(!safe(SIZE_MAX,256));
    CHECK(!safe(0x57000,SIZE_MAX));
    CHECK(!safe(0x2000,256));
    CHECK(!safe(0x8000,256));
    CHECK(!safe(0xc00000,256));
    decision=true;before=calls;
    CHECK(safe(0x900000,4096) && calls==before+1);
    hs_flash_window_end();decision=false;
    CHECK(!safe(0x57000,256));
    puts("PASS scoped flash window: task/ISR isolation, idle, nested, exact ranges, SDK delegation");
    return 0;
}
