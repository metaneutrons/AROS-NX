/*
 * Backlight: preferences editor for the panel's brightness.
 *
 * It writes ENV:Sys/backlight.prefs (Use) and ENVARC:Sys/backlight.prefs
 * (Save) in the IFF format of <prefs/backlight.h>; IPrefs applies the file
 * at boot and whenever it changes. The slider sets the backlight directly
 * while it moves, through the platform's backlight operations; Cancel puts
 * the level back.
 */

#define MUIMASTER_YES_INLINE_STDARG

#include <aros/backlight.h>
#include <aros/kernel.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/memory.h>
#include <libraries/iffparse.h>
#include <libraries/mui.h>
#include <prefs/backlight.h>
#include <prefs/prefhdr.h>
#include <proto/alib.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/iffparse.h>
#include <proto/intuition.h>
#include <proto/kernel.h>
#include <proto/muimaster.h>

#include <string.h>

static const char version[] __attribute__((used)) =
    "$VER: Backlight 1.0 (04.10.2026)";

#define ENV_DIR     "ENV:Sys"
#define ENVARC_DIR  "ENVARC:Sys"
#define ENV_FILE    "ENV:Sys/backlight.prefs"
#define ENVARC_FILE "ENVARC:Sys/backlight.prefs"

enum
{
    ID_SAVE = 1,
    ID_USE,
    ID_CANCEL,
    ID_DEFAULT,
    ID_LEVEL
};

struct Editor
{
    struct KrnBacklightOps *ops;
    ULONG start_level;              /* the ops' level when the editor opened */
    Object *app, *win, *slider;
};

static ULONG percent_from_level(struct Editor *ed, ULONG level)
{
    return (level * 100 + (ed->ops->levels - 1) / 2) / (ed->ops->levels - 1);
}

static ULONG level_from_percent(struct Editor *ed, ULONG percent)
{
    if (percent < BACKLIGHT_MIN_LEVEL)
        percent = BACKLIGHT_MIN_LEVEL;
    if (percent > 100)
        percent = 100;
    return (percent * (ed->ops->levels - 1) + 50) / 100;
}

static void editor_message(struct Editor *ed, CONST_STRPTR text)
{
    MUI_Request(ed->app, ed->win, 0, "Backlight", "_OK", (STRPTR)text,
                TAG_DONE);
}

static BOOL editor_init(struct Editor *ed)
{
    APTR KernelBase = OpenResource("kernel.resource");

    if (!KernelBase)
        return FALSE;
    ed->ops = (struct KrnBacklightOps *)KrnGetSystemAttr(KATTR_BacklightOps);
    if (!ed->ops || ed->ops == (APTR)-1
        || ed->ops->version != KRN_BACKLIGHT_OPS_VERSION
        || ed->ops->levels < 2)
        return FALSE;
    ed->start_level = ed->ops->get_level();
    return TRUE;
}

static ULONG editor_percent(struct Editor *ed)
{
    IPTR value = 0;

    get(ed->slider, MUIA_Numeric_Value, &value);
    return (ULONG)value;
}

