/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Expected OpenGL doorbell layouts are listed independently of the selector
 * table. A changed argument count or layout status therefore fails here.
 */
#include "qemu/osdep.h"
#include "hw/misc/sgi-kona-gl-decode.h"

typedef struct Expected {
    uint32_t selector;
    SGIKonaGLLayoutEvidence evidence;
    unsigned argc;
    uint32_t arg0;      /* a value the decoder must accept for argument 0 */
} Expected;

static const Expected subset[] = {
    { 0x0,   SGI_KONA_GL_LAYOUT_UNRESOLVED,  0, 0 },     /* glBegin */
    { 0x1a,  SGI_KONA_GL_LAYOUT_INFERRED,    1, 0x4100 }, /* glClear */
    { 0x1b,  SGI_KONA_GL_LAYOUT_INFERRED,    4, 0 },     /* glClearAccum */
    { 0x1c,  SGI_KONA_GL_LAYOUT_INFERRED,    1, 0 },     /* glClearIndex */
    { 0x1d,  SGI_KONA_GL_LAYOUT_INFERRED,    4, 0x3f800000 },
    { 0x1e,  SGI_KONA_GL_LAYOUT_INFERRED,    1, 0 },     /* glClearStencil */
    { 0x1f,  SGI_KONA_GL_LAYOUT_UNRESOLVED,  0, 0 },     /* glClearDepth */
    { 0x39,  SGI_KONA_GL_LAYOUT_UNRESOLVED,  0, 0 },     /* glReadPixels */
    { 0x71,  SGI_KONA_GL_LAYOUT_ESTABLISHED, 1, 0 },     /* glEnd */
    { 0x8c,  SGI_KONA_GL_LAYOUT_INFERRED,    2, 0 },     /* glVertex2f */
    { 0x90,  SGI_KONA_GL_LAYOUT_INFERRED,    3, 0 },     /* glVertex3f */
    { 0x116, SGI_KONA_GL_LAYOUT_ESTABLISHED, 4, 0 },     /* readback */
    { 0x119, SGI_KONA_GL_LAYOUT_UNRESOLVED,  0, 0 },
    { 0x1ca, SGI_KONA_GL_LAYOUT_ESTABLISHED, 1, 0 },
    { 0x1cc, SGI_KONA_GL_LAYOUT_ESTABLISHED, 1, 0 },     /* FlushRaster */
    { 0x1d6, SGI_KONA_GL_LAYOUT_UNRESOLVED,  0, 0 },
    { 0x500, SGI_KONA_GL_LAYOUT_UNRESOLVED,  0, 0 },
    { 0xc6,  SGI_KONA_GL_LAYOUT_ESTABLISHED, 1, 1 },     /* glFinish */
    { 0xc7,  SGI_KONA_GL_LAYOUT_ESTABLISHED, 1, 0 },     /* glFlush */
};

static void test_table_is_the_subset(void)
{
    size_t count;
    const SGIKonaGLSelectorInfo *table = sgi_kona_gl_selector_table(&count);

    g_assert_cmpuint(count, ==, ARRAY_SIZE(subset));
    for (size_t i = 0; i < count; i++) {
        g_assert_cmpuint(table[i].argc, <=, SGI_KONA_GL_MAX_ARGS);
        g_assert_cmpuint(table[i].num_arg0_values, <=,
                         ARRAY_SIZE(table[i].arg0_values));
        g_assert_nonnull(table[i].call);
        for (size_t j = i + 1; j < count; j++) {
            g_assert_cmphex(table[i].selector, !=, table[j].selector);
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(subset); i++) {
        const SGIKonaGLSelectorInfo *info = sgi_kona_gl_lookup(
            subset[i].selector);

        g_assert_nonnull(info);
        g_assert_cmpint(info->evidence, ==, subset[i].evidence);
        g_assert_cmpuint(info->argc, ==, subset[i].argc);
    }
}

/* Every framed selector decodes with exactly its argument words. */
static void test_each_subset_selector(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(subset); i++) {
        const Expected *e = &subset[i];
        uint32_t words[1 + SGI_KONA_GL_MAX_ARGS + 1];
        SGIKonaGLCommand cmd;
        SGIKonaGLDecodeResult r;

        words[0] = e->selector;
        words[1] = e->arg0;
        for (unsigned a = 1; a < SGI_KONA_GL_MAX_ARGS + 1; a++) {
            words[1 + a] = 0x1000 + a;
        }

        r = sgi_kona_gl_decode(words, ARRAY_SIZE(words),
                               SGI_KONA_GL_LENGTH_UNDECLARED, &cmd);
        g_assert_cmphex(cmd.selector, ==, e->selector);
        g_assert_nonnull(cmd.info);
        if (e->evidence == SGI_KONA_GL_LAYOUT_UNRESOLVED) {
            g_assert_cmpint(r, ==, SGI_KONA_GL_DECODE_UNRESOLVED_LAYOUT);
            g_assert_cmpuint(cmd.length, ==, 1);
            continue;
        }
        g_assert_cmpint(r, ==, SGI_KONA_GL_DECODE_OK);
        g_assert_cmpuint(cmd.argc, ==, e->argc);
        g_assert_cmpuint(cmd.length, ==, 1 + e->argc);
        g_assert_cmphex(cmd.args[0], ==, e->arg0);
        for (unsigned a = 1; a < e->argc; a++) {
            g_assert_cmphex(cmd.args[a], ==, 0x1000 + a);
        }

        /* The same packet with the transport declaring its true length. */
        r = sgi_kona_gl_decode(words, ARRAY_SIZE(words), 1 + e->argc, &cmd);
        g_assert_cmpint(r, ==, SGI_KONA_GL_DECODE_OK);
    }
}

