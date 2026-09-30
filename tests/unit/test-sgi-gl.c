/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The shared GL context and software rasterizer, on paths a family's
 * replay fixtures need not reach: whole batches of every primitive mode,
 * glFinish, the clear op a context builds, unsupported rule sets,
 * colour-index mode, glBitmap and view-volume clipping.
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
    uint32_t stencil[W * H];
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

static uint32_t planes_load_colour(void *opaque, SGIColourTarget target,
                                   int x, int y)
{
    Planes *p = opaque;

    g_assert_cmpuint(target, <, 2);
    return p->colour[target][planes_index(x, y)];
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

static uint32_t planes_load_stencil(void *opaque, int x, int y)
{
    return ((Planes *)opaque)->stencil[planes_index(x, y)];
}

static void planes_store_stencil(void *opaque, int x, int y, uint32_t value)
{
    ((Planes *)opaque)->stencil[planes_index(x, y)] = value;
}

static SGIRasterTarget planes_target(Planes *p)
{
    return (SGIRasterTarget) {
        .store_colour = planes_store_colour,
        .load_colour = planes_load_colour,
        .store_index = planes_store_index,
        .load_depth = planes_load_depth,
        .store_depth = planes_store_depth,
        .load_stencil = planes_load_stencil,
        .store_stencil = planes_store_stencil,
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
    rules.dither = SGI_DITHER_BAYER4_TOP_ROWS + 1;
    g_assert_false(sgi_raster_rules_supported(&rules));
    /* A window anchor needs the window's top row; none needs no anchor. */
    rules.dither = SGI_DITHER_BAYER4_TOP_ROWS;
    rules.dither_anchor = SGI_DITHER_WINDOW;
    g_assert_false(sgi_raster_rules_supported(&rules));
    rules.dither = SGI_DITHER_NONE;
    g_assert_true(sgi_raster_rules_supported(&rules));
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

/* The supported rules with an eight-bit stencil buffer. */
static const SGIRasterRules rules_stencil = {
    .subpixel_bits = 8,
    .color_bits = 8,
    .depth_bits = 24,
    .stencil_bits = 8,
    .rounding = SGI_ROUND_HALF_AWAY,
    .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
    .arith = SGI_ARITH_DOUBLE,
    .line_rule = SGI_LINE_COVERAGE_DOUBLE,
    .evidence = "test",
};

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
 * and raster ops land in one set of planes, with a stencil buffer.
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
        .rules = &rules_stencil,
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

static void frag_stencil(FragScene *f, uint32_t func, int32_t ref,
                         uint32_t mask, uint32_t fail, uint32_t zfail,
                         uint32_t zpass)
{
    uint32_t word[3] = { func, ref, mask };

    frag_command(f, SGI_GL_OP_STENCIL_FUNC, word, 3);
    word[0] = fail, word[1] = zfail, word[2] = zpass;
    frag_command(f, SGI_GL_OP_STENCIL_OP, word, 3);
}

static void frag_clear(FragScene *f, uint32_t buffers)
{
    frag_command(f, SGI_GL_OP_CLEAR, &buffers, 1);
}

static uint32_t frag_stencil_at(const FragScene *f, int x, int y)
{
    return f->p.stencil[planes_index(x, y)];
}

/*
 * The stencil test (OpenGL 1.2.1, 4.1.5) and glClear's stencil buffer
 * (4.2.3), against an eight-bit stencil buffer:
 *
 * - initially disabled, GL_ALWAYS, reference 0, masks all ones, every
 *   operation GL_KEEP, clear value 0;
 * - a clear writes the clear value under the write mask;
 * - a fragment passes when the masked reference compares with the masked
 *   stored value as the function says, and the operation for its outcome
 *   updates the buffer: fail, depth fail or depth pass, a disabled depth
 *   test passing;
 * - GL_INCR and GL_DECR clamp, GL_INVERT inverts, GL_REPLACE writes the
 *   reference clamped to 255, GL_ZERO zero, always under the write mask;
 * - an operation or function the specification lacks changes nothing;
 * - planes with no stencil buffer pass every fragment.
 */
static void test_stencil_test(void)
{
    static FragScene f;
    uint32_t out[SGI_GL_GET_WORDS], word[1];

    frag_init(&f);
    g_assert_cmpuint(get(&f.c, SGI_GL_STENCIL_TEST, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmpuint(get(&f.c, SGI_GL_STENCIL_FUNC, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0x207);
    g_assert_cmpuint(get(&f.c, SGI_GL_STENCIL_REF, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmpuint(get(&f.c, SGI_GL_STENCIL_VALUE_MASK, out), ==, 1);
    g_assert_cmphex(out[0], ==, UINT32_MAX);
    g_assert_cmpuint(get(&f.c, SGI_GL_STENCIL_WRITEMASK, out), ==, 1);
    g_assert_cmphex(out[0], ==, UINT32_MAX);
    for (uint32_t p = SGI_GL_STENCIL_FAIL; p <= SGI_GL_STENCIL_PASS_DEPTH_PASS;
         p++) {
        g_assert_cmpuint(get(&f.c, p, out), ==, 1);
        g_assert_cmphex(out[0], ==, SGI_GL_KEEP);
    }
    g_assert_cmpuint(get(&f.c, SGI_GL_STENCIL_CLEAR_VALUE, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);

    /* Clear to 0x5a under write mask 0x0f over 0xff: 0xfa. */
    memset(f.p.stencil, 0xff, sizeof(f.p.stencil));
    word[0] = 0x5a;
    frag_command(&f, SGI_GL_OP_CLEAR_STENCIL, word, 1);
    word[0] = 0x0f;
    frag_command(&f, SGI_GL_OP_STENCIL_MASK, word, 1);
    frag_clear(&f, SGI_GL_STENCIL_BUFFER_BIT);
    g_assert_cmphex(frag_stencil_at(&f, 0, 0), ==, 0xfa);
    g_assert_cmphex(frag_stencil_at(&f, W - 1, H - 1), ==, 0xfa);
    word[0] = UINT32_MAX;
    frag_command(&f, SGI_GL_OP_STENCIL_MASK, word, 1);
    word[0] = 0;
    frag_command(&f, SGI_GL_OP_CLEAR_STENCIL, word, 1);
    frag_clear(&f, SGI_GL_STENCIL_BUFFER_BIT);
    g_assert_cmphex(frag_stencil_at(&f, 3, 3), ==, 0);

    /* Mark the left half with 1, then draw only where it is 1. */
    enable(&f.c, SGI_GL_STENCIL_TEST, true);
    enable(&f.c, SGI_GL_SCISSOR_TEST, true);
    frag_scissor(&f, 0, 0, W / 2, H);
    frag_stencil(&f, 0x207, 1, 0xff, SGI_GL_KEEP, SGI_GL_KEEP,
                 SGI_GL_REPLACE);
    frag_quad(&f, 0, 0, 1, 1);
    enable(&f.c, SGI_GL_SCISSOR_TEST, false);
    g_assert_cmphex(frag_stencil_at(&f, 0, 0), ==, 1);
    g_assert_cmphex(frag_stencil_at(&f, W / 2, 0), ==, 0);
    memset(f.p.colour, 0, sizeof(f.p.colour));
    frag_stencil(&f, 0x202, 1, 0xff, SGI_GL_KEEP, SGI_GL_KEEP, SGI_GL_KEEP);
    frag_quad(&f, 0, 1, 0, 1);
    g_assert_cmpuint(frag_count(&f, 0x00ff00), ==, W * H / 2);
    g_assert_cmphex(frag_pixel(&f, W / 2 - 1, 5), ==, 0x00ff00);
    g_assert_cmphex(frag_pixel(&f, W / 2, 5), ==, 0);
    /* GL_LESS: reference 0 below the stored 1, the left half again. */
    memset(f.p.colour, 0, sizeof(f.p.colour));
    frag_stencil(&f, 0x201, 0, 0xff, SGI_GL_KEEP, SGI_GL_KEEP, SGI_GL_KEEP);
    frag_quad(&f, 0, 1, 0, 1);
    g_assert_cmpuint(frag_count(&f, 0x00ff00), ==, W * H / 2);
    g_assert_cmphex(frag_pixel(&f, 0, 5), ==, 0x00ff00);
    /* The value mask: reference 3 and stored 1 agree in bit 0. */
    memset(f.p.colour, 0, sizeof(f.p.colour));
    frag_stencil(&f, 0x202, 3, 0x01, SGI_GL_KEEP, SGI_GL_KEEP, SGI_GL_KEEP);
    frag_quad(&f, 0, 1, 0, 1);
    g_assert_cmpuint(frag_count(&f, 0x00ff00), ==, W * H / 2);

    /*
     * Failing: GL_NEVER, fail GL_INCR twice from 254 clamps at 255;
     * GL_DECR from 1 clamps at 0; GL_INVERT of 0x0f under write mask
     * 0x3c is 0x33; GL_ZERO; GL_REPLACE with reference 300 writes 255.
     */
    memset(f.p.stencil, 0, sizeof(f.p.stencil));
    f.p.stencil[planes_index(0, 0)] = 254;
    f.p.stencil[planes_index(1, 0)] = 1;
    f.p.stencil[planes_index(2, 0)] = 0x0f;
    f.p.stencil[planes_index(3, 0)] = 0x77;
    f.p.stencil[planes_index(4, 0)] = 0x10;
    memset(f.p.colour, 0, sizeof(f.p.colour));
    frag_stencil(&f, 0x200, 0, 0xff, SGI_GL_INCR, SGI_GL_KEEP, SGI_GL_KEEP);
    frag_quad(&f, 0, 1, 0, 1);
    frag_quad(&f, 0, 1, 0, 1);
    g_assert_cmpuint(frag_count(&f, 0), ==, W * H);
    g_assert_cmphex(frag_stencil_at(&f, 0, 0), ==, 255);
    g_assert_cmphex(frag_stencil_at(&f, 5, 5), ==, 2);
    g_assert_cmphex(frag_stencil_at(&f, 1, 0), ==, 3);
    frag_stencil(&f, 0x200, 0, 0xff, SGI_GL_DECR, SGI_GL_KEEP, SGI_GL_KEEP);
    for (int i = 0; i < 3; i++) {
        frag_quad(&f, 0, 1, 0, 1);
    }
    g_assert_cmphex(frag_stencil_at(&f, 5, 5), ==, 0);
    g_assert_cmphex(frag_stencil_at(&f, 1, 0), ==, 0);
    g_assert_cmphex(frag_stencil_at(&f, 0, 0), ==, 252);
    g_assert_cmphex(frag_stencil_at(&f, 2, 0), ==, 0x0e);
    word[0] = 0x3c;
    frag_command(&f, SGI_GL_OP_STENCIL_MASK, word, 1);
    frag_stencil(&f, 0x200, 0, 0xff, SGI_GL_INVERT, SGI_GL_KEEP,
                 SGI_GL_KEEP);
    frag_quad(&f, 0, 1, 0, 1);
    g_assert_cmphex(frag_stencil_at(&f, 2, 0), ==, 0x32);
    word[0] = UINT32_MAX;
    frag_command(&f, SGI_GL_OP_STENCIL_MASK, word, 1);
    frag_stencil(&f, 0x200, 0, 0xff, 0, SGI_GL_KEEP, SGI_GL_KEEP);
    frag_quad(&f, 0, 1, 0, 1);
    g_assert_cmphex(frag_stencil_at(&f, 3, 0), ==, 0);
    frag_stencil(&f, 0x200, 300, 0xff, SGI_GL_REPLACE, SGI_GL_KEEP,
                 SGI_GL_KEEP);
    frag_quad(&f, 0, 1, 0, 1);
    g_assert_cmphex(frag_stencil_at(&f, 4, 0), ==, 255);
    g_assert_cmpuint(get(&f.c, SGI_GL_STENCIL_REF, out), ==, 1);
    g_assert_cmpint(out[0], ==, 300);

    /*
     * Depth fail and depth pass: the depth buffer holds 0 on the left and
     * the far plane on the right, so GL_LESS fails on the left.
     */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            f.p.depth[planes_index(x, y)] = x < W / 2 ? 0 : 0xffffff;
            f.p.stencil[planes_index(x, y)] = 10;
        }
    }
    enable(&f.c, SGI_GL_DEPTH_TEST, true);
    frag_stencil(&f, 0x207, 0, 0xff, SGI_GL_KEEP, SGI_GL_INCR, SGI_GL_DECR);
    frag_quad(&f, 0, 1, 0, 1);
    g_assert_cmphex(frag_stencil_at(&f, 0, 0), ==, 11);
    g_assert_cmphex(frag_stencil_at(&f, W - 1, 0), ==, 9);
    enable(&f.c, SGI_GL_DEPTH_TEST, false);
    frag_quad(&f, 0, 1, 0, 1);
    g_assert_cmphex(frag_stencil_at(&f, 0, 0), ==, 10);
    g_assert_cmphex(frag_stencil_at(&f, W - 1, 0), ==, 8);

    /* Neither a bad operation nor a bad function changes anything. */
    frag_stencil(&f, 0x208, 7, 0x12, 0x1e04, SGI_GL_KEEP, SGI_GL_KEEP);
    g_assert_cmpuint(get(&f.c, SGI_GL_STENCIL_FUNC, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0x207);
    g_assert_cmpuint(get(&f.c, SGI_GL_STENCIL_FAIL, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_KEEP);
    g_assert_cmpuint(get(&f.c, SGI_GL_STENCIL_PASS_DEPTH_PASS, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_DECR);

    /* Without a stencil buffer GL_NEVER draws, and clears write nothing. */
    f.rec.rules = &rules_supported;
    memset(f.p.colour, 0, sizeof(f.p.colour));
    frag_stencil(&f, 0x200, 0, 0xff, SGI_GL_INCR, SGI_GL_INCR, SGI_GL_INCR);
    frag_quad(&f, 0, 1, 0, 1);
    g_assert_cmpuint(frag_count(&f, 0x00ff00), ==, W * H);
    g_assert_cmphex(frag_stencil_at(&f, 0, 0), ==, 10);
    frag_clear(&f, SGI_GL_STENCIL_BUFFER_BIT);
    g_assert_cmphex(frag_stencil_at(&f, 0, 0), ==, 10);
    f.rec.rules = &rules_stencil;
    f.rec.target.load_stencil = NULL;
    frag_quad(&f, 0, 1, 0, 1);
    g_assert_cmphex(frag_stencil_at(&f, 0, 0), ==, 10);
}

static void frag_blend(FragScene *f, uint32_t src, uint32_t dst,
                       uint32_t equation)
{
    uint32_t word[2] = { src, dst };

    frag_command(f, SGI_GL_OP_BLEND_FUNC, word, 2);
    frag_command(f, SGI_GL_OP_BLEND_EQUATION, &equation, 1);
}

/*
 * Blending (OpenGL 1.2.1, 4.1.6), over a window of (1, 0.5, 0) and with
 * source alpha 0.25, which eight bits hold as 64 / 255:
 *
 * - initially disabled, GL_ONE, GL_ZERO and GL_FUNC_ADD, constant 0;
 * - source over destination by the source alpha;
 * - each equation, the constant factors and GL_SRC_ALPHA_SATURATE, with
 *   the destination's alpha 1 as planes without alpha have it;
 * - a factor or equation the specification lacks changes nothing;
 * - not in colour-index mode, and not where the planes cannot be read.
 */
static void test_blend(void)
{
    static const struct {
        uint32_t src, dst, eq;
        float r, g, b, a;
        uint32_t want;
    } runs[] = {
        /* 64 / 255 (0, 1, 1) + 191 / 255 (1, 128 / 255, 0) */
        { SGI_GL_SRC_ALPHA, SGI_GL_ONE_MINUS_SRC_ALPHA, SGI_GL_FUNC_ADD,
          0, 1, 1, 0.25f, 0xbfa040 },
        { 1, 1, SGI_GL_FUNC_ADD, 0, 0.25f, 1, 1, 0xffc0ff },
        { 1, 1, SGI_GL_FUNC_SUBTRACT, 0.5f, 0.25f, 1, 1, 0x0000ff },
        { 1, 1, SGI_GL_FUNC_REVERSE_SUBTRACT, 0.5f, 0.25f, 1, 1, 0x7f4000 },
        { 0, 0, SGI_GL_MIN, 0.5f, 1, 1, 1, 0x808000 },
        { 0, 0, SGI_GL_MAX, 0.5f, 0.25f, 1, 1, 0xff80ff },
        /* Destination alpha 1: the source whole, the destination not. */
        { SGI_GL_DST_ALPHA, SGI_GL_ONE_MINUS_DST_ALPHA, SGI_GL_FUNC_ADD,
          0, 0, 1, 0, 0x0000ff },
        /* min(0.251, 1 - 1) = 0 */
        { SGI_GL_SRC_ALPHA_SATURATE, 1, SGI_GL_FUNC_ADD, 0, 0, 1, 0.25f,
          0xff8000 },
        /* Constant (0.5, 0.25, 1, 0.5); 0.25 + 0.5 * 128 / 255 is 128. */
        { SGI_GL_CONSTANT_COLOR, SGI_GL_ONE_MINUS_CONSTANT_ALPHA,
          SGI_GL_FUNC_ADD, 1, 1, 1, 1, 0xff80ff },
        { SGI_GL_DST_COLOR, SGI_GL_SRC_COLOR, SGI_GL_FUNC_ADD,
          0.5f, 0.5f, 0.5f, 1, 0xff8100 },
    };
    static FragScene f;
    uint32_t out[SGI_GL_GET_WORDS], word[4];

    frag_init(&f);
    g_assert_cmpuint(get(&f.c, SGI_GL_BLEND, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmpuint(get(&f.c, SGI_GL_BLEND_SRC, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    g_assert_cmpuint(get(&f.c, SGI_GL_BLEND_DST, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmpuint(get(&f.c, SGI_GL_BLEND_EQUATION, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_FUNC_ADD);
    g_assert_cmpuint(get(&f.c, SGI_GL_BLEND_COLOR, out), ==, 4);
    g_assert_cmphex(out[0] | out[1] | out[2] | out[3], ==, 0);

    word[0] = bits(0.5f), word[1] = bits(0.25f), word[2] = bits(2);
    word[3] = bits(0.5f);
    frag_command(&f, SGI_GL_OP_BLEND_COLOR, word, 4);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_BLEND_COLOR, 2), ==, 1);
    enable(&f.c, SGI_GL_BLEND, true);
    for (int i = 0; i < ARRAY_SIZE(runs); i++) {
        enable(&f.c, SGI_GL_BLEND, false);
        frag_quad(&f, 1, 0.5f, 0, 1);
        enable(&f.c, SGI_GL_BLEND, true);
        frag_blend(&f, runs[i].src, runs[i].dst, runs[i].eq);
        frag_quad(&f, runs[i].r, runs[i].g, runs[i].b, runs[i].a);
        g_assert_cmphex(frag_pixel(&f, 7, 9), ==, runs[i].want);
        g_assert_cmpuint(frag_count(&f, runs[i].want), ==, W * H);
    }

    /* GL_SRC_COLOR is no source factor, 0x8005 no equation. */
    frag_blend(&f, SGI_GL_SRC_COLOR, 0, 0x8005);
    g_assert_cmpuint(get(&f.c, SGI_GL_BLEND_SRC, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_DST_COLOR);
    g_assert_cmpuint(get(&f.c, SGI_GL_BLEND_EQUATION, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_FUNC_ADD);
    g_assert_cmpuint(get(&f.c, SGI_GL_BLEND, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);

    /* Planes that cannot be read take the source. */
    frag_blend(&f, 1, 1, SGI_GL_FUNC_ADD);
    f.rec.target.load_colour = NULL;
    frag_quad(&f, 0, 0, 1, 1);
    g_assert_cmpuint(frag_count(&f, 0x0000ff), ==, W * H);
    f.rec.target = planes_target(&f.p);

    /* In colour-index mode the index is stored as it is. */
    sgi_gl_set_index_mode(&f.c, 8);
    word[0] = bits(5);
    frag_command(&f, SGI_GL_OP_INDEXF, word, 1);
    frag_quad(&f, 0, 0, 1, 1);
    g_assert_cmpuint(frag_count(&f, 5), ==, W * H);
}

static void frag_logic_op(FragScene *f, uint32_t op)
{
    frag_command(f, SGI_GL_OP_LOGIC_OP, &op, 1);
}

/*
 * The logical operation (OpenGL 1.2.1, 4.1.8, table 4.3): each of the 16
 * over a destination of 0xf0 in every channel with a source of 0xcc,
 * while GL_COLOR_LOGIC_OP is enabled, and replacing blending; through
 * EXT_blend_logic_op's blend equation; on a colour index while
 * GL_INDEX_LOGIC_OP is enabled, under the index mask. Initially both are
 * disabled with GL_COPY, and an operation outside GL_CLEAR to GL_SET
 * changes nothing.
 */
static void test_logic_op(void)
{
    static const uint8_t want[16] = {
        0x00, 0xc0, 0x0c, 0xcc, 0x30, 0xf0, 0x3c, 0xfc,
        0x03, 0xc3, 0x0f, 0xcf, 0x33, 0xf3, 0x3f, 0xff,
    };
    static FragScene f;
    uint32_t out[SGI_GL_GET_WORDS], word[1];
    const float s = 0xcc / 255.0f, d = 0xf0 / 255.0f;

    frag_init(&f);
    g_assert_cmpuint(get(&f.c, SGI_GL_COLOR_LOGIC_OP, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmpuint(get(&f.c, SGI_GL_INDEX_LOGIC_OP, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmpuint(get(&f.c, SGI_GL_LOGIC_OP_MODE, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_COPY);

    enable(&f.c, SGI_GL_BLEND, true);
    frag_blend(&f, 1, 1, SGI_GL_FUNC_ADD);
    for (uint32_t op = 0; op < 16; op++) {
        enable(&f.c, SGI_GL_COLOR_LOGIC_OP, false);
        enable(&f.c, SGI_GL_BLEND, false);
        frag_quad(&f, d, d, d, 1);
        enable(&f.c, SGI_GL_BLEND, true);
        enable(&f.c, SGI_GL_COLOR_LOGIC_OP, true);
        frag_logic_op(&f, SGI_GL_CLEAR + op);
        frag_quad(&f, s, s, s, 1);
        g_assert_cmpuint(frag_count(&f, want[op] * 0x010101), ==, W * H);
    }
    g_assert_cmpuint(get(&f.c, SGI_GL_COLOR_LOGIC_OP, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    frag_logic_op(&f, SGI_GL_SET + 1);
    g_assert_cmpuint(get(&f.c, SGI_GL_LOGIC_OP_MODE, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_SET);

    /* The blend equation GL_LOGIC_OP: GL_XOR. */
    enable(&f.c, SGI_GL_COLOR_LOGIC_OP, false);
    frag_logic_op(&f, SGI_GL_CLEAR + 6);
    enable(&f.c, SGI_GL_BLEND, false);
    frag_quad(&f, d, d, d, 1);
    enable(&f.c, SGI_GL_BLEND, true);
    frag_blend(&f, 1, 1, SGI_GL_LOGIC_OP);
    frag_quad(&f, s, s, s, 1);
    g_assert_cmpuint(frag_count(&f, 0x3c3c3c), ==, W * H);

    /*
     * Colour index 0xcc XOR 0xf0 under the mask 0x3f; GL_INDEX_LOGIC_OP
     * applies there, GL_COLOR_LOGIC_OP does not.
     */
    enable(&f.c, SGI_GL_BLEND, false);
    enable(&f.c, SGI_GL_COLOR_LOGIC_OP, true);
    sgi_gl_set_index_mode(&f.c, 8);
    for (int i = 0; i < W * H; i++) {
        f.p.colour[0][i] = 0xf0;
    }
    word[0] = bits(0xcc);
    frag_command(&f, SGI_GL_OP_INDEXF, word, 1);
    frag_quad(&f, 0, 0, 0, 1);
    g_assert_cmpuint(frag_count(&f, 0xcc), ==, W * H);
    enable(&f.c, SGI_GL_INDEX_LOGIC_OP, true);
    word[0] = 0x3f;
    frag_command(&f, SGI_GL_OP_INDEX_MASK, word, 1);
    frag_quad(&f, 0, 0, 0, 1);
    g_assert_cmpuint(frag_count(&f, 0xc0 | ((0xcc ^ 0xcc) & 0x3f)), ==,
                     W * H);
    frag_quad(&f, 0, 0, 0, 1);
    g_assert_cmpuint(frag_count(&f, 0xc0 | ((0xcc ^ 0xc0) & 0x3f)), ==,
                     W * H);

    /* Planes that cannot be read store the index as it is. */
    f.rec.target.load_colour = NULL;
    frag_quad(&f, 0, 0, 0, 1);
    g_assert_cmpuint(frag_count(&f, 0xcc), ==, W * H);
}

static void frag_colour_mask(FragScene *f, bool r, bool g, bool b, bool a)
{
    uint32_t word[4] = { r, g, b, a };

    frag_command(f, SGI_GL_OP_COLOR_MASK, word, 4);
}

/*
 * glColorMask (OpenGL 1.2.1, 4.2.2): initially every channel; with some
 * cleared a draw and a clear keep the stored bits of those channels, with
 * none they write nothing, and the alpha channel's bit changes nothing in
 * planes without alpha. Planes that cannot be read are written whole.
 */
static void test_colour_mask(void)
{
    static FragScene f;
    uint32_t out[SGI_GL_GET_WORDS], word[4];

    frag_init(&f);
    g_assert_cmpuint(get(&f.c, SGI_GL_COLOR_WRITEMASK, out), ==, 4);
    for (int i = 0; i < 4; i++) {
        g_assert_cmphex(out[i], ==, 1);
    }
    frag_quad(&f, 0, 0.5f, 1, 1);
    frag_colour_mask(&f, true, false, true, false);
    g_assert_cmpuint(get(&f.c, SGI_GL_COLOR_WRITEMASK, out), ==, 4);
    g_assert_cmphex(out[0] << 3 | out[1] << 2 | out[2] << 1 | out[3], ==,
                    0xa);
    frag_quad(&f, 1, 1, 0, 1);
    g_assert_cmpuint(frag_count(&f, 0xff8000), ==, W * H);

    word[0] = bits(0), word[1] = bits(1), word[2] = bits(1);
    word[3] = bits(1);
    frag_command(&f, SGI_GL_OP_CLEAR_COLOR, word, 4);
    frag_colour_mask(&f, false, false, true, true);
    frag_clear(&f, SGI_GL_COLOR_BUFFER_BIT);
    g_assert_cmpuint(frag_count(&f, 0xff80ff), ==, W * H);

    frag_colour_mask(&f, false, false, false, true);
    frag_quad(&f, 0, 0, 0, 1);
    frag_clear(&f, SGI_GL_COLOR_BUFFER_BIT);
    g_assert_cmpuint(frag_count(&f, 0xff80ff), ==, W * H);

    /* Unreadable planes: no channel still writes nothing, some all. */
    f.rec.target.load_colour = NULL;
    frag_quad(&f, 0, 0, 0, 1);
    frag_clear(&f, SGI_GL_COLOR_BUFFER_BIT);
    g_assert_cmpuint(frag_count(&f, 0xff80ff), ==, W * H);
    frag_colour_mask(&f, true, false, false, false);
    frag_quad(&f, 0, 0, 0, 1);
    g_assert_cmpuint(frag_count(&f, 0), ==, W * H);
    frag_clear(&f, SGI_GL_COLOR_BUFFER_BIT);
    g_assert_cmpuint(frag_count(&f, 0x00ffff), ==, W * H);
}

static void frag_fog(FragScene *f, uint32_t pname, float v0, float v1,
                     float v2, float v3)
{
    uint32_t word[5] = { pname, bits(v0), bits(v1), bits(v2), bits(v3) };

    frag_command(f, SGI_GL_OP_FOG, word, 5);
}

/*
 * Fog (OpenGL 1.2.1, 3.10) of blue toward red, the fog coordinate |z| in
 * eye coordinates, 0.5 for a quad at z -0.5 under identity matrices:
 *
 * - initially disabled, GL_EXP, density 1, start 0, end 1, colour and
 *   index 0;
 * - GL_LINEAR from 0 to 2 gives f 0.75, GL_EXP of density 2 e^-1 and
 *   GL_EXP2 of density 1 e^-0.25;
 * - GL_LINEAR with start and end both 0.5 leaves a fragment before them
 *   and fogs one at them;
 * - a bitmap takes the raster position's eye distance;
 * - in colour-index mode (1 - f) times the fog index is added;
 * - a negative density or an unknown mode changes nothing.
 */
static void test_fog(void)
{
    static const uint8_t image[1] = { 0x80 };
    static FragScene f;
    uint32_t out[SGI_GL_GET_WORDS], word[3];
    SGIGLBitmap b;

    frag_init(&f);
    g_assert_cmpuint(get(&f.c, SGI_GL_FOG, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmpuint(get(&f.c, SGI_GL_FOG_MODE, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_EXP);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_FOG_DENSITY, 0), ==, 1);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_FOG_START, 0), ==, 0);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_FOG_END, 0), ==, 1);
    g_assert_cmpuint(get(&f.c, SGI_GL_FOG_COLOR, out), ==, 4);
    g_assert_cmphex(out[0] | out[1] | out[2] | out[3], ==, 0);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_FOG_INDEX, 0), ==, 0);

    frag_fog(&f, SGI_GL_FOG_COLOR, 2, 0, 0, 1);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_FOG_COLOR, 0), ==, 1);
    frag_fog(&f, SGI_GL_FOG_MODE, SGI_GL_LINEAR, 0, 0, 0);
    frag_fog(&f, SGI_GL_FOG_END, 2, 0, 0, 0);
    frag_quad_at(&f, 0, 0, 1, 1, -0.5f);
    g_assert_cmpuint(frag_count(&f, 0x0000ff), ==, W * H);
    enable(&f.c, SGI_GL_FOG, true);
    g_assert_cmpuint(get(&f.c, SGI_GL_FOG, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    frag_quad_at(&f, 0, 0, 1, 1, -0.5f);
    g_assert_cmpuint(frag_count(&f, 0x4000bf), ==, W * H);
    frag_quad_at(&f, 0, 0, 1, 1, 0.5f);
    g_assert_cmpuint(frag_count(&f, 0x4000bf), ==, W * H);

    frag_fog(&f, SGI_GL_FOG_MODE, SGI_GL_EXP, 0, 0, 0);
    frag_fog(&f, SGI_GL_FOG_DENSITY, 2, 0, 0, 0);
    frag_quad_at(&f, 0, 0, 1, 1, -0.5f);
    g_assert_cmpuint(frag_count(&f, 0xa1005e), ==, W * H);
    frag_fog(&f, SGI_GL_FOG_MODE, SGI_GL_EXP2, 0, 0, 0);
    frag_fog(&f, SGI_GL_FOG_DENSITY, 1, 0, 0, 0);
    frag_quad_at(&f, 0, 0, 1, 1, -0.5f);
    g_assert_cmpuint(frag_count(&f, 0x3800c7), ==, W * H);

    frag_fog(&f, SGI_GL_FOG_MODE, SGI_GL_LINEAR, 0, 0, 0);
    frag_fog(&f, SGI_GL_FOG_START, 0.5f, 0, 0, 0);
    frag_fog(&f, SGI_GL_FOG_END, 0.5f, 0, 0, 0);
    frag_quad_at(&f, 0, 0, 1, 1, -0.25f);
    g_assert_cmpuint(frag_count(&f, 0x0000ff), ==, W * H);
    frag_quad_at(&f, 0, 0, 1, 1, -0.5f);
    g_assert_cmpuint(frag_count(&f, 0xff0000), ==, W * H);

    /* A bitmap at eye (0, 0, -0.5), under GL_LINEAR from 0 to 2. */
    frag_fog(&f, SGI_GL_FOG_START, 0, 0, 0, 0);
    frag_fog(&f, SGI_GL_FOG_END, 2, 0, 0, 0);
    word[0] = bits(0), word[1] = bits(0), word[2] = bits(1);
    frag_command(&f, SGI_GL_OP_COLOR3F, word, 3);
    word[0] = bits(0), word[1] = bits(0), word[2] = bits(-0.5f);
    frag_command(&f, SGI_GL_OP_RASTER_POS3F, word, 3);
    g_assert_true(sgi_gl_bitmap_place(&f.c, 1, 1, 0, 0, 0, 0, &b));
    sgi_gl_bitmap_bytes(&f.c, &f.sink, &b, 0, image, 1);
    g_assert_cmphex(frag_pixel(&f, 32, 32), ==, 0x4000bf);
    g_assert_cmpuint(frag_count(&f, 0x4000bf), ==, 1);

    /* Neither changes the state. */
    frag_fog(&f, SGI_GL_FOG_DENSITY, -1, 0, 0, 0);
    frag_fog(&f, SGI_GL_FOG_MODE, 0x2600, 0, 0, 0);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_FOG_DENSITY, 0), ==, 1);
    g_assert_cmpuint(get(&f.c, SGI_GL_FOG_MODE, out), ==, 1);
    g_assert_cmphex(out[0], ==, SGI_GL_LINEAR);

    /*
     * Index 4 with fog index 3: f 0.625 gives 5.125, stored as 5, and f
     * 0.25 gives 6.25, stored as 6.
     */
    sgi_gl_set_index_mode(&f.c, 8);
    frag_fog(&f, SGI_GL_FOG_INDEX, 3, 0, 0, 0);
    word[0] = bits(4);
    frag_command(&f, SGI_GL_OP_INDEXF, word, 1);
    frag_quad_at(&f, 0, 0, 0, 1, -0.75f);
    g_assert_cmpuint(frag_count(&f, 5), ==, W * H);
    frag_fog(&f, SGI_GL_FOG_START, 0, 0, 0, 0);
    frag_fog(&f, SGI_GL_FOG_END, 1, 0, 0, 0);
    frag_quad_at(&f, 0, 0, 0, 1, -0.75f);
    g_assert_cmpuint(frag_count(&f, 6), ==, W * H);
    /* Before the start f is 1 and the index stays 4, not 2.5. */
    frag_fog(&f, SGI_GL_FOG_START, 1, 0, 0, 0);
    frag_fog(&f, SGI_GL_FOG_END, 2, 0, 0, 0);
    frag_quad_at(&f, 0, 0, 0, 1, -0.5f);
    g_assert_cmpuint(frag_count(&f, 4), ==, W * H);
}

static void frag_offset(FragScene *f, SGIGLOp op, float factor, float c)
{
    uint32_t word[2] = { bits(factor), bits(c) };

    frag_command(f, op, word, 2);
}

/* A quad over the window whose normalized depth runs from z0 to z1 in x. */
static void frag_slope(FragScene *f, float r, float g, float b, float z0,
                       float z1)
{
    static const float corner[4][2] = { { -1, -1 }, { 1, -1 },
                                        { 1, 1 }, { -1, 1 } };
    uint32_t word[4] = { bits(r), bits(g), bits(b), bits(1) };

    frag_command(f, SGI_GL_OP_COLOR4F, word, 4);
    word[0] = SGI_GL_QUADS;
    frag_command(f, SGI_GL_OP_BEGIN, word, 1);
    for (int i = 0; i < 4; i++) {
        word[0] = bits(corner[i][0]);
        word[1] = bits(corner[i][1]);
        /* Normalized z (zd + 1) / 2 under identity matrices. */
        word[2] = bits(2 * (corner[i][0] < 0 ? z0 : z1) - 1);
        frag_command(f, SGI_GL_OP_VERTEX3F, word, 3);
    }
    frag_command(f, SGI_GL_OP_END, word, 0);
}

/*
 * Polygon offset (OpenGL 1.2.1, 3.5.5): a second quad at the depth of
 * the first fails GL_LESS unless offset toward the eye. Initially every
 * enable is off and factor, units and bias 0. With GL_POLYGON_OFFSET_FILL
 * enabled, units of -1 move it one depth step nearer and +1 one farther;
 * EXT_polygon_offset's bias moves it in window depth; a factor scales the
 * larger depth slope, here 0.5 over the window's 64 pixels, so a factor
 * of -0.5 moves it 1/256 nearer, more than the depth rounding. Lines are
 * not offset, and neither are polygons with only the line and point
 * enables. The offset depth is clamped to [0, 1].
 */
static void test_polygon_offset(void)
{
    static FragScene f;
    uint32_t out[SGI_GL_GET_WORDS], word[2];
    static const uint32_t caps[3] = { SGI_GL_POLYGON_OFFSET_FILL,
                                      SGI_GL_POLYGON_OFFSET_LINE,
                                      SGI_GL_POLYGON_OFFSET_POINT };

    frag_init(&f);
    for (int i = 0; i < 3; i++) {
        g_assert_cmpuint(get(&f.c, caps[i], out), ==, 1);
        g_assert_cmphex(out[0], ==, 0);
    }
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_POLYGON_OFFSET_FACTOR, 0), ==, 0);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_POLYGON_OFFSET_UNITS, 0), ==, 0);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_POLYGON_OFFSET_BIAS, 0), ==, 0);
    enable(&f.c, SGI_GL_DEPTH_TEST, true);
    frag_clear(&f, SGI_GL_DEPTH_BUFFER_BIT);

    /* Units: -1 passes GL_LESS at an equal depth, +1 does not. */
    frag_quad_at(&f, 1, 0, 0, 1, 0);
    frag_offset(&f, SGI_GL_OP_POLYGON_OFFSET, 0, -1);
    frag_quad_at(&f, 0, 1, 0, 1, 0);
    g_assert_cmpuint(frag_count(&f, 0xff0000), ==, W * H);
    enable(&f.c, SGI_GL_POLYGON_OFFSET_LINE, true);
    enable(&f.c, SGI_GL_POLYGON_OFFSET_POINT, true);
    frag_quad_at(&f, 0, 1, 0, 1, 0);
    g_assert_cmpuint(frag_count(&f, 0xff0000), ==, W * H);
    enable(&f.c, SGI_GL_POLYGON_OFFSET_FILL, true);
    g_assert_cmpuint(get(&f.c, SGI_GL_POLYGON_OFFSET_FILL, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    frag_offset(&f, SGI_GL_OP_POLYGON_OFFSET, 0, 1);
    frag_quad_at(&f, 0, 1, 0, 1, 0);
    g_assert_cmpuint(frag_count(&f, 0xff0000), ==, W * H);
    frag_offset(&f, SGI_GL_OP_POLYGON_OFFSET, 0, -1);
    frag_quad_at(&f, 0, 1, 0, 1, 0);
    g_assert_cmpuint(frag_count(&f, 0x00ff00), ==, W * H);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_POLYGON_OFFSET_UNITS, 0), ==,
                      -1);

    /* A line at that depth is not offset: GL_LESS fails. */
    word[0] = SGI_GL_LINES;
    frag_command(&f, SGI_GL_OP_BEGIN, word, 1);
    word[0] = bits(-1), word[1] = bits(0.5f / 32);
    frag_command(&f, SGI_GL_OP_VERTEX2F, word, 2);
    word[0] = bits(1);
    frag_command(&f, SGI_GL_OP_VERTEX2F, word, 2);
    frag_command(&f, SGI_GL_OP_END, word, 0);
    g_assert_cmpuint(frag_count(&f, 0x00ff00), ==, W * H);

    /* EXT's bias of -1/1024 in window depth; it zeroes the units. */
    frag_offset(&f, SGI_GL_OP_POLYGON_OFFSET_EXT, 0, -1.0f / 1024);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_POLYGON_OFFSET_UNITS, 0), ==, 0);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_POLYGON_OFFSET_BIAS, 0), ==,
                      -1.0f / 1024);
    frag_quad_at(&f, 0, 0, 1, 1, 0);
    g_assert_cmpuint(frag_count(&f, 0x0000ff), ==, W * H);

    /* The slope: a sloped quad over itself, factor -0.5 then +0.5. */
    frag_offset(&f, SGI_GL_OP_POLYGON_OFFSET, 0, 0);
    frag_clear(&f, SGI_GL_DEPTH_BUFFER_BIT);
    frag_slope(&f, 1, 0, 0, 0.25f, 0.75f);
    frag_offset(&f, SGI_GL_OP_POLYGON_OFFSET, 0.5f, 0);
    frag_slope(&f, 0, 1, 0, 0.25f, 0.75f);
    g_assert_cmpuint(frag_count(&f, 0xff0000), ==, W * H);
    frag_offset(&f, SGI_GL_OP_POLYGON_OFFSET, -0.5f, 0);
    frag_slope(&f, 0, 1, 0, 0.25f, 0.75f);
    g_assert_cmpuint(frag_count(&f, 0x00ff00), ==, W * H);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_POLYGON_OFFSET_FACTOR, 0), ==,
                      -0.5f);

    /* Clamped: at the near plane an offset nearer still is 0, not less. */
    frag_offset(&f, SGI_GL_OP_POLYGON_OFFSET, 0, 0);
    frag_quad_at(&f, 1, 0, 0, 1, -1);
    frag_offset(&f, SGI_GL_OP_POLYGON_OFFSET_EXT, 0, -0.5f);
    word[0] = 0x200 + SGI_GL_LEQUAL;
    frag_command(&f, SGI_GL_OP_DEPTH_FUNC, word, 1);
    frag_quad_at(&f, 0, 1, 0, 1, -1);
    g_assert_cmpuint(frag_count(&f, 0x00ff00), ==, W * H);
    g_assert_cmphex(f.p.depth[planes_index(5, 5)], ==, 0);
}

static void frag_clip_plane(FragScene *f, uint32_t plane, float a, float b,
                            float c, float d)
{
    uint32_t word[5] = { plane, bits(a), bits(b), bits(c), bits(d) };

    frag_command(f, SGI_GL_OP_CLIP_PLANE, word, 5);
}

/*
 * Client clip planes (OpenGL 1.2.1, 2.11): a plane is transformed by the
 * inverse of the modelview current when it is given, so (1, 0, 0, 0)
 * given under a translation of 0.5 in x keeps x >= 0.5 in eye
 * coordinates, window columns 48 and up under identity matrices, while
 * GL_CLIP_PLANE2 is enabled. A quad, a line and a point are clipped, and
 * a raster position outside is invalid. Planes start disabled and zero;
 * glGetClipPlane answers the eye-coordinate plane, and a plane number
 * past the last changes nothing.
 */
static void test_clip_planes(void)
{
    static FragScene f;
    uint32_t out[SGI_GL_GET_WORDS], word[3];
    float p;

    frag_init(&f);
    for (int i = 0; i < SGI_GL_CLIP_PLANES; i++) {
        g_assert_cmpuint(get(&f.c, SGI_GL_CLIP_PLANE0 + i, out), ==, 1);
        g_assert_cmphex(out[0], ==, 0);
        g_assert_true(sgi_gl_get_clip_plane(&f.c, i, out));
        g_assert_cmphex(out[0] | out[1] | out[2] | out[3], ==, 0);
    }
    g_assert_false(sgi_gl_get_clip_plane(&f.c, SGI_GL_CLIP_PLANES, out));

    word[0] = bits(0.5f), word[1] = 0, word[2] = 0;
    frag_command(&f, SGI_GL_OP_TRANSLATE, word, 3);
    frag_clip_plane(&f, 2, 1, 0, 0, 0);
    /* Plane 3 would keep nothing, but stays disabled. */
    frag_clip_plane(&f, 3, 0, 0, 0, -1);
    frag_clip_plane(&f, SGI_GL_CLIP_PLANES, 0, 1, 0, 0);
    frag_command(&f, SGI_GL_OP_LOAD_IDENTITY, word, 0);
    g_assert_true(sgi_gl_get_clip_plane(&f.c, 2, out));
    memcpy(&p, &out[3], sizeof(p));
    g_assert_cmpfloat(p, ==, -0.5f);
    memcpy(&p, &out[0], sizeof(p));
    g_assert_cmpfloat(p, ==, 1);

    frag_quad(&f, 1, 1, 1, 1);
    g_assert_cmpuint(frag_count(&f, 0xffffff), ==, W * H);
    memset(&f.p, 0, sizeof(f.p));
    enable(&f.c, SGI_GL_CLIP_PLANE0 + 2, true);
    g_assert_cmpuint(get(&f.c, SGI_GL_CLIP_PLANE0 + 2, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    frag_quad(&f, 1, 1, 1, 1);
    g_assert_cmpuint(frag_count(&f, 0xffffff), ==, 16 * H);
    g_assert_cmphex(frag_pixel(&f, 48, 0), ==, 0xffffff);
    g_assert_cmphex(frag_pixel(&f, 47, 0), ==, 0);

    /* A line along row 10 and points either side of x 0.5. */
    memset(&f.p, 0, sizeof(f.p));
    word[0] = SGI_GL_LINES;
    frag_command(&f, SGI_GL_OP_BEGIN, word, 1);
    word[0] = bits(-1), word[1] = bits(10.5f / 32 - 1);
    frag_command(&f, SGI_GL_OP_VERTEX2F, word, 2);
    word[0] = bits(1);
    frag_command(&f, SGI_GL_OP_VERTEX2F, word, 2);
    frag_command(&f, SGI_GL_OP_END, word, 0);
    /* The negative endpoint perturbation excludes the final diamond at 63. */
    g_assert_cmpuint(frag_count(&f, 0xffffff), ==, 15);
    g_assert_cmphex(frag_pixel(&f, 63, 10), ==, 0);
    g_assert_cmphex(frag_pixel(&f, 48, 10), ==, 0xffffff);
    word[0] = SGI_GL_POINTS;
    frag_command(&f, SGI_GL_OP_BEGIN, word, 1);
    word[0] = bits(0.25f), word[1] = bits(0.5f);
    frag_command(&f, SGI_GL_OP_VERTEX2F, word, 2);
    word[0] = bits(0.75f);
    frag_command(&f, SGI_GL_OP_VERTEX2F, word, 2);
    frag_command(&f, SGI_GL_OP_END, word, 0);
    g_assert_cmpuint(frag_count(&f, 0xffffff), ==, 16);
    g_assert_cmphex(frag_pixel(&f, 56, 48), ==, 0xffffff);

    word[0] = bits(0.25f), word[1] = bits(0);
    frag_command(&f, SGI_GL_OP_RASTER_POS2F, word, 2);
    g_assert_cmpuint(get(&f.c, SGI_GL_CURRENT_RASTER_POSITION_VALID, out),
                     ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    word[0] = bits(0.75f);
    frag_command(&f, SGI_GL_OP_RASTER_POS2F, word, 2);
    g_assert_cmpuint(get(&f.c, SGI_GL_CURRENT_RASTER_POSITION_VALID, out),
                     ==, 1);
    g_assert_cmphex(out[0], ==, 1);

    enable(&f.c, SGI_GL_CLIP_PLANE0 + 2, false);
    frag_quad(&f, 1, 1, 1, 1);
    g_assert_cmpuint(frag_count(&f, 0xffffff), ==, W * H);
}

/* Public OpenGL 1.2.1 aliased point and line footprints in window pixels. */
static void wide_vertex(FragScene *f, float x, float y)
{
    frag_command(f, SGI_GL_OP_VERTEX2F,
                 (uint32_t[]) { bits(x / 32 - 1), bits(y / 32 - 1) }, 2);
}

static void test_wide_coverage(void)
{
    static const struct {
        unsigned mode;
        float size;
        float ax, ay, bx, by;
        int x0, y0, x1, y1;
    } cases[] = {
        { SGI_GL_POINTS, 2, 4.25, 3.75, 0, 0, 3, 3, 4, 4 },
        { SGI_GL_POINTS, 0.25, 4.25, 3.75, 0, 0, 4, 3, 4, 3 },
        { SGI_GL_POINTS, 1.5, 4.25, 3.75, 0, 0, 3, 3, 4, 4 },
        { SGI_GL_POINTS, 5, 4.25, 3.75, 0, 0, 2, 1, 6, 5 },
        { SGI_GL_LINES, 2, 1.5, 3.5, 5.5, 3.5, 1, 2, 4, 3 },
        { SGI_GL_LINES, 3, 1.5, 3.5, 5.5, 3.5, 1, 2, 4, 4 },
        { SGI_GL_LINES, 2, 3.5, 1.5, 3.5, 5.5, 2, 1, 3, 4 },
        { SGI_GL_LINES, 3, 3.5, 1.5, 3.5, 5.5, 2, 1, 4, 4 },
        /* On a shifted diamond tip epsilon dominates epsilon squared. */
        { SGI_GL_LINES, 2, 5.5, 3.5, 1.5, 3.5, 1, 2, 4, 3 },
        { SGI_GL_LINES, 1, 1, 3.5, 5, 3.5, 0, 3, 3, 3 },
        { SGI_GL_LINES, 1, 5, 3.5, 1, 3.5, 1, 3, 4, 3 },
        { SGI_GL_LINES, 3, 3.5, 5.5, 3.5, 1.5, 2, 2, 4, 5 },
        { SGI_GL_POINTS, 5, 0, 0, 0, 0, 0, 0, 2, 2 },
        { SGI_GL_LINES, 3, -3, 3.5, 5.5, 3.5, 0, 2, 4, 4 },
    };
    FragScene f;

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        SGIGLOp setter = SGI_GL_OP_LINE_WIDTH;

        frag_init(&f);
        if (cases[i].mode == SGI_GL_POINTS) {
            setter = SGI_GL_OP_POINT_SIZE;
        }
        frag_command(&f, setter, (uint32_t[]) { bits(cases[i].size) }, 1);
        frag_command(&f, SGI_GL_OP_BEGIN,
                     (uint32_t[]) { cases[i].mode }, 1);
        wide_vertex(&f, cases[i].ax, cases[i].ay);
        if (cases[i].mode == SGI_GL_LINES) {
            wide_vertex(&f, cases[i].bx, cases[i].by);
        }
        frag_command(&f, SGI_GL_OP_END, NULL, 0);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                bool inside = x >= cases[i].x0 && x <= cases[i].x1 &&
                              y >= cases[i].y0 && y <= cases[i].y1;
                g_assert_cmphex(frag_pixel(&f, x, y), ==,
                                inside ? 0xffffff : 0);
            }
        }
    }
}

/* Directed diagonal ties are x-major, including slopes exactly +/- one. */
static void test_wide_diagonal(void)
{
    FragScene f;

    for (int width = 2; width <= 3; width++) {
        for (int direction = -1; direction <= 1; direction += 2) {
            frag_init(&f);
            f.c.line_width = width;
            frag_command(&f, SGI_GL_OP_BEGIN,
                         (uint32_t[]) { SGI_GL_LINES }, 1);
            wide_vertex(&f, 1.5, direction > 0 ? 1.5 : 5.5);
            wide_vertex(&f, 5.5, direction > 0 ? 5.5 : 1.5);
            frag_command(&f, SGI_GL_OP_END, NULL, 0);
            for (int y = 0; y < H; y++) {
                for (int x = 0; x < W; x++) {
                    int base = direction > 0 ? x - (width == 3) : 5 - x;
                    bool covered = x >= 1 && x <= 4 &&
                                   y >= base && y < base + width;

                    g_assert_cmphex(frag_pixel(&f, x, y), ==,
                                    covered ? 0xffffff : 0);
                }
            }
        }
    }
    /* Stencil increments expose double ownership at a connected endpoint. */
    frag_init(&f);
    f.c.line_width = 2;
    enable(&f.c, SGI_GL_STENCIL_TEST, true);
    frag_command(&f, SGI_GL_OP_STENCIL_OP,
                 (uint32_t[]) { SGI_GL_KEEP, SGI_GL_KEEP, SGI_GL_INCR }, 3);
    frag_command(&f, SGI_GL_OP_BEGIN,
                 (uint32_t[]) { SGI_GL_LINE_STRIP }, 1);
    wide_vertex(&f, 1.5, 3.5);
    wide_vertex(&f, 3.5, 3.5);
    wide_vertex(&f, 5.5, 3.5);
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    g_assert_cmpuint(frag_count(&f, 0xffffff), ==, 8);
    for (int x = 1; x <= 4; x++) {
        g_assert_cmpuint(f.p.stencil[planes_index(x, 2)], ==, 1);
        g_assert_cmpuint(f.p.stencil[planes_index(x, 3)], ==, 1);
    }
}

/* Integer endpoint perturbations own the neighboring column exactly once. */
static void test_line_integer_endpoints(void)
{
    FragScene f;

    frag_init(&f);
    enable(&f.c, SGI_GL_STENCIL_TEST, true);
    frag_command(&f, SGI_GL_OP_STENCIL_OP,
                 (uint32_t[]) { SGI_GL_KEEP, SGI_GL_KEEP, SGI_GL_INCR }, 3);
    frag_command(&f, SGI_GL_OP_BEGIN,
                 (uint32_t[]) { SGI_GL_LINE_STRIP }, 1);
    wide_vertex(&f, 1, 3.5);
    wide_vertex(&f, 3, 3.5);
    wide_vertex(&f, 5, 3.5);
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    g_assert_cmpuint(frag_count(&f, 0xffffff), ==, 4);
    for (int x = 0; x <= 3; x++) {
        g_assert_cmpuint(f.p.stencil[planes_index(x, 3)], ==, 1);
    }
}

static FragScene wide_state_scene;

static void wide_invalid_setters(void)
{
    FragScene *f = &wide_state_scene;

    frag_command(f, SGI_GL_OP_BEGIN, (uint32_t[]) { SGI_GL_POINTS }, 1);
    frag_command(f, SGI_GL_OP_POINT_SIZE, (uint32_t[]) { bits(2) }, 1);
    frag_command(f, SGI_GL_OP_LINE_WIDTH, (uint32_t[]) { bits(2) }, 1);
    wide_vertex(f, 4.25, 3.75);
    frag_command(f, SGI_GL_OP_END, NULL, 0);
}

static void test_wide_state(void)
{
    FragScene *f = &wide_state_scene;
    g_autofree char *log = NULL;

    frag_init(f);
    f->c.point_size = 5;
    f->c.line_width = 3;
    log = logged_by(wide_invalid_setters);
    g_assert_nonnull(strstr(log, "invalid operation"));
    g_assert_cmpfloat(get_float(&f->c, SGI_GL_POINT_SIZE, 0), ==, 5);
    g_assert_cmpfloat(get_float(&f->c, SGI_GL_LINE_WIDTH, 0), ==, 3);
    g_assert_cmpuint(frag_count(f, 0xffffff), ==, 25);
    for (unsigned i = 0; i < 3; i++) {
        float invalid[] = { NAN, INFINITY, -1 };

        frag_command(f, SGI_GL_OP_POINT_SIZE,
                     (uint32_t[]) { bits(invalid[i]) }, 1);
        frag_command(f, SGI_GL_OP_LINE_WIDTH,
                     (uint32_t[]) { bits(invalid[i]) }, 1);
    }
    g_assert_cmpfloat(get_float(&f->c, SGI_GL_POINT_SIZE, 0), ==, 5);
    g_assert_cmpfloat(get_float(&f->c, SGI_GL_LINE_WIDTH, 0), ==, 3);
}

/* Direct submissions test the compatibility sentinel and arithmetic domain. */
static void test_wide_dimensions(void)
{
    const float sizes[] = { 0, 2147483520.0f, 2147483648.0f,
                            FLT_MAX, INFINITY, NAN, -1 };
    FragScene f;
    float v[2][SGI_GL_VERTEX_FLOATS] = { 0 };

    for (unsigned mode = SGI_GL_POINTS; mode <= SGI_GL_LINES; mode++) {
        for (unsigned i = 0; i < ARRAY_SIZE(sizes); i++) {
            SGIGLDraw d = {
                .mode = mode, .count = mode == SGI_GL_POINTS ? 1 : 2,
                .vertices = &v[0][0], .state = &f.c.cur,
                .point_size = sizes[i], .line_width = sizes[i],
            };

            frag_init(&f);
            for (unsigned j = 0; j < 2; j++) {
                v[j][SGI_GL_V_POSITION] = (j ? 5.5f : 1.5f) / 32 - 1;
                v[j][SGI_GL_V_POSITION + 1] = 3.5f / 32 - 1;
                v[j][SGI_GL_V_POSITION + 3] = 1;
                v[j][SGI_GL_V_TEXCOORD + 3] = 1;
                for (unsigned k = 0; k < 4; k++) {
                    v[j][SGI_GL_V_COLOR + k] = 1;
                }
            }
            sgi_soft_draw(&rules_stencil, &f.rec.target, &d);
            g_assert_cmpuint(frag_count(&f, 0xffffff), ==,
                            i == 0 ? (mode == SGI_GL_POINTS ? 1 : 4) :
                            i == 1 ?
                            (mode == SGI_GL_POINTS ? W * H : 4 * H) : 0);
        }
    }
}

/*
 * The rasterization state kept and answered (OpenGL 1.2.1, 3.3 to 3.5):
 * point size and line width 1, stipple repeat 1 and
 * pattern 0xffff, both polygon modes GL_FILL and the smoothing and
 * stipple enables off at first. A size or width of 0 and a bad face or
 * mode change nothing; the repeat clamps to [1, 256]; drawing is as the
 * initial state draws.
 */
static void test_polygon_stipple_state(void)
{
    SGIGLContext a, b;
    uint32_t rows[32] = { 0 }, out[32];

    sgi_gl_init(&a, 1);
    sgi_gl_init(&b, 2);
    g_assert_true(sgi_gl_get_polygon_stipple(&a, out));
    for (unsigned i = 0; i < 32; i++) {
        g_assert_cmphex(out[i], ==, UINT32_MAX);
        rows[i] = 1u << i;
    }
    g_assert_true(sgi_gl_polygon_stipple(&a, rows));
    rows[0] = 0;
    g_assert_true(sgi_gl_get_polygon_stipple(&a, out));
    g_assert_cmphex(out[0], ==, 1);
    g_assert_true(sgi_gl_get_polygon_stipple(&b, out));
    g_assert_cmphex(out[0], ==, UINT32_MAX);
    a.in_begin = true;
    g_assert_false(sgi_gl_polygon_stipple(&a, rows));
    memset(out, 0x5a, sizeof(out));
    g_assert_false(sgi_gl_get_polygon_stipple(&a, out));
    g_assert_cmphex(out[0], ==, 0x5a5a5a5a);
    a.in_begin = false;
    g_assert_true(sgi_gl_get_polygon_stipple(&a, out));
    g_assert_cmphex(out[0], ==, 1);
}

/* Absolute window phase survives fan splits and drawable translation. */
static void test_polygon_stipple_phase(void)
{
    FragScene f;
    uint32_t rows[32] = { 0 };

    rows[0] = 1;
    rows[1] = 1u << 31;
    rows[31] = (1u << 31) | 1;
    for (unsigned shifted = 0; shifted < 2; shifted++) {
        int shift = shifted ? 16 : 0;

        frag_init(&f);
        f.c.cur.viewport[0] = f.c.cur.viewport[1] = -shift;
        f.c.cur.drawable.origin[0] = f.c.cur.drawable.origin[1] = shift;
        g_assert_true(sgi_gl_polygon_stipple(&f.c, rows));
        enable(&f.c, SGI_GL_POLYGON_STIPPLE, true);
        frag_quad(&f, 1, 0, 0, 1);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                unsigned wx = (unsigned)(x - shift) & 31;
                unsigned wy = (unsigned)(y - shift) & 31;

                g_assert_cmphex(frag_pixel(&f, x, y), ==,
                               (rows[wy] & (1u << wx)) ? 0xff0000 : 0);
            }
        }
        memset(&f.p, 0, sizeof(f.p));
        enable(&f.c, SGI_GL_POLYGON_STIPPLE, false);
        frag_quad(&f, 1, 0, 0, 1);
        g_assert_cmpuint(frag_count(&f, 0xff0000), ==, W * H);
        for (unsigned i = 0; i < 32; i++) {
            rows[i] = UINT32_MAX;
        }
        g_assert_true(sgi_gl_polygon_stipple(&f.c, rows));
        enable(&f.c, SGI_GL_POLYGON_STIPPLE, true);
        frag_quad(&f, 0, 1, 0, 1);
        g_assert_cmpuint(frag_count(&f, 0x00ff00), ==, W * H);
        rows[0] = 1;
        rows[1] = 1u << 31;
        memset(&rows[2], 0, 29 * sizeof(rows[0]));
        rows[31] = (1u << 31) | 1;
    }
}

static void test_polygon_stipple_fragment_ops(void)
{
    FragScene f;
    uint32_t rows[32] = { 0 };
    Planes before;

    for (unsigned index = 0; index < 2; index++) {
        for (unsigned targets = 0; targets < 2; targets++) {
            frag_init(&f);
            memset(&f.p, 0x55, sizeof(f.p));
            before = f.p;
            f.c.cur.index_bits = index ? 8 : 0;
            f.c.cur.index_mask = 3;
            f.c.cur.colour_mask = SGI_GL_COLOUR_MASK_RED;
            f.c.cur.blend = true;
            f.c.cur.blend_src = f.c.cur.blend_dst = 1;
            f.c.cur.draw_buffers = targets ? SGI_GL_DRAW_FRONT : 0;
            f.c.cur.depth_test = f.c.cur.stencil_test = true;
            f.c.cur.depth_func = SGI_GL_ALWAYS;
            f.c.cur.stencil_op[2] = SGI_GL_INCR;
            g_assert_true(sgi_gl_polygon_stipple(&f.c, rows));
            enable(&f.c, SGI_GL_POLYGON_STIPPLE, true);
            frag_quad(&f, 1, 0, 0, 1);
            g_assert_cmpmem(&f.p, sizeof(f.p), &before, sizeof(before));
            rows[0] = 1;
            g_assert_true(sgi_gl_polygon_stipple(&f.c, rows));
            frag_quad(&f, 1, 0, 0, 1);
            g_assert_cmpuint(f.p.depth[0], !=, before.depth[0]);
            g_assert_cmpuint(f.p.stencil[0], ==, 0x56);
            g_assert_cmpuint(f.p.depth[1], ==, before.depth[1]);
            if (!targets) {
                g_assert_cmpmem(f.p.colour, sizeof(f.p.colour),
                                before.colour, sizeof(before.colour));
            }
            rows[0] = 0;
        }
    }
    frag_init(&f);
    g_assert_true(sgi_gl_polygon_stipple(&f.c, rows));
    enable(&f.c, SGI_GL_POLYGON_STIPPLE, true);
    frag_command(&f, SGI_GL_OP_BEGIN, (uint32_t[]) { SGI_GL_POINTS }, 1);
    wide_vertex(&f, 1.5, 1.5);
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    g_assert_cmphex(frag_pixel(&f, 1, 1), ==, 0xffffff);
    frag_command(&f, SGI_GL_OP_BEGIN, (uint32_t[]) { SGI_GL_LINES }, 1);
    wide_vertex(&f, 2.5, 2.5);
    wide_vertex(&f, 6.5, 2.5);
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    g_assert_cmphex(frag_pixel(&f, 3, 2), ==, 0xffffff);
    {
        SGIGLBitmap bitmap;
        uint8_t byte = 0x80;

        frag_command(&f, SGI_GL_OP_RASTER_POS2F,
                     (uint32_t[]) { bits(8.5f / 32 - 1),
                                    bits(8.5f / 32 - 1) }, 2);
        g_assert_true(sgi_gl_bitmap_place(&f.c, 1, 1, 0, 0, 0, 0, &bitmap));
        sgi_gl_bitmap_bytes(&f.c, &f.sink, &bitmap, 0, &byte, 1);
        g_assert_cmphex(frag_pixel(&f, 8, 8), ==, 0xffffff);
    }
    f.c.clear_color[0] = 1;
    frag_clear(&f, SGI_GL_COLOR_BUFFER_BIT);
    g_assert_cmpuint(frag_count(&f, 0xff0000), ==, W * H);
}

static void test_polygon_state_begin_guard(void)
{
    FragScene f;

    frag_init(&f);
    frag_command(&f, SGI_GL_OP_BEGIN, (uint32_t[]) { SGI_GL_TRIANGLES }, 1);
    frag_command(&f, SGI_GL_OP_POLYGON_MODE,
                 (uint32_t[]) { SGI_GL_FRONT_AND_BACK, SGI_GL_LINE }, 2);
    enable(&f.c, SGI_GL_POLYGON_STIPPLE, true);
    g_assert_cmphex(f.c.polygon_mode[0], ==, SGI_GL_FILL);
    g_assert_cmphex(f.c.polygon_mode[1], ==, SGI_GL_FILL);
    g_assert_cmpuint(f.c.raster_enables & SGI_GL_RASTER_POLYGON_STIPPLE, ==, 0);
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    enable(&f.c, SGI_GL_POLYGON_STIPPLE, true);
    frag_command(&f, SGI_GL_OP_BEGIN, (uint32_t[]) { SGI_GL_TRIANGLES }, 1);
    enable(&f.c, SGI_GL_POLYGON_STIPPLE, false);
    g_assert_cmpuint(f.c.raster_enables & SGI_GL_RASTER_POLYGON_STIPPLE, !=, 0);
    {
        uint32_t out = 0x5a5a5a5a;
        unsigned n = 0;

        g_assert_false(sgi_gl_get(&f.c, SGI_GL_POLYGON_STIPPLE, &out, &n));
        g_assert_cmphex(out, ==, 0x5a5a5a5a);
    }
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
}

static void test_raster_state_kept(void)
{
    static const uint32_t caps[5] = {
        SGI_GL_POINT_SMOOTH, SGI_GL_LINE_SMOOTH, SGI_GL_LINE_STIPPLE,
        SGI_GL_POLYGON_SMOOTH, SGI_GL_POLYGON_STIPPLE,
    };
    static FragScene f;
    uint32_t out[SGI_GL_GET_WORDS], word[2];

    frag_init(&f);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_POINT_SIZE, 0), ==, 1);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_LINE_WIDTH, 0), ==, 1);
    g_assert_cmpuint(get(&f.c, SGI_GL_LINE_STIPPLE_REPEAT, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    g_assert_cmpuint(get(&f.c, SGI_GL_LINE_STIPPLE_PATTERN, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0xffff);
    g_assert_cmpuint(get(&f.c, SGI_GL_POLYGON_MODE, out), ==, 2);
    g_assert_cmphex(out[0], ==, SGI_GL_FILL);
    g_assert_cmphex(out[1], ==, SGI_GL_FILL);

    word[0] = bits(4);
    frag_command(&f, SGI_GL_OP_POINT_SIZE, word, 1);
    word[0] = bits(0);
    frag_command(&f, SGI_GL_OP_POINT_SIZE, word, 1);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_POINT_SIZE, 0), ==, 4);
    word[0] = bits(2.5f);
    frag_command(&f, SGI_GL_OP_LINE_WIDTH, word, 1);
    word[0] = bits(-1);
    frag_command(&f, SGI_GL_OP_LINE_WIDTH, word, 1);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_LINE_WIDTH, 0), ==, 2.5f);
    word[0] = 300, word[1] = 0x1f0f0f;
    frag_command(&f, SGI_GL_OP_LINE_STIPPLE, word, 2);
    g_assert_cmpuint(get(&f.c, SGI_GL_LINE_STIPPLE_REPEAT, out), ==, 1);
    g_assert_cmphex(out[0], ==, 256);
    g_assert_cmpuint(get(&f.c, SGI_GL_LINE_STIPPLE_PATTERN, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0x0f0f);
    word[0] = -3;
    frag_command(&f, SGI_GL_OP_LINE_STIPPLE, word, 2);
    g_assert_cmpuint(get(&f.c, SGI_GL_LINE_STIPPLE_REPEAT, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
    word[0] = SGI_GL_BACK, word[1] = SGI_GL_LINE;
    frag_command(&f, SGI_GL_OP_POLYGON_MODE, word, 2);
    word[0] = SGI_GL_FRONT, word[1] = SGI_GL_FILL + 1;
    frag_command(&f, SGI_GL_OP_POLYGON_MODE, word, 2);
    word[0] = 0x406, word[1] = SGI_GL_POINT;
    frag_command(&f, SGI_GL_OP_POLYGON_MODE, word, 2);
    g_assert_cmpuint(get(&f.c, SGI_GL_POLYGON_MODE, out), ==, 2);
    g_assert_cmphex(out[0], ==, SGI_GL_FILL);
    g_assert_cmphex(out[1], ==, SGI_GL_LINE);
    word[0] = SGI_GL_FRONT_AND_BACK, word[1] = SGI_GL_POINT;
    frag_command(&f, SGI_GL_OP_POLYGON_MODE, word, 2);
    g_assert_cmpuint(get(&f.c, SGI_GL_POLYGON_MODE, out), ==, 2);
    g_assert_cmphex(out[0], ==, SGI_GL_POINT);
    g_assert_cmphex(out[1], ==, SGI_GL_POINT);

    for (int i = 0; i < ARRAY_SIZE(caps); i++) {
        g_assert_cmpuint(get(&f.c, caps[i], out), ==, 1);
        g_assert_cmphex(out[0], ==, 0);
        enable(&f.c, caps[i], true);
        for (int j = 0; j < ARRAY_SIZE(caps); j++) {
            g_assert_cmpuint(get(&f.c, caps[j], out), ==, 1);
            g_assert_cmphex(out[0], ==, j <= i);
        }
    }
    frag_quad(&f, 1, 1, 1, 1);
    g_assert_cmpuint(frag_count(&f, 0xffffff), ==, W * H);
    enable(&f.c, SGI_GL_LINE_STIPPLE, false);
    g_assert_cmpuint(get(&f.c, SGI_GL_LINE_STIPPLE, out), ==, 1);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmpuint(get(&f.c, SGI_GL_POLYGON_STIPPLE, out), ==, 1);
    g_assert_cmphex(out[0], ==, 1);
}

/*
 * Clip coordinates are compared to within 2^-40 and attributes, which are
 * floats, to within 2^-20.
 */
#define CLIP_EPSILON (1.0 / (1ull << 40))
#define ATTR_EPSILON (1.0 / (1 << 20))

/*
 * A vertex at clip coordinates (x, y, z, w), the same in eye coordinates,
 * with colour (r, g, b, 1), texture coordinate s and fog coordinate f.
 */
static SGIVolumeVertex volume_vertex(double x, double y, double z, double w,
                                     float r, float g, float b, float s,
                                     float f)
{
    SGIVolumeVertex v = { .clip = { x, y, z, w }, .eye = { x, y, z, w } };

    v.attr[SGI_GL_V_POSITION + 3] = 1;
    v.attr[SGI_GL_V_COLOR] = r;
    v.attr[SGI_GL_V_COLOR + 1] = g;
    v.attr[SGI_GL_V_COLOR + 2] = b;
    v.attr[SGI_GL_V_COLOR + 3] = 1;
    v.attr[SGI_GL_V_TEXCOORD] = s;
    v.attr[SGI_GL_V_TEXCOORD + 3] = 1;
    v.attr[SGI_GL_V_FOG] = f;
    return v;
}

/* v is at clip (x, y, z, w) with colour (r, g, b), s and f as above. */
static void assert_volume_vertex(const SGIVolumeVertex *v, double x,
                                 double y, double z, double w, float r,
                                 float g, float b, float s, float f)
{
    const double c[4] = { x, y, z, w };
    const float rgba[4] = { r, g, b, 1 };

    for (int k = 0; k < 4; k++) {
        g_assert_cmpfloat_with_epsilon(v->clip[k], c[k], CLIP_EPSILON);
        g_assert_cmpfloat_with_epsilon(v->eye[k], c[k], CLIP_EPSILON);
    }
    for (int k = 0; k < 4; k++) {
        g_assert_cmpfloat_with_epsilon(v->attr[SGI_GL_V_COLOR + k], rgba[k],
                                       ATTR_EPSILON);
    }
    g_assert_cmpfloat_with_epsilon(v->attr[SGI_GL_V_TEXCOORD], s, ATTR_EPSILON);
    g_assert_cmpfloat_with_epsilon(v->attr[SGI_GL_V_FOG], f, ATTR_EPSILON);
}

/*
 * The index in poly, of n vertices, of the vertex at clip (x, y, z, w),
 * which must be there once.
 */
static unsigned volume_find(const SGIVolumeVertex *poly, unsigned n,
                            double x, double y, double z, double w)
{
    unsigned found = n;

    for (unsigned i = 0; i < n; i++) {
        if (fabs(poly[i].clip[0] - x) < CLIP_EPSILON &&
            fabs(poly[i].clip[1] - y) < CLIP_EPSILON &&
            fabs(poly[i].clip[2] - z) < CLIP_EPSILON &&
            fabs(poly[i].clip[3] - w) < CLIP_EPSILON) {
            g_assert_cmpuint(found, ==, n);
            found = i;
        }
    }
    g_assert_cmpuint(found, <, n);
    return found;
}

/*
 * View-volume clipping of a triangle (OpenGL 1.2.1, 2.11) at w 1, where
 * clip and normalized device coordinates agree. Red (-1/2, -1/2, -3),
 * green (1/2, -1/2, 3) and blue (0, 1/2, 0) cross the near plane z = -w
 * and the far plane z = w. By hand: edge red-green crosses the near
 * plane a third of the way along, at (-1/6, -1/2, -1), and the far plane
 * two thirds along, at (1/6, -1/2, 1); green-blue crosses the far plane
 * two thirds along, at (1/6, 1/6, 1); blue-red crosses the near plane a
 * third along, at (-1/6, 1/6, -1). Each new vertex's colour, texture
 * coordinate s (0, 1 and 1/2) and fog coordinate (0, 6 and 3) are
 * interpolated at the same t (2.13.8). The pentagon keeps the triangle's
 * winding.
 */
static void test_volume_clip_triangle_near_and_far(void)
{
    const SGIVolumeVertex in[3] = {
        volume_vertex(-0.5, -0.5, -3, 1, 1, 0, 0, 0, 0),
        volume_vertex(0.5, -0.5, 3, 1, 0, 1, 0, 1, 6),
        volume_vertex(0, 0.5, 0, 1, 0, 0, 1, 0.5, 3),
    };
    const SGIVolumeVertex inside[3] = {
        volume_vertex(-1, -1, -1, 1, 1, 0, 0, 0, 0),
        volume_vertex(2, -2, 2, 2, 0, 1, 0, 1, 6),
        volume_vertex(0, 0.5, 0.25, 1, 0, 0, 1, 0.5, 3),
    };
    SGIVolumeVertex out[SGI_VOLUME_TRIANGLE_MAX];
    unsigned n = sgi_volume_clip_triangle(in, out), a;

    g_assert_cmpuint(n, ==, 5);
    a = volume_find(out, n, -1.0 / 6, -0.5, -1, 1);
    assert_volume_vertex(&out[a], -1.0 / 6, -0.5, -1, 1,
                         2.0 / 3, 1.0 / 3, 0, 1.0 / 3, 2);
    assert_volume_vertex(&out[(a + 1) % n], 1.0 / 6, -0.5, 1, 1,
                         1.0 / 3, 2.0 / 3, 0, 2.0 / 3, 4);
    assert_volume_vertex(&out[(a + 2) % n], 1.0 / 6, 1.0 / 6, 1, 1,
                         0, 1.0 / 3, 2.0 / 3, 2.0 / 3, 4);
    assert_volume_vertex(&out[(a + 3) % n], 0, 0.5, 0, 1,
                         0, 0, 1, 0.5, 3);
    assert_volume_vertex(&out[(a + 4) % n], -1.0 / 6, 1.0 / 6, -1, 1,
                         1.0 / 3, 0, 2.0 / 3, 1.0 / 3, 2);

    /* Inside every plane, even on them: unchanged, bit for bit. */
    g_assert_cmpuint(sgi_volume_clip_triangle(inside, out), ==, 3);
    g_assert_cmpmem(out, 3 * sizeof(out[0]), inside, sizeof(inside));
}

/*
 * A segment crossing the side planes is clipped in clip coordinates, not
 * after the divide: from (-2, 0, 0, 1), red 0, to (4, 0, 0, 2), red 1,
 * w + x is -1 and 6, so the segment enters x >= -w at t = 1/7, at
 * (-8/7, 0, 0, 8/7), and w - x is 3 and -2, so it leaves x <= w at
 * t = 3/5, at (8/5, 0, 0, 8/5); red is 1/7 and 3/5 there. A vertical
 * segment from y = -3 to 3 at w 1 keeps -1 to 1. A segment beyond one
 * plane at both ends is dropped, and so is one that crosses two planes
 * but never lies inside both.
 */
static void test_volume_clip_segment_side_planes(void)
{
    SGIVolumeVertex a = volume_vertex(-2, 0, 0, 1, 0, 0, 0, 0, 0);
    SGIVolumeVertex b = volume_vertex(4, 0, 0, 2, 1, 0, 0, 1, 7);

    g_assert_true(sgi_volume_clip_segment(&a, &b));
    assert_volume_vertex(&a, -8.0 / 7, 0, 0, 8.0 / 7, 1.0 / 7, 0, 0,
                         1.0 / 7, 1);
    assert_volume_vertex(&b, 8.0 / 5, 0, 0, 8.0 / 5, 3.0 / 5, 0, 0,
                         3.0 / 5, 21.0 / 5);

    a = volume_vertex(0.25, -3, 0, 1, 0, 1, 0, 0, 0);
    b = volume_vertex(0.25, 3, 0, 1, 0, 0, 1, 1, 0);
    g_assert_true(sgi_volume_clip_segment(&a, &b));
    assert_volume_vertex(&a, 0.25, -1, 0, 1, 0, 2.0 / 3, 1.0 / 3,
                         1.0 / 3, 0);
    assert_volume_vertex(&b, 0.25, 1, 0, 1, 0, 1.0 / 3, 2.0 / 3,
                         2.0 / 3, 0);

    a = volume_vertex(1.5, 0, 0, 1, 0, 0, 0, 0, 0);
    b = volume_vertex(3, -0.5, 0.5, 2, 0, 0, 0, 0, 0);
    g_assert_false(sgi_volume_clip_segment(&a, &b));

    /*
     * One that passes outside a corner, from (-2, 0) to (1/4, 5/2) at
     * w 1, enters x >= -w only at t = 4/9 but has already left y <= w at
     * t = 2/5: no part lies inside both, and it is dropped, whichever end
     * comes first, not drawn reversed between the two cuts.
     */
    a = volume_vertex(-2, 0, 0, 1, 0, 0, 0, 0, 0);
    b = volume_vertex(0.25, 2.5, 0, 1, 0, 0, 0, 0, 0);
    g_assert_false(sgi_volume_clip_segment(&a, &b));
    g_assert_false(sgi_volume_clip_segment(&b, &a));
}

/*
 * Vertices at w <= 0. The view volume holds no point with w < 0
 * (2.11), so a triangle with one is cut where it leaves the volume, and
 * one with every vertex negated, which divides to the same window
 * points, keeps nothing. Red (-1/2, -1/2, 0, 1), green (1/2, -1/2, 0, 1)
 * and blue (0, 0, -3, -1): along red-blue w + z is 1 - 5t and every
 * other plane's value falls to 0 later, at t = 1/3 or 3/5, so the near
 * plane cuts it first at t = 1/5, at (-2/5, -2/5, -3/5, 3/5), coloured
 * (4/5, 0, 1/5); green-blue likewise at (2/5, -2/5, -3/5, 3/5), coloured
 * (0, 4/5, 1/5). A segment from (0, 0, 0, 1) to (0, 0, 2, -1) is cut by
 * the far plane, w - z = 1 - 4t, at t = 1/4: (0, 0, 1/2, 1/2).
 */
static void test_volume_clip_negative_w(void)
{
    SGIVolumeVertex in[3] = {
        volume_vertex(-0.5, -0.5, 0, 1, 1, 0, 0, 0, 0),
        volume_vertex(0.5, -0.5, 0, 1, 0, 1, 0, 1, 0),
        volume_vertex(0, 0, -3, -1, 0, 0, 1, 0, 5),
    };
    SGIVolumeVertex out[SGI_VOLUME_TRIANGLE_MAX], a, b;
    unsigned n = sgi_volume_clip_triangle(in, out), i;

    g_assert_cmpuint(n, ==, 4);
    i = volume_find(out, n, -0.5, -0.5, 0, 1);
    assert_volume_vertex(&out[i], -0.5, -0.5, 0, 1, 1, 0, 0, 0, 0);
    assert_volume_vertex(&out[(i + 1) % n], 0.5, -0.5, 0, 1,
                         0, 1, 0, 1, 0);
    assert_volume_vertex(&out[(i + 2) % n], 0.4, -0.4, -0.6, 0.6,
                         0, 0.8, 0.2, 0.8, 1);
    assert_volume_vertex(&out[(i + 3) % n], -0.4, -0.4, -0.6, 0.6,
                         0.8, 0, 0.2, 0, 1);

    for (i = 0; i < 3; i++) {
        for (int k = 0; k < 4; k++) {
            in[i].clip[k] = -in[i].clip[k];
        }
    }
    g_assert_cmpuint(sgi_volume_clip_triangle(in, out), ==, 0);

    a = volume_vertex(0, 0, 0, 1, 1, 0, 0, 0, 0);
    b = volume_vertex(0, 0, 2, -1, 0, 0, 1, 1, 0);
    g_assert_true(sgi_volume_contains(&a));
    g_assert_false(sgi_volume_contains(&b));
    g_assert_true(sgi_volume_clip_segment(&a, &b));
    assert_volume_vertex(&a, 0, 0, 0, 1, 1, 0, 0, 0, 0);
    assert_volume_vertex(&b, 0, 0, 0.5, 0.5, 0.75, 0, 0.25, 0.25, 0);
    b = volume_vertex(0.5, 0.5, 0, -1, 0, 0, 0, 0, 0);
    g_assert_false(sgi_volume_contains(&b));
    b.clip[3] = 0.5;
    g_assert_true(sgi_volume_contains(&b));
}

/*
 * Two triangles sharing an edge that crosses a plane make the same
 * vertex on it, bit for bit, whichever way round each has the edge, so
 * the clipped mesh stays closed: the quad A B C D's triangles A B C and
 * A C D share A C, which crosses the left, bottom and near planes.
 */
static void test_volume_clip_shares_edges(void)
{
    const SGIVolumeVertex A = volume_vertex(-1.5, -0.8, -2, 0.5,
                                            1, 0, 0, 0, 0.3);
    const SGIVolumeVertex B = volume_vertex(1.5, -0.8, -2, 0.5,
                                            0, 1, 0, 0.7, 0.1);
    const SGIVolumeVertex C = volume_vertex(0.6, 0.7, 1.3, 2,
                                            0, 0, 1, 0.3, 0.9);
    const SGIVolumeVertex D = volume_vertex(-0.7, 0.6, 1.1, 1.9,
                                            1, 1, 0, 0.9, 0.2);
    const SGIVolumeVertex abc[3] = { A, B, C }, acd[3] = { A, C, D };
    SGIVolumeVertex p[SGI_VOLUME_TRIANGLE_MAX], q[SGI_VOLUME_TRIANGLE_MAX];
    unsigned np = sgi_volume_clip_triangle(abc, p);
    unsigned nq = sgi_volume_clip_triangle(acd, q);
    unsigned shared = 0;

    g_assert_cmpuint(np, >=, 3);
    g_assert_cmpuint(nq, >=, 3);
    /* The vertex A C's crossing makes is in both, and the same. */
    for (unsigned i = 0; i < np; i++) {
        for (unsigned j = 0; j < nq; j++) {
            if (fabs(p[i].clip[0] - q[j].clip[0]) < CLIP_EPSILON &&
                fabs(p[i].clip[1] - q[j].clip[1]) < CLIP_EPSILON &&
                fabs(p[i].clip[3] - q[j].clip[3]) < CLIP_EPSILON &&
                p[i].clip[3] != C.clip[3]) {
                g_assert_cmpmem(&p[i], sizeof(p[i]), &q[j], sizeof(q[j]));
                shared++;
            }
        }
    }
    g_assert_cmpuint(shared, ==, 1);
}

/* A triangle drawn through a scene from its clip coordinates. */
static void frag_clip_triangle(FragScene *f, const float v[3][4])
{
    uint32_t word[4];

    word[0] = SGI_GL_TRIANGLES;
    frag_command(f, SGI_GL_OP_BEGIN, word, 1);
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 4; k++) {
            word[k] = bits(v[i][k]);
        }
        frag_command(f, SGI_GL_OP_VERTEX4F, word, 4);
    }
    frag_command(f, SGI_GL_OP_END, word, 0);
}

/*
 * Drawing clips to the view volume and never divides by a w <= 0. The
 * negative-w triangle above keeps a trapezoid from normalized y -2/3,
 * window row 10.7, to -1/2, row 16, x -2/3 to 2/3 at its bottom and -1/2
 * to 1/2 at its top, and draws only that: row 13 at the centre column
 * and the pixel at (12, 11) inside its left edge, but neither row 9 below
 * it nor rows 17 and 20 above, which the triangle's vertices divided by
 * their w, (16, 16), (48, 16) and (32, 32), would cover. A segment is
 * cut the same way. The negated triangle draws nothing, and nor does a
 * point with w < 0 that divides to a window point inside the viewport,
 * or one beyond the far plane.
 */
static void test_draw_clips_to_the_view_volume(void)
{
    static FragScene f;
    float tri[3][4] = {
        { -0.5, -0.5, 0, 1 }, { 0.5, -0.5, 0, 1 }, { 0, 0, -3, -1 },
    };
    uint32_t word[4];

    frag_init(&f);
    word[0] = bits(1), word[1] = bits(1), word[2] = bits(1);
    frag_command(&f, SGI_GL_OP_COLOR3F, word, 3);
    frag_clip_triangle(&f, tri);
    g_assert_cmphex(frag_pixel(&f, 32, 13), ==, 0xffffff);
    g_assert_cmphex(frag_pixel(&f, 12, 11), ==, 0xffffff);
    g_assert_cmphex(frag_pixel(&f, 32, 9), ==, 0);
    g_assert_cmphex(frag_pixel(&f, 32, 20), ==, 0);
    g_assert_cmphex(frag_pixel(&f, 20, 17), ==, 0);

    /*
     * Its left edge as a segment keeps the part from window (16, 16) down
     * to (10.7, 10.7), through (13, 13), and not the part the divide
     * would put up towards (32, 32).
     */
    memset(&f.p, 0, sizeof(f.p));
    word[0] = SGI_GL_LINES;
    frag_command(&f, SGI_GL_OP_BEGIN, word, 1);
    for (int i = 0; i < 3; i += 2) {
        for (int k = 0; k < 4; k++) {
            word[k] = bits(tri[i][k]);
        }
        frag_command(&f, SGI_GL_OP_VERTEX4F, word, 4);
    }
    frag_command(&f, SGI_GL_OP_END, word, 0);
    g_assert_cmphex(frag_pixel(&f, 13, 13), ==, 0xffffff);
    g_assert_cmphex(frag_pixel(&f, 20, 20), ==, 0);

    memset(&f.p, 0, sizeof(f.p));
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 4; k++) {
            tri[i][k] = -tri[i][k];
        }
    }
    frag_clip_triangle(&f, tri);
    word[0] = SGI_GL_POINTS;
    frag_command(&f, SGI_GL_OP_BEGIN, word, 1);
    word[0] = bits(0.25), word[1] = bits(0.25), word[2] = bits(0);
    word[3] = bits(-1);
    frag_command(&f, SGI_GL_OP_VERTEX4F, word, 4);
    word[2] = bits(2), word[3] = bits(1);
    frag_command(&f, SGI_GL_OP_VERTEX4F, word, 4);
    frag_command(&f, SGI_GL_OP_END, word, 0);
    g_assert_cmpuint(frag_count(&f, 0), ==, W * H);
}

