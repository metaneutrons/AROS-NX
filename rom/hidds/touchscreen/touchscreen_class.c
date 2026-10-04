/* touchscreen.hidd: the pointing device driver class. */

#include <aros/debug.h>
#include <proto/exec.h>
#include <proto/oop.h>
#include <proto/utility.h>
#include <utility/tagitem.h>

#include "touchscreen_intern.h"

#define SysBase ((struct ExecBase *)TSD(cl)->cs_SysBase)
#define OOPBase TSD(cl)->cs_OOPBase
#define UtilityBase TSD(cl)->cs_UtilityBase

static BOOL touch_cal_valid(const struct HIDD_TouchCalibration *cal)
{
    return cal && cal->x_min < cal->x_max && cal->y_min < cal->y_max
        && cal->x_max <= 65535 && cal->y_max <= 65535
        && !(cal->flags & ~(vHidd_TouchCal_SwapXY | vHidd_TouchCal_MirrorX
                            | vHidd_TouchCal_MirrorY));
}

OOP_Object *Touch__Root__New(OOP_Class *cl, OOP_Object *o,
                             struct pRoot_New *msg)
{
    OOP_Object *controller;
    const struct HIDD_TouchCalibration *cal;
    CONST_STRPTR name;
    struct TagItem *tag, *state;
    ULONG width, height, i;

    controller = (OOP_Object *)GetTagData(aHidd_TouchScreen_Controller, 0,
                                          msg->attrList);
    name = (CONST_STRPTR)GetTagData(aHidd_TouchScreen_Name, 0, msg->attrList);
    width = GetTagData(aHidd_TouchScreen_Width, 0, msg->attrList);
    height = GetTagData(aHidd_TouchScreen_Height, 0, msg->attrList);
    cal = (const struct HIDD_TouchCalibration *)
        GetTagData(aHidd_TouchScreen_DefaultCalibration, 0, msg->attrList);
    if (!controller || !name || !name[0] || width < 2 || height < 2
        || width > 32768 || height > 32768 || !touch_cal_valid(cal))
    {
        bug("[Touch] refused: controller %p, name %s, %lux%lu, "
            "calibration %s\n", controller, name ? (const char *)name : "(none)",
            (unsigned long)width, (unsigned long)height,
            touch_cal_valid(cal) ? "ok" : "missing or invalid");
        return NULL;
    }

    if (!TSD(cl)->midReadFrame || TSD(cl)->midReadFrame == (OOP_MethodID)-1)
    {
        TSD(cl)->midStart = OOP_GetMethodID(IID_Hidd_TouchController,
                                            moHidd_TouchController_Start);
        TSD(cl)->midStop = OOP_GetMethodID(IID_Hidd_TouchController,
                                           moHidd_TouchController_Stop);
        TSD(cl)->midReadFrame = OOP_GetMethodID(
            IID_Hidd_TouchController, moHidd_TouchController_ReadFrame);
        if (TSD(cl)->midReadFrame == (OOP_MethodID)-1)
        {
            bug("[Touch] no class implements %s\n",
                IID_Hidd_TouchController);
            return NULL;
        }
    }

    o = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
    if (!o)
        return NULL;

