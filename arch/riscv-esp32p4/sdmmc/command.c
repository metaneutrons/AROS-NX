/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <proto/exec.h>
#include <string.h>
#include "command.h"
#include "dma.h"
#include "registers.h"

static ULONG command_read(ULONG offset)
{
    return *(volatile ULONG *)(IPTR)(P4_SDMMC_BASE + offset);
}

static void command_write(ULONG offset, ULONG value)
{
    if (offset == P4SD_RINTSTS)
        value &= P4SD_INT_CMD_DATA_MASK;
    *(volatile ULONG *)(IPTR)(P4_SDMMC_BASE + offset) = value;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

static BOOL command_wait(struct P4SDMMCHost *host, ULONG offset,
                          ULONG mask, ULONG limit)
{
    ULONG start = host->now_us(), polls = P4HOST_COMMAND_POLLS;
    while (command_read(offset) & mask)
        if (!--polls || (ULONG)(host->now_us() - start) >= limit)
            return FALSE;
    return TRUE;
}

static BOOL command_valid(struct P4SDMMCHost *host,
                           const struct P4HostCommand *request)
{
    ULONG flags;
    if (!host || !request || !host->now_us || !host->invalidate ||
        host->lock.ss_Owner != FindTask(NULL) ||
        request->slot > 1 || !request->generation ||
        request->generation != host->generation ||
        !host->slot_clock[request->slot] || host->dma_faulted ||
        host->dma_length || request->index > 63 ||
        (request->writing != FALSE && request->writing != TRUE) ||
        (request->early_cmd18 != FALSE && request->early_cmd18 != TRUE))
        return FALSE;
    flags = request->response_flags;
    if (flags & ~(ULONG)(MMC_RSP_PRESENT | MMC_RSP_136 | MMC_RSP_CRC |
                         MMC_RSP_BUSY | MMC_RSP_OPCODE))
        return FALSE;
    if (!(flags & MMC_RSP_PRESENT) && flags)
        return FALSE;
    if ((flags & MMC_RSP_136) && (flags & (MMC_RSP_BUSY | MMC_RSP_OPCODE)))
        return FALSE;
    if (request->response_error_mask &&
        (!(flags & MMC_RSP_PRESENT) || (flags & MMC_RSP_136)))
        return FALSE;
    if (request->diagnostic_fault != P4HOST_FAULT_NONE &&
        (request->diagnostic_fault < P4HOST_FAULT_CMD_CRC ||
         request->diagnostic_fault > P4HOST_FAULT_TIMEOUT ||
         request->slot != 0 || request->index != 18 || request->writing ||
         request->length < 1024 || request->length % 512 ||
         request->block_size != 512))
        return FALSE;
    if (!request->observe && request->observe_context)
        return FALSE;
    if (!request->length)
        return !request->data && !request->block_size && !request->writing &&
               !request->early_cmd18;
    if (!host->bounce || !host->fifo_reset || !host->dma_prepare ||
        !host->dma_ready || !host->dma_complete || !host->dma_reset ||
        !request->data || request->length > P4HOST_DMA_MAX ||
        (request->length >= 4 && request->length % 4) ||
        (IPTR)request->data > (IPTR)-1 - request->length ||
        !request->block_size || request->block_size > 2048 ||
        request->length % request->block_size)
        return FALSE;
    if (request->early_cmd18 &&
        (request->slot != 0 || request->index != 18 || request->writing ||
         request->length < 1024 || request->length % 512 ||
         request->block_size != 512))
        return FALSE;
    return TRUE;
}

BOOL p4host_command(struct P4SDMMCHost *host,
                    const struct P4HostCommand *request,
                    struct P4HostCommandResult *result)
{
    struct P4HostCommandResult outcome;
    ULONG command, start, limit, polls, response[4] = {0, 0, 0, 0};
    BOOL prepared = FALSE, cmd_done = FALSE, data_over = FALSE;
    if (!result)
        return FALSE;
    memset(&outcome, 0, sizeof(outcome));
    outcome.status = P4HOST_COMMAND_REFUSED;
    if (!command_valid(host, request))
        goto finished;
    /* Refuse an inhibited command before changing FIFO/DMA. A stuck command
     * start cannot be recovered by resetting DMA; invalidate both clients. */
    if (!command_wait(host, P4SD_CMD, P4SD_CMD_START,
                       P4HOST_COMMAND_TIMEOUT_US))
    {
        host->dma_faulted = TRUE;
        host->invalidate(host);
        outcome.status = P4HOST_COMMAND_TIMEOUT;
        goto finished;
    }
    if (request->index != 12 &&
        !command_wait(host, P4SD_STATUS, P4SD_STATUS_DATA_BUSY,
                       P4HOST_BUSY_TIMEOUT_US))
    {
        outcome.status = P4HOST_COMMAND_TIMEOUT;
        goto finished;
    }
    command = request->index | ((ULONG)request->slot << P4SD_CMD_SLOT_S);
    if (request->response_flags & MMC_RSP_PRESENT)
        command |= P4SD_CMD_RESP;
    if (request->response_flags & MMC_RSP_136)
        command |= P4SD_CMD_RESP_LONG;
    if (request->response_flags & MMC_RSP_CRC)
        command |= P4SD_CMD_RESP_CRC;
    if (!request->index)
        command |= P4SD_CMD_SEND_INIT;
    else if (request->index == 12)
        command |= P4SD_CMD_STOP_ABORT;
    else
        command |= P4SD_CMD_WAIT_DATA;
    if (request->length)
    {
        if (!host->fifo_reset(host))
        {
            host->dma_faulted = TRUE;
            host->invalidate(host);
            outcome.status = P4HOST_COMMAND_DMA_ERROR;
            goto finished;
        }
        if (request->writing)
        {
            memmove(host->bounce, request->data, request->length);
            command |= P4SD_CMD_WRITE;
        }
        command_write(P4SD_BLKSIZ, request->block_size);
        command_write(P4SD_BYTCNT, request->length);
        if (!host->dma_prepare(host, host->bounce, request->length,
                                request->writing))
        {
            outcome.status = P4HOST_COMMAND_DMA_ERROR;
            goto finished;
        }
        prepared = TRUE;
        command |= P4SD_CMD_DATA;
    }
    command_write(P4SD_RINTSTS, P4SD_INT_CMD_DATA_MASK);
    command_write(P4SD_CMDARG, request->argument);
    command_write(P4SD_CMD, command | P4SD_CMD_USE_HOLD | P4SD_CMD_START);
    outcome.started = TRUE;
    if (request->observe)
        request->observe(request->observe_context, request,
                         P4HOST_OBSERVE_START, &outcome);
    start = host->now_us();
    limit = request->length ? P4HOST_DATA_TIMEOUT_US : P4HOST_COMMAND_TIMEOUT_US;
    polls = P4HOST_COMMAND_POLLS;
    do
    {
        outcome.raw = command_read(P4SD_RINTSTS);
        outcome.idstatus = prepared ? command_read(P4SD_IDSTS) : 0;
        if (request->observe)
            request->observe(request->observe_context, request,
                             P4HOST_OBSERVE_SAMPLE, &outcome);
        /* Inject before completion/error decisions, including CMD_DONE and
         * completed descriptors in the same sample. Data CRC waits until a
         * command response is observed; it cannot be lost to early success. */
        if (request->diagnostic_fault != P4HOST_FAULT_NONE &&
            (request->diagnostic_fault != P4HOST_FAULT_DATA_CRC ||
             cmd_done || (outcome.raw & P4SD_INT_CMD_DONE)))
        {
            outcome.injected_fault = request->diagnostic_fault;
            if (request->diagnostic_fault == P4HOST_FAULT_CMD_CRC)
                outcome.raw |= P4SD_INT_RCRC;
            else if (request->diagnostic_fault == P4HOST_FAULT_DATA_CRC)
                outcome.raw |= P4SD_INT_DCRC;
            else
            {
                outcome.status = P4HOST_COMMAND_TIMEOUT;
                goto failed;
            }
        }
        /* Error precedence is unconditional, including CMD_DONE/DTO and
         * completed descriptors in the very same sample. No RX copy yet. */
        if (outcome.raw & (P4SD_INT_CMD_ERRORS |
                          (prepared ? P4SD_INT_DATA_ERRORS : 0)))
        {
            outcome.status = P4HOST_COMMAND_ERROR;
            goto failed;
        }
        if (outcome.idstatus & P4SD_IDSTS_ERRORS)
        {
            outcome.status = P4HOST_COMMAND_DMA_ERROR;
            goto failed;
        }
        if (!cmd_done && (outcome.raw & P4SD_INT_CMD_DONE))
        {
            cmd_done = TRUE;
            if (request->response_flags & MMC_RSP_PRESENT)
            {
                response[0] = command_read(P4SD_RESP0);
                if (request->response_flags & MMC_RSP_136)
                {
                    response[3] = response[0];
                    response[2] = command_read(P4SD_RESP1);
                    response[1] = command_read(P4SD_RESP2);
                    response[0] = command_read(P4SD_RESP3);
                }
            }
            if (response[0] & request->response_error_mask)
            {
                outcome.status = P4HOST_COMMAND_CARD_ERROR;
                goto failed;
            }
        }
        if (outcome.raw & P4SD_INT_DATA_OVER)
            data_over = TRUE;
        if (cmd_done && (!prepared ||
            ((data_over || request->early_cmd18) &&
             host->dma_ready(host, request->length))))
            break;
        if (!--polls || (ULONG)(host->now_us() - start) >= limit ||
            (!cmd_done && (ULONG)(host->now_us() - start) >=
                          P4HOST_COMMAND_TIMEOUT_US))
        {
            outcome.status = P4HOST_COMMAND_TIMEOUT;
            goto failed;
        }
    } while (TRUE);
    if ((request->response_flags & MMC_RSP_BUSY) &&
        !command_wait(host, P4SD_STATUS, P4SD_STATUS_DATA_BUSY,
                       P4HOST_BUSY_TIMEOUT_US))
    {
        outcome.status = P4HOST_COMMAND_TIMEOUT;
        goto failed;
    }
    /* Resample errors after potentially slow cache/descriptor and busy
     * checks, before completing/copying either response or receive payload. */
    outcome.raw = command_read(P4SD_RINTSTS);
    outcome.idstatus = prepared ? command_read(P4SD_IDSTS) : 0;
    if (outcome.raw & (P4SD_INT_CMD_ERRORS |
                      (prepared ? P4SD_INT_DATA_ERRORS : 0)))
    {
        outcome.status = P4HOST_COMMAND_ERROR;
        goto failed;
    }
    if (outcome.idstatus & P4SD_IDSTS_ERRORS)
    {
        outcome.status = P4HOST_COMMAND_DMA_ERROR;
        goto failed;
    }
    if (prepared && !host->dma_complete(host, host->bounce,
                                       request->length))
    {
        outcome.status = P4HOST_COMMAND_DMA_ERROR;
        goto failed;
    }
    /* FIFO reset/settlement in DMA completion can take time. Keep payload
     * staged until a final post-completion status sample also has no error. */
    outcome.raw = command_read(P4SD_RINTSTS);
    outcome.idstatus = prepared ? command_read(P4SD_IDSTS) : 0;
    if (outcome.raw & (P4SD_INT_CMD_ERRORS |
                      (prepared ? P4SD_INT_DATA_ERRORS : 0)))
    {
        outcome.status = P4HOST_COMMAND_ERROR;
        goto failed;
    }
    if (outcome.idstatus & P4SD_IDSTS_ERRORS)
    {
        outcome.status = P4HOST_COMMAND_DMA_ERROR;
        goto failed;
    }
    if (prepared && !request->writing && request->data != host->bounce)
        memmove(request->data, host->bounce, request->length);
    memcpy(outcome.response, response, sizeof(response));
    outcome.status = P4HOST_COMMAND_OK;
    command_write(P4SD_RINTSTS, P4SD_INT_CMD_DATA_MASK);
    goto finished;
failed:
    if (prepared && (!host->dma_reset(host) || !host->fifo_reset(host)))
    {
        host->dma_faulted = TRUE;
        host->invalidate(host);
    }
finished:
    if (outcome.started && request->observe)
        request->observe(request->observe_context, request,
                         P4HOST_OBSERVE_FINISH, &outcome);
    *result = outcome;
    return outcome.status == P4HOST_COMMAND_OK;
}
