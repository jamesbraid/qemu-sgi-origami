/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include <math.h>
#include "qemu/module.h"
#include "ui/egl-helpers.h"
#include "ui/sgi-renderer.h"
#include "hw/display/sgi/rasterizer.h"
#include "hw/display/sgi/render-shadow.h"
#include "../qtest/prereq.h"

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

static const uint8_t texture_pixels[16] = {
    0, 0, 0, 127, 255, 0, 128, 127,
    0, 255, 128, 127, 255, 255, 255, 127,
};

static double texture_axis(double c, SGIRendererTextureWrap wrap)
{
    if (wrap == SGI_RENDERER_TEXTURE_REPEAT) {
        return c - floor(c);
    }
    if (wrap == SGI_RENDERER_TEXTURE_EDGE) {
        return CLAMP(c, 0.25, 0.75);
    }
    if (wrap == SGI_RENDERER_TEXTURE_BORDER) {
        return CLAMP(c, -0.25, 1.25);
    }
    return CLAMP(c, 0, 1);
}

static bool texture_apply(void *opaque, const double coord[4], double colour[4])
{
    const SGIRendererTexture *t = opaque;
    double u = texture_axis(coord[0] / coord[3], t->wrap[0]);
    double v = texture_axis(coord[1] / coord[3], t->wrap[1]);
    double sx = u * 2 - (t->linear ? 0.5 : 0);
    double sy = v * 2 - (t->linear ? 0.5 : 0);
    int ix = floor(sx), iy = floor(sy);
    double ax = sx - ix, ay = sy - iy, texel[4] = { 0 };

    if (!t->linear) {
        if (u == 1 && t->wrap[0] != SGI_RENDERER_TEXTURE_BORDER) {
            ix = 1;
        }
        if (v == 1 && t->wrap[1] != SGI_RENDERER_TEXTURE_BORDER) {
            iy = 1;
        }
    }
    for (unsigned y = 0; y < (t->linear ? 2 : 1); y++) {
        for (unsigned x = 0; x < (t->linear ? 2 : 1); x++) {
            int px = ix + x, py = iy + y;
            double weight = t->linear ?
                            (x ? ax : 1 - ax) * (y ? ay : 1 - ay) : 1;

            if (t->wrap[0] == SGI_RENDERER_TEXTURE_REPEAT) {
                px = (px + 2) % 2;
            }
            if (t->wrap[1] == SGI_RENDERER_TEXTURE_REPEAT) {
                py = (py + 2) % 2;
            }
            for (unsigned c = 0; c < 4; c++) {
                double value = px < 0 || px > 1 || py < 0 || py > 1 ?
                               t->border[c] :
                               t->texels[(py * 2 + px) * 4 + c] / 255.0;

                texel[c] += weight * value;
            }
        }
    }
    for (unsigned c = 0; c < 4; c++) {
        if (t->environment == SGI_RENDERER_TEXTURE_REPLACE) {
            colour[c] = texel[c];
        } else if (t->environment == SGI_RENDERER_TEXTURE_MODULATE) {
            colour[c] *= texel[c];
        } else if (t->environment == SGI_RENDERER_TEXTURE_DECAL && c < 3) {
            colour[c] = colour[c] * (1 - texel[3]) + texel[c] * texel[3];
        } else if (t->environment == SGI_RENDERER_TEXTURE_BLEND) {
            colour[c] = c == 3 ? colour[c] * texel[c] :
                        colour[c] * (1 - texel[c]) +
                        t->env_colour[c] * texel[c];
        }
    }
    return true;
}

static void test_shader_texture(gconstpointer opaque)
{
    unsigned input = GPOINTER_TO_UINT(opaque);
    bool precision = input == 14;
    unsigned mode = precision ? 4 : input >= 8 ? input - 8 : input;
    const char *node = g_getenv("QEMU_TEST_SGI_RENDER_NODE");
    QemuEGLDisplayLease *provider = NULL;
    SGIRendererTexture texture = {
        .serial = 1, .width = 2, .height = 2,
        .format = SGI_RENDERER_TEXTURE_RGBA,
        .linear = mode & 1, .environment = mode / 2,
        .wrap = { mode / 2, mode / 2 },
        .env_colour = { 0.25f, 0.5f, 0.75f, 0.25f },
        .border = { 0.25f, 0.5f, 0.75f, 0.5f },
        .scale = { 1, 1, 1, 1 },
        .texels = texture_pixels, .bytes = sizeof(texture_pixels),
        .apply = texture_apply,
    };
    g_autofree SGIRendererRequest *request = g_new0(SGIRendererRequest, 1);
    SGIRenderer *renderer;
    Error *err = NULL;
    uint8_t boundary_texels[sizeof(texture_pixels)];

    if (!node) {
        qtest_prereq_missing("SGI_RENDER_NODE",
                             "QEMU_TEST_SGI_RENDER_NODE is not set");
        return;
    }
    g_assert_true(egl_init_session(node, DISPLAY_GL_MODE_CORE,
                                  &provider, &err));
    g_assert_null(err);
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);

    memcpy(boundary_texels, texture_pixels, sizeof(boundary_texels));
    if (precision) {
        for (unsigned i = 3; i < sizeof(boundary_texels); i += 4) {
            boundary_texels[i] = 128;
        }
        texture.texels = boundary_texels;
    }
    texture.opaque = &texture;
    request->texture = &texture;
    request->clip[2] = request->clip[3] = 127;
    /* An odd subpixel denominator avoids exact byte-rounding ties. */
    request->vertices[1][0] = request->vertices[2][1] = 127.25;
    for (unsigned i = 0; i < 3; i++) {
        double q = 1 << i;

        request->vertices[i][3] = 1;
        request->inverse_w[i] = 1 / q;
        request->texcoord[i][0] = (i == 1 ? 1.25 : -0.25) * q;
        request->texcoord[i][1] = (i == 2 ? 1.25 : -0.25) * q;
        request->texcoord[i][3] = q;
        if (input >= 8 && !precision) {
            request->clip_planes = 1u << mode;
            request->clip_distance[i][mode] = (i == 1 ? 64 : -64) * q;
        }
        memcpy(request->colour[i], (double[]) { 0.5, 0.25, 0.75, 0.5 },
               sizeof(request->colour[i]));
    }
    if (!sgi_renderer_draw(renderer, request, &err)) {
        if (!precision) {
            g_error("Renderer draw failed: %s",
                    err ? error_get_pretty(err) : "no error detail");
        }
        g_assert_nonnull(err);
        g_assert_cmpstr(error_get_pretty(err), ==,
                        "Invalid SGI OpenGL renderer staging");
        g_assert_false(sgi_renderer_staging_valid(request));
        error_free(err);
    } else {
        g_assert_null(err);
        g_assert_true(sgi_renderer_staging_valid(request));
    }
    sgi_renderer_free(renderer);
    egl_cleanup_session(provider);
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
        qtest_prereq_missing("SGI_RENDER_NODE",
                             "QEMU_TEST_SGI_RENDER_NODE is not set");
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
        if (!sgi_renderer_draw(renderer, request, &err)) {
            g_error("Renderer draw failed: %s",
                    err ? error_get_pretty(err) : "no error detail");
        }
        g_assert_null(err);
        g_assert_true(sgi_renderer_staging_valid(request));
        g_assert_cmpuint(request->generation, ==, rgb + 1);
    }
    request->clip[0] = 50;
    request->clip[1] = 25;
    request->clip[2] = 90;
    request->clip[3] = 80;
    if (!sgi_renderer_draw(renderer, request, &err)) {
        g_error("Renderer draw failed: %s",
                err ? error_get_pretty(err) : "no error detail");
    }
    g_assert_null(err);
    g_assert_true(sgi_renderer_staging_valid(request));
    memset(request, 0, sizeof(*request));
    request->smooth = true;
    /* An odd subpixel denominator avoids exact byte-rounding ties. */
    request->vertices[1][0] = request->vertices[2][1] = 127.25;
    request->clip[2] = request->clip[3] = 127;
    for (unsigned i = 0; i < 3; i++) {
        request->vertices[i][3] = 1;
        request->colour[i][i] = request->colour[i][3] = 1;
    }
    for (unsigned mode = 0; mode < 2; mode++) {
        for (unsigned i = 0; i < 3; i++) {
            request->inverse_w[i] = mode ? 1 << i : 1;
        }
        if (!sgi_renderer_draw(renderer, request, &err)) {
            g_error("Renderer draw failed: %s",
                    err ? error_get_pretty(err) : "no error detail");
        }
        g_assert_null(err);
        g_assert_true(sgi_renderer_staging_valid(request));
    }
    egl_cleanup_session(provider);
    g_assert_cmpint(fcntl(fd, F_GETFD), >=, 0);
    /* The worker must survive after provider display globals are cleared. */
    if (!sgi_renderer_draw(renderer, request, &err)) {
        g_error("Renderer draw failed: %s",
                err ? error_get_pretty(err) : "no error detail");
    }
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
            /*
             * A readback that silently returns zeros raises no GL error.
             * Staging validation cannot tell it from a precision
             * difference, so it refuses the draw; that is safe because
             * the caller's software draw is exact. Neither refusal
             * latches, and the next correct readback draws.
             */
            for (unsigned i = 0; i < 2; i++) {
                g_assert_cmpint(sgi_renderer_draw_checked(renderer, request,
                                                          &err), ==,
                                SGI_RENDER_DRAW_REFUSED);
                g_assert_cmpstr(error_get_pretty(err), ==,
                                "Invalid SGI OpenGL renderer staging");
                g_clear_pointer(&err, error_free);
            }
            epoxy_glReadPixels = saved_read_pixels;
            g_assert_cmpint(sgi_renderer_draw_checked(renderer, request,
                                                      &err), ==,
                            SGI_RENDER_DRAW_OK);
            g_assert_null(err);
            g_assert_true(sgi_renderer_staging_valid(request));
        } else {
            if (!sgi_renderer_draw(renderer, request, &err)) {
                g_error("Renderer draw failed: %s",
                        err ? error_get_pretty(err) : "no error detail");
            }
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

static PFNGLGETPROGRAMIVPROC saved_program_status;
static unsigned program_links, failing_program_link;

/* Fail the link status of the nth program the worker builds, from one. */
static void fail_nth_program_link(GLuint program, GLenum name, GLint *value)
{
    saved_program_status(program, name, value);
    if (name == GL_LINK_STATUS && ++program_links == failing_program_link) {
        *value = GL_FALSE;
    }
}

/*
 * A program that fails to build when the host has the OpenGL it needs is a
 * host setup failure, reported by renderer creation, not a refusal of each
 * later draw that would use it. The worker builds, in order, its request,
 * colour, index, texture, smooth triangle and line programs.
 */
static void test_program_failure_fails_creation(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const char *const programs[] = {
        "index", "texture", "window", "line",
    };

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(true)) {
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(programs); i++) {
        SGIRenderer *renderer = NULL;
        SGIRenderCreateResult result;
        Error *err = NULL;

        saved_program_status = epoxy_glGetProgramiv;
        program_links = 0;
        failing_program_link = 3 + i;
        epoxy_glGetProgramiv = fail_nth_program_link;
        result = sgi_renderer_create(&renderer, &err);
        epoxy_glGetProgramiv = saved_program_status;
        g_assert_cmpuint(program_links, >=, failing_program_link);
        g_assert_cmpint(result, ==, SGI_RENDER_CREATE_HOST_ERROR);
        g_assert_null(renderer);
        g_assert_nonnull(err);
        g_assert_nonnull(strstr(error_get_pretty(err), programs[i]));
        error_free(err);
    }
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
    /* Polygon smoothing now draws aliased, as on the scalar path. */
    draw.polygon_stipple_enabled = true;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 4);
    g_assert_cmpuint(f.commits, ==, 3);

    draw.polygon_stipple_enabled = false;
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
    /* The fault must reach this draw, not the smooth triangle probe. */
    g_assert_cmpint(sgi_render_test_window_probe(renderer, &err), ==,
                    SGI_RENDER_OK);
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
    /* The fault must reach this draw, not the smooth triangle probe. */
    g_assert_cmpint(sgi_render_test_window_probe(renderer, &err), ==,
                    SGI_RENDER_OK);
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
    /* The fault must reach this draw, not the smooth triangle probe. */
    g_assert_cmpint(sgi_render_test_window_probe(renderer, &err), ==,
                    SGI_RENDER_OK);
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

static bool unexpected_texture_components(void *opaque, uint32_t x,
                                          uint32_t y, uint16_t component[4])
{
    g_assert_not_reached();
}

static bool unexpected_border_components(void *opaque, int32_t x, int32_t y,
                                         uint16_t component[4])
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
    /* One-dimensional texturing has no host path; the scalar path owns it. */
    state.texture_1d = 1;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
    g_assert_cmpuint(f.commits, ==, ARRAY_SIZE(cases));
    state.texture_1d = 0;
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
    /* The scalar path drops a 1D image under the 2D enable. */
    image.one_dimensional = true;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
    image.one_dimensional = false;
    /* The host program has no depth comparison. */
    image.shadow_compare = SGI_GL_TEXTURE_LEQUAL_R_SGIX;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
    image.shadow_compare = 0;
    /*
     * Raw components reach the scalar sampler at up to 16 bits, while the
     * host samples the 8-bit texture resource; no comparison covers them.
     */
    image.component_bits[0] = image.component_bits[1] = 8;
    image.component_bits[2] = image.component_bits[3] = 8;
    image.read_texel_components = unexpected_texture_components;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
    image.read_texel_components = NULL;
    image.read_border_texel_components = unexpected_border_components;
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                             &draw, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
    image.read_border_texel_components = NULL;
    memset(image.component_bits, 0, sizeof(image.component_bits));
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
    SGIRendererTriangle triangle = {
        .x = { -256, 2048, -256 }, .y = { -256, -256, 2048 },
        .z = { 0.5, 0.5, 0.5 }, .inv_w = { 1, 1, 1 },
    };
    uint16_t rgba[4 * 4 * 4] = { 0 };
    uint8_t written[4 * 4] = { 0 };
    uint32_t texels[4] = { 128, 64, 192, 128 };
    SGIRendererFrame frame = {
        .width = 4, .height = 4, .count = 1,
        .primitive = SGI_RENDERER_WINDOW_TRIANGLES, .triangles = &triangle,
        .shade_model = SGI_GL_FLAT,
        .colour_steps = { 255, 255, 255, 255 },
        .rgba = rgba, .written = written,
        .written_capacity = sizeof(written),
        .texture = {
            .width = 1, .height = 1, .component_bits = 8,
            .env_mode = SGI_GL_REPLACE,
            .wrap_s = SGI_GL_REPEAT, .wrap_t = SGI_GL_REPEAT,
            .slots = texels, .capacity_slots = ARRAY_SIZE(texels),
        },
    };
    static const float colour[4] = { 0.25f, 0.5f, 0.75f, 0.5f };
    static const double environment[4] = { 0.75, 0.25, 0.5, 1 };
    SGIRenderer *renderer;
    Error *err = NULL;

    for (unsigned v = 0; v < 3; v++) {
        memcpy(triangle.colour[v], colour, sizeof(colour));
        triangle.texcoord[v][0] = triangle.texcoord[v][1] = 0.5;
        triangle.texcoord[v][3] = 1;
    }
    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(true)) {
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);

#define FRAME_REFUSED(message) do { \
        g_assert_false(sgi_renderer_draw_frame(renderer, &frame, &err)); \
        g_assert_nonnull(err); \
        g_assert_cmpstr(error_get_pretty(err), ==, message); \
        g_clear_pointer(&err, error_free); \
    } while (0)
    frame.texture.border[0] = NAN;
    FRAME_REFUSED("Invalid SGI OpenGL texture border colour");
    frame.texture.border[0] = 0;
    frame.texture.border[1] = 2;
    FRAME_REFUSED("Invalid SGI OpenGL texture border colour");
    frame.texture.border[1] = 0;
    frame.texture.wrap_s = 0xffffffffu;
    FRAME_REFUSED("Invalid SGI OpenGL texture wrap");
    frame.texture.wrap_s = SGI_GL_REPEAT;
    frame.texture.env_mode = 0xffffffffu;
    FRAME_REFUSED("Invalid SGI OpenGL texture environment");
    frame.texture.env_mode = SGI_GL_BLEND;
    frame.texture.environment[0] = NAN;
    FRAME_REFUSED("Invalid SGI OpenGL texture environment colour");
    frame.texture.environment[0] = 1.5;
    FRAME_REFUSED("Invalid SGI OpenGL texture environment colour");
    memcpy(frame.texture.environment, environment, sizeof(environment));
    triangle.texcoord[1][3] = 0;
    FRAME_REFUSED("Invalid SGI OpenGL texture coordinates");
    triangle.texcoord[1][3] = 1;
    /* A textured frame is window triangles only. */
    frame.primitive = SGI_RENDERER_TRIANGLES;
    FRAME_REFUSED("Invalid SGI OpenGL frame request");
    frame.primitive = SGI_RENDERER_WINDOW_TRIANGLES;
