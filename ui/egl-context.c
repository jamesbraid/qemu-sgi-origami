#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "qapi/error.h"
#include "ui/egl-context.h"

typedef struct QemuEGLContextState {
    EGLContext context;
    QemuEGLDisplayLease *lease;
    QemuThread owner;
    struct QemuEGLContextState *next;
} QemuEGLContextState;

static QemuEGLContextState *contexts;
static QemuMutex context_mutex;
static gsize context_mutex_initialized;

static void context_lock(void)
{
    if (g_once_init_enter(&context_mutex_initialized)) {
        qemu_mutex_init(&context_mutex);
        g_once_init_leave(&context_mutex_initialized, 1);
    }
    qemu_mutex_lock(&context_mutex);
}

static QemuEGLContextState *context_find(EGLContext context)
{
    QemuEGLContextState *state;

    for (state = contexts; state; state = state->next) {
        if (state->context == context) {
            return state;
        }
    }
    return NULL;
}

QEMUGLContext qemu_egl_create_context(DisplayGLCtx *dgc,
                                      QEMUGLParams *params,
                                      EGLContext share_context)
{
    EGLContext ctx;
    EGLDisplay display = qemu_egl_display;
    EGLConfig config = qemu_egl_config;
    DisplayGLMode mode = qemu_egl_mode;
    QemuEGLDisplayLease *lease;
    QemuEGLContextState *state = NULL;
    Error *err = NULL;
    EGLint ctx_att_core[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_CONTEXT_CLIENT_VERSION, params->major_ver,
        EGL_CONTEXT_MINOR_VERSION_KHR, params->minor_ver,
        EGL_NONE
    };
    EGLint ctx_att_gles[] = {
        EGL_CONTEXT_CLIENT_VERSION, params->major_ver,
        EGL_CONTEXT_MINOR_VERSION_KHR, params->minor_ver,
        EGL_NONE
    };

    if (!qemu_egl_display_acquire_context(&lease, &err)) {
        error_report_err(err);
        return EGL_NO_CONTEXT;
    }
    if (lease) {
        state = g_try_new0(QemuEGLContextState, 1);
        if (!state) {
            qemu_egl_display_release(lease);
            return EGL_NO_CONTEXT;
        }
        display = qemu_egl_display_lease_display(lease);
        config = qemu_egl_display_lease_config(lease);
        mode = qemu_egl_display_lease_mode(lease);
    }
    ctx = eglCreateContext(display, config, share_context,
                           mode == DISPLAY_GL_MODE_ES ? ctx_att_gles :
                                                        ctx_att_core);
    if (state) {
        if (ctx == EGL_NO_CONTEXT) {
            qemu_egl_display_release(lease);
            g_free(state);
        } else {
            state->context = ctx;
            state->lease = lease;
            qemu_thread_get_self(&state->owner);
            qemu_egl_display_context_created(lease);
            context_lock();
            state->next = contexts;
            contexts = state;
            qemu_mutex_unlock(&context_mutex);
        }
    }
    return ctx;
}

void qemu_egl_destroy_context(DisplayGLCtx *dgc, QEMUGLContext ctx)
{
    QemuEGLContextState *state, **link;
    EGLDisplay display;

    context_lock();
    state = context_find(ctx);
    if (!state) {
        eglDestroyContext(qemu_egl_display, ctx);
        qemu_mutex_unlock(&context_mutex);
        return;
    }
    display = qemu_egl_display_lease_display(state->lease);
    /* EGL can defer destruction of a context current on another thread. */
    if (!qemu_thread_is_self(&state->owner) ||
        (eglGetCurrentContext() == ctx &&
         !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                         EGL_NO_CONTEXT)) ||
        !eglDestroyContext(display, ctx)) {
        qemu_egl_display_quarantine(state->lease);
        error_report("egl: context teardown quarantined");
        qemu_mutex_unlock(&context_mutex);
        return;
    }
    for (link = &contexts; *link != state; link = &(*link)->next) {
        /* Find the retained context's list entry. */
    }
    *link = state->next;
    qemu_egl_display_context_destroyed(state->lease);
    qemu_egl_display_release(state->lease);
    g_free(state);
    qemu_mutex_unlock(&context_mutex);
}

int qemu_egl_make_context_current(DisplayGLCtx *dgc, QEMUGLContext ctx)
{
    QemuEGLContextState *state;
    EGLDisplay display = qemu_egl_display;
    int result = -1;

    context_lock();
    state = context_find(ctx == EGL_NO_CONTEXT ? eglGetCurrentContext() : ctx);
    if (state) {
        if (!qemu_thread_is_self(&state->owner) ||
            !qemu_egl_display_lease_usable(state->lease)) {
            error_report("egl: context binding refused");
            goto out;
        }
        display = qemu_egl_display_lease_display(state->lease);
    }
    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        error_report("egl: eglMakeCurrent failed: %s", qemu_egl_get_error_string());
        goto out;
    }
    result = 0;
out:
    qemu_mutex_unlock(&context_mutex);
    return result;
}
