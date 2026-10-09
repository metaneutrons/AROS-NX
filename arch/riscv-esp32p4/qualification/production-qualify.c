/*
 * Bounded user-space production qualification for the ESP32-P4 SMP build.
 *
 * This is an ordinary Workbench tool.  It exercises two affinity-pinned
 * workers, host-referenced read-only SD READ64 requests, and Intuition drawing
 * on the public Workbench screen.  It does not modify the kernel or media.
 */

#include <aros/config.h>

#include <exec/types.h>
#include <devices/newstyle.h>
#include <devices/timer.h>
#include <devices/trackdisk.h>
#include <dos/dos.h>
#include <exec/execbase.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <intuition/intuition.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/kernel.h>
#include <proto/timer.h>

#define DEBUG 1
#include <aros/debug.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "qualification-card.h"

#ifndef QUAL_DEFAULT_SECONDS
#define QUAL_DEFAULT_SECONDS 1800UL
#endif

#define PQ_MIN_SECONDS             30UL
#define PQ_PRODUCTION_SECONDS      1800UL
#define PQ_MAX_SECONDS             3600UL
#define PQ_SMOKE_SECONDS           30UL
#define PQ_REPORT_SECONDS          300UL
#define PQ_REPORT_PERIOD_SECONDS   5UL
#define PQ_READ_PERIOD_SECONDS     2UL
#define PQ_GFX_PERIOD_SECONDS     1UL
#define PQ_WORKER_TIMEOUT_SECONDS  2UL
#define PQ_STOP_TIMEOUT_SECONDS    2UL
#define PQ_COVERAGE_GRACE_SECONDS  5UL
#define PQ_READ_SECTORS            128UL
#define PQ_SECTOR_BYTES            512UL
#define PQ_READ_BYTES              (PQ_READ_SECTORS * PQ_SECTOR_BYTES)
#define PQ_BUFFER_BYTES            (PQ_READ_BYTES + 64UL)
#define PQ_WORKER_STACK            16384UL
#define PQ_WORKER_PRIORITY         (-10)
#define PQ_LCG_STEPS               16777216UL
#define PQ_LCG_POLL_MASK           0x0000ffffUL
#define PQ_LCG_MULTIPLIER          1664525UL
#define PQ_LCG_INCREMENT            1013904223UL
#define PQ_COLOR_TEXT              1
#define PQ_COLOR_BAR               3

APTR KernelBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Device *TimerBase;

enum PQState
{
    PQ_STATE_PREPARING = 0,
    PQ_STATE_RUNNING,
    PQ_STATE_PASS,
    PQ_STATE_SMOKE,
    PQ_STATE_FAIL,
    PQ_STATE_ABORT,
    PQ_STATE_INCOMPLETE
};

enum PQReason
{
    PQ_REASON_NONE = 0,
    PQ_REASON_ARGUMENTS,
    PQ_REASON_KERNEL_RESOURCE,
    PQ_REASON_ONE_HART,
    PQ_REASON_MAIN_HART,
    PQ_REASON_SIGNAL,
    PQ_REASON_TIMER_PORT,
    PQ_REASON_TIMER_REQUEST,
    PQ_REASON_TIMER_OPEN,
    PQ_REASON_ECLOCK,
    PQ_REASON_INTUITION_OPEN,
    PQ_REASON_GRAPHICS_OPEN,
    PQ_REASON_WINDOW_OPEN,
    PQ_REASON_SD_PORT,
    PQ_REASON_SD_REQUEST,
    PQ_REASON_SD_OPEN,
    PQ_REASON_SD_GEOMETRY,
    PQ_REASON_SD_NOT_PROTECTED,
    PQ_REASON_CARD_MBR,
    PQ_REASON_CARD_RANGE,
    PQ_REASON_WORKER_MASK,
    PQ_REASON_WORKER_CREATE,
    PQ_REASON_WORKER_START,
    PQ_REASON_WORKER_CPU,
    PQ_REASON_WORKER_MISMATCH,
    PQ_REASON_WORKER_TIMEOUT,
    PQ_REASON_SD_READ,
    PQ_REASON_SD_MISMATCH,
    PQ_REASON_SD_LATE,
    PQ_REASON_GRAPHICS_HART,
    PQ_REASON_GRAPHICS_LATE,
    PQ_REASON_WINDOW_CLOSED,
    PQ_REASON_CTRL_C,
    PQ_REASON_COVERAGE,
    PQ_REASON_DURATION
};

struct PQReport
{
    ULONG nonce;
    ULONG state;
    ULONG reason;
    ULONG goal_seconds;
    ULONG duration_seconds;
    ULONG cpu_batches[2];
    ULONG cpu_errors[2];
    ULONG sd_reads;
    ULONG sd_mismatches;
    ULONG sd_errors;
    ULONG gfx_updates;
    ULONG fixture_verified;
};

struct PQShared
{
    struct Task *controller;
    ULONG controller_signal;
    volatile ULONG epoch;
    volatile ULONG stop;
};

struct PQWorker
{
    struct PQShared *shared;
    ULONG hart;
    volatile ULONG ready;
    volatile ULONG stopped;
    volatile ULONG ready_error;
    volatile ULONG done_epoch;
    volatile ULONG work_signal_bit;
};

struct PQReference
{
    ULONG lba;
    ULONG fnv32;
};

static struct PQReport pq_report;
static struct PQShared pq_shared;
static struct PQWorker pq_workers[2];
static struct Task *pq_tasks[2];
static struct MsgPort *pq_sd_port;
static struct IOStdReq *pq_sd_io;
static struct MsgPort *pq_timer_port;
static struct timerequest *pq_timer_io;
static BOOL pq_sd_opened;
static BOOL pq_timer_opened;
static struct Window *pq_window;
static UBYTE *pq_sd_raw;
static UBYTE *pq_sd_buffer;
static ULONG pq_eclock_hz;
static ULONG pq_vblank_hz;
static ULONG pq_main_done_bit;
static ULONG pq_done_mask;
static BOOL pq_smoke;
static void pq_cpu_worker(void);

