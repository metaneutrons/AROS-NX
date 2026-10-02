/* Private public-Exec adapters; selected only by the unfinished SMP build. */
#ifndef P4_SEMAPHORE_PUBLIC_H
#define P4_SEMAPHORE_PUBLIC_H

struct SignalSemaphore;
struct Task;
struct ExecBase;

/* Current live RUN continuation; caller owns initialized semaphore storage.
 * owner is either the current task (exclusive) or NULL (shared).
 * No borrowed request or arena pointer survives a gate or is dereferenced. */
void Exec_P4ObtainSemaphore(struct SignalSemaphore *, struct Task *task,
    struct Task *owner, struct ExecBase *);
void Exec_P4ReleaseSemaphore(struct SignalSemaphore *, struct ExecBase *);

#endif
