/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_open.h"

#define P4_HOSTED_CCCR_INT_ENABLE UINT32_C(0x04)
#define P4_HOSTED_CCCR_BLOCK_LOW UINT32_C(0x10)
#define P4_HOSTED_CCCR_BLOCK_HIGH UINT32_C(0x11)
#define P4_HOSTED_OPEN_FUNCTION 1u
#define P4_HOSTED_PRIVATE_INTERFACE 5u
#define P4_HOSTED_PRIVATE_INIT_PACKET UINT8_C(0x33)
#define P4_HOSTED_CHECKSUM_CAPABILITY (UINT8_C(1) << 7)

static int ranges_overlap(const void *a, size_t a_size,
                          const void *b, size_t b_size)
{
    uintptr_t a_start, b_start, a_length, b_length;

    if (a_size == 0 || b_size == 0)
        return 0;
#if SIZE_MAX > UINTPTR_MAX
    if ((uintmax_t)a_size > (uintmax_t)UINTPTR_MAX ||
        (uintmax_t)b_size > (uintmax_t)UINTPTR_MAX)
        return 1;
#endif
    a_start = (uintptr_t)a;
    b_start = (uintptr_t)b;
    a_length = (uintptr_t)a_size;
    b_length = (uintptr_t)b_size;
    if (a_start <= b_start)
        return b_start - a_start < a_length;
    return a_start - b_start < b_length;
}

static int fail_gate(struct p4_hosted_open_result *result,
                     enum p4_hosted_open_stage stage,
                     enum p4_hosted_open_error error)
{
    if (result->error == P4_HOSTED_OPEN_ERROR_NONE) {
        result->stage = stage;
        result->error = error;
    }
    result->gate_ok = 0;
    return 0;
}

static int gate_budget(const struct p4_hosted_open_ops *ops,
                       struct p4_hosted_open_result *result,
                       enum p4_hosted_open_stage stage)
{
    if (ops->budget_ok(ops->context) == 1)
        return 1;
    return fail_gate(result, stage,
                     P4_HOSTED_OPEN_ERROR_BUDGET_OR_GENERATION);
}

/* Every operation is followed by a budget/generation check, even after a
 * failed callback, so elapsed time is reported distinctly from bus failure. */
static int gate_callback_result(const struct p4_hosted_open_ops *ops,
                                struct p4_hosted_open_result *result,
                                enum p4_hosted_open_stage stage,
                                int callback_ok)
{
    int budget_ok = ops->budget_ok(ops->context) == 1;
    if (!budget_ok)
        return fail_gate(result, stage,
                         P4_HOSTED_OPEN_ERROR_BUDGET_OR_GENERATION);
    if (callback_ok != 1)
        return fail_gate(result, stage, P4_HOSTED_OPEN_ERROR_CALLBACK);
    return 1;
}

static int gate_read_byte(const struct p4_hosted_open_ops *ops,
                          struct p4_hosted_open_result *result,
                          enum p4_hosted_open_stage stage,
                          unsigned function, uint32_t address,
                          uint8_t *value)
{
    int ok;
    if (!gate_budget(ops, result, stage))
        return 0;
    ok = ops->read_byte(ops->context, function, address, value);
    return gate_callback_result(ops, result, stage, ok);
}

static int gate_write_byte(const struct p4_hosted_open_ops *ops,
                           struct p4_hosted_open_result *result,
                           enum p4_hosted_open_stage stage,
                           unsigned function, uint32_t address,
                           uint8_t value)
{
    int ok;
    if (!gate_budget(ops, result, stage))
        return 0;
    ok = ops->write_byte(ops->context, function, address, value);
    return gate_callback_result(ops, result, stage, ok);
}

static int gate_read_word(const struct p4_hosted_open_ops *ops,
                          struct p4_hosted_open_result *result,
                          enum p4_hosted_open_stage stage,
                          uint32_t address, uint32_t *value)
{
    int ok;
    if (!gate_budget(ops, result, stage))
        return 0;
    ok = ops->read_word(ops->context, address, value);
    return gate_callback_result(ops, result, stage, ok);
}

