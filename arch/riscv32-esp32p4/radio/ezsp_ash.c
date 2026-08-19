/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Clean-room ASH (Asynchronous Serial Host) & EZSP / ZCL Frame
          Engine for Zigbee 3.0 on ESP32-C6 / Seeed Studio D1001.
*/

#include "types.h"
#include "../include/ezsp_ash.h"

/*
 * CRC-16 CCITT (Polynomial 0x1021, Initial 0xFFFF) for ASH Packet Integrity
 */
static UWORD crc16_ccitt(const UBYTE *data, UWORD len) {
    UWORD crc = 0xFFFF;
    while (len--) {
        UBYTE b = *data++;
        crc = (crc >> 8) | (crc << 8);
        crc ^= b;
        crc ^= (crc & 0xFF) >> 4;
        crc ^= (crc << 12);
        crc ^= ((crc & 0xFF) << 5);
    }
    return crc;
}

/*
 * Encode payload into an ASH Frame with byte stuffing and CRC-16.
 */
UWORD ash_encode_frame(const UBYTE *payload, UWORD len, UBYTE *out_buf, UWORD out_max, UBYTE control)
{
    if (!out_buf || out_max < 6 + len * 2)
        return 0;

    UBYTE raw[256];
    raw[0] = control;
    for (UWORD i = 0; i < len && i < 250; i++) {
        raw[1 + i] = payload[i];
    }
    UWORD raw_len = 1 + len;

    /* Compute CRC-16 over Control Byte + Payload */
    UWORD crc = crc16_ccitt(raw, raw_len);
    raw[raw_len++] = (UBYTE)(crc >> 8);
    raw[raw_len++] = (UBYTE)(crc & 0xFF);

    /* Byte Stuffing into output buffer */
    UBYTE *out = out_buf;
    *out++ = ASH_FRAME_FLAG;

    for (UWORD i = 0; i < raw_len; i++) {
        UBYTE b = raw[i];
        if (b == ASH_FRAME_FLAG || b == ASH_FRAME_ESC || b == ASH_FRAME_XON || b == ASH_FRAME_XOFF || b == ASH_FRAME_SUBSTITUTE) {
            *out++ = ASH_FRAME_ESC;
            *out++ = b ^ 0x20;
        } else {
            *out++ = b;
        }
    }

    *out++ = ASH_FRAME_FLAG;
    return (UWORD)(out - out_buf);
}

/*
 * Decode an incoming ASH Frame, verify CRC-16, and extract payload.
 */
WORD ash_decode_frame(const UBYTE *in_buf, UWORD in_len, UBYTE *payload, UWORD max_payload, UBYTE *out_ctrl)
{
    if (!in_buf || in_len < 4 || !payload)
        return -1;

    UBYTE unescaped[256];
    UWORD ulen = 0;
    BOOL esc = FALSE;

    for (UWORD i = 0; i < in_len; i++) {
        UBYTE b = in_buf[i];
        if (b == ASH_FRAME_FLAG) {
            if (ulen > 0) break; /* End of frame */
            continue; /* Start of frame */
        }
        if (b == ASH_FRAME_ESC) {
            esc = TRUE;
            continue;
        }
        if (esc) {
            b ^= 0x20;
            esc = FALSE;
        }
        if (ulen < sizeof(unescaped)) {
            unescaped[ulen++] = b;
        }
    }

    if (ulen < 3) return -1; /* Control + 2-byte CRC minimum */

    /* Verify CRC */
    UWORD calculated_crc = crc16_ccitt(unescaped, ulen - 2);
    UWORD frame_crc = ((UWORD)unescaped[ulen - 2] << 8) | unescaped[ulen - 1];

    if (calculated_crc != frame_crc)
        return -1; /* CRC Mismatch */

    if (out_ctrl)
        *out_ctrl = unescaped[0];

    UWORD payload_len = ulen - 3;
    if (payload_len > max_payload)
        return -1;

    for (UWORD i = 0; i < payload_len; i++) {
        payload[i] = unescaped[1 + i];
    }

    return (WORD)payload_len;
}

