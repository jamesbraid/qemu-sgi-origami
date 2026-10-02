/* SPDX-License-Identifier: BSD-3-Clause */
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
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/display/sgi/gl-context.h"
#include "migration/qemu-file.h"

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
        l->spot_cos_cutoff = -1.0f;
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
    c->cur.dither = 1;
    sgi_gl_identity(c->cur.modelview);
    sgi_gl_identity(c->cur.projection);
    sgi_gl_identity(c->texture);
    c->cur.depth_func = SGI_GL_LESS;
    c->cur.alpha_func = SGI_GL_ALWAYS;
    c->cur.stencil_func = SGI_GL_ALWAYS;
    c->cur.stencil_value_mask = UINT32_MAX;
    c->cur.stencil_write_mask = UINT32_MAX;
    for (int i = 0; i < 3; i++) {
        c->cur.stencil_op[i] = SGI_GL_KEEP;
    }
    c->cur.blend_src = 1;
    c->cur.blend_equation = SGI_GL_FUNC_ADD;
    c->cur.logic_op_mode = SGI_GL_COPY;
    c->cur.texture_env_mode = SGI_GL_MODULATE;
    c->cur_texcoord[3] = c->raster_texcoord[3] = 1.0f;
    c->cur.fog_mode = SGI_GL_EXP;
    c->cur.fog_density = 1.0f;
    c->cur.fog_end = 1.0f;
    c->point_size = 1.0f;
    c->line_width = 1.0f;
    c->line_stipple_repeat = 1;
    c->line_stipple_pattern = 0xffff;
    c->polygon_mode[0] = c->polygon_mode[1] = SGI_GL_FILL;
    memset(c->polygon_stipple, 0xff, sizeof(c->polygon_stipple));
    c->cur.depth_mask = 1;
    c->cur.depth_range[1] = 1.0f;
    c->clear_depth = UINT32_MAX;
    c->cur.draw_buffers = SGI_GL_DRAW_FRONT;
    c->cur.index_mask = UINT32_MAX;
    c->cur.colour_mask = SGI_GL_COLOUR_MASK_ALL;
    c->cur.cull_face_mode = SGI_GL_BACK;
    c->cur.front_face = SGI_GL_CCW;
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

static bool sgi_gl_polygon_owned(const SGIGLContext *c)
{
    return c->polygon.error ||
           (c->polygon.enabled && c->in_begin &&
            c->begin_mode == SGI_GL_POLYGON);
}

static void sgi_gl_polygon_release(SGIGLPolygonAssembly *p)
{
    g_free(p->vertices);
    p->vertices = NULL;
    p->count = p->capacity = p->failed = p->loaded = 0;
}

bool sgi_gl_enable_polygon_modes(SGIGLContext *c, const SGIGLSink *sink)
{
    if (c->in_begin || !sink || !sink->draw_checked) {
        return false;
    }
    c->polygon.enabled = 1;
    return true;
}

static bool sgi_gl_open_line(const SGIGLContext *c)
{
    return c->in_begin && (c->begin_mode == SGI_GL_LINE_STRIP ||
                           c->begin_mode == SGI_GL_LINE_LOOP);
}

bool sgi_gl_enable_line_stipple(SGIGLContext *c, const SGIGLSink *sink)
{
    if (c->in_begin || !sink || !sink->draw_checked) {
        return false;
    }
    c->standalone_stipple = true;
    return true;
}

void sgi_gl_cleanup(SGIGLContext *c)
{
    sgi_gl_polygon_release(&c->polygon);
    c->polygon.enabled = c->polygon.error = 0;
    c->standalone_stipple = c->line_phase_loaded = false;
    c->draw_error_loaded = false;
    c->line_phase = 0;
}

void sgi_gl_reset(SGIGLContext *c, uint32_t hw_id)
{
    uint32_t enabled = c->polygon.enabled;
    bool standalone = c->standalone_stipple;

    sgi_gl_cleanup(c);
    sgi_gl_init(c, hw_id);
    c->polygon.enabled = enabled;
    c->standalone_stipple = standalone;
}

/* Count and bytes are representation bounds, not hardware size limits. */
static bool sgi_gl_polygon_reserve(SGIGLPolygonAssembly *p, size_t count)
{
    size_t capacity = MAX(p->capacity, 8);
    float *vertices;

    if (count > UINT32_MAX ||
        count > SIZE_MAX / SGI_GL_VERTEX_FLOATS / sizeof(float)) {
        return false;
    }
    if (count <= p->capacity) {
        return true;
    }
    while (capacity < count) {
        if (capacity > UINT32_MAX / 2 ||
            capacity > SIZE_MAX / SGI_GL_VERTEX_FLOATS / sizeof(float) / 2) {
            capacity = count;
            break;
        }
        capacity *= 2;
    }
    vertices = g_try_realloc_n(p->vertices, capacity,
                               SGI_GL_VERTEX_FLOATS * sizeof(float));
    if (!vertices) {
        return false;
    }
    p->vertices = vertices;
    p->capacity = capacity;
    return true;
}

bool sgi_gl_clone(SGIGLContext *dst, const SGIGLContext *src)
{
    SGIGLContext copy;
    bool open = src->in_begin && src->begin_mode == SGI_GL_POLYGON;

    if (dst == src ||
        (sgi_gl_open_line(src) &&
         dst->standalone_stipple != src->standalone_stipple) ||
        (open && dst->polygon.enabled != src->polygon.enabled) ||
        (!dst->polygon.enabled &&
         (src->polygon.count || src->polygon.failed ||
          (sgi_gl_polygon_owned(src) &&
           (src->polygon.enabled || !dst->standalone_stipple))))) {
        return false;
    }
    copy = *src; /* Ownership is replaced before the copy becomes visible. */
    copy.polygon.vertices = NULL;
    copy.polygon.capacity = 0;
    copy.polygon.enabled = dst->polygon.enabled;
    copy.polygon.loaded = 0;
    copy.standalone_stipple = dst->standalone_stipple;
    copy.line_phase_loaded = copy.draw_error_loaded = false;
    if (!sgi_gl_polygon_reserve(&copy.polygon, src->polygon.count)) {
        return false;
    }
    if (src->polygon.count) {
        memcpy(copy.polygon.vertices, src->polygon.vertices,
               (size_t)src->polygon.count *
               SGI_GL_VERTEX_FLOATS * sizeof(float));
    }
    sgi_gl_cleanup(dst);
    *dst = copy;
    return true;
}

bool sgi_gl_consume_error(SGIGLContext *c, uint32_t *error)
{
    if (c->in_begin) {
        return false;
    }
    *error = c->polygon.error;
    c->polygon.error = 0;
    return true;
}

static void sgi_gl_draw_result(SGIGLContext *c, const SGIGLSink *sink,
                               const SGIGLDraw *d)
{
    if (!c->polygon.enabled && !c->standalone_stipple) {
        sink->draw(sink->opaque, d);
    } else if (!sink->draw_checked) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-gl: activated modes require checked draw callback\n");
    } else if (sink->draw_checked(sink->opaque, d) ==
               SGI_GL_DRAW_OUT_OF_MEMORY && !c->polygon.error) {
        c->polygon.error = SGI_GL_OUT_OF_MEMORY;
    }
}

bool sgi_gl_polygon_stipple(SGIGLContext *c, const uint32_t rows[32])
{
    if (c->in_begin) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-gl: polygon stipple inside glBegin: "
                      "invalid operation\n");
        return false;
    }
    memcpy(c->polygon_stipple, rows, sizeof(c->polygon_stipple));
    return true;
}

bool sgi_gl_get_polygon_stipple(const SGIGLContext *c, uint32_t rows[32])
{
    if (c->in_begin) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-gl: polygon stipple query inside glBegin: "
                      "invalid operation\n");
        return false;
    }
    memcpy(rows, c->polygon_stipple, sizeof(c->polygon_stipple));
    return true;
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

/*
 * inv = m^-1, both column-major, by cofactors in double precision; false,
 * with inv unset, for a singular or non-finite m.
 */
static bool sgi_gl_invert(const float m[16], double inv[16])
{
    double a[16], det = 0;

    for (int i = 0; i < 16; i++) {
        a[i] = m[i];
    }
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            double minor[9];
            int n = 0;

            for (int c = 0; c < 4; c++) {
                for (int r = 0; r < 4; r++) {
                    if (c != col && r != row) {
                        minor[n++] = a[c * 4 + r];
                    }
                }
            }
            /* The cofactor of a[col][row] is the adjugate's [row][col]. */
            inv[row * 4 + col] = ((row + col) & 1 ? -1 : 1) *
                (minor[0] * (minor[4] * minor[8] - minor[5] * minor[7]) -
                 minor[3] * (minor[1] * minor[8] - minor[2] * minor[7]) +
                 minor[6] * (minor[1] * minor[5] - minor[2] * minor[4]));
        }
    }
    for (int k = 0; k < 4; k++) {
        det += a[k * 4] * inv[k];
    }
    if (det == 0 || !isfinite(det)) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        inv[i] /= det;
    }
    return true;
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
    [SGI_GL_OP_CULL_FACE] = 1,
    [SGI_GL_OP_FRONT_FACE] = 1,
    [SGI_GL_OP_SCISSOR] = 4,
    [SGI_GL_OP_ALPHA_FUNC] = 2,
    [SGI_GL_OP_STENCIL_FUNC] = 3,
    [SGI_GL_OP_STENCIL_OP] = 3,
    [SGI_GL_OP_STENCIL_MASK] = 1,
    [SGI_GL_OP_CLEAR_STENCIL] = 1,
    [SGI_GL_OP_BLEND_FUNC] = 2,
    [SGI_GL_OP_BLEND_EQUATION] = 1,
    [SGI_GL_OP_BLEND_COLOR] = 4,
    [SGI_GL_OP_LOGIC_OP] = 1,
    [SGI_GL_OP_COLOR_MASK] = 4,
    [SGI_GL_OP_FOG] = 5,
    [SGI_GL_OP_POLYGON_OFFSET] = 2,
    [SGI_GL_OP_POLYGON_OFFSET_EXT] = 2,
    [SGI_GL_OP_CLIP_PLANE] = 5,
    [SGI_GL_OP_POINT_SIZE] = 1,
    [SGI_GL_OP_LINE_WIDTH] = 1,
    [SGI_GL_OP_LINE_STIPPLE] = 2,
    [SGI_GL_OP_POLYGON_MODE] = 2,
    [SGI_GL_OP_TEXCOORD2F] = 2,
    [SGI_GL_OP_TEXCOORD3F] = 3,
    [SGI_GL_OP_TEXCOORD4F] = 4,
    [SGI_GL_OP_TEX_ENV] = 5,
    [SGI_GL_OP_SWAP_TMESH] = 0,
};

uint32_t sgi_gl_draw_targets(const SGIGLDrawState *st,
                             SGIColourTarget out[SGI_GL_MAX_TARGETS])
{
    const SGIColourTarget pick[SGI_GL_MAX_TARGETS] = {
        st->drawable.front, st->drawable.back,
        st->drawable.extra[0], st->drawable.extra[1],
    };
    uint32_t n = 0;

    for (int bit = 0; bit < SGI_GL_MAX_TARGETS; bit++) {
        bool dup = false;

        if (!(st->draw_buffers & (1u << bit))) {
            continue;
        }
        if (bit >= 2 && !pick[bit]) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: draw buffer bit %d of 0x%x has "
                          "no buffer; ignored\n", bit, st->draw_buffers);
            continue;
        }
        /*
         * A drawable whose buffers are one target, as a single-buffered
         * one may be, is stored to once: a read-modify-write stage in front
         * of the store must not apply twice.
         */
        for (uint32_t i = 0; i < n; i++) {
            dup |= out[i] == pick[bit];
        }
        if (!dup) {
            out[n++] = pick[bit];
        }
    }
    if (st->draw_buffers & ~0xfu) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: draw buffers 0x%x beyond bit 3 "
                      "ignored\n", st->draw_buffers);
    }
    return n;
}

/*
 * glClear (OpenGL 1.2.1, 4.2.3) as a raster op over the drawable's
 * rectangles. A clear passes only the pixel ownership, scissor and dither
 * steps, and the viewport does not bound it; ownership is the drawable's
 * rectangles, the scissor box bounds the op's rectangle while the test is
 * enabled, and the clear colour is not dithered, whatever GL_DITHER's
 * enable, as the hardware goldens of the one head measured show. The
 * depth buffer is cleared only when the depth mask allows, the stencil
 * buffer under the stencil write mask, the colour buffers under the colour
 * or index mask, and the accumulation buffer is not modelled.
 */
