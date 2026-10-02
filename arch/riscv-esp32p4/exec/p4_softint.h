#ifndef P4_SOFTINT_H
#define P4_SOFTINT_H
struct ExecBase;
struct Interrupt;
int krnP4SoftIntPrepare(void);
int krnP4SoftIntPending(struct ExecBase *base);
void krnP4SoftIntEnqueue(struct Interrupt *interrupt, struct ExecBase *base);
int krnP4SoftIntBegin(struct ExecBase *base);
struct Interrupt *krnP4SoftIntTake(struct ExecBase *base);
#endif
