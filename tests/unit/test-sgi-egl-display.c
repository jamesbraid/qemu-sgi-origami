/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/drm.h"
#include "qapi/error.h"
#include "qemu/thread.h"
#include "ui/egl-helpers.h"
#include "ui/egl-context.h"

int display_opengl;
static unsigned initialized, terminated, native_destroyed, contexts_destroyed;
static int native_fd;
static EGLContext current;
static uintptr_t next_context = 4;
static bool fail_destroy, fail_terminate, fail_bind;

int qemu_drm_rendernode_open(const char *name)
{
    native_fd = open("/dev/null", O_RDWR);
    g_assert_cmpint(native_fd, >=, 0);
    return native_fd;
}

struct gbm_device *gbm_create_device(int fd)
{
    g_assert_cmpint(fd, ==, native_fd);
    return (struct gbm_device *)(uintptr_t)3;
}

void gbm_device_destroy(struct gbm_device *device)
{
    g_assert_cmpuint(terminated, ==, 1);
    native_destroyed++;
}

void qemu_gl_run_texture_blit(QemuGLShader *shader, bool flip)
{
    g_assert_not_reached();
}

static EGLDisplay get_display(EGLNativeDisplayType native)
{
    return (EGLDisplay)(uintptr_t)1;
}

static EGLDisplay get_platform(EGLenum platform, void *native,
                               const EGLint *attrs)
{
    return (EGLDisplay)(uintptr_t)1;
}

static const char *query_string(EGLDisplay display, EGLint name)
{
    if (name == EGL_EXTENSIONS) {
        return "EGL_EXT_platform_base EGL_KHR_surfaceless_context "
               "EGL_MESA_image_dma_buf_export "
               "EGL_EXT_image_dma_buf_import_modifiers";
    }
    return "1.5";
}

static EGLBoolean initialize(EGLDisplay display, EGLint *major, EGLint *minor)
{
    initialized++;
    *major = 1;
    *minor = 5;
    return EGL_TRUE;
}

static EGLBoolean bind_api(EGLenum api)
{
    return EGL_TRUE;
}

static EGLBoolean choose_config(EGLDisplay display, const EGLint *attrs,
                                EGLConfig *config, EGLint size, EGLint *count)
{
    *config = (EGLConfig)(uintptr_t)2;
    *count = 1;
    return EGL_TRUE;
}

static EGLContext create_context(EGLDisplay display, EGLConfig config,
                                 EGLContext share, const EGLint *attrs)
{
    return (EGLContext)next_context++;
}

static EGLBoolean make_current(EGLDisplay display, EGLSurface draw,
                               EGLSurface read, EGLContext context)
{
    if (fail_bind && context != EGL_NO_CONTEXT) {
        return EGL_FALSE;
    }
    current = context;
    return EGL_TRUE;
}

static EGLContext get_current(void)
{
    return current;
}

static EGLBoolean destroy_context(EGLDisplay display, EGLContext context)
{
    g_assert_true(current != context);
    if (fail_destroy) {
        return EGL_FALSE;
    }
    contexts_destroyed++;
    return EGL_TRUE;
}

static EGLBoolean terminate(EGLDisplay display)
{
    g_assert_true(current == EGL_NO_CONTEXT);
    if (fail_terminate) {
        return EGL_FALSE;
    }
    terminated++;
    return EGL_TRUE;
}

static EGLBoolean release_thread(void)
{
    current = EGL_NO_CONTEXT;
    return EGL_TRUE;
}

static void install_fake_egl(void)
{
    epoxy_eglGetDisplay = get_display;
    epoxy_eglGetPlatformDisplayEXT = get_platform;
    epoxy_eglQueryString = query_string;
    epoxy_eglInitialize = initialize;
    epoxy_eglBindAPI = bind_api;
    epoxy_eglChooseConfig = choose_config;
    epoxy_eglCreateContext = create_context;
    epoxy_eglMakeCurrent = make_current;
    epoxy_eglGetCurrentContext = get_current;
    epoxy_eglDestroyContext = destroy_context;
    epoxy_eglTerminate = terminate;
    epoxy_eglReleaseThread = release_thread;
}

static void test_retains_display(void)
{
    QemuEGLDisplayLease *provider = NULL, *renderer, *replacement = NULL;
    Error *err = NULL;
    QEMUGLParams params = { .major_ver = 3, .minor_ver = 3 };
    QEMUGLContext child;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }
    install_fake_egl();
    g_assert_true(egl_init_session("fixture", DISPLAY_GL_MODE_CORE,
                                  &provider, &err));
    g_assert_null(err);
    renderer = qemu_egl_display_acquire_existing(DISPLAY_GL_MODE_CORE, &err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    child = qemu_egl_create_context(NULL, &params, EGL_NO_CONTEXT);
    g_assert_true(child != EGL_NO_CONTEXT);
    egl_cleanup_session(provider);
    g_assert_cmpuint(contexts_destroyed, ==, 1);
    g_assert_cmpuint(terminated, ==, 0);
    g_assert_cmpuint(native_destroyed, ==, 0);
    g_assert_true(qemu_egl_display_lease_display(renderer) ==
                  (EGLDisplay)(uintptr_t)1);
    g_assert_true(egl_init_session("fixture", DISPLAY_GL_MODE_CORE,
                                  &replacement, &err));
    g_assert_cmpuint(initialized, ==, 1);
    provider = NULL;
    g_assert_false(egl_init_session("other", DISPLAY_GL_MODE_CORE,
                                   &provider, &err));
    g_assert_null(provider);
    g_assert_nonnull(err);
    error_free(err);
    egl_cleanup_session(replacement);
    g_assert_cmpuint(terminated, ==, 0);
    qemu_egl_display_release(renderer);
    g_assert_cmpuint(terminated, ==, 0);
    qemu_egl_destroy_context(NULL, child);
    g_assert_cmpuint(terminated, ==, 1);
    g_assert_cmpuint(native_destroyed, ==, 1);
    g_assert_cmpint(fcntl(native_fd, F_GETFD), ==, -1);
}

