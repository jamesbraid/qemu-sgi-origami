/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/module.h"
#include "ui/egl-helpers.h"
#include "ui/sgi-renderer.h"
#include "hw/display/sgi/rasterizer.h"

int display_opengl;

void qemu_gl_run_texture_blit(QemuGLShader *shader, bool flip)
{
    g_assert_not_reached();
}

typedef struct MesaTestEnv {
    char *software;
    char *driver;
} MesaTestEnv;

static void mesa_test_env_clear(MesaTestEnv *env)
{
    if (env->software) {
        g_setenv("LIBGL_ALWAYS_SOFTWARE", env->software, true);
    } else {
        g_unsetenv("LIBGL_ALWAYS_SOFTWARE");
    }
    if (env->driver) {
        g_setenv("GALLIUM_DRIVER", env->driver, true);
    } else {
        g_unsetenv("GALLIUM_DRIVER");
    }
    g_free(env->software);
    g_free(env->driver);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(MesaTestEnv, mesa_test_env_clear)

static void mesa_test_env_set(MesaTestEnv *env)
{
    env->software = g_strdup(g_getenv("LIBGL_ALWAYS_SOFTWARE"));
    env->driver = g_strdup(g_getenv("GALLIUM_DRIVER"));
    g_setenv("LIBGL_ALWAYS_SOFTWARE", "1", true);
    g_setenv("GALLIUM_DRIVER", "llvmpipe", true);
}

static bool mesa_test_available(bool need_core)
{
    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE,
    };
    const EGLint compat_attrs[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK,
        EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE,
    };
    const EGLint core_attrs[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE,
    };
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform_display;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLConfig config;
    EGLint nconfigs;
    const char *reason = "Mesa surfaceless EGL is unavailable";
    bool initialized = false;
    bool available = false;

    platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
        eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!platform_display) {
        goto done;
    }
    display = platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                               EGL_DEFAULT_DISPLAY, NULL);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) {
        goto done;
    }
    initialized = true;
    if (!epoxy_has_egl_extension(display, "EGL_KHR_surfaceless_context") ||
        !eglBindAPI(EGL_OPENGL_API) ||
        !eglChooseConfig(display, config_attrs, &config, 1, &nconfigs) ||
        !nconfigs) {
        goto done;
    }

    reason = "Mesa llvmpipe 3.3 compatibility context is unavailable";
    for (unsigned i = 0; i < (need_core ? 2 : 1); i++) {
        const char *renderer;

        if (i) {
            reason = "Mesa llvmpipe 3.3 core context is unavailable";
        }
        context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                                   i ? core_attrs : compat_attrs);
        if (context == EGL_NO_CONTEXT ||
            !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
            goto done;
        }
        renderer = (const char *)glGetString(GL_RENDERER);
        if (!renderer || !strstr(renderer, "llvmpipe")) {
            goto done;
        }
        g_assert_true(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                     EGL_NO_CONTEXT));
        g_assert_true(eglDestroyContext(display, context));
        context = EGL_NO_CONTEXT;
    }
    available = true;

done:
    if (context != EGL_NO_CONTEXT) {
        g_assert_true(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                     EGL_NO_CONTEXT));
        g_assert_true(eglDestroyContext(display, context));
    }
    if (initialized) {
        g_assert_true(eglTerminate(display));
    }
    if (!available) {
        if (g_getenv("QEMU_TEST_SGI_REQUIRE_MESA")) {
            g_error("%s", reason);
        }
        g_test_skip(reason);
    }
    return available;
}

static void test_shader_output(void)
{
    const char *node = g_getenv("QEMU_TEST_SGI_RENDER_NODE");
    QemuEGLDisplayLease *provider = NULL;
    SGIRenderer *renderer;
    SGIRendererRequest *request;
    Error *err = NULL;
    int fd;
    static const float vertices[3][4] = {
        { 16.25f, 16.25f, 0.0f, 1.0f },
        { 111.25f, 16.25f, 0.0f, 1.0f },
        { 16.25f, 111.25f, 0.0f, 1.0f },
    };

    if (!node) {
        g_test_skip("QEMU_TEST_SGI_RENDER_NODE is not set");
        return;
    }
    g_assert_true(egl_init_session(node, DISPLAY_GL_MODE_CORE,
                                  &provider, &err));
    g_assert_null(err);
    fd = qemu_egl_rn_fd;
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    request = g_new0(SGIRendererRequest, 1);
    memcpy(request->vertices, vertices, sizeof(vertices));
    request->clip[2] = request->clip[3] = 127;
    for (unsigned rgb = 0; rgb < 8; rgb++) {
        request->generation = rgb + 1;
        for (unsigned c = 0; c < 3; c++) {
            request->rgb[c] = rgb & (1 << c) ? 255 : 0;
        }
        memset(request->rgba, 0xa5, sizeof(request->rgba));
        g_assert_true(sgi_renderer_draw(renderer, request, &err));
        g_assert_null(err);
        g_assert_true(sgi_renderer_staging_valid(request));
        g_assert_cmpuint(request->generation, ==, rgb + 1);
    }
    request->clip[0] = 50;
    request->clip[1] = 25;
    request->clip[2] = 90;
    request->clip[3] = 80;
    g_assert_true(sgi_renderer_draw(renderer, request, &err));
    g_assert_null(err);
    g_assert_true(sgi_renderer_staging_valid(request));
    egl_cleanup_session(provider);
    g_assert_cmpint(fcntl(fd, F_GETFD), >=, 0);
    /* The worker must survive after provider display globals are cleared. */
    g_assert_true(sgi_renderer_draw(renderer, request, &err));
    g_assert_null(err);
    g_assert_true(sgi_renderer_staging_valid(request));
    sgi_renderer_free(renderer);
    g_assert_cmpint(fcntl(fd, F_GETFD), ==, -1);
    g_free(request);
}

