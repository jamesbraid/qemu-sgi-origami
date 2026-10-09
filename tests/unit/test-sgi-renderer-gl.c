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

/* The llvmpipe compatibility context's limit, read by mesa_test_available. */
static GLint mesa_test_max_viewport[2];

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
        if (!i) {
            glGetIntegerv(GL_MAX_VIEWPORT_DIMS, mesa_test_max_viewport);
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

#ifdef CONFIG_GBM
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
#endif

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

static void fail_scalar_clear_read_pixels(GLint x, GLint y, GLsizei width,
                                           GLsizei height, GLenum format,
                                           GLenum type, void *pixels)
{
    saved_read_pixels(x, y, width, height,
                      format == GL_DEPTH_STENCIL ? GL_NONE : format,
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
#ifdef CONFIG_GBM
    const char *node = g_getenv("QEMU_TEST_SGI_RENDER_NODE");
#else
    const char *node = NULL;
#endif
    g_auto(MesaTestEnv) mesa_env = { 0 };
    QemuEGLDisplayLease *provider = NULL;
    SGIRenderer *renderer;
    SGIRendererRequest *request;
    Error *err = NULL;
#ifdef CONFIG_GBM
    int fd = -1;
#endif
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
#ifdef CONFIG_GBM
    if (node) {
        g_assert_true(egl_init_session(node, DISPLAY_GL_MODE_CORE,
                                       &provider, &err));
        g_assert_null(err);
        fd = qemu_egl_rn_fd;
    }
#endif
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
#ifdef CONFIG_GBM
    if (node) {
        if (failure >= 5) {
            g_assert_cmpint(fcntl(fd, F_GETFD), >=, 0);
        } else {
            g_assert_cmpint(fcntl(fd, F_GETFD), ==, -1);
        }
    }
#endif
    if (!node) {
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
    uint32_t texel[4];
    uint32_t texels[2][2][4];
    uint32_t tag[4][4];
    uint8_t written[4][4];
    unsigned snapshots, commits, dense_commits;
    bool fail_snapshot, fail_texture_snapshot, fail_commit;
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
    if (f->fail_snapshot ||
        (resource->role == SGI_RENDER_TEXTURE && f->fail_texture_snapshot)) {
        error_setg(errp, "native snapshot failed");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    if (resource->role == SGI_RENDER_TEXTURE) {
        g_assert_true((resource->width == 1 && resource->height == 1) ||
                      (resource->width == 2 && resource->height == 2));
        g_assert_cmpuint(stride_slots, ==, resource->width * 4);
        g_assert_cmpuint(capacity_slots, ==,
                         stride_slots * resource->height);
        for (uint32_t y = 0; y < resource->height; y++) {
            for (uint32_t x = 0; x < resource->width; x++) {
                const uint32_t *texel = resource->width == 1 ? f->texel :
                                        f->texels[y][x];

                memcpy(&slots[y * stride_slots + x * 4], texel,
                       4 * sizeof(*texel));
            }
        }
        return SGI_RENDER_OK;
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

static SGIRenderCommitResult native_blend_clear_commit(
    void *opaque, const SGIRenderResource *res, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots,
    size_t stride_slots, size_t capacity_slots, Error **errp)
{
    NativeDrawFixture *f = opaque;
    uint64_t complete = 0;

    f->dense_commits++;
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            for (unsigned c = 0; c < 3; c++) {
                f->colour[y][x][c] =
                    slots[(y - rect->y0) * stride_slots +
                          (x - rect->x0) * 3 + c];
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
    SGIRasterRules rules = {
        .subpixel_bits = 8, .color_bits = 12,
        .rounding = SGI_ROUND_HALF_AWAY,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE, .evidence = "synthetic",
    };
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
    /*
     * Wholly outside the view volume, the smooth triangle leaves the scalar
     * path nothing to scan, so the draw finishes before any snapshot.
     */
    for (unsigned i = 0; i < 3; i++) {
        vertices[i][SGI_GL_V_POSITION] += 3;
    }
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 4);
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
    g_assert_cmpuint(f.snapshots, ==, 5);
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
    g_assert_cmpuint(f.snapshots, ==, 6);
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
    g_assert_cmpuint(f.snapshots, ==, 7);
    g_assert_cmpuint(f.commits, ==, 3);
    sgi_renderer_free(renderer);
}

static void test_decoded_triangle_viewport_fill(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    NativeDrawFixture f = { 0 };
    SGIRenderResource resource = {
        .backing_id = 1, .backing_generation = 1,
        .view_id = 2, .view_generation = 1,
        .role = SGI_RENDER_COLOUR, .target = 7,
        .buffer = SGI_RENDER_FRONT, .eye = SGI_RENDER_MONO,
        .width = 4, .height = 4, .depth = 1,
        .bottom_origin = true, .component_count = 3,
        .component = {
            { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
        },
    };
    SGIRenderResourceOps ops = {
        .snapshot = native_draw_snapshot,
        .commit_covered = native_draw_commit_covered,
    };
    SGIRasterTarget target = {
        .opaque = &f, .resources = &resource, .resource_count = 1,
        .resource_ops = &ops,
    };
    SGIRasterRules rules = {
        .subpixel_bits = 8, .color_bits = 8,
        .rounding = SGI_ROUND_HALF_AWAY,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE, .evidence = "synthetic",
    };
    SGIGLDrawState state = {
        .shade_model = SGI_GL_FLAT,
        .draw_buffers = SGI_GL_DRAW_FRONT,
        .colour_mask = SGI_GL_COLOUR_MASK_ALL,
        .drawable = {
            .bound = true,
            .clip = { .n = 1, .r[0] = { 0, 0, 3, 3 } },
            .front = 7,
        },
        .viewport = { -8192, -8192, 16384, 16384 },
    };
    uint32_t polygon_modes[2] = { SGI_GL_FILL, SGI_GL_FILL };
    float vertices[3][SGI_GL_VERTEX_FLOATS] = { 0 };
    SGIGLDraw draw = {
        .mode = SGI_GL_TRIANGLES, .count = 3,
        .vertices = &vertices[0][0], .state = &state,
        .polygon_modes = polygon_modes,
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
        vertices[i][SGI_GL_V_POSITION + 3] = 1;
        vertices[i][SGI_GL_V_COLOR] = 1;
        vertices[i][SGI_GL_V_COLOR + 3] = 1;
    }
    vertices[1][SGI_GL_V_POSITION] = 0.5f;
    vertices[2][SGI_GL_V_POSITION + 1] = 0.5f;
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);

    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.written[0][0], ==, 1);
    g_assert_cmpuint(f.written[3][3], ==, 1);
    g_assert_cmpuint(f.colour[0][0][0], ==, 255);

    memset(f.written, 0, sizeof(f.written));
    /* Either face selecting LINE or POINT makes the whole draw unsupported. */
    for (unsigned i = 0; i < 4; i++) {
        static const uint32_t modes[4][2] = {
            { SGI_GL_FILL, SGI_GL_LINE },
            { SGI_GL_FILL, SGI_GL_POINT },
            { SGI_GL_LINE, SGI_GL_FILL },
            { SGI_GL_POINT, SGI_GL_FILL },
        };

        memcpy(polygon_modes, modes[i], sizeof(polygon_modes));
        g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                                 &draw, &err), ==,
                        SGI_RENDER_UNSUPPORTED);
        g_assert_nonnull(err);
        g_clear_pointer(&err, error_free);
    }
    g_assert_cmpuint(f.snapshots, ==, 1);
    g_assert_cmpuint(f.commits, ==, 1);
    sgi_renderer_free(renderer);
}

/*
 * A viewport beyond the host GL limit is unsupported for that draw only: the
 * renderer must stay usable, so a following ordinary draw still commits. A
 * viewport exactly at the limit is accepted.
 */
static void test_decoded_triangle_viewport_limit(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    int32_t over_limit[4][4] = {
        { 0, 0, INT32_MAX, 4 },
        { 0, 0, 4, INT32_MAX },
    };
    NativeDrawFixture f = { 0 };
    SGIRenderResource resource = {
        .backing_id = 1, .backing_generation = 1,
        .view_id = 2, .view_generation = 1,
        .role = SGI_RENDER_COLOUR, .target = 7,
        .buffer = SGI_RENDER_FRONT, .eye = SGI_RENDER_MONO,
        .width = 4, .height = 4, .depth = 1,
        .bottom_origin = true, .component_count = 3,
        .component = {
            { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
        },
    };
    SGIRenderResourceOps ops = {
        .snapshot = native_draw_snapshot,
        .commit_covered = native_draw_commit_covered,
    };
    SGIRasterTarget target = {
        .opaque = &f, .resources = &resource, .resource_count = 1,
        .resource_ops = &ops,
    };
    SGIRasterRules rules = {
        .subpixel_bits = 8, .color_bits = 8,
        .rounding = SGI_ROUND_HALF_AWAY,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE, .evidence = "synthetic",
    };
    SGIGLDrawState state = {
        .shade_model = SGI_GL_FLAT,
        .draw_buffers = SGI_GL_DRAW_FRONT,
        .colour_mask = SGI_GL_COLOUR_MASK_ALL,
        .drawable = {
            .bound = true,
            .clip = { .n = 1, .r[0] = { 0, 0, 3, 3 } },
            .front = 7,
        },
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
        vertices[i][SGI_GL_V_POSITION + 3] = 1;
        vertices[i][SGI_GL_V_COLOR] = 1;
        vertices[i][SGI_GL_V_COLOR + 3] = 1;
    }
    vertices[0][SGI_GL_V_POSITION] = -1;
    vertices[0][SGI_GL_V_POSITION + 1] = -1;
    vertices[1][SGI_GL_V_POSITION] = 3;
    vertices[1][SGI_GL_V_POSITION + 1] = -1;
    vertices[2][SGI_GL_V_POSITION] = -1;
    vertices[2][SGI_GL_V_POSITION + 1] = 3;
    g_assert_cmpint(mesa_test_max_viewport[0], >=, 4);
    g_assert_cmpint(mesa_test_max_viewport[1], >=, 4);
    g_assert_cmpint(mesa_test_max_viewport[0], <, INT32_MAX);
    g_assert_cmpint(mesa_test_max_viewport[1], <, INT32_MAX);
    memcpy(over_limit[2], (int32_t[4]) {
        0, 0, mesa_test_max_viewport[0] + 1, 4,
    }, sizeof(over_limit[2]));
    memcpy(over_limit[3], (int32_t[4]) {
        0, 0, 4, mesa_test_max_viewport[1] + 1,
    }, sizeof(over_limit[3]));
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);

    for (unsigned i = 0; i < G_N_ELEMENTS(over_limit); i++) {
        memcpy(state.viewport, over_limit[i], sizeof(state.viewport));
        g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                                 &draw, &err), ==,
                        SGI_RENDER_UNSUPPORTED);
        g_assert_nonnull(err);
        g_clear_pointer(&err, error_free);
    }
    g_assert_cmpuint(f.snapshots, ==, 0);
    g_assert_cmpuint(f.commits, ==, 0);

    /* The triangle covers window (0..2w, 0..2h), so every pixel either way. */
    for (unsigned pass = 0; pass < 2; pass++) {
        if (pass) {
            memcpy(state.viewport, (int32_t[4]) { 0, 0, 4, 4 },
                   sizeof(state.viewport));
        } else {
            memcpy(state.viewport, (int32_t[4]) {
                0, 0, mesa_test_max_viewport[0], mesa_test_max_viewport[1],
            }, sizeof(state.viewport));
        }
        memset(f.written, 0, sizeof(f.written));
        memset(f.colour, 0, sizeof(f.colour));
        g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                                 &draw, &err), ==,
                        SGI_RENDER_OK);
        g_assert_null(err);
        g_assert_cmpuint(f.commits, ==, pass + 1);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                g_assert_cmpuint(f.written[y][x], ==, 1);
                g_assert_cmpuint(f.colour[y][x][0], ==, 255);
            }
        }
    }
    sgi_renderer_free(renderer);
}

static void test_decoded_triangle_clip_union(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float xy[3][2] = {
        { -1, -1 }, { 1, -1 }, { -1, 1 },
    };
    NativeDrawFixture f = { 0 };
    SGIRenderResource resource = {
        .backing_id = 1, .backing_generation = 1,
        .view_id = 2, .view_generation = 1,
        .role = SGI_RENDER_COLOUR, .target = 7,
        .buffer = SGI_RENDER_FRONT, .eye = SGI_RENDER_MONO,
        .width = 4, .height = 4, .depth = 1,
        .bottom_origin = true, .component_count = 3,
        .component = {
            { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
        },
    };
    SGIRenderResourceOps ops = {
        .snapshot = native_draw_snapshot,
        .commit_covered = native_draw_commit_covered,
    };
    SGIRasterTarget target = {
        .opaque = &f, .resources = &resource, .resource_count = 1,
        .resource_ops = &ops,
    };
    SGIRasterRules rules = {
        .subpixel_bits = 8, .color_bits = 8,
        .rounding = SGI_ROUND_HALF_AWAY,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE, .evidence = "synthetic",
    };
    SGIGLDrawState state = {
        .shade_model = SGI_GL_FLAT,
        .draw_buffers = SGI_GL_DRAW_FRONT,
        .colour_mask = SGI_GL_COLOUR_MASK_ALL,
        .blend = 1,
        .blend_src = SGI_GL_SRC_ALPHA,
        .blend_dst = SGI_GL_ONE_MINUS_SRC_ALPHA,
        .blend_equation = SGI_GL_FUNC_ADD,
        .drawable = {
            .bound = true,
            .clip = {
                .n = 2,
                .r = {
                    { 0, 0, 1, 0 },
                    { 0, 0, 0, 1 },
                },
            },
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
        vertices[i][SGI_GL_V_COLOR + 3] = 0.25f;
    }
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            f.colour[y][x][2] = 255;
            f.tag[y][x] = 1;
        }
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 1);
    g_assert_cmpuint(f.commits, ==, 1);
    g_assert_cmpuint(f.colour[0][0][0], ==, 0x40);
    g_assert_cmpuint(f.colour[0][1][0], ==, 0x40);
    g_assert_cmpuint(f.colour[1][0][0], ==, 0x40);
    g_assert_cmpuint(f.colour[1][1][0], ==, 0);
    g_assert_cmpuint(f.colour[1][1][2], ==, 255);
    g_assert_cmpuint(f.tag[0][0], ==, 9);
    g_assert_cmpuint(f.tag[0][1], ==, 9);
    g_assert_cmpuint(f.tag[1][0], ==, 9);
    g_assert_cmpuint(f.tag[1][1], ==, 1);
    g_assert_cmpuint(f.written[0][0], ==, 1);
    g_assert_cmpuint(f.written[0][1], ==, 1);
    g_assert_cmpuint(f.written[1][0], ==, 1);
    g_assert_cmpuint(f.written[1][1], ==, 0);

    state.drawable.clip.n = 1;
    state.drawable.clip.r[0] = (SGIClipRect) { 5, 5, 6, 6 };
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 1);
    g_assert_cmpuint(f.commits, ==, 1);

    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            f.colour[y][x][0] = 0;
            f.colour[y][x][1] = 0;
            f.colour[y][x][2] = 255;
            f.tag[y][x] = 1;
            f.written[y][x] = 0;
        }
    }
    state.drawable.clip.n = 0;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 1);
    g_assert_cmpuint(f.commits, ==, 1);
    g_assert_cmpuint(f.written[1][1], ==, 0);
    g_assert_cmpuint(f.colour[1][1][0], ==, 0);
    g_assert_cmpuint(f.tag[1][1], ==, 1);

    state.drawable.clip.empty = 1;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 1);
    g_assert_cmpuint(f.commits, ==, 1);

    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            f.colour[y][x][0] = 0;
            f.colour[y][x][1] = 0;
            f.colour[y][x][2] = 255;
            f.tag[y][x] = 1;
            f.written[y][x] = 0;
        }
    }
    state.drawable.clip.empty = 0;
    state.drawable.clip.n = 2;
    state.drawable.clip.r[0] = (SGIClipRect) { 0, 0, 1, 0 };
    state.drawable.clip.r[1] = (SGIClipRect) { 0, 0, 0, 1 };
    state.scissor_test = 1;
    state.scissor[2] = 2;
    state.scissor[3] = 1;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 2);
    g_assert_cmpuint(f.commits, ==, 2);
    g_assert_cmpuint(f.colour[0][0][0], ==, 0x40);
    g_assert_cmpuint(f.colour[0][1][0], ==, 0x40);
    g_assert_cmpuint(f.colour[1][0][0], ==, 0);
    g_assert_cmpuint(f.tag[1][0], ==, 1);
    g_assert_cmpuint(f.written[1][0], ==, 0);

    state.scissor[2] = 0;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 2);
    g_assert_cmpuint(f.commits, ==, 2);

    state.scissor[2] = -1;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 2);
    g_assert_cmpuint(f.commits, ==, 2);

    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            f.colour[y][x][0] = 0;
            f.colour[y][x][1] = 0;
            f.colour[y][x][2] = 255;
            f.tag[y][x] = 1;
            f.written[y][x] = 0;
        }
    }
    state.drawable.clip.n = 0;
    state.scissor[0] = 1;
    state.scissor[2] = 1;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 2);
    g_assert_cmpuint(f.commits, ==, 2);
    g_assert_cmpuint(f.colour[0][1][0], ==, 0);
    g_assert_cmpuint(f.tag[0][1], ==, 1);
    g_assert_cmpuint(f.written[0][1], ==, 0);
    sgi_renderer_free(renderer);
}

