#ifndef P4_BOARD_UPDATE_H
#define P4_BOARD_UPDATE_H

/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include <stddef.h>
#include <stdint.h>

#define P4_BOARD_UPDATE_CHUNK_SIZE 238u
/* Matches the smallest source OTA slot; target layout remains a separate gate. */
#define P4_BOARD_UPDATE_MAX_IMAGE_SIZE UINT32_C(1572864)
#define P4_BOARD_UPDATE_NO_FAILED_CHUNK UINT32_MAX
/* Arithmetic bound, not measured transfer qualification. A deployment must
 * choose and qualify its total budget; no production default is supplied. */
#define P4_BOARD_UPDATE_MAX_TOTAL_US INT32_MAX
#define P4_BOARD_UPDATE_MAX_CALL_US UINT32_C(10000000)
#define P4_BOARD_UPDATE_MAX_CHUNKS \
    ((P4_BOARD_UPDATE_MAX_IMAGE_SIZE + P4_BOARD_UPDATE_CHUNK_SIZE - 1u) / \
     P4_BOARD_UPDATE_CHUNK_SIZE)

enum p4_board_update_status {
    P4_BOARD_UPDATE_INVALID_INPUT = 0,
    P4_BOARD_UPDATE_PREFLIGHT_REFUSED,
    P4_BOARD_UPDATE_CANCELLED_BEFORE_BEGIN,
    P4_BOARD_UPDATE_LEASE_FAILED,
    P4_BOARD_UPDATE_SOURCE_CHANGED,
    P4_BOARD_UPDATE_BEGIN_FAILED,
    P4_BOARD_UPDATE_BEGIN_UNKNOWN,
    P4_BOARD_UPDATE_SOURCE_READ_FAILED,
    P4_BOARD_UPDATE_SOURCE_BAD_COUNT,
    P4_BOARD_UPDATE_SHORT_READ,
    P4_BOARD_UPDATE_TRAILING_DATA,
    P4_BOARD_UPDATE_CANCELLED_INCOMPLETE,
    P4_BOARD_UPDATE_WRITE_REJECTED,
    P4_BOARD_UPDATE_WRITE_UNKNOWN,
    P4_BOARD_UPDATE_END_COMMIT_UNKNOWN,
    P4_BOARD_UPDATE_WAIT_REBOOT,
    P4_BOARD_UPDATE_POSTBOOT_FAILED,
    P4_BOARD_UPDATE_INSTALLED,
    P4_BOARD_UPDATE_BUDGET_EXPIRED,
    P4_BOARD_UPDATE_CLOCK_FAILED
};

enum p4_board_update_budget_error {
    P4_BOARD_UPDATE_BUDGET_OK = 0,
    P4_BOARD_UPDATE_BUDGET_TIMEOUT,
    P4_BOARD_UPDATE_BUDGET_CLOCK
};

enum p4_board_update_call_result {
    P4_BOARD_UPDATE_CALL_ACKNOWLEDGED = 0,
    P4_BOARD_UPDATE_CALL_REJECTED = 1,
    P4_BOARD_UPDATE_CALL_AMBIGUOUS = 2
};

/* Terminal owner policy, not permission to resume traffic. Once BEGIN has
 * been attempted even a rejected response cannot prove the fallback untouched.
 * No terminal path may replay a request or send END as cleanup. */
enum p4_board_update_finish_mode {
    P4_BOARD_UPDATE_FINISH_NONE = 0,
    P4_BOARD_UPDATE_FINISH_NO_BEGIN,
    P4_BOARD_UPDATE_FINISH_INCOMPLETE,
    P4_BOARD_UPDATE_FINISH_COMMIT_UNKNOWN,
    P4_BOARD_UPDATE_FINISH_WAIT_REBOOT
};

/* These are trusted caller attestations from manifest/inspection work. The
 * engine does not parse manifests, authenticate images, or compute a digest. */
struct p4_board_update_gates {
    uint8_t manifest_accepted;
    uint8_t inspection_passed;
    uint8_t recovery_verified;
    uint8_t target_compatible;
};

/* read() returns zero on success and sets actual_size to the exact number of
 * bytes copied, never more than capacity. EOF is success with zero bytes. */
struct p4_board_update_reader {
    void *context;
    size_t context_size;
    int (*read)(void *context, uint32_t offset, uint8_t *destination,
                size_t capacity, size_t *actual_size, uint32_t timeout_us);
};

