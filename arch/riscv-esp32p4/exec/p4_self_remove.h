#ifndef P4_SELF_REMOVE_H
#define P4_SELF_REMOVE_H
struct Task;
struct ExecBase;
/* Current RUN owner: ordinary continuation or registered outer-trap callback.
 * Never returns into the removed callback or frees its own context. */
void Exec_P4RemoveSelf(struct Task *, struct ExecBase *) __attribute__((noreturn));
#endif
