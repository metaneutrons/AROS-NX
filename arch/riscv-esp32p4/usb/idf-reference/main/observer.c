/* Copyright (C) 2026, The AROS Development Team.
 * Reference only: unmodified official CDC example plus read-only USB observation.
 * Explicit development option adds one backed-up flash-sector journal.
 * Not linked into AROS. Never flash IDF's generated bootloader/table.
 */
#include <stdint.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tusb.h"
#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32P4 || !CONFIG_ESP32P4_SELECTS_REV_LESS_V3 || CONFIG_ESP_REV_MIN_FULL != 100
#error This reference is restricted to the reviewed ESP32-P4 rev1.x target
#endif
#if defined(CONFIG_SPIRAM)
#error PSRAM is not part of this HS reference measurement
#endif
_Static_assert(TINYUSB_PORT_DEFAULT_HS==TINYUSB_PORT_HIGH_SPEED_0,
               "Reference must use native HS, not the Debug/FS connector");
#if defined(HS_REFERENCE_PERSIST)
void hs_journal_observe(const char *,size_t);
#endif

static uint32_t reg(uintptr_t address)
{
    return *(volatile const uint32_t *)address;
}

static void observer(void *argument)
{
    (void)argument;
    unsigned sample=0;
    for(;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        /* No FIFO pop, W1C acknowledgement or peripheral/clock write. */
        char text[768];
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
#if defined(HS_REFERENCE_PERSIST)
        /* Capture before writing flash/logging; never reclassify a later FS
         * boot as the preceding HS execution. Wrapper attempts once only. */
        if(sample>=6)hs_journal_observe(text,(size_t)n);
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
    esp_err_t result=tinyusb_driver_install(config);
    if(result==ESP_OK && xTaskCreate(observer,"HS observer",4096,NULL,1,NULL)!=pdPASS)
        ESP_LOGE("HSReference","observer task unavailable");
    return result;
}

/* Preserve the official example's descriptors, PHY, task and echo path.
 * The install wrapper adds a low-priority observer task; optional persistence
 * is separate from the USB register sampling and disabled by default. */
#define tinyusb_driver_install observed_install
#define app_main reference_app_main
#include "tusb_serial_device_main.c"
#undef app_main
#undef tinyusb_driver_install

void app_main(void)
{
    reference_app_main();
}