/* The stencil rules with the 4x4 ordered dither, anchored to the screen. */
static const SGIRasterRules rules_dither = {
    .subpixel_bits = 8,
    .color_bits = 8,
    .depth_bits = 24,
    .stencil_bits = 8,
    .rounding = SGI_ROUND_HALF_AWAY,
    .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
    .arith = SGI_ARITH_DOUBLE,
    .line_rule = SGI_LINE_COVERAGE_DOUBLE,
    .dither = SGI_DITHER_BAYER4_TOP_ROWS,
    .dither_anchor = SGI_DITHER_SCREEN,
    .evidence = "test",
};

/* The 4x4 Bayer thresholds, in sixteenths, the top row first. */
static const unsigned test_bayer4[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
};

/* A scene drawing under the dither, the display's top line plane row 63. */
static void dither_init(FragScene *f)
{
    frag_init(f);
    f->rec.rules = &rules_dither;
    f->rec.target.screen_top = H - 1;
}

/* A flat-shaded triangle in one colour whose view-volume clip is the window. */
static void dither_triangle(FragScene *f, float r, float g, float b)
{
    static const float corner[3][2] = { { -1, -1 }, { 3, -1 }, { -1, 3 } };
    uint32_t word[3] = { SGI_GL_FLAT };

    frag_command(f, SGI_GL_OP_SHADE_MODEL, word, 1);
    word[0] = bits(r), word[1] = bits(g), word[2] = bits(b);
    frag_command(f, SGI_GL_OP_COLOR3F, word, 3);
    word[0] = SGI_GL_TRIANGLES;
    frag_command(f, SGI_GL_OP_BEGIN, word, 1);
    for (int i = 0; i < 3; i++) {
        word[0] = bits(corner[i][0]);
        word[1] = bits(corner[i][1]);
        frag_command(f, SGI_GL_OP_VERTEX2F, word, 2);
    }
    frag_command(f, SGI_GL_OP_END, word, 0);
}

