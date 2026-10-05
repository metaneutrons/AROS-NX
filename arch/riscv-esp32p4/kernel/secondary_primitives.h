/* Private E2 diagnostics; not an Exec SMP ABI. */
#ifndef P4_SECONDARY_PRIMITIVES_H
#define P4_SECONDARY_PRIMITIVES_H
int krnP4E2Prepare(void);
int krnP4E2Run(unsigned int epoch);
void krnP4E2Teardown(void);
void krnP4E2WorkerInit(void);
void krnP4E2WorkerStep(void);
int krnP4E2IRQ(void);
#endif
