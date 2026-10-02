/* Private diagnostic wire protocol; not an Exec ABI. One owner per lane.
 * All hardware accesses use the internal-SRAM uncached alias. A ticket is
 * published LAST after a release fence; its reader performs an acquire fence.
 * Only hart0 writes request/input and only hart1 writes response/output.
 * Neither owner may reuse its payload until the matching ticket is received.
 */
#ifndef P4_SECONDARY_MAILBOX_H
#define P4_SECONDARY_MAILBOX_H
#include <stdint.h>
#define P4_MAIL_WORDS 256u
#define P4_MAIL_OK 1u
#define P4_MAIL_EPOCH 2u
#define P4_MAIL_SEQUENCE 3u
#define P4_MAIL_LENGTH 4u
#define P4_MAIL_CHECKSUM 5u
struct p4_mailbox {
    uint32_t request[16]; /* ticket, epoch, sequence, words, checksum */
    uint32_t response[16]; /* ticket, status, sequence, checksum, accepted */
    uint32_t input[P4_MAIL_WORDS];
    uint32_t output[P4_MAIL_WORDS];
} __attribute__((aligned(64)));
struct p4_mail_state { uint32_t ticket, sequence, accepted; };
static inline __attribute__((always_inline)) void p4_mail_fence(void)
{
#ifndef P4_SECONDARY_HOST_TEST
    __asm__ volatile("fence rw, rw" ::: "memory");
#endif
}
static inline __attribute__((always_inline)) uint32_t
p4_mail_hash(volatile uint32_t *words, uint32_t count)
{
    uint32_t hash = 0x811c9dc5u, i;
    for (i = 0; i < count; ++i)
        hash = (hash ^ words[i]) * 0x01000193u;
    return hash;
}
/* Exactly one bounded transaction. A duplicate ticket has no side effects;
 * duplicate/stale logical sequences with a fresh ticket receive a refusal.
 * Epoch/sequence/ticket wrap is not supported: each boot starts a new epoch.
 */
static inline __attribute__((always_inline)) int
p4_mail_step_payload(volatile struct p4_mailbox *mail, struct p4_mail_state *state,
             uint32_t epoch, volatile uint32_t *input, volatile uint32_t *output)
{
    uint32_t ticket = mail->request[0], seq, length, status, hash = 0, i;
    if (!ticket || ticket <= state->ticket)
        return 0;
    p4_mail_fence();
    seq = mail->request[2];
    length = mail->request[3];
    if (mail->request[1] != epoch)
        status = P4_MAIL_EPOCH;
    else if (state->sequence == UINT32_MAX || seq != state->sequence + 1u)
        status = P4_MAIL_SEQUENCE;
    else if (length != P4_MAIL_WORDS)
        status = P4_MAIL_LENGTH;
    else if (p4_mail_hash(input, length) != mail->request[4])
        status = P4_MAIL_CHECKSUM;
    else {
        for (i = 0; i < length; ++i)
            output[i] = input[i] ^ (0xa5a50000u | seq);
        hash = p4_mail_hash(output, length);
        state->sequence = seq;
        ++state->accepted;
        status = P4_MAIL_OK;
    }
    mail->response[1] = status;
    mail->response[2] = seq;
    mail->response[3] = hash;
    mail->response[4] = state->accepted;
    p4_mail_fence();
    mail->response[0] = ticket;
    p4_mail_fence();
    state->ticket = ticket;
    return 1;
}
static inline __attribute__((always_inline)) int
p4_mail_step(volatile struct p4_mailbox *mail, struct p4_mail_state *state,
             uint32_t epoch)
{
    return p4_mail_step_payload(mail, state, epoch, mail->input, mail->output);
}
/* Diagnostic lane selector. Mode is part of the published request tuple,
   not a hint that may be sampled before ticket acquisition. External
   pointers are a trusted primary reservation, never wire-supplied addresses. */
static inline __attribute__((always_inline)) int
p4_mail_step_selected(volatile struct p4_mailbox *mail,
                     struct p4_mail_state *state, uint32_t epoch,
                     volatile uint32_t *external_input,
                     volatile uint32_t *external_output)
{
    if (!mail->request[0] || mail->request[0] <= state->ticket) return 0;
    p4_mail_fence();
    if (mail->request[5] > 1 ||
        (mail->request[5] == 1 && (!external_input || !external_output)))
        return 0; /* never acknowledge an unavailable/unknown lane */
    if (mail->request[5] == 1)
        return p4_mail_step_payload(mail, state, epoch,
                                    external_input, external_output);
    return p4_mail_step(mail, state, epoch);
}
#endif
