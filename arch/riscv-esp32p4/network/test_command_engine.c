/* Host fixture for the production ESP32-P4 shared SDMMC command engine.
 * test-command-engine.py inserts the actual command.c function bodies.
 * Native host pointers are used here; this does not test the RV32 pointer ABI. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t ULONG;
typedef uintptr_t IPTR;
typedef uint8_t UBYTE;
typedef int16_t BOOL;

#define TRUE 1
#define FALSE 0
#define RADIO_IRQ_BITS ((1UL << 16) | (1UL << 17))
#define REGISTER_COUNT 256U
#define EVENT_CAPACITY 128U
#define RESPONSE_READ_CAPACITY 8U
#define OBSERVATION_CAPACITY 16U

struct Task
{
    unsigned id;
};

struct SignalSemaphore
{
    struct Task *ss_Owner;
};

struct P4SDMMCHost;

/* command.h and dma.h are the production headers.  Suppress only host.h's
 * platform-only resource layout and provide the fields used by extracted code. */
#define ESP32P4_SDMMC_HOST_H
#include "command.h"
#include "dma.h"
#include "registers.h"

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
    ULONG slot_clock[2];
    ULONG dma_length;
    BOOL dma_faulted;
    UBYTE *bounce;
};

enum event_kind
{
    EVENT_MMIO_WRITE = 1,
    EVENT_MMIO_BARRIER,
    EVENT_FIFO_RESET,
    EVENT_DMA_PREPARE,
    EVENT_DMA_READY,
    EVENT_DMA_COMPLETE,
    EVENT_DMA_RESET,
    EVENT_INVALIDATE,
    EVENT_OBSERVATION
};

struct event
{
    enum event_kind kind;
    ULONG offset;
    ULONG value;
};

struct observation_record
{
    enum P4HostCommandObservation observation;
    enum P4HostCommandStatus status;
    enum P4HostCommandFault injected_fault;
    BOOL started;
    ULONG raw, idstatus;
    unsigned event_index;
};

struct observation_log
{
    const struct P4HostCommand *request;
    struct observation_record records[OBSERVATION_CAPACITY];
    unsigned count;
};

static struct Task task_a = { 1U };
static struct Task task_b = { 2U };
static struct Task *active_task;
static struct P4SDMMCHost command_host;
static ULONG registers[REGISTER_COUNT];
static UBYTE bounce_buffer[P4HOST_DMA_MAX];
static UBYTE rx_payload[P4HOST_DMA_MAX];
static UBYTE client_data[P4HOST_DMA_MAX + 64UL];
static struct event events[EVENT_CAPACITY];
static ULONG response_reads[RESPONSE_READ_CAPACITY];
static unsigned event_count;
static unsigned response_read_count;
static unsigned read_count;
static unsigned write_count;
static unsigned rint_read_count;
static unsigned status_read_count;
static unsigned cmd_write_count;
static unsigned fifo_reset_count;
static unsigned dma_prepare_count;
static unsigned dma_ready_count;
static unsigned dma_complete_count;
static unsigned dma_reset_count;
static unsigned invalidate_count;
static unsigned test_cases;
static unsigned checks;
static ULONG timer_value;
static ULONG timer_step;
static ULONG now_call_count;
static ULONG next_raw;
static ULONG next_idstatus;
static ULONG late_raw_after_ready;
static ULONG late_idstatus_after_ready;
static ULONG late_raw_after_complete;
static ULONG late_idstatus_after_complete;
static ULONG raw_after_rint_read_count;
static ULONG raw_after_rint_read;
static ULONG last_command;
static ULONG dma_ready_after_calls;
static ULONG busy_reads_before_clear;
static unsigned post_start_status_reads;
static BOOL dma_ready_value;
static BOOL dma_prepare_value;
static BOOL dma_complete_value;
static BOOL dma_reset_value;
static BOOL fifo_reset_value;
static BOOL stuck_command_start;
static BOOL post_start_busy;
static BOOL busy_stuck;
static BOOL command_started;
static BOOL dma_prepare_saw_tx_copy;
static BOOL dma_writing;
static ULONG dma_transfer_length;
static UBYTE expected_tx[8];

static void mock_invalidate(struct P4SDMMCHost *host);
static BOOL mock_fifo_reset(struct P4SDMMCHost *host);
static BOOL mock_dma_reset(struct P4SDMMCHost *host);
static BOOL mock_dma_prepare(struct P4SDMMCHost *host, UBYTE *data,
                             ULONG length, BOOL writing);
static BOOL mock_dma_ready(struct P4SDMMCHost *host, ULONG length);
static BOOL mock_dma_complete(struct P4SDMMCHost *host, UBYTE *data,
                              ULONG length);

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

static void record_event(enum event_kind kind, ULONG offset, ULONG value)
{
    if (event_count < EVENT_CAPACITY)
    {
        events[event_count].kind = kind;
        events[event_count].offset = offset;
        events[event_count].value = value;
        ++event_count;
    }
}

static void observe_command(void *context,
                            const struct P4HostCommand *request,
                            enum P4HostCommandObservation observation,
                            const struct P4HostCommandResult *result)
{
    struct observation_log *log = (struct observation_log *)context;
    struct observation_record *record;
    CHECK(log != NULL);
    CHECK(request == log->request);
    CHECK(result != NULL);
    CHECK(log->count < OBSERVATION_CAPACITY);
    record = &log->records[log->count];
    record->observation = observation;
    record->status = result->status;
    record->injected_fault = result->injected_fault;
    record->started = result->started;
    record->raw = result->raw;
    record->idstatus = result->idstatus;
    record->event_index = event_count;
    ++log->count;
    record_event(EVENT_OBSERVATION, (ULONG)observation, result->raw);
}

static void attach_observer(struct P4HostCommand *request,
                            struct observation_log *log)
{
    memset(log, 0, sizeof(*log));
    log->request = request;
    request->observe = observe_command;
    request->observe_context = log;
}

static unsigned find_event(enum event_kind kind, unsigned start)
{
    unsigned index;
    for (index = start; index < event_count; ++index)
        if (events[index].kind == kind)
            return index;
    return EVENT_CAPACITY;
}

static unsigned find_observation_event(
    enum P4HostCommandObservation observation, unsigned start)
{
    unsigned index;
    for (index = start; index < event_count; ++index)
        if (events[index].kind == EVENT_OBSERVATION &&
            events[index].offset == (ULONG)observation)
            return index;
    return EVENT_CAPACITY;
}

static unsigned find_command_start_event(void)
{
    unsigned index;
    for (index = 0U; index < event_count; ++index)
        if (events[index].kind == EVENT_MMIO_WRITE &&
            events[index].offset == P4SD_CMD &&
            (events[index].value & P4SD_CMD_START))
            return index;
    return EVENT_CAPACITY;
}

static struct Task *FindTask(const char *name)
{
    (void)name;
    return active_task;
}

