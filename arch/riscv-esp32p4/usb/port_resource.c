/* Copyright (C) 2026, The AROS Development Team.
 * Permanent, single-task HS device owner. No DOS or remote backend dependency.
 * Debug/FS Serial/JTAG remains untouched and is the recovery transport.
 */
#define TimerBase P4USBTimerBase
#include <aros/asmcall.h>
#include <aros/debug.h>
#include <aros/symbolsets.h>
#include <exec/lists.h>
#include <exec/resident.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <utility/tagitem.h>
#include <setjmp.h>
#include <string.h>
#include "tusb.h"
#include "device/dcd.h"
#include "port_resource.h"
#include "port_broker.h"
#include "hs_phy.h"
#include "cdc_log.h"
#include "descriptors.h"
#include "p4_usb_platform.h"
#include "firmware_gate.h"
#include "../sdmmc/host.h"
#include "../kernel/console_tap.h"
#if !defined(P4_USB_PORT_KERNEL)
#include LC_LIBDEFS_FILE
#endif

#if !defined(P4_USB_VID) || !defined(P4_USB_PID) || \
    P4_USB_VID < 1 || P4_USB_VID > 65534 || P4_USB_PID < 1 || P4_USB_PID > 65534
#error Explicit authorized P4_USB_VID and P4_USB_PID are required
#endif
#if !defined(P4_USB_PORT_KERNEL)
struct ExecBase *SysBase;
#endif
struct Device *TimerBase;
static struct SignalSemaphore lock;
static struct p4_port_broker broker;
static struct Task *owner;
static struct P4SDMMCHost *clock_host;
static struct P4ConsoleTapResource *tap;
static struct p4_cdc_log logs;
static uint32_t tap_token;
static struct timerequest *timer;
static p4_usb_hs_phy_t phy;
static uint32_t state = P4_USB_PORT_STARTING;
static uint32_t call_start, call_polls;
static bool polling, lock_held, controller_touched, phy_active;
static bool invalidate_pending;
static uint32_t last_client_ms;
static struct MsgPort diagnostic_lease;
static jmp_buf failure;
static const char *fault_reason;
static struct P4USBSnapshot observation;
static uint32_t dcd_resets,reset_size,enum_size,rx_irqs,setup_rx,setup_done;

/* Owner-only trace points in the actual DCD, never an extra FIFO pop.
 * Keep the immediate post-reset read distinct from a later GUI snapshot. */
void p4_usb_dcd_trace(enum p4_usb_trace_point point,uint32_t value)
{
    p4_usb_owner_check();
    switch(point) {
    case P4_USB_TRACE_RESET: ++dcd_resets;reset_size=value;break;
    case P4_USB_TRACE_ENUM: enum_size=value;break;
    case P4_USB_TRACE_RX_IRQ: ++rx_irqs;break;
    case P4_USB_TRACE_SETUP_RX: ++setup_rx;break;
    case P4_USB_TRACE_SETUP_DONE: ++setup_done;break;
    }
}

static bool try_lock(void) { return AttemptSemaphore(&lock) != 0; }
static void unlock(void) { ReleaseSemaphore(&lock); }
static uint32_t port_state(void) { return __atomic_load_n(&state, __ATOMIC_ACQUIRE); }
static void set_state(uint32_t s) { __atomic_store_n(&state,s,__ATOMIC_RELEASE); }

static bool session(uint32_t token)
{
    return port_state()==P4_USB_PORT_READY &&
        !__atomic_load_n(&invalidate_pending,__ATOMIC_ACQUIRE) && broker.claimed && broker.open &&
        broker.owner_key==(uintptr_t)FindTask(NULL) && broker.active_generation==token;
}
static uint32_t port_claim(void)
{
    uint32_t token=0;
    if(port_state()!=P4_USB_PORT_READY || !try_lock())return 0;
    if(!__atomic_load_n(&invalidate_pending,__ATOMIC_ACQUIRE) &&
       p4_port_broker_claim(&broker,FindTask(NULL),&token)==P4_PORT_BROKER_OK) {
        (void)p4_port_broker_service_open(&broker,FindTask(NULL),token);
        last_client_ms=p4_usb_millis();
    }
    unlock();return token;
}
static int port_valid(uint32_t token)
{
    /* Lock contention is not stale-session proof: -1 means retry. */
    if(!try_lock())return -1;
    int valid=session(token);
    if(valid)last_client_ms=p4_usb_millis();
    unlock();return valid;
}
static int port_release(uint32_t token)
{
    if(!try_lock())return -1;
    int valid=session(token);
    if(valid) {
        (void)p4_port_broker_release(&broker,FindTask(NULL),token);
        __atomic_store_n(&invalidate_pending,true,__ATOMIC_RELEASE);
    }
    unlock();return valid;
}
static uint32_t port_read(uint32_t token,void *bytes,uint32_t capacity)
{
    size_t n=0;
    if(!bytes || capacity>P4_USB_PORT_CHUNK || !try_lock())return 0;
    if(session(token)) {
        (void)p4_port_broker_service_read(&broker,FindTask(NULL),token,bytes,capacity,&n);
        last_client_ms=p4_usb_millis();
    }
    unlock();return (uint32_t)n;
}
static uint32_t port_write(uint32_t token,const void *bytes,uint32_t length)
{
    size_t n=0;
    if(!bytes || length>P4_USB_PORT_CHUNK || !try_lock())return 0;
    if(session(token)) {
        (void)p4_port_broker_service_write(&broker,FindTask(NULL),token,bytes,length,&n);
        last_client_ms=p4_usb_millis();
    }
    unlock();return (uint32_t)n;
}

