/*
 * Flat fills and plane row ownership for boards on the shared rasterizer's
 * rules (rust/sgi-gpu/src/fill.rs, hw/display/sgi/gpu-board.c).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef HW_DISPLAY_SGI_GPU_BOARD_H
#define HW_DISPLAY_SGI_GPU_BOARD_H

#include "qapi/error.h"
#include "qemu/notify.h"
#include "qemu/thread.h"
#include "hw/display/sgi-gpu.h"
#include "hw/display/sgi/board-memory.h"

/* One eligible triangle: snapped vertices at 2^-sub_bits, and its word. */
typedef struct SgiGpuFillTri {
    int32_t x[3], y[3];
    uint32_t word;
    uint32_t write_mask;
    int32_t clip_x0, clip_y0, clip_x1, clip_y1;   /* inclusive, pixels */
} SgiGpuFillTri;

typedef struct SgiGpuFillClear {
    int32_t x0, y0, x1, y1;     /* inclusive, pixels */
    uint32_t value;
    uint32_t write_mask;
} SgiGpuFillClear;

/* Queue work on a plane; returns its fence, 0 on failure. */
uint64_t sgi_gpu_fill_tris(SgiGpuPlane *plane, uint32_t sub_bits,
                           const SgiGpuFillTri *tris, size_t count);
uint64_t sgi_gpu_fill_clear(SgiGpuPlane *plane, const SgiGpuFillClear *clear);

#define SGI_BOARD_GPU_SURFACES 8

/* Convert rows y..y+rows-1 between GPU words and the board's bytes. */
typedef struct SgiBoardGpuOps {
    void (*pack)(void *opaque, unsigned surface, uint32_t y, uint32_t rows,
                 uint32_t *words);
    void (*unpack)(void *opaque, unsigned surface, uint32_t y, uint32_t rows,
                   const uint32_t *words);
} SgiBoardGpuOps;

typedef struct SgiBoardGpu {
    const char *who;
    unsigned nsurfaces;
    SgiGpu *gpu;
    const SgiBoardGpuOps *ops;
    void *opaque;
    SGIBoardMemoryGpu hook;
    SgiGpuPlane *plane[SGI_BOARD_GPU_SURFACES];
    uint8_t *rows[SGI_BOARD_GPU_SURFACES];
    uint8_t surface_of[SGI_PLANES_MAX];
    uint32_t width, height, min_pixels;
    bool lost;
    Notifier shutdown;
    QemuMutex lock;
    uint32_t *stage;
    uint64_t fence;     /* of the last fill submitted, for pacing */
    uint64_t switches, readback_bytes, upload_bytes, draws, clears;
} SgiBoardGpu;

/* NULL, with errp set, when no GPU can be opened. */
SgiBoardGpu *sgi_board_gpu_new(const char *who, unsigned surfaces,
                               uint32_t width, uint32_t height,
                               uint32_t min_pixels, const SgiBoardGpuOps *ops,
                               void *opaque, Error **errp);
void sgi_board_gpu_free(SgiBoardGpu *g);
/* Let plane id of m hold surface's rows; CPU accesses to it then check. */
void sgi_board_gpu_attach(SgiBoardGpu *g, SGIBoardMemory *m, uint32_t id,
                          unsigned surface);
/* The planes are laid out again at width x height: nothing is kept. */
void sgi_board_gpu_resize(SgiBoardGpu *g, uint32_t width, uint32_t height);

/* Whether draws may go to the GPU now: its device is open and not lost. */
bool sgi_board_gpu_usable(SgiBoardGpu *g);
/* The CPU's copy of every row current, for migration and the like. */
void sgi_board_gpu_sync_all(SgiBoardGpu *g);
/* Every row CPU-owned, the GPU's copy dropped: reset and migration load. */
void sgi_board_gpu_reset(SgiBoardGpu *g);

/* Fill work per surface; false when the GPU failed and stopped the machine. */
typedef struct SgiBoardGpuTris {
    unsigned surface;
    const SgiGpuFillTri *tris;
    unsigned count;
} SgiBoardGpuTris;

typedef struct SgiBoardGpuClears {
    unsigned surface;
    const SgiGpuFillClear *clears;
    unsigned count;
} SgiBoardGpuClears;

bool sgi_board_gpu_tris(SgiBoardGpu *g, const SgiBoardGpuTris *jobs,
                        unsigned njobs, uint32_t sub_bits);
bool sgi_board_gpu_clears(SgiBoardGpu *g, const SgiBoardGpuClears *jobs,
                          unsigned njobs);

#endif
