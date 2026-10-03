/*
 * Copyright (C) 2015-2016 Gerd Hoffmann <kraxel@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu/osdep.h"

#include "qemu/drm.h"
#include "qemu/thread.h"
#include "qemu/error-report.h"
#include "ui/console.h"
#include "ui/egl-helpers.h"
#include "system/system.h"
#include "qapi/error.h"
#include "trace.h"
#include "standard-headers/drm/drm_fourcc.h"

EGLDisplay qemu_egl_display;
EGLConfig qemu_egl_config;
DisplayGLMode qemu_egl_mode;
bool qemu_egl_angle_d3d;

typedef struct QemuEGLDisplayRecord {
    EGLDisplay display;
    EGLConfig config;
    DisplayGLMode mode;
    EGLContext root;
    QemuThread owner;
    char *rendernode;
    uint64_t generation;
    unsigned references;
    unsigned providers;
    bool gbm;
    bool quarantined;
#ifdef CONFIG_GBM
    struct gbm_device *native;
    int fd;
#endif
} QemuEGLDisplayRecord;

struct QemuEGLDisplayLease {
    QemuEGLDisplayRecord *record;
    unsigned contexts;
    bool provider;
    bool quarantined;
};

static QemuEGLDisplayRecord *egl_record;
static QemuRecMutex egl_record_mutex;
static gsize egl_record_mutex_initialized;
static uint64_t egl_generation;
static bool egl_startup_quarantined;
static EGLContext egl_failed_context;

static void egl_record_lock(void)
{
    if (g_once_init_enter(&egl_record_mutex_initialized)) {
        qemu_rec_mutex_init(&egl_record_mutex);
        g_once_init_leave(&egl_record_mutex_initialized, 1);
    }
    qemu_rec_mutex_lock(&egl_record_mutex);
}

static void egl_record_unlock(void)
{
    qemu_rec_mutex_unlock(&egl_record_mutex);
}

EGLDisplay qemu_egl_display_lease_display(const QemuEGLDisplayLease *lease)
{
    return lease->record->display;
}

EGLConfig qemu_egl_display_lease_config(const QemuEGLDisplayLease *lease)
{
    return lease->record->config;
}

uint64_t qemu_egl_display_lease_generation(const QemuEGLDisplayLease *lease)
{
    return lease->record->generation;
}

bool qemu_egl_display_lease_usable(const QemuEGLDisplayLease *lease)
{
    bool usable;

    egl_record_lock();
    usable = !lease->record->quarantined;
    egl_record_unlock();
    return usable;
}

void qemu_egl_display_context_created(QemuEGLDisplayLease *lease)
{
    egl_record_lock();
    lease->contexts++;
    egl_record_unlock();
}

void qemu_egl_display_context_destroyed(QemuEGLDisplayLease *lease)
{
    egl_record_lock();
    assert(lease->contexts);
    lease->contexts--;
    egl_record_unlock();
}

void qemu_egl_display_quarantine(QemuEGLDisplayLease *lease)
{
    egl_record_lock();
    lease->quarantined = true;
    lease->record->quarantined = true;
    egl_record_unlock();
}

QemuEGLDisplayLease *qemu_egl_display_acquire_existing(DisplayGLMode mode,
                                                     Error **errp)
{
    QemuEGLDisplayLease *lease = NULL;

    egl_record_lock();
    if (!egl_record || !egl_record->gbm || egl_record->quarantined ||
        egl_record->mode != mode) {
        error_setg(errp, "No compatible managed GBM EGL display");
    } else {
        lease = g_try_new0(QemuEGLDisplayLease, 1);
        if (!lease) {
            error_setg(errp, "Cannot allocate EGL display lease");
        } else {
            lease->record = egl_record;
            egl_record->references++;
        }
    }
    egl_record_unlock();
    return lease;
}

bool qemu_egl_display_acquire_context(QemuEGLDisplayLease **result,
                                      Error **errp)
{
    QemuEGLDisplayLease *lease;
    bool success = false;

    *result = NULL;
    egl_record_lock();
    if (!egl_record) {
        success = true;
    } else if (egl_record->quarantined || !egl_record->providers) {
        error_setg(errp, "No usable EGL provider for a new context");
    } else {
        lease = g_try_new0(QemuEGLDisplayLease, 1);
        if (!lease) {
            error_setg(errp, "Cannot allocate EGL context display lease");
        } else {
            lease->record = egl_record;
            egl_record->references++;
            *result = lease;
            success = true;
        }
    }
    egl_record_unlock();
    return success;
}

DisplayGLMode qemu_egl_display_lease_mode(const QemuEGLDisplayLease *lease)
{
    return lease->record->mode;
}

static void egl_record_release(QemuEGLDisplayLease *lease)
{
    QemuEGLDisplayRecord *record = lease->record;

    assert(!lease->contexts);
    if (lease->quarantined) {
        return;
    }
    assert(record->references);
    if (--record->references == 0) {
        assert(!record->providers && record->root == EGL_NO_CONTEXT);
        if (!eglTerminate(record->display)) {
            record->quarantined = true;
            record->references++;
            lease->quarantined = true;
            return;
        }
#ifdef CONFIG_GBM
        if (record->native) {
            gbm_device_destroy(record->native);
            close(record->fd);
        }
#endif
        egl_record = NULL;
        g_free(record->rendernode);
        g_free(record);
    }
    g_free(lease);
}

void qemu_egl_display_release(QemuEGLDisplayLease *lease)
{
    if (lease) {
        egl_record_lock();
        assert(!lease->provider);
        egl_record_release(lease);
        egl_record_unlock();
    }
}

/* ------------------------------------------------------------------ */