#undef FRAME_REFUSED

    g_assert_true(sgi_renderer_draw_frame(renderer, &frame, &err));
    g_assert_null(err);
    /* BLEND as the scalar path computes it, in double from the floats. */
    for (unsigned c = 0; c < 4; c++) {
        double t = texels[c] / 255.0;
        double value = c < 3 ? colour[c] * (1 - t) + environment[c] * t :
                               colour[c] * t;

        g_assert_cmpuint(written[0], ==, 1);
        g_assert_cmpuint(rgba[c], ==, (uint32_t)(value * 255 + 0.5) * 257);
    }
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
    uint32_t texels[16][16][4];
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
    bool smooth_rules; /* The rules smooth points and lines. */
    /* Nonzero overrides: rules colour bits, rules alpha bits plus one, */
    unsigned rules_colour, rules_alpha_plus_one;
    unsigned alpha_storage; /* and stored alpha bits. */
    /* Native rows run down; see PrimFixture. Clips stay in window rows. */
    bool top_origin;
    /* With state.texture_2d: a level-zero RGBA8 image, rows from T zero. */
    SGIGLTexImage image;
    uint32_t texels[16][16][4];
    /* With use_phase, each path borrows its own stipple phase. */
    bool use_phase;
    uint32_t phase[2];
    /*
     * With attempt set, the Mesa draw goes through sgi_render_draw_attempt()
     * and leaves its result and outcome here; ops replaces prim_ops.
     */
    const SGIRenderAttemptOptions *attempt;
    SGIRenderDrawResult attempt_result;
    SGIRenderAttemptOutcome outcome;
    const SGIRenderResourceOps *ops;
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
        .smooth_primitives = p->smooth_rules,
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
    p->draw.line_phase = p->use_phase ? &p->phase[0] : NULL;
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
        .resource_ops = p->ops ? p->ops : &prim_ops,
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
    draw.line_phase = p->use_phase ? &p->phase[1] : NULL;
    if (p->rules_case == 1) {
        rules.subpixel_bits = 4;
    } else if (p->rules_case == 2) {
        rules.tie_rule = SGI_TIE_TOP_LEFT;
    } else if (p->rules_case == 3) {
        rules.arith = SGI_ARITH_FIXED16;
    } else if (p->rules_case == 4) {
        rules.rounding = SGI_ROUND_HALF_EVEN;
    }
    if (p->attempt) {
        p->attempt_result = sgi_render_draw_attempt(renderer, &rules, &target,
                                                    &draw, p->attempt,
                                                    &p->outcome, errp);
        return p->outcome.status;
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
        0.05f, -0.07f, -1.3f, 1,
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
 * Random flat and smooth triangles crossing or touching the near and far
 * planes, clipped there by the scalar path, must match it pixel for pixel.
 */
static void test_scalar_match_depth_clipped(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    g_autoptr(GRand) rand = g_rand_new_with_seed(1279);
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
    for (unsigned i = 0; i < 600; i++) {
        g_autofree char *name = g_strdup_printf("depth-clipped %u", i);

        prim_case_init(p, SGI_GL_TRIANGLES, 3 + g_rand_int_range(rand, 0, 2));
        p->state.shade_model = g_rand_boolean(rand) ? SGI_GL_SMOOTH :
                                                      SGI_GL_FLAT;
        for (unsigned v = 0; v < 3; v++) {
            float w = g_rand_int_range(rand, 4, 17) / 8.0f;
            float *pos;
            int pick = g_rand_int_range(rand, 0, 4);

            prim_vertex_n(p, g_rand_int_range(rand, 0, 65) / 4.0f,
                          g_rand_int_range(rand, 0, 65) / 4.0f);
            pos = &p->vertices[v][SGI_GL_V_POSITION];
            /* On a plane, past one, or between them. */
            pos[2] = pick == 0 ? -1 : pick == 1 ? 1 :
                     g_rand_int_range(rand, -48, 49) / 32.0f;
            for (unsigned c = 0; c < 4; c++) {
                pos[c] *= w;
            }
        }
        if (prim_compare(renderer, name, p, false)) {
            failures++;
        }
        cases++;
    }
    g_test_message("depth clipped: %u of %u cases differ", failures, cases);
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * The mixed-frame set combines widths, modes and state that the other
 * cases prove one at a time; these cases compare the combinations: every
 * proven width, RGB and RGBA, every polygon mode, flat and smooth, under
 * clips, scissors, offset and oversized viewports, matrices and top-origin
 * rows.
 */
static void test_scalar_match_cross(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const unsigned widths[] = { 1, 2, 4, 5, 6, 8, 10 };
    static const uint32_t modes[] = {
        SGI_GL_TRIANGLES, SGI_GL_TRIANGLE_STRIP, SGI_GL_TRIANGLE_FAN,
        SGI_GL_QUADS, SGI_GL_QUAD_STRIP, SGI_GL_POLYGON,
    };
    static const float points[8][2] = {
        { 1.25f, 0.75f }, { 14.5f, 2.125f }, { 3.5f, 6.5f },
        { 12.75f, 7.25f }, { 2.375f, 11.25f }, { 13.5f, 12.5f },
        { 6.5f, 15.25f }, { 9.75f, 14.5f },
    };
    static const SGIClipRect disjoint[] = {
        { 1, 1, 5, 4 }, { 9, 2, 14, 6 }, { 3, 10, 6, 15 },
    };
    static const char *const states[] = {
        "clips", "scissor", "offset viewport", "oversized viewport",
        "orthographic", "perspective", "top-origin",
        "top-origin, scissor and perspective",
    };
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
            /* One-bit stored alpha is refused; see store widths. */
            if (widths[w] == 1 && components == 4) {
                continue;
            }
            for (unsigned m = 0; m < ARRAY_SIZE(modes); m++) {
                for (unsigned k = 0; k < 2 * ARRAY_SIZE(states); k++) {
                    unsigned state = k / 2;
                    bool smooth = k & 1;
                    g_autofree char *name = g_strdup_printf(
                        "%u-bit rgb%s mode %u %s %s", widths[w],
                        components == 4 ? "a" : "", modes[m],
                        smooth ? "smooth" : "flat", states[state]);

                    prim_case_init(p, modes[m], components);
                    p->bits = widths[w];
                    p->state.shade_model = smooth ? SGI_GL_SMOOTH :
                                                    SGI_GL_FLAT;
                    switch (state) {
                    case 0:
                        prim_set_clips(p, disjoint, ARRAY_SIZE(disjoint));
                        break;
                    case 1:
                    case 7:
                        p->state.scissor_test = 1;
                        memcpy(p->state.scissor, (int32_t[4]) { -4, 3, 13, 9 },
                               sizeof(p->state.scissor));
                        break;
                    case 2:
                        memcpy(p->state.viewport,
                               (int32_t[4]) { 3, -2, 10, 12 },
                               sizeof(p->state.viewport));
                        break;
                    case 3:
                        memcpy(p->state.viewport,
                               (int32_t[4]) { -8, -8, 40, 40 },
                               sizeof(p->state.viewport));
                        break;
                    }
                    p->top_origin = state >= 6;
                    for (unsigned v = 0; v < ARRAY_SIZE(points); v++) {
                        prim_vertex_window(p, points[v][0], points[v][1]);
                    }
                    if (state == 4 || state == 5 || state == 7) {
                        prim_matrices(p, state != 4);
                    }
                    if (prim_compare(renderer, name, p, false)) {
                        failures++;
                    }
                    cases++;
                }
            }
        }
    }
    /* Lines are proven at 8 bits; combine their widths with the state. */
    for (unsigned m = SGI_GL_LINES; m <= SGI_GL_LINE_STRIP; m++) {
        for (unsigned k = 0; k < 3 * 2 * ARRAY_SIZE(states); k++) {
            static const unsigned line_widths[] = { 1, 3, 6 };
            unsigned state = k / 6, width = line_widths[k / 2 % 3];
            bool smooth = k & 1;
            g_autofree char *name = g_strdup_printf(
                "line mode %u width %u %s %s", m, width,
                smooth ? "smooth" : "flat", states[state]);

            prim_case_init(p, m, 4);
            p->draw.line_width = width;
            p->state.shade_model = smooth ? SGI_GL_SMOOTH : SGI_GL_FLAT;
            switch (state) {
            case 0:
                prim_set_clips(p, disjoint, ARRAY_SIZE(disjoint));
                break;
            case 1:
            case 7:
                p->state.scissor_test = 1;
                memcpy(p->state.scissor, (int32_t[4]) { -4, 3, 13, 9 },
                       sizeof(p->state.scissor));
                break;
            case 2:
                memcpy(p->state.viewport, (int32_t[4]) { 3, -2, 10, 12 },
                       sizeof(p->state.viewport));
                break;
            case 3:
                memcpy(p->state.viewport, (int32_t[4]) { -8, -8, 40, 40 },
                       sizeof(p->state.viewport));
                break;
            }
            p->top_origin = state >= 6;
            for (unsigned v = 0; v < ARRAY_SIZE(points); v++) {
                prim_vertex_window(p, points[v][0], points[v][1]);
            }
            if (state == 4 || state == 5 || state == 7) {
                prim_matrices(p, state != 4);
            }
            if (prim_compare(renderer, name, p, false)) {
                failures++;
            }
            cases++;
        }
    }
    g_test_message("cross: %u of %u cases differ", failures, cases);
    sgi_renderer_free(renderer);
    g_free(p);
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

/* Resize a prim_texture() image to width by height, refilling its texels. */
static void prim_texture_size(PrimCase *p, unsigned width, unsigned height)
{
    p->image.width = width;
    p->image.height = height;
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width; x++) {
            uint32_t *t = p->texels[y][x];

            t[0] = (x * 83 + y * 29 + 11) & 255;
            t[1] = (x * 47 + y * 101 + 60) & 255;
            t[2] = (x * 13 + y * 157 + 200) & 255;
            t[3] = (x * 71 + y * 37 + 90) & 255;
        }
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

/*
 * Place a prim_texture() triangle at window positions xy with clip w and
 * texture coordinates st, q one, so that its edges and texel boundaries
 * fall where the scalar path's double arithmetic and float32
 * interpolation can disagree.
 */
static void prim_texture_place(PrimCase *p, const float xy[3][2],
                               const float st[3][2], const float w[3])
{
    for (unsigned v = 0; v < 3; v++) {
        float *vertex = p->vertices[v];

        vertex[SGI_GL_V_POSITION] = (xy[v][0] * 2 / PRIM_SIZE - 1) * w[v];
        vertex[SGI_GL_V_POSITION + 1] = (xy[v][1] * 2 / PRIM_SIZE - 1) * w[v];
        vertex[SGI_GL_V_POSITION + 2] = 0;
        vertex[SGI_GL_V_POSITION + 3] = w[v];
        vertex[SGI_GL_V_TEXCOORD] = st[v][0];
        vertex[SGI_GL_V_TEXCOORD + 1] = st[v][1];
        vertex[SGI_GL_V_TEXCOORD + 2] = 0;
        vertex[SGI_GL_V_TEXCOORD + 3] = 1;
    }
}

/*
 * The vector that showed textured triangles differ: a 2 by 2 NEAREST
 * texture where the scalar path's t at pixel (4,10) is 0.5 within
 * rounding, a texel boundary, so float32 interpolation picks the other
 * row.
 */
static void test_scalar_match_texture_boundary(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float xy[3][2] = {
        { 1.5f, 1.5f }, { 1.75f, 12.25f }, { 13.5f, 13.5f },
    };
    static const float st[3][2] = {
        { -0.6f, -0.35f }, { 1.7f, 0.2f }, { 0.45f, 1.9f },
    };
    static const float w[3] = { 1, 1, 1 };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(true)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    prim_case_init(p, SGI_GL_TRIANGLES, 4);
    prim_texture(p, 2, SGI_GL_NEAREST, SGI_GL_REPEAT, SGI_GL_REPEAT,
                 SGI_GL_MODULATE);
    prim_texture_place(p, xy, st, w);
    prim_expect(renderer, "texel boundary at (4,10)", p, &failures);
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * Textured triangles away from the fixture: diagonal and axis-aligned
 * edges through pixel centres, every vertex order, clip w of 0.5, 1 and 2,
 * flat and smooth, every environment, both filters and the accepted wraps,
 * then a seeded random sweep.
 */
static void test_scalar_match_texture_sweep(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float shapes[][3][2] = {
        { { 13.5f, 13.5f }, { 1.5f, 1.5f }, { 1.75f, 12.25f } },
        { { 2.5f, 1.5f }, { 2.5f, 14.5f }, { 13.25f, 7.75f } },
        { { 14.5f, 13.5f }, { 1.5f, 13.5f }, { 7.75f, 2.25f } },
        { { 0.5f, 0.5f }, { 15.5f, 3.5f }, { 4.5f, 15.5f } },
    };
    static const float coords[][3][2] = {
        { { -0.6f, -0.35f }, { 1.7f, 0.2f }, { 0.45f, 1.9f } },
        { { 0, 0 }, { 1, 0 }, { 0.5f, 1 } },
        { { 0.25f, 2.75f }, { -1.5f, 0.5f }, { 3, -0.125f } },
    };
    static const float weights[][3] = {
        { 1, 1, 1 }, { 1, 2, 0.5f }, { 2, 0.5f, 1 },
    };
    static const uint32_t envs[] = {
        SGI_GL_REPLACE, SGI_GL_MODULATE, SGI_GL_DECAL, SGI_GL_BLEND,
    };
    static const uint32_t wraps[] = {
        SGI_GL_REPEAT, SGI_GL_CLAMP, SGI_GL_CLAMP_TO_EDGE,
        SGI_GL_CLAMP_TO_BORDER,
    };
    static const uint32_t filters[] = { SGI_GL_NEAREST, SGI_GL_LINEAR };
    PrimCase *p = g_new(PrimCase, 1);
    g_autoptr(GRand) rand = g_rand_new_with_seed(20261009);
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
    for (unsigned s = 0; s < ARRAY_SIZE(shapes); s++) {
        for (unsigned k = 0; k < 3 * ARRAY_SIZE(coords) *
                                 ARRAY_SIZE(weights); k++) {
            unsigned first = k % 3, c = k / 3 % ARRAY_SIZE(coords);
            unsigned wi = k / 3 / ARRAY_SIZE(coords);
            float xy[3][2], st[3][2], w[3];

            for (unsigned v = 0; v < 3; v++) {
                memcpy(xy[v], shapes[s][(first + v) % 3], sizeof(xy[v]));
                memcpy(st[v], coords[c][v], sizeof(st[v]));
                w[v] = weights[wi][v];
            }
            for (unsigned m = 0; m < 2 * ARRAY_SIZE(envs) *
                                     ARRAY_SIZE(filters) *
                                     ARRAY_SIZE(wraps); m++) {
                unsigned smooth = m & 1, e = m / 2 % ARRAY_SIZE(envs);
                unsigned f = m / 2 / ARRAY_SIZE(envs) % ARRAY_SIZE(filters);
                unsigned wr = m / 2 / ARRAY_SIZE(envs) / ARRAY_SIZE(filters);
                g_autofree char *name = g_strdup_printf(
                    "shape %u from %u coords %u w %u %s env 0x%x filter "
                    "0x%x wrap 0x%x", s, first, c, wi,
                    smooth ? "smooth" : "flat", envs[e], filters[f],
                    wraps[wr]);

                prim_case_init(p, SGI_GL_TRIANGLES, 4);
                prim_texture(p, (m / 8) & 1 ? 4 : 2, filters[f], wraps[wr],
                             wraps[(wr + m / 4) % ARRAY_SIZE(wraps)],
                             envs[e]);
                p->state.shade_model = smooth ? SGI_GL_SMOOTH : SGI_GL_FLAT;
                prim_texture_place(p, xy, st, w);
                for (unsigned v = 0; smooth && v < 3; v++) {
                    p->vertices[v][SGI_GL_V_COLOR + v] = 0.1f + 0.4f * v;
                }
                if (prim_compare(renderer, name, p, false)) {
                    failures++;
                }
                cases++;
            }
        }
    }
    for (unsigned i = 0; i < 1200; i++) {
        float xy[3][2], st[3][2], w[3];
        g_autofree char *name = g_strdup_printf("random %u", i);

        for (unsigned v = 0; v < 3; v++) {
            /* Quarter pixels put many edges through pixel centres. */
            xy[v][0] = g_rand_int_range(rand, 0, 65) / 4.0f;
            xy[v][1] = g_rand_int_range(rand, 0, 65) / 4.0f;
            st[v][0] = g_rand_int_range(rand, -64, 129) / 32.0f;
            st[v][1] = g_rand_int_range(rand, -64, 129) / 32.0f;
            w[v] = g_rand_int_range(rand, 2, 17) / 8.0f;
        }
        int state = g_rand_int_range(rand, 0, 6);

        prim_case_init(p, SGI_GL_TRIANGLES, 3 + g_rand_int_range(rand, 0, 2));
        prim_texture(p, 2, filters[g_rand_int_range(rand, 0, 2)],
                     wraps[g_rand_int_range(rand, 0, 4)],
                     wraps[g_rand_int_range(rand, 0, 4)],
                     envs[g_rand_int_range(rand, 0, 4)]);
        /* Power-of-two sizes from 1 to 16, square or not. */
        prim_texture_size(p, 1u << g_rand_int_range(rand, 0, 5),
                          1u << g_rand_int_range(rand, 0, 5));
        p->state.shade_model = g_rand_boolean(rand) ? SGI_GL_SMOOTH :
                                                      SGI_GL_FLAT;
        prim_texture_place(p, xy, st, w);
        /* Then the state the cross cases vary for untextured draws. */
        switch (state) {
        case 1:
            p->top_origin = true;
            break;
        case 2:
            p->state.scissor_test = 1;
            memcpy(p->state.scissor, (int32_t[4]) { -4, 3, 13, 9 },
                   sizeof(p->state.scissor));
            break;
        case 3:
            memcpy(p->state.viewport, (int32_t[4]) { 3, -2, 10, 12 },
                   sizeof(p->state.viewport));
            break;
        case 4:
            prim_set_clips(p, (SGIClipRect[]) {
                { 1, 1, 5, 4 }, { 9, 2, 14, 6 }, { 3, 10, 6, 15 },
            }, 3);
            break;
        case 5:
            prim_matrices(p, g_rand_boolean(rand));
            break;
        }
        for (unsigned v = 0; v < 3; v++) {
            for (unsigned c = 0; c < 4; c++) {
                p->vertices[v][SGI_GL_V_COLOR + c] =
                    g_rand_int_range(rand, 0, 256) / 255.0f;
            }
        }
        if (prim_compare(renderer, name, p, false)) {
            failures++;
        }
        cases++;
    }
    g_test_message("texture sweep: %u of %u cases differ", failures, cases);
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * The scalar path drops a whole textured triangle when any vertex of its
 * clipped polygon has a texture q that is not positive. Here vertex c has
 * q 0 and lies 1/8192 of the half-width inside the right plane, and b lies
 * past it at c's height. Clipping makes a vertex on the plane next to c,
 * so the fan piece holding c snaps to zero area and is dropped, while the
 * piece between a, the cut on ab and the cut on bc, whose q are positive,
 * survives. Mesa must draw nothing, as the scalar path does, or refuse
 * before any snapshot.
 */
static void test_texture_invalid_clipped_vertex(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float clip[3][4] = {
        { -0.75f, -0.75f, 0, 1 },               /* a, inside */
        { 3, 0.5f, 0, 1 },                      /* b, past the right plane */
        { 1 - 1.0f / 8192, 0.5f, 0, 1 },        /* c, just inside it */
    };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(true)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned first = 0; first < 3; first++) {
        g_autofree PrimFixture *scalar = g_new(PrimFixture, 1);
        g_autofree PrimFixture *mesa = g_new(PrimFixture, 1);
        g_autofree char *name = g_strdup_printf("q 0 at c, from %u", first);
        unsigned drawn = 0;
        SGIRenderResult result;

        prim_case_init(p, SGI_GL_TRIANGLES, 4);
        prim_texture(p, 2, SGI_GL_NEAREST, SGI_GL_REPEAT, SGI_GL_REPEAT,
                     SGI_GL_REPLACE);
        for (unsigned i = 0; i < 3; i++) {
            unsigned v = (first + i) % 3;

            memcpy(&p->vertices[i][SGI_GL_V_POSITION], clip[v],
                   sizeof(clip[0]));
            p->vertices[i][SGI_GL_V_TEXCOORD + 3] = v == 2 ? 0 : 1;
        }
        prim_destination(scalar, p);
        prim_destination(mesa, p);
        prim_scalar_draw(p, scalar);
        for (unsigned y = 0; y < PRIM_SIZE; y++) {
            for (unsigned x = 0; x < PRIM_SIZE; x++) {
                drawn += scalar->written[y][x];
            }
        }
        g_assert_cmpuint(drawn, ==, 0);
        result = prim_mesa_draw(renderer, p, mesa, &err);
        g_clear_pointer(&err, error_free);
        if (result == SGI_RENDER_UNSUPPORTED && !mesa->snapshots) {
            continue;
        }
        if (result != SGI_RENDER_OK || prim_report(name, scalar, mesa)) {
            g_test_message("%s: result %d differs from the scalar drop",
                           name, result);
            failures++;
        }
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * The scalar path drops a fragment whose interpolated depth rounds outside
 * [0, 1]. With two vertices on the near or far plane, pixel centres on
 * their edge sit at a depth the scalar sum can round past the plane. Flat,
 * smooth and textured draws must match the scalar path, drops included.
 */
static void test_scalar_match_depth_planes(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const struct {
        const char *name;
        float v[3][2];
    } shapes[] = {
        { "left", { { 2.5f, 1.5f }, { 2.5f, 14.5f }, { 13.25f, 7.75f } } },
        { "right", { { 13.5f, 14.5f }, { 13.5f, 1.5f }, { 2.25f, 8.25f } } },
        { "top", { { 14.5f, 13.5f }, { 1.5f, 13.5f }, { 7.75f, 2.25f } } },
        { "bottom", { { 1.5f, 2.5f }, { 14.5f, 2.5f }, { 8.25f, 13.75f } } },
        { "diagonal", { { 13.5f, 13.5f }, { 1.5f, 1.5f }, { 1.75f, 12.25f } } },
    };
    static const float apex[] = { -0.37f, 0.1f, 0.53f, 0.9f, -0.85f };
    /* Normalized device z of the edge: near, far, and away from both. */
    static const float planes[] = { -1, 1, 0.25f };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(true)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned s = 0; s < ARRAY_SIZE(shapes); s++) {
        for (unsigned z = 0; z < ARRAY_SIZE(planes); z++) {
            for (unsigned a = 0; a < ARRAY_SIZE(apex); a++) {
                for (unsigned k = 0; k < 9; k++) {
                    /*
                     * Flat, smooth and textured, in each vertex order, so
                     * the apex may come first.
                     */
                    static const char *const kinds[] = {
                        "flat", "smooth", "textured",
                    };
                    unsigned kind = k % 3, first = k / 3;
                    g_autofree PrimFixture *scalar = g_new(PrimFixture, 1);
                    g_autofree PrimFixture *mesa = g_new(PrimFixture, 1);
                    g_autofree char *name = g_strdup_printf(
                        "%s %s edge at z %g, apex z %g, from vertex %u",
                        kinds[kind], shapes[s].name, planes[z], apex[a],
                        first);
                    SGIRenderResult result;
                    unsigned differences;

                    prim_case_init(p, SGI_GL_TRIANGLES, 4);
                    if (kind == 2) {
                        prim_texture(p, 2, SGI_GL_NEAREST, SGI_GL_REPEAT,
                                     SGI_GL_REPEAT, SGI_GL_MODULATE);
                    } else {
                        p->state.shade_model = kind ? SGI_GL_SMOOTH :
                                                      SGI_GL_FLAT;
                        for (unsigned i = 0; i < 3; i++) {
                            prim_vertex(p, 0, 0, 0.2f + i * 0.3f, 0.7f,
                                        0.4f, 1);
                        }
                    }
                    for (unsigned i = 0; i < 3; i++) {
                        unsigned v = (first + i) % 3;
                        float *pos = &p->vertices[i][SGI_GL_V_POSITION];

                        pos[0] = shapes[s].v[v][0] * 2 / PRIM_SIZE - 1;
                        pos[1] = shapes[s].v[v][1] * 2 / PRIM_SIZE - 1;
                        pos[2] = v < 2 ? planes[z] : apex[a];
                    }
                    prim_destination(scalar, p);
                    prim_destination(mesa, p);
                    prim_scalar_draw(p, scalar);
                    result = prim_mesa_draw(renderer, p, mesa, &err);
                    if (result != SGI_RENDER_OK) {
                        g_test_message("%s: checked draw returned %d: %s",
                                       name, result, err ?
                                       error_get_pretty(err) : "(no error)");
                        g_clear_pointer(&err, error_free);
                        failures++;
                        continue;
                    }
                    differences = prim_report(name, scalar, mesa);
                    if (differences) {
                        g_test_message("%s: %u pixels differ", name,
                                       differences);
                        failures++;
                    }
                }
            }
        }
    }
    g_test_message("depth planes: %u differ", failures);
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
 * A draw on a large resource moves only the region it can write: the
 * snapshot and commit rectangles stay near the geometry, pixels outside it
 * keep their values, and every pixel equals the scalar path's. Region
 * cases cover flat and smooth triangles, points, lines, a blended and a
 * textured triangle, several clips and a scissor, on both row origins.
 */
#define REGION_W 300
#define REGION_H 200

typedef struct RegionFixture {
    uint32_t *rgba;             /* REGION_W * REGION_H * 4, native rows */
    uint8_t *written;
    bool top_origin;
    uint64_t largest_rect;      /* the largest snapshot or commit area */
    const PrimCase *p;          /* its texels, for a textured draw */
} RegionFixture;

static unsigned region_row(const RegionFixture *f, int y)
{
    return f->top_origin ? REGION_H - 1 - y : y;
}

static void region_scalar_store(void *opaque, SGIColourTarget target, int x,
                                int y, const SGIRasterColour *colour)
{
    RegionFixture *f = opaque;
    size_t at;

    if (x < 0 || x >= REGION_W || y < 0 || y >= REGION_H) {
        return;
    }
    at = (size_t)region_row(f, y) * REGION_W + x;
    memcpy(&f->rgba[at * 4], colour->component, 4 * sizeof(uint32_t));
    f->written[at] = 1;
}

static bool region_scalar_load(void *opaque, SGIColourTarget target, int x,
                               int y, SGIRasterColour *colour)
{
    RegionFixture *f = opaque;

    if (x < 0 || x >= REGION_W || y < 0 || y >= REGION_H) {
        return false;
    }
    memcpy(colour->component,
           &f->rgba[((size_t)region_row(f, y) * REGION_W + x) * 4],
           4 * sizeof(uint32_t));
    return true;
}

static SGIRenderResult region_snapshot(void *opaque,
                                       const SGIRenderResource *res,
                                       uint32_t slice,
                                       const SGIRenderRect *rect,
                                       uint32_t *slots, size_t stride_slots,
                                       size_t capacity_slots, Error **errp)
{
    RegionFixture *f = opaque;
    uint64_t area = (uint64_t)(rect->x1 - rect->x0 + 1) *
                    (rect->y1 - rect->y0 + 1);

    if (res->role == SGI_RENDER_TEXTURE) {
        for (uint32_t y = rect->y0; y <= rect->y1; y++) {
            for (uint32_t x = rect->x0; x <= rect->x1; x++) {
                memcpy(&slots[(y - rect->y0) * stride_slots +
                              (x - rect->x0) * 4],
                       f->p->texels[y][x], 4 * sizeof(uint32_t));
            }
        }
        return SGI_RENDER_OK;
    }
    f->largest_rect = MAX(f->largest_rect, area);
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            memcpy(&slots[(y - rect->y0) * stride_slots + (x - rect->x0) * 4],
                   &f->rgba[((size_t)y * REGION_W + x) * 4],
                   4 * sizeof(uint32_t));
        }
    }
    return SGI_RENDER_OK;
}

static SGIRenderCommitResult region_commit_covered(
    void *opaque, const SGIRenderResource *res, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, const SGIRenderWriteCoverage *writes,
    Error **errp)
{
    RegionFixture *f = opaque;
    uint64_t area = (uint64_t)(rect->x1 - rect->x0 + 1) *
                    (rect->y1 - rect->y0 + 1);

    f->largest_rect = MAX(f->largest_rect, area);
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            size_t at = (size_t)y * REGION_W + x;

            if (!writes->pixel[(size_t)(y - rect->y0) * writes->stride_bytes +
                               x - rect->x0]) {
                continue;
            }
            memcpy(&f->rgba[at * 4],
                   &slots[(y - rect->y0) * stride_slots + (x - rect->x0) * 4],
                   4 * sizeof(uint32_t));
            f->written[at] = 1;
        }
    }
    return (SGIRenderCommitResult) {
        .status = SGI_RENDER_OK, .complete_pixels = area,
        .extent_known = true,
    };
}

