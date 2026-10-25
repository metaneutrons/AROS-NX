/* Source-bound host orchestration tests for the opt-in USB echo command.
 * Build main.c separately with P4_USB_ECHO_HOST_TEST and -Dmain=
 * p4_usb_echo_app_main, then link it with echo.c and this fixture.
 */
#include "main_test_platform.h"
#include "descriptors.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern int p4_usb_echo_app_main(void);
extern struct Device *TimerBase;

enum event_kind
{
    EV_READ_ARGS,
    EV_OPEN_RESOURCE,
    EV_ALLOC_VEC,
    EV_FREE_VEC,
    EV_NEW_LIST,
    EV_CREATE_PORT,
    EV_FIND_PORT,
    EV_ADD_PORT,
    EV_REMOVE_PORT,
    EV_DELETE_PORT,
    EV_FREE_ARGS,
    EV_HOST_ACQUIRE,
    EV_HOST_RELEASE,
    EV_FIND_TASK,
    EV_CREATE_IO,
    EV_OPEN_TIMER,
    EV_CLOSE_TIMER,
    EV_DELETE_IO,
    EV_TIMER_IO,
    EV_GET_TIME,
    EV_SET_SIGNAL,
    EV_IDENTITY,
    EV_PHY_SETUP,
    EV_PHY_DEINIT,
    EV_TUSB_INIT,
    EV_DCD_POLL,
    EV_TUD_TASK,
    EV_MOUNTED,
    EV_VENDOR_AVAILABLE,
    EV_VENDOR_READ,
    EV_VENDOR_WRITE_AVAILABLE,
    EV_VENDOR_WRITE,
    EV_VENDOR_FLUSH,
    EV_MMIO_READ,
    EV_MMIO_WRITE
};

struct event
{
    enum event_kind kind;
    uintptr_t first;
    uintptr_t second;
};

#define MAX_EVENTS 32768u
#define MAX_IO_BYTES 64u
#define MAX_IO_CALLS 16u

struct scenario
{
    bool read_args_ok;
    bool enable;
    LONG vid;
    LONG pid;
    bool seconds_present;
    LONG seconds;
    bool steady_clocks;
    bool identity_ok;
    bool resource_present;
    ULONG host_abi;
    ULONG host_size;
    bool host_has_acquire;
    bool host_has_release;
    unsigned host_busy_on_call;
    unsigned acquire_calls;
    unsigned release_calls;
    unsigned lock_depth;
    bool owner_already_present;
    unsigned create_port_fail_on_call;
    unsigned create_port_calls;
    bool create_io_fails;
    LONG timer_open_result;
    unsigned close_timer_calls;
    unsigned delete_io_calls;
    unsigned delete_port_calls;
    unsigned do_io_fail_on_call;
    unsigned do_io_calls;
    uint32_t now_ms;
    unsigned set_signal_calls;
    unsigned ctrl_c_on_call;
    uintptr_t routed_address;
    uint32_t routed_value;
    uint32_t dwc_gintmsk;
    uint32_t dwc_ahbcfg;
    uint32_t dwc_dctl;
    unsigned identity_calls;
    uint16_t identity_vid;
    uint16_t identity_pid;
    p4_usb_hs_phy_result_t phy_setup_result;
    p4_usb_hs_phy_result_t phy_deinit_results[16];
    size_t phy_deinit_result_count;
    size_t phy_deinit_result_offset;
    unsigned phy_setup_calls;
    unsigned phy_deinit_calls;
    p4_usb_hs_phy_t *phy_state_seen;
    bool alloc_vec_fails;
    size_t alloc_vec_calls;
    size_t free_vec_calls;
    size_t new_list_calls;
    size_t owner_allocation_size;
    ULONG owner_allocation_flags;
    bool tusb_init_ok;
    unsigned tusb_init_calls;
    unsigned dcd_calls;
    unsigned tud_task_calls;
    bool expire_guard_in_dcd;
    bool guard_expired;
    bool mounted;
    uint8_t input[MAX_IO_BYTES];
    size_t input_length;
    size_t input_offset;
    uint32_t write_available[MAX_IO_CALLS];
    size_t write_available_count;
    size_t write_available_offset;
    uint32_t write_return[MAX_IO_CALLS];
    size_t write_return_count;
    size_t write_return_offset;
    uint32_t write_offered[MAX_IO_CALLS];
    uint32_t write_accepted[MAX_IO_CALLS];
    size_t write_calls;
    uint8_t output[MAX_IO_BYTES];
    size_t output_length;
    unsigned read_calls;
    unsigned flush_calls;
    char last_message[256];
};

static struct scenario state;
static struct event events[MAX_EVENTS];
static size_t event_count;
static struct P4SDMMCHost host;
static struct Task current_task;
static struct Device timer_device;
static struct MsgPort ports[2];
static struct MsgPort foreign_owner;
static struct MsgPort *registered_owner;
static struct timerequest timer_request;
static struct RDArgs parsed_args;
struct observed_owner_record
{
    struct MsgPort port;
    char name[32];
    uint32_t signature;
    p4_usb_hs_phy_t saved_phy;
};
_Alignas(max_align_t) static unsigned char owner_storage[512];
static LONG args_vid;
static LONG args_pid;
static LONG args_seconds;
static unsigned args_enable;
static unsigned args_steady;

static void fail(const char *expression, int line)
{
    fprintf(stderr, "test_main:%d: check failed: %s\n", line, expression);
    exit(EXIT_FAILURE);
}

#define CHECK(expression) do { if (!(expression)) fail(#expression, __LINE__); } while (0)

