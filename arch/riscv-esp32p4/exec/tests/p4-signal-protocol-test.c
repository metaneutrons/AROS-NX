/*
 * Host regression fixture for the portable P4 Exec signal/wait state machine.
 * The fixture's mutex models the caller-held Task.tc_SpinLock. Queue effects
 * are applied separately while the lock is held, as production adapters must.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../p4_signal_protocol.h"

#define CHECK(condition) do { ++checks; if (!(condition)) { fprintf(stderr, "FAIL line %d: %s", __LINE__, #condition); return 0; } } while (0)

struct fixture
{
    struct p4_signal_task_state task;
    void *wait_pending;
    int running;
    int ready;
    int waiting;
    unsigned int queue_moves;
};

static unsigned long checks;

static void fixture_init(struct fixture *fixture, uint32_t owner_hart)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->task.owner = P4_SIGNAL_OWNER_RUNNING;
    fixture->task.owner_hart = owner_hart;
    fixture->task.exception_flag_mask = UINT8_C(0x04);
    fixture->running = 1;
}

static int fixture_invariant(const struct fixture *fixture)
{
    unsigned int membership = (unsigned int)fixture->running +
        (unsigned int)fixture->ready + (unsigned int)fixture->waiting;

    if (fixture->task.owner == P4_SIGNAL_OWNER_REMOVED)
        return membership == 0;
    if (membership != 1)
        return 0;
    if ((fixture->task.owner == P4_SIGNAL_OWNER_RUNNING) !=
        (fixture->running != 0))
        return 0;
    if ((fixture->task.owner == P4_SIGNAL_OWNER_READY) !=
        (fixture->ready != 0))
        return 0;
    if ((fixture->task.owner == P4_SIGNAL_OWNER_WAITING) !=
        (fixture->waiting != 0))
        return 0;
    if (fixture->wait_pending != 0 &&
        (fixture->task.owner != P4_SIGNAL_OWNER_RUNNING ||
         fixture->wait_pending != fixture))
        return 0;

    return 1;
}

static int fixture_apply_effect(
    struct fixture *fixture,
    struct p4_signal_effect effect)
{
    if (effect.status != P4_SIGNAL_STATUS_OK)
        return 0;

    switch (effect.queue_transition)
    {
    case P4_SIGNAL_QUEUE_NONE:
        break;

    case P4_SIGNAL_QUEUE_RUNNING_TO_READY:
        if (!fixture->running ||
            fixture->task.owner != P4_SIGNAL_OWNER_RUNNING)
            return 0;
        fixture->running = 0;
        fixture->ready = 1;
        fixture->task.owner = P4_SIGNAL_OWNER_READY;
        ++fixture->queue_moves;
        break;

    case P4_SIGNAL_QUEUE_RUNNING_TO_WAIT:
        if (!fixture->running ||
            fixture->task.owner != P4_SIGNAL_OWNER_RUNNING)
            return 0;
        fixture->running = 0;
        fixture->waiting = 1;
        fixture->task.owner = P4_SIGNAL_OWNER_WAITING;
        ++fixture->queue_moves;
        break;

    case P4_SIGNAL_QUEUE_WAITING_TO_READY:
        if (!fixture->waiting ||
            fixture->task.owner != P4_SIGNAL_OWNER_WAITING)
            return 0;
        fixture->waiting = 0;
        fixture->ready = 1;
        fixture->task.owner = P4_SIGNAL_OWNER_READY;
        ++fixture->queue_moves;
        break;

    default:
        return 0;
    }

    return fixture_invariant(fixture);
}

static int test_signal_before_wait_arm(void)
{
    struct fixture fixture;
    struct p4_signal_effect effect;

    fixture_init(&fixture, 0);
    effect = p4_signal_raise_locked(&fixture.task, 0, UINT32_C(0x01), 1);
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.queue_transition == P4_SIGNAL_QUEUE_NONE);
    CHECK(effect.request_reschedule == 0);
    CHECK(fixture_apply_effect(&fixture, effect));

    effect = p4_signal_wait_arm_locked(&fixture.task,
        &fixture.wait_pending, &fixture, UINT32_C(0x01));
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.wait_outcome == P4_SIGNAL_WAIT_RETURNED_SIGNALS);
    CHECK(effect.returned_signals == UINT32_C(0x01));
    CHECK(fixture.wait_pending == 0);
    CHECK(fixture.task.sig_received == 0);
    CHECK(fixture_apply_effect(&fixture, effect));
    return 1;
}

static int test_signal_between_arm_and_switch_commit(void)
{
    struct fixture fixture;
    struct p4_signal_effect effect;

    fixture_init(&fixture, 0);
    effect = p4_signal_wait_arm_locked(&fixture.task,
        &fixture.wait_pending, &fixture, UINT32_C(0x02));
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.wait_outcome == P4_SIGNAL_WAIT_ARMED);
    CHECK(fixture.wait_pending == &fixture);
    CHECK(fixture_apply_effect(&fixture, effect));

    effect = p4_signal_raise_locked(&fixture.task, 1, UINT32_C(0x02), 1);
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.queue_transition == P4_SIGNAL_QUEUE_NONE);
    CHECK(effect.request_reschedule == 1);
    CHECK(effect.reschedule_hart == 0);
    CHECK(effect.remote_reschedule == 1);
    CHECK(fixture.task.owner == P4_SIGNAL_OWNER_RUNNING);
    CHECK(fixture_apply_effect(&fixture, effect));

    effect = p4_signal_switch_commit_locked(&fixture.task,
        &fixture.wait_pending, &fixture);
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.queue_transition == P4_SIGNAL_QUEUE_RUNNING_TO_READY);
    CHECK(fixture.wait_pending == 0);
    CHECK(fixture_apply_effect(&fixture, effect));
    CHECK(fixture.task.owner == P4_SIGNAL_OWNER_READY);
    CHECK(fixture.queue_moves == 1);
    return 1;
}

static int test_signal_after_wait_commit(void)
{
    struct fixture fixture;
    struct p4_signal_effect effect;

    fixture_init(&fixture, 0);
    effect = p4_signal_wait_arm_locked(&fixture.task,
        &fixture.wait_pending, &fixture, UINT32_C(0x08));
    CHECK(effect.wait_outcome == P4_SIGNAL_WAIT_ARMED);
    CHECK(fixture_apply_effect(&fixture, effect));

    effect = p4_signal_switch_commit_locked(&fixture.task,
        &fixture.wait_pending, &fixture);
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.queue_transition == P4_SIGNAL_QUEUE_RUNNING_TO_WAIT);
    CHECK(fixture.wait_pending == 0);
    CHECK(fixture_apply_effect(&fixture, effect));
    CHECK(fixture.task.owner == P4_SIGNAL_OWNER_WAITING);

    effect = p4_signal_raise_locked(&fixture.task, 0, UINT32_C(0x08), 1);
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.queue_transition == P4_SIGNAL_QUEUE_WAITING_TO_READY);
    CHECK(fixture_apply_effect(&fixture, effect));
    CHECK(fixture.task.owner == P4_SIGNAL_OWNER_READY);
    CHECK(fixture.queue_moves == 2);

    effect = p4_signal_switch_commit_locked(&fixture.task,
        &fixture.wait_pending, &fixture);
    CHECK(effect.status == P4_SIGNAL_STATUS_INVALID_STATE);
    CHECK(fixture.queue_moves == 2);
    return 1;
}

static int test_stale_wait_mask_requires_live_token(void)
{
    struct fixture fixture;
    struct p4_signal_effect effect;

    fixture_init(&fixture, 0);
    fixture.task.sig_wait = UINT32_C(0x02); /* left by an earlier Wait */
    effect = p4_signal_raise_locked(
        &fixture.task, 0, UINT32_C(0x02), 1);
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.request_reschedule == 0);
    CHECK(effect.queue_transition == P4_SIGNAL_QUEUE_NONE);
    CHECK(fixture.task.sig_wait == UINT32_C(0x02));
    CHECK(fixture.task.sig_received == UINT32_C(0x02));
    CHECK(fixture.wait_pending == 0);
    CHECK(fixture_apply_effect(&fixture, effect));

    effect = p4_signal_wait_arm_locked(&fixture.task,
        &fixture.wait_pending, &fixture, UINT32_C(0x02));
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.wait_outcome == P4_SIGNAL_WAIT_RETURNED_SIGNALS);
    CHECK(effect.returned_signals == UINT32_C(0x02));
    CHECK(fixture.wait_pending == 0);
    CHECK(fixture.task.sig_received == 0);
    CHECK(fixture_apply_effect(&fixture, effect));
    return 1;
}

