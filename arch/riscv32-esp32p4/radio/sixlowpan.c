/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Enterprise-Grade RFC 6282 / RFC 4944 6LoWPAN Engine
          (IPHC Compression, Decompression, Fragmentation & Reassembly)
          for IEEE 802.15.4 / Thread / Matter on AROS.
*/

#include "types.h"
#include "../include/sixlowpan.h"

static struct sixlowpan_reassembly_context g_reassembly_slots[SIXLOWPAN_MAX_REASSEMBLY_SLOTS];
static UWORD g_next_datagram_tag = 1;

static inline void copy_mem(const void *src, void *dst, ULONG n) {
    const UBYTE *s = (const UBYTE *)src;
    UBYTE *d = (UBYTE *)dst;
    while (n--) *d++ = *s++;
}

static inline void set_mem(void *dst, UBYTE val, ULONG n) {
    UBYTE *d = (UBYTE *)dst;
    while (n--) *d++ = val;
}

static inline BOOL is_link_local_prefix(const UBYTE *addr) {
    return (addr[0] == 0xFE && addr[1] == 0x80);
}

static inline BOOL is_multicast_prefix(const UBYTE *addr) {
    return (addr[0] == 0xFF);
}

static inline BOOL match_iid_eui64(const UBYTE *addr_iid, const UBYTE *eui64) {
    if ((addr_iid[0] ^ 0x02) != eui64[0]) return FALSE;
    for (int i = 1; i < 8; i++) {
        if (addr_iid[i] != eui64[i]) return FALSE;
    }
    return TRUE;
}

void sixlowpan_engine_init(void) {
    for (int i = 0; i < SIXLOWPAN_MAX_REASSEMBLY_SLOTS; i++) {
        g_reassembly_slots[i].active = FALSE;
        g_reassembly_slots[i].received_bytes = 0;
        set_mem(g_reassembly_slots[i].received_chunks_bitmap, 0, sizeof(g_reassembly_slots[i].received_chunks_bitmap));
    }
    g_next_datagram_tag = 1;
}

/*
 * Compress a standard 40-byte IPv6 packet (+ optional UDP) into 6LoWPAN IPHC format.
 */
