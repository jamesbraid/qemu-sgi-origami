/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Time the shared SGI software rasterizer on fixed scenes at a head's
 * full 1280x1024, and print a digest of the planes each scene leaves so
 * that a change to the rasterizer can show it drew the same pixels.
 *
 * The planes sit in device-backed board memory and are reached through
 * sgi_plane_load() and sgi_plane_store(), the way the families' targets
 * reach theirs: the "rgba" scenes store complete RGBA words through
 * store_rgba behind a per-pixel window-rectangle test, and the "colour"
 * scene stores packed colours through store_colour. Scenes are drawn from
 * a fixed seed, so a digest depends only on the rasterizer.
 *
 *   sgi-raster-bench [-s scene] [-r repeats]
 *
 * prints one line a scene and repeat: its name, milliseconds and digest.
 *
 *   sgi-raster-bench -c cases [-v]
 *
 * draws a corpus of small randomized cases instead, for comparing two
 * builds of the rasterizer pixel for pixel: each case a primitive batch of
 * one kind (slivers, sub-pixel triangles, huge ones the view volume clips,
 * edges through pixel centres, lines and points) under randomized state
 * and pixel ownership, into small planes. It prints a digest of every
 * case's planes and one of the order and arguments of every target call,
 * and with -v a line for each case, so two builds' outputs differ where
 * and only where they drew differently.
 *
 *   sgi-raster-bench -d [-s scene | -c cases [-v]]
 *
 * draws each scene, or each textured case of the corpus, by the double
 * precision texture path and by the default fixed-point one, and prints
 * how far their colour lies apart and whether depth, stencil and the
 * pixels written are identical.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "hw/display/sgi/board-memory.h"
#include "hw/display/sgi/rasterizer.h"

#define W 1280
#define H 1024
#define TEX_LOG2 8
#define TEX_SIZE (1u << TEX_LOG2)
#define TEX_LEVELS (TEX_LOG2 + 1)
#define TRIANGLES 2000

typedef struct Bench {
    SGIBoardMemory mem;
    const SGIPlane *colour[2];
    const SGIPlane *depth;
    SGIClipList clip;
    uint32_t *texels[TEX_LEVELS];
    SGIGLTexLevel levels[TEX_LEVELS];
    SGIGLTexPyramid pyramid;
    SGIGLTexImage image;
} Bench;

static bool bench_allowed(Bench *b, int x, int y)
{
    return sgi_clip_contains(&b->clip, x, y);
}

static void bench_store_rgba(void *opaque, SGIColourTarget target, int x,
                             int y, const SGIRasterColour *colour)
{
    Bench *b = opaque;
    const uint32_t *c = colour->component;

    if (target < 2 && bench_allowed(b, x, y)) {
        sgi_plane_store(&b->mem, b->colour[target], x, y,
                        (c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3]);
    }
}

static bool bench_load_rgba(void *opaque, SGIColourTarget target, int x,
                            int y, SGIRasterColour *colour)
{
    Bench *b = opaque;
    uint32_t word;

    if (target >= 2) {
        return false;
    }
    word = sgi_plane_load(&b->mem, b->colour[target], x, y);
    for (unsigned i = 0; i < 4; i++) {
        colour->component[i] = (word >> (24 - 8 * i)) & 0xff;
    }
    return true;
}

static void bench_store_colour(void *opaque, SGIColourTarget target, int x,
                               int y, uint32_t colour)
{
    Bench *b = opaque;

    if (target < 2 && bench_allowed(b, x, y)) {
        sgi_plane_store(&b->mem, b->colour[target], x, y, colour);
    }
}

static uint32_t bench_load_colour(void *opaque, SGIColourTarget target,
                                  int x, int y)
{
    Bench *b = opaque;

    return target < 2 ? sgi_plane_load(&b->mem, b->colour[target], x, y) : 0;
}

static uint32_t bench_load_depth(void *opaque, int x, int y)
{
    Bench *b = opaque;

    return sgi_plane_load(&b->mem, b->depth, x, y);
}

static void bench_store_depth(void *opaque, int x, int y, uint32_t depth)
{
    Bench *b = opaque;

    if (bench_allowed(b, x, y)) {
        sgi_plane_store(&b->mem, b->depth, x, y, depth & 0xffffff);
    }
}

static bool bench_read_texel(void *opaque, uint32_t level, uint32_t x,
                             uint32_t y, uint32_t *rgba)
{
    Bench *b = opaque;
    uint32_t size = MAX(1, TEX_SIZE >> level);

    *rgba = b->texels[level][y * size + x];
    return true;
}

static const SGIRasterRules rules_rgba = {
    .subpixel_bits = 8,
    .color_bits = 8,
    .alpha_bits = 8,
    .depth_bits = 24,
    .rounding = SGI_ROUND_HALF_AWAY,
    .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
    .arith = SGI_ARITH_DOUBLE,
    .line_rule = SGI_LINE_COVERAGE_DOUBLE,
    .alpha_test_bits = 12,
    .smooth_primitives = true,
};

static const SGIRasterRules rules_colour = {
    .subpixel_bits = 8,
    .color_bits = 8,
    .depth_bits = 24,
    .rounding = SGI_ROUND_HALF_AWAY,
    .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
    .arith = SGI_ARITH_DOUBLE,
    .line_rule = SGI_LINE_COVERAGE_DOUBLE,
};

static SGIRasterTarget bench_target(Bench *b, bool rgba)
{
    return (SGIRasterTarget) {
        .store_colour = rgba ? NULL : bench_store_colour,
        .load_colour = rgba ? NULL : bench_load_colour,
        .store_rgba = rgba ? bench_store_rgba : NULL,
        .load_rgba = rgba ? bench_load_rgba : NULL,
        .load_depth = bench_load_depth,
        .store_depth = bench_store_depth,
        .texture = &b->image,
        .opaque = b,
#ifdef SGI_RASTER_PARALLEL_MAX_BANDS
        .concurrent_pixels = true,
#endif
    };
}

static void bench_add_plane(Bench *b, uint64_t base, SGIPlaneFormat format,
                            const SGIPlane **out)
{
    SGIPlane plane = {
        .base = base, .len = (uint64_t)W * H * 4,
        .width = W, .height = H, .stride = W * 4,
        .format = format, .packing = SGI_PACK_BE_RED_HIGH,
        .bottom_origin = true,
    };
    uint32_t id = sgi_board_memory_add_plane(&b->mem, &plane, &error_abort);

    *out = sgi_board_memory_plane(&b->mem, id);
}

static void bench_init(Bench *b)
{
    uint64_t plane = (uint64_t)W * H * 4;
    GRand *rand = g_rand_new_with_seed(0x5ca1a4);

    memset(b, 0, sizeof(*b));
    sgi_board_memory_init(&b->mem, 3 * plane);
    bench_add_plane(b, 0, SGI_PLANE_RGBA8, &b->colour[0]);
    bench_add_plane(b, plane, SGI_PLANE_RGBA8, &b->colour[1]);
    bench_add_plane(b, 2 * plane, SGI_PLANE_Z24, &b->depth);
    /* Two overlapping windows' rectangles, as a desktop leaves them. */
    b->clip.n = 2;
    b->clip.r[0] = (SGIClipRect) { 0, 0, W - 1, H / 2 };
    b->clip.r[1] = (SGIClipRect) { 0, H / 2 + 1, W - 1, H - 1 };

    for (unsigned l = 0; l < TEX_LEVELS; l++) {
        uint32_t size = MAX(1, TEX_SIZE >> l);

        b->texels[l] = g_new(uint32_t, size * size);
        for (uint32_t i = 0; i < size * size; i++) {
            b->texels[l][i] = g_rand_int(rand) | 0xff;
        }
        b->levels[l] = (SGIGLTexLevel) {
            .level = l, .width = size, .height = size, .depth = 1,
            .base_format = SGI_GL_RGBA, .internal_format = SGI_GL_RGBA,
            .read_texel = bench_read_texel, .opaque = b,
        };
    }
    b->pyramid = (SGIGLTexPyramid) {
        .levels = b->levels, .count = TEX_LEVELS,
        .base_level = 0, .max_level = 1000,
        .min_lod = -1000, .max_lod = 1000,
    };
    b->image = (SGIGLTexImage) {
        .width = TEX_SIZE, .height = TEX_SIZE, .depth = 1,
        .base_format = SGI_GL_RGBA,
        .min_filter = SGI_GL_LINEAR, .mag_filter = SGI_GL_LINEAR,
        .wrap_s = SGI_GL_REPEAT, .wrap_t = SGI_GL_REPEAT,
        .wrap_r = SGI_GL_REPEAT,
        .pyramid = &b->pyramid,
        .opaque = b,
    };
    g_rand_free(rand);
}