static void sgi_gl_clear(SGIGLContext *c, const SGIGLSink *sink,
                         uint32_t mask)
{
    const SGIGLDrawable *d = &c->cur.drawable;
    SGIRasterOp op = {
        .kind = SGI_RASTER_CLEAR,
    };

    if (mask & ~(SGI_GL_COLOR_BUFFER_BIT | SGI_GL_DEPTH_BUFFER_BIT |
                 SGI_GL_STENCIL_BUFFER_BIT)) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: glClear of buffers 0x%x beyond "
                      "colour, depth and stencil ignored\n", mask);
    }
    op.ntargets = sgi_gl_draw_targets(&c->cur, op.targets);
    if (!d->bound) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: glClear with no window bound "
                      "dropped\n");
        return;
    }
    if (!d->clip.n) {
        /* A window others wholly cover: nothing to clear. */
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
    if (c->cur.scissor_test) {
        int64_t sx = (int64_t)d->origin[0] + c->cur.scissor[0];
        int64_t sy = (int64_t)d->origin[1] + c->cur.scissor[1];
        int64_t x0 = MAX(op.x0, sx), y0 = MAX(op.y0, sy);
        int64_t x1 = MIN(op.x1, sx + c->cur.scissor[2] - 1);
        int64_t y1 = MIN(op.y1, sy + c->cur.scissor[3] - 1);

        if (x0 > x1 || y0 > y1) {
            return;
        }
        op.x0 = x0;
        op.y0 = y0;
        op.x1 = x1;
        op.y1 = y1;
    }
    if (mask & SGI_GL_COLOR_BUFFER_BIT) {
        op.clear_buffers |= SGI_GL_COLOR_BUFFER_BIT;
        memcpy(op.clear_rgba, c->clear_color, sizeof(op.clear_rgba));
        op.clear_colour_mask = c->cur.colour_mask;
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
    if (mask & SGI_GL_STENCIL_BUFFER_BIT) {
        op.clear_buffers |= SGI_GL_STENCIL_BUFFER_BIT;
        op.clear_stencil = c->clear_stencil;
        op.stencil_mask = c->cur.stencil_write_mask;
    }
    if (op.clear_buffers) {
        sink->raster(sink->opaque, &op);
    }
}

/*
 * Whether vertices are lit: GL_LIGHTING enabled in RGBA mode. Colour-index
 * lighting (2.13.5) is not modelled; an index stays the current index.
 */
static bool sgi_gl_lit(const SGIGLContext *c)
{
    return c->lighting && !c->cur.index_bits;
}

static float sgi_gl_dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/*
 * v scaled to unit length in place; a zero vector, whose direction the
 * specification leaves undefined, stays zero and so lights nothing.
 */
static void sgi_gl_unit3(float v[3])
{
    float len = sqrtf(sgi_gl_dot3(v, v));

    if (len > 0.0f) {
        for (int i = 0; i < 3; i++) {
            v[i] /= len;
        }
    }
}

/*
 * The normal in eye coordinates (OpenGL 1.2.1, 2.10.3): n' = n M^-1 for M
 * the modelview's upper left 3x3, rescaled when GL_RESCALE_NORMAL is
 * enabled and then made unit length when GL_NORMALIZE is. n M^-1 is M's
 * cofactor matrix applied to n and divided by M's determinant. A singular
 * M, for which the specification gives no normal, leaves n unchanged.
 * A family's own normal matrix N replaces both M^-1 and the rescale:
 * n' = N n, as given.
 */
static void sgi_gl_eye_normal(const SGIGLContext *c, const float n[3],
                              float out[3])
{
    const float *m = c->cur.modelview;

    if (c->normal_matrix_given) {
        const float *nm = c->normal_matrix;

        for (int j = 0; j < 3; j++) {
            out[j] = nm[j] * n[0] + nm[3 + j] * n[1] + nm[6 + j] * n[2];
        }
        if (c->normalize || c->normalize_always) {
            sgi_gl_unit3(out);
        }
        return;
    }
    /* M[row][col] is m[col * 4 + row]. */
#define M(r, k) m[(k) * 4 + (r)]
    float cof[3][3] = {
        { M(1, 1) * M(2, 2) - M(1, 2) * M(2, 1),
          M(1, 2) * M(2, 0) - M(1, 0) * M(2, 2),
          M(1, 0) * M(2, 1) - M(1, 1) * M(2, 0) },
        { M(0, 2) * M(2, 1) - M(0, 1) * M(2, 2),
          M(0, 0) * M(2, 2) - M(0, 2) * M(2, 0),
          M(0, 1) * M(2, 0) - M(0, 0) * M(2, 1) },
        { M(0, 1) * M(1, 2) - M(0, 2) * M(1, 1),
          M(0, 2) * M(1, 0) - M(0, 0) * M(1, 2),
          M(0, 0) * M(1, 1) - M(0, 1) * M(1, 0) },
    };
    float det = M(0, 0) * cof[0][0] + M(0, 1) * cof[0][1] +
                M(0, 2) * cof[0][2];
#undef M

    if (det == 0.0f || !isfinite(det)) {
        memcpy(out, n, 3 * sizeof(float));
    } else {
        /* (n M^-1)_j = sum_i n_i cof[j][i] / det: M^-1[i][j] = cof[j][i]. */
        for (int j = 0; j < 3; j++) {
            out[j] = (n[0] * cof[j][0] + n[1] * cof[j][1] +
                      n[2] * cof[j][2]) / det;
        }
        if (c->rescale_normal) {
            /* 1 / |row 3 of M^-1|: row 3 is cof[.][2] / det. */
            float r[3] = { cof[0][2] / det, cof[1][2] / det,
                           cof[2][2] / det };
            float len = sqrtf(sgi_gl_dot3(r, r));

            if (len > 0.0f) {
                for (int i = 0; i < 3; i++) {
                    out[i] /= len;
                }
            }
        }
    }
    if (c->normalize || c->normalize_always) {
        sgi_gl_unit3(out);
    }
}

/*
 * One face's lit colour (OpenGL 1.2.1, 2.13.1), clamped to [0, 1] (2.13.6):
 *
 *   c = e + a * as + sum over enabled lights i of
 *       att_i spot_i (a * a_i + (n . VP_i) d * d_i +
 *                     f_i (n . h_i)^s s * s_i)
 *
 * with the face's material e, a, d, s and shininess, the light model's
 * ambient as, and alpha the material's diffuse alpha. V is the vertex in
 * eye coordinates and n the face's normal. VP_i is the unit vector from V
 * to the light: the light's own direction for a light at infinity (w 0),
 * the reverse of V's direction for a vertex at infinity, and the light's
 * direction less V's when both are; a dot product below 0 counts as 0,
 * and f_i is 1 when n . VP_i does not.
 * h_i is VP_i plus the unit vector from V to the eye, (0, 0, 1) without a
 * local viewer, made unit length. att_i is 1 / (k0 + k1 d + k2 d^2) at the
 * distance d from V to a light with w not 0, and 1 otherwise; spot_i is 1
 * for a cutoff of 180 and otherwise (PV_i . sd_i)^exponent, where PV_i is
 * the unit vector from the light to V, the reverse of VP_i, and sd_i the
 * unit spot direction, or 0 if that product is below the cutoff's cosine,
 * spot_cos_cutoff.
 * The specification reads spot_i the same for a light at infinity, and so
 * does this.
 *
 * Approximated: this is the specification's equation in single-precision
 * arithmetic in the order written, not a head's geometry-engine
 * arithmetic, whose precision and ordering are not published. A head's
 * own lit pixels would settle how far they differ.
 */
static void sgi_gl_light_face(const SGIGLContext *c, const float eye[4],
                              const float n[3], const SGIGLMaterial *m,
                              float out[4])
{
    float sum[3];

    for (int k = 0; k < 3; k++) {
        sum[k] = m->emission[k] + m->ambient[k] * c->light_model_ambient[k];
    }
    for (int i = 0; i < SGI_GL_LIGHTS; i++) {
        const SGIGLLight *l = &c->light[i];
        const float *p = l->position;
        float vp[3], h[3], sd[3], ndl, ndh, att = 1.0f, spot = 1.0f;

        if (!(c->lights_enabled & (1u << i))) {
            continue;
        }
        /* The vector from V to the light (2.13.1's P1P2). */
        for (int k = 0; k < 3; k++) {
            if (p[3] == 0.0f && eye[3] == 0.0f) {
                vp[k] = p[k] - eye[k];
            } else if (p[3] == 0.0f) {
                vp[k] = p[k];
            } else if (eye[3] == 0.0f) {
                vp[k] = -eye[k];
            } else {
                vp[k] = p[k] / p[3] - eye[k] / eye[3];
            }
        }
        if (p[3] != 0.0f) {
            float d = sqrtf(sgi_gl_dot3(vp, vp));

            att = 1.0f / (l->attenuation[0] + l->attenuation[1] * d +
                          l->attenuation[2] * d * d);
        }
        sgi_gl_unit3(vp);
        if (l->spot_cutoff != 180.0f) {
            float s;

            memcpy(sd, l->spot_direction, sizeof(sd));
            sgi_gl_unit3(sd);
            s = -sgi_gl_dot3(vp, sd);
            spot = s >= l->spot_cos_cutoff ?
                   powf(MAX(s, 0.0f), l->spot_exponent) : 0.0f;
        }
        ndl = MAX(sgi_gl_dot3(n, vp), 0.0f);
        /* The half vector, with the eye's direction from V. */
        for (int k = 0; k < 3; k++) {
            float ve = k == 2 ? 1.0f : 0.0f;

            if (c->light_model_local_viewer) {
                ve = eye[3] == 0.0f ? -eye[k] : -eye[k] / eye[3];
            }
            h[k] = ve;
        }
        if (c->light_model_local_viewer) {
            sgi_gl_unit3(h);
        }
        for (int k = 0; k < 3; k++) {
            h[k] += vp[k];
        }
        sgi_gl_unit3(h);
        ndh = ndl != 0.0f ? powf(MAX(sgi_gl_dot3(n, h), 0.0f),
                                 m->shininess) : 0.0f;
        for (int k = 0; k < 3; k++) {
            sum[k] += att * spot * (m->ambient[k] * l->ambient[k] +
                                    ndl * m->diffuse[k] * l->diffuse[k] +
                                    ndh * m->specular[k] * l->specular[k]);
        }
    }
    for (int k = 0; k < 3; k++) {
        out[k] = MIN(MAX(sum[k], 0.0f), 1.0f);
    }
    out[3] = MIN(MAX(m->diffuse[3], 0.0f), 1.0f);
}

/*
 * Light a vertex at object position obj with normal n: its front colour
 * into front and, with two-sided lighting, its back colour into back,
 * from the back material and the reversed normal (2.13.1). Without
 * two-sided lighting a back-facing polygon takes the front colour, and
 * back is the front colour.
 */
static void sgi_gl_light_vertex(const SGIGLContext *c, const float obj[4],
                                const float n[3], float front[4],
                                float back[4])
{
    const float *mv = c->cur.modelview;
    float eye[4], normal[3];

    for (int row = 0; row < 4; row++) {
        eye[row] = mv[row] * obj[0] + mv[4 + row] * obj[1] +
                   mv[8 + row] * obj[2] + mv[12 + row] * obj[3];
    }
    sgi_gl_eye_normal(c, n, normal);
    sgi_gl_light_face(c, eye, normal, &c->material[0], front);
    if (!c->light_model_two_side) {
        memcpy(back, front, 4 * sizeof(float));
        return;
    }
    for (int k = 0; k < 3; k++) {
        normal[k] = -normal[k];
    }
    sgi_gl_light_face(c, eye, normal, &c->material[1], back);
}

/* Send one piece of the open primitive: count of v0 to v3. */
static void sgi_gl_submit(SGIGLContext *c, const SGIGLSink *sink,
                          uint32_t mode, uint32_t count, const float *v0,
                          const float *v1, const float *v2, const float *v3)
{
    bool stipple = mode == SGI_GL_LINES ? c->standalone_stipple :
                                        c->polygon.enabled;
    const float *v[4] = { v0, v1, v2, v3 };
    float verts[4][SGI_GL_VERTEX_FLOATS];
    SGIGLDraw d = {
        .context = c->hw_id,
        .mode = mode,
        .count = count,
        .vertices = &verts[0][0],
        .back_colors = sgi_gl_lit(c) && c->light_model_two_side,
        .state = &c->cur,
        .point_size = c->point_size,
        .line_width = c->line_width,
        .polygon_modes = c->polygon.enabled ? c->polygon_mode : NULL,
        .line_stipple_enabled = stipple &&
            !!(c->raster_enables & SGI_GL_RASTER_LINE_STIPPLE),
        .line_stipple_repeat = stipple ? c->line_stipple_repeat : 0,
        .line_stipple_pattern =
            stipple ? c->line_stipple_pattern : 0,
        .line_phase = mode == SGI_GL_LINES && c->standalone_stipple ?
                      &c->line_phase : NULL,
        .line_phase_reset = c->begin_mode == SGI_GL_LINES,
        .polygon_stipple = c->polygon_stipple,
        .polygon_stipple_enabled =
            !!(c->raster_enables & SGI_GL_RASTER_POLYGON_STIPPLE),
    };

    for (uint32_t i = 0; i < count; i++) {
        memcpy(verts[i], v[i], sizeof(verts[i]));
    }
    sgi_gl_draw_result(c, sink, &d);
}

/* Texture coordinates transform before clipping (OpenGL 1.2.1, 2.10.2). */
static void sgi_gl_texture_coord(const SGIGLContext *c, float out[4])
{
    for (int row = 0; row < 4; row++) {
        double value = 0;

        for (int k = 0; k < 4; k++) {
            value += (double)c->texture[k * 4 + row] * c->cur_texcoord[k];
        }
        out[row] = value;
    }
}

/* Hold a vertex in slot n of the three a context keeps. */
static void sgi_gl_hold(SGIGLContext *c, unsigned int n, const float *v)
{
    memcpy(c->held[n], v, sizeof(c->held[n]));
}

void sgi_gl_capture_vertex(SGIGLContext *c, uint32_t mode,
                           const uint32_t *bits, unsigned int components,
                           float out[SGI_GL_VERTEX_FLOATS])
{
    memset(out, 0, SGI_GL_VERTEX_FLOATS * sizeof(float));
    out[SGI_GL_V_POSITION + 3] = 1.0f;
    memcpy(&out[SGI_GL_V_POSITION], bits, components * sizeof(uint32_t));
    memcpy(&out[SGI_GL_V_COLOR], c->cur_color, sizeof(c->cur_color));
    memcpy(&out[SGI_GL_V_NORMAL], c->cur_normal, sizeof(c->cur_normal));
    sgi_gl_texture_coord(c, &out[SGI_GL_V_TEXCOORD]);
    memcpy(&out[SGI_GL_V_EDGE], &c->cur_edge, sizeof(c->cur_edge));
    out[SGI_GL_V_INDEX] = c->cur_index;
    /* Material and lighting belong to the instant this vertex arrived. */
    if (sgi_gl_lit(c)) {
        sgi_gl_light_vertex(c, &out[SGI_GL_V_POSITION], c->cur_normal,
                            &out[SGI_GL_V_COLOR], &out[SGI_GL_V_BACK_COLOR]);
    } else {
        memcpy(&out[SGI_GL_V_BACK_COLOR], c->cur_color,
               sizeof(c->cur_color));
    }
    if (c->polygon.enabled &&
        (mode == SGI_GL_TRIANGLE_STRIP || mode == SGI_GL_TRIANGLE_FAN ||
         mode == SGI_GL_QUAD_STRIP)) {
        uint32_t edge = 1;

        memcpy(&out[SGI_GL_V_EDGE], &edge, sizeof(edge));
    }
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
    sgi_gl_capture_vertex(c, c->begin_mode, (const uint32_t *)pos,
                          components, cur);
    if (c->polygon.enabled && c->begin_mode == SGI_GL_POLYGON) {
        SGIGLPolygonAssembly *p = &c->polygon;

        if (p->failed) {
            return;
        }
        if (p->count == UINT32_MAX ||
            !sgi_gl_polygon_reserve(p, (size_t)p->count + 1)) {
            sgi_gl_polygon_release(p);
            p->failed = 1;
            if (!p->error) {
                p->error = SGI_GL_OUT_OF_MEMORY;
            }
            return;
        }
        memcpy(&p->vertices[(size_t)p->count * SGI_GL_VERTEX_FLOATS], cur,
               sizeof(cur));
        p->count++;
        return;
    }

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
        if (c->strip_swap && c->strip_prev) {
            /*
             * After a swap the vertex replaces the middle one of the
             * three the strip keeps, so its triangle shares the
             * strip's oldest and newest vertices, and keeps the facing
             * of the triangle before it, as a fan's does. The oldest
             * stays the oldest for the next swap.
             */
            if (c->strip_odd) {
                sgi_gl_submit(c, sink, SGI_GL_TRIANGLES, 3, h2, h1, cur,
                              NULL);
            } else {
                sgi_gl_submit(c, sink, SGI_GL_TRIANGLES, 3, h1, h2, cur,
                              NULL);
            }
            c->strip_swap = 0;
            sgi_gl_hold(c, 0, h1);
            sgi_gl_hold(c, 1, cur);
            return;
        }
        c->strip_swap = 0;
        if (c->strip_odd) {
            sgi_gl_submit(c, sink, SGI_GL_TRIANGLES, 3, h1, h0, cur, NULL);
        } else {
            sgi_gl_submit(c, sink, SGI_GL_TRIANGLES, 3, h0, h1, cur, NULL);
        }
        c->strip_odd = !c->strip_odd;
        sgi_gl_hold(c, 2, h0);
        c->strip_prev = 1;
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
 * and projection matrices like a vertex. Inside the view volume and on
 * the kept side of every enabled client clip plane (2.11) it
 * becomes the current raster position in window coordinates, keeping
 * clip w, with the eye distance, the current colour clamped to [0, 1]
 * (2.13.6), or with lighting enabled the front colour the current normal
 * lights there, and the current colour index.
 * Outside, only the valid bit is reset: the specification leaves the
 * rest undefined, and keeping it is the model's choice. Window depth goes
 * through the depth range, as for a draw. The eye distance is the
 * exact Euclidean one of the eye x, y and z, which 2.12 allows a head to
 * approximate; how a head does is not published. A window matrix, when
 * the draw state has one, takes the projection's, the viewport's and the
 * depth range's places here as in a draw; a bitmap's rows still run up
 * window y, which under window_rows_down is down the display.
 */
static void sgi_gl_raster_pos(SGIGLContext *c, const float obj[4])
{
    const float *mv = c->cur.modelview;
    double eye[4], clip[4], win[3];

    for (int row = 0; row < 4; row++) {
        eye[row] = 0;
        for (int k = 0; k < 4; k++) {
            eye[row] += (double)mv[k * 4 + row] * obj[k];
        }
    }
    sgi_gl_eye_to_clip(&c->cur, eye, clip);
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
    for (int i = 0; i < SGI_GL_CLIP_PLANES; i++) {
        const float *p = c->cur.clip_plane[i];

        if ((c->cur.clip_planes & (1u << i)) &&
            !(p[0] * eye[0] + p[1] * eye[1] + p[2] * eye[2] +
              p[3] * eye[3] >= 0)) {
            c->raster_valid = 0;
            return;
        }
    }
    c->raster_valid = 1;
    sgi_gl_clip_to_window(&c->cur, clip, win);
    c->raster_pos[0] = win[0];
    c->raster_pos[1] = win[1];
    c->raster_pos[2] = c->cur.window_matrix_given ? win[2] :
                       c->cur.depth_range[0] +
                       (c->cur.depth_range[1] - c->cur.depth_range[0]) *
                       win[2];
    c->raster_pos[3] = clip[3];
    c->raster_distance = sqrt(eye[0] * eye[0] + eye[1] * eye[1] +
                              eye[2] * eye[2]);
    if (sgi_gl_lit(c)) {
        float back[4];

        sgi_gl_light_vertex(c, obj, c->cur_normal, c->raster_color, back);
    } else {
        for (int i = 0; i < 4; i++) {
            c->raster_color[i] = MIN(MAX(c->cur_color[i], 0.0f), 1.0f);
        }
    }
    c->raster_index = c->cur_index;
    sgi_gl_texture_coord(c, c->raster_texcoord);
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
 * The faces a glMaterial, glColorMaterial or glCullFace face names, as a
 * mask of material[] entries, or 0 for a value that names none.
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

float sgi_gl_spot_cos(float degrees)
{
    return degrees == 180.0f ? -1.0f :
           cosf(degrees * (float)M_PI / 180.0f);
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
            l->spot_cos_cutoff = sgi_gl_spot_cos(v[0]);
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

bool sgi_gl_set_light(SGIGLContext *c, uint32_t light,
                      const SGIGLEyeLight *e)
{
    SGIGLLight *l;

    if (light >= SGI_GL_LIGHTS) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: light %u ignored\n", light);
        return false;
    }
    l = &c->light[light];
    memcpy(l->position, e->position, sizeof(l->position));
    memcpy(l->spot_direction, e->spot_direction, sizeof(l->spot_direction));
    l->spot_exponent = e->spot_exponent;
    memcpy(l->attenuation, e->attenuation, sizeof(l->attenuation));
    memcpy(l->ambient, e->ambient, sizeof(l->ambient));
    memcpy(l->diffuse, e->diffuse, sizeof(l->diffuse));
    memcpy(l->specular, e->specular, sizeof(l->specular));
    /*
     * The cosine is kept as given, so the cone's edge falls where the
     * family's board puts it; the degrees are only glGet's answer.
     */
    if (e->spot_cos_cutoff <= -1.0f) {
        l->spot_cos_cutoff = -1.0f;
        l->spot_cutoff = 180.0f;
    } else {
        l->spot_cos_cutoff = MIN(e->spot_cos_cutoff, 1.0f);
        l->spot_cutoff = acos(l->spot_cos_cutoff) * 180.0 / M_PI;
    }
    return true;
}

void sgi_gl_set_normal_matrix(SGIGLContext *c, const float m[9])
{
    c->normal_matrix_given = m != NULL;
    if (m) {
        memcpy(c->normal_matrix, m, sizeof(c->normal_matrix));
    } else {
        memset(c->normal_matrix, 0, sizeof(c->normal_matrix));
    }
}

void sgi_gl_set_window_matrix(SGIGLContext *c, const float m[16],
                              float depth_scale, bool rows_down)
{
    SGIGLDrawState *st = &c->cur;

    st->window_matrix_given = m != NULL;
    if (m) {
        memcpy(st->window_matrix, m, sizeof(st->window_matrix));
    } else {
        memset(st->window_matrix, 0, sizeof(st->window_matrix));
    }
    st->window_depth_scale = m ? depth_scale : 0;
    st->window_rows_down = m && rows_down;
}

void sgi_gl_eye_to_clip(const SGIGLDrawState *st, const double eye[4],
                        double clip[4])
{
    const float *m = st->window_matrix_given ? st->window_matrix :
                                               st->projection;

    for (int row = 0; row < 4; row++) {
        clip[row] = 0;
        for (int k = 0; k < 4; k++) {
            clip[row] += m[k * 4 + row] * eye[k];
        }
    }
    if (st->window_matrix_given) {
        clip[0] /= SGI_GL_WINDOW_GUARD;
        clip[1] /= SGI_GL_WINDOW_GUARD;
        clip[2] = 2.0 * st->window_depth_scale * clip[2] - clip[3];
    }
}

void sgi_gl_clip_to_window(const SGIGLDrawState *st, const double clip[4],
                           double out[3])
{
    const int32_t *vp = st->viewport;

    if (st->window_matrix_given) {
        out[0] = clip[0] * SGI_GL_WINDOW_GUARD / clip[3];
        out[1] = clip[1] * SGI_GL_WINDOW_GUARD / clip[3];
    } else {
        /* 2.10.1: xw = (px / 2) xd + ox, ox = x + w / 2. */
        out[0] = (clip[0] / clip[3] + 1.0) * vp[2] / 2.0 + vp[0];
        out[1] = (clip[1] / clip[3] + 1.0) * vp[3] / 2.0 + vp[1];
    }
    out[2] = (clip[2] / clip[3] + 1.0) / 2.0;
}

void sgi_gl_set_normalize_always(SGIGLContext *c, bool on)
{
    c->normalize_always = on;
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

/*
 * glFogfv (3.10). A negative density or a mode other than GL_LINEAR,
 * GL_EXP or GL_EXP2 is an error and changes nothing; the colour is
 * clamped to [0, 1].
 */
static void sgi_gl_fog(SGIGLContext *c, uint32_t pname, const float v[4])
{
    uint32_t mode = v[0] >= 0.0f && v[0] < 65536.0f ? (uint32_t)v[0] : 0;

    switch (pname) {
    case SGI_GL_FOG_MODE:
        if (mode == SGI_GL_LINEAR || mode == SGI_GL_EXP ||
            mode == SGI_GL_EXP2) {
            c->cur.fog_mode = mode;
            return;
        }
        break;
    case SGI_GL_FOG_DENSITY:
        if (v[0] >= 0.0f) {
            c->cur.fog_density = v[0];
            return;
        }
        break;
    case SGI_GL_FOG_START:
        c->cur.fog_start = v[0];
        return;
    case SGI_GL_FOG_END:
        c->cur.fog_end = v[0];
        return;
    case SGI_GL_FOG_INDEX:
        c->cur.fog_index = v[0];
        return;
    case SGI_GL_FOG_COLOR:
        for (int i = 0; i < 4; i++) {
            c->cur.fog_color[i] = v[i] >= 1.0f ? 1.0f :
                                  v[i] > 0.0f ? v[i] : 0.0f;
        }
        return;
    default:
        break;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: glFog parameter 0x%x value %g "
                  "ignored\n", pname, v[0]);
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

/* Whether op is one of glStencilOp's operations (4.1.5). */
static bool sgi_gl_stencil_op_valid(uint32_t op)
{
    switch (op) {
    case 0:
    case SGI_GL_KEEP:
    case SGI_GL_REPLACE:
    case SGI_GL_INCR:
    case SGI_GL_DECR:
    case SGI_GL_INVERT:
        return true;
    default:
        return false;
    }
}

/*
 * Whether glBlendFunc accepts factor as a source factor (OpenGL 1.2.1,
 * table 4.1, with the imaging subset's constant factors) or, with dst,
 * as a destination one (table 4.2).
 */
static bool sgi_gl_blend_factor_valid(uint32_t factor, bool dst)
{
    switch (factor) {
    case 0:
    case 1:
    case SGI_GL_SRC_ALPHA:
    case SGI_GL_ONE_MINUS_SRC_ALPHA:
    case SGI_GL_DST_ALPHA:
    case SGI_GL_ONE_MINUS_DST_ALPHA:
    case SGI_GL_CONSTANT_COLOR:
    case SGI_GL_ONE_MINUS_CONSTANT_COLOR:
    case SGI_GL_CONSTANT_ALPHA:
    case SGI_GL_ONE_MINUS_CONSTANT_ALPHA:
        return true;
    case SGI_GL_SRC_COLOR:
    case SGI_GL_ONE_MINUS_SRC_COLOR:
        return dst;
    case SGI_GL_DST_COLOR:
    case SGI_GL_ONE_MINUS_DST_COLOR:
    case SGI_GL_SRC_ALPHA_SATURATE:
        return !dst;
    default:
        return false;
    }
}

/* Whether glBlendEquation accepts equation. */
static bool sgi_gl_blend_equation_valid(uint32_t equation)
{
    switch (equation) {
    case SGI_GL_FUNC_ADD:
    case SGI_GL_MIN:
    case SGI_GL_MAX:
    case SGI_GL_FUNC_SUBTRACT:
    case SGI_GL_FUNC_REVERSE_SUBTRACT:
    case SGI_GL_LOGIC_OP:
        return true;
    default:
        return false;
    }
}

/*
 * glClipPlane (2.11): the plane p in object coordinates becomes p M^-1 in
 * eye coordinates, M the modelview current at the call. A plane number
 * the context does not keep, or a modelview with no inverse, which the
 * specification leaves undefined, changes nothing.
 */
static void sgi_gl_clip_plane(SGIGLContext *c, uint32_t plane,
                              const float p[4])
{
    double inv[16];

    if (plane >= SGI_GL_CLIP_PLANES ||
        !sgi_gl_invert(c->cur.modelview, inv)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: glClipPlane of plane %u "
                      "ignored\n", plane);
        return;
    }
    for (int j = 0; j < 4; j++) {
        double v = 0;

        /* (p M^-1)_j = sum_i p_i M^-1[i][j], M^-1[i][j] at inv[j * 4 + i]. */
        for (int i = 0; i < 4; i++) {
            v += p[i] * inv[j * 4 + i];
        }
        c->cur.clip_plane[plane][j] = v;
    }
}

/*
 * The rasterization enables kept but not drawn with, as their
 * raster_enables bits; 0 for any other capability.
 */
static uint32_t sgi_gl_raster_enable(uint32_t cap)
{
    switch (cap) {
    case SGI_GL_POINT_SMOOTH:
        return SGI_GL_RASTER_POINT_SMOOTH;
    case SGI_GL_LINE_SMOOTH:
        return SGI_GL_RASTER_LINE_SMOOTH;
    case SGI_GL_LINE_STIPPLE:
        return SGI_GL_RASTER_LINE_STIPPLE;
    case SGI_GL_POLYGON_SMOOTH:
        return SGI_GL_RASTER_POLYGON_SMOOTH;
    case SGI_GL_POLYGON_STIPPLE:
        return SGI_GL_RASTER_POLYGON_STIPPLE;
    default:
        return 0;
    }
}

/*
 * glPointSize, glLineWidth, glLineStipple and glPolygonMode (3.3 to 3.5):
 * a size or width not above 0 or a face or mode the specification lacks
 * is an error and changes nothing, and the repeat clamps to [1, 256].
 * The state is kept and answered, and a value the rasterizer does not
 * draw is logged.
 */
static void sgi_gl_raster_state(SGIGLContext *c, uint16_t op,
                                const uint32_t *a)
{
    float f = sgi_gl_float(a[0]);
    int32_t repeat;
    unsigned faces;

    switch (op) {
    case SGI_GL_OP_POINT_SIZE:
    case SGI_GL_OP_LINE_WIDTH:
        if (c->in_begin) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-gl: size setter inside glBegin: "
                          "invalid operation\n");
            return;
        }
        if (!isfinite(f) || !(f > 0.0f)) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: %s(%g) ignored\n",
                          op == SGI_GL_OP_POINT_SIZE ? "glPointSize" :
                          "glLineWidth", f);
            return;
        }
        *(op == SGI_GL_OP_POINT_SIZE ? &c->point_size : &c->line_width) = f;
        return;
    case SGI_GL_OP_LINE_STIPPLE:
        if (c->in_begin) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-gl: line stipple inside glBegin: "
                          "invalid operation\n");
            return;
        }
        repeat = a[0];
        c->line_stipple_repeat = MIN(MAX(repeat, 1), 256);
        c->line_stipple_pattern = a[1] & 0xffff;
        return;
    default:
        if (c->in_begin) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-gl: polygon mode inside glBegin: "
                          "invalid operation\n");
            return;
        }
        faces = a[0] == SGI_GL_FRONT_AND_BACK ? 3 :
                a[0] == SGI_GL_FRONT ? 1 : a[0] == SGI_GL_BACK ? 2 : 0;
        if (!faces || a[1] - SGI_GL_POINT > SGI_GL_FILL - SGI_GL_POINT) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: glPolygonMode(0x%x, "
                          "0x%x) ignored\n", a[0], a[1]);
            return;
        }
        for (int i = 0; i < 2; i++) {
            if (faces & (1u << i)) {
                c->polygon_mode[i] = a[1];
            }
        }
        if (a[1] != SGI_GL_FILL && !c->polygon.enabled) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: polygon mode 0x%x is kept; "
                          "polygons are drawn filled\n", a[1]);
        }
        return;
    }
}

