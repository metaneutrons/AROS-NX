/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Native opt-in SDIO reset/bootstrap and setup/restoration. An additional
 * explicit diagnostic enables bounded OPEN/INIT with Fn0/CLOSE cleanup.
 * Optional one-shot Init/STA/Start/GetMAC follows the startup reply. This is not a WLAN
 * data-plane client. Separate CCCR reset/query diagnostic; no C6 flash writes. */
#include <aros/asmcall.h>
#include <aros/debug.h>
#include <aros/symbolsets.h>
#include <exec/resident.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dosextens.h>
#include <utility/tagitem.h>
#include "../board/board.h"
#include "../sdmmc/command.h"
#include "../sdmmc/registers.h"
#include "../sdmmc/memory.h"
#include "runtime_probe.h"
#include "sdio_probe.h"
#include "sdio_commands.h"
#if P4_WIFI_HOSTED_INIT
#if !P4_WIFI_SDIO_SETUP || P4_WIFI_SDIO_RESET_QUERY
#error "Hosted INIT requires setup and must not use reset-query"
#endif
#include "hosted_open.h"
#endif
#if P4_WIFI_HOSTED_REPLY
#if !P4_WIFI_HOSTED_INIT
#error "Hosted reply requires the explicit INIT gate"
#endif
#include "hosted_config.h"
#include "hosted_tx.h"
#endif
#if P4_WIFI_HOSTED_OWNER
#if !P4_WIFI_HOSTED_REPLY || P4_WIFI_HOSTED_SCAN || P4_WIFI_HOSTED_MAC
#error "Hosted owner requires reply and excludes one-shot MAC/scan diagnostics"
#endif
#include "hosted_owner.h"
#include "hosted_wifi.h"
#include "hosted_mac.h"
#include <proto/kernel.h>
#include <proto/task.h>
#ifndef P4_WIFI_OWNER_CPU
#define P4_WIFI_OWNER_CPU 1
#endif
#if P4_WIFI_OWNER_CPU < 0 || P4_WIFI_OWNER_CPU > 1
#error "P4 owner CPU must be 0 or 1"
#endif
#endif
#if P4_WIFI_HOSTED_MAC
#if !P4_WIFI_HOSTED_REPLY
#error "Hosted MAC requires the explicit startup reply gate"
#endif
#include "hosted_startup.h"
/* Exact successful reference-app IDF6 profile, preprocessed from its actual
 * build. NVS is explicitly disabled by reference.c. This is source-derived
 * compatibility, not installed C6 binary lineage or a generic IDF default. */
#endif
#if P4_WIFI_HOSTED_MAC || P4_WIFI_HOSTED_OWNER
static const struct p4_hosted_wifi_config wifi_reference_profile = {
    10, 32, 0, 16, 0, 0, 0, 1, 1, 0, 0, 0, 6, 0, 752, 32,
    UINT64_C(129), 1, 7, INT32_C(0x1f2f3f4f)
};
#endif
#if P4_WIFI_HOSTED_SCAN
#if !P4_WIFI_HOSTED_MAC
#error "Hosted scan requires successful typed WiFi initialization and MAC"
#endif
#include "hosted_scan_sequence.h"
#endif
#if P4_WIFI_SDIO_RESET_QUERY
#include "sdio_reset_query.h"
#if P4_WIFI_SDIO_SETUP
#error "Reset-query diagnostic must not enable function setup"
#endif
#endif
#if P4_WIFI_SDIO_SETUP
#include "sdio_setup.h"
#include "sdio_restore.h"
#endif
#include LC_LIBDEFS_FILE

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
static struct P4WifiBase *wifi_resource_base;

/* The noexpunge device registers its permanent unit, not an opener. Closing
 * an opener therefore never waits for registration or transport ownership. */
static BOOL wifi_device_attach(struct P4WifiBase *base, APTR client,
    const struct p4_wifi_device_callbacks *callbacks)
{
    BOOL accepted = FALSE;
    if (!base || !client || !callbacks || !callbacks->service ||
        !callbacks->frame || !callbacks->ready || !callbacks->stopped ||
        !AttemptSemaphore(&base->client_lock))
        return FALSE;
    if (base->ready && base->runtime && base->owner_task && !base->client) {
        base->client = client;
        base->callbacks = callbacks;
        callbacks->ready(client, base->mac);
        accepted = TRUE;
    }
    ReleaseSemaphore(&base->client_lock);
    return accepted;
}

/* Not a CloseDevice hook. Reserved for teardown outside Forbid, after client
 * queues have retired; permanent device registration normally never detaches. */
static void wifi_device_detach(struct P4WifiBase *base, APTR client)
{
    if (!base || !client) return;
    ObtainSemaphore(&base->client_lock);
    if (base->client == client) {
        base->client = NULL;
        base->callbacks = NULL;
    }
    ReleaseSemaphore(&base->client_lock);
}

static BOOL wifi_device_send(struct P4WifiBase *, const UBYTE *, ULONG);

#if P4_WIFI_HOSTED_OWNER
APTR KernelBase;
APTR TaskResBase;

/* SetTaskAffinity copies the mask; unlike a transferred NP_Affinity mask,
 * the caller can free it on either success or failure. Pin only this radio
 * process, before any controller access; leave legacy DOS/GUI placement alone. */
static int wifi_owner_pin(void)
{
#if defined(__AROSEXEC_SMP__)
    APTR mask;
    BOOL changed;
    KernelBase = OpenResource("kernel.resource");
    TaskResBase = OpenResource("task.resource");
    if (!KernelBase || !TaskResBase || KrnGetCPUCount() <= P4_WIFI_OWNER_CPU)
        return 0;
    mask = KrnAllocCPUMask();
    if (!mask)
        return 0;
    KrnClearCPUMask(mask);
    KrnGetCPUMask(P4_WIFI_OWNER_CPU, mask);
    changed = SetTaskAffinity(NULL, mask);
    KrnFreeCPUMask(mask);
    if (!changed)
        return 0;
    Delay(1); /* Scheduling point before verifying the actual running hart. */
    return KrnGetCPUNumber() == P4_WIFI_OWNER_CPU;
#else
    return P4_WIFI_OWNER_CPU == 0;
#endif
}
#endif

#if P4_BOARD_WIFI_SDIO
#define WIFI_GPIO_BASE  (P4_HPPERIPH1_BASE + 0x20000)
#define WIFI_IOMUX_BASE (P4_HPPERIPH1_BASE + 0x21000)

struct wifi_probe_context
{
    struct P4SDMMCHost *host;
    ULONG generation;
    ULONG peer_clock;
    struct P4HostCommandResult last;
#if P4_WIFI_HOSTED_OWNER
    ULONG owner_events;
    ULONG owner_last_event;
    ULONG owner_boot_start;
    ULONG owner_open_start;
    BOOL owner_cleanup;
#endif
#if P4_WIFI_SDIO_RESET_QUERY
    ULONG rejected_r5;
    BOOL rejected_r5_valid;
#endif
};

static ULONG wifi_read(IPTR address)
{
    return *(volatile ULONG *)address;
}

