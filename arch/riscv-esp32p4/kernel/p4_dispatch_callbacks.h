#ifndef P4_DISPATCH_CALLBACKS_H
#define P4_DISPATCH_CALLBACKS_H
struct Task;
struct ExecBase;
/* Current hart retains RUN throughout. Returns 0 if callbacks/remote removal
 * retired the selected task; caller must discard and dispatch another frame.
 * Returns with raw MIE clear; no saved context is read here. */
int krnP4DispatchCallbacks(struct Task *, struct ExecBase *);
#endif
