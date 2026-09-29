/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Migration of the shared GL context: every byte of SGIGLContext is
 * described, a populated context survives the trip exactly, an open
 * primitive goes on where it stopped, and a context no command could have
 * left is refused.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "io/channel-buffer.h"
#include "migration/vmstate.h"
#include "../migration/qemu-file.h"
#include "hw/display/sgi/gl-context.h"

#define CONTEXTS 2

/* A family with a table of contexts, sent as the shared subsection. */
typedef struct TestHead {
    uint32_t reg;
    SGIGLContext gl[CONTEXTS];
} TestHead;

DEFINE_VMSTATE_SGI_GL_CONTEXTS(vmstate_test_head_gl, "test-head/gl-contexts",
                               TestHead, gl, CONTEXTS);

static const VMStateDescription vmstate_test_head = {
    .name = "test-head",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(reg, TestHead),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_test_head_gl,
        NULL
    },
};

/* The saved stream, copied out before closing the file frees it. */
static GByteArray *save_head(TestHead *h)
{
    QIOChannelBuffer *bioc = qio_channel_buffer_new(4096);
    QEMUFile *f = qemu_file_new_output(QIO_CHANNEL(bioc));
    GByteArray *stream;

    g_assert_cmpint(vmstate_save_state(f, &vmstate_test_head, h, NULL,
                                       &error_abort), ==, 0);
    g_assert_cmpint(qemu_fflush(f), ==, 0);
    stream = g_byte_array_new();
    g_byte_array_append(stream, bioc->data, bioc->usage);
    /*
     * What follows a device in a real stream: loading peeks one byte past
     * the device for another subsection, and a stream that ends there is
     * an error.
     */
    g_byte_array_append(stream, (const guint8 *)"", 1);
    qemu_fclose(f);
    object_unref(OBJECT(bioc));
    return stream;
}

/* The load's result; *why, when given, takes the refusal's message. */
static int load_head(GByteArray *stream, TestHead *h, char **why)
{
    QIOChannelBuffer *bioc = qio_channel_buffer_new(stream->len);
    QEMUFile *f;
    Error *err = NULL;
    int ret;

    memcpy(bioc->data, stream->data, stream->len);
    bioc->usage = stream->len;
    f = qemu_file_new_input(QIO_CHANNEL(bioc));
    ret = vmstate_load_state(f, &vmstate_test_head, h, 1, &err);
    if (err) {
        g_test_message("load: %s", error_get_pretty(err));
        if (why) {
            *why = g_strdup(error_get_pretty(err));
        }
    }
    error_free(err);
    qemu_fclose(f);
    object_unref(OBJECT(bioc));
    return ret;
}

static void put_u32(SGIGLContext *c, size_t offset, uint32_t v)
{
    memcpy((uint8_t *)c + offset, &v, sizeof(v));
}

static uint32_t float_bits(float f)
{
    uint32_t v;

    memcpy(&v, &f, sizeof(v));
    return v;
}

/*
 * A valid context in which every field holds a value, and every field of
 * the other variant another value: a field the trip loses keeps the
 * destination's, and the comparison catches it. Floats take arbitrary
 * bit patterns, NaNs among them, which must survive as bits.
 */
