/* Copyright (C) 2026, The AROS Development Team.
 * Read-only touch diagnosis. No USB claims, remote grants or hardware writes.
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <stdio.h>
#include "port_resource.h"

int main(void)
{
    struct Library *IntuitionBase=OpenLibrary((CONST_STRPTR)"intuition.library",39);
    if(!IntuitionBase)return 20;
    struct P4USBPort *port=OpenResource((CONST_STRPTR)P4_USB_PORT_NAME);
    struct EasyStruct request={sizeof(request),0,(STRPTR)"P4 HS USB - read-only status",
        NULL,(STRPTR)"Refresh|Close"};
    for(;;) {
        char text[1100];struct P4USBSnapshot s;
        int result=0;
        if(port && port->abi==P4_USB_PORT_ABI && port->size>=sizeof(*port) && port->snapshot)
            result=port->snapshot(&s,sizeof(s));
        if(result==1 && s.version==P4_USB_SNAPSHOT_VERSION && s.size==sizeof(s)) {
            snprintf(text,sizeof(text),
                "State %lu phase %lu loops %lu poll %lu\n"
                "Reset-end %lu  SETUP %lu  mounts %lu\n"
                "ID %08lx  HWCFG2 %08lx  CFG4 %08lx\n"
                "IRQ seen %08lx  now %08lx  mask %08lx\n"
                "OTG %08lx  AHB %08lx  USB %08lx\n"
                "DCFG %08lx  DCTL %08lx  DSTS %08lx\n"
                "PCGC %08lx  HPCLK %08lx\n"
                "LPCLK %08lx  SUSPENDM %08lx  UTMI %08lx\n"
                "EP0 OUT ctl %08lx size %08lx irq %08lx\n\n"
                "DCD resets %lu size reset %08lx enum %08lx\n"
                "RX IRQ %lu SETUP RX %lu DONE %lu\n\n"
                "State: 0 starting, 1 ready, 2 faulted\n"
                "Phase: 1 prerequisites, 2 PHY, 3 core, 4 polling, 9 fault\n"
                "Cached by USB owner. Refresh checks progress.\n"
                "No remote access, reset or flash is authorized here.",
                (unsigned long)s.state,(unsigned long)s.phase,(unsigned long)s.loops,
                (unsigned long)s.polling,
                (unsigned long)s.resets,(unsigned long)s.setups,(unsigned long)s.mounts,
                (unsigned long)s.id,(unsigned long)s.hwcfg2,(unsigned long)s.hwcfg4,
                (unsigned long)s.irq_seen,(unsigned long)s.gintsts,(unsigned long)s.gintmsk,
                (unsigned long)s.gotgctl,(unsigned long)s.gahbcfg,(unsigned long)s.gusbcfg,
                (unsigned long)s.dcfg,(unsigned long)s.dctl,(unsigned long)s.dsts,
                (unsigned long)s.pcgcctl,(unsigned long)s.hp_clock,
                (unsigned long)s.lp_control,(unsigned long)s.suspendm,(unsigned long)s.utmi_fc06,
                (unsigned long)s.doepctl0,(unsigned long)s.doeptsiz0,(unsigned long)s.doepint0,
                (unsigned long)s.dcd_resets,(unsigned long)s.reset_size,(unsigned long)s.enum_size,
                (unsigned long)s.rx_irqs,(unsigned long)s.setup_rx,(unsigned long)s.setup_done);
        } else {
            snprintf(text,sizeof(text),"%s\nNo USB operation has been started.",
                result<0?"Snapshot temporarily busy; use Refresh.":
                "Matched permanent USBPort/snapshot unavailable.");
        }
        request.es_TextFormat=(STRPTR)text;
        if(EasyRequestArgs(NULL,&request,NULL,NULL)!=1)break;
    }
    CloseLibrary(IntuitionBase);return 0;
}
