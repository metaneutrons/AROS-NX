/* Fixed-storage transactional accounting for P4 SMP semaphore requests.
 * The caller serializes every entrypoint with the shared registry gate. */
#include <stddef.h>
#include <stdint.h>

#include "p4_semaphore_ledger.h"

#define P4_SEMAPHORE_LEDGER_MAGIC UINT32_C(0x5034534c)

static int p4_semaphore_ledger_ranges_overlap(
    uintptr_t first, uintptr_t first_size,
    uintptr_t second, uintptr_t second_size)
{
    if (first <= second)
        return second - first < first_size;
    return first - second < second_size;
}

static int p4_semaphore_ledger_array_range(
    const void *array, uint32_t count, size_t item_size,
    uintptr_t *start, uintptr_t *size)
{
    uintptr_t address;
    uintptr_t bytes;

    if (array == NULL || count == 0 || item_size == 0 ||
        (uintptr_t)array % (uintptr_t)sizeof(void *) != 0 ||
        (uintptr_t)count > UINTPTR_MAX / (uintptr_t)item_size)
        return 0;

    address = (uintptr_t)array;
    bytes = (uintptr_t)count * (uintptr_t)item_size;
    if (address > UINTPTR_MAX - bytes)
        return 0;

    *start = address;
    *size = bytes;
    return 1;
}

static int p4_semaphore_ledger_op_pointer(
    const struct p4_semaphore_ledger *ledger,
    const struct p4_semaphore_ledger_operation *operation,
    uint32_t *index)
{
    uintptr_t base = (uintptr_t)ledger->operations;
    uintptr_t address = (uintptr_t)operation;
    uintptr_t offset;
    uintptr_t bytes = (uintptr_t)ledger->operation_capacity *
                      (uintptr_t)sizeof(*ledger->operations);

    if (operation == NULL || address < base)
        return 0;
    offset = address - base;
    if (offset >= bytes || offset % (uintptr_t)sizeof(*ledger->operations) != 0)
        return 0;
    *index = (uint32_t)(offset / (uintptr_t)sizeof(*ledger->operations));
    return 1;
}

static int p4_semaphore_ledger_entry_pointer(
    const struct p4_semaphore_ledger *ledger,
    const struct p4_semaphore_ledger_entry *entry,
    uint32_t *index)
{
    uintptr_t base = (uintptr_t)ledger->entries;
    uintptr_t address = (uintptr_t)entry;
    uintptr_t offset;
    uintptr_t bytes = (uintptr_t)ledger->entry_capacity *
                      (uintptr_t)sizeof(*ledger->entries);

    if (entry == NULL || address < base)
        return 0;
    offset = address - base;
    if (offset >= bytes || offset % (uintptr_t)sizeof(*ledger->entries) != 0)
        return 0;
    *index = (uint32_t)(offset / (uintptr_t)sizeof(*ledger->entries));
    return 1;
}

static int p4_semaphore_ledger_key_before(
    const void *left_owner, uint32_t left_generation,
    const void *right_owner, uint32_t right_generation)
{
    uintptr_t left = (uintptr_t)left_owner;
    uintptr_t right = (uintptr_t)right_owner;

    return left < right || (left == right && left_generation < right_generation);
}

static int p4_semaphore_ledger_op_zero(
    const struct p4_semaphore_ledger_operation *operation)
{
    return operation->active_next == NULL && operation->first == NULL &&
           operation->owner == NULL && operation->generation == 0 &&
           operation->entry_count == 0 && operation->construction_closed == 0 &&
           operation->committed == 0 && operation->cancelled == 0 &&
           operation->active == 0;
}

static int p4_semaphore_ledger_entry_zero(
    const struct p4_semaphore_ledger_entry *entry)
{
    return entry->operation == NULL && entry->operation_next == NULL &&
           entry->request_next == NULL && entry->semaphore == NULL &&
           entry->request == NULL && entry->state == P4_SEMAPHORE_LEDGER_ENTRY_FREE;
}

static void p4_semaphore_ledger_clear_operation(
    struct p4_semaphore_ledger_operation *operation)
{
    operation->active_next = NULL;
    operation->first = NULL;
    operation->owner = NULL;
    operation->generation = 0;
    operation->entry_count = 0;
    operation->construction_closed = 0;
    operation->committed = 0;
    operation->cancelled = 0;
    operation->active = 0;
}

static void p4_semaphore_ledger_clear_entry(
    struct p4_semaphore_ledger_entry *entry)
{
    entry->operation = NULL;
    entry->operation_next = NULL;
    entry->request_next = NULL;
    entry->semaphore = NULL;
    entry->request = NULL;
    entry->state = P4_SEMAPHORE_LEDGER_ENTRY_FREE;
}