static void wifi_write(IPTR address, ULONG value)
{
    *(volatile ULONG *)address = value;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

/* ESP-IDF f80e6467, esp32p4 register/hw_ver1 GPIO/IO_MUX contracts.
 * Only the profile's radio pins are touched; storage pins remain untouched.
 * Clock output0, CMD input/output1, D0 input/output2. Matrix peripheral OEN.
 * Internal pulls are not proof of sufficient external electrical pull-ups. */
static void wifi_pin(unsigned pin, BOOL output, unsigned signal)
{
    IPTR mux = WIFI_IOMUX_BASE + 4 + pin * 4;
    ULONG value = wifi_read(mux);
    value &= ~((1UL << 7) | (3UL << 10) | (7UL << 12));
    value |= (1UL << 8) | (1UL << 9) | (2UL << 10) | (1UL << 12);
    wifi_write(mux, value);
    /* GPIO_ENABLE W1TC/W1TS touches this pin only. */
    wifi_write(WIFI_GPIO_BASE + 0x28, 1UL << pin);
    wifi_write(WIFI_GPIO_BASE + 0x74 + pin * 4,
               wifi_read(WIFI_GPIO_BASE + 0x74 + pin * 4) & ~(1UL << 2));
    if (output)
    {
        wifi_write(WIFI_GPIO_BASE + 0x558 + pin * 4, signal);
        wifi_write(WIFI_GPIO_BASE + 0x24, 1UL << pin);
    }
}

static void wifi_configure_pins(void)
{
    wifi_pin(P4_BOARD_WIFI_CLK_GPIO, TRUE, 0);
    wifi_pin(P4_BOARD_WIFI_CMD_GPIO, TRUE, 1);
    wifi_pin(P4_BOARD_WIFI_D0_GPIO, TRUE, 2);
    wifi_pin(P4_BOARD_WIFI_D1_GPIO, FALSE, 0);
    wifi_pin(P4_BOARD_WIFI_D2_GPIO, FALSE, 0);
    wifi_pin(P4_BOARD_WIFI_D3_GPIO, FALSE, 0);
    wifi_write(WIFI_GPIO_BASE + 0x15c, P4_BOARD_WIFI_CMD_GPIO | (1UL << 7));
    wifi_write(WIFI_GPIO_BASE + 0x160, P4_BOARD_WIFI_D0_GPIO | (1UL << 7));
    /* IDF no-CD-pin contract: slot1 is permanently present. Endpoint127,
     * constant zero0x3e, matrix enabled. This is not the slave reset pin.
     * Leave card interrupt routing unchanged in this single-variable test. */
    wifi_write(WIFI_GPIO_BASE + 0x354, 0x3e | (1UL << 7));
    /* Only slot1 width and DDR bits; no PWREN/UHS voltage field on P4 v1.3. */
    wifi_write(P4_SDMMC_BASE + P4SD_CTYPE,
               wifi_read(P4_SDMMC_BASE + P4SD_CTYPE) & ~0x00020002UL);
    wifi_write(P4_SDMMC_BASE + 0x74,
               wifi_read(P4_SDMMC_BASE + 0x74) & ~(1UL << 17));
    wifi_write(P4_SDMMC_BASE + P4SD_RST_N,
               wifi_read(P4_SDMMC_BASE + P4SD_RST_N) | (1UL << 1));
}

/* Read-only snapshots taken while owned, then reported after release.
 * Index order: CLKDIV, CLKSRC, CLKENA, CTYPE, RST_N, UHS, STATUS, CMD,
 * CMDARG, RINTSTS, GPIO_ENABLE, GPIO_IN, CMD_IN, D0_IN, CLK_OUT, CMD_OUT,
 * D0_OUT, CLK_MUX, CMD_MUX, D0_MUX, CDETECT, CD_IN, INT_IN, WP_IN.
 * Registers, not electrical waveforms. */
#define WIFI_SNAPSHOT_WORDS 24
static void wifi_snapshot(ULONG *words)
{
    static const ULONG offsets[] = {
        P4SD_CLKDIV, P4SD_CLKSRC, P4SD_CLKENA, P4SD_CTYPE, P4SD_RST_N,
        0x74, P4SD_STATUS, P4SD_CMD, P4SD_CMDARG, P4SD_RINTSTS
    };
    unsigned i;
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
        words[i] = wifi_read(P4_SDMMC_BASE + offsets[i]);
    words[10] = wifi_read(WIFI_GPIO_BASE + 0x20);
    words[11] = wifi_read(WIFI_GPIO_BASE + 0x3c);
    words[12] = wifi_read(WIFI_GPIO_BASE + 0x15c);
    words[13] = wifi_read(WIFI_GPIO_BASE + 0x160);
    words[14] = wifi_read(WIFI_GPIO_BASE + 0x558 + P4_BOARD_WIFI_CLK_GPIO * 4);
    words[15] = wifi_read(WIFI_GPIO_BASE + 0x558 + P4_BOARD_WIFI_CMD_GPIO * 4);
    words[16] = wifi_read(WIFI_GPIO_BASE + 0x558 + P4_BOARD_WIFI_D0_GPIO * 4);
    words[17] = wifi_read(WIFI_IOMUX_BASE + 4 + P4_BOARD_WIFI_CLK_GPIO * 4);
    words[18] = wifi_read(WIFI_IOMUX_BASE + 4 + P4_BOARD_WIFI_CMD_GPIO * 4);
    words[19] = wifi_read(WIFI_IOMUX_BASE + 4 + P4_BOARD_WIFI_D0_GPIO * 4);
    words[20] = wifi_read(P4_SDMMC_BASE + 0x50);
    words[21] = wifi_read(WIFI_GPIO_BASE + 0x354);
    words[22] = wifi_read(WIFI_GPIO_BASE + 0x35c);
    words[23] = wifi_read(WIFI_GPIO_BASE + 0x364);
}

static int wifi_budget(void *opaque)
{
    struct wifi_probe_context *context = opaque;
    return context->host->lock.ss_Owner == FindTask(NULL) &&
           context->host->generation == context->generation &&
           p4host_budget_ok(context->host);
}

static void wifi_yield(void *opaque)
{
    (void)opaque;
    /* One DOS tick is 20 ms: required after CMD0 as well as readiness polls.
     * Give the powered radio time to become ready. The shared aggregate
     * budget still caps the entire sequence; do not reset it per poll. */
    Delay(1);
}

#if P4_WIFI_SDIO_RESET_QUERY
/* Under command ownership, before another request can overwrite RESP0.
 * Keep rejected responses out of the host API's success-only payload fields.
 * No DMA, reset, allocation or UART operation in this observer. */
static void wifi_observe_rejected_r5(void *opaque,
    const struct P4HostCommand *request,
    enum P4HostCommandObservation observation,
    const struct P4HostCommandResult *outcome)
{
    struct wifi_probe_context *context = opaque;
    if (observation == P4HOST_OBSERVE_FINISH &&
        request->slot == P4_BOARD_WIFI_SLOT && request->index == 52 &&
        request->response_flags == MMC_RSP_R5 && !request->data &&
        !request->length && !request->writing &&
        outcome->started && outcome->status == P4HOST_COMMAND_CARD_ERROR &&
        (outcome->raw & P4SD_INT_CMD_DONE) &&
        !(outcome->raw & P4SD_INT_CMD_ERRORS))
    {
        context->rejected_r5 = wifi_read(P4_SDMMC_BASE + P4SD_RESP0);
        context->rejected_r5_valid = TRUE;
    }
}
#endif

/* Called only after the shared motor and generation/budget validation.
 * Hardware response timeout/CRC must be the only low command/data status
 * apart from CMD_DONE. Card errors, bus/DMA faults and refused/pre-start
 * requests remain unclassified failures. No rejected response is published. */
static int wifi_command_failure(const struct P4HostCommandResult *last)
{
    ULONG raw = last->raw & P4SD_INT_CMD_DATA_MASK;
    if (!last->started || last->idstatus ||
        last->injected_fault != P4HOST_FAULT_NONE)
        return 0;
    if (last->status == P4HOST_COMMAND_ERROR)
    {
        raw &= ~P4SD_INT_CMD_DONE;
        if (raw == P4SD_INT_RTO)
            return P4_SDIO_COMMAND_RESPONSE_TIMEOUT;
        if (raw == P4SD_INT_RCRC)
            return P4_SDIO_COMMAND_RESPONSE_CRC;
    }
    /* A started, unanswered command may hit the motor's polling deadline
     * without a peripheral RTO bit. Never tolerate pre-start/busy timeout. */
    if (last->status == P4HOST_COMMAND_TIMEOUT && raw == 0)
        return P4_SDIO_COMMAND_RESPONSE_TIMEOUT;
    return 0;
}

static int wifi_command(void *opaque, unsigned index, uint32_t argument,
                         enum p4_sdio_response_type response,
                         uint32_t error_mask, uint32_t *word)
{
    struct wifi_probe_context *context = opaque;
    struct P4HostCommand request;
    static const ULONG flags[] = {MMC_RSP_NONE, MMC_RSP_R4, MMC_RSP_R6,
                                  MMC_RSP_R1, MMC_RSP_R5};
    BOOL completed;
    if (!wifi_budget(context) || response > P4_SDIO_RESP_R5 ||
        response < P4_SDIO_RESP_NONE)
        return 0;
    p4host_zero(&request, sizeof(request));
    request.slot = P4_BOARD_WIFI_SLOT;
    request.generation = context->generation;
    request.index = index;
    request.argument = argument;
    request.response_flags = flags[response];
    request.response_error_mask = error_mask;
#if P4_WIFI_SDIO_RESET_QUERY
    if (index == 52)
    {
        request.observe = wifi_observe_rejected_r5;
        request.observe_context = context;
    }
#endif
    completed = context->host->command(context->host, &request, &context->last);
    if (!wifi_budget(context) || context->host->dma_faulted)
        return 0;
    if (!completed)
        return index == 52 && argument == UINT32_C(0x80000c08) &&
            response == P4_SDIO_RESP_R5 &&
            error_mask == P4_SDIO_PROBE_R5_ERROR_MASK ?
            wifi_command_failure(&context->last) : 0;
    *word = context->last.response[0];
    return 1;
}

#if P4_WIFI_SDIO_SETUP
/* CMD52 writes carry their byte in CMDARG, not a data-transfer phase.
 * wifi_command leaves request.writing/data/length zero accordingly. */
static int wifi_register_read(void *opaque, unsigned function,
                              uint32_t address, uint8_t *value)
{
    uint32_t argument, response;
    if (!value || !p4_sdio_cmd52(0, function, 0, address, 0, &argument) ||
        wifi_command(opaque, 52, argument, P4_SDIO_RESP_R5,
                      P4_SDIO_PROBE_R5_ERROR_MASK, &response) <= 0 ||
        (response & P4_SDIO_PROBE_R5_ERROR_MASK))
        return 0;
    *value = (uint8_t)response;
    return 1;
}

static int wifi_register_write(void *opaque, unsigned function,
                               uint32_t address, uint8_t value)
{
    uint32_t argument, response;
    return p4_sdio_cmd52(1, function, 0, address, value, &argument) &&
        wifi_command(opaque, 52, argument, P4_SDIO_RESP_R5,
                     P4_SDIO_PROBE_R5_ERROR_MASK, &response) > 0 &&
        !(response & P4_SDIO_PROBE_R5_ERROR_MASK);
}
#endif

#if P4_WIFI_HOSTED_INIT
/* All data phases use the same owned command motor as storage. Register
 * words use byte-mode4; FIFO uses the explicitly planned512-byte bus length.
 * The motor stages data in its permanent DMA bounce buffer; no stack DMA.
 * Failure does not retry a possibly consumed FIFO transaction. */
static int wifi_transfer(struct wifi_probe_context *context, ULONG argument,
                          UBYTE *data, ULONG length, ULONG block_size,
                          BOOL writing)
{
    struct P4HostCommand request;
    BOOL completed;
    if (!wifi_budget(context))
        return 0;
    p4host_zero(&request, sizeof(request));
    request.slot = P4_BOARD_WIFI_SLOT;
    request.generation = context->generation;
    request.index = 53;
    request.argument = argument;
    request.response_flags = MMC_RSP_R5;
    request.response_error_mask = P4_SDIO_PROBE_R5_ERROR_MASK;
    request.data = data;
    request.length = length;
    request.block_size = block_size;
    request.writing = writing;
    completed = context->host->command(context->host, &request, &context->last);
    return completed && wifi_budget(context) && !context->host->dma_faulted;
}

static int wifi_word_read(void *opaque, uint32_t address, uint32_t *word)
{
    uint32_t argument;
    UBYTE bytes[4];
    if (!word || !p4_sdio_cmd53(0, 1, 0, 1, address, 4, 0, &argument) ||
        !wifi_transfer(opaque, argument, bytes, 4, 4, FALSE))
        return 0;
    *word = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
            ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    return 1;
}

static int wifi_word_write(void *opaque, uint32_t address, uint32_t word)
{
    uint32_t argument;
    UBYTE bytes[4];
    bytes[0] = (UBYTE)word; bytes[1] = (UBYTE)(word >> 8);
    bytes[2] = (UBYTE)(word >> 16); bytes[3] = (UBYTE)(word >> 24);
    return p4_sdio_cmd53(1, 1, 0, 1, address, 4, 0, &argument) &&
        wifi_transfer(opaque, argument, bytes, 4, 4, TRUE);
}

static int wifi_fifo_read(void *opaque,
                          const struct p4_hosted_fifo_plan *plan,
                          uint8_t *buffer)
{
    uint32_t expected;
    /* Defense in depth at the runtime boundary: no write/other function or
     * arbitrary address supplied by the pure plan may reach the controller. */
    if (!plan || !buffer || !plan->logical_bytes ||
        plan->logical_bytes > P4_HOSTED_FIFO_BUS_MAX ||
        plan->bus_bytes != ((plan->logical_bytes + 511U) & ~UINT32_C(511)) ||
        !p4_sdio_cmd53(0, 1, 1, 1,
                       P4_HOSTED_FIFO_END - plan->logical_bytes,
                       plan->bus_bytes / 512U, 512, &expected) ||
        expected != plan->argument)
        return 0;
    return wifi_transfer(opaque, plan->argument, buffer, plan->bus_bytes,
                          512, FALSE);
}
#if P4_WIFI_HOSTED_REPLY
static int wifi_tx_session(void *opaque, uint32_t generation)
{
    struct wifi_probe_context *context = opaque;
    return generation == context->generation &&
        context->host->generation == generation &&
        context->host->lock.ss_Owner == FindTask(NULL) &&
        context->host->slot_clock[0] == context->peer_clock &&
        !context->host->dma_faulted;
}

/* Return authoritative motor facts, not merely a successful callback.
 * The motor has no partial-byte counter: full bus_bytes is justified only
 * by its complete DMA/data-over success contract. Started failures publish
 * zero completed bytes and must poison the FIFO; they are never retried. */
static int wifi_fifo_write(void *opaque,
                           const struct p4_hosted_fifo_plan *plan,
                           const uint8_t *buffer,
                           struct p4_hosted_tx_completion *completion)
{
    struct wifi_probe_context *context = opaque;
    struct P4HostCommand request;
    struct P4HostCommandResult outcome;
    uint32_t expected;
    BOOL completed;
    if (!plan || !buffer || !completion ||
        !plan->logical_bytes || plan->logical_bytes > 1536U ||
        plan->credits != 1U ||
        plan->bus_bytes != ((plan->logical_bytes + 511U) & ~UINT32_C(511)) ||
        !p4_sdio_cmd53(1, 1, 1, 1,
                       P4_HOSTED_FIFO_END - plan->logical_bytes,
                       plan->bus_bytes / 512U, 512, &expected) ||
        expected != plan->argument)
        return 0;
    p4host_zero(completion, sizeof(*completion));
    if (!wifi_tx_session(context, context->generation) || !wifi_budget(context))
        return 1; /* Known unstarted refusal, no stale prior command result. */
    p4host_zero(&request, sizeof(request));
    p4host_zero(&outcome, sizeof(outcome));
    request.slot = P4_BOARD_WIFI_SLOT;
    request.generation = context->generation;
    request.index = 53;
    request.argument = plan->argument;
    request.response_flags = MMC_RSP_R5;
    request.response_error_mask = P4_SDIO_PROBE_R5_ERROR_MASK;
    request.data = (UBYTE *)buffer; /* Motor stages writes into its bounce. */
    request.length = plan->bus_bytes;
    request.block_size = 512;
    request.writing = TRUE;
    completed = context->host->command(context->host, &request, &outcome);
    context->last = outcome;
    /* Refuse malformed BOOL/result combinations as ambiguous facts. */
    if ((completed != FALSE && completed != TRUE) ||
        (outcome.started != FALSE && outcome.started != TRUE) ||
        (completed && (!outcome.started || outcome.status != P4HOST_COMMAND_OK)))
        return 0;
    completion->started = (unsigned)outcome.started;
    completion->success = completed &&
        wifi_tx_session(context, context->generation) && wifi_budget(context);
    completion->completed_bytes = completion->success ? plan->bus_bytes : 0;
    return 1;
}
#endif
#endif

#if P4_WIFI_HOSTED_OWNER
struct wifi_owner_control {
    struct p4_hosted_owner_ops ops;
    struct p4_hosted_rx_ops rx_ops;
    struct p4_hosted_owner_result result;
    struct p4_hosted_mac mac;
    struct p4_hosted_tx_result frame_tx;
    uint8_t request[256];
    uint32_t uid;
    int32_t status;
    unsigned init_attempted, start_attempted, startup_ok, cleanup_ok;
};
struct wifi_device_runtime {
    struct wifi_probe_context *context;
    struct p4_hosted_owner *owner;
    struct wifi_owner_control *control;
    struct p4_hosted_fifo *fifo;
    const struct p4_hosted_tx_ops *tx;
    unsigned checksum;
    BOOL servicing;
};
static int wifi_owner_acquire(void *, uint32_t);
static int wifi_owner_budget(void *, uint32_t);
static void wifi_owner_release(void *);

static BOOL wifi_device_send(struct P4WifiBase *base,
                             const UBYTE *bytes, ULONG size)
{
    struct wifi_device_runtime *runtime;
    struct p4_hosted_frame frame;
    size_t written = 0;
    int sent = 0;
    if (!base || !base->ready || !base->runtime ||
        base->owner_task != FindTask(NULL) || !bytes || size < 14 || size > 1518)
        return FALSE;
    runtime = base->runtime;
    if (!runtime->servicing || runtime->owner->poisoned || runtime->fifo->poisoned ||
        runtime->owner->pending_active)
        return FALSE;
    p4host_zero(&frame, sizeof(frame));
    frame.interface_type = P4_HOSTED_STA;
    frame.payload = bytes;
    frame.payload_size = size;
    if (!p4_hosted_encode(runtime->owner->wire,
            sizeof(runtime->owner->wire), runtime->checksum, &frame, &written))
        return FALSE;
    if (wifi_owner_acquire(runtime->context, P4_HOSTED_OWNER_MAX_ACQUIRE_US)) {
        if (wifi_owner_budget(runtime->context, P4_HOSTED_OWNER_MAX_SLICE_US)) {
            p4host_zero(&runtime->control->frame_tx,
                        sizeof(runtime->control->frame_tx));
            sent = p4_hosted_tx_send(runtime->fifo,
                runtime->context->generation, runtime->tx,
                runtime->owner->wire, sizeof(runtime->owner->wire), written,
                runtime->checksum, &runtime->control->frame_tx);
        }
        wifi_owner_release(runtime->context);
    }
    p4host_zero(runtime->owner->wire, sizeof(runtime->owner->wire));
    if (runtime->fifo->poisoned)
        p4_hosted_owner_cancel(runtime->owner, runtime->fifo,
                                runtime->context->generation);
    return sent ? TRUE : FALSE;
}
struct wifi_owner_storage {
    struct p4_hosted_owner owner;
    struct wifi_owner_control control;
};

static uint32_t wifi_owner_now(void *opaque)
{
    struct wifi_probe_context *context = opaque;
    return context->host->now_us();
}

static int wifi_owner_acquire(void *opaque, uint32_t wait_us)
{
    struct wifi_probe_context *context = opaque;
    return context->host->acquire(context->host, wait_us) ? 1 : 0;
}

static int wifi_owner_budget(void *opaque, uint32_t usec)
{
    struct wifi_probe_context *context = opaque;
    return wifi_tx_session(context, context->generation) &&
        context->host->set_budget(context->host, usec);
}

static void wifi_owner_release(void *opaque)
{
    struct wifi_probe_context *context = opaque;
    context->host->release(context->host); /* Also ends this slice's budget. */
}

static int wifi_owner_stopping(void *opaque)
{
    struct wifi_probe_context *context = opaque;
    return !context->owner_cleanup &&
        !!(SetSignal(0, 0) & SIGBREAKF_CTRL_C);
}

static int wifi_owner_event(void *opaque,
                            const struct p4_hosted_inbox_item *item)
{
    struct wifi_probe_context *context = opaque;
    /* Current opt-in owner consumes metadata only, never claims to forward
     * link events to SANA-II. No borrowed payload outlives this callback. */
    ++context->owner_events;
    context->owner_last_event = item->msg_id;
    return 1;
}

static int wifi_owner_frame(void *opaque, const uint8_t *bytes, size_t size)
{
    struct P4WifiBase *base = wifi_resource_base;
    int consumed = 1;
    (void)opaque; (void)bytes; (void)size;
    /* No attached client means an administratively unopened interface. Drop
     * unclaimed frames without fabricating a receive completion or link-up. */
    ObtainSemaphore(&base->client_lock);
    if (base->ready && base->client && base->callbacks)
        consumed = base->callbacks->frame(base->client, bytes, (ULONG)size);
    ReleaseSemaphore(&base->client_lock);
    return consumed;
}

static ULONG wifi_owner_remaining(struct wifi_probe_context *context,
                                  ULONG started, ULONG limit)
{
    ULONG elapsed = context->host->now_us() - started;
    return elapsed < limit ? limit - elapsed : 0;
}

static int wifi_owner_request(struct wifi_probe_context *context,
    struct p4_hosted_owner *owner, struct wifi_owner_control *control,
    struct p4_hosted_fifo *fifo, unsigned checksum, uint32_t id,
    ULONG remaining)
{
    struct p4_hosted_rpc rpc;
    size_t written = 0;
    int encoded, exchanged;
    if (!remaining || !p4_hosted_request_ids_reserve(&owner->ids,
            context->generation, id, &control->uid))
        return 0;
    if (id == 278u)
        encoded = p4_hosted_wifi_init_request(control->uid,
            &wifi_reference_profile, control->request,
            sizeof(control->request), &written);
    else if (id == 260u)
        encoded = p4_hosted_wifi_set_mode_request(control->uid, 1,
            control->request, sizeof(control->request), &written);
    else if (id == 257u)
        encoded = p4_hosted_mac_request(control->uid, control->request,
            sizeof(control->request), &written);
    else
        encoded = p4_hosted_wifi_simple_request(id, control->uid,
            control->request, sizeof(control->request), &written);
    if (!encoded) {
        p4_hosted_request_ids_finish(&owner->ids, context->generation,
                                     control->uid, id);
        return 0;
    }
    /* Conservatively mark mutating attempts even if their TX later refuses.
     * Ambiguous poisoned sessions are never resurrected for cleanup RPCs. */
    if (id == 278u) control->init_attempted = 1;
    if (id == 280u) control->start_attempted = 1;
    p4host_zero(&control->result, sizeof(control->result));
    exchanged = p4_hosted_owner_exchange(owner, fifo, &control->ops,
        context->generation, checksum, control->request, written,
        remaining, &control->result);
    p4host_zero(control->request, sizeof(control->request));
    if (!exchanged)
        return 0;
    rpc.msg_type = owner->reply.msg_type;
    rpc.msg_id = owner->reply.msg_id;
    rpc.uid = owner->reply.uid;
    rpc.payload = owner->reply.payload;
    rpc.payload_size = owner->reply.payload_size;
    if (id == 257u) {
        if (!p4_hosted_mac_response(&rpc, control->uid, &control->mac) ||
            (control->mac.status == 0 && !control->mac.valid_mac)) {
            p4_hosted_owner_cancel(owner, fifo, context->generation);
            return 0;
        }
        return control->mac.status == 0;
    }
    if (!p4_hosted_wifi_status_response(&rpc, id, control->uid,
                                       &control->status)) {
        p4_hosted_owner_cancel(owner, fifo, context->generation);
        return 0;
    }
    return control->status == 0;
}

/* Enter with the bootstrap acquisition held, and always return unowned.
 * Preserved OPEN has already populated owner.rx before config buffer reuse.
 * State/buffers/control were allocated before acquiring the host. */
static void wifi_owner_service(struct wifi_probe_context *context,
    struct p4_hosted_owner *owner, struct wifi_owner_control *control,
    struct p4_hosted_open_result *opened,
    const struct p4_hosted_open_ops *open_ops,
    const struct p4_hosted_tx_ops *tx_ops,
    p4_sdio_setup_ops_t *setup_ops, p4_sdio_setup_result_t *setup,
    p4_sdio_restore_result_t *restored)
{
    static const uint32_t startup_ids[] = {278u, 260u, 280u, 257u};
    struct P4SDMMCHost *host = context->host;
    unsigned checksum = !!(opened->init.capabilities & P4_HOSTED_CONFIG_CAP_CHECKSUM);
    unsigned index;
    ULONG cleanup_start, cleanup_remaining, cleanup_wait;
    int protocol_cleanup = 1;
    struct P4WifiBase *base = wifi_resource_base;
    struct wifi_device_runtime runtime;
    runtime.context = context;
    runtime.owner = owner;
    runtime.control = control;
    runtime.fifo = &opened->fifo_state;
    runtime.tx = tx_ops;
    runtime.checksum = checksum;
    runtime.servicing = FALSE;
    control->rx_ops.context = context;
    control->rx_ops.session_ok = wifi_tx_session;
    control->rx_ops.budget_ok = wifi_budget;
    control->rx_ops.read_word = wifi_word_read;
    control->rx_ops.write_word = wifi_word_write;
    control->rx_ops.read_fifo = wifi_fifo_read;
    control->ops.context = context;
    control->ops.acquire = wifi_owner_acquire;
    control->ops.set_budget = wifi_owner_budget;
    control->ops.release = wifi_owner_release;
    control->ops.now_us = wifi_owner_now;
    control->ops.wait = wifi_yield;
    control->ops.stopping = wifi_owner_stopping;
    control->ops.dispatch_event = wifi_owner_event;
    control->ops.dispatch_frame = wifi_owner_frame;
    control->ops.rx = &control->rx_ops;
    control->ops.tx = tx_ops;
    wifi_owner_release(context);
    if (p4_hosted_owner_ids_init(owner, context->generation,
                                context->owner_open_start)) {
        for (index = 0; index < sizeof(startup_ids) / sizeof(startup_ids[0]); ++index)
            if (!wifi_owner_request(context, owner, control, &opened->fifo_state,
                checksum, startup_ids[index], wifi_owner_remaining(context,
                    context->owner_boot_start, 8000000UL)))
                break;
        control->startup_ok = index == sizeof(startup_ids) / sizeof(startup_ids[0]);
        if (control->startup_ok) {
            ObtainSemaphore(&base->client_lock);
            CopyMem(control->mac.mac, base->mac, sizeof(base->mac));
            base->owner_task = FindTask(NULL);
            base->runtime = &runtime;
            base->ready = TRUE;
            if (base->client && base->callbacks)
                base->callbacks->ready(base->client, base->mac);
            ReleaseSemaphore(&base->client_lock);
            /* Identity/status only. No SSID, passwords or peer payload logs. */
            bug("[WiFi] owner ready CPU=%u MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
                P4_WIFI_OWNER_CPU, control->mac.mac[0], control->mac.mac[1],
                control->mac.mac[2], control->mac.mac[3], control->mac.mac[4],
                control->mac.mac[5]);
            while (!wifi_owner_stopping(context)) {
                ObtainSemaphore(&base->client_lock);
                if (base->client && base->callbacks) {
                    runtime.servicing = TRUE;
                    base->callbacks->service(base->client, base);
                    runtime.servicing = FALSE;
                }
                ReleaseSemaphore(&base->client_lock);
                if (!p4_hosted_owner_step(owner, &opened->fifo_state, &control->ops,
                    context->generation, checksum, P4_HOSTED_OWNER_MAX_SLICE_US,
                    &control->result))
                    break;
                wifi_yield(context); /* Always outside controller ownership. */
            }
        }
    }
    ObtainSemaphore(&base->client_lock);
    base->ready = FALSE;
    base->runtime = NULL;
    base->owner_task = NULL;
    if (base->client && base->callbacks)
        base->callbacks->stopped(base->client);
    ReleaseSemaphore(&base->client_lock);
    context->owner_cleanup = TRUE; /* SIGBREAK cannot cancel its own cleanup. */
    cleanup_start = wifi_owner_now(context);
    if (!owner->poisoned && !opened->fifo_state.poisoned) {
        if (control->start_attempted)
            protocol_cleanup = wifi_owner_request(context, owner, control,
                &opened->fifo_state, checksum, 281u,
                wifi_owner_remaining(context, cleanup_start, 2000000UL));
        if (control->init_attempted)
            protocol_cleanup = wifi_owner_request(context, owner, control,
                &opened->fifo_state, checksum, 279u,
                wifi_owner_remaining(context, cleanup_start, 2000000UL)) && protocol_cleanup;
    } else protocol_cleanup = 0;
    /* Even failed protocol cleanup gets one bounded CLOSE/restoration attempt
     * on the original healthy host generation, never a controller reset. */
    cleanup_remaining = wifi_owner_remaining(context, cleanup_start, 2000000UL);
    cleanup_wait = cleanup_remaining < P4_HOSTED_OWNER_MAX_ACQUIRE_US
        ? cleanup_remaining : P4_HOSTED_OWNER_MAX_ACQUIRE_US;
    if (cleanup_wait && host->acquire(host, cleanup_wait)) {
        ULONG remaining = wifi_owner_remaining(context, cleanup_start, 2000000UL);
        if (remaining && wifi_tx_session(context, context->generation) &&
            host->set_budget(host, remaining)) {
            opened->fifo_poisoned = opened->fifo_state.poisoned;
            int close_ok = p4_hosted_open_cleanup(open_ops, opened);
            int restore_ok = p4_sdio_restore_function1(setup_ops, setup, restored);
            control->cleanup_ok = protocol_cleanup && close_ok && restore_ok;
        }
        wifi_owner_release(context);
    }
    p4_hosted_owner_cancel(owner, &opened->fifo_state, context->generation);
    bug("[WiFi] owner stopped startup=%u cleanup=%u events=%u last=%u error=%u\n",
        control->startup_ok, control->cleanup_ok, context->owner_events,
        context->owner_last_event, control->result.error);
}
#endif

#if !P4_WIFI_HOSTED_OWNER
static BOOL wifi_device_send(struct P4WifiBase *base,
                             const UBYTE *bytes, ULONG size)
{
    (void)base; (void)bytes; (void)size;
    return FALSE;
}
#endif

static void wifi_identify(void)
{
    struct P4SDMMCHost *host = OpenResource(P4_SDMMC_HOST_NAME);
    struct wifi_probe_context context;
#if P4_WIFI_SDIO_RESET_QUERY
    p4_sdio_reset_query_result_t reset_query;
#else
    struct p4_sdio_probe_result result;
#endif
    struct p4_sdio_probe_ops ops;
#if P4_WIFI_SDIO_SETUP
    p4_sdio_setup_ops_t setup_ops;
    p4_sdio_setup_result_t setup;
    p4_sdio_restore_result_t restored;
    int setup_attempted = 0, setup_ok = 0, restore_ok = 0;
#endif
#if P4_WIFI_HOSTED_INIT
    struct p4_hosted_open_ops open_ops;
    struct p4_hosted_open_result opened;
    UBYTE *open_buffer = NULL;
    int open_attempted = 0, open_ok = 0, open_cleanup_ok = 0;
#endif
#if P4_WIFI_HOSTED_REPLY
    struct p4_hosted_tx_ops tx_ops;
    struct p4_hosted_tx_result sent;
    size_t config_size = 0;
    int reply_attempted = 0, reply_ok = 0;
#endif
#if P4_WIFI_HOSTED_OWNER
    struct wifi_owner_storage *owner_storage = NULL;
#endif
#if P4_WIFI_HOSTED_MAC
    struct p4_hosted_startup *wlan = NULL;
    int mac_attempted = 0, mac_ok = 0;
#endif
#if P4_WIFI_HOSTED_SCAN
    struct p4_hosted_scan_sequence *scan = NULL;
    int scan_attempted = 0, scan_ok = 0;
#endif
    struct P4HostCommandResult identify_last;
    ULONG actual = 0, peer_clock, generation;
    ULONG before[WIFI_SNAPSHOT_WORDS], after[WIFI_SNAPSHOT_WORDS];
    unsigned snapshot_index;
    int success = 0;
    p4host_zero(&context, sizeof(context));
    p4host_zero(&ops, sizeof(ops));
#if P4_WIFI_SDIO_SETUP
    p4host_zero(&setup_ops, sizeof(setup_ops));
    p4host_zero(&setup, sizeof(setup));
    p4host_zero(&restored, sizeof(restored));
#endif
#if P4_WIFI_HOSTED_INIT
    p4host_zero(&open_ops, sizeof(open_ops));
    p4host_zero(&opened, sizeof(opened));
#endif
#if P4_WIFI_HOSTED_REPLY
    p4host_zero(&tx_ops, sizeof(tx_ops));
    p4host_zero(&sent, sizeof(sent));
#endif
    if (!host || host->abi != P4_SDMMC_HOST_ABI || host->size != sizeof(*host) ||
        !host->acquire || !host->release || !host->set_budget ||
        !host->set_clock || !host->command || !host->now_us)
    {
        bug("[WiFi] compatible shared host absent\n");
        return;
    }
#if P4_WIFI_HOSTED_INIT
    /* Allocate outside shared-controller ownership; never put the receive
     * stream on the small process stack or point DMA at that stack. */
    open_buffer = AllocVec(4096, MEMF_PUBLIC | MEMF_CLEAR);
    if (!open_buffer)
    {
        bug("[WiFi] INIT buffer unavailable\n");
        return;
    }
#if P4_WIFI_HOSTED_OWNER
    owner_storage = AllocVec(sizeof(*owner_storage), MEMF_PUBLIC | MEMF_CLEAR);
    if (!owner_storage) {
        FreeVec(open_buffer);
        bug("[WiFi] owner metadata unavailable\n");
        return;
    }
#endif
#if P4_WIFI_HOSTED_MAC
    wlan = AllocVec(sizeof(*wlan), MEMF_PUBLIC | MEMF_CLEAR);
    if (!wlan)
    {
        FreeVec(open_buffer);
        bug("[WiFi] WLAN metadata unavailable\n");
        return;
    }
#if P4_WIFI_HOSTED_SCAN
    scan = AllocVec(sizeof(*scan), MEMF_PUBLIC | MEMF_CLEAR);
    if (!scan)
    {
        FreeVec(wlan);
        FreeVec(open_buffer);
        bug("[WiFi] scan metadata unavailable\n");
        return;
    }
#endif
#endif
#endif
    if (!host->acquire(host, P4_SDMMC_HOST_WAIT_US))
    {
#if P4_WIFI_HOSTED_INIT
        FreeVec(open_buffer);
#if P4_WIFI_HOSTED_MAC
        FreeVec(wlan);
#if P4_WIFI_HOSTED_SCAN
        FreeVec(scan);
#endif
#endif
#endif
        bug("[WiFi] shared host busy\n");
#if P4_WIFI_HOSTED_OWNER
        FreeVec(owner_storage);
#endif
        return;
    }
    peer_clock = host->slot_clock[0];
    generation = host->generation;
    /* No reset of a healthy peer, and no silent reset of a faulted host.
     * Storage owns initialization/recovery; radio joins only healthy state. */
    if (!host->initialization_started || !peer_clock || !host->host_div ||
        host->host_div > 16 || host->dma_faulted || host->dma_length ||
        !host->set_budget(host,
#if P4_WIFI_SDIO_SETUP
                          8000000UL /* Reserve a distinct2s cleanup phase. */
#else
                          P4_SDMMC_BUDGET_MAX_US
#endif
                          ))
    {
        host->release(host);
#if P4_WIFI_HOSTED_INIT
        FreeVec(open_buffer);
#if P4_WIFI_HOSTED_MAC
        FreeVec(wlan);
#if P4_WIFI_HOSTED_SCAN
        FreeVec(scan);
#endif
#endif
#endif
        bug("[WiFi] host not ready for bounded join\n");
#if P4_WIFI_HOSTED_OWNER
        FreeVec(owner_storage);
#endif
        return;
    }
    wifi_configure_pins();
    context.host = host;
    context.generation = generation;
    context.peer_clock = peer_clock;
#if P4_WIFI_HOSTED_OWNER
    context.owner_boot_start = host->budget_start;
#endif
    ops.context = &context;
    ops.command = wifi_command;
    ops.budget_ok = wifi_budget;
    ops.yield = wifi_yield;
    if (host->set_clock(host, 1, 400000, FALSE, &actual) && wifi_budget(&context))
    {
        wifi_snapshot(before);
#if P4_WIFI_SDIO_RESET_QUERY
        /* Quiesced radio only: one RES write then CMD0/CMD5 query. No
         * continuation to selection/setup, even after a successful query. */
        success = p4_sdio_reset_query(&ops, &reset_query);
#else
        success = p4_sdio_probe_bootstrap(&ops, &result);
#endif
        identify_last = context.last;
#if P4_WIFI_SDIO_SETUP
        if (success && wifi_budget(&context))
        {
            setup_attempted = 1;
            setup_ops.context = &context;
            setup_ops.read_register = wifi_register_read;
            setup_ops.write_register = wifi_register_write;
            setup_ops.budget_ok = wifi_budget;
            setup_ops.yield = wifi_yield;
            setup_ok = p4_sdio_setup_function1(&setup_ops, &setup);
#if P4_WIFI_HOSTED_INIT
            if (setup_ok && wifi_budget(&context))
            {
                open_attempted = 1;
                open_ops.context = &context;
                open_ops.read_byte = wifi_register_read;
                open_ops.write_byte = wifi_register_write;
                open_ops.read_word = wifi_word_read;
                open_ops.write_word = wifi_word_write;
                open_ops.read_fifo = wifi_fifo_read;
                open_ops.budget_ok = wifi_budget;
                open_ops.yield = wifi_yield;
#if P4_WIFI_HOSTED_OWNER
                /* Conservative first-fragment lower bound: OPEN and config
                 * transmission must not renew an already received suffix. */
                context.owner_open_start = wifi_owner_now(&context);
                open_ok = p4_hosted_open_receive_init_preserve(&open_ops, generation,
                                                     open_buffer, 4096, &opened);
                if (open_ok)
                    open_ok = p4_hosted_rx_take_open(&owner_storage->owner.rx,
                        &opened, generation, open_buffer, 4096,
                        &owner_storage->owner.rx_result);
#else
                open_ok = p4_hosted_open_receive_init(&open_ops, generation,
                                                     open_buffer, 4096, &opened);
#endif
#if P4_WIFI_HOSTED_REPLY
                if (open_ok && wifi_tx_session(&context, generation) &&
                    wifi_budget(&context))
                {
                    reply_attempted = 1;
                    /* Pinned C6 slave sdio_slave_api.c BUFFER_NUM=20.
                     * This explicit source-profile gate is not installed
                     * binary identity; INIT's software queues are unrelated.
                     * Fresh power startup remains a physical prerequisite. */
                    if (p4_hosted_config_encode(&opened.init, open_buffer,
                                                4096, &config_size) &&
                        p4_hosted_fifo_set_pool(&opened.fifo_state, generation, 20))
                    {
                        tx_ops.context = &context;
                        tx_ops.read_word = wifi_word_read;
                        tx_ops.write_fifo = wifi_fifo_write;
                        tx_ops.session_ok = wifi_tx_session;
                        tx_ops.budget_ok = wifi_budget;
                        tx_ops.yield = wifi_yield;
                        reply_ok = p4_hosted_tx_send(&opened.fifo_state,
                            generation, &tx_ops, open_buffer, 4096, config_size,
                            !!(opened.init.capabilities & P4_HOSTED_CONFIG_CAP_CHECKSUM),
                            &sent);
#if P4_WIFI_HOSTED_OWNER
                        if (reply_ok && wifi_tx_session(&context, generation) &&
                            wifi_budget(&context)) {
                            wifi_owner_service(&context, &owner_storage->owner,
                                &owner_storage->control, &opened, &open_ops,
                                &tx_ops, &setup_ops, &setup, &restored);
                            /* Service has closed/restored and released the bus;
                             * do not inspect host fields or run legacy cleanup. */
                            p4host_zero(open_buffer, 4096);
                            FreeVec(open_buffer);
                            p4host_zero(owner_storage, sizeof(*owner_storage));
                            FreeVec(owner_storage);
                            return;
                        }
#endif
#if P4_WIFI_HOSTED_MAC
                        if (reply_ok && wifi_tx_session(&context, generation) &&
                            wifi_budget(&context))
                        {
                            mac_attempted = 1;
                            /* One fresh epoch, UIDs1..4, same aggregate
                             * deadline/ledger; no retries or budget renewal. */
                            mac_ok = p4_hosted_startup_run(wlan,
                                &wifi_reference_profile, &opened.fifo_state,
                                generation, &open_ops, &tx_ops,
                                !!(opened.init.capabilities & P4_HOSTED_CONFIG_CAP_CHECKSUM),
                                open_buffer, 4096);
#if P4_WIFI_HOSTED_SCAN
                            if (mac_ok)
                            {
                                scan_attempted = 1;
                                /* Blocking reference scan uses UIDs7..9 under
                                 * the same8s aggregate operation budget. */
                                scan_ok = p4_hosted_scan_sequence_run(scan,
                                    wlan, &opened.fifo_state, generation,
                                    &open_ops, &tx_ops,
                                    !!(opened.init.capabilities & P4_HOSTED_CONFIG_CAP_CHECKSUM),
                                    open_buffer, 4096);
                            }
#endif
                        }
#endif
                    }
                    opened.fifo_poisoned = opened.fifo_state.poisoned;
                }
#endif
            }
#endif
            /* Do not renew a failed operation's budget and call it success.
             * Cleanup has its own explicit reserve under the same ownership;
             * generation changes refuse restoration rather than reset SD. */
            if (!setup.mutation_attempted)
                restore_ok = 1;
            else if (host->generation == context.generation &&
                     host->slot_clock[0] == peer_clock &&
                     host->set_budget(host, 0) &&
                     host->set_budget(host, 2000000UL))
            {
#if P4_WIFI_HOSTED_INIT
#if P4_WIFI_HOSTED_MAC
                /* Stop/Deinit precede CLOSE while FIFO remains available.
                 * This is the same2s cleanup reserve, never per-RPC budgets.
                 * Unknown/poisoned outcomes remain a failed gate. */
                if (wlan->entered)
                {
#if P4_WIFI_HOSTED_SCAN
                    if (scan->entered)
                        p4_hosted_scan_sequence_cleanup(scan, wlan,
                            &opened.fifo_state, generation, &open_ops, &tx_ops,
                            !!(opened.init.capabilities & P4_HOSTED_CONFIG_CAP_CHECKSUM),
                            open_buffer, 4096);
#endif
                    p4_hosted_startup_cleanup(wlan, &opened.fifo_state,
                        generation, &open_ops, &tx_ops,
                        !!(opened.init.capabilities & P4_HOSTED_CONFIG_CAP_CHECKSUM),
                        open_buffer, 4096);
                }
                opened.fifo_poisoned = opened.fifo_state.poisoned;
#endif
                if (open_attempted)
                    open_cleanup_ok = p4_hosted_open_cleanup(&open_ops, &opened);
#endif
                /* Try restoring Fn1 even if OPEN cleanup failed. Keep the
                 * same reserved budget; no silent reset or budget renewal. */
                restore_ok = p4_sdio_restore_function1(&setup_ops, &setup,
                                                       &restored);
            }
        }
#endif
        wifi_snapshot(after);
    }
    else
    {
        host->set_budget(host, 0);
        host->release(host);
#if P4_WIFI_HOSTED_INIT
        FreeVec(open_buffer);
#if P4_WIFI_HOSTED_MAC
        FreeVec(wlan);
#if P4_WIFI_HOSTED_SCAN
        FreeVec(scan);
#endif
#endif
#endif
        bug("[WiFi] slot1 clock failed\n");
#if P4_WIFI_HOSTED_OWNER
        FreeVec(owner_storage);
#endif
        return;
    }
    /* Snapshot before release; no MMIO or host field inspection afterward. */
    success = success && host->slot_clock[0] == peer_clock && wifi_budget(&context);
#if P4_WIFI_SDIO_SETUP
    success = success && setup_attempted && setup_ok && restore_ok;
#endif
#if P4_WIFI_HOSTED_INIT
    success = success && open_attempted && open_ok && open_cleanup_ok;
#endif
#if P4_WIFI_HOSTED_REPLY
    success = success && reply_attempted && reply_ok;
#endif
#if P4_WIFI_HOSTED_MAC
    success = success && mac_attempted && mac_ok && wlan->cleanup_success;
#endif
#if P4_WIFI_HOSTED_SCAN
    success = success && scan_attempted && scan_ok && scan->cleanup_success;
#endif
    host->set_budget(host, 0);
    host->release(host);
#if P4_WIFI_HOSTED_INIT
    /* Reporting below uses only copied metadata, never frame.payload. */
    FreeVec(open_buffer);
#endif
#if P4_WIFI_HOSTED_OWNER
    p4host_zero(owner_storage, sizeof(*owner_storage));
    FreeVec(owner_storage);
#endif
    /* Runtime console is deliberately nonblocking. Defer and pace these
     * snapshots after releasing ownership; never delay the storage peer
     * merely to print diagnostics. The captured command result is local. */
    Delay(50);
#if P4_WIFI_SDIO_RESET_QUERY
    bug("[WiFi] reset-query ok=%u stage=%u cmd=%u\n",
        success, reset_query.stage, reset_query.command_index);
    Delay(5);
    bug("[WiFi] reset attempted=%u ack=%u error=%u\n",
        reset_query.reset_attempted, reset_query.reset_acknowledged,
        reset_query.error);
    Delay(5);
    bug("[WiFi] reset-query status=%u raw=%08x id=%08x\n",
        identify_last.status, identify_last.raw, identify_last.idstatus);
    Delay(5);
    bug("[WiFi] reset-query clock=%u R4=%08x fn=%u\n",
        actual, reset_query.response, reset_query.functions);
    Delay(5);
    bug("[WiFi] rejected R5 valid=%u value=%08x\n",
        context.rejected_r5_valid, context.rejected_r5);
#else
    bug("[WiFi] slot1 probe ok=%u stage=%u cmd=%u\n",
         success, result.stage, result.command_index);
    Delay(5);
    bug("[WiFi] bootstrap reset attempted=%u ack=%u status=%d\n",
         result.reset_attempted, result.reset_acknowledged, result.reset_status);
    Delay(5);
    bug("[WiFi] error=%u status=%u\n", result.error, identify_last.status);
    Delay(5);
    bug("[WiFi] raw=%08x id=%08x\n", identify_last.raw, identify_last.idstatus);
    Delay(5);
    bug("[WiFi] response=%08x\n", identify_last.response[0]);
    Delay(5);
    bug("[WiFi] clock=%u OCR=%08x RCA=%04x fn=%u\n",
         actual, result.ocr, result.rca, result.functions);
    Delay(5);
    bug("[WiFi] CCCR=%02x SDIO=%02x bus=%02x cap=%02x\n",
         result.cccr_revision, result.sdio_revision,
         result.bus_interface, result.capabilities);
    Delay(5);
    bug("[WiFi] enable=%02x ready=%02x CIS=%06x FBR1=%02x\n",
         result.io_enable, result.io_ready, result.cis_pointer,
         result.fbr1_interface);
#endif
#if P4_WIFI_SDIO_SETUP
    Delay(5);
    bug("[WiFi] setup attempted=%u ok=%u stage=%u error=%u\n",
        setup_attempted, setup_ok, setup.stage, setup.error);
    Delay(5);
    bug("[WiFi] setup ioe=%02x ready=%02x block=%u polls=%u\n",
        setup.observed_io_enable, setup.observed_io_ready,
        setup.observed_block_size, setup.poll_count);
    Delay(5);
    bug("[WiFi] restore ok=%u stage=%u error=%u mutated=%u\n",
        restore_ok, restored.stage, restored.error, setup.mutation_attempted);
#endif
#if P4_WIFI_HOSTED_INIT
    Delay(5);
    bug("[WiFi] INIT attempted=%u ok=%u stage=%u error=%u\n",
        open_attempted, open_ok, opened.stage, opened.error);
    Delay(5);
    bug("[WiFi] INIT polls=%u raw=%08x counter=%u consumed=%u\n",
        opened.polls, opened.raw_interrupt, opened.length_counter,
        opened.stream_consumed);
    Delay(5);
    bug("[WiFi] INIT preopen=%u ambiguous=%u poisoned=%u\n",
        opened.preopen_length_counter, opened.stream_ambiguous,
        opened.fifo_poisoned);
    Delay(5);
    bug("[WiFi] INIT chip=%02x caps=%02x bytes=%u\n",
        opened.init.chip_id, opened.init.capabilities, opened.logical_bytes);
    Delay(5);
    bug("[WiFi] INIT extra frames=%u bytes=%u discarded=%u\n",
        opened.additional_frame_count, opened.additional_frame_bytes,
        opened.additional_bytes_discarded);
    Delay(5);
    bug("[WiFi] INIT cleanup ok=%u error=%u flags=%08x\n",
        open_cleanup_ok, opened.cleanup_error, opened.cleanup_flags);
#endif
#if P4_WIFI_HOSTED_REPLY
    Delay(5);
    bug("[WiFi] reply attempted=%u ok=%u stage=%u error=%u polls=%u\n",
        reply_attempted, reply_ok, sent.stage, sent.error, sent.polls);
    Delay(5);
    bug("[WiFi] reply token=%08x logical=%u bus=%u write=%u\n",
        sent.token, sent.plan.logical_bytes, sent.plan.bus_bytes, sent.write_called);
    Delay(5);
    bug("[WiFi] reply started=%u complete=%u bytes=%u sent=%u poison=%u\n",
        sent.completion.started, sent.completion.success,
        sent.completion.completed_bytes, opened.fifo_state.tx_sent,
        opened.fifo_state.poisoned);
#endif
#if P4_WIFI_HOSTED_MAC
    /* Bits0..4: Init, Mode, Start, Stop, Deinit. Status values without their
     * valid bit are only initialized metadata, never remote acknowledgments. */
    Delay(5);
    bug("[WiFi] WLAN attempted=%02x valid=%02x written=%02x\n",
        wlan->init.attempted | (wlan->mode.attempted << 1) |
        (wlan->start.attempted << 2) | (wlan->stop.attempted << 3) |
        (wlan->deinit.attempted << 4),
        wlan->init.status_valid | (wlan->mode.status_valid << 1) |
        (wlan->start.status_valid << 2) | (wlan->stop.status_valid << 3) |
        (wlan->deinit.status_valid << 4),
        wlan->init.write_called | (wlan->mode.write_called << 1) |
        (wlan->start.write_called << 2) | (wlan->stop.write_called << 3) |
        (wlan->deinit.write_called << 4));
    Delay(5);
    bug("[WiFi] WLAN init=%u mode=%u start=%u stop=%u deinit=%u cleanup=%u\n",
        wlan->init.success, wlan->mode.success, wlan->start.success,
        wlan->stop.success, wlan->deinit.success, wlan->cleanup_success);
    Delay(5);
    bug("[WiFi] WLAN status init=%d mode=%d start=%d stop=%d deinit=%d\n",
        wlan->init.status, wlan->mode.status, wlan->start.status,
        wlan->stop.status, wlan->deinit.status);
    Delay(5);
    bug("[WiFi] WLAN error init=%u mode=%u start=%u stop=%u deinit=%u\n",
        wlan->init.error, wlan->mode.error, wlan->start.error,
        wlan->stop.error, wlan->deinit.error);
    Delay(5);
    bug("[WiFi] MAC attempted=%u ok=%u error=%u polls=%u\n",
        wlan->mac_attempted, mac_ok, wlan->mac.error, wlan->mac.polls);
    Delay(5);
    bug("[WiFi] MAC matched=%u status=%d events=%u unmatched=%u\n",
        wlan->mac.matched, wlan->mac.mac.status, wlan->mac.events,
        wlan->mac.unmatched);
    Delay(5);
    bug("[WiFi] MAC valid=%u %02x:%02x:%02x:%02x:%02x:%02x\n",
        wlan->mac.mac.valid_mac, wlan->mac.mac.mac[0], wlan->mac.mac.mac[1],
        wlan->mac.mac.mac[2], wlan->mac.mac.mac[3], wlan->mac.mac.mac[4],
        wlan->mac.mac.mac[5]);
#endif
#if P4_WIFI_HOSTED_SCAN
    Delay(5);
    bug("[WiFi] SCAN attempted=%u ok=%u aps=%u records=%u empty_workaround=%u cleanup=%u\n",
        scan_attempted, scan_ok, scan->ap_count.number,
        (unsigned)scan->aps.record_count, scan->skipped_empty_records,
        scan->cleanup_success);
    Delay(5);
    bug("[WiFi] SCAN valid start=%u count=%u records=%u stop=%u\n",
        scan->start.status_valid, scan->count.status_valid,
        scan->records.status_valid, scan->stop.status_valid);
    Delay(5);
    bug("[WiFi] SCAN status start=%d count=%d records=%d stop=%d\n",
        scan->start.status, scan->count.status,
        scan->records.status, scan->stop.status);
    Delay(5);
    bug("[WiFi] SCAN error start=%u count=%u records=%u stop=%u polls=%u\n",
        scan->start.error, scan->count.error, scan->records.error,
        scan->stop.error, scan->start.polls);
#endif
    for (snapshot_index = 0; snapshot_index < WIFI_SNAPSHOT_WORDS; ++snapshot_index)
    {
        Delay(5);
        bug("[WiFi] reg%u before=%08x after=%08x\n", snapshot_index,
             before[snapshot_index], after[snapshot_index]);
    }
    /* No inference of firmware/protocol compatibility from SDIO enumeration. */
#if P4_WIFI_HOSTED_MAC
    FreeVec(wlan);
#endif
#if P4_WIFI_HOSTED_SCAN
    FreeVec(scan);
#endif
}
#endif

AROS_UFH3(static ULONG, wifi_runner,
           AROS_UFPA(void *, dummy, A0),
           AROS_UFPA(LONG, argsize, D0),
           AROS_UFPA(struct ExecBase *, sysbase, A6))
{
    AROS_USERFUNC_INIT
    (void)dummy; (void)argsize; (void)sysbase;
#if P4_BOARD_WIFI_SDIO
#if P4_WIFI_HOSTED_OWNER
    if (!wifi_owner_pin())
        bug("[WiFi] owner CPU placement refused; no radio access\n");
    else
#endif
    wifi_identify();
#else
    bug("[WiFi] board has no supported SDIO radio\n");
#endif
    CloseLibrary((struct Library *)DOSBase);
    return 0;
    AROS_USERFUNC_EXIT
}

AROS_UFH3(static APTR, wifi_afterdos,
           AROS_UFPA(void *, dummy, D0),
           AROS_UFPA(BPTR, seglist, A0),
           AROS_UFPA(struct ExecBase *, sysbase, A6))
{
    AROS_USERFUNC_INIT
    (void)dummy; (void)seglist; (void)sysbase;
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 0);
    if (!DOSBase)
        return NULL;
    if (!CreateNewProcTags(NP_Entry, (IPTR)wifi_runner,
                           NP_Name, (IPTR)
#if P4_WIFI_HOSTED_OWNER
                           "P4 WiFi owner",
#else
                           "P4 SDIO identification",
#endif
                           NP_StackSize, 8192, NP_Priority, 0, TAG_DONE))
    {
        CloseLibrary((struct Library *)DOSBase);
        bug("[WiFi] identification process unavailable\n");
    }
    return NULL;
    AROS_USERFUNC_EXIT
}