static int test_armed_match_and_exception_poke(void)
{
    struct fixture fixture;
    struct p4_signal_effect effect;

    fixture_init(&fixture, 0);
    effect = p4_signal_wait_arm_locked(&fixture.task,
        &fixture.wait_pending, &fixture, UINT32_C(0x04));
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.wait_outcome == P4_SIGNAL_WAIT_ARMED);
    CHECK(fixture_apply_effect(&fixture, effect));

    effect = p4_signal_raise_locked(
        &fixture.task, 1, UINT32_C(0x04), 1);
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.request_reschedule == 1);
    CHECK(effect.reschedule_hart == 0);
    CHECK(effect.remote_reschedule == 1);
    CHECK(fixture_apply_effect(&fixture, effect));

    fixture.wait_pending = 0;
    fixture.task.sig_wait = UINT32_C(0x08); /* stale, no live Wait */
    fixture.task.sig_except = UINT32_C(0x10);
    effect = p4_signal_raise_locked(
        &fixture.task, 0, UINT32_C(0x10), 1);
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.request_reschedule == 1);
    CHECK(effect.remote_reschedule == 1);
    CHECK((fixture.task.flags & fixture.task.exception_flag_mask) != 0);
    return 1;
}

static int test_repeated_wait_arm_is_nonmutating(void)
{
    struct fixture fixture;
    struct p4_signal_effect effect;
    uint32_t saved_wait;
    uint32_t saved_received;
    void *saved_pending;

    fixture_init(&fixture, 0);
    effect = p4_signal_wait_arm_locked(&fixture.task,
        &fixture.wait_pending, &fixture, UINT32_C(0x80));
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.wait_outcome == P4_SIGNAL_WAIT_ARMED);
    saved_wait = fixture.task.sig_wait;
    saved_received = fixture.task.sig_received;
    saved_pending = fixture.wait_pending;

    effect = p4_signal_wait_arm_locked(&fixture.task,
        &fixture.wait_pending, &fixture, UINT32_C(0x40));
    CHECK(effect.status == P4_SIGNAL_STATUS_PENDING_BUSY);
    CHECK(fixture.task.sig_wait == saved_wait);
    CHECK(fixture.task.sig_received == saved_received);
    CHECK(fixture.wait_pending == saved_pending);
    return 1;
}