static void test_triangle_resource_row_origin(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float xy[3][2] = {
        { -1, -1 }, { 1, -1 }, { -1, 0.5f },
    };
    NativeDrawFixture f[2] = { 0 };
    SGIRenderResource resource = {
        .backing_id = 1, .backing_generation = 1,
        .view_id = 2, .view_generation = 1,
        .role = SGI_RENDER_COLOUR, .target = 7,
        .buffer = SGI_RENDER_FRONT, .eye = SGI_RENDER_MONO,
        .width = 4, .height = 4, .depth = 1,
        .component_count = 3,
        .component = {
            { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
        },
    };
    SGIRenderResourceOps ops = {
        .snapshot = native_draw_snapshot,
        .commit_covered = native_draw_commit_covered,
    };
    SGIRasterTarget target = {
        .resources = &resource, .resource_count = 1,
        .resource_ops = &ops,
    };
    SGIRasterRules rules = {
        .subpixel_bits = 8, .color_bits = 8,
        .rounding = SGI_ROUND_HALF_AWAY,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE, .evidence = "synthetic",
    };
    SGIGLDrawState state = {
        .shade_model = SGI_GL_SMOOTH,
        .draw_buffers = SGI_GL_DRAW_FRONT,
        .colour_mask = SGI_GL_COLOUR_MASK_ALL,
        .blend = 1,
        .blend_src = SGI_GL_SRC_ALPHA,
        .blend_dst = SGI_GL_ONE_MINUS_SRC_ALPHA,
        .blend_equation = SGI_GL_FUNC_ADD,
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
        vertices[i][SGI_GL_V_COLOR] = 0.8f;
        vertices[i][SGI_GL_V_COLOR + 1] = 0.2f;
        vertices[i][SGI_GL_V_COLOR + 2] = 0.3f;
        vertices[i][SGI_GL_V_COLOR + 3] = 0.5f;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    for (unsigned origin = 0; origin < 2; origin++) {
        resource.bottom_origin = origin == 0;
        target.opaque = &f[origin];
        for (unsigned y = 0; y < 4; y++) {
            unsigned window_y = origin ? 3 - y : y;

            for (unsigned x = 0; x < 4; x++) {
                f[origin].tag[y][x] = 1;
                f[origin].colour[y][x][0] = 19 + 17 * window_y + 3 * x;
                f[origin].colour[y][x][1] = 49 + 11 * window_y + 5 * x;
                f[origin].colour[y][x][2] = 83 + 7 * window_y + 2 * x;
            }
        }
        g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                                 &draw, &err), ==,
                        SGI_RENDER_OK);
        g_assert_null(err);
        g_assert_cmpuint(f[origin].snapshots, ==, 1);
        g_assert_cmpuint(f[origin].commits, ==, 1);
        g_assert_cmpuint(f[origin].dense_commits, ==, 0);
    }
    g_assert_cmpuint(f[0].written[0][0], ==, 1);
    g_assert_cmpuint(f[0].written[3][0], ==, 0);
    g_assert_cmpuint(f[1].written[3][0], ==, 1);
    g_assert_cmpuint(f[1].written[0][0], ==, 0);
    g_assert_cmpuint(f[0].colour[0][0][0], >, 19);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            g_assert_cmpuint(f[0].written[y][x], ==,
                             f[1].written[3 - y][x]);
            g_assert_cmpuint(f[0].tag[y][x], ==, f[1].tag[3 - y][x]);
            for (unsigned c = 0; c < 3; c++) {
                g_assert_cmpuint(f[0].colour[y][x][c], ==,
                                 f[1].colour[3 - y][x][c]);
            }
        }
    }

    /* Native top-row clip maps to Mesa row 3 before GL scissor x 1..2. */
    resource.bottom_origin = false;
    target.opaque = &f[1];
    state.blend = 0;
    state.scissor_test = 1;
    state.scissor[0] = 1;
    state.scissor[1] = 3;
    state.scissor[2] = 2;
    state.scissor[3] = 1;
    state.drawable.clip.r[0] = (SGIClipRect) { 0, 0, 3, 0 };
    vertices[0][SGI_GL_V_POSITION] = -1;
    vertices[0][SGI_GL_V_POSITION + 1] = -1;
    vertices[1][SGI_GL_V_POSITION] = 1;
    vertices[1][SGI_GL_V_POSITION + 1] = 1;
    vertices[2][SGI_GL_V_POSITION] = -1;
    vertices[2][SGI_GL_V_POSITION + 1] = 1;
    for (unsigned i = 0; i < 3; i++) {
        vertices[i][SGI_GL_V_COLOR] = 1;
        vertices[i][SGI_GL_V_COLOR + 1] = 0;
        vertices[i][SGI_GL_V_COLOR + 2] = 0;
        vertices[i][SGI_GL_V_COLOR + 3] = 1;
    }
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            f[1].colour[y][x][0] = 11 + y * 4 + x;
            f[1].colour[y][x][1] = 33 + y * 4 + x;
            f[1].colour[y][x][2] = 55 + y * 4 + x;
            f[1].tag[y][x] = 1;
            f[1].written[y][x] = 0;
        }
    }
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f[1].snapshots, ==, 2);
    g_assert_cmpuint(f[1].commits, ==, 2);
    g_assert_cmpuint(f[1].dense_commits, ==, 0);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            bool covered = y == 0 && (x == 1 || x == 2);

            g_assert_cmpuint(f[1].written[y][x], ==, covered);
            g_assert_cmpuint(f[1].tag[y][x], ==, covered ? 9 : 1);
            g_assert_cmpuint(f[1].colour[y][x][0], ==,
                             covered ? 255 : 11 + y * 4 + x);
            g_assert_cmpuint(f[1].colour[y][x][1], ==,
                             covered ? 0 : 33 + y * 4 + x);
            g_assert_cmpuint(f[1].colour[y][x][2], ==,
                             covered ? 0 : 55 + y * 4 + x);
        }
    }
    sgi_renderer_free(renderer);
}

static void test_flat_triangle_alpha_blend(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float xy[3][2] = {
        { -1, -1 }, { 1, -1 }, { -1, 1 },
    };
    NativeDrawFixture f = { 0 };
    SGIRenderResource resource = {
        .backing_id = 1, .backing_generation = 1,
        .view_id = 2, .view_generation = 1,
        .role = SGI_RENDER_COLOUR, .target = 7,
        .buffer = SGI_RENDER_FRONT, .eye = SGI_RENDER_MONO,
        .width = 4, .height = 4, .depth = 1,
        .bottom_origin = true, .component_count = 3,
        .component = {
            { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
        },
    };
    SGIRenderResourceOps ops = {
        .snapshot = native_draw_snapshot,
        .commit_covered = native_draw_commit_covered,
    };
    SGIRasterTarget target = {
        .opaque = &f, .resources = &resource, .resource_count = 1,
        .resource_ops = &ops,
    };
    SGIRasterRules rules = {
        .subpixel_bits = 8, .color_bits = 8,
        .rounding = SGI_ROUND_HALF_AWAY,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE, .evidence = "synthetic",
    };
    SGIGLDrawState state = {
        .shade_model = SGI_GL_FLAT,
        .draw_buffers = SGI_GL_DRAW_FRONT,
        .colour_mask = SGI_GL_COLOUR_MASK_ALL,
        .blend = 1,
        .blend_src = SGI_GL_SRC_ALPHA,
        .blend_dst = SGI_GL_ONE_MINUS_SRC_ALPHA,
        .blend_equation = SGI_GL_FUNC_ADD,
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
        vertices[i][SGI_GL_V_COLOR + 3] = 0.25f;
    }
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            f.colour[y][x][2] = 255;
            f.tag[y][x] = 1;
        }
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 1);
    g_assert_cmpuint(f.commits, ==, 1);
    g_assert_cmpuint(f.colour[0][0][0], ==, 0x40);
    g_assert_cmpuint(f.colour[0][0][1], ==, 0);
    g_assert_cmpuint(f.colour[0][0][2], ==, 0xbf);
    g_assert_cmpuint(f.colour[3][3][0], ==, 0);
    g_assert_cmpuint(f.colour[3][3][2], ==, 0xff);
    g_assert_cmpuint(f.written[0][0], ==, 1);
    g_assert_cmpuint(f.written[3][3], ==, 0);
    g_assert_cmpuint(f.tag[0][0], ==, 9);
    g_assert_cmpuint(f.tag[3][3], ==, 1);

    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            f.colour[y][x][0] = 0;
            f.colour[y][x][1] = 0;
            f.colour[y][x][2] = 255;
            f.tag[y][x] = 1;
            f.written[y][x] = 0;
        }
    }
    vertices[0][SGI_GL_V_COLOR] = 0;
    vertices[0][SGI_GL_V_COLOR + 1] = 1;
    vertices[0][SGI_GL_V_COLOR + 3] = 1;
    vertices[1][SGI_GL_V_COLOR] = 0;
    vertices[1][SGI_GL_V_COLOR + 2] = 1;
    vertices[1][SGI_GL_V_COLOR + 3] = 1;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 2);
    g_assert_cmpuint(f.commits, ==, 2);
    g_assert_cmpuint(f.colour[0][0][0], ==, 0x40);
    g_assert_cmpuint(f.colour[0][0][1], ==, 0);
    g_assert_cmpuint(f.colour[0][0][2], ==, 0xbf);
    g_assert_cmpuint(f.colour[3][3][2], ==, 0xff);

    state.blend_src = SGI_GL_CONSTANT_COLOR;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    state.blend_src = SGI_GL_SRC_ALPHA;
    state.blend_equation = SGI_GL_LOGIC_OP;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    state.blend_equation = SGI_GL_FUNC_ADD;
    state.color_logic_op = 1;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    state.color_logic_op = 0;
    resource.component_count = 4;
    resource.component[3] = (SGIRenderComponent) {
        SGI_RENDER_A, SGI_RENDER_UNORM, 8,
    };
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    resource.component_count = 3;
    for (unsigned c = 0; c < 3; c++) {
        resource.component[c].storage_bits = 12;
    }
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    for (unsigned c = 0; c < 3; c++) {
        resource.component[c].storage_bits = 8;
    }
    g_assert_cmpuint(f.snapshots, ==, 2);
    g_assert_cmpuint(f.commits, ==, 2);
    g_assert_cmpuint(f.colour[0][0][0], ==, 0x40);
    g_assert_cmpuint(f.tag[0][0], ==, 9);

    saved_read_pixels = epoxy_glReadPixels;
    epoxy_glReadPixels = fail_colour_read_pixels;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_HOST_ERROR);
    epoxy_glReadPixels = saved_read_pixels;
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(f.snapshots, ==, 3);
    g_assert_cmpuint(f.commits, ==, 2);
    g_assert_cmpuint(f.dense_commits, ==, 0);
    g_assert_cmpuint(f.colour[0][0][0], ==, 0x40);
    g_assert_cmpuint(f.colour[0][0][2], ==, 0xbf);
    g_assert_cmpuint(f.tag[0][0], ==, 9);
    sgi_renderer_free(renderer);
}

static void test_blend_without_native_alpha(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const struct {
        uint32_t src, dst;
        float alpha;
        uint32_t rgb;
    } cases[] = {
        { SGI_GL_DST_ALPHA, 0, 0.25f, 0xff0000 },
        { 0, SGI_GL_ONE_MINUS_DST_ALPHA, 0.75f, 0x000000 },
        { SGI_GL_SRC_ALPHA_SATURATE, 1, 0.25f, 0x0000ff },
        { SGI_GL_SRC_ALPHA, SGI_GL_ONE_MINUS_SRC_ALPHA, 0, 0x0000ff },
        { SGI_GL_SRC_ALPHA, SGI_GL_ONE_MINUS_SRC_ALPHA, 1, 0xff0000 },
    };
    static const float xy[3][2] = {
        { -1, -1 }, { 1, -1 }, { -1, 1 },
    };
    NativeDrawFixture f = { 0 };
    SGIRenderResource resource = {
        .backing_id = 1, .backing_generation = 1,
        .view_id = 2, .view_generation = 1,
        .role = SGI_RENDER_COLOUR, .target = 7,
        .buffer = SGI_RENDER_FRONT, .eye = SGI_RENDER_MONO,
        .width = 4, .height = 4, .depth = 1,
        .bottom_origin = true, .component_count = 3,
        .component = {
            { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
        },
    };
    SGIRenderResourceOps ops = {
        .snapshot = native_draw_snapshot,
        .commit = native_blend_clear_commit,
        .commit_covered = native_draw_commit_covered,
    };
    SGIRasterTarget target = {
        .opaque = &f, .resources = &resource, .resource_count = 1,
        .resource_ops = &ops,
    };
    SGIRasterRules rules = {
        .subpixel_bits = 8, .color_bits = 8,
        .rounding = SGI_ROUND_HALF_AWAY,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE, .evidence = "synthetic",
    };
    SGIGLDrawState state = {
        .shade_model = SGI_GL_FLAT,
        .draw_buffers = SGI_GL_DRAW_FRONT,
        .colour_mask = SGI_GL_COLOUR_MASK_ALL,
        .blend = 1,
        .blend_src = SGI_GL_SRC_ALPHA,
        .blend_dst = SGI_GL_ONE_MINUS_SRC_ALPHA,
        .blend_equation = SGI_GL_FUNC_ADD,
        .drawable = {
            .bound = true,
            .clip = { .n = 1, .r[0] = { 0, 0, 3, 3 } },
            .front = 7,
        },
        .viewport = { 0, 0, 4, 4 },
    };
    SGIRasterOp clear = {
        .kind = SGI_RASTER_CLEAR,
        .targets = { 7 }, .ntargets = 1,
        .x0 = 0, .y0 = 0, .x1 = 3, .y1 = 3,
        .clip = { .n = 1, .r[0] = { 0, 0, 3, 3 } },
        .clear_buffers = SGI_GL_COLOR_BUFFER_BIT,
        .clear_rgba = { 0, 0, 1, 0 },
        .clear_colour_mask = SGI_GL_COLOUR_MASK_ALL,
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
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        for (unsigned clear_alpha = 0; clear_alpha < 2; clear_alpha++) {
            unsigned commits = f.commits;

            g_test_message("factor case %u, clear alpha %u", i, clear_alpha);
            clear.clear_rgba[3] = clear_alpha;
            g_assert_cmpint(sgi_render_raster_checked(renderer, &rules,
                                                       &target, &clear, &err),
                            ==, SGI_RENDER_OK);
            g_assert_null(err);
            g_assert_cmpuint(f.commits, ==, commits);
            g_assert_cmpuint(f.colour[0][0][2], ==, 0xff);
            g_assert_cmpuint(f.colour[3][3][2], ==, 0xff);
            memset(f.written, 0, sizeof(f.written));
            for (unsigned y = 0; y < 4; y++) {
                for (unsigned x = 0; x < 4; x++) {
                    f.tag[y][x] = 1;
                }
            }
            state.blend_src = cases[i].src;
            state.blend_dst = cases[i].dst;
            for (unsigned v = 0; v < 3; v++) {
                vertices[v][SGI_GL_V_COLOR + 3] = cases[i].alpha;
            }
            g_assert_cmpint(sgi_render_draw_checked(renderer, &rules,
                                                     &target, &draw, &err),
                            ==, SGI_RENDER_OK);
            g_assert_null(err);
            g_assert_cmphex((f.colour[0][0][0] << 16) |
                            (f.colour[0][0][1] << 8) |
                             f.colour[0][0][2], ==, cases[i].rgb);
            g_assert_cmphex((f.colour[3][3][0] << 16) |
                            (f.colour[3][3][1] << 8) |
                             f.colour[3][3][2], ==, 0x0000ff);
            g_assert_cmpuint(f.written[0][0], ==, 1);
            g_assert_cmpuint(f.written[3][3], ==, 0);
            g_assert_cmpuint(f.tag[0][0], ==, 9);
            g_assert_cmpuint(f.tag[3][3], ==, 1);
            g_assert_cmpuint(f.commits, ==, commits + 1);
        }
    }
    {
        unsigned snapshots = f.snapshots;
        unsigned commits = f.commits;
        unsigned dense_commits = f.dense_commits;

        /* A refused constant factor takes no snapshot, changes nothing. */
        state.blend_src = SGI_GL_DST_ALPHA;
        state.blend_dst = SGI_GL_ONE_MINUS_CONSTANT_ALPHA;
        g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                                 &draw, &err), ==,
                        SGI_RENDER_UNSUPPORTED);
        g_assert_nonnull(err);
        error_free(err);
        err = NULL;
        g_assert_cmpuint(f.snapshots, ==, snapshots);
        g_assert_cmpuint(f.commits, ==, commits);
        g_assert_cmpuint(f.dense_commits, ==, dense_commits);
        g_assert_cmpuint(f.colour[0][0][0], ==, 0xff);
        g_assert_cmpuint(f.tag[0][0], ==, 9);

        state.blend_src = SGI_GL_DST_ALPHA;
        state.blend_dst = 0;
        for (unsigned v = 0; v < 3; v++) {
            vertices[v][SGI_GL_V_COLOR + 3] = 0.25f;
        }
        saved_read_pixels = epoxy_glReadPixels;
        epoxy_glReadPixels = fail_colour_read_pixels;
        g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                                 &draw, &err), ==,
                        SGI_RENDER_HOST_ERROR);
        epoxy_glReadPixels = saved_read_pixels;
        g_assert_nonnull(err);
        error_free(err);
        g_assert_cmpuint(f.snapshots, ==, snapshots + 1);
        g_assert_cmpuint(f.commits, ==, commits);
        g_assert_cmpuint(f.dense_commits, ==, dense_commits);
        g_assert_cmpuint(f.colour[0][0][0], ==, 0xff);
        g_assert_cmpuint(f.colour[3][3][2], ==, 0xff);
        g_assert_cmpuint(f.tag[0][0], ==, 9);
    }
    sgi_renderer_free(renderer);
}

static bool unexpected_texture_read(void *opaque, uint32_t x, uint32_t y,
                                    uint32_t *rgba)
{
    g_assert_not_reached();
}

