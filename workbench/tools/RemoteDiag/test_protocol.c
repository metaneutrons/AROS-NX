/* Source-bound host tests for protocol.c. Compile with:
   cc -std=c11 -Wall -Wextra -Werror -I. protocol.c test_protocol.c -o test_protocol
*/
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
    ++failures; return; \
} } while (0)

struct fake {
    unsigned calls;
    unsigned op;
    uint32_t ticket;
    size_t length;
    uint8_t body[RD_REQUEST_MAX];
    bool accept;
};

static bool submit(void *context, uint32_t ticket, unsigned op,
    const uint8_t *body, size_t length)
{
    struct fake *fake = context;
    ++fake->calls;
    fake->op = op;
    fake->ticket = ticket;
    fake->length = length;
    if (length <= sizeof(fake->body) && length)
        memcpy(fake->body, body, length);
    return fake->accept;
}

static uint32_t test_crc_update(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
    }
    return crc;
}

static uint32_t test_frame_crc(const uint8_t *frame, size_t body_length)
{
    uint32_t crc = test_crc_update(~0u, frame, 28);
    crc = test_crc_update(crc, frame + RD_HEADER, body_length);
    return ~crc;
}

static size_t request(uint8_t *frame, unsigned op, uint32_t id,
    uint64_t session, const uint8_t *body, size_t length)
{
    memset(frame, 0, RD_HEADER + length);
    memcpy(frame, "RDG1", 4);
    frame[4] = 1;
    frame[6] = (uint8_t)op;
    rd_put32(frame + 12, (uint32_t)length);
    rd_put32(frame + 16, id);
    rd_put64(frame + 20, session);
    if (length)
        memcpy(frame + RD_HEADER, body, length);
    rd_put32(frame + 28, test_frame_crc(frame, length));
    return RD_HEADER + length;
}

static size_t drain_response(struct rd_link *link, uint8_t *output, size_t capacity,
    size_t fragment)
{
    size_t total = 0;
    const uint8_t *part;
    size_t remaining = rd_link_peek(link, &part);
    if (remaining > capacity)
        return 0;
    memcpy(output, part, remaining);
    while (remaining) {
        size_t count = remaining < fragment ? remaining : fragment;
        if (!rd_link_consume(link, count))
            return 0;
        total += count;
        remaining = rd_link_peek(link, &part);
    }
    return total;
}

static bool response_is(const uint8_t *frame, size_t length, unsigned op,
    uint32_t status, uint32_t id, uint64_t session,
    const uint8_t *body, size_t body_length)
{
    if (length != RD_HEADER + body_length || memcmp(frame, "RDG1", 4) ||
        frame[4] != 1 || frame[5] || frame[6] != (uint8_t)op ||
        frame[7] != 0x80 || rd_u32(frame + 8) != status ||
        rd_u32(frame + 12) != body_length || rd_u32(frame + 16) != id ||
        rd_u64(frame + 20) != session || rd_u32(frame + 28) != test_frame_crc(frame, body_length))
        return false;
    return !body_length || memcmp(frame + RD_HEADER, body, body_length) == 0;
}

static void test_hello_bytewise_and_fragmented_response(void)
{
    struct fake fake = { .accept = true };
    struct rd_link link;
    uint8_t frame[RD_HEADER + RD_REQUEST_MAX], output[RD_HEADER + RD_DATA_MAX];
    uint8_t cookie[8] = { 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };
    uint8_t body[16] = { 1, 0, 0, 0, 0, 16, 0, 0, 0, 0, 64, 0, 1, 0, 0, 0 };
    const uint8_t *part;
    size_t length = request(frame, RD_HELLO, 1, 0, cookie, sizeof(cookie));
    rd_link_init(&link, submit, &fake);
    for (size_t i = 0; i < length; ++i)
        CHECK(rd_link_feed(&link, frame + i, 1) == 1);
    CHECK(fake.calls == 0);
    CHECK(rd_u64(&link.tx[20]) == rd_u64(cookie));
    CHECK(rd_u32(&link.tx[12]) == sizeof(body));
    CHECK(rd_u32(link.tx + RD_HEADER) == 1);
    CHECK(rd_u32(link.tx + RD_HEADER + 4) == RD_DATA_MAX);
    CHECK(rd_u32(link.tx + RD_HEADER + 8) == RD_SNAPSHOT_MAX);
    CHECK(rd_u32(link.tx + RD_HEADER + 12) == 1);
    length = drain_response(&link, output, sizeof(output), 3);
    CHECK(response_is(output, length, RD_HELLO, RD_OK, 1, rd_u64(cookie), body, sizeof(body)));
    CHECK(rd_link_peek(&link, &part) == 0);
}

