#ifndef RD_SCREEN_TEST_NATIVE_H
#define RD_SCREEN_TEST_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef int16_t WORD;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef void *APTR;
typedef uint8_t BOOL;
#define CONST_STRPTR const char *

struct Library { int kind; };
struct Device { struct Library library; };
struct IntuitionBase { struct Library library; };
struct RastPort { int unused; };
struct Screen {
    WORD Width;
    WORD Height;
    struct RastPort RastPort;
};
struct MsgPort { int unused; };
struct Message { int unused; };
struct Unit { int unused; };
struct IORequest {
    struct Message io_Message;
    struct Device *io_Device;
    struct Unit *io_Unit;
    UWORD io_Command;
    UBYTE io_Flags;
    int8_t io_Error;
};
struct IOStdReq {
    struct Message io_Message;
    struct Device *io_Device;
    struct Unit *io_Unit;
    UWORD io_Command;
    UBYTE io_Flags;
    int8_t io_Error;
    ULONG io_Actual;
    ULONG io_Length;
    APTR io_Data;
    ULONG io_Offset;
};
struct InputEvent {
    struct InputEvent *ie_NextEvent;
    UBYTE ie_Class;
    UBYTE ie_SubClass;
    UWORD ie_Code;
    UWORD ie_Qualifier;
    union {
        struct { WORD x; WORD y; } xy;
        APTR address;
    } ie_position;
};
#define ie_X ie_position.xy.x
#define ie_Y ie_position.xy.y
#define ie_EventAddress ie_position.address
struct IEPointerPixel {
    struct Screen *iepp_Screen;
    struct { WORD X; WORD Y; } iepp_Position;
};

#define MEMF_PUBLIC 1u
#define MEMF_CLEAR 2u
#define IND_WRITEEVENT 42u
#define RECTFMT_RGB 0u
#define IECLASS_RAWMOUSE 2u
#define IECLASS_NEWPOINTERPOS 19u
#define IESUBCLASS_COMPATIBLE 0u
#define IESUBCLASS_PIXEL 1u
#define IECODE_UP_PREFIX 0x80u
#define IECODE_LBUTTON 0x68u
#define IECODE_RBUTTON 0x69u
#define IECODE_MBUTTON 0x6au
#define IECODE_NOBUTTON 0xffu
#define IEQUALIFIER_CONTROL (1u << 3)
#define IEQUALIFIER_MIDBUTTON (1u << 12)
#define IEQUALIFIER_RBUTTON (1u << 13)
#define IEQUALIFIER_LEFTBUTTON (1u << 14)

struct rd_test_event {
    UBYTE event_class;
    UBYTE sub_class;
    UWORD code;
    UWORD qualifier;
    WORD x;
    WORD y;
    struct Screen *pointer_screen;
    WORD pointer_x;
    WORD pointer_y;
    bool has_pointer;
    size_t event_size;
};

struct rd_test_state {
    struct Screen screen;
    bool screen_available;
    bool fail_intuition_open;
    bool fail_cyber_open;
    bool fail_alloc_call;
    bool fail_msg_port;
    bool fail_io_request;
    bool fail_input_open;
    bool press_button_on_lock;
    ULONG qualifier_on_lock;
    int fail_doio_call;
    bool override_read_count;
    ULONG read_count;
    ULONG qualifier;
    char last_screen_name[128];
    int open_library_calls;
    int close_library_calls;
    int lock_calls;
    int unlock_calls;
    int alloc_calls;
    int free_calls;
    int msg_port_creates;
    int msg_port_deletes;
    int io_request_creates;
    int io_request_deletes;
    int input_opens;
    int input_closes;
    int read_calls;
    UWORD read_dest_modulo;
    UWORD read_src_x;
    UWORD read_src_y;
    UWORD read_width;
    UWORD read_height;
    ULONG read_format;
    int doio_calls;
    struct rd_test_event events[16];
};

extern struct rd_test_state rd_test;
extern struct Device *InputBase;
void rd_test_reset(void);

struct Library *OpenLibrary(const char *name, ULONG version);
void CloseLibrary(struct Library *library);
APTR AllocVec(ULONG size, ULONG flags);
void FreeVec(APTR memory);
struct Screen *LockPubScreen(UBYTE *name);
void UnlockPubScreen(UBYTE *name, struct Screen *screen);
struct MsgPort *CreateMsgPort(void);
void DeleteMsgPort(struct MsgPort *port);
APTR CreateIORequest(struct MsgPort *port, ULONG size);
void DeleteIORequest(struct IORequest *request);
int OpenDevice(const char *name, ULONG unit, struct IORequest *request, ULONG flags);
void CloseDevice(struct IORequest *request);
int DoIO(struct IORequest *request);
ULONG PeekQualifier(void);
ULONG ReadPixelArray(APTR destination, UWORD dest_x, UWORD dest_y,
    UWORD dest_modulo, struct RastPort *rast_port, UWORD src_x, UWORD src_y,
    UWORD width, UWORD height, UBYTE format);

#endif
