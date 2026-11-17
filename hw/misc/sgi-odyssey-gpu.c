/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Odyssey colour planes on the host GPU, each row owned by the CPU, both or the
 * GPU; a CPU access downloads a GPU-only row first.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "qemu/notify.h"
#include "system/runstate.h"
#include "hw/misc/sgi-odyssey.h"
#include "hw/misc/sgi-odyssey-gpu.h"
#include "hw/misc/sgi-odyssey-scanout-gpu.h"
#include "hw/display/sgi-gpu-vpro.h"
#include "trace.h"

/* Rows a download takes beyond the one asked for, if the GPU holds them. */
#define OG_READ_AHEAD 64

static SGIBoardMemory *og_mem(OdysseyGpu *g, unsigned b)
{
    return b ? &g->s->buffer1_mem : &g->s->mem;
}

static const SGIPlane *og_plane(OdysseyGpu *g, unsigned b)
{
    return sgi_board_memory_plane(og_mem(g, b),
                                  b ? g->s->buffer1_plane :
                                      g->s->colour_plane);
}

/* The bytes of row y of buffer b in board memory. */
static uint8_t *og_row_bytes(OdysseyGpu *g, unsigned b, uint32_t y)
{
    const SGIPlane *p = og_plane(g, b);

    return og_mem(g, b)->bytes + p->base + (uint64_t)y * p->stride;
}

static void og_lost(OdysseyGpu *g)
{
    if (!g->lost) {
        g->lost = true;
        odyssey_gpu_stop(g->gpu, g->gpu ? sgi_gpu_lost(g->gpu) : NULL);
    }
}

/*
 * Wait for the worker thread that opens the device: exiting while the driver
 * is still starting hangs or crashes, and by exit() it is too late.
 */
static void og_exit(Notifier *n, void *data)
{
    OdysseyGpu *g = container_of(n, OdysseyGpu, exit);
    int64_t deadline = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;

    while (g->gpu && !sgi_gpu_ready(g->gpu) && !sgi_gpu_lost(g->gpu) &&
           g_get_monotonic_time() < deadline) {
        g_usleep(10000);
    }
}

OdysseyGpu *odyssey_gpu_new(SGIOdysseyState *s, uint32_t min_pixels,
                            Error **errp)
{
    OdysseyGpu *g;
    const SGIPlane *p = sgi_board_memory_plane(&s->mem, s->colour_plane);

    if (p->packing != SGI_PACK_BE_RED_HIGH || p->format != SGI_PLANE_RGBA8 ||
        p->width != SGI_ODYSSEY_WIDTH || p->stride != 4 * p->width) {
        error_setg(errp, "x-gpu: the colour plane's layout is not the one "
                   "the GPU kernels take");
        return NULL;
    }
    g = g_new0(OdysseyGpu, 1);
    g->s = s;
    g->width = p->width;
    g->height = p->height;
    g->min_pixels = min_pixels;
    g->gpu = sgi_gpu_open();
    if (!g->gpu) {
        warn_report("sgi-odyssey: x-gpu has no GPU back end (built without "
                    "it or SGI_GPU=off); drawing on the CPU");
    }
    qemu_mutex_init(&g->lock);
    g->exit.notify = og_exit;
    qemu_register_shutdown_notifier(&g->exit);
    for (unsigned b = 0; b < ODSY_GPU_BUFFERS; b++) {
        g->rows[b] = g_malloc0(g->height);
    }
    g->stage = g_new(uint32_t, (size_t)g->width * g->height);
    return g;
}

void odyssey_gpu_free(OdysseyGpu *g)
{
    if (!g) {
        return;
    }
    notifier_remove(&g->exit);
    for (unsigned b = 0; b < ODSY_GPU_BUFFERS; b++) {
        if (g->plane[b]) {
            sgi_gpu_plane_free(g->plane[b]);
        }
        g_free(g->rows[b]);
    }
    qemu_mutex_destroy(&g->lock);
    g_free(g->stage);
    g_free(g);
}

const char *odyssey_gpu_state(OdysseyGpu *g)
{
    if (!g->gpu || g->lost || sgi_gpu_lost(g->gpu)) {
        return "unavailable";
    }
    return sgi_gpu_ready(g->gpu) ? "ready" : "opening";
}

