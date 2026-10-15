#include "port_broker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(bool condition, const char *expression, int line)
{
    if (!condition)
    {
        fprintf(stderr, "test_port_broker:%d: check failed: %s\n", line, expression);
        exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

static void fill(uint8_t *bytes, size_t length, uint8_t seed)
{
    size_t i;
    for (i = 0; i < length; ++i)
        bytes[i] = (uint8_t)(seed + i * 37u);
}

static void test_no_service_and_claim_validation(void)
{
    struct p4_port_broker broker;
    int owner = 0, other = 0;
    const uint8_t input[] = { 0x01, 0x82, 0x03, 0xf4 };
    uint8_t output[8];
    uint32_t generation = 9;
    size_t count = 99;
    uint32_t other_generation = 17;

    p4_port_broker_init(&broker);
    CHECK(sizeof(broker.rx_ring) + sizeof(broker.tx_ring) == 2048);
    CHECK(p4_port_broker_claim(&broker, NULL, &generation) ==
          P4_PORT_BROKER_INVALID_ARGUMENT);
    CHECK(generation == 0);
    CHECK(p4_port_broker_claim(&broker, &owner, NULL) ==
          P4_PORT_BROKER_INVALID_ARGUMENT);

    CHECK(p4_port_broker_usb_receive(&broker, &owner, 1, input, sizeof(input),
                                     &count) == P4_PORT_BROKER_NO_SERVICE);
    CHECK(count == 0 && broker.rx_used == 0);
    CHECK(broker.counters.rx_rejected == sizeof(input));
    CHECK(p4_port_broker_usb_peek(&broker, &owner, 1, output, sizeof(output),
                                  &count) == P4_PORT_BROKER_NO_SERVICE);
    CHECK(count == 0);

    CHECK(p4_port_broker_claim(&broker, &owner, &generation) == P4_PORT_BROKER_OK);
    CHECK(generation == 1);
    count = 17;
    CHECK(p4_port_broker_claim(&broker, &other, &other_generation) ==
          P4_PORT_BROKER_BUSY);
    CHECK(other_generation == 0);
    CHECK(p4_port_broker_service_open(&broker, &owner, generation) ==
          P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_service_open(&broker, &owner, generation) ==
          P4_PORT_BROKER_ALREADY_OPEN);
    CHECK(p4_port_broker_service_close(&broker, &other, generation) ==
          P4_PORT_BROKER_WRONG_OWNER);
    CHECK(p4_port_broker_service_close(&broker, &owner, generation + 1u) ==
          P4_PORT_BROKER_STALE_SESSION);
    CHECK(p4_port_broker_release(&broker, &owner, generation) == P4_PORT_BROKER_OK);
}

static void test_rx_partial_backpressure_and_wrap(void)
{
    struct p4_port_broker broker;
    int owner = 0;
    uint32_t generation;
    uint8_t first[P4_PORT_BROKER_RING_CAPACITY];
    uint8_t tail[1300];
    uint8_t output[2048];
    size_t count;

    fill(first, sizeof(first), 7);
    fill(tail, sizeof(tail), 0xa1);
    p4_port_broker_init(&broker);
    CHECK(p4_port_broker_claim(&broker, &owner, &generation) == P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_service_open(&broker, &owner, generation) ==
          P4_PORT_BROKER_OK);

    CHECK(p4_port_broker_usb_receive(&broker, &owner, generation, first,
                                     sizeof(first), &count) == P4_PORT_BROKER_OK);
    CHECK(count == sizeof(first) && broker.rx_used == sizeof(first));
    CHECK(p4_port_broker_usb_receive(&broker, &owner, generation, tail, 2,
                                     &count) == P4_PORT_BROKER_WOULD_BLOCK);
    CHECK(count == 0 && broker.rx_used == P4_PORT_BROKER_RING_CAPACITY);
    CHECK(broker.counters.rx_rejected == 2);

    CHECK(p4_port_broker_service_read(&broker, &owner, generation, output, 100,
                                      &count) == P4_PORT_BROKER_OK);
    CHECK(count == 100 && memcmp(output, first, count) == 0);
    CHECK(p4_port_broker_usb_receive(&broker, &owner, generation, tail,
                                     sizeof(tail), &count) == P4_PORT_BROKER_PARTIAL);
    CHECK(count == 100 && broker.rx_used == P4_PORT_BROKER_RING_CAPACITY);
    CHECK(broker.counters.rx_rejected == 2 + sizeof(tail) - 100);

    CHECK(p4_port_broker_service_read(&broker, &owner, generation, output,
                                      P4_PORT_BROKER_RING_CAPACITY, &count) ==
          P4_PORT_BROKER_OK);
    CHECK(count == P4_PORT_BROKER_RING_CAPACITY);
    CHECK(memcmp(output, first + 100, sizeof(first) - 100) == 0);
    CHECK(memcmp(output + sizeof(first) - 100, tail, 100) == 0);
    CHECK(broker.rx_used == 0);
    CHECK(p4_port_broker_service_read(&broker, &owner, generation, output, 1,
                                      &count) == P4_PORT_BROKER_WOULD_BLOCK);
    CHECK(count == 0);

    /* Make the RX indices cross the physical end and preserve byte ordering. */
    CHECK(p4_port_broker_usb_receive(&broker, &owner, generation, first, 400,
                                     &count) == P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_service_read(&broker, &owner, generation, output, 300,
                                      &count) == P4_PORT_BROKER_OK);
    CHECK(memcmp(output, first, 300) == 0);
    CHECK(p4_port_broker_usb_receive(&broker, &owner, generation, tail, 1000,
                                     &count) == P4_PORT_BROKER_PARTIAL);
    CHECK(count == 924);
    CHECK(p4_port_broker_service_read(&broker, &owner, generation, output,
                                      1024, &count) ==
          P4_PORT_BROKER_OK);
    CHECK(count == 1024);
    CHECK(memcmp(output, first + 300, 100) == 0);
    CHECK(memcmp(output + 100, tail, 924) == 0);
    CHECK(p4_port_broker_release(&broker, &owner, generation) == P4_PORT_BROKER_OK);
}

static void test_tx_peek_consume_and_backpressure(void)
{
    struct p4_port_broker broker;
    int owner = 0;
    uint32_t generation;
    uint8_t payload[P4_PORT_BROKER_RING_CAPACITY];
    uint8_t extra[] = { 0x41, 0x42 };
    uint8_t output[P4_PORT_BROKER_RING_CAPACITY];
    size_t count;

    fill(payload, sizeof(payload), 0x31);
    p4_port_broker_init(&broker);
    CHECK(p4_port_broker_claim(&broker, &owner, &generation) == P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_service_open(&broker, &owner, generation) ==
          P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_service_write(&broker, &owner, generation, payload,
                                       sizeof(payload), &count) ==
          P4_PORT_BROKER_OK);
    CHECK(count == sizeof(payload));
    CHECK(p4_port_broker_service_write(&broker, &owner, generation, extra,
                                       sizeof(extra), &count) ==
          P4_PORT_BROKER_WOULD_BLOCK);
    CHECK(count == 0 && broker.tx_used == sizeof(payload));

    CHECK(p4_port_broker_usb_peek(&broker, &owner, generation, output, 512,
                                  &count) == P4_PORT_BROKER_OK);
    CHECK(count == 512 && memcmp(output, payload, count) == 0);
    CHECK(p4_port_broker_usb_peek(&broker, &owner, generation, output, 512,
                                  &count) == P4_PORT_BROKER_OK);
    CHECK(count == 512 && memcmp(output, payload, count) == 0);
    CHECK(p4_port_broker_usb_consume(&broker, &owner, generation, 200) ==
          P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_usb_consume(&broker, &owner, generation, 825) ==
          P4_PORT_BROKER_INVALID_LENGTH);
    CHECK(broker.tx_used == sizeof(payload) - 200);

    CHECK(p4_port_broker_usb_peek(&broker, &owner, generation, output,
                                  sizeof(output), &count) ==
          P4_PORT_BROKER_PARTIAL);
    CHECK(count == sizeof(payload) - 200);
    CHECK(memcmp(output, payload + 200, count) == 0);
    CHECK(p4_port_broker_usb_consume(&broker, &owner, generation, count) ==
          P4_PORT_BROKER_OK);
    CHECK(broker.tx_used == 0);
    CHECK(p4_port_broker_usb_peek(&broker, &owner, generation, output, 1,
                                  &count) == P4_PORT_BROKER_WOULD_BLOCK);
    CHECK(count == 0);
    CHECK(broker.counters.tx_accepted == sizeof(payload));
    CHECK(broker.counters.tx_delivered == sizeof(payload));
    CHECK(p4_port_broker_release(&broker, &owner, generation) == P4_PORT_BROKER_OK);
}

static void test_closed_port_discards_input_and_stale_output(void)
{
    struct p4_port_broker broker;
    int owner = 0;
    uint32_t generation;
    const uint8_t old_rx[] = { 1, 2, 3 };
    const uint8_t old_tx[] = { 9, 8, 7, 6 };
    const uint8_t closed_input[] = { 0xa0, 0xa1 };
    uint8_t output[16];
    size_t count;

    p4_port_broker_init(&broker);
    CHECK(p4_port_broker_claim(&broker, &owner, &generation) == P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_service_open(&broker, &owner, generation) ==
          P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_usb_receive(&broker, &owner, generation, old_rx,
                                     sizeof(old_rx), &count) == P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_service_write(&broker, &owner, generation, old_tx,
                                       sizeof(old_tx), &count) ==
          P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_service_close(&broker, &owner, generation) ==
          P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_service_close(&broker, &owner, generation) ==
          P4_PORT_BROKER_ALREADY_CLOSED);
    CHECK(broker.rx_used == 0 && broker.tx_used == 0);
    CHECK(broker.counters.queued_discarded == sizeof(old_rx) + sizeof(old_tx));

    CHECK(p4_port_broker_usb_receive(&broker, &owner, generation, closed_input,
                                     sizeof(closed_input), &count) ==
          P4_PORT_BROKER_DRAINED);
    CHECK(count == 0 && broker.rx_used == 0);
    CHECK(p4_port_broker_usb_peek(&broker, &owner, generation, output,
                                  sizeof(output), &count) ==
          P4_PORT_BROKER_CLOSED);
    CHECK(count == 0);
    CHECK(p4_port_broker_service_read(&broker, &owner, generation, output,
                                      sizeof(output), &count) ==
          P4_PORT_BROKER_CLOSED);
    CHECK(count == 0);
    CHECK(p4_port_broker_usb_consume(&broker, &owner, generation, 1) ==
          P4_PORT_BROKER_CLOSED);

    CHECK(p4_port_broker_service_open(&broker, &owner, generation) ==
          P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_usb_peek(&broker, &owner, generation, output, 1,
                                  &count) == P4_PORT_BROKER_WOULD_BLOCK);
    CHECK(p4_port_broker_service_read(&broker, &owner, generation, output, 1,
                                      &count) == P4_PORT_BROKER_WOULD_BLOCK);
    CHECK(broker.counters.rx_rejected == sizeof(closed_input));

    CHECK(p4_port_broker_release(&broker, &owner, generation) == P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_usb_receive(&broker, &owner, generation, closed_input,
                                     sizeof(closed_input), &count) ==
          P4_PORT_BROKER_NO_SERVICE);
    CHECK(count == 0 && broker.rx_used == 0);
    CHECK(p4_port_broker_claim(&broker, &owner, &generation) == P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_service_open(&broker, &owner, generation) ==
          P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_usb_peek(&broker, &owner, generation, output, 1,
                                  &count) == P4_PORT_BROKER_WOULD_BLOCK);
    CHECK(p4_port_broker_release(&broker, &owner, generation) == P4_PORT_BROKER_OK);
}

static void test_reset_invalidates_and_requires_reclaim(void)
{
    struct p4_port_broker broker;
    int owner = 0, other = 0;
    uint32_t old_generation, generation;
    const uint8_t payload[] = { 0x55, 0xaa, 0x1e };
    uint8_t output[8];
    size_t count;

    p4_port_broker_init(&broker);
    CHECK(p4_port_broker_claim(&broker, &owner, &old_generation) ==
          P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_service_open(&broker, &owner, old_generation) ==
          P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_usb_receive(&broker, &owner, old_generation, payload,
                                     sizeof(payload), &count) ==
          P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_service_write(&broker, &owner, old_generation, payload,
                                       sizeof(payload), &count) ==
          P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_usb_reset(&broker, &owner, old_generation) ==
          P4_PORT_BROKER_OK);
    CHECK(!broker.claimed && !broker.open && broker.rx_used == 0 &&
          broker.tx_used == 0);
    CHECK(broker.counters.queued_discarded == 2 * sizeof(payload));
    CHECK(p4_port_broker_service_write(&broker, &owner, old_generation, payload,
                                       sizeof(payload), &count) ==
          P4_PORT_BROKER_NO_SERVICE);
    CHECK(count == 0);

    CHECK(p4_port_broker_claim(&broker, &owner, &generation) == P4_PORT_BROKER_OK);
    CHECK(generation == old_generation + 1u);
    CHECK(p4_port_broker_service_open(&broker, &owner, generation) ==
          P4_PORT_BROKER_OK);
    CHECK(p4_port_broker_service_write(&broker, &owner, old_generation, payload,
                                       sizeof(payload), &count) ==
          P4_PORT_BROKER_STALE_SESSION);
    CHECK(count == 0);
    CHECK(p4_port_broker_service_write(&broker, &other, generation, payload,
                                       sizeof(payload), &count) ==
          P4_PORT_BROKER_WRONG_OWNER);
    CHECK(count == 0);
    CHECK(p4_port_broker_usb_peek(&broker, &owner, generation, output,
                                  sizeof(output), &count) ==
          P4_PORT_BROKER_WOULD_BLOCK);
    CHECK(count == 0);
    CHECK(p4_port_broker_release(&broker, &owner, generation) == P4_PORT_BROKER_OK);
}

static void test_generation_and_counter_wrap_fail_closed(void)
{
    struct p4_port_broker broker;
    int owner = 0;
    uint32_t generation;
    const uint8_t bytes[] = { 4, 3, 2, 1 };
    size_t count;

    p4_port_broker_init(&broker);
    /* Exercise the last representable token without exhausting the token space. */
    broker.last_generation = UINT32_MAX - 1u;
    CHECK(p4_port_broker_claim(&broker, &owner, &generation) == P4_PORT_BROKER_OK);
    CHECK(generation == UINT32_MAX);
    CHECK(p4_port_broker_service_open(&broker, &owner, generation) ==
          P4_PORT_BROKER_OK);
    broker.counters.rx_accepted = UINT32_MAX - 1u;
    CHECK(p4_port_broker_usb_receive(&broker, &owner, generation, bytes,
                                     sizeof(bytes), &count) == P4_PORT_BROKER_OK);
    CHECK(count == sizeof(bytes));
    CHECK(broker.counters.rx_accepted == 2u);
    CHECK(p4_port_broker_release(&broker, &owner, generation) == P4_PORT_BROKER_OK);

    generation = 99;
    CHECK(p4_port_broker_claim(&broker, &owner, &generation) ==
          P4_PORT_BROKER_GENERATION_EXHAUSTED);
    CHECK(generation == 0 && !broker.claimed);
    CHECK(p4_port_broker_service_open(&broker, &owner, UINT32_MAX) ==
          P4_PORT_BROKER_NO_SERVICE);
}

int main(void)
{
    test_no_service_and_claim_validation();
    test_rx_partial_backpressure_and_wrap();
    test_tx_peek_consume_and_backpressure();
    test_closed_port_discards_input_and_stale_output();
    test_reset_invalidates_and_requires_reclaim();
    test_generation_and_counter_wrap_fail_closed();
    puts("test_port_broker: pass");
    return EXIT_SUCCESS;
}
