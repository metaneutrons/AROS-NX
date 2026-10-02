/* Single-hart, unpublished plain MemHeader reservation; not an Exec allocator. */
#ifndef P4_BOOT_RESERVE_H
#define P4_BOOT_RESERVE_H

#include <stddef.h>
#include <stdint.h>

struct MemHeader;

/* The caller owns readable/writable region storage, with interrupts masked
 * and no published allocator or secondary hart. Refuses managed/fragmented
 * pools and malformed/aliased outputs without any write. On success, removes
 * an aligned prefix from the sole free chunk, retaining a valid free chunk
 * for subsequent TLSF conversion. Lower/Upper remain informationally intact.
 * No allocation, locks, Task access or callback occurs here. */
int krnP4BootReservePrefix(struct MemHeader *pool, uintptr_t region_begin,
    uintptr_t region_end, size_t bytes, size_t alignment, void **out);

#endif