const char *qemu_egl_get_error_string(void)
{
    EGLint error = eglGetError();

    switch (error) {
    case EGL_SUCCESS:
        return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED:
        return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS:
        return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC:
        return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE:
        return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONTEXT:
        return "EGL_BAD_CONTEXT";
    case EGL_BAD_CONFIG:
        return "EGL_BAD_CONFIG";
    case EGL_BAD_CURRENT_SURFACE:
        return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY:
        return "EGL_BAD_DISPLAY";
    case EGL_BAD_SURFACE:
        return "EGL_BAD_SURFACE";
    case EGL_BAD_MATCH:
        return "EGL_BAD_MATCH";
    case EGL_BAD_PARAMETER:
        return "EGL_BAD_PARAMETER";
    case EGL_BAD_NATIVE_PIXMAP:
        return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW:
        return "EGL_BAD_NATIVE_WINDOW";
    case EGL_CONTEXT_LOST:
        return "EGL_CONTEXT_LOST";
    default:
        return "Unknown EGL error";
    }
}

static void egl_fb_delete_texture(egl_fb *fb)
{
    if (!fb->delete_texture) {
        return;
    }

    glDeleteTextures(1, &fb->texture);
    fb->delete_texture = false;
}

void egl_fb_destroy(egl_fb *fb)
{
    if (!fb->framebuffer) {
        return;
    }

    egl_fb_delete_texture(fb);
    glDeleteFramebuffers(1, &fb->framebuffer);

    fb->width = 0;
    fb->height = 0;
    fb->x = 0;
    fb->y = 0;
    fb->texture = 0;
    fb->framebuffer = 0;
}

void egl_fb_setup_default(egl_fb *fb, int width, int height, int x, int y)
{
    fb->width = width;
    fb->height = height;
    fb->x = x;
    fb->y = y;
    fb->framebuffer = 0; /* default framebuffer */
}

void egl_fb_setup_for_tex(egl_fb *fb, int width, int height,
                          GLuint texture, bool delete)
{
    egl_fb_delete_texture(fb);

    fb->width = width;
    fb->height = height;
    fb->texture = texture;
    fb->delete_texture = delete;
    if (!fb->framebuffer) {
        glGenFramebuffers(1, &fb->framebuffer);
    }

    glBindFramebuffer(GL_FRAMEBUFFER_EXT, fb->framebuffer);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_2D, fb->texture, 0);
}

void egl_fb_setup_new_tex(egl_fb *fb, int width, int height)
{
    GLuint texture;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height,
                 0, GL_BGRA, GL_UNSIGNED_BYTE, 0);

    egl_fb_setup_for_tex(fb, width, height, texture, true);
}