static void record(enum event_kind kind, uintptr_t first, uintptr_t second)
{
    if (event_count >= MAX_EVENTS)
        fail("event log capacity", __LINE__);
    events[event_count++] = (struct event){ kind, first, second };
}

static size_t count(enum event_kind kind)
{
    size_t i;
    size_t result = 0;

    for (i = 0; i < event_count; ++i)
        if (events[i].kind == kind)
            ++result;
    return result;
}

static size_t first_event(enum event_kind kind)
{
    size_t i;

    for (i = 0; i < event_count; ++i)
        if (events[i].kind == kind)
            return i;
    return MAX_EVENTS;
}

static size_t nth_event(enum event_kind kind, size_t occurrence)
{
    size_t i;

    for (i = 0; i < event_count; ++i)
        if (events[i].kind == kind && occurrence-- == 0)
            return i;
    return MAX_EVENTS;
}

static void host_sync(void);

static BOOL host_acquire(struct P4SDMMCHost *candidate, ULONG timeout_us)
{
    unsigned call = ++state.acquire_calls;
    bool success = call != state.host_busy_on_call;

    record(EV_HOST_ACQUIRE, (uintptr_t)candidate, timeout_us);
    if (candidate != &host || timeout_us != 0)
        fail("shared host acquire target and nonblocking timeout", __LINE__);
    if (success)
        ++state.lock_depth;
    return success;
}

static void host_release(struct P4SDMMCHost *candidate)
{
    record(EV_HOST_RELEASE, (uintptr_t)candidate, state.lock_depth);
    if (candidate != &host || state.lock_depth == 0)
        fail("balanced shared host release", __LINE__);
    --state.lock_depth;
    ++state.release_calls;
}

static void reset_scenario(void)
{
    memset(&state, 0, sizeof(state));
    memset(events, 0, sizeof(events));
    memset(ports, 0, sizeof(ports));
    memset(&foreign_owner, 0, sizeof(foreign_owner));
    memset(owner_storage, 0, sizeof(owner_storage));
    memset(&timer_request, 0, sizeof(timer_request));
    memset(&timer_device, 0, sizeof(timer_device));
    registered_owner = NULL;
    event_count = 0;

    state.read_args_ok = true;
    state.enable = true;
    state.vid = 0x1234;
    state.pid = 0x5678;
    state.seconds_present = true;
    state.seconds = 1;
    state.steady_clocks = true;
    state.identity_ok = true;
    state.resource_present = true;
    state.host_abi = P4_SDMMC_HOST_ABI;
    state.host_size = (ULONG)sizeof(host);
    state.host_has_acquire = true;
    state.host_has_release = true;
    state.phy_setup_result = P4_USB_HS_PHY_OK;
    state.phy_deinit_results[0] = P4_USB_HS_PHY_OK;
    state.phy_deinit_result_count = 1;
    state.tusb_init_ok = true;
    state.mounted = true;
    state.dwc_gintmsk = 0x12345678u;
    state.dwc_ahbcfg = 0xa5u;
    state.dwc_dctl = 0x10u;

    args_vid = state.vid;
    args_pid = state.pid;
    args_seconds = state.seconds;
    args_enable = 1;
    args_steady = 1;
    host_sync();
}

static void host_sync(void)
{
    host.abi = state.host_abi;
    host.size = state.host_size;
    host.acquire = state.host_has_acquire ? host_acquire : NULL;
    host.release = state.host_has_release ? host_release : NULL;
}

static void set_deinit_results(const p4_usb_hs_phy_result_t *results,
                               size_t result_count)
{
    CHECK(result_count <= sizeof(state.phy_deinit_results) /
                          sizeof(state.phy_deinit_results[0]));
    memcpy(state.phy_deinit_results, results,
           result_count * sizeof(state.phy_deinit_results[0]));
    state.phy_deinit_result_count = result_count;
    state.phy_deinit_result_offset = 0;
}

static int run_app(void)
{
    args_vid = state.vid;
    args_pid = state.pid;
    args_seconds = state.seconds;
    host_sync();
    return p4_usb_echo_app_main();
}

static void assert_no_phy_or_writes(void)
{
    CHECK(state.phy_setup_calls == 0);
    CHECK(state.phy_deinit_calls == 0);
    CHECK(count(EV_PHY_SETUP) == 0);
    CHECK(count(EV_PHY_DEINIT) == 0);
    CHECK(count(EV_MMIO_WRITE) == 0);
}

static void reject_before_phy(void)
{
    CHECK(run_app() == RETURN_FAIL);
    assert_no_phy_or_writes();
}

struct RDArgs *ReadArgs(CONST_STRPTR template, SIPTR *args,
                        struct RDArgs *existing)
{
    record(EV_READ_ARGS, (uintptr_t)template, (uintptr_t)existing);
    CHECK(strcmp(template,
        "ENABLE/S,VID/N/A,PID/N/A,SECONDS/N,STEADYCLOCKS/S") == 0);
    if (!state.read_args_ok)
        return NULL;

    args[0] = state.enable ? (SIPTR)&args_enable : 0;
    args[1] = (SIPTR)&args_vid;
    args[2] = (SIPTR)&args_pid;
    args[3] = state.seconds_present ? (SIPTR)&args_seconds : 0;
    args[4] = state.steady_clocks ? (SIPTR)&args_steady : 0;
    return &parsed_args;
}

LONG IoErr(void)
{
    return 123;
}

void PrintFault(LONG error, CONST_STRPTR header)
{
    (void)snprintf(state.last_message, sizeof(state.last_message),
                   "fault %ld: %s", (long)error, header);
}

void FreeArgs(struct RDArgs *args)
{
    CHECK(args == &parsed_args);
    record(EV_FREE_ARGS, (uintptr_t)args, 0);
}

