#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define P4_SECONDARY_HOST_TEST 1
#include "../secondary_mailbox.h"

#define GUARD_WORDS 4u
#define GUARD_BEFORE 0x62c50000u
#define GUARD_AFTER  0xa7310000u
#define OUTPUT_POISON 0xd3adbeefu

struct guarded_words {
    uint32_t before[GUARD_WORDS];
    volatile uint32_t words[P4_MAIL_WORDS];
    uint32_t after[GUARD_WORDS];
};

struct guarded_mailbox {
    uint32_t before[GUARD_WORDS];
    struct p4_mailbox mailbox;
    uint32_t after[GUARD_WORDS];
};

static struct p4_mailbox mail;
static struct p4_mail_state state;
static struct guarded_words input, output;
static struct guarded_mailbox selected_mail;
static struct p4_mail_state selected_state;
static uint32_t ticket;
static uint32_t selected_ticket;

static void set_guards(struct guarded_words *buffer, uint32_t salt)
{
    unsigned int i;
    for (i = 0; i < GUARD_WORDS; ++i) {
        buffer->before[i] = GUARD_BEFORE ^ salt ^ i;
        buffer->after[i] = GUARD_AFTER ^ salt ^ i;
    }
}

static void check_guards(const struct guarded_words *buffer, uint32_t salt)
{
    unsigned int i;
    for (i = 0; i < GUARD_WORDS; ++i) {
        assert(buffer->before[i] == (GUARD_BEFORE ^ salt ^ i));
        assert(buffer->after[i] == (GUARD_AFTER ^ salt ^ i));
    }
}

static void fill_input(uint32_t seq)
{
    unsigned int i;
    for (i = 0; i < P4_MAIL_WORDS; ++i)
        input.words[i] = i * 7u + seq;
    check_guards(&input, 0x13579bdfu);
}

static void poison_output(uint32_t salt)
{
    unsigned int i;
    for (i = 0; i < P4_MAIL_WORDS; ++i)
        output.words[i] = OUTPUT_POISON ^ salt ^ i;
    check_guards(&output, 0x2468ace0u);
}

static void save_output(uint32_t snapshot[P4_MAIL_WORDS])
{
    unsigned int i;
    for (i = 0; i < P4_MAIL_WORDS; ++i)
        snapshot[i] = output.words[i];
}

static void check_output(const uint32_t snapshot[P4_MAIL_WORDS])
{
    unsigned int i;
    for (i = 0; i < P4_MAIL_WORDS; ++i)
        assert(output.words[i] == snapshot[i]);
    check_guards(&output, 0x2468ace0u);
}

static void check_mail_guards(void)
{
    check_guards(&input, 0x13579bdfu);
    check_guards(&output, 0x2468ace0u);
}

static void publish(uint32_t epoch, uint32_t seq, uint32_t length,
                    uint32_t checksum_xor)
{
    fill_input(seq);
    mail.request[1] = epoch;
    mail.request[2] = seq;
    mail.request[3] = length;
    mail.request[4] = p4_mail_hash(input.words, P4_MAIL_WORDS) ^ checksum_xor;
    mail.request[0] = ++ticket;
}

static void transact_for(uint32_t worker_epoch, uint32_t request_epoch,
                        uint32_t seq, uint32_t length,
                        uint32_t checksum_xor, uint32_t expected)
{
    uint32_t output_before[P4_MAIL_WORDS];
    uint32_t accepted_before = state.accepted;
    uint32_t sequence_before = state.sequence;
    unsigned int i;
    int stepped;

    publish(request_epoch, seq, length, checksum_xor);
    poison_output(ticket);
    save_output(output_before);
    stepped = p4_mail_step_payload(&mail, &state, worker_epoch,
                                   input.words, output.words);
    assert(stepped == 1);
    if (mail.response[0] != ticket || mail.response[1] != expected)
        fprintf(stderr, "transaction ticket=%u response-ticket=%u seq=%u epoch=%u expected=%u got=%u\n",
                ticket, mail.response[0], seq, request_epoch, expected,
                mail.response[1]);
    assert(mail.response[0] == ticket && mail.response[1] == expected);
    assert(mail.response[2] == seq && mail.response[4] == state.accepted);
    assert(state.ticket == ticket);
    if (expected == P4_MAIL_OK) {
        assert(state.sequence == seq && state.accepted == accepted_before + 1u);
        for (i = 0; i < P4_MAIL_WORDS; ++i)
            assert(output.words[i] == (input.words[i] ^ (0xa5a50000u | seq)));
        assert(mail.response[3] == p4_mail_hash(output.words, P4_MAIL_WORDS));
    } else {
        assert(state.sequence == sequence_before);
        assert(state.accepted == accepted_before);
        assert(mail.response[3] == 0);
        check_output(output_before);
    }
    check_mail_guards();
}

