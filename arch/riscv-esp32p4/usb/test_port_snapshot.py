#!/usr/bin/env python3
"""Actual-source cached snapshot tests; no hardware or USB is opened."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parent
header = (root / "port_resource.h").read_text()
source = (root / "port_resource.c").read_text()
structure = header[header.index("struct P4USBSnapshot {"):]
structure = structure[:structure.index("};") + 2]
accessor = source[source.index("static int port_snapshot("):source.index("void p4_usb_owner_check(")]
sampler = source[source.index("static void observe("):source.index("static int log_pop(")]
events = source[source.index("static uint32_t reset_events,"):source.index("/* Separate, read-only firmware")]
trace = source[source.index("static uint32_t dcd_resets,"):source.index("static bool try_lock(void)")]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define P4_USB_PORT 1
#include "p4_usb_platform.h"
#define P4_USB_SNAPSHOT_VERSION 3u
''' + structure + r'''
static struct P4USBSnapshot observation;
static bool busy, controller_touched, polling;
static unsigned reads, locks;
static uint32_t current_state;
static bool try_lock(void) { if(busy)return false; ++locks; return true; }
static void unlock(void) { assert(locks); --locks; }
static uint32_t port_state(void) { return current_state; }
static uint32_t mmio(uintptr_t address) {
    ++reads;
    switch(address) {
    case 0x500e6018u: case 0x50111048u: case 0x500e515cu:
    case 0x50000040u: case 0x50000048u: case 0x50000000u:
    case 0x50000008u: case 0x5000000cu: case 0x50000014u:
    case 0x50000018u: case 0x50000800u: case 0x50000804u:
    case 0x50000808u: case 0x50000e00u: case 0x5009c018u:
    case 0x50000050u: case 0x50000b00u: case 0x50000b10u:
    case 0x50000b08u:
        return (uint32_t)address;
    default: assert(!"unreviewed/destructive register read"); return 0;
    }
}
enum { DCD_EVENT_BUS_RESET_START=1, DCD_EVENT_BUS_RESET_END,
       DCD_EVENT_UNPLUGGED, DCD_EVENT_SETUP_RECEIVED };
static unsigned invalidations;
static void bus_changed(void) { ++invalidations; }
static unsigned owner_checks;
void p4_usb_owner_check(void) { ++owner_checks; }
''' + trace + accessor + sampler + events + r'''
int main(void) {
    p4_usb_dcd_trace(P4_USB_TRACE_RESET,0x60000000u);
    p4_usb_dcd_trace(P4_USB_TRACE_ENUM,0x20000000u);
    p4_usb_dcd_trace(P4_USB_TRACE_RX_IRQ,0);
    p4_usb_dcd_trace(P4_USB_TRACE_SETUP_RX,0);
    p4_usb_dcd_trace(P4_USB_TRACE_SETUP_DONE,0);
    p4_usb_dcd_trace(P4_USB_TRACE_RESET,0);
    assert(owner_checks==6 && dcd_resets==2 && reset_size==0);
    assert(enum_size==0x20000000u && rx_irqs==1 && setup_rx==1 && setup_done==1);
    /* The pinned DWC2 emits END only. Optional START must not be required
     * for either reset accounting or stale-session invalidation. */
    tud_event_hook_cb(1,DCD_EVENT_BUS_RESET_END,true);
    assert(reset_events==1 && invalidations==1);
    tud_event_hook_cb(1,DCD_EVENT_BUS_RESET_START,true);
    assert(reset_events==1 && invalidations==2);
    tud_event_hook_cb(1,DCD_EVENT_BUS_RESET_END,true);
    assert(reset_events==2 && invalidations==3);
    tud_event_hook_cb(1,DCD_EVENT_SETUP_RECEIVED,true);
    assert(setup_events==1 && reset_events==2 && invalidations==3);
    tud_event_hook_cb(1,DCD_EVENT_UNPLUGGED,true);
    assert(invalidations==4);
    tud_event_hook_cb(1,999,false);
    assert(invalidations==4 && reset_events==2);
    tud_mount_cb();tud_umount_cb();
    assert(mount_events==1 && invalidations==6);
    struct P4USBSnapshot out, before;
    memset(&out,0xa5,sizeof(out));before=out;
    assert(port_snapshot(NULL,sizeof(out))==0);
    assert(port_snapshot(&out,sizeof(out)-1)==0);
    assert(port_snapshot(&out,sizeof(out)+1)==0);
    assert(!memcmp(&out,&before,sizeof(out)) && !reads && !locks);
    busy=true;
    assert(port_snapshot(&out,sizeof(out))==-1);
    assert(!memcmp(&out,&before,sizeof(out)) && !reads && !locks);
    busy=false;current_state=2;
    assert(port_snapshot(&out,sizeof(out))==1 && !reads && !locks);
    assert(out.version==3 && out.size==sizeof(out) && out.state==2 && out.id==0);
    observe(1,0,0,0,0,0);
    assert(reads==3 && observation.id==0 && observation.phase==1 && !locks);
    controller_touched=true;polling=true;reads=0;
    observe(4,100,0x1234,2,3,1);
    assert(reads==19 && !locks);
    current_state=1;reads=0;
    assert(port_snapshot(&out,sizeof(out))==1 && reads==0);
    assert(out.id==0x50000040u && out.hwcfg2==0x50000048u);
    assert(out.pcgcctl==0x50000e00u && out.utmi_fc06==0x5009c018u);
    assert(out.hwcfg4==0x50000050u && out.doepctl0==0x50000b00u);
    assert(out.doeptsiz0==0x50000b10u && out.doepint0==0x50000b08u && out.polling==1);
    assert(out.dcd_resets==2 && out.reset_size==0 && out.enum_size==0x20000000u);
    assert(out.rx_irqs==1 && out.setup_rx==1 && out.setup_done==1);
    assert(out.phase==4 && out.loops==100 && out.irq_seen==0x1234);
    assert(out.resets==2 && out.setups==3 && out.mounts==1);
    out.loops=999;assert(observation.loops==100);
    before=observation;busy=true;
    observe(9,1000,0xffffffffu,10,20,30);
    assert(!memcmp(&before,&observation,sizeof(before)));
    busy=false;current_state=2;
    assert(port_snapshot(&out,sizeof(out))==1 && out.state==2 && out.loops==100);
    assert(!locks);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="p4-usb-snapshot-") as directory:
    base = Path(directory)
    unit = base / "fixture.c"
    unit.write_text(fixture)
    standalone = base / "standalone.c"
    standalone.write_text('#include <assert.h>\n#include "p4_usb_platform.h"\n'
                          'int main(void) { volatile uint32_t reads=0;\n'
                          'p4_usb_dcd_trace(P4_USB_TRACE_RESET,++reads);\n'
                          'assert(reads==0);return 0;}\n')
    for opt in ("0", "2"):
        binary = base / ("test-O" + opt)
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall",
                        "-Wextra", "-Werror", "-O" + opt,
                        "-fsanitize=address,undefined", "-I", str(root),
                        str(unit), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        noop = base / ("standalone-O" + opt)
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall",
                        "-Wextra", "-Werror", "-O" + opt, "-I", str(root),
                        str(standalone), "-o", str(noop)], check=True)
        subprocess.run([str(noop)], check=True)
        print("PASS cached snapshot accessor/sampler O" + opt, flush=True)
