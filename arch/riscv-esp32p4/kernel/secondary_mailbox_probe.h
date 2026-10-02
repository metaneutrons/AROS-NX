/* Included by the release controller after its fence/cycle helpers. */
#include "secondary_mailbox.h"
extern struct p4_mailbox __p4_secondary_mailbox;
extern unsigned char __p4_secondary_worker_start[], __p4_secondary_worker_end[];
static volatile struct p4_mailbox *mailbox(void)
{
    return (volatile struct p4_mailbox *)uncached(
        (uint32_t *)&__p4_secondary_mailbox);
}
static void mailbox_prepare(void)
{
    volatile uint32_t *words = (volatile uint32_t *)mailbox();
    unsigned int i;
    /* Only while hart1 is held in reset: discard dirty BSS aliases first. */
    krnP4CacheSyncData(&__p4_secondary_mailbox, sizeof __p4_secondary_mailbox);
    for (i = 0; i < sizeof __p4_secondary_mailbox / sizeof *words; ++i)
        words[i] = 0;
    barrier();
    /* Entry-range maintenance alone does not cover its new C callee. */
    krnP4SyncCode(__p4_secondary_worker_start,
                 (uintptr_t)__p4_secondary_worker_end -
                 (uintptr_t)__p4_secondary_worker_start);
}
static int mailbox_wait(uint32_t ticket)
{
    volatile struct p4_mailbox *mail = mailbox();
    uint32_t start = cycles();
    unsigned int left = 1000000;
    do {
        uint32_t received = mail->response[0];
        barrier();
        if (received == ticket)
            return 1;
    } while (--left && (uint32_t)(cycles() - start) < 36000000u);
    return 0;
}
static int mailbox_request(uint32_t ticket, uint32_t epoch, uint32_t seq,
                           uint32_t length, uint32_t corrupt,
                           uint32_t expected, uint32_t accepted)
{
    volatile struct p4_mailbox *mail = mailbox();
    unsigned int i;
    for (i = 0; i < P4_MAIL_WORDS; ++i)
        mail->input[i] = seq * 0x1020304u ^ (i * 0x01010101u);
    mail->request[1] = epoch;
    mail->request[2] = seq;
    mail->request[3] = length;
    mail->request[4] = p4_mail_hash(mail->input, P4_MAIL_WORDS) ^ corrupt;
    barrier();
    mail->request[0] = ticket; /* publish LAST */
    barrier();
    if (!mailbox_wait(ticket) || mail->response[1] != expected ||
        mail->response[2] != seq || mail->response[4] != accepted)
        return 0;
    if (expected != P4_MAIL_OK)
        return mail->response[3] == 0;
    for (i = 0; i < P4_MAIL_WORDS; ++i)
        if (mail->output[i] != (mail->input[i] ^ (0xa5a50000u | seq)))
            return 0;
    return mail->response[3] == p4_mail_hash(mail->output, P4_MAIL_WORDS);
}
static int mailbox_run(uint32_t epoch)
{
    uint32_t seq, ticket = 0;
    /* Counter-probe: an old/absent ack must not satisfy a fresh ticket. */
    if (mailbox_wait(1))
        return 0;
    for (seq = 1; seq <= 64; ++seq)
        if (!mailbox_request(++ticket, epoch, seq, P4_MAIL_WORDS, 0,
                             P4_MAIL_OK, seq))
            return 0;
    if (!mailbox_request(++ticket, epoch + 1u, 65, P4_MAIL_WORDS, 0,
                         P4_MAIL_EPOCH, 64) ||
        !mailbox_request(++ticket, epoch, 64, P4_MAIL_WORDS, 0,
                         P4_MAIL_SEQUENCE, 64) ||
        !mailbox_request(++ticket, epoch, 63, P4_MAIL_WORDS, 0,
                         P4_MAIL_SEQUENCE, 64) ||
        !mailbox_request(++ticket, epoch, 66, P4_MAIL_WORDS, 0,
                         P4_MAIL_SEQUENCE, 64) ||
        !mailbox_request(++ticket, epoch, 65, UINT32_MAX, 0,
                         P4_MAIL_LENGTH, 64) ||
        !mailbox_request(++ticket, epoch, 65, P4_MAIL_WORDS, 1,
                         P4_MAIL_CHECKSUM, 64))
        return 0;
    return mailbox_request(++ticket, epoch, 65, P4_MAIL_WORDS, 0,
                           P4_MAIL_OK, 65);
}
