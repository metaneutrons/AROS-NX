/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Enterprise-Grade OpenThread Spinel v1.0 HDLC Protocol Engine
          for ESP32-C6 / ESP32-H2 on AROS.
*/

#include "types.h"
#include "../include/spinel.h"

static spinel_rx_callback_t g_spinel_rx_cb = NULL;
static void *g_spinel_user_data = NULL;

static UBYTE g_rx_buf[512];
static UWORD g_rx_len = 0;
static BOOL  g_rx_escaped = FALSE;

/* CRC-16 CCITT for Spinel Frame Integrity */
static UWORD spinel_crc16(const UBYTE *data, UWORD len) {
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

void spinel_engine_init(spinel_rx_callback_t rx_cb, void *user_data) {
    g_spinel_rx_cb = rx_cb;
    g_spinel_user_data = user_data;
    g_rx_len = 0;
    g_rx_escaped = FALSE;
}

static UWORD hdlc_stuff_and_wrap(const UBYTE *raw, UWORD raw_len, UBYTE *out, UWORD out_max) {
    if (!raw || !out || out_max < raw_len * 2 + 5)
        return 0;

    UWORD crc = spinel_crc16(raw, raw_len);
    UBYTE *p = out;
    *p++ = SPINEL_FRAME_DELIMITER;

    for (UWORD i = 0; i < raw_len; i++) {
        UBYTE b = raw[i];
        if (b == SPINEL_FRAME_DELIMITER || b == SPINEL_FRAME_ESCAPE || b == SPINEL_FRAME_XON || b == SPINEL_FRAME_XOFF) {
            *p++ = SPINEL_FRAME_ESCAPE;
            *p++ = b ^ 0x20;
        } else {
            *p++ = b;
        }
    }

    /* Append CRC-16 */
    UBYTE crc_bytes[2] = { (UBYTE)(crc & 0xFF), (UBYTE)(crc >> 8) };
    for (int i = 0; i < 2; i++) {
        UBYTE b = crc_bytes[i];
        if (b == SPINEL_FRAME_DELIMITER || b == SPINEL_FRAME_ESCAPE || b == SPINEL_FRAME_XON || b == SPINEL_FRAME_XOFF) {
            *p++ = SPINEL_FRAME_ESCAPE;
            *p++ = b ^ 0x20;
        } else {
            *p++ = b;
        }
    }

    *p++ = SPINEL_FRAME_DELIMITER;
    return (UWORD)(p - out);
}

UWORD spinel_encode_set_prop_uint8(UBYTE tid, UWORD prop, UBYTE val, UBYTE *out_buf, UWORD out_max) {
    UBYTE raw[6];
    raw[0] = SPINEL_HEADER_FLAG | (tid & SPINEL_HEADER_TID_MASK);
    raw[1] = SPINEL_CMD_PROP_VALUE_SET;
    raw[2] = (UBYTE)(prop & 0xFF);
    raw[3] = (UBYTE)(prop >> 8);
    raw[4] = val;
    return hdlc_stuff_and_wrap(raw, 5, out_buf, out_max);
}

UWORD spinel_encode_set_prop_uint16(UBYTE tid, UWORD prop, UWORD val, UBYTE *out_buf, UWORD out_max) {
    UBYTE raw[7];
    raw[0] = SPINEL_HEADER_FLAG | (tid & SPINEL_HEADER_TID_MASK);
    raw[1] = SPINEL_CMD_PROP_VALUE_SET;
    raw[2] = (UBYTE)(prop & 0xFF);
    raw[3] = (UBYTE)(prop >> 8);
    raw[4] = (UBYTE)(val & 0xFF);
    raw[5] = (UBYTE)(val >> 8);
    return hdlc_stuff_and_wrap(raw, 6, out_buf, out_max);
}

UWORD spinel_encode_raw_stream(UBYTE tid, const UBYTE *frame, UWORD len, UBYTE *out_buf, UWORD out_max) {
    if (!frame || len == 0 || len > 128) return 0;

    UBYTE raw[140];
    raw[0] = SPINEL_HEADER_FLAG | (tid & SPINEL_HEADER_TID_MASK);
    raw[1] = SPINEL_CMD_PROP_VALUE_SET;
    raw[2] = (UBYTE)(SPINEL_PROP_STREAM_RAW & 0xFF);
    raw[3] = (UBYTE)(SPINEL_PROP_STREAM_RAW >> 8);
    raw[4] = (UBYTE)(len & 0xFF);
    raw[5] = (UBYTE)(len >> 8);

    for (UWORD i = 0; i < len; i++) {
        raw[6 + i] = frame[i];
    }

    return hdlc_stuff_and_wrap(raw, 6 + len, out_buf, out_max);
}

/* Parse a completed unescaped Spinel Frame */
static void parse_spinel_frame(const UBYTE *buf, UWORD len) {
    if (len < 5) return; /* Header(1) + Cmd(1) + Prop(2) + CRC(2) */

    /* Verify CRC-16 */
    UWORD frame_crc = ((UWORD)buf[len - 1] << 8) | buf[len - 2];
    UWORD calc_crc = spinel_crc16(buf, len - 2);
    if (frame_crc != calc_crc) return; /* Discard corrupted frame */

    UBYTE cmd = buf[1];
    UWORD prop = ((UWORD)buf[3] << 8) | buf[2];

    if (cmd == SPINEL_CMD_PROP_VALUE_IS || cmd == SPINEL_CMD_PROP_VALUE_INSERT) {
        if (prop == SPINEL_PROP_STREAM_RAW) {
            /* Raw radio frame arrived from air */
            if (len >= 8 && g_spinel_rx_cb) {
                UWORD psdu_len = ((UWORD)buf[5] << 8) | buf[4];
                if (psdu_len <= 128 && len >= 6 + psdu_len + 2) {
                    struct spinel_rx_frame_meta meta;
                    meta.length = psdu_len;
                    for (UWORD i = 0; i < psdu_len; i++) {
                        meta.psdu[i] = buf[6 + i];
                    }
                    /* Trailing metadata (RSSI / LQI) if present */
                    meta.rssi_dbm = -60;
                    meta.lqi = 255;
                    meta.timestamp_us = 0;

                    g_spinel_rx_cb(&meta, g_spinel_user_data);
                }
            }
        }
    }
}

/* Byte-by-byte HDLC parser state machine fed by UART RX interrupt */
void spinel_process_rx_byte(UBYTE b) {
    if (b == SPINEL_FRAME_DELIMITER) {
        if (g_rx_len > 0) {
            parse_spinel_frame(g_rx_buf, g_rx_len);
            g_rx_len = 0;
        }
        g_rx_escaped = FALSE;
        return;
    }

    if (b == SPINEL_FRAME_ESCAPE) {
        g_rx_escaped = TRUE;
        return;
    }

    if (g_rx_escaped) {
        b ^= 0x20;
        g_rx_escaped = FALSE;
    }

    if (g_rx_len < sizeof(g_rx_buf)) {
        g_rx_buf[g_rx_len++] = b;
    }
}
