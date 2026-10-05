/* E2 qualification only. Private SRAM worker, no Exec/allocator/console on
   hart1. CPU/CLIC registers are the deliberate E2-A3 exception, not drivers.
   P4 pre-v3 (hw_ver1) IDF v6.0.1: crosscore_int_ll.h, interrupt_clic_ll.h,
   reg_base.h, hp_system_reg.h, interrupt_core{0,1}_reg.h. */
#include <stdint.h>
#include "hardware.h"
#include "psram.h"
#include "kernel_intern.h"
#include "secondary_hw.h"
#include "secondary_mailbox.h"
#include "secondary_primitives.h"
#define SRAM __attribute__((section(".sramtext.secondary_mailbox"), noinline))
#define INLINE static inline __attribute__((always_inline))
#define E2_LINE 22u
#define E2_FROM(c) (0x500e5010u + (c) * 4u)
#define E2_ROUTE(c, s) (0x500d6000u + (c) * 0x800u + (s) * 4u)
#define E2_SOURCE(c) (79u + (c))
#define E2_CTL (0x3f000100u)
#define E2_SCRATCH (P4_FB_BASE - 4096u)
#define E2_N 8192u
#define CMD_ATOMIC 1u
#define CMD_EXEC 2u
#define CMD_PARK 3u
#define CMD_EXEC_AGAIN 4u
#define CMD_ARM 5u
#define PREDICTOR 0x1030u
struct e2_control {
    uint32_t command[16]; /* id, go, primary-active, resume */
    uint32_t worker[16]; /* ready, done, result, contention, misa, park, resume */
    uint32_t irq_request[2][16]; /* generation, destination */
    uint32_t irq_ack[2][16]; /* generation, count, hart, mcause, fence count */
} __attribute__((aligned(64)));
static struct e2_control e2;
/* Canonical cached internal SRAM ONLY for atomic words, shared L1D. No
   uncached alias is used for these words, including during initialization. */
