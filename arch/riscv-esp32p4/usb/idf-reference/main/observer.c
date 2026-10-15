/* Copyright (C) 2026, The AROS Development Team.
 * Reference only: unmodified official CDC example plus read-only USB observation.
 * Explicit development option adds one backed-up flash-sector journal.
 * Not linked into AROS. Never flash IDF's generated bootloader/table.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tusb.h"
#include "sdkconfig.h"
#if defined(HS_REFERENCE_LINK_TRACE)
#include "link_events.h"
#include "device/dcd.h"
_Static_assert(HS_LINK_EVENT_COUNT==DCD_EVENT_COUNT,"Pinned USB event IDs changed");
_Static_assert(DCD_EVENT_BUS_RESET==1 && DCD_EVENT_SETUP_RECEIVED==6 &&
               DCD_EVENT_XFER_COMPLETE==7 && USBD_EVENT_FUNC_CALL==8,
               "Pinned USB event semantics changed");
static struct hs_link_events link_events;
/* queue_event invokes this only AFTER successful enqueue. No MMIO, logging,
 * flash I/O, allocation or scheduler call in this potentially ISR callback. */
void tud_event_hook_cb(uint8_t rhport,uint32_t eventid,bool in_isr)
{
    (void)in_isr;
    hs_link_events_record(&link_events,rhport,eventid);
}
#endif

#if !CONFIG_IDF_TARGET_ESP32P4 || !CONFIG_ESP32P4_SELECTS_REV_LESS_V3 || CONFIG_ESP_REV_MIN_FULL != 100
#error This reference is restricted to the reviewed ESP32-P4 rev1.x target
#endif
#if defined(CONFIG_SPIRAM)
#error PSRAM is not part of this HS reference measurement
#endif
_Static_assert(TINYUSB_PORT_DEFAULT_HS==TINYUSB_PORT_HIGH_SPEED_0,
               "Reference must use native HS, not the Debug/FS connector");
#if defined(HS_REFERENCE_PERSIST)
void hs_reference_trace_start(void);
void hs_reference_trace_event(const char *,int,int);
void hs_reference_trace_sample(const char *,size_t);
void hs_reference_trace_note(const char *,size_t);
#endif

static uint32_t reg(uintptr_t address)
{
    return *(volatile const uint32_t *)address;
}

#if defined(HS_REFERENCE_LINK_TRACE)
static void link_phy_note(const char *stage)
{
    char text[237];
    uint32_t hp=reg(0x500e6018),lp=reg(0x50111048);
    int n;
    /* Do not touch UTMI registers while either clock is gated or reset held. */
    if((hp & (1u<<16)) && (lp & (1u<<30)) && !(lp & 6u)) {
        n=snprintf(text,sizeof(text),
            "stage=%s hp=%08lx lp=%08lx fc=%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx\n",
            stage,(unsigned long)hp,(unsigned long)lp,
            (unsigned long)reg(0x5009c000),(unsigned long)reg(0x5009c004),
            (unsigned long)reg(0x5009c008),(unsigned long)reg(0x5009c00c),
            (unsigned long)reg(0x5009c010),(unsigned long)reg(0x5009c014),
            (unsigned long)reg(0x5009c018),(unsigned long)reg(0x5009c01c));
    } else {
        n=snprintf(text,sizeof(text),"stage=%s hp=%08lx lp=%08lx fc=unread_clock_or_reset\n",
                   stage,(unsigned long)hp,(unsigned long)lp);
    }
    if(n<=0 || (size_t)n>=sizeof(text))return;
#if defined(HS_REFERENCE_PERSIST)
    hs_reference_trace_note(text,(size_t)n);
#endif
    ESP_LOGI("HSLink","%s",text);
}
#endif

