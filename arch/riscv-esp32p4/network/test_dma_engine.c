/* Host fixture for the production ESP32-P4 shared SDMMC DMA engine.
 * tools/test-dma-engine.py inserts current function bodies from dma.c. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t ULONG;
typedef uintptr_t IPTR;
typedef uint8_t UBYTE;
typedef int16_t BOOL;
typedef void *APTR;

#define TRUE 1
#define FALSE 0
#define EVENT_CAPACITY 256U
#define WRITE_CAPACITY 32U

/* INSERT_ACTUAL_CACHEOPS_CONSTANTS */

struct Task
{
    unsigned id;
};

struct SignalSemaphore
{
    struct Task *ss_Owner;
};

struct P4SDMMCHost;

/* dma.h uses host.h for the resource ABI. Supply only the types and fields
 * consumed by these extracted functions, then include the production DMA
 * descriptor declaration, constants and function prototypes. */
#define ESP32P4_SDMMC_HOST_H
#include "dma.h"
#include "registers.h"

struct Library
{
    unsigned unused;
};

struct KrnCacheOps
{
    ULONG version;
    void (*writeback_range)(APTR, ULONG);
    void (*invalidate_range)(APTR, ULONG);
};

struct P4SDMMCHost
{
    struct SignalSemaphore lock;
    ULONG (*now_us)(void);
    APTR dma_desc;
    UBYTE *bounce;
    UBYTE *dma_data;
    ULONG dma_length;
    BOOL dma_writing;
    BOOL dma_faulted;
    void (*invalidate)(struct P4SDMMCHost *);
};

enum event_kind
{
    EVENT_MMIO_READ = 1,
    EVENT_MMIO_WRITE,
    EVENT_CACHE_WRITEBACK,
    EVENT_CACHE_INVALIDATE,
    EVENT_BARRIER
};

struct event
{
    enum event_kind kind;
    ULONG offset;
    ULONG value;
    APTR address;
    ULONG length;
};

struct write_event
{
    ULONG offset;
    ULONG value;
};

static struct Task task_a = { 1U };
static struct Task task_b = { 2U };
static struct Task *active_task;
static struct P4SDMMCHost dma_host;
static struct Library kernel_library = { 0U };
static struct KrnCacheOps mock_cache_ops;
static struct KrnCacheOps *returned_cache_ops;
static IPTR cache_attribute_result;
static BOOL resource_available;
static ULONG registers[128];
static struct event events[EVENT_CAPACITY];
static struct write_event writes[WRITE_CAPACITY];
static unsigned event_count;
static unsigned write_count;
static unsigned read_count;
static unsigned cache_writeback_count;
static unsigned cache_invalidate_count;
static unsigned resource_calls;
static unsigned attribute_calls;
static unsigned test_cases;
static unsigned checks;
static ULONG timer_value;
static ULONG timer_step;
static ULONG now_call_count;
static unsigned invalidate_count;
static ULONG stuck_ctrl_mask;
static BOOL stuck_bmod;
static UBYTE test_data[P4HOST_DMA_MAX + 64UL]
    __attribute__((aligned(64)));
static UBYTE unaligned_data[P4HOST_DMA_MAX + 64UL]
    __attribute__((aligned(64)));

static unsigned writes_to(ULONG offset);

static void fail_check(const char *expression, const char *file, int line)
{
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
    exit(1);
}