static int gate_write_word(const struct p4_hosted_open_ops *ops,
                           struct p4_hosted_open_result *result,
                           enum p4_hosted_open_stage stage,
                           uint32_t address, uint32_t value)
{
    int ok;
    if (!gate_budget(ops, result, stage))
        return 0;
    ok = ops->write_word(ops->context, address, value);
    return gate_callback_result(ops, result, stage, ok);
}

static int valid_gate_ops(const struct p4_hosted_open_ops *ops)
{
    return ops && ops->read_byte && ops->write_byte && ops->read_word &&
        ops->write_word && ops->read_fifo && ops->budget_ok && ops->yield;
}

static int valid_snapshot(const struct p4_hosted_open_result *result)
{
    return (result->snapshot_flags & P4_HOSTED_OPEN_SAVED_ALL) ==
        P4_HOSTED_OPEN_SAVED_ALL;
}

static int decode_profile_frame(const uint8_t *bytes, size_t available,
                                unsigned checksum_enabled,
                                struct p4_hosted_frame *frame)
{
    struct p4_hosted_frame unchecked;
    if (!p4_hosted_decode(bytes, available, 0, &unchecked))
        return 0;
    if (!checksum_enabled) {
        *frame = unchecked;
        return 1;
    }
    return p4_hosted_decode(bytes, available, 1, frame);
}

int p4_hosted_open_receive_init(const struct p4_hosted_open_ops *ops,
                                uint32_t generation, uint8_t *buffer,
                                size_t capacity,
                                struct p4_hosted_open_result *result)
{
    uint8_t value;
    uint32_t word, clear_bits;
    unsigned poll;
    unsigned checksum_enabled;
    size_t stream_offset;
    size_t clear_index;
    int packet_pending = 0;
    volatile unsigned char *result_bytes;
    struct p4_hosted_frame decoded;
    struct p4_hosted_frame additional;

    if (!result)
        return 0;
    /* Aliasing would let result initialization or the FIFO callback corrupt
     * the operation table or output buffer. Refuse before touching any of the
     * caller-owned objects (including before dereferencing callbacks). */
    if ((buffer && ranges_overlap(buffer, capacity, result, sizeof *result)) ||
        (ops && ranges_overlap(ops, sizeof *ops, result, sizeof *result)) ||
        (ops && buffer && ranges_overlap(ops, sizeof *ops, buffer, capacity)))
        return 0;
    result_bytes = (volatile unsigned char *)(void *)result;
    for (clear_index = 0; clear_index < sizeof *result; ++clear_index)
        result_bytes[clear_index] = 0;
    result->initialized = 1;
    result->generation = generation;
    result->cleanup_error = P4_HOSTED_OPEN_CLEANUP_NOT_RUN;

    if (!valid_gate_ops(ops) || !buffer)
        return fail_gate(result, P4_HOSTED_OPEN_STAGE_ARGUMENTS,
                         P4_HOSTED_OPEN_ERROR_INVALID_ARGUMENT);
    if (capacity < 512u)
        return fail_gate(result, P4_HOSTED_OPEN_STAGE_BUFFER_CAPACITY,
                         P4_HOSTED_OPEN_ERROR_BUFFER_TOO_SMALL);

    result->stage = P4_HOSTED_OPEN_STAGE_SNAPSHOT_INTERRUPT_ENABLE;
    if (!gate_read_byte(ops, result, result->stage, 0,
                        P4_HOSTED_CCCR_INT_ENABLE,
                        &result->saved_interrupt_enable))
        return 0;
    result->snapshot_flags |= P4_HOSTED_OPEN_SAVED_INTERRUPT_ENABLE;

    result->stage = P4_HOSTED_OPEN_STAGE_SNAPSHOT_BLOCK_LOW;
    if (!gate_read_byte(ops, result, result->stage, 0,
                        P4_HOSTED_CCCR_BLOCK_LOW,
                        &result->saved_block_low))
        return 0;
    result->snapshot_flags |= P4_HOSTED_OPEN_SAVED_BLOCK_LOW;

