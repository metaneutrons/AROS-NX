#ifndef P4_REMOVE_COMPLETION_H
#define P4_REMOVE_COMPLETION_H
#include <stdint.h>
/* Ticket storage is independent of the removed Task and every Task stack.
 * All transitions require the caller's one shared completion gate. A target
 * with multiple removers needs one ticket per requester, not a shared stack
 * signal. This leaf neither dereferences identities nor sends notifications.
 * Output objects must be valid, aligned int storage disjoint from the ticket.
 * Ticket-overlapping outputs are refused before any mutation. */
struct p4_remove_completion
{
    uint32_t requester_ref;
    uint32_t service_ref;
    uint32_t completed;
};
/* Initialize only zeroed, unpublished storage, before retirement publication. */
int p4_remove_completion_init(struct p4_remove_completion *ticket);
/* Service owns cleanup and must finish every target/context/free access before
 * this acknowledgement. Returns notify=1 only while requester owns its ref.
 * Caller must snapshot/pin any recipient before unlocking and sending a wake. */
int p4_remove_completion_ack(struct p4_remove_completion *ticket,
                             int *notify, int *reclaim);
/* Normal requester may drop only after completion. Cancellation may drop an
 * outstanding request, but cannot consume the service's cleanup reference. */
int p4_remove_completion_drop(struct p4_remove_completion *ticket,
                              int cancel, int *reclaim);
/* Read under the same gate; false does not permit Task storage to be freed. */
int p4_remove_completion_ready(const struct p4_remove_completion *ticket);
#endif
