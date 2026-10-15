/* Copyright (C) 2026, The AROS Development Team. */
#ifndef P4_USB_FIRMWARE_GATE_H
#define P4_USB_FIRMWARE_GATE_H
#include <stdbool.h>
#include <stdint.h>
#define P4_USB_FW_QUERY 0x50u
#define P4_USB_FW_REPLY_SIZE 16u
/* Only a capability query is implemented. A future writer must implement the
 * signed paired-image contract independently of remote/input permissions. */
const uint8_t *p4_usb_firmware_query(uint8_t type,uint8_t request,
    uint16_t value,uint16_t index,uint16_t length);
#endif
