/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * One OpenGL context's state machine, in the shape the SGI graphics heads
 * share.
 *
 * A family decodes its command words into SGIGLCmd and hands them here.
 * The state they change is OpenGL 1.2.1's; the arithmetic is double
 * precision with results stored at single, the precision the client
 * sends. No head's own arithmetic is published, so that is the model's.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "hw/display/sgi/gl-context.h"

static float sgi_gl_float(uint32_t bits)
{
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t sgi_gl_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void sgi_gl_identity(float m[16])
{
    for (int i = 0; i < 16; i++) {
        m[i] = i % 5 == 0;
    }
}

static void sgi_gl_rgba(float v[4], float r, float g, float b, float a)
{
    v[0] = r;
    v[1] = g;
    v[2] = b;
    v[3] = a;
}

/* The lighting state's initial values (OpenGL 1.2.1, table 2.10). */
static void sgi_gl_lighting_init(SGIGLContext *c)
{
    for (int i = 0; i < SGI_GL_LIGHTS; i++) {
        SGIGLLight *l = &c->light[i];
        float on = i == 0 ? 1.0f : 0.0f;

        sgi_gl_rgba(l->ambient, 0, 0, 0, 1);
        sgi_gl_rgba(l->diffuse, on, on, on, 1);
        sgi_gl_rgba(l->specular, on, on, on, 1);
        sgi_gl_rgba(l->position, 0, 0, 1, 0);
        l->spot_direction[2] = -1.0f;
        l->spot_cutoff = 180.0f;
        l->attenuation[0] = 1.0f;
    }
    for (int i = 0; i < 2; i++) {
        SGIGLMaterial *m = &c->material[i];

        sgi_gl_rgba(m->ambient, 0.2f, 0.2f, 0.2f, 1);
        sgi_gl_rgba(m->diffuse, 0.8f, 0.8f, 0.8f, 1);
        sgi_gl_rgba(m->specular, 0, 0, 0, 1);
        sgi_gl_rgba(m->emission, 0, 0, 0, 1);
        m->color_indexes[1] = m->color_indexes[2] = 1.0f;
    }
    sgi_gl_rgba(c->light_model_ambient, 0.2f, 0.2f, 0.2f, 1);
    c->color_material_face = SGI_GL_FRONT_AND_BACK;
    c->color_material_mode = SGI_GL_AMBIENT_AND_DIFFUSE;
}

/* OpenGL 1.2.1 initial state (2.7, 2.10, 2.13.7, 4.1.5, 4.2.1-4.2.3). */
void sgi_gl_init(SGIGLContext *c, uint32_t hw_id)
{
    memset(c, 0, sizeof(*c));
    c->hw_id = hw_id;
    for (int i = 0; i < 4; i++) {
        c->cur_color[i] = 1.0f;
    }
    c->cur.shade_model = SGI_GL_SMOOTH;
    c->matrix_mode = SGI_GL_MODELVIEW;
    c->dither = 1;
    sgi_gl_identity(c->cur.modelview);
    sgi_gl_identity(c->cur.projection);
    sgi_gl_identity(c->texture);
    c->cur.depth_func = SGI_GL_LESS;
    c->cur.depth_mask = 1;
    c->cur.depth_range[1] = 1.0f;
    c->clear_depth = UINT32_MAX;
    c->cur.draw_buffers = SGI_GL_DRAW_FRONT;
    c->cur.index_mask = UINT32_MAX;
    c->cur_index = 1.0f;
    c->cur_normal[2] = 1.0f;
    c->cur_edge = 1;
    c->raster_pos[3] = 1.0f;
    c->raster_valid = 1;
    for (int i = 0; i < 4; i++) {
        c->raster_color[i] = 1.0f;
    }
    c->raster_index = 1.0f;
    sgi_gl_lighting_init(c);
}

void sgi_gl_set_drawable(SGIGLContext *c, const SGIGLDrawable *d)
{
    c->cur.drawable = *d;
}

void sgi_gl_set_index_mode(SGIGLContext *c, uint32_t index_bits)
{
    c->cur.index_bits = MIN(index_bits, 32);
}

/*
 * 2.13.9 converts an index to fixed point with an unspecified number of
 * fraction bits and keeps the integer part, which for a value already in
 * fixed point is its floor; a value beyond 32 bits, or not a number,
 * keeps nothing a buffer could hold and stores 0.
 */
uint32_t sgi_gl_index_value(uint32_t index_bits, double index)
{
    uint32_t mask = index_bits >= 32 ? UINT32_MAX : (1u << index_bits) - 1;
    double f = floor(index);

    if (!(f >= -2147483648.0 && f < 4294967296.0)) {
        return 0;
    }
    return (f < 0 ? (uint32_t)(int32_t)f : (uint32_t)f) & mask;
}

/*
 * m = m * b, both column-major as glLoadMatrixf lays them out (2.10.2),
 * formed in double precision and stored at single.
 */
static void sgi_gl_multiply(float m[16], const double b[16])
{
    float r[16];

    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            double sum = 0;

            for (int k = 0; k < 4; k++) {
                sum += (double)m[k * 4 + row] * b[col * 4 + k];
            }
            r[col * 4 + row] = sum;
        }
    }
    memcpy(m, r, sizeof(r));
}

/* The current matrix of the stack a matrix mode names, or NULL. */
static float *sgi_gl_slot_matrix(SGIGLContext *c, uint32_t slot)
{
    switch (slot) {
    case SGI_GL_MODELVIEW:
        return c->cur.modelview;
    case SGI_GL_PROJECTION:
        return c->cur.projection;
    case SGI_GL_TEXTURE:
        return c->texture;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-gl: matrix mode 0x%x is not "
                      "modelled; matrix command ignored\n", slot);
        return NULL;
    }
}

static float *sgi_gl_current_matrix(SGIGLContext *c)
{
    return sgi_gl_slot_matrix(c, c->matrix_mode);
}

bool sgi_gl_set_matrix(SGIGLContext *c, uint32_t slot, const float m[16])
{
    float *dst = sgi_gl_slot_matrix(c, slot);

    if (!dst) {
        return false;
    }
    memcpy(dst, m, 16 * sizeof(float));
    return true;
}

/*
 * glOrtho's and glFrustum's matrices (OpenGL 1.2.1, 2.10.2), from the six
 * arguments the client sends as floats. Arguments the client itself would
 * have rejected with GL_INVALID_VALUE leave the matrix unchanged.
 */