static PFNEGLCREATECONTEXTPROC saved_create;
static PFNGLGETSHADERIVPROC saved_shader_status;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC saved_framebuffer_status;
static PFNGLREADPIXELSPROC saved_read_pixels;
static PFNGLRENDERBUFFERSTORAGEPROC saved_renderbuffer_storage;
static PFNEGLMAKECURRENTPROC saved_make_current;
static PFNEGLDESTROYCONTEXTPROC saved_destroy;
static PFNEGLRELEASETHREADPROC saved_release;

static EGLContext fail_create(EGLDisplay display, EGLConfig config,
                               EGLContext share, const EGLint *attributes)
{
    return EGL_NO_CONTEXT;
}

static void fail_shader_status(GLuint shader, GLenum name, GLint *value)
{
    saved_shader_status(shader, name, value);
    if (name == GL_COMPILE_STATUS) {
        *value = GL_FALSE;
    }
}

static GLenum fail_framebuffer_status(GLenum target)
{
    return GL_FRAMEBUFFER_UNSUPPORTED;
}

static void fail_read_pixels(GLint x, GLint y, GLsizei width, GLsizei height,
                              GLenum format, GLenum type, void *pixels)
{
    memset(pixels, 0, SGI_RENDERER_BYTES);
}

static void fail_stencil_storage(GLenum target, GLenum internalformat,
                                 GLsizei width, GLsizei height)
{
    saved_renderbuffer_storage(target, GL_NONE, width, height);
}

static void fail_stencil_read_pixels(GLint x, GLint y, GLsizei width,
                                     GLsizei height, GLenum format,
                                     GLenum type, void *pixels)
{
    saved_read_pixels(x, y, width, height,
                      format == GL_STENCIL_INDEX ? GL_NONE : format,
                      type, pixels);
}

static void fail_colour_read_pixels(GLint x, GLint y, GLsizei width,
                                    GLsizei height, GLenum format,
                                    GLenum type, void *pixels)
{
    saved_read_pixels(x, y, width, height,
                      format == GL_RGBA ? GL_NONE : format,
                      type, pixels);
}

static unsigned clear_readback_count;

static void fail_second_clear_read_pixels(GLint x, GLint y, GLsizei width,
                                           GLsizei height, GLenum format,
                                           GLenum type, void *pixels)
{
    clear_readback_count++;
    saved_read_pixels(x, y, width, height,
                      clear_readback_count == 2 ? GL_NONE : format,
                      type, pixels);
}

static EGLBoolean fail_unbind(EGLDisplay display, EGLSurface draw,
                               EGLSurface read, EGLContext context)
{
    if (context == EGL_NO_CONTEXT) {
        return EGL_FALSE;
    }
    return saved_make_current(display, draw, read, context);
}

static EGLBoolean fail_destroy(EGLDisplay display, EGLContext context)
{
    return EGL_FALSE;
}

static EGLBoolean fail_release(void)
{
    return EGL_FALSE;
}

static void test_worker_fault(gconstpointer opaque)
{
    unsigned failure = GPOINTER_TO_UINT(opaque);
    const char *node = g_getenv("QEMU_TEST_SGI_RENDER_NODE");
    g_auto(MesaTestEnv) mesa_env = { 0 };
    QemuEGLDisplayLease *provider = NULL;
    SGIRenderer *renderer;
    SGIRendererRequest *request;
    Error *err = NULL;
    int fd;
    static const float vertices[3][4] = {
        { 16.25f, 16.25f, 0, 1 },
        { 111.25f, 16.25f, 0, 1 },
        { 16.25f, 111.25f, 0, 1 },
    };

    if (!node) {
        mesa_test_env_set(&mesa_env);
        if (!g_test_subprocess() && !mesa_test_available(false)) {
            return;
        }
    }
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }
    if (node) {
        g_assert_true(egl_init_session(node, DISPLAY_GL_MODE_CORE,
                                       &provider, &err));
        g_assert_null(err);
        fd = qemu_egl_rn_fd;
    } else {
        fd = -1;
    }
    saved_create = epoxy_eglCreateContext;
    saved_shader_status = (PFNGLGETSHADERIVPROC)
                          eglGetProcAddress("glGetShaderiv");
    saved_framebuffer_status = epoxy_glCheckFramebufferStatus;
    saved_read_pixels = epoxy_glReadPixels;
    saved_make_current = epoxy_eglMakeCurrent;
    saved_destroy = epoxy_eglDestroyContext;
    saved_release = epoxy_eglReleaseThread;
    if (failure == 1) {
        epoxy_eglCreateContext = fail_create;
    } else if (failure == 2) {
        epoxy_glGetShaderiv = fail_shader_status;
    } else if (failure == 3) {
        epoxy_glCheckFramebufferStatus = fail_framebuffer_status;
    } else if (failure == 4) {
        epoxy_glReadPixels = fail_read_pixels;
    }
    renderer = sgi_renderer_new(&err);
    if (failure <= 3) {
        g_assert_null(renderer);
        g_assert_nonnull(err);
        error_free(err);
    } else {
        g_assert_nonnull(renderer);
        g_assert_null(err);
        request = g_new0(SGIRendererRequest, 1);
        memcpy(request->vertices, vertices, sizeof(vertices));
        request->clip[2] = request->clip[3] = 127;
        request->rgb[0] = 255;
        if (failure == 4) {
            g_assert_false(sgi_renderer_draw(renderer, request, &err));
            g_assert_nonnull(err);
            error_free(err);
            err = NULL;
            g_assert_false(sgi_renderer_draw(renderer, request, &err));
            error_free(err);
        } else {
            g_assert_true(sgi_renderer_draw(renderer, request, &err));
            g_assert_null(err);
            if (failure == 5) {
                epoxy_eglMakeCurrent = fail_unbind;
            } else if (failure == 6) {
                epoxy_eglDestroyContext = fail_destroy;
            } else {
                epoxy_eglReleaseThread = fail_release;
            }
        }
        sgi_renderer_free(renderer);
        g_free(request);
    }
    epoxy_eglCreateContext = saved_create;
    epoxy_glGetShaderiv = saved_shader_status;
    epoxy_glCheckFramebufferStatus = saved_framebuffer_status;
    epoxy_glReadPixels = saved_read_pixels;
    epoxy_eglMakeCurrent = saved_make_current;
    epoxy_eglDestroyContext = saved_destroy;
    epoxy_eglReleaseThread = saved_release;
    egl_cleanup_session(provider);
    if (node) {
        if (failure >= 5) {
            g_assert_cmpint(fcntl(fd, F_GETFD), >=, 0);
        } else {
            g_assert_cmpint(fcntl(fd, F_GETFD), ==, -1);
        }
    } else {
        SGIRenderer *next;

        err = NULL;
        next = sgi_renderer_new(&err);
        if (failure >= 5) {
            g_assert_null(next);
            g_assert_nonnull(err);
            error_free(err);
        } else {
            g_assert_nonnull(next);
            g_assert_null(err);
            sgi_renderer_free(next);
        }
    }
}