static void bench_cleanup(Bench *b)
{
    for (unsigned l = 0; l < TEX_LEVELS; l++) {
        g_free(b->texels[l]);
    }
    sgi_board_memory_cleanup(&b->mem);
}

/* FNV-1a over every plane byte. */
static uint64_t bench_digest(const Bench *b)
{
    uint64_t h = 0xcbf29ce484222325ull;

    for (uint64_t i = 0; i < b->mem.size; i++) {
        h = (h ^ b->mem.bytes[i]) * 0x100000001b3ull;
    }
    return h;
}

static void context_init(Bench *b, SGIGLContext *c)
{
    SGIGLDrawable d = {
        .bound = true,
        .width = W, .height = H,
        .clip = b->clip,
        .front = 0,
        .back = 1,
    };

    sgi_gl_init(c, 1);
    sgi_gl_set_drawable(c, &d);
    c->cur.viewport[2] = W;
    c->cur.viewport[3] = H;
    c->cur.dither = 0;
}

static void clear(const SGIRasterRules *rules, const SGIRasterTarget *t,
                  Bench *b)
{
    SGIRasterOp op = {
        .kind = SGI_RASTER_CLEAR,
        .targets = { 0 },
        .ntargets = 1,
        .x0 = 0, .y0 = 0, .x1 = W - 1, .y1 = H - 1,
        .clip = b->clip,
        .clear_buffers = SGI_GL_COLOR_BUFFER_BIT | SGI_GL_DEPTH_BUFFER_BIT,
        .clear_rgba = { 0.1f, 0.2f, 0.3f, 1.0f },
        .clear_colour_mask = SGI_GL_COLOUR_MASK_ALL,
        .clear_depth24 = 0xffffff,
    };

    sgi_soft_raster(rules, t, &op);
}

/*
 * TRIANGLES triangles of about 150 by 120 pixels in perspective, each
 * vertex with its own colour and texture coordinates.
 */
static float *triangles(uint32_t seed, float alpha)
{
    float *v = g_new0(float, (size_t)TRIANGLES * 3 * SGI_GL_VERTEX_FLOATS);
    GRand *rand = g_rand_new_with_seed(seed);

    for (unsigned t = 0; t < TRIANGLES; t++) {
        double cx = g_rand_double_range(rand, -1.1, 1.1);
        double cy = g_rand_double_range(rand, -1.1, 1.1);
        double cz = g_rand_double_range(rand, -0.9, 0.9);

        for (unsigned i = 0; i < 3; i++) {
            float *p = &v[((size_t)t * 3 + i) * SGI_GL_VERTEX_FLOATS];
            double w = g_rand_double_range(rand, 1, 2);

            p[SGI_GL_V_POSITION + 0] =
                (cx + g_rand_double_range(rand, -0.12, 0.12)) * w;
            p[SGI_GL_V_POSITION + 1] =
                (cy + g_rand_double_range(rand, -0.12, 0.12)) * w;
            p[SGI_GL_V_POSITION + 2] =
                (cz + g_rand_double_range(rand, -0.1, 0.1)) * w;
            p[SGI_GL_V_POSITION + 3] = w;
            for (unsigned k = 0; k < 3; k++) {
                p[SGI_GL_V_COLOR + k] = g_rand_double(rand);
            }
            p[SGI_GL_V_COLOR + 3] = alpha;
            p[SGI_GL_V_TEXCOORD + 0] = g_rand_double_range(rand, -1, 2) * w;
            p[SGI_GL_V_TEXCOORD + 1] = g_rand_double_range(rand, -1, 2) * w;
            p[SGI_GL_V_TEXCOORD + 3] = w;
        }
    }
    g_rand_free(rand);
    return v;
}

/*
 * TRIANGLES slivers about 300 pixels long and two wide at any angle, in
 * perspective: most of each one's bounding box is outside it, as with
 * the distant faces of a city model.
 */
static float *slivers(uint32_t seed)
{
    float *v = g_new0(float, (size_t)TRIANGLES * 3 * SGI_GL_VERTEX_FLOATS);
    GRand *rand = g_rand_new_with_seed(seed);

    for (unsigned t = 0; t < TRIANGLES; t++) {
        double cx = g_rand_double_range(rand, -0.8, 0.8);
        double cy = g_rand_double_range(rand, -0.8, 0.8);
        double cz = g_rand_double_range(rand, -0.9, 0.9);
        double a = g_rand_double_range(rand, 0, 2 * M_PI);
        double dx = cos(a) * 0.25, dy = sin(a) * 0.3;
        double end[3][2] = {
            { cx - dx, cy - dy }, { cx + dx, cy + dy },
            { cx + dx - dy * 0.008, cy + dy + dx * 0.008 },
        };

        for (unsigned i = 0; i < 3; i++) {
            float *p = &v[((size_t)t * 3 + i) * SGI_GL_VERTEX_FLOATS];
            double w = g_rand_double_range(rand, 1, 2);

            p[SGI_GL_V_POSITION + 0] = end[i][0] * w;
            p[SGI_GL_V_POSITION + 1] = end[i][1] * w;
            p[SGI_GL_V_POSITION + 2] =
                (cz + g_rand_double_range(rand, -0.1, 0.1)) * w;
            p[SGI_GL_V_POSITION + 3] = w;
            for (unsigned k = 0; k < 3; k++) {
                p[SGI_GL_V_COLOR + k] = g_rand_double(rand);
            }
            p[SGI_GL_V_COLOR + 3] = 1;
        }
    }
    g_rand_free(rand);
    return v;
}

typedef enum Scene {
    SCENE_CLEAR,
    SCENE_SMOOTH,
    SCENE_TEXTURE,
    SCENE_MIPMAP,
    SCENE_BLEND,
    SCENE_COLOUR,
    SCENE_SLIVER,
    SCENE_PERSPECTIVE,
    SCENE_TRILINEAR,
    SCENE_COUNT,
} Scene;

static const char *const scene_names[SCENE_COUNT] = {
    [SCENE_CLEAR] = "clear",
    [SCENE_SMOOTH] = "smooth",
    [SCENE_TEXTURE] = "texture",
    [SCENE_MIPMAP] = "mipmap",
    [SCENE_BLEND] = "blend",
    [SCENE_COLOUR] = "colour",
    [SCENE_SLIVER] = "sliver",
    [SCENE_PERSPECTIVE] = "perspective",
    [SCENE_TRILINEAR] = "trilinear",
};

/*
 * GLperf's depth-tested trilinear fill, as a frame of two quads: one
 * across the whole window, and over it a floor in perspective on the
 * lower 640 rows, its 1/W falling from 1 at the bottom to 1/8 at the
 * top, so its level of detail runs from magnified to about 5. Both
 * repeat the texture: the window quad 8 times across, the floor up to
 * 34 times at its far edge, where coordinate precision runs out first.
 * 2,129,920 textured fragments.
 */
#define TRILINEAR_FLOOR_ROWS 640