static int p4_semaphore_ledger_valid(
    const struct p4_semaphore_ledger *ledger)
{
    uintptr_t ledger_start, ledger_size;
    uintptr_t operations_start, operations_size;
    uintptr_t entries_start, entries_size;
    struct p4_semaphore_ledger_operation *operation;
    struct p4_semaphore_ledger_entry *entry;
    uint32_t operation_slots = 0;
    uint32_t entry_slots = 0;
    uint32_t assigned_entries = 0;
    uint32_t listed_entries = 0;
    uint32_t listed_operations = 0;
    uint32_t listed_requests = 0;
    uint32_t i;
    int have_previous_key = 0;
    const void *previous_owner = NULL;
    uint32_t previous_generation = 0;
    uintptr_t previous_request = 0;

    if (ledger == NULL ||
        (uintptr_t)ledger % (uintptr_t)sizeof(void *) != 0 ||
        ledger->initialized != P4_SEMAPHORE_LEDGER_MAGIC ||
        ledger->operation_capacity == 0 ||
        ledger->operation_capacity > P4_SEMAPHORE_LEDGER_LIMIT ||
        ledger->entry_capacity == 0 ||
        ledger->entry_capacity > P4_SEMAPHORE_LEDGER_LIMIT ||
        ledger->active_operations > ledger->operation_capacity ||
        ledger->active_entries > ledger->entry_capacity ||
        ledger->request_count > ledger->active_entries ||
        !p4_semaphore_ledger_array_range(
            ledger->operations, ledger->operation_capacity,
            sizeof(*ledger->operations), &operations_start, &operations_size) ||
        !p4_semaphore_ledger_array_range(
            ledger->entries, ledger->entry_capacity,
            sizeof(*ledger->entries), &entries_start, &entries_size) ||
        !p4_semaphore_ledger_array_range(
            ledger, 1, sizeof(*ledger), &ledger_start, &ledger_size) ||
        p4_semaphore_ledger_ranges_overlap(
            ledger_start, ledger_size, operations_start, operations_size) ||
        p4_semaphore_ledger_ranges_overlap(
            ledger_start, ledger_size, entries_start, entries_size) ||
        p4_semaphore_ledger_ranges_overlap(
            operations_start, operations_size, entries_start, entries_size))
        return 0;

    /* The array scan validates every slot, including unused slots. */
    for (i = 0; i < ledger->operation_capacity; ++i)
    {
        operation = &ledger->operations[i];
        if (operation->active == 0)
        {
            if (!p4_semaphore_ledger_op_zero(operation))
                return 0;
            continue;
        }
        if (operation->active != 1u || operation->owner == NULL ||
            operation->generation == 0 || operation->entry_count == 0 ||
            operation->entry_count > ledger->entry_capacity ||
            operation->construction_closed > 1u || operation->committed > 1u ||
            operation->cancelled > 1u || operation->first == NULL ||
            (operation->committed != 0 &&
             operation->construction_closed == 0))
            return 0;
        ++operation_slots;
    }
    if (operation_slots != ledger->active_operations)
        return 0;

    operation = ledger->operations_head;
    while (operation != NULL && listed_operations < ledger->active_operations &&
           listed_operations < ledger->operation_capacity)
    {
        uint32_t operation_index;
        uint32_t traversed = 0;

        if (!p4_semaphore_ledger_op_pointer(ledger, operation, &operation_index) ||
            operation->active != 1u ||
            (have_previous_key != 0 &&
             !p4_semaphore_ledger_key_before(
                 previous_owner, previous_generation,
                 operation->owner, operation->generation)))
            return 0;
        (void)operation_index;
        have_previous_key = 1;
        previous_owner = operation->owner;
        previous_generation = operation->generation;

        entry = operation->first;
        while (entry != NULL && traversed < operation->entry_count &&
               traversed < ledger->entry_capacity)
        {
            uint32_t entry_index;
            int configured;

            if (!p4_semaphore_ledger_entry_pointer(ledger, entry, &entry_index) ||
                entry->operation != operation ||
                entry->state < P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED ||
                entry->state > P4_SEMAPHORE_LEDGER_ENTRY_WITHDRAWN)
                return 0;
            (void)entry_index;
            configured = entry->semaphore != NULL && entry->request != NULL;
            if ((entry->semaphore == NULL) != (entry->request == NULL) ||
                (entry->state != P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED &&
                 configured == 0) ||
                (operation->construction_closed != 0 && configured == 0) ||
                (entry->request == NULL && entry->request_next != NULL) ||
                (operation->committed != 0 &&
                 entry->state != P4_SEMAPHORE_LEDGER_ENTRY_OWNED))
                return 0;
            if (configured != 0)
                ++assigned_entries;
            ++listed_entries;
            ++traversed;
            entry = entry->operation_next;
        }
        if (entry != NULL || traversed != operation->entry_count)
            return 0;
        operation = operation->active_next;
        ++listed_operations;
    }
    if (operation != NULL || listed_operations != ledger->active_operations ||
        listed_entries != ledger->active_entries)
        return 0;

    for (i = 0; i < ledger->entry_capacity; ++i)
    {
        uint32_t operation_index;
        entry = &ledger->entries[i];
        if (entry->state == P4_SEMAPHORE_LEDGER_ENTRY_FREE)
        {
            if (!p4_semaphore_ledger_entry_zero(entry))
                return 0;
            continue;
        }
        if (entry->state < P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED ||
            entry->state > P4_SEMAPHORE_LEDGER_ENTRY_WITHDRAWN ||
            !p4_semaphore_ledger_op_pointer(
                ledger, entry->operation, &operation_index) ||
            ledger->operations[operation_index].active != 1u ||
            ((entry->semaphore == NULL) != (entry->request == NULL)) ||
            ((entry->state != P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED) &&
             entry->semaphore == NULL))
            return 0;
        ++entry_slots;
    }
    if (entry_slots != ledger->active_entries ||
        assigned_entries != ledger->request_count)
        return 0;

    entry = ledger->requests_head;
    while (entry != NULL && listed_requests < ledger->request_count &&
           listed_requests < ledger->entry_capacity)
    {
        uint32_t entry_index;
        uint32_t operation_index;
        uintptr_t request_value;

        if (!p4_semaphore_ledger_entry_pointer(ledger, entry, &entry_index) ||
            entry->state == P4_SEMAPHORE_LEDGER_ENTRY_FREE ||
            entry->semaphore == NULL || entry->request == NULL ||
            !p4_semaphore_ledger_op_pointer(
                ledger, entry->operation, &operation_index) ||
            ledger->operations[operation_index].active != 1u)
            return 0;
        (void)entry_index;
        request_value = (uintptr_t)entry->request;
        if (listed_requests != 0 && request_value <= previous_request)
            return 0;
        previous_request = request_value;
        ++listed_requests;
        entry = entry->request_next;
    }
    return entry == NULL && listed_requests == ledger->request_count;
}

