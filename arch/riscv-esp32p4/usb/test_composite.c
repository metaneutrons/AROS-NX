/* Offline composition test for the existing echo, CDC-log and RemoteDiag
 * portable sources. It shares one bounded fake endpoint/controller fixture;
 * it does not emulate TinyUSB, DWC2, AROS scheduling, or physical USB.
 *
 * Compile with cdc_log.c, echo.c, RemoteDiag protocol.c/service.c/path.c,
 * plus test_service.c compiled as a renamed-main object. The latter supplies
 * the repository's existing fake native screen/filesystem backends.
 */
#include "cdc_log.h"
#include "echo.h"
#include "protocol.h"
#include "service.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FAKE_FIFO_CAPACITY 2048u
#define FAKE_CDC_ENDPOINT_CAPACITY 4u
#define FAKE_WIRE_CAPACITY 16384u
#define FAKE_VENDOR_WRITE_MAX 3u
#define FAKE_VENDOR_READ_MAX 7u

enum app_mode { APP_ECHO, APP_REMOTE };

struct byte_fifo {
    uint8_t data[FAKE_FIFO_CAPACITY];
    size_t head;
    size_t used;
};

struct fake_controller {
    struct byte_fifo vendor_host_to_device;
    struct byte_fifo vendor_device_to_host;
    struct byte_fifo cdc_host_to_device;
    struct byte_fifo cdc_device_to_host;
    bool mounted;
    bool cdc_dtr;
    size_t cdc_write_max;
    uint32_t vendor_write_calls;
    uint32_t vendor_flushes;
    uint32_t cdc_flushes;
    uint32_t cdc_discarded;
};

struct fake_worker {
    bool busy;
    bool started;
    uint32_t ticket;
    uint32_t generation;
    unsigned operation;
    size_t input_size;
    uint8_t input[RD_REQUEST_MAX];
    int status;
    size_t output_size;
    uint8_t output[RD_DATA_MAX];
};

struct fixture {
    enum app_mode mode;
    struct fake_controller controller;
    struct p4_echo echo;
    struct p4_console_ring console;
    struct p4_cdc_log cdc_log;
    struct rd_link link;
    struct rd_service service;
    struct fake_worker worker;
    bool service_clear_pending;
    uint32_t cdc_pop_calls;
};

static void check_fifo(const struct byte_fifo *fifo)
{
    assert(fifo->head < FAKE_FIFO_CAPACITY);
    assert(fifo->used <= FAKE_FIFO_CAPACITY);
}

static size_t fifo_room(const struct byte_fifo *fifo)
{
    return FAKE_FIFO_CAPACITY - fifo->used;
}

static size_t fifo_write(struct byte_fifo *fifo, const uint8_t *data,
                         size_t length)
{
    size_t tail, count, first;

    check_fifo(fifo);
    count = length < fifo_room(fifo) ? length : fifo_room(fifo);
    tail = (fifo->head + fifo->used) % FAKE_FIFO_CAPACITY;
    first = FAKE_FIFO_CAPACITY - tail;
    if (first > count)
        first = count;
    if (first)
        memcpy(fifo->data + tail, data, first);
    if (count > first)
        memcpy(fifo->data, data + first, count - first);
    fifo->used += count;
    return count;
}

static size_t fifo_read(struct byte_fifo *fifo, uint8_t *data, size_t length)
{
    size_t count, first;

    check_fifo(fifo);
    count = length < fifo->used ? length : fifo->used;
    first = FAKE_FIFO_CAPACITY - fifo->head;
    if (first > count)
        first = count;
    if (first)
        memcpy(data, fifo->data + fifo->head, first);
    if (count > first)
        memcpy(data + first, fifo->data, count - first);
    fifo->head = (fifo->head + count) % FAKE_FIFO_CAPACITY;
    fifo->used -= count;
    return count;
}

static int queue_bytes(struct byte_fifo *fifo, const void *data, size_t length)
{
    return fifo_write(fifo, (const uint8_t *)data, length) == length;
}

static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
    }
    return crc;
}

static uint32_t frame_crc(const uint8_t *frame, size_t body_size)
{
    uint32_t crc = crc_update(~0u, frame, 28);
    crc = crc_update(crc, frame + RD_HEADER, body_size);
    return ~crc;
}

