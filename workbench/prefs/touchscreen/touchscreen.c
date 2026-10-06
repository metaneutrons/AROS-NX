/*
 * Touchscreen: preferences editor for touchscreen.hidd.
 *
 * It writes ENV:Sys/touchscreen.prefs (Use) and ENVARC:Sys/touchscreen.prefs
 * (Save); the driver applies the file at boot and whenever it changes.
 * While calibrating, and to show the result straight away, the editor sets
 * the driver's calibration directly; Cancel puts the previous one back.
 */

#define MUIMASTER_YES_INLINE_STDARG

#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/memory.h>
#include <hidd/touchscreen.h>
#include <libraries/mui.h>
#include <oop/oop.h>
#include <proto/alib.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <proto/oop.h>

#include <stdio.h>
#include <string.h>

#include "touchscreen.h"

static const char version[] __attribute__((used)) =
    "$VER: Touchscreen 1.1 (04.10.2026)";

#define ENV_DIR     "ENV:Sys"
#define ENVARC_DIR  "ENVARC:Sys"
#define FILE_BYTES  2048

OOP_AttrBase HiddTouchScreenAttrBase;

enum
{
    ID_SAVE = 1,
    ID_USE,
    ID_CANCEL,
    ID_CALIBRATE,
    ID_CAL_DEFAULT,
    ID_GESTURE_DEFAULTS,
    ID_TAPDRAG
};

struct Editor
{
    struct TouchScreenBase *base;
    OOP_Object *driver;
    char name[32];             /* calibration key */
    ULONG width, height;
    struct TouchCal default_cal;
    struct TouchCal start_cal;      /* in effect when the editor opened */
    struct TouchPrefs prefs;
    char keep[FILE_BYTES];          /* the file as found, for other names */
    LONG keep_length;

    Object *app, *win, *cal_text, *mode, *hold, *slop, *release;
    Object *tapdrag_on, *tapdrag_ms, *tapdrag_px, *two_finger;
};

static const char *pages[] = {"Calibration", "Gestures", NULL};
static const char *modes[] =
{
    "Tap to click, hold to drag",
    "Touch presses the button",
    NULL
};

static void cal_from_hidd(struct TouchCal *out,
                          const struct HIDD_TouchCalibration *in)
{
    out->x_min = in->x_min;
    out->x_max = in->x_max;
    out->y_min = in->y_min;
    out->y_max = in->y_max;
    out->flags = in->flags;
}

static void cal_to_hidd(struct HIDD_TouchCalibration *out,
                        const struct TouchCal *in)
{
    out->x_min = in->x_min;
    out->x_max = in->x_max;
    out->y_min = in->y_min;
    out->y_max = in->y_max;
    out->flags = in->flags;
}

static void editor_message(struct Editor *ed, CONST_STRPTR text)
{
    MUI_Request(ed->app, ed->win, 0, "Touchscreen", "_OK", (STRPTR)text,
                TAG_DONE);
}

/* Put a calibration into effect in the driver now. */
static void editor_apply_cal(struct Editor *ed, const struct TouchCal *cal)
{
    struct HIDD_TouchCalibration c;

    cal_to_hidd(&c, cal);
    OOP_SetAttrsTags(ed->driver, aHidd_TouchScreen_Calibration, (IPTR)&c,
                     TAG_DONE);
}

static void editor_defaults(struct Editor *ed, struct TouchPrefs *p)
{
    p->have = 0;
    p->cal = ed->default_cal;
    touch_params_default(&p->params);
    p->mode = TOUCH_TAP;
}

/* The first touchscreen driver; the base's list is documented in
   <hidd/touchscreen.h>. */