static GLuint compile_probe_shader(GLenum kind, const char *source)
{
    GLuint shader = glCreateShader(kind);
    GLint compiled;

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512];

        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        g_error("Mesa probe shader: %s", log);
    }
    return shader;
}

static void test_surfaceless_llvmpipe_pixels(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const char *vertex_source =
        "#version 330 core\n"
        "void main() {\n"
        "  vec2 p[3] = vec2[3](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));\n"
        "  gl_Position = vec4(p[gl_VertexID], 0, 1);\n"
        "}\n";
    static const char *fragment_source =
        "#version 330 core\n"
        "uniform usampler2D source_image;\n"
        "layout(location = 0) out uvec4 pixel;\n"
        "void main() { pixel = texelFetch(source_image,\n"
        "                             ivec2(gl_FragCoord.xy), 0); }\n";
    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE,
    };
    const EGLint context_attrs[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE,
    };
    const EGLint compat_attrs[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK,
        EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE,
    };
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform_display;
    EGLDisplay display;
    EGLConfig config;
    EGLint nconfigs;
    g_autofree uint16_t *input = g_new(uint16_t, 4096 * 4);
    g_autofree uint16_t *output = g_new(uint16_t, 4096 * 4);

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(true)) {
        return;
    }
    platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
        eglGetProcAddress("eglGetPlatformDisplayEXT");
    g_assert_nonnull(platform_display);
    display = platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                               EGL_DEFAULT_DISPLAY, NULL);
    g_assert_true(display != EGL_NO_DISPLAY);
    g_assert_true(eglInitialize(display, NULL, NULL));
    g_assert_true(eglBindAPI(EGL_OPENGL_API));
    g_test_message("EGL vendor=%s version=%s APIs=%s",
                   eglQueryString(display, EGL_VENDOR),
                   eglQueryString(display, EGL_VERSION),
                   eglQueryString(display, EGL_CLIENT_APIS));
    g_assert_true(eglChooseConfig(display, config_attrs, &config, 1,
                                   &nconfigs));
    g_assert_cmpint(nconfigs, ==, 1);

    for (unsigned i = 0; i < 4096; i++) {
        input[i * 4] = i;
        input[i * 4 + 1] = 4095 - i;
        input[i * 4 + 2] = (i * 37) & 4095;
        input[i * 4 + 3] = (i * 251) & 4095;
    }

    for (unsigned iteration = 0; iteration < 3; iteration++) {
        const EGLint *attrs = iteration ? compat_attrs : context_attrs;
        EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                                              attrs);
        GLuint source_image, target_image, framebuffer, vao, program;
        GLuint vertex, fragment;
        GLint linked;
        const char *renderer;

        g_assert_true(context != EGL_NO_CONTEXT);
        g_assert_true(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                     context));
        renderer = (const char *)glGetString(GL_RENDERER);
        g_assert_nonnull(renderer);
        g_test_message("GL vendor=%s renderer=%s version=%s",
                       glGetString(GL_VENDOR), renderer,
                       glGetString(GL_VERSION));
        g_assert_nonnull(strstr(renderer, "llvmpipe"));

        vertex = compile_probe_shader(GL_VERTEX_SHADER, vertex_source);
        fragment = compile_probe_shader(GL_FRAGMENT_SHADER, fragment_source);
        program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        g_assert_cmpint(linked, ==, GL_TRUE);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "source_image"), 0);
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);

        glGenTextures(1, &source_image);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, source_image);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16UI, 4096, 1, 0,
                     GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, input);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glGenTextures(1, &target_image);
        glBindTexture(GL_TEXTURE_2D, target_image);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16UI, 4096, 1, 0,
                     GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, NULL);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, target_image, 0);
        g_assert_cmpint(glCheckFramebufferStatus(GL_FRAMEBUFFER), ==,
                        GL_FRAMEBUFFER_COMPLETE);
        glViewport(0, 0, 4096, 1);
        glBindTexture(GL_TEXTURE_2D, source_image);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, 4096, 1, GL_RGBA_INTEGER,
                     GL_UNSIGNED_SHORT, output);
        g_assert_cmpint(glGetError(), ==, GL_NO_ERROR);
        g_assert_cmpmem(output, 4096 * 4 * sizeof(*output),
                        input, 4096 * 4 * sizeof(*input));
        if (iteration) {
            GLuint legacy_image;
            uint16_t pixel[4];

            glGenTextures(1, &legacy_image);
            glBindTexture(GL_TEXTURE_2D, legacy_image);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16, 4, 4, 0,
                         GL_RGBA, GL_UNSIGNED_SHORT, NULL);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, legacy_image, 0);
            g_assert_cmpint(glCheckFramebufferStatus(GL_FRAMEBUFFER), ==,
                            GL_FRAMEBUFFER_COMPLETE);
            glUseProgram(0);
            glViewport(0, 0, 4, 4);
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            glDisable(GL_DITHER);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            glBegin(GL_TRIANGLES);
            glColor4f(1, 0.25f, 0.5f, 1);
            glVertex2f(-1, -1);
            glVertex2f(3, -1);
            glVertex2f(-1, 3);
            glEnd();
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_SHORT, pixel);
            g_assert_cmpint(glGetError(), ==, GL_NO_ERROR);
            g_assert_cmpuint(pixel[0], ==, 65535);
            g_assert_cmpuint(pixel[1], ==, 16384);
            g_assert_cmpuint(pixel[2], ==, 32768);
            g_assert_cmpuint(pixel[3], ==, 65535);
            glDeleteTextures(1, &legacy_image);
        }

        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &target_image);
        glDeleteTextures(1, &source_image);
        glDeleteVertexArrays(1, &vao);
        glDeleteProgram(program);
        g_assert_true(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                     EGL_NO_CONTEXT));
        g_assert_true(eglDestroyContext(display, context));
    }
    g_assert_true(eglTerminate(display));
    g_assert_true(eglReleaseThread());
}

