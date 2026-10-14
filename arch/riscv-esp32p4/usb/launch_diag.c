/* Copyright (c) 2026, The AROS Development Team.
 * Explicit consent gate for private-lab USB tests.
 */
#ifndef LAUNCH_DIAG_HOST_TEST
#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/libraries.h>
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#endif

#if defined(P4_USB_LAUNCH_ECHO)
static const char diag_command[] =
    "FLASHDISK0P0:USBEcho ENABLE VID 4617 PID 1 SECONDS 600 "
    "STEADYCLOCKS";
static const char log_path[] = "RAM:USBEcho.log";
static const char consent_title[] = "Private USB echo test";
static const char consent_text[] =
    "Starts USBEcho for 10 minutes on the P4 high-speed USB port.\n"
    "Private VID/PID 1209:0001 (pid.codes); do not\n"
    "redistribute this in-house identity.\n\n"
    "USB bulk accepts test bytes and returns their echoes.\n"
    "No screen capture, filesystem access, pointer control or\n"
    "input injection is enabled; no desktop data is exposed.\n\n"
    "The log is volatile local RAM:USBEcho.log; it is not\n"
    "exposed by this USB service or writable over USB.\n\n"
    "Use only a controlled USB host; confirm before starting.";
static const char failure_title[] = "USBEcho did not complete";
static const char failure_text[] =
    "USBEcho could not start or returned an error.\n"
    "If created, inspect volatile local\n"
    "RAM:USBEcho.log; it is not exposed over USB.\n"
    "Check USB state before another run.";
#else
static const char diag_command[] =
    "FLASHDISK0P0:USBDiag ENABLE VID 4617 PID 1 SECONDS 600 "
    "STEADYCLOCKS NONEAUTH SCREEN Workbench ROOT SYS:Prefs "
    "ALLOWMOUSE UNATTENDED";
static const char log_path[] = "RAM:USBDiag.log";
static const char consent_title[] = "Private USB diagnostics";
static const char consent_text[] =
    "Starts USBDiag for 10 minutes on P4 high-speed USB.\n"
    "Private VID/PID 1209:0001 (pid.codes); do not\n"
    "redistribute this in-house identity.\n\n"
    "USB access has no authentication or encryption.\n"
    "A host can capture Workbench, read files under\n"
    "SYS:Prefs, and move or click the pointer. The\n"
    "service exposes no file-write operation, but\n"
    "clicks can still activate the desktop.\n\n"
    "The log is volatile local RAM:USBDiag.log. It is\n"
    "not exposed by this USB service or writable over\n"
    "USB.\n\n"
    "Use only a controlled USB host. No local touch or\n"
    "mouse during the run; input arbitration is absent.\n"
    "Confirm before starting.";
static const char failure_title[] = "USBDiag did not complete";
static const char failure_text[] =
    "USBDiag could not start or returned an error.\n"
    "If created, inspect volatile local\n"
    "RAM:USBDiag.log; it is not exposed over USB.\n"
    "Check USB state before another run.";
#endif

static LONG run_diagnostic(void)
{
    BPTR input = BNULL;
    BPTR output = BNULL;
    BPTR error = BNULL;
    LONG result = RETURN_FAIL;

    /* Keep startup diagnostics locally; this log is not USB-exported. */
    output = Open((CONST_STRPTR)log_path, MODE_NEWFILE);
    if (!output)
        goto out;

    input = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    if (!input)
        goto out;

    /* stderr is deliberately separate from the diagnostic log. */
    error = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (!error)
        goto out;

    result = SystemTags((STRPTR)diag_command,
                        SYS_Input, (IPTR)input,
                        SYS_Output, (IPTR)output,
                        SYS_Error, (IPTR)error,
                        SYS_Asynch, (IPTR)FALSE,
                        SYS_Background, (IPTR)TRUE,
                        TAG_END);

out:
    if (error)
        Close(error);
    if (output)
        Close(output);
    if (input)
        Close(input);
    return result;
}

int main(void)
{
    struct IntuitionBase *IntuitionBase;
    struct EasyStruct consent = {
        sizeof(struct EasyStruct),
        0,
        (CONST_STRPTR)consent_title,
        (CONST_STRPTR)consent_text,
        (CONST_STRPTR)"Start|Cancel"
    };
    struct EasyStruct failure = {
        sizeof(struct EasyStruct),
        0,
        (CONST_STRPTR)failure_title,
        (CONST_STRPTR)failure_text,
        (CONST_STRPTR)"OK"
    };
    LONG result;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary(
        (CONST_STRPTR)"intuition.library", 39);
    if (!IntuitionBase)
        return RETURN_FAIL;

    /* EasyRequestArgs numbers the leftmost gadget 1; every other result is
     * cancellation or requester failure and must leave USB untouched. */
    if (EasyRequestArgs(NULL, &consent, NULL, NULL) != 1) {
        CloseLibrary(&IntuitionBase->LibNode);
        return RETURN_OK;
    }

    result = run_diagnostic();
    if (result != RETURN_OK) {
        EasyRequestArgs(NULL, &failure, NULL, NULL);
    }

    CloseLibrary(&IntuitionBase->LibNode);
    return (int)result;
}