static int test_zero_mask_and_exception_wakeup(void)
{
    struct fixture fixture;
    struct p4_signal_effect effect;

    fixture_init(&fixture, 0);
    effect = p4_signal_wait_arm_locked(&fixture.task,
        &fixture.wait_pending, &fixture, 0);
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.wait_outcome == P4_SIGNAL_WAIT_ARMED);
    CHECK(fixture_apply_effect(&fixture, effect));

    effect = p4_signal_switch_commit_locked(&fixture.task,
        &fixture.wait_pending, &fixture);
    CHECK(effect.queue_transition == P4_SIGNAL_QUEUE_RUNNING_TO_WAIT);
    CHECK(fixture_apply_effect(&fixture, effect));

    effect = p4_signal_raise_locked(&fixture.task, 0, UINT32_C(0x10), 0);
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.queue_transition == P4_SIGNAL_QUEUE_NONE);
    CHECK(fixture.task.owner == P4_SIGNAL_OWNER_WAITING);
    CHECK(fixture_apply_effect(&fixture, effect));

    effect = p4_signal_set_except_locked(&fixture.task,
        UINT32_C(0x10), UINT32_C(0x10), 0);
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.queue_transition == P4_SIGNAL_QUEUE_WAITING_TO_READY);
    CHECK((fixture.task.flags & fixture.task.exception_flag_mask) != 0);
    CHECK(fixture_apply_effect(&fixture, effect));
    CHECK(fixture.task.owner == P4_SIGNAL_OWNER_READY);
    return 1;
}

static int test_setsignal_does_not_raise_exception(void)
{
    struct fixture fixture;
    struct p4_signal_effect effect;

    fixture_init(&fixture, 0);
    fixture.task.sig_except = UINT32_C(0x40);
    effect = p4_signal_set_locked(&fixture.task,
        UINT32_C(0x40), UINT32_C(0x40));
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK((fixture.task.flags & fixture.task.exception_flag_mask) == 0);
    CHECK(fixture_apply_effect(&fixture, effect));

    effect = p4_signal_wait_arm_locked(&fixture.task,
        &fixture.wait_pending, &fixture, UINT32_C(0x02));
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.wait_outcome == P4_SIGNAL_WAIT_ARMED);
    CHECK(fixture_apply_effect(&fixture, effect));

    effect = p4_signal_switch_commit_locked(&fixture.task,
        &fixture.wait_pending, &fixture);
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.queue_transition == P4_SIGNAL_QUEUE_RUNNING_TO_WAIT);
    CHECK((fixture.task.flags & fixture.task.exception_flag_mask) == 0);
    CHECK(fixture_apply_effect(&fixture, effect));
    CHECK(fixture.task.owner == P4_SIGNAL_OWNER_WAITING);
    return 1;
}