static const struct PQReference pq_references[QUAL_CARD_RANGE_COUNT] =
{
    QUAL_CARD_RANGES
};

static const char *pq_state_name(ULONG state)
{
    switch (state)
    {
        case PQ_STATE_PREPARING: return "PREPARING";
        case PQ_STATE_RUNNING:   return "RUNNING";
        case PQ_STATE_PASS:      return "PASS";
        case PQ_STATE_SMOKE:     return "SMOKE";
        case PQ_STATE_FAIL:      return "FAIL";
        case PQ_STATE_ABORT:     return "ABORT";
        default:                 return "INCOMPLETE";
    }
}

static const char *pq_reason_name(ULONG reason)
{
    static const char *const names[] =
    {
        "none", "arguments", "kernel-resource", "one-hart", "main-hart",
        "signal", "timer-port", "timer-request", "timer-open", "eclock",
        "intuition-open", "graphics-open", "window-open", "sd-port",
        "sd-request", "sd-open", "sd-geometry", "sd-not-protected",
        "card-mbr", "card-range", "worker-mask", "worker-create",
        "worker-start", "worker-cpu", "worker-mismatch", "worker-timeout",
        "sd-read", "sd-mismatch", "sd-late", "graphics-hart",
        "graphics-late", "window-closed", "ctrl-c", "coverage", "duration"
    };

    return reason < (sizeof(names) / sizeof(names[0])) ? names[reason] : "unknown";
}

static ULONG pq_read_eclock(UQUAD *value)
{
    struct EClockVal ev;
    ULONG hz = ReadEClock(&ev);

    *value = ((UQUAD)ev.ev_hi << 32) | ev.ev_lo;
    return hz;
}

static ULONG pq_elapsed_seconds(UQUAD start, UQUAD now)
{
    if (!pq_eclock_hz || now < start)
        return 0;
    return (ULONG)((now - start) / pq_eclock_hz);
}

static void pq_clear_controller_signal(void)
{
    if (pq_main_done_bit < 32)
        SetSignal(0, 1UL << pq_main_done_bit);
}

static ULONG pq_fnv1a(const UBYTE *data, ULONG length)
{
    ULONG hash = 2166136261UL;
    ULONG i;

    for (i = 0; i < length; ++i)
    {
        hash ^= data[i];
        hash *= 16777619UL;
    }
    return hash;
}

/* Independent affine jump-ahead reference for x = a*x + c modulo 2^32. */
static ULONG pq_lcg_advance(ULONG state, ULONG steps)
{
    ULONG acc_mult = 1;
    ULONG acc_plus = 0;
    ULONG cur_mult = PQ_LCG_MULTIPLIER;
    ULONG cur_plus = PQ_LCG_INCREMENT;

    while (steps)
    {
        if (steps & 1UL)
        {
            acc_plus = acc_plus * cur_mult + cur_plus;
            acc_mult *= cur_mult;
        }
        cur_plus = (cur_mult + 1UL) * cur_plus;
        cur_mult *= cur_mult;
        steps >>= 1;
    }

    return acc_mult * state + acc_plus;
}

static BOOL pq_parse_seconds(int argc, char **argv, ULONG *goal, BOOL *smoke)
{
    BOOL duration_seen = FALSE;
    int i;

    *goal = QUAL_DEFAULT_SECONDS;
    *smoke = (QUAL_DEFAULT_SECONDS < PQ_PRODUCTION_SECONDS);

    for (i = 1; i < argc; ++i)
    {
        const char *arg = argv[i];
        const char *number = arg;
        char *end = NULL;
        unsigned long parsed;

        if (!strcmp(arg, "SMOKE") || !strcmp(arg, "--smoke"))
        {
            *smoke = TRUE;
            if (!duration_seen)
                *goal = PQ_SMOKE_SECONDS;
            continue;
        }

        if (!strncmp(arg, "SECONDS=", 8))
            number = arg + 8;
        else if (!strncmp(arg, "DURATION=", 9))
            number = arg + 9;

        if (duration_seen || !*number)
            return FALSE;

        parsed = strtoul(number, &end, 10);
        if (end == number || *end != '\0' || parsed < PQ_MIN_SECONDS ||
            parsed > PQ_MAX_SECONDS)
            return FALSE;

        *goal = (ULONG)parsed;
        duration_seen = TRUE;
        if (*goal < PQ_PRODUCTION_SECONDS)
            *smoke = TRUE;
    }

    if (*goal < PQ_PRODUCTION_SECONDS)
        *smoke = TRUE;

#ifdef QUAL_SMOKE_ONLY
    *smoke = TRUE;
    if (*goal >= PQ_PRODUCTION_SECONDS)
        return FALSE;
#else
    if (*smoke && *goal >= PQ_PRODUCTION_SECONDS)
        return FALSE;
#endif

    return TRUE;
}

static BOOL pq_open_timer(void)
{
    pq_timer_port = CreateMsgPort();
    if (!pq_timer_port)
    {
        pq_report.reason = PQ_REASON_TIMER_PORT;
        return FALSE;
    }

    pq_timer_io = (struct timerequest *)CreateIORequest(
        pq_timer_port, sizeof(*pq_timer_io));
    if (!pq_timer_io)
    {
        pq_report.reason = PQ_REASON_TIMER_REQUEST;
        return FALSE;
    }

    if (OpenDevice(TIMERNAME, UNIT_ECLOCK,
                   (struct IORequest *)pq_timer_io, 0))
    {
        pq_report.reason = PQ_REASON_TIMER_OPEN;
        return FALSE;
    }

    pq_timer_opened = TRUE;
    TimerBase = (struct Device *)pq_timer_io->tr_node.io_Device;
    {
        UQUAD ignored;
        pq_eclock_hz = pq_read_eclock(&ignored);
    }
    if (!pq_eclock_hz)
    {
        pq_report.reason = PQ_REASON_ECLOCK;
        return FALSE;
    }
    return TRUE;
}

