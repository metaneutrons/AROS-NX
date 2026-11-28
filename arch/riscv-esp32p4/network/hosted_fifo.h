/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_HOSTED_FIFO_H
#define P4_HOSTED_FIFO_H
#include <stdint.h>

#define P4_HOSTED_FIFO_END UINT32_C(0x1f800)
#define P4_HOSTED_RX_MASK UINT32_C(0xfffff)
#define P4_HOSTED_TX_MASK UINT32_C(0xfff)
#define P4_HOSTED_FIFO_BUS_MAX UINT32_C(65536)

struct p4_hosted_fifo_plan {
    uint32_t argument;
    uint32_t logical_bytes;
    uint32_t bus_bytes;
    uint32_t credits;
};

/* One outstanding transfer under shared-host ownership. Counters start at
 * zero only after a known new slave transport session, never on reacquiring
 * the shared controller. Stale-generation calls refuse without altering the
 * current ledger, including its pending plan. The shared-host owner must
 * invalidate/rebootstrap its obsolete session, not fabricate consumed data.
 * Ambiguous FIFO completion in the matching generation poisons that session.
 * pool_limit comes from a validated peer contract, not a guessed free count.
 * Zero means unknown: RX of INIT is allowed, TX is refused until negotiation.
 */
struct p4_hosted_fifo {
    uint32_t generation;
    uint32_t rx_consumed;
    uint32_t tx_sent;
    uint32_t pool_limit;
    struct p4_hosted_fifo_plan pending;
    unsigned pending_write;
    unsigned busy;
    unsigned poisoned;
    unsigned initialized;
};

int p4_hosted_fifo_start(struct p4_hosted_fifo *state, uint32_t generation,
                         uint32_t pool_limit);
int p4_hosted_fifo_set_pool(struct p4_hosted_fifo *state, uint32_t generation,
                            uint32_t pool_limit);
/* fn1 incrementing CMD53; block-only bus padding is excluded from counters
 * and from END-logical_bytes addressing. RX lengths may aggregate frames.
 * Output remains untouched on refusal and must not overlap state.
 * No I/O, allocation or IRQ operation.
 */
int p4_hosted_fifo_rx(struct p4_hosted_fifo *state, uint32_t generation,
                      uint32_t peer_total, struct p4_hosted_fifo_plan *plan);
int p4_hosted_fifo_tx(struct p4_hosted_fifo *state, uint32_t generation,
                      uint32_t peer_token_register, uint32_t wire_bytes,
                      struct p4_hosted_fifo_plan *plan);
/* success: exactly 0 or 1; started: exactly 0 or 1. Failed pre-start requests
 * release the pending plan without advancing. Any started failure/partial
 * success poisons. completed_bytes must equal bus_bytes on full success.
 * The caller must obtain these facts from the shared command motor result.
 */
int p4_hosted_fifo_finish(struct p4_hosted_fifo *state, uint32_t generation,
                          unsigned started, unsigned success,
                          uint32_t completed_bytes);
#endif
