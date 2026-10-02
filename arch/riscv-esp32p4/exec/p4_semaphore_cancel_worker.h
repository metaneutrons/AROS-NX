#ifndef P4_SEMAPHORE_CANCEL_WORKER_H
#define P4_SEMAPHORE_CANCEL_WORKER_H
struct Task;
struct ExecBase;
/* One bounded operation-only pass by the protected current ServiceTask.
 * Retry bindings and resource-drop hooks retain blocked-pass wake interest.
 * Return1 only for node/finish progress, not claims
 * or claim releases; a blocked pass must not spin the service drain loop. */
int Exec_P4SemaphoreCancellationPass(struct Task *, struct ExecBase *);
#endif
