#include "link_reconnect.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);abort(); } } while(0)

struct fixture { unsigned calls;bool detach_ok,attach_ok; };
static bool disconnect(void *ctx)
{ struct fixture *f=ctx;CHECK(f->calls++==0);return f->detach_ok; }
static void note(void *ctx,bool attached,bool ok)
{
    struct fixture *f=ctx;
    CHECK(f->calls++==(attached?4u:1u));
    CHECK(ok==(attached?f->attach_ok:f->detach_ok));
}
static void wait_ms(void *ctx,unsigned ms)
{ struct fixture *f=ctx;CHECK(f->calls++==2);CHECK(ms==1000); }
static bool connect(void *ctx)
{ struct fixture *f=ctx;CHECK(f->calls++==3);return f->attach_ok; }
int main(void)
{
    for(unsigned errors=0;errors<4;++errors) {
        struct fixture f={0,(errors&1u)==0,(errors&2u)==0};
        struct hs_reconnect_ops ops={&f,disconnect,connect,wait_ms,note};
        bool attempted=false;
        CHECK(hs_reconnect_once(&attempted,&ops));
        CHECK(attempted && f.calls==5);
        CHECK(!hs_reconnect_once(&attempted,&ops));
        CHECK(f.calls==5);
    }
    bool attempted=false;
    struct hs_reconnect_ops empty={0};
    CHECK(!hs_reconnect_once(NULL,&empty));
    CHECK(!hs_reconnect_once(&attempted,NULL));
    CHECK(!hs_reconnect_once(&attempted,&empty));
    CHECK(!attempted);
    puts("PASS reconnect: four outcome pairs, order, duration, once-only and invalid inputs");
}