static ULONG mock_now_us(void)
{
    ULONG result = timer_value;
    ++now_call_count;
    timer_value += timer_step;
    return result;
}

static ULONG mock_mmio_read(ULONG offset)
{
    ULONG value;
    CHECK((offset / sizeof(ULONG)) < REGISTER_COUNT);
    ++read_count;
    if (offset == P4SD_RINTSTS)
    {
        ++rint_read_count;
        if (raw_after_rint_read_count &&
            rint_read_count >= raw_after_rint_read_count)
            registers[offset / sizeof(ULONG)] = raw_after_rint_read;
    }
    if (offset == P4SD_STATUS)
    {
        ++status_read_count;
        if (command_started && post_start_busy)
        {
            ++post_start_status_reads;
            if (busy_stuck || post_start_status_reads <= busy_reads_before_clear)
                return P4SD_STATUS_DATA_BUSY;
            return registers[offset / sizeof(ULONG)] &
                   ~P4SD_STATUS_DATA_BUSY;
        }
    }
    value = registers[offset / sizeof(ULONG)];
    if (offset >= P4SD_RESP0 && offset <= P4SD_RESP3 &&
        (offset - P4SD_RESP0) % sizeof(ULONG) == 0U &&
        response_read_count < RESPONSE_READ_CAPACITY)
    {
        response_reads[response_read_count] = offset;
        ++response_read_count;
    }
    return value;
}

static void mock_mmio_write(ULONG offset, ULONG value)
{
    ULONG *reg;
    CHECK((offset / sizeof(ULONG)) < REGISTER_COUNT);
    ++write_count;
    record_event(EVENT_MMIO_WRITE, offset, value);
    reg = &registers[offset / sizeof(ULONG)];
    if (offset == P4SD_RINTSTS)
    {
        *reg &= ~value;
    }
    else if (offset == P4SD_CMD && (value & P4SD_CMD_START))
    {
        ++cmd_write_count;
        last_command = value;
        command_started = TRUE;
        *reg = stuck_command_start ? value : value & ~P4SD_CMD_START;
        if (!stuck_command_start)
        {
            registers[P4SD_RINTSTS / sizeof(ULONG)] |= next_raw;
            registers[P4SD_IDSTS / sizeof(ULONG)] = next_idstatus;
            if (post_start_busy)
                registers[P4SD_STATUS / sizeof(ULONG)] |=
                    P4SD_STATUS_DATA_BUSY;
        }
    }
    else
    {
        *reg = value;
    }
}

static void mock_mmio_barrier(void)
{
    record_event(EVENT_MMIO_BARRIER, 0U, 0U);
}

/* INSERT_ACTUAL_COMMAND_ENGINE */

static void begin_case(void)
{
    ++test_cases;
}

static void setup_case(void)
{
    memset(&command_host, 0, sizeof(command_host));
    memset(registers, 0, sizeof(registers));
    memset(bounce_buffer, 0xA5, sizeof(bounce_buffer));
    memset(rx_payload, 0x5A, sizeof(rx_payload));
    memset(client_data, 0xCC, sizeof(client_data));
    memset(events, 0, sizeof(events));
    memset(response_reads, 0, sizeof(response_reads));
    event_count = 0U;
    response_read_count = 0U;
    read_count = 0U;
    write_count = 0U;
    rint_read_count = 0U;
    status_read_count = 0U;
    cmd_write_count = 0U;
    fifo_reset_count = 0U;
    dma_prepare_count = 0U;
    dma_ready_count = 0U;
    dma_complete_count = 0U;
    dma_reset_count = 0U;
    invalidate_count = 0U;
    timer_value = 0U;
    timer_step = 1U;
    now_call_count = 0U;
    next_raw = P4SD_INT_CMD_DONE;
    next_idstatus = P4SD_IDSTS_TI | P4SD_IDSTS_RI;
    late_raw_after_ready = 0U;
    late_idstatus_after_ready = 0U;
    late_raw_after_complete = 0U;
    late_idstatus_after_complete = 0U;
    raw_after_rint_read_count = 0U;
    raw_after_rint_read = 0U;
    last_command = 0U;
    dma_ready_after_calls = 0U;
    busy_reads_before_clear = 0U;
    post_start_status_reads = 0U;
    dma_ready_value = TRUE;
    dma_prepare_value = TRUE;
    dma_complete_value = TRUE;
    dma_reset_value = TRUE;
    fifo_reset_value = TRUE;
    dma_prepare_saw_tx_copy = FALSE;
    stuck_command_start = FALSE;
    post_start_busy = FALSE;
    busy_stuck = FALSE;
    command_started = FALSE;
    dma_writing = FALSE;
    dma_transfer_length = 0U;
    memset(expected_tx, 0, sizeof(expected_tx));
    active_task = &task_a;
    command_host.lock.ss_Owner = &task_a;
    command_host.now_us = mock_now_us;
    command_host.invalidate = mock_invalidate;
    command_host.fifo_reset = mock_fifo_reset;
    command_host.dma_reset = mock_dma_reset;
    command_host.dma_prepare = mock_dma_prepare;
    command_host.dma_ready = mock_dma_ready;
    command_host.dma_complete = mock_dma_complete;
    command_host.generation = 7U;
    command_host.slot_clock[0] = 400000UL;
    command_host.slot_clock[1] = 25000000UL;
    command_host.bounce = bounce_buffer;
    registers[P4SD_RINTSTS / sizeof(ULONG)] = RADIO_IRQ_BITS;
}

static BOOL mock_fifo_reset(struct P4SDMMCHost *host)
{
    CHECK(host == &command_host);
    ++fifo_reset_count;
    record_event(EVENT_FIFO_RESET, 0U, fifo_reset_value ? 1U : 0U);
    return fifo_reset_value;
}

static BOOL mock_dma_reset(struct P4SDMMCHost *host)
{
    CHECK(host == &command_host);
    ++dma_reset_count;
    record_event(EVENT_DMA_RESET, 0U, dma_reset_value ? 1U : 0U);
    return dma_reset_value;
}

static BOOL mock_dma_prepare(struct P4SDMMCHost *host, UBYTE *data,
                             ULONG length, BOOL writing)
{
    ULONG index;
    CHECK(host == &command_host);
    CHECK(data == bounce_buffer);
    ++dma_prepare_count;
    dma_transfer_length = length;
    dma_writing = writing;
    if (writing)
    {
        for (index = 0U; index < length && index < sizeof(expected_tx); ++index)
            if (data[index] != expected_tx[index])
                break;
        dma_prepare_saw_tx_copy = index == length;
    }
    record_event(EVENT_DMA_PREPARE, length, writing ? 1U : 0U);
    return dma_prepare_value;
}

