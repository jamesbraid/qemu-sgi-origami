/*
 * VPro scan-out compose on the GPU: tells the shared scanout how to lend a
 * display-shm viewer the picture odyssey_resolve_row() would have made.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "hw/misc/sgi-odyssey-scanout-gpu.h"
#include "hw/display/sgi-gpu-scanout.h"

#define OGS_FRAMES 32

void odyssey_gpu_stop(SgiGpu *gpu, const char *why)
{
    static bool stopped;
    const char *adapter = gpu ? sgi_gpu_adapter_name(gpu) : NULL;

    if (qatomic_xchg(&stopped, true)) {
        return;
    }
    error_report("sgi-odyssey: the GPU (%s) failed%s%s; its rows are gone",
                 adapter ? adapter : "unknown adapter", why ? ": " : "",
                 why ? why : "");
    qemu_system_vmstop_request(RUN_STATE_INTERNAL_ERROR);
}

typedef struct OgsFrame {
    uint32_t id;
    SgiGpuFrame *frame;
} OgsFrame;

/* One compose, for the verify mode's check on the worker thread. */
typedef struct OgsVerify {
    struct OdysseyGpuScanout *g;
    SgiGpuFrame *frame;
    uint32_t *expect;
    uint32_t width, height;
    void (*done)(void *opaque, bool ok);
    void *done_opaque;
} OgsVerify;

struct OdysseyGpuScanout {
    SGIOdysseyState *s;
    SgiGpu *gpu;
    OdysseyGpuPlanes planes;
    SgiGpuVproScanout *sc;
    SgiGpuVproTables *tables;
    /* The id and overlay planes as the GPU last saw them. */
    uint8_t *did_seen, *overlay_seen;
    OgsFrame frame[OGS_FRAMES];
    QEMUTimer *open_timer;      /* waits for the device to finish opening */
    bool verify;
    unsigned verified, differing;   /* frames compared, words that differed */
};

static OgsFrame *ogs_frame(OdysseyGpuScanout *g, uint32_t id)
{
    for (unsigned i = 0; i < OGS_FRAMES; i++) {
        if (g->frame[i].id == id) {
            return &g->frame[i];
        }
    }
    return NULL;
}

#ifdef _WIN32
/* The viewer's copy of the frame's NT handle and shared fence. */
static bool ogs_lend(const SgiGpuExport *ex, DisplayShmGpuSlot *out)
{
    HANDLE self = GetCurrentProcess(), buffer, fence;

    if (ex->kind != SGI_GPU_EXPORT_WIN32 || ex->fence < 0 ||
        !DuplicateHandle(self, (HANDLE)(intptr_t)ex->handle, self, &buffer, 0,
                         FALSE, DUPLICATE_SAME_ACCESS)) {
        return false;
    }
    if (!DuplicateHandle(self, (HANDLE)(intptr_t)ex->fence, self, &fence, 0,
                         FALSE, DUPLICATE_SAME_ACCESS)) {
        CloseHandle(buffer);
        return false;
    }
    *out = (DisplayShmGpuSlot) {
        .kind = DSHM_GPU_WIN32,
        .width = ex->width,
        .height = ex->height,
        .stride = ex->stride,
        .fourcc = ex->fourcc,
        .handle = (intptr_t)buffer,
        .fence = (intptr_t)fence,
        .adapter_luid = ex->adapter_id,
    };
    return true;
}
#else
static bool ogs_lend(const SgiGpuExport *ex, DisplayShmGpuSlot *out)
{
    int fd;

    if (ex->kind != SGI_GPU_EXPORT_DMABUF) {
        return false;
    }
    fd = qemu_dup(ex->handle);
    if (fd < 0) {
        return false;
    }
    *out = (DisplayShmGpuSlot) {
        .kind = DSHM_GPU_DMABUF,
        .width = ex->width,
        .height = ex->height,
        .stride = ex->stride,
        .fourcc = ex->fourcc,
        .modifier = ex->modifier,
        .handle = fd,
        .fence = -1,
    };
    return true;
}
#endif

