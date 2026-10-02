/* Actual-source host fixture for the P4 callback escape primitive. */
#define P4_CALLBACK_ESCAPE_TEST 1
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Apple's host Clang rejects __builtin_setjmp/__builtin_longjmp for x86_64.
 * Keep the production helper body, but map those call sites to the host's
 * native setjmp/longjmp for this executable fixture. This checks the real
 * wrapper/escape control flow, not the RV32 compiler builtin ABI. */
typedef jmp_buf p4_host_jmp_buf;
#define intptr_t p4_host_jmp_buf
#define __builtin_setjmp(buffer) \
    setjmp(*(p4_host_jmp_buf *)(void *)(buffer))
#define __builtin_longjmp(buffer, value) \
    longjmp(*(p4_host_jmp_buf *)(void *)(buffer), (value))

struct ExecBase { unsigned identity; };
struct Task { unsigned identity; };
struct p4_cpu_local
{
    unsigned trap_depth;
    struct Task *current_task;
};
struct p4_exec_runtime
{
    uint32_t hart_id;
    struct p4_cpu_local *cpu_local;
};

#define MSTATUS_MIE UINT32_C(8)
#define P4_PUBLIC_SPIN_BAD_STATE 7

static struct ExecBase bases[2];
static struct Task tasks[2];
static struct Task other_task;
static struct p4_cpu_local locals[2];
static struct p4_exec_runtime runtimes[2];
static unsigned int active_hart;
static uint32_t mock_mstatus;
static unsigned int fatal_calls;
static unsigned int fatal_code;
static unsigned int dispatch_calls;
static unsigned int callback_calls;
static unsigned int post_transfer_calls;
static unsigned int service_owner_calls;
static int service_owner_result;
static int harness_armed;
static jmp_buf harness_jump;

static struct p4_exec_runtime *p4_exec_runtime_current(
    struct ExecBase *sys_base);
static void mock_fatal(unsigned int code) __attribute__((noreturn));
void KrnCli(void);
void KrnDispatch(void);
static int krnP4TaskRecordServiceOwnerRunning(
    struct Task *task, unsigned int hart, struct ExecBase *sys_base);

#define csr_read(reg) (mock_mstatus)
#define krnP4PublicSpinFatal(code) mock_fatal((unsigned int)(code))

#include "../p4_callback_escape.c"
#undef intptr_t

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        fprintf(stderr, "FAIL %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
    } \
} while (0)

static unsigned int checks;
static unsigned int failures;

static struct p4_exec_runtime *p4_exec_runtime_current(
    struct ExecBase *sys_base)
{
    CHECK(active_hart < 2u);
    CHECK(sys_base == &bases[active_hart]);
    return active_hart < 2u ? &runtimes[active_hart] : NULL;
}

static void mock_fatal(unsigned int code)
{
    ++fatal_calls;
    fatal_code = code;
    if (harness_armed)
        longjmp(harness_jump, 2);
    abort();
}

void KrnCli(void)
{
    mock_mstatus &= ~MSTATUS_MIE;
}

void KrnDispatch(void)
{
    ++dispatch_calls;
    if (harness_armed)
        longjmp(harness_jump, 3);
    abort();
}

static int krnP4TaskRecordServiceOwnerRunning(
    struct Task *task, unsigned int hart, struct ExecBase *sys_base)
{
    CHECK(sys_base == &bases[active_hart]);
    CHECK(task == &tasks[active_hart]);
    CHECK(hart == active_hart);
    ++service_owner_calls;
    return service_owner_result;
}

struct callback_args
{
    unsigned int called;
    unsigned int escaped;
};

static void ordinary_callback(void *opaque)
{
    struct callback_args *args = opaque;
    CHECK(mock_mstatus == 0);
    ++callback_calls;
    ++args->called;
    mock_mstatus |= MSTATUS_MIE;
}