bool odyssey_gpu_usable(OdysseyGpu *g)
{
    if (!g->gpu || g->lost || !sgi_gpu_ready(g->gpu)) {
        return false;
    }
    if (qatomic_load_acquire(&g->planes_made)) {
        return true;
    }
    if (sgi_gpu_lost(g->gpu)) {
        og_lost(g);
        return false;
    }
    qemu_mutex_lock(&g->lock);
    for (unsigned b = 0; b < ODSY_GPU_BUFFERS; b++) {
        if (!g->plane[b]) {
            g->plane[b] = sgi_gpu_plane_new(g->gpu, g->width, g->height);
        }
        if (!g->plane[b]) {
            qemu_mutex_unlock(&g->lock);
            og_lost(g);
            return false;
        }
    }
    qatomic_store_release(&g->planes_made, true);
    qemu_mutex_unlock(&g->lock);
    return true;
}

static void og_set(OdysseyGpu *g, unsigned b, uint32_t y0, uint32_t y1,
                   uint8_t state)
{
    for (uint32_t y = y0; y <= y1; y++) {
        qatomic_set(&g->rows[b][y], state);
    }
}

/*
 * Wait for the fence ODSY_GPU_INFLIGHT draws back so the queue stays bounded;
 * the caller holds g->lock, not the big lock.
 */
static void og_pace(OdysseyGpu *g, uint64_t fence)
{
    uint64_t old = g->fences[g->next_fence];

    if (old && !sgi_gpu_fence_wait(g->gpu, old)) {
        og_lost(g);
    }
    g->fences[g->next_fence] = fence;
    g->next_fence = (g->next_fence + 1) % ODSY_GPU_INFLIGHT;
}

/* Download rows y0..y1 of buffer b, all in the GPU state; the lock is held. */
static void og_download(OdysseyGpu *g, unsigned b, uint32_t y0, uint32_t y1)
{
    uint32_t n = y1 - y0 + 1;

    if (g->lost || !sgi_gpu_plane_read(g->plane[b], y0, n, g->stage,
                                       g->width)) {
        /* The machine is stopping; the rows keep the CPU's last copy. */
        og_lost(g);
        og_set(g, b, y0, y1, ODSY_GPU_CPU);
        return;
    }
    for (uint32_t y = y0; y <= y1; y++) {
        uint8_t *dst = og_row_bytes(g, b, y);
        const uint32_t *src = g->stage + (size_t)(y - y0) * g->width;

        for (uint32_t x = 0; x < g->width; x++) {
            stl_be_p(dst + 4 * x, src[x]);
        }
    }
    og_set(g, b, y0, y1, ODSY_GPU_BOTH);
    g->switches++;
    g->readback_bytes += (uint64_t)n * g->width * 4;
    trace_sgi_odyssey_gpu_rows("download", b, y0, n);
}

void odyssey_gpu_cpu_access(OdysseyGpu *g, unsigned b, uint32_t y,
                            uint32_t rows, bool write)
{
    uint32_t y1;

    if (b >= ODSY_GPU_BUFFERS || y >= g->height || !rows) {
        return;
    }
    y1 = MIN(y + rows, g->height) - 1;
    qemu_mutex_lock(&g->lock);
    for (uint32_t r = y; r <= y1; r++) {
        if (g->rows[b][r] == ODSY_GPU_GPU) {
            uint32_t end = r;

            /* The rest of this run and some ahead: sweeps read in order. */
            while (end + 1 < g->height && end + 1 - r < OG_READ_AHEAD &&
                   g->rows[b][end + 1] == ODSY_GPU_GPU) {
                end++;
            }
            og_download(g, b, r, end);
            r = MAX(r, MIN(end, y1));
        }
    }
    if (write) {
        og_set(g, b, y, y1, ODSY_GPU_CPU);
    }
    qemu_mutex_unlock(&g->lock);
}

void odyssey_gpu_sync_all(OdysseyGpu *g)
{
    if (g) {
        for (unsigned b = 0; b < ODSY_GPU_BUFFERS; b++) {
            odyssey_gpu_cpu_access(g, b, 0, g->height, false);
        }
    }
}

void odyssey_gpu_reset(OdysseyGpu *g)
{
    if (!g) {
        return;
    }
    qemu_mutex_lock(&g->lock);
    for (unsigned b = 0; b < ODSY_GPU_BUFFERS; b++) {
        og_set(g, b, 0, g->height - 1, ODSY_GPU_CPU);
    }
    qemu_mutex_unlock(&g->lock);
}