    result->stage = P4_HOSTED_OPEN_STAGE_SNAPSHOT_BLOCK_HIGH;
    if (!gate_read_byte(ops, result, result->stage, 0,
                        P4_HOSTED_CCCR_BLOCK_HIGH,
                        &result->saved_block_high))
        return 0;
    result->snapshot_flags |= P4_HOSTED_OPEN_SAVED_BLOCK_HIGH;

    result->fn0_mutation_attempted = 1;
    result->stage = P4_HOSTED_OPEN_STAGE_WRITE_INTERRUPT_ENABLE;
    if (!gate_write_byte(ops, result, result->stage, 0,
                         P4_HOSTED_CCCR_INT_ENABLE,
                         (uint8_t)(result->saved_interrupt_enable |
                                   P4_HOSTED_OPEN_INTERRUPT_ENABLE)))
        return 0;

    result->stage = P4_HOSTED_OPEN_STAGE_VERIFY_INTERRUPT_ENABLE;
    if (!gate_read_byte(ops, result, result->stage, 0,
                        P4_HOSTED_CCCR_INT_ENABLE, &value))
        return 0;
    if (value != (uint8_t)(result->saved_interrupt_enable |
                           P4_HOSTED_OPEN_INTERRUPT_ENABLE))
        return fail_gate(result, result->stage,
                         P4_HOSTED_OPEN_ERROR_REGISTER_VERIFY);

    result->stage = P4_HOSTED_OPEN_STAGE_WRITE_BLOCK_LOW;
    if (!gate_write_byte(ops, result, result->stage, 0,
                         P4_HOSTED_CCCR_BLOCK_LOW,
                         (uint8_t)P4_HOSTED_OPEN_BLOCK_SIZE))
        return 0;

    result->stage = P4_HOSTED_OPEN_STAGE_WRITE_BLOCK_HIGH;
    if (!gate_write_byte(ops, result, result->stage, 0,
                         P4_HOSTED_CCCR_BLOCK_HIGH,
                         (uint8_t)(P4_HOSTED_OPEN_BLOCK_SIZE >> 8)))
        return 0;

    result->stage = P4_HOSTED_OPEN_STAGE_VERIFY_BLOCK_LOW;
    if (!gate_read_byte(ops, result, result->stage, 0,
                        P4_HOSTED_CCCR_BLOCK_LOW, &value))
        return 0;
    if (value != (uint8_t)P4_HOSTED_OPEN_BLOCK_SIZE)
        return fail_gate(result, result->stage,
                         P4_HOSTED_OPEN_ERROR_REGISTER_VERIFY);

    result->stage = P4_HOSTED_OPEN_STAGE_VERIFY_BLOCK_HIGH;
    if (!gate_read_byte(ops, result, result->stage, 0,
                        P4_HOSTED_CCCR_BLOCK_HIGH, &value))
        return 0;
    if (value != (uint8_t)(P4_HOSTED_OPEN_BLOCK_SIZE >> 8))
        return fail_gate(result, result->stage,
                         P4_HOSTED_OPEN_ERROR_REGISTER_VERIFY);

    result->stage = P4_HOSTED_OPEN_STAGE_READ_PREOPEN_LENGTH;
    if (!gate_read_word(ops, result, result->stage,
                        P4_HOSTED_OPEN_PACKET_LENGTH, &word))
        return 0;
    result->preopen_length_counter = word & P4_HOSTED_OPEN_RX_COUNTER_MASK;
    if (result->preopen_length_counter != 0)
        return fail_gate(result,
                         P4_HOSTED_OPEN_STAGE_VALIDATE_PREOPEN_LENGTH,
                         P4_HOSTED_OPEN_ERROR_STALE_COUNTER);

