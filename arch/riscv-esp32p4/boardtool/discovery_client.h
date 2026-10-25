/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_BOARD_DISCOVERY_CLIENT_H
#define P4_BOARD_DISCOVERY_CLIENT_H
#include "discovery_wire.h"

/* Adapter states deliberately match private ABI8. An adapter must check the
 * actual ABI/size/hooks before entering this transaction. No transport or OTA
 * entry point is exposed here. All hooks run on one requesting task. */
enum board_client_lease_state {
    BOARD_CLIENT_LEASE_IDLE, BOARD_CLIENT_LEASE_REQUESTED,
    BOARD_CLIENT_LEASE_ACTIVE, BOARD_CLIENT_LEASE_RELEASING,
    BOARD_CLIENT_LEASE_FAILED, BOARD_CLIENT_LEASE_RELEASED,
    BOARD_CLIENT_LEASE_BUSY
};
enum board_client_discovery_state {
    BOARD_CLIENT_DISCOVERY_IDLE, BOARD_CLIENT_DISCOVERY_REQUESTED,
    BOARD_CLIENT_DISCOVERY_READY, BOARD_CLIENT_DISCOVERY_UNSUPPORTED,
    BOARD_CLIENT_DISCOVERY_FAILED, BOARD_CLIENT_DISCOVERY_BUSY
};
enum board_client_result {
    BOARD_CLIENT_OK = 0, BOARD_CLIENT_INVALID, BOARD_CLIENT_UNAVAILABLE,
    BOARD_CLIENT_TIMEOUT, BOARD_CLIENT_CANCELLED, BOARD_CLIENT_FAILED,
    BOARD_CLIENT_UNSUPPORTED, BOARD_CLIENT_REMOTE_ERROR,
    BOARD_CLIENT_BAD_REPLY, BOARD_CLIENT_RELEASE_UNCONFIRMED,
    BOARD_CLIENT_CLOCK_FAILED
};
struct board_client_reply {
    uint32_t token, generation, uid, nonce;
    int32_t remote_status;
    uint32_t inventory_size;
    uint8_t inventory[BOARD_DISCOVERY_WIRE_MAX];
};
struct board_client_observation {
    uint32_t token, generation, uid, nonce;
    struct board_discovery_snapshot snapshot;
};
struct board_client_ops {
    /* Monotonic milliseconds modulo2^32, stable across this <=30s job.
     * Return0 on an unavailable/invalid clock. Wall clock is not acceptable. */
    int (*clock_ms)(void *, uint32_t *);
    void (*wait)(void *); /* Bounded pacing; may not wait indefinitely. */
    int (*cancelled)(void *);
    uint32_t (*request)(void *);
    unsigned (*lease_poll)(void *, uint32_t);
    int (*discover)(void *, uint32_t);
    unsigned (*discovery_poll)(void *, uint32_t, struct board_client_reply *);
    int (*release)(void *, uint32_t);
};
/* Caller-owned scratch; use heap storage in small-stack frontends. */
struct board_client_workspace {
    struct board_client_reply reply;
    struct board_client_observation candidate;
};
#define BOARD_CLIENT_WORK_MS 22000u
#define BOARD_CLIENT_TOTAL_MS 30000u

/* At most one lease and one job. Reserve8s for confirmed release within the
 * original30s aggregate budget. Success publishes only after RELEASED; WiFi
 * stays offline/unassociated. Output is a historical registry observation,
 * not raw-table attestation, authentication or write/recovery permission.
 * Refusal leaves output unchanged. Workspace/output/context must be disjoint;
 * neither callback nor adapter may retain scratch/output pointers. */
enum board_client_result board_client_discover(const struct board_client_ops *,
    void *, size_t context_size, struct board_client_workspace *,
    struct board_client_observation *);
#endif