static BOOL mock_dma_ready(struct P4SDMMCHost *host, ULONG length)
{
    CHECK(host == &command_host);
    CHECK(length == dma_transfer_length);
    ++dma_ready_count;
    record_event(EVENT_DMA_READY, length, dma_ready_count);
    if (late_raw_after_ready)
        registers[P4SD_RINTSTS / sizeof(ULONG)] |= late_raw_after_ready;
    if (late_idstatus_after_ready)
        registers[P4SD_IDSTS / sizeof(ULONG)] |= late_idstatus_after_ready;
    if (dma_ready_after_calls && dma_ready_count < dma_ready_after_calls)
        return FALSE;
    return dma_ready_value;
}

static BOOL mock_dma_complete(struct P4SDMMCHost *host, UBYTE *data,
                              ULONG length)
{
    CHECK(host == &command_host);
    CHECK(data == bounce_buffer);
    CHECK(length == dma_transfer_length);
    ++dma_complete_count;
    record_event(EVENT_DMA_COMPLETE, length, dma_writing ? 1U : 0U);
    if (!dma_writing)
        memcpy(data, rx_payload, length);
    if (late_raw_after_complete)
        registers[P4SD_RINTSTS / sizeof(ULONG)] |= late_raw_after_complete;
    if (late_idstatus_after_complete)
        registers[P4SD_IDSTS / sizeof(ULONG)] |= late_idstatus_after_complete;
    return dma_complete_value;
}

static void mock_invalidate(struct P4SDMMCHost *host)
{
    CHECK(host == &command_host);
    ++invalidate_count;
    record_event(EVENT_INVALIDATE, 0U, 0U);
}

static struct P4HostCommand make_command(unsigned slot, ULONG index,
                                         ULONG response_flags)
{
    struct P4HostCommand request;
    memset(&request, 0, sizeof(request));
    request.slot = slot;
    request.generation = command_host.generation;
    request.index = index;
    request.argument = 0x12345678UL;
    request.response_flags = response_flags;
    return request;
}

static void set_result_sentinel(struct P4HostCommandResult *result)
{
    memset(result, 0xA7, sizeof(*result));
}

static void check_response_zero(const struct P4HostCommandResult *result)
{
    CHECK(result->response[0] == 0U);
    CHECK(result->response[1] == 0U);
    CHECK(result->response[2] == 0U);
    CHECK(result->response[3] == 0U);
}

static void expect_refused(const struct P4HostCommand *request)
{
    struct P4HostCommandResult result;
    set_result_sentinel(&result);
    CHECK(!p4host_command(&command_host, request, &result));
    CHECK(result.status == P4HOST_COMMAND_REFUSED);
    CHECK(!result.started);
    check_response_zero(&result);
    CHECK(read_count == 0U);
    CHECK(write_count == 0U);
    CHECK(fifo_reset_count == 0U);
    CHECK(dma_prepare_count == 0U);
    CHECK(dma_ready_count == 0U);
    CHECK(dma_complete_count == 0U);
}

static void test_actual_bounds_and_native_model(void)
{
    begin_case();
    CHECK(P4HOST_COMMAND_TIMEOUT_US == 250000UL);
    CHECK(P4HOST_DATA_TIMEOUT_US == 5000000UL);
    CHECK(P4HOST_BUSY_TIMEOUT_US == 1000000UL);
    CHECK(P4HOST_COMMAND_POLLS == 1000000UL);
    CHECK(sizeof(ULONG) == 4U);
    CHECK(sizeof(BOOL) == 2U);
    CHECK(sizeof(IPTR) == sizeof(void *));
}

static void test_actual_rintsts_write_masks_radio_bits(void)
{
    begin_case();
    setup_case();
    registers[P4SD_RINTSTS / sizeof(ULONG)] |= P4SD_INT_CMD_DONE;
    command_write(P4SD_RINTSTS,
                  P4SD_INT_CMD_DATA_MASK | RADIO_IRQ_BITS);
    CHECK(write_count == 1U);
    CHECK(events[0].kind == EVENT_MMIO_WRITE);
    CHECK(events[0].value == P4SD_INT_CMD_DATA_MASK);
    CHECK((registers[P4SD_RINTSTS / sizeof(ULONG)] &
           P4SD_INT_CMD_DONE) == 0U);
    CHECK((registers[P4SD_RINTSTS / sizeof(ULONG)] & RADIO_IRQ_BITS) ==
          RADIO_IRQ_BITS);
}

static void test_invalid_requests_are_non_mmio(void)
{
    struct P4HostCommand request;

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_R1);
    request.slot = 2U;
    expect_refused(&request);

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_R1);
    request.generation = 0U;
    expect_refused(&request);

    setup_case();
    request = make_command(1U, 1U, MMC_RSP_R1);
    request.generation++;
    expect_refused(&request);

    setup_case();
    command_host.slot_clock[1] = 0U;
    request = make_command(1U, 1U, MMC_RSP_R1);
    expect_refused(&request);

    setup_case();
    active_task = &task_b;
    request = make_command(0U, 1U, MMC_RSP_R1);
    expect_refused(&request);

    setup_case();
    command_host.dma_faulted = TRUE;
    request = make_command(0U, 1U, MMC_RSP_R1);
    expect_refused(&request);

    setup_case();
    command_host.dma_length = 4U;
    request = make_command(0U, 1U, MMC_RSP_R1);
    expect_refused(&request);

    setup_case();
    request = make_command(0U, 64U, MMC_RSP_NONE);
    expect_refused(&request);

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_R1);
    request.response_flags |= (1UL << 8);
    expect_refused(&request);

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_CRC);
    expect_refused(&request);

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_R2 | MMC_RSP_OPCODE);
    expect_refused(&request);

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_R2 | MMC_RSP_BUSY);
    expect_refused(&request);

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_NONE);
    request.response_error_mask = 1U;
    expect_refused(&request);

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_R2);
    request.response_error_mask = 1U;
    expect_refused(&request);

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_NONE);
    request.writing = 2;
    expect_refused(&request);

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_NONE);
    request.early_cmd18 = 2;
    expect_refused(&request);

    setup_case();
    command_host.now_us = NULL;
    request = make_command(0U, 1U, MMC_RSP_NONE);
    expect_refused(&request);

    setup_case();
    command_host.invalidate = NULL;
    request = make_command(0U, 1U, MMC_RSP_NONE);
    expect_refused(&request);

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_NONE);
    request.length = 1U;
    request.block_size = 1U;
    request.data = client_data;
    command_host.dma_prepare = NULL;
    expect_refused(&request);

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_NONE);
    request.length = 4U;
    request.block_size = 1U;
    request.data = client_data;
    command_host.fifo_reset = NULL;
    expect_refused(&request);

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_NONE);
    request.length = 4U;
    request.block_size = 1U;
    request.data = client_data;
    command_host.dma_reset = NULL;
    expect_refused(&request);

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_NONE);
    request.length = 4U;
    request.block_size = 1U;
    request.data = client_data;
    command_host.dma_ready = NULL;
    expect_refused(&request);

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_NONE);
    request.length = 4U;
    request.block_size = 1U;
    request.data = client_data;
    command_host.dma_complete = NULL;
    expect_refused(&request);

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_NONE);
    request.length = 1U;
    request.block_size = 1U;
    request.data = client_data;
    command_host.bounce = NULL;
    expect_refused(&request);

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_NONE);
    setup_case();
    request = make_command(0U, 53U, MMC_RSP_NONE);
    request.length = 5U;
    request.block_size = 1U;
    request.data = client_data;
    expect_refused(&request);

    setup_case();
    request = make_command(1U, 53U, MMC_RSP_NONE);
    request.length = 7U;
    request.block_size = 1U;
    request.data = client_data;
    expect_refused(&request);

    setup_case();
    request = make_command(1U, 53U, MMC_RSP_NONE);
    request.length = 513U;
    request.block_size = 1U;
    request.data = client_data;
    expect_refused(&request);

    setup_case();
    request = make_command(1U, 53U, MMC_RSP_NONE);
    request.length = P4HOST_DMA_MAX + 4UL;
    request.block_size = 1U;
    request.data = client_data;
    expect_refused(&request);

    setup_case();
    request = make_command(1U, 53U, MMC_RSP_NONE);
    request.length = 5U;
    request.block_size = 1U;
    request.data = client_data;
    request.early_cmd18 = TRUE;
    expect_refused(&request);

    setup_case();
    request = make_command(1U, 18U, MMC_RSP_NONE);
    request.length = 1024U;
    request.block_size = 512U;
    request.data = client_data;
    request.early_cmd18 = TRUE;
    expect_refused(&request);
}