/* glXSwapBuffers and glFinish share 0xc6, told apart by argument 0. */
static void test_swap_and_finish_share_a_selector(void)
{
    const uint32_t swap[] = { 0xc6, 0 };
    const uint32_t finish[] = { 0xc6, 1 };
    SGIKonaGLCommand cmd;

    g_assert_cmpint(sgi_kona_gl_decode(swap, 2, 0, &cmd), ==,
                    SGI_KONA_GL_DECODE_OK);
    g_assert_cmphex(cmd.args[0], ==, 0);
    g_assert_cmpint(sgi_kona_gl_decode(finish, 2, 0, &cmd), ==,
                    SGI_KONA_GL_DECODE_OK);
    g_assert_cmphex(cmd.args[0], ==, 1);
}

static void test_unknown_selector(void)
{
    /* 0x1a6 is a real catalogue selector, but outside the subset. */
    const uint32_t words[] = { 0x1a6, 0x14, 0, 0 };
    const uint32_t junk[] = { 0xdeadbeef };
    SGIKonaGLCommand cmd;

    g_assert_cmpint(sgi_kona_gl_decode(words, ARRAY_SIZE(words), 0, &cmd), ==,
                    SGI_KONA_GL_DECODE_UNKNOWN_SELECTOR);
    g_assert_null(cmd.info);
    g_assert_cmphex(cmd.selector, ==, 0x1a6);
    g_assert_cmpuint(cmd.length, ==, 1);

    g_assert_cmpint(sgi_kona_gl_decode(junk, 1, 0, &cmd), ==,
                    SGI_KONA_GL_DECODE_UNKNOWN_SELECTOR);
    g_assert_null(sgi_kona_gl_lookup(0xdeadbeef));
}

static void test_truncated_packet(void)
{
    const uint32_t vertex[] = { 0x90, 0x3f800000, 0x40000000 };
    SGIKonaGLCommand cmd;

    g_assert_cmpint(sgi_kona_gl_decode(vertex, ARRAY_SIZE(vertex), 0, &cmd),
                    ==, SGI_KONA_GL_DECODE_TRUNCATED);
    g_assert_cmpuint(cmd.length, ==, 4);
    g_assert_nonnull(cmd.info);

    /* A selector alone is truncated too, and so is nothing at all. */
    g_assert_cmpint(sgi_kona_gl_decode(vertex, 1, 0, &cmd), ==,
                    SGI_KONA_GL_DECODE_TRUNCATED);
    g_assert_cmpint(sgi_kona_gl_decode(vertex, 0, 0, &cmd), ==,
                    SGI_KONA_GL_DECODE_TRUNCATED);
    g_assert_cmpuint(cmd.length, ==, 1);
}

