/* Portable, Task-independent storage for external removal tickets.
 * This module deliberately does not allocate, call out, or dereference the
 * opaque requester/target identities.  Callers serialize every operation
 * with the registry/completion gate and keep the supplied bank storage mapped
 * for its complete lifetime. */
#include <stddef.h>
#include <stdint.h>

#include "p4_remove_bank.h"

#define P4_REMOVE_BANK_MAGIC UINT32_C(0x50445242)

struct p4_remove_bank_view
{
    struct p4_remove_bank_slot *slots;
    uint32_t capacity;
    uint32_t serial;
};

struct p4_remove_span
{
    uintptr_t begin;
    uintptr_t end;
};

static int p4_remove_make_span(const void *object, size_t size,
                               size_t alignment,
                               struct p4_remove_span *span)
{
    uintptr_t begin;
    uintptr_t length;

    if (object == NULL || span == NULL || size == 0u || alignment == 0u)
        return 0;

    begin = (uintptr_t)object;
    if ((begin % (uintptr_t)alignment) != 0u)
        return 0;

    length = (uintptr_t)size;
    if ((size_t)length != size || begin > UINTPTR_MAX - length)
        return 0;

    span->begin = begin;
    span->end = begin + length;
    return 1;
}

static int p4_remove_spans_overlap(const struct p4_remove_span *left,
                                  const struct p4_remove_span *right)
{
    return left->begin < right->end && right->begin < left->end;
}

static int p4_remove_completion_zero(
    const struct p4_remove_completion *completion)
{
    return completion->requester_ref == 0u &&
           completion->service_ref == 0u && completion->completed == 0u;
}

static int p4_remove_slot_zero(const struct p4_remove_bank_slot *slot)
{
    return slot->requester_identity == (uintptr_t)0 &&
           slot->target_identity == (uintptr_t)0 && slot->generation == 0u &&
           slot->active == 0u &&
           p4_remove_completion_zero(&slot->completion);
}

static int p4_remove_bank_view_init(
    const struct p4_remove_bank *bank,
    const void *output_a, size_t output_a_size, size_t output_a_alignment,
    const void *output_b, size_t output_b_size, size_t output_b_alignment,
    struct p4_remove_bank_view *view)
{
    struct p4_remove_span bank_span;
    struct p4_remove_span pool_span;
    struct p4_remove_span output_a_span = {0, 0};
    struct p4_remove_span output_b_span = {0, 0};
    struct p4_remove_bank_slot *slots;
    uint32_t capacity;
    uint32_t serial;
    uint32_t index;
    size_t pool_size;

    if (view == NULL ||
        !p4_remove_make_span(bank, sizeof(*bank),
                             _Alignof(struct p4_remove_bank), &bank_span))
        return 0;

    if (output_a != NULL)
    {
        if (!p4_remove_make_span(output_a, output_a_size,
                                 output_a_alignment, &output_a_span) ||
            p4_remove_spans_overlap(&bank_span, &output_a_span))
            return 0;
    }

    if (output_b != NULL)
    {
        if (!p4_remove_make_span(output_b, output_b_size,
                                 output_b_alignment, &output_b_span) ||
            p4_remove_spans_overlap(&bank_span, &output_b_span) ||
            (output_a != NULL &&
             p4_remove_spans_overlap(&output_a_span, &output_b_span)))
            return 0;
    }

    /* The header is not read until its range and all caller outputs have been
     * separated.  The slot pointer and capacity are untrusted until checked. */
    if (bank->magic != P4_REMOVE_BANK_MAGIC || bank->capacity == 0u ||
        bank->capacity > P4_REMOVE_BANK_MAX_SLOTS)
        return 0;

    capacity = bank->capacity;
    serial = bank->serial;
    slots = bank->slots;
    if ((size_t)capacity > SIZE_MAX / sizeof(*slots))
        return 0;
    pool_size = (size_t)capacity * sizeof(*slots);

