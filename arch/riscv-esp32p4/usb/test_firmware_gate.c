#include "firmware_gate.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void)
{
    const uint8_t *p=p4_usb_firmware_query(0xc0,0x50,0,0,16);
    assert(p && !memcmp(p,"P4UP\1\0\1\0",8));
    for(unsigned i=8;i<16;++i)assert(p[i]==0);
    for(unsigned type=0;type<256;++type)
        if(type!=0xc0)assert(!p4_usb_firmware_query(type,0x50,0,0,16));
    for(unsigned op=0;op<256;++op)
        if(op!=0x50)assert(!p4_usb_firmware_query(0xc0,op,0,0,16));
    assert(!p4_usb_firmware_query(0xc0,0x50,1,0,16));
    assert(!p4_usb_firmware_query(0xc0,0x50,0,1,16));
    for(unsigned n=0;n<65536;++n)
        if(n!=16)assert(!p4_usb_firmware_query(0xc0,0x50,0,0,n));
    puts("firmware gate: exact capability request accepted; all mutation/shape counterprobes refused");
}