static void self_remove_callback(void *opaque)
{
    struct callback_args *args = opaque;
    CHECK(mock_mstatus == 0);
    CHECK(krnP4SelfRemoveContextAllowed(&bases[active_hart]) == 1);
    ++callback_calls;
    /* RemTask starts by disabling locally before context validation. */
    KrnCli();
    krnP4SelfRemoveTransfer(&bases[active_hart]);
    ++args->called;
    ++args->escaped;
    ++post_transfer_calls;
}

static void stale_identity_callback(void *opaque)
{
    struct callback_args *args = opaque;
    struct Task *saved = locals[active_hart].current_task;
    locals[active_hart].current_task = &other_task;
    CHECK(krnP4SelfRemoveContextAllowed(&bases[active_hart]) == 0);
    locals[active_hart].current_task = saved;
    ++args->called;
}

static void hart_one_callback(void *opaque)
{
    struct callback_args *args = opaque;
    CHECK(active_hart == 1u);
    CHECK(p4_callback_slots[0] != NULL);
    CHECK(p4_callback_slots[1] != NULL);
    ++args->called;
}

static void nested_hart_callback(void *opaque)
{
    struct callback_args nested = {0, 0};
    struct callback_args *args = opaque;
    unsigned int saved_hart = active_hart;
    int result;

    CHECK(saved_hart == 0u);
    CHECK(p4_callback_slots[0] != NULL);
    active_hart = 1u;
    mock_mstatus = 0;
    result = krnP4RunTaskCallback(
        hart_one_callback, &nested, &bases[1]);
    CHECK(result == 1);
    CHECK(nested.called == 1u);
    CHECK(p4_callback_slots[1] == NULL);
    active_hart = saved_hart;
    mock_mstatus = 0;
    CHECK(p4_callback_slots[0] != NULL);
    ++args->called;
}

static void duplicate_wrapper_callback(void *opaque)
{
    struct callback_args *args = opaque;
    struct callback_args nested = {0, 0};
    (void)krnP4RunTaskCallback(ordinary_callback, &nested,
                               &bases[active_hart]);
    ++args->called;
}

static void reset_fixture(unsigned int hart, unsigned int trap_depth)
{
    unsigned int index;
    active_hart = hart;
    mock_mstatus = 0;
    fatal_calls = 0;
    fatal_code = 0;
    dispatch_calls = 0;
    callback_calls = 0;
    post_transfer_calls = 0;
    service_owner_calls = 0;
    service_owner_result = 0;
    harness_armed = 0;
    for (index = 0; index < 2u; ++index)
    {
        bases[index].identity = index + 1u;
        tasks[index].identity = index + 1u;
        locals[index].trap_depth = index == hart ? trap_depth : 1u;
        locals[index].current_task = &tasks[index];
        runtimes[index].hart_id = index;
        runtimes[index].cpu_local = &locals[index];
        p4_callback_slots[index] = NULL;
    }
    other_task.identity = 99u;
}

static void test_ordinary_callback_and_interrupt_mask(void)
{
    struct callback_args args = {0, 0};
    int result;
    reset_fixture(0u, 1u);
    result = krnP4RunTaskCallback(ordinary_callback, &args, &bases[0]);
    CHECK(result == 1);
    CHECK(args.called == 1u);
    CHECK(callback_calls == 1u);
    CHECK(mock_mstatus == 0);
    CHECK(p4_callback_slots[0] == NULL);
    CHECK(fatal_calls == 0u);
}

static void test_callback_self_remove_uses_actual_builtin_escape(void)
{
    struct callback_args args = {0, 0};
    int result;
    reset_fixture(0u, 1u);
    result = krnP4RunTaskCallback(self_remove_callback, &args, &bases[0]);
    CHECK(result == 0);
    CHECK(callback_calls == 1u);
    CHECK(args.called == 0u && args.escaped == 0u);
    CHECK(post_transfer_calls == 0u);
    CHECK(dispatch_calls == 0u);
    CHECK(fatal_calls == 0u);
    CHECK(mock_mstatus == 0);
    CHECK(p4_callback_slots[0] == NULL);
}