static void test_null_arguments_are_non_mmio(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;

    begin_case();
    setup_case();
    request = make_command(0U, 1U, MMC_RSP_R1);
    set_result_sentinel(&result);
    CHECK(!p4host_command(NULL, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_REFUSED);
    CHECK(read_count == 0U && write_count == 0U);

    setup_case();
    set_result_sentinel(&result);
    CHECK(!p4host_command(&command_host, NULL, &result));
    CHECK(result.status == P4HOST_COMMAND_REFUSED);
    CHECK(read_count == 0U && write_count == 0U);

    setup_case();
    request = make_command(0U, 1U, MMC_RSP_R1);
    CHECK(!p4host_command(&command_host, &request, NULL));
    CHECK(read_count == 0U && write_count == 0U);
}

static void test_slot_and_generation_dispatch(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;

    begin_case();
    setup_case();
    request = make_command(1U, 52U, MMC_RSP_R5);
    request.argument = 0xAABBCCDDUL;
    set_result_sentinel(&result);
    CHECK(p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_OK && result.started);
    CHECK((last_command & 63UL) == 52UL);
    CHECK((last_command & (1UL << P4SD_CMD_SLOT_S)) != 0U);
    CHECK((last_command & P4SD_CMD_RESP) != 0U);
    CHECK((last_command & P4SD_CMD_RESP_CRC) != 0U);
    CHECK((last_command & P4SD_CMD_WAIT_DATA) != 0U);
    CHECK(registers[P4SD_CMDARG / sizeof(ULONG)] == 0xAABBCCDDUL);
    CHECK((registers[P4SD_RINTSTS / sizeof(ULONG)] & RADIO_IRQ_BITS) ==
          RADIO_IRQ_BITS);
    CHECK(result.response[0] == 0U);
}

static void test_cmd0_and_cmd12_encoding(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;

    begin_case();
    setup_case();
    request = make_command(0U, 0U, MMC_RSP_NONE);
    CHECK(p4host_command(&command_host, &request, &result));
    CHECK((last_command & 63UL) == 0U);
    CHECK((last_command & P4SD_CMD_SEND_INIT) != 0U);
    CHECK((last_command & P4SD_CMD_WAIT_DATA) == 0U);
    CHECK((last_command & P4SD_CMD_STOP_ABORT) == 0U);

    setup_case();
    registers[P4SD_STATUS / sizeof(ULONG)] = P4SD_STATUS_DATA_BUSY;
    request = make_command(0U, 12U, MMC_RSP_R1);
    CHECK(p4host_command(&command_host, &request, &result));
    CHECK((last_command & 63UL) == 12U);
    CHECK((last_command & P4SD_CMD_STOP_ABORT) != 0U);
    CHECK((last_command & P4SD_CMD_WAIT_DATA) == 0U);
    CHECK((last_command & P4SD_CMD_RESP) != 0U);
    CHECK((last_command & P4SD_CMD_RESP_CRC) != 0U);
    CHECK((last_command & (1UL << P4SD_CMD_SLOT_S)) == 0U);
}

static void test_long_response_is_msw_first_and_success_only(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;

    begin_case();
    setup_case();
    registers[P4SD_RESP0 / sizeof(ULONG)] = 0x11111111UL;
    registers[P4SD_RESP1 / sizeof(ULONG)] = 0x22222222UL;
    registers[P4SD_RESP2 / sizeof(ULONG)] = 0x33333333UL;
    registers[P4SD_RESP3 / sizeof(ULONG)] = 0x44444444UL;
    request = make_command(0U, 2U, MMC_RSP_R2);
    CHECK(p4host_command(&command_host, &request, &result));
    CHECK(result.response[0] == 0x44444444UL);
    CHECK(result.response[1] == 0x33333333UL);
    CHECK(result.response[2] == 0x22222222UL);
    CHECK(result.response[3] == 0x11111111UL);
    CHECK(response_read_count == 4U);
    CHECK(response_reads[0] == P4SD_RESP0);
    CHECK(response_reads[1] == P4SD_RESP1);
    CHECK(response_reads[2] == P4SD_RESP2);
    CHECK(response_reads[3] == P4SD_RESP3);
}

static void test_r5_mask_error_does_not_publish_response(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;

    begin_case();
    setup_case();
    registers[P4SD_RESP0 / sizeof(ULONG)] = 0x00004000UL;
    request = make_command(1U, 52U, MMC_RSP_R5);
    request.response_error_mask = 0x00004000UL;
    set_result_sentinel(&result);
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_CARD_ERROR);
    CHECK(result.started);
    CHECK((result.raw & P4SD_INT_CMD_DONE) != 0U);
    check_response_zero(&result);
}

static void configure_data_request(struct P4HostCommand *request,
                                   unsigned slot, ULONG index, BOOL writing,
                                   ULONG length, ULONG block_size)
{
    *request = make_command(slot, index, MMC_RSP_R5);
    request->data = client_data + 4;
    request->length = length;
    request->block_size = block_size;
    request->writing = writing;
}