static void sgi_gl_capability(SGIGLContext *c, uint32_t cap, bool on)
{
    uint32_t raster = sgi_gl_raster_enable(cap);

    if (raster) {
        if ((cap == SGI_GL_POLYGON_STIPPLE || cap == SGI_GL_LINE_STIPPLE) &&
            c->in_begin) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-gl: stipple enable inside glBegin: "
                          "invalid operation\n");
            return;
        }
        c->raster_enables = on ? c->raster_enables | raster :
                                 c->raster_enables & ~raster;
        if (on && cap == SGI_GL_LINE_STIPPLE && !c->standalone_stipple) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: standalone line stipple is "
                          "kept; standalone lines are drawn without it\n");
        } else if (on && cap != SGI_GL_POLYGON_STIPPLE &&
                   cap != SGI_GL_LINE_STIPPLE) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: capability 0x%x is kept; "
                          "drawn without it\n", cap);
        }
        return;
    }
    switch (cap) {
    case SGI_GL_DEPTH_TEST:
        c->cur.depth_test = on;
        return;
    case SGI_GL_DITHER:
        c->cur.dither = on;
        return;
    case SGI_GL_CULL_FACE:
        c->cur.cull_face = on;
        return;
    case SGI_GL_SCISSOR_TEST:
        c->cur.scissor_test = on;
        return;
    case SGI_GL_ALPHA_TEST:
        c->cur.alpha_test = on;
        return;
    case SGI_GL_STENCIL_TEST:
        c->cur.stencil_test = on;
        return;
    case SGI_GL_BLEND:
        c->cur.blend = on;
        return;
    case SGI_GL_INDEX_LOGIC_OP:
        c->cur.index_logic_op = on;
        return;
    case SGI_GL_COLOR_LOGIC_OP:
        c->cur.color_logic_op = on;
        return;
    case SGI_GL_TEXTURE_2D:
        c->cur.texture_2d = on;
        return;
    case SGI_GL_FOG:
        c->cur.fog = on;
        return;
    case SGI_GL_POLYGON_OFFSET_FILL:
        c->cur.polygon_offset = deposit32(c->cur.polygon_offset, 0, 1, on);
        return;
    case SGI_GL_POLYGON_OFFSET_LINE:
        c->cur.polygon_offset = deposit32(c->cur.polygon_offset, 1, 1, on);
        return;
    case SGI_GL_POLYGON_OFFSET_POINT:
        c->cur.polygon_offset = deposit32(c->cur.polygon_offset, 2, 1, on);
        return;
    case SGI_GL_CLIP_PLANE0 ... SGI_GL_CLIP_PLANE0 + SGI_GL_CLIP_PLANES - 1:
        c->cur.clip_planes = deposit32(c->cur.clip_planes,
                                       cap - SGI_GL_CLIP_PLANE0, 1, on);
        return;
    case SGI_GL_COLOR_MATERIAL:
        c->color_material = on;
        sgi_gl_track_colour(c);
        return;
    case SGI_GL_LIGHTING:
        c->lighting = on;
        if (on && c->cur.index_bits) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: colour-index lighting is not "
                          "modelled; indices stay unlit\n");
        }
        return;
    case SGI_GL_NORMALIZE:
        c->normalize = on;
        return;
    case SGI_GL_RESCALE_NORMAL:
        c->rescale_normal = on;
        return;
    case SGI_GL_LIGHT0 ... SGI_GL_LIGHT0 + SGI_GL_LIGHTS - 1:
        c->lights_enabled = deposit32(c->lights_enabled, cap - SGI_GL_LIGHT0,
                                      1, on);
        return;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-gl: capability 0x%x is not modelled; "
                      "%s ignored\n", cap, on ? "glEnable" : "glDisable");
        return;
    }
}