static size_t make_request(uint8_t *frame, unsigned operation, uint32_t id,
                           uint64_t session, const uint8_t *body,
                           size_t body_size)
{
    assert(body_size <= RD_REQUEST_MAX);
    memset(frame, 0, RD_HEADER + body_size);
    memcpy(frame, "RDG1", 4);
    frame[4] = 1;
    frame[6] = (uint8_t)operation;
    rd_put32(frame + 12, (uint32_t)body_size);
    rd_put32(frame + 16, id);
    rd_put64(frame + 20, session);
    if (body_size)
        memcpy(frame + RD_HEADER, body, body_size);
    rd_put32(frame + 28, frame_crc(frame, body_size));
    return RD_HEADER + body_size;
}

static bool submit_job(void *context, uint32_t ticket, unsigned operation,
                       const uint8_t *body, size_t body_size)
{
    struct fixture *fixture = context;
    struct fake_worker *worker = &fixture->worker;

    if (worker->busy || body_size > sizeof(worker->input))
        return false;
    worker->busy = true;
    worker->started = false;
    worker->ticket = ticket;
    worker->generation = fixture->link.generation;
    worker->operation = operation;
    worker->input_size = body_size;
    if (body_size)
        memcpy(worker->input, body, body_size);
    return true;
}

static int console_pop(void *context, struct p4_console_record *record)
{
    struct fixture *fixture = context;

    ++fixture->cdc_pop_calls;
    return p4_console_ring_pop(&fixture->console, record);
}

static uint32_t console_dropped(void *context)
{
    const struct fixture *fixture = context;

    return p4_console_ring_dropped(&fixture->console);
}

static bool cdc_connected(void *context)
{
    const struct fixture *fixture = context;

    return fixture->controller.cdc_dtr;
}

static size_t cdc_room(void *context)
{
    const struct fixture *fixture = context;
    const struct byte_fifo *fifo = &fixture->controller.cdc_device_to_host;
    size_t room = fifo_room(fifo);

    if (room > FAKE_CDC_ENDPOINT_CAPACITY - fifo->used)
        room = FAKE_CDC_ENDPOINT_CAPACITY - fifo->used;
    return fixture->controller.cdc_dtr ? room : 0u;
}

static size_t cdc_write(void *context, const uint8_t *data, size_t length)
{
    struct fixture *fixture = context;
    struct fake_controller *controller = &fixture->controller;
    size_t count = length;

    if (count > controller->cdc_write_max)
        count = controller->cdc_write_max;
    if (count > cdc_room(context))
        count = cdc_room(context);
    return fifo_write(&controller->cdc_device_to_host, data, count);
}

static void cdc_flush(void *context)
{
    struct fixture *fixture = context;

    ++fixture->controller.cdc_flushes;
}

static size_t vendor_write(struct fixture *fixture, const uint8_t *data,
                           size_t length)
{
    size_t count = length;

    if (count > FAKE_VENDOR_WRITE_MAX)
        count = FAKE_VENDOR_WRITE_MAX;
    ++fixture->controller.vendor_write_calls;
    return fifo_write(&fixture->controller.vendor_device_to_host, data, count);
}

static void echo_owner_step(struct fixture *fixture)
{
    uint8_t input[FAKE_VENDOR_READ_MAX];
    const uint8_t *output;
    size_t count, written;

    if (!fixture->controller.mounted)
        return;
    count = p4_echo_space(&fixture->echo);
    if (count > sizeof(input))
        count = sizeof(input);
    if (count > fixture->controller.vendor_host_to_device.used)
        count = fixture->controller.vendor_host_to_device.used;
    if (count) {
        count = fifo_read(&fixture->controller.vendor_host_to_device,
                          input, count);
        assert(p4_echo_push(&fixture->echo, input, count));
    }

    count = p4_echo_peek(&fixture->echo, &output);
    if (count > FAKE_VENDOR_WRITE_MAX)
        count = FAKE_VENDOR_WRITE_MAX;
    if (count) {
        written = vendor_write(fixture, output, count);
        assert(p4_echo_consume(&fixture->echo, written));
        ++fixture->controller.vendor_flushes;
    }
}

