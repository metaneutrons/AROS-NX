/*
 * Bounded lookup and state transitions for P4 Task lifetime records.
 * The caller supplies one serialization gate; see p4_task_registry.h.
 */
#include "p4_task_registry.h"
#include <stddef.h>
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
#include "p4_semaphore_ledger.h"
#include "../exec/p4_remove_bank.h"
#endif

struct p4_task_registry_match
{
    struct p4_task_registry_record *record;
    struct p4_task_registry_record *previous;
};

static uint64_t p4_task_registry_construction_pins(
    const struct p4_task_registry_record *record)
{
    return (uint64_t)record->construction_children +
           (record->construction_creator != 0 ? 1u : 0u);
}

static int p4_task_registry_wait_scope_valid(
    const struct p4_task_registry_record *record)
{
    const struct p4_task_wait_scope *scope = &record->wait_scope;
    uint64_t minimum_pins = (uint64_t)record->service_pending +
                            p4_task_registry_construction_pins(record) +
                            record->worker_protected +
                            record->semaphore_pending +
                            record->semaphore_inflight;

    if (scope->active > 1u || scope->construction_closed > 1u ||
        scope->cancel_requested > 1u || scope->cancel_claimed > 1u)
        return 0;

    if (scope->active == 0)
        return scope->kind == 0 && scope->queued_count == 0 &&
               scope->construction_closed == 0 &&
               scope->cancel_requested == 0 && scope->cancel_claimed == 0 &&
               scope->semaphore_index == 0 && scope->request_index == 0 &&
               scope->list_index == 0;

    if (scope->generation == 0 ||
        (record->lifetime.flags & P4_TASK_LIFETIME_CLAIMED) != 0 ||
        (scope->cancel_requested != 0 &&
         (record->lifetime.flags & P4_TASK_LIFETIME_RETIRED) == 0) ||
        (scope->cancel_claimed != 0 && scope->cancel_requested == 0) ||
        record->lifetime.notification_pins <= minimum_pins)
        return 0;

    if (scope->kind == P4_TASK_WAIT_SINGLE)
        return scope->semaphore_index != 0 && scope->request_index != 0 &&
               scope->list_index == 0;

    if (scope->kind == P4_TASK_WAIT_LIST ||
        scope->kind == P4_TASK_WAIT_OPERATION)
        return scope->semaphore_index == 0 && scope->request_index == 0 &&
               scope->list_index != 0;

    return 0;
}

static int p4_task_registry_semaphore_reservations_valid(
    const struct p4_task_registry *registry,
    const struct p4_task_registry_record *record)
{
    uint64_t reserved = (uint64_t)record->service_pending +
                        p4_task_registry_construction_pins(record) +
                        record->worker_protected +
                        record->wait_scope.active +
                        record->semaphore_pending +
                        record->semaphore_inflight;

    if ((record->semaphore_pending == 0) !=
            (record->semaphore_pending_generation == 0) ||
        (record->semaphore_inflight == 0) !=
            (record->semaphore_inflight_generation == 0))
        return 0;

    if ((record->semaphore_pending != 0 &&
         record->semaphore_pending_generation >
             registry->semaphore_notification_serial) ||
        (record->semaphore_inflight != 0 &&
         record->semaphore_inflight_generation >
             registry->semaphore_notification_serial) ||
        (record->semaphore_pending != 0 &&
         record->semaphore_inflight != 0 &&
         record->semaphore_pending_generation ==
             record->semaphore_inflight_generation) ||
        reserved > record->lifetime.notification_pins)
        return 0;

    return 1;
}

static int p4_task_registry_service_generation_valid(
    const struct p4_task_registry *registry,
    const struct p4_task_registry_record *record)
{
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
    int owned = record->service_pending != 0u || record->service_inflight != 0u;
    return (record->service_generation != 0u) == owned &&
           record->service_generation <= registry->semaphore_notification_serial;
#else
    (void)registry;
    return record->service_generation == 0u;
#endif
}

/*
 * Validate the bounded list and find one identity. The count bounds traversal,
 * so a cycle or inconsistent count fails without following a link forever.
 * Duplicate matches for the requested identity also invalidate the lookup.
 */
static int p4_task_registry_find_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    struct p4_task_registry_record *candidate,
    struct p4_task_registry_match *match)
{
    struct p4_task_registry_record *record;
    struct p4_task_registry_record *previous = 0;
    struct p4_task_registry_record *found = 0;
    struct p4_task_registry_record *found_previous = 0;
    uint32_t visited = 0;
    uint32_t identity_children = 0;
    uint64_t children = 0, constructions = 0;

    if (registry == 0 || registry->count > P4_TASK_REGISTRY_LIMIT)
        return 0;

    record = registry->head;
    while (record != 0 && visited < registry->count &&
           visited < P4_TASK_REGISTRY_LIMIT)
    {
        if (record->identity == 0 ||
            ((record->creation_frame == 0u) !=
             (record->creation_generation == 0u)) ||
            ((record->creation_frame == 0u) !=
             (record->creation_target == 0u)) ||
            record->creation_claimed > 1u ||
            (record->creation_frame == 0u && record->creation_claimed != 0u) ||
            record->creation_generation > registry->creation_channel_serial ||
            (record->creation_frame != 0u &&
             (record->creation_frame % _Alignof(struct p4_creation_channel) != 0u ||
              sizeof(struct p4_creation_channel) >
                  UINTPTR_MAX - record->creation_frame)) ||
            record->taskresource_unobserved > 1u ||
            record->construction_children > P4_TASK_REGISTRY_LIMIT ||
            record->construction_generation > registry->construction_serial ||
            (record->construction_creator != 0 &&
             (record->construction_creator == record->identity ||
              record->construction_generation == 0u)) ||
            (record->lifetime.flags &
             ~(P4_TASK_LIFETIME_RETIRED | P4_TASK_LIFETIME_CLAIMED)) != 0 ||
            (record->lifetime.running_harts & ~UINT32_C(3)) != 0 ||
            record->lifetime.running_harts == UINT32_C(3) ||
            record->service_pending > 1u ||
            record->service_queued > 1u ||
            record->service_inflight > 1u ||
            record->service_postfree > 1u ||
            (record->service_postfree != 0u && record->service_inflight != 1u) ||
            record->worker_protected > 1u ||
            !p4_task_registry_service_generation_valid(registry, record) ||
            !p4_task_registry_semaphore_reservations_valid(registry, record) ||
            !p4_task_registry_wait_scope_valid(record) ||
            (record->service_inflight != 0 &&
             (record->lifetime.flags !=
                  (P4_TASK_LIFETIME_RETIRED | P4_TASK_LIFETIME_CLAIMED) ||
              record->taskresource_unobserved != 0u ||
              record->lifetime.notification_pins != 0 ||
              record->lifetime.running_harts != 0 ||
              record->service_pending != 0 ||
              record->service_queued != 0 ||
              record->semaphore_pending != 0 ||
              record->semaphore_inflight != 0 ||
              record->worker_protected != 0 ||
              record->construction_creator != 0 ||
              record->construction_children != 0u ||
              record->wait_scope.active != 0)) ||
            (record->service_inflight == 0 &&
             (record->lifetime.flags & P4_TASK_LIFETIME_CLAIMED) != 0) ||
            (record->worker_protected != 0 &&
             (record->lifetime.flags != 0 ||
              record->lifetime.notification_pins == 0 ||
              record->service_pending != 0 ||
              record->service_queued != 0)) ||
            (record->service_pending != 0 &&
             ((record->lifetime.flags & P4_TASK_LIFETIME_RETIRED) == 0 ||
              record->lifetime.notification_pins == 0)) ||
            (record->service_queued != 0 &&
             (record->service_pending != 1u ||
              (record->lifetime.flags & P4_TASK_LIFETIME_RETIRED) == 0 ||
              record->lifetime.notification_pins != 1u ||
              record->lifetime.running_harts != 0 ||
              record->wait_scope.active != 0 ||
              record->semaphore_pending != 0 ||
              record->semaphore_inflight != 0)))
            return 0;

        children += record->construction_children;
        constructions += record->construction_creator != 0 ? 1u : 0u;
        if (identity != 0 && record->construction_creator == identity)
            ++identity_children;

        if (candidate != 0 && record == candidate)
            return 0;

        if (identity != 0 && record->identity == identity)
        {
            if (found != 0)
                return 0;
            found = record;
            found_previous = previous;
        }

        previous = record;
        record = record->next;
        ++visited;
    }

    if (record != 0 || visited != registry->count ||
        children != constructions ||
        (found != 0 && found->construction_children != identity_children))
        return 0;

    if (match != 0)
    {
        match->record = found;
        match->previous = found_previous;
    }

    return 1;
}

static int p4_task_registry_record_is_quiescent(
    const struct p4_task_registry_record *record)
{
    return record != 0 && record->next == 0 && record->identity == 0 &&
           record->taskresource_unobserved == 0u &&
           record->service_pending == 0 && record->service_queued == 0 &&
           record->service_inflight == 0 &&
           record->service_postfree == 0 &&
           record->service_generation == 0 &&
           record->semaphore_pending == 0 &&
           record->semaphore_pending_generation == 0 &&
           record->semaphore_inflight == 0 &&
           record->semaphore_inflight_generation == 0 &&
           record->worker_protected == 0 &&
           record->construction_creator == 0 &&
           record->construction_generation == 0u &&
           record->construction_children == 0u &&
           record->creation_frame == 0u &&
           record->creation_generation == 0u &&
           record->creation_target == 0u &&
           record->creation_claimed == 0u &&
           record->wait_scope.generation == 0 &&
           record->wait_scope.active == 0 &&
           record->wait_scope.kind == 0 &&
           record->wait_scope.queued_count == 0 &&
           record->wait_scope.construction_closed == 0 &&
           record->wait_scope.cancel_requested == 0 &&
           record->wait_scope.cancel_claimed == 0 &&
           record->wait_scope.semaphore_index == 0 &&
           record->wait_scope.request_index == 0 &&
           record->wait_scope.list_index == 0 &&
           record->lifetime.notification_pins == 0 &&
           record->lifetime.running_harts == 0 && record->lifetime.flags == 0;
}

void p4_task_registry_init(struct p4_task_registry *registry)
{
    if (registry == 0)
        return;

    registry->head = 0;
    registry->count = 0;
    registry->semaphore_notification_serial = 0;
    registry->construction_serial = 0;
    registry->creation_channel_serial = 0;
}