/*
 * Every 4x4 block of plane 0, wherever it starts, holds each of a
 * channel's values lo and hi exactly n_lo and 16 - n_lo times. A 4x4
 * block of a pattern repeating every four pixels holds each threshold
 * once, whatever the pattern is anchored to, so this holds whichever
 * anchor the hardware has.
 */
static void assert_dither_blocks(const FragScene *f, uint32_t lo,
                                 unsigned n_lo)
{
    for (int by = 0; by + 4 <= H; by++) {
        for (int bx = 0; bx + 4 <= W; bx++) {
            for (int k = 0; k < 3; k++) {
                unsigned nlo = 0, nhi = 0;

                for (int y = by; y < by + 4; y++) {
                    for (int x = bx; x < bx + 4; x++) {
                        uint32_t v = (frag_pixel(f, x, y) >> (8 * k)) & 0xff;

                        nlo += v == lo;
                        nhi += v == lo + 1;
                    }
                }
                g_assert_cmpuint(nlo, ==, n_lo);
                g_assert_cmpuint(nhi, ==, 16 - n_lo);
            }
        }
    }
}

/*
 * The 4x4 ordered dither turns a flat 0.5 grey, 127.5 steps, into 127
 * where the threshold is below 8 sixteenths and 128 where it is not: in
 * every 4x4 block eight of each, for every channel. With GL_DITHER
 * disabled every pixel rounds to 128. The result of a blend is dithered
 * too, the source rounded before blending: a white source by a constant
 * factor of 0.5 dithers as the grey does.
 */
static void test_dither_counts(void)
{
    static FragScene f;
    uint32_t word[4];

    dither_init(&f);
    dither_triangle(&f, 0.5f, 0.5f, 0.5f);
    assert_dither_blocks(&f, 127, 8);

    memset(&f.p, 0, sizeof(f.p));
    enable(&f.c, SGI_GL_DITHER, false);
    dither_triangle(&f, 0.5f, 0.5f, 0.5f);
    g_assert_cmpuint(frag_count(&f, 0x808080), ==, W * H);

    memset(&f.p, 0, sizeof(f.p));
    enable(&f.c, SGI_GL_DITHER, true);
    enable(&f.c, SGI_GL_BLEND, true);
    for (int i = 0; i < 4; i++) {
        word[i] = bits(0.5f);
    }
    frag_command(&f, SGI_GL_OP_BLEND_COLOR, word, 4);
    frag_blend(&f, SGI_GL_CONSTANT_COLOR, 0, SGI_GL_FUNC_ADD);
    dither_triangle(&f, 1, 1, 1);
    assert_dither_blocks(&f, 127, 8);
}