void *OpenResource(CONST_STRPTR name)
{
    record(EV_OPEN_RESOURCE, (uintptr_t)name, 0);
    CHECK(strcmp(name, P4_SDMMC_HOST_NAME) == 0);
    return state.resource_present ? &host : NULL;
}

APTR AllocVec(IPTR size, ULONG requirements)
{
    record(EV_ALLOC_VEC, (uintptr_t)size, requirements);
    ++state.alloc_vec_calls;
    state.owner_allocation_size = (size_t)size;
    state.owner_allocation_flags = requirements;
    CHECK(size > 0 && (size_t)size <= sizeof(owner_storage));
    CHECK(requirements == (MEMF_PUBLIC | MEMF_CLEAR));
    if (state.alloc_vec_fails)
        return NULL;
    memset(owner_storage, 0, (size_t)size);
    return owner_storage;
}

void FreeVec(APTR memory)
{
    record(EV_FREE_VEC, (uintptr_t)memory, 0);
    CHECK(memory == owner_storage);
    ++state.free_vec_calls;
}

void NewList(struct List *list)
{
    record(EV_NEW_LIST, (uintptr_t)list, 0);
    ++state.new_list_calls;
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
    list->lh_Type = 0;
    list->l_pad = 0;
}

struct MsgPort *CreateMsgPort(void)
{
    unsigned call = ++state.create_port_calls;

    record(EV_CREATE_PORT, call, 0);
    if (call == state.create_port_fail_on_call)
        return NULL;
    CHECK(call <= 2);
    return &ports[call - 1];
}

struct MsgPort *FindPort(CONST_STRPTR name)
{
    record(EV_FIND_PORT, (uintptr_t)name, 0);
    CHECK(strcmp(name, "AROS.P4.USBEcho.owner") == 0);
    if (state.owner_already_present)
        return &foreign_owner;
    return registered_owner;
}

void AddPort(struct MsgPort *port)
{
    record(EV_ADD_PORT, (uintptr_t)port, (uintptr_t)port->mp_Node.ln_Name);
    CHECK(port == &((struct observed_owner_record *)owner_storage)->port);
    CHECK(port->mp_Flags == PA_IGNORE && port->mp_SigTask == NULL);
    CHECK(port->mp_MsgList.lh_Head ==
          (struct Node *)&port->mp_MsgList.lh_Tail);
    CHECK(port->mp_MsgList.lh_Tail == NULL);
    CHECK(port->mp_MsgList.lh_TailPred ==
          (struct Node *)&port->mp_MsgList.lh_Head);
    CHECK(port->mp_Node.ln_Name ==
          ((struct observed_owner_record *)owner_storage)->name);
    CHECK(strcmp(port->mp_Node.ln_Name, "AROS.P4.USBEcho.owner") == 0);
    registered_owner = port;
}

void RemPort(struct MsgPort *port)
{
    record(EV_REMOVE_PORT, (uintptr_t)port, 0);
    CHECK(port == registered_owner);
    registered_owner = NULL;
}

void DeleteMsgPort(struct MsgPort *port)
{
    record(EV_DELETE_PORT, (uintptr_t)port, 0);
    ++state.delete_port_calls;
}

struct Task *FindTask(CONST_STRPTR name)
{
    record(EV_FIND_TASK, (uintptr_t)name, 0);
    CHECK(name == NULL);
    return &current_task;
}

struct IORequest *CreateIORequest(struct MsgPort *port, ULONG size)
{
    record(EV_CREATE_IO, (uintptr_t)port, size);
    CHECK(size == sizeof(timer_request));
    if (state.create_io_fails)
        return NULL;
    return &timer_request.tr_node;
}

LONG OpenDevice(CONST_STRPTR name, ULONG unit, struct IORequest *request,
                ULONG flags)
{
    record(EV_OPEN_TIMER, (uintptr_t)request, ((uintptr_t)unit << 32) | flags);
    CHECK(strcmp(name, "timer.device") == 0);
    CHECK(unit == UNIT_MICROHZ && flags == 0);
    if (state.timer_open_result == 0)
        request->io_Device = &timer_device;
    return state.timer_open_result;
}

void CloseDevice(struct IORequest *request)
{
    record(EV_CLOSE_TIMER, (uintptr_t)request, 0);
    CHECK(request == &timer_request.tr_node);
    ++state.close_timer_calls;
}

void DeleteIORequest(struct IORequest *request)
{
    record(EV_DELETE_IO, (uintptr_t)request, 0);
    CHECK(request == &timer_request.tr_node);
    ++state.delete_io_calls;
}

void GetSysTime(struct timeval *time)
{
    record(EV_GET_TIME, state.now_ms, 0);
    time->tv_secs = state.now_ms / 1000u;
    time->tv_micro = (state.now_ms % 1000u) * 1000u;
}

LONG DoIO(struct IORequest *request)
{
    struct timerequest *timer = (struct timerequest *)request;
    unsigned call = ++state.do_io_calls;
    uint32_t milliseconds;

    record(EV_TIMER_IO, (uintptr_t)request, (uintptr_t)timer->tr_time.tv_micro);
    CHECK(request == &timer_request.tr_node);
    CHECK(request->io_Command == TR_ADDREQUEST);
    if (call == state.do_io_fail_on_call)
        return 1;
    milliseconds = timer->tr_time.tv_secs * 1000u +
                   timer->tr_time.tv_micro / 1000u;
    state.now_ms += milliseconds;
    return 0;
}

ULONG SetSignal(ULONG new_signals, ULONG signal_mask)
{
    unsigned call = ++state.set_signal_calls;

    record(EV_SET_SIGNAL, new_signals, signal_mask);
    CHECK(new_signals == 0 && signal_mask == SIGBREAKF_CTRL_C);
    return call == state.ctrl_c_on_call ? SIGBREAKF_CTRL_C : 0;
}