WORD sixlowpan_compress_ipv6(const UBYTE *src_ipv6, UWORD src_len,
                             UBYTE *dst_frame, UWORD dst_max_len,
                             const UBYTE *src_eui64, const UBYTE *dst_eui64)
{
    if (!src_ipv6 || src_len < 40 || !dst_frame || dst_max_len < 4)
        return -1;

    UBYTE *out = dst_frame;
    UBYTE iphc0 = SIXLOWPAN_DISPATCH_IPHC;
    UBYTE iphc1 = 0;

    ULONG ver_tc_fl = ((ULONG)src_ipv6[0] << 24) | ((ULONG)src_ipv6[1] << 16) |
                      ((ULONG)src_ipv6[2] << 8)  | ((ULONG)src_ipv6[3]);
    ULONG tc = (ver_tc_fl >> 20) & 0xFF;
    ULONG fl = ver_tc_fl & 0xFFFFF;

    if (tc == 0 && fl == 0) {
        iphc0 |= IPHC_TF_COMPRESSED;
    } else if (fl == 0) {
        iphc0 |= IPHC_TF_TRAFFIC_CLASS;
    } else {
        iphc0 |= IPHC_TF_TRAFFIC_CLASS_ECN;
    }

    UBYTE nexthdr = src_ipv6[6];
    if (nexthdr == 17) {
        iphc0 |= IPHC_NH_COMPRESSED;
    }

    UBYTE hoplimit = src_ipv6[7];
    if (hoplimit == 1) {
        iphc0 |= IPHC_HLIM_1;
    } else if (hoplimit == 64) {
        iphc0 |= IPHC_HLIM_64;
    } else if (hoplimit == 255) {
        iphc0 |= IPHC_HLIM_255;
    } else {
        iphc0 |= IPHC_HLIM_INLINE;
    }

    const UBYTE *src_ip = &src_ipv6[8];
    if (is_link_local_prefix(src_ip) && src_eui64 && match_iid_eui64(&src_ip[8], src_eui64)) {
        iphc1 |= IPHC_SAM_0;
    } else if (is_link_local_prefix(src_ip)) {
        iphc1 |= IPHC_SAM_64;
    } else {
        iphc1 |= IPHC_SAM_128;
    }

    const UBYTE *dst_ip = &src_ipv6[24];
    BOOL is_mcast = is_multicast_prefix(dst_ip);
    if (is_mcast) {
        iphc1 |= IPHC_M_MULTICAST;
        /* Check for ff02::XX link-local multicast */
        BOOL is_ff02_8bit = (dst_ip[0] == 0xFF && dst_ip[1] == 0x02);
        for (int i = 2; i < 15 && is_ff02_8bit; i++) {
            if (dst_ip[i] != 0) is_ff02_8bit = FALSE;
        }

        if (is_ff02_8bit) {
            iphc1 |= IPHC_DAM_0; /* DAM = 11: 8-bit inline (ff02::XX) */
        } else {
            iphc1 |= IPHC_DAM_128; /* DAM = 00: Full 128-bit inline */
        }
    } else if (is_link_local_prefix(dst_ip) && dst_eui64 && match_iid_eui64(&dst_ip[8], dst_eui64)) {
        iphc1 |= IPHC_DAM_0;
    } else if (is_link_local_prefix(dst_ip)) {
        iphc1 |= IPHC_DAM_64;
    } else {
        iphc1 |= IPHC_DAM_128;
    }

    *out++ = iphc0;
    *out++ = iphc1;

    if ((iphc0 & IPHC_TF_COMPRESSED) == IPHC_TF_TRAFFIC_CLASS) {
        *out++ = (UBYTE)tc;
    } else if ((iphc0 & IPHC_TF_COMPRESSED) == IPHC_TF_TRAFFIC_CLASS_ECN) {
        *out++ = (UBYTE)tc;
        *out++ = (UBYTE)(fl >> 16);
        *out++ = (UBYTE)(fl >> 8);
        *out++ = (UBYTE)(fl & 0xFF);
    }

    if (!(iphc0 & IPHC_NH_COMPRESSED)) {
        *out++ = nexthdr;
    }

    if ((iphc0 & 0x03) == IPHC_HLIM_INLINE) {
        *out++ = hoplimit;
    }

    if ((iphc1 & 0x30) == IPHC_SAM_128) {
        copy_mem(src_ip, out, 16);
        out += 16;
    } else if ((iphc1 & 0x30) == IPHC_SAM_64) {
        copy_mem(&src_ip[8], out, 8);
        out += 8;
    }

    if (is_mcast) {
        if ((iphc1 & 0x03) == IPHC_DAM_0) {
            *out++ = dst_ip[15]; /* 8-bit inline for ff02::XX */
        } else {
            copy_mem(dst_ip, out, 16);
            out += 16;
        }
    } else {
        if ((iphc1 & 0x03) == IPHC_DAM_128) {
            copy_mem(dst_ip, out, 16);
            out += 16;
        } else if ((iphc1 & 0x03) == IPHC_DAM_64) {
            copy_mem(&dst_ip[8], out, 8);
            out += 8;
        }
    }

    UWORD payload_offset = 40;
    if (nexthdr == 17 && src_len >= 48) {
        const UBYTE *udp = &src_ipv6[40];
        UWORD src_port = ((UWORD)udp[0] << 8) | udp[1];
        UWORD dst_port = ((UWORD)udp[2] << 8) | udp[3];

        *out++ = SIXLOWPAN_NHC_UDP_ID | SIXLOWPAN_NHC_UDP_PORTS_INLINE;
        *out++ = (UBYTE)(src_port >> 8);
        *out++ = (UBYTE)(src_port & 0xFF);
        *out++ = (UBYTE)(dst_port >> 8);
        *out++ = (UBYTE)(dst_port & 0xFF);
        *out++ = udp[6];
        *out++ = udp[7];
        payload_offset = 48;
    }

    UWORD rem = src_len - payload_offset;
    if (out - dst_frame + rem > dst_max_len)
        return -1;

    copy_mem(&src_ipv6[payload_offset], out, rem);
    out += rem;

    return (WORD)(out - dst_frame);
}

/*
 * Decompress a 6LoWPAN IPHC frame back into a complete standard 40-byte IPv6 packet.
 */