static void test_typed_texture_triangle(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float xy[3][2] = {
        { -1, -1 }, { 1, -1 }, { -1, 1 },
    };
    static const struct {
        uint32_t mode;
        float primary[3];
        uint32_t texel[4];
        uint8_t expected[3];
        float env[3];
    } cases[] = {
        { SGI_GL_REPLACE, { 1, 1, 1 }, { 64, 128, 192, 170 },
          { 64, 128, 192 } },
        { SGI_GL_MODULATE, { 0.5f, 1, 0 }, { 128, 128, 128, 170 },
          { 64, 128, 0 } },
        { SGI_GL_BLEND, { 0.25f, 0.5f, 0.75f }, { 128, 64, 192, 128 },
          { 128, 112, 143 }, { 0.75f, 0.25f, 0.5f } },
        { SGI_GL_DECAL, { 0.5f, 0, 0 }, { 0, 0, 255, 170 },
          { 43, 0, 170 } },
    };
    NativeDrawFixture f = { 0 };
    SGIRenderResource resources[] = {
        {
            .backing_id = 1, .backing_generation = 1,
            .view_id = 2, .view_generation = 1,
            .role = SGI_RENDER_COLOUR, .target = 7,
            .buffer = SGI_RENDER_FRONT, .eye = SGI_RENDER_MONO,
            .width = 4, .height = 4, .depth = 1,
            .bottom_origin = true, .component_count = 3,
            .component = {
                { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
            },
        },
        {
            .backing_id = 3, .backing_generation = 1,
            .view_id = 4, .view_generation = 1,
            .role = SGI_RENDER_TEXTURE,
            .buffer = SGI_RENDER_BUFFER_NONE, .eye = SGI_RENDER_MONO,
            .width = 1, .height = 1, .depth = 1,
            .bottom_origin = true, .component_count = 4,
            .component = {
                { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_A, SGI_RENDER_UNORM, 8 },
            },
        },
    };
    SGIGLTexImage image = {
        .width = 1, .height = 1, .depth = 1,
        .base_format = SGI_GL_RGBA,
        .min_filter = SGI_GL_NEAREST, .mag_filter = SGI_GL_NEAREST,
        .wrap_s = SGI_GL_CLAMP_TO_EDGE, .wrap_t = SGI_GL_CLAMP_TO_EDGE,
        .read_texel = unexpected_texture_read,
    };
    SGIRenderResourceOps ops = {
        .snapshot = native_draw_snapshot,
        .commit_covered = native_draw_commit_covered,
    };
    SGIRasterTarget target = {
        .opaque = &f, .resources = resources, .resource_count = 2,
        .resource_ops = &ops, .texture = &image,
    };
    SGIRasterRules rules = {
        .subpixel_bits = 8, .color_bits = 8,
        .rounding = SGI_ROUND_HALF_AWAY,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE, .evidence = "synthetic",
    };
    SGIGLDrawState state = {
        .shade_model = SGI_GL_FLAT,
        .draw_buffers = SGI_GL_DRAW_FRONT,
        .colour_mask = SGI_GL_COLOUR_MASK_ALL,
        .texture_2d = 1,
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
        vertices[i][SGI_GL_V_COLOR + 3] = 1;
        vertices[i][SGI_GL_V_TEXCOORD + 3] = 1;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    g_assert_true(sgi_renderer_texture_supported(renderer, &err));
    g_assert_null(err);
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        state.texture_env_mode = cases[i].mode;
        memcpy(state.texture_env_color, cases[i].env, sizeof(cases[i].env));
        memcpy(f.texel, cases[i].texel, sizeof(f.texel));
        memset(f.colour, 0, sizeof(f.colour));
        memset(f.written, 0, sizeof(f.written));
        for (unsigned v = 0; v < 3; v++) {
            memcpy(&vertices[v][SGI_GL_V_COLOR], cases[i].primary,
                   sizeof(cases[i].primary));
        }
        g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                                 &draw, &err), ==,
                        SGI_RENDER_OK);
        g_assert_null(err);
        g_assert_cmpuint(f.snapshots, ==, (i + 1) * 2);
        g_assert_cmpuint(f.commits, ==, i + 1);
        for (unsigned c = 0; c < 3; c++) {
            g_assert_cmpuint(f.colour[0][0][c], ==, cases[i].expected[c]);
        }
        g_assert_cmpuint(f.written[0][0], ==, 1);
        g_assert_cmpuint(f.written[3][3], ==, 0);
    }
    state.texture_3d = 1;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
    state.texture_3d = 0;
    image.min_filter = SGI_GL_LINEAR;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
    image.min_filter = SGI_GL_NEAREST;
    image.base_format = SGI_GL_RGB;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
    image.base_format = SGI_GL_RGBA;
    resources[1].border[0] = 1;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
    resources[1].border[0] = 0;
    image.width = resources[1].width = 3;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
    image.width = resources[1].width = 1;
    g_assert_cmpuint(f.snapshots, ==, 2 * ARRAY_SIZE(cases));
    g_assert_cmpuint(f.commits, ==, ARRAY_SIZE(cases));
    state.drawable.clip.r[0] = (SGIClipRect) { 5, 5, 6, 6 };
    /* Nearest power-of-two shapes below, at, and above 16 MiB of slots. */
    image.width = resources[1].width = 1024;
    image.height = resources[1].height = 512;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    image.height = resources[1].height = 1024;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    image.width = resources[1].width = 2048;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err),
                            "host staging budget"));
    g_clear_pointer(&err, error_free);
    image.height = resources[1].height = 1;
    state.drawable.clip.r[0] = (SGIClipRect) { 0, 0, 3, 3 };
    g_assert_cmpuint(f.snapshots, ==, 2 * ARRAY_SIZE(cases));
    g_assert_cmpuint(f.commits, ==, ARRAY_SIZE(cases));
    /* A legal power-of-two width whose four-slot stride wraps uint32_t. */
    image.width = resources[1].width = 1u << 30;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    sizeof(size_t) == 4 ? SGI_RENDER_RESOURCE_ERROR :
                                          SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    if (sizeof(size_t) > 4) {
        g_assert_nonnull(strstr(error_get_pretty(err),
                                "host staging budget"));
    }
    g_clear_pointer(&err, error_free);
    image.height = resources[1].height = 1u << 30;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
    image.width = resources[1].width = 1;
    image.height = resources[1].height = 1;
    g_assert_cmpuint(f.snapshots, ==, 2 * ARRAY_SIZE(cases));
    g_assert_cmpuint(f.commits, ==, ARRAY_SIZE(cases));
    vertices[0][SGI_GL_V_POSITION] = -2;
    vertices[0][SGI_GL_V_TEXCOORD + 3] = 0;
    vertices[0][SGI_GL_V_COLOR] = 0;
    vertices[1][SGI_GL_V_COLOR] = 1;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 2 * (ARRAY_SIZE(cases) + 1));
    g_assert_cmpuint(f.commits, ==, ARRAY_SIZE(cases) + 1);
    g_assert_cmpuint(f.colour[0][0][0], ==, 43);
    state.shade_model = SGI_GL_SMOOTH;
    state.texture_env_mode = SGI_GL_MODULATE;
    f.texel[0] = 255;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 2 * (ARRAY_SIZE(cases) + 2));
    g_assert_cmpuint(f.commits, ==, ARRAY_SIZE(cases) + 2);
    /* Pixel (0, 0) has red 7/16 after clipping; round 255 * 7/16. */
    g_assert_cmpuint(f.colour[0][0][0], ==, 112);
    state.shade_model = SGI_GL_FLAT;
    state.texture_env_mode = SGI_GL_DECAL;
    f.texel[0] = 0;
    vertices[0][SGI_GL_V_POSITION] = -1;
    vertices[0][SGI_GL_V_TEXCOORD + 3] = 0;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
    vertices[0][SGI_GL_V_TEXCOORD + 3] = 1;
    g_assert_cmpuint(f.snapshots, ==, 2 * (ARRAY_SIZE(cases) + 2));
    g_assert_cmpuint(f.commits, ==, ARRAY_SIZE(cases) + 2);
    f.fail_texture_snapshot = true;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(f.snapshots, ==, 2 * (ARRAY_SIZE(cases) + 3));
    g_assert_cmpuint(f.commits, ==, ARRAY_SIZE(cases) + 2);
    g_assert_cmpuint(f.colour[0][0][0], ==, 112);
    sgi_renderer_free(renderer);
}

static void native_draw_fill_sentinel(NativeDrawFixture *f)
{
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            f->colour[y][x][0] = 5;
            f->colour[y][x][1] = 7;
            f->colour[y][x][2] = 9;
            f->tag[y][x] = 1;
            f->written[y][x] = 0;
        }
    }
}

static void test_typed_texture_multi_texel(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float xy[3][2] = {
        { -1, -1 }, { 1, -1 }, { -1, 1 },
    };
    static const struct {
        float s, t;
        uint32_t filter, wrap;
        uint8_t rgb[3];
        bool colored_border;
        uint32_t wrap_t;
    } samples[] = {
        { -0.25f, 0.25f, SGI_GL_NEAREST, SGI_GL_REPEAT, { 0, 255, 0 } },
        { -0.25f, 0.25f, SGI_GL_NEAREST, SGI_GL_CLAMP_TO_EDGE,
          { 255, 0, 0 } },
        { 0.25f, 0.75f, SGI_GL_NEAREST, SGI_GL_CLAMP_TO_EDGE,
          { 0, 0, 255 } },
        { 0.25f, -0.25f, SGI_GL_NEAREST, SGI_GL_REPEAT, { 0, 0, 255 } },
        { 0.25f, -0.25f, SGI_GL_NEAREST, SGI_GL_CLAMP_TO_EDGE,
          { 255, 0, 0 } },
        { 0.375f, 0.625f, SGI_GL_LINEAR, SGI_GL_REPEAT, { 96, 64, 191 } },
        { 0, 0, SGI_GL_LINEAR, SGI_GL_REPEAT, { 128, 128, 128 } },
        { 0, 0, SGI_GL_LINEAR, SGI_GL_CLAMP_TO_EDGE, { 255, 0, 0 } },
        { 1, 0.25f, SGI_GL_NEAREST, SGI_GL_CLAMP, { 0, 255, 0 } },
        { 1, 0.25f, SGI_GL_NEAREST, SGI_GL_CLAMP_TO_BORDER,
          { 64, 128, 191 }, true },
        { 0, 0.25f, SGI_GL_LINEAR, SGI_GL_CLAMP, { 128, 0, 0 } },
        { 0, 0.25f, SGI_GL_LINEAR, SGI_GL_CLAMP,
          { 159, 64, 96 }, true },
        { -0.25f, 0.25f, SGI_GL_LINEAR, SGI_GL_CLAMP_TO_BORDER,
          { 0, 0, 0 } },
        { 0.25f, -0.25f, SGI_GL_NEAREST, SGI_GL_CLAMP_TO_EDGE,
          { 0, 0, 255 }, false, SGI_GL_REPEAT },
    };
    static const struct {
        float s1, t2;
        uint8_t rgb[3];
    } projective[] = {
        { 3, 18, { 255, 255, 255 } },
        { 2, 12, { 255, 0, 0 } },
    };
    NativeDrawFixture f = { 0 };
    SGIRenderResource resources[] = {
        {
            .backing_id = 1, .backing_generation = 1,
            .view_id = 2, .view_generation = 1,
            .role = SGI_RENDER_COLOUR, .target = 7,
            .buffer = SGI_RENDER_FRONT, .eye = SGI_RENDER_MONO,
            .width = 4, .height = 4, .depth = 1,
            .bottom_origin = true, .component_count = 3,
            .component = {
                { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
            },
        },
        {
            .backing_id = 3, .backing_generation = 1,
            .view_id = 4, .view_generation = 1,
            .role = SGI_RENDER_TEXTURE,
            .buffer = SGI_RENDER_BUFFER_NONE, .eye = SGI_RENDER_MONO,
            .width = 2, .height = 2, .depth = 1,
            .bottom_origin = true, .component_count = 4,
            .component = {
                { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_A, SGI_RENDER_UNORM, 8 },
            },
        },
    };
    SGIGLTexImage image = {
        .width = 2, .height = 2, .depth = 1,
        .base_format = SGI_GL_RGBA,
        .min_filter = SGI_GL_NEAREST, .mag_filter = SGI_GL_NEAREST,
        .wrap_s = SGI_GL_REPEAT, .wrap_t = SGI_GL_REPEAT,
        .read_texel = unexpected_texture_read,
    };
    SGIRenderResourceOps ops = {
        .snapshot = native_draw_snapshot,
        .commit_covered = native_draw_commit_covered,
    };
    SGIRasterTarget target = {
        .opaque = &f, .resources = resources, .resource_count = 2,
        .resource_ops = &ops, .texture = &image,
    };
    SGIRasterRules rules = {
        .subpixel_bits = 8, .color_bits = 8,
        .rounding = SGI_ROUND_HALF_AWAY,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE, .evidence = "synthetic",
    };
    SGIGLDrawState state = {
        .shade_model = SGI_GL_FLAT,
        .draw_buffers = SGI_GL_DRAW_FRONT,
        .colour_mask = SGI_GL_COLOUR_MASK_ALL,
        .texture_2d = 1,
        .texture_env_mode = SGI_GL_REPLACE,
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
        vertices[i][SGI_GL_V_COLOR + 3] = 1;
        vertices[i][SGI_GL_V_TEXCOORD + 3] = 1;
    }
    memcpy(f.texels[0][0], (uint32_t[4]) { 255, 0, 0, 255 },
           sizeof(f.texels[0][0]));
    memcpy(f.texels[0][1], (uint32_t[4]) { 0, 255, 0, 255 },
           sizeof(f.texels[0][1]));
    memcpy(f.texels[1][0], (uint32_t[4]) { 0, 0, 255, 255 },
           sizeof(f.texels[1][0]));
    memcpy(f.texels[1][1], (uint32_t[4]) { 255, 255, 255, 255 },
           sizeof(f.texels[1][1]));
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    for (unsigned i = 0; i < ARRAY_SIZE(samples); i++) {
        native_draw_fill_sentinel(&f);
        image.min_filter = image.mag_filter = samples[i].filter;
        image.wrap_s = samples[i].wrap;
        image.wrap_t = samples[i].wrap_t ? samples[i].wrap_t : samples[i].wrap;
        memset(image.border_color, 0, sizeof(image.border_color));
        if (samples[i].colored_border) {
            image.border_color[0] = 0.25f;
            image.border_color[1] = 0.5f;
            image.border_color[2] = 0.75f;
            image.border_color[3] = 1;
        }
        for (unsigned v = 0; v < 3; v++) {
            vertices[v][SGI_GL_V_TEXCOORD] = samples[i].s;
            vertices[v][SGI_GL_V_TEXCOORD + 1] = samples[i].t;
        }
        g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                                 &draw, &err), ==,
                        SGI_RENDER_OK);
        g_assert_null(err);
        g_assert_cmpuint(f.snapshots, ==, (i + 1) * 2);
        g_assert_cmpuint(f.commits, ==, i + 1);
        for (unsigned c = 0; c < 3; c++) {
            g_assert_cmpuint(f.colour[0][0][c], ==, samples[i].rgb[c]);
            g_assert_cmpuint(f.colour[1][1][c], ==, samples[i].rgb[c]);
            g_assert_cmpuint(f.colour[3][3][c], ==, 5 + 2 * c);
        }
        g_assert_cmpuint(f.written[0][0], ==, 1);
        g_assert_cmpuint(f.written[1][1], ==, 1);
        g_assert_cmpuint(f.written[3][3], ==, 0);
        g_assert_cmpuint(f.tag[0][0], ==, 9);
        g_assert_cmpuint(f.tag[3][3], ==, 1);
    }
    image.border_color[0] = NAN;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==,
                    "Unsupported SGI Mesa texture resource or sampler");
    g_clear_pointer(&err, error_free);
    g_assert_cmpuint(f.snapshots, ==, 2 * ARRAY_SIZE(samples));
    image.border_color[0] = 0;
    image.min_filter = image.mag_filter = SGI_GL_NEAREST;
    image.wrap_s = image.wrap_t = SGI_GL_REPEAT;
    vertices[0][SGI_GL_V_POSITION] = -2;
    vertices[1][SGI_GL_V_POSITION] = 2;
    vertices[1][SGI_GL_V_POSITION + 1] = -2;
    vertices[1][SGI_GL_V_POSITION + 3] = 2;
    vertices[2][SGI_GL_V_POSITION] = -4;
    vertices[2][SGI_GL_V_POSITION + 1] = 4;
    vertices[2][SGI_GL_V_POSITION + 3] = 4;
    for (unsigned v = 0; v < 3; v++) {
        vertices[v][SGI_GL_V_TEXCOORD] = 0;
        vertices[v][SGI_GL_V_TEXCOORD + 1] = 0;
    }
    vertices[1][SGI_GL_V_TEXCOORD + 3] = 2;
    vertices[2][SGI_GL_V_TEXCOORD + 3] = 2;
    /*
     * At pixel (0, 0), D=15/16. S=T are 3/5 and 2/5; without Q they
     * are 18/23 and 12/23, so the second case selects a different texel.
     */
    for (unsigned i = 0; i < ARRAY_SIZE(projective); i++) {
        native_draw_fill_sentinel(&f);
        vertices[1][SGI_GL_V_TEXCOORD] = projective[i].s1;
        vertices[2][SGI_GL_V_TEXCOORD + 1] = projective[i].t2;
        g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                                 &draw, &err), ==,
                        SGI_RENDER_OK);
        g_assert_null(err);
        g_assert_cmpuint(f.snapshots, ==,
                         2 * (ARRAY_SIZE(samples) + i + 1));
        g_assert_cmpuint(f.commits, ==, ARRAY_SIZE(samples) + i + 1);
        for (unsigned c = 0; c < 3; c++) {
            g_assert_cmpuint(f.colour[0][0][c], ==,
                             projective[i].rgb[c]);
            g_assert_cmpuint(f.colour[3][3][c], ==, 5 + 2 * c);
        }
        g_assert_cmpuint(f.written[0][0], ==, 1);
        g_assert_cmpuint(f.written[3][3], ==, 0);
    }
    native_draw_fill_sentinel(&f);
    f.fail_texture_snapshot = true;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
    g_assert_cmpuint(f.snapshots, ==,
                     2 * (ARRAY_SIZE(samples) + ARRAY_SIZE(projective) + 1));
    g_assert_cmpuint(f.commits, ==,
                     ARRAY_SIZE(samples) + ARRAY_SIZE(projective));
    g_assert_cmpuint(f.colour[0][0][0], ==, 5);
    g_assert_cmpuint(f.written[0][0], ==, 0);
    f.fail_texture_snapshot = false;
    saved_read_pixels = epoxy_glReadPixels;
    epoxy_glReadPixels = fail_colour_read_pixels;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_HOST_ERROR);
    epoxy_glReadPixels = saved_read_pixels;
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
    g_assert_cmpuint(f.snapshots, ==,
                     2 * (ARRAY_SIZE(samples) + ARRAY_SIZE(projective) + 2));
    g_assert_cmpuint(f.commits, ==,
                     ARRAY_SIZE(samples) + ARRAY_SIZE(projective));
    g_assert_cmpuint(f.colour[0][0][0], ==, 5);
    g_assert_cmpuint(f.written[0][0], ==, 0);
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
    err = NULL;
    frame.written = written;
    frame.clip_count = SGI_RENDERER_CLEAR_RECTS + 1;
    g_assert_false(sgi_renderer_draw_frame(renderer, &frame, &err));
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    frame.clip_count = 1;
    frame.clips[0] = (SGIRendererClearRect) { 2, 0, 1, 0 };
    g_assert_false(sgi_renderer_draw_frame(renderer, &frame, &err));
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    frame.clips[0] = (SGIRendererClearRect) { 0, 0, 4, 0 };
    g_assert_false(sgi_renderer_draw_frame(renderer, &frame, &err));
    g_assert_nonnull(err);
    error_free(err);
    sgi_renderer_free(renderer);
}

