/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_WIFI_OWNER_OTA_QUEUE_H
#define P4_WIFI_OWNER_OTA_QUEUE_H
#include "../boardtool/update.h"
struct P4WifiBase;

/* ABI10 dispatcher contract, not write authorization. A trusted
 * future frontend worker must inspect/authenticate the source and live target
 * before submission. Caller input remains immutable through submit; accepted
 * submission copies all bytes before requesting radio quiescence. No caller
 * pointer is retained. Explicit budgets still fit the unqualified30s lease.
 * Resource hooks are NULL unless the owner, host privacy/recovery and separate
 * C6 update/recovery release assertions are enabled. Installed C6 credential
 * privacy is required for association, not for installing its first fix.
 * Those assertions are not runtime source/layout/recovery proofs. */
struct p4_wifi_ota_request {
    const uint8_t *image;
    uint32_t image_size, total_budget_us, finish_reserve_us, max_call_us;
    struct p4_board_update_gates gates;
};
enum p4_wifi_ota_job_state {
    P4_WIFI_OTA_JOB_INVALID = 0,
    P4_WIFI_OTA_JOB_QUEUED,
    P4_WIFI_OTA_JOB_RUNNING,
    P4_WIFI_OTA_JOB_DONE,
    P4_WIFI_OTA_JOB_BUSY
};
struct p4_wifi_ota_observation {
    uint32_t token, generation;
    enum p4_wifi_ota_job_state state;
    uint32_t acknowledged_bytes;
    uint8_t cancel_requested, result_valid;
    /* Valid only after DONE; never concurrently copied from a live engine. */
    struct p4_board_update_result result;
};
uint32_t p4_wifi_ota_submit(struct P4WifiBase *,
    const struct p4_wifi_ota_request *);
enum p4_wifi_ota_job_state p4_wifi_ota_poll(struct P4WifiBase *, uint32_t,
    struct p4_wifi_ota_observation *);
/* Poll/cancel/discard use a distinct short-held lock, never client_lock or
 * transport. The submitting task owns the token. Cancel is idempotent and
 * does not discard results, release firmware or imply restored peer state.
 * Close must request cancel and await DONE before discarding; no task-kill
 * recovery is provided. BUSY means retry a metadata operation, never OTA. */
int p4_wifi_ota_cancel(struct P4WifiBase *, uint32_t);
int p4_wifi_ota_discard(struct P4WifiBase *, uint32_t);
#endif
