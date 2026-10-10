/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_HOSTED_OPEN_H
#define P4_HOSTED_OPEN_H

#include <stddef.h>
#include <stdint.h>

#include "hosted_fifo.h"
#include "hosted_init.h"
#include "hosted_wire.h"

#define P4_HOSTED_OPEN_CONTROL_ADDRESS UINT32_C(0x8c)
#define P4_HOSTED_OPEN_VALUE UINT8_C(1)
#define P4_HOSTED_CLOSE_VALUE UINT8_C(2)
#define P4_HOSTED_OPEN_INTERRUPT_ENABLE UINT8_C(0x03)
#define P4_HOSTED_OPEN_BLOCK_SIZE UINT16_C(512)
#define P4_HOSTED_OPEN_MAX_POLLS 100u
#define P4_HOSTED_OPEN_LOGICAL_BUFFER_CAPACITY 4096u

#define P4_HOSTED_OPEN_INT_RAW UINT32_C(0x50)
#define P4_HOSTED_OPEN_INT_CLEAR UINT32_C(0xd4)
#define P4_HOSTED_OPEN_PACKET_LENGTH UINT32_C(0x60)
#define P4_HOSTED_OPEN_INT_PACKET (UINT32_C(1) << 23)
#define P4_HOSTED_OPEN_INT_RX_UNDERFLOW (UINT32_C(1) << 16)
#define P4_HOSTED_OPEN_INT_TX_OVERFLOW (UINT32_C(1) << 17)
#define P4_HOSTED_OPEN_INT_CLEAR_MASK \
    ((UINT32_C(1) << 23) | (UINT32_C(1) << 7) | (UINT32_C(1) << 6))
#define P4_HOSTED_OPEN_RX_COUNTER_MASK UINT32_C(0xfffff)

enum p4_hosted_open_snapshot_flag {
    P4_HOSTED_OPEN_SAVED_INTERRUPT_ENABLE = 1u << 0,
    P4_HOSTED_OPEN_SAVED_BLOCK_LOW = 1u << 1,
    P4_HOSTED_OPEN_SAVED_BLOCK_HIGH = 1u << 2,
    P4_HOSTED_OPEN_SAVED_ALL = 7u
};

enum p4_hosted_open_stage {
    P4_HOSTED_OPEN_STAGE_NONE = 0,
    P4_HOSTED_OPEN_STAGE_ARGUMENTS,
    P4_HOSTED_OPEN_STAGE_SNAPSHOT_INTERRUPT_ENABLE,
    P4_HOSTED_OPEN_STAGE_SNAPSHOT_BLOCK_LOW,
    P4_HOSTED_OPEN_STAGE_SNAPSHOT_BLOCK_HIGH,
    P4_HOSTED_OPEN_STAGE_WRITE_INTERRUPT_ENABLE,
    P4_HOSTED_OPEN_STAGE_VERIFY_INTERRUPT_ENABLE,
    P4_HOSTED_OPEN_STAGE_WRITE_BLOCK_LOW,
    P4_HOSTED_OPEN_STAGE_WRITE_BLOCK_HIGH,
    P4_HOSTED_OPEN_STAGE_VERIFY_BLOCK_LOW,
    P4_HOSTED_OPEN_STAGE_VERIFY_BLOCK_HIGH,
    P4_HOSTED_OPEN_STAGE_WRITE_OPEN,
    P4_HOSTED_OPEN_STAGE_READ_PREOPEN_LENGTH,
    P4_HOSTED_OPEN_STAGE_VALIDATE_PREOPEN_LENGTH,
    P4_HOSTED_OPEN_STAGE_POLL_BUDGET,
    P4_HOSTED_OPEN_STAGE_POLL_LIMIT,
    P4_HOSTED_OPEN_STAGE_READ_INTERRUPT,
    P4_HOSTED_OPEN_STAGE_CLEAR_INTERRUPT,
    P4_HOSTED_OPEN_STAGE_READ_LENGTH,
    P4_HOSTED_OPEN_STAGE_PLAN_FIFO,
    P4_HOSTED_OPEN_STAGE_BUFFER_CAPACITY,
    P4_HOSTED_OPEN_STAGE_FIFO_BUDGET,
    P4_HOSTED_OPEN_STAGE_READ_FIFO,
    P4_HOSTED_OPEN_STAGE_FINISH_FIFO,
    P4_HOSTED_OPEN_STAGE_DECODE_FRAME,
    P4_HOSTED_OPEN_STAGE_VALIDATE_FRAME,
    P4_HOSTED_OPEN_STAGE_CHECKSUM,
    P4_HOSTED_OPEN_STAGE_PARSE_INIT,
    P4_HOSTED_OPEN_STAGE_VALIDATE_CHIP,
    P4_HOSTED_OPEN_STAGE_CLEANUP_CLOSE,
    P4_HOSTED_OPEN_STAGE_CLEANUP_WRITE_INTERRUPT_ENABLE,
    P4_HOSTED_OPEN_STAGE_CLEANUP_WRITE_BLOCK_LOW,
    P4_HOSTED_OPEN_STAGE_CLEANUP_WRITE_BLOCK_HIGH,
    P4_HOSTED_OPEN_STAGE_CLEANUP_READ_INTERRUPT_ENABLE,
    P4_HOSTED_OPEN_STAGE_CLEANUP_READ_BLOCK_LOW,
    P4_HOSTED_OPEN_STAGE_CLEANUP_READ_BLOCK_HIGH
};

