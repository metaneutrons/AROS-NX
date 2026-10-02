#ifndef P4_REMOVE_BANK_H
#define P4_REMOVE_BANK_H

#include <stdint.h>
#include "p4_remove_completion.h"

#define P4_REMOVE_BANK_MAX_SLOTS 20000u

/* Handles name a bank slot without exposing its address. Slot indices are
 * zero-based; generations never wrap during one bank lifetime. */
struct p4_remove_handle
{
    uint32_t index;
    uint32_t generation;
};

/* Caller-provided permanent storage. Treat fields as private bank state; no
 * operation returns a pointer to one of these records. The kernel registry's
 * fused admit_removal and finish_removal transactions may update validated
 * slots under the same gate to combine retirement/ticket publication or ACK,
 * recipient reservation and final target detach. Fused admission may also
 * reuse a canceled requester_ref0 slot with a fresh generation: the target's
 * registry service ownership is separate from the ticket service ref. The
 * standalone cancel/drop contract below remains unchanged; canceled handles
 * must not be used afterward. Production service finalization scans under
 * the registry gate, never retaining a bank handle across gate release.
 * Task identities are
 * opaque integer values and are never dereferenced by this module. */
struct p4_remove_bank_slot
{
    uintptr_t requester_identity;
    uintptr_t target_identity;
    uint32_t generation;
    uint32_t active;
    struct p4_remove_completion completion;
};

/* Header and slot array must remain mapped and stable for the bank lifetime.
 * Every operation except unpublished initialization requires the caller to
 * hold the same registry/completion gate. */
struct p4_remove_bank
{
    uint32_t magic;
    uint32_t capacity;
    uint32_t serial;
    struct p4_remove_bank_slot *slots;
};

/* Initialize only zero-initialized, unpublished header and slot storage.
 * Returns 1 on success and -1 on malformed/overlapping storage. */
int p4_remove_bank_init(struct p4_remove_bank *bank,
                        struct p4_remove_bank_slot *slots,
                        uint32_t capacity);

/* Reserve independent requester and service references. A full bank or
 * exhausted nonwrapping serial returns 0. Invalid identity/output/storage
 * returns -1. Refusal leaves all bank state and output unchanged. */
int p4_remove_bank_reserve(struct p4_remove_bank *bank,
                           uintptr_t requester_identity,
                           uintptr_t target_identity,
                           struct p4_remove_handle *out_handle);

/* A false ready value is not permission to free the target's Task storage. */
int p4_remove_bank_ready(const struct p4_remove_bank *bank,
                         struct p4_remove_handle handle,
                         uintptr_t expected_requester,
                         int *out_ready);

/* Drop only the requester's reference. cancel=1 may drop before ACK; cancel=0
 * requires completion. The service reference is never consumed here. */
int p4_remove_bank_drop(struct p4_remove_bank *bank,
                        struct p4_remove_handle handle,
                        uintptr_t expected_requester,
                        int cancel,
                        int *out_reclaim);

/* Cancel all requester references for one retiring opaque identity. The
 * caller holds the registry gate through this call and publication of
 * retirement, before that requester's storage can become cleanup-eligible.
 * Service references survive; already-ACKed tickets are reclaimed. Returns
 * 1 for a valid bank (including zero matches), -1 for malformed/aliased input.
 * Validation precedes every mutation; out_dropped is unchanged on refusal. */
int p4_remove_bank_cancel_requester(struct p4_remove_bank *bank,
                                   uintptr_t requester_identity,
                                   uint32_t *out_dropped);

/* ACK only after the caller has completed every target/context/free access.
 * A nonzero notify identity means the requester reference still exists; the
 * caller must reserve/pin its recipient before releasing the shared gate. */
int p4_remove_bank_ack(struct p4_remove_bank *bank,
                       struct p4_remove_handle handle,
                       uintptr_t expected_target,
                       uintptr_t *out_notify_requester,
                       int *out_reclaim);

/* Count or find outstanding service references for one opaque target ID.
 * next_target scans from first_index (which may equal capacity) and returns
 * handles, never slot pointers. All of these calls require the shared gate. */
int p4_remove_bank_count_target(const struct p4_remove_bank *bank,
                                uintptr_t target_identity,
                                uint32_t *out_count);
int p4_remove_bank_next_target(const struct p4_remove_bank *bank,
                               uintptr_t target_identity,
                               uint32_t first_index,
                               struct p4_remove_handle *out_handle);

#endif
