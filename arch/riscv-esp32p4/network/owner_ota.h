/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_WIFI_OWNER_OTA_H
#define P4_WIFI_OWNER_OTA_H

#include "../boardtool/update.h"
struct P4WifiBase;

/* Port-private owner entry, NOT a resource ABI hook or write authorization.
 * Only the actual radio owner, holding client_lock but not the shared host,
 * may call it inside its active task/token/generation maintenance lease.
 * Gates are trusted caller attestations, not authentication implemented here.
 * The future job dispatcher must own an immutable inspected image, verify live
 * target/layout and independent recovery, and enforce the aggregate engine
 * deadline. The original 30s lease is not a qualified full-image budget.
 * No pointer is retained. Status changes only for a decoded matching reply.
 * Preflight refusal performs no I/O or mutation. After entering BEGIN even a
 * local failure is sticky; any failed/ambiguous attempted command closes the
 * transaction. No retry, reset, cleanup END or normal radio release follows.
 * WRITE counts only acknowledged bytes; END requires the exact image length.
 * An ACK is not installation or postboot health. */
enum p4_board_update_call_result p4_wifi_owner_ota_exchange(
    struct P4WifiBase *base, uint32_t token, uint32_t generation,
    uint32_t request_id, const uint8_t *data, size_t data_size,
    uint32_t image_size, const struct p4_board_update_gates *gates,
    uint32_t timeout_us, int32_t *remote_status);

/* Direct owner-side engine finalization, never the requester queue/poll hook.
 * Explicitly maps engine modes to the native ABI; no enum cast is valid.
 * NO_BEGIN may restart only a pristine read-only lease. Any begun mode closes
 * the transaction and retains sticky silence even on timeout/session failure.
 * Zero means a bounded confirmed handoff, never queued intent or C6 health.
 * This does not supply the future job/aggregate-budget adapter. */
int p4_wifi_owner_ota_finish(struct P4WifiBase *base, uint32_t token,
    uint32_t generation, enum p4_board_update_finish_mode mode,
    uint32_t timeout_us);

/* Synchronous owner-only engine binding. The future queue must own and freeze
 * these bytes BEFORE quiescence, and retain them through completion. These
 * callbacks are trusted internal ownership/cancellation queries: nonblocking,
 * no transport, no detach/Close/re-entry and no mutation of image/job/result.
 * Inspection/authenticity/layout/recovery remain caller attestations, not
 * properties established by this structure. No pointer is retained on return.
 * total_budget_us is measured from the ORIGINAL maintenance request, including
 * quiescence; it cannot exceed the existing 30s preparation lease. This binding
 * does not provide a qualified full-image lease or an asynchronous ABI job.
 * INVALID_INPUT admission refusals leave result/state unchanged and the
 * requester retains its existing lease/release obligation. Accepted runs own
 * finalization even if the engine exits before its acquire-validation callback;
 * no clock failure or expired deadline permits a late restart. */
struct p4_wifi_owner_ota_job {
    const uint8_t *image;
    uint32_t image_size;
    uint32_t total_budget_us, finish_reserve_us, max_call_us;
    struct p4_board_update_gates gates;
    void *source_context;
    size_t source_context_size;
    int (*source_held)(void *);
    int (*cancelled)(void *);
};
enum p4_board_update_status p4_wifi_owner_ota_run(struct P4WifiBase *base,
    uint32_t token, uint32_t generation,
    const struct p4_wifi_owner_ota_job *job,
    struct p4_board_update_result *result);

#endif
