/* Private two-hart qualification for the P4 software-atomic service. */
#ifndef P4_SOFT_ATOMIC_PROBE_H
#define P4_SOFT_ATOMIC_PROBE_H

#include <stdint.h>

/*
 * Private E3 gate only. Prepare and Check run on hart 0 with MIE clear;
 * Prepare's caller keeps hart 1 reset-held. A later Prepare may reuse only
 * the same immutable service policy and only after the prior Check passed,
 * again with hart 1 reset-held. Exercise runs on the named hart with MIE
 * clear after the controller starts the secondary worker.
 */
int krnP4SoftAtomicProbePrepare(uintptr_t scratch_begin);
int krnP4SoftAtomicProbeExercise(uint32_t hart);
int krnP4SoftAtomicProbeCheck(void);

#endif /* P4_SOFT_ATOMIC_PROBE_H */
