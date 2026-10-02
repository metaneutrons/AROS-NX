#ifndef P4_RETIRED_TRAP_H
#define P4_RETIRED_TRAP_H
struct ExecBase;
struct ExceptionContext;
/* Outermost trap, MIE clear, no Task/list lock held. Returns one only after
 * discarding a retired owner and installing a new task's return frame. */
int krnP4RetiredTrap(struct ExceptionContext *, struct ExecBase *);
#endif