static bool ogs_slot_new(void *opaque, uint32_t id, uint32_t width,
                         uint32_t height, DisplayShmGpuSlot *out)
{
    OdysseyGpuScanout *g = opaque;
    OgsFrame *slot = ogs_frame(g, 0);
    SgiGpuExport ex;
    SgiGpuFrame *frame;

    if (!slot || !sgi_gpu_ready(g->gpu)) {
        return false;
    }
    frame = sgi_gpu_frame_new(g->gpu, width, height);
    if (!frame) {
        return false;
    }
    /* No exportable memory from this adapter: the viewer gets shared memory. */
    if (!sgi_gpu_frame_export(frame, &ex) || !ogs_lend(&ex, out)) {
        sgi_gpu_frame_free(frame);
        return false;
    }
    *slot = (OgsFrame) { .id = id, .frame = frame };
    return true;
}

static void ogs_slot_free(void *opaque, uint32_t id)
{
    OgsFrame *slot = ogs_frame(opaque, id);

    if (slot) {
        sgi_gpu_frame_free(slot->frame);
        *slot = (OgsFrame) { };
    }
}

static bool ogs_ready(void *opaque)
{
    OdysseyGpuScanout *g = opaque;

    return sgi_gpu_ready(g->gpu);
}

/* The device opens on a worker thread: tell the viewers when it is up. */
static void ogs_open_poll(void *opaque)
{
    OdysseyGpuScanout *g = opaque;

    if (sgi_gpu_ready(g->gpu)) {
        sgi_scanout_gpu_ready(&g->s->scanout);
    } else if (!sgi_gpu_lost(g->gpu)) {
        timer_mod(g->open_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 20);
    }
}

static bool ogs_usable(void *opaque)
{
    OdysseyGpuScanout *g = opaque;
    SGIOdysseyState *s = g->s;
    SgiGpuPlane *buf0, *buf1;
    const char *lost;

    if (!sgi_gpu_ready(g->gpu)) {
        return false;
    }
    lost = sgi_gpu_lost(g->gpu);
    if (lost) {
        odyssey_gpu_stop(g->gpu, lost);
        return false;
    }
    if (s->scanout.mode.width > SGI_ODYSSEY_WIDTH ||
        s->scanout.mode.height > SGI_ODYSSEY_HEIGHT) {
        return false;
    }
    if (!g->sc) {
        g->sc = sgi_gpu_vpro_scanout_new(g->gpu, SGI_ODYSSEY_WIDTH,
                                         SGI_ODYSSEY_HEIGHT);
    }
    return g->sc && g->planes(s, &buf0, &buf1);
}

static void ogs_tables(OdysseyGpuScanout *g)
{
    SGIOdysseyState *s = g->s;
    SgiGpuVproTables *t = g->tables;

    t->width = MIN(s->scanout.mode.width, SGI_ODYSSEY_WIDTH);
    t->height = MIN(s->scanout.mode.height, SGI_ODYSSEY_HEIGHT);
    t->displayed_buffer = s->displayed_buffer;
    memcpy(t->lut, s->lut, sizeof(t->lut));
    memcpy(t->dispmode, s->dispmode, sizeof(t->dispmode));
    memcpy(t->overlay_mode, s->overlay_mode, sizeof(t->overlay_mode));
    memcpy(t->cursor, s->cursor, sizeof(t->cursor));
}

/* The rows [*first, *last] where the id or overlay plane moved on. */
static bool ogs_changed_rows(OdysseyGpuScanout *g, uint32_t *first,
                             uint32_t *last)
{
    const size_t row = SGI_ODYSSEY_WIDTH;
    bool any = false;

    for (uint32_t y = 0; y < SGI_ODYSSEY_HEIGHT; y++) {
        if (memcmp(g->s->did + y * row, g->did_seen + y * row, row) ||
            memcmp(g->s->overlay + y * row, g->overlay_seen + y * row, row)) {
            if (!any) {
                *first = y;
            }
            *last = y;
            any = true;
        }
    }
    return any;
}

/* Verify mode, on the worker thread: the GPU's frame against the CPU's. */
static void ogs_verified_done(void *opaque, bool ok)
{
    OgsVerify *v = opaque;
    uint32_t w = v->width, h = v->height;
    g_autofree uint32_t *got = g_new(uint32_t, w * h);
    unsigned bad = 0;

    if (ok && sgi_gpu_frame_read(v->frame, got, w)) {
        for (size_t i = 0; i < (size_t)w * h; i++) {
            bad += got[i] != v->expect[i];
        }
        qatomic_inc(&v->g->verified);
        qatomic_add(&v->g->differing, bad);
        if (bad) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: GPU scan-out %ux%u: "
                          "%u words differ from the CPU's\n", w, h, bad);
        }
    }
    v->done(v->done_opaque, ok);
    g_free(v->expect);
    g_free(v);
}