static bool sgi_gl_projection_matrix(uint16_t op, const uint32_t *p,
                                     double m[16])
{
    double l = sgi_gl_float(p[0]), r = sgi_gl_float(p[1]);
    double b = sgi_gl_float(p[2]), t = sgi_gl_float(p[3]);
    double n = sgi_gl_float(p[4]), f = sgi_gl_float(p[5]);

    if (l == r || b == t || n == f ||
        (op == SGI_GL_OP_FRUSTUM && (n <= 0 || f <= 0))) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: projection with invalid "
                      "bounds ignored\n");
        return false;
    }
    memset(m, 0, 16 * sizeof(double));
    if (op == SGI_GL_OP_ORTHO) {
        m[0] = 2 / (r - l);
        m[5] = 2 / (t - b);
        m[10] = -2 / (f - n);
        m[12] = -(r + l) / (r - l);
        m[13] = -(t + b) / (t - b);
        m[14] = -(f + n) / (f - n);
        m[15] = 1;
    } else {
        m[0] = 2 * n / (r - l);
        m[5] = 2 * n / (t - b);
        m[8] = (r + l) / (r - l);
        m[9] = (t + b) / (t - b);
        m[10] = -(f + n) / (f - n);
        m[11] = -1;
        m[14] = -2 * f * n / (f - n);
    }
    return true;
}

/*
 * glTranslatef's, glScalef's and glRotatef's matrices (OpenGL 1.2.1,
 * 2.10.2). The rotation is by the angle in degrees, counter-clockwise
 * looking from the axis toward the origin, about the axis scaled to unit
 * length. A zero axis leaves the matrix unchanged.
 */
static bool sgi_gl_transform_matrix(uint16_t op, const uint32_t *p,
                                    double m[16])
{
    double x, y, z, len, a, c, sn;

    memset(m, 0, 16 * sizeof(double));
    m[0] = m[5] = m[10] = m[15] = 1;
    switch (op) {
    case SGI_GL_OP_TRANSLATE:
        m[12] = sgi_gl_float(p[0]);
        m[13] = sgi_gl_float(p[1]);
        m[14] = sgi_gl_float(p[2]);
        return true;
    case SGI_GL_OP_SCALE:
        m[0] = sgi_gl_float(p[0]);
        m[5] = sgi_gl_float(p[1]);
        m[10] = sgi_gl_float(p[2]);
        return true;
    default:
        a = sgi_gl_float(p[0]) * M_PI / 180.0;
        x = sgi_gl_float(p[1]);
        y = sgi_gl_float(p[2]);
        z = sgi_gl_float(p[3]);
        len = sqrt(x * x + y * y + z * z);
        if (!(len > 0)) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: glRotatef about a zero "
                          "axis ignored\n");
            return false;
        }
        x /= len;
        y /= len;
        z /= len;
        c = cos(a);
        sn = sin(a);
        /* Column-major: m[col * 4 + row]. */
        m[0] = x * x * (1 - c) + c;
        m[1] = y * x * (1 - c) + z * sn;
        m[2] = x * z * (1 - c) - y * sn;
        m[4] = x * y * (1 - c) - z * sn;
        m[5] = y * y * (1 - c) + c;
        m[6] = y * z * (1 - c) + x * sn;
        m[8] = x * z * (1 - c) + y * sn;
        m[9] = y * z * (1 - c) - x * sn;
        m[10] = z * z * (1 - c) + c;
        return true;
    }
}

/*
 * glPushMatrix and glPopMatrix on the current mode's stack (2.10.2). Each
 * stack holds SGI_GL_STACK_DEPTH matrices, the current one included, the
 * least OpenGL 1.2.1 allows for the modelview stack (table 6.25) and more
 * than it requires for the projection and texture stacks, which need only
 * two; what depths a head has is not known. A push onto a full stack and
 * a pop of the last matrix change nothing, which is what the
 * specification has the stack-overflow and stack-underflow errors do; the
 * client reports the errors itself, if at all. The discriminating test
 * is gl-matrix-stack-holds-32.
 */
static void sgi_gl_stack_command(SGIGLContext *c, uint16_t op, float *m)
{
    float (*stack)[16] = m == c->cur.modelview ? c->mv_stack :
                         m == c->cur.projection ? c->pr_stack : c->tx_stack;
    uint32_t *depth = m == c->cur.modelview ? &c->mv_depth :
                      m == c->cur.projection ? &c->pr_depth : &c->tx_depth;

    if (op == SGI_GL_OP_PUSH_MATRIX) {
        if (*depth >= SGI_GL_STACK_DEPTH - 1) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: glPushMatrix on a full "
                          "stack ignored\n");
            return;
        }
        memcpy(stack[(*depth)++], m, 16 * sizeof(float));
        return;
    }
    if (*depth == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: glPopMatrix of the last "
                      "matrix ignored\n");
        return;
    }
    memcpy(m, stack[--(*depth)], 16 * sizeof(float));
}

static void sgi_gl_matrix_command(SGIGLContext *c, const SGIGLCmd *cmd)
{
    float *m, load[16];
    double arg[16];

    if (cmd->op == SGI_GL_OP_LOAD_MATRIX) {
        /* The words are the floats' bits; copying keeps every bit. */
        memcpy(load, cmd->args, sizeof(load));
        sgi_gl_set_matrix(c, c->matrix_mode, load);
        return;
    }
    m = sgi_gl_current_matrix(c);
    if (!m) {
        return;
    }
    switch (cmd->op) {
    case SGI_GL_OP_PUSH_MATRIX:
    case SGI_GL_OP_POP_MATRIX:
        sgi_gl_stack_command(c, cmd->op, m);
        return;
    case SGI_GL_OP_TRANSLATE:
    case SGI_GL_OP_SCALE:
    case SGI_GL_OP_ROTATE:
        if (sgi_gl_transform_matrix(cmd->op, cmd->args, arg)) {
            sgi_gl_multiply(m, arg);
        }
        return;
    case SGI_GL_OP_LOAD_IDENTITY:
        sgi_gl_identity(m);
        return;
    case SGI_GL_OP_MULT_MATRIX:
        for (int i = 0; i < 16; i++) {
            arg[i] = sgi_gl_float(cmd->args[i]);
        }
        sgi_gl_multiply(m, arg);
        return;
    default:
        if (sgi_gl_projection_matrix(cmd->op, cmd->args, arg)) {
            sgi_gl_multiply(m, arg);
        }
        return;
    }
}