int p4_usb_set_identity(uint16_t vid, uint16_t pid)
{
    record(EV_IDENTITY, vid, pid);
    ++state.identity_calls;
    state.identity_vid = vid;
    state.identity_pid = pid;
    return state.identity_ok;
}

p4_usb_hs_phy_result_t p4_usb_hs_phy_setup(
    p4_usb_hs_phy_t *phy, const p4_usb_hs_phy_platform_t *platform)
{
    record(EV_PHY_SETUP, (uintptr_t)phy, (uintptr_t)platform);
    ++state.phy_setup_calls;
    state.phy_state_seen = phy;
    CHECK(phy != NULL && platform != NULL);
    CHECK(platform->try_rcc_lock != NULL && platform->rcc_unlock != NULL);
    phy->lock_context = platform->context;
    phy->try_rcc_lock = platform->try_rcc_lock;
    phy->rcc_unlock = platform->rcc_unlock;
    phy->saved_hp_clock = 0x13579bdfu;
    phy->saved_lp_control = 0x2468ace0u;
    phy->saved_suspendm = 0x00200000u;
    phy->saved_utmi_fc06 = 0x9u;
    phy->snapshot_flags = 3u;
    phy->phase = 2u;
    if (state.phy_setup_result == P4_USB_HS_PHY_OK ||
        state.phy_setup_result == P4_USB_HS_PHY_RESTORE_ERROR)
    {
        CHECK(platform->try_rcc_lock(platform->context));
        platform->rcc_unlock(platform->context);
    }
    return state.phy_setup_result;
}

p4_usb_hs_phy_result_t p4_usb_hs_phy_deinit(p4_usb_hs_phy_t *phy)
{
    p4_usb_hs_phy_result_t result = P4_USB_HS_PHY_OK;

    if (state.phy_deinit_result_offset < state.phy_deinit_result_count)
        result = state.phy_deinit_results[state.phy_deinit_result_offset++];
    record(EV_PHY_DEINIT, (uintptr_t)phy, (uintptr_t)result);
    ++state.phy_deinit_calls;
    CHECK(phy != NULL && phy == state.phy_state_seen);
    if (result == P4_USB_HS_PHY_BUSY)
        return result;
    CHECK(phy->try_rcc_lock != NULL && phy->rcc_unlock != NULL);
    CHECK(phy->try_rcc_lock(phy->lock_context));
    phy->rcc_unlock(phy->lock_context);
    if (result == P4_USB_HS_PHY_OK)
    {
        phy->lock_context = NULL;
        phy->try_rcc_lock = NULL;
        phy->rcc_unlock = NULL;
        phy->snapshot_flags = 0;
        phy->phase = 0;
    }
    return result;
}

bool tusb_init(uint8_t rhport, const tusb_rhport_init_t *init)
{
    record(EV_TUSB_INIT, rhport,
           init ? ((uintptr_t)init->role << 32) | (uintptr_t)init->speed : 0);
    ++state.tusb_init_calls;
    CHECK(rhport == 1);
    CHECK(init != NULL && init->role == TUSB_ROLE_DEVICE &&
          init->speed == TUSB_SPEED_HIGH);
    if (state.tusb_init_ok)
        p4_usb_poll_enable(true);
    return state.tusb_init_ok;
}

void dcd_int_handler(uint8_t rhport)
{
    record(EV_DCD_POLL, rhport, 0);
    CHECK(rhport == 1);
    ++state.dcd_calls;
    if (state.expire_guard_in_dcd && !state.guard_expired)
    {
        state.now_ms += 100u;
        state.guard_expired = true;
    }
    p4_usb_guard();
}

void tud_task_ext(uint32_t timeout_ms, bool in_isr)
{
    record(EV_TUD_TASK, timeout_ms, in_isr);
    CHECK(timeout_ms == 0 && !in_isr);
    ++state.tud_task_calls;
    p4_usb_guard();
}

bool tud_mounted(void)
{
    record(EV_MOUNTED, state.mounted, 0);
    return state.mounted;
}

uint32_t tud_vendor_available(void)
{
    uint32_t available = (uint32_t)(state.input_length - state.input_offset);

    record(EV_VENDOR_AVAILABLE, available, 0);
    return available;
}

uint32_t tud_vendor_read(void *buffer, uint32_t length)
{
    size_t remaining = state.input_length - state.input_offset;
    size_t amount = length < remaining ? length : remaining;

    record(EV_VENDOR_READ, length, amount);
    ++state.read_calls;
    memcpy(buffer, state.input + state.input_offset, amount);
    state.input_offset += amount;
    return (uint32_t)amount;
}

uint32_t tud_vendor_write_available(void)
{
    uint32_t available = UINT32_MAX;

    if (state.write_available_offset < state.write_available_count)
        available = state.write_available[state.write_available_offset++];
    record(EV_VENDOR_WRITE_AVAILABLE, available, 0);
    return available;
}

uint32_t tud_vendor_write(const void *buffer, uint32_t length)
{
    uint32_t accepted = length;

    CHECK(state.write_calls < MAX_IO_CALLS);
    if (state.write_return_offset < state.write_return_count)
        accepted = state.write_return[state.write_return_offset++];
    if (accepted > length)
        accepted = length;
    record(EV_VENDOR_WRITE, length, accepted);
    state.write_offered[state.write_calls] = length;
    state.write_accepted[state.write_calls] = accepted;
    ++state.write_calls;
    CHECK(state.output_length + accepted <= sizeof(state.output));
    memcpy(state.output + state.output_length, buffer, accepted);
    state.output_length += accepted;
    return accepted;
}