static void test_length_mismatch(void)
{
    const uint32_t color[] = { 0x1d, 0, 0, 0, 0x3f800000 };
    SGIKonaGLCommand cmd;

    g_assert_cmpint(sgi_kona_gl_decode(color, ARRAY_SIZE(color), 4, &cmd), ==,
                    SGI_KONA_GL_DECODE_LENGTH_MISMATCH);
    g_assert_cmpuint(cmd.length, ==, 5);
    g_assert_cmpint(sgi_kona_gl_decode(color, ARRAY_SIZE(color), 6, &cmd), ==,
                    SGI_KONA_GL_DECODE_LENGTH_MISMATCH);
    /* A declared length is checked before the available words are. */
    g_assert_cmpint(sgi_kona_gl_decode(color, 2, 3, &cmd), ==,
                    SGI_KONA_GL_DECODE_LENGTH_MISMATCH);
}

static void test_unexpected_literal_argument(void)
{
    const uint32_t end[] = { 0x71, 5 };
    const uint32_t swap[] = { 0xc6, 2 };
    SGIKonaGLCommand cmd;

    g_assert_cmpint(sgi_kona_gl_decode(end, 2, 0, &cmd), ==,
                    SGI_KONA_GL_DECODE_UNEXPECTED_ARGUMENT);
    g_assert_cmpuint(cmd.length, ==, 2);
    g_assert_cmphex(cmd.args[0], ==, 5);
    g_assert_cmpint(sgi_kona_gl_decode(swap, 2, 0, &cmd), ==,
                    SGI_KONA_GL_DECODE_UNEXPECTED_ARGUMENT);
}

/*
 * One frame as the doorbell would see it: clear colour, clear, one
 * triangle's vertices, end, swap, finish, with an unknown word in the
 * middle that the caller skips by its one-word length.
 */
static void test_multi_command_stream(void)
{
    static const uint32_t stream[] = {
        0x1d, 0x00000000, 0x00000000, 0x3f000000, 0x3f800000,
        0x1a, 0x00004000,
        0x90, 0xbf800000, 0xbf800000, 0x00000000,
        0x90, 0x3f800000, 0xbf800000, 0x00000000,
        0x12345678,
        0x90, 0x00000000, 0x3f800000, 0x00000000,
        0x71, 0x0,
        0xc6, 0x0,
        0xc6, 0x1,
    };
    static const struct {
        SGIKonaGLDecodeResult result;
        uint32_t selector;
        unsigned length;
    } want[] = {
        { SGI_KONA_GL_DECODE_OK, 0x1d, 5 },
        { SGI_KONA_GL_DECODE_OK, 0x1a, 2 },
        { SGI_KONA_GL_DECODE_OK, 0x90, 4 },
        { SGI_KONA_GL_DECODE_OK, 0x90, 4 },
        { SGI_KONA_GL_DECODE_UNKNOWN_SELECTOR, 0x12345678, 1 },
        { SGI_KONA_GL_DECODE_OK, 0x90, 4 },
        { SGI_KONA_GL_DECODE_OK, 0x71, 2 },
        { SGI_KONA_GL_DECODE_OK, 0xc6, 2 },
        { SGI_KONA_GL_DECODE_OK, 0xc6, 2 },
    };
    size_t pos = 0, n = 0;

    while (pos < ARRAY_SIZE(stream)) {
        SGIKonaGLCommand cmd;
        SGIKonaGLDecodeResult r = sgi_kona_gl_decode(
            &stream[pos], ARRAY_SIZE(stream) - pos,
            SGI_KONA_GL_LENGTH_UNDECLARED, &cmd);

        g_assert_cmpuint(n, <, ARRAY_SIZE(want));
        g_assert_cmpint(r, ==, want[n].result);
        g_assert_cmphex(cmd.selector, ==, want[n].selector);
        g_assert_cmpuint(cmd.length, ==, want[n].length);
        if (n == 3) {
            g_assert_cmphex(cmd.args[0], ==, 0x3f800000);
            g_assert_cmphex(cmd.args[1], ==, 0xbf800000);
        }
        pos += cmd.length;
        n++;
    }
    g_assert_cmpuint(pos, ==, ARRAY_SIZE(stream));
    g_assert_cmpuint(n, ==, ARRAY_SIZE(want));
}

static void test_result_names(void)
{
    g_assert_cmpstr(sgi_kona_gl_decode_result_name(SGI_KONA_GL_DECODE_OK),
                    ==, "ok");
    g_assert_cmpstr(sgi_kona_gl_decode_result_name(
                        SGI_KONA_GL_DECODE_LENGTH_MISMATCH),
                    ==, "length mismatch");
    g_assert_cmpstr(sgi_kona_gl_decode_result_name(99), ==, "invalid");
}