static BOOL pq_open_window(void)
{
    const char *title = pq_smoke ? "SMOKE QUALIFICATION" :
                                   "Production Qualification";

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 0);
    if (!IntuitionBase)
    {
        pq_report.reason = PQ_REASON_INTUITION_OPEN;
        return FALSE;
    }

    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 0);
    if (!GfxBase)
    {
        pq_report.reason = PQ_REASON_GRAPHICS_OPEN;
        return FALSE;
    }

    if (KrnGetCPUNumber() != 0)
    {
        pq_report.reason = PQ_REASON_MAIN_HART;
        return FALSE;
    }

    pq_window = OpenWindowTags(NULL,
        WA_PubScreenName, (IPTR)"Workbench",
        WA_Left,          24,
        WA_Top,           36,
        WA_Width,         620,
        WA_Height,        220,
        WA_Title,         (IPTR)title,
        WA_IDCMP,         IDCMP_CLOSEWINDOW | IDCMP_RAWKEY,
        WA_CloseGadget,   TRUE,
        WA_DragBar,       TRUE,
        WA_DepthGadget,   TRUE,
        WA_Activate,      TRUE,
        WA_SmartRefresh,  TRUE,
        TAG_END);
    if (!pq_window)
    {
        pq_report.reason = PQ_REASON_WINDOW_OPEN;
        return FALSE;
    }
    return TRUE;
}

static BOOL pq_open_sd(void)
{
    struct DriveGeometry geometry;
    ULONG i;

    pq_sd_port = CreateMsgPort();
    if (!pq_sd_port)
    {
        pq_report.reason = PQ_REASON_SD_PORT;
        ++pq_report.sd_errors;
        return FALSE;
    }

    pq_sd_io = (struct IOStdReq *)CreateIORequest(pq_sd_port,
                                                   sizeof(*pq_sd_io));
    if (!pq_sd_io)
    {
        pq_report.reason = PQ_REASON_SD_REQUEST;
        ++pq_report.sd_errors;
        return FALSE;
    }

    if (OpenDevice("sdcard.device", 0, (struct IORequest *)pq_sd_io, 0))
    {
        pq_report.reason = PQ_REASON_SD_OPEN;
        ++pq_report.sd_errors;
        return FALSE;
    }
    pq_sd_opened = TRUE;

    pq_sd_raw = (UBYTE *)AllocMem(PQ_BUFFER_BYTES,
                                  MEMF_PUBLIC | MEMF_CLEAR);
    if (!pq_sd_raw)
    {
        pq_report.reason = PQ_REASON_SD_REQUEST;
        ++pq_report.sd_errors;
        return FALSE;
    }
    pq_sd_buffer = (UBYTE *)(((IPTR)pq_sd_raw + 63) & ~(IPTR)63);

    memset(&geometry, 0, sizeof(geometry));
    pq_sd_io->io_Command = TD_GETGEOMETRY;
    pq_sd_io->io_Data = &geometry;
    pq_sd_io->io_Length = sizeof(geometry);
    pq_sd_io->io_Actual = 0;
    pq_sd_io->io_Offset = 0;
    if (DoIO((struct IORequest *)pq_sd_io) || pq_sd_io->io_Error ||
        pq_sd_io->io_Actual != sizeof(geometry) ||
        geometry.dg_SectorSize != PQ_SECTOR_BYTES ||
        geometry.dg_TotalSectors < QUAL_CARD_SECTOR_COUNT)
    {
        pq_report.reason = PQ_REASON_SD_GEOMETRY;
        ++pq_report.sd_errors;
        return FALSE;
    }

    pq_sd_io->io_Command = TD_PROTSTATUS;
    pq_sd_io->io_Data = NULL;
    pq_sd_io->io_Length = 0;
    pq_sd_io->io_Actual = 0;
    pq_sd_io->io_Offset = 0;
    if (DoIO((struct IORequest *)pq_sd_io) || pq_sd_io->io_Error ||
        pq_sd_io->io_Actual == 0)
    {
        pq_report.reason = PQ_REASON_SD_NOT_PROTECTED;
        ++pq_report.sd_errors;
        return FALSE;
    }

    /* The MBR fingerprint identifies this exact host-built SYS fixture. */
    pq_sd_io->io_Command = NSCMD_TD_READ64;
    pq_sd_io->io_Data = pq_sd_buffer;
    pq_sd_io->io_Length = PQ_SECTOR_BYTES;
    pq_sd_io->io_Actual = 0;
    pq_sd_io->io_Offset = 0;
    if (DoIO((struct IORequest *)pq_sd_io) || pq_sd_io->io_Error ||
        pq_sd_io->io_Actual != PQ_SECTOR_BYTES)
    {
        pq_report.reason = PQ_REASON_CARD_MBR;
        ++pq_report.sd_errors;
        return FALSE;
    }
    if (pq_fnv1a(pq_sd_buffer, PQ_SECTOR_BYTES) != QUAL_CARD_MBR_FNV32)
    {
        pq_report.reason = PQ_REASON_CARD_MBR;
        ++pq_report.sd_mismatches;
        return FALSE;
    }

    for (i = 0; i < QUAL_CARD_RANGE_COUNT; ++i)
    {
        UQUAD byte_offset = (UQUAD)pq_references[i].lba * PQ_SECTOR_BYTES;

        if ((UQUAD)pq_references[i].lba + PQ_READ_SECTORS >
            QUAL_CARD_SECTOR_COUNT)
        {
            pq_report.reason = PQ_REASON_CARD_RANGE;
            ++pq_report.sd_errors;
            return FALSE;
        }

        pq_sd_io->io_Command = NSCMD_TD_READ64;
        pq_sd_io->io_Data = pq_sd_buffer;
        pq_sd_io->io_Length = PQ_READ_BYTES;
        pq_sd_io->io_Actual = (ULONG)(byte_offset >> 32);
        pq_sd_io->io_Offset = (ULONG)byte_offset;
        if (DoIO((struct IORequest *)pq_sd_io) || pq_sd_io->io_Error ||
            pq_sd_io->io_Actual != PQ_READ_BYTES)
        {
            pq_report.reason = PQ_REASON_CARD_RANGE;
            ++pq_report.sd_errors;
            return FALSE;
        }
        if (pq_fnv1a(pq_sd_buffer, PQ_READ_BYTES) != pq_references[i].fnv32)
        {
            pq_report.reason = PQ_REASON_CARD_RANGE;
            ++pq_report.sd_mismatches;
            return FALSE;
        }
    }

    pq_report.fixture_verified = 1;
    return TRUE;
}

