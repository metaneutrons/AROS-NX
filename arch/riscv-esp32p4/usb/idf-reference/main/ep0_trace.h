/* Read-only diagnostic snapshots. Publish once; never spin in the USB ISR. */
#ifndef HS_EP0_TRACE_H
#define HS_EP0_TRACE_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "trace.h"
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"EP0 publication must be lock-free");
struct hs_ep0_snapshot {
    uint32_t gusb,dcfg,dsts,irq,mask,out,size,epirq,rxpeek,rxfifo,local;
};
/* K low16=DOEPMSK; bits16/17=EP0 OUT pending/mask;
 * bits18/19=EP0 IN pending/mask. Other endpoints are deliberately omitted. */
static inline uint32_t hs_ep0_local(uint32_t doepmsk,uint32_t daint,uint32_t mask)
{
    return (doepmsk & 0xffffu) | (daint & 0x10000u) |
        ((mask & 0x10000u)<<1) | ((daint & 1u)<<18) | ((mask & 1u)<<19);
}
struct hs_ep0_capture {
    atomic_uint state; /* 0 empty, 1 single writer, 2 immutable/published */
    struct hs_ep0_snapshot snapshot;
};
static inline void hs_ep0_init(struct hs_ep0_capture *c)
{ atomic_init(&c->state,0); }
static inline bool hs_ep0_begin(struct hs_ep0_capture *c)
{
    unsigned expected=0;
    return atomic_compare_exchange_strong_explicit(&c->state,&expected,1,
        memory_order_relaxed,memory_order_relaxed);
}
static inline void hs_ep0_publish(struct hs_ep0_capture *c,
                                  const struct hs_ep0_snapshot *s)
{
    c->snapshot=*s;
    atomic_store_explicit(&c->state,2,memory_order_release);
}
static inline bool hs_ep0_read(const struct hs_ep0_capture *c,
                              struct hs_ep0_snapshot *out)
{
    if(atomic_load_explicit(&c->state,memory_order_acquire)!=2)return false;
    *out=c->snapshot;
    return true;
}
/* R=first reset-end, L=late live state, N=no published reset snapshot.
 * Counters are sampled at report time, not at the reset snapshot time. */
static inline int hs_ep0_format(char *text,size_t size,char kind,bool dma,
    bool mounted,uint32_t reset,uint32_t setup,uint32_t xfer,
    const struct hs_ep0_snapshot *s)
{
    if(!text || !s || size<HS_TRACE_SAMPLE_CHUNK+1u ||
       (kind!='R' && kind!='L' && kind!='N'))return -1;
    return snprintf(text,size,
        "EP0 %c dma=%u m=%u r=%08x s=%08x x=%08x\n"
        "G=%08x D=%08x S=%08x I=%08x M=%08x\n"
        "O=%08x Z=%08x E=%08x R=%08x F=%08x K=%08x\n",
        kind,(unsigned)dma,(unsigned)mounted,(unsigned)reset,(unsigned)setup,
        (unsigned)xfer,(unsigned)s->gusb,(unsigned)s->dcfg,(unsigned)s->dsts,
        (unsigned)s->irq,(unsigned)s->mask,(unsigned)s->out,(unsigned)s->size,
        (unsigned)s->epirq,(unsigned)s->rxpeek,(unsigned)s->rxfifo,(unsigned)s->local);
}
#endif
