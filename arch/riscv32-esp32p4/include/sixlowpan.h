/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Enterprise-Grade RFC 6282 / RFC 4944 6LoWPAN Engine
          (IPHC Compression, Decompression, Fragmentation & Reassembly)
          for IEEE 802.15.4 / Thread / Matter on AROS.
*/

#ifndef RISCV32_SIXLOWPAN_H
#define RISCV32_SIXLOWPAN_H

#include "types.h"

/*
 * 6LoWPAN Dispatch and Framing Constants (RFC 4944 / RFC 6282)
 */
#define SIXLOWPAN_DISPATCH_IPV6         0x41    /* Uncompressed IPv6: 01 000001 */
#define SIXLOWPAN_DISPATCH_IPHC         0x60    /* IPHC Compressed:   011 xxxxx */
#define SIXLOWPAN_DISPATCH_FRAG1        0xC0    /* First Fragment:    11000 xxx */
#define SIXLOWPAN_DISPATCH_FRAGN        0xE0    /* Subsequent Frag:   11100 xxx */

#define SIXLOWPAN_MAX_FRAME_SIZE        127     /* IEEE 802.15.4 Maximum PSDU */
#define SIXLOWPAN_MAX_IPV6_SIZE         1280    /* IPv6 Standard Minimum MTU */
#define SIXLOWPAN_MAX_REASSEMBLY_SLOTS  4       /* Concurrent reassembly contexts */
#define SIXLOWPAN_REASSEMBLY_TIMEOUT_MS 2000    /* 2.0s reassembly expiration */

/*
 * IPHC Header Bitfields (RFC 6282 Section 3.1)
 */
#define IPHC_TF_TRAFFIC_CLASS_ECN       0x00    /* ECN + DSCP (4 bytes) */
#define IPHC_TF_ECN_FLOW_LABEL          0x08    /* ECN + Flow Label (3 bytes) */
#define IPHC_TF_TRAFFIC_CLASS           0x10    /* ECN + DSCP (1 byte) */
#define IPHC_TF_COMPRESSED              0x18    /* Fully elided (0 bytes) */

#define IPHC_NH_INLINE                  0x00    /* Next Header carried inline */
#define IPHC_NH_COMPRESSED              0x04    /* Next Header compressed via NHC */

#define IPHC_HLIM_INLINE                0x00    /* Hop Limit carried inline (1 byte) */
#define IPHC_HLIM_1                     0x01    /* Hop Limit = 1 (elided) */
#define IPHC_HLIM_64                    0x02    /* Hop Limit = 64 (elided) */
#define IPHC_HLIM_255                   0x03    /* Hop Limit = 255 (elided) */

#define IPHC_CID                        0x80    /* Context Identifier Extension */
#define IPHC_SAC_STATEFUL               0x40    /* Source Address Context */
#define IPHC_SAM_128                    0x00    /* Full 128-bit Address inline */
#define IPHC_SAM_64                     0x10    /* 64-bit Address inline */
#define IPHC_SAM_16                     0x20    /* 16-bit Address inline */
#define IPHC_SAM_0                      0x30    /* Fully elided from Link Layer */

#define IPHC_M_MULTICAST                0x08    /* Multicast Destination */
#define IPHC_DAC_STATEFUL               0x04    /* Destination Address Context */
#define IPHC_DAM_128                    0x00    /* Full 128-bit Dest inline */
#define IPHC_DAM_64                     0x01    /* 64-bit Dest inline */
#define IPHC_DAM_16                     0x02    /* 16-bit Dest inline */
#define IPHC_DAM_0                      0x03    /* Fully elided from Link Layer */

/* UDP Next Header Compression (RFC 6282 Section 4.3.3) */
#define SIXLOWPAN_NHC_UDP_ID            0xF0    /* 1111 0xxx */
#define SIXLOWPAN_NHC_UDP_PORTS_INLINE  0x00    /* Both ports inline (4 bytes) */
#define SIXLOWPAN_NHC_UDP_PORTS_SRC_4B  0x01    /* Src 16-bit, Dst 8-bit */
#define SIXLOWPAN_NHC_UDP_PORTS_DST_4B  0x02    /* Src 8-bit, Dst 16-bit */
#define SIXLOWPAN_NHC_UDP_PORTS_4B_EACH 0x03    /* Both ports 4-bit compressed */
#define SIXLOWPAN_NHC_UDP_CSUM_ELIDED   0x04    /* Checksum elided */

/*
 * Reassembly Context Structure (RFC 4944 Section 5.3)
 */
struct sixlowpan_reassembly_context
{
    BOOL        active;
    UBYTE       src_eui64[8];
    UWORD       datagram_tag;
    UWORD       datagram_size;
    UWORD       received_bytes;
    ULONG       received_chunks_bitmap[6]; /* Bitmask of 8-byte chunks received */
    ULONG       timestamp_ms;
    UBYTE       buffer[SIXLOWPAN_MAX_IPV6_SIZE];
};

/*
 * Fragment Callback Definition for Transmit Engine
 */
typedef int (*sixlowpan_tx_func_t)(const UBYTE *frag_data, UWORD frag_len, void *user_data);

/* Function Prototypes */
void sixlowpan_engine_init(void);

WORD sixlowpan_compress_ipv6(const UBYTE *src_ipv6, UWORD src_len,
                             UBYTE *dst_frame, UWORD dst_max_len,
                             const UBYTE *src_eui64, const UBYTE *dst_eui64);

WORD sixlowpan_decompress_ipv6(const UBYTE *src_frame, UWORD src_len,
                               UBYTE *dst_ipv6, UWORD dst_max_len,
                               const UBYTE *src_eui64, const UBYTE *dst_eui64);

int  sixlowpan_fragment_and_send(const UBYTE *compressed_data, UWORD comp_len,
                                 UWORD uncompressed_size,
                                 sixlowpan_tx_func_t tx_func, void *user_data);

WORD sixlowpan_process_rx_fragment(const UBYTE *frag_data, UWORD frag_len,
                                   const UBYTE *src_eui64, ULONG current_time_ms,
                                   UBYTE *out_complete_ipv6, UWORD max_out_len);

#endif /* RISCV32_SIXLOWPAN_H */
