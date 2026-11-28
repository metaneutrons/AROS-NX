/* Copyright (c) 2026, The AROS Development Team.
 * Explicit consent gate for private-lab USB tests.
 */
#include <stddef.h>

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
static const char consent_title[] = "START: USBEcho";
static const char consent_text[] =
    "Starts USBEcho for 10 minutes on the P4 high-speed USB port.\n"
    "Private VID/PID 1209:0001 (pid.codes); do not\n"
    "redistribute this in-house identity.\n\n"
    "USB bulk accepts test bytes and returns their echoes.\n"
    "No screen capture, filesystem access, pointer control or\n"
    "input injection is enabled; no desktop data is exposed.\n\n"
    "The log is volatile local RAM:USBEcho.log; it is not\n"
    "exposed by this USB service or writable over USB.\n\n"
    "Use only a controlled USB host; confirm before starting.\n"
    "After Start, wait for FINISHED or FAILED; do not relaunch.";
static const char failed_title[] = "FAILED: USBEcho";
static const char finished_title[] = "FINISHED: USBEcho";
#else
static const char diag_command[] =
    "FLASHDISK0P0:USBDiag ENABLE VID 4617 PID 1 SECONDS 600 "
    "STEADYCLOCKS NONEAUTH SCREEN Workbench ROOT SYS:Prefs "
    "ALLOWMOUSE UNATTENDED";
static const char log_path[] = "RAM:USBDiag.log";
static const char consent_title[] = "START: USBDiag";
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
    "Confirm before starting.\n"
    "After Start, wait for FINISHED or FAILED; do not relaunch.";
static const char failed_title[] = "FAILED: USBDiag";
static const char finished_title[] = "FINISHED: USBDiag";
#endif

#define USB_LOG_TAIL_MAX 1024
#define RESULT_TEXT_CAPACITY 320

enum launch_phase
{
    LAUNCH_PHASE_NONE,
    LAUNCH_PHASE_LOG_OPEN,
    LAUNCH_PHASE_STDIN_OPEN,
    LAUNCH_PHASE_STDERR_OPEN
};

enum log_status
{
    LOG_STATUS_NOT_READ,
    LOG_STATUS_UNAVAILABLE,
    LOG_STATUS_READ_ERROR,
    LOG_STATUS_NO_KNOWN_MARKER,
    LOG_STATUS_ARGUMENTS_REJECTED,
    LOG_STATUS_DURATION_REJECTED,
    LOG_STATUS_STEADYCLOCKS_REQUIRED,
    LOG_STATUS_DIAG_AUTH_REQUIRED,
    LOG_STATUS_DIAG_INPUT_POLICY_REQUIRED,
    LOG_STATUS_DIAG_WORKER_FAILED,
    LOG_STATUS_DIAG_WORKER_ENABLED,
    LOG_STATUS_DIAG_TAP_UNAVAILABLE,
    LOG_STATUS_DIAG_TAP_CLAIMED,
    LOG_STATUS_DIAG_RUNTIME_STATUS,
    LOG_STATUS_CLOCK_OWNER_MISSING,
    LOG_STATUS_CLOCK_LOCK_BUSY,
    LOG_STATUS_OWNER_ALREADY_CLAIMED,
    LOG_STATUS_IRQ_ALREADY_ROUTED,
    LOG_STATUS_PHY_SETUP_FAILED,
    LOG_STATUS_HS_POLLING_READY,
    LOG_STATUS_USB_RUNTIME_FAULT,
    LOG_STATUS_PHY_RESTORE_QUARANTINED
};

struct launch_outcome
{
    LONG result;
    LONG dos_error;
    LONG log_error;
    LONG detail_code;
    enum launch_phase phase;
    enum log_status status;
    int command_called;
    int log_created;
    int has_detail_code;
};

struct known_log_marker
{
    const char *text;
    enum log_status status;
};