    {
        struct TouchData *data = OOP_INST_DATA(cl, o);

        data->tsd = TSD(cl);
        data->controller = controller;
        for (i = 0; name[i] && i < TOUCH_NAME_MAX - 1; ++i)
            data->name[i] = name[i];
        data->name[i] = 0;
        data->width = width;
        data->height = height;
        data->default_cal = *cal;
        data->firmware_paths = (CONST_STRPTR const *)
            GetTagData(aHidd_TouchScreen_FirmwarePaths, 0, msg->attrList);
        InitSemaphore(&data->lock);
        data->settings.calibration = *cal;
        touch_params_default(&data->settings.params);
        data->settings.mode = TOUCH_TAP;
        data->generation = 1;
        data->stopped_signal = -1;
        data->prefs_signal = -1;

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
        if (!data->callback || !Touch_StartWorker(data))
        {
            OOP_MethodID dispose = OOP_GetMethodID(IID_Root, moRoot_Dispose);

            bug("[Touch] %s: worker or input callback unavailable\n",
                data->name);
            /* The controller stays the caller's when creation fails. */
            data->controller = NULL;
            OOP_DoSuperMethod(cl, o, (OOP_Msg)&dispose);
            return NULL;
        }
        data->node.tdn_Driver = o;
        ObtainSemaphore(&TSB(cl)->tsb_Lock);
        AddTail((struct List *)&TSB(cl)->tsb_Drivers,
                (struct Node *)&data->node.tdn_Node);
        ReleaseSemaphore(&TSB(cl)->tsb_Lock);
        bug("[Touch] %s: %lux%lu, default calibration X %lu..%lu "
            "Y %lu..%lu flags 0x%lx\n", data->name,
            (unsigned long)width, (unsigned long)height,
            (unsigned long)cal->x_min, (unsigned long)cal->x_max,
            (unsigned long)cal->y_min, (unsigned long)cal->y_max,
            (unsigned long)cal->flags);
    }
    return o;
}

VOID Touch__Root__Dispose(OOP_Class *cl, OOP_Object *o, OOP_Msg msg)
{
    struct TouchData *data = OOP_INST_DATA(cl, o);

    bug("[Touch] %s: disposed; worker %p running %u\n", data->name,
        data->worker, (unsigned int)data->running);
    ObtainSemaphore(&TSB(cl)->tsb_Lock);
    Remove((struct Node *)&data->node.tdn_Node);
    ReleaseSemaphore(&TSB(cl)->tsb_Lock);
    data->running = FALSE;
    if (data->worker)
        Wait(1UL << data->stopped_signal);
    if (data->stopped_signal >= 0)
    {
        FreeSignal(data->stopped_signal);
        data->stopped_signal = -1;
    }
    if (data->controller)
        OOP_DisposeObject(data->controller);
    OOP_DoSuperMethod(cl, o, msg);
}

VOID Touch__Root__Get(OOP_Class *cl, OOP_Object *o, struct pRoot_Get *msg)
{
    struct TouchData *data = OOP_INST_DATA(cl, o);
    ULONG idx;

    if (IS_TOUCHSCREEN_ATTR(msg->attrID, idx))
    {
        switch (idx)
        {
        case aoHidd_TouchScreen_Controller:
            *msg->storage = (IPTR)data->controller;
            return;
        case aoHidd_TouchScreen_Name:
            *msg->storage = (IPTR)data->name;
            return;
        case aoHidd_TouchScreen_Width:
            *msg->storage = data->width;
            return;
        case aoHidd_TouchScreen_Height:
            *msg->storage = data->height;
            return;
        case aoHidd_TouchScreen_DefaultCalibration:
            *msg->storage = (IPTR)&data->default_cal;
            return;
        case aoHidd_TouchScreen_Calibration:
            ObtainSemaphore(&data->lock);
            data->snapshot = data->settings.calibration;
            ReleaseSemaphore(&data->lock);
            *msg->storage = (IPTR)&data->snapshot;
            return;
        }
    }
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

VOID Touch__Root__Set(OOP_Class *cl, OOP_Object *o, struct pRoot_Set *msg)
{
    struct TouchData *data = OOP_INST_DATA(cl, o);
    struct TagItem *tag, *state = msg->attrList;

    while ((tag = NextTagItem(&state)))
    {
        ULONG idx;

        if (IS_TOUCHSCREEN_ATTR(tag->ti_Tag, idx)
            && idx == aoHidd_TouchScreen_Calibration)
        {
            const struct HIDD_TouchCalibration *cal =
                (const struct HIDD_TouchCalibration *)tag->ti_Data;

            if (!touch_cal_valid(cal))
            {
                bug("[Touch] %s: invalid calibration ignored\n", data->name);
                continue;
            }
            ObtainSemaphore(&data->lock);
            data->settings.calibration = *cal;
            ++data->generation;
            ReleaseSemaphore(&data->lock);
        }
    }
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}