static float *trilinear_quads(void)
{
    double top = 2.0 * TRILINEAR_FLOOR_ROWS / H - 1;
    /* x, y, z, w, s, t for each corner, anticlockwise from bottom left */
    const double corner[8][6] = {
        { -1, -1, 0.8, 1, 0, 0 },
        { 1, -1, 0.8, 1, 8, 0 },
        { 1, 1, 0.8, 1, 8, 6.4 },
        { -1, 1, 0.8, 1, 0, 6.4 },
        { -1, -1, -0.5, 1, 16, 0 },
        { 1, -1, -0.5, 1, 20, 0 },
        { 8, top * 8, 0.5 * 8, 8, 34, 16 },
        { -8, top * 8, 0.5 * 8, 8, 2, 16 },
    };
    float *v = g_new0(float, 8 * SGI_GL_VERTEX_FLOATS);

    for (unsigned i = 0; i < 8; i++) {
        float *p = &v[i * SGI_GL_VERTEX_FLOATS];

        for (unsigned k = 0; k < 4; k++) {
            p[SGI_GL_V_POSITION + k] = corner[i][k];
            p[SGI_GL_V_COLOR + k] = 1;
        }
        p[SGI_GL_V_TEXCOORD + 0] = corner[i][4];
        p[SGI_GL_V_TEXCOORD + 1] = corner[i][5];
        p[SGI_GL_V_TEXCOORD + 3] = 1;
    }
    return v;
}

/*
 * A texture with structure at every scale, for the trilinear scene:
 * 16-texel checks in red, a ramp in green, diagonal stripes in blue and
 * a pattern in alpha, each level the box filter of the one above.
 */
static void trilinear_texels(Bench *b)
{
    for (uint32_t y = 0; y < TEX_SIZE; y++) {
        for (uint32_t x = 0; x < TEX_SIZE; x++) {
            uint32_t r = (((x >> 4) ^ (y >> 4)) & 1) ? 230 : 40;
            uint32_t g = x, bl = ((x * 3 + y * 5) & 63) * 4;

            b->texels[0][y * TEX_SIZE + x] = (r << 24) | (g << 16) |
                                              (bl << 8) | ((x ^ y) & 0xff);
        }
    }
    for (unsigned l = 1; l < TEX_LEVELS; l++) {
        uint32_t n = TEX_SIZE >> l, p = 2 * n;
        const uint32_t *up = b->texels[l - 1];

        for (uint32_t y = 0; y < n; y++) {
            for (uint32_t x = 0; x < n; x++) {
                const uint32_t c[4] = {
                    up[2 * y * p + 2 * x], up[2 * y * p + 2 * x + 1],
                    up[(2 * y + 1) * p + 2 * x],
                    up[(2 * y + 1) * p + 2 * x + 1],
                };
                uint32_t out = 0;

                for (unsigned k = 0; k < 4; k++) {
                    unsigned shift = 24 - 8 * k, sum = 2;

                    for (unsigned i = 0; i < 4; i++) {
                        sum += (c[i] >> shift) & 0xff;
                    }
                    out |= (sum >> 2) << shift;
                }
                b->texels[l][y * n + x] = out;
            }
        }
    }
}

/* Draw scene into a new bench, returning it and the milliseconds taken. */
static Bench *scene_draw(Scene scene, double *ms)
{
    bool rgba = scene != SCENE_COLOUR;
    const SGIRasterRules *rules = rgba ? &rules_rgba : &rules_colour;
    g_autofree float *verts = scene == SCENE_SLIVER ? slivers(scene + 1) :
        scene == SCENE_TRILINEAR ? trilinear_quads() :
        triangles(scene + 1, scene == SCENE_BLEND ? 0.5f : 1.0f);
    Bench *b = g_new(Bench, 1);
    SGIRasterTarget t;
    SGIGLContext c;
    SGIGLDraw d = {
        .context = 1,
        .mode = SGI_GL_TRIANGLES,
        .count = TRIANGLES * 3,
        .vertices = verts,
    };
    int64_t start, end;

    if (scene == SCENE_PERSPECTIVE) {
        /*
         * Texture coordinates as a client gives them, Q 1, so Q/W varies
         * across each triangle as 1/W does: the mipmap scene's triangles
         * have Q = W, which leaves Q/W flat.
         */
        for (unsigned i = 0; i < TRIANGLES * 3; i++) {
            float *p = &verts[(size_t)i * SGI_GL_VERTEX_FLOATS];
            float w = p[SGI_GL_V_POSITION + 3];

            p[SGI_GL_V_TEXCOORD + 0] /= w;
            p[SGI_GL_V_TEXCOORD + 1] /= w;
            p[SGI_GL_V_TEXCOORD + 3] = 1;
        }
    }
    bench_init(b);
    t = bench_target(b, rgba);
    context_init(b, &c);
    d.state = &c.cur;
    switch (scene) {
    case SCENE_CLEAR:
        break;
    case SCENE_SMOOTH:
    case SCENE_COLOUR:
    case SCENE_SLIVER:
        c.cur.depth_test = 1;
        break;
    case SCENE_TRILINEAR:
        trilinear_texels(b);
        d.mode = SGI_GL_QUADS;
        d.count = 8;
        /* fall through */
    case SCENE_MIPMAP:
    case SCENE_PERSPECTIVE:
        b->image.min_filter = SGI_GL_LINEAR_MIPMAP_LINEAR;
        /* fall through */
    case SCENE_TEXTURE:
        c.cur.depth_test = 1;
        c.cur.texture_2d = 1;
        c.cur.texture_env_mode = SGI_GL_MODULATE;
        break;
    case SCENE_BLEND:
        c.cur.blend = 1;
        c.cur.blend_src = SGI_GL_SRC_ALPHA;
        c.cur.blend_dst = SGI_GL_ONE_MINUS_SRC_ALPHA;
        break;
    default:
        g_assert_not_reached();
    }

    start = get_clock();
    if (scene == SCENE_CLEAR) {
        for (unsigned i = 0; i < 16; i++) {
            clear(rules, &t, b);
        }
    } else {
        clear(rules, &t, b);
        sgi_soft_draw(rules, &t, &d);
    }
    end = get_clock();
    *ms = (end - start) / 1e6;
    sgi_gl_cleanup(&c);
    return b;
}

static void scene_run(Scene scene, unsigned repeat)
{
    double ms;
    Bench *b = scene_draw(scene, &ms);

    printf("%-8s %u %9.3f ms %016" PRIx64 "\n", scene_names[scene], repeat,
           ms, bench_digest(b));
    bench_cleanup(b);
    g_free(b);
}

/*
 * How far the fixed-point texture path's colour lies from the double
 * path's: pixels whose colour differs, the largest difference of any
 * channel out of 255, and the sum of every channel's difference.
 */
typedef struct Diff {
    uint64_t pixels, differing, channels, sum;
    unsigned max;
} Diff;

static void diff_colour(Diff *diff, uint32_t a, uint32_t b, unsigned channels)
{
    unsigned most = 0;

    for (unsigned k = 0; k < channels; k++) {
        unsigned x = (a >> (8 * k)) & 0xff, y = (b >> (8 * k)) & 0xff;
        unsigned d = x > y ? x - y : y - x;

        diff->sum += d;
        most = MAX(most, d);
    }
    diff->pixels++;
    diff->channels += channels;
    diff->differing += most > 0;
    diff->max = MAX(diff->max, most);
}

static void diff_print(const char *what, const Diff *diff, bool exact)
{
    printf("%s: %" PRIu64 " pixels, %" PRIu64 " differ, max %u/255, "
           "mean %.4f; depth, stencil and pixels written %s\n", what,
           diff->pixels, diff->differing, diff->max,
           diff->channels ? (double)diff->sum / diff->channels : 0.0,
           exact ? "identical" : "DIFFER");
}

/*
 * Draw scene by the double-precision texture path and by the default
 * one, and compare: the colour planes, every pixel, and the depth plane
 * exactly.
 */
static void scene_diff(Scene scene)
{
    double ms;
    Bench *a, *b;
    uint64_t plane = (uint64_t)W * H * 4;
    Diff diff = { 0 };
    bool exact;

    sgi_raster_set_texture_path(SGI_RASTER_TEXTURE_DOUBLE);
    a = scene_draw(scene, &ms);
    sgi_raster_set_texture_path(SGI_RASTER_TEXTURE_SPANS);
    b = scene_draw(scene, &ms);
    for (uint64_t i = 0; i < 2 * plane; i += 4) {
        uint32_t x, y;

        memcpy(&x, &a->mem.bytes[i], 4);
        memcpy(&y, &b->mem.bytes[i], 4);
        diff_colour(&diff, x, y, 4);
    }
    exact = !memcmp(&a->mem.bytes[2 * plane], &b->mem.bytes[2 * plane],
                    plane);
    diff_print(scene_names[scene], &diff, exact);
    bench_cleanup(a);
    bench_cleanup(b);
    g_free(a);
    g_free(b);
}

