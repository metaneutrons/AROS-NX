/* Host tests for the production custom OSAL and TinyUSB FIFO implementation. */
#include "tusb.h"
#include "tusb_os_custom.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jmp_buf fault_env;
static bool fault_armed;
static bool owner_allowed = true;
static const char *last_fault;
static unsigned owner_checks;
static uint32_t fake_millis = 123456;
static uint32_t delayed_millis;

void p4_usb_owner_check(void)
{
    ++owner_checks;
    if (!owner_allowed)
        p4_usb_fault("non-owner stack access");
}

void p4_usb_fault(const char *reason)
{
    last_fault = reason;
    if (fault_armed)
        longjmp(fault_env, 1);
    fprintf(stderr, "unexpected OSAL fatal hook: %s\n", reason);
    exit(EXIT_FAILURE);
}

uint32_t p4_usb_millis(void)
{
    return fake_millis;
}

void p4_usb_delay(uint32_t milliseconds)
{
    p4_usb_owner_check();
    delayed_millis += milliseconds;
}

static void check(bool condition, const char *expression, int line)
{
    if (!condition)
    {
        fprintf(stderr, "test_osal:%d: check failed: %s\n", line, expression);
        exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

typedef void (*fault_action_t)(void *context);

static void expect_fault(const char *expected, fault_action_t action, void *context)
{
    int jumped;

    last_fault = NULL;
    fault_armed = true;
    jumped = setjmp(fault_env);
    if (jumped == 0)
    {
        action(context);
        fault_armed = false;
        CHECK(false && "expected OSAL fatal hook");
    }
    fault_armed = false;
    CHECK(last_fault != NULL);
    CHECK(strcmp(last_fault, expected) == 0);
}

static void action_current_task(void *context)
{
    (void)context;
    (void)osal_task_get_current_handle();
}

static void action_spin_unlock(void *context)
{
    osal_spin_unlock((osal_spinlock_t *)context, false);
}

static void action_spin_deinit(void *context)
{
    osal_spin_deinit((osal_spinlock_t *)context);
}

static void action_semaphore_post(void *context)
{
    osal_semaphore_t semaphore = (osal_semaphore_t)context;
    (void)osal_semaphore_post(semaphore, false);
}

static void action_mutex_unlock(void *context)
{
    osal_mutex_t mutex = (osal_mutex_t)context;
    (void)osal_mutex_unlock(mutex);
}
static void action_mutex_lock(void *context)
{
    (void)osal_mutex_lock((osal_mutex_t)context, OSAL_TIMEOUT_WAIT_FOREVER);
}

typedef struct
{
    osal_queue_t queue;
    uint32_t item;
} queue_send_context_t;

static void action_queue_send(void *context)
{
    queue_send_context_t *send = (queue_send_context_t *)context;
    (void)osal_queue_send(send->queue, &send->item, false);
}

static void test_owner_gate_task_time_and_delay(void)
{
    owner_allowed = false;
    expect_fault("non-owner stack access", action_current_task, NULL);
    owner_allowed = true;
    CHECK(owner_checks > 0);

    CHECK(osal_task_get_current_handle() == (osal_task_handle_t)(uintptr_t)1);
    CHECK(osal_time_millis() == fake_millis);
    osal_task_delay(17);
    CHECK(delayed_millis == 17);
}

static void test_spinlock_fault_hooks(void)
{
    osal_spinlock_t spin = { 0 };

    expect_fault("lock imbalance", action_spin_unlock, &spin);
    CHECK(spin.nesting == 0);

    osal_spin_lock(&spin, false);
    CHECK(spin.nesting == 1);
    expect_fault("held lock", action_spin_deinit, &spin);
    CHECK(spin.nesting == 1);
    osal_spin_unlock(&spin, false);
    CHECK(spin.nesting == 0);
    osal_spin_deinit(&spin);
}

static void test_semaphore_overflow_and_nonblocking_wait(void)
{
    osal_semaphore_def_t semaphore_storage = { .count = UINT16_MAX };
    osal_semaphore_t semaphore = &semaphore_storage;
    osal_mutex_def_t mutex_storage;
    osal_mutex_t mutex;

    expect_fault("semaphore overflow", action_semaphore_post, semaphore);
    CHECK(semaphore->count == UINT16_MAX);

    semaphore_storage.count = 0;
    CHECK(!osal_semaphore_wait(semaphore, OSAL_TIMEOUT_WAIT_FOREVER));
    CHECK(semaphore->count == 0);

    mutex = osal_mutex_create(&mutex_storage);
    CHECK(mutex != NULL && mutex->count == 1);
    expect_fault("mutex imbalance", action_mutex_unlock, mutex);
    CHECK(mutex->count == 1);

    CHECK(osal_mutex_lock(mutex, OSAL_TIMEOUT_NOTIMEOUT));
    CHECK(mutex->count == 0);
    expect_fault("recursive mutex", action_mutex_lock, mutex);
    CHECK(mutex->count == 0);
    CHECK(osal_mutex_unlock(mutex));
    CHECK(mutex->count == 1);
}

static void test_full_queue_rejects_without_mutation(void)
{
    OSAL_QUEUE_DEF(0, queue, 2, uint32_t);
    uint32_t first = 0x12345678;
    uint32_t second = 0x90abcdef;
    uint32_t output = 0;
    uint8_t before_buffer[sizeof(queue_buf)];
    uint16_t before_write;
    uint16_t before_read;
    queue_send_context_t send;

    memset(queue_buf, 0xa5, sizeof(queue_buf));
    send.queue = osal_queue_create(&queue);
    send.item = 0xfeedcafe;
    CHECK(osal_queue_send(send.queue, &first, false));
    CHECK(osal_queue_send(send.queue, &second, false));
    CHECK(tu_fifo_count(&queue.ff) == sizeof(queue_buf));
    CHECK(tu_fifo_remaining(&queue.ff) == 0);

    memcpy(before_buffer, queue_buf, sizeof(before_buffer));
    before_write = queue.ff.wr_idx;
    before_read = queue.ff.rd_idx;
    expect_fault("USB event queue full", action_queue_send, &send);
    CHECK(memcmp(queue_buf, before_buffer, sizeof(before_buffer)) == 0);
    CHECK(queue.ff.wr_idx == before_write);
    CHECK(queue.ff.rd_idx == before_read);
    CHECK(tu_fifo_count(&queue.ff) == sizeof(queue_buf));

    CHECK(osal_queue_receive(send.queue, &output, OSAL_TIMEOUT_NOTIMEOUT));
    CHECK(output == first);
    CHECK(osal_queue_receive(send.queue, &output, OSAL_TIMEOUT_NOTIMEOUT));
    CHECK(output == second);
    CHECK(osal_queue_empty(send.queue));

    output = 0xaaaaaaaa;
    CHECK(!osal_queue_receive(send.queue, &output, OSAL_TIMEOUT_WAIT_FOREVER));
    CHECK(output == 0xaaaaaaaa);
}

int main(void)
{
    test_owner_gate_task_time_and_delay();
    test_spinlock_fault_hooks();
    test_semaphore_overflow_and_nonblocking_wait();
    test_full_queue_rejects_without_mutation();
    puts("test_osal: pass");
    return EXIT_SUCCESS;
}
