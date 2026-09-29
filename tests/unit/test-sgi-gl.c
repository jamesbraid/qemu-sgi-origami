/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The shared GL context and software rasterizer, on paths a family's
 * replay fixtures need not reach: whole batches of every primitive mode,
 * glFinish, the clear op a context builds, unsupported rule sets,
 * colour-index mode and glBitmap.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/rcu.h"
#include "hw/display/sgi/rasterizer.h"

#define W 64
#define H 64

/* A plane pair and a depth plane, indexed by plane coordinates. */
typedef struct Planes {
    uint32_t colour[2][W * H];
    uint32_t depth[W * H];
} Planes;

static unsigned planes_index(int x, int y)
{
    g_assert(x >= 0 && x < W && y >= 0 && y < H);
    return y * W + x;
}

/* Here a colour target is a plane number; its meaning is the family's. */
static void planes_store_colour(void *opaque, SGIColourTarget target, int x,
                                int y, uint32_t colour)
{
    Planes *p = opaque;

    g_assert_cmpuint(target, <, 2);
    p->colour[target][planes_index(x, y)] = colour;
}

/* An index plane keeps the bits the write mask leaves (4.2.2). */
static void planes_store_index(void *opaque, SGIColourTarget target, int x,
                               int y, uint32_t index, uint32_t mask)
{
    Planes *p = opaque;
    uint32_t *pixel;

    g_assert_cmpuint(target, <, 2);
    pixel = &p->colour[target][planes_index(x, y)];
    *pixel = (*pixel & ~mask) | (index & mask);
}

static uint32_t planes_load_depth(void *opaque, int x, int y)
{
    return ((Planes *)opaque)->depth[planes_index(x, y)];
}

static void planes_store_depth(void *opaque, int x, int y, uint32_t depth)
{
    ((Planes *)opaque)->depth[planes_index(x, y)] = depth;
}

static SGIRasterTarget planes_target(Planes *p)
{
    return (SGIRasterTarget) {
        .store_colour = planes_store_colour,
        .store_index = planes_store_index,
        .load_depth = planes_load_depth,
        .store_depth = planes_store_depth,
        .opaque = p,
    };
}

/*
 * The only rule values the rasterizer implements, which are currently
 * MGRAS's set.
 */
static const SGIRasterRules rules_supported = {
    .subpixel_bits = 8,
    .color_bits = 8,
    .depth_bits = 24,
    .rounding = SGI_ROUND_HALF_AWAY,
    .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
    .arith = SGI_ARITH_DOUBLE,
    .line_rule = SGI_LINE_COVERAGE_DOUBLE,
    .drop_nonpositive_w = true,
    .evidence = "test",
};

/* A context drawing into the whole of plane 0, smooth shaded. */
static void context_init(SGIGLContext *c)
{
    SGIGLDrawable d = {
        .bound = true,
        .clip = { .n = 1, .r[0] = { 0, 0, W - 1, H - 1 } },
        .front = 0,
        .back = 1,
    };

    sgi_gl_init(c, 0x1234);
    sgi_gl_set_drawable(c, &d);
    c->cur.viewport[2] = W;
    c->cur.viewport[3] = H;
}

typedef struct Recorder {
    const SGIRasterRules *rules;
    SGIRasterTarget target;
    unsigned draws;
    unsigned fences;
    uint32_t fence_context;
    SGIRasterOp op;
    unsigned ops;
} Recorder;

static void recorder_draw(void *opaque, const SGIGLDraw *d)
{
    Recorder *r = opaque;

    g_assert_cmphex(d->context, ==, 0x1234);
    g_assert_cmpuint(d->nmaterials, ==, 0);
    sgi_soft_draw(r->rules, &r->target, d);
    r->draws++;
}

static void recorder_raster(void *opaque, const SGIRasterOp *op)
{
    Recorder *r = opaque;

    r->op = *op;
    r->ops++;
}

static void recorder_fence(void *opaque, uint32_t context)
{
    Recorder *r = opaque;

    r->fences++;
    r->fence_context = context;
}

static SGIGLSink recorder_sink(Recorder *r)
{
    return (SGIGLSink) {
        .draw = recorder_draw,
        .raster = recorder_raster,
        .fence = recorder_fence,
        .opaque = r,
    };
}

static void command(SGIGLContext *c, const SGIGLSink *sink, SGIGLOp op,
                    const uint32_t *args, unsigned nargs)
{
    SGIGLCmd cmd = { .op = op, .nargs = nargs, .args = args };

    sgi_gl_command(c, sink, &cmd);
}

static uint32_t bits(float f)
{
    uint32_t b;

    memcpy(&b, &f, sizeof(b));
    return b;
}

#define NVERTS 9

/*
 * Each mode drawn as one batch through the rasterizer and as the pieces
 * the context sends vertex by vertex leaves the same pixels.
 */
static void test_batch_matches_pieces(void)
{
    static Planes batch, pieces;
    float verts[NVERTS][SGI_GL_VERTEX_FLOATS];
    GRand *rand = g_rand_new_with_seed(937);

    for (int i = 0; i < NVERTS; i++) {
        memset(verts[i], 0, sizeof(verts[i]));
        verts[i][SGI_GL_V_POSITION + 0] = g_rand_double_range(rand, -0.9, 0.9);
        verts[i][SGI_GL_V_POSITION + 1] = g_rand_double_range(rand, -0.9, 0.9);
        verts[i][SGI_GL_V_POSITION + 3] = 1.0f;
        for (int k = 0; k < 4; k++) {
            verts[i][SGI_GL_V_COLOR + k] = g_rand_double(rand);
        }
    }
    g_rand_free(rand);

    for (uint32_t mode = SGI_GL_POINTS; mode <= SGI_GL_POLYGON; mode++) {
        for (int flat = 0; flat < 2; flat++) {
            SGIGLContext c;
            SGIRasterTarget t = planes_target(&batch);
            Recorder rec = {
                .rules = &rules_supported,
                .target = planes_target(&pieces),
            };
            SGIGLSink sink = recorder_sink(&rec);
            SGIGLDraw d = {
                .context = 0x1234,
                .mode = mode,
                .count = NVERTS,
                .vertices = &verts[0][0],
            };
            uint32_t word[4];

            memset(&batch, 0, sizeof(batch));
            memset(&pieces, 0, sizeof(pieces));
            context_init(&c);
            c.cur.shade_model = flat ? SGI_GL_FLAT : SGI_GL_SMOOTH;
            d.state = &c.cur;
            sgi_soft_draw(&rules_supported, &t, &d);

            word[0] = mode;
            command(&c, &sink, SGI_GL_OP_BEGIN, word, 1);
            for (int i = 0; i < NVERTS; i++) {
                for (int k = 0; k < 4; k++) {
                    word[k] = bits(verts[i][SGI_GL_V_COLOR + k]);
                }
                command(&c, &sink, SGI_GL_OP_COLOR4F, word, 4);
                word[0] = bits(verts[i][SGI_GL_V_POSITION + 0]);
                word[1] = bits(verts[i][SGI_GL_V_POSITION + 1]);
                command(&c, &sink, SGI_GL_OP_VERTEX2F, word, 2);
            }
            command(&c, &sink, SGI_GL_OP_END, word, 0);

            g_assert_cmpuint(rec.draws, >, 0);
            g_assert(memcmp(&batch, &pieces, sizeof(batch)) == 0);
            for (int i = 0; i < W * H; i++) {
                if (batch.colour[0][i]) {
                    break;
                }
                g_assert_cmpint(i, <, W * H - 1);
            }
        }
    }
}

/* glFinish reports a fence with the context's identity, after its work. */
static void test_finish_is_a_fence(void)
{
    SGIGLContext c;
    Recorder rec = { .rules = &rules_supported };
    SGIGLSink sink = recorder_sink(&rec);

    context_init(&c);
    command(&c, &sink, SGI_GL_OP_FINISH, NULL, 0);
    g_assert_cmpuint(rec.fences, ==, 1);
    g_assert_cmphex(rec.fence_context, ==, 0x1234);

    sink.fence = NULL;
    command(&c, &sink, SGI_GL_OP_FINISH, NULL, 0);
    g_assert_cmpuint(rec.fences, ==, 1);
}

/*
 * glClear becomes one CLEAR op over the drawable with the draw buffers'
 * targets and the context's clear values; the depth buffer drops out while
 * the depth mask is off, and nothing is sent without a window.
 */
static void test_clear_op(void)
{
    SGIGLContext c;
    Recorder rec = { .rules = &rules_supported };
    SGIGLSink sink = recorder_sink(&rec);
    uint32_t word[4] = { bits(0.25f), bits(0.5f), bits(0.75f), bits(1) };
    SGIGLDrawable unbound = { .bound = false };

    context_init(&c);
    command(&c, &sink, SGI_GL_OP_CLEAR_COLOR, word, 4);
    word[0] = 0x123456;
    command(&c, &sink, SGI_GL_OP_CLEAR_DEPTH, word, 1);
    word[0] = SGI_GL_DRAW_FRONT | SGI_GL_DRAW_BACK;
    command(&c, &sink, SGI_GL_OP_DRAW_BUFFERS, word, 1);
    word[0] = SGI_GL_COLOR_BUFFER_BIT | SGI_GL_DEPTH_BUFFER_BIT;
    command(&c, &sink, SGI_GL_OP_CLEAR, word, 1);
    g_assert_cmpuint(rec.ops, ==, 1);
    g_assert_cmpuint(rec.op.kind, ==, SGI_RASTER_CLEAR);
    g_assert_cmpuint(rec.op.ntargets, ==, 2);
    g_assert_cmpuint(rec.op.targets[0], ==, 0);
    g_assert_cmpuint(rec.op.targets[1], ==, 1);
    g_assert_cmphex(rec.op.clear_buffers, ==,
                    SGI_GL_COLOR_BUFFER_BIT | SGI_GL_DEPTH_BUFFER_BIT);
    g_assert_cmpfloat(rec.op.clear_rgba[2], ==, 0.75f);
    g_assert_cmphex(rec.op.clear_depth24, ==, 0x123456);
    g_assert_cmpint(rec.op.x1, ==, W - 1);
    g_assert_cmpint(rec.op.y1, ==, H - 1);

    word[0] = 0;
    command(&c, &sink, SGI_GL_OP_DEPTH_MASK, word, 1);
    word[0] = SGI_GL_DEPTH_BUFFER_BIT;
    command(&c, &sink, SGI_GL_OP_CLEAR, word, 1);
    g_assert_cmpuint(rec.ops, ==, 1);
    word[0] = SGI_GL_COLOR_BUFFER_BIT | SGI_GL_DEPTH_BUFFER_BIT;
    command(&c, &sink, SGI_GL_OP_CLEAR, word, 1);
    g_assert_cmpuint(rec.ops, ==, 2);
    g_assert_cmphex(rec.op.clear_buffers, ==, SGI_GL_COLOR_BUFFER_BIT);

    sgi_gl_set_drawable(&c, &unbound);
    command(&c, &sink, SGI_GL_OP_CLEAR, word, 1);
    g_assert_cmpuint(rec.ops, ==, 2);
}

/*
 * One point whose snapped x and whose depth both fall on a half step,
 * below zero and at 0.5, under each rounding. With one subpixel bit and a
 * viewport starting at -8, x_ndc -5/32 maps to window x -1.25, which is
 * -2.5 subpixels; y_ndc 0 maps to row 8; z_ndc 0 is window depth 0.5,
 * which is 0.5 steps of a one-bit depth plane.
 */
static void test_rounding(void)
{
    static const struct {
        uint8_t rounding;
        int x;
        uint32_t depth;
    } cases[] = {
        { SGI_ROUND_FLOOR, -2, 0 },
        { SGI_ROUND_HALF_UP, -1, 1 },
        { SGI_ROUND_HALF_EVEN, -1, 0 },
        { SGI_ROUND_HALF_AWAY, -2, 1 },
    };

    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        static Planes p;
        SGIRasterRules rules = rules_supported;
        SGIRasterTarget t = planes_target(&p);
        SGIGLContext c;
        SGIGLDrawable d = {
            .bound = true,
            .origin = { 32, 32 },
            .clip = { .n = 1, .r[0] = { 0, 0, W - 1, H - 1 } },
            .front = 0,
            .back = 1,
        };
        float v[SGI_GL_VERTEX_FLOATS] = { -5.0f / 32, 0, 0, 1, 1, 1, 1, 1 };
        SGIGLDraw draw = {
            .mode = SGI_GL_POINTS,
            .count = 1,
            .vertices = v,
            .state = &c.cur,
        };

        rules.subpixel_bits = 1;
        rules.depth_bits = 1;
        rules.rounding = cases[i].rounding;
        memset(&p, 0xff, sizeof(p));
        sgi_gl_init(&c, 0);
        sgi_gl_set_drawable(&c, &d);
        c.cur.viewport[0] = -8;
        c.cur.viewport[2] = 16;
        c.cur.viewport[3] = 16;
        c.cur.depth_test = 1;
        c.cur.depth_func = SGI_GL_ALWAYS;
        sgi_soft_draw(&rules, &t, &draw);
        g_assert_cmphex(p.colour[0][planes_index(32 + cases[i].x, 32 + 8)],
                        ==, 0xffffff);
        g_assert_cmphex(p.depth[planes_index(32 + cases[i].x, 32 + 8)], ==,
                        cases[i].depth);
    }
}

/*
 * sgi_gl_set_matrix() loads the stack its slot names whatever the matrix
 * mode, and a draw then uses it; a slot the context does not keep
 * changes nothing.
 */
static void test_set_matrix(void)
{
    static Planes p;
    /* x and y shifted by a quarter of the viewport, column-major. */
    static const float shift[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0.25f, 0, 1,
    };
    static const float twos[16] = {
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    };
    SGIRasterTarget t = planes_target(&p);
    SGIGLContext c;
    float identity[16], v[SGI_GL_VERTEX_FLOATS] = { 0, 0, 0, 1, 1, 1, 1, 1 };
    SGIGLDraw draw = {
        .mode = SGI_GL_POINTS,
        .count = 1,
        .vertices = v,
        .state = &c.cur,
    };
    uint32_t word[1] = { SGI_GL_PROJECTION };

    context_init(&c);
    memcpy(identity, c.cur.projection, sizeof(identity));
    command(&c, NULL, SGI_GL_OP_MATRIX_MODE, word, 1);
    g_assert_true(sgi_gl_set_matrix(&c, SGI_GL_MODELVIEW, shift));
    g_assert(memcmp(c.cur.modelview, shift, sizeof(shift)) == 0);
    g_assert(memcmp(c.cur.projection, identity, sizeof(identity)) == 0);
    g_assert_cmphex(c.matrix_mode, ==, SGI_GL_PROJECTION);

    g_assert_false(sgi_gl_set_matrix(&c, 0x1800, twos));
    g_assert(memcmp(c.cur.modelview, shift, sizeof(shift)) == 0);
    g_assert(memcmp(c.cur.projection, identity, sizeof(identity)) == 0);

    /* The origin lands at window (48, 40) rather than (32, 32). */
    memset(&p, 0, sizeof(p));
    sgi_soft_draw(&rules_supported, &t, &draw);
    g_assert_cmphex(p.colour[0][planes_index(48, 40)], ==, 0xffffff);
    g_assert_cmphex(p.colour[0][planes_index(32, 32)], ==, 0);

    g_assert_true(sgi_gl_set_matrix(&c, SGI_GL_PROJECTION, twos));
    g_assert(memcmp(c.cur.projection, twos, sizeof(twos)) == 0);
    g_assert(memcmp(c.cur.modelview, shift, sizeof(shift)) == 0);
}

/*
 * A draw stores through the target of each draw buffer it selects, as
 * the drawable gave them and nothing else: here the front buffer is plane
 * 1 and the back plane 0.
 */
static void test_draw_targets(void)
{
    static Planes p;
    SGIRasterTarget t = planes_target(&p);
    SGIGLContext c;
    SGIGLDrawable d = {
        .bound = true,
        .clip = { .n = 1, .r[0] = { 0, 0, W - 1, H - 1 } },
        .front = 1,
        .back = 0,
    };
    float v[SGI_GL_VERTEX_FLOATS] = { 0, 0, 0, 1, 1, 1, 1, 1 };
    SGIGLDraw draw = {
        .mode = SGI_GL_POINTS,
        .count = 1,
        .vertices = v,
        .state = &c.cur,
    };
    SGIColourTarget targets[2];

    context_init(&c);
    sgi_gl_set_drawable(&c, &d);
    c.cur.draw_buffers = SGI_GL_DRAW_FRONT;
    g_assert_cmpuint(sgi_gl_draw_targets(&c.cur, targets), ==, 1);
    g_assert_cmpuint(targets[0], ==, 1);
    memset(&p, 0, sizeof(p));
    sgi_soft_draw(&rules_supported, &t, &draw);
    g_assert_cmphex(p.colour[1][planes_index(32, 32)], ==, 0xffffff);
    g_assert_cmphex(p.colour[0][planes_index(32, 32)], ==, 0);

    c.cur.draw_buffers = SGI_GL_DRAW_FRONT | SGI_GL_DRAW_BACK;
    memset(&p, 0, sizeof(p));
    sgi_soft_draw(&rules_supported, &t, &draw);
    g_assert_cmphex(p.colour[1][planes_index(32, 32)], ==, 0xffffff);
    g_assert_cmphex(p.colour[0][planes_index(32, 32)], ==, 0xffffff);

    c.cur.draw_buffers = 0;
    g_assert_cmpuint(sgi_gl_draw_targets(&c.cur, targets), ==, 0);
}