static BOOL editor_find(struct Editor *ed)
{
    struct TouchScreenDriverNode *node;
    IPTR value = 0;

    ed->base = (struct TouchScreenBase *)OpenLibrary(TOUCHSCREEN_NAME, 1);
    if (!ed->base)
        return FALSE;
    ObtainSemaphoreShared(&ed->base->tsb_Lock);
    node = (struct TouchScreenDriverNode *)ed->base->tsb_Drivers.mlh_Head;
    if (node->tdn_Node.mln_Succ)
        ed->driver = node->tdn_Driver;
    ReleaseSemaphore(&ed->base->tsb_Lock);
    if (!ed->driver)
        return FALSE;

    OOP_GetAttr(ed->driver, aHidd_TouchScreen_Name, &value);
    strncpy(ed->name, (const char *)value, sizeof(ed->name) - 1);
    OOP_GetAttr(ed->driver, aHidd_TouchScreen_Width, &value);
    ed->width = (ULONG)value;
    OOP_GetAttr(ed->driver, aHidd_TouchScreen_Height, &value);
    ed->height = (ULONG)value;
    OOP_GetAttr(ed->driver, aHidd_TouchScreen_DefaultCalibration, &value);
    cal_from_hidd(&ed->default_cal,
                  (const struct HIDD_TouchCalibration *)value);
    OOP_GetAttr(ed->driver, aHidd_TouchScreen_Calibration, &value);
    cal_from_hidd(&ed->start_cal,
                  (const struct HIDD_TouchCalibration *)value);
    return ed->name[0] && ed->width && ed->height;
}

static void editor_load(struct Editor *ed)
{
    BPTR file;

    editor_defaults(ed, &ed->prefs);
    /* ENV: falls through to ENVARC: when nothing was used this session. */
    file = Open(TOUCH_PREFS_PATH, MODE_OLDFILE);
    if (file)
    {
        LONG got = Read(file, ed->keep, sizeof(ed->keep) - 1);

        if (got > 0)
            ed->keep_length = got;
        Close(file);
    }
    if (ed->keep_length)
        touch_prefs_parse(&ed->prefs, ed->keep, (size_t)ed->keep_length,
                          ed->name);
    /* What the driver uses wins over the file: they differ only while
       another program calibrates. */
    ed->prefs.cal = ed->start_cal;
}

static BOOL editor_write(struct Editor *ed, CONST_STRPTR dir,
                         CONST_STRPTR path, const struct TouchPrefs *p)
{
    char buf[FILE_BYTES];
    struct TouchPrefs out = *p;
    size_t length;
    BPTR lock, file;
    LONG written;

    out.have = TOUCH_PREFS_HAVE_MODE | TOUCH_PREFS_HAVE_PARAMS;
    /* The default needs no line, so a corrected default in a later
       driver still applies. */
    if (memcmp(&p->cal, &ed->default_cal, sizeof(p->cal)))
        out.have |= TOUCH_PREFS_HAVE_CAL;
    length = touch_prefs_format(&out, ed->name, ed->keep,
                                (size_t)ed->keep_length, buf, sizeof(buf));
    if (!length)
        return FALSE;
    lock = CreateDir(dir);
    if (lock)
        UnLock(lock);
    file = Open(path, MODE_NEWFILE);
    if (!file)
        return FALSE;
    written = Write(file, buf, (LONG)length);
    if (!Close(file))
        return FALSE;
    return written == (LONG)length;
}

static BOOL envarc_writable(void)
{
    struct InfoData *info = AllocVec(sizeof(*info), MEMF_ANY);
    BPTR lock = Lock("ENVARC:", SHARED_LOCK);
    BOOL writable = FALSE;

    if (info && lock && Info(lock, info))
        writable = info->id_DiskState == ID_VALIDATED;
    if (lock)
        UnLock(lock);
    FreeVec(info);
    return writable;
}

static void editor_show_cal(struct Editor *ed)
{
    const struct TouchCal *c = &ed->prefs.cal;
    char text[256];
    char orientation[48];

    orientation[0] = 0;
    if (c->flags & TOUCH_PREFS_SWAP_XY)
        strcat(orientation, "axes swapped, ");
    if (c->flags & TOUCH_PREFS_MIRROR_X)
        strcat(orientation, "X mirrored, ");
    if (c->flags & TOUCH_PREFS_MIRROR_Y)
        strcat(orientation, "Y mirrored, ");
    if (!orientation[0])
        strcpy(orientation, "as mounted, ");
    orientation[strlen(orientation) - 2] = 0;
    snprintf(text, sizeof(text),
             "\33cTouchscreen: %s\n"
             "Raw range X %lu-%lu, Y %lu-%lu\n"
             "Orientation: %s\n"
             "%s",
             ed->name,
             (unsigned long)c->x_min, (unsigned long)c->x_max,
             (unsigned long)c->y_min, (unsigned long)c->y_max,
             orientation,
             memcmp(c, &ed->default_cal, sizeof(*c))
                 ? "Calibrated"
                 : "Measured default");
    set(ed->cal_text, MUIA_Text_Contents, (IPTR)text);
}