static void populate(SGIGLContext *c, bool other)
{
    uint8_t *b = (uint8_t *)c;

    for (size_t i = 0; i < sizeof(*c); i++) {
        b[i] = (uint8_t)(i * 131 + 29) ^ (other ? 0xff : 0);
    }

    /* Everything sgi_gl_context_valid() limits, set inside its limits. */
    c->cur.depth_test = !other;
    c->cur.depth_mask = !other;
    c->cur.cull_face = !other;
    c->dither = !other;
    c->light_model_local_viewer = !other;
    c->light_model_two_side = !other;
    c->color_material = !other;
    c->lighting = !other;
    c->normalize = !other;
    c->rescale_normal = !other;
    c->normal_matrix_given = !other;
    c->normalize_always = !other;
    c->cur_edge = !other;
    c->raster_valid = !other;
    c->strip_odd = !other;
    c->mv_depth = other ? 5 : SGI_GL_STACK_DEPTH - 1;
    c->pr_depth = other ? SGI_GL_STACK_DEPTH - 2 : 7;
    c->tx_depth = other ? 0 : 2;
    c->cur.depth_func = other ? SGI_GL_NEVER : SGI_GL_GEQUAL;
    c->cur.depth_range[0] = other ? 1.0f : 0.25f;
    c->cur.depth_range[1] = other ? 0.0f : 0.75f;
    c->cur.index_bits = other ? 32 : 8;
    c->cur.cull_face_mode = other ? SGI_GL_FRONT : SGI_GL_FRONT_AND_BACK;
    c->cur.front_face = other ? SGI_GL_CCW : SGI_GL_CW;
    c->cur.drawable.bound = !other;
    c->cur.drawable.clip.empty = !other;
    c->cur.drawable.clip.n = other ? 3 : SGI_CLIP_MAX;
    c->lights_enabled = other ? 0x5a : 0xa5;
    c->color_material_face = other ? SGI_GL_FRONT : SGI_GL_BACK;
    c->color_material_mode = other ? SGI_GL_EMISSION : SGI_GL_SPECULAR;
    c->in_begin = !other;
    c->begin_mode = other ? SGI_GL_LINE_STRIP : SGI_GL_QUAD_STRIP;
    c->nheld = other ? 0 : 3;
    c->cur.scissor_test = !other;
    c->cur.scissor[2] = other ? 0 : 1280;
    c->cur.scissor[3] = other ? 1024 : 0;
    c->cur.alpha_test = !other;
    c->cur.alpha_func = other ? SGI_GL_EQUAL : SGI_GL_NOTEQUAL;
    c->cur.alpha_ref = other ? 1.0f : 0.375f;
    c->cur.stencil_test = !other;
    c->cur.stencil_func = other ? SGI_GL_LEQUAL : SGI_GL_NEVER;
    c->cur.stencil_op[0] = other ? SGI_GL_INVERT : 0;
    c->cur.stencil_op[1] = other ? SGI_GL_INCR : SGI_GL_REPLACE;
    c->cur.stencil_op[2] = other ? SGI_GL_DECR : SGI_GL_KEEP;
    c->cur.blend = !other;
    c->cur.blend_src = other ? SGI_GL_SRC_ALPHA_SATURATE : SGI_GL_DST_ALPHA;
    c->cur.blend_dst = other ? SGI_GL_SRC_COLOR : 0;
    c->cur.blend_equation = other ? SGI_GL_MAX : SGI_GL_FUNC_SUBTRACT;
    for (int i = 0; i < 4; i++) {
        c->cur.blend_color[i] = other ? 0.25f * i : 1.0f - 0.125f * i;
    }
    c->cur.index_logic_op = !other;
    c->cur.color_logic_op = other;
    c->cur.logic_op_mode = other ? SGI_GL_SET : SGI_GL_CLEAR;
    c->cur.colour_mask = other ? SGI_GL_COLOUR_MASK_RED :
                                 SGI_GL_COLOUR_MASK_ALL &
                                 ~SGI_GL_COLOUR_MASK_GREEN;
    c->cur.fog = !other;
    c->cur.fog_mode = other ? SGI_GL_LINEAR : SGI_GL_EXP2;
    c->cur.fog_density = other ? 0.0f : 2.5f;
    for (int i = 0; i < 4; i++) {
        c->cur.fog_color[i] = other ? 1.0f - 0.25f * i : 0.125f * i;
    }

    g_assert_true(sgi_gl_context_valid(c, &error_abort));
}

/* The first byte at which two contexts differ, or -1. */
static ssize_t first_difference(const SGIGLContext *a, const SGIGLContext *b)
{
    const uint8_t *pa = (const uint8_t *)a, *pb = (const uint8_t *)b;

    for (size_t i = 0; i < sizeof(*a); i++) {
        if (pa[i] != pb[i]) {
            return i;
        }
    }
    return -1;
}

/*
 * Mark the bytes of the structure at base that vmsd describes, its nested
 * descriptions and subsections included. A field kind this walk does not
 * know fails it, so a description that outgrows the walk is noticed.
 */