int p4_semaphore_ledger_validate(const struct p4_semaphore_ledger *ledger)
{
    return p4_semaphore_ledger_valid(ledger);
}

static int p4_semaphore_ledger_output_disjoint(
    const struct p4_semaphore_ledger *ledger,
    const void *output,
    size_t output_size,
    size_t output_alignment)
{
    uintptr_t ledger_start = (uintptr_t)ledger;
    uintptr_t operation_start = (uintptr_t)ledger->operations;
    uintptr_t entry_start = (uintptr_t)ledger->entries;
    uintptr_t ledger_size = (uintptr_t)sizeof(*ledger);
    uintptr_t operation_size = (uintptr_t)ledger->operation_capacity *
        (uintptr_t)sizeof(*ledger->operations);
    uintptr_t entry_size = (uintptr_t)ledger->entry_capacity *
        (uintptr_t)sizeof(*ledger->entries);
    uintptr_t output_start = (uintptr_t)output;
    uintptr_t bytes = (uintptr_t)output_size;

    if (output == NULL || bytes == 0 ||
        output_alignment == 0 ||
        output_start % (uintptr_t)output_alignment != 0 ||
        output_start > UINTPTR_MAX - bytes)
        return 0;
    return !p4_semaphore_ledger_ranges_overlap(
               output_start, bytes, ledger_start, ledger_size) &&
           !p4_semaphore_ledger_ranges_overlap(
               output_start, bytes, operation_start, operation_size) &&
           !p4_semaphore_ledger_ranges_overlap(
               output_start, bytes, entry_start, entry_size);
}

static struct p4_semaphore_ledger_operation *p4_semaphore_ledger_find_operation(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *raw_operation)
{
    uint32_t index;

    if (!p4_semaphore_ledger_op_pointer(ledger, raw_operation, &index) ||
        ledger->operations[index].active != 1u)
        return NULL;
    return &ledger->operations[index];
}