    if (!p4_hosted_fifo_start(&result->fifo_state, generation, 0))
        return fail_gate(result, P4_HOSTED_OPEN_STAGE_PLAN_FIFO,
                         P4_HOSTED_OPEN_ERROR_FIFO_COUNTER);

    result->stage = P4_HOSTED_OPEN_STAGE_WRITE_OPEN;
    result->open_attempted = 1;
    if (!gate_write_byte(ops, result, result->stage, P4_HOSTED_OPEN_FUNCTION,
                         P4_HOSTED_OPEN_CONTROL_ADDRESS,
                         P4_HOSTED_OPEN_VALUE))
        return 0;

    for (poll = 0; poll < P4_HOSTED_OPEN_MAX_POLLS; ++poll) {
        result->stage = P4_HOSTED_OPEN_STAGE_POLL_BUDGET;
        if (!gate_budget(ops, result, result->stage))
            return 0;

        result->stage = P4_HOSTED_OPEN_STAGE_READ_INTERRUPT;
        ++result->polls;
        if (!gate_read_word(ops, result, result->stage,
                            P4_HOSTED_OPEN_INT_RAW,
                            &result->raw_interrupt))
            return 0;

        if (result->raw_interrupt & P4_HOSTED_OPEN_INT_RX_UNDERFLOW)
            return fail_gate(result, result->stage,
                             P4_HOSTED_OPEN_ERROR_RX_UNDERFLOW);
        if (result->raw_interrupt & P4_HOSTED_OPEN_INT_TX_OVERFLOW)
            return fail_gate(result, result->stage,
                             P4_HOSTED_OPEN_ERROR_TX_OVERFLOW);

        clear_bits = result->raw_interrupt & P4_HOSTED_OPEN_INT_CLEAR_MASK;
        if (clear_bits &&
            !gate_write_word(ops, result, P4_HOSTED_OPEN_STAGE_CLEAR_INTERRUPT,
                             P4_HOSTED_OPEN_INT_CLEAR, clear_bits))
            return 0;

        if (result->raw_interrupt & P4_HOSTED_OPEN_INT_PACKET) {
            packet_pending = 1;
            break;
        }

        ops->yield(ops->context);
        result->stage = P4_HOSTED_OPEN_STAGE_POLL_BUDGET;
        if (!gate_budget(ops, result, result->stage))
            return 0;
    }
    if (!packet_pending)
        return fail_gate(result, P4_HOSTED_OPEN_STAGE_POLL_LIMIT,
                         P4_HOSTED_OPEN_ERROR_POLL_EXHAUSTED);

    result->stage = P4_HOSTED_OPEN_STAGE_READ_LENGTH;
    if (!gate_read_word(ops, result, result->stage,
                        P4_HOSTED_OPEN_PACKET_LENGTH, &word))
        return 0;
    result->length_counter = word & P4_HOSTED_OPEN_RX_COUNTER_MASK;

    result->stage = P4_HOSTED_OPEN_STAGE_PLAN_FIFO;
    if (!p4_hosted_fifo_rx(&result->fifo_state, generation,
                           result->length_counter, &result->fifo_plan)) {
        result->fifo_poisoned = result->fifo_state.poisoned;
        return fail_gate(result, result->stage,
                         P4_HOSTED_OPEN_ERROR_FIFO_COUNTER);
    }
    result->logical_bytes = result->fifo_plan.logical_bytes;

    result->stage = P4_HOSTED_OPEN_STAGE_BUFFER_CAPACITY;
    if ((size_t)result->fifo_plan.bus_bytes > capacity) {
        (void)p4_hosted_fifo_finish(&result->fifo_state, generation,
                                    0, 0, 0);
        return fail_gate(result, result->stage,
                         P4_HOSTED_OPEN_ERROR_BUFFER_TOO_SMALL);
    }

    result->stage = P4_HOSTED_OPEN_STAGE_FIFO_BUDGET;
    if (!gate_budget(ops, result, result->stage)) {
        (void)p4_hosted_fifo_finish(&result->fifo_state, generation,
                                    0, 0, 0);
        return 0;
    }

