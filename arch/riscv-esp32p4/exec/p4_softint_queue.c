/* Portable shared-gate softinterrupt transactions, selected only for P4 SMP. */
#if !defined(P4_SOFTINT_QUEUE_TEST)
#include <stddef.h>
#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <exec/nodes.h>
#endif
#include "p4_softint_queue.h"

static int p4_softint_lists_valid(struct ExecBase *base,
                                struct Interrupt *target, unsigned int *found)
{
    unsigned int i;
    *found = 0;
    for (i = 0; i < 5u; ++i)
    {
        struct List *list = &base->SoftInts[i].sh_List;
        struct Node *node = list->lh_Head;
        struct Node *previous = (struct Node *)&list->lh_Head;
        struct Node *tail = (struct Node *)&list->lh_Tail;
        unsigned int left = 20000u;
        if (list->lh_Tail != NULL)
            return 0;
        while (node != NULL && node != tail && left--)
        {
            if (node->ln_Pred != previous || node->ln_Succ == NULL ||
                node->ln_Type != NT_SOFTINT)
                return 0;
            if (node == (struct Node *)target)
                ++*found;
            previous = node;
            node = node->ln_Succ;
        }
        if (node != tail || list->lh_TailPred != previous)
            return 0;
    }
    return *found <= 1u;
}

int p4_softint_enqueue(struct p4_softint_state *state, struct ExecBase *base,
                      struct Interrupt *interrupt)
{
    unsigned int found, priority;
    struct List *list;
    struct Node *node;
    if (state == NULL || base == NULL || interrupt == NULL ||
        state->pending > 1u || state->active > 1u ||
        !p4_softint_lists_valid(base, interrupt, &found))
        return 0;
    if (found)
    {
        state->pending = 1;
        return 1; /* Already queued exactly once. */
    }
    if ((interrupt->is_Node.ln_Type != NT_INTERRUPT &&
         interrupt->is_Node.ln_Type != NT_UNKNOWN) ||
        interrupt->is_Node.ln_Pri < -32 || interrupt->is_Node.ln_Pri > 32)
        return 0;
    priority = (unsigned int)((int)interrupt->is_Node.ln_Pri + 32) >> 4;
    list = &base->SoftInts[priority].sh_List;
    node = &interrupt->is_Node;
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
    node->ln_Type = NT_SOFTINT;
    state->pending = 1;
    return 1;
}

int p4_softint_begin(struct p4_softint_state *state, uint32_t hart)
{
    if (state == NULL || hart != 0 || state->pending > 1u || state->active > 1u)
        return -1;
    if (!state->pending || state->active)
        return 0;
    state->active = 1;
    return 1;
}

int p4_softint_take(struct p4_softint_state *state, struct ExecBase *base,
                    uint32_t hart, struct Interrupt **interrupt)
{
    unsigned int found;
    int priority;
    if (interrupt == NULL)
        return 0;
    *interrupt = NULL;
    if (state == NULL || base == NULL || hart != 0 || state->active != 1u ||
        state->pending > 1u || !p4_softint_lists_valid(base, NULL, &found))
        return 0;
    for (priority = 4; priority >= 0; --priority)
    {
        struct List *list = &base->SoftInts[priority].sh_List;
        struct Node *node = list->lh_Head;
        if (node != (struct Node *)&list->lh_Tail)
        {
            list->lh_Head = node->ln_Succ;
            node->ln_Succ->ln_Pred = (struct Node *)&list->lh_Head;
            node->ln_Type = NT_INTERRUPT;
            *interrupt = (struct Interrupt *)node;
            return 1;
        }
    }
    /* Empty observation and dispatcher release are one shared-gate step;
     * a later enqueue sets pending again and must send a copied wake. */
    state->pending = 0;
    state->active = 0;
    return 1;
}
