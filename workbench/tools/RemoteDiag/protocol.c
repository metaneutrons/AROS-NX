/* Copyright (C) 2026, The AROS Development Team. */
#include "protocol.h"
#include <string.h>
uint32_t rd_u32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
void rd_put32(uint8_t *p,uint32_t x) { for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(x>>(8*i)); }
uint64_t rd_u64(const uint8_t *p) { return rd_u32(p)|(uint64_t)rd_u32(p+4)<<32; }
void rd_put64(uint8_t *p,uint64_t x) { rd_put32(p,(uint32_t)x);rd_put32(p+4,(uint32_t)(x>>32)); }
static uint32_t crc_update(uint32_t c,const uint8_t *p,size_t n) {
    for(size_t i=0;i<n;++i) { c^=p[i];for(unsigned k=0;k<8;++k)c=(c>>1)^((0u-(c&1u))&0xedb88320u); }return c;
}
uint32_t rd_crc(const uint8_t *p,size_t n) { return ~crc_update(~0u,p,n); }
static uint32_t frame_crc(const uint8_t *p,size_t n) { return ~crc_update(crc_update(~0u,p,28),p+RD_HEADER,n); }
static void response(struct rd_link *l,int status,const uint8_t *p,size_t n) {
    memset(l->tx,0,RD_HEADER);memcpy(l->tx,"RDG1",4);l->tx[4]=1;
    l->tx[6]=(uint8_t)l->operation; l->tx[7]=0x80;
    rd_put32(l->tx+8,(uint32_t)status);rd_put32(l->tx+12,(uint32_t)n);
    rd_put32(l->tx+16,l->request_id);rd_put64(l->tx+20,l->session);
    if(n)memcpy(l->tx+RD_HEADER,p,n);
    rd_put32(l->tx+28,frame_crc(l->tx,n));l->tx_size=RD_HEADER+n;l->tx_sent=0;
}
void rd_link_init(struct rd_link *l,rd_submit_fn submit,void *ctx) {
    memset(l,0,sizeof(*l));l->submit=submit;l->context=ctx;l->generation=1;l->rx_need=RD_HEADER;
}
void rd_link_reset(struct rd_link *l) {
    uint32_t next=l->generation+1;if(!next)next=1;
    uint32_t serial=l->serial;
    rd_submit_fn f=l->submit;void *c=l->context;rd_link_init(l,f,c);l->generation=next;l->serial=serial;
}
static void dispatch(struct rd_link *l) {
    uint8_t *h=l->rx,*p=h+RD_HEADER;uint32_t n=rd_u32(h+12),id=rd_u32(h+16);
    unsigned op=h[6];l->operation=op;l->request_id=id;
    if(rd_u32(h+28)!=frame_crc(h,n)) { l->poisoned=true;return; }
    if(op==RD_HELLO) {
        if(id!=1 || rd_u64(h+20)!=0 || n!=8 || !rd_u64(p) || rd_u64(p)==l->session) {response(l,RD_SESSION,NULL,0);return;}
        /* A new explicit host invocation may replace an idle NONEAUTH session
           without resetting USB. Pending jobs/undrained replies block feed. */
        if(l->session && !++l->generation)++l->generation;
        l->session=rd_u64(p);l->sequence=id;
        uint8_t caps[16];rd_put32(caps,1);rd_put32(caps+4,RD_DATA_MAX);
        rd_put32(caps+8,RD_SNAPSHOT_MAX);rd_put32(caps+12,l->generation);
        response(l,RD_OK,caps,sizeof(caps));return;
    }
    if(!l->session || rd_u64(h+20)!=l->session || id<=l->sequence || !id) {response(l,RD_SESSION,NULL,0);return;}
    l->sequence=id;
    if(op<RD_STATUS || op>RD_READ) {response(l,RD_BAD_REQUEST,NULL,0);return;}
    /* No ticket reuse during a process lifetime. Exhaustion requires restart. */
    if(l->serial==UINT32_MAX) {l->poisoned=true;return;}
    l->pending=true;l->ticket=++l->serial;
    if(!l->submit || !l->submit(l->context,l->ticket,op,p,n)) {
        l->pending=false;response(l,RD_BUSY,NULL,0);
    }
}
size_t rd_link_feed(struct rd_link *l,const uint8_t *data,size_t n) {
    if(!data || l->pending || l->tx_size || l->poisoned)return 0;
    size_t used=0;
    while(used<n && !l->pending && !l->tx_size && !l->poisoned) {
        size_t count=l->rx_need-l->rx_used;if(count>n-used)count=n-used;
        memcpy(l->rx+l->rx_used,data+used,count);l->rx_used+=count;used+=count;
        if(l->rx_used!=l->rx_need)continue;
        if(l->rx_need==RD_HEADER) {
            uint8_t *h=l->rx;
            if(memcmp(h,"RDG1",4)||h[4]!=1||h[5]||h[7]||rd_u32(h+8)||rd_u32(h+12)>RD_REQUEST_MAX) {l->poisoned=true;break;}
            l->rx_need=RD_HEADER+rd_u32(h+12);if(l->rx_need>RD_HEADER)continue;
        }
        dispatch(l);l->rx_used=0;l->rx_need=RD_HEADER;
    }return used;
}
size_t rd_link_peek(struct rd_link *l,const uint8_t **p) { *p=l->tx+l->tx_sent;return l->tx_size-l->tx_sent; }
bool rd_link_consume(struct rd_link *l,size_t n) {
    if(n>l->tx_size-l->tx_sent)return false;
    l->tx_sent+=n;
    if(l->tx_sent==l->tx_size)l->tx_sent=l->tx_size=0;
    return true;
}
void rd_link_complete(struct rd_link *l,uint32_t ticket,int status,const uint8_t *p,size_t n) {
    if(!l->pending || ticket!=l->ticket)return;
    if(n>RD_DATA_MAX || (n&&!p) || status<RD_OK || status>RD_END) {status=RD_IO;n=0;}
    l->pending=false;response(l,status,p,n);
}
