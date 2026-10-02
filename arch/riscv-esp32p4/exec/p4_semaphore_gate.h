/* Private bounded gate for P4 SMP SignalSemaphore state transitions. */
#ifndef P4_SEMAPHORE_GATE_H
#define P4_SEMAPHORE_GATE_H

struct ExecBase;
struct SignalSemaphore;

/* The semaphore must already be initialized, and the caller must own its
 * storage for the complete locked interval. This gate is leaf-only: do not
 * acquire another semaphore gate while holding it, do not allocate or free
 * while holding it, and never hold it across Wait(), Signal(), ReplyMsg(),
 * Alert(), or any callback. It arbitrates semaphore state only; it does not
 * pin or cancel queued waiter storage. */
void Exec_P4SemaphoreLock(struct SignalSemaphore *, struct ExecBase *);
void Exec_P4SemaphoreUnlock(struct SignalSemaphore *, struct ExecBase *);

#endif /* P4_SEMAPHORE_GATE_H */
