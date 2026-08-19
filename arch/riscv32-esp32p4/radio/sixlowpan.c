/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Clean-room RFC 6282 / RFC 4944 6LoWPAN Compression & Decompression
          Engine for IEEE 802.15.4 / Thread / Matter on AROS.
*/

#include "types.h"
#include "../include/sixlowpan.h"

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
    /* IPv6 IID has U/L bit (bit 1 of byte 0) inverted compared to EUI-64 */
    if ((addr_iid[0] ^ 0x02) != eui64[0]) return FALSE;
    for (int i = 1; i < 8; i++) {
        if (addr_iid[i] != eui64[i]) return FALSE;
    }
    return TRUE;
}

/*
 * Compress a standard 40-byte IPv6 packet (+ optional UDP) into 6LoWPAN IPHC format.
 * Returns compressed frame length, or -1 on error.
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

    /* Check Traffic Class / Flow Label (IPv6 bytes 0..3) */
    ULONG ver_tc_fl = ((ULONG)src_ipv6[0] << 24) | ((ULONG)src_ipv6[1] << 16) |
                      ((ULONG)src_ipv6[2] << 8)  | ((ULONG)src_ipv6[3]);
    ULONG tc = (ver_tc_fl >> 20) & 0xFF;
    ULONG fl = ver_tc_fl & 0xFFFFF;

    if (tc == 0 && fl == 0) {
        iphc0 |= IPHC_TF_COMPRESSED; /* Fully elided */
    } else if (fl == 0) {
        iphc0 |= IPHC_TF_TRAFFIC_CLASS;
    } else {
        iphc0 |= IPHC_TF_TRAFFIC_CLASS_ECN;
    }

    /* Next Header (IPv6 byte 6) */
    UBYTE nexthdr = src_ipv6[6];
    if (nexthdr == 17) { /* UDP (Next Header Compression) */
        iphc0 |= IPHC_NH_COMPRESSED;
    }

    /* Hop Limit (IPv6 byte 7) */
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

    /* Source Address Compression (IPv6 bytes 8..23) */
    const UBYTE *src_ip = &src_ipv6[8];
    if (is_link_local_prefix(src_ip) && src_eui64 && match_iid_eui64(&src_ip[8], src_eui64)) {
        iphc1 |= IPHC_SAM_0; /* 0 bits inline: derived from EUI-64 */
    } else if (is_link_local_prefix(src_ip)) {
        iphc1 |= IPHC_SAM_64; /* 64-bit IID inline */
    } else {
        iphc1 |= IPHC_SAM_128; /* Full 128-bit address inline */
    }

    /* Destination Address Compression (IPv6 bytes 24..39) */
    const UBYTE *dst_ip = &src_ipv6[24];
    if (is_multicast_prefix(dst_ip)) {
        iphc1 |= IPHC_M_MULTICAST;
        iphc1 |= IPHC_DAM_128;
    } else if (is_link_local_prefix(dst_ip) && dst_eui64 && match_iid_eui64(&dst_ip[8], dst_eui64)) {
        iphc1 |= IPHC_DAM_0; /* 0 bits inline: derived from EUI-64 */
    } else if (is_link_local_prefix(dst_ip)) {
        iphc1 |= IPHC_DAM_64; /* 64-bit IID inline */
    } else {
        iphc1 |= IPHC_DAM_128;
    }

    /* Write 2-byte IPHC Header */
    *out++ = iphc0;
    *out++ = iphc1;

    /* Inline Traffic Class / Flow Label */
    if ((iphc0 & IPHC_TF_COMPRESSED) == IPHC_TF_TRAFFIC_CLASS) {
        *out++ = (UBYTE)tc;
    } else if ((iphc0 & IPHC_TF_COMPRESSED) == IPHC_TF_TRAFFIC_CLASS_ECN) {
        *out++ = (UBYTE)tc;
        *out++ = (UBYTE)(fl >> 16);
        *out++ = (UBYTE)(fl >> 8);
        *out++ = (UBYTE)(fl & 0xFF);
    }

    /* Inline Next Header */
    if (!(iphc0 & IPHC_NH_COMPRESSED)) {
        *out++ = nexthdr;
    }

    /* Inline Hop Limit */
    if ((iphc0 & 0x03) == IPHC_HLIM_INLINE) {
        *out++ = hoplimit;
    }

    /* Inline Source Address */
    if ((iphc1 & 0x30) == IPHC_SAM_128) {
        copy_mem(src_ip, out, 16);
        out += 16;
    } else if ((iphc1 & 0x30) == IPHC_SAM_64) {
        copy_mem(&src_ip[8], out, 8);
        out += 8;
    }

    /* Inline Destination Address */
    if ((iphc1 & 0x03) == IPHC_DAM_128) {
        copy_mem(dst_ip, out, 16);
        out += 16;
    } else if ((iphc1 & 0x03) == IPHC_DAM_64) {
        copy_mem(&dst_ip[8], out, 8);
        out += 8;
    }

    /* UDP Next Header Compression */
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
        *out++ = udp[6]; /* Checksum High */
        *out++ = udp[7]; /* Checksum Low */
        payload_offset = 48;
    }

    /* Copy remainder payload */
    UWORD rem = src_len - payload_offset;
    if (out - dst_frame + rem > dst_max_len)
        return -1;

    copy_mem(&src_ipv6[payload_offset], out, rem);
    out += rem;

    return (WORD)(out - dst_frame);
}