static const SGIRenderResourceOps region_ops = {
    .snapshot = region_snapshot,
    .commit_covered = region_commit_covered,
};

static void region_fill(RegionFixture *f)
{
    memset(f->written, 0, REGION_W * REGION_H);
    for (size_t i = 0; i < (size_t)REGION_W * REGION_H; i++) {
        f->rgba[i * 4] = i * 7 & 255;
        f->rgba[i * 4 + 1] = i * 13 & 255;
        f->rgba[i * 4 + 2] = i * 29 & 255;
        f->rgba[i * 4 + 3] = i * 3 & 255;
    }
}

static void test_draw_region(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const SGIClipRect several[] = {
        { 0, 0, 299, 199 },
        { 20, 15, 140, 90 }, { 120, 60, 260, 180 },
    };
    RegionFixture scalar = { 0 }, mesa = { 0 };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(true)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    scalar.rgba = g_new(uint32_t, REGION_W * REGION_H * 4);
    mesa.rgba = g_new(uint32_t, REGION_W * REGION_H * 4);
    scalar.written = g_new(uint8_t, REGION_W * REGION_H);
    mesa.written = g_new(uint8_t, REGION_W * REGION_H);
    for (unsigned k = 0; k < 2 * 8; k++) {
        unsigned kind = k / 2;
        bool top = k & 1;
        SGIRenderResource res[2];
        SGIRasterRules rules;
        SGIRasterTarget scalar_target = {
            .opaque = &scalar, .store_rgba = region_scalar_store,
            .load_rgba = region_scalar_load,
        };
        SGIRasterTarget mesa_target = {
            .opaque = &mesa, .resources = res, .resource_count = 1,
            .resource_ops = &region_ops,
        };
        SGIGLDraw draw;
        SGIRenderResult result;
        unsigned differences = 0;
        /* The geometry's window box, plus margin, bounds the region. */
        uint64_t bound = 40 * 40;
        const char *names[] = {
            "flat fan", "smooth fan", "points", "lines", "blended triangle",
            "textured triangle", "several clips", "scissor",
        };

        prim_case_init(p, kind == 2 ? SGI_GL_POINTS :
                          kind == 3 ? SGI_GL_LINE_STRIP :
                          kind == 0 || kind == 1 ? SGI_GL_TRIANGLE_FAN :
                          SGI_GL_TRIANGLES, 4);
        p->state.viewport[2] = REGION_W;
        p->state.viewport[3] = REGION_H;
        p->state.drawable.clip.r[0] = (SGIClipRect) {
            0, 0, REGION_W - 1, REGION_H - 1,
        };
        if (kind == 5) {
            prim_texture(p, 4, SGI_GL_LINEAR, SGI_GL_REPEAT, SGI_GL_CLAMP,
                         SGI_GL_MODULATE);
            p->draw.count = 0;
        }
        /* Window positions in a 300x200 viewport, near (150,100). */
#define V(x, y, r) do { \
            float *v_ = p->vertices[p->draw.count]; \
            prim_vertex(p, 0, 0, r, 0.6f, 0.3f, 0.8f); \
            v_[SGI_GL_V_POSITION] = (x) * 2.0f / REGION_W - 1; \
            v_[SGI_GL_V_POSITION + 1] = (y) * 2.0f / REGION_H - 1; \
        } while (0)
        V(131.25f, 93.5f, 0.1f);
        V(160.5f, 95.25f, 0.5f);
        V(155.75f, 121, 0.9f);
        if (kind <= 1 || kind >= 6) {
            V(133, 117.5f, 0.4f);
        }
#undef V
        if (kind == 5) {
            for (unsigned v = 0; v < 3; v++) {
                p->vertices[v][SGI_GL_V_TEXCOORD] = v * 0.7f;
                p->vertices[v][SGI_GL_V_TEXCOORD + 1] = v ? 1.3f : -0.2f;
                p->vertices[v][SGI_GL_V_TEXCOORD + 3] = 1;
            }
        }
        p->state.shade_model = kind == 1 || kind == 3 ? SGI_GL_SMOOTH :
                                                      SGI_GL_FLAT;
        p->draw.point_size = 3;
        p->draw.line_width = 2;
        if (kind == 4) {
            p->state.blend = 1;
            p->state.blend_equation = SGI_GL_FUNC_ADD;
            p->state.blend_src = SGI_GL_SRC_ALPHA;
            p->state.blend_dst = SGI_GL_ONE_MINUS_SRC_ALPHA;
        }
        if (kind == 6) {
            prim_set_clips(p, several, ARRAY_SIZE(several));
        }
        if (kind == 7) {
            p->state.scissor_test = 1;
            memcpy(p->state.scissor, (int32_t[4]) { 140, 0, 300, 105 },
                   sizeof(p->state.scissor));
        }
        scalar.top_origin = mesa.top_origin = top;
        scalar.p = mesa.p = p;
        region_fill(&scalar);
        region_fill(&mesa);
        prim_rules(p, &rules);
        prim_resources(p, res);
        res[0].width = REGION_W;
        res[0].height = REGION_H;
        res[0].bottom_origin = !top;
        draw = p->draw;
        p->image.read_texel = prim_read_texel;
        p->image.opaque = p;
        scalar_target.texture = mesa_target.texture =
            p->state.texture_2d ? &p->image : NULL;
        mesa_target.resource_count = p->state.texture_2d ? 2 : 1;
        g_assert_cmpint(sgi_soft_draw(&rules, &scalar_target, &draw), ==,
                        SGI_GL_DRAW_OK);
        if (top) {
            /* Drawable clips arrive in native rows. */
            for (unsigned i = 0; i < p->state.drawable.clip.n; i++) {
                SGIClipRect *r = &p->state.drawable.clip.r[i];
                int32_t y0 = r->y0;

                r->y0 = REGION_H - 1 - r->y1;
                r->y1 = REGION_H - 1 - y0;
            }
        }
        mesa.largest_rect = 0;
        result = sgi_render_draw_checked(renderer, &rules, &mesa_target,
                                         &draw, &err);
        if (result != SGI_RENDER_OK) {
            g_test_message("%s %s: result %d: %s", names[kind],
                           top ? "top" : "bottom", result,
                           err ? error_get_pretty(err) : "");
            g_clear_pointer(&err, error_free);
            failures++;
            continue;
        }
        for (size_t i = 0; i < (size_t)REGION_W * REGION_H; i++) {
            if (memcmp(&scalar.rgba[i * 4], &mesa.rgba[i * 4],
                       4 * sizeof(uint32_t)) ||
                scalar.written[i] != mesa.written[i]) {
                if (!differences++) {
                    g_test_message("%s %s: first difference at (%zu,%zu)",
                                   names[kind], top ? "top" : "bottom",
                                   i % REGION_W, i / REGION_W);
                }
            }
        }
        g_test_message("%s %s: largest rectangle %" PRIu64 " pixels, "
                       "%u differ", names[kind], top ? "top" : "bottom",
                       mesa.largest_rect, differences);
        if (differences || mesa.largest_rect > bound) {
            failures++;
        }
    }
    sgi_renderer_free(renderer);
    g_free(scalar.rgba);
    g_free(mesa.rgba);
    g_free(scalar.written);
    g_free(mesa.written);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * A draw whose window extent lies wholly off the resource, in the part of
 * a viewport that hangs off it, writes nothing on the scalar path. The
 * checked draw must agree: OK, no snapshot, no pixel changed. Left of or
 * below the resource, or above it for triangles, which run upside down,
 * the region's far edge is negative, so it must come out empty rather
 * than wrap in the region's unsigned fields.
 */
static void test_draw_region_off_resource(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    RegionFixture scalar = { 0 }, mesa = { 0 };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(true)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    scalar.rgba = g_new(uint32_t, REGION_W * REGION_H * 4);
    mesa.rgba = g_new(uint32_t, REGION_W * REGION_H * 4);
    scalar.written = g_new(uint8_t, REGION_W * REGION_H);
    mesa.written = g_new(uint8_t, REGION_W * REGION_H);
    for (unsigned k = 0; k < 2 * 3 * 2; k++) {
        static const char *const sides[] = { "left", "below", "above" };
        bool triangle = k & 1, top = k / 6;
        unsigned side = k / 2 % 3;
        SGIRenderResource res[2];
        SGIRasterRules rules;
        SGIRasterTarget scalar_target = {
            .opaque = &scalar, .store_rgba = region_scalar_store,
            .load_rgba = region_scalar_load,
        };
        SGIRasterTarget mesa_target = {
            .opaque = &mesa, .resources = res, .resource_count = 1,
            .resource_ops = &region_ops,
        };
        const char *name = triangle ? "triangle" : "line";
        SGIGLDraw draw;
        SGIRenderResult result;
        unsigned differences = 0;

        prim_case_init(p, triangle ? SGI_GL_TRIANGLES : SGI_GL_LINES, 4);
        /* A 300x200 viewport with 100 columns or rows off one edge. */
        p->state.viewport[0] = side == 0 ? -100 : 0;
        p->state.viewport[1] = side == 1 ? -100 : side == 2 ? 100 : 0;
        p->state.viewport[2] = REGION_W;
        p->state.viewport[3] = REGION_H;
        p->state.drawable.clip.r[0] = (SGIClipRect) {
            0, 0, REGION_W - 1, REGION_H - 1,
        };
        /*
         * a runs 20 to 60 viewport pixels in from the off edge, 80 to 40
         * pixels off the resource; b runs along that edge.
         */
#define V(a, b) do { \
            float *v_ = p->vertices[p->draw.count]; \
            float x_ = side ? (b) : (a); \
            float y_ = side == 1 ? (a) : side == 2 ? REGION_H - (a) : (b); \
            prim_vertex(p, 0, 0, 0.6f, 0.3f, 0.8f, 1); \
            v_[SGI_GL_V_POSITION] = x_ * 2.0f / REGION_W - 1; \
            v_[SGI_GL_V_POSITION + 1] = y_ * 2.0f / REGION_H - 1; \
        } while (0)
        V(20.5f, 40.5f);
        V(60.5f, 70.5f);
        if (triangle) {
            V(30.5f, 90.5f);
        }
#undef V
        p->draw.line_width = 1;
        scalar.top_origin = mesa.top_origin = top;
        scalar.p = mesa.p = p;
        region_fill(&scalar);
        region_fill(&mesa);
        prim_rules(p, &rules);
        prim_resources(p, res);
        res[0].width = REGION_W;
        res[0].height = REGION_H;
        res[0].bottom_origin = !top;
        draw = p->draw;
        g_assert_cmpint(sgi_soft_draw(&rules, &scalar_target, &draw), ==,
                        SGI_GL_DRAW_OK);
        mesa.largest_rect = 0;
        result = sgi_render_draw_checked(renderer, &rules, &mesa_target,
                                         &draw, &err);
        if (result != SGI_RENDER_OK) {
            g_test_message("%s %s %s: result %d: %s", name, sides[side],
                           top ? "top" : "bottom", result,
                           err ? error_get_pretty(err) : "");
            g_clear_pointer(&err, error_free);
            failures++;
            continue;
        }
        for (size_t i = 0; i < (size_t)REGION_W * REGION_H; i++) {
            differences += scalar.written[i] || mesa.written[i] ||
                           memcmp(&scalar.rgba[i * 4], &mesa.rgba[i * 4],
                                  4 * sizeof(uint32_t));
        }
        g_test_message("%s %s %s: largest rectangle %" PRIu64 " pixels, "
                       "%u differ", name, sides[side],
                       top ? "top" : "bottom", mesa.largest_rect,
                       differences);
        if (differences || mesa.largest_rect) {
            failures++;
        }
    }
    sgi_renderer_free(renderer);
    g_free(scalar.rgba);
    g_free(mesa.rgba);
    g_free(scalar.written);
    g_free(mesa.written);
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
 * rules without alpha the scalar path hands the store canonical zero alpha
 * where Mesa would store the colour's alpha, so that pairing is refused
 * too.
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

/* A segment from window (x0, y0) to (x1, y1) under the case's viewport. */
static void prim_segment(PrimCase *p, float x0, float y0, float x1, float y1)
{
    prim_vertex_window(p, x0, y0);
    prim_vertex_window(p, x1, y1);
}

/*
 * Exact ties: endpoints on diamond edges and corners in all four diagonal
 * directions, rows and columns through pixel centres, slopes of one
 * through corners, half-pixel shifts at even widths, and one-subpixel
 * moves either side of each, at widths 1 to 6.
 */