static void test_slots_are_per_hart(void)
{
    struct callback_args args = {0, 0};
    int result;
    reset_fixture(0u, 1u);
    result = krnP4RunTaskCallback(nested_hart_callback, &args, &bases[0]);
    CHECK(result == 1);
    CHECK(args.called == 1u);
    CHECK(p4_callback_slots[0] == NULL);
    CHECK(p4_callback_slots[1] == NULL);
    CHECK(mock_mstatus == 0);
    CHECK(fatal_calls == 0u);
}

static void test_context_and_service_owner_snapshots(void)
{
    reset_fixture(0u, 0u);
    CHECK(krnP4SelfRemoveContextAllowed(&bases[0]) == 1);
    locals[0].trap_depth = 1u;
    CHECK(krnP4SelfRemoveContextAllowed(&bases[0]) == 0);
    locals[0].trap_depth = 2u;
    CHECK(krnP4SelfRemoveContextAllowed(&bases[0]) == 0);
    locals[0].trap_depth = 0u;
    service_owner_result = 1;
    CHECK(krnP4SelfRemoveCurrentRetired(&bases[0]) == 1);
    CHECK(service_owner_calls == 1u);
}

static void test_normal_self_remove_dispatch_path(void)
{
    int result;
    reset_fixture(1u, 0u);
    harness_armed = 1;
    result = setjmp(harness_jump);
    if (result == 0)
        krnP4SelfRemoveTransfer(&bases[1]);
    harness_armed = 0;
    CHECK(result == 3);
    CHECK(dispatch_calls == 1u);
    CHECK(fatal_calls == 0u);
    CHECK(mock_mstatus == 0);
}

static void test_nested_depth_refusal_before_slot_publication(void)
{
    struct callback_args args = {0, 0};
    int result;
    reset_fixture(0u, 2u);
    harness_armed = 1;
    result = setjmp(harness_jump);
    if (result == 0)
        (void)krnP4RunTaskCallback(ordinary_callback, &args, &bases[0]);
    harness_armed = 0;
    CHECK(result == 2);
    CHECK(fatal_calls == 1u);
    CHECK(fatal_code == P4_PUBLIC_SPIN_BAD_STATE);
    CHECK(args.called == 0u);
    CHECK(p4_callback_slots[0] == NULL);
}

static void test_stale_identity_is_not_authorized(void)
{
    struct callback_args args = {0, 0};
    int result;
    reset_fixture(0u, 1u);
    result = krnP4RunTaskCallback(stale_identity_callback, &args, &bases[0]);
    CHECK(result == 1);
    CHECK(args.called == 1u);
    CHECK(locals[0].current_task == &tasks[0]);
    CHECK(p4_callback_slots[0] == NULL);
    CHECK(fatal_calls == 0u);
}

static void test_duplicate_wrapper_fails_closed(void)
{
    struct callback_args args = {0, 0};
    int result;
    reset_fixture(0u, 1u);
    harness_armed = 1;
    result = setjmp(harness_jump);
    if (result == 0)
        (void)krnP4RunTaskCallback(
            duplicate_wrapper_callback, &args, &bases[0]);
    harness_armed = 0;
    CHECK(result == 2);
    CHECK(fatal_calls == 1u);
    CHECK(fatal_code == P4_PUBLIC_SPIN_BAD_STATE);
    CHECK(p4_callback_slots[0] != NULL);
    /* Fatal is terminal in production. Restore test-only private state. */
    p4_callback_slots[0] = NULL;
}

int main(void)
{
    test_ordinary_callback_and_interrupt_mask();
    test_callback_self_remove_uses_actual_builtin_escape();
    test_slots_are_per_hart();
    test_context_and_service_owner_snapshots();
    test_normal_self_remove_dispatch_path();
    test_nested_depth_refusal_before_slot_publication();
    test_stale_identity_is_not_authorized();
    test_duplicate_wrapper_fails_closed();
    if (failures != 0u)
    {
        fprintf(stderr, "FAIL p4_callback_escape checks=%u failures=%u\n",
                checks, failures);
        return EXIT_FAILURE;
    }
    printf("PASS p4_callback_escape checks=%u\n", checks);
    return EXIT_SUCCESS;
}
