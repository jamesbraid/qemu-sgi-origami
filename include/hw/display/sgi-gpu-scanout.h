/*
 * VPro scan-out compose on the SGI GPU back end: C interface to
 * rust/sgi-gpu/src/scanout.rs.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef HW_DISPLAY_SGI_GPU_SCANOUT_H
#define HW_DISPLAY_SGI_GPU_SCANOUT_H

#include "hw/display/sgi-gpu.h"

typedef struct SgiGpuVproScanout SgiGpuVproScanout;

/* The board state odyssey_resolve_row() reads besides the planes. */
typedef struct SgiGpuVproTables {
    uint32_t width, height;     /* visible pixels and lines */
    uint32_t displayed_buffer;
    uint32_t lut[0x2000];
    uint32_t dispmode[32];
    uint32_t overlay_mode[8];
    uint32_t cursor[0x100];
} SgiGpuVproTables;

/* For colour and id planes of width x height pixels; NULL on failure. */
SgiGpuVproScanout *sgi_gpu_vpro_scanout_new(SgiGpu *gpu, uint32_t width,
                                            uint32_t height);
/* Waits for every queued compose. */
void sgi_gpu_vpro_scanout_free(SgiGpuVproScanout *sc);
/* Returns when every compose queued so far has called its done. */
void sgi_gpu_vpro_scanout_quiesce(SgiGpuVproScanout *sc);

/*
 * Compose the two colour planes into frame on a worker thread, waiting for
 * the GPU before done(opaque, ok) runs there. The tables and the id rows
 * (row_count rows of the plane width from row_y, did and overlay pointing
 * at the first) are copied before this returns; the planes and the frame
 * must stay until done. False: nothing was queued.
 */
bool sgi_gpu_vpro_scanout_compose(SgiGpuVproScanout *sc,
                                  const SgiGpuVproTables *tables,
                                  uint32_t row_y, uint32_t row_count,
                                  const uint8_t *did, const uint8_t *overlay,
                                  SgiGpuPlane *buf0, SgiGpuPlane *buf1,
                                  SgiGpuFrame *frame,
                                  void (*done)(void *opaque, bool ok),
                                  void *opaque);

#endif