static int test_duplicate_signal_wakes_once(void)
{
    struct fixture fixture;
    struct p4_signal_effect effect;

    fixture_init(&fixture, 0);
    fixture.running = 0;
    fixture.waiting = 1;
    fixture.task.owner = P4_SIGNAL_OWNER_WAITING;
    fixture.task.sig_wait = UINT32_C(0x03);

    effect = p4_signal_raise_locked(&fixture.task, 0, UINT32_C(0x01), 1);
    CHECK(effect.queue_transition == P4_SIGNAL_QUEUE_WAITING_TO_READY);
    CHECK(fixture_apply_effect(&fixture, effect));

    effect = p4_signal_raise_locked(&fixture.task, 0, UINT32_C(0x02), 1);
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.queue_transition == P4_SIGNAL_QUEUE_NONE);
    CHECK(fixture_apply_effect(&fixture, effect));
    CHECK(fixture.task.sig_received == UINT32_C(0x03));
    CHECK(fixture.queue_moves == 1);
    CHECK(fixture.ready == 1 && fixture.running == 0);
    return 1;
}

static int test_setsignal_serial_orders(void)
{
    struct fixture first;
    struct fixture second;
    struct p4_signal_effect effect;

    fixture_init(&first, 0);
    first.task.sig_received = UINT32_C(0x01);
    effect = p4_signal_set_locked(&first.task, UINT32_C(0x01), 0);
    CHECK(effect.returned_signals == UINT32_C(0x01));
    CHECK(fixture_apply_effect(&first, effect));
    effect = p4_signal_raise_locked(&first.task, 0, UINT32_C(0x01), 0);
    CHECK(fixture_apply_effect(&first, effect));
    CHECK(first.task.sig_received == UINT32_C(0x01));

    fixture_init(&second, 0);
    second.task.sig_received = UINT32_C(0x01);
    effect = p4_signal_raise_locked(&second.task, 0, UINT32_C(0x01), 0);
    CHECK(fixture_apply_effect(&second, effect));
    effect = p4_signal_set_locked(&second.task, UINT32_C(0x01), 0);
    CHECK(effect.returned_signals == UINT32_C(0x01));
    CHECK(fixture_apply_effect(&second, effect));
    CHECK(second.task.sig_received == 0);
    CHECK(fixture_invariant(&first) && fixture_invariant(&second));
    return 1;
}

static int test_setexcept_signal_orders(void)
{
    struct fixture first;
    struct fixture second;
    struct p4_signal_effect effect;

    fixture_init(&first, 0);
    effect = p4_signal_set_except_locked(&first.task,
        UINT32_C(0x20), UINT32_C(0x20), 0);
    CHECK(effect.returned_signals == 0);
    CHECK(effect.request_reschedule == 0);
    CHECK(fixture_apply_effect(&first, effect));
    effect = p4_signal_raise_locked(&first.task, 0, UINT32_C(0x20), 1);
    CHECK(effect.request_reschedule == 1);
    CHECK(effect.reschedule_hart == 0);
    CHECK((first.task.flags & first.task.exception_flag_mask) != 0);
    CHECK(fixture_apply_effect(&first, effect));

    fixture_init(&second, 0);
    effect = p4_signal_raise_locked(&second.task, 0, UINT32_C(0x20), 1);
    CHECK(effect.request_reschedule == 0);
    CHECK(fixture_apply_effect(&second, effect));
    effect = p4_signal_set_except_locked(&second.task,
        UINT32_C(0x20), UINT32_C(0x20), 1);
    CHECK(effect.returned_signals == 0);
    CHECK(effect.request_reschedule == 1);
    CHECK(effect.reschedule_hart == 0);
    CHECK(effect.remote_reschedule == 1);
    CHECK((second.task.flags & second.task.exception_flag_mask) != 0);
    CHECK(fixture_apply_effect(&second, effect));

    CHECK(first.task.sig_received == second.task.sig_received);
    CHECK(first.task.sig_except == second.task.sig_except);
    CHECK(first.task.flags == second.task.flags);
    return 1;
}

