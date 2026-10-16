#include "progress.h"
#include "trace.h"
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);abort(); } } while(0)
int main(void)
{
    char text[HS_PROGRESS_SAMPLE_MAX+1u];
    const struct hs_progress_sample maximum={UINT32_MAX,UINT32_MAX,UINT32_MAX,
        UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    int n=hs_progress_format(text,sizeof(text),&maximum);
    CHECK(n>0 && (unsigned)n<=HS_PROGRESS_SAMPLE_MAX);
    CHECK(strstr(text,"sample=6") && strstr(text,"reset=4294967295"));
    CHECK(strstr(text,"DCTL=ffffffff"));
    CHECK(hs_progress_format(text,sizeof(text)-1,&maximum)==-1);
    CHECK(hs_progress_format(NULL,sizeof(text),&maximum)==-1);
    CHECK(hs_progress_format(text,sizeof(text),NULL)==-1);
    /* Initial9 + observer entry1 + four call checkpoints + sample1 + end1. */
    CHECK(9u+1u+4u+1u+1u==HS_TRACE_SLOTS);
    char state[HS_TRACE_TEXT_MAX+1u];
    const struct hs_attach_state raw={2,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    n=hs_attach_format(state,sizeof(state),"123456789012345678901234",INT64_MIN,
                       UINT32_MAX,UINT32_MAX,&raw);
    CHECK(n>0 && (unsigned)n<=HS_TRACE_TEXT_MAX);
    CHECK(strstr(state,"dctl=00000002") && strstr(state,"wrap=ffffffff"));
    CHECK(hs_attach_format(state,sizeof(state),"1234567890123456789012345",0,0,0,&raw)==-1);
    CHECK(hs_attach_format(state,sizeof(state)-1,"x",0,0,0,&raw)==-1);
    CHECK(hs_attach_format(state,sizeof(state),NULL,0,0,0,&raw)==-1);
    CHECK(hs_attach_format(state,sizeof(state),"x",0,0,0,NULL)==-1);
    CHECK(hs_attach_format(NULL,sizeof(state),"x",0,0,0,&raw)==-1);
    puts("PASS progress/attach format: worst-case bound, fields, invalid arguments, slot budget");
}