static void observer(void *argument)
{
    (void)argument;
    unsigned sample=0;
    for(;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        /* No FIFO pop, W1C acknowledgement or peripheral/clock write. */
#if defined(HS_REFERENCE_LINK_TRACE)
        char text[1024];
#else
        char text[768];
#endif
        int n=snprintf(text,sizeof(text),
            "[HSReference] sample=%u mounted=%u mode=%s\n"
            "ID %08lx CFG2 %08lx CFG4 %08lx\n"
            "IRQ %08lx mask %08lx OTG %08lx AHB %08lx USB %08lx\n"
            "DCFG %08lx DCTL %08lx DSTS %08lx PCGC %08lx\n"
            "HPCLK %08lx LPCLK %08lx SUSPENDM %08lx UTMI %08lx\n"
            "EP0 OUT ctl %08lx size %08lx irq %08lx\n",
            ++sample,(unsigned)tud_mounted(),
#if CONFIG_TINYUSB_MODE_DMA
            "DMA",
#else
            "PIO",
#endif
            (unsigned long)reg(0x50000040),(unsigned long)reg(0x50000048),
            (unsigned long)reg(0x50000050),(unsigned long)reg(0x50000014),
            (unsigned long)reg(0x50000018),(unsigned long)reg(0x50000000),
            (unsigned long)reg(0x50000008),(unsigned long)reg(0x5000000c),
            (unsigned long)reg(0x50000800),(unsigned long)reg(0x50000804),
            (unsigned long)reg(0x50000808),(unsigned long)reg(0x50000e00),
            (unsigned long)reg(0x500e6018),(unsigned long)reg(0x50111048),
            (unsigned long)reg(0x500e515c),(unsigned long)reg(0x5009c018),
            (unsigned long)reg(0x50000b00),(unsigned long)reg(0x50000b10),
            (unsigned long)reg(0x50000b08));
        if(n<=0 || (size_t)n>=sizeof(text))continue;
#if defined(HS_REFERENCE_LINK_TRACE)
        struct hs_link_counts counts;
        hs_link_events_snapshot(&link_events,&counts);
        uint32_t dsts=reg(0x50000808);
        int extra=snprintf(text+n,sizeof(text)-(size_t)n,
            "[HSLink] queued reset=%lu unplug=%lu sof=%lu suspend=%lu resume=%lu setup=%lu xfer=%lu filtered=%lu\n"
            "[HSLink] dsts=%08lx line=%lu enum=%lu suspend=%lu frame=%lu\n",
            (unsigned long)counts.event[1],(unsigned long)counts.event[2],
            (unsigned long)counts.event[3],(unsigned long)counts.event[4],
            (unsigned long)counts.event[5],(unsigned long)counts.event[6],
            (unsigned long)counts.event[7],
            (unsigned long)counts.ignored,(unsigned long)dsts,
            (unsigned long)((dsts>>22)&3u),(unsigned long)((dsts>>1)&3u),
            (unsigned long)(dsts&1u),(unsigned long)((dsts>>8)&0x3fffu));
        if(extra<=0 || (size_t)extra>=sizeof(text)-(size_t)n)continue;
        n+=extra;
#endif
#if defined(HS_REFERENCE_PERSIST)
        /* Capture before writing flash/logging; never reclassify a later FS
         * boot as the preceding HS execution. Wrapper attempts once only. */
        if(sample>=6)hs_reference_trace_sample(text,(size_t)n);
#endif
        ESP_LOGI("HSReference","%s",text);
        if(tud_cdc_n_connected(0)) {
            /* Bounded/nonblocking; partial observation is not a complete dump. */
            size_t queued=tinyusb_cdcacm_write_queue(0,(const uint8_t *)text,(size_t)n);
            (void)tinyusb_cdcacm_write_flush(0,0);
            if(queued!=(size_t)n)ESP_LOGW("HSReference","partial CDC dump %u/%u",
                (unsigned)queued,(unsigned)n);
        }
    }
}

static esp_err_t observed_install(const tinyusb_config_t *config)
{
#if defined(HS_REFERENCE_LINK_TRACE)
    link_phy_note("link_pre_install");
#endif
#if defined(HS_REFERENCE_PERSIST)
    hs_reference_trace_event("usb_install_enter",0,__LINE__);
#endif
#if defined(HS_REFERENCE_INJECT_INSTALL_ERROR)
    (void)config;
    hs_reference_trace_event("usb_install_injected",ESP_ERR_INVALID_STATE,__LINE__);
    esp_err_t result=ESP_ERR_INVALID_STATE;
#else
    esp_err_t result=tinyusb_driver_install(config);
#endif
#if defined(HS_REFERENCE_PERSIST)
    hs_reference_trace_event("usb_install_return",result,__LINE__);
#endif
#if defined(HS_REFERENCE_LINK_TRACE)
    link_phy_note("link_post_install");
#endif
    if(result==ESP_OK) {
        BaseType_t created=xTaskCreate(observer,"HS observer",4096,NULL,1,NULL);
#if defined(HS_REFERENCE_PERSIST)
        hs_reference_trace_event("observer_create",created==pdPASS?0:ESP_ERR_NO_MEM,__LINE__);
#endif
        if(created!=pdPASS)ESP_LOGE("HSReference","observer task unavailable");
    }
    return result;
}

/* Preserve the official example's descriptors, PHY, task and echo path.
 * The install wrapper adds a low-priority observer task; optional persistence
 * is separate from the USB register sampling and disabled by default. */
#define tinyusb_driver_install observed_install
#define app_main reference_app_main
#if defined(HS_REFERENCE_PERSIST)
/* Record checked example outcomes, preserving the ordinary abort on error.
 * Arbitrary panic/ROM/bootloader failures are NOT safely flash-logged here. */
static void observed_check(esp_err_t error,const char *file,int line,
                           const char *function,const char *expression)
{
    hs_reference_trace_event(error==ESP_OK?"example_check_ok":"example_error",error,line);
    (void)file;(void)function;(void)expression;
    if(error==ESP_OK)return;
#if defined(HS_REFERENCE_INJECT_INSTALL_ERROR)
    /* Keep the deliberately failing self-test recoverable without a panic
     * loop. Normal builds retain the exact configured IDF error behavior. */
    hs_reference_trace_event("injected_error_park",error,line);
    for(;;)vTaskDelay(pdMS_TO_TICKS(1000));
#endif
#if defined(NDEBUG)
    /* Match IDF's evaluate-and-ignore configuration. */
    return;
#elif defined(CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT)
    abort();
#else
    _esp_error_check_failed(error,file,line,function,expression);
#endif
}
#undef ESP_ERROR_CHECK
#define ESP_ERROR_CHECK(x) do { \
    esp_err_t trace_error_=(x); \
    observed_check(trace_error_,__FILE__,__LINE__,__ASSERT_FUNC,#x); \
} while(0)
#endif
#include "tusb_serial_device_main.c"
#undef app_main
#undef tinyusb_driver_install

void app_main(void)
{
#if defined(HS_REFERENCE_LINK_TRACE)
    hs_link_events_init(&link_events);
#endif
#if defined(HS_REFERENCE_PERSIST)
    hs_reference_trace_start();
#endif
    reference_app_main();
}
