#include "port_broker.h"

#include <string.h>

static void counter_add(uint32_t *counter, size_t amount)
{
    *counter += (uint32_t)amount;
}

static void clear_queues(struct p4_port_broker *broker)
{
    counter_add(&broker->counters.queued_discarded,
                (size_t)broker->rx_used + broker->tx_used);

    memset(broker->rx_ring, 0, sizeof(broker->rx_ring));
    memset(broker->tx_ring, 0, sizeof(broker->tx_ring));
    broker->rx_head = 0;
    broker->rx_tail = 0;
    broker->rx_used = 0;
    broker->tx_head = 0;
    broker->tx_tail = 0;
    broker->tx_used = 0;
}

static void invalidate_session(struct p4_port_broker *broker)
{
    clear_queues(broker);
    broker->open = false;
    broker->claimed = false;
    broker->owner_key = (uintptr_t)0;
    broker->active_generation = 0;
}

static enum p4_port_broker_status validate(
    const struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation)
{
    if (broker == NULL || owner == NULL || generation == 0)
        return P4_PORT_BROKER_INVALID_ARGUMENT;
    if (!broker->claimed)
        return P4_PORT_BROKER_NO_SERVICE;
    if ((uintptr_t)owner != broker->owner_key)
        return P4_PORT_BROKER_WRONG_OWNER;
    if (generation != broker->active_generation)
        return P4_PORT_BROKER_STALE_SESSION;
    return P4_PORT_BROKER_OK;
}

static size_t ring_put(uint8_t *ring,
                       uint16_t *tail,
                       uint16_t *used,
                       const uint8_t *bytes,
                       size_t length)
{
    size_t room = P4_PORT_BROKER_RING_CAPACITY - *used;
    size_t count = length < room ? length : room;
    size_t i;

    for (i = 0; i < count; ++i)
    {
        ring[*tail] = bytes[i];
        ++*tail;
        if (*tail == P4_PORT_BROKER_RING_CAPACITY)
            *tail = 0;
    }
    *used = (uint16_t)(*used + count);
    return count;
}

static size_t ring_get(uint8_t *ring,
                       uint16_t *head,
                       uint16_t *used,
                       uint8_t *bytes,
                       size_t capacity,
                       bool consume)
{
    size_t count = capacity < *used ? capacity : *used;
    size_t i;
    uint16_t position = *head;

    for (i = 0; i < count; ++i)
    {
        bytes[i] = ring[position];
        ++position;
        if (position == P4_PORT_BROKER_RING_CAPACITY)
            position = 0;
    }

    if (consume)
    {
        *head = position;
        *used = (uint16_t)(*used - count);
    }
    return count;
}

static enum p4_port_broker_status count_status(size_t count, size_t requested)
{
    if (count == requested)
        return P4_PORT_BROKER_OK;
    if (count != 0)
        return P4_PORT_BROKER_PARTIAL;
    return P4_PORT_BROKER_WOULD_BLOCK;
}

void p4_port_broker_init(struct p4_port_broker *broker)
{
    if (broker != NULL)
        memset(broker, 0, sizeof(*broker));
}

enum p4_port_broker_status p4_port_broker_claim(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t *generation)
{
    if (generation != NULL)
        *generation = 0;
    if (broker == NULL || owner == NULL || generation == NULL)
        return P4_PORT_BROKER_INVALID_ARGUMENT;
    if (broker->claimed)
        return P4_PORT_BROKER_BUSY;
    if (broker->last_generation == UINT32_MAX)
        return P4_PORT_BROKER_GENERATION_EXHAUSTED;

    clear_queues(broker);
    ++broker->last_generation;
    broker->active_generation = broker->last_generation;
    broker->owner_key = (uintptr_t)owner;
    broker->claimed = true;
    broker->open = false;
    *generation = broker->active_generation;
    return P4_PORT_BROKER_OK;
}

enum p4_port_broker_status p4_port_broker_release(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation)
{
    enum p4_port_broker_status status = validate(broker, owner, generation);
    if (status != P4_PORT_BROKER_OK)
        return status;

    invalidate_session(broker);
    return P4_PORT_BROKER_OK;
}

enum p4_port_broker_status p4_port_broker_usb_reset(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation)
{
    enum p4_port_broker_status status = validate(broker, owner, generation);
    if (status != P4_PORT_BROKER_OK)
        return status;

    invalidate_session(broker);
    return P4_PORT_BROKER_OK;
}

enum p4_port_broker_status p4_port_broker_service_open(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation)
{
    enum p4_port_broker_status status = validate(broker, owner, generation);
    if (status != P4_PORT_BROKER_OK)
        return status;
    if (broker->open)
        return P4_PORT_BROKER_ALREADY_OPEN;

    /* Enforce an empty boundary even if a caller reopens after unusual use. */
    clear_queues(broker);
    broker->open = true;
    return P4_PORT_BROKER_OK;
}

enum p4_port_broker_status p4_port_broker_service_close(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation)
{
    enum p4_port_broker_status status = validate(broker, owner, generation);
    if (status != P4_PORT_BROKER_OK)
        return status;
    if (!broker->open)
        return P4_PORT_BROKER_ALREADY_CLOSED;

