/* Host fixture for the production SDMMC TX-staging privacy lifecycle.
 * The runner inserts the actual DMA, command and full-reset function bodies. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t ULONG;
typedef uintptr_t IPTR;
typedef uint8_t UBYTE;
typedef int16_t BOOL;
typedef void *APTR;

#define TRUE ((BOOL)1)
#define FALSE ((BOOL)0)
#define EVENT_CAPACITY 4096U
#define REG_CAPACITY 128U

/* INSERT_ACTUAL_PLATFORM_CONSTANTS */

struct Task
{
    unsigned id;
};

struct SignalSemaphore
{
    struct Task *ss_Owner;
};

struct P4SDMMCHost;

/* Keep the production public headers, but replace only the platform host ABI. */
#define ESP32P4_SDMMC_HOST_H
#include "clock.h"
#include "command.h"
#include "dma.h"
#include "registers.h"

/* INSERT_HOST_ULONG_CONSTANTS */

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
    void (*invalidate)(struct P4SDMMCHost *);
    BOOL (*fifo_reset)(struct P4SDMMCHost *);
    BOOL (*dma_reset)(struct P4SDMMCHost *);
    BOOL (*dma_prepare)(struct P4SDMMCHost *, UBYTE *, ULONG, BOOL);
    BOOL (*dma_ready)(struct P4SDMMCHost *, ULONG);
    BOOL (*dma_complete)(struct P4SDMMCHost *, UBYTE *, ULONG);
    ULONG generation;
    unsigned host_div;
    ULONG slot_clock[2];
    APTR dma_desc;
    UBYTE *bounce;
    UBYTE *dma_data;
    ULONG dma_length;
    BOOL dma_writing;
    BOOL dma_faulted;
    BOOL initialization_started;
    ULONG budget_start;
    ULONG budget_us;
};

enum event_kind
{
    EVENT_MMIO_READ = 1,
    EVENT_MMIO_WRITE,
    EVENT_CACHE_WRITEBACK,
    EVENT_CACHE_INVALIDATE,
    EVENT_RESET_SETTLED,
    EVENT_COMMAND_START
};

struct event
{
    enum event_kind kind;
    IPTR address;
    ULONG value;
    APTR buffer;
    ULONG length;
    BOOL dma_active;
};

struct register_cell
{
    IPTR address;
    ULONG value;
};

static struct Task test_task = { 1U };
static struct Task *active_task = &test_task;
static struct P4SDMMCHost dma_host;
static struct Library kernel_library;
static struct KrnCacheOps test_cache_ops;
static struct KrnCacheOps *cache_ops_result = &test_cache_ops;
static struct register_cell register_cells[REG_CAPACITY];
static unsigned register_count;
static struct event events[EVENT_CAPACITY];
static unsigned event_count;
static UBYTE bounce_ram[P4HOST_DMA_MAX];
static struct P4HostDMADesc descriptor_ram[P4HOST_DMA_COUNT];
static UBYTE payload[P4HOST_DMA_MAX + 64UL];
static UBYTE receive[P4HOST_DMA_MAX + 64UL];
static UBYTE *expected_tx_client;
static ULONG timer_value;
static ULONG timer_step;
static ULONG invalidate_count;
static ULONG scrub_count;
static ULONG scrub_faulted_count;
static ULONG scrub_while_active_count;
static ULONG tx_stage_count;
static ULONG command_count;
static ULONG mmio_write_count;
static ULONG checks;
static ULONG cases;
static BOOL dma_active;
static BOOL stuck_bmod_swr;
static BOOL fail_cleanup_reset;
static BOOL stuck_ctrl_dma_reset;
static BOOL stuck_ctrl_reset_all;
static BOOL inject_command_error;
static BOOL inject_rx_final_error;
static BOOL expire_during_tx_stage;
static BOOL expire_during_full_reset;
static ULONG fifo_clear_time;
static ULONG all_reset_clear_time;
static ULONG dma_epoch;
static ULONG fifo_clear_epoch;
static ULONG all_reset_clear_epoch;
static BOOL fifo_clear_seen;
static BOOL all_reset_clear_seen;

static void maybe_mark_dma_idle(void);