    if (!p4_remove_make_span(slots, pool_size,
                             _Alignof(struct p4_remove_bank_slot),
                             &pool_span) ||
        p4_remove_spans_overlap(&bank_span, &pool_span) ||
        (output_a != NULL &&
         p4_remove_spans_overlap(&pool_span, &output_a_span)) ||
        (output_b != NULL &&
         p4_remove_spans_overlap(&pool_span, &output_b_span)))
        return 0;

    /* Validate the complete bounded pool before any operation mutates it. */
    for (index = 0u; index < capacity; ++index)
    {
        const struct p4_remove_bank_slot *slot = &slots[index];

        if (slot->generation > serial || slot->active > 1u)
            return 0;

        if (slot->active == 0u)
        {
            if (slot->requester_identity != (uintptr_t)0 ||
                slot->target_identity != (uintptr_t)0 ||
                !p4_remove_completion_zero(&slot->completion))
                return 0;
        }
        else
        {
            const struct p4_remove_completion *completion =
                &slot->completion;

            if (slot->generation == 0u ||
                slot->requester_identity == (uintptr_t)0 ||
                slot->target_identity == (uintptr_t)0 ||
                completion->requester_ref > 1u ||
                completion->service_ref > 1u || completion->completed > 1u ||
                completion->service_ref != 1u - completion->completed ||
                (completion->requester_ref == 0u &&
                 completion->service_ref == 0u))
                return 0;
        }
    }

    view->slots = slots;
    view->capacity = capacity;
    view->serial = serial;
    return 1;
}

static int p4_remove_handle_matches(const struct p4_remove_bank_view *view,
                                    struct p4_remove_handle handle,
                                    const struct p4_remove_bank_slot **slot)
{
    const struct p4_remove_bank_slot *candidate;

    if (handle.index >= view->capacity || handle.generation == 0u)
        return 0;

    candidate = &view->slots[handle.index];
    if (candidate->active == 0u ||
        candidate->generation != handle.generation)
        return 0;

    *slot = candidate;
    return 1;
}

static void p4_remove_bank_clear_slot(struct p4_remove_bank_slot *slot)
{
    uint32_t generation = slot->generation;

    slot->requester_identity = (uintptr_t)0;
    slot->target_identity = (uintptr_t)0;
    slot->active = 0u;
    slot->completion.requester_ref = 0u;
    slot->completion.service_ref = 0u;
    slot->completion.completed = 0u;
    slot->generation = generation;
}

int p4_remove_bank_init(struct p4_remove_bank *bank,
                        struct p4_remove_bank_slot *slots,
                        uint32_t capacity)
{
    struct p4_remove_span bank_span;
    struct p4_remove_span pool_span;
    uint32_t index;
    size_t pool_size;

    if (capacity == 0u || capacity > P4_REMOVE_BANK_MAX_SLOTS ||
        !p4_remove_make_span(bank, sizeof(*bank),
                             _Alignof(struct p4_remove_bank), &bank_span) ||
        (size_t)capacity > SIZE_MAX / sizeof(*slots))
        return -1;

    pool_size = (size_t)capacity * sizeof(*slots);
    if (!p4_remove_make_span(slots, pool_size,
                             _Alignof(struct p4_remove_bank_slot),
                             &pool_span) ||
        p4_remove_spans_overlap(&bank_span, &pool_span))
        return -1;

    /* Initialization is allowed only on zero-initialized, unpublished
     * caller storage. Check every typed field before publishing the header. */
    if (bank->magic != 0u || bank->capacity != 0u || bank->serial != 0u ||
        bank->slots != NULL)
        return -1;

    for (index = 0u; index < capacity; ++index)
    {
        if (!p4_remove_slot_zero(&slots[index]))
            return -1;
    }

    bank->slots = slots;
    bank->capacity = capacity;
    bank->serial = 0u;
    bank->magic = P4_REMOVE_BANK_MAGIC;
    return 1;
}