enum p4_hosted_open_error {
    P4_HOSTED_OPEN_ERROR_NONE = 0,
    P4_HOSTED_OPEN_ERROR_INVALID_ARGUMENT,
    P4_HOSTED_OPEN_ERROR_BUFFER_TOO_SMALL,
    P4_HOSTED_OPEN_ERROR_REGISTER_VERIFY,
    P4_HOSTED_OPEN_ERROR_CALLBACK,
    P4_HOSTED_OPEN_ERROR_BUDGET_OR_GENERATION,
    P4_HOSTED_OPEN_ERROR_STALE_COUNTER,
    P4_HOSTED_OPEN_ERROR_POLL_EXHAUSTED,
    P4_HOSTED_OPEN_ERROR_RX_UNDERFLOW,
    P4_HOSTED_OPEN_ERROR_TX_OVERFLOW,
    P4_HOSTED_OPEN_ERROR_FIFO_COUNTER,
    P4_HOSTED_OPEN_ERROR_INVALID_FRAME,
    P4_HOSTED_OPEN_ERROR_FRAME_PROFILE,
    P4_HOSTED_OPEN_ERROR_DUPLICATE_INIT,
    P4_HOSTED_OPEN_ERROR_INVALID_INIT,
    P4_HOSTED_OPEN_ERROR_UNSUPPORTED_CHIP,
    P4_HOSTED_OPEN_ERROR_CHECKSUM
};

enum p4_hosted_open_cleanup_error {
    P4_HOSTED_OPEN_CLEANUP_NOT_RUN = 0,
    P4_HOSTED_OPEN_CLEANUP_OK,
    P4_HOSTED_OPEN_CLEANUP_CALLBACK,
    P4_HOSTED_OPEN_CLEANUP_BUDGET_OR_GENERATION,
    P4_HOSTED_OPEN_CLEANUP_INVALID_STATE,
    P4_HOSTED_OPEN_CLEANUP_VERIFY_MISMATCH
};

enum p4_hosted_open_cleanup_flag {
    P4_HOSTED_OPEN_CLEANUP_CLOSE_ATTEMPTED = 1u << 0,
    P4_HOSTED_OPEN_CLEANUP_CLOSE_WRITE_OK = 1u << 1,
    P4_HOSTED_OPEN_CLEANUP_WRITE_IE_ATTEMPTED = 1u << 2,
    P4_HOSTED_OPEN_CLEANUP_WRITE_BLOCK_LOW_ATTEMPTED = 1u << 3,
    P4_HOSTED_OPEN_CLEANUP_WRITE_BLOCK_HIGH_ATTEMPTED = 1u << 4,
    P4_HOSTED_OPEN_CLEANUP_WRITE_IE_OK = 1u << 5,
    P4_HOSTED_OPEN_CLEANUP_WRITE_BLOCK_LOW_OK = 1u << 6,
    P4_HOSTED_OPEN_CLEANUP_WRITE_BLOCK_HIGH_OK = 1u << 7,
    P4_HOSTED_OPEN_CLEANUP_READ_IE_ATTEMPTED = 1u << 8,
    P4_HOSTED_OPEN_CLEANUP_READ_BLOCK_LOW_ATTEMPTED = 1u << 9,
    P4_HOSTED_OPEN_CLEANUP_READ_BLOCK_HIGH_ATTEMPTED = 1u << 10,
    P4_HOSTED_OPEN_CLEANUP_VERIFY_IE_OK = 1u << 11,
    P4_HOSTED_OPEN_CLEANUP_VERIFY_BLOCK_LOW_OK = 1u << 12,
    P4_HOSTED_OPEN_CLEANUP_VERIFY_BLOCK_HIGH_OK = 1u << 13
};

/* All operations run synchronously under the caller's existing shared-host
 * ownership. Register words are little-endian Fn1 CMD53 byte-mode transfers.
 * read_fifo must fill exactly plan->bus_bytes, including block padding, and
 * return 1 only after the complete transfer succeeds. A zero return is
 * ambiguous: the receive stream may have advanced, so this gate poisons the
 * FIFO session and never retries it. `yield` is mandatory and is called after
 * each successful empty-packet poll. budget_ok returns 1 only while the
 * caller's deadline and shared-host generation/ownership remain valid. */