static ULONG *register_value(IPTR address)
{
    unsigned index;
    for (index = 0; index < register_count; ++index)
        if (register_cells[index].address == address)
            return &register_cells[index].value;
    if (register_count >= REG_CAPACITY)
    {
        fprintf(stderr, "FAIL: register model capacity exhausted\n");
        exit(1);
    }
    register_cells[register_count].address = address;
    register_cells[register_count].value = 0;
    ++register_count;
    return &register_cells[register_count - 1U].value;
}

static void record(enum event_kind kind, IPTR address, ULONG value,
                   APTR buffer, ULONG length)
{
    if (event_count < EVENT_CAPACITY)
    {
        events[event_count].kind = kind;
        events[event_count].address = address;
        events[event_count].value = value;
        events[event_count].buffer = buffer;
        events[event_count].length = length;
        events[event_count].dma_active = dma_active;
        ++event_count;
    }
    if (kind == EVENT_MMIO_WRITE)
        ++mmio_write_count;
}

static BOOL tx_residue_pending(void);

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
            fail_check(#expression, __FILE__, __LINE__);                    \
    } while (0)

static ULONG mock_now_us(void)
{
    ULONG result = timer_value;
    timer_value += timer_step;
    maybe_mark_dma_idle();
    return result;
}

static struct Task *FindTask(const char *name)
{
    (void)name;
    return active_task;
}

static struct Library *OpenResource(const char *name)
{
    CHECK(strcmp(name, "kernel.resource") == 0);
    return &kernel_library;
}

static IPTR KrnGetSystemAttr(ULONG attribute)
{
    CHECK(attribute == KATTR_CacheOps);
    return (IPTR)cache_ops_result;
}

static BOOL bytes_are_zero(const UBYTE *bytes, ULONG length)
{
    ULONG index;
    for (index = 0; index < length; ++index)
        if (bytes[index] != 0)
            return FALSE;
    return TRUE;
}

static BOOL bytes_have_nonzero(const UBYTE *bytes, ULONG length)
{
    ULONG index;
    for (index = 0; index < length; ++index)
        if (bytes[index] != 0)
            return TRUE;
    return FALSE;
}

static void maybe_mark_dma_idle(void)
{
    ULONG ctrl = *register_value(P4_SDMMC_BASE + P4SD_CTRL);
    ULONG bmod = *register_value(P4_SDMMC_BASE + P4SD_BMOD);
    BOOL controls_off = !(ctrl & (P4SD_CTRL_DMA_ENABLE | P4SD_CTRL_DMA_RESET |
                                  P4SD_CTRL_FIFO_RESET | P4SD_CTRL_RESET_ALL)) &&
                        !(bmod & (P4SD_BMOD_DE | P4SD_BMOD_SWR));
    BOOL fifo_settled = fifo_clear_seen && fifo_clear_epoch == dma_epoch &&
        (ULONG)(timer_value - fifo_clear_time) >= 20UL;
    BOOL all_reset_settled = all_reset_clear_seen &&
        all_reset_clear_epoch == dma_epoch &&
        (ULONG)(timer_value - all_reset_clear_time) >= 20UL;
    if (dma_active && controls_off && (fifo_settled || all_reset_settled))
    {
        dma_active = FALSE;
        record(EVENT_RESET_SETTLED, P4_SDMMC_BASE + P4SD_CTRL, 0UL, NULL, 0UL);
    }
}

