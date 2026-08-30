/* Task-context polling and independently implemented contact-frame policy. */

#include <aros/debug.h>
#include <devices/timer.h>
#include <exec/io.h>
#include <exec/tasks.h>
#include <proto/exec.h>

#include "p4touch_intern.h"

#define P4_TOUCH_POLL_US       50000UL
#define P4_TOUCH_RELEASE_POLLS 2U

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
    BOOL timer_open = FALSE, acquired = FALSE, down = FALSE;
    WORD last_x = 0, last_y = 0;
    ULONG zero_polls = 0, reads = 0, errors = 0, contact_frames = 0;
    ULONG multi_frames = 0, max_contacts = 0, max_reported = 0;

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
            zero_polls = 0;
            if (down)
            {
                p4touch_event(data, vHidd_Mouse_Release,
                              vHidd_Mouse_Button1, last_x, last_y);
                down = FALSE;
            }
            if (errors == 1 || !(errors & 0x3f))
                bug("[P4Touch/C4] read error %lu after %lu polls; "
                    "press state released\n",
                    (unsigned long)errors, (unsigned long)reads);
        }
        else if (frame.count)
        {
            ULONG primary, i;
            const struct KrnTouchScreenContact *contact;
            WORD x, y;

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
            if (!down)
            {
                p4touch_event(data, vHidd_Mouse_Press,
                              vHidd_Mouse_Button1, x, y);
                down = TRUE;
            }
        }
        else if (down && ++zero_polls >= P4_TOUCH_RELEASE_POLLS)
        {
            p4touch_event(data, vHidd_Mouse_Release,
                          vHidd_Mouse_Button1, last_x, last_y);
            down = FALSE;
            zero_polls = 0;
        }
        if (!(reads % 100))
            bug("[P4Touch/C4] heartbeat after %lu polls; down %u, frames "
                "%lu, multi %lu, max %lu/%lu, events %lu, errors %lu\n",
                (unsigned long)reads, (unsigned int)down,
                (unsigned long)contact_frames, (unsigned long)multi_frames,
                (unsigned long)max_contacts, (unsigned long)max_reported,
                (unsigned long)data->published_events,
                (unsigned long)errors);

        timer->tr_node.io_Command = TR_ADDREQUEST;
        timer->tr_time.tv_secs = 0;
        timer->tr_time.tv_micro = P4_TOUCH_POLL_US;
        if (DoIO(&timer->tr_node))
        {
            bug("[P4Touch/C4] timer request failed; stopped\n");
            data->running = FALSE;
        }
    }

out:
    bug("[P4Touch/C4] worker exiting after %lu polls; running %u, "
        "frames %lu, multi %lu, max %lu/%lu, events %lu, errors %lu\n",
        (unsigned long)reads, (unsigned int)data->running,
        (unsigned long)contact_frames, (unsigned long)multi_frames,
        (unsigned long)max_contacts, (unsigned long)max_reported,
        (unsigned long)data->published_events,
        (unsigned long)errors);
    if (acquired)
        data->ops->release();
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