    clear_queues(broker);
    broker->open = false;
    return P4_PORT_BROKER_OK;
}

enum p4_port_broker_status p4_port_broker_usb_receive(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation,
    const uint8_t *bytes,
    size_t length,
    size_t *accepted)
{
    enum p4_port_broker_status status;
    size_t count;

    if (accepted != NULL)
        *accepted = 0;
    if (broker == NULL || owner == NULL || generation == 0 || accepted == NULL ||
        (bytes == NULL && length != 0))
        return P4_PORT_BROKER_INVALID_ARGUMENT;

    status = validate(broker, owner, generation);
    if (status == P4_PORT_BROKER_NO_SERVICE)
    {
        counter_add(&broker->counters.rx_rejected, length);
        return status;
    }
    if (status != P4_PORT_BROKER_OK)
        return status;
    if (!broker->open)
    {
        counter_add(&broker->counters.rx_rejected, length);
        return length == 0 ? P4_PORT_BROKER_OK : P4_PORT_BROKER_DRAINED;
    }
    if (length == 0)
        return P4_PORT_BROKER_OK;

    count = ring_put(broker->rx_ring, &broker->rx_tail, &broker->rx_used,
                     bytes, length);
    *accepted = count;
    counter_add(&broker->counters.rx_accepted, count);
    counter_add(&broker->counters.rx_rejected, length - count);
    return count_status(count, length);
}

enum p4_port_broker_status p4_port_broker_service_read(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation,
    uint8_t *bytes,
    size_t capacity,
    size_t *read_count)
{
    enum p4_port_broker_status status;
    size_t count;

    if (read_count != NULL)
        *read_count = 0;
    if (broker == NULL || owner == NULL || generation == 0 || read_count == NULL ||
        (bytes == NULL && capacity != 0))
        return P4_PORT_BROKER_INVALID_ARGUMENT;

    status = validate(broker, owner, generation);
    if (status != P4_PORT_BROKER_OK)
        return status;
    if (!broker->open)
        return P4_PORT_BROKER_CLOSED;
    if (capacity == 0)
        return P4_PORT_BROKER_OK;

    count = ring_get(broker->rx_ring, &broker->rx_head, &broker->rx_used,
                     bytes, capacity, true);
    *read_count = count;
    counter_add(&broker->counters.rx_delivered, count);
    return count_status(count, capacity);
}

enum p4_port_broker_status p4_port_broker_service_write(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation,
    const uint8_t *bytes,
    size_t length,
    size_t *accepted)
{
    enum p4_port_broker_status status;
    size_t count;

    if (accepted != NULL)
        *accepted = 0;
    if (broker == NULL || owner == NULL || generation == 0 || accepted == NULL ||
        (bytes == NULL && length != 0))
        return P4_PORT_BROKER_INVALID_ARGUMENT;

    status = validate(broker, owner, generation);
    if (status != P4_PORT_BROKER_OK)
        return status;
    if (!broker->open)
        return P4_PORT_BROKER_CLOSED;
    if (length == 0)
        return P4_PORT_BROKER_OK;

    count = ring_put(broker->tx_ring, &broker->tx_tail, &broker->tx_used,
                     bytes, length);
    *accepted = count;
    counter_add(&broker->counters.tx_accepted, count);
    return count_status(count, length);
}

enum p4_port_broker_status p4_port_broker_usb_peek(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation,
    uint8_t *bytes,
    size_t capacity,
    size_t *peeked)
{
    enum p4_port_broker_status status;
    size_t count;

    if (peeked != NULL)
        *peeked = 0;
    if (broker == NULL || owner == NULL || generation == 0 || peeked == NULL ||
        (bytes == NULL && capacity != 0))
        return P4_PORT_BROKER_INVALID_ARGUMENT;

    status = validate(broker, owner, generation);
    if (status != P4_PORT_BROKER_OK)
        return status;
    if (!broker->open)
        return P4_PORT_BROKER_CLOSED;
    if (capacity == 0)
        return P4_PORT_BROKER_OK;

    count = ring_get(broker->tx_ring, &broker->tx_head, &broker->tx_used,
                     bytes, capacity, false);
    *peeked = count;
    return count_status(count, capacity);
}

enum p4_port_broker_status p4_port_broker_usb_consume(
    struct p4_port_broker *broker,
    const void *owner,
    uint32_t generation,
    size_t count)
{
    enum p4_port_broker_status status = validate(broker, owner, generation);
    if (status != P4_PORT_BROKER_OK)
        return status;
    if (!broker->open)
        return P4_PORT_BROKER_CLOSED;
    if (count > broker->tx_used)
        return P4_PORT_BROKER_INVALID_LENGTH;

    if (count != 0)
    {
        broker->tx_head = (uint16_t)(broker->tx_head + count);
        while (broker->tx_head >= P4_PORT_BROKER_RING_CAPACITY)
            broker->tx_head = (uint16_t)(broker->tx_head - P4_PORT_BROKER_RING_CAPACITY);
        broker->tx_used = (uint16_t)(broker->tx_used - count);
        counter_add(&broker->counters.tx_delivered, count);
    }
    return P4_PORT_BROKER_OK;
}