static void test_pending_copy_completion_and_replay_rejection(void)
{
    struct fake fake = { .accept = true };
    struct rd_link link;
    uint8_t frame[RD_HEADER + RD_REQUEST_MAX], output[RD_HEADER + RD_DATA_MAX];
    uint8_t cookie[8] = { 9, 8, 7, 6, 5, 4, 3, 2 };
    uint8_t request_body[] = { 0x10, 0x20, 0x30, 0x40 };
    uint8_t completion[] = { 0xaa, 0xbb, 0xcc };
    uint64_t session = rd_u64(cookie);
    size_t length;
    rd_link_init(&link, submit, &fake);
    length = request(frame, RD_HELLO, 1, 0, cookie, sizeof(cookie));
    CHECK(rd_link_feed(&link, frame, length) == length);
    CHECK(drain_response(&link, output, sizeof(output), 7) == RD_HEADER + 16);
    length = request(frame, RD_STATUS, 2, session, request_body, sizeof(request_body));
    CHECK(rd_link_feed(&link, frame, length / 2) == length / 2);
    CHECK(fake.calls == 0);
    CHECK(rd_link_feed(&link, frame + length / 2, length - length / 2) == length - length / 2);
    CHECK(fake.calls == 1 && fake.op == RD_STATUS && fake.ticket == 1);
    CHECK(fake.length == sizeof(request_body));
    CHECK(memcmp(fake.body, request_body, sizeof(request_body)) == 0);
    memset(link.rx, 0, length); /* pending work must not borrow the receive buffer */
    CHECK(memcmp(fake.body, request_body, sizeof(request_body)) == 0);
    CHECK(rd_link_feed(&link, request_body, sizeof(request_body)) == 0);
    rd_link_complete(&link, fake.ticket, RD_OK, completion, sizeof(completion));
    length = drain_response(&link, output, sizeof(output), 5);
    CHECK(response_is(output, length, RD_STATUS, RD_OK, 2, session,
        completion, sizeof(completion)));

    length = request(frame, RD_MOVE, 3, session, request_body, sizeof(request_body));
    CHECK(rd_link_feed(&link, frame, length) == length && link.pending);
    CHECK(fake.calls == 2 && fake.ticket == 2 && fake.op == RD_MOVE);
    rd_link_complete(&link, 1, RD_OK, (const uint8_t *)"stale", 5);
    CHECK(link.pending && link.tx_size == 0);
    rd_link_complete(&link, fake.ticket, RD_OK, completion, sizeof(completion));
    length = drain_response(&link, output, sizeof(output), 5);
    CHECK(response_is(output, length, RD_MOVE, RD_OK, 3, session,
        completion, sizeof(completion)));

    length = request(frame, RD_MOVE, 2, session, NULL, 0);
    CHECK(rd_link_feed(&link, frame, length) == length);
    CHECK(fake.calls == 2);
    length = drain_response(&link, output, sizeof(output), 11);
    CHECK(response_is(output, length, RD_MOVE, RD_SESSION, 2, session, NULL, 0));
}

