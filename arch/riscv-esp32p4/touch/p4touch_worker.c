/* Task-context polling and independently implemented contact-frame policy. */

#include <aros/debug.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/timer.h>

#include "p4touch_intern.h"
#include "p4touch_policy.h"
#include "p4touch_coords.h"
#include "../board/board.h"

/* A coherent 44-byte report on the proven 10-kHz bus is CPU-polled at
 * priority 20. Keep the idle gap until the transport can yield efficiently;
 * short taps are separated by the first-empty policy, not a faster loop. */
#define P4_TOUCH_POLL_US       50000UL
#define P4_TOUCH_FW_RETRY_US   1000000UL
#define P4_TOUCH_RECOVERY_ERRORS 3U
#define P4_TOUCH_MAX_RECOVERIES  3U

/* Only a controller that runs from RAM (the D1001's GSL3670) has a firmware
   file; the board profile names it. A GT911 runs from its own ROM. */
#ifdef P4_BOARD_TOUCH_FW_PATH
#define P4_TOUCH_HAS_FIRMWARE 1
#define P4_TOUCH_FW_PATH P4_BOARD_TOUCH_FW_PATH
#define P4_TOUCH_FW_FALLBACK P4_BOARD_TOUCH_FW_FALLBACK
#else
#define P4_TOUCH_HAS_FIRMWARE 0
#endif

static BOOL p4touch_wait(struct timerequest *timer, ULONG micros)
{
    timer->tr_node.io_Command = TR_ADDREQUEST;
    timer->tr_time.tv_secs = micros / 1000000UL;
    timer->tr_time.tv_micro = micros % 1000000UL;
    return DoIO(&timer->tr_node) == 0;
}

#if P4_TOUCH_HAS_FIRMWARE
static UBYTE *p4touch_read_firmware(struct DosLibrary *DOSBase,
                                    const char **loaded_path)
{
    static const char *paths[] =
    {
        P4_TOUCH_FW_PATH,
        P4_TOUCH_FW_FALLBACK,
        NULL
    };
    ULONG i;

    *loaded_path = NULL;
    for (i = 0; paths[i]; ++i)
    {
        BPTR file = Open((CONST_STRPTR)paths[i], MODE_OLDFILE);
        LONG size, got;
        UBYTE *data;

        if (!file)
            continue;
        /* A failed FAT ACTION_FINDINPUT currently leaves DOS with an
           allocated NIL-style FileHandle instead of returning BPTR zero.
           Never Seek/Read such a handle: fh_Type is the handler port and is
           installed only by a successful open. */
        if (!((struct FileHandle *)BADDR(file))->fh_Type)
        {
            Close(file);
            continue;
        }
        Seek(file, 0, OFFSET_END);
        size = Seek(file, 0, OFFSET_BEGINNING);
        if (size != (LONG)KRN_TOUCHSCREEN_FW_BYTES)
        {
            bug("[P4Touch/C4] rejected firmware %s: %ld bytes, expected %lu\n",
                paths[i], (long)size,
                (unsigned long)KRN_TOUCHSCREEN_FW_BYTES);
            Close(file);
            continue;
        }
        data = AllocVec(KRN_TOUCHSCREEN_FW_BYTES, MEMF_PUBLIC);
        if (!data)
        {
            Close(file);
            return NULL;
        }
        got = Read(file, data, KRN_TOUCHSCREEN_FW_BYTES);
        Close(file);
        if (got != (LONG)KRN_TOUCHSCREEN_FW_BYTES)
        {
            bug("[P4Touch/C4] short firmware read from %s: %ld/%lu\n",
                paths[i], (long)got,
                (unsigned long)KRN_TOUCHSCREEN_FW_BYTES);
            FreeVec(data);
            continue;
        }
        *loaded_path = paths[i];
        return data;
    }
    return NULL;
}
#endif

static WORD p4touch_x(ULONG raw, const struct KrnTouchScreenOps *ops)
{
    return p4touch_coordinate(raw, P4_BOARD_TOUCH_X_MIN,
                             P4_BOARD_TOUCH_X_MAX, ops->logical_width, 0);
}

