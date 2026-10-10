/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include "sdio_probe.h"

static unsigned int checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "line %u: %s\n", (unsigned int)__LINE__, #expr); \
    exit(1); } } while (0)

static const unsigned int cccr_addresses[] = {
    0x00U, 0x01U, 0x02U, 0x03U, 0x07U, 0x08U, 0x09U, 0x0aU, 0x0bU,
    0x100U
};

struct fake_host
{
    unsigned int calls;
    unsigned int budget_calls;
    unsigned int yields;
    unsigned int polls;
    unsigned int register_reads;
    unsigned int fail_call;
    unsigned int fail_address;
    unsigned int budget_fail_at;
    unsigned int not_ready_polls;
    uint32_t initial_ocr;
    uint32_t poll_ocr;
    uint32_t r6;
    uint32_t r1;
    uint32_t r5_error_bits;
    uint8_t functions;
    uint8_t never_ready;
    uint8_t honor_error_masks;
    uint8_t fail_address_enabled;
    uint8_t registers[0x101];
};

static void fake_init(struct fake_host *fake)
{
    *fake = (struct fake_host){ 0 };
    fake->initial_ocr = P4_SDIO_PROBE_HOST_OCR;
    fake->poll_ocr = P4_SDIO_PROBE_HOST_OCR;
    fake->functions = 1;
    fake->r6 = UINT32_C(0x12340000);
    fake->registers[0x00] = 0x21;
    fake->registers[0x01] = 0x03;
    fake->registers[0x02] = 0x04;
    fake->registers[0x03] = 0x04;
    fake->registers[0x07] = 0x80;
    fake->registers[0x08] = 0xa5;
    fake->registers[0x09] = 0x34;
    fake->registers[0x0a] = 0x56;
    fake->registers[0x0b] = 0x12;
    fake->registers[0x100] = 0x42;
}

static int fake_command(void *context, unsigned int index, uint32_t argument,
    p4_sdio_response_type_t response_type, uint32_t error_mask,
    uint32_t *response)
{
    struct fake_host *fake = (struct fake_host *)context;
    unsigned int call = ++fake->calls;
    uint32_t raw = 0;

    CHECK(response != NULL);
    switch (index)
    {
        case 0:
            CHECK(call == 1U);
            CHECK(argument == 0);
            CHECK(response_type == P4_SDIO_RESP_NONE);
            CHECK(error_mask == 0);
            break;
        case 5:
            CHECK(response_type == P4_SDIO_RESP_R4);
            CHECK(error_mask == 0);
            if (argument == 0)
            {
                CHECK(call == 2U);
                CHECK(fake->yields == 1U);
                raw = ((uint32_t)fake->functions << 28) |
                    fake->initial_ocr;
            }
            else
            {
                CHECK(argument ==
                    (fake->initial_ocr & P4_SDIO_PROBE_HOST_OCR));
                ++fake->polls;
                raw = ((uint32_t)fake->functions << 28) | fake->poll_ocr;
                if (!fake->never_ready &&
                    fake->polls > fake->not_ready_polls)
                    raw |= UINT32_C(0x80000000);
            }
            break;
        case 3:
            CHECK(response_type == P4_SDIO_RESP_R6);
            CHECK(error_mask == P4_SDIO_PROBE_R6_ERROR_MASK);
            CHECK(argument == 0);
            CHECK(fake->polls > 0U && !fake->never_ready);
            raw = fake->r6;
            break;
        case 7:
            CHECK(response_type == P4_SDIO_RESP_R1);
            CHECK(error_mask == P4_SDIO_PROBE_R1_ERROR_MASK);
            CHECK(argument == (fake->r6 & UINT32_C(0xffff0000)));
            raw = fake->r1;
            break;
        case 52:
        {
            unsigned int address;

            CHECK(response_type == P4_SDIO_RESP_R5);
            CHECK(error_mask == P4_SDIO_PROBE_R5_ERROR_MASK);
            CHECK((argument & UINT32_C(0x80000000)) == 0);
            CHECK(((argument >> 28) & 7U) == 0U);
            CHECK((argument & UINT32_C(0x08000000)) == 0);
            CHECK((argument & UINT32_C(0x000000ff)) == 0);
            address = (argument >> 9) & 0x1ffffU;
            CHECK(fake->register_reads <
                sizeof(cccr_addresses) / sizeof(cccr_addresses[0]));
            CHECK(address == cccr_addresses[fake->register_reads]);
            CHECK(address <= 0x100U);
            raw = fake->registers[address] | fake->r5_error_bits;
            ++fake->register_reads;
            if (fake->fail_address_enabled && address == fake->fail_address)
                *response = raw;
            break;
        }
        default:
            CHECK(0 && "unexpected command index");
            break;
    }

    *response = raw;
    if (call == fake->fail_call)
        return 0;
    if (fake->honor_error_masks && (raw & error_mask) != 0)
        return 0;
    if (index == 52U && fake->fail_address_enabled &&
        ((argument >> 9) & 0x1ffffU) == fake->fail_address)
        return 0;
    return 1;
}