static struct Task *pq_create_worker(ULONG hart)
{
    void *mask = KrnAllocCPUMask();
    struct Task *task;

    if (!mask)
        return NULL;
    KrnClearCPUMask(mask);
    KrnGetCPUMask(hart, mask);

    task = NewCreateTask(TASKTAG_NAME, (IPTR)(hart ? "pq.cpu1" : "pq.cpu0"),
                         TASKTAG_PRI, (IPTR)PQ_WORKER_PRIORITY,
                         TASKTAG_PC, (IPTR)pq_cpu_worker,
                         TASKTAG_STACKSIZE, PQ_WORKER_STACK,
                         TASKTAG_USERDATA, (IPTR)&pq_workers[hart],
                         TASKTAG_AFFINITY, (IPTR)mask,
                         TAG_DONE);
    if (!task)
        KrnFreeCPUMask(mask);
    return task;
}

static ULONG pq_lcg_seed(ULONG nonce, ULONG hart, ULONG epoch)
{
    ULONG seed = nonce ^ (hart ? 0x91e10da5UL : 0x243f6a88UL);
    seed ^= epoch * 0x9e3779b9UL;
    return seed ? seed : 0x6d2b79f5UL;
}

static void pq_worker_exit(struct PQWorker *worker, LONG signal_bit)
{
    if (signal_bit >= 0)
        FreeSignal(signal_bit);

    __atomic_store_n(&worker->stopped, 1, __ATOMIC_RELEASE);
    Signal(worker->shared->controller, worker->shared->controller_signal);
    Wait(0);
}

static void pq_cpu_worker(void)
{
    struct Task *me = FindTask(NULL);
    struct PQWorker *worker = (struct PQWorker *)me->tc_UserData;
    struct PQShared *shared = worker->shared;
    LONG signal_bit = AllocSignal(-1);

    if (signal_bit < 0)
    {
        __atomic_store_n(&worker->ready_error, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&worker->ready, 1, __ATOMIC_RELEASE);
        Signal(shared->controller, shared->controller_signal);
        pq_worker_exit(worker, signal_bit);
    }

    __atomic_store_n(&worker->work_signal_bit, (ULONG)signal_bit,
                     __ATOMIC_RELAXED);
    if (KrnGetCPUNumber() != worker->hart)
        __atomic_store_n(&worker->ready_error, 2, __ATOMIC_RELAXED);
    __atomic_store_n(&worker->ready, 1, __ATOMIC_RELEASE);
    Signal(shared->controller, shared->controller_signal);

    for (;;)
    {
        ULONG epoch;
        ULONG seed;
        ULONG value;
        ULONG expected;
        ULONG i;
        ULONG start_hart;
        BOOL cancelled = FALSE;

        if (__atomic_load_n(&shared->stop, __ATOMIC_ACQUIRE))
            break;

        Wait(1UL << signal_bit);
        if (__atomic_load_n(&shared->stop, __ATOMIC_ACQUIRE))
            break;

        epoch = __atomic_load_n(&shared->epoch, __ATOMIC_ACQUIRE);
        seed = pq_lcg_seed(pq_report.nonce, worker->hart, epoch);
        start_hart = KrnGetCPUNumber();
        value = seed;

        for (i = 0; i < PQ_LCG_STEPS; ++i)
        {
            value = value * PQ_LCG_MULTIPLIER + PQ_LCG_INCREMENT;
            if (!(i & PQ_LCG_POLL_MASK) &&
                __atomic_load_n(&shared->stop, __ATOMIC_ACQUIRE))
            {
                cancelled = TRUE;
                break;
            }
        }

        if (cancelled)
            break;

        expected = pq_lcg_advance(seed, PQ_LCG_STEPS);
        if (start_hart != worker->hart ||
            KrnGetCPUNumber() != worker->hart || value != expected)
        {
            __atomic_add_fetch(&pq_report.cpu_errors[worker->hart], 1,
                               __ATOMIC_RELAXED);
        }
        else
        {
            __atomic_add_fetch(&pq_report.cpu_batches[worker->hart], 1,
                               __ATOMIC_RELAXED);
        }

        __atomic_store_n(&worker->done_epoch, epoch, __ATOMIC_RELEASE);
        Signal(shared->controller, shared->controller_signal);
    }

    pq_worker_exit(worker, signal_bit);
}

static BOOL pq_wait_worker_ready(UQUAD start, ULONG timeout_seconds)
{
    UQUAD now;

    while (!__atomic_load_n(&pq_workers[0].ready, __ATOMIC_ACQUIRE) ||
           !__atomic_load_n(&pq_workers[1].ready, __ATOMIC_ACQUIRE))
    {
        ULONG sigs;

        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
        {
            pq_report.reason = PQ_REASON_CTRL_C;
            return FALSE;
        }
        pq_clear_controller_signal();
        Delay(1);
        pq_read_eclock(&now);
        if (now - start > (UQUAD)timeout_seconds * pq_eclock_hz)
        {
            pq_report.reason = PQ_REASON_WORKER_START;
            return FALSE;
        }
        sigs = SetSignal(0, 0);
        (void)sigs;
    }

    if (__atomic_load_n(&pq_workers[0].ready_error, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&pq_workers[1].ready_error, __ATOMIC_ACQUIRE))
    {
        pq_report.reason = PQ_REASON_WORKER_START;
        return FALSE;
    }
    return TRUE;
}