static void test_two_headless_renderers(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    const float vertices[3][4] = {
        { 16.25f, 16.25f, 0, 1 },
        { 111.25f, 16.25f, 0, 1 },
        { 16.25f, 111.25f, 0, 1 },
    };
    g_autofree SGIRendererRequest *request = g_new0(SGIRendererRequest, 1);
    SGIRenderer *first, *second;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        return;
    }
    memcpy(request->vertices, vertices, sizeof(vertices));
    request->clip[2] = request->clip[3] = SGI_RENDERER_SIZE - 1;
    request->rgb[0] = 255;

    first = sgi_renderer_new(&err);
    g_assert_nonnull(first);
    g_assert_null(err);
    second = sgi_renderer_new(&err);
    g_assert_nonnull(second);
    g_assert_null(err);

    g_assert_true(sgi_renderer_draw(first, request, &err));
    g_assert_null(err);
    request->rgb[0] = 0;
    request->rgb[1] = 255;
    g_assert_true(sgi_renderer_draw(second, request, &err));
    g_assert_null(err);
    sgi_renderer_free(first);
    request->rgb[1] = 0;
    request->rgb[2] = 255;
    g_assert_true(sgi_renderer_draw(second, request, &err));
    g_assert_null(err);
    sgi_renderer_free(second);
}

static void test_headless_display_accepts_ui_context(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    QemuEGLDisplayLease *lease = NULL;
    SGIRenderer *renderer;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    g_assert_true(qemu_egl_display_acquire_context(&lease, &err));
    g_assert_nonnull(lease);
    g_assert_null(err);
    g_assert_cmpint(qemu_egl_display_lease_mode(lease), ==,
                    DISPLAY_GL_MODE_CORE);
    qemu_egl_display_release(lease);
    sgi_renderer_free(renderer);
}

typedef struct NativeDrawFixture {
    uint32_t colour[4][4][3];
    uint32_t tag[4][4];
    uint8_t written[4][4];
    unsigned snapshots, commits, dense_commits;
    bool fail_snapshot, fail_commit;
} NativeDrawFixture;

static SGIRenderResult native_draw_snapshot(void *opaque,
                                             const SGIRenderResource *resource,
                                             uint32_t slice,
                                             const SGIRenderRect *rect,
                                             uint32_t *slots,
                                             size_t stride_slots,
                                             size_t capacity_slots,
                                             Error **errp)
{
    NativeDrawFixture *f = opaque;

    f->snapshots++;
    if (f->fail_snapshot) {
        error_setg(errp, "native snapshot failed");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            for (unsigned c = 0; c < 3; c++) {
                slots[(y - rect->y0) * stride_slots +
                      (x - rect->x0) * 3 + c] = f->colour[y][x][c];
            }
        }
    }
    return SGI_RENDER_OK;
}

static SGIRenderCommitResult native_draw_commit(void *opaque,
                                                const SGIRenderResource *res,
                                                uint32_t slice,
                                                const SGIRenderRect *rect,
                                                const uint32_t *slots,
                                                size_t stride_slots,
                                                size_t capacity_slots,
                                                Error **errp)
{
    NativeDrawFixture *f = opaque;

    f->dense_commits++;
    error_setg(errp, "dense commit cannot preserve native tags");
    return (SGIRenderCommitResult) {
        .status = SGI_RENDER_RESOURCE_ERROR,
        .extent_known = true,
    };
}