static WORD p4touch_y(ULONG raw, const struct KrnTouchScreenOps *ops)
{
    return p4touch_coordinate(raw, P4_BOARD_TOUCH_Y_MIN,
                             P4_BOARD_TOUCH_Y_MAX, ops->logical_height,
                             P4_BOARD_TOUCH_MIRROR_Y);
}

static ULONG p4touch_distance(WORD ax, WORD ay, WORD bx, WORD by)
{
    LONG dx = (LONG)ax - (LONG)bx;
    LONG dy = (LONG)ay - (LONG)by;

    return (ULONG)(dx * dx + dy * dy);
}

/* Hardware IDs are retained for diagnosis but are not assumed to be stable.
   Keep the visible primary pointer on the contact nearest its last position. */
static ULONG p4touch_primary(const struct KrnTouchScreenFrame *frame,
                             const struct KrnTouchScreenOps *ops,
                             BOOL down, WORD last_x, WORD last_y)
{
    ULONG best = 0, best_distance = ~0UL, i;

    if (!down || frame->count < 2)
        return 0;
    for (i = 0; i < frame->count; ++i)
    {
        WORD x = p4touch_x(frame->contact[i].x, ops);
        WORD y = p4touch_y(frame->contact[i].y, ops);
        ULONG distance = p4touch_distance(x, y, last_x, last_y);

        if (distance < best_distance)
        {
            best = i;
            best_distance = distance;
        }
    }
    return best;
}

static VOID p4touch_event(struct P4TouchMouseData *data, UWORD type,
                          UWORD button, WORD x, WORD y)
{
    struct pHidd_Mouse_Event event;

    event.button = button;
    event.x = x;
    event.y = y;
    event.type = type;
    ++data->published_events;
    if (type != vHidd_Mouse_Motion)
        bug("[P4Touch/C4] event %lu type %u button %u at %d,%d\n",
            (unsigned long)data->published_events, (unsigned int)type,
            (unsigned int)button, (int)x, (int)y);
    data->callback(data->callbackdata, &event);
}

struct P4TouchEmitter
{
    struct P4TouchMouseData *data;
    ULONG right_gestures;
};

static void p4touch_emit(void *context, unsigned int action,
                         unsigned int button, int16_t x, int16_t y)
{
    struct P4TouchEmitter *e = context;
    UWORD type = action == P4_TOUCH_MOTION ? vHidd_Mouse_Motion
               : action == P4_TOUCH_PRESS ? vHidd_Mouse_Press
               : vHidd_Mouse_Release;
    UWORD mapped = button == P4_TOUCH_LEFT ? vHidd_Mouse_Button1
                 : button == P4_TOUCH_RIGHT ? vHidd_Mouse_Button2
                 : vHidd_Mouse_NoButton;

    if (action == P4_TOUCH_PRESS && button == P4_TOUCH_RIGHT)
        ++e->right_gestures;
    p4touch_event(e->data, type, mapped, x, y);
}

static uint32_t p4touch_now(struct timerequest *timer)
{
    struct Device *TimerBase = timer->tr_node.io_Device;
    struct timeval now;

    GetUpTime(&now);
    return (uint32_t)now.tv_secs * 1000U + now.tv_micro / 1000U;
}

/* Read a startup-only preference beside the successfully loaded firmware.
   That volume is already mounted; ENV/ENVARC assigns may not exist yet.
   Missing/invalid preferences always select the safe tap mode. */
static unsigned int p4touch_mode(struct DosLibrary *DOSBase, const char *fw)
{
    char path[256], value[8];
    ULONG i = 0, directory = 0;
    BPTR file;
    LONG count;
    const char name[] = "p4touch.mode";

    /* Without a firmware file there is no directory to look in. */
    if (!fw)
        return P4_TOUCH_TAP;
    while (fw[i] && i < sizeof(path) - sizeof(name))
    {
        path[i] = fw[i];
        if (fw[i] == '/' || fw[i] == ':')
            directory = i + 1;
        ++i;
    }
    if (fw[i] || !directory)
        return P4_TOUCH_TAP;
    for (i = 0; i < sizeof(name); ++i)
        path[directory + i] = name[i];
    file = Open(path, MODE_OLDFILE);
    if (!file)
        return P4_TOUCH_TAP;
    if (!((struct FileHandle *)BADDR(file))->fh_Type)
    {
        Close(file);
        return P4_TOUCH_TAP;
    }
    count = Read(file, value, sizeof(value));
    Close(file);
    return (count == 6 || (count == 7 && value[6] == '\n'))
        && value[0] == 'd' && value[1] == 'i' && value[2] == 'r'
        && value[3] == 'e' && value[4] == 'c' && value[5] == 't'
        ? P4_TOUCH_DIRECT : P4_TOUCH_TAP;
}