/* Independently specified by the cleared 159-entry return-wire contract. */
static const SGIKonaGLQueryInfo expected_queries[] = {
    { 0x0b10, 0x04, 1 },
    { 0x0b11, 0x03, 1 },
    { 0x0b14, 0x04, 6 },
    { 0x0b20, 0x04, 1 },
    { 0x0b21, 0x03, 1 },
    { 0x0b24, 0x04, 1 },
    { 0x0b25, 0x04, 1 },
    { 0x0b26, 0x04, 1 },
    { 0x0b40, 0x04, 2 },
    { 0x0b41, 0x04, 1 },
    { 0x0b42, 0x04, 1 },
    { 0x0b44, 0x04, 1 },
    { 0x0b45, 0x04, 1 },
    { 0x0b46, 0x04, 1 },
    { 0x0b50, 0x04, 1 },
    { 0x0b51, 0x04, 1 },
    { 0x0b52, 0x04, 1 },
    { 0x0b53, 0x13, 4 },
    { 0x0b54, 0x04, 1 },
    { 0x0b55, 0x04, 1 },
    { 0x0b56, 0x04, 1 },
    { 0x0b57, 0x04, 1 },
    { 0x0b60, 0x04, 1 },
    { 0x0b61, 0x03, 1 },
    { 0x0b62, 0x03, 1 },
    { 0x0b63, 0x03, 1 },
    { 0x0b64, 0x03, 1 },
    { 0x0b65, 0x04, 1 },
    { 0x0b66, 0x13, 4 },
    { 0x0b70, 0x12, 2 },
    { 0x0b71, 0x04, 1 },
    { 0x0b72, 0x04, 1 },
    { 0x0b73, 0x12, 1 },
    { 0x0b74, 0x04, 1 },
    { 0x0b80, 0x13, 4 },
    { 0x0b90, 0x04, 1 },
    { 0x0b91, 0x04, 1 },
    { 0x0b92, 0x04, 1 },
    { 0x0b93, 0x04, 1 },
    { 0x0b94, 0x04, 1 },
    { 0x0b95, 0x04, 1 },
    { 0x0b96, 0x04, 1 },
    { 0x0b97, 0x04, 1 },
    { 0x0b98, 0x04, 1 },
    { 0x0ba0, 0x04, 1 },
    { 0x0ba1, 0x04, 1 },
    { 0x0ba2, 0x04, 4 },
    { 0x0ba3, 0x04, 1 },
    { 0x0ba4, 0x04, 1 },
    { 0x0ba5, 0x04, 1 },
    { 0x0ba6, 0x03, 16 },
    { 0x0ba7, 0x03, 16 },
    { 0x0ba8, 0x03, 16 },
    { 0x0bb0, 0x04, 1 },
    { 0x0bc0, 0x04, 1 },
    { 0x0bc1, 0x04, 1 },
    { 0x0bc2, 0x13, 1 },
    { 0x0bd0, 0x04, 1 },
    { 0x0be0, 0x04, 1 },
    { 0x0be1, 0x04, 1 },
    { 0x0be2, 0x04, 1 },
    { 0x0bf0, 0x04, 1 },
    { 0x0bf1, 0x04, 1 },
    { 0x0bf2, 0x04, 1 },
    { 0x0c01, 0x04, 1 },
    { 0x0c02, 0x04, 1 },
    { 0x0c10, 0x04, 4 },
    { 0x0c11, 0x04, 1 },
    { 0x0c20, 0x03, 1 },
    { 0x0c21, 0x04, 1 },
    { 0x0c22, 0x13, 4 },
    { 0x0c23, 0x04, 4 },
    { 0x0c50, 0x04, 1 },
    { 0x0c51, 0x04, 1 },
    { 0x0c52, 0x04, 1 },
    { 0x0c53, 0x04, 1 },
    { 0x0c54, 0x04, 1 },
    { 0x0c60, 0x04, 1 },
    { 0x0c61, 0x04, 1 },
    { 0x0c62, 0x04, 1 },
    { 0x0c63, 0x04, 1 },
    { 0x0cb0, 0x04, 1 },
    { 0x0cb1, 0x04, 1 },
    { 0x0cb2, 0x04, 1 },
    { 0x0cb3, 0x04, 1 },
    { 0x0cb4, 0x04, 1 },
    { 0x0cb5, 0x04, 1 },
    { 0x0cb6, 0x04, 1 },
    { 0x0cb7, 0x04, 1 },
    { 0x0cb8, 0x04, 1 },
    { 0x0cb9, 0x04, 1 },
    { 0x0d70, 0x04, 1 },
    { 0x2a00, 0x04, 1 },
    { 0x2a01, 0x04, 1 },
    { 0x2a02, 0x04, 1 },
    { 0x3000, 0x04, 1 },
    { 0x3001, 0x04, 1 },
    { 0x3002, 0x04, 1 },
    { 0x3003, 0x04, 1 },
    { 0x3004, 0x04, 1 },
    { 0x3005, 0x04, 1 },
    { 0x4000, 0x04, 1 },
    { 0x4001, 0x04, 1 },
    { 0x4002, 0x04, 1 },
    { 0x4003, 0x04, 1 },
    { 0x4004, 0x04, 1 },
    { 0x4005, 0x04, 1 },
    { 0x4006, 0x04, 1 },
    { 0x4007, 0x04, 1 },
    { 0x8005, 0x13, 4 },
    { 0x8009, 0x04, 1 },
    { 0x8010, 0x04, 1 },
    { 0x8011, 0x04, 1 },
    { 0x8012, 0x04, 1 },
    { 0x8024, 0x04, 1 },
    { 0x802e, 0x04, 1 },
    { 0x8037, 0x04, 1 },
    { 0x8038, 0x03, 1 },
    { 0x8039, 0x03, 1 },
    { 0x8094, 0x04, 1 },
    { 0x809d, 0x04, 1 },
    { 0x809e, 0x04, 1 },
    { 0x809f, 0x04, 1 },
    { 0x80a0, 0x04, 1 },
    { 0x80aa, 0x03, 1 },
    { 0x80ab, 0x04, 1 },
    { 0x80ac, 0x04, 1 },
    { 0x80b1, 0x03, 16 },
    { 0x80b2, 0x04, 1 },
    { 0x80b4, 0x03, 1 },
    { 0x80b5, 0x03, 1 },
    { 0x80b6, 0x03, 1 },
    { 0x80b7, 0x03, 1 },
    { 0x80b8, 0x03, 1 },
    { 0x80b9, 0x03, 1 },
    { 0x80ba, 0x03, 1 },
    { 0x80bb, 0x03, 1 },
    { 0x80bc, 0x04, 1 },
    { 0x80d0, 0x04, 1 },
    { 0x80d1, 0x04, 1 },
    { 0x80d2, 0x04, 1 },
    { 0x8126, 0x03, 1 },
    { 0x8127, 0x03, 1 },
    { 0x8128, 0x03, 1 },
    { 0x8129, 0x03, 3 },
    { 0x812b, 0x04, 1 },
    { 0x8148, 0x00, 1 },
    { 0x8149, 0x04, 1 },
    { 0x814a, 0x03, 3 },
    { 0x814b, 0x03, 3 },
    { 0x817d, 0x04, 1 },
    { 0x817e, 0x03, 4 },
    { 0x8181, 0x04, 1 },
    { 0x8183, 0x04, 1 },
    { 0x8196, 0x04, 1 },
    { 0x8198, 0x04, 1 },
    { 0x8199, 0x03, 4 },
    { 0x81c4, 0x04, 1 },
    { 0x81c7, 0x04, 1 },
};

