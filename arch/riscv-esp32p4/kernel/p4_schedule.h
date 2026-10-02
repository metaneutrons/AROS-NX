/* P4 SMP-only scheduler decision, selected by the P4 core_Schedule override. */
#ifndef P4_SCHEDULE_H
#define P4_SCHEDULE_H

struct ExecBase;

/*
 * Outermost trap only, local MIE clear. The current task's unique RUN
 * reference must remain held for the call. This makes a scheduling decision
 * without mutating task queues or admitting a replacement task.
 */
int krnP4ShouldSchedule(struct ExecBase *SysBase);

#endif /* P4_SCHEDULE_H */
