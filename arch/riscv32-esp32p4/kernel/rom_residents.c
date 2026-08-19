/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Standard AmigaOS / AROS ROM Resident Table & Coldstart AutoInit Scanner
          for ESP32-P4 on AROS.
*/

#include "types.h"
#include "../include/rom_resident.h"
#include "../include/board.h"
#include "../boot/uart.h"

/* Forward declare subsystem initializers */
extern int p4gfx_init(void);
extern int p4touch_init(void);
extern int sdmmc_host_init(void);
extern int dwc2_hcd_init(void);
extern int p4radio_init(void);
extern int emac_p4_init(const UBYTE *mac);
extern int p4audio_i2s_init(ULONG rate);
extern int ppa_p4_init(void);
extern int gpio_p4_init(void);

static int resident_init_gfx(void)      { return p4gfx_init(); }
static int resident_init_gpio(void)     { return gpio_p4_init(); }
static int resident_init_touch(void)    { return p4touch_init(); }
static int resident_init_sd(void)       { return sdmmc_host_init(); }
static int resident_init_usb(void)      { return dwc2_hcd_init(); }
static int resident_init_radio(void)    { return p4radio_init(); }
static int resident_init_emac(void)     { return emac_p4_init(NULL); }
static int resident_init_audio(void)    { return p4audio_i2s_init(44100); }
static int resident_init_ppa(void)      { return ppa_p4_init(); }

/*
 * ROM Resident Table Definitions (ordered by descending rt_Pri priority)
 */
