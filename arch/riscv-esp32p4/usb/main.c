/* Copyright (c) 2026, The AROS Development Team.
 * USB echo experiment, not a remote-control service. Never installed/autostarted.
 */
#ifdef P4_USB_ECHO_HOST_TEST
#include "main_test_platform.h"
#else
#include <exec/types.h>
#include <exec/tasks.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <dos/dos.h>
#include "tusb.h"
#include "device/dcd.h"
#include "../sdmmc/host.h"
#endif
#include <setjmp.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "echo.h"
#include "descriptors.h"
#include "hs_phy.h"
#include "p4_usb_platform.h"
#ifdef P4_USB_DIAG
#include "bridge.h"
static struct rd_bridge *diagnostic;
#endif

static uint32_t echo_mmio_read(uintptr_t address)
{
#ifdef P4_USB_ECHO_HOST_TEST
    return p4_usb_test_read(address);
#else
    uint32_t value=*(volatile uint32_t *)address;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    return value;
#endif
}
static void echo_mmio_write(uintptr_t address,uint32_t value)
{
#ifdef P4_USB_ECHO_HOST_TEST
    p4_usb_test_write(address,value);
#else
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    *(volatile uint32_t *)address=value;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
#endif
}

/* Existing semaphore covers current runtime SDMMC/radio clock writers.
 * DSI/GDMA reinit remains outside it: explicitly require the steady normal
 * desktop, no panel/stress/reinit runner. This is not a general RCC service.
 * Never substitute a USB-private mutex or local Forbid() on an SMP system.
 */
static struct P4SDMMCHost *clock_host;
static bool rcc_try_lock(void *ctx) { (void)ctx; return clock_host && clock_host->acquire(clock_host,0); }
static void rcc_unlock(void *ctx) { (void)ctx; clock_host->release(clock_host); }

struct Device *TimerBase;
static struct Task *owner;
static struct timerequest *timer;
static struct MsgPort *timer_port;
struct echo_owner_record {
    struct MsgPort port;
    char name[32];
    uint32_t signature;
    p4_usb_hs_phy_t saved_phy;
};
static struct echo_owner_record *owner_record;
static struct MsgPort *owner_port;
static bool owner_claimed;
static jmp_buf failure;
static const char *fault_reason;
static bool polling_enabled;
static uint32_t call_start, call_polls;
static struct p4_echo echo;
#define phy (owner_record->saved_phy)
static bool phy_active;
static bool controller_touched;
static bool quarantine;

void p4_usb_fault(const char *reason) { fault_reason = reason; longjmp(failure, 1); }
void p4_usb_owner_check(void)
{
    if (!owner || FindTask(NULL) != owner) p4_usb_fault("non-owner stack access");
}
uint32_t p4_usb_millis(void)
{
    struct timeval tv;
    GetSysTime(&tv);
    return (uint32_t)tv.tv_secs * 1000u + (uint32_t)tv.tv_micro / 1000u;
}
void p4_usb_guard(void)
{
    p4_usb_owner_check();
    if (++call_polls > 100000u || (uint32_t)(p4_usb_millis() - call_start) >= 100u)
        p4_usb_fault("controller polling budget expired");
}
void p4_usb_delay(uint32_t ms)
{
    p4_usb_owner_check();
    if (ms > 100u) p4_usb_fault("oversized stack delay");
    timer->tr_node.io_Command = TR_ADDREQUEST;
    timer->tr_time.tv_secs = 0; timer->tr_time.tv_micro = ms * 1000u;
    if (DoIO((struct IORequest *)timer)) p4_usb_fault("timer failed");
}
void p4_usb_poll_enable(bool on) { p4_usb_owner_check(); polling_enabled = on; }
static void begin_call(void) { call_start=p4_usb_millis(); call_polls=0; }

void tud_event_hook_cb(uint8_t port, uint32_t event, bool in_isr)
{
    (void)port; (void)in_isr; p4_usb_owner_check();
    if (event == DCD_EVENT_BUS_RESET_START || event == DCD_EVENT_UNPLUGGED) {
        p4_echo_reset(&echo);
#ifdef P4_USB_DIAG
        if(diagnostic)rd_bridge_reset(diagnostic);
#endif
    }
}
void tud_mount_cb(void) {
    p4_echo_reset(&echo);
#ifdef P4_USB_DIAG
    if(diagnostic)rd_bridge_reset(diagnostic);
#endif
}
void tud_umount_cb(void) { tud_mount_cb(); }

