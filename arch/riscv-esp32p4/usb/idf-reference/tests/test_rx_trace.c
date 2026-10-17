#include "rx_trace.h"
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x))abort(); } while(0)
int main(void)
{
    struct hs_rx_counters s;
    hs_rx_init(&s);
    hs_rx_record(&s,0,HS_RX_IRQ,UINT32_MAX);
    CHECK(atomic_load(&s.v[0])==0);
    hs_rx_record(&s,1,HS_RX_IRQ,0x80010);
    hs_rx_record(&s,1,HS_RX_IRQ,0x1000);
    hs_rx_record(&s,1,HS_RX_POP,6u<<17);
    hs_rx_record(&s,1,HS_RX_POP,4u<<17);
    hs_rx_record(&s,1,HS_RX_POP,(6u<<17)|1u);
    hs_rx_record(&s,1,HS_RX_EP_SETUP,0);
    hs_rx_record(&s,1,HS_RX_EP_SETUP,1);
    const unsigned expected[9]={2,1,3,1,1,1,(6u<<17)|1u,(1u<<6)|(1u<<4),0x81010};
    for(unsigned i=0;i<9;i++)CHECK(atomic_load(&s.v[i])==expected[i]);
    for(unsigned i=0;i<9;i++)atomic_store(&s.v[i],UINT32_MAX);
    char text[HS_TRACE_SAMPLE_CHUNK+1u];
    int n=hs_rx_format(text,sizeof(text),&s,UINT32_MAX,UINT32_MAX,UINT32_MAX);
    CHECK(n>0 && (unsigned)n<=HS_TRACE_SAMPLE_CHUNK);
    CHECK(strstr(text,"sr=ffffffff sd=ffffffff ep=ffffffff"));
    CHECK(strstr(text,"clk=ffffffff div=ffffffff ref=ffffffff"));
    CHECK(hs_rx_format(text,sizeof(text)-1,&s,0,0,0)==-1);
    CHECK(hs_rx_format(NULL,sizeof(text),&s,0,0,0)==-1);
    CHECK(hs_rx_format(text,sizeof(text),NULL,0,0,0)==-1);
    hs_rx_record(&s,1,HS_RX_IRQ,0);
    CHECK(atomic_load(&s.v[0])==0); /* Defined modulo32 wrap. */
    CHECK(6u+1u+4u+2u+1u+1u<=HS_TRACE_SLOTS);
    puts("PASS RX counters, port/EP filtering, wrapping, format and fifteen-slot budget");
}
