/* Real protocol+dispatch over host pipes with fake backends, not native USB. */
#include "service.h"
#include <stdio.h>
#include <stdlib.h>
struct rd_screen {int dummy;};struct rd_files {int dummy;};
static struct rd_service service;
static struct rd_link link_state;
static bool submit(void *ctx,uint32_t ticket,unsigned op,const uint8_t *p,size_t n) {
    (void)ctx;uint8_t out[RD_DATA_MAX];size_t size;
    int status=rd_service_execute(&service,op,p,n,out,&size);
    rd_link_complete(&link_state,ticket,status,out,size);return true;
}
int main(void) {
    struct rd_screen screen;struct rd_files files;
    service.screen=&screen;service.files=&files;service.mouse=true;
    rd_link_init(&link_state,submit,NULL);
    for(;;) {
        int c=getchar();if(c==EOF)break;uint8_t byte=(uint8_t)c;
        if(rd_link_feed(&link_state,&byte,1)!=1)break;
        const uint8_t *data;size_t size=rd_link_peek(&link_state,&data);
        if(size) {if(fwrite(data,1,size,stdout)!=size||fflush(stdout))break;rd_link_consume(&link_state,size);}
    }
    rd_service_clear(&service);return 0;
}
