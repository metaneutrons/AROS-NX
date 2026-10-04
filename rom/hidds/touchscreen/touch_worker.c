/* touchscreen.hidd: the worker, a Process that polls the controller and
   does the driver's file work (touch_files.c), and the task that starts
   it once dos.library exists. */

#include <aros/asmcall.h>
#include <aros/debug.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/oop.h>
#include <proto/timer.h>

#include "touchscreen_intern.h"
#include "touch_policy.h"
#include "touch_coords.h"

/* One coherent report per poll, then a pause. Short taps are separated by
   the first empty report (touch_policy.c), not by a faster loop. */
#define TOUCH_POLL_US          50000UL
#define TOUCH_RETRY_US         1000000UL
#define TOUCH_START_ATTEMPTS   5U
#define TOUCH_RECOVERY_ERRORS  3U
#define TOUCH_MAX_RECOVERIES   3U
/* How often the starter looks for dos.library during boot. */
#define TOUCH_DOS_WAIT_US      200000UL

#undef OOPBase
#define OOPBase data->tsd->cs_OOPBase

static BOOL touch_wait(struct timerequest *timer, ULONG micros)
{
    timer->tr_node.io_Command = TR_ADDREQUEST;
    timer->tr_time.tv_secs = micros / 1000000UL;
    timer->tr_time.tv_micro = micros % 1000000UL;
    return DoIO(&timer->tr_node) == 0;
}

static ULONG touch_attr(struct TouchData *data, OOP_Object *obj,
                        OOP_AttrID attr)
{
    IPTR value = 0;

    OOP_GetAttr(obj, attr, &value);
    return (ULONG)value;
}

#define CONTROLLER_ATTR(data, idx) \
    touch_attr((data), (data)->controller, \
               (data)->tsd->hiddTouchControllerAB + (idx))

static BOOL touch_start(struct TouchData *data, const UBYTE *firmware,
                        ULONG bytes)
{
    struct pHidd_TouchController_Start msg;

    msg.mID = data->tsd->midStart;
    msg.firmware = firmware;
    msg.bytes = bytes;
    return (BOOL)OOP_DoMethod(data->controller, (OOP_Msg)&msg);
}

static VOID touch_stop(struct TouchData *data)
{
    struct pHidd_TouchController_Stop msg;

    msg.mID = data->tsd->midStop;
    OOP_DoMethod(data->controller, (OOP_Msg)&msg);
}

static BOOL touch_read(struct TouchData *data, struct HIDD_TouchFrame *frame)
{
    struct pHidd_TouchController_ReadFrame msg;

    msg.mID = data->tsd->midReadFrame;
    msg.frame = frame;
    return (BOOL)OOP_DoMethod(data->controller, (OOP_Msg)&msg);
}

/* Raw contact to screen, through the current calibration (swap first,
   then each axis with its own bounds and mirror). */
static void touch_map(const struct HIDD_TouchCalibration *cal,
                      ULONG width, ULONG height,
                      ULONG raw_x, ULONG raw_y, WORD *x, WORD *y)
{
    if (cal->flags & vHidd_TouchCal_SwapXY)
    {
        ULONG t = raw_x;

        raw_x = raw_y;
        raw_y = t;
    }
    *x = touch_coordinate(raw_x, cal->x_min, cal->x_max, width,
                          (cal->flags & vHidd_TouchCal_MirrorX) != 0);
    *y = touch_coordinate(raw_y, cal->y_min, cal->y_max, height,
                          (cal->flags & vHidd_TouchCal_MirrorY) != 0);
}

/* Pick up settings the preferences changed (struct TouchSettings). */
static void touch_refresh(struct TouchData *data, ULONG *generation,
                          struct HIDD_TouchCalibration *cal,
                          struct TouchPolicy *policy)
{
    struct ExecBase *SysBase = data->tsd->cs_SysBase;
    struct TouchParams params;
    unsigned int mode;

