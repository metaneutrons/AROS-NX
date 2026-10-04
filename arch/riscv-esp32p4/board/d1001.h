#ifndef ESP32P4_BOARD_D1001_H
#define ESP32P4_BOARD_D1001_H

/* See display/DISPLAY-CONTRACT.md for measured D1001 wiring and polarity. */
#define P4_BOARD_NAME                   "d1001"
#define P4_BOARD_FLASH_BYTES            0x02000000UL
#define P4_BOARD_PARTITION_OFFSET       0x00008000UL
#define P4_BOARD_BSP_PART_TYPE          0x40
#define P4_BOARD_BSP_PART_LABEL         "arosbsp"
#define P4_BOARD_CONSOLE_USB            1
#define P4_BOARD_I2C0_SDA_GPIO          37
#define P4_BOARD_I2C0_SCL_GPIO          38
#define P4_BOARD_TOUCH_IRQ_GPIO         16
#define P4_BOARD_TOUCH_GSL3670          1
#define P4_BOARD_TOUCH_ADDR             0x40
#define P4_BOARD_I2C1_SDA_GPIO          20
#define P4_BOARD_I2C1_SCL_GPIO          21
#define P4_BOARD_BACKLIGHT_GPIO         14
/* Fixed level until brightness is a runtime setting (Fabian, 2026-10-04;
   it was the 20 % default kept low during display bring-up). */
#define P4_BOARD_BACKLIGHT_PERCENT      50
#define P4_BOARD_SD_HAS_DETECT          1
#define P4_BOARD_SD_DETECT_GPIO         45
#define P4_BOARD_SD_HAS_POWER_GPIO      1
#define P4_BOARD_SD_POWER_GPIO          46
#define P4_BOARD_PSRAM_LDO_CHANNEL       2
#define P4_BOARD_PSRAM_LDO_MV            1800
#define P4_BOARD_TOUCH_FW_PATH          "DEVS:Firmware/silead/gsl3670-d1001.fw"
#define P4_BOARD_TOUCH_FW_FALLBACK      "FLASHDISK0P0:Firmware/silead/gsl3670-d1001.fw"

/* Single-finger perimeter measurement, 2026-09-30. Inclusive endpoints;
   raw Y is mirrored by the mouse HIDD. Visual edges/menu opening verified. */
#define P4_BOARD_TOUCH_X_MIN            16U
#define P4_BOARD_TOUCH_X_MAX            1638U
#define P4_BOARD_TOUCH_Y_MIN            15U
#define P4_BOARD_TOUCH_Y_MAX            874U
#define P4_BOARD_TOUCH_MIRROR_Y         1

/*
 * Panel: JD9365, native 800 x 1280 portrait, shown rotated 90 degrees
 * clockwise as 1280 x 800. Timing is display/DISPLAY-CONTRACT.md's Set A,
 * the set the working reference writes; the history behind each value is
 * at its use in kernel/hardware.h. Power, reset and backlight enable go
 * through the PCA9535 below.
 */
#define P4_BOARD_PANEL_JD9365           1
#define P4_BOARD_PANEL_EXPANDER         1
#define P4_BOARD_PANEL_H_RES            800
#define P4_BOARD_PANEL_V_RES            1280
#define P4_BOARD_PANEL_ROTATE           90
#define P4_BOARD_PANEL_LANES            2
#define P4_BOARD_PANEL_LANE_MBPS        1500
#define P4_BOARD_PANEL_DPI_MHZ          40
#define P4_BOARD_PANEL_HSYNC            20
#define P4_BOARD_PANEL_HBP              20
#define P4_BOARD_PANEL_HFP              40
#define P4_BOARD_PANEL_VSYNC            4
#ifdef P4_PANEL_24BIT
#define P4_BOARD_PANEL_VBP              12
#else
#define P4_BOARD_PANEL_VBP              30
#endif
#define P4_BOARD_PANEL_VFP              30

#define P4_PCA9535_ADDR                0x20
#define P4_EXP_LCD_PWR_EN               (1U << 0)
#define P4_EXP_LCD_RST                  (1U << 2)   /* active low */
#define P4_EXP_BAT_READ_EN              (1U << 6)
#define P4_EXP_LCD_BL_EN                (1U << 7)
#define P4_EXP_PWR_HOLD                 (1U << 8)
#define P4_EXP_BAT_CHARGE_EN            (1U << 10)
#define P4_EXP_AMP_EN                   (1U << 11)
#define P4_EXP_TOUCH_RST                (1U << 12)  /* active low */

/* Last four MiB of arosbsp, at physical flash offset 0xc00000. */
#define P4_BOARD_FLASHDISK_PART_OFFSET 0x003E0000UL
#define P4_BOARD_FLASHDISK_SIZE        0x00400000UL

#endif
