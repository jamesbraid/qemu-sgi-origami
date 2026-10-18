/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <math.h>
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

/* The uniforms the window and textured window programs share. */
typedef struct SGIWindowUniforms {
    GLint height, flat, x, y, z, inv_w, c0, c1, c2, steps;
} SGIWindowUniforms;

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
    SGIWindowUniforms texture_window;
    GLint texture_t0, texture_t1, texture_t2;
    GLint texture_sampler, texture_mode, texture_wrap_s;
    GLint texture_wrap_t, texture_linear;
    GLint texture_border, texture_environment;
    GLint max_viewport[2];
    GLfloat point_range[2];
    GLuint colour_program;
    GLint colour_steps, colour_flat;
    GLuint window_program;
    Error *window_error;
    SGIWindowUniforms window;
    GLuint line_program;
    Error *line_error;
    GLint line_ends, line_inv_w, line_colour_a, line_colour_b, line_steps;
    GLint line_range, line_x_major, line_reverse, line_counter, line_stipple;
    GLint line_repeat, line_pattern, line_flat;
    char *vendor, *renderer_name, *version;
    /* The cached frame target; see renderer_gl_frame_target(). */
    GLuint frame_texture, frame_framebuffer, frame_stencil;
    uint32_t frame_width, frame_height;
    GLuint vao;
    GLuint vbo;
    GLuint texture;
    GLuint framebuffer;
    GLuint sampled_texture;
    uint64_t texture_serial;
    bool texture_ready;
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

/*
 * Untextured frames: the fragment colour is clamped and rounded to the
 * frame's component steps before blending, as the scalar rasterizer
 * rounds a source colour before it blends.
 */