static bool rcc_lock(void *ctx) { (void)ctx;return clock_host->acquire(clock_host,0); }
static void rcc_unlock(void *ctx) { (void)ctx;clock_host->release(clock_host); }
static int port_snapshot(struct P4USBSnapshot *out,uint32_t bytes)
{
    if(!out || bytes!=sizeof(*out))return 0;
    if(!try_lock())return -1;
    *out=observation;
    out->version=P4_USB_SNAPSHOT_VERSION;out->size=sizeof(*out);
    out->state=port_state();
    unlock();return 1;
}
void p4_usb_owner_check(void)
{
    if(FindTask(NULL)!=owner)p4_usb_fault("non-owner controller access");
}
void p4_usb_fault(const char *reason)
{
    fault_reason=reason;longjmp(failure,1);
}
uint32_t p4_usb_millis(void)
{
    struct timeval tv;GetSysTime(&tv);
    return (uint32_t)tv.tv_secs*1000u+(uint32_t)tv.tv_micro/1000u;
}
void p4_usb_guard(void)
{
    p4_usb_owner_check();
    if(++call_polls>100000u || (uint32_t)(p4_usb_millis()-call_start)>=100u)
        p4_usb_fault("controller polling budget exceeded");
}
void p4_usb_delay(uint32_t ms)
{
    p4_usb_owner_check();
    if(ms>100u)p4_usb_fault("oversized controller delay");
    timer->tr_node.io_Command=TR_ADDREQUEST;
    timer->tr_time.tv_secs=0;timer->tr_time.tv_micro=ms*1000u;
    if(DoIO((struct IORequest *)timer))p4_usb_fault("timer failure");
}
void p4_usb_poll_enable(bool on) { p4_usb_owner_check();polling=on; }
static void begin_call(void) { call_start=p4_usb_millis();call_polls=0; }
static uint32_t mmio(uintptr_t a) { return *(volatile uint32_t *)a; }
static void write_mmio(uintptr_t a,uint32_t v)
{
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    *(volatile uint32_t *)a=v;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}
/* Only the owning task samples known, non-destructive registers. Do not read
 * GRXSTSP/FIFOs or clear W1C status. No native pointer/log payload is exposed. */
static void observe(uint32_t phase,uint32_t loops,uint32_t irq_seen,
                    uint32_t resets,uint32_t setups,uint32_t mounts)
{
    struct P4USBSnapshot next={0};
    next.phase=phase;next.loops=loops;next.irq_seen=irq_seen;
    next.polling=polling;
    next.dcd_resets=dcd_resets;next.reset_size=reset_size;next.enum_size=enum_size;
    next.rx_irqs=rx_irqs;next.setup_rx=setup_rx;next.setup_done=setup_done;
    next.resets=resets;next.setups=setups;next.mounts=mounts;
    next.hp_clock=mmio(0x500e6018u);next.lp_control=mmio(0x50111048u);
    next.suspendm=mmio(0x500e515cu);
    if(controller_touched) {
        next.id=mmio(0x50000040u);next.hwcfg2=mmio(0x50000048u);
        next.gotgctl=mmio(0x50000000u);next.gahbcfg=mmio(0x50000008u);
        next.gusbcfg=mmio(0x5000000cu);next.gintsts=mmio(0x50000014u);
        next.gintmsk=mmio(0x50000018u);next.dcfg=mmio(0x50000800u);
        next.dctl=mmio(0x50000804u);next.dsts=mmio(0x50000808u);
        next.pcgcctl=mmio(0x50000e00u);next.utmi_fc06=mmio(0x5009c018u);
        /* Read-only configuration/status. Never sample GRXSTSP (FIFO pop)
         * or acknowledge the W1C endpoint interrupt register here. */
        next.hwcfg4=mmio(0x50000050u);next.doepctl0=mmio(0x50000b00u);
        next.doeptsiz0=mmio(0x50000b10u);next.doepint0=mmio(0x50000b08u);
    }
    if(try_lock()){observation=next;unlock();}
}
static int log_pop(void *ctx,struct p4_console_record *record)
{ (void)ctx;return tap->pop(owner,tap_token,record); }
static uint32_t log_dropped(void *ctx)
{ (void)ctx;return tap->dropped(owner,tap_token); }
static bool cdc_connected(void *ctx) { (void)ctx;return tud_cdc_connected(); }
static size_t cdc_room(void *ctx) { (void)ctx;return tud_cdc_write_available(); }
static size_t cdc_write(void *ctx,const uint8_t *p,size_t n)
{ (void)ctx;return tud_cdc_write(p,(uint32_t)n); }
static void cdc_flush(void *ctx) { (void)ctx;(void)tud_cdc_write_flush(); }