static VOID P4TouchWorker(struct P4TouchMouseData *data)
{
    struct ExecBase *SysBase = data->ptd->cs_SysBase;
    struct Task *self = FindTask(NULL);
    struct MsgPort *port = NULL;
    struct timerequest *timer = NULL;
    struct DosLibrary *DOSBase = NULL;
    UBYTE *firmware = NULL;
    const char *firmware_path = NULL;
    BOOL timer_open = FALSE, acquired = FALSE;
    struct P4TouchPolicy policy;
    struct P4TouchEmitter emitter = {data, 0};
    ULONG reads = 0, errors = 0, contact_frames = 0;
    ULONG multi_frames = 0;
    ULONG max_contacts = 0, max_reported = 0;
    ULONG firmware_attempts = 0, recoveries = 0, consecutive_errors = 0;
#if P4_TOUCH_EDGE_TRACE
    ULONG edge_frames = 0, raw_min_x = ~0UL, raw_min_y = ~0UL;
    ULONG raw_max_x = 0, raw_max_y = 0;
#endif

    p4touch_policy_init(&policy, P4_TOUCH_TAP);
    port = CreateMsgPort();
    if (port)
        timer = (struct timerequest *)CreateIORequest(
            port, sizeof(struct timerequest));
    if (timer && !OpenDevice(TIMERNAME, UNIT_VBLANK,
                             &timer->tr_node, 0))
        timer_open = TRUE;
    if (!timer_open)
    {
        bug("[P4Touch/C4] worker has no timer.device; stopped\n");
        data->running = FALSE;
        goto out;
    }
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 0);
#if P4_TOUCH_HAS_FIRMWARE
    while (data->running && !firmware)
    {
        ULONG failed_record = 0, status = 0;
        LONG result;

        ++firmware_attempts;
        if (!DOSBase)
            DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 0);
        if (DOSBase)
            firmware = p4touch_read_firmware(DOSBase, &firmware_path);
        if (!firmware)
        {
            if (firmware_attempts == 1 || !(firmware_attempts % 30U))
                bug("[P4Touch/C4] external firmware unavailable; attempt %lu; "
                    "desktop remains unblocked\n",
                    (unsigned long)firmware_attempts);
            if (!p4touch_wait(timer, P4_TOUCH_FW_RETRY_US))
                data->running = FALSE;
            continue;
        }
        result = data->ops->load_firmware(firmware,
                                          KRN_TOUCHSCREEN_FW_BYTES,
                                          &failed_record, &status);
        if (result)
        {
            bug("[P4Touch/C4] external firmware load failed from %s: "
                "result %ld record %lu status 0x%08lx; retrying\n",
                firmware_path, (long)result, (unsigned long)failed_record,
                (unsigned long)status);
            FreeVec(firmware);
            firmware = NULL;
            firmware_path = NULL;
            if (!p4touch_wait(timer, P4_TOUCH_FW_RETRY_US))
                data->running = FALSE;
        }
        else
            bug("[P4Touch/C4] loaded external firmware %s: %lu records, "
                "status 0x%08lx\n", firmware_path,
                (unsigned long)KRN_TOUCHSCREEN_FW_RECORDS,
                (unsigned long)status);
    }