static BOOL pq_handle_window_events(void)
{
    struct IntuiMessage *msg;
    BOOL close_requested = FALSE;

    if (!pq_window)
        return FALSE;

    while ((msg = (struct IntuiMessage *)GetMsg(pq_window->UserPort)) != NULL)
    {
        if (msg->Class == IDCMP_CLOSEWINDOW)
            close_requested = TRUE;
        ReplyMsg((struct Message *)msg);
    }

    if (close_requested)
    {
        pq_report.reason = PQ_REASON_WINDOW_CLOSED;
        return FALSE;
    }
    return TRUE;
}

static BOOL pq_draw_window(ULONG elapsed, BOOL count_update)
{
    char line[112];
    struct RastPort *rp;
    ULONG y = 34;

    if (KrnGetCPUNumber() != 0)
    {
        pq_report.reason = PQ_REASON_GRAPHICS_HART;
        return FALSE;
    }
    if (!pq_window || !pq_window->RPort)
        return FALSE;

    rp = pq_window->RPort;
    SetAPen(rp, 0);
    RectFill(rp, 4, 18, pq_window->Width - 5, pq_window->Height - 5);
    SetAPen(rp, PQ_COLOR_TEXT);
    Move(rp, 14, y);
    Text(rp, "READ-ONLY PRODUCTION QUALIFICATION", 34);
    y += 22;

    snprintf(line, sizeof(line), "Run %08lx   state %s   %s",
             pq_report.nonce, pq_state_name(pq_report.state),
             pq_smoke ? "SMOKE ONLY: NEVER A QUALIFICATION PASS" :
                        "Close this window to abort safely");
    Move(rp, 14, y);
    Text(rp, line, strlen(line));
    y += 20;

    snprintf(line, sizeof(line), "Elapsed %lu / %lu seconds",
             elapsed, pq_report.goal_seconds);
    Move(rp, 14, y);
    Text(rp, line, strlen(line));
    y += 20;

    snprintf(line, sizeof(line), "Verified CPU batches: hart 0 %lu   hart 1 %lu",
             __atomic_load_n(&pq_report.cpu_batches[0], __ATOMIC_ACQUIRE),
             __atomic_load_n(&pq_report.cpu_batches[1], __ATOMIC_ACQUIRE));
    Move(rp, 14, y);
    Text(rp, line, strlen(line));
    y += 20;

    snprintf(line, sizeof(line), "SD READ64: full %lu  mismatch %lu  errors %lu",
             pq_report.sd_reads, pq_report.sd_mismatches,
             pq_report.sd_errors);
    Move(rp, 14, y);
    Text(rp, line, strlen(line));
    y += 20;

    snprintf(line, sizeof(line), "Workbench window updates %lu",
             pq_report.gfx_updates);
    Move(rp, 14, y);
    Text(rp, line, strlen(line));

    SetAPen(rp, PQ_COLOR_BAR);
    RectFill(rp, 14, pq_window->Height - 22,
             14 + (LONG)((elapsed % 100UL) * 5UL),
             pq_window->Height - 14);

    if (count_update)
        ++pq_report.gfx_updates;
    return TRUE;
}

static BOOL pq_read_one_range(ULONG read_index)
{
    const struct PQReference *ref =
        &pq_references[read_index % QUAL_CARD_RANGE_COUNT];
    UQUAD byte_offset = (UQUAD)ref->lba * PQ_SECTOR_BYTES;
    ULONG hash;

    pq_sd_io->io_Command = NSCMD_TD_READ64;
    pq_sd_io->io_Data = pq_sd_buffer;
    pq_sd_io->io_Length = PQ_READ_BYTES;
    pq_sd_io->io_Actual = (ULONG)(byte_offset >> 32);
    pq_sd_io->io_Offset = (ULONG)byte_offset;
    if (DoIO((struct IORequest *)pq_sd_io) || pq_sd_io->io_Error ||
        pq_sd_io->io_Actual != PQ_READ_BYTES)
    {
        ++pq_report.sd_errors;
        pq_report.reason = PQ_REASON_SD_READ;
        return FALSE;
    }

    ++pq_report.sd_reads;
    hash = pq_fnv1a(pq_sd_buffer, PQ_READ_BYTES);
    if (hash != ref->fnv32)
    {
        ++pq_report.sd_mismatches;
        pq_report.reason = PQ_REASON_SD_MISMATCH;
        return FALSE;
    }
    return TRUE;
}

static BOOL pq_wait_for_batch(ULONG epoch, UQUAD batch_start,
                              UQUAD run_deadline)
{
    UQUAD now;

    while (__atomic_load_n(&pq_workers[0].done_epoch, __ATOMIC_ACQUIRE) != epoch ||
           __atomic_load_n(&pq_workers[1].done_epoch, __ATOMIC_ACQUIRE) != epoch)
    {
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
        {
            pq_report.reason = PQ_REASON_CTRL_C;
            return FALSE;
        }
        if (!pq_handle_window_events())
            return FALSE;
        pq_clear_controller_signal();
        Delay(1);
        pq_read_eclock(&now);
        if (now >= run_deadline)
        {
            pq_report.reason = PQ_REASON_DURATION;
            return FALSE;
        }
        if (now - batch_start > (UQUAD)PQ_WORKER_TIMEOUT_SECONDS * pq_eclock_hz)
        {
            pq_report.reason = PQ_REASON_WORKER_TIMEOUT;
            return FALSE;
        }
    }

    if (__atomic_load_n(&pq_report.cpu_errors[0], __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&pq_report.cpu_errors[1], __ATOMIC_ACQUIRE))
    {
        pq_report.reason = PQ_REASON_WORKER_MISMATCH;
        return FALSE;
    }
    return TRUE;
}

static ULONG pq_required_coverage(ULONG elapsed_seconds)
{
    return (elapsed_seconds + 1UL) / 2UL;
}

