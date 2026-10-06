/*
 * touchscreen.hidd: what the worker does through DOS. It reads the
 * controller's firmware image, when the controller needs one, and applies
 * ENV:Sys/touchscreen.prefs to the driver's settings once ENV: exists and
 * again whenever the file changes.
 *
 * All of it runs in the worker, which is a Process for this reason: from a
 * plain task SetIoErr() does nothing and IoErr() reads past the end of
 * struct Task, so a failed Open() can come back as a handle to nothing.
 * The worker calls touch_files_poll() once per poll; the waits below are
 * counted in polls so that nothing here blocks the touch.
 */

#include <aros/debug.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/notify.h>
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include "touchscreen_intern.h"
#include "touch_prefs.h"

#define TOUCH_PREFS_BYTES       2048
/* Polls (50 ms each) between looks for ENV: during boot. */
#define TOUCH_ENV_LOOK_POLLS    20
/* A Startup-Sequence may assign ENV: to RAM: first and add ENVARC: to it a
   line later; a read in between would miss a saved file. */
#define TOUCH_ENV_SETTLE_POLLS  20
/* A writer may create, write and close in several steps, each of which
   notifies; reading after a short quiet time sees the finished file. */
#define TOUCH_CHANGE_SETTLE_POLLS 4

enum
{
    TOUCH_FILES_WAIT_ENV,
    TOUCH_FILES_ENV_SETTLE,
    TOUCH_FILES_WATCHING
};

BOOL touch_files_open(struct TouchData *data, struct TouchFiles *f)
{
    struct ExecBase *SysBase = data->tsd->cs_SysBase;

    f->state = TOUCH_FILES_WAIT_ENV;
    f->due = 0;
    f->pending = FALSE;
    f->notifying = FALSE;
    f->DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    f->buf = AllocVec(TOUCH_PREFS_BYTES, MEMF_ANY);
    f->notify_signal = AllocSignal(-1);
    if (f->DOSBase && f->buf && f->notify_signal >= 0)
        return TRUE;
    touch_files_close(data, f);
    return FALSE;
}

void touch_files_close(struct TouchData *data, struct TouchFiles *f)
{
    struct ExecBase *SysBase = data->tsd->cs_SysBase;
    struct DosLibrary *DOSBase = f->DOSBase;

    if (f->notifying)
        EndNotify(&f->notify);
    f->notifying = FALSE;
    if (f->notify_signal >= 0)
        FreeSignal(f->notify_signal);
    f->notify_signal = -1;
    if (f->buf)
        FreeVec(f->buf);
    f->buf = NULL;
    if (DOSBase)
        CloseLibrary((struct Library *)DOSBase);
    f->DOSBase = NULL;
}

/*
 * The controller's firmware image from the first path that holds one of
 * exactly the right size. Only controllers that run from RAM (the D1001's
 * GSL3670) need one; the board names where it lives.
 */
UBYTE *touch_files_firmware(struct TouchData *data, struct TouchFiles *f,
                            ULONG bytes, CONST_STRPTR *loaded_path)
{
    struct ExecBase *SysBase = data->tsd->cs_SysBase;
    struct DosLibrary *DOSBase = f->DOSBase;
    ULONG i;

    *loaded_path = NULL;
    if (!data->firmware_paths)
        return NULL;
    for (i = 0; data->firmware_paths[i]; ++i)
    {
        CONST_STRPTR path = data->firmware_paths[i];
        BPTR file = Open(path, MODE_OLDFILE);
        LONG size, got;
        UBYTE *image;

        if (!file)
            continue;
        Seek(file, 0, OFFSET_END);
        size = Seek(file, 0, OFFSET_BEGINNING);
        if (size != (LONG)bytes)
        {
            bug("[Touch] %s: rejected firmware %s: %ld bytes, expected "
                "%lu\n", data->name, path, (long)size, (unsigned long)bytes);
            Close(file);
            continue;
        }
        image = AllocVec(bytes, MEMF_PUBLIC);
        if (!image)
        {
            Close(file);
            return NULL;
        }
        got = Read(file, image, bytes);
        Close(file);
        if (got != (LONG)bytes)
        {
            bug("[Touch] %s: short firmware read from %s: %ld/%lu\n",
                data->name, path, (long)got, (unsigned long)bytes);
            FreeVec(image);
            continue;
        }
        *loaded_path = path;
        return image;
    }
    return NULL;
}

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
    got = Read(file, buf, size);
    Close(file);
    return got > 0 ? got : 0;
}

static void touch_apply_prefs(struct TouchData *data, struct TouchFiles *f)
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

    length = touch_read_prefs(f->DOSBase, f->buf, TOUCH_PREFS_BYTES);
    if (length)
        touch_prefs_parse(&prefs, f->buf, (size_t)length, data->name);

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

/*
 * Once per poll. ENV: comes from the Startup-Sequence; until then there is
 * nothing to read, and asking DOS for a missing assign would raise a
 * requester, so the DOS list is looked at instead. Once ENV: is settled
 * the file is read and watched; RAM: notifies even for a file that does
 * not exist yet.
 */
void touch_files_poll(struct TouchData *data, struct TouchFiles *f,
                      ULONG poll)
{
    struct ExecBase *SysBase = data->tsd->cs_SysBase;
    struct DosLibrary *DOSBase = f->DOSBase;
    ULONG mask;

    switch (f->state)
    {
    case TOUCH_FILES_WAIT_ENV:
        if (poll % TOUCH_ENV_LOOK_POLLS || !touch_env_present(DOSBase))
            return;
        f->state = TOUCH_FILES_ENV_SETTLE;
        f->due = poll + TOUCH_ENV_SETTLE_POLLS;
        return;

    case TOUCH_FILES_ENV_SETTLE:
        if ((LONG)(poll - f->due) < 0)
            return;
        f->notify.nr_Name = (STRPTR)TOUCH_PREFS_PATH;
        f->notify.nr_Flags = NRF_SEND_SIGNAL;
        f->notify.nr_stuff.nr_Signal.nr_Task = FindTask(NULL);
        f->notify.nr_stuff.nr_Signal.nr_SignalNum = f->notify_signal;
        f->notifying = StartNotify(&f->notify);
        if (!f->notifying)
            bug("[Touch] %s: cannot watch %s (error %ld); prefs are read "
                "once\n", data->name, TOUCH_PREFS_PATH, (long)IoErr());
        touch_apply_prefs(data, f);
        f->state = TOUCH_FILES_WATCHING;
        return;

    case TOUCH_FILES_WATCHING:
        if (!f->notifying)
            return;
        mask = 1UL << f->notify_signal;
        if (SetSignal(0, mask) & mask)
        {
            /* Every further change restarts the quiet time. */
            f->pending = TRUE;
            f->due = poll + TOUCH_CHANGE_SETTLE_POLLS;
        }
        if (f->pending && (LONG)(poll - f->due) >= 0)
        {
            f->pending = FALSE;
            touch_apply_prefs(data, f);
        }
        return;
    }
}