void sgi_gl_set_lighting(SGIGLContext *c, bool on)
{
    sgi_gl_capability(c, SGI_GL_LIGHTING, on);
}

void sgi_gl_set_local_viewer(SGIGLContext *c, bool on)
{
    c->light_model_local_viewer = on;
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
            .distance = c->raster_distance,
        },
    };

    if (!d->bound) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: glBitmap with no window bound "
                      "dropped\n");
        return;
    }
    if (!d->clip.n) {
        /* A window others wholly cover: the image draws nothing. */
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
    case SGI_GL_OP_TEXCOORD2F:
    case SGI_GL_OP_TEXCOORD3F:
    case SGI_GL_OP_TEXCOORD4F:
        c->cur_texcoord[2] = 0;
        c->cur_texcoord[3] = 1;
        memcpy(c->cur_texcoord, a, cmd->nargs * sizeof(float));
        return;
    case SGI_GL_OP_TEX_ENV:
        if (a[0] == SGI_GL_TEXTURE_ENV_MODE) {
            float mode = sgi_gl_float(a[1]);

            if (mode == SGI_GL_REPLACE || mode == SGI_GL_MODULATE ||
                mode == SGI_GL_DECAL || mode == SGI_GL_BLEND) {
                c->cur.texture_env_mode = mode;
                return;
            }
        } else if (a[0] == SGI_GL_TEXTURE_ENV_COLOR) {
            for (int i = 0; i < 4; i++) {
                float v = sgi_gl_float(a[i + 1]);

                c->cur.texture_env_color[i] = v >= 1 ? 1 : v > 0 ? v : 0;
            }
            return;
        }
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: texture environment "
                      "parameter 0x%x value %g ignored\n", a[0],
                      sgi_gl_float(a[1]));
        return;
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
    case SGI_GL_OP_CULL_FACE:
        if (!sgi_gl_faces(a[0])) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: glCullFace(0x%x) "
                          "ignored\n", a[0]);
            return;
        }
        c->cur.cull_face_mode = a[0];
        return;
    case SGI_GL_OP_FRONT_FACE:
        if (a[0] != SGI_GL_CW && a[0] != SGI_GL_CCW) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: glFrontFace(0x%x) "
                          "ignored\n", a[0]);
            return;
        }
        c->cur.front_face = a[0];
        return;
    case SGI_GL_OP_DRAW_BUFFERS:
        c->cur.draw_buffers = a[0];
        return;
    case SGI_GL_OP_SCISSOR:
        /* A negative width or height is an error and changes nothing. */
        if ((int32_t)a[2] < 0 || (int32_t)a[3] < 0) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: glScissor of %d by %d "
                          "ignored\n", (int32_t)a[2], (int32_t)a[3]);
            return;
        }
        for (int i = 0; i < 4; i++) {
            c->cur.scissor[i] = a[i];
        }
        return;
    case SGI_GL_OP_ALPHA_FUNC: {
        float ref = sgi_gl_float(a[1]);

        if (a[0] - SGI_GL_NEVER_ENUM > SGI_GL_ALWAYS) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: alpha function 0x%x "
                          "ignored\n", a[0]);
            return;
        }
        c->cur.alpha_func = a[0] - SGI_GL_NEVER_ENUM;
        /* Clamped to [0, 1] (4.1.4); not a number clamps to 0. */
        c->cur.alpha_ref = ref >= 1.0f ? 1.0f : ref > 0.0f ? ref : 0.0f;
        return;
    }
    case SGI_GL_OP_STENCIL_FUNC:
        if (a[0] - SGI_GL_NEVER_ENUM > SGI_GL_ALWAYS) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: stencil function 0x%x "
                          "ignored\n", a[0]);
            return;
        }
        c->cur.stencil_func = a[0] - SGI_GL_NEVER_ENUM;
        c->cur.stencil_ref = a[1];
        c->cur.stencil_value_mask = a[2];
        return;
    case SGI_GL_OP_STENCIL_OP:
        for (int i = 0; i < 3; i++) {
            if (!sgi_gl_stencil_op_valid(a[i])) {
                qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: glStencilOp(0x%x, "
                              "0x%x, 0x%x) ignored\n", a[0], a[1], a[2]);
                return;
            }
        }
        memcpy(c->cur.stencil_op, a, sizeof(c->cur.stencil_op));
        return;
    case SGI_GL_OP_STENCIL_MASK:
        c->cur.stencil_write_mask = a[0];
        return;
    case SGI_GL_OP_CLEAR_STENCIL:
        c->clear_stencil = a[0];
        return;
    case SGI_GL_OP_BLEND_FUNC:
        if (!sgi_gl_blend_factor_valid(a[0], false) ||
            !sgi_gl_blend_factor_valid(a[1], true)) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: glBlendFunc(0x%x, 0x%x) "
                          "ignored\n", a[0], a[1]);
            return;
        }
        c->cur.blend_src = a[0];
        c->cur.blend_dst = a[1];
        return;
    case SGI_GL_OP_BLEND_EQUATION:
        if (!sgi_gl_blend_equation_valid(a[0])) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: glBlendEquation(0x%x) "
                          "ignored\n", a[0]);
            return;
        }
        c->cur.blend_equation = a[0];
        return;
    case SGI_GL_OP_BLEND_COLOR:
        for (int i = 0; i < 4; i++) {
            float v = sgi_gl_float(a[i]);

            c->cur.blend_color[i] = v >= 1.0f ? 1.0f : v > 0.0f ? v : 0.0f;
        }
        return;
    case SGI_GL_OP_LOGIC_OP:
        if (a[0] - SGI_GL_CLEAR > SGI_GL_SET - SGI_GL_CLEAR) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: glLogicOp(0x%x) "
                          "ignored\n", a[0]);
            return;
        }
        c->cur.logic_op_mode = a[0];
        return;
    case SGI_GL_OP_FOG:
        memcpy(pos, &a[1], sizeof(pos));
        sgi_gl_fog(c, a[0], pos);
        return;
    case SGI_GL_OP_POLYGON_OFFSET:
    case SGI_GL_OP_POLYGON_OFFSET_EXT:
        c->cur.offset_factor = sgi_gl_float(a[0]);
        c->cur.offset_units = cmd->op == SGI_GL_OP_POLYGON_OFFSET ?
                              sgi_gl_float(a[1]) : 0.0f;
        c->cur.offset_bias = cmd->op == SGI_GL_OP_POLYGON_OFFSET_EXT ?
                             sgi_gl_float(a[1]) : 0.0f;
        return;
    case SGI_GL_OP_CLIP_PLANE:
        memcpy(pos, &a[1], sizeof(pos));
        sgi_gl_clip_plane(c, a[0], pos);
        return;
    case SGI_GL_OP_POINT_SIZE:
    case SGI_GL_OP_LINE_WIDTH:
    case SGI_GL_OP_LINE_STIPPLE:
    case SGI_GL_OP_POLYGON_MODE:
        sgi_gl_raster_state(c, cmd->op, a);
        return;
    case SGI_GL_OP_COLOR_MASK:
        c->cur.colour_mask = 0;
        for (int i = 0; i < 4; i++) {
            c->cur.colour_mask |= (a[i] != 0) << i;
        }
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
        if (c->in_begin) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: nested glBegin ignored\n");
            return;
        }
        if (a[0] > SGI_GL_POLYGON) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: glBegin of mode %u "
                          "ignored\n", a[0]);
            return;
        }
        c->line_phase = 0;
        c->in_begin = true;
        c->begin_mode = a[0];
        c->nheld = 0;
        c->strip_odd = 0;
        c->strip_prev = c->strip_swap = 0;
        return;
    case SGI_GL_OP_SWAP_TMESH:
        if (!c->in_begin || c->begin_mode != SGI_GL_TRIANGLE_STRIP) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: a mesh swap outside an "
                          "open triangle strip ignored\n");
            return;
        }
        if (!c->strip_prev) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: a mesh swap before the "
                          "strip's first triangle ignored\n");
            return;
        }
        /* Two swaps before a vertex undo each other. */
        c->strip_swap ^= 1;
        return;
    case SGI_GL_OP_END:
        if (!c->in_begin && c->polygon.enabled) {
            return;
        }
        if (c->polygon.enabled && c->begin_mode == SGI_GL_POLYGON) {
            SGIGLDraw d = {
                .context = c->hw_id, .mode = SGI_GL_POLYGON,
                .count = c->polygon.count, .vertices = c->polygon.vertices,
                .state = &c->cur,
                .back_colors = sgi_gl_lit(c) && c->light_model_two_side,
                .point_size = c->point_size, .line_width = c->line_width,
                .polygon_modes = c->polygon_mode, .fill_by_triangle = true,
                .polygon_stipple = c->polygon_stipple,
                .polygon_stipple_enabled =
                    !!(c->raster_enables & SGI_GL_RASTER_POLYGON_STIPPLE),
                .line_stipple_enabled =
                    !!(c->raster_enables & SGI_GL_RASTER_LINE_STIPPLE),
                .line_stipple_repeat = c->line_stipple_repeat,
                .line_stipple_pattern = c->line_stipple_pattern,
            };

            if (!c->polygon.failed && d.count >= 3) {
                sgi_gl_draw_result(c, sink, &d);
            }
            sgi_gl_polygon_release(&c->polygon);
        }
        /*
         * A line loop closes from its last vertex to its first, in the
         * first vertex's colour when flat (2.13.7).
         */
        if (c->in_begin && c->begin_mode == SGI_GL_LINE_LOOP &&
            c->nheld == 2) {
            sgi_gl_submit(c, sink, SGI_GL_LINES, 2, c->held[1], c->held[0],
                          NULL, NULL);
        }
        c->line_phase = 0;
        c->in_begin = false;
        c->begin_mode = 0;
        c->nheld = 0;
        c->strip_odd = 0;
        c->strip_prev = c->strip_swap = 0;
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

