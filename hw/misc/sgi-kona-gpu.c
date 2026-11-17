/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * KONA colour banks on the host GPU: flat fills and colour clears. A bank's
 * colour and tag planes are separate GPU surfaces; see gpu-board.c.
 */
#include "qemu/osdep.h"
#include "qapi/visitor.h"
#include "hw/misc/sgi-kona.h"

#define SKG_RECTS SGI_CLIP_MAX
#define SKG_JOBS (2 * SGI_GL_MAX_TARGETS)

/* Surfaces: 2 * bank is the colour plane, 2 * bank + 1 its tag plane. */
static const SGIPlane *skg_plane(SGIKonaState *s, unsigned surface)
{
    uint32_t id = surface & 1 ? (surface >> 1 ? s->plane_back_tag :
                                                s->plane_front_tag) :
                                (surface >> 1 ? s->plane_back :
                                                s->plane_front);

    return sgi_board_memory_plane(&s->mem, id);
}

static void skg_pack(void *opaque, unsigned surface, uint32_t y,
                     uint32_t rows, uint32_t *words)
{
    SGIKonaState *s = opaque;
    const SGIPlane *p = skg_plane(s, surface);
    unsigned bpp = sgi_plane_bytes_per_pixel(p->format);

    for (uint32_t r = 0; r < rows; r++) {
        const uint8_t *src = s->mem.bytes + p->base +
                             (uint64_t)(y + r) * p->stride;

        for (uint32_t x = 0; x < p->width; x++) {
            words[(size_t)r * p->width + x] = sgi_plane_decode(p,
                                                               src + x * bpp);
        }
    }
}

static void skg_unpack(void *opaque, unsigned surface, uint32_t y,
                       uint32_t rows, const uint32_t *words)
{
    SGIKonaState *s = opaque;
    const SGIPlane *p = skg_plane(s, surface);
    unsigned bpp = sgi_plane_bytes_per_pixel(p->format);

    for (uint32_t r = 0; r < rows; r++) {
        uint8_t *dst = s->mem.bytes + p->base +
                       (uint64_t)(y + r) * p->stride;

        for (uint32_t x = 0; x < p->width; x++) {
            sgi_plane_encode(p, words[(size_t)r * p->width + x],
                             dst + x * bpp);
        }
    }
}

static const SgiBoardGpuOps skg_ops = {
    .pack = skg_pack,
    .unpack = skg_unpack,
};

bool sgi_kona_gpu_realize(SGIKonaState *s, Error **errp)
{
    const SGIPlane *p = sgi_board_memory_plane(&s->mem, s->plane_front);

    if (s->host_gpu) {
        error_setg(errp, "x-gpu and host-gpu are exclusive");
        return false;
    }
    if (!s->mem.bytes || p->format != SGI_PLANE_RGB24) {
        error_setg(errp, "x-gpu: the colour plane's layout is not the one "
                   "the GPU kernels take");
        return false;
    }
    s->gpu = sgi_board_gpu_new("sgi-kona", 4, p->width, p->height,
                               s->gpu_min_pixels, &skg_ops, s, errp);
    if (!s->gpu) {
        return false;
    }
    sgi_board_gpu_attach(s->gpu, &s->mem, s->plane_front, 0);
    sgi_board_gpu_attach(s->gpu, &s->mem, s->plane_front_tag, 1);
    sgi_board_gpu_attach(s->gpu, &s->mem, s->plane_back, 2);
    sgi_board_gpu_attach(s->gpu, &s->mem, s->plane_back_tag, 3);
    return true;
}

/* The bank a draw's target names, or -1 for any target the GPU leaves alone. */
static int skg_bank(SGIKonaState *s, SGIColourTarget target)
{
    return target == s->plane_front ? 0 : target == s->plane_back ? 1 : -1;
}

static bool skg_targets(SGIKonaState *s, const SGIColourTarget *targets,
                        unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (skg_bank(s, targets[i]) < 0) {
            return false;
        }
    }
    return n != 0;
}

typedef struct SkgBox {
    int32_t x0, y0, x1, y1;
} SkgBox;

static bool skg_and(SkgBox *r, int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
    r->x0 = MAX(r->x0, x0);
    r->y0 = MAX(r->y0, y0);
    r->x1 = MIN(r->x1, x1);
    r->y1 = MIN(r->y1, y1);
    return r->x0 <= r->x1 && r->y0 <= r->y1;
}

