/*
 * Host tests for the actual P4 soft-interrupt queue implementation.
 * P4_SOFTINT_QUEUE_TEST suppresses only Exec header inclusion; queue logic is
 * compiled verbatim. Calls are serialized, representing the caller-held P4
 * shared gate. This does not test that gate, Cause(), or hardware delivery.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NT_UNKNOWN   0u
#define NT_INTERRUPT 2u
#define NT_SOFTINT   11u

struct Node
{
    struct Node *ln_Succ;
    struct Node *ln_Pred;
    uint8_t ln_Type;
    int8_t ln_Pri;
};

struct List
{
    struct Node *lh_Head;
    struct Node *lh_Tail;
    struct Node *lh_TailPred;
};

struct Interrupt
{
    struct Node is_Node;
};

struct SoftIntList
{
    struct List sh_List;
};

struct ExecBase
{
    struct SoftIntList SoftInts[5];
};

#define P4_SOFTINT_QUEUE_TEST 1
#include "../p4_softint_queue.c"

static unsigned int checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 0; \
    } \
} while (0)

static void list_init(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

static void exec_init(struct ExecBase *base)
{
    size_t i;
    memset(base, 0, sizeof(*base));
    for (i = 0; i < 5u; ++i)
        list_init(&base->SoftInts[i].sh_List);
}

static void interrupt_init(struct Interrupt *interrupt, int priority,
                           uint8_t type)
{
    memset(interrupt, 0, sizeof(*interrupt));
    interrupt->is_Node.ln_Pri = (int8_t)priority;
    interrupt->is_Node.ln_Type = type;
}

static unsigned int queue_length(const struct List *list)
{
    const struct Node *node = list->lh_Head;
    const struct Node *tail = (const struct Node *)&list->lh_Tail;
    unsigned int length = 0;

    while (node != tail && length < 20001u)
    {
        ++length;
        node = node->ln_Succ;
    }
    return length;
}

static int queue_contains(const struct List *list,
                          const struct Interrupt *interrupt)
{
    const struct Node *node = list->lh_Head;
    const struct Node *tail = (const struct Node *)&list->lh_Tail;
    unsigned int left = 20001u;

    while (node != tail && node != NULL && left-- != 0u)
    {
        if (node == (const struct Node *)interrupt)
            return 1;
        node = node->ln_Succ;
    }
    return 0;
}

struct snapshot
{
    struct List lists[5];
    struct p4_softint_state state;
    struct Interrupt nodes[4];
};

static void take_snapshot(struct snapshot *snapshot,
                          const struct ExecBase *base,
                          const struct p4_softint_state *state,
                          const struct Interrupt nodes[4])
{
    size_t i;
    for (i = 0; i < 5u; ++i)
        snapshot->lists[i] = base->SoftInts[i].sh_List;
    snapshot->state = *state;
    for (i = 0; i < 4u; ++i)
        snapshot->nodes[i] = nodes[i];
}

static int same_node(const struct Node *a, const struct Node *b)
{
    return a->ln_Succ == b->ln_Succ && a->ln_Pred == b->ln_Pred &&
        a->ln_Type == b->ln_Type && a->ln_Pri == b->ln_Pri;
}

static int snapshot_matches(const struct snapshot *snapshot,
                            const struct ExecBase *base,
                            const struct p4_softint_state *state,
                            const struct Interrupt nodes[4])
{
    size_t i;
    if (state->pending != snapshot->state.pending ||
        state->active != snapshot->state.active)
        return 0;
    for (i = 0; i < 5u; ++i)
    {
        const struct List *a = &snapshot->lists[i];
        const struct List *b = &base->SoftInts[i].sh_List;
        if (a->lh_Head != b->lh_Head || a->lh_Tail != b->lh_Tail ||
            a->lh_TailPred != b->lh_TailPred)
            return 0;
    }
    for (i = 0; i < 4u; ++i)
    {
        if (!same_node(&snapshot->nodes[i].is_Node, &nodes[i].is_Node))
            return 0;
    }
    return 1;
}

static int test_priority_order_and_fifo(void)
{
    static const int priorities[] = {-32, -17, -16, -1, 0, 15, 16, 31, 32};
    static const unsigned int buckets[] = {0u, 0u, 1u, 1u, 2u, 2u, 3u, 3u, 4u};
    struct ExecBase base;
    struct p4_softint_state state = {0u, 0u};
    struct Interrupt items[9];
    struct Interrupt *taken;
    unsigned int expected = 0;
    int priority;
    size_t i;

    exec_init(&base);
    for (i = 0; i < sizeof(items) / sizeof(items[0]); ++i)
    {
        interrupt_init(&items[i], priorities[i], NT_INTERRUPT);
        CHECK(p4_softint_enqueue(&state, &base, &items[i]) == 1);
        CHECK(items[i].is_Node.ln_Type == NT_SOFTINT);
        CHECK(queue_contains(&base.SoftInts[buckets[i]].sh_List, &items[i]));
    }
    CHECK(state.pending == 1u && state.active == 0u);
    CHECK(p4_softint_begin(&state, 0u) == 1);
    for (priority = 4; priority >= 0; --priority)
    {
        for (i = 0; i < sizeof(items) / sizeof(items[0]); ++i)
        {
            if (buckets[i] == (unsigned int)priority)
            {
                taken = NULL;
                CHECK(p4_softint_take(&state, &base, 0u, &taken) == 1);
                CHECK(taken != NULL);
                CHECK(taken == &items[i]);
                CHECK(taken->is_Node.ln_Type == NT_INTERRUPT);
                ++expected;
            }
        }
    }
    CHECK(expected == 9u);
    CHECK(queue_length(&base.SoftInts[0].sh_List) == 0u);
    CHECK(queue_length(&base.SoftInts[1].sh_List) == 0u);
    CHECK(queue_length(&base.SoftInts[2].sh_List) == 0u);
    CHECK(queue_length(&base.SoftInts[3].sh_List) == 0u);
    CHECK(queue_length(&base.SoftInts[4].sh_List) == 0u);
    taken = (struct Interrupt *)(uintptr_t)1u;
    CHECK(p4_softint_take(&state, &base, 0u, &taken) == 1);
    CHECK(taken == NULL);
    CHECK(state.pending == 0u && state.active == 0u);
    return 1;
}

static int test_duplicate_and_requeue(void)
{
    struct ExecBase base;
    struct p4_softint_state state = {0u, 0u};
    struct Interrupt interrupt;
    struct Interrupt *taken = NULL;

    exec_init(&base);
    interrupt_init(&interrupt, 4, NT_INTERRUPT);
    CHECK(p4_softint_enqueue(&state, &base, &interrupt) == 1);
    state.pending = 0u;
    CHECK(p4_softint_enqueue(&state, &base, &interrupt) == 1);
    CHECK(state.pending == 1u);
    CHECK(queue_length(&base.SoftInts[2].sh_List) == 1u);
    CHECK(p4_softint_begin(&state, 0u) == 1);
    CHECK(p4_softint_begin(&state, 0u) == 0); /* active callback: no recursion */
    CHECK(p4_softint_take(&state, &base, 0u, &taken) == 1);
    CHECK(taken == &interrupt);
    CHECK(state.pending == 1u && state.active == 1u);

    /* Simulate a handler requeueing itself while the owner is active. */
    CHECK(p4_softint_enqueue(&state, &base, taken) == 1);
    CHECK(queue_length(&base.SoftInts[2].sh_List) == 1u);
    CHECK(p4_softint_begin(&state, 0u) == 0);
    taken = NULL;
    CHECK(p4_softint_take(&state, &base, 0u, &taken) == 1);
    CHECK(taken == &interrupt);
    CHECK(state.pending == 1u && state.active == 1u);
    taken = (struct Interrupt *)(uintptr_t)1u;
    CHECK(p4_softint_take(&state, &base, 0u, &taken) == 1);
    CHECK(taken == NULL);
    CHECK(state.pending == 0u && state.active == 0u);
    return 1;
}