static int test_exception_callback_reentry(void)
{
    struct fixture fixture;
    struct p4_signal_effect effect;
    uint32_t raised;
    uint32_t pending;

    fixture_init(&fixture, 0);
    fixture.task.sig_received = UINT32_C(0x01);
    fixture.task.sig_except = UINT32_C(0x01);
    fixture.task.flags |= fixture.task.exception_flag_mask;

    raised = p4_signal_exception_take_locked(&fixture.task);
    CHECK(raised == UINT32_C(0x01));
    CHECK(fixture.task.sig_received == 0);
    CHECK(fixture.task.sig_except == 0);
    CHECK((fixture.task.flags & fixture.task.exception_flag_mask) == 0);

    /*
     * User exception code runs with the task lock released. Reentrant Exec
     * calls enable bit 2, then Signal sets it before the callback returns.
     */
    effect = p4_signal_set_except_locked(&fixture.task,
        UINT32_C(0x02), UINT32_C(0x02), 0);
    CHECK(fixture_apply_effect(&fixture, effect));
    effect = p4_signal_raise_locked(&fixture.task, 0, UINT32_C(0x02), 1);
    CHECK(effect.request_reschedule == 1);
    CHECK(fixture_apply_effect(&fixture, effect));

    pending = p4_signal_exception_merge_locked(&fixture.task,
        UINT32_C(0x01));
    CHECK(pending == UINT32_C(0x02));
    CHECK(fixture.task.sig_received == UINT32_C(0x02));
    CHECK((fixture.task.flags & fixture.task.exception_flag_mask) != 0);

    raised = p4_signal_exception_take_locked(&fixture.task);
    CHECK(raised == UINT32_C(0x02));
    CHECK(fixture.task.sig_received == 0);
    CHECK(fixture.task.sig_except == UINT32_C(0x01));
    CHECK((fixture.task.flags & fixture.task.exception_flag_mask) == 0);
    return 1;
}

static int test_pending_token_ownership_and_preemption(void)
{
    struct fixture fixture;
    struct p4_signal_effect effect;
    int other_task;

    fixture_init(&fixture, 0);
    fixture.wait_pending = &other_task;
    effect = p4_signal_switch_commit_locked(&fixture.task,
        &fixture.wait_pending, &fixture);
    CHECK(effect.status == P4_SIGNAL_STATUS_PENDING_OWNER_MISMATCH);
    CHECK(fixture.wait_pending == &other_task);
    CHECK(fixture.task.owner == P4_SIGNAL_OWNER_RUNNING);
    CHECK(fixture.queue_moves == 0);

    fixture.wait_pending = 0;
    effect = p4_signal_switch_commit_locked(&fixture.task,
        &fixture.wait_pending, &fixture);
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.queue_transition == P4_SIGNAL_QUEUE_RUNNING_TO_READY);
    CHECK(fixture_apply_effect(&fixture, effect));
    CHECK(fixture.task.owner == P4_SIGNAL_OWNER_READY);
    CHECK(fixture.queue_moves == 1);
    effect = p4_signal_switch_commit_locked(&fixture.task,
        &fixture.wait_pending, &fixture);
    CHECK(effect.status == P4_SIGNAL_STATUS_INVALID_STATE);
    CHECK(fixture.queue_moves == 1);
    return 1;
}

