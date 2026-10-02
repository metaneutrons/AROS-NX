/* Production Exec secondary entry candidate. Not a diagnostic worker entry.
 * Preparation/READY/GO do not release hardware; the launch controller must
 * qualify shared visibility and publish online only after the matching ACK. */
#ifndef P4_SECONDARY_EXEC_H
#define P4_SECONDARY_EXEC_H

#define P4_SECONDARY_EXEC_PREPARED 1
#define P4_SECONDARY_EXEC_GO 2
#define P4_SECONDARY_EXEC_READY 1
#define P4_SECONDARY_EXEC_FAILED 2
#define P4_SECONDARY_EXEC_STACK_BYTES 16384

#ifndef __ASSEMBLER__
#include <stdint.h>
struct ExecBase;
struct KernelBase;

/* Hart0 writes command, hart1 writes reply; independent cache lines.
 * All runtime accesses to these arrays use the uncached SRAM alias. */
struct p4_secondary_exec_exchange
{
    uint32_t command[16]; /* state, generation, SysBase, KernelBase */
    uint32_t reply[16];   /* state, generation, hart, cause, pc, ISR top, tp */
} __attribute__((aligned(64)));
extern struct p4_secondary_exec_exchange __p4_secondary_exec_exchange;
extern unsigned char __p4_secondary_exec_entry[];
extern unsigned char __p4_secondary_exec_entry_end[];

/* Primary only, MIE clear, retained Disable nesting, worker READY, secondary
 * reset/clock-held. One launch generation per boot; refusal never releases it.
 * No callsite is installed until public synchronization/lifecycle is complete. */
int krnP4SecondaryExecPrepare(uint32_t, struct ExecBase *, struct KernelBase *);
int krnP4SecondaryExecReady(uint32_t);
/* Online publication precedes GO. A matching READY alone is not CPU2 proof. */
int krnP4SecondaryExecGo(uint32_t);
int krnP4SecondaryExecBootstrap(uint32_t, struct ExecBase *, struct KernelBase *);
void krnP4SecondaryExecDispatch(struct ExecBase *) __attribute__((noreturn));
#endif
#endif