static int test_empty_then_later_enqueue(void)
{
    struct ExecBase base;
    struct p4_softint_state state = {1u, 0u};
    struct Interrupt interrupt;
    struct Interrupt *taken = NULL;

    exec_init(&base);
    interrupt_init(&interrupt, 0, NT_UNKNOWN);
    CHECK(p4_softint_begin(&state, 0u) == 1);
    CHECK(p4_softint_take(&state, &base, 0u, &taken) == 1);
    CHECK(taken == NULL);
    CHECK(state.pending == 0u && state.active == 0u);

    /* This is the lost-wake boundary: a later enqueue re-arms pending. */
    CHECK(p4_softint_enqueue(&state, &base, &interrupt) == 1);
    CHECK(state.pending == 1u && state.active == 0u);
    CHECK(p4_softint_begin(&state, 0u) == 1);
    CHECK(p4_softint_take(&state, &base, 0u, &taken) == 1);
    CHECK(taken == &interrupt);
    taken = (struct Interrupt *)(uintptr_t)1u;
    CHECK(p4_softint_take(&state, &base, 0u, &taken) == 1);
    CHECK(taken == NULL);
    CHECK(state.pending == 0u && state.active == 0u);
    return 1;
}

static int test_invalid_enqueue_unchanged(void)
{
    struct ExecBase base;
    struct p4_softint_state state = {0u, 0u};
    struct Interrupt nodes[4];
    struct snapshot before;
    int priorities[] = {-33, 33};
    size_t i;

    exec_init(&base);
    for (i = 0; i < 4u; ++i)
        interrupt_init(&nodes[i], 0, NT_INTERRUPT);
    for (i = 0; i < sizeof(priorities) / sizeof(priorities[0]); ++i)
    {
        nodes[0].is_Node.ln_Pri = (int8_t)priorities[i];
        take_snapshot(&before, &base, &state, nodes);
        CHECK(p4_softint_enqueue(&state, &base, &nodes[0]) == 0);
        CHECK(snapshot_matches(&before, &base, &state, nodes));
    }
    state.pending = 2u;
    nodes[0].is_Node.ln_Pri = 0;
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_enqueue(&state, &base, &nodes[0]) == 0);
    CHECK(snapshot_matches(&before, &base, &state, nodes));
    state.pending = 0u;
    state.active = 2u;
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_enqueue(&state, &base, &nodes[0]) == 0);
    CHECK(snapshot_matches(&before, &base, &state, nodes));
    state.active = 0u;
    nodes[0].is_Node.ln_Pri = 0;
    nodes[0].is_Node.ln_Type = NT_SOFTINT; /* foreign/unlinked marker */
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_enqueue(&state, &base, &nodes[0]) == 0);
    CHECK(snapshot_matches(&before, &base, &state, nodes));

    nodes[0].is_Node.ln_Type = 99u;
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_enqueue(&state, &base, &nodes[0]) == 0);
    CHECK(snapshot_matches(&before, &base, &state, nodes));
    CHECK(p4_softint_enqueue(NULL, &base, &nodes[0]) == 0);
    CHECK(p4_softint_enqueue(&state, NULL, &nodes[0]) == 0);
    CHECK(p4_softint_enqueue(&state, &base, NULL) == 0);
    return 1;
}