static struct p4_semaphore_ledger_entry *p4_semaphore_ledger_find_entry(
    struct p4_semaphore_ledger_operation *operation,
    uint32_t index)
{
    struct p4_semaphore_ledger_entry *entry = operation->first;

    if (index >= operation->entry_count)
        return NULL;
    while (index != 0)
    {
        entry = entry->operation_next;
        --index;
    }
    return entry;
}

static int p4_semaphore_ledger_operation_is_active(
    const struct p4_semaphore_ledger_operation *operation)
{
    return operation != NULL && operation->active == 1u;
}

int p4_semaphore_ledger_init(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operations,
    uint32_t operation_capacity,
    struct p4_semaphore_ledger_entry *entries,
    uint32_t entry_capacity)
{
    uintptr_t ledger_start, ledger_size;
    uintptr_t operations_start, operations_size;
    uintptr_t entries_start, entries_size;
    uint32_t i;

    if (ledger == NULL || operations == NULL || entries == NULL ||
        operation_capacity == 0 ||
        operation_capacity > P4_SEMAPHORE_LEDGER_LIMIT ||
        entry_capacity == 0 || entry_capacity > P4_SEMAPHORE_LEDGER_LIMIT ||
        (uintptr_t)ledger % (uintptr_t)sizeof(void *) != 0 ||
        !p4_semaphore_ledger_array_range(
            ledger, 1, sizeof(*ledger), &ledger_start, &ledger_size) ||
        !p4_semaphore_ledger_array_range(
            operations, operation_capacity, sizeof(*operations),
            &operations_start, &operations_size) ||
        !p4_semaphore_ledger_array_range(
            entries, entry_capacity, sizeof(*entries),
            &entries_start, &entries_size) ||
        p4_semaphore_ledger_ranges_overlap(
            ledger_start, ledger_size, operations_start, operations_size) ||
        p4_semaphore_ledger_ranges_overlap(
            ledger_start, ledger_size, entries_start, entries_size) ||
        p4_semaphore_ledger_ranges_overlap(
            operations_start, operations_size, entries_start, entries_size))
        return -1;

    if (ledger->operations_head != NULL || ledger->requests_head != NULL ||
        ledger->operations != NULL || ledger->entries != NULL ||
        ledger->operation_capacity != 0 || ledger->entry_capacity != 0 ||
        ledger->active_operations != 0 || ledger->active_entries != 0 ||
        ledger->request_count != 0 || ledger->initialized != 0)
        return 0;
    for (i = 0; i < operation_capacity; ++i)
        if (!p4_semaphore_ledger_op_zero(&operations[i]))
            return 0;
    for (i = 0; i < entry_capacity; ++i)
        if (!p4_semaphore_ledger_entry_zero(&entries[i]))
            return 0;

    ledger->operations_head = NULL;
    ledger->requests_head = NULL;
    ledger->operations = operations;
    ledger->entries = entries;
    ledger->operation_capacity = operation_capacity;
    ledger->entry_capacity = entry_capacity;
    ledger->active_operations = 0;
    ledger->active_entries = 0;
    ledger->request_count = 0;
    ledger->initialized = P4_SEMAPHORE_LEDGER_MAGIC;
    return 1;
}

int p4_semaphore_ledger_reserve(
    struct p4_semaphore_ledger *ledger,
    const void *owner,
    uint32_t generation,
    uint32_t entry_count,
    struct p4_semaphore_ledger_operation **out_operation)
{
    struct p4_semaphore_ledger_operation *operation;
    struct p4_semaphore_ledger_operation *op_previous = NULL;
    struct p4_semaphore_ledger_operation *op_next;
    struct p4_semaphore_ledger_entry *entry;
    struct p4_semaphore_ledger_entry *first = NULL;
    struct p4_semaphore_ledger_entry *last = NULL;
    uint32_t free_operation = UINT32_MAX;
    uint32_t free_entries = 0;
    uint32_t requested_entries = entry_count;
    uint32_t i;

    if (!p4_semaphore_ledger_valid(ledger))
        return -1;
    if (!p4_semaphore_ledger_output_disjoint(
            ledger, out_operation, sizeof(*out_operation),
            sizeof(void *)) ||
        owner == NULL || generation == 0 || entry_count == 0)
        return -1;
    if (entry_count > ledger->entry_capacity ||
        ledger->active_operations == ledger->operation_capacity ||
        ledger->active_entries > ledger->entry_capacity - entry_count)
        return 0;

    for (i = 0; i < ledger->operation_capacity; ++i)
        if (ledger->operations[i].active == 0)
        {
            free_operation = i;
            break;
        }
    if (free_operation == UINT32_MAX)
        return 0;

