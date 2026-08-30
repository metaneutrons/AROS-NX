/* Generic mouse.hidd subclass for the D1001 contact-zero worker. */

#include <aros/debug.h>
#include <proto/exec.h>
#include <proto/oop.h>
#include <proto/utility.h>
#include <utility/tagitem.h>

#include "p4touch_intern.h"

#define SysBase ((struct ExecBase *)PTD(cl)->cs_SysBase)
#define OOPBase PTD(cl)->cs_OOPBase
#define UtilityBase PTD(cl)->cs_UtilityBase

OOP_Object *P4Touch__Root__New(OOP_Class *cl, OOP_Object *o,
                               struct pRoot_New *msg)
{
    struct P4TouchStaticData *ptd = PTD(cl);
    struct TagItem *tag, *state;

    ObtainSemaphore(&ptd->lock);
    if (ptd->mousehidd)
    {
        ReleaseSemaphore(&ptd->lock);
        return NULL;
    }
    ReleaseSemaphore(&ptd->lock);

    o = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
    if (!o)
        return NULL;

    {
        struct P4TouchMouseData *data = OOP_INST_DATA(cl, o);

        data->ops = ptd->ops;
        data->ptd = ptd;
        state = msg->attrList;
        while ((tag = NextTagItem(&state)))
        {
            ULONG idx;
            if (IS_HIDDINPUT_ATTR(tag->ti_Tag, idx)
                && idx == aoHidd_Input_IrqHandler)
                data->callback = (InputIrqCallBack_t)tag->ti_Data;
        }
        OOP_GetAttr(o, aHidd_Input_IrqHandlerData,
                    (IPTR *)&data->callbackdata);
        if (!data->callback || !P4TouchStartWorker(data))
        {
            OOP_MethodID dispose = OOP_GetMethodID(IID_Root, moRoot_Dispose);

            bug("[P4Touch/C4] worker or input callback unavailable\n");
            OOP_DoSuperMethod(cl, o, (OOP_Msg)&dispose);
            return NULL;
        }

        ObtainSemaphore(&ptd->lock);
        ptd->mousehidd = o;
        ReleaseSemaphore(&ptd->lock);
    }
    return o;
}

VOID P4Touch__Root__Dispose(OOP_Class *cl, OOP_Object *o, OOP_Msg msg)
{
    struct P4TouchStaticData *ptd = PTD(cl);
    struct P4TouchMouseData *data = OOP_INST_DATA(cl, o);

    bug("[P4Touch/C4] driver object disposed; worker %p running %u\n",
        data->worker, (unsigned int)data->running);
    data->running = FALSE;
    if (data->worker)
        Wait(1UL << data->stopped_signal);
    if (data->stopped_signal >= 0)
    {
        FreeSignal(data->stopped_signal);
        data->stopped_signal = -1;
    }
    ObtainSemaphore(&ptd->lock);
    ptd->mousehidd = NULL;
    ReleaseSemaphore(&ptd->lock);
    OOP_DoSuperMethod(cl, o, msg);
}

VOID P4Touch__Root__Get(OOP_Class *cl, OOP_Object *o,
                        struct pRoot_Get *msg)
{
    ULONG idx;

    if (IS_HIDDMOUSE_ATTR(msg->attrID, idx))
    {
        switch (idx)
        {
        case aoHidd_Mouse_RelativeCoords:
            *msg->storage = FALSE;
            return;
        case aoHidd_Mouse_Extended:
            *msg->storage = FALSE;
            return;
        }
    }
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}
