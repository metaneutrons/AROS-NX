#ifndef ESP32P4_TOUCH_INTERN_H
#define ESP32P4_TOUCH_INTERN_H

#include <aros/touchscreen.h>
#include <dos/bptr.h>
#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <hidd/input.h>
#include <hidd/mouse.h>
#include <oop/oop.h>

#define CLID_Hidd_Mouse_P4Touch "hidd.mouse.esp32p4.touch"

struct P4TouchStaticData
{
    struct SignalSemaphore lock;
    OOP_Class *mouseclass;
    OOP_Object *mousehidd;
    struct KrnTouchScreenOps *ops;
    OOP_AttrBase hiddInputAB;
    OOP_AttrBase hiddMouseAB;
    OOP_MethodID hiddInputBase;
    struct ExecBase *cs_SysBase;
    struct Library *cs_OOPBase;
    struct Library *cs_UtilityBase;
    BPTR cs_SegList;
};

struct P4TouchBase
{
    struct Library library;
    struct P4TouchStaticData ptd;
};

struct P4TouchMouseData
{
    InputIrqCallBack_t callback;
    APTR callbackdata;
    struct KrnTouchScreenOps *ops;
    struct P4TouchStaticData *ptd;
    struct Task *owner;
    struct Task *volatile worker;
    volatile BOOL running;
    BYTE stopped_signal;
    ULONG published_events;
};

#define PTD(cl) (&((struct P4TouchBase *)(cl)->UserData)->ptd)

#undef HiddInputAB
#define HiddInputAB (PTD(cl)->hiddInputAB)
#undef HiddMouseAB
#define HiddMouseAB (PTD(cl)->hiddMouseAB)
#undef HiddInputBase
#define HiddInputBase (PTD(cl)->hiddInputBase)
BOOL P4TouchStartWorker(struct P4TouchMouseData *data);

#endif /* ESP32P4_TOUCH_INTERN_H */
