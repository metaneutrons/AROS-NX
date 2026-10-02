/* Deterministic host model tests for p4_task_lifetime.c. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../p4_task_lifetime.c"

static pthread_mutex_t lifetime_gate = PTHREAD_MUTEX_INITIALIZER;
static unsigned checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 0; \
    } \
} while (0)

#define LOCKED_CALL(expression) __extension__ ({ \
    int locked_result; \
    pthread_mutex_lock(&lifetime_gate); \
    locked_result = (expression); \
    pthread_mutex_unlock(&lifetime_gate); \
    locked_result; \
})

static int leave_hart0(struct p4_task_lifetime *lifetime)
{
    return p4_task_lifetime_leave_running_locked(lifetime, 0);
}

static struct p4_task_lifetime snapshot(struct p4_task_lifetime *lifetime)
{
    struct p4_task_lifetime copy;

    pthread_mutex_lock(&lifetime_gate);
    copy = *lifetime;
    pthread_mutex_unlock(&lifetime_gate);
    return copy;
}

static int matches_snapshot(struct p4_task_lifetime *lifetime,
                            const struct p4_task_lifetime *expected)
{
    struct p4_task_lifetime current = snapshot(lifetime);
    return memcmp(expected, &current, sizeof(current)) == 0;
}

static int unchanged_after_refusal(struct p4_task_lifetime *lifetime,
                                   int (*operation)(struct p4_task_lifetime *))
{
    struct p4_task_lifetime before = snapshot(lifetime);
    int result;

    pthread_mutex_lock(&lifetime_gate);
    result = operation(lifetime);
    pthread_mutex_unlock(&lifetime_gate);
    {
        struct p4_task_lifetime after = snapshot(lifetime);
        return !result && memcmp(&before, &after, sizeof(before)) == 0;
    }
}

static int test_foreign_wake_then_delayed_notification(void)
{
    struct p4_task_lifetime life;
    int grant_published = 0;
    int foreign_wake = 0;
    int delayed_signal = 0;

    p4_task_lifetime_init(&life);
    CHECK(LOCKED_CALL(p4_task_lifetime_enter_running_locked(&life, 0)));
    CHECK(LOCKED_CALL(p4_task_lifetime_pin_locked(&life)));

    /* A grant was published. A foreign SIGF_SINGLE wakes the waiter first;
     * it retires itself while the delayed notifier still owns its pin. */
    grant_published = 1;
    foreign_wake = grant_published;
    CHECK(foreign_wake);
    CHECK(LOCKED_CALL(p4_task_lifetime_retire_locked(&life)));
    CHECK(snapshot(&life).notification_pins == 1);
    CHECK(!LOCKED_CALL(p4_task_lifetime_reclaimable_locked(&life)));
    CHECK(!LOCKED_CALL(p4_task_lifetime_claim_reclaim_locked(&life)));

    /* The notifier may now Signal(task) and then drop its pin.  Running
     * context ownership independently prevents reclamation until switch-out. */
    delayed_signal = foreign_wake;
    CHECK(delayed_signal);
    CHECK(LOCKED_CALL(p4_task_lifetime_unpin_locked(&life)));
    CHECK(!LOCKED_CALL(p4_task_lifetime_reclaimable_locked(&life)));
    CHECK(LOCKED_CALL(p4_task_lifetime_leave_running_locked(&life, 0)));
    CHECK(LOCKED_CALL(p4_task_lifetime_reclaimable_locked(&life)));
    CHECK(LOCKED_CALL(p4_task_lifetime_claim_reclaim_locked(&life)));
    CHECK(!LOCKED_CALL(p4_task_lifetime_claim_reclaim_locked(&life)));
    return 1;
}

static int test_notify_and_switch_orders(void)
{
    struct p4_task_lifetime life;

    /* Notification and unpin can precede retirement, but current execution
     * still owns the allocation until the scheduler completes the switch. */
    p4_task_lifetime_init(&life);
    CHECK(LOCKED_CALL(p4_task_lifetime_enter_running_locked(&life, 1)));
    CHECK(LOCKED_CALL(p4_task_lifetime_pin_locked(&life)));
    CHECK(LOCKED_CALL(p4_task_lifetime_unpin_locked(&life)));
    CHECK(LOCKED_CALL(p4_task_lifetime_retire_locked(&life)));
    CHECK(!LOCKED_CALL(p4_task_lifetime_reclaimable_locked(&life)));
    CHECK(LOCKED_CALL(p4_task_lifetime_leave_running_locked(&life, 1)));
    CHECK(LOCKED_CALL(p4_task_lifetime_claim_reclaim_locked(&life)));

    /* A scheduler switch may finish before a delayed notification; the pin
     * alone then keeps the retired allocation alive. */
    p4_task_lifetime_init(&life);
    CHECK(LOCKED_CALL(p4_task_lifetime_enter_running_locked(&life, 0)));
    CHECK(LOCKED_CALL(p4_task_lifetime_pin_locked(&life)));
    CHECK(LOCKED_CALL(p4_task_lifetime_retire_locked(&life)));
    CHECK(LOCKED_CALL(p4_task_lifetime_leave_running_locked(&life, 0)));
    CHECK(!LOCKED_CALL(p4_task_lifetime_reclaimable_locked(&life)));
    CHECK(LOCKED_CALL(p4_task_lifetime_unpin_locked(&life)));
    CHECK(LOCKED_CALL(p4_task_lifetime_claim_reclaim_locked(&life)));
    return 1;
}