static void transact(uint32_t request_epoch, uint32_t seq, uint32_t length,
                     uint32_t checksum_xor, uint32_t expected)
{
    transact_for(123, request_epoch, seq, length, checksum_xor, expected);
}

static void check_unpublished_request(void)
{
    uint32_t output_before[P4_MAIL_WORDS];
    uint32_t response_before[16];
    unsigned int i;
    int stepped;

    fill_input(1);
    poison_output(0x100u);
    save_output(output_before);
    for (i = 0; i < 16; ++i)
        response_before[i] = mail.response[i];
    mail.request[1] = 123;
    mail.request[2] = 1;
    mail.request[3] = P4_MAIL_WORDS;
    mail.request[4] = p4_mail_hash(input.words, P4_MAIL_WORDS);
    mail.request[0] = 0; /* Payload present, but no publication ticket. */

    stepped = p4_mail_step_payload(&mail, &state, 123,
                                   input.words, output.words);
    assert(!stepped);
    assert(state.ticket == 0 && state.sequence == 0 && state.accepted == 0);
    for (i = 0; i < 16; ++i)
        assert(mail.response[i] == response_before[i]);
    check_output(output_before);
    check_mail_guards();
}

static void check_stale_and_duplicate_tickets(void)
{
    uint32_t output_before[P4_MAIL_WORDS];
    uint32_t response_before[16];
    uint32_t ticket_before = state.ticket;
    uint32_t sequence_before = state.sequence;
    uint32_t accepted_before = state.accepted;
    unsigned int i;

    /* A duplicate published ticket must not re-run a next-sequence payload. */
    fill_input(state.sequence + 1u);
    mail.request[1] = 123;
    mail.request[2] = state.sequence + 1u;
    mail.request[3] = P4_MAIL_WORDS;
    mail.request[4] = p4_mail_hash(input.words, P4_MAIL_WORDS);
    mail.request[0] = state.ticket;
    poison_output(0x200u);
    save_output(output_before);
    for (i = 0; i < 16; ++i)
        response_before[i] = mail.response[i];
    assert(!p4_mail_step_payload(&mail, &state, 123,
                                 input.words, output.words));
    assert(state.ticket == ticket_before && state.sequence == sequence_before);
    assert(state.accepted == accepted_before);
    for (i = 0; i < 16; ++i)
        assert(mail.response[i] == response_before[i]);
    check_output(output_before);
    check_mail_guards();

    /* A lower ticket is stale even though its logical sequence is next. */
    mail.request[0] = state.ticket - 1u;
    poison_output(0x300u);
    save_output(output_before);
    for (i = 0; i < 16; ++i)
        response_before[i] = mail.response[i];
    assert(!p4_mail_step_payload(&mail, &state, 123,
                                 input.words, output.words));
    assert(state.ticket == ticket_before && state.sequence == sequence_before);
    assert(state.accepted == accepted_before);
    for (i = 0; i < 16; ++i)
        assert(mail.response[i] == response_before[i]);
    check_output(output_before);
    check_mail_guards();
}

static void reset_protocol_state(void)
{
    memset(&mail, 0, sizeof mail);
    memset(&state, 0, sizeof state);
    ticket = 0;
}