/* Bus reset callbacks never wait on a client. Admission remains shut until
 * the owner has invalidated the queues under the actual shared semaphore. */
static void bus_changed(void)
{
    p4_usb_owner_check();
    __atomic_store_n(&invalidate_pending,true,__ATOMIC_RELEASE);
    p4_cdc_log_reset(&logs);
}
static uint32_t reset_events,setup_events,mount_events;
void tud_event_hook_cb(uint8_t port,uint32_t event,bool isr)
{
    (void)port;(void)isr;
    /* START is optional (DWC2 reports END only). Count completed resets,
     * and invalidate at either edge so END-only DCDs cannot retain a lease. */
    if(event==DCD_EVENT_BUS_RESET_END)++reset_events;
    if(event==DCD_EVENT_SETUP_RECEIVED)++setup_events;
    if(event==DCD_EVENT_BUS_RESET_START || event==DCD_EVENT_BUS_RESET_END ||
       event==DCD_EVENT_UNPLUGGED)bus_changed();
}
void tud_mount_cb(void) { ++mount_events;bus_changed(); }
void tud_umount_cb(void) { bus_changed(); }

/* Separate, read-only firmware capability request. No remote grant can
 * authorize flash writes. Backend absence is reported, never faked success.
 * Device vendor IN request0x50, wValue/wIndex0, exactly16 bytes, little-endian:
 * "P4UP", protocol1, status1 (NO_BACKEND), writable0, reserved0.
 * All other vendor control requests, including mutation, stall.
 */
bool tud_vendor_control_xfer_cb(uint8_t rhport,uint8_t stage,
                               tusb_control_request_t const *request)
{
    p4_usb_owner_check();
    const uint8_t *capability=p4_usb_firmware_query(request->bmRequestType,
        request->bRequest,request->wValue,request->wIndex,request->wLength);
    if(!capability)return false;
    if(stage!=CONTROL_STAGE_SETUP)return true;
    return tud_control_xfer(rhport,request,(void *)capability,P4_USB_FW_REPLY_SIZE);
}

static void bulk_step(void)
{
    uint8_t bytes[P4_USB_PORT_CHUNK];size_t n=0;
    if(!try_lock())return;
    lock_held=true;
    if(broker.claimed && (uint32_t)(p4_usb_millis()-last_client_ms)>=2000u)
        __atomic_store_n(&invalidate_pending,true,__ATOMIC_RELEASE);
    if(__atomic_load_n(&invalidate_pending,__ATOMIC_ACQUIRE)) {
        if(broker.claimed)
            (void)p4_port_broker_usb_reset(&broker,(void *)broker.owner_key,broker.active_generation);
        /* Only USB owner touches endpoints. In-flight bytes already delivered
         * cannot be retracted; queued application data is never replayed. */
        tud_vendor_read_flush();(void)tud_vendor_write_clear();
        __atomic_store_n(&invalidate_pending,false,__ATOMIC_RELEASE);
    }
    if(tud_mounted()) {
        uint32_t available=tud_vendor_available();
        if(available>sizeof(bytes))available=sizeof(bytes);
        if(broker.claimed && broker.open) {
            uint32_t room=P4_PORT_BROKER_RING_CAPACITY-broker.rx_used;
            if(available>room)available=room;
        }
        if(available) {
            uint32_t got=tud_vendor_read(bytes,available);
            (void)p4_port_broker_usb_receive(&broker,(void *)broker.owner_key,
                broker.active_generation,bytes,got,&n);
        }
        uint32_t room=tud_vendor_write_available();
        if(room>sizeof(bytes))room=sizeof(bytes);
        if(broker.claimed && broker.open && room) {
            (void)p4_port_broker_usb_peek(&broker,(void *)broker.owner_key,
                broker.active_generation,bytes,room,&n);
            if(n) {
                n=tud_vendor_write(bytes,(uint32_t)n);
                (void)p4_port_broker_usb_consume(&broker,(void *)broker.owner_key,
                    broker.active_generation,n);
                (void)tud_vendor_write_flush();
            }
        }
    }
    lock_held=false;unlock();
}

