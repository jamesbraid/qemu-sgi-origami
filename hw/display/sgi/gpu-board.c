/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * GPU planes and per-row ownership for boards that draw flat fills on the
 * host GPU; the row states are in hw/display/sgi/board-memory.h.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "system/runstate.h"
#include "hw/display/sgi-gpu-board.h"
#include "trace.h"

/* How long realize waits for the GPU to open before it carries on. */
#define SBG_OPEN_WAIT_MS 30000
/* How long shutdown waits for an open that realize gave up on. */
#define SBG_EXIT_WAIT_MS 60000

/* Rows a download takes beyond the one asked for, if the GPU holds them. */
#define SBG_READ_AHEAD 64

static void sbg_lost(SgiBoardGpu *g)
{
    static bool stopped;
    const char *why = g->gpu ? sgi_gpu_lost(g->gpu) : NULL;
    const char *adapter = g->gpu ? sgi_gpu_adapter_name(g->gpu) : NULL;

    g->lost = true;
    if (qatomic_xchg(&stopped, true)) {
        return;
    }
    error_report("%s: the GPU (%s) failed%s%s; its rows are gone", g->who,
                 adapter ? adapter : "unknown adapter", why ? ": " : "",
                 why ? why : "");
    qemu_system_vmstop_request(RUN_STATE_INTERNAL_ERROR);
}

static void sbg_wait_open(SgiBoardGpu *g, unsigned ms)
{
    int64_t deadline = g_get_monotonic_time() + ms * G_TIME_SPAN_MILLISECOND;

    while (!sgi_gpu_ready(g->gpu) && !sgi_gpu_lost(g->gpu) &&
           g_get_monotonic_time() < deadline) {
        g_usleep(10 * 1000);
    }
}

/* Exiting while the driver is still opening on its worker can crash. */
static void sbg_shutdown(Notifier *n, void *data)
{
    sbg_wait_open(container_of(n, SgiBoardGpu, shutdown), SBG_EXIT_WAIT_MS);
}

SgiBoardGpu *sgi_board_gpu_new(const char *who, unsigned surfaces,
                               uint32_t width, uint32_t height,
                               uint32_t min_pixels, const SgiBoardGpuOps *ops,
                               void *opaque, Error **errp)
{
    SgiBoardGpu *g = g_new0(SgiBoardGpu, 1);

    g_assert(surfaces && surfaces <= SGI_BOARD_GPU_SURFACES);
    g->who = who;
    g->nsurfaces = surfaces;
    g->ops = ops;
    g->opaque = opaque;
    g->min_pixels = min_pixels;
    g->gpu = sgi_gpu_open();
    if (!g->gpu) {
        error_setg(errp, "x-gpu: no GPU is available (QEMU was built "
                   "without the GPU back end or no adapter opened)");
        g_free(g);
        return NULL;
    }
    sbg_wait_open(g, SBG_OPEN_WAIT_MS);
    g->shutdown.notify = sbg_shutdown;
    qemu_register_shutdown_notifier(&g->shutdown);
    g->hook.opaque = g;
    qemu_mutex_init(&g->lock);
    sgi_board_gpu_resize(g, width, height);
    return g;
}

static void sbg_drop_planes(SgiBoardGpu *g)
{
    for (unsigned b = 0; b < g->nsurfaces; b++) {
        if (g->plane[b]) {
            sgi_gpu_plane_free(g->plane[b]);
            g->plane[b] = NULL;
        }
    }
}

void sgi_board_gpu_free(SgiBoardGpu *g)
{
    if (!g) {
        return;
    }
    notifier_remove(&g->shutdown);
    sbg_drop_planes(g);
    for (unsigned b = 0; b < g->nsurfaces; b++) {
        g_free(g->rows[b]);
    }
    qemu_mutex_destroy(&g->lock);
    g_free(g->stage);
    g_free(g);
}

void sgi_board_gpu_resize(SgiBoardGpu *g, uint32_t width, uint32_t height)
{
    qemu_mutex_lock(&g->lock);
    sbg_drop_planes(g);
    g->width = width;
    g->height = height;
    for (unsigned p = 0; p < SGI_PLANES_MAX; p++) {
        if (g->hook.rows[p]) {
            g->hook.height[p] = height;
        }
    }
    for (unsigned b = 0; b < g->nsurfaces; b++) {
        /* The accessors read the old array until the new one is in place. */
        uint8_t *rows = g_malloc0(height);
        uint8_t *old = g->rows[b];

        g->rows[b] = rows;
        for (unsigned p = 0; old && p < SGI_PLANES_MAX; p++) {
            if (g->hook.rows[p] == old) {
                g->hook.rows[p] = rows;
            }
        }
        g_free(old);
    }
    g_free(g->stage);
    g->stage = g_new(uint32_t, (size_t)width * height);
    qemu_mutex_unlock(&g->lock);
}

