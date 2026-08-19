/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: IEEE 802.15.4 (Zigbee 3.0 / Thread / Matter) Radio Interface
          for ESP32-C6 / ESP32-H2 companion chip on Seeed Studio D1001.
*/

#ifndef ESP32P4_RADIO_802154_H
#define ESP32P4_RADIO_802154_H

#include "types.h"

/*
 * Pinout mapping for ESP32-C6 companion coprocessor on Seeed D1001 / ESP32-P4
 */
#define D1001_RADIO_CHIP_EN_GPIO     5   /* Active High Power / Hardware Reset */
#define D1001_RADIO_BOOT_GPIO        6   /* High for Run Mode, Low for ROM Boot */
#define D1001_RADIO_IRQ_GPIO         7   /* Data Ready / Packet Available IRQ */

#define D1001_RADIO_UART_TX_GPIO     37  /* ESP32-P4 TX -> C6 RX */
#define D1001_RADIO_UART_RX_GPIO     38  /* ESP32-P4 RX <- C6 TX */
#define D1001_RADIO_UART_RTS_GPIO    10  /* Hardware Flow Control RTS */
#define D1001_RADIO_UART_CTS_GPIO    11  /* Hardware Flow Control CTS */

/* SDIO Bus for high-throughput ESP-Hosted Wi-Fi 6 + 802.15.4 */
#define D1001_RADIO_SDIO_CLK_GPIO    18
#define D1001_RADIO_SDIO_CMD_GPIO    19
#define D1001_RADIO_SDIO_D0_GPIO     14
#define D1001_RADIO_SDIO_D1_GPIO     15
#define D1001_RADIO_SDIO_D2_GPIO     16
#define D1001_RADIO_SDIO_D3_GPIO     17

/*
 * IEEE 802.15.4 Physical Constants
 */
#define IEEE802154_CHAN_MIN          11  /* 2405 MHz */
#define IEEE802154_CHAN_MAX          26  /* 2480 MHz */
#define IEEE802154_MAX_PSDU_LEN      127 /* Maximum PHY Layer Frame Size */
#define IEEE802154_ADDR_LEN_EXT      8   /* 64-bit Extended EUI-64 Address */
#define IEEE802154_ADDR_LEN_SHORT    2   /* 16-bit Short Network Address */

/*
 * OpenThread Spinel Protocol Constants (RCP Mode for Thread / Matter)
 */
#define SPINEL_HEADER_FLAG           0x80
#define SPINEL_CMD_NOOP              0
#define SPINEL_CMD_RESET             1
#define SPINEL_CMD_PROP_VALUE_GET    2
#define SPINEL_CMD_PROP_VALUE_SET    3
#define SPINEL_CMD_PROP_VALUE_IS     4

#define SPINEL_PROP_LAST_STATUS      0
#define SPINEL_PROP_PROTOCOL_VERSION 1
#define SPINEL_PROP_NCP_VERSION      2
#define SPINEL_PROP_INTERFACE_TYPE   3
#define SPINEL_PROP_HWADDR           5
#define SPINEL_PROP_MAC_15_4_PANID   7
#define SPINEL_PROP_MAC_15_4_SADDR   8
#define SPINEL_PROP_PHY_CHAN         32
#define SPINEL_PROP_STREAM_RAW       4096

/* Driver Interface Functions */
int  p4radio_init(void);
int  p4radio_set_channel(UBYTE channel);
int  p4radio_set_pan_id(UWORD pan_id);
int  p4radio_send_frame(const UBYTE *frame, UWORD len);
void p4radio_get_eui64(UBYTE *eui64);

#endif /* ESP32P4_RADIO_802154_H */