WORD sixlowpan_decompress_ipv6(const UBYTE *src_frame, UWORD src_len,
                               UBYTE *dst_ipv6, UWORD dst_max_len,
                               const UBYTE *src_eui64, const UBYTE *dst_eui64)
{
    if (!src_frame || src_len < 2 || !dst_ipv6 || dst_max_len < 40)
        return -1;

    const UBYTE *in = src_frame;
    UBYTE iphc0 = *in++;
    UBYTE iphc1 = *in++;

    if ((iphc0 & 0xE0) != SIXLOWPAN_DISPATCH_IPHC)
        return -1;

    set_mem(dst_ipv6, 0, 40);
    dst_ipv6[0] = 0x60; /* IPv6 */

    UBYTE tf = (iphc0 & IPHC_TF_COMPRESSED);
    if (tf == IPHC_TF_TRAFFIC_CLASS) {
        dst_ipv6[1] = *in++;
    } else if (tf == IPHC_TF_TRAFFIC_CLASS_ECN) {
        dst_ipv6[1] = *in++;
        dst_ipv6[1] |= (*in++ & 0x0F);
        dst_ipv6[2] = *in++;
        dst_ipv6[3] = *in++;
    }

    UBYTE nexthdr = 0;
    BOOL is_udp_nhc = (iphc0 & IPHC_NH_COMPRESSED) ? TRUE : FALSE;
    if (!is_udp_nhc) {
        nexthdr = *in++;
        dst_ipv6[6] = nexthdr;
    } else {
        dst_ipv6[6] = 17;
    }

    UBYTE hlim = (iphc0 & 0x03);
    if (hlim == IPHC_HLIM_1) {
        dst_ipv6[7] = 1;
    } else if (hlim == IPHC_HLIM_64) {
        dst_ipv6[7] = 64;
    } else if (hlim == IPHC_HLIM_255) {
        dst_ipv6[7] = 255;
    } else {
        dst_ipv6[7] = *in++;
    }

    UBYTE *dst_src_ip = &dst_ipv6[8];
    UBYTE sam = (iphc1 & 0x30);
    if (sam == IPHC_SAM_0) {
        dst_src_ip[0] = 0xFE; dst_src_ip[1] = 0x80;
        if (src_eui64) {
            dst_src_ip[8] = src_eui64[0] ^ 0x02;
            copy_mem(&src_eui64[1], &dst_src_ip[9], 7);
        }
    } else if (sam == IPHC_SAM_64) {
        dst_src_ip[0] = 0xFE; dst_src_ip[1] = 0x80;
        copy_mem(in, &dst_src_ip[8], 8);
        in += 8;
    } else {
        copy_mem(in, dst_src_ip, 16);
        in += 16;
    }

    UBYTE *dst_dst_ip = &dst_ipv6[24];
    UBYTE dam = (iphc1 & 0x03);
    if (iphc1 & IPHC_M_MULTICAST) {
        if (dam == IPHC_DAM_0) { /* DAM = 11: 8-bit inline (ff02::XX) */
            dst_dst_ip[0] = 0xFF;
            dst_dst_ip[1] = 0x02;
            dst_dst_ip[15] = *in++;
        } else { /* DAM = 00: Full 128-bit inline */
            copy_mem(in, dst_dst_ip, 16);
            in += 16;
        }
    } else {
        if (dam == IPHC_DAM_0) {
            dst_dst_ip[0] = 0xFE; dst_dst_ip[1] = 0x80;
            if (dst_eui64) {
                dst_dst_ip[8] = dst_eui64[0] ^ 0x02;
                copy_mem(&dst_eui64[1], &dst_dst_ip[9], 7);
            }
        } else if (dam == IPHC_DAM_64) {
            dst_dst_ip[0] = 0xFE; dst_dst_ip[1] = 0x80;
            copy_mem(in, &dst_dst_ip[8], 8);
            in += 8;
        } else {
            copy_mem(in, dst_dst_ip, 16);
            in += 16;
        }
    }

    UBYTE *out = &dst_ipv6[40];
    if (is_udp_nhc) {
        UBYTE nhc = *in++;
        if ((nhc & 0xF8) == SIXLOWPAN_NHC_UDP_ID) {
            UBYTE src_p_hi = *in++;
            UBYTE src_p_lo = *in++;
            UBYTE dst_p_hi = *in++;
            UBYTE dst_p_lo = *in++;
            UBYTE csum_hi  = *in++;
            UBYTE csum_lo  = *in++;

            out[0] = src_p_hi; out[1] = src_p_lo;
            out[2] = dst_p_hi; out[3] = dst_p_lo;

            UWORD udp_payload_len = (src_len - (UWORD)(in - src_frame));
            UWORD total_udp_len = 8 + udp_payload_len;

            out[4] = (UBYTE)(total_udp_len >> 8);
            out[5] = (UBYTE)(total_udp_len & 0xFF);
            out[6] = csum_hi;
            out[7] = csum_lo;
            out += 8;
        }
    }

    UWORD rem = src_len - (UWORD)(in - src_frame);
    if ((out - dst_ipv6) + rem > dst_max_len)
        return -1;

    copy_mem(in, out, rem);
    out += rem;

    UWORD ipv6_payload_len = (UWORD)(out - dst_ipv6) - 40;
    dst_ipv6[4] = (UBYTE)(ipv6_payload_len >> 8);
    dst_ipv6[5] = (UBYTE)(ipv6_payload_len & 0xFF);

    return (WORD)(out - dst_ipv6);
}