    for (i = 0; i < ledger->entry_capacity; ++i)
        if (ledger->entries[i].state == P4_SEMAPHORE_LEDGER_ENTRY_FREE)
            ++free_entries;
    if (free_entries < entry_count)
        return 0;

    op_next = ledger->operations_head;
    while (op_next != NULL &&
           p4_semaphore_ledger_key_before(
               op_next->owner, op_next->generation, owner, generation))
    {
        op_previous = op_next;
        op_next = op_next->active_next;
    }
    if (op_next != NULL && op_next->owner == owner &&
        op_next->generation == generation)
        return 0;

    operation = &ledger->operations[free_operation];
    for (i = 0; i < ledger->entry_capacity && entry_count != 0; ++i)
    {
        entry = &ledger->entries[i];
        if (entry->state != P4_SEMAPHORE_LEDGER_ENTRY_FREE)
            continue;
        entry->operation = operation;
        entry->operation_next = NULL;
        entry->request_next = NULL;
        entry->semaphore = NULL;
        entry->request = NULL;
        entry->state = P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED;
        if (last == NULL)
            first = entry;
        else
            last->operation_next = entry;
        last = entry;
        --entry_count;
    }

    operation->active_next = op_next;
    operation->first = first;
    operation->owner = owner;
    operation->generation = generation;
    operation->entry_count = requested_entries;
    operation->construction_closed = 0;
    operation->committed = 0;
    operation->cancelled = 0;
    operation->active = 1;
    if (op_previous == NULL)
        ledger->operations_head = operation;
    else
        op_previous->active_next = operation;
    ++ledger->active_operations;
    ledger->active_entries += operation->entry_count;
    *out_operation = operation;
    return 1;
}

int p4_semaphore_ledger_set_entry(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *raw_operation,
    uint32_t index,
    const void *semaphore,
    const void *request)
{
    struct p4_semaphore_ledger_operation *operation;
    struct p4_semaphore_ledger_entry *entry;
    struct p4_semaphore_ledger_entry *previous = NULL;
    struct p4_semaphore_ledger_entry *current;
    uintptr_t request_value;

    if (!p4_semaphore_ledger_valid(ledger))
        return -1;
    if (semaphore == NULL || request == NULL)
        return -1;
    operation = p4_semaphore_ledger_find_operation(ledger, raw_operation);
    if (!p4_semaphore_ledger_operation_is_active(operation))
        return 0;
    if (operation->construction_closed != 0 || operation->committed != 0 ||
        operation->cancelled != 0 || index >= operation->entry_count)
        return 0;
    entry = p4_semaphore_ledger_find_entry(operation, index);
    if (entry == NULL || entry->state != P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED ||
        entry->semaphore != NULL || entry->request != NULL)
        return 0;

    request_value = (uintptr_t)request;
    current = ledger->requests_head;
    while (current != NULL &&
           (uintptr_t)current->request < request_value)
    {
        previous = current;
        current = current->request_next;
    }
    if (current != NULL && (uintptr_t)current->request == request_value)
        return 0;

    entry->semaphore = semaphore;
    entry->request = request;
    entry->request_next = current;
    if (previous == NULL)
        ledger->requests_head = entry;
    else
        previous->request_next = entry;
    ++ledger->request_count;
    return 1;
}

int p4_semaphore_ledger_close(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *raw_operation)
{
    struct p4_semaphore_ledger_operation *operation;
    struct p4_semaphore_ledger_entry *entry;
    uint32_t visited = 0;

    if (!p4_semaphore_ledger_valid(ledger))
        return -1;
    operation = p4_semaphore_ledger_find_operation(ledger, raw_operation);
    if (!p4_semaphore_ledger_operation_is_active(operation) ||
        operation->construction_closed != 0 || operation->cancelled != 0 ||
        operation->committed != 0)
        return 0;

    entry = operation->first;
    while (entry != NULL && visited < operation->entry_count)
    {
        if (entry->semaphore == NULL || entry->request == NULL ||
            entry->state != P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED)
            return 0;
        entry = entry->operation_next;
        ++visited;
    }
    if (entry != NULL || visited != operation->entry_count)
        return -1;
    operation->construction_closed = 1;
    return 1;
}

int p4_semaphore_ledger_entry_at(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *raw_operation,
    uint32_t index,
    struct p4_semaphore_ledger_entry **out_entry)
{
    struct p4_semaphore_ledger_operation *operation;
    struct p4_semaphore_ledger_entry *entry;