static void test_frame_texture_validation(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    float vertices[3 * SGI_RENDERER_FRAME_VERTEX_FLOATS] = { 0 };
    uint16_t rgba[4 * 4 * 4] = { 0 };
    uint8_t written[4 * 4] = { 0 };
    uint32_t texels[4] = { 255, 0, 0, 255 };
    SGIRendererFrame frame = {
        .width = 4, .height = 4, .count = 3,
        .vertices = vertices, .rgba = rgba,
        .written = written, .written_capacity = sizeof(written),
        .texture = {
            .width = 1, .height = 1, .component_bits = 8,
            .env_mode = SGI_GL_REPLACE,
            .wrap_s = SGI_GL_REPEAT, .wrap_t = SGI_GL_REPEAT,
            .slots = texels, .capacity_slots = ARRAY_SIZE(texels),
        },
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

    frame.texture.border_bytes[0] = NAN;
    g_assert_false(sgi_renderer_draw_frame(renderer, &frame, &err));
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==,
                    "Invalid SGI OpenGL texture border colour");
    g_clear_pointer(&err, error_free);

    frame.texture.border_bytes[0] = 0;
    frame.texture.border_bytes[1] = 256;
    g_assert_false(sgi_renderer_draw_frame(renderer, &frame, &err));
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==,
                    "Invalid SGI OpenGL texture border colour");
    g_clear_pointer(&err, error_free);

    frame.texture.border_bytes[1] = 0;
    frame.texture.wrap_s = 0xffffffffu;
    g_assert_false(sgi_renderer_draw_frame(renderer, &frame, &err));
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==,
                    "Invalid SGI OpenGL texture wrap");
    g_clear_pointer(&err, error_free);

    frame.texture.wrap_s = SGI_GL_REPEAT;
    frame.texture.env_mode = 0xffffffffu;
    g_assert_false(sgi_renderer_draw_frame(renderer, &frame, &err));
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==,
                    "Invalid SGI OpenGL texture environment");
    g_clear_pointer(&err, error_free);

    frame.texture.env_mode = SGI_GL_REPLACE;
    frame.texture.environment_bytes[0] = NAN;
    g_assert_false(sgi_renderer_draw_frame(renderer, &frame, &err));
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==,
                    "Invalid SGI OpenGL texture environment colour");
    g_clear_pointer(&err, error_free);

    frame.texture.env_mode = SGI_GL_BLEND;
    frame.texture.environment_bytes[0] = 191.25;
    frame.texture.environment_bytes[1] = 256;
    g_assert_false(sgi_renderer_draw_frame(renderer, &frame, &err));
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==,
                    "Invalid SGI OpenGL texture environment colour");
    g_clear_pointer(&err, error_free);

    frame.texture.environment_bytes[1] = 63.75;
    frame.texture.environment_bytes[2] = 127.5;
    frame.texture.environment_bytes[3] = 255;
    texels[0] = 128;
    texels[1] = 64;
    texels[2] = 192;
    texels[3] = 128;
    frame.shade_model = SGI_GL_FLAT;
    for (unsigned i = 0; i < 4; i++) {
        frame.modelview[i * 5] = 1;
        frame.projection[i * 5] = 1;
    }
    for (unsigned i = 0; i < 3; i++) {
        float *v = &vertices[i * SGI_RENDERER_FRAME_VERTEX_FLOATS];

        v[3] = 1;
        v[4] = 0.25f;
        v[5] = 0.5f;
        v[6] = 0.75f;
        v[7] = 0.5f;
        v[11] = 1;
    }
    vertices[0] = -1;
    vertices[1] = -1;
    vertices[SGI_RENDERER_FRAME_VERTEX_FLOATS] = 1;
    vertices[SGI_RENDERER_FRAME_VERTEX_FLOATS + 1] = -1;
    vertices[2 * SGI_RENDERER_FRAME_VERTEX_FLOATS] = -1;
    vertices[2 * SGI_RENDERER_FRAME_VERTEX_FLOATS + 1] = 1;
    g_assert_true(sgi_renderer_draw_frame(renderer, &frame, &err));
    g_assert_null(err);
    g_assert_cmpuint(written[0], ==, 1);
    g_assert_cmpuint(rgba[0], ==, 128 * 257);
    g_assert_cmpuint(rgba[1], ==, 112 * 257);
    g_assert_cmpuint(rgba[2], ==, 143 * 257);
    g_assert_cmpuint(rgba[3], ==, 64 * 257);
    sgi_renderer_free(renderer);
}

typedef struct NativeClearFixture {
    uint32_t colour[2][4][4][4];
    unsigned writes[2][4][4];
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
            f->writes[bank][y][x]++;
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
    /* Stored alpha matches the rules' alpha precision, as the gate requires. */
    SGIRasterRules rules = {
        .color_bits = 12, .alpha_bits = 12, .evidence = "synthetic",
    };
    SGIRasterOp op = {
        .kind = SGI_RASTER_CLEAR,
        .targets = { 7, 8 }, .ntargets = 2,
        .x0 = 0, .y0 = 0, .x1 = 3, .y1 = 3,
        .clip = {
            .n = 3,
            .r = { { 1, 1, 2, 2 }, { 3, 0, 3, 1 },
                   { 2, 1, 3, 2 } },
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
                { SGI_RENDER_A, SGI_RENDER_UNORM, 12 },
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
    g_assert_cmpuint(f.commits, ==, 6);
    for (unsigned bank = 0; bank < 2; bank++) {
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                bool covered = (x >= 1 && x <= 3 && y >= 1 && y <= 2) ||
                               (x == 3 && y == 0);
                uint32_t original = 17 + bank * 3 + y * 19 + x * 7;

                g_assert_cmpuint(f.writes[bank][y][x], ==, covered ? 1 : 0);
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
    g_assert_cmpuint(f.commits, ==, 6);
    resources[1].width = 4;
    resources[1].height = 4;

    op.clip.n = 0;
    op.clip.empty = 1;
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 2);
    g_assert_cmpuint(f.commits, ==, 6);

    op.clip.empty = 0;
    op.clear_buffers |= SGI_GL_DEPTH_BUFFER_BIT;
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 2);
    g_assert_cmpuint(f.commits, ==, 6);

    op.clear_buffers = SGI_GL_COLOR_BUFFER_BIT;
    op.clip.n = 3;
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
    g_assert_cmpuint(f.commits, ==, 6);
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
    g_assert_cmpuint(f.commits, ==, 11);
    g_assert_cmpuint(f.colour[0][0][0][0], ==, 17);
    g_assert_cmpuint(f.colour[0][0][3][0], ==, 4095);
    g_assert_cmpuint(f.colour[0][3][3][0], ==, 17);
    g_assert_cmpuint(f.colour[1][0][0][0], ==, 17);
    g_assert_cmpuint(f.colour[1][0][3][0], ==, 4095);
    g_assert_cmpuint(f.colour[1][1][1][0], ==, 4095);
    g_assert_cmpuint(f.colour[1][1][2][0], ==, 17);
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
    g_assert_cmpuint(f.commits, ==, 11);
    g_assert_cmpuint(f.colour[0][0][0][0], ==, 17);
    g_assert_cmpuint(f.colour[1][0][0][0], ==, 17);
    sgi_renderer_free(renderer);
}

static void test_decoded_clear_full_screen_commit(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    NativeClearFixture f = { 0 };
    SGIRenderResource resource = {
        .role = SGI_RENDER_COLOUR, .target = 7,
        .buffer = SGI_RENDER_FRONT, .width = 4, .height = 4,
        .depth = 1, .bottom_origin = true, .component_count = 3,
        .component = {
            { SGI_RENDER_R, SGI_RENDER_UNORM, 12 },
            { SGI_RENDER_G, SGI_RENDER_UNORM, 12 },
            { SGI_RENDER_B, SGI_RENDER_UNORM, 12 },
        },
    };
    const SGIRenderResourceOps ops = {
        .snapshot = native_clear_snapshot,
        .commit = native_clear_commit,
    };
    SGIRasterTarget target = {
        .opaque = &f, .resources = &resource, .resource_count = 1,
        .resource_ops = &ops,
    };
    SGIRasterRules rules = { .color_bits = 12, .evidence = "synthetic" };
    SGIRasterOp op = {
        .kind = SGI_RASTER_CLEAR, .targets = { 7 }, .ntargets = 1,
        .x0 = 0, .y0 = 0, .x1 = 3, .y1 = 3,
        .clear_buffers = SGI_GL_COLOR_BUFFER_BIT,
        .clear_rgba = { 1, 0, 0, 1 },
        .clear_colour_mask = SGI_GL_COLOUR_MASK_ALL,
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
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 1);
    g_assert_cmpuint(f.commits, ==, 1);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            g_assert_cmpuint(f.writes[0][y][x], ==, 1);
            g_assert_cmpuint(f.colour[0][y][x][0], ==, 4095);
        }
    }
    memset(f.writes, 0, sizeof(f.writes));
    op.clip.n = 2;
    op.clip.r[0] = (SGIClipRect) { 0, 0, 2, 3 };
    op.clip.r[1] = (SGIClipRect) { 2, 0, 3, 3 };
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.commits, ==, 2);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            g_assert_cmpuint(f.writes[0][y][x], ==, 1);
        }
    }
    sgi_renderer_free(renderer);
}

typedef struct NativeScalarClearFixture {
    uint32_t colour[4][4][3];
    uint32_t depth[4][4];
    uint32_t stencil[4][4];
    unsigned writes[SGI_RENDER_STENCIL + 1][4][4];
    unsigned snapshots, commits;
    SGIRenderRole fail_snapshot_role, fail_commit_role;
    bool fail_snapshot, fail_commit;
} NativeScalarClearFixture;

static SGIRenderResult native_scalar_clear_snapshot(
    void *opaque, const SGIRenderResource *res, uint32_t slice,
    const SGIRenderRect *rect, uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, Error **errp)
{
    NativeScalarClearFixture *f = opaque;

    f->snapshots++;
    if (f->fail_snapshot && res->role == f->fail_snapshot_role) {
        error_setg(errp, "scalar clear snapshot failed");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            size_t at = (y - rect->y0) * stride_slots +
                        (x - rect->x0) * res->component_count;

            if (res->role == SGI_RENDER_COLOUR) {
                memcpy(&slots[at], f->colour[y][x], 3 * sizeof(uint32_t));
            } else if (res->role == SGI_RENDER_DEPTH) {
                slots[at] = f->depth[y][x];
            } else {
                slots[at] = f->stencil[y][x];
            }
        }
    }
    return SGI_RENDER_OK;
}

static SGIRenderCommitResult native_scalar_clear_commit(
    void *opaque, const SGIRenderResource *res, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, Error **errp)
{
    NativeScalarClearFixture *f = opaque;
    uint64_t complete = 0;

    f->commits++;
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            size_t at = (y - rect->y0) * stride_slots +
                        (x - rect->x0) * res->component_count;

            if (f->fail_commit && res->role == f->fail_commit_role &&
                complete == 1) {
                error_setg(errp, "scalar clear commit failed after one pixel");
                return (SGIRenderCommitResult) {
                    .status = SGI_RENDER_RESOURCE_ERROR,
                    .complete_pixels = complete,
                    .extent_known = true,
                };
            }
            f->writes[res->role][y][x]++;
            if (res->role == SGI_RENDER_COLOUR) {
                memcpy(f->colour[y][x], &slots[at], 3 * sizeof(uint32_t));
            } else if (res->role == SGI_RENDER_DEPTH) {
                f->depth[y][x] = slots[at];
            } else {
                f->stencil[y][x] = slots[at];
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

static void test_decoded_clear_depth_stencil(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float probe_vertices[3][4] = {
        { 16.25f, 16.25f, 0, 1 },
        { 111.25f, 16.25f, 0, 1 },
        { 16.25f, 111.25f, 0, 1 },
    };
    g_autofree SGIRendererRequest *probe = g_new0(SGIRendererRequest, 1);
    NativeScalarClearFixture f = { 0 };
    SGIRenderResource resources[3] = {
        {
            .role = SGI_RENDER_COLOUR, .target = 7,
            .buffer = SGI_RENDER_FRONT, .width = 4, .height = 4,
            .depth = 1, .bottom_origin = true, .component_count = 3,
            .component = {
                { SGI_RENDER_R, SGI_RENDER_UNORM, 12 },
                { SGI_RENDER_G, SGI_RENDER_UNORM, 12 },
                { SGI_RENDER_B, SGI_RENDER_UNORM, 12 },
            },
        },
        {
            .role = SGI_RENDER_DEPTH, .width = 4, .height = 4,
            .depth = 1, .bottom_origin = true, .component_count = 1,
            .component = { { SGI_RENDER_SCALAR, SGI_RENDER_UINT, 24 } },
        },
        {
            .role = SGI_RENDER_STENCIL, .width = 4, .height = 4,
            .depth = 1, .bottom_origin = true, .component_count = 1,
            .component = { { SGI_RENDER_SCALAR, SGI_RENDER_UINT, 8 } },
        },
    };
    const SGIRenderResourceOps ops = {
        .snapshot = native_scalar_clear_snapshot,
        .commit = native_scalar_clear_commit,
    };
    SGIRasterTarget target = {
        .opaque = &f, .resources = resources, .resource_count = 3,
        .resource_ops = &ops,
    };
    SGIRasterRules rules = {
        .color_bits = 12, .depth_bits = 24, .stencil_bits = 8,
        .evidence = "synthetic",
    };
    SGIRasterOp op = {
        .kind = SGI_RASTER_CLEAR, .targets = { 7 }, .ntargets = 1,
        .x0 = 0, .y0 = 0, .x1 = 3, .y1 = 3,
        .clip = { .n = 2, .r = {
            { 1, 1, 2, 2 }, { 2, 1, 3, 2 },
        } },
        .clear_buffers = SGI_GL_COLOR_BUFFER_BIT |
                         SGI_GL_DEPTH_BUFFER_BIT |
                         SGI_GL_STENCIL_BUFFER_BIT,
        .clear_rgba = { 1, 0, 0, 1 },
        .clear_colour_mask = SGI_GL_COLOUR_MASK_RED,
        .clear_depth24 = 0x123456,
        .clear_stencil = 0xa5, .stencil_mask = 0x0f,
    };
    SGIRenderer *renderer;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        return;
    }
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            f.colour[y][x][0] = 17;
            f.colour[y][x][1] = 23;
            f.colour[y][x][2] = 31;
            f.depth[y][x] = 0xabcdef;
            f.stencil[y][x] = 0xf0;
        }
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 3);
    g_assert_cmpuint(f.commits, ==, 6);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            bool covered = x >= 1 && x <= 3 && y >= 1 && y <= 2;

            g_assert_cmpuint(f.writes[SGI_RENDER_COLOUR][y][x], ==,
                             covered ? 1 : 0);
            g_assert_cmpuint(f.writes[SGI_RENDER_DEPTH][y][x], ==,
                             covered ? 1 : 0);
            g_assert_cmpuint(f.writes[SGI_RENDER_STENCIL][y][x], ==,
                             covered ? 1 : 0);
            g_assert_cmpuint(f.colour[y][x][0], ==, covered ? 4095 : 17);
            g_assert_cmpuint(f.colour[y][x][1], ==, 23);
            g_assert_cmpuint(f.colour[y][x][2], ==, 31);
            g_assert_cmphex(f.depth[y][x], ==,
                            covered ? 0x123456 : 0xabcdef);
            g_assert_cmphex(f.stencil[y][x], ==, covered ? 0xf5 : 0xf0);
        }
    }
    memcpy(probe->vertices, probe_vertices, sizeof(probe_vertices));
    probe->clip[2] = probe->clip[3] = 127;
    probe->rgb[1] = 255;
    g_assert_true(sgi_renderer_draw(renderer, probe, &err));
    g_assert_null(err);
    g_assert_true(sgi_renderer_staging_valid(probe));

    f.fail_snapshot = true;
    f.fail_snapshot_role = SGI_RENDER_STENCIL;
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 6);
    g_assert_cmpuint(f.commits, ==, 6);
    f.fail_snapshot = false;

    resources[2].component[0].storage_bits = 16;
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 6);
    resources[2].component[0].storage_bits = 8;
    rules.depth_bits = 16;
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 6);
    rules.depth_bits = 24;

    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            f.colour[y][x][0] = 17;
            f.depth[y][x] = 0xabcdef;
            f.stencil[y][x] = 0xf0;
        }
    }
    f.fail_commit = true;
    f.fail_commit_role = SGI_RENDER_STENCIL;
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 9);
    g_assert_cmpuint(f.commits, ==, 11);
    g_assert_cmpuint(f.colour[1][1][0], ==, 4095);
    g_assert_cmphex(f.depth[1][1], ==, 0x123456);
    g_assert_cmphex(f.stencil[1][1], ==, 0xf5);
    g_assert_cmphex(f.stencil[1][2], ==, 0xf0);
    sgi_renderer_free(renderer);

    f.fail_commit = false;
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    op.clear_buffers = SGI_GL_DEPTH_BUFFER_BIT;
    op.clear_depth24 = 0x654321;
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmphex(f.depth[1][1], ==, 0x654321);
    g_assert_cmphex(f.depth[0][0], ==, 0xabcdef);
    g_assert_cmpuint(f.snapshots, ==, 10);
    g_assert_cmpuint(f.commits, ==, 13);

    op.clear_buffers = SGI_GL_STENCIL_BUFFER_BIT;
    op.clear_stencil = 0x0c;
    op.stencil_mask = 0xf0;
    op.clear_rgba[0] = NAN;
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmphex(f.stencil[1][1], ==, 0x05);
    g_assert_cmphex(f.stencil[0][0], ==, 0xf0);
    g_assert_cmpuint(f.snapshots, ==, 11);
    g_assert_cmpuint(f.commits, ==, 15);
    sgi_renderer_free(renderer);

    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    op.clear_buffers = SGI_GL_COLOR_BUFFER_BIT |
                       SGI_GL_DEPTH_BUFFER_BIT |
                       SGI_GL_STENCIL_BUFFER_BIT;
    op.clear_rgba[0] = 1;
    saved_read_pixels = epoxy_glReadPixels;
    epoxy_glReadPixels = fail_scalar_clear_read_pixels;
    g_assert_cmpint(sgi_render_raster_checked(renderer, &rules, &target,
                                               &op, &err), ==,
                    SGI_RENDER_HOST_ERROR);
    epoxy_glReadPixels = saved_read_pixels;
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(f.snapshots, ==, 14);
    g_assert_cmpuint(f.commits, ==, 15);
    sgi_renderer_free(renderer);
}

/*
 * Scalar comparison: one draw through sgi_soft_draw() and through the
 * checked Mesa path, from the same 16x16 native image, must leave the same
 * component values and coverage. Differences are reported per pixel so
 * that every case in a test runs before it fails.
 */
#define PRIM_SIZE 16

typedef struct PrimFixture {
    uint32_t rgba[PRIM_SIZE][PRIM_SIZE][4];
    uint8_t written[PRIM_SIZE][PRIM_SIZE];
    uint32_t depth[PRIM_SIZE][PRIM_SIZE], stencil[PRIM_SIZE][PRIM_SIZE];
    uint32_t texels[4][4][4];
    unsigned components;
    unsigned snapshots, commits;
    /*
     * Native rows run down the display: the scalar path's window row y is
     * native row PRIM_SIZE - 1 - y, as a top-origin family maps it.
     */
    bool top_origin;
} PrimFixture;

static unsigned prim_native_row(const PrimFixture *f, int y)
{
    return f->top_origin ? PRIM_SIZE - 1 - y : y;
}

