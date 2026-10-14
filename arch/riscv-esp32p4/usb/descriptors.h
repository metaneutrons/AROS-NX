#ifndef P4_USB_DESCRIPTORS_H
#define P4_USB_DESCRIPTORS_H
#include <stdint.h>
/* Must be explicitly supplied for an authorized laboratory test. No product ID allocation. */
int p4_usb_set_identity(uint16_t vid, uint16_t pid);
#endif