/* The arguments each command takes. */
static const uint8_t sgi_gl_nargs[] = {
    [SGI_GL_OP_VIEWPORT] = 4,
    [SGI_GL_OP_CLEAR_COLOR] = 4,
    [SGI_GL_OP_CLEAR_DEPTH] = 1,
    [SGI_GL_OP_SHADE_MODEL] = 1,
    [SGI_GL_OP_MATRIX_MODE] = 1,
    [SGI_GL_OP_LOAD_MATRIX] = 16,
    [SGI_GL_OP_LOAD_IDENTITY] = 0,
    [SGI_GL_OP_MULT_MATRIX] = 16,
    [SGI_GL_OP_FRUSTUM] = 6,
    [SGI_GL_OP_ORTHO] = 6,
    [SGI_GL_OP_PUSH_MATRIX] = 0,
    [SGI_GL_OP_POP_MATRIX] = 0,
    [SGI_GL_OP_ROTATE] = 4,
    [SGI_GL_OP_SCALE] = 3,
    [SGI_GL_OP_TRANSLATE] = 3,
    [SGI_GL_OP_ENABLE] = 1,
    [SGI_GL_OP_DISABLE] = 1,
    [SGI_GL_OP_DEPTH_FUNC] = 1,
    [SGI_GL_OP_DEPTH_MASK] = 1,
    [SGI_GL_OP_DRAW_BUFFERS] = 1,
    [SGI_GL_OP_COLOR3F] = 3,
    [SGI_GL_OP_COLOR4F] = 4,
    [SGI_GL_OP_COLOR3UB] = 3,
    [SGI_GL_OP_COLOR4UB] = 4,
    [SGI_GL_OP_NORMAL3F] = 3,
    [SGI_GL_OP_EDGE_FLAG] = 1,
    [SGI_GL_OP_BEGIN] = 1,
    [SGI_GL_OP_END] = 0,
    [SGI_GL_OP_VERTEX2F] = 2,
    [SGI_GL_OP_VERTEX3F] = 3,
    [SGI_GL_OP_VERTEX4F] = 4,
    [SGI_GL_OP_VERTEX2I] = 2,
    [SGI_GL_OP_FLUSH] = 0,
    [SGI_GL_OP_CLEAR] = 1,
    [SGI_GL_OP_FINISH] = 0,
    [SGI_GL_OP_RASTER_POS2F] = 2,
    [SGI_GL_OP_RASTER_POS3F] = 3,
    [SGI_GL_OP_RASTER_POS4F] = 4,
    [SGI_GL_OP_INDEXF] = 1,
    [SGI_GL_OP_INDEX_MASK] = 1,
    [SGI_GL_OP_CLEAR_INDEX] = 1,
    [SGI_GL_OP_DEPTH_RANGE] = 2,
    [SGI_GL_OP_LIGHT] = 6,
    [SGI_GL_OP_MATERIAL] = 6,
    [SGI_GL_OP_LIGHT_MODEL] = 5,
    [SGI_GL_OP_COLOR_MATERIAL] = 2,
};

uint32_t sgi_gl_draw_targets(const SGIGLDrawState *st,
                             SGIColourTarget out[2])
{
    uint32_t n = 0;

    if (st->draw_buffers & ~(SGI_GL_DRAW_FRONT | SGI_GL_DRAW_BACK)) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: draw buffers 0x%x beyond the "
                      "front and back buffers ignored\n", st->draw_buffers);
    }
    if (st->draw_buffers & SGI_GL_DRAW_FRONT) {
        out[n++] = st->drawable.front;
    }
    /*
     * A drawable whose front and back are one target, as a single-buffered
     * one may be, is stored to once: a read-modify-write stage in front of
     * the store must not apply twice.
     */
    if ((st->draw_buffers & SGI_GL_DRAW_BACK) &&
        !(n && st->drawable.back == out[0])) {
        out[n++] = st->drawable.back;
    }
    return n;
}

/*
 * glClear (OpenGL 1.2.1, 4.2.3) as a raster op over the drawable's
 * rectangles. A clear passes only the pixel ownership, scissor and
 * dither steps, and the viewport does not bound it; ownership is the
 * drawable's rectangles, and the scissor test and dithering are not
 * modelled. The depth buffer is cleared only when the depth mask allows,
 * and the stencil and accumulation buffers are not modelled.
 */
static void sgi_gl_clear(SGIGLContext *c, const SGIGLSink *sink,
                         uint32_t mask)
{
    const SGIGLDrawable *d = &c->cur.drawable;
    SGIRasterOp op = {
        .kind = SGI_RASTER_CLEAR,
    };

    if (mask & ~(SGI_GL_COLOR_BUFFER_BIT | SGI_GL_DEPTH_BUFFER_BIT)) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: glClear of buffers 0x%x beyond "
                      "colour and depth ignored\n", mask);
    }
    op.ntargets = sgi_gl_draw_targets(&c->cur, op.targets);
    if (!d->bound || !d->clip.n) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: glClear with no window bound "
                      "dropped\n");
        return;
    }
    op.clip = d->clip;
    op.x0 = d->clip.r[0].x0;
    op.y0 = d->clip.r[0].y0;
    op.x1 = d->clip.r[0].x1;
    op.y1 = d->clip.r[0].y1;
    for (uint32_t i = 1; i < MIN(d->clip.n, SGI_CLIP_MAX); i++) {
        op.x0 = MIN(op.x0, d->clip.r[i].x0);
        op.y0 = MIN(op.y0, d->clip.r[i].y0);
        op.x1 = MAX(op.x1, d->clip.r[i].x1);
        op.y1 = MAX(op.y1, d->clip.r[i].y1);
    }
    if (mask & SGI_GL_COLOR_BUFFER_BIT) {
        op.clear_buffers |= SGI_GL_COLOR_BUFFER_BIT;
        memcpy(op.clear_rgba, c->clear_color, sizeof(op.clear_rgba));
        op.clear_colour_mask = UINT32_MAX;
        /* In colour-index mode the clear index, under glIndexMask (4.2.3). */
        op.index_bits = c->cur.index_bits;
        op.clear_index = sgi_gl_index_value(c->cur.index_bits,
                                            c->clear_index);
        op.index_mask = c->cur.index_mask;
    }
    if ((mask & SGI_GL_DEPTH_BUFFER_BIT) && c->cur.depth_mask) {
        op.clear_buffers |= SGI_GL_DEPTH_BUFFER_BIT;
        op.clear_depth24 = c->clear_depth;
    }
    if (op.clear_buffers) {
        sink->raster(sink->opaque, &op);
    }
}

/* Send one piece of the open primitive: count of v0 to v3. */
static void sgi_gl_submit(SGIGLContext *c, const SGIGLSink *sink,
                          uint32_t mode, uint32_t count, const float *v0,
                          const float *v1, const float *v2, const float *v3)
{
    const float *v[4] = { v0, v1, v2, v3 };
    float verts[4][SGI_GL_VERTEX_FLOATS];
    SGIGLDraw d = {
        .context = c->hw_id,
        .mode = mode,
        .count = count,
        .vertices = &verts[0][0],
        .state = &c->cur,
    };

    for (uint32_t i = 0; i < count; i++) {
        memcpy(verts[i], v[i], sizeof(verts[i]));
    }
    sink->draw(sink->opaque, &d);
}

