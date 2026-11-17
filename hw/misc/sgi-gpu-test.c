/*
 * Test device for the SGI GPU back end: writes planes and reads them back.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/display/sgi-gpu.h"
#include "qapi/error.h"
#include "qemu/thread.h"

#define TYPE_X_SGI_GPU_TEST "x-sgi-gpu-test"
OBJECT_DECLARE_SIMPLE_TYPE(SgiGpuTest, X_SGI_GPU_TEST)

struct SgiGpuTest {
    DeviceState parent_obj;
    SgiGpu *gpu;
    uint32_t width;
    uint32_t height;
    QemuMutex lock;
    QemuThread thread;
    bool running;
    char result[160];
};

static uint32_t pattern(uint32_t seed, uint32_t x, uint32_t y, uint32_t w)
{
    return (y * w + x + 1) * 0x9e3779b1u ^ seed;
}

static void set_result(SgiGpuTest *s, const char *msg)
{
    qemu_mutex_lock(&s->lock);
    g_strlcpy(s->result, msg, sizeof(s->result));
    qemu_mutex_unlock(&s->lock);
}

/* Fills @rows rows at @y of @words (@stride apart) with the pattern. */
static void fill(uint32_t *words, size_t stride, uint32_t seed,
                 uint32_t w, uint32_t y, uint32_t rows)
{
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t x = 0; x < w; x++) {
            words[r * stride + x] = pattern(seed, x, y + r, w);
        }
    }
}

static bool check(SgiGpuTest *s, const uint32_t *words, size_t stride,
                  uint32_t seed, uint32_t y, uint32_t rows, const char *what)
{
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t x = 0; x < s->width; x++) {
            uint32_t want = seed == 0 ? 0 : pattern(seed, x, y + r, s->width);
            uint32_t got = words[r * stride + x];

            if (got != want) {
                char msg[sizeof(s->result)];

                snprintf(msg, sizeof(msg),
                         "fail: %s at (%u,%u): got 0x%08x want 0x%08x",
                         what, x, y + r, got, want);
                set_result(s, msg);
                return false;
            }
        }
    }
    return true;
}

static bool fail(SgiGpuTest *s, const char *msg)
{
    char buf[sizeof(s->result)];

    snprintf(buf, sizeof(buf), "fail: %s", msg);
    set_result(s, buf);
    return false;
}

static bool run_checks(SgiGpuTest *s)
{
    const uint32_t w = s->width, h = s->height;
    const size_t wstride = w + 3, rstride = w + 5;
    uint32_t *src = g_new0(uint32_t, wstride * h);
    uint32_t *dst = g_new0(uint32_t, rstride * h);
    SgiGpuPlane *p = sgi_gpu_plane_new(s->gpu, w, h);
    bool ok = false;

    if (!p) {
        fail(s, "plane_new returned NULL");
        goto out;
    }

    /* A new plane is zero. */
    if (!sgi_gpu_plane_read(p, 0, h, dst, rstride)) {
        fail(s, "read of a new plane failed");
        goto out;
    }
    if (!check(s, dst, rstride, 0, 0, h, "new plane")) {
        goto out;
    }

    /* Upper half through a padded stride, lower half packed. */
    fill(src, wstride, 1, w, 0, h / 2);
    if (!sgi_gpu_plane_write(p, 0, h / 2, src, wstride)) {
        fail(s, "write of the upper half failed");
        goto out;
    }
    fill(src, w, 1, w, h / 2, h - h / 2);
    if (!sgi_gpu_plane_write(p, h / 2, h - h / 2, src, w)) {
        fail(s, "write of the lower half failed");
        goto out;
    }
    memset(dst, 0, sizeof(uint32_t) * rstride * h);
    if (!sgi_gpu_plane_read(p, 0, h, dst, rstride)) {
        fail(s, "read failed");
        goto out;
    }
    if (!check(s, dst, rstride, 1, 0, h, "whole plane")) {
        goto out;
    }

    /* A band rewrite leaves its neighbours alone. */
    fill(src, w, 2, w, 2, 2);
    if (!sgi_gpu_plane_write(p, 2, 2, src, w)) {
        fail(s, "band write failed");
        goto out;
    }
    if (!sgi_gpu_plane_read(p, 1, 4, dst, rstride)) {
        fail(s, "band read failed");
        goto out;
    }
    if (!check(s, dst, rstride, 1, 1, 1, "row above band") ||
        !check(s, dst + rstride, rstride, 2, 2, 2, "band") ||
        !check(s, dst + 3 * rstride, rstride, 1, 4, 1, "row below band")) {
        goto out;
    }

    /* Bad ranges and fences. */
    if (sgi_gpu_plane_write(p, h, 1, src, w) ||
        sgi_gpu_plane_read(p, h - 1, 2, dst, rstride)) {
        fail(s, "an out-of-range band was accepted");
        goto out;
    }
    if (!sgi_gpu_fence_done(s->gpu, 0) || !sgi_gpu_fence_wait(s->gpu, 0)) {
        fail(s, "fence 0 is not done");
        goto out;
    }
    if (sgi_gpu_fence_done(s->gpu, UINT64_MAX) ||
        sgi_gpu_fence_wait(s->gpu, UINT64_MAX)) {
        fail(s, "an unissued fence is done");
        goto out;
    }
    if (sgi_gpu_lost(s->gpu)) {
        fail(s, sgi_gpu_lost(s->gpu));
        goto out;
    }
    ok = true;
out:
    sgi_gpu_plane_free(p);
    g_free(src);
    g_free(dst);
    return ok;
}