/*
 * Dithering follows blending (4.1.7): the source is rounded, not
 * dithered, before it is blended. A 0.5 grey source, 127.5 steps, blended
 * with GL_ONE and GL_ZERO rounds to 128 and blends to 128 exactly, which
 * no threshold moves: every pixel 128. Dithered first, half its pixels
 * would be 127.
 */
static void test_dither_follows_blend(void)
{
    static FragScene f;

    dither_init(&f);
    enable(&f.c, SGI_GL_BLEND, true);
    frag_blend(&f, 1, 0, SGI_GL_FUNC_ADD);
    dither_triangle(&f, 0.5f, 0.5f, 0.5f);
    g_assert_cmpuint(frag_count(&f, 0x808080), ==, W * H);
}

/*
 * Where the pattern falls, anchored to the screen, rows counted down from
 * the display's top line: each channel of pixel (x, y) is
 * floor(c * 255 + B / 16) for the threshold B at column x mod 4 and row
 * (top - y) mod 4 in planes whose rows run up the display, (y - top) mod 4
 * in planes whose rows run down it. Red 0.5, green 0.25 and blue 0.0396,
 * 127.5, 63.75 and 10.098 steps, round up from 8, 4 and 15 sixteenths.
 * Moving the top line a row moves the pattern with it. Which anchor the
 * hardware has is not known; this pins the one the model takes.
 */
static void test_dither_screen_pattern(void)
{
    static FragScene f;
    static const float c[3] = { 0.5f, 0.25f, 0.0396f };
    static const struct {
        int top;
        bool down;
    } runs[] = { { H - 1, false }, { H, false }, { 0, true }, { 1, true } };

    for (int i = 0; i < ARRAY_SIZE(runs); i++) {
        int top = runs[i].top;

        dither_init(&f);
        f.rec.target.screen_top = top;
        f.rec.target.screen_rows_down = runs[i].down;
        dither_triangle(&f, c[0], c[1], c[2]);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int row = runs[i].down ? y - top : top - y;
                unsigned b = test_bayer4[row & 3][x & 3];
                uint32_t want = 0;

                for (int k = 0; k < 3; k++) {
                    want = (want << 8) |
                           (uint32_t)floor(c[k] * 255 + b / 16.0);
                }
                g_assert_cmphex(frag_pixel(&f, x, y), ==, want);
            }
        }
    }
}

/*
 * A clear is not dithered, whatever GL_DITHER's enable: a clear colour of
 * 0.5 grey rounds to 128 at every pixel under the dithering rules.
 */
static void test_dither_skips_clears(void)
{
    static FragScene f;
    uint32_t word[4];

    dither_init(&f);
    for (int i = 0; i < 4; i++) {
        word[i] = bits(0.5f);
    }
    frag_command(&f, SGI_GL_OP_CLEAR_COLOR, word, 4);
    frag_clear(&f, SGI_GL_COLOR_BUFFER_BIT);
    g_assert_cmpuint(frag_count(&f, 0x808080), ==, W * H);
}

/*
 * Draws a smooth-shaded, partly blended scene of random triangles, with
 * GL_DITHER on or off, under rules.
 */
static void dither_scene(FragScene *f, const SGIRasterRules *rules,
                         bool dither)
{
    GRand *rand = g_rand_new_with_seed(4129);
    uint32_t word[4];

    frag_init(f);
    f->rec.rules = rules;
    f->rec.target.screen_top = H - 1;
    enable(&f->c, SGI_GL_DITHER, dither);
    frag_blend(f, SGI_GL_SRC_ALPHA, SGI_GL_ONE_MINUS_SRC_ALPHA,
               SGI_GL_FUNC_ADD);
    for (int t = 0; t < 24; t++) {
        enable(&f->c, SGI_GL_BLEND, t & 1);
        word[0] = SGI_GL_TRIANGLES;
        frag_command(f, SGI_GL_OP_BEGIN, word, 1);
        for (int i = 0; i < 3; i++) {
            for (int k = 0; k < 4; k++) {
                word[k] = bits(g_rand_double(rand));
            }
            frag_command(f, SGI_GL_OP_COLOR4F, word, 4);
            word[0] = bits(g_rand_double_range(rand, -1.2, 1.2));
            word[1] = bits(g_rand_double_range(rand, -1.2, 1.2));
            frag_command(f, SGI_GL_OP_VERTEX2F, word, 2);
        }
        frag_command(f, SGI_GL_OP_END, word, 0);
    }
    g_rand_free(rand);
}

/*
 * Rules without a dither draw exactly as before dithering existed:
 * GL_DITHER's enable, on initially, changes no byte of a smooth-shaded,
 * partly blended scene, which the dithering rules do change, and a window
 * anchor beside no dither is never read.
 */
static void test_dither_none_is_unchanged(void)
{
    static FragScene off, on, anchored, dithered;
    SGIRasterRules window = rules_stencil;

    window.dither_anchor = SGI_DITHER_WINDOW;
    dither_scene(&off, &rules_stencil, false);
    dither_scene(&on, &rules_stencil, true);
    dither_scene(&anchored, &window, true);
    dither_scene(&dithered, &rules_dither, true);
    g_assert_cmpmem(&off.p, sizeof(off.p), &on.p, sizeof(on.p));
    g_assert_cmpmem(&off.p, sizeof(off.p), &anchored.p, sizeof(anchored.p));
    g_assert_true(memcmp(&off.p.colour, &dithered.p.colour,
                         sizeof(off.p.colour)) != 0);
}

/*
 * Give the scene's context the window matrix whose rows, as a matrix is
 * written on paper, are rows.
 */
static void window_matrix(FragScene *f, const float rows[4][4], float scale,
                          bool down)
{
    float m[16];

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            m[col * 4 + row] = rows[row][col];
        }
    }
    sgi_gl_set_window_matrix(&f->c, m, scale, down);
}

/* White primitive of mode through eye positions xyz, n of them. */
static void window_draw(FragScene *f, uint32_t mode, const float v[][3],
                        unsigned n)
{
    uint32_t word[3] = { bits(1), bits(1), bits(1) };

    frag_command(f, SGI_GL_OP_COLOR3F, word, 3);
    word[0] = mode;
    frag_command(f, SGI_GL_OP_BEGIN, word, 1);
    for (unsigned i = 0; i < n; i++) {
        for (int k = 0; k < 3; k++) {
            word[k] = bits(v[i][k]);
        }
        frag_command(f, SGI_GL_OP_VERTEX3F, word, 3);
    }
    frag_command(f, SGI_GL_OP_END, word, 0);
}

/* Whether each row y0 to y1 of column x holds white, and no other row. */
static void assert_column(const FragScene *f, int x, int y0, int y1)
{
    for (int y = 0; y < H; y++) {
        g_assert_cmphex(frag_pixel(f, x, y), ==,
                        y >= y0 && y <= y1 ? 0xffffff : 0);
    }
}

static const float window_square[4][3] = {
    { -1, -1, 0 }, { 1, -1, 0 }, { 1, 1, 0 }, { -1, 1, 0 },
};

/*
 * A window matrix places a primitive in window pixels as it gives them,
 * unrounded, and the viewport, here one pixel, plays no part. The matrix
 * x' = 16 x + 32, y' = 16 y + Y takes the square from -1 to 1 to columns
 * 16 to 48 and rows Y - 16 to Y + 16. With Y = 32.5 its bottom edge lies
 * on row 16's centres, which a bottom edge does not own, and its top edge
 * on row 48's, which a top edge does: rows 17 to 48. A client's bias of
 * 1/256 row down, Y = 32.5 - 1/256, puts row 16's centres inside and row
 * 48's outside: rows 16 to 47. Columns 16 to 47 in both. A point at the
 * eye origin under y' = 16 y + 17 - 1/256 lies at window row 16.996, in
 * row 16; rounded to a whole row it would be in row 17.
 */
static void test_window_matrix_position(void)
{
    static FragScene f;
    static const struct {
        float y;
        int row0, row1;
    } runs[] = { { 32.5f, 17, 48 }, { 32.5f - 1.0f / 256, 16, 47 } };
    uint32_t viewport[4] = { 0, 0, 1, 1 };

    for (int i = 0; i < ARRAY_SIZE(runs); i++) {
        const float rows[4][4] = {
            { 16, 0, 0, 32 }, { 0, 16, 0, runs[i].y },
            { 0, 0, 0, 0.5f }, { 0, 0, 0, 1 },
        };

        frag_init(&f);
        frag_command(&f, SGI_GL_OP_VIEWPORT, viewport, 4);
        window_matrix(&f, rows, 1, false);
        window_draw(&f, SGI_GL_QUADS, window_square, 4);
        assert_column(&f, 16, runs[i].row0, runs[i].row1);
        assert_column(&f, 47, runs[i].row0, runs[i].row1);
        assert_column(&f, 15, 1, 0);
        assert_column(&f, 48, 1, 0);
    }

    {
        static const float point[1][3] = { { 0, 0, 0 } };
        const float rows[4][4] = {
            { 16, 0, 0, 32 }, { 0, 16, 0, 17 - 1.0f / 256 },
            { 0, 0, 0, 0.5f }, { 0, 0, 0, 1 },
        };

        frag_init(&f);
        window_matrix(&f, rows, 1, false);
        window_draw(&f, SGI_GL_POINTS, point, 1);
        assert_column(&f, 32, 16, 16);
    }
}

/*
 * A primitive reaching far past the window is clipped to the guard band,
 * not dropped. Under x' = 16 x + 32, y' = 16 y + 32 the triangle
 * (-1, -1), (6250, -1), (-1, 4) runs from window (16, 16) to (100032, 16)
 * and (16, 96), beyond the rasterizer's reach of 65536 pixels, and covers
 * the window from column and row 16 up: the guard band cuts it back, and
 * its part in the window draws.
 */
static void test_window_matrix_guard_band(void)
{
    static FragScene f;
    static const float rows[4][4] = {
        { 16, 0, 0, 32 }, { 0, 16, 0, 32 }, { 0, 0, 0, 0.5f }, { 0, 0, 0, 1 },
    };
    static const float tri[3][3] = {
        { -1, -1, 0 }, { 6250, -1, 0 }, { -1, 4, 0 },
    };

    frag_init(&f);
    window_matrix(&f, rows, 1, false);
    window_draw(&f, SGI_GL_TRIANGLES, tri, 3);
    assert_column(&f, 16, 16, H - 1);
    assert_column(&f, W - 1, 16, H - 1);
    assert_column(&f, 15, 1, 0);
}

/*
 * Window depth is the depth scale times h2 / h3, and the depth range does
 * not map it again. Under x' = (32 x + 64) / 2, y' = (32 y + 64) / 2,
 * h2 = -z and h3 = 2, a scale of -0.5 puts eye z 2 at window depth
 * -0.5 x -2 / 2 = 0.5, stored as 0.5 x (2^24 - 1) rounded half away,
 * 8388608, whatever glDepthRange says; eye (0.5, -0.25) is window (40, 28).
 * glRasterPos goes through the same matrix, to (40, 28, 0.5) with w 2.
 */
static void test_window_matrix_depth(void)
{
    static FragScene f;
    static const float rows[4][4] = {
        { 32, 0, 0, 64 }, { 0, 32, 0, 64 }, { 0, 0, -1, 0 }, { 0, 0, 0, 2 },
    };
    static const float square[4][3] = {
        { -1, -1, 2 }, { 1, -1, 2 }, { 1, 1, 2 }, { -1, 1, 2 },
    };
    uint32_t word[3] = { SGI_GL_DEPTH_TEST };

    frag_init(&f);
    frag_command(&f, SGI_GL_OP_ENABLE, word, 1);
    word[0] = SGI_GL_ALWAYS + SGI_GL_NEVER_ENUM;
    frag_command(&f, SGI_GL_OP_DEPTH_FUNC, word, 1);
    word[0] = bits(0.5f), word[1] = bits(1);
    frag_command(&f, SGI_GL_OP_DEPTH_RANGE, word, 2);
    window_matrix(&f, rows, -0.5f, false);
    window_draw(&f, SGI_GL_QUADS, square, 4);
    g_assert_cmphex(frag_pixel(&f, 40, 28), ==, 0xffffff);
    g_assert_cmpuint(f.p.depth[planes_index(40, 28)], ==, 8388608);
    g_assert_cmpuint(f.p.depth[planes_index(16, 16)], ==, 8388608);
    g_assert_cmpuint(f.p.depth[planes_index(15, 16)], ==, 0);

    word[0] = bits(0.5f), word[1] = bits(-0.25f), word[2] = bits(2);
    frag_command(&f, SGI_GL_OP_RASTER_POS3F, word, 3);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_CURRENT_RASTER_POSITION, 0), ==,
                      40);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_CURRENT_RASTER_POSITION, 1), ==,
                      28);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_CURRENT_RASTER_POSITION, 2), ==,
                      0.5f);
    g_assert_cmpfloat(get_float(&f.c, SGI_GL_CURRENT_RASTER_POSITION, 3), ==,
                      2);
}

/*
 * Under a window matrix a primitive is clipped to window depth 0 to 1,
 * OpenGL's near and far planes, before the divide. The perspective matrix
 * x' = (16 x - 32 z) / -z, y' = (16 y - 32 z) / -z, depth
 * (-1.5 z - 1.5) / -z puts eye z -1 at depth 0 and -3 at depth 1, and eye
 * z -2 at window (8 x + 32, 8 y + 32) and depth 0.75. The triangle
 * (-2, -2, -2), (2, -2, -2), (0, 2, 1) has its third vertex behind the
 * eye. Cut at z -1, a third and a quarter of the way along its sides, it
 * keeps the trapezoid (16, 16), (48, 16), (53.33, 21.33), (10.67, 21.33):
 * rows 16 to 20 at column 32, row 20 reaching column 12 but not 10, and
 * nothing below or above. Its depth falls linearly from 0.75 at row 16's
 * edge to 0 at the cut, 0.3984375 at row 18's centres, to within the
 * cut's snapping to the subpixel grid.
 */
static void test_window_matrix_clips_near(void)
{
    static FragScene f;
    static const float rows[4][4] = {
        { 16, 0, -32, 0 }, { 0, 16, -32, 0 },
        { 0, 0, -1.5f, -1.5f }, { 0, 0, -1, 0 },
    };
    static const float tri[3][3] = {
        { -2, -2, -2 }, { 2, -2, -2 }, { 0, 2, 1 },
    };
    uint32_t word[1] = { SGI_GL_DEPTH_TEST };
    double want = 0.3984375 * 16777215;

    frag_init(&f);
    frag_command(&f, SGI_GL_OP_ENABLE, word, 1);
    word[0] = SGI_GL_ALWAYS + SGI_GL_NEVER_ENUM;
    frag_command(&f, SGI_GL_OP_DEPTH_FUNC, word, 1);
    window_matrix(&f, rows, 1, false);
    window_draw(&f, SGI_GL_TRIANGLES, tri, 3);
    assert_column(&f, 32, 16, 20);
    g_assert_cmphex(frag_pixel(&f, 12, 20), ==, 0xffffff);
    g_assert_cmphex(frag_pixel(&f, 10, 20), ==, 0);
    g_assert_cmpfloat_with_epsilon(f.p.depth[planes_index(32, 18)], want,
                                   16777215.0 / 1024);
}

/*
 * Facing under a window matrix whose rows run down the display. The matrix
 * x' = 16 x + 32, y' = -16 y + 32 turns the client's y upside down, so a
 * triangle counter-clockwise as the client drew it runs clockwise in
 * window rows. It is still the front face: kept with back faces culled,
 * culled with front faces culled, and the reverse under glFrontFace(GL_CW).
 * Two-sided colour follows the same facing, and rows running up give the
 * same answers.
 */
static void test_window_matrix_rows_down_facing(void)
{
    static FragScene f;
    static const float ccw[3][3] = { { -1, -1, 0 }, { 1, -1, 0 },
                                     { 0, 1, 0 } };
    static const float cw[3][3] = { { -1, -1, 0 }, { 0, 1, 0 },
                                    { 1, -1, 0 } };
    static const struct {
        uint32_t cull, front;
        bool ccw_drawn, cw_drawn;
    } runs[] = {
        { SGI_GL_BACK, SGI_GL_CCW, true, false },
        { SGI_GL_FRONT, SGI_GL_CCW, false, true },
        { SGI_GL_BACK, SGI_GL_CW, false, true },
        { SGI_GL_FRONT, SGI_GL_CW, true, false },
    };
    uint32_t word[1];

    for (int down = 0; down < 2; down++) {
        const float rows[4][4] = {
            { 16, 0, 0, 32 }, { 0, down ? -16 : 16, 0, 32 },
            { 0, 0, 0, 0.5f }, { 0, 0, 0, 1 },
        };

        for (int i = 0; i < ARRAY_SIZE(runs); i++) {
            frag_init(&f);
            window_matrix(&f, rows, 1, down);
            enable(&f.c, SGI_GL_CULL_FACE, true);
            word[0] = runs[i].cull;
            frag_command(&f, SGI_GL_OP_CULL_FACE, word, 1);
            word[0] = runs[i].front;
            frag_command(&f, SGI_GL_OP_FRONT_FACE, word, 1);
            window_draw(&f, SGI_GL_TRIANGLES, ccw, 3);
            g_assert_cmpint(frag_pixel(&f, 32, 32) == 0xffffff, ==,
                            runs[i].ccw_drawn);
            memset(&f.p, 0, sizeof(f.p));
            window_draw(&f, SGI_GL_TRIANGLES, cw, 3);
            g_assert_cmpint(frag_pixel(&f, 32, 32) == 0xffffff, ==,
                            runs[i].cw_drawn);
        }

        /* Front red, back blue, drawn straight through the rasterizer. */
        frag_init(&f);
        window_matrix(&f, rows, 1, down);
        for (int t = 0; t < 2; t++) {
            const float (*v)[3] = t ? cw : ccw;
            float verts[3][SGI_GL_VERTEX_FLOATS] = { { 0 } };
            SGIGLDraw d = {
                .context = 0x1234, .mode = SGI_GL_TRIANGLES, .count = 3,
                .vertices = &verts[0][0], .back_colors = true,
                .state = &f.c.cur,
            };

            for (int i = 0; i < 3; i++) {
                memcpy(&verts[i][SGI_GL_V_POSITION], v[i], sizeof(v[i]));
                verts[i][SGI_GL_V_POSITION + 3] = 1;
                verts[i][SGI_GL_V_COLOR] = 1;
                verts[i][SGI_GL_V_COLOR + 3] = 1;
                verts[i][SGI_GL_V_BACK_COLOR + 2] = 1;
                verts[i][SGI_GL_V_BACK_COLOR + 3] = 1;
            }
            memset(&f.p, 0, sizeof(f.p));
            sgi_soft_draw(&rules_stencil, &f.rec.target, &d);
            g_assert_cmphex(frag_pixel(&f, 32, 32), ==, t ? 0x0000ff :
                                                            0xff0000);
        }
    }
}

/*
 * Without a window matrix nothing changes: a context given one and then
 * none draws, byte for byte, as one never given one, and its window
 * matrix state is sgi_gl_init()'s again, which migration then leaves out.
 */
static void test_window_matrix_off_is_unchanged(void)
{
    static FragScene plain, cleared;
    static const float rows[4][4] = {
        { 3, 0, 0, 1 }, { 0, -5, 0, 2 }, { 0, 0, 7, 0 }, { 0, 0, 0, 1 },
    };
    static const float tri[3][3] = {
        { -0.9f, -0.7f, 0.3f }, { 0.8f, -0.2f, -0.4f }, { -0.1f, 0.95f, 0 },
    };
    SGIGLContext init;

    frag_init(&plain);
    frag_init(&cleared);
    window_matrix(&cleared, rows, -2, true);
    sgi_gl_set_window_matrix(&cleared.c, NULL, 4, true);
    sgi_gl_init(&init, 0);
    g_assert_cmpmem(&cleared.c.cur.window_matrix_given,
                    offsetof(SGIGLDrawState, window_rows_down) +
                    sizeof(uint32_t) -
                    offsetof(SGIGLDrawState, window_matrix_given),
                    &init.cur.window_matrix_given,
                    offsetof(SGIGLDrawState, window_rows_down) +
                    sizeof(uint32_t) -
                    offsetof(SGIGLDrawState, window_matrix_given));
    window_draw(&plain, SGI_GL_TRIANGLES, tri, 3);
    window_draw(&cleared, SGI_GL_TRIANGLES, tri, 3);
    g_assert_cmpuint(frag_count(&plain, 0), <, W * H);
    g_assert_cmpmem(&plain.p, sizeof(plain.p), &cleared.p,
                    sizeof(cleared.p));
}

/* Public OpenGL equations, using synthetic image storage only. */
static void test_texture_coordinates(void)
{
    SGIGLContext c;
    uint32_t out[SGI_GL_GET_WORDS];
    float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0,
                    2, 0, 1, 1, 0, 0, 0, 1 };

    context_init(&c);
    g_assert_cmpuint(get(&c, SGI_GL_CURRENT_TEXTURE_COORDS, out), ==, 4);
    g_assert_cmphex(out[3], ==, bits(1));
    command(&c, NULL, SGI_GL_OP_TEXCOORD3F,
            (uint32_t[]) { bits(0.25f), bits(0.5f), bits(0.125f) }, 3);
    get(&c, SGI_GL_CURRENT_TEXTURE_COORDS, out);
    g_assert_cmphex(out[0], ==, bits(0.25f));
    g_assert_cmphex(out[2], ==, bits(0.125f));
    g_assert_true(sgi_gl_set_matrix(&c, SGI_GL_TEXTURE, m));
    command(&c, NULL, SGI_GL_OP_RASTER_POS3F,
            (uint32_t[]) { 0, 0, 0 }, 3);
    get(&c, SGI_GL_CURRENT_RASTER_TEXTURE_COORDS, out);
    g_assert_cmphex(out[0], ==, bits(0.5f));
    g_assert_cmphex(out[3], ==, bits(1.125f));
    sgi_gl_init(&c, 7);
    get(&c, SGI_GL_CURRENT_TEXTURE_COORDS, out);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmphex(out[3], ==, bits(1));
    get(&c, SGI_GL_TEXTURE_ENV_MODE, out);
    g_assert_cmphex(out[0], ==, SGI_GL_MODULATE);
}

typedef struct TexturePixels {
    uint32_t pixel[4];
    unsigned reads;
    bool fail;
} TexturePixels;

static bool texture_read(void *opaque, uint32_t x, uint32_t y, uint32_t *rgba)
{
    TexturePixels *p = opaque;

    g_assert_cmpuint(x, <, 2);
    g_assert_cmpuint(y, <, 2);
    p->reads++;
    *rgba = p->pixel[y * 2 + x];
    return !p->fail;
}

static SGIGLTexImage texture_image(TexturePixels *p)
{
    return (SGIGLTexImage) {
        .width = 2, .height = 2, .base_format = SGI_GL_RGBA,
        .min_filter = SGI_GL_NEAREST, .mag_filter = SGI_GL_NEAREST,
        .wrap_s = SGI_GL_REPEAT, .wrap_t = SGI_GL_REPEAT,
        .read_texel = texture_read, .opaque = p,
    };
}

static void texture_env(FragScene *f, uint32_t pname, float a, float b,
                        float c, float d)
{
    frag_command(f, SGI_GL_OP_TEX_ENV,
                 (uint32_t[]) { pname, bits(a), bits(b), bits(c), bits(d) }, 5);
}

static void texture_coord(FragScene *f, float s, float t, float r, float q)
{
    frag_command(f, SGI_GL_OP_TEXCOORD4F,
                 (uint32_t[]) { bits(s), bits(t), bits(r), bits(q) }, 4);
}

static void test_texture_sampling(void)
{
    const struct {
        uint32_t filter, wrap;
        float s, t;
        uint32_t expected;
    } cases[] = {
        { SGI_GL_NEAREST, SGI_GL_REPEAT, 0.25f, 0.25f, 0xff0000 },
        { SGI_GL_LINEAR, SGI_GL_REPEAT, 0.5f, 0.5f, 0x808080 },
        { SGI_GL_LINEAR, SGI_GL_CLAMP, 0, 0.25f, 0x800000 },
        { SGI_GL_LINEAR, SGI_GL_CLAMP_TO_EDGE, 0, 0.25f, 0xff0000 },
        { SGI_GL_LINEAR, SGI_GL_CLAMP_TO_BORDER, -0.25f, 0.25f, 0 },
        { SGI_GL_LINEAR, SGI_GL_REPEAT, 0, 0.25f, 0x808000 },
        { SGI_GL_NEAREST, SGI_GL_REPEAT, -0.25f, 0.25f, 0x00ff00 },
        { SGI_GL_NEAREST, SGI_GL_CLAMP, 1, 0.25f, 0x00ff00 },
        { SGI_GL_NEAREST, SGI_GL_CLAMP_TO_BORDER, 1, 0.25f, 0 },
        { SGI_GL_NEAREST, SGI_GL_REPEAT, 1e30f, 0.25f, 0xff0000 },
    };
    TexturePixels pixels = { .pixel = {
        0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff } };
    SGIGLTexImage image = texture_image(&pixels);
    FragScene f;

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        frag_init(&f);
        enable(&f.c, SGI_GL_TEXTURE_2D, true);
        texture_env(&f, SGI_GL_TEXTURE_ENV_MODE, SGI_GL_REPLACE, 0, 0, 0);
        image.min_filter = image.mag_filter = cases[i].filter;
        image.wrap_s = image.wrap_t = cases[i].wrap;
        f.rec.target.texture = &image;
        texture_coord(&f, cases[i].s, cases[i].t, 0, 1);
        frag_quad(&f, 1, 0, 1, 1);
        g_assert_cmphex(frag_pixel(&f, 16, 16), ==, cases[i].expected);
    }
}

static uint32_t texture_rgb(double r, double g, double b)
{
    return ((uint32_t)floor(r * 255 + 0.5) << 16) |
           ((uint32_t)floor(g * 255 + 0.5) << 8) |
           (uint32_t)floor(b * 255 + 0.5);
}

/* Different unused channels expose accidental generic-RGBA environments. */
static void test_texture_environment(void)
{
    const double l = 64.0 / 255, u = 64.0 / 255, b = 128.0 / 255;
    const struct {
        uint32_t format, mode;
        double rgb[3], alpha;
    } cases[] = {
        { SGI_GL_ALPHA, SGI_GL_REPLACE, { 0.25, 0.5, 0.75 }, u },
        { SGI_GL_ALPHA, SGI_GL_MODULATE, { 0.25, 0.5, 0.75 }, 0.8 * u },
        { SGI_GL_ALPHA, SGI_GL_BLEND, { 0.25, 0.5, 0.75 }, 0.8 * u },
        { SGI_GL_LUMINANCE, SGI_GL_REPLACE, { l, l, l }, 0.8 },
        { SGI_GL_LUMINANCE, SGI_GL_MODULATE,
          { 0.25 * l, 0.5 * l, 0.75 * l }, 0.8 },
        { SGI_GL_LUMINANCE, SGI_GL_BLEND,
          { 0.25 * (1 - l), 0.5 * (1 - l) + l, 0.75 * (1 - l) }, 0.8 },
        { SGI_GL_LUMINANCE_ALPHA, SGI_GL_REPLACE, { l, l, l }, u },
        { SGI_GL_LUMINANCE_ALPHA, SGI_GL_MODULATE,
          { 0.25 * l, 0.5 * l, 0.75 * l }, 0.8 * u },
        { SGI_GL_LUMINANCE_ALPHA, SGI_GL_BLEND,
          { 0.25 * (1 - l), 0.5 * (1 - l) + l, 0.75 * (1 - l) }, 0.8 * u },
        { SGI_GL_INTENSITY, SGI_GL_REPLACE, { l, l, l }, l },
        { SGI_GL_INTENSITY, SGI_GL_MODULATE,
          { 0.25 * l, 0.5 * l, 0.75 * l }, 0.8 * l },
        { SGI_GL_INTENSITY, SGI_GL_BLEND,
          { 0.25 * (1 - l), 0.5 * (1 - l) + l, 0.75 * (1 - l) },
          0.8 * (1 - l) + 0.2 * l },
        { SGI_GL_RGB, SGI_GL_REPLACE, { l, 0, b }, 0.8 },
        { SGI_GL_RGB, SGI_GL_MODULATE, { 0.25 * l, 0, 0.75 * b }, 0.8 },
        { SGI_GL_RGB, SGI_GL_BLEND,
          { 0.25 * (1 - l), 0.5, 0.75 * (1 - b) }, 0.8 },
        { SGI_GL_RGBA, SGI_GL_REPLACE, { l, 0, b }, u },
        { SGI_GL_RGBA, SGI_GL_MODULATE, { 0.25 * l, 0, 0.75 * b }, 0.8 * u },
        { SGI_GL_RGBA, SGI_GL_BLEND,
          { 0.25 * (1 - l), 0.5, 0.75 * (1 - b) }, 0.8 * u },
        { SGI_GL_RGB, SGI_GL_DECAL, { l, 0, b }, 0.8 },
        { SGI_GL_RGBA, SGI_GL_DECAL,
          { 0.25 * (1 - u) + l * u, 0.5 * (1 - u),
            0.75 * (1 - u) + b * u }, 0.8 },
    };
    TexturePixels pixels = { .pixel = {
        0x40008040, 0x40008040, 0x40008040, 0x40008040 } };
    SGIGLTexImage image = texture_image(&pixels);
    FragScene f;

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        for (int alpha_check = 0; alpha_check < 4; alpha_check++) {
            double a = 1;

            if (alpha_check == 1) {
                a = floor(cases[i].alpha * 255 + 0.5) / 255;
            }
            double want[3];

            /* The existing blend path packs source RGB and alpha first. */
            for (int k = 0; k < 3; k++) {
                want[k] = cases[i].rgb[k];
                if (alpha_check == 1) {
                    want[k] = floor(want[k] * 255 + 0.5) / 255 * a;
                }
            }

            frag_init(&f);
            enable(&f.c, SGI_GL_TEXTURE_2D, true);
            image.base_format = cases[i].format;
            f.rec.target.texture = &image;
            texture_env(&f, SGI_GL_TEXTURE_ENV_MODE, cases[i].mode, 0, 0, 0);
            texture_env(&f, SGI_GL_TEXTURE_ENV_COLOR, 0, 1, 0, 0.2f);
            texture_coord(&f, 0.25f, 0.25f, 0, 1);
            if (alpha_check == 1) {
                enable(&f.c, SGI_GL_BLEND, true);
                frag_command(&f, SGI_GL_OP_BLEND_FUNC,
                             (uint32_t[]) { SGI_GL_SRC_ALPHA, 0 }, 2);
            }
            if (alpha_check >= 2) {
                /* Check texture alpha before blending, in alpha-test bits. */
                float ref = floor(cases[i].alpha * 255 + 0.5) / 255;

                if (alpha_check == 2) {
                    ref -= 1.0 / 255;
                } else {
                    ref += 1.0 / 255;
                }

                enable(&f.c, SGI_GL_ALPHA_TEST, true);
                frag_command(&f, SGI_GL_OP_ALPHA_FUNC,
                             (uint32_t[]) { SGI_GL_NEVER_ENUM + SGI_GL_GREATER,
                                            bits(ref) }, 2);
            }
            frag_quad(&f, 0.25f, 0.5f, 0.75f, 0.8f);
            g_assert_cmphex(frag_pixel(&f, 16, 16), ==,
                           alpha_check == 3 ? 0 :
                           texture_rgb(want[0], want[1], want[2]));
        }
    }
}

