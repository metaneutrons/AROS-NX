/*
 * Actual-source host fixture for the P4 CLIC route sweep.
 *
 * The prior mailbox fixture supplies the host-only runtime/atomic mocks and
 * includes p4_ipi.c verbatim. This translation unit enables the additional
 * route-helper test guard and mocks only the MMIO register windows used by
 * p4_ipi_route(). It is register-model evidence, not silicon routing proof.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P4_IPI_ROUTE_TEST 1

#define ROUTE_BASE UINT32_C(0x500d6000)
#define ROUTE_WINDOW_BYTES UINT32_C(0x800)
#define ROUTE_WORDS 128u

static uint32_t route_regs[2][ROUTE_WORDS];
static unsigned route_reads[2];
static unsigned route_writes[2];
static uint32_t route_read_addresses[ROUTE_WORDS];
static unsigned route_address_count;
static unsigned route_mmio_errors;

static unsigned long p4_r32(unsigned long address);
static void p4_w32(unsigned long address, unsigned long value);

/* Reuse the existing P4_IPI_TEST boundary setup and actual-source include. */
#define main p4_ipi_mailbox_fixture_main
#include "p4-ipi-test.c"
#undef main

static int decode_route_address(unsigned long address, unsigned *hart,
                               unsigned *source)
{
    unsigned long offset;

    if (address < (unsigned long)ROUTE_BASE)
        return 0;
    offset = address - (unsigned long)ROUTE_BASE;
    if (offset >= 2ul * (unsigned long)ROUTE_WINDOW_BYTES || (offset & 3ul))
        return 0;

    *hart = (unsigned)(offset / (unsigned long)ROUTE_WINDOW_BYTES);
    *source = (unsigned)((offset % (unsigned long)ROUTE_WINDOW_BYTES) / 4ul);
    return *hart < 2u && *source < ROUTE_WORDS;
}

static unsigned long p4_r32(unsigned long address)
{
    unsigned hart, source;

    if (!decode_route_address(address, &hart, &source))
    {
        ++route_mmio_errors;
        return 0ul;
    }
    if (route_address_count < ROUTE_WORDS)
        route_read_addresses[route_address_count] = (uint32_t)address;
    else
        ++route_mmio_errors;
    ++route_address_count;
    ++route_reads[hart];
    return (unsigned long)route_regs[hart][source];
}

static void p4_w32(unsigned long address, unsigned long value)
{
    unsigned hart, source;

    if (!decode_route_address(address, &hart, &source) || value > UINT32_MAX)
    {
        ++route_mmio_errors;
        return;
    }
    ++route_writes[hart];
    route_regs[hart][source] = (uint32_t)value;
}

static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void reset_route_fixture(void)
{
    memset(route_regs, 0, sizeof(route_regs));
    memset(route_reads, 0, sizeof(route_reads));
    memset(route_writes, 0, sizeof(route_writes));
    memset(route_read_addresses, 0, sizeof(route_read_addresses));
    route_address_count = 0u;
    route_mmio_errors = 0u;
}

static void fill_pattern(uint32_t seed)
{
    uint32_t state = seed;

    if (state == 0u)
        state = UINT32_C(0x6d2b79f5);
    for (unsigned hart = 0; hart < 2u; ++hart)
    {
        for (unsigned source = 0; source < ROUTE_WORDS; ++source)
        {
            uint32_t value = next_random(&state);
            /* Include non-line-22 routes, foreign line-22 routes, and words
             * with arbitrary high bits to check the production mask/write. */
            route_regs[hart][source] = value;
        }
    }
}

static void check_route_case(unsigned hart, uint32_t seed)
{
    uint32_t before[2][ROUTE_WORDS];
    unsigned expected_writes = 0u;
    unsigned target = 79u + hart;
    unsigned opposite = 80u - hart;

    reset_route_fixture();
    fill_pattern(seed);

    /* Force several routes to line 22, including high-bit payloads, and
     * ensure the target/opposite source cover both special cases. */
    route_regs[hart][3u] = UINT32_C(0xa5a50016);
    route_regs[hart][17u] = UINT32_C(0xdeadbe16);
    route_regs[hart][121u] = UINT32_C(0x80000016);
    route_regs[hart][target] = UINT32_C(0xcafe0107);
    route_regs[hart][opposite] = UINT32_C(0xfacebeef);
    route_regs[1u - hart][3u] = UINT32_C(0xa5a50016);
    memcpy(before, route_regs, sizeof(before));

    p4_ipi_route((uint32_t)hart);

    CHECK(route_mmio_errors == 0u);
    CHECK(route_reads[hart] == ROUTE_WORDS);
    CHECK(route_address_count == ROUTE_WORDS);
    CHECK(route_reads[1u - hart] == 0u);
    CHECK(route_writes[1u - hart] == 0u);
    CHECK(memcmp(route_regs[1u - hart], before[1u - hart],
                 sizeof(route_regs[1u - hart])) == 0);

    for (unsigned source = 0; source < ROUTE_WORDS; ++source)
    {
        uint32_t old_value = before[hart][source];
        uint32_t expected_value;
        uint32_t expected_address = ROUTE_BASE +
            (uint32_t)hart * ROUTE_WINDOW_BYTES + source * 4u;

        CHECK(route_read_addresses[source] == expected_address);
        if (source == target)
            expected_value = 22u;
        else if ((old_value & 0x3fu) == 22u || source == opposite)
            expected_value = 0u;
        else
            expected_value = old_value;
        CHECK(route_regs[hart][source] == expected_value);
        if (source == target || (old_value & 0x3fu) == 22u ||
            source == opposite)
            ++expected_writes;
    }
    CHECK(route_writes[hart] == expected_writes);
    CHECK(route_regs[hart][target] == 22u);
    CHECK(route_regs[hart][opposite] == 0u);

    /* The sanitization is idempotent; the target and opposite-latch entries
     * are rewritten on every pass, with no other second-pass changes. */
    memset(route_reads, 0, sizeof(route_reads));
    memset(route_writes, 0, sizeof(route_writes));
    route_address_count = 0u;
    p4_ipi_route((uint32_t)hart);
    CHECK(route_mmio_errors == 0u);
    CHECK(route_reads[hart] == ROUTE_WORDS);
    CHECK(route_writes[hart] == 2u);
    CHECK(route_reads[1u - hart] == 0u);
    CHECK(route_writes[1u - hart] == 0u);
    CHECK(route_regs[hart][target] == 22u);
    CHECK(route_regs[hart][opposite] == 0u);
}

int main(void)
{
    static const uint32_t seeds[] = {
        UINT32_C(0x1), UINT32_C(0x12345678), UINT32_C(0x9e3779b9),
        UINT32_C(0xdeadbeef), UINT32_C(0xf00dcafe)
    };

    for (unsigned hart = 0; hart < 2u; ++hart)
        for (unsigned i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i)
            check_route_case(hart, seeds[i]);

    printf("p4-ipi-route-test: PASS (%u checks; 10 route cases)\n", checks);
    return 0;
}