int p4_task_registry_validate_locked(struct p4_task_registry *registry)
{
    struct p4_task_registry_match match;
    return p4_task_registry_find_locked(registry, 0, 0, &match);
}

int p4_task_registry_register_locked(
    struct p4_task_registry *registry,
    struct p4_task_registry_record *record,
    struct Task *identity)
{
    struct p4_task_registry_match match;

    if (registry == 0 || record == 0 || identity == 0 ||
        registry->count >= P4_TASK_REGISTRY_LIMIT ||
        !p4_task_registry_record_is_quiescent(record) ||
        !p4_task_registry_find_locked(registry, identity, record, &match) ||
        match.record != 0)
        return 0;

    /* The full scan above validates shape and excludes duplicate identity. */
    p4_task_lifetime_init(&record->lifetime);
    record->identity = identity;
    record->next = registry->head;
    registry->head = record;
    ++registry->count;
    return 1;
}

static struct p4_task_registry_record *p4_task_registry_lookup_locked(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_match match;

    if (identity == 0 || !p4_task_registry_find_locked(
            registry, identity, 0, &match))
        return 0;

    return match.record;
}

int p4_task_registry_taskresource_observer_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t mark_unobserved)
{
    struct p4_task_registry_match match;
    struct p4_task_registry_record *record;
    uint64_t durable_pins;

    if (registry == 0 || identity == 0 || mark_unobserved > 1u)
        return -1;

    if (!p4_task_registry_find_locked(registry, identity, 0, &match))
        return -1;

    record = match.record;
    if (record == 0 || record->lifetime.flags != 0u)
        return 0;

    durable_pins = (uint64_t)record->service_pending +
                   p4_task_registry_construction_pins(record) +
                   record->worker_protected + record->wait_scope.active +
                   record->semaphore_pending + record->semaphore_inflight;
    if (record->construction_creator == 0 &&
        record->lifetime.notification_pins <= durable_pins)
        return 0;

    if (mark_unobserved != 0u)
        record->taskresource_unobserved = 1u;
    return 1;
}

int p4_task_registry_pin_locked(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);

    return record != 0 && record->service_inflight == 0 &&
           p4_task_lifetime_pin_locked(&record->lifetime);
}

int p4_task_registry_unpin_locked(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);
    uint64_t minimum_pins;

    if (record == 0 || record->service_inflight != 0)
        return 0;

    minimum_pins = (uint64_t)record->service_pending +
                   p4_task_registry_construction_pins(record) +
                   record->worker_protected + record->wait_scope.active +
                   record->semaphore_pending + record->semaphore_inflight;
    if (record->lifetime.notification_pins <= minimum_pins)
        return 0;

    return p4_task_lifetime_unpin_locked(&record->lifetime);
}

int p4_task_registry_protect_worker_locked(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);

    if (record == 0 || record->worker_protected != 0 ||
        record->lifetime.flags != 0 ||
        (record->lifetime.running_harts != UINT32_C(1) &&
         record->lifetime.running_harts != UINT32_C(2)) ||
        record->service_pending != 0 || record->service_queued != 0 ||
        record->lifetime.notification_pins == UINT32_MAX)
        return 0;

    /* The permanent pin and marker publish together under the caller's gate. */
    ++record->lifetime.notification_pins;
    record->worker_protected = 1u;
    return 1;
}

int p4_task_registry_is_worker_locked(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_match match;

    if (identity == 0 ||
        !p4_task_registry_find_locked(registry, identity, 0, &match) ||
        match.record == 0)
        return -1;

    return match.record->worker_protected == 1u ? 1 : 0;
}

int p4_task_registry_retire_locked(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);

    return record != 0 && record->service_inflight == 0 &&
           record->worker_protected == 0 &&
           record->wait_scope.kind != P4_TASK_WAIT_OPERATION &&
           p4_task_lifetime_retire_locked(&record->lifetime);
}

int p4_task_registry_retire_for_service_locked(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);

    if (record == 0 || record->service_inflight != 0 ||
        record->worker_protected != 0 ||
        record->wait_scope.kind == P4_TASK_WAIT_OPERATION ||
        record->service_pending != 0 ||
        (record->lifetime.flags &
         (P4_TASK_LIFETIME_RETIRED | P4_TASK_LIFETIME_CLAIMED)) != 0 ||
        record->lifetime.notification_pins == UINT32_MAX)
        return 0;

#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
    /* Refuse BEFORE irreversible retirement. A later final-free ACK must
     * never depend on new serial capacity; this target keeps its own token. */
    if (registry->semaphore_notification_serial == UINT32_MAX)
        return 0;
    record->service_generation = ++registry->semaphore_notification_serial;
#endif

    /* The caller's lifetime gate makes service ownership and retirement one
     * publication: there is no instant where the retiring Task lacks a pin. */
    ++record->lifetime.notification_pins;
    record->lifetime.flags |= P4_TASK_LIFETIME_RETIRED;
    record->service_pending = 1u;
    return 1;
}

int p4_task_registry_enter_running_locked(
    struct p4_task_registry *registry, struct Task *identity, uint32_t hart)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);

    return record != 0 && record->service_inflight == 0 &&
           p4_task_lifetime_enter_running_locked(&record->lifetime, hart);
}

int p4_task_registry_leave_running_locked(
    struct p4_task_registry *registry, struct Task *identity, uint32_t hart)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);

    return record != 0 && record->service_inflight == 0 &&
           p4_task_lifetime_leave_running_locked(&record->lifetime, hart);
}

struct p4_task_registry_record *p4_task_registry_claim_reclaim_locked(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_record *next;
    struct p4_task_registry_match match;
    struct p4_task_registry_record *record;

    if (identity == 0 ||
        !p4_task_registry_find_locked(registry, identity, 0, &match) ||
        match.record == 0)
        return 0;

    record = match.record;
    if (record->service_inflight != 0 || record->service_pending != 0 ||
        record->wait_scope.active != 0 ||
        !p4_task_lifetime_claim_reclaim_locked(&record->lifetime))
        return 0;

    next = record->next;
    record->taskresource_unobserved = 0u;
    /* Clear its link while still registered; do not touch it after detach. */
    record->next = 0;
    if (match.previous == 0)
        registry->head = next;
    else
        match.previous->next = next;
    --registry->count;

    return record;
}

struct p4_task_registry_record *p4_task_registry_claim_service_locked(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_match match;
    struct p4_task_registry_record *record;

    if (identity == 0 ||
        !p4_task_registry_find_locked(registry, identity, 0, &match) ||
        match.record == 0)
        return 0;

    record = match.record;
    if (record->service_inflight != 0 || record->service_pending != 1u ||
        (record->lifetime.flags & P4_TASK_LIFETIME_RETIRED) == 0 ||
        (record->lifetime.flags & P4_TASK_LIFETIME_CLAIMED) != 0 ||
        record->lifetime.notification_pins != 1u ||
        record->lifetime.running_harts != 0 ||
        record->wait_scope.active != 0 ||
        record->semaphore_pending != 0 ||
        record->semaphore_inflight != 0)
        return 0;

    /* Consume the designated ServiceTask reference and claim while still
     * under the caller's gate; never expose a zero-pin linked record. */
    record->taskresource_unobserved = 0u;
    record->service_pending = 0;
    record->service_queued = 0;
    record->service_generation = 0;
    record->lifetime.notification_pins = 0;
    record->lifetime.flags |= P4_TASK_LIFETIME_CLAIMED;

    if (match.previous == 0)
        registry->head = record->next;
    else
        match.previous->next = record->next;
    --registry->count;
    record->next = 0;

    return record;
}

struct Task *p4_task_registry_take_service_ready_locked(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);

    if (record == 0 || record->service_inflight != 0 ||
        record->service_queued != 0 ||
        record->service_pending != 1u ||
        (record->lifetime.flags & P4_TASK_LIFETIME_RETIRED) == 0 ||
        (record->lifetime.flags & P4_TASK_LIFETIME_CLAIMED) != 0 ||
        record->lifetime.notification_pins != 1u ||
        record->lifetime.running_harts != 0 ||
        record->wait_scope.active != 0 ||
        record->semaphore_pending != 0 ||
        record->semaphore_inflight != 0)
        return 0;

    /* This is an exactly-once reservation only. It neither enqueues a
     * message nor transfers/detaches the registry record. */
    record->service_queued = 1u;
    return record->identity;
}

struct p4_task_registry_record *p4_task_registry_claim_queued_service_locked(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);
    if (record == 0 || record->service_queued != 1u)
        return 0;
    return p4_task_registry_claim_service_locked(registry, identity);
}

int p4_task_registry_claim_next_service_locked(
    struct p4_task_registry *registry,
    struct p4_task_registry_record **out_record)
{
    struct p4_task_registry_match match;
    struct p4_task_registry_record *record;
    struct p4_task_registry_record *claimed;
    uint32_t visited = 0;

    if (out_record == 0)
        return -1;
    *out_record = 0;

    /* Validate the complete bounded chain before selecting or detaching any
     * record. A later malformed link must not leave an earlier claim changed. */
    if (!p4_task_registry_find_locked(registry, 0, 0, &match))
        return -1;

    record = registry->head;
    while (record != 0 && visited < registry->count &&
           visited < P4_TASK_REGISTRY_LIMIT)
    {
        if (record->service_queued == 1u)
        {
            /* Re-run the identity lookup through the existing queued-claim
             * path. It rejects duplicate identity records before mutation. */
            claimed = p4_task_registry_claim_queued_service_locked(
                registry, record->identity);
            if (claimed == 0)
                return -1;
            *out_record = claimed;
            return 1;
        }
        record = record->next;
        ++visited;
    }

    /* The validated list should terminate after exactly count records. Keep
     * the second traversal independently bounded in case that invariant ever
     * changes or this helper is reused with a different validator. */
    if (record != 0 || visited != registry->count)
        return -1;
    return 0;
}

static int p4_task_registry_ranges_overlap(
    const void *first, uintptr_t first_size,
    const void *second, uintptr_t second_size)
{
    uintptr_t first_start = (uintptr_t)first;
    uintptr_t second_start = (uintptr_t)second;
    uintptr_t first_end;
    uintptr_t second_end;

    if (first_size > UINTPTR_MAX - first_start ||
        second_size > UINTPTR_MAX - second_start)
        return 1;

    first_end = first_start + first_size;
    second_end = second_start + second_size;
    return first_start < second_end && second_start < first_end;
}