    if (*generation == data->generation)
        return;
    ObtainSemaphoreShared(&data->lock);
    *cal = data->settings.calibration;
    params = data->settings.params;
    mode = data->settings.mode;
    *generation = data->generation;
    ReleaseSemaphore(&data->lock);
    touch_policy_set_params(policy, &params);
    touch_policy_set_mode(policy, mode);
    bug("[Touch] %s: settings %lu: mode %s, calibration X=%lu..%lu "
        "Y=%lu..%lu flags 0x%lx; hold %lu ms, slop %ld px, tap-drag %lu "
        "ms/%ld px, two-finger right %u\n", data->name,
        (unsigned long)*generation,
        mode == TOUCH_DIRECT ? "direct" : "tap",
        (unsigned long)cal->x_min, (unsigned long)cal->x_max,
        (unsigned long)cal->y_min, (unsigned long)cal->y_max,
        (unsigned long)cal->flags, (unsigned long)params.hold_ms,
        (long)params.slop_px, (unsigned long)params.tapdrag_ms,
        (long)params.tapdrag_px, params.two_finger_right);
}

static ULONG touch_distance(WORD ax, WORD ay, WORD bx, WORD by)
{
    LONG dx = (LONG)ax - (LONG)bx;
    LONG dy = (LONG)ay - (LONG)by;

    return (ULONG)(dx * dx + dy * dy);
}

/* Hardware IDs are kept for diagnosis but not assumed to be stable. The
   visible pointer follows the contact nearest its last position. */
static ULONG touch_primary(const struct HIDD_TouchFrame *frame,
                           const struct HIDD_TouchCalibration *cal,
                           ULONG width, ULONG height,
                           BOOL down, WORD last_x, WORD last_y)
{
    ULONG best = 0, best_distance = ~0UL, i;

    if (!down || frame->count < 2)
        return 0;
    for (i = 0; i < frame->count; ++i)
    {
        WORD x, y;
        ULONG distance;

        touch_map(cal, width, height, frame->contact[i].x,
                  frame->contact[i].y, &x, &y);
        distance = touch_distance(x, y, last_x, last_y);
        if (distance < best_distance)
        {
            best = i;
            best_distance = distance;
        }
    }
    return best;
}

static VOID touch_event(struct TouchData *data, UWORD type, UWORD button,
                        WORD x, WORD y)
{
    struct pHidd_Mouse_Event event;

    event.button = button;
    event.x = x;
    event.y = y;
    event.type = type;
    ++data->published_events;
    if (type != vHidd_Mouse_Motion)
        bug("[Touch] %s: event %lu type %u button %u at %d,%d\n",
            data->name, (unsigned long)data->published_events,
            (unsigned int)type, (unsigned int)button, (int)x, (int)y);
    data->callback(data->callbackdata, &event);
}

struct TouchEmitter
{
    struct TouchData *data;
    ULONG right_gestures;
};

static void touch_emit(void *context, unsigned int action,
                       unsigned int button, int16_t x, int16_t y)
{
    struct TouchEmitter *e = context;
    UWORD type = action == TOUCH_MOTION ? vHidd_Mouse_Motion
               : action == TOUCH_PRESS ? vHidd_Mouse_Press
               : vHidd_Mouse_Release;
    UWORD mapped = button == TOUCH_LEFT ? vHidd_Mouse_Button1
                 : button == TOUCH_RIGHT ? vHidd_Mouse_Button2
                 : vHidd_Mouse_NoButton;

    if (action == TOUCH_PRESS && button == TOUCH_RIGHT)
        ++e->right_gestures;
    touch_event(e->data, type, mapped, x, y);
}

static uint32_t touch_now(struct timerequest *timer)
{
    struct Device *TimerBase = timer->tr_node.io_Device;
    struct timeval now;

    GetUpTime(&now);
    return (uint32_t)now.tv_secs * 1000U + now.tv_micro / 1000U;
}

/*
 * Start the controller, with its firmware if it needs one. The image may
 * be on a volume that mounts after the driver starts, so it is looked for
 * every second; once the controller runs, the image is kept for restarts
 * and this returns. A chip that needs none is tried a few times. FALSE
 * stops the worker.
 */