static const struct known_log_marker known_log_markers[] = {
    {"USBEcho: explicit ENABLE and authorized laboratory VID/PID required",
     LOG_STATUS_ARGUMENTS_REJECTED},
    {"USBEcho: SECONDS must be 1..600", LOG_STATUS_DURATION_REJECTED},
    {"USBEcho: STEADYCLOCKS required: normal desktop, no concurrent display/stress reinitialization",
     LOG_STATUS_STEADYCLOCKS_REQUIRED},
    {"USBDiag: explicit NONEAUTH required; local USB has no authentication or encryption",
     LOG_STATUS_DIAG_AUTH_REQUIRED},
    {"USBDiag: mouse requires UNATTENDED; concurrent local touch/mouse arbitration is not implemented",
     LOG_STATUS_DIAG_INPUT_POLICY_REQUIRED},
    {"USBDiag: worker startup failed", LOG_STATUS_DIAG_WORKER_FAILED},
    {"USBDiag: unauthenticated USB diagnosis explicitly enabled; no file writes, drag or keyboard",
     LOG_STATUS_DIAG_WORKER_ENABLED},
    {"USBDiag: matched console tap unavailable/busy; NOLOGS is explicit lab-only fallback",
     LOG_STATUS_DIAG_TAP_UNAVAILABLE},
    {"[USBDiag] console mirror claimed", LOG_STATUS_DIAG_TAP_CLAIMED},
    {"USBEcho: matched permanent SDMMC clock owner required",
     LOG_STATUS_CLOCK_OWNER_MISSING},
    {"USBEcho: hardware start refused; SDMMC clock owner busy",
     LOG_STATUS_CLOCK_LOCK_BUSY},
    {"USBEcho: another echo owner is present", LOG_STATUS_OWNER_ALREADY_CLAIMED},
    {"USBEcho: HS IRQ source already routed; refusing ownership",
     LOG_STATUS_IRQ_ALREADY_ROUTED},
    {"USBEcho: PHY setup refused/failed", LOG_STATUS_PHY_SETUP_FAILED}
};

static size_t text_length(const char *text)
{
    size_t length = 0;

    while (text[length] != '\0')
        length++;
    return length;
}

static int line_equals(const UBYTE *line, size_t length, const char *text)
{
    size_t i = 0;

    while (text[i] != '\0') {
        if (i >= length || line[i] != (UBYTE)text[i])
            return 0;
        i++;
    }
    return i == length;
}

static int line_has_prefix(const UBYTE *line, size_t length,
                           const char *prefix)
{
    size_t i = 0;

    while (prefix[i] != '\0') {
        if (i >= length || line[i] != (UBYTE)prefix[i])
            return 0;
        i++;
    }
    return 1;
}

static int parse_signed_decimal(const UBYTE *text, size_t length,
                                LONG *value)
{
    ULONG maximum = (ULONG)~(ULONG)0 >> 1;
    ULONG limit = maximum;
    ULONG magnitude = 0;
    size_t i = 0;
    int negative = 0;

    if (length == 0)
        return 0;
    if (text[i] == '-') {
        negative = 1;
        limit = maximum + 1u;
        i++;
    }
    if (i == length)
        return 0;
    for (; i < length; i++) {
        ULONG digit;

        if (text[i] < '0' || text[i] > '9')
            return 0;
        digit = (ULONG)(text[i] - '0');
        if (magnitude > (limit - digit) / 10u)
            return 0;
        magnitude = magnitude * 10u + digit;
    }
    if (negative) {
        if (magnitude == maximum + 1u)
            *value = -(LONG)maximum - 1;
        else
            *value = -(LONG)magnitude;
    } else {
        *value = (LONG)magnitude;
    }
    return 1;
}