static void cover(const VMStateDescription *vmsd, size_t base,
                  uint8_t *covered, size_t len)
{
    for (const VMStateField *f = vmsd->fields; f->name; f++) {
        size_t n = f->flags & VMS_ARRAY ? f->num : 1;

        g_assert_cmphex(f->flags & ~(VMS_SINGLE | VMS_ARRAY | VMS_STRUCT),
                        ==, 0);
        g_assert_null(f->field_exists);
        for (size_t i = 0; i < n; i++) {
            size_t at = base + f->offset + i * f->size;

            if (f->flags & VMS_STRUCT) {
                cover(f->vmsd, at, covered, len);
                continue;
            }
            g_assert_cmpuint(at + f->size, <=, len);
            for (size_t k = at; k < at + f->size; k++) {
                if (covered[k]) {
                    g_test_message("%s/%s describes byte %zu twice",
                                   vmsd->name, f->name, k);
                    g_assert_not_reached();
                }
                covered[k] = 1;
            }
        }
    }
    for (const VMStateDescription * const *s = vmsd->subsections; s && *s;
         s++) {
        cover(*s, base, covered, len);
    }
}

typedef void (*SubsectionFn)(const VMStateDescription *sub, size_t base,
                             void *opaque);

/*
 * Call fn for every subsection of vmsd and of the descriptions nested in
 * it, with base the offset in the context of the structure it describes.
 */
static void for_each_subsection(const VMStateDescription *vmsd, size_t base,
                                SubsectionFn fn, void *opaque)
{
    for (const VMStateField *f = vmsd->fields; f->name; f++) {
        size_t n = f->flags & VMS_ARRAY ? f->num : 1;

        for (size_t i = 0; f->flags & VMS_STRUCT && i < n; i++) {
            for_each_subsection(f->vmsd, base + f->offset + i * f->size, fn,
                                opaque);
        }
    }
    for (const VMStateDescription * const *s = vmsd->subsections; s && *s;
         s++) {
        fn(*s, base, opaque);
        for_each_subsection(*s, base, fn, opaque);
    }
}

typedef void (*ElementFn)(const VMStateDescription *sub, size_t base,
                          size_t at, size_t size, void *opaque);

typedef struct ElementWalk {
    ElementFn fn;
    void *opaque;
} ElementWalk;

static void walk_elements(const VMStateDescription *sub, size_t base,
                          void *opaque)
{
    ElementWalk *w = opaque;

    for (const VMStateField *f = sub->fields; f->name; f++) {
        size_t n = f->flags & VMS_ARRAY ? f->num : 1;

        g_assert_false(f->flags & VMS_STRUCT);
        for (size_t i = 0; i < n; i++) {
            w->fn(sub, base, base + f->offset + i * f->size, f->size,
                  w->opaque);
        }
    }
}

/*
 * Call fn for each element of each field a subsection describes, at its
 * offset in the context, with the subsection and its structure's offset.
 */
static void for_each_optional(ElementFn fn, void *opaque)
{
    ElementWalk w = { fn, opaque };

    for_each_subsection(&vmstate_sgi_gl_context, 0, walk_elements, &w);
}

static void reset_element(const VMStateDescription *sub, size_t base,
                          size_t at, size_t size, void *opaque)
{
    SGIGLContext *c = opaque;
    g_autofree SGIGLContext *init = g_new(SGIGLContext, 1);

    sgi_gl_init(init, 0);
    memcpy((uint8_t *)c + at, (uint8_t *)init + at, size);
}

/*
 * Give every field a subsection describes sgi_gl_init()'s value, which a
 * real load finds in a reset device: a subsection that .needed leaves
 * out keeps the destination's value.
 */
static void reset_optional(SGIGLContext *c)
{
    for_each_optional(reset_element, c);
}

/*
 * Every byte of SGIGLContext is migrated, once. A field added without a
 * description, or dropped from one, fails here, and so does padding, in
 * which a small field could otherwise hide.
 */
static void test_every_byte_described(void)
{
    g_autofree uint8_t *covered = g_malloc0(sizeof(SGIGLContext));
    cover(&vmstate_sgi_gl_context, 0, covered, sizeof(SGIGLContext));
    for (size_t i = 0; i < sizeof(SGIGLContext); i++) {
        if (!covered[i]) {
            g_test_message("byte %zu of SGIGLContext is not migrated", i);
        }
        g_assert_true(covered[i]);
    }
}