static void remote_owner_step(struct fixture *fixture)
{
    struct fake_controller *controller = &fixture->controller;
    struct rd_link *link = &fixture->link;
    const uint8_t *output;
    size_t count, used;
    uint8_t input[FAKE_VENDOR_READ_MAX];

    if (!controller->mounted)
        return;

    /* Match the bridge's one-job backpressure: do not take vendor bytes while
     * a native job, reply, or partial response still owns the link.
     */
    if (!fixture->worker.busy && !link->pending && !link->tx_size &&
        !link->poisoned && controller->vendor_host_to_device.used) {
        count = controller->vendor_host_to_device.used;
        if (count > sizeof(input))
            count = sizeof(input);
        used = fifo_read(&controller->vendor_host_to_device, input, count);
        assert(rd_link_feed(link, input, used) == used);
    }

    count = rd_link_peek(link, &output);
    if (count) {
        used = vendor_write(fixture, output, count);
        if (used) {
            assert(rd_link_consume(link, used));
            ++controller->vendor_flushes;
        }
    }
}

static void cdc_owner_step(struct fixture *fixture)
{
    uint8_t discarded[64];
    size_t count = fixture->controller.cdc_host_to_device.used;

    /* This mirrors the application policy only: all CDC RX is discarded and
     * never fed to the vendor protocol, echo ring, reset, or flash controls.
     */
    if (count > sizeof(discarded))
        count = sizeof(discarded);
    count = fifo_read(&fixture->controller.cdc_host_to_device, discarded,
                      count);
    fixture->controller.cdc_discarded += (uint32_t)count;
    assert(p4_cdc_log_step(&fixture->cdc_log, 256u));
}

static void owner_poll(struct fixture *fixture)
{
    if (fixture->mode == APP_ECHO)
        echo_owner_step(fixture);
    else
        remote_owner_step(fixture);
    cdc_owner_step(fixture);
}

static void worker_start(struct fixture *fixture)
{
    struct fake_worker *worker = &fixture->worker;

    assert(worker->busy && !worker->started);
    worker->output_size = 0;
    if (worker->generation != fixture->link.generation)
        worker->status = RD_SESSION;
    else
        worker->status = rd_service_execute(&fixture->service,
                                            worker->operation,
                                            worker->input,
                                            worker->input_size,
                                            worker->output,
                                            &worker->output_size);
    worker->started = true;
}

static void worker_finish(struct fixture *fixture)
{
    struct fake_worker *worker = &fixture->worker;

    assert(worker->busy && worker->started);
    rd_link_complete(&fixture->link, worker->ticket, worker->status,
                     worker->output, worker->output_size);
    worker->busy = false;
    worker->started = false;
    if (fixture->service_clear_pending) {
        rd_service_clear(&fixture->service);
        fixture->service_clear_pending = false;
    }
}

static void fake_bus_reset(struct fixture *fixture)
{
    memset(&fixture->controller.vendor_host_to_device, 0,
           sizeof(fixture->controller.vendor_host_to_device));
    memset(&fixture->controller.vendor_device_to_host, 0,
           sizeof(fixture->controller.vendor_device_to_host));
    memset(&fixture->controller.cdc_host_to_device, 0,
           sizeof(fixture->controller.cdc_host_to_device));
    memset(&fixture->controller.cdc_device_to_host, 0,
           sizeof(fixture->controller.cdc_device_to_host));
    fixture->controller.mounted = false;
    fixture->controller.cdc_dtr = false;
    p4_echo_reset(&fixture->echo);
    p4_cdc_log_reset(&fixture->cdc_log);
    rd_link_reset(&fixture->link);
    fixture->service_clear_pending = true;
}

static void fake_reconnect(struct fixture *fixture)
{
    fixture->controller.mounted = true;
}

static size_t host_drain(struct byte_fifo *fifo, uint8_t *output,
                         size_t capacity)
{
    assert(fifo->used <= capacity);
    return fifo_read(fifo, output, fifo->used);
}

static void init_fixture(struct fixture *fixture)
{
    static const struct p4_cdc_source source_template = {
        NULL, console_pop, console_dropped
    };
    static const struct p4_cdc_sink sink_template = {
        NULL, cdc_connected, cdc_room, cdc_write, cdc_flush
    };
    struct p4_cdc_source source = source_template;
    struct p4_cdc_sink sink = sink_template;
    static uint64_t fake_screen_storage[4];
    static uint64_t fake_files_storage[4];

    memset(fixture, 0, sizeof(*fixture));
    fixture->mode = APP_ECHO;
    fixture->controller.cdc_write_max = 3u;
    source.context = fixture;
    sink.context = fixture;
    fixture->service.screen = (struct rd_screen *)fake_screen_storage;
    fixture->service.files = (struct rd_files *)fake_files_storage;
    p4_echo_reset(&fixture->echo);
    rd_link_init(&fixture->link, submit_job, fixture);
    assert(p4_cdc_log_init(&fixture->cdc_log, &source, &sink));
    p4_cdc_log_reset(&fixture->cdc_log);
}

