/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio_reset.h"

#define REGISTER_BYTES 0x2000u
#define REGISTER_WORDS (REGISTER_BYTES / sizeof(uint32_t))
#define MAX_EVENTS 16u

static unsigned int checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned int)__LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

struct io_event
{
    char type;
    uint32_t address;
    uint32_t value;
};

struct fake_gpio
{
    uint32_t registers[REGISTER_WORDS];
    struct io_event events[MAX_EVENTS];
    unsigned int event_count;
    unsigned int output_writes;
    unsigned int reads;
    unsigned int waits;
    unsigned int pin;
    unsigned int active_level;
    int ignore_enable_set;
    int fail_assertion_read;
    int fail_final_output_read;
    int fail_final_input_read;
    int fail_mux_readback;
    int fail_route_readback;
    int mux_written;
    int route_written;
    uint32_t bit;
    uint32_t output;
    uint32_t input;
    uint32_t enable;
    uint32_t mux;
    uint32_t drive;
    uint32_t route;
    uint32_t old_output;
    uint32_t old_input;
    uint32_t old_enable;
    uint32_t old_mux;
    uint32_t old_drive;
    uint32_t old_route;
};

static uint32_t *fake_register(struct fake_gpio *fake, uint32_t address)
{
    CHECK((address & 3u) == 0u);
    CHECK(address < REGISTER_BYTES);
    return &fake->registers[address / sizeof(uint32_t)];
}

static int is_allowed_read(const struct fake_gpio *fake, uint32_t address)
{
    return address == fake->output || address == fake->input ||
           address == fake->enable || address == fake->mux ||
           address == fake->drive || address == fake->route;
}

static void record_event(struct fake_gpio *fake, char type,
                         uint32_t address, uint32_t value)
{
    CHECK(fake->event_count < MAX_EVENTS);
    fake->events[fake->event_count].type = type;
    fake->events[fake->event_count].address = address;
    fake->events[fake->event_count].value = value;
    ++fake->event_count;
}

static uint32_t fake_read(void *context, uint32_t address)
{
    struct fake_gpio *fake = (struct fake_gpio *)context;
    uint32_t value;

    ++fake->reads;
    CHECK(is_allowed_read(fake, address));
    value = *fake_register(fake, address);

    if (address == fake->mux && fake->mux_written &&
        fake->fail_mux_readback)
        value ^= UINT32_C(0x1000);
    if (address == fake->route && fake->route_written &&
        fake->fail_route_readback)
        value ^= UINT32_C(1);
    if (address == fake->input && fake->output_writes == 2u &&
        fake->fail_assertion_read)
    {
        if (fake->active_level)
            value &= ~fake->bit;
        else
            value |= fake->bit;
    }
    if (address == fake->output && fake->output_writes >= 4u &&
        fake->fail_final_output_read)
        value ^= fake->bit;
    if (address == fake->input && fake->output_writes >= 4u &&
        fake->fail_final_input_read)
        value ^= fake->bit;

    return value;
}

static void fake_write(void *context, uint32_t address, uint32_t value)
{
    struct fake_gpio *fake = (struct fake_gpio *)context;
    uint32_t *reg;

    record_event(fake, 'W', address, value);
    if (address == fake->output + 4u)
    {
        CHECK(value == fake->bit);
        ++fake->output_writes;
        *fake_register(fake, fake->output) |= value; /* W1TS */
        reg = fake_register(fake, fake->input);
        *reg = (*reg & ~fake->bit) |
               (*fake_register(fake, fake->output) & fake->bit);
    }
    else if (address == fake->output + 8u)
    {
        CHECK(value == fake->bit);
        ++fake->output_writes;
        *fake_register(fake, fake->output) &= ~value; /* W1TC */
        reg = fake_register(fake, fake->input);
        *reg = (*reg & ~fake->bit) |
               (*fake_register(fake, fake->output) & fake->bit);
    }
    else if (address == fake->enable + 4u)
    {
        CHECK(value == fake->bit);
        if (!fake->ignore_enable_set)
            *fake_register(fake, fake->enable) |= value; /* W1TS */
    }
    else if (address == fake->enable + 8u)
    {
        CHECK(value == fake->bit);
        *fake_register(fake, fake->enable) &= ~value; /* W1TC */
    }
    else if (address == fake->mux)
    {
        *fake_register(fake, address) = value;
        fake->mux_written = 1;
    }
    else if (address == fake->drive)
        *fake_register(fake, address) = value;
    else if (address == fake->route)
    {
        *fake_register(fake, address) = value;
        fake->route_written = 1;
    }
    else
        CHECK(0 && "unexpected GPIO/storage/IOMUX write");
}

