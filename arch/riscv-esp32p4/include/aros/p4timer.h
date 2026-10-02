#ifndef AROS_P4TIMER_H
#define AROS_P4TIMER_H
#include <stdint.h>
/* P4-specific KATTR_PlatformTimer payload. Immutable for kernel lifetime.
 * Matched target ABI required; no mutable timer ownership/deadline fields. */
#define KRN_P4_TIMER_OPS_VERSION 1u
struct KrnP4TimerOps
{
    uint32_t version;
    uint32_t size;
    uint32_t frequency;
    uint64_t (*read)(void);
};
#endif
