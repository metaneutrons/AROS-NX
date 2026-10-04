/*
 * touchscreen.hidd: a small DOS process applies ENV:Sys/touchscreen.prefs
 * to one driver's settings, once ENV: exists and again whenever the file
 * changes.
 *
 * DOS file calls need a Process (they read pr_WindowPtr and friends); the
 * polling worker is a plain task and must not block on file I/O anyway, so
 * the worker only starts and stops this process.
 */

#include <aros/asmcall.h>
#include <aros/debug.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/notify.h>
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include "touchscreen_intern.h"
#include "touch_prefs.h"

#define TOUCH_PREFS_BYTES     2048
/* Ticks (1/50 s) between looks for the ENV: assign during boot. */
#define TOUCH_ENV_POLL_TICKS  50
/* A writer may create, write and close in several steps, each of which
   notifies; reading after a short quiet time sees the finished file. */
#define TOUCH_SETTLE_TICKS    10

static BOOL touch_env_present(struct DosLibrary *DOSBase)
{
    struct DosList *list;
    BOOL found;

    list = LockDosList(LDF_ASSIGNS | LDF_DEVICES | LDF_VOLUMES | LDF_READ);
    found = FindDosEntry(list, "ENV",
                         LDF_ASSIGNS | LDF_DEVICES | LDF_VOLUMES) != NULL;
    UnLockDosList(LDF_ASSIGNS | LDF_DEVICES | LDF_VOLUMES | LDF_READ);
    return found;
}

/* The file, or nothing; a missing file means defaults. */
static LONG touch_read_prefs(struct DosLibrary *DOSBase, char *buf,
                               LONG size)
{
    BPTR file = Open((CONST_STRPTR)TOUCH_PREFS_PATH, MODE_OLDFILE);
    LONG got;

    if (!file)
        return 0;
    /* See touch_read_firmware(): a failed FAT open may hand back a
       handle without a handler port. */
    if (!((struct FileHandle *)BADDR(file))->fh_Type)
    {
        Close(file);
        return 0;
    }
    got = Read(file, buf, size);
    Close(file);
    return got > 0 ? got : 0;
}

static void touch_apply_prefs(struct TouchData *data,
                              struct DosLibrary *DOSBase, char *buf)
{
    struct ExecBase *SysBase = data->tsd->cs_SysBase;
    const struct HIDD_TouchCalibration *def = &data->default_cal;
    struct TouchPrefs prefs;
    LONG length;

    prefs.cal.x_min = def->x_min;
    prefs.cal.x_max = def->x_max;
    prefs.cal.y_min = def->y_min;
    prefs.cal.y_max = def->y_max;
    prefs.cal.flags = def->flags;
    touch_params_default(&prefs.params);
    prefs.mode = TOUCH_TAP;
    prefs.have = 0;

    length = touch_read_prefs(DOSBase, buf, TOUCH_PREFS_BYTES);
    if (length)
        touch_prefs_parse(&prefs, buf, (size_t)length, data->name);

    ObtainSemaphore(&data->lock);
    data->settings.calibration.x_min = prefs.cal.x_min;
    data->settings.calibration.x_max = prefs.cal.x_max;
    data->settings.calibration.y_min = prefs.cal.y_min;
    data->settings.calibration.y_max = prefs.cal.y_max;
    data->settings.calibration.flags = prefs.cal.flags;
    data->settings.params = prefs.params;
    data->settings.mode = prefs.mode;
    ++data->generation;
    ReleaseSemaphore(&data->lock);

    bug("[Touch] %s: prefs %s, %ld bytes, found 0x%x\n", data->name,
        TOUCH_PREFS_PATH, (long)length, prefs.have);
}