/* Upload rows y0..y1 of buffer b that only the CPU holds; the lock is held. */
static bool og_upload(OdysseyGpu *g, unsigned b, uint32_t y0, uint32_t y1)
{
    uint32_t y = y0;

    while (y <= y1) {
        uint32_t end;

        if (g->rows[b][y] != ODSY_GPU_CPU) {
            y++;
            continue;
        }
        end = y;
        while (end + 1 <= y1 && g->rows[b][end + 1] == ODSY_GPU_CPU) {
            end++;
        }
        for (uint32_t r = y; r <= end; r++) {
            const uint8_t *src = og_row_bytes(g, b, r);
            uint32_t *dst = g->stage + (size_t)(r - y) * g->width;

            for (uint32_t x = 0; x < g->width; x++) {
                dst[x] = ldl_be_p(src + 4 * x);
            }
        }
        if (!sgi_gpu_plane_write(g->plane[b], y, end - y + 1, g->stage,
                                 g->width)) {
            og_lost(g);
            return false;
        }
        og_set(g, b, y, end, ODSY_GPU_BOTH);
        g->switches++;
        g->upload_bytes += (uint64_t)(end - y + 1) * g->width * 4;
        trace_sgi_odyssey_gpu_rows("upload", b, y, end - y + 1);
        y = end + 1;
    }
    return true;
}

typedef struct OgRect {
    int32_t x0, y0, x1, y1;
} OgRect;

static bool og_rect_and(OgRect *r, int64_t x0, int64_t y0, int64_t x1,
                        int64_t y1)
{
    r->x0 = MAX(r->x0, x0);
    r->y0 = MAX(r->y0, y0);
    r->x1 = MIN(r->x1, x1);
    r->y1 = MIN(r->y1, y1);
    return r->x0 <= r->x1 && r->y0 <= r->y1;
}

/*
 * The plane rectangles a draw's box may reach: inside the kept clip rectangles
 * and the window's list, whose overlaps a flat store ignores.
 */
static unsigned og_rects(OdysseyGpu *g, const OgRect *box,
                         const SGIClipList *clip, OgRect *out)
{
    SGIOdysseyState *s = g->s;
    unsigned n = 0;

    for (unsigned i = 0; i < s->clip_kept; i++) {
        const SGIClipRect *k = &s->clip_rect[i];
        unsigned m = clip ? clip->n : 1;

        for (unsigned j = 0; j < m; j++) {
            OgRect r = *box;

            if (og_rect_and(&r, k->x0, k->y0, k->x1, k->y1) &&
                og_rect_and(&r, 0, 0, (int64_t)g->width - 1,
                            (int64_t)g->height - 1) &&
                (!clip || og_rect_and(&r, clip->r[j].x0, clip->r[j].y0,
                                      clip->r[j].x1, clip->r[j].y1))) {
                out[n++] = r;
            }
        }
    }
    return n;
}

/* Make the GPU's rows current for the rects, then mark them its own. */
static bool og_prepare(OdysseyGpu *g, const SGIColourTarget *targets,
                       unsigned ntargets, const OgRect *rects, unsigned n,
                       bool done)
{
    uint32_t y0 = UINT32_MAX, y1 = 0;

    for (unsigned i = 0; i < n; i++) {
        y0 = MIN(y0, rects[i].y0);
        y1 = MAX(y1, rects[i].y1);
    }
    for (unsigned t = 0; t < ntargets; t++) {
        unsigned b = targets[t];

        if (b >= ODSY_GPU_BUFFERS) {
            continue;
        }
        if (!done) {
            if (!og_upload(g, b, y0, y1)) {
                return false;
            }
        } else {
            og_set(g, b, y0, y1, ODSY_GPU_GPU);
        }
    }
    return true;
}

/* Whether the CPU alone holds any row the rects span; the lock is held. */
static bool og_cpu_held(OdysseyGpu *g, const SGIColourTarget *targets,
                        unsigned ntargets, const OgRect *rects, unsigned n)
{
    for (unsigned t = 0; t < ntargets; t++) {
        unsigned b = targets[t];

        for (unsigned i = 0; b < ODSY_GPU_BUFFERS && i < n; i++) {
            for (int32_t y = rects[i].y0; y <= rects[i].y1; y++) {
                if (g->rows[b][y] == ODSY_GPU_CPU) {
                    return true;
                }
            }
        }
    }
    return false;
}

#define OG_RECTS_MAX (SGI_CLIP_MAX * SGI_ODYSSEY_CLIP_RECTS)