int p4_remove_bank_reserve(struct p4_remove_bank *bank,
                           uintptr_t requester_identity,
                           uintptr_t target_identity,
                           struct p4_remove_handle *out_handle)
{
    struct p4_remove_bank_view view;
    struct p4_remove_bank_slot *slot;
    struct p4_remove_handle result;
    uint32_t index;

    if (requester_identity == (uintptr_t)0 ||
        target_identity == (uintptr_t)0 || out_handle == NULL)
        return -1;

    if (!p4_remove_bank_view_init(
            bank, out_handle, sizeof(*out_handle),
            _Alignof(struct p4_remove_handle), NULL, 0u, 0u, &view))
        return -1;

    if (view.serial == UINT32_MAX)
        return 0;

    for (index = 0u; index < view.capacity; ++index)
    {
        if (view.slots[index].active == 0u)
            break;
    }
    if (index == view.capacity)
        return 0;

    slot = &view.slots[index];
    if (!p4_remove_completion_init(&slot->completion))
        return -1;

    result.index = index;
    result.generation = view.serial + 1u;
    slot->requester_identity = requester_identity;
    slot->target_identity = target_identity;
    slot->generation = result.generation;
    slot->active = 1u;
    bank->serial = result.generation;
    *out_handle = result;
    return 1;
}

int p4_remove_bank_ready(const struct p4_remove_bank *bank,
                         struct p4_remove_handle handle,
                         uintptr_t expected_requester,
                         int *out_ready)
{
    struct p4_remove_bank_view view;
    const struct p4_remove_bank_slot *slot;

    if (expected_requester == (uintptr_t)0 || out_ready == NULL)
        return -1;
    if (!p4_remove_bank_view_init(
            bank, out_ready, sizeof(*out_ready), _Alignof(int), NULL, 0u, 0u,
            &view))
        return -1;
    if (!p4_remove_handle_matches(&view, handle, &slot) ||
        slot->requester_identity != expected_requester ||
        slot->completion.requester_ref == 0u)
        return 0;

    *out_ready = p4_remove_completion_ready(&slot->completion);
    return 1;
}

int p4_remove_bank_drop(struct p4_remove_bank *bank,
                        struct p4_remove_handle handle,
                        uintptr_t expected_requester,
                        int cancel,
                        int *out_reclaim)
{
    struct p4_remove_bank_view view;
    const struct p4_remove_bank_slot *candidate;
    struct p4_remove_bank_slot *slot;
    int reclaim;

    if (expected_requester == (uintptr_t)0 || out_reclaim == NULL ||
        (cancel != 0 && cancel != 1))
        return -1;
    if (!p4_remove_bank_view_init(
            bank, out_reclaim, sizeof(*out_reclaim), _Alignof(int), NULL, 0u,
            0u, &view))
        return -1;
    if (!p4_remove_handle_matches(&view, handle, &candidate) ||
        candidate->requester_identity != expected_requester ||
        candidate->completion.requester_ref == 0u)
        return 0;

    slot = &view.slots[handle.index];
    if (!p4_remove_completion_drop(&slot->completion, cancel, &reclaim))
        return 0;
    if (reclaim != 0)
        p4_remove_bank_clear_slot(slot);
    *out_reclaim = reclaim;
    return 1;
}