static void test_reset_discards_late_completion_and_rotates_generation(void)
{
    struct fake fake = { .accept = true };
    struct rd_link link;
    uint8_t frame[RD_HEADER + RD_REQUEST_MAX], output[RD_HEADER + RD_DATA_MAX];
    uint8_t cookie[8] = { 1, 3, 5, 7, 9, 11, 13, 15 };
    uint8_t next_cookie[8] = { 2, 4, 6, 8, 10, 12, 14, 16 };
    uint8_t hello_body[16];
    uint64_t old_session = rd_u64(cookie), new_session = rd_u64(next_cookie);
    const uint8_t *part;
    size_t length;
    rd_link_init(&link, submit, &fake);
    length = request(frame, RD_HELLO, 1, 0, cookie, sizeof(cookie));
    CHECK(rd_link_feed(&link, frame, length) == length);
    CHECK(drain_response(&link, output, sizeof(output), 19) == RD_HEADER + 16);
    length = request(frame, RD_CAPTURE, 2, old_session, NULL, 0);
    CHECK(rd_link_feed(&link, frame, length) == length && link.pending);
    uint32_t old_ticket = fake.ticket;
    uint32_t old_generation = link.generation;
    rd_link_reset(&link);
    CHECK(!link.pending && link.session == 0 && link.generation != old_generation);
    rd_link_complete(&link, old_ticket, RD_OK, (const uint8_t *)"late", 4);
    CHECK(rd_link_peek(&link, &part) == 0);
    length = request(frame, RD_HELLO, 1, 0, next_cookie, sizeof(next_cookie));
    CHECK(rd_link_feed(&link, frame, length) == length);
    rd_put32(hello_body, 1);
    rd_put32(hello_body + 4, RD_DATA_MAX);
    rd_put32(hello_body + 8, RD_SNAPSHOT_MAX);
    rd_put32(hello_body + 12, link.generation);
    CHECK(rd_u32(link.tx + RD_HEADER + 12) == link.generation);
    length = drain_response(&link, output, sizeof(output), 13);
    CHECK(response_is(output, length, RD_HELLO, RD_OK, 1, new_session,
        hello_body, sizeof(hello_body)));
    length = request(frame, RD_STATUS, 2, new_session, NULL, 0);
    CHECK(rd_link_feed(&link, frame, length) == length && link.pending);
    CHECK(fake.ticket == old_ticket + 1);
    rd_link_complete(&link, old_ticket, RD_OK, (const uint8_t *)"late", 4);
    CHECK(link.pending && link.tx_size == 0);
    rd_link_complete(&link, fake.ticket, RD_OK, NULL, 0);
    length = drain_response(&link, output, sizeof(output), 17);
    CHECK(response_is(output, length, RD_STATUS, RD_OK, 2, new_session, NULL, 0));
}