static enum log_status classify_log_line(const UBYTE *line, size_t length,
                                         LONG *detail_code,
                                         int *has_detail_code)
{
    static const char ready_prefix[] =
        "USBEcho: polled HS PIO ready; duration=";
    static const char restore_prefix[] = "USBEcho: restore failed (";
    static const char restore_suffix[] =
        "); owner/snapshot quarantined until reboot";
    size_t i;

    for (i = 0; i < sizeof(known_log_markers) / sizeof(known_log_markers[0]);
         i++) {
        if (line_equals(line, length, known_log_markers[i].text))
            return known_log_markers[i].status;
    }

    if (line_has_prefix(line, length, ready_prefix)) {
        LONG duration;
        size_t prefix_length = text_length(ready_prefix);

        if (parse_signed_decimal(line + prefix_length,
                                 length - prefix_length, &duration) &&
            duration >= 1 && duration <= 600)
            return LOG_STATUS_HS_POLLING_READY;
    }

    if (line_has_prefix(line, length, restore_prefix)) {
        size_t prefix_length = text_length(restore_prefix);
        size_t suffix_length = text_length(restore_suffix);

        if (length > prefix_length + suffix_length &&
            line_equals(line + length - suffix_length, suffix_length,
                        restore_suffix) &&
            parse_signed_decimal(line + prefix_length,
                                 length - prefix_length - suffix_length,
                                 detail_code)) {
            *has_detail_code = 1;
            return LOG_STATUS_PHY_RESTORE_QUARANTINED;
        }
    }

    if (line_has_prefix(line, length, "USBEcho: "))
        return LOG_STATUS_USB_RUNTIME_FAULT;
    if (line_has_prefix(line, length, "USBDiag: "))
        return LOG_STATUS_DIAG_RUNTIME_STATUS;
    return LOG_STATUS_NO_KNOWN_MARKER;
}

static void summarize_log(struct launch_outcome *outcome)
{
    BPTR file = BNULL;
    struct FileInfoBlock *fib = NULL;
    UBYTE tail[USB_LOG_TAIL_MAX + 1];
    LONG file_size;
    LONG start;
    LONG requested;
    LONG bytes_read;
    size_t index = 0;

    outcome->status = LOG_STATUS_UNAVAILABLE;
    if (!outcome->log_created)
        return;

    file = Open((CONST_STRPTR)log_path, MODE_OLDFILE);
    if (!file) {
        outcome->log_error = IoErr();
        return;
    }

    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) {
        outcome->status = LOG_STATUS_READ_ERROR;
        outcome->log_error = IoErr();
        goto out;
    }
    if (!ExamineFH(file, fib) || fib->fib_Size < 0) {
        outcome->status = LOG_STATUS_READ_ERROR;
        outcome->log_error = IoErr();
        goto out;
    }

    file_size = fib->fib_Size;
    start = file_size > USB_LOG_TAIL_MAX ?
            file_size - (USB_LOG_TAIL_MAX + 1) : 0;
    requested = file_size - start;
    if (Seek(file, start, OFFSET_BEGINNING) == -1) {
        outcome->status = LOG_STATUS_READ_ERROR;
        outcome->log_error = IoErr();
        goto out;
    }
    bytes_read = Read(file, tail, requested);
    if (bytes_read < 0) {
        outcome->status = LOG_STATUS_READ_ERROR;
        outcome->log_error = IoErr();
        goto out;
    }

    outcome->status = LOG_STATUS_NO_KNOWN_MARKER;
    if (start > 0 && bytes_read > 0) {
        if (tail[0] == '\n') {
            index = 1;
        } else {
            while (index < (size_t)bytes_read && tail[index] != '\n')
                index++;
            if (index < (size_t)bytes_read)
                index++;
        }
    }

    while (index < (size_t)bytes_read) {
        size_t end = index;
        size_t line_length;
        LONG detail_code = 0;
        int has_detail_code = 0;
        enum log_status status;

        while (end < (size_t)bytes_read && tail[end] != '\n')
            end++;
        if (end == (size_t)bytes_read)
            break;
        line_length = end - index;
        if (line_length > 0 && tail[index + line_length - 1] == '\r')
            line_length--;
        status = classify_log_line(tail + index, line_length,
                                   &detail_code, &has_detail_code);
        if (status != LOG_STATUS_NO_KNOWN_MARKER) {
            outcome->status = status;
            outcome->has_detail_code = has_detail_code;
            outcome->detail_code = detail_code;
        }
        index = end + 1;
    }

out:
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    Close(file);
}

static const char *phase_name(enum launch_phase phase)
{
    switch (phase) {
    case LAUNCH_PHASE_LOG_OPEN: return "STDOUT_LOG_OPEN";
    case LAUNCH_PHASE_STDIN_OPEN: return "STDIN_OPEN";
    case LAUNCH_PHASE_STDERR_OPEN: return "STDERR_OPEN";
    default: return "UNKNOWN_SETUP";
    }
}