static SGIRenderCommitResult native_draw_commit_covered(
    void *opaque, const SGIRenderResource *res, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots,
    size_t stride_slots, size_t capacity_slots,
    const SGIRenderWriteCoverage *writes, Error **errp)
{
    NativeDrawFixture *f = opaque;
    uint64_t scanned = 0;

    f->commits++;
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            uint8_t written = writes->pixel[(y - rect->y0) *
                                            writes->stride_bytes +
                                            x - rect->x0];

            f->written[y][x] = written;
            if (!written) {
                scanned++;
                continue;
            }
            if (f->fail_commit && scanned == 1) {
                f->colour[y][x][0] =
                    slots[(y - rect->y0) * stride_slots +
                          (x - rect->x0) * 3];
                error_setg(errp, "paired native write failed at pixel %" PRIu64,
                           scanned);
                return (SGIRenderCommitResult) {
                    .status = SGI_RENDER_RESOURCE_ERROR,
                    .complete_pixels = scanned,
                    .partial_pixel = true,
                    .extent_known = true,
                };
            }
            for (unsigned c = 0; c < 3; c++) {
                f->colour[y][x][c] =
                    slots[(y - rect->y0) * stride_slots +
                          (x - rect->x0) * 3 + c];
            }
            f->tag[y][x] = 9;
            scanned++;
        }
    }
    return (SGIRenderCommitResult) {
        .status = SGI_RENDER_OK,
        .complete_pixels = scanned,
        .extent_known = true,
    };
}