/* Out-of-plane stores are dropped, as board memory drops them. */
static void prim_scalar_store(void *opaque, SGIColourTarget target, int x,
                              int y, const SGIRasterColour *colour)
{
    PrimFixture *f = opaque;
    unsigned row;

    if (x < 0 || x >= PRIM_SIZE || y < 0 || y >= PRIM_SIZE) {
        return;
    }
    row = prim_native_row(f, y);
    for (unsigned c = 0; c < f->components; c++) {
        f->rgba[row][x][c] = colour->component[c];
    }
    f->written[row][x] = 1;
}

static bool prim_scalar_load(void *opaque, SGIColourTarget target, int x,
                             int y, SGIRasterColour *colour)
{
    PrimFixture *f = opaque;
    unsigned row;

    if (x < 0 || x >= PRIM_SIZE || y < 0 || y >= PRIM_SIZE) {
        return false;
    }
    row = prim_native_row(f, y);
    for (unsigned c = 0; c < 4; c++) {
        colour->component[c] = c < f->components ? f->rgba[row][x][c] : 0;
    }
    return true;
}

static void prim_scalar_store_depth(void *opaque, int x, int y,
                                    uint32_t depth)
{
    PrimFixture *f = opaque;

    if (x >= 0 && x < PRIM_SIZE && y >= 0 && y < PRIM_SIZE) {
        f->depth[y][x] = depth;
    }
}

static uint32_t prim_scalar_load_stencil(void *opaque, int x, int y)
{
    PrimFixture *f = opaque;

    return x >= 0 && x < PRIM_SIZE && y >= 0 && y < PRIM_SIZE ?
           f->stencil[y][x] : 0;
}

static void prim_scalar_store_stencil(void *opaque, int x, int y,
                                      uint32_t value)
{
    PrimFixture *f = opaque;

    if (x >= 0 && x < PRIM_SIZE && y >= 0 && y < PRIM_SIZE) {
        f->stencil[y][x] = value;
    }
}

/* The fixture's slot for component c of pixel (x, y) in a resource. */
static uint32_t *prim_slot(PrimFixture *f, const SGIRenderResource *res,
                           uint32_t x, uint32_t y, unsigned c)
{
    switch (res->role) {
    case SGI_RENDER_TEXTURE:
        return &f->texels[y][x][c];
    case SGI_RENDER_DEPTH:
        return &f->depth[y][x];
    case SGI_RENDER_STENCIL:
        return &f->stencil[y][x];
    default:
        return &f->rgba[y][x][c];
    }
}

static SGIRenderResult prim_snapshot(void *opaque,
                                     const SGIRenderResource *resource,
                                     uint32_t slice, const SGIRenderRect *rect,
                                     uint32_t *slots, size_t stride_slots,
                                     size_t capacity_slots, Error **errp)
{
    PrimFixture *f = opaque;
    unsigned n = resource->component_count;

    f->snapshots++;
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            for (unsigned c = 0; c < n; c++) {
                slots[(y - rect->y0) * stride_slots + (x - rect->x0) * n + c] =
                    *prim_slot(f, resource, x, y, c);
            }
        }
    }
    return SGI_RENDER_OK;
}

static SGIRenderCommitResult prim_commit_covered(
    void *opaque, const SGIRenderResource *res, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots,
    size_t stride_slots, size_t capacity_slots,
    const SGIRenderWriteCoverage *writes, Error **errp)
{
    PrimFixture *f = opaque;
    uint64_t complete = 0;

    f->commits++;
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            /* Every pixel of the rectangle is accounted for, written or not. */
            complete++;
            if (writes && !writes->pixel[(y - rect->y0) *
                                         writes->stride_bytes +
                                         x - rect->x0]) {
                continue;
            }
            for (unsigned c = 0; c < res->component_count; c++) {
                *prim_slot(f, res, x, y, c) =
                    slots[(y - rect->y0) * stride_slots +
                          (x - rect->x0) * res->component_count + c];
            }
            if (res->role == SGI_RENDER_COLOUR) {
                f->written[y][x] = 1;
            }
        }
    }
    return (SGIRenderCommitResult) {
        .status = SGI_RENDER_OK,
        .complete_pixels = complete,
        .extent_known = true,
    };
}

/* Decoded CLEAR commits each clipped rectangle densely. */
static SGIRenderCommitResult prim_commit(void *opaque,
                                         const SGIRenderResource *res,
                                         uint32_t slice,
                                         const SGIRenderRect *rect,
                                         const uint32_t *slots,
                                         size_t stride_slots,
                                         size_t capacity_slots, Error **errp)
{
    return prim_commit_covered(opaque, res, slice, rect, slots, stride_slots,
                               capacity_slots, NULL, errp);
}

typedef struct PrimCase {
    SGIGLDrawState state;
    SGIGLDraw draw;
    float vertices[64][SGI_GL_VERTEX_FLOATS];
    unsigned components;
    /* Nonzero selects one unsupported raster rule for the Mesa path. */
    unsigned rules_case;
    /* Nonzero fills every destination component with dest_value - 1. */
    uint32_t dest_value;
    unsigned bits; /* Colour, alpha and storage bits; zero means 8. */
    /* Nonzero overrides: rules colour bits, rules alpha bits plus one, */
    unsigned rules_colour, rules_alpha_plus_one;
    unsigned alpha_storage; /* and stored alpha bits. */
    /* Native rows run down; see PrimFixture. Clips stay in window rows. */
    bool top_origin;
    /* With state.texture_2d: a level-zero RGBA8 image, rows from T zero. */
    SGIGLTexImage image;
    uint32_t texels[4][4][4];
} PrimCase;

static bool prim_read_texel(void *opaque, uint32_t x, uint32_t y,
                            uint32_t *rgba)
{
    const PrimCase *p = opaque;
    const uint32_t *t = p->texels[y][x];

    *rgba = t[0] << 24 | t[1] << 16 | t[2] << 8 | t[3];
    return true;
}

static void prim_case_init(PrimCase *p, uint32_t mode, unsigned components)
{
    memset(p, 0, sizeof(*p));
    p->components = components;
    p->state = (SGIGLDrawState) {
        .shade_model = SGI_GL_FLAT,
        .draw_buffers = SGI_GL_DRAW_FRONT,
        .colour_mask = SGI_GL_COLOUR_MASK_ALL,
        .drawable = {
            .bound = true,
            .clip = { .n = 1, .r[0] = { 0, 0, PRIM_SIZE - 1,
                                        PRIM_SIZE - 1 } },
            .front = 7,
        },
        .viewport = { 0, 0, PRIM_SIZE, PRIM_SIZE },
        .depth_range = { 0, 1 },
    };
    for (unsigned i = 0; i < 4; i++) {
        p->state.modelview[i * 5] = 1;
        p->state.projection[i * 5] = 1;
    }
    p->draw = (SGIGLDraw) {
        .mode = mode, .vertices = &p->vertices[0][0], .state = &p->state,
    };
}

/* Window coordinates; keep them on the 1/256 grid both paths snap to. */
static void prim_vertex(PrimCase *p, float x, float y, float r, float g,
                        float b, float a)
{
    float *v = p->vertices[p->draw.count++];

    g_assert_cmpuint(p->draw.count, <=, G_N_ELEMENTS(p->vertices));
    v[SGI_GL_V_POSITION] = x * 2 / PRIM_SIZE - 1;
    v[SGI_GL_V_POSITION + 1] = y * 2 / PRIM_SIZE - 1;
    v[SGI_GL_V_POSITION + 3] = 1;
    v[SGI_GL_V_COLOR] = r;
    v[SGI_GL_V_COLOR + 1] = g;
    v[SGI_GL_V_COLOR + 2] = b;
    v[SGI_GL_V_COLOR + 3] = a;
}

/* Distinct colours per vertex, so a wrong provoking vertex shows. */
static void prim_vertex_n(PrimCase *p, float x, float y)
{
    unsigned n = p->draw.count;

    prim_vertex(p, x, y, (37 + 41 * n) % 256 / 255.0f,
                (91 + 23 * n) % 256 / 255.0f, (13 + 67 * n) % 256 / 255.0f,
                (200 + 29 * n) % 256 / 255.0f);
}

static void prim_destination(PrimFixture *f, const PrimCase *p)
{
    unsigned bits = p->bits ? p->bits : 8;
    uint32_t max = (1u << bits) - 1;

    memset(f, 0, sizeof(*f));
    f->components = p->components;
    f->top_origin = p->top_origin;
    memcpy(f->texels, p->texels, sizeof(f->texels));
    for (unsigned y = 0; y < PRIM_SIZE; y++) {
        for (unsigned x = 0; x < PRIM_SIZE; x++) {
            uint32_t v[4] = {
                x * 17 + y * 3, y * 15 + x * 5 + 40, x * y * 7 + 90,
                x * 11 + y * 13 + 7,
            };

            for (unsigned c = 0; c < 4; c++) {
                uint32_t m = c == 3 && p->alpha_storage ?
                             (1u << p->alpha_storage) - 1 : max;

                /* Spread the 8-bit pattern over the whole width. */
                f->rgba[y][x][c] = p->dest_value ? p->dest_value - 1 :
                                   (v[c] & 255) * m / 255;
            }
            f->depth[y][x] = (x * 0x13579 + y * 0x2468ace) & 0xffffff;
            f->stencil[y][x] = (x * 29 + y * 53 + 5) & 255;
        }
    }
}

static void prim_rules(const PrimCase *p, SGIRasterRules *rules)
{
    unsigned bits = p->bits ? p->bits : 8;

    *rules = (SGIRasterRules) {
        .subpixel_bits = 8,
        .color_bits = p->rules_colour ? p->rules_colour : bits,
        .depth_bits = 24,
        .alpha_bits = p->rules_alpha_plus_one ? p->rules_alpha_plus_one - 1 :
                      p->components == 4 ? bits : 0,
        .stencil_bits = 8,
        .rounding = SGI_ROUND_HALF_AWAY,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE, .line_rule = SGI_LINE_COVERAGE_DOUBLE,
        .evidence = "synthetic",
    };
}

static void prim_resources(const PrimCase *p, SGIRenderResource res[2])
{
    unsigned bits = p->bits ? p->bits : 8;

    res[0] = (SGIRenderResource) {
        .backing_id = 1, .backing_generation = 1,
        .view_id = 2, .view_generation = 1,
        .role = SGI_RENDER_COLOUR, .target = 7,
        .buffer = SGI_RENDER_FRONT, .eye = SGI_RENDER_MONO,
        .width = PRIM_SIZE, .height = PRIM_SIZE, .depth = 1,
        .bottom_origin = !p->top_origin, .component_count = p->components,
        .component = {
            { SGI_RENDER_R, SGI_RENDER_UNORM, bits },
            { SGI_RENDER_G, SGI_RENDER_UNORM, bits },
            { SGI_RENDER_B, SGI_RENDER_UNORM, bits },
            { SGI_RENDER_A, SGI_RENDER_UNORM,
              p->alpha_storage ? p->alpha_storage : bits },
        },
    };
    res[1] = (SGIRenderResource) {
        .backing_id = 3, .backing_generation = 1,
        .view_id = 4, .view_generation = 1,
        .role = SGI_RENDER_TEXTURE,
        .buffer = SGI_RENDER_BUFFER_NONE, .eye = SGI_RENDER_MONO,
        .width = p->image.width, .height = p->image.height, .depth = 1,
        .bottom_origin = true, .component_count = 4,
        .component = {
            { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_A, SGI_RENDER_UNORM, 8 },
        },
    };
}

static const SGIRenderResourceOps prim_ops = {
    .snapshot = prim_snapshot,
    .commit = prim_commit,
    .commit_covered = prim_commit_covered,
};

/* Draw through the scalar path into f, as a family's planes would. */
static void prim_scalar_draw(PrimCase *p, PrimFixture *f)
{
    SGIRasterRules rules;
    SGIRasterTarget target = {
        .opaque = f,
        .store_rgba = prim_scalar_store, .load_rgba = prim_scalar_load,
        .texture = p->state.texture_2d ? &p->image : NULL,
    };

    prim_rules(p, &rules);
    p->image.read_texel = prim_read_texel;
    p->image.opaque = p;
    g_assert_true(sgi_raster_rules_supported(&rules));
    g_assert_cmpint(sgi_soft_draw(&rules, &target, &p->draw), ==,
                    SGI_GL_DRAW_OK);
}

/*
 * Draw through the checked Mesa path into f. Its drawable clip arrives in
 * native rows, so a top-origin case flips the window-row clips.
 */
static SGIRenderResult prim_mesa_draw(SGIRenderer *renderer, PrimCase *p,
                                      PrimFixture *f, Error **errp)
{
    SGIRenderResource res[2];
    SGIRasterRules rules;
    SGIGLDrawState state = p->state;
    SGIGLDraw draw = p->draw;
    SGIRasterTarget target = {
        .opaque = f, .resources = res,
        .resource_count = p->state.texture_2d ? 2 : 1,
        .resource_ops = &prim_ops,
        .texture = p->state.texture_2d ? &p->image : NULL,
    };

    prim_rules(p, &rules);
    prim_resources(p, res);
    p->image.read_texel = prim_read_texel;
    p->image.opaque = p;
    if (p->top_origin) {
        for (unsigned i = 0; i < state.drawable.clip.n; i++) {
            SGIClipRect *r = &state.drawable.clip.r[i];
            int32_t y0 = r->y0;

            r->y0 = PRIM_SIZE - 1 - r->y1;
            r->y1 = PRIM_SIZE - 1 - y0;
        }
    }
    draw.state = &state;
    if (p->rules_case == 1) {
        rules.subpixel_bits = 4;
    } else if (p->rules_case == 2) {
        rules.tie_rule = SGI_TIE_TOP_LEFT;
    } else if (p->rules_case == 3) {
        rules.arith = SGI_ARITH_FIXED16;
    } else if (p->rules_case == 4) {
        rules.rounding = SGI_ROUND_HALF_EVEN;
    }
    return sgi_render_draw_checked(renderer, &rules, &target, &draw, errp);
}

/* Returns the number of differing pixels and logs each one. */
static unsigned prim_report(const char *name, const PrimFixture *scalar,
                            const PrimFixture *mesa)
{
    unsigned differences = 0;

    for (int y = PRIM_SIZE - 1; y >= 0; y--) {
        for (unsigned x = 0; x < PRIM_SIZE; x++) {
            bool same = scalar->written[y][x] == mesa->written[y][x];

            for (unsigned c = 0; c < scalar->components; c++) {
                same &= scalar->rgba[y][x][c] == mesa->rgba[y][x][c];
            }
            if (!same) {
                differences++;
                g_test_message("%s: pixel (%u,%d) scalar %s %u,%u,%u,%u "
                               "mesa %s %u,%u,%u,%u", name, x, y,
                               scalar->written[y][x] ? "drawn" : "kept",
                               scalar->rgba[y][x][0], scalar->rgba[y][x][1],
                               scalar->rgba[y][x][2], scalar->rgba[y][x][3],
                               mesa->written[y][x] ? "drawn" : "kept",
                               mesa->rgba[y][x][0], mesa->rgba[y][x][1],
                               mesa->rgba[y][x][2], mesa->rgba[y][x][3]);
            }
        }
    }
    if (differences) {
        GString *rows = g_string_new(NULL);

        /* Coverage maps, top row first: S scalar only, M Mesa only. */
        for (int y = PRIM_SIZE - 1; y >= 0; y--) {
            g_string_append_printf(rows, "%s: row %2d ", name, y);
            for (unsigned x = 0; x < PRIM_SIZE; x++) {
                bool s = scalar->written[y][x], m = mesa->written[y][x];

                g_string_append_c(rows, s && m ? '#' : s ? 'S' :
                                        m ? 'M' : '.');
            }
            g_test_message("%s", rows->str);
            g_string_truncate(rows, 0);
        }
        g_string_free(rows, true);
    }
    return differences;
}

/*
 * Returns the number of differing pixels. A refused draw instead counts
 * one failure unless it is UNSUPPORTED before any snapshot.
 */
static unsigned prim_compare(SGIRenderer *renderer, const char *name,
                             PrimCase *p, bool refused)
{
    g_autofree PrimFixture *scalar = g_new(PrimFixture, 1);
    g_autofree PrimFixture *mesa = g_new(PrimFixture, 1);
    SGIRenderResult result;
    Error *err = NULL;

    prim_destination(scalar, p);
    prim_destination(mesa, p);
    prim_scalar_draw(p, scalar);
    result = prim_mesa_draw(renderer, p, mesa, &err);
    if (refused) {
        if (result == SGI_RENDER_UNSUPPORTED && err && !mesa->snapshots) {
            g_clear_pointer(&err, error_free);
            return 0;
        }
        g_test_message("%s: expected a refusal, got %d after %u snapshots",
                       name, result, mesa->snapshots);
        g_clear_pointer(&err, error_free);
        return 1;
    }
    if (result != SGI_RENDER_OK) {
        g_test_message("%s: checked draw returned %d: %s", name, result,
                       err ? error_get_pretty(err) : "(no error)");
        g_clear_pointer(&err, error_free);
        return PRIM_SIZE * PRIM_SIZE;
    }
    return prim_report(name, scalar, mesa);
}

static void prim_expect(SGIRenderer *renderer, const char *name, PrimCase *p,
                        unsigned *failures)
{
    unsigned differences = prim_compare(renderer, name, p, false);

    if (differences) {
        g_test_message("%s: %u pixels differ", name, differences);
        (*failures)++;
    }
}

static void prim_expect_refused(SGIRenderer *renderer, const char *name,
                                PrimCase *p, unsigned *failures)
{
    if (prim_compare(renderer, name, p, true)) {
        (*failures)++;
    }
}

/* A vertex at window (x, y) under the case's viewport and identity matrices. */
static void prim_vertex_window(PrimCase *p, float x, float y)
{
    const int32_t *vp = p->state.viewport;
    float *v;

    prim_vertex_n(p, 0, 0);
    v = p->vertices[p->draw.count - 1];
    v[SGI_GL_V_POSITION] = (x - vp[0]) * 2.0 / vp[2] - 1;
    v[SGI_GL_V_POSITION + 1] = (y - vp[1]) * 2.0 / vp[3] - 1;
}

/* A triangle reaching past every edge of the 16x16 window. */
static void prim_cover(PrimCase *p)
{
    prim_vertex_window(p, -3.25f, -2.5f);
    prim_vertex_window(p, 30.75f, 1.5f);
    prim_vertex_window(p, 2.5f, 29.25f);
}

static void prim_set_clips(PrimCase *p, const SGIClipRect *r, unsigned n)
{
    p->state.drawable.clip.n = n;
    memcpy(p->state.drawable.clip.r, r, n * sizeof(*r));
}