static void test_idle_hello_replaces_session_without_usb_reset(void)
{
    struct fake fake = { .accept = true };
    struct rd_link link;
    uint8_t frame[RD_HEADER + RD_REQUEST_MAX], output[RD_HEADER + RD_DATA_MAX];
    uint8_t first_cookie[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    uint8_t next_cookie[8] = { 2, 2, 2, 2, 2, 2, 2, 2 };
    uint8_t hello_body[16];
    uint64_t first_session = rd_u64(first_cookie), next_session = rd_u64(next_cookie);
    size_t length;
    rd_link_init(&link, submit, &fake);

    length = request(frame, RD_HELLO, 1, 0, first_cookie, sizeof(first_cookie));
    CHECK(rd_link_feed(&link, frame, length) == length);
    CHECK(drain_response(&link, output, sizeof(output), 23) == RD_HEADER + 16);
    CHECK(link.generation == 1 && link.session == first_session);

    /* A new host client may take over an idle link without resetting USB. */
    length = request(frame, RD_HELLO, 1, 0, next_cookie, sizeof(next_cookie));
    CHECK(rd_link_feed(&link, frame, length) == length);
    CHECK(link.generation == 2 && link.session == next_session && link.sequence == 1);
    rd_put32(hello_body, 1);
    rd_put32(hello_body + 4, RD_DATA_MAX);
    rd_put32(hello_body + 8, RD_SNAPSHOT_MAX);
    rd_put32(hello_body + 12, 2);
    length = drain_response(&link, output, sizeof(output), 7);
    CHECK(response_is(output, length, RD_HELLO, RD_OK, 1, next_session,
        hello_body, sizeof(hello_body)));

    /* Reusing the active cookie cannot create a second logical session. */
    length = request(frame, RD_HELLO, 1, 0, next_cookie, sizeof(next_cookie));
    CHECK(rd_link_feed(&link, frame, length) == length);
    length = drain_response(&link, output, sizeof(output), 9);
    CHECK(response_is(output, length, RD_HELLO, RD_SESSION, 1, next_session, NULL, 0));
    CHECK(link.generation == 2 && link.session == next_session && link.sequence == 1);

    /* The displaced host's old session is stale; current sequence still works. */
    length = request(frame, RD_STATUS, 2, first_session, NULL, 0);
    CHECK(rd_link_feed(&link, frame, length) == length);
    length = drain_response(&link, output, sizeof(output), 16);
    CHECK(response_is(output, length, RD_STATUS, RD_SESSION, 2, next_session, NULL, 0));
    CHECK(fake.calls == 0 && link.sequence == 1);

    length = request(frame, RD_STATUS, 2, next_session, NULL, 0);
    CHECK(rd_link_feed(&link, frame, length) == length && link.pending);
    rd_link_complete(&link, fake.ticket, RD_OK, NULL, 0);
    length = drain_response(&link, output, sizeof(output), 21);
    CHECK(response_is(output, length, RD_STATUS, RD_OK, 2, next_session, NULL, 0));
}

static void test_bad_crc_version_and_oversized_header_poison(void)
{
    struct fake fake = { .accept = true };
    struct rd_link link;
    uint8_t frame[RD_HEADER + RD_REQUEST_MAX];
    size_t length;
    rd_link_init(&link, submit, &fake);
    length = request(frame, RD_HELLO, 1, 0, (const uint8_t *)"12345678", 8);
    frame[length - 1] ^= 1;
    CHECK(rd_link_feed(&link, frame, length) == length);
    CHECK(link.poisoned && !link.tx_size && fake.calls == 0);

    rd_link_reset(&link);
    length = request(frame, RD_HELLO, 1, 0, (const uint8_t *)"12345678", 8);
    frame[4] = 2;
    CHECK(rd_link_feed(&link, frame, RD_HEADER) == RD_HEADER);
    CHECK(link.poisoned && !link.tx_size);

    rd_link_reset(&link);
    length = request(frame, RD_HELLO, 1, 0, NULL, 0);
    rd_put32(frame + 12, RD_REQUEST_MAX + 1u);
    CHECK(rd_link_feed(&link, frame, RD_HEADER) == RD_HEADER);
    CHECK(link.poisoned && !link.tx_size);
    (void)length;
}

static void test_max_request_and_invalid_completion(void)
{
    struct fake fake = { .accept = true };
    struct rd_link link;
    uint8_t frame[RD_HEADER + RD_REQUEST_MAX], output[RD_HEADER + RD_DATA_MAX];
    uint8_t cookie[8] = { 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xaa, 0xbb };
    uint8_t large_body[RD_REQUEST_MAX];
    size_t length;
    memset(large_body, 0x5a, sizeof(large_body));
    rd_link_init(&link, submit, &fake);
    length = request(frame, RD_HELLO, 1, 0, cookie, sizeof(cookie));
    CHECK(rd_link_feed(&link, frame, length) == length);
    CHECK(drain_response(&link, output, sizeof(output), 32) == RD_HEADER + 16);
    length = request(frame, RD_READ, 2, rd_u64(cookie), large_body, sizeof(large_body));
    CHECK(rd_link_feed(&link, frame, length) == length);
    CHECK(fake.calls == 1 && fake.length == RD_REQUEST_MAX);
    rd_link_complete(&link, fake.ticket, RD_OK, large_body, RD_DATA_MAX + 1u);
    length = drain_response(&link, output, sizeof(output), 97);
    CHECK(response_is(output, length, RD_READ, RD_IO, 2, rd_u64(cookie), NULL, 0));
}

static void test_submit_busy_and_consume_bounds(void)
{
    struct fake fake = { .accept = false };
    struct rd_link link;
    uint8_t frame[RD_HEADER + RD_REQUEST_MAX], output[RD_HEADER + RD_DATA_MAX];
    uint8_t cookie[8] = { 0x91, 0x82, 0x73, 0x64, 0x55, 0x46, 0x37, 0x28 };
    size_t length;
    rd_link_init(&link, submit, &fake);
    length = request(frame, RD_HELLO, 1, 0, cookie, sizeof(cookie));
    CHECK(rd_link_feed(&link, frame, length) == length);
    CHECK(drain_response(&link, output, sizeof(output), 64) == RD_HEADER + 16);
    length = request(frame, RD_STATUS, 2, rd_u64(cookie), NULL, 0);
    CHECK(rd_link_feed(&link, frame, length) == length);
    CHECK(fake.calls == 1 && !link.pending);
    const uint8_t *part;
    size_t remaining = rd_link_peek(&link, &part);
    CHECK(remaining == RD_HEADER && !rd_link_consume(&link, remaining + 1));
    CHECK(rd_link_consume(&link, remaining));
    CHECK(rd_link_peek(&link, &part) == 0);
}

static void test_fragmentation_sweep(void)
{
    for (size_t fragment = 1; fragment <= 37; ++fragment) {
        struct fake fake = { .accept = true };
        struct rd_link link;
        uint8_t frame[RD_HEADER + RD_REQUEST_MAX], output[RD_HEADER + RD_DATA_MAX];
        uint8_t cookie[8] = { 0xde, 0xad, 0xbe, 0xef, 0x10, 0x32, 0x54, 0x76 };
        uint8_t result[] = { 0x31, 0x41, 0x59 };
        const uint8_t *part;
        size_t length, offset = 0;
        rd_link_init(&link, submit, &fake);
        length = request(frame, RD_HELLO, 1, 0, cookie, sizeof(cookie));
        while (offset < length) {
            size_t count = length - offset < fragment ? length - offset : fragment;
            CHECK(rd_link_feed(&link, frame + offset, count) == count);
            offset += count;
        }
        CHECK(rd_link_peek(&link, &part) == RD_HEADER + 16);
        CHECK(drain_response(&link, output, sizeof(output), fragment) == RD_HEADER + 16);

        offset = 0;
        length = request(frame, RD_STATUS, 2, rd_u64(cookie), NULL, 0);
        while (offset < length) {
            size_t count = length - offset < fragment ? length - offset : fragment;
            CHECK(rd_link_feed(&link, frame + offset, count) == count);
            offset += count;
        }
        CHECK(fake.calls == 1 && link.pending);
        rd_link_complete(&link, fake.ticket, RD_OK, result, sizeof(result));
        length = drain_response(&link, output, sizeof(output), fragment);
        CHECK(response_is(output, length, RD_STATUS, RD_OK, 2, rd_u64(cookie),
            result, sizeof(result)));
    }
}

static void test_ticket_exhaustion_poison_closes_link(void)
{
    struct fake fake = { .accept = true };
    struct rd_link link;
    uint8_t frame[RD_HEADER + RD_REQUEST_MAX], output[RD_HEADER + RD_DATA_MAX];
    uint8_t cookie[8] = { 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98 };
    size_t length;
    rd_link_init(&link, submit, &fake);
    length = request(frame, RD_HELLO, 1, 0, cookie, sizeof(cookie));
    CHECK(rd_link_feed(&link, frame, length) == length);
    CHECK(drain_response(&link, output, sizeof(output), 64) == RD_HEADER + 16);
    link.serial = UINT32_MAX;
    length = request(frame, RD_STATUS, 2, rd_u64(cookie), NULL, 0);
    CHECK(rd_link_feed(&link, frame, length) == length);
    CHECK(link.poisoned && !link.pending && link.tx_size == 0 && fake.calls == 0);
    CHECK(rd_link_feed(&link, frame, length) == 0);
}

int main(void)
{
    test_hello_bytewise_and_fragmented_response();
    test_pending_copy_completion_and_replay_rejection();
    test_reset_discards_late_completion_and_rotates_generation();
    test_idle_hello_replaces_session_without_usb_reset();
    test_bad_crc_version_and_oversized_header_poison();
    test_max_request_and_invalid_completion();
    test_submit_busy_and_consume_bounds();
    test_fragmentation_sweep();
    test_ticket_exhaustion_poison_closes_link();
    if (failures)
        return EXIT_FAILURE;
    puts("RemoteDiag protocol tests passed");
    return EXIT_SUCCESS;
}
