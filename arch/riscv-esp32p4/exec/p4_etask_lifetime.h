/* Private metadata ownership, separate from Task RUN/pin ownership. */
#ifndef P4_ETASK_LIFETIME_H
#define P4_ETASK_LIFETIME_H
#include <stdint.h>

struct p4_etask_lifetime
{
    uint32_t initialized;
    uint32_t task_ref;
    uint32_t result_ref;
    uint32_t child_edges;
    uint32_t readers;
    uint32_t closing;
    uint32_t expunge;
    uint32_t reclaim_claimed;
};

enum p4_etask_release
{
    P4_ETASK_REFUSED = 0,
    P4_ETASK_RETAINED = 1,
    P4_ETASK_RECLAIM = 2
};

/* All operations require one caller-held topology gate. No Task/ETask is
 * dereferenced and no allocation, free, notification or lock occurs here.
 * Child edges retain METADATA, never a Task pin. The parent cleanup must
 * reparent/orphan every edge before dropping its own metadata reference.
 * Temporary readers must not use this lease as permission to read Task
 * storage: acquire a Task pin outside the gate and revalidate the association.
 * RECLAIM is an exactly-once ownership transfer; free only after gate unlock,
 * and do not call these helpers again with reclaimed storage. */
/* init requires already cleared, all-zero storage; it is not a clearing
 * operation. Nonzero storage refuses without mutation. */
int p4_etask_lifetime_init(struct p4_etask_lifetime *state);
int p4_etask_lifetime_valid(const struct p4_etask_lifetime *state);
int p4_etask_lifetime_add_edge(struct p4_etask_lifetime *state);
int p4_etask_lifetime_drop_edge(struct p4_etask_lifetime *state);
int p4_etask_lifetime_read_pin(struct p4_etask_lifetime *state);
enum p4_etask_release p4_etask_lifetime_read_unpin(
    struct p4_etask_lifetime *state);
/* Exclusive cleanup only after Task RUN/pins drain, or failed unpublished
 * construction with creator ownership. Closing blocks new active edges and
 * readers. This primitive cannot itself verify Task registry ownership. */
int p4_etask_lifetime_begin_cleanup(struct p4_etask_lifetime *state);
/* retain_result transfers Task ownership to exactly one exited-result queue
 * owner. The caller must detach active membership and publish dead membership
 * atomically under the topology gate before releasing it. */
enum p4_etask_release p4_etask_lifetime_finish_cleanup(
    struct p4_etask_lifetime *state, int retain_result);
/* Caller must detach the exited node before dropping the result ownership. */
enum p4_etask_release p4_etask_lifetime_drop_result(
    struct p4_etask_lifetime *state);
#endif
