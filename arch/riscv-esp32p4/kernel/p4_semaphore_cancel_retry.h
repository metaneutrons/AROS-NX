/* Caller-serialized retry interest, separate from the durable wake transport. */
#ifndef P4_SEMAPHORE_CANCEL_RETRY_H
#define P4_SEMAPHORE_CANCEL_RETRY_H
#include <stdint.h>
struct p4_semaphore_cancel_retry
{
    uint32_t active;
    uint32_t waiting;
};
/* Zero-initialize before publication. All calls share the registry gate.
 * Begin registers interest BEFORE a pass can encounter a resource refusal.
 * End retains interest iff some operation remained blocked. A producer event
 * during/after the pass asks the caller to latch the existing durable cleanup
 * wake under that SAME gate and poke only after every outer lock is released.
 * The leaf never consumes that wake: End cannot erase an already-latched
 * event. No event is synthesized just because an owner remains blocked.
 * Return1 success/interest,0 busy/no interest,-1 malformed unchanged. */
int p4_semaphore_cancel_retry_begin(struct p4_semaphore_cancel_retry *);
int p4_semaphore_cancel_retry_end(struct p4_semaphore_cancel_retry *,
    uint32_t blocked);
int p4_semaphore_cancel_retry_event(const struct p4_semaphore_cancel_retry *);

struct Task;
struct ExecBase;
/* Protected worker bindings precede installation of the consumer.
 * Implementations authenticate the protected armed RUN identity,
 * serialize the interest leaf with pin/headroom producers under the registry
 * gate, and preserve the existing wake latch. Declarations alone do not bind
 * the consumer or prove production wake coverage. Caller supplies its own
 * current worker continuation, not merely another Task's protected identity. */
void krnP4SemaphoreCancelRetryBegin(struct Task *, struct ExecBase *);
void krnP4SemaphoreCancelRetryEnd(struct Task *, uint32_t, struct ExecBase *);
#endif