static void configure_diagnostic_request(struct P4HostCommand *request,
                                         struct observation_log *log,
                                         enum P4HostCommandFault fault)
{
    configure_data_request(request, 0U, 18U, FALSE, 1024U, 512U);
    request->diagnostic_fault = fault;
    attach_observer(request, log);
}

static void expect_diagnostic_refused(struct P4HostCommand *request,
                                      struct observation_log *log)
{
    attach_observer(request, log);
    expect_refused(request);
    CHECK(log->count == 0U);
}

static void test_diagnostic_validation_is_non_mmio(void)
{
    struct P4HostCommand request;
    struct observation_log log;
    int context_token;

    begin_case();
    setup_case();
    configure_data_request(&request, 0U, 18U, FALSE, 1024U, 512U);
    request.diagnostic_fault = (enum P4HostCommandFault)99;
    expect_diagnostic_refused(&request, &log);

    setup_case();
    configure_data_request(&request, 1U, 18U, FALSE, 1024U, 512U);
    request.diagnostic_fault = P4HOST_FAULT_CMD_CRC;
    expect_diagnostic_refused(&request, &log);

    setup_case();
    configure_data_request(&request, 0U, 17U, FALSE, 1024U, 512U);
    request.diagnostic_fault = P4HOST_FAULT_DATA_CRC;
    expect_diagnostic_refused(&request, &log);

    setup_case();
    configure_data_request(&request, 0U, 18U, TRUE, 1024U, 512U);
    request.diagnostic_fault = P4HOST_FAULT_TIMEOUT;
    expect_diagnostic_refused(&request, &log);

    setup_case();
    configure_data_request(&request, 0U, 18U, FALSE, 512U, 512U);
    request.diagnostic_fault = P4HOST_FAULT_CMD_CRC;
    expect_diagnostic_refused(&request, &log);

    setup_case();
    configure_data_request(&request, 0U, 18U, FALSE, 1280U, 512U);
    request.diagnostic_fault = P4HOST_FAULT_DATA_CRC;
    expect_diagnostic_refused(&request, &log);

    setup_case();
    configure_data_request(&request, 0U, 18U, FALSE, 1024U, 256U);
    request.diagnostic_fault = P4HOST_FAULT_TIMEOUT;
    expect_diagnostic_refused(&request, &log);

    setup_case();
    request = make_command(0U, 18U, MMC_RSP_R5);
    request.observe_context = &context_token;
    expect_refused(&request);
}

static void test_diagnostic_observer_and_no_observer_regression(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult baseline, result;
    struct observation_log log;
    UBYTE baseline_payload[1024];
    unsigned baseline_reads, baseline_writes, baseline_fifo_resets;
    unsigned baseline_dma_prepares, baseline_dma_ready, baseline_dma_complete;
    unsigned baseline_dma_resets, command_start_event, last_rint_write, index;

    begin_case();
    setup_case();
    configure_data_request(&request, 0U, 18U, FALSE, 1024U, 512U);
    next_raw = P4SD_INT_CMD_DONE | P4SD_INT_DATA_OVER;
    CHECK(p4host_command(&command_host, &request, &baseline));
    CHECK(baseline.status == P4HOST_COMMAND_OK);
    CHECK(baseline.injected_fault == P4HOST_FAULT_NONE);
    CHECK(find_event(EVENT_OBSERVATION, 0U) == EVENT_CAPACITY);
    memcpy(baseline_payload, client_data + 4, sizeof(baseline_payload));
    baseline_reads = read_count;
    baseline_writes = write_count;
    baseline_fifo_resets = fifo_reset_count;
    baseline_dma_prepares = dma_prepare_count;
    baseline_dma_ready = dma_ready_count;
    baseline_dma_complete = dma_complete_count;
    baseline_dma_resets = dma_reset_count;

    setup_case();
    configure_diagnostic_request(&request, &log, P4HOST_FAULT_NONE);
    next_raw = P4SD_INT_CMD_DONE | P4SD_INT_DATA_OVER;
    CHECK(p4host_command(&command_host, &request, &result));
    CHECK(result.status == baseline.status);
    CHECK(result.started == baseline.started);
    CHECK(result.raw == baseline.raw);
    CHECK(result.idstatus == baseline.idstatus);
    CHECK(result.injected_fault == baseline.injected_fault);
    CHECK(memcmp(result.response, baseline.response,
                 sizeof(result.response)) == 0);
    CHECK(read_count == baseline_reads);
    CHECK(write_count == baseline_writes);
    CHECK(fifo_reset_count == baseline_fifo_resets);
    CHECK(dma_prepare_count == baseline_dma_prepares);
    CHECK(dma_ready_count == baseline_dma_ready);
    CHECK(dma_complete_count == baseline_dma_complete);
    CHECK(dma_reset_count == baseline_dma_resets);
    CHECK(memcmp(client_data + 4, baseline_payload,
                 sizeof(baseline_payload)) == 0);
    CHECK(log.count == 3U);
    CHECK(log.records[0].observation == P4HOST_OBSERVE_START);
    CHECK(log.records[0].started);
    CHECK(log.records[1].observation == P4HOST_OBSERVE_SAMPLE);
    CHECK(log.records[1].raw == (RADIO_IRQ_BITS | next_raw));
    CHECK(log.records[1].idstatus == next_idstatus);
    CHECK(log.records[1].injected_fault == P4HOST_FAULT_NONE);
    CHECK(log.records[2].observation == P4HOST_OBSERVE_FINISH);
    CHECK(log.records[2].status == P4HOST_COMMAND_OK);
    CHECK(log.records[2].injected_fault == P4HOST_FAULT_NONE);
    command_start_event = find_command_start_event();
    CHECK(command_start_event < event_count);
    CHECK(log.records[0].event_index > command_start_event);
    CHECK(log.records[1].event_index > log.records[0].event_index);
    CHECK(log.records[2].event_index > log.records[1].event_index);
    CHECK(find_observation_event(P4HOST_OBSERVE_FINISH, 0U) ==
          log.records[2].event_index);
    last_rint_write = EVENT_CAPACITY;
    for (index = 0U; index < event_count; ++index)
        if (events[index].kind == EVENT_MMIO_WRITE &&
            events[index].offset == P4SD_RINTSTS)
            last_rint_write = index;
    CHECK(last_rint_write < event_count);
    CHECK(log.records[2].event_index > last_rint_write);
}