/*
 * The corpus: planes small enough to digest per case, each pixel a word,
 * and every call the rasterizer makes on the target folded into a digest
 * in the order it made them.
 */
#define CW 160
#define CH 120
#define CTEX_LOG2 3
#define CTEX_LEVELS (CTEX_LOG2 + 1)
#define CVERTS 24

typedef struct Corpus {
    uint32_t colour[2][CW * CH];
    uint32_t depth[CW * CH];
    uint32_t stencil[CW * CH];
    /*
     * Each pixel's calls in order, and the calls for no pixel of the
     * planes, summed: a rasterizer that draws pixels on several threads
     * still makes each pixel's calls in one order.
     */
    uint64_t pixel_calls[CW * CH];
    uint64_t outside;
    /* Which pixels had a colour or depth store, for comparing paths. */
    uint8_t written[CW * CH];
    uint32_t texels[CTEX_LEVELS][(1u << CTEX_LOG2) * (1u << CTEX_LOG2)];
    /* Raw-component reads: each slot's width, and texels out of range. */
    uint8_t bits[4];
    bool overflow;
    SGIGLTexLevel levels[CTEX_LEVELS];
    SGIGLTexPyramid pyramid;
    SGIGLTexImage image;
} Corpus;

static uint64_t fnv(uint64_t h, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++, v >>= 8) {
        h = (h ^ (v & 0xff)) * 0x100000001b3ull;
    }
    return h;
}

static void corpus_call(Corpus *c, unsigned kind, unsigned target, int x,
                        int y, uint64_t value)
{
    uint64_t h = fnv(fnv(0, kind), target);

    h = fnv(fnv(h, (uint32_t)x | (uint64_t)(uint32_t)y << 32), value);
    if (x >= 0 && x < CW && y >= 0 && y < CH) {
        c->pixel_calls[y * CW + x] = fnv(c->pixel_calls[y * CW + x], h);
        /* Colour, index, depth and RGBA stores. */
        c->written[y * CW + x] |= (kind == 1 || kind == 3 || kind == 8) |
                                  (kind == 5) << 1;
    } else {
        qatomic_add(&c->outside, h);
    }
}

static uint64_t corpus_calls(const Corpus *c)
{
    uint64_t h = 0xcbf29ce484222325ull;

    for (unsigned i = 0; i < CW * CH; i++) {
        h = fnv(h, c->pixel_calls[i]);
    }
    return fnv(h, c->outside);
}

/* Stores outside the planes are folded into the call digest, not made. */
static uint32_t *corpus_at(uint32_t *plane, int x, int y)
{
    return x >= 0 && x < CW && y >= 0 && y < CH ? &plane[y * CW + x] : NULL;
}

static void corpus_store_colour(void *opaque, SGIColourTarget target, int x,
                                int y, uint32_t colour)
{
    Corpus *c = opaque;
    uint32_t *p = target < 2 ? corpus_at(c->colour[target], x, y) : NULL;

    corpus_call(c, 1, target, x, y, colour);
    if (p) {
        *p = colour;
    }
}

static uint32_t corpus_load_colour(void *opaque, SGIColourTarget target,
                                   int x, int y)
{
    Corpus *c = opaque;
    uint32_t *p = target < 2 ? corpus_at(c->colour[target], x, y) : NULL;

    corpus_call(c, 2, target, x, y, 0);
    return p ? *p : 0;
}

static void corpus_store_index(void *opaque, SGIColourTarget target, int x,
                               int y, uint32_t index, uint32_t mask)
{
    Corpus *c = opaque;
    uint32_t *p = target < 2 ? corpus_at(c->colour[target], x, y) : NULL;

    corpus_call(c, 3, target, x, y, index | (uint64_t)mask << 32);
    if (p) {
        *p = (*p & ~mask) | (index & mask);
    }
}

static uint32_t corpus_load_depth(void *opaque, int x, int y)
{
    Corpus *c = opaque;
    uint32_t *p = corpus_at(c->depth, x, y);

    corpus_call(c, 4, 0, x, y, 0);
    return p ? *p : 0;
}

static void corpus_store_depth(void *opaque, int x, int y, uint32_t depth)
{
    Corpus *c = opaque;
    uint32_t *p = corpus_at(c->depth, x, y);

    corpus_call(c, 5, 0, x, y, depth);
    if (p) {
        *p = depth;
    }
}

static uint32_t corpus_load_stencil(void *opaque, int x, int y)
{
    Corpus *c = opaque;
    uint32_t *p = corpus_at(c->stencil, x, y);

    corpus_call(c, 6, 0, x, y, 0);
    return p ? *p : 0;
}

static void corpus_store_stencil(void *opaque, int x, int y, uint32_t value)
{
    Corpus *c = opaque;
    uint32_t *p = corpus_at(c->stencil, x, y);

    corpus_call(c, 7, 0, x, y, value);
    if (p) {
        *p = value;
    }
}

static void corpus_store_rgba(void *opaque, SGIColourTarget target, int x,
                              int y, const SGIRasterColour *colour)
{
    Corpus *c = opaque;
    uint32_t *p = target < 2 ? corpus_at(c->colour[target], x, y) : NULL;
    const uint32_t *k = colour->component;
    uint32_t word = (k[0] << 24) | (k[1] << 16) | (k[2] << 8) | k[3];

    corpus_call(c, 8, target, x, y, word);
    if (p) {
        *p = word;
    }
}

static bool corpus_load_rgba(void *opaque, SGIColourTarget target, int x,
                             int y, SGIRasterColour *colour)
{
    Corpus *c = opaque;
    uint32_t *p = target < 2 ? corpus_at(c->colour[target], x, y) : NULL;

    corpus_call(c, 9, target, x, y, 0);
    if (!p) {
        return false;
    }
    for (unsigned i = 0; i < 4; i++) {
        colour->component[i] = (*p >> (24 - 8 * i)) & 0xff;
    }
    return true;
}

static bool corpus_read_texel(void *opaque, uint32_t level, uint32_t x,
                              uint32_t y, uint32_t *rgba)
{
    Corpus *c = opaque;
    uint32_t size = MAX(1, (1u << CTEX_LOG2) >> level);

    *rgba = c->texels[level][y * size + x];
    return true;
}

static bool corpus_chance(GRand *rand, unsigned percent)
{
    return g_rand_int_range(rand, 0, 100) < percent;
}

/*
 * A texel's components at the slot widths, from its packed word; with
 * overflow set, about one texel in 61 holds a value too wide for its slot.
 */
static bool corpus_read_texel_components(void *opaque, uint32_t level,
                                         uint32_t x, uint32_t y,
                                         uint16_t component[4])
{
    Corpus *c = opaque;
    uint32_t size = MAX(1, (1u << CTEX_LOG2) >> level);
    uint32_t word = c->texels[level][y * size + x];

    for (unsigned k = 0; k < 4; k++) {
        component[k] = (word >> (8 * k)) & ((1u << c->bits[k]) - 1);
    }
    if (c->overflow && word % 61 == 0) {
        component[0] = 1u << c->bits[0];
    }
    return true;
}

static void corpus_texels(Corpus *c, GRand *rand)
{
    for (unsigned l = 0; l < CTEX_LEVELS; l++) {
        uint32_t size = MAX(1, (1u << CTEX_LOG2) >> l);

        for (uint32_t i = 0; i < size * size; i++) {
            c->texels[l][i] = g_rand_int(rand);
        }
    }
}