AROS_UFH3S(ULONG, Touch_PrefsEntry,
          AROS_UFHA(STRPTR, argptr, A0),
          AROS_UFHA(ULONG, argsize, D0),
          AROS_UFHA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT

    struct Task *self = FindTask(NULL);
    struct TouchData *data = self->tc_UserData;
    struct DosLibrary *DOSBase;
    struct NotifyRequest notify;
    BOOL notifying = FALSE;
    BYTE notify_signal = -1;
    char *buf = NULL;

    (void)argptr;
    (void)argsize;

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    if (DOSBase)
        buf = AllocVec(TOUCH_PREFS_BYTES, MEMF_ANY);
    notify_signal = AllocSignal(-1);
    if (!DOSBase || !buf || notify_signal < 0)
    {
        bug("[Touch] %s: prefs process cannot start; defaults stay\n",
            data->name);
        goto out;
    }

    /* ENV: comes from the Startup-Sequence; until then there is nothing
       to read, and asking DOS for a missing assign would raise a
       requester. */
    while (!(SetSignal(0, 0) & SIGBREAKF_CTRL_C)
           && !touch_env_present(DOSBase))
        Delay(TOUCH_ENV_POLL_TICKS);
    /* A Startup-Sequence may assign ENV: to RAM: first and add ENVARC:
       to it a line later; a read in between would miss a saved file. */
    Delay(TOUCH_ENV_POLL_TICKS);

    if (!(SetSignal(0, 0) & SIGBREAKF_CTRL_C))
    {
        /* RAM: notifies even for a file that does not exist yet. */
        notify.nr_Name = (STRPTR)TOUCH_PREFS_PATH;
        notify.nr_Flags = NRF_SEND_SIGNAL;
        notify.nr_stuff.nr_Signal.nr_Task = self;
        notify.nr_stuff.nr_Signal.nr_SignalNum = notify_signal;
        notifying = StartNotify(&notify);
        if (!notifying)
            bug("[Touch] %s: cannot watch %s (error %ld); prefs are read "
                "once\n", data->name, TOUCH_PREFS_PATH, (long)IoErr());
        touch_apply_prefs(data, DOSBase, buf);
    }

    while (notifying)
    {
        ULONG got = Wait((1UL << notify_signal) | SIGBREAKF_CTRL_C);

        if (got & SIGBREAKF_CTRL_C)
            break;
        Delay(TOUCH_SETTLE_TICKS);
        SetSignal(0, 1UL << notify_signal);
        touch_apply_prefs(data, DOSBase, buf);
    }

out:
    if (notifying)
        EndNotify(&notify);
    if (notify_signal >= 0)
        FreeSignal(notify_signal);
    if (buf)
        FreeVec(buf);
    if (DOSBase)
        CloseLibrary((struct Library *)DOSBase);

    /* The worker signals this process only while it sees prefs_alive
       under Forbid(), so the flag must drop before the process goes. */
    Forbid();
    data->prefs_alive = FALSE;
    Signal(data->prefs_owner, 1UL << data->prefs_signal);
    return 0;

    AROS_USERFUNC_EXIT
}

BOOL Touch_StartPrefs(struct TouchData *data, struct DosLibrary *DOSBase)
{
    struct ExecBase *SysBase = data->tsd->cs_SysBase;
    struct Process *proc;

    data->prefs_owner = FindTask(NULL);
    data->prefs_signal = AllocSignal(-1);
    if (data->prefs_signal < 0)
        return FALSE;
    data->prefs_alive = TRUE;
    proc = CreateNewProcTags(NP_Entry, (IPTR)Touch_PrefsEntry,
                             NP_Name, (IPTR)"touchscreen prefs",
                             NP_UserData, (IPTR)data,
                             NP_WindowPtr, (IPTR)-1,
                             NP_StackSize, 16384,
                             NP_Priority, 0,
                             TAG_DONE);
    data->prefs = (struct Task *)proc;
    if (!proc)
    {
        data->prefs_alive = FALSE;
        FreeSignal(data->prefs_signal);
        data->prefs_signal = -1;
        return FALSE;
    }
    return TRUE;
}

void Touch_StopPrefs(struct TouchData *data)
{
    struct ExecBase *SysBase = data->tsd->cs_SysBase;
    BOOL wait = FALSE;

    if (data->prefs_signal < 0)
        return;
    Forbid();
    if (data->prefs_alive)
    {
        Signal(data->prefs, SIGBREAKF_CTRL_C);
        wait = TRUE;
    }
    Permit();
    if (wait)
        Wait(1UL << data->prefs_signal);
    FreeSignal(data->prefs_signal);
    data->prefs_signal = -1;
}