static void mock_writeback_range(APTR address, ULONG length)
{
    UBYTE *buffer = address;
    BOOL is_bounce = address == (APTR)dma_host.bounce;
    ULONG safe_length = length;
    maybe_mark_dma_idle();
    record(EVENT_CACHE_WRITEBACK, 0UL, 0UL, address, length);
    if (is_bounce)
    {
        if (safe_length > P4HOST_DMA_MAX)
            safe_length = P4HOST_DMA_MAX;
        if (bytes_are_zero(buffer, safe_length) && tx_residue_pending())
        {
            ++scrub_count;
            CHECK((fifo_clear_seen && fifo_clear_epoch == dma_epoch) ||
                  (all_reset_clear_seen &&
                   all_reset_clear_epoch == dma_epoch));
            if (dma_host.dma_faulted)
                ++scrub_faulted_count;
            if (dma_active)
                ++scrub_while_active_count;
            CHECK(!dma_active);
            CHECK(!( *register_value(P4_SDMMC_BASE + P4SD_CTRL) &
                      (P4SD_CTRL_DMA_ENABLE | P4SD_CTRL_DMA_RESET |
                       P4SD_CTRL_FIFO_RESET | P4SD_CTRL_RESET_ALL)));
            CHECK(!( *register_value(P4_SDMMC_BASE + P4SD_BMOD) &
                      (P4SD_BMOD_DE | P4SD_BMOD_SWR)));
        }
        else
        {
            ++tx_stage_count;
            CHECK(!dma_active);
            if (expire_during_tx_stage)
            {
                expire_during_tx_stage = FALSE;
                timer_value += (ULONG)(dma_host.budget_us + (ULONG)1U);
            }
        }
        memcpy(bounce_ram, buffer, safe_length);
    }
    else if (address == dma_host.dma_desc)
    {
        CHECK(length <= sizeof(descriptor_ram));
        memcpy(descriptor_ram, buffer, length);
    }
    else
    {
        CHECK(FALSE);
    }
}

static void mock_invalidate_range(APTR address, ULONG length)
{
    ++invalidate_count;
    record(EVENT_CACHE_INVALIDATE, 0UL, 0UL, address, length);
    if (address == dma_host.dma_desc)
    {
        CHECK(length <= sizeof(descriptor_ram));
        memcpy(address, descriptor_ram, length);
    }
    else if (address == (APTR)dma_host.bounce)
    {
        CHECK(length <= sizeof(bounce_ram));
        memcpy(address, bounce_ram, length);
        if (inject_rx_final_error)
        {
            inject_rx_final_error = FALSE;
            *register_value(P4_SDMMC_BASE + P4SD_RINTSTS) |= P4SD_INT_DCRC;
        }
    }
    else
    {
        CHECK(FALSE);
    }
}

static ULONG model_sdmmc_read(ULONG offset)
{
    ULONG address = P4_SDMMC_BASE + offset;
    ULONG *cell = register_value(address);
    ULONG value = *cell;
    if (offset == P4SD_CTRL)
    {
        ULONG reset = value & (P4SD_CTRL_RESET_ALL | P4SD_CTRL_DMA_RESET |
                               P4SD_CTRL_FIFO_RESET);
        if (reset & P4SD_CTRL_RESET_ALL)
        {
            if (!stuck_ctrl_reset_all)
            {
                *cell = 0;
                all_reset_clear_seen = TRUE;
                all_reset_clear_time = timer_value;
                all_reset_clear_epoch = dma_epoch;
                value = 0;
            }
        }
        else
        {
            if ((reset & P4SD_CTRL_DMA_RESET) && !stuck_ctrl_dma_reset)
            {
                *cell &= ~P4SD_CTRL_DMA_RESET;
                value = *cell;
            }
            if (reset & P4SD_CTRL_FIFO_RESET)
            {
                *cell &= ~P4SD_CTRL_FIFO_RESET;
                fifo_clear_seen = TRUE;
                fifo_clear_time = timer_value;
                fifo_clear_epoch = dma_epoch;
                value = *cell;
            }
        }
    }
    else if (offset == P4SD_BMOD && (*cell & P4SD_BMOD_SWR) &&
             !stuck_bmod_swr)
    {
        *cell &= ~P4SD_BMOD_SWR;
        value = *cell;
    }
    else if (offset == P4SD_CMD)
    {
        *cell &= ~P4SD_CMD_START;
        value = *cell;
    }
    else if (offset == P4SD_RINTSTS && inject_command_error && command_count)
    {
        inject_command_error = FALSE;
        *cell |= P4SD_INT_DCRC;
        value = *cell;
    }
    record(EVENT_MMIO_READ, address, value, NULL, 0UL);
    return value;
}

