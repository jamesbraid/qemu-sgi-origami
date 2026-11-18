/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * MGRAS colour banks on the host GPU: flat fills and colour clears. A bank
 * is several surfaces (colour, format tag, alpha, bank 1's deep slot), and
 * GPU rows count up from the bottom where the board's arrays count down.
 */
#include "qemu/osdep.h"
#include "qapi/visitor.h"
#include "hw/misc/sgi-mgras.h"
#include "hw/misc/sgi-mgras-gl.h"
#include "hw/misc/sgi-mgras-gpu.h"

enum {
    SURF_FRONT, SURF_FRONT_FORMAT, SURF_FRONT_ALPHA,
    SURF_BACK, SURF_BACK_FORMAT, SURF_BACK_ALPHA, SURF_SLOT,
    SURFACES,
};

#define SMG_RECTS SGI_CLIP_MAX
/* Submissions the GPU may be behind by before the board's thread waits. */
#define SMG_INFLIGHT 32
/* Triangle records a surface holds before the batch goes out. */
#define SMG_BATCH 256
QEMU_BUILD_BUG_ON(SMG_RECTS > SMG_BATCH);

typedef struct SmgBatch {
    SgiGpuFillTri tris[SURFACES][SMG_BATCH];
    unsigned n[SURFACES];
    bool pending;
    QemuMutex lock;     /* queue, fences and submission, from any thread */
    uint64_t fences[SMG_INFLIGHT];
    unsigned next_fence;
    int64_t busy_since; /* when the status register first read busy, or 0 */
} SmgBatch;
/* Far enough inside int32 that the kernel's edge differences cannot wrap. */
#define SMG_COORD_LIMIT (1 << 29)

/* The bytes or words of surface b, as the board's arrays hold them. */
static void smg_row(SGIMgrasState *s, unsigned b, uint32_t mem_row,
                    uint32_t *words, bool pack)
{
    size_t at = (size_t)mem_row * s->width;
    uint32_t *w32 = NULL;
    uint8_t *w8 = NULL;

    switch (b) {
    case SURF_FRONT:
        w32 = s->front;
        break;
    case SURF_BACK:
        w32 = s->back;
        break;
    case SURF_SLOT:
        w32 = s->slot[0];
        break;
    case SURF_FRONT_FORMAT:
        w8 = s->format;
        break;
    case SURF_BACK_FORMAT:
        w8 = s->back_format;
        break;
    case SURF_FRONT_ALPHA:
        w8 = s->front_alpha;
        break;
    default:
        w8 = s->back_alpha;
        break;
    }
    for (uint32_t x = 0; x < s->width; x++) {
        if (pack) {
            words[x] = w32 ? w32[at + x] : w8[at + x];
        } else if (w32) {
            w32[at + x] = words[x];
        } else {
            w8[at + x] = words[x];
        }
    }
}

static void smg_pack(void *opaque, unsigned surface, uint32_t y,
                     uint32_t rows, uint32_t *words)
{
    SGIMgrasState *s = opaque;

    for (uint32_t r = 0; r < rows; r++) {
        smg_row(s, surface, s->height - 1 - (y + r), words + (size_t)r *
                s->width, true);
    }
}

static void smg_unpack(void *opaque, unsigned surface, uint32_t y,
                       uint32_t rows, const uint32_t *words)
{
    SGIMgrasState *s = opaque;

    for (uint32_t r = 0; r < rows; r++) {
        smg_row(s, surface, s->height - 1 - (y + r), (uint32_t *)words +
                (size_t)r * s->width, false);
    }
}

static const SgiBoardGpuOps smg_ops = {
    .pack = smg_pack,
    .unpack = smg_unpack,
};

bool sgi_mgras_gpu_realize(SGIMgrasState *s, Error **errp)
{
    if (s->gl_renderer_opt_in) {
        error_setg(errp, "x-gpu and x-mesa-renderer are exclusive");
        return false;
    }
    s->gpu = sgi_board_gpu_new("sgi-mgras", SURFACES, s->width, s->height,
                               s->gpu_min_pixels, &smg_ops, s, errp);
    if (!s->gpu) {
        return false;
    }
    /* The banks are reached by pointer, so callers ask for rows themselves. */
    for (unsigned b = 0; b < SURFACES; b++) {
        sgi_board_gpu_attach(s->gpu, &s->gpu_mem, b, b);
    }
    s->gpu_batch = g_new0(SmgBatch, 1);
    qemu_mutex_init(&s->gpu_batch->lock);
    return true;
}

void sgi_mgras_gpu_cleanup(SGIMgrasState *s)
{
    sgi_board_gpu_free(s->gpu);
    s->gpu = NULL;
    if (s->gpu_batch) {
        qemu_mutex_destroy(&s->gpu_batch->lock);
    }
    g_free(s->gpu_batch);
    s->gpu_batch = NULL;
}

/* Keep the queue bounded: wait for the submission SMG_INFLIGHT back. */
static void smg_pace_locked(SGIMgrasState *s)
{
    SmgBatch *p = s->gpu_batch;
    uint64_t old = p->fences[p->next_fence];

    if (old) {
        /* A lost device is found by the next sgi_board_gpu_usable(). */
        sgi_gpu_fence_wait(s->gpu->gpu, old);
    }
    p->fences[p->next_fence] = s->gpu->fence;
    p->next_fence = (p->next_fence + 1) % SMG_INFLIGHT;
}

/* Submit the queued triangles; the caller holds the batch lock. */
static void smg_flush_locked(SGIMgrasState *s)
{
    SmgBatch *p = s->gpu_batch;
    SgiBoardGpuTris jobs[SURFACES];
    unsigned njobs = 0;

    if (!qatomic_read(&p->pending)) {
        return;
    }
    for (unsigned b = 0; b < SURFACES; b++) {
        if (p->n[b]) {
            jobs[njobs++] = (SgiBoardGpuTris) { b, p->tris[b], p->n[b] };
        }
    }
    sgi_board_gpu_tris(s->gpu, jobs, njobs, 8);
    memset(p->n, 0, sizeof(p->n));
    qatomic_set(&p->pending, false);
    smg_pace_locked(s);
}

/*
 * Any thread about to touch a pixel, including a scalar draw's band workers,
 * first has the queued triangles submitted, so the rows' owner is current.
 */
static void smg_flush(SGIMgrasState *s)
{
    SmgBatch *p = s->gpu_batch;

    if (!p || !qatomic_read(&p->pending)) {
        return;
    }
    qemu_mutex_lock(&p->lock);
    smg_flush_locked(s);
    qemu_mutex_unlock(&p->lock);
}

/* Draws are queued or running on the GPU: the status register reads busy. */
bool sgi_mgras_gpu_busy(SGIMgrasState *s)
{
    SgiBoardGpu *g = s->gpu;

    if (!g || !s->gpu_batch || sgi_gpu_lost(g->gpu)) {
        return false;
    }
    return qatomic_read(&s->gpu_batch->pending) ||
           !sgi_gpu_fence_done(g->gpu, qatomic_read(&g->fence));
}

/*
 * The status register's view: busy for at most x-test-gpu-busy-cap-ms of
 * continuous GPU work, as for CPU work, since the kernel gives up on the
 * idle bit after seconds. Pixel accesses wait for their rows regardless.
 */
bool sgi_mgras_gpu_status_busy(SGIMgrasState *s)
{
    SmgBatch *p = s->gpu_batch;
    int64_t now;

    if (!sgi_mgras_gpu_busy(s)) {
        if (p) {
            p->busy_since = 0;
        }
        return false;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (!p->busy_since) {
        p->busy_since = now;
    }
    return now - p->busy_since < (int64_t)s->gpu_busy_cap_ms * SCALE_MS;
}

/* The end of a slice: the thread has the big lock, which the GPU work drops. */
void sgi_mgras_gpu_flush(SGIMgrasState *s)
{
    if (!s->gpu_batch || !qatomic_read(&s->gpu_batch->pending)) {
        return;
    }
    if (sgi_head_work_may_unlock(&s->work) && !s->work.drawing) {
        sgi_head_work_unlock(&s->work);
        smg_flush(s);
        sgi_head_work_relock(&s->work);
    } else {
        smg_flush(s);
    }
}

/* The surfaces a colour target names: bank 1's include the slot's word. */
static unsigned smg_surfaces(SGIColourTarget target)
{
    return (target & 1 ? 7u : 0) | (target & 2 ? 0x78u : 0) |
           (target & 4 ? 0x40u : 0);
}

/* The CPU is about to read or write the target's pixel at GL row y. */
void sgi_mgras_gpu_rows(SGIMgrasState *s, SGIColourTarget target, int y,
                        bool write)
{
    const SGIBoardMemoryGpu *h = &s->gpu->hook;
    unsigned set = smg_surfaces(target);

    smg_flush(s);
    if ((unsigned)y >= s->height) {
        return;
    }
    for (unsigned b = 0; b < SURFACES; b++) {
        uint8_t state;

        if (!(set & (1u << b))) {
            continue;
        }
        state = qatomic_read(&h->rows[b][y]);
        if (state == SGI_ROWS_GPU || (write && state != SGI_ROWS_CPU)) {
            h->cpu_access(h->opaque, b, y, write);
        }
    }
}

/* Everything the CPU might touch: bring the GPU's rows down and forget them. */
void sgi_mgras_gpu_cpu_all(SGIMgrasState *s)
{
    smg_flush(s);
    sgi_board_gpu_sync_all(s->gpu);
    sgi_board_gpu_reset(s->gpu);
}

void sgi_mgras_gpu_sync_all(SGIMgrasState *s)
{
    smg_flush(s);
    sgi_board_gpu_sync_all(s->gpu);
}

typedef struct SmgBox {
    int32_t x0, y0, x1, y1;
} SmgBox;

static bool smg_and(SmgBox *r, int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
    r->x0 = MAX(r->x0, x0);
    r->y0 = MAX(r->y0, y0);
    r->x1 = MIN(r->x1, x1);
    r->y1 = MIN(r->y1, y1);
    return r->x0 <= r->x1 && r->y0 <= r->y1;
}

/* The plane rectangles a box may reach: within the plane and the clip list. */
static unsigned smg_rects(SGIMgrasState *s, const SmgBox *box,
                          const SGIClipList *clip, SmgBox *out)
{
    unsigned n = 0;

    for (unsigned j = 0; j < (clip ? MIN(clip->n, SMG_RECTS) : 1); j++) {
        SmgBox r = *box;

        if (smg_and(&r, 0, 0, (int64_t)s->width - 1, (int64_t)s->height - 1) &&
            (!clip || smg_and(&r, clip->r[j].x0, clip->r[j].y0,
                              clip->r[j].x1, clip->r[j].y1))) {
            out[n++] = r;
        }
    }
    return n;
}

/* Only the two colour banks: not the deep slots, the overlay or a mask. */
static bool smg_targets(const SGIColourTarget *targets, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (targets[i] != 1 && targets[i] != 2) {
            return false;
        }
    }
    return n != 0;
}

/* One job's words for every surface the target writes. */
typedef struct SmgWords {
    uint32_t value[SURFACES];
    uint32_t mask[SURFACES];
} SmgWords;

static void smg_words(SGIColourTarget target, const SGIRasterColour *c,
                      SmgWords *w)
{
    uint32_t rgb = c->component[0] << 16 | c->component[1] << 8 |
                   c->component[2];
    unsigned first = target == 1 ? SURF_FRONT : SURF_BACK;

    w->value[first] = rgb;
    w->value[first + 1] = SGI_MGRAS_PIXEL_FORMAT_RGB;
    w->value[first + 2] = c->component[3];
    for (unsigned i = 0; i < 3; i++) {
        w->mask[first + i] = UINT32_MAX;
    }
    if (target == 2) {
        w->value[SURF_SLOT] = 0;
        w->mask[SURF_SLOT] = SGI_MGRAS_SLOT_WRITTEN;
    }
}

static bool smg_triangle(void *opaque, const SGIRasterGpuTriangle *tri,
                         bool draw)
{
    SGIMgrasState *s = opaque;
    SmgBatch *p = s->gpu_batch;
    SmgBox rects[SMG_RECTS], box;
    int64_t x[3], y[3];
    unsigned n;

    for (int i = 0; i < 3; i++) {
        x[i] = tri->x[i] + (int64_t)tri->ox * 256;
        y[i] = tri->y[i] + (int64_t)tri->oy * 256;
        if (x[i] > SMG_COORD_LIMIT || x[i] < -SMG_COORD_LIMIT ||
            y[i] > SMG_COORD_LIMIT || y[i] < -SMG_COORD_LIMIT) {
            return false;
        }
    }
    if (!smg_targets(tri->targets, tri->ntargets)) {
        return false;
    }
    if (!draw) {
        return true;
    }
    box = (SmgBox) { tri->x0 + tri->ox, tri->y0 + tri->oy,
                     tri->x1 + tri->ox, tri->y1 + tri->oy };
    n = smg_rects(s, &box, tri->clip, rects);
    if (!n) {
        return true;
    }
    qemu_mutex_lock(&p->lock);
    for (unsigned t = 0; t < tri->ntargets; t++) {
        unsigned first = tri->targets[t] == 1 ? SURF_FRONT : SURF_BACK;

        if (p->n[first] + n > SMG_BATCH) {
            smg_flush_locked(s);
            break;
        }
    }
    for (unsigned t = 0; t < tri->ntargets; t++) {
        SmgWords w = { 0 };
        unsigned first = tri->targets[t] == 1 ? SURF_FRONT : SURF_BACK;
        unsigned count = tri->targets[t] == 1 ? 3 : 4;

        smg_words(tri->targets[t], &tri->colour, &w);
        for (unsigned b = first; b < first + count; b++) {
            for (unsigned i = 0; i < n; i++) {
                p->tris[b][p->n[b]++] = (SgiGpuFillTri) {
                    .x = { x[0], x[1], x[2] }, .y = { y[0], y[1], y[2] },
                    .word = w.value[b], .write_mask = w.mask[b],
                    .clip_x0 = rects[i].x0, .clip_y0 = rects[i].y0,
                    .clip_x1 = rects[i].x1, .clip_y1 = rects[i].y1,
                };
            }
        }
    }
    qatomic_set(&p->pending, true);
    qemu_mutex_unlock(&p->lock);
    return true;
}

static bool smg_clear(void *opaque, const SGIRasterOp *op,
                      const SGIRasterColour *colour, uint32_t rgba_mask)
{
    SGIMgrasState *s = opaque;
    SgiGpuFillClear recs[SURFACES][SMG_RECTS];
    SgiBoardGpuClears jobs[SGI_GL_MAX_TARGETS * SURFACES];
    SmgBox rects[SMG_RECTS], box = { op->x0, op->y0, op->x1, op->y1 };
    unsigned n, njobs = 0;

    smg_flush(s);
    if ((int64_t)(op->x1 - op->x0 + 1) * (op->y1 - op->y0 + 1) <
        (int64_t)s->gpu_min_pixels || op->kind != SGI_RASTER_CLEAR ||
        op->index_bits || rgba_mask != 0xf ||
        !smg_targets(op->targets, op->ntargets)) {
        return false;
    }
    n = smg_rects(s, &box, op->clip.n > 1 ? &op->clip : NULL, rects);
    if (!n) {
        return true;
    }
    for (unsigned t = 0; t < op->ntargets; t++) {
        SmgWords w = { 0 };
        unsigned first = op->targets[t] == 1 ? SURF_FRONT : SURF_BACK;
        unsigned count = op->targets[t] == 1 ? 3 : 4;

        smg_words(op->targets[t], colour, &w);
        for (unsigned b = first; b < first + count; b++) {
            for (unsigned i = 0; i < n; i++) {
                recs[b][i] = (SgiGpuFillClear) {
                    .x0 = rects[i].x0, .y0 = rects[i].y0,
                    .x1 = rects[i].x1, .y1 = rects[i].y1,
                    .value = w.value[b], .write_mask = w.mask[b],
                };
            }
            jobs[njobs++] = (SgiBoardGpuClears) { b, recs[b], n };
        }
    }
    sgi_board_gpu_clears(s->gpu, jobs, njobs);
    qemu_mutex_lock(&s->gpu_batch->lock);
    smg_pace_locked(s);
    qemu_mutex_unlock(&s->gpu_batch->lock);
    return true;
}

void sgi_mgras_gpu_target(SGIMgrasState *s, SGIRasterTarget *t)
{
    /* Only in a slice off the big lock; clip-ID stores are per pixel. */
    if (!s->gpu || !s->work.drawing || !sgi_head_work_may_unlock(&s->work) ||
        sgi_mgras_gl_clip_id_active(s) || !sgi_board_gpu_usable(s->gpu)) {
        return;
    }
    t->gpu_triangle = smg_triangle;
    t->gpu_clear = smg_clear;
    t->gpu_min_pixels = s->gpu_min_pixels;
}

/* x-gpu's state and counters, read-only host diagnostics. */
static void smg_get_ready(Object *obj, Visitor *v, const char *name,
                          void *opaque, Error **errp)
{
    SgiBoardGpu *g = SGI_MGRAS(obj)->gpu;
    bool value = g && sgi_gpu_ready(g->gpu);

    visit_type_bool(v, name, &value, errp);
}

static void smg_get_busy(Object *obj, Visitor *v, const char *name,
                         void *opaque, Error **errp)
{
    bool value = sgi_mgras_gpu_busy(SGI_MGRAS(obj));

    visit_type_bool(v, name, &value, errp);
}

static void smg_get_counter(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    SgiBoardGpu *g = SGI_MGRAS(obj)->gpu;
    uint64_t value = 0;

    if (g) {
        switch ((uintptr_t)opaque) {
        case 0:
            value = qatomic_read(&g->switches);
            break;
        case 1:
            value = qatomic_read(&g->draws) + qatomic_read(&g->clears);
            break;
        default:
            value = qatomic_read(&g->readback_bytes);
            break;
        }
    }
    visit_type_uint64(v, name, &value, errp);
}

/* Whether this binary has the GPU back end at all, for tests to skip on. */
static void smg_get_built(Object *obj, Visitor *v, const char *name,
                          void *opaque, Error **errp)
{
#ifdef CONFIG_HAVE_SGI_GPU
    bool value = true;
#else
    bool value = false;
#endif

    visit_type_bool(v, name, &value, errp);
}

void sgi_mgras_gpu_instance_init(Object *obj)
{
    object_property_add(obj, "x-gpu-built", "bool", NULL, smg_get_built, NULL,
                        NULL, NULL);
    object_property_add(obj, "x-gpu-ready", "bool", NULL, smg_get_ready, NULL,
                        NULL, NULL);
    object_property_add(obj, "x-gpu-busy", "bool", NULL, smg_get_busy, NULL,
                        NULL, NULL);
    object_property_add(obj, "x-gpu-switches", "uint64", NULL,
                        smg_get_counter, NULL, NULL, (void *)(uintptr_t)0);
    object_property_add(obj, "x-gpu-draws", "uint64", NULL, smg_get_counter,
                        NULL, NULL, (void *)(uintptr_t)1);
    object_property_add(obj, "x-gpu-readback-bytes", "uint64", NULL,
                        smg_get_counter, NULL, NULL, (void *)(uintptr_t)2);
}
