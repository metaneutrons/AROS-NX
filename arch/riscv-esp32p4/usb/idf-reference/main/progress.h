/* One-slot sample keeps progress-mode journal bounded to sixteen records. */
#ifndef HS_REFERENCE_PROGRESS_H
#define HS_REFERENCE_PROGRESS_H
#include <stdint.h>
#include <stdio.h>
#include "trace.h"
#define HS_PROGRESS_SAMPLE_MAX HS_TRACE_SAMPLE_CHUNK
struct hs_progress_sample {
    unsigned mounted;
    uint32_t dcfg,dctl,dsts,usbctl,reset,setup,xfer;
};
static inline int hs_progress_format(char *text,size_t size,
                                    const struct hs_progress_sample *s)
{
    if(!text || !s || size<HS_PROGRESS_SAMPLE_MAX+1u)return -1;
    return snprintf(text,size,
        "[HSReference] sample=6 mounted=%u max=HS\n"
        "DCFG=%08x DCTL=%08x DSTS=%08x USBCTRL=%08x\n"
        "reset=%u setup=%u xfer=%u\n",
        s->mounted,(unsigned)s->dcfg,(unsigned)s->dctl,(unsigned)s->dsts,
        (unsigned)s->usbctl,(unsigned)s->reset,(unsigned)s->setup,(unsigned)s->xfer);
}
#endif
