/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Enterprise-Grade OpenThread Spinel v1.0 HDLC Protocol Engine
          Header for ESP32-C6 / ESP32-H2 on AROS.
*/

#ifndef RISCV32_SPINEL_H
#define RISCV32_SPINEL_H

#include "types.h"

#define SPINEL_FRAME_DELIMITER          0x7E
#define SPINEL_FRAME_ESCAPE             0x7D
#define SPINEL_FRAME_XON                0x11
#define SPINEL_FRAME_XOFF               0x13

#define SPINEL_HEADER_FLAG              0x80
#define SPINEL_HEADER_TID_MASK          0x0F

/* Spinel Commands */
#define SPINEL_CMD_NOOP                 0
#define SPINEL_CMD_RESET                1
#define SPINEL_CMD_PROP_VALUE_GET       2
#define SPINEL_CMD_PROP_VALUE_SET       3
#define SPINEL_CMD_PROP_VALUE_IS        4
#define SPINEL_CMD_PROP_VALUE_INSERT    5
#define SPINEL_CMD_PROP_VALUE_REMOVE    6

/* Spinel Properties */
#define SPINEL_PROP_LAST_STATUS         0
#define SPINEL_PROP_PROTOCOL_VERSION    1
#define SPINEL_PROP_NCP_VERSION         2
#define SPINEL_PROP_INTERFACE_TYPE      3
#define SPINEL_PROP_HWADDR              5
#define SPINEL_PROP_MAC_15_4_PANID      7
#define SPINEL_PROP_MAC_15_4_SADDR      8
#define SPINEL_PROP_PHY_CHAN            32
#define SPINEL_PROP_PHY_TX_POWER        34
#define SPINEL_PROP_MAC_PROMISCUOUS     48
#define SPINEL_PROP_STREAM_RAW          4096

/* Raw Radio Frame Metadata */
struct spinel_rx_frame_meta
{
    BYTE    rssi_dbm;
    UBYTE   lqi;
    ULONG   timestamp_us;
    UWORD   length;
    UBYTE   psdu[128];
};

typedef void (*spinel_rx_callback_t)(const struct spinel_rx_frame_meta *frame, void *user_data);

/* Function Prototypes */
void  spinel_engine_init(spinel_rx_callback_t rx_cb, void *user_data);
UWORD spinel_encode_set_prop_uint8(UBYTE tid, UWORD prop, UBYTE val, UBYTE *out_buf, UWORD out_max);
UWORD spinel_encode_set_prop_uint16(UBYTE tid, UWORD prop, UWORD val, UBYTE *out_buf, UWORD out_max);
UWORD spinel_encode_raw_stream(UBYTE tid, const UBYTE *frame, UWORD len, UBYTE *out_buf, UWORD out_max);
void  spinel_process_rx_byte(UBYTE b);

#endif /* RISCV32_SPINEL_H */
