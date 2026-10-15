/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "boardtool.h"
#include "../../../arch/riscv-esp32p4/network/runtime_probe.h"

static uint32_t native_submit(void *context,
    const struct p4_wifi_ota_request *request)
{
    struct P4WifiBase *radio = context;
    return radio->ota_submit(radio, request);
}
static enum p4_wifi_ota_job_state native_poll(void *context, uint32_t token,
    struct p4_wifi_ota_observation *reply)
{
    struct P4WifiBase *radio = context;
    return radio->ota_poll(radio, token, reply);
}
static int native_cancel(void *context, uint32_t token)
{
    struct P4WifiBase *radio = context;
    return radio->ota_cancel(radio, token);
}
static int native_discard(void *context, uint32_t token)
{
    struct P4WifiBase *radio = context;
    return radio->ota_discard(radio, token);
}

/* Private prepared-job adapter, NOT a frontend admission gate. No current
 * operation dispatch invokes it: source/target/recovery attestation and a
 * qualified transfer budget must precede enabling UPDATE-C6. */
int boardtool_ota_prepared(const struct p4_wifi_ota_request *request,
    struct p4_wifi_ota_observation *terminal, int *unconfirmed)
{
    static const struct board_ota_client_ops ops = {
        native_submit, native_poll, native_cancel, native_discard
    };
    struct board_ota_client client;
    struct P4WifiBase *radio;
    if (!request || !terminal || !unconfirmed ||
        (SetSignal(0, 0) & SIGBREAKF_CTRL_C)) return 0;
    radio = (struct P4WifiBase *)OpenResource(P4_WIFI_DEVICE_BRIDGE_NAME);
    if (!radio || radio->bridge_abi != P4_WIFI_DEVICE_BRIDGE_ABI ||
        radio->bridge_size < sizeof(*radio) || !radio->ota_submit ||
        !radio->ota_poll || !radio->ota_cancel || !radio->ota_discard)
        return 0;
    board_ota_client_init(&client);
    if (!board_ota_client_start(&client, &ops, radio, request)) return 0;
    /* Same submitting task survives Close/CTRL-C until DONE and discard.
     * Owner stall or persistent invalid metadata leaves this task pending;
     * no invented timeout, task kill, reset or OTA replay makes that safe. */
    do {
        board_ota_client_step(&client,
            (SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0);
        if (client.phase != BOARD_OTA_CLIENT_FINISHED) Delay(1);
    } while (client.phase != BOARD_OTA_CLIENT_FINISHED);
    *terminal = client.terminal;
    *unconfirmed = client.observation_unconfirmed;
    return 1; /* drained, NOT installed; caller must inspect terminal result */
}