/* This is called only after the complete registry has passed validation. */
static int p4_task_registry_output_aliases_locked(
    struct p4_task_registry *registry,
    const void *output,
    uintptr_t output_size)
{
    struct p4_task_registry_record *record = registry->head;
    uint32_t visited = 0;

    if (p4_task_registry_ranges_overlap(
            output, output_size, registry,
            (uintptr_t)sizeof(*registry)))
        return 1;

    while (record != 0 && visited < registry->count &&
           visited < P4_TASK_REGISTRY_LIMIT)
    {
        if (p4_task_registry_ranges_overlap(
                output, output_size, record,
                (uintptr_t)sizeof(*record)))
            return 1;
        record = record->next;
        ++visited;
    }

    /* The preceding full validation makes any other outcome impossible. */
    return record != 0 || visited != registry->count;
}

#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
static int p4_task_registry_creation_admit_locked(
    struct p4_task_registry *registry, struct Task *owner, uint32_t hart,
    struct p4_task_registry_match *match)
{
    if (owner == 0 || hart >= P4_TASK_LIFETIME_HARTS ||
        !p4_task_registry_find_locked(registry, owner, 0, match))
        return -1;
    if (match->record == 0 || match->record->lifetime.flags != 0u ||
        match->record->lifetime.running_harts != (UINT32_C(1) << hart))
        return 0;
    return 1;
}

int p4_task_registry_creation_begin_locked(struct p4_task_registry *registry,
    struct Task *owner, uint32_t hart, struct Task *target,
    struct p4_creation_channel *frame)
{
    struct p4_task_registry_match match;
    struct p4_task_registry_record *cursor;
    int admitted = p4_task_registry_creation_admit_locked(
        registry, owner, hart, &match);
    if (admitted != 1)
        return admitted;
    if (target == 0 || target == owner || frame == 0 ||
        (uintptr_t)frame % _Alignof(struct p4_creation_channel) != 0u ||
        sizeof(*frame) > UINTPTR_MAX - (uintptr_t)frame ||
        p4_task_registry_output_aliases_locked(registry, frame, sizeof(*frame)))
        return -1;
    /* Check all active frame extents before reading trusted caller storage. */
    for (cursor = registry->head; cursor != 0; cursor = cursor->next)
        if (cursor->creation_frame != 0u &&
            p4_task_registry_ranges_overlap(frame, sizeof(*frame),
                (const void *)cursor->creation_frame, sizeof(*frame)))
            return -1;
    if (frame->generation != 0u || frame->previous_frame != 0u ||
        frame->previous_generation != 0u || frame->previous_target != 0u ||
        frame->previous_claimed != 0u)
        return -1;
    if (registry->creation_channel_serial == UINT32_MAX)
        return 0;
    frame->value = 1u; /* Private CALLER_OWNED; no public tag authority. */
    frame->previous_frame = match.record->creation_frame;
    frame->previous_generation = match.record->creation_generation;
    frame->previous_target = match.record->creation_target;
    frame->previous_claimed = match.record->creation_claimed;
    frame->generation = ++registry->creation_channel_serial;
    match.record->creation_frame = (uintptr_t)frame;
    match.record->creation_generation = frame->generation;
    match.record->creation_target = (uintptr_t)target;
    match.record->creation_claimed = 0u;
    return 1;
}

int p4_task_registry_creation_end_locked(struct p4_task_registry *registry,
    struct Task *owner, uint32_t hart, uint32_t expected_generation,
    struct p4_creation_channel *frame)
{
    struct p4_task_registry_match match;
    int admitted = p4_task_registry_creation_admit_locked(
        registry, owner, hart, &match);
    if (admitted != 1)
        return admitted;
    /* Non-top, absent and retired frames are opaque: do not read them. */
    if (frame == 0 || match.record->creation_frame != (uintptr_t)frame ||
        expected_generation == 0u ||
        match.record->creation_generation != expected_generation)
        return 0;
    if (p4_task_registry_output_aliases_locked(registry, frame, sizeof(*frame)) ||
        frame->generation != match.record->creation_generation ||
        ((frame->previous_frame == 0u) != (frame->previous_generation == 0u)) ||
        ((frame->previous_frame == 0u) != (frame->previous_target == 0u)) ||
        frame->previous_claimed > 1u ||
        (frame->previous_frame == 0u && frame->previous_claimed != 0u) ||
        frame->previous_generation >= frame->generation ||
        (frame->previous_frame != 0u &&
         (frame->previous_frame % _Alignof(struct p4_creation_channel) != 0u ||
          p4_task_registry_ranges_overlap(frame, sizeof(*frame),
              (const void *)frame->previous_frame, sizeof(*frame)))))
        return -1;
    match.record->creation_frame = frame->previous_frame;
    match.record->creation_generation = frame->previous_generation;
    match.record->creation_target = frame->previous_target;
    match.record->creation_claimed = frame->previous_claimed;
    frame->generation = 0u;
    frame->previous_frame = 0u;
    frame->previous_generation = 0u;
    frame->previous_target = 0u;
    frame->previous_claimed = 0u;
    return 1;
}

uintptr_t p4_task_registry_creation_resolve_locked(
    struct p4_task_registry *registry, struct Task *owner, uint32_t hart,
    struct Task *target, uint32_t generation, uintptr_t tag_value)
{
    struct p4_task_registry_match match;
    uintptr_t address;
    if (p4_task_registry_creation_admit_locked(registry, owner, hart, &match) != 1 ||
        match.record->creation_frame == 0u ||
        match.record->creation_target != (uintptr_t)target ||
        match.record->creation_generation != generation ||
        match.record->creation_claimed != 0u)
        return 0u;
    address = match.record->creation_frame +
              offsetof(struct p4_creation_channel, value);
    if (tag_value != address)
        return 0u;
    match.record->creation_claimed = 1u;
    return address;
}

int p4_task_registry_register_constructing_locked(
    struct p4_task_registry *registry, struct p4_task_registry_record *record,
    struct Task *identity, struct Task *creator, uint32_t hart,
    uint32_t *out_generation)
{
    struct p4_task_registry_match owner;
    if (registry == 0 || record == 0 || identity == 0 || creator == 0 ||
        identity == creator || hart >= P4_TASK_LIFETIME_HARTS ||
        out_generation == 0 ||
        (uintptr_t)record % _Alignof(struct p4_task_registry_record) != 0u ||
        (uintptr_t)out_generation % _Alignof(uint32_t) != 0u ||
        !p4_task_registry_find_locked(registry, creator, 0, &owner) ||
        p4_task_registry_output_aliases_locked(registry, record, sizeof(*record)) ||
        p4_task_registry_output_aliases_locked(registry, out_generation,
            sizeof(*out_generation)) ||
        p4_task_registry_ranges_overlap(record, sizeof(*record),
            out_generation, sizeof(*out_generation)))
        return -1;
    if (owner.record == 0 || owner.record->lifetime.flags != 0u ||
        owner.record->lifetime.running_harts != (UINT32_C(1) << hart) ||
        owner.record->lifetime.notification_pins == UINT32_MAX ||
        owner.record->construction_children >= P4_TASK_REGISTRY_LIMIT ||
        registry->construction_serial == UINT32_MAX)
        return 0;
    /* Fresh registration is the last fallible action. Both designated pins,
     * owner association and generation publish together under this gate. */
    if (!p4_task_registry_register_locked(registry, record, identity))
        return 0;
    record->construction_creator = creator;
    record->construction_generation = ++registry->construction_serial;
    record->lifetime.notification_pins = 1u;
    ++owner.record->construction_children;
    ++owner.record->lifetime.notification_pins;
    *out_generation = record->construction_generation;
    return 1;
}

int p4_task_registry_finish_construction_locked(
    struct p4_task_registry *registry, struct Task *identity,
    struct Task *creator, uint32_t generation, uint32_t hart)
{
    struct p4_task_registry_match target, owner;
    if (registry == 0 || identity == 0 || creator == 0 || identity == creator ||
        hart >= P4_TASK_LIFETIME_HARTS ||
        !p4_task_registry_find_locked(registry, identity, 0, &target) ||
        !p4_task_registry_find_locked(registry, creator, 0, &owner))
        return -1;
    if (generation == 0u || target.record == 0 || owner.record == 0 ||
        target.record->construction_creator != creator ||
        target.record->construction_generation != generation ||
        owner.record->lifetime.flags != 0u ||
        owner.record->lifetime.running_harts != (UINT32_C(1) << hart))
        return 0;
    if (owner.record->construction_children == 0u ||
        owner.record->lifetime.notification_pins == 0u ||
        target.record->lifetime.notification_pins == 0u)
        return -1;
    /* No Task fields or caller pointers are used after release. A retiring
     * target becomes cleanup-eligible only after this constructor boundary.
     * The binding must queue/poke eligible target and creator under its gate. */
    target.record->construction_creator = 0;
    --target.record->lifetime.notification_pins;
    --owner.record->construction_children;
    --owner.record->lifetime.notification_pins;
    return 1;
}
int p4_task_registry_finish_orphan_construction_locked(
    struct p4_task_registry *registry, struct Task *identity,
    struct Task *creator, uint32_t generation)
{
    struct p4_task_registry_match target, owner;
    if (registry == 0 || identity == 0 || creator == 0 || identity == creator ||
        !p4_task_registry_find_locked(registry, identity, 0, &target) ||
        !p4_task_registry_find_locked(registry, creator, 0, &owner))
        return -1;
    if (generation == 0u || target.record == 0 || owner.record == 0 ||
        target.record->construction_creator != creator ||
        target.record->construction_generation != generation ||
        target.record->service_pending != 1u ||
        owner.record->service_pending != 1u ||
        target.record->lifetime.flags != P4_TASK_LIFETIME_RETIRED ||
        owner.record->lifetime.flags != P4_TASK_LIFETIME_RETIRED ||
        target.record->lifetime.running_harts != 0u ||
        owner.record->lifetime.running_harts != 0u)
        return 0;
    if (owner.record->construction_children == 0u ||
        owner.record->lifetime.notification_pins < 2u ||
        target.record->lifetime.notification_pins < 2u)
        return -1;
    /* Both saved continuations are irrevocably retired and RUN-drained.
     * The worker's final Task/queue/topology accesses precede this commit;
     * remaining references, including both service pins, are untouched. */
    target.record->construction_creator = 0;
    --target.record->lifetime.notification_pins;
    --owner.record->construction_children;
    --owner.record->lifetime.notification_pins;
    return 1;
}
#endif

