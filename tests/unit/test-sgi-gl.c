/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The shared GL context and software rasterizer, on paths a family's
 * replay fixtures need not reach: whole batches of every primitive mode,
 * glFinish, the clear op a context builds, and unsupported rule sets.
 */
#include "qemu/osdep.h"
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

    g_assert_false(sgi_gl_set_matrix(&c, 0x1702, twos));
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
    return g_test_run();
}
