#ifndef P4_REMOVE_EXTERNAL_H
#define P4_REMOVE_EXTERNAL_H
struct Task;
struct ExecBase;
/* Public external RemTask candidate: normal current Task context only.
 * Retire and wait through the registry, never the generic target-free path.
 * No target access follows ordinary pin release. Full generic-state and
 * runtime qualification remain required before enabling the SMP variant. */
void Exec_P4RemoveExternal(struct Task *target, struct Task *requester,
    struct ExecBase *);
#endif
