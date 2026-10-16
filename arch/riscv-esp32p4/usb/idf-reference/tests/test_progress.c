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
    puts("PASS compact progress sample: worst-case bound, fields, invalid arguments, slot budget");
}
