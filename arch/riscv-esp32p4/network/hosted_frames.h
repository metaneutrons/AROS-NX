#ifndef P4_HOSTED_FRAMES_H
#define P4_HOSTED_FRAMES_H

/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include <stddef.h>
#include <stdint.h>

/* Local bounded policy, not a negotiated C6 limit. The pinned STA callback
 * passes complete Wi-Fi Ethernet data through as one STA/0 SDIO message; this
 * queue accepts a complete base Ethernet header and caps each owned frame at
 * 1518 bytes. */
#define P4_HOSTED_FRAME_QUEUE_CAPACITY 4u
#define P4_HOSTED_ETHERNET_HEADER_SIZE 14u
#define P4_HOSTED_ETHERNET_FRAME_MAX 1518u

struct p4_hosted_owned_frame {
    size_t size;
    uint8_t bytes[P4_HOSTED_ETHERNET_FRAME_MAX];
};

/* Fixed-capacity caller-owned queue, serialized by its lifecycle owner. */
struct p4_hosted_frames {
    struct p4_hosted_owned_frame items[P4_HOSTED_FRAME_QUEUE_CAPACITY];
    uint32_t generation;
    size_t head;
    size_t count;
    unsigned initialized;
    unsigned poisoned;
};

/* Zero-initialize once. Rebind only to a different nonzero generation after
 * the caller independently establishes peer freshness; same-generation
 * restart is refused. Reinitialization wipes all old frame bytes. */
int p4_hosted_frames_init(struct p4_hosted_frames *frames,
                          uint32_t generation);

/* Copy one complete Ethernet frame into owned storage. Lengths outside
 * 14..1518 are refused. The input and full queue object ranges must be
 * overflow-safe and disjoint. A push attempted while full wipes and poisons
 * this matching generation rather than silently dropping a frame. */
int p4_hosted_frames_push(struct p4_hosted_frames *frames,
                          uint32_t generation,
                          const uint8_t *bytes, size_t size);

/* Copy the oldest frame to caller storage, publish its byte length and wipe
 * the consumed slot. Queue, full output-capacity and output-size ranges must
 * be overflow-safe and pairwise disjoint. Refusal leaves state and outputs
 * unchanged. Call only outside shared-host ownership; this is not concurrent. */
int p4_hosted_frames_pop(struct p4_hosted_frames *frames,
                         uint32_t generation,
                         uint8_t *out, size_t capacity, size_t *out_size);

/* Wipe every owned frame and poison only the matching generation. */
int p4_hosted_frames_invalidate(struct p4_hosted_frames *frames,
                                uint32_t generation);

#endif
