#ifndef P4_TIMER_SNAPSHOT_H
#define P4_TIMER_SNAPSHOT_H
#include <stdint.h>
struct KrnP4TimerOps;
const struct KrnP4TimerOps *krnP4TimerSnapshotOps(void);
/* Prepare after software atomics, before SysBase publication; hart1 held. */
int krnP4TimerSnapshotPrepare(void);
/* Serialized UNIT0 latch/read; preserves raw MIE and Exec nesting. */
uint64_t krnP4TimerSnapshotRead(void);
#endif
