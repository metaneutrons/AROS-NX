/* Private P4 cleanup worker; no public Exec ABI extension. */
#ifndef P4_CLEANUP_WORKER_H
#define P4_CLEANUP_WORKER_H
struct ExecBase;
struct Task;
/* Either admitted hart, safe boundary only: no outer Task, queue or registry
 * lock. The registry serializes snapshot ownership; permanent worker pin
 * protects storage while the caller delivers outside the registry gate. */
void Exec_P4DeliverCleanupWake(struct ExecBase *base);
/* Protected ServiceTask continuation only; does not consume raw Task messages. */
void Exec_P4ServiceLoop(struct ExecBase *base);
/* Protected service continuation only; consumes one durable batch using its
 * existing pins and signals outside all semaphore/registry gates. */
int Exec_P4DeliverOneSemaphoreNotification(struct Task *worker,
    struct ExecBase *base);
#endif