/* Hold a vertex in slot n of the three a context keeps. */
static void sgi_gl_hold(SGIGLContext *c, unsigned int n, const float *v)
{
    memcpy(c->held[n], v, sizeof(c->held[n]));
}

/*
 * One vertex of the primitive open since glBegin (OpenGL 1.2.1, 2.6.1),
 * carrying the current attributes. The context holds at most three
 * earlier vertices, in held[] with nheld of them in use, and every piece
 * is sent as its own draw as soon as its last vertex arrives, so a
 * context switch between two vertices loses nothing. A line loop's
 * closing segment is sent at glEnd. Vertices that complete no piece
 * before glEnd draw nothing.
 */
static void sgi_gl_vertex(SGIGLContext *c, const SGIGLSink *sink,
                          const float *pos, unsigned int components)
{
    float cur[SGI_GL_VERTEX_FLOATS] = { 0 };
    const float *const h0 = c->held[0], *const h1 = c->held[1];
    const float *const h2 = c->held[2];
    unsigned int n = c->nheld;

    if (!c->in_begin) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: vertex outside glBegin ignored\n");
        return;
    }
    cur[SGI_GL_V_POSITION + 3] = 1.0f;
    memcpy(&cur[SGI_GL_V_POSITION], pos, components * sizeof(float));
    memcpy(&cur[SGI_GL_V_COLOR], c->cur_color, sizeof(c->cur_color));
    memcpy(&cur[SGI_GL_V_NORMAL], c->cur_normal, sizeof(c->cur_normal));
    cur[SGI_GL_V_TEXCOORD + 3] = 1.0f;
    memcpy(&cur[SGI_GL_V_EDGE], &c->cur_edge, sizeof(c->cur_edge));
    cur[SGI_GL_V_INDEX] = c->cur_index;

    switch (c->begin_mode) {
    case SGI_GL_POINTS:
        sgi_gl_submit(c, sink, SGI_GL_POINTS, 1, cur, NULL, NULL, NULL);
        return;
    case SGI_GL_LINES:
        if (n < 1) {
            break;
        }
        sgi_gl_submit(c, sink, SGI_GL_LINES, 2, h0, cur, NULL, NULL);
        c->nheld = 0;
        return;
    case SGI_GL_LINE_STRIP:
        if (n < 1) {
            break;
        }
        sgi_gl_submit(c, sink, SGI_GL_LINES, 2, h0, cur, NULL, NULL);
        sgi_gl_hold(c, 0, cur);
        return;
    case SGI_GL_LINE_LOOP:
        /* Slot 0 keeps the first vertex for the closing segment. */
        if (n < 1) {
            break;
        }
        sgi_gl_submit(c, sink, SGI_GL_LINES, 2, c->held[n - 1], cur, NULL,
                      NULL);
        sgi_gl_hold(c, 1, cur);
        c->nheld = 2;
        return;
    case SGI_GL_TRIANGLES:
        if (n < 2) {
            break;
        }
        sgi_gl_submit(c, sink, SGI_GL_TRIANGLES, 3, h0, h1, cur, NULL);
        c->nheld = 0;
        return;
    case SGI_GL_TRIANGLE_STRIP:
        if (n < 2) {
            break;
        }
        sgi_gl_submit(c, sink, SGI_GL_TRIANGLES, 3, h0, h1, cur, NULL);
        sgi_gl_hold(c, 0, h1);
        sgi_gl_hold(c, 1, cur);
        return;
    case SGI_GL_TRIANGLE_FAN:
        if (n < 2) {
            break;
        }
        sgi_gl_submit(c, sink, SGI_GL_TRIANGLES, 3, h0, h1, cur, NULL);
        sgi_gl_hold(c, 1, cur);
        return;
    case SGI_GL_POLYGON:
        if (n < 2) {
            break;
        }
        sgi_gl_submit(c, sink, SGI_GL_POLYGON, 3, h0, h1, cur, NULL);
        sgi_gl_hold(c, 1, cur);
        return;
    case SGI_GL_QUADS:
        if (n < 3) {
            break;
        }
        sgi_gl_submit(c, sink, SGI_GL_QUADS, 4, h0, h1, h2, cur);
        c->nheld = 0;
        return;
    case SGI_GL_QUAD_STRIP:
        if (n < 3) {
            break;
        }
        sgi_gl_submit(c, sink, SGI_GL_QUAD_STRIP, 4, h0, h1, h2, cur);
        sgi_gl_hold(c, 0, h2);
        sgi_gl_hold(c, 1, cur);
        c->nheld = 2;
        return;
    default:
        g_assert_not_reached();
    }
    sgi_gl_hold(c, n, cur);
    c->nheld = n + 1;
}

/*
 * glRasterPos (OpenGL 1.2.1, 2.12): the point goes through the modelview
 * and projection matrices like a vertex. Inside the view volume it
 * becomes the current raster position in window coordinates, keeping
 * clip w, with the eye distance, the current colour clamped to [0, 1]
 * (2.13.6) and the current colour index; lighting is not modelled.
 * Outside, only the valid bit is reset: the specification leaves the
 * rest undefined, and keeping it is the model's choice. Client clip
 * planes are not modelled, and window depth goes through the depth
 * range, as for a draw. The eye distance is the
 * exact Euclidean one of the eye x, y and z, which 2.12 allows a head to
 * approximate; how a head does is not published.
 */
static void sgi_gl_raster_pos(SGIGLContext *c, const float obj[4])
{
    const float *mv = c->cur.modelview, *pr = c->cur.projection;
    const int32_t *vp = c->cur.viewport;
    double eye[4], clip[4];

    for (int row = 0; row < 4; row++) {
        eye[row] = 0;
        for (int k = 0; k < 4; k++) {
            eye[row] += (double)mv[k * 4 + row] * obj[k];
        }
    }
    for (int row = 0; row < 4; row++) {
        clip[row] = 0;
        for (int k = 0; k < 4; k++) {
            clip[row] += (double)pr[k * 4 + row] * eye[k];
        }
    }
    /*
     * Clip (0, 0, 0, 0) passes the view-volume test and has no window
     * position; it is culled, the model's choice for a point with no
     * image.
     */
    for (int i = 0; i < 3; i++) {
        if (!(clip[3] > 0) ||
            !(-clip[3] <= clip[i] && clip[i] <= clip[3])) {
            c->raster_valid = 0;
            return;
        }
    }
    c->raster_valid = 1;
    c->raster_pos[0] = (clip[0] / clip[3] + 1) * vp[2] / 2 + vp[0];
    c->raster_pos[1] = (clip[1] / clip[3] + 1) * vp[3] / 2 + vp[1];
    c->raster_pos[2] = c->cur.depth_range[0] +
                       (c->cur.depth_range[1] - c->cur.depth_range[0]) *
                       ((clip[2] / clip[3] + 1) / 2);
    c->raster_pos[3] = clip[3];
    c->raster_distance = sqrt(eye[0] * eye[0] + eye[1] * eye[1] +
                              eye[2] * eye[2]);
    for (int i = 0; i < 4; i++) {
        c->raster_color[i] = MIN(MAX(c->cur_color[i], 0.0f), 1.0f);
    }
    c->raster_index = c->cur_index;
}

