/* Portable external RemTask acknowledgement protocol; integration pending. */
#include <stddef.h>
#include "p4_remove_completion.h"

/* Difference comparisons avoid overflowing an end-address calculation. */
static int completion_output_overlaps(const struct p4_remove_completion *ticket,
                                      const int *output)
{
    uintptr_t t = (uintptr_t)ticket, o = (uintptr_t)output;
    return t <= o ? o - t < sizeof(*ticket) : t - o < sizeof(*output);
}

static int completion_valid(const struct p4_remove_completion *ticket)
{
    return ticket != NULL && ticket->requester_ref <= 1u &&
           ticket->service_ref <= 1u && ticket->completed <= 1u &&
           ticket->service_ref == 1u - ticket->completed;
}

int p4_remove_completion_init(struct p4_remove_completion *ticket)
{
    if (ticket == NULL || ticket->requester_ref || ticket->service_ref ||
        ticket->completed)
        return 0;
    ticket->requester_ref = ticket->service_ref = 1;
    return 1;
}

int p4_remove_completion_ack(struct p4_remove_completion *ticket,
                             int *notify, int *reclaim)
{
    if (notify == NULL || reclaim == NULL || notify == reclaim ||
        completion_output_overlaps(ticket, notify) ||
        completion_output_overlaps(ticket, reclaim) ||
        !completion_valid(ticket) || !ticket->service_ref)
        return 0;
    ticket->completed = 1;
    ticket->service_ref = 0;
    *notify = ticket->requester_ref != 0;
    *reclaim = ticket->requester_ref == 0;
    return 1;
}

int p4_remove_completion_drop(struct p4_remove_completion *ticket,
                              int cancel, int *reclaim)
{
    if (reclaim == NULL || (cancel != 0 && cancel != 1) ||
        completion_output_overlaps(ticket, reclaim) ||
        !completion_valid(ticket) || !ticket->requester_ref ||
        (!cancel && !ticket->completed))
        return 0;
    ticket->requester_ref = 0;
    *reclaim = ticket->service_ref == 0;
    return 1;
}

int p4_remove_completion_ready(const struct p4_remove_completion *ticket)
{
    return completion_valid(ticket) && ticket->requester_ref && ticket->completed;
}
