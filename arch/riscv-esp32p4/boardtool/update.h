#ifndef P4_BOARD_UPDATE_H
#define P4_BOARD_UPDATE_H

/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include <stddef.h>
#include <stdint.h>

#define P4_BOARD_UPDATE_CHUNK_SIZE 238u
/* Matches the smallest source OTA slot; target layout remains a separate gate. */
#define P4_BOARD_UPDATE_MAX_IMAGE_SIZE UINT32_C(1572864)
#define P4_BOARD_UPDATE_NO_FAILED_CHUNK UINT32_MAX
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
    P4_BOARD_UPDATE_INSTALLED
};

enum p4_board_update_call_result {
    P4_BOARD_UPDATE_CALL_ACKNOWLEDGED = 0,
    P4_BOARD_UPDATE_CALL_REJECTED = 1,
    P4_BOARD_UPDATE_CALL_AMBIGUOUS = 2
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
                size_t capacity, size_t *actual_size);
};

/* Callbacks are synchronous and must complete under a finite adapter
 * deadline; the engine bounds request count, not callback wall-clock time.
 * Context spans must describe their complete objects and must not overlap the
 * plan, result or scratch. Context objects may share storage with each other.
 * A write callback issues at most one remote request, consumes data before
 * returning and never retains it. Return ACKNOWLEDGED only for a verified
 * matching success response; uncertain responses are AMBIGUOUS. No callback
 * may retain any pointer argument after returning. */
struct p4_board_update_ops {
    void *context;
    size_t context_size;
    /* Zero means an exclusive lease was acquired; nonzero means none was. */
    int (*acquire_maintenance)(void *context);
    int (*release_maintenance)(void *context);
    /* Must confirm immutable source ownership remains held through the run,
     * then revalidate the inspected object immediately before BEGIN. */
    int (*revalidate_source)(void *context, uint32_t expected_size);
    int (*is_cancelled)(void *context);
    enum p4_board_update_call_result (*begin)(void *context,
                                               uint32_t image_size,
                                               int32_t *remote_status);
    enum p4_board_update_call_result (*write)(void *context,
                                              const uint8_t *data,
                                              size_t data_size,
                                              int32_t *remote_status);
    enum p4_board_update_call_result (*end)(void *context,
                                            int32_t *remote_status);
};

struct p4_board_update_plan {
    uint32_t image_size;
    struct p4_board_update_gates gates;
    struct p4_board_update_reader reader;
    struct p4_board_update_ops ops;
    uint8_t *scratch;
    size_t scratch_size;
};

/* Result owns no pointers. Progress counts only acknowledged WRITE requests. */
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