void egl_fb_blit(egl_fb *dst, egl_fb *src, bool flip)
{
    GLuint x1 = 0;
    GLuint y1 = 0;
    GLuint x2, y2;
    GLuint w = src->width;
    GLuint h = src->height;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, src->framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst->framebuffer);
    glViewport(0, 0, dst->width, dst->height);
    glClear(GL_COLOR_BUFFER_BIT);

    if (src->dmabuf) {
        x1 = qemu_dmabuf_get_x(src->dmabuf);
        y1 = qemu_dmabuf_get_y(src->dmabuf);
        w = qemu_dmabuf_get_width(src->dmabuf);
        h = qemu_dmabuf_get_height(src->dmabuf);
    }

    w = (x1 + w) > src->width ? src->width - x1 : w;
    h = (y1 + h) > src->height ? src->height - y1 : h;

    y2 = flip ? y1 : h + y1;
    y1 = flip ? h + y1 : y1;
    x2 = x1 + w;

    glBlitFramebuffer(x1, y1, x2, y2,
                      dst->x, dst->y,
                      dst->x + dst->width, dst->y + dst->height,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

void egl_fb_read(DisplaySurface *dst, egl_fb *src)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src->framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    glReadPixels(0, 0, surface_width(dst), surface_height(dst),
                 GL_BGRA, GL_UNSIGNED_BYTE, surface_data(dst));
}

void egl_fb_read_rect(DisplaySurface *dst, egl_fb *src, int x, int y, int w, int h)
{
    assert(surface_width(dst) == src->width);
    assert(surface_height(dst) == src->height);
    assert(surface_format(dst) == PIXMAN_x8r8g8b8);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, src->framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    glPixelStorei(GL_PACK_ROW_LENGTH, surface_stride(dst) / 4);
    glReadPixels(x, y, w, h,
                 GL_BGRA, GL_UNSIGNED_BYTE, surface_data(dst) + x * 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
}

void egl_texture_blit(QemuGLShader *gls, egl_fb *dst, egl_fb *src, bool flip)
{
    glBindFramebuffer(GL_FRAMEBUFFER_EXT, dst->framebuffer);
    glViewport(0, 0, dst->width, dst->height);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, src->texture);
    qemu_gl_run_texture_blit(gls, flip);
}

void egl_texture_blend(QemuGLShader *gls, egl_fb *dst, egl_fb *src, bool flip,
                       int x, int y, double scale_x, double scale_y)
{
    glBindFramebuffer(GL_FRAMEBUFFER_EXT, dst->framebuffer);
    int w = scale_x * src->width;
    int h = scale_y * src->height;
    if (flip) {
        glViewport(x, y, w, h);
    } else {
        glViewport(x, dst->height - h - y, w, h);
    }
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, src->texture);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    qemu_gl_run_texture_blit(gls, flip);
    glDisable(GL_BLEND);
}

/* ---------------------------------------------------------------------- */

EGLContext qemu_egl_rn_ctx;

#ifdef CONFIG_GBM

int qemu_egl_rn_fd;
struct gbm_device *qemu_egl_rn_gbm_dev;