bool sgi_gl_get_clip_plane(const SGIGLContext *c, uint32_t plane,
                           uint32_t out[4])
{
    unsigned n;

    return plane < SGI_GL_CLIP_PLANES &&
           sgi_gl_get_floats(c->cur.clip_plane[plane], 4, out, &n);
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
    if (c->in_begin && (pname == SGI_GL_LINE_STIPPLE ||
                        pname == SGI_GL_LINE_STIPPLE_REPEAT ||
                        pname == SGI_GL_LINE_STIPPLE_PATTERN)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-gl: line stipple query inside glBegin: "
                      "invalid operation\n");
        return false;
    }
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
        return sgi_gl_get_floats(c->cur_texcoord, 4, out, n);
    case SGI_GL_CURRENT_RASTER_TEXTURE_COORDS:
        return sgi_gl_get_floats(c->raster_texcoord, 4, out, n);
    case SGI_GL_TEXTURE_2D:
        out[0] = c->cur.texture_2d;
        *n = 1;
        return true;
    case SGI_GL_TEXTURE_ENV_MODE:
        out[0] = c->cur.texture_env_mode;
        *n = 1;
        return true;
    case SGI_GL_TEXTURE_ENV_COLOR:
        return sgi_gl_get_floats(c->cur.texture_env_color, 4, out, n);
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
        out[0] = c->cur.dither;
        *n = 1;
        return true;
    case SGI_GL_CULL_FACE:
        out[0] = c->cur.cull_face;
        *n = 1;
        return true;
    case SGI_GL_CULL_FACE_MODE:
        out[0] = c->cur.cull_face_mode;
        *n = 1;
        return true;
    case SGI_GL_FRONT_FACE:
        out[0] = c->cur.front_face;
        *n = 1;
        return true;
    case SGI_GL_SCISSOR_TEST:
        out[0] = c->cur.scissor_test;
        *n = 1;
        return true;
    case SGI_GL_SCISSOR_BOX:
        memcpy(out, c->cur.scissor, sizeof(c->cur.scissor));
        *n = 4;
        return true;
    case SGI_GL_ALPHA_TEST:
        out[0] = c->cur.alpha_test;
        *n = 1;
        return true;
    case SGI_GL_ALPHA_TEST_FUNC:
        out[0] = SGI_GL_NEVER_ENUM + c->cur.alpha_func;
        *n = 1;
        return true;
    case SGI_GL_ALPHA_TEST_REF:
        return sgi_gl_get_floats(&c->cur.alpha_ref, 1, out, n);
    case SGI_GL_STENCIL_TEST:
        out[0] = c->cur.stencil_test;
        *n = 1;
        return true;
    case SGI_GL_STENCIL_FUNC:
        out[0] = SGI_GL_NEVER_ENUM + c->cur.stencil_func;
        *n = 1;
        return true;
    case SGI_GL_STENCIL_REF:
        out[0] = c->cur.stencil_ref;
        *n = 1;
        return true;
    case SGI_GL_STENCIL_VALUE_MASK:
        out[0] = c->cur.stencil_value_mask;
        *n = 1;
        return true;
    case SGI_GL_STENCIL_WRITEMASK:
        out[0] = c->cur.stencil_write_mask;
        *n = 1;
        return true;
    case SGI_GL_STENCIL_FAIL:
    case SGI_GL_STENCIL_PASS_DEPTH_FAIL:
    case SGI_GL_STENCIL_PASS_DEPTH_PASS:
        out[0] = c->cur.stencil_op[pname - SGI_GL_STENCIL_FAIL];
        *n = 1;
        return true;
    case SGI_GL_STENCIL_CLEAR_VALUE:
        out[0] = c->clear_stencil;
        *n = 1;
        return true;
    case SGI_GL_BLEND:
        out[0] = c->cur.blend;
        *n = 1;
        return true;
    case SGI_GL_BLEND_SRC:
    case SGI_GL_BLEND_DST:
        out[0] = pname == SGI_GL_BLEND_SRC ? c->cur.blend_src :
                                             c->cur.blend_dst;
        *n = 1;
        return true;
    case SGI_GL_BLEND_EQUATION:
        out[0] = c->cur.blend_equation;
        *n = 1;
        return true;
    case SGI_GL_BLEND_COLOR:
        return sgi_gl_get_floats(c->cur.blend_color, 4, out, n);
    case SGI_GL_INDEX_LOGIC_OP:
    case SGI_GL_COLOR_LOGIC_OP:
        out[0] = pname == SGI_GL_INDEX_LOGIC_OP ? c->cur.index_logic_op :
                                                  c->cur.color_logic_op;
        *n = 1;
        return true;
    case SGI_GL_LOGIC_OP_MODE:
        out[0] = c->cur.logic_op_mode;
        *n = 1;
        return true;
    case SGI_GL_COLOR_WRITEMASK:
        for (int i = 0; i < 4; i++) {
            out[i] = extract32(c->cur.colour_mask, i, 1);
        }
        *n = 4;
        return true;
    case SGI_GL_FOG:
        out[0] = c->cur.fog;
        *n = 1;
        return true;
    case SGI_GL_FOG_MODE:
        out[0] = c->cur.fog_mode;
        *n = 1;
        return true;
    case SGI_GL_FOG_DENSITY:
        return sgi_gl_get_floats(&c->cur.fog_density, 1, out, n);
    case SGI_GL_FOG_START:
        return sgi_gl_get_floats(&c->cur.fog_start, 1, out, n);
    case SGI_GL_FOG_END:
        return sgi_gl_get_floats(&c->cur.fog_end, 1, out, n);
    case SGI_GL_FOG_COLOR:
        return sgi_gl_get_floats(c->cur.fog_color, 4, out, n);
    case SGI_GL_FOG_INDEX:
        return sgi_gl_get_floats(&c->cur.fog_index, 1, out, n);
    case SGI_GL_POLYGON_OFFSET_FILL:
    case SGI_GL_POLYGON_OFFSET_LINE:
    case SGI_GL_POLYGON_OFFSET_POINT:
        out[0] = extract32(c->cur.polygon_offset,
                           pname == SGI_GL_POLYGON_OFFSET_FILL ? 0 :
                           pname == SGI_GL_POLYGON_OFFSET_LINE ? 1 : 2, 1);
        *n = 1;
        return true;
    case SGI_GL_POLYGON_OFFSET_FACTOR:
        return sgi_gl_get_floats(&c->cur.offset_factor, 1, out, n);
    case SGI_GL_CLIP_PLANE0 ... SGI_GL_CLIP_PLANE0 + SGI_GL_CLIP_PLANES - 1:
        out[0] = extract32(c->cur.clip_planes, pname - SGI_GL_CLIP_PLANE0, 1);
        *n = 1;
        return true;
    case SGI_GL_POINT_SMOOTH:
    case SGI_GL_LINE_SMOOTH:
    case SGI_GL_LINE_STIPPLE:
    case SGI_GL_POLYGON_SMOOTH:
    case SGI_GL_POLYGON_STIPPLE:
        if (pname == SGI_GL_POLYGON_STIPPLE && c->in_begin) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-gl: polygon stipple query inside glBegin: "
                          "invalid operation\n");
            return false;
        }
        out[0] = !!(c->raster_enables & sgi_gl_raster_enable(pname));
        *n = 1;
        return true;
    case SGI_GL_POINT_SIZE:
        return sgi_gl_get_floats(&c->point_size, 1, out, n);
    case SGI_GL_LINE_WIDTH:
        return sgi_gl_get_floats(&c->line_width, 1, out, n);
    case SGI_GL_LINE_STIPPLE_REPEAT:
        out[0] = c->line_stipple_repeat;
        *n = 1;
        return true;
    case SGI_GL_LINE_STIPPLE_PATTERN:
        out[0] = c->line_stipple_pattern;
        *n = 1;
        return true;
    case SGI_GL_POLYGON_MODE:
        memcpy(out, c->polygon_mode, sizeof(c->polygon_mode));
        *n = 2;
        return true;
    case SGI_GL_POLYGON_OFFSET_UNITS:
        return sgi_gl_get_floats(&c->cur.offset_units, 1, out, n);
    case SGI_GL_POLYGON_OFFSET_BIAS:
        return sgi_gl_get_floats(&c->cur.offset_bias, 1, out, n);
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
    case SGI_GL_LIGHTING:
        out[0] = c->lighting;
        *n = 1;
        return true;
    case SGI_GL_NORMALIZE:
        out[0] = c->normalize;
        *n = 1;
        return true;
    case SGI_GL_RESCALE_NORMAL:
        out[0] = c->rescale_normal;
        *n = 1;
        return true;
    case SGI_GL_LIGHT0 ... SGI_GL_LIGHT0 + SGI_GL_LIGHTS - 1:
        out[0] = extract32(c->lights_enabled, pname - SGI_GL_LIGHT0, 1);
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

/*
 * The most vertices each primitive mode leaves held between pieces, as
 * sgi_gl_vertex() keeps them: a line strip keeps its last vertex, a line
 * loop its first and last, a quad strip three until its first quad.
 */
static unsigned int sgi_gl_held_max(uint32_t mode)
{
    switch (mode) {
    case SGI_GL_POINTS:
        return 0;
    case SGI_GL_LINES:
    case SGI_GL_LINE_STRIP:
        return 1;
    case SGI_GL_QUADS:
    case SGI_GL_QUAD_STRIP:
        return 3;
    default:
        return 2;
    }
}

/*
 * How many vertices a primitive of this mode holds after count vertices
 * since glBegin, or -1 when k cannot be that number. Only count mod 2,
 * 3 and 4 and k itself decide it, never count's absolute size, so a
 * count that is right only modulo 12 is enough: once a mode has all it
 * will ever hold, k alone says so.
 */
static bool sgi_gl_resume_held_ok(uint32_t mode, unsigned int k,
                                  uint32_t count)
{
    switch (mode) {
    case SGI_GL_POINTS:
        return k == 0;
    case SGI_GL_LINES:
        return k == count % 2;
    case SGI_GL_TRIANGLES:
        return k == count % 3;
    case SGI_GL_QUADS:
        return k == count % 4;
    case SGI_GL_QUAD_STRIP:
        return k >= 2 ? k == 2 + count % 2 : k == count;
    case SGI_GL_LINE_STRIP:
        return k <= 1 && (k == 1 || count == 0);
    default:
        /* Strips of triangles, fans, loops, polygons: first and last. */
        return k <= 2 && (k == 2 || k == count);
    }
}