    result->stage = P4_HOSTED_OPEN_STAGE_READ_FIFO;
    if (ops->read_fifo(ops->context, &result->fifo_plan, buffer) != 1) {
        (void)p4_hosted_fifo_finish(&result->fifo_state, generation,
                                    1, 0, 0);
        result->fifo_poisoned = result->fifo_state.poisoned;
        result->stream_ambiguous = 1;
        if (ops->budget_ok(ops->context) != 1)
            return fail_gate(result, result->stage,
                             P4_HOSTED_OPEN_ERROR_BUDGET_OR_GENERATION);
        return fail_gate(result, result->stage,
                         P4_HOSTED_OPEN_ERROR_CALLBACK);
    }

    result->stream_consumed = 1;
    if (!gate_budget(ops, result, P4_HOSTED_OPEN_STAGE_FINISH_FIFO)) {
        (void)p4_hosted_fifo_finish(&result->fifo_state, generation,
                                    1, 0, 0);
        result->fifo_poisoned = result->fifo_state.poisoned;
        return 0;
    }
    if (!p4_hosted_fifo_finish(&result->fifo_state, generation,
                               1, 1, result->fifo_plan.bus_bytes)) {
        result->fifo_poisoned = result->fifo_state.poisoned;
        return fail_gate(result, P4_HOSTED_OPEN_STAGE_FINISH_FIFO,
                         P4_HOSTED_OPEN_ERROR_FIFO_COUNTER);
    }
    if (result->fifo_state.poisoned)
        result->fifo_poisoned = 1;

    result->stage = P4_HOSTED_OPEN_STAGE_DECODE_FRAME;
    if (!p4_hosted_decode(buffer, result->logical_bytes, 0, &result->frame))
        return fail_gate(result, result->stage,
                         P4_HOSTED_OPEN_ERROR_INVALID_FRAME);

    result->stage = P4_HOSTED_OPEN_STAGE_VALIDATE_FRAME;
    if (result->frame.interface_type != P4_HOSTED_PRIVATE_INTERFACE ||
        result->frame.interface_number != 0 ||
        result->frame.packet_type != P4_HOSTED_PRIVATE_INIT_PACKET ||
        result->frame.flags != 0)
        return fail_gate(result, result->stage,
                         P4_HOSTED_OPEN_ERROR_FRAME_PROFILE);

    result->stage = P4_HOSTED_OPEN_STAGE_PARSE_INIT;
    if (!p4_hosted_init_parse(result->frame.payload,
                              result->frame.payload_size, &result->init))
        return fail_gate(result, result->stage,
                         P4_HOSTED_OPEN_ERROR_INVALID_INIT);

    result->stage = P4_HOSTED_OPEN_STAGE_VALIDATE_CHIP;
    if (result->init.chip_id != P4_HOSTED_INIT_CHIP_ESP32C6)
        return fail_gate(result, result->stage,
                         P4_HOSTED_OPEN_ERROR_UNSUPPORTED_CHIP);

    checksum_enabled = (result->init.capabilities &
                        P4_HOSTED_CHECKSUM_CAPABILITY) != 0;
    if (checksum_enabled) {
        result->stage = P4_HOSTED_OPEN_STAGE_CHECKSUM;
        if (!p4_hosted_decode(buffer, result->frame.wire_size, 1, &decoded))
            return fail_gate(result, result->stage,
                             P4_HOSTED_OPEN_ERROR_CHECKSUM);
        result->frame = decoded;
    }