static BOOL pq_run(UQUAD *run_start_out, UQUAD *run_end_out)
{
    UQUAD start;
    UQUAD now;
    UQUAD last_batch;
    UQUAD last_gfx;
    UQUAD last_sd;
    UQUAD next_gfx;
    UQUAD next_sd;
    UQUAD run_deadline;
    ULONG epoch = 0;
    ULONG last_read_index = 0;
    ULONG goal_seconds;

    pq_eclock_hz = pq_read_eclock(&start);
    if (!pq_eclock_hz)
    {
        pq_report.reason = PQ_REASON_ECLOCK;
        return FALSE;
    }
    *run_start_out = start;
    pq_report.state = PQ_STATE_RUNNING;
    pq_draw_window(0, FALSE);

    last_batch = start;
    last_gfx = start;
    last_sd = start;
    next_gfx = start;
    next_sd = start;
    goal_seconds = pq_report.goal_seconds;
    run_deadline = start +
        (UQUAD)(goal_seconds + PQ_COVERAGE_GRACE_SECONDS) * pq_eclock_hz;

    for (;;)
    {
        UQUAD batch_start;
        ULONG elapsed;
        ULONG needed;
        BOOL coverage_done;

        pq_read_eclock(&now);
        elapsed = pq_elapsed_seconds(start, now);
        pq_report.duration_seconds = elapsed;

        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
        {
            pq_report.reason = PQ_REASON_CTRL_C;
            pq_report.state = PQ_STATE_ABORT;
            break;
        }
        if (!pq_handle_window_events())
        {
            pq_report.state = PQ_STATE_ABORT;
            break;
        }

        if (now >= run_deadline)
        {
            pq_report.reason = PQ_REASON_DURATION;
            pq_report.state = PQ_STATE_INCOMPLETE;
            break;
        }

        if (now - last_batch > (UQUAD)PQ_WORKER_TIMEOUT_SECONDS * pq_eclock_hz)
        {
            pq_report.reason = PQ_REASON_WORKER_TIMEOUT;
            pq_report.state = PQ_STATE_FAIL;
            break;
        }
        if (now - last_gfx > (UQUAD)PQ_WORKER_TIMEOUT_SECONDS * pq_eclock_hz)
        {
            pq_report.reason = PQ_REASON_GRAPHICS_LATE;
            pq_report.state = PQ_STATE_FAIL;
            break;
        }
        if (now - last_sd > (UQUAD)(PQ_READ_PERIOD_SECONDS + 2UL) * pq_eclock_hz)
        {
            pq_report.reason = PQ_REASON_SD_LATE;
            pq_report.state = PQ_STATE_FAIL;
            break;
        }

        needed = pq_required_coverage(elapsed);
        coverage_done = elapsed >= goal_seconds &&
            __atomic_load_n(&pq_report.cpu_batches[0], __ATOMIC_ACQUIRE) >= needed &&
            __atomic_load_n(&pq_report.cpu_batches[1], __ATOMIC_ACQUIRE) >= needed &&
            pq_report.sd_reads >= needed && pq_report.gfx_updates >= needed;
        if (coverage_done)
        {
            *run_end_out = now;
            break;
        }

        ++epoch;
        __atomic_store_n(&pq_shared.epoch, epoch, __ATOMIC_RELEASE);
        __atomic_store_n(&pq_workers[0].done_epoch, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&pq_workers[1].done_epoch, 0, __ATOMIC_RELAXED);
        batch_start = now;
        Signal(pq_tasks[0], 1UL << pq_workers[0].work_signal_bit);
        Signal(pq_tasks[1], 1UL << pq_workers[1].work_signal_bit);

        /* Main-task READ64 and HIDD drawing overlap the two worker batches. */
        pq_read_eclock(&now);
        if (now >= next_sd || pq_report.sd_reads < needed)
        {
            ULONG index = last_read_index++;
            UQUAD read_start = now;

            if (!pq_read_one_range(index))
            {
                pq_report.state = PQ_STATE_FAIL;
                break;
            }
            pq_read_eclock(&now);
            if (now - read_start >
                (UQUAD)PQ_WORKER_TIMEOUT_SECONDS * pq_eclock_hz)
            {
                pq_report.reason = PQ_REASON_SD_LATE;
                pq_report.state = PQ_STATE_FAIL;
                break;
            }
            last_sd = now;
            next_sd = now + (UQUAD)PQ_READ_PERIOD_SECONDS * pq_eclock_hz;
        }

        pq_read_eclock(&now);
        elapsed = pq_elapsed_seconds(start, now);
        needed = pq_required_coverage(elapsed);
        if (now >= next_gfx || pq_report.gfx_updates < needed)
        {
            UQUAD gfx_start = now;

            if (!pq_draw_window(elapsed, TRUE))
            {
                pq_report.state = PQ_STATE_FAIL;
                break;
            }
            pq_read_eclock(&now);
            if (now - gfx_start >
                (UQUAD)PQ_WORKER_TIMEOUT_SECONDS * pq_eclock_hz)
            {
                pq_report.reason = PQ_REASON_GRAPHICS_LATE;
                pq_report.state = PQ_STATE_FAIL;
                break;
            }
            last_gfx = now;
            next_gfx = now + (UQUAD)PQ_GFX_PERIOD_SECONDS * pq_eclock_hz;
        }

        if (!pq_wait_for_batch(epoch, batch_start, run_deadline))
        {
            if (pq_report.reason == PQ_REASON_CTRL_C ||
                pq_report.reason == PQ_REASON_WINDOW_CLOSED)
                pq_report.state = PQ_STATE_ABORT;
            else if (pq_report.reason == PQ_REASON_DURATION)
                pq_report.state = PQ_STATE_INCOMPLETE;
            else
                pq_report.state = PQ_STATE_FAIL;
            break;
        }
        pq_read_eclock(&last_batch);
    }

    pq_read_eclock(run_end_out);
    pq_report.duration_seconds = pq_elapsed_seconds(start, *run_end_out);
    return pq_report.state == PQ_STATE_RUNNING;
}

