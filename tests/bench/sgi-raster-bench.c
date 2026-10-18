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
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qapi/error.h"
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
    .evidence = "benchmark",
};

static const SGIRasterRules rules_colour = {
    .subpixel_bits = 8,
    .color_bits = 8,
    .depth_bits = 24,
    .rounding = SGI_ROUND_HALF_AWAY,
    .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
    .arith = SGI_ARITH_DOUBLE,
    .line_rule = SGI_LINE_COVERAGE_DOUBLE,
    .evidence = "benchmark",
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
    };
}

static void bench_add_plane(Bench *b, uint64_t base, SGIPlaneFormat format,
                            const SGIPlane **out)
{
    SGIPlane plane = {
        .base = base, .len = (uint64_t)W * H * 4,
        .width = W, .height = H, .stride = W * 4,
        .format = format, .packing = SGI_PACK_BE_RED_HIGH,
        .bottom_origin = true, .evidence = "benchmark",
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

typedef enum Scene {
    SCENE_CLEAR,
    SCENE_SMOOTH,
    SCENE_TEXTURE,
    SCENE_MIPMAP,
    SCENE_BLEND,
    SCENE_COLOUR,
    SCENE_COUNT,
} Scene;

static const char *const scene_names[SCENE_COUNT] = {
    [SCENE_CLEAR] = "clear",
    [SCENE_SMOOTH] = "smooth",
    [SCENE_TEXTURE] = "texture",
    [SCENE_MIPMAP] = "mipmap",
    [SCENE_BLEND] = "blend",
    [SCENE_COLOUR] = "colour",
};

static void scene_run(Scene scene, unsigned repeat)
{
    bool rgba = scene != SCENE_COLOUR;
    const SGIRasterRules *rules = rgba ? &rules_rgba : &rules_colour;
    g_autofree float *verts = triangles(scene + 1,
                                        scene == SCENE_BLEND ? 0.5f : 1.0f);
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

    bench_init(b);
    t = bench_target(b, rgba);
    context_init(b, &c);
    d.state = &c.cur;
    switch (scene) {
    case SCENE_CLEAR:
        break;
    case SCENE_SMOOTH:
    case SCENE_COLOUR:
        c.cur.depth_test = 1;
        break;
    case SCENE_MIPMAP:
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
    printf("%-8s %u %9.3f ms %016" PRIx64 "\n", scene_names[scene], repeat,
           (end - start) / 1e6, bench_digest(b));
    sgi_gl_cleanup(&c);
    bench_cleanup(b);
    g_free(b);
}

int main(int argc, char **argv)
{
    unsigned repeats = 1;
    int only = -1;
    int opt;

    while ((opt = getopt(argc, argv, "s:r:h")) != -1) {
        switch (opt) {
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
            fprintf(stderr, "usage: %s [-s scene] [-r repeats]\n", argv[0]);
            return opt != 'h';
        }
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
