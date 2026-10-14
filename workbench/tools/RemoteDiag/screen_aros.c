/* Copyright (C) 2026, The AROS Development Team. */
#include "backend.h"

#ifdef RD_SCREEN_HOST_TEST
#include "screen_test_native.h"
#else
#include <cybergraphx/cybergraphics.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <proto/cybergraphics.h>
#include <proto/exec.h>
#include <proto/input.h>
#include <proto/intuition.h>
#endif

#include <limits.h>
#include <string.h>

struct IntuitionBase *IntuitionBase;
struct Library *CyberGfxBase;
struct Device *InputBase;

struct rd_screen {
    char *public_name;
    struct MsgPort *input_port;
    struct IOStdReq *input_io;
    bool allow_mouse;
    bool input_open;
    bool libraries_open;
    UWORD pending_button;
    UWORD pending_button_qualifier;
};

static ULONG rd_screen_library_users;
static ULONG rd_screen_input_users;

#define RD_MOUSE_BUTTONS \
    (IEQUALIFIER_LEFTBUTTON | IEQUALIFIER_RBUTTON | IEQUALIFIER_MIDBUTTON)

static bool rd_screen_libraries_acquire(void)
{
    if (rd_screen_library_users == 0) {
        IntuitionBase = (struct IntuitionBase *)OpenLibrary(
            (CONST_STRPTR)"intuition.library", 0);
        if (IntuitionBase == NULL)
            return false;

        CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 0);
        if (CyberGfxBase == NULL) {
            CloseLibrary((struct Library *)IntuitionBase);
            IntuitionBase = NULL;
            return false;
        }
    }

    ++rd_screen_library_users;
    return true;
}

