/*
 * Copyright (c) 2026, The AROS Development Team. All rights reserved.
 *
 * Portable state-machine core for the ESP32-P4 Exec signal/wait protocol.
 * This file deliberately has no dependency on AROS headers or kernel IPI APIs.
 */

#ifndef P4_SIGNAL_PROTOCOL_H
#define P4_SIGNAL_PROTOCOL_H

#include <stdint.h>

/*
 * These values describe task ownership, not the numeric Exec tc_State ABI.
 * The P4 adapter maps TS_RUN/READY/WAIT and removed states to this enum.
 */
enum p4_signal_owner_state
{
    P4_SIGNAL_OWNER_INVALID = 0,
    P4_SIGNAL_OWNER_RUNNING,
    P4_SIGNAL_OWNER_READY,
    P4_SIGNAL_OWNER_WAITING,
    P4_SIGNAL_OWNER_REMOVED
};

enum p4_signal_queue_transition
{
    P4_SIGNAL_QUEUE_NONE = 0,
    P4_SIGNAL_QUEUE_RUNNING_TO_READY,
    P4_SIGNAL_QUEUE_RUNNING_TO_WAIT,
    P4_SIGNAL_QUEUE_WAITING_TO_READY
};

enum p4_signal_wait_outcome
{
    P4_SIGNAL_WAIT_NO_CHANGE = 0,
    P4_SIGNAL_WAIT_ARMED,
    P4_SIGNAL_WAIT_RETURNED_SIGNALS
};

enum p4_signal_status
{
    P4_SIGNAL_STATUS_OK = 0,
    P4_SIGNAL_STATUS_INVALID_ARGUMENT,
    P4_SIGNAL_STATUS_INVALID_STATE,
    P4_SIGNAL_STATUS_NOT_LIVE,
    P4_SIGNAL_STATUS_PENDING_BUSY,
    P4_SIGNAL_STATUS_PENDING_OWNER_MISMATCH
};

/*
 * A lock-protected view of the shared Task fields. exception_flag_mask is
 * supplied by the adapter; the portable core does not assume the AROS
 * TF_EXCEPT value or even that the flag uses a particular bit.
 */
struct p4_signal_task_state
{
    uint32_t sig_received;
    uint32_t sig_wait;
    uint32_t sig_except;
    uint32_t owner_hart;
    uint8_t flags;
    uint8_t exception_flag_mask;
    enum p4_signal_owner_state owner;
};

/*
 * Queue changes and rescheduling are effects, not actions performed by this
 * state machine. The caller must apply queue_transition while it still holds
 * the task lock. Its scheduler helper acquires the required queue lock(s) in
 * the established order and publishes matching tc_State/queue ownership
 * before returning to the caller.
 *
 * After releasing all task/queue locks, the caller may honor
 * request_reschedule at reschedule_hart. A local Reschedule() or remote IPI
 * must never run while those locks are held.
 */
struct p4_signal_effect
{
    enum p4_signal_status status;
    enum p4_signal_queue_transition queue_transition;
    enum p4_signal_wait_outcome wait_outcome;
    uint32_t returned_signals;
    uint32_t reschedule_hart;
    uint8_t request_reschedule;
    uint8_t remote_reschedule;
};

/*
 * Adapter contract for effects returned by this state machine:
 *
 * - The P4 adapter calls its queue-apply helper only for a non-NONE
 *   queue_transition, while the task lock remains held. The helper acquires
 *   ordered source/destination queue locks, verifies old membership, performs
 *   exactly one list move, and publishes matching tc_State before returning.
 *   The helper returns a negative value on failure, zero when no poke is
 *   needed, or (hart_id + 1) when a scheduler poke is required. The encoding
 *   keeps hart zero distinct from the no-poke result.
 * - After all task/queue locks are released, convert the returned hart to an
 *   immutable poke effect and call the platform's asynchronous poke path.
 *   For request_reschedule, use reschedule_hart. No Task pointer is required
 *   by the poke API, so a caller can release a task lifetime pin before poke.
 *
 * These are integration contracts, not functions called by the portable
 * state-machine core. Never wait for a remote hart or invoke user callbacks
 * while a task or queue lock is held.
 */
/*
 * All functions ending in _locked require the caller to hold the canonical
 * Task state lock (Task.tc_SpinLock in the P4 adapter). The same lock must
 * serialize every read or update of received/wait/exception masks, flags,
 * owner state and the per-hart wait-pending token.
 *
 * Wait arm and switch commit form a two-phase handshake:
 * - arm publishes sig_wait and wait_pending_slot=task_key while owner stays
 *   RUNNING;
 * - after context save, switch commit consumes that exact token and returns
 *   RUNNING_TO_WAIT or RUNNING_TO_READY according to the final signal check.
 * A non-NULL token for another task is rejected without being changed.
 *
 * A returned queue transition is not evidence that a queue operation
 * happened. The caller must apply it before unlocking, or fail closed. No
 * callback, scheduler entry, Reschedule(), or IPI is performed here.
 */
struct p4_signal_effect p4_signal_wait_arm_locked(
    struct p4_signal_task_state *state,
    void **wait_pending_slot,
    void *task_key,
    uint32_t wait_mask);

struct p4_signal_effect p4_signal_switch_commit_locked(
    struct p4_signal_task_state *state,
    void **wait_pending_slot,
    void *task_key);

struct p4_signal_effect p4_signal_raise_locked(
    struct p4_signal_task_state *state,
    /* True only when the owning hart's pending slot equals this task. */
    int wait_armed,
    uint32_t signal_set,
    uint32_t caller_hart);

struct p4_signal_effect p4_signal_set_locked(
    struct p4_signal_task_state *state,
    uint32_t signal_mask,
    uint32_t new_signals);

struct p4_signal_effect p4_signal_set_except_locked(
    struct p4_signal_task_state *state,
    uint32_t signal_mask,
    uint32_t new_exceptions,
    uint32_t caller_hart);

/* Consume one exception snapshot under lock; call user code after unlocking. */
uint32_t p4_signal_exception_take_locked(
    struct p4_signal_task_state *state);

/*
 * Merge an exception callback's returned mask under lock. Returns the mask
 * that is pending for another exception pass; it does not call the callback.
 */
uint32_t p4_signal_exception_merge_locked(
    struct p4_signal_task_state *state,
    uint32_t callback_mask);

#endif /* P4_SIGNAL_PROTOCOL_H */
