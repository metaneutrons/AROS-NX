#ifndef P4_USB_ECHO_H
#define P4_USB_ECHO_H
#include <stddef.h>
#include <stdint.h>
#define P4_ECHO_CAPACITY 4096u
/* Single owner only; USB OUT is not consumed when this ring is full. */
struct p4_echo { uint8_t data[P4_ECHO_CAPACITY]; size_t head, used;
    uint32_t generation; uint64_t received, sent; };
void p4_echo_reset(struct p4_echo *e);
size_t p4_echo_space(const struct p4_echo *e);
int p4_echo_push(struct p4_echo *e, const void *data, size_t length);
size_t p4_echo_peek(const struct p4_echo *e, const uint8_t **data);
int p4_echo_consume(struct p4_echo *e, size_t length);
#endif
