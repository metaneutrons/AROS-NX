#ifndef P4_TASKRESOURCE_BOOTSTRAP_H
#define P4_TASKRESOURCE_BOOTSTRAP_H
struct ExecBase;
/* Caller must hold Disable through snapshot/publication. Read-only authority
 * check: fails for an online/released secondary or an already published base.
 * Briefly acquires the lifetime gate for the conservative registry check;
 * does not allocate, restore the caller's IRQ mask, or release a CPU. */
int krnP4TaskResourceBootstrapIsolated(struct ExecBase *);
/* Same hardware isolation plus one lifetime-gated final transaction. Trusted
 * callback contract is defined by krnP4TaskResourceBootstrapApply. */
int krnP4TaskResourceBootstrapTransaction(struct ExecBase *,
    int (*)(void *), void *);
#endif