static void test_diagnostic_fault_modes_reset_without_publication(void)
{
    static const enum P4HostCommandFault faults[] = {
        P4HOST_FAULT_CMD_CRC,
        P4HOST_FAULT_DATA_CRC,
        P4HOST_FAULT_TIMEOUT
    };
    static const enum P4HostCommandStatus statuses[] = {
        P4HOST_COMMAND_ERROR,
        P4HOST_COMMAND_ERROR,
        P4HOST_COMMAND_TIMEOUT
    };
    static const ULONG error_bits[] = {
        P4SD_INT_RCRC,
        P4SD_INT_DCRC,
        0U
    };
    struct P4HostCommand request;
    struct P4HostCommandResult result;
    struct observation_log log;
    unsigned fault_index;

    begin_case();
    for (fault_index = 0U;
         fault_index < sizeof(faults) / sizeof(faults[0]); ++fault_index)
    {
        unsigned command_start_event, first_fifo_reset, cleanup_fifo_reset;
        setup_case();
        configure_diagnostic_request(&request, &log, faults[fault_index]);
        next_raw = P4SD_INT_CMD_DONE | P4SD_INT_DATA_OVER;
        set_result_sentinel(&result);
        CHECK(!p4host_command(&command_host, &request, &result));
        CHECK(result.status == statuses[fault_index]);
        CHECK(result.started);
        CHECK(result.injected_fault == faults[fault_index]);
        CHECK(result.raw == (RADIO_IRQ_BITS | next_raw |
                             error_bits[fault_index]));
        CHECK(result.idstatus == next_idstatus);
        CHECK(log.count == 3U);
        CHECK(log.records[0].observation == P4HOST_OBSERVE_START);
        CHECK(log.records[1].observation == P4HOST_OBSERVE_SAMPLE);
        CHECK(log.records[1].raw == (RADIO_IRQ_BITS | next_raw));
        CHECK(log.records[1].idstatus == next_idstatus);
        CHECK(log.records[1].injected_fault == P4HOST_FAULT_NONE);
        CHECK(log.records[2].observation == P4HOST_OBSERVE_FINISH);
        CHECK(log.records[2].status == statuses[fault_index]);
        CHECK(log.records[2].raw == result.raw);
        CHECK(log.records[2].injected_fault == faults[fault_index]);
        command_start_event = find_command_start_event();
        CHECK(command_start_event < event_count);
        CHECK(log.records[0].event_index > command_start_event);
        CHECK(log.records[1].event_index > log.records[0].event_index);
        CHECK(log.records[2].event_index > log.records[1].event_index);
        CHECK(dma_reset_count == 1U);
        CHECK(fifo_reset_count == 2U);
        CHECK(dma_complete_count == 0U);
        CHECK(!command_host.dma_faulted);
        CHECK(invalidate_count == 0U);
        CHECK(client_data[4] == 0xCCU);
        CHECK(client_data[4U + 1023U] == 0xCCU);
        check_response_zero(&result);
        first_fifo_reset = find_event(EVENT_FIFO_RESET, 0U);
        cleanup_fifo_reset = find_event(EVENT_FIFO_RESET,
                                        first_fifo_reset + 1U);
        CHECK(first_fifo_reset < cleanup_fifo_reset);
        CHECK(cleanup_fifo_reset < event_count);
        CHECK(log.records[2].event_index > cleanup_fifo_reset);
    }
}

static void test_data_crc_injection_beats_same_sample_early_cmd18(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;
    struct observation_log log;

    begin_case();
    setup_case();
    configure_diagnostic_request(&request, &log, P4HOST_FAULT_DATA_CRC);
    request.early_cmd18 = TRUE;
    next_raw = P4SD_INT_CMD_DONE | P4SD_INT_DATA_OVER;
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_ERROR);
    CHECK(result.started);
    CHECK(result.injected_fault == P4HOST_FAULT_DATA_CRC);
    CHECK(result.raw == (RADIO_IRQ_BITS | P4SD_INT_CMD_DONE |
                         P4SD_INT_DATA_OVER | P4SD_INT_DCRC));
    CHECK(log.count == 3U);
    CHECK(log.records[1].observation == P4HOST_OBSERVE_SAMPLE);
    CHECK(log.records[1].raw == (RADIO_IRQ_BITS | P4SD_INT_CMD_DONE |
                                 P4SD_INT_DATA_OVER));
    CHECK(log.records[1].idstatus == next_idstatus);
    CHECK(log.records[1].injected_fault == P4HOST_FAULT_NONE);
    CHECK(log.records[2].observation == P4HOST_OBSERVE_FINISH);
    CHECK(dma_ready_count == 0U);
    CHECK(dma_complete_count == 0U);
    CHECK(dma_reset_count == 1U && fifo_reset_count == 2U);
    CHECK(client_data[4] == 0xCCU && client_data[4U + 1023U] == 0xCCU);
    check_response_zero(&result);
}

static void test_data_crc_injection_waits_for_delayed_cmd_done(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;
    struct observation_log log;

    begin_case();
    setup_case();
    configure_diagnostic_request(&request, &log, P4HOST_FAULT_DATA_CRC);
    next_raw = 0U;
    raw_after_rint_read_count = 2U;
    raw_after_rint_read = RADIO_IRQ_BITS | P4SD_INT_CMD_DONE |
                          P4SD_INT_DATA_OVER;
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_ERROR);
    CHECK(result.injected_fault == P4HOST_FAULT_DATA_CRC);
    CHECK(result.raw == (RADIO_IRQ_BITS | P4SD_INT_CMD_DONE |
                         P4SD_INT_DATA_OVER | P4SD_INT_DCRC));
    CHECK(log.count == 4U);
    CHECK(log.records[0].observation == P4HOST_OBSERVE_START);
    CHECK(log.records[1].observation == P4HOST_OBSERVE_SAMPLE);
    CHECK(log.records[1].raw == RADIO_IRQ_BITS);
    CHECK(log.records[1].injected_fault == P4HOST_FAULT_NONE);
    CHECK(log.records[2].observation == P4HOST_OBSERVE_SAMPLE);
    CHECK(log.records[2].raw == (RADIO_IRQ_BITS | P4SD_INT_CMD_DONE |
                                 P4SD_INT_DATA_OVER));
    CHECK(log.records[2].injected_fault == P4HOST_FAULT_NONE);
    CHECK(log.records[3].observation == P4HOST_OBSERVE_FINISH);
    CHECK(log.records[3].raw == result.raw);
    CHECK(dma_ready_count == 0U);
    CHECK(dma_complete_count == 0U);
    CHECK(dma_reset_count == 1U && fifo_reset_count == 2U);
    CHECK(client_data[4] == 0xCCU && client_data[4U + 1023U] == 0xCCU);
    check_response_zero(&result);
}

static void test_diagnostic_callbacks_are_absent_before_start(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;
    struct observation_log log;

    begin_case();
    setup_case();
    configure_diagnostic_request(&request, &log, P4HOST_FAULT_CMD_CRC);
    registers[P4SD_CMD / sizeof(ULONG)] = P4SD_CMD_START;
    stuck_command_start = TRUE;
    timer_step = 100000UL;
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_TIMEOUT);
    CHECK(!result.started);
    CHECK(result.injected_fault == P4HOST_FAULT_NONE);
    CHECK(log.count == 0U);
    CHECK(cmd_write_count == 0U);
    CHECK(fifo_reset_count == 0U);
    CHECK(dma_prepare_count == 0U);
    CHECK(invalidate_count == 1U);
}