    if (!p4_semaphore_ledger_valid(ledger))
        return -1;
    if (!p4_semaphore_ledger_output_disjoint(
            ledger, out_entry, sizeof(*out_entry), sizeof(void *)))
        return -1;
    operation = p4_semaphore_ledger_find_operation(ledger, raw_operation);
    if (!p4_semaphore_ledger_operation_is_active(operation) ||
        index >= operation->entry_count)
        return 0;
    entry = p4_semaphore_ledger_find_entry(operation, index);
    if (entry == NULL)
        return -1;
    *out_entry = entry;
    return 1;
}

enum p4_semaphore_ledger_transition
{
    P4_SEMAPHORE_LEDGER_TRANSITION_ACQUIRE,
    P4_SEMAPHORE_LEDGER_TRANSITION_QUEUE,
    P4_SEMAPHORE_LEDGER_TRANSITION_GRANT,
    P4_SEMAPHORE_LEDGER_TRANSITION_WITHDRAW,
    P4_SEMAPHORE_LEDGER_TRANSITION_RESTART,
    P4_SEMAPHORE_LEDGER_TRANSITION_ROLLBACK
};

static int p4_semaphore_ledger_transition_entry(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *raw_operation,
    uint32_t index,
    enum p4_semaphore_ledger_transition transition)
{
    struct p4_semaphore_ledger_operation *operation;
    struct p4_semaphore_ledger_entry *entry;
    uint32_t expected;
    uint32_t target;

    if (!p4_semaphore_ledger_valid(ledger))
        return -1;
    operation = p4_semaphore_ledger_find_operation(ledger, raw_operation);
    if (!p4_semaphore_ledger_operation_is_active(operation) ||
        index >= operation->entry_count)
        return 0;
    if (operation->construction_closed == 0 || operation->committed != 0)
        return 0;
    if (transition != P4_SEMAPHORE_LEDGER_TRANSITION_ROLLBACK &&
        transition != P4_SEMAPHORE_LEDGER_TRANSITION_WITHDRAW &&
        operation->cancelled != 0)
        return 0;
    if (transition == P4_SEMAPHORE_LEDGER_TRANSITION_WITHDRAW &&
        operation->cancelled == 0)
        return 0;
    if (transition == P4_SEMAPHORE_LEDGER_TRANSITION_ROLLBACK &&
        operation->cancelled == 0)
        return 0;

    switch (transition)
    {
        case P4_SEMAPHORE_LEDGER_TRANSITION_ACQUIRE:
            expected = P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED;
            target = P4_SEMAPHORE_LEDGER_ENTRY_OWNED;
            break;
        case P4_SEMAPHORE_LEDGER_TRANSITION_QUEUE:
            expected = P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED;
            target = P4_SEMAPHORE_LEDGER_ENTRY_QUEUED;
            break;
        case P4_SEMAPHORE_LEDGER_TRANSITION_GRANT:
            expected = P4_SEMAPHORE_LEDGER_ENTRY_QUEUED;
            target = P4_SEMAPHORE_LEDGER_ENTRY_OWNED;
            break;
        case P4_SEMAPHORE_LEDGER_TRANSITION_WITHDRAW:
            expected = P4_SEMAPHORE_LEDGER_ENTRY_QUEUED;
            target = P4_SEMAPHORE_LEDGER_ENTRY_WITHDRAWN;
            break;
        case P4_SEMAPHORE_LEDGER_TRANSITION_RESTART:
            expected = P4_SEMAPHORE_LEDGER_ENTRY_RELEASED;
            target = P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED;
            break;
        case P4_SEMAPHORE_LEDGER_TRANSITION_ROLLBACK:
            expected = P4_SEMAPHORE_LEDGER_ENTRY_OWNED;
            target = P4_SEMAPHORE_LEDGER_ENTRY_RELEASED;
            break;
        default:
            return -1;
    }

    entry = p4_semaphore_ledger_find_entry(operation, index);
    if (entry == NULL)
        return -1;
    if (entry->state != expected)
        return 0;
    entry->state = target;
    return 1;
}

int p4_semaphore_ledger_acquire(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation,
    uint32_t index)
{
    return p4_semaphore_ledger_transition_entry(
        ledger, operation, index, P4_SEMAPHORE_LEDGER_TRANSITION_ACQUIRE);
}

int p4_semaphore_ledger_queue(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation,
    uint32_t index)
{
    return p4_semaphore_ledger_transition_entry(
        ledger, operation, index, P4_SEMAPHORE_LEDGER_TRANSITION_QUEUE);
}

int p4_semaphore_ledger_grant(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation,
    uint32_t index)
{
    return p4_semaphore_ledger_transition_entry(
        ledger, operation, index, P4_SEMAPHORE_LEDGER_TRANSITION_GRANT);
}

