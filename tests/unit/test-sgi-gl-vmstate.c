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

/*
 * The bytes of SGIGLContext no field holds, which the compiler leaves
 * after each bool. They are found from the neighbours' offsets, so the
 * list follows the structure; the field-coverage test fails on any other
 * byte no description carries.
 */
static const struct {
    size_t start, end;
} padding[] = {
    { offsetof(SGIGLContext, cur.drawable.bound) + sizeof(bool),
      offsetof(SGIGLContext, cur.drawable.origin) },
    { offsetof(SGIGLContext, in_begin) + sizeof(bool),
      offsetof(SGIGLContext, held) },
};

static bool is_padding(size_t at)
{
    for (size_t i = 0; i < ARRAY_SIZE(padding); i++) {
        if (at >= padding[i].start && at < padding[i].end) {
            return true;
        }
    }
    return false;
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
    for (size_t i = 0; i < ARRAY_SIZE(padding); i++) {
        memset(b + padding[i].start, 0, padding[i].end - padding[i].start);
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
    c->cur.drawable.clip.n = other ? 3 : SGI_CLIP_MAX;
    c->lights_enabled = other ? 0x5a : 0xa5;
    c->color_material_face = other ? SGI_GL_FRONT : SGI_GL_BACK;
    c->color_material_mode = other ? SGI_GL_EMISSION : SGI_GL_SPECULAR;
    c->in_begin = !other;
    c->begin_mode = other ? SGI_GL_LINE_STRIP : SGI_GL_QUAD_STRIP;
    c->nheld = other ? 0 : 3;

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

/*
 * Every byte of SGIGLContext but padding is migrated, once. A field added
 * without a description, or dropped from one, fails here.
 */
static void test_every_byte_described(void)
{
    g_autofree uint8_t *covered = g_malloc0(sizeof(SGIGLContext));
    size_t described = 0, padded = 0;

    for (size_t i = 0; i < ARRAY_SIZE(padding); i++) {
        g_assert_cmpuint(padding[i].start, <, padding[i].end);
        padded += padding[i].end - padding[i].start;
    }
    cover(&vmstate_sgi_gl_context, 0, covered, sizeof(SGIGLContext));
    for (size_t i = 0; i < sizeof(SGIGLContext); i++) {
        if (!covered[i] && !is_padding(i)) {
            g_test_message("byte %zu of SGIGLContext is not migrated", i);
        }
        g_assert_true(covered[i] || is_padding(i));
        g_assert_false(covered[i] && is_padding(i));
        described += covered[i];
    }
    g_assert_cmpuint(described + padded, ==, sizeof(SGIGLContext));
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
    g_test_add_func("/sgi-gl-vmstate/open-primitive-survives",
                    test_open_primitive_survives);
    g_test_add_func("/sgi-gl-vmstate/reachable-states-valid",
                    test_reachable_states_valid);
    g_test_add_func("/sgi-gl-vmstate/refuses-corrupt", test_refuses_corrupt);
    return g_test_run();
}