static BOOL touch_bring_up(struct TouchData *data, struct timerequest *timer,
                           struct TouchFiles *files, ULONG bytes,
                           UBYTE **firmware)
{
    struct ExecBase *SysBase = data->tsd->cs_SysBase;
    ULONG attempts = 0, missing = 0;

    while (data->running)
    {
        if (bytes && !*firmware)
        {
            CONST_STRPTR path;

            *firmware = touch_files_firmware(data, files, bytes, &path);
            if (!*firmware)
            {
                if (++missing == 1 || !(missing % 30U))
                    bug("[Touch] %s: firmware unavailable; attempt %lu; "
                        "desktop remains unblocked\n", data->name,
                        (unsigned long)missing);
                if (!touch_wait(timer, TOUCH_RETRY_US))
                    return FALSE;
                continue;
            }
            bug("[Touch] %s: firmware from %s\n", data->name, path);
        }
        ++attempts;
        if (touch_start(data, *firmware, bytes))
            return TRUE;
        bug("[Touch] %s: controller start failed, attempt %lu\n",
            data->name, (unsigned long)attempts);
        if (!bytes && attempts >= TOUCH_START_ATTEMPTS)
            return FALSE;
        if (*firmware)
        {
            /* A bad image would fail forever; read it again. */
            FreeVec(*firmware);
            *firmware = NULL;
        }
        if (!touch_wait(timer, TOUCH_RETRY_US))
            return FALSE;
    }
    return FALSE;
}