static unsigned counted_stores;

static void counting_store_colour(void *opaque, SGIColourTarget target,
                                  int x, int y, uint32_t colour)
{
    counted_stores++;
    planes_store_colour(opaque, target, x, y, colour);
}

/*
 * Front and back given as one target are one target: a draw and a clear
 * to both buffers store each pixel once.
 */
static void test_same_front_and_back(void)
{
    static Planes p;
    SGIRasterTarget t = planes_target(&p);
    SGIGLContext c;
    SGIGLDrawable d = {
        .bound = true,
        .clip = { .n = 1, .r[0] = { 32, 32, 32, 32 } },
        .front = 1,
        .back = 1,
    };
    float v[SGI_GL_VERTEX_FLOATS] = { 0, 0, 0, 1, 1, 1, 1, 1 };
    SGIGLDraw draw = {
        .mode = SGI_GL_POINTS,
        .count = 1,
        .vertices = v,
        .state = &c.cur,
    };
    Recorder rec = { .rules = &rules_supported };
    SGIGLSink sink = recorder_sink(&rec);
    SGIColourTarget targets[2];
    uint32_t word[1] = { SGI_GL_COLOR_BUFFER_BIT };

    context_init(&c);
    sgi_gl_set_drawable(&c, &d);
    c.cur.draw_buffers = SGI_GL_DRAW_FRONT | SGI_GL_DRAW_BACK;
    g_assert_cmpuint(sgi_gl_draw_targets(&c.cur, targets), ==, 1);
    g_assert_cmpuint(targets[0], ==, 1);

    t.store_colour = counting_store_colour;
    counted_stores = 0;
    sgi_soft_draw(&rules_supported, &t, &draw);
    g_assert_cmpuint(counted_stores, ==, 1);

    command(&c, &sink, SGI_GL_OP_CLEAR, word, 1);
    g_assert_cmpuint(rec.ops, ==, 1);
    g_assert_cmpuint(rec.op.ntargets, ==, 1);
    counted_stores = 0;
    sgi_soft_raster(&rules_supported, &t, &rec.op);
    g_assert_cmpuint(counted_stores, ==, 1);
}

/* A CLEAR op to two targets writes both of them, and nothing else. */
static void test_clear_two_targets(void)
{
    static Planes p;
    SGIRasterTarget t = planes_target(&p);
    SGIRasterOp op = {
        .kind = SGI_RASTER_CLEAR,
        .targets = { 1, 0 },
        .ntargets = 2,
        .x0 = 4,
        .y0 = 4,
        .x1 = 7,
        .y1 = 7,
        .clip = { .n = 1, .r[0] = { 4, 4, 7, 7 } },
        .clear_buffers = SGI_GL_COLOR_BUFFER_BIT,
        .clear_rgba = { 1, 0, 1, 1 },
        .clear_colour_mask = UINT32_MAX,
    };

    memset(&p, 0, sizeof(p));
    sgi_soft_raster(&rules_supported, &t, &op);
    g_assert_cmphex(p.colour[0][planes_index(5, 5)], ==, 0xff00ff);
    g_assert_cmphex(p.colour[1][planes_index(5, 5)], ==, 0xff00ff);
    g_assert_cmphex(p.colour[0][planes_index(8, 5)], ==, 0);
    g_assert_cmphex(p.colour[1][planes_index(5, 3)], ==, 0);
}

/* A rule set the rasterizer does not implement draws nothing. */
static void test_unsupported_rules(void)
{
    static Planes p;
    SGIRasterRules rules = rules_supported;
    SGIRasterTarget t = planes_target(&p);
    SGIRasterOp op = {
        .kind = SGI_RASTER_CLEAR,
        .targets = { 1 },
        .ntargets = 1,
        .x1 = W - 1,
        .y1 = H - 1,
        .clip = { .n = 1, .r[0] = { 0, 0, W - 1, H - 1 } },
        .clear_buffers = SGI_GL_COLOR_BUFFER_BIT,
        .clear_rgba = { 1, 1, 1, 1 },
        .clear_colour_mask = UINT32_MAX,
    };

    g_assert_true(sgi_raster_rules_supported(&rules_supported));
    rules.line_rule = SGI_LINE_BRESENHAM_DIAMOND;
    g_assert_false(sgi_raster_rules_supported(&rules));
    rules = rules_supported;
    rules.drop_nonpositive_w = false;
    g_assert_false(sgi_raster_rules_supported(&rules));
    rules = rules_supported;
    rules.subpixel_bits = 13;
    g_assert_false(sgi_raster_rules_supported(&rules));

    memset(&p, 0, sizeof(p));
    sgi_soft_raster(&rules, &t, &op);
    g_assert_cmphex(p.colour[1][0], ==, 0);
    sgi_soft_raster(&rules_supported, &t, &op);
    g_assert_cmphex(p.colour[1][0], ==, 0xffffff);
    g_assert_cmphex(p.colour[0][0], ==, 0);
}

/* The words sgi_gl_get() gives for pname, which it must keep. */
static unsigned get(const SGIGLContext *c, uint32_t pname,
                    uint32_t out[SGI_GL_GET_WORDS])
{
    unsigned n = 0;

    memset(out, 0xa5, SGI_GL_GET_WORDS * sizeof(uint32_t));
    g_assert_true(sgi_gl_get(c, pname, out, &n));
    g_assert_cmpuint(n, >=, 1);
    g_assert_cmpuint(n, <=, SGI_GL_GET_WORDS);
    return n;
}

/*
 * glGet of the matrices, the matrix mode, the viewport, the shade model
 * and the clear colour (OpenGL 1.2.1, 6.1.2 and tables 6.4 to 6.9): each
 * matrix column-major and from its own stack whatever the mode, the
 * enumerants as sent.
 */
static void test_get_state(void)
{
    static const uint32_t viewport[4] = { 3, 5, 120, 90 };
    uint32_t word[6], out[SGI_GL_GET_WORDS];
    SGIGLContext c;

    context_init(&c);
    /* A translation's x lands in column 3, element 12. */
    word[0] = bits(7), word[1] = bits(-2), word[2] = bits(0.5f);
    command(&c, NULL, SGI_GL_OP_TRANSLATE, word, 3);
    word[0] = SGI_GL_PROJECTION;
    command(&c, NULL, SGI_GL_OP_MATRIX_MODE, word, 1);
    /* glOrtho(0, 4, 0, 2, -1, 1): x scale 0.5, x offset -1. */
    word[0] = bits(0), word[1] = bits(4), word[2] = bits(0);
    word[3] = bits(2), word[4] = bits(-1), word[5] = bits(1);
    command(&c, NULL, SGI_GL_OP_ORTHO, word, 6);
    command(&c, NULL, SGI_GL_OP_VIEWPORT, viewport, 4);
    word[0] = SGI_GL_FLAT;
    command(&c, NULL, SGI_GL_OP_SHADE_MODEL, word, 1);
    word[0] = bits(0.25f), word[1] = bits(0.5f);
    word[2] = bits(0.75f), word[3] = bits(1);
    command(&c, NULL, SGI_GL_OP_CLEAR_COLOR, word, 4);

    g_assert_cmpuint(get(&c, SGI_GL_MODELVIEW_MATRIX, out), ==, 16);
    for (int i = 0; i < 16; i++) {
        static const float mv[16] = {
            1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 7, -2, 0.5f, 1,
        };

        g_assert_cmphex(out[i], ==, bits(mv[i]));
    }
    g_assert_cmpuint(get(&c, SGI_GL_PROJECTION_MATRIX, out), ==, 16);
    for (int i = 0; i < 16; i++) {
        static const float pr[16] = {
            0.5f, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1,
        };

        g_assert_cmphex(out[i], ==, bits(pr[i]));
    }
    g_assert_cmpuint(get(&c, SGI_GL_MATRIX_MODE, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_PROJECTION);
    g_assert_cmpuint(get(&c, SGI_GL_VIEWPORT, out), ==, 4);
    g_assert(memcmp(out, viewport, sizeof(viewport)) == 0);
    g_assert_cmpuint(get(&c, SGI_GL_SHADE_MODEL, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_FLAT);
    g_assert_cmpuint(get(&c, SGI_GL_COLOR_CLEAR_VALUE, out), ==, 4);
    g_assert_cmphex(out[0], ==, bits(0.25f));
    g_assert_cmphex(out[1], ==, bits(0.5f));
    g_assert_cmphex(out[2], ==, bits(0.75f));
    g_assert_cmphex(out[3], ==, bits(1));
}

static float get_float(const SGIGLContext *c, uint32_t pname, unsigned i)
{
    uint32_t out[SGI_GL_GET_WORDS];
    float f;

    g_assert_cmpuint(i, <, get(c, pname, out));
    memcpy(&f, &out[i], sizeof(f));
    return f;
}

/*
 * The current raster position (OpenGL 1.2.1, 2.12): initially (0, 0, 0,
 * 1), valid, at distance 0 and white; glRasterPos carries its point
 * through the modelview and projection matrices and the viewport, with
 * depth range [0, 1], keeps clip w, takes the eye distance and the
 * current colour clamped to [0, 1], and a point outside the view volume
 * only resets the valid bit.
 */
static void test_raster_position(void)
{
    static const uint32_t viewport[4] = { 10, 20, 100, 50 };
    uint32_t word[6], out[SGI_GL_GET_WORDS];
    SGIGLContext c;

    context_init(&c);
    g_assert_cmpuint(get(&c, SGI_GL_CURRENT_RASTER_POSITION, out), ==, 4);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmphex(out[1], ==, 0);
    g_assert_cmphex(out[2], ==, 0);
    g_assert_cmphex(out[3], ==, bits(1));
    g_assert_cmpuint(get(&c, SGI_GL_CURRENT_RASTER_POSITION_VALID, out),
                     ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_DISTANCE, 0), ==,
                      0);
    for (int i = 0; i < 4; i++) {
        g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_COLOR, i), ==,
                          1);
        g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_TEXTURE_COORDS,
                                    i), ==, i == 3);
    }

    /* Modelview: translate by (3, 4, 0); projection: glOrtho(0, 10, ...). */
    word[0] = bits(3), word[1] = bits(4), word[2] = bits(0);
    command(&c, NULL, SGI_GL_OP_TRANSLATE, word, 3);
    word[0] = SGI_GL_PROJECTION;
    command(&c, NULL, SGI_GL_OP_MATRIX_MODE, word, 1);
    word[0] = bits(0), word[1] = bits(10), word[2] = bits(0);
    word[3] = bits(10), word[4] = bits(-4), word[5] = bits(4);
    command(&c, NULL, SGI_GL_OP_ORTHO, word, 6);
    command(&c, NULL, SGI_GL_OP_VIEWPORT, viewport, 4);
    word[0] = bits(2), word[1] = bits(-0.5f), word[2] = bits(0.25f);
    command(&c, NULL, SGI_GL_OP_COLOR3F, word, 3);

    /*
     * Object (2, 1, 2): eye (5, 5, 2), clip (0, 0, -0.5, 1), window
     * (10 + 50, 20 + 25, 0.25).
     */
    word[0] = bits(2), word[1] = bits(1), word[2] = bits(2);
    command(&c, NULL, SGI_GL_OP_RASTER_POS3F, word, 3);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_POSITION, 0), ==,
                      60);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_POSITION, 1), ==,
                      45);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_POSITION, 2), ==,
                      0.25f);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_POSITION, 3), ==,
                      1);
    get(&c, SGI_GL_CURRENT_RASTER_POSITION_VALID, out);
    g_assert_cmphex(out[0], ==, 1);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_DISTANCE, 0), ==,
                      sqrtf(54));
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_COLOR, 0), ==, 1);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_COLOR, 1), ==, 0);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_COLOR, 2), ==,
                      0.25f);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_COLOR, 3), ==, 1);

    /* Object (6, 2, 0, 2): eye (12, 10, 0, 2), clip x 0.4 over w 2. */
    word[0] = bits(6), word[1] = bits(2), word[2] = bits(0), word[3] = bits(2);
    command(&c, NULL, SGI_GL_OP_RASTER_POS4F, word, 4);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_POSITION, 0), ==,
                      70);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_POSITION, 3), ==,
                      2);

    /* x beyond the view volume: only the valid bit changes. */
    word[0] = bits(8), word[1] = bits(1);
    command(&c, NULL, SGI_GL_OP_RASTER_POS2F, word, 2);
    get(&c, SGI_GL_CURRENT_RASTER_POSITION_VALID, out);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_POSITION, 3), ==,
                      2);

    /*
     * A point the projection takes to clip (0, 0, 0, 0) is culled, not
     * divided by its zero w.
     */
    word[0] = bits(2), word[1] = bits(1);
    command(&c, NULL, SGI_GL_OP_RASTER_POS2F, word, 2);
    get(&c, SGI_GL_CURRENT_RASTER_POSITION_VALID, out);
    g_assert_cmphex(out[0], ==, 1);
    word[0] = word[1] = word[2] = word[3] = bits(0);
    command(&c, NULL, SGI_GL_OP_RASTER_POS4F, word, 4);
    get(&c, SGI_GL_CURRENT_RASTER_POSITION_VALID, out);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_POSITION, 3), ==,
                      1);

    /* Back inside, with z 0 by default: window z 0.5. */
    word[0] = bits(2), word[1] = bits(1);
    command(&c, NULL, SGI_GL_OP_RASTER_POS2F, word, 2);
    get(&c, SGI_GL_CURRENT_RASTER_POSITION_VALID, out);
    g_assert_cmphex(out[0], ==, 1);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_POSITION, 2), ==,
                      0.5f);
}


/*
 * Colour-index state (OpenGL 1.2.1, 2.7, 2.12, 4.2.2, 4.2.3 and table
 * 6.5): the current index starts at 1, the clear index at 0 and the write
 * mask at all ones; glRasterPos takes the current index as the raster
 * index, and the context stays in RGBA mode until its visual says
 * otherwise.
 */
static void test_index_state(void)
{
    uint32_t word[2], out[SGI_GL_GET_WORDS];
    SGIGLContext c;

    context_init(&c);
    g_assert_cmpuint(c.cur.index_bits, ==, 0);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_INDEX, 0), ==, 1);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_INDEX, 0), ==, 1);
    g_assert_cmpfloat(get_float(&c, SGI_GL_INDEX_CLEAR_VALUE, 0), ==, 0);
    get(&c, SGI_GL_INDEX_WRITEMASK, out);
    g_assert_cmphex(out[0], ==, UINT32_MAX);

    word[0] = bits(37.5f);
    command(&c, NULL, SGI_GL_OP_INDEXF, word, 1);
    word[0] = 0x0f;
    command(&c, NULL, SGI_GL_OP_INDEX_MASK, word, 1);
    word[0] = bits(3.25f);
    command(&c, NULL, SGI_GL_OP_CLEAR_INDEX, word, 1);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_INDEX, 0), ==, 37.5f);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_INDEX, 0), ==, 1);
    g_assert_cmpfloat(get_float(&c, SGI_GL_INDEX_CLEAR_VALUE, 0), ==, 3.25f);
    get(&c, SGI_GL_INDEX_WRITEMASK, out);
    g_assert_cmphex(out[0], ==, 0x0f);

    word[0] = bits(0), word[1] = bits(0);
    command(&c, NULL, SGI_GL_OP_RASTER_POS2F, word, 2);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_INDEX, 0), ==,
                      37.5f);

    sgi_gl_set_index_mode(&c, 8);
    g_assert_cmpuint(c.cur.index_bits, ==, 8);
    sgi_gl_set_index_mode(&c, 0);
    g_assert_cmpuint(c.cur.index_bits, ==, 0);
}

/*
 * An index as a buffer of n bits stores it (2.13.9): the integer part,
 * masked to n bits.
 */
static void test_index_value(void)
{
    g_assert_cmphex(sgi_gl_index_value(8, 2.0), ==, 2);
    g_assert_cmphex(sgi_gl_index_value(8, 2.99), ==, 2);
    g_assert_cmphex(sgi_gl_index_value(8, 300), ==, 44);
    g_assert_cmphex(sgi_gl_index_value(8, -1), ==, 0xff);
    g_assert_cmphex(sgi_gl_index_value(12, 4095.5), ==, 0xfff);
    g_assert_cmphex(sgi_gl_index_value(32, 4294967295.0), ==, UINT32_MAX);
    g_assert_cmphex(sgi_gl_index_value(8, NAN), ==, 0);
}

/*
 * In colour-index mode a draw stores the vertex's index, not its colour:
 * flat from the provoking vertex, smooth interpolated like a colour
 * component, and a clear stores the clear index; each under the write
 * mask. Planes without an index store draw nothing.
 */