static int test_exception_operations_reject_nonlive_tasks(void)
{
    struct p4_signal_task_state state;
    struct p4_signal_task_state before;
    uint32_t raised;
    uint32_t pending;

    memset(&state, 0, sizeof(state));
    state.sig_received = UINT32_C(0x05);
    state.sig_wait = UINT32_C(0x04);
    state.sig_except = UINT32_C(0x01);
    state.flags = UINT8_C(0x84);
    state.exception_flag_mask = UINT8_C(0x04);

    state.owner = P4_SIGNAL_OWNER_INVALID;
    before = state;
    raised = p4_signal_exception_take_locked(&state);
    CHECK(raised == 0);
    pending = p4_signal_exception_merge_locked(&state, UINT32_C(0x02));
    CHECK(pending == 0);
    CHECK(state.sig_received == before.sig_received);
    CHECK(state.sig_wait == before.sig_wait);
    CHECK(state.sig_except == before.sig_except);
    CHECK(state.flags == before.flags);
    CHECK(state.exception_flag_mask == before.exception_flag_mask);
    CHECK(state.owner == before.owner);

    state.owner = P4_SIGNAL_OWNER_REMOVED;
    before = state;
    raised = p4_signal_exception_take_locked(&state);
    CHECK(raised == 0);
    pending = p4_signal_exception_merge_locked(&state, UINT32_C(0x02));
    CHECK(pending == 0);
    CHECK(state.sig_received == before.sig_received);
    CHECK(state.sig_wait == before.sig_wait);
    CHECK(state.sig_except == before.sig_except);
    CHECK(state.flags == before.flags);
    CHECK(state.exception_flag_mask == before.exception_flag_mask);
    CHECK(state.owner == before.owner);
    return 1;
}

struct concurrent_context
{
    struct p4_signal_task_state *state;
    pthread_mutex_t *lock;
    int failed;
};

static void *setsignal_thread(void *argument)
{
    struct concurrent_context *context =
        (struct concurrent_context *)argument;
    unsigned int i;

    for (i = 0; i < 12000; ++i)
    {
        struct p4_signal_effect effect;
        (void)pthread_mutex_lock(context->lock);
        effect = p4_signal_set_locked(context->state,
            UINT32_C(0x000000ff), UINT32_C(0x0000005a));
        if (effect.status != P4_SIGNAL_STATUS_OK ||
            effect.queue_transition != P4_SIGNAL_QUEUE_NONE)
            context->failed = 1;
        (void)pthread_mutex_unlock(context->lock);
    }
    return 0;
}

static void *signal_thread(void *argument)
{
    struct concurrent_context *context =
        (struct concurrent_context *)argument;
    unsigned int i;

    for (i = 0; i < 12000; ++i)
    {
        struct p4_signal_effect effect;
        (void)pthread_mutex_lock(context->lock);
        effect = p4_signal_raise_locked(context->state, 0,
            UINT32_C(0x80000000), 0);
        if (effect.status != P4_SIGNAL_STATUS_OK ||
            effect.queue_transition != P4_SIGNAL_QUEUE_NONE)
            context->failed = 1;
        (void)pthread_mutex_unlock(context->lock);
    }
    return 0;
}

static int test_external_lock_serializes_field_updates(void)
{
    struct p4_signal_task_state state;
    struct concurrent_context context;
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_t setter;
    pthread_t signaler;

    memset(&state, 0, sizeof(state));
    state.owner = P4_SIGNAL_OWNER_RUNNING;
    state.exception_flag_mask = UINT8_C(0x04);
    context.state = &state;
    context.lock = &lock;
    context.failed = 0;

    CHECK(pthread_create(&setter, 0, setsignal_thread, &context) == 0);
    CHECK(pthread_create(&signaler, 0, signal_thread, &context) == 0);
    CHECK(pthread_join(setter, 0) == 0);
    CHECK(pthread_join(signaler, 0) == 0);
    CHECK(context.failed == 0);
    CHECK((state.sig_received & UINT32_C(0x000000ff)) ==
          UINT32_C(0x0000005a));
    CHECK((state.sig_received & UINT32_C(0x80000000)) != 0);
    CHECK(pthread_mutex_destroy(&lock) == 0);
    return 1;
}

int main(void)
{
    int ok = 1;

    ok &= test_signal_before_wait_arm();
    ok &= test_signal_between_arm_and_switch_commit();
    ok &= test_signal_after_wait_commit();
    ok &= test_stale_wait_mask_requires_live_token();
    ok &= test_armed_match_and_exception_poke();
    ok &= test_repeated_wait_arm_is_nonmutating();
    ok &= test_zero_mask_and_exception_wakeup();
    ok &= test_setsignal_does_not_raise_exception();
    ok &= test_duplicate_signal_wakes_once();
    ok &= test_setsignal_serial_orders();
    ok &= test_setexcept_signal_orders();
    ok &= test_exception_callback_reentry();
    ok &= test_pending_token_ownership_and_preemption();
    ok &= test_exception_operations_reject_nonlive_tasks();
    ok &= test_external_lock_serializes_field_updates();

    if (!ok)
        return 1;

    printf("P4 signal protocol checks: %lu passed\n", checks);
    return 0;
}