static void test_decoded_triangle_native_commit(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float xy[3][2] = {
        { -1, -1 }, { 1, -1 }, { -1, 1 },
    };
    static const float probe_vertices[3][4] = {
        { 16.25f, 16.25f, 0, 1 },
        { 111.25f, 16.25f, 0, 1 },
        { 16.25f, 111.25f, 0, 1 },
    };
    g_autofree SGIRendererRequest *probe = g_new0(SGIRendererRequest, 1);
    NativeDrawFixture f = { 0 };
    SGIRenderResource resource = {
        .backing_id = 1, .backing_generation = 1,
        .view_id = 2, .view_generation = 1,
        .role = SGI_RENDER_COLOUR, .target = 7,
        .buffer = SGI_RENDER_FRONT, .eye = SGI_RENDER_MONO,
        .width = 4, .height = 4, .depth = 1,
        .bottom_origin = true, .component_count = 3,
        .component = {
            { SGI_RENDER_R, SGI_RENDER_UNORM, 12 },
            { SGI_RENDER_G, SGI_RENDER_UNORM, 12 },
            { SGI_RENDER_B, SGI_RENDER_UNORM, 12 },
        },
    };
    SGIRenderResourceOps ops = {
        .snapshot = native_draw_snapshot,
        .commit = native_draw_commit,
        .commit_covered = native_draw_commit_covered,
    };
    SGIRasterTarget target = {
        .opaque = &f, .resources = &resource, .resource_count = 1,
        .resource_ops = &ops,
    };
    SGIRasterRules rules = { .color_bits = 12, .evidence = "synthetic" };
    SGIGLDrawState state = {
        .shade_model = SGI_GL_SMOOTH,
        .draw_buffers = SGI_GL_DRAW_FRONT,
        .colour_mask = SGI_GL_COLOUR_MASK_ALL,
        .drawable = {
            .bound = true,
            .clip = { .n = 1, .r[0] = { 0, 0, 3, 3 } },
            .front = 7,
        },
        .viewport = { 0, 0, 4, 4 },
    };
    float vertices[3][SGI_GL_VERTEX_FLOATS] = { 0 };
    SGIGLDraw draw = {
        .mode = SGI_GL_TRIANGLES, .count = 3,
        .vertices = &vertices[0][0], .state = &state,
    };
    SGIRenderer *renderer;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        return;
    }
    for (unsigned i = 0; i < 4; i++) {
        state.modelview[i * 5] = 1;
        state.projection[i * 5] = 1;
    }
    for (unsigned i = 0; i < 3; i++) {
        vertices[i][SGI_GL_V_POSITION] = xy[i][0];
        vertices[i][SGI_GL_V_POSITION + 1] = xy[i][1];
        vertices[i][SGI_GL_V_POSITION + 3] = 1;
        vertices[i][SGI_GL_V_COLOR] = 1;
        vertices[i][SGI_GL_V_COLOR + 3] = 1;
    }
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            f.tag[y][x] = 1;
            for (unsigned c = 0; c < 3; c++) {
                f.colour[y][x][c] = 17;
            }
        }
    }
    f.colour[0][0][0] = 4095;
    f.colour[0][0][1] = 0;
    f.colour[0][0][2] = 0;
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    ops.commit_covered = NULL;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 0);
    g_assert_cmpuint(f.commits, ==, 0);
    g_assert_cmpuint(f.dense_commits, ==, 0);
    ops.commit_covered = native_draw_commit_covered;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 1);
    g_assert_cmpuint(f.commits, ==, 1);
    g_assert_cmpuint(f.dense_commits, ==, 0);
    g_assert_cmpuint(f.written[0][0], ==, 1);
    g_assert_cmpuint(f.written[3][3], ==, 0);
    g_assert_cmpuint(f.colour[0][0][0], ==, 4095);
    g_assert_cmpuint(f.colour[0][0][1], ==, 0);
    g_assert_cmpuint(f.colour[0][0][2], ==, 0);
    g_assert_cmpuint(f.colour[3][3][0], ==, 17);
    g_assert_cmpuint(f.colour[3][3][1], ==, 17);
    g_assert_cmpuint(f.colour[3][3][2], ==, 17);
    g_assert_cmpuint(f.tag[0][0], ==, 9);
    g_assert_cmpuint(f.tag[3][3], ==, 1);
    memcpy(probe->vertices, probe_vertices, sizeof(probe_vertices));
    probe->clip[2] = probe->clip[3] = 127;
    probe->rgb[1] = 255;
    g_assert_true(sgi_renderer_draw(renderer, probe, &err));
    g_assert_null(err);
    g_assert_true(sgi_renderer_staging_valid(probe));

    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            f.tag[y][x] = 1;
            for (unsigned c = 0; c < 3; c++) {
                f.colour[y][x][c] = 17;
            }
        }
    }
    state.modelview[12] = 0.5f;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 2);
    g_assert_cmpuint(f.commits, ==, 2);
    g_assert_cmpuint(f.colour[0][0][0], ==, 17);
    g_assert_cmpuint(f.colour[0][1][0], ==, 4095);
    g_assert_cmpuint(f.tag[0][0], ==, 1);
    g_assert_cmpuint(f.tag[0][1], ==, 9);

    f.fail_snapshot = true;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 3);
    g_assert_cmpuint(f.commits, ==, 2);
    f.fail_snapshot = false;
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            f.tag[y][x] = 1;
            for (unsigned c = 0; c < 3; c++) {
                f.colour[y][x][c] = 17;
            }
        }
    }
    state.modelview[12] = 0;
    f.fail_commit = true;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 4);
    g_assert_cmpuint(f.commits, ==, 3);
    g_assert_cmpuint(f.colour[0][0][0], ==, 4095);
    g_assert_cmpuint(f.colour[0][1][0], ==, 4095);
    g_assert_cmpuint(f.colour[0][1][1], ==, 17);
    g_assert_cmpuint(f.tag[0][0], ==, 9);
    g_assert_cmpuint(f.tag[0][1], ==, 1);

    state.blend = 1;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 4);
    g_assert_cmpuint(f.commits, ==, 3);

    state.blend = 0;
    f.fail_commit = false;
    rules.smooth_primitives = true;
    draw.polygon_smooth = true;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 4);
    g_assert_cmpuint(f.commits, ==, 3);

    draw.polygon_smooth = false;
    rules.smooth_primitives = false;
    state.clip_planes = 1;
    state.clip_plane[0][3] = -1;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 4);
    g_assert_cmpuint(f.commits, ==, 3);

    state.clip_planes = 0;
    state.depth_test = 1;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    state.depth_test = 0;
    state.stencil_test = 1;
    for (unsigned mask = 0; mask <= 0xff; mask += 0xff) {
        state.stencil_write_mask = mask;
        g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                                 &draw, &err), ==,
                        SGI_RENDER_UNSUPPORTED);
        g_assert_nonnull(err);
        error_free(err);
        err = NULL;
        if (mask == 0xff) {
            break;
        }
    }
    state.stencil_test = 0;
    state.alpha_test = 1;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    state.alpha_test = 0;
    g_assert_cmpuint(f.snapshots, ==, 4);
    g_assert_cmpuint(f.commits, ==, 3);

    state.drawable.clip.n = 0;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 4);
    g_assert_cmpuint(f.commits, ==, 3);

    state.drawable.bound = false;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 4);
    g_assert_cmpuint(f.commits, ==, 3);

    state.drawable.bound = true;
    state.drawable.clip.n = 1;
    f.fail_commit = false;
    vertices[0][SGI_GL_V_COLOR] = NAN;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 4);
    g_assert_cmpuint(f.commits, ==, 3);

    vertices[0][SGI_GL_V_COLOR] = 1;
    state.dither = 1;
    rules.dither = SGI_DITHER_BAYER4_TOP_ROWS;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 4);
    g_assert_cmpuint(f.commits, ==, 3);
    state.dither = 0;
    rules.dither = SGI_DITHER_NONE;
    for (unsigned i = 0; i < 3; i++) {
        vertices[i][SGI_GL_V_POSITION] += 3;
    }
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 5);
    g_assert_cmpuint(f.commits, ==, 3);
    g_assert_cmpuint(f.dense_commits, ==, 0);
    for (unsigned i = 0; i < 3; i++) {
        vertices[i][SGI_GL_V_POSITION] -= 3;
    }
    sgi_renderer_free(renderer);

    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    saved_renderbuffer_storage = epoxy_glRenderbufferStorage;
    epoxy_glRenderbufferStorage = fail_stencil_storage;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_HOST_ERROR);
    epoxy_glRenderbufferStorage = saved_renderbuffer_storage;
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 6);
    g_assert_cmpuint(f.commits, ==, 3);
    sgi_renderer_free(renderer);

    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    saved_read_pixels = epoxy_glReadPixels;
    epoxy_glReadPixels = fail_stencil_read_pixels;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_HOST_ERROR);
    epoxy_glReadPixels = saved_read_pixels;
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 7);
    g_assert_cmpuint(f.commits, ==, 3);
    sgi_renderer_free(renderer);

    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    saved_read_pixels = epoxy_glReadPixels;
    epoxy_glReadPixels = fail_colour_read_pixels;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_HOST_ERROR);
    epoxy_glReadPixels = saved_read_pixels;
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(f.snapshots, ==, 8);
    g_assert_cmpuint(f.commits, ==, 3);
    sgi_renderer_free(renderer);
}

static void test_frame_coverage_capacity(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    float vertices[3 * SGI_RENDERER_FRAME_VERTEX_FLOATS] = { 0 };
    uint16_t rgba[4 * 4 * 4] = { 0 };
    uint8_t written[4 * 4] = { 0 };
    SGIRendererFrame frame = {
        .width = 4, .height = 4, .count = 3,
        .vertices = vertices, .rgba = rgba,
        .written = written, .written_capacity = sizeof(written) - 1,
    };
    SGIRenderer *renderer;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    g_assert_false(sgi_renderer_draw_frame(renderer, &frame, &err));
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    frame.written_capacity = sizeof(written);
    frame.written = NULL;
    g_assert_false(sgi_renderer_draw_frame(renderer, &frame, &err));
    g_assert_nonnull(err);
    error_free(err);
    sgi_renderer_free(renderer);
}