void tud_vendor_write_flush(void)
{
    record(EV_VENDOR_FLUSH, 0, 0);
    ++state.flush_calls;
}

uint32_t p4_usb_test_read(uintptr_t address)
{
    uint32_t value = 0;

    if (address == state.routed_address && state.routed_address != 0)
        value = state.routed_value;
    else if (address == 0x50000008u)
        value = state.dwc_ahbcfg;
    else if (address == 0x50000804u)
        value = state.dwc_dctl;
    record(EV_MMIO_READ, address, value);
    return value;
}

void p4_usb_test_write(uintptr_t address, uint32_t value)
{
    record(EV_MMIO_WRITE, address, value);
    if (address == 0x50000018u)
        state.dwc_gintmsk = value;
    else if (address == 0x50000008u)
        state.dwc_ahbcfg = value;
    else if (address == 0x50000804u)
        state.dwc_dctl = value;
}

int p4_test_puts(const char *message)
{
    (void)snprintf(state.last_message, sizeof(state.last_message), "%s", message);
    return 0;
}

int p4_test_printf(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    (void)vsnprintf(state.last_message, sizeof(state.last_message), format, args);
    va_end(args);
    return 0;
}

static void test_argument_and_resource_gates(void)
{
    reset_scenario();
    state.read_args_ok = false;
    reject_before_phy();
    CHECK(count(EV_OPEN_RESOURCE) == 0);
    CHECK(count(EV_READ_ARGS) == 1);
    CHECK(count(EV_FREE_ARGS) == 0);

    reset_scenario();
    state.enable = false;
    reject_before_phy();
    CHECK(state.identity_calls == 0 && count(EV_OPEN_RESOURCE) == 0);

    reset_scenario();
    state.vid = 0;
    reject_before_phy();
    CHECK(state.identity_calls == 0 && count(EV_OPEN_RESOURCE) == 0);

    reset_scenario();
    state.pid = 65535;
    reject_before_phy();
    CHECK(state.identity_calls == 0 && count(EV_OPEN_RESOURCE) == 0);

    reset_scenario();
    state.seconds = 0;
    reject_before_phy();
    CHECK(state.identity_calls == 1 && count(EV_OPEN_RESOURCE) == 0);

    reset_scenario();
    state.seconds = 601;
    reject_before_phy();
    CHECK(state.identity_calls == 1 && count(EV_OPEN_RESOURCE) == 0);

    reset_scenario();
    state.steady_clocks = false;
    reject_before_phy();
    CHECK(state.identity_calls == 1 && count(EV_OPEN_RESOURCE) == 0);

    reset_scenario();
    state.resource_present = false;
    reject_before_phy();
    CHECK(count(EV_OPEN_RESOURCE) == 1 && count(EV_CREATE_PORT) == 0);

    reset_scenario();
    state.host_abi = P4_SDMMC_HOST_ABI - 1;
    reject_before_phy();
    CHECK(count(EV_CREATE_PORT) == 0);

    reset_scenario();
    state.host_size = (ULONG)sizeof(host) - 1u;
    reject_before_phy();
    CHECK(count(EV_CREATE_PORT) == 0);

    reset_scenario();
    state.host_has_acquire = false;
    reject_before_phy();
    CHECK(count(EV_CREATE_PORT) == 0);

    reset_scenario();
    state.host_has_release = false;
    reject_before_phy();
    CHECK(count(EV_CREATE_PORT) == 0);

    reset_scenario();
    state.host_busy_on_call = 1;
    reject_before_phy();
    CHECK(state.acquire_calls == 1 && state.release_calls == 0);
    CHECK(count(EV_CREATE_PORT) == 0);
    CHECK(state.alloc_vec_calls == 1 && state.free_vec_calls == 1);
    CHECK(count(EV_MMIO_READ) == 0 && count(EV_ADD_PORT) == 0);

    reset_scenario();
    state.owner_already_present = true;
    reject_before_phy();
    CHECK(state.acquire_calls == 1 && state.release_calls == 1);
    CHECK(count(EV_ADD_PORT) == 0 && count(EV_REMOVE_PORT) == 0);
    CHECK(count(EV_MMIO_READ) == 0);
    CHECK(state.alloc_vec_calls == 1 && state.free_vec_calls == 1);

    reset_scenario();
    state.alloc_vec_fails = true;
    reject_before_phy();
    CHECK(state.alloc_vec_calls == 1 && state.free_vec_calls == 0);
    CHECK(count(EV_NEW_LIST) == 0 && count(EV_ADD_PORT) == 0);
    CHECK(count(EV_MMIO_READ) == 0);
}

static void test_routed_irq_refusal(void)
{
    uintptr_t blocked = 0x500d6000u + 0x800u + 95u * 4u;

    reset_scenario();
    state.routed_address = blocked;
    state.routed_value = 1;
    reject_before_phy();
    CHECK(count(EV_MMIO_READ) == 4);
    CHECK(events[nth_event(EV_MMIO_READ, 3)].first == blocked);
    CHECK(count(EV_MMIO_WRITE) == 0);
    CHECK(count(EV_ADD_PORT) == 1 && count(EV_REMOVE_PORT) == 1);
    CHECK(registered_owner == NULL);
    CHECK(state.alloc_vec_calls == 1 && state.free_vec_calls == 1);
    CHECK(count(EV_CREATE_PORT) == 0 && count(EV_DELETE_PORT) == 0);
    CHECK(state.acquire_calls == 1 && state.release_calls == 1);
    CHECK(strstr(state.last_message, "IRQ source already routed") != NULL);
}