static void test_scalar_match_line_ties(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float ties[][4] = {
        { 2.75f, 14.25f, 7.5f, 11 }, { 7.5f, 11, 2.75f, 14.25f },
        { 3.25f, 2.75f, 9.5f, 4.5f }, { 9.5f, 4.5f, 3.25f, 2.75f },
        { 2.5f, 2, 8.5f, 8 }, { 8.5f, 8, 2.5f, 2 },
        { 12.5f, 2, 6.5f, 8 }, { 6.5f, 8, 12.5f, 2 },
        { 1.5f, 5.5f, 12.5f, 5.5f }, { 12.5f, 6.5f, 1.5f, 6.5f },
        { 6.5f, 1.5f, 6.5f, 12.5f }, { 7.5f, 13.5f, 7.5f, 2.5f },
        { 2, 2, 10, 10 }, { 12, 2, 4, 10 }, { 10, 10, 2, 2 },
        { 2.25f, 7, 14, 9.5f }, { 14, 9.5f, 2.25f, 7 },
        { 4.75f, 3.25f, 5.25f, 13.75f }, { 11, 1.5f, 3, 13.5f },
    };
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
    for (unsigned width = 1; width <= 6; width++) {
        for (unsigned i = 0; i < ARRAY_SIZE(ties); i++) {
            for (int nudge = -1; nudge <= 1; nudge++) {
                const float d = nudge / 256.0f;
                g_autofree char *name = g_strdup_printf(
                    "tie %u width %u nudge %d", i, width, nudge);

                prim_case_init(p, SGI_GL_LINES, 4);
                p->draw.line_width = width;
                prim_segment(p, ties[i][0] + d, ties[i][1] - d,
                             ties[i][2] + d, ties[i][3] + d);
                prim_expect(renderer, name, p, &failures);
                cases++;
            }
        }
    }
    g_test_message("line ties: %u of %u cases differ", failures, cases);
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * Strips and loops sharing endpoints as direction changes, smooth colour
 * with perspective weights, viewport, scissor and clip bounds, a long
 * segment and a single blended segment.
 */
static void test_scalar_match_line_state(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const SGIClipRect clips[] = {
        { 1, 1, 6, 5 }, { 5, 4, 12, 10 }, { 13, 0, 15, 15 },
    };
    static const float strip[][2] = {
        { 1.5f, 1.5f }, { 7.5f, 4.5f }, { 4.5f, 12.5f }, { 13.5f, 13.5f },
        { 14.25f, 2.75f }, { 2.75f, 8.25f },
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
        for (unsigned width = 1; width <= 4; width += 3) {
            for (unsigned mode = SGI_GL_LINE_LOOP; mode <= SGI_GL_LINE_STRIP;
                 mode++) {
                g_autofree char *name = g_strdup_printf(
                    "%s %s width %u", shade ? "smooth" : "flat",
                    mode == SGI_GL_LINE_LOOP ? "loop" : "strip", width);

                prim_case_init(p, mode, 4);
                p->state.shade_model = shade ? SGI_GL_SMOOTH : SGI_GL_FLAT;
                p->draw.line_width = width;
                for (unsigned v = 0; v < ARRAY_SIZE(strip); v++) {
                    prim_vertex_window(p, strip[v][0], strip[v][1]);
                }
                prim_expect(renderer, name, p, &failures);
            }
        }
    }

    /* Clip w of 2 at one end weights smooth colour for perspective. */
    for (unsigned width = 1; width <= 3; width += 2) {
        g_autofree char *name = g_strdup_printf("perspective width %u",
                                                width);
        float *v;

        prim_case_init(p, SGI_GL_LINES, 4);
        p->state.shade_model = SGI_GL_SMOOTH;
        p->draw.line_width = width;
        prim_segment(p, 1.25f, 2.5f, 14.5f, 12.75f);
        v = p->vertices[1];
        for (unsigned c = 0; c < 4; c++) {
            v[SGI_GL_V_POSITION + c] *= 2;
        }
        prim_expect(renderer, name, p, &failures);
    }

    prim_case_init(p, SGI_GL_LINE_STRIP, 4);
    p->state.shade_model = SGI_GL_SMOOTH;
    p->draw.line_width = 2;
    prim_set_clips(p, clips, ARRAY_SIZE(clips));
    p->state.scissor_test = 1;
    memcpy(p->state.scissor, (int32_t[4]) { 2, 0, 12, 13 },
           sizeof(p->state.scissor));
    for (unsigned v = 0; v < ARRAY_SIZE(strip); v++) {
        prim_vertex_window(p, strip[v][0], strip[v][1]);
    }
    prim_expect(renderer, "clips and scissor", p, &failures);

    /* The same strip on a top-origin resource, rows mapped as a family does. */
    for (unsigned width = 1; width <= 3; width += 2) {
        g_autofree char *name = g_strdup_printf("top-origin strip width %u",
                                                width);

        p->top_origin = true;
        p->draw.line_width = width;
        prim_expect(renderer, name, p, &failures);
    }

    /* Fragments past the viewport are dropped, as the scalar path drops. */
    prim_case_init(p, SGI_GL_LINES, 4);
    memcpy(p->state.viewport, (int32_t[4]) { 3, 2, 9, 10 },
           sizeof(p->state.viewport));
    p->draw.line_width = 5;
    prim_segment(p, 3.25f, 6.5f, 11.75f, 8.25f);
    prim_segment(p, 7.5f, 2.25f, 9.5f, 11.75f);
    prim_expect(renderer, "wide lines at the viewport edge", p, &failures);

    prim_case_init(p, SGI_GL_LINES, 4);
    memcpy(p->state.viewport, (int32_t[4]) { -2000, -2000, 4000, 4000 },
           sizeof(p->state.viewport));
    p->state.shade_model = SGI_GL_SMOOTH;
    p->draw.line_width = 3;
    prim_segment(p, -1500.25f, 3.5f, 1500.75f, 12.25f);
    prim_segment(p, 7.25f, -1700.5f, 9.75f, 1800.25f);
    prim_expect(renderer, "long segments", p, &failures);

    prim_case_init(p, SGI_GL_LINES, 4);
    p->state.blend = 1;
    p->state.blend_src = SGI_GL_SRC_ALPHA;
    p->state.blend_dst = SGI_GL_ONE_MINUS_SRC_ALPHA;
    p->state.blend_equation = SGI_GL_FUNC_ADD;
    p->state.shade_model = SGI_GL_SMOOTH;
    p->draw.line_width = 3;
    prim_segment(p, 1.25f, 2.5f, 14.5f, 12.75f);
    prim_expect(renderer, "one blended segment", p, &failures);
    prim_segment(p, 2.5f, 13.25f, 12.75f, 1.5f);
    prim_expect_refused(renderer, "two blended segments", p, &failures);

    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * Stipple counts produced base fragments in submitted direction, carried
 * along strips and loops and, through a borrowed phase, across draws, and
 * counted even for fragments outside the drawable.
 */
static void test_scalar_match_line_stipple(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const uint16_t patterns[] = { 0xf0f0, 0x5a3c, 0x0001, 0xfff7 };
    static const float strip[][2] = {
        { 1.5f, 1.25f }, { 14.5f, 4.75f }, { 3.25f, 13.5f },
        { 12.75f, 10.5f }, { 20.5f, 30.25f }, { 2.5f, 7.5f },
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
    for (unsigned i = 0; i < ARRAY_SIZE(patterns); i++) {
        for (unsigned repeat = 1; repeat <= 3; repeat++) {
            for (unsigned width = 1; width <= 3; width += 2) {
                for (unsigned mode = SGI_GL_LINES; mode <= SGI_GL_LINE_STRIP;
                     mode++) {
                    g_autofree char *name = g_strdup_printf(
                        "stipple 0x%04x x%u width %u mode %u", patterns[i],
                        repeat, width, mode);

                    prim_case_init(p, mode, 4);
                    p->draw.line_width = width;
                    p->draw.line_stipple_enabled = true;
                    p->draw.line_stipple_repeat = repeat;
                    p->draw.line_stipple_pattern = patterns[i];
                    for (unsigned v = 0; v < ARRAY_SIZE(strip); v++) {
                        prim_vertex_window(p, strip[v][0], strip[v][1]);
                    }
                    prim_expect(renderer, name, p, &failures);
                }
            }
        }
    }

    /* A borrowed phase: two draws continue one count, including clips. */
    for (unsigned clipped = 0; clipped < 2; clipped++) {
        prim_case_init(p, SGI_GL_LINES, 4);
        p->use_phase = true;
        p->phase[0] = p->phase[1] = 5;
        p->draw.line_stipple_enabled = true;
        p->draw.line_stipple_repeat = 2;
        p->draw.line_stipple_pattern = 0x3c5a;
        if (clipped) {
            p->state.drawable.clip.n = 0;
        }
        prim_segment(p, 1.25f, 2.75f, 13.5f, 6.25f);
        prim_segment(p, 4.5f, 14.25f, 9.75f, 1.5f);
        prim_segment(p, 6, 6, 6, 6);
        for (unsigned draw = 0; draw < 2; draw++) {
            g_autofree char *name = g_strdup_printf(
                "phase draw %u%s", draw, clipped ? " with no clip" : "");

            prim_expect(renderer, name, p, &failures);
            if (p->phase[0] != p->phase[1]) {
                g_test_message("%s: scalar phase %u, Mesa phase %u", name,
                               p->phase[0], p->phase[1]);
                failures++;
            }
        }
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * A seeded sweep of random segments: endpoints past the window edges, so
 * view-volume clipping cuts some, on quarter-pixel and 1/256 grids, at
 * widths 1 to 6, flat and smooth, with clip w of 0.5, 1 or 2 at each end
 * and stipple on a third of them.
 */
static void test_scalar_match_line_sweep(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float ws[] = { 0.5f, 1, 2 };
    PrimCase *p = g_new(PrimCase, 1);
    GRand *rand = g_rand_new_with_seed(20261009);
    SGIRenderer *renderer;
    unsigned failures = 0, cases = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_rand_free(rand);
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned i = 0; i < 600; i++) {
        float grid = i & 1 ? 256 : 4;
        float e[4];
        g_autofree char *name = NULL;

        for (unsigned k = 0; k < 4; k++) {
            e[k] = g_rand_int_range(rand, -3 * grid, 19 * grid) / grid;
        }
        prim_case_init(p, SGI_GL_LINES, 4);
        p->state.shade_model = g_rand_boolean(rand) ? SGI_GL_SMOOTH :
                                                      SGI_GL_FLAT;
        p->draw.line_width = g_rand_int_range(rand, 1, 7);
        if (i % 3 == 0) {
            p->draw.line_stipple_enabled = true;
            p->draw.line_stipple_repeat = g_rand_int_range(rand, 1, 4);
            p->draw.line_stipple_pattern = g_rand_int_range(rand, 0, 65536);
        }
        prim_segment(p, e[0], e[1], e[2], e[3]);
        for (unsigned v = 0; v < 2; v++) {
            float w = ws[g_rand_int_range(rand, 0, ARRAY_SIZE(ws))];

            for (unsigned c = 0; c < 4; c++) {
                p->vertices[v][SGI_GL_V_POSITION + c] *= w;
            }
        }
        name = g_strdup_printf("sweep %u (%g,%g)-(%g,%g) width %g", i, e[0],
                               e[1], e[2], e[3], p->draw.line_width);
        prim_expect(renderer, name, p, &failures);
        cases++;
    }
    g_test_message("line sweep: %u of %u cases differ", failures, cases);
    sgi_renderer_free(renderer);
    g_rand_free(rand);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * Segments the parallelogram route cannot draw exactly are refused before
 * the snapshot: a vertex past the line viewport, a huge width, and every
 * line on a renderer whose probe found a difference.
 */
static void test_lines_refused(void)
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
    prim_case_init(p, SGI_GL_LINES, 4);
    memcpy(p->state.viewport, (int32_t[4]) { -8192, -8192, 16384, 16384 },
           sizeof(p->state.viewport));
    prim_segment(p, -6000.25f, 5.5f, 6000.75f, 9.25f);
    prim_expect_refused(renderer, "past the line viewport", p, &failures);

    prim_case_init(p, SGI_GL_LINES, 4);
    p->draw.line_width = 9000;
    prim_segment(p, 1.25f, 2.5f, 14.5f, 12.75f);
    prim_expect_refused(renderer, "huge width", p, &failures);

    prim_case_init(p, SGI_GL_LINES, 4);
    p->smooth_rules = true;
    p->draw.line_smooth = true;
    prim_segment(p, 1.25f, 2.5f, 14.5f, 12.75f);
    prim_expect_refused(renderer, "smoothed under smoothing rules", p,
                        &failures);

    /*
     * A draw refused after its segments are counted, here for want of a
     * covered commit, leaves a borrowed phase alone, so the caller's scalar
     * fallback counts it once.
     */
    {
        static const SGIRenderResourceOps no_covered = {
            .snapshot = prim_snapshot,
        };
        SGIRenderResource res[2];
        SGIRasterRules rules;
        g_autofree PrimFixture *f = g_new(PrimFixture, 1);
        SGIRasterTarget target = {
            .opaque = f, .resources = res, .resource_count = 1,
            .resource_ops = &no_covered,
        };
        uint32_t phase = 7;
        SGIRenderResult result;

        prim_case_init(p, SGI_GL_LINE_STRIP, 4);
        p->draw.line_stipple_enabled = true;
        p->draw.line_stipple_repeat = 2;
        p->draw.line_stipple_pattern = 0x3c5a;
        prim_segment(p, 1.25f, 2.5f, 14.5f, 12.75f);
        prim_vertex_window(p, 3.5f, 13.25f);
        p->draw.line_phase = &phase;
        prim_rules(p, &rules);
        prim_resources(p, res);
        prim_destination(f, p);
        result = sgi_render_draw_checked(renderer, &rules, &target, &p->draw,
                                         &err);
        if (result != SGI_RENDER_UNSUPPORTED || phase != 7 || f->snapshots) {
            g_test_message("refused draw: result %d, phase %u, %u snapshots",
                           result, phase, f->snapshots);
            failures++;
        }
        g_clear_pointer(&err, error_free);
        p->draw.line_phase = NULL;
    }
    sgi_renderer_free(renderer);

    /* Moving the band one subpixel must fail the probe and refuse lines. */
    sgi_render_test_line_band(1);
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    prim_case_init(p, SGI_GL_LINES, 4);
    prim_segment(p, 1.25f, 2.5f, 14.5f, 12.75f);
    prim_expect_refused(renderer, "probe failure", p, &failures);
    sgi_renderer_free(renderer);
    sgi_render_test_line_band(0);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * A renderer whose smooth triangles differ from the scalar path, here
 * moved one subpixel right, must refuse smooth triangles before any
 * snapshot and still draw flat ones.
 */
static void test_smooth_probe_refused(void)
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
    sgi_render_test_window_shift(1);
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned smooth = 0; smooth < 2; smooth++) {
        prim_case_init(p, SGI_GL_TRIANGLE_FAN, 4);
        p->state.shade_model = smooth ? SGI_GL_SMOOTH : SGI_GL_FLAT;
        prim_vertex_n(p, 1, 1.25f);
        prim_vertex_n(p, 14.5f, 2);
        prim_vertex_n(p, 13, 13.75f);
        prim_vertex_n(p, 2.25f, 12);
        if (smooth) {
            prim_expect_refused(renderer, "probe failure, smooth", p,
                                &failures);
        } else {
            prim_expect(renderer, "probe failure, flat", p, &failures);
        }
    }
    sgi_renderer_free(renderer);
    sgi_render_test_window_shift(0);

    /* A host failure in the probe is a failure, never a refusal. */
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    {
        g_autofree PrimFixture *f = g_new(PrimFixture, 1);
        SGIRenderResult result;

        prim_case_init(p, SGI_GL_TRIANGLE_FAN, 4);
        p->state.shade_model = SGI_GL_SMOOTH;
        prim_vertex_n(p, 1, 1.25f);
        prim_vertex_n(p, 14.5f, 2);
        prim_vertex_n(p, 13, 13.75f);
        prim_destination(f, p);
        saved_renderbuffer_storage = epoxy_glRenderbufferStorage;
        epoxy_glRenderbufferStorage = fail_stencil_storage;
        result = prim_mesa_draw(renderer, p, f, &err);
        epoxy_glRenderbufferStorage = saved_renderbuffer_storage;
        if (result != SGI_RENDER_HOST_ERROR || f->snapshots) {
            g_test_message("probe host failure: result %d, %u snapshots",
                           result, f->snapshots);
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
 * Native GL lines differ from the scalar diamond-exit rule: llvmpipe omits
 * a width-one start pixel on an exact diamond boundary and differs by a
 * row or column at the ends of even widths. These are the vectors that
 * showed it; the parallelogram route must match them.
 */
static void test_scalar_match_lines(void)
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
        { "lines width 5", SGI_GL_LINES, 5, 0 },
        { "lines width 6", SGI_GL_LINES, 6, 0 },
        { "off-tie lines width 6", SGI_GL_LINES, 6, 3.0f / 256 },
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
        prim_expect(renderer, cases[i].name, p, &failures);
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * A viewport whose origin cannot be mirrored for the upside-down triangle
 * frame within 32 bits is refused rather than wrapped.
 */
static void test_viewport_mirror_refused(void)
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
    prim_case_init(p, SGI_GL_TRIANGLES, 4);
    prim_vertex_n(p, 1, 1);
    prim_vertex_n(p, 14, 2);
    prim_vertex_n(p, 3, 13);
    memcpy(p->state.viewport, (int32_t[4]) { 0, INT32_MIN, 16, 16 },
           sizeof(p->state.viewport));
    prim_expect_refused(renderer, "mirrored origin past 32 bits", p,
                        &failures);
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

/*
 * Smoothing parity with the scalar path. Under rules that do not smooth,
 * sgi_soft_draw() ignores the point, line and polygon enables and draws
 * aliased, and Mesa must draw the same pixels. Under rules that smooth,
 * the scalar path smooths points, lines and polygons, so those are
 * refused before the snapshot.
 */
static void test_smoothing_parity(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const struct {
        uint32_t mode;
        bool point, line, polygon;
    } enables[] = {
        { SGI_GL_POINTS, true, false, false },
        { SGI_GL_POINTS, true, true, true },
        { SGI_GL_LINES, false, true, false },
        { SGI_GL_LINE_STRIP, false, true, false },
        { SGI_GL_LINE_LOOP, true, true, true },
        { SGI_GL_TRIANGLES, false, false, true },
        { SGI_GL_TRIANGLES, false, true, false },
        { SGI_GL_QUADS, true, true, true },
        { SGI_GL_POLYGON, false, false, true },
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
    for (unsigned smooth_rules = 0; smooth_rules < 2; smooth_rules++) {
        for (unsigned i = 0; i < ARRAY_SIZE(enables); i++) {
            for (unsigned shade = 0; shade < 2; shade++) {
                bool scalar_smooths = smooth_rules &&
                    ((enables[i].mode == SGI_GL_POINTS && enables[i].point) ||
                     (enables[i].mode >= SGI_GL_LINES &&
                      enables[i].mode <= SGI_GL_LINE_STRIP &&
                      enables[i].line) ||
                     (enables[i].mode >= SGI_GL_TRIANGLES &&
                      enables[i].mode <= SGI_GL_POLYGON &&
                      enables[i].polygon));
                g_autofree char *name = g_strdup_printf(
                    "%s rules mode %u point %d line %d polygon %d %s",
                    smooth_rules ? "smoothing" : "aliased", enables[i].mode,
                    enables[i].point, enables[i].line, enables[i].polygon,
                    shade ? "smooth" : "flat");

                prim_case_init(p, enables[i].mode, 4);
                p->smooth_rules = smooth_rules;
                p->state.shade_model = shade ? SGI_GL_SMOOTH : SGI_GL_FLAT;
                p->draw.point_size = 3;
                p->draw.line_width = 2;
                p->draw.point_smooth = enables[i].point;
                p->draw.line_smooth = enables[i].line;
                p->draw.polygon_smooth = enables[i].polygon;
                prim_vertex_n(p, 2.25f, 2.75f);
                prim_vertex_n(p, 13.5f, 3.25f);
                prim_vertex_n(p, 12.75f, 13.5f);
                prim_vertex_n(p, 3.5f, 12.25f);
                if (scalar_smooths) {
                    prim_expect_refused(renderer, name, p, &failures);
                } else {
                    prim_expect(renderer, name, p, &failures);
                }
            }
        }
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

/* A snapshot that cannot represent the stored data, having read it. */
static SGIRenderResult prim_refusing_snapshot(void *opaque,
                                              const SGIRenderResource *res,
                                              uint32_t slice,
                                              const SGIRenderRect *rect,
                                              uint32_t *slots,
                                              size_t stride_slots,
                                              size_t capacity_slots,
                                              Error **errp)
{
    PrimFixture *f = opaque;

    f->snapshots++;
    error_setg(errp, "test view cannot hold the stored data");
    return SGI_RENDER_UNSUPPORTED;
}

/* A provider that refuses once it has started to commit. */
static SGIRenderCommitResult prim_refusing_commit(
    void *opaque, const SGIRenderResource *res, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, const SGIRenderWriteCoverage *writes,
    Error **errp)
{
    PrimFixture *f = opaque;

    f->commits++;
    error_setg(errp, "test commit refused");
    return (SGIRenderCommitResult) {
        .status = SGI_RENDER_UNSUPPORTED,
        .extent_known = true,
    };
}

typedef struct PrimCheck {
    unsigned calls;
    SGIRenderResult result;
    uint64_t covered;
} PrimCheck;

static SGIRenderResult prim_check_staged(void *opaque,
                                         const SGIRenderResource *res,
                                         const SGIRenderRect *rect,
                                         const uint32_t *slots,
                                         size_t stride_slots,
                                         size_t capacity_slots,
                                         const SGIRenderWriteCoverage *writes,
                                         Error **errp)
{
    PrimCheck *c = opaque;

    c->calls++;
    for (size_t i = 0; i < writes->capacity_bytes; i++) {
        c->covered += writes->pixel[i];
    }
    if (c->result != SGI_RENDER_OK) {
        error_setg(errp, "test precision check result %d", c->result);
    }
    return c->result;
}

static void prim_fan(PrimCase *p, unsigned components)
{
    prim_case_init(p, SGI_GL_TRIANGLE_FAN, components);
    prim_vertex_n(p, 1, 1.25f);
    prim_vertex_n(p, 14.5f, 2);
    prim_vertex_n(p, 13, 13.75f);
    prim_vertex_n(p, 2.25f, 12);
}

/*
 * One attempt; returns 1 and logs it unless the result, status, reason and
 * the fixture's snapshot and commit counts are as expected.
 */
static unsigned prim_attempt_expect(SGIRenderer *renderer, const char *name,
                                    PrimCase *p, SGIRenderDrawResult result,
                                    SGIRenderResult status,
                                    SGIRenderRefusal reason,
                                    unsigned snapshots, unsigned commits)
{
    g_autofree PrimFixture *f = g_new(PrimFixture, 1);
    Error *err = NULL;
    bool ok;

    prim_destination(f, p);
    prim_mesa_draw(renderer, p, f, &err);
    ok = p->attempt_result == result && p->outcome.status == status &&
         p->outcome.reason == reason && f->snapshots == snapshots &&
         f->commits == commits && (result == SGI_RENDER_DRAW_OK) == !err;
    if (!ok) {
        g_test_message("%s: result %d status %d reason %s, %u snapshots, "
                       "%u commits: %s", name, p->attempt_result,
                       p->outcome.status,
                       sgi_render_refusal_name(p->outcome.reason),
                       f->snapshots, f->commits,
                       err ? error_get_pretty(err) : "(no error)");
    }
    g_clear_pointer(&err, error_free);
    return !ok;
}

/*
 * The typed attempt: OK commits; each refusal names its feature row and
 * happens before any snapshot, or for a family snapshot or precision
 * refusal before any commit, and leaves the renderer usable; a failure
 * is never a refusal.
 */
static void test_attempt_outcomes(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const SGIRenderResourceOps refusing_snapshot = {
        .snapshot = prim_refusing_snapshot,
        .commit_covered = prim_commit_covered,
    };
    static const SGIRenderResourceOps refusing_commit = {
        .snapshot = prim_snapshot,
        .commit_covered = prim_refusing_commit,
    };
    static const SGIRenderResourceOps dense_only = {
        .snapshot = prim_snapshot,
        .commit = prim_commit,
    };
    SGIRenderAttemptOptions plain = { 0 };
    PrimCheck check = { 0 };
    SGIRenderAttemptOptions checked = {
        .check_staged = prim_check_staged, .opaque = &check,
    };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0;
    Error *err = NULL;

#define EXPECT(name, setup, result, status, reason, snapshots, commits) \
    do { \
        prim_fan(p, 4); \
        p->attempt = &plain; \
        setup; \
        failures += prim_attempt_expect(renderer, name, p, result, status, \
                                        reason, snapshots, commits); \
    } while (0)
#define REFUSED(name, setup, reason) \
    EXPECT(name, setup, SGI_RENDER_DRAW_REFUSED, SGI_RENDER_UNSUPPORTED, \
           SGI_RENDER_REFUSAL_##reason, 0, 0)

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    EXPECT("ok", , SGI_RENDER_DRAW_OK, SGI_RENDER_OK,
           SGI_RENDER_REFUSAL_NONE, 1, 1);
    REFUSED("rules", p->rules_case = 1, RULES);
    REFUSED("fog", p->state.fog = 1, FOG);
    REFUSED("depth test", p->state.depth_test = 1, FRAGMENT_TESTS);
    REFUSED("cull", p->state.cull_face = 1, FACING);
    REFUSED("colour index without an index view", p->state.index_bits = 8,
            RESOURCE);
    REFUSED("colour mask", p->state.colour_mask = SGI_GL_COLOUR_MASK_RED,
            BUFFERS);
    REFUSED("polygon stipple", p->draw.polygon_stipple_enabled = true,
            PRIMITIVE);
    REFUSED("store width", p->alpha_storage = 4, STORE_WIDTH);
    REFUSED("blend overlap",
            (p->state.blend = 1, p->state.blend_src = SGI_GL_SRC_ALPHA,
             p->state.blend_dst = SGI_GL_ONE_MINUS_SRC_ALPHA,
             p->state.blend_equation = SGI_GL_FUNC_ADD), BLEND_OVERLAP);
    REFUSED("covered commit", p->ops = &dense_only, PROVIDER);
    EXPECT("flat depth plane",
           (p->vertices[0][SGI_GL_V_POSITION + 2] = -1,
            p->vertices[1][SGI_GL_V_POSITION + 2] = -1), SGI_RENDER_DRAW_OK,
           SGI_RENDER_OK, SGI_RENDER_REFUSAL_NONE, 1, 1);
    EXPECT("snapshot refusal", p->ops = &refusing_snapshot,
           SGI_RENDER_DRAW_REFUSED, SGI_RENDER_UNSUPPORTED,
           SGI_RENDER_REFUSAL_SNAPSHOT, 1, 0);
    check = (PrimCheck) { .result = SGI_RENDER_UNSUPPORTED };
    EXPECT("precision refusal", p->attempt = &checked,
           SGI_RENDER_DRAW_REFUSED, SGI_RENDER_UNSUPPORTED,
           SGI_RENDER_REFUSAL_PRECISION, 1, 0);
    if (check.calls != 1 || !check.covered) {
        g_test_message("precision check: %u calls, %" PRIu64 " covered",
                       check.calls, check.covered);
        failures++;
    }
    check = (PrimCheck) { .result = SGI_RENDER_OK };
    EXPECT("precision accepted", p->attempt = &checked, SGI_RENDER_DRAW_OK,
           SGI_RENDER_OK, SGI_RENDER_REFUSAL_NONE, 1, 1);
    check = (PrimCheck) { .result = SGI_RENDER_HOST_ERROR };
    EXPECT("precision check failure", p->attempt = &checked,
           SGI_RENDER_DRAW_FAILED, SGI_RENDER_HOST_ERROR,
           SGI_RENDER_REFUSAL_NONE, 1, 0);
    /*
     * A borrowed stipple phase is the scalar fallback's to count: a family
     * snapshot or precision refusal must leave it as it was.
     */
    for (unsigned i = 0; i < 2; i++) {
        const char *name = i ? "phase after precision refusal" :
                               "phase after snapshot refusal";

        check = (PrimCheck) { .result = SGI_RENDER_UNSUPPORTED };
        EXPECT(name,
               (prim_lines(p, SGI_GL_LINE_STRIP, 2, 0),
                p->attempt = i ? &checked : &plain,
                p->ops = i ? NULL : &refusing_snapshot,
                p->draw.line_stipple_enabled = true,
                p->draw.line_stipple_repeat = 2,
                p->draw.line_stipple_pattern = 0x3c5a,
                p->use_phase = true, p->phase[1] = 7),
               SGI_RENDER_DRAW_REFUSED, SGI_RENDER_UNSUPPORTED,
               i ? SGI_RENDER_REFUSAL_PRECISION : SGI_RENDER_REFUSAL_SNAPSHOT,
               1, 0);
        if (p->phase[1] != 7) {
            g_test_message("%s: phase %u, expected 7", name, p->phase[1]);
            failures++;
        }
    }
    /* A provider refusing during its commit has failed, not refused. */
    EXPECT("commit refusal", p->ops = &refusing_commit,
           SGI_RENDER_DRAW_FAILED, SGI_RENDER_RESOURCE_ERROR,
           SGI_RENDER_REFUSAL_NONE, 1, 1);
    /* None of the refusals above latched: the renderer still draws. */
    EXPECT("ok after refusals", , SGI_RENDER_DRAW_OK, SGI_RENDER_OK,
           SGI_RENDER_REFUSAL_NONE, 1, 1);
    saved_read_pixels = epoxy_glReadPixels;
    epoxy_glReadPixels = fail_colour_read_pixels;
    EXPECT("host failure", , SGI_RENDER_DRAW_FAILED, SGI_RENDER_HOST_ERROR,
           SGI_RENDER_REFUSAL_NONE, 1, 0);
    epoxy_glReadPixels = saved_read_pixels;
    sgi_renderer_free(renderer);

    /* A host failure in the line probe is a failure, not a refusal. */
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    saved_renderbuffer_storage = epoxy_glRenderbufferStorage;
    epoxy_glRenderbufferStorage = fail_stencil_storage;
    EXPECT("line probe host failure",
           (prim_lines(p, SGI_GL_LINES, 1, 0), p->attempt = &plain),
           SGI_RENDER_DRAW_FAILED, SGI_RENDER_HOST_ERROR,
           SGI_RENDER_REFUSAL_NONE, 0, 0);
    epoxy_glRenderbufferStorage = saved_renderbuffer_storage;
#undef REFUSED
#undef EXPECT
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/* Every reason has its own name and the class its row belongs to. */
static void test_refusal_names(void)
{
    g_autoptr(GHashTable) names = g_hash_table_new(g_str_hash, g_str_equal);

    for (unsigned r = 0; r < SGI_RENDER_REFUSAL__MAX; r++) {
        const char *name = sgi_render_refusal_name(r);

        g_assert_nonnull(name);
        g_assert_true(g_hash_table_add(names, (gpointer)name));
    }
    g_assert_cmpstr(sgi_render_refusal_name(SGI_RENDER_REFUSAL_FOG), ==,
                    "fog");
    g_assert_cmpstr(sgi_render_refusal_name(SGI_RENDER_REFUSAL__MAX), ==,
                    "unknown");
    g_assert_cmpint(sgi_render_refusal_class(SGI_RENDER_REFUSAL_FOG), ==,
                    SGI_RENDER_REFUSAL_CLASS_STATE);
    g_assert_cmpint(sgi_render_refusal_class(SGI_RENDER_REFUSAL_HOST_VIEWPORT),
                    ==, SGI_RENDER_REFUSAL_CLASS_HOST_LIMIT);
    g_assert_cmpint(sgi_render_refusal_class(SGI_RENDER_REFUSAL_HOST_PROBE),
                    ==, SGI_RENDER_REFUSAL_CLASS_HOST_LIMIT);
    g_assert_cmpint(sgi_render_refusal_class(SGI_RENDER_REFUSAL_PRECISION),
                    ==, SGI_RENDER_REFUSAL_CLASS_PRECISION);
}

/*
 * With the mixed-frame flag, a draw outside the proven set is refused
 * before any snapshot, and one inside it draws exactly what the scalar
 * path draws. Without the flag the same refused draws are accepted, so the
 * flag alone refuses them.
 */
static void test_mixed_frame_set(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const SGIRenderAttemptOptions mixed = { .mixed_frame = true };
    static const SGIRenderAttemptOptions without_flag = { 0 };
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
    for (unsigned k = 0; ; k++) {
        g_autofree PrimFixture *scalar = g_new(PrimFixture, 1);
        g_autofree PrimFixture *mesa = g_new(PrimFixture, 1);
        const char *name = NULL;
        bool proven = true, plain = true;

        switch (k) {
        case 0:
            name = "flat fan";
            prim_fan(p, 4);
            break;
        case 1:
            name = "smooth fan with scissor on a top-origin resource";
            prim_fan(p, 4);
            p->state.shade_model = SGI_GL_SMOOTH;
            p->top_origin = true;
            p->state.scissor_test = 1;
            memcpy(p->state.scissor, (int32_t[4]) { 3, 2, 9, 10 },
                   sizeof(p->state.scissor));
            break;
        case 2:
            name = "5-bit fan";
            prim_fan(p, 3);
            p->bits = 5;
            break;
        case 3:
            name = "3-bit fan";
            prim_fan(p, 3);
            p->bits = 3;
            proven = false;
            break;
        case 4:
            name = "points of size 4";
            prim_case_init(p, SGI_GL_POINTS, 4);
            p->draw.point_size = 4;
            prim_vertex_n(p, 4.5f, 5.5f);
            prim_vertex_n(p, 10.25f, 11.75f);
            break;
        case 5:
            name = "points of size 5";
            prim_case_init(p, SGI_GL_POINTS, 4);
            p->draw.point_size = 5;
            prim_vertex_n(p, 4.5f, 5.5f);
            proven = false;
            break;
        case 6:
            name = "points in an offset viewport";
            prim_case_init(p, SGI_GL_POINTS, 4);
            memcpy(p->state.viewport, (int32_t[4]) { 2, 1, 12, 13 },
                   sizeof(p->state.viewport));
            prim_vertex_n(p, 4.5f, 5.5f);
            proven = false;
            break;
        case 7:
            name = "lines of width 6";
            prim_lines(p, SGI_GL_LINES, 6, 0);
            break;
        case 8:
            name = "lines of width 7";
            prim_lines(p, SGI_GL_LINES, 7, 0);
            proven = false;
            break;
        case 9:
        case 10:
            name = k == 9 ? "blended triangle" :
                   "blended triangle with a scissor";
            prim_case_init(p, SGI_GL_TRIANGLES, 4);
            p->state.blend = 1;
            p->state.blend_src = SGI_GL_SRC_ALPHA;
            p->state.blend_dst = SGI_GL_ONE_MINUS_SRC_ALPHA;
            p->state.blend_equation = SGI_GL_FUNC_ADD;
            prim_vertex_n(p, 1, 1.25f);
            prim_vertex_n(p, 14.5f, 2);
            prim_vertex_n(p, 7, 13.75f);
            if (k == 10) {
                p->state.scissor_test = 1;
                memcpy(p->state.scissor, (int32_t[4]) { 0, 0, 16, 16 },
                       sizeof(p->state.scissor));
                proven = false;
            }
            break;
        case 11:
            name = "textured triangle";
            prim_case_init(p, SGI_GL_TRIANGLES, 4);
            prim_texture(p, 2, SGI_GL_NEAREST, SGI_GL_REPEAT, SGI_GL_REPEAT,
                         SGI_GL_REPLACE);
            break;
        case 12:
            name = "textured triangle with a 32-texel image";
            prim_case_init(p, SGI_GL_TRIANGLES, 4);
            prim_texture(p, 2, SGI_GL_NEAREST, SGI_GL_REPEAT, SGI_GL_REPEAT,
                         SGI_GL_REPLACE);
            /* Refused before any snapshot, so its texels are never read. */
            p->image.width = 32;
            proven = false;
            plain = false;
            break;
        }
        if (!name) {
            break;
        }
        cases++;
        p->attempt = &mixed;
        prim_destination(scalar, p);
        prim_destination(mesa, p);
        prim_scalar_draw(p, scalar);
        prim_mesa_draw(renderer, p, mesa, &err);
        g_clear_pointer(&err, error_free);
        if (!proven) {
            if (p->attempt_result != SGI_RENDER_DRAW_REFUSED ||
                p->outcome.reason != SGI_RENDER_REFUSAL_MIXED_FRAME ||
                mesa->snapshots) {
                g_test_message("%s: result %d reason %s after %u snapshots",
                               name, p->attempt_result,
                               sgi_render_refusal_name(p->outcome.reason),
                               mesa->snapshots);
                failures++;
            }
            if (!plain) {
                continue;
            }
            p->attempt = &without_flag;
            prim_destination(mesa, p);
            prim_mesa_draw(renderer, p, mesa, &err);
            g_clear_pointer(&err, error_free);
            if (p->attempt_result != SGI_RENDER_DRAW_OK) {
                g_test_message("%s: not accepted without the flag: %d", name,
                               p->attempt_result);
                failures++;
            }
            continue;
        }
        if (p->attempt_result != SGI_RENDER_DRAW_OK) {
            g_test_message("%s: result %d reason %s", name,
                           p->attempt_result,
                           sgi_render_refusal_name(p->outcome.reason));
            failures++;
        } else if (prim_report(name, scalar, mesa)) {
            failures++;
        }
    }
    g_test_message("mixed frame: %u of %u cases wrong", failures, cases);
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * The renderer records the driver it runs on. The tests run on the
 * reference llvmpipe build, the only driver the mixed-frame set is proven
 * on; a different build there must fail loudly, since every proof below
 * would then be about a driver the set does not name. Off the reference,
 * the mixed-frame flag refuses every draw and CLEAR before any snapshot,
 * and the same work without the flag still draws.
 */
static void test_driver_identity(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const SGIRenderAttemptOptions mixed = { .mixed_frame = true };
    static const SGIRenderAttemptOptions plain = { 0 };
    static const SGIClipRect full[] = { { 0, 0, 15, 15 } };
    PrimCase *p = g_new(PrimCase, 1);
    g_autofree PrimFixture *f = g_new(PrimFixture, 1);
    const SGIRendererIdentity *id;
    SGIRenderer *renderer;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    id = sgi_renderer_identity(renderer);
    g_test_message("driver: vendor %s renderer %s version %s", id->vendor,
                   id->renderer, id->version);
    if (!id->reference) {
        g_error("not the reference llvmpipe build (Mesa %s, LLVM %s, %u-bit "
                "vectors): renderer %s, version %s",
                SGI_RENDERER_REFERENCE_MESA, SGI_RENDERER_REFERENCE_LLVM,
                SGI_RENDERER_REFERENCE_VECTOR_BITS, id->renderer,
                id->version);
    }
    g_assert_cmpstr(id->vendor, ==, "Mesa");
    g_assert_cmpstr(id->mesa_version, ==, SGI_RENDERER_REFERENCE_MESA);

    sgi_renderer_test_set_reference(renderer, false);
    prim_fan(p, 4);
    p->attempt = &mixed;
    prim_destination(f, p);
    prim_mesa_draw(renderer, p, f, &err);
    g_clear_pointer(&err, error_free);
    g_assert_cmpint(p->attempt_result, ==, SGI_RENDER_DRAW_REFUSED);
    g_assert_cmpint(p->outcome.reason, ==, SGI_RENDER_REFUSAL_HOST_DRIVER);
    g_assert_cmpint(sgi_render_refusal_class(p->outcome.reason), ==,
                    SGI_RENDER_REFUSAL_CLASS_HOST_LIMIT);
    g_assert_cmpuint(f->snapshots, ==, 0);
    p->attempt = &plain;
    prim_destination(f, p);
    prim_mesa_draw(renderer, p, f, &err);
    g_assert_null(err);
    g_assert_cmpint(p->attempt_result, ==, SGI_RENDER_DRAW_OK);
    {
        SGIRenderResource res[2];
        SGIRasterRules rules;
        SGIRasterTarget target = {
            .opaque = f, .resources = res, .resource_count = 1,
            .resource_ops = &prim_ops,
        };
        SGIRasterOp op = { 0 };
        SGIRenderAttemptOutcome outcome;

        prim_case_init(p, SGI_GL_TRIANGLES, 4);
        prim_rules(p, &rules);
        prim_resources(p, res);
        prim_destination(f, p);
        prim_clear_op(&op, full, 1, NULL);
        op.clear_buffers = SGI_GL_COLOR_BUFFER_BIT;
        op.clear_colour_mask = SGI_GL_COLOUR_MASK_ALL;
        g_assert_cmpint(sgi_render_raster_attempt(renderer, &rules, &target,
                                                  &op, &mixed, &outcome,
                                                  &err), ==,
                        SGI_RENDER_DRAW_REFUSED);
        g_clear_pointer(&err, error_free);
        g_assert_cmpint(outcome.reason, ==, SGI_RENDER_REFUSAL_HOST_DRIVER);
        g_assert_cmpuint(f->snapshots, ==, 0);
    }
    sgi_renderer_free(renderer);
    g_free(p);
}

/* A draw's signature as prim_mesa_draw() would present it. */
static void prim_signature(PrimCase *p, SGIRenderSignature *signature)
{
    SGIRenderResource res[2];
    SGIRasterRules rules;
    SGIRasterTarget target = {
        .resources = res, .resource_count = p->state.texture_2d ? 2 : 1,
        .resource_ops = &prim_ops,
        .texture = p->state.texture_2d ? &p->image : NULL,
    };

    prim_rules(p, &rules);
    prim_resources(p, res);
    if (p->rules_case == 1) {
        rules.subpixel_bits = 4;
    }
    sgi_render_draw_signature(&rules, &target, &p->draw, signature);
}

/*
 * Signatures are stable text: the same draw always gives the same one,
 * colours, vertex positions and state outside version 1 do not change it,
 * and each version 1 field does. The in-set tag follows the mixed-frame
 * check.
 */
static void test_draw_signature(void)
{
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderSignature base, other;

    prim_fan(p, 4);
    prim_signature(p, &base);
    g_test_message("fan: %s", base.text);
    g_assert_cmpuint(base.version, ==, 2);
    g_assert_true(g_str_has_prefix(base.text, "v2 polygon "));
    g_assert_true(base.in_set);
    prim_signature(p, &other);
    g_assert_cmpstr(other.text, ==, base.text);

#define SAME(change) do { \
        prim_fan(p, 4); \
        change; \
        prim_signature(p, &other); \
        g_assert_cmpstr(other.text, ==, base.text); \
    } while (0)
#define DIFFERENT(change) do { \
        prim_fan(p, 4); \
        change; \
        prim_signature(p, &other); \
        g_assert_cmpstr(other.text, !=, base.text); \
    } while (0)
    SAME(p->vertices[0][SGI_GL_V_COLOR] = 0.125f);
    SAME(p->vertices[1][SGI_GL_V_POSITION] += 0.25f);
    SAME(p->state.colour_mask = SGI_GL_COLOUR_MASK_RED);
    SAME(p->state.depth_test = 1);
    SAME(p->state.dither = 1);
    DIFFERENT(p->draw.mode = SGI_GL_POINTS);
    DIFFERENT(p->draw.polygon_stipple_enabled = true);
    DIFFERENT(p->state.shade_model = SGI_GL_SMOOTH);
    DIFFERENT((p->state.blend = 1, p->state.blend_src = SGI_GL_SRC_ALPHA));
    DIFFERENT(prim_texture(p, 2, SGI_GL_NEAREST, SGI_GL_REPEAT,
                           SGI_GL_REPEAT, SGI_GL_REPLACE));
    DIFFERENT(p->smooth_rules = true);
    DIFFERENT(p->bits = 5);
    DIFFERENT(p->alpha_storage = 4);
    DIFFERENT(p->top_origin = true);
    DIFFERENT(memcpy(p->state.viewport, (int32_t[4]) { 1, 0, 16, 16 },
                     sizeof(p->state.viewport)));
    DIFFERENT(p->state.scissor_test = 1);
    DIFFERENT(p->state.drawable.clip.r[0].x1 = 7);
    DIFFERENT(p->state.modelview[0] = 0.5f);
    DIFFERENT(p->vertices[0][SGI_GL_V_POSITION + 2] = -1);
#undef SAME
#undef DIFFERENT

    /* Rules are named, so renumbering an enum cannot change the text. */
    g_assert_nonnull(strstr(base.text, " rules=8/half-away/"
                            "top-left-bottom-origin/double/0 "));
    /* Rules the scalar path implements but Mesa refuses are not in-set. */
    prim_fan(p, 4);
    p->rules_case = 1;
    prim_signature(p, &other);
    g_assert_false(other.in_set);

    /* Text that would not fit is one overflow signature, never cut. */
    sgi_render_test_signature_limit(64);
    prim_fan(p, 4);
    prim_signature(p, &other);
    g_assert_cmpstr(other.text, ==, "v2 overflow");
    sgi_render_test_signature_limit(0);
    /* The largest field values still fit the real bound. */
    prim_case_init(p, SGI_GL_POINTS, 4);
    prim_texture(p, 2, SGI_GL_NEAREST, SGI_GL_REPEAT, SGI_GL_REPEAT,
                 SGI_GL_REPLACE);
    p->draw.point_size = FLT_MAX;
    p->draw.line_stipple_enabled = true;
    p->draw.polygon_stipple_enabled = true;
    p->state.blend = UINT32_MAX;
    p->state.blend_equation = UINT32_MAX;
    p->state.blend_src = UINT32_MAX;
    p->state.blend_dst = UINT32_MAX;
    p->state.texture_env_mode = UINT32_MAX;
    p->image.min_filter = p->image.mag_filter = UINT32_MAX;
    p->image.wrap_s = p->image.wrap_t = UINT32_MAX;
    p->image.width = p->image.height = UINT32_MAX;
    p->state.shade_model = UINT32_MAX;
    p->top_origin = true;
    prim_vertex_n(p, 1, 1);
    prim_signature(p, &other);
    g_test_message("largest: %s (%zu)", other.text, strlen(other.text));
    g_assert_cmpstr(other.text, !=, "v2 overflow");

    /*
     * Texture state the scalar path draws or drops by rules the host
     * program lacks is out of the set: the 1D enable, a one-dimensional
     * image under the 2D enable, and a shadow comparison.
     */
    prim_case_init(p, SGI_GL_TRIANGLES, 4);
    prim_texture(p, 2, SGI_GL_NEAREST, SGI_GL_REPEAT, SGI_GL_REPEAT,
                 SGI_GL_REPLACE);
    prim_signature(p, &other);
    g_assert_true(other.in_set);
    p->state.texture_1d = 1;
    prim_signature(p, &other);
    g_assert_false(other.in_set);
    p->state.texture_1d = 0;
    p->image.one_dimensional = true;
    prim_signature(p, &other);
    g_assert_false(other.in_set);
    p->image.one_dimensional = false;
    p->image.shadow_compare = SGI_GL_TEXTURE_LEQUAL_R_SGIX;
    prim_signature(p, &other);
    g_assert_false(other.in_set);
    /* So is a polygon the scalar path smooths. */
    prim_fan(p, 4);
    p->smooth_rules = true;
    p->draw.polygon_smooth = true;
    prim_signature(p, &other);
    g_assert_false(other.in_set);
    p->smooth_rules = false;
    prim_signature(p, &other);
    g_assert_true(other.in_set);

    /* Out of the set: a constant-colour blend Mesa refuses outright. */
    prim_fan(p, 4);
    p->state.blend = 1;
    p->state.blend_equation = SGI_GL_FUNC_ADD;
    p->state.blend_src = SGI_GL_CONSTANT_COLOR;
    prim_signature(p, &other);
    g_assert_false(other.in_set);
    /* A blended triangle under a scissor, accepted but not proven. */
    prim_case_init(p, SGI_GL_TRIANGLES, 4);
    p->state.blend = 1;
    p->state.blend_equation = SGI_GL_FUNC_ADD;
    p->state.blend_src = SGI_GL_SRC_ALPHA;
    p->state.blend_dst = SGI_GL_ONE_MINUS_SRC_ALPHA;
    prim_vertex_n(p, 1, 1.25f);
    prim_vertex_n(p, 14.5f, 2);
    prim_vertex_n(p, 7, 13.75f);
    prim_signature(p, &other);
    g_assert_true(other.in_set);
    p->state.scissor_test = 1;
    memcpy(p->state.scissor, (int32_t[4]) { 0, 0, 16, 16 },
           sizeof(p->state.scissor));
    prim_signature(p, &other);
    g_assert_false(other.in_set);
    g_free(p);
}

/*
 * Decoded CLEAR through the attempt: it commits, refuses unsupported
 * planes, a family snapshot or precision refusal before any commit, and
 * under the mixed-frame flag widths scalar-match-clear does not prove.
 */
static void test_attempt_clear(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const SGIClipRect full[] = { { 0, 0, 15, 15 } };
    static const SGIRenderResourceOps refusing_snapshot = {
        .snapshot = prim_refusing_snapshot,
        .commit = prim_commit,
    };
    PrimCheck check = { 0 };
    const SGIRenderAttemptOptions checked = {
        .check_staged = prim_check_staged, .opaque = &check,
    };
    static const SGIRenderAttemptOptions mixed = { .mixed_frame = true };
    static const struct {
        const char *name;
        unsigned bits, index_bits;
        bool refusing_snapshot, check, mixed;
        SGIRenderResult check_result;
        SGIRenderDrawResult result;
        SGIRenderRefusal reason;
        unsigned snapshots, commits;
    } cases[] = {
        { "ok", 8, 0, false, false, false, SGI_RENDER_OK,
          SGI_RENDER_DRAW_OK, SGI_RENDER_REFUSAL_NONE, 1, 1 },
        { "colour index", 8, 8, false, false, false, SGI_RENDER_OK,
          SGI_RENDER_DRAW_REFUSED, SGI_RENDER_REFUSAL_RASTER_OP, 0, 0 },
        { "snapshot refusal", 8, 0, true, false, false, SGI_RENDER_OK,
          SGI_RENDER_DRAW_REFUSED, SGI_RENDER_REFUSAL_SNAPSHOT, 1, 0 },
        { "precision refusal", 8, 0, false, true, false,
          SGI_RENDER_UNSUPPORTED, SGI_RENDER_DRAW_REFUSED,
          SGI_RENDER_REFUSAL_PRECISION, 1, 0 },
        { "precision accepted", 8, 0, false, true, false, SGI_RENDER_OK,
          SGI_RENDER_DRAW_OK, SGI_RENDER_REFUSAL_NONE, 1, 1 },
        { "mixed 10-bit", 10, 0, false, false, true, SGI_RENDER_OK,
          SGI_RENDER_DRAW_OK, SGI_RENDER_REFUSAL_NONE, 1, 1 },
        { "mixed 5-bit", 5, 0, false, false, true, SGI_RENDER_OK,
          SGI_RENDER_DRAW_REFUSED, SGI_RENDER_REFUSAL_MIXED_FRAME, 0, 0 },
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
        g_autofree PrimFixture *f = g_new(PrimFixture, 1);
        SGIRenderResource res[2];
        SGIRasterRules rules;
        SGIRasterTarget target = {
            .opaque = f, .resources = res, .resource_count = 1,
            .resource_ops = &prim_ops,
        };
        const SGIRenderAttemptOptions *opts = NULL;
        SGIRasterOp op = { 0 };
        SGIRenderAttemptOutcome outcome;
        SGIRenderDrawResult result;

        prim_case_init(p, SGI_GL_TRIANGLES, 4);
        p->bits = cases[i].bits;
        prim_rules(p, &rules);
        prim_resources(p, res);
        prim_destination(f, p);
        prim_clear_op(&op, full, 1, NULL);
        op.clear_buffers = SGI_GL_COLOR_BUFFER_BIT;
        op.clear_colour_mask = SGI_GL_COLOUR_MASK_ALL;
        op.clear_rgba[0] = 0.5f;
        op.index_bits = cases[i].index_bits;
        check = (PrimCheck) { .result = cases[i].check_result };
        if (cases[i].refusing_snapshot) {
            target.resource_ops = &refusing_snapshot;
        }
        if (cases[i].check) {
            opts = &checked;
        } else if (cases[i].mixed) {
            opts = &mixed;
        }
        result = sgi_render_raster_attempt(renderer, &rules, &target, &op,
                                           opts, &outcome, &err);
        if (result != cases[i].result || outcome.reason != cases[i].reason ||
            f->snapshots != cases[i].snapshots ||
            f->commits != cases[i].commits ||
            (cases[i].check && check.calls != 1)) {
            const char *why = err ? error_get_pretty(err) : "(no error)";

            g_test_message("%s: result %d reason %s, %u snapshots, %u "
                           "commits, %u checks: %s", cases[i].name, result,
                           sgi_render_refusal_name(outcome.reason),
                           f->snapshots, f->commits, check.calls, why);
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
 * One shadow-compared draw: capture native into private, draw the scalar
 * path into native, then compare Mesa against it. phase_offset shifts the
 * phase the shadow is told the draw started from.
 */
static SGIRenderShadowResult prim_shadow_with(SGIRenderShadow *shadow,
                                              PrimCase *p,
                                              PrimFixture *native,
                                              PrimFixture *private,
                                              uint32_t phase_offset,
                                              unsigned flags,
                                              SGIRenderShadowOutcome *out);

/* With masks, the scalar store callbacks' written bytes are passed. */
#define PRIM_SHADOW_MASKS 1
/* A stray scalar-only side effect at pixel (1,14), as a tag write. */
#define PRIM_SHADOW_STRAY 2

static SGIRenderShadowResult prim_shadow(SGIRenderShadow *shadow,
                                         PrimCase *p, PrimFixture *native,
                                         PrimFixture *private,
                                         uint32_t phase_offset,
                                         SGIRenderShadowOutcome *out)
{
    return prim_shadow_with(shadow, p, native, private, phase_offset,
                            PRIM_SHADOW_MASKS, out);
}

static SGIRenderShadowResult prim_shadow_with(SGIRenderShadow *shadow,
                                              PrimCase *p,
                                              PrimFixture *native,
                                              PrimFixture *private,
                                              uint32_t phase_offset,
                                              unsigned flags,
                                              SGIRenderShadowOutcome *out)
{
    SGIRenderResource res[2];
    SGIRasterRules rules;
    SGIRasterTarget native_target = {
        .opaque = native, .resources = res,
        .resource_count = p->state.texture_2d ? 2 : 1,
        .resource_ops = &prim_ops,
        .texture = p->state.texture_2d ? &p->image : NULL,
    };
    SGIRasterTarget private_target = native_target;
    g_autofree PrimFixture *after = g_new(PrimFixture, 1);
    const uint8_t *masks[2] = { NULL, NULL };
    uint32_t before = p->phase[0];
    Error *err = NULL;
    SGIRenderShadowResult result;

    private_target.opaque = private;
    prim_rules(p, &rules);
    prim_resources(p, res);
    prim_destination(native, p);
    memset(private, 0, sizeof(*private));
    private->components = p->components;
    g_assert_cmpint(sgi_render_shadow_capture(shadow, &native_target,
                                              &private_target, &err), ==,
                    SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpmem(private->rgba, sizeof(private->rgba), native->rgba,
                    sizeof(native->rgba));
    prim_scalar_draw(p, native);
    if (flags & PRIM_SHADOW_STRAY) {
        native->written[14][1] = 1;
    }
    memcpy(after, native, sizeof(*after));
    masks[0] = &native->written[0][0];
    result = sgi_render_shadow_draw(shadow, &rules, &native_target,
                                    &private_target, &p->draw,
                                    before + phase_offset,
                                    flags & PRIM_SHADOW_MASKS ? masks : NULL,
                                    out);
    /* The shadow never writes native storage or the scalar phase. */
    g_assert_cmpmem(native->rgba, sizeof(native->rgba), after->rgba,
                    sizeof(after->rgba));
    if (p->use_phase) {
        g_assert_true(p->draw.line_phase == &p->phase[0]);
    }
    return result;
}

/*
 * Shadow comparison: a draw both paths render alike matches; one Mesa
 * draws differently, here smooth triangles moved one subpixel, mismatches
 * at its first differing pixel; state Mesa refuses is a refusal; a Mesa
 * failure is counted without latching and the next draw gets a new
 * renderer; and a borrowed stipple phase is compared on its own copy.
 */
static void test_shadow_comparison(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    PrimCase *p = g_new(PrimCase, 1);
    g_autofree PrimFixture *native = g_new(PrimFixture, 1);
    g_autofree PrimFixture *private = g_new(PrimFixture, 1);
    SGIRenderShadowOutcome out;
    SGIRenderShadowCounts counts;
    SGIRenderShadow *shadow;
    SGIRenderer *before;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    shadow = sgi_render_shadow_new();
    g_assert_null(sgi_render_shadow_disabled(shadow));

    prim_fan(p, 4);
    g_assert_cmpint(prim_shadow(shadow, p, native, private, 0, &out), ==,
                    SGI_RENDER_SHADOW_MATCH);
    g_assert_true(out.signature.in_set);
    counts = sgi_render_shadow_counts(shadow, out.signature.text);
    g_assert_cmpuint(counts.matches, ==, 1);

    /* Smooth, once to pass the probe, then moved one subpixel. */
    prim_fan(p, 4);
    p->state.shade_model = SGI_GL_SMOOTH;
    g_assert_cmpint(prim_shadow(shadow, p, native, private, 0, &out), ==,
                    SGI_RENDER_SHADOW_MATCH);
    sgi_render_test_window_shift(1);
    prim_fan(p, 4);
    p->state.shade_model = SGI_GL_SMOOTH;
    g_assert_cmpint(prim_shadow(shadow, p, native, private, 0, &out), ==,
                    SGI_RENDER_SHADOW_MISMATCH);
    sgi_render_test_window_shift(0);
    g_test_message("mismatch at resource %zu (%u,%u): scalar %u,%u,%u,%u "
                   "mesa %u,%u,%u,%u", out.resource, out.x, out.y,
                   out.scalar[0], out.scalar[1], out.scalar[2],
                   out.scalar[3], out.mesa[0], out.mesa[1], out.mesa[2],
                   out.mesa[3]);
    g_assert_cmpuint(out.resource, ==, 0);
    g_assert_cmpuint(out.components, ==, 4);
    g_assert_false(out.phase_differs);
    g_assert_cmpmem(out.scalar, sizeof(out.scalar),
                    native->rgba[out.y][out.x], sizeof(out.scalar));
    g_assert_cmpmem(out.mesa, sizeof(out.mesa), private->rgba[out.y][out.x],
                    sizeof(out.mesa));
    counts = sgi_render_shadow_counts(shadow, out.signature.text);
    g_assert_cmpuint(counts.matches, ==, 1);
    g_assert_cmpuint(counts.mismatches, ==, 1);

    /* A constant-colour blend Mesa refuses outright. */
    prim_case_init(p, SGI_GL_TRIANGLES, 4);
    p->state.blend = 1;
    p->state.blend_equation = SGI_GL_FUNC_ADD;
    p->state.blend_src = SGI_GL_CONSTANT_COLOR;
    p->state.blend_dst = 0;
    prim_vertex_n(p, 1, 1.25f);
    prim_vertex_n(p, 14.5f, 2);
    prim_vertex_n(p, 7, 13.75f);
    g_assert_cmpint(prim_shadow(shadow, p, native, private, 0, &out), ==,
                    SGI_RENDER_SHADOW_REFUSED);
    g_assert_cmpint(out.refusal, ==, SGI_RENDER_REFUSAL_BLEND);
    g_assert_false(out.signature.in_set);

    /* A failed Mesa draw is counted, never latched, and not repeated. */
    before = sgi_render_shadow_test_renderer(shadow);
    sgi_renderer_test_fail_draw(before, 1);
    prim_fan(p, 4);
    g_assert_cmpint(prim_shadow(shadow, p, native, private, 0, &out), ==,
                    SGI_RENDER_SHADOW_FAILED);
    g_assert_cmpint(out.status, ==, SGI_RENDER_HOST_ERROR);
    g_assert_null(sgi_render_shadow_disabled(shadow));
    g_assert_nonnull(sgi_render_shadow_test_renderer(shadow));
    prim_fan(p, 4);
    g_assert_cmpint(prim_shadow(shadow, p, native, private, 0, &out), ==,
                    SGI_RENDER_SHADOW_MATCH);
    counts = sgi_render_shadow_counts(shadow, out.signature.text);
    g_assert_cmpuint(counts.matches, ==, 2);
    g_assert_cmpuint(counts.failures, ==, 1);

    /*
     * A stippled strip with no visible pixel still counts its phase, so
     * the pixels agree and only the phase can differ.
     */
    for (unsigned offset = 0; offset < 2; offset++) {
        prim_case_init(p, SGI_GL_LINE_STRIP, 4);
        p->draw.line_stipple_enabled = true;
        p->draw.line_stipple_repeat = 2;
        p->draw.line_stipple_pattern = 0x3c5a;
        p->state.drawable.clip.n = 0;
        p->use_phase = true;
        p->phase[0] = 5;
        prim_segment(p, 1.25f, 2.5f, 14.5f, 12.75f);
        prim_vertex_window(p, 3.5f, 13.25f);
        g_assert_cmpint(prim_shadow(shadow, p, native, private, offset,
                                    &out), ==,
                        offset ? SGI_RENDER_SHADOW_MISMATCH :
                                 SGI_RENDER_SHADOW_MATCH);
        g_assert_cmpuint(p->phase[0], !=, 5);
        g_assert_cmpint(out.phase_differs, ==, offset != 0);
        if (offset) {
            g_assert_cmpuint(out.scalar_phase, ==, p->phase[0]);
            g_assert_cmpuint(out.mesa_phase, !=, p->phase[0]);
        }
    }
    counts = sgi_render_shadow_totals(shadow);
    g_assert_cmpuint(counts.matches, ==, 4);
    g_assert_cmpuint(counts.mismatches, ==, 2);
    g_assert_cmpuint(counts.refusals, ==, 1);
    g_assert_cmpuint(counts.failures, ==, 1);
    g_assert_cmpuint(counts.without_coverage, ==, 0);

    /* Without scalar masks only contents are compared, and counted apart. */
    prim_fan(p, 4);
    g_assert_cmpint(prim_shadow_with(shadow, p, native, private, 0, 0,
                                     &out), ==, SGI_RENDER_SHADOW_MATCH);
    g_assert_false(out.coverage_compared);
    g_assert_cmpuint(sgi_render_shadow_totals(shadow).without_coverage, ==,
                     1);

    /* A private target describing other rows does not pair. */
    {
        SGIRenderResource res[2], other[2];
        SGIRasterRules rules;
        SGIRasterTarget native_target = {
            .opaque = native, .resources = res, .resource_count = 1,
            .resource_ops = &prim_ops,
        };
        SGIRasterTarget private_target = {
            .opaque = private, .resources = other, .resource_count = 1,
            .resource_ops = &prim_ops,
        };

        before = sgi_render_shadow_test_renderer(shadow);
        prim_fan(p, 4);
        prim_rules(p, &rules);
        prim_resources(p, res);
        prim_resources(p, other);
        other[0].bottom_origin = false;
        g_assert_cmpint(sgi_render_shadow_draw(shadow, &rules, &native_target,
                                               &private_target, &p->draw, 0,
                                               NULL, &out), ==,
                        SGI_RENDER_SHADOW_FAILED);
        g_assert_cmpint(out.status, ==, SGI_RENDER_RESOURCE_ERROR);
        g_assert_true(sgi_render_shadow_test_renderer(shadow) == before);
    }
    sgi_render_shadow_free(shadow);
    g_free(p);
}

/*
 * Write coverage: a pixel one path writes and the other does not is a
 * mismatch even when both leave the same value there. Here every pixel
 * already holds the draw's colour, so contents always agree: smooth
 * triangles of one colour moved one subpixel change only which pixels
 * Mesa writes, as an edge-ownership swap between two triangles of one
 * colour would; and a scalar store with a side effect Mesa lacks, as a
 * tag write, shows only in the scalar mask.
 */
static void test_shadow_coverage(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    PrimCase *p = g_new(PrimCase, 1);
    g_autofree PrimFixture *native = g_new(PrimFixture, 1);
    g_autofree PrimFixture *private = g_new(PrimFixture, 1);
    SGIRenderShadowOutcome out;
    SGIRenderShadow *shadow;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    shadow = sgi_render_shadow_new();
    for (unsigned k = 0; k < 4; k++) {
        prim_case_init(p, SGI_GL_TRIANGLE_FAN, 4);
        p->state.shade_model = k ? SGI_GL_SMOOTH : SGI_GL_FLAT;
        /* 0.5 stores as 128 at 8 bits; the destination already holds it. */
        p->dest_value = 129;
        prim_vertex(p, 1, 1.25f, 0.5f, 0.5f, 0.5f, 0.5f);
        prim_vertex(p, 14.5f, 2, 0.5f, 0.5f, 0.5f, 0.5f);
        prim_vertex(p, 13, 13.75f, 0.5f, 0.5f, 0.5f, 0.5f);
        prim_vertex(p, 2.25f, 12, 0.5f, 0.5f, 0.5f, 0.5f);
        if (k == 2) {
            sgi_render_test_window_shift(1);
        }
        prim_shadow_with(shadow, p, native, private, 0,
                         PRIM_SHADOW_MASKS |
                         (k == 3 ? PRIM_SHADOW_STRAY : 0), &out);
        sgi_render_test_window_shift(0);
        g_test_message("case %u: result %d at (%u,%u) scalar %s mesa %s", k,
                       out.result, out.x, out.y,
                       out.scalar_wrote ? "wrote" : "kept",
                       out.mesa_wrote ? "wrote" : "kept");
        g_assert_true(out.coverage_compared);
        if (k < 2) {
            /* Flat, then smooth, which also passes the probe. */
            g_assert_cmpint(out.result, ==, SGI_RENDER_SHADOW_MATCH);
            continue;
        }
        g_assert_cmpint(out.result, ==, SGI_RENDER_SHADOW_MISMATCH);
        g_assert_cmpmem(out.scalar, sizeof(out.scalar), out.mesa,
                        sizeof(out.mesa));
        g_assert_cmpint(out.scalar_wrote, !=, out.mesa_wrote);
        if (k == 3) {
            g_assert_cmpuint(out.x, ==, 1);
            g_assert_cmpuint(out.y, ==, 14);
            g_assert_true(out.scalar_wrote);
        }
    }
    sgi_render_shadow_free(shadow);
    g_free(p);
}

/* Decoded CLEAR in shadow, as draws are. */
static void test_shadow_raster(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const SGIClipRect several[] = {
        { 1, 1, 5, 4 }, { 4, 3, 12, 9 }, { 13, 0, 15, 15 },
    };
    PrimCase *p = g_new(PrimCase, 1);
    g_autofree PrimFixture *native = g_new(PrimFixture, 1);
    g_autofree PrimFixture *private = g_new(PrimFixture, 1);
    SGIRenderShadowOutcome out;
    SGIRenderShadow *shadow;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    shadow = sgi_render_shadow_new();
    for (unsigned k = 0; k < 5; k++) {
        SGIRenderResource res[2];
        SGIRasterRules rules;
        SGIRasterTarget native_target = {
            .opaque = native, .resources = res, .resource_count = 1,
            .resource_ops = &prim_ops,
            .store_rgba = prim_scalar_store, .load_rgba = prim_scalar_load,
        };
        SGIRasterTarget private_target = native_target;
        SGIRasterOp op = { 0 };
        const uint8_t *masks[1] = { &native->written[0][0] };
        prim_case_init(p, SGI_GL_TRIANGLES, 4);
        p->bits = k == 4 ? 5 : 8;
        prim_rules(p, &rules);
        prim_resources(p, res);
        prim_destination(native, p);
        memset(private, 0, sizeof(*private));
        private->components = 4;
        private_target.opaque = private;
        prim_clear_op(&op, several, ARRAY_SIZE(several), NULL);
        op.clear_buffers = SGI_GL_COLOR_BUFFER_BIT;
        op.clear_colour_mask = SGI_GL_COLOUR_MASK_ALL;
        op.clear_rgba[0] = 0.25f;
        op.clear_rgba[3] = 1;
        op.index_bits = k == 1 ? 8 : 0;
        g_assert_cmpint(sgi_render_shadow_capture(shadow, &native_target,
                                                  &private_target, NULL), ==,
                        SGI_RENDER_OK);
        if (k != 1) {
            sgi_soft_raster(&rules, &native_target, &op);
        }
        if (k == 3) {
            native->written[14][1] = 1;
        }
        sgi_render_shadow_raster(shadow, &rules, &native_target,
                                 &private_target, &op,
                                 k == 2 ? NULL : masks, &out);
        g_test_message("clear %u: %s result %d", k, out.signature.text,
                       out.result);
        g_assert_true(g_str_has_prefix(out.signature.text, "v2 "));
        switch (k) {
        case 0:
        case 4:
            g_assert_cmpint(out.result, ==, SGI_RENDER_SHADOW_MATCH);
            g_assert_true(out.coverage_compared);
            g_assert_cmpint(out.signature.in_set, ==, k == 0);
            break;
        case 1:
            g_assert_cmpint(out.result, ==, SGI_RENDER_SHADOW_REFUSED);
            g_assert_cmpint(out.refusal, ==, SGI_RENDER_REFUSAL_RASTER_OP);
            g_assert_false(out.signature.in_set);
            break;
        case 2:
            g_assert_cmpint(out.result, ==, SGI_RENDER_SHADOW_MATCH);
            g_assert_false(out.coverage_compared);
            break;
        case 3:
            g_assert_cmpint(out.result, ==, SGI_RENDER_SHADOW_MISMATCH);
            g_assert_cmpuint(out.x, ==, 1);
            g_assert_cmpuint(out.y, ==, 14);
            g_assert_true(out.scalar_wrote);
            g_assert_false(out.mesa_wrote);
            break;
        }
    }
    sgi_render_shadow_free(shadow);
    g_free(p);
}

/*
 * Colour-index comparisons: a 16 by 16 bottom-origin front index resource
 * in the mixed-frame fixture, against sgi_soft_draw() through a scalar
 * store_index that merges under the mask as the families do. Each index
 * pixel records whether a fragment reached it.
 */
typedef struct IndexFixture {
    uint32_t index[PRIM_SIZE][PRIM_SIZE];
    uint8_t written[PRIM_SIZE][PRIM_SIZE];
    unsigned bits;
    unsigned snapshots, commits;
    uint32_t effective_mask;
    /* Nonzero: the index commit stops after this many pixels. */
    unsigned fail_after;
} IndexFixture;

static uint32_t index_width_mask(unsigned bits)
{
    return bits >= 32 ? UINT32_MAX : (1u << bits) - 1;
}

static void index_destination(IndexFixture *f, unsigned bits)
{
    memset(f, 0, sizeof(*f));
    f->bits = bits;
    for (unsigned y = 0; y < PRIM_SIZE; y++) {
        for (unsigned x = 0; x < PRIM_SIZE; x++) {
            f->index[y][x] = (x * 0x9e3779b1u + y * 0x85ebca6bu + 0x1234) &
                             index_width_mask(bits);
        }
    }
}

static void index_scalar_store(void *opaque, SGIColourTarget target, int x,
                               int y, uint32_t index, uint32_t mask)
{
    IndexFixture *f = opaque;
    uint32_t m = mask & index_width_mask(f->bits);

    if (target != 7 || x < 0 || x >= PRIM_SIZE || y < 0 || y >= PRIM_SIZE) {
        return;
    }
    f->index[y][x] = (f->index[y][x] & ~m) | (index & m);
    f->written[y][x] = 1;
}

static SGIRenderResult index_snapshot(void *opaque,
                                      const SGIRenderResource *resource,
                                      uint32_t slice,
                                      const SGIRenderRect *rect,
                                      uint32_t *slots, size_t stride_slots,
                                      size_t capacity_slots, Error **errp)
{
    IndexFixture *f = opaque;

    g_assert_cmpint(resource->role, ==, SGI_RENDER_INDEX);
    f->snapshots++;
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            slots[(y - rect->y0) * stride_slots + x - rect->x0] =
                f->index[y][x];
        }
    }
    return SGI_RENDER_OK;
}

static SGIRenderCommitResult index_commit(
    void *opaque, const SGIRenderResource *res, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, const SGIRenderWriteCoverage *writes,
    uint32_t effective_mask, Error **errp)
{
    IndexFixture *f = opaque;
    uint64_t complete = 0;

    f->commits++;
    f->effective_mask = effective_mask;
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            if (f->fail_after && complete == f->fail_after) {
                error_setg(errp, "index commit stopped at pixel %" PRIu64,
                           complete);
                return (SGIRenderCommitResult) {
                    .status = SGI_RENDER_RESOURCE_ERROR,
                    .complete_pixels = complete,
                    .extent_known = true,
                };
            }
            complete++;
            if (!writes->pixel[(y - rect->y0) * writes->stride_bytes +
                               x - rect->x0]) {
                continue;
            }
            /* The final value arrives merged; store it once. */
            f->index[y][x] = slots[(y - rect->y0) * stride_slots +
                                   x - rect->x0];
            f->written[y][x] = 1;
        }
    }
    return (SGIRenderCommitResult) {
        .status = SGI_RENDER_OK,
        .complete_pixels = complete,
        .extent_known = true,
    };
}

/* A colour commit, which an indexed draw must never call. */
static SGIRenderCommitResult index_colour_commit(
    void *opaque, const SGIRenderResource *res, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, const SGIRenderWriteCoverage *writes,
    Error **errp)
{
    g_assert_not_reached();
}

static const SGIRenderResourceOps index_ops = {
    .snapshot = index_snapshot,
    .commit_covered = index_colour_commit,
    .commit_index_covered = index_commit,
};

static SGIRenderResource index_resource(unsigned bits)
{
    return (SGIRenderResource) {
        .backing_id = 5, .backing_generation = 1,
        .view_id = 6, .view_generation = 1,
        .role = SGI_RENDER_INDEX, .target = 7,
        .buffer = SGI_RENDER_FRONT, .eye = SGI_RENDER_MONO,
        .width = PRIM_SIZE, .height = PRIM_SIZE, .depth = 1,
        .bottom_origin = true, .component_count = 1,
        .component = { { SGI_RENDER_SCALAR, SGI_RENDER_UINT, bits } },
    };
}

/*
 * An indexed case: prim_case_init() state in index mode at bits, mask
 * all ones, flat shading.
 */
static void index_case_init(PrimCase *p, uint32_t mode, unsigned bits)
{
    prim_case_init(p, mode, 4);
    p->state.index_bits = bits;
    p->state.index_mask = UINT32_MAX;
}

/* A vertex at window (x, y) carrying index. */
static void index_vertex(PrimCase *p, float x, float y, float index)
{
    prim_vertex_n(p, x, y);
    p->vertices[p->draw.count - 1][SGI_GL_V_INDEX] = index;
}

/* Rules for both paths of an indexed case; true names a dither. */
static bool index_rules_dither;

static void index_rules(const PrimCase *p, SGIRasterRules *rules)
{
    prim_rules(p, rules);
    if (index_rules_dither) {
        rules->dither = SGI_DITHER_BAYER4_TOP_ROWS;
    }
}

static void index_scalar_draw(PrimCase *p, IndexFixture *f)
{
    SGIRasterRules rules;
    SGIRasterTarget target = {
        .opaque = f, .store_index = index_scalar_store,
    };

    index_rules(p, &rules);
    g_assert_cmpint(sgi_soft_draw(&rules, &target, &p->draw), ==,
                    SGI_GL_DRAW_OK);
}

/*
 * The Mesa attempt into f, with res_bits the index resource's width (zero
 * for the draw's own) and ops replacing index_ops when set.
 */
static SGIRenderDrawResult index_mesa_draw(SGIRenderer *renderer,
                                           PrimCase *p, IndexFixture *f,
                                           unsigned res_bits,
                                           const SGIRenderResourceOps *ops,
                                           const SGIRenderAttemptOptions *o,
                                           SGIRenderAttemptOutcome *outcome,
                                           Error **errp)
{
    SGIRenderResource res = index_resource(res_bits ? res_bits :
                                           p->state.index_bits);
    SGIRasterRules rules;
    SGIRasterTarget target = {
        .opaque = f, .resources = &res, .resource_count = 1,
        .resource_ops = ops ? ops : &index_ops,
    };

    index_rules(p, &rules);
    return sgi_render_draw_attempt(renderer, &rules, &target, &p->draw, o,
                                   outcome, errp);
}

/* Returns the number of pixels whose index or coverage differ. */
static unsigned index_compare(SGIRenderer *renderer, const char *name,
                              PrimCase *p)
{
    static const SGIRenderAttemptOptions mixed = { .mixed_frame = true };
    g_autofree IndexFixture *scalar = g_new(IndexFixture, 1);
    g_autofree IndexFixture *mesa = g_new(IndexFixture, 1);
    SGIRenderAttemptOutcome outcome;
    SGIRenderDrawResult result;
    unsigned differences = 0, drawn = 0;
    Error *err = NULL;

    index_destination(scalar, p->state.index_bits);
    index_destination(mesa, p->state.index_bits);
    index_scalar_draw(p, scalar);
    result = index_mesa_draw(renderer, p, mesa, 0, NULL, &mixed, &outcome,
                             &err);
    if (result != SGI_RENDER_DRAW_OK) {
        g_test_message("%s: attempt returned %d (%s): %s", name, result,
                       sgi_render_refusal_name(outcome.reason),
                       err ? error_get_pretty(err) : "(no error)");
        g_clear_pointer(&err, error_free);
        return PRIM_SIZE * PRIM_SIZE;
    }
    for (unsigned y = 0; y < PRIM_SIZE; y++) {
        for (unsigned x = 0; x < PRIM_SIZE; x++) {
            drawn += scalar->written[y][x];
            if (scalar->index[y][x] != mesa->index[y][x] ||
                scalar->written[y][x] != mesa->written[y][x]) {
                differences++;
                g_test_message("%s: pixel (%u,%u) scalar %s 0x%x mesa %s "
                               "0x%x", name, x, y,
                               scalar->written[y][x] ? "drawn" : "kept",
                               scalar->index[y][x],
                               mesa->written[y][x] ? "drawn" : "kept",
                               mesa->index[y][x]);
            }
        }
    }
    /* A case that draws nothing proves nothing. */
    if (!drawn) {
        g_test_message("%s: the scalar path drew no pixel", name);
        differences++;
    }
    return differences;
}

static void index_expect(SGIRenderer *renderer, const char *name, PrimCase *p,
                         unsigned *failures)
{
    unsigned differences = index_compare(renderer, name, p);

    if (differences) {
        g_test_message("%s: %u pixels differ", name, differences);
        (*failures)++;
    }
}

/* Vertices for each accepted primitive, inside the window. */
static void index_shape(PrimCase *p, uint32_t mode, const float *indices)
{
    static const float points[][2] = {
        { 3.5f, 3.5f }, { 10.25f, 4.75f }, { 6.5f, 12.5f }, { 13.5f, 13.5f },
    };
    static const float strip[][2] = {
        { 1.25f, 2.5f }, { 2.75f, 13.5f }, { 7.5f, 1.75f }, { 8.25f, 14.25f },
        { 14.5f, 3.25f }, { 13.75f, 12.5f },
    };
    static const float fan[][2] = {
        { 8.25f, 8.5f }, { 1.5f, 2.25f }, { 14.25f, 1.75f },
        { 14.5f, 14.25f }, { 1.75f, 13.5f },
    };
    static const unsigned quad_order[] = { 0, 2, 3, 1 };
    const float (*xy)[2] = mode == SGI_GL_POINTS ? points :
                           mode == SGI_GL_TRIANGLE_FAN ||
                           mode == SGI_GL_POLYGON ? fan : strip;
    unsigned n = mode == SGI_GL_POINTS ? ARRAY_SIZE(points) :
                 mode == SGI_GL_TRIANGLE_FAN ? ARRAY_SIZE(fan) :
                 mode == SGI_GL_POLYGON ? 4 :
                 mode == SGI_GL_TRIANGLES ? 6 :
                 mode == SGI_GL_QUADS ? 4 : ARRAY_SIZE(strip);

    for (unsigned i = 0; i < n; i++) {
        /* Quads and polygons take their corners in order round the edge. */
        unsigned k = mode == SGI_GL_QUADS ? quad_order[i] :
                     mode == SGI_GL_POLYGON ? i + 1 : i;

        index_vertex(p, xy[k][0], xy[k][1], indices[i]);
    }
}

/*
 * Each accepted primitive, flat with a different index per vertex so the
 * provoking vertex shows, and smooth with one index throughout: points of
 * sizes 1 to 4, triangles, strips, fans, quadrilaterals, quad strips and
 * polygons, all bit-identical to the scalar path with matching coverage.
 */
static void test_index_primitives(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const uint32_t modes[] = {
        SGI_GL_POINTS, SGI_GL_TRIANGLES, SGI_GL_TRIANGLE_STRIP,
        SGI_GL_TRIANGLE_FAN, SGI_GL_QUADS, SGI_GL_QUAD_STRIP, SGI_GL_POLYGON,
    };
    static const float distinct[] = { 3, 17, 42, 99, 200, 7 };
    static const float same[] = { 77, 77, 77, 77, 77, 77 };
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
    for (unsigned i = 0; i < ARRAY_SIZE(modes); i++) {
        for (unsigned smooth = 0; smooth < 2; smooth++) {
            unsigned sizes = modes[i] == SGI_GL_POINTS ? 4 : 1;

            for (unsigned size = 1; size <= sizes; size++) {
                g_autofree char *name = g_strdup_printf(
                    "mode %u %s size %u", modes[i],
                    smooth ? "smooth" : "flat", size);

                index_case_init(p, modes[i], 8);
                p->state.shade_model = smooth ? SGI_GL_SMOOTH : SGI_GL_FLAT;
                p->draw.point_size = size;
                index_shape(p, modes[i], smooth ? same : distinct);
                index_expect(renderer, name, p, &failures);
            }
        }
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * Index values at widths 8, 12 and 32, including the largest, negative
 * and fractional ones, through flat triangles and quads, and index masks:
 * full, zero and partial over the destination's old values. A covered
 * pixel whose value does not change is still written; an uncovered one
 * never is.
 */
static void test_index_values_and_masks(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const unsigned widths[] = { 8, 12, 32 };
    static const float values[] = {
        0, 1, 255, 4095, 4294967040.0f, -1, -2.5f, 300.75f, -70000.25f,
    };
    static const uint32_t masks[] = {
        UINT32_MAX, 0, 0x0f0, 0xa5a5a5a5, 0x80000001,
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
    for (unsigned w = 0; w < ARRAY_SIZE(widths); w++) {
        for (unsigned v = 0; v < ARRAY_SIZE(values); v++) {
            for (unsigned m = 0; m < ARRAY_SIZE(masks); m++) {
                for (unsigned quad = 0; quad < 2; quad++) {
                    float idx[6];
                    g_autofree char *name = g_strdup_printf(
                        "width %u value %g mask 0x%x %s", widths[w],
                        values[v], masks[m], quad ? "quad" : "triangles");

                    for (unsigned k = 0; k < 6; k++) {
                        idx[k] = values[v];
                    }
                    index_case_init(p, quad ? SGI_GL_QUADS :
                                    SGI_GL_TRIANGLES, widths[w]);
                    p->state.index_mask = masks[m];
                    index_shape(p, p->draw.mode, idx);
                    index_expect(renderer, name, p, &failures);
                }
            }
        }
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * States the scalar path ignores in index mode change nothing here
 * either: blending, the alpha test, the colour mask, the RGBA logical
 * operation and dithering under rules that dither.
 */
static void test_index_ignored_states(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float idx[] = { 9, 9, 9, 9, 9, 9 };
    static const char *const names[] = {
        "blend", "alpha test", "colour mask", "colour logic op", "dither",
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
    for (unsigned i = 0; i < ARRAY_SIZE(names); i++) {
        index_case_init(p, SGI_GL_TRIANGLES, 8);
        index_shape(p, SGI_GL_TRIANGLES, idx);
        switch (i) {
        case 0:
            p->state.blend = 1;
            p->state.blend_equation = SGI_GL_FUNC_ADD;
            p->state.blend_src = SGI_GL_SRC_ALPHA;
            p->state.blend_dst = SGI_GL_ONE_MINUS_SRC_ALPHA;
            break;
        case 1:
            p->state.alpha_test = 1;
            p->state.alpha_func = 0x0200; /* GL_NEVER */
            break;
        case 2:
            p->state.colour_mask = 0;
            break;
        case 3:
            p->state.color_logic_op = 1;
            p->state.logic_op_mode = 0x1500; /* GL_CLEAR */
            break;
        case 4:
            p->state.dither = 1;
            index_rules_dither = true;
            break;
        }
        index_expect(renderer, names[i], p, &failures);
        index_rules_dither = false;
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * Apply refused case i to an accepted indexed draw p: returns its name and
 * expected reason, or NULL past the last case.
 */
static const char *index_refusal_case(PrimCase *p, unsigned i,
                                      SGIRenderRefusal *reason,
                                      unsigned *res_bits, bool *no_commit)
{
    static const float unequal[] = { 5, 6, 5, 5, 5, 5 };
    static const uint32_t line_modes[2] = { SGI_GL_LINE, SGI_GL_LINE };

    switch (i) {
    case 0:
        p->draw.mode = SGI_GL_LINES;
        *reason = SGI_RENDER_REFUSAL_PRIMITIVE;
        return "lines";
    case 1:
        p->state.shade_model = SGI_GL_SMOOTH;
        p->draw.count = 0;
        index_shape(p, SGI_GL_TRIANGLES, unequal);
        *reason = SGI_RENDER_REFUSAL_COLOUR_INDEX;
        return "smooth unequal";
    case 2:
        p->state.shade_model = SGI_GL_SMOOTH;
        p->vertices[1][SGI_GL_V_POSITION + 2] = -1.5f;
        *reason = SGI_RENDER_REFUSAL_INDEX_CLIPPED;
        return "smooth past the near plane";
    case 3:
        p->state.fog = 1;
        *reason = SGI_RENDER_REFUSAL_FOG;
        return "fog";
    case 4:
        p->state.index_logic_op = 1;
        *reason = SGI_RENDER_REFUSAL_LOGIC_OP;
        return "index logic op";
    case 5:
        p->state.texture_2d = 1;
        *reason = SGI_RENDER_REFUSAL_TEXTURE;
        return "texture";
    case 6:
        p->state.depth_test = 1;
        *reason = SGI_RENDER_REFUSAL_FRAGMENT_TESTS;
        return "depth test";
    case 7:
        p->state.stencil_test = 1;
        *reason = SGI_RENDER_REFUSAL_FRAGMENT_TESTS;
        return "stencil test";
    case 8:
        p->state.clip_planes = 1;
        *reason = SGI_RENDER_REFUSAL_CLIP_PLANES;
        return "clip plane";
    case 9:
        p->state.polygon_offset = 1;
        *reason = SGI_RENDER_REFUSAL_POLYGON_OFFSET;
        return "polygon offset";
    case 10:
        p->state.cull_face = 1;
        *reason = SGI_RENDER_REFUSAL_FACING;
        return "culling";
    case 11:
        p->smooth_rules = true;
        p->draw.polygon_smooth = true;
        *reason = SGI_RENDER_REFUSAL_SMOOTHING;
        return "polygon smoothing";
    case 12:
        p->draw.polygon_modes = line_modes;
        *reason = SGI_RENDER_REFUSAL_PRIMITIVE;
        return "polygon mode";
    case 13:
        p->draw.polygon_stipple_enabled = true;
        *reason = SGI_RENDER_REFUSAL_PRIMITIVE;
        return "polygon stipple";
    case 14:
        p->state.draw_buffers = SGI_GL_DRAW_BACK;
        *reason = SGI_RENDER_REFUSAL_BUFFERS;
        return "back buffer";
    case 15:
        *res_bits = 12;
        *reason = SGI_RENDER_REFUSAL_STORE_WIDTH;
        return "width mismatch";
    case 16:
        p->vertices[2][SGI_GL_V_INDEX] = INFINITY;
        *reason = SGI_RENDER_REFUSAL_GEOMETRY;
        return "non-finite index";
    case 17:
        *no_commit = true;
        *reason = SGI_RENDER_REFUSAL_PROVIDER;
        return "no index commit";
    case 18:
        p->state.scissor_test = 1;
        *reason = SGI_RENDER_REFUSAL_MIXED_FRAME;
        return "scissor";
    case 19:
        p->state.drawable.clip.n = 2;
        p->state.drawable.clip.r[1] = p->state.drawable.clip.r[0];
        *reason = SGI_RENDER_REFUSAL_MIXED_FRAME;
        return "two clips";
    case 20:
        p->state.viewport[0] = 1;
        *reason = SGI_RENDER_REFUSAL_MIXED_FRAME;
        return "offset viewport";
    case 21:
        p->state.modelview[0] = 0.5f;
        *reason = SGI_RENDER_REFUSAL_MIXED_FRAME;
        return "matrix";
    case 22:
        p->draw.mode = SGI_GL_POINTS;
        p->draw.point_size = 5;
        *reason = SGI_RENDER_REFUSAL_MIXED_FRAME;
        return "point size 5";
    default:
        return NULL;
    }
}

/*
 * Every refused state returns its reason before the snapshot, commits
 * nothing, and is out of the set; an accepted draw is in it.
 */
static void test_index_refusals(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const SGIRenderResourceOps no_index_commit = {
        .snapshot = index_snapshot,
        .commit_covered = index_colour_commit,
    };
    static const float idx[] = { 5, 5, 5, 5, 5, 5 };
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
    for (unsigned i = 0; ; i++) {
        g_autofree IndexFixture *f = g_new(IndexFixture, 1);
        static const SGIRenderAttemptOptions mixed = { .mixed_frame = true };
        const SGIRenderResourceOps *ops;
        SGIRenderRefusal expected;
        SGIRenderAttemptOutcome outcome;
        SGIRenderSignature signature;
        SGIRenderResource res;
        SGIRasterRules rules;
        SGIRasterTarget target;
        SGIRenderDrawResult result;
        unsigned res_bits = 0;
        bool no_commit = false;
        const char *name;

        index_case_init(p, SGI_GL_TRIANGLES, 8);
        index_shape(p, SGI_GL_TRIANGLES, idx);
        name = index_refusal_case(p, i, &expected, &res_bits, &no_commit);
        ops = no_commit ? &no_index_commit : NULL;
        if (!name) {
            break;
        }
        index_destination(f, 8);
        result = index_mesa_draw(renderer, p, f, res_bits, ops, &mixed,
                                 &outcome, &err);
        res = index_resource(res_bits ? res_bits : 8);
        target = (SGIRasterTarget) {
            .resources = &res, .resource_count = 1,
            .resource_ops = ops ? ops : &index_ops,
        };
        prim_rules(p, &rules);
        sgi_render_draw_signature(&rules, &target, &p->draw, &signature);
        if (result != SGI_RENDER_DRAW_REFUSED || outcome.reason != expected ||
            f->snapshots || f->commits || !err ||
            (ops != &no_index_commit && signature.in_set)) {
            g_test_message("%s: result %d reason %s, %u snapshots, "
                           "%u commits, in-set %d", name, result,
                           sgi_render_refusal_name(outcome.reason),
                           f->snapshots, f->commits, signature.in_set);
            failures++;
        }
        g_clear_pointer(&err, error_free);
    }
    /* An accepted indexed draw is in the set, and says it is indexed. */
    {
        SGIRenderResource res = index_resource(8);
        SGIRasterTarget target = {
            .resources = &res, .resource_count = 1,
            .resource_ops = &index_ops,
        };
        SGIRenderSignature signature;
        SGIRasterRules rules;

        index_case_init(p, SGI_GL_TRIANGLES, 8);
        index_shape(p, SGI_GL_TRIANGLES, idx);
        prim_rules(p, &rules);
        sgi_render_draw_signature(&rules, &target, &p->draw, &signature);
        g_assert_true(signature.in_set);
        g_assert_true(g_str_has_suffix(signature.text, " index=8"));
    }
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * An indexed draw selects only the index resource, even beside a colour
 * resource for the same target; a colour draw never selects it.
 */
static void test_index_resource_selection(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float idx[] = { 33, 33, 33, 33, 33, 33 };
    static const SGIRenderAttemptOptions none = { 0 };
    PrimCase *p = g_new(PrimCase, 1);
    g_autofree IndexFixture *f = g_new(IndexFixture, 1);
    SGIRenderResource res[2];
    SGIRasterRules rules;
    SGIRasterTarget target = {
        .opaque = f, .resources = res, .resource_count = 2,
        .resource_ops = &index_ops,
    };
    SGIRenderAttemptOutcome outcome;
    SGIRenderer *renderer;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    index_case_init(p, SGI_GL_TRIANGLES, 8);
    index_shape(p, SGI_GL_TRIANGLES, idx);
    prim_rules(p, &rules);
    prim_resources(p, res);
    res[1] = index_resource(8);
    index_destination(f, 8);
    g_assert_cmpint(sgi_render_draw_attempt(renderer, &rules, &target,
                                            &p->draw, &none, &outcome,
                                            &err), ==, SGI_RENDER_DRAW_OK);
    g_assert_null(err);
    g_assert_cmpuint(f->commits, ==, 1);
    g_assert_cmphex(f->effective_mask, ==, 0xff);
    /* Only the index resource: an RGBA draw refuses for want of colour. */
    p->state.index_bits = 0;
    target.resources = &res[1];
    target.resource_count = 1;
    g_assert_cmpint(sgi_render_draw_attempt(renderer, &rules, &target,
                                            &p->draw, &none, &outcome,
                                            &err), ==,
                    SGI_RENDER_DRAW_REFUSED);
    g_assert_cmpint(outcome.reason, ==, SGI_RENDER_REFUSAL_RESOURCE);
    g_clear_pointer(&err, error_free);
    g_assert_cmpuint(f->commits, ==, 1);
    sgi_renderer_free(renderer);
    g_free(p);
}

static void fail_index_read_pixels(GLint x, GLint y, GLsizei width,
                                   GLsizei height, GLenum format,
                                   GLenum type, void *pixels)
{
    saved_read_pixels(x, y, width, height,
                      format == GL_RED_INTEGER ? GL_NONE : format,
                      type, pixels);
}

/*
 * Failures: an incomplete index framebuffer, a failed integer readback
 * and a failed coverage readback commit nothing and fail the renderer; a
 * commit that stops partway reports its prefix as a failure.
 */
static void test_index_failures(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float idx[] = { 12, 12, 12, 12, 12, 12 };
    static const SGIRenderAttemptOptions none = { 0 };
    PrimCase *p = g_new(PrimCase, 1);
    g_autofree IndexFixture *f = g_new(IndexFixture, 1);
    SGIRenderAttemptOutcome outcome;
    SGIRenderer *renderer;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    index_case_init(p, SGI_GL_TRIANGLES, 8);
    index_shape(p, SGI_GL_TRIANGLES, idx);

    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    index_destination(f, 8);
    saved_framebuffer_status = epoxy_glCheckFramebufferStatus;
    epoxy_glCheckFramebufferStatus = fail_framebuffer_status;
    g_assert_cmpint(index_mesa_draw(renderer, p, f, 0, NULL, &none, &outcome,
                                    &err), ==, SGI_RENDER_DRAW_FAILED);
    epoxy_glCheckFramebufferStatus = saved_framebuffer_status;
    g_assert_cmpint(outcome.status, ==, SGI_RENDER_HOST_ERROR);
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
    g_assert_cmpuint(f->snapshots, ==, 1);
    g_assert_cmpuint(f->commits, ==, 0);
    sgi_renderer_free(renderer);

    for (unsigned stencil = 0; stencil < 2; stencil++) {
        renderer = sgi_renderer_new(&err);
        g_assert_nonnull(renderer);
        index_destination(f, 8);
        saved_read_pixels = epoxy_glReadPixels;
        epoxy_glReadPixels = stencil ? fail_stencil_read_pixels :
                                       fail_index_read_pixels;
        g_assert_cmpint(index_mesa_draw(renderer, p, f, 0, NULL, &none,
                                        &outcome, &err), ==,
                        SGI_RENDER_DRAW_FAILED);
        epoxy_glReadPixels = saved_read_pixels;
        g_assert_cmpint(outcome.status, ==, SGI_RENDER_HOST_ERROR);
        g_assert_nonnull(err);
        g_clear_pointer(&err, error_free);
        g_assert_cmpuint(f->snapshots, ==, 1);
        g_assert_cmpuint(f->commits, ==, 0);
        sgi_renderer_free(renderer);
    }

    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    index_destination(f, 8);
    f->fail_after = 5;
    g_assert_cmpint(index_mesa_draw(renderer, p, f, 0, NULL, &none, &outcome,
                                    &err), ==, SGI_RENDER_DRAW_FAILED);
    g_assert_cmpint(outcome.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
    g_assert_cmpuint(f->commits, ==, 1);
    sgi_renderer_free(renderer);
    g_free(p);
}

/*
 * Off the reference driver an indexed draw is refused as host-driver
 * before any snapshot even without the mixed-frame flag, since its subset
 * is proven only there; an RGBA draw with the flag clear still draws.
 */
static void test_index_off_reference(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float idx[] = { 40, 40, 40, 40, 40, 40 };
    static const SGIRenderAttemptOptions none = { 0 };
    PrimCase *p = g_new(PrimCase, 1);
    g_autofree IndexFixture *f = g_new(IndexFixture, 1);
    g_autofree PrimFixture *rgba = g_new(PrimFixture, 1);
    SGIRenderAttemptOutcome outcome;
    SGIRenderer *renderer;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    index_case_init(p, SGI_GL_TRIANGLES, 8);
    index_shape(p, SGI_GL_TRIANGLES, idx);
    index_destination(f, 8);
    g_assert_cmpint(index_mesa_draw(renderer, p, f, 0, NULL, &none, &outcome,
                                    &err), ==, SGI_RENDER_DRAW_OK);
    g_assert_null(err);

    sgi_renderer_test_set_reference(renderer, false);
    index_destination(f, 8);
    g_assert_cmpint(index_mesa_draw(renderer, p, f, 0, NULL, &none, &outcome,
                                    &err), ==, SGI_RENDER_DRAW_REFUSED);
    g_assert_cmpint(outcome.reason, ==, SGI_RENDER_REFUSAL_HOST_DRIVER);
    g_assert_nonnull(err);
    g_clear_pointer(&err, error_free);
    g_assert_cmpuint(f->snapshots, ==, 0);
    g_assert_cmpuint(f->commits, ==, 0);

    prim_fan(p, 4);
    prim_destination(rgba, p);
    g_assert_cmpint(prim_mesa_draw(renderer, p, rgba, &err), ==,
                    SGI_RENDER_OK);
    g_assert_null(err);
    sgi_renderer_free(renderer);
    g_free(p);
}

/*
 * The RGBA depth-plane vectors as flat indexed triangles: with an edge on
 * the near or far plane the scalar path may drop fragments whose depth
 * rounds past it, which the integer program cannot follow, so each such
 * draw is refused before the snapshot, and every draw accepted equals the
 * scalar path.
 */
static void test_index_depth_planes(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float shapes[][3][2] = {
        { { 2.5f, 1.5f }, { 2.5f, 14.5f }, { 13.25f, 7.75f } },
        { { 13.5f, 14.5f }, { 13.5f, 1.5f }, { 2.25f, 8.25f } },
        { { 14.5f, 13.5f }, { 1.5f, 13.5f }, { 7.75f, 2.25f } },
        { { 1.5f, 2.5f }, { 14.5f, 2.5f }, { 8.25f, 13.75f } },
        { { 13.5f, 13.5f }, { 1.5f, 1.5f }, { 1.75f, 12.25f } },
    };
    static const float apex[] = { -0.37f, 0.1f, 0.53f, 0.9f, -0.85f };
    static const float planes[] = { -1, 1, 0.25f };
    static const SGIRenderAttemptOptions mixed = { .mixed_frame = true };
    PrimCase *p = g_new(PrimCase, 1);
    SGIRenderer *renderer;
    unsigned failures = 0, accepted = 0, refused = 0;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    for (unsigned s = 0; s < ARRAY_SIZE(shapes); s++) {
        for (unsigned z = 0; z < ARRAY_SIZE(planes); z++) {
            for (unsigned a = 0; a < ARRAY_SIZE(apex); a++) {
                for (unsigned first = 0; first < 3; first++) {
                    g_autofree IndexFixture *scalar = g_new(IndexFixture, 1);
                    g_autofree IndexFixture *mesa = g_new(IndexFixture, 1);
                    SGIRenderAttemptOutcome outcome;
                    SGIRenderDrawResult result;
                    unsigned differences = 0;

                    index_case_init(p, SGI_GL_TRIANGLES, 12);
                    for (unsigned i = 0; i < 3; i++) {
                        unsigned v = (first + i) % 3;

                        index_vertex(p, shapes[s][v][0], shapes[s][v][1],
                                     100 + i);
                        p->vertices[i][SGI_GL_V_POSITION + 2] =
                            v < 2 ? planes[z] : apex[a];
                    }
                    index_destination(scalar, 12);
                    index_destination(mesa, 12);
                    index_scalar_draw(p, scalar);
                    result = index_mesa_draw(renderer, p, mesa, 0, NULL,
                                             &mixed, &outcome, &err);
                    g_clear_pointer(&err, error_free);
                    if (result == SGI_RENDER_DRAW_REFUSED &&
                        outcome.reason == SGI_RENDER_REFUSAL_GEOMETRY &&
                        !mesa->snapshots) {
                        refused++;
                        continue;
                    }
                    if (result != SGI_RENDER_DRAW_OK) {
                        g_test_message("shape %u plane %g apex %g from %u: "
                                       "result %d (%s)", s, planes[z],
                                       apex[a], first, result,
                                       sgi_render_refusal_name(
                                           outcome.reason));
                        failures++;
                        continue;
                    }
                    accepted++;
                    for (unsigned y = 0; y < PRIM_SIZE; y++) {
                        for (unsigned x = 0; x < PRIM_SIZE; x++) {
                            differences +=
                                scalar->index[y][x] != mesa->index[y][x] ||
                                scalar->written[y][x] != mesa->written[y][x];
                        }
                    }
                    if (differences) {
                        g_test_message("shape %u plane %g apex %g from %u: "
                                       "%u pixels differ", s, planes[z],
                                       apex[a], first, differences);
                        failures++;
                    }
                }
            }
        }
    }
    g_test_message("depth planes: %u accepted, %u refused, %u differ",
                   accepted, refused, failures);
    /* Both outcomes occur, so the case cannot pass vacuously. */
    g_assert_cmpuint(accepted, >, 0);
    g_assert_cmpuint(refused, >, 0);
    sgi_renderer_free(renderer);
    g_free(p);
    if (failures) {
        g_test_fail();
    }
}

/*
 * An indexed draw after an RGBA draw, and an RGBA draw after it, on one
 * renderer give the results each gives on a fresh renderer.
 */
static void test_index_carry_over(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float idx[] = { 21, 21, 21, 21, 21, 21 };
    static const SGIRenderAttemptOptions none = { 0 };
    PrimCase *p = g_new(PrimCase, 1);
    PrimCase *rgba = g_new(PrimCase, 1);
    g_autofree IndexFixture *fresh = g_new(IndexFixture, 1);
    g_autofree IndexFixture *after = g_new(IndexFixture, 1);
    g_autofree PrimFixture *rgba_fresh = g_new(PrimFixture, 1);
    g_autofree PrimFixture *rgba_after = g_new(PrimFixture, 1);
    SGIRenderAttemptOutcome outcome;
    SGIRenderer *renderer;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        g_free(rgba);
        return;
    }
    index_case_init(p, SGI_GL_TRIANGLE_FAN, 12);
    index_shape(p, SGI_GL_TRIANGLE_FAN, idx);
    prim_fan(rgba, 4);

    renderer = sgi_renderer_new(&err);
    index_destination(fresh, 12);
    g_assert_cmpint(index_mesa_draw(renderer, p, fresh, 0, NULL, &none,
                                    &outcome, &err), ==, SGI_RENDER_DRAW_OK);
    sgi_renderer_free(renderer);
    renderer = sgi_renderer_new(&err);
    prim_destination(rgba_fresh, rgba);
    g_assert_cmpint(prim_mesa_draw(renderer, rgba, rgba_fresh, &err), ==,
                    SGI_RENDER_OK);
    sgi_renderer_free(renderer);

    renderer = sgi_renderer_new(&err);
    prim_destination(rgba_after, rgba);
    g_assert_cmpint(prim_mesa_draw(renderer, rgba, rgba_after, &err), ==,
                    SGI_RENDER_OK);
    index_destination(after, 12);
    g_assert_cmpint(index_mesa_draw(renderer, p, after, 0, NULL, &none,
                                    &outcome, &err), ==, SGI_RENDER_DRAW_OK);
    prim_destination(rgba_after, rgba);
    g_assert_cmpint(prim_mesa_draw(renderer, rgba, rgba_after, &err), ==,
                    SGI_RENDER_OK);
    g_assert_null(err);
    sgi_renderer_free(renderer);
    g_assert_cmpmem(fresh->index, sizeof(fresh->index), after->index,
                    sizeof(after->index));
    g_assert_cmpmem(fresh->written, sizeof(fresh->written), after->written,
                    sizeof(after->written));
    g_assert_cmpmem(rgba_fresh->rgba, sizeof(rgba_fresh->rgba),
                    rgba_after->rgba, sizeof(rgba_after->rgba));
    g_free(p);
    g_free(rgba);
}

/* The shadow capture's dense copy into a private index resource. */
static SGIRenderCommitResult index_dense_commit(
    void *opaque, const SGIRenderResource *res, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, Error **errp)
{
    IndexFixture *f = opaque;
    uint64_t complete = 0;

    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            f->index[y][x] = slots[(y - rect->y0) * stride_slots +
                                   x - rect->x0];
            complete++;
        }
    }
    return (SGIRenderCommitResult) {
        .status = SGI_RENDER_OK,
        .complete_pixels = complete,
        .extent_known = true,
    };
}

static const SGIRenderResourceOps index_shadow_ops = {
    .snapshot = index_snapshot,
    .commit = index_dense_commit,
    .commit_covered = index_colour_commit,
    .commit_index_covered = index_commit,
};

/*
 * Shadow comparison of an indexed draw: Mesa's result through the private
 * copy matches the scalar draw, coverage included, and a native result
 * that differs is a mismatch at its pixel.
 */
static void test_index_shadow(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float idx[] = { 3, 17, 42, 99, 200, 7 };
    PrimCase *p = g_new(PrimCase, 1);
    g_autofree IndexFixture *native = g_new(IndexFixture, 1);
    g_autofree IndexFixture *private = g_new(IndexFixture, 1);
    SGIRenderResource res = index_resource(8);
    SGIRasterTarget native_target = {
        .opaque = native, .resources = &res, .resource_count = 1,
        .resource_ops = &index_ops,
    };
    SGIRasterTarget private_target = native_target;
    SGIRenderShadowOutcome out;
    const uint8_t *masks[1];
    SGIRenderShadow *shadow;
    SGIRasterRules rules;
    Error *err = NULL;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        g_free(p);
        return;
    }
    shadow = sgi_render_shadow_new();
    g_assert_null(sgi_render_shadow_disabled(shadow));
    index_case_init(p, SGI_GL_QUAD_STRIP, 8);
    index_shape(p, SGI_GL_QUAD_STRIP, idx);
    prim_rules(p, &rules);
    private_target.opaque = private;
    private_target.resource_ops = &index_shadow_ops;
    index_destination(native, 8);
    index_destination(private, 8);
    memset(private->index, 0, sizeof(private->index));
    g_assert_cmpint(sgi_render_shadow_capture(shadow, &native_target,
                                              &private_target, &err), ==,
                    SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpmem(private->index, sizeof(private->index), native->index,
                    sizeof(native->index));
    index_scalar_draw(p, native);
    masks[0] = &native->written[0][0];
    g_assert_cmpint(sgi_render_shadow_draw(shadow, &rules, &native_target,
                                           &private_target, &p->draw, 0,
                                           masks, &out), ==,
                    SGI_RENDER_SHADOW_MATCH);
    g_assert_true(out.coverage_compared);
    g_assert_true(out.signature.in_set);
    /* A Mesa result that differs is a mismatch at its first pixel. */
    index_destination(native, 8);
    index_destination(private, 8);
    g_assert_cmpint(sgi_render_shadow_capture(shadow, &native_target,
                                              &private_target, &err), ==,
                    SGI_RENDER_OK);
    index_scalar_draw(p, native);
    native->index[8][8] ^= 1;
    g_assert_cmpint(sgi_render_shadow_draw(shadow, &rules, &native_target,
                                           &private_target, &p->draw, 0,
                                           masks, &out), ==,
                    SGI_RENDER_SHADOW_MISMATCH);
    g_assert_cmpuint(out.x, ==, 8);
    g_assert_cmpuint(out.y, ==, 8);
    sgi_render_shadow_free(shadow);
    g_free(p);
}

/*
 * Timing of a small draw through the two host routes, enabled by
 * QEMU_TEST_SGI_BENCH: the legacy batch KONA sends for one small quad in
 * a 1280x1024 window (two triangles over 10 by 8 tiles of 128 pixels),
 * and the checked draw path with one small triangle on a 1280x1024
 * RGBA8 resource. It records numbers and asserts nothing about them.
 */
#define BENCH_W 1280
#define BENCH_H 1024
#define BENCH_REPS 10

typedef struct BenchPlane {
    uint32_t *slots;            /* BENCH_W * BENCH_H * 4 */
} BenchPlane;

static SGIRenderResult bench_snapshot(void *opaque,
                                      const SGIRenderResource *resource,
                                      uint32_t slice,
                                      const SGIRenderRect *rect,
                                      uint32_t *slots, size_t stride_slots,
                                      size_t capacity_slots, Error **errp)
{
    BenchPlane *p = opaque;
    size_t n = resource->component_count;

    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        memcpy(&slots[(y - rect->y0) * stride_slots],
               &p->slots[((size_t)y * BENCH_W + rect->x0) * n],
               (size_t)(rect->x1 - rect->x0 + 1) * n * sizeof(*slots));
    }
    return SGI_RENDER_OK;
}

static SGIRenderCommitResult bench_commit_covered(
    void *opaque, const SGIRenderResource *res, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, const SGIRenderWriteCoverage *writes,
    Error **errp)
{
    BenchPlane *p = opaque;
    size_t n = res->component_count;

    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            if (writes->pixel[(size_t)(y - rect->y0) * writes->stride_bytes +
                              x - rect->x0]) {
                memcpy(&p->slots[((size_t)y * BENCH_W + x) * n],
                       &slots[(y - rect->y0) * stride_slots +
                              (x - rect->x0) * n], n * sizeof(*slots));
            }
        }
    }
    return (SGIRenderCommitResult) {
        .status = SGI_RENDER_OK,
        .complete_pixels = (uint64_t)(rect->x1 - rect->x0 + 1) *
                           (rect->y1 - rect->y0 + 1),
        .extent_known = true,
    };
}

static const SGIRenderResourceOps bench_ops = {
    .snapshot = bench_snapshot,
    .commit_covered = bench_commit_covered,
};

static void bench_report(const char *label, double wall_ms, unsigned reps)
{
    g_test_message("%s: %.3f ms per call (%u calls)", label,
                   wall_ms / reps, reps);
}

static void test_benchmark(void)
{
    g_auto(MesaTestEnv) mesa_env = { 0 };
    static const float quad[4][2] = {
        { 100, 100 }, { 116, 100 }, { 116, 116 }, { 100, 116 },
    };
    static const unsigned tri[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
    const int64_t bounds[4] = { 0, 0, BENCH_W - 1, BENCH_H - 1 };
    unsigned columns = BENCH_W / SGI_RENDERER_SIZE;
    unsigned rows = BENCH_H / SGI_RENDERER_SIZE;
    unsigned count = 2 * columns * rows;
    g_autofree SGIRendererRequest *requests = NULL;
    g_autofree BenchPlane *plane = g_new0(BenchPlane, 1);
    SGIRenderer *renderer;
    Error *err = NULL;
    int64_t t;

    mesa_test_env_set(&mesa_env);
    if (!mesa_test_available(false)) {
        return;
    }
    g_test_message("LP_NUM_THREADS=%s", g_getenv("LP_NUM_THREADS") ?:
                   "(unset)");
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    requests = g_new0(SGIRendererRequest, count);

    /* (a) The legacy batch for one small flat quad. */
    for (unsigned rep = 0; rep <= BENCH_REPS; rep++) {
        for (unsigned i = 0; i < count; i++) {
            SGIRendererRequest *r = &requests[i];
            unsigned tile = i % (columns * rows);
            const unsigned *v = tri[i / (columns * rows)];

            memset(r, 0, sizeof(*r));
            r->generation = 1;
            r->rgb[0] = 200;
            r->alpha = 1;
            for (unsigned k = 0; k < 3; k++) {
                r->vertices[k][0] = quad[v[k]][0];
                r->vertices[k][1] = quad[v[k]][1];
                r->vertices[k][3] = 1;
                r->depth[k] = 0.5;
            }
            g_assert_true(sgi_renderer_request_tile(
                r, (int64_t)(tile % columns) * SGI_RENDERER_SIZE,
                (int64_t)(tile / columns) * SGI_RENDERER_SIZE,
                SGI_RENDERER_SIZE, SGI_RENDERER_SIZE, bounds));
        }
        if (!rep) {
            /* The first batch warms the context; it is not counted. */
            g_assert_cmpint(sgi_renderer_draw_batch_checked(renderer, requests,
                                                            count, &err), ==,
                            SGI_RENDER_DRAW_OK);
            t = get_clock();
            continue;
        }
        g_assert_cmpint(sgi_renderer_draw_batch_checked(renderer, requests,
                                                        count, &err), ==,
                        SGI_RENDER_DRAW_OK);
    }
    bench_report("legacy batch, one 16x16 quad in 1280x1024 (160 requests)",
                 (get_clock() - t) / 1e6, BENCH_REPS);

    /* (b) The checked draw path, flat then smooth. */
    plane->slots = g_new0(uint32_t, (size_t)BENCH_W * BENCH_H * 4);
    for (unsigned smooth = 0; smooth < 2; smooth++) {
        SGIRenderResource res = {
            .backing_id = 1, .backing_generation = 1,
            .view_id = 2, .view_generation = 1,
            .role = SGI_RENDER_COLOUR, .target = 7,
            .buffer = SGI_RENDER_FRONT, .eye = SGI_RENDER_MONO,
            .width = BENCH_W, .height = BENCH_H, .depth = 1,
            .bottom_origin = true, .component_count = 4,
            .component = {
                { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_A, SGI_RENDER_UNORM, 8 },
            },
        };
        SGIRasterTarget target = {
            .opaque = plane, .resources = &res, .resource_count = 1,
            .resource_ops = &bench_ops,
        };
        SGIRasterRules rules = {
            .subpixel_bits = 8, .color_bits = 8, .alpha_bits = 8,
            .depth_bits = 24, .stencil_bits = 8,
            .rounding = SGI_ROUND_HALF_AWAY,
            .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
            .arith = SGI_ARITH_DOUBLE, .line_rule = SGI_LINE_COVERAGE_DOUBLE,
        };
        SGIGLDrawState st = {
            .shade_model = smooth ? SGI_GL_SMOOTH : SGI_GL_FLAT,
            .draw_buffers = SGI_GL_DRAW_FRONT,
            .colour_mask = SGI_GL_COLOUR_MASK_ALL,
            .drawable = {
                .bound = true,
                .clip = { .n = 1, .r[0] = { 0, 0, BENCH_W - 1,
                                            BENCH_H - 1 } },
                .front = 7,
            },
            .viewport = { 0, 0, BENCH_W, BENCH_H },
            .depth_range = { 0, 1 },
        };
        float v[3][SGI_GL_VERTEX_FLOATS] = { { 0 } };
        SGIGLDraw draw = {
            .mode = SGI_GL_TRIANGLES, .count = 3, .vertices = &v[0][0],
            .state = &st,
        };

        for (unsigned i = 0; i < 4; i++) {
            st.modelview[i * 5] = 1;
            st.projection[i * 5] = 1;
        }
        for (unsigned k = 0; k < 3; k++) {
            v[k][SGI_GL_V_POSITION] = (quad[k][0] * 2) / BENCH_W - 1;
            v[k][SGI_GL_V_POSITION + 1] = (quad[k][1] * 2) / BENCH_H - 1;
            v[k][SGI_GL_V_POSITION + 3] = 1;
            v[k][SGI_GL_V_COLOR] = 0.25f * (k + 1);
            v[k][SGI_GL_V_COLOR + 3] = 1;
        }
        for (unsigned rep = 0; rep <= BENCH_REPS; rep++) {
            SGIRenderAttemptOutcome outcome;

            g_assert_cmpint(sgi_render_draw_attempt(renderer, &rules, &target,
                                                    &draw, NULL, &outcome,
                                                    &err), ==,
                            SGI_RENDER_DRAW_OK);
            if (!rep) {
                t = get_clock();
            }
        }
        bench_report(smooth ?
                     "checked draw, one smooth 16-pixel triangle, 1280x1024" :
                     "checked draw, one flat 16-pixel triangle, 1280x1024",
                     (get_clock() - t) / 1e6, BENCH_REPS);
    }
    g_free(plane->slots);
    sgi_renderer_free(renderer);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    module_call_init(MODULE_INIT_QOM);
    /* Registered only on request: the CI report check rejects skips. */
    if (g_getenv("QEMU_TEST_SGI_BENCH")) {
        g_test_add_func("/sgi-renderer-gl/benchmark", test_benchmark);
    }
    g_test_add_func("/sgi-renderer-gl/surfaceless-llvmpipe-pixels",
                    test_surfaceless_llvmpipe_pixels);
    g_test_add_func("/sgi-renderer-gl/program-failure-fails-creation",
                    test_program_failure_fails_creation);
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
    g_test_add_func("/sgi-renderer-gl/attempt-outcomes",
                    test_attempt_outcomes);
    g_test_add_func("/sgi-renderer-gl/refusal-names", test_refusal_names);
    g_test_add_func("/sgi-renderer-gl/mixed-frame-set",
                    test_mixed_frame_set);
    g_test_add_func("/sgi-renderer-gl/attempt-clear", test_attempt_clear);
    g_test_add_func("/sgi-renderer-gl/draw-signature", test_draw_signature);
    g_test_add_func("/sgi-renderer-gl/shadow-comparison",
                    test_shadow_comparison);
    g_test_add_func("/sgi-renderer-gl/shadow-coverage", test_shadow_coverage);
    g_test_add_func("/sgi-renderer-gl/shadow-raster", test_shadow_raster);
    g_test_add_func("/sgi-renderer-gl/index-primitives",
                    test_index_primitives);
    g_test_add_func("/sgi-renderer-gl/index-values-and-masks",
                    test_index_values_and_masks);
    g_test_add_func("/sgi-renderer-gl/index-ignored-states",
                    test_index_ignored_states);
    g_test_add_func("/sgi-renderer-gl/index-refusals", test_index_refusals);
    g_test_add_func("/sgi-renderer-gl/index-resource-selection",
                    test_index_resource_selection);
    g_test_add_func("/sgi-renderer-gl/index-failures", test_index_failures);
    g_test_add_func("/sgi-renderer-gl/index-off-reference",
                    test_index_off_reference);
    g_test_add_func("/sgi-renderer-gl/index-depth-planes",
                    test_index_depth_planes);
    g_test_add_func("/sgi-renderer-gl/index-carry-over",
                    test_index_carry_over);
    g_test_add_func("/sgi-renderer-gl/index-shadow", test_index_shadow);
    g_test_add_func("/sgi-renderer-gl/driver-identity",
                    test_driver_identity);
    g_test_add_func("/sgi-renderer-gl/scalar-match-cross",
                    test_scalar_match_cross);
    g_test_add_func("/sgi-renderer-gl/texture-invalid-clipped-vertex",
                    test_texture_invalid_clipped_vertex);
    g_test_add_func("/sgi-renderer-gl/scalar-match-depth-clipped",
                    test_scalar_match_depth_clipped);
    g_test_add_func("/sgi-renderer-gl/scalar-match-depth-planes",
                    test_scalar_match_depth_planes);
    g_test_add_func("/sgi-renderer-gl/scalar-match-widths",
                    test_scalar_match_widths);
    g_test_add_func("/sgi-renderer-gl/scalar-match-texture-boundary",
                    test_scalar_match_texture_boundary);
    g_test_add_func("/sgi-renderer-gl/scalar-match-texture-sweep",
                    test_scalar_match_texture_sweep);
    g_test_add_func("/sgi-renderer-gl/scalar-match-texture",
                    test_scalar_match_texture);
    g_test_add_func("/sgi-renderer-gl/scalar-match-clear",
                    test_scalar_match_clear);
    g_test_add_func("/sgi-renderer-gl/draw-region", test_draw_region);
    g_test_add_func("/sgi-renderer-gl/draw-region-off-resource",
                    test_draw_region_off_resource);
    g_test_add_func("/sgi-renderer-gl/scalar-match-store-widths",
                    test_scalar_match_store_widths);
    g_test_add_func("/sgi-renderer-gl/scalar-match-points",
                    test_scalar_match_points);
    g_test_add_func("/sgi-renderer-gl/scalar-match-lines",
                    test_scalar_match_lines);
    g_test_add_func("/sgi-renderer-gl/scalar-match-line-ties",
                    test_scalar_match_line_ties);
    g_test_add_func("/sgi-renderer-gl/scalar-match-line-state",
                    test_scalar_match_line_state);
    g_test_add_func("/sgi-renderer-gl/scalar-match-line-stipple",
                    test_scalar_match_line_stipple);
    g_test_add_func("/sgi-renderer-gl/scalar-match-line-sweep",
                    test_scalar_match_line_sweep);
    g_test_add_func("/sgi-renderer-gl/lines-refused", test_lines_refused);
    g_test_add_func("/sgi-renderer-gl/smooth-probe-refused",
                    test_smooth_probe_refused);
    g_test_add_func("/sgi-renderer-gl/viewport-mirror-refused",
                    test_viewport_mirror_refused);
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
    g_test_add_func("/sgi-renderer-gl/smoothing-parity",
                    test_smoothing_parity);
    g_test_add_func("/sgi-renderer-gl/decoded-clear-native-commit",
                    test_decoded_clear_native_commit);
    g_test_add_func("/sgi-renderer-gl/decoded-clear-full-screen",
                    test_decoded_clear_full_screen_commit);
    g_test_add_func("/sgi-renderer-gl/decoded-clear-depth-stencil",
                    test_decoded_clear_depth_stencil);
    if (g_getenv("QEMU_TEST_SGI_RENDER_NODE")) {
#ifdef CONFIG_GBM
        g_test_add_func("/sgi-renderer-gl/shader-output", test_shader_output);
#endif
        for (unsigned mode = 0; mode < 8; mode++) {
            g_autofree char *path = g_strdup_printf(
                "/sgi-renderer-gl/texture/%u", mode);

            g_test_add_data_func(path, GUINT_TO_POINTER(mode),
                                 test_shader_texture);
        }
    }
    for (unsigned plane = 0; plane < SGI_RENDERER_CLIP_PLANES; plane++) {
        g_autofree char *path = g_strdup_printf("/sgi-renderer-gl/user-clip/%u",
                                                plane);

        g_test_add_data_func(path, GUINT_TO_POINTER(8 + plane),
                             test_shader_texture);
    }
    g_test_add_data_func("/sgi-renderer-gl/texture-precision-refusal",
                        GUINT_TO_POINTER(14), test_shader_texture);
    g_test_add_data_func("/sgi-renderer-gl/context-failure",
                         GUINT_TO_POINTER(1), test_worker_fault);
    g_test_add_data_func("/sgi-renderer-gl/shader-failure",
                         GUINT_TO_POINTER(2), test_worker_fault);
    g_test_add_data_func("/sgi-renderer-gl/framebuffer-failure",
                         GUINT_TO_POINTER(3), test_worker_fault);
    g_test_add_data_func("/sgi-renderer-gl/zero-readback-refusal",
                         GUINT_TO_POINTER(4), test_worker_fault);
    g_test_add_data_func("/sgi-renderer-gl/unbind-failure",
                         GUINT_TO_POINTER(5), test_worker_fault);
    g_test_add_data_func("/sgi-renderer-gl/destroy-failure",
                         GUINT_TO_POINTER(6), test_worker_fault);
    g_test_add_data_func("/sgi-renderer-gl/release-thread-failure",
                         GUINT_TO_POINTER(7), test_worker_fault);
    return g_test_run();
}