#endif
    if (!data->running)
        goto out;

    p4touch_policy_init(&policy, p4touch_mode(DOSBase, firmware_path));
    bug("[P4Touch/C4] gesture mode %s; hold %u ms, slop %u px\n",
        policy.mode == P4_TOUCH_DIRECT ? "direct" : "tap",
        P4_TOUCH_HOLD_MS, P4_TOUCH_SLOP_PX);
    bug("[P4Touch/C4] calibration X=%u..%u Y=%u..%u%s\n",
        P4_BOARD_TOUCH_X_MIN, P4_BOARD_TOUCH_X_MAX,
        P4_BOARD_TOUCH_Y_MIN, P4_BOARD_TOUCH_Y_MAX,
        P4_BOARD_TOUCH_MIRROR_Y ? "; mirrored Y" : "");

    acquired = data->ops->acquire();
    if (!acquired)
    {
        bug("[P4Touch/C4] cannot acquire persistent I2C0 session; stopped\n");
        data->running = FALSE;
        goto out;
    }

    bug("[P4Touch/C4] polling worker started with 50-ms delay after I2C0 read; "
        "task priority %d, timer request %p\n", (int)self->tc_Node.ln_Pri,
        timer);
    while (data->running)
    {
        struct KrnTouchScreenFrame frame;
        BOOL read_ok;

        ++reads;
        read_ok = data->ops->read_contacts(&frame);
        if (!read_ok)
        {
            ++errors;
            ++consecutive_errors;
            p4touch_policy_cancel(&policy, p4touch_emit, &emitter);
            if (errors == 1 || !(errors & 0x3f))
                bug("[P4Touch/C4] read error %lu after %lu polls; "
                    "press state released\n",
                    (unsigned long)errors, (unsigned long)reads);
            if (consecutive_errors >= P4_TOUCH_RECOVERY_ERRORS)
            {
                ULONG failed_record = 0, status = 0;
                LONG result = 0;

                data->ops->release();
                acquired = FALSE;
                if (++recoveries > P4_TOUCH_MAX_RECOVERIES)
                {
                    bug("[P4Touch/C4] recovery limit exhausted; touch stopped, "
                        "desktop remains live\n");
                    data->running = FALSE;
                    continue;
                }
#if P4_TOUCH_HAS_FIRMWARE
                result = data->ops->load_firmware(
                    firmware, KRN_TOUCHSCREEN_FW_BYTES,
                    &failed_record, &status);
#endif
                if (result || !(acquired = data->ops->acquire()))
                {
                    bug("[P4Touch/C4] recovery %lu failed: result %ld "
                        "record %lu status 0x%08lx\n",
                        (unsigned long)recoveries, (long)result,
                        (unsigned long)failed_record,
                        (unsigned long)status);
                    data->running = FALSE;
                    continue;
                }
                consecutive_errors = 0;
                bug("[P4Touch/C4] recovery %lu passed; status 0x%08lx\n",
                    (unsigned long)recoveries, (unsigned long)status);
            }
        }
        else if (frame.count)
        {
            ULONG primary, i;
            const struct KrnTouchScreenContact *contact;
            WORD x, y;

            consecutive_errors = 0;

            ++contact_frames;
            if (frame.count > max_contacts)
                max_contacts = frame.count;
            if (frame.reported_count > max_reported)
                max_reported = frame.reported_count;
            if (frame.count > 1)
            {
                ++multi_frames;
                if (multi_frames <= 80 || !(multi_frames & 0x7f))
                {
                    bug("[P4Touch/C4] multi frame %lu reported %lu parsed %lu",
                        (unsigned long)multi_frames,
                        (unsigned long)frame.reported_count,
                        (unsigned long)frame.count);
                    for (i = 0; i < frame.count; ++i)
                        bug(" [%lu:%lu,%lu]",
                            (unsigned long)frame.contact[i].id,
                            (unsigned long)frame.contact[i].x,
                            (unsigned long)frame.contact[i].y);
                    bug("\n");
                }
            }

            primary = p4touch_primary(&frame, data->ops, policy.active,
                                      policy.x, policy.y);
            contact = &frame.contact[primary];
            x = p4touch_x(contact->x, data->ops);
            y = p4touch_y(contact->y, data->ops);

#if P4_TOUCH_EDGE_TRACE
            /* Diagnostic only: never learn calibration from arbitrary use.
               Single-contact extrema exclude ambiguous multi-touch frames. */
            if (frame.count == 1)
            {
                ++edge_frames;
                if (contact->x < raw_min_x) raw_min_x = contact->x;
                if (contact->y < raw_min_y) raw_min_y = contact->y;
                if (contact->x > raw_max_x) raw_max_x = contact->x;
                if (contact->y > raw_max_y) raw_max_y = contact->y;
                if (edge_frames == 1 || !(edge_frames % 8))
                    bug("[P4Touch/C4] edge sample %lu raw %lu,%lu logical %d,%d "
                        "range X=%lu..%lu Y=%lu..%lu\n",
                        (unsigned long)edge_frames,
                        (unsigned long)contact->x, (unsigned long)contact->y,
                        (int)x, (int)y,
                        (unsigned long)raw_min_x, (unsigned long)raw_max_x,
                        (unsigned long)raw_min_y, (unsigned long)raw_max_y);
            }
#endif

            p4touch_policy_step(&policy, frame.count, x, y,
                                p4touch_now(timer), p4touch_emit, &emitter);
        }
        else
        {
            consecutive_errors = 0;
            p4touch_policy_step(&policy, 0, policy.x, policy.y,
                                p4touch_now(timer), p4touch_emit, &emitter);
        }
        if (!(reads % 100))
            bug("[P4Touch/C4] heartbeat after %lu polls; down %u button %u, "
                "frames %lu, multi %lu, right %lu, max %lu/%lu, events %lu, "
                "errors %lu\n",
                (unsigned long)reads, (unsigned int)(policy.button != 0),
                (unsigned int)policy.button,
                (unsigned long)contact_frames, (unsigned long)multi_frames,
                (unsigned long)emitter.right_gestures,
                (unsigned long)max_contacts, (unsigned long)max_reported,
                (unsigned long)data->published_events,
                (unsigned long)errors);

        if (!p4touch_wait(timer, P4_TOUCH_POLL_US))
        {
            bug("[P4Touch/C4] timer request failed; stopped\n");
            data->running = FALSE;
        }
    }

