/*
 * SGI graphics GPU back end: C interface to rust/sgi-gpu.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef HW_DISPLAY_SGI_GPU_H
#define HW_DISPLAY_SGI_GPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct SgiGpu SgiGpu;
typedef struct SgiGpuPlane SgiGpuPlane;
typedef struct SgiGpuFrame SgiGpuFrame;

/* Process-wide device, opened on a worker thread; NULL if disabled. */
SgiGpu *sgi_gpu_open(void);
bool sgi_gpu_ready(SgiGpu *gpu);
/* Adapter name once ready, for logs and query output. */
const char *sgi_gpu_adapter_name(SgiGpu *gpu);
/* Device loss is fatal; this returns its message once it has happened. */
const char *sgi_gpu_lost(SgiGpu *gpu);

/* A plane holds one 32-bit word per pixel, laid out as the board stores it. */
SgiGpuPlane *sgi_gpu_plane_new(SgiGpu *gpu, uint32_t width, uint32_t height);
void sgi_gpu_plane_free(SgiGpuPlane *plane);
bool sgi_gpu_plane_write(SgiGpuPlane *plane, uint32_t y, uint32_t rows,
                         const uint32_t *words, size_t stride_words);
/* Blocks until every submitted batch touching the plane has finished. */
bool sgi_gpu_plane_read(SgiGpuPlane *plane, uint32_t y, uint32_t rows,
                        uint32_t *words, size_t stride_words);

/* Batches complete in submission order; fences are monotonic per device. */
bool sgi_gpu_fence_done(SgiGpu *gpu, uint64_t fence);
bool sgi_gpu_fence_wait(SgiGpu *gpu, uint64_t fence);

typedef enum SgiGpuExportKind {
    SGI_GPU_EXPORT_NONE = 0,
    SGI_GPU_EXPORT_DMABUF = 1,
    SGI_GPU_EXPORT_IOSURFACE = 2,
    SGI_GPU_EXPORT_WIN32 = 3,
} SgiGpuExportKind;

typedef struct SgiGpuExport {
    SgiGpuExportKind kind;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t fourcc;
    uint64_t modifier;  /* DRM format modifier of a dma-buf */
    uint64_t offset;    /* byte offset of the pixels in the buffer */
    int64_t handle;     /* fd, Mach port or HANDLE; owned by the frame */
    int64_t fence;      /* HANDLE of a shared fence, or -1; owned too */
    uint64_t adapter_id; /* LUID, render node dev_t or registry ID, or 0 */
} SgiGpuExport;

/* An exportable XRGB8888 frame; falls back to readback when not exportable. */
SgiGpuFrame *sgi_gpu_frame_new(SgiGpu *gpu, uint32_t width, uint32_t height);
void sgi_gpu_frame_free(SgiGpuFrame *frame);
bool sgi_gpu_frame_export(SgiGpuFrame *frame, SgiGpuExport *out);
bool sgi_gpu_frame_read(SgiGpuFrame *frame, uint32_t *xrgb,
                        size_t stride_words);

#endif
