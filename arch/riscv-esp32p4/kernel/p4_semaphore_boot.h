/* Private, permanent semaphore arena prepared before Exec publication. */
#ifndef P4_SEMAPHORE_BOOT_H
#define P4_SEMAPHORE_BOOT_H
#include <stddef.h>
#include <stdint.h>

struct MemHeader;
struct SemaphoreRequest;
struct p4_semaphore_ledger;

/* Hart0 only, IRQs masked, hart1 reset-held, plain unpublished PSRAM pool.
 * Failure withholds Exec; never call an Exec allocator to recover here. */
int krnP4SemaphoreBootPrepare(struct MemHeader *, uintptr_t, uintptr_t);
struct p4_semaphore_ledger *krnP4SemaphoreBootLedger(void);
struct SemaphoreRequest *krnP4SemaphoreBootRequest(uint32_t);
size_t krnP4SemaphoreBootBytes(void);
uint32_t krnP4SemaphoreBootOperationCapacity(void);
uint32_t krnP4SemaphoreBootEntryCapacity(void);
#endif
