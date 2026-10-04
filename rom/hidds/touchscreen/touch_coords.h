#ifndef TOUCHSCREEN_COORDS_H
#define TOUCHSCREEN_COORDS_H

#include <stdint.h>

/* Inclusive measured bounds map to the full logical axis; outliers saturate.
   16-bit bounds keep the rounded product within uint32_t. */
static inline int16_t touch_coordinate(uint32_t raw, uint16_t low,
                                         uint16_t high, uint16_t size,
                                         unsigned int mirror)
{
    uint32_t span, value;

    if (high <= low || !size || size > 32768U)
        return 0;
    if (raw < low) raw = low;
    if (raw > high) raw = high;
    span = (uint32_t)high - low;
    value = ((raw - low) * (size - 1U) + span / 2U) / span;
    return (int16_t)(mirror ? size - 1U - value : value);
}

#endif
