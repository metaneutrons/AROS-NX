/* Boot-only prefix reservation before a plain PSRAM MemHeader is published. */
#if !defined(P4_BOOT_RESERVE_TEST)
#include <exec/memory.h>
#include <exec/memheaderext.h>
#include <exec/nodes.h>
#endif
#include "p4_boot_reserve.h"

int krnP4BootReservePrefix(struct MemHeader *pool, uintptr_t region_begin,
    uintptr_t region_end, size_t bytes, size_t alignment, void **out)
{
    uintptr_t header = (uintptr_t)pool;
    uintptr_t output = (uintptr_t)out;
    uintptr_t lower, upper, chunk, reserved, next;
    size_t chunk_alignment = MEMCHUNK_TOTAL;
    struct MemChunk *old_chunk, *new_chunk;

    /* Address/range checks precede every pool/output dereference. Caller
     * owns the actual backing storage; these checks are not an MMU probe. */
    if (pool == NULL || out == NULL || region_begin >= region_end ||
        region_end - region_begin < sizeof(*pool) ||
        header < region_begin || header > region_end - sizeof(*pool) ||
        header % _Alignof(struct MemHeader) != 0 ||
        output % _Alignof(void *) != 0 ||
        output > UINTPTR_MAX - sizeof(*out) ||
        (output < region_end && output + sizeof(*out) > region_begin) ||
        bytes == 0 || alignment == 0 ||
        (alignment & (alignment - 1u)) != 0 ||
        chunk_alignment == 0 ||
        (chunk_alignment & (chunk_alignment - 1u)) != 0)
        return 0;

    lower = (uintptr_t)pool->mh_Lower;
    upper = (uintptr_t)pool->mh_Upper;
    old_chunk = pool->mh_First;
    chunk = (uintptr_t)old_chunk;
    if (pool->mh_Node.ln_Type != NT_MEMORY ||
        (pool->mh_Attributes & MEMF_MANAGED) != 0 ||
        lower != header || upper > region_end || lower >= upper ||
        upper - lower < MEMHEADER_TOTAL + sizeof(*old_chunk) ||
        old_chunk == NULL || chunk < lower + MEMHEADER_TOTAL ||
        chunk > upper - sizeof(*old_chunk) ||
        chunk % chunk_alignment != 0 ||
        old_chunk->mc_Next != NULL ||
        old_chunk->mc_Bytes != upper - chunk ||
        pool->mh_Free != old_chunk->mc_Bytes)
        return 0;

    if (alignment < chunk_alignment)
        alignment = chunk_alignment;
    if (chunk > UINTPTR_MAX - (alignment - 1u))
        return 0;
    reserved = (chunk + alignment - 1u) & ~(uintptr_t)(alignment - 1u);
    if (reserved > upper || bytes > upper - reserved ||
        reserved + bytes > UINTPTR_MAX - (chunk_alignment - 1u))
        return 0;
    next = (reserved + bytes + chunk_alignment - 1u) &
           ~(uintptr_t)(chunk_alignment - 1u);
    if (next > upper || upper - next < MEMCHUNK_TOTAL)
        return 0;

    /* No failing operation/callout remains after the first write. */
    new_chunk = (struct MemChunk *)next;
    new_chunk->mc_Next = NULL;
    new_chunk->mc_Bytes = upper - next;
    pool->mh_First = new_chunk;
    pool->mh_Free = upper - next;
    *out = (void *)reserved;
    return 1;
}
