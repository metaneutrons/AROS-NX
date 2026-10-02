/* Private constructor ownership channel; not a public Task tag or ABI.
 * When the complete scope binding is selected, NewCreateTaskA prepends this
 * output to its existing NewAddTask tag chain,
 * preserving vector interposition (notably task.resource's prelaunch hook).
 * The output belongs to the calling RUN stack and must outlive NewAddTask.
 * No pointer to it may be retained after that call returns or the caller dies.
 */
#ifndef P4_TASK_CREATION_H
#define P4_TASK_CREATION_H

#define EXEC_PLATFORM_CREATION_OWNERSHIP_TAG 0xd034434fu
#define EXEC_PLATFORM_CREATION_GENERATION_TAG 0xd0344347u
#include "../kernel/p4_creation_channel.h"
#define EXEC_PLATFORM_CREATION_CALLER_OWNED 1u
#define EXEC_PLATFORM_CREATION_REGISTERED 2u
#define EXEC_PLATFORM_CREATION_SERVICE_OWNED 3u

/* CALLER: setup failed before lifetime publication; caller frees its MemList.
 * REGISTERED: lifetime published; constructor must succeed or complete a
 * cancellation handoff. A NULL result here is an invariant failure, not an
 * allocation error. SERVICE: cancellation has transferred the MemList to
 * cleanup; caller must clear output handles and never FreeEntry it again.
 * The current constructor does not yet produce SERVICE: cancellation and
 * creator-retirement recovery remain required before SMP activation.
 * No output decoding is enabled until a current-Task-bound registered channel
 * resolver is installed. The numeric TAG_USER value alone grants no write.
 * The generation travels separately, never recovered by reading a candidate
 * pointer. Scoped routing requires all three Begin/End/Refused hooks; platform
 * selection remains absent until constructor cancellation/no-save recovery.
 * Without that selection NewCreateTaskA forwards the original tags unchanged;
 * it must not expose an unregistered private stack output to interposers.
 */
#endif