static struct observed_owner_record *owner_view(void)
{
    return (struct observed_owner_record *)owner_storage;
}

static void assert_owner_storage(bool retained)
{
    struct observed_owner_record *owner_record = owner_view();
    uintptr_t base = (uintptr_t)owner_storage;
    uintptr_t name = (uintptr_t)owner_record->port.mp_Node.ln_Name;

    CHECK(state.alloc_vec_calls == 1);
    CHECK(state.owner_allocation_size == sizeof(*owner_record));
    CHECK(state.owner_allocation_flags == (MEMF_PUBLIC | MEMF_CLEAR));
    CHECK(owner_record->signature == 0x50345545u);
    CHECK(name >= base && name < base + state.owner_allocation_size);
    CHECK(strcmp(owner_record->port.mp_Node.ln_Name,
                 "AROS.P4.USBEcho.owner") == 0);
    CHECK(owner_record->port.mp_Flags == PA_IGNORE);
    CHECK(owner_record->port.mp_SigTask == NULL);
    CHECK(owner_record->port.mp_MsgList.lh_Head ==
          (struct Node *)&owner_record->port.mp_MsgList.lh_Tail);
    CHECK(owner_record->port.mp_MsgList.lh_Tail == NULL);
    CHECK(owner_record->port.mp_MsgList.lh_TailPred ==
          (struct Node *)&owner_record->port.mp_MsgList.lh_Head);
    CHECK(state.new_list_calls == 1);
    if (retained)
    {
        CHECK(owner_record->saved_phy.snapshot_flags == 3u);
        CHECK(owner_record->saved_phy.try_rcc_lock == NULL);
        CHECK(owner_record->saved_phy.rcc_unlock == NULL);
        CHECK(owner_record->saved_phy.lock_context == NULL);
    }
}

static void assert_completed_cleanup(bool controller_touched)
{
    CHECK(count(EV_ADD_PORT) == 1 && count(EV_REMOVE_PORT) == 1);
    CHECK(events[first_event(EV_ADD_PORT)].first ==
          events[first_event(EV_REMOVE_PORT)].first);
    CHECK(events[first_event(EV_ADD_PORT)].first ==
          (uintptr_t)&owner_view()->port);
    CHECK(count(EV_DELETE_PORT) == 1 && state.delete_port_calls == 1);
    CHECK(events[first_event(EV_DELETE_PORT)].first == (uintptr_t)&ports[0]);
    CHECK(state.free_vec_calls == 1 && count(EV_FREE_VEC) == 1);
    CHECK(first_event(EV_REMOVE_PORT) < first_event(EV_FREE_VEC));
    CHECK(count(EV_FREE_ARGS) == 1);
    CHECK(first_event(EV_DELETE_PORT) < first_event(EV_FREE_ARGS));
    CHECK(count(EV_CLOSE_TIMER) == 1 && state.close_timer_calls == 1);
    CHECK(count(EV_DELETE_IO) == 1 && state.delete_io_calls == 1);
    CHECK(TimerBase == NULL);
    CHECK(count(EV_PHY_SETUP) == 1 && count(EV_PHY_DEINIT) >= 1);
    CHECK(count(EV_PHY_DEINIT) == state.phy_deinit_calls);
    CHECK(events[first_event(EV_PHY_DEINIT)].first ==
          events[first_event(EV_PHY_SETUP)].first);
    if (controller_touched)
    {
        CHECK(count(EV_MMIO_WRITE) == 3);
        CHECK(events[first_event(EV_MMIO_WRITE)].first == 0x50000018u);
        CHECK(first_event(EV_MMIO_WRITE) < first_event(EV_PHY_DEINIT));
    }
    else
        CHECK(count(EV_MMIO_WRITE) == 0);
    CHECK(nth_event(EV_PHY_DEINIT, state.phy_deinit_calls - 1) <
          first_event(EV_REMOVE_PORT));
    CHECK(first_event(EV_REMOVE_PORT) < first_event(EV_CLOSE_TIMER));
    CHECK(registered_owner == NULL);
    assert_owner_storage(false);
    CHECK(state.lock_depth == 0);
}

static void test_shortest_run_and_partial_echo(void)
{
    static const uint8_t payload[] = { 0x00, 0x7f, 0xff, 0x41, 0x00, 0x92 };
    static const uint32_t write_windows[] = { 6, 0, 4, 3 };
    static const uint32_t write_results[] = { 2, 1, 3 };
    static const uintptr_t expected_writes[] = {
        0x50000018u, 0x50000008u, 0x50000804u
    };
    static const uint32_t expected_values[] = { 0, 0xa4u, 0x12u };
    size_t i;

    reset_scenario();
    memcpy(state.input, payload, sizeof(payload));
    state.input_length = sizeof(payload);
    memcpy(state.write_available, write_windows, sizeof(write_windows));
    state.write_available_count = sizeof(write_windows) / sizeof(write_windows[0]);
    memcpy(state.write_return, write_results, sizeof(write_results));
    state.write_return_count = sizeof(write_results) / sizeof(write_results[0]);

    CHECK(run_app() == RETURN_OK);
    CHECK(state.now_ms == 1000u);
    CHECK(state.do_io_calls == 500u && state.dcd_calls == 500u);
    CHECK(state.tud_task_calls == 500u && state.set_signal_calls == 500u);
    CHECK(state.identity_calls == 1 && state.identity_vid == 0x1234 &&
          state.identity_pid == 0x5678);
    CHECK(state.phy_setup_calls == 1 && state.phy_deinit_calls == 1);
    CHECK(state.acquire_calls == 3 && state.release_calls == 3);
    CHECK(state.lock_depth == 0);
    CHECK(count(EV_MMIO_READ) == 6);
    CHECK(count(EV_MMIO_WRITE) == 3);
    CHECK(count(EV_VENDOR_READ) == 1 && state.read_calls == 1);
    CHECK(state.write_calls == 3 && state.flush_calls == 3);
    CHECK(state.output_length == sizeof(payload));
    CHECK(memcmp(state.output, payload, sizeof(payload)) == 0);
    CHECK(state.write_offered[0] == 6 && state.write_accepted[0] == 2);
    CHECK(state.write_offered[1] == 4 && state.write_accepted[1] == 1);
    CHECK(state.write_offered[2] == 3 && state.write_accepted[2] == 3);
    for (i = 0; i < 3; ++i)
    {
        const struct event *write = &events[nth_event(EV_MMIO_WRITE, i)];
        CHECK(write->first == expected_writes[i]);
        CHECK(write->second == expected_values[i]);
    }
    CHECK(first_event(EV_ADD_PORT) < first_event(EV_MMIO_READ));
    CHECK(first_event(EV_MMIO_READ) < first_event(EV_PHY_SETUP));
    CHECK(first_event(EV_PHY_SETUP) < first_event(EV_TUSB_INIT));
    CHECK(first_event(EV_TUSB_INIT) < first_event(EV_DCD_POLL));
    assert_completed_cleanup(true);
}