static int test_bad_state_and_hart(void)
{
    struct ExecBase base;
    struct p4_softint_state state = {1u, 0u};
    struct Interrupt nodes[4];
    struct snapshot before;
    struct Interrupt *taken;

    exec_init(&base);
    interrupt_init(&nodes[0], 0, NT_INTERRUPT);
    interrupt_init(&nodes[1], 0, NT_INTERRUPT);
    interrupt_init(&nodes[2], 0, NT_INTERRUPT);
    interrupt_init(&nodes[3], 0, NT_INTERRUPT);
    CHECK(p4_softint_enqueue(&state, &base, &nodes[0]) == 1);

    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_begin(&state, 1u) == -1);
    CHECK(snapshot_matches(&before, &base, &state, nodes));
    CHECK(p4_softint_begin(&state, 0u) == 1);
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_begin(&state, 0u) == 0);
    CHECK(snapshot_matches(&before, &base, &state, nodes));
    state.active = 0u;
    state.pending = 0u;
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_begin(&state, 0u) == 0);
    CHECK(snapshot_matches(&before, &base, &state, nodes));
    state.pending = 1u;
    CHECK(p4_softint_begin(NULL, 0u) == -1);
    state.pending = 2u;
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_begin(&state, 0u) == -1);
    CHECK(snapshot_matches(&before, &base, &state, nodes));
    state.pending = 1u;
    state.active = 2u;
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_begin(&state, 0u) == -1);
    CHECK(snapshot_matches(&before, &base, &state, nodes));

    state.pending = 1u;
    state.active = 0u;
    taken = (struct Interrupt *)(uintptr_t)1u;
    state.active = 0u;
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_take(&state, &base, 0u, &taken) == 0);
    CHECK(taken == NULL);
    CHECK(snapshot_matches(&before, &base, &state, nodes));
    state.active = 0u;
    state.pending = 1u;
    taken = (struct Interrupt *)(uintptr_t)1u;
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_take(&state, &base, 1u, &taken) == 0);
    CHECK(taken == NULL);
    CHECK(snapshot_matches(&before, &base, &state, nodes));
    taken = (struct Interrupt *)(uintptr_t)1u;
    CHECK(p4_softint_take(&state, &base, 0u, NULL) == 0);
    CHECK(p4_softint_take(NULL, &base, 0u, &taken) == 0);
    CHECK(taken == NULL);
    taken = (struct Interrupt *)(uintptr_t)1u;
    CHECK(p4_softint_take(&state, NULL, 0u, &taken) == 0);
    CHECK(taken == NULL);
    CHECK(snapshot_matches(&before, &base, &state, nodes));

    state.active = 1u;
    state.pending = 2u;
    taken = (struct Interrupt *)(uintptr_t)1u;
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_take(&state, &base, 0u, &taken) == 0);
    CHECK(taken == NULL);
    CHECK(snapshot_matches(&before, &base, &state, nodes));
    state.pending = 1u;
    state.active = 2u;
    taken = (struct Interrupt *)(uintptr_t)1u;
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_take(&state, &base, 0u, &taken) == 0);
    CHECK(taken == NULL);
    CHECK(snapshot_matches(&before, &base, &state, nodes));
    return 1;
}