static const char *status_name(enum log_status status)
{
    switch (status) {
    case LOG_STATUS_UNAVAILABLE: return "LOG_UNAVAILABLE";
    case LOG_STATUS_READ_ERROR: return "LOG_READ_ERROR";
    case LOG_STATUS_NO_KNOWN_MARKER: return "NO_KNOWN_MARKER";
    case LOG_STATUS_ARGUMENTS_REJECTED: return "ARGUMENTS_REJECTED";
    case LOG_STATUS_DURATION_REJECTED: return "DURATION_REJECTED";
    case LOG_STATUS_STEADYCLOCKS_REQUIRED: return "STEADYCLOCKS_REQUIRED";
    case LOG_STATUS_DIAG_AUTH_REQUIRED: return "DIAG_AUTH_REQUIRED";
    case LOG_STATUS_DIAG_INPUT_POLICY_REQUIRED: return "DIAG_INPUT_POLICY_REQUIRED";
    case LOG_STATUS_DIAG_WORKER_FAILED: return "DIAG_WORKER_FAILED";
    case LOG_STATUS_DIAG_WORKER_ENABLED: return "DIAG_WORKER_ENABLED";
    case LOG_STATUS_DIAG_TAP_UNAVAILABLE: return "DIAG_TAP_UNAVAILABLE";
    case LOG_STATUS_DIAG_TAP_CLAIMED: return "DIAG_TAP_CLAIMED";
    case LOG_STATUS_DIAG_RUNTIME_STATUS: return "DIAG_RUNTIME_STATUS";
    case LOG_STATUS_CLOCK_OWNER_MISSING: return "CLOCK_OWNER_MISSING";
    case LOG_STATUS_CLOCK_LOCK_BUSY: return "CLOCK_LOCK_BUSY";
    case LOG_STATUS_OWNER_ALREADY_CLAIMED: return "OWNER_ALREADY_CLAIMED";
    case LOG_STATUS_IRQ_ALREADY_ROUTED: return "IRQ_ALREADY_ROUTED";
    case LOG_STATUS_PHY_SETUP_FAILED: return "PHY_SETUP_FAILED";
    case LOG_STATUS_HS_POLLING_READY: return "HS_POLLING_READY";
    case LOG_STATUS_USB_RUNTIME_FAULT: return "USB_RUNTIME_FAULT";
    case LOG_STATUS_PHY_RESTORE_QUARANTINED:
        return "PHY_RESTORE_QUARANTINED";
    default: return "NOT_READ";
    }
}

static int status_is_failure(enum log_status status)
{
    switch (status) {
    case LOG_STATUS_ARGUMENTS_REJECTED:
    case LOG_STATUS_DURATION_REJECTED:
    case LOG_STATUS_STEADYCLOCKS_REQUIRED:
    case LOG_STATUS_DIAG_AUTH_REQUIRED:
    case LOG_STATUS_DIAG_INPUT_POLICY_REQUIRED:
    case LOG_STATUS_DIAG_WORKER_FAILED:
    case LOG_STATUS_DIAG_TAP_UNAVAILABLE:
    case LOG_STATUS_CLOCK_OWNER_MISSING:
    case LOG_STATUS_CLOCK_LOCK_BUSY:
    case LOG_STATUS_OWNER_ALREADY_CLAIMED:
    case LOG_STATUS_IRQ_ALREADY_ROUTED:
    case LOG_STATUS_PHY_SETUP_FAILED:
    case LOG_STATUS_USB_RUNTIME_FAULT:
    case LOG_STATUS_PHY_RESTORE_QUARANTINED:
        return 1;
    default:
        return 0;
    }
}

static void append_text(char *buffer, size_t capacity, size_t *length,
                        const char *text)
{
    while (text[0] != '\0' && *length + 1 < capacity)
        buffer[(*length)++] = *text++;
    buffer[*length] = '\0';
}

static void append_number(char *buffer, size_t capacity, size_t *length,
                          LONG number)
{
    char digits[sizeof(ULONG) * 8];
    size_t count = 0;
    ULONG magnitude;

    if (number < 0) {
        append_text(buffer, capacity, length, "-");
        magnitude = (ULONG)0 - (ULONG)number;
    } else {
        magnitude = (ULONG)number;
    }
    do {
        digits[count++] = (char)('0' + magnitude % 10u);
        magnitude /= 10u;
    } while (magnitude != 0 && count < sizeof(digits));
    while (count > 0 && *length + 1 < capacity)
        buffer[(*length)++] = digits[--count];
    buffer[*length] = '\0';
}