    stream_offset = result->frame.wire_size;
    while (stream_offset < result->logical_bytes) {
        size_t remaining = (size_t)result->logical_bytes - stream_offset;
        const uint8_t *frame_bytes = buffer + stream_offset;

        result->stage = P4_HOSTED_OPEN_STAGE_DECODE_FRAME;
        if (!decode_profile_frame(frame_bytes, remaining, checksum_enabled,
                                  &additional)) {
            if (checksum_enabled &&
                p4_hosted_decode(frame_bytes, remaining, 0, &decoded))
                return fail_gate(result, P4_HOSTED_OPEN_STAGE_CHECKSUM,
                                 P4_HOSTED_OPEN_ERROR_CHECKSUM);
            return fail_gate(result, result->stage,
                             P4_HOSTED_OPEN_ERROR_INVALID_FRAME);
        }
        result->stage = P4_HOSTED_OPEN_STAGE_VALIDATE_FRAME;
        if (additional.wire_size == 0 || additional.flags != 0)
            return fail_gate(result, result->stage,
                             P4_HOSTED_OPEN_ERROR_FRAME_PROFILE);
        if (additional.interface_type == P4_HOSTED_PRIVATE_INTERFACE)
            return fail_gate(result, result->stage,
                             P4_HOSTED_OPEN_ERROR_DUPLICATE_INIT);
        if (additional.interface_type != P4_HOSTED_SERIAL ||
            additional.interface_number != 0)
            return fail_gate(result, result->stage,
                             P4_HOSTED_OPEN_ERROR_FRAME_PROFILE);

        ++result->additional_frame_count;
        result->additional_frame_bytes += (uint32_t)additional.wire_size;
        ++result->additional_serial_frame_count;
        result->additional_bytes_discarded +=
            (uint32_t)additional.wire_size;
        stream_offset += additional.wire_size;
    }

    result->stage = P4_HOSTED_OPEN_STAGE_NONE;
    result->error = P4_HOSTED_OPEN_ERROR_NONE;
    result->gate_ok = 1;
    return 1;
}

static void cleanup_failure(struct p4_hosted_open_result *result,
                            enum p4_hosted_open_stage stage,
                            enum p4_hosted_open_cleanup_error error)
{
    if (result->cleanup_error == P4_HOSTED_OPEN_CLEANUP_NOT_RUN) {
        result->cleanup_stage = stage;
        result->cleanup_error = error;
    }
}

/* Returns false only when the budget has expired. A failed bus callback is
 * recorded, but the next restoration operation is still attempted if budget
 * remains, so cleanup makes best effort across all saved Fn0 bytes. */
static int cleanup_after_callback(
    const struct p4_hosted_open_ops *ops,
    struct p4_hosted_open_result *result,
    enum p4_hosted_open_stage stage, int callback_ok)
{
    if (callback_ok != 1)
        cleanup_failure(result, stage, P4_HOSTED_OPEN_CLEANUP_CALLBACK);
    if (ops->budget_ok(ops->context) != 1) {
        cleanup_failure(result, stage,
                        P4_HOSTED_OPEN_CLEANUP_BUDGET_OR_GENERATION);
        return 0;
    }
    return 1;
}

static int cleanup_budget(const struct p4_hosted_open_ops *ops,
                          struct p4_hosted_open_result *result,
                          enum p4_hosted_open_stage stage)
{
    if (ops->budget_ok(ops->context) == 1)
        return 1;
    cleanup_failure(result, stage,
                    P4_HOSTED_OPEN_CLEANUP_BUDGET_OR_GENERATION);
    return 0;
}

static int cleanup_write(const struct p4_hosted_open_ops *ops,
                         struct p4_hosted_open_result *result,
                         enum p4_hosted_open_stage stage,
                         unsigned function, uint32_t address,
                         uint8_t value, unsigned attempted_flag,
                         unsigned success_flag)
{
    int ok;
    if (!cleanup_budget(ops, result, stage))
        return 0;
    result->cleanup_flags |= attempted_flag;
    ok = ops->write_byte(ops->context, function, address, value);
    if (ok == 1)
        result->cleanup_flags |= success_flag;
    return cleanup_after_callback(ops, result, stage, ok);
}