static void host_send_vendor(struct fixture *fixture, const void *data,
                             size_t length)
{
    assert(queue_bytes(&fixture->controller.vendor_host_to_device, data,
                       length));
}

static void check_frame(const uint8_t *frame, size_t length, unsigned operation,
                        unsigned status, uint32_t request_id,
                        uint64_t session, const uint8_t *body,
                        size_t body_size)
{
    assert(length == RD_HEADER + body_size);
    assert(memcmp(frame, "RDG1", 4) == 0);
    assert(frame[4] == 1u && frame[5] == 0u);
    assert(frame[6] == operation && frame[7] == 0x80u);
    assert(rd_u32(frame + 8) == status);
    assert(rd_u32(frame + 12) == body_size);
    assert(rd_u32(frame + 16) == request_id);
    assert(rd_u64(frame + 20) == session);
    assert(rd_u32(frame + 28) == frame_crc(frame, body_size));
    if (body_size)
        assert(memcmp(frame + RD_HEADER, body, body_size) == 0);
}

static size_t exchange(struct fixture *fixture, unsigned operation,
                       uint32_t request_id, uint64_t session,
                       const uint8_t *body, size_t body_size,
                       uint8_t *response, size_t capacity)
{
    uint8_t request[RD_HEADER + RD_REQUEST_MAX];
    size_t request_size = make_request(request, operation, request_id, session,
                                       body, body_size);
    size_t response_size = 0, expected_size = 0;

    host_send_vendor(fixture, request, request_size);
    for (unsigned poll = 0; poll < 2048u; ++poll) {
        owner_poll(fixture);
        if (fixture->worker.busy) {
            if (!fixture->worker.started)
                worker_start(fixture);
            worker_finish(fixture);
        }

        if (fixture->controller.vendor_device_to_host.used) {
            uint8_t chunk[FAKE_FIFO_CAPACITY];
            size_t count = host_drain(&fixture->controller.vendor_device_to_host,
                                      chunk, sizeof(chunk));
            assert(response_size + count <= capacity);
            memcpy(response + response_size, chunk, count);
            response_size += count;
        }
        if (response_size >= RD_HEADER && !expected_size)
            expected_size = RD_HEADER + rd_u32(response + 12);
        if (expected_size && response_size == expected_size)
            return response_size;
    }
    assert(!"RemoteDiag response did not complete through the fake endpoint");
    return 0;
}

static uint64_t do_hello(struct fixture *fixture, uint8_t *response,
                         size_t capacity, uint8_t cookie[8])
{
    uint8_t caps[16];
    uint64_t session;
    size_t length;

    length = exchange(fixture, RD_HELLO, 1u, 0u, cookie, 8u,
                      response, capacity);
    session = rd_u64(cookie);
    rd_put32(caps, 1u);
    rd_put32(caps + 4, RD_DATA_MAX);
    rd_put32(caps + 8, RD_SNAPSHOT_MAX);
    rd_put32(caps + 12, fixture->link.generation);
    check_frame(response, length, RD_HELLO, RD_OK, 1u, session,
                caps, sizeof(caps));
    return session;
}