const struct Resident wifiProbeResident =
{
    RTC_MATCHWORD, (struct Resident *)&wifiProbeResident,
    (APTR)((const char *)&wifiProbeResident + sizeof(struct Resident)),
    RTF_AFTERDOS, 1, NT_TASK, -126,
    "esp32p4 SDIO identification", "esp32p4 SDIO identification 1.0",
    &wifi_afterdos
};

static int P4Wifi_Init(LIBBASETYPEPTR P4WifiBase)
{
    SysBase = P4WifiBase->sysbase;
    wifi_resource_base = P4WifiBase;
    P4WifiBase->bridge_abi = P4_WIFI_DEVICE_BRIDGE_ABI;
    P4WifiBase->bridge_size = sizeof(*P4WifiBase);
    P4WifiBase->attach = wifi_device_attach;
    P4WifiBase->detach = wifi_device_detach;
    P4WifiBase->send_frame = wifi_device_send;
    InitSemaphore(&P4WifiBase->client_lock);
    P4WifiBase->client = NULL;
    P4WifiBase->callbacks = NULL;
    P4WifiBase->ready = FALSE;
    P4WifiBase->runtime = NULL;
    P4WifiBase->owner_task = NULL;
    return TRUE;
}
ADD2INITLIB(P4Wifi_Init, 0)
