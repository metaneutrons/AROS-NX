/* Private, bounded E3 lock qualification; never an Exec scheduler. */
#ifndef P4_SPINLOCK_PROBE_H
#define P4_SPINLOCK_PROBE_H
#include <stdint.h>
int krnP4SpinProbePrepare(void);
int krnP4SpinProbeExercise(uint32_t hart);
int krnP4SpinProbeCheck(void);
#endif