/* Each variant survives the trip over the other, byte for byte. */
static void test_round_trip(void)
{
    for (int dir = 0; dir < 2; dir++) {
        g_autofree TestHead *src = g_new0(TestHead, 1);
        g_autofree TestHead *dst = g_new0(TestHead, 1);
        GByteArray *stream;
        ssize_t diff;

        src->reg = 0x1234;
        for (int i = 0; i < CONTEXTS; i++) {
            populate(&src->gl[i], dir ^ i);
            populate(&dst->gl[i], !(dir ^ i));
            reset_optional(&dst->gl[i]);
        }
        stream = save_head(src);
        g_assert_cmpint(load_head(stream, dst, NULL), ==, 0);
        g_assert_cmphex(dst->reg, ==, 0x1234);
        for (int i = 0; i < CONTEXTS; i++) {
            diff = first_difference(&dst->gl[i], &src->gl[i]);
            if (diff >= 0) {
                g_test_message("context %d differs at byte %zd", i, diff);
            }
            g_assert_cmpint(diff, ==, -1);
        }
        g_byte_array_unref(stream);
    }
}

static void assert_not_needed(const VMStateDescription *sub, size_t base,
                              void *opaque)
{
    SGIGLContext *c = opaque;

    g_assert_nonnull(sub->needed);
    if (sub->needed((uint8_t *)c + base)) {
        g_test_message("%s is needed at sgi_gl_init()'s values", sub->name);
    }
    g_assert_false(sub->needed((uint8_t *)c + base));
}

/* A context at sgi_gl_init()'s values needs no subsection. */
static void test_initial_needs_no_subsection(void)
{
    g_autofree SGIGLContext *c = g_new(SGIGLContext, 1);

    sgi_gl_init(c, 1);
    for_each_subsection(&vmstate_sgi_gl_context, 0, assert_not_needed, c);
}

/*
 * One element of a subsection's field moved away from sgi_gl_init()'s
 * value, to the one moved holds, in an otherwise initial context: the
 * subsection is needed, and the context survives the trip into a reset
 * destination byte for byte.
 */
static void trip_one_element(const VMStateDescription *sub, size_t base,
                             size_t at, size_t size, void *opaque)
{
    const SGIGLContext *moved = opaque;
    g_autofree TestHead *src = g_new0(TestHead, 1);
    g_autofree TestHead *dst = g_new0(TestHead, 1);
    uint8_t *field;
    GByteArray *stream;

    for (int i = 0; i < CONTEXTS; i++) {
        sgi_gl_init(&src->gl[i], 0x40 + i);
        sgi_gl_init(&dst->gl[i], 0);
    }
    field = (uint8_t *)&src->gl[1] + at;
    if (!memcmp(field, (const uint8_t *)moved + at, size)) {
        return;
    }
    memcpy(field, (const uint8_t *)moved + at, size);
    g_assert_true(sgi_gl_context_valid(&src->gl[1], &error_abort));
    if (!sub->needed((uint8_t *)&src->gl[1] + base)) {
        g_test_message("%s is not needed with byte %zu moved", sub->name,
                       at);
    }
    g_assert_true(sub->needed((uint8_t *)&src->gl[1] + base));
    stream = save_head(src);
    g_assert_cmpint(load_head(stream, dst, NULL), ==, 0);
    g_byte_array_unref(stream);
    g_assert_cmpint(first_difference(&dst->gl[1], &src->gl[1]), ==, -1);
}

/* Each field a subsection describes, element by element, needs it. */
static void test_each_optional_field_travels(void)
{
    for (int other = 0; other < 2; other++) {
        g_autofree SGIGLContext *moved = g_new(SGIGLContext, 1);

        populate(moved, other);
        for_each_optional(trip_one_element, moved);
    }
}

typedef struct Draws {
    uint32_t n;
    uint32_t mode[8];
    uint32_t count[8];
    float modelview_x[8];
    float position[8][4][4];
} Draws;