/* Callbacks are synchronous and must complete under a finite adapter
 * deadline. Every blocking callback receives a positive timeout capped by
 * the remaining original aggregate budget and max_call_us. It must enforce
 * that timeout, including lock acquisition and I/O; the engine cannot preempt
 * a misbehaving callback. Cancellation/clock queries must be nonblocking.
 * Context spans must describe their complete objects and must not overlap the
 * plan, result or scratch. Context objects may share storage with each other.
 * A write callback issues at most one remote request, consumes data before
 * returning and never retains it. Return ACKNOWLEDGED only for a verified
 * matching success response; uncertain responses are AMBIGUOUS. No callback
 * may retain any pointer argument after returning. */
struct p4_board_update_ops {
    void *context;
    size_t context_size;
    /* Return exactly1 with a monotonic microsecond clock modulo2^32. It must
     * remain valid across the <=INT32_MAX-us job; no wall clock or reset. */
    int (*clock_us)(void *context, uint32_t *now);
    /* Zero means an exclusive lease was acquired; nonzero means none was. */
    int (*acquire_maintenance)(void *context, uint32_t timeout_us);
    /* Called at most once after acquiring a lease, only while the original
     * deadline and clock remain valid, with the terminal policy selected
     * by the engine. NO_BEGIN permits ordinary bounded read-only release.
     * All other modes must hand ownership to a fenced recovery/reboot state:
     * no WiFiStart, normal polling, auto-reassociation or automatic reset.
     * Return zero only after that bounded handoff is confirmed, not merely
     * after a release request was queued. A failure remains visible in the
     * result and must not erase the transfer/commit classification. */
    int (*finish_maintenance)(void *context,
                              enum p4_board_update_finish_mode mode,
                              uint32_t timeout_us);
    /* Must confirm immutable source ownership remains held through the run,
     * then revalidate the inspected object immediately before BEGIN. */
    int (*revalidate_source)(void *context, uint32_t expected_size,
                              uint32_t timeout_us);
    int (*is_cancelled)(void *context);
    enum p4_board_update_call_result (*begin)(void *context,
                                               uint32_t image_size,
                                               int32_t *remote_status,
                                               uint32_t timeout_us);
    enum p4_board_update_call_result (*write)(void *context,
                                              const uint8_t *data,
                                              size_t data_size,
                                              int32_t *remote_status,
                                              uint32_t timeout_us);
    enum p4_board_update_call_result (*end)(void *context,
                                            int32_t *remote_status,
                                            uint32_t timeout_us);
};

struct p4_board_update_plan {
    uint32_t image_size;
    /* One nonrenewable deadline starts before cancellation/lease acquisition.
     * Work cannot consume finish_reserve_us. Finalization uses only the
     * remaining original deadline, never a fresh reserve after expiry. */
    uint32_t total_budget_us;
    uint32_t finish_reserve_us;
    uint32_t max_call_us;
    struct p4_board_update_gates gates;
    struct p4_board_update_reader reader;
    struct p4_board_update_ops ops;
    uint8_t *scratch;
    size_t scratch_size;
};

/* Result owns no pointers. Progress counts only acknowledged WRITE requests.
 * The legacy lease_release_* names record the once-only terminal handoff;
 * they do not mean that normal radio traffic has been resumed. If the clock
 * fails or the total deadline expires before handoff, finish_mode is still
 * selected but lease_release_called remains0 and lease_release_status is -1.
 * A late verified END acknowledgement remains WAIT_REBOOT, not installed;
 * budget_error and unconfirmed handoff prevent postboot confirmation. */
struct p4_board_update_result {
    enum p4_board_update_status status;
    uint32_t image_size;
    uint32_t total_chunks;
    uint32_t acknowledged_chunks;
    uint32_t acknowledged_bytes;
    uint32_t failed_chunk_index;
    int32_t callback_status;
    int32_t local_status;
    int32_t lease_release_status;
    enum p4_board_update_finish_mode finish_mode;
    enum p4_board_update_budget_error budget_error;
    uint32_t elapsed_us;
    uint8_t lease_acquired;
    uint8_t lease_release_called;
    uint8_t begin_attempted;
    uint8_t end_attempted;
};

/* The input plan, callback contexts and scratch must remain valid throughout
 * this call. The function is single-flight and stores none of their pointers.
 * Callbacks may write only their documented out-parameters and callback
 * context. They must not alter the plan or result or retain pointers. */
enum p4_board_update_status
p4_board_update_run(const struct p4_board_update_plan *plan,
                    struct p4_board_update_result *result);

/* Call only after an explicit post-reboot identity and health observation.
 * These attestations do not represent a digest computed by this engine. */
int p4_board_update_confirm_postboot(struct p4_board_update_result *result,
                                     int target_identity_matches,
                                     int health_check_passed);

#endif