static void check_embedded_wrapper(void)
{
    struct p4_mailbox embedded = {0};
    struct p4_mail_state embedded_state = {0};
    unsigned int i;

    for (i = 0; i < P4_MAIL_WORDS; ++i)
        embedded.input[i] = i * 11u + 1u;
    embedded.request[1] = 777;
    embedded.request[2] = 1;
    embedded.request[3] = P4_MAIL_WORDS;
    embedded.request[4] = p4_mail_hash(embedded.input, P4_MAIL_WORDS);
    embedded.request[0] = 1;
    assert(p4_mail_step(&embedded, &embedded_state, 777));
    assert(embedded.response[0] == 1 && embedded.response[1] == P4_MAIL_OK);
    for (i = 0; i < P4_MAIL_WORDS; ++i)
        assert(embedded.output[i] == (embedded.input[i] ^ 0xa5a50001u));
    assert(!p4_mail_step(&embedded, &embedded_state, 777));
}

static void set_selected_guards(void)
{
    unsigned int i;
    for (i = 0; i < GUARD_WORDS; ++i) {
        selected_mail.before[i] = 0x51a70000u ^ i;
        selected_mail.after[i] = 0x93e20000u ^ i;
    }
}

static void check_selected_guards(void)
{
    unsigned int i;
    check_mail_guards();
    for (i = 0; i < GUARD_WORDS; ++i) {
        assert(selected_mail.before[i] == (0x51a70000u ^ i));
        assert(selected_mail.after[i] == (0x93e20000u ^ i));
    }
}

static void fill_selected_inputs(uint32_t seq)
{
    unsigned int i;
    for (i = 0; i < P4_MAIL_WORDS; ++i) {
        input.words[i] = 0x11110000u ^ (i * 3u + seq);
        selected_mail.mailbox.input[i] = 0x22220000u ^ (i * 5u + seq);
    }
    check_selected_guards();
    assert(p4_mail_hash(input.words, P4_MAIL_WORDS) !=
           p4_mail_hash(selected_mail.mailbox.input, P4_MAIL_WORDS));
}

static void poison_selected_outputs(uint32_t salt)
{
    unsigned int i;
    poison_output(salt);
    for (i = 0; i < P4_MAIL_WORDS; ++i)
        selected_mail.mailbox.output[i] = 0xc0010000u ^ salt ^ i;
    check_selected_guards();
}

static void save_embedded_output(uint32_t snapshot[P4_MAIL_WORDS])
{
    unsigned int i;
    for (i = 0; i < P4_MAIL_WORDS; ++i)
        snapshot[i] = selected_mail.mailbox.output[i];
}

static void check_embedded_output(const uint32_t snapshot[P4_MAIL_WORDS])
{
    unsigned int i;
    for (i = 0; i < P4_MAIL_WORDS; ++i)
        assert(selected_mail.mailbox.output[i] == snapshot[i]);
    check_selected_guards();
}

static void check_selected_noop(volatile uint32_t *external_input,
                                volatile uint32_t *external_output)
{
    uint32_t request_before[16], response_before[16];
    uint32_t external_before[P4_MAIL_WORDS];
    uint32_t embedded_before[P4_MAIL_WORDS];
    struct p4_mail_state state_before = selected_state;
    unsigned int i;

    save_output(external_before);
    save_embedded_output(embedded_before);
    for (i = 0; i < 16; ++i) {
        request_before[i] = selected_mail.mailbox.request[i];
        response_before[i] = selected_mail.mailbox.response[i];
    }
    assert(!p4_mail_step_selected(&selected_mail.mailbox, &selected_state,
                                  321, external_input, external_output));
    assert(selected_state.ticket == state_before.ticket);
    assert(selected_state.sequence == state_before.sequence);
    assert(selected_state.accepted == state_before.accepted);
    for (i = 0; i < 16; ++i) {
        assert(selected_mail.mailbox.request[i] == request_before[i]);
        assert(selected_mail.mailbox.response[i] == response_before[i]);
    }
    check_output(external_before);
    check_embedded_output(embedded_before);
}

