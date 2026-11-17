/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MISC_SGI_ODYSSEY_CLOCK_H
#define HW_MISC_SGI_ODYSSEY_CLOCK_H

#include "qemu/host-utils.h"
#include "qemu/timer.h"

/* The window the host tick rate is measured over, in nanoseconds. */
#define SGI_ODYSSEY_CAL_MIN_NS (100LL * SCALE_MS)
#define SGI_ODYSSEY_CAL_MAX_NS (10000LL * SCALE_MS)

/*
 * Host ticks per ms from dticks ticks over dns ns, or 0 when the window is
 * outside the bounds or the rate is not between 1 MHz and 100 GHz.
 */
static inline uint64_t sgi_odyssey_ticks_per_ms(int64_t dticks, int64_t dns)
{
    uint64_t lo, hi;

    if (dticks <= 0 || dns < SGI_ODYSSEY_CAL_MIN_NS ||
        dns > SGI_ODYSSEY_CAL_MAX_NS) {
        return 0;
    }
    mulu64(&lo, &hi, dticks, SCALE_MS);
    divu128(&lo, &hi, dns);
    return hi == 0 && lo >= 1000 && lo <= 100000000 ? lo : 0;
}

#endif
