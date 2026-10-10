/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_HOSTED_INBOX_H
#define P4_HOSTED_INBOX_H

#include "hosted_rpc.h"

#define P4_HOSTED_INBOX_CAPACITY 4u

/* Every queued envelope owns a complete bounded payload. */
struct p4_hosted_inbox_item {
    uint32_t msg_type;
    uint32_t msg_id;
    uint32_t uid;
    size_t payload_size;
    uint8_t payload[P4_HOSTED_RPC_MAX_SIZE];
};

/* Caller-exclusive, serialized state; this is not a concurrent queue. */
struct p4_hosted_inbox {
    struct p4_hosted_inbox_item items[P4_HOSTED_INBOX_CAPACITY];
    uint32_t generation;
    size_t head;
    size_t count;
    unsigned initialized;
    unsigned poisoned;
};

/* Zero-initialize the object before its first call. A matching generation
 * cannot be restarted; a different nonzero generation must represent a
 * transport epoch whose freshness was established by the caller. */
int p4_hosted_inbox_init(struct p4_hosted_inbox *inbox,
                         uint32_t generation);

/* Accept only bounded EVENT (769..776) or RESPONSE (513..605) envelopes.
 * Events may use UID zero; responses require a nonzero UID. The descriptor,
 * its nonempty payload and the inbox must be disjoint and overflow-safe.
 * An attempted push into a full matching inbox wipes and poisons it. */
int p4_hosted_inbox_push(struct p4_hosted_inbox *inbox,
                         uint32_t generation,
                         const struct p4_hosted_rpc *rpc);

/* Copy the oldest owned item to out and wipe its slot. The output and inbox
 * ranges must be disjoint. Refusal leaves both state and output unchanged. */
int p4_hosted_inbox_pop(struct p4_hosted_inbox *inbox,
                        uint32_t generation,
                        struct p4_hosted_inbox_item *out);

/* Wipe all owned payloads and poison only the matching generation. */
int p4_hosted_inbox_invalidate(struct p4_hosted_inbox *inbox,
                               uint32_t generation);

#endif