static void corpus_texture(Corpus *c, GRand *rand)
{
    static const uint32_t formats[] = {
        SGI_GL_RGBA, SGI_GL_RGB, SGI_GL_LUMINANCE, SGI_GL_LUMINANCE_ALPHA,
        SGI_GL_ALPHA, SGI_GL_INTENSITY,
    };
    static const uint32_t wraps[] = {
        SGI_GL_REPEAT, SGI_GL_REPEAT, SGI_GL_CLAMP, SGI_GL_CLAMP_TO_EDGE,
        SGI_GL_CLAMP_TO_BORDER,
    };
    uint32_t format = formats[g_rand_int_range(rand, 0, ARRAY_SIZE(formats))];
    bool raw = g_rand_boolean(rand);

    for (unsigned k = 0; k < 4; k++) {
        c->bits[k] = g_rand_int_range(rand, 1, 13);
    }
    c->overflow = corpus_chance(rand, 10);
    corpus_texels(c, rand);
    for (unsigned l = 0; l < CTEX_LEVELS; l++) {
        uint32_t size = MAX(1, (1u << CTEX_LOG2) >> l);

        c->levels[l] = (SGIGLTexLevel) {
            .level = l, .width = size, .height = size, .depth = 1,
            .base_format = format, .internal_format = format,
            .read_texel = corpus_read_texel, .opaque = c,
        };
        if (raw) {
            memcpy(c->levels[l].component_bits, c->bits, sizeof(c->bits));
            c->levels[l].read_texel_components =
                corpus_read_texel_components;
        }
    }
    c->pyramid = (SGIGLTexPyramid) {
        .levels = c->levels, .count = CTEX_LEVELS,
        .base_level = 0, .max_level = 1000,
        .min_lod = -1000, .max_lod = 1000,
    };
    c->image = (SGIGLTexImage) {
        .width = 1u << CTEX_LOG2, .height = 1u << CTEX_LOG2, .depth = 1,
        .base_format = format,
        .min_filter = g_rand_boolean(rand) ? SGI_GL_LINEAR :
                      SGI_GL_LINEAR_MIPMAP_LINEAR,
        .mag_filter = g_rand_boolean(rand) ? SGI_GL_LINEAR : SGI_GL_NEAREST,
        .wrap_s = wraps[g_rand_int_range(rand, 0, ARRAY_SIZE(wraps))],
        .wrap_t = wraps[g_rand_int_range(rand, 0, ARRAY_SIZE(wraps))],
        .wrap_r = SGI_GL_REPEAT,
        .border_color = { 0.25, 0.5, 0.75, 1 },
        .pyramid = &c->pyramid,
        .opaque = c,
    };
}

static uint64_t corpus_planes(const Corpus *c)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const uint32_t *planes[4] = {
        c->colour[0], c->colour[1], c->depth, c->stencil,
    };

    for (unsigned p = 0; p < 4; p++) {
        for (unsigned i = 0; i < CW * CH; i++) {
            h = fnv(h, planes[p][i]);
        }
    }
    return h;
}

/*
 * A window coordinate the case's vertices are made from: on the subpixel
 * grid, on a pixel centre or edge, or anywhere.
 */
static double corpus_coord(GRand *rand, double lo, double hi, unsigned sub)
{
    double v = g_rand_double_range(rand, lo, hi);

    switch (g_rand_int_range(rand, 0, 4)) {
    case 0:
        return floor(v) + 0.5;
    case 1:
        return floor(v);
    case 2:
        return floor(v * (1 << sub)) / (1 << sub);
    default:
        return v;
    }
}

typedef enum CorpusShape {
    SHAPE_RANDOM,
    SHAPE_SLIVER,
    SHAPE_SUBPIXEL,
    SHAPE_HUGE,
    SHAPE_AXIS,
    SHAPE_BEHIND,
    SHAPE_COUNT,
} CorpusShape;

/*
 * Window positions for one vertex triple of the given shape, in pixels
 * from the window origin, with a clip w for each; positions outside the
 * view volume are left for the clipper.
 */
static void corpus_triple(GRand *rand, CorpusShape shape, const int vp[4],
                          unsigned sub, double pos[3][2], double w[3])
{
    double x0 = vp[0] - 8, x1 = vp[0] + vp[2] + 8;
    double y0 = vp[1] - 8, y1 = vp[1] + vp[3] + 8;

    for (unsigned i = 0; i < 3; i++) {
        pos[i][0] = corpus_coord(rand, x0, x1, sub);
        pos[i][1] = corpus_coord(rand, y0, y1, sub);
        w[i] = 1;
    }
    switch (shape) {
    case SHAPE_SLIVER: {
        /* The third vertex near the line through the first two. */
        double t = g_rand_double_range(rand, -0.5, 1.5);
        double off = g_rand_double_range(rand, -1.5, 1.5) /
                     (g_rand_boolean(rand) ? 1 : (1 << sub));

        pos[2][0] = pos[0][0] + t * (pos[1][0] - pos[0][0]) + off;
        pos[2][1] = pos[0][1] + t * (pos[1][1] - pos[0][1]) - off;
        break;
    }
    case SHAPE_SUBPIXEL:
        for (unsigned i = 1; i < 3; i++) {
            pos[i][0] = pos[0][0] + g_rand_double_range(rand, -1.2, 1.2);
            pos[i][1] = pos[0][1] + g_rand_double_range(rand, -1.2, 1.2);
        }
        break;
    case SHAPE_HUGE:
        for (unsigned i = 0; i < 3; i++) {
            pos[i][0] = g_rand_double_range(rand, -40, 40) * vp[2];
            pos[i][1] = g_rand_double_range(rand, -40, 40) * vp[3];
        }
        break;
    case SHAPE_AXIS:
        /* A horizontal and a vertical edge, on pixel centres or not. */
        pos[1][1] = pos[0][1];
        pos[2][0] = g_rand_boolean(rand) ? pos[0][0] : pos[1][0];
        break;
    case SHAPE_BEHIND:
        /* Some vertices behind the eye: the near plane cuts them. */
        for (unsigned i = 0; i < 3; i++) {
            w[i] = g_rand_double_range(rand, -1, 2);
        }
        break;
    default:
        break;
    }
}

static void corpus_vertex(GRand *rand, float *v, const int vp[4],
                          const double pos[2], double w, bool smooth_z)
{
    /* Window to normalized device coordinates, then times w. */
    double nx = 2 * (pos[0] - vp[0]) / vp[2] - 1;
    double ny = 2 * (pos[1] - vp[1]) / vp[3] - 1;
    double nz = g_rand_double_range(rand, smooth_z ? -1.2 : -0.9,
                                    smooth_z ? 1.2 : 0.9);

    v[SGI_GL_V_POSITION + 0] = nx * w;
    v[SGI_GL_V_POSITION + 1] = ny * w;
    v[SGI_GL_V_POSITION + 2] = nz * w;
    v[SGI_GL_V_POSITION + 3] = w;
    for (unsigned k = 0; k < 4; k++) {
        v[SGI_GL_V_COLOR + k] = g_rand_double_range(rand, -0.1, 1.1);
        v[SGI_GL_V_BACK_COLOR + k] = g_rand_double(rand);
    }
    v[SGI_GL_V_TEXCOORD + 0] = g_rand_double_range(rand, -2, 3) * w;
    v[SGI_GL_V_TEXCOORD + 1] = g_rand_double_range(rand, -2, 3) * w;
    /* Q varies across a primitive in two vertices in five. */
    v[SGI_GL_V_TEXCOORD + 3] = w * (corpus_chance(rand, 40) ?
                                    g_rand_double_range(rand, 0.5, 2) : 1);
    v[SGI_GL_V_FOG] = g_rand_double_range(rand, 0, 4);
    v[SGI_GL_V_EDGE] = 1;
    v[SGI_GL_V_INDEX] = g_rand_int_range(rand, 0, 256);
    v[SGI_GL_V_BACK_INDEX] = g_rand_int_range(rand, 0, 256);
}

static const uint32_t corpus_modes[] = {
    SGI_GL_TRIANGLES, SGI_GL_TRIANGLES, SGI_GL_TRIANGLES,
    SGI_GL_TRIANGLE_STRIP, SGI_GL_TRIANGLE_FAN, SGI_GL_QUADS,
    SGI_GL_POLYGON, SGI_GL_LINES, SGI_GL_LINE_STRIP, SGI_GL_POINTS,
};