static void p4_task_registry_semaphore_notification_clear(
    struct p4_semaphore_notification *snapshot)
{
    snapshot->identity = 0;
    snapshot->count = 0;
    snapshot->generation = 0;
}

int p4_task_registry_claim_next_semaphore_notification_locked(
    struct p4_task_registry *registry,
    struct p4_semaphore_notification *out_snapshot)
{
    struct p4_task_registry_match match;
    struct p4_task_registry_match selected_match;
    struct p4_task_registry_record *record;
    uint32_t visited = 0;

    if (out_snapshot == 0 ||
        (uintptr_t)out_snapshot %
            _Alignof(struct p4_semaphore_notification) != 0 ||
        !p4_task_registry_find_locked(registry, 0, 0, &match) ||
        p4_task_registry_output_aliases_locked(registry, out_snapshot,
            (uintptr_t)sizeof(*out_snapshot)))
        return -1;

    record = registry->head;
    while (record != 0 && visited < registry->count &&
           visited < P4_TASK_REGISTRY_LIMIT)
    {
        if (record->semaphore_pending != 0 &&
            record->semaphore_inflight == 0)
        {
            uint32_t count = record->semaphore_pending;
            uint32_t generation = record->semaphore_pending_generation;
            struct Task *identity = record->identity;

            /* The validator checks shape; also prove the selected raw
             * identity names exactly this record before publishing a claim. */
            if (!p4_task_registry_find_locked(
                    registry, identity, 0, &selected_match) ||
                selected_match.record != record)
                return -1;

            record->semaphore_pending = 0;
            record->semaphore_pending_generation = 0;
            record->semaphore_inflight = count;
            record->semaphore_inflight_generation = generation;
            out_snapshot->identity = identity;
            out_snapshot->count = count;
            out_snapshot->generation = generation;
            return 1;
        }

        record = record->next;
        ++visited;
    }

    if (record != 0 || visited != registry->count)
        return -1;
    p4_task_registry_semaphore_notification_clear(out_snapshot);
    return 0;
}

int p4_task_registry_finish_semaphore_notification_locked(
    struct p4_task_registry *registry,
    const struct p4_semaphore_notification *snapshot)
{
    struct p4_task_registry_match match;
    struct p4_task_registry_record *record;
    struct Task *identity;
    uint32_t count, generation;

    if (snapshot == 0 || (uintptr_t)snapshot %
            _Alignof(struct p4_semaphore_notification) != 0 ||
        !p4_task_registry_find_locked(registry, 0, 0, &match) ||
        p4_task_registry_output_aliases_locked(registry, snapshot,
            (uintptr_t)sizeof(*snapshot)))
        return -1;

    /* Do not read caller storage until the full registry and complete
     * snapshot range have been proved disjoint from all linked state. */
    identity = snapshot->identity;
    count = snapshot->count;
    generation = snapshot->generation;
    if (identity == 0 || count == 0 || generation == 0)
        return -1;

    if (generation > registry->semaphore_notification_serial)
        return 0;
    if (!p4_task_registry_find_locked(registry, identity, 0, &match))
        return -1;
    record = match.record;
    if (record == 0 || record->semaphore_inflight != count ||
        record->semaphore_inflight_generation != generation)
        return 0;

    /* Registry validation established the complete reserved-pin floor. */
    record->semaphore_inflight = 0;
    record->semaphore_inflight_generation = 0;
    record->lifetime.notification_pins -= count;
    return 1;
}

int p4_task_registry_claim_next_retained_service_locked(
    struct p4_task_registry *registry,
    struct p4_task_registry_record **out_record)
{
    struct p4_task_registry_match match;
    struct p4_task_registry_match selected_match;
    struct p4_task_registry_record *record;
    uint32_t visited = 0;

    if (out_record == 0 ||
        !p4_task_registry_find_locked(registry, 0, 0, &match))
        return -1;

    /* Do not clear the output until it is proven not to alias registry state
     * or a registered record: writing through an alias could corrupt the list. */
    if (p4_task_registry_output_aliases_locked(
            registry, out_record, (uintptr_t)sizeof(*out_record)))
        return -1;
    *out_record = 0;

    record = registry->head;
    while (record != 0 && visited < registry->count &&
           visited < P4_TASK_REGISTRY_LIMIT)
    {
        if (record->service_queued == 1u)
        {
            /* Full-list validation does not impose global O(n^2) identity
             * uniqueness. Recheck the selected identity before mutation. */
            if (!p4_task_registry_find_locked(
                    registry, record->identity, 0, &selected_match) ||
                selected_match.record != record)
                return -1;

            record->service_pending = 0;
            record->service_queued = 0;
            record->taskresource_unobserved = 0u;
            record->lifetime.notification_pins = 0;
            record->lifetime.flags =
                P4_TASK_LIFETIME_RETIRED | P4_TASK_LIFETIME_CLAIMED;
            record->service_inflight = 1u;
            *out_record = record;
            return 1;
        }

        record = record->next;
        ++visited;
    }

    if (record != 0 || visited != registry->count)
        return -1;
    return 0;
}

static int p4_task_registry_retained_finish_match_locked(
    struct p4_task_registry *registry,
    struct p4_task_registry_record *raw_record,
    struct p4_task_registry_match *out_match)
{
    struct p4_task_registry_match match;
    struct p4_task_registry_record *record;
    uint32_t visited = 0;

    if (registry == 0 || raw_record == 0 ||
        !p4_task_registry_find_locked(registry, 0, 0, &match))
        return 0;

    /* Compare the untrusted pointer against validated members before reading
     * any field through it; a caller may pass an unmapped identity value. */
    record = registry->head;
    while (record != 0 && record != raw_record &&
           visited < registry->count && visited < P4_TASK_REGISTRY_LIMIT)
    {
        record = record->next;
        ++visited;
    }
    if (record == 0 || record != raw_record)
        return 0;

    if (!p4_task_registry_find_locked(
            registry, record->identity, 0, &match) ||
        match.record != record || record->service_inflight != 1u ||
        record->lifetime.flags !=
            (P4_TASK_LIFETIME_RETIRED | P4_TASK_LIFETIME_CLAIMED) ||
        record->lifetime.notification_pins != 0 ||
        record->lifetime.running_harts != 0 ||
        record->service_pending != 0 || record->service_queued != 0 ||
        record->worker_protected != 0 ||
        record->wait_scope.active != 0)
        return 0;

    *out_match = match;
    return 1;
}

static void p4_task_registry_retained_finish_commit_locked(
    struct p4_task_registry *registry,
    const struct p4_task_registry_match *match)
{
    struct p4_task_registry_record *record = match->record;
    if (match->previous == 0)
        registry->head = record->next;
    else
        match->previous->next = record->next;
    --registry->count;
    record->next = 0;
    record->service_inflight = 0;
    record->service_postfree = 0;
    record->service_generation = 0;
    record->taskresource_unobserved = 0u;
}

int p4_task_registry_finish_retained_service_locked(
    struct p4_task_registry *registry,
    struct p4_task_registry_record *raw_record)
{
    struct p4_task_registry_match match;
    if (!p4_task_registry_retained_finish_match_locked(
            registry, raw_record, &match))
        return 0;
    p4_task_registry_retained_finish_commit_locked(registry, &match);
    return 1;
}

#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
int p4_task_registry_defer_removal_finish_locked(
    struct p4_task_registry *registry,
    struct p4_task_registry_record *raw_record)
{
    struct p4_task_registry_match match;
    if (!p4_task_registry_retained_finish_match_locked(
            registry, raw_record, &match))
        return 0;
    match.record->service_postfree = 1u;
    return 1;
}

int p4_task_registry_next_postfree_after_locked(
    struct p4_task_registry *registry, uintptr_t after,
    struct p4_task_registry_record **out_record)
{
    struct p4_task_registry_record *record, *selected = 0;
    struct p4_task_registry_match match;
    if (out_record == 0 ||
        (uintptr_t)out_record % _Alignof(struct p4_task_registry_record *) != 0u ||
        !p4_task_registry_find_locked(registry, 0, 0, &match) ||
        p4_task_registry_output_aliases_locked(registry, out_record,
            sizeof(*out_record)))
        return -1;
    for (record = registry->head; record != 0; record = record->next)
        if (record->service_postfree == 1u && (uintptr_t)record > after &&
            (selected == 0 || (uintptr_t)record < (uintptr_t)selected))
            selected = record;
    if (selected != 0 &&
        !p4_task_registry_retained_finish_match_locked(registry, selected, &match))
        return -1;
    *out_record = selected;
    return selected != 0 ? 1 : 0;
}

int p4_task_registry_finish_removal_locked(
    struct p4_task_registry *registry, struct p4_remove_bank *bank,
    struct p4_task_registry_record *raw_record)
{
    struct p4_task_registry_match target_match;
    struct p4_task_registry_match recipient_match;
    struct p4_task_registry_record *recipient;
    uintptr_t target;
    uint32_t service_count;
    uint32_t index;
    uint32_t copies;
    uint32_t notifications = 0u;
    uintptr_t slot_bytes;

    /* Validate the registry before even comparing caller-owned bank spans.
     * The raw target pointer is resolved by membership, never dereferenced
     * speculatively. All target/context/FreeEntry accesses must already end. */
    if (!p4_task_registry_find_locked(registry, 0, 0, &recipient_match))
        return -1;
    if (!p4_task_registry_retained_finish_match_locked(
            registry, raw_record, &target_match))
        return 0;
    if (bank == 0 || (uintptr_t)bank % _Alignof(struct p4_remove_bank) != 0u ||
        sizeof(*bank) > UINTPTR_MAX - (uintptr_t)bank ||
        p4_task_registry_output_aliases_locked(
            registry, bank, sizeof(*bank)))
        return -1;
    /* A disjoint mapped header may now be read, but not its untrusted pool.
     * Reject pool aliases before the bank validator scans typed slot fields. */
    if (bank->capacity == 0u || bank->capacity > P4_REMOVE_BANK_MAX_SLOTS ||
        (uintptr_t)bank->capacity > UINTPTR_MAX / sizeof(*bank->slots) ||
        bank->slots == 0 ||
        (uintptr_t)bank->slots % _Alignof(struct p4_remove_bank_slot) != 0u)
        return -1;
    slot_bytes = (uintptr_t)bank->capacity * sizeof(*bank->slots);
    if (slot_bytes > UINTPTR_MAX - (uintptr_t)bank->slots ||
        p4_task_registry_ranges_overlap(bank, sizeof(*bank),
            bank->slots, slot_bytes) ||
        p4_task_registry_output_aliases_locked(registry, bank->slots, slot_bytes))
        return -1;
    target = (uintptr_t)target_match.record->identity;
    if (p4_remove_bank_count_target(bank, target, &service_count) != 1)
        return -1;