static void fake_wait(void *context, uint32_t duration_us)
{
    struct fake_gpio *fake = (struct fake_gpio *)context;

    record_event(fake, 'D', 0u, duration_us);
    ++fake->waits;
}

static struct p4_radio_reset_ops fake_ops(struct fake_gpio *fake)
{
    struct p4_radio_reset_ops ops;

    ops.context = fake;
    ops.read = fake_read;
    ops.write = fake_write;
    ops.wait_us = fake_wait;
    return ops;
}

static void fake_init(struct fake_gpio *fake, unsigned int pin,
                      unsigned int active_level)
{
    memset(fake, 0, sizeof(*fake));
    fake->pin = pin;
    fake->active_level = active_level;
    fake->bit = UINT32_C(1) << (pin & 31u);
    fake->output = pin < 32u ? 0x04u : 0x10u;
    fake->input = pin < 32u ? 0x3cu : 0x40u;
    fake->enable = pin < 32u ? 0x20u : 0x2cu;
    fake->mux = 0x1004u + pin * 4u;
    fake->drive = 0x74u + pin * 4u;
    fake->route = 0x558u + pin * 4u;

    fake->old_output = UINT32_C(0xa55a966b);
    if (active_level)
        fake->old_output |= fake->bit;
    else
        fake->old_output &= ~fake->bit;
    fake->old_input = UINT32_C(0x5a5ac3d5) & ~fake->bit;
    fake->old_enable = UINT32_C(0x92345678) & ~fake->bit;
    fake->old_mux = UINT32_C(0xa5a57fff);
    fake->old_drive = UINT32_C(0x5aa500ff);
    fake->old_route = UINT32_C(0x9b7a5fff);

    *fake_register(fake, fake->output) = fake->old_output;
    *fake_register(fake, fake->input) = fake->old_input;
    *fake_register(fake, fake->enable) = fake->old_enable;
    *fake_register(fake, fake->mux) = fake->old_mux;
    *fake_register(fake, fake->drive) = fake->old_drive;
    *fake_register(fake, fake->route) = fake->old_route;
}

static void check_event(const struct fake_gpio *fake, unsigned int index,
                        char type, uint32_t address, uint32_t value)
{
    CHECK(index < fake->event_count);
    CHECK(fake->events[index].type == type);
    CHECK(fake->events[index].address == address);
    CHECK(fake->events[index].value == value);
}

static void check_reset_sequence(const struct fake_gpio *fake,
                                 unsigned int active_level,
                                 unsigned int expected_reads)
{
    uint32_t bit = UINT32_C(1) << 22;
    uint32_t expected_output = active_level ?
        (fake->old_output & ~bit) : (fake->old_output | bit);
    uint32_t expected_input = (fake->old_input & ~bit) |
                              (expected_output & bit);
    uint32_t expected_mux = (fake->old_mux & ~UINT32_C(0x7180)) |
                            UINT32_C(0x1200);
    uint32_t expected_drive = fake->old_drive & ~UINT32_C(4);
    uint32_t expected_route = (fake->old_route & ~UINT32_C(0xfff)) |
                              UINT32_C(0x500);
    uint32_t preload = active_level ? 0x18u : 0x14u;
    uint32_t assert_write = active_level ? 0x14u : 0x18u;
    uint32_t release_write = active_level ? 0x18u : 0x14u;

    CHECK(fake->pin == 54u);
    CHECK(fake->bit == bit);
    CHECK(fake->output == 0x10u);
    CHECK(fake->input == 0x40u);
    CHECK(fake->enable == 0x2cu);
    CHECK(fake->mux == 0x10dcu);
    CHECK(fake->drive == 0x14cu);
    CHECK(fake->route == 0x630u);

    CHECK(fake->event_count == 10u);
    check_event(fake, 0u, 'W', preload, bit);
    check_event(fake, 1u, 'W', 0x10dcu, expected_mux);
    check_event(fake, 2u, 'W', 0x14cu, expected_drive);
    check_event(fake, 3u, 'W', 0x630u, expected_route);
    check_event(fake, 4u, 'W', 0x30u, bit);
    check_event(fake, 5u, 'W', assert_write, bit);
    check_event(fake, 6u, 'D', 0u, 200000u);
    check_event(fake, 7u, 'W', release_write, bit);
    check_event(fake, 8u, 'D', 0u, 1000000u);
    check_event(fake, 9u, 'W', release_write, bit);
    CHECK(fake->reads == expected_reads);
    CHECK(fake->waits == 2u);

    CHECK(*fake_register((struct fake_gpio *)fake, 0x10u) ==
          expected_output);
    CHECK((*fake_register((struct fake_gpio *)fake, 0x10u) & ~bit) ==
          (fake->old_output & ~bit));
    CHECK(*fake_register((struct fake_gpio *)fake, 0x40u) ==
          expected_input);
    CHECK((*fake_register((struct fake_gpio *)fake, 0x40u) & ~bit) ==
          (fake->old_input & ~bit));
    CHECK(*fake_register((struct fake_gpio *)fake, 0x2cu) ==
          (fake->old_enable | bit));
    CHECK((*fake_register((struct fake_gpio *)fake, 0x2cu) & ~bit) ==
          (fake->old_enable & ~bit));

    CHECK(*fake_register((struct fake_gpio *)fake, 0x10dcu) ==
          expected_mux);
    CHECK((*fake_register((struct fake_gpio *)fake, 0x10dcu) &
           UINT32_C(0x7000)) == UINT32_C(0x1000));
    CHECK((*fake_register((struct fake_gpio *)fake, 0x10dcu) &
           UINT32_C(0x0180)) == 0u);
    CHECK((*fake_register((struct fake_gpio *)fake, 0x10dcu) &
           UINT32_C(0x0200)) == UINT32_C(0x0200));
    CHECK((*fake_register((struct fake_gpio *)fake, 0x10dcu) &
           ~UINT32_C(0x7180)) ==
          ((fake->old_mux & ~UINT32_C(0x7180)) |
           UINT32_C(0x0200)));
    CHECK(*fake_register((struct fake_gpio *)fake, 0x14cu) ==
          expected_drive);
    CHECK((*fake_register((struct fake_gpio *)fake, 0x14cu) &
           UINT32_C(4)) == 0u);
    CHECK(*fake_register((struct fake_gpio *)fake, 0x630u) ==
          expected_route);
    CHECK((*fake_register((struct fake_gpio *)fake, 0x630u) &
           UINT32_C(0xfff)) == UINT32_C(0x500));
}