static int test_self_retirement_is_deferred(void)
{
    struct p4_task_lifetime life;

    p4_task_lifetime_init(&life);
    CHECK(LOCKED_CALL(p4_task_lifetime_enter_running_locked(&life, 0)));
    CHECK(LOCKED_CALL(p4_task_lifetime_pin_locked(&life)));

    /* Retire only marks/removes the object; it never waits for the caller's
     * own running context or its interrupted notification pin. */
    CHECK(LOCKED_CALL(p4_task_lifetime_retire_locked(&life)));
    CHECK(!LOCKED_CALL(p4_task_lifetime_reclaimable_locked(&life)));
    CHECK(!LOCKED_CALL(p4_task_lifetime_pin_locked(&life)));
    CHECK(LOCKED_CALL(p4_task_lifetime_unpin_locked(&life)));
    CHECK(!LOCKED_CALL(p4_task_lifetime_claim_reclaim_locked(&life)));
    CHECK(LOCKED_CALL(p4_task_lifetime_leave_running_locked(&life, 0)));

    /* A later service, not RemTask/self-retirement, owns reclamation. */
    CHECK(LOCKED_CALL(p4_task_lifetime_claim_reclaim_locked(&life)));
    return 1;
}

static int test_refusal_paths_do_not_mutate(void)
{
    struct p4_task_lifetime life;
    struct p4_task_lifetime before;

    p4_task_lifetime_init(&life);
    CHECK(unchanged_after_refusal(&life,
                                  p4_task_lifetime_unpin_locked));
    CHECK(LOCKED_CALL(p4_task_lifetime_pin_locked(&life)));
    CHECK(LOCKED_CALL(p4_task_lifetime_retire_locked(&life)));
    CHECK(unchanged_after_refusal(&life,
                                  p4_task_lifetime_retire_locked));
    CHECK(unchanged_after_refusal(&life,
                                  p4_task_lifetime_pin_locked));
    CHECK(unchanged_after_refusal(&life,
                                  p4_task_lifetime_claim_reclaim_locked));
    CHECK(LOCKED_CALL(p4_task_lifetime_unpin_locked(&life)));

    /* A successful claim is one-shot; later operations refuse without
     * changing the claimed record. */
    CHECK(LOCKED_CALL(p4_task_lifetime_claim_reclaim_locked(&life)));
    CHECK(unchanged_after_refusal(&life,
                                  p4_task_lifetime_retire_locked));
    CHECK(unchanged_after_refusal(&life,
                                  p4_task_lifetime_pin_locked));
    CHECK(unchanged_after_refusal(&life,
                                  p4_task_lifetime_claim_reclaim_locked));

    p4_task_lifetime_init(&life);
    pthread_mutex_lock(&lifetime_gate);
    life.notification_pins = UINT32_MAX;
    before = life;
    pthread_mutex_unlock(&lifetime_gate);
    CHECK(unchanged_after_refusal(&life,
                                  p4_task_lifetime_pin_locked));
    CHECK(matches_snapshot(&life, &before));

    p4_task_lifetime_init(&life);
    CHECK(LOCKED_CALL(p4_task_lifetime_enter_running_locked(&life, 1)));
    CHECK(unchanged_after_refusal(&life,
                                  leave_hart0));
    pthread_mutex_lock(&lifetime_gate);
    before = life;
    pthread_mutex_unlock(&lifetime_gate);
    CHECK(!LOCKED_CALL(p4_task_lifetime_enter_running_locked(&life, 0)));
    CHECK(matches_snapshot(&life, &before));
    CHECK(!LOCKED_CALL(p4_task_lifetime_enter_running_locked(
        &life, P4_TASK_LIFETIME_HARTS)));
    CHECK(matches_snapshot(&life, &before));
    return 1;
}

int main(void)
{
    if (!test_foreign_wake_then_delayed_notification() ||
        !test_notify_and_switch_orders() ||
        !test_self_retirement_is_deferred() ||
        !test_refusal_paths_do_not_mutate())
        return 1;

    printf("PASS: %u P4 task-lifetime protocol checks\n", checks);
    return 0;
}