bool sgi_gl_resume_primitive(SGIGLContext *c, uint32_t mode,
                             const float (*v)[SGI_GL_VERTEX_FLOATS],
                             unsigned int k, uint32_t count)
{
    if (c->in_begin) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-gl: resume inside glBegin ignored\n");
        return false;
    }
    if (mode > SGI_GL_POLYGON || k > SGI_GL_HELD_VERTICES ||
        (k && !v) || !sgi_gl_resume_held_ok(mode, k, count) ||
        (c->polygon.enabled && mode == SGI_GL_POLYGON)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: resume of mode %u with "
                      "%u held of %u sent refused\n", mode, k, count);
        return false;
    }
    /* line_phase is deliberately left alone, unlike glBegin. */
    c->in_begin = true;
    c->begin_mode = mode;
    c->nheld = k;
    for (unsigned int i = 0; i < k; i++) {
        sgi_gl_hold(c, i, v[i]);
    }
    /* The strip's next triangle faces as its count's parity says. */
    c->strip_odd = mode == SGI_GL_TRIANGLE_STRIP && k == 2 ? count & 1 : 0;
    /* The oldest retained vertex of a resumed strip is not sent. */
    c->strip_prev = c->strip_swap = 0;
    return true;
}

void sgi_gl_discard_primitive(SGIGLContext *c)
{
    if (c->polygon.enabled && c->begin_mode == SGI_GL_POLYGON) {
        sgi_gl_polygon_release(&c->polygon);
    }
    c->in_begin = false;
    c->begin_mode = 0;
    c->nheld = 0;
    c->strip_odd = c->strip_prev = c->strip_swap = 0;
}