    /* The complete bank is now validated. Check every recipient, including
     * duplicate raw registry identities, before any cross-object mutation.
     * A retired requester must have cancelled its references in this SAME
     * gate transaction; a surviving reference to it is not a benign refusal. */
    for (index = 0u; index < bank->capacity; ++index)
    {
        const struct p4_remove_bank_slot *slot = &bank->slots[index];
        if (slot->active == 0u || slot->target_identity != target ||
            slot->completion.service_ref == 0u ||
            slot->completion.requester_ref == 0u)
            continue;
        if (!p4_task_registry_find_locked(registry,
                (struct Task *)slot->requester_identity, 0, &recipient_match) ||
            recipient_match.record == 0 ||
            recipient_match.record->lifetime.flags != 0u)
            return -1;
        ++notifications;
    }

    /* Aggregate headroom per recipient; multiple removal tickets and an
     * existing semaphore batch share counted SIGF_SINGLE reservations. No
     * request/Task pointer enters that batch. Pending and inflight remain
     * separately generation-bound. Retirement already issued the target's
     * nonwrapping token, even for tickets joining after retirement. */
    for (recipient = registry->head; recipient != 0;
         recipient = recipient->next)
    {
        copies = 0u;
        for (index = 0u; index < bank->capacity; ++index)
        {
            const struct p4_remove_bank_slot *slot = &bank->slots[index];
            if (slot->active != 0u && slot->target_identity == target &&
                slot->completion.service_ref != 0u &&
                slot->completion.requester_ref != 0u &&
                slot->requester_identity == (uintptr_t)recipient->identity)
                ++copies;
        }
        if (copies == 0u)
            continue;
        if (recipient->lifetime.notification_pins > UINT32_MAX - copies ||
            recipient->semaphore_pending > UINT32_MAX - copies)
            return 0;
        if (recipient->semaphore_pending == 0u &&
            recipient->semaphore_inflight_generation ==
                target_match.record->service_generation)
            return -1; /* Issuance uniqueness was violated, not pressure. */
    }

    /* Commit: no callout or fallible helper after the first write. This
     * kernel transaction is the bank's only non-leaf mutator: full bank and
     * cross-object preflight above establishes the same ACK/clear invariants
     * as the leaf without repeating a full-pool validation for every ticket.
     * Gate ownership spans every ACK and target detach, excluding late joins. */
    for (index = 0u; index < bank->capacity; ++index)
    {
        struct p4_remove_bank_slot *slot = &bank->slots[index];
        if (slot->active == 0u || slot->target_identity != target ||
            slot->completion.service_ref == 0u)
            continue;
        if (slot->completion.requester_ref != 0u)
        {
            for (recipient = registry->head;
                 (uintptr_t)recipient->identity != slot->requester_identity;
                 recipient = recipient->next)
                ; /* Exact live membership was proved above. */
            ++recipient->lifetime.notification_pins;
            if (recipient->semaphore_pending == 0u)
                recipient->semaphore_pending_generation =
                    target_match.record->service_generation;
            ++recipient->semaphore_pending;
            slot->completion.service_ref = 0u;
            slot->completion.completed = 1u;
        }
        else
        {
            slot->requester_identity = 0u;
            slot->target_identity = 0u;
            slot->active = 0u;
            slot->completion.service_ref = 0u;
            slot->completion.completed = 0u;
            /* requester_ref already zero; preserve generation against ABA. */
        }
    }
    p4_task_registry_retained_finish_commit_locked(registry, &target_match);
    return (int)notifications + 1;
}
#endif

int p4_task_registry_running_owner_locked(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);
    if (record == 0 || record->lifetime.flags != 0)
        return -1;
    if (record->lifetime.running_harts == 0)
        return 0;
    return record->lifetime.running_harts == 1u ? 1 : 2;
}

int p4_task_registry_service_owner_running_locked(
    struct p4_task_registry *registry, struct Task *identity, uint32_t hart)
{
    struct p4_task_registry_record *record;
    if (hart >= P4_TASK_LIFETIME_HARTS)
        return 0;
    record = p4_task_registry_lookup_locked(registry, identity);
    return record != 0 && record->service_pending == 1u &&
           record->lifetime.notification_pins >= 1u &&
           (record->lifetime.flags & P4_TASK_LIFETIME_RETIRED) != 0 &&
           record->lifetime.running_harts == (UINT32_C(1) << hart);
}

static int p4_task_registry_wait_matches(
    const struct p4_task_registry_record *record, uint32_t generation)
{
    return record != 0 && generation != 0 &&
           record->wait_scope.active == 1u &&
           record->wait_scope.generation == generation;
}

static int p4_task_registry_wait_cancel_eligible(
    const struct p4_task_registry_record *record)
{
    return record != 0 &&
           (record->lifetime.flags & P4_TASK_LIFETIME_RETIRED) != 0 &&
           record->wait_scope.active == 1u &&
           record->wait_scope.cancel_claimed == 0 &&
           (record->wait_scope.queued_count != 0 ||
            record->lifetime.running_harts == 0);
}

static int p4_task_registry_wait_matches_legacy(
    const struct p4_task_registry_record *record, uint32_t generation)
{
    return p4_task_registry_wait_matches(record, generation) &&
           (record->wait_scope.kind == P4_TASK_WAIT_SINGLE ||
            record->wait_scope.kind == P4_TASK_WAIT_LIST);
}

static void p4_task_registry_wait_scope_clear(
    struct p4_task_wait_scope *scope)
{
    uint32_t generation = scope->generation;

    scope->active = 0;
    scope->kind = 0;
    scope->queued_count = 0;
    scope->construction_closed = 0;
    scope->cancel_requested = 0;
    scope->cancel_claimed = 0;
    scope->semaphore_index = 0;
    scope->request_index = 0;
    scope->list_index = 0;
    scope->generation = generation;
}

int p4_task_registry_wait_begin_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t kind,
    const void *semaphore_index,
    const void *request_index,
    const void *list_index,
    uint32_t *out_generation)
{
    struct p4_task_registry_match match;
    struct p4_task_registry_record *record;
    uint32_t generation;

    if (out_generation == 0 || identity == 0 ||
        !p4_task_registry_find_locked(registry, identity, 0, &match) ||
        match.record == 0 ||
        p4_task_registry_output_aliases_locked(
            registry, out_generation, (uintptr_t)sizeof(*out_generation)))
        return 0;

    *out_generation = 0;
    record = match.record;
    if (record->lifetime.flags != 0 || record->service_inflight != 0 ||
        record->wait_scope.active != 0 ||
        (record->lifetime.running_harts != UINT32_C(1) &&
         record->lifetime.running_harts != UINT32_C(2)) ||
        record->wait_scope.generation == UINT32_MAX ||
        record->lifetime.notification_pins == UINT32_MAX)
        return 0;

    if (kind == P4_TASK_WAIT_SINGLE)
    {
        if (semaphore_index == 0 || request_index == 0 || list_index != 0)
            return 0;
    }
    else if (kind == P4_TASK_WAIT_LIST)
    {
        if (semaphore_index != 0 || request_index != 0 || list_index == 0)
            return 0;
    }
    else
        return 0;

    generation = record->wait_scope.generation + 1u;
    if (!p4_task_lifetime_pin_locked(&record->lifetime))
        return 0;

    record->wait_scope.active = 1u;
    record->wait_scope.kind = kind;
    record->wait_scope.queued_count = 0;
    record->wait_scope.construction_closed = 0;
    record->wait_scope.cancel_requested = 0;
    record->wait_scope.cancel_claimed = 0;
    record->wait_scope.semaphore_index = semaphore_index;
    record->wait_scope.request_index = request_index;
    record->wait_scope.list_index = list_index;
    record->wait_scope.generation = generation;
    *out_generation = generation;
    return 1;
}

#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
static int p4_task_registry_ledger_overlap(
    const struct p4_semaphore_ledger *ledger, const void *storage,
    uintptr_t bytes)
{
    return p4_task_registry_ranges_overlap(storage, bytes, ledger,
               (uintptr_t)sizeof(*ledger)) ||
           p4_task_registry_ranges_overlap(storage, bytes, ledger->operations,
               (uintptr_t)ledger->operation_capacity * sizeof(*ledger->operations)) ||
           p4_task_registry_ranges_overlap(storage, bytes, ledger->entries,
               (uintptr_t)ledger->entry_capacity * sizeof(*ledger->entries));
}

int p4_task_registry_ledger_disjoint_locked(struct p4_task_registry *registry,
    const struct p4_semaphore_ledger *ledger)
{
    struct p4_task_registry_record *cursor;
    if (!p4_task_registry_find_locked(registry, 0, 0, 0) || ledger == 0 ||
        (uintptr_t)ledger % _Alignof(struct p4_semaphore_ledger) != 0 ||
        p4_task_registry_output_aliases_locked(registry, ledger,
            (uintptr_t)sizeof(*ledger)))
        return 0;
    if (ledger->operation_capacity == 0 ||
        ledger->operation_capacity > P4_SEMAPHORE_LEDGER_LIMIT ||
        ledger->entry_capacity == 0 ||
        ledger->entry_capacity > P4_SEMAPHORE_LEDGER_LIMIT ||
        p4_task_registry_ledger_overlap(ledger, registry, sizeof(*registry)))
        return 0;
    for (cursor = registry->head; cursor != 0; cursor = cursor->next)
        if (p4_task_registry_ledger_overlap(ledger, cursor, sizeof(*cursor)))
            return 0;
    return 1;
}

int p4_task_registry_wait_reserve_operation_locked(
    struct p4_task_registry *registry, struct p4_semaphore_ledger *ledger,
    struct Task *identity, uint32_t entry_count,
    struct p4_task_wait_operation *output)
{
    struct p4_task_registry_match match;
    struct p4_task_registry_record *record;
    struct p4_task_registry_record *cursor;
    struct p4_semaphore_ledger_operation *operation;
    uint32_t generation;
    int result;

