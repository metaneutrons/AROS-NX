#ifndef P4_SOFTINT_QUEUE_H
#define P4_SOFTINT_QUEUE_H
#include <stdint.h>
struct ExecBase;
struct Interrupt;
struct p4_softint_state
{
    uint32_t pending;
    uint32_t active;
};
/* Caller holds the shared softint gate, with local interrupts masked.
 * Descriptor storage remains caller-owned until its handler completes.
 * All five lists, descriptor type and these flags share the same gate.
 * No Task callback, IRQ enable, allocation or wake is performed here. */
int p4_softint_enqueue(struct p4_softint_state *state, struct ExecBase *base,
                      struct Interrupt *interrupt);
/* Only hart0 may begin/dequeue; active blocks nested dispatcher reentry. */
int p4_softint_begin(struct p4_softint_state *state, uint32_t hart);
int p4_softint_take(struct p4_softint_state *state, struct ExecBase *base,
                    uint32_t hart, struct Interrupt **interrupt);
#endif