static void test_index_draw(void)
{
    static Planes p;
    Recorder rec = { .rules = &rules_supported };
    SGIGLSink sink = recorder_sink(&rec);
    SGIGLContext c;
    uint32_t word[4];

    rec.target = planes_target(&p);
    memset(&p, 0, sizeof(p));
    context_init(&c);
    sgi_gl_set_index_mode(&c, 8);
    c.cur.shade_model = SGI_GL_FLAT;

    /* Window (8, 8), (40, 8), (8, 40): the last vertex's index, 300. */
    word[0] = SGI_GL_TRIANGLES;
    command(&c, &sink, SGI_GL_OP_BEGIN, word, 1);
    word[0] = bits(5);
    command(&c, &sink, SGI_GL_OP_INDEXF, word, 1);
    word[0] = bits(-0.75f), word[1] = bits(-0.75f);
    command(&c, &sink, SGI_GL_OP_VERTEX2F, word, 2);
    word[0] = bits(0.25f);
    command(&c, &sink, SGI_GL_OP_VERTEX2F, word, 2);
    word[0] = bits(300);
    command(&c, &sink, SGI_GL_OP_INDEXF, word, 1);
    word[0] = bits(-0.75f), word[1] = bits(0.25f);
    command(&c, &sink, SGI_GL_OP_VERTEX2F, word, 2);
    command(&c, &sink, SGI_GL_OP_END, word, 0);
    g_assert_cmphex(p.colour[0][planes_index(12, 12)], ==, 44);
    g_assert_cmphex(p.colour[0][planes_index(50, 50)], ==, 0);
    g_assert_cmphex(p.colour[1][planes_index(12, 12)], ==, 0);

    /* The write mask keeps the stored index's high bits. */
    word[0] = 0x0f;
    command(&c, &sink, SGI_GL_OP_INDEX_MASK, word, 1);
    word[0] = bits(0xa3);
    command(&c, &sink, SGI_GL_OP_INDEXF, word, 1);
    word[0] = SGI_GL_POINTS;
    command(&c, &sink, SGI_GL_OP_BEGIN, word, 1);
    word[0] = bits(-0.75f + 4.0f / 32), word[1] = bits(-0.75f + 4.0f / 32);
    command(&c, &sink, SGI_GL_OP_VERTEX2F, word, 2);
    command(&c, &sink, SGI_GL_OP_END, word, 0);
    g_assert_cmphex(p.colour[0][planes_index(12, 12)], ==, 0x23);

    /*
     * Smooth: a segment along row 2 from x 0.5 with index 0 to x 16.5
     * with index 16 gives pixel k the index k.
     */
    word[0] = UINT32_MAX;
    command(&c, &sink, SGI_GL_OP_INDEX_MASK, word, 1);
    c.cur.shade_model = SGI_GL_SMOOTH;
    word[0] = SGI_GL_LINES;
    command(&c, &sink, SGI_GL_OP_BEGIN, word, 1);
    word[0] = bits(0);
    command(&c, &sink, SGI_GL_OP_INDEXF, word, 1);
    word[0] = bits(0.5f / 32 - 1), word[1] = bits(2.5f / 32 - 1);
    command(&c, &sink, SGI_GL_OP_VERTEX2F, word, 2);
    word[0] = bits(16);
    command(&c, &sink, SGI_GL_OP_INDEXF, word, 1);
    word[0] = bits(16.5f / 32 - 1);
    command(&c, &sink, SGI_GL_OP_VERTEX2F, word, 2);
    command(&c, &sink, SGI_GL_OP_END, word, 0);
    for (int k = 0; k < 16; k++) {
        g_assert_cmphex(p.colour[0][planes_index(k, 2)], ==, k);
    }

    /*
     * Smooth, in perspective, with one index at every vertex: every
     * fragment has exactly that index, never one step below it.
     */
    memset(&p, 0, sizeof(p));
    word[0] = bits(300);
    command(&c, &sink, SGI_GL_OP_INDEXF, word, 1);
    word[0] = SGI_GL_TRIANGLES;
    command(&c, &sink, SGI_GL_OP_BEGIN, word, 1);
    for (int i = 0; i < 3; i++) {
        static const float v[3][3] = {
            { -0.9f, -0.8f, 1 }, { 0.93f, -0.7f, 2.7f }, { -0.1f, 0.95f, 5.3f },
        };

        word[0] = bits(v[i][0] * v[i][2]), word[1] = bits(v[i][1] * v[i][2]);
        word[2] = bits(0), word[3] = bits(v[i][2]);
        command(&c, &sink, SGI_GL_OP_VERTEX4F, word, 4);
    }
    command(&c, &sink, SGI_GL_OP_END, word, 0);
    for (int i = 0, drawn = 0; i < W * H; i++) {
        if (p.colour[0][i]) {
            g_assert_cmphex(p.colour[0][i], ==, 44);
            drawn++;
        }
        if (i == W * H - 1) {
            g_assert_cmpint(drawn, >, 1000);
        }
    }

    /* A clear stores the clear index, masked to the index bits. */
    word[0] = bits(257.75f);
    command(&c, &sink, SGI_GL_OP_CLEAR_INDEX, word, 1);
    word[0] = SGI_GL_COLOR_BUFFER_BIT;
    command(&c, &sink, SGI_GL_OP_CLEAR, word, 1);
    g_assert_cmpuint(rec.op.index_bits, ==, 8);
    g_assert_cmphex(rec.op.clear_index, ==, 1);
    g_assert_cmphex(rec.op.index_mask, ==, UINT32_MAX);
    sgi_soft_raster(&rules_supported, &rec.target, &rec.op);
    g_assert_cmphex(p.colour[0][planes_index(40, 40)], ==, 1);
    g_assert_cmphex(p.colour[0][planes_index(12, 12)], ==, 1);

    /* No index store: nothing drawn. */
    rec.target.store_index = NULL;
    rec.op.clear_index = 9;
    sgi_soft_raster(&rules_supported, &rec.target, &rec.op);
    g_assert_cmphex(p.colour[0][planes_index(40, 40)], ==, 1);
}

/* A recorder that rasterizes each raster op as well as keeping it. */
static void drawing_raster(void *opaque, const SGIRasterOp *op)
{
    Recorder *r = opaque;

    recorder_raster(opaque, op);
    sgi_soft_raster(r->rules, &r->target, op);
}

/*
 * glBitmap (OpenGL 1.2.1, 3.7). With the raster position at window
 * (10.5, 20.5) and the origin at (2, 3), the image's lower-left corner is
 * at pixel (8, 17), and the position then moves by (xmove, ymove). The
 * 10x2 image is rows of two bytes, the bottom row first, the most
 * significant bit leftmost: 0x80 0x40 sets columns 0 and 9 of row 0, 0x01
 * 0xff sets column 7 and columns 8 and 9 of row 1, the rest of the last
 * byte lying beyond the width. A set bit takes the raster colour in RGBA
 * mode and the raster index in colour-index mode, and the image arrives
 * whole or in runs alike.
 */
static void test_bitmap(void)
{
    static const uint8_t image[4] = { 0x80, 0x40, 0x01, 0xff };
    static const struct {
        int x, y;
    } set[] = { { 8, 17 }, { 17, 17 }, { 15, 18 }, { 16, 18 }, { 17, 18 } };
    static Planes p;
    Recorder rec = { .rules = &rules_supported };
    SGIGLSink sink = recorder_sink(&rec);
    SGIGLContext c;
    SGIGLBitmap b;
    uint32_t word[4];
    unsigned drawn = 0;

    sink.raster = drawing_raster;
    rec.target = planes_target(&p);
    memset(&p, 0, sizeof(p));
    context_init(&c);
    word[0] = bits(1), word[1] = bits(0.5f), word[2] = bits(0);
    command(&c, &sink, SGI_GL_OP_COLOR3F, word, 3);
    /* Window (10.5, 20.5): NDC x 10.5 / 32 - 1, y 20.5 / 32 - 1. */
    word[0] = bits(10.5f / 32 - 1), word[1] = bits(20.5f / 32 - 1);
    command(&c, &sink, SGI_GL_OP_RASTER_POS2F, word, 2);
    g_assert_true(sgi_gl_bitmap_place(&c, 10, 2, 2, 3, 12, -1.5f, &b));
    g_assert_cmpint(b.x, ==, 8);
    g_assert_cmpint(b.y, ==, 17);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_POSITION, 0), ==,
                      22.5f);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_POSITION, 1), ==,
                      19);
    sgi_gl_bitmap_bytes(&c, &sink, &b, 0, image, 4);
    g_assert_cmpuint(rec.op.kind, ==, SGI_RASTER_BITMAP);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            bool on = false;

            for (int i = 0; i < ARRAY_SIZE(set); i++) {
                on |= set[i].x == x && set[i].y == y;
            }
            g_assert_cmphex(p.colour[0][planes_index(x, y)], ==,
                            on ? 0xff8000 : 0);
            drawn += on;
        }
    }
    g_assert_cmpuint(drawn, ==, ARRAY_SIZE(set));

    /* In colour-index mode, in two runs: the raster index. */
    memset(&p, 0, sizeof(p));
    sgi_gl_set_index_mode(&c, 8);
    word[0] = bits(0x42);
    command(&c, &sink, SGI_GL_OP_INDEXF, word, 1);
    word[0] = bits(10.5f / 32 - 1), word[1] = bits(20.5f / 32 - 1);
    command(&c, &sink, SGI_GL_OP_RASTER_POS2F, word, 2);
    g_assert_true(sgi_gl_bitmap_place(&c, 10, 2, 2, 3, 0, 0, &b));
    sgi_gl_bitmap_bytes(&c, &sink, &b, 0, image, 1);
    sgi_gl_bitmap_bytes(&c, &sink, &b, 1, image + 1, 3);
    for (int i = 0; i < ARRAY_SIZE(set); i++) {
        g_assert_cmphex(p.colour[0][planes_index(set[i].x, set[i].y)], ==,
                        0x42);
    }
    g_assert_cmphex(p.colour[0][planes_index(9, 17)], ==, 0);

    /* An invalid raster position neither draws nor moves. */
    word[0] = bits(2), word[1] = bits(0);
    command(&c, &sink, SGI_GL_OP_RASTER_POS2F, word, 2);
    g_assert_false(sgi_gl_bitmap_place(&c, 10, 2, 0, 0, 5, 5, &b));
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_POSITION, 0), ==,
                      10.5f);
}

/*
 * A bitmap's fragments are bounded by the window, not the viewport, and
 * pass the depth test at the raster position's depth.
 */
static void test_bitmap_bounds_and_depth(void)
{
    static const uint8_t ones[8] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    static Planes p;
    Recorder rec = { .rules = &rules_supported };
    SGIGLSink sink = recorder_sink(&rec);
    SGIGLContext c;
    SGIGLBitmap b;
    uint32_t word[2];

    sink.raster = drawing_raster;
    rec.target = planes_target(&p);
    memset(&p, 0, sizeof(p));
    context_init(&c);
    /* A 16x4 image from (56, 1) runs off the 64-pixel window's edge. */
    word[0] = bits(56.5f / 32 - 1), word[1] = bits(1.5f / 32 - 1);
    command(&c, &sink, SGI_GL_OP_RASTER_POS2F, word, 2);
    c.cur.viewport[2] = 8;
    c.cur.viewport[3] = 8;
    g_assert_true(sgi_gl_bitmap_place(&c, 16, 4, 0, 0, 0, 0, &b));
    sgi_gl_bitmap_bytes(&c, &sink, &b, 0, ones, 8);
    g_assert_cmphex(p.colour[0][planes_index(56, 1)], ==, 0xffffff);
    g_assert_cmphex(p.colour[0][planes_index(63, 4)], ==, 0xffffff);
    g_assert_cmphex(p.colour[0][planes_index(55, 1)], ==, 0);
    g_assert_cmphex(p.colour[0][planes_index(56, 5)], ==, 0);

    /* Raster depth 0.5 against a stored 0.25 under LESS: nothing drawn. */
    memset(&p, 0, sizeof(p));
    for (int i = 0; i < W * H; i++) {
        p.depth[i] = 0x400000;
    }
    c.cur.depth_test = 1;
    sgi_gl_bitmap_bytes(&c, &sink, &b, 0, ones, 8);
    g_assert_cmphex(p.colour[0][planes_index(56, 1)], ==, 0);
    for (int i = 0; i < W * H; i++) {
        p.depth[i] = 0xc00000;
    }
    sgi_gl_bitmap_bytes(&c, &sink, &b, 0, ones, 8);
    g_assert_cmphex(p.colour[0][planes_index(56, 1)], ==, 0xffffff);
    g_assert_cmphex(p.depth[planes_index(56, 1)], ==, 0x800000);
}

/*
 * The raster position's z is already a window depth, through the depth
 * range when glRasterPos set it (2.12), and a bitmap's fragments take it
 * as it is (3.7). With glDepthRange(0.5, 1) a raster position at
 * normalized depth 0.5 has window depth 0.75, stored as 0xbfffff in 24
 * bits; mapped through the range a second time it would be 0.875,
 * 0xdfffff.
 */
static void test_bitmap_depth_is_the_raster_depth(void)
{
    static const uint8_t one = 0x80;
    static Planes p;
    Recorder rec = { .rules = &rules_supported };
    SGIGLSink sink = recorder_sink(&rec);
    SGIGLContext c;
    SGIGLBitmap b;
    uint32_t word[3];

    sink.raster = drawing_raster;
    rec.target = planes_target(&p);
    memset(&p, 0, sizeof(p));
    context_init(&c);
    word[0] = bits(0.5f), word[1] = bits(1);
    command(&c, &sink, SGI_GL_OP_DEPTH_RANGE, word, 2);
    word[0] = bits(10.5f / 32 - 1), word[1] = bits(20.5f / 32 - 1);
    word[2] = bits(0);
    command(&c, &sink, SGI_GL_OP_RASTER_POS3F, word, 3);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_POSITION, 2), ==,
                      0.75f);
    c.cur.depth_test = 1;
    c.cur.depth_func = SGI_GL_ALWAYS;
    g_assert_true(sgi_gl_bitmap_place(&c, 1, 1, 0, 0, 0, 0, &b));
    sgi_gl_bitmap_bytes(&c, &sink, &b, 0, &one, 1);
    g_assert_cmphex(p.colour[0][planes_index(10, 20)], ==, 0xffffff);
    g_assert_cmphex(p.depth[planes_index(10, 20)], ==, 0xbfffff);
}

/*
 * glDepthRange (OpenGL 1.2.1, 2.10.1): initially [0, 1]; each value is
 * clamped to [0, 1] and kept, and far may be below near.
 */
static void test_depth_range(void)
{
    uint32_t word[2], out[SGI_GL_GET_WORDS];
    SGIGLContext c;

    context_init(&c);
    g_assert_cmpuint(get(&c, SGI_GL_DEPTH_RANGE, out), ==, 2);
    g_assert_cmphex(out[0], ==, bits(0));
    g_assert_cmphex(out[1], ==, bits(1));

    word[0] = bits(0.75f), word[1] = bits(0.25f);
    command(&c, NULL, SGI_GL_OP_DEPTH_RANGE, word, 2);
    g_assert_cmpuint(get(&c, SGI_GL_DEPTH_RANGE, out), ==, 2);
    g_assert_cmphex(out[0], ==, bits(0.75f));
    g_assert_cmphex(out[1], ==, bits(0.25f));

    word[0] = bits(-2), word[1] = bits(3);
    command(&c, NULL, SGI_GL_OP_DEPTH_RANGE, word, 2);
    g_assert_cmpuint(get(&c, SGI_GL_DEPTH_RANGE, out), ==, 2);
    g_assert_cmphex(out[0], ==, bits(0));
    g_assert_cmphex(out[1], ==, bits(1));
}

/*
 * glIsEnabled of dither and the depth test, the depth function and write
 * mask, and the stack depths (OpenGL 1.2.1, 6.1.1, 6.1.2): initially
 * dither on, depth test off, GL_LESS, writes on, and one matrix a stack.
 */