/* Column-major matrices, as the draw state holds them. */
static void prim_matrices(PrimCase *p, bool perspective)
{
    static const float modelview[16] = {
        0.8660254f, 0.5f, 0, 0,
        -0.5f, 0.8660254f, 0, 0,
        0, 0, 1, 0,
        0.13f, -0.21f, -2.5f, 1,
    };
    static const float ortho[16] = {
        0.9f, 0, 0, 0,
        0, 1.1f, 0, 0,
        0, 0, -0.5f, 0,
        0.05f, -0.07f, -0.25f, 1,
    };
    /* Near 1, far 10, a 90 degree field of view. */
    static const float frustum[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, -11.0f / 9, -1,
        0, 0, -20.0f / 9, 0,
    };

    memcpy(p->state.modelview, modelview, sizeof(modelview));
    memcpy(p->state.projection, perspective ? frustum : ortho,
           sizeof(ortho));
}

static void test_scalar_match_state(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const SGIClipRect disjoint[] = {
        { 1, 1, 5, 4 }, { 9, 2, 14, 6 }, { 3, 10, 6, 15 },
    };
    static const SGIClipRect touching[] = {
        { 0, 0, 7, 15 }, { 8, 0, 15, 7 },
    };
    static const SGIClipRect overlapping[] = {
        { 2, 2, 10, 10 }, { 6, 6, 13, 13 }, { -4, 12, 20, 30 },
    };
    static const int32_t scissors[][4] = {
        { 3, 2, 6, 8 },         /* inside */
        { -4, 10, 16, 10 },     /* crossing two edges */
        { 5, 5, 0, 4 },         /* empty */
    };
    static const int32_t viewports[][4] = {
        { 3, -2, 10, 12 },
        { -8, -8, 40, 40 },
        { -8192, -8192, 16384, 16384 },
    };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned top = 0; top < 4; top++) {
        const char *rows = top & 1 ? "top-origin" : "bottom-origin";
        uint32_t shade = top & 2 ? SGI_GL_SMOOTH : SGI_GL_FLAT;
        g_autofree char *name = NULL;

#define STATE_CASE(fmt, ...) do { \
            p->top_origin = top & 1; \
            p->state.shade_model = shade; \
            name = g_strdup_printf("%s %s " fmt, rows, \
                                   shade == SGI_GL_FLAT ? "flat" : "smooth", \
                                   __VA_ARGS__); \
            prim_expect(renderer, name, p, &failures); \
            g_clear_pointer(&name, g_free); \
        } while (0)
        prim_case_init(p, SGI_GL_TRIANGLES, 4);
        prim_cover(p);
        prim_set_clips(p, disjoint, ARRAY_SIZE(disjoint));
        STATE_CASE("%s", "disjoint clips");

        prim_case_init(p, SGI_GL_TRIANGLES, 4);
        prim_cover(p);
        prim_set_clips(p, touching, ARRAY_SIZE(touching));
        STATE_CASE("%s", "touching clips");

        prim_case_init(p, SGI_GL_TRIANGLES, 3);
        prim_cover(p);
        prim_set_clips(p, overlapping, ARRAY_SIZE(overlapping));
        STATE_CASE("%s", "overlapping clips");

        for (unsigned i = 0; i < ARRAY_SIZE(scissors); i++) {
            prim_case_init(p, SGI_GL_TRIANGLES, 4);
            prim_cover(p);
            prim_set_clips(p, disjoint, ARRAY_SIZE(disjoint) - 1);
            p->state.scissor_test = 1;
            memcpy(p->state.scissor, scissors[i], sizeof(scissors[i]));
            STATE_CASE("scissor %d,%d %dx%d", scissors[i][0],
                       scissors[i][1], scissors[i][2], scissors[i][3]);
        }

        for (unsigned i = 0; i < ARRAY_SIZE(viewports); i++) {
            prim_case_init(p, SGI_GL_TRIANGLE_FAN, 4);
            memcpy(p->state.viewport, viewports[i], sizeof(viewports[i]));
            prim_vertex_window(p, 1.25f, 0.75f);
            prim_vertex_window(p, 14.5f, 2.125f);
            prim_vertex_window(p, 12.75f, 13.5f);
            prim_vertex_window(p, 2.375f, 11.25f);
            STATE_CASE("viewport %d,%d %dx%d", viewports[i][0],
                       viewports[i][1], viewports[i][2], viewports[i][3]);
        }

        for (unsigned perspective = 0; perspective < 2; perspective++) {
            static const float object[][3] = {
                { -0.83f, -0.61f, 0.37f }, { 0.77f, -0.29f, -0.42f },
                { 0.18f, 0.93f, 0.11f }, { -0.55f, 0.47f, -0.66f },
            };

            prim_case_init(p, SGI_GL_TRIANGLE_FAN, 4);
            prim_matrices(p, perspective);
            for (unsigned v = 0; v < ARRAY_SIZE(object); v++) {
                float *vertex;

                prim_vertex_n(p, 0, 0);
                vertex = p->vertices[p->draw.count - 1];
                memcpy(&vertex[SGI_GL_V_POSITION], object[v],
                       sizeof(object[v]));
            }
            STATE_CASE("%s matrices",
                       perspective ? "perspective" : "orthographic");
        }
#undef STATE_CASE
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * Colours whose exact product with 255 is a half step: 0.9 * 255 = 229.5.
 * The scalar path rounds the float input in double; the result must not
 * depend on float32 arithmetic in the worker.
 */
static void test_scalar_match_colour_ties(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float ties[][4] = {
        { 0.9f, 0.1f, 0.3f, 0.7f },
        { 0.5f, 0.7f, 0.9f, 0.1f },
        { 0.3f, 0.5f, 0.1f, 0.9f },
    };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned components = 3; components <= 4; components++) {
        for (unsigned i = 0; i < ARRAY_SIZE(ties); i++) {
            const float *c = ties[i];
            g_autofree char *name = NULL;

            prim_case_init(p, SGI_GL_TRIANGLE_FAN, components);
            prim_vertex(p, 1, 1, 0.2f, 0.4f, 0.6f, 0.8f);
            prim_vertex(p, 14, 2, 0.2f, 0.4f, 0.6f, 0.8f);
            prim_vertex(p, 13, 13, 0.2f, 0.4f, 0.6f, 0.8f);
            prim_vertex(p, 2, 12, c[0], c[1], c[2], c[3]);
            name = g_strdup_printf("flat fan rgb%s %u",
                                   components == 4 ? "a" : "", i);
            prim_expect(renderer, name, p, &failures);
            g_clear_pointer(&name, g_free);

            prim_case_init(p, SGI_GL_POINTS, components);
            p->draw.point_size = 3;
            prim_vertex(p, 4.5f, 4.5f, c[0], c[1], c[2], c[3]);
            prim_vertex(p, 11.5f, 10.5f, c[3], c[2], c[1], c[0]);
            name = g_strdup_printf("points rgb%s %u",
                                   components == 4 ? "a" : "", i);
            prim_expect(renderer, name, p, &failures);
        }
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * Draw p into f through the scalar path or Mesa, returning the pixels this
 * draw wrote in written.
 */
static bool prim_draw_into(SGIRenderer *renderer, PrimCase *p, PrimFixture *f,
                           bool mesa, uint8_t written[PRIM_SIZE][PRIM_SIZE])
{
    Error *err = NULL;

    memset(f->written, 0, sizeof(f->written));
    if (mesa) {
        SGIRenderResult result = prim_mesa_draw(renderer, p, f, &err);

        if (result != SGI_RENDER_OK) {
            g_test_message("checked draw returned %d: %s", result,
                           err ? error_get_pretty(err) : "(no error)");
            g_clear_pointer(&err, error_free);
            return false;
        }
    } else {
        prim_scalar_draw(p, f);
    }
    memcpy(written, f->written, sizeof(f->written));
    return true;
}

/*
 * Two triangles sharing an edge through pixel centres, one through each
 * path, must leave the image an all-scalar frame leaves, with no pixel
 * written by both.
 */
static void test_scalar_match_shared_edges(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const struct {
        const char *name;
        float a[3][2], b[3][2];
    } edges[] = {
        { "horizontal", { { 2, 4.5f }, { 14, 4.5f }, { 8, 12 } },
                        { { 14, 4.5f }, { 2, 4.5f }, { 8, 1 } } },
        { "vertical", { { 7.5f, 1 }, { 7.5f, 14 }, { 2, 8 } },
                      { { 7.5f, 14 }, { 7.5f, 1 }, { 13, 8 } } },
        { "diagonal", { { 1.5f, 1.5f }, { 13.5f, 13.5f }, { 1.5f, 13.5f } },
                      { { 13.5f, 13.5f }, { 1.5f, 1.5f }, { 13.5f, 1.5f } } },
        { "steep", { { 4.5f, 0.5f }, { 9.5f, 15.5f }, { 1.5f, 12.5f } },
                   { { 9.5f, 15.5f }, { 4.5f, 0.5f }, { 14.5f, 3.5f } } },
    };
    PrimCase *a = g_new(PrimCase, 1), *b = g_new(PrimCase, 1);
    g_autofree PrimFixture *expected = g_new(PrimFixture, 1);
    g_autofree PrimFixture *mixed = g_new(PrimFixture, 1);
    uint8_t wa[PRIM_SIZE][PRIM_SIZE], wb[PRIM_SIZE][PRIM_SIZE];
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(a);
        g_free(b);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned i = 0; i < ARRAY_SIZE(edges); i++) {
        for (unsigned variant = 0; variant < 8; variant++) {
            bool top = variant & 1, mesa_first = variant & 2;
            bool smooth = variant & 4;
            g_autofree char *name = g_strdup_printf(
                "%s %s edge, %s, Mesa draws %s", smooth ? "smooth" : "flat",
                edges[i].name, top ? "top-origin" : "bottom-origin",
                mesa_first ? "the first" : "the second");
            unsigned differences, twice = 0;

            prim_case_init(a, SGI_GL_TRIANGLES, 4);
            prim_case_init(b, SGI_GL_TRIANGLES, 4);
            a->top_origin = b->top_origin = top;
            for (unsigned v = 0; v < 3; v++) {
                prim_vertex(a, edges[i].a[v][0], edges[i].a[v][1],
                            0.9f, 0.1f + v * 0.3f, 0.2f, 1);
                prim_vertex(b, edges[i].b[v][0], edges[i].b[v][1],
                            0.1f, 0.8f, 0.3f + v * 0.25f, 1);
            }
            if (smooth) {
                a->state.shade_model = b->state.shade_model = SGI_GL_SMOOTH;
            }
            prim_destination(expected, a);
            prim_destination(mixed, a);
            g_assert_true(prim_draw_into(renderer, a, expected, false, wa));
            g_assert_true(prim_draw_into(renderer, b, expected, false, wb));
            for (unsigned y = 0; y < PRIM_SIZE; y++) {
                for (unsigned x = 0; x < PRIM_SIZE; x++) {
                    g_assert_false(wa[y][x] && wb[y][x]);
                    expected->written[y][x] = wa[y][x] || wb[y][x];
                }
            }
            if (!prim_draw_into(renderer, a, mixed, mesa_first, wa) ||
                !prim_draw_into(renderer, b, mixed, !mesa_first, wb)) {
                failures++;
                continue;
            }
            for (unsigned y = 0; y < PRIM_SIZE; y++) {
                for (unsigned x = 0; x < PRIM_SIZE; x++) {
                    mixed->written[y][x] = wa[y][x] || wb[y][x];
                    if (wa[y][x] && wb[y][x]) {
                        g_test_message("%s: pixel (%u,%u) written twice",
                                       name, x, y);
                        twice++;
                    }
                }
            }
            differences = prim_report(name, expected, mixed);
            if (differences || twice) {
                g_test_message("%s: %u pixels differ, %u written twice",
                               name, differences, twice);
                failures++;
            }
        }
    }
    sgi_renderer_free(renderer);
    g_free(a);
    g_free(b);
    if (failures) {
        g_test_fail();
    }
}

/*
 * Unblended draws at component widths other than 8. The scalar rules
 * stop at 10 bits, so 12- and 16-bit storage has no scalar comparison.
 * Stored alpha of one bit is refused: the scalar path rounds incoming
 * alpha to at least two bits before truncating it to the store.
 */