static struct {
    uint32_t add, cas, lock, protected_count;
} atom __attribute__((aligned(64)));
static uint32_t test_code[16] __attribute__((aligned(64)));
extern unsigned char __p4_e2_irq[];
extern uint32_t __p4_secondary_report[];
extern unsigned char __p4_secondary_stack[], __p4_secondary_stack_end[];
extern struct p4_mailbox __p4_secondary_mailbox;
extern unsigned long __esp32p4_psram_size;
static uint32_t saved_mtvec, saved_mstatus, saved_mie, saved_cfg, saved_threshold;
static uint32_t saved_lines[48], saved_routes[2][128];
static int installed;
INLINE uintptr_t alias(void *p)
{
    uintptr_t out;
    __asm__ volatile("add %0,%1,%2" : "=r"(out) : "r"((uintptr_t)p),
                     "r"(p4_secondary_uncached_offset));
    return out;
}
INLINE volatile struct e2_control *control(void)
{ return (volatile struct e2_control *)alias(&e2); }
INLINE void fence(void) { __asm__ volatile("fence iorw,iorw" ::: "memory"); }
INLINE uint32_t tick(void)
{ uint32_t n; __asm__ volatile("csrr %0,mcycle" : "=r"(n)); return n; }
INLINE uint32_t hart(void)
{ uint32_t n; __asm__ volatile("csrr %0,mhartid" : "=r"(n)); return n; }
INLINE int wait_word(volatile uint32_t *p, uint32_t value)
{
    uint32_t start = tick(), left = 1000000;
    do { uint32_t n = *p; fence(); if (n == value) return 1; }
    while (--left && (uint32_t)(tick() - start) < 36000000u);
    return 0;
}
INLINE int cas_once(uint32_t *p, uint32_t expected, uint32_t desired)
{
    uint32_t old, failed;
    /* A single LR/SC attempt, not GCC's unbounded internal retry loop. */
    __asm__ volatile("lr.w.aq %0,(%2)\nli %1,1\nbne %0,%3,1f\n"
                     "sc.w.rl %1,%4,(%2)\n1:"
                     : "=&r"(old), "=&r"(failed)
                     : "r"(p), "r"(expected), "r"(desired) : "memory");
    return failed == 0;
}
INLINE int atomic_work(unsigned int h)
{
    uint32_t i, left, value, old, retries = 0;
    for (i = 0; i < E2_N; ++i) {
        __asm__ volatile("amoadd.w.aqrl %0,%2,(%1)" : "=r"(old)
                         : "r"(&atom.add), "r"(1u) : "memory");
        value = __atomic_load_n(&atom.cas, __ATOMIC_RELAXED);
        if (!i) {
            volatile struct e2_control *c = control();
            /* Both snapshot CAS=0 before either can attempt its update.
               Exactly one must lose that real shared-word race, even if
               bus arbitration otherwise serializes the remaining loops. */
            fence();
            if (h) { c->worker[13] = 1; fence();
                if (!wait_word(&c->command[8], 1)) return 0; }
            else { c->command[8] = 1; fence();
                if (!wait_word(&c->worker[13], 1)) return 0; }
        }
        left = 1000000;
        do {
            if (cas_once(&atom.cas, value, value + 1u)) break;
            ++retries;
            value = __atomic_load_n(&atom.cas, __ATOMIC_RELAXED);
        } while (--left);
        if (!left) return 0;
        left = 1000000;
        while (!cas_once(&atom.lock, 0, 1) && --left) { ++retries; }
        if (!left) return 0;
        ++atom.protected_count;
        __atomic_store_n(&atom.lock, 0, __ATOMIC_RELEASE);
        if (!i) {
            volatile struct e2_control *c = control();
            /* Both execute an update before either may complete its loop.
               This is a two-way progress barrier, not sequential totals. */
            if (h) { c->worker[8] = 1; fence();
                if (!wait_word(&c->command[5], 1)) return 0; }
            else { c->command[5] = 1; fence();
                if (!wait_word(&c->worker[8], 1)) return 0; }
        }
    }
    if (h) control()->worker[9] = retries;
    else control()->command[6] = retries;
    fence();
    return 1;
}
SRAM int krnP4E2IRQ(void)
{
    volatile struct e2_control *c = control();
    uint32_t h = hart(), cause, generation, destination, reply;
    __asm__ volatile("csrr %0,mcause" : "=r"(cause));
    if (h > 1) return 0;
    fence();
    generation = c->irq_request[h][0];
    destination = c->irq_request[h][1];
    reply = h == 1 && c->irq_request[0][0] != c->irq_ack[0][0];
    c->irq_ack[h][3] = cause;
    if (!(cause & 0x80000000u) || (cause & 0xfffu) != E2_LINE ||
        !(p4_r32(E2_FROM(h)) & 1u)) {
        /* Fatal CPU exception is not a retryable protocol timeout. Stop the
           other hart before private park; never restore a normal image on
           an unconfirmed secondary. Host deadline detects no PASS/READY. */
        if (!h) {
            p4_w32(P4_SECONDARY_RESET_REG,
                p4_r32(P4_SECONDARY_RESET_REG) | P4_SECONDARY_RESET_BIT);
            p4_w32(P4_SECONDARY_CLOCK_REG,
                p4_r32(P4_SECONDARY_CLOCK_REG) & ~P4_SECONDARY_CLOCK_BIT);
            fence();
        }
        return 0;
    }
    p4_w32(E2_FROM(h), 0);
    p4_w32(P4_CLIC_CTRL(E2_LINE), E2_CTL);
    fence();
    __asm__ volatile("fence.i" ::: "memory");
    ++c->irq_ack[h][4];
    c->irq_ack[h][2] = h;
    if (reply) p4_w32(E2_FROM(0), 1);
    if (destination == h) {
        fence();
        c->irq_ack[h][0] = generation;
        fence();
    }
    /* Count is completion, not entry: wrong-destination counter-probes may
       change the request tuple as soon as they observe this publication. */
    ++c->irq_ack[h][1];
    fence();
    return 1;
}
SRAM void krnP4E2WorkerInit(void)
{
    volatile struct e2_control *c = control();
    uint32_t i, misa;
    __asm__ volatile("csrr %0,misa" : "=r"(misa));
    c->worker[4] = misa;
    /* CLIC addresses are hart-local; +0x10000 is REMOTE access only. */
    for (i = 0; i < 48; ++i) p4_w32(P4_CLIC_CTRL(i), 0);
    p4_w32(P4_CLIC_BASE, (p4_r32(P4_CLIC_BASE) & ~0x1eu) | 6u);
    p4_w32(P4_CLIC_BASE + 8, 0);
    p4_w32(P4_CLIC_CTRL(E2_LINE), 0); /* explicit ARM after routes/latches */
    fence();
}
SRAM void krnP4E2WorkerStep(void)
{
    volatile struct e2_control *c = control();
    uint32_t cmd = c->command[0], predictor, start, left;
    if (!cmd || c->worker[1] == cmd) return;
    fence();
    if (cmd == CMD_ATOMIC) {
        c->worker[3] = !cas_once(&atom.lock, 0, 1);
        if (!c->worker[3]) __atomic_store_n(&atom.lock, 0, __ATOMIC_RELEASE);
        fence(); c->worker[0] = cmd; fence();
        if (!wait_word(&c->command[1], cmd) ||
            !wait_word(&c->command[2], cmd)) c->worker[2] = 0;
        else c->worker[2] = atomic_work(1);
    } else if (cmd == CMD_ARM) {
        p4_w32(P4_CLIC_CTRL(E2_LINE), E2_CTL);
        fence(); __asm__ volatile("csrsi mstatus,8" ::: "memory");
        c->worker[2] = 1;
    } else if (cmd == CMD_EXEC || cmd == CMD_EXEC_AGAIN) {
        c->worker[2] = ((uint32_t (*)(void))test_code)();
    } else if (cmd == CMD_PARK) {
        uint32_t sp;
        __asm__ volatile("csrci mstatus,8\ncsrr %0,0x7c1\ncsrc 0x7c1,%1"
            : "=&r"(predictor) : "r"(PREDICTOR) : "memory");
        __asm__ volatile("mv %0,sp" : "=r"(sp));
        c->worker[10] = sp;
        c->worker[11] = predictor & PREDICTOR;
        fence(); c->worker[5] = cmd; fence();
        start = tick(); left = 10000000;
        while (c->command[3] != cmd && --left &&
               (uint32_t)(tick() - start) < 360000000u) { }
        if (!left || c->command[3] != cmd) {
            c->worker[2] = 0;
            /* Never escape SRAM after a failed cache-off rendezvous. */
            fence(); c->worker[1] = cmd; fence();
            for (;;) __asm__ volatile("wfi");
        }
        __asm__ volatile("csrs 0x7c1,%0\ncsrsi mstatus,8" ::
                         "r"(predictor & PREDICTOR) : "memory");
        __asm__ volatile("csrr %0,0x7c1" : "=r"(predictor));
        c->worker[12] = predictor & PREDICTOR;
        c->worker[6] = cmd; c->worker[2] = 1;
    } else c->worker[2] = 0;
    fence(); c->worker[1] = cmd; fence();
}
int krnP4E2Prepare(void)
{
    volatile uint32_t *w;
    unsigned int i;
    if (__esp32p4_psram_size < 32u * 1024u * 1024u) return 0;
    krnP4CacheSyncData(&e2, sizeof e2);
    w = (volatile uint32_t *)control();
    for (i = 0; i < sizeof e2 / 4; ++i) w[i] = 0;
    atom.add = atom.cas = atom.protected_count = 0;
    atom.lock = 1; /* deliberate contention before start-barrier release */
    w = (volatile uint32_t *)E2_SCRATCH;
    for (i = 0; i < 1024; ++i) w[i] = 0xcafef00du;
    krnP4CacheSyncData((void *)E2_SCRATCH, 4096);
    installed = 0;
    fence();
    return 1;
}
static int psram_exchanges(uint32_t epoch)
{
    volatile struct p4_mailbox *m = (volatile struct p4_mailbox *)alias(&__p4_secondary_mailbox);
    volatile uint32_t *p = (volatile uint32_t *)E2_SCRATCH;
    uint32_t seq, i, ticket = m->request[0], start, left, old_hash, n;
    for (seq = 66; seq <= 130; ++seq) {
        for (i = 0; i < 256; ++i) p[16+i] = seq * 0x1020304u ^ i;
        /* Alternate shared-L1D handoff and forced physical PSRAM reload. */
        if (seq & 1) krnP4CacheSyncData((void *)(E2_SCRATCH + 64), 1024);
        m->request[1] = epoch; m->request[2] = seq;
        m->request[3] = 256; m->request[4] = p4_mail_hash(p+16, 256);
        m->request[5] = 1;
        fence(); m->request[0] = ++ticket; fence();
        if (!wait_word(&m->response[0], ticket) || m->response[1] != P4_MAIL_OK ||
            m->response[2] != seq || m->response[4] != seq) {
            krnP4PutStr("[smp-e2] PSRAM response seq="); krnP4PutHex32(seq);
            krnP4PutStr(" status="); krnP4PutHex32(m->response[1]);
            krnP4PutStr(" accepted="); krnP4PutHex32(m->response[4]);
            krnP4PutStr("\n"); return 0;
        }
        if (seq & 1) krnP4CacheSyncData((void *)(E2_SCRATCH + 1088), 1024);
        for (i = 0; i < 256; ++i)
            if (p[272+i] != (p[16+i] ^ (0xa5a50000u | seq))) {
                krnP4PutStr("[smp-e2] PSRAM payload seq="); krnP4PutHex32(seq);
                krnP4PutStr(" word="); krnP4PutHex32(i);
                krnP4PutStr(" got="); krnP4PutHex32(p[272+i]);
                krnP4PutStr(" want="); krnP4PutHex32(p[16+i] ^ (0xa5a50000u | seq));
                krnP4PutStr("\n"); return 0;
            }
        if (m->response[3] != p4_mail_hash(p+272, 256)) return 0;
    }
    /* Missing publication: changed data/sequence without a fresh ticket
       must neither generate an ack nor overwrite the previous output. */
    old_hash = p4_mail_hash(p+272, 256);
    p[16] ^= 1; m->request[2] = 131;
    m->request[4] = p4_mail_hash(p+16, 256);
    start = tick(); left = 100000;
    do {
        if (m->response[0] != ticket || m->response[2] != 130 ||
            p4_mail_hash(p+272, 256) != old_hash) return 0;
    } while (--left && (uint32_t)(tick() - start) < 3600000u);
    /* Refusal must preserve output and ownership, not merely report an
       error. No array indexing is permitted before length validation. */
    for (n = 0; n < 6; ++n) {
        uint32_t expected;
        m->request[1] = epoch + (n == 0);
        m->request[2] = n == 1 ? 130 : n == 2 ? 129 : n == 3 ? 132 : 131;
        m->request[3] = n == 4 ? UINT32_MAX : 256;
        m->request[4] = p4_mail_hash(p+16, 256) ^ (n == 5);
        expected = n == 0 ? P4_MAIL_EPOCH : n < 4 ? P4_MAIL_SEQUENCE :
                   n == 4 ? P4_MAIL_LENGTH : P4_MAIL_CHECKSUM;
        fence(); m->request[0] = ++ticket; fence();
        if (!wait_word(&m->response[0], ticket) || m->response[1] != expected ||
            m->response[4] != 130 || m->response[3] != 0 ||
            p4_mail_hash(p+272, 256) != old_hash) return 0;
    }
    m->request[1] = epoch; m->request[2] = 131; m->request[3] = 256;
    m->request[4] = p4_mail_hash(p+16, 256);
    fence(); m->request[0] = ++ticket; fence();
    if (!wait_word(&m->response[0], ticket) || m->response[1] != P4_MAIL_OK ||
        m->response[4] != 131) return 0;
    krnP4CacheSyncData((void *)E2_SCRATCH, 4096);
    for (i = 0; i < 16; ++i) if (p[i] != 0xcafef00du) return 0;
    for (i = 528; i < 1024; ++i) if (p[i] != 0xcafef00du) return 0;
    for (i = 0; i < 256; ++i)
        if (p[272+i] != (p[16+i] ^ (0xa5a50000u | 131u))) return 0;
    return m->response[3] == p4_mail_hash(p+272, 256);
}
static void install_primary(void)
{
    unsigned int i, h, s;
    __asm__ volatile("csrr %0,mstatus\ncsrr %1,mie\ncsrr %2,mtvec\ncsrci mstatus,8"
        : "=r"(saved_mstatus), "=r"(saved_mie), "=r"(saved_mtvec) :: "memory");
    saved_cfg = p4_r32(P4_CLIC_BASE); saved_threshold = p4_r32(P4_CLIC_BASE+8);
    for (i = 0; i < 48; ++i) {
        saved_lines[i] = p4_r32(P4_CLIC_CTRL(i));
        p4_w32(P4_CLIC_CTRL(i), 0);
    }
    /* hw_ver1 defines exactly 128 contiguous map words before status+200.
       Preserve every map and make line22 exclusive on BOTH harts. */
    for (h = 0; h < 2; ++h) for (s = 0; s < 128; ++s) {
        uint32_t v = p4_r32(E2_ROUTE(h, s));
        saved_routes[h][s] = v;
        if (s == E2_SOURCE(h)) v = E2_LINE;
        else if ((v & 63u) == E2_LINE || s == E2_SOURCE(1-h)) v = 0;
        p4_w32(E2_ROUTE(h, s), v);
    }
    p4_w32(E2_FROM(0), 0); p4_w32(E2_FROM(1), 0);
    p4_w32(P4_CLIC_BASE, (saved_cfg & ~0x1eu) | 6u);
    p4_w32(P4_CLIC_BASE+8, 0);
    p4_w32(P4_CLIC_CTRL(E2_LINE), E2_CTL);
    __asm__ volatile("csrw mtvec,%0\ncsrsi mstatus,8" :: "r"(__p4_e2_irq) : "memory");
    installed = 1;
}
/* The complete cache-off window, including return from cache functions,
   resides in SRAM and uses the primary's internal boot stack. */