/*
 * Decompress a 6LoWPAN IPHC frame back into a complete standard 40-byte IPv6 packet.
 * Returns reconstructed IPv6 packet length, or -1 on error.
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

    /* Version: IPv6 (6) */
    dst_ipv6[0] = 0x60;

    /* Traffic Class / Flow Label */
    UBYTE tf = (iphc0 & IPHC_TF_COMPRESSED);
    if (tf == IPHC_TF_TRAFFIC_CLASS) {
        dst_ipv6[1] = *in++;
    } else if (tf == IPHC_TF_TRAFFIC_CLASS_ECN) {
        dst_ipv6[1] = *in++;
        dst_ipv6[1] |= (*in++ & 0x0F);
        dst_ipv6[2] = *in++;
        dst_ipv6[3] = *in++;
    }

    /* Next Header */
    UBYTE nexthdr = 0;
    BOOL is_udp_nhc = (iphc0 & IPHC_NH_COMPRESSED) ? TRUE : FALSE;
    if (!is_udp_nhc) {
        nexthdr = *in++;
        dst_ipv6[6] = nexthdr;
    } else {
        dst_ipv6[6] = 17; /* UDP */
    }

    /* Hop Limit */
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

    /* Reconstruct Source Address */
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

    /* Reconstruct Destination Address */
    UBYTE *dst_dst_ip = &dst_ipv6[24];
    UBYTE dam = (iphc1 & 0x03);
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

    /* Decompress UDP NHC */
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

            /* Calculate UDP length later based on remaining payload */
            UWORD udp_payload_len = (src_len - (UWORD)(in - src_frame));
            UWORD total_udp_len = 8 + udp_payload_len;

            out[4] = (UBYTE)(total_udp_len >> 8);
            out[5] = (UBYTE)(total_udp_len & 0xFF);
            out[6] = csum_hi;
            out[7] = csum_lo;
            out += 8;
        }
    }

    /* Copy remaining payload */
    UWORD rem = src_len - (UWORD)(in - src_frame);
    if ((out - dst_ipv6) + rem > dst_max_len)
        return -1;

    copy_mem(in, out, rem);
    out += rem;

    /* Fill in IPv6 Payload Length (bytes 4..5) */
    UWORD ipv6_payload_len = (UWORD)(out - dst_ipv6) - 40;
    dst_ipv6[4] = (UBYTE)(ipv6_payload_len >> 8);
    dst_ipv6[5] = (UBYTE)(ipv6_payload_len & 0xFF);

    return (WORD)(out - dst_ipv6);
}