int p4_semaphore_ledger_withdraw(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation,
    uint32_t index)
{
    return p4_semaphore_ledger_transition_entry(
        ledger, operation, index, P4_SEMAPHORE_LEDGER_TRANSITION_WITHDRAW);
}

int p4_semaphore_ledger_restart_released(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation,
    uint32_t index)
{
    return p4_semaphore_ledger_transition_entry(
        ledger, operation, index, P4_SEMAPHORE_LEDGER_TRANSITION_RESTART);
}

int p4_semaphore_ledger_commit(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *raw_operation)
{
    struct p4_semaphore_ledger_operation *operation;
    struct p4_semaphore_ledger_entry *entry;
    uint32_t visited = 0;

    if (!p4_semaphore_ledger_valid(ledger))
        return -1;
    operation = p4_semaphore_ledger_find_operation(ledger, raw_operation);
    if (!p4_semaphore_ledger_operation_is_active(operation) ||
        operation->construction_closed == 0 || operation->cancelled != 0 ||
        operation->committed != 0)
        return 0;

    entry = operation->first;
    while (entry != NULL && visited < operation->entry_count)
    {
        if (entry->state != P4_SEMAPHORE_LEDGER_ENTRY_OWNED)
            return 0;
        entry = entry->operation_next;
        ++visited;
    }
    if (entry != NULL || visited != operation->entry_count)
        return -1;
    operation->committed = 1;
    return 1;
}

int p4_semaphore_ledger_cancel(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *raw_operation)
{
    struct p4_semaphore_ledger_operation *operation;

    if (!p4_semaphore_ledger_valid(ledger))
        return -1;
    operation = p4_semaphore_ledger_find_operation(ledger, raw_operation);
    if (!p4_semaphore_ledger_operation_is_active(operation) ||
        operation->cancelled != 0)
        return 0;
    operation->cancelled = 1;
    return 1;
}

int p4_semaphore_ledger_rollback_owned(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation,
    uint32_t index)
{
    return p4_semaphore_ledger_transition_entry(
        ledger, operation, index, P4_SEMAPHORE_LEDGER_TRANSITION_ROLLBACK);
}

int p4_semaphore_ledger_release(
    struct p4_semaphore_ledger *ledger,
    const void *semaphore,
    uint32_t total_nest,
    uint32_t *out_selected_pending)
{
    struct p4_semaphore_ledger_operation *operation;
    struct p4_semaphore_ledger_entry *entry;
    uint32_t pending = 0;

    if (!p4_semaphore_ledger_valid(ledger))
        return -1;
    if (semaphore == NULL ||
        !p4_semaphore_ledger_output_disjoint(
            ledger, out_selected_pending, sizeof(*out_selected_pending),
            sizeof(uint32_t)))
        return -1;

    operation = ledger->operations_head;
    while (operation != NULL)
    {
        entry = operation->first;
        while (entry != NULL)
        {
            if (operation->committed == 0 &&
                entry->state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED &&
                entry->semaphore == semaphore)
                ++pending;
            entry = entry->operation_next;
        }
        operation = operation->active_next;
    }

    if (total_nest < pending || (total_nest == 0 && pending == 0))
        return -1;
    if (total_nest > pending)
    {
        *out_selected_pending = 0;
        return 1;
    }
    if (pending == 0)
        return -1;

    operation = ledger->operations_head;
    while (operation != NULL)
    {
        entry = operation->first;
        while (entry != NULL)
        {
            if (operation->committed == 0 &&
                entry->state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED &&
                entry->semaphore == semaphore)
            {
                entry->state = P4_SEMAPHORE_LEDGER_ENTRY_RELEASED;
                *out_selected_pending = 1;
                return 1;
            }
            entry = entry->operation_next;
        }
        operation = operation->active_next;
    }
    return -1;
}

/* Locate an operation and validate all references/counts needed by the
 * infallible detach pass. The caller holds the shared lifetime gate. */
