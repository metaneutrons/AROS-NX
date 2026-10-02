#ifndef P4_CALLBACK_ESCAPE_H
#define P4_CALLBACK_ESCAPE_H
struct ExecBase;
/* Caller owns current RUN at outer trap depth 1. The invocation holds no
 * Task/list locks. Return 0 means self-removal abandoned the callback; the
 * caller must discard/finish releasing RUN, never install its old frame. */
int krnP4RunTaskCallback(void (*invoke)(void *), void *, struct ExecBase *);
/* Called with local interrupts masked BEFORE irreversible self-retirement.
 * Only an ordinary continuation or our matching scoped callback may retire. */
int krnP4SelfRemoveContextAllowed(struct ExecBase *);
/* A concurrent remover may win before our pin/retire transaction. The current
 * RUN identity must then already belong to deferred service retirement. */
int krnP4SelfRemoveCurrentRetired(struct ExecBase *);
/* After retirement/unlock/final unpin, RUN still owns the current identity.
 * No Task fields are read. Normal continuation dispatches; callback unwinds
 * directly to its outer kernel owner without a nested scheduling ecall. */
void krnP4SelfRemoveTransfer(struct ExecBase *) __attribute__((noreturn));
#endif