static void test_query_inventory(void)
{
    size_t count;
    unsigned types[256] = { 0 };
    const SGIKonaGLQueryInfo *table = sgi_kona_gl_query_table(&count);

    g_assert_cmpuint(count, ==, 159);
    for (unsigned i = 0; i < ARRAY_SIZE(expected_queries); i++) {
        const SGIKonaGLQueryInfo *want = &expected_queries[i];
        const SGIKonaGLQueryInfo *got = sgi_kona_gl_query_lookup(want->pname);

        g_assert_nonnull(got);
        g_assert_cmphex(got->type, ==, want->type);
        g_assert_cmpuint(got->count, ==, want->count);
        types[got->type]++;
    }
    for (unsigned i = 0; i < count; i++) {
        for (unsigned j = i + 1; j < count; j++) {
            g_assert_cmphex(table[i].pname, !=, table[j].pname);
        }
    }
    g_assert_cmpuint(types[0x04], ==, 120);
    g_assert_cmpuint(types[0x03], ==, 30);
    g_assert_cmpuint(types[0x13], ==, 6);
    g_assert_cmpuint(types[0x12], ==, 2);
    g_assert_cmpuint(types[0x00], ==, 1);
    g_assert_null(sgi_kona_gl_query_lookup(0xd33));
}