static void sbg_cpu_access(void *opaque, uint32_t plane, uint32_t y,
                           bool write);

void sgi_board_gpu_attach(SgiBoardGpu *g, SGIBoardMemory *m, uint32_t id,
                          unsigned surface)
{
    g_assert(surface < g->nsurfaces && id < SGI_PLANES_MAX);
    g->surface_of[id] = surface;
    g->hook.rows[id] = g->rows[surface];
    g->hook.height[id] = g->height;
    g->hook.cpu_access = sbg_cpu_access;
    m->gpu = &g->hook;
}

bool sgi_board_gpu_usable(SgiBoardGpu *g)
{
    if (g->lost || !sgi_gpu_ready(g->gpu)) {
        return false;
    }
    if (sgi_gpu_lost(g->gpu)) {
        sbg_lost(g);
        return false;
    }
    qemu_mutex_lock(&g->lock);
    for (unsigned b = 0; b < g->nsurfaces; b++) {
        if (!g->plane[b]) {
            g->plane[b] = sgi_gpu_plane_new(g->gpu, g->width, g->height);
        }
        if (!g->plane[b]) {
            qemu_mutex_unlock(&g->lock);
            sbg_lost(g);
            return false;
        }
    }
    qemu_mutex_unlock(&g->lock);
    return true;
}

static void sbg_set(SgiBoardGpu *g, unsigned b, uint32_t y0, uint32_t y1,
                    uint8_t state)
{
    for (uint32_t y = y0; y <= y1; y++) {
        qatomic_set(&g->rows[b][y], state);
    }
}

/* Download rows y0..y1 of surface b, all in the GPU state; the lock is held. */
static void sbg_download(SgiBoardGpu *g, unsigned b, uint32_t y0, uint32_t y1)
{
    uint32_t n = y1 - y0 + 1;

    if (g->lost || !g->plane[b] ||
        !sgi_gpu_plane_read(g->plane[b], y0, n, g->stage, g->width)) {
        /* The machine is stopping; the rows keep the CPU's last copy. */
        sbg_lost(g);
        sbg_set(g, b, y0, y1, SGI_ROWS_CPU);
        return;
    }
    g->ops->unpack(g->opaque, b, y0, n, g->stage);
    sbg_set(g, b, y0, y1, SGI_ROWS_BOTH);
    g->switches++;
    g->readback_bytes += (uint64_t)n * g->width * 4;
    trace_sgi_board_gpu_rows(g->who, "download", b, y0, n);
}

static void sbg_cpu_access(void *opaque, uint32_t plane, uint32_t y,
                           bool write)
{
    SgiBoardGpu *g = opaque;
    unsigned b = g->surface_of[plane];

    qemu_mutex_lock(&g->lock);
    if (y < g->height) {
        if (g->rows[b][y] == SGI_ROWS_GPU) {
            uint32_t end = y;

            /* The rest of this run and some ahead: sweeps read in order. */
            while (end + 1 < g->height && end + 1 - y < SBG_READ_AHEAD &&
                   g->rows[b][end + 1] == SGI_ROWS_GPU) {
                end++;
            }
            sbg_download(g, b, y, end);
        }
        if (write) {
            sbg_set(g, b, y, y, SGI_ROWS_CPU);
        }
    }
    qemu_mutex_unlock(&g->lock);
}

void sgi_board_gpu_sync_all(SgiBoardGpu *g)
{
    if (!g) {
        return;
    }
    qemu_mutex_lock(&g->lock);
    for (unsigned b = 0; b < g->nsurfaces; b++) {
        for (uint32_t y = 0; y < g->height; y++) {
            if (g->rows[b][y] == SGI_ROWS_GPU) {
                uint32_t end = y;

                while (end + 1 < g->height &&
                       g->rows[b][end + 1] == SGI_ROWS_GPU) {
                    end++;
                }
                sbg_download(g, b, y, end);
                y = end;
            }
        }
    }
    qemu_mutex_unlock(&g->lock);
}