    /* Complete registry admission and storage checks before either state
     * machine changes. Ledger reserve performs its own full validation. */
    if (identity == 0 || output == 0 ||
        (uintptr_t)output % _Alignof(struct p4_task_wait_operation) != 0 ||
        ledger == 0 ||
        (uintptr_t)ledger % _Alignof(struct p4_semaphore_ledger) != 0 ||
        !p4_task_registry_find_locked(registry, identity, 0, &match) ||
        match.record == 0 ||
        p4_task_registry_output_aliases_locked(registry, output,
            (uintptr_t)sizeof(*output)) ||
        p4_task_registry_output_aliases_locked(registry, ledger,
            (uintptr_t)sizeof(*ledger)))
        return 0;

    /* Reject a ledger object overlapping registry storage before reading
     * ledger metadata through its typed pointer. Backing arrays are checked
     * separately below before reserve can mutate them. */
    if (ledger->operation_capacity == 0 ||
        ledger->operation_capacity > P4_SEMAPHORE_LEDGER_LIMIT ||
        ledger->entry_capacity == 0 ||
        ledger->entry_capacity > P4_SEMAPHORE_LEDGER_LIMIT ||
        p4_task_registry_ledger_overlap(ledger, output,
            (uintptr_t)sizeof(*output)) ||
        p4_task_registry_ledger_overlap(ledger, registry,
            (uintptr_t)sizeof(*registry)))
        return 0;

    record = match.record;
    if (record->lifetime.flags != 0 || record->service_inflight != 0 ||
        record->wait_scope.active != 0 ||
        (record->lifetime.running_harts != UINT32_C(1) &&
         record->lifetime.running_harts != UINT32_C(2)) ||
        record->wait_scope.generation == UINT32_MAX ||
        record->lifetime.notification_pins == UINT32_MAX)
        return 0;

    /* The validated list is bounded and stable under the caller's gate. */
    for (cursor = registry->head; cursor != 0; cursor = cursor->next)
        if (p4_task_registry_ledger_overlap(ledger, cursor,
                (uintptr_t)sizeof(*cursor)))
            return 0;

    generation = record->wait_scope.generation + 1u;
    result = p4_semaphore_ledger_reserve(ledger, identity, generation,
        entry_count, &operation);
    if (result != 1)
        return result;

    /* Infallible publication: pin capacity and every registry invariant were
     * checked above. No unlock/callback/Task dereference separates reserve
     * from publishing its independent lifetime scope. */
    ++record->lifetime.notification_pins;
    record->wait_scope.generation = generation;
    record->wait_scope.active = 1u;
    record->wait_scope.kind = P4_TASK_WAIT_OPERATION;
    record->wait_scope.queued_count = 0;
    record->wait_scope.construction_closed = 0;
    record->wait_scope.cancel_requested = 0;
    record->wait_scope.cancel_claimed = 0;
    record->wait_scope.semaphore_index = 0;
    record->wait_scope.request_index = 0;
    record->wait_scope.list_index = operation;
    output->operation = operation;
    output->generation = generation;
    return 1;
}

/* Resolve opaque arena identity only after both storage domains are proved
 * disjoint. entry_at validates the full ledger and membership before any
 * field is read through the operation pointer from the scope. */
int p4_task_registry_operation_resolve_locked(
    struct p4_task_registry *registry, struct p4_semaphore_ledger *ledger,
    struct Task *identity, uint32_t generation,
    struct p4_task_registry_record **out_record,
    struct p4_semaphore_ledger_operation **out_operation)
{
    struct p4_task_registry_record *record;
    struct p4_semaphore_ledger_entry *entry;
    struct p4_semaphore_ledger_operation *operation;
    int result;

    /* Validate header separation before reading arena metadata, then all
     * output ranges before publication. No failed resolution writes state. */
    if (!p4_task_registry_ledger_disjoint_locked(registry, ledger) ||
        out_record == 0 || out_operation == 0 ||
        (uintptr_t)out_record % _Alignof(void *) != 0 ||
        (uintptr_t)out_operation % _Alignof(void *) != 0 ||
        p4_task_registry_ranges_overlap(out_record, sizeof(*out_record),
            out_operation, sizeof(*out_operation)) ||
        p4_task_registry_output_aliases_locked(registry, out_record,
            sizeof(*out_record)) ||
        p4_task_registry_output_aliases_locked(registry, out_operation,
            sizeof(*out_operation)) ||
        p4_task_registry_ledger_overlap(ledger, out_record,
            sizeof(*out_record)) ||
        p4_task_registry_ledger_overlap(ledger, out_operation,
            sizeof(*out_operation)) ||
        !p4_semaphore_ledger_validate(ledger))
        return -1;
    record = p4_task_registry_lookup_locked(registry, identity);
    if (record == 0 ||
        !p4_task_registry_wait_matches(record, generation) ||
        record->wait_scope.kind != P4_TASK_WAIT_OPERATION ||
        record->service_inflight != 0)
        return 0;

    operation = (struct p4_semaphore_ledger_operation *)
        record->wait_scope.list_index;
    result = p4_semaphore_ledger_entry_at(ledger, operation, 0, &entry);
    /* A matching active scope must reference a live member of this arena.
     * An absent/free/wrong-owner operation is corruption, not a stale call. */
    if (result != 1 ||
        entry->operation != operation || operation->owner != identity ||
        operation->generation != generation ||
        operation->construction_closed != record->wait_scope.construction_closed ||
        operation->cancelled != record->wait_scope.cancel_requested)
        return -1;
    *out_record = record;
    *out_operation = operation;
    return 1;
}

int p4_task_registry_retire_with_ledger_locked(
    struct p4_task_registry *registry, struct p4_semaphore_ledger *ledger,
    struct Task *identity, uint32_t for_service)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);
    struct p4_semaphore_ledger_operation *operation;
    int result;

    if (record == 0 || for_service > 1u)
        return 0;
    if (record->wait_scope.kind != P4_TASK_WAIT_OPERATION)
        return for_service != 0 ?
            p4_task_registry_retire_for_service_locked(registry, identity) :
            p4_task_registry_retire_locked(registry, identity);

    result = p4_task_registry_operation_resolve_locked(registry, ledger, identity,
        record->wait_scope.generation, &record, &operation);
    if (result != 1)
        return result;
    if (record->lifetime.flags != 0 ||
        record->worker_protected != 0 || operation->cancelled != 0 ||
        (for_service != 0 && (record->service_pending != 0 ||
         record->lifetime.notification_pins == UINT32_MAX)))
        return 0;
    /* Serial refusal precedes ledger cancellation, which is the last
     * fallible mutator. A failed cancellation consumes no generation. */
    if (for_service != 0 &&
        registry->semaphore_notification_serial == UINT32_MAX)
        return 0;
    result = p4_semaphore_ledger_cancel(ledger, operation);
    if (result != 1)
        return result;
    /* Both validators and service-pin overflow checks precede cancellation.
     * All subsequent retirement publication is infallible under this gate. */
    record->lifetime.flags |= P4_TASK_LIFETIME_RETIRED;
    record->wait_scope.cancel_requested = 1u;
    if (for_service != 0)
    {
        record->service_generation = ++registry->semaphore_notification_serial;
        ++record->lifetime.notification_pins;
        record->service_pending = 1u;
    }
    return 1;
}