AROS_UFH3S(ULONG, Touch_Worker,
           AROS_UFHA(STRPTR, argptr, A0),
           AROS_UFHA(ULONG, argsize, D0),
           AROS_UFHA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT

    struct Task *self = FindTask(NULL);
    struct TouchData *data = self->tc_UserData;
    struct MsgPort *port = NULL;
    struct timerequest *timer = NULL;
    struct TouchFiles files;
    UBYTE *firmware = NULL;
    BOOL timer_open = FALSE, started = FALSE, files_open = FALSE;
    struct TouchPolicy policy;
    struct HIDD_TouchCalibration cal;
    ULONG generation = 0, bytes;
    struct TouchEmitter emitter = {data, 0};
    ULONG reads = 0, errors = 0, contact_frames = 0;
    ULONG multi_frames = 0;
    ULONG max_contacts = 0, max_reported = 0;
    ULONG recoveries = 0, consecutive_errors = 0;
#if TOUCHSCREEN_EDGE_TRACE
    ULONG edge_frames = 0, raw_min_x = ~0UL, raw_min_y = ~0UL;
    ULONG raw_max_x = 0, raw_max_y = 0;
#endif

    (void)argptr;
    (void)argsize;
    touch_policy_init(&policy, TOUCH_TAP);
    files.notify_signal = -1;
    files.DOSBase = NULL;
    files.buf = NULL;
    files.notifying = FALSE;
    port = CreateMsgPort();
    if (port)
        timer = (struct timerequest *)CreateIORequest(
            port, sizeof(struct timerequest));
    if (timer && !OpenDevice(TIMERNAME, UNIT_VBLANK, &timer->tr_node, 0))
        timer_open = TRUE;
    if (!timer_open)
    {
        bug("[Touch] %s: worker has no timer.device; stopped\n", data->name);
        data->running = FALSE;
        goto out;
    }
    files_open = touch_files_open(data, &files);
    if (!files_open)
        bug("[Touch] %s: no DOS for files; defaults stay\n", data->name);

    bytes = CONTROLLER_ATTR(data, aoHidd_TouchController_FirmwareBytes);
    if (bytes && !files_open)
    {
        bug("[Touch] %s: firmware needed but no DOS; touch stopped\n",
            data->name);
        data->running = FALSE;
        goto out;
    }
    started = touch_bring_up(data, timer, &files, bytes, &firmware);
    if (!started)
    {
        if (data->running)
            bug("[Touch] %s: controller does not start; touch stopped, "
                "desktop remains live\n", data->name);
        data->running = FALSE;
        goto out;
    }
    bug("[Touch] %s: %s started, raw %lux%lu; polling every 50 ms at "
        "priority %d\n", data->name,
        (char *)(IPTR)CONTROLLER_ATTR(data, aoHidd_TouchController_ChipName),
        (unsigned long)CONTROLLER_ATTR(data, aoHidd_TouchController_RawWidth),
        (unsigned long)CONTROLLER_ATTR(data, aoHidd_TouchController_RawHeight),
        (int)self->tc_Node.ln_Pri);

    touch_refresh(data, &generation, &cal, &policy);

    while (data->running)
    {
        struct HIDD_TouchFrame frame;
        BOOL read_ok;

        ++reads;
        if (files_open)
            touch_files_poll(data, &files, reads);
        touch_refresh(data, &generation, &cal, &policy);
        read_ok = touch_read(data, &frame);
        if (!read_ok)
        {
            ++errors;
            ++consecutive_errors;
            touch_policy_cancel(&policy, touch_emit, &emitter);
            if (errors == 1 || !(errors & 0x3f))
                bug("[Touch] %s: read error %lu after %lu polls; press "
                    "state released\n", data->name, (unsigned long)errors,
                    (unsigned long)reads);
            if (consecutive_errors >= TOUCH_RECOVERY_ERRORS)
            {
                touch_stop(data);
                if (++recoveries > TOUCH_MAX_RECOVERIES)
                {
                    bug("[Touch] %s: recovery limit exhausted; touch "
                        "stopped, desktop remains live\n", data->name);
                    started = FALSE;
                    data->running = FALSE;
                    continue;
                }
                if (!touch_start(data, firmware, bytes))
                {
                    bug("[Touch] %s: recovery %lu failed\n", data->name,
                        (unsigned long)recoveries);
                    started = FALSE;
                    data->running = FALSE;
                    continue;
                }
                consecutive_errors = 0;
                bug("[Touch] %s: recovery %lu passed\n", data->name,
                    (unsigned long)recoveries);
            }
        }
        else if (frame.count)
        {
            const struct HIDD_TouchContact *contact;
            ULONG primary, i;
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
                    bug("[Touch] %s: multi frame %lu reported %lu parsed %lu",
                        data->name, (unsigned long)multi_frames,
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

            primary = touch_primary(&frame, &cal, data->width, data->height,
                                    policy.active, policy.x, policy.y);
            contact = &frame.contact[primary];
            touch_map(&cal, data->width, data->height, contact->x,
                      contact->y, &x, &y);

#if TOUCHSCREEN_EDGE_TRACE
            /* Diagnostic only: never learn calibration from arbitrary use.
               Single-contact extremes exclude ambiguous multi-touch. */
            if (frame.count == 1)
            {
                ++edge_frames;
                if (contact->x < raw_min_x) raw_min_x = contact->x;
                if (contact->y < raw_min_y) raw_min_y = contact->y;
                if (contact->x > raw_max_x) raw_max_x = contact->x;
                if (contact->y > raw_max_y) raw_max_y = contact->y;
                if (edge_frames == 1 || !(edge_frames % 8))
                    bug("[Touch] %s: edge sample %lu raw %lu,%lu logical "
                        "%d,%d range X=%lu..%lu Y=%lu..%lu\n", data->name,
                        (unsigned long)edge_frames,
                        (unsigned long)contact->x, (unsigned long)contact->y,
                        (int)x, (int)y,
                        (unsigned long)raw_min_x, (unsigned long)raw_max_x,
                        (unsigned long)raw_min_y, (unsigned long)raw_max_y);
            }
#endif

            touch_policy_step(&policy, frame.count, x, y, touch_now(timer),
                              touch_emit, &emitter);
        }
        else
        {
            consecutive_errors = 0;
            touch_policy_step(&policy, 0, policy.x, policy.y,
                              touch_now(timer), touch_emit, &emitter);
        }
        if (!(reads % 100))
            bug("[Touch] %s: heartbeat after %lu polls; down %u button %u, "
                "frames %lu, multi %lu, right %lu, max %lu/%lu, events %lu, "
                "errors %lu\n", data->name,
                (unsigned long)reads, (unsigned int)(policy.button != 0),
                (unsigned int)policy.button,
                (unsigned long)contact_frames, (unsigned long)multi_frames,
                (unsigned long)emitter.right_gestures,
                (unsigned long)max_contacts, (unsigned long)max_reported,
                (unsigned long)data->published_events,
                (unsigned long)errors);

        if (!touch_wait(timer, TOUCH_POLL_US))
        {
            bug("[Touch] %s: timer request failed; stopped\n", data->name);
            data->running = FALSE;
        }
    }

out:
    touch_policy_cancel(&policy, touch_emit, &emitter);
    bug("[Touch] %s: worker exiting after %lu polls; frames %lu, multi %lu, "
        "right %lu, max %lu/%lu, events %lu, errors %lu\n", data->name,
        (unsigned long)reads, (unsigned long)contact_frames,
        (unsigned long)multi_frames, (unsigned long)emitter.right_gestures,
        (unsigned long)max_contacts, (unsigned long)max_reported,
        (unsigned long)data->published_events, (unsigned long)errors);
    if (started)
        touch_stop(data);
    if (files_open)
        touch_files_close(data, &files);
    if (firmware)
        FreeVec(firmware);
    if (timer_open)
        CloseDevice(&timer->tr_node);
    if (timer)
        DeleteIORequest(&timer->tr_node);
    if (port)
        DeleteMsgPort(port);
    /* Under Forbid() so the owner cannot free the code before this
       returns; the process ends with it. */
    Forbid();
    data->worker = NULL;
    Signal(data->owner, 1UL << data->stopped_signal);
    return 0;

    AROS_USERFUNC_EXIT
}

/*
 * The driver is created during ROM initialisation, before dos.library
 * exists, and the worker has to be a Process for its file work. This task
 * waits for dos.library, starts the worker Process and ends. If the driver
 * is disposed of first, it ends without starting anything.
 */
static VOID Touch_Starter(struct TouchData *data)
{
    struct ExecBase *SysBase = data->tsd->cs_SysBase;
    struct MsgPort *port = CreateMsgPort();
    struct timerequest *timer = port
        ? (struct timerequest *)CreateIORequest(port, sizeof(*timer)) : NULL;
    struct DosLibrary *DOSBase = NULL;
    struct Process *proc = NULL;

    if (timer && !OpenDevice(TIMERNAME, UNIT_VBLANK, &timer->tr_node, 0))
    {
        while (data->running
               && !(DOSBase = (struct DosLibrary *)OpenLibrary("dos.library",
                                                               36)))
            touch_wait(timer, TOUCH_DOS_WAIT_US);
        CloseDevice(&timer->tr_node);
    }
    if (DOSBase && data->running)
    {
        proc = CreateNewProcTags(NP_Entry, (IPTR)Touch_Worker,
                                 NP_Name, (IPTR)"touchscreen",
                                 NP_UserData, (IPTR)data,
                                 NP_Priority, 20,
                                 NP_StackSize, 32768,
                                 NP_WindowPtr, (IPTR)-1,
                                 TAG_DONE);
        if (!proc)
            bug("[Touch] %s: cannot start the worker; touch stopped\n",
                data->name);
    }
    if (DOSBase)
        CloseLibrary((struct Library *)DOSBase);
    if (timer)
        DeleteIORequest(&timer->tr_node);
    if (port)
        DeleteMsgPort(port);

    /* Hand over to the worker, or tell the owner nothing runs. */
    Forbid();
    data->worker = (struct Task *)proc;
    if (!proc)
    {
        data->running = FALSE;
        Signal(data->owner, 1UL << data->stopped_signal);
    }
}

BOOL Touch_StartWorker(struct TouchData *data)
{
    struct ExecBase *SysBase = data->tsd->cs_SysBase;
    struct TagItem tags[] =
    {
        {TASKTAG_NAME, (IPTR)"touchscreen start"},
        {TASKTAG_PRI, 0},
        {TASKTAG_PC, (IPTR)Touch_Starter},
        {TASKTAG_ARG1, (IPTR)data},
        {TASKTAG_STACKSIZE, 8192},
        {TAG_DONE, 0}
    };

    data->owner = FindTask(NULL);
    data->stopped_signal = AllocSignal(-1);
    if (data->stopped_signal < 0)
        return FALSE;
    data->running = TRUE;
    Forbid();
    data->worker = NewCreateTaskA(tags);
    Permit();
    if (!data->worker)
    {
        data->running = FALSE;
        FreeSignal(data->stopped_signal);
        data->stopped_signal = -1;
    }
    return data->worker != NULL;
}