static int fake_budget_ok(void *context)
{
    struct fake_host *fake = (struct fake_host *)context;
    ++fake->budget_calls;
    return fake->budget_fail_at == 0U ||
        fake->budget_calls != fake->budget_fail_at;
}

static void fake_yield(void *context)
{
    struct fake_host *fake = (struct fake_host *)context;
    ++fake->yields;
}

static p4_sdio_probe_ops_t fake_ops(struct fake_host *fake)
{
    p4_sdio_probe_ops_t ops;
    ops.context = fake;
    ops.command = fake_command;
    ops.budget_ok = fake_budget_ok;
    ops.yield = fake_yield;
    return ops;
}

static void test_success(void)
{
    struct fake_host fake;
    p4_sdio_probe_ops_t ops;
    p4_sdio_probe_result_t result;

    fake_init(&fake);
    ops = fake_ops(&fake);
    CHECK(p4_sdio_probe_identify(&ops, &result) == 1);
    CHECK(result.complete == 1);
    CHECK(result.stage == P4_SDIO_PROBE_STAGE_COMPLETE);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_NONE);
    CHECK(result.command_index == 52U);
    CHECK(result.functions == 1U);
    CHECK(result.ocr == P4_SDIO_PROBE_HOST_OCR);
    CHECK(result.rca == 0x1234U);
    CHECK(result.cccr_revision == 1U);
    CHECK(result.sdio_revision == 2U);
    CHECK(result.io_enable == 0x04U);
    CHECK(result.io_ready == 0x04U);
    CHECK(result.bus_interface == 0x80U);
    CHECK(result.capabilities == 0xa5U);
    CHECK(result.cis_pointer == UINT32_C(0x125634));
    CHECK(result.fbr1_interface == 0x42U);
    CHECK(fake.calls == 15U);
    CHECK(fake.polls == 1U);
    CHECK(fake.register_reads == 10U);
    CHECK(fake.budget_calls == fake.calls + 1U);
    CHECK(fake.yields == 1U);
}

/* Regression fixture from the observed JC1060 C6 register snapshot.
 * The wider slave OCR must be intersected with the host's supported range;
 * IO_READY alone must not be mistaken for IO_ENABLE or WLAN readiness. */
static void test_observed_c6_snapshot(void)
{
    struct fake_host fake;
    p4_sdio_probe_ops_t ops;
    p4_sdio_probe_result_t result;

    fake_init(&fake);
    fake.initial_ocr = UINT32_C(0x00ffff00);
    fake.poll_ocr = UINT32_C(0x00ffff00);
    fake.functions = 2;
    fake.r6 = UINT32_C(0x00010000);
    fake.registers[0x00] = 0x32;
    fake.registers[0x02] = 0x00;
    fake.registers[0x03] = 0x02;
    fake.registers[0x07] = 0x40;
    fake.registers[0x08] = 0x17;
    fake.registers[0x09] = 0x00;
    fake.registers[0x0a] = 0x10;
    fake.registers[0x0b] = 0x00;
    fake.registers[0x100] = 0x00;
    ops = fake_ops(&fake);
    CHECK(p4_sdio_probe_identify(&ops, &result) == 1);
    CHECK(result.complete == 1 && result.error == P4_SDIO_PROBE_ERROR_NONE);
    CHECK(result.functions == 2U && result.ocr == UINT32_C(0x00ffff00));
    CHECK(result.rca == 1U);
    CHECK(result.cccr_revision == 2U && result.sdio_revision == 3U);
    CHECK(result.io_enable == 0U && result.io_ready == 2U);
    CHECK(result.bus_interface == 0x40U && result.capabilities == 0x17U);
    CHECK(result.cis_pointer == UINT32_C(0x001000));
    CHECK(result.fbr1_interface == 0U);
    CHECK(fake.calls == 15U && fake.polls == 1U);
    CHECK(fake.register_reads == 10U && fake.yields == 1U);
}

