/* Private RUN-stack capability, not a public TagItem or Exec ABI.
 * Only trusted constructor code may create or modify a live frame. Remote
 * retirement/cleanup must treat its recorded address as opaque, never read it.
 */
#ifndef P4_CREATION_CHANNEL_H
#define P4_CREATION_CHANNEL_H
#include <stdint.h>

struct p4_creation_channel
{
    uint32_t value;
    uint32_t generation;
    uintptr_t previous_frame;
    uint32_t previous_generation;
    uintptr_t previous_target;
    uint32_t previous_claimed;
};

#endif