struct p4_hosted_open_ops {
    void *context;
    int (*read_byte)(void *context, unsigned function, uint32_t address,
                     uint8_t *value);
    int (*write_byte)(void *context, unsigned function, uint32_t address,
                      uint8_t value);
    int (*read_word)(void *context, uint32_t address, uint32_t *value);
    int (*write_word)(void *context, uint32_t address, uint32_t value);
    int (*read_fifo)(void *context,
                     const struct p4_hosted_fifo_plan *plan,
                     uint8_t *buffer);
    int (*budget_ok)(void *context);
    void (*yield)(void *context);
};

/* Diagnostics and cleanup state are kept separately from gate success.
 * frame.payload borrows `buffer`; retain that buffer for as long as the frame
 * is used. Snapshot bytes are meaningful only when their corresponding flag
 * is set. Cleanup completion means the writes/readbacks completed at the bus
 * callback level; CLOSE is not a readback of the C6's datapath state. */
struct p4_hosted_open_result {
    unsigned initialized;
    enum p4_hosted_open_stage stage;
    enum p4_hosted_open_error error;
    unsigned gate_ok;
    uint32_t generation;
    unsigned polls;
    uint32_t raw_interrupt;
    uint32_t preopen_length_counter;
    uint32_t length_counter;
    uint32_t logical_bytes;
    uint32_t additional_frame_count;
    uint32_t additional_frame_bytes;
    uint32_t additional_serial_frame_count;
    uint32_t additional_bytes_discarded; /* Complete outer wire bytes skipped. */
    unsigned suffix_preserved;
    unsigned suffix_taken;
    uint32_t serial_suffix_offset;
    uint32_t serial_suffix_bytes;
    struct p4_hosted_fifo_plan fifo_plan;
    struct p4_hosted_frame frame;
    struct p4_hosted_init init;
    uint8_t saved_interrupt_enable;
    uint8_t saved_block_low;
    uint8_t saved_block_high;
    unsigned snapshot_flags;
    unsigned fn0_mutation_attempted;
    unsigned open_attempted;
    unsigned stream_consumed;
    unsigned stream_ambiguous;
    unsigned fifo_poisoned;
    struct p4_hosted_fifo fifo_state;
    unsigned cleanup_attempted;
    unsigned cleanup_complete;
    enum p4_hosted_open_stage cleanup_stage;
    enum p4_hosted_open_cleanup_error cleanup_error;
    unsigned cleanup_flags;
};

/* Snapshot Fn0 CCCR IE/block-size bytes before mutation, enable IE bits0/1,
 * set and verify 512-byte Fn0 block size, require a zero pre-OPEN RX counter,
 * send OPEN, then receive one bounded private INIT frame and any bounded
 * serial frames aggregated behind it. A zero counter is a necessary
 * stale-session guard, not proof of a new C6 epoch.
 * The pinned slave emits INIT once at initial OPEN; later OPEN events only
 * toggle the datapath. A fresh C6 power startup is required for an accepted
 * first-INIT gate. The caller provides its persistent buffer;
 * 4096 bytes is the recommended capacity. It validates following
 * unfragmented interface-3, number-0 Hosted frames when the pinned slave
 * aggregated them with INIT, records their count and wire size, then discards
 * their payloads opaquely. This is not RPC parsing or acceptance. Aliasing
 * among buffer, ops, and result is rejected before output/callback access;
 * the result is untouched on alias refusal.
 * No allocation, TX, host reply, RPC dispatch, reset or implicit cleanup
 * occurs here. On failure, call cleanup separately
 * while the caller's cleanup budget and host ownership are active. */
int p4_hosted_open_receive_init(const struct p4_hosted_open_ops *ops,
                                uint32_t generation, uint8_t *buffer,
                                size_t capacity,
                                struct p4_hosted_open_result *result);

/* Same bounded OPEN/INIT gate, but retain and validate every following
 * SERIAL/0 frame in buffer. The raw suffix starts at serial_suffix_offset
 * and has serial_suffix_bytes bytes only when gate_ok and suffix_preserved
 * are both set. suffix_taken starts at zero; the accepted one-shot RX handoff
 * sets it before validating/copying suffix contents, so it cannot be replayed
 * after a content failure. It may contain MORE fragments and must be handed
 * to the new RX assembler before buffer reuse. It is not parsed or dispatched
 * here. */
int p4_hosted_open_receive_init_preserve(
    const struct p4_hosted_open_ops *ops, uint32_t generation,
    uint8_t *buffer, size_t capacity,
    struct p4_hosted_open_result *result);

/* CLOSE is attempted first whenever OPEN was attempted, including an
 * ambiguous failed OPEN callback. If this gate attempted any Fn0 mutation,
 * restore all three snapshotted bytes and verify each one. Uses only the
 * caller's current cleanup budget; it never renews it. It does not undo a C6
 * reset, restore an unconsumed RX stream, or prove C6 application state. */
int p4_hosted_open_cleanup(const struct p4_hosted_open_ops *ops,
                           struct p4_hosted_open_result *result);

#endif
