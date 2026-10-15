#ifndef P4_PORT_BROKER_H
#define P4_PORT_BROKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The two fixed rings use 2048 bytes in total. */
#define P4_PORT_BROKER_RING_CAPACITY 1024u

enum p4_port_broker_status
{
    P4_PORT_BROKER_OK = 0,
    P4_PORT_BROKER_PARTIAL,
    P4_PORT_BROKER_WOULD_BLOCK,
    P4_PORT_BROKER_DRAINED,
    P4_PORT_BROKER_CLOSED,
    P4_PORT_BROKER_ALREADY_OPEN,
    P4_PORT_BROKER_ALREADY_CLOSED,
    P4_PORT_BROKER_NO_SERVICE,
    P4_PORT_BROKER_BUSY,
    P4_PORT_BROKER_STALE_SESSION,
    P4_PORT_BROKER_WRONG_OWNER,
    P4_PORT_BROKER_INVALID_ARGUMENT,
    P4_PORT_BROKER_INVALID_LENGTH,
    P4_PORT_BROKER_GENERATION_EXHAUSTED
};

struct p4_port_broker_counters
{
    /* All counters wrap modulo 2^32. */
    uint32_t rx_accepted;
    uint32_t rx_delivered;
    uint32_t tx_accepted;
    uint32_t tx_delivered;
    uint32_t rx_rejected;
    uint32_t queued_discarded;
};

/*
 * Caller-owned storage; no allocation or hidden synchronization is used.
 * Fields are public only so the object can be statically allocated. Callers
 * must treat them as private except in isolated tests. Initialize once before
 * publishing the broker to other contexts; do not reinitialize a live broker.
 */
struct p4_port_broker
{
    uint32_t last_generation;
    uint32_t active_generation;
    uintptr_t owner_key;
    bool claimed;
    bool open;

    uint16_t rx_head;
    uint16_t rx_tail;
    uint16_t rx_used;
    uint16_t tx_head;
    uint16_t tx_tail;
    uint16_t tx_used;
    uint8_t rx_ring[P4_PORT_BROKER_RING_CAPACITY];
    uint8_t tx_ring[P4_PORT_BROKER_RING_CAPACITY];

    struct p4_port_broker_counters counters;
};

/*
 * Every operation after initialization must be serialized by the caller.
 * Hold the caller's lock across USB peek, endpoint write, and USB consume.
 * The broker never calls USB/controller code and never retains data pointers.
 * Owner is an opaque, non-null object identity: only its uintptr_t key is kept
 * and compared; it is never dereferenced. The identity must be unique among
 * concurrent claimants. Tokens are monotonically issued and never reused.
 */
void p4_port_broker_init(struct p4_port_broker *broker);

enum p4_port_broker_status p4_port_broker_claim(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t *generation);

enum p4_port_broker_status p4_port_broker_release(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation);

/* USB reset invalidates the current session and queues; a new claim is needed. */
enum p4_port_broker_status p4_port_broker_usb_reset(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation);

enum p4_port_broker_status p4_port_broker_service_open(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation);

/* Closing clears both queues so a later open cannot replay stale bytes. */
enum p4_port_broker_status p4_port_broker_service_close(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation);

/*
 * USB input has already been removed from the endpoint when this is called.
 * With no service it is rejected; with a claimed but closed port it is
 * drained and dropped. accepted is the number copied into the RX ring.
 */
enum p4_port_broker_status p4_port_broker_usb_receive(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation,
    const uint8_t *bytes,
    size_t length,
    size_t *accepted);

/* Service-side read from bytes received by USB. */
enum p4_port_broker_status p4_port_broker_service_read(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation,
    uint8_t *bytes,
    size_t capacity,
    size_t *read_count);

/* Service-side write to the USB TX queue. Unaccepted suffixes remain caller-owned. */
enum p4_port_broker_status p4_port_broker_service_write(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation,
    const uint8_t *bytes,
    size_t length,
    size_t *accepted);

/* USB owner copies queued TX bytes without consuming them. */
enum p4_port_broker_status p4_port_broker_usb_peek(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation,
    uint8_t *bytes,
    size_t capacity,
    size_t *peeked);

/* Consume only bytes actually accepted by the USB endpoint. */
enum p4_port_broker_status p4_port_broker_usb_consume(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation,
    size_t count);

#endif