static void texture_vertex(FragScene *f, float x, float y, float w,
                           float s, float t, float q)
{
    texture_coord(f, s, t, 0, q);
    frag_command(f, SGI_GL_OP_VERTEX4F,
                 (uint32_t[]) { bits(x * w), bits(y * w), 0, bits(w) }, 4);
}

/* Replicas use their own centres and the original, unshifted endpoints. */
static void test_wide_interpolation(void)
{
    TexturePixels pixels = { .pixel = {
        0x000000ff, 0xffffffff, 0x000000ff, 0xffffffff } };
    SGIGLTexImage image = texture_image(&pixels);
    FragScene f;
    float v[2][SGI_GL_VERTEX_FLOATS] = { 0 };
    SGIGLDraw d = {
        .mode = SGI_GL_LINES, .count = 2, .vertices = &v[0][0],
        .state = &f.c.cur, .line_width = 2,
    };

    frag_init(&f);
    f.c.cur.depth_test = true;
    f.c.cur.depth_func = SGI_GL_ALWAYS;
    for (unsigned j = 0; j < 2; j++) {
        float w = j + 1;

        v[j][SGI_GL_V_POSITION] = ((j ? 5.5f : 1.5f) / 32 - 1) * w;
        v[j][SGI_GL_V_POSITION + 1] = ((j ? 3.5f : 1.5f) / 32 - 1) * w;
        v[j][SGI_GL_V_POSITION + 2] = (j ? 1 : -1) * w;
        v[j][SGI_GL_V_POSITION + 3] = w;
        v[j][SGI_GL_V_TEXCOORD] = j;
        v[j][SGI_GL_V_TEXCOORD + 1] = 0.25f * w;
        v[j][SGI_GL_V_TEXCOORD + 3] = w;
        for (unsigned k = 0; k < 3; k++) {
            v[j][SGI_GL_V_COLOR + k] = j;
        }
        v[j][SGI_GL_V_COLOR + 3] = 1;
    }
    sgi_soft_draw(&rules_stencil, &f.rec.target, &d);
    /* t=1/5 and 3/10; inverse-W colour is 1/9 and 3/17. */
    g_assert_cmphex(frag_pixel(&f, 2, 1), ==, 0x1c1c1c);
    g_assert_cmphex(frag_pixel(&f, 2, 2), ==, 0x2d2d2d);
    g_assert_cmpuint(f.p.depth[planes_index(2, 1)], ==, 3355443);
    g_assert_cmpuint(f.p.depth[planes_index(2, 2)], ==, 5033165);

    memset(&f.p, 0, sizeof(f.p));
    f.rec.target.texture = &image;
    image.min_filter = image.mag_filter = SGI_GL_LINEAR;
    enable(&f.c, SGI_GL_TEXTURE_2D, true);
    texture_env(&f, SGI_GL_TEXTURE_ENV_MODE, SGI_GL_REPLACE, 0, 0, 0);
    sgi_soft_draw(&rules_stencil, &f.rec.target, &d);
    /* S=1/10 and 3/20 blend the repeated white/black boundary differently. */
    g_assert_cmphex(frag_pixel(&f, 2, 1), ==, 0x4d4d4d);
    g_assert_cmphex(frag_pixel(&f, 2, 2), ==, 0x333333);

    memset(&f.p, 0, sizeof(f.p));
    enable(&f.c, SGI_GL_TEXTURE_2D, false);
    d.line_width = 3;
    v[0][SGI_GL_V_POSITION + 2] = -0.5f;
    v[1][SGI_GL_V_POSITION + 2] = 1;
    sgi_soft_draw(&rules_stencil, &f.rec.target, &d);
    /* Raw t=-1/10: the retained clamp extends the first endpoint's values. */
    g_assert_cmpuint(f.p.depth[planes_index(1, 0)], ==, 4194304);
    g_assert_cmphex(frag_pixel(&f, 1, 0), ==, 0);
    f.c.cur.shade_model = SGI_GL_FLAT;
    sgi_soft_draw(&rules_stencil, &f.rec.target, &d);
    g_assert_cmphex(frag_pixel(&f, 2, 1), ==, 0xffffff);
}

/* The square's fragments individually observe ownership, scissor and depth. */
static void test_wide_fragment_ops(void)
{
    FragScene f;

    frag_init(&f);
    f.c.point_size = 5;
    f.c.cur.drawable.clip = (SGIClipList) {
        .n = 2, .r = { { 2, 1, 3, 5 }, { 5, 1, 6, 5 } },
    };
    frag_scissor(&f, 2, 2, 5, 2);
    enable(&f.c, SGI_GL_SCISSOR_TEST, true);
    f.c.cur.colour_mask = SGI_GL_COLOUR_MASK_RED;
    frag_command(&f, SGI_GL_OP_BEGIN, (uint32_t[]) { SGI_GL_POINTS }, 1);
    wide_vertex(&f, 4.25, 3.75);
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    g_assert_cmpuint(frag_count(&f, 0xff0000), ==, 8);
    g_assert_cmphex(frag_pixel(&f, 4, 3), ==, 0);
    enable(&f.c, SGI_GL_DEPTH_TEST, true);
    memset(&f.p, 0, sizeof(f.p));
    frag_command(&f, SGI_GL_OP_BEGIN, (uint32_t[]) { SGI_GL_POINTS }, 1);
    wide_vertex(&f, 4.25, 3.75);
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    g_assert_cmpuint(frag_count(&f, 0xff0000), ==, 0);

    frag_init(&f);
    f.c.point_size = 5;
    frag_command(&f, SGI_GL_OP_BEGIN, (uint32_t[]) { SGI_GL_POINTS }, 1);
    wide_vertex(&f, -0.25, 3.75);
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    g_assert_cmpuint(frag_count(&f, 0xffffff), ==, 0);
}

/* At (16.5,16.5), weights 1/2,1/4,1/4 produce S=2/13, not 1/4. */
static void test_texture_projective(void)
{
    TexturePixels pixels = { .pixel = {
        0xffffffff, 0, 0xffffffff, 0 } };
    SGIGLTexImage image = texture_image(&pixels);
    FragScene f;
    uint32_t triangle = SGI_GL_TRIANGLES;

    frag_init(&f);
    image.min_filter = image.mag_filter = SGI_GL_LINEAR;
    image.wrap_s = image.wrap_t = SGI_GL_CLAMP;
    f.rec.target.texture = &image;
    enable(&f.c, SGI_GL_TEXTURE_2D, true);
    texture_env(&f, SGI_GL_TEXTURE_ENV_MODE, SGI_GL_REPLACE, 0, 0, 0);
    frag_command(&f, SGI_GL_OP_SHADE_MODEL, (uint32_t[]) { SGI_GL_FLAT }, 1);
    frag_command(&f, SGI_GL_OP_BEGIN, &triangle, 1);
    texture_vertex(&f, -63.0f / 64, -63.0f / 64, 1, 0, 0.25f, 1);
    texture_vertex(&f, 65.0f / 64, -63.0f / 64, 2, 1, 0.5f, 2);
    texture_vertex(&f, -63.0f / 64, 65.0f / 64, 4, 0, 0.25f, 1);
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    /* Linear CLAMP white/black with black border: 1/2+2*(2/13). */
    g_assert_cmphex(frag_pixel(&f, 16, 16), ==,
                   texture_rgb(21.0 / 26, 21.0 / 26, 21.0 / 26));
}

static void test_texture_fog_order(void)
{
    TexturePixels pixels = { .pixel = {
        0x0000ffff, 0x0000ffff, 0x0000ffff, 0x0000ffff } };
    SGIGLTexImage image = texture_image(&pixels);
    FragScene f;

    frag_init(&f);
    f.rec.target.texture = &image;
    enable(&f.c, SGI_GL_TEXTURE_2D, true);
    texture_env(&f, SGI_GL_TEXTURE_ENV_MODE, SGI_GL_REPLACE, 0, 0, 0);
    texture_coord(&f, 0.25f, 0.25f, 0, 1);
    frag_fog(&f, SGI_GL_FOG_MODE, SGI_GL_LINEAR, 0, 0, 0);
    frag_fog(&f, SGI_GL_FOG_END, 1, 0, 0, 0);
    frag_fog(&f, SGI_GL_FOG_COLOR, 1, 0, 0, 0);
    enable(&f.c, SGI_GL_FOG, true);
    frag_quad_at(&f, 0, 1, 0, 1, -0.5f);
    g_assert_cmphex(frag_pixel(&f, 16, 16), ==, 0x800080);
}

/* Each scene renders many fragments, so repeated reports are detectable. */
static FragScene texture_error_scene;
static SGIGLTexImage texture_error_image;
static TexturePixels texture_error_pixels;

static void texture_error_draw(void)
{
    FragScene *f = &texture_error_scene;

    frag_command(f, SGI_GL_OP_COLOR4F,
                 (uint32_t[]) { bits(1), 0, bits(1), bits(1) }, 4);
    frag_command(f, SGI_GL_OP_BEGIN, (uint32_t[]) { SGI_GL_TRIANGLES }, 1);
    frag_command(f, SGI_GL_OP_VERTEX2F, (uint32_t[]) { bits(-1), bits(-1) }, 2);
    frag_command(f, SGI_GL_OP_VERTEX2F, (uint32_t[]) { bits(3), bits(-1) }, 2);
    frag_command(f, SGI_GL_OP_VERTEX2F, (uint32_t[]) { bits(-1), bits(3) }, 2);
    frag_command(f, SGI_GL_OP_END, NULL, 0);
}

static void texture_error_init(void)
{
    frag_init(&texture_error_scene);
    texture_error_pixels = (TexturePixels) {
        .pixel = { 0xff0000ff, 0xff0000ff, 0xff0000ff, 0xff0000ff },
    };
    texture_error_image = texture_image(&texture_error_pixels);
    texture_error_scene.rec.target.texture = &texture_error_image;
    enable(&texture_error_scene.c, SGI_GL_TEXTURE_2D, true);
    texture_coord(&texture_error_scene, 0.25f, 0.25f, 0, 1);
}

static void texture_error_assert(const char *reason, uint32_t colour)
{
    g_autofree char *log = logged_by(texture_error_draw);
    const char *at = strstr(log, reason);

    g_assert_nonnull(at);
    g_assert_null(strstr(at + strlen(reason), reason));
    g_assert_cmphex(frag_pixel(&texture_error_scene, 16, 16), ==, colour);
}

static void test_texture_errors(void)
{
    for (int incomplete = 0; incomplete < 6; incomplete++) {
        texture_error_init();
        switch (incomplete) {
        case 0:
            texture_error_scene.rec.target.texture = NULL;
            break;
        case 1:
            texture_error_image.width = 0;
            break;
        case 2:
            texture_error_image.height = 3;
            break;
        case 3:
            texture_error_image.width = UINT32_MAX;
            break;
        case 4:
            texture_error_image.read_texel = NULL;
            break;
        case 5:
            texture_error_image.width = 0x80000000u;
            break;
        }
        texture_error_assert("incomplete texture image", 0xff00ff);
        g_assert_cmpuint(texture_error_pixels.reads, ==, 0);
    }
    for (int unsupported = 0; unsupported < 9; unsupported++) {
        texture_error_init();
        switch (unsupported) {
        case 0:
            texture_error_image.mag_filter = SGI_GL_LINEAR;
            break;
        case 1:
            texture_error_image.min_filter = SGI_GL_NEAREST_MIPMAP_LINEAR;
                break;
        case 2:
            texture_error_image.border = 1;
            break;
        case 3:
            texture_error_image.base_format = 0;
            break;
        case 4:
            texture_error_image.wrap_s = 0;
            break;
        case 5:
            texture_error_scene.c.cur.texture_env_mode = 0;
            break;
        case 6:
            texture_error_image.border_color[1] = NAN;
            break;
        case 7:
            texture_error_image.base_format = SGI_GL_INTENSITY;
                texture_error_scene.c.cur.texture_env_mode = SGI_GL_DECAL;
                break;
        case 8:
            texture_error_image.min_filter = texture_error_image.mag_filter =
                    SGI_GL_NEAREST_MIPMAP_LINEAR; break;
        }
        texture_error_assert("unsupported texture state", 0);
        g_assert_cmpuint(texture_error_pixels.reads, ==, 0);
    }
    texture_error_init();
    texture_error_pixels.fail = true;
    texture_error_assert("image-read failure", 0);
    g_assert_cmpuint(texture_error_pixels.reads, >, 1);
    for (int bad = 0; bad < 4; bad++) {
        texture_error_init();
        texture_coord(&texture_error_scene, bad == 0 ? NAN : 0.25f, 0.25f, 0,
                      bad == 1 ? 0 : bad == 2 ? -1 : bad == 3 ? INFINITY : 1);
        texture_error_assert("invalid clipped coordinates", 0);
        g_assert_cmpuint(texture_error_pixels.reads, ==, 0);
    }
    texture_error_init();
    enable(&texture_error_scene.c, SGI_GL_TEXTURE_2D, false);
    texture_error_image.border = 1;
    {
        g_autofree char *log = logged_by(texture_error_draw);
        g_assert_cmpstr(log, ==, "");
        g_assert_cmphex(frag_pixel(&texture_error_scene, 16, 16), ==, 0xff00ff);
        g_assert_cmpuint(texture_error_pixels.reads, ==, 0);
    }
    texture_error_init();
    sgi_gl_set_index_mode(&texture_error_scene.c, 8);
    texture_error_image.border = 1;
    {
        g_autofree char *log = logged_by(texture_error_draw);
        g_assert_cmpstr(log, ==, "");
        g_assert_cmpuint(texture_error_pixels.reads, ==, 0);
    }
}

static void test_texture_environment_without_image(void)
{
    static const uint32_t modes[] = {
        SGI_GL_REPLACE, SGI_GL_MODULATE, SGI_GL_DECAL, SGI_GL_BLEND,
    };

    texture_error_init();
    texture_error_scene.rec.target.texture = NULL;
    texture_error_scene.rec.target.store_colour = counting_store_colour;
    texture_error_scene.c.cur.texture_env_mode = 0;
    counted_stores = 0;
    texture_error_assert("unsupported texture state", 0);
    g_assert_cmpuint(counted_stores, ==, 0);
    g_assert_cmpuint(texture_error_pixels.reads, ==, 0);

    for (unsigned i = 0; i < ARRAY_SIZE(modes); i++) {
        texture_error_init();
        texture_error_scene.rec.target.texture = NULL;
        texture_error_scene.rec.target.store_colour = counting_store_colour;
        texture_error_scene.c.cur.texture_env_mode = modes[i];
        counted_stores = 0;
        texture_error_assert("incomplete texture image", 0xff00ff);
        g_assert_cmpuint(counted_stores, >, 0);
        g_assert_cmpuint(texture_error_pixels.reads, ==, 0);
    }
}

/* Points, lines, matrix mixing and a fresh descriptor at each binding. */
static void test_texture_points_lines_binding(void)
{
    TexturePixels first = { .pixel = {
        0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff } };
    TexturePixels second = { .pixel = {
        0x0000ffff, 0x0000ffff, 0x0000ffff, 0x0000ffff } };
    SGIGLTexImage image = texture_image(&first);
    SGIGLTexImage rebound = texture_image(&second);
    FragScene f;
    float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0,
                    2, 0, 1, 1, 0, 0, 0, 1 };

    frag_init(&f);
    f.rec.target.texture = &image;
    enable(&f.c, SGI_GL_TEXTURE_2D, true);
    texture_env(&f, SGI_GL_TEXTURE_ENV_MODE, SGI_GL_REPLACE, 0, 0, 0);
    texture_coord(&f, 0.5f, 0.5f, 0, 2);
    frag_command(&f, SGI_GL_OP_BEGIN, (uint32_t[]) { SGI_GL_POINTS }, 1);
    frag_command(&f, SGI_GL_OP_VERTEX2F, (uint32_t[]) { 0, 0 }, 2);
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    g_assert_cmphex(frag_pixel(&f, 32, 32), ==, 0xff0000);

    texture_coord(&f, 0.25f, 0.25f, 0.25f, 1);
    sgi_gl_set_matrix(&f.c, SGI_GL_TEXTURE, m);
    frag_command(&f, SGI_GL_OP_BEGIN, (uint32_t[]) { SGI_GL_POINTS }, 1);
    frag_command(&f, SGI_GL_OP_VERTEX2F, (uint32_t[]) { 0, 0 }, 2);
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    g_assert_cmphex(frag_pixel(&f, 32, 32), ==, 0x00ff00);

    sgi_gl_init(&f.c, 0x1234);
    context_init(&f.c);
    enable(&f.c, SGI_GL_TEXTURE_2D, true);
    texture_env(&f, SGI_GL_TEXTURE_ENV_MODE, SGI_GL_REPLACE, 0, 0, 0);
    frag_command(&f, SGI_GL_OP_BEGIN, (uint32_t[]) { SGI_GL_LINES }, 1);
    texture_vertex(&f, -0.75f, -0.484375f, 1, 0.5f, 0.5f, 2);
    texture_vertex(&f, 0.75f, -0.484375f, 2, 0.5f, 0.5f, 2);
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    g_assert_cmphex(frag_pixel(&f, 32, 16), ==, 0xff0000);

    f.rec.target.texture = &rebound;
    first.fail = true;
    texture_coord(&f, 0.25f, 0.25f, 0, 1);
    frag_quad(&f, 1, 1, 1, 1);
    g_assert_cmphex(frag_pixel(&f, 16, 16), ==, 0x0000ff);
    g_assert_cmpuint(second.reads, >, 0);
}

/* A behind-eye input still reaches clipping and leaves a visible trapezoid. */
static void test_texture_clips_behind_eye(void)
{
    const float rows[4][4] = {
        { 16, 0, -32, 0 }, { 0, 16, -32, 0 },
        { 0, 0, -1.5f, -1.5f }, { 0, 0, -1, 0 },
    };
    const float tri[3][3] = { { -2, -2, -2 }, { 2, -2, -2 }, { 0, 2, 1 } };
    TexturePixels pixels = { .pixel = {
        0xff0000ff, 0xff0000ff, 0xff0000ff, 0xff0000ff } };
    SGIGLTexImage image = texture_image(&pixels);
    FragScene f;

    frag_init(&f);
    f.rec.target.texture = &image;
    enable(&f.c, SGI_GL_TEXTURE_2D, true);
    texture_env(&f, SGI_GL_TEXTURE_ENV_MODE, SGI_GL_REPLACE, 0, 0, 0);
    texture_coord(&f, 0.25f, 0.25f, 0, 1);
    window_matrix(&f, rows, 1, false);
    window_draw(&f, SGI_GL_TRIANGLES, tri, 3);
    g_assert_cmphex(frag_pixel(&f, 32, 18), ==, 0xff0000);
    g_assert_cmphex(frag_pixel(&f, 32, 21), ==, 0);
}

/* Invalid Q in the final clipped vertex rejects the entire polygon. */
static void test_texture_invalid_clipped_polygon(void)
{
    TexturePixels pixels = { .pixel = {
        0xff0000ff, 0xff0000ff, 0xff0000ff, 0xff0000ff } };
    SGIGLTexImage image = texture_image(&pixels);
    FragScene f;

    frag_init(&f);
    f.rec.target.texture = &image;
    enable(&f.c, SGI_GL_TEXTURE_2D, true);
    texture_env(&f, SGI_GL_TEXTURE_ENV_MODE, SGI_GL_REPLACE, 0, 0, 0);
    frag_command(&f, SGI_GL_OP_BEGIN, (uint32_t[]) { SGI_GL_TRIANGLES }, 1);
    texture_vertex(&f, -2, -0.75f, 1, 0.25f, 0.25f, -4);
    texture_vertex(&f, 0.75f, -0.75f, 1, 0.25f, 0.25f, 10);
    texture_vertex(&f, 0.75f, 0.75f, 1, 0.25f, 0.25f, 1);
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    g_assert_cmpuint(frag_count(&f, 0xff0000), ==, 0);
    g_assert_cmpuint(pixels.reads, ==, 0);

    /* A line with one surviving invalid endpoint also writes nothing. */
    frag_command(&f, SGI_GL_OP_BEGIN, (uint32_t[]) { SGI_GL_LINES }, 1);
    texture_vertex(&f, -0.75f, -0.484375f, 1, 0.25f, 0.25f, 1);
    texture_vertex(&f, 0.75f, -0.484375f, 1, 0.25f, 0.25f, 0);
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    g_assert_cmpuint(frag_count(&f, 0xff0000), ==, 0);
    g_assert_cmpuint(pixels.reads, ==, 0);

    /* Invalid original Q may disappear in clipping; valid survivors draw. */
    frag_command(&f, SGI_GL_OP_BEGIN, (uint32_t[]) { SGI_GL_TRIANGLES }, 1);
    texture_vertex(&f, -2, -0.75f, 1, 0.25f, 0.25f, -1);
    texture_vertex(&f, 0.75f, -0.75f, 1, 0.25f, 0.25f, 10);
    texture_vertex(&f, 0.75f, 0.75f, 1, 0.25f, 0.25f, 10);
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    g_assert_cmpuint(frag_count(&f, 0xff0000), >, 0);
    g_assert_cmpuint(pixels.reads, >, 0);
}

/* A submitted quad or polygon is rejected before its first triangle draws. */
static void test_texture_invalid_quad_polygon(void)
{
    const uint32_t modes[] = {
        SGI_GL_QUADS, SGI_GL_QUAD_STRIP, SGI_GL_POLYGON,
    };
    const float corner[4][2] = {
        { -0.75f, -0.75f }, { 0.75f, -0.75f },
        { 0.75f, 0.75f }, { -0.75f, 0.75f },
    };
    TexturePixels pixels = { .pixel = {
        0xff0000ff, 0xff0000ff, 0xff0000ff, 0xff0000ff } };
    SGIGLTexImage image = texture_image(&pixels);
    FragScene f;

    for (unsigned m = 0; m < ARRAY_SIZE(modes); m++) {
        float v[4][SGI_GL_VERTEX_FLOATS] = { 0 };
        SGIGLDraw draw = {
            .mode = modes[m], .count = 4, .vertices = &v[0][0],
            .state = &f.c.cur,
        };

        frag_init(&f);
        pixels.reads = 0;
        f.rec.target.texture = &image;
        enable(&f.c, SGI_GL_TEXTURE_2D, true);
        texture_env(&f, SGI_GL_TEXTURE_ENV_MODE, SGI_GL_REPLACE, 0, 0, 0);
        for (int i = 0; i < 4; i++) {
            v[i][SGI_GL_V_POSITION] = corner[i][0];
            v[i][SGI_GL_V_POSITION + 1] = corner[i][1];
            v[i][SGI_GL_V_POSITION + 3] = 1;
            v[i][SGI_GL_V_TEXCOORD] = 0.25f;
            v[i][SGI_GL_V_TEXCOORD + 1] = 0.25f;
            v[i][SGI_GL_V_TEXCOORD + 3] = 1;
        }
        v[modes[m] == SGI_GL_QUAD_STRIP ? 2 : 3][SGI_GL_V_TEXCOORD + 3] = 0;
        sgi_soft_draw(f.rec.rules, &f.rec.target, &draw);
        g_assert_cmpuint(frag_count(&f, 0xff0000), ==, 0);
        g_assert_cmpuint(pixels.reads, ==, 0);
    }
}

/* Synthetic scalar planes exercise the shared interface, not board storage. */
typedef struct AlphaScene {
    FragScene f;               /* planes first, for existing depth hooks */
    SGIRasterRules rules;
    SGIRasterColour pixel[2];
    unsigned stores, reads;
    int failed_target;
} AlphaScene;

static void alpha_store(void *opaque, SGIColourTarget target, int x, int y,
                         const SGIRasterColour *colour)
{
    AlphaScene *s = opaque;

    s->pixel[target] = *colour;
    s->stores++;
}

static bool alpha_load(void *opaque, SGIColourTarget target, int x, int y,
                        SGIRasterColour *colour)
{
    AlphaScene *s = opaque;

    s->reads++;
    *colour = s->pixel[target];
    return target != s->failed_target;
}

static void alpha_init(AlphaScene *s)
{
    memset(s, 0, sizeof(*s));
    frag_init(&s->f);
    s->failed_target = -1;
    s->rules = rules_stencil;
    s->rules.alpha_bits = 8;
    s->f.rec.rules = &s->rules;
    s->f.rec.target.opaque = s;
    s->f.rec.target.store_rgba = alpha_store;
    s->f.rec.target.load_rgba = alpha_load;
    s->f.c.cur.dither = false;
}

static void alpha_point(AlphaScene *s, unsigned r, unsigned g, unsigned b,
                         double a)
{
    frag_command(&s->f, SGI_GL_OP_COLOR4F,
                 (uint32_t[]) { bits(r / 255.0), bits(g / 255.0),
                                bits(b / 255.0), bits(a) }, 4);
    frag_command(&s->f, SGI_GL_OP_BEGIN, (uint32_t[]) { SGI_GL_POINTS }, 1);
    wide_vertex(&s->f, 32.5, 32.5);
    frag_command(&s->f, SGI_GL_OP_END, NULL, 0);
}

static void test_polygon_stipple_rgba_texture(void)
{
    AlphaScene s;
    TexturePixels pixels = { .pixel = {
        0xff000040, 0xff000040, 0xff000040, 0xff000040 } };
    SGIGLTexImage image = texture_image(&pixels);
    uint32_t rows[32] = { 0 };

    alpha_init(&s);
    s.f.c.cur.drawable.clip.r[0] = (SGIClipRect) {32, 32, 32, 32};
    s.pixel[0] = (SGIRasterColour) { {10, 20, 30, 40} };
    s.f.c.cur.colour_mask = SGI_GL_COLOUR_MASK_ALPHA;
    s.f.c.cur.blend = true;
    s.f.c.cur.blend_src = s.f.c.cur.blend_dst = 1;
    s.f.rec.target.texture = &image;
    enable(&s.f.c, SGI_GL_TEXTURE_2D, true);
    g_assert_true(sgi_gl_polygon_stipple(&s.f.c, rows));
    enable(&s.f.c, SGI_GL_POLYGON_STIPPLE, true);
    frag_quad(&s.f, 1, 1, 1, 1);
    g_assert_cmpuint(s.stores, ==, 0);
    g_assert_cmpuint(s.reads, ==, 0);
    g_assert_cmpuint(pixels.reads, ==, 0);
    g_assert_cmpuint(s.pixel[0].component[3], ==, 40);
    rows[0] = 1;
    g_assert_true(sgi_gl_polygon_stipple(&s.f.c, rows));
    frag_quad(&s.f, 1, 1, 1, 1);
    g_assert_cmpuint(s.stores, ==, 1);
    g_assert_cmpuint(s.reads, ==, 1);
    g_assert_cmpuint(pixels.reads, >, 0);
    g_assert_cmpuint(s.pixel[0].component[0], ==, 10);
    g_assert_cmpuint(s.pixel[0].component[3], ==, 104);
}

static void test_alpha_storage(void)
{
    AlphaScene s;

    alpha_init(&s);
    alpha_point(&s, 255, 0, 0, 64.0 / 255);
    g_assert_cmpuint(s.stores, ==, 1);
    g_assert_cmpuint(s.pixel[0].component[3], ==, 64);
    alpha_point(&s, 255, 0, 0, 0.5);
    g_assert_cmpuint(s.pixel[0].component[3], ==, 128);
    s.rules.color_bits = 10;
    alpha_point(&s, 255, 0, 0, 0.5);
    g_assert_cmpuint(s.pixel[0].component[0], ==, 1023);
    g_assert_cmpuint(s.pixel[0].component[3], ==, 127);
}

static void test_alpha_blend_masks(void)
{
    static const struct {
        uint32_t equation, src, dst;
        uint32_t expected[4];
    } cases[] = {
        { SGI_GL_FUNC_ADD, 1, 0, {255, 0, 0, 64} },
        { SGI_GL_FUNC_ADD, SGI_GL_DST_ALPHA, 0, {128, 0, 0, 32} },
        { SGI_GL_FUNC_ADD, SGI_GL_SRC_ALPHA, SGI_GL_ONE_MINUS_SRC_ALPHA,
          {64, 0, 191, 112} },
        { SGI_GL_FUNC_ADD, SGI_GL_SRC_ALPHA_SATURATE, 1,
          {64, 0, 255, 192} },
        { SGI_GL_MIN, 1, 1, {0, 0, 0, 64} },
        { SGI_GL_MAX, 1, 1, {255, 0, 255, 128} },
        { SGI_GL_FUNC_SUBTRACT, 1, 1, {255, 0, 0, 0} },
        { SGI_GL_FUNC_REVERSE_SUBTRACT, 1, 1,
          {0, 0, 255, 64} },
    };
    AlphaScene s;

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        alpha_init(&s);
        s.pixel[0] = (SGIRasterColour) { {0, 0, 255, 128} };
        s.f.c.cur.blend = true;
        s.f.c.cur.blend_equation = cases[i].equation;
        s.f.c.cur.blend_src = cases[i].src;
        s.f.c.cur.blend_dst = cases[i].dst;
        alpha_point(&s, 255, 0, 0, 64.0 / 255);
        g_assert_cmpmem(s.pixel[0].component, sizeof(cases[i].expected),
                        cases[i].expected, sizeof(cases[i].expected));
    }
    for (unsigned alpha_only = 0; alpha_only < 2; alpha_only++) {
        uint32_t expected[4] = { alpha_only ? 10 : 200,
                                alpha_only ? 20 : 150,
                                alpha_only ? 30 : 100,
                                alpha_only ? 90 : 40 };

        alpha_init(&s);
        s.pixel[0] = (SGIRasterColour) { {10, 20, 30, 40} };
        s.f.c.cur.colour_mask = alpha_only ? SGI_GL_COLOUR_MASK_ALPHA : 7;
        alpha_point(&s, 200, 150, 100, 90.0 / 255);
        g_assert_cmpmem(s.pixel[0].component, sizeof(expected),
                        expected, sizeof(expected));
    }
    alpha_init(&s);
    s.pixel[0].component[3] = 0xaa;
    s.f.c.cur.color_logic_op = true;
    s.f.c.cur.blend = true;     /* Logic supersedes blending. */
    s.f.c.cur.logic_op_mode = SGI_GL_CLEAR + 6;
    alpha_point(&s, 0, 0, 0, 0x55 / 255.0);
    g_assert_cmpuint(s.pixel[0].component[3], ==, 0xff);

    alpha_init(&s);
    s.f.c.cur.draw_buffers = SGI_GL_DRAW_FRONT | SGI_GL_DRAW_BACK;
    s.pixel[0].component[3] = 64;
    s.pixel[1].component[3] = 128;
    s.f.c.cur.blend = true;
    s.f.c.cur.blend_src = SGI_GL_DST_ALPHA;
    s.f.c.cur.blend_dst = 0;
    alpha_point(&s, 255, 0, 0, 1);
    g_assert_cmpuint(s.pixel[0].component[0], ==, 64);
    g_assert_cmpuint(s.pixel[1].component[0], ==, 128);
    g_assert_cmpuint(s.stores, ==, 2);
}