static void test_timer_open_failure_does_not_close_unopened_device(void)
{
    reset_scenario();
    state.timer_open_result = 1;
    CHECK(run_app() == RETURN_FAIL);
    CHECK(count(EV_OPEN_TIMER) == 1);
    CHECK(count(EV_CLOSE_TIMER) == 0 && state.close_timer_calls == 0);
    CHECK(count(EV_DELETE_IO) == 1 && state.delete_io_calls == 1);
    CHECK(count(EV_DELETE_PORT) == 1 && state.delete_port_calls == 1);
    CHECK(count(EV_FREE_ARGS) == 1);
    CHECK(state.alloc_vec_calls == 1 && state.free_vec_calls == 1);
    CHECK(count(EV_REMOVE_PORT) == 1 && count(EV_ADD_PORT) == 1);
    CHECK(count(EV_PHY_SETUP) == 0 && count(EV_PHY_DEINIT) == 0);
    CHECK(count(EV_MMIO_WRITE) == 0);
    CHECK(TimerBase == NULL);
    CHECK(registered_owner == NULL);
    assert_owner_storage(false);
}

static void assert_runtime_fault_cleanup(const char *message)
{
    CHECK(strstr(state.last_message, message) != NULL);
    CHECK(count(EV_TUSB_INIT) == 1);
    CHECK(count(EV_MMIO_WRITE) == 3);
    assert_completed_cleanup(true);
}

static void test_runtime_fault_cleanup(void)
{
    reset_scenario();
    state.tusb_init_ok = false;
    CHECK(run_app() == RETURN_FAIL);
    assert_runtime_fault_cleanup("TinyUSB init failed");
    CHECK(state.dcd_calls == 0 && state.do_io_calls == 0);

    reset_scenario();
    state.do_io_fail_on_call = 1;
    CHECK(run_app() == RETURN_FAIL);
    assert_runtime_fault_cleanup("timer failed");
    CHECK(state.dcd_calls == 1 && state.do_io_calls == 1);

    reset_scenario();
    state.expire_guard_in_dcd = true;
    CHECK(run_app() == RETURN_FAIL);
    assert_runtime_fault_cleanup("controller polling budget expired");
    CHECK(state.guard_expired && state.dcd_calls == 1);
    CHECK(state.tud_task_calls == 0 && state.do_io_calls == 0);
}

static void test_retryable_phy_teardown(void)
{
    static const p4_usb_hs_phy_result_t busy_then_ok[] = {
        P4_USB_HS_PHY_BUSY, P4_USB_HS_PHY_BUSY, P4_USB_HS_PHY_OK
    };
    static const p4_usb_hs_phy_result_t restore_error_then_ok[] = {
        P4_USB_HS_PHY_RESTORE_ERROR, P4_USB_HS_PHY_OK
    };
    static const p4_usb_hs_phy_result_t setup_restore_retry[] = {
        P4_USB_HS_PHY_RESTORE_ERROR, P4_USB_HS_PHY_OK
    };

    reset_scenario();
    set_deinit_results(busy_then_ok,
                       sizeof(busy_then_ok) / sizeof(busy_then_ok[0]));
    CHECK(run_app() == RETURN_OK);
    CHECK(state.phy_deinit_calls == 3);
    CHECK(state.do_io_calls == 502 && state.now_ms == 1002u);
    CHECK(state.acquire_calls == 3 && state.release_calls == 3);
    assert_completed_cleanup(true);

    reset_scenario();
    set_deinit_results(restore_error_then_ok,
                       sizeof(restore_error_then_ok) /
                       sizeof(restore_error_then_ok[0]));
    CHECK(run_app() == RETURN_OK);
    CHECK(state.phy_deinit_calls == 2);
    CHECK(state.do_io_calls == 501 && state.now_ms == 1001u);
    assert_completed_cleanup(true);

    reset_scenario();
    state.phy_setup_result = P4_USB_HS_PHY_RESTORE_ERROR;
    set_deinit_results(setup_restore_retry,
                       sizeof(setup_restore_retry) /
                       sizeof(setup_restore_retry[0]));
    CHECK(run_app() == RETURN_FAIL);
    CHECK(state.phy_setup_calls == 1 && state.phy_deinit_calls == 2);
    CHECK(state.tusb_init_calls == 0 && count(EV_DCD_POLL) == 0);
    CHECK(state.do_io_calls == 1 && state.now_ms == 1u);
    assert_completed_cleanup(false);
}