static void test_echo_and_remote_with_cdc_blocked(void)
{
    struct fixture fixture;
    uint8_t response[RD_RESPONSE_MAX];
    uint8_t echo_input[] = { 'e', 'c', 'h', 'o', 0, 0x7f, 0xff, '\n' };
    uint8_t echo_output[sizeof(echo_input)];
    uint8_t hello_cookie[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t body[16] = { 0 };
    uint8_t expected[RD_DATA_MAX];
    uint64_t session;
    uint32_t snapshot;
    size_t output_size = 0, length;

    init_fixture(&fixture);
    fixture.controller.mounted = true;
    host_send_vendor(&fixture, echo_input, sizeof(echo_input));
    for (unsigned poll = 0; poll < 100u && output_size < sizeof(echo_input); ++poll) {
        owner_poll(&fixture);
        output_size += host_drain(&fixture.controller.vendor_device_to_host,
                                  echo_output + output_size,
                                  sizeof(echo_output) - output_size);
    }
    assert(output_size == sizeof(echo_input));
    assert(memcmp(echo_output, echo_input, sizeof(echo_input)) == 0);
    assert(fixture.echo.used == 0u && fixture.echo.received == sizeof(echo_input));
    assert(fixture.controller.vendor_write_calls > 1u);
    assert(!fixture.controller.cdc_dtr && fixture.cdc_pop_calls == 0u);

    /* The same bounded fake vendor FIFOs now carry RDG1 frames. The CDC
     * reader remains absent while status, screenshot and root-relative file
     * operations execute through the real protocol/service/path sources.
     */
    fixture.mode = APP_REMOTE;
    fixture.controller.vendor_write_calls = 0u;
    for (unsigned i = 0; i < 70u; ++i) {
        char record[8];
        int count = snprintf(record, sizeof(record), "L%02u\n", i);
        assert(count == 4);
        (void)p4_console_ring_push(&fixture.console, record, (size_t)count, 0u);
    }
    assert(fixture.console.count == P4_CONSOLE_RING_SLOTS);
    assert(p4_console_ring_dropped(&fixture.console) == 6u);

    session = do_hello(&fixture, response, sizeof(response), hello_cookie);
    rd_put32(expected, 5u); /* fake screen + file backends; mouse disabled */
    rd_put32(expected + 4, 0u);
    length = exchange(&fixture, RD_STATUS, 2u, session, NULL, 0u,
                      response, sizeof(response));
    check_frame(response, length, RD_STATUS, RD_OK, 2u, session,
                expected, 8u);

    length = exchange(&fixture, RD_CAPTURE, 3u, session, body, sizeof(body),
                      response, sizeof(response));
    assert(length == RD_HEADER + 20u);
    assert(memcmp(response, "RDG1", 4) == 0);
    assert(response[6] == RD_CAPTURE && rd_u32(response + 8) == RD_OK);
    snapshot = rd_u32(response + RD_HEADER);
    assert(snapshot != 0u);
    assert(rd_u32(response + RD_HEADER + 4) == 6u);
    assert(rd_u32(response + RD_HEADER + 8) == 2u);
    assert(rd_u32(response + RD_HEADER + 12) == 1u);
    assert(rd_u32(response + RD_HEADER + 16) == rd_crc((const uint8_t *)"RGBRGB", 6u));

    rd_put32(body, snapshot);
    rd_put32(body + 4, 1u);
    rd_put32(body + 8, 4u);
    length = exchange(&fixture, RD_CHUNK, 4u, session, body, 12u,
                      response, sizeof(response));
    check_frame(response, length, RD_CHUNK, RD_OK, 4u, session,
                (const uint8_t *)"GBRG", 4u);
    rd_put32(body, snapshot);
    length = exchange(&fixture, RD_RELEASE, 5u, session, body, 4u,
                      response, sizeof(response));
    check_frame(response, length, RD_RELEASE, RD_OK, 5u, session, NULL, 0u);

    length = exchange(&fixture, RD_STAT, 6u, session,
                      (const uint8_t *)"file", 5u, response, sizeof(response));
    assert(length == RD_HEADER + 212u);
    assert(rd_u32(response + 8) == RD_OK);
    assert(rd_u32(response + RD_HEADER) == 0xfacebeefu);
    assert(rd_u32(response + RD_HEADER + 4) == 6u);
    assert(strcmp((const char *)response + RD_HEADER + 24, "native") == 0);

    memset(body, 0, sizeof(body));
    rd_put32(body, 0u);
    rd_put32(body + 4, 6u);
    memcpy(body + 8, "file", 5u);
    length = exchange(&fixture, RD_READ, 7u, session, body, 13u,
                      response, sizeof(response));
    assert(length == RD_HEADER + 20u);
    snapshot = rd_u32(response + RD_HEADER);
    assert(rd_u32(response + RD_HEADER + 4) == 6u);
    rd_put32(body, snapshot);
    rd_put32(body + 4, 0u);
    rd_put32(body + 8, 6u);
    length = exchange(&fixture, RD_CHUNK, 8u, session, body, 12u,
                      response, sizeof(response));
    assert(length == RD_HEADER + 6u);
    for (size_t i = 0; i < 6u; ++i)
        assert(response[RD_HEADER + i] == 0u);
    rd_put32(body, snapshot);
    length = exchange(&fixture, RD_RELEASE, 9u, session, body, 4u,
                      response, sizeof(response));
    check_frame(response, length, RD_RELEASE, RD_OK, 9u, session, NULL, 0u);

    assert(!fixture.controller.cdc_dtr);
    assert(fixture.cdc_pop_calls == 0u);
    assert(fixture.console.count == P4_CONSOLE_RING_SLOTS);
    assert(fixture.controller.vendor_write_calls > 10u);
    assert(fixture.worker.busy == false);
    rd_service_clear(&fixture.service);
}

static void test_cdc_overflow_reset_and_stale_work(void)
{
    struct fixture fixture;
    uint8_t response[RD_RESPONSE_MAX];
    uint8_t request[RD_HEADER + RD_REQUEST_MAX];
    uint8_t body[16] = { 0 };
    uint8_t old_cookie[8] = { 0xa1, 0xa2, 0xa3, 0xa4, 0xb1, 0xb2, 0xb3, 0xb4 };
    uint8_t new_cookie[8] = { 0xc1, 0xc2, 0xc3, 0xc4, 0xd1, 0xd2, 0xd3, 0xd4 };
    uint8_t cdc_wire[FAKE_WIRE_CAPACITY + 1u] = { 0 };
    uint8_t expected_status[8];
    uint64_t old_session, new_session;
    uint32_t old_ticket, old_generation;
    size_t cdc_wire_size = 0, length, request_size;

    init_fixture(&fixture);
    fixture.mode = APP_REMOTE;
    fixture.controller.mounted = true;
    for (unsigned i = 0; i < 70u; ++i) {
        char record[8];
        int count = snprintf(record, sizeof(record), "L%02u\n", i);
        assert(count == 4);
        (void)p4_console_ring_push(&fixture.console, record, (size_t)count, 0u);
    }
    assert(fixture.console.count == P4_CONSOLE_RING_SLOTS);
    assert(p4_console_ring_dropped(&fixture.console) == 6u);
    old_session = do_hello(&fixture, response, sizeof(response), old_cookie);

    /* Keep one native capture executing across reset. The result is computed
     * by the existing RemoteDiag fake backend, then delivered after the link
     * generation has changed so the production protocol must reject it.
     */
    request_size = make_request(request, RD_CAPTURE, 2u, old_session,
                                body, sizeof(body));
    host_send_vendor(&fixture, request, request_size);
    for (unsigned poll = 0; poll < 100u && !fixture.worker.busy; ++poll)
        owner_poll(&fixture);
    assert(fixture.worker.busy && fixture.link.pending);
    old_ticket = fixture.worker.ticket;
    old_generation = fixture.link.generation;
    worker_start(&fixture);
    assert(fixture.worker.started && fixture.worker.status == RD_OK);

    /* A CDC reader is present briefly. Its endpoint has only four bytes of
     * room, so the continuity line is left partially queued before reset.
     */
    fixture.controller.cdc_dtr = true;
    fixture.controller.cdc_write_max = 4u;
    owner_poll(&fixture);
    cdc_wire_size += fifo_read(&fixture.controller.cdc_device_to_host,
                               cdc_wire + cdc_wire_size, 1u);
    cdc_wire[cdc_wire_size] = 0;
    assert(cdc_wire_size == 1u && cdc_wire[0] == '[');
    assert(fixture.cdc_log.position > 1u &&
           fixture.cdc_log.position < fixture.cdc_log.length);

    /* CDC terminal input is explicitly discarded and cannot become a
     * RemoteDiag request. Bytes still queued at bus reset are discarded with
     * the fake controller FIFO; no CDC input is replayed after reconnect.
     */
    assert(queue_bytes(&fixture.controller.cdc_host_to_device,
                       "RDG1 fake terminal command", 26u));
    owner_poll(&fixture);
    assert(fixture.controller.cdc_host_to_device.used == 0u);
    assert(fixture.controller.cdc_discarded == 26u);
    assert(fixture.link.session == old_session && fixture.worker.busy);

    /* Leave bytes in the vendor OUT FIFO while the native job is in flight.
     * The reset callback clears both endpoints and rd_link parser state.
     */
    assert(queue_bytes(&fixture.controller.vendor_host_to_device,
                       "stale-input", 11u));
    assert(fixture.link.rx_used == 0u);
    fake_bus_reset(&fixture);
    assert(fixture.link.generation != old_generation);
    assert(fixture.link.rx_used == 0u && fixture.link.rx_need == RD_HEADER);
    assert(fixture.controller.vendor_host_to_device.used == 0u);
    assert(fixture.controller.vendor_device_to_host.used == 0u);
    assert(fixture.controller.cdc_device_to_host.used == 0u);
    worker_finish(&fixture); /* old ticket completion must be ignored */
    assert(!fixture.link.pending && fixture.link.tx_size == 0u);
    assert(!fixture.worker.busy && !fixture.service.snapshot.data);

    fake_reconnect(&fixture);
    new_session = do_hello(&fixture, response, sizeof(response), new_cookie);
    assert(new_session != old_session);
    assert(fixture.link.serial >= 1u);
    assert(fixture.controller.vendor_host_to_device.used == 0u);

    /* Old host-session traffic is rejected before any native job is queued. */
    length = exchange(&fixture, RD_STATUS, 2u, old_session, NULL, 0u,
                      response, sizeof(response));
    check_frame(response, length, RD_STATUS, RD_SESSION, 2u, new_session,
                NULL, 0u);
    assert(!fixture.worker.busy);

    /* A new-session operation receives its own ticket and response. */
    rd_put32(expected_status, 5u);
    rd_put32(expected_status + 4, 0u);
    length = exchange(&fixture, RD_STATUS, 2u, new_session, NULL, 0u,
                      response, sizeof(response));
    check_frame(response, length, RD_STATUS, RD_OK, 2u, new_session,
                expected_status, sizeof(expected_status));
    assert(fixture.controller.vendor_write_calls > 0u);
    assert(fixture.controller.vendor_device_to_host.used == 0u);

    /* Reset continuity replaces the unsent CDC suffix with a fresh notice.
     * The initial byte already observed by the host is followed by one full
     * notice, then the unconditional loss warning and original raw records.
     */
    fixture.controller.cdc_dtr = true;
    fixture.controller.cdc_write_max = 3u;
    for (unsigned poll = 0; poll < 2048u; ++poll) {
        owner_poll(&fixture);
        if (fixture.controller.cdc_device_to_host.used) {
            size_t count = fifo_read(&fixture.controller.cdc_device_to_host,
                                     cdc_wire + cdc_wire_size,
                                     sizeof(cdc_wire) - cdc_wire_size);
            cdc_wire_size += count;
            cdc_wire[cdc_wire_size] = 0;
        }
        if (!fixture.console.count && fixture.cdc_log.position == 0u &&
            fixture.cdc_log.length == 0u)
            break;
    }
    assert(fixture.console.count == 0u);
    assert(strstr((const char *)cdc_wire,
                  "[[p4cdc stream reset; continuity not guaranteed]\n") ==
           (const char *)cdc_wire);
    assert(strstr((const char *)cdc_wire,
                  "[p4cdc overflow records=00000000]\n") == NULL);
    assert(strstr((const char *)cdc_wire,
                  "[p4cdc overflow records=00000006]\n") != NULL);
    assert(strstr((const char *)cdc_wire, "L00\n") != NULL);
    assert(strstr((const char *)cdc_wire, "stale-input") == NULL);
    assert(fixture.cdc_pop_calls >= P4_CONSOLE_RING_SLOTS);

    /* Reconnected CDC RX is still log-only; changing its bytes cannot alter
     * the live vendor session or enqueue a native operation.
     */
    {
        uint32_t sequence = fixture.link.sequence;
        uint32_t vendor_used = (uint32_t)fixture.controller.vendor_host_to_device.used;

        assert(queue_bytes(&fixture.controller.cdc_host_to_device,
                           "RESET FLASH", 11u));
        owner_poll(&fixture);
        assert(fixture.controller.cdc_host_to_device.used == 0u);
        assert(fixture.controller.cdc_discarded == 37u);
        assert(fixture.link.sequence == sequence);
        assert(fixture.controller.vendor_host_to_device.used == vendor_used);
        assert(!fixture.worker.busy);
    }

    assert(old_ticket != 0u);
    rd_service_clear(&fixture.service);
    puts("composite offline composition passes; USB controller/hardware not exercised");
}

int main(void)
{
    test_echo_and_remote_with_cdc_blocked();
    test_cdc_overflow_reset_and_stale_work();
    return 0;
}