static void test_get_enables_and_depths(void)
{
    uint32_t word[1], out[SGI_GL_GET_WORDS];
    SGIGLContext c;

    context_init(&c);
    g_assert_cmpuint(get(&c, SGI_GL_DITHER, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    g_assert_cmpuint(get(&c, SGI_GL_DEPTH_TEST, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmpuint(get(&c, SGI_GL_DEPTH_FUNC, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0x201);
    g_assert_cmpuint(get(&c, SGI_GL_DEPTH_WRITEMASK, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    g_assert_cmpuint(get(&c, SGI_GL_MODELVIEW_STACK_DEPTH, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    g_assert_cmpuint(get(&c, SGI_GL_PROJECTION_STACK_DEPTH, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);

    word[0] = SGI_GL_DITHER;
    command(&c, NULL, SGI_GL_OP_DISABLE, word, 1);
    word[0] = SGI_GL_DEPTH_TEST;
    command(&c, NULL, SGI_GL_OP_ENABLE, word, 1);
    word[0] = 0x206;
    command(&c, NULL, SGI_GL_OP_DEPTH_FUNC, word, 1);
    word[0] = 0;
    command(&c, NULL, SGI_GL_OP_DEPTH_MASK, word, 1);
    command(&c, NULL, SGI_GL_OP_PUSH_MATRIX, word, 0);
    command(&c, NULL, SGI_GL_OP_PUSH_MATRIX, word, 0);
    word[0] = SGI_GL_PROJECTION;
    command(&c, NULL, SGI_GL_OP_MATRIX_MODE, word, 1);
    command(&c, NULL, SGI_GL_OP_PUSH_MATRIX, word, 0);

    g_assert_cmpuint(get(&c, SGI_GL_DITHER, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmpuint(get(&c, SGI_GL_DEPTH_TEST, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    g_assert_cmpuint(get(&c, SGI_GL_DEPTH_FUNC, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0x206);
    g_assert_cmpuint(get(&c, SGI_GL_DEPTH_WRITEMASK, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmpuint(get(&c, SGI_GL_MODELVIEW_STACK_DEPTH, out), ==, 1);
    g_assert_cmphex(out[0], ==, 3);
    g_assert_cmpuint(get(&c, SGI_GL_PROJECTION_STACK_DEPTH, out), ==, 1);
    g_assert_cmphex(out[0], ==, 2);
}

static float light_float(const SGIGLContext *c, uint32_t light,
                         uint32_t pname, unsigned i)
{
    uint32_t out[SGI_GL_GET_WORDS];
    unsigned n = 0;
    float f;

    g_assert_true(sgi_gl_get_light(c, light, pname, out, &n));
    g_assert_cmpuint(i, <, n);
    memcpy(&f, &out[i], sizeof(f));
    return f;
}

static float material_float(const SGIGLContext *c, uint32_t face,
                            uint32_t pname, unsigned i)
{
    uint32_t out[SGI_GL_GET_WORDS];
    unsigned n = 0;
    float f;

    g_assert_true(sgi_gl_get_material(c, face, pname, out, &n));
    g_assert_cmpuint(i, <, n);
    memcpy(&f, &out[i], sizeof(f));
    return f;
}

static void light_command(SGIGLContext *c, SGIGLOp op, uint32_t a0,
                          uint32_t a1, float v0, float v1, float v2, float v3)
{
    uint32_t word[6] = { a0, a1, bits(v0), bits(v1), bits(v2), bits(v3) };

    command(c, NULL, op, word, 6);
}

/*
 * The lighting state's initial values (OpenGL 1.2.1, table 2.10): light 0
 * diffuse and specular white and the others black, positions (0, 0, 1, 0),
 * spot directions (0, 0, -1), cutoff 180, attenuation (1, 0, 0); both
 * faces' ambient 0.2, diffuse 0.8, colour indexes (0, 1, 1); light model
 * ambient 0.2, local viewer and two-sided off; colour material off, on
 * both faces' ambient and diffuse.
 */
static void test_lighting_initial_state(void)
{
    uint32_t out[SGI_GL_GET_WORDS];
    unsigned n;
    SGIGLContext c;

    context_init(&c);
    for (uint32_t l = 0; l < SGI_GL_LIGHTS; l++) {
        for (unsigned i = 0; i < 3; i++) {
            g_assert_cmpfloat(light_float(&c, l, SGI_GL_AMBIENT, i), ==, 0);
            g_assert_cmpfloat(light_float(&c, l, SGI_GL_DIFFUSE, i), ==,
                              l == 0);
            g_assert_cmpfloat(light_float(&c, l, SGI_GL_SPECULAR, i), ==,
                              l == 0);
            g_assert_cmpfloat(light_float(&c, l, SGI_GL_POSITION, i), ==,
                              i == 2);
            g_assert_cmpfloat(light_float(&c, l, SGI_GL_SPOT_DIRECTION, i),
                              ==, i == 2 ? -1 : 0);
        }
        g_assert_cmpfloat(light_float(&c, l, SGI_GL_DIFFUSE, 3), ==, 1);
        g_assert_cmpfloat(light_float(&c, l, SGI_GL_POSITION, 3), ==, 0);
        g_assert_cmpfloat(light_float(&c, l, SGI_GL_SPOT_EXPONENT, 0), ==, 0);
        g_assert_cmpfloat(light_float(&c, l, SGI_GL_SPOT_CUTOFF, 0), ==, 180);
        g_assert_cmpfloat(light_float(&c, l, SGI_GL_CONSTANT_ATTENUATION, 0),
                          ==, 1);
        g_assert_cmpfloat(light_float(&c, l, SGI_GL_LINEAR_ATTENUATION, 0),
                          ==, 0);
    }
    g_assert_false(sgi_gl_get_light(&c, SGI_GL_LIGHTS, SGI_GL_AMBIENT, out,
                                    &n));
    for (uint32_t face = SGI_GL_FRONT; face <= SGI_GL_BACK; face++) {
        g_assert_cmpfloat(material_float(&c, face, SGI_GL_AMBIENT, 0), ==,
                          0.2f);
        g_assert_cmpfloat(material_float(&c, face, SGI_GL_DIFFUSE, 1), ==,
                          0.8f);
        g_assert_cmpfloat(material_float(&c, face, SGI_GL_DIFFUSE, 3), ==, 1);
        g_assert_cmpfloat(material_float(&c, face, SGI_GL_SPECULAR, 0), ==, 0);
        g_assert_cmpfloat(material_float(&c, face, SGI_GL_EMISSION, 3), ==, 1);
        g_assert_cmpfloat(material_float(&c, face, SGI_GL_SHININESS, 0), ==,
                          0);
        g_assert_cmpfloat(material_float(&c, face, SGI_GL_COLOR_INDEXES, 0),
                          ==, 0);
        g_assert_cmpfloat(material_float(&c, face, SGI_GL_COLOR_INDEXES, 2),
                          ==, 1);
    }
    g_assert_false(sgi_gl_get_material(&c, SGI_GL_FRONT_AND_BACK,
                                       SGI_GL_AMBIENT, out, &n));
    g_assert_cmpfloat(get_float(&c, SGI_GL_LIGHT_MODEL_AMBIENT, 2), ==, 0.2f);
    g_assert_cmpfloat(get_float(&c, SGI_GL_LIGHT_MODEL_AMBIENT, 3), ==, 1);
    g_assert_cmpuint(get(&c, SGI_GL_LIGHT_MODEL_TWO_SIDE, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmpuint(get(&c, SGI_GL_COLOR_MATERIAL, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmpuint(get(&c, SGI_GL_COLOR_MATERIAL_FACE, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_FRONT_AND_BACK);
    g_assert_cmpuint(get(&c, SGI_GL_COLOR_MATERIAL_PARAMETER, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_AMBIENT_AND_DIFFUSE);
}

/*
 * glLightfv keeps the position in eye coordinates, through the modelview
 * current at the call, and the spot direction through its upper left
 * 3x3 (OpenGL 1.2.1, 2.13.2); out-of-range spot and attenuation values
 * change nothing. glMaterialfv sets the faces it names, both for
 * GL_FRONT_AND_BACK and ambient and diffuse for GL_AMBIENT_AND_DIFFUSE.
 * glLightModel sets the ambient and the two booleans.
 */
static void test_lighting_commands(void)
{
    uint32_t word[5], out[SGI_GL_GET_WORDS];
    SGIGLContext c;

    context_init(&c);
    word[0] = bits(3), word[1] = bits(4), word[2] = bits(5);
    command(&c, NULL, SGI_GL_OP_TRANSLATE, word, 3);
    light_command(&c, SGI_GL_OP_LIGHT, 2, SGI_GL_POSITION, 1, 2, 3, 1);
    light_command(&c, SGI_GL_OP_LIGHT, 2, SGI_GL_SPOT_DIRECTION, 0, 1, 0, 1);
    light_command(&c, SGI_GL_OP_LIGHT, 2, SGI_GL_SPOT_CUTOFF, 45, 0, 0, 0);
    light_command(&c, SGI_GL_OP_LIGHT, 2, SGI_GL_SPOT_CUTOFF, 95, 0, 0, 0);
    light_command(&c, SGI_GL_OP_LIGHT, 2, SGI_GL_SPOT_EXPONENT, 129, 0, 0, 0);
    light_command(&c, SGI_GL_OP_LIGHT, 2, SGI_GL_QUADRATIC_ATTENUATION, 0.5f,
                  0, 0, 0);
    light_command(&c, SGI_GL_OP_LIGHT, 2, SGI_GL_LINEAR_ATTENUATION, -1, 0,
                  0, 0);
    light_command(&c, SGI_GL_OP_LIGHT, 2, SGI_GL_DIFFUSE, 0.5f, 0.25f,
                  0.125f, 0.75f);

    g_assert_cmpfloat(light_float(&c, 2, SGI_GL_POSITION, 0), ==, 4);
    g_assert_cmpfloat(light_float(&c, 2, SGI_GL_POSITION, 1), ==, 6);
    g_assert_cmpfloat(light_float(&c, 2, SGI_GL_POSITION, 2), ==, 8);
    g_assert_cmpfloat(light_float(&c, 2, SGI_GL_POSITION, 3), ==, 1);
    g_assert_cmpfloat(light_float(&c, 2, SGI_GL_SPOT_DIRECTION, 0), ==, 0);
    g_assert_cmpfloat(light_float(&c, 2, SGI_GL_SPOT_DIRECTION, 1), ==, 1);
    g_assert_cmpfloat(light_float(&c, 2, SGI_GL_SPOT_DIRECTION, 2), ==, 0);
    g_assert_cmpfloat(light_float(&c, 2, SGI_GL_SPOT_CUTOFF, 0), ==, 45);
    g_assert_cmpfloat(light_float(&c, 2, SGI_GL_SPOT_EXPONENT, 0), ==, 0);
    g_assert_cmpfloat(light_float(&c, 2, SGI_GL_QUADRATIC_ATTENUATION, 0), ==,
                      0.5f);
    g_assert_cmpfloat(light_float(&c, 2, SGI_GL_LINEAR_ATTENUATION, 0), ==, 0);
    g_assert_cmpfloat(light_float(&c, 2, SGI_GL_DIFFUSE, 1), ==, 0.25f);
    g_assert_cmpfloat(light_float(&c, 2, SGI_GL_DIFFUSE, 3), ==, 0.75f);
    g_assert_cmpfloat(light_float(&c, 0, SGI_GL_POSITION, 0), ==, 0);

    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_BACK, SGI_GL_SPECULAR,
                  0.5f, 0.5f, 0.5f, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT_AND_BACK,
                  SGI_GL_AMBIENT_AND_DIFFUSE, 0.1f, 0.2f, 0.3f, 0.4f);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_SHININESS,
                  64, 99, 99, 99);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_SHININESS,
                  200, 0, 0, 0);
    g_assert_cmpfloat(material_float(&c, SGI_GL_FRONT, SGI_GL_SPECULAR, 0),
                      ==, 0);
    g_assert_cmpfloat(material_float(&c, SGI_GL_BACK, SGI_GL_SPECULAR, 0),
                      ==, 0.5f);
    for (uint32_t face = SGI_GL_FRONT; face <= SGI_GL_BACK; face++) {
        g_assert_cmpfloat(material_float(&c, face, SGI_GL_AMBIENT, 2), ==,
                          0.3f);
        g_assert_cmpfloat(material_float(&c, face, SGI_GL_DIFFUSE, 3), ==,
                          0.4f);
    }
    g_assert_cmpfloat(material_float(&c, SGI_GL_FRONT, SGI_GL_SHININESS, 0),
                      ==, 64);
    g_assert_cmpfloat(material_float(&c, SGI_GL_BACK, SGI_GL_SHININESS, 0),
                      ==, 0);

    word[0] = SGI_GL_LIGHT_MODEL_AMBIENT;
    word[1] = bits(0.5f), word[2] = bits(0.25f);
    word[3] = bits(0.125f), word[4] = bits(1);
    command(&c, NULL, SGI_GL_OP_LIGHT_MODEL, word, 5);
    word[0] = SGI_GL_LIGHT_MODEL_TWO_SIDE, word[1] = bits(1);
    command(&c, NULL, SGI_GL_OP_LIGHT_MODEL, word, 5);
    g_assert_cmpfloat(get_float(&c, SGI_GL_LIGHT_MODEL_AMBIENT, 1), ==, 0.25f);
    g_assert_cmpuint(get(&c, SGI_GL_LIGHT_MODEL_TWO_SIDE, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    g_assert_cmpuint(get(&c, SGI_GL_LIGHT_MODEL_LOCAL_VIEWER, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
}

/*
 * With GL_COLOR_MATERIAL enabled the parameter glColorMaterial names
 * tracks the current colour (OpenGL 1.2.1, 2.13.3): from the enable on,
 * and at each colour command, on the faces it names only. Disabled, a
 * colour command leaves the material alone.
 */
static void test_color_material_tracks_the_colour(void)
{
    uint32_t word[4];
    SGIGLContext c;

    context_init(&c);
    word[0] = bits(0.5f), word[1] = bits(0.25f), word[2] = bits(0.75f);
    command(&c, NULL, SGI_GL_OP_COLOR3F, word, 3);
    g_assert_cmpfloat(material_float(&c, SGI_GL_FRONT, SGI_GL_DIFFUSE, 0),
                      ==, 0.8f);

    word[0] = SGI_GL_FRONT, word[1] = SGI_GL_DIFFUSE;
    command(&c, NULL, SGI_GL_OP_COLOR_MATERIAL, word, 2);
    word[0] = SGI_GL_COLOR_MATERIAL;
    command(&c, NULL, SGI_GL_OP_ENABLE, word, 1);
    g_assert_cmpfloat(material_float(&c, SGI_GL_FRONT, SGI_GL_DIFFUSE, 2),
                      ==, 0.75f);
    g_assert_cmpfloat(material_float(&c, SGI_GL_FRONT, SGI_GL_AMBIENT, 0),
                      ==, 0.2f);
    g_assert_cmpfloat(material_float(&c, SGI_GL_BACK, SGI_GL_DIFFUSE, 0),
                      ==, 0.8f);

    word[0] = 255, word[1] = 0, word[2] = 51, word[3] = 102;
    command(&c, NULL, SGI_GL_OP_COLOR4UB, word, 4);
    g_assert_cmpfloat(material_float(&c, SGI_GL_FRONT, SGI_GL_DIFFUSE, 0),
                      ==, 1);
    g_assert_cmpfloat(material_float(&c, SGI_GL_FRONT, SGI_GL_DIFFUSE, 3),
                      ==, 0.4f);
    word[0] = bits(0.125f), word[1] = bits(0.25f);
    word[2] = bits(0.5f), word[3] = bits(0.625f);
    command(&c, NULL, SGI_GL_OP_COLOR4F, word, 4);
    g_assert_cmpfloat(material_float(&c, SGI_GL_FRONT, SGI_GL_DIFFUSE, 3),
                      ==, 0.625f);
    command(&c, NULL, SGI_GL_OP_COLOR3F, word, 3);
    g_assert_cmpfloat(material_float(&c, SGI_GL_FRONT, SGI_GL_DIFFUSE, 3),
                      ==, 1);

    word[0] = SGI_GL_COLOR_MATERIAL;
    command(&c, NULL, SGI_GL_OP_DISABLE, word, 1);
    word[0] = bits(0), word[1] = bits(0), word[2] = bits(0);
    command(&c, NULL, SGI_GL_OP_COLOR3F, word, 3);
    g_assert_cmpfloat(material_float(&c, SGI_GL_FRONT, SGI_GL_DIFFUSE, 0),
                      ==, 0.125f);
}

/*
 * GL_TEXTURE selects the texture matrix (OpenGL 1.2.1, 2.10.2), initially
 * the identity, with its own stack: matrix commands in that mode change it
 * and leave the modelview and projection alone, glGet answers it
 * column-major, and its stack depth counts the current matrix.
 */
static void test_texture_matrix(void)
{
    uint32_t word[3], out[SGI_GL_GET_WORDS];
    SGIGLContext c;

    context_init(&c);
    g_assert_cmpuint(get(&c, SGI_GL_TEXTURE_MATRIX, out), ==, 16);
    for (int i = 0; i < 16; i++) {
        g_assert_cmphex(out[i], ==, bits(i % 5 == 0));
    }
    /* A saved modelview, which a texture push must not disturb. */
    word[0] = bits(5), word[1] = bits(0), word[2] = bits(0);
    command(&c, NULL, SGI_GL_OP_TRANSLATE, word, 3);
    command(&c, NULL, SGI_GL_OP_PUSH_MATRIX, word, 0);
    command(&c, NULL, SGI_GL_OP_LOAD_IDENTITY, word, 0);
    word[0] = SGI_GL_TEXTURE;
    command(&c, NULL, SGI_GL_OP_MATRIX_MODE, word, 1);
    command(&c, NULL, SGI_GL_OP_PUSH_MATRIX, word, 0);
    word[0] = bits(2), word[1] = bits(3), word[2] = bits(4);
    command(&c, NULL, SGI_GL_OP_TRANSLATE, word, 3);
    g_assert_cmpuint(get(&c, SGI_GL_TEXTURE_STACK_DEPTH, out), ==, 1);
    g_assert_cmphex(out[0], ==, 2);
    g_assert_cmpuint(get(&c, SGI_GL_TEXTURE_MATRIX, out), ==, 16);
    g_assert_cmphex(out[12], ==, bits(2));
    g_assert_cmphex(out[14], ==, bits(4));
    g_assert_cmpuint(get(&c, SGI_GL_MODELVIEW_MATRIX, out), ==, 16);
    g_assert_cmphex(out[12], ==, bits(0));
    g_assert_cmpuint(get(&c, SGI_GL_MODELVIEW_STACK_DEPTH, out), ==, 1);
    g_assert_cmphex(out[0], ==, 2);
    g_assert_cmpuint(get(&c, SGI_GL_PROJECTION_STACK_DEPTH, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);

    command(&c, NULL, SGI_GL_OP_POP_MATRIX, word, 0);
    g_assert_cmpuint(get(&c, SGI_GL_TEXTURE_STACK_DEPTH, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    g_assert_cmpuint(get(&c, SGI_GL_TEXTURE_MATRIX, out), ==, 16);
    g_assert_cmphex(out[12], ==, bits(0));
    word[0] = SGI_GL_MODELVIEW;
    command(&c, NULL, SGI_GL_OP_MATRIX_MODE, word, 1);
    command(&c, NULL, SGI_GL_OP_POP_MATRIX, word, 0);
    g_assert_cmpuint(get(&c, SGI_GL_MODELVIEW_MATRIX, out), ==, 16);
    g_assert_cmphex(out[12], ==, bits(5));
}

/*
 * The depth range maps normalized depth (zd + 1) / 2 to window depth
 * n + (f - n) z (OpenGL 1.2.1, 2.10.1), for a draw and for glRasterPos.
 * The view volume still clips on normalized depth, so a point beyond the
 * far plane is dropped even where the mapped depth would lie in [0, 1];
 * far may be below near.
 */
static void test_depth_range_maps_window_depth(void)
{
    static const struct {
        float zd, n, f;
        uint32_t depth;         /* 0 for a dropped point */
    } cases[] = {
        { 0, 0.25f, 0.75f, 0x800000 },
        { -1, 0.25f, 0.75f, 0x400000 },
        { 1, 0.25f, 0.75f, 0xbfffff },
        { 1.5f, 0.25f, 0.75f, 0 },
        { -1, 1, 0, 0xffffff },
        { 1, 0.5f, 0.5f, 0x800000 },
    };

    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        static Planes p;
        SGIRasterTarget t = planes_target(&p);
        SGIGLContext c;
        float v[SGI_GL_VERTEX_FLOATS] = { 0, 0, cases[i].zd, 1, 1, 1, 1, 1 };
        uint32_t word[2] = { bits(cases[i].n), bits(cases[i].f) };
        SGIGLDraw draw = {
            .mode = SGI_GL_POINTS,
            .count = 1,
            .vertices = v,
            .state = &c.cur,
        };

        memset(&p, 0, sizeof(p));
        context_init(&c);
        command(&c, NULL, SGI_GL_OP_DEPTH_RANGE, word, 2);
        c.cur.depth_test = 1;
        c.cur.depth_func = SGI_GL_ALWAYS;
        sgi_soft_draw(&rules_supported, &t, &draw);
        g_assert_cmphex(p.depth[planes_index(W / 2, H / 2)], ==,
                        cases[i].depth);
    }
}

/* glRasterPos's window z goes through the depth range too (2.12). */
static void test_depth_range_maps_the_raster_position(void)
{
    uint32_t word[3];
    SGIGLContext c;

    context_init(&c);
    word[0] = bits(0.25f), word[1] = bits(0.75f);
    command(&c, NULL, SGI_GL_OP_DEPTH_RANGE, word, 2);
    word[0] = bits(0), word[1] = bits(0), word[2] = bits(-1);
    command(&c, NULL, SGI_GL_OP_RASTER_POS3F, word, 3);
    g_assert_cmpfloat(get_float(&c, SGI_GL_CURRENT_RASTER_POSITION, 2), ==,
                      0.25f);
}

/*
 * Per-vertex lighting (OpenGL 1.2.1, 2.13.1). Each case lights one point
 * or triangle and compares the vertex's lit colour with the equation's
 * value worked by hand. The model evaluates the equation in single
 * precision, so the comparisons allow 2^-16.
 */
#define LIT_EPSILON (1.0 / 65536)

/* The last draw's vertices, as the context sent them. */
typedef struct Capture {
    float v[4][SGI_GL_VERTEX_FLOATS];
    unsigned count;
    bool back_colors;
    unsigned draws;
} Capture;

static void capture_draw(void *opaque, const SGIGLDraw *d)
{
    Capture *cap = opaque;

    g_assert_cmpuint(d->count, <=, 4);
    memcpy(cap->v, d->vertices, d->count * sizeof(cap->v[0]));
    cap->count = d->count;
    cap->back_colors = d->back_colors;
    cap->draws++;
}

static SGIGLSink capture_sink(Capture *cap)
{
    return (SGIGLSink) { .draw = capture_draw, .opaque = cap };
}

static void enable(SGIGLContext *c, uint32_t cap, bool on)
{
    command(c, NULL, on ? SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, &cap, 1);
}

static void light_model(SGIGLContext *c, uint32_t pname, float v0, float v1,
                        float v2, float v3)
{
    uint32_t word[5] = { pname, bits(v0), bits(v1), bits(v2), bits(v3) };

    command(c, NULL, SGI_GL_OP_LIGHT_MODEL, word, 5);
}

static void three(SGIGLContext *c, SGIGLOp op, float x, float y, float z)
{
    uint32_t word[3] = { bits(x), bits(y), bits(z) };

    command(c, NULL, op, word, 3);
}

/*
 * A context with lighting and light 0 enabled and every lighting colour
 * zero: no light model ambient, black lights 0 and 1, and a black
 * material on both faces with diffuse alpha 1. A case turns on only the
 * terms it tests.
 */
static void lit_init(SGIGLContext *c)
{
    static const uint32_t pnames[] = {
        SGI_GL_AMBIENT, SGI_GL_DIFFUSE, SGI_GL_SPECULAR,
    };

    context_init(c);
    enable(c, SGI_GL_LIGHTING, true);
    enable(c, SGI_GL_LIGHT0, true);
    light_model(c, SGI_GL_LIGHT_MODEL_AMBIENT, 0, 0, 0, 1);
    for (int i = 0; i < ARRAY_SIZE(pnames); i++) {
        light_command(c, SGI_GL_OP_LIGHT, 0, pnames[i], 0, 0, 0, 1);
        light_command(c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT_AND_BACK,
                      pnames[i], 0, 0, 0, 1);
    }
}

/* The vertex of one point drawn at (x, y, z) with normal n. */
static const float *lit_vertex(SGIGLContext *c, float x, float y, float z,
                               float nx, float ny, float nz)
{
    static Capture cap;
    SGIGLSink sink = capture_sink(&cap);
    uint32_t word[3] = { bits(nx), bits(ny), bits(nz) };

    memset(&cap, 0, sizeof(cap));
    command(c, &sink, SGI_GL_OP_NORMAL3F, word, 3);
    word[0] = SGI_GL_POINTS;
    command(c, &sink, SGI_GL_OP_BEGIN, word, 1);
    word[0] = bits(x), word[1] = bits(y), word[2] = bits(z);
    command(c, &sink, SGI_GL_OP_VERTEX3F, word, 3);
    command(c, &sink, SGI_GL_OP_END, word, 0);
    g_assert_cmpuint(cap.draws, ==, 1);
    return cap.v[0];
}

/* Its colour. */
static const float *lit_point(SGIGLContext *c, float x, float y, float z,
                              float nx, float ny, float nz)
{
    return &lit_vertex(c, x, y, z, nx, ny, nz)[SGI_GL_V_COLOR];
}

static void assert_rgba(const float *got, float r, float g, float b,
                        float a)
{
    g_assert_cmpfloat_with_epsilon(got[0], r, LIT_EPSILON);
    g_assert_cmpfloat_with_epsilon(got[1], g, LIT_EPSILON);
    g_assert_cmpfloat_with_epsilon(got[2], b, LIT_EPSILON);
    g_assert_cmpfloat_with_epsilon(got[3], a, LIT_EPSILON);
}

/*
 * The ambient terms alone: material ambient (0.4, 0.2, 0.1) times the
 * light model's (0.25, 0.5, 1) is (0.1, 0.1, 0.1), and times light 0's
 * (0.5, 0.5, 0.5) adds (0.2, 0.1, 0.05). Alpha is the material's diffuse
 * alpha. Emission adds as it is. Disabling light 0 drops its term, and
 * enabling light 1 adds its own.
 */
static void test_lighting_ambient_and_emission(void)
{
    SGIGLContext c;

    lit_init(&c);
    light_model(&c, SGI_GL_LIGHT_MODEL_AMBIENT, 0.25f, 0.5f, 1, 1);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_AMBIENT, 0.5f, 0.5f, 0.5f,
                  1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_AMBIENT,
                  0.4f, 0.2f, 0.1f, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_DIFFUSE,
                  0, 0, 0, 0.75f);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0.3f, 0.2f, 0.15f, 0.75f);

    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_EMISSION,
                  0.25f, 0, 0, 1);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0.55f, 0.2f, 0.15f, 0.75f);

    enable(&c, SGI_GL_LIGHT0, false);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0.35f, 0.1f, 0.1f, 0.75f);

    light_command(&c, SGI_GL_OP_LIGHT, 1, SGI_GL_AMBIENT, 1, 1, 1, 1);
    enable(&c, SGI_GL_LIGHT0 + 1, true);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0.75f, 0.3f, 0.2f, 0.75f);
}

/*
 * The diffuse term: a light at infinity toward (0, 3, 4), made unit
 * (0, 0.6, 0.8), on normal (0, 0, 1) gives n . VP = 0.8, times light
 * diffuse (1, 0.5, 1) and material diffuse (0.5, 1, 0.25): (0.4, 0.4,
 * 0.2). A normal facing away gives 0, not a negative colour.
 */
static void test_lighting_diffuse(void)
{
    SGIGLContext c;

    lit_init(&c);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_POSITION, 0, 3, 4, 0);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_DIFFUSE, 1, 0.5f, 1, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_DIFFUSE,
                  0.5f, 1, 0.25f, 1);
    assert_rgba(lit_point(&c, 7, -2, 5, 0, 0, 1), 0.4f, 0.4f, 0.2f, 1);
    assert_rgba(lit_point(&c, 7, -2, 5, 0, 0, -1), 0, 0, 0, 1);
}

/*
 * The specular term with a viewer at infinity: h = (0, 0.6, 0.8) +
 * (0, 0, 1) = (0, 0.6, 1.8), and n . h made unit is 1.8 / sqrt(3.6), whose
 * square is 0.9, so to shininess 10 it is 0.9^5 = 0.59049; times material
 * specular (1, 0.5, 0.25). With the light behind the surface n . VP is
 * negative and f is 0, though n . h, 0.316 at shininess 1, is not. With a
 * local viewer and the
 * vertex at (3, 0, -4), the eye lies along (-0.6, 0, 0.8) and a light
 * along (0.6, 0, 0.8) makes h (0, 0, 1.6), so n . h is 1; at infinity the
 * same light gives 0.59049 again.
 */
static void test_lighting_specular(void)
{
    SGIGLContext c;

    lit_init(&c);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_POSITION, 0, 3, 4, 0);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_SPECULAR, 1, 1, 1, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_SPECULAR,
                  1, 0.5f, 0.25f, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_SHININESS,
                  10, 0, 0, 0);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0.59049f, 0.295245f,
                0.1476225f, 1);

    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_POSITION, 0, 3, -4, 0);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_SHININESS,
                  1, 0, 0, 0);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0, 0, 0, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_SHININESS,
                  10, 0, 0, 0);

    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_POSITION, 3, 0, 4, 0);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_SPECULAR,
                  0.5f, 0.5f, 0.5f, 1);
    assert_rgba(lit_point(&c, 3, 0, -4, 0, 0, 1), 0.295245f, 0.295245f,
                0.295245f, 1);
    light_model(&c, SGI_GL_LIGHT_MODEL_LOCAL_VIEWER, 1, 0, 0, 0);
    assert_rgba(lit_point(&c, 3, 0, -4, 0, 0, 1), 0.5f, 0.5f, 0.5f, 1);
}

/*
 * Attenuation 1 / (k0 + k1 d + k2 d^2) with k = (0.5, 0.25, 0.125) for a
 * light 2 from the vertex is 1 / 1.5, whether the position is (0, 0, 2, 1)
 * or (0, 0, 4, 2); it scales the light's ambient term too. A light at
 * infinity is not attenuated.
 */
static void test_lighting_attenuation(void)
{
    SGIGLContext c;

    lit_init(&c);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_DIFFUSE, 1, 1, 1, 1);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_AMBIENT, 0.5f, 0, 0, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT,
                  SGI_GL_AMBIENT_AND_DIFFUSE, 0.5f, 0.5f, 0.5f, 1);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_CONSTANT_ATTENUATION,
                  0.5f, 0, 0, 0);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_LINEAR_ATTENUATION,
                  0.25f, 0, 0, 0);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_QUADRATIC_ATTENUATION,
                  0.125f, 0, 0, 0);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_POSITION, 0, 0, 2, 1);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0.5f, 1 / 3.0f, 1 / 3.0f,
                1);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_POSITION, 0, 0, 4, 2);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0.5f, 1 / 3.0f, 1 / 3.0f,
                1);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_POSITION, 0, 0, 1, 0);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0.75f, 0.5f, 0.5f, 1);
}