static int test_corrupt_list_refusal(void)
{
    struct ExecBase base;
    struct p4_softint_state state = {0u, 0u};
    struct Interrupt nodes[4];
    struct snapshot before;
    struct Interrupt *taken;
    struct List *list;

    exec_init(&base);
    for (size_t i = 0; i < 4u; ++i)
        interrupt_init(&nodes[i], 0, NT_INTERRUPT);
    list = &base.SoftInts[2].sh_List;

    /* Refuse a non-null tail sentinel without changing any state. */
    list->lh_Tail = (struct Node *)&nodes[3];
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_enqueue(&state, &base, &nodes[0]) == 0);
    CHECK(snapshot_matches(&before, &base, &state, nodes));
    state.active = 1u;
    state.pending = 1u;
    taken = (struct Interrupt *)(uintptr_t)1u;
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_take(&state, &base, 0u, &taken) == 0);
    CHECK(taken == NULL);
    CHECK(snapshot_matches(&before, &base, &state, nodes));

    /* Restore, enqueue two valid members, then corrupt the tail backlink. */
    exec_init(&base);
    state.pending = 0u;
    state.active = 0u;
    interrupt_init(&nodes[0], 0, NT_INTERRUPT);
    CHECK(p4_softint_enqueue(&state, &base, &nodes[0]) == 1);
    CHECK(p4_softint_enqueue(&state, &base, &nodes[1]) == 1);
    list = &base.SoftInts[2].sh_List;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_enqueue(&state, &base, &nodes[2]) == 0);
    CHECK(snapshot_matches(&before, &base, &state, nodes));

    /* Target is seen first; the validator must still inspect the later node. */
    list->lh_TailPred = &nodes[1].is_Node;
    nodes[1].is_Node.ln_Pred = (struct Node *)&list->lh_Head;
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_enqueue(&state, &base, &nodes[0]) == 0);
    CHECK(snapshot_matches(&before, &base, &state, nodes));
    state.pending = 1u;
    state.active = 1u;
    taken = (struct Interrupt *)(uintptr_t)1u;
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_take(&state, &base, 0u, &taken) == 0);
    CHECK(taken == NULL);
    CHECK(snapshot_matches(&before, &base, &state, nodes));

    /* A reachable cycle is rejected on the repeated-node backlink check. */
    exec_init(&base);
    state.pending = 0u;
    state.active = 0u;
    interrupt_init(&nodes[0], 0, NT_INTERRUPT);
    CHECK(p4_softint_enqueue(&state, &base, &nodes[0]) == 1);
    list = &base.SoftInts[2].sh_List;
    nodes[0].is_Node.ln_Succ = &nodes[0].is_Node;
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_enqueue(&state, &base, &nodes[2]) == 0);
    CHECK(snapshot_matches(&before, &base, &state, nodes));
    return 1;
}

