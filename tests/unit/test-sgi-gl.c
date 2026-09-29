/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The shared GL context and software rasterizer, on paths a family's
 * replay fixtures need not reach: whole batches of every primitive mode,
 * glFinish, the clear op a context builds, unsupported rule sets,
 * colour-index mode and glBitmap.
 */
#include "qemu/osdep.h"
#include <math.h>
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
    return g_test_run();
}
