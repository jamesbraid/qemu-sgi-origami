/* SPDX-License-Identifier: BSD-3-Clause */
/* MGRAS flat fills on the host GPU (sgi-mgras-gpu.c). */
#ifndef HW_MISC_SGI_MGRAS_GPU_H
#define HW_MISC_SGI_MGRAS_GPU_H

#include "hw/misc/sgi-mgras.h"

bool sgi_mgras_gpu_realize(SGIMgrasState *s, Error **errp);
void sgi_mgras_gpu_cleanup(SGIMgrasState *s);
void sgi_mgras_gpu_instance_init(Object *obj);
/* Offer a draw's flat fills and clears to the GPU, where that is allowed. */
void sgi_mgras_gpu_target(SGIMgrasState *s, SGIRasterTarget *t);
void sgi_mgras_gpu_rows(SGIMgrasState *s, SGIColourTarget target, int y,
                        bool write);
/* Submit what a slice's draws queued; the board's thread, at a slice's end. */
void sgi_mgras_gpu_flush(SGIMgrasState *s);
/* Draws submitted to the GPU have not all finished. */
bool sgi_mgras_gpu_busy(SGIMgrasState *s);
/* As the status register shows it: bounded by x-test-gpu-busy-cap-ms. */
bool sgi_mgras_gpu_status_busy(SGIMgrasState *s);
void sgi_mgras_gpu_cpu_all(SGIMgrasState *s);
void sgi_mgras_gpu_sync_all(SGIMgrasState *s);

/* The CPU reads or writes the target's pixel on GL row y. */
static inline void sgi_mgras_gpu_pixel(SGIMgrasState *s,
                                       SGIColourTarget target, int y,
                                       bool write)
{
    if (G_UNLIKELY(s->gpu)) {
        sgi_mgras_gpu_rows(s, target, y, write);
    }
}

/* The same, for the board's array row (counted down from the top). */
static inline void sgi_mgras_gpu_mem(SGIMgrasState *s, SGIColourTarget target,
                                     int mem_row, bool write)
{
    if (G_UNLIKELY(s->gpu)) {
        sgi_mgras_gpu_rows(s, target, s->height - 1 - mem_row, write);
    }
}

/* The CPU may touch any pixel: take every row back. */
static inline void sgi_mgras_gpu_all(SGIMgrasState *s)
{
    if (G_UNLIKELY(s->gpu)) {
        sgi_mgras_gpu_cpu_all(s);
    }
}

/* The CPU's arrays were replaced wholesale: the GPU's rows are stale. */
static inline void sgi_mgras_gpu_forget(SGIMgrasState *s)
{
    if (G_UNLIKELY(s->gpu)) {
        sgi_board_gpu_reset(s->gpu);
    }
}

/* The CPU reads any pixel; the GPU keeps its rows. */
static inline void sgi_mgras_gpu_read_all(SGIMgrasState *s)
{
    if (G_UNLIKELY(s->gpu)) {
        sgi_mgras_gpu_sync_all(s);
    }
}

#endif
