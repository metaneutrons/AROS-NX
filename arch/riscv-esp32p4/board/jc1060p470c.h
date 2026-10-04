#ifndef ESP32P4_BOARD_JC1060P470C_H
#define ESP32P4_BOARD_JC1060P470C_H

/*
 * Guition JC1060P470C (_I_W_Y; sold as JC1060WP470C): JC-ESP32P4-M3 module
 * with an ESP32-P4NRW32 (32 MB PSRAM in package), 16 MB flash and an
 * ESP32-C6 on SDIO; 7" 1024 x 600 JD9165BA MIPI-DSI panel, GT911 touch.
 *
 * Sources, all in the vendor package (JC1060WP470C/): schematic
 * 5-Schematic/JC1060P470C_I_W_Y-V1.0.pdf, the ESP-IDF 5.5.4 demos'
 * esp32_p4_function_ev_board.c and sdkconfig, and the panel dtsi. Every
 * value below is read from those, not measured on this port; the first
 * boot gate on the board is still open (ROADMAP D1, J rows).
 *
 * USB-C "USB2" is the P4's USB-Serial-JTAG (GPIO24/25), the console. RESET
 * (SW2) drives CHIP_PU and BOOT (SW1) GPIO35, so the board can always be
 * hard reset and forced into the ROM loader by hand.
 */
/*
 * Two panel batches, one board ID each (board.mk): v1 is the vendor's
 * "Old_Panel", v2 its "New_Panel". Only the init table and the old batch's
 * horizontal back porch differ.
 */
#if defined(P4_JC1060_PANEL_V1)
#define P4_BOARD_NAME                   "jc1060p470c-v1"
#elif defined(P4_JC1060_PANEL_V2)
#define P4_BOARD_NAME                   "jc1060p470c-v2"
#else
#error "JC1060P470C needs P4_JC1060_PANEL_V1 or _V2 (board.mk)"
#endif
#define P4_BOARD_FLASH_BYTES            0x01000000UL
#define P4_BOARD_PARTITION_OFFSET       0x00008000UL
#define P4_BOARD_BSP_PART_TYPE          0x40
#define P4_BOARD_BSP_PART_LABEL         "arosbsp"
#define P4_BOARD_CONSOLE_USB            1

/* In-package PSRAM on VO2, as the vendor sdkconfig selects (1800 mV). */
#define P4_BOARD_PSRAM_LDO_CHANNEL       2
#define P4_BOARD_PSRAM_LDO_MV            1800

/*
 * Panel: JD9165BA, native 1024 x 600 landscape, so no rotation. Straps on
 * the FPC fix the scan direction (UPDN high, SHLR low); the vendor driver
 * sends MADCTL 0 and no mirror. The LCD bias boost runs whenever 3V3 is
 * up, so there is no power-enable line, and the DSI PHY is on VO3 at
 * 2.5 V like the D1001's.
 */
#define P4_BOARD_PANEL_JD9165           1
#define P4_BOARD_PANEL_H_RES            1024
#define P4_BOARD_PANEL_V_RES            600
#define P4_BOARD_PANEL_ROTATE           0
#define P4_BOARD_PANEL_LANES            2
#define P4_BOARD_PANEL_LANE_MBPS        750
/*
 * The D-PHY PLL reference is PLL_F20M (20 MHz) on this silicon; the vendor
 * firmware's and this port's clock registers agree (JTAG, 2026-10-03), and
 * ESP-IDF computes N = 4, M = 150 for 750 Mbit/s against it.
 */
#define P4_BOARD_DSI_PLLREF_MHZ         20
/*
 * The vendor asks for 52 MHz; ESP-IDF rounds PLL_F240M / 52 to the divider
 * 5, so the demos run at 48 MHz, inside the panel's 40.8-67.2 MHz range.
 * This port takes the same 48 MHz directly (about 56 Hz refresh).
 */
#define P4_BOARD_PANEL_DPI_MHZ          48
/* ...and times the host against the requested 52 MHz, shortening the bridge
   line to 1241 pixels. The factory firmware's registers, read over JTAG on
   2026-10-03, are exactly these (HSA 43, HBP 245, HLINE 2423, bridge 1241). */
#define P4_BOARD_PANEL_DPI_NOMINAL_MHZ  52
#define P4_BOARD_PANEL_HSYNC            24
#ifdef P4_JC1060_PANEL_V1
#define P4_BOARD_PANEL_HBP              160     /* IDF demo; Arduino differs */
#else
#define P4_BOARD_PANEL_HBP              136     /* dtsi and datasheet */
#endif
#define P4_BOARD_PANEL_HFP              160
#define P4_BOARD_PANEL_VSYNC            2
#define P4_BOARD_PANEL_VBP              21
#define P4_BOARD_PANEL_VFP              12

/*
 * Panel reset: the schematic routes FPC1 pin 4 "LCD_RST" to GPIO0 and the
 * vendor BSP header agrees; two other demos name GPIO5 and GPIO27, which
 * the first hardware run has to rule out. Active low.
 */
#define P4_BOARD_PANEL_RESET_GPIO       0
/* MP3202 boost enable, 10 k pull-down: off until driven, active high. */
#define P4_BOARD_BACKLIGHT_GPIO         23

/*
 * Touch: GT911 on the panel FPC, I2C on GPIO7/8 with 5.1 k pull-ups (the
 * bus also carries the ES8311 at 0x18, the RX8025T RTC and the camera's
 * SCCB). INT and RST are wired to GPIO21/22; the level of INT while RST
 * rises selects address 0x5D (low) or 0x14 (high). The panel is not
 * rotated, so raw X/Y map straight to the screen; the range comes from
 * the controller's own configuration at probe time.
 */
#define P4_BOARD_TOUCH_GT911            1
#define P4_BOARD_I2C0_SDA_GPIO          7
#define P4_BOARD_I2C0_SCL_GPIO          8
#define P4_BOARD_TOUCH_ADDR             0x5D
#define P4_BOARD_TOUCH_ADDR_ALT         0x14
#define P4_BOARD_TOUCH_IRQ_GPIO         21
#define P4_BOARD_TOUCH_RST_GPIO         22
#define P4_BOARD_TOUCH_MIRROR_Y         0
#define P4_BOARD_TOUCH_X_MIN            0U
#define P4_BOARD_TOUCH_X_MAX            1023U
#define P4_BOARD_TOUCH_Y_MIN            0U
#define P4_BOARD_TOUCH_Y_MAX            599U

/*
 * MicroSD: slot 0 IOMUX pins 39-44, 4-bit, like the D1001. There is no
 * card-detect contact, and the card supply (VO4 through a P-FET whose gate
 * is tied low) is always on: the GPIO45 control resistor is not fitted.
 */
#define P4_BOARD_SD_HAS_DETECT          0
#define P4_BOARD_SD_HAS_POWER_GPIO      0

/* Last four MiB of arosbsp, at physical flash offset 0xc00000. */
#define P4_BOARD_FLASHDISK_PART_OFFSET 0x003E0000UL
#define P4_BOARD_FLASHDISK_SIZE        0x00400000UL

#endif