static void record_draw(void *opaque, const SGIGLDraw *d)
{
    Draws *r = opaque;

    g_assert_cmpuint(r->n, <, ARRAY_SIZE(r->mode));
    r->mode[r->n] = d->mode;
    r->count[r->n] = d->count;
    r->modelview_x[r->n] = d->state->modelview[0];
    for (uint32_t v = 0; v < d->count; v++) {
        memcpy(r->position[r->n][v],
               &d->vertices[v * SGI_GL_VERTEX_FLOATS + SGI_GL_V_POSITION],
               sizeof(r->position[r->n][v]));
    }
    r->n++;
}

static void send_cmd(SGIGLContext *c, Draws *r, uint16_t op, uint32_t nargs,
                 const uint32_t *args)
{
    SGIGLSink sink = { .draw = record_draw, .opaque = r };
    SGIGLCmd cmd = { .op = op, .nargs = nargs, .args = args };

    sgi_gl_command(c, &sink, &cmd);
}

static void send_vertex(SGIGLContext *c, Draws *r, float x, float y)
{
    uint32_t a[3] = { float_bits(x), float_bits(y), float_bits(0.5f) };

    send_cmd(c, r, SGI_GL_OP_VERTEX3F, 3, a);
}

/*
 * A triangle two vertices in when the guest stops is finished by its third
 * on the destination, under the modelview the client loaded: neither
 * comes back as initial state.
 */
static void test_open_primitive_survives(void)
{
    g_autofree TestHead *src = g_new0(TestHead, 1);
    g_autofree TestHead *dst = g_new0(TestHead, 1);
    static const float scale2[16] = {
        2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1,
    };
    uint32_t mode = SGI_GL_TRIANGLES, answer[SGI_GL_GET_WORDS];
    Draws before = { 0 }, after = { 0 };
    GByteArray *stream;
    unsigned n;

    for (int i = 0; i < CONTEXTS; i++) {
        sgi_gl_init(&src->gl[i], 0x40 + i);
        sgi_gl_init(&dst->gl[i], 0);
    }
    sgi_gl_set_matrix(&src->gl[1], SGI_GL_MODELVIEW, scale2);
    send_cmd(&src->gl[1], &before, SGI_GL_OP_BEGIN, 1, &mode);
    send_vertex(&src->gl[1], &before, 0.25f, 0.0f);
    send_vertex(&src->gl[1], &before, 0.0f, 0.25f);
    g_assert_cmpuint(before.n, ==, 0);

    stream = save_head(src);
    g_assert_cmpint(load_head(stream, dst, NULL), ==, 0);
    g_byte_array_unref(stream);

    g_assert_cmphex(dst->gl[1].hw_id, ==, 0x41);
    g_assert_true(sgi_gl_get(&dst->gl[1], SGI_GL_MODELVIEW_MATRIX, answer,
                             &n));
    g_assert_cmpuint(n, ==, 16);
    g_assert_cmpmem(answer, sizeof(scale2), scale2, sizeof(scale2));
    send_vertex(&dst->gl[1], &after, -0.25f, 0.0f);
    g_assert_cmpuint(after.n, ==, 1);
    g_assert_cmpuint(after.mode[0], ==, SGI_GL_TRIANGLES);
    g_assert_cmpuint(after.count[0], ==, 3);
    g_assert_cmpfloat(after.modelview_x[0], ==, 2.0f);
    /* The held vertices keep the object coordinates they arrived with. */
    g_assert_cmpfloat(after.position[0][0][0], ==, 0.25f);
    g_assert_cmpfloat(after.position[0][1][1], ==, 0.25f);
    g_assert_cmpfloat(after.position[0][2][0], ==, -0.25f);
}

/*
 * Every state the context's own commands reach is valid: the held-vertex
 * limit per mode follows the assembler, vertex by vertex.
 */
static void test_reachable_states_valid(void)
{
    for (uint32_t mode = SGI_GL_POINTS; mode <= SGI_GL_POLYGON; mode++) {
        g_autofree SGIGLContext *c = g_new0(SGIGLContext, 1);
        Draws r = { 0 };

        sgi_gl_init(c, 1);
        g_assert_true(sgi_gl_context_valid(c, &error_abort));
        send_cmd(c, &r, SGI_GL_OP_BEGIN, 1, &mode);
        for (int v = 0; v < 7; v++) {
            r.n = 0;
            send_vertex(c, &r, v, v & 1);
            g_assert_true(sgi_gl_context_valid(c, &error_abort));
        }
        send_cmd(c, &r, SGI_GL_OP_END, 0, NULL);
        g_assert_true(sgi_gl_context_valid(c, &error_abort));
    }
}