static void corpus_state(GRand *rand, SGIGLDrawState *st)
{
    st->shade_model = g_rand_boolean(rand) ? SGI_GL_SMOOTH : SGI_GL_FLAT;
    st->depth_test = corpus_chance(rand, 60);
    st->depth_func = g_rand_int_range(rand, 0, 8);
    st->depth_mask = corpus_chance(rand, 80);
    st->depth_range[0] = corpus_chance(rand, 80) ? 0 : g_rand_double(rand);
    st->depth_range[1] = corpus_chance(rand, 80) ? 1 : g_rand_double(rand);
    st->draw_buffers = g_rand_int_range(rand, 1, 4);
    st->colour_mask = corpus_chance(rand, 80) ? SGI_GL_COLOUR_MASK_ALL :
                      (uint32_t)g_rand_int_range(rand, 0, 16);
    st->cull_face = corpus_chance(rand, 25);
    st->cull_face_mode = (uint32_t[]){ SGI_GL_FRONT, SGI_GL_BACK,
                                       SGI_GL_FRONT_AND_BACK }
                         [g_rand_int_range(rand, 0, 3)];
    st->front_face = g_rand_boolean(rand) ? SGI_GL_CW : SGI_GL_CCW;
    st->scissor_test = corpus_chance(rand, 30);
    st->scissor[0] = g_rand_int_range(rand, -20, CW);
    st->scissor[1] = g_rand_int_range(rand, -20, CH);
    st->scissor[2] = g_rand_int_range(rand, 0, CW);
    st->scissor[3] = g_rand_int_range(rand, 0, CH);
    st->alpha_test = corpus_chance(rand, 15);
    st->alpha_func = g_rand_int_range(rand, 0, 8);
    st->alpha_ref = g_rand_double(rand);
    st->stencil_test = corpus_chance(rand, 20);
    st->stencil_func = g_rand_int_range(rand, 0, 8);
    st->stencil_ref = g_rand_int_range(rand, 0, 256);
    st->stencil_value_mask = g_rand_int(rand);
    st->stencil_write_mask = g_rand_int(rand);
    for (unsigned i = 0; i < 3; i++) {
        st->stencil_op[i] = (uint32_t[]){ SGI_GL_KEEP, 0, SGI_GL_REPLACE,
                                          SGI_GL_INCR, SGI_GL_DECR,
                                          SGI_GL_INVERT }
                            [g_rand_int_range(rand, 0, 6)];
    }
    st->blend = corpus_chance(rand, 20);
    st->blend_src = SGI_GL_SRC_ALPHA;
    st->blend_dst = SGI_GL_ONE_MINUS_SRC_ALPHA;
    st->blend_equation = SGI_GL_FUNC_ADD;
    st->color_logic_op = !st->blend && corpus_chance(rand, 10);
    st->logic_op_mode = SGI_GL_CLEAR + g_rand_int_range(rand, 0, 16);
    st->fog = corpus_chance(rand, 15);
    st->fog_mode = (uint32_t[]){ SGI_GL_LINEAR, SGI_GL_EXP, SGI_GL_EXP2 }
                   [g_rand_int_range(rand, 0, 3)];
    st->fog_density = g_rand_double(rand);
    st->fog_start = 0;
    st->fog_end = 4;
    for (unsigned k = 0; k < 4; k++) {
        st->fog_color[k] = g_rand_double(rand);
    }
    st->polygon_offset = corpus_chance(rand, 15) ? SGI_GL_OFFSET_FILL : 0;
    st->offset_factor = g_rand_double_range(rand, -2, 2);
    st->offset_units = g_rand_double_range(rand, -4, 4);
    st->clip_planes = corpus_chance(rand, 10) ? 1 : 0;
    st->clip_plane[0][0] = g_rand_double_range(rand, -1, 1);
    st->clip_plane[0][1] = g_rand_double_range(rand, -1, 1);
    st->clip_plane[0][3] = g_rand_double_range(rand, -0.5, 0.5);
    st->dither = corpus_chance(rand, 30);
}

/*
 * Draw case index of the corpus, leaving its planes in c, which is
 * allocated zeroed; whether the case textured is returned.
 */