/* The plane rectangles a box may reach: within the plane and the clip list. */
static unsigned skg_rects(SGIKonaState *s, const SkgBox *box,
                          const SGIClipList *clip, SkgBox *out)
{
    unsigned n = 0;

    for (unsigned j = 0; j < (clip ? MIN(clip->n, SKG_RECTS) : 1); j++) {
        SkgBox r = *box;

        if (skg_and(&r, 0, 0, (int64_t)s->gpu->width - 1,
                    (int64_t)s->gpu->height - 1) &&
            (!clip || skg_and(&r, clip->r[j].x0, clip->r[j].y0,
                              clip->r[j].x1, clip->r[j].y1))) {
            out[n++] = r;
        }
    }
    return n;
}

/* Every row the fill covered; the draw's deferral folds them into a range. */
static void skg_invalidate(SGIKonaState *s, const SGIPlane *p, int y0, int y1)
{
    for (int y = y0; y <= y1; y++) {
        sgi_kona_invalidate_store(s, p, y);
    }
}

/* Far enough inside int32 that the kernel's edge differences cannot wrap. */
#define SKG_COORD_LIMIT (1 << 29)

static bool skg_triangle(void *opaque, const SGIRasterGpuTriangle *tri,
                         bool draw)
{
    SGIKonaState *s = opaque;
    SgiGpuFillTri col[SKG_RECTS], tag[SKG_RECTS];
    SgiBoardGpuTris jobs[SKG_JOBS];
    SkgBox rects[SKG_RECTS], box;
    int64_t x[3], y[3];
    unsigned n, njobs = 0;

    for (int i = 0; i < 3; i++) {
        x[i] = tri->x[i] + (int64_t)tri->ox * 256;
        y[i] = tri->y[i] + (int64_t)tri->oy * 256;
        if (x[i] > SKG_COORD_LIMIT || x[i] < -SKG_COORD_LIMIT ||
            y[i] > SKG_COORD_LIMIT || y[i] < -SKG_COORD_LIMIT) {
            return false;
        }
    }
    if (!skg_targets(s, tri->targets, tri->ntargets)) {
        return false;
    }
    if (!draw) {
        return true;
    }
    box = (SkgBox) { tri->x0 + tri->ox, tri->y0 + tri->oy,
                     tri->x1 + tri->ox, tri->y1 + tri->oy };
    n = skg_rects(s, &box, tri->clip, rects);
    for (unsigned i = 0; i < n; i++) {
        SgiGpuFillTri t = {
            .x = { x[0], x[1], x[2] }, .y = { y[0], y[1], y[2] },
            .write_mask = UINT32_MAX,
            .clip_x0 = rects[i].x0, .clip_y0 = rects[i].y0,
            .clip_x1 = rects[i].x1, .clip_y1 = rects[i].y1,
        };

        col[i] = t;
        col[i].word = tri->rgb & 0x00ffffff;
        tag[i] = t;
        tag[i].word = SGI_KONA_TAG_DIRECT;
    }
    if (!n) {
        return true;
    }
    for (unsigned t = 0; t < tri->ntargets; t++) {
        unsigned bank = skg_bank(s, tri->targets[t]);

        jobs[njobs++] = (SgiBoardGpuTris) { 2 * bank, col, n };
        jobs[njobs++] = (SgiBoardGpuTris) { 2 * bank + 1, tag, n };
    }
    sgi_board_gpu_tris(s->gpu, jobs, njobs, 8);
    for (unsigned t = 0; t < tri->ntargets; t++) {
        const SGIPlane *p = skg_plane(s, 2 * skg_bank(s, tri->targets[t]));

        for (unsigned i = 0; i < n; i++) {
            skg_invalidate(s, p, rects[i].y0, rects[i].y1);
        }
    }
    return true;
}