/*
 * Fragment and Transmit an uncompressed or compressed IPv6 packet (RFC 4944 Section 5.3).
 */
int sixlowpan_fragment_and_send(const UBYTE *data, UWORD data_len,
                                UWORD uncompressed_size,
                                sixlowpan_tx_func_t tx_func, void *user_data)
{
    if (!data || data_len == 0 || !tx_func)
        return -1;

    /* If frame fits in one IEEE 802.15.4 frame (~100 bytes payload), send directly */
    if (data_len <= 100) {
        return tx_func(data, data_len, user_data);
    }

    UWORD tag = g_next_datagram_tag++;
    if (g_next_datagram_tag == 0) g_next_datagram_tag = 1;

    UBYTE frag_buf[SIXLOWPAN_MAX_FRAME_SIZE];
    UWORD dgram_size = (uncompressed_size > 0) ? uncompressed_size : data_len;

    /* 1. Send FRAG1: Header is 4 bytes [11000 size:11] [tag:16] */
    frag_buf[0] = SIXLOWPAN_DISPATCH_FRAG1 | ((dgram_size >> 8) & 0x07);
    frag_buf[1] = (UBYTE)(dgram_size & 0xFF);
    frag_buf[2] = (UBYTE)(tag >> 8);
    frag_buf[3] = (UBYTE)(tag & 0xFF);

    /* FRAG1 payload must be a multiple of 8 bytes (e.g. 96 bytes) */
    UWORD frag1_payload_len = 96;
    if (frag1_payload_len > data_len) frag1_payload_len = data_len;

    copy_mem(data, &frag_buf[4], frag1_payload_len);
    int res = tx_func(frag_buf, 4 + frag1_payload_len, user_data);
    if (res < 0) return res;

    /* 2. Send FRAGN: Header is 5 bytes [11100 size:11] [tag:16] [offset:8] */
    UWORD offset = frag1_payload_len;
    while (offset < data_len) {
        UWORD chunk_len = data_len - offset;
        /* Fit chunk into max 96 bytes (aligned to 8 bytes if not last) */
        if (chunk_len > 96) chunk_len = 96;

        frag_buf[0] = SIXLOWPAN_DISPATCH_FRAGN | ((dgram_size >> 8) & 0x07);
        frag_buf[1] = (UBYTE)(dgram_size & 0xFF);
        frag_buf[2] = (UBYTE)(tag >> 8);
        frag_buf[3] = (UBYTE)(tag & 0xFF);
        frag_buf[4] = (UBYTE)(offset / 8); /* Datagram offset in 8-byte units */

        copy_mem(&data[offset], &frag_buf[5], chunk_len);
        res = tx_func(frag_buf, 5 + chunk_len, user_data);
        if (res < 0) return res;

        offset += chunk_len;
    }

    return (int)data_len;
}

/*
 * Process an incoming RX 6LoWPAN fragment, perform reassembly, and return complete IPv6 frame.
 */
