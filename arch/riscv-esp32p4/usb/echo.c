/* Copyright (c) 2026, The AROS Development Team. */
#include "echo.h"
#include <string.h>
void p4_echo_reset(struct p4_echo *e)
{
    e->head = e->used = 0;
    if (++e->generation == 0) e->generation = 1;
    e->received = e->sent = 0;
    memset(e->data, 0, sizeof(e->data));
}
size_t p4_echo_space(const struct p4_echo *e) { return P4_ECHO_CAPACITY - e->used; }
int p4_echo_push(struct p4_echo *e, const void *data, size_t length)
{
    size_t tail, first;
    if (length > p4_echo_space(e) || (length && !data)) return 0;
    tail = (e->head + e->used) % P4_ECHO_CAPACITY;
    first = P4_ECHO_CAPACITY - tail;
    if (first > length) first = length;
    if (first) memcpy(e->data + tail, data, first);
    if (length > first) memcpy(e->data, (const uint8_t *)data + first, length - first);
    e->used += length; e->received += length;
    return 1;
}
size_t p4_echo_peek(const struct p4_echo *e, const uint8_t **data)
{
    size_t n = P4_ECHO_CAPACITY - e->head;
    if (n > e->used) n = e->used;
    *data = e->data + e->head;
    return n;
}
int p4_echo_consume(struct p4_echo *e, size_t length)
{
    if (length > e->used) return 0;
    e->head = (e->head + length) % P4_ECHO_CAPACITY;
    e->used -= length; e->sent += length;
    return 1;
}