/* FORM PREF with PRHD and BKLT, as IPrefs reads it. */
static BOOL editor_write(CONST_STRPTR dir, CONST_STRPTR path, ULONG percent)
{
    struct IFFHandle *iff;
    struct FilePrefHeader
    {
        UBYTE ph_Version;
        UBYTE ph_Type;
        UBYTE ph_Flags[4];
    } head;
    struct BacklightPrefs prefs;
    BPTR lock, file;
    BOOL ok = FALSE;

    memset(&head, 0, sizeof(head));
    head.ph_Version = PHV_CURRENT;
    memset(&prefs, 0, sizeof(prefs));
    prefs.bp_Level = AROS_WORD2BE((UWORD)percent);

    lock = CreateDir(dir);
    if (lock)
        UnLock(lock);
    file = Open(path, MODE_NEWFILE);
    if (!file)
        return FALSE;
    iff = AllocIFF();
    if (iff)
    {
        iff->iff_Stream = (IPTR)file;
        InitIFFasDOS(iff);
        if (!OpenIFF(iff, IFFF_WRITE))
        {
            ok = !PushChunk(iff, ID_PREF, ID_FORM, IFFSIZE_UNKNOWN)
              && !PushChunk(iff, ID_PREF, ID_PRHD, sizeof(head))
              && WriteChunkBytes(iff, &head, sizeof(head)) == sizeof(head)
              && !PopChunk(iff)
              && !PushChunk(iff, ID_PREF, ID_BKLT, sizeof(prefs))
              && WriteChunkBytes(iff, &prefs, sizeof(prefs)) == sizeof(prefs)
              && !PopChunk(iff)
              && !PopChunk(iff);
            CloseIFF(iff);
        }
        FreeIFF(iff);
    }
    if (!Close(file))
        ok = FALSE;
    return ok;
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

static BOOL editor_build(struct Editor *ed)
{
    Object *save, *use, *cancel, *deflt;

    ed->app = ApplicationObject,
        MUIA_Application_Title, (IPTR)"Backlight",
        MUIA_Application_Version, (IPTR)version,
        MUIA_Application_Description, (IPTR)"Backlight preferences",
        MUIA_Application_Base, (IPTR)"BACKLIGHTPREFS",
        MUIA_Application_SingleTask, TRUE,
        SubWindow, (IPTR)(ed->win = WindowObject,
            MUIA_Window_Title, (IPTR)"Backlight",
            MUIA_Window_ID, MAKE_ID('B', 'K', 'L', 'T'),
            WindowContents, (IPTR)(VGroup,
                Child, (IPTR)(HGroup,
                    GroupFrame,
                    Child, (IPTR)Label1("Brightness:"),
                    Child, (IPTR)(ed->slider = SliderObject,
                        MUIA_Numeric_Min, BACKLIGHT_MIN_LEVEL,
                        MUIA_Numeric_Max, 100,
                        MUIA_Numeric_Format, (IPTR)"%ld %%",
                        MUIA_Numeric_Value,
                            percent_from_level(ed, ed->start_level),
                        MUIA_CycleChain, TRUE,
                    End),
                    Child, (IPTR)(deflt = SimpleButton("_Default")),
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
    DoMethod(deflt, MUIM_Notify, MUIA_Pressed, FALSE,
             (IPTR)ed->app, 2, MUIM_Application_ReturnID, ID_DEFAULT);
    DoMethod(ed->slider, MUIM_Notify, MUIA_Numeric_Value, MUIV_EveryTime,
             (IPTR)ed->app, 2, MUIM_Application_ReturnID, ID_LEVEL);
    return TRUE;
}

/* TRUE when the editor should close. */
static BOOL editor_handle(struct Editor *ed, ULONG id)
{
    switch (id)
    {
    case ID_SAVE:
    case ID_USE:
        if (!editor_write(ENV_DIR, ENV_FILE, editor_percent(ed)))
        {
            editor_message(ed, "Cannot write " ENV_FILE ".");
            return FALSE;
        }
        if (id == ID_SAVE)
        {
            if (!envarc_writable())
                editor_message(ed,
                    "ENVARC: is read-only on this system.\n"
                    "The setting is in use until the next reset.");
            else if (!editor_write(ENVARC_DIR, ENVARC_FILE,
                                   editor_percent(ed)))
                editor_message(ed, "Cannot write " ENVARC_FILE ".\n"
                                   "The setting is in use until the "
                                   "next reset.");
        }
        return TRUE;

    case ID_CANCEL:
        ed->ops->set_level(ed->start_level);
        return TRUE;

    case ID_DEFAULT:
        set(ed->slider, MUIA_Numeric_Value,
            percent_from_level(ed, ed->ops->default_level));
        break;

    case ID_LEVEL:
        ed->ops->set_level(level_from_percent(ed, editor_percent(ed)));
        break;
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

    if (!editor_init(&ed))
    {
        MUI_Request(NULL, NULL, 0, "Backlight", "_OK",
                    "This system has no backlight control.", TAG_DONE);
        return RETURN_FAIL;
    }
    /* File errors are reported by the editor, not by DOS requesters (a
       write to a read-only ENVARC: would raise one). */
    me->pr_WindowPtr = (APTR)-1;
    if (!editor_build(&ed))
        goto out;
    set(ed.win, MUIA_Window_Open, TRUE);

    while (!done)
    {
        ULONG id = DoMethod(ed.app, MUIM_Application_NewInput, (IPTR)&signals);

        if (id == (ULONG)MUIV_Application_ReturnID_Quit)
        {
            ed.ops->set_level(ed.start_level);
            done = TRUE;
        }
        else if (id)
            done = editor_handle(&ed, id);
        if (!done && signals)
        {
            signals = Wait(signals | SIGBREAKF_CTRL_C);
            if (signals & SIGBREAKF_CTRL_C)
            {
                ed.ops->set_level(ed.start_level);
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
    return rc;
}
