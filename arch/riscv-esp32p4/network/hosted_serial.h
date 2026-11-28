/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef HOSTED_SERIAL_H
#define HOSTED_SERIAL_H

#include <stddef.h>
#include <stdint.h>

/* Independently implemented ESP-Hosted pserial TLV byte codec.
 * The 4096-byte protobuf cap is local defensive policy, not a vendor limit.
 * This codec handles one complete serial message only; it does not implement
 * ESP-Hosted framing, transport fragmentation, or reassembly. */
#define HOSTED_SERIAL_ENDPOINT_SIZE 6u
#define HOSTED_SERIAL_TLV_HEADER_SIZE 3u
#define HOSTED_SERIAL_MESSAGE_OVERHEAD 12u
#define HOSTED_SERIAL_PROTOBUF_MAX 4096u
#define HOSTED_SERIAL_MESSAGE_MAX \
    (HOSTED_SERIAL_MESSAGE_OVERHEAD + HOSTED_SERIAL_PROTOBUF_MAX)

enum hosted_serial_endpoint {
    HOSTED_SERIAL_ENDPOINT_RPCRSP = 1,
    HOSTED_SERIAL_ENDPOINT_RPCEVT = 2
};

struct hosted_serial_message {
    enum hosted_serial_endpoint endpoint;
    const uint8_t *protobuf; /* Borrowed from the decoded wire buffer. */
    size_t protobuf_size;
    size_t wire_size; /* Set by decode; ignored by encode. */
};

/* Decodes exactly one complete message. Unknown endpoints/types, empty or
 * over-limit protobuf data, truncation, and trailing bytes are refused.
 * The output is unchanged on refusal and borrows protobuf bytes from `bytes`.
 * `message` must not overlap the `available` input byte range. */
int hosted_serial_decode(const uint8_t *bytes, size_t available,
                         struct hosted_serial_message *message);

/* Emits canonical endpoint and data TLVs. Refusal leaves both `bytes` and
 * `*written` unchanged. The output range must not overlap the message object,
 * `written`, or the protobuf input; `message`, `written`, and protobuf input
 * must also be pairwise disjoint. */
int hosted_serial_encode(uint8_t *bytes, size_t capacity,
                         const struct hosted_serial_message *message,
                         size_t *written);

#endif
