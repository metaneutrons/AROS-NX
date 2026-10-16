/* Actual CDC pump with fake source/USB endpoint; no device access. */
#include "cdc_log.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct fixture {
    struct p4_console_ring ring;
    bool connected;
    size_t room,partial,maximum;
    bool bad_write,bad_record;
    unsigned pops,flushes;
    uint8_t output[16384];size_t used;
};
static int pop(void *p,struct p4_console_record *r) {
    struct fixture *f=p;++f->pops;
    if(f->bad_record){r->length=129;return 1;}
    return p4_console_ring_pop(&f->ring,r);
}
static uint32_t drops(void *p){return p4_console_ring_dropped(&((struct fixture *)p)->ring);}
static bool connected(void *p){return ((struct fixture *)p)->connected;}
static size_t room(void *p){return ((struct fixture *)p)->room;}
static size_t write_part(void *p,const uint8_t *bytes,size_t n) {
    struct fixture *f=p;if(n>f->maximum)f->maximum=n;
    if(f->bad_write)return n+1;
    if(n>f->partial)n=f->partial;
    assert(f->used+n<sizeof(f->output));memcpy(f->output+f->used,bytes,n);f->used+=n;return n;
}
static void flush(void *p){++((struct fixture *)p)->flushes;}
static void init(struct fixture *f,struct p4_cdc_log *log) {
    memset(f,0,sizeof(*f));f->connected=true;f->room=512;f->partial=512;
    const struct p4_cdc_source src={f,pop,drops};
    const struct p4_cdc_sink sink={f,connected,room,write_part,flush};
    assert(p4_cdc_log_init(log,&src,&sink));
}
int main(void) {
    struct fixture f;struct p4_cdc_log log;init(&f,&log);
    assert(!p4_cdc_log_init(NULL,NULL,NULL));
    assert(p4_console_ring_push(&f.ring,"one\n",4,0));
    f.connected=false;assert(p4_cdc_log_step(&log,256));assert(f.pops==0&&f.used==0);
    f.connected=true;f.room=0;assert(p4_cdc_log_step(&log,256));assert(f.used==0&&log.length==4);
    f.room=8;f.partial=1;
    assert(p4_cdc_log_step(&log,2));assert(f.used==2&&log.position==2);
    assert(p4_cdc_log_step(&log,2));assert(f.used==4&&!memcmp(f.output,"one\n",4));
    assert(f.maximum<=4);
    init(&f,&log);f.partial=0;
    assert(p4_console_ring_push(&f.ring,"retry",5,1));
    assert(p4_cdc_log_step(&log,256)&&log.position==0&&log.length==5);
    f.partial=512;assert(p4_cdc_log_step(&log,256));assert(f.used==5&&!memcmp(f.output,"retry",5));
    init(&f,&log);
    for(unsigned i=0;i<70;++i)(void)p4_console_ring_push(&f.ring,"x\n",2,(uint8_t)(i&1));
    for(unsigned i=0;i<8;++i)assert(p4_cdc_log_step(&log,256));
    assert(strstr((char *)f.output,"[p4cdc overflow records=00000006]\n"));
    assert(f.used==sizeof("[p4cdc overflow records=00000006]\n")-1+128);
    assert(f.maximum<=128);
    init(&f,&log);p4_cdc_log_reset(&log);assert(p4_cdc_log_step(&log,256));
    assert(strstr((char *)f.output,"stream reset; continuity not guaranteed"));
    init(&f,&log);f.bad_record=true;assert(!p4_cdc_log_step(&log,256));
    init(&f,&log);f.bad_write=true;
    assert(p4_console_ring_push(&f.ring,"bad",3,0));assert(!p4_cdc_log_step(&log,256));
    init(&f,&log);assert(p4_console_ring_push(&f.ring,"budget",6,0));
    assert(p4_cdc_log_step(&log,0)&&f.used==0&&f.pops==0);
    assert(p4_cdc_log_step(&log,UINT32_MAX)&&f.used==6);
    puts("CDC log pump passes: bounds, partial I/O, no reader, overflow and reset (fake USB)");return 0;
}