static void editor_show(struct Editor *ed)
{
    const struct TouchParams *pp = &ed->prefs.params;

    set(ed->mode, MUIA_Cycle_Active, ed->prefs.mode == TOUCH_DIRECT);
    set(ed->hold, MUIA_Numeric_Value, pp->hold_ms);
    set(ed->slop, MUIA_Numeric_Value, pp->slop_px);
    set(ed->release, MUIA_Numeric_Value, pp->release_polls);
    set(ed->tapdrag_on, MUIA_Selected, pp->tapdrag_ms != 0);
    set(ed->tapdrag_ms, MUIA_Numeric_Value,
        pp->tapdrag_ms ? pp->tapdrag_ms : TOUCH_TAPDRAG_MS);
    set(ed->tapdrag_px, MUIA_Numeric_Value, pp->tapdrag_px);
    set(ed->tapdrag_ms, MUIA_Disabled, pp->tapdrag_ms == 0);
    set(ed->tapdrag_px, MUIA_Disabled, pp->tapdrag_ms == 0);
    set(ed->two_finger, MUIA_Selected, pp->two_finger_right != 0);
    editor_show_cal(ed);
}

static ULONG get_value(Object *obj, ULONG attribute)
{
    IPTR value = 0;

    get(obj, attribute, &value);
    return (ULONG)value;
}

static void editor_collect(struct Editor *ed)
{
    struct TouchParams *pp = &ed->prefs.params;

    ed->prefs.mode = get_value(ed->mode, MUIA_Cycle_Active)
                   ? TOUCH_DIRECT : TOUCH_TAP;
    pp->hold_ms = get_value(ed->hold, MUIA_Numeric_Value);
    pp->slop_px = (int32_t)get_value(ed->slop, MUIA_Numeric_Value);
    pp->release_polls = get_value(ed->release, MUIA_Numeric_Value);
    pp->tapdrag_ms = get_value(ed->tapdrag_on, MUIA_Selected)
                   ? get_value(ed->tapdrag_ms, MUIA_Numeric_Value) : 0;
    pp->tapdrag_px = (int32_t)get_value(ed->tapdrag_px, MUIA_Numeric_Value);
    pp->two_finger_right = get_value(ed->two_finger, MUIA_Selected) != 0;
}

static void editor_calibrate(struct Editor *ed)
{
    struct TouchCal fitted;
    struct Screen *screen = NULL;
    int result;

    /* Map with the measured default while the targets are touched, so a
       bad calibration cannot keep a target out of reach. The driver takes
       it at its next poll, well before the first target is reached. */
    editor_apply_cal(ed, &ed->default_cal);

    get(ed->win, MUIA_Window_Screen, &screen);
    set(ed->app, MUIA_Application_Sleep, TRUE);
    result = Touch_Calibrate(screen, &ed->default_cal, ed->width,
                             ed->height, &fitted);
    set(ed->app, MUIA_Application_Sleep, FALSE);

    if (result == TOUCH_CAL_OK)
        ed->prefs.cal = fitted;
    /* The new calibration, or the previous one again, takes effect now. */
    editor_apply_cal(ed, &ed->prefs.cal);
    editor_show_cal(ed);
    if (result > 0)
        editor_message(ed,
            result == TOUCH_CAL_SPREAD
                ? "The touches were too close together.\n"
                  "The previous calibration stays."
            : result == TOUCH_CAL_AXES
                ? "The touches did not match the four corners.\n"
                  "The previous calibration stays."
                : "The result is outside the controller's range.\n"
                  "The previous calibration stays.");
}

static Object *slider(LONG min, LONG max, CONST_STRPTR format)
{
    return SliderObject,
        MUIA_Numeric_Min, min,
        MUIA_Numeric_Max, max,
        MUIA_Numeric_Format, (IPTR)format,
        MUIA_CycleChain, TRUE,
    End;
}

static Object *checkmark(void)
{
    Object *obj = MUI_MakeObject(MUIO_Checkmark, NULL);

    if (obj)
        set(obj, MUIA_CycleChain, TRUE);
    return obj;
}

