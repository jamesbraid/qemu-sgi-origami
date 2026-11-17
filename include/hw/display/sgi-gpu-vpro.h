/*
 * VPro (Odyssey) drawing and scan-out on the SGI GPU back end.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef HW_DISPLAY_SGI_GPU_VPRO_H
#define HW_DISPLAY_SGI_GPU_VPRO_H

#include "hw/display/sgi-gpu.h"

/*
 * One eligible draw: clears and flat untextured triangles, w = 1, with depth,
 * blend and logic ops off, in window coordinates snapped to 1/256 pixel.
 */
typedef struct SgiGpuVproTri {
    int32_t x[3], y[3];         /* 1/256 pixel */
    uint32_t colour;            /* stored word, after the colour-store rule */
    uint32_t write_mask;
    int32_t clip_x0, clip_y0, clip_x1, clip_y1;   /* inclusive, pixels */
} SgiGpuVproTri;

typedef struct SgiGpuVproClear {
    int32_t x0, y0, x1, y1;     /* inclusive, pixels */
    uint32_t value;
    uint32_t write_mask;
} SgiGpuVproClear;

/* Queue work on a colour plane; returns its fence, 0 on failure. */
uint64_t sgi_gpu_vpro_tris(SgiGpuPlane *colour, const SgiGpuVproTri *tris,
                           size_t count);
uint64_t sgi_gpu_vpro_clear(SgiGpuPlane *plane, const SgiGpuVproClear *clear);

#endif
