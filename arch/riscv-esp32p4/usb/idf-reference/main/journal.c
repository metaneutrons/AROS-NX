/* Development-only one-shot snapshot, not an updater or security mechanism. */
#include "journal.h"
#include <string.h>

static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put32(uint8_t *p,uint32_t v)
{ for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(i*8)); }
uint32_t hs_journal_crc(const uint8_t *p,size_t n)
{
    uint32_t c=0xffffffffu;
    for(size_t i=0;i<n;++i) {
        c^=p[i];
        for(unsigned b=0;b<8;++b)c=(c>>1)^(0xedb88320u & (0u-(c&1u)));
    }
    return c^0xffffffffu;
}
size_t hs_journal_length(const uint8_t *p,size_t bytes)
{
    if(!p || bytes!=HS_JOURNAL_SECTOR || memcmp(p,"HSJ1",4) || get32(p+4)!=1)return 0;
    uint32_t n=get32(p+8);
    if(!n || n>HS_JOURNAL_TEXT_MAX || get32(p+12)!=hs_journal_crc(p+16,n))return 0;
    for(size_t i=16+n;i<bytes;++i)if(p[i]!=0xff)return 0;
    return n;
}

int hs_journal_save(const struct hs_journal_store *s,const char *text,size_t n,
                    uint8_t *work,size_t bytes)
{
    static const uint8_t expected_mac[6]={0x80,0xf1,0xb2,0xd3,0x3b,0xa6};
    if(!s || !text || !work || !n || n>HS_JOURNAL_TEXT_MAX ||
       bytes!=HS_JOURNAL_SECTOR || !s->read || !s->erase || !s->write ||
       s->address!=0x20000u || s->size!=0x800000u || s->encrypted ||
       memcmp(s->mac,expected_mac,6))return HS_JOURNAL_REFUSED;
    uintptr_t t=(uintptr_t)text,w=(uintptr_t)work;
    if((t>=w && t-w<bytes) || (w>=t && w-t<n))return HS_JOURNAL_REFUSED;
    if(!s->read(s->ctx,HS_JOURNAL_OFFSET,work,bytes))return HS_JOURNAL_READ_ERROR;
    if(hs_journal_length(work,bytes))return HS_JOURNAL_PRESERVED;
    /* Never replace an invalid/partial previous capture or unknown content.
     * CRC is a development preimage/corruption guard, not authentication. */
    if(s->debug_connected || hs_journal_crc(work,bytes)!=s->preimage_crc)
        return HS_JOURNAL_REFUSED;

    uint8_t header[24],segment[8];
    if(!s->read(s->ctx,0,header,sizeof(header)))return HS_JOURNAL_READ_ERROR;
    if(header[0]!=0xe9 || !header[1] || header[1]>16 ||
       header[12]!=18 || header[13]!=0 || header[23]!=1)return HS_JOURNAL_REFUSED;
    uint32_t pos=24;
    for(unsigned i=0;i<header[1];++i) {
        if(pos>HS_JOURNAL_OFFSET-8)return HS_JOURNAL_REFUSED;
        if(!s->read(s->ctx,pos,segment,8))return HS_JOURNAL_READ_ERROR;
        uint32_t length=get32(segment+4);
        if(length>HS_JOURNAL_OFFSET-pos-8)return HS_JOURNAL_REFUSED;
        pos+=8+length;
    }
    /* Bound the complete Espressif image incl. checksum padding and SHA256.
     * Image authenticity/checksum was checked by the bootloader/host; this
     * parser enforces non-overlap, not a signed image verification contract. */
    if(((pos+16u)&~15u)+32u>HS_JOURNAL_OFFSET)return HS_JOURNAL_REFUSED;
    memset(work,0xff,bytes);memcpy(work,"HSJ1",4);
    put32(work+4,1);put32(work+8,(uint32_t)n);
    put32(work+12,hs_journal_crc((const uint8_t *)text,n));
    memcpy(work+16,text,n);
    if(!s->erase(s->ctx,HS_JOURNAL_OFFSET,bytes) ||
       !s->write(s->ctx,HS_JOURNAL_OFFSET,work,bytes))return HS_JOURNAL_WRITE_UNCERTAIN;
    if(!s->read(s->ctx,HS_JOURNAL_OFFSET,work,bytes))return HS_JOURNAL_WRITE_UNCERTAIN;
    if(hs_journal_length(work,bytes)!=n || memcmp(work+16,text,n))
        return HS_JOURNAL_WRITE_UNCERTAIN;
    return HS_JOURNAL_WRITTEN;
}