static BOOL pq_stop_workers(void)
{
    UQUAD start = 0;
    UQUAD now = 0;
    BOOL all_stopped = TRUE;
    ULONG i;

    __atomic_store_n(&pq_shared.stop, 1, __ATOMIC_RELEASE);
    for (i = 0; i < 2; ++i)
    {
        ULONG signal_bit = __atomic_load_n(&pq_workers[i].work_signal_bit,
                                           __ATOMIC_ACQUIRE);
        if (pq_tasks[i] && signal_bit < 32)
            Signal(pq_tasks[i], 1UL << signal_bit);
    }

    if (pq_eclock_hz)
        pq_read_eclock(&start);
    while (pq_tasks[0] || pq_tasks[1])
    {
        all_stopped = TRUE;
        for (i = 0; i < 2; ++i)
            if (pq_tasks[i] &&
                !__atomic_load_n(&pq_workers[i].stopped, __ATOMIC_ACQUIRE))
                all_stopped = FALSE;
        if (all_stopped)
            break;
        pq_clear_controller_signal();
        Delay(1);
        if (pq_eclock_hz)
        {
            pq_read_eclock(&now);
            if (now - start > (UQUAD)PQ_STOP_TIMEOUT_SECONDS * pq_eclock_hz)
                break;
        }
    }

    Forbid();
    for (i = 0; i < 2; ++i)
    {
        if (pq_tasks[i])
        {
            RemTask(pq_tasks[i]);
            pq_tasks[i] = NULL;
        }
    }
    Permit();

    if (!all_stopped)
    {
        pq_report.reason = PQ_REASON_WORKER_TIMEOUT;
        pq_report.state = PQ_STATE_INCOMPLETE;
    }
    return all_stopped;
}

static void pq_close_io(void)
{
    if (pq_sd_io)
    {
        if (pq_sd_opened)
        {
            CloseDevice((struct IORequest *)pq_sd_io);
            pq_sd_opened = FALSE;
        }
        DeleteIORequest((struct IORequest *)pq_sd_io);
        pq_sd_io = NULL;
    }
    if (pq_sd_port)
    {
        DeleteMsgPort(pq_sd_port);
        pq_sd_port = NULL;
    }
    if (pq_sd_raw)
    {
        FreeMem(pq_sd_raw, PQ_BUFFER_BYTES);
        pq_sd_raw = NULL;
        pq_sd_buffer = NULL;
    }

    if (pq_timer_io)
    {
        if (pq_timer_opened)
        {
            CloseDevice((struct IORequest *)pq_timer_io);
            pq_timer_opened = FALSE;
        }
        DeleteIORequest((struct IORequest *)pq_timer_io);
        pq_timer_io = NULL;
        TimerBase = NULL;
    }
    if (pq_timer_port)
    {
        DeleteMsgPort(pq_timer_port);
        pq_timer_port = NULL;
    }
}

