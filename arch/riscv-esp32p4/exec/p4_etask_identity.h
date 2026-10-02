/* Private ETask identity/PID registry leaf; callers own synchronization. */
#ifndef P4_ETASK_IDENTITY_H
#define P4_ETASK_IDENTITY_H

#include <stdint.h>

#define P4_ETASK_IDENTITY_LIMIT 20000u

struct p4_etask_identity_record
{
    struct p4_etask_identity_record *next;
    const void *identity;
    uint32_t pid;
};

struct p4_etask_identity_registry
{
    struct p4_etask_identity_record *head;
    uint32_t count;
};

/* Every call requires the caller-held metadata topology gate. The same gate
 * must cover the caller's shared PID counter read/update and all metadata
 * publication/expunge transitions. These helpers never dereference identity,
 * allocate/free storage, acquire locks, or invoke callbacks.
 *
 * Records remain linked while their metadata is active or retained-dead and
 * may be released only at final metadata expunge. Reserve requires a fresh,
 * all-zero record and a non-NULL opaque identity; success links that record
 * and returns its reserved nonzero PID. The caller then publishes the
 * returned PID to its shared counter before dropping the same gate. A zero
 * return refuses without changing registry, record, or caller counter.
 *
 * The registry is maintained in strictly ascending PID order. This permits
 * bounded gap selection without repeated full-list searches. All records
 * must be created/released through this API while holding the gate. Bounded
 * shape checks detect count/head disagreement, cycles, null identities,
 * zero/duplicate/out-of-order PIDs, but cannot validate arbitrary pointer
 * provenance or recover from memory corruption that makes a linked record
 * address unreadable. Identity uniqueness is enforced when reserving.
 *
 * Lookup validates the full bounded shape and exactly one requested identity
 * before returning a borrowed record. It never changes the registry; the
 * caller must keep the same gate held through any use or owner-state change.
 * NULL indicates absent identity or invalid registry, never a usable prefix.
 * Release unlinks by opaque identity and returns the detached record; the
 * caller frees it only after dropping the topology gate. The detached record
 * retains identity/PID fields, and its next field is cleared. */
/* Initialization accepts only an already zero-initialized registry. */
int p4_etask_identity_registry_init(
    struct p4_etask_identity_registry *registry);
uint32_t p4_etask_identity_reserve(
    struct p4_etask_identity_registry *registry,
    struct p4_etask_identity_record *record,
    const void *identity,
    uint32_t current_counter);
struct p4_etask_identity_record *p4_etask_identity_lookup(
    const struct p4_etask_identity_registry *registry,
    const void *identity);
struct p4_etask_identity_record *p4_etask_identity_release(
    struct p4_etask_identity_registry *registry,
    const void *identity);

#endif