static void assert_quarantined_owner(bool controller_touched,
                                     uint32_t cleanup_start_ms,
                                     size_t deinit_attempts)
{
    struct observed_owner_record *owner_record = owner_view();

    CHECK(state.phy_setup_calls == 1);
    CHECK(state.phy_deinit_calls == deinit_attempts);
    CHECK(count(EV_PHY_DEINIT) == deinit_attempts);
    CHECK(count(EV_ADD_PORT) == 1 && count(EV_REMOVE_PORT) == 0);
    CHECK(registered_owner == &owner_record->port);
    CHECK(events[first_event(EV_ADD_PORT)].first == (uintptr_t)&owner_record->port);
    CHECK(state.free_vec_calls == 0 && count(EV_FREE_VEC) == 0);
    CHECK(count(EV_DELETE_PORT) == 1 && state.delete_port_calls == 1);
    CHECK(events[first_event(EV_DELETE_PORT)].first == (uintptr_t)&ports[0]);
    CHECK(count(EV_CLOSE_TIMER) == 1 && state.close_timer_calls == 1);
    CHECK(count(EV_DELETE_IO) == 1 && state.delete_io_calls == 1);
    CHECK(count(EV_FREE_ARGS) == 1);
    CHECK(TimerBase == NULL);
    CHECK(owner_record->signature == 0x50345545u);
    CHECK(owner_record->saved_phy.snapshot_flags == 3u);
    CHECK(owner_record->saved_phy.try_rcc_lock == NULL);
    CHECK(owner_record->saved_phy.rcc_unlock == NULL);
    CHECK(owner_record->saved_phy.lock_context == NULL);
    CHECK(owner_record->port.mp_Flags == PA_IGNORE);
    CHECK(owner_record->port.mp_SigTask == NULL);
    CHECK(strcmp(owner_record->port.mp_Node.ln_Name,
                 "AROS.P4.USBEcho.owner") == 0);
    CHECK((uintptr_t)owner_record->port.mp_Node.ln_Name >=
          (uintptr_t)owner_storage);
    CHECK((uintptr_t)owner_record->port.mp_Node.ln_Name <
          (uintptr_t)owner_storage + state.owner_allocation_size);
    CHECK(state.alloc_vec_calls == 1);
    CHECK(state.owner_allocation_size == sizeof(*owner_record));
    CHECK(state.owner_allocation_flags == (MEMF_PUBLIC | MEMF_CLEAR));
    CHECK(state.now_ms - cleanup_start_ms <= 500u);
    CHECK(state.lock_depth == 0);
    if (controller_touched)
        CHECK(count(EV_MMIO_WRITE) == 3);
    else
        CHECK(count(EV_MMIO_WRITE) == 0);
    CHECK(nth_event(EV_PHY_DEINIT, deinit_attempts - 1) <
          first_event(EV_FREE_ARGS));
    CHECK(strstr(state.last_message, "owner/snapshot quarantined until reboot") != NULL);
}

static void test_permanent_busy_quarantine(void)
{
    p4_usb_hs_phy_result_t results[16];
    size_t i;

    for (i = 0; i < sizeof(results) / sizeof(results[0]); ++i)
        results[i] = P4_USB_HS_PHY_BUSY;
    reset_scenario();
    set_deinit_results(results, sizeof(results) / sizeof(results[0]));
    CHECK(run_app() == RETURN_FAIL);
    CHECK(state.do_io_calls == 515);
    CHECK(state.now_ms == 1015u);
    assert_quarantined_owner(true, 1000u, 16);
}

static void test_permanent_restore_error_quarantine(void)
{
    p4_usb_hs_phy_result_t results[16];
    size_t i;

    for (i = 0; i < sizeof(results) / sizeof(results[0]); ++i)
        results[i] = P4_USB_HS_PHY_RESTORE_ERROR;
    reset_scenario();
    set_deinit_results(results, sizeof(results) / sizeof(results[0]));
    CHECK(run_app() == RETURN_FAIL);
    CHECK(state.do_io_calls == 515);
    CHECK(state.now_ms == 1015u);
    assert_quarantined_owner(true, 1000u, 16);
}

static void test_timer_failure_during_retry_quarantine(void)
{
    static const p4_usb_hs_phy_result_t busy_then_ok[] = {
        P4_USB_HS_PHY_BUSY, P4_USB_HS_PHY_OK
    };

    reset_scenario();
    set_deinit_results(busy_then_ok,
                       sizeof(busy_then_ok) / sizeof(busy_then_ok[0]));
    state.do_io_fail_on_call = 501;
    CHECK(run_app() == RETURN_FAIL);
    CHECK(state.phy_deinit_calls == 1);
    CHECK(state.do_io_calls == 501 && state.now_ms == 1000u);
    assert_quarantined_owner(true, 1000u, 1);
}

static void run_isolated(void (*test_case)(void))
{
    pid_t child = fork();
    int status;

    CHECK(child >= 0);
    if (child == 0)
    {
        test_case();
        _exit(EXIT_SUCCESS);
    }
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
}

int main(void)
{
    test_argument_and_resource_gates();
    test_routed_irq_refusal();
    test_shortest_run_and_partial_echo();
    test_timer_open_failure_does_not_close_unopened_device();
    test_runtime_fault_cleanup();
    test_retryable_phy_teardown();
    run_isolated(test_permanent_busy_quarantine);
    run_isolated(test_permanent_restore_error_quarantine);
    run_isolated(test_timer_failure_during_retry_quarantine);
    puts("test_main: pass (source orchestration only; fake platform, timer, controller, PHY and MMIO)");
    return EXIT_SUCCESS;
}