static void rd_screen_libraries_release(void)
{
    if (rd_screen_library_users == 0)
        return;

    --rd_screen_library_users;
    if (rd_screen_library_users == 0) {
        CloseLibrary(CyberGfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        CyberGfxBase = NULL;
        IntuitionBase = NULL;
    }
}

static bool rd_screen_input_open(struct rd_screen *screen)
{
    screen->input_port = CreateMsgPort();
    if (screen->input_port == NULL)
        return false;

    screen->input_io = (struct IOStdReq *)CreateIORequest(
        screen->input_port, sizeof(*screen->input_io));
    if (screen->input_io == NULL)
        return false;

    if (OpenDevice((CONST_STRPTR)"input.device", 0,
        (struct IORequest *)screen->input_io, 0) != 0)
        return false;

    if (rd_screen_input_users == 0)
        InputBase = screen->input_io->io_Device;
    ++rd_screen_input_users;
    screen->input_open = true;
    return true;
}

static void rd_screen_input_close(struct rd_screen *screen)
{
    if (screen->input_open) {
        CloseDevice((struct IORequest *)screen->input_io);
        if (rd_screen_input_users != 0)
            --rd_screen_input_users;
        if (rd_screen_input_users == 0)
            InputBase = NULL;
        screen->input_open = false;
    }
    if (screen->input_io != NULL) {
        DeleteIORequest((struct IORequest *)screen->input_io);
        screen->input_io = NULL;
    }
    if (screen->input_port != NULL) {
        DeleteMsgPort(screen->input_port);
        screen->input_port = NULL;
    }
}

static void rd_screen_event_init(struct InputEvent *event, UBYTE event_class,
    UBYTE sub_class, UWORD code, UWORD qualifier)
{
    memset(event, 0, sizeof(*event));
    event->ie_Class = event_class;
    event->ie_SubClass = sub_class;
    event->ie_Code = code;
    event->ie_Qualifier = qualifier;
}

static int rd_screen_send_event(struct rd_screen *screen, struct InputEvent *event)
{
    screen->input_io->io_Command = IND_WRITEEVENT;
    screen->input_io->io_Data = event;
    screen->input_io->io_Length = sizeof(*event);
    return DoIO((struct IORequest *)screen->input_io) == 0 ? RD_OK : RD_IO;
}

static int rd_screen_release_pending(struct rd_screen *screen)
{
    struct InputEvent event;
    UWORD qualifier;
    int status;

    if (screen->pending_button == 0)
        return RD_OK;

    /* Keep current modifiers and other physical buttons; clear our button bit. */
    qualifier = (UWORD)PeekQualifier();
    qualifier &= (UWORD)~screen->pending_button_qualifier;
    rd_screen_event_init(&event, IECLASS_RAWMOUSE, IESUBCLASS_COMPATIBLE,
        screen->pending_button | IECODE_UP_PREFIX, qualifier);
    status = rd_screen_send_event(screen, &event);
    if (status == RD_OK)
        screen->pending_button = 0;
    return status;
}

static void rd_screen_destroy(struct rd_screen *screen)
{
    if (screen == NULL)
        return;

    if (screen->input_open && screen->pending_button != 0)
        (void)rd_screen_release_pending(screen);
    rd_screen_input_close(screen);
    if (screen->public_name != NULL)
        FreeVec(screen->public_name);
    if (screen->libraries_open)
        rd_screen_libraries_release();
    FreeVec(screen);
}

struct rd_screen *rd_screen_open(const char *public_name, bool allow_mouse)
{
    struct rd_screen *screen;
    struct Screen *target;
    size_t name_length;

    /* NULL and empty names would let LockPubScreen select the default screen. */
    if (public_name == NULL || public_name[0] == '\0')
        return NULL;
    name_length = strlen(public_name);
    if (name_length >= RD_PATH_MAX)
        return NULL;

    screen = (struct rd_screen *)AllocVec(sizeof(*screen), MEMF_PUBLIC | MEMF_CLEAR);
    if (screen == NULL)
        return NULL;
    screen->public_name = (char *)AllocVec((ULONG)(name_length + 1), MEMF_PUBLIC);
    if (screen->public_name == NULL) {
        FreeVec(screen);
        return NULL;
    }
    memcpy(screen->public_name, public_name, name_length + 1);

    if (!rd_screen_libraries_acquire()) {
        FreeVec(screen->public_name);
        FreeVec(screen);
        return NULL;
    }
    screen->libraries_open = true;

    target = LockPubScreen((UBYTE *)screen->public_name);
    if (target == NULL) {
        rd_screen_libraries_release();
        FreeVec(screen->public_name);
        FreeVec(screen);
        return NULL;
    }
    UnlockPubScreen(NULL, target);

    screen->allow_mouse = allow_mouse && rd_screen_input_open(screen);
    if (allow_mouse && !screen->allow_mouse) {
        /* Keep screenshots available when optional input setup is unavailable. */
        rd_screen_input_close(screen);
    }

    return screen;
}

void rd_screen_close(struct rd_screen *screen)
{
    rd_screen_destroy(screen);
}

bool rd_screen_has_mouse(const struct rd_screen *screen)
{
    return screen != NULL && screen->allow_mouse && screen->input_open;
}

int rd_screen_capture(struct rd_screen *screen, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, struct rd_snapshot *snapshot)
{
    struct Screen *target;
    uint64_t pixel_count;
    uint64_t byte_count;
    uint32_t screen_width;
    uint32_t screen_height;
    uint32_t row_bytes;
    ULONG copied;
    int status;

    if (snapshot == NULL)
        return RD_BAD_REQUEST;
    snapshot->data = NULL;
    snapshot->size = 0;
    snapshot->width = 0;
    snapshot->height = 0;

    if (screen == NULL || screen->public_name == NULL)
        return RD_UNAVAILABLE;
    target = LockPubScreen((UBYTE *)screen->public_name);
    if (target == NULL)
        return RD_UNAVAILABLE;
    status = RD_OK;
    if (target->Width <= 0 || target->Height <= 0) {
        status = RD_UNAVAILABLE;
        goto done;
    }

    screen_width = (uint32_t)target->Width;
    screen_height = (uint32_t)target->Height;

    if (width == 0 && height == 0 && x == 0 && y == 0) {
        width = screen_width;
        height = screen_height;
    } else if (width == 0 || height == 0) {
        status = RD_BAD_REQUEST;
        goto done;
    }

    if (width == 0 || height == 0 ||
        x > screen_width || y > screen_height ||
        width > screen_width - x || height > screen_height - y) {
        status = RD_BAD_REQUEST;
        goto done;
    }

    pixel_count = (uint64_t)width * (uint64_t)height;
    byte_count = pixel_count * 3u;
    row_bytes = width * 3u;
    if (byte_count > RD_SNAPSHOT_MAX || row_bytes > UINT16_MAX) {
        status = RD_LIMIT;
        goto done;
    }

    snapshot->data = (uint8_t *)AllocVec((ULONG)byte_count, MEMF_PUBLIC);
    if (snapshot->data == NULL) {
        status = RD_IO;
        goto done;
    }

    copied = ReadPixelArray(snapshot->data, 0, 0, (UWORD)row_bytes,
        &target->RastPort, (UWORD)x, (UWORD)y, (UWORD)width,
        (UWORD)height, RECTFMT_RGB);
    if ((uint64_t)copied != pixel_count) {
        FreeVec(snapshot->data);
        snapshot->data = NULL;
        status = RD_IO;
        goto done;
    }

    snapshot->size = (uint32_t)byte_count;
    snapshot->width = width;
    snapshot->height = height;
done:
    /* Do not keep a public-screen disposal pin while the snapshot is transferred. */
    UnlockPubScreen(NULL, target);
    return status;
}

static int rd_screen_move_pointer(struct rd_screen *screen, struct Screen *target,
    uint32_t x, uint32_t y)
{
    struct IEPointerPixel pointer;
    struct InputEvent event;
    UWORD qualifier;

    pointer.iepp_Screen = target;
    pointer.iepp_Position.X = (WORD)x;
    pointer.iepp_Position.Y = (WORD)y;
    rd_screen_event_init(&event, IECLASS_NEWPOINTERPOS, IESUBCLASS_PIXEL,
        IECODE_NOBUTTON, 0);
    event.ie_EventAddress = &pointer;
    /* A screen lock can block; recheck, though a press can still race DoIO. */
    qualifier = (UWORD)PeekQualifier();
    if ((qualifier & RD_MOUSE_BUTTONS) != 0)
        return RD_DENIED;
    event.ie_Qualifier = qualifier;
    return rd_screen_send_event(screen, &event);
}

int rd_screen_mouse(struct rd_screen *screen, uint32_t x, uint32_t y,
    unsigned button)
{
    struct Screen *target;
    struct InputEvent event;
    UWORD code;
    UWORD button_qualifier;
    UWORD qualifier;
    int status;

    if (screen == NULL || screen->public_name == NULL)
        return RD_UNAVAILABLE;
    if (!rd_screen_has_mouse(screen))
        return RD_DENIED;
    if (button > 3)
        return RD_BAD_REQUEST;
    status = rd_screen_release_pending(screen);
    if (status != RD_OK)
        return RD_IO;

    /* Moving with any button held would turn an absolute move into a drag. */
    if (((UWORD)PeekQualifier() & RD_MOUSE_BUTTONS) != 0)
        return RD_DENIED;

    target = LockPubScreen((UBYTE *)screen->public_name);
    if (target == NULL)
        return RD_UNAVAILABLE;
    if (target->Width <= 0 || target->Height <= 0) {
        status = RD_UNAVAILABLE;
        goto done;
    }
    if (x >= (uint32_t)target->Width || y >= (uint32_t)target->Height ||
        x > INT16_MAX || y > INT16_MAX) {
        status = RD_BAD_REQUEST;
        goto done;
    }

    status = rd_screen_move_pointer(screen, target, x, y);
    if (status != RD_OK || button == 0)
        goto done;

    /* Recheck just before click injection; a physical press can still race DoIO. */
    qualifier = (UWORD)PeekQualifier();
    if ((qualifier & RD_MOUSE_BUTTONS) != 0) {
        status = RD_DENIED;
        goto done;
    }

    switch (button) {
    case 1:
        code = IECODE_LBUTTON;
        button_qualifier = IEQUALIFIER_LEFTBUTTON;
        break;
    case 2:
        code = IECODE_RBUTTON;
        button_qualifier = IEQUALIFIER_RBUTTON;
        break;
    default:
        code = IECODE_MBUTTON;
        button_qualifier = IEQUALIFIER_MIDBUTTON;
        break;
    }

    screen->pending_button = code;
    screen->pending_button_qualifier = button_qualifier;
    rd_screen_event_init(&event, IECLASS_RAWMOUSE, IESUBCLASS_COMPATIBLE,
        code, qualifier | button_qualifier);
    status = rd_screen_send_event(screen, &event);
    if (status != RD_OK) {
        /* DoIO may have accepted the down event despite returning an error. */
        (void)rd_screen_release_pending(screen);
        status = RD_IO;
        goto done;
    }

    status = rd_screen_release_pending(screen);
done:
    UnlockPubScreen(NULL, target);
    return status;
}