/*
 * A spotlight at (0, 0, 2) pointing down -z with cutoff 30 degrees and
 * exponent 2. Straight below it the spot factor is 1. At (1, 0, 0) the
 * light-to-vertex direction (1, 0, -2) / sqrt(5) makes 26.6 degrees with
 * the axis, so the factor is (2 / sqrt(5))^2 = 0.8, and n . VP is
 * 2 / sqrt(5): 0.8 * 0.894427 = 0.715542. At (2, 0, 0), 45 degrees off,
 * it is outside the cone and unlit.
 */
static void test_lighting_spot(void)
{
    SGIGLContext c;

    lit_init(&c);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_DIFFUSE, 1, 1, 1, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_DIFFUSE,
                  1, 1, 1, 1);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_POSITION, 0, 0, 2, 1);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_SPOT_DIRECTION, 0, 0, -3,
                  0);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_SPOT_CUTOFF, 30, 0, 0, 0);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_SPOT_EXPONENT, 2, 0, 0, 0);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 1, 1, 1, 1);
    assert_rgba(lit_point(&c, 1, 0, 0, 0, 0, 1), 0.7155418f, 0.7155418f,
                0.7155418f, 1);
    assert_rgba(lit_point(&c, 2, 0, 0, 0, 0, 1), 0, 0, 0, 1);
}

/*
 * The normal goes to eye coordinates by the inverse transpose of the
 * modelview's upper left 3x3 (2.10.3). Under glScalef(2, 1, 1) the normal
 * (1, 1, 0) becomes (0.5, 1, 0), which a light along +x set before the
 * scale meets at 0.5, halved by material diffuse 0.5: 0.25. With
 * GL_NORMALIZE it is (1, 2, 0) / sqrt(5), giving 0.5 / sqrt(5). Under
 * glScalef(2, 2, 2) the normal (1, 0, 0) shrinks to 0.5; GL_RESCALE_NORMAL
 * scales it back to 1. Under glRotatef(90, 0, 0, 1) then glScalef(2, 1, 1)
 * the upper left 3x3 is not symmetric: (1, 1, 0) becomes (-1, 0.5, 0),
 * which a light along +y meets at 0.5, where the matrix itself would give
 * 2 and its inverse without the transpose -1.
 */
static void test_lighting_normal_transform(void)
{
    SGIGLContext c;

    lit_init(&c);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_POSITION, 1, 0, 0, 0);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_DIFFUSE, 1, 1, 1, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_DIFFUSE,
                  0.5f, 0.5f, 0.5f, 1);
    three(&c, SGI_GL_OP_SCALE, 2, 1, 1);
    assert_rgba(lit_point(&c, 0, 0, 0, 1, 1, 0), 0.25f, 0.25f, 0.25f, 1);
    enable(&c, SGI_GL_NORMALIZE, true);
    assert_rgba(lit_point(&c, 0, 0, 0, 1, 1, 0), 0.2236068f, 0.2236068f,
                0.2236068f, 1);
    enable(&c, SGI_GL_NORMALIZE, false);

    command(&c, NULL, SGI_GL_OP_LOAD_IDENTITY, NULL, 0);
    three(&c, SGI_GL_OP_SCALE, 2, 2, 2);
    assert_rgba(lit_point(&c, 0, 0, 0, 1, 0, 0), 0.25f, 0.25f, 0.25f, 1);
    enable(&c, SGI_GL_RESCALE_NORMAL, true);
    assert_rgba(lit_point(&c, 0, 0, 0, 1, 0, 0), 0.5f, 0.5f, 0.5f, 1);
    enable(&c, SGI_GL_RESCALE_NORMAL, false);

    command(&c, NULL, SGI_GL_OP_LOAD_IDENTITY, NULL, 0);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_POSITION, 0, 1, 0, 0);
    command(&c, NULL, SGI_GL_OP_ROTATE,
            (uint32_t[]) { bits(90), bits(0), bits(0), bits(1) }, 4);
    three(&c, SGI_GL_OP_SCALE, 2, 1, 1);
    assert_rgba(lit_point(&c, 0, 0, 0, 1, 1, 0), 0.25f, 0.25f, 0.25f, 1);
}

/*
 * Two-sided lighting lights the back colour with the back material and
 * the reversed normal. A normal pointing away from a light along +z
 * leaves the front colour black and gives the back the back material's
 * diffuse and alpha. One-sided, the back colour is the front one and the
 * draw says it carries none.
 */
static void test_lighting_two_sided(void)
{
    static Capture cap;
    SGIGLSink sink = capture_sink(&cap);
    SGIGLContext c;
    const float *v;

    lit_init(&c);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_DIFFUSE, 1, 1, 1, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_DIFFUSE,
                  1, 0, 0, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_BACK, SGI_GL_DIFFUSE,
                  0, 1, 0, 0.5f);
    v = lit_vertex(&c, 0, 0, 0, 0, 0, -1);
    assert_rgba(&v[SGI_GL_V_COLOR], 0, 0, 0, 1);
    assert_rgba(&v[SGI_GL_V_BACK_COLOR], 0, 0, 0, 1);
    light_model(&c, SGI_GL_LIGHT_MODEL_TWO_SIDE, 1, 0, 0, 0);
    v = lit_vertex(&c, 0, 0, 0, 0, 0, -1);
    assert_rgba(&v[SGI_GL_V_COLOR], 0, 0, 0, 1);
    assert_rgba(&v[SGI_GL_V_BACK_COLOR], 0, 1, 0, 0.5f);

    /* The draw's flag follows two-sided lighting. */
    memset(&cap, 0, sizeof(cap));
    command(&c, &sink, SGI_GL_OP_BEGIN, (uint32_t[]) { SGI_GL_POINTS }, 1);
    command(&c, &sink, SGI_GL_OP_VERTEX2F, (uint32_t[]) { 0, 0 }, 2);
    g_assert_true(cap.back_colors);
    light_model(&c, SGI_GL_LIGHT_MODEL_TWO_SIDE, 0, 0, 0, 0);
    command(&c, &sink, SGI_GL_OP_VERTEX2F, (uint32_t[]) { 0, 0 }, 2);
    g_assert_false(cap.back_colors);
    command(&c, &sink, SGI_GL_OP_END, NULL, 0);
}

