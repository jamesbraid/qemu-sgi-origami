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
    SGI_RENDERER_WORK_CLEAR,
} SGIRendererWorkKind;

typedef struct SGIRendererGL {
    QemuThread thread;
    QemuMutex mutex;
    QemuCond condition;
    QemuEGLDisplayLease *lease;
    SGIRendererRequest request;
    SGIRendererFrame frame;
    uint32_t *frame_texture_slots;
    SGIRendererClear clear;
    SGIRendererWorkKind work_kind;
    Error *error;
    bool ready;
    bool pending;
    bool stop;
    bool success;
    EGLContext context;
    GLuint program;
    GLuint texture_program;
    Error *texture_error;
    GLint texture_sampler, texture_mode, texture_wrap_s;
    GLint texture_wrap_t, texture_shade, texture_linear;
    GLint texture_border;
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

static bool renderer_gl_texture_program(SGIRendererGL *renderer)
{
    static const char *vertex_source =
        "#version 450 compatibility\n"
        "out vec4 smooth_colour;\n"
        "flat out vec4 flat_colour;\n"
        "out vec4 texture_coord;\n"
        "void main() {\n"
        "  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
        "  smooth_colour = gl_Color; flat_colour = gl_Color;\n"
        "  texture_coord = gl_MultiTexCoord0;\n"
        "}\n";
    static const char *fragment_source =
        "#version 450 compatibility\n"
        "#define WRAP_REPEAT " G_STRINGIFY(GL_REPEAT) "\n"
        "#define WRAP_CLAMP " G_STRINGIFY(GL_CLAMP) "\n"
        "#define WRAP_EDGE " G_STRINGIFY(GL_CLAMP_TO_EDGE) "\n"
        "#define WRAP_BORDER " G_STRINGIFY(GL_CLAMP_TO_BORDER) "\n"
        "uniform usampler2D image_slots;\n"
        "uniform int environment_mode, wrap_s, wrap_t, flat_shade;\n"
        "uniform int linear_filter;\n"
        "uniform dvec4 border_bytes;\n"
        "in vec4 smooth_colour;\n"
        "flat in vec4 flat_colour;\n"
        "in vec4 texture_coord;\n"
        "layout(location=0) out vec4 pixel;\n"
        "double wrapped_coord(double x, int n, int wrap) {\n"
        "  if (wrap == WRAP_REPEAT) return fract(x);\n"
        "  double edge = 0.5 / double(n);\n"
        "  if (wrap == WRAP_EDGE)\n"
        "    return clamp(x, edge, 1.0 - edge);\n"
        "  if (wrap == WRAP_BORDER)\n"
        "    return clamp(x, -edge, 1.0 + edge);\n"
        "  return clamp(x, 0.0, 1.0);\n"
        "}\n"
        "int wrapped_index(int i, int n, int wrap) {\n"
        "  if (wrap == WRAP_REPEAT)\n"
        "    return i < 0 ? n - 1 : (i >= n ? 0 : i);\n"
        "  return wrap == WRAP_EDGE ? clamp(i, 0, n - 1) : i;\n"
        "}\n"
        "dvec4 fetch_slot(ivec2 xy, ivec2 size) {\n"
        "  xy = ivec2(wrapped_index(xy.x, size.x, wrap_s),\n"
        "             wrapped_index(xy.y, size.y, wrap_t));\n"
        "  if (any(lessThan(xy, ivec2(0))) ||\n"
        "      any(greaterThanEqual(xy, size))) return border_bytes;\n"
        "  return dvec4(texelFetch(image_slots, xy, 0));\n"
        "}\n"
        "void main() {\n"
        "  ivec2 size = textureSize(image_slots, 0);\n"
        "  dvec2 st = dvec2(texture_coord.xy) / double(texture_coord.w);\n"
        "  dvec2 uv = dvec2(wrapped_coord(st.x, size.x, wrap_s),\n"
        "                    wrapped_coord(st.y, size.y, wrap_t));\n"
        "  dvec4 texel;\n"
        "  if (linear_filter != 0) {\n"
        "    dvec2 pos = uv * dvec2(size) - 0.5;\n"
        "    ivec2 lo = ivec2(floor(pos));\n"
        "    ivec2 hi = lo + ivec2(1);\n"
        "    dvec2 weight = fract(pos);\n"
        "    dvec4 c00 = fetch_slot(lo, size);\n"
        "    dvec4 c10 = fetch_slot(ivec2(hi.x, lo.y), size);\n"
        "    dvec4 c01 = fetch_slot(ivec2(lo.x, hi.y), size);\n"
        "    dvec4 c11 = fetch_slot(hi, size);\n"
        "    texel = c00 * (1.0 - weight.x) * (1.0 - weight.y) +\n"
        "            c10 * weight.x * (1.0 - weight.y) +\n"
        "            c01 * (1.0 - weight.x) * weight.y +\n"
        "            c11 * weight.x * weight.y;\n"
        "  } else {\n"
        "    ivec2 xy = ivec2(floor(uv * dvec2(size)));\n"
        "    if (wrap_s == WRAP_CLAMP)\n"
        "      xy.x = clamp(xy.x, 0, size.x - 1);\n"
        "    if (wrap_t == WRAP_CLAMP)\n"
        "      xy.y = clamp(xy.y, 0, size.y - 1);\n"
        "    texel = fetch_slot(xy, size);\n"
        "  }\n"
        "  dvec4 primary = dvec4(flat_shade != 0 ? flat_colour :\n"
        "                         smooth_colour);\n"
        "  dvec4 bytes;\n"
        "  if (environment_mode == 0) {\n"
        "    bytes = texel;\n"
        "  } else if (environment_mode == 1) {\n"
        "    bytes = primary * texel;\n"
        "  } else {\n"
        "    double alpha = texel.a;\n"
        "    bytes.rgb = (primary.rgb * (255.0 - alpha) * 255.0 +\n"
        "                 texel.rgb * alpha) / 255.0;\n"
        "    bytes.a = primary.a * 255.0;\n"
        "  }\n"
        "  uvec4 rounded = uvec4(floor(clamp(bytes, 0.0, 255.0) + 0.5));\n"
        "  pixel = vec4(rounded) / 255.0;\n"
        "}\n";
    GLuint vertex, fragment;
    GLint linked;
    char log[512];

    vertex = renderer_shader(renderer, GL_VERTEX_SHADER, vertex_source);
    if (!vertex) {
        return false;
    }
    fragment = renderer_shader(renderer, GL_FRAGMENT_SHADER, fragment_source);
    if (!fragment) {
        glDeleteShader(vertex);
        return false;
    }
    renderer->texture_program = glCreateProgram();
    glAttachShader(renderer->texture_program, vertex);
    glAttachShader(renderer->texture_program, fragment);
    glLinkProgram(renderer->texture_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    glGetProgramiv(renderer->texture_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        glGetProgramInfoLog(renderer->texture_program, sizeof(log), NULL, log);
        error_setg(&renderer->error, "SGI texture program failed: %s", log);
        return false;
    }
    renderer->texture_sampler = glGetUniformLocation(renderer->texture_program,
                                                      "image_slots");
    renderer->texture_mode = glGetUniformLocation(renderer->texture_program,
                                                   "environment_mode");
    renderer->texture_wrap_s = glGetUniformLocation(renderer->texture_program,
                                                     "wrap_s");
    renderer->texture_wrap_t = glGetUniformLocation(renderer->texture_program,
                                                     "wrap_t");
    renderer->texture_shade = glGetUniformLocation(renderer->texture_program,
                                                    "flat_shade");
    renderer->texture_linear = glGetUniformLocation(renderer->texture_program,
                                                     "linear_filter");
    renderer->texture_border = glGetUniformLocation(renderer->texture_program,
                                                     "border_bytes");
    if (renderer->texture_sampler < 0 || renderer->texture_mode < 0 ||
        renderer->texture_wrap_s < 0 || renderer->texture_wrap_t < 0 ||
        renderer->texture_shade < 0 || renderer->texture_linear < 0 ||
        renderer->texture_border < 0 ||
        renderer_gl_error(renderer, "texture program setup")) {
        if (!renderer->error) {
            error_setg(&renderer->error,
                       "SGI texture program is missing a uniform");
        }
        return false;
    }
    return true;
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
    GLint major, minor;
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
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (major > 4 || (major == 4 && minor >= 5)) {
        if (!renderer_gl_texture_program(renderer)) {
            renderer->texture_error = renderer->error;
            renderer->error = NULL;
        }
    } else {
        error_setg(&renderer->texture_error,
                   "SGI typed textures require OpenGL 4.5, host has %d.%d",
                   major, minor);
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
    GLuint texture = 0, sample = 0, framebuffer = 0, stencil = 0;
    GLenum error;

    if (!qemu_egl_display_lease_usable(renderer->lease)) {
        error_setg(&renderer->error, "SGI renderer EGL display is quarantined");
        return false;
    }
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0,
                 frame->texture.slots ? GL_RGBA8 : GL_RGBA16,
                 frame->width, frame->height,
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
    if (frame->blend) {
        glBlendEquation(frame->blend_equation);
        glBlendFunc(frame->blend_src, frame->blend_dst);
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
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
    if (frame->texture.slots) {
        glActiveTexture(GL_TEXTURE0);
        glGenTextures(1, &sample);
        glBindTexture(GL_TEXTURE_2D, sample);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI,
                     frame->texture.width, frame->texture.height, 0,
                     GL_RGBA_INTEGER, GL_UNSIGNED_INT,
                     frame->texture.slots);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glUseProgram(renderer->texture_program);
        glUniform1i(renderer->texture_sampler, 0);
        glUniform1i(renderer->texture_mode,
                    frame->texture.env_mode == GL_REPLACE ? 0 :
                    frame->texture.env_mode == GL_MODULATE ? 1 : 2);
        glUniform1i(renderer->texture_wrap_s, frame->texture.wrap_s);
        glUniform1i(renderer->texture_wrap_t, frame->texture.wrap_t);
        glUniform4dv(renderer->texture_border, 1,
                     frame->texture.border_bytes);
        glUniform1i(renderer->texture_shade,
                    frame->shade_model == GL_FLAT);
        glUniform1i(renderer->texture_linear, frame->texture.linear_filter);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xff);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    if (frame->clip_count) {
        /* Stencil 1 marks the clip union; one triangle draw changes it to 2. */
        glClearStencil(1);
        glEnable(GL_SCISSOR_TEST);
        for (unsigned i = 0; i < frame->clip_count; i++) {
            const SGIRendererClearRect *rect = &frame->clips[i];

            glScissor(rect->x0, rect->y0,
                      rect->x1 - rect->x0 + 1,
                      rect->y1 - rect->y0 + 1);
            glClear(GL_STENCIL_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
        glStencilFunc(GL_EQUAL, 1, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
    } else {
        glStencilFunc(GL_ALWAYS, 1, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    }
    glShadeModel(frame->shade_model);
    glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(frame->projection);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(frame->modelview);
    glBegin(frame->texture.slots ? GL_TRIANGLE_FAN : GL_TRIANGLES);
    for (uint32_t i = 0; i < frame->count; i++) {
        const float *vertex = frame->vertices +
                              i * SGI_RENDERER_FRAME_VERTEX_FLOATS;

        glColor4fv(vertex + 4);
        if (frame->texture.slots) {
            glTexCoord4fv(vertex + 8);
        }
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
    } else if (frame->clip_count) {
        size_t pixels = (size_t)frame->width * frame->height;

        for (size_t i = 0; i < pixels; i++) {
            if (frame->written[i] > 2) {
                error_setg(&renderer->error,
                           "SGI renderer native clip coverage is invalid");
                break;
            }
            frame->written[i] = frame->written[i] == 2;
        }
    }
out:
    glUseProgram(0);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xff);
    glStencilFunc(GL_ALWAYS, 0, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glDeleteRenderbuffers(1, &stencil);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    glDeleteTextures(1, &sample);
    return !renderer->error;
}

static bool renderer_gl_execute_clear(SGIRendererGL *renderer)
{
    SGIRendererClear *clear = &renderer->clear;
    GLuint texture = 0, scalar = 0, framebuffer = 0;
    GLbitfield buffers = 0;
    GLenum error;

    if (!qemu_egl_display_lease_usable(renderer->lease)) {
        error_setg(&renderer->error, "SGI renderer EGL display is quarantined");
        return false;
    }
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (clear->colour) {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16, clear->width, clear->height,
                     0, GL_RGBA, GL_UNSIGNED_SHORT, clear->pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, texture, 0);
        buffers |= GL_COLOR_BUFFER_BIT;
    }
    if (clear->depth || clear->stencil) {
        glGenTextures(1, &scalar);
        glBindTexture(GL_TEXTURE_2D, scalar);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8,
                     clear->width, clear->height, 0, GL_DEPTH_STENCIL,
                     GL_UNSIGNED_INT_24_8, clear->depth_stencil);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D, scalar, 0);
        if (clear->depth) {
            buffers |= GL_DEPTH_BUFFER_BIT;
        }
        if (clear->stencil) {
            buffers |= GL_STENCIL_BUFFER_BIT;
        }
    }
    glDrawBuffer(clear->colour ? GL_COLOR_ATTACHMENT0 : GL_NONE);
    glReadBuffer(clear->colour ? GL_COLOR_ATTACHMENT0 : GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        error_setg(&renderer->error,
                   "SGI renderer native clear framebuffer failed");
        goto out;
    }
    glDisable(GL_DITHER);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(!!(clear->colour_mask & 1),
                !!(clear->colour_mask & 2),
                !!(clear->colour_mask & 4),
                !!(clear->colour_mask & 8));
    glClearColor(clear->rgba[0], clear->rgba[1],
                 clear->rgba[2], clear->rgba[3]);
    glDepthMask(GL_TRUE);
    glClearDepth((double)clear->depth24 / 0xffffffu);
    glStencilMask(clear->stencil_mask);
    glClearStencil(clear->stencil8);
    for (unsigned i = 0; i < clear->rect_count; i++) {
        const SGIRendererClearRect *rect = &clear->rects[i];

        glScissor(rect->x0, rect->y0,
                  rect->x1 - rect->x0 + 1,
                  rect->y1 - rect->y0 + 1);
        glClear(buffers);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (clear->colour) {
        glReadPixels(0, 0, clear->width, clear->height, GL_RGBA,
                     GL_UNSIGNED_SHORT, clear->pixels);
    }
    if (clear->depth || clear->stencil) {
        glReadPixels(0, 0, clear->width, clear->height, GL_DEPTH_STENCIL,
                     GL_UNSIGNED_INT_24_8, clear->depth_stencil);
    }
    error = glGetError();
    if (error != GL_NO_ERROR) {
        error_setg(&renderer->error, "SGI renderer native clear failed: "
                   "GL error 0x%x", error);
    }
out:
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xff);
    glDepthMask(GL_TRUE);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    glDeleteTextures(1, &scalar);
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
            glDeleteProgram(renderer->texture_program);
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
        switch (renderer->work_kind) {
        case SGI_RENDERER_WORK_REQUEST:
            renderer->success = renderer_gl_execute(renderer);
            break;
        case SGI_RENDERER_WORK_FRAME:
            renderer->success = renderer_gl_execute_frame(renderer);
            break;
        case SGI_RENDERER_WORK_CLEAR:
            renderer->success = renderer_gl_execute_clear(renderer);
            break;
        default:
            g_assert_not_reached();
        }
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
    error_free(renderer->texture_error);
    qemu_cond_destroy(&renderer->condition);
    qemu_mutex_destroy(&renderer->mutex);
    g_free(renderer);
}

static void *renderer_gl_create(SGIRenderCreateResult *result, Error **errp)
{
    SGIRendererGL *renderer = g_try_new0(SGIRendererGL, 1);
    bool success;
    QemuEGLAcquireResult acquire_result;

    *result = SGI_RENDER_CREATE_HOST_ERROR;
    if (!renderer) {
        *result = SGI_RENDER_CREATE_OOM;
        error_setg(errp, "Cannot allocate SGI GL worker");
        return NULL;
    }
    renderer->lease = qemu_egl_display_acquire_surfaceless(
        &acquire_result, errp);
    if (!renderer->lease) {
        if (acquire_result == QEMU_EGL_ACQUIRE_OOM) {
            *result = SGI_RENDER_CREATE_OOM;
        } else if (acquire_result == QEMU_EGL_ACQUIRE_UNAVAILABLE) {
            *result = SGI_RENDER_CREATE_UNAVAILABLE;
        }
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
    *result = SGI_RENDER_CREATE_OK;
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
    uint32_t *texture = NULL;
    bool success;

    if (frame->texture.slots) {
        size_t bytes = (size_t)frame->texture.width * frame->texture.height *
                       4 * sizeof(*texture);

        texture = g_try_malloc(bytes);
        if (!texture) {
            error_setg(errp, "Cannot allocate SGI GL worker texture");
            return false;
        }
        memcpy(texture, frame->texture.slots, bytes);
    }
    qemu_mutex_lock(&renderer->mutex);
    renderer->frame = *frame;
    renderer->frame_texture_slots = texture;
    renderer->frame.texture.slots = texture;
    renderer->work_kind = SGI_RENDERER_WORK_FRAME;
    renderer->pending = true;
    qemu_cond_signal(&renderer->condition);
    while (renderer->pending) {
        qemu_cond_wait(&renderer->condition, &renderer->mutex);
    }
    success = renderer->success;
    g_clear_pointer(&renderer->frame_texture_slots, g_free);
    renderer->frame.texture.slots = NULL;
    if (!success) {
        error_propagate(errp, renderer->error);
        renderer->error = NULL;
    }
    qemu_mutex_unlock(&renderer->mutex);
    return success;
}

static bool renderer_gl_supports_texture(void *opaque, Error **errp)
{
    SGIRendererGL *renderer = opaque;

    if (!renderer->texture_program || renderer->texture_error) {
        error_setg(errp, "%s", renderer->texture_error ?
                   error_get_pretty(renderer->texture_error) :
                   "SGI typed texture program is unavailable");
        return false;
    }
    return true;
}

static bool renderer_gl_clear(void *opaque, SGIRendererClear *clear,
                              Error **errp)
{
    SGIRendererGL *renderer = opaque;
    bool success;

    qemu_mutex_lock(&renderer->mutex);
    renderer->clear = *clear;
    renderer->work_kind = SGI_RENDERER_WORK_CLEAR;
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
    .supports_texture = renderer_gl_supports_texture,
    .clear = renderer_gl_clear,
    .destroy = renderer_gl_destroy,
};

static void renderer_gl_register(void)
{
    sgi_renderer_register_gl(&renderer_gl_ops);
}
module_init(renderer_gl_register, MODULE_INIT_QOM);
