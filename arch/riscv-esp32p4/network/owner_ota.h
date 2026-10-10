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

#endif