static bool renderer_gl_colour_program(SGIRendererGL *renderer)
{
    static const char *vertex_source =
        "#version 330 compatibility\n"
        "out vec4 smooth_colour;\n"
        "flat out vec4 flat_colour;\n"
        "void main() {\n"
        "  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
        "  smooth_colour = gl_Color; flat_colour = gl_Color;\n"
        "}\n";
    static const char *fragment_source =
        "#version 330 compatibility\n"
        "uniform vec4 steps;\n"
        "uniform int flat_shade;\n"
        "in vec4 smooth_colour;\n"
        "flat in vec4 flat_colour;\n"
        "layout(location=0) out vec4 pixel;\n"
        "void main() {\n"
        "  vec4 c = clamp(flat_shade != 0 ? flat_colour : smooth_colour,\n"
        "                 0.0, 1.0);\n"
        "  for (int k = 0; k < 4; k++) {\n"
        "    if (steps[k] > 0.0) c[k] = floor(c[k] * steps[k] + 0.5) /\n"
        "                              steps[k];\n"
        "  }\n"
        "  pixel = c;\n"
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
    renderer->colour_program = glCreateProgram();
    glAttachShader(renderer->colour_program, vertex);
    glAttachShader(renderer->colour_program, fragment);
    glLinkProgram(renderer->colour_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    glGetProgramiv(renderer->colour_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        glGetProgramInfoLog(renderer->colour_program, sizeof(log), NULL, log);
        error_setg(&renderer->error, "SGI colour program failed: %s", log);
        return false;
    }
    renderer->colour_steps = glGetUniformLocation(renderer->colour_program,
                                                  "steps");
    renderer->colour_flat = glGetUniformLocation(renderer->colour_program,
                                                 "flat_shade");
    if (renderer->colour_steps < 0 || renderer->colour_flat < 0) {
        error_setg(&renderer->error, "SGI colour program is missing a uniform");
        return false;
    }
    return true;
}

/*
 * Window triangle frames: each triangle arrives at the scalar rasterizer's
 * snapped window positions, mapped through a power-of-two viewport and
 * turned upside down as triangle frames are, so GL covers exactly the
 * scalar path's pixels. The fragment program recomputes the scalar path's
 * integer edge functions, barycentric weights, depth and 1 / w correction
 * in double, in its order of operations, drops the fragments whose depth
 * it drops, and rounds to the native steps.
 */
#define RENDERER_WINDOW_VERTEX \
    "#version 450 compatibility\n" \
    "uniform float height;\n" \
    "void main() {\n" \
    "  vec2 p = vec2(gl_Vertex.x, height - gl_Vertex.y);\n" \
    "  gl_Position = vec4((p + " \
    G_STRINGIFY(SGI_RENDERER_WINDOW_REACH) ".0) / " \
    G_STRINGIFY(SGI_RENDERER_WINDOW_REACH) ".0 * 0.5 - 1.0, 0.0, 1.0);\n" \
    "}\n"

#define RENDERER_WINDOW_FRAGMENT_DECLS \
    "#version 450 compatibility\n" \
    "uniform float height;\n" \
    "uniform int flat_shade;\n" \
    "uniform dvec3 vx, vy, vz, inv_w;\n" \
    "uniform dvec4 c0, c1, c2, steps;\n" \
    "layout(location=0) out vec4 pixel;\n"

/* main() up to the weights w0, w1 and w2, their sum and the colour. */
#define RENDERER_WINDOW_FRAGMENT_MAIN \
    "double channel(double c, double s) {\n" \
    "  if (s <= 0.0) return c;\n" \
    "  if (!(c > 0.0)) return 0.0;\n" \
    "  if (c >= 1.0) return 1.0;\n" \
    "  return floor(c * s + 0.5) / s;\n" \
    "}\n" \
    "double edge(int a, int b, double px, double py) {\n" \
    "  precise double e = (vx[b] - vx[a]) * (py - vy[a]) -\n" \
    "                     (vy[b] - vy[a]) * (px - vx[a]);\n" \
    "  return e;\n" \
    "}\n" \
    "void main() {\n" \
    "  int px = int(floor(gl_FragCoord.x));\n" \
    "  int py = int(height) - 1 - int(floor(gl_FragCoord.y));\n" \
    "  double cx = double(px) * 256.0 + 128.0;\n" \
    "  double cy = double(py) * 256.0 + 128.0;\n" \
    "  precise double area = edge(0, 1, vx[2], vy[2]);\n" \
    "  precise double b1 = edge(2, 0, cx, cy) / area;\n" \
    "  precise double b2 = edge(0, 1, cx, cy) / area;\n" \
    "  precise double z = vz[0] + b1 * (vz[1] - vz[0]) +\n" \
    "                     b2 * (vz[2] - vz[0]);\n" \
    "  if (!(z >= 0.0 && z <= 1.0)) discard;\n" \
    "  precise double w0 = (1.0 - b1 - b2) * inv_w[0];\n" \
    "  precise double w1 = b1 * inv_w[1];\n" \
    "  precise double w2 = b2 * inv_w[2];\n" \
    "  precise double sum = w0 + w1 + w2;\n" \
    "  precise dvec4 rgba = flat_shade != 0 ? c0 :\n" \
    "      (w0 * c0 + w1 * c1 + w2 * c2) / sum;\n"

#define RENDERER_WINDOW_FRAGMENT_TAIL \
    "  pixel = vec4(channel(rgba.r, steps.r), channel(rgba.g, steps.g),\n" \
    "               channel(rgba.b, steps.b), channel(rgba.a, steps.a));\n" \
    "}\n"

static GLuint renderer_gl_link(SGIRendererGL *renderer, const char *name,
                               const char *vertex_source,
                               const char *fragment_source)
{
    GLuint vertex, fragment, program;
    GLint linked;
    char log[512];

    vertex = renderer_shader(renderer, GL_VERTEX_SHADER, vertex_source);
    if (!vertex) {
        return 0;
    }
    fragment = renderer_shader(renderer, GL_FRAGMENT_SHADER, fragment_source);
    if (!fragment) {
        glDeleteShader(vertex);
        return 0;
    }
    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        error_setg(&renderer->error, "SGI %s program failed: %s", name, log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static bool renderer_gl_uniform(SGIRendererGL *renderer, GLuint program,
                                const char *program_name, const char *name,
                                GLint *location)
{
    *location = glGetUniformLocation(program, name);
    if (*location < 0) {
        error_setg(&renderer->error, "SGI %s program is missing uniform %s",
                   program_name, name);
        return false;
    }
    return true;
}

static bool renderer_gl_window_uniforms(SGIRendererGL *renderer,
                                        GLuint program, const char *name,
                                        SGIWindowUniforms *u)
{
    return renderer_gl_uniform(renderer, program, name, "height",
                               &u->height) &&
           renderer_gl_uniform(renderer, program, name, "flat_shade",
                               &u->flat) &&
           renderer_gl_uniform(renderer, program, name, "vx", &u->x) &&
           renderer_gl_uniform(renderer, program, name, "vy", &u->y) &&
           renderer_gl_uniform(renderer, program, name, "vz", &u->z) &&
           renderer_gl_uniform(renderer, program, name, "inv_w",
                               &u->inv_w) &&
           renderer_gl_uniform(renderer, program, name, "c0", &u->c0) &&
           renderer_gl_uniform(renderer, program, name, "c1", &u->c1) &&
           renderer_gl_uniform(renderer, program, name, "c2", &u->c2) &&
           renderer_gl_uniform(renderer, program, name, "steps", &u->steps);
}

static bool renderer_gl_window_program(SGIRendererGL *renderer)
{
    renderer->window_program = renderer_gl_link(renderer, "window",
        RENDERER_WINDOW_VERTEX,
        RENDERER_WINDOW_FRAGMENT_DECLS RENDERER_WINDOW_FRAGMENT_MAIN
        RENDERER_WINDOW_FRAGMENT_TAIL);
    return renderer->window_program &&
           renderer_gl_window_uniforms(renderer, renderer->window_program,
                                       "window", &renderer->window) &&
           !renderer_gl_error(renderer, "window program setup");
}

/*
 * Textured window triangles: the window program's weights also give the
 * texture coordinates, in double, as the scalar path interpolates them.
 * The program then samples level zero exactly as the scalar path does:
 * it reduces each coordinate by its wrap mode, takes the nearest texel or
 * the four linear taps with their double weights in the scalar order,
 * reads taps outside the image as the border colour, normalizes texels by
 * 255, and applies the environment to the primary colour in [0, 1].
 * Texture sizes are powers of two, so a mask wraps a repeated index.
 */
static bool renderer_gl_texture_program(SGIRendererGL *renderer)
{
    static const char *fragment_source =
        RENDERER_WINDOW_FRAGMENT_DECLS
        "#define WRAP_REPEAT " G_STRINGIFY(GL_REPEAT) "\n"
        "#define WRAP_CLAMP " G_STRINGIFY(GL_CLAMP) "\n"
        "#define WRAP_EDGE " G_STRINGIFY(GL_CLAMP_TO_EDGE) "\n"
        "#define WRAP_BORDER " G_STRINGIFY(GL_CLAMP_TO_BORDER) "\n"
        "uniform dvec4 t0, t1, t2;\n"
        "uniform usampler2D image_slots;\n"
        "uniform int environment_mode, wrap_s, wrap_t, linear_filter;\n"
        "uniform dvec4 border, environment;\n"
        "double reduce(double x, int n, int wrap) {\n"
        "  precise double half_texel = 0.5 / double(n);\n"
        "  if (wrap == WRAP_REPEAT) return x - floor(x);\n"
        "  if (wrap == WRAP_EDGE)\n"
        "    return min(max(x, half_texel), 1.0 - half_texel);\n"
        "  if (wrap == WRAP_BORDER)\n"
        "    return min(max(x, -half_texel), 1.0 + half_texel);\n"
        "  return min(max(x, 0.0), 1.0);\n"
        "}\n"
        "dvec4 tap(int x, int y, ivec2 size) {\n"
        "  if (wrap_s == WRAP_REPEAT) x = x & (size.x - 1);\n"
        "  if (wrap_t == WRAP_REPEAT) y = y & (size.y - 1);\n"
        "  if (x < 0 || y < 0 || x >= size.x || y >= size.y) return border;\n"
        "  return dvec4(texelFetch(image_slots, ivec2(x, y), 0)) / 255.0;\n"
        "}\n"
        RENDERER_WINDOW_FRAGMENT_MAIN
        "  precise dvec4 tc = (w0 * t0 + w1 * t1 + w2 * t2) / sum;\n"
        "  if (!(tc.q > 0.0) || isinf(tc.q)) discard;\n"
        "  precise double s = tc.s / tc.q;\n"
        "  precise double t = tc.t / tc.q;\n"
        "  if (isinf(s) || isnan(s) || isinf(t) || isnan(t)) discard;\n"
        "  ivec2 size = textureSize(image_slots, 0);\n"
        "  s = reduce(s, size.x, wrap_s);\n"
        "  t = reduce(t, size.y, wrap_t);\n"
        "  dvec4 texel = dvec4(0.0);\n"
        "  if (linear_filter == 0) {\n"
        "    int x = int(floor(double(size.x) * s));\n"
        "    int y = int(floor(double(size.y) * t));\n"
        "    if (s == 1.0 && wrap_s != WRAP_BORDER) x = size.x - 1;\n"
        "    if (t == 1.0 && wrap_t != WRAP_BORDER) y = size.y - 1;\n"
        "    texel = tap(x, y, size);\n"
        "  } else {\n"
        "    precise double u = double(size.x) * s - 0.5;\n"
        "    precise double v = double(size.y) * t - 0.5;\n"
        "    int x = int(floor(u)), y = int(floor(v));\n"
        "    precise double a = u - double(x), b = v - double(y);\n"
        "    for (int j = 0; j < 2; j++) {\n"
        "      for (int i = 0; i < 2; i++) {\n"
        "        precise double weight = (i != 0 ? a : 1.0 - a) *\n"
        "                                (j != 0 ? b : 1.0 - b);\n"
        "        if (weight != 0.0) {\n"
        "          precise dvec4 term = weight * tap(x + i, y + j, size);\n"
        "          texel = texel + term;\n"
        "        }\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "  if (environment_mode == 0) {\n"
        "    rgba = texel;\n"
        "  } else if (environment_mode == 1) {\n"
        "    rgba = rgba * texel;\n"
        "  } else if (environment_mode == 2) {\n"
        "    rgba.rgb = rgba.rgb * (1.0 - texel.a) + texel.rgb * texel.a;\n"
        "  } else {\n"
        "    rgba.rgb = rgba.rgb * (1.0 - texel.rgb) +\n"
        "               environment.rgb * texel.rgb;\n"
        "    rgba.a = rgba.a * texel.a;\n"
        "  }\n"
        RENDERER_WINDOW_FRAGMENT_TAIL;
    renderer->texture_program = renderer_gl_link(renderer, "texture",
        RENDERER_WINDOW_VERTEX, fragment_source);
    if (!renderer->texture_program ||
        !renderer_gl_window_uniforms(renderer, renderer->texture_program,
                                     "texture", &renderer->texture_window)) {
        return false;
    }
    return renderer_gl_uniform(renderer, renderer->texture_program,
                               "texture", "t0", &renderer->texture_t0) &&
           renderer_gl_uniform(renderer, renderer->texture_program,
                               "texture", "t1", &renderer->texture_t1) &&
           renderer_gl_uniform(renderer, renderer->texture_program,
                               "texture", "t2", &renderer->texture_t2) &&
           renderer_gl_uniform(renderer, renderer->texture_program,
                               "texture", "image_slots",
                               &renderer->texture_sampler) &&
           renderer_gl_uniform(renderer, renderer->texture_program,
                               "texture", "environment_mode",
                               &renderer->texture_mode) &&
           renderer_gl_uniform(renderer, renderer->texture_program,
                               "texture", "wrap_s",
                               &renderer->texture_wrap_s) &&
           renderer_gl_uniform(renderer, renderer->texture_program,
                               "texture", "wrap_t",
                               &renderer->texture_wrap_t) &&
           renderer_gl_uniform(renderer, renderer->texture_program,
                               "texture", "linear_filter",
                               &renderer->texture_linear) &&
           renderer_gl_uniform(renderer, renderer->texture_program,
                               "texture", "border",
                               &renderer->texture_border) &&
           renderer_gl_uniform(renderer, renderer->texture_program,
                               "texture", "environment",
                               &renderer->texture_environment) &&
           !renderer_gl_error(renderer, "texture program setup");
}

/*
 * Line frames: each segment is a parallelogram in window pixels mapped
 * through a power-of-two viewport, so its snapped vertices arrive exact.
 * The fragment keeps only major-axis steps lo to hi, applies the stipple
 * bit of its base pixel, and computes its colour in double from its
 * centre's clamped parameter along the segment, exactly as the scalar
 * rasterizer does, before rounding it to the native steps.
 */
static bool renderer_gl_line_program(SGIRendererGL *renderer)
{
    static const char *vertex_source =
        "#version 450 compatibility\n"
        "void main() {\n"
        "  gl_Position = vec4((gl_Vertex.xy + "
        G_STRINGIFY(SGI_RENDERER_LINE_REACH) ".0) / "
        G_STRINGIFY(SGI_RENDERER_LINE_REACH) ".0 * 0.5 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char *fragment_source =
        "#version 450 compatibility\n"
        "uniform dvec4 ends, colour_a, colour_b, steps;\n"
        "uniform dvec2 inv_w;\n"
        "uniform ivec2 range;\n"
        "uniform int x_major, reverse_dir, counter, stipple, repeat_;\n"
        "uniform int pattern, flat_shade;\n"
        "layout(location=0) out vec4 pixel;\n"
        "double channel(double c, double s) {\n"
        "  if (s <= 0.0) return c;\n"
        "  if (!(c > 0.0)) return 0.0;\n"
        "  if (c >= 1.0) return 1.0;\n"
        "  return floor(c * s + 0.5) / s;\n"
        "}\n"
        "void main() {\n"
        "  ivec2 p = ivec2(floor(gl_FragCoord.xy));\n"
        "  int c = x_major != 0 ? p.x : p.y;\n"
        "  if (c < range.x || c > range.y) discard;\n"
        "  if (stipple != 0) {\n"
        "    int i = reverse_dir != 0 ? range.y - c : c - range.x;\n"
        "    int bit = ((counter + i) / repeat_) % 16;\n"
        "    if (((pattern >> bit) & 1) == 0) discard;\n"
        "  }\n"
        "  dvec4 rgba = colour_b;\n"
        "  if (flat_shade == 0) {\n"
        "    precise double cx = (double(p.x) + 0.5) * 256.0;\n"
        "    precise double cy = (double(p.y) + 0.5) * 256.0;\n"
        "    precise double dx = ends.z - ends.x;\n"
        "    precise double dy = ends.w - ends.y;\n"
        "    precise double t = ((cx - ends.x) * dx + (cy - ends.y) * dy) /\n"
        "                       (dx * dx + dy * dy);\n"
        "    t = min(max(t, 0.0lf), 1.0lf);\n"
        "    precise double wa = (1.0 - t) * inv_w.x;\n"
        "    precise double wb = t * inv_w.y;\n"
        "    precise dvec4 mixed = (wa * colour_a + wb * colour_b) /\n"
        "                          (wa + wb);\n"
        "    rgba = mixed;\n"
        "  }\n"
        "  pixel = vec4(channel(rgba.r, steps.r), channel(rgba.g, steps.g),\n"
        "               channel(rgba.b, steps.b), channel(rgba.a, steps.a));\n"
        "}\n";
    GLuint vertex, fragment;
    GLint linked;
    char log[512];
    struct {
        GLint *location;
        const char *name;
    } uniforms[] = {
        { &renderer->line_ends, "ends" },
        { &renderer->line_inv_w, "inv_w" },
        { &renderer->line_colour_a, "colour_a" },
        { &renderer->line_colour_b, "colour_b" },
        { &renderer->line_steps, "steps" },
        { &renderer->line_range, "range" },
        { &renderer->line_x_major, "x_major" },
        { &renderer->line_reverse, "reverse_dir" },
        { &renderer->line_counter, "counter" },
        { &renderer->line_stipple, "stipple" },
        { &renderer->line_repeat, "repeat_" },
        { &renderer->line_pattern, "pattern" },
        { &renderer->line_flat, "flat_shade" },
    };

    vertex = renderer_shader(renderer, GL_VERTEX_SHADER, vertex_source);
    if (!vertex) {
        return false;
    }
    fragment = renderer_shader(renderer, GL_FRAGMENT_SHADER, fragment_source);
    if (!fragment) {
        glDeleteShader(vertex);
        return false;
    }
    renderer->line_program = glCreateProgram();
    glAttachShader(renderer->line_program, vertex);
    glAttachShader(renderer->line_program, fragment);
    glLinkProgram(renderer->line_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    glGetProgramiv(renderer->line_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        glGetProgramInfoLog(renderer->line_program, sizeof(log), NULL, log);
        error_setg(&renderer->error, "SGI line program failed: %s", log);
        return false;
    }
    for (size_t i = 0; i < ARRAY_SIZE(uniforms); i++) {
        *uniforms[i].location = glGetUniformLocation(renderer->line_program,
                                                     uniforms[i].name);
        if (*uniforms[i].location < 0) {
            error_setg(&renderer->error,
                       "SGI line program is missing uniform %s",
                       uniforms[i].name);
            return false;
        }
    }
    return !renderer_gl_error(renderer, "line program setup");
}

static bool renderer_gl_start(SGIRendererGL *renderer)
{
    static const char *vertex_source =
        "#version 330 core\n"
        "layout(location=0) in vec4 vertex;\n"
        "layout(location=1) in vec4 projected_colour;\n"
        "layout(location=2) in float reciprocal_w;\n"
        "layout(location=3) in vec4 projected_texcoord;\n"
        "layout(location=4) in vec4 projected_clip0;\n"
        "layout(location=5) in vec2 projected_clip1;\n"
        "out vec4 clip0; out vec2 clip1;\n"
        "out vec4 interpolated_colour; out float inverse_w;\n"
        "out vec4 texcoord;\n"
        "void main() { gl_Position = vec4(vertex.xy / 64.0 - 1.0,"
        "vertex.z, vertex.w); interpolated_colour = projected_colour;"
        "inverse_w = reciprocal_w; texcoord = projected_texcoord;"
        "clip0 = projected_clip0; clip1 = projected_clip1; }\n";
    static const char *fragment_source =
        "#version 330 core\n"
        "uniform vec4 colour; uniform bool smooth_shading;\n"
        "uniform bool textured; uniform sampler2D image;\n"
        "uniform int clip_planes; in vec4 clip0; in vec2 clip1;\n"
        "uniform int texture_format, environment; uniform ivec2 wraps;\n"
        "uniform bool linear_filter;\n"
        "uniform vec4 border_colour, env_colour, post_scale, post_bias;\n"
        "in vec4 interpolated_colour; in float inverse_w;\n"
        "in vec4 texcoord; layout(location=0) out vec4 pixel;\n"
        "vec4 components(vec4 t) {\n"
        "  if (texture_format == 0) t.rgb = vec3(1.0);\n"
        "  else if (texture_format == 1 || texture_format == 2) t.gb = t.rr;\n"
        "  else if (texture_format == 3) t = t.rrrr;\n"
        "  if (texture_format == 1 || texture_format == 4) t.a = 1.0;\n"
        "  return t; }\n"
        "float axis(float c, int n, int wrap) {\n"
        "  float half_texel = 0.5 / float(n);\n"
        "  if (wrap == 0) return c - floor(c);\n"
        "  if (wrap == 2) return clamp(c, half_texel, 1.0 - half_texel);\n"
        "  if (wrap == 3) return clamp(c, -half_texel, 1.0 + half_texel);\n"
        "  return clamp(c, 0.0, 1.0); }\n"
        "vec4 tap(ivec2 p, ivec2 size) {\n"
        "  for (int k = 0; k < 2; k++) {\n"
        "    if (wraps[k] == 0) p[k] = (p[k] % size[k] + size[k]) % size[k];\n"
        "    else if (p[k] < 0 || p[k] >= size[k])\n"
        "      return components(border_colour); }\n"
        "  return components(texelFetch(image, p, 0)); }\n"
        "vec4 sample_image(vec2 uv) {\n"
        "  ivec2 size = textureSize(image, 0);\n"
        "  uv = vec2(axis(uv.x, size.x, wraps.x),"
        "axis(uv.y, size.y, wraps.y));\n"
        "  if (!linear_filter) {\n"
        "    ivec2 p = ivec2(floor(uv * vec2(size)));\n"
        "    for (int k = 0; k < 2; k++)\n"
        "      if (uv[k] == 1.0 && wraps[k] != 3) p[k] = size[k] - 1;\n"
        "    return tap(p, size); }\n"
        "  vec2 v = uv * vec2(size) - 0.5; ivec2 p = ivec2(floor(v));\n"
        "  vec2 a = fract(v); vec4 t = vec4(0.0);\n"
        "  for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) {\n"
        "    float weight = (x == 1 ? a.x : 1.0 - a.x) *"
        "(y == 1 ? a.y : 1.0 - a.y);\n"
        "    if (weight != 0.0) t += weight * tap(p + ivec2(x,y), size); }\n"
        "  return t; }\n"
        "vec4 apply_environment(vec4 c, vec4 t) {\n"
        "  for (int k = 0; k < 4; k++) {\n"
        "    bool alpha = k == 3;\n"
        "    if ((!alpha && texture_format == 0) ||"
        "(alpha && (texture_format == 1 || texture_format == 4))) continue;\n"
        "    if (environment == 0) c[k] = t[k];\n"
        "    else if (environment == 1) c[k] *= t[k];\n"
        "    else if (environment == 2) { if (!alpha) c[k] ="
        "texture_format == 4 ? t[k] : c[k] * (1.0 - t.a) + t[k] * t.a; }\n"
        "    else c[k] = alpha && texture_format != 3 ? c[k] * t[k] :"
        "c[k] * (1.0 - t[k]) + env_colour[k] * t[k]; }\n"
        "  return c; }\n"
        "void main() { for (int p = 0; p < 6; p++) {"
        "float d = p < 4 ? clip0[p] : clip1[p - 4];"
        "if ((clip_planes & (1 << p)) != 0 && !(d / inverse_w >= 0.0))"
        "discard; } vec4 c = smooth_shading ? "
        "interpolated_colour / inverse_w : colour;\n"
        "  if (textured) { vec4 t = sample_image(texcoord.xy / texcoord.w);"
        "t = clamp(t * post_scale + post_bias, 0.0, 1.0);"
        "c = apply_environment(c, t); } "
        "pixel = vec4(floor(clamp(c.rgb, 0.0, 1.0) * 255.0 + 0.5) / "
        "255.0, 1.0); }\n";
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
    renderer->vendor = g_strdup((const char *)glGetString(GL_VENDOR));
    renderer->renderer_name =
        g_strdup((const char *)glGetString(GL_RENDERER));
    renderer->version = g_strdup((const char *)glGetString(GL_VERSION));
    info_report("SGI renderer GL vendor=%s renderer=%s version=%s",
                renderer->vendor, renderer->renderer_name, renderer->version);
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
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 19 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 19 * sizeof(float),
                            (void *)(4 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 19 * sizeof(float),
                            (void *)(8 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 19 * sizeof(float),
                            (void *)(9 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, 19 * sizeof(float),
                            (void *)(13 * sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, 19 * sizeof(float),
                            (void *)(17 * sizeof(float)));
    glEnableVertexAttribArray(5);
    glGenTextures(1, &renderer->sampled_texture);
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
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, renderer->max_viewport);
    glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, renderer->point_range);
    if (!renderer_gl_colour_program(renderer)) {
        return false;
    }
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    /*
     * A host too old for these programs is a host limit, and the draws
     * that need them are refused. A program that fails to build on a host
     * new enough is a setup failure, so creation fails, rather than a
     * refusal that would send every such draw to the scalar path.
     */
    if (major > 4 || (major == 4 && minor >= 5)) {
        if (!renderer_gl_texture_program(renderer) ||
            !renderer_gl_window_program(renderer) ||
            !renderer_gl_line_program(renderer)) {
            return false;
        }
    } else {
        error_setg(&renderer->texture_error,
                   "SGI typed textures require OpenGL 4.5, host has %d.%d",
                   major, minor);
        error_setg(&renderer->window_error,
                   "SGI smooth triangles require OpenGL 4.5, host has %d.%d",
                   major, minor);
        error_setg(&renderer->line_error,
                   "SGI lines require OpenGL 4.5, host has %d.%d",
                   major, minor);
    }
    return !renderer_gl_error(renderer, "startup");
}

static bool renderer_gl_texture(SGIRendererGL *renderer,
                                  const SGIRendererTexture *t)
{
    float scale[4], bias[4];
    GLint maximum;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, renderer->sampled_texture);
    if (!t) {
        return true;
    }
    if (!renderer->texture_ready || renderer->texture_serial != t->serial) {
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
        if (t->width > maximum || t->height > maximum) {
            error_setg(&renderer->error,
                       "SGI texture exceeds host texture size %d", maximum);
            return false;
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
        glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
        glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->width, t->height, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, t->texels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        renderer->texture_serial = t->serial;
        renderer->texture_ready = true;
    }
    for (unsigned c = 0; c < 4; c++) {
        scale[c] = t->scale[c];
        bias[c] = t->bias[c];
        if (!isfinite(scale[c]) || !isfinite(bias[c])) {
            error_setg(&renderer->error,
                       "SGI texture post-filter state exceeds host precision");
            return false;
        }
    }
    glUniform1i(glGetUniformLocation(renderer->program, "image"), 0);
    glUniform1i(glGetUniformLocation(renderer->program, "texture_format"),
                 t->format);
    glUniform1i(glGetUniformLocation(renderer->program, "environment"),
                 t->environment);
    glUniform2i(glGetUniformLocation(renderer->program, "wraps"),
                 t->wrap[0], t->wrap[1]);
    glUniform1i(glGetUniformLocation(renderer->program, "linear_filter"),
                 t->linear);
    glUniform4fv(glGetUniformLocation(renderer->program, "border_colour"),
                  1, t->border);
    glUniform4fv(glGetUniformLocation(renderer->program, "env_colour"),
                  1, t->env_colour);
    glUniform4fv(glGetUniformLocation(renderer->program, "post_scale"),
                  1, scale);
    glUniform4fv(glGetUniformLocation(renderer->program, "post_bias"),
                  1, bias);
    return !renderer_gl_error(renderer, "texture upload");
}

static bool renderer_gl_execute(SGIRendererGL *renderer)
{
    SGIRendererRequest *request = &renderer->request;
    float vertices[3][19] = { 0 };
    double scale = MAX(request->inverse_w[0],
                        MAX(request->inverse_w[1], request->inverse_w[2]));
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
    for (unsigned i = 0; i < 3; i++) {
        memcpy(vertices[i], request->vertices[i], sizeof(request->vertices[i]));
        if (request->smooth || request->texture || request->clip_planes) {
            float inverse_w = request->inverse_w[i] / scale;

            if (!isfinite(inverse_w) || inverse_w <= 0) {
                error_setg(&renderer->error,
                           "SGI renderer reciprocal w is outside host precision");
                return false;
            }
            for (unsigned c = 0; c < 4; c++) {
                vertices[i][4 + c] = request->colour[i][c] * inverse_w;
                if (request->texture) {
                    vertices[i][9 + c] = request->texcoord[i][c] * inverse_w;
                    if (!isfinite(vertices[i][9 + c])) {
                        error_setg(&renderer->error,
                                   "SGI texture coordinates exceed host "
                                   "precision");
                        return false;
                    }
                }
            }
            for (unsigned p = 0; p < SGI_RENDERER_CLIP_PLANES; p++) {
                if (request->clip_planes & (1u << p)) {
                    vertices[i][13 + p] = request->clip_distance[i][p] *
                                          inverse_w;
                    if (!isfinite(vertices[i][13 + p])) {
                        error_setg(&renderer->error,
                                   "SGI clip distance exceeds host precision");
                        return false;
                    }
                }
            }
            vertices[i][8] = inverse_w;
            if (request->texture && vertices[i][12] <= 0) {
                error_setg(&renderer->error,
                           "SGI texture q is outside host precision");
                return false;
            }
        }
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
    if (request->texture) {
        glUniform4f(glGetUniformLocation(renderer->program, "colour"),
                     request->colour[0][0], request->colour[0][1],
                     request->colour[0][2], request->colour[0][3]);
    } else {
        glUniform4f(glGetUniformLocation(renderer->program, "colour"),
                     request->rgb[0] / 255.0f, request->rgb[1] / 255.0f,
                     request->rgb[2] / 255.0f, request->alpha);
    }
    glUniform1i(glGetUniformLocation(renderer->program, "smooth_shading"),
                 request->smooth);
    glUniform1i(glGetUniformLocation(renderer->program, "clip_planes"),
                 request->clip_planes);
    glUniform1i(glGetUniformLocation(renderer->program, "textured"),
                 request->texture != NULL);
    if (!renderer_gl_texture(renderer, request->texture)) {
        return false;
    }
    glBindVertexArray(renderer->vao);
    glBindBuffer(GL_ARRAY_BUFFER, renderer->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
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

/* Drop the cached frame target, after a failure or for a new size. */
static void renderer_gl_frame_target_free(SGIRendererGL *renderer)
{
    glDeleteRenderbuffers(1, &renderer->frame_stencil);
    glDeleteFramebuffers(1, &renderer->frame_framebuffer);
    glDeleteTextures(1, &renderer->frame_texture);
    renderer->frame_stencil = 0;
    renderer->frame_framebuffer = 0;
    renderer->frame_texture = 0;
    renderer->frame_width = renderer->frame_height = 0;
}

/*
 * The float colour texture, stencil and framebuffer a frame draws into,
 * kept for the next frame of the same size: every frame uploads, clears
 * and reads back the region it uses, so nothing earlier shows through.
 */
static bool renderer_gl_frame_target(SGIRendererGL *renderer,
                                     uint32_t width, uint32_t height)
{
    if (renderer->frame_framebuffer && renderer->frame_width == width &&
        renderer->frame_height == height) {
        glBindFramebuffer(GL_FRAMEBUFFER, renderer->frame_framebuffer);
        glBindTexture(GL_TEXTURE_2D, renderer->frame_texture);
        return true;
    }
    renderer_gl_frame_target_free(renderer);
    glGenTextures(1, &renderer->frame_texture);
    glBindTexture(GL_TEXTURE_2D, renderer->frame_texture);
    /*
     * Float storage keeps blending free of intermediate fixed point
     * rounding; readback clamps as a native store does.
     */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_SHORT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &renderer->frame_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, renderer->frame_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, renderer->frame_texture, 0);
    glGenRenderbuffers(1, &renderer->frame_stencil);
    glBindRenderbuffer(GL_RENDERBUFFER, renderer->frame_stencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, renderer->frame_stencil);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        error_setg(&renderer->error, "SGI renderer native framebuffer failed");
        renderer_gl_frame_target_free(renderer);
        return false;
    }
    renderer->frame_width = width;
    renderer->frame_height = height;
    return true;
}

static bool renderer_gl_execute_frame(SGIRendererGL *renderer)
{
    SGIRendererFrame *frame = &renderer->frame;
    GLuint sample = 0;
    const SGIWindowUniforms *window = frame->texture.slots ?
                                      &renderer->texture_window :
                                      &renderer->window;
    /* The region the frame uploads, draws and reads back. */
    SGIRendererClearRect region = frame->region_given ? frame->region :
        (SGIRendererClearRect) { 0, 0, frame->width - 1, frame->height - 1 };
    GLsizei region_width = region.x1 - region.x0 + 1;
    GLsizei region_height = region.y1 - region.y0 + 1;
    GLenum error;

    if (!qemu_egl_display_lease_usable(renderer->lease)) {
        error_setg(&renderer->error, "SGI renderer EGL display is quarantined");
        return false;
    }
    if (!renderer_gl_frame_target(renderer, frame->width, frame->height)) {
        goto out;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, region.x0, region.y0, region_width,
                    region_height, GL_RGBA, GL_UNSIGNED_SHORT, frame->rgba);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    if (frame->primitive == SGI_RENDERER_WINDOW_TRIANGLES) {
        /* The window program maps window pixels through this viewport. */
        glViewport(-SGI_RENDERER_WINDOW_REACH, -SGI_RENDERER_WINDOW_REACH,
                   4 * SGI_RENDERER_WINDOW_REACH,
                   4 * SGI_RENDERER_WINDOW_REACH);
    } else if (frame->primitive == SGI_RENDERER_LINES) {
        /* The line program maps window pixels through this viewport. */
        glViewport(-SGI_RENDERER_LINE_REACH, -SGI_RENDERER_LINE_REACH,
                   4 * SGI_RENDERER_LINE_REACH, 4 * SGI_RENDERER_LINE_REACH);
    } else if (frame->viewport_given) {
        /* sgi_renderer_draw_frame() has checked the host viewport limit. */
        glViewport(frame->viewport[0], frame->viewport[1],
                   frame->viewport[2], frame->viewport[3]);
    } else {
        glViewport(0, 0, frame->width, frame->height);
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glPointSize(frame->point_size ? frame->point_size : 1);
    glDisable(GL_POINT_SMOOTH);
    glDisable(GL_POINT_SPRITE);
    glDisable(GL_PROGRAM_POINT_SIZE);
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
                    frame->texture.env_mode == GL_MODULATE ? 1 :
                    frame->texture.env_mode == GL_DECAL ? 2 : 3);
        glUniform1i(renderer->texture_wrap_s, frame->texture.wrap_s);
        glUniform1i(renderer->texture_wrap_t, frame->texture.wrap_t);
        glUniform4dv(renderer->texture_border, 1, frame->texture.border);
        glUniform4dv(renderer->texture_environment, 1,
                     frame->texture.environment);
        glUniform1i(renderer->texture_linear, frame->texture.linear_filter);
    } else if (frame->primitive == SGI_RENDERER_WINDOW_TRIANGLES) {
        glUseProgram(renderer->window_program);
    } else if (frame->primitive == SGI_RENDERER_LINES) {
        glUseProgram(renderer->line_program);
        glUniform4d(renderer->line_steps, frame->colour_steps[0],
                    frame->colour_steps[1], frame->colour_steps[2],
                    frame->colour_steps[3]);
        glUniform1i(renderer->line_flat, frame->shade_model == GL_FLAT);
        glUniform1i(renderer->line_stipple, frame->line_stipple);
        glUniform1i(renderer->line_repeat, MAX(frame->line_stipple_repeat, 1));
        glUniform1i(renderer->line_pattern, frame->line_stipple_pattern);
    } else {
        glUseProgram(renderer->colour_program);
        glUniform4f(renderer->colour_steps, frame->colour_steps[0],
                    frame->colour_steps[1], frame->colour_steps[2],
                    frame->colour_steps[3]);
        glUniform1i(renderer->colour_flat, frame->shade_model == GL_FLAT);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xff);
    glClearStencil(0);
    /* Outside the region the stencil is stale, and the scissor keeps out. */
    glScissor(region.x0, region.y0, region_width, region_height);
    glEnable(GL_SCISSOR_TEST);
    glClear(GL_STENCIL_BUFFER_BIT);
    /*
     * Stencil bit 0 marks the clip union and bit 1 a written pixel, so
     * primitives of one frame that overlap still each draw.
     */
    glStencilMask(0x2);
    if (frame->clip_count) {
        glStencilMask(0xff);
        glClearStencil(1);
        glEnable(GL_SCISSOR_TEST);
        for (unsigned i = 0; i < frame->clip_count; i++) {
            const SGIRendererClearRect *rect = &frame->clips[i];

            glScissor(rect->x0, rect->y0,
                      rect->x1 - rect->x0 + 1,
                      rect->y1 - rect->y0 + 1);
            glClear(GL_STENCIL_BUFFER_BIT);
        }
        glStencilMask(0x2);
        glStencilFunc(GL_EQUAL, 3, 0x1);
    } else {
        glStencilFunc(GL_ALWAYS, 3, 0xff);
    }
    glScissor(region.x0, region.y0, region_width, region_height);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glShadeModel(frame->shade_model);
    glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(frame->projection);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(frame->modelview);
    if (frame->primitive == SGI_RENDERER_WINDOW_TRIANGLES) {
        glUniform1f(window->height, frame->height);
        glUniform1i(window->flat, frame->shade_model == GL_FLAT);
        glUniform4d(window->steps, frame->colour_steps[0],
                    frame->colour_steps[1], frame->colour_steps[2],
                    frame->colour_steps[3]);
    }
    for (uint32_t i = 0; frame->primitive == SGI_RENDERER_WINDOW_TRIANGLES &&
                         i < frame->count; i++) {
        const SGIRendererTriangle *t = &frame->triangles[i];
        const GLint colours[3] = { window->c0, window->c1, window->c2 };
        const GLint coords[3] = {
            renderer->texture_t0, renderer->texture_t1, renderer->texture_t2,
        };

        glUniform3d(window->x, t->x[0], t->x[1], t->x[2]);
        glUniform3d(window->y, t->y[0], t->y[1], t->y[2]);
        glUniform3d(window->z, t->z[0], t->z[1], t->z[2]);
        glUniform3d(window->inv_w, t->inv_w[0], t->inv_w[1], t->inv_w[2]);
        for (unsigned v = 0; v < 3; v++) {
            glUniform4d(colours[v], t->colour[v][0], t->colour[v][1],
                        t->colour[v][2], t->colour[v][3]);
            if (frame->texture.slots) {
                glUniform4dv(coords[v], 1, t->texcoord[v]);
            }
        }
        glBegin(GL_TRIANGLES);
        for (unsigned v = 0; v < 3; v++) {
            glVertex2f(t->x[v] / 256.0, t->y[v] / 256.0);
        }
        glEnd();
    }
    for (uint32_t i = 0; frame->primitive == SGI_RENDERER_LINES &&
                         i < frame->count; i++) {
        static const unsigned corner[6] = { 0, 1, 2, 0, 2, 3 };
        const SGIRendererLine *l = &frame->lines[i];

        glUniform4d(renderer->line_ends, l->a[0], l->a[1], l->b[0], l->b[1]);
        glUniform2d(renderer->line_inv_w, l->inv_w[0], l->inv_w[1]);
        glUniform4d(renderer->line_colour_a, l->colour[0][0], l->colour[0][1],
                    l->colour[0][2], l->colour[0][3]);
        glUniform4d(renderer->line_colour_b, l->colour[1][0], l->colour[1][1],
                    l->colour[1][2], l->colour[1][3]);
        glUniform2i(renderer->line_range, l->lo, l->hi);
        glUniform1i(renderer->line_x_major, l->x_major);
        glUniform1i(renderer->line_reverse, l->reverse);
        glUniform1i(renderer->line_counter, l->counter);
        glBegin(GL_TRIANGLES);
        for (unsigned v = 0; v < 6; v++) {
            glVertex2fv(l->quad[corner[v]]);
        }
        glEnd();
    }
    if (frame->primitive != SGI_RENDERER_WINDOW_TRIANGLES &&
        frame->primitive != SGI_RENDERER_LINES) {
        glBegin(frame->primitive == SGI_RENDERER_POINTS ? GL_POINTS :
                GL_TRIANGLES);
    }
    for (uint32_t i = 0;
         frame->primitive != SGI_RENDERER_WINDOW_TRIANGLES &&
         frame->primitive != SGI_RENDERER_LINES && i < frame->count; i++) {
        const float *vertex = frame->vertices +
                              i * SGI_RENDERER_FRAME_VERTEX_FLOATS;

        glColor4fv(vertex + 4);
        glVertex4fv(vertex);
    }
    if (frame->primitive != SGI_RENDERER_WINDOW_TRIANGLES &&
        frame->primitive != SGI_RENDERER_LINES) {
        glEnd();
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    glPixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glReadPixels(region.x0, region.y0, region_width, region_height, GL_RGBA,
                 GL_UNSIGNED_SHORT, frame->rgba);
    if (renderer_gl_error(renderer, "native colour readback")) {
        goto out;
    }
    glReadPixels(region.x0, region.y0, region_width, region_height,
                 GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, frame->written);
    error = glGetError();
    if (error != GL_NO_ERROR) {
        error_setg(&renderer->error, "SGI renderer native draw failed: "
                   "GL error 0x%x", error);
    } else {
        size_t pixels = (size_t)region_width * region_height;

        for (size_t i = 0; i < pixels; i++) {
            uint8_t s = frame->written[i];

            /* Written outside the clip union, or a stray bit. */
            if (s > 3 || (frame->clip_count && s == 2)) {
                error_setg(&renderer->error,
                           "SGI renderer native clip coverage is invalid");
                break;
            }
            frame->written[i] = s >> 1;
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
    glDeleteTextures(1, &sample);
    if (renderer->error) {
        /* Rebuild the frame target rather than trust it after a failure. */
        renderer_gl_frame_target_free(renderer);
    }
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
            renderer_gl_frame_target_free(renderer);
            glDeleteFramebuffers(1, &renderer->framebuffer);
            glDeleteTextures(1, &renderer->texture);
            glDeleteTextures(1, &renderer->sampled_texture);
            glDeleteBuffers(1, &renderer->vbo);
            glDeleteVertexArrays(1, &renderer->vao);
            glDeleteProgram(renderer->program);
            glDeleteProgram(renderer->texture_program);
            glDeleteProgram(renderer->colour_program);
            glDeleteProgram(renderer->window_program);
            glDeleteProgram(renderer->line_program);
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
    error_free(renderer->window_error);
    error_free(renderer->line_error);
    g_free(renderer->vendor);
    g_free(renderer->renderer_name);
    g_free(renderer->version);
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

static bool renderer_gl_supports_viewport(void *opaque,
                                          const int32_t viewport[4],
                                          Error **errp)
{
    SGIRendererGL *renderer = opaque;

    /* GL would clamp the size and silently change the window mapping. */
    if (viewport[2] > renderer->max_viewport[0] ||
        viewport[3] > renderer->max_viewport[1]) {
        error_setg(errp, "SGI renderer host viewport limit is %dx%d",
                   renderer->max_viewport[0], renderer->max_viewport[1]);
        return false;
    }
    return true;
}

static bool renderer_gl_supports_primitive(void *opaque, uint32_t primitive,
                                           float size, Error **errp)
{
    SGIRendererGL *renderer = opaque;

    if (primitive == SGI_RENDERER_WINDOW_TRIANGLES) {
        if (!renderer->window_program || renderer->window_error) {
            error_setg(errp, "%s", renderer->window_error ?
                       error_get_pretty(renderer->window_error) :
                       "SGI window program is unavailable");
            return false;
        }
        if (renderer->max_viewport[0] < 4 * SGI_RENDERER_WINDOW_REACH ||
            renderer->max_viewport[1] < 4 * SGI_RENDERER_WINDOW_REACH) {
            error_setg(errp, "SGI renderer window triangles need a %d-pixel "
                       "viewport", 4 * SGI_RENDERER_WINDOW_REACH);
            return false;
        }
        return true;
    }
    if (primitive == SGI_RENDERER_LINES) {
        /* Lines draw as triangles in the fixed power-of-two viewport. */
        if (!renderer->line_program || renderer->line_error) {
            error_setg(errp, "%s", renderer->line_error ?
                       error_get_pretty(renderer->line_error) :
                       "SGI line program is unavailable");
            return false;
        }
        if (renderer->max_viewport[0] < 4 * SGI_RENDERER_LINE_REACH ||
            renderer->max_viewport[1] < 4 * SGI_RENDERER_LINE_REACH) {
            error_setg(errp, "SGI renderer lines need a %d-pixel viewport",
                       4 * SGI_RENDERER_LINE_REACH);
            return false;
        }
        return true;
    }
    if (primitive != SGI_RENDERER_POINTS ||
        !(size >= renderer->point_range[0] &&
          size <= renderer->point_range[1])) {
        error_setg(errp, "SGI renderer host aliased point size range is "
                   "%g to %g", renderer->point_range[0],
                   renderer->point_range[1]);
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

static void renderer_gl_identify(void *opaque, const char **vendor,
                                 const char **name, const char **version)
{
    SGIRendererGL *renderer = opaque;

    *vendor = renderer->vendor;
    *name = renderer->renderer_name;
    *version = renderer->version;
}

static const SGIRendererOps renderer_gl_ops = {
    .create = renderer_gl_create,
    .draw = renderer_gl_draw,
    .draw_frame = renderer_gl_draw_frame,
    .supports_texture = renderer_gl_supports_texture,
    .supports_viewport = renderer_gl_supports_viewport,
    .supports_primitive = renderer_gl_supports_primitive,
    .clear = renderer_gl_clear,
    .destroy = renderer_gl_destroy,
    .identify = renderer_gl_identify,
};

static void renderer_gl_register(void)
{
    sgi_renderer_register_gl(&renderer_gl_ops);
}
module_init(renderer_gl_register, MODULE_INIT_QOM);
