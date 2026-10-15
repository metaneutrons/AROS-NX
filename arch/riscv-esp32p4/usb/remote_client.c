/* Copyright (C) 2026, The AROS Development Team.
 * Remote backend process, not a USB controller owner. Service lifetime is
 * the explicit grant. Only bounded copies cross the permanent resource ABI.
 */
#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdio.h>
#include "port_resource.h"
#include "../../../workbench/tools/RemoteDiag/bridge.h"

int main(void)
{
    SIPTR args[7]={0};
    struct RDArgs *rd=ReadArgs((CONST_STRPTR)"ENABLE/S,NONEAUTH/S,SCREEN/K/A,ROOT/K,ALLOWMOUSE/S,UNATTENDED/S,SECONDS/N",args,NULL);
    struct rd_bridge *bridge=NULL;struct P4USBPort *port=NULL;
    uint32_t token=0,elapsed=0;int result=RETURN_FAIL;
    if(!rd){PrintFault(IoErr(),(CONST_STRPTR)"USBRemote");return result;}
    LONG seconds=args[6]?*(LONG *)args[6]:600;
    if(!args[0] || !args[1] || (args[4]&&!args[5]) || seconds<1 || seconds>600) {
        puts("USBRemote: explicit ENABLE/NONEAUTH, 1..600 seconds, and UNATTENDED for mouse required");goto out;
    }
    port=OpenResource((CONST_STRPTR)P4_USB_PORT_NAME);
    if(!port || port->abi!=P4_USB_PORT_ABI || port->size<sizeof(*port) ||
       !port->state || !port->claim || !port->valid || !port->release || !port->read || !port->write ||
       port->state()!=P4_USB_PORT_READY) {
        puts("USBRemote: permanent HS owner unavailable");goto out;
    }
    bridge=rd_bridge_open((const char *)args[2],(const char *)args[3],args[4]!=0);
    if(!bridge){puts("USBDiag: worker startup failed");goto out;}
    /* Admission only after the actual backend exists. No grant at boot. */
    token=port->claim();
    if(!token){puts("USBRemote: remote endpoint unavailable/busy");goto out;}
    puts("USBRemote: service enabled; CDC lifetime independent; firmware writes not granted");
    while(elapsed<(uint32_t)seconds*50u) {
        if(SetSignal(0,SIGBREAKF_CTRL_C)&SIGBREAKF_CTRL_C)break;
        int valid=token?port->valid(token):0;
        if(valid==0) {
            if(token) {
                rd_bridge_reset(bridge);
                token=0;
            }
            rd_bridge_poll(bridge);
            if(port->state()!=P4_USB_PORT_READY)goto out;
            /* Stay disabled until owner has drained/reset transport queues.
             * No previous-session reply survives bridge reset. */
            token=port->claim();
            if(!token){Delay(1);++elapsed;continue;}
        } else if(valid<0){Delay(1);++elapsed;continue;}
        rd_bridge_poll(bridge);
        uint8_t bytes[P4_USB_PORT_CHUNK];
        size_t n=rd_bridge_room(bridge);
        if(n>sizeof(bytes))n=sizeof(bytes);
        if(n) {
            n=port->read(token,bytes,(uint32_t)n);
            if(n && rd_bridge_feed(bridge,bytes,n)!=n)goto out;
        }
        const uint8_t *p;
        n=rd_bridge_peek(bridge,&p);
        if(n>sizeof(bytes))n=sizeof(bytes);
        if(n) {
            uint32_t accepted=port->write(token,p,(uint32_t)n);
            if(!rd_bridge_consume(bridge,accepted))goto out;
        }
        Delay(1);++elapsed;
    }
    result=RETURN_OK;
out:
    /* Revoke before waiting on a potentially blocked native DOS worker.
     * Already running native operations cannot be forcibly cancelled. */
    if(port && token) {
        while(port->release(token)<0)Delay(1);
    }
    rd_bridge_close(bridge);
    if(result==RETURN_OK)puts("USBRemote: remote grant closed; CDC remains active");
    FreeArgs(rd);return result;
}
