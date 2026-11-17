/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Odyssey (VPro) colour planes on the host GPU: which rows each side holds
 * current, and the draws it takes (hw/misc/sgi-odyssey-gpu.c).
 */
#ifndef HW_MISC_SGI_ODYSSEY_GPU_H
#define HW_MISC_SGI_ODYSSEY_GPU_H

#include "hw/display/sgi/rasterizer.h"

#include "qemu/notify.h"
#include "qemu/thread.h"
#include "hw/display/sgi-gpu.h"

typedef struct SGIOdysseyState SGIOdysseyState;

/* The colour buffers' rows: which copy is current (odyssey_gpu_rows). */
enum { ODSY_GPU_CPU, ODSY_GPU_BOTH, ODSY_GPU_GPU };
#define ODSY_GPU_BUFFERS 2
/* Draws that may be in flight before the next waits for the oldest. */
#define ODSY_GPU_INFLIGHT 32

typedef struct OdysseyGpu {
    SGIOdysseyState *s;
    SgiGpu *gpu;
    SgiGpuPlane *plane[ODSY_GPU_BUFFERS];
    /* Per buffer and row, an ODSY_GPU_* value; the GPU's rows when set. */
    uint8_t *rows[ODSY_GPU_BUFFERS];
    uint32_t width, height, min_pixels;
    bool lost;
    bool planes_made;
    QemuMutex lock;
    Notifier exit;
    uint32_t *stage;
    uint64_t switches, readback_bytes, upload_bytes, draws, clears;
    /* Filled draws offered the GPU, and those it took whole. */
    uint64_t offered, taken;
    /* The last fences submitted, so the queue stays bounded. */
    uint64_t fences[ODSY_GPU_INFLIGHT];
    unsigned next_fence;
} OdysseyGpu;

/* NULL, with errp set, when no GPU can be opened. */
OdysseyGpu *odyssey_gpu_new(SGIOdysseyState *s, uint32_t min_pixels,
                            Error **errp);
void odyssey_gpu_free(OdysseyGpu *g);

/* "opening", "ready" or "unavailable", for the tests. */
const char *odyssey_gpu_state(OdysseyGpu *g);
/* Whether draws may go to the GPU now: its device is open and not lost. */
bool odyssey_gpu_usable(OdysseyGpu *g);
/* Offer a draw's target the GPU hooks (SGIRasterTarget.gpu_triangle). */
void odyssey_gpu_target(OdysseyGpu *g, SGIRasterTarget *t);

/*
 * Make the CPU's copy of colour buffer b current for rows y .. y + rows - 1
 * before it reads or writes them. A write also drops the GPU's copy.
 */
void odyssey_gpu_cpu_access(OdysseyGpu *g, unsigned b, uint32_t y,
                            uint32_t rows, bool write);
/* The CPU's copy of every row current, for migration and the like. */
void odyssey_gpu_sync_all(OdysseyGpu *g);
/* Every row CPU-owned, the GPU's copy dropped: reset and migration load. */
void odyssey_gpu_reset(OdysseyGpu *g);

/*
 * Fast path of the CPU accessors: whether row y of buffer b needs
 * odyssey_gpu_cpu_access() first (a write unless CPU-only; a read if GPU-held).
 */
static inline bool odyssey_gpu_needs(const OdysseyGpu *g, unsigned b,
                                     uint32_t y, bool write)
{
    uint8_t state;

    if (y >= g->height) {
        return false;
    }
    state = qatomic_read(&g->rows[b][y]);
    return write ? state != ODSY_GPU_CPU : state == ODSY_GPU_GPU;
}

#endif