void sgi_board_gpu_reset(SgiBoardGpu *g)
{
    if (!g) {
        return;
    }
    qemu_mutex_lock(&g->lock);
    for (unsigned b = 0; b < g->nsurfaces; b++) {
        sbg_set(g, b, 0, g->height - 1, SGI_ROWS_CPU);
    }
    qemu_mutex_unlock(&g->lock);
}

/* Upload rows y0..y1 of surface b that only the CPU holds; the lock is held. */
static bool sbg_upload(SgiBoardGpu *g, unsigned b, uint32_t y0, uint32_t y1)
{
    uint32_t y = y0;

    while (y <= y1) {
        uint32_t end;

        if (g->rows[b][y] != SGI_ROWS_CPU) {
            y++;
            continue;
        }
        end = y;
        while (end + 1 <= y1 && g->rows[b][end + 1] == SGI_ROWS_CPU) {
            end++;
        }
        g->ops->pack(g->opaque, b, y, end - y + 1, g->stage);
        if (!sgi_gpu_plane_write(g->plane[b], y, end - y + 1, g->stage,
                                 g->width)) {
            sbg_lost(g);
            return false;
        }
        sbg_set(g, b, y, end, SGI_ROWS_BOTH);
        g->switches++;
        g->upload_bytes += (uint64_t)(end - y + 1) * g->width * 4;
        trace_sgi_board_gpu_rows(g->who, "upload", b, y, end - y + 1);
        y = end + 1;
    }
    return true;
}

bool sgi_board_gpu_tris(SgiBoardGpu *g, const SgiBoardGpuTris *jobs,
                        unsigned njobs, uint32_t sub_bits)
{
    bool ok = true;
    uint32_t count = 0;

    qemu_mutex_lock(&g->lock);
    for (unsigned j = 0; ok && j < njobs; j++) {
        uint32_t y0 = UINT32_MAX, y1 = 0;

        for (unsigned i = 0; i < jobs[j].count; i++) {
            y0 = MIN(y0, jobs[j].tris[i].clip_y0);
            y1 = MAX(y1, jobs[j].tris[i].clip_y1);
        }
        if (!jobs[j].count) {
            continue;
        }
        ok = sbg_upload(g, jobs[j].surface, y0, y1);
        if (ok) {
            uint64_t fence = sgi_gpu_fill_tris(g->plane[jobs[j].surface],
                                               sub_bits, jobs[j].tris,
                                               jobs[j].count);

            qatomic_set(&g->fence, fence);
            ok = fence != 0;
        }
        if (!ok) {
            sbg_lost(g);
            break;
        }
        sbg_set(g, jobs[j].surface, y0, y1, SGI_ROWS_GPU);
        count += jobs[j].count;
    }
    g->draws++;
    qemu_mutex_unlock(&g->lock);
    trace_sgi_board_gpu_draw(g->who, "triangle", count);
    return ok;
}

bool sgi_board_gpu_clears(SgiBoardGpu *g, const SgiBoardGpuClears *jobs,
                          unsigned njobs)
{
    bool ok = true;
    uint32_t count = 0;

    qemu_mutex_lock(&g->lock);
    for (unsigned j = 0; ok && j < njobs; j++) {
        uint32_t y0 = UINT32_MAX, y1 = 0;

        for (unsigned i = 0; i < jobs[j].count; i++) {
            y0 = MIN(y0, jobs[j].clears[i].y0);
            y1 = MAX(y1, jobs[j].clears[i].y1);
        }
        if (!jobs[j].count) {
            continue;
        }
        ok = sbg_upload(g, jobs[j].surface, y0, y1);
        for (unsigned i = 0; ok && i < jobs[j].count; i++) {
            uint64_t fence = sgi_gpu_fill_clear(g->plane[jobs[j].surface],
                                                &jobs[j].clears[i]);

            qatomic_set(&g->fence, fence);
            ok = fence != 0;
        }
        if (!ok) {
            sbg_lost(g);
            break;
        }
        sbg_set(g, jobs[j].surface, y0, y1, SGI_ROWS_GPU);
        count += jobs[j].count;
    }
    g->clears++;
    qemu_mutex_unlock(&g->lock);
    trace_sgi_board_gpu_draw(g->who, "clear", count);
    return ok;
}
