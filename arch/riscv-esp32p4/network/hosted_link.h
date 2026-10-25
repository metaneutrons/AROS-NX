/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_HOSTED_LINK_H
#define P4_HOSTED_LINK_H

#include "hosted_association.h"

enum p4_hosted_link_kind {
    P4_HOSTED_LINK_NONE = 0,
    P4_HOSTED_LINK_UP_HINT,
    P4_HOSTED_LINK_DOWN,
    P4_HOSTED_LINK_STATION_START,
    P4_HOSTED_LINK_STATION_STOP
};

/* Owner-only observation ledger, not association acceptance. Event775 has
 * UID zero: even an exact SSID/BSSID match cannot identify the Connect RPC
 * which caused it. UP_HINT must never directly publish S2EVENT_CONNECT or
 * permit Ethernet delivery. A later correlated current-AP query and active
 * target/lifecycle checks are required. Generation separates host sessions,
 * not delayed events from different attempts in the same C6 session.
 * DOWN and station START/STOP observations invalidate any earlier hint.
 * Lifecycle observations carry no authenticated AP identity and are not
 * causal attempt barriers; later UID-zero events can still be delayed. */
struct p4_hosted_link_observation {
    uint32_t generation;
    uint32_t revision;
    uint32_t kind;
    int32_t status;
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint32_t ssid_length;
    uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    uint32_t channel;
    int32_t authmode;
    uint32_t reason;
};

/* Initialize/reset outside transport dispatch, after discarding every prior
 * observation/confirmation token. This resets revision even for the same
 * generation; (generation, revision) is not valid across initialization.
 * Nonzero generation required; refusal leaves output unchanged.
 * No borrowed bytes survive either call. */
int p4_hosted_link_init(struct p4_hosted_link_observation *link,
                         uint32_t generation);

/* Accept typed Event775/776 and successful Event773 station START/STOP,
 * UID zero and the matching live generation. Other valid Event773 IDs are
 * unrelated metadata: this function refuses them without changing the ledger.
 * Decode and validate before publishing a complete owned snapshot. Nonzero
 * connected status is DOWN, never UP_HINT. Revision exhaustion fails closed
 * rather than wrapping and accidentally validating an earlier confirmation.
 * link must be disjoint from rpc and its nonempty payload. Refusal leaves the
 * entire ledger unchanged; the owner retires on a malformed known event.
 * Unrelated event IDs are handled by the dispatcher, not by this function. */
int p4_hosted_link_observe(struct p4_hosted_link_observation *link,
                            uint32_t generation,
                            const struct p4_hosted_rpc *rpc);

#endif
