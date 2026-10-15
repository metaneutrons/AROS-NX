/* Copyright (C) 2026, The AROS Development Team. */
#ifndef P4_USB_PORT_RESOURCE_H
#define P4_USB_PORT_RESOURCE_H
#include <exec/libraries.h>
#include <stdint.h>
#define P4_USB_PORT_NAME "esp32p4usb.resource"
#define P4_USB_PORT_ABI 1u
#define P4_USB_PORT_CHUNK 512u
enum p4_usb_port_state {
    P4_USB_PORT_STARTING, P4_USB_PORT_READY, P4_USB_PORT_FAULTED
};
/* Private permanent BSP resource. No caller enters TinyUSB or supplies a
 * callback. All operations are task-bound, bounded copies, with nonblocking
 * lock admission. A zero claim/read/write means unavailable or backpressure;
 * valid() distinguishes a retired session. USB detach/reset retires claims.
 * release must precede client task termination. valid/read/write renew a
 * 2000ms lease; a nonresponsive/crashed client is retired by the USB owner
 * without dereferencing the task pointer. Already submitted native backend
 * operations cannot be forcibly cancelled or retrospectively retracted.
 * Firmware writes are NOT implemented by this ABI. */
struct P4USBPort {
    struct Library library;
    struct ExecBase *sysbase;
    uint32_t abi, size;
    uint32_t (*state)(void);
    uint32_t (*claim)(void);
    int (*valid)(uint32_t token);
    int (*release)(uint32_t token);
    uint32_t (*read)(uint32_t token, void *bytes, uint32_t capacity);
    uint32_t (*write)(uint32_t token, const void *bytes, uint32_t length);
};
#endif