/*
 * A field added to a nested description, as the header's rule says: a
 * subsection named for that description, here a stand-in for a light
 * inside a context, both nested in a family's table.
 */
typedef struct TestLight {
    uint32_t base;
    uint32_t added;
} TestLight;

typedef struct TestTable {
    TestLight light[2];
} TestTable;

static bool test_added_needed(void *opaque)
{
    return true;
}

static const VMStateDescription vmstate_test_light_added = {
    .name = "test-light/added",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = test_added_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(added, TestLight),
        VMSTATE_END_OF_LIST()
    },
};

/* The same field under the outer description's name, against the rule. */
static const VMStateDescription vmstate_test_light_misnamed = {
    .name = "test-table/added",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = test_added_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(added, TestLight),
        VMSTATE_END_OF_LIST()
    },
};

#define TEST_LIGHT(_vmsd, _sub)                                          \
    static const VMStateDescription _vmsd = {                            \
        .name = "test-light",                                            \
        .version_id = 1,                                                 \
        .minimum_version_id = 1,                                         \
        .fields = (const VMStateField[]) {                               \
            VMSTATE_UINT32(base, TestLight),                             \
            VMSTATE_END_OF_LIST()                                        \
        },                                                               \
        .subsections = (const VMStateDescription * const []) {           \
            &(_sub), NULL                                                \
        },                                                               \
    }

#define TEST_TABLE(_vmsd, _light)                                        \
    static const VMStateDescription _vmsd = {                            \
        .name = "test-table",                                            \
        .version_id = 1,                                                 \
        .minimum_version_id = 1,                                         \
        .fields = (const VMStateField[]) {                               \
            VMSTATE_STRUCT_ARRAY(light, TestTable, 2, 0, _light,         \
                                 TestLight),                             \
            VMSTATE_END_OF_LIST()                                        \
        },                                                               \
    }

TEST_LIGHT(vmstate_test_light, vmstate_test_light_added);
TEST_TABLE(vmstate_test_table, vmstate_test_light);
TEST_LIGHT(vmstate_test_light_bad, vmstate_test_light_misnamed);
TEST_TABLE(vmstate_test_table_bad, vmstate_test_light_bad);

/* Save src and load it into dst under vmsd; the load's result. */
static int table_trip(const VMStateDescription *vmsd, TestTable *src,
                      TestTable *dst)
{
    QIOChannelBuffer *bioc = qio_channel_buffer_new(4096);
    QEMUFile *f = qemu_file_new_output(QIO_CHANNEL(bioc));
    GByteArray *stream = g_byte_array_new();
    Error *err = NULL;
    int ret;

    g_assert_cmpint(vmstate_save_state(f, vmsd, src, NULL, &error_abort),
                    ==, 0);
    g_assert_cmpint(qemu_fflush(f), ==, 0);
    g_byte_array_append(stream, bioc->data, bioc->usage);
    g_byte_array_append(stream, (const guint8 *)"", 1);
    qemu_fclose(f);
    object_unref(OBJECT(bioc));

    bioc = qio_channel_buffer_new(stream->len);
    memcpy(bioc->data, stream->data, stream->len);
    bioc->usage = stream->len;
    f = qemu_file_new_input(QIO_CHANNEL(bioc));
    ret = vmstate_load_state(f, vmsd, dst, 1, &err);
    if (err) {
        g_test_message("load: %s", error_get_pretty(err));
    }
    error_free(err);
    qemu_fclose(f);
    object_unref(OBJECT(bioc));
    g_byte_array_unref(stream);
    return ret;
}