static BOOL editor_build(struct Editor *ed)
{
    Object *save, *use, *cancel, *calibrate, *cal_default;
    Object *gesture_defaults;

    ed->app = ApplicationObject,
        MUIA_Application_Title, (IPTR)"Touchscreen",
        MUIA_Application_Version, (IPTR)version,
        MUIA_Application_Description, (IPTR)"Touchscreen preferences",
        MUIA_Application_Base, (IPTR)"TOUCHSCREENPREFS",
        MUIA_Application_SingleTask, TRUE,
        SubWindow, (IPTR)(ed->win = WindowObject,
            MUIA_Window_Title, (IPTR)"Touchscreen",
            MUIA_Window_ID, MAKE_ID('T', 'S', 'C', 'R'),
            WindowContents, (IPTR)(VGroup,
                Child, (IPTR)(RegisterGroup(pages),
                    MUIA_Register_Frame, TRUE,

                    Child, (IPTR)(VGroup,
                        Child, (IPTR)(ed->cal_text = TextObject,
                            TextFrame,
                            MUIA_Background, MUII_TextBack,
                        End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)(calibrate =
                                SimpleButton("_Calibrate...")),
                            Child, (IPTR)(cal_default =
                                SimpleButton("_Measured default")),
                        End),
                        Child, (IPTR)VSpace(0),
                    End),

                    Child, (IPTR)(ColGroup(2),
                        Child, (IPTR)Label1("Touch mode:"),
                        Child, (IPTR)(ed->mode = CycleObject,
                            MUIA_Cycle_Entries, (IPTR)modes,
                            MUIA_CycleChain, TRUE,
                        End),
                        Child, (IPTR)Label1("Hold to drag:"),
                        Child, (IPTR)(ed->hold = slider(100, 3000, "%ld ms")),
                        Child, (IPTR)Label1("Tap tolerance:"),
                        Child, (IPTR)(ed->slop = slider(1, 64, "%ld px")),
                        Child, (IPTR)Label1("Release after:"),
                        Child, (IPTR)(ed->release = slider(1, 10, "%ld polls")),
                        Child, (IPTR)Label1("Double-tap and drag:"),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)(ed->tapdrag_on = checkmark()),
                            Child, (IPTR)HSpace(0),
                        End),
                        Child, (IPTR)Label1("Second tap within:"),
                        Child, (IPTR)(ed->tapdrag_ms = slider(50, 1000, "%ld ms")),
                        Child, (IPTR)Label1("and within:"),
                        Child, (IPTR)(ed->tapdrag_px = slider(1, 256, "%ld px")),
                        Child, (IPTR)Label1("Two fingers, right button:"),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)(ed->two_finger = checkmark()),
                            Child, (IPTR)HSpace(0),
                        End),
                        Child, (IPTR)HSpace(0),
                        Child, (IPTR)(gesture_defaults =
                            SimpleButton("_Defaults")),
                    End),
                End),
                Child, (IPTR)(HGroup,
                    Child, (IPTR)(save = SimpleButton("_Save")),
                    Child, (IPTR)HSpace(0),
                    Child, (IPTR)(use = SimpleButton("_Use")),
                    Child, (IPTR)HSpace(0),
                    Child, (IPTR)(cancel = SimpleButton("_Cancel")),
                End),
            End),
        End),
    End;
    if (!ed->app)
        return FALSE;

    DoMethod(ed->win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             (IPTR)ed->app, 2, MUIM_Application_ReturnID, ID_CANCEL);
    DoMethod(save, MUIM_Notify, MUIA_Pressed, FALSE,
             (IPTR)ed->app, 2, MUIM_Application_ReturnID, ID_SAVE);
    DoMethod(use, MUIM_Notify, MUIA_Pressed, FALSE,
             (IPTR)ed->app, 2, MUIM_Application_ReturnID, ID_USE);
    DoMethod(cancel, MUIM_Notify, MUIA_Pressed, FALSE,
             (IPTR)ed->app, 2, MUIM_Application_ReturnID, ID_CANCEL);
    DoMethod(calibrate, MUIM_Notify, MUIA_Pressed, FALSE,
             (IPTR)ed->app, 2, MUIM_Application_ReturnID, ID_CALIBRATE);
    DoMethod(cal_default, MUIM_Notify, MUIA_Pressed, FALSE,
             (IPTR)ed->app, 2, MUIM_Application_ReturnID, ID_CAL_DEFAULT);
    DoMethod(gesture_defaults, MUIM_Notify, MUIA_Pressed, FALSE,
             (IPTR)ed->app, 2, MUIM_Application_ReturnID,
             ID_GESTURE_DEFAULTS);
    DoMethod(ed->tapdrag_on, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             (IPTR)ed->app, 2, MUIM_Application_ReturnID, ID_TAPDRAG);
    return TRUE;
}