/*
 * glDepthRange (OpenGL 1.2.1, 2.10.1): near and far are each clamped to
 * [0, 1] and kept, far may be below near, and a value that is not a number
 * clamps to 0, the model's choice. Draws and glRasterPos map normalized
 * depth through the range.
 */
static void sgi_gl_depth_range(SGIGLContext *c, float n, float f)
{
    float range[2] = { n, f };

    for (int i = 0; i < 2; i++) {
        range[i] = range[i] >= 1.0f ? 1.0f : range[i] > 0.0f ? range[i] : 0.0f;
        c->cur.depth_range[i] = range[i];
    }
}

/*
 * The faces a glMaterial or glColorMaterial face names, as a mask of
 * material[] entries, or 0 for a value that names none.
 */
static unsigned sgi_gl_faces(uint32_t face)
{
    switch (face) {
    case SGI_GL_FRONT:
        return 1;
    case SGI_GL_BACK:
        return 2;
    case SGI_GL_FRONT_AND_BACK:
        return 3;
    default:
        return 0;
    }
}

/*
 * One material parameter of the faces in the mask faces (2.13.2). v holds
 * four floats; GL_SHININESS uses the first and GL_COLOR_INDEXES three.
 * Shininess outside [0, 128] is an error and changes nothing.
 */
static void sgi_gl_material(SGIGLContext *c, unsigned faces, uint32_t pname,
                            const float v[4])
{
    if (pname == SGI_GL_SHININESS && !(v[0] >= 0.0f && v[0] <= 128.0f)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: material shininess %g "
                      "ignored\n", v[0]);
        return;
    }
    for (int i = 0; i < 2; i++) {
        SGIGLMaterial *m = &c->material[i];

        if (!(faces & (1u << i))) {
            continue;
        }
        switch (pname) {
        case SGI_GL_AMBIENT:
            memcpy(m->ambient, v, sizeof(m->ambient));
            break;
        case SGI_GL_DIFFUSE:
            memcpy(m->diffuse, v, sizeof(m->diffuse));
            break;
        case SGI_GL_AMBIENT_AND_DIFFUSE:
            memcpy(m->ambient, v, sizeof(m->ambient));
            memcpy(m->diffuse, v, sizeof(m->diffuse));
            break;
        case SGI_GL_SPECULAR:
            memcpy(m->specular, v, sizeof(m->specular));
            break;
        case SGI_GL_EMISSION:
            memcpy(m->emission, v, sizeof(m->emission));
            break;
        case SGI_GL_SHININESS:
            m->shininess = v[0];
            break;
        case SGI_GL_COLOR_INDEXES:
            memcpy(m->color_indexes, v, sizeof(m->color_indexes));
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: material parameter "
                          "0x%x ignored\n", pname);
            return;
        }
    }
}

/*
 * With GL_COLOR_MATERIAL enabled the material parameter glColorMaterial
 * chose tracks the current colour (2.13.3): each colour command sets it,
 * and so do enabling and a new glColorMaterial while enabled, since from
 * then on the parameter is the current colour. Colour-index lighting
 * takes no part.
 */
static void sgi_gl_track_colour(SGIGLContext *c)
{
    if (c->color_material) {
        sgi_gl_material(c, sgi_gl_faces(c->color_material_face),
                        c->color_material_mode, c->cur_color);
    }
}

static void sgi_gl_transform(const float m[16], const float in[4],
                             unsigned n, float out[4])
{
    for (int row = 0; row < 4; row++) {
        double v = 0;

        for (unsigned k = 0; k < n; k++) {
            v += (double)m[k * 4 + row] * in[k];
        }
        out[row] = v;
    }
}

/*
 * glLightfv (2.13.2). The position is transformed by the modelview
 * current at the call and the spot direction by its upper left 3x3, so
 * both are kept in eye coordinates. A spot exponent outside [0, 128], a
 * cutoff outside [0, 90] other than 180 or a negative attenuation is an
 * error and changes nothing.
 */
static void sgi_gl_light(SGIGLContext *c, uint32_t n, uint32_t pname,
                         const float v[4])
{
    SGIGLLight *l;
    float eye[4];

    if (n >= SGI_GL_LIGHTS) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: light %u ignored\n", n);
        return;
    }
    l = &c->light[n];
    switch (pname) {
    case SGI_GL_AMBIENT:
        memcpy(l->ambient, v, sizeof(l->ambient));
        return;
    case SGI_GL_DIFFUSE:
        memcpy(l->diffuse, v, sizeof(l->diffuse));
        return;
    case SGI_GL_SPECULAR:
        memcpy(l->specular, v, sizeof(l->specular));
        return;
    case SGI_GL_POSITION:
        sgi_gl_transform(c->cur.modelview, v, 4, l->position);
        return;
    case SGI_GL_SPOT_DIRECTION:
        sgi_gl_transform(c->cur.modelview, v, 3, eye);
        memcpy(l->spot_direction, eye, sizeof(l->spot_direction));
        return;
    case SGI_GL_SPOT_EXPONENT:
        if (v[0] >= 0.0f && v[0] <= 128.0f) {
            l->spot_exponent = v[0];
            return;
        }
        break;
    case SGI_GL_SPOT_CUTOFF:
        if ((v[0] >= 0.0f && v[0] <= 90.0f) || v[0] == 180.0f) {
            l->spot_cutoff = v[0];
            return;
        }
        break;
    case SGI_GL_CONSTANT_ATTENUATION:
    case SGI_GL_LINEAR_ATTENUATION:
    case SGI_GL_QUADRATIC_ATTENUATION:
        if (v[0] >= 0.0f) {
            l->attenuation[pname - SGI_GL_CONSTANT_ATTENUATION] = v[0];
            return;
        }
        break;
    default:
        break;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: light parameter 0x%x value %g "
                  "ignored\n", pname, v[0]);
}

