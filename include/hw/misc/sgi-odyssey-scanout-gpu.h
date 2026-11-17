/*
 * VPro scan-out compose on the GPU: the glue between the Odyssey board, the
 * shared scanout and the SGI GPU back end.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef HW_MISC_SGI_ODYSSEY_SCANOUT_GPU_H
#define HW_MISC_SGI_ODYSSEY_SCANOUT_GPU_H

#include "hw/misc/sgi-odyssey.h"
#include "hw/display/sgi-gpu.h"

typedef struct OdysseyGpuScanout OdysseyGpuScanout;

/*
 * The colour buffers as GPU planes when every row is GPU-current; false when
 * the CPU owns any, and the scan-out then resolves on the CPU. Main loop.
 */
typedef bool (*OdysseyGpuPlanes)(SGIOdysseyState *s, SgiGpuPlane **buf0,
                                 SgiGpuPlane **buf1);

/*
 * Compose s's scan-out on gpu for viewers that take GPU images; set
 * SGI_GPU_SCANOUT_VERIFY to compare each frame with odyssey_resolve_row().
 */
OdysseyGpuScanout *odyssey_gpu_scanout_new(SGIOdysseyState *s, SgiGpu *gpu,
                                           OdysseyGpuPlanes planes);
/*
 * The one way a lost GPU stops the machine: an error naming the adapter and
 * a stop request, once, since GPU-only rows cannot be recovered.
 */
void odyssey_gpu_stop(SgiGpu *gpu, const char *why);

/* Before the board's scanout is cleaned up. */
void odyssey_gpu_scanout_free(OdysseyGpuScanout *g);

#endif
