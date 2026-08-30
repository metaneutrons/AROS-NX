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

#include "p4touch_intern.h"

#define P4_TOUCH_POLL_US       50000UL
#define P4_TOUCH_RELEASE_POLLS 2U
#define P4_TOUCH_FW_RETRY_US   1000000UL
#define P4_TOUCH_RECOVERY_ERRORS 3U
#define P4_TOUCH_MAX_RECOVERIES  3U

#define P4_TOUCH_FW_PATH "DEVS:Firmware/silead/gsl3670-d1001.fw"
#define P4_TOUCH_FW_FALLBACK \
    "FLASHDISK0P0:Firmware/silead/gsl3670-d1001.fw"

static BOOL p4touch_wait(struct timerequest *timer, ULONG micros)
{
    timer->tr_node.io_Command = TR_ADDREQUEST;
    timer->tr_time.tv_secs = micros / 1000000UL;
    timer->tr_time.tv_micro = micros % 1000000UL;
    return DoIO(&timer->tr_node) == 0;
}

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

static WORD p4touch_scale(ULONG raw, ULONG raw_size, ULONG logical_size)
{
    ULONG scaled;

    if (raw >= raw_size)
        raw = raw_size - 1;
    scaled = (raw * (logical_size - 1) + (raw_size - 1) / 2)
           / (raw_size - 1);
    return (WORD)scaled;
}

static WORD p4touch_scale_mirror(ULONG raw, ULONG raw_size,
                                 ULONG logical_size)
{
    return (WORD)(logical_size - 1
        - p4touch_scale(raw, raw_size, logical_size));
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
        WORD x = p4touch_scale(frame->contact[i].x, ops->raw_width,
                               ops->logical_width);
        WORD y = p4touch_scale_mirror(frame->contact[i].y, ops->raw_height,
                                      ops->logical_height);
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

static VOID P4TouchWorker(struct P4TouchMouseData *data)
{
    struct ExecBase *SysBase = data->ptd->cs_SysBase;
    struct Task *self = FindTask(NULL);
    struct MsgPort *port = NULL;
    struct timerequest *timer = NULL;
    struct DosLibrary *DOSBase = NULL;
    UBYTE *firmware = NULL;
    const char *firmware_path = NULL;
    BOOL timer_open = FALSE, acquired = FALSE, down = FALSE;
    UWORD active_button = vHidd_Mouse_NoButton;
    WORD last_x = 0, last_y = 0;
    ULONG zero_polls = 0, reads = 0, errors = 0, contact_frames = 0;
    ULONG multi_frames = 0, right_gestures = 0;
    ULONG max_contacts = 0, max_reported = 0;
    ULONG firmware_attempts = 0, recoveries = 0, consecutive_errors = 0;

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
    if (!data->running)
        goto out;

    acquired = data->ops->acquire();
    if (!acquired)
    {
        bug("[P4Touch/C4] cannot acquire persistent I2C0 session; stopped\n");
        data->running = FALSE;
        goto out;
    }

    bug("[P4Touch/C4] polling worker started at 20 Hz with persistent I2C0; "
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
            zero_polls = 0;
            if (down)
            {
                p4touch_event(data, vHidd_Mouse_Release,
                              active_button, last_x, last_y);
                down = FALSE;
                active_button = vHidd_Mouse_NoButton;
            }
            if (errors == 1 || !(errors & 0x3f))
                bug("[P4Touch/C4] read error %lu after %lu polls; "
                    "press state released\n",
                    (unsigned long)errors, (unsigned long)reads);
            if (consecutive_errors >= P4_TOUCH_RECOVERY_ERRORS)
            {
                ULONG failed_record = 0, status = 0;
                LONG result;

                data->ops->release();
                acquired = FALSE;
                if (++recoveries > P4_TOUCH_MAX_RECOVERIES)
                {
                    bug("[P4Touch/C4] recovery limit exhausted; touch stopped, "
                        "desktop remains live\n");
                    data->running = FALSE;
                    continue;
                }
                result = data->ops->load_firmware(
                    firmware, KRN_TOUCHSCREEN_FW_BYTES,
                    &failed_record, &status);
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
            UWORD desired_button;
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

            primary = p4touch_primary(&frame, data->ops, down,
                                      last_x, last_y);
            contact = &frame.contact[primary];
            x = p4touch_scale(contact->x, data->ops->raw_width,
                              data->ops->logical_width);
            y = p4touch_scale_mirror(contact->y, data->ops->raw_height,
                                     data->ops->logical_height);

            zero_polls = 0;
            if (!down || x != last_x || y != last_y)
                p4touch_event(data, vHidd_Mouse_Motion,
                              vHidd_Mouse_NoButton, x, y);
            last_x = x;
            last_y = y;

            /* A second contact promotes the active press to Button2.  Once
               promoted, keep Button2 latched while either contact remains;
               sequential finger release must not synthesize a new left
               press.  This also permits the Amiga menu button to be dragged. */
            desired_button = frame.count > 1
                           || active_button == vHidd_Mouse_Button2
                           ? vHidd_Mouse_Button2 : vHidd_Mouse_Button1;
            if (!down)
            {
                p4touch_event(data, vHidd_Mouse_Press,
                              desired_button, x, y);
                active_button = desired_button;
                down = TRUE;
                if (desired_button == vHidd_Mouse_Button2)
                    ++right_gestures;
            }
            else if (active_button != desired_button)
            {
                p4touch_event(data, vHidd_Mouse_Release,
                              active_button, x, y);
                p4touch_event(data, vHidd_Mouse_Press,
                              desired_button, x, y);
                active_button = desired_button;
                ++right_gestures;
            }
        }
        else if (down && ++zero_polls >= P4_TOUCH_RELEASE_POLLS)
        {
            p4touch_event(data, vHidd_Mouse_Release,
                          active_button, last_x, last_y);
            down = FALSE;
            active_button = vHidd_Mouse_NoButton;
            zero_polls = 0;
        }
        else
            consecutive_errors = 0;
        if (!(reads % 100))
            bug("[P4Touch/C4] heartbeat after %lu polls; down %u button %u, "
                "frames %lu, multi %lu, right %lu, max %lu/%lu, events %lu, "
                "errors %lu\n",
                (unsigned long)reads, (unsigned int)down,
                (unsigned int)active_button,
                (unsigned long)contact_frames, (unsigned long)multi_frames,
                (unsigned long)right_gestures,
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
    bug("[P4Touch/C4] worker exiting after %lu polls; running %u, "
        "frames %lu, multi %lu, right %lu, max %lu/%lu, events %lu, "
        "errors %lu\n",
        (unsigned long)reads, (unsigned int)data->running,
        (unsigned long)contact_frames, (unsigned long)multi_frames,
        (unsigned long)right_gestures,
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