static void test_scalar_match_widths(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const unsigned widths[] = { 1, 2, 4, 5, 6, 10 };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned w = 0; w < ARRAY_SIZE(widths); w++) {
        for (unsigned components = 3; components <= 4; components++) {
            const char *rgb = components == 4 ? "rgba" : "rgb";
            g_autofree char *name = NULL;

            prim_case_init(p, SGI_GL_TRIANGLE_FAN, components);
            p->bits = widths[w];
            prim_vertex_n(p, 1, 1.25f);
            prim_vertex_n(p, 14.5f, 2);
            prim_vertex_n(p, 13, 13.75f);
            prim_vertex_n(p, 2.25f, 12);
            prim_vertex(p, 0.5f, 6, 0.9f, 0.1f, 0.3f, 0.7f);
            for (unsigned smooth = 0; smooth < 2; smooth++) {
                p->state.shade_model = smooth ? SGI_GL_SMOOTH : SGI_GL_FLAT;
                g_clear_pointer(&name, g_free);
                name = g_strdup_printf("%u-bit %s %s fan", widths[w], rgb,
                                       smooth ? "smooth" : "flat");
                if (widths[w] == 1 && components == 4) {
                    prim_expect_refused(renderer, name, p, &failures);
                } else {
                    prim_expect(renderer, name, p, &failures);
                }
            }
            g_clear_pointer(&name, g_free);

            prim_case_init(p, SGI_GL_POINTS, components);
            p->bits = widths[w];
            p->draw.point_size = 2;
            for (unsigned i = 0; i < 6; i++) {
                prim_vertex(p, 2.5f + 2 * i, 3 + 1.75f * i, i / 5.0f,
                            1 - i / 5.0f, 0.5f, (i * 37 % 11) / 10.0f);
            }
            name = g_strdup_printf("%u-bit %s points", widths[w], rgb);
            if (widths[w] == 1 && components == 4) {
                prim_expect_refused(renderer, name, p, &failures);
            } else {
                prim_expect(renderer, name, p, &failures);
            }
        }
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

static void prim_texture(PrimCase *p, unsigned size, uint32_t filter,
                         uint32_t wrap_s, uint32_t wrap_t, uint32_t env)
{
    static const float coords[3][2] = {
        { -0.6f, -0.35f }, { 1.7f, 0.2f }, { 0.45f, 1.9f },
    };

    p->state.texture_2d = 1;
    p->state.texture_env_mode = env;
    p->state.texture_env_color[0] = 0.2f;
    p->state.texture_env_color[1] = 0.6f;
    p->state.texture_env_color[2] = 0.85f;
    p->state.texture_env_color[3] = 0.4f;
    p->image = (SGIGLTexImage) {
        .width = size, .height = size, .depth = 1,
        .base_format = SGI_GL_RGBA,
        .min_filter = filter, .mag_filter = filter,
        .wrap_s = wrap_s, .wrap_t = wrap_t,
        .border_color = { 0.25f, 0.5f, 0.75f, 0.6f },
    };
    for (unsigned y = 0; y < size; y++) {
        for (unsigned x = 0; x < size; x++) {
            uint32_t *t = p->texels[y][x];

            t[0] = (x * 83 + y * 29 + 11) & 255;
            t[1] = (x * 47 + y * 101 + 60) & 255;
            t[2] = (x * 13 + y * 157 + 200) & 255;
            t[3] = (x * 71 + y * 37 + 90) & 255;
        }
    }
    prim_vertex(p, 0.5f, 1.25f, 0.3f, 0.65f, 0.95f, 0.55f);
    prim_vertex(p, 15.25f, 2.5f, 0.3f, 0.65f, 0.95f, 0.55f);
    prim_vertex(p, 3.75f, 15.5f, 0.3f, 0.65f, 0.95f, 0.55f);
    for (unsigned v = 0; v < 3; v++) {
        p->vertices[v][SGI_GL_V_TEXCOORD] = coords[v][0];
        p->vertices[v][SGI_GL_V_TEXCOORD + 1] = coords[v][1];
        p->vertices[v][SGI_GL_V_TEXCOORD + 3] = 1;
    }
}

static void test_scalar_match_texture(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const uint32_t envs[] = {
        SGI_GL_REPLACE, SGI_GL_MODULATE, SGI_GL_DECAL, SGI_GL_BLEND,
    };
    static const uint32_t wraps[][2] = {
        { SGI_GL_REPEAT, SGI_GL_REPEAT },
        { SGI_GL_CLAMP, SGI_GL_CLAMP },
        { SGI_GL_CLAMP_TO_EDGE, SGI_GL_CLAMP_TO_EDGE },
        { SGI_GL_CLAMP_TO_BORDER, SGI_GL_CLAMP_TO_BORDER },
        { SGI_GL_REPEAT, SGI_GL_CLAMP_TO_BORDER },
        { SGI_GL_CLAMP, SGI_GL_CLAMP_TO_EDGE },
    };
    static const uint32_t filters[] = { SGI_GL_NEAREST, SGI_GL_LINEAR };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0, cases = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(true)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned components = 3; components <= 4; components++) {
        for (unsigned size = 2; size <= 4; size += 2) {
            for (unsigned e = 0; e < ARRAY_SIZE(envs); e++) {
                for (unsigned f = 0; f < ARRAY_SIZE(filters); f++) {
                    for (unsigned w = 0; w < ARRAY_SIZE(wraps); w++) {
                        g_autofree char *name = g_strdup_printf(
                            "texture rgb%s %ux%u env 0x%x filter 0x%x "
                            "wrap 0x%x 0x%x", components == 4 ? "a" : "",
                            size, size, envs[e], filters[f], wraps[w][0],
                            wraps[w][1]);

                        prim_case_init(p, SGI_GL_TRIANGLES, components);
                        prim_texture(p, size, filters[f], wraps[w][0],
                                     wraps[w][1], envs[e]);
                        prim_expect(renderer, name, p, &failures);
                        cases++;
                    }
                }
            }
        }
    }
    g_test_message("texture: %u of %u cases differ", failures, cases);
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/* One decoded CLEAR through sgi_soft_raster() and the checked Mesa path. */
static unsigned prim_clear_compare(SGIRenderer *renderer, const char *name,
                                   PrimCase *p, const SGIRasterOp *op)
{
    g_autofree PrimFixture *scalar = g_new(PrimFixture, 1);
    g_autofree PrimFixture *mesa = g_new(PrimFixture, 1);
    SGIRenderResource res[4];
    SGIRasterRules rules;
    SGIRasterTarget scalar_target = {
        .opaque = scalar,
        .store_rgba = prim_scalar_store, .load_rgba = prim_scalar_load,
        .store_depth = prim_scalar_store_depth,
        .load_stencil = prim_scalar_load_stencil,
        .store_stencil = prim_scalar_store_stencil,
    };
    SGIRasterTarget mesa_target = {
        .opaque = mesa, .resources = res, .resource_count = 3,
        .resource_ops = &prim_ops,
    };
    SGIRenderResult result;
    Error *err = NULL;
    unsigned differences;

    prim_rules(p, &rules);
    prim_resources(p, res);
    res[1] = (SGIRenderResource) {
        .backing_id = 5, .backing_generation = 1,
        .view_id = 6, .view_generation = 1,
        .role = SGI_RENDER_DEPTH, .width = PRIM_SIZE, .height = PRIM_SIZE,
        .depth = 1, .bottom_origin = true, .component_count = 1,
        .component = { { SGI_RENDER_SCALAR, SGI_RENDER_UINT, 24 } },
    };
    res[2] = (SGIRenderResource) {
        .backing_id = 7, .backing_generation = 1,
        .view_id = 8, .view_generation = 1,
        .role = SGI_RENDER_STENCIL, .width = PRIM_SIZE, .height = PRIM_SIZE,
        .depth = 1, .bottom_origin = true, .component_count = 1,
        .component = { { SGI_RENDER_SCALAR, SGI_RENDER_UINT, 8 } },
    };
    prim_destination(scalar, p);
    prim_destination(mesa, p);
    g_assert_true(sgi_raster_rules_supported(&rules));
    sgi_soft_raster(&rules, &scalar_target, op);
    result = sgi_render_raster_checked(renderer, &rules, &mesa_target, op,
                                       &err);
    if (result != SGI_RENDER_OK) {
        g_test_message("%s: checked clear returned %d: %s", name, result,
                       err ? error_get_pretty(err) : "(no error)");
        g_clear_pointer(&err, error_free);
        return PRIM_SIZE * PRIM_SIZE;
    }
    differences = prim_report(name, scalar, mesa);
    for (unsigned y = 0; y < PRIM_SIZE; y++) {
        for (unsigned x = 0; x < PRIM_SIZE; x++) {
            if (scalar->depth[y][x] != mesa->depth[y][x] ||
                scalar->stencil[y][x] != mesa->stencil[y][x]) {
                g_test_message("%s: pixel (%u,%u) scalar depth 0x%x "
                               "stencil 0x%x mesa depth 0x%x stencil 0x%x",
                               name, x, y, scalar->depth[y][x],
                               scalar->stencil[y][x], mesa->depth[y][x],
                               mesa->stencil[y][x]);
                differences++;
            }
        }
    }
    return differences;
}

/*
 * The op's rectangle is the union of its clips, bounded by a scissor, as
 * sgi_gl_clear() builds it.
 */
static void prim_clear_op(SGIRasterOp *op, const SGIClipRect *clips,
                          unsigned n, const int32_t *scissor)
{
    op->kind = SGI_RASTER_CLEAR;
    op->targets[0] = 7;
    op->ntargets = 1;
    op->clip.n = n;
    memcpy(op->clip.r, clips, n * sizeof(*clips));
    op->x0 = op->y0 = INT32_MAX;
    op->x1 = op->y1 = INT32_MIN;
    for (unsigned i = 0; i < n; i++) {
        op->x0 = MIN(op->x0, clips[i].x0);
        op->y0 = MIN(op->y0, clips[i].y0);
        op->x1 = MAX(op->x1, clips[i].x1);
        op->y1 = MAX(op->y1, clips[i].y1);
    }
    if (scissor) {
        op->x0 = MAX(op->x0, scissor[0]);
        op->y0 = MAX(op->y0, scissor[1]);
        op->x1 = MIN(op->x1, scissor[0] + scissor[2] - 1);
        op->y1 = MIN(op->y1, scissor[1] + scissor[3] - 1);
    }
}

static void test_scalar_match_clear(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const SGIClipRect full[] = { { 0, 0, 15, 15 } };
    static const SGIClipRect several[] = {
        { 1, 1, 5, 4 }, { 4, 3, 12, 9 }, { 13, 0, 15, 15 },
    };
    static const SGIClipRect outside[] = { { -3, 10, 20, 22 } };
    static const int32_t scissor[4] = { 2, 2, 11, 9 };
    static const struct {
        const SGIClipRect *clips;
        unsigned n;
        const int32_t *scissor;
        const char *name;
    } shapes[] = {
        { full, 1, NULL, "full" },
        { several, 3, NULL, "several clips" },
        { several, 3, scissor, "several clips and scissor" },
        { outside, 1, NULL, "clip past the plane" },
    };
    static const float colours[][4] = {
        { 0.9f, 0.1f, 0.3f, 0.7f },
        { 0.25f, 0.5f, 0.75f, 1 },
    };
    static const uint32_t masks[] = {
        SGI_GL_COLOUR_MASK_ALL,
        SGI_GL_COLOUR_MASK_RED | SGI_GL_COLOUR_MASK_BLUE,
        SGI_GL_COLOUR_MASK_GREEN | SGI_GL_COLOUR_MASK_ALPHA,
    };
    static const unsigned widths[] = { 4, 8, 10 };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0, cases = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned w = 0; w < ARRAY_SIZE(widths); w++) {
        for (unsigned components = 3; components <= 4; components++) {
            for (unsigned s = 0; s < ARRAY_SIZE(shapes); s++) {
                for (unsigned k = 0; k < ARRAY_SIZE(colours) *
                                         ARRAY_SIZE(masks); k++) {
                    const float *colour = colours[k % ARRAY_SIZE(colours)];
                    uint32_t mask = masks[k / ARRAY_SIZE(colours)];
                    SGIRasterOp op = { 0 };
                    g_autofree char *name = g_strdup_printf(
                        "clear %u-bit rgb%s %s colour %u mask 0x%x",
                        widths[w], components == 4 ? "a" : "",
                        shapes[s].name, k % 2, mask);
                    unsigned differences;

                    prim_case_init(p, SGI_GL_TRIANGLES, components);
                    p->bits = widths[w];
                    prim_clear_op(&op, shapes[s].clips, shapes[s].n,
                                  shapes[s].scissor);
                    op.clear_buffers = SGI_GL_COLOR_BUFFER_BIT;
                    if (k & 1) {
                        op.clear_buffers |= SGI_GL_DEPTH_BUFFER_BIT |
                                            SGI_GL_STENCIL_BUFFER_BIT;
                    }
                    memcpy(op.clear_rgba, colour, sizeof(op.clear_rgba));
                    op.clear_colour_mask = mask;
                    op.clear_depth24 = 0x9abcde;
                    op.clear_stencil = 0xa5;
                    op.stencil_mask = 0x3c;
                    differences = prim_clear_compare(renderer, name, p, &op);
                    if (differences) {
                        g_test_message("%s: %u pixels differ", name,
                                       differences);
                        failures++;
                    }
                    cases++;
                }
            }
        }
    }
    /* One-bit stored alpha: the scalar path rounds alpha to two bits. */
    {
        SGIRasterOp op = { 0 };
        SGIRasterRules rules;
        SGIRenderResource res[2];
        g_autofree PrimFixture *mesa = g_new(PrimFixture, 1);
        SGIRasterTarget target = {
            .opaque = mesa, .resources = res, .resource_count = 1,
            .resource_ops = &prim_ops,
        };

        prim_case_init(p, SGI_GL_TRIANGLES, 4);
        p->bits = 1;
        prim_rules(p, &rules);
        prim_resources(p, res);
        prim_destination(mesa, p);
        prim_clear_op(&op, full, 1, NULL);
        op.clear_buffers = SGI_GL_COLOR_BUFFER_BIT;
        op.clear_colour_mask = SGI_GL_COLOUR_MASK_ALL;
        op.clear_rgba[3] = 0.7f;
        if (sgi_render_raster_checked(renderer, &rules, &target, &op,
                                      &err) != SGI_RENDER_UNSUPPORTED ||
            mesa->snapshots) {
            g_test_message("one-bit alpha clear was not refused");
            failures++;
        }
        g_clear_pointer(&err, error_free);
    }
    g_test_message("clear: %u of %u cases differ", failures, cases);
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * Stores whose widths differ from the rules' are refused, for draws and
 * CLEAR, because the scalar path rounds to the rules' widths: with 10-bit
 * colour and 2-bit stored alpha, alpha 0.5 rounds to 512 of 1023 and
 * truncates to 1 of 3, where rounding straight to the store gives 2. Under
 * rules without alpha, as Odyssey pairs its rules with RGBA8, the scalar
 * path hands the store canonical zero alpha where Mesa would store the
 * colour's alpha, so that pairing is refused too.
 */
static void test_scalar_match_store_widths(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const struct {
        const char *name;
        unsigned bits, rules_colour, rules_alpha_plus_one, alpha_storage;
        unsigned components;
        bool refused;
    } cases[] = {
        { "10-bit colour, 2-bit alpha", 10, 0, 3, 2, 4, true },
        { "8-bit store, 6-bit rules", 8, 6, 0, 0, 3, true },
        { "RGB store, 8-bit alpha rules", 8, 0, 9, 0, 3, false },
        { "RGBA8 store, 4-bit alpha rules", 8, 0, 5, 4, 4, true },
        { "RGBA8 store, rules without alpha", 8, 0, 1, 0, 4, true },
        { "RGBA10 store, 10-bit alpha rules", 10, 0, 0, 0, 4, false },
        { "RGBA4 store, 4-bit alpha rules", 4, 0, 0, 0, 4, false },
    };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        for (unsigned kind = 0; kind < 4; kind++) {
            static const char *const kinds[] = {
                "flat fan", "smooth fan", "points", "clear",
            };
            g_autofree char *name = g_strdup_printf("%s %s", cases[i].name,
                                                    kinds[kind]);

            prim_case_init(p, kind == 2 ? SGI_GL_POINTS : SGI_GL_TRIANGLE_FAN,
                           cases[i].components);
            p->bits = cases[i].bits;
            p->rules_colour = cases[i].rules_colour;
            p->rules_alpha_plus_one = cases[i].rules_alpha_plus_one;
            p->alpha_storage = cases[i].alpha_storage;
            p->state.shade_model = kind == 1 ? SGI_GL_SMOOTH : SGI_GL_FLAT;
            p->draw.point_size = 2;
            prim_vertex(p, 1, 1.25f, 0.5f, 0.25f, 0.75f, 0.5f);
            prim_vertex(p, 14.5f, 2, 0.9f, 0.1f, 0.3f, 0.7f);
            prim_vertex(p, 13, 13.75f, 0.2f, 0.6f, 0.4f, 0.35f);
            prim_vertex(p, 2.25f, 12, 0.5f, 0.5f, 0.5f, 0.5f);
            if (kind == 3) {
                static const SGIClipRect full[] = { { 0, 0, 15, 15 } };
                SGIRasterOp op = { 0 };
                SGIRasterRules rules;
                SGIRenderResource res[2];
                g_autofree PrimFixture *mesa = g_new(PrimFixture, 1);
                SGIRasterTarget target = {
                    .opaque = mesa, .resources = res, .resource_count = 1,
                    .resource_ops = &prim_ops,
                };
                SGIRenderResult result;

                prim_clear_op(&op, full, 1, NULL);
                op.clear_buffers = SGI_GL_COLOR_BUFFER_BIT;
                op.clear_colour_mask = SGI_GL_COLOUR_MASK_ALL;
                memcpy(op.clear_rgba, (float[4]) { 0.5f, 0.25f, 0.9f, 0.5f },
                       sizeof(op.clear_rgba));
                if (!cases[i].refused) {
                    unsigned d = prim_clear_compare(renderer, name, p, &op);

                    if (d) {
                        g_test_message("%s: %u pixels differ", name, d);
                        failures++;
                    }
                    continue;
                }
                prim_rules(p, &rules);
                prim_resources(p, res);
                prim_destination(mesa, p);
                result = sgi_render_raster_checked(renderer, &rules, &target,
                                                   &op, &err);
                if (result != SGI_RENDER_UNSUPPORTED || mesa->snapshots) {
                    g_test_message("%s: expected a refusal, got %d", name,
                                   result);
                    failures++;
                }
                g_clear_pointer(&err, error_free);
            } else if (cases[i].refused) {
                prim_expect_refused(renderer, name, p, &failures);
            } else {
                prim_expect(renderer, name, p, &failures);
            }
        }
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

static void test_scalar_match_points(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float centres[][2] = {
        { 2.5f, 2.5f }, { 6.25f, 3.75f }, { 10, 4 }, { 13.75f, 2.5f },
        { 3, 9.5f }, { 7.5f, 10.25f }, { 11.0f, 11.0f }, { 13.5f, 8 },
    };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned size = 1; size <= 4; size++) {
        g_autofree char *name = g_strdup_printf("points size %u", size);

        prim_case_init(p, SGI_GL_POINTS, 4);
        p->draw.point_size = size;
        for (unsigned i = 0; i < G_N_ELEMENTS(centres); i++) {
            prim_vertex_n(p, centres[i][0], centres[i][1]);
        }
        prim_expect(renderer, name, p, &failures);
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * With offset zero several endpoints sit exactly on a pixel's diamond
 * boundary; a small offset on the 1/256 grid moves every endpoint off it.
 */
static void prim_lines(PrimCase *p, uint32_t mode, float width, float offset)
{
    static const float ends[][4] = {
        { 1.5f, 1.5f, 6.5f, 1.5f },      /* x-major, centres */
        { 8, 1, 14, 3.25f },              /* shallow, off centre */
        { 1.5f, 3, 1.5f, 8.5f },          /* vertical */
        { 3, 3, 8, 8 },                   /* slope one */
        { 14, 4, 9, 9 },                  /* slope minus one, reversed */
        { 10.25f, 14.75f, 12.5f, 6.25f }, /* steep, downward */
        { 2.75f, 14.25f, 7.5f, 11 },      /* shallow, downward */
    };

    prim_case_init(p, mode, 4);
    p->draw.line_width = width;
    for (unsigned i = 0; i < G_N_ELEMENTS(ends); i++) {
        prim_vertex_n(p, ends[i][0] + offset, ends[i][1] + offset * 2);
        prim_vertex_n(p, ends[i][2] + offset, ends[i][3] + offset * 2);
    }
}

/*
 * Lines stay unsupported. Against the scalar diamond-exit rule, llvmpipe
 * omits a width-one start pixel on an exact diamond boundary and differs
 * by a row or column at the ends of even widths; these are the vectors
 * that showed it.
 */
static void test_lines_refused(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const struct {
        const char *name;
        uint32_t mode;
        float width, offset;
    } cases[] = {
        { "lines width 1", SGI_GL_LINES, 1, 0 },
        { "line strip width 1", SGI_GL_LINE_STRIP, 1, 0 },
        { "line loop width 1", SGI_GL_LINE_LOOP, 1, 0 },
        { "lines width 2", SGI_GL_LINES, 2, 0 },
        { "lines width 3", SGI_GL_LINES, 3, 0 },
        { "off-tie lines width 1", SGI_GL_LINES, 1, 3.0f / 256 },
        { "off-tie line strip width 1", SGI_GL_LINE_STRIP, 1, 3.0f / 256 },
        { "off-tie line loop width 1", SGI_GL_LINE_LOOP, 1, 3.0f / 256 },
        { "off-tie lines width 2", SGI_GL_LINES, 2, 3.0f / 256 },
        { "off-tie lines width 3", SGI_GL_LINES, 3, 3.0f / 256 },
        { "off-tie lines width 4", SGI_GL_LINES, 4, 3.0f / 256 },
    };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        prim_lines(p, cases[i].mode, cases[i].width, cases[i].offset);
        prim_expect_refused(renderer, cases[i].name, p, &failures);
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/* Other subpixel precision, tie rules or arithmetic refuse before snapshot. */
static void test_raster_rules_refused(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned i = 0; i < 4; i++) {
        static const char *const names[] = {
            "subpixel bits 4", "top-left tie rule", "fixed-point arithmetic",
            "round half to even",
        };

        prim_case_init(p, SGI_GL_TRIANGLES, 4);
        p->rules_case = i + 1;
        prim_vertex_n(p, 1, 1);
        prim_vertex_n(p, 14, 2);
        prim_vertex_n(p, 3, 13);
        prim_expect_refused(renderer, names[i], p, &failures);
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

static void test_scalar_match_polygons(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float strip[][2] = {
        { 1, 1 }, { 2.5f, 7.25f }, { 5.75f, 1.5f }, { 7, 8 },
        { 10.25f, 2 }, { 12.5f, 7.5f }, { 14.75f, 3.25f },
    };
    static const float quads[][2] = {
        { 1, 9 }, { 6.5f, 8.25f }, { 7.25f, 14.5f }, { 1.75f, 15 },
        { 9, 9.5f }, { 14.5f, 9 }, { 15, 13.75f }, { 8.5f, 14.25f },
    };
    static const float polygon[][2] = {
        { 8, 1.25f }, { 13.5f, 3 }, { 14.75f, 8.5f }, { 9.5f, 13.25f },
        { 3.25f, 10 }, { 2.5f, 4.75f },
    };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned shade = 0; shade < 2; shade++) {
        uint32_t model = shade ? SGI_GL_SMOOTH : SGI_GL_FLAT;
        const char *kind = shade ? "smooth" : "flat";
        g_autofree char *name = NULL;

        prim_case_init(p, SGI_GL_TRIANGLE_STRIP, 4);
        p->state.shade_model = model;
        for (unsigned i = 0; i < G_N_ELEMENTS(strip); i++) {
            prim_vertex_n(p, strip[i][0], strip[i][1]);
        }
        name = g_strdup_printf("%s triangle strip", kind);
        prim_expect(renderer, name, p, &failures);
        g_clear_pointer(&name, g_free);

        prim_case_init(p, SGI_GL_TRIANGLE_FAN, 4);
        p->state.shade_model = model;
        for (unsigned i = 0; i < G_N_ELEMENTS(polygon); i++) {
            prim_vertex_n(p, polygon[i][0], polygon[i][1]);
        }
        name = g_strdup_printf("%s triangle fan", kind);
        prim_expect(renderer, name, p, &failures);
        g_clear_pointer(&name, g_free);

        prim_case_init(p, SGI_GL_QUADS, 4);
        p->state.shade_model = model;
        for (unsigned i = 0; i < G_N_ELEMENTS(quads); i++) {
            prim_vertex_n(p, quads[i][0], quads[i][1]);
        }
        name = g_strdup_printf("%s quads", kind);
        prim_expect(renderer, name, p, &failures);
        g_clear_pointer(&name, g_free);

        prim_case_init(p, SGI_GL_QUAD_STRIP, 4);
        p->state.shade_model = model;
        for (unsigned i = 0; i < G_N_ELEMENTS(strip); i++) {
            prim_vertex_n(p, strip[i][0], strip[i][1] + 6 * (i & 1));
        }
        name = g_strdup_printf("%s quad strip", kind);
        prim_expect(renderer, name, p, &failures);
        g_clear_pointer(&name, g_free);

        prim_case_init(p, SGI_GL_POLYGON, 4);
        p->state.shade_model = model;
        for (unsigned i = 0; i < G_N_ELEMENTS(polygon); i++) {
            prim_vertex_n(p, polygon[i][0], polygon[i][1]);
        }
        name = g_strdup_printf("%s polygon", kind);
        prim_expect(renderer, name, p, &failures);
        g_clear_pointer(&name, g_free);
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * Smoothing stays unsupported and refuses before the snapshot, but only
 * for the class it applies to (OpenGL 1.2.1, 3.3 to 3.5): a triangle with
 * line smoothing on draws.
 */
/* Polygon modes do not apply to points, so a LINE/POINT pair draws them. */
static void test_points_ignore_polygon_modes(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const uint32_t modes[2] = { SGI_GL_LINE, SGI_GL_POINT };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    prim_case_init(p, SGI_GL_POINTS, 4);
    p->draw.point_size = 2;
    p->draw.polygon_modes = modes;
    prim_vertex_n(p, 3.5f, 4.5f);
    prim_vertex_n(p, 11, 9.25f);
    prim_expect(renderer, "points with polygon modes", p, &failures);
    prim_case_init(p, SGI_GL_TRIANGLES, 4);
    p->draw.polygon_modes = modes;
    prim_vertex_n(p, 1, 1);
    prim_vertex_n(p, 14, 2);
    prim_vertex_n(p, 3, 13);
    prim_expect_refused(renderer, "triangle with polygon modes", p,
                        &failures);
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

static void test_smoothing_refused(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;
    static const struct {
        uint32_t mode;
        bool point, line, polygon, refused;
    } smoothing[] = {
        { SGI_GL_POINTS, true, false, false, true },
        { SGI_GL_LINES, false, true, false, true },
        { SGI_GL_LINE_STRIP, false, true, false, true },
        { SGI_GL_LINE_LOOP, false, true, false, true },
        { SGI_GL_TRIANGLES, false, false, true, true },
        { SGI_GL_QUADS, false, false, true, true },
        { SGI_GL_POLYGON, false, false, true, true },
        { SGI_GL_TRIANGLES, true, true, false, false },
        { SGI_GL_QUADS, true, true, false, false },
        /* Lines are refused for their own rasterization. */
        { SGI_GL_LINES, true, false, true, true },
        { SGI_GL_POINTS, false, true, true, false },
    };

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned i = 0; i < G_N_ELEMENTS(smoothing); i++) {
        g_autofree char *name = g_strdup_printf(
            "mode %u smooth point %d line %d polygon %d", smoothing[i].mode,
            smoothing[i].point, smoothing[i].line, smoothing[i].polygon);
        PrimFixture f;
        SGIRenderResource resource = {
            .backing_id = 1, .backing_generation = 1,
            .view_id = 2, .view_generation = 1,
            .role = SGI_RENDER_COLOUR, .target = 7,
            .buffer = SGI_RENDER_FRONT, .eye = SGI_RENDER_MONO,
            .width = PRIM_SIZE, .height = PRIM_SIZE, .depth = 1,
            .bottom_origin = true, .component_count = 4,
            .component = {
                { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_A, SGI_RENDER_UNORM, 8 },
            },
        };
        SGIRenderResourceOps ops = {
            .snapshot = prim_snapshot,
            .commit_covered = prim_commit_covered,
        };
        SGIRasterTarget target = {
            .opaque = &f, .resources = &resource, .resource_count = 1,
            .resource_ops = &ops,
        };
        SGIRasterRules rules = {
            .subpixel_bits = 8, .color_bits = 8, .alpha_bits = 8,
            .rounding = SGI_ROUND_HALF_AWAY,
            .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
            .arith = SGI_ARITH_DOUBLE, .evidence = "synthetic",
        };
        SGIRenderResult result, expected;

        prim_case_init(p, smoothing[i].mode, 4);
        prim_destination(&f, p);
        for (unsigned v = 0; v < 4; v++) {
            prim_vertex_n(p, v & 1 ? 12 : 3, v & 2 ? 12 : 3.5f);
        }
        p->draw.point_smooth = smoothing[i].point;
        p->draw.line_smooth = smoothing[i].line;
        p->draw.polygon_smooth = smoothing[i].polygon;
        result = sgi_render_draw_checked(renderer, &rules, &target, &p->draw,
                                         &err);
        expected = smoothing[i].refused ? SGI_RENDER_UNSUPPORTED :
                                          SGI_RENDER_OK;
        if (result != expected ||
            !!err != smoothing[i].refused ||
            f.snapshots != !smoothing[i].refused) {
            g_test_message("%s: result %d, %u snapshots", name, result,
                           f.snapshots);
            failures++;
        }
        g_clear_pointer(&err, error_free);
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * The constant-colour factors stay unsupported: llvmpipe's float32 blend
 * and the scalar double-precision blend round exact half steps in
 * opposite directions, for example blend colour 0.9 on a stored 195.
 */
static bool prim_blend_factor_constant(uint32_t factor)
{
    return factor == SGI_GL_CONSTANT_COLOR ||
           factor == SGI_GL_ONE_MINUS_CONSTANT_COLOR ||
           factor == SGI_GL_CONSTANT_ALPHA ||
           factor == SGI_GL_ONE_MINUS_CONSTANT_ALPHA;
}

static const uint32_t prim_blend_factors[] = {
    0, 1, SGI_GL_SRC_COLOR, SGI_GL_ONE_MINUS_SRC_COLOR, SGI_GL_DST_COLOR,
    SGI_GL_ONE_MINUS_DST_COLOR, SGI_GL_SRC_ALPHA, SGI_GL_ONE_MINUS_SRC_ALPHA,
    SGI_GL_DST_ALPHA, SGI_GL_ONE_MINUS_DST_ALPHA, SGI_GL_CONSTANT_COLOR,
    SGI_GL_ONE_MINUS_CONSTANT_COLOR, SGI_GL_CONSTANT_ALPHA,
    SGI_GL_ONE_MINUS_CONSTANT_ALPHA, SGI_GL_SRC_ALPHA_SATURATE,
};

static const uint32_t prim_blend_equations[] = {
    SGI_GL_FUNC_ADD, SGI_GL_FUNC_SUBTRACT, SGI_GL_FUNC_REVERSE_SUBTRACT,
    SGI_GL_MIN, SGI_GL_MAX,
};

/*
 * Every factor pair and equation, flat and smooth, RGBA8 and RGB8. A pair
 * with a constant-colour factor must be refused before the snapshot.
 */
/*
 * Pieces of one blended draw that overlap: each blends against the value
 * the previous one stored, clamped and rounded to the storage steps.
 */
static void prim_blend_overlap_case(PrimCase *p, unsigned components,
                                    uint32_t mode, uint32_t src, uint32_t dst,
                                    uint32_t equation)
{
    prim_case_init(p, mode, components);
    p->state.blend = 1;
    p->state.blend_src = src;
    p->state.blend_dst = dst;
    p->state.blend_equation = equation;
}

/*
 * A blended draw that splits into several primitives is refused; the same
 * vectors drawn as one primitive must still match.
 */
static void test_blend_overlap_refused(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned components = 3; components <= 4; components++) {
        g_autofree char *name = NULL;

        /* 64 blended with black at alpha 1/255 is 63.749 each time. */
        prim_blend_overlap_case(p, components, SGI_GL_TRIANGLES,
                                SGI_GL_SRC_ALPHA, SGI_GL_ONE_MINUS_SRC_ALPHA,
                                SGI_GL_FUNC_ADD);
        p->dest_value = 64 + 1;
        for (unsigned i = 0; i < 2; i++) {
            prim_vertex(p, -1, -1, 0, 0, 0, 1 / 255.0f);
            prim_vertex(p, 3 * PRIM_SIZE, -1, 0, 0, 0, 1 / 255.0f);
            prim_vertex(p, -1, 3 * PRIM_SIZE, 0, 0, 0, 1 / 255.0f);
        }
        name = g_strdup_printf("coincident triangles %u", components);
        prim_expect_refused(renderer, name, p, &failures);
        g_clear_pointer(&name, g_free);
        p->draw.count = 3;
        name = g_strdup_printf("one of the triangles %u", components);
        prim_expect(renderer, name, p, &failures);
        g_clear_pointer(&name, g_free);

        prim_blend_overlap_case(p, components, SGI_GL_POINTS,
                                SGI_GL_SRC_ALPHA, SGI_GL_ONE_MINUS_SRC_ALPHA,
                                SGI_GL_FUNC_ADD);
        p->draw.point_size = 3;
        for (unsigned i = 0; i < 4; i++) {
            prim_vertex(p, 5.5f + (i & 1), 7.5f, 0.1f, 0.9f, 0.5f, 0.3f);
        }
        name = g_strdup_printf("repeated points %u", components);
        prim_expect_refused(renderer, name, p, &failures);
        g_clear_pointer(&name, g_free);

        /* A concave polygon's fan triangles cover some pixels twice. */
        prim_blend_overlap_case(p, components, SGI_GL_POLYGON,
                                SGI_GL_SRC_ALPHA, SGI_GL_ONE_MINUS_SRC_ALPHA,
                                SGI_GL_FUNC_ADD);
        p->state.shade_model = SGI_GL_SMOOTH;
        prim_vertex_n(p, 1, 1);
        prim_vertex_n(p, 15, 2);
        prim_vertex_n(p, 4.5f, 6.25f);
        prim_vertex_n(p, 14, 14.5f);
        prim_vertex_n(p, 2, 13);
        name = g_strdup_printf("concave polygon %u", components);
        prim_expect_refused(renderer, name, p, &failures);
        g_clear_pointer(&name, g_free);

        prim_blend_overlap_case(p, components, SGI_GL_QUADS,
                                SGI_GL_SRC_ALPHA, SGI_GL_ONE_MINUS_SRC_ALPHA,
                                SGI_GL_FUNC_ADD);
        prim_vertex(p, 1, 1, 0.8f, 0.2f, 0.1f, 0.6f);
        prim_vertex(p, 11, 1.5f, 0.8f, 0.2f, 0.1f, 0.6f);
        prim_vertex(p, 10.5f, 11, 0.8f, 0.2f, 0.1f, 0.6f);
        prim_vertex(p, 1.25f, 10, 0.8f, 0.2f, 0.1f, 0.6f);
        prim_vertex(p, 5, 4.5f, 0.1f, 0.3f, 0.9f, 0.45f);
        prim_vertex(p, 15, 5, 0.1f, 0.3f, 0.9f, 0.45f);
        prim_vertex(p, 14.5f, 15, 0.1f, 0.3f, 0.9f, 0.45f);
        prim_vertex(p, 5.5f, 14, 0.1f, 0.3f, 0.9f, 0.45f);
        name = g_strdup_printf("overlapping quads %u", components);
        prim_expect_refused(renderer, name, p, &failures);
        g_clear_pointer(&name, g_free);

        /* The first result is below zero and stored as zero. */
        for (unsigned e = 0; e < 2; e++) {
            prim_blend_overlap_case(p, components, SGI_GL_TRIANGLES, 1, 1,
                                    e ? SGI_GL_FUNC_REVERSE_SUBTRACT :
                                        SGI_GL_FUNC_SUBTRACT);
            for (unsigned i = 0; i < 2; i++) {
                float c = e ? 0.8f : 0.2f;

                prim_vertex(p, -1, -1, c, c, c, c);
                prim_vertex(p, 3 * PRIM_SIZE, -1, c, c, c, c);
                prim_vertex(p, -1, 3 * PRIM_SIZE, c, c, c, c);
            }
            name = g_strdup_printf("%s out of range %u",
                                   e ? "reverse subtract" : "subtract",
                                   components);
            prim_expect_refused(renderer, name, p, &failures);
            g_clear_pointer(&name, g_free);
            p->draw.count = 3;
            name = g_strdup_printf("%s once %u",
                                   e ? "reverse subtract" : "subtract",
                                   components);
            prim_expect(renderer, name, p, &failures);
            g_clear_pointer(&name, g_free);
        }
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * 10-bit storage crosses the worker as 16-bit UNORM, which cannot hold
 * every 10-bit value exactly: 504 blended with black at alpha 1/1023 is
 * 503.507, stored as 504 by the scalar path and 503 through the worker.
 */
static void test_blend_10_bit_refused(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned components = 3; components <= 4; components++) {
        g_autofree char *name = g_strdup_printf("10-bit blend %u",
                                                components);

        prim_case_init(p, SGI_GL_TRIANGLES, components);
        p->bits = 10;
        p->dest_value = 504 + 1;
        p->state.blend = 1;
        p->state.blend_src = SGI_GL_SRC_ALPHA;
        p->state.blend_dst = SGI_GL_ONE_MINUS_SRC_ALPHA;
        p->state.blend_equation = SGI_GL_FUNC_ADD;
        prim_vertex(p, -1, -1, 0, 0, 0, 1 / 1023.0f);
        prim_vertex(p, 3 * PRIM_SIZE, -1, 0, 0, 0, 1 / 1023.0f);
        prim_vertex(p, -1, 3 * PRIM_SIZE, 0, 0, 0, 1 / 1023.0f);
        prim_expect_refused(renderer, name, p, &failures);
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

static void test_scalar_match_blend(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0, cases = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned components = 3; components <= 4; components++) {
        for (unsigned e = 0; e < G_N_ELEMENTS(prim_blend_equations); e++) {
            for (unsigned s = 0; s < G_N_ELEMENTS(prim_blend_factors); s++) {
                /* Source alpha saturate is a source factor only. */
                for (unsigned d = 0;
                     d + 1 < G_N_ELEMENTS(prim_blend_factors); d++) {
                    g_autofree char *name = NULL;

                    prim_case_init(p, SGI_GL_TRIANGLES, components);
                    p->state.shade_model = (s + d) & 1 ? SGI_GL_SMOOTH :
                                                         SGI_GL_FLAT;
                    p->state.blend = 1;
                    p->state.blend_src = prim_blend_factors[s];
                    p->state.blend_dst = prim_blend_factors[d];
                    p->state.blend_equation = prim_blend_equations[e];
                    p->state.blend_color[0] = 0.2f;
                    p->state.blend_color[1] = 0.6f;
                    p->state.blend_color[2] = 0.9f;
                    p->state.blend_color[3] = 0.35f;
                    prim_vertex(p, -1, -1, 0.31f, 0.77f, 0.05f, 0.43f);
                    prim_vertex(p, 3 * PRIM_SIZE, -1, 0.9f, 0.12f, 0.55f,
                                0.8f);
                    prim_vertex(p, -1, 3 * PRIM_SIZE, 0.6f, 0.4f, 0.98f,
                                0.17f);
                    name = g_strdup_printf("blend rgb%s %s src 0x%x dst "
                                           "0x%x eq 0x%x",
                                           components == 4 ? "a" : "",
                                           p->state.shade_model ==
                                           SGI_GL_FLAT ? "flat" : "smooth",
                                           p->state.blend_src,
                                           p->state.blend_dst,
                                           p->state.blend_equation);
                    if (prim_blend_factor_constant(p->state.blend_src) ||
                        prim_blend_factor_constant(p->state.blend_dst)) {
                        prim_expect_refused(renderer, name, p, &failures);
                    } else {
                        prim_expect(renderer, name, p, &failures);
                    }
                    cases++;
                }
            }
        }
    }
    g_test_message("blend: %u of %u cases differ", failures, cases);
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
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
    g_test_add_func("/sgi-renderer-gl/decoded-triangle-viewport-fill",
                    test_decoded_triangle_viewport_fill);
    g_test_add_func("/sgi-renderer-gl/decoded-triangle-viewport-limit",
                    test_decoded_triangle_viewport_limit);
    g_test_add_func("/sgi-renderer-gl/decoded-triangle-clip-union",
                    test_decoded_triangle_clip_union);
    g_test_add_func("/sgi-renderer-gl/triangle-resource-row-origin",
                    test_triangle_resource_row_origin);
    g_test_add_func("/sgi-renderer-gl/flat-triangle-alpha-blend",
                    test_flat_triangle_alpha_blend);
    g_test_add_func("/sgi-renderer-gl/blend-without-native-alpha",
                    test_blend_without_native_alpha);
    g_test_add_func("/sgi-renderer-gl/frame-coverage-capacity",
                    test_frame_coverage_capacity);
    g_test_add_func("/sgi-renderer-gl/frame-texture-validation",
                    test_frame_texture_validation);
    g_test_add_func("/sgi-renderer-gl/typed-texture-triangle",
                    test_typed_texture_triangle);
    g_test_add_func("/sgi-renderer-gl/typed-texture-multi-texel",
                    test_typed_texture_multi_texel);
    g_test_add_func("/sgi-renderer-gl/scalar-match-state",
                    test_scalar_match_state);
    g_test_add_func("/sgi-renderer-gl/scalar-match-colour-ties",
                    test_scalar_match_colour_ties);
    g_test_add_func("/sgi-renderer-gl/scalar-match-shared-edges",
                    test_scalar_match_shared_edges);
    g_test_add_func("/sgi-renderer-gl/scalar-match-widths",
                    test_scalar_match_widths);
    g_test_add_func("/sgi-renderer-gl/scalar-match-texture",
                    test_scalar_match_texture);
    g_test_add_func("/sgi-renderer-gl/scalar-match-clear",
                    test_scalar_match_clear);
    g_test_add_func("/sgi-renderer-gl/scalar-match-store-widths",
                    test_scalar_match_store_widths);
    g_test_add_func("/sgi-renderer-gl/scalar-match-points",
                    test_scalar_match_points);
    g_test_add_func("/sgi-renderer-gl/lines-refused", test_lines_refused);
    g_test_add_func("/sgi-renderer-gl/raster-rules-refused",
                    test_raster_rules_refused);
    g_test_add_func("/sgi-renderer-gl/scalar-match-polygons",
                    test_scalar_match_polygons);
    g_test_add_func("/sgi-renderer-gl/points-ignore-polygon-modes",
                    test_points_ignore_polygon_modes);
    g_test_add_func("/sgi-renderer-gl/scalar-match-blend",
                    test_scalar_match_blend);
    g_test_add_func("/sgi-renderer-gl/blend-overlap-refused",
                    test_blend_overlap_refused);
    g_test_add_func("/sgi-renderer-gl/blend-10-bit-refused",
                    test_blend_10_bit_refused);
    g_test_add_func("/sgi-renderer-gl/smoothing-refused",
                    test_smoothing_refused);
    g_test_add_func("/sgi-renderer-gl/decoded-clear-native-commit",
                    test_decoded_clear_native_commit);
    g_test_add_func("/sgi-renderer-gl/decoded-clear-full-screen",
                    test_decoded_clear_full_screen_commit);
    g_test_add_func("/sgi-renderer-gl/decoded-clear-depth-stencil",
                    test_decoded_clear_depth_stencil);
#ifdef CONFIG_GBM
    if (g_getenv("QEMU_TEST_SGI_RENDER_NODE")) {
        g_test_add_func("/sgi-renderer-gl/shader-output", test_shader_output);
    }
#endif
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