static bool ogs_compose(void *opaque, uint32_t id,
                        void (*done)(void *done_opaque, bool ok),
                        void *done_opaque)
{
    OdysseyGpuScanout *g = opaque;
    SGIOdysseyState *s = g->s;
    OgsFrame *slot = ogs_frame(g, id);
    SgiGpuPlane *buf0, *buf1;
    OgsVerify *v = NULL;
    uint32_t first = 0, last = 0, rows = 0;
    size_t at;
    bool queued;

    if (!slot || !g->sc || !g->planes(s, &buf0, &buf1)) {
        return false;
    }
    ogs_tables(g);
    if (ogs_changed_rows(g, &first, &last)) {
        rows = last - first + 1;
    }
    at = (size_t)first * SGI_ODYSSEY_WIDTH;
    if (g->verify) {
        v = g_new0(OgsVerify, 1);
        v->g = g;
        v->frame = slot->frame;
        v->width = g->tables->width;
        v->height = g->tables->height;
        v->expect = g_new0(uint32_t, (size_t)v->width * v->height);
        for (uint32_t y = 0; y < v->height; y++) {
            s->scanout.ops->resolve_row(s, y, &v->expect[(size_t)y * v->width]);
        }
        v->done = done;
        v->done_opaque = done_opaque;
        done = ogs_verified_done;
        done_opaque = v;
    }
    queued = sgi_gpu_vpro_scanout_compose(g->sc, g->tables, first, rows,
                                          s->did + at, s->overlay + at,
                                          buf0, buf1, slot->frame, done,
                                          done_opaque);
    if (!queued) {
        if (v) {
            g_free(v->expect);
            g_free(v);
        }
        return false;
    }
    memcpy(g->did_seen + at, s->did + at, (size_t)rows * SGI_ODYSSEY_WIDTH);
    memcpy(g->overlay_seen + at, s->overlay + at,
           (size_t)rows * SGI_ODYSSEY_WIDTH);
    return true;
}

static void ogs_quiesce(void *opaque)
{
    OdysseyGpuScanout *g = opaque;

    if (g->sc) {
        sgi_gpu_vpro_scanout_quiesce(g->sc);
    }
}

static const SGIScanoutGpuOps ogs_ops = {
    .slot_new = ogs_slot_new,
    .slot_free = ogs_slot_free,
    .ready = ogs_ready,
    .usable = ogs_usable,
    .compose = ogs_compose,
    .quiesce = ogs_quiesce,
};

OdysseyGpuScanout *odyssey_gpu_scanout_new(SGIOdysseyState *s, SgiGpu *gpu,
                                           OdysseyGpuPlanes planes)
{
    OdysseyGpuScanout *g = g_new0(OdysseyGpuScanout, 1);
    size_t plane_bytes = (size_t)SGI_ODYSSEY_WIDTH * SGI_ODYSSEY_HEIGHT;

    g->s = s;
    g->gpu = gpu;
    g->planes = planes;
    g->tables = g_new0(SgiGpuVproTables, 1);
    g->did_seen = g_malloc0(plane_bytes);
    g->overlay_seen = g_malloc0(plane_bytes);
    g->verify = getenv("SGI_GPU_SCANOUT_VERIFY");
    sgi_scanout_set_gpu(&s->scanout, &ogs_ops, g);
    if (!sgi_gpu_ready(gpu)) {
        g->open_timer = timer_new_ms(QEMU_CLOCK_REALTIME, ogs_open_poll, g);
        timer_mod(g->open_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 20);
    }
    return g;
}

void odyssey_gpu_scanout_free(OdysseyGpuScanout *g)
{
    if (!g) {
        return;
    }
    if (g->open_timer) {
        timer_free(g->open_timer);
    }
    for (unsigned i = 0; i < OGS_FRAMES; i++) {
        if (g->frame[i].id) {
            sgi_gpu_frame_free(g->frame[i].frame);
        }
    }
    sgi_gpu_vpro_scanout_free(g->sc);
    if (g->verify) {
        info_report("sgi-odyssey: GPU scan-out verify: %u frames, %u words "
                    "differ", g->verified, g->differing);
    }
    g_free(g->tables);
    g_free(g->did_seen);
    g_free(g->overlay_seen);
    g_free(g);
}
