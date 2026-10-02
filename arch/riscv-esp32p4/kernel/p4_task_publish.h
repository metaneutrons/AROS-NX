#ifndef P4_TASK_PUBLISH_H
#define P4_TASK_PUBLISH_H
struct Task;
struct ExecBase;
/* Independent allocation; success retains one creator pin. Creation failure
 * leaves no registered record. Caller drops the creator pin only after its
 * final Task access; reclamation later frees the detached record separately. */
int krnP4TaskRecordCreate(struct Task *, struct ExecBase *);
int krnP4NewTaskReady(struct Task *, struct ExecBase *);
int krnP4BootTaskRegister(struct Task *, struct ExecBase *);
#endif