static void test_slot1_cmd53_subword_tails_and_neighbours(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;
    ULONG length;
    unsigned index;

    for (length = 1U; length <= 3U; ++length)
    {
        begin_case();
        setup_case();
        configure_data_request(&request, 1U, 53U, FALSE, length, 1U);
        next_raw = P4SD_INT_CMD_DONE | P4SD_INT_DATA_OVER;
        for (index = 0U; index < length; ++index)
            rx_payload[index] = (UBYTE)(0x30U + index);
        CHECK(p4host_command(&command_host, &request, &result));
        CHECK(result.status == P4HOST_COMMAND_OK);
        CHECK(dma_transfer_length == length);
        CHECK(dma_complete_count == 1U);
        CHECK((last_command & (1UL << P4SD_CMD_SLOT_S)) != 0U);
        CHECK((last_command & P4SD_CMD_DATA) != 0U);
        CHECK((last_command & P4SD_CMD_WRITE) == 0U);
        CHECK(registers[P4SD_BYTCNT / sizeof(ULONG)] == length);
        CHECK(client_data[3] == 0xCCU);
        for (index = 0U; index < length; ++index)
            CHECK(client_data[4U + index] == (UBYTE)(0x30U + index));
        CHECK(client_data[4U + length] == 0xCCU);
    }
}

static void test_slot1_cmd53_requires_data_over_and_all_descriptors(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;

    begin_case();
    setup_case();
    configure_data_request(&request, 1U, 53U, FALSE, 4U, 1U);
    next_raw = P4SD_INT_CMD_DONE;
    timer_step = 1000000UL;
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_TIMEOUT);
    CHECK(rint_read_count == 5U);
    CHECK(dma_ready_count == 0U);
    CHECK(dma_complete_count == 0U);
    CHECK(client_data[4] == 0xCCU && client_data[7] == 0xCCU);
    check_response_zero(&result);

    setup_case();
    configure_data_request(&request, 1U, 53U, FALSE, 4U, 1U);
    next_raw = P4SD_INT_CMD_DONE | P4SD_INT_DATA_OVER;
    dma_ready_value = FALSE;
    timer_step = 1000000UL;
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_TIMEOUT);
    CHECK(rint_read_count == 5U);
    CHECK(dma_ready_count > 0U);
    CHECK(dma_ready_count == 5U);
    CHECK(dma_complete_count == 0U);
    CHECK(client_data[4] == 0xCCU && client_data[7] == 0xCCU);
    check_response_zero(&result);
}

static void test_tx_data_is_staged_before_dma_arm(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;
    unsigned index;
    unsigned prepare_event = EVENT_CAPACITY;
    unsigned command_event = EVENT_CAPACITY;

    begin_case();
    setup_case();
    configure_data_request(&request, 1U, 53U, TRUE, 4U, 1U);
    for (index = 0U; index < 4U; ++index)
    {
        expected_tx[index] = (UBYTE)(0x70U + index);
        request.data[index] = expected_tx[index];
    }
    next_raw = P4SD_INT_CMD_DONE | P4SD_INT_DATA_OVER;
    CHECK(p4host_command(&command_host, &request, &result));
    CHECK(dma_prepare_saw_tx_copy);
    CHECK(dma_writing);
    CHECK(dma_transfer_length == 4U);
    CHECK(registers[P4SD_BYTCNT / sizeof(ULONG)] == 4U);
    CHECK((last_command & P4SD_CMD_WRITE) != 0U);
    CHECK((last_command & P4SD_CMD_DATA) != 0U);
    CHECK(dma_complete_count == 1U);
    for (index = 0U; index < 4U; ++index)
        CHECK(bounce_buffer[index] == (UBYTE)(0x70U + index));
    for (index = 0U; index < event_count; ++index)
    {
        if (events[index].kind == EVENT_DMA_PREPARE)
            prepare_event = index;
        if (events[index].kind == EVENT_MMIO_WRITE &&
            events[index].offset == P4SD_CMD)
            command_event = index;
    }
    CHECK(prepare_event < command_event);
}

static void test_simultaneous_data_error_precedes_done_and_ready(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;

    begin_case();
    setup_case();
    configure_data_request(&request, 1U, 53U, FALSE, 4U, 1U);
    next_raw = P4SD_INT_CMD_DONE | P4SD_INT_DATA_OVER | P4SD_INT_DTO;
    dma_ready_value = TRUE;
    memset(client_data + 4, 0xCC, 4U);
    set_result_sentinel(&result);
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_ERROR);
    CHECK((result.raw & (P4SD_INT_CMD_DONE | P4SD_INT_DATA_OVER |
                         P4SD_INT_DTO)) ==
          (P4SD_INT_CMD_DONE | P4SD_INT_DATA_OVER | P4SD_INT_DTO));
    CHECK(dma_ready_count == 0U);
    CHECK(dma_complete_count == 0U);
    CHECK(dma_reset_count == 1U);
    CHECK(client_data[4] == 0xCCU && client_data[7] == 0xCCU);
    check_response_zero(&result);
}

static void test_late_raw_and_idmac_errors_prevent_copy(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;

    begin_case();
    setup_case();
    configure_data_request(&request, 0U, 18U, FALSE, 512U, 512U);
    next_raw = P4SD_INT_CMD_DONE | P4SD_INT_DATA_OVER;
    late_raw_after_ready = P4SD_INT_DCRC;
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_ERROR);
    CHECK((result.raw & P4SD_INT_DCRC) != 0U);
    CHECK(dma_complete_count == 0U);
    CHECK(client_data[4] == 0xCCU && client_data[4U + 512U] == 0xCCU);
    check_response_zero(&result);

    setup_case();
    configure_data_request(&request, 0U, 18U, FALSE, 512U, 512U);
    next_raw = P4SD_INT_CMD_DONE | P4SD_INT_DATA_OVER;
    late_idstatus_after_ready = P4SD_IDSTS_FBE;
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_DMA_ERROR);
    CHECK((result.idstatus & P4SD_IDSTS_FBE) != 0U);
    CHECK(dma_complete_count == 0U);
    CHECK(client_data[4] == 0xCCU && client_data[4U + 512U] == 0xCCU);
    check_response_zero(&result);

    setup_case();
    configure_data_request(&request, 0U, 18U, FALSE, 512U, 512U);
    next_raw = P4SD_INT_CMD_DONE | P4SD_INT_DATA_OVER;
    late_raw_after_complete = P4SD_INT_DCRC;
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_ERROR);
    CHECK((result.raw & P4SD_INT_DCRC) != 0U);
    CHECK(dma_complete_count == 1U);
    CHECK(client_data[4] == 0xCCU && client_data[4U + 512U] == 0xCCU);
    check_response_zero(&result);

    setup_case();
    configure_data_request(&request, 0U, 18U, FALSE, 512U, 512U);
    next_raw = P4SD_INT_CMD_DONE | P4SD_INT_DATA_OVER;
    late_idstatus_after_complete = P4SD_IDSTS_FBE;
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_DMA_ERROR);
    CHECK((result.idstatus & P4SD_IDSTS_FBE) != 0U);
    CHECK(dma_complete_count == 1U);
    CHECK(client_data[4] == 0xCCU && client_data[4U + 512U] == 0xCCU);
    check_response_zero(&result);
}

