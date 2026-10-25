#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "flash_ab_layout.h"

static unsigned checks;
static void le32(unsigned char *p, uint32_t n)
{ p[0]=n; p[1]=n>>8; p[2]=n>>16; p[3]=n>>24; }
static void entry(unsigned char *p, unsigned type, unsigned sub,
    const char *name, uint32_t off, uint32_t size)
{
    memset(p, 0, 32); p[0]=0xaa; p[1]=0x50; p[2]=type; p[3]=sub;
    le32(p+4,off); le32(p+8,size); memcpy(p+12,name,strlen(name));
}
static void fixture(unsigned char *t)
{
    memset(t,255,0xc00);
    entry(t,1,2,"nvs",0x21000,0x6000);
    entry(t+32,1,4,"nvs_key",0x27000,0x1000);
    entry(t+64,1,0,"otadata",0x28000,0x2000);
    entry(t+96,1,1,"phy_init",0x2a000,0x1000);
    entry(t+128,0,0x10,"KERNEL_A",0x40000,0x80000);
    entry(t+160,0,0x11,"KERNEL_B",0xc0000,0x80000);
    entry(t+192,0x40,0,"BSP_A",0x140000,0x600000);
    entry(t+224,0x40,1,"BSP_B",0x740000,0x600000);
    entry(t+256,0x41,0,"DEVELOP",0xd40000,0x2c0000);
}
static int resolve(const unsigned char *t, uint32_t chip, uint32_t running,
    struct P4ABLayout *out)
{ return p4_ab_layout_resolve(t,0xc00,0x20000,chip,0x1000000,running,
    0x80000,0x600000,0x2c0000,out); }
static void rejected(const unsigned char *t, int code)
{
    struct P4ABLayout out, zero;
    memset(&out,0xff,sizeof(out)); memset(&zero,0,sizeof(zero));
    assert(resolve(t,0x1000000,0x40000,&out)==code); ++checks;
    assert(!memcmp(&out,&zero,sizeof(out))); ++checks;
}
int main(void)
{
    unsigned char t[0xc00], temp[32];
    struct P4ABLayout out;
    unsigned i;
    for(i=0;i<2;i++)
    {
        fixture(t);
        assert(resolve(t,i?0x2000000:0x1000000,0x40000,&out)==1); ++checks;
        assert(out.active_slot==0 && out.bsp[0].offset==0x140000); ++checks;
        assert(resolve(t,i?0x2000000:0x1000000,0xc0000,&out)==1); ++checks;
        assert(out.active_slot==1 && out.bsp[1].offset==0x740000); ++checks;
        assert(out.develop.offset==0xd40000 && out.develop.size==0x2c0000); ++checks;
    }
    /* A change in listing order cannot change the selected BSP. */
    memcpy(temp,t+192,32); memcpy(t+192,t+224,32); memcpy(t+224,temp,32);
    assert(resolve(t,0x1000000,0x40000,&out)==1); ++checks;
    assert(out.active_slot==0 && out.bsp[0].offset==0x140000); ++checks;
    assert(resolve(t,0x1000000,0xc0000,&out)==1); ++checks;
    assert(out.active_slot==1 && out.bsp[1].offset==0x740000); ++checks;
    fixture(t); assert(resolve(t,0x1000000,0x20000,&out)==P4_AB_LAYOUT_WRONG_SLOT); ++checks;
    fixture(t); t[0]=0; rejected(t,P4_AB_LAYOUT_BAD_TABLE);
    fixture(t); memset(t+256,255,32); rejected(t,P4_AB_LAYOUT_MISSING);
    fixture(t); memcpy(t+288,t+192,32); rejected(t,P4_AB_LAYOUT_AMBIGUOUS);
    fixture(t); le32(t+192+4,0xc0000); rejected(t,P4_AB_LAYOUT_BAD_RANGE);
    fixture(t); le32(t+192+8,0); rejected(t,P4_AB_LAYOUT_BAD_RANGE);
    fixture(t); le32(t+192+4,0xfffff000); rejected(t,P4_AB_LAYOUT_BAD_RANGE);
    fixture(t); le32(t+192+8,0xfffff000); rejected(t,P4_AB_LAYOUT_BAD_RANGE);
    fixture(t); t[192+3]=1; rejected(t,P4_AB_LAYOUT_AMBIGUOUS);
    fixture(t); t[160+3]=0x10; rejected(t,P4_AB_LAYOUT_AMBIGUOUS);
    fixture(t); le32(t+128+4,0x41000); rejected(t,P4_AB_LAYOUT_BAD_RANGE);
    fixture(t); le32(t+192+8,0x601000); rejected(t,P4_AB_LAYOUT_BAD_RANGE);
    fixture(t); le32(t+256+8,0x2bf000); rejected(t,P4_AB_LAYOUT_BAD_RANGE);
    fixture(t); le32(t+192+28,1); rejected(t,P4_AB_LAYOUT_BAD_TABLE);
    fixture(t); t[192+12+6]=1; rejected(t,P4_AB_LAYOUT_MISSING);
    fixture(t); entry(t+288,0,0x12,"OTHER",0x30000,0x10000);
    rejected(t,P4_AB_LAYOUT_BAD_RANGE);
    fixture(t); entry(t+288,0x40,0,"ALIAS",0x140000,0x1000);
    rejected(t,P4_AB_LAYOUT_BAD_RANGE);
    fixture(t); le32(t+4,0x20000); rejected(t,P4_AB_LAYOUT_BAD_RANGE);
    fixture(t); assert(p4_ab_layout_resolve(t,31,0x20000,0x1000000,
        0x1000000,0x40000,0x80000,0x600000,0x2c0000,&out)==P4_AB_LAYOUT_BAD_TABLE); ++checks;
    /* D1001 may retain an unmapped high storage partition; boot payloads may not. */
    fixture(t); entry(t+288,1,0x81,"storage",0x1020000,0xfe0000);
    assert(resolve(t,0x2000000,0xc0000,&out)==1); ++checks;
    assert(resolve(t,0x1000000,0xc0000,&out)==P4_AB_LAYOUT_BAD_RANGE); ++checks;
    printf("PASS: %u actual resolver checks\n",checks);
    return 0;
}