static int p4_semaphore_ledger_detach_preflight(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *raw_operation,
    struct p4_semaphore_ledger_operation **out_operation,
    struct p4_semaphore_ledger_operation **out_previous,
    uint32_t *out_owned_entries,
    int *out_has_owned_or_queued)
{
    struct p4_semaphore_ledger_operation *operation;
    struct p4_semaphore_ledger_operation *op_previous = NULL;
    struct p4_semaphore_ledger_operation *op_current;
    struct p4_semaphore_ledger_entry *entry;
    struct p4_semaphore_ledger_entry *request_current;
    uint32_t operation_index;
    uint32_t visited = 0;
    uint32_t expected_requests = 0;
    uint32_t matching_requests = 0;
    uint32_t owned_entries = 0;
    int has_owned_or_queued = 0;

    if (!p4_semaphore_ledger_valid(ledger))
        return -1;
    if (!p4_semaphore_ledger_op_pointer(ledger, raw_operation, &operation_index))
        return 0;
    operation = &ledger->operations[operation_index];
    if (operation->active != 1u)
        return 0;

    entry = operation->first;
    while (entry != NULL && visited < operation->entry_count)
    {
        if (entry->state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED)
        {
            ++owned_entries;
            has_owned_or_queued = 1;
        }
        else if (entry->state == P4_SEMAPHORE_LEDGER_ENTRY_QUEUED)
            has_owned_or_queued = 1;
        if (entry->request != NULL)
            ++expected_requests;
        entry = entry->operation_next;
        ++visited;
    }
    if (entry != NULL || visited != operation->entry_count)
        return -1;

    op_current = ledger->operations_head;
    while (op_current != NULL && op_current != operation)
    {
        op_previous = op_current;
        op_current = op_current->active_next;
    }
    if (op_current != operation)
        return -1;

    request_current = ledger->requests_head;
    visited = 0;
    while (request_current != NULL && visited < ledger->request_count)
    {
        if (request_current->operation == operation)
            ++matching_requests;
        request_current = request_current->request_next;
        ++visited;
    }
    if (request_current != NULL || visited != ledger->request_count ||
        matching_requests != expected_requests)
        return -1;

    *out_operation = operation;
    *out_previous = op_previous;
    *out_owned_entries = owned_entries;
    *out_has_owned_or_queued = has_owned_or_queued;
    return 1;
}

/* Preflight must have validated both lists and their counts while the caller
 * still holds the shared lifetime gate. This helper has no failure path. */
static void p4_semaphore_ledger_detach_and_clear(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation,
    struct p4_semaphore_ledger_operation *op_previous)
{
    struct p4_semaphore_ledger_entry *entry;
    struct p4_semaphore_ledger_entry *next_entry;
    struct p4_semaphore_ledger_entry *request_previous = NULL;
    struct p4_semaphore_ledger_entry *request_current = ledger->requests_head;

    /* Remove all request-index references first. The validated counts and
     * links make this pass infallible while the caller holds the gate. */
    while (request_current != NULL)
    {
        struct p4_semaphore_ledger_entry *request_next =
            request_current->request_next;
        if (request_current->operation == operation)
        {
            if (request_previous == NULL)
                ledger->requests_head = request_next;
            else
                request_previous->request_next = request_next;
            request_current->request_next = NULL;
            --ledger->request_count;
        }
        else
            request_previous = request_current;
        request_current = request_next;
    }

    ledger->active_entries -= operation->entry_count;
    ledger->active_operations -= 1u;
    if (op_previous == NULL)
        ledger->operations_head = operation->active_next;
    else
        op_previous->active_next = operation->active_next;

    entry = operation->first;
    while (entry != NULL)
    {
        next_entry = entry->operation_next;
        p4_semaphore_ledger_clear_entry(entry);
        entry = next_entry;
    }
    p4_semaphore_ledger_clear_operation(operation);
}

int p4_semaphore_ledger_complete(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *raw_operation)
{
    struct p4_semaphore_ledger_operation *operation;
    struct p4_semaphore_ledger_operation *op_previous;
    uint32_t owned_entries;
    int has_owned_or_queued;
    int result = p4_semaphore_ledger_detach_preflight(
        ledger, raw_operation, &operation, &op_previous,
        &owned_entries, &has_owned_or_queued);

    (void)has_owned_or_queued;
    if (result != 1)
        return result;
    if (operation->construction_closed == 0 || operation->committed != 0 ||
        operation->cancelled != 0 || owned_entries != operation->entry_count)
        return 0;

    p4_semaphore_ledger_detach_and_clear(ledger, operation, op_previous);
    return 1;
}

int p4_semaphore_ledger_reclaim(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *raw_operation)
{
    struct p4_semaphore_ledger_operation *operation;
    struct p4_semaphore_ledger_operation *op_previous;
    uint32_t owned_entries;
    int has_owned_or_queued;
    int result = p4_semaphore_ledger_detach_preflight(
        ledger, raw_operation, &operation, &op_previous,
        &owned_entries, &has_owned_or_queued);

    (void)owned_entries;
    if (result != 1)
        return result;
    if (operation->committed == 0 &&
        (operation->cancelled == 0 || has_owned_or_queued != 0))
        return 0;

    p4_semaphore_ledger_detach_and_clear(ledger, operation, op_previous);
    return 1;
}
