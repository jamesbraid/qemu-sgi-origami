/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/thread.h"
#include "ui/egl-helpers.h"
#include "ui/sgi-renderer.h"

typedef enum SGIRendererWorkKind {
    SGI_RENDERER_WORK_REQUEST,
    SGI_RENDERER_WORK_FRAME,
} SGIRendererWorkKind;

typedef struct SGIRendererGL {
    QemuThread thread;
    QemuMutex mutex;
    QemuCond condition;
    QemuEGLDisplayLease *lease;
    SGIRendererRequest request;
    SGIRendererFrame frame;
    SGIRendererWorkKind work_kind;
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
        EGL_CONTEXT_OPENGL_PROFILE_MASK,
        EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
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

static bool renderer_gl_execute_frame(SGIRendererGL *renderer)
{
    SGIRendererFrame *frame = &renderer->frame;
    GLuint texture = 0, framebuffer = 0, stencil = 0;
    GLenum error;

    if (!qemu_egl_display_lease_usable(renderer->lease)) {
        error_setg(&renderer->error, "SGI renderer EGL display is quarantined");
        return false;
    }
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16, frame->width, frame->height,
                 0, GL_RGBA, GL_UNSIGNED_SHORT, frame->rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture, 0);
    glGenRenderbuffers(1, &stencil);
    glBindRenderbuffer(GL_RENDERBUFFER, stencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8,
                          frame->width, frame->height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, stencil);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        error_setg(&renderer->error, "SGI renderer native framebuffer failed");
        goto out;
    }
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glViewport(0, 0, frame->width, frame->height);
    glUseProgram(0);
    glDisable(GL_MULTISAMPLE);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glDisable(GL_SAMPLE_ALPHA_TO_ONE);
    glDisable(GL_SAMPLE_COVERAGE);
    glDisable(GL_RASTERIZER_DISCARD);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);
    glDisable(GL_TEXTURE_1D);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_TEXTURE_3D);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_COLOR_LOGIC_OP);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xff);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilFunc(GL_ALWAYS, 1, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glShadeModel(GL_SMOOTH);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(frame->projection);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(frame->modelview);
    glBegin(GL_TRIANGLES);
    for (uint32_t i = 0; i < frame->count; i++) {
        const float *vertex = frame->vertices +
                              i * SGI_RENDERER_FRAME_VERTEX_FLOATS;

        glColor4fv(vertex + 4);
        glVertex4fv(vertex);
    }
    glEnd();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    glPixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glReadPixels(0, 0, frame->width, frame->height, GL_RGBA,
                 GL_UNSIGNED_SHORT, frame->rgba);
    if (renderer_gl_error(renderer, "native colour readback")) {
        goto out;
    }
    glReadPixels(0, 0, frame->width, frame->height, GL_STENCIL_INDEX,
                 GL_UNSIGNED_BYTE, frame->written);
    error = glGetError();
    if (error != GL_NO_ERROR) {
        error_setg(&renderer->error, "SGI renderer native draw failed: "
                   "GL error 0x%x", error);
    }
out:
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xff);
    glStencilFunc(GL_ALWAYS, 0, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glDeleteRenderbuffers(1, &stencil);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    return !renderer->error;
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
        renderer->success = renderer->work_kind == SGI_RENDERER_WORK_FRAME ?
                            renderer_gl_execute_frame(renderer) :
                            renderer_gl_execute(renderer);
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
    renderer->lease = qemu_egl_display_acquire_surfaceless(errp);
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
    renderer->work_kind = SGI_RENDERER_WORK_REQUEST;
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

static bool renderer_gl_draw_frame(void *opaque, SGIRendererFrame *frame,
                                   Error **errp)
{
    SGIRendererGL *renderer = opaque;
    bool success;

    qemu_mutex_lock(&renderer->mutex);
    renderer->frame = *frame;
    renderer->work_kind = SGI_RENDERER_WORK_FRAME;
    renderer->pending = true;
    qemu_cond_signal(&renderer->condition);
    while (renderer->pending) {
        qemu_cond_wait(&renderer->condition, &renderer->mutex);
    }
    success = renderer->success;
    if (!success) {
        error_propagate(errp, renderer->error);
        renderer->error = NULL;
    }
    qemu_mutex_unlock(&renderer->mutex);
    return success;
}

static const SGIRendererOps renderer_gl_ops = {
    .create = renderer_gl_create,
    .draw = renderer_gl_draw,
    .draw_frame = renderer_gl_draw_frame,
    .destroy = renderer_gl_destroy,
};

static void renderer_gl_register(void)
{
    sgi_renderer_register_gl(&renderer_gl_ops);
}
module_init(renderer_gl_register, MODULE_INIT_QOM);