SRAM static int cache_window(void)
{
    volatile struct e2_control *c = control();
    unsigned long token;
    uint32_t status, sp;
    __asm__ volatile("csrr %0,mstatus\nmv %1,sp\ncsrci mstatus,8"
        : "=r"(status), "=r"(sp) :: "memory");
    if (sp < 0x4ff00000u || sp >= 0x4ffc0000u || c->worker[5] != CMD_PARK ||
        c->worker[10] < (uintptr_t)__p4_secondary_stack ||
        c->worker[10] >= (uintptr_t)__p4_secondary_stack_end) {
        __asm__ volatile("csrw mstatus,%0" :: "r"(status) : "memory");
        return 0;
    }
    ++c->command[7]; /* observable admission counter, not a cache-off claim */
    token = krnP4CacheOff();
    fence();
    krnP4CacheOn(token);
    fence(); c->command[3] = CMD_PARK; fence();
    __asm__ volatile("csrw mstatus,%0" :: "r"(status) : "memory");
    return 1;
}
int krnP4E2Run(unsigned int epoch)
{
    volatile struct e2_control *c = control();
    uint32_t misa, count;
    int ok = 0;
    if (!psram_exchanges(epoch)) { krnP4PutStr("[smp-e2] PSRAM FAIL\n"); return 0; }
    krnP4PutStr("[smp-e2] PSRAM PASS; shared/physical=66 publication-refusal=1 refusals=6 guards=ok\n");
    __asm__ volatile("csrr %0,misa" : "=r"(misa));
    krnP4PutStr("[smp-e2] misa0="); krnP4PutHex32(misa);
    krnP4PutStr(" misa1="); krnP4PutHex32(c->worker[4]); krnP4PutStr("\n");
    if (!(misa & c->worker[4] & 1u)) return 0;
    c->command[0] = CMD_ATOMIC; fence();
    if (!wait_word(&c->worker[0], CMD_ATOMIC) || c->worker[3] != 1) return 0;
    __atomic_store_n(&atom.lock, 0, __ATOMIC_RELEASE);
    c->command[2] = CMD_ATOMIC; fence(); c->command[1] = CMD_ATOMIC; fence();
    if (!atomic_work(0) || !wait_word(&c->worker[1], CMD_ATOMIC) ||
        c->worker[2] != 1 || atom.add != 2*E2_N || atom.cas != 2*E2_N ||
        atom.protected_count != 2*E2_N || atom.lock ||
        !(c->worker[9] | c->command[6])) {
        krnP4PutStr("[smp-e2] atomic detail done="); krnP4PutHex32(c->worker[1]);
        krnP4PutStr(" result="); krnP4PutHex32(c->worker[2]);
        krnP4PutStr(" add="); krnP4PutHex32(atom.add);
        krnP4PutStr(" cas="); krnP4PutHex32(atom.cas);
        krnP4PutStr(" protected="); krnP4PutHex32(atom.protected_count);
        krnP4PutStr(" retries0="); krnP4PutHex32(c->command[6]);
        krnP4PutStr(" retries1="); krnP4PutHex32(c->worker[9]);
        krnP4PutStr("\n"); return 0;
    }
    krnP4PutStr("[smp-e2] concurrent retries0="); krnP4PutHex32(c->command[6]);
    krnP4PutStr(" retries1="); krnP4PutHex32(c->worker[9]); krnP4PutStr("\n");
    krnP4PutStr("[smp-e2] ATOMICS PASS; AMO/LRSC/lock=16384 each forced-contention=1\n");
    install_primary();
    c->command[0] = CMD_ARM; fence();
    if (!wait_word(&c->worker[1], CMD_ARM) || c->worker[2] != 1) goto out;
    /* Wrong/stale destination and absent trigger cannot grant completion. */
    c->irq_request[1][0] = 1; c->irq_request[1][1] = 0; fence();
    p4_w32(E2_FROM(1), 1);
    if (!wait_word(&c->irq_ack[1][1], 1) || c->irq_ack[1][0]) goto out;
    c->irq_request[1][1] = 1; c->irq_request[1][0] = 2; fence();
    if (wait_word(&c->irq_ack[1][0], 2)) goto out;
    p4_w32(E2_FROM(1), 1);
    if (!wait_word(&c->irq_ack[1][0], 2) || c->irq_ack[1][2] != 1) goto out;
    /* Hart1 triggers the reciprocal IPI through a one-shot worker command.
       That command is IRQ-only: CPU0 must receive a real interrupt. */
    c->irq_request[0][0] = 3; c->irq_request[0][1] = 0; fence();
    /* Hart1 IRQ sends reply when this lane has a fresh pending generation. */
    c->irq_request[1][0] = 3; fence(); p4_w32(E2_FROM(1), 1);
    if (!wait_word(&c->irq_ack[0][0], 3) || c->irq_ack[0][2] != 0) goto out;
    /* Write executable SRAM through D-cache, require a remote fence ack
       BEFORE granting the worker permission to fetch the new code. */
    test_code[0] = 0x02500513u; /* addi a0,zero,37 */
    test_code[1] = 0x00008067u;
    krnP4SyncCode(test_code, sizeof test_code);
    count = c->irq_ack[1][4]; c->irq_request[1][0] = 4; fence();
    p4_w32(E2_FROM(1), 1);
    if (!wait_word(&c->irq_ack[1][0], 4) || c->irq_ack[1][4] != count+1) goto out;
    c->command[0] = CMD_EXEC; fence();
    if (!wait_word(&c->worker[1], CMD_EXEC) || c->worker[2] != 37) goto out;
    test_code[0] = 0x03500513u; /* replace already fetched code: return 53 */
    krnP4SyncCode(test_code, sizeof test_code);
    count = c->irq_ack[1][4]; c->irq_request[1][0] = 5; fence();
    /* A stale ack for generation 4 cannot authorize execution generation 5. */
    if (wait_word(&c->irq_ack[1][0], 5)) goto out;
    p4_w32(E2_FROM(1), 1);
    if (!wait_word(&c->irq_ack[1][0], 5) || c->irq_ack[1][4] != count+1) goto out;
    c->command[0] = CMD_EXEC_AGAIN; fence();
    if (!wait_word(&c->worker[1], CMD_EXEC_AGAIN) || c->worker[2] != 53) goto out;
    krnP4PutStr("[smp-e2] IPI/FENCE PASS; harts=0,1 wrong-destination/absent-trigger/stale-ack refused remote-code=37,53\n");
    /* Negative rendezvous: no PARK request -> no cache-off call. */
    if (wait_word(&c->worker[5], CMD_PARK)) goto out;
    if (cache_window() || c->command[7]) goto out;
    c->command[0] = CMD_PARK; fence();
    if (!wait_word(&c->worker[5], CMD_PARK) || !cache_window() ||
        !wait_word(&c->worker[1], CMD_PARK) || c->worker[6] != CMD_PARK ||
        c->worker[2] != 1 || c->command[7] != 1 ||
        c->worker[11] != c->worker[12]) goto out;
    krnP4CacheSyncData((void *)E2_SCRATCH, 4096);
    if (*(volatile uint32_t *)E2_SCRATCH != 0xcafef00du) goto out;
    krnP4PutStr("[smp-e2] CACHE PASS; missing-park refused SRAM stacks remote resume PSRAM reload=ok\n");
    ok = 1;
out:
    __asm__ volatile("csrci mstatus,8" ::: "memory");
    return ok;
}
void krnP4E2Teardown(void)
{
    unsigned int i, h, s;
    if (!installed) return;
    __asm__ volatile("csrci mstatus,8" ::: "memory");
    /* Caller MUST confirm secondary global reset/clock-off first. A line
       mask is not an in-flight ISR barrier. */
    /* Do NOT access remote CLIC MMIO after clock gating: its local bus can
       no longer answer. Global reset is already the stronger ISR barrier. */
    p4_w32(E2_FROM(0), 0); p4_w32(E2_FROM(1), 0); fence();
    for (h = 0; h < 2; ++h) for (s = 0; s < 128; ++s)
        p4_w32(E2_ROUTE(h, s), saved_routes[h][s]);
    for (i = 0; i < 48; ++i) p4_w32(P4_CLIC_CTRL(i), saved_lines[i]);
    p4_w32(P4_CLIC_BASE, saved_cfg); p4_w32(P4_CLIC_BASE+8, saved_threshold);
    __asm__ volatile("csrw mtvec,%0\ncsrw mie,%1\ncsrw mstatus,%2" ::
        "r"(saved_mtvec), "r"(saved_mie), "r"(saved_mstatus) : "memory");
    installed = 0;
}