typedef struct NativeClearFixture {
    uint32_t colour[2][4][4][4];
    unsigned snapshots, commits;
    uint32_t fail_snapshot_target;
    uint32_t fail_commit_target;
} NativeClearFixture;

static SGIRenderResult native_clear_snapshot(void *opaque,
                                              const SGIRenderResource *resource,
                                              uint32_t slice,
                                              const SGIRenderRect *rect,
                                              uint32_t *slots,
                                              size_t stride_slots,
                                              size_t capacity_slots,
                                              Error **errp)
{
    NativeClearFixture *f = opaque;
    unsigned bank = resource->target - 7;

    g_assert_cmpuint(bank, <, 2);
    f->snapshots++;
    if (resource->target == f->fail_snapshot_target) {
        error_setg(errp, "native clear snapshot failed");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            for (unsigned c = 0; c < resource->component_count; c++) {
                slots[(y - rect->y0) * stride_slots +
                      (x - rect->x0) * resource->component_count + c] =
                    f->colour[bank][y][x][c];
            }
        }
    }
    return SGI_RENDER_OK;
}

static SGIRenderCommitResult native_clear_commit(
    void *opaque, const SGIRenderResource *resource, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, Error **errp)
{
    NativeClearFixture *f = opaque;
    unsigned bank = resource->target - 7;
    uint64_t complete = 0;

    g_assert_cmpuint(bank, <, 2);
    f->commits++;
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            if (resource->target == f->fail_commit_target && complete == 1) {
                error_setg(errp, "native clear commit failed after one pixel");
                return (SGIRenderCommitResult) {
                    .status = SGI_RENDER_RESOURCE_ERROR,
                    .complete_pixels = complete,
                    .extent_known = true,
                };
            }
            for (unsigned c = 0; c < resource->component_count; c++) {
                f->colour[bank][y][x][c] =
                    slots[(y - rect->y0) * stride_slots +
                          (x - rect->x0) * resource->component_count + c];
            }
            complete++;
        }
    }
    return (SGIRenderCommitResult) {
        .status = SGI_RENDER_OK,
        .complete_pixels = complete,
        .extent_known = true,
    };
}