static int cleanup_read_verify(const struct p4_hosted_open_ops *ops,
                               struct p4_hosted_open_result *result,
                               enum p4_hosted_open_stage stage,
                               uint32_t address, uint8_t expected,
                               unsigned attempted_flag,
                               unsigned success_flag)
{
    uint8_t value = 0;
    int ok;
    if (!cleanup_budget(ops, result, stage))
        return 0;
    result->cleanup_flags |= attempted_flag;
    ok = ops->read_byte(ops->context, 0, address, &value);
    if (ok == 1 && value == expected)
        result->cleanup_flags |= success_flag;
    else if (ok == 1)
        cleanup_failure(result, stage,
                        P4_HOSTED_OPEN_CLEANUP_VERIFY_MISMATCH);
    return cleanup_after_callback(ops, result, stage, ok);
}

static int cleanup_requirements_met(
    const struct p4_hosted_open_result *result)
{
    unsigned required = 0;
    if (result->open_attempted)
        required |= P4_HOSTED_OPEN_CLEANUP_CLOSE_ATTEMPTED |
            P4_HOSTED_OPEN_CLEANUP_CLOSE_WRITE_OK;
    if (result->fn0_mutation_attempted)
        required |= P4_HOSTED_OPEN_CLEANUP_WRITE_IE_ATTEMPTED |
            P4_HOSTED_OPEN_CLEANUP_WRITE_BLOCK_LOW_ATTEMPTED |
            P4_HOSTED_OPEN_CLEANUP_WRITE_BLOCK_HIGH_ATTEMPTED |
            P4_HOSTED_OPEN_CLEANUP_WRITE_IE_OK |
            P4_HOSTED_OPEN_CLEANUP_WRITE_BLOCK_LOW_OK |
            P4_HOSTED_OPEN_CLEANUP_WRITE_BLOCK_HIGH_OK |
            P4_HOSTED_OPEN_CLEANUP_READ_IE_ATTEMPTED |
            P4_HOSTED_OPEN_CLEANUP_READ_BLOCK_LOW_ATTEMPTED |
            P4_HOSTED_OPEN_CLEANUP_READ_BLOCK_HIGH_ATTEMPTED |
            P4_HOSTED_OPEN_CLEANUP_VERIFY_IE_OK |
            P4_HOSTED_OPEN_CLEANUP_VERIFY_BLOCK_LOW_OK |
            P4_HOSTED_OPEN_CLEANUP_VERIFY_BLOCK_HIGH_OK;
    return (result->cleanup_flags & required) == required;
}

int p4_hosted_open_cleanup(const struct p4_hosted_open_ops *ops,
                           struct p4_hosted_open_result *result)
{
    if (!result || result->initialized != 1)
        return 0;
    if (ops && ranges_overlap(ops, sizeof *ops, result, sizeof *result))
        return 0;
    if (result->cleanup_attempted)
        return result->cleanup_complete == 1;
    result->cleanup_attempted = 1;

    if (!ops || !ops->budget_ok ||
        ((result->open_attempted || result->fn0_mutation_attempted) &&
         !ops->write_byte) ||
        (result->fn0_mutation_attempted && !ops->read_byte)) {
        cleanup_failure(result, P4_HOSTED_OPEN_STAGE_ARGUMENTS,
                        P4_HOSTED_OPEN_CLEANUP_INVALID_STATE);
        return 0;
    }
    if (result->fn0_mutation_attempted && !valid_snapshot(result)) {
        cleanup_failure(result, P4_HOSTED_OPEN_STAGE_ARGUMENTS,
                        P4_HOSTED_OPEN_CLEANUP_INVALID_STATE);
        return 0;
    }

    if (result->open_attempted) {
        int ok;
        if (!cleanup_budget(ops, result,
                            P4_HOSTED_OPEN_STAGE_CLEANUP_CLOSE))
            return 0;
        result->cleanup_flags |= P4_HOSTED_OPEN_CLEANUP_CLOSE_ATTEMPTED;
        ok = ops->write_byte(ops->context, P4_HOSTED_OPEN_FUNCTION,
                             P4_HOSTED_OPEN_CONTROL_ADDRESS,
                             P4_HOSTED_CLOSE_VALUE);
        if (ok == 1)
            result->cleanup_flags |= P4_HOSTED_OPEN_CLEANUP_CLOSE_WRITE_OK;
        if (!cleanup_after_callback(ops, result,
                                    P4_HOSTED_OPEN_STAGE_CLEANUP_CLOSE, ok))
            return 0;
    }

