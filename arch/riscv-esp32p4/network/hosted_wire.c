#include "hosted_wire.h"

/* The relocatable BSP module must not pull C-runtime initialization, whose
 * .eh_frame section the boot loader does not place. Volatile byte accesses
 * keep the compiler from lowering this overlap-safe copy back to memmove.
 * Integer-address ordering avoids relational comparison of unrelated objects. */
static void payload_move(uint8_t *destination, const uint8_t *source,
                          size_t length)
{
    volatile uint8_t *out = destination;
    const volatile uint8_t *in = source;
    size_t index;
    if ((uintptr_t)destination > (uintptr_t)source &&
        (uintptr_t)destination - (uintptr_t)source < length)
    {
        for (index = length; index != 0; --index)
            out[index - 1] = in[index - 1];
    }
    else
    {
        for (index = 0; index < length; ++index)
            out[index] = in[index];
    }
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

/* Additive byte checksum, with the checksum field treated as zero. */
static uint16_t frame_sum(const uint8_t *p, size_t length)
{
    size_t i;
    uint16_t sum = 0;
    for (i = 0; i < length; ++i)
        if (i != 6 && i != 7)
            sum = (uint16_t)(sum + p[i]);
    return sum;
}

static int valid_fields(unsigned type, unsigned number,
                        unsigned flags, unsigned throttle)
{
    return type >= P4_HOSTED_STA && type <= P4_HOSTED_ETH &&
        number <= 15 && (flags & ~P4_HOSTED_MORE_FRAGMENT) == 0 &&
        throttle <= 2;
}

int p4_hosted_decode(const uint8_t *bytes, size_t available,
                     unsigned checksum_enabled, struct p4_hosted_frame *frame)
{
    struct p4_hosted_frame candidate;
    size_t offset, size, total;
    if (!bytes || !frame || checksum_enabled > 1 ||
        available < P4_HOSTED_HEADER_SIZE)
        return 0;
    candidate.interface_type = bytes[0] & 15;
    candidate.interface_number = bytes[0] >> 4;
    candidate.flags = bytes[1];
    /* This profile rejects reserved bits instead of silently interpreting
     * a future header as the negotiated older protocol. */
    candidate.throttle = bytes[10];
    if (!valid_fields(candidate.interface_type, candidate.interface_number,
                      candidate.flags, candidate.throttle))
        return 0;
    size = get16(bytes + 2);
    offset = get16(bytes + 4);
    if (size == 0 || offset < P4_HOSTED_HEADER_SIZE ||
        offset > P4_HOSTED_FRAME_MAX || size > P4_HOSTED_FRAME_MAX - offset)
        return 0;
    total = offset + size;
    if (total > available || (checksum_enabled &&
        frame_sum(bytes, total) != get16(bytes + 6)))
        return 0;
    candidate.sequence = get16(bytes + 8);
    candidate.packet_type = bytes[11];
    candidate.payload = bytes + offset;
    candidate.payload_size = size;
    candidate.wire_size = total;
    *frame = candidate;
    return 1;
}

int p4_hosted_encode(uint8_t *bytes, size_t capacity,
                     unsigned checksum_enabled,
                     const struct p4_hosted_frame *frame, size_t *written)
{
    size_t total;
    if (!bytes || !frame || !written || checksum_enabled > 1 ||
        !frame->payload || frame->payload_size == 0 ||
        frame->payload_size > P4_HOSTED_FRAME_MAX - P4_HOSTED_HEADER_SIZE ||
        !valid_fields(frame->interface_type, frame->interface_number,
                      frame->flags, frame->throttle))
        return 0;
    total = P4_HOSTED_HEADER_SIZE + frame->payload_size;
    if (capacity < total)
        return 0;
    payload_move(bytes + P4_HOSTED_HEADER_SIZE, frame->payload,
                   frame->payload_size);
    bytes[0] = (uint8_t)((frame->interface_number << 4) | frame->interface_type);
    bytes[1] = frame->flags;
    put16(bytes + 2, (uint16_t)frame->payload_size);
    put16(bytes + 4, P4_HOSTED_HEADER_SIZE);
    put16(bytes + 6, 0);
    put16(bytes + 8, frame->sequence);
    bytes[10] = frame->throttle;
    bytes[11] = frame->packet_type;
    if (checksum_enabled)
        put16(bytes + 6, frame_sum(bytes, total));
    *written = total;
    return 1;
}
