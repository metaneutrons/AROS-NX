/* Actual reservation body with host MemHeader layout; no Exec service mocks. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

struct MemChunk { struct MemChunk *mc_Next; uintptr_t mc_Bytes; };
struct MemHeader
{
    struct { unsigned int ln_Type; } mh_Node;
    unsigned int mh_Attributes;
    struct MemChunk *mh_First;
    void *mh_Lower, *mh_Upper;
    uintptr_t mh_Free;
};
#define MEMCHUNK_TOTAL 16u
#define MEMHEADER_TOTAL ((sizeof(struct MemHeader) + 15u) & ~(size_t)15u)
#define MEMF_MANAGED (1u << 15)
#define NT_MEMORY 10u
#define P4_BOOT_RESERVE_TEST 1
#include "../p4_boot_reserve.c"

static _Alignas(64) unsigned char memory[4096];
static unsigned char before[4096];
static unsigned int checks;
static unsigned int failures;
#define CHECK(expression) do { ++checks; if (!(expression)) { ++failures; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expression); } } while (0)

static struct MemHeader *prepare(void)
{
    struct MemHeader *pool = (struct MemHeader *)(void *)memory;
    memset(memory, 0, sizeof(memory));
    pool->mh_Node.ln_Type = NT_MEMORY;
    pool->mh_Lower = memory;
    pool->mh_Upper = memory + sizeof(memory);
    pool->mh_First = (struct MemChunk *)(void *)(memory + MEMHEADER_TOTAL);
    pool->mh_First->mc_Bytes = sizeof(memory) - MEMHEADER_TOTAL;
    pool->mh_Free = pool->mh_First->mc_Bytes;
    return pool;
}

static void refused(struct MemHeader *pool, uintptr_t begin, uintptr_t end,
    size_t bytes, size_t alignment, void **out)
{
    memcpy(before, memory, sizeof(memory));
    CHECK(!krnP4BootReservePrefix(pool, begin, end, bytes, alignment, out));
    CHECK(memcmp(before, memory, sizeof(memory)) == 0);
}

int main(void)
{
    uintptr_t begin = (uintptr_t)memory;
    uintptr_t end = begin + sizeof(memory);
    struct MemHeader *pool;
    void *out;
    size_t alignment, bytes;

    for (alignment = 1u; alignment <= 256u; alignment *= 2u)
    {
        for (bytes = 1u; bytes <= 1025u; bytes += 127u)
        {
            uintptr_t old_free, reserved, next;
            pool = prepare();
            old_free = pool->mh_Free;
            out = NULL;
            CHECK(krnP4BootReservePrefix(pool, begin, end, bytes,
                                         alignment, &out));
            reserved = (uintptr_t)out;
            next = (uintptr_t)pool->mh_First;
            CHECK(reserved % alignment == 0);
            CHECK(reserved >= begin + MEMHEADER_TOTAL);
            CHECK(next >= reserved + bytes && next % MEMCHUNK_TOTAL == 0);
            CHECK(pool->mh_Free == end - next && pool->mh_Free < old_free);
            CHECK(pool->mh_First->mc_Next == NULL);
            CHECK(pool->mh_First->mc_Bytes == pool->mh_Free);
            CHECK(pool->mh_Lower == memory && pool->mh_Upper == memory + sizeof(memory));
            /* The reserved span can be cleared without corrupting the next
               free chunk; a second reservation remains structurally valid. */
            memset(out, 0xa5, bytes);
            CHECK(pool->mh_First->mc_Bytes == pool->mh_Free);
            CHECK(krnP4BootReservePrefix(pool, begin, end, 64u, 64u, &out));
        }
    }

    pool = prepare(); out = (void *)(uintptr_t)0x1234u;
    refused(pool, begin, end, 0, 16u, &out);
    refused(pool, begin, end, 64u, 0, &out);
    refused(pool, begin, end, 64u, 3u, &out);
    refused(pool, begin, end, SIZE_MAX, 16u, &out);
    refused(pool, begin, end, sizeof(memory), 16u, &out);
    refused(pool, begin, begin, 64u, 16u, &out);
    refused(pool, begin, begin + sizeof(*pool) - 1u, 64u, 16u, &out);
    refused(pool, begin + 1u, end, 64u, 16u, &out);
    refused(NULL, begin, end, 64u, 16u, &out);
    refused(pool, begin, end, 64u, 16u, NULL);
    refused(pool, begin, end, 64u, 16u, (void **)(void *)memory);
    refused(pool, begin, end, 64u, 16u, (void **)(void *)(memory + 256u));
    refused(pool, begin, end, 64u, 16u, (void **)(uintptr_t)(UINTPTR_MAX - 1u));
    CHECK(out == (void *)(uintptr_t)0x1234u);

    pool->mh_Attributes = MEMF_MANAGED;
    refused(pool, begin, end, 64u, 16u, &out);
    pool = prepare(); pool->mh_Node.ln_Type = 0;
    refused(pool, begin, end, 64u, 16u, &out);
    pool = prepare(); pool->mh_Lower = memory + 16u;
    refused(pool, begin, end, 64u, 16u, &out);
    pool = prepare(); pool->mh_Upper = memory + sizeof(memory) - 1u;
    refused(pool, begin, end, 64u, 16u, &out);
    pool = prepare(); pool->mh_First = NULL;
    refused(pool, begin, end, 64u, 16u, &out);
    pool = prepare(); pool->mh_First = (struct MemChunk *)(void *)(memory + sizeof(memory));
    refused(pool, begin, end, 64u, 16u, &out);
    pool = prepare(); pool->mh_First->mc_Next = pool->mh_First;
    refused(pool, begin, end, 64u, 16u, &out);
    pool = prepare(); --pool->mh_First->mc_Bytes;
    refused(pool, begin, end, 64u, 16u, &out);
    pool = prepare(); --pool->mh_Free;
    refused(pool, begin, end, 64u, 16u, &out);
    pool = prepare();
    refused(pool, begin, end, (size_t)pool->mh_Free - MEMCHUNK_TOTAL + 1u,
            16u, &out);

    printf("P4 boot-reserve checks: %u passed, %u failed\n", checks, failures);
    return failures != 0u;
}