/*
 * Build a standard ZCL (Zigbee Cluster Library) On/Off command frame.
 */
UWORD zcl_build_on_off_cmd(UWORD dest_addr, UBYTE endpoint, UBYTE seq, UBYTE cmd, UBYTE *out_frame)
{
    if (!out_frame) return 0;

    UBYTE *p = out_frame;
    /* EZSP Header: Sequence, FrameControl, CommandId (SEND_UNICAST = 0x0034) */
    *p++ = seq;
    *p++ = 0x00; /* Frame Control */
    *p++ = (UBYTE)(EZSP_CMD_SEND_UNICAST & 0xFF);
    *p++ = (UBYTE)(EZSP_CMD_SEND_UNICAST >> 8);

    /* OutgoingMessageType: 0x00 = EMBER_OUTGOING_DIRECT */
    *p++ = 0x00;

    /* Destination Short Address (16-bit) */
    *p++ = (UBYTE)(dest_addr & 0xFF);
    *p++ = (UBYTE)(dest_addr >> 8);

    /* APS Frame: ProfileID=0x0104 (Home Automation), ClusterID=0x0006 (On/Off) */
    *p++ = 0x04; *p++ = 0x01; /* Profile HA (0x0104) */
    *p++ = (UBYTE)(ZCL_CLUSTER_ON_OFF & 0xFF);
    *p++ = (UBYTE)(ZCL_CLUSTER_ON_OFF >> 8);
    *p++ = endpoint;          /* Source Endpoint */
    *p++ = endpoint;          /* Destination Endpoint */
    *p++ = 0x00; *p++ = 0x00; /* APS Options (None) */
    *p++ = 0x00;              /* GroupID */
    *p++ = seq;               /* Sequence number */

    /* ZCL Message Payload: FrameControl (Cluster-Specific = 0x01), Seq, Command */
    *p++ = 3;                 /* ZCL payload length = 3 bytes */
    *p++ = 0x01;              /* Cluster-specific, Client-to-Server */
    *p++ = seq;               /* ZCL Transaction Seq */
    *p++ = cmd;               /* 0x00=Off, 0x01=On, 0x02=Toggle */

    return (UWORD)(p - out_frame);
}

/*
 * Build a standard ZCL Level Control (Dimmer / Slider) command frame.
 */
UWORD zcl_build_level_cmd(UWORD dest_addr, UBYTE endpoint, UBYTE seq, UBYTE level, UWORD transition_100ms, UBYTE *out_frame)
{
    if (!out_frame) return 0;

    UBYTE *p = out_frame;
    *p++ = seq;
    *p++ = 0x00;
    *p++ = (UBYTE)(EZSP_CMD_SEND_UNICAST & 0xFF);
    *p++ = (UBYTE)(EZSP_CMD_SEND_UNICAST >> 8);
    *p++ = 0x00; /* Direct */

    *p++ = (UBYTE)(dest_addr & 0xFF);
    *p++ = (UBYTE)(dest_addr >> 8);

    *p++ = 0x04; *p++ = 0x01; /* Profile HA */
    *p++ = (UBYTE)(ZCL_CLUSTER_LEVEL_CONTROL & 0xFF);
    *p++ = (UBYTE)(ZCL_CLUSTER_LEVEL_CONTROL >> 8);
    *p++ = endpoint;
    *p++ = endpoint;
    *p++ = 0x00; *p++ = 0x00;
    *p++ = 0x00;
    *p++ = seq;

    /* ZCL Payload: 6 bytes */
    *p++ = 6;
    *p++ = 0x01;
    *p++ = seq;
    *p++ = ZCL_CMD_MOVE_TO_LEVEL;
    *p++ = level; /* 0..254 */
    *p++ = (UBYTE)(transition_100ms & 0xFF);
    *p++ = (UBYTE)(transition_100ms >> 8);

    return (UWORD)(p - out_frame);
}