int p4_task_registry_admit_removal_locked(
    struct p4_task_registry *registry, struct p4_semaphore_ledger *ledger,
    struct p4_remove_bank *bank, struct Task *requester, struct Task *target,
    uint32_t hart, uint32_t retire_live, struct p4_remove_handle *out_handle)
{
    struct p4_task_registry_match requester_match, target_match;
    struct p4_remove_bank_slot *slot;
    uintptr_t slot_bytes;
    uint64_t reserved;
    uint32_t index, selected, generation, service_count;
    int result;

    /* Validate mapped headers and every cross-domain span before validators
     * follow typed backing arrays. Raw Task identities are never read. */
    if (requester == 0 || target == 0 || requester == target || hart > 1u ||
        retire_live > 1u || out_handle == 0 ||
        (uintptr_t)out_handle % _Alignof(struct p4_remove_handle) != 0u ||
        !p4_task_registry_find_locked(registry, requester, 0, &requester_match) ||
        p4_task_registry_output_aliases_locked(registry, out_handle,
            sizeof(*out_handle)) || bank == 0 ||
        (uintptr_t)bank % _Alignof(struct p4_remove_bank) != 0u ||
        p4_task_registry_output_aliases_locked(registry, bank, sizeof(*bank)) ||
        p4_task_registry_ranges_overlap(bank, sizeof(*bank), out_handle,
            sizeof(*out_handle)) || ledger == 0 ||
        (uintptr_t)ledger % _Alignof(struct p4_semaphore_ledger) != 0u ||
        p4_task_registry_ranges_overlap(ledger, sizeof(*ledger), bank,
            sizeof(*bank)) ||
        p4_task_registry_ranges_overlap(ledger, sizeof(*ledger), out_handle,
            sizeof(*out_handle)) ||
        !p4_task_registry_ledger_disjoint_locked(registry, ledger) ||
        p4_task_registry_ledger_overlap(ledger, bank, sizeof(*bank)) ||
        p4_task_registry_ledger_overlap(ledger, out_handle, sizeof(*out_handle)))
        return -1;
    if (bank->capacity == 0u || bank->capacity > P4_REMOVE_BANK_MAX_SLOTS ||
        (uintptr_t)bank->capacity > UINTPTR_MAX / sizeof(*bank->slots) ||
        bank->slots == 0 ||
        (uintptr_t)bank->slots % _Alignof(struct p4_remove_bank_slot) != 0u)
        return -1;
    slot_bytes = (uintptr_t)bank->capacity * sizeof(*bank->slots);
    if (p4_task_registry_ranges_overlap(bank->slots, slot_bytes,
            bank, sizeof(*bank)) ||
        p4_task_registry_ranges_overlap(bank->slots, slot_bytes,
            out_handle, sizeof(*out_handle)) ||
        p4_task_registry_output_aliases_locked(registry, bank->slots, slot_bytes) ||
        p4_task_registry_ledger_overlap(ledger, bank->slots, slot_bytes) ||
        !p4_semaphore_ledger_validate(ledger) ||
        p4_remove_bank_count_target(bank, (uintptr_t)target, &service_count) != 1)
        return -1;
    if (requester_match.record == 0 ||
        requester_match.record->lifetime.flags != 0u ||
        requester_match.record->worker_protected != 0u ||
        requester_match.record->lifetime.running_harts != (UINT32_C(1) << hart))
        return 0;
    if (!p4_task_registry_find_locked(registry, target, 0, &target_match))
        return -1;
    if (target_match.record == 0 || target_match.record->worker_protected != 0u)
        return 0;
    if (target_match.record->lifetime.flags != 0u)
    {
        if (retire_live != 0u || target_match.record->service_generation == 0u ||
            (target_match.record->service_pending == 0u &&
             target_match.record->service_inflight == 0u))
            return 0;
        result = 2; /* Retained raw-identity join; never dereference Task. */
    }
    else
    {
        reserved = (uint64_t)target_match.record->worker_protected +
                   p4_task_registry_construction_pins(target_match.record) +
                   target_match.record->wait_scope.active +
                   target_match.record->semaphore_pending +
                   target_match.record->semaphore_inflight;
        if (retire_live == 0u ||
            target_match.record->lifetime.notification_pins <= reserved)
            return 0; /* A separate ordinary target pin must precede locks. */
        result = 1;
    }
    if (bank->serial == UINT32_MAX)
        return 0;
    selected = bank->capacity;
    for (index = 0u; index < bank->capacity; ++index)
    {
        slot = &bank->slots[index];
        /* A synchronous normal-Task caller may own only one outstanding
         * requester interest, including an ACK not yet consumed. Reject
         * reentry before any retirement or ticket mutation. */
        if (slot->active != 0u && slot->completion.requester_ref != 0u &&
            slot->requester_identity == (uintptr_t)requester)
            return 0;
        /* Cancellation discarded this handle's requester interest. Its
         * outstanding ticket service ref is not the target registry service
         * pin/retained cleanup claim. No production service consumer retains
         * a bank handle across this gate; final ACK scans the bank afresh.
         * Select an orphan for reuse, but overwrite ONLY after retirement's
         * last fallible step, preserving all state on refusal. */
        if (selected == bank->capacity &&
            (slot->active == 0u || slot->completion.requester_ref == 0u))
            selected = index;
    }
    if (selected == bank->capacity)
        return 0;
    generation = bank->serial + 1u;

    /* Last fallible transition. No bank slot/reference has been published,
     * so failed retirement/cancellation cannot orphan service ownership. */
    if (result == 1)
    {
        int retired = p4_task_registry_retire_with_ledger_locked(
            registry, ledger, target, 1u);
        if (retired != 1)
            return retired;
        /* This target is also a retiring requester. Cancel all its existing
         * requester references in this same gate before cleanup can run.
         * Full bank validation proved these writes cannot fail. */
        for (index = 0u; index < bank->capacity; ++index)
        {
            slot = &bank->slots[index];
            if (slot->active == 0u ||
                slot->requester_identity != (uintptr_t)target ||
                slot->completion.requester_ref == 0u)
                continue;
            slot->completion.requester_ref = 0u;
            if (slot->completion.service_ref == 0u)
            {
                slot->requester_identity = 0u;
                slot->target_identity = 0u;
                slot->active = 0u;
                slot->completion.completed = 0u;
                /* Preserve slot generation against stale handles. */
            }
        }
    }
    /* Infallible publication from the preflighted inactive/cancelled slot.
     * A new nonwrapping generation invalidates any old canceled handle.
     * Neither an
     * allocator, validator nor completion-init call follows retirement. */
    slot = &bank->slots[selected];
    slot->requester_identity = (uintptr_t)requester;
    slot->target_identity = (uintptr_t)target;
    slot->generation = generation;
    slot->completion.requester_ref = 1u;
    slot->completion.service_ref = 1u;
    slot->completion.completed = 0u;
    slot->active = 1u;
    bank->serial = generation;
    out_handle->index = selected;
    out_handle->generation = generation;
    return result;
}

int p4_task_registry_wait_operation_set_entry_locked(
    struct p4_task_registry *registry, struct p4_semaphore_ledger *ledger,
    struct Task *identity, uint32_t generation, uint32_t index,
    const void *semaphore, const void *request)
{
    struct p4_semaphore_ledger_operation *operation;
    struct p4_task_registry_record *record;
    int result = p4_task_registry_operation_resolve_locked(registry, ledger,
        identity, generation, &record, &operation);
    if (result != 1)
        return result;
    if (record->lifetime.flags != 0 ||
        record->wait_scope.cancel_requested != 0 ||
        record->wait_scope.construction_closed != 0)
        return 0;
    return p4_semaphore_ledger_set_entry(ledger, operation, index,
        semaphore, request);
}

int p4_task_registry_wait_operation_close_locked(
    struct p4_task_registry *registry, struct p4_semaphore_ledger *ledger,
    struct Task *identity, uint32_t generation)
{
    struct p4_semaphore_ledger_operation *operation;
    struct p4_task_registry_record *record;
    int result = p4_task_registry_operation_resolve_locked(registry, ledger,
        identity, generation, &record, &operation);
    if (result != 1)
        return result;
    if (record->lifetime.flags != 0 ||
        record->wait_scope.cancel_requested != 0 ||
        record->wait_scope.construction_closed != 0)
        return 0;
    result = p4_semaphore_ledger_close(ledger, operation);
    if (result == 1)
        record->wait_scope.construction_closed = 1u;
    return result;
}

int p4_task_registry_wait_operation_complete_locked(
    struct p4_task_registry *registry, struct p4_semaphore_ledger *ledger,
    struct Task *identity, uint32_t generation)
{
    struct p4_semaphore_ledger_operation *operation;
    struct p4_task_registry_record *record;
    int result = p4_task_registry_operation_resolve_locked(registry, ledger,
        identity, generation, &record, &operation);
    if (result != 1)
        return result;
    if (record->lifetime.flags != 0 ||
        record->wait_scope.construction_closed != 1u ||
        record->wait_scope.cancel_requested != 0 ||
        record->wait_scope.queued_count != 0)
        return 0;
    result = p4_semaphore_ledger_complete(ledger, operation);
    if (result != 1)
        return result;
    /* Registry validation established the designated pin floor. */
    --record->lifetime.notification_pins;
    p4_task_registry_wait_scope_clear(&record->wait_scope);
    return 1;
}

int p4_task_registry_wait_operation_finish_cancel_locked(
    struct p4_task_registry *registry, struct p4_semaphore_ledger *ledger,
    struct Task *identity, uint32_t generation)
{
    struct p4_semaphore_ledger_operation *operation;
    struct p4_task_registry_record *record;
    int result = p4_task_registry_operation_resolve_locked(registry, ledger,
        identity, generation, &record, &operation);
    if (result != 1)
        return result;
    if (
        (record->lifetime.flags & P4_TASK_LIFETIME_RETIRED) == 0 ||
        record->wait_scope.cancel_requested != 1u ||
        record->wait_scope.cancel_claimed != 1u ||
        record->wait_scope.queued_count != 0 ||
        record->lifetime.running_harts != 0 || operation->cancelled != 1u)
        return 0;
    result = p4_semaphore_ledger_reclaim(ledger, operation);
    if (result != 1)
        return result;
    --record->lifetime.notification_pins;
    p4_task_registry_wait_scope_clear(&record->wait_scope);
    return 1;
}

static int p4_task_registry_claim_wait_cancel_with_ledger_locked(
    struct p4_task_registry *registry, struct p4_semaphore_ledger *ledger,
    uintptr_t after_identity, int ordered, int operation_only,
    struct p4_task_wait_cancel_snapshot *snapshot)
{
    struct p4_task_registry_match match;
    struct p4_task_registry_record *cursor;
    if (snapshot == 0 ||
        (uintptr_t)snapshot % _Alignof(struct p4_task_wait_cancel_snapshot) != 0 ||
        ledger == 0 ||
        (uintptr_t)ledger % _Alignof(struct p4_semaphore_ledger) != 0 ||
        !p4_task_registry_find_locked(registry, 0, 0, &match) ||
        p4_task_registry_output_aliases_locked(registry, ledger,
            (uintptr_t)sizeof(*ledger)))
        return -1;
    if (ledger->operation_capacity == 0 ||
        ledger->operation_capacity > P4_SEMAPHORE_LEDGER_LIMIT ||
        ledger->entry_capacity == 0 ||
        ledger->entry_capacity > P4_SEMAPHORE_LEDGER_LIMIT ||
        p4_task_registry_ledger_overlap(ledger, registry,
            (uintptr_t)sizeof(*registry)) ||
        p4_task_registry_ledger_overlap(ledger, snapshot,
            (uintptr_t)sizeof(*snapshot)))
        return -1;
    for (cursor = registry->head; cursor != 0; cursor = cursor->next)
        if (p4_task_registry_ledger_overlap(ledger, cursor,
                (uintptr_t)sizeof(*cursor)))
            return -1;
    if (!p4_semaphore_ledger_validate(ledger))
        return -1;
    if (operation_only != 0)
    {
        struct p4_task_registry_record *selected = 0;
        struct p4_task_registry_record *resolved;
        struct p4_semaphore_ledger_operation *operation;
        struct p4_semaphore_ledger_entry *entry;
        uint32_t queued = 0;
        /* Selection has not mutated anything yet. Independent validators do
         * not prove the cross-domain record->operation link: resolve exactly
         * the candidate that the ordered selector will claim before allowing
         * it to publish cancel_claimed or any caller snapshot field. */
        for (cursor = registry->head; cursor != 0; cursor = cursor->next)
            if (cursor->wait_scope.kind == P4_TASK_WAIT_OPERATION &&
                p4_task_registry_wait_cancel_eligible(cursor) &&
                (uintptr_t)cursor->identity > after_identity &&
                (selected == 0 || (uintptr_t)cursor->identity <
                    (uintptr_t)selected->identity))
                selected = cursor;
        if (selected != 0)
        {
            if (p4_task_registry_operation_resolve_locked(registry, ledger,
                    selected->identity, selected->wait_scope.generation,
                    &resolved, &operation) != 1 || resolved != selected ||
                operation->cancelled != 1u ||
                operation->construction_closed !=
                    selected->wait_scope.construction_closed)
                return -1;
            for (entry = operation->first; entry != 0;
                 entry = entry->operation_next)
                if (entry->state == P4_SEMAPHORE_LEDGER_ENTRY_QUEUED)
                    ++queued;
            if (queued != selected->wait_scope.queued_count)
                return -1;
        }
        return p4_task_registry_claim_wait_operation_cancel_after_locked(
            registry, after_identity, snapshot);
    }
    if (ordered != 0)
        return p4_task_registry_claim_wait_cancel_after_locked(registry,
            after_identity, snapshot);
    return p4_task_registry_claim_next_wait_cancel_locked(registry, snapshot);
}

