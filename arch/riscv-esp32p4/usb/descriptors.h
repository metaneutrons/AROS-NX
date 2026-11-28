#ifndef P4_USB_DESCRIPTORS_H
#define P4_USB_DESCRIPTORS_H
#include <stdint.h>

/* Stable host-facing interface layout; vendor bulk remains interface 0. */
enum {
    P4_USB_VENDOR_INTERFACE = 0,
    P4_USB_CDC_CONTROL_INTERFACE = 1,
    P4_USB_CDC_DATA_INTERFACE = 2
};

#define P4_USB_VENDOR_EP_OUT 0x01
#define P4_USB_VENDOR_EP_IN 0x81
#define P4_USB_CDC_EP_NOTIFY 0x82
#define P4_USB_CDC_EP_OUT 0x03
#define P4_USB_CDC_EP_IN 0x83
#define P4_USB_CDC_FIFO_SIZE 1024
#define P4_USB_CDC_WRITE_CHUNK_MAX 128

/* Must be explicitly supplied for an authorized laboratory test. No product ID allocation. */
int p4_usb_set_identity(uint16_t vid, uint16_t pid);
#endif
