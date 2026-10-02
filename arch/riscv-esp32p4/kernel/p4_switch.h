/* Outgoing P4 SMP context handoff; full lifecycle remains configure-gated. */
#ifndef P4_SWITCH_H
#define P4_SWITCH_H
struct ExceptionContext;
struct ExecBase;
/* Outermost private ISR frame, local MIE clear, current unique RUN owner.
 * Returns with no old Task/context access remaining and current_task cleared.
 * Removal/discard callers require their separate no-save transaction. */
void krnP4Switch(struct ExceptionContext *, struct ExecBase *);
#endif
