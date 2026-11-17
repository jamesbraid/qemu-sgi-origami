/*
 * SGI graphics GPU back end: stand-ins for builds without rust/sgi-gpu.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "hw/display/sgi-gpu.h"
#include "hw/display/sgi-gpu-board.h"
#include "hw/display/sgi-gpu-vpro.h"
#include "hw/display/sgi-gpu-scanout.h"

SgiGpu *sgi_gpu_open(void)
{
    return NULL;
}

bool sgi_gpu_ready(SgiGpu *gpu)
{
    return false;
}

const char *sgi_gpu_adapter_name(SgiGpu *gpu)
{
    return NULL;
}

const char *sgi_gpu_lost(SgiGpu *gpu)
{
    return NULL;
}

SgiGpuPlane *sgi_gpu_plane_new(SgiGpu *gpu, uint32_t width, uint32_t height)
{
    return NULL;
}

void sgi_gpu_plane_free(SgiGpuPlane *plane)
{
}

bool sgi_gpu_plane_write(SgiGpuPlane *plane, uint32_t y, uint32_t rows,
                         const uint32_t *words, size_t stride_words)
{
    return false;
}

bool sgi_gpu_plane_read(SgiGpuPlane *plane, uint32_t y, uint32_t rows,
                        uint32_t *words, size_t stride_words)
{
    return false;
}

bool sgi_gpu_fence_done(SgiGpu *gpu, uint64_t fence)
{
    return false;
}

bool sgi_gpu_fence_wait(SgiGpu *gpu, uint64_t fence)
{
    return false;
}

SgiGpuFrame *sgi_gpu_frame_new(SgiGpu *gpu, uint32_t width, uint32_t height)
{
    return NULL;
}

void sgi_gpu_frame_free(SgiGpuFrame *frame)
{
}

bool sgi_gpu_frame_export(SgiGpuFrame *frame, SgiGpuExport *out)
{
    return false;
}

bool sgi_gpu_frame_read(SgiGpuFrame *frame, uint32_t *xrgb, size_t stride_words)
{
    return false;
}

uint64_t sgi_gpu_vpro_tris(SgiGpuPlane *colour, const SgiGpuVproTri *tris,
                           size_t count)
{
    return 0;
}

uint64_t sgi_gpu_vpro_clear(SgiGpuPlane *plane, const SgiGpuVproClear *clear)
{
    return 0;
}

uint64_t sgi_gpu_fill_tris(SgiGpuPlane *plane, uint32_t sub_bits,
                           const SgiGpuFillTri *tris, size_t count)
{
    return 0;
}

uint64_t sgi_gpu_fill_clear(SgiGpuPlane *plane, const SgiGpuFillClear *clear)
{
    return 0;
}

SgiGpuVproScanout *sgi_gpu_vpro_scanout_new(SgiGpu *gpu, uint32_t width,
                                            uint32_t height)
{
    return NULL;
}

void sgi_gpu_vpro_scanout_free(SgiGpuVproScanout *sc)
{
}

void sgi_gpu_vpro_scanout_quiesce(SgiGpuVproScanout *sc)
{
}

bool sgi_gpu_vpro_scanout_compose(SgiGpuVproScanout *sc,
                                  const SgiGpuVproTables *tables,
                                  uint32_t row_y, uint32_t row_count,
                                  const uint8_t *did, const uint8_t *overlay,
                                  SgiGpuPlane *buf0, SgiGpuPlane *buf1,
                                  SgiGpuFrame *frame,
                                  void (*done)(void *opaque, bool ok),
                                  void *opaque)
{
    return false;
}