static bool corpus_draw(unsigned index, Corpus *c)
{
    static const SGIRasterRules base = {
        .subpixel_bits = 8,
        .color_bits = 8,
        .depth_bits = 24,
        .rounding = SGI_ROUND_HALF_AWAY,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE,
        .line_rule = SGI_LINE_COVERAGE_DOUBLE,
        .smooth_primitives = true,
    };
    GRand *rand = g_rand_new_with_seed(0x5ba45 + index);
    float verts[CVERTS * SGI_GL_VERTEX_FLOATS] = { 0 };
    uint32_t stipple[32];
    uint32_t modes[2] = { SGI_GL_FILL, SGI_GL_FILL };
    SGIRasterRules rules = base;
    SGIGLDrawable drawable = { .bound = true, .front = 0, .back = 1 };
    SGIGLContext ctx;
    SGIRasterTarget t;
    SGIGLDraw d = { .context = 1, .vertices = verts };
    bool rgba = g_rand_boolean(rand);
    CorpusShape shape = g_rand_int_range(rand, 0, SHAPE_COUNT);
    int vp[4];
    unsigned nclip;

    rules.subpixel_bits = (uint8_t[]){ 4, 8, 8, 12 }
                          [g_rand_int_range(rand, 0, 4)];
    rules.stencil_bits = corpus_chance(rand, 50) ? 8 : 0;
    rules.alpha_bits = rgba ? 8 : 0;
    rules.rounding = g_rand_int_range(rand, 0, SGI_ROUND_HALF_AWAY + 1);
    rules.dither = corpus_chance(rand, 20) ? SGI_DITHER_BAYER4_TOP_ROWS :
                   SGI_DITHER_NONE;
    rules.dither_anchor = SGI_DITHER_SCREEN;
    rules.smooth_rule = g_rand_int_range(rand, 0,
                                         SGI_SMOOTH_TWO_PIXEL_FILTER + 1);
    rules.depth_float = corpus_chance(rand, 20);
    if (corpus_chance(rand, 30)) {
        /*
         * Depth from the head's own eye-space row, as VPro gives it: about
         * (z / w + 1) / 2, so most fragments land inside [0, 1].
         */
        rules.depth_row_given = true;
        rules.depth_row[2] = 0.5 + g_rand_double_range(rand, -0.05, 0.05);
        rules.depth_row[3] = 0.5 + g_rand_double_range(rand, -0.05, 0.05);
        if (g_rand_boolean(rand)) {
            rules.depth_divisor_given = true;
            rules.depth_divisor[2] = g_rand_double_range(rand, -0.1, 0.1);
            rules.depth_divisor[3] = 1;
        }
        rules.depth_slack = g_rand_double_range(rand, 0, 0.01);
    }

    for (unsigned i = 0; i < CW * CH; i++) {
        c->colour[0][i] = g_rand_int(rand);
        c->colour[1][i] = g_rand_int(rand);
        c->depth[i] = g_rand_int(rand) & 0xffffff;
        c->stencil[i] = g_rand_int(rand) & 0xff;
    }
    corpus_texture(c, rand);

    /*
     * Ownership: the window somewhere on the planes, one to four
     * rectangles of it, or rectangles the window wholly misses.
     */
    drawable.origin[0] = g_rand_int_range(rand, -10, CW / 4);
    drawable.origin[1] = g_rand_int_range(rand, -10, CH / 4);
    drawable.width = CW;
    drawable.height = CH;
    nclip = g_rand_int_range(rand, 1, 5);
    drawable.clip.n = nclip;
    for (unsigned i = 0; i < nclip; i++) {
        int x0 = g_rand_int_range(rand, 0, CW * 3 / 4);
        int y0 = g_rand_int_range(rand, 0, CH * 3 / 4);

        drawable.clip.r[i] = (SGIClipRect) {
            x0, y0, MIN(CW - 1, x0 + g_rand_int_range(rand, CW / 8, CW)),
            MIN(CH - 1, y0 + g_rand_int_range(rand, CH / 8, CH)),
        };
    }
    if (corpus_chance(rand, 30)) {
        drawable.clip.r[0] = (SGIClipRect) { 0, 0, CW - 1, CH - 1 };
    }

    sgi_gl_init(&ctx, 1);
    sgi_gl_set_drawable(&ctx, &drawable);
    corpus_state(rand, &ctx.cur);
    vp[0] = g_rand_int_range(rand, -20, 20);
    vp[1] = g_rand_int_range(rand, -20, 20);
    vp[2] = corpus_chance(rand, 50) ? 128 : g_rand_int_range(rand, 1, CW);
    vp[3] = corpus_chance(rand, 50) ? 64 : g_rand_int_range(rand, 1, CH);
    memcpy(ctx.cur.viewport, vp, sizeof(vp));
    if (!rgba && corpus_chance(rand, 40)) {
        ctx.cur.index_bits = 8;
        ctx.cur.index_mask = corpus_chance(rand, 70) ? 0xff : g_rand_int(rand);
        ctx.cur.index_logic_op = corpus_chance(rand, 10);
    }
    if (corpus_chance(rand, 35)) {
        ctx.cur.texture_2d = 1;
        ctx.cur.texture_env_mode = g_rand_boolean(rand) ? SGI_GL_MODULATE :
                                   SGI_GL_DECAL;
    }

    d.mode = corpus_modes[g_rand_int_range(rand, 0,
                                           ARRAY_SIZE(corpus_modes))];
    d.count = d.mode == SGI_GL_QUADS ? 4 * g_rand_int_range(rand, 1, 6) :
              d.mode == SGI_GL_TRIANGLES || d.mode == SGI_GL_POLYGON ?
              3 * g_rand_int_range(rand, 1, 8) :
              g_rand_int_range(rand, 1, CVERTS + 1);
    d.count = MIN(d.count, CVERTS);
    for (unsigned i = 0; i < d.count; i += 3) {
        double pos[3][2], w[3];

        corpus_triple(rand, shape, vp, rules.subpixel_bits, pos, w);
        for (unsigned j = 0; j < 3 && i + j < d.count; j++) {
            corpus_vertex(rand, &verts[(i + j) * SGI_GL_VERTEX_FLOATS], vp,
                          pos[j], w[j], shape == SHAPE_HUGE);
        }
    }
    d.state = &ctx.cur;
    d.back_colors = corpus_chance(rand, 20);
    d.point_size = g_rand_int_range(rand, 1, 6);
    d.line_width = g_rand_int_range(rand, 1, 4);
    d.point_smooth = corpus_chance(rand, 10);
    d.line_smooth = corpus_chance(rand, 10);
    d.polygon_smooth = corpus_chance(rand, 5);
    if (corpus_chance(rand, 10)) {
        for (unsigned i = 0; i < 32; i++) {
            stipple[i] = g_rand_int(rand);
        }
        d.polygon_stipple = stipple;
        d.polygon_stipple_enabled = true;
    }
    if (corpus_chance(rand, 5)) {
        modes[0] = modes[1] = g_rand_boolean(rand) ? SGI_GL_LINE :
                                                     SGI_GL_POINT;
        d.polygon_modes = modes;
    }

    if (corpus_chance(rand, 40)) {
        /*
         * The state most drawing uses: none of the per-fragment work beyond
         * interpolation, the depth test and the store, so the cases reach
         * whatever shorter path the rasterizer keeps for it.
         */
        SGIGLDrawState *st = &ctx.cur;

        st->texture_2d = 0;
        st->fog = st->clip_planes = st->alpha_test = st->stencil_test = 0;
        st->blend = st->color_logic_op = st->index_logic_op = 0;
        st->index_bits = 0;
        st->colour_mask = corpus_chance(rand, 90) ? SGI_GL_COLOUR_MASK_ALL :
                          0;
        st->depth_test = corpus_chance(rand, 70);
        d.polygon_stipple_enabled = false;
        d.polygon_modes = NULL;
        d.polygon_smooth = false;
    }
    t = (SGIRasterTarget) {
        .store_colour = rgba ? NULL : corpus_store_colour,
        .load_colour = rgba ? NULL : corpus_load_colour,
        .store_index = corpus_store_index,
        .store_rgba = rgba ? corpus_store_rgba : NULL,
        .load_rgba = rgba ? corpus_load_rgba : NULL,
        .load_depth = corpus_load_depth,
        .store_depth = corpus_store_depth,
        .load_stencil = rules.stencil_bits ? corpus_load_stencil : NULL,
        .store_stencil = rules.stencil_bits ? corpus_store_stencil : NULL,
        .texture = ctx.cur.texture_2d ? &c->image : NULL,
        .stipple_opaque = corpus_chance(rand, 30),
        .stipple_background = { 0.25, 0.5, 0.75, 1 },
        .stipple_background_index = 7,
        .opaque = c,
#ifdef SGI_RASTER_PARALLEL_MAX_BANDS
        .concurrent_pixels = true,
#endif
    };
    sgi_soft_draw(&rules, &t, &d);
    if (corpus_chance(rand, 15)) {
        /* A clear of part of the window, under the window's rectangles. */
        SGIRasterOp op = {
            .kind = SGI_RASTER_CLEAR, .ntargets = 1,
            .x0 = g_rand_int_range(rand, 0, CW),
            .y0 = g_rand_int_range(rand, 0, CH),
            .clip = drawable.clip,
            .clear_rgba = { g_rand_double(rand), g_rand_double(rand),
                            g_rand_double(rand), g_rand_double(rand) },
            .clear_colour_mask = corpus_chance(rand, 70) ?
                                 SGI_GL_COLOUR_MASK_ALL :
                                 g_rand_int_range(rand, 0, 16),
            .clear_depth24 = g_rand_int(rand) & 0xffffff,
            .clear_stencil = g_rand_int_range(rand, 0, 256),
            .stencil_mask = g_rand_int(rand),
            .index_bits = ctx.cur.index_bits,
            .clear_index = g_rand_int_range(rand, 0, 256),
            .index_mask = ctx.cur.index_mask,
        };

        op.clear_buffers = (corpus_chance(rand, 70) ?
                            SGI_GL_COLOR_BUFFER_BIT : 0) |
                           (corpus_chance(rand, 50) ?
                            SGI_GL_DEPTH_BUFFER_BIT : 0) |
                           (corpus_chance(rand, 30) ?
                            SGI_GL_STENCIL_BUFFER_BIT : 0);
        op.x1 = MIN(CW - 1, op.x0 + g_rand_int_range(rand, 0, CW));
        op.y1 = MIN(CH - 1, op.y0 + g_rand_int_range(rand, 0, CH));
        sgi_soft_raster(&rules, &t, &op);
    }
    if (corpus_chance(rand, 15)) {
        /* A glBitmap run, textured from the raster position. */
        uint8_t bytes[24 * 16 / 8];
        SGIRasterOp op = {
            .kind = SGI_RASTER_BITMAP, .ntargets = 1,
            .bitmap = {
                .state = &ctx.cur,
                .x = g_rand_int_range(rand, -8, CW),
                .y = g_rand_int_range(rand, -8, CH),
                .width = g_rand_int_range(rand, 1, 25),
                .height = g_rand_int_range(rand, 1, 17),
                .bytes = bytes,
                .z = g_rand_double(rand),
                .rgba = { g_rand_double(rand), g_rand_double(rand),
                          g_rand_double(rand), 1 },
                .index = g_rand_int_range(rand, 0, 256),
                .texcoord = { g_rand_double_range(rand, -2, 3),
                              g_rand_double_range(rand, -2, 3), 0, 1 },
            },
        };

        for (unsigned i = 0; i < sizeof(bytes); i++) {
            bytes[i] = g_rand_int(rand);
        }
        op.bitmap.count = (op.bitmap.width + 7) / 8 * op.bitmap.height;
        sgi_soft_raster(&rules, &t, &op);
    }
    if (t.texture && corpus_chance(rand, 30)) {
        /*
         * New texel contents between two draws of one image: whatever a
         * draw remembers of the first must not reach the second.
         */
        corpus_texels(c, rand);
        sgi_soft_draw(&rules, &t, &d);
    }
    sgi_gl_cleanup(&ctx);
    g_rand_free(rand);
    return t.texture;
}

static void corpus_case(unsigned index, uint64_t *planes, uint64_t *calls)
{
    g_autofree Corpus *c = g_new0(Corpus, 1);

    corpus_draw(index, c);
    *planes = corpus_planes(c);
    *calls = corpus_calls(c);
}