static void prepare_selected_request(uint32_t seq, uint32_t mode,
                                     uint32_t checksum_mode)
{
    volatile uint32_t *checksum_words;
    fill_selected_inputs(seq);
    checksum_words = checksum_mode ? input.words :
                                     selected_mail.mailbox.input;
    selected_mail.mailbox.request[1] = 321;
    selected_mail.mailbox.request[2] = seq;
    selected_mail.mailbox.request[3] = P4_MAIL_WORDS;
    selected_mail.mailbox.request[4] =
        p4_mail_hash(checksum_words, P4_MAIL_WORDS);
    selected_mail.mailbox.request[5] = mode;
}

static void accept_selected_request(uint32_t seq, uint32_t mode)
{
    uint32_t external_before[P4_MAIL_WORDS];
    uint32_t embedded_before[P4_MAIL_WORDS];
    volatile uint32_t *selected_input = mode ? input.words :
                                               selected_mail.mailbox.input;
    volatile uint32_t *selected_output_words = mode ? output.words :
                                                      selected_mail.mailbox.output;
    unsigned int i;
    uint32_t accepted_before = selected_state.accepted;

    poison_selected_outputs(selected_ticket);
    save_output(external_before);
    save_embedded_output(embedded_before);
    assert(p4_mail_step_selected(&selected_mail.mailbox, &selected_state,
                                 321, input.words, output.words));
    assert(selected_mail.mailbox.response[0] == selected_ticket);
    assert(selected_mail.mailbox.response[1] == P4_MAIL_OK);
    assert(selected_mail.mailbox.response[2] == seq);
    assert(selected_mail.mailbox.response[4] == accepted_before + 1u);
    assert(selected_state.ticket == selected_ticket);
    assert(selected_state.sequence == seq);
    assert(selected_state.accepted == accepted_before + 1u);
    for (i = 0; i < P4_MAIL_WORDS; ++i)
        assert(selected_output_words[i] ==
               (selected_input[i] ^ (0xa5a50000u | seq)));
    assert(selected_mail.mailbox.response[3] ==
           p4_mail_hash(selected_output_words, P4_MAIL_WORDS));
    if (mode)
        check_embedded_output(embedded_before);
    else
        check_output(external_before);
    check_selected_guards();
}

static void selected_success(uint32_t seq, uint32_t mode)
{
    prepare_selected_request(seq, mode, mode);
    /* Mode and payload are staged while the previous ticket is still current. */
    selected_mail.mailbox.request[0] = selected_ticket;
    poison_selected_outputs(seq);
    check_selected_noop(input.words, output.words);
    ++selected_ticket;
    selected_mail.mailbox.request[0] = selected_ticket; /* publish last */
    accept_selected_request(seq, mode);
}

static void selected_wrong_lane_checksum(uint32_t seq, uint32_t mode)
{
    uint32_t sequence_before = selected_state.sequence;
    uint32_t accepted_before = selected_state.accepted;
    uint32_t external_before[P4_MAIL_WORDS];
    uint32_t embedded_before[P4_MAIL_WORDS];

    prepare_selected_request(seq, mode, mode ^ 1u);
    assert(selected_mail.mailbox.request[4] !=
           p4_mail_hash(mode ? input.words : selected_mail.mailbox.input,
                        P4_MAIL_WORDS));
    poison_selected_outputs(selected_ticket + 1u);
    save_output(external_before);
    save_embedded_output(embedded_before);
    ++selected_ticket;
    selected_mail.mailbox.request[0] = selected_ticket;
    assert(p4_mail_step_selected(&selected_mail.mailbox, &selected_state,
                                 321, input.words, output.words));
    assert(selected_mail.mailbox.response[0] == selected_ticket);
    assert(selected_mail.mailbox.response[1] == P4_MAIL_CHECKSUM);
    assert(selected_mail.mailbox.response[2] == seq);
    assert(selected_mail.mailbox.response[3] == 0);
    assert(selected_mail.mailbox.response[4] == accepted_before);
    assert(selected_state.ticket == selected_ticket);
    assert(selected_state.sequence == sequence_before);
    assert(selected_state.accepted == accepted_before);
    check_output(external_before);
    check_embedded_output(embedded_before);
}