#define CHECK(expression)                                                   \
    do                                                                      \
    {                                                                       \
        ++checks;                                                           \
        if (!(expression))                                                  \
            fail_check(#expression, __FILE__, __LINE__);                   \
    } while (0)

static void record_event(enum event_kind kind, ULONG offset, ULONG value,
                         APTR address, ULONG length)
{
    if (event_count < EVENT_CAPACITY)
    {
        events[event_count].kind = kind;
        events[event_count].offset = offset;
        events[event_count].value = value;
        events[event_count].address = address;
        events[event_count].length = length;
        ++event_count;
    }
}

static struct Task *FindTask(const char *name)
{
    (void)name;
    return active_task;
}

static struct Library *OpenResource(const char *name)
{
    CHECK(strcmp(name, "kernel.resource") == 0);
    ++resource_calls;
    return resource_available ? &kernel_library : NULL;
}

static IPTR KrnGetSystemAttr(ULONG attribute)
{
    CHECK(attribute == KATTR_CacheOps);
    ++attribute_calls;
    if (cache_attribute_result == 1UL)
        return (IPTR)returned_cache_ops;
    return (IPTR)cache_attribute_result;
}

static ULONG mock_now_us(void)
{
    ULONG result = timer_value;
    ++now_call_count;
    timer_value += timer_step;
    return result;
}

static void modeled_memory_barrier(void)
{
    record_event(EVENT_BARRIER, 0UL, 0UL, NULL, 0UL);
}

static void mock_writeback_range(APTR address, ULONG length)
{
    ++cache_writeback_count;
    record_event(EVENT_CACHE_WRITEBACK, 0UL, 0UL, address, length);
}

static void mock_invalidate_range(APTR address, ULONG length)
{
    ++cache_invalidate_count;
    record_event(EVENT_CACHE_INVALIDATE, 0UL, 0UL, address, length);
}

static void mock_invalidate_host(struct P4SDMMCHost *host)
{
    CHECK(host == &dma_host);
    ++invalidate_count;
}

static ULONG dma_read(ULONG offset)
{
    ULONG value;
    CHECK((offset / sizeof(ULONG)) <
          (sizeof(registers) / sizeof(registers[0])));
    ++read_count;
    value = registers[offset / sizeof(ULONG)];
    record_event(EVENT_MMIO_READ, offset, value, NULL, 0UL);
    return value;
}

static void dma_write(ULONG offset, ULONG value)
{
    ULONG *reg;
    CHECK((offset / sizeof(ULONG)) <
          (sizeof(registers) / sizeof(registers[0])));
    reg = &registers[offset / sizeof(ULONG)];
    if (write_count < WRITE_CAPACITY)
    {
        writes[write_count].offset = offset;
        writes[write_count].value = value;
        ++write_count;
    }
    record_event(EVENT_MMIO_WRITE, offset, value, NULL, 0UL);

    if (offset == P4SD_CTRL)
    {
        ULONG reset_bits = value & (P4SD_CTRL_FIFO_RESET | P4SD_CTRL_DMA_RESET);
        ULONG held_bits = reset_bits & stuck_ctrl_mask;
        *reg = (value & ~(P4SD_CTRL_FIFO_RESET | P4SD_CTRL_DMA_RESET)) |
               held_bits;
    }
    else if (offset == P4SD_BMOD)
    {
        *reg = stuck_bmod ? value : (value & ~P4SD_BMOD_SWR);
    }
    else if (offset == P4SD_IDSTS)
    {
        /* IDSTS interrupt status is write-one-to-clear. */
        *reg &= ~value;
    }
    else
    {
        *reg = value;
    }
    modeled_memory_barrier();
}

/* INSERT_ACTUAL_DMA_SOURCE */

static void begin_case(void)
{
    ++test_cases;
}

static void clear_activity(void)
{
    memset(events, 0, sizeof(events));
    memset(writes, 0, sizeof(writes));
    event_count = 0U;
    write_count = 0U;
    read_count = 0U;
    cache_writeback_count = 0U;
    cache_invalidate_count = 0U;
}

static void setup_host(void)
{
    memset(&dma_host, 0, sizeof(dma_host));
    memset(registers, 0, sizeof(registers));
    clear_activity();
    active_task = &task_a;
    dma_host.lock.ss_Owner = &task_a;
    dma_host.now_us = mock_now_us;
    timer_value = 0UL;
    timer_step = 1UL;
    now_call_count = 0UL;
    stuck_ctrl_mask = 0UL;
    stuck_bmod = FALSE;
    resource_available = TRUE;
    returned_cache_ops = &mock_cache_ops;
    cache_attribute_result = 1UL;
    resource_calls = 0U;
    attribute_calls = 0U;
    mock_cache_ops.version = KRN_CACHE_OPS_VERSION;
    mock_cache_ops.writeback_range = mock_writeback_range;
    mock_cache_ops.invalidate_range = mock_invalidate_range;
    p4host_dma_init(&dma_host);
    /* The fixture starts after a successful physical controller reset. */
    dma_host.dma_faulted = FALSE;
    dma_host.invalidate = mock_invalidate_host;
    invalidate_count = 0U;
}

static ULONG rounded_cache_length(ULONG length)
{
    return (length + 63UL) & ~63UL;
}

static unsigned find_event(enum event_kind kind, ULONG offset)
{
    unsigned index;
    for (index = 0U; index < event_count; ++index)
        if (events[index].kind == kind && events[index].offset == offset)
            return index;
    fail_check("requested event exists", __FILE__, __LINE__);
    return 0U;
}

static unsigned find_cache_event(enum event_kind kind, APTR address)
{
    unsigned index;
    for (index = 0U; index < event_count; ++index)
        if (events[index].kind == kind && events[index].address == address)
            return index;
    fail_check("requested cache event exists", __FILE__, __LINE__);
    return 0U;
}

static void check_no_activity(void)
{
    CHECK(read_count == 0U);
    CHECK(write_count == 0U);
    CHECK(cache_writeback_count == 0U);
    CHECK(cache_invalidate_count == 0U);
}

static void test_descriptor_layout_and_init(void)
{
    begin_case();
    memset(&dma_host, 0, sizeof(dma_host));
    active_task = &task_a;
    dma_host.lock.ss_Owner = &task_a;
    dma_host.now_us = mock_now_us;
    clear_activity();
    p4host_dma_init(&dma_host);
    CHECK(sizeof(struct P4HostDMADesc) == 64U);
    CHECK(offsetof(struct P4HostDMADesc, reserved) == 16U);
    CHECK(((IPTR)dma_host.dma_desc & 63U) == 0U);
    CHECK(((IPTR)dma_host.bounce & 63U) == 0U);
    CHECK(dma_host.dma_desc == (APTR)descriptors);
    CHECK(dma_host.bounce == bounce);
    CHECK(dma_host.dma_data == NULL);
    CHECK(dma_host.dma_length == 0UL);
    CHECK(dma_host.dma_writing == FALSE);
    CHECK(dma_host.dma_faulted == TRUE);
    check_no_activity();
}

static void check_cache_refusal_and_retry(void)
{
    ULONG cache_length = rounded_cache_length(65UL);
    CHECK(!p4host_dma_prepare(&dma_host, test_data, 65UL, FALSE));
    CHECK(dma_host.dma_data == NULL);
    CHECK(dma_host.dma_length == 0UL);
    CHECK(dma_host.dma_writing == FALSE);
    check_no_activity();
    CHECK(resource_calls == 1U);
    CHECK(attribute_calls == (resource_available ? 1U : 0U));

    resource_available = TRUE;
    returned_cache_ops = &mock_cache_ops;
    cache_attribute_result = 1UL;
    mock_cache_ops.version = KRN_CACHE_OPS_VERSION;
    mock_cache_ops.writeback_range = mock_writeback_range;
    mock_cache_ops.invalidate_range = mock_invalidate_range;
    clear_activity();
    CHECK(p4host_dma_prepare(&dma_host, test_data, 65UL, FALSE));
    CHECK(dma_host.dma_length == 65UL);
    CHECK(cache_writeback_count == 2U);
    CHECK(cache_invalidate_count == 0U);
    CHECK(events[find_cache_event(EVENT_CACHE_WRITEBACK, bounce)].length ==
          cache_length);
    CHECK(p4host_dma_reset(&dma_host));
}

static void test_cache_lookup_refusals_retry(void)
{
    unsigned refusal;
    begin_case();
    for (refusal = 0U; refusal < 7U; ++refusal)
    {
        setup_host();
        switch (refusal)
        {
            case 0U:
                resource_available = FALSE;
                break;
            case 1U:
                cache_attribute_result = 0UL;
                break;
            case 2U:
            cache_attribute_result = (IPTR)-1;
                break;
            case 3U:
                mock_cache_ops.version = KRN_CACHE_OPS_VERSION + 1UL;
                break;
            case 4U:
                mock_cache_ops.writeback_range = NULL;
                break;
            case 5U:
                mock_cache_ops.invalidate_range = NULL;
                break;
            default:
                returned_cache_ops = NULL;
                break;
        }
        clear_activity();
        check_cache_refusal_and_retry();
        CHECK(resource_calls >= 2U);
        CHECK(attribute_calls >= 1U);
    }
}

static void test_invalid_arguments_and_wrong_owner_are_nonmutating(void)
{
    ULONG saved_control;
    ULONG saved_size;
    ULONG saved_buffer;
    ULONG saved_next;
    begin_case();
    setup_host();
    dma_host.dma_data = test_data;
    dma_host.dma_writing = TRUE;
    descriptors[0].control = 0x12345678UL;
    descriptors[0].size = 17UL;
    descriptors[0].buffer = 23UL;
    descriptors[0].next = 31UL;
    saved_control = descriptors[0].control;
    saved_size = descriptors[0].size;
    saved_buffer = descriptors[0].buffer;
    saved_next = descriptors[0].next;

    CHECK(!p4host_dma_prepare(NULL, test_data, 64UL, FALSE));
    CHECK(!p4host_dma_prepare(&dma_host, NULL, 64UL, FALSE));
    CHECK(!p4host_dma_prepare(&dma_host, test_data, 0UL, FALSE));
    CHECK(!p4host_dma_prepare(&dma_host, test_data, P4HOST_DMA_MAX + 1UL,
                              FALSE));
    CHECK(!p4host_dma_prepare(
        &dma_host,
        (UBYTE *)(IPTR)-1,
        1UL,
        FALSE));
    CHECK(!p4host_dma_prepare(&dma_host, test_data, 64UL, 2));
    active_task = &task_b;
    CHECK(!p4host_dma_prepare(&dma_host, test_data, 64UL, FALSE));
    CHECK(dma_host.dma_data == test_data);
    CHECK(dma_host.dma_length == 0UL);
    CHECK(dma_host.dma_writing == TRUE);
    CHECK(dma_host.dma_faulted == FALSE);
    CHECK(descriptors[0].control == saved_control);
    CHECK(descriptors[0].size == saved_size);
    CHECK(descriptors[0].buffer == saved_buffer);
    CHECK(descriptors[0].next == saved_next);
    check_no_activity();
    CHECK(resource_calls == 0U);
    CHECK(attribute_calls == 0U);
}

static void check_descriptor_chain(ULONG length)
{
    ULONG count = (length + P4HOST_DMA_SEGMENT - 1UL) /
                  P4HOST_DMA_SEGMENT;
    ULONG offset = 0UL;
    ULONG index;
    CHECK(count > 0UL && count <= P4HOST_DMA_COUNT);
    for (index = 0UL; index < count; ++index)
    {
        ULONG bytes = length - offset;
        ULONG expected_control = P4HOST_DESC_CHAIN | P4HOST_DESC_OWN;
        ULONG expected_next;
        struct P4HostDMADesc *desc = &descriptors[index];
        if (bytes > P4HOST_DMA_SEGMENT)
            bytes = P4HOST_DMA_SEGMENT;
        if (index == 0UL)
            expected_control |= P4HOST_DESC_FIRST;
        if (index + 1UL == count)
            expected_control |= P4HOST_DESC_LAST;
        expected_next = index + 1UL == count ? 0UL :
            (ULONG)(IPTR)&descriptors[index + 1UL];
        CHECK(desc->control == expected_control && desc->size == bytes &&
              desc->buffer == (ULONG)(IPTR)(bounce + offset) &&
              desc->next == expected_next);
        offset += bytes;
    }
    CHECK(offset == length);
}

static void test_boundary_transfer_lengths_and_descriptor_fields(void)
{
    static const ULONG lengths[] = {
        1UL, 63UL, 64UL, 65UL, 511UL, 512UL, 513UL,
        1023UL, 1024UL, 1025UL, 65535UL, 65536UL
    };
    unsigned index;
    begin_case();
    setup_host();
    for (index = 0U; index < sizeof(lengths) / sizeof(lengths[0]); ++index)
    {
        ULONG length = lengths[index];
        ULONG expected_count =
            (length + P4HOST_DMA_SEGMENT - 1UL) / P4HOST_DMA_SEGMENT;
        ULONG byte;
        clear_activity();
        for (byte = 0UL; byte < length; ++byte)
            test_data[byte] = (UBYTE)(byte * 17UL + length);
        CHECK(p4host_dma_prepare(&dma_host, test_data, length, TRUE));
        CHECK(dma_host.dma_data == test_data);
        CHECK(dma_host.dma_length == length);
        CHECK(dma_host.dma_writing == TRUE);
        check_descriptor_chain(length);
        CHECK(cache_writeback_count == 2U);
        CHECK(cache_invalidate_count == 0U);
        CHECK(events[find_cache_event(EVENT_CACHE_WRITEBACK, bounce)].length ==
              rounded_cache_length(length));
        CHECK(memcmp(bounce, test_data, length) == 0);
        CHECK(registers[P4SD_DBADDR / sizeof(ULONG)] ==
              (ULONG)(IPTR)descriptors);
        CHECK(expected_count <= P4HOST_DMA_COUNT);
        if (index + 1U != sizeof(lengths) / sizeof(lengths[0]))
            CHECK(p4host_dma_reset(&dma_host));
    }
}

static void test_publish_after_cache_and_ready_matching_length(void)
{
    ULONG length = P4HOST_DMA_SEGMENT + 1UL;
    ULONG count = (length + P4HOST_DMA_SEGMENT - 1UL) /
                  P4HOST_DMA_SEGMENT;
    unsigned dbaddr;
    unsigned desc_wb;
    unsigned data_wb;
    unsigned index;
    begin_case();
    setup_host();
    CHECK(p4host_dma_prepare(&dma_host, test_data, length, FALSE));
    dbaddr = find_event(EVENT_MMIO_WRITE, P4SD_DBADDR);
    desc_wb = find_cache_event(EVENT_CACHE_WRITEBACK, descriptors);
    data_wb = find_cache_event(EVENT_CACHE_WRITEBACK, bounce);
    CHECK(desc_wb < dbaddr);
    CHECK(data_wb < dbaddr);
    CHECK(events[desc_wb].length == sizeof(descriptors));
    CHECK(events[data_wb].length == rounded_cache_length(length));
    CHECK(registers[P4SD_DBADDR / sizeof(ULONG)] ==
          (ULONG)(IPTR)descriptors);
    CHECK(((IPTR)descriptors & 63U) == 0U);

    clear_activity();
    CHECK(!p4host_dma_ready(&dma_host, 0UL));
    CHECK(!p4host_dma_ready(&dma_host, length - 1UL));
    CHECK(!p4host_dma_ready(&dma_host, length + 1UL));
    CHECK(!p4host_dma_ready(&dma_host, P4HOST_DMA_MAX + 1UL));
    CHECK(cache_invalidate_count == 0U);
    CHECK(read_count == 0U && write_count == 0U);
    for (index = 0U; index < count; ++index)
        descriptors[index].control &= ~P4HOST_DESC_OWN;
    CHECK(p4host_dma_ready(&dma_host, length));
    CHECK(cache_invalidate_count == 1U);
    CHECK(events[find_cache_event(EVENT_CACHE_INVALIDATE, descriptors)].length ==
          count * sizeof(descriptors[0]));
    CHECK(cache_writeback_count == 0U);
    CHECK(p4host_dma_reset(&dma_host));
}

static void finish_descriptors(ULONG length)
{
    ULONG count = (length + P4HOST_DMA_SEGMENT - 1UL) /
                  P4HOST_DMA_SEGMENT;
    ULONG index;
    for (index = 0UL; index < count; ++index)
        descriptors[index].control &= ~P4HOST_DESC_OWN;
}

static void test_rx_rounded_cache_and_fifo_settle(void)
{
    ULONG length = 65UL;
    ULONG data_cache_length = rounded_cache_length(length);
    ULONG before_settle;
    unsigned data_invalidate;
    ULONG index;
    begin_case();
    setup_host();
    memset(unaligned_data, 0xC7, sizeof(unaligned_data));
    CHECK(p4host_dma_prepare(&dma_host, unaligned_data + 1, length, FALSE));
    CHECK(dma_host.dma_data == unaligned_data + 1);
    CHECK(cache_writeback_count == 2U);
    CHECK(events[find_cache_event(EVENT_CACHE_WRITEBACK, bounce)].length ==
          data_cache_length);
    CHECK(unaligned_data[0] == 0xC7U);
    CHECK(unaligned_data[length + 1UL] == 0xC7U);
    for (index = 0UL; index < length; ++index)
        bounce[index] = (UBYTE)(index * 3UL + 7UL);
    finish_descriptors(length);
    clear_activity();
    timer_value = 0UL;
    before_settle = timer_value;
    CHECK(p4host_dma_complete(&dma_host, unaligned_data + 1, length));
    CHECK(timer_value - before_settle >= 20UL);
    CHECK(dma_host.dma_data == NULL);
    CHECK(dma_host.dma_length == 0UL);
    CHECK(dma_host.dma_writing == FALSE);
    CHECK(cache_invalidate_count == 2U);
    CHECK(events[find_cache_event(EVENT_CACHE_INVALIDATE, descriptors)].length ==
          sizeof(descriptors));
    data_invalidate = find_cache_event(EVENT_CACHE_INVALIDATE, bounce);
    CHECK(events[data_invalidate].length == data_cache_length);
    CHECK(data_invalidate > find_event(EVENT_MMIO_WRITE, P4SD_CTRL));
    CHECK(unaligned_data[0] == 0xC7U);
    CHECK(unaligned_data[length + 1UL] == 0xC7U);
    for (index = 0UL; index < length; ++index)
        CHECK(unaligned_data[index + 1UL] == (UBYTE)(index * 3UL + 7UL));
    CHECK(registers[P4SD_CTRL / sizeof(ULONG)] == 0UL);
}

static void test_tx_does_not_invalidate_client_data(void)
{
    ULONG length = 129UL;
    ULONG byte;
    unsigned index;
    begin_case();
    setup_host();
    memset(unaligned_data, 0xD9, sizeof(unaligned_data));
    for (byte = 0UL; byte < length; ++byte)
        unaligned_data[byte + 1UL] = (UBYTE)(byte * 11UL + 3UL);
    CHECK(p4host_dma_prepare(&dma_host, unaligned_data + 1, length, TRUE));
    CHECK(memcmp(bounce, unaligned_data + 1, length) == 0);
    CHECK(unaligned_data[0] == 0xD9U);
    CHECK(unaligned_data[length + 1UL] == 0xD9U);
    finish_descriptors(length);
    clear_activity();
    CHECK(p4host_dma_complete(&dma_host, unaligned_data + 1, length));
    CHECK(cache_writeback_count == 0U);
    CHECK(cache_invalidate_count == 1U);
    for (index = 0U; index < event_count; ++index)
        if (events[index].kind == EVENT_CACHE_INVALIDATE)
            CHECK(events[index].address != unaligned_data + 1 &&
                  events[index].address == descriptors);
    CHECK(dma_host.dma_data == NULL);
    CHECK(dma_host.dma_length == 0UL);
    CHECK(dma_host.dma_writing == FALSE);
}

static void prepare_completed_rx(ULONG length)
{
    setup_host();
    memset(test_data, 0xA5, length);
    CHECK(p4host_dma_prepare(&dma_host, test_data, length, FALSE));
    finish_descriptors(length);
    clear_activity();
}

static void check_completion_refusal_keeps_transfer(void)
{
    CHECK(dma_host.dma_data == test_data);
    CHECK(dma_host.dma_length == 513UL);
    CHECK(dma_host.dma_writing == FALSE);
    CHECK(dma_host.dma_faulted == FALSE);
    CHECK(read_count <= 1U);
    CHECK(writes_to(P4SD_BMOD) == 0U);
    CHECK(writes_to(P4SD_CTRL) == 0U);
    CHECK(cache_invalidate_count == 1U);
    CHECK(cache_writeback_count == 0U);
    CHECK(events[find_cache_event(EVENT_CACHE_INVALIDATE, descriptors)].length ==
          sizeof(descriptors));
    {
        ULONG index;
        for (index = 0UL; index < 513UL; ++index)
            CHECK(test_data[index] == 0xA5U);
    }
}

static unsigned writes_to(ULONG offset)
{
    unsigned index;
    unsigned count = 0U;
    for (index = 0U; index < write_count; ++index)
        if (writes[index].offset == offset)
            ++count;
    return count;
}

static void test_completion_error_and_malformed_descriptors_refuse(void)
{
    begin_case();
    prepare_completed_rx(513UL);
    registers[P4SD_IDSTS / sizeof(ULONG)] = P4SD_IDSTS_ERRORS;
    CHECK(!p4host_dma_complete(&dma_host, test_data, 513UL));
    check_completion_refusal_keeps_transfer();

    prepare_completed_rx(513UL);
    descriptors[0].control |= P4HOST_DESC_OWN;
    CHECK(!p4host_dma_complete(&dma_host, test_data, 513UL));
    check_completion_refusal_keeps_transfer();

    prepare_completed_rx(513UL);
    descriptors[1].control &= ~P4HOST_DESC_LAST;
    CHECK(!p4host_dma_complete(&dma_host, test_data, 513UL));
    check_completion_refusal_keeps_transfer();

    prepare_completed_rx(513UL);
    descriptors[0].control |= P4HOST_DESC_LAST;
    CHECK(!p4host_dma_complete(&dma_host, test_data, 513UL));
    check_completion_refusal_keeps_transfer();
}

static void test_active_transfer_refusals_are_nonmutating(void)
{
    ULONG saved_control;
    unsigned saved_reads;
    begin_case();
    setup_host();
    CHECK(p4host_dma_prepare(&dma_host, test_data, 513UL, FALSE));
    saved_control = descriptors[0].control;
    clear_activity();
    CHECK(!p4host_dma_prepare(&dma_host, test_data, 512UL, TRUE));
    CHECK(!p4host_dma_ready(&dma_host, 512UL));
    CHECK(!p4host_dma_complete(&dma_host, test_data, 512UL));
    CHECK(!p4host_dma_complete(&dma_host, unaligned_data, 513UL));
    saved_reads = read_count;
    CHECK(saved_reads == 0U);
    check_no_activity();
    CHECK(dma_host.dma_data == test_data);
    CHECK(dma_host.dma_length == 513UL);
    CHECK(dma_host.dma_writing == FALSE);
    CHECK(dma_host.dma_faulted == FALSE);
    CHECK(descriptors[0].control == saved_control);
    CHECK(p4host_dma_reset(&dma_host));
}

static void test_faulted_engine_refuses_until_reset_retry(void)
{
    begin_case();
    setup_host();
    CHECK(p4host_dma_prepare(&dma_host, test_data, 513UL, TRUE));
    clear_activity();
    stuck_bmod = TRUE;
    timer_step = 0UL;
    CHECK(!p4host_dma_reset(&dma_host));
    CHECK(dma_host.dma_data == test_data);
    CHECK(dma_host.dma_length == 513UL);
    CHECK(dma_host.dma_writing == TRUE);
    CHECK(dma_host.dma_faulted == TRUE);
    CHECK(invalidate_count == 1U);

    clear_activity();
    CHECK(!p4host_dma_prepare(&dma_host, test_data, 64UL, FALSE));
    CHECK(!p4host_dma_ready(&dma_host, 513UL));
    CHECK(!p4host_dma_complete(&dma_host, test_data, 513UL));
    check_no_activity();
    CHECK(dma_host.dma_data == test_data);
    CHECK(dma_host.dma_length == 513UL);
    CHECK(dma_host.dma_writing == TRUE);

    stuck_bmod = FALSE;
    timer_step = 1UL;
    CHECK(p4host_dma_reset(&dma_host));
    CHECK(dma_host.dma_data == NULL);
    CHECK(dma_host.dma_length == 0UL);
    CHECK(dma_host.dma_writing == FALSE);
    CHECK(dma_host.dma_faulted == FALSE);
    CHECK(p4host_dma_prepare(&dma_host, test_data, 64UL, FALSE));
}

static void test_reset_stuck_bmod_and_ctrl_with_timer_bounds(void)
{
    begin_case();
    setup_host();
    CHECK(p4host_dma_prepare(&dma_host, test_data, 64UL, TRUE));
    clear_activity();
    timer_value = 0UL;
    now_call_count = 0UL;
    stuck_bmod = TRUE;
    timer_step = 0UL;
    CHECK(!p4host_dma_reset(&dma_host));
    CHECK(now_call_count == 1000000UL);
    CHECK(read_count == 1000001U);
    CHECK(writes_to(P4SD_CTRL) == 0U);
    CHECK(dma_host.dma_data == test_data);
    CHECK(dma_host.dma_length == 64UL);
    CHECK(dma_host.dma_writing == TRUE);
    CHECK(dma_host.dma_faulted == TRUE);

    setup_host();
    CHECK(p4host_dma_prepare(&dma_host, test_data, 64UL, TRUE));
    clear_activity();
    now_call_count = 0UL;
    stuck_ctrl_mask = P4SD_CTRL_DMA_RESET;
    timer_value = (ULONG)-11;
    CHECK(!p4host_dma_reset(&dma_host));
    CHECK(timer_value - (ULONG)-11 >= 250000UL);
    CHECK(now_call_count == 250002UL);
    CHECK(writes_to(P4SD_IDSTS) == 0U);
    CHECK((registers[P4SD_CTRL / sizeof(ULONG)] & P4SD_CTRL_DMA_RESET) != 0UL);
    CHECK(dma_host.dma_data == test_data);
    CHECK(dma_host.dma_length == 64UL);
    CHECK(dma_host.dma_writing == TRUE);
    CHECK(dma_host.dma_faulted == TRUE);
}

static void test_fifo_settle_stopped_timer_is_poll_bounded(void)
{
    begin_case();
    setup_host();
    timer_step = 0UL;
    CHECK(!p4host_fifo_reset(&dma_host));
    CHECK(now_call_count == 1000002UL);
    CHECK(read_count == 4U);
    CHECK(writes_to(P4SD_CTRL) == 1U);
    CHECK(registers[P4SD_CTRL / sizeof(ULONG)] == 0UL);
}

static void test_wrong_owner_refusal_and_inspect(void)
{
    begin_case();
    setup_host();
    active_task = &task_b;
    CHECK(!p4host_fifo_reset(&dma_host));
    CHECK(!p4host_dma_reset(&dma_host));
    CHECK(!p4host_dma_ready(&dma_host, 64UL));
    CHECK(!p4host_dma_complete(&dma_host, test_data, 64UL));
    p4host_dma_inspect(&dma_host);
    check_no_activity();
    active_task = &task_a;
    p4host_dma_inspect(&dma_host);
    CHECK(cache_invalidate_count == 0U);
}

static void test_active_fifo_reset_refuses_without_mutation(void)
{
    begin_case();
    setup_host();
    CHECK(p4host_dma_prepare(&dma_host, test_data, 64UL, FALSE));
    clear_activity();
    CHECK(!p4host_fifo_reset(&dma_host));
    CHECK(read_count == 1U);
    CHECK(write_count == 0U);
    CHECK(cache_writeback_count == 0U);
    CHECK(cache_invalidate_count == 0U);
    CHECK(dma_host.dma_data == test_data);
    CHECK(dma_host.dma_length == 64UL);
    CHECK(dma_host.dma_faulted == FALSE);
    CHECK((registers[P4SD_CTRL / sizeof(ULONG)] & P4SD_CTRL_DMA_ENABLE) != 0UL);
    CHECK((registers[P4SD_BMOD / sizeof(ULONG)] & P4SD_BMOD_DE) != 0UL);
}

static void test_ready_and_complete_refuse_missing_cache_ops(void)
{
    begin_case();
    setup_host();
    CHECK(p4host_dma_prepare(&dma_host, test_data, 64UL, FALSE));
    cache_ops = NULL;
    clear_activity();
    CHECK(!p4host_dma_ready(&dma_host, 64UL));
    CHECK(!p4host_dma_complete(&dma_host, test_data, 64UL));
    check_no_activity();
    CHECK(dma_host.dma_data == test_data);
    CHECK(dma_host.dma_length == 64UL);
    CHECK(dma_host.dma_faulted == FALSE);
}

int main(void)
{
    test_descriptor_layout_and_init();
    test_cache_lookup_refusals_retry();
    test_invalid_arguments_and_wrong_owner_are_nonmutating();
    test_boundary_transfer_lengths_and_descriptor_fields();
    test_publish_after_cache_and_ready_matching_length();
    test_rx_rounded_cache_and_fifo_settle();
    test_tx_does_not_invalidate_client_data();
    test_completion_error_and_malformed_descriptors_refuse();
    test_active_transfer_refusals_are_nonmutating();
    test_faulted_engine_refuses_until_reset_retry();
    test_reset_stuck_bmod_and_ctrl_with_timer_bounds();
    test_fifo_settle_stopped_timer_is_poll_bounded();
    test_wrong_owner_refusal_and_inspect();
    test_active_fifo_reset_refuses_without_mutation();
    test_ready_and_complete_refuse_missing_cache_ops();
    printf("PASS: %u cases, %u checks\n", test_cases, checks);
    return 0;
}
