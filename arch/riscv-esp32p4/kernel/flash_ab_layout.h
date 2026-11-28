#ifndef P4_FLASH_AB_LAYOUT_H
#define P4_FLASH_AB_LAYOUT_H

#include <stdint.h>
#include <stddef.h>

/* Pure, read-only partition contract shared by native and host tests.
 * The second stage authenticates/checks its table; this resolver additionally
 * refuses ambiguity, overlaps and a kernel/BSP selection by listing order.
 * It is not a write backend, package digest or OTA confirmation mechanism. */
struct P4ABExtent { uint32_t offset, size; };
struct P4ABLayout
{
    struct P4ABExtent kernel[2], bsp[2], develop;
    unsigned active_slot;
};

enum P4ABLayoutResult
{
    P4_AB_LAYOUT_OK = 1,
    P4_AB_LAYOUT_BAD_TABLE = -1,
    P4_AB_LAYOUT_BAD_RANGE = -2,
    P4_AB_LAYOUT_AMBIGUOUS = -3,
    P4_AB_LAYOUT_MISSING = -4,
    P4_AB_LAYOUT_WRONG_SLOT = -5
};

int p4_ab_layout_resolve(const unsigned char *table, size_t bytes,
    uint32_t table_offset, uint32_t flash_bytes, uint32_t mapped_bytes,
    uint32_t running_kernel_offset, uint32_t kernel_bytes,
    uint32_t bsp_bytes, uint32_t develop_bytes, struct P4ABLayout *out);

#endif