static void test_absent_pin_has_no_effects(void)
{
    struct fake_gpio fake;
    struct p4_radio_reset_ops ops;

    fake_init(&fake, 54u, 0u);
    ops = fake_ops(&fake);
    CHECK(p4_radio_reset(&ops, -1, 0u) == 1);
    CHECK(fake.reads == 0u);
    CHECK(fake.event_count == 0u);
    CHECK(fake.waits == 0u);
    CHECK(p4_radio_reset(NULL, -1, 1u) == 1);
}

static void test_invalid_arguments_have_no_effects(void)
{
    struct fake_gpio fake;
    struct p4_radio_reset_ops ops;
    struct p4_radio_reset_ops invalid_ops;

    fake_init(&fake, 54u, 0u);
    ops = fake_ops(&fake);
    CHECK(p4_radio_reset(&ops, -2, 0u) == 0);
    CHECK(p4_radio_reset(&ops, 55, 0u) == 0);
    CHECK(p4_radio_reset(&ops, 54, 2u) == 0);
    CHECK(p4_radio_reset(NULL, 54, 0u) == 0);

    invalid_ops = ops;
    invalid_ops.read = NULL;
    CHECK(p4_radio_reset(&invalid_ops, 54, 0u) == 0);
    invalid_ops = ops;
    invalid_ops.write = NULL;
    CHECK(p4_radio_reset(&invalid_ops, 54, 0u) == 0);
    invalid_ops = ops;
    invalid_ops.wait_us = NULL;
    CHECK(p4_radio_reset(&invalid_ops, 54, 0u) == 0);

    CHECK(fake.reads == 0u);
    CHECK(fake.event_count == 0u);
    CHECK(fake.waits == 0u);
    CHECK(*fake_register(&fake, 0x10u) == fake.old_output);
    CHECK(*fake_register(&fake, 0x2cu) == fake.old_enable);
    CHECK(*fake_register(&fake, 0x10dcu) == fake.old_mux);
}

