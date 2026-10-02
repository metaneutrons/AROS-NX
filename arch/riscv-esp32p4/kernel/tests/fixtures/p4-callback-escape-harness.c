/* Execute actual compiler-generated RV32 callback unwind, not P4 hardware. */
#include <stddef.h>
#include <stdint.h>

struct Task { unsigned int marker; };
struct ExecBase { unsigned int marker; };
struct p4_cpu_local { struct Task *current_task; unsigned int trap_depth; };
struct p4_exec_runtime { unsigned int hart_id; struct p4_cpu_local *cpu_local; };
static struct Task task;
static struct ExecBase base;
static struct p4_cpu_local local = { &task, 1 };
static struct p4_exec_runtime runtime = { 0, &local };
static unsigned int checks;
static unsigned int returned;
static unsigned int expected_dispatch;
#define MSTATUS_MIE UINT32_C(8)
#define P4_PUBLIC_SPIN_BAD_STATE 9
#define csr_read(reg) ({ uintptr_t value; __asm__ volatile("csrr %0," #reg : "=r"(value)); value; })

static void finish(uint32_t value) __attribute__((noreturn));
static void finish(uint32_t value)
{
    *(volatile uint32_t *)UINT32_C(0x100000) = value;
    for (;;) __asm__ volatile("wfi");
}
static void check(int condition)
{
    ++checks;
    if (!condition) finish(UINT32_C(0x13333));
}
static struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *arg)
{
    check(arg == &base);
    return &runtime;
}
static void KrnCli(void) { __asm__ volatile("csrci mstatus,8" ::: "memory"); }
static void krnP4PublicSpinFatal(int code) __attribute__((noreturn));
static void krnP4PublicSpinFatal(int code)
{
    (void)code;
    finish(UINT32_C(0x13333));
}
static int krnP4TaskRecordServiceOwnerRunning(struct Task *identity,
    unsigned int hart, struct ExecBase *arg)
{
    check(identity == &task && hart == runtime.hart_id && arg == &base);
    return 1;
}
static void KrnDispatch(void)
{
    check(expected_dispatch == 1 && checks >= 30);
    finish(UINT32_C(0x5555));
}
#define P4_CALLBACK_ESCAPE_TEST 1
#include "../../p4_callback_escape.c"

static void normal_callback(void *arg)
{
    check(arg == &returned);
    check(krnP4SelfRemoveContextAllowed(&base) == 1);
    ++returned;
}
void probe_unwind(void *arg)
{
    check(arg == NULL);
    check(krnP4SelfRemoveContextAllowed(&base) == 1);
    check(krnP4SelfRemoveCurrentRetired(&base) == 1);
    /* Simulate a callback that modifies the ABI's callee-saved registers
     * before abandoning its frame. This function's epilogue never runs. */
    __asm__ volatile(
        "li s1,0x44444444\nli s11,0x66666666\n"
        "fmv.w.x fs0,s1\nfmv.w.x fs11,s11\n"
        ::: "s1", "s11", "fs0", "fs11");
    krnP4SelfRemoveTransfer(&base);
}
extern int register_probe(void (*)(void *), void *, struct ExecBase *);
int fixture_main(void)
{
    unsigned int hart;
    KrnCli();
    check(krnP4SelfRemoveContextAllowed(&base) == 0);
    local.trap_depth = 2;
    check(krnP4SelfRemoveContextAllowed(&base) == 0);
    local.trap_depth = 1;
    for (hart = 0; hart < 2; ++hart)
    {
        runtime.hart_id = hart;
        check(krnP4RunTaskCallback(normal_callback, &returned, &base) == 1);
        check(returned == hart + 1);
        check(p4_callback_slots[hart] == NULL);
        check(register_probe(probe_unwind, NULL, &base) == 0);
        check(p4_callback_slots[hart] == NULL);
        check(krnP4SelfRemoveContextAllowed(&base) == 0);
    }
    local.trap_depth = 0;
    check(krnP4SelfRemoveContextAllowed(&base) == 1);
    expected_dispatch = 1;
    krnP4SelfRemoveTransfer(&base);
}
