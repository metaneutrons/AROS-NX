/* Diagnostic only: observe existing PIO values, never read USB registers. */
#ifndef HS_RX_TRACE_H
#define HS_RX_TRACE_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "trace.h"
_Static_assert(ATOMIC_INT_LOCK_FREE==2 && sizeof(unsigned)==4,
               "RX counters require lock-free 32-bit atomics");
enum hs_rx_site { HS_RX_IRQ, HS_RX_POP, HS_RX_EP_SETUP };
struct hs_rx_counters { atomic_uint v[9]; };
extern struct hs_rx_counters hs_rx_state;
static inline void hs_rx_init(struct hs_rx_counters *s)
{ for(unsigned i=0;i<9;i++)atomic_init(&s->v[i],0); }
static inline void hs_rx_record(struct hs_rx_counters *s, unsigned port,
                               enum hs_rx_site site,uint32_t value)
{
    if(port!=1)return;
    if(site==HS_RX_IRQ) {
        atomic_fetch_add_explicit(&s->v[0],1,memory_order_relaxed);
        if(value & (1u<<4))atomic_fetch_add_explicit(&s->v[1],1,memory_order_relaxed);
        atomic_fetch_or_explicit(&s->v[8],value,memory_order_relaxed);
    } else if(site==HS_RX_POP) {
        unsigned kind=(value>>17)&15u;
        atomic_fetch_add_explicit(&s->v[2],1,memory_order_relaxed);
        atomic_store_explicit(&s->v[6],value,memory_order_relaxed);
        atomic_fetch_or_explicit(&s->v[7],1u<<kind,memory_order_relaxed);
        if((value&15u)==0 && (kind==6 || kind==4))
            atomic_fetch_add_explicit(&s->v[kind==6?3:4],1,memory_order_relaxed);
    } else if(site==HS_RX_EP_SETUP && value==0) {
        atomic_fetch_add_explicit(&s->v[5],1,memory_order_relaxed);
    }
}
/* Independent relaxed counter loads; wrapping counts, not a coherent snapshot. */
static inline int hs_rx_format(char *text,size_t size,const struct hs_rx_counters *s)
{
    if(!text || !s || size<HS_TRACE_SAMPLE_CHUNK+1u)return -1;
    unsigned v[9];
    for(unsigned i=0;i<9;i++)v[i]=atomic_load_explicit(&s->v[i],memory_order_relaxed);
    return snprintf(text,size,
        "RX irq=%08x rx=%08x pop=%08x sr=%08x sd=%08x ep=%08x\n"
        "last=%08x kinds=%08x ints=%08x\n",
        v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8]);
}
#endif