out:
    p4touch_policy_cancel(&policy, p4touch_emit, &emitter);
    bug("[P4Touch/C4] worker exiting after %lu polls; running %u, "
        "frames %lu, multi %lu, right %lu, max %lu/%lu, events %lu, "
        "errors %lu\n",
        (unsigned long)reads, (unsigned int)data->running,
        (unsigned long)contact_frames, (unsigned long)multi_frames,
        (unsigned long)emitter.right_gestures,
        (unsigned long)max_contacts, (unsigned long)max_reported,
        (unsigned long)data->published_events,
        (unsigned long)errors);
    if (acquired)
        data->ops->release();
    if (firmware)
        FreeVec(firmware);
    if (DOSBase)
        CloseLibrary((struct Library *)DOSBase);
    if (timer_open)
        CloseDevice(&timer->tr_node);
    if (timer)
        DeleteIORequest(&timer->tr_node);
    if (port)
        DeleteMsgPort(port);
    data->worker = NULL;
    Signal(data->owner, 1UL << data->stopped_signal);
}

BOOL P4TouchStartWorker(struct P4TouchMouseData *data)
{
    struct ExecBase *SysBase = data->ptd->cs_SysBase;
    struct TagItem tags[] =
    {
        {TASKTAG_NAME, (IPTR)"ESP32-P4 touch poll"},
        {TASKTAG_PRI, 20},
        {TASKTAG_PC, (IPTR)P4TouchWorker},
        {TASKTAG_ARG1, (IPTR)data},
        {TASKTAG_STACKSIZE, 32768},
        {TAG_DONE, 0}
    };

    data->owner = FindTask(NULL);
    data->stopped_signal = AllocSignal(-1);
    if (data->stopped_signal < 0)
        return FALSE;
    data->running = TRUE;
    data->worker = NewCreateTaskA(tags);
    if (!data->worker)
    {
        data->running = FALSE;
        FreeSignal(data->stopped_signal);
        data->stopped_signal = -1;
    }
    return data->worker != NULL;
}