static void build_result_text(const struct launch_outcome *outcome,
                              char *buffer, size_t capacity)
{
    size_t length = 0;

    if (capacity == 0)
        return;
    buffer[0] = '\0';
    if (outcome->command_called) {
        append_text(buffer, capacity, &length, "DOS return: ");
        append_number(buffer, capacity, &length, outcome->result);
    } else {
        append_text(buffer, capacity, &length, "Setup failed: ");
        append_text(buffer, capacity, &length, phase_name(outcome->phase));
    }
    append_text(buffer, capacity, &length, "\nLast known status: ");
    append_text(buffer, capacity, &length, status_name(outcome->status));
    if (!outcome->command_called) {
        append_text(buffer, capacity, &length, "\nDOS IoErr: ");
        append_number(buffer, capacity, &length, outcome->dos_error);
    }
    if ((outcome->status == LOG_STATUS_READ_ERROR ||
         outcome->status == LOG_STATUS_UNAVAILABLE) && outcome->log_error != 0) {
        append_text(buffer, capacity, &length, "\nLog IoErr: ");
        append_number(buffer, capacity, &length, outcome->log_error);
    }
    if (outcome->has_detail_code) {
        append_text(buffer, capacity, &length, "\nRestore code: ");
        append_number(buffer, capacity, &length, outcome->detail_code);
    }
    if (outcome->command_called && outcome->result == RETURN_OK)
        append_text(buffer, capacity, &length,
                    "\nCommand ended; elapsed time is not verified.");
    else if (!outcome->command_called)
        append_text(buffer, capacity, &length,
                    "\nUSB command was not started.");
    append_text(buffer, capacity, &length, "\nLocal log (not shown): ");
    append_text(buffer, capacity, &length, log_path);
}

static void run_diagnostic(struct launch_outcome *outcome)
{
    BPTR input = BNULL;
    BPTR output = BNULL;
    BPTR error = BNULL;

    outcome->result = RETURN_FAIL;
    outcome->dos_error = 0;
    outcome->log_error = 0;
    outcome->detail_code = 0;
    outcome->phase = LAUNCH_PHASE_NONE;
    outcome->status = LOG_STATUS_NOT_READ;
    outcome->command_called = 0;
    outcome->log_created = 0;
    outcome->has_detail_code = 0;

    /* Keep startup diagnostics locally; this log is not USB-exported. */
    output = Open((CONST_STRPTR)log_path, MODE_NEWFILE);
    if (!output) {
        outcome->phase = LAUNCH_PHASE_LOG_OPEN;
        outcome->dos_error = IoErr();
        goto out;
    }
    outcome->log_created = 1;

    input = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    if (!input) {
        outcome->phase = LAUNCH_PHASE_STDIN_OPEN;
        outcome->dos_error = IoErr();
        goto out;
    }

    /* stderr is deliberately separate from the diagnostic log. */
    error = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (!error) {
        outcome->phase = LAUNCH_PHASE_STDERR_OPEN;
        outcome->dos_error = IoErr();
        goto out;
    }

    outcome->command_called = 1;
    outcome->result = SystemTags((STRPTR)diag_command,
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
    summarize_log(outcome);
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
    struct launch_outcome outcome;
    char result_text[RESULT_TEXT_CAPACITY];
    int failed;
    struct EasyStruct final_status = {
        sizeof(struct EasyStruct),
        0,
        NULL,
        (CONST_STRPTR)result_text,
        (CONST_STRPTR)"OK"
    };

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

    run_diagnostic(&outcome);
    build_result_text(&outcome, result_text, sizeof(result_text));
    failed = !outcome.command_called || outcome.result != RETURN_OK ||
             status_is_failure(outcome.status);
    final_status.es_Title = (CONST_STRPTR)
        (failed ? failed_title : finished_title);
    EasyRequestArgs(NULL, &final_status, NULL, NULL);

    CloseLibrary(&IntuitionBase->LibNode);
    return (int)outcome.result;
}
