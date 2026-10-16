#include "ep0_trace.h"
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x))abort(); } while(0)
int main(void)
{
    struct hs_ep0_capture capture;
    struct hs_ep0_snapshot out={0};
    const struct hs_ep0_snapshot all={UINT32_MAX,UINT32_MAX,UINT32_MAX,
        UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    hs_ep0_init(&capture);
    CHECK(!hs_ep0_read(&capture,&out));
    CHECK(hs_ep0_begin(&capture));
    CHECK(!hs_ep0_begin(&capture));
    CHECK(hs_ep0_local(UINT32_MAX,UINT32_MAX,UINT32_MAX)==0x000fffffu);
    CHECK(hs_ep0_local(9,0x10000,0)==0x10009u);
    CHECK(hs_ep0_local(0,0,0x10001)==0xa0000u);
    CHECK(hs_ep0_local(0,0xfffefffe,0xfffefffe)==0);
    CHECK(!hs_ep0_read(&capture,&out));
    hs_ep0_publish(&capture,&all);
    CHECK(hs_ep0_read(&capture,&out));
    CHECK(memcmp(&all,&out,sizeof(all))==0);
    CHECK(!hs_ep0_begin(&capture));
    char text[HS_TRACE_SAMPLE_CHUNK+1u];
    int n=hs_ep0_format(text,sizeof(text),'R',true,true,UINT32_MAX,
                        UINT32_MAX,UINT32_MAX,&out);
    CHECK(n>0 && (unsigned)n<=HS_TRACE_SAMPLE_CHUNK);
    CHECK(strstr(text,"EP0 R dma=1 m=1") && strstr(text,"R=ffffffff F=ffffffff"));
    CHECK(hs_ep0_format(text,sizeof(text),'L',false,false,0,0,0,&out)>0);
    CHECK(hs_ep0_format(text,sizeof(text),'N',false,false,0,0,0,&out)>0);
    CHECK(hs_ep0_format(text,sizeof(text),'X',false,false,0,0,0,&out)==-1);
    CHECK(hs_ep0_format(text,sizeof(text)-1,'R',false,false,0,0,0,&out)==-1);
    CHECK(hs_ep0_format(NULL,sizeof(text),'R',false,false,0,0,0,&out)==-1);
    CHECK(hs_ep0_format(text,sizeof(text),'R',false,false,0,0,0,NULL)==-1);
    /* Six initial + observer + four calls + reset snapshot + late + terminal. */
    CHECK(6u+1u+4u+1u+1u+1u<=HS_TRACE_SLOTS);
    puts("PASS EP0 publish-once, formats, bounds and fourteen-slot budget");
}
