/* Host-only AROS and TinyUSB surface for source-bound main.c tests. */
#ifndef P4_USB_MAIN_TEST_PLATFORM_H
#define P4_USB_MAIN_TEST_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>

#include "hs_phy.h"
#include "p4_usb_platform.h"

typedef int32_t LONG;
typedef intptr_t SIPTR;
typedef intptr_t IPTR;
typedef uint32_t ULONG;
typedef uint8_t UBYTE;
typedef int BOOL;
typedef void *APTR;
typedef char *STRPTR;
typedef const char *CONST_STRPTR;

#define RETURN_OK 0
#define RETURN_FAIL 20
#define UNIT_MICROHZ 3u
#define TR_ADDREQUEST 1u
#define SIGBREAKF_CTRL_C 0x1000u
#define MEMF_PUBLIC 0x0001u
#define MEMF_CLEAR 0x0002u
#define PA_IGNORE 2u
#define P4_SDMMC_HOST_NAME "esp32p4sdmmc.resource"
#define P4_SDMMC_HOST_ABI 8u

struct Device { unsigned unused; };
struct Task { unsigned unused; };
struct RDArgs { unsigned unused; };
#define tv_secs tv_sec
#define tv_micro tv_usec
struct Node
{
    struct Node *ln_Succ;
    struct Node *ln_Pred;
    UBYTE ln_Type;
    int8_t ln_Pri;
    STRPTR ln_Name;
};
struct List
{
    struct Node *lh_Head;
    struct Node *lh_Tail;
    struct Node *lh_TailPred;
    UBYTE lh_Type;
    UBYTE l_pad;
};
struct MsgPort
{
    struct Node mp_Node;
    UBYTE mp_Flags;
    UBYTE mp_SigBit;
    void *mp_SigTask;
    struct List mp_MsgList;
};
struct IORequest
{
    struct Device *io_Device;
    uint32_t io_Command;
};
struct timerequest
{
    struct IORequest tr_node;
    struct timeval tr_time;
};

/* Only the fields main.c consumes from the private host ABI are represented. */
struct P4SDMMCHost
{
    ULONG abi;
    ULONG size;
    BOOL (*acquire)(struct P4SDMMCHost *host, ULONG timeout_us);
    void (*release)(struct P4SDMMCHost *host);
};

struct RDArgs *ReadArgs(CONST_STRPTR template, SIPTR *args,
                        struct RDArgs *existing);
LONG IoErr(void);
void PrintFault(LONG error, CONST_STRPTR header);
void FreeArgs(struct RDArgs *args);
void *OpenResource(CONST_STRPTR name);
APTR AllocVec(IPTR size, ULONG requirements);
void FreeVec(APTR memory);
void NewList(struct List *list);
struct MsgPort *CreateMsgPort(void);
struct MsgPort *FindPort(CONST_STRPTR name);
void AddPort(struct MsgPort *port);
void RemPort(struct MsgPort *port);
void DeleteMsgPort(struct MsgPort *port);
struct Task *FindTask(CONST_STRPTR name);
struct IORequest *CreateIORequest(struct MsgPort *port, ULONG size);
LONG OpenDevice(CONST_STRPTR name, ULONG unit, struct IORequest *request,
                ULONG flags);
void CloseDevice(struct IORequest *request);
void DeleteIORequest(struct IORequest *request);
void GetSysTime(struct timeval *time);
LONG DoIO(struct IORequest *request);
ULONG SetSignal(ULONG new_signals, ULONG signal_mask);

typedef enum tusb_role_t
{
    TUSB_ROLE_DEVICE = 1
} tusb_role_t;

typedef enum tusb_speed_t
{
    TUSB_SPEED_HIGH = 2
} tusb_speed_t;

typedef struct tusb_rhport_init_t
{
    tusb_role_t role;
    tusb_speed_t speed;
} tusb_rhport_init_t;

#define DCD_EVENT_BUS_RESET_START 0x10u
#define DCD_EVENT_UNPLUGGED 0x11u

bool tusb_init(uint8_t rhport, const tusb_rhport_init_t *init);
void dcd_int_handler(uint8_t rhport);
void tud_task_ext(uint32_t timeout_ms, bool in_isr);
bool tud_mounted(void);
uint32_t tud_vendor_available(void);
uint32_t tud_vendor_read(void *buffer, uint32_t length);
uint32_t tud_vendor_write_available(void);
uint32_t tud_vendor_write(const void *buffer, uint32_t length);
void tud_vendor_write_flush(void);

uint32_t p4_usb_test_read(uintptr_t address);
void p4_usb_test_write(uintptr_t address, uint32_t value);
int p4_test_puts(const char *message);
int p4_test_printf(const char *format, ...);

/* Keep command output inspectable without making the test depend on stdout. */
#ifdef P4_USB_ECHO_HOST_TEST
#define puts p4_test_puts
#define printf p4_test_printf
#endif

#endif