static void model_sdmmc_write(ULONG offset, ULONG value)
{
    ULONG address = P4_SDMMC_BASE + offset;
    ULONG *cell = register_value(address);
    if (offset == P4SD_RINTSTS)
    {
        *cell &= ~(value & P4SD_INT_CMD_DATA_MASK);
    }
    else if (offset == P4SD_IDSTS)
    {
        *cell &= ~value;
    }
    else if (offset == P4SD_CMD && (value & P4SD_CMD_START))
    {
        ULONG length = *register_value(P4_SDMMC_BASE + P4SD_BYTCNT);
        ULONG count = (ULONG)(length + P4HOST_DMA_SEGMENT - (ULONG)1U) /
                      P4HOST_DMA_SEGMENT;
        ULONG index;
        ++command_count;
        record(EVENT_COMMAND_START, address, value, NULL, 0UL);
        if (fail_cleanup_reset)
        {
            fail_cleanup_reset = FALSE;
            stuck_bmod_swr = TRUE;
        }
        *cell = value & ~P4SD_CMD_START;
        *register_value(P4_SDMMC_BASE + P4SD_RINTSTS) =
            P4SD_INT_CMD_DONE | ((value & P4SD_CMD_DATA) ? P4SD_INT_DATA_OVER : 0UL);
        if (value & P4SD_CMD_DATA)
        {
            CHECK(dma_active);
            if (value & P4SD_CMD_WRITE)
            {
                CHECK(expected_tx_client != NULL);
                CHECK(dma_host.dma_data == expected_tx_client);
                CHECK(memcmp(bounce_ram, expected_tx_client, length) == 0);
            }
            for (index = 0; index < count; ++index)
                descriptor_ram[index].control &= ~P4HOST_DESC_OWN;
            if (!(value & P4SD_CMD_WRITE))
            {
                for (index = 0; index < length; ++index)
                    bounce_ram[index] = (UBYTE)0xA5U;
            }
        }
    }
    else if (offset == P4SD_CTRL && value == 0UL && all_reset_clear_seen &&
             expire_during_full_reset)
    {
        *cell = value;
        expire_during_full_reset = FALSE;
        /* This write follows the successful RESET_ALL poll. The physical
         * reset is complete; only the remaining caller budget is expired. */
        timer_value += (ULONG)(dma_host.budget_us + (ULONG)1U);
    }
    else
    {
        *cell = value;
    }
    if ((offset == P4SD_CTRL && (value & P4SD_CTRL_DMA_ENABLE)) ||
        (offset == P4SD_BMOD && (value & P4SD_BMOD_DE)))
    {
        if (!dma_active)
        {
            ++dma_epoch;
            fifo_clear_seen = FALSE;
            all_reset_clear_seen = FALSE;
        }
        dma_active = TRUE;
    }
    record(EVENT_MMIO_WRITE, address, value, NULL, 0UL);
}

static ULONG model_clock_read(IPTR address)
{
    ULONG value = *register_value(address);
    if (address == (IPTR)(P4_SDMMC_BASE + P4SD_CTRL))
        value = model_sdmmc_read(P4SD_CTRL);
    else if (address == (IPTR)(P4_SDMMC_BASE + P4SD_CMD))
        value = model_sdmmc_read(P4SD_CMD);
    else if (address == (IPTR)(P4_SDMMC_BASE + P4SD_RINTSTS))
        value = model_sdmmc_read(P4SD_RINTSTS);
    record(EVENT_MMIO_READ, address, value, NULL, 0UL);
    return value;
}

static void model_clock_write(IPTR address, ULONG value)
{
    if (address >= (IPTR)P4_SDMMC_BASE &&
        address <= (IPTR)(P4_SDMMC_BASE + P4SD_CLK_EDGE_SEL))
    {
        ULONG offset = (ULONG)(address - (IPTR)P4_SDMMC_BASE);
        model_sdmmc_write(offset, value);
        return;
    }
    *register_value(address) = value;
    record(EVENT_MMIO_WRITE, address, value, NULL, 0UL);
}

static void mock_invalidate_host(struct P4SDMMCHost *host)
{
    CHECK(host == &dma_host);
    ++host->generation;
    ++invalidate_count;
}

static void modeled_memory_barrier(void)
{
    /* Compiler barrier from production is represented by source ordering. */
}

/* INSERT_ACTUAL_BUDGET_HELPER */
/* INSERT_ACTUAL_SOURCE */

static BOOL tx_residue_pending(void)
{
    return tx_residue != 0UL;
}