static void sgi_gl_light_model(SGIGLContext *c, uint32_t pname,
                               const float v[4])
{
    switch (pname) {
    case SGI_GL_LIGHT_MODEL_AMBIENT:
        memcpy(c->light_model_ambient, v, sizeof(c->light_model_ambient));
        return;
    case SGI_GL_LIGHT_MODEL_LOCAL_VIEWER:
        c->light_model_local_viewer = v[0] != 0.0f;
        return;
    case SGI_GL_LIGHT_MODEL_TWO_SIDE:
        c->light_model_two_side = v[0] != 0.0f;
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: light model parameter 0x%x "
                      "ignored\n", pname);
        return;
    }
}

static void sgi_gl_color_material(SGIGLContext *c, uint32_t face,
                                  uint32_t mode)
{
    switch (mode) {
    case SGI_GL_AMBIENT:
    case SGI_GL_DIFFUSE:
    case SGI_GL_SPECULAR:
    case SGI_GL_EMISSION:
    case SGI_GL_AMBIENT_AND_DIFFUSE:
        break;
    default:
        mode = 0;
        break;
    }
    if (!sgi_gl_faces(face) || !mode) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: glColorMaterial(0x%x, 0x%x) "
                      "ignored\n", face, mode);
        return;
    }
    c->color_material_face = face;
    c->color_material_mode = mode;
    sgi_gl_track_colour(c);
}

static void sgi_gl_capability(SGIGLContext *c, uint32_t cap, bool on)
{
    switch (cap) {
    case SGI_GL_DEPTH_TEST:
        c->cur.depth_test = on;
        return;
    case SGI_GL_DITHER:
        c->dither = on;
        return;
    case SGI_GL_COLOR_MATERIAL:
        c->color_material = on;
        sgi_gl_track_colour(c);
        return;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-gl: capability 0x%x is not modelled; "
                      "%s ignored\n", cap, on ? "glEnable" : "glDisable");
        return;
    }
}

bool sgi_gl_bitmap_place(SGIGLContext *c, uint32_t width, uint32_t height,
                         float xorig, float yorig, float xmove, float ymove,
                         SGIGLBitmap *b)
{
    if (!c->raster_valid) {
        return false;
    }
    /* Beyond 2^30 a corner lies off any plane; keep it there. */
    b->x = MIN(MAX(floor((double)c->raster_pos[0] - xorig), -0x40000000),
               0x40000000);
    b->y = MIN(MAX(floor((double)c->raster_pos[1] - yorig), -0x40000000),
               0x40000000);
    b->width = width;
    b->height = height;
    c->raster_pos[0] += xmove;
    c->raster_pos[1] += ymove;
    return true;
}

/*
 * The fragments of glBitmap's set bits take the raster position's depth
 * and its colour or colour index (3.7) and pass the per-fragment
 * operations like a primitive's: pixel ownership against the drawable,
 * the depth test and the draw buffers. The viewport does not bound them.
 */
void sgi_gl_bitmap_bytes(SGIGLContext *c, const SGIGLSink *sink,
                         const SGIGLBitmap *b, uint32_t first,
                         const uint8_t *bytes, uint32_t count)
{
    const SGIGLDrawable *d = &c->cur.drawable;
    SGIRasterOp op = {
        .kind = SGI_RASTER_BITMAP,
        .bitmap = {
            .state = &c->cur,
            .x = b->x,
            .y = b->y,
            .width = b->width,
            .height = b->height,
            .first = first,
            .count = count,
            .bytes = bytes,
            .z = c->raster_pos[2],
            .index = c->raster_index,
        },
    };

    if (!d->bound || !d->clip.n) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: glBitmap with no window bound "
                      "dropped\n");
        return;
    }
    memcpy(op.bitmap.rgba, c->raster_color, sizeof(op.bitmap.rgba));
    op.ntargets = sgi_gl_draw_targets(&c->cur, op.targets);
    op.clip = d->clip;
    sink->raster(sink->opaque, &op);
}

