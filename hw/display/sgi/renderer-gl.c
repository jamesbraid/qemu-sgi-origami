/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/thread.h"
#include "ui/egl-helpers.h"
#include "ui/sgi-renderer.h"

typedef struct SGIRendererGL {
    QemuThread thread;
    QemuMutex mutex;
    QemuCond condition;
    QemuEGLDisplayLease *lease;
    SGIRendererRequest request;
    Error *error;
    bool ready;
    bool pending;
    bool stop;
    bool success;
    EGLContext context;
    GLuint program;
    GLuint vao;
    GLuint vbo;
    GLuint texture;
    GLuint framebuffer;
} SGIRendererGL;

static bool renderer_gl_error(SGIRendererGL *renderer, const char *operation)
{
    GLenum error = glGetError();

    if (error == GL_NO_ERROR) {
        return false;
    }
    error_setg(&renderer->error, "SGI renderer %s failed: GL error 0x%x",
               operation, error);
    return true;
}

static GLuint renderer_shader(SGIRendererGL *renderer, GLenum type,
                               const char *source)
{
    GLuint shader = glCreateShader(type);
    GLint compiled;
    char log[512];

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        error_setg(&renderer->error, "SGI renderer shader failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool renderer_gl_start(SGIRendererGL *renderer)
{
    static const char *vertex_source =
        "#version 330 core\n"
        "layout(location=0) in vec4 vertex;\n"
        "void main() { gl_Position = vec4(vertex.xy / 64.0 - 1.0,"
        "vertex.z, vertex.w); }\n";
    static const char *fragment_source =
        "#version 330 core\n"
        "uniform vec3 colour;\n"
        "layout(location=0) out vec4 pixel;\n"
        "void main() { pixel = vec4(colour, 1.0); }\n";
    EGLint attributes[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_NONE,
    };
    EGLDisplay display = qemu_egl_display_lease_display(renderer->lease);
    GLuint vertex, fragment;
    GLint linked;
    char log[512];

    if (!qemu_egl_display_lease_usable(renderer->lease) ||
        !eglBindAPI(EGL_OPENGL_API)) {
        error_setg(&renderer->error, "SGI renderer cannot bind desktop OpenGL");
        return false;
    }
    renderer->context = eglCreateContext(display,
                            qemu_egl_display_lease_config(renderer->lease),
                            EGL_NO_CONTEXT, attributes);
    if (renderer->context == EGL_NO_CONTEXT) {
        error_setg(&renderer->error, "SGI renderer cannot create EGL context");
        return false;
    }
    qemu_egl_display_context_created(renderer->lease);
    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                         renderer->context)) {
        error_setg(&renderer->error, "SGI renderer cannot bind EGL context");
        return false;
    }
    info_report("SGI renderer GL vendor=%s renderer=%s version=%s",
                glGetString(GL_VENDOR), glGetString(GL_RENDERER),
                glGetString(GL_VERSION));
    vertex = renderer_shader(renderer, GL_VERTEX_SHADER, vertex_source);
    if (!vertex) {
        return false;
    }
    fragment = renderer_shader(renderer, GL_FRAGMENT_SHADER, fragment_source);
    if (!fragment) {
        glDeleteShader(vertex);
        return false;
    }
    renderer->program = glCreateProgram();
    glAttachShader(renderer->program, vertex);
    glAttachShader(renderer->program, fragment);
    glLinkProgram(renderer->program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    glGetProgramiv(renderer->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        glGetProgramInfoLog(renderer->program, sizeof(log), NULL, log);
        error_setg(&renderer->error, "SGI renderer program failed: %s", log);
        return false;
    }
    glGenVertexArrays(1, &renderer->vao);
    glBindVertexArray(renderer->vao);
    glGenBuffers(1, &renderer->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, renderer->vbo);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glGenTextures(1, &renderer->texture);
    glBindTexture(GL_TEXTURE_2D, renderer->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SGI_RENDERER_SIZE,
                  SGI_RENDERER_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &renderer->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, renderer->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, renderer->texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        error_setg(&renderer->error, "SGI renderer framebuffer is incomplete");
        return false;
    }
    return !renderer_gl_error(renderer, "startup");
}

static bool renderer_gl_execute(SGIRendererGL *renderer)
{
    SGIRendererRequest *request = &renderer->request;
    static const GLenum disabled[] = {
        GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST,
        GL_CULL_FACE, GL_DITHER, GL_MULTISAMPLE, GL_SAMPLE_ALPHA_TO_COVERAGE,
        GL_SAMPLE_ALPHA_TO_ONE, GL_SAMPLE_COVERAGE, GL_COLOR_LOGIC_OP,
        GL_FRAMEBUFFER_SRGB, GL_RASTERIZER_DISCARD, GL_POLYGON_OFFSET_FILL,
    };

    if (!qemu_egl_display_lease_usable(renderer->lease)) {
        error_setg(&renderer->error, "SGI renderer EGL display is quarantined");
        return false;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(disabled); i++) {
        glDisable(disabled[i]);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, renderer->framebuffer);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glViewport(0, 0, SGI_RENDERER_SIZE, SGI_RENDERER_SIZE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(request->clip[0], request->clip[1],
                request->clip[2] - request->clip[0] + 1,
                request->clip[3] - request->clip[1] + 1);
    glEnable(GL_SCISSOR_TEST);
    glUseProgram(renderer->program);
    glUniform3f(glGetUniformLocation(renderer->program, "colour"),
                 request->rgb[0] / 255.0f, request->rgb[1] / 255.0f,
                 request->rgb[2] / 255.0f);
    glBindVertexArray(renderer->vao);
    glBindBuffer(GL_ARRAY_BUFFER, renderer->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(request->vertices), request->vertices,
                   GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    glPixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glReadPixels(0, 0, SGI_RENDERER_SIZE, SGI_RENDERER_SIZE,
                  GL_RGBA, GL_UNSIGNED_BYTE, request->rgba);
    return !renderer_gl_error(renderer, "draw");
}

static void renderer_gl_cleanup(SGIRendererGL *renderer)
{
    EGLDisplay display = qemu_egl_display_lease_display(renderer->lease);
    bool clean = true;

    if (renderer->context != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() == renderer->context) {
            glDeleteFramebuffers(1, &renderer->framebuffer);
            glDeleteTextures(1, &renderer->texture);
            glDeleteBuffers(1, &renderer->vbo);
            glDeleteVertexArrays(1, &renderer->vao);
            glDeleteProgram(renderer->program);
            clean = eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                    EGL_NO_CONTEXT);
        }
        if (clean) {
            clean = eglDestroyContext(display, renderer->context);
            if (clean) {
                qemu_egl_display_context_destroyed(renderer->lease);
            }
        }
    }
    if (!eglReleaseThread()) {
        clean = false;
    }
    if (clean) {
        qemu_egl_display_release(renderer->lease);
    } else {
        /* Retain the display and native resources after uncertain teardown. */
        qemu_egl_display_quarantine(renderer->lease);
        error_report("SGI renderer EGL teardown quarantined");
    }
}

static void *renderer_gl_worker(void *opaque)
{
    SGIRendererGL *renderer = opaque;
    bool started = renderer_gl_start(renderer);

    qemu_mutex_lock(&renderer->mutex);
    renderer->success = started;
    renderer->ready = true;
    qemu_cond_signal(&renderer->condition);
    while (started && !renderer->stop) {
        while (!renderer->pending && !renderer->stop) {
            qemu_cond_wait(&renderer->condition, &renderer->mutex);
        }
        if (renderer->stop) {
            break;
        }
        qemu_mutex_unlock(&renderer->mutex);
        renderer->success = renderer_gl_execute(renderer);
        qemu_mutex_lock(&renderer->mutex);
        renderer->pending = false;
        qemu_cond_signal(&renderer->condition);
    }
    qemu_mutex_unlock(&renderer->mutex);
    renderer_gl_cleanup(renderer);
    return NULL;
}

static void renderer_gl_destroy(void *opaque)
{
    SGIRendererGL *renderer = opaque;

    qemu_mutex_lock(&renderer->mutex);
    renderer->stop = true;
    qemu_cond_signal(&renderer->condition);
    qemu_mutex_unlock(&renderer->mutex);
    qemu_thread_join(&renderer->thread);
    error_free(renderer->error);
    qemu_cond_destroy(&renderer->condition);
    qemu_mutex_destroy(&renderer->mutex);
    g_free(renderer);
}

static void *renderer_gl_create(Error **errp)
{
    SGIRendererGL *renderer = g_try_new0(SGIRendererGL, 1);
    bool success;

    if (!renderer) {
        error_setg(errp, "Cannot allocate SGI GL worker");
        return NULL;
    }
    renderer->lease = qemu_egl_display_acquire_existing(DISPLAY_GL_MODE_CORE,
                                                       errp);
    if (!renderer->lease) {
        g_free(renderer);
        return NULL;
    }
    qemu_mutex_init(&renderer->mutex);
    qemu_cond_init(&renderer->condition);
    qemu_thread_create(&renderer->thread, "sgi-renderer", renderer_gl_worker,
                       renderer, QEMU_THREAD_JOINABLE);
    qemu_mutex_lock(&renderer->mutex);
    while (!renderer->ready) {
        qemu_cond_wait(&renderer->condition, &renderer->mutex);
    }
    success = renderer->success;
    if (!success) {
        error_propagate(errp, renderer->error);
        renderer->error = NULL;
    }
    qemu_mutex_unlock(&renderer->mutex);
    if (!success) {
        renderer_gl_destroy(renderer);
        return NULL;
    }
    return renderer;
}

static bool renderer_gl_draw(void *opaque, SGIRendererRequest *request,
                              Error **errp)
{
    SGIRendererGL *renderer = opaque;
    bool success;

    qemu_mutex_lock(&renderer->mutex);
    renderer->request = *request;
    renderer->pending = true;
    qemu_cond_signal(&renderer->condition);
    while (renderer->pending) {
        qemu_cond_wait(&renderer->condition, &renderer->mutex);
    }
    success = renderer->success;
    if (success) {
        memcpy(request->rgba, renderer->request.rgba, sizeof(request->rgba));
    } else {
        error_propagate(errp, renderer->error);
        renderer->error = NULL;
    }
    qemu_mutex_unlock(&renderer->mutex);
    return success;
}

static const SGIRendererOps renderer_gl_ops = {
    .create = renderer_gl_create,
    .draw = renderer_gl_draw,
    .destroy = renderer_gl_destroy,
};

static void renderer_gl_register(void)
{
    sgi_renderer_register_gl(&renderer_gl_ops);
}
module_init(renderer_gl_register, MODULE_INIT_QOM);