static void test_decoded_clear_native_commit(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float vertices[3][4] = {
        { 16.25f, 16.25f, 0, 1 },
        { 111.25f, 16.25f, 0, 1 },
        { 16.25f, 111.25f, 0, 1 },
    };
    g_autofree SGIRendererRequest *request = g_new0(SGIRendererRequest, 1);
    NativeClearFixture f = { 0 };
    SGIRenderResource resources[2] = { 0 };
    const SGIRenderResourceOps ops = {
        .snapshot = native_clear_snapshot,
        .commit = native_clear_commit,
    };
    SGIRasterTarget target = {
        .opaque = &f, .resources = resources, .resource_count = 2,
        .resource_ops = &ops,
    };
    SGIRasterRules rules = { .color_bits = 12, .evidence = "synthetic" };
    SGIRasterOp op = {
        .kind = SGI_RASTER_CLEAR,
        .targets = { 7, 8 }, .ntargets = 2,
        .x0 = 0, .y0 = 0, .x1 = 3, .y1 = 3,
        .clip = {
            .n = 2,
            .r = { { 1, 1, 2, 2 }, { 3, 0, 3, 1 } },
        },
        .clear_buffers = SGI_GL_COLOR_BUFFER_BIT,
        .clear_rgba = { 1.0f, 0.0f, 0.5f, 1.0f },
        .clear_colour_mask = SGI_GL_COLOUR_MASK_RED |
                             SGI_GL_COLOUR_MASK_BLUE,
    };
    SGIRenderer *renderer;
    SGIRenderResult result;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        return;
    }
    for (unsigned bank = 0; bank < 2; bank++) {
        resources[bank] = (SGIRenderResource) {
            .backing_id = bank + 1, .backing_generation = 1,
            .view_id = bank + 1, .view_generation = 1,
            .role = SGI_RENDER_COLOUR, .target = bank + 7,
            .buffer = bank ? SGI_RENDER_BACK : SGI_RENDER_FRONT,
            .eye = SGI_RENDER_MONO,
            .width = 4, .height = 4, .depth = 1,
            .bottom_origin = true, .component_count = bank ? 4 : 3,
            .component = {
                { SGI_RENDER_R, SGI_RENDER_UNORM, 12 },
                { SGI_RENDER_G, SGI_RENDER_UNORM, 12 },
                { SGI_RENDER_B, SGI_RENDER_UNORM, 12 },
                { SGI_RENDER_A, SGI_RENDER_UNORM, 8 },
            },
        };
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                for (unsigned c = 0; c < 4; c++) {
                    f.colour[bank][y][x][c] =
                        17 + bank * 3 + y * 19 + x * 7 + c * 31;
                }
            }
        }
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 2);
    g_assert_cmpuint(f.commits, ==, 2);
    for (unsigned bank = 0; bank < 2; bank++) {
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                bool covered = (x >= 1 && x <= 2 && y >= 1 && y <= 2) ||
                               (x == 3 && y <= 1);
                uint32_t original = 17 + bank * 3 + y * 19 + x * 7;

                g_assert_cmpuint(f.colour[bank][y][x][0], ==,
                                 covered ? 4095 : original);
                g_assert_cmpuint(f.colour[bank][y][x][1], ==, original + 31);
                g_assert_cmpuint(f.colour[bank][y][x][2], ==,
                                 covered ? 2048 : original + 62);
                if (bank) {
                    g_assert_cmpuint(f.colour[bank][y][x][3], ==,
                                     original + 93);
                }
            }
        }
    }

    memcpy(request->vertices, vertices, sizeof(vertices));
    request->clip[2] = request->clip[3] = 127;
    request->rgb[0] = request->rgb[1] = request->rgb[2] = 255;
    g_assert_true(sgi_renderer_draw(renderer, request, &err));
    g_assert_null(err);
    g_assert_true(sgi_renderer_staging_valid(request));

    resources[1].width = INT_MAX;
    resources[1].height = INT_MAX;
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 2);
    g_assert_cmpuint(f.commits, ==, 2);
    resources[1].width = 4;
    resources[1].height = 4;

    op.clip.n = 0;
    op.clip.empty = 1;
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 2);
    g_assert_cmpuint(f.commits, ==, 2);

    op.clip.empty = 0;
    op.clear_buffers |= SGI_GL_DEPTH_BUFFER_BIT;
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 2);
    g_assert_cmpuint(f.commits, ==, 2);

    op.clear_buffers = SGI_GL_COLOR_BUFFER_BIT;
    for (unsigned bank = 0; bank < 2; bank++) {
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                for (unsigned c = 0; c < 4; c++) {
                    f.colour[bank][y][x][c] = 17;
                }
            }
        }
    }
    f.fail_snapshot_target = 8;
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 4);
    g_assert_cmpuint(f.commits, ==, 2);
    g_assert_cmpuint(f.colour[0][0][0][0], ==, 17);
    g_assert_cmpuint(f.colour[1][0][0][0], ==, 17);
    f.fail_snapshot_target = 0;
    for (unsigned bank = 0; bank < 2; bank++) {
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                for (unsigned c = 0; c < 4; c++) {
                    f.colour[bank][y][x][c] = 17;
                }
            }
        }
    }
    f.fail_commit_target = 8;
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(f.snapshots, ==, 6);
    g_assert_cmpuint(f.commits, ==, 4);
    g_assert_cmpuint(f.colour[0][0][0][0], ==, 4095);
    g_assert_cmpuint(f.colour[0][3][3][0], ==, 4095);
    g_assert_cmpuint(f.colour[1][0][0][0], ==, 4095);
    g_assert_cmpuint(f.colour[1][0][1][0], ==, 17);
    err = NULL;
    f.fail_commit_target = 0;
    for (unsigned bank = 0; bank < 2; bank++) {
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                for (unsigned c = 0; c < 4; c++) {
                    f.colour[bank][y][x][c] = 17;
                }
            }
        }
    }
    clear_readback_count = 0;
    saved_read_pixels = epoxy_glReadPixels;
    epoxy_glReadPixels = fail_second_clear_read_pixels;
    result = sgi_render_raster_checked(renderer, &rules, &target, &op, &err);
    epoxy_glReadPixels = saved_read_pixels;
    g_assert_cmpint(result, ==, SGI_RENDER_HOST_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(clear_readback_count, ==, 2);
    g_assert_cmpuint(f.snapshots, ==, 8);
    g_assert_cmpuint(f.commits, ==, 4);
    g_assert_cmpuint(f.colour[0][0][0][0], ==, 17);
    g_assert_cmpuint(f.colour[1][0][0][0], ==, 17);
    sgi_renderer_free(renderer);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    module_call_init(MODULE_INIT_QOM);
    g_test_add_func("/sgi-renderer-gl/surfaceless-llvmpipe-pixels",
                    test_surfaceless_llvmpipe_pixels);
    g_test_add_func("/sgi-renderer-gl/two-headless-renderers",
                    test_two_headless_renderers);
    g_test_add_func("/sgi-renderer-gl/headless-display-ui-context",
                    test_headless_display_accepts_ui_context);
    g_test_add_func("/sgi-renderer-gl/decoded-triangle-native-commit",
                    test_decoded_triangle_native_commit);
    g_test_add_func("/sgi-renderer-gl/frame-coverage-capacity",
                    test_frame_coverage_capacity);
    g_test_add_func("/sgi-renderer-gl/decoded-clear-native-commit",
                    test_decoded_clear_native_commit);
    if (g_getenv("QEMU_TEST_SGI_RENDER_NODE")) {
        g_test_add_func("/sgi-renderer-gl/shader-output", test_shader_output);
    }
    g_test_add_data_func("/sgi-renderer-gl/context-failure",
                         GUINT_TO_POINTER(1), test_worker_fault);
    g_test_add_data_func("/sgi-renderer-gl/shader-failure",
                         GUINT_TO_POINTER(2), test_worker_fault);
    g_test_add_data_func("/sgi-renderer-gl/framebuffer-failure",
                         GUINT_TO_POINTER(3), test_worker_fault);
    g_test_add_data_func("/sgi-renderer-gl/readback-validation-failure",
                         GUINT_TO_POINTER(4), test_worker_fault);
    g_test_add_data_func("/sgi-renderer-gl/unbind-failure",
                         GUINT_TO_POINTER(5), test_worker_fault);
    g_test_add_data_func("/sgi-renderer-gl/destroy-failure",
                         GUINT_TO_POINTER(6), test_worker_fault);
    g_test_add_data_func("/sgi-renderer-gl/release-thread-failure",
                         GUINT_TO_POINTER(7), test_worker_fault);
    return g_test_run();
}