static bool og_triangle(void *opaque, const SGIRasterGpuTriangle *tri,
                        bool draw)
{
    SGIOdysseyState *s = opaque;
    OdysseyGpu *g = s->gpu;
    SgiGpuVproTri out[OG_RECTS_MAX];
    OgRect rects[OG_RECTS_MAX], box;
    int64_t x[3], y[3];
    uint32_t colour, count = 0;
    unsigned n;

    for (int i = 0; i < 3; i++) {
        x[i] = tri->x[i] + (int64_t)tri->ox * 256;
        y[i] = tri->y[i] + (int64_t)tri->oy * 256;
        /* The kernel's edge terms are 32-bit differences; 2^30 keeps them. */
        if (llabs(x[i]) >= (1 << 30) || llabs(y[i]) >= (1 << 30)) {
            return false;
        }
    }
    if (!draw) {
        return true;
    }
    box = (OgRect) { tri->x0 + tri->ox, tri->y0 + tri->oy,
                     tri->x1 + tri->ox, tri->y1 + tri->oy };
    n = og_rects(g, &box, tri->clip, rects);
    if (!n) {
        return true;
    }
    colour = (tri->colour.component[0] << 24) |
             (tri->colour.component[1] << 16) |
             (tri->colour.component[2] << 8) | tri->colour.component[3];
    for (unsigned i = 0; i < n; i++) {
        out[count++] = (SgiGpuVproTri) {
            .x = { x[0], x[1], x[2] }, .y = { y[0], y[1], y[2] },
            .colour = colour, .write_mask = UINT32_MAX,
            .clip_x0 = rects[i].x0, .clip_y0 = rects[i].y0,
            .clip_x1 = rects[i].x1, .clip_y1 = rects[i].y1,
        };
    }
    qemu_mutex_lock(&g->lock);
    if (!og_prepare(g, tri->targets, tri->ntargets, rects, n, false)) {
        qemu_mutex_unlock(&g->lock);
        return true;
    }
    for (unsigned t = 0; t < tri->ntargets; t++) {
        uint64_t fence;

        if (tri->targets[t] >= ODSY_GPU_BUFFERS) {
            continue;
        }
        fence = sgi_gpu_vpro_tris(g->plane[tri->targets[t]], out, count);
        if (!fence) {
            og_lost(g);
            break;
        }
        og_pace(g, fence);
    }
    og_prepare(g, tri->targets, tri->ntargets, rects, n, true);
    g->draws++;
    qemu_mutex_unlock(&g->lock);
    trace_sgi_odyssey_gpu_draw("triangle", count);
    return true;
}

static bool og_clear(void *opaque, const SGIRasterOp *op,
                     const SGIRasterColour *colour, uint32_t rgba_mask)
{
    SGIOdysseyState *s = opaque;
    OdysseyGpu *g = s->gpu;
    OgRect rects[OG_RECTS_MAX], box = { op->x0, op->y0, op->x1, op->y1 };
    uint32_t mask = 0, word, count = 0;
    unsigned n;

    if ((int64_t)(op->x1 - op->x0 + 1) * (op->y1 - op->y0 + 1) <
        (int64_t)g->min_pixels || op->kind != SGI_RASTER_CLEAR ||
        op->index_bits) {
        return false;
    }
    for (unsigned k = 0; k < 4; k++) {
        if (rgba_mask & (1u << k)) {
            mask |= 0xffu << (24 - 8 * k);
        }
    }
    word = (colour->component[0] << 24) | (colour->component[1] << 16) |
           (colour->component[2] << 8) | colour->component[3];
    n = og_rects(g, &box, op->clip.n > 1 ? &op->clip : NULL, rects);
    if (!n || !mask) {
        return true;
    }
    qemu_mutex_lock(&g->lock);
    /* Uploading rows only to clear them makes CPU draws download them back. */
    if (og_cpu_held(g, op->targets, op->ntargets, rects, n)) {
        qemu_mutex_unlock(&g->lock);
        return false;
    }
    if (og_prepare(g, op->targets, op->ntargets, rects, n, false)) {
        for (unsigned t = 0; t < op->ntargets; t++) {
            if (op->targets[t] >= ODSY_GPU_BUFFERS) {
                continue;
            }
            for (unsigned i = 0; i < n; i++) {
                SgiGpuVproClear c = {
                    .x0 = rects[i].x0, .y0 = rects[i].y0,
                    .x1 = rects[i].x1, .y1 = rects[i].y1,
                    .value = word, .write_mask = mask,
                };

                uint64_t fence = sgi_gpu_vpro_clear(g->plane[op->targets[t]],
                                                    &c);

                if (!fence) {
                    og_lost(g);
                    break;
                }
                og_pace(g, fence);
                count++;
            }
        }
        og_prepare(g, op->targets, op->ntargets, rects, n, true);
        g->clears++;
    }
    qemu_mutex_unlock(&g->lock);
    trace_sgi_odyssey_gpu_draw("clear", count);
    return true;
}

void odyssey_gpu_target(OdysseyGpu *g, SGIRasterTarget *t)
{
    t->gpu_triangle = og_triangle;
    t->gpu_clear = og_clear;
    t->gpu_min_pixels = g->min_pixels;
}