static int egl_rendernode_init(const char *rendernode, DisplayGLMode mode)
{
    qemu_egl_rn_fd = -1;
    int rc;

    qemu_egl_rn_fd = qemu_drm_rendernode_open(rendernode);
    if (qemu_egl_rn_fd == -1) {
        error_report("egl: no drm render node available");
        goto err;
    }

    qemu_egl_rn_gbm_dev = gbm_create_device(qemu_egl_rn_fd);
    if (!qemu_egl_rn_gbm_dev) {
        error_report("egl: gbm_create_device failed");
        goto err;
    }

    rc = qemu_egl_init_dpy_mesa((EGLNativeDisplayType)qemu_egl_rn_gbm_dev,
                                mode);
    if (rc != 0) {
        /* qemu_egl_init_dpy_mesa reports error */
        goto err;
    }

    if (!epoxy_has_egl_extension(qemu_egl_display,
                                 "EGL_KHR_surfaceless_context")) {
        error_report("egl: EGL_KHR_surfaceless_context not supported");
        goto err;
    }
    if (!epoxy_has_egl_extension(qemu_egl_display,
                                 "EGL_MESA_image_dma_buf_export")) {
        error_report("egl: EGL_MESA_image_dma_buf_export not supported");
        goto err;
    }
    if (!epoxy_has_egl_extension(qemu_egl_display,
                                 "EGL_EXT_image_dma_buf_import_modifiers")) {
        error_report("egl: EGL_EXT_image_dma_buf_import_modifiers not supported");
        goto err;
    }

    qemu_egl_rn_ctx = qemu_egl_init_ctx();
    if (!qemu_egl_rn_ctx) {
        error_report("egl: egl_init_ctx failed");
        goto err;
    }

    return 0;

err:
    if (egl_startup_quarantined) {
        return -1;
    }
    if (qemu_egl_display != EGL_NO_DISPLAY) {
        if (!eglTerminate(qemu_egl_display)) {
            egl_startup_quarantined = true;
            return -1;
        }
        qemu_egl_display = EGL_NO_DISPLAY;
    }
    g_clear_pointer(&qemu_egl_rn_gbm_dev, gbm_device_destroy);
    g_clear_fd(&qemu_egl_rn_fd, NULL);
    return -1;
}

bool egl_dmabuf_export_texture(uint32_t tex_id, int *fd, EGLint *offset,
                               EGLint *stride, EGLint *fourcc, int *num_planes,
                               EGLuint64KHR *modifier)
{
    EGLImageKHR image;
    EGLuint64KHR modifiers[DMABUF_MAX_PLANES];
    int i;

    image = eglCreateImageKHR(qemu_egl_display, eglGetCurrentContext(),
                              EGL_GL_TEXTURE_2D_KHR,
                              (EGLClientBuffer)(unsigned long)tex_id,
                              NULL);
    if (!image) {
        return false;
    }

    eglExportDMABUFImageQueryMESA(qemu_egl_display, image, fourcc,
                                  num_planes, modifiers);
    eglExportDMABUFImageMESA(qemu_egl_display, image, fd, stride, offset);
    eglDestroyImageKHR(qemu_egl_display, image);

    /* Only first modifier matters. */
    if (modifier) {
        *modifier = modifiers[0];
    }

    for (i = 0; i < *num_planes; i++) {
        if (fd[i] < 0) {
            return false;
        }
    }
    return true;
}

void egl_dmabuf_import_texture(QemuDmaBuf *dmabuf)
{
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    EGLint attrs[64];
    int i = 0, j;
    uint64_t modifier = qemu_dmabuf_get_modifier(dmabuf);
    uint32_t texture = qemu_dmabuf_get_texture(dmabuf);
    int nfds, noffsets, nstrides;
    const int *fds = qemu_dmabuf_get_fds(dmabuf, &nfds);
    const uint32_t *offsets = qemu_dmabuf_get_offsets(dmabuf, &noffsets);
    const uint32_t *strides = qemu_dmabuf_get_strides(dmabuf, &nstrides);
    uint32_t num_planes = qemu_dmabuf_get_num_planes(dmabuf);

    EGLint fd_attrs[] = {
        EGL_DMA_BUF_PLANE0_FD_EXT,
        EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT,
        EGL_DMA_BUF_PLANE3_FD_EXT,
    };
    EGLint offset_attrs[] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT,
        EGL_DMA_BUF_PLANE3_OFFSET_EXT,
    };
    EGLint stride_attrs[] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE3_PITCH_EXT,
    };
    EGLint modifier_lo_attrs[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
    };
    EGLint modifier_hi_attrs[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
    };

    if (texture != 0) {
        return;
    }

    assert(nfds >= num_planes);
    assert(noffsets >= num_planes);
    assert(nstrides >= num_planes);

    attrs[i++] = EGL_WIDTH;
    attrs[i++] = qemu_dmabuf_get_backing_width(dmabuf);
    attrs[i++] = EGL_HEIGHT;
    attrs[i++] = qemu_dmabuf_get_backing_height(dmabuf);
    attrs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
    attrs[i++] = qemu_dmabuf_get_fourcc(dmabuf);

    for (j = 0; j < num_planes; j++) {
        attrs[i++] = fd_attrs[j];
        /* fd[1-3] may be -1 if using a joint buffer for all planes */
        attrs[i++] = fds[j] >= 0 ? fds[j] : fds[0];
        attrs[i++] = stride_attrs[j];
        attrs[i++] = strides[j];
        attrs[i++] = offset_attrs[j];
        attrs[i++] = offsets[j];
        if (modifier != DRM_FORMAT_MOD_INVALID) {
            attrs[i++] = modifier_lo_attrs[j];
            attrs[i++] = (modifier >>  0) & 0xffffffff;
            attrs[i++] = modifier_hi_attrs[j];
            attrs[i++] = (modifier >> 32) & 0xffffffff;
        }
    }

    attrs[i++] = EGL_NONE;

    image = eglCreateImageKHR(qemu_egl_display,
                              EGL_NO_CONTEXT,
                              EGL_LINUX_DMA_BUF_EXT,
                              NULL, attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        error_report("eglCreateImageKHR failed");
        return;
    }

    glGenTextures(1, &texture);
    qemu_dmabuf_set_texture(dmabuf, texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)image);
    eglDestroyImageKHR(qemu_egl_display, image);
}

