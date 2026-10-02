#ifndef P4_DISCARD_H
#define P4_DISCARD_H
struct ExecBase;
/* Outermost ISR, MIE clear, unique RUN owner. Removal already retired and
 * detached the Task and transferred a service reference; no context is saved.
 * Normal Switch callers have no current Task here and skip this function. */
void krnP4DiscardCurrent(struct ExecBase *);
#endif