static const struct Resident g_rom_residents[] = {
    {
        .rt_MatchWord   = RTC_MATCHWORD,
        .rt_MatchTag    = (struct Resident *)&g_rom_residents[0],
        .rt_EndSkip     = (APTR)&g_rom_residents[1],
        .rt_Flags       = RTF_COLDSTART,
        .rt_Version     = 1,
        .rt_Type        = NT_RESOURCE,
        .rt_Pri         = 85,
        .rt_Name        = "ppa.resource",
        .rt_IdString    = "ppa.resource 1.0 (19.08.2026) 2D GPU Blitter\r\n",
        .rt_Init        = (APTR)resident_init_ppa
    },
    {
        .rt_MatchWord   = RTC_MATCHWORD,
        .rt_MatchTag    = (struct Resident *)&g_rom_residents[1],
        .rt_EndSkip     = (APTR)&g_rom_residents[2],
        .rt_Flags       = RTF_COLDSTART,
        .rt_Version     = 1,
        .rt_Type        = NT_RESOURCE,
        .rt_Pri         = 82,
        .rt_Name        = "gpio.resource",
        .rt_IdString    = "gpio.resource 1.0 (19.08.2026) GPIO Matrix & Buttons\r\n",
        .rt_Init        = (APTR)resident_init_gpio
    },
    {
        .rt_MatchWord   = RTC_MATCHWORD,
        .rt_MatchTag    = (struct Resident *)&g_rom_residents[2],
        .rt_EndSkip     = (APTR)&g_rom_residents[3],
        .rt_Flags       = RTF_COLDSTART,
        .rt_Version     = 1,
        .rt_Type        = NT_RESOURCE,
        .rt_Pri         = 80,
        .rt_Name        = "p4gfx.hidd",
        .rt_IdString    = "p4gfx.hidd 1.0 (19.08.2026) MIPI-DSI Display\r\n",
        .rt_Init        = (APTR)resident_init_gfx
    },
    {
        .rt_MatchWord   = RTC_MATCHWORD,
        .rt_MatchTag    = (struct Resident *)&g_rom_residents[3],
        .rt_EndSkip     = (APTR)&g_rom_residents[4],
        .rt_Flags       = RTF_COLDSTART,
        .rt_Version     = 1,
        .rt_Type        = NT_RESOURCE,
        .rt_Pri         = 70,
        .rt_Name        = "p4touch.hidd",
        .rt_IdString    = "p4touch.hidd 1.0 (19.08.2026) Capacitive Touch\r\n",
        .rt_Init        = (APTR)resident_init_touch
    },
    {
        .rt_MatchWord   = RTC_MATCHWORD,
        .rt_MatchTag    = (struct Resident *)&g_rom_residents[4],
        .rt_EndSkip     = (APTR)&g_rom_residents[5],
        .rt_Flags       = RTF_COLDSTART,
        .rt_Version     = 1,
        .rt_Type        = NT_DEVICE,
        .rt_Pri         = 60,
        .rt_Name        = "esp32p4_sd.device",
        .rt_IdString    = "esp32p4_sd.device 1.0 (19.08.2026) SDMMC Storage\r\n",
        .rt_Init        = (APTR)resident_init_sd
    },
    {
        .rt_MatchWord   = RTC_MATCHWORD,
        .rt_MatchTag    = (struct Resident *)&g_rom_residents[5],
        .rt_EndSkip     = (APTR)&g_rom_residents[6],
        .rt_Flags       = RTF_COLDSTART,
        .rt_Version     = 1,
        .rt_Type        = NT_DEVICE,
        .rt_Pri         = 50,
        .rt_Name        = "dwc2.hardware",
        .rt_IdString    = "dwc2.hardware 1.0 (19.08.2026) USB Host Controller\r\n",
        .rt_Init        = (APTR)resident_init_usb
    },
    {
        .rt_MatchWord   = RTC_MATCHWORD,
        .rt_MatchTag    = (struct Resident *)&g_rom_residents[6],
        .rt_EndSkip     = (APTR)&g_rom_residents[7],
        .rt_Flags       = RTF_COLDSTART,
        .rt_Version     = 1,
        .rt_Type        = NT_DEVICE,
        .rt_Pri         = 40,
        .rt_Name        = "esp32c6.device",
        .rt_IdString    = "esp32c6.device 1.0 (19.08.2026) Wi-Fi/802.15.4/Zigbee\r\n",
        .rt_Init        = (APTR)resident_init_radio
    },
    {
        .rt_MatchWord   = RTC_MATCHWORD,
        .rt_MatchTag    = (struct Resident *)&g_rom_residents[7],
        .rt_EndSkip     = (APTR)&g_rom_residents[8],
        .rt_Flags       = RTF_COLDSTART,
        .rt_Version     = 1,
        .rt_Type        = NT_DEVICE,
        .rt_Pri         = 35,
        .rt_Name        = "emac.device",
        .rt_IdString    = "emac.device 1.0 (19.08.2026) 10/100 Ethernet MAC\r\n",
        .rt_Init        = (APTR)resident_init_emac
    },
    {
        .rt_MatchWord   = RTC_MATCHWORD,
        .rt_MatchTag    = (struct Resident *)&g_rom_residents[8],
        .rt_EndSkip     = (APTR)0,
        .rt_Flags       = RTF_COLDSTART,
        .rt_Version     = 1,
        .rt_Type        = NT_DEVICE,
        .rt_Pri         = 30,
        .rt_Name        = "p4audio.device",
        .rt_IdString    = "p4audio.device 1.0 (19.08.2026) I2S Audio DAC\r\n",
        .rt_Init        = (APTR)resident_init_audio
    }
};

#define NUM_RESIDENTS (sizeof(g_rom_residents) / sizeof(g_rom_residents[0]))

/*
 * Standard Exec Coldstart Resident Scanner (InitCode(RTF_COLDSTART))
 */
void exec_init_coldstart_residents(void) {
    uart_puts("[exec] Scanning Kickstart ROM Resident Modules (RTF_COLDSTART)...\n");

    for (ULONG i = 0; i < NUM_RESIDENTS; i++) {
        const struct Resident *res = &g_rom_residents[i];

        if (res->rt_MatchWord == RTC_MATCHWORD && (res->rt_Flags & RTF_COLDSTART)) {
            uart_puts("  [autoinit] Pri ");
            /* Print priority */
            if (res->rt_Pri >= 0) uart_putc('+');
            uart_puts(" -> Initializing: ");
            uart_puts(res->rt_Name);
            uart_puts(" ... ");

            resident_init_func_t init_func = (resident_init_func_t)res->rt_Init;
            if (init_func) {
                int err = init_func();
                if (err == 0) {
                    uart_puts("OK\n");
                } else {
                    uart_puts("WARN\n");
                }
            } else {
                uart_puts("SKIPPED\n");
            }
        }
    }

    uart_puts("[exec] All Kickstart ROM Resident Modules initialized.\n");
}