WORD sixlowpan_process_rx_fragment(const UBYTE *frag_data, UWORD frag_len,
                                   const UBYTE *src_eui64, ULONG current_time_ms,
                                   UBYTE *out_complete_ipv6, UWORD max_out_len)
{
    if (!frag_data || frag_len < 2 || !out_complete_ipv6)
        return -1;

    UBYTE dispatch = frag_data[0] & 0xF8;

    /* Non-fragmented IPHC packet */
    if ((frag_data[0] & 0xE0) == SIXLOWPAN_DISPATCH_IPHC) {
        return sixlowpan_decompress_ipv6(frag_data, frag_len, out_complete_ipv6, max_out_len, src_eui64, NULL);
    }

    /* Check for FRAG1 or FRAGN */
    if (dispatch != SIXLOWPAN_DISPATCH_FRAG1 && dispatch != SIXLOWPAN_DISPATCH_FRAGN) {
        return -1;
    }

    UWORD dgram_size = (((UWORD)(frag_data[0] & 0x07)) << 8) | frag_data[1];
    UWORD dgram_tag  = (((UWORD)frag_data[2]) << 8) | frag_data[3];
    UWORD offset = 0;
    UWORD hdr_len = 4;

    if (dispatch == SIXLOWPAN_DISPATCH_FRAGN) {
        if (frag_len < 5) return -1;
        offset = ((UWORD)frag_data[4]) * 8;
        hdr_len = 5;
    }

    UWORD payload_len = frag_len - hdr_len;
    if (offset + payload_len > SIXLOWPAN_MAX_IPV6_SIZE)
        return -1;

    /* Find matching reassembly context or allocate new slot */
    struct sixlowpan_reassembly_context *ctx = NULL;
    int free_slot = -1;

    for (int i = 0; i < SIXLOWPAN_MAX_REASSEMBLY_SLOTS; i++) {
        if (g_reassembly_slots[i].active) {
            /* Check timeout */
            if (current_time_ms - g_reassembly_slots[i].timestamp_ms > SIXLOWPAN_REASSEMBLY_TIMEOUT_MS) {
                g_reassembly_slots[i].active = FALSE;
                free_slot = i;
                continue;
            }

            if (g_reassembly_slots[i].datagram_tag == dgram_tag &&
                g_reassembly_slots[i].datagram_size == dgram_size) {
                ctx = &g_reassembly_slots[i];
                break;
            }
        } else if (free_slot == -1) {
            free_slot = i;
        }
    }

    if (!ctx) {
        if (free_slot == -1) free_slot = 0; /* Evict oldest slot */
        ctx = &g_reassembly_slots[free_slot];
        ctx->active = TRUE;
        ctx->datagram_tag = dgram_tag;
        ctx->datagram_size = dgram_size;
        ctx->received_bytes = 0;
        ctx->timestamp_ms = current_time_ms;
        if (src_eui64) copy_mem(src_eui64, ctx->src_eui64, 8);
        set_mem(ctx->received_chunks_bitmap, 0, sizeof(ctx->received_chunks_bitmap));
    }

    /* Copy fragment payload into reassembly buffer at exact offset */
    copy_mem(&frag_data[hdr_len], &ctx->buffer[offset], payload_len);
    ctx->received_bytes += payload_len;

    /* Mark chunk bitmask */
    UWORD chunk_start = offset / 8;
    UWORD chunk_count = (payload_len + 7) / 8;
    for (UWORD c = 0; c < chunk_count; c++) {
        UWORD idx = (chunk_start + c) / 32;
        UWORD bit = (chunk_start + c) % 32;
        if (idx < 6) ctx->received_chunks_bitmap[idx] |= (1UL << bit);
    }

    /* Check if full datagram has been reassembled */
    if (ctx->received_bytes >= dgram_size || (offset + payload_len >= dgram_size)) {
        ctx->active = FALSE;

        /* If reassembled buffer is compressed IPHC, decompress it */
        if ((ctx->buffer[0] & 0xE0) == SIXLOWPAN_DISPATCH_IPHC) {
            return sixlowpan_decompress_ipv6(ctx->buffer, dgram_size, out_complete_ipv6, max_out_len, src_eui64, NULL);
        }

        /* Uncompressed raw IPv6 */
        UWORD copy_len = (dgram_size < max_out_len) ? dgram_size : max_out_len;
        copy_mem(ctx->buffer, out_complete_ipv6, copy_len);
        return (WORD)copy_len;
    }

    return 0; /* Incomplete, waiting for remaining fragments */
}