int p4_remove_bank_cancel_requester(struct p4_remove_bank *bank,
                                   uintptr_t requester_identity,
                                   uint32_t *out_dropped)
{
    struct p4_remove_bank_view view;
    uint32_t index, dropped = 0u;

    if (requester_identity == (uintptr_t)0 || out_dropped == NULL ||
        !p4_remove_bank_view_init(bank, out_dropped, sizeof(*out_dropped),
            _Alignof(uint32_t), NULL, 0u, 0u, &view))
        return -1;

    /* The validated pool establishes every drop precondition. Direct field
     * updates below cannot fail after the first ticket has changed. Keep the
     * completed/service equivalence and clear only fully unreferenced slots. */
    for (index = 0u; index < view.capacity; ++index)
    {
        struct p4_remove_bank_slot *slot = &view.slots[index];
        if (slot->active != 0u &&
            slot->requester_identity == requester_identity &&
            slot->completion.requester_ref != 0u)
        {
            slot->completion.requester_ref = 0u;
            if (slot->completion.service_ref == 0u)
                p4_remove_bank_clear_slot(slot);
            ++dropped;
        }
    }
    *out_dropped = dropped;
    return 1;
}

int p4_remove_bank_ack(struct p4_remove_bank *bank,
                       struct p4_remove_handle handle,
                       uintptr_t expected_target,
                       uintptr_t *out_notify_requester,
                       int *out_reclaim)
{
    struct p4_remove_bank_view view;
    const struct p4_remove_bank_slot *candidate;
    struct p4_remove_bank_slot *slot;
    uintptr_t notify_requester;
    int notify;
    int reclaim;

    if (expected_target == (uintptr_t)0 || out_notify_requester == NULL ||
        out_reclaim == NULL)
        return -1;
    if (!p4_remove_bank_view_init(
            bank, out_notify_requester, sizeof(*out_notify_requester),
            _Alignof(uintptr_t), out_reclaim, sizeof(*out_reclaim),
            _Alignof(int), &view))
        return -1;
    if (!p4_remove_handle_matches(&view, handle, &candidate) ||
        candidate->target_identity != expected_target ||
        candidate->completion.service_ref == 0u)
        return 0;

    slot = &view.slots[handle.index];
    if (!p4_remove_completion_ack(&slot->completion, &notify, &reclaim))
        return 0;

    if (notify != 0)
        notify_requester = slot->requester_identity;
    else
        notify_requester = (uintptr_t)0;

    if (reclaim != 0)
        p4_remove_bank_clear_slot(slot);
    *out_notify_requester = notify_requester;
    *out_reclaim = reclaim;
    return 1;
}

int p4_remove_bank_count_target(const struct p4_remove_bank *bank,
                                uintptr_t target_identity,
                                uint32_t *out_count)
{
    struct p4_remove_bank_view view;
    uint32_t index;
    uint32_t count = 0u;

    if (target_identity == (uintptr_t)0 || out_count == NULL)
        return -1;
    if (!p4_remove_bank_view_init(
            bank, out_count, sizeof(*out_count), _Alignof(uint32_t), NULL, 0u,
            0u, &view))
        return -1;

    for (index = 0u; index < view.capacity; ++index)
    {
        const struct p4_remove_bank_slot *slot = &view.slots[index];
        if (slot->active != 0u && slot->target_identity == target_identity &&
            slot->completion.service_ref != 0u)
            ++count;
    }
    *out_count = count;
    return 1;
}

int p4_remove_bank_next_target(const struct p4_remove_bank *bank,
                               uintptr_t target_identity,
                               uint32_t first_index,
                               struct p4_remove_handle *out_handle)
{
    struct p4_remove_bank_view view;
    struct p4_remove_handle result;
    uint32_t index;

    if (target_identity == (uintptr_t)0 || out_handle == NULL)
        return -1;
    if (!p4_remove_bank_view_init(
            bank, out_handle, sizeof(*out_handle),
            _Alignof(struct p4_remove_handle), NULL, 0u, 0u, &view))
        return -1;
    if (first_index > view.capacity)
        return -1;

    for (index = first_index; index < view.capacity; ++index)
    {
        const struct p4_remove_bank_slot *slot = &view.slots[index];
        if (slot->active != 0u && slot->target_identity == target_identity &&
            slot->completion.service_ref != 0u)
        {
            result.index = index;
            result.generation = slot->generation;
            *out_handle = result;
            return 1;
        }
    }
    return 0;
}