static void echo_step(void)
{
    uint8_t buf[512]; const uint8_t *data; size_t n, room;
#ifdef P4_USB_DIAG
    rd_bridge_poll(diagnostic);
    if(!tud_mounted())return;
    room=rd_bridge_room(diagnostic);n=tud_vendor_available();
    if(n>room)n=room;
    if(n>sizeof(buf))n=sizeof(buf);
    if(n) {
        n=tud_vendor_read(buf,n);
        if(rd_bridge_feed(diagnostic,buf,n)!=n)p4_usb_fault("diagnostic receive state mismatch");
    }
    n=rd_bridge_peek(diagnostic,&data);room=tud_vendor_write_available();
    if(n>room)n=room;
    if(n>512)n=512;
    if(n) {
        n=tud_vendor_write(data,n);
        if(!rd_bridge_consume(diagnostic,n))p4_usb_fault("diagnostic partial write mismatch");
        tud_vendor_write_flush();
    }
#else
    if (!tud_mounted()) return;
    room = p4_echo_space(&echo);
    n = tud_vendor_available();
    if (n > room) n=room;
    if (n > sizeof(buf)) n=sizeof(buf);
    if (n) {
        n=tud_vendor_read(buf,n);
        if (!p4_echo_push(&echo,buf,n)) p4_usb_fault("echo ring overflow");
    }
    n=p4_echo_peek(&echo,&data);
    room=tud_vendor_write_available();
    if (n>room) n=room;
    if (n>512) n=512;
    if (n) {
        n=tud_vendor_write(data,n);
        if (!p4_echo_consume(&echo,n)) p4_usb_fault("invalid partial write");
        tud_vendor_write_flush();
    }
#endif
}

int main(void)
{
    SIPTR args[10]={0}; struct RDArgs *rd;
    uint32_t started;
    volatile uint32_t seconds=120;
    volatile int result=RETURN_FAIL;
    const p4_usb_hs_phy_platform_t platform={NULL,rcc_try_lock,rcc_unlock};
    owner_claimed=quarantine=phy_active=controller_touched=polling_enabled=false;
    owner_record=NULL; owner_port=NULL; owner=NULL; clock_host=NULL;
    fault_reason=NULL;
#ifdef P4_USB_DIAG
    diagnostic=NULL;
    rd=ReadArgs((CONST_STRPTR)"ENABLE/S,VID/N/A,PID/N/A,SECONDS/N,STEADYCLOCKS/S,SCREEN/K/A,ROOT/K,ALLOWMOUSE/S,NONEAUTH/S,UNATTENDED/S",args,NULL);
#else
    rd=ReadArgs((CONST_STRPTR)"ENABLE/S,VID/N/A,PID/N/A,SECONDS/N,STEADYCLOCKS/S",args,NULL);
#endif
    if (!rd) { PrintFault(IoErr(),(CONST_STRPTR)"USBEcho"); return RETURN_FAIL; }
    if (!args[0] || *(LONG *)args[1] <= 0 || *(LONG *)args[1]>65534 ||
        *(LONG *)args[2]<=0 || *(LONG *)args[2]>65534 ||
        !p4_usb_set_identity(*(LONG *)args[1],*(LONG *)args[2])) {
        puts("USBEcho: explicit ENABLE and authorized laboratory VID/PID required"); goto out;
    }
    if (args[3]) {
        LONG value=*(LONG *)args[3];
        if(value<1 || value>600) { puts("USBEcho: SECONDS must be 1..600"); goto out; }
        seconds=(uint32_t)value;
    }
    if(!args[4]) {
        puts("USBEcho: STEADYCLOCKS required: normal desktop, no concurrent display/stress reinitialization");
        goto out;
    }
#ifdef P4_USB_DIAG
    if(!args[8]) {
        puts("USBDiag: explicit NONEAUTH required; local USB has no authentication or encryption");goto out;
    }
    if(args[7]&&!args[9]) {
        puts("USBDiag: mouse requires UNATTENDED; concurrent local touch/mouse arbitration is not implemented");goto out;
    }
    /* Start the DOS worker before controller ownership. Native handler calls
       can wait; no such call runs in the USB owner or interrupt callback. */
    diagnostic=rd_bridge_open((const char *)args[5],(const char *)args[6],args[7]!=0);
    if(!diagnostic){puts("USBDiag: worker startup failed");goto out;}
    puts("USBDiag: unauthenticated USB diagnosis explicitly enabled; no file writes, drag or keyboard");
#endif
    clock_host=OpenResource((CONST_STRPTR)P4_SDMMC_HOST_NAME);
    if(!clock_host || clock_host->abi!=P4_SDMMC_HOST_ABI ||
        clock_host->size<sizeof(*clock_host) || !clock_host->acquire || !clock_host->release) {
        puts("USBEcho: matched permanent SDMMC clock owner required"); goto out;
    }
    owner_record=AllocVec(sizeof(*owner_record),MEMF_PUBLIC|MEMF_CLEAR);
    if(!owner_record) goto out;
    owner_port=&owner_record->port;
    owner_port->mp_Flags=PA_IGNORE;
    NewList(&owner_port->mp_MsgList);
    memcpy(owner_record->name,"AROS.P4.USBEcho.owner",sizeof("AROS.P4.USBEcho.owner"));
    owner_record->signature=0x50345545u;
    /* Fail before MMIO if the actual runtime clock-writer lock is busy. */
    if (!rcc_try_lock(NULL)) {
        puts("USBEcho: hardware start refused; SDMMC clock owner busy");
        goto out;
    }
    /* Every copy of this experiment uses the same permanent host semaphore
       for the check/add pair. A local Forbid/FindPort pair is not SMP-safe. */
    if(FindPort((CONST_STRPTR)"AROS.P4.USBEcho.owner")) {
        rcc_unlock(NULL);
        puts("USBEcho: another echo owner is present"); goto out;
    }
    owner_port->mp_Node.ln_Name=owner_record->name;
    AddPort(owner_port); owner_claimed=true;
    rcc_unlock(NULL);
    /* USB HS sources94/95 must be unrouted on BOTH harts. This polled
       experiment never steals a routed controller or unmasks a CLIC line. */
    for(unsigned hart=0;hart<2;++hart)
        for(unsigned source=94;source<=95;++source)
            if(echo_mmio_read(0x500d6000u+hart*0x800u+source*4u)&63u) {
                puts("USBEcho: HS IRQ source already routed; refusing ownership"); goto out;
            }
    owner=FindTask(NULL);
    timer_port=CreateMsgPort();
    if(!timer_port) goto out;
    timer=(struct timerequest *)CreateIORequest(timer_port,sizeof(*timer));
    if(!timer || OpenDevice((CONST_STRPTR)"timer.device",UNIT_MICROHZ,(struct IORequest *)timer,0)) goto out;
    TimerBase=timer->tr_node.io_Device;
    if(setjmp(failure)) { printf("USBEcho: %s\n",fault_reason); goto stop; }
    const p4_usb_hs_phy_result_t setup=p4_usb_hs_phy_setup(&phy,&platform);
    if(setup!=P4_USB_HS_PHY_OK) {
        phy_active=(setup==P4_USB_HS_PHY_RESTORE_ERROR);
        puts("USBEcho: PHY setup refused/failed"); goto stop;
    }
    phy_active=true;
    controller_touched=true;
    begin_call();
    const tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};
    if(!tusb_init(1,&init)) p4_usb_fault("TinyUSB init failed");
    printf("USBEcho: polled HS PIO ready; duration=%lu\n",(unsigned long)seconds);
    started=p4_usb_millis();
    while((uint32_t)(p4_usb_millis()-started)<seconds*1000u) {
        if(SetSignal(0,SIGBREAKF_CTRL_C)&SIGBREAKF_CTRL_C) break;
        begin_call();
        if(polling_enabled) dcd_int_handler(1);
        tud_task_ext(0,false);
        echo_step();
        p4_usb_delay(2);
    }
    result=RETURN_OK;
