/* SMP dispatcher; caller executes on the owner's private ISR stack. */
#ifndef P4_DISPATCH_H
#define P4_DISPATCH_H
struct Task;
struct ExecBase;
/* Local IRQs masked, outgoing context fully committed and current_task NULL.
 * Returns a lifetime-owned RUN task, or NULL after a bounded empty/retry scan.
 * The caller installs its saved CPU/FPU context before returning from trap.
 * No Task/list lock is held across context installation or launch callbacks. */
struct Task *krnP4Dispatch(struct ExecBase *);
#endif
