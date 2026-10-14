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
#endif
