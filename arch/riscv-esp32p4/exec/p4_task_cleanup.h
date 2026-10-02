#ifndef P4_TASK_CLEANUP_H
#define P4_TASK_CLEANUP_H
struct ExecBase;
/* Trusted housekeeper only. Its continuation must be non-retirable while it
 * owns a retained or detached record. Claims before any Task dereference,
 * finishes every Task/context access before freeing entries, and frees the
 * private record only after completion. SMP retries postfree records without
 * rereading Task; blocked records do not prevent claiming new cleanup work.
 * The P4 SMP ServiceTask selects this consumer. Metadata topology and
 * external-removal completion must be bound before enabling SMP runtime. */
int krnP4CleanupOneRetiredTask(struct ExecBase *base);
#endif