static void *destroy_on_other_thread(void *opaque)
{
    qemu_egl_destroy_context(NULL, opaque);
    return NULL;
}

static void test_quarantine(gconstpointer opaque)
{
    unsigned failure = GPOINTER_TO_UINT(opaque);
    QemuEGLDisplayLease *provider = NULL, *replacement = NULL;
    QEMUGLParams params = { .major_ver = 3, .minor_ver = 3 };
    QEMUGLContext child;
    QemuThread thread;
    Error *err = NULL;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }
    install_fake_egl();
    g_assert_true(egl_init_session("fixture", DISPLAY_GL_MODE_CORE,
                                  &provider, &err));
    child = qemu_egl_create_context(NULL, &params, EGL_NO_CONTEXT);
    g_assert_true(child != EGL_NO_CONTEXT);
    g_assert_cmpint(qemu_egl_make_context_current(NULL, child), ==, 0);
    egl_cleanup_session(provider);
    g_assert_cmpuint(terminated, ==, 0);
    if (failure == 1) {
        fail_destroy = true;
        qemu_egl_destroy_context(NULL, child);
    } else if (failure == 2) {
        fail_terminate = true;
        qemu_egl_destroy_context(NULL, child);
    } else {
        qemu_thread_create(&thread, "egl-test", destroy_on_other_thread,
                           child, QEMU_THREAD_JOINABLE);
        qemu_thread_join(&thread);
        g_assert_true(current == child);
    }
    g_assert_cmpuint(terminated, ==, 0);
    g_assert_cmpuint(native_destroyed, ==, 0);
    g_assert_cmpint(fcntl(native_fd, F_GETFD), >=, 0);
    g_assert_false(egl_init_session("fixture", DISPLAY_GL_MODE_CORE,
                                   &replacement, &err));
    g_assert_null(replacement);
    g_assert_nonnull(err);
    error_free(err);
}

static void test_startup_failure(gconstpointer opaque)
{
    unsigned failure = GPOINTER_TO_UINT(opaque);
    QemuEGLDisplayLease *provider = NULL;
    QemuEGLDisplayLease *renderer = NULL;
    QEMUGLParams params = { .major_ver = 3, .minor_ver = 3 };
    QEMUGLContext child = EGL_NO_CONTEXT;
    Error *err = NULL;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }
    install_fake_egl();
    if (failure == 3) {
        g_assert_true(egl_init_session("fixture", DISPLAY_GL_MODE_CORE,
                                      &provider, &err));
        renderer = qemu_egl_display_acquire_existing(DISPLAY_GL_MODE_CORE,
                                                     &err);
        g_assert_nonnull(renderer);
        child = qemu_egl_create_context(NULL, &params, EGL_NO_CONTEXT);
        g_assert_true(child != EGL_NO_CONTEXT);
        egl_cleanup_session(provider);
        provider = NULL;
    }
    fail_bind = true;
    fail_destroy = failure != 1;
    g_assert_false(egl_init_session("fixture", DISPLAY_GL_MODE_CORE,
                                   &provider, &err));
    g_assert_null(provider);
    g_assert_nonnull(err);
    error_free(err);
    if (failure == 1) {
        g_assert_cmpuint(terminated, ==, 1);
        g_assert_cmpuint(native_destroyed, ==, 1);
        g_assert_cmpint(fcntl(native_fd, F_GETFD), ==, -1);
    } else {
        if (renderer) {
            qemu_egl_display_release(renderer);
            fail_destroy = false;
            qemu_egl_destroy_context(NULL, child);
        }
        g_assert_cmpuint(terminated, ==, 0);
        g_assert_cmpuint(native_destroyed, ==, 0);
        g_assert_cmpint(fcntl(native_fd, F_GETFD), >=, 0);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-egl-display/retains-display", test_retains_display);
    g_test_add_data_func("/sgi-egl-display/destroy-failure",
                         GUINT_TO_POINTER(1), test_quarantine);
    g_test_add_data_func("/sgi-egl-display/terminate-failure",
                         GUINT_TO_POINTER(2), test_quarantine);
    g_test_add_data_func("/sgi-egl-display/wrong-thread",
                         GUINT_TO_POINTER(3), test_quarantine);
    g_test_add_data_func("/sgi-egl-display/startup-bind-failure",
                         GUINT_TO_POINTER(1), test_startup_failure);
    g_test_add_data_func("/sgi-egl-display/startup-destroy-failure",
                         GUINT_TO_POINTER(2), test_startup_failure);
    g_test_add_data_func("/sgi-egl-display/recreation-destroy-failure",
                         GUINT_TO_POINTER(3), test_startup_failure);
    return g_test_run();
}