/*
 * With GL_COLOR_MATERIAL on the front diffuse, glColor3f(0.25, 0.5, 1)
 * is the diffuse material the light at (0, 0.6, 0.8) meets at 0.8.
 */
static void test_lighting_color_material(void)
{
    uint32_t word[2] = { SGI_GL_FRONT, SGI_GL_DIFFUSE };
    SGIGLContext c;

    lit_init(&c);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_POSITION, 0, 3, 4, 0);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_DIFFUSE, 1, 1, 1, 1);
    command(&c, NULL, SGI_GL_OP_COLOR_MATERIAL, word, 2);
    enable(&c, SGI_GL_COLOR_MATERIAL, true);
    three(&c, SGI_GL_OP_COLOR3F, 0.25f, 0.5f, 1);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0.2f, 0.4f, 0.8f, 1);
}

/*
 * The lit colour is clamped to [0, 1] (2.13.6), alpha too. Unlit, the
 * vertex keeps the current colour and its back colour is the same.
 */
static void test_lighting_clamps_and_disables(void)
{
    uint32_t out[SGI_GL_GET_WORDS];
    SGIGLContext c;
    const float *v;

    lit_init(&c);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_EMISSION,
                  2, -1, 0.5f, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_DIFFUSE,
                  0, 0, 0, 1.5f);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 1, 0, 0.5f, 1);

    g_assert_cmpuint(get(&c, SGI_GL_LIGHTING, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    g_assert_cmpuint(get(&c, SGI_GL_LIGHT0, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    g_assert_cmpuint(get(&c, SGI_GL_LIGHT0 + 1, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    enable(&c, SGI_GL_LIGHTING, false);
    three(&c, SGI_GL_OP_COLOR3F, 0.25f, 0.5f, 0.75f);
    v = lit_vertex(&c, 0, 0, 0, 0, 0, 1);
    assert_rgba(&v[SGI_GL_V_COLOR], 0.25f, 0.5f, 0.75f, 1);
    assert_rgba(&v[SGI_GL_V_BACK_COLOR], 0.25f, 0.5f, 0.75f, 1);
    g_assert_cmpuint(get(&c, SGI_GL_LIGHTING, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
}

/* With lighting enabled glRasterPos takes the lit front colour (2.12). */
static void test_lighting_raster_position(void)
{
    SGIGLContext c;

    lit_init(&c);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_POSITION, 0, 3, 4, 0);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_DIFFUSE, 1, 1, 1, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_DIFFUSE,
                  0.5f, 0.25f, 1, 1);
    three(&c, SGI_GL_OP_COLOR3F, 1, 1, 1);
    three(&c, SGI_GL_OP_RASTER_POS3F, 0, 0, 0);
    g_assert_cmpfloat_with_epsilon(get_float(&c, SGI_GL_CURRENT_RASTER_COLOR,
                                             0), 0.4f, LIT_EPSILON);
    g_assert_cmpfloat_with_epsilon(get_float(&c, SGI_GL_CURRENT_RASTER_COLOR,
                                             2), 0.8f, LIT_EPSILON);
}

/*
 * Drawn, a back-facing triangle (clockwise in window coordinates) takes
 * its back colour under two-sided lighting and the front colour
 * otherwise; a front-facing one always takes the front colour. Every
 * triangle of a strip has the strip's facing, the odd ones included, and
 * a strip drawn as one batch does the same.
 */
static void test_lighting_two_sided_draws(void)
{
    static Planes p;
    SGIGLContext c;
    Recorder rec = { .rules = &rules_supported };
    SGIGLSink sink;
    /* Clockwise, then counter-clockwise, then a strip of two. */
    static const float cw[3][2] = { { -1, 0 }, { -1, 1 }, { 0, 0 } };
    static const float ccw[3][2] = { { 0, -1 }, { 1, -1 }, { 1, 0 } };
    static const float strip[4][2] = {
        { -1, -1 }, { -0.5f, -1 }, { -1, -0.5f }, { -0.5f, -0.5f },
    };
    uint32_t word[3];

    rec.target = planes_target(&p);
    sink = recorder_sink(&rec);
    for (int two = 0; two < 2; two++) {
        memset(&p, 0, sizeof(p));
        lit_init(&c);
        light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_DIFFUSE, 1, 1, 1, 1);
        light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_DIFFUSE,
                      1, 0, 0, 1);
        light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_BACK, SGI_GL_EMISSION,
                      0, 1, 0, 1);
        light_model(&c, SGI_GL_LIGHT_MODEL_TWO_SIDE, two, 0, 0, 0);
        three(&c, SGI_GL_OP_NORMAL3F, 0, 0, 1);
        word[0] = SGI_GL_TRIANGLES;
        command(&c, &sink, SGI_GL_OP_BEGIN, word, 1);
        for (int i = 0; i < 3; i++) {
            word[0] = bits(cw[i][0]), word[1] = bits(cw[i][1]);
            command(&c, &sink, SGI_GL_OP_VERTEX2F, word, 2);
        }
        for (int i = 0; i < 3; i++) {
            word[0] = bits(ccw[i][0]), word[1] = bits(ccw[i][1]);
            command(&c, &sink, SGI_GL_OP_VERTEX2F, word, 2);
        }
        command(&c, &sink, SGI_GL_OP_END, word, 0);
        word[0] = SGI_GL_TRIANGLE_STRIP;
        command(&c, &sink, SGI_GL_OP_BEGIN, word, 1);
        for (int i = 0; i < 4; i++) {
            word[0] = bits(strip[i][0]), word[1] = bits(strip[i][1]);
            command(&c, &sink, SGI_GL_OP_VERTEX2F, word, 2);
        }
        command(&c, &sink, SGI_GL_OP_END, word, 0);

        /* Lit front red; the back material's emission is green. */
        g_assert_cmphex(p.colour[0][planes_index(20, 36)], ==,
                        two ? 0x00ff00 : 0xff0000);
        g_assert_cmphex(p.colour[0][planes_index(56, 20)], ==, 0xff0000);
        /* Strip pieces 0 and 1, at (-0.9, -0.9) and (-0.6, -0.6). */
        g_assert_cmphex(p.colour[0][planes_index(3, 3)], ==, 0xff0000);
        g_assert_cmphex(p.colour[0][planes_index(12, 12)], ==, 0xff0000);
    }

    /* The batch strip, with back colours marked green. */
    {
        float verts[4][SGI_GL_VERTEX_FLOATS] = { 0 };
        SGIRasterTarget t = planes_target(&p);
        SGIGLDraw d = {
            .mode = SGI_GL_TRIANGLE_STRIP,
            .count = 4,
            .vertices = &verts[0][0],
            .back_colors = true,
        };

        memset(&p, 0, sizeof(p));
        context_init(&c);
        d.state = &c.cur;
        for (int i = 0; i < 4; i++) {
            verts[i][SGI_GL_V_POSITION] = strip[i][0];
            verts[i][SGI_GL_V_POSITION + 1] = strip[i][1];
            verts[i][SGI_GL_V_POSITION + 3] = 1;
            verts[i][SGI_GL_V_COLOR] = 1;
            verts[i][SGI_GL_V_BACK_COLOR + 1] = 1;
        }
        sgi_soft_draw(&rules_supported, &t, &d);
        g_assert_cmphex(p.colour[0][planes_index(3, 3)], ==, 0xff0000);
        g_assert_cmphex(p.colour[0][planes_index(12, 12)], ==, 0xff0000);
    }
}

/*
 * Rescaling multiplies the transformed normal by 1 / |row 3 of M^-1|
 * (2.10.3, m31, m32 and m33 of M^-1), M the modelview. With M's third
 * row (0, 1, 2), M^-1's third row is (0, -0.5, 0.5) and its third column
 * (0, 0, 0.5). The normal (0, 0, 1) becomes (0, -0.5, 0.5), rescaled to
 * (0, -0.707107, 0.707107), which a light along +z meets at 0.707107;
 * scaled by the column instead it would be (0, -1, 1), which clamps to 1.
 */
static void test_lighting_rescale_normal_uses_a_row(void)
{
    uint32_t m[16] = { 0 };
    SGIGLContext c;

    lit_init(&c);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_DIFFUSE, 1, 1, 1, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_DIFFUSE,
                  1, 1, 1, 1);
    m[0] = m[5] = m[15] = bits(1);
    m[6] = bits(1);
    m[10] = bits(2);
    command(&c, NULL, SGI_GL_OP_LOAD_MATRIX, m, 16);
    enable(&c, SGI_GL_RESCALE_NORMAL, true);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0.7071068f, 0.7071068f,
                0.7071068f, 1);
}

/*
 * With both the light and the vertex at infinity, the vector from the
 * vertex to the light is P2 - P1 made unit length (2.13.1). The light
 * (0, 0, 1, 0) and the vertex (1, 0, 1, 0) give (-1, 0, 0), which the
 * normal (-1, 0, 0) meets at 1; the light's own direction would meet it
 * at 0. The specification leaves the result of lighting a vertex whose
 * eye w is 0 undefined, but not the vector.
 */
static void test_lighting_both_at_infinity(void)
{
    static Capture cap;
    SGIGLSink sink = capture_sink(&cap);
    SGIGLContext c;
    uint32_t word[4];

    lit_init(&c);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_DIFFUSE, 1, 1, 1, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_DIFFUSE,
                  0.5f, 0.5f, 0.5f, 1);
    three(&c, SGI_GL_OP_NORMAL3F, -1, 0, 0);
    memset(&cap, 0, sizeof(cap));
    word[0] = SGI_GL_POINTS;
    command(&c, &sink, SGI_GL_OP_BEGIN, word, 1);
    word[0] = bits(1), word[1] = bits(0), word[2] = bits(1), word[3] = 0;
    command(&c, &sink, SGI_GL_OP_VERTEX4F, word, 4);
    command(&c, &sink, SGI_GL_OP_END, word, 0);
    g_assert_cmpuint(cap.draws, ==, 1);
    assert_rgba(&cap.v[0][SGI_GL_V_COLOR], 0.5f, 0.5f, 0.5f, 1);
}

/* A clockwise and a counter-clockwise triangle, as two-sided-draws has. */
static const float cull_cw[3][2] = { { -1, 0 }, { -1, 1 }, { 0, 0 } };
static const float cull_ccw[3][2] = { { 0, -1 }, { 1, -1 }, { 1, 0 } };
/* A counter-clockwise strip of two triangles in the lower-left corner. */
static const float cull_strip[4][2] = {
    { -1, -1 }, { -0.5f, -1 }, { -1, -0.5f }, { -0.5f, -0.5f },
};

static void cull_vertices(SGIGLContext *c, const SGIGLSink *sink,
                          uint32_t mode, const float (*v)[2], unsigned n)
{
    uint32_t word[2] = { mode };

    command(c, sink, SGI_GL_OP_BEGIN, word, 1);
    for (unsigned i = 0; i < n; i++) {
        word[0] = bits(v[i][0]), word[1] = bits(v[i][1]);
        command(c, sink, SGI_GL_OP_VERTEX2F, word, 2);
    }
    command(c, sink, SGI_GL_OP_END, word, 0);
}

/*
 * Into cleared planes: both triangles, the strip, a point at pixel
 * (48, 48) and a line along row 60.
 */
static void cull_scene(SGIGLContext *c, const SGIGLSink *sink, Planes *p)
{
    static const float point[1][2] = { { 0.51f, 0.51f } };
    static const float line[2][2] = { { 0.2f, 0.9f }, { 0.8f, 0.9f } };

    memset(p, 0, sizeof(*p));
    cull_vertices(c, sink, SGI_GL_TRIANGLES, cull_cw, 3);
    cull_vertices(c, sink, SGI_GL_TRIANGLES, cull_ccw, 3);
    cull_vertices(c, sink, SGI_GL_TRIANGLE_STRIP, cull_strip, 4);
    cull_vertices(c, sink, SGI_GL_POINTS, point, 1);
    cull_vertices(c, sink, SGI_GL_LINES, line, 2);
}

/*
 * glCullFace, glFrontFace and GL_CULL_FACE (OpenGL 1.2.1, 3.5.1 and
 * 2.13.1): culling starts disabled, on the back face, counter-clockwise
 * front, and glGet answers each; a bad mode or direction changes nothing.
 */