static void reset_model(void)
{
    memset(&dma_host, 0, sizeof(dma_host));
    memset(register_cells, 0, sizeof(register_cells));
    memset(events, 0, sizeof(events));
    memset(bounce_ram, 0, sizeof(bounce_ram));
    memset(descriptor_ram, 0, sizeof(descriptor_ram));
    memset(payload, 0, sizeof(payload));
    memset(receive, 0x5A, sizeof(receive));
    expected_tx_client = NULL;
    register_count = 0;
    event_count = 0;
    timer_value = 100UL;
    timer_step = 1UL;
    invalidate_count = 0;
    scrub_count = 0;
    scrub_faulted_count = 0;
    scrub_while_active_count = 0;
    tx_stage_count = 0;
    command_count = 0;
    mmio_write_count = 0;
    dma_active = FALSE;
    stuck_bmod_swr = FALSE;
    fail_cleanup_reset = FALSE;
    stuck_ctrl_dma_reset = FALSE;
    stuck_ctrl_reset_all = FALSE;
    inject_command_error = FALSE;
    inject_rx_final_error = FALSE;
    expire_during_tx_stage = FALSE;
    expire_during_full_reset = FALSE;
    fifo_clear_time = 0;
    all_reset_clear_time = 0;
    dma_epoch = 0;
    fifo_clear_epoch = 0;
    all_reset_clear_epoch = 0;
    fifo_clear_seen = FALSE;
    all_reset_clear_seen = FALSE;
    active_task = &test_task;
    test_cache_ops.version = KRN_CACHE_OPS_VERSION;
    test_cache_ops.writeback_range = mock_writeback_range;
    test_cache_ops.invalidate_range = mock_invalidate_range;
    cache_ops_result = &test_cache_ops;
    dma_host.lock.ss_Owner = &test_task;
    dma_host.now_us = mock_now_us;
    dma_host.invalidate = mock_invalidate_host;
    dma_host.generation = 1UL;
    p4host_dma_init(&dma_host);
    dma_host.fifo_reset = p4host_fifo_reset;
    dma_host.dma_reset = p4host_dma_reset;
    dma_host.dma_prepare = p4host_dma_prepare;
    dma_host.dma_ready = p4host_dma_ready;
    dma_host.dma_complete = p4host_dma_complete;
    CHECK(p4host_reset(&dma_host));
    CHECK(!dma_host.dma_faulted);
    CHECK(dma_host.initialization_started);
    dma_host.slot_clock[0] = 400000UL;
}

static struct P4HostCommand make_write(UBYTE *data, ULONG length)
{
    struct P4HostCommand request;
    memset(&request, 0, sizeof(request));
    request.slot = 0U;
    request.generation = dma_host.generation;
    request.index = 24UL;
    request.argument = 0x13579BDFUL;
    request.response_flags = MMC_RSP_PRESENT | MMC_RSP_CRC;
    request.data = data;
    request.length = length;
    request.block_size = length;
    request.writing = TRUE;
    expected_tx_client = data;
    request.diagnostic_fault = P4HOST_FAULT_NONE;
    return request;
}

static struct P4HostCommand make_read(UBYTE *data, ULONG length)
{
    struct P4HostCommand request = make_write(data, length);
    request.index = 17UL;
    request.writing = FALSE;
    return request;
}

static void fill_secret(UBYTE *data, ULONG length, UBYTE seed)
{
    ULONG index;
    for (index = 0; index < length; ++index)
        data[index] = (UBYTE)(seed + (UBYTE)(index % 239UL));
}

static void check_bounce_erased(void)
{
    ULONG extent = P4HOST_DMA_MAX;
    CHECK(bytes_are_zero((const UBYTE *)dma_host.bounce, extent));
    CHECK(bytes_are_zero(bounce_ram, extent));
}

static void test_success_and_shorter_followup(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;
    BOOL ok;
    ++cases;
    reset_model();
    fill_secret(payload, 1024UL, (UBYTE)0x31U);
    request = make_write(payload, 1024UL);
    ok = p4host_command(&dma_host, &request, &result);
    CHECK(ok && result.status == P4HOST_COMMAND_OK && result.started);
    CHECK(command_count == 1UL && tx_stage_count == 1UL);
    CHECK(scrub_count == 1UL && scrub_while_active_count == 0UL);
    check_bounce_erased();
    CHECK(!dma_active && !dma_host.dma_faulted && !dma_host.dma_length);

    fill_secret(payload, 32UL, (UBYTE)0x71U);
    request = make_write(payload, 32UL);
    request.generation = dma_host.generation;
    ok = p4host_command(&dma_host, &request, &result);
    CHECK(ok && result.status == P4HOST_COMMAND_OK);
    CHECK(tx_stage_count == 2UL && scrub_count == 2UL);
    check_bounce_erased();
    CHECK(scrub_while_active_count == 0UL);
}