static struct Node bounded_nodes[20001];
static struct Node bounded_nodes_before[20001];

static int test_scan_bound(void)
{
    struct ExecBase base;
    struct p4_softint_state state = {0u, 0u};
    struct Interrupt nodes[4];
    struct snapshot before;
    struct List *list;
    size_t i;

    exec_init(&base);
    for (i = 0; i < 4u; ++i)
        interrupt_init(&nodes[i], 0, NT_INTERRUPT);
    list = &base.SoftInts[0].sh_List;
    memset(bounded_nodes, 0, sizeof(bounded_nodes));
    list->lh_Head = &bounded_nodes[0];
    list->lh_Tail = NULL;
    list->lh_TailPred = &bounded_nodes[20000];
    for (i = 0; i < 20001u; ++i)
    {
        bounded_nodes[i].ln_Pred = i == 0u ? (struct Node *)&list->lh_Head :
            &bounded_nodes[i - 1u];
        bounded_nodes[i].ln_Succ = i == 20000u ?
            (struct Node *)&list->lh_Tail : &bounded_nodes[i + 1u];
        bounded_nodes[i].ln_Type = NT_SOFTINT;
    }
    memcpy(bounded_nodes_before, bounded_nodes, sizeof(bounded_nodes));
    take_snapshot(&before, &base, &state, nodes);
    CHECK(p4_softint_enqueue(&state, &base, &nodes[0]) == 0);
    CHECK(snapshot_matches(&before, &base, &state, nodes));
    for (i = 0; i < 20001u; ++i)
    {
        if (bounded_nodes[i].ln_Succ != bounded_nodes_before[i].ln_Succ ||
            bounded_nodes[i].ln_Pred != bounded_nodes_before[i].ln_Pred ||
            bounded_nodes[i].ln_Type != bounded_nodes_before[i].ln_Type ||
            bounded_nodes[i].ln_Pri != bounded_nodes_before[i].ln_Pri)
        {
            CHECK(0);
        }
    }
    CHECK(1);
    return 1;
}

int main(void)
{
    if (!test_priority_order_and_fifo() || !test_duplicate_and_requeue() ||
        !test_empty_then_later_enqueue() || !test_invalid_enqueue_unchanged() ||
        !test_bad_state_and_hart() || !test_corrupt_list_refusal() ||
        !test_scan_bound())
        return 1;
    printf("PASS p4_softint_queue checks=%u\n", checks);
    return 0;
}