static void test_query_native_bytes(void)
{
    static const uint64_t doubles[] = {
        UINT64_C(0x3fd0000000000001), UINT64_C(0x3fe0000000000001),
    };
    static const uint8_t expected[] = {
        0x3f, 0xd0, 0, 0, 0, 0, 0, 1,
        0x3f, 0xe0, 0, 0, 0, 0, 0, 1,
    };
    uint8_t out[18];
    size_t length;
    const SGIKonaGLQueryInfo *info = sgi_kona_gl_query_lookup(0xb70);

    memset(out, 0xa5, sizeof(out));
    g_assert_true(sgi_kona_gl_query_serialize(info, doubles, 2,
                                             out + 1, 16, &length));
    g_assert_cmpuint(length, ==, 16);
    g_assert_cmpmem(out + 1, 16, expected, sizeof(expected));
    g_assert_cmphex(out[0], ==, 0xa5);
    g_assert_cmphex(out[17], ==, 0xa5);
    memset(out, 0xa5, sizeof(out));
    g_assert_false(sgi_kona_gl_query_serialize(info, doubles, 2,
                                              out, 15, &length));
    for (unsigned i = 0; i < sizeof(out); i++) {
        g_assert_cmphex(out[i], ==, 0xa5);
    }
    g_assert_false(sgi_kona_gl_query_serialize(info, doubles, 1,
                                              out, sizeof(out), &length));
    info = sgi_kona_gl_query_lookup(0x8148);
    g_assert_true(sgi_kona_gl_query_serialize(info, (uint64_t[]){ 1 }, 1,
                                             out + 1, 1, &length));
    g_assert_cmpuint(length, ==, 1);
    g_assert_cmphex(out[0], ==, 0xa5);
    g_assert_cmphex(out[1], ==, 1);
    g_assert_cmphex(out[2], ==, 0xa5);
}

static void test_query_word_bytes(void)
{
    static const uint64_t words[] = {
        0x3f000001, 0xbf800002, 0x00000003, 0x7fffffff,
    };
    static const uint8_t expected[] = {
        0x3f, 0, 0, 1, 0xbf, 0x80, 0, 2,
        0, 0, 0, 3, 0x7f, 0xff, 0xff, 0xff,
    };
    static const uint32_t names[] = { 0xb53, 0xba2 };
    uint8_t out[18];
    size_t length;

    for (unsigned k = 0; k < ARRAY_SIZE(names); k++) {
        const SGIKonaGLQueryInfo *info = sgi_kona_gl_query_lookup(names[k]);

        memset(out, 0xa5, sizeof(out));
        g_assert_true(sgi_kona_gl_query_serialize(info, words, 4,
                                                 out + 1, 16, &length));
        g_assert_cmpuint(length, ==, 16);
        g_assert_cmpmem(out + 1, 16, expected, sizeof(expected));
        g_assert_cmphex(out[0], ==, 0xa5);
        g_assert_cmphex(out[17], ==, 0xa5);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-kona-gl-decode/table", test_table_is_the_subset);
    g_test_add_func("/sgi-kona-gl-decode/subset", test_each_subset_selector);
    g_test_add_func("/sgi-kona-gl-decode/swap-finish",
                    test_swap_and_finish_share_a_selector);
    g_test_add_func("/sgi-kona-gl-decode/unknown", test_unknown_selector);
    g_test_add_func("/sgi-kona-gl-decode/truncated", test_truncated_packet);
    g_test_add_func("/sgi-kona-gl-decode/length-mismatch",
                    test_length_mismatch);
    g_test_add_func("/sgi-kona-gl-decode/unexpected-argument",
                    test_unexpected_literal_argument);
    g_test_add_func("/sgi-kona-gl-decode/stream", test_multi_command_stream);
    g_test_add_func("/sgi-kona-gl-decode/result-names", test_result_names);
    g_test_add_func("/sgi-kona-gl-decode/query-inventory",
                    test_query_inventory);
    g_test_add_func("/sgi-kona-gl-decode/query-native-bytes",
                    test_query_native_bytes);
    g_test_add_func("/sgi-kona-gl-decode/query-word-bytes",
                    test_query_word_bytes);
    return g_test_run();
}
