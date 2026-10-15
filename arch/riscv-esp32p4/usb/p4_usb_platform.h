#ifndef P4_USB_PLATFORM_H
#define P4_USB_PLATFORM_H
#include <stdbool.h>
#include <stdint.h>
void p4_usb_owner_check(void);
void p4_usb_guard(void);
void p4_usb_fault(const char *reason);
void p4_usb_poll_enable(bool enabled);
uint32_t p4_usb_millis(void);
void p4_usb_delay(uint32_t ms);
enum p4_usb_trace_point {
    P4_USB_TRACE_RESET, P4_USB_TRACE_ENUM, P4_USB_TRACE_RX_IRQ,
    P4_USB_TRACE_SETUP_RX, P4_USB_TRACE_SETUP_DONE
};
#if defined(P4_USB_PORT)
void p4_usb_dcd_trace(enum p4_usb_trace_point point, uint32_t value);
#else
/* Do not evaluate MMIO arguments in the unchanged standalone experiment. */
#define p4_usb_dcd_trace(point, value) ((void)0)
#endif
#endif