    if (result->fn0_mutation_attempted) {
        if (!cleanup_write(ops, result,
                P4_HOSTED_OPEN_STAGE_CLEANUP_WRITE_INTERRUPT_ENABLE, 0,
                P4_HOSTED_CCCR_INT_ENABLE, result->saved_interrupt_enable,
                P4_HOSTED_OPEN_CLEANUP_WRITE_IE_ATTEMPTED,
                P4_HOSTED_OPEN_CLEANUP_WRITE_IE_OK))
            return 0;
        if (!cleanup_write(ops, result,
                P4_HOSTED_OPEN_STAGE_CLEANUP_WRITE_BLOCK_LOW, 0,
                P4_HOSTED_CCCR_BLOCK_LOW, result->saved_block_low,
                P4_HOSTED_OPEN_CLEANUP_WRITE_BLOCK_LOW_ATTEMPTED,
                P4_HOSTED_OPEN_CLEANUP_WRITE_BLOCK_LOW_OK))
            return 0;
        if (!cleanup_write(ops, result,
                P4_HOSTED_OPEN_STAGE_CLEANUP_WRITE_BLOCK_HIGH, 0,
                P4_HOSTED_CCCR_BLOCK_HIGH, result->saved_block_high,
                P4_HOSTED_OPEN_CLEANUP_WRITE_BLOCK_HIGH_ATTEMPTED,
                P4_HOSTED_OPEN_CLEANUP_WRITE_BLOCK_HIGH_OK))
            return 0;
        if (!cleanup_read_verify(ops, result,
                P4_HOSTED_OPEN_STAGE_CLEANUP_READ_INTERRUPT_ENABLE,
                P4_HOSTED_CCCR_INT_ENABLE, result->saved_interrupt_enable,
                P4_HOSTED_OPEN_CLEANUP_READ_IE_ATTEMPTED,
                P4_HOSTED_OPEN_CLEANUP_VERIFY_IE_OK))
            return 0;
        if (!cleanup_read_verify(ops, result,
                P4_HOSTED_OPEN_STAGE_CLEANUP_READ_BLOCK_LOW,
                P4_HOSTED_CCCR_BLOCK_LOW, result->saved_block_low,
                P4_HOSTED_OPEN_CLEANUP_READ_BLOCK_LOW_ATTEMPTED,
                P4_HOSTED_OPEN_CLEANUP_VERIFY_BLOCK_LOW_OK))
            return 0;
        if (!cleanup_read_verify(ops, result,
                P4_HOSTED_OPEN_STAGE_CLEANUP_READ_BLOCK_HIGH,
                P4_HOSTED_CCCR_BLOCK_HIGH, result->saved_block_high,
                P4_HOSTED_OPEN_CLEANUP_READ_BLOCK_HIGH_ATTEMPTED,
                P4_HOSTED_OPEN_CLEANUP_VERIFY_BLOCK_HIGH_OK))
            return 0;
    }

    result->cleanup_complete = (unsigned)cleanup_requirements_met(result);
    if (result->cleanup_complete &&
        result->cleanup_error == P4_HOSTED_OPEN_CLEANUP_NOT_RUN)
        result->cleanup_error = P4_HOSTED_OPEN_CLEANUP_OK;
    else if (!result->cleanup_complete &&
             result->cleanup_error == P4_HOSTED_OPEN_CLEANUP_NOT_RUN)
        cleanup_failure(result, P4_HOSTED_OPEN_STAGE_ARGUMENTS,
                        P4_HOSTED_OPEN_CLEANUP_INVALID_STATE);
    return result->cleanup_complete == 1;
}