bool sgi_gl_context_valid(const SGIGLContext *c, Error **errp)
{
    const SGIGLDrawState *st = &c->cur;
    const struct {
        const char *name;
        uint32_t value;
    } flags[] = {
        { "depth_test", st->depth_test },
        { "depth_mask", st->depth_mask },
        { "cull_face", st->cull_face },
        { "scissor_test", st->scissor_test },
        { "alpha_test", st->alpha_test },
        { "stencil_test", st->stencil_test },
        { "blend", st->blend },
        { "index_logic_op", st->index_logic_op },
        { "color_logic_op", st->color_logic_op },
        { "fog", st->fog },
        { "texture_2d", st->texture_2d },
        { "drawable.bound", st->drawable.bound },
        { "drawable.clip.empty", st->drawable.clip.empty },
        { "window_matrix_given", st->window_matrix_given },
        { "window_rows_down", st->window_rows_down },
        { "dither", st->dither },
        { "light_model_local_viewer", c->light_model_local_viewer },
        { "light_model_two_side", c->light_model_two_side },
        { "color_material", c->color_material },
        { "lighting", c->lighting },
        { "normalize", c->normalize },
        { "rescale_normal", c->rescale_normal },
        { "normal_matrix_given", c->normal_matrix_given },
        { "normalize_always", c->normalize_always },
        { "cur_edge", c->cur_edge },
        { "raster_valid", c->raster_valid },
        { "strip_odd", c->strip_odd },
        { "strip_prev", c->strip_prev },
        { "strip_swap", c->strip_swap },
        { "in_begin", c->in_begin },
    };
    const struct {
        const char *name;
        uint32_t depth;
    } stacks[] = {
        { "mv_depth", c->mv_depth },
        { "pr_depth", c->pr_depth },
        { "tx_depth", c->tx_depth },
    };

    for (size_t i = 0; i < ARRAY_SIZE(flags); i++) {
        if (flags[i].value > 1) {
            error_setg(errp, "sgi-gl: %s is %u, not 0 or 1", flags[i].name,
                       flags[i].value);
            return false;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(stacks); i++) {
        if (stacks[i].depth >= SGI_GL_STACK_DEPTH) {
            error_setg(errp, "sgi-gl: %s %u is past the stack's %u",
                       stacks[i].name, stacks[i].depth, SGI_GL_STACK_DEPTH);
            return false;
        }
    }
    if (st->depth_func > SGI_GL_ALWAYS) {
        error_setg(errp, "sgi-gl: depth function %u", st->depth_func);
        return false;
    }
    for (int i = 0; i < 2; i++) {
        if (!(st->depth_range[i] >= 0.0f && st->depth_range[i] <= 1.0f)) {
            error_setg(errp, "sgi-gl: depth range %g is outside [0, 1]",
                       st->depth_range[i]);
            return false;
        }
    }
    if (st->index_bits > 32) {
        error_setg(errp, "sgi-gl: %u bits a colour index", st->index_bits);
        return false;
    }
    if (!sgi_gl_faces(st->cull_face_mode)) {
        error_setg(errp, "sgi-gl: cull face 0x%x", st->cull_face_mode);
        return false;
    }
    if (st->front_face != SGI_GL_CW && st->front_face != SGI_GL_CCW) {
        error_setg(errp, "sgi-gl: front face 0x%x", st->front_face);
        return false;
    }
    if (st->scissor[2] < 0 || st->scissor[3] < 0) {
        error_setg(errp, "sgi-gl: scissor box of %d by %d", st->scissor[2],
                   st->scissor[3]);
        return false;
    }
    if (st->alpha_func > SGI_GL_ALWAYS) {
        error_setg(errp, "sgi-gl: alpha function %u", st->alpha_func);
        return false;
    }
    if (!(st->alpha_ref >= 0.0f && st->alpha_ref <= 1.0f)) {
        error_setg(errp, "sgi-gl: alpha reference %g is outside [0, 1]",
                   st->alpha_ref);
        return false;
    }
    if (st->stencil_func > SGI_GL_ALWAYS) {
        error_setg(errp, "sgi-gl: stencil function %u", st->stencil_func);
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (!sgi_gl_stencil_op_valid(st->stencil_op[i])) {
            error_setg(errp, "sgi-gl: stencil operation 0x%x",
                       st->stencil_op[i]);
            return false;
        }
    }
    if (!sgi_gl_blend_factor_valid(st->blend_src, false) ||
        !sgi_gl_blend_factor_valid(st->blend_dst, true)) {
        error_setg(errp, "sgi-gl: blend factors 0x%x and 0x%x", st->blend_src,
                   st->blend_dst);
        return false;
    }
    if (!sgi_gl_blend_equation_valid(st->blend_equation)) {
        error_setg(errp, "sgi-gl: blend equation 0x%x", st->blend_equation);
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (!(st->blend_color[i] >= 0.0f && st->blend_color[i] <= 1.0f)) {
            error_setg(errp, "sgi-gl: blend colour %g is outside [0, 1]",
                       st->blend_color[i]);
            return false;
        }
    }
    if (st->logic_op_mode - SGI_GL_CLEAR > SGI_GL_SET - SGI_GL_CLEAR) {
        error_setg(errp, "sgi-gl: logical operation 0x%x", st->logic_op_mode);
        return false;
    }
    if (st->colour_mask > SGI_GL_COLOUR_MASK_ALL) {
        error_setg(errp, "sgi-gl: colour mask 0x%x", st->colour_mask);
        return false;
    }
    if (st->texture_env_mode != SGI_GL_REPLACE &&
        st->texture_env_mode != SGI_GL_MODULATE &&
        st->texture_env_mode != SGI_GL_DECAL &&
        st->texture_env_mode != SGI_GL_BLEND) {
        error_setg(errp, "sgi-gl: texture environment mode 0x%x",
                   st->texture_env_mode);
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (!(st->texture_env_color[i] >= 0 && st->texture_env_color[i] <= 1)) {
            error_setg(errp, "sgi-gl: texture environment colour %g is "
                       "outside [0, 1]", st->texture_env_color[i]);
            return false;
        }
    }
    if (st->fog_mode != SGI_GL_LINEAR && st->fog_mode != SGI_GL_EXP &&
        st->fog_mode != SGI_GL_EXP2) {
        error_setg(errp, "sgi-gl: fog mode 0x%x", st->fog_mode);
        return false;
    }
    if (!(st->fog_density >= 0.0f)) {
        error_setg(errp, "sgi-gl: fog density %g", st->fog_density);
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (!(st->fog_color[i] >= 0.0f && st->fog_color[i] <= 1.0f)) {
            error_setg(errp, "sgi-gl: fog colour %g is outside [0, 1]",
                       st->fog_color[i]);
            return false;
        }
    }
    if (st->polygon_offset & ~(SGI_GL_OFFSET_FILL | SGI_GL_OFFSET_LINE |
                               SGI_GL_OFFSET_POINT)) {
        error_setg(errp, "sgi-gl: polygon offset enables 0x%x",
                   st->polygon_offset);
        return false;
    }
    if (st->clip_planes >> SGI_GL_CLIP_PLANES) {
        error_setg(errp, "sgi-gl: clip plane enables 0x%x", st->clip_planes);
        return false;
    }
    if (st->drawable.clip.n > SGI_CLIP_MAX) {
        error_setg(errp, "sgi-gl: %u clip rectangles", st->drawable.clip.n);
        return false;
    }
    if (c->lights_enabled >> SGI_GL_LIGHTS) {
        error_setg(errp, "sgi-gl: light enables 0x%x", c->lights_enabled);
        return false;
    }
    if (!sgi_gl_faces(c->color_material_face)) {
        error_setg(errp, "sgi-gl: colour material face 0x%x",
                   c->color_material_face);
        return false;
    }
    switch (c->color_material_mode) {
    case SGI_GL_AMBIENT:
    case SGI_GL_DIFFUSE:
    case SGI_GL_SPECULAR:
    case SGI_GL_EMISSION:
    case SGI_GL_AMBIENT_AND_DIFFUSE:
        break;
    default:
        error_setg(errp, "sgi-gl: colour material parameter 0x%x",
                   c->color_material_mode);
        return false;
    }
    if (!isfinite(c->point_size) || !isfinite(c->line_width) ||
        !(c->point_size > 0.0f) || !(c->line_width > 0.0f)) {
        error_setg(errp, "sgi-gl: point size %g or line width %g",
                   c->point_size, c->line_width);
        return false;
    }
    if (c->line_stipple_repeat - 1 > 255 || c->line_stipple_pattern > 0xffff) {
        error_setg(errp, "sgi-gl: line stipple repeat %u, pattern 0x%x",
                   c->line_stipple_repeat, c->line_stipple_pattern);
        return false;
    }
    for (int i = 0; i < 2; i++) {
        if (c->polygon_mode[i] - SGI_GL_POINT > SGI_GL_FILL - SGI_GL_POINT) {
            error_setg(errp, "sgi-gl: polygon mode 0x%x", c->polygon_mode[i]);
            return false;
        }
    }
    if (c->raster_enables & ~(SGI_GL_RASTER_POINT_SMOOTH |
                              SGI_GL_RASTER_LINE_SMOOTH |
                              SGI_GL_RASTER_LINE_STIPPLE |
                              SGI_GL_RASTER_POLYGON_SMOOTH |
                              SGI_GL_RASTER_POLYGON_STIPPLE)) {
        error_setg(errp, "sgi-gl: rasterization enables 0x%x",
                   c->raster_enables);
        return false;
    }
    if (c->polygon.enabled > 1 || c->polygon.failed > 1 ||
        c->polygon.count > c->polygon.capacity ||
        (c->polygon.count && !c->polygon.vertices) ||
        (c->polygon.failed && c->polygon.count) ||
        (c->polygon.error && c->polygon.error != SGI_GL_OUT_OF_MEMORY) ||
        ((c->polygon.count || c->polygon.failed) &&
         (!c->polygon.enabled || !c->in_begin ||
          c->begin_mode != SGI_GL_POLYGON)) ||
        (c->polygon.enabled && c->in_begin &&
         c->begin_mode == SGI_GL_POLYGON && c->nheld)) {
        error_setg(errp, "sgi-gl: invalid owned polygon assembly");
        return false;
    }
    if (c->begin_mode > SGI_GL_POLYGON) {
        error_setg(errp, "sgi-gl: primitive mode %u", c->begin_mode);
        return false;
    }
    if (c->nheld > (c->in_begin ? sgi_gl_held_max(c->begin_mode) : 0)) {
        error_setg(errp, "sgi-gl: %u vertices held in mode %u%s", c->nheld,
                   c->begin_mode, c->in_begin ? "" : " outside glBegin");
        return false;
    }
    return true;
}

static bool sgi_gl_context_post_load(void *opaque, int version_id,
                                     Error **errp)
{
    SGIGLContext *c = opaque;
    bool valid;

    if (c->polygon.enabled && c->in_begin &&
        c->begin_mode == SGI_GL_POLYGON && !c->polygon.loaded) {
        error_setg(errp, "sgi-gl: old open polygon has no deferred boundary");
        return false;
    }
    valid = sgi_gl_context_valid(c, errp);
    if (valid && c->draw_error_loaded &&
        ((!c->standalone_stipple && !c->polygon.enabled) || c->polygon.loaded ||
         c->polygon.error != SGI_GL_OUT_OF_MEMORY)) {
        error_setg(errp, "sgi-gl: incompatible standalone draw error");
        valid = false;
    }
    c->draw_error_loaded = false;
    if (valid && (c->line_phase_loaded ?
                  !c->standalone_stipple || !sgi_gl_open_line(c) ||
                  c->line_phase >= 16 * c->line_stipple_repeat :
                  c->standalone_stipple && sgi_gl_open_line(c))) {
        error_setg(errp, "sgi-gl: incompatible standalone line phase");
        valid = false;
    }
    c->line_phase_loaded = false;
    if (!valid) {
        sgi_gl_polygon_release(&c->polygon);
    }
    c->polygon.loaded = 0;
    return valid;
}

/*
 * A float travels as the IEEE single bits it holds, in the stream's byte
 * order, so a NaN's payload and a zero's sign survive.
 */
#define VMSTATE_SGI_GL_FLOAT(_f, _s)                                     \
    VMSTATE_SINGLE(_f, _s, 0, vmstate_info_uint32, float)
#define VMSTATE_SGI_GL_FLOATS(_f, _s, _n)                                \
    VMSTATE_ARRAY(_f, _s, _n, 0, vmstate_info_uint32, float)
#define VMSTATE_SGI_GL_FLOATS_2D(_f, _s, _n1, _n2)                       \
    VMSTATE_2DARRAY(_f, _s, _n1, _n2, 0, vmstate_info_uint32, float)

/*
 * What sgi_gl_init() leaves, which a subsection's .needed compares its
 * fields against: a subsection is sent whenever one of its fields holds
 * anything else.
 */
static const SGIGLContext *sgi_gl_initial(void)
{
    static SGIGLContext initial;
    static gsize done;

    if (g_once_init_enter(&done)) {
        sgi_gl_init(&initial, 0);
        g_once_init_leave(&done, 1);
    }
    return &initial;
}

/* Whether field _f of _s differs, bit for bit, from _init's. */
#define SGI_GL_MOVED(_s, _init, _f) \
    (memcmp(&(_s)->_f, &(_init)->_f, sizeof((_s)->_f)) != 0)

static const VMStateDescription vmstate_sgi_clip_rect = {
    .name = "sgi-clip-rect",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(x0, SGIClipRect),
        VMSTATE_INT32(y0, SGIClipRect),
        VMSTATE_INT32(x1, SGIClipRect),
        VMSTATE_INT32(y1, SGIClipRect),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_clip_empty_needed(void *opaque)
{
    SGIGLDrawable *d = opaque;

    return d->clip.empty != 0;
}

/* Sent only while the clip keeps no pixel; sgi_gl_init() leaves it 0. */
static const VMStateDescription vmstate_sgi_gl_clip_empty = {
    .name = "sgi-gl-drawable/clip-empty",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_clip_empty_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(clip.empty, SGIGLDrawable),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_drawable_extra_needed(void *opaque)
{
    SGIGLDrawable *d = opaque;

    return d->extra[0] != 0 || d->extra[1] != 0;
}

/* Sent only while a deep visual names a third or fourth target. */
static const VMStateDescription vmstate_sgi_gl_drawable_extra = {
    .name = "sgi-gl-drawable/extra",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_drawable_extra_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(extra, SGIGLDrawable, 2),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_gl_drawable = {
    .name = "sgi-gl-drawable",
    .version_id = 1,
    .minimum_version_id = 1,
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sgi_gl_clip_empty,
        &vmstate_sgi_gl_drawable_extra,
        NULL
    },
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(bound, SGIGLDrawable),
        VMSTATE_INT32_ARRAY(origin, SGIGLDrawable, 2),
        VMSTATE_UINT32(clip.n, SGIGLDrawable),
        VMSTATE_STRUCT_ARRAY(clip.r, SGIGLDrawable, SGI_CLIP_MAX, 0,
                             vmstate_sgi_clip_rect, SGIClipRect),
        VMSTATE_UINT32(front, SGIGLDrawable),
        VMSTATE_UINT32(back, SGIGLDrawable),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_scissor_needed(void *opaque)
{
    const SGIGLDrawState *st = opaque, *init = &sgi_gl_initial()->cur;

    return SGI_GL_MOVED(st, init, scissor_test) ||
           SGI_GL_MOVED(st, init, scissor);
}

static const VMStateDescription vmstate_sgi_gl_scissor = {
    .name = "sgi-gl-draw-state/scissor",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_scissor_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(scissor_test, SGIGLDrawState),
        VMSTATE_INT32_ARRAY(scissor, SGIGLDrawState, 4),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_alpha_test_needed(void *opaque)
{
    const SGIGLDrawState *st = opaque, *init = &sgi_gl_initial()->cur;

    return SGI_GL_MOVED(st, init, alpha_test) ||
           SGI_GL_MOVED(st, init, alpha_func) ||
           SGI_GL_MOVED(st, init, alpha_ref);
}

static const VMStateDescription vmstate_sgi_gl_alpha_test = {
    .name = "sgi-gl-draw-state/alpha-test",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_alpha_test_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(alpha_test, SGIGLDrawState),
        VMSTATE_UINT32(alpha_func, SGIGLDrawState),
        VMSTATE_SGI_GL_FLOAT(alpha_ref, SGIGLDrawState),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_stencil_needed(void *opaque)
{
    const SGIGLDrawState *st = opaque, *init = &sgi_gl_initial()->cur;

    return SGI_GL_MOVED(st, init, stencil_test) ||
           SGI_GL_MOVED(st, init, stencil_func) ||
           SGI_GL_MOVED(st, init, stencil_ref) ||
           SGI_GL_MOVED(st, init, stencil_value_mask) ||
           SGI_GL_MOVED(st, init, stencil_write_mask) ||
           SGI_GL_MOVED(st, init, stencil_op);
}

static const VMStateDescription vmstate_sgi_gl_stencil = {
    .name = "sgi-gl-draw-state/stencil",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_stencil_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(stencil_test, SGIGLDrawState),
        VMSTATE_UINT32(stencil_func, SGIGLDrawState),
        VMSTATE_INT32(stencil_ref, SGIGLDrawState),
        VMSTATE_UINT32(stencil_value_mask, SGIGLDrawState),
        VMSTATE_UINT32(stencil_write_mask, SGIGLDrawState),
        VMSTATE_UINT32_ARRAY(stencil_op, SGIGLDrawState, 3),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_blend_needed(void *opaque)
{
    const SGIGLDrawState *st = opaque, *init = &sgi_gl_initial()->cur;

    return SGI_GL_MOVED(st, init, blend) ||
           SGI_GL_MOVED(st, init, blend_src) ||
           SGI_GL_MOVED(st, init, blend_dst) ||
           SGI_GL_MOVED(st, init, blend_equation) ||
           SGI_GL_MOVED(st, init, blend_color);
}

static const VMStateDescription vmstate_sgi_gl_blend = {
    .name = "sgi-gl-draw-state/blend",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_blend_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(blend, SGIGLDrawState),
        VMSTATE_UINT32(blend_src, SGIGLDrawState),
        VMSTATE_UINT32(blend_dst, SGIGLDrawState),
        VMSTATE_UINT32(blend_equation, SGIGLDrawState),
        VMSTATE_SGI_GL_FLOATS(blend_color, SGIGLDrawState, 4),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_logic_op_needed(void *opaque)
{
    const SGIGLDrawState *st = opaque, *init = &sgi_gl_initial()->cur;

    return SGI_GL_MOVED(st, init, index_logic_op) ||
           SGI_GL_MOVED(st, init, color_logic_op) ||
           SGI_GL_MOVED(st, init, logic_op_mode);
}

static const VMStateDescription vmstate_sgi_gl_logic_op = {
    .name = "sgi-gl-draw-state/logic-op",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_logic_op_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(index_logic_op, SGIGLDrawState),
        VMSTATE_UINT32(color_logic_op, SGIGLDrawState),
        VMSTATE_UINT32(logic_op_mode, SGIGLDrawState),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_colour_mask_needed(void *opaque)
{
    const SGIGLDrawState *st = opaque, *init = &sgi_gl_initial()->cur;

    return SGI_GL_MOVED(st, init, colour_mask);
}

static const VMStateDescription vmstate_sgi_gl_colour_mask = {
    .name = "sgi-gl-draw-state/colour-mask",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_colour_mask_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(colour_mask, SGIGLDrawState),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_fog_needed(void *opaque)
{
    const SGIGLDrawState *st = opaque, *init = &sgi_gl_initial()->cur;

    return SGI_GL_MOVED(st, init, fog) ||
           SGI_GL_MOVED(st, init, fog_mode) ||
           SGI_GL_MOVED(st, init, fog_density) ||
           SGI_GL_MOVED(st, init, fog_start) ||
           SGI_GL_MOVED(st, init, fog_end) ||
           SGI_GL_MOVED(st, init, fog_color) ||
           SGI_GL_MOVED(st, init, fog_index);
}

static const VMStateDescription vmstate_sgi_gl_fog = {
    .name = "sgi-gl-draw-state/fog",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_fog_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(fog, SGIGLDrawState),
        VMSTATE_UINT32(fog_mode, SGIGLDrawState),
        VMSTATE_SGI_GL_FLOAT(fog_density, SGIGLDrawState),
        VMSTATE_SGI_GL_FLOAT(fog_start, SGIGLDrawState),
        VMSTATE_SGI_GL_FLOAT(fog_end, SGIGLDrawState),
        VMSTATE_SGI_GL_FLOATS(fog_color, SGIGLDrawState, 4),
        VMSTATE_SGI_GL_FLOAT(fog_index, SGIGLDrawState),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_polygon_offset_needed(void *opaque)
{
    const SGIGLDrawState *st = opaque, *init = &sgi_gl_initial()->cur;

    return SGI_GL_MOVED(st, init, polygon_offset) ||
           SGI_GL_MOVED(st, init, offset_factor) ||
           SGI_GL_MOVED(st, init, offset_units) ||
           SGI_GL_MOVED(st, init, offset_bias);
}

static const VMStateDescription vmstate_sgi_gl_polygon_offset = {
    .name = "sgi-gl-draw-state/polygon-offset",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_polygon_offset_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(polygon_offset, SGIGLDrawState),
        VMSTATE_SGI_GL_FLOAT(offset_factor, SGIGLDrawState),
        VMSTATE_SGI_GL_FLOAT(offset_units, SGIGLDrawState),
        VMSTATE_SGI_GL_FLOAT(offset_bias, SGIGLDrawState),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_clip_planes_needed(void *opaque)
{
    const SGIGLDrawState *st = opaque, *init = &sgi_gl_initial()->cur;

    return SGI_GL_MOVED(st, init, clip_planes) ||
           SGI_GL_MOVED(st, init, clip_plane);
}

static const VMStateDescription vmstate_sgi_gl_clip_planes = {
    .name = "sgi-gl-draw-state/clip-planes",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_clip_planes_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(clip_planes, SGIGLDrawState),
        VMSTATE_SGI_GL_FLOATS_2D(clip_plane, SGIGLDrawState,
                                 SGI_GL_CLIP_PLANES, 4),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_window_matrix_needed(void *opaque)
{
    const SGIGLDrawState *st = opaque, *init = &sgi_gl_initial()->cur;

    return SGI_GL_MOVED(st, init, window_matrix_given) ||
           SGI_GL_MOVED(st, init, window_matrix) ||
           SGI_GL_MOVED(st, init, window_depth_scale) ||
           SGI_GL_MOVED(st, init, window_rows_down);
}

static const VMStateDescription vmstate_sgi_gl_window_matrix = {
    .name = "sgi-gl-draw-state/window-matrix",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_window_matrix_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(window_matrix_given, SGIGLDrawState),
        VMSTATE_SGI_GL_FLOATS(window_matrix, SGIGLDrawState, 16),
        VMSTATE_SGI_GL_FLOAT(window_depth_scale, SGIGLDrawState),
        VMSTATE_UINT32(window_rows_down, SGIGLDrawState),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_texture_env_needed(void *opaque)
{
    const SGIGLDrawState *st = opaque, *init = &sgi_gl_initial()->cur;

    return SGI_GL_MOVED(st, init, texture_2d) ||
           SGI_GL_MOVED(st, init, texture_env_mode) ||
           SGI_GL_MOVED(st, init, texture_env_color);
}

static const VMStateDescription vmstate_sgi_gl_texture_env = {
    .name = "sgi-gl-draw-state/texture-environment",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_texture_env_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(texture_2d, SGIGLDrawState),
        VMSTATE_UINT32(texture_env_mode, SGIGLDrawState),
        VMSTATE_SGI_GL_FLOATS(texture_env_color, SGIGLDrawState, 4),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_gl_draw_state = {
    .name = "sgi-gl-draw-state",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SGI_GL_FLOATS(modelview, SGIGLDrawState, 16),
        VMSTATE_SGI_GL_FLOATS(projection, SGIGLDrawState, 16),
        VMSTATE_INT32_ARRAY(viewport, SGIGLDrawState, 4),
        VMSTATE_UINT32(shade_model, SGIGLDrawState),
        VMSTATE_UINT32(depth_test, SGIGLDrawState),
        VMSTATE_UINT32(depth_func, SGIGLDrawState),
        VMSTATE_UINT32(depth_mask, SGIGLDrawState),
        VMSTATE_SGI_GL_FLOATS(depth_range, SGIGLDrawState, 2),
        VMSTATE_UINT32(draw_buffers, SGIGLDrawState),
        VMSTATE_UINT32(index_bits, SGIGLDrawState),
        VMSTATE_UINT32(index_mask, SGIGLDrawState),
        VMSTATE_UINT32(cull_face, SGIGLDrawState),
        VMSTATE_UINT32(cull_face_mode, SGIGLDrawState),
        VMSTATE_UINT32(front_face, SGIGLDrawState),
        VMSTATE_STRUCT(drawable, SGIGLDrawState, 0, vmstate_sgi_gl_drawable,
                       SGIGLDrawable),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sgi_gl_scissor,
        &vmstate_sgi_gl_alpha_test,
        &vmstate_sgi_gl_stencil,
        &vmstate_sgi_gl_blend,
        &vmstate_sgi_gl_logic_op,
        &vmstate_sgi_gl_colour_mask,
        &vmstate_sgi_gl_fog,
        &vmstate_sgi_gl_texture_env,
        &vmstate_sgi_gl_polygon_offset,
        &vmstate_sgi_gl_clip_planes,
        &vmstate_sgi_gl_window_matrix,
        NULL
    },
};

static const VMStateDescription vmstate_sgi_gl_light = {
    .name = "sgi-gl-light",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SGI_GL_FLOATS(ambient, SGIGLLight, 4),
        VMSTATE_SGI_GL_FLOATS(diffuse, SGIGLLight, 4),
        VMSTATE_SGI_GL_FLOATS(specular, SGIGLLight, 4),
        VMSTATE_SGI_GL_FLOATS(position, SGIGLLight, 4),
        VMSTATE_SGI_GL_FLOATS(spot_direction, SGIGLLight, 3),
        VMSTATE_SGI_GL_FLOAT(spot_exponent, SGIGLLight),
        VMSTATE_SGI_GL_FLOAT(spot_cutoff, SGIGLLight),
        VMSTATE_SGI_GL_FLOATS(attenuation, SGIGLLight, 3),
        VMSTATE_SGI_GL_FLOAT(spot_cos_cutoff, SGIGLLight),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_gl_material = {
    .name = "sgi-gl-material",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SGI_GL_FLOATS(ambient, SGIGLMaterial, 4),
        VMSTATE_SGI_GL_FLOATS(diffuse, SGIGLMaterial, 4),
        VMSTATE_SGI_GL_FLOATS(specular, SGIGLMaterial, 4),
        VMSTATE_SGI_GL_FLOATS(emission, SGIGLMaterial, 4),
        VMSTATE_SGI_GL_FLOAT(shininess, SGIGLMaterial),
        VMSTATE_SGI_GL_FLOATS(color_indexes, SGIGLMaterial, 3),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_clear_stencil_needed(void *opaque)
{
    const SGIGLContext *c = opaque;

    return SGI_GL_MOVED(c, sgi_gl_initial(), clear_stencil);
}

static const VMStateDescription vmstate_sgi_gl_clear_stencil = {
    .name = "sgi-gl-context/clear-stencil",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_clear_stencil_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(clear_stencil, SGIGLContext),
        VMSTATE_END_OF_LIST()
    },
};

static int sgi_gl_polygon_put(QEMUFile *f, void *pv, size_t size,
                               const VMStateField *field, JSONWriter *vmdesc)
{
    const SGIGLPolygonAssembly *p = pv;

    qemu_put_be32(f, p->count);
    qemu_put_be32(f, p->failed);
    qemu_put_be32(f, p->error);
    for (uint32_t i = 0; i < p->count; i++) {
        for (unsigned k = 0; k < SGI_GL_VERTEX_FLOATS; k++) {
            qemu_put_be32(f, sgi_gl_bits(
                p->vertices[(size_t)i * SGI_GL_VERTEX_FLOATS + k]));
        }
    }
    return 0;
}

static int sgi_gl_polygon_get(QEMUFile *f, void *pv, size_t size,
                               const VMStateField *field)
{
    SGIGLPolygonAssembly *dst = pv;
    SGIGLPolygonAssembly p = { .enabled = dst->enabled, .loaded = 1 };
    uint32_t count;
    int ret = -EINVAL;

    if (dst->loaded) {
        /* A second marker must not overwrite the first owned payload. */
        sgi_gl_polygon_release(dst);
        return -EINVAL;
    }
    if (!dst->enabled) {
        return -EINVAL;
    }
    count = qemu_get_be32(f);
    p.failed = qemu_get_be32(f);
    p.error = qemu_get_be32(f);
    if (p.failed > 1 || (p.failed && count) ||
        (p.error && p.error != SGI_GL_OUT_OF_MEMORY)) {
        return -EINVAL;
    }
    /* Allocate only for records that arrive, not a claimed huge count. */
    for (uint32_t i = 0; i < count; i++) {
        float record[SGI_GL_VERTEX_FLOATS];

        for (unsigned k = 0; k < SGI_GL_VERTEX_FLOATS; k++) {
            record[k] = sgi_gl_float(qemu_get_be32(f));
        }
        if (qemu_file_get_error(f)) {
            goto fail;
        }
        if (!sgi_gl_polygon_reserve(&p, (size_t)i + 1)) {
            ret = -ENOMEM;
            goto fail;
        }
        memcpy(&p.vertices[(size_t)i * SGI_GL_VERTEX_FLOATS], record,
               sizeof(record));
        p.count++;
    }
    if (qemu_file_get_error(f)) {
        goto fail;
    }
    *dst = p;
    return 0;
fail:
    sgi_gl_polygon_release(&p);
    return ret;
}

static const VMStateInfo vmstate_info_sgi_gl_polygon = {
    .name = "sgi-gl-owned-polygon",
    .get = sgi_gl_polygon_get,
    .put = sgi_gl_polygon_put,
};

static bool sgi_gl_polygon_needed(void *opaque)
{
    const SGIGLContext *c = opaque;

    return c->polygon.enabled && sgi_gl_polygon_owned(c);
}

static const VMStateDescription vmstate_sgi_gl_polygon = {
    .name = "sgi-gl-context/polygon-assembly",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_polygon_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_SINGLE(polygon, SGIGLContext, 0, vmstate_info_sgi_gl_polygon,
                       SGIGLPolygonAssembly),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_draw_error_needed(void *opaque)
{
    const SGIGLContext *c = opaque;

    return !c->polygon.enabled && c->polygon.error;
}

static int sgi_gl_draw_error_pre_load(void *opaque)
{
    SGIGLContext *c = opaque;

    if (c->draw_error_loaded || c->polygon.loaded) {
        sgi_gl_polygon_release(&c->polygon);
        return -EINVAL;
    }
    c->draw_error_loaded = true;
    return 0;
}

static const VMStateDescription vmstate_sgi_gl_draw_error = {
    .name = "sgi-gl-context/draw-error",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_draw_error_needed,
    .pre_load = sgi_gl_draw_error_pre_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(polygon.error, SGIGLContext),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_polygon_stipple_needed(void *opaque)
{
    const SGIGLContext *c = opaque;

    return SGI_GL_MOVED(c, sgi_gl_initial(), polygon_stipple);
}

static const VMStateDescription vmstate_sgi_gl_polygon_stipple = {
    .name = "sgi-gl-context/polygon-stipple",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_polygon_stipple_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(polygon_stipple, SGIGLContext, 32),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_line_phase_needed(void *opaque)
{
    SGIGLContext *c = opaque;

    return c->standalone_stipple && sgi_gl_open_line(c);
}

static int sgi_gl_line_phase_pre_load(void *opaque)
{
    SGIGLContext *c = opaque;

    c->line_phase_loaded = true;
    return 0;
}

static const VMStateDescription vmstate_sgi_gl_line_phase = {
    .name = "sgi-gl-context/line-phase",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_line_phase_needed,
    .pre_load = sgi_gl_line_phase_pre_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(line_phase, SGIGLContext),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_strip_mesh_needed(void *opaque)
{
    const SGIGLContext *c = opaque;

    return c->strip_prev || c->strip_swap;
}

static const VMStateDescription vmstate_sgi_gl_strip_mesh = {
    .name = "sgi-gl-context/strip-mesh",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_strip_mesh_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(strip_prev, SGIGLContext),
        VMSTATE_UINT32(strip_swap, SGIGLContext),
        VMSTATE_END_OF_LIST()
    },
};

/* Old streams omit the optional pattern, including on a reused context. */
static int sgi_gl_context_pre_load(void *opaque)
{
    SGIGLContext *c = opaque;

    memset(c->polygon_stipple, 0xff, sizeof(c->polygon_stipple));
    c->line_phase = 0;
    c->strip_prev = c->strip_swap = 0;
    c->line_phase_loaded = c->draw_error_loaded = false;
    sgi_gl_polygon_release(&c->polygon);
    c->polygon.error = 0;
    return 0;
}

static bool sgi_gl_rasterization_needed(void *opaque)
{
    const SGIGLContext *c = opaque, *init = sgi_gl_initial();

    return SGI_GL_MOVED(c, init, point_size) ||
           SGI_GL_MOVED(c, init, line_width) ||
           SGI_GL_MOVED(c, init, line_stipple_repeat) ||
           SGI_GL_MOVED(c, init, line_stipple_pattern) ||
           SGI_GL_MOVED(c, init, polygon_mode) ||
           SGI_GL_MOVED(c, init, raster_enables);
}

static const VMStateDescription vmstate_sgi_gl_rasterization = {
    .name = "sgi-gl-context/rasterization",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_rasterization_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_SGI_GL_FLOAT(point_size, SGIGLContext),
        VMSTATE_SGI_GL_FLOAT(line_width, SGIGLContext),
        VMSTATE_UINT32(line_stipple_repeat, SGIGLContext),
        VMSTATE_UINT32(line_stipple_pattern, SGIGLContext),
        VMSTATE_UINT32_ARRAY(polygon_mode, SGIGLContext, 2),
        VMSTATE_UINT32(raster_enables, SGIGLContext),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_gl_texture_coords_needed(void *opaque)
{
    const SGIGLContext *c = opaque, *init = sgi_gl_initial();

    return SGI_GL_MOVED(c, init, cur_texcoord) ||
           SGI_GL_MOVED(c, init, raster_texcoord);
}

static const VMStateDescription vmstate_sgi_gl_texture_coords = {
    .name = "sgi-gl-context/texture-coordinates",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_gl_texture_coords_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_SGI_GL_FLOATS(cur_texcoord, SGIGLContext, 4),
        VMSTATE_SGI_GL_FLOATS(raster_texcoord, SGIGLContext, 4),
        VMSTATE_END_OF_LIST()
    },
};

const VMStateDescription vmstate_sgi_gl_context = {
    .name = "sgi-gl-context",
    .version_id = SGI_GL_VMSTATE_VERSION,
    .minimum_version_id = SGI_GL_VMSTATE_VERSION,
    .pre_load = sgi_gl_context_pre_load,
    .post_load_errp = sgi_gl_context_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(hw_id, SGIGLContext),
        VMSTATE_STRUCT(cur, SGIGLContext, 0, vmstate_sgi_gl_draw_state,
                       SGIGLDrawState),
        VMSTATE_SGI_GL_FLOATS_2D(mv_stack, SGIGLContext,
                                 SGI_GL_STACK_DEPTH - 1, 16),
        VMSTATE_SGI_GL_FLOATS_2D(pr_stack, SGIGLContext,
                                 SGI_GL_STACK_DEPTH - 1, 16),
        VMSTATE_UINT32(mv_depth, SGIGLContext),
        VMSTATE_UINT32(pr_depth, SGIGLContext),
        VMSTATE_SGI_GL_FLOATS(texture, SGIGLContext, 16),
        VMSTATE_SGI_GL_FLOATS_2D(tx_stack, SGIGLContext,
                                 SGI_GL_STACK_DEPTH - 1, 16),
        VMSTATE_UINT32(tx_depth, SGIGLContext),
        VMSTATE_UINT32(matrix_mode, SGIGLContext),
        /*
         * The draw state's enable, described here in the context's list,
         * where the base layout has always carried it.
         */
        VMSTATE_UINT32(cur.dither, SGIGLContext),
        VMSTATE_STRUCT_ARRAY(light, SGIGLContext, SGI_GL_LIGHTS, 0,
                             vmstate_sgi_gl_light, SGIGLLight),
        VMSTATE_STRUCT_ARRAY(material, SGIGLContext, 2, 0,
                             vmstate_sgi_gl_material, SGIGLMaterial),
        VMSTATE_SGI_GL_FLOATS(light_model_ambient, SGIGLContext, 4),
        VMSTATE_UINT32(light_model_local_viewer, SGIGLContext),
        VMSTATE_UINT32(light_model_two_side, SGIGLContext),
        VMSTATE_UINT32(color_material, SGIGLContext),
        VMSTATE_UINT32(color_material_face, SGIGLContext),
        VMSTATE_UINT32(color_material_mode, SGIGLContext),
        VMSTATE_UINT32(lighting, SGIGLContext),
        VMSTATE_UINT32(lights_enabled, SGIGLContext),
        VMSTATE_UINT32(normalize, SGIGLContext),
        VMSTATE_UINT32(rescale_normal, SGIGLContext),
        VMSTATE_UINT32(normal_matrix_given, SGIGLContext),
        VMSTATE_SGI_GL_FLOATS(normal_matrix, SGIGLContext, 9),
        VMSTATE_UINT32(normalize_always, SGIGLContext),
        VMSTATE_SGI_GL_FLOATS(cur_color, SGIGLContext, 4),
        VMSTATE_SGI_GL_FLOAT(cur_index, SGIGLContext),
        VMSTATE_SGI_GL_FLOATS(cur_normal, SGIGLContext, 3),
        VMSTATE_UINT32(cur_edge, SGIGLContext),
        VMSTATE_SGI_GL_FLOATS(raster_pos, SGIGLContext, 4),
        VMSTATE_UINT32(raster_valid, SGIGLContext),
        VMSTATE_SGI_GL_FLOAT(raster_distance, SGIGLContext),
        VMSTATE_SGI_GL_FLOATS(raster_color, SGIGLContext, 4),
        VMSTATE_SGI_GL_FLOAT(raster_index, SGIGLContext),
        VMSTATE_SGI_GL_FLOATS(clear_color, SGIGLContext, 4),
        VMSTATE_SGI_GL_FLOAT(clear_index, SGIGLContext),
        VMSTATE_UINT32(clear_depth, SGIGLContext),
        VMSTATE_UINT32(begin_mode, SGIGLContext),
        VMSTATE_UINT32(in_begin, SGIGLContext),
        VMSTATE_SGI_GL_FLOATS_2D(held, SGIGLContext, SGI_GL_HELD_VERTICES,
                                 SGI_GL_VERTEX_FLOATS),
        VMSTATE_UINT32(nheld, SGIGLContext),
        VMSTATE_UINT32(strip_odd, SGIGLContext),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sgi_gl_clear_stencil,
        &vmstate_sgi_gl_rasterization,
        &vmstate_sgi_gl_polygon_stipple,
        &vmstate_sgi_gl_line_phase,
        &vmstate_sgi_gl_strip_mesh,
        &vmstate_sgi_gl_polygon,
        &vmstate_sgi_gl_draw_error,
        &vmstate_sgi_gl_texture_coords,
        NULL
    },
};