static void check_failed_configuration_cleanup(const struct fake_gpio *fake,
                                              unsigned int expected_reads)
{
    uint32_t bit = UINT32_C(1) << 22;
    uint32_t expected_mux = (fake->old_mux & ~UINT32_C(0x7180)) |
                            UINT32_C(0x1200);
    uint32_t expected_drive = fake->old_drive & ~UINT32_C(4);
    uint32_t expected_route = (fake->old_route & ~UINT32_C(0xfff)) |
                              UINT32_C(0x500);

    CHECK(fake->event_count == 6u);
    check_event(fake, 0u, 'W', 0x14u, bit);
    check_event(fake, 1u, 'W', 0x10dcu, expected_mux);
    check_event(fake, 2u, 'W', 0x14cu, expected_drive);
    check_event(fake, 3u, 'W', 0x630u, expected_route);
    check_event(fake, 4u, 'W', 0x30u, bit);
    check_event(fake, 5u, 'W', 0x34u, bit);
    CHECK(fake->reads == expected_reads);
    CHECK(fake->waits == 0u);
    CHECK(fake->output_writes == 1u);
    CHECK(fake->registers[0x10u / sizeof(uint32_t)] & bit);
    CHECK((fake->registers[0x10u / sizeof(uint32_t)] & ~bit) ==
          (fake->old_output & ~bit));
    CHECK((fake->registers[0x40u / sizeof(uint32_t)] & bit) == bit);
    CHECK((fake->registers[0x40u / sizeof(uint32_t)] & ~bit) ==
          (fake->old_input & ~bit));
    CHECK((fake->registers[0x2cu / sizeof(uint32_t)] & bit) == 0u);
    CHECK((fake->registers[0x2cu / sizeof(uint32_t)] & ~bit) ==
          (fake->old_enable & ~bit));
    CHECK(fake->registers[0x10dcu / sizeof(uint32_t)] == expected_mux);
    CHECK(fake->registers[0x14cu / sizeof(uint32_t)] == expected_drive);
    CHECK(fake->registers[0x630u / sizeof(uint32_t)] == expected_route);
}

static void test_enable_readback_failure_disables_output(void)
{
    struct fake_gpio fake;
    struct p4_radio_reset_ops ops;

    fake_init(&fake, 54u, 0u);
    fake.ignore_enable_set = 1;
    ops = fake_ops(&fake);
    CHECK(p4_radio_reset(&ops, 54, 0u) == 0);
    check_failed_configuration_cleanup(&fake, 4u);
}

static void test_mux_readback_failure_disables_output(void)
{
    struct fake_gpio fake;
    struct p4_radio_reset_ops ops;

    fake_init(&fake, 54u, 0u);
    fake.fail_mux_readback = 1;
    ops = fake_ops(&fake);
    CHECK(p4_radio_reset(&ops, 54, 0u) == 0);
    check_failed_configuration_cleanup(&fake, 5u);
}

static void test_route_readback_failure_disables_output(void)
{
    struct fake_gpio fake;
    struct p4_radio_reset_ops ops;

    fake_init(&fake, 54u, 0u);
    fake.fail_route_readback = 1;
    ops = fake_ops(&fake);
    CHECK(p4_radio_reset(&ops, 54, 0u) == 0);
    check_failed_configuration_cleanup(&fake, 6u);
}

static void test_gpio54_active_low(void)
{
    struct fake_gpio fake;
    struct p4_radio_reset_ops ops;

    fake_init(&fake, 54u, 0u);
    ops = fake_ops(&fake);
    CHECK(p4_radio_reset(&ops, 54, 0u) == 1);
    check_reset_sequence(&fake, 0u, 9u);
}

static void test_gpio54_active_high_alternate(void)
{
    struct fake_gpio fake;
    struct p4_radio_reset_ops ops;

    fake_init(&fake, 54u, 1u);
    ops = fake_ops(&fake);
    CHECK(p4_radio_reset(&ops, 54, 1u) == 1);
    check_reset_sequence(&fake, 1u, 9u);
}

static void test_failed_assertion_still_releases(void)
{
    struct fake_gpio fake;
    struct p4_radio_reset_ops ops;

    fake_init(&fake, 54u, 0u);
    fake.fail_assertion_read = 1;
    ops = fake_ops(&fake);
    CHECK(p4_radio_reset(&ops, 54, 0u) == 0);
    check_reset_sequence(&fake, 0u, 9u);
}

static void test_final_output_readback_failure(void)
{
    struct fake_gpio fake;
    struct p4_radio_reset_ops ops;

    fake_init(&fake, 54u, 0u);
    fake.fail_final_output_read = 1;
    ops = fake_ops(&fake);
    CHECK(p4_radio_reset(&ops, 54, 0u) == 0);
    check_reset_sequence(&fake, 0u, 8u);
}

static void test_final_input_readback_failure(void)
{
    struct fake_gpio fake;
    struct p4_radio_reset_ops ops;

    fake_init(&fake, 54u, 0u);
    fake.fail_final_input_read = 1;
    ops = fake_ops(&fake);
    CHECK(p4_radio_reset(&ops, 54, 0u) == 0);
    check_reset_sequence(&fake, 0u, 9u);
}

int main(void)
{
    test_absent_pin_has_no_effects();
    test_invalid_arguments_have_no_effects();
    test_enable_readback_failure_disables_output();
    test_mux_readback_failure_disables_output();
    test_route_readback_failure_disables_output();
    test_gpio54_active_low();
    test_gpio54_active_high_alternate();
    test_failed_assertion_still_releases();
    test_final_output_readback_failure();
    test_final_input_readback_failure();

    printf("radio reset: PASS (%u checks, 10 scenarios)\n", checks);
    return 0;
}