static void test_cull_face_state(void)
{
    uint32_t word[1], out[SGI_GL_GET_WORDS];
    SGIGLContext c;

    context_init(&c);
    g_assert_cmpuint(get(&c, SGI_GL_CULL_FACE, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmpuint(get(&c, SGI_GL_CULL_FACE_MODE, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_BACK);
    g_assert_cmpuint(get(&c, SGI_GL_FRONT_FACE, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_CCW);

    enable(&c, SGI_GL_CULL_FACE, true);
    word[0] = SGI_GL_FRONT_AND_BACK;
    command(&c, NULL, SGI_GL_OP_CULL_FACE, word, 1);
    word[0] = SGI_GL_CW;
    command(&c, NULL, SGI_GL_OP_FRONT_FACE, word, 1);
    g_assert_cmpuint(get(&c, SGI_GL_CULL_FACE, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    g_assert_cmpuint(get(&c, SGI_GL_CULL_FACE_MODE, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_FRONT_AND_BACK);
    g_assert_cmpuint(get(&c, SGI_GL_FRONT_FACE, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_CW);

    /* GL_LEFT is no face to cull, and 0x902 no direction. */
    word[0] = 0x406;
    command(&c, NULL, SGI_GL_OP_CULL_FACE, word, 1);
    word[0] = 0x902;
    command(&c, NULL, SGI_GL_OP_FRONT_FACE, word, 1);
    g_assert_cmpuint(get(&c, SGI_GL_CULL_FACE_MODE, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_FRONT_AND_BACK);
    g_assert_cmpuint(get(&c, SGI_GL_FRONT_FACE, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_CW);

    enable(&c, SGI_GL_CULL_FACE, false);
    g_assert_cmpuint(get(&c, SGI_GL_CULL_FACE, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
}

/*
 * Which polygons are rasterized (3.5.1): with culling disabled, all of
 * them; enabled, the front-facing ones unless the mode is GL_FRONT or
 * GL_FRONT_AND_BACK, and the back-facing ones unless it is GL_BACK or
 * GL_FRONT_AND_BACK. A positive window area is front facing under GL_CCW
 * and a negative one under GL_CW. Every triangle of a strip keeps the
 * strip's facing. Points and lines are never culled. The last case
 * turns culling off again under GL_FRONT_AND_BACK.
 */
static void test_cull_face_draws(void)
{
    static const struct {
        bool enable;
        uint32_t mode;
        uint32_t front;
        bool cw_drawn;
        bool ccw_drawn;
    } runs[] = {
        { false, SGI_GL_BACK, SGI_GL_CCW, true, true },
        { true, SGI_GL_BACK, SGI_GL_CCW, false, true },
        { true, SGI_GL_FRONT, SGI_GL_CCW, true, false },
        { true, SGI_GL_FRONT_AND_BACK, SGI_GL_CCW, false, false },
        { true, SGI_GL_BACK, SGI_GL_CW, true, false },
        { true, SGI_GL_FRONT, SGI_GL_CW, false, true },
        { false, SGI_GL_FRONT_AND_BACK, SGI_GL_CW, true, true },
    };
    static Planes p;
    SGIGLContext c;
    Recorder rec = { .rules = &rules_supported };
    SGIGLSink sink;
    uint32_t word[1];

    rec.target = planes_target(&p);
    sink = recorder_sink(&rec);
    for (int i = 0; i < ARRAY_SIZE(runs); i++) {
        context_init(&c);
        enable(&c, SGI_GL_CULL_FACE, true);
        enable(&c, SGI_GL_CULL_FACE, runs[i].enable);
        word[0] = runs[i].mode;
        command(&c, NULL, SGI_GL_OP_CULL_FACE, word, 1);
        word[0] = runs[i].front;
        command(&c, NULL, SGI_GL_OP_FRONT_FACE, word, 1);
        cull_scene(&c, &sink, &p);

        g_assert_cmphex(p.colour[0][planes_index(20, 36)], ==,
                        runs[i].cw_drawn ? 0xffffff : 0);
        g_assert_cmphex(p.colour[0][planes_index(56, 20)], ==,
                        runs[i].ccw_drawn ? 0xffffff : 0);
        /* Strip pieces 0 and 1, the odd one at (12, 12). */
        g_assert_cmphex(p.colour[0][planes_index(3, 3)], ==,
                        runs[i].ccw_drawn ? 0xffffff : 0);
        g_assert_cmphex(p.colour[0][planes_index(12, 12)], ==,
                        runs[i].ccw_drawn ? 0xffffff : 0);
        g_assert_cmphex(p.colour[0][planes_index(48, 48)], ==, 0xffffff);
        g_assert_cmphex(p.colour[0][planes_index(48, 60)], ==, 0xffffff);
    }
}

/*
 * Two-sided lighting's colour choice follows glFrontFace (2.13.1): under
 * GL_CW the clockwise triangle is the front and takes the lit front
 * colour, red, and the counter-clockwise triangle and strip are the back
 * and take the back material's green emission.
 */
static void test_lighting_two_sided_front_face(void)
{
    static Planes p;
    SGIGLContext c;
    Recorder rec = { .rules = &rules_supported };
    SGIGLSink sink;
    uint32_t word[1] = { SGI_GL_CW };

    rec.target = planes_target(&p);
    sink = recorder_sink(&rec);
    lit_init(&c);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_DIFFUSE, 1, 1, 1, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_DIFFUSE,
                  1, 0, 0, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_BACK, SGI_GL_EMISSION,
                  0, 1, 0, 1);
    light_model(&c, SGI_GL_LIGHT_MODEL_TWO_SIDE, 1, 0, 0, 0);
    command(&c, NULL, SGI_GL_OP_FRONT_FACE, word, 1);
    three(&c, SGI_GL_OP_NORMAL3F, 0, 0, 1);
    cull_scene(&c, &sink, &p);

    g_assert_cmphex(p.colour[0][planes_index(20, 36)], ==, 0xff0000);
    g_assert_cmphex(p.colour[0][planes_index(56, 20)], ==, 0x00ff00);
    g_assert_cmphex(p.colour[0][planes_index(3, 3)], ==, 0x00ff00);
    g_assert_cmphex(p.colour[0][planes_index(12, 12)], ==, 0x00ff00);
}

/*
 * A light as a family decodes it: white diffuse along +z, no spot, no
 * attenuation, black ambient and specular.
 */
static SGIGLEyeLight eye_light(float x, float y, float z, float w)
{
    return (SGIGLEyeLight) {
        .position = { x, y, z, w },
        .spot_direction = { 0, 0, -1 },
        .spot_cos_cutoff = -1,
        .attenuation = { 1, 0, 0 },
        .ambient = { 0, 0, 0, 1 },
        .diffuse = { 1, 1, 1, 1 },
        .specular = { 0, 0, 0, 1 },
    };
}

/*
 * sgi_gl_set_light() takes eye coordinates as given: set under
 * glRotatef(90, 1, 0, 0), which would take (0, 0, 1) to (0, -1, 0), the
 * light at infinity along +z still meets the normal (0, 0, 1) at 1 once
 * the modelview is the identity again, and diffuse 0.5 times material
 * diffuse 1 gives 0.5. glGetLightfv answers what was given, and a cutoff
 * cosine of -1 as 180 degrees. Light 8 does not exist.
 */
static void test_set_light_is_eye_space(void)
{
    SGIGLEyeLight e = eye_light(0, 0, 1, 0);
    SGIGLContext c;

    lit_init(&c);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_DIFFUSE,
                  1, 1, 1, 1);
    command(&c, NULL, SGI_GL_OP_ROTATE,
            (uint32_t[]) { bits(90), bits(1), bits(0), bits(0) }, 4);
    e.diffuse[0] = e.diffuse[1] = e.diffuse[2] = 0.5f;
    g_assert_true(sgi_gl_set_light(&c, 0, &e));
    command(&c, NULL, SGI_GL_OP_LOAD_IDENTITY, NULL, 0);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0.5f, 0.5f, 0.5f, 1);

    g_assert_cmpfloat(light_float(&c, 0, SGI_GL_POSITION, 1), ==, 0);
    g_assert_cmpfloat(light_float(&c, 0, SGI_GL_POSITION, 2), ==, 1);
    g_assert_cmpfloat(light_float(&c, 0, SGI_GL_DIFFUSE, 0), ==, 0.5f);
    g_assert_cmpfloat(light_float(&c, 0, SGI_GL_SPOT_CUTOFF, 0), ==, 180);
    g_assert_false(sgi_gl_set_light(&c, SGI_GL_LIGHTS, &e));
}

/*
 * The spot cutoff arrives as a cosine and is compared as one. The
 * spotlight at (0, 0, 2) pointing down -z with exponent 2, as
 * lighting-spot has, but with cosine 0.75 (41.4 degrees): at (1, 0, 0)
 * the light-to-vertex direction makes cosine 2 / sqrt(5) = 0.894 with the
 * axis, inside the cone, so the spot factor is 0.8 and n . VP 0.894427,
 * 0.715542; at (2, 0, 0) it makes 0.707, outside, and is unlit. A cosine
 * of -1, or below, is no spot at all, even at exponent 128: (2, 0, 0) then
 * takes n . VP = 0.707107. One above 1 is clamped to 1, 0 degrees, which
 * glGetLightfv answers. Attenuation (0.5, 0.25, 0.125) at distance 2,
 * straight below, gives 1 / 1.5.
 */
static void test_set_light_spot_cosine(void)
{
    SGIGLEyeLight e = eye_light(0, 0, 2, 1);
    SGIGLContext c;

    lit_init(&c);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_DIFFUSE,
                  1, 1, 1, 1);
    e.spot_cos_cutoff = 0.75f;
    e.spot_exponent = 2;
    g_assert_true(sgi_gl_set_light(&c, 0, &e));
    assert_rgba(lit_point(&c, 1, 0, 0, 0, 0, 1), 0.7155418f, 0.7155418f,
                0.7155418f, 1);
    assert_rgba(lit_point(&c, 2, 0, 0, 0, 0, 1), 0, 0, 0, 1);
    g_assert_cmpfloat_with_epsilon(light_float(&c, 0, SGI_GL_SPOT_CUTOFF, 0),
                                   41.40962f, 0.0001);

    e.spot_exponent = 128;
    e.spot_cos_cutoff = -1;
    g_assert_true(sgi_gl_set_light(&c, 0, &e));
    assert_rgba(lit_point(&c, 2, 0, 0, 0, 0, 1), 0.7071068f, 0.7071068f,
                0.7071068f, 1);
    e.spot_cos_cutoff = -2;
    g_assert_true(sgi_gl_set_light(&c, 0, &e));
    assert_rgba(lit_point(&c, 2, 0, 0, 0, 0, 1), 0.7071068f, 0.7071068f,
                0.7071068f, 1);

    /*
     * A cosine above 1 is clamped to 1, a cutoff of 0 degrees: only the
     * spot's axis is lit, and (1, 0, 0), off it, is not.
     */
    e.spot_exponent = 0;
    e.spot_cos_cutoff = 2;
    g_assert_true(sgi_gl_set_light(&c, 0, &e));
    g_assert_cmpfloat(light_float(&c, 0, SGI_GL_SPOT_CUTOFF, 0), ==, 0);
    assert_rgba(lit_point(&c, 1, 0, 0, 0, 0, 1), 0, 0, 0, 1);
    e.spot_cos_cutoff = -1;

    e.attenuation[0] = 0.5f;
    e.attenuation[1] = 0.25f;
    e.attenuation[2] = 0.125f;
    g_assert_true(sgi_gl_set_light(&c, 0, &e));
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 2 / 3.0f, 2 / 3.0f,
                2 / 3.0f, 1);
}

/*
 * A light at infinity along +z, white, on material diffuse 0.5: a unit
 * normal (0, 0, 1) in eye coordinates lights to 0.5, and n . VP scales it.
 */
static void normal_matrix_init(SGIGLContext *c)
{
    lit_init(c);
    light_command(c, SGI_GL_OP_LIGHT, 0, SGI_GL_DIFFUSE, 1, 1, 1, 1);
    light_command(c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_DIFFUSE,
                  0.5f, 0.5f, 0.5f, 1);
}

/*
 * A family's normal matrix N is used as given: n' = N n. With N's first
 * column (0.6, 0, 0.8), the normal (1, 0, 0) becomes (0.6, 0, 0.8), which
 * the light meets at 0.8: 0.4. N's transpose would give (0.6, 0, 0) and
 * its inverse transpose (1.667, 0, 0), both 0. The modelview, here
 * glScalef(2, 2, 2), plays no part. Without N the modelview's inverse
 * transpose returns: (0, 0, 1) shrinks to 0.5, giving 0.25.
 */
static void test_normal_matrix_as_given(void)
{
    static const float n[9] = { 0.6f, 0, 0.8f, 0, 1, 0, 0, 0, 1 };
    SGIGLContext c;

    normal_matrix_init(&c);
    three(&c, SGI_GL_OP_SCALE, 2, 2, 2);
    sgi_gl_set_normal_matrix(&c, n);
    assert_rgba(lit_point(&c, 0, 0, 0, 1, 0, 0), 0.4f, 0.4f, 0.4f, 1);
    sgi_gl_set_normal_matrix(&c, NULL);
    assert_rgba(lit_point(&c, 0, 0, 0, 1, 0, 0), 0, 0, 0, 1);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0.25f, 0.25f, 0.25f, 1);
}

/*
 * N = 0.5 I makes (0, 0, 1) half length: 0.25, and GL_RESCALE_NORMAL,
 * whose factor comes from the modelview (1 / 0.5 under glScalef(2, 2, 2)),
 * leaves it so. Always-normalise makes it unit, 0.5, as GL_NORMALIZE
 * does. It does the same without N: the modelview's 0.5 normal becomes
 * unit, where it lights to 0.25 with neither enable.
 */
static void test_normal_matrix_normalize_always(void)
{
    static const float half[9] = { 0.5f, 0, 0, 0, 0.5f, 0, 0, 0, 0.5f };
    SGIGLContext c;

    normal_matrix_init(&c);
    three(&c, SGI_GL_OP_SCALE, 2, 2, 2);
    sgi_gl_set_normal_matrix(&c, half);
    enable(&c, SGI_GL_RESCALE_NORMAL, true);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0.25f, 0.25f, 0.25f, 1);
    enable(&c, SGI_GL_RESCALE_NORMAL, false);
    sgi_gl_set_normalize_always(&c, true);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0.5f, 0.5f, 0.5f, 1);
    sgi_gl_set_normalize_always(&c, false);
    enable(&c, SGI_GL_NORMALIZE, true);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0.5f, 0.5f, 0.5f, 1);
    enable(&c, SGI_GL_NORMALIZE, false);

    sgi_gl_set_normal_matrix(&c, NULL);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0.25f, 0.25f, 0.25f, 1);
    sgi_gl_set_normalize_always(&c, true);
    assert_rgba(lit_point(&c, 0, 0, 0, 0, 0, 1), 0.5f, 0.5f, 0.5f, 1);
}

/*
 * The direct setters change the state glEnable(GL_LIGHTING) and
 * glLightModel(GL_LIGHT_MODEL_LOCAL_VIEWER) change. With lighting off the
 * point takes the current colour, white. The light along (0.6, 0, 0.8)
 * at infinity, material specular 0.5 and shininess 10 give the specular
 * term lighting-specular has: 0.295245 with the viewer at infinity, and
 * 0.5 with a local viewer and the vertex at (3, 0, -4), where h is
 * (0, 0, 1).
 */
static void test_set_lighting_and_local_viewer(void)
{
    uint32_t out[SGI_GL_GET_WORDS];
    SGIGLContext c;

    lit_init(&c);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_POSITION, 3, 0, 4, 0);
    light_command(&c, SGI_GL_OP_LIGHT, 0, SGI_GL_SPECULAR, 1, 1, 1, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_SPECULAR,
                  0.5f, 0.5f, 0.5f, 1);
    light_command(&c, SGI_GL_OP_MATERIAL, SGI_GL_FRONT, SGI_GL_SHININESS,
                  10, 0, 0, 0);

    sgi_gl_set_lighting(&c, false);
    g_assert_cmpuint(get(&c, SGI_GL_LIGHTING, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    assert_rgba(lit_point(&c, 3, 0, -4, 0, 0, 1), 1, 1, 1, 1);
    sgi_gl_set_lighting(&c, true);
    g_assert_cmpuint(get(&c, SGI_GL_LIGHTING, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    assert_rgba(lit_point(&c, 3, 0, -4, 0, 0, 1), 0.295245f, 0.295245f,
                0.295245f, 1);

    sgi_gl_set_local_viewer(&c, true);
    g_assert_cmpuint(get(&c, SGI_GL_LIGHT_MODEL_LOCAL_VIEWER, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    assert_rgba(lit_point(&c, 3, 0, -4, 0, 0, 1), 0.5f, 0.5f, 0.5f, 1);
    sgi_gl_set_local_viewer(&c, false);
    assert_rgba(lit_point(&c, 3, 0, -4, 0, 0, 1), 0.295245f, 0.295245f,
                0.295245f, 1);
}

/*
 * sgi_gl_spot_cos() is the cosine glLightfv stores, so a family that
 * migrates only the degrees rebuilds it exactly: -1 for 180, 0.5 for 60,
 * and bit for bit what glLightf(GL_SPOT_CUTOFF) left for 25, an angle
 * whose single-precision cosine differs from the double one rounded.
 */
static void test_spot_cos_matches_gllight(void)
{
    SGIGLContext c;

    context_init(&c);
    g_assert_cmpfloat(sgi_gl_spot_cos(180), ==, -1);
    g_assert_cmpfloat_with_epsilon(sgi_gl_spot_cos(60), 0.5, LIT_EPSILON);
    light_command(&c, SGI_GL_OP_LIGHT, 2, SGI_GL_SPOT_CUTOFF, 25, 0, 0, 0);
    g_assert_cmphex(bits(c.light[2].spot_cos_cutoff), ==,
                    bits(sgi_gl_spot_cos(25)));
    light_command(&c, SGI_GL_OP_LIGHT, 2, SGI_GL_SPOT_CUTOFF, 180, 0, 0, 0);
    g_assert_cmpfloat(c.light[2].spot_cos_cutoff, ==, -1);
}

/* What the unimplemented and guest-error log masks received from fn. */
static char *logged_by(void (*fn)(void))
{
    g_autofree char *dir = g_dir_make_tmp("test-sgi-gl.XXXXXX", NULL);
    g_autofree char *path = NULL;
    char *text = NULL;

    g_assert_nonnull(dir);
    path = g_build_filename(dir, "log", NULL);
    qemu_set_log_filename_flags(path, LOG_UNIMP | LOG_GUEST_ERROR,
                                &error_abort);
    fn();
    /* Closing the log is deferred to an RCU callback, which flushes it. */
    qemu_set_log_filename_flags(NULL, 0, &error_abort);
    drain_call_rcu();
    if (!g_file_get_contents(path, &text, NULL, NULL)) {
        text = g_strdup("");
    }
    g_remove(path);
    g_rmdir(dir);
    return text;
}

static Planes window_planes;
static SGIGLDrawable window_drawable;

/*
 * A clear to white, a one-pixel bitmap at window (10, 20) and a triangle
 * over the lower-left half of the viewport, drawn into window_planes
 * through window_drawable.
 */
static void window_scene(void)
{
    static const uint8_t image[1] = { 0x80 };
    Recorder rec = { .rules = &rules_supported };
    SGIGLSink sink = recorder_sink(&rec);
    SGIGLContext c;
    SGIGLBitmap b;
    uint32_t word[4] = { bits(1), bits(1), bits(1), bits(1) };

    sink.raster = drawing_raster;
    rec.target = planes_target(&window_planes);
    memset(&window_planes, 0, sizeof(window_planes));
    context_init(&c);
    sgi_gl_set_drawable(&c, &window_drawable);
    command(&c, &sink, SGI_GL_OP_CLEAR_COLOR, word, 4);
    word[0] = SGI_GL_COLOR_BUFFER_BIT;
    command(&c, &sink, SGI_GL_OP_CLEAR, word, 1);

    word[0] = bits(10.5f / 32 - 1), word[1] = bits(20.5f / 32 - 1);
    command(&c, &sink, SGI_GL_OP_RASTER_POS2F, word, 2);
    g_assert_true(sgi_gl_bitmap_place(&c, 1, 1, 0, 0, 0, 0, &b));
    sgi_gl_bitmap_bytes(&c, &sink, &b, 0, image, 1);

    word[0] = SGI_GL_TRIANGLES;
    command(&c, &sink, SGI_GL_OP_BEGIN, word, 1);
    word[0] = bits(-1), word[1] = bits(-1);
    command(&c, &sink, SGI_GL_OP_VERTEX2F, word, 2);
    word[0] = bits(1);
    command(&c, &sink, SGI_GL_OP_VERTEX2F, word, 2);
    word[0] = bits(-1), word[1] = bits(1);
    command(&c, &sink, SGI_GL_OP_VERTEX2F, word, 2);
    command(&c, &sink, SGI_GL_OP_END, word, 0);
}

static unsigned window_pixels_written(void)
{
    unsigned n = 0;

    for (int i = 0; i < W * H; i++) {
        n += window_planes.colour[0][i] != 0;
    }
    return n;
}

/*
 * A bound window with no visible rectangle, one other windows wholly
 * cover, is a normal state (OpenGL 1.2.1, 4.1.1: every fragment fails
 * the pixel ownership test). The clear, the bitmap and the triangle draw
 * nothing and log nothing, whatever the list's empty flag says. The same
 * scene into the whole plane writes every pixel, and into no bound window
 * logs each of the three as dropped, which shows the log is captured.
 */
static void test_covered_window_draws_nothing(void)
{
    g_autofree char *covered = NULL;
    g_autofree char *covered_empty = NULL;
    g_autofree char *whole = NULL;
    g_autofree char *unbound = NULL;

    window_drawable = (SGIGLDrawable) { .bound = true };
    covered = logged_by(window_scene);
    g_assert_cmpstr(covered, ==, "");
    g_assert_cmpuint(window_pixels_written(), ==, 0);

    window_drawable.clip.empty = true;
    covered_empty = logged_by(window_scene);
    g_assert_cmpstr(covered_empty, ==, "");
    g_assert_cmpuint(window_pixels_written(), ==, 0);

    window_drawable = (SGIGLDrawable) {
        .bound = true,
        .clip = { .n = 1, .r[0] = { 0, 0, W - 1, H - 1 } },
    };
    whole = logged_by(window_scene);
    g_assert_cmpstr(whole, ==, "");
    g_assert_cmpuint(window_pixels_written(), ==, W * H);

    window_drawable = (SGIGLDrawable) { .bound = false };
    unbound = logged_by(window_scene);
    g_assert_nonnull(strstr(unbound, "glClear with no window bound"));
    g_assert_nonnull(strstr(unbound, "glBitmap with no window bound"));
    g_assert_nonnull(strstr(unbound, "draw with no window bound"));
    g_assert_cmpuint(window_pixels_written(), ==, 0);
}

static SGIRasterOp window_op;

static void window_op_clear(void)
{
    SGIRasterTarget t = planes_target(&window_planes);

    memset(&window_planes, 0, sizeof(window_planes));
    sgi_soft_raster(&rules_supported, &t, &window_op);
}

/*
 * A raster op carries its own clip and needs no bound window. A clear
 * whose list is empty writes nothing and logs nothing, even under a
 * colour mask that would otherwise be reported as unmodelled; a zeroed
 * list is still no clip and writes the whole rectangle.
 */
static void test_empty_clip_without_a_window(void)
{
    g_autofree char *empty = NULL;
    g_autofree char *unclipped = NULL;

    window_op = (SGIRasterOp) {
        .kind = SGI_RASTER_CLEAR,
        .ntargets = 1,
        .x0 = 2, .y0 = 3, .x1 = 9, .y1 = 7,
        .clip = { .empty = true },
        .clear_buffers = SGI_GL_COLOR_BUFFER_BIT,
        .clear_rgba = { 1, 1, 1, 1 },
        .clear_colour_mask = 0xff,
    };
    empty = logged_by(window_op_clear);
    g_assert_cmpstr(empty, ==, "");
    g_assert_cmpuint(window_pixels_written(), ==, 0);

    window_op.clip.empty = false;
    window_op.clear_colour_mask = UINT32_MAX;
    unclipped = logged_by(window_op_clear);
    g_assert_cmpstr(unclipped, ==, "");
    g_assert_cmpuint(window_pixels_written(), ==, 8 * 5);
}

/*
 * The per-fragment operations' cases draw through a context whose draws
 * and raster ops land in one set of planes.
 */
typedef struct FragScene {
    Planes p;
    SGIGLContext c;
    Recorder rec;
    SGIGLSink sink;
} FragScene;

static void frag_init(FragScene *f)
{
    memset(&f->p, 0, sizeof(f->p));
    context_init(&f->c);
    f->rec = (Recorder) {
        .rules = &rules_supported,
        .target = planes_target(&f->p),
    };
    f->sink = recorder_sink(&f->rec);
    f->sink.raster = drawing_raster;
}

static void frag_command(FragScene *f, SGIGLOp op, const uint32_t *args,
                         unsigned nargs)
{
    command(&f->c, &f->sink, op, args, nargs);
}

/* A quad over the whole window in one colour, at normalized depth z. */
static void frag_quad_at(FragScene *f, float r, float g, float b, float a,
                         float z)
{
    static const float corner[4][2] = { { -1, -1 }, { 1, -1 },
                                        { 1, 1 }, { -1, 1 } };
    uint32_t word[4] = { bits(r), bits(g), bits(b), bits(a) };

    frag_command(f, SGI_GL_OP_COLOR4F, word, 4);
    word[0] = SGI_GL_QUADS;
    frag_command(f, SGI_GL_OP_BEGIN, word, 1);
    for (int i = 0; i < 4; i++) {
        word[0] = bits(corner[i][0]);
        word[1] = bits(corner[i][1]);
        word[2] = bits(z);
        frag_command(f, SGI_GL_OP_VERTEX3F, word, 3);
    }
    frag_command(f, SGI_GL_OP_END, word, 0);
}

static void frag_quad(FragScene *f, float r, float g, float b, float a)
{
    frag_quad_at(f, r, g, b, a, 0);
}

static uint32_t frag_pixel(const FragScene *f, int x, int y)
{
    return f->p.colour[0][planes_index(x, y)];
}

/* How many of plane 0's pixels hold colour. */
static unsigned frag_count(const FragScene *f, uint32_t colour)
{
    unsigned n = 0;

    for (int i = 0; i < W * H; i++) {
        n += f->p.colour[0][i] == colour;
    }
    return n;
}

static void frag_scissor(FragScene *f, int32_t x, int32_t y, int32_t w,
                         int32_t h)
{
    uint32_t word[4] = { x, y, w, h };

    frag_command(f, SGI_GL_OP_SCISSOR, word, 4);
}

/*
 * The scissor test (OpenGL 1.2.1, 4.1.2): disabled at first; enabled, a
 * fragment is kept only inside the box, window x from 8 to 11 and y from
 * 16 to 17 here, for draws, glClear and glBitmap alike. A negative size
 * is an error and changes nothing, and glGet answers the enable and box.
 */
static void test_scissor(void)
{
    static const uint8_t image[2] = { 0xff, 0xff };
    static FragScene f;
    uint32_t out[SGI_GL_GET_WORDS], word[4];
    SGIGLBitmap b;

    frag_init(&f);
    g_assert_cmpuint(get(&f.c, SGI_GL_SCISSOR_TEST, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    frag_scissor(&f, 8, 16, 4, 2);
    frag_scissor(&f, 0, 0, -1, 2);
    g_assert_cmpuint(get(&f.c, SGI_GL_SCISSOR_BOX, out), ==, 4);
    g_assert_cmpint(out[0], ==, 8);
    g_assert_cmpint(out[1], ==, 16);
    g_assert_cmpint(out[2], ==, 4);
    g_assert_cmpint(out[3], ==, 2);

    frag_quad(&f, 1, 1, 1, 1);
    g_assert_cmpuint(frag_count(&f, 0xffffff), ==, W * H);
    memset(&f.p, 0, sizeof(f.p));
    enable(&f.c, SGI_GL_SCISSOR_TEST, true);
    g_assert_cmpuint(get(&f.c, SGI_GL_SCISSOR_TEST, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    frag_quad(&f, 1, 1, 1, 1);
    g_assert_cmpuint(frag_count(&f, 0xffffff), ==, 8);
    g_assert_cmphex(frag_pixel(&f, 8, 16), ==, 0xffffff);
    g_assert_cmphex(frag_pixel(&f, 11, 17), ==, 0xffffff);
    g_assert_cmphex(frag_pixel(&f, 7, 16), ==, 0);
    g_assert_cmphex(frag_pixel(&f, 12, 17), ==, 0);
    g_assert_cmphex(frag_pixel(&f, 8, 15), ==, 0);
    g_assert_cmphex(frag_pixel(&f, 11, 18), ==, 0);

    word[0] = bits(1), word[1] = bits(0), word[2] = bits(0), word[3] = 0;
    frag_command(&f, SGI_GL_OP_CLEAR_COLOR, word, 4);
    word[0] = SGI_GL_COLOR_BUFFER_BIT;
    frag_command(&f, SGI_GL_OP_CLEAR, word, 1);
    g_assert_cmpuint(frag_count(&f, 0xff0000), ==, 8);
    g_assert_cmpuint(frag_count(&f, 0), ==, W * H - 8);

    /* A 16 by 1 bitmap across the box's lower row. */
    memset(&f.p, 0, sizeof(f.p));
    word[0] = bits(0.5f / 32 - 1), word[1] = bits(16.5f / 32 - 1);
    frag_command(&f, SGI_GL_OP_RASTER_POS2F, word, 2);
    g_assert_true(sgi_gl_bitmap_place(&f.c, 16, 1, 0, 0, 0, 0, &b));
    sgi_gl_bitmap_bytes(&f.c, &f.sink, &b, 0, image, 2);
    g_assert_cmpuint(frag_count(&f, 0xffffff), ==, 4);
    g_assert_cmphex(frag_pixel(&f, 8, 16), ==, 0xffffff);

    /* A box beyond the window clears nothing; disabled, all of it. */
    memset(&f.p, 0, sizeof(f.p));
    frag_scissor(&f, W, 0, 4, 4);
    word[0] = SGI_GL_COLOR_BUFFER_BIT;
    frag_command(&f, SGI_GL_OP_CLEAR, word, 1);
    g_assert_cmpuint(frag_count(&f, 0), ==, W * H);
    enable(&f.c, SGI_GL_SCISSOR_TEST, false);
    frag_command(&f, SGI_GL_OP_CLEAR, word, 1);
    g_assert_cmpuint(frag_count(&f, 0xff0000), ==, W * H);
}

static void frag_alpha_func(FragScene *f, uint32_t func, float ref)
{
    uint32_t word[2] = { func, bits(ref) };

    frag_command(f, SGI_GL_OP_ALPHA_FUNC, word, 2);
}

/*
 * The alpha test (OpenGL 1.2.1, 4.1.4): initially disabled, GL_ALWAYS and
 * 0. Enabled, a fragment of alpha 0.5 is kept by the comparisons that
 * hold for it against the reference, both in eight bits, where 0.5 and
 * 0.501 are the same 128. A reference outside [0, 1] clamps, a function
 * outside GL_NEVER to GL_ALWAYS changes nothing, and in colour-index mode
 * the test does not apply.
 */
static void test_alpha_test(void)
{
    static const struct {
        uint32_t func;
        float ref;
        bool kept;
    } runs[] = {
        { 0x200, 0, false },            /* GL_NEVER */
        { 0x201, 0.5f, false },         /* GL_LESS */
        { 0x201, 0.51f, true },
        { 0x202, 0.501f, true },        /* GL_EQUAL, both 128 */
        { 0x203, 0.5f, true },          /* GL_LEQUAL */
        { 0x204, 0.5f, false },         /* GL_GREATER */
        { 0x204, -1, true },
        { 0x205, 0.5f, false },         /* GL_NOTEQUAL */
        { 0x206, 2, false },            /* GL_GEQUAL, reference 1 */
        { 0x207, 1, true },             /* GL_ALWAYS */
    };
    static FragScene f;
    uint32_t out[SGI_GL_GET_WORDS];

    frag_init(&f);
    g_assert_cmpuint(get(&f.c, SGI_GL_ALPHA_TEST, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmpuint(get(&f.c, SGI_GL_ALPHA_TEST_FUNC, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0x207);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_ALPHA_TEST_REF, 0), ==, 0);

    frag_alpha_func(&f, 0x200, 0);
    frag_quad(&f, 1, 1, 1, 0.5f);
    g_assert_cmpuint(frag_count(&f, 0xffffff), ==, W * H);
    enable(&f.c, SGI_GL_ALPHA_TEST, true);
    g_assert_cmpuint(get(&f.c, SGI_GL_ALPHA_TEST, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    for (int i = 0; i < ARRAY_SIZE(runs); i++) {
        memset(&f.p, 0, sizeof(f.p));
        frag_alpha_func(&f, runs[i].func, runs[i].ref);
        frag_quad(&f, 1, 1, 1, 0.5f);
        g_assert_cmpuint(frag_count(&f, 0xffffff), ==,
                         runs[i].kept ? W * H : 0);
    }
    g_assert_cmpuint(get(&f.c, SGI_GL_ALPHA_TEST_FUNC, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0x207);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_ALPHA_TEST_REF, 0), ==, 1);
    frag_alpha_func(&f, 0x207, -1);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_ALPHA_TEST_REF, 0), ==, 0);
    frag_alpha_func(&f, 0x207, 2);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_ALPHA_TEST_REF, 0), ==, 1);
    frag_alpha_func(&f, 0x208, 0.25f);
    g_assert_cmpuint(get(&f.c, SGI_GL_ALPHA_TEST_FUNC, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0x207);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_ALPHA_TEST_REF, 0), ==, 1);

    /* GL_NEVER passes everything in colour-index mode. */
    memset(&f.p, 0, sizeof(f.p));
    frag_alpha_func(&f, 0x200, 0);
    sgi_gl_set_index_mode(&f.c, 8);
    frag_quad(&f, 1, 1, 1, 0.5f);
    g_assert_cmpuint(frag_count(&f, 1), ==, W * H);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-gl/batch-matches-pieces", test_batch_matches_pieces);
    g_test_add_func("/sgi-gl/finish-is-a-fence", test_finish_is_a_fence);
    g_test_add_func("/sgi-gl/clear-op", test_clear_op);
    g_test_add_func("/sgi-gl/rounding", test_rounding);
    g_test_add_func("/sgi-gl/set-matrix", test_set_matrix);
    g_test_add_func("/sgi-gl/draw-targets", test_draw_targets);
    g_test_add_func("/sgi-gl/clear-two-targets", test_clear_two_targets);
    g_test_add_func("/sgi-gl/same-front-and-back",
                    test_same_front_and_back);
    g_test_add_func("/sgi-gl/unsupported-rules", test_unsupported_rules);
    g_test_add_func("/sgi-gl/get-state", test_get_state);
    g_test_add_func("/sgi-gl/raster-position", test_raster_position);
    g_test_add_func("/sgi-gl/index-state", test_index_state);
    g_test_add_func("/sgi-gl/index-value", test_index_value);
    g_test_add_func("/sgi-gl/index-draw", test_index_draw);
    g_test_add_func("/sgi-gl/bitmap", test_bitmap);
    g_test_add_func("/sgi-gl/bitmap-bounds-and-depth",
                    test_bitmap_bounds_and_depth);
    g_test_add_func("/sgi-gl/bitmap-depth-is-the-raster-depth",
                    test_bitmap_depth_is_the_raster_depth);
    g_test_add_func("/sgi-gl/depth-range", test_depth_range);
    g_test_add_func("/sgi-gl/depth-range-maps-window-depth",
                    test_depth_range_maps_window_depth);
    g_test_add_func("/sgi-gl/depth-range-maps-the-raster-position",
                    test_depth_range_maps_the_raster_position);
    g_test_add_func("/sgi-gl/get-enables-and-depths",
                    test_get_enables_and_depths);
    g_test_add_func("/sgi-gl/lighting-initial-state",
                    test_lighting_initial_state);
    g_test_add_func("/sgi-gl/lighting-commands", test_lighting_commands);
    g_test_add_func("/sgi-gl/texture-matrix", test_texture_matrix);
    g_test_add_func("/sgi-gl/color-material-tracks-the-colour",
                    test_color_material_tracks_the_colour);
    g_test_add_func("/sgi-gl/lighting-ambient-and-emission",
                    test_lighting_ambient_and_emission);
    g_test_add_func("/sgi-gl/lighting-diffuse", test_lighting_diffuse);
    g_test_add_func("/sgi-gl/lighting-specular", test_lighting_specular);
    g_test_add_func("/sgi-gl/lighting-attenuation",
                    test_lighting_attenuation);
    g_test_add_func("/sgi-gl/lighting-spot", test_lighting_spot);
    g_test_add_func("/sgi-gl/lighting-normal-transform",
                    test_lighting_normal_transform);
    g_test_add_func("/sgi-gl/lighting-two-sided", test_lighting_two_sided);
    g_test_add_func("/sgi-gl/lighting-color-material",
                    test_lighting_color_material);
    g_test_add_func("/sgi-gl/lighting-clamps-and-disables",
                    test_lighting_clamps_and_disables);
    g_test_add_func("/sgi-gl/lighting-raster-position",
                    test_lighting_raster_position);
    g_test_add_func("/sgi-gl/lighting-two-sided-draws",
                    test_lighting_two_sided_draws);
    g_test_add_func("/sgi-gl/lighting-rescale-normal-uses-a-row",
                    test_lighting_rescale_normal_uses_a_row);
    g_test_add_func("/sgi-gl/lighting-both-at-infinity",
                    test_lighting_both_at_infinity);
    g_test_add_func("/sgi-gl/cull-face-state", test_cull_face_state);
    g_test_add_func("/sgi-gl/cull-face-draws", test_cull_face_draws);
    g_test_add_func("/sgi-gl/lighting-two-sided-front-face",
                    test_lighting_two_sided_front_face);
    g_test_add_func("/sgi-gl/set-light-is-eye-space",
                    test_set_light_is_eye_space);
    g_test_add_func("/sgi-gl/set-light-spot-cosine",
                    test_set_light_spot_cosine);
    g_test_add_func("/sgi-gl/normal-matrix-as-given",
                    test_normal_matrix_as_given);
    g_test_add_func("/sgi-gl/normal-matrix-normalize-always",
                    test_normal_matrix_normalize_always);
    g_test_add_func("/sgi-gl/set-lighting-and-local-viewer",
                    test_set_lighting_and_local_viewer);
    g_test_add_func("/sgi-gl/spot-cos-matches-gllight",
                    test_spot_cos_matches_gllight);
    g_test_add_func("/sgi-gl/covered-window-draws-nothing",
                    test_covered_window_draws_nothing);
    g_test_add_func("/sgi-gl/empty-clip-without-a-window",
                    test_empty_clip_without_a_window);
    g_test_add_func("/sgi-gl/scissor", test_scissor);
    g_test_add_func("/sgi-gl/alpha-test", test_alpha_test);
    return g_test_run();
}
