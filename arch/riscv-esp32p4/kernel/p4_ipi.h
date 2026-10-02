/* Private, bounded SMP scheduler mailbox. No Task identities are stored. */
#ifndef P4_IPI_H
#define P4_IPI_H
#include <stdint.h>

#define P4_IPI_LINE 22u
struct ExecBase;
struct p4_signal_effect;

struct p4_ipi_batch
{
    uint32_t schedule_flags;
    uint32_t ticks;
};

/* Primary, MIE clear, secondary reset-held, after atomic service preparation. */
int krnP4IpiPrepare(void);
/* Owner only, MIE clear, after local runtime/mtvec/CLIC setup. Relatches work
 * published before arm. Secondary additionally requires retained outer
 * Disable nesting and the protected cleanup-worker/mask ready ACK; the query
 * precedes the IPI gate. CPU-count publication must follow arm acknowledgement. */
void krnP4IpiArm(struct ExecBase *SysBase);
/* Owner only, in a trap with MIE clear. Acknowledge before consuming work. */
struct p4_ipi_batch krnP4IpiDrain(struct ExecBase *SysBase);
/* Owning trap only; updates no global VBlank/softint state or Task field. */
void krnP4IpiApplyBatch(const struct p4_ipi_batch *batch,
                       struct ExecBase *SysBase);
/* Primary timer only. Unarmed destinations receive no historic tick debt.
 * Arm readiness is distinct from public CPU-count publication. */
void krnP4IpiPostTick(uint32_t destination, struct ExecBase *SysBase);
void krnP4SignalPoke(const struct p4_signal_effect *effect,
                     struct ExecBase *SysBase);
/* Asynchronous both-hart doorbell only. Never accesses a Task or delivers
 * Signal inline. A caller still owning outer Task/queue locks MUST retain its
 * outer Disable nesting until releasing them: this helper balances its own
 * Disable/Enable and can expose a self-IPI when no outer mask remains.
 * Unarmed destinations retain flags without a latch write; either admitted
 * hart can consume the gate-serialized wake at a safe boundary. */
void krnP4CleanupPoke(struct ExecBase *SysBase);
/* Copied RUN-owner destination only; never reads the retiring Task. Same
 * outer-Disable contract as CleanupPoke when Task/queue locks remain held.
 * Requests an outer trap; it cannot force a hart with MIE masked to drain. */
void krnP4RetiredOwnerPoke(uint32_t destination, struct ExecBase *SysBase);
#endif
