#include "screen_test_native.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct rd_test_state rd_test;

static struct IntuitionBase test_intuition_base;
static struct Library test_cyber_base;
static struct Device test_input_device;

void rd_test_reset(void)
{
    memset(&rd_test, 0, sizeof(rd_test));
    rd_test.screen.Width = 640;
    rd_test.screen.Height = 480;
    rd_test.screen_available = true;
}

struct Library *OpenLibrary(const char *name, ULONG version)
{
    (void)version;
    ++rd_test.open_library_calls;
    if (strcmp(name, "intuition.library") == 0)
        return rd_test.fail_intuition_open ? NULL :
            (struct Library *)&test_intuition_base;
    if (strcmp(name, "cybergraphics.library") == 0)
        return rd_test.fail_cyber_open ? NULL : &test_cyber_base;
    return NULL;
}

void CloseLibrary(struct Library *library)
{
    (void)library;
    ++rd_test.close_library_calls;
}

APTR AllocVec(ULONG size, ULONG flags)
{
    ++rd_test.alloc_calls;
    if (rd_test.fail_alloc_call)
        return NULL;
    return (flags & MEMF_CLEAR) ? calloc(1, size) : malloc(size);
}

void FreeVec(APTR memory)
{
    if (memory != NULL) {
        ++rd_test.free_calls;
        free(memory);
    }
}

struct Screen *LockPubScreen(UBYTE *name)
{
    ++rd_test.lock_calls;
    (void)snprintf(rd_test.last_screen_name, sizeof(rd_test.last_screen_name),
        "%s", name == NULL ? "<default>" : (const char *)name);
    if (rd_test.press_button_on_lock) {
        rd_test.qualifier |= rd_test.qualifier_on_lock;
        rd_test.press_button_on_lock = false;
    }
    return rd_test.screen_available ? &rd_test.screen : NULL;
}

void UnlockPubScreen(UBYTE *name, struct Screen *screen)
{
    (void)name;
    (void)screen;
    ++rd_test.unlock_calls;
}

struct MsgPort *CreateMsgPort(void)
{
    ++rd_test.msg_port_creates;
    if (rd_test.fail_msg_port)
        return NULL;
    return (struct MsgPort *)calloc(1, sizeof(struct MsgPort));
}

void DeleteMsgPort(struct MsgPort *port)
{
    if (port != NULL) {
        ++rd_test.msg_port_deletes;
        free(port);
    }
}

APTR CreateIORequest(struct MsgPort *port, ULONG size)
{
    (void)port;
    ++rd_test.io_request_creates;
    if (rd_test.fail_io_request)
        return NULL;
    return calloc(1, size);
}

void DeleteIORequest(struct IORequest *request)
{
    if (request != NULL) {
        ++rd_test.io_request_deletes;
        free(request);
    }
}

int OpenDevice(const char *name, ULONG unit, struct IORequest *request, ULONG flags)
{
    (void)unit;
    (void)flags;
    ++rd_test.input_opens;
    if (strcmp(name, "input.device") != 0 || rd_test.fail_input_open)
        return 1;
    request->io_Device = &test_input_device;
    return 0;
}

void CloseDevice(struct IORequest *request)
{
    (void)request;
    ++rd_test.input_closes;
}

static UWORD button_qualifier(UWORD code)
{
    switch (code) {
    case IECODE_LBUTTON: return IEQUALIFIER_LEFTBUTTON;
    case IECODE_RBUTTON: return IEQUALIFIER_RBUTTON;
    default: return IEQUALIFIER_MIDBUTTON;
    }
}

int DoIO(struct IORequest *request)
{
    struct IOStdReq *io = (struct IOStdReq *)request;
    struct InputEvent *event = (struct InputEvent *)io->io_Data;
    int call_number = ++rd_test.doio_calls;
    struct rd_test_event *record = NULL;

    if (rd_test.doio_calls <= (int)(sizeof(rd_test.events) / sizeof(rd_test.events[0]))) {
        record = &rd_test.events[rd_test.doio_calls - 1];
        record->event_class = event->ie_Class;
        record->sub_class = event->ie_SubClass;
        record->code = event->ie_Code;
        record->qualifier = event->ie_Qualifier;
        record->x = event->ie_X;
        record->y = event->ie_Y;
        record->event_size = io->io_Length;
        if (event->ie_Class == IECLASS_NEWPOINTERPOS) {
            struct IEPointerPixel *pointer =
                (struct IEPointerPixel *)event->ie_EventAddress;
            record->has_pointer = true;
            record->pointer_screen = pointer->iepp_Screen;
            record->pointer_x = pointer->iepp_Position.X;
            record->pointer_y = pointer->iepp_Position.Y;
        }
    }

    if (event->ie_Class == IECLASS_RAWMOUSE) {
        if (event->ie_Code == IECODE_LBUTTON || event->ie_Code == IECODE_RBUTTON ||
            event->ie_Code == IECODE_MBUTTON) {
            rd_test.qualifier |= button_qualifier(event->ie_Code);
        } else if (event->ie_Code == (IECODE_LBUTTON | IECODE_UP_PREFIX) ||
            event->ie_Code == (IECODE_RBUTTON | IECODE_UP_PREFIX) ||
            event->ie_Code == (IECODE_MBUTTON | IECODE_UP_PREFIX)) {
            rd_test.qualifier &= (ULONG)~button_qualifier(
                event->ie_Code & (UWORD)~IECODE_UP_PREFIX);
        }
    }

    return rd_test.fail_doio_call == call_number ? -1 : 0;
}

ULONG PeekQualifier(void)
{
    return rd_test.qualifier;
}

ULONG ReadPixelArray(APTR destination, UWORD dest_x, UWORD dest_y,
    UWORD dest_modulo, struct RastPort *rast_port, UWORD src_x, UWORD src_y,
    UWORD width, UWORD height, UBYTE format)
{
    UBYTE *pixels = (UBYTE *)destination;
    ULONG row;
    ULONG column;
    (void)rast_port;

    ++rd_test.read_calls;
    rd_test.read_dest_modulo = dest_modulo;
    rd_test.read_src_x = src_x;
    rd_test.read_src_y = src_y;
    rd_test.read_width = width;
    rd_test.read_height = height;
    rd_test.read_format = format;
    for (row = 0; row < height; ++row) {
        for (column = 0; column < (ULONG)width * 3u; ++column) {
            pixels[(ULONG)dest_y * dest_modulo + row * dest_modulo +
                (ULONG)dest_x * 3u + column] = (UBYTE)(src_x + src_y + column + row);
        }
    }
    if (rd_test.override_read_count)
        return rd_test.read_count;
    return (ULONG)width * (ULONG)height;
}
