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
    SGIRasterRules rules = { .color_bits = 8, .evidence = "synthetic" };
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
    polygon_modes[1] = SGI_GL_LINE;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
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
    SGIRasterRules rules = { .color_bits = 8, .evidence = "synthetic" };
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
    SGIRasterRules rules = { .color_bits = 8, .evidence = "synthetic" };
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
    SGIRasterRules rules = { .color_bits = 8, .evidence = "synthetic" };
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
    SGIRasterRules rules = { .color_bits = 8, .evidence = "synthetic" };
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
    state.blend_equation = SGI_GL_FUNC_REVERSE_SUBTRACT;
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
    SGIRasterRules rules = { .color_bits = 8, .evidence = "synthetic" };
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

        state.blend_src = SGI_GL_DST_ALPHA;
        state.blend_dst = SGI_GL_ONE_MINUS_SRC_ALPHA;
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
    SGIRasterRules rules = { .color_bits = 8, .evidence = "synthetic" };
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
    SGIRasterRules rules = { .color_bits = 8, .evidence = "synthetic" };
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
    SGIRasterRules rules = { .color_bits = 12, .evidence = "synthetic" };
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