void sgi_gl_command(SGIGLContext *c, const SGIGLSink *sink,
                    const SGIGLCmd *cmd)
{
    const uint32_t *a = cmd->args;
    float pos[4];

    if (cmd->op >= ARRAY_SIZE(sgi_gl_nargs) ||
        cmd->nargs != sgi_gl_nargs[cmd->op]) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: command %u with %u "
                      "arguments ignored\n", cmd->op, cmd->nargs);
        return;
    }
    switch (cmd->op) {
    case SGI_GL_OP_VIEWPORT:
        for (int i = 0; i < 4; i++) {
            c->cur.viewport[i] = (int32_t)a[i];
        }
        return;
    case SGI_GL_OP_CLEAR_COLOR:
        memcpy(c->clear_color, a, sizeof(c->clear_color));
        return;
    case SGI_GL_OP_CLEAR_DEPTH:
        c->clear_depth = a[0];
        return;
    case SGI_GL_OP_SHADE_MODEL:
        c->cur.shade_model = a[0];
        return;
    case SGI_GL_OP_MATRIX_MODE:
        c->matrix_mode = a[0];
        return;
    case SGI_GL_OP_LOAD_MATRIX:
    case SGI_GL_OP_LOAD_IDENTITY:
    case SGI_GL_OP_MULT_MATRIX:
    case SGI_GL_OP_FRUSTUM:
    case SGI_GL_OP_ORTHO:
    case SGI_GL_OP_PUSH_MATRIX:
    case SGI_GL_OP_POP_MATRIX:
    case SGI_GL_OP_ROTATE:
    case SGI_GL_OP_SCALE:
    case SGI_GL_OP_TRANSLATE:
        sgi_gl_matrix_command(c, cmd);
        return;
    case SGI_GL_OP_ENABLE:
    case SGI_GL_OP_DISABLE:
        sgi_gl_capability(c, a[0], cmd->op == SGI_GL_OP_ENABLE);
        return;
    case SGI_GL_OP_DEPTH_FUNC:
        if (a[0] - SGI_GL_NEVER_ENUM > SGI_GL_ALWAYS) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: depth function 0x%x "
                          "ignored\n", a[0]);
            return;
        }
        c->cur.depth_func = a[0] - SGI_GL_NEVER_ENUM;
        return;
    case SGI_GL_OP_DEPTH_MASK:
        c->cur.depth_mask = a[0] != 0;
        return;
    case SGI_GL_OP_DRAW_BUFFERS:
        c->cur.draw_buffers = a[0];
        return;
    case SGI_GL_OP_COLOR3F:
        memcpy(c->cur_color, a, 3 * sizeof(float));
        c->cur_color[3] = 1.0f;
        sgi_gl_track_colour(c);
        return;
    case SGI_GL_OP_COLOR4F:
        memcpy(c->cur_color, a, sizeof(c->cur_color));
        sgi_gl_track_colour(c);
        return;
    case SGI_GL_OP_COLOR3UB:
    case SGI_GL_OP_COLOR4UB:
        /* A byte c is c / 255 (OpenGL 1.2.1, table 2.6); 3ub's alpha is 1. */
        for (int i = 0; i < 4; i++) {
            c->cur_color[i] = i == cmd->nargs ? 1.0f : (a[i] & 0xff) / 255.0f;
        }
        sgi_gl_track_colour(c);
        return;
    case SGI_GL_OP_NORMAL3F:
        memcpy(c->cur_normal, a, sizeof(c->cur_normal));
        return;
    case SGI_GL_OP_EDGE_FLAG:
        c->cur_edge = a[0] != 0;
        return;
    case SGI_GL_OP_BEGIN:
        if (a[0] > SGI_GL_POLYGON) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: glBegin of mode %u "
                          "ignored\n", a[0]);
            return;
        }
        c->in_begin = true;
        c->begin_mode = a[0];
        c->nheld = 0;
        return;
    case SGI_GL_OP_END:
        /*
         * A line loop closes from its last vertex to its first, in the
         * first vertex's colour when flat (2.13.7).
         */
        if (c->in_begin && c->begin_mode == SGI_GL_LINE_LOOP &&
            c->nheld == 2) {
            sgi_gl_submit(c, sink, SGI_GL_LINES, 2, c->held[1], c->held[0],
                          NULL, NULL);
        }
        c->in_begin = false;
        c->begin_mode = 0;
        c->nheld = 0;
        return;
    case SGI_GL_OP_VERTEX2F:
    case SGI_GL_OP_VERTEX3F:
    case SGI_GL_OP_VERTEX4F:
        memcpy(pos, a, cmd->nargs * sizeof(float));
        sgi_gl_vertex(c, sink, pos, cmd->nargs);
        return;
    case SGI_GL_OP_VERTEX2I:
        /* Integer components convert to float unscaled (2.7). */
        pos[0] = (int32_t)a[0];
        pos[1] = (int32_t)a[1];
        sgi_gl_vertex(c, sink, pos, 2);
        return;
    case SGI_GL_OP_RASTER_POS2F:
    case SGI_GL_OP_RASTER_POS3F:
    case SGI_GL_OP_RASTER_POS4F:
        pos[2] = 0.0f;
        pos[3] = 1.0f;
        memcpy(pos, a, cmd->nargs * sizeof(float));
        sgi_gl_raster_pos(c, pos);
        return;
    case SGI_GL_OP_INDEXF:
        memcpy(&c->cur_index, &a[0], sizeof(c->cur_index));
        return;
    case SGI_GL_OP_INDEX_MASK:
        c->cur.index_mask = a[0];
        return;
    case SGI_GL_OP_CLEAR_INDEX:
        memcpy(&c->clear_index, &a[0], sizeof(c->clear_index));
        return;
    case SGI_GL_OP_DEPTH_RANGE:
        sgi_gl_depth_range(c, sgi_gl_float(a[0]), sgi_gl_float(a[1]));
        return;
    case SGI_GL_OP_LIGHT:
        memcpy(pos, &a[2], sizeof(pos));
        sgi_gl_light(c, a[0], a[1], pos);
        return;
    case SGI_GL_OP_MATERIAL:
        memcpy(pos, &a[2], sizeof(pos));
        if (!sgi_gl_faces(a[0])) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: material face 0x%x "
                          "ignored\n", a[0]);
            return;
        }
        sgi_gl_material(c, sgi_gl_faces(a[0]), a[1], pos);
        return;
    case SGI_GL_OP_LIGHT_MODEL:
        memcpy(pos, &a[1], sizeof(pos));
        sgi_gl_light_model(c, a[0], pos);
        return;
    case SGI_GL_OP_COLOR_MATERIAL:
        sgi_gl_color_material(c, a[0], a[1]);
        return;
    case SGI_GL_OP_FLUSH:
        /* Every piece has already been sent. */
        return;
    case SGI_GL_OP_CLEAR:
        sgi_gl_clear(c, sink, a[0]);
        return;
    case SGI_GL_OP_FINISH:
        /* Every piece has already been sent; the fence follows them. */
        if (sink->fence) {
            sink->fence(sink->opaque, c->hw_id);
        }
        return;
    default:
        g_assert_not_reached();
    }
}

static bool sgi_gl_get_floats(const float *v, unsigned count, uint32_t *out,
                              unsigned *n)
{
    for (unsigned i = 0; i < count; i++) {
        out[i] = sgi_gl_bits(v[i]);
    }
    *n = count;
    return true;
}

bool sgi_gl_get_light(const SGIGLContext *c, uint32_t light, uint32_t pname,
                      uint32_t *out, unsigned *n)
{
    const SGIGLLight *l;

    if (light >= SGI_GL_LIGHTS) {
        return false;
    }
    l = &c->light[light];
    switch (pname) {
    case SGI_GL_AMBIENT:
        return sgi_gl_get_floats(l->ambient, 4, out, n);
    case SGI_GL_DIFFUSE:
        return sgi_gl_get_floats(l->diffuse, 4, out, n);
    case SGI_GL_SPECULAR:
        return sgi_gl_get_floats(l->specular, 4, out, n);
    case SGI_GL_POSITION:
        return sgi_gl_get_floats(l->position, 4, out, n);
    case SGI_GL_SPOT_DIRECTION:
        return sgi_gl_get_floats(l->spot_direction, 3, out, n);
    case SGI_GL_SPOT_EXPONENT:
        return sgi_gl_get_floats(&l->spot_exponent, 1, out, n);
    case SGI_GL_SPOT_CUTOFF:
        return sgi_gl_get_floats(&l->spot_cutoff, 1, out, n);
    case SGI_GL_CONSTANT_ATTENUATION:
    case SGI_GL_LINEAR_ATTENUATION:
    case SGI_GL_QUADRATIC_ATTENUATION:
        return sgi_gl_get_floats(
            &l->attenuation[pname - SGI_GL_CONSTANT_ATTENUATION], 1, out, n);
    default:
        return false;
    }
}