static void *test_thread(void *opaque)
{
    SgiGpuTest *s = opaque;

    if (run_checks(s)) {
        set_result(s, "ok");
    }
    return NULL;
}

static void set_run(Object *obj, bool value, Error **errp)
{
    SgiGpuTest *s = X_SGI_GPU_TEST(obj);

    if (!value) {
        return;
    }
    if (!sgi_gpu_ready(s->gpu)) {
        error_setg(errp, "the GPU is not ready");
        return;
    }
    if (s->running) {
        error_setg(errp, "a run is already in progress");
        return;
    }
    set_result(s, "running");
    s->running = true;
    qemu_thread_create(&s->thread, "sgi-gpu-test", test_thread, s,
                       QEMU_THREAD_JOINABLE);
}

static char *get_state(Object *obj, Error **errp)
{
    SgiGpuTest *s = X_SGI_GPU_TEST(obj);

    if (!s->gpu) {
        return g_strdup("disabled");
    }
    if (sgi_gpu_lost(s->gpu)) {
        return g_strdup("unavailable");
    }
    return g_strdup(sgi_gpu_ready(s->gpu) ? "ready" : "opening");
}

static char *get_adapter(Object *obj, Error **errp)
{
    SgiGpuTest *s = X_SGI_GPU_TEST(obj);
    const char *name = s->gpu ? sgi_gpu_adapter_name(s->gpu) : NULL;

    return g_strdup(name ? name : "");
}

static char *get_lost(Object *obj, Error **errp)
{
    SgiGpuTest *s = X_SGI_GPU_TEST(obj);
    const char *msg = s->gpu ? sgi_gpu_lost(s->gpu) : NULL;

    return g_strdup(msg ? msg : "");
}

static char *get_result(Object *obj, Error **errp)
{
    SgiGpuTest *s = X_SGI_GPU_TEST(obj);
    char *r;

    qemu_mutex_lock(&s->lock);
    r = g_strdup(s->result);
    qemu_mutex_unlock(&s->lock);
    return r;
}

static void sgi_gpu_test_init(Object *obj)
{
    SgiGpuTest *s = X_SGI_GPU_TEST(obj);

    qemu_mutex_init(&s->lock);
    g_strlcpy(s->result, "idle", sizeof(s->result));
    object_property_add_str(obj, "state", get_state, NULL);
    object_property_add_str(obj, "adapter", get_adapter, NULL);
    object_property_add_str(obj, "lost", get_lost, NULL);
    object_property_add_str(obj, "result", get_result, NULL);
    object_property_add_bool(obj, "x-run", NULL, set_run);
}

static void sgi_gpu_test_realize(DeviceState *dev, Error **errp)
{
    SgiGpuTest *s = X_SGI_GPU_TEST(dev);

    if (!s->width || !s->height || s->width > 4096 || s->height > 4096 ||
        s->height < 6) {
        error_setg(errp, "width must be 1..4096 and height 6..4096");
        return;
    }
    s->gpu = sgi_gpu_open();
}

static void sgi_gpu_test_finalize(Object *obj)
{
    SgiGpuTest *s = X_SGI_GPU_TEST(obj);

    if (s->running) {
        qemu_thread_join(&s->thread);
    }
    qemu_mutex_destroy(&s->lock);
}

static const Property sgi_gpu_test_props[] = {
    DEFINE_PROP_UINT32("width", SgiGpuTest, width, 64),
    DEFINE_PROP_UINT32("height", SgiGpuTest, height, 48),
};

static void sgi_gpu_test_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sgi_gpu_test_realize;
    device_class_set_props(dc, sgi_gpu_test_props);
    dc->user_creatable = true;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo sgi_gpu_test_info = {
    .name = TYPE_X_SGI_GPU_TEST,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SgiGpuTest),
    .instance_init = sgi_gpu_test_init,
    .instance_finalize = sgi_gpu_test_finalize,
    .class_init = sgi_gpu_test_class_init,
};

static void sgi_gpu_test_register(void)
{
    type_register_static(&sgi_gpu_test_info);
}

type_init(sgi_gpu_test_register)
