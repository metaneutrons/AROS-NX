#ifndef P4_REMOVE_WAIT_H
#define P4_REMOVE_WAIT_H
#include "p4_remove_bank.h"
struct Task;
struct ExecBase;
/* Wait for an already admitted exact scalar ticket. The caller is the live
 * current RUN continuation, outside every Task/queue/semaphore/registry lock,
 * with its ordinary target pin released. No target pointer is accepted here.
 * Normal Task context only, never a trap/callback. SIGF_SINGLE is a wake hint,
 * not completion. A retired requester transfers without returning; success
 * consumes its ACKed ticket before returning. Re-query CPU-local ownership
 * after every Wait; no per-hart pointer survives a possible migration.
 * This private helper is not yet the public RemTask admission/capacity route. */
void Exec_P4AwaitRemoval(struct Task *, struct p4_remove_handle,
    struct ExecBase *);
#endif