void egl_dmabuf_release_texture(QemuDmaBuf *dmabuf)
{
    uint32_t texture;

    texture = qemu_dmabuf_get_texture(dmabuf);
    if (texture == 0) {
        return;
    }

    glDeleteTextures(1, &texture);
    qemu_dmabuf_set_texture(dmabuf, 0);
}

EGLSyncKHR egl_create_sync(void)
{
    EGLSyncKHR sync;

    if (epoxy_has_egl_extension(qemu_egl_display,
                                "EGL_KHR_fence_sync") &&
        epoxy_has_egl_extension(qemu_egl_display,
                                "EGL_ANDROID_native_fence_sync")) {
        sync = eglCreateSyncKHR(qemu_egl_display,
                                EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
        if (sync != EGL_NO_SYNC_KHR) {
            return sync;
        }
    }

    return NULL;
}

int egl_create_fence(EGLSyncKHR sync)
{
    int fence_fd = -1;

    if (sync) {
        fence_fd = eglDupNativeFenceFDANDROID(qemu_egl_display,
                                              sync);
        eglDestroySyncKHR(qemu_egl_display, sync);
    }

    return fence_fd;
}

#endif /* CONFIG_GBM */

/* ---------------------------------------------------------------------- */

EGLSurface qemu_egl_init_surface_x11(EGLContext ectx, EGLNativeWindowType win)
{
    EGLSurface esurface;
    EGLBoolean b;

    esurface = eglCreateWindowSurface(qemu_egl_display,
                                      qemu_egl_config,
                                      win, NULL);
    if (esurface == EGL_NO_SURFACE) {
        error_report("egl: eglCreateWindowSurface failed");
        return NULL;
    }

    b = eglMakeCurrent(qemu_egl_display, esurface, esurface, ectx);
    if (b == EGL_FALSE) {
        error_report("egl: eglMakeCurrent failed");
        return NULL;
    }

    return esurface;
}

/* ---------------------------------------------------------------------- */

#if defined(CONFIG_X11) || defined(CONFIG_GBM) || defined(WIN32)

/*
 * Taken from glamor_egl.h from the Xorg xserver, which is MIT licensed
 *
 * Create an EGLDisplay from a native display type. This is a little quirky
 * for a few reasons.
 *
 * 1: GetPlatformDisplayEXT and GetPlatformDisplay are the API you want to
 * use, but have different function signatures in the third argument; this
 * happens not to matter for us, at the moment, but it means epoxy won't alias
 * them together.
 *
 * 2: epoxy 1.3 and earlier don't understand EGL client extensions, which
 * means you can't call "eglGetPlatformDisplayEXT" directly, as the resolver
 * will crash.
 *
 * 3: You can't tell whether you have EGL 1.5 at this point, because
 * eglQueryString(EGL_VERSION) is a property of the display, which we don't
 * have yet. So you have to query for extensions no matter what. Fortunately
 * epoxy_has_egl_extension _does_ let you query for client extensions, so
 * we don't have to write our own extension string parsing.
 *
 * 4. There is no EGL_KHR_platform_base to complement the EXT one, thus one
 * needs to know EGL 1.5 is supported in order to use the eglGetPlatformDisplay
 * function pointer.
 * We can workaround this (circular dependency) by probing for the EGL 1.5
 * platform extensions (EGL_KHR_platform_gbm and friends) yet it doesn't seem
 * like mesa will be able to advertise these (even though it can do EGL 1.5).
 */
EGLDisplay qemu_egl_get_display(EGLNativeDisplayType native,
                                EGLenum platform)
{
    EGLDisplay dpy = EGL_NO_DISPLAY;

    /* In practise any EGL 1.5 implementation would support the EXT extension */
    if (epoxy_has_egl_extension(NULL, "EGL_EXT_platform_base")) {
        if (platform != 0) {
            dpy = eglGetPlatformDisplayEXT(platform, native, NULL);
        }
    }

    if (dpy == EGL_NO_DISPLAY) {
        /* fallback */
        dpy = eglGetDisplay(native);
    }
    return dpy;
}

static int qemu_egl_init_dpy_unlocked(EGLNativeDisplayType dpy,
                             EGLenum platform,
                             DisplayGLMode mode)
{
    static const EGLint conf_att_core[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,   5,
        EGL_GREEN_SIZE, 5,
        EGL_BLUE_SIZE,  5,
        EGL_ALPHA_SIZE, 0,
        EGL_NONE,
    };
    static const EGLint conf_att_gles[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,   5,
        EGL_GREEN_SIZE, 5,
        EGL_BLUE_SIZE,  5,
        EGL_ALPHA_SIZE, 0,
        EGL_NONE,
    };
    EGLint major, minor;
    EGLBoolean b;
    EGLint n;
    bool gles = (mode == DISPLAY_GL_MODE_ES);

    qemu_egl_display = qemu_egl_get_display(dpy, platform);
    if (qemu_egl_display == EGL_NO_DISPLAY) {
        error_report("egl: eglGetDisplay failed: %s", qemu_egl_get_error_string());
        return -1;
    }

    b = eglInitialize(qemu_egl_display, &major, &minor);
    if (b == EGL_FALSE) {
        error_report("egl: eglInitialize failed: %s", qemu_egl_get_error_string());
        return -1;
    }

    b = eglBindAPI(gles ?  EGL_OPENGL_ES_API : EGL_OPENGL_API);
    if (b == EGL_FALSE) {
        error_report("egl: eglBindAPI failed (%s mode): %s",
                     gles ? "gles" : "core", qemu_egl_get_error_string());
        return -1;
    }

    b = eglChooseConfig(qemu_egl_display,
                        gles ? conf_att_gles : conf_att_core,
                        &qemu_egl_config, 1, &n);
    if (b == EGL_FALSE || n != 1) {
        error_report("egl: eglChooseConfig failed (%s mode): %s",
                     gles ? "gles" : "core", qemu_egl_get_error_string());
        return -1;
    }

    qemu_egl_mode = gles ? DISPLAY_GL_MODE_ES : DISPLAY_GL_MODE_CORE;
    return 0;
}

static int qemu_egl_init_dpy(EGLNativeDisplayType dpy, EGLenum platform,
                            DisplayGLMode mode)
{
    int result;

    egl_record_lock();
    if (egl_record) {
        error_report("egl: cannot replace a managed display");
        result = -1;
    } else {
        result = qemu_egl_init_dpy_unlocked(dpy, platform, mode);
    }
    egl_record_unlock();
    return result;
}

#endif

#if defined(CONFIG_X11) || defined(CONFIG_GBM)
int qemu_egl_init_dpy_x11(EGLNativeDisplayType dpy, DisplayGLMode mode)
{
#ifdef EGL_KHR_platform_x11
    return qemu_egl_init_dpy(dpy, EGL_PLATFORM_X11_KHR, mode);
#else
    return qemu_egl_init_dpy(dpy, 0, mode);
#endif
}

int qemu_egl_init_dpy_mesa(EGLNativeDisplayType dpy, DisplayGLMode mode)
{
#ifdef EGL_MESA_platform_gbm
    return qemu_egl_init_dpy(dpy, EGL_PLATFORM_GBM_MESA, mode);
#else
    return qemu_egl_init_dpy(dpy, 0, mode);
#endif
}
#endif


#ifdef WIN32
int qemu_egl_init_dpy_win32(EGLNativeDisplayType dpy, DisplayGLMode mode)
{
    /* prefer GL ES, as that's what ANGLE supports */
    if (mode == DISPLAY_GL_MODE_ON) {
        mode = DISPLAY_GL_MODE_ES;
    }

    if (qemu_egl_init_dpy(dpy, 0, mode) < 0) {
        return -1;
    }

#ifdef EGL_D3D11_DEVICE_ANGLE
    if (epoxy_has_egl_extension(qemu_egl_display, "EGL_EXT_device_query")) {
        EGLDeviceEXT device;
        void *d3d11_device;

        if (!eglQueryDisplayAttribEXT(qemu_egl_display,
                                      EGL_DEVICE_EXT,
                                      (EGLAttrib *)&device)) {
            return 0;
        }

        if (!eglQueryDeviceAttribEXT(device,
                                     EGL_D3D11_DEVICE_ANGLE,
                                     (EGLAttrib *)&d3d11_device)) {
            return 0;
        }

        trace_egl_init_d3d11_device(device);
        qemu_egl_angle_d3d = device != NULL;
    }
#endif

    return 0;
}
#endif

bool qemu_egl_has_dmabuf(void)
{
    if (qemu_egl_display == EGL_NO_DISPLAY) {
        return false;
    }

    return epoxy_has_egl_extension(qemu_egl_display,
                                   "EGL_EXT_image_dma_buf_import");
}

EGLContext qemu_egl_init_ctx(void)
{
    static const EGLint ctx_att_core[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    static const EGLint ctx_att_gles[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    bool gles = (qemu_egl_mode == DISPLAY_GL_MODE_ES);
    EGLContext ectx;
    EGLBoolean b;

    ectx = eglCreateContext(qemu_egl_display, qemu_egl_config, EGL_NO_CONTEXT,
                            gles ? ctx_att_gles : ctx_att_core);
    if (ectx == EGL_NO_CONTEXT) {
        error_report("egl: eglCreateContext failed");
        return NULL;
    }

    b = eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, ectx);
    if (b == EGL_FALSE) {
        error_report("egl: eglMakeCurrent failed");
        if (!eglDestroyContext(qemu_egl_display, ectx)) {
            egl_failed_context = ectx;
            egl_startup_quarantined = true;
        }
        return NULL;
    }

    return ectx;
}

static bool egl_init_display(const char *rendernode, DisplayGLMode mode,
                             Error **errp)
{
    ERRP_GUARD();

    if (mode == DISPLAY_GL_MODE_OFF) {
        error_setg(errp, "egl: turning off GL doesn't make sense");
        return false;
    }

#ifdef WIN32
    if (qemu_egl_init_dpy_win32(EGL_DEFAULT_DISPLAY, mode) < 0) {
        error_setg(errp, "egl: init failed");
        return false;
    }
    qemu_egl_rn_ctx = qemu_egl_init_ctx();
    if (!qemu_egl_rn_ctx) {
        error_setg(errp, "egl: egl_init_ctx failed");
        return false;
    }
#elif defined(CONFIG_GBM)
    if (egl_rendernode_init(rendernode, mode) < 0) {
        error_setg(errp, "egl: render node init failed");
        return false;
    }
#endif

    if (!qemu_egl_rn_ctx) {
        error_setg(errp, "egl: not available on this platform");
        return false;
    }

    display_opengl = 1;
    return true;
}

static void egl_record_aliases(QemuEGLDisplayRecord *record)
{
    qemu_egl_display = record ? record->display : EGL_NO_DISPLAY;
    qemu_egl_config = record ? record->config : NULL;
    qemu_egl_mode = record ? record->mode : DISPLAY_GL_MODE_OFF;
    qemu_egl_rn_ctx = record ? record->root : EGL_NO_CONTEXT;
#ifdef CONFIG_GBM
    qemu_egl_rn_gbm_dev = record ? record->native : NULL;
    qemu_egl_rn_fd = record ? record->fd : -1;
#endif
}

bool egl_init_session(const char *rendernode, DisplayGLMode mode,
                      QemuEGLDisplayLease **session, Error **errp)
{
    QemuEGLDisplayLease *lease;
    QemuEGLDisplayRecord *record;
    DisplayGLMode requested = mode == DISPLAY_GL_MODE_ON ?
                              DISPLAY_GL_MODE_CORE : mode;
    bool success = false;

    assert(session && !*session);
    egl_record_lock();
    record = egl_record;
    if (record && (record->quarantined || record->mode != requested ||
                   g_strcmp0(record->rendernode, rendernode) ||
                   !qemu_thread_is_self(&record->owner))) {
        error_setg(errp, "egl: incompatible managed provider session");
        goto out;
    }
    lease = g_try_new0(QemuEGLDisplayLease, 1);
    if (!lease) {
        error_setg(errp, "egl: cannot allocate provider session");
        goto out;
    }
    if (!record) {
        if (qemu_egl_display != EGL_NO_DISPLAY) {
            error_setg(errp, "egl: cannot replace an unmanaged display");
            g_free(lease);
            goto out;
        }
        record = g_try_new0(QemuEGLDisplayRecord, 1);
        if (!record) {
            error_setg(errp, "egl: cannot allocate display record");
            g_free(lease);
            goto out;
        }
        if (!egl_init_display(rendernode, mode, errp)) {
            if (!egl_startup_quarantined) {
                g_free(record);
                g_free(lease);
                goto out;
            }
            record->quarantined = true;
            lease->quarantined = true;
        }
        record->display = qemu_egl_display;
        record->config = qemu_egl_config;
        record->mode = qemu_egl_mode;
        record->root = egl_startup_quarantined ? egl_failed_context :
                                               qemu_egl_rn_ctx;
        record->rendernode = g_strdup(rendernode);
        record->generation = ++egl_generation;
        qemu_thread_get_self(&record->owner);
#ifdef CONFIG_GBM
        record->native = qemu_egl_rn_gbm_dev;
        record->fd = qemu_egl_rn_fd;
        record->gbm = record->native != NULL;
#endif
        egl_record = record;
    } else {
        egl_record_aliases(record);
        if (!record->providers) {
            record->root = qemu_egl_init_ctx();
            if (!record->root) {
                record->root = egl_failed_context;
                record->quarantined = true;
                lease->quarantined = true;
            }
            qemu_egl_rn_ctx = record->root;
        }
    }
    lease->record = record;
    record->references++;
    if (record->quarantined) {
        lease->quarantined = true;
        if (!errp || !*errp) {
            error_setg(errp, "egl: provider startup quarantined");
        }
        goto out;
    }
    lease->provider = true;
    record->providers++;
    *session = lease;
    success = true;
out:
    egl_record_unlock();
    return success;
}

void egl_cleanup_session(QemuEGLDisplayLease *lease)
{
    QemuEGLDisplayRecord *record;

    if (!lease) {
        return;
    }
    egl_record_lock();
    record = lease->record;
    assert(lease->provider && !lease->contexts && record->providers);
    if (--record->providers == 0) {
        if (!qemu_thread_is_self(&record->owner) ||
            (eglGetCurrentContext() == record->root &&
             !eglMakeCurrent(record->display, EGL_NO_SURFACE,
                             EGL_NO_SURFACE, EGL_NO_CONTEXT)) ||
            !eglDestroyContext(record->display, record->root)) {
            lease->quarantined = true;
            record->quarantined = true;
            error_report("egl: provider teardown quarantined");
        } else {
            record->root = EGL_NO_CONTEXT;
        }
        egl_record_aliases(NULL);
    }
    egl_record_release(lease);
    egl_record_unlock();
}
