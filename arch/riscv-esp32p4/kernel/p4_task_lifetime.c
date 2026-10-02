/*
 * Bounded state transitions for task notification pins and running contexts.
 * The caller supplies serialization; see p4_task_lifetime.h.
 */
#include "p4_task_lifetime.h"

void p4_task_lifetime_init(struct p4_task_lifetime *lifetime)
{
    if (lifetime == 0)
        return;

    lifetime->notification_pins = 0;
    lifetime->running_harts = 0;
    lifetime->flags = 0;
}

int p4_task_lifetime_pin_locked(struct p4_task_lifetime *lifetime)
{
    if (lifetime == 0 ||
        (lifetime->flags & (P4_TASK_LIFETIME_RETIRED |
                            P4_TASK_LIFETIME_CLAIMED)) != 0 ||
        lifetime->notification_pins == UINT32_MAX)
        return 0;

    ++lifetime->notification_pins;
    return 1;
}

int p4_task_lifetime_unpin_locked(struct p4_task_lifetime *lifetime)
{
    if (lifetime == 0 ||
        (lifetime->flags & P4_TASK_LIFETIME_CLAIMED) != 0 ||
        lifetime->notification_pins == 0)
        return 0;

    --lifetime->notification_pins;
    return 1;
}

int p4_task_lifetime_retire_locked(struct p4_task_lifetime *lifetime)
{
    if (lifetime == 0 ||
        (lifetime->flags & (P4_TASK_LIFETIME_RETIRED |
                            P4_TASK_LIFETIME_CLAIMED)) != 0)
        return 0;

    lifetime->flags |= P4_TASK_LIFETIME_RETIRED;
    return 1;
}

int p4_task_lifetime_enter_running_locked(
    struct p4_task_lifetime *lifetime, uint32_t hart)
{
    uint32_t hart_bit;

    if (lifetime == 0 || hart >= P4_TASK_LIFETIME_HARTS ||
        (lifetime->flags & (P4_TASK_LIFETIME_RETIRED |
                            P4_TASK_LIFETIME_CLAIMED)) != 0 ||
        lifetime->running_harts != 0)
        return 0;

    hart_bit = UINT32_C(1) << hart;
    lifetime->running_harts |= hart_bit;
    return 1;
}

int p4_task_lifetime_leave_running_locked(
    struct p4_task_lifetime *lifetime, uint32_t hart)
{
    uint32_t hart_bit;

    if (lifetime == 0 || hart >= P4_TASK_LIFETIME_HARTS)
        return 0;

    hart_bit = UINT32_C(1) << hart;
    if ((lifetime->running_harts & hart_bit) == 0)
        return 0;

    lifetime->running_harts &= ~hart_bit;
    return 1;
}

int p4_task_lifetime_reclaimable_locked(
    const struct p4_task_lifetime *lifetime)
{
    return lifetime != 0 &&
           (lifetime->flags & P4_TASK_LIFETIME_RETIRED) != 0 &&
           (lifetime->flags & P4_TASK_LIFETIME_CLAIMED) == 0 &&
           lifetime->notification_pins == 0 &&
           lifetime->running_harts == 0;
}

int p4_task_lifetime_claim_reclaim_locked(
    struct p4_task_lifetime *lifetime)
{
    if (!p4_task_lifetime_reclaimable_locked(lifetime))
        return 0;

    lifetime->flags |= P4_TASK_LIFETIME_CLAIMED;
    return 1;
}