static void test_nested_subsection_name(void)
{
    TestTable src = { { { 1, 0x11 }, { 2, 0x22 } } }, dst = { 0 };

    g_assert_cmpint(table_trip(&vmstate_test_table, &src, &dst), ==, 0);
    g_assert_cmpmem(&dst, sizeof(dst), &src, sizeof(src));

    /*
     * Named for the outer description, the subsection is not found under
     * the light's name, and the rest of the stream is misread.
     */
    memset(&dst, 0, sizeof(dst));
    if (table_trip(&vmstate_test_table_bad, &src, &dst) == 0) {
        g_assert_cmpint(memcmp(&dst, &src, sizeof(dst)), !=, 0);
    }
}

/* A field set to value, and the words the refusal names. */
typedef struct Corruption {
    size_t offset;
    uint32_t value;
    const char *reason;
} Corruption;

#define BAD(_field, _value, _reason) \
    { offsetof(SGIGLContext, _field), (_value), (_reason) }

static const Corruption corruptions[] = {
    BAD(cur.depth_test, 2, "depth_test"),
    BAD(cur.depth_mask, 2, "depth_mask"),
    BAD(cur.cull_face, 2, "cull_face"),
    BAD(dither, 2, "dither"),
    BAD(light_model_local_viewer, 2, "light_model_local_viewer"),
    BAD(light_model_two_side, 2, "light_model_two_side"),
    BAD(color_material, 2, "color_material is"),
    BAD(lighting, 2, "lighting"),
    BAD(normalize, 2, "normalize is"),
    BAD(rescale_normal, 2, "rescale_normal"),
    BAD(normal_matrix_given, 2, "normal_matrix_given"),
    BAD(normalize_always, 2, "normalize_always"),
    BAD(cur_edge, 0x80000000, "cur_edge"),
    BAD(raster_valid, 2, "raster_valid"),
    BAD(strip_odd, 2, "strip_odd"),
    BAD(cur.drawable.bound, 2, "drawable.bound"),
    BAD(cur.drawable.clip.empty, 2, "drawable.clip.empty"),
    BAD(in_begin, 2, "in_begin"),
    BAD(mv_depth, SGI_GL_STACK_DEPTH, "mv_depth"),
    BAD(pr_depth, SGI_GL_STACK_DEPTH, "pr_depth"),
    BAD(tx_depth, UINT32_MAX, "tx_depth"),
    BAD(cur.depth_func, SGI_GL_ALWAYS + 1, "depth function"),
    BAD(cur.depth_range[0], 0x3fc00000 /* 1.5 */, "depth range"),
    BAD(cur.depth_range[1], 0xbe800000 /* -0.25 */, "depth range"),
    BAD(cur.depth_range[0], 0x7fc00000 /* NaN */, "depth range"),
    BAD(cur.index_bits, 33, "colour index"),
    BAD(cur.cull_face_mode, SGI_GL_CW, "cull face"),
    BAD(cur.front_face, SGI_GL_FRONT, "front face"),
    BAD(cur.drawable.clip.n, SGI_CLIP_MAX + 1, "clip rectangles"),
    BAD(cur.scissor_test, 2, "scissor_test"),
    BAD(cur.scissor[2], 0xffffffff /* -1 */, "scissor box"),
    BAD(cur.scissor[3], 0x80000000, "scissor box"),
    BAD(cur.alpha_test, 2, "alpha_test"),
    BAD(cur.alpha_func, SGI_GL_ALWAYS + 1, "alpha function"),
    BAD(cur.alpha_ref, 0x3f800001 /* just above 1 */, "alpha reference"),
    BAD(cur.alpha_ref, 0x80000001 /* just below 0 */, "alpha reference"),
    BAD(cur.alpha_ref, 0x7fc00000 /* NaN */, "alpha reference"),
    BAD(cur.stencil_test, 2, "stencil_test"),
    BAD(cur.stencil_func, SGI_GL_ALWAYS + 1, "stencil function"),
    BAD(cur.stencil_op[0], 1, "stencil operation"),
    BAD(cur.stencil_op[1], SGI_GL_DECR + 1, "stencil operation"),
    BAD(cur.stencil_op[2], SGI_GL_KEEP - 1, "stencil operation"),
    BAD(cur.blend, 2, "blend is"),
    /* A destination-only factor as the source, and the reverse. */
    BAD(cur.blend_src, SGI_GL_SRC_COLOR, "blend factors"),
    BAD(cur.blend_dst, SGI_GL_SRC_ALPHA_SATURATE, "blend factors"),
    BAD(cur.blend_equation, SGI_GL_FUNC_ADD + 3, "blend equation"),
    BAD(cur.blend_color[3], 0x3f800001 /* just above 1 */, "blend colour"),
    BAD(cur.index_logic_op, 2, "index_logic_op"),
    BAD(cur.color_logic_op, 2, "color_logic_op"),
    BAD(cur.logic_op_mode, SGI_GL_SET + 1, "logical operation"),
    BAD(cur.logic_op_mode, SGI_GL_CLEAR - 1, "logical operation"),
    BAD(cur.colour_mask, SGI_GL_COLOUR_MASK_ALL + 1, "colour mask"),
    BAD(cur.fog, 2, "fog is"),
    BAD(cur.fog_mode, SGI_GL_EXP2 + 1, "fog mode"),
    BAD(cur.fog_mode, 0, "fog mode"),
    BAD(cur.fog_density, 0xbf800000 /* -1 */, "fog density"),
    BAD(cur.fog_density, 0x7fc00000 /* NaN */, "fog density"),
    BAD(cur.fog_color[0], 0xbe800000 /* -0.25 */, "fog colour"),
    BAD(lights_enabled, 1u << SGI_GL_LIGHTS, "light enables"),
    BAD(color_material_face, 0, "colour material face"),
    BAD(color_material_mode, SGI_GL_SHININESS, "colour material parameter"),
    BAD(begin_mode, SGI_GL_POLYGON + 1, "primitive mode"),
    /* The valid context holds three vertices of a quad strip. */
    BAD(nheld, SGI_GL_HELD_VERTICES + 1, "vertices held"),
    BAD(begin_mode, SGI_GL_LINES, "vertices held"),
    BAD(begin_mode, SGI_GL_POINTS, "vertices held"),
    BAD(begin_mode, SGI_GL_TRIANGLE_FAN, "vertices held"),
};