static void port_task(void)
{
    /* These survive the bounded controller longjmp fault boundary. */
    volatile uint32_t loops=0,irq_seen=0,last_observed=0,phase=1;
    struct MsgPort *timer_port=NULL;
    const p4_usb_hs_phy_platform_t platform={NULL,rcc_lock,rcc_unlock};
    owner=FindTask(NULL);
    observe(phase,0,0,0,0,0);
    clock_host=OpenResource(P4_SDMMC_HOST_NAME);
    tap=OpenResource(P4_CONSOLE_TAP_NAME);
    if(!clock_host || clock_host->abi!=P4_SDMMC_HOST_ABI ||
       clock_host->size<sizeof(*clock_host) || !clock_host->acquire || !clock_host->release ||
       !tap || tap->abi!=P4_CONSOLE_TAP_ABI || tap->size<sizeof(*tap) ||
       !tap->claim || !tap->release || !tap->pop || !tap->dropped)goto done;
    timer_port=CreateMsgPort();
    if(timer_port)timer=(struct timerequest *)CreateIORequest(timer_port,sizeof(*timer));
    if(!timer || OpenDevice("timer.device",UNIT_MICROHZ,(struct IORequest *)timer,0))goto done;
    TimerBase=timer->tr_node.io_Device;
    if(!(tap_token=tap->claim(owner)))goto done;
    const struct p4_cdc_source source={NULL,log_pop,log_dropped};
    const struct p4_cdc_sink sink={NULL,cdc_connected,cdc_room,cdc_write,cdc_flush};
    if(!p4_cdc_log_init(&logs,&source,&sink))goto done;
    if(!p4_usb_set_identity(P4_USB_VID,P4_USB_PID))goto done;
    /* Refuse existing routed/diagnostic ownership; never steal IRQs. */
    if(!rcc_lock(NULL))goto done;
    if(FindPort("AROS.P4.USBEcho.owner")){rcc_unlock(NULL);goto done;}
    diagnostic_lease.mp_Node.ln_Name="AROS.P4.USBEcho.owner";
    diagnostic_lease.mp_Flags=PA_IGNORE;
    NEWLIST(&diagnostic_lease.mp_MsgList);
    AddPort(&diagnostic_lease);rcc_unlock(NULL);
    for(unsigned hart=0;hart<2;++hart)
        for(unsigned src=94;src<=95;++src)
            if(mmio(0x500d6000u+hart*0x800u+src*4u)&63u)goto done;
    if(setjmp(failure)) {
        if(lock_held){lock_held=false;unlock();}
        bug("[USBPort] fault: %s; remote admission closed\n",fault_reason);
        goto stop;
    }
    phase=2;observe(phase,0,0,0,0,0);
    p4_usb_hs_phy_result_t setup=p4_usb_hs_phy_setup(&phy,&platform);
    if(setup!=P4_USB_HS_PHY_OK) {
        phy_active=setup==P4_USB_HS_PHY_RESTORE_ERROR;goto stop;
    }
    phy_active=true;controller_touched=true;phase=3;
    observe(phase,0,0,0,0,0);begin_call();
    const tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};
    if(!tusb_init(1,&init))p4_usb_fault("TinyUSB init failed");
    set_state(P4_USB_PORT_READY);
    phase=4;observe(phase,0,0,0,0,0);
    bug("[USBPort] HS device owner ready; CDC logs; remote closed; firmware writes unavailable\n");
    for(;;) {
        begin_call();
        ++loops;irq_seen|=mmio(0x50000014u);
        if(polling)dcd_int_handler(1);
        tud_task_ext(0,false);bulk_step();
        uint8_t discard[64];uint32_t n=tud_cdc_available();
        if(n>sizeof(discard))n=sizeof(discard);
        if(n)(void)tud_cdc_read(discard,n);
        if(!p4_cdc_log_step(&logs,256))p4_usb_fault("CDC log stream failed");
        uint32_t now=p4_usb_millis();
        if((uint32_t)(now-last_observed)>=250u) {
            observe(phase,loops,irq_seen,reset_events,setup_events,mount_events);
            last_observed=now;
        }
        p4_usb_delay(2);
    }