static void alpha_clear(AlphaScene *s, double alpha)
{
    frag_command(&s->f, SGI_GL_OP_CLEAR_COLOR,
                 (uint32_t[]) {0, 0, bits(1), bits(alpha)}, 4);
    frag_command(&s->f, SGI_GL_OP_CLEAR,
                 (uint32_t[]) {SGI_GL_COLOR_BUFFER_BIT}, 1);
}

static void test_alpha_clear_precision(void)
{
    AlphaScene s;

    alpha_init(&s);
    frag_scissor(&s.f, 32, 32, 1, 1);
    enable(&s.f.c, SGI_GL_SCISSOR_TEST, true);
    s.f.c.cur.drawable.clip = (SGIClipList) {
        .n = 2, .r = { {0, 0, 32, 63}, {34, 0, 63, 63} },
    };
    s.f.c.cur.blend = s.f.c.cur.alpha_test = true;
    s.f.c.cur.alpha_func = SGI_GL_NEVER;
    alpha_clear(&s, 0);
    g_assert_cmpuint(s.stores, ==, 1);
    g_assert_cmpuint(s.pixel[0].component[2], ==, 255);
    g_assert_cmpuint(s.pixel[0].component[3], ==, 0);
    alpha_clear(&s, 1);
    g_assert_cmpuint(s.pixel[0].component[3], ==, 255);
    s.f.c.cur.colour_mask = SGI_GL_COLOUR_MASK_ALPHA;
    s.pixel[0] = (SGIRasterColour) { {10, 20, 30, 40} };
    alpha_clear(&s, 0);
    g_assert_cmpuint(s.pixel[0].component[0], ==, 10);
    g_assert_cmpuint(s.pixel[0].component[2], ==, 30);
    g_assert_cmpuint(s.pixel[0].component[3], ==, 0);
    alpha_clear(&s, 0.5);
    g_assert_cmpuint(s.pixel[0].component[3], ==, 128);
    s.rules.color_bits = 10;
    alpha_clear(&s, 0.5);
    g_assert_cmpuint(s.pixel[0].component[3], ==, 127);

    alpha_init(&s);
    s.rules.alpha_bits = 1;
    s.f.c.cur.alpha_test = true;
    s.f.c.cur.alpha_func = SGI_GL_GREATER;
    s.f.c.cur.alpha_ref = 0.4;
    alpha_point(&s, 255, 0, 0, 0.5);
    g_assert_cmpuint(s.stores, ==, 1);
    g_assert_cmpuint(s.pixel[0].component[3], ==, 0);
    s.rules.alpha_bits = 10;
    alpha_point(&s, 255, 0, 0, 0.5);
    g_assert_cmpuint(s.pixel[0].component[3], ==, 512);
    s.rules.color_bits = s.rules.alpha_bits = 1;
    alpha_point(&s, 255, 0, 0, 0.5);
    g_assert_cmpuint(s.stores, ==, 3); /* Internal alpha precision is two. */
    s.rules.alpha_bits = 0;
    s.pixel[0].component[3] = UINT32_MAX; /* Absent A must be ignored. */
    s.f.c.cur.blend = true;
    s.f.c.cur.blend_src = SGI_GL_DST_ALPHA;
    s.f.c.cur.blend_dst = 0;
    alpha_point(&s, 255, 0, 0, 0.5);
    g_assert_cmpuint(s.pixel[0].component[0], ==, 1);
    g_assert_cmpuint(s.pixel[0].component[3], ==, 0);
    s.f.c.cur.colour_mask = SGI_GL_COLOUR_MASK_ALPHA;
    alpha_point(&s, 255, 0, 0, 1);
    g_assert_cmpuint(s.stores, ==, 4);
}

static void alpha_depth_stencil(AlphaScene *s)
{
    s->f.c.cur.depth_test = true;
    s->f.c.cur.depth_func = SGI_GL_ALWAYS;
    s->f.c.cur.stencil_test = true;
    s->f.c.cur.stencil_op[2] = SGI_GL_INCR;
}

static AlphaScene alpha_error_scene;

static void alpha_failed_square(void)
{
    alpha_point(&alpha_error_scene, 255, 0, 0, 1);
}

static void test_alpha_read_failures(void)
{
    AlphaScene s;
    g_autofree char *log = NULL;
    const char *report;

    alpha_init(&s);
    alpha_depth_stencil(&s);
    s.f.c.cur.draw_buffers = SGI_GL_DRAW_FRONT | SGI_GL_DRAW_BACK;
    s.f.c.cur.colour_mask = SGI_GL_COLOUR_MASK_ALPHA;
    s.failed_target = 1;
    alpha_point(&s, 255, 0, 0, 1);
    g_assert_cmpuint(s.reads, ==, 2);
    g_assert_cmpuint(s.stores, ==, 0);
    g_assert_cmpuint(s.f.p.depth[planes_index(32, 32)], ==, 0);
    g_assert_cmpuint(s.f.p.stencil[planes_index(32, 32)], ==, 0);
    s.f.rec.target.load_rgba = NULL;
    alpha_point(&s, 255, 0, 0, 1);
    g_assert_cmpuint(s.stores, ==, 0);
    g_assert_cmpuint(s.f.p.depth[planes_index(32, 32)], ==, 0);
    g_assert_cmpuint(s.f.p.stencil[planes_index(32, 32)], ==, 0);
    frag_scissor(&s.f, 32, 32, 1, 1);
    enable(&s.f.c, SGI_GL_SCISSOR_TEST, true);
    frag_command(&s.f, SGI_GL_OP_CLEAR,
                 (uint32_t[]) {SGI_GL_COLOR_BUFFER_BIT |
                               SGI_GL_DEPTH_BUFFER_BIT |
                               SGI_GL_STENCIL_BUFFER_BIT}, 1);
    g_assert_cmpuint(s.f.p.depth[planes_index(32, 32)], ==, 0);
    s.f.rec.target.load_rgba = alpha_load;
    frag_command(&s.f, SGI_GL_OP_CLEAR,
                 (uint32_t[]) {SGI_GL_COLOR_BUFFER_BIT |
                               SGI_GL_DEPTH_BUFFER_BIT |
                               SGI_GL_STENCIL_BUFFER_BIT}, 1);
    g_assert_cmpuint(s.stores, ==, 0);
    g_assert_cmpuint(s.f.p.depth[planes_index(32, 32)], ==, 0);

    alpha_init(&s);
    alpha_depth_stencil(&s);
    s.f.rec.target.store_rgba = NULL;
    alpha_point(&s, 255, 0, 0, 1);
    g_assert_cmpuint(s.f.p.depth[planes_index(32, 32)], ==, 0);
    alpha_init(&s);
    alpha_depth_stencil(&s);
    s.f.c.cur.colour_mask = 0;
    s.f.rec.target.load_rgba = NULL;
    alpha_point(&s, 255, 0, 0, 1);
    g_assert_cmpuint(s.stores, ==, 0);
    g_assert_cmpuint(s.f.p.depth[planes_index(32, 32)], ==, 8388608);
    g_assert_cmpuint(s.f.p.stencil[planes_index(32, 32)], ==, 1);
    alpha_init(&s);
    alpha_depth_stencil(&s);
    s.f.c.cur.colour_mask = SGI_GL_COLOUR_MASK_ALPHA;
    s.pixel[0].component[0] = 256; /* Successful hook, invalid component. */
    alpha_point(&s, 255, 0, 0, 1);
    g_assert_cmpuint(s.stores, ==, 0);
    g_assert_cmpuint(s.f.p.depth[planes_index(32, 32)], ==, 0);
    alpha_init(&alpha_error_scene);
    alpha_error_scene.f.c.point_size = 2;
    alpha_error_scene.f.c.cur.colour_mask = SGI_GL_COLOUR_MASK_ALPHA;
    alpha_error_scene.failed_target = 0;
    log = logged_by(alpha_failed_square);
    report = strstr(log, "RGBA destination read failed");
    g_assert_nonnull(report);
    g_assert_null(strstr(report + 1, "RGBA destination read failed"));
    g_assert_cmpuint(alpha_error_scene.reads, ==, 4);
    g_assert_cmpuint(alpha_error_scene.stores, ==, 0);

}

/* DRAW_NONE removes colour work, not independently enabled plane writes. */
static void test_alpha_zero_targets(void)
{
    AlphaScene s;
    const uint8_t byte = 0x80;

    for (unsigned operation = 0; operation < 3; operation++) {
        for (unsigned blend = 0; blend < 2; blend++) {
            alpha_init(&s);
            alpha_depth_stencil(&s);
            s.f.c.cur.draw_buffers = 0;
            s.f.rec.target.load_rgba = NULL;
            s.f.c.cur.colour_mask = SGI_GL_COLOUR_MASK_ALPHA;
            s.f.c.cur.blend = blend;
            if (operation == 0) {
                alpha_point(&s, 255, 0, 0, 1);
            } else {
                SGIRasterOp op = {
                    .kind = operation == 1 ? SGI_RASTER_BITMAP :
                                             SGI_RASTER_CLEAR,
                    .ntargets = 0,
                    .x0 = 32, .y0 = 32, .x1 = 32, .y1 = 32,
                    .clip = { .n = 1, .r = { {32, 32, 32, 32} } },
                    .clear_buffers = SGI_GL_COLOR_BUFFER_BIT |
                                     SGI_GL_DEPTH_BUFFER_BIT |
                                     SGI_GL_STENCIL_BUFFER_BIT,
                    .clear_colour_mask = SGI_GL_COLOUR_MASK_ALPHA,
                    .clear_depth24 = 17, .clear_stencil = 9,
                    .stencil_mask = 255,
                    .bitmap = {
                        .state = &s.f.c.cur, .x = 32, .y = 32,
                        .width = 1, .height = 1, .count = 1,
                        .bytes = &byte, .z = 0.5, .rgba = {1, 0, 0, 1},
                    },
                };

                sgi_soft_raster(&s.rules, &s.f.rec.target, &op);
            }
            g_assert_cmpuint(s.stores, ==, 0);
            g_assert_cmpuint(s.reads, ==, 0);
            g_assert_cmpuint(s.f.p.depth[planes_index(32, 32)], ==,
                            operation == 2 ? 17 : 8388608);
            g_assert_cmpuint(s.f.p.stencil[planes_index(32, 32)], ==,
                            operation == 2 ? 9 : 1);
        }
    }
    /* Bitmap selection comes from the op, independently of DRAW_NONE. */
    alpha_init(&s);
    alpha_depth_stencil(&s);
    s.f.c.cur.draw_buffers = 0;
    s.f.c.cur.colour_mask = SGI_GL_COLOUR_MASK_ALPHA;
    s.f.rec.target.load_rgba = NULL;
    {
        SGIRasterOp op = {
            .kind = SGI_RASTER_BITMAP, .ntargets = 1,
            .bitmap = {
                .state = &s.f.c.cur, .x = 32, .y = 32,
                .width = 1, .height = 1, .count = 1,
                .bytes = &byte, .z = 0.5, .rgba = {1, 0, 0, 1},
            },
        };

        sgi_soft_raster(&s.rules, &s.f.rec.target, &op);
    }
    g_assert_cmpuint(s.stores, ==, 0);
    g_assert_cmpuint(s.f.p.depth[planes_index(32, 32)], ==, 0);
    g_assert_cmpuint(s.f.p.stencil[planes_index(32, 32)], ==, 0);
    /* Absent colour targets do not make a missing alpha store consistent. */
    alpha_init(&s);
    alpha_depth_stencil(&s);
    s.f.c.cur.draw_buffers = 0;
    s.f.rec.target.store_rgba = NULL;
    s.f.rec.target.load_rgba = NULL;
    alpha_point(&s, 255, 0, 0, 1);
    g_assert_cmpuint(s.f.p.depth[planes_index(32, 32)], ==, 0);
}

static void test_alpha_texture_fog_rejection(void)
{
    TexturePixels texels = { .pixel = {
        0xff000040, 0xff000040, 0xff000040, 0xff000040 } };
    SGIGLTexImage image = texture_image(&texels);
    AlphaScene s;

    alpha_init(&s);
    s.f.rec.target.texture = &image;
    enable(&s.f.c, SGI_GL_TEXTURE_2D, true);
    texture_env(&s.f, SGI_GL_TEXTURE_ENV_MODE, SGI_GL_REPLACE, 0, 0, 0);
    texture_coord(&s.f, 0.25, 0.25, 0, 1);
    s.f.c.cur.fog = true;
    s.f.c.cur.fog_mode = SGI_GL_LINEAR;
    s.f.c.cur.fog_start = -1;
    s.f.c.cur.fog_end = 1;
    s.f.c.cur.fog_color[2] = 1;
    alpha_point(&s, 255, 255, 255, 1);
    g_assert_cmpuint(s.pixel[0].component[0], ==, 128);
    g_assert_cmpuint(s.pixel[0].component[2], ==, 128);
    g_assert_cmpuint(s.pixel[0].component[3], ==, 64);
    alpha_depth_stencil(&s);
    s.f.c.cur.alpha_test = true;
    s.f.c.cur.alpha_func = SGI_GL_GREATER;
    s.f.c.cur.alpha_ref = 0.5;
    alpha_point(&s, 255, 255, 255, 1);
    g_assert_cmpuint(s.stores, ==, 1);
    g_assert_cmpuint(s.f.p.depth[planes_index(32, 32)], ==, 0);
    g_assert_cmpuint(s.f.p.stencil[planes_index(32, 32)], ==, 0);
    s.f.c.cur.alpha_test = false;
    s.f.c.cur.depth_func = SGI_GL_NEVER;
    alpha_point(&s, 255, 255, 255, 1);
    g_assert_cmpuint(s.stores, ==, 1);
    s.f.c.cur.depth_test = false;
    s.f.c.cur.stencil_func = SGI_GL_NEVER;
    alpha_point(&s, 255, 255, 255, 1);
    g_assert_cmpuint(s.stores, ==, 1);
}

static void polygon_mode_vertices(float v[][SGI_GL_VERTEX_FLOATS],
                                  const float xy[][2], unsigned n)
{
    memset(v, 0, n * SGI_GL_VERTEX_FLOATS * sizeof(float));
    for (unsigned i = 0; i < n; i++) {
        uint32_t edge = 1;

        v[i][SGI_GL_V_POSITION] = xy[i][0] / 32 - 1;
        v[i][SGI_GL_V_POSITION + 1] = xy[i][1] / 32 - 1;
        v[i][SGI_GL_V_POSITION + 3] = 1;
        v[i][SGI_GL_V_TEXCOORD + 3] = 1;
        for (unsigned k = 0; k < 4; k++) {
            v[i][SGI_GL_V_COLOR + k] = 1;
            v[i][SGI_GL_V_BACK_COLOR + k] = 1;
        }
        memcpy(&v[i][SGI_GL_V_EDGE], &edge, sizeof(edge));
    }
}

static void polygon_mode_draw(FragScene *f, float v[][SGI_GL_VERTEX_FLOATS],
                              unsigned n, uint32_t primitive,
                              uint32_t front, uint32_t back)
{
    uint32_t modes[2] = { front, back };
    SGIGLDraw d = {
        .mode = primitive, .count = n, .vertices = &v[0][0],
        .state = &f->c.cur, .polygon_modes = modes,
        .line_width = f->c.line_width, .point_size = f->c.point_size,
    };

    sgi_soft_draw(f->rec.rules, &f->rec.target, &d);
}

static const float polygon_mode_square[4][2] = {
    { 8.5, 8.5 }, { 24.5, 8.5 }, { 24.5, 24.5 }, { 8.5, 24.5 },
};

static void test_polygon_mode_boundary(void)
{
    FragScene f;
    float v[4][SGI_GL_VERTEX_FLOATS];

    frag_init(&f);
    polygon_mode_vertices(v, polygon_mode_square, 4);
    polygon_mode_draw(&f, v, 4, SGI_GL_POLYGON, SGI_GL_LINE, SGI_GL_LINE);
    g_assert_cmphex(frag_pixel(&f, 16, 16), ==, 0);
    g_assert_cmphex(frag_pixel(&f, 16, 8), ==, 0xffffff);
    g_assert_cmphex(frag_pixel(&f, 8, 16), ==, 0xffffff);
    memset(&f.p, 0, sizeof(f.p));
    polygon_mode_draw(&f, v, 4, SGI_GL_QUADS, SGI_GL_LINE, SGI_GL_LINE);
    g_assert_cmphex(frag_pixel(&f, 16, 16), ==, 0);
    g_assert_cmphex(frag_pixel(&f, 16, 8), ==, 0xffffff);
}

static void test_polygon_mode_edge_flags(void)
{
    FragScene f;
    float v[4][SGI_GL_VERTEX_FLOATS];
    uint32_t off = 0;

    frag_init(&f);
    polygon_mode_vertices(v, polygon_mode_square, 4);
    memcpy(&v[0][SGI_GL_V_EDGE], &off, sizeof(off));
    polygon_mode_draw(&f, v, 4, SGI_GL_POLYGON, SGI_GL_POINT, SGI_GL_POINT);
    g_assert_cmphex(frag_pixel(&f, 8, 8), ==, 0);
    g_assert_cmphex(frag_pixel(&f, 24, 8), ==, 0xffffff);
    g_assert_cmpuint(frag_count(&f, 0xffffff), ==, 3);
    memset(&f.p, 0, sizeof(f.p));
    polygon_mode_draw(&f, v, 4, SGI_GL_POLYGON, SGI_GL_LINE, SGI_GL_LINE);
    g_assert_cmphex(frag_pixel(&f, 16, 8), ==, 0);
    g_assert_cmphex(frag_pixel(&f, 24, 16), ==, 0xffffff);
}

static void test_polygon_mode_client_boundary(void)
{
    FragScene f;
    float v[4][SGI_GL_VERTEX_FLOATS];
    uint32_t off = 0;

    frag_init(&f);
    polygon_mode_vertices(v, polygon_mode_square, 4);
    for (unsigned i = 0; i < 4; i++) {
        memcpy(&v[i][SGI_GL_V_EDGE], &off, sizeof(off));
    }
    f.c.cur.clip_planes = 1;
    f.c.cur.clip_plane[0][0] = 1;
    f.c.cur.clip_plane[0][3] = 1 - 16.5f / 32;
    polygon_mode_draw(&f, v, 4, SGI_GL_POLYGON, SGI_GL_LINE, SGI_GL_LINE);
    g_assert_cmphex(frag_pixel(&f, 16, 16), ==, 0xffffff);
    g_assert_cmphex(frag_pixel(&f, 24, 16), ==, 0);
    g_assert_cmphex(frag_pixel(&f, 20, 8), ==, 0);
}

static void test_polygon_mode_facing(void)
{
    const float rows[4][4] = {
        { 32, 0, 0, 32 }, { 0, 32, 0, 32 },
        { 0, 0, 0.5, 0.5 }, { 0, 0, 0, 1 },
    };

    for (unsigned down = 0; down < 2; down++) {
        for (unsigned cw = 0; cw < 2; cw++) {
            for (unsigned reverse = 0; reverse < 2; reverse++) {
                FragScene f;
                float v[4][SGI_GL_VERTEX_FLOATS];
                float xy[4][2];
                bool front = !(down ^ cw ^ reverse);

                frag_init(&f);
                for (unsigned i = 0; i < 4; i++) {
                    memcpy(xy[i], polygon_mode_square[reverse ? 3 - i : i],
                           sizeof(xy[i]));
                }
                polygon_mode_vertices(v, xy, 4);
                window_matrix(&f, rows, 1, down);
                f.c.cur.front_face = cw ? SGI_GL_CW : SGI_GL_CCW;
                polygon_mode_draw(&f, v, 4, SGI_GL_POLYGON,
                                  SGI_GL_LINE, SGI_GL_POINT);
                g_assert_cmphex(frag_pixel(&f, 16, 8), ==,
                                front ? 0xffffff : 0);
                memset(&f.p, 0, sizeof(f.p));
                f.c.cur.cull_face = true;
                f.c.cur.cull_face_mode = SGI_GL_FRONT;
                polygon_mode_draw(&f, v, 4, SGI_GL_POLYGON,
                                  SGI_GL_LINE, SGI_GL_POINT);
                g_assert_cmpuint(frag_count(&f, 0xffffff), ==, front ? 0 : 4);
            }
        }
    }
}

static void test_polygon_mode_zero_area(void)
{
    const float xy[3][2] = { { 8.5, 8.5 }, { 16.5, 8.5 }, { 24.5, 8.5 } };

    for (unsigned cw = 0; cw < 2; cw++) {
        FragScene f;
        float v[3][SGI_GL_VERTEX_FLOATS];

        frag_init(&f);
        polygon_mode_vertices(v, xy, 3);
        f.c.cur.front_face = cw ? SGI_GL_CW : SGI_GL_CCW;
        polygon_mode_draw(&f, v, 3, SGI_GL_POLYGON,
                          SGI_GL_LINE, SGI_GL_POINT);
        g_assert_cmpuint(frag_count(&f, 0xffffff), ==, 3);
        f.c.cur.depth_test = true;
        f.c.cur.depth_func = SGI_GL_ALWAYS;
        f.c.cur.polygon_offset = SGI_GL_OFFSET_POINT;
        f.c.cur.offset_factor = 1000;
        f.c.cur.offset_units = 2;
        polygon_mode_draw(&f, v, 3, SGI_GL_POLYGON,
                          SGI_GL_LINE, SGI_GL_POINT);
        g_assert_cmpuint(f.p.depth[planes_index(8, 8)], ==, 8388610);
        memset(&f.p, 0, sizeof(f.p));
        f.c.cur.cull_face = true;
        f.c.cur.cull_face_mode = SGI_GL_BACK;
        polygon_mode_draw(&f, v, 3, SGI_GL_POLYGON,
                          SGI_GL_LINE, SGI_GL_POINT);
        g_assert_cmpuint(frag_count(&f, 0xffffff), ==, 0);
    }
}

static void test_polygon_mode_strip_edges(void)
{
    const uint32_t primitives[] = {
        SGI_GL_TRIANGLE_STRIP, SGI_GL_TRIANGLE_FAN, SGI_GL_QUAD_STRIP,
    };
    const float strip[4][2] = {
        { 8.5, 8.5 }, { 24.5, 8.5 }, { 8.5, 24.5 }, { 24.5, 24.5 },
    };
    uint32_t off = 0;

    for (unsigned m = 0; m < G_N_ELEMENTS(primitives); m++) {
        FragScene f;
        float v[4][SGI_GL_VERTEX_FLOATS];

        frag_init(&f);
        polygon_mode_vertices(v, primitives[m] == SGI_GL_TRIANGLE_FAN ?
                              polygon_mode_square : strip, 4);
        for (unsigned i = 0; i < 4; i++) {
            memcpy(&v[i][SGI_GL_V_EDGE], &off, sizeof(off));
        }
        polygon_mode_draw(&f, v, 4, primitives[m], SGI_GL_LINE, SGI_GL_LINE);
        g_assert_cmphex(frag_pixel(&f, 16, 8), ==, 0xffffff);
        g_assert_cmphex(frag_pixel(&f, 16, 16), ==,
                        primitives[m] == SGI_GL_QUAD_STRIP ? 0 : 0xffffff);
    }
}

static void test_polygon_mode_volume_boundary(void)
{
    const float xy[4][2] = {
        { -8.5, 8.5 }, { 24.5, 8.5 }, { 24.5, 24.5 }, { -8.5, 24.5 },
    };
    FragScene f;
    float v[4][SGI_GL_VERTEX_FLOATS];
    uint32_t off = 0;

    frag_init(&f);
    f.c.line_width = 3;
    polygon_mode_vertices(v, xy, 4);
    for (unsigned i = 0; i < 4; i++) {
        memcpy(&v[i][SGI_GL_V_EDGE], &off, sizeof(off));
    }
    polygon_mode_draw(&f, v, 4, SGI_GL_POLYGON, SGI_GL_LINE, SGI_GL_LINE);
    g_assert_cmphex(frag_pixel(&f, 0, 16), ==, 0xffffff);
    g_assert_cmphex(frag_pixel(&f, 24, 16), ==, 0);
    memset(&f.p, 0, sizeof(f.p));
    f.c.point_size = 3;
    polygon_mode_draw(&f, v, 4, SGI_GL_POLYGON, SGI_GL_POINT, SGI_GL_POINT);
    g_assert_cmphex(frag_pixel(&f, 0, 24), ==, 0xffffff);
    g_assert_cmphex(frag_pixel(&f, 0, 8), ==, 0);
}

static void test_polygon_mode_provoking(void)
{
    const uint32_t primitives[] = { SGI_GL_POLYGON, SGI_GL_QUADS };
    const uint32_t modes[] = { SGI_GL_POINT, SGI_GL_LINE };

    for (unsigned p = 0; p < G_N_ELEMENTS(primitives); p++) {
        for (unsigned m = 0; m < G_N_ELEMENTS(modes); m++) {
            for (unsigned back = 0; back < 2; back++) {
                FragScene f;
                float v[4][SGI_GL_VERTEX_FLOATS];
                uint32_t both[2] = { modes[m], modes[m] };
                SGIGLDraw d = {
                    .mode = primitives[p], .count = 4, .vertices = &v[0][0],
                    .state = &f.c.cur, .polygon_modes = both,
                    .line_width = 1, .point_size = 1, .back_colors = true,
                };

                frag_init(&f);
                polygon_mode_vertices(v, polygon_mode_square, 4);
                f.c.cur.shade_model = SGI_GL_FLAT;
                f.c.cur.front_face = back ? SGI_GL_CW : SGI_GL_CCW;
                for (unsigned i = 0; i < 4; i++) {
                    v[i][SGI_GL_V_COLOR] = 0;
                    v[i][SGI_GL_V_COLOR + 1] = 0;
                    v[i][SGI_GL_V_BACK_COLOR] = 0;
                    v[i][SGI_GL_V_BACK_COLOR + 1] = 1;
                    v[i][SGI_GL_V_BACK_COLOR + 2] = 0;
                }
                v[0][SGI_GL_V_COLOR] = 1;
                v[0][SGI_GL_V_COLOR + 2] = 0;
                v[0][SGI_GL_V_BACK_COLOR] = 1;
                v[0][SGI_GL_V_BACK_COLOR + 1] = 0;
                sgi_soft_draw(f.rec.rules, &f.rec.target, &d);
                g_assert_cmphex(frag_pixel(&f, 24, 8), ==,
                    back ? (p == 0 ? 0xff0000 : 0x00ff00) :
                           (p == 0 ? 0xff0000 : 0x0000ff));
            }
        }
    }
}

static void test_polygon_mode_offset(void)
{
    const uint32_t modes[] = { SGI_GL_POINT, SGI_GL_LINE };

    for (unsigned m = 0; m < G_N_ELEMENTS(modes); m++) {
        FragScene f;
        float v[4][SGI_GL_VERTEX_FLOATS];
        unsigned x = m == 0 ? 8 : 16;
        uint32_t flag = m == 0 ? SGI_GL_OFFSET_POINT : SGI_GL_OFFSET_LINE;

        frag_init(&f);
        polygon_mode_vertices(v, polygon_mode_square, 4);
        v[2][SGI_GL_V_POSITION + 2] = 1;
        f.c.cur.depth_test = true;
        f.c.cur.depth_func = SGI_GL_ALWAYS;
        f.c.cur.offset_factor = 1;
        f.c.cur.polygon_offset = SGI_GL_OFFSET_FILL;
        polygon_mode_draw(&f, v, 4, SGI_GL_POLYGON, modes[m], modes[m]);
        g_assert_cmpuint(f.p.depth[planes_index(x, 8)], ==, 8388608);
        f.c.cur.polygon_offset = flag;
        polygon_mode_draw(&f, v, 4, SGI_GL_POLYGON, modes[m], modes[m]);
        g_assert_cmpuint(f.p.depth[planes_index(x, 8)], ==, 8912895);
        f.c.cur.offset_bias = 1;
        polygon_mode_draw(&f, v, 4, SGI_GL_POLYGON, modes[m], modes[m]);
        g_assert_cmpuint(f.p.depth[planes_index(x, 8)], ==, 16777215);
    }
}

static void test_polygon_mode_stipple(void)
{
    const float xy[4][2] = {
        { 8.5, 8.5 }, { 16.5, 8.5 }, { 16.5, 16.5 }, { 8.5, 16.5 },
    };
    FragScene f;
    float v[4][SGI_GL_VERTEX_FLOATS];
    uint32_t modes[2] = { SGI_GL_LINE, SGI_GL_LINE };
    uint32_t rows[32] = { 0 };
    SGIGLDraw d = {
        .mode = SGI_GL_POLYGON, .count = 4, .vertices = &v[0][0],
        .state = &f.c.cur, .polygon_modes = modes, .line_width = 1,
        .line_stipple_enabled = true, .line_stipple_repeat = 1,
        .line_stipple_pattern = 1, .polygon_stipple_enabled = true,
        .polygon_stipple = rows,
    };

    frag_init(&f);
    polygon_mode_vertices(v, xy, 4);
    sgi_soft_draw(f.rec.rules, &f.rec.target, &d);
    g_assert_cmpuint(frag_count(&f, 0xffffff), ==, 2);
    g_assert_cmphex(frag_pixel(&f, 8, 8), ==, 0xffffff);
    g_assert_cmphex(frag_pixel(&f, 16, 16), ==, 0xffffff);
    g_assert_cmphex(frag_pixel(&f, 16, 8), ==, 0);
    memset(&f.p, 0, sizeof(f.p));
    d.line_width = 3;
    sgi_soft_draw(f.rec.rules, &f.rec.target, &d);
    for (unsigned y = 7; y <= 9; y++) {
        g_assert_cmphex(frag_pixel(&f, 8, y), ==, 0xffffff);
        g_assert_cmphex(frag_pixel(&f, 9, y), ==, 0);
    }
    memset(&f.p, 0, sizeof(f.p));
    d.line_width = 1;
    d.line_stipple_pattern = 4;
    frag_scissor(&f, 10, 8, 1, 1);
    enable(&f.c, SGI_GL_SCISSOR_TEST, true);
    sgi_soft_draw(f.rec.rules, &f.rec.target, &d);
    g_assert_cmphex(frag_pixel(&f, 10, 8), ==, 0xffffff);
    memset(&f.p, 0, sizeof(f.p));
    enable(&f.c, SGI_GL_SCISSOR_TEST, false);
    {
        float reversed[4][2];

        for (unsigned i = 0; i < 4; i++) {
            memcpy(reversed[i], xy[(4 - i) % 4], sizeof(reversed[i]));
        }
        polygon_mode_vertices(v, reversed, 4);
    }
    sgi_soft_draw(f.rec.rules, &f.rec.target, &d);
    g_assert_cmphex(frag_pixel(&f, 8, 10), ==, 0xffffff);
    g_assert_cmphex(frag_pixel(&f, 8, 9), ==, 0);
    memset(&f.p, 0, sizeof(f.p));
    d.line_stipple_repeat = 2;
    d.line_stipple_pattern = 1;
    sgi_soft_draw(f.rec.rules, &f.rec.target, &d);
    g_assert_cmphex(frag_pixel(&f, 8, 8), ==, 0xffffff);
    g_assert_cmphex(frag_pixel(&f, 8, 9), ==, 0xffffff);
    g_assert_cmphex(frag_pixel(&f, 8, 10), ==, 0);
}

