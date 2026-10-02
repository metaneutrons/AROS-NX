#ifndef P4_ETASK_GATE_H
#define P4_ETASK_GATE_H
#include <stdint.h>
struct ExecBase;
/* Once during primary Exec preparation, MIE clear and hart1 held. No Exec
 * calls are permitted here because Exec vectors do not yet exist. */
int krnP4ETaskGatePrepare(void);
/* One IRQ-safe metadata/identity transaction, not a Task-storage pin.
 * Nonrecursive. Never acquire Task/queue/registry locks or call out while
 * held. Allocation, frees, Task pins and notifications belong outside it.
 * Returned token records caller MIE; unlock restores only that bit. */
uint32_t krnP4ETaskGateLock(struct ExecBase *base);
void krnP4ETaskGateUnlock(uint32_t token);
#endif
