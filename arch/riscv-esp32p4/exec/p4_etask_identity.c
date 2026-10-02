/* Bounded ETask identity/PID registry leaf. The topology gate is external. */
#include "p4_etask_identity.h"

#define P4_ETASK_IDENTITY_WRAP 1024u

static int registry_shape_valid(
    const struct p4_etask_identity_registry *registry,
    const void *identity_to_reserve)
{
    const struct p4_etask_identity_record *record;
    uint32_t previous_pid = 0;
    uint32_t index;

    if (registry == 0 || registry->count > P4_ETASK_IDENTITY_LIMIT)
        return 0;
    if (registry->count == 0)
        return registry->head == 0;
    if (registry->head == 0)
        return 0;

    record = registry->head;
    for (index = 0; index < registry->count; ++index)
    {
        if (record == 0 || record->identity == 0 || record->pid == 0 ||
            record->pid <= previous_pid)
            return 0;
        if (identity_to_reserve != 0 &&
            record->identity == identity_to_reserve)
            return 0;
        previous_pid = record->pid;
        record = record->next;
    }

    /* Exact count and a NULL terminator reject both short lists and cycles. */
    return record == 0;
}

static uint32_t first_gap_in_range(
    const struct p4_etask_identity_registry *registry,
    uint32_t lower,
    uint32_t upper)
{
    const struct p4_etask_identity_record *record = registry->head;
    uint32_t candidate = lower;
    uint32_t index;

    if (lower == 0 || lower > upper)
        return 0;

    for (index = 0; index < registry->count; ++index)
    {
        uint32_t pid = record->pid;

        if (pid < lower)
        {
            record = record->next;
            continue;
        }
        if (pid > upper)
            break;
        if (pid > candidate)
            return candidate;
        if (pid == candidate)
        {
            if (candidate == upper)
                return 0;
            ++candidate;
        }
        record = record->next;
    }

    return candidate <= upper ? candidate : 0;
}

static uint32_t first_available_pid(
    const struct p4_etask_identity_registry *registry,
    uint32_t current_counter)
{
    uint32_t first = current_counter + 1u;
    uint32_t candidate;

    /* Preserve Exec's legacy zero-wrap transition to PID 1024. */
    if (first == 0)
        first = P4_ETASK_IDENTITY_WRAP;

    candidate = first_gap_in_range(registry, first, UINT32_MAX);
    if (candidate != 0)
        return candidate;

    /* After wrapping, legacy allocation resumes at 1024. Values below 1024
     * are not revisited by this counter cycle. */
    if (first > P4_ETASK_IDENTITY_WRAP)
        return first_gap_in_range(registry, P4_ETASK_IDENTITY_WRAP,
                                  first - 1u);
    return 0;
}

static int record_is_zero(const struct p4_etask_identity_record *record)
{
    return record != 0 && record->next == 0 && record->identity == 0 &&
           record->pid == 0;
}

int p4_etask_identity_registry_init(
    struct p4_etask_identity_registry *registry)
{
    if (registry == 0 || registry->head != 0 || registry->count != 0)
        return 0;
    registry->head = 0;
    registry->count = 0;
    return 1;
}

uint32_t p4_etask_identity_reserve(
    struct p4_etask_identity_registry *registry,
    struct p4_etask_identity_record *record,
    const void *identity,
    uint32_t current_counter)
{
    struct p4_etask_identity_record *previous = 0;
    struct p4_etask_identity_record *cursor;
    uint32_t pid;
    uint32_t index;

    if (identity == 0 || !record_is_zero(record) ||
        !registry_shape_valid(registry, identity) ||
        registry->count >= P4_ETASK_IDENTITY_LIMIT)
        return 0;

    pid = first_available_pid(registry, current_counter);
    if (pid == 0)
        return 0;

    cursor = registry->head;
    for (index = 0; index < registry->count && cursor != 0; ++index)
    {
        if (cursor->pid > pid)
            break;
        previous = cursor;
        cursor = cursor->next;
    }

    /* Shape validation plus first-gap selection guarantees this insertion
     * point is unique and preserves strict PID order. */
    if (cursor != 0 && cursor->pid == pid)
        return 0;

    record->identity = identity;
    record->pid = pid;
    record->next = cursor;
    if (previous != 0)
        previous->next = record;
    else
        registry->head = record;
    ++registry->count;
    return pid;
}

struct p4_etask_identity_record *p4_etask_identity_lookup(
    const struct p4_etask_identity_registry *registry,
    const void *identity)
{
    struct p4_etask_identity_record *record;
    struct p4_etask_identity_record *found = 0;
    uint32_t index;

    if (identity == 0 || !registry_shape_valid(registry, 0))
        return 0;

    record = registry->head;
    for (index = 0; index < registry->count; ++index)
    {
        if (record->identity == identity)
        {
            /* Duplicate identities indicate external registry corruption. */
            if (found != 0)
                return 0;
            found = record;
        }
        record = record->next;
    }
    return found;
}

struct p4_etask_identity_record *p4_etask_identity_release(
    struct p4_etask_identity_registry *registry,
    const void *identity)
{
    struct p4_etask_identity_record *previous = 0;
    struct p4_etask_identity_record *record;
    struct p4_etask_identity_record *found =
        p4_etask_identity_lookup(registry, identity);
    uint32_t index;

    if (found == 0)
        return 0;

    /* Locate predecessor in a second bounded pass; all fallible validation
     * and identity uniqueness checks are complete before the first mutation. */
    record = registry->head;
    for (index = 0; index < registry->count && record != found; ++index)
    {
        previous = record;
        record = record->next;
    }
    if (record != found)
        return 0;

    if (previous != 0)
        previous->next = found->next;
    else
        registry->head = found->next;
    --registry->count;
    found->next = 0;
    return found;
}