static void test_polygon_mode_stipple_bounds(void)
{
    const float xy[4][2] = {
        { 8.5, 8.5 }, { 16.5, 8.5 }, { 16.5, 16.5 }, { 8.5, 16.5 },
    };
    FragScene f;
    float v[4][SGI_GL_VERTEX_FLOATS];
    uint32_t modes[2] = { SGI_GL_LINE, SGI_GL_LINE };
    uint32_t rows[32] = { 0 }, off = 0;
    SGIGLDraw d = {
        .mode = SGI_GL_POLYGON, .count = 4, .vertices = &v[0][0],
        .state = &f.c.cur, .polygon_modes = modes, .line_width = 1,
        .line_stipple_enabled = true, .line_stipple_repeat = 1,
        .line_stipple_pattern = 4, .polygon_stipple_enabled = true,
        .polygon_stipple = rows,
    };

    frag_init(&f);
    polygon_mode_vertices(v, xy, 4);
    f.c.cur.drawable.clip.r[0] = (SGIClipRect) { 10, 8, 10, 8 };
    sgi_soft_draw(f.rec.rules, &f.rec.target, &d);
    g_assert_cmphex(frag_pixel(&f, 10, 8), ==, 0xffffff);
    memset(&f.p, 0, sizeof(f.p));
    f.c.cur.drawable.origin[0] = 5;
    f.c.cur.drawable.origin[1] = 7;
    f.c.cur.drawable.clip.r[0] = (SGIClipRect) { 15, 15, 15, 15 };
    sgi_soft_draw(f.rec.rules, &f.rec.target, &d);
    g_assert_cmphex(frag_pixel(&f, 15, 15), ==, 0xffffff);
    frag_init(&f);
    d.line_stipple_pattern = 1;
    memcpy(&v[0][SGI_GL_V_EDGE], &off, sizeof(off));
    sgi_soft_draw(f.rec.rules, &f.rec.target, &d);
    g_assert_cmphex(frag_pixel(&f, 16, 8), ==, 0xffffff);
    g_assert_cmphex(frag_pixel(&f, 8, 8), ==, 0);
    polygon_mode_vertices(v, xy, 4);
    memset(&f.p, 0, sizeof(f.p));
    d.line_stipple_enabled = false;
    d.line_stipple_pattern = 0;
    sgi_soft_draw(f.rec.rules, &f.rec.target, &d);
    g_assert_cmpuint(frag_count(&f, 0xffffff), ==, 32);
    memset(&f.p, 0, sizeof(f.p));
    modes[0] = modes[1] = SGI_GL_POINT;
    d.point_size = 3;
    d.line_stipple_enabled = true;
    sgi_soft_draw(f.rec.rules, &f.rec.target, &d);
    g_assert_cmpuint(frag_count(&f, 0xffffff), ==, 36);
    memset(&f.p, 0, sizeof(f.p));
    modes[0] = modes[1] = SGI_GL_LINE;
    d.line_stipple_enabled = false;
    f.c.cur.draw_buffers = 0;
    f.c.cur.colour_mask = 0;
    f.c.cur.depth_test = true;
    f.c.cur.depth_func = SGI_GL_ALWAYS;
    f.c.cur.stencil_test = true;
    f.c.cur.stencil_op[2] = SGI_GL_INCR;
    sgi_soft_draw(f.rec.rules, &f.rec.target, &d);
    g_assert_cmpuint(f.p.depth[planes_index(10, 8)], ==, 8388608);
    g_assert_cmpuint(f.p.stencil[planes_index(10, 8)], ==, 1);
    g_assert_cmpuint(frag_count(&f, 0), ==, W * H);
}

static void test_polygon_mode_fill_fan(void)
{
    const float xy[5][2] = {
        { -8.5, 8.5 }, { 24.5, 8.5 }, { 32.5, 16.5 },
        { 24.5, 24.5 }, { -8.5, 24.5 },
    };

    for (unsigned textured = 0; textured < 2; textured++) {
        for (unsigned flat = 0; flat < 2; flat++) {
            for (unsigned mixed = 0; mixed < 2; mixed++) {
                for (unsigned back = 0; back < 2; back++) {
                    FragScene expected, actual;
                    float v[5][SGI_GL_VERTEX_FLOATS];
                    uint32_t modes[2] = {
                        back && mixed ? SGI_GL_POINT : SGI_GL_FILL,
                        !back && mixed ? SGI_GL_POINT : SGI_GL_FILL,
                    };
                    TexturePixels pixels = { .pixel = {
                        0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff } };
                    SGIGLTexImage image = texture_image(&pixels);
                    SGIGLDraw d = {
                        .mode = SGI_GL_POLYGON, .count = 5,
                        .vertices = &v[0][0], .state = &actual.c.cur,
                        .polygon_modes = modes, .fill_by_triangle = true,
                        .back_colors = true,
                    };

                    frag_init(&expected);
                    frag_init(&actual);
                    polygon_mode_vertices(v, xy, 5);
                    for (unsigned i = 0; i < 5; i++) {
                        v[i][SGI_GL_V_COLOR] = i / 4.0f;
                        v[i][SGI_GL_V_COLOR + 1] = 1 - i / 4.0f;
                        v[i][SGI_GL_V_BACK_COLOR + 2] = i / 4.0f;
                        v[i][SGI_GL_V_TEXCOORD] = i / 4.0f;
                        v[i][SGI_GL_V_TEXCOORD + 1] = 1 - i / 4.0f;
                    }
                    expected.c.cur.shade_model = actual.c.cur.shade_model =
                        flat ? SGI_GL_FLAT : SGI_GL_SMOOTH;
                    expected.c.cur.front_face = actual.c.cur.front_face =
                        back ? SGI_GL_CW : SGI_GL_CCW;
                    expected.c.cur.texture_2d =
                        actual.c.cur.texture_2d = textured;
                    expected.rec.target.texture = actual.rec.target.texture =
                        &image;
                    expected.c.cur.polygon_offset =
                        actual.c.cur.polygon_offset = SGI_GL_OFFSET_FILL;
                    expected.c.cur.offset_units = actual.c.cur.offset_units = 2;
                    sgi_soft_draw(actual.rec.rules, &actual.rec.target, &d);
                    for (unsigned i = 1; i + 1 < 5; i++) {
                        float tri[3][SGI_GL_VERTEX_FLOATS];
                        SGIGLDraw old = d;

                        memcpy(tri[0], v[0], sizeof(tri[0]));
                        memcpy(tri[1], v[i], sizeof(tri[0]));
                        memcpy(tri[2], v[i + 1], sizeof(tri[0]));
                        old.count = 3;
                        old.vertices = &tri[0][0];
                        old.state = &expected.c.cur;
                        old.polygon_modes = NULL;
                        old.fill_by_triangle = false;
                        sgi_soft_draw(expected.rec.rules,
                                      &expected.rec.target, &old);
                    }
                    g_assert_cmpmem(&expected.p, sizeof(expected.p),
                                    &actual.p, sizeof(actual.p));
                    g_assert_cmpuint(frag_count(&actual, 0), <, W * H);
                }
            }
        }
    }
}

static void test_polygon_mode_invalid_fill(void)
{
    const float xy[5][2] = {
        { 8.5, 8.5 }, { 24.5, 8.5 }, { 32.5, 16.5 },
        { 24.5, 24.5 }, { 8.5, 24.5 },
    };
    TexturePixels pixels = { .pixel = {
        0xff0000ff, 0xff0000ff, 0xff0000ff, 0xff0000ff } };
    SGIGLTexImage image = texture_image(&pixels);

    for (unsigned malformed = 0; malformed < 2; malformed++) {
        for (unsigned mixed = 0; mixed < 2; mixed++) {
            FragScene old, actual;
            float v[5][SGI_GL_VERTEX_FLOATS];
            uint32_t modes[2] = { SGI_GL_FILL,
                                 mixed ? SGI_GL_POINT : SGI_GL_FILL };
            SGIGLDraw d = {
                .mode = SGI_GL_POLYGON, .count = 5, .vertices = &v[0][0],
                .state = &actual.c.cur, .polygon_modes = modes,
                .fill_by_triangle = true,
            };

            frag_init(&old);
            frag_init(&actual);
            old.c.cur.texture_2d = actual.c.cur.texture_2d = !malformed;
            old.rec.target.texture = actual.rec.target.texture = &image;
            polygon_mode_vertices(v, xy, 5);
            if (malformed) {
                v[4][SGI_GL_V_POSITION] = NAN;
            } else {
                v[4][SGI_GL_V_TEXCOORD + 3] = 0;
            }
            sgi_soft_draw(actual.rec.rules, &actual.rec.target, &d);
            for (unsigned i = 1; i + 1 < 5; i++) {
                float tri[3][SGI_GL_VERTEX_FLOATS];
                SGIGLDraw streamed = d;

                memcpy(tri[0], v[0], sizeof(tri[0]));
                memcpy(tri[1], v[i], sizeof(tri[0]));
                memcpy(tri[2], v[i + 1], sizeof(tri[0]));
                streamed.count = 3;
                streamed.vertices = &tri[0][0];
                streamed.state = &old.c.cur;
                streamed.polygon_modes = NULL;
                streamed.fill_by_triangle = false;
                sgi_soft_draw(old.rec.rules, &old.rec.target, &streamed);
            }
            if (malformed && mixed) {
                g_assert_cmpuint(frag_count(&actual, 0), ==, W * H);
            } else {
                g_assert_cmpmem(&old.p, sizeof(old.p),
                                &actual.p, sizeof(actual.p));
                g_assert_cmpuint(frag_count(&actual, 0), <, W * H);
            }
            if (!malformed) {
                memset(&actual.p, 0, sizeof(actual.p));
                pixels.reads = 0;
                d.fill_by_triangle = false;
                sgi_soft_draw(actual.rec.rules, &actual.rec.target, &d);
                g_assert_cmpuint(frag_count(&actual, 0), ==, W * H);
                g_assert_cmpuint(pixels.reads, ==, 0);
            }
        }
    }
}

static void test_polygon_mode_coincident_records(void)
{
    const float xy[4][2] = {
        { 8.5, 8.5 }, { 8.5, 8.5 }, { 24.5, 8.5 }, { 24.5, 24.5 },
    };
    const float exact[4][2] = {
        { 16.5, 8.5 }, { 8.5, 16.5 }, { 24.5, 24.5 }, { 24.5, 8.5 },
    };
    FragScene f;
    float v[4][SGI_GL_VERTEX_FLOATS];

    frag_init(&f);
    f.c.cur.stencil_test = true;
    f.c.cur.stencil_op[2] = SGI_GL_INCR;
    polygon_mode_vertices(v, xy, 4);
    polygon_mode_draw(&f, v, 4, SGI_GL_POLYGON, SGI_GL_POINT, SGI_GL_POINT);
    g_assert_cmpuint(f.p.stencil[planes_index(8, 8)], ==, 2);
    memset(&f.p, 0, sizeof(f.p));
    f.c.cur.clip_planes = 1;
    f.c.cur.clip_plane[0][0] = 1;
    f.c.cur.clip_plane[0][3] = 1 - 16.5f / 32;
    polygon_mode_vertices(v, exact, 4);
    polygon_mode_draw(&f, v, 4, SGI_GL_POLYGON, SGI_GL_POINT, SGI_GL_POINT);
    g_assert_cmpuint(f.p.stencil[planes_index(16, 8)], ==, 1);
    g_assert_cmpuint(f.p.stencil[planes_index(16, 20)], ==, 1);
}

static void test_polygon_mode_texture_boundary(void)
{
    FragScene f;
    float v[4][SGI_GL_VERTEX_FLOATS];
    TexturePixels pixels = { .pixel = {
        0xff0000ff, 0xff0000ff, 0xff0000ff, 0xff0000ff } };
    SGIGLTexImage image = texture_image(&pixels);
    uint32_t modes[2] = { SGI_GL_LINE, SGI_GL_LINE };
    SGIGLDraw d = {
        .mode = SGI_GL_POLYGON, .count = 4, .vertices = &v[0][0],
        .state = &f.c.cur, .polygon_modes = modes, .line_width = 1,
    };

    frag_init(&f);
    f.c.cur.texture_2d = true;
    f.rec.target.texture = &image;
    polygon_mode_vertices(v, polygon_mode_square, 4);
    v[3][SGI_GL_V_TEXCOORD + 3] = 0;
    f.c.cur.depth_test = true;
    f.c.cur.depth_func = SGI_GL_ALWAYS;
    f.c.cur.stencil_test = true;
    f.c.cur.stencil_op[2] = SGI_GL_INCR;
    sgi_soft_draw(f.rec.rules, &f.rec.target, &d);
    g_assert_cmpuint(pixels.reads, ==, 0);
    g_assert_cmpuint(frag_count(&f, 0), ==, W * H);
    g_assert_cmpuint(f.p.depth[planes_index(16, 8)], ==, 0);
    g_assert_cmpuint(f.p.stencil[planes_index(16, 8)], ==, 0);
    v[3][SGI_GL_V_TEXCOORD + 3] = 1;
    sgi_soft_draw(f.rec.rules, &f.rec.target, &d);
    g_assert_cmphex(frag_pixel(&f, 16, 8), ==, 0xff0000);
    g_assert_cmpuint(f.p.stencil[planes_index(16, 8)], ==, 1);
    g_assert_cmpuint(f.p.depth[planes_index(16, 8)], ==, 8388608);
    g_assert_cmpuint(pixels.reads, >, 0);
}

static int polygon_allocation_budget = -1;

#ifdef SGI_GL_ALLOCATION_WRAP
void *__real_g_try_realloc_n(void *mem, gsize blocks, gsize size);
void *__wrap_g_try_realloc_n(void *mem, gsize blocks, gsize size);

void *__wrap_g_try_realloc_n(void *mem, gsize blocks, gsize size)
{
    if (polygon_allocation_budget == 0) {
        return NULL;
    }
    if (polygon_allocation_budget > 0) {
        polygon_allocation_budget--;
    }
    return __real_g_try_realloc_n(mem, blocks, size);
}
#endif

static void test_polygon_mode_workspace_failure(void)
{
    const float xy[5][2] = {
        { 8.5, 8.5 }, { 24.5, 8.5 }, { 32.5, 16.5 },
        { 24.5, 24.5 }, { 8.5, 24.5 },
    };
    FragScene f;
    float v[5][SGI_GL_VERTEX_FLOATS];
    uint32_t modes[2] = { SGI_GL_LINE, SGI_GL_LINE };
    SGIGLDraw d = {
        .mode = SGI_GL_POLYGON, .count = 5, .vertices = &v[0][0],
        .state = &f.c.cur, .polygon_modes = modes, .line_width = 1,
    };

#ifndef SGI_GL_ALLOCATION_WRAP
    g_test_skip("linker does not support allocation wrapping");
    return;
#endif
    frag_init(&f);
    polygon_mode_vertices(v, xy, 5);
    f.c.cur.depth_test = true;
    f.c.cur.depth_func = SGI_GL_ALWAYS;
    f.c.cur.stencil_test = true;
    f.c.cur.stencil_op[2] = SGI_GL_INCR;
    polygon_allocation_budget = 0;
    g_assert_cmpint(sgi_soft_draw(f.rec.rules, &f.rec.target, &d), ==,
                    SGI_GL_DRAW_OUT_OF_MEMORY);
    polygon_allocation_budget = -1;
    g_assert_cmpuint(frag_count(&f, 0), ==, W * H);
    for (unsigned i = 0; i < W * H; i++) {
        g_assert_cmpuint(f.p.depth[i], ==, 0);
        g_assert_cmpuint(f.p.stencil[i], ==, 0);
    }
    g_assert_cmpint(sgi_soft_draw(f.rec.rules, &f.rec.target, &d), ==,
                    SGI_GL_DRAW_OK);
    g_assert_cmphex(frag_pixel(&f, 16, 8), ==, 0xffffff);
}

static SGIGLDrawResult recorder_checked_draw(void *opaque, const SGIGLDraw *d)
{
    Recorder *r = opaque;
    SGIGLDrawResult result;

    g_assert_cmphex(d->context, ==, 0x1234);
    result = sgi_soft_draw(r->rules, &r->target, d);
    r->draws++;
    return result;
}

static void test_polygon_mode_activation(void)
{
    FragScene f;
    SGIGLSink missing = { 0 };
    uint32_t mode = SGI_GL_POLYGON;

    frag_init(&f);
    g_assert_false(sgi_gl_enable_polygon_modes(&f.c, &missing));
    g_assert_false(f.c.polygon.enabled);
    f.sink.draw_checked = recorder_checked_draw;
    g_assert_true(sgi_gl_enable_polygon_modes(&f.c, &f.sink));
    frag_command(&f, SGI_GL_OP_POLYGON_MODE,
                 (uint32_t[]) { SGI_GL_FRONT_AND_BACK, SGI_GL_LINE }, 2);
    frag_command(&f, SGI_GL_OP_BEGIN, &mode, 1);
    g_assert_false(sgi_gl_enable_polygon_modes(&f.c, &f.sink));
    for (unsigned i = 0; i < 4; i++) {
        wide_vertex(&f, polygon_mode_square[i][0], polygon_mode_square[i][1]);
    }
    g_assert_cmpuint(f.rec.draws, ==, 0);
    g_assert_cmpuint(frag_count(&f, 0), ==, W * H);
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    g_assert_cmpuint(f.rec.draws, ==, 1);
    g_assert_cmphex(frag_pixel(&f, 16, 16), ==, 0);
    g_assert_cmphex(frag_pixel(&f, 16, 8), ==, 0xffffff);
    g_assert_null(f.c.polygon.vertices);
    memset(&f.p, 0, sizeof(f.p));
    f.sink.draw_checked = NULL;
    frag_command(&f, SGI_GL_OP_BEGIN, &mode, 1);
    for (unsigned i = 0; i < 4; i++) {
        wide_vertex(&f, polygon_mode_square[i][0], polygon_mode_square[i][1]);
    }
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    g_assert_cmpuint(f.rec.draws, ==, 1);
    g_assert_cmpuint(frag_count(&f, 0), ==, W * H);
    g_assert_null(f.c.polygon.vertices);
    g_assert_cmpuint(f.c.polygon.error, ==, 0);
    sgi_gl_cleanup(&f.c);
}

static void test_polygon_mode_activation_oom(void)
{
    const float xy[5][2] = {
        { 8.5, 8.5 }, { 24.5, 8.5 }, { 32.5, 16.5 },
        { 24.5, 24.5 }, { 8.5, 24.5 },
    };
    FragScene f;
    uint32_t mode = SGI_GL_POLYGON, error;

#ifndef SGI_GL_ALLOCATION_WRAP
    g_test_skip("linker does not support allocation wrapping");
    return;
#endif
    frag_init(&f);
    f.sink.draw_checked = recorder_checked_draw;
    g_assert_true(sgi_gl_enable_polygon_modes(&f.c, &f.sink));
    frag_command(&f, SGI_GL_OP_POLYGON_MODE,
                 (uint32_t[]) { SGI_GL_FRONT_AND_BACK, SGI_GL_LINE }, 2);
    frag_command(&f, SGI_GL_OP_BEGIN, &mode, 1);
    for (unsigned i = 0; i < 5; i++) {
        wide_vertex(&f, xy[i][0], xy[i][1]);
    }
    polygon_allocation_budget = 0;
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    polygon_allocation_budget = -1;
    g_assert_cmpuint(f.rec.draws, ==, 1);
    g_assert_null(f.c.polygon.vertices);
    g_assert_false(f.c.in_begin);
    g_assert_cmpuint(frag_count(&f, 0), ==, W * H);
    g_assert_cmphex(f.c.polygon.error, ==, SGI_GL_OUT_OF_MEMORY);
    frag_command(&f, SGI_GL_OP_BEGIN, &mode, 1);
    for (unsigned i = 0; i < 5; i++) {
        wide_vertex(&f, xy[i][0], xy[i][1]);
    }
    frag_command(&f, SGI_GL_OP_END, NULL, 0);
    g_assert_cmphex(frag_pixel(&f, 16, 8), ==, 0xffffff);
    g_assert_true(sgi_gl_consume_error(&f.c, &error));
    g_assert_cmphex(error, ==, SGI_GL_OUT_OF_MEMORY);
    g_assert_true(sgi_gl_consume_error(&f.c, &error));
    g_assert_cmpuint(error, ==, 0);
    sgi_gl_cleanup(&f.c);
}

/* Synthetic family-owned images with observable level identities and reads. */
typedef struct MipmapRecordPixels {
    uint32_t level, width, height;
    uint32_t pixels[16];
    unsigned reads;
    bool fail;
} MipmapRecordPixels;

typedef struct MipmapFixture {
    FragScene scene;
    MipmapRecordPixels pixels[4];
    SGIGLTexLevel levels[4];
    SGIGLTexPyramid pyramid;
    SGIGLTexImage image;
} MipmapFixture;

static bool mipmap_read(void *opaque, uint32_t level, uint32_t x, uint32_t y,
                        uint32_t *rgba)
{
    MipmapRecordPixels *p = opaque;

    g_assert_cmpuint(level, ==, p->level);
    g_assert_cmpuint(x, <, p->width);
    g_assert_cmpuint(y, <, p->height);
    p->reads++;
    if (p->fail) {
        return false;
    }
    *rgba = p->pixels[y * p->width + x];
    return true;
}

/* An opt-in pyramid must never consult ignored legacy image properties. */
static bool mipmap_legacy_read(void *opaque, uint32_t x, uint32_t y,
                               uint32_t *rgba)
{
    g_assert_not_reached();
    return false;
}

static void mipmap_init(MipmapFixture *f, uint32_t min, uint32_t mag,
                        double min_lod, double max_lod)
{
    static const uint32_t colours[3] = {
        0xff0000ff, 0x00ff00ff, 0x0000ffff,
    };

    memset(f, 0, sizeof(*f));
    frag_init(&f->scene);
    for (unsigned i = 0; i < 3; i++) {
        uint32_t n = 4 >> i;
        MipmapRecordPixels *p = &f->pixels[i];

        p->level = i;
        p->width = p->height = n;
        for (unsigned j = 0; j < n * n; j++) {
            p->pixels[j] = colours[i];
        }
        f->levels[i] = (SGIGLTexLevel) {
            .level = i, .width = n, .height = n,
            .base_format = SGI_GL_RGBA, .internal_format = SGI_GL_RGBA,
            .read_texel = mipmap_read, .opaque = p,
        };
    }
    f->pyramid = (SGIGLTexPyramid) {
        .levels = f->levels, .count = 3, .base_level = 0, .max_level = 2,
        .min_lod = min_lod, .max_lod = max_lod,
    };
    f->image = (SGIGLTexImage) {
        /* These are ignored even though the sampler and pyramid are valid. */
        .width = 3, .height = 0, .base_format = 0, .border = 1,
        .read_texel = mipmap_legacy_read,
        .min_filter = min, .mag_filter = mag,
        .wrap_s = SGI_GL_REPEAT, .wrap_t = SGI_GL_REPEAT,
        .pyramid = &f->pyramid,
    };
    f->scene.rec.target.texture = &f->image;
    enable(&f->scene.c, SGI_GL_TEXTURE_2D, true);
    texture_env(&f->scene, SGI_GL_TEXTURE_ENV_MODE, SGI_GL_REPLACE, 0, 0, 0);
    texture_coord(&f->scene, 0.5f, 0.5f, 0, 1);
}

static void mipmap_point(MipmapFixture *f)
{
    frag_command(&f->scene, SGI_GL_OP_COLOR4F,
                 (uint32_t[]) { bits(1), 0, bits(1), bits(1) }, 4);
    frag_command(&f->scene, SGI_GL_OP_BEGIN,
                 (uint32_t[]) { SGI_GL_POINTS }, 1);
    frag_command(&f->scene, SGI_GL_OP_VERTEX2F, (uint32_t[]) { 0, 0 }, 2);
    frag_command(&f->scene, SGI_GL_OP_END, NULL, 0);
}

/* Fixed bounds make the discrete lambda boundary exact, independent of log2. */
static void test_mipmap_filter_boundaries(void)
{
    const struct {
        uint32_t min, mag;
        double lambda;
        uint32_t rgb;
        unsigned reads[3];
    } cases[] = {
        { SGI_GL_NEAREST, SGI_GL_NEAREST, 2, 0xff0000, { 1, 0, 0 } },
        { SGI_GL_LINEAR, SGI_GL_NEAREST, 2, 0xff0000, { 4, 0, 0 } },
        { SGI_GL_NEAREST_MIPMAP_NEAREST, SGI_GL_NEAREST,
          0.5, 0xff0000, { 1, 0, 0 } },
        { SGI_GL_NEAREST_MIPMAP_NEAREST, SGI_GL_NEAREST,
          0.500001, 0x00ff00, { 0, 1, 0 } },
        { SGI_GL_NEAREST_MIPMAP_NEAREST, SGI_GL_NEAREST,
          1.5, 0x00ff00, { 0, 1, 0 } },
        { SGI_GL_NEAREST_MIPMAP_NEAREST, SGI_GL_NEAREST,
          1.500001, 0x0000ff, { 0, 0, 1 } },
        { SGI_GL_LINEAR_MIPMAP_NEAREST, SGI_GL_NEAREST,
          1, 0x00ff00, { 0, 4, 0 } },
        { SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_NEAREST,
          0.25, 0xbf4000, { 1, 1, 0 } },
        { SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_NEAREST,
          1.5, 0x008080, { 0, 1, 1 } },
        { SGI_GL_LINEAR_MIPMAP_LINEAR, SGI_GL_NEAREST,
          0.25, 0xbf4000, { 4, 4, 0 } },
        /* LINEAR mag changes c to 0.5 for nearest-within mip filters. */
        { SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_LINEAR,
          0.25, 0xff0000, { 4, 0, 0 } },
        { SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_LINEAR,
          0.5, 0xff0000, { 4, 0, 0 } },
        { SGI_GL_NEAREST_MIPMAP_NEAREST, SGI_GL_LINEAR,
          0.5, 0xff0000, { 4, 0, 0 } },
        /* alpha=0 and final clamp must not call the unused image. */
        { SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_NEAREST,
          1, 0x00ff00, { 0, 1, 0 } },
        { SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_NEAREST,
          DBL_MAX, 0x0000ff, { 0, 0, 1 } },
        { SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_NEAREST,
          -DBL_MAX, 0xff0000, { 1, 0, 0 } },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        MipmapFixture f;

        mipmap_init(&f, cases[i].min, cases[i].mag,
                    cases[i].lambda, cases[i].lambda);
        mipmap_point(&f);
        g_assert_cmphex(frag_pixel(&f.scene, 32, 32), ==, cases[i].rgb);
        for (unsigned j = 0; j < 3; j++) {
            g_assert_cmpuint(f.pixels[j].reads, ==, cases[i].reads[j]);
        }
    }
}

/* Different texels distinguish each within-level filter at the same LOD. */
static void test_mipmap_asymmetric_filters(void)
{
    const struct {
        uint32_t min, rgb;
        unsigned level1_reads;
    } cases[] = {
        { SGI_GL_NEAREST_MIPMAP_NEAREST, 0x0000ff, 1 },
        { SGI_GL_LINEAR_MIPMAP_NEAREST, 0x404040, 4 },
        { SGI_GL_NEAREST_MIPMAP_LINEAR, 0x4040ff, 1 },
        { SGI_GL_LINEAR_MIPMAP_LINEAR, 0x707070, 4 },
    };
    static const uint32_t pixels[] = {
        0x000000ff, 0xff0000ff, 0x00ff00ff, 0x0000ffff,
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        MipmapFixture f;

        mipmap_init(&f, cases[i].min, SGI_GL_NEAREST, 1.25, 1.25);
        memcpy(f.pixels[1].pixels, pixels, sizeof(pixels));
        f.pixels[2].pixels[0] = 0xffffffff;
        mipmap_point(&f);
        g_assert_cmphex(frag_pixel(&f.scene, 32, 32), ==, cases[i].rgb);
        g_assert_cmpuint(f.pixels[1].reads, ==, cases[i].level1_reads);
    }
}

/* A required missing image disables texturing even on magnification. */
static void test_mipmap_missing_level(void)
{
    MipmapFixture f;

    mipmap_init(&f, SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_NEAREST, -1, -1);
    f.levels[1].read_texel = NULL;
    mipmap_point(&f);
    g_assert_cmphex(frag_pixel(&f.scene, 32, 32), ==, 0xff00ff);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmpuint(f.pixels[i].reads, ==, 0);
    }
}

/* A constant coordinate's zero rho still passes through the LOD clamp. */
static void test_mipmap_zero_footprint(void)
{
    MipmapFixture f;

    mipmap_init(&f, SGI_GL_NEAREST_MIPMAP_NEAREST, SGI_GL_NEAREST, 2, 1000);
    frag_quad(&f.scene, 1, 0, 1, 1);
    g_assert_cmphex(frag_pixel(&f.scene, 16, 16), ==, 0x0000ff);
    g_assert_cmpuint(f.pixels[0].reads, ==, 0);
    g_assert_cmpuint(f.pixels[1].reads, ==, 0);
    g_assert_cmpuint(f.pixels[2].reads, >, 0);
}