int p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
    struct p4_task_registry *registry, struct p4_semaphore_ledger *ledger,
    struct p4_task_wait_cancel_snapshot *snapshot)
{
    return p4_task_registry_claim_wait_cancel_with_ledger_locked(registry,
        ledger, 0, 0, 0, snapshot);
}

int p4_task_registry_claim_wait_cancel_after_with_ledger_locked(
    struct p4_task_registry *registry, struct p4_semaphore_ledger *ledger,
    uintptr_t after_identity, struct p4_task_wait_cancel_snapshot *snapshot)
{
    return p4_task_registry_claim_wait_cancel_with_ledger_locked(registry,
        ledger, after_identity, 1, 0, snapshot);
}

int p4_task_registry_claim_wait_operation_cancel_after_with_ledger_locked(
    struct p4_task_registry *registry, struct p4_semaphore_ledger *ledger,
    uintptr_t after_identity, struct p4_task_wait_cancel_snapshot *snapshot)
{
    return p4_task_registry_claim_wait_cancel_with_ledger_locked(registry,
        ledger, after_identity, 1, 1, snapshot);
}
#endif

int p4_task_registry_wait_add_request_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);

    if (record == 0 || record->lifetime.flags != 0 ||
        record->service_inflight != 0 ||
        !p4_task_registry_wait_matches_legacy(record, generation) ||
        record->wait_scope.cancel_requested != 0 ||
        record->wait_scope.construction_closed != 0 ||
        record->wait_scope.queued_count == UINT32_MAX)
        return 0;

    ++record->wait_scope.queued_count;
    return 1;
}

int p4_task_registry_wait_close_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);

    if (record == 0 || record->lifetime.flags != 0 ||
        record->service_inflight != 0 ||
        !p4_task_registry_wait_matches_legacy(record, generation) ||
        record->wait_scope.cancel_requested != 0 ||
        record->wait_scope.construction_closed != 0)
        return 0;

    record->wait_scope.construction_closed = 1u;
    return 1;
}

int p4_task_registry_wait_ready_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);

    return record != 0 && record->lifetime.flags == 0 &&
           record->service_inflight == 0 &&
           p4_task_registry_wait_matches_legacy(record, generation) &&
           record->wait_scope.construction_closed == 1u &&
           record->wait_scope.queued_count == 0 &&
           record->wait_scope.cancel_requested == 0;
}

int p4_task_registry_wait_grant_request_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);

    if (record == 0 || record->lifetime.flags != 0 ||
        record->service_inflight != 0 ||
        !p4_task_registry_wait_matches_legacy(record, generation) ||
        record->wait_scope.cancel_requested != 0 ||
        record->wait_scope.queued_count == 0 ||
        record->lifetime.notification_pins == UINT32_MAX)
        return 0;

    /* A distinct pin retains Task/stack storage across the later Signal. */
    if (!p4_task_lifetime_pin_locked(&record->lifetime))
        return 0;
    --record->wait_scope.queued_count;
    return 1;
}

int p4_task_registry_wait_withdraw_request_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);

    if (record == 0 ||
        !p4_task_registry_wait_matches_legacy(record, generation) ||
        record->wait_scope.queued_count == 0 ||
        (record->wait_scope.cancel_requested == 0 &&
         (record->lifetime.flags & P4_TASK_LIFETIME_RETIRED) == 0))
        return 0;

    --record->wait_scope.queued_count;
    return 1;
}

int p4_task_registry_wait_finish_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);
    uint64_t other_pins;

    if (record == 0 || record->wait_scope.kind == P4_TASK_WAIT_OPERATION ||
        record->lifetime.flags != 0 ||
        record->service_inflight != 0 ||
        !p4_task_registry_wait_matches(record, generation) ||
        record->wait_scope.construction_closed != 1u ||
        record->wait_scope.queued_count != 0 ||
        record->wait_scope.cancel_requested != 0)
        return 0;

    other_pins = (uint64_t)record->service_pending +
                 p4_task_registry_construction_pins(record) +
                 record->worker_protected + record->semaphore_pending +
                 record->semaphore_inflight;
    if (record->lifetime.notification_pins <= other_pins)
        return 0;

    --record->lifetime.notification_pins;
    p4_task_registry_wait_scope_clear(&record->wait_scope);
    return 1;
}

int p4_task_registry_wait_cancel_ready_locked(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);

    if (record == 0)
        return -1;

    return p4_task_registry_wait_cancel_eligible(record);
}

static void p4_task_registry_wait_snapshot_clear(
    struct p4_task_wait_cancel_snapshot *snapshot)
{
    snapshot->identity = 0;
    snapshot->generation = 0;
    snapshot->kind = 0;
    snapshot->queued_count = 0;
    snapshot->construction_closed = 0;
    snapshot->cancel_requested = 0;
    snapshot->semaphore_index = 0;
    snapshot->request_index = 0;
    snapshot->list_index = 0;
}

static int p4_task_registry_claim_wait_cancel_locked(
    struct p4_task_registry *registry,
    uintptr_t after_identity, int ordered, int operation_only,
    struct p4_task_wait_cancel_snapshot *out_snapshot)
{
    struct p4_task_registry_match match;
    struct p4_task_registry_match selected_match;
    struct p4_task_registry_record *record;
    struct p4_task_registry_record *selected = 0;
    uint32_t visited = 0;

    if (out_snapshot == 0 ||
        !p4_task_registry_find_locked(registry, 0, 0, &match) ||
        p4_task_registry_output_aliases_locked(
            registry, out_snapshot, (uintptr_t)sizeof(*out_snapshot)))
        return -1;

    record = registry->head;
    while (record != 0 && visited < registry->count &&
           visited < P4_TASK_REGISTRY_LIMIT)
    {
        if (p4_task_registry_wait_cancel_eligible(record) &&
            (operation_only == 0 ||
                record->wait_scope.kind == P4_TASK_WAIT_OPERATION) &&
            (ordered == 0 || ((uintptr_t)record->identity > after_identity &&
                (selected == 0 || (uintptr_t)record->identity <
                    (uintptr_t)selected->identity))))
        {
            if (!p4_task_registry_find_locked(
                    registry, record->identity, 0, &selected_match) ||
                selected_match.record != record)
                return -1;
            selected = record;
            if (ordered == 0)
                break;
        }
        record = record->next;
        ++visited;
    }

    if (record != 0 && (ordered != 0 || selected == 0))
        return -1;
    if ((ordered != 0 || selected == 0) && visited != registry->count)
        return -1;

    if (selected == 0)
    {
        p4_task_registry_wait_snapshot_clear(out_snapshot);
        return 0;
    }

    /* Caller-owned snapshot is distinct from all registry storage. Publish
     * it only after full validation and selected-identity uniqueness checks. */
    out_snapshot->identity = selected->identity;
    out_snapshot->generation = selected->wait_scope.generation;
    out_snapshot->kind = selected->wait_scope.kind;
    out_snapshot->queued_count = selected->wait_scope.queued_count;
    out_snapshot->construction_closed =
        selected->wait_scope.construction_closed;
    out_snapshot->cancel_requested = 1u;
    out_snapshot->semaphore_index = selected->wait_scope.semaphore_index;
    out_snapshot->request_index = selected->wait_scope.request_index;
    out_snapshot->list_index = selected->wait_scope.list_index;
    selected->wait_scope.cancel_requested = 1u;
    selected->wait_scope.cancel_claimed = 1u;
    return 1;
}

int p4_task_registry_claim_next_wait_cancel_locked(
    struct p4_task_registry *registry,
    struct p4_task_wait_cancel_snapshot *out_snapshot)
{
    return p4_task_registry_claim_wait_cancel_locked(registry, 0, 0, 0,
        out_snapshot);
}

int p4_task_registry_claim_wait_cancel_after_locked(
    struct p4_task_registry *registry, uintptr_t after_identity,
    struct p4_task_wait_cancel_snapshot *out_snapshot)
{
    return p4_task_registry_claim_wait_cancel_locked(registry, after_identity,
        1, 0, out_snapshot);
}

int p4_task_registry_claim_wait_operation_cancel_after_locked(
    struct p4_task_registry *registry, uintptr_t after_identity,
    struct p4_task_wait_cancel_snapshot *out_snapshot)
{
    return p4_task_registry_claim_wait_cancel_locked(registry, after_identity,
        1, 1, out_snapshot);
}

int p4_task_registry_wait_finish_cancel_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);
    uint64_t other_pins;

    if (record == 0 || record->wait_scope.kind == P4_TASK_WAIT_OPERATION ||
        record->service_inflight != 0 ||
        !p4_task_registry_wait_matches(record, generation) ||
        (record->lifetime.flags & P4_TASK_LIFETIME_RETIRED) == 0 ||
        record->wait_scope.cancel_requested != 1u ||
        record->wait_scope.cancel_claimed != 1u ||
        record->wait_scope.queued_count != 0 ||
        record->lifetime.running_harts != 0)
        return 0;

    other_pins = (uint64_t)record->service_pending +
                 p4_task_registry_construction_pins(record) +
                 record->worker_protected + record->semaphore_pending +
                 record->semaphore_inflight;
    if (record->lifetime.notification_pins <= other_pins)
        return 0;

    --record->lifetime.notification_pins;
    p4_task_registry_wait_scope_clear(&record->wait_scope);
    return 1;
}

int p4_task_registry_wait_release_cancel_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation)
{
    struct p4_task_registry_record *record =
        p4_task_registry_lookup_locked(registry, identity);

    if (record == 0 || record->service_inflight != 0 ||
        !p4_task_registry_wait_matches(record, generation) ||
        (record->lifetime.flags & P4_TASK_LIFETIME_RETIRED) == 0 ||
        record->wait_scope.cancel_requested != 1u ||
        record->wait_scope.cancel_claimed != 1u)
        return 0;

    /* Request remains sticky; a later scan retries only when work remains or
     * the last RUN owner has drained. */
    record->wait_scope.cancel_claimed = 0;
    return 1;
}
