/* Copyright (C) 2026, The AROS Development Team. */
#include "firmware_gate.h"
#include <stddef.h>
const uint8_t *p4_usb_firmware_query(uint8_t type,uint8_t request,
    uint16_t value,uint16_t index,uint16_t length)
{
    /* magic, little-endian version/status, writable flag/reserved all zero. */
    static const uint8_t reply[P4_USB_FW_REPLY_SIZE]={'P','4','U','P',1,0,1,0};
    if(type!=0xc0 || request!=P4_USB_FW_QUERY || value || index ||
       length!=P4_USB_FW_REPLY_SIZE)return NULL;
    return reply;
}