static bool skg_clear(void *opaque, const SGIRasterOp *op,
                      const SGIRasterColour *colour, uint32_t rgba_mask)
{
    SGIKonaState *s = opaque;
    SgiGpuFillClear col[SKG_RECTS], tag[SKG_RECTS];
    SgiBoardGpuClears jobs[SKG_JOBS];
    SkgBox rects[SKG_RECTS], box = { op->x0, op->y0, op->x1, op->y1 };
    uint32_t word;
    unsigned n, njobs = 0;

    if ((int64_t)(op->x1 - op->x0 + 1) * (op->y1 - op->y0 + 1) <
        (int64_t)s->gpu_min_pixels || op->kind != SGI_RASTER_CLEAR ||
        op->index_bits || !skg_targets(s, op->targets, op->ntargets)) {
        return false;
    }
    word = (colour->component[0] << 16) | (colour->component[1] << 8) |
           colour->component[2];
    n = skg_rects(s, &box, op->clip.n > 1 ? &op->clip : NULL, rects);
    for (unsigned i = 0; i < n; i++) {
        SgiGpuFillClear c = {
            .x0 = rects[i].x0, .y0 = rects[i].y0,
            .x1 = rects[i].x1, .y1 = rects[i].y1,
            .write_mask = UINT32_MAX,
        };

        col[i] = c;
        col[i].value = word & 0x00ffffff;
        tag[i] = c;
        tag[i].value = SGI_KONA_TAG_DIRECT;
    }
    if (!n) {
        return true;
    }
    for (unsigned t = 0; t < op->ntargets; t++) {
        unsigned bank = skg_bank(s, op->targets[t]);

        jobs[njobs++] = (SgiBoardGpuClears) { 2 * bank, col, n };
        jobs[njobs++] = (SgiBoardGpuClears) { 2 * bank + 1, tag, n };
    }
    sgi_board_gpu_clears(s->gpu, jobs, njobs);
    for (unsigned t = 0; t < op->ntargets; t++) {
        const SGIPlane *p = skg_plane(s, 2 * skg_bank(s, op->targets[t]));

        for (unsigned i = 0; i < n; i++) {
            skg_invalidate(s, p, rects[i].y0, rects[i].y1);
        }
    }
    return true;
}

void sgi_kona_gpu_target(SGIKonaState *s, SGIRasterTarget *t)
{
    int ctx = s->context_current;

    /* Only in a slice off the big lock, and not for a per-pixel window. */
    if (!s->gpu || !s->work.drawing || !sgi_head_work_may_unlock(&s->work) ||
        (ctx != SGI_KONA_CONTEXT_NONE &&
         s->state[ctx]->window.owned_by_pixel) ||
        !sgi_board_gpu_usable(s->gpu)) {
        return;
    }
    t->gpu_triangle = skg_triangle;
    t->gpu_clear = skg_clear;
    t->gpu_min_pixels = s->gpu_min_pixels;
}

/* x-gpu's state and counters, read-only host diagnostics. */
static void skg_get_ready(Object *obj, Visitor *v, const char *name,
                          void *opaque, Error **errp)
{
    SgiBoardGpu *g = SGI_KONA(obj)->gpu;
    bool value = g && sgi_gpu_ready(g->gpu);

    visit_type_bool(v, name, &value, errp);
}

static void skg_get_counter(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    SgiBoardGpu *g = SGI_KONA(obj)->gpu;
    uint64_t value = 0;

    if (g) {
        switch ((uintptr_t)opaque) {
        case 0:
            value = qatomic_read(&g->switches);
            break;
        case 1:
            value = qatomic_read(&g->readback_bytes);
            break;
        case 2:
            value = qatomic_read(&g->draws) + qatomic_read(&g->clears);
            break;
        default:
            value = qatomic_read(&g->upload_bytes);
            break;
        }
    }
    visit_type_uint64(v, name, &value, errp);
}

void sgi_kona_gpu_instance_init(Object *obj)
{
    object_property_add(obj, "x-gpu-ready", "bool", NULL, skg_get_ready, NULL,
                        NULL, NULL);
    object_property_add(obj, "x-gpu-switches", "uint64", NULL,
                        skg_get_counter, NULL, NULL, (void *)(uintptr_t)0);
    object_property_add(obj, "x-gpu-readback-bytes", "uint64", NULL,
                        skg_get_counter, NULL, NULL, (void *)(uintptr_t)1);
    object_property_add(obj, "x-gpu-draws", "uint64", NULL, skg_get_counter,
                        NULL, NULL, (void *)(uintptr_t)2);
    object_property_add(obj, "x-gpu-upload-bytes", "uint64", NULL,
                        skg_get_counter, NULL, NULL, (void *)(uintptr_t)3);
}