static void selected_missing_pointer_recovery(uint32_t seq)
{
    prepare_selected_request(seq, 1, 1);
    poison_selected_outputs(selected_ticket + 1u);
    check_selected_noop(input.words, output.words);
    ++selected_ticket;
    selected_mail.mailbox.request[0] = selected_ticket;
    check_selected_noop(0, output.words);
    check_selected_noop(input.words, 0);
    /* The same unacknowledged ticket succeeds once both trusted pointers exist. */
    accept_selected_request(seq, 1);
}

static void selected_invalid_mode_recovery(uint32_t seq)
{
    prepare_selected_request(seq, 2, 0);
    poison_selected_outputs(selected_ticket + 1u);
    check_selected_noop(input.words, output.words);
    ++selected_ticket;
    selected_mail.mailbox.request[0] = selected_ticket;
    check_selected_noop(input.words, output.words);

    /* Unknown modes consume no ticket; retry that ticket on the valid SRAM lane. */
    selected_mail.mailbox.request[5] = 0;
    p4_mail_fence();
    selected_mail.mailbox.request[0] = selected_ticket;
    accept_selected_request(seq, 0);
}

static void check_lane_selector(void)
{
    uint32_t i;

    memset(&selected_mail, 0, sizeof selected_mail);
    memset(&selected_state, 0, sizeof selected_state);
    selected_ticket = 0;
    set_selected_guards();
    set_guards(&input, 0x13579bdfu);
    set_guards(&output, 0x2468ace0u);
    for (i = 1; i <= 1000; ++i)
        selected_success(i, i & 1u); /* odd: external; even: embedded */

    selected_wrong_lane_checksum(1001, 1);
    selected_wrong_lane_checksum(1001, 0);
    selected_success(1001, 1);
    selected_missing_pointer_recovery(1002);
    selected_invalid_mode_recovery(1003);
    assert(selected_state.sequence == 1003);
    assert(selected_state.accepted == 1003);
    assert(selected_state.ticket == selected_ticket);
    check_selected_guards();
}

int main(void)
{
    uint32_t i;

    set_guards(&input, 0x13579bdfu);
    set_guards(&output, 0x2468ace0u);
    poison_output(0);
    check_unpublished_request();

    for (i = 1; i <= 1000; ++i)
        transact(123, i, P4_MAIL_WORDS, 0, P4_MAIL_OK);

    check_stale_and_duplicate_tickets();
    transact(124, 1001, P4_MAIL_WORDS, 0, P4_MAIL_EPOCH);
    transact(123, 1000, P4_MAIL_WORDS, 0, P4_MAIL_SEQUENCE);
    transact(123, 999, P4_MAIL_WORDS, 0, P4_MAIL_SEQUENCE);
    transact(123, 1002, P4_MAIL_WORDS, 0, P4_MAIL_SEQUENCE);
    transact(123, 1001, 0, 0, P4_MAIL_LENGTH);
    transact(123, 1001, UINT32_MAX, 0, P4_MAIL_LENGTH);
    transact(123, 1001, P4_MAIL_WORDS, 1, P4_MAIL_CHECKSUM);
    assert(state.accepted == 1000 && state.sequence == 1000);
    transact(123, 1001, P4_MAIL_WORDS, 0, P4_MAIL_OK);

    state.sequence = UINT32_MAX;
    transact(123, 0, P4_MAIL_WORDS, 0, P4_MAIL_SEQUENCE);

    /* Model a fresh worker/epoch while external buffers still hold old data. */
    reset_protocol_state();
    poison_output(0x400u);
    transact_for(124, 125, 1, P4_MAIL_WORDS, 0, P4_MAIL_EPOCH);
    transact_for(124, 124, 1, P4_MAIL_WORDS, 0, P4_MAIL_OK);
    assert(state.ticket == 2 && state.sequence == 1 && state.accepted == 1);
    check_mail_guards();
    check_embedded_wrapper();
    check_lane_selector();

    puts("mailbox payload/selector: 1000 external-buffer exchanges; guards, unpublished/duplicate/stale tickets, refusal output preservation, epoch/sequence/length/checksum/wrap refusals, fresh worker state, embedded wrapper, alternating selector lanes, null/invalid-mode no-op and same-ticket recovery passed");
    return 0;
}