static void test_initial_failures(void)
{
    struct fake_host fake;
    p4_sdio_probe_ops_t ops;
    p4_sdio_probe_result_t result;

    fake_init(&fake);
    fake.fail_call = 1;
    ops = fake_ops(&fake);
    CHECK(p4_sdio_probe_identify(&ops, &result) == 0);
    CHECK(result.stage == P4_SDIO_PROBE_STAGE_CMD0);
    CHECK(result.command_index == 0U);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_COMMAND);
    CHECK(result.complete == 0U && result.functions == 0U);
    CHECK(fake.calls == 1U);

    fake_init(&fake);
    fake.functions = 0;
    ops = fake_ops(&fake);
    CHECK(p4_sdio_probe_identify(&ops, &result) == 0);
    CHECK(result.stage == P4_SDIO_PROBE_STAGE_CMD5_INITIAL);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_NO_FUNCTIONS);
    CHECK(fake.calls == 2U && fake.polls == 0U);

    fake_init(&fake);
    fake.initial_ocr = UINT32_C(0x00004000);
    fake.poll_ocr = fake.initial_ocr;
    ops = fake_ops(&fake);
    CHECK(p4_sdio_probe_identify(&ops, &result) == 0);
    CHECK(result.stage == P4_SDIO_PROBE_STAGE_CMD5_INITIAL);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_NO_VOLTAGE);
    CHECK(fake.calls == 2U && fake.polls == 0U);
}

static void test_polling_and_budget(void)
{
    struct fake_host fake;
    p4_sdio_probe_ops_t ops;
    p4_sdio_probe_result_t result;

    fake_init(&fake);
    fake.not_ready_polls = 3;
    ops = fake_ops(&fake);
    CHECK(p4_sdio_probe_identify(&ops, &result) == 1);
    CHECK(fake.polls == 4U);
    CHECK(fake.yields == 4U);

    fake_init(&fake);
    fake.never_ready = 1;
    ops = fake_ops(&fake);
    CHECK(p4_sdio_probe_identify(&ops, &result) == 0);
    CHECK(result.stage == P4_SDIO_PROBE_STAGE_CMD5_POLL);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_NOT_READY);
    CHECK(result.complete == 0U);
    CHECK(fake.polls == P4_SDIO_PROBE_MAX_POLLS);
    CHECK(fake.yields == P4_SDIO_PROBE_MAX_POLLS);
    CHECK(fake.calls == P4_SDIO_PROBE_MAX_POLLS + 2U);

    fake_init(&fake);
    fake.budget_fail_at = 3;
    ops = fake_ops(&fake);
    CHECK(p4_sdio_probe_identify(&ops, &result) == 0);
    CHECK(result.stage == P4_SDIO_PROBE_STAGE_CMD5_POLL);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_BUDGET);
    CHECK(fake.budget_calls == 3U);
    CHECK(fake.calls == 2U);

    /* Budget expires during the mandatory idle interval: no CMD5 follows. */
    fake_init(&fake);
    fake.budget_fail_at = 2;
    ops = fake_ops(&fake);
    CHECK(p4_sdio_probe_identify(&ops, &result) == 0);
    CHECK(result.stage == P4_SDIO_PROBE_STAGE_CMD5_INITIAL);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_BUDGET);
    CHECK(result.complete == 0U);
    CHECK(fake.calls == 1U && fake.yields == 1U);

    /* Failed CMD0 must not trigger a settling delay or a subsequent command. */
    fake_init(&fake);
    fake.fail_call = 1;
    ops = fake_ops(&fake);
    CHECK(p4_sdio_probe_identify(&ops, &result) == 0);
    CHECK(result.stage == P4_SDIO_PROBE_STAGE_CMD0);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_COMMAND);
    CHECK(fake.calls == 1U && fake.yields == 0U);

    fake_init(&fake);
    ops = fake_ops(&fake);
    ops.yield = NULL;
    CHECK(p4_sdio_probe_identify(&ops, &result) == 0);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_INVALID_ARGUMENT);
    CHECK(fake.calls == 0U && fake.yields == 0U);
}