bool sgi_gl_get_material(const SGIGLContext *c, uint32_t face,
                         uint32_t pname, uint32_t *out, unsigned *n)
{
    const SGIGLMaterial *m;

    if (face != SGI_GL_FRONT && face != SGI_GL_BACK) {
        return false;
    }
    m = &c->material[face == SGI_GL_BACK];
    switch (pname) {
    case SGI_GL_AMBIENT:
        return sgi_gl_get_floats(m->ambient, 4, out, n);
    case SGI_GL_DIFFUSE:
        return sgi_gl_get_floats(m->diffuse, 4, out, n);
    case SGI_GL_SPECULAR:
        return sgi_gl_get_floats(m->specular, 4, out, n);
    case SGI_GL_EMISSION:
        return sgi_gl_get_floats(m->emission, 4, out, n);
    case SGI_GL_SHININESS:
        return sgi_gl_get_floats(&m->shininess, 1, out, n);
    case SGI_GL_COLOR_INDEXES:
        return sgi_gl_get_floats(m->color_indexes, 3, out, n);
    default:
        return false;
    }
}

/*
 * Texture coordinates are not kept, so GL_CURRENT_TEXTURE_COORDS and
 * GL_CURRENT_RASTER_TEXTURE_COORDS answer OpenGL's initial (0, 0, 0, 1)
 * (2.7, 2.12), wrong once a client has sent glTexCoord.
 */
bool sgi_gl_get(const SGIGLContext *c, uint32_t pname, uint32_t *out,
                unsigned *n)
{
    switch (pname) {
    case SGI_GL_CURRENT_COLOR:
        for (int i = 0; i < 4; i++) {
            out[i] = sgi_gl_bits(c->cur_color[i]);
        }
        *n = 4;
        return true;
    case SGI_GL_CURRENT_NORMAL:
        for (int i = 0; i < 3; i++) {
            out[i] = sgi_gl_bits(c->cur_normal[i]);
        }
        *n = 3;
        return true;
    case SGI_GL_CURRENT_TEXTURE_COORDS:
    case SGI_GL_CURRENT_RASTER_TEXTURE_COORDS:
        out[0] = out[1] = out[2] = 0;
        out[3] = sgi_gl_bits(1.0f);
        *n = 4;
        return true;
    case SGI_GL_EDGE_FLAG:
        out[0] = c->cur_edge;
        *n = 1;
        return true;
    case SGI_GL_SHADE_MODEL:
        out[0] = c->cur.shade_model;
        *n = 1;
        return true;
    case SGI_GL_MATRIX_MODE:
        out[0] = c->matrix_mode;
        *n = 1;
        return true;
    case SGI_GL_VIEWPORT:
        memcpy(out, c->cur.viewport, sizeof(c->cur.viewport));
        *n = 4;
        return true;
    /* Column-major, the order glLoadMatrixf takes (6.1.2). */
    case SGI_GL_MODELVIEW_MATRIX:
    case SGI_GL_PROJECTION_MATRIX:
        for (int i = 0; i < 16; i++) {
            out[i] = sgi_gl_bits(pname == SGI_GL_MODELVIEW_MATRIX ?
                                 c->cur.modelview[i] :
                                 c->cur.projection[i]);
        }
        *n = 16;
        return true;
    case SGI_GL_CURRENT_RASTER_POSITION:
    case SGI_GL_CURRENT_RASTER_COLOR:
        for (int i = 0; i < 4; i++) {
            out[i] = sgi_gl_bits(pname == SGI_GL_CURRENT_RASTER_POSITION ?
                                 c->raster_pos[i] : c->raster_color[i]);
        }
        *n = 4;
        return true;
    case SGI_GL_CURRENT_RASTER_POSITION_VALID:
        out[0] = c->raster_valid;
        *n = 1;
        return true;
    case SGI_GL_CURRENT_RASTER_DISTANCE:
        out[0] = sgi_gl_bits(c->raster_distance);
        *n = 1;
        return true;
    /* The indices as floats, the form the context keeps them in. */
    case SGI_GL_CURRENT_INDEX:
    case SGI_GL_CURRENT_RASTER_INDEX:
    case SGI_GL_INDEX_CLEAR_VALUE:
        out[0] = sgi_gl_bits(pname == SGI_GL_CURRENT_INDEX ? c->cur_index :
                             pname == SGI_GL_CURRENT_RASTER_INDEX ?
                             c->raster_index : c->clear_index);
        *n = 1;
        return true;
    case SGI_GL_INDEX_WRITEMASK:
        out[0] = c->cur.index_mask;
        *n = 1;
        return true;
    case SGI_GL_COLOR_CLEAR_VALUE:
        for (int i = 0; i < 4; i++) {
            out[i] = sgi_gl_bits(c->clear_color[i]);
        }
        *n = 4;
        return true;
    /* glGetBooleanv of a capability is glIsEnabled (6.1.1). */
    case SGI_GL_DEPTH_TEST:
        out[0] = c->cur.depth_test;
        *n = 1;
        return true;
    case SGI_GL_DITHER:
        out[0] = c->dither;
        *n = 1;
        return true;
    case SGI_GL_DEPTH_FUNC:
        out[0] = SGI_GL_NEVER_ENUM + c->cur.depth_func;
        *n = 1;
        return true;
    case SGI_GL_DEPTH_WRITEMASK:
        out[0] = c->cur.depth_mask;
        *n = 1;
        return true;
    /* The matrices a stack holds, the current one included (6.1.2). */
    case SGI_GL_MODELVIEW_STACK_DEPTH:
        out[0] = c->mv_depth + 1;
        *n = 1;
        return true;
    case SGI_GL_PROJECTION_STACK_DEPTH:
        out[0] = c->pr_depth + 1;
        *n = 1;
        return true;
    case SGI_GL_TEXTURE_STACK_DEPTH:
        out[0] = c->tx_depth + 1;
        *n = 1;
        return true;
    case SGI_GL_TEXTURE_MATRIX:
        return sgi_gl_get_floats(c->texture, 16, out, n);
    case SGI_GL_DEPTH_RANGE:
        for (int i = 0; i < 2; i++) {
            out[i] = sgi_gl_bits(c->cur.depth_range[i]);
        }
        *n = 2;
        return true;
    case SGI_GL_LIGHT_MODEL_AMBIENT:
        return sgi_gl_get_floats(c->light_model_ambient, 4, out, n);
    case SGI_GL_LIGHT_MODEL_LOCAL_VIEWER:
        out[0] = c->light_model_local_viewer;
        *n = 1;
        return true;
    case SGI_GL_LIGHT_MODEL_TWO_SIDE:
        out[0] = c->light_model_two_side;
        *n = 1;
        return true;
    case SGI_GL_COLOR_MATERIAL:
        out[0] = c->color_material;
        *n = 1;
        return true;
    case SGI_GL_COLOR_MATERIAL_FACE:
        out[0] = c->color_material_face;
        *n = 1;
        return true;
    case SGI_GL_COLOR_MATERIAL_PARAMETER:
        out[0] = c->color_material_mode;
        *n = 1;
        return true;
    default:
        return false;
    }
}