stop:
    set_state(P4_USB_PORT_FAULTED);
    observe(9,loops,irq_seen,reset_events,setup_events,mount_events);
    if(controller_touched) {
        write_mmio(0x50000018u,0);
        write_mmio(0x50000008u,mmio(0x50000008u)&~1u);
        write_mmio(0x50000804u,mmio(0x50000804u)|2u);
    }
    if(phy_active) {
        /* One bounded restore attempt. Permanent module and snapshot survive
         * failure; no later owner/restart is admitted before reboot. */
        /* hs_phy_deinit has fixed MMIO/readback operations and a try-lock;
         * it never calls TinyUSB, delay, guard or longjmp hooks. */
        if(p4_usb_hs_phy_deinit(&phy)!=P4_USB_HS_PHY_OK)
            bug("[USBPort] PHY restore failed; quarantined until reboot\n");
    }
done:
    set_state(P4_USB_PORT_FAULTED);
    /* Preserve pre-cleanup controller evidence; early failures have no MMIO. */
    if(try_lock()){observation.state=P4_USB_PORT_FAULTED;unlock();}
    if(tap && tap_token)tap->release(owner,tap_token);
    if(TimerBase)CloseDevice((struct IORequest *)timer);
    if(timer)DeleteIORequest((struct IORequest *)timer);
    if(timer_port)DeleteMsgPort(timer_port);
    bug("[USBPort] owner stopped; Debug/FS recovery unchanged\n");
}

AROS_UFH3(static APTR, port_afterdos,
    AROS_UFPA(void *,dummy,D0),AROS_UFPA(BPTR,seglist,A0),
    AROS_UFPA(struct ExecBase *,sysbase,A6))
{
    AROS_USERFUNC_INIT
    (void)dummy;(void)seglist;(void)sysbase;
    if(!NewCreateTask(TASKTAG_NAME,(IPTR)"P4 HS USB owner",TASKTAG_PRI,0,
        TASKTAG_STACKSIZE,16384,TASKTAG_PC,(IPTR)port_task,TAG_DONE)) {
        set_state(P4_USB_PORT_FAULTED);bug("[USBPort] owner task unavailable\n");
    }
    return NULL;
    AROS_USERFUNC_EXIT
}
const struct Resident p4_usb_afterdos_resident={
    RTC_MATCHWORD,(struct Resident *)&p4_usb_afterdos_resident,
    (APTR)((const char *)&p4_usb_afterdos_resident+sizeof(struct Resident)),
    RTF_AFTERDOS,1,NT_TASK,-127,"esp32p4 HS owner","esp32p4 HS owner 1.0",&port_afterdos
};
static int port_init(struct P4USBPort *base)
{
    SysBase=base->sysbase;
    InitSemaphore(&lock);p4_port_broker_init(&broker);
    base->abi=P4_USB_PORT_ABI;base->size=sizeof(*base);
    base->state=port_state;base->claim=port_claim;base->valid=port_valid;
    base->release=port_release;base->read=port_read;base->write=port_write;
    base->snapshot=port_snapshot;
    return TRUE;
}
#if !defined(P4_USB_PORT_KERNEL)
ADD2INITLIB(port_init,0)
#else
static struct P4USBPort kernel_port;
AROS_UFH3(static APTR, port_core_init,
    AROS_UFPA(void *,dummy,D0),AROS_UFPA(BPTR,seglist,A0),
    AROS_UFPA(struct ExecBase *,sysbase,A6))
{
    AROS_USERFUNC_INIT
    (void)dummy;(void)seglist;
    kernel_port.sysbase=sysbase;
    port_init(&kernel_port);
    kernel_port.library.lib_Node.ln_Name=(STRPTR)P4_USB_PORT_NAME;
    kernel_port.library.lib_Node.ln_Type=NT_RESOURCE;
    kernel_port.library.lib_Version=1;
    AddResource(&kernel_port);
    return &kernel_port;
    AROS_USERFUNC_EXIT
}
const struct Resident p4_usb_port_resident={
    RTC_MATCHWORD,(struct Resident *)&p4_usb_port_resident,
    (APTR)((const char *)&p4_usb_port_resident+sizeof(struct Resident)),
    RTF_COLDSTART,1,NT_RESOURCE,7,P4_USB_PORT_NAME,"esp32p4usb.resource 1.0",&port_core_init
};
#endif