stop:
    /* Never enter another upstream wait loop after a longjmp. Fixed scoped
       DWC disconnect/mask first; then restore only PHY-owned fields. */
    if(controller_touched) {
        echo_mmio_write(0x50000018UL,0);
        echo_mmio_write(0x50000008UL,echo_mmio_read(0x50000008UL)&~1u);
        echo_mmio_write(0x50000804UL,echo_mmio_read(0x50000804UL)|2u);
        controller_touched=false;
    }
    if(phy_active) {
        p4_usb_hs_phy_result_t restored=P4_USB_HS_PHY_BUSY;
        const uint32_t cleanup_start=p4_usb_millis();
        for(unsigned attempt=0;attempt<16;++attempt) {
            restored=p4_usb_hs_phy_deinit(&phy);
            if(restored==P4_USB_HS_PHY_OK) break;
            if(attempt==15 || (uint32_t)(p4_usb_millis()-cleanup_start)>=500u) break;
            /* No longjmp from cleanup: timer failure terminates retries. */
            timer->tr_node.io_Command=TR_ADDREQUEST;
            timer->tr_time.tv_secs=0; timer->tr_time.tv_micro=1000;
            if(DoIO((struct IORequest *)timer)) break;
        }
        if(restored!=P4_USB_HS_PHY_OK) {
            result=RETURN_FAIL; quarantine=true;
            /* Permanent snapshot/lease, not dangling program code or task.
               This is a deliberate fault quarantine, reclaimed by reboot.
               No later echo process may take over this unresolved state. */
            phy.try_rcc_lock=NULL; phy.rcc_unlock=NULL; phy.lock_context=NULL;
            printf("USBEcho: restore failed (%d); owner/snapshot quarantined until reboot\n",restored);
        } else phy_active=false;
    }
out:
#ifdef P4_USB_DIAG
    rd_bridge_close(diagnostic);diagnostic=NULL;
#endif
    if(owner_claimed && !quarantine) { RemPort(owner_port); owner_claimed=false; }
    if(owner_record && !quarantine) FreeVec(owner_record);
    owner_port=NULL; owner_record=NULL;
    if(TimerBase) CloseDevice((struct IORequest *)timer);
    if(timer) { DeleteIORequest((struct IORequest *)timer); timer=NULL; }
    TimerBase=NULL;
    if(timer_port) { DeleteMsgPort(timer_port); timer_port=NULL; }
    FreeArgs(rd);
    return result;
}