static void pq_close_graphics(void)
{
    if (pq_window)
    {
        CloseWindow(pq_window);
        pq_window = NULL;
    }
    if (GfxBase)
    {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = NULL;
    }
    if (IntuitionBase)
    {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
}

static void pq_publish_records(void)
{
    bug("[pq] id=%08lx state=%s\n", pq_report.nonce,
        (IPTR)pq_state_name(pq_report.state));
    bug("[pq] id=%08lx duration=%lu goal=%lu\n", pq_report.nonce,
        pq_report.duration_seconds, pq_report.goal_seconds);
    bug("[pq] id=%08lx cpu0=%lu cpu1=%lu\n", pq_report.nonce,
        pq_report.cpu_batches[0], pq_report.cpu_batches[1]);
    bug("[pq] id=%08lx cpuerr0=%lu cpuerr1=%lu\n", pq_report.nonce,
        pq_report.cpu_errors[0], pq_report.cpu_errors[1]);
    bug("[pq] id=%08lx sdreads=%lu\n", pq_report.nonce, pq_report.sd_reads);
    bug("[pq] id=%08lx sdmismatch=%lu\n", pq_report.nonce,
        pq_report.sd_mismatches);
    bug("[pq] id=%08lx sderrors=%lu\n", pq_report.nonce,
        pq_report.sd_errors);
    bug("[pq] id=%08lx gfxupdates=%lu\n", pq_report.nonce,
        pq_report.gfx_updates);
    bug("[pq] id=%08lx reason=%lu fixture=%lu\n", pq_report.nonce,
        pq_report.reason, pq_report.fixture_verified);
}

static void pq_report_wait_five_seconds(void)
{
    ULONG ticks = pq_vblank_hz ? pq_vblank_hz * PQ_REPORT_PERIOD_SECONDS : 500UL;
    ULONG i;

    for (i = 0; i < ticks; ++i)
    {
        if (pq_window)
        {
            struct IntuiMessage *msg;
            while ((msg = (struct IntuiMessage *)GetMsg(pq_window->UserPort)) != NULL)
            {
                if (msg->Class == IDCMP_CLOSEWINDOW)
                {
                    ReplyMsg((struct Message *)msg);
                    CloseWindow(pq_window);
                    pq_window = NULL;
                    break;
                }
                ReplyMsg((struct Message *)msg);
            }
        }
        pq_clear_controller_signal();
        Delay(1);
    }
}

static void pq_retrieve_window(void)
{
    ULONG elapsed;

    pq_publish_records();
    for (elapsed = 0; elapsed < PQ_REPORT_SECONDS;
         elapsed += PQ_REPORT_PERIOD_SECONDS)
    {
        pq_report_wait_five_seconds();
        pq_publish_records();
    }
}

static void pq_finish_state(BOOL run_completed)
{
    ULONG elapsed = pq_report.duration_seconds;
    ULONG required = pq_required_coverage(elapsed);
    BOOL clean = run_completed && pq_report.reason == PQ_REASON_NONE &&
        elapsed >= pq_report.goal_seconds && pq_report.fixture_verified &&
        pq_report.cpu_batches[0] >= required &&
        pq_report.cpu_batches[1] >= required &&
        pq_report.sd_reads >= required && pq_report.gfx_updates >= required &&
        pq_report.cpu_errors[0] == 0 && pq_report.cpu_errors[1] == 0 &&
        pq_report.sd_mismatches == 0 && pq_report.sd_errors == 0;

    if (clean)
        pq_report.state = pq_smoke ? PQ_STATE_SMOKE : PQ_STATE_PASS;
    else if (pq_report.state == PQ_STATE_RUNNING ||
             pq_report.state == PQ_STATE_PREPARING)
    {
        pq_report.state = PQ_STATE_FAIL;
        if (pq_report.reason == PQ_REASON_NONE)
            pq_report.reason = PQ_REASON_COVERAGE;
    }
}

int main(int argc, char **argv)
{
    UQUAD run_start = 0;
    UQUAD run_end = 0;
    UQUAD nonce_clock = 0;
    BOOL run_completed = FALSE;
    BOOL ready = FALSE;
    ULONG i;
    int result = RETURN_FAIL;

    memset(&pq_report, 0, sizeof(pq_report));
    memset(&pq_shared, 0, sizeof(pq_shared));
    memset(pq_workers, 0, sizeof(pq_workers));
    memset(pq_tasks, 0, sizeof(pq_tasks));
    pq_report.state = PQ_STATE_PREPARING;
    pq_main_done_bit = 0xffffffffUL;
    {
        ULONG cycles;
        asm volatile("csrr %0, mcycle" : "=r"(cycles));
        pq_report.nonce = cycles ^ (ULONG)(IPTR)FindTask(NULL) ^
                          (ULONG)SysBase->VBlankFrequency;
        if (!pq_report.nonce)
            pq_report.nonce = 0x50513451UL;
    }

    if (!pq_parse_seconds(argc, argv, &pq_report.goal_seconds, &pq_smoke))
    {
        pq_report.reason = PQ_REASON_ARGUMENTS;
        goto cleanup;
    }

    KernelBase = OpenResource("kernel.resource");
    if (!KernelBase)
    {
        pq_report.reason = PQ_REASON_KERNEL_RESOURCE;
        goto cleanup;
    }
    if (KrnGetCPUCount() < 2)
    {
        pq_report.reason = PQ_REASON_ONE_HART;
        goto cleanup;
    }
    if (KrnGetCPUNumber() != 0)
    {
        pq_report.reason = PQ_REASON_MAIN_HART;
        goto cleanup;
    }

    pq_main_done_bit = AllocSignal(-1);
    if ((LONG)pq_main_done_bit < 0)
    {
        pq_report.reason = PQ_REASON_SIGNAL;
        goto cleanup;
    }
    pq_done_mask = 1UL << pq_main_done_bit;
    pq_shared.controller = FindTask(NULL);
    pq_shared.controller_signal = pq_done_mask;

    pq_vblank_hz = SysBase->VBlankFrequency;
    if (!pq_vblank_hz)
        pq_vblank_hz = 100;

    if (!pq_open_timer())
        goto cleanup;

    pq_read_eclock(&nonce_clock);
    pq_report.nonce ^= (ULONG)nonce_clock ^ (ULONG)(nonce_clock >> 32);

    if (!pq_open_window())
        goto cleanup;
    pq_draw_window(0, FALSE);

    if (!pq_open_sd())
        goto cleanup;

    pq_workers[0].shared = &pq_shared;
    pq_workers[1].shared = &pq_shared;
    pq_workers[0].hart = 0;
    pq_workers[1].hart = 1;
    pq_workers[0].work_signal_bit = 0xffffffffUL;
    pq_workers[1].work_signal_bit = 0xffffffffUL;

    for (i = 0; i < 2; ++i)
    {
        pq_tasks[i] = pq_create_worker(i);
        if (!pq_tasks[i])
        {
            pq_report.reason = PQ_REASON_WORKER_CREATE;
            goto cleanup;
        }
    }

    {
        UQUAD ready_start;
        pq_read_eclock(&ready_start);
        ready = pq_wait_worker_ready(ready_start, PQ_STOP_TIMEOUT_SECONDS);
    }
    if (!ready)
        goto cleanup;

    if (!pq_run(&run_start, &run_end))
        goto cleanup;
    run_completed = TRUE;

cleanup:
    if (pq_tasks[0] || pq_tasks[1])
    {
        if (!pq_stop_workers())
        {
            run_completed = FALSE;
            pq_report.reason = PQ_REASON_WORKER_TIMEOUT;
            pq_report.state = PQ_STATE_INCOMPLETE;
        }
    }

    if (pq_report.reason == PQ_REASON_CTRL_C ||
        pq_report.reason == PQ_REASON_WINDOW_CLOSED)
        pq_report.state = PQ_STATE_ABORT;

    if (run_start && run_end)
        pq_report.duration_seconds = pq_elapsed_seconds(run_start, run_end);

    pq_finish_state(run_completed);

    if (pq_window && KrnGetCPUNumber() == 0)
    {
        pq_draw_window(pq_report.duration_seconds, FALSE);
        {
            char final_line[96];
            snprintf(final_line, sizeof(final_line), "%s: %s",
                     pq_state_name(pq_report.state),
                     pq_reason_name(pq_report.reason));
            SetAPen(pq_window->RPort, PQ_COLOR_TEXT);
            Move(pq_window->RPort, 14, pq_window->Height - 34);
            Text(pq_window->RPort, final_line, strlen(final_line));
        }
    }

    pq_close_io();
    if (pq_main_done_bit < 32)
    {
        FreeSignal((LONG)pq_main_done_bit);
        pq_main_done_bit = 0xffffffffUL;
    }

    /* The report is first emitted after every worker has been removed. */
    pq_retrieve_window();

    if (pq_report.state == PQ_STATE_PASS)
        result = RETURN_OK;
    else if (pq_report.state == PQ_STATE_SMOKE)
        result = RETURN_WARN;

    pq_close_graphics();
    return result;
}
