#ifndef TOUCHSCREEN_INTERN_H
#define TOUCHSCREEN_INTERN_H

#include <dos/bptr.h>
#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <hidd/input.h>
#include <hidd/mouse.h>
#include <hidd/touchscreen.h>
#include <oop/oop.h>

#include "touch_policy.h"

/*
 * What the preferences change: the calibration (starting from the measured
 * default), the gesture parameters and the mode. The prefs process
 * (touch_settings.c) and Set write them under `lock` with `generation`
 * bumped; the worker copies them under the shared lock when the generation
 * moves, so a change applies at the next poll (the gesture part at the
 * next contact) without stopping the worker.
 */
struct TouchSettings
{
    struct HIDD_TouchCalibration calibration;
    struct TouchParams params;
    unsigned int mode;              /* TOUCH_TAP or TOUCH_DIRECT */
};

struct TouchStaticData
{
    struct ExecBase *cs_SysBase;
    struct Library  *cs_OOPBase;
    struct Library  *cs_UtilityBase;
    BPTR             cs_SegList;
    OOP_Class       *touchClass;

    OOP_AttrBase     hiddInputAB;
    OOP_AttrBase     hiddMouseAB;
    OOP_AttrBase     hiddTouchScreenAB;
    OOP_AttrBase     hiddTouchControllerAB;

    /* IID_Hidd_TouchController methods, resolved by the first New: the
       interface exists only once a controller class is registered. */
    OOP_MethodID     midStart;
    OOP_MethodID     midStop;
    OOP_MethodID     midReadFrame;
};

struct TouchBase
{
    struct TouchScreenBase pub;     /* public: see <hidd/touchscreen.h> */
    struct TouchStaticData tsd;
};

#define TOUCH_NAME_MAX 32

/* One driver object: one controller, one touch surface. */
struct TouchData
{
    struct TouchScreenDriverNode node;  /* in the base's tsb_Drivers */
    struct TouchStaticData *tsd;
    InputIrqCallBack_t      callback;
    APTR                    callbackdata;

    OOP_Object             *controller;
    char                    name[TOUCH_NAME_MAX];
    ULONG                   width, height;
    struct HIDD_TouchCalibration default_cal;
    struct HIDD_TouchCalibration snapshot;     /* what Get hands out */
    CONST_STRPTR const     *firmware_paths;

    struct SignalSemaphore  lock;
    struct TouchSettings    settings;
    volatile ULONG          generation;

    /* The polling worker (touch_worker.c). */
    struct Task            *owner;
    struct Task * volatile  worker;
    volatile BOOL           running;
    BYTE                    stopped_signal;
    ULONG                   published_events;

    /* The prefs process (touch_settings.c), started by the worker. */
    struct Task            *prefs;
    struct Task            *prefs_owner;
    volatile BOOL           prefs_alive;
    BYTE                    prefs_signal;
};

#define TSD(cl) (&((struct TouchBase *)(cl)->UserData)->tsd)
#define TSB(cl) (&((struct TouchBase *)(cl)->UserData)->pub)

#undef HiddInputAB
#define HiddInputAB (TSD(cl)->hiddInputAB)
#undef HiddMouseAB
#define HiddMouseAB (TSD(cl)->hiddMouseAB)
#undef HiddTouchScreenAttrBase
#define HiddTouchScreenAttrBase (TSD(cl)->hiddTouchScreenAB)
#undef HiddTouchControllerAttrBase
#define HiddTouchControllerAttrBase (TSD(cl)->hiddTouchControllerAB)

BOOL Touch_StartWorker(struct TouchData *data);
struct DosLibrary;
BOOL Touch_StartPrefs(struct TouchData *data, struct DosLibrary *DOSBase);
void Touch_StopPrefs(struct TouchData *data);

#endif /* TOUCHSCREEN_INTERN_H */
