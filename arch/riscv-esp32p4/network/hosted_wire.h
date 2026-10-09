#ifndef P4_HOSTED_WIRE_H
#define P4_HOSTED_WIRE_H

#include <stddef.h>
#include <stdint.h>

/* Independently implemented ESP-Hosted 0.0.6 wire profile. Not a native ABI.
 * Limit includes the header, but excludes SDIO DMA/block padding.
 * Transport capacity and firmware compatibility still require negotiation. */
#define P4_HOSTED_HEADER_SIZE 12u
#define P4_HOSTED_FRAME_MAX 1536u
#define P4_HOSTED_MORE_FRAGMENT 1u

enum p4_hosted_interface {
    P4_HOSTED_STA = 1, P4_HOSTED_AP, P4_HOSTED_SERIAL,
    P4_HOSTED_HCI, P4_HOSTED_PRIVATE, P4_HOSTED_TEST, P4_HOSTED_ETH
};

struct p4_hosted_frame {
    uint8_t interface_type;
    uint8_t interface_number;
    uint8_t flags;
    uint8_t throttle; /* 0 unchanged, 1 throttle on, 2 throttle off */
    uint8_t packet_type;
    uint16_t sequence;
    const uint8_t *payload; /* Borrowed until receive buffer is released. */
    size_t payload_size;
    size_t wire_size; /* Actual header/payload, not transport padding. */
};

/* checksum_enabled must be exactly 0 or 1, from the startup capabilities.
 * Trailing DMA padding is allowed; callers split streaming batches using
 * wire_size. No sequence policy or fragment reassembly is implied here.
 * Refusal leaves *frame untouched. */
int p4_hosted_decode(const uint8_t *bytes, size_t available,
                     unsigned checksum_enabled, struct p4_hosted_frame *frame);

/* Creates a canonical offset=12 frame, no block padding. Payload may overlap
 * bytes (memmove semantics). Refusal preserves bytes and *written.
 * written/frame metadata must not alias the output buffer. */
int p4_hosted_encode(uint8_t *bytes, size_t capacity,
                     unsigned checksum_enabled,
                     const struct p4_hosted_frame *frame, size_t *written);

#endif