/* Translate the reviewed two-pixel triangle to a covered pixel centre. */
static void test_mipmap_projective_triangle(void)
{
    MipmapFixture f;
    const double a = log2(32.0 / 25);

    mipmap_init(&f, SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_NEAREST, -1000, 1000);
    frag_command(&f.scene, SGI_GL_OP_BEGIN,
                 (uint32_t[]) { SGI_GL_TRIANGLES }, 1);
    /* Window vertices (16,16),(18,16),(16,18); sample at (16.5,16.5). */
    texture_vertex(&f.scene, -0.5f, -0.5f, 1, 0, 0, 1);
    texture_vertex(&f.scene, -0.4375f, -0.5f, 1, 1, 0, 2);
    texture_vertex(&f.scene, -0.5f, -0.4375f, 1, 0, 0, 1);
    frag_command(&f.scene, SGI_GL_OP_END, NULL, 0);
    g_assert_cmphex(frag_pixel(&f.scene, 16, 16), ==,
                   texture_rgb(1 - a, a, 0));
}

#ifdef SGI_GL_TEXTURE_TEST
#include "tests/unit/sgi-gl-texture-test.h"

static void mipmap_close(double actual, double expected)
{
    g_assert_true(isfinite(actual));
    g_assert_cmpfloat(fabs(actual - expected), <=,
                      0.000000000001 * MAX(1, fabs(expected)));
}

static void test_mipmap_numeric_derivatives(void)
{
    const int64_t xy[3][2] = { { 0, 0 }, { 512, 0 }, { 0, 512 } };
    const double coords[3][4] = {
        { 0, 0, 0, 1 }, { 1, 0, 0, 2 }, { 0, 0, 0, 1 },
    };
    const double iw[3] = { 1, 1, 1 };
    double value[3], dx[3], dy[3], result[5];

    g_assert_true(sgi_gl_texture_test_plane(xy, 8, coords, iw, value, dx, dy));
    mipmap_close(dx[0], 0.5);
    mipmap_close(dx[2], 0.5);
    mipmap_close(dy[0], 0);
    mipmap_close(dy[2], 0);
    for (unsigned k = 0; k < 3; k++) {
        value[k] += 0.5 * dx[k] + 0.5 * dy[k];
    }
    g_assert_true(sgi_gl_texture_test_derivatives(value, dx, dy, 4, 4, result));
    mipmap_close(result[0], 8.0 / 25);
    mipmap_close(result[1], 0);
    mipmap_close(result[2], 0);
    mipmap_close(result[3], 0);
    mipmap_close(result[4], 32.0 / 25);

    /* Line z=.5: A=.5, D=1.5, A'=D'=1, length=2. */
    g_assert_true(sgi_gl_texture_test_derivatives(
        (double[]) { 0.5, 0, 1.5 }, (double[]) { 1, 0, 1 },
        (double[]) { 0, 0, 0 }, 8, 1, result));
    mipmap_close(result[0], 4.0 / 9);
    mipmap_close(result[4] / 2, 16.0 / 9);
    /* Oblique, rectangular footprint: max(hypot(3,4),hypot(0,2))=5. */
    g_assert_true(sgi_gl_texture_test_derivatives(
        (double[]) { 0, 0, 1 }, (double[]) { 0.75, 2, 0 },
        (double[]) { 0, 1, 0 }, 4, 2, result));
    mipmap_close(result[4], 5);
    for (unsigned bad = 0; bad < 4; bad++) {
        double v[3] = { 0, 0, 1 }, x[3] = { 1, 0, 0 };

        if (bad == 0) {
            v[2] = 0;
        } else if (bad == 1) {
            v[2] = DBL_MIN;
        } else if (bad == 2) {
            x[0] = INFINITY;
        } else {
            x[0] = DBL_MAX;
        }
        g_assert_false(sgi_gl_texture_test_derivatives(v, x,
            (double[]) { 0, 0, 0 }, 4, 4, result));
    }
}

static void test_mipmap_numeric_selection(void)
{
    MipmapFixture f;
    double numeric[2];
    uint32_t out[3];
    const double ties[] = { nextafter(0.5, 0), 0.5,
                           nextafter(0.5, INFINITY) };

    for (unsigned i = 0; i < ARRAY_SIZE(ties); i++) {
        mipmap_init(&f, SGI_GL_NEAREST_MIPMAP_NEAREST, SGI_GL_NEAREST,
                    ties[i], ties[i]);
        g_assert_true(sgi_gl_texture_test_select(f.image.min_filter,
            f.image.mag_filter, &f.pyramid, 2, 1, out, numeric));
        g_assert_cmpuint(out[0], ==, i == 2 ? 1 : 0);
        mipmap_close(numeric[0], ties[i]);
    }
    mipmap_init(&f, SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_NEAREST, -1000, 1000);
    g_assert_true(sgi_gl_texture_test_select(f.image.min_filter,
        f.image.mag_filter, &f.pyramid, 2, 32.0 / 25, out, numeric));
    mipmap_close(numeric[0], log2(32.0 / 25));
    mipmap_close(numeric[1], log2(32.0 / 25));
    g_assert_cmpuint(out[0], ==, 0);
    g_assert_cmpuint(out[1], ==, 1);
    f.pyramid.min_lod = 2;
    g_assert_true(sgi_gl_texture_test_select(f.image.min_filter,
        f.image.mag_filter, &f.pyramid, 2, 0, out, numeric));
    mipmap_close(numeric[0], 2);
    g_assert_cmpuint(out[0], ==, 2);
    g_assert_false(sgi_gl_texture_test_select(f.image.min_filter,
        f.image.mag_filter, &f.pyramid, 2, INFINITY, out, numeric));
}

static void test_mipmap_numeric_samples(void)
{
    MipmapFixture f;
    double sample[4], other[4] = { 1, 1, 1, 1 };
    const uint32_t pixels[4] = { 0x000000ff, 0xff0000ff,
                                0x00ff00ff, 0x0000ffff };

    mipmap_init(&f, SGI_GL_LINEAR_MIPMAP_LINEAR, SGI_GL_NEAREST, 0, 2);
    memcpy(f.pixels[1].pixels, pixels, sizeof(pixels));
    g_assert_true(sgi_gl_texture_test_sample(&f.image, &f.levels[1],
        SGI_GL_LINEAR, 0.5, 0.5, sample));
    for (unsigned k = 0; k < 3; k++) {
        mipmap_close(sample[k], 0.25);
    }
    mipmap_close(sample[3], 1);
    sgi_gl_texture_test_mix(sample, other, 0.25);
    for (unsigned k = 0; k < 3; k++) {
        mipmap_close(sample[k], 7.0 / 16);
    }
    mipmap_close(sample[3], 1);
}
#endif

static MipmapFixture mipmap_logged;

static void mipmap_logged_draw(void)
{
    frag_quad(&mipmap_logged.scene, 1, 0, 1, 1);
}

static void mipmap_expect_log(const char *reason, bool fallback)
{
    g_autofree char *log = logged_by(mipmap_logged_draw);
    const char *at = strstr(log, reason);

    g_assert_nonnull(at);
    g_assert_null(strstr(at + strlen(reason), reason));
    g_assert_cmphex(frag_pixel(&mipmap_logged.scene, 16, 16), ==,
                   fallback ? 0xff00ff : 0);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmpuint(mipmap_logged.pixels[i].reads, ==, 0);
    }
}

static void test_mipmap_validation(void)
{
    for (unsigned bad = 0; bad < 12; bad++) {
        MipmapFixture *f = &mipmap_logged;

        mipmap_init(f, SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_NEAREST,
                    -1000, 1000);
        switch (bad) {
        case 0:
            f->pyramid.count = SGI_GL_TEX_LEVELS_MAX + 1;
            break;
        case 1:
            f->pyramid.levels = NULL;
            break;
        case 2:
            f->levels[1].level = 0;
            break;
        case 3:
            f->levels[0].level = 2;
            break;
        case 4:
            f->pyramid.min_lod = NAN;
            break;
        case 5:
            f->pyramid.max_lod = INFINITY;
            break;
        case 6:
            f->pyramid.min_lod = 1001;
            break;
        case 7:
            f->image.min_filter = 0;
            break;
        case 8:
            f->image.mag_filter = SGI_GL_NEAREST_MIPMAP_LINEAR;
            break;
        case 9:
            f->levels[1].internal_format = 0;
            break;
        case 10:
            f->levels[1].border = 1;
            break;
        case 11:
            f->levels[1].internal_format = SGI_GL_ALPHA8;
            break;
        }
        mipmap_expect_log("unsupported texture state", false);
    }
    for (unsigned missing = 0; missing < 6; missing++) {
        MipmapFixture *f = &mipmap_logged;

        mipmap_init(f, SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_NEAREST, -1, -1);
        switch (missing) {
        case 0:
            f->pyramid.count = 0;
            f->pyramid.levels = NULL;
            break;
        case 1:
            f->levels[1].read_texel = NULL;
            break;
        case 2:
            f->pyramid.count = 2;
            break;
        case 3:
            f->levels[1].width = 1;
            break;
        case 4:
            f->levels[0].internal_format = SGI_GL_RGBA4;
            f->levels[1].internal_format = SGI_GL_RGBA8;
            break;
        case 5:
            f->levels[0].width = 3;
            break;
        }
        mipmap_expect_log("incomplete texture image", true);
    }
    /* Invalid sampler policy must not hide behind a missing base image. */
    mipmap_init(&mipmap_logged, 0, SGI_GL_NEAREST, 0, 0);
    mipmap_logged.pyramid.count = 0;
    mipmap_logged.pyramid.levels = NULL;
    mipmap_expect_log("unsupported texture state", false);
}

static void test_mipmap_absolute_levels(void)
{
    for (unsigned range = 0; range < 3; range++) {
        MipmapFixture f;
        uint32_t base = range == 0 ? 2 :
                        range == 1 ? UINT32_MAX - 1 : UINT32_MAX;

        mipmap_init(&f, SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_NEAREST, 1, 1);
        f.pyramid.base_level = base;
        f.pyramid.max_level = range == 0 ? 4 : UINT32_MAX;
        f.pyramid.count = range == 2 ? 1 : range == 1 ? 2 : 3;
        for (unsigned i = 0; i < f.pyramid.count; i++) {
            f.levels[i].level = f.pixels[i].level = base + i;
        }
        mipmap_point(&f);
        g_assert_cmphex(frag_pixel(&f.scene, 32, 32), ==,
                       range == 2 ? 0xff0000 : 0x00ff00);
    }
    for (unsigned mipmap = 0; mipmap < 2; mipmap++) {
        MipmapFixture f;

        mipmap_init(&f, mipmap ? SGI_GL_NEAREST_MIPMAP_LINEAR : SGI_GL_NEAREST,
                    SGI_GL_NEAREST, 1, 1);
        f.pyramid.base_level = 1;
        f.pyramid.max_level = 0;
        mipmap_point(&f);
        g_assert_cmphex(frag_pixel(&f.scene, 32, 32), ==,
                       mipmap ? 0xff00ff : 0x00ff00);
    }
    /* An inactive record's image properties are not consulted. */
    MipmapFixture f;

    mipmap_init(&f, SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_NEAREST, 1, 1);
    f.pyramid.max_level = 1;
    f.levels[2].internal_format = 0;
    f.levels[2].border = 1;
    f.levels[2].width = 0;
    mipmap_point(&f);
    g_assert_cmphex(frag_pixel(&f.scene, 32, 32), ==, 0x00ff00);
    g_assert_cmpuint(f.pixels[2].reads, ==, 0);
}

static void mipmap_rectangular(MipmapFixture *f)
{
    static const uint32_t colours[] = {
        0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff,
    };

    for (unsigned i = 0; i < 4; i++) {
        uint32_t w = 8 >> i;

        f->pixels[i] = (MipmapRecordPixels) {
            .level = i, .width = w, .height = 1,
        };
        for (unsigned j = 0; j < w; j++) {
            f->pixels[i].pixels[j] = colours[i];
        }
        f->levels[i] = (SGIGLTexLevel) {
            .level = i, .width = w, .height = 1,
            .base_format = SGI_GL_RGBA, .internal_format = SGI_GL_RGBA,
            .read_texel = mipmap_read, .opaque = &f->pixels[i],
        };
    }
    f->pyramid.count = 4;
    f->pyramid.max_level = 3;
}

static void test_mipmap_projective_line(void)
{
    for (unsigned width = 1; width <= 3; width += 2) {
        MipmapFixture f;
        const double a = log2(16.0 / 9);

        mipmap_init(&f, SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_NEAREST,
                    -1000, 1000);
        mipmap_rectangular(&f);
        frag_command(&f.scene, SGI_GL_OP_LINE_WIDTH,
                     (uint32_t[]) { bits(width) }, 1);
        frag_command(&f.scene, SGI_GL_OP_BEGIN,
                     (uint32_t[]) { SGI_GL_LINES }, 1);
        texture_vertex(&f.scene, -0.484375f, -0.484375f, 1, 0, 0, 1);
        texture_vertex(&f.scene, -0.421875f, -0.484375f, 1, 1, 0, 2);
        frag_command(&f.scene, SGI_GL_OP_END, NULL, 0);
        for (int y = 16 - width / 2; y <= 16 + width / 2; y++) {
            g_assert_cmphex(frag_pixel(&f.scene, 17, y), ==,
                           texture_rgb(1 - a, a, 0));
        }
    }
}

static void test_mipmap_second_read_failure(void)
{
    MipmapFixture f;
    Planes before;

    mipmap_init(&f, SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_NEAREST, 0.25, 0.25);
    memset(&f.scene.p, 0x5a, sizeof(f.scene.p));
    before = f.scene.p;
    f.pixels[1].fail = true;
    enable(&f.scene.c, SGI_GL_DEPTH_TEST, true);
    enable(&f.scene.c, SGI_GL_STENCIL_TEST, true);
    frag_command(&f.scene, SGI_GL_OP_DEPTH_FUNC,
                 (uint32_t[]) { SGI_GL_ALWAYS }, 1);
    frag_stencil(&f.scene, SGI_GL_ALWAYS, 1, 0xff,
                 SGI_GL_REPLACE, SGI_GL_REPLACE, SGI_GL_REPLACE);
    mipmap_point(&f);
    g_assert_cmpmem(&before, sizeof(before), &f.scene.p, sizeof(f.scene.p));
    g_assert_cmpuint(f.pixels[0].reads, ==, 1);
    g_assert_cmpuint(f.pixels[1].reads, ==, 1);
    /* At an exact level, an unavailable zero-weight neighbour is untouched. */
    memset(&f.scene.p, 0, sizeof(f.scene.p));
    f.pyramid.min_lod = f.pyramid.max_lod = 0;
    f.pixels[0].reads = f.pixels[1].reads = 0;
    mipmap_point(&f);
    g_assert_cmpuint(f.pixels[0].reads, ==, 1);
    g_assert_cmpuint(f.pixels[1].reads, ==, 0);
}

static void test_mipmap_rebinding_and_degenerate(void)
{
    MipmapFixture first, second;

    mipmap_init(&first, SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_NEAREST, 1, 1);
    mipmap_init(&second, SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_NEAREST, 2, 2);
    mipmap_point(&first);
    first.pixels[1].fail = true;
    first.scene.rec.target.texture = &second.image;
    mipmap_point(&first);
    g_assert_cmphex(frag_pixel(&first.scene, 32, 32), ==, 0x0000ff);
    g_assert_cmpuint(second.pixels[2].reads, ==, 1);
    mipmap_point(&second);
    g_assert_cmpuint(second.pixels[2].reads, ==, 2);
    for (unsigned mode = 0; mode < 2; mode++) {
        MipmapFixture f;

        mipmap_init(&f, SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_NEAREST, 0, 2);
        frag_command(&f.scene, SGI_GL_OP_BEGIN,
                     (uint32_t[]) {
                         mode ? SGI_GL_TRIANGLES : SGI_GL_LINES }, 1);
        for (unsigned i = 0; i < (mode ? 3 : 2); i++) {
            frag_command(&f.scene, SGI_GL_OP_VERTEX2F,
                         (uint32_t[]) { 0, 0 }, 2);
        }
        frag_command(&f.scene, SGI_GL_OP_END, NULL, 0);
        for (unsigned i = 0; i < 3; i++) {
            g_assert_cmpuint(f.pixels[i].reads, ==, 0);
        }
        g_assert_cmpuint(frag_count(&f.scene, 0), ==, W * H);
    }
}

/* Blend the levels before applying the per-format environment and fog. */
static void test_mipmap_format_pipeline(void)
{
    const double l = 80.0 / 255, b = 112.0 / 255, u = 80.0 / 255;
    const struct {
        uint32_t format, mode;
        double rgb[3], alpha;
    } cases[] = {
        { SGI_GL_ALPHA, SGI_GL_REPLACE, { 0.25, 0.5, 0.75 }, u },
        { SGI_GL_ALPHA, SGI_GL_MODULATE, { 0.25, 0.5, 0.75 }, 0.8 * u },
        { SGI_GL_ALPHA, SGI_GL_BLEND, { 0.25, 0.5, 0.75 }, 0.8 * u },
        { SGI_GL_LUMINANCE, SGI_GL_REPLACE, { l, l, l }, 0.8 },
        { SGI_GL_LUMINANCE, SGI_GL_MODULATE,
          { 0.25 * l, 0.5 * l, 0.75 * l }, 0.8 },
        { SGI_GL_LUMINANCE, SGI_GL_BLEND,
          { 0.25 * (1 - l), 0.5 * (1 - l) + l, 0.75 * (1 - l) }, 0.8 },
        { SGI_GL_LUMINANCE_ALPHA, SGI_GL_REPLACE, { l, l, l }, u },
        { SGI_GL_LUMINANCE_ALPHA, SGI_GL_MODULATE,
          { 0.25 * l, 0.5 * l, 0.75 * l }, 0.8 * u },
        { SGI_GL_LUMINANCE_ALPHA, SGI_GL_BLEND,
          { 0.25 * (1 - l), 0.5 * (1 - l) + l, 0.75 * (1 - l) }, 0.8 * u },
        { SGI_GL_INTENSITY, SGI_GL_REPLACE, { l, l, l }, l },
        { SGI_GL_INTENSITY, SGI_GL_MODULATE,
          { 0.25 * l, 0.5 * l, 0.75 * l }, 0.8 * l },
        { SGI_GL_INTENSITY, SGI_GL_BLEND,
          { 0.25 * (1 - l), 0.5 * (1 - l) + l, 0.75 * (1 - l) },
          0.8 * (1 - l) + 0.2 * l },
        { SGI_GL_RGB, SGI_GL_REPLACE, { l, 0, b }, 0.8 },
        { SGI_GL_RGB, SGI_GL_MODULATE, { 0.25 * l, 0, 0.75 * b }, 0.8 },
        { SGI_GL_RGB, SGI_GL_BLEND,
          { 0.25 * (1 - l), 0.5, 0.75 * (1 - b) }, 0.8 },
        { SGI_GL_RGBA, SGI_GL_REPLACE, { l, 0, b }, u },
        { SGI_GL_RGBA, SGI_GL_MODULATE, { 0.25 * l, 0, 0.75 * b }, 0.8 * u },
        { SGI_GL_RGBA, SGI_GL_BLEND,
          { 0.25 * (1 - l), 0.5, 0.75 * (1 - b) }, 0.8 * u },
        { SGI_GL_RGB, SGI_GL_DECAL, { l, 0, b }, 0.8 },
        { SGI_GL_RGBA, SGI_GL_DECAL,
          { 0.25 * (1 - u) + l * u, 0.5 * (1 - u),
            0.75 * (1 - u) + b * u }, 0.8 },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        for (unsigned operation = 0; operation < 4; operation++) {
            MipmapFixture f;
            double expected[3];

            mipmap_init(&f, SGI_GL_NEAREST_MIPMAP_LINEAR,
                        SGI_GL_NEAREST, 0.25, 0.25);
            for (unsigned level = 0; level < 3; level++) {
                f.levels[level].base_format = cases[i].format;
                f.levels[level].internal_format = cases[i].format;
                for (unsigned j = 0; j < 16; j++) {
                    f.pixels[level].pixels[j] = level ? 0x80004080 : 0x40008040;
                }
            }
            texture_env(&f.scene, SGI_GL_TEXTURE_ENV_MODE,
                        cases[i].mode, 0, 0, 0);
            texture_env(&f.scene, SGI_GL_TEXTURE_ENV_COLOR, 0, 1, 0, 0.2f);
            memcpy(expected, cases[i].rgb, sizeof(expected));
            if (operation == 1 || operation == 2) {
                float ref = floor(cases[i].alpha * 255 + 0.5) / 255;

                ref += operation == 1 ? -1.0 / 255 : 1.0 / 255;
                enable(&f.scene.c, SGI_GL_ALPHA_TEST, true);
                frag_command(&f.scene, SGI_GL_OP_ALPHA_FUNC,
                    (uint32_t[]) { SGI_GL_NEVER_ENUM + SGI_GL_GREATER,
                                   bits(ref) }, 2);
            }
            if (operation == 3) {
                frag_fog(&f.scene, SGI_GL_FOG_MODE, SGI_GL_LINEAR, 0, 0, 0);
                frag_fog(&f.scene, SGI_GL_FOG_END, 1, 0, 0, 0);
                frag_fog(&f.scene, SGI_GL_FOG_COLOR, 0, 0, 1, 0);
                enable(&f.scene.c, SGI_GL_FOG, true);
                for (unsigned k = 0; k < 3; k++) {
                    expected[k] = 0.5 * expected[k] + (k == 2 ? 0.5 : 0);
                }
            }
            frag_quad_at(&f.scene, 0.25f, 0.5f, 0.75f, 0.8f, -0.5f);
            g_assert_cmphex(frag_pixel(&f.scene, 16, 16), ==,
                operation == 2 ? 0 :
                texture_rgb(expected[0], expected[1], expected[2]));
            g_assert_cmpuint(f.pixels[0].reads, >, 0);
            g_assert_cmpuint(f.pixels[1].reads, >, 0);
            g_assert_cmpuint(f.pixels[2].reads, ==, 0);
        }
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-gl/mipmap-format-pipeline",
                    test_mipmap_format_pipeline);
    g_test_add_func("/sgi-gl/mipmap-numeric-derivatives",
                    test_mipmap_numeric_derivatives);
    g_test_add_func("/sgi-gl/mipmap-numeric-selection",
                    test_mipmap_numeric_selection);
    g_test_add_func("/sgi-gl/mipmap-numeric-samples",
                    test_mipmap_numeric_samples);
    g_test_add_func("/sgi-gl/mipmap-validation",
                    test_mipmap_validation);
    g_test_add_func("/sgi-gl/mipmap-absolute-levels",
                    test_mipmap_absolute_levels);
    g_test_add_func("/sgi-gl/mipmap-projective-line",
                    test_mipmap_projective_line);
    g_test_add_func("/sgi-gl/mipmap-second-read-failure",
                    test_mipmap_second_read_failure);
    g_test_add_func("/sgi-gl/mipmap-rebinding-and-degenerate",
                    test_mipmap_rebinding_and_degenerate);
    g_test_add_func("/sgi-gl/mipmap-filter-boundaries",
                    test_mipmap_filter_boundaries);
    g_test_add_func("/sgi-gl/mipmap-asymmetric-filters",
                    test_mipmap_asymmetric_filters);
    g_test_add_func("/sgi-gl/mipmap-missing-level",
                    test_mipmap_missing_level);
    g_test_add_func("/sgi-gl/mipmap-zero-footprint",
                    test_mipmap_zero_footprint);
    g_test_add_func("/sgi-gl/mipmap-projective-triangle",
                    test_mipmap_projective_triangle);
    g_test_add_func("/sgi-gl/texture-coordinates", test_texture_coordinates);
    g_test_add_func("/sgi-gl/texture-sampling", test_texture_sampling);
    g_test_add_func("/sgi-gl/texture-environment", test_texture_environment);
    g_test_add_func("/sgi-gl/texture-projective", test_texture_projective);
    g_test_add_func("/sgi-gl/texture-fog-order", test_texture_fog_order);
    g_test_add_func("/sgi-gl/texture-errors", test_texture_errors);
    g_test_add_func("/sgi-gl/texture-environment-without-image",
                    test_texture_environment_without_image);
    g_test_add_func("/sgi-gl/texture-invalid-quad-polygon",
                    test_texture_invalid_quad_polygon);
    g_test_add_func("/sgi-gl/texture-invalid-clipped-polygon",
                    test_texture_invalid_clipped_polygon);
    g_test_add_func("/sgi-gl/texture-points-lines-binding",
                    test_texture_points_lines_binding);
    g_test_add_func("/sgi-gl/texture-clips-behind-eye",
                    test_texture_clips_behind_eye);
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
    g_test_add_func("/sgi-gl/stencil-test", test_stencil_test);
    g_test_add_func("/sgi-gl/blend", test_blend);
    g_test_add_func("/sgi-gl/logic-op", test_logic_op);
    g_test_add_func("/sgi-gl/colour-mask", test_colour_mask);
    g_test_add_func("/sgi-gl/fog", test_fog);
    g_test_add_func("/sgi-gl/polygon-offset", test_polygon_offset);
    g_test_add_func("/sgi-gl/clip-planes", test_clip_planes);
    g_test_add_func("/sgi-gl/alpha-storage", test_alpha_storage);
    g_test_add_func("/sgi-gl/alpha-blend-masks", test_alpha_blend_masks);
    g_test_add_func("/sgi-gl/alpha-clear-precision",
                    test_alpha_clear_precision);
    g_test_add_func("/sgi-gl/alpha-read-failures", test_alpha_read_failures);
    g_test_add_func("/sgi-gl/alpha-zero-targets", test_alpha_zero_targets);
    g_test_add_func("/sgi-gl/alpha-texture-fog-rejection",
                    test_alpha_texture_fog_rejection);
    g_test_add_func("/sgi-gl/wide-coverage", test_wide_coverage);
    g_test_add_func("/sgi-gl/wide-diagonal", test_wide_diagonal);
    g_test_add_func("/sgi-gl/wide-state", test_wide_state);
    g_test_add_func("/sgi-gl/line-integer-endpoints",
                    test_line_integer_endpoints);
    g_test_add_func("/sgi-gl/wide-interpolation", test_wide_interpolation);
    g_test_add_func("/sgi-gl/wide-fragment-ops", test_wide_fragment_ops);
    g_test_add_func("/sgi-gl/wide-dimensions", test_wide_dimensions);
    g_test_add_func("/sgi-gl/polygon-stipple-rgba-texture",
                    test_polygon_stipple_rgba_texture);
    g_test_add_func("/sgi-gl/polygon-stipple-state",
                    test_polygon_stipple_state);
    g_test_add_func("/sgi-gl/polygon-stipple-phase",
                    test_polygon_stipple_phase);
    g_test_add_func("/sgi-gl/polygon-stipple-fragment-ops",
                    test_polygon_stipple_fragment_ops);
    g_test_add_func("/sgi-gl/polygon-state-begin-guard",
                    test_polygon_state_begin_guard);
    g_test_add_func("/sgi-gl/raster-state-kept", test_raster_state_kept);
    g_test_add_func("/sgi-gl/volume-clip-triangle-near-and-far",
                    test_volume_clip_triangle_near_and_far);
    g_test_add_func("/sgi-gl/volume-clip-segment-side-planes",
                    test_volume_clip_segment_side_planes);
    g_test_add_func("/sgi-gl/volume-clip-negative-w",
                    test_volume_clip_negative_w);
    g_test_add_func("/sgi-gl/volume-clip-shares-edges",
                    test_volume_clip_shares_edges);
    g_test_add_func("/sgi-gl/draw-clips-to-the-view-volume",
                    test_draw_clips_to_the_view_volume);
    g_test_add_func("/sgi-gl/dither-counts", test_dither_counts);
    g_test_add_func("/sgi-gl/dither-follows-blend",
                    test_dither_follows_blend);
    g_test_add_func("/sgi-gl/dither-screen-pattern",
                    test_dither_screen_pattern);
    g_test_add_func("/sgi-gl/dither-skips-clears", test_dither_skips_clears);
    g_test_add_func("/sgi-gl/dither-none-is-unchanged",
                    test_dither_none_is_unchanged);
    g_test_add_func("/sgi-gl/window-matrix-position",
                    test_window_matrix_position);
    g_test_add_func("/sgi-gl/window-matrix-guard-band",
                    test_window_matrix_guard_band);
    g_test_add_func("/sgi-gl/window-matrix-depth", test_window_matrix_depth);
    g_test_add_func("/sgi-gl/window-matrix-clips-near",
                    test_window_matrix_clips_near);
    g_test_add_func("/sgi-gl/window-matrix-rows-down-facing",
                    test_window_matrix_rows_down_facing);
    g_test_add_func("/sgi-gl/window-matrix-off-is-unchanged",
                    test_window_matrix_off_is_unchanged);
    g_test_add_func("/sgi-gl/polygon-mode-boundary",
                    test_polygon_mode_boundary);
    g_test_add_func("/sgi-gl/polygon-mode-edge-flags",
                    test_polygon_mode_edge_flags);
    g_test_add_func("/sgi-gl/polygon-mode-client-boundary",
                    test_polygon_mode_client_boundary);
    g_test_add_func("/sgi-gl/polygon-mode-facing",
                    test_polygon_mode_facing);
    g_test_add_func("/sgi-gl/polygon-mode-zero-area",
                    test_polygon_mode_zero_area);
    g_test_add_func("/sgi-gl/polygon-mode-strip-edges",
                    test_polygon_mode_strip_edges);
    g_test_add_func("/sgi-gl/polygon-mode-volume-boundary",
                    test_polygon_mode_volume_boundary);
    g_test_add_func("/sgi-gl/polygon-mode-provoking",
                    test_polygon_mode_provoking);
    g_test_add_func("/sgi-gl/polygon-mode-offset",
                    test_polygon_mode_offset);
    g_test_add_func("/sgi-gl/polygon-mode-stipple",
                    test_polygon_mode_stipple);
    g_test_add_func("/sgi-gl/polygon-mode-fill-fan",
                    test_polygon_mode_fill_fan);
    g_test_add_func("/sgi-gl/polygon-mode-invalid-fill",
                    test_polygon_mode_invalid_fill);
    g_test_add_func("/sgi-gl/polygon-mode-coincident-records",
                    test_polygon_mode_coincident_records);
    g_test_add_func("/sgi-gl/polygon-mode-texture-boundary",
                    test_polygon_mode_texture_boundary);
    g_test_add_func("/sgi-gl/polygon-mode-workspace-failure",
                    test_polygon_mode_workspace_failure);
    g_test_add_func("/sgi-gl/polygon-mode-activation",
                    test_polygon_mode_activation);
    g_test_add_func("/sgi-gl/polygon-mode-activation-oom",
                    test_polygon_mode_activation_oom);
    g_test_add_func("/sgi-gl/polygon-mode-stipple-bounds",
                    test_polygon_mode_stipple_bounds);
    return g_test_run();
}