static void test_tx_alias_is_refused_before_touching_dma(void)
{
    ULONG writes_before;
    ULONG invalidations_before;
    BOOL ok;
    ++cases;
    reset_model();
    writes_before = mmio_write_count;
    invalidations_before = invalidate_count;
    ok = p4host_dma_prepare(&dma_host, dma_host.bounce + 16UL, 128UL, TRUE);
    CHECK(!ok);
    CHECK(mmio_write_count == writes_before);
    CHECK(invalidate_count == invalidations_before);
    CHECK(!tx_residue_pending() && !dma_host.dma_length && !dma_active);
    CHECK(!tx_stage_count && !command_count);
}

static void test_pre_enable_budget_expiry(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;
    BOOL ok;
    ++cases;
    reset_model();
    fill_secret(payload, 512UL, (UBYTE)0x43U);
    request = make_write(payload, 512UL);
    dma_host.budget_start = timer_value;
    dma_host.budget_us = 40UL;
    expire_during_tx_stage = TRUE;
    ok = p4host_command(&dma_host, &request, &result);
    CHECK(!ok && result.status == P4HOST_COMMAND_DMA_ERROR);
    CHECK(!result.started && !command_count && tx_stage_count == 1UL);
    CHECK(scrub_count == 1UL && !dma_active && !dma_host.dma_faulted);
    CHECK(!p4host_budget_ok(&dma_host));
    check_bounce_erased();
}

static void test_command_error_resets_and_scrubs(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;
    BOOL ok;
    ++cases;
    reset_model();
    fill_secret(payload, 512UL, (UBYTE)0x56U);
    request = make_write(payload, 512UL);
    inject_command_error = TRUE;
    ok = p4host_command(&dma_host, &request, &result);
    CHECK(!ok && result.status == P4HOST_COMMAND_ERROR && result.started);
    CHECK(scrub_count == 1UL && scrub_while_active_count == 0UL);
    CHECK(!dma_host.dma_faulted && !dma_active);
    check_bounce_erased();
}

static void test_failed_reset_preserves_then_full_reset_scrubs(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;
    BOOL ok;
    ++cases;
    reset_model();
    fill_secret(payload, 512UL, (UBYTE)0x68U);
    request = make_write(payload, 512UL);
    inject_command_error = TRUE;
    fail_cleanup_reset = TRUE;
    timer_step = 30000UL;
    ok = p4host_command(&dma_host, &request, &result);
    CHECK(!ok && result.status == P4HOST_COMMAND_ERROR && result.started);
    CHECK(dma_host.dma_faulted && dma_active);
    CHECK(scrub_count == 0UL && scrub_while_active_count == 0UL);
    CHECK(bytes_have_nonzero(bounce_ram, 512UL));
    CHECK(bytes_have_nonzero(dma_host.bounce, 512UL));
    CHECK(dma_host.dma_data == payload && dma_host.dma_length == 512UL);

    stuck_bmod_swr = FALSE;
    timer_step = 1UL;

    /* Physical reset succeeds, but the serialized cache API is unavailable.
     * Sticky residue and fault state must survive; secrets stay untouched. */
    cache_ops = NULL;
    cache_ops_result = NULL;
    ok = p4host_reset(&dma_host);
    CHECK(!ok && dma_host.dma_faulted && !dma_active);
    CHECK(scrub_count == 0UL && tx_residue_pending());
    CHECK(bytes_have_nonzero(bounce_ram, 512UL));
    CHECK(bytes_have_nonzero(dma_host.bounce, 512UL));
    CHECK(dma_host.dma_data == payload && dma_host.dma_length == 512UL);

    /* Cache support returns. Scrub must occur after RESET_ALL even when the
     * operation budget expires immediately after physical reset completes. */
    cache_ops_result = &test_cache_ops;
    dma_host.budget_start = timer_value;
    dma_host.budget_us = 1000UL;
    expire_during_full_reset = TRUE;
    ok = p4host_reset(&dma_host);
    CHECK(!ok); /* Reset is physically complete, but caller budget expired. */
    CHECK(scrub_count == 1UL && scrub_faulted_count == 1UL);
    CHECK(scrub_while_active_count == 0UL && !dma_active);
    CHECK(dma_host.dma_faulted);
    CHECK(!tx_residue_pending());
    CHECK(!dma_host.dma_data && !dma_host.dma_length && !dma_host.dma_writing);
    check_bounce_erased();

    dma_host.budget_us = 0UL;
    ok = p4host_reset(&dma_host);
    CHECK(ok && !dma_host.dma_faulted);
    CHECK(scrub_count == 1UL);
    CHECK(!dma_host.dma_data && !dma_host.dma_length && !dma_host.dma_writing);
}