static void test_early_cmd18_is_storage_slot0_only(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;

    begin_case();
    setup_case();
    request = make_command(0U, 18U, MMC_RSP_R1);
    request.data = client_data + 4;
    request.length = 1024U;
    request.block_size = 512U;
    request.early_cmd18 = TRUE;
    next_raw = P4SD_INT_CMD_DONE;
    CHECK(p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_OK);
    CHECK(dma_ready_count == 1U);
    CHECK(dma_complete_count == 1U);
    CHECK(client_data[4] == 0x5AU && client_data[4U + 1024U] == 0xCCU);

    setup_case();
    configure_data_request(&request, 0U, 17U, FALSE, 512U, 512U);
    next_raw = P4SD_INT_CMD_DONE;
    timer_step = 1000000UL;
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_TIMEOUT);
    CHECK(dma_ready_count == 0U && dma_complete_count == 0U);
    CHECK(client_data[4] == 0xCCU);

    setup_case();
    configure_data_request(&request, 1U, 53U, FALSE, 4U, 1U);
    next_raw = P4SD_INT_CMD_DONE;
    timer_step = 1000000UL;
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_TIMEOUT);
    CHECK(dma_ready_count == 0U && dma_complete_count == 0U);
    CHECK(client_data[4] == 0xCCU);
}

static void test_stalled_timer_is_bounded_by_poll_count(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;

    begin_case();
    setup_case();
    request = make_command(0U, 13U, MMC_RSP_NONE);
    next_raw = 0U;
    timer_step = 0U;
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_TIMEOUT);
    CHECK(rint_read_count == P4HOST_COMMAND_POLLS);
    CHECK(now_call_count >= P4HOST_COMMAND_POLLS);
    CHECK(!command_host.dma_faulted);
    check_response_zero(&result);
}

static void test_timeout_timer_wraps_and_command_wait_is_capped(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;

    begin_case();
    setup_case();
    request = make_command(0U, 13U, MMC_RSP_NONE);
    next_raw = 0U;
    timer_value = 0xFFFFFF00UL;
    timer_step = 100000UL;
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_TIMEOUT);
    CHECK(rint_read_count < P4HOST_COMMAND_POLLS);
    CHECK(rint_read_count == 2U);
    CHECK(!command_host.dma_faulted);
    check_response_zero(&result);

    setup_case();
    request = make_command(0U, 13U, MMC_RSP_NONE);
    registers[P4SD_CMD / sizeof(ULONG)] = P4SD_CMD_START;
    stuck_command_start = TRUE;
    timer_step = 0U;
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_TIMEOUT);
    CHECK(!result.started);
    CHECK(command_host.dma_faulted);
    CHECK(invalidate_count == 1U);
    CHECK(rint_read_count == 0U);
    check_response_zero(&result);
}

static void test_busy_response_waits_and_times_out(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;

    begin_case();
    setup_case();
    request = make_command(0U, 7U, MMC_RSP_R1b);
    post_start_busy = TRUE;
    busy_reads_before_clear = 3U;
    CHECK(p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_OK);
    CHECK(post_start_status_reads == 4U);

    setup_case();
    request = make_command(0U, 7U, MMC_RSP_R1b);
    post_start_busy = TRUE;
    busy_stuck = TRUE;
    timer_step = 100000UL;
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_TIMEOUT);
    CHECK(post_start_status_reads == 10U);
    check_response_zero(&result);
}

static void test_dma_reset_failure_invalidates_without_copy(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;

    begin_case();
    setup_case();
    configure_data_request(&request, 0U, 18U, FALSE, 512U, 512U);
    next_raw = P4SD_INT_CMD_DONE | P4SD_INT_DTO;
    dma_reset_value = FALSE;
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_ERROR);
    CHECK(dma_reset_count == 1U);
    CHECK(command_host.dma_faulted);
    CHECK(invalidate_count == 1U);
    CHECK(dma_complete_count == 0U);
    CHECK(client_data[4] == 0xCCU && client_data[4U + 512U] == 0xCCU);
    check_response_zero(&result);
}

static void test_initial_fifo_reset_failure_invalidates(void)
{
    struct P4HostCommand request;
    struct P4HostCommandResult result;

    begin_case();
    setup_case();
    configure_data_request(&request, 0U, 18U, FALSE, 512U, 512U);
    fifo_reset_value = FALSE;
    CHECK(!p4host_command(&command_host, &request, &result));
    CHECK(result.status == P4HOST_COMMAND_DMA_ERROR);
    CHECK(!result.started);
    CHECK(command_host.dma_faulted);
    CHECK(invalidate_count == 1U);
    CHECK(dma_prepare_count == 0U);
    CHECK(dma_complete_count == 0U);
    CHECK(client_data[4] == 0xCCU);
    check_response_zero(&result);
}

int main(void)
{
    test_actual_bounds_and_native_model();
    test_actual_rintsts_write_masks_radio_bits();
    test_invalid_requests_are_non_mmio();
    test_null_arguments_are_non_mmio();
    test_slot_and_generation_dispatch();
    test_cmd0_and_cmd12_encoding();
    test_long_response_is_msw_first_and_success_only();
    test_r5_mask_error_does_not_publish_response();
    test_diagnostic_validation_is_non_mmio();
    test_diagnostic_observer_and_no_observer_regression();
    test_diagnostic_fault_modes_reset_without_publication();
    test_data_crc_injection_beats_same_sample_early_cmd18();
    test_data_crc_injection_waits_for_delayed_cmd_done();
    test_diagnostic_callbacks_are_absent_before_start();
    test_slot1_cmd53_subword_tails_and_neighbours();
    test_slot1_cmd53_requires_data_over_and_all_descriptors();
    test_tx_data_is_staged_before_dma_arm();
    test_simultaneous_data_error_precedes_done_and_ready();
    test_late_raw_and_idmac_errors_prevent_copy();
    test_early_cmd18_is_storage_slot0_only();
    test_stalled_timer_is_bounded_by_poll_count();
    test_timeout_timer_wraps_and_command_wait_is_capped();
    test_busy_response_waits_and_times_out();
    test_dma_reset_failure_invalidates_without_copy();
    test_initial_fifo_reset_failure_invalidates();
    printf("PASS: %u cases, %u checks\n", test_cases, checks);
    return 0;
}
