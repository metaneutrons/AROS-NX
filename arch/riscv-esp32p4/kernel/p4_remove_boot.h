/* Boot-only reservation for the portable external-removal ticket bank. */
#ifndef P4_REMOVE_BOOT_H
#define P4_REMOVE_BOOT_H

#include <stddef.h>
#include <stdint.h>

struct MemHeader;
struct p4_remove_bank;

/* Hart0-only, IRQ-masked, on a plain PSRAM MemHeader before allocator
 * conversion/publication and while hart1 remains reset-held. The supplied
 * region must be that PSRAM pool. One attempt is allowed, including failed
 * ones. The prefix is withheld permanently if bank initialization fails. */
int krnP4RemoveBootPrepare(struct MemHeader *pool,
                           uintptr_t region_begin,
                           uintptr_t region_end);

/* These getters expose the prepared bank header, not the private slot array.
 * Before successful preparation they return NULL/zero. */
struct p4_remove_bank *krnP4RemoveBootBank(void);
size_t krnP4RemoveBootBytes(void);
uint32_t krnP4RemoveBootCapacity(void);

#endif
