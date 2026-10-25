/* Source-bound portable dispatch tests; no AROS libraries or USB device. */
#include "service.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct rd_screen {int dummy;};struct rd_files {int dummy;};
static int backend_status,clicks,reads;
bool rd_screen_has_mouse(const struct rd_screen *screen) {return screen!=NULL;}
int rd_screen_capture(struct rd_screen *s,uint32_t x,uint32_t y,uint32_t w,uint32_t h,struct rd_snapshot *out) {
    (void)s;(void)x;(void)y;(void)w;(void)h;if(backend_status)return backend_status;
    out->data=malloc(6);assert(out->data);memcpy(out->data,"RGBRGB",6);
    out->size=6;out->width=2;out->height=1;return RD_OK;
}
int rd_screen_mouse(struct rd_screen *s,uint32_t x,uint32_t y,unsigned button) {
    (void)s;(void)x;(void)y;(void)button;++clicks;return backend_status;
}
int rd_files_stat(struct rd_files *f,const char *p,struct rd_metadata *m) {
    (void)f;(void)p;if(backend_status)return backend_status;
    m->protection=0xfacebeefu;m->type=-3;m->days=-20;m->size=6;
    strcpy(m->name,"native");strcpy(m->comment,"comment");return RD_OK;
}
int rd_files_list(struct rd_files *f,const char *p,uint32_t i,struct rd_metadata *m) {
    if(i>0)return RD_END;return rd_files_stat(f,p,m);
}
int rd_files_snapshot(struct rd_files *f,const char *p,uint32_t o,uint32_t n,struct rd_snapshot *out) {
    (void)f;(void)p;(void)o;++reads;if(backend_status)return backend_status;
    if(n>16)return RD_LIMIT;out->data=calloc(n?n:1,1);assert(out->data);out->size=n;return RD_OK;
}
int main(void) {
    struct rd_screen screen;struct rd_files files;
    struct rd_service s={.screen=&screen,.files=&files};
    uint8_t in[1024]={0},out[RD_DATA_MAX];size_t n;
    const char *bad[]={"/","/a","a/","a//b",".","..","a/../b","SYS:x","a:b","a\\b","a\n"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);++i)assert(!rd_valid_relative(bad[i]));
    assert(rd_valid_relative("")&&rd_valid_relative("prefs/net/file")&&rd_valid_relative("x..y"));
    char large[RD_PATH_MAX+1];memset(large,'a',sizeof(large));large[sizeof(large)-1]=0;assert(!rd_valid_relative(large));
    assert(rd_service_execute(&s,RD_STATUS,NULL,0,out,&n)==RD_OK&&n==8&&rd_u32(out)==5);
    assert(rd_service_execute(&s,RD_CLICK,in,12,out,&n)==RD_DENIED&&clicks==0);
    s.mouse=true;assert(rd_service_execute(&s,RD_CLICK,in,12,out,&n)==RD_BAD_REQUEST&&clicks==0);
    rd_put32(in+8,1);assert(rd_service_execute(&s,RD_CLICK,in,12,out,&n)==RD_OK&&clicks==1);
    memset(in,0,sizeof(in));assert(rd_service_execute(&s,RD_CAPTURE,in,16,out,&n)==RD_OK&&n==20);
    uint32_t id=rd_u32(out);assert(id&&rd_u32(out+4)==6&&rd_u32(out+16)==rd_crc((const uint8_t *)"RGBRGB",6));
    rd_put32(in,id);rd_put32(in+4,1);rd_put32(in+8,4);
    assert(rd_service_execute(&s,RD_CHUNK,in,12,out,&n)==RD_OK&&n==4&&!memcmp(out,"GBRG",4));
    rd_put32(in+4,UINT32_MAX);assert(rd_service_execute(&s,RD_CHUNK,in,12,out,&n)==RD_LIMIT);
    rd_put32(in,id+1);assert(rd_service_execute(&s,RD_RELEASE,in,4,out,&n)==RD_CHANGED);
    backend_status=RD_IO;memset(in,0,16);
    assert(rd_service_execute(&s,RD_CAPTURE,in,16,out,&n)==RD_IO&&s.snapshot_id==id&&s.snapshot.data);
    backend_status=0;assert(rd_service_execute(&s,RD_STAT,(const uint8_t *)"file",5,out,&n)==RD_OK&&n==212);
    assert(rd_u32(out)==0xfacebeefu&&(int32_t)rd_u32(out+8)==-3&&(int32_t)rd_u32(out+12)==-20);
    assert(!strcmp((char *)out+132,"comment"));
    assert(rd_service_execute(&s,RD_STAT,(const uint8_t *)"file",4,out,&n)==RD_BAD_REQUEST);
    assert(rd_service_execute(&s,RD_STAT,(const uint8_t *)"a\0b\0",4,out,&n)==RD_BAD_REQUEST);
    memset(in,0,sizeof(in));memcpy(in+8,"SYS:file",9);
    assert(rd_service_execute(&s,RD_READ,in,17,out,&n)==RD_BAD_REQUEST&&reads==0);
    memset(in,0,sizeof(in));memcpy(in+8,"file",5);
    assert(rd_service_execute(&s,RD_READ,in,13,out,&n)==RD_OK&&reads==1&&rd_u32(out+4)==0&&rd_u32(out+16)==0);
    assert(rd_u32(out)!=id);rd_put32(in,rd_u32(out));
    assert(rd_service_execute(&s,RD_RELEASE,in,4,out,&n)==RD_OK&&!s.snapshot.data);
    s.files=NULL;assert(rd_service_execute(&s,RD_STAT,(const uint8_t *)"file",5,out,&n)==RD_DENIED);
    assert(rd_service_execute(&s,100,NULL,0,out,&n)==RD_BAD_REQUEST);
    rd_service_clear(&s);puts("RemoteDiag dispatch/path tests pass (fake backends)");return 0;
}