/*
 * Draw each textured case of the corpus by the double-precision texture
 * path and by the default one, and compare their planes: colour where
 * either path wrote it, the rest exactly. The default path must also
 * draw exactly what the general per-fragment loop draws with the same
 * sampler, its specialised row loops aside.
 */
static void corpus_diff(unsigned count, bool verbose)
{
    Diff all = { 0 };
    unsigned textured = 0, inexact = 0, unlike = 0;

    for (unsigned i = 0; i < count; i++) {
        g_autofree Corpus *a = g_new0(Corpus, 1);
        g_autofree Corpus *b = g_new0(Corpus, 1);
        g_autofree Corpus *g = g_new0(Corpus, 1);
        Diff diff = { 0 };
        bool exact;

        sgi_raster_set_texture_path(SGI_RASTER_TEXTURE_DOUBLE);
        if (!corpus_draw(i, a)) {
            continue;
        }
        sgi_raster_set_texture_path(SGI_RASTER_TEXTURE_FIXED);
        corpus_draw(i, g);
        sgi_raster_set_texture_path(SGI_RASTER_TEXTURE_SPANS);
        corpus_draw(i, b);
        textured++;
        if (memcmp(b->colour, g->colour, sizeof(b->colour)) ||
            memcmp(b->depth, g->depth, sizeof(b->depth)) ||
            memcmp(b->stencil, g->stencil, sizeof(b->stencil)) ||
            memcmp(b->written, g->written, sizeof(b->written))) {
            printf("case %u: row loops and general loop differ\n", i);
            unlike++;
        }
        exact = !memcmp(a->depth, b->depth, sizeof(a->depth)) &&
                !memcmp(a->stencil, b->stencil, sizeof(a->stencil)) &&
                !memcmp(a->written, b->written, sizeof(a->written));
        for (unsigned p = 0; p < 2; p++) {
            for (unsigned k = 0; k < CW * CH; k++) {
                if (a->written[k] & 1) {
                    diff_colour(&diff, a->colour[p][k], b->colour[p][k], 4);
                } else if (a->colour[p][k] != b->colour[p][k]) {
                    exact = false;
                }
            }
        }
        inexact += !exact;
        all.pixels += diff.pixels;
        all.differing += diff.differing;
        all.channels += diff.channels;
        all.sum += diff.sum;
        all.max = MAX(all.max, diff.max);
        if (verbose || !exact) {
            printf("case %u: ", i);
            diff_print("textured", &diff, exact);
        }
    }
    printf("corpus %u cases, %u textured, %u inexact, %u where row loops "
           "differ from the general loop\n", count, textured, inexact,
           unlike);
    diff_print("corpus", &all, !inexact);
}

static void corpus_run(unsigned count, bool verbose)
{
    uint64_t planes_all = 0xcbf29ce484222325ull;
    uint64_t calls_all = planes_all;
    int64_t start = get_clock();

    for (unsigned i = 0; i < count; i++) {
        uint64_t planes, calls;

        corpus_case(i, &planes, &calls);
        planes_all = fnv(planes_all, planes);
        calls_all = fnv(calls_all, calls);
        if (verbose) {
            printf("case %u planes %016" PRIx64 " calls %016" PRIx64 "\n",
                   i, planes, calls);
        }
    }
    printf("corpus %u cases %9.3f ms planes %016" PRIx64 " calls %016"
           PRIx64 "\n", count, (get_clock() - start) / 1e6, planes_all,
           calls_all);
}

/*
 * Time single-triangle draws of growing size, each a draw of its own as
 * immediate-mode clients send them: microseconds a draw and the pixels it
 * covers, for choosing the pixel count below which a draw stays on one
 * thread. Run once with -j 1 and once with more bands.
 */
static void sizes_run(unsigned repeats)
{
    static const unsigned legs[] = { 8, 16, 32, 64, 96, 128, 192, 256, 384,
                                     512 };
    Bench *b = g_new(Bench, 1);
    SGIRasterTarget t;
    SGIGLContext c;

    bench_init(b);
    t = bench_target(b, true);
    context_init(b, &c);
    c.cur.depth_test = 1;
    for (unsigned l = 0; l < ARRAY_SIZE(legs); l++) {
        unsigned draws = MAX(20, 400000 / (legs[l] * legs[l]));
        GRand *rand = g_rand_new_with_seed(legs[l]);
        int64_t start, end;

        start = get_clock();
        for (unsigned i = 0; i < draws * MAX(repeats, 1); i++) {
            float v[3][SGI_GL_VERTEX_FLOATS] = { 0 };
            double x = g_rand_double_range(rand, 0, W - legs[l]);
            double y = g_rand_double_range(rand, 0, H - legs[l]);
            double px[3][2] = {
                { x, y }, { x + legs[l], y }, { x, y + legs[l] },
            };
            SGIGLDraw d = {
                .context = 1, .mode = SGI_GL_TRIANGLES, .count = 3,
                .vertices = &v[0][0], .state = &c.cur,
            };

            for (unsigned k = 0; k < 3; k++) {
                v[k][SGI_GL_V_POSITION + 0] = 2 * px[k][0] / W - 1;
                v[k][SGI_GL_V_POSITION + 1] = 2 * px[k][1] / H - 1;
                v[k][SGI_GL_V_POSITION + 3] = 1;
                for (unsigned j = 0; j < 4; j++) {
                    v[k][SGI_GL_V_COLOR + j] = g_rand_double(rand);
                }
            }
            sgi_soft_draw(&rules_rgba, &t, &d);
        }
        end = get_clock();
        printf("size %3u pixels %6u draws %6u %9.3f us/draw\n", legs[l],
               legs[l] * legs[l] / 2, draws * MAX(repeats, 1),
               (end - start) / 1e3 / (draws * MAX(repeats, 1)));
        g_rand_free(rand);
    }
    sgi_gl_cleanup(&c);
    bench_cleanup(b);
    g_free(b);
}

int main(int argc, char **argv)
{
    unsigned repeats = 1, cases = 0, bands = 1;
    uint64_t min_pixels = 0;
    bool verbose = false, sizes = false, diff = false;
    int only = -1;
    int opt;

    while ((opt = getopt(argc, argv, "s:r:c:j:m:vzdh")) != -1) {
        switch (opt) {
        case 'd':
            diff = true;
            break;
        case 'j':
            bands = atoi(optarg);
            break;
        case 'm':
            if (qemu_strtou64(optarg, NULL, 0, &min_pixels)) {
                fprintf(stderr, "bad pixel count %s\n", optarg);
                return 1;
            }
            break;
        case 'z':
            sizes = true;
            break;
        case 'c':
            cases = atoi(optarg);
            break;
        case 'v':
            verbose = true;
            break;
        case 's':
            for (only = 0; only < SCENE_COUNT; only++) {
                if (!strcmp(optarg, scene_names[only])) {
                    break;
                }
            }
            if (only == SCENE_COUNT) {
                fprintf(stderr, "unknown scene %s\n", optarg);
                return 1;
            }
            break;
        case 'r':
            repeats = atoi(optarg);
            break;
        default:
            fprintf(stderr, "usage: %s [-j bands] [-m min-pixels] "
                    "[-s scene] [-r repeats] | -c cases [-v] | -z | "
                    "-d [-s scene | -c cases [-v]]\n",
                    argv[0]);
            return opt != 'h';
        }
    }
#ifdef SGI_RASTER_PARALLEL_MAX_BANDS
    sgi_raster_set_parallel(bands, min_pixels);
#else
    if (bands > 1) {
        fprintf(stderr, "this rasterizer draws on one thread only\n");
        return 1;
    }
#endif
    if (sizes) {
        sizes_run(repeats);
        return 0;
    }
    if (diff && cases) {
        corpus_diff(cases, verbose);
        return 0;
    }
    if (diff) {
        for (int s = 0; s < SCENE_COUNT; s++) {
            if ((only < 0 || only == s) && s != SCENE_CLEAR) {
                scene_diff(s);
            }
        }
        return 0;
    }
    if (cases) {
        corpus_run(cases, verbose);
        return 0;
    }
    for (unsigned r = 0; r < repeats; r++) {
        for (int s = 0; s < SCENE_COUNT; s++) {
            if (only < 0 || only == s) {
                scene_run(s, r);
            }
        }
    }
    return 0;
}