static void test_stuck_full_reset_preserves_residue_until_retry(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;
    ULONG generation_before;
    BOOL ok;
    ++cases;
    reset_model();
    fill_secret(payload, 512UL, (UBYTE)0x7AU);
    request = make_write(payload, 512UL);
    inject_command_error = TRUE;
    fail_cleanup_reset = TRUE;
    timer_step = 30000UL;
    ok = p4host_command(&dma_host, &request, &result);
    CHECK(!ok && result.status == P4HOST_COMMAND_ERROR && result.started);
    CHECK(dma_host.dma_faulted && dma_active && tx_residue_pending());
    CHECK(bytes_have_nonzero(bounce_ram, 512UL));
    CHECK(bytes_have_nonzero(dma_host.bounce, 512UL));
    CHECK(dma_host.dma_data == payload && dma_host.dma_length == 512UL);
    CHECK(scrub_count == 0UL);

    stuck_bmod_swr = FALSE;
    stuck_ctrl_reset_all = TRUE;
    timer_step = 1UL;
    generation_before = dma_host.generation;
    ok = p4host_reset(&dma_host);
    CHECK(!ok && dma_host.dma_faulted);
    CHECK(dma_host.generation != generation_before);
    CHECK(dma_active && tx_residue_pending());
    CHECK(!all_reset_clear_seen || all_reset_clear_epoch != dma_epoch);
    CHECK(scrub_count == 0UL && scrub_while_active_count == 0UL);
    CHECK(bytes_have_nonzero(bounce_ram, 512UL));
    CHECK(bytes_have_nonzero(dma_host.bounce, 512UL));
    CHECK(dma_host.dma_data == payload && dma_host.dma_length == 512UL);

    stuck_ctrl_reset_all = FALSE;
    generation_before = dma_host.generation;
    ok = p4host_reset(&dma_host);
    CHECK(ok && !dma_host.dma_faulted && !dma_active);
    CHECK(dma_host.generation != generation_before);
    CHECK(scrub_count == 1UL && scrub_while_active_count == 0UL);
    CHECK(all_reset_clear_seen && all_reset_clear_epoch == dma_epoch);
    CHECK(!tx_residue_pending());
    CHECK(!dma_host.dma_data && !dma_host.dma_length && !dma_host.dma_writing);
    check_bounce_erased();
}

static void test_rx_final_error_does_not_copy_client_data(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;
    BOOL ok;
    ++cases;
    reset_model();
    memset(receive, 0x5A, 512UL);
    request = make_read(receive, 512UL);
    inject_rx_final_error = TRUE;
    ok = p4host_command(&dma_host, &request, &result);
    CHECK(!ok && result.status == P4HOST_COMMAND_ERROR && result.started);
    CHECK(receive[0] == (UBYTE)0x5AU && receive[511] == (UBYTE)0x5AU);
    CHECK(!inject_rx_final_error && !dma_host.dma_faulted);
    CHECK(!dma_active && !scrub_while_active_count);
}

int main(void)
{
    test_success_and_shorter_followup();
    test_tx_alias_is_refused_before_touching_dma();
    test_pre_enable_budget_expiry();
    test_command_error_resets_and_scrubs();
    test_failed_reset_preserves_then_full_reset_scrubs();
    test_stuck_full_reset_preserves_residue_until_retry();
    test_rx_final_error_does_not_copy_client_data();
    printf("PASS: %lu cases, %lu checks\n", (unsigned long)cases,
           (unsigned long)checks);
    return 0;
}