/* A stream carrying one bad field in the second context is refused. */
static void test_refuses_corrupt(void)
{
    for (size_t i = 0; i <= ARRAY_SIZE(corruptions); i++) {
        g_autofree TestHead *src = g_new0(TestHead, 1);
        g_autofree TestHead *dst = g_new0(TestHead, 1);
        g_autofree char *why = NULL;
        const char *reason;
        GByteArray *stream;

        populate(&src->gl[0], false);
        populate(&src->gl[1], false);
        if (i < ARRAY_SIZE(corruptions)) {
            put_u32(&src->gl[1], corruptions[i].offset, corruptions[i].value);
            reason = corruptions[i].reason;
        } else {
            /* Vertices held with no primitive open. */
            src->gl[1].in_begin = false;
            reason = "outside glBegin";
        }
        g_assert_false(sgi_gl_context_valid(&src->gl[1], NULL));
        stream = save_head(src);
        g_assert_cmpint(load_head(stream, dst, &why), !=, 0);
        g_assert_nonnull(why);
        if (!strstr(why, reason)) {
            g_test_message("case %zu: expected \"%s\"", i, reason);
        }
        g_assert_nonnull(strstr(why, reason));
        g_byte_array_unref(stream);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    module_call_init(MODULE_INIT_QOM);
    g_test_add_func("/sgi-gl-vmstate/every-byte-described",
                    test_every_byte_described);
    g_test_add_func("/sgi-gl-vmstate/round-trip", test_round_trip);
    g_test_add_func("/sgi-gl-vmstate/initial-needs-no-subsection",
                    test_initial_needs_no_subsection);
    g_test_add_func("/sgi-gl-vmstate/each-optional-field-travels",
                    test_each_optional_field_travels);
    g_test_add_func("/sgi-gl-vmstate/open-primitive-survives",
                    test_open_primitive_survives);
    g_test_add_func("/sgi-gl-vmstate/reachable-states-valid",
                    test_reachable_states_valid);
    g_test_add_func("/sgi-gl-vmstate/refuses-corrupt", test_refuses_corrupt);
    g_test_add_func("/sgi-gl-vmstate/nested-subsection-name",
                    test_nested_subsection_name);
    return g_test_run();
}