static void test_relative_address_and_status(void)
{
    struct fake_host fake;
    p4_sdio_probe_ops_t ops;
    p4_sdio_probe_result_t result;

    fake_init(&fake);
    fake.r6 = 0;
    ops = fake_ops(&fake);
    CHECK(p4_sdio_probe_identify(&ops, &result) == 0);
    CHECK(result.stage == P4_SDIO_PROBE_STAGE_CMD3);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_INVALID_RCA);
    CHECK(fake.calls == 4U);

    fake_init(&fake);
    fake.r6 = UINT32_C(0x12348000);
    fake.honor_error_masks = 1;
    ops = fake_ops(&fake);
    CHECK(p4_sdio_probe_identify(&ops, &result) == 0);
    CHECK(result.stage == P4_SDIO_PROBE_STAGE_CMD3);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_R6_STATUS);
    CHECK(fake.calls == 4U);

    fake_init(&fake);
    fake.r1 = UINT32_C(0x00008000);
    fake.honor_error_masks = 1;
    ops = fake_ops(&fake);
    CHECK(p4_sdio_probe_identify(&ops, &result) == 0);
    CHECK(result.stage == P4_SDIO_PROBE_STAGE_CMD7);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_R1_STATUS);
    CHECK(result.rca == 0x1234U);
    CHECK(fake.calls == 5U && fake.register_reads == 0U);
}

static void test_cccr_and_final_read(void)
{
    struct fake_host fake;
    p4_sdio_probe_ops_t ops;
    p4_sdio_probe_result_t result;

    fake_init(&fake);
    fake.r5_error_bits = UINT32_C(0x00000100);
    fake.honor_error_masks = 1;
    ops = fake_ops(&fake);
    CHECK(p4_sdio_probe_identify(&ops, &result) == 0);
    CHECK(result.stage == P4_SDIO_PROBE_STAGE_CCCR);
    CHECK(result.command_index == 52U);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_R5_STATUS);
    CHECK(result.complete == 0U);
    CHECK(fake.register_reads == 1U);

    fake_init(&fake);
    fake.fail_address_enabled = 1;
    fake.fail_address = 0x100U;
    ops = fake_ops(&fake);
    CHECK(p4_sdio_probe_identify(&ops, &result) == 0);
    CHECK(result.stage == P4_SDIO_PROBE_STAGE_FBR1);
    CHECK(result.command_index == 52U);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_COMMAND);
    CHECK(result.complete == 0U);
    CHECK(result.cis_pointer == UINT32_C(0x125634));
    CHECK(result.fbr1_interface == 0U);
    CHECK(fake.register_reads == 10U);
}

static void test_result_zeroing_and_validation(void)
{
    p4_sdio_probe_result_t result;
    volatile unsigned char *bytes = (volatile unsigned char *)&result;
    unsigned int i;

    for (i = 0; i < sizeof(result); ++i)
        bytes[i] = 0xa5U;
    CHECK(p4_sdio_probe_identify(NULL, &result) == 0);
    CHECK(result.stage == P4_SDIO_PROBE_STAGE_START);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_INVALID_ARGUMENT);
    CHECK(result.command_index == 0U);
    CHECK(result.ocr == 0U && result.rca == 0U && result.functions == 0U);
    CHECK(result.cccr_revision == 0U && result.sdio_revision == 0U);
    CHECK(result.io_enable == 0U && result.io_ready == 0U);
    CHECK(result.bus_interface == 0U && result.capabilities == 0U);
    CHECK(result.cis_pointer == 0U && result.fbr1_interface == 0U);
    CHECK(result.complete == 0U);
    CHECK(p4_sdio_probe_identify(NULL, NULL) == 0);
}

int main(void)
{
    test_success();
    test_observed_c6_snapshot();
    test_initial_failures();
    test_polling_and_budget();
    test_relative_address_and_status();
    test_cccr_and_final_read();
    test_result_zeroing_and_validation();
    printf("PASS: %u SDIO probe checks\n", checks);
    return 0;
}