/* TRUE when the editor should close. */
static BOOL editor_handle(struct Editor *ed, ULONG id)
{
    switch (id)
    {
    case ID_SAVE:
    case ID_USE:
        editor_collect(ed);
        if (!editor_write(ed, ENV_DIR, TOUCH_PREFS_PATH, &ed->prefs))
        {
            editor_message(ed, "Cannot write " TOUCH_PREFS_PATH ".");
            return FALSE;
        }
        if (id == ID_SAVE)
        {
            if (!envarc_writable())
                editor_message(ed,
                    "ENVARC: is read-only on this system.\n"
                    "The settings are in use until the next reset.");
            else if (!editor_write(ed, ENVARC_DIR, TOUCH_PREFS_ENVARC,
                                   &ed->prefs))
                editor_message(ed, "Cannot write " TOUCH_PREFS_ENVARC ".\n"
                                   "The settings are in use until the "
                                   "next reset.");
        }
        return TRUE;

    case ID_CANCEL:
        editor_apply_cal(ed, &ed->start_cal);
        return TRUE;

    case ID_CALIBRATE:
        editor_calibrate(ed);
        break;

    case ID_CAL_DEFAULT:
        ed->prefs.cal = ed->default_cal;
        editor_apply_cal(ed, &ed->prefs.cal);
        editor_show_cal(ed);
        break;

    case ID_GESTURE_DEFAULTS:
        editor_collect(ed);
        touch_params_default(&ed->prefs.params);
        ed->prefs.mode = TOUCH_TAP;
        editor_show(ed);
        break;

    case ID_TAPDRAG:
    {
        BOOL off = !get_value(ed->tapdrag_on, MUIA_Selected);

        set(ed->tapdrag_ms, MUIA_Disabled, off);
        set(ed->tapdrag_px, MUIA_Disabled, off);
        break;
    }
    }
    return FALSE;
}

int main(void)
{
    static struct Editor ed;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR window_ptr = me->pr_WindowPtr;
    ULONG signals = 0;
    BOOL done = FALSE;
    int rc = RETURN_FAIL;

    HiddTouchScreenAttrBase = OOP_ObtainAttrBase(IID_Hidd_TouchScreen);
    if (!HiddTouchScreenAttrBase || !editor_find(&ed))
    {
        MUI_Request(NULL, NULL, 0, "Touchscreen", "_OK",
                    "This system has no touchscreen driver.", TAG_DONE);
        goto out;
    }

    /* File errors are reported by the editor, not by DOS requesters (a
       write that falls through to read-only ENVARC: would raise one). */
    me->pr_WindowPtr = (APTR)-1;
    editor_load(&ed);
    if (!editor_build(&ed))
        goto out;
    editor_show(&ed);
    set(ed.win, MUIA_Window_Open, TRUE);

    while (!done)
    {
        ULONG id = DoMethod(ed.app, MUIM_Application_NewInput, (IPTR)&signals);

        if (id == (ULONG)MUIV_Application_ReturnID_Quit)
        {
            editor_apply_cal(&ed, &ed.start_cal);
            done = TRUE;
        }
        else if (id)
            done = editor_handle(&ed, id);
        if (!done && signals)
        {
            signals = Wait(signals | SIGBREAKF_CTRL_C);
            if (signals & SIGBREAKF_CTRL_C)
            {
                editor_apply_cal(&ed, &ed.start_cal);
                done = TRUE;
            }
        }
    }
    set(ed.win, MUIA_Window_Open, FALSE);
    rc = RETURN_OK;

out:
    if (ed.app)
        MUI_DisposeObject(ed.app);
    me->pr_WindowPtr = window_ptr;
    if (HiddTouchScreenAttrBase)
        OOP_ReleaseAttrBase(IID_Hidd_TouchScreen);
    if (ed.base)
        CloseLibrary((struct Library *)ed.base);
    return rc;
}
