/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Expected OpenGL doorbell layouts are listed independently of the selector
 * table. A changed argument count or layout status therefore fails here.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/bswap.h"
#include "hw/misc/sgi-kona-gl-decode.h"

typedef struct Expected {
    uint32_t selector;
    SGIKonaGLLayoutEvidence evidence;
    unsigned argc;
    uint32_t arg0;      /* a value the decoder must accept for argument 0 */
} Expected;

static const Expected subset[] = {
    { 0x72, SGI_KONA_GL_LAYOUT_ESTABLISHED, 2, 0x40723c00 },
    { 0x73, SGI_KONA_GL_LAYOUT_ESTABLISHED, 1, 0x4391e000 },
    { 0x74, SGI_KONA_GL_LAYOUT_ESTABLISHED, 1, 291 },
    { 0x75, SGI_KONA_GL_LAYOUT_ESTABLISHED, 1, 0x12000000 },
    { 0x34, SGI_KONA_GL_LAYOUT_ESTABLISHED, 4, 0xc71 },
    { 0x35, SGI_KONA_GL_LAYOUT_ESTABLISHED, 4, 0xc71 },
    { 0x36, SGI_KONA_GL_LAYOUT_ESTABLISHED, 4, 0xc71 },
    { 0x17b, SGI_KONA_GL_LAYOUT_ESTABLISHED, 14, 0 },
    { 0x17c, SGI_KONA_GL_LAYOUT_ESTABLISHED, 4, 0 },
    { 0x179, SGI_KONA_GL_LAYOUT_ESTABLISHED, 22, 0 },
    { 0x17d, SGI_KONA_GL_LAYOUT_ESTABLISHED, 6, 0 },
    { 0x0,   SGI_KONA_GL_LAYOUT_UNRESOLVED,  0, 0 },     /* glBegin */
    { 0x1a,  SGI_KONA_GL_LAYOUT_INFERRED,    1, 0x4100 }, /* glClear */
    { 0x1b,  SGI_KONA_GL_LAYOUT_INFERRED,    4, 0 },     /* glClearAccum */
    { 0x1c,  SGI_KONA_GL_LAYOUT_INFERRED,    1, 0 },     /* glClearIndex */
    { 0x1d,  SGI_KONA_GL_LAYOUT_INFERRED,    4, 0x3f800000 },
    { 0x1e,  SGI_KONA_GL_LAYOUT_INFERRED,    1, 0 },     /* glClearStencil */
    { 0x1f,  SGI_KONA_GL_LAYOUT_UNRESOLVED,  0, 0 },     /* glClearDepth */
    { 0x38,  SGI_KONA_GL_LAYOUT_ESTABLISHED, 7, 0 },     /* glCopyPixels */
    { 0x39,  SGI_KONA_GL_LAYOUT_ESTABLISHED, 7, 0 },     /* glReadPixels */
    { 0x71,  SGI_KONA_GL_LAYOUT_ESTABLISHED, 1, 0 },     /* glEnd */
    { 0x8c,  SGI_KONA_GL_LAYOUT_INFERRED,    2, 0 },     /* glVertex2f */
    { 0x90,  SGI_KONA_GL_LAYOUT_INFERRED,    3, 0 },     /* glVertex3f */
    { 0x110, SGI_KONA_GL_LAYOUT_ESTABLISHED, 4, 0 },
    { 0x115, SGI_KONA_GL_LAYOUT_ESTABLISHED, 4, 0 },
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

static void test_copy_pixels_framing(void)
{
    const uint32_t stream[] = {
        0x38, 0x71, 0xc6, 0x1a, 0x1d, 0x90, 0x1800, 0x80000000,
        0xc6, 1,
    };
    SGIKonaGLCommand cmd;

    for (size_t n = 1; n < 8; n++) {
        g_assert_cmpint(sgi_kona_gl_decode(stream, n, 0, &cmd), ==,
                        SGI_KONA_GL_DECODE_TRUNCATED);
        g_assert_cmpuint(cmd.length, ==, 8);
    }
    g_assert_cmpint(sgi_kona_gl_decode(stream, ARRAY_SIZE(stream), 0, &cmd),
                    ==, SGI_KONA_GL_DECODE_OK);
    g_assert_cmpuint(cmd.length, ==, 8);
    g_assert_cmpuint(cmd.argc, ==, 7);
    for (size_t i = 0; i < cmd.argc; i++) {
        g_assert_cmphex(cmd.args[i], ==, stream[i + 1]);
    }
    g_assert_cmpint(sgi_kona_gl_decode(stream, ARRAY_SIZE(stream), 7, &cmd),
                    ==, SGI_KONA_GL_DECODE_LENGTH_MISMATCH);
    g_assert_cmpint(sgi_kona_gl_decode(stream + 8, 2, 0, &cmd), ==,
                    SGI_KONA_GL_DECODE_OK);
    g_assert_cmphex(cmd.selector, ==, 0xc6);
    g_assert_cmphex(cmd.args[0], ==, 1);
}

static void test_copy_query_framing(void)
{
    const uint32_t selectors[] = { 0x110, 0x115 };
    uint32_t stream[] = { 0, 0x71, 0xc6, 0x1a, 0x1d, 0xc6, 1 };
    SGIKonaGLCommand cmd;

    for (size_t i = 0; i < ARRAY_SIZE(selectors); i++) {
        stream[0] = selectors[i];
        for (size_t n = 1; n < 5; n++) {
            g_assert_cmpint(sgi_kona_gl_decode(stream, n, 0, &cmd), ==,
                            SGI_KONA_GL_DECODE_TRUNCATED);
            g_assert_cmpuint(cmd.length, ==, 5);
        }
        g_assert_cmpint(sgi_kona_gl_decode(stream, ARRAY_SIZE(stream), 0,
                                          &cmd), ==, SGI_KONA_GL_DECODE_OK);
        g_assert_cmpuint(cmd.length, ==, 5);
        g_assert_cmpuint(cmd.argc, ==, 4);
        for (size_t k = 0; k < cmd.argc; k++) {
            g_assert_cmphex(cmd.args[k], ==, stream[k + 1]);
        }
        g_assert_cmpint(sgi_kona_gl_decode(stream, ARRAY_SIZE(stream), 6,
                                          &cmd), ==,
                        SGI_KONA_GL_DECODE_LENGTH_MISMATCH);
        g_assert_cmpint(sgi_kona_gl_decode(stream + 5, 2, 0, &cmd), ==,
                        SGI_KONA_GL_DECODE_OK);
        g_assert_cmphex(cmd.selector, ==, 0xc6);
        g_assert_cmphex(cmd.args[0], ==, 1);
    }
}

static void test_read_pixels_framing(void)
{
    const uint32_t stream[] = {
        0x39, 0x71, 0xc6, 0x1a, 0x1d, 0x110, 0x115, 0x17d, 0xc6, 1,
    };
    SGIKonaGLCommand cmd;

    for (size_t n = 1; n < 8; n++) {
        g_assert_cmpint(sgi_kona_gl_decode(stream, n, 0, &cmd), ==,
                        SGI_KONA_GL_DECODE_TRUNCATED);
        g_assert_cmpuint(cmd.length, ==, 8);
    }
    g_assert_cmpint(sgi_kona_gl_decode(stream, ARRAY_SIZE(stream), 0, &cmd), ==,
                    SGI_KONA_GL_DECODE_OK);
    g_assert_cmpuint(cmd.length, ==, 8);
    g_assert_cmpuint(cmd.argc, ==, 7);
    for (size_t k = 0; k < cmd.argc; k++) {
        g_assert_cmphex(cmd.args[k], ==, stream[k + 1]);
    }
    g_assert_cmpint(sgi_kona_gl_decode(stream, ARRAY_SIZE(stream), 7, &cmd), ==,
                    SGI_KONA_GL_DECODE_LENGTH_MISMATCH);
    g_assert_cmpint(sgi_kona_gl_decode(stream + 8, 2, 0, &cmd), ==,
                    SGI_KONA_GL_DECODE_OK);
    g_assert_cmphex(cmd.selector, ==, 0xc6);
    g_assert_cmphex(cmd.args[0], ==, 1);
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

static void test_texture_transform(void)
{
    double scale[4], bias[4];

    /* Default records carry zero packed data but disable every channel. */
    g_assert_true(sgi_kona_gl_texture_transform(0, 0, 0, scale, bias));
    for (unsigned k = 0; k < 4; k++) {
        g_assert_cmpfloat(scale[k], ==, 1.0);
        g_assert_cmpfloat(bias[k], ==, 0.0);
    }
    g_assert_true(sgi_kona_gl_texture_transform(0xf, 0xff804000,
                                               0x004080ff, scale, bias));
    g_assert_cmpfloat(scale[0], ==, 1.0);
    g_assert_cmpfloat(scale[1], ==, 128.0 / 255.0);
    g_assert_cmpfloat(scale[2], ==, 64.0 / 255.0);
    g_assert_cmpfloat(scale[3], ==, 0.0);
    g_assert_cmpfloat(bias[0], ==, 0.0);
    g_assert_cmpfloat(bias[1], ==, 64.0 / 255.0);
    g_assert_cmpfloat(bias[2], ==, 128.0 / 255.0);
    g_assert_cmpfloat(bias[3], ==, 1.0);

    /* Disabled channels ignore arbitrary bytes in both packed words. */
    g_assert_true(sgi_kona_gl_texture_transform(0xa, 0x80ff40ff,
                                               0x40ff80ff, scale, bias));
    g_assert_cmpfloat(scale[0], ==, 128.0 / 255.0);
    g_assert_cmpfloat(bias[0], ==, 64.0 / 255.0);
    g_assert_cmpfloat(scale[2], ==, 64.0 / 255.0);
    g_assert_cmpfloat(bias[2], ==, 128.0 / 255.0);
    for (unsigned k = 1; k < 4; k += 2) {
        g_assert_cmpfloat(scale[k], ==, 1.0);
        g_assert_cmpfloat(bias[k], ==, 0.0);
    }
    g_assert_false(sgi_kona_gl_texture_transform(0x10, 0, 0, scale, bias));
    g_assert_cmpfloat(scale[0], ==, 128.0 / 255.0);
    g_assert_cmpfloat(bias[0], ==, 64.0 / 255.0);
}

static void test_texture_sampler(void)
{
    static const uint32_t filters[8] = {
        SGI_GL_NEAREST, SGI_GL_LINEAR, 0,
        SGI_GL_NEAREST_MIPMAP_NEAREST, SGI_GL_NEAREST_MIPMAP_LINEAR,
        SGI_GL_LINEAR_MIPMAP_NEAREST, SGI_GL_LINEAR_MIPMAP_LINEAR, 0,
    };
    uint32_t bind[5] = { 7, 0, 0, 0, 1 };
    uint32_t record[22] = { 0, 0x00101010, 256u << 10 };
    SGIKonaTextureParams original = {
        .base_level = 2, .max_level = 5,
        .min_lod = -1000, .max_lod = 1000,
        .border_color = { 0.25, 0.5, 0.75, 1 },
        .post_scale = { 0.5, 1, 1, 1 }, .post_bias = { 0, 0, 0.25, 0 },
    };
    SGIKonaTextureParams p;

    for (unsigned min = 0; min < 8; min++) {
        for (unsigned mag = 0; mag < 8; mag++) {
            bool supported = filters[min] &&
                             (mag < 2 || (mag >= 3 && mag <= 5));

            p = original;
            p.base_level = 0;
            SGIKonaTextureParams before = p;

            bind[2] = (min << 2) | (mag << 5) | 3;
            g_assert_cmpint(sgi_kona_gl_texture_sampler(bind, record, &p),
                            ==, supported);
            if (!supported) {
                g_assert_cmpmem(&p, sizeof(p), &before, sizeof(before));
                continue;
            }
            g_assert_cmphex(p.min_filter, ==, filters[min]);
            g_assert_cmphex(p.mag_filter, ==,
                            mag == 5 ? SGI_GL_LINEAR_SHARPEN_SGIS :
                            mag >= 3 ? SGI_GL_LINEAR_DETAIL_SGIS :
                            mag ? SGI_GL_LINEAR : SGI_GL_NEAREST);
            g_assert_cmphex(p.wrap_s, ==, SGI_GL_REPEAT);
            g_assert_cmphex(p.wrap_t, ==, SGI_GL_REPEAT);
            g_assert_cmpuint(p.base_level, ==, 0);
            g_assert_cmpuint(p.max_level, ==, 5);
            g_assert_cmpfloat(p.post_scale[0], ==, 0.5);
            g_assert_cmpfloat(p.post_bias[2], ==, 0.25);
            g_assert_cmpfloat(p.border_color[1], ==, 0.5);
            g_assert_cmpfloat(p.max_lod, ==, 3);
        }
    }
    bind[2] = 4 << 2;
    record[2] = 96 | (288 << 10);
    p = original;
    p.base_level = 0;
    g_assert_true(sgi_kona_gl_texture_sampler(bind, record, &p));
    g_assert_cmpfloat(p.min_lod, ==, 0.5);
    g_assert_cmpfloat(p.max_lod, ==, 3.5);
    for (unsigned axis = 12; axis <= 14; axis++) {
        p = original;
        bind[3] = 1 << axis;
        g_assert_false(sgi_kona_gl_texture_sampler(bind, record, &p));
        g_assert_cmpmem(&p, sizeof(p), &original, sizeof(original));
    }
    bind[3] = 0;
    for (unsigned mode = 1; mode <= 3; mode++) {
        p = original;
        p.base_level = 0;
        SGIKonaTextureParams before = p;

        bind[2] = (4 << 2) | (mode << 10);
        g_assert_false(sgi_kona_gl_texture_sampler(bind, record, &p));
        g_assert_cmpmem(&p, sizeof(p), &before, sizeof(before));
    }
    bind[2] = 4 << 2;
    for (unsigned lo = 1; lo < 64; lo++) {
        p = original;
        p.base_level = 0;
        record[2] = lo | (256 << 10);
        g_assert_true(sgi_kona_gl_texture_sampler(bind, record, &p));
        g_assert_cmpfloat_with_epsilon(p.min_lod, log2(lo / 64.0),
                                         0.000000000001);
        g_assert_cmpfloat(p.max_lod, ==, 3);
    }
    record[2] = 320 | (256 << 10);
    g_assert_false(sgi_kona_gl_texture_sampler(bind, record, &p));
    record[2] = 256 << 10;
    for (unsigned axis = 0; axis <= 8; axis += 8) {
        record[1] = 0x00101010 & ~(0xffu << axis);
        g_assert_false(sgi_kona_gl_texture_sampler(bind, record, &p));
    }
    record[1] = 0x00ff8020;
    p = original;
    p.base_level = 0;
    g_assert_true(sgi_kona_gl_texture_sampler(bind, record, &p));
    g_assert_cmpfloat(p.lod_scale_s, ==, 2);
    g_assert_cmpfloat(p.lod_scale_t, ==, 8);
    record[1] = 0x0000ff01;
    g_assert_true(sgi_kona_gl_texture_sampler(bind, record, &p));
    g_assert_cmpfloat(p.lod_scale_s, ==, 1.0 / 16);
    g_assert_cmpfloat(p.lod_scale_t, ==, 255.0 / 16);
    p = original;
    g_assert_false(sgi_kona_gl_texture_sampler(bind, record, &p));
    g_assert_cmpmem(&p, sizeof(p), &original, sizeof(original));
}

static void test_texture_sharpen_sampler(void)
{
    const uint32_t modes[] = {
        SGI_GL_LINEAR_SHARPEN_SGIS, SGI_GL_LINEAR_SHARPEN_COLOR_SGIS,
        SGI_GL_LINEAR_SHARPEN_ALPHA_SGIS,
    };
    const uint32_t channels[] = { 0, 0x04000000, 0x08000000 };
    uint32_t bind[5] = { 7, 4095, (3 << 2) | (5 << 5), 0, 1 };
    uint32_t record[22] = { 0, 0x00101010, 256 << 10 };
    SGIKonaTextureParams original = { .max_level = 5 };

    for (unsigned k = 0; k < ARRAY_SIZE(modes); k++) {
        SGIKonaTextureParams p = original;

        bind[2] = (3 << 2) | (5 << 5) | channels[k];
        g_assert_true(sgi_kona_gl_texture_sampler(bind, record, &p));
        g_assert_cmphex(p.mag_filter, ==, modes[k]);
        g_assert_cmpuint(p.filter_curve_slot, ==, 4095);
        g_assert_cmpfloat(p.min_lod, ==, -1000);
        g_assert_cmpfloat(p.max_lod, ==, 3);
    }
    for (unsigned min = 0; min <= 6; min++) {
        SGIKonaTextureParams p = original;

        bind[2] = (min << 2) | (5 << 5);
        g_assert_cmpint(sgi_kona_gl_texture_sampler(bind, record, &p),
                        ==, min != 2);
    }
    for (unsigned lo = 1; lo < 64; lo++) {
        SGIKonaTextureParams p = original;

        record[2] = lo | (256 << 10);
        g_assert_true(sgi_kona_gl_texture_sampler(bind, record, &p));
        g_assert_cmpfloat_with_epsilon(p.min_lod, log2(lo / 64.0),
                                         0.000000000001);
    }
    for (unsigned hi = 0; hi < 64; hi++) {
        SGIKonaTextureParams p = original;

        record[2] = hi << 10;
        g_assert_true(sgi_kona_gl_texture_sampler(bind, record, &p));
        g_assert_cmpfloat_with_epsilon(p.max_lod,
                                         hi ? log2(hi / 64.0) : -1000,
                                         0.000000000001);
    }
    record[2] = 96 | (256 << 10);
    SGIKonaTextureParams p = original;
    g_assert_true(sgi_kona_gl_texture_sampler(bind, record, &p));
    g_assert_cmpfloat(p.min_lod, ==, 0.5);
    record[2] = 256 << 10;
    bind[2] |= 0x0c000000;
    p = original;
    g_assert_false(sgi_kona_gl_texture_sampler(bind, record, &p));
    bind[2] &= ~0x0c000000;
    p.base_level = 1;
    g_assert_false(sgi_kona_gl_texture_sampler(bind, record, &p));
}

static void test_texture_detail_sampler(void)
{
    const uint32_t modes[] = {
        SGI_GL_LINEAR_DETAIL_SGIS, SGI_GL_LINEAR_DETAIL_COLOR_SGIS,
        SGI_GL_LINEAR_DETAIL_ALPHA_SGIS,
    };
    const uint32_t channels[] = { 0, 0x04000000, 0x08000000 };
    uint32_t bind[5] = { 7, 4095, 0, 0, 1 };
    uint32_t record[22] = { 0, 0x00101010, 256 << 10 };
    SGIKonaTextureParams original = { .max_level = 5 };

    for (unsigned mag = 3; mag <= 4; mag++) {
        for (unsigned channel = 0; channel < ARRAY_SIZE(modes); channel++) {
            for (unsigned level = 0; level <= 4; level++) {
                SGIKonaTextureParams p = original;

                bind[2] = (3 << 2) | (mag << 5) | channels[channel] |
                          (level << 21);
                g_assert_true(sgi_kona_gl_texture_sampler(bind, record, &p));
                g_assert_cmphex(p.mag_filter, ==, modes[channel]);
                g_assert_cmphex(p.detail_mode, ==,
                                mag == 3 ? SGI_GL_ADD : SGI_GL_MODULATE);
                g_assert_cmpint(p.detail_level, ==, -(int)level);
                g_assert_cmpuint(p.filter_curve_slot, ==, 4095);
            }
        }
    }
    for (unsigned level = 5; level <= 7; level++) {
        SGIKonaTextureParams p = original;

        bind[2] = (3 << 2) | (3 << 5) | (level << 21);
        g_assert_false(sgi_kona_gl_texture_sampler(bind, record, &p));
        g_assert_cmpmem(&p, sizeof(p), &original, sizeof(original));
    }
    bind[2] = (3 << 2) | (3 << 5);
    record[2] = 32 | (256 << 10);
    SGIKonaTextureParams p = original;
    g_assert_true(sgi_kona_gl_texture_sampler(bind, record, &p));
    g_assert_cmpfloat(p.min_lod, ==, -1);
    record[2] = 32 << 10;
    g_assert_true(sgi_kona_gl_texture_sampler(bind, record, &p));
    g_assert_cmpfloat(p.max_lod, ==, -1);
    record[2] = 256 << 10;
    bind[2] |= 0x0c000000;
    g_assert_false(sgi_kona_gl_texture_sampler(bind, record, &p));
    bind[2] &= ~0x0c000000;
    p.base_level = 1;
    g_assert_false(sgi_kona_gl_texture_sampler(bind, record, &p));
}

static void test_texture_native_layout(void)
{
    static const struct {
        uint32_t storage, layout;
    } families[] = {
        { 0xd, 0x2200 }, { 0xe, 0x2200 }, { 8, 0x2200 },
        { 0x10, 0x2200 }, { 0x14, 0x2200 }, { 0x30, 0x2181 },
    };
    uint32_t h[20] = { [0] = SGI_GL_TEXTURE_2D };

    for (unsigned k = 0; k < ARRAY_SIZE(families); k++) {
        for (unsigned bank = 0; bank < 2; bank++) {
            for (unsigned dma = 0; dma < 2; dma++) {
                h[6] = dma;
                h[9] = families[k].storage;
                h[17] = families[k].layout | (bank << 3) | (dma << 2);
                g_assert_true(sgi_kona_gl_texture_native_layout(h));
                h[17] ^= 4;
                g_assert_false(sgi_kona_gl_texture_native_layout(h));
                h[17] ^= 5;
                g_assert_false(sgi_kona_gl_texture_native_layout(h));
                h[17] = (families[k].storage == 0x30 ? 0x2201 : 0x2180) |
                        (bank << 3) | (dma << 2);
                g_assert_false(sgi_kona_gl_texture_native_layout(h));
            }
        }
    }
    h[0] = SGI_GL_TEXTURE_1D;
    h[6] = 0;
    h[9] = 0xd;
    h[17] = 0x2200;
    g_assert_true(sgi_kona_gl_texture_native_layout(h));
    h[0] = SGI_GL_TEXTURE_3D;
    g_assert_false(sgi_kona_gl_texture_native_layout(h));
    h[0] = SGI_GL_TEXTURE_2D;
    h[17] = 0x2240;
    g_assert_false(sgi_kona_gl_texture_native_layout(h));
    h[17] = 0x2200;
    h[9] = 0xff;
    g_assert_false(sgi_kona_gl_texture_native_layout(h));
    h[9] = 0xd;
    h[6] = 2;
    g_assert_false(sgi_kona_gl_texture_native_layout(h));
}

static void test_texture_comparison_flag(void)
{
    static const uint32_t targets[] = {
        0x10000000, 0x20000000, 0x30000000,
    };
    static const uint32_t refused[] = { 0, 2, UINT32_MAX };
    uint32_t bind[5] = { 0, 0, 0x24, 0, 1 };
    uint32_t record[22] = { 0, 0x00101010, 256 << 10 };
    SGIKonaTextureParams p, before = {
        .min_lod = -1000, .max_lod = 1000,
        .lod_scale_s = 9, .lod_scale_t = 7, .lod_scale_r = 5,
    };

    for (unsigned t = 0; t < ARRAY_SIZE(targets); t++) {
        bind[1] = targets[t];
        for (unsigned k = 0; k < ARRAY_SIZE(refused); k++) {
            bool accepted;

            bind[4] = refused[k];
            p = before;
            if (t == 0) {
                /* Normalizing unused axes must not erase comparison state. */
                bind[3] = 0x6000;
                accepted = sgi_kona_gl_texture_sampler_1d(bind, record, &p);
            } else if (t == 1) {
                bind[3] = 0;
                accepted = sgi_kona_gl_texture_sampler(bind, record, &p);
            } else {
                accepted = sgi_kona_gl_texture_volume_sampler(bind, record,
                                                               false, &p);
            }
            g_assert_false(accepted);
            g_assert_cmpmem(&p, sizeof(p), &before, sizeof(before));
        }
    }
}

static void test_texture_volume_layout(void)
{
    static const struct {
        uint32_t storage, layout;
    } families[] = {
        { 0xd, 0x11c0 }, { 0xe, 0x11c0 }, { 8, 0x11c0 },
        { 0x30, 0x1141 },
    };
    uint32_t h[20] = { [0] = SGI_GL_TEXTURE_3D };

    for (unsigned k = 0; k < ARRAY_SIZE(families); k++) {
        for (unsigned parity = 0; parity < 2; parity++) {
            for (unsigned dma = 0; dma < 2; dma++) {
                uint32_t extra = (parity << 3) | (dma << 2);

                h[6] = dma;
                h[9] = families[k].storage;
                h[17] = families[k].layout | extra;
                g_assert_true(sgi_kona_gl_texture_native_layout(h));
                h[17] ^= 4;
                g_assert_false(sgi_kona_gl_texture_native_layout(h));
                h[17] = (h[9] == 0x30 ? 0x11c1 : 0x1140) | extra;
                g_assert_false(sgi_kona_gl_texture_native_layout(h));
                h[17] = (h[9] == 0x30 ? 0x21c1 : 0x21c0) | extra;
                g_assert_false(sgi_kona_gl_texture_native_layout(h));
                h[17] = (h[9] == 0x30 ? 0x2181 : 0x2200) | extra;
                g_assert_false(sgi_kona_gl_texture_native_layout(h));
                h[17] = families[k].layout | extra;
                h[0] = SGI_GL_TEXTURE_2D;
                g_assert_false(sgi_kona_gl_texture_native_layout(h));
                h[0] = SGI_GL_TEXTURE_3D;
            }
        }
    }
}

static void test_texture_binding_layout(void)
{
    /* Luminance, alpha and depth storage, then the 16-bit colour codes. */
    static const uint32_t storages[] = {
        0xd, 0xe, 8, SGI_KONA_GL_STORAGE_RGB4, SGI_KONA_GL_STORAGE_RGB5, 0x30,
    };
    static const uint32_t targets[] = {
        SGI_GL_TEXTURE_1D, SGI_GL_TEXTURE_2D, SGI_GL_TEXTURE_3D,
    };

    for (unsigned k = 0; k < ARRAY_SIZE(storages); k++) {
        for (unsigned t = 0; t < ARRAY_SIZE(targets); t++) {
            bool colour = storages[k] == 0x30;
            uint32_t record = targets[t] == SGI_GL_TEXTURE_3D ?
                              (colour ? 0x22 : 0x23) : (colour ? 0x43 : 0x44);

            g_assert_true(sgi_kona_gl_texture_storage_binding_layout(
                storages[k], targets[t], record));
            g_assert_true(sgi_kona_gl_texture_storage_binding_layout(
                storages[k], targets[t], record | 0x12340000));
            g_assert_false(sgi_kona_gl_texture_storage_binding_layout(
                storages[k], targets[t], record ^ 1));
            g_assert_false(sgi_kona_gl_texture_storage_binding_layout(
                storages[k], targets[t], record ^ 0x60));
        }
    }
    g_assert_false(sgi_kona_gl_texture_storage_binding_layout(
        0x15, SGI_GL_TEXTURE_2D, 0x44));
    g_assert_false(sgi_kona_gl_texture_storage_binding_layout(
        0xd, 0xdead, 0x44));
}

static void test_texture_shadow_sampler(void)
{
    SGIKonaTextureParams p, before;
    uint32_t bind[5] = { 7, 0x2003f000, 0x34400, 0xf11, 0 };
    uint32_t record[22] = { 0x44, 0x1010, 256u << 10 };

    p = (SGIKonaTextureParams) {
        .max_level = 1000, .min_lod = -1000, .max_lod = 1000,
    };
    for (unsigned target = 1; target <= 2; target++) {
        for (unsigned compare = 1; compare <= 2; compare++) {
            for (unsigned ambient = 0; ambient <= 63; ambient++) {
                bind[1] = (target << 28) | (63 << 12);
                bind[2] = (0xd << 14) | (compare << 10) | 0x24;
                bind[3] = target == 1 ? 0xf01 : 0xf11;
                record[5] = ambient;
                g_assert_true(target == 1 ?
                    sgi_kona_gl_texture_sampler_1d(bind, record, &p) :
                    sgi_kona_gl_texture_sampler(bind, record, &p));
                g_assert_cmpuint(p.shadow_compare, ==, compare == 1 ?
                    SGI_GL_TEXTURE_GEQUAL_R_SGIX :
                    SGI_GL_TEXTURE_LEQUAL_R_SGIX);
                g_assert_cmpfloat(p.shadow_ambient, ==, ambient / 63.0);
            }
        }
    }
    before = p;
    for (unsigned bad = 0; bad < 8; bad++) {
        uint32_t b[5], r[22];

        memcpy(b, bind, sizeof(b));
        memcpy(r, record, sizeof(r));
        switch (bad) {
        case 0:
            b[4] = 1;
            break;
        case 1:
            b[2] |= 0xc00;
            break;
        case 2:
            b[3] &= ~0xf00;
            break;
        case 3:
            b[1] = 0x20003000;
            break;
        case 4:
            b[2] ^= 1 << 14;
            break;
        case 5:
            r[4] = 1;
            break;
        case 6:
            r[5] = 64;
            break;
        case 7:
            b[1] = 0x3003f000;
            break;
        }
        g_assert_false(sgi_kona_gl_texture_sampler(b, r, &p));
        g_assert_cmpmem(&p, sizeof(p), &before, sizeof(before));
    }
    /* Ordinary color bindings must clear the previous shadow state. */
    bind[1] = 0x20021000;
    bind[2] = (0x30 << 14) | 0x24;
    bind[3] = 0x11;
    bind[4] = 1;
    g_assert_true(sgi_kona_gl_texture_sampler(bind, record, &p));
    g_assert_cmpuint(p.shadow_compare, ==, 0);
    g_assert_cmpfloat(p.shadow_ambient, ==, 0);
}

static void test_texture_pixel_depth(void)
{
    static const struct {
        uint32_t type, source, expected;
        bool swap;
    } cases[] = {
        { 0, 0x80000000, 0 }, { 0, 0x7f000000, UINT32_MAX },
        { 0, 0, 0x01010101 }, { 1, 0x01000000, 0x01010101 },
        { 2, 0x80000000, 0 }, { 2, 0x7fff0000, UINT32_MAX },
        { 2, 0, 0x00010001 }, { 3, 0x80000000, 0x80008000 },
        { 3, 0x00800000, 0x80008000, true },
        { 4, 0x80000000, 0 }, { 4, 0x7fffffff, UINT32_MAX },
        { 4, 0, 1 }, { 5, 0x7fffffff, 0x7fffffff },
        { 5, 0x80000000, 0x80000000 },
        { 5, 0x78563412, 0x12345678, true },
        { 6, 0x3f000000, 0x80000000 },
        { 6, 0x0000003f, 0x80000000, true },
        { 6, 0x40000000, UINT32_MAX }, { 6, 0xbf800000, 0 },
    };
    uint32_t depth, source;

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        depth = 0xdeadbeef;
        g_assert_true(sgi_kona_gl_texture_pixel_depth(cases[i].type,
            &cases[i].source, cases[i].swap, 0x3f800000, 0, &depth));
        g_assert_cmphex(depth, ==, cases[i].expected);
    }
    source = 0x40000000; /* Two scales to one half before clamping. */
    g_assert_true(sgi_kona_gl_texture_pixel_depth(6, &source, false,
        0x3e800000, 0, &depth));
    g_assert_cmphex(depth, ==, 0x80000000);
    source = 0;
    g_assert_true(sgi_kona_gl_texture_pixel_depth(5, &source, false,
        0x3f800000, 0x3e800000, &depth));
    g_assert_cmphex(depth, ==, 0x40000000);
    source = 0x7fc00000;
    depth = 0xdeadbeef;
    g_assert_false(sgi_kona_gl_texture_pixel_depth(6, &source, false,
        0x3f800000, 0, &depth));
    source = 0;
    g_assert_false(sgi_kona_gl_texture_pixel_depth(7, &source, false,
        0x3f800000, 0, &depth));
    g_assert_false(sgi_kona_gl_texture_pixel_depth(5, &source, false,
        0x7f800000, 0, &depth));
    g_assert_false(sgi_kona_gl_texture_pixel_depth(5, &source, false,
        0x3f800000, 0x7fc00000, &depth));
    g_assert_cmphex(depth, ==, 0xdeadbeef);
}

static void test_texture_sampler_1d(void)
{
    uint32_t bind[5] = { 0, 0x10000000, 0x24, 1, 1 };
    uint32_t record[22] = { 0, 0x1010, 256 << 10 };
    SGIKonaTextureParams p = { .min_lod = -1000, .max_lod = 1000 };
    SGIKonaTextureParams original;

    g_assert_true(sgi_kona_gl_texture_sampler_1d(bind, record, &p));
    g_assert_cmpuint(p.min_filter, ==, SGI_GL_LINEAR);
    g_assert_cmpfloat(p.lod_scale_t, ==, 1);
    g_assert_cmpfloat(p.lod_scale_r, ==, 1);
    original = p;
    for (unsigned scale = 0; scale <= 255; scale += 255) {
        for (unsigned wrap = 0; wrap < 4; wrap++) {
            uint32_t unused_bind[5], unused_record[22];

            memcpy(unused_bind, bind, sizeof(unused_bind));
            memcpy(unused_record, record, sizeof(unused_record));
            unused_bind[3] |= wrap << 13;
            unused_record[1] = (scale << 8) | 16;
            p.lod_scale_t = 9;
            p.lod_scale_r = 7;
            g_assert_true(sgi_kona_gl_texture_sampler_1d(unused_bind,
                                                         unused_record, &p));
            g_assert_cmpmem(&p, sizeof(p), &original, sizeof(original));
        }
    }
    record[1] = 0x1000;
    g_assert_false(sgi_kona_gl_texture_sampler_1d(bind, record, &p));
    g_assert_cmpmem(&p, sizeof(p), &original, sizeof(original));
    record[1] = 0x1010;
    for (unsigned k = 0; k < 6; k++) {
        uint32_t bad[5];

        memcpy(bad, bind, sizeof(bad));
        switch (k) {
        case 0:
            bad[1] = 0x20000000;
            break;
        case 1:
            bad[3] |= 0x10;
            break;
        case 2:
            bad[3] |= 0x100;
            break;
        case 3:
            bad[3] |= 0x1000;
            break;
        case 4:
            bad[2] = 3 << 5;
            break;
        case 5:
            bad[2] |= 0xc00;
            break;
        }
        g_assert_false(sgi_kona_gl_texture_sampler_1d(bad, record, &p));
        g_assert_cmpmem(&p, sizeof(p), &original, sizeof(original));
    }
    p.base_level = 1;
    g_assert_false(sgi_kona_gl_texture_sampler_1d(bind, record, &p));
}

static void test_texture_volume_sampler(void)
{
    uint32_t bind[5] = { 7, 0x30000000, 0, 0x123, 1 };
    uint32_t record[14] = { 0, 0x00402010, 256 << 10 };
    SGIKonaTextureParams original = {
        .max_level = 1000, .min_lod = -1000, .max_lod = 1000,
        .wrap_r = SGI_GL_REPEAT, .lod_scale_r = 1,
    }, p;
    uint32_t width, height, depth;

    sgi_kona_gl_texture_volume_dimensions(bind, &width, &height, &depth);
    g_assert_cmpuint(width, ==, 8);
    g_assert_cmpuint(height, ==, 4);
    g_assert_cmpuint(depth, ==, 2);
    for (unsigned min = 0; min < 8; min++) {
        for (unsigned mag = 0; mag < 8; mag++) {
            bool supported = min >= 3 && min <= 6 && mag < 2;

            bind[2] = (min << 2) | (mag << 5);
            p = original;
            g_assert_cmpint(sgi_kona_gl_texture_volume_sampler(bind, record,
                                                               true, &p),
                            ==, supported);
            if (!supported) {
                g_assert_cmpmem(&p, sizeof(p), &original, sizeof(original));
            } else {
                g_assert_cmpfloat(p.lod_scale_s, ==, 1);
                g_assert_cmpfloat(p.lod_scale_t, ==, 2);
                g_assert_cmpfloat(p.lod_scale_r, ==, 4);
                g_assert_cmphex(p.wrap_r, ==, SGI_GL_REPEAT);
                g_assert_cmpfloat(p.max_lod, ==, 3);
            }
        }
    }
    for (unsigned min = 0; min < 2; min++) {
        for (unsigned mag = 0; mag < 2; mag++) {
            bind[2] = (min << 2) | (mag << 5);
            p = original;
            g_assert_cmpint(sgi_kona_gl_texture_volume_sampler(bind, record,
                                                               false, &p),
                            ==, min == mag);
        }
    }
    bind[2] = 3 << 2;
    for (unsigned bit = 12; bit <= 14; bit++) {
        bind[3] = 1 << bit;
        p = original;
        g_assert_false(sgi_kona_gl_texture_volume_sampler(bind, record,
                                                          true, &p));
    }
    bind[3] = 0;
    record[1] &= 0xffff;
    p = original;
    g_assert_false(sgi_kona_gl_texture_volume_sampler(bind, record, true, &p));
    record[1] = 0x00101010;
    bind[1] = 0x20000000;
    g_assert_false(sgi_kona_gl_texture_volume_sampler(bind, record, true, &p));
    bind[1] = 0x30000000;
    p.base_level = 1;
    g_assert_false(sgi_kona_gl_texture_volume_sampler(bind, record, true, &p));
}

static void test_texture_negative_lod(void)
{
    uint32_t bind[5] = { [4] = 1 };
    uint32_t record[22] = { 0, 0x00101010 };
    SGIKonaTextureParams original = { .max_level = 1000 };

    for (unsigned min = 3; min <= 6; min++) {
        for (unsigned mag = 0; mag < 2; mag++) {
            bind[2] = (min << 2) | (mag << 5);
            for (unsigned lo = 0; lo < 64; lo++) {
                for (unsigned hi = 0; hi < 64; hi++) {
                    SGIKonaTextureParams p = original;

                    record[2] = lo | (hi << 10);
                    g_assert_cmpint(sgi_kona_gl_texture_sampler(bind,
                                                               record, &p),
                                    ==, lo <= hi);
                    if (lo > hi) {
                        g_assert_cmpmem(&p, sizeof(p), &original,
                                        sizeof(original));
                    } else {
                        g_assert_cmpfloat_with_epsilon(p.min_lod,
                            lo ? log2(lo / 64.0) : -1000, 0.000000000001);
                        g_assert_cmpfloat_with_epsilon(p.max_lod,
                            hi ? log2(hi / 64.0) : -1000, 0.000000000001);
                    }
                }
            }
        }
    }
    record[2] = 63 | (64 << 10);
    g_assert_true(sgi_kona_gl_texture_sampler(bind, record, &original));
    g_assert_cmpfloat_with_epsilon(original.min_lod,
                                     -0.0227200765000835, 0.000000000001);
    g_assert_cmpfloat(original.max_lod, ==, 0);
    record[2] = 64 | (63 << 10);
    g_assert_false(sgi_kona_gl_texture_sampler(bind, record, &original));
}

static void test_texture_dimensions(void)
{
    uint32_t bind[5] = { 0 }, width, height;

    for (unsigned s = 0; s < 16; s++) {
        for (unsigned t = 0; t < 16; t++) {
            bind[3] = 0xffff0000 | s | (t << 4);
            sgi_kona_gl_texture_dimensions(bind, &width, &height);
            g_assert_cmpuint(width, ==, 1u << s);
            g_assert_cmpuint(height, ==, 1u << t);
        }
    }
}

static void test_texture_pixel_conversion(void)
{
    const uint32_t byte_source[] = { 0x4080c0ff };
    const uint32_t float_source[] = {
        0x3e800000, 0x3f000000, 0x3f400000, 0x3f800000,
    };
    const uint32_t expected[] = {
        0x00000040, 0x4080c0ff, 0x4080c0ff, 0x404040ff, 0x40404080,
    };
    const uint32_t expected_float[] = {
        0x00000040, 0x4080bfff, 0x4080bfff, 0x404040ff, 0x40404080,
    };
    const uint32_t components[] = { 1, 3, 4, 1, 2 };
    const uint32_t saturated[] = {
        0xbf800000, 0x40000000, 0x00000000, 0x80000000,
    };
    uint32_t rgba;

    for (uint32_t format = 3; format <= 7; format++) {
        g_assert_cmpuint(sgi_kona_gl_texture_pixel_bytes(format, 1), ==,
                         components[format - 3]);
        g_assert_cmpuint(sgi_kona_gl_texture_pixel_bytes(format, 6), ==,
                         4 * components[format - 3]);
        g_assert_true(sgi_kona_gl_texture_pixel_rgba(format, 1, byte_source,
                                                     false, &rgba));
        g_assert_cmphex(rgba, ==, expected[format - 3]);
        g_assert_true(sgi_kona_gl_texture_pixel_rgba(format, 6, float_source,
                                                     false, &rgba));
        g_assert_cmphex(rgba, ==, expected_float[format - 3]);
        for (uint32_t type = 7; type <= 13; type++) {
            if ((format == 4 && type == 8) ||
                (format == 5 && type >= 9 && type <= 12)) {
                continue;
            }
            rgba = 0x12345678;
            g_assert_cmpuint(sgi_kona_gl_texture_pixel_bytes(format, type),
                             ==, 0);
            g_assert_false(sgi_kona_gl_texture_pixel_rgba(format, type,
                                                          float_source,
                                                          false, &rgba));
            g_assert_cmphex(rgba, ==, 0x12345678);
        }
        for (unsigned component = 0;
             component < components[format - 3]; component++) {
            const uint32_t invalid[] = {
                0x7fc00000, 0x7f800001, 0x7f800000, 0xff800000,
            };
            for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
                uint32_t source[4];

                memcpy(source, float_source, sizeof(source));
                source[component] = invalid[i];
                rgba = 0x12345678;
                g_assert_false(sgi_kona_gl_texture_pixel_rgba(format, 6,
                                                              source, false,
                                                              &rgba));
                g_assert_cmphex(rgba, ==, 0x12345678);
            }
        }
    }
    g_assert_true(sgi_kona_gl_texture_pixel_rgba(5, 6, saturated,
                                                false, &rgba));
    g_assert_cmphex(rgba, ==, 0x00ff0000);
    for (uint32_t format = 11; format <= 13; format++) {
        rgba = 0x12345678;
        g_assert_cmpuint(sgi_kona_gl_texture_pixel_bytes(format, 1), ==, 0);
        g_assert_cmpuint(sgi_kona_gl_texture_pixel_bytes(format, 6), ==, 0);
        g_assert_false(sgi_kona_gl_texture_pixel_rgba(format, 1,
                                                      byte_source, false,
                                                      &rgba));
        g_assert_cmphex(rgba, ==, 0x12345678);
    }
}

/*
 * The storage code each supported index uploads as: the library's records
 * give RGB4-class formats code 0x10 and RGB5 and RGB5_A1 code 0x14, both
 * in family zero, while RGB and RGBA keep 0x30; the record byte follows
 * the family, 0x44 (0x23 for a volume) for family zero and 0x43 (0x22)
 * for 0x30.
 */
static void test_texture_format_storage(void)
{
    static const struct {
        uint32_t index, storage;
    } cases[] = {
        { 0, 0xd }, { 1, 8 }, { 2, 0x30 }, { 17, 0xe }, { 25, 0x10 },
        { 29, 0x10 }, { 30, 0x14 }, { 31, 0x14 }, { 33, 0x30 }, { 34, 0x30 },
        { 63, 0xd },
    };
    uint32_t storage;

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        g_assert_true(sgi_kona_gl_texture_format_storage(cases[i].index,
                                                         &storage));
        g_assert_cmphex(storage, ==, cases[i].storage);
    }
    storage = 0xdeadbeef;
    g_assert_false(sgi_kona_gl_texture_format_storage(4, &storage));
    g_assert_cmphex(storage, ==, 0xdeadbeef);

    g_assert_true(sgi_kona_gl_texture_storage_binding_layout(
        0x14, SGI_GL_TEXTURE_2D, 0x44));
    g_assert_false(sgi_kona_gl_texture_storage_binding_layout(
        0x14, SGI_GL_TEXTURE_2D, 0x43));
    g_assert_true(sgi_kona_gl_texture_storage_binding_layout(
        0x14, SGI_GL_TEXTURE_3D, 0x23));
    g_assert_true(sgi_kona_gl_texture_storage_binding_layout(
        0x10, SGI_GL_TEXTURE_2D, 0x44));
    g_assert_true(sgi_kona_gl_texture_storage_binding_layout(
        0x30, SGI_GL_TEXTURE_2D, 0x43));
    g_assert_false(sgi_kona_gl_texture_storage_binding_layout(
        0x30, SGI_GL_TEXTURE_2D, 0x44));
    g_assert_false(sgi_kona_gl_texture_storage_binding_layout(
        0x50, SGI_GL_TEXTURE_2D, 0x44));
}

static void test_texture_internal_format(void)
{
    static const struct {
        uint32_t index, base, internal;
    } cases[] = {
        { 0, SGI_GL_LUMINANCE, SGI_GL_LUMINANCE },
        { 1, SGI_GL_LUMINANCE_ALPHA, SGI_GL_LUMINANCE_ALPHA },
        { 2, SGI_GL_RGBA, SGI_GL_RGBA },
        { 3, SGI_GL_LUMINANCE, 0x8042 },
        { 6, SGI_GL_LUMINANCE, 0x803f },
        { 7, SGI_GL_ALPHA, 0x803b },
        { 9, SGI_GL_LUMINANCE_ALPHA, 0x8043 },
        { 10, SGI_GL_LUMINANCE, 0x8040 },
        { 11, SGI_GL_ALPHA, 0x803c },
        { 13, SGI_GL_LUMINANCE_ALPHA, 0x8044 },
        { 14, SGI_GL_LUMINANCE_ALPHA, 0x8045 },
        { 15, SGI_GL_LUMINANCE, SGI_GL_LUMINANCE },
        { 16, SGI_GL_LUMINANCE, 0x8041 },
        { 17, SGI_GL_ALPHA, SGI_GL_ALPHA },
        { 18, SGI_GL_ALPHA, 0x803d },
        { 21, SGI_GL_LUMINANCE_ALPHA, SGI_GL_LUMINANCE_ALPHA },
        { 23, SGI_GL_ALPHA, 0x803e },
        { 25, SGI_GL_RGB, 0x804e },
        { 26, SGI_GL_RGBA, SGI_GL_RGBA2 },
        { 27, SGI_GL_RGB, SGI_GL_R3_G3_B2 },
        { 28, SGI_GL_RGB, SGI_GL_RGB4 },
        { 29, SGI_GL_RGBA, SGI_GL_RGBA4 },
        { 30, SGI_GL_RGB, SGI_GL_RGB5 },
        { 31, SGI_GL_RGBA, SGI_GL_RGB5_A1 },
        { 33, SGI_GL_RGB, 0x8051 },
        { 34, SGI_GL_RGBA, 0x8058 },
        { 37, SGI_GL_RGB, SGI_GL_RGB },
        { 38, SGI_GL_RGB, SGI_GL_RGB },
        { 41, SGI_GL_RGBA, SGI_GL_RGBA },
        { 63, SGI_GL_DEPTH_COMPONENT, SGI_GL_DEPTH_COMPONENT },
    };
    uint32_t base, internal;

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        g_assert_true(sgi_kona_gl_texture_internal_format(cases[i].index,
                                                          &base, &internal));
        g_assert_cmphex(base, ==, cases[i].base);
        g_assert_cmphex(internal, ==, cases[i].internal);
    }
    for (uint32_t index = 0; index < 4096; index++) {
        bool supported = false;

        for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
            supported |= cases[i].index == index;
        }
        base = internal = 0xdeadbeef;
        g_assert_cmpint(sgi_kona_gl_texture_internal_format(index, &base,
                                                            &internal),
                        ==, supported);
        if (!supported) {
            g_assert_cmphex(base, ==, 0xdeadbeef);
            g_assert_cmphex(internal, ==, 0xdeadbeef);
        }
    }
}

static void test_texture_integer_conversion(void)
{
    static const struct {
        uint32_t type, width, source[4], expected[5];
    } cases[] = {
        { 0, 1, { 0x80ff007f },
          { 0, 0x000001ff, 0x000001ff, 0x000000ff, 0 } },
        { 2, 2, { 0x8000ffff, 0x00007fff },
          { 0, 0x000000ff, 0x000000ff, 0x000000ff, 0 } },
        { 3, 2, { 0x00008000, 0xffff4000 },
          { 0, 0x0080ffff, 0x0080ff40, 0x000000ff, 0x00000080 } },
        { 4, 4, { 0x80000000, 0xffffffff, 0, 0x7fffffff },
          { 0, 0x000000ff, 0x000000ff, 0x000000ff, 0 } },
        { 5, 4, { 0, 0x80000000, 0xffffffff, 0x40000000 },
          { 0, 0x0080ffff, 0x0080ff40, 0x000000ff, 0x00000080 } },
    };
    const uint32_t components[] = { 1, 3, 4, 1, 2 };
    uint32_t rgba;

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        for (uint32_t format = 3; format <= 7; format++) {
            g_assert_cmpuint(sgi_kona_gl_texture_pixel_bytes(format,
                                                             cases[i].type),
                             ==, cases[i].width * components[format - 3]);
            g_assert_true(sgi_kona_gl_texture_pixel_rgba(format, cases[i].type,
                                                         cases[i].source,
                                                         false, &rgba));
            g_assert_cmphex(rgba, ==, cases[i].expected[format - 3]);
        }
    }
    /* Signed zero maps slightly positive under OpenGL 1.2.1 table 2.6. */
    const uint32_t zero[] = { 0 };
    g_assert_true(sgi_kona_gl_texture_pixel_rgba(6, 0, zero, false, &rgba));
    g_assert_cmphex(rgba, ==, 0x010101ff);
}

static void test_texture_swap_bytes(void)
{
    static const struct {
        uint32_t type, source[4], expected[5];
    } cases[] = {
        { 0, { 0x80ff007f },
          { 0, 0x000001ff, 0x000001ff, 0x000000ff, 0 } },
        { 1, { 0x4080c0ff },
          { 0x00000040, 0x4080c0ff, 0x4080c0ff, 0x404040ff, 0x40404080 } },
        { 2, { 0x0080ffff, 0x0000ff7f },
          { 0, 0x000000ff, 0x000000ff, 0x000000ff, 0 } },
        { 3, { 0x00000080, 0xffff0040 },
          { 0, 0x0080ffff, 0x0080ff40, 0x000000ff, 0x00000080 } },
        { 4, { 0x00000080, 0xffffffff, 0, 0xffffff7f },
          { 0, 0x000000ff, 0x000000ff, 0x000000ff, 0 } },
        { 5, { 0, 0x00000080, 0xffffffff, 0x00000040 },
          { 0, 0x0080ffff, 0x0080ff40, 0x000000ff, 0x00000080 } },
        { 6, { 0x0000803e, 0x0000003f, 0x0000403f, 0x0000803f },
          { 0x00000040, 0x4080bfff, 0x4080bfff, 0x404040ff, 0x40404080 } },
    };
    const uint32_t components[] = { 1, 3, 4, 1, 2 };
    const uint32_t invalid[] = {
        0x0000c07f, 0x0100807f, 0x0000807f, 0x000080ff,
    };
    const uint32_t saturated[] = { 0x000080bf, 0x00000040, 0, 0x00000080 };
    uint32_t rgba;

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        for (uint32_t format = 3; format <= 7; format++) {
            uint32_t source[4];

            memcpy(source, cases[i].source, sizeof(source));
            g_assert_true(sgi_kona_gl_texture_pixel_rgba(format, cases[i].type,
                                                         source, true, &rgba));
            g_assert_cmphex(rgba, ==, cases[i].expected[format - 3]);
            g_assert_cmpmem(source, sizeof(source), cases[i].source,
                            sizeof(cases[i].source));
        }
    }
    for (uint32_t format = 3; format <= 7; format++) {
        for (unsigned k = 0; k < components[format - 3]; k++) {
            for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
                uint32_t source[4], original[4];

                memcpy(source, cases[6].source, sizeof(source));
                source[k] = invalid[i];
                memcpy(original, source, sizeof(original));
                rgba = 0x12345678;
                g_assert_false(sgi_kona_gl_texture_pixel_rgba(format, 6,
                                                              source, true,
                                                              &rgba));
                g_assert_cmphex(rgba, ==, 0x12345678);
                g_assert_cmpmem(source, sizeof(source), original,
                                sizeof(original));
            }
        }
    }
    g_assert_true(sgi_kona_gl_texture_pixel_rgba(5, 6, saturated, true, &rgba));
    g_assert_cmphex(rgba, ==, 0x00ff0000);
    /* Raw infinity becomes finite after swapping; test the decoded value. */
    const uint32_t raw_infinity[] = { 0x7f800000 };

    g_assert_true(sgi_kona_gl_texture_pixel_rgba(3, 6, raw_infinity,
                                                true, &rgba));
    g_assert_cmphex(rgba, ==, 0);
    for (uint32_t format = 0; format < 14; format++) {
        for (uint32_t type = 0; type <= 13; type++) {
            bool packed = (format == 4 && type == 8) ||
                          ((format == 5 || format == 10) &&
                           type >= 9 && type <= 12);

            if ((format > 10 || type > 6) && !packed) {
                rgba = 0x12345678;
                g_assert_false(sgi_kona_gl_texture_pixel_rgba(format, type,
                                                              cases[6].source,
                                                              true, &rgba));
                g_assert_cmphex(rgba, ==, 0x12345678);
            }
        }
    }
}

static void test_texture_filter_curve(void)
{
    uint32_t words[33] = { 0 };
    double values[64];

    /* A literal quartet crosses signs and all mantissa/exponent positions. */
    words[0] = 0x9ffc0101;
    words[1] = 0x80;
    /* Native padding is not a coefficient or a validity field. */
    words[32] = 0xdeadbeef;
    g_assert_true(sgi_kona_gl_texture_filter_curve(words, values));
    g_assert_cmpfloat(values[0], ==, 1);
    g_assert_cmpfloat(values[1], ==, -0.5);
    g_assert_cmpfloat(values[2], ==, 7.9375);
    g_assert_cmpfloat(values[3], ==, -4);
    for (unsigned i = 4; i < 64; i++) {
        g_assert_cmpfloat(values[i], ==, 0);
    }

    /* Default sharpening sends negated public weights at selected rho nodes. */
    words[0] = 0x200;
    words[4] = 0x280;
    words[8] = 0x300;
    words[16] = 0x380;
    g_assert_true(sgi_kona_gl_texture_filter_curve(words, values));
    g_assert_cmpfloat(values[0], ==, -1);
    g_assert_cmpfloat(values[8], ==, -0.75);
    g_assert_cmpfloat(values[16], ==, -0.5);
    g_assert_cmpfloat(values[32], ==, -0.25);
    g_assert_cmpfloat(values[63], ==, 0);
}

static void test_texture_filter_curve_refusal(void)
{
    uint32_t words[33] = { 0 };
    double values[64], unchanged[64];

    for (unsigned i = 0; i < 64; i++) {
        values[i] = unchanged[i] = i + 0.25;
    }
    for (unsigned word = 1; word < 32; word += 2) {
        words[word] = 0x100;
        g_assert_false(sgi_kona_gl_texture_filter_curve(words, values));
        g_assert_cmpmem(values, sizeof(values), unchanged, sizeof(unchanged));
        words[word] = 0;
    }
    g_assert_false(sgi_kona_gl_texture_filter_curve(NULL, values));
    g_assert_false(sgi_kona_gl_texture_filter_curve(words, NULL));
    g_assert_cmpmem(values, sizeof(values), unchanged, sizeof(unchanged));
}

static void test_bitmap_pio_contiguous(void)
{
    const uint32_t header[9] = { 12, 2, 3, 0, 0 };
    const uint32_t words[] = { 0xdeadbe80, 0x40ccdda0, 0x10eeeeee };
    SGIKonaGLBitmapLayout layout;
    uint8_t out;

    g_assert_true(sgi_kona_gl_bitmap_pio_layout(header, 0, 0, 0, 4, &layout));
    g_assert_cmpuint(layout.payload_words, ==, 3);
    g_assert_cmpuint(layout.stride, ==, 4);
    g_assert_cmpuint(layout.phase, ==, 3);
    g_assert_false(layout.strided);
    for (uint32_t row = 0; row < 2; row++) {
        for (uint32_t byte = 0; byte < 2; byte++) {
            static const uint8_t expected[2][2] = { { 0x80, 0x40 },
                                                   { 0xa0, 0x10 } };

            g_assert_true(sgi_kona_gl_bitmap_pio_byte(&layout, words, 3,
                                                     false, row, byte, &out));
            g_assert_cmphex(out, ==, expected[row][byte]);
        }
    }
}

static void test_bitmap_pio_skips(void)
{
    const uint32_t header[9] = { 9, 2, 1, 0, 1 };
    const uint32_t words[] = { 0xdead60c0, 0xbeef21a0 };
    SGIKonaGLBitmapLayout layout;
    uint8_t out;

    g_assert_true(sgi_kona_gl_bitmap_pio_layout(header, 17, 1, 9, 2, &layout));
    g_assert_cmpuint(layout.payload_words, ==, 2);
    g_assert_cmpuint(layout.phase, ==, 2);
    g_assert_cmpuint(layout.bit_skip, ==, 1);
    g_assert_cmpuint(layout.stride, ==, 4);
    for (uint32_t row = 0; row < 2; row++) {
        for (uint32_t byte = 0; byte < 2; byte++) {
            static const uint8_t expected[2][2] = { { 0xc1, 0x80 },
                                                   { 0x43, 0 } };

            g_assert_true(sgi_kona_gl_bitmap_pio_byte(&layout, words, 2,
                                                     false, row, byte, &out));
            g_assert_cmphex(out, ==, expected[row][byte]);
        }
    }
}

static void test_bitmap_pio_row_padding(void)
{
    const uint32_t header[9] = { 9, 5, 0, 0, 1 };
    const uint32_t words[] = { 0x80000000, 0xfffffff0, 0x80eeeeee,
                              0xdead4000, 0xbe200000, 0x10000000 };
    SGIKonaGLBitmapLayout layout;
    uint8_t out;

    g_assert_true(sgi_kona_gl_bitmap_pio_layout(header, 17, 0, 0, 1, &layout));
    g_assert_cmpuint(layout.payload_words, ==, 6);
    g_assert_cmpuint(layout.stride, ==, 3);
    g_assert_true(sgi_kona_gl_bitmap_pio_byte(&layout, words, 6,
                                            false, 1, 0, &out));
    g_assert_cmphex(out, ==, 0xf0);
    g_assert_true(sgi_kona_gl_bitmap_pio_byte(&layout, words, 6,
                                            false, 1, 1, &out));
    g_assert_cmphex(out, ==, 0x80);
    g_assert_true(sgi_kona_gl_bitmap_pio_byte(&layout, words, 6,
                                            false, 4, 0, &out));
    g_assert_cmphex(out, ==, 0x10);
}

static void test_bitmap_pio_bit_order(void)
{
    const uint32_t header[9] = { 3, 1, 0, 0, 0 };
    const uint32_t word = 0x05ffffff;
    SGIKonaGLBitmapLayout layout;
    uint8_t out;

    g_assert_true(sgi_kona_gl_bitmap_pio_layout(header, 0, 0, 0, 1, &layout));
    g_assert_true(sgi_kona_gl_bitmap_pio_byte(&layout, &word, 1,
                                            true, 0, 0, &out));
    g_assert_cmphex(out, ==, 0xa0);
    g_assert_true(sgi_kona_gl_bitmap_pio_byte(&layout, &word, 1,
                                            false, 0, 0, &out));
    g_assert_cmphex(out, ==, 0);
}

static void test_bitmap_pio_skip_rows_phase(void)
{
    const uint32_t header[9] = { 8, 2, 1, 0, 0 };
    const uint32_t words[] = { 0xdeadaa80, 0x40ffffff };
    SGIKonaGLBitmapLayout layout;
    uint8_t out;

    g_assert_true(sgi_kona_gl_bitmap_pio_layout(header, 0, 2, 0, 1, &layout));
    g_assert_cmpuint(layout.phase, ==, 3);
    g_assert_cmpuint(layout.payload_words, ==, 2);
    g_assert_true(sgi_kona_gl_bitmap_pio_byte(&layout, words, 2,
                                            false, 0, 0, &out));
    g_assert_cmphex(out, ==, 0x80);
    g_assert_true(sgi_kona_gl_bitmap_pio_byte(&layout, words, 2,
                                            false, 1, 0, &out));
    g_assert_cmphex(out, ==, 0x40);
}

static void test_bitmap_pio_large_counts(void)
{
    const uint32_t header[9] = { UINT32_MAX, UINT32_MAX, 3, 0, 0 };
    SGIKonaGLBitmapLayout layout;

    g_assert_true(sgi_kona_gl_bitmap_pio_layout(header, 0, UINT32_MAX, 0,
                                               8, &layout));
    g_assert_cmpuint(layout.phase, ==, 3);
    g_assert_cmpuint(layout.stride, ==, 536870912);
    g_assert_cmpuint(layout.payload_words, ==, UINT64_C(576460752169205761));
}

static void test_bitmap_pio_refusals(void)
{
    uint32_t header[9] = { 8, 1, 0, 0, 0 };
    SGIKonaGLBitmapLayout layout = { .payload_words = 77 };
    const SGIKonaGLBitmapLayout before = layout;
    uint8_t out = 0xa5;
    uint32_t word = 0xffffffff;

    g_assert_false(sgi_kona_gl_bitmap_pio_layout(header, 0, 0, 0, 3, &layout));
    header[3] = 1;
    g_assert_false(sgi_kona_gl_bitmap_pio_layout(header, 0, 0, 0, 1, &layout));
    header[3] = 0;
    header[2] = 4;
    g_assert_false(sgi_kona_gl_bitmap_pio_layout(header, 0, 0, 0, 1, &layout));
    header[2] = 0;
    header[4] = 1;
    g_assert_false(sgi_kona_gl_bitmap_pio_layout(header, 0, 0, 0, 1, &layout));
    g_assert_cmpmem(&layout, sizeof(layout), &before, sizeof(before));
    header[4] = 0;
    g_assert_true(sgi_kona_gl_bitmap_pio_layout(header, 0, 0, 0, 1, &layout));
    g_assert_false(sgi_kona_gl_bitmap_pio_byte(&layout, &word, 0,
                                             false, 0, 0, &out));
    g_assert_false(sgi_kona_gl_bitmap_pio_byte(&layout, &word, 1,
                                             false, 1, 0, &out));
    g_assert_false(sgi_kona_gl_bitmap_pio_byte(&layout, &word, 1,
                                             false, 0, 1, &out));
    g_assert_cmphex(out, ==, 0xa5);
}

static void test_bitmap_dma_contiguous_padding(void)
{
    const uint32_t header[9] = { 10, 3, 2, 1, 0 };
    const uint32_t words[] = { 0xaabbffff, 0xcccc0000, 0xdddde040 };
    const uint8_t expected[][2] = { { 0xff, 0xc0 }, { 0, 0 }, { 0xe0, 0x40 } };
    SGIKonaGLBitmapDMALayout d;

    g_assert_true(sgi_kona_gl_bitmap_dma_layout(header, 0, 1, 0, 4, &d));
    g_assert_cmpuint(d.source_phase, ==, 0);
    g_assert_cmpuint(d.bitmap.phase, ==, 2);
    g_assert_cmpuint(d.lines, ==, 1);
    g_assert_cmpuint(d.total_bytes, ==, 12);
    g_assert_cmpuint(d.line_bytes, ==, 12);
    g_assert_cmpuint(d.pitch, ==, 12);
    g_assert_cmpuint(d.span, ==, 12);
    for (uint32_t row = 0; row < 3; row++) {
        for (uint32_t byte = 0; byte < 2; byte++) {
            uint64_t offset;
            uint8_t out;

            g_assert_true(sgi_kona_gl_bitmap_pio_offset(&d.bitmap, row,
                                                        byte, &offset));
            g_assert_cmpuint(offset, ==, 2 + row * 4 + byte);
            g_assert_true(sgi_kona_gl_bitmap_pio_byte(&d.bitmap, words,
                              ARRAY_SIZE(words), false, row, byte, &out));
            g_assert_cmphex(out, ==, expected[row][byte]);
        }
    }
}

static void test_bitmap_dma_strided_bit_skips(void)
{
    const uint32_t header[9] = { 10, 3, 1, 1, 1 };
    const uint8_t source[][2] = {
        { 0x16, 0x40 }, { 0xac, 0xd8 }, { 0xff, 0xff },
    };
    const uint64_t positions[][2] = { { 1, 2 }, { 4, 5 }, { 11, 12 } };
    const uint8_t expected[][3][2] = {
        { { 0xb2, 0 }, { 0x66, 0xc0 }, { 0xff, 0xc0 } },
        { { 0x40, 0 }, { 0xa8, 0xc0 }, { 0xff, 0xc0 } },
    };
    uint32_t words[4] = { 0 };
    SGIKonaGLBitmapDMALayout d;

    g_assert_true(sgi_kona_gl_bitmap_dma_layout(header, 24, 1, 11, 1, &d));
    g_assert_cmpuint(d.source_phase, ==, 1);
    g_assert_cmpuint(d.bitmap.bit_skip, ==, 3);
    g_assert_cmpuint(d.lines, ==, 3);
    g_assert_cmpuint(d.total_bytes, ==, 6);
    g_assert_cmpuint(d.line_bytes, ==, 2);
    g_assert_cmpuint(d.pitch, ==, 3);
    g_assert_cmpuint(d.span, ==, 8);
    g_assert_cmpuint(d.bitmap.payload_words, ==, ARRAY_SIZE(words));
    for (uint32_t row = 0; row < 3; row++) {
        for (uint32_t byte = 0; byte < 2; byte++) {
            uint64_t offset;

            g_assert_true(sgi_kona_gl_bitmap_pio_offset(&d.bitmap, row,
                                                        byte, &offset));
            g_assert_cmpuint(offset, ==, positions[row][byte]);
            words[offset / 4] |= (uint32_t)source[row][byte] <<
                                 (24 - 8 * (offset & 3));
        }
    }
    for (unsigned lsb = 0; lsb < 2; lsb++) {
        for (uint32_t row = 0; row < 3; row++) {
            for (uint32_t byte = 0; byte < 2; byte++) {
                uint8_t out;

                g_assert_true(sgi_kona_gl_bitmap_pio_byte(&d.bitmap, words,
                                  ARRAY_SIZE(words), lsb, row, byte, &out));
                g_assert_cmphex(out, ==, expected[lsb][row][byte]);
            }
        }
    }
}

static void test_bitmap_dma_partial_widths(void)
{
    uint32_t header[9] = { 1, 2, 3, 1, 0 };
    const uint32_t words[] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
    SGIKonaGLBitmapDMALayout d;

    for (uint32_t width = 1; width <= 33; width++) {
        uint32_t row_bytes = (width + 7) / 8;
        uint32_t total = 4 * ((3 + 2 * row_bytes + 3) / 4);

        header[0] = width;
        g_assert_true(sgi_kona_gl_bitmap_dma_layout(header, 0, 0, 0, 1, &d));
        g_assert_cmpuint(d.total_bytes, ==, total);
        g_assert_cmpuint(d.source_phase, ==, 0);
        g_assert_cmpuint(d.bitmap.phase, ==, 3);
        for (uint32_t row = 0; row < 2; row++) {
            for (uint32_t byte = 0; byte < row_bytes; byte++) {
                uint8_t out, expected = 0xff;

                if (byte == row_bytes - 1 && (width & 7)) {
                    expected <<= 8 - (width & 7);
                }
                g_assert_true(sgi_kona_gl_bitmap_pio_byte(&d.bitmap, words,
                                  ARRAY_SIZE(words), false, row, byte, &out));
                g_assert_cmphex(out, ==, expected);
            }
        }
    }
}

static void test_bitmap_dma_refusals(void)
{
    const struct {
        uint32_t header[9];
        uint32_t row_length, skip_pixels, alignment;
    } cases[] = {
        { { 32, 1, 0, 0, 0 }, 0, 0, 4 },
        { { 32, 1, 0, 2, 0 }, 0, 0, 4 },
        { { 32, 1, 4, 1, 0 }, 0, 0, 4 },
        { { 0, 1, 0, 1, 0 }, 0, 0, 4 },
        { { 32, 0, 0, 1, 0 }, 0, 0, 4 },
        { { 32, 1, 0, 1, 0 }, 0, 0, 3 },
        { { 32, 1, 0, 1, 0 }, 0, 1, 4 },
        { { 32, 1, 0, 1, 1 }, 0, 0, 4 },
        { { 32, 1, 0, 1, 1 }, 16, 0, 1 },
        { { 32, UINT32_MAX, 0, 1, 0 }, 0, 0, 4 },
    };
    SGIKonaGLBitmapDMALayout before = { .total_bytes = 0x1234 };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        SGIKonaGLBitmapDMALayout d = before;

        g_assert_false(sgi_kona_gl_bitmap_dma_layout(cases[i].header,
                          cases[i].row_length, 0, cases[i].skip_pixels,
                          cases[i].alignment, &d));
        g_assert_cmpmem(&d, sizeof(d), &before, sizeof(before));
    }
}

static void test_bitmap_dma_large_span(void)
{
    const uint32_t header[9] = { 1, 16, 1, 1, 1 };
    SGIKonaGLBitmapDMALayout d;

    g_assert_true(sgi_kona_gl_bitmap_dma_layout(header, UINT32_MAX, 0, 0,
                                                1, &d));
    g_assert_cmpuint(d.total_bytes, ==, 16);
    g_assert_cmpuint(d.line_bytes, ==, 1);
    g_assert_cmpuint(d.pitch, ==, 536870912);
    g_assert_cmpuint(d.span, ==, UINT64_C(8053063681));
    g_assert_cmpuint(d.source_phase, ==, 1);
}

static void test_bitmap_pio_offset_refusals(void)
{
    SGIKonaGLBitmapLayout l = {
        .width = 1, .height = 3, .row_bytes = 1, .stride = UINT64_MAX,
    };
    uint64_t offset = 0x1234;

    g_assert_false(sgi_kona_gl_bitmap_pio_offset(NULL, 0, 0, &offset));
    g_assert_false(sgi_kona_gl_bitmap_pio_offset(&l, 0, 0, NULL));
    g_assert_false(sgi_kona_gl_bitmap_pio_offset(&l, 3, 0, &offset));
    g_assert_false(sgi_kona_gl_bitmap_pio_offset(&l, 0, 1, &offset));
    g_assert_false(sgi_kona_gl_bitmap_pio_offset(&l, 2, 0, &offset));
    l.phase = 1;
    g_assert_false(sgi_kona_gl_bitmap_pio_offset(&l, 1, 0, &offset));
    g_assert_cmpuint(offset, ==, 0x1234);
}

static void test_pixels_pio_contiguous_padding(void)
{
    const uint32_t header[8] = { 1, 3, 4, 1, 2, 0, 0, 0 };
    const uint32_t words[] = {
        0xaabb1020, 0x30ee4050, 0x60dd7080, 0x90cccccc,
    };
    const uint8_t expected[][3] = {
        { 0x10, 0x20, 0x30 }, { 0x40, 0x50, 0x60 }, { 0x70, 0x80, 0x90 },
    };
    SGIKonaGLPixelLayout l;

    g_assert_true(sgi_kona_gl_pixels_pio_layout(header, 0, 0, 0, 4, &l));
    g_assert_false(l.strided);
    g_assert_cmpuint(l.stride, ==, 4);
    g_assert_cmpuint(l.row_bytes, ==, 3);
    g_assert_cmpuint(l.payload_words, ==, 4);
    for (uint32_t row = 0; row < 3; row++) {
        for (uint32_t byte = 0; byte < 3; byte++) {
            uint8_t out;

            g_assert_true(sgi_kona_gl_pixels_pio_byte(&l, words,
                              ARRAY_SIZE(words), row, byte, &out));
            g_assert_cmphex(out, ==, expected[row][byte]);
        }
    }
}

static void test_pixels_pio_strided_phases(void)
{
    const uint32_t header[8] = { 1, 3, 4, 1, 1, 0, 1, 0 };
    const uint32_t words[] = {
        0xaa112233, 0xaaaabb44, 0x5566eeee, 0xcc778899,
    };
    const uint8_t expected[][3] = {
        { 0x11, 0x22, 0x33 }, { 0x44, 0x55, 0x66 }, { 0x77, 0x88, 0x99 },
    };
    SGIKonaGLPixelLayout l;

    g_assert_true(sgi_kona_gl_pixels_pio_layout(header, 2, 0, 0, 1, &l));
    g_assert_true(l.strided);
    g_assert_cmpuint(l.stride, ==, 6);
    g_assert_cmpuint(l.payload_words, ==, 4);
    for (uint32_t row = 0; row < 3; row++) {
        for (uint32_t byte = 0; byte < 3; byte++) {
            uint8_t out;

            g_assert_true(sgi_kona_gl_pixels_pio_byte(&l, words,
                              ARRAY_SIZE(words), row, byte, &out));
            g_assert_cmphex(out, ==, expected[row][byte]);
        }
    }
}

static void test_pixels_pio_skips(void)
{
    const uint32_t header[8] = { 2, 2, 4, 1, 2, 0, 1, 0 };
    const uint32_t words[] = {
        0xaa010203, 0x040506aa, 0xbb070809, 0x0a0b0cbb,
    };
    SGIKonaGLPixelLayout l;

    g_assert_true(sgi_kona_gl_pixels_pio_layout(header, 4, 1, 1, 4, &l));
    g_assert_cmpuint(l.phase, ==, 1);
    g_assert_cmpuint(l.stride, ==, 12);
    g_assert_cmpuint(l.payload_words, ==, 4);
    for (uint32_t row = 0; row < 2; row++) {
        for (uint32_t byte = 0; byte < 6; byte++) {
            uint8_t out;

            g_assert_true(sgi_kona_gl_pixels_pio_byte(&l, words,
                              ARRAY_SIZE(words), row, byte, &out));
            g_assert_cmpuint(out, ==, 1 + row * 6 + byte);
        }
    }
}

static void test_pixels_pio_scalar_widths(void)
{
    const uint32_t sizes[] = { 1, 1, 2, 2, 4, 4, 4 };
    const uint32_t components[] = { 1, 3, 4, 1, 2 };
    uint32_t header[8] = { 1, 1, 3, 0, 3, 0, 0, 0 };
    const uint32_t words[] = {
        0xaabbcc01, 0x02030405, 0x06070809, 0x0a0b0c0d, 0x0e0f10ee,
    };
    SGIKonaGLPixelLayout l;

    for (header[2] = 3; header[2] <= 7; header[2]++) {
        for (header[3] = 0; header[3] < ARRAY_SIZE(sizes); header[3]++) {
            uint32_t bytes = components[header[2] - 3] * sizes[header[3]];

            g_assert_true(sgi_kona_gl_pixels_pio_layout(header, 0, 0, 0,
                                                        1, &l));
            g_assert_cmpuint(l.pixel_bytes, ==, bytes);
            g_assert_cmpuint(l.payload_words, ==, (bytes + 6) / 4);
            for (uint32_t byte = 0; byte < bytes; byte++) {
                uint8_t out;

                g_assert_true(sgi_kona_gl_pixels_pio_byte(&l, words,
                                  ARRAY_SIZE(words), 0, byte, &out));
                g_assert_cmpuint(out, ==, byte + 1);
            }
        }
    }
}

static void test_pixels_pio_refusals(void)
{
    uint32_t header[8] = { 1, 1, 5, 1, 0, 0, 0, 0 };
    SGIKonaGLPixelLayout before = { .width = 0x1234 };
    SGIKonaGLPixelLayout l = before;
    const uint32_t word = 0x12345678;
    uint8_t out = 0xa5;

    header[5] = 1;
    g_assert_false(sgi_kona_gl_pixels_pio_layout(header, 0, 0, 0, 4, &l));
    g_assert_cmpmem(&l, sizeof(l), &before, sizeof(before));
    header[5] = 0;
    g_assert_false(sgi_kona_gl_pixels_pio_layout(header, 0, 0, 0, 3, &l));
    g_assert_false(sgi_kona_gl_pixels_pio_layout(header, 2, 0, 0, 4, &l));
    header[2] = 14;
    g_assert_false(sgi_kona_gl_pixels_pio_layout(header, 0, 0, 0, 4, &l));
    header[2] = 5;
    g_assert_true(sgi_kona_gl_pixels_pio_layout(header, 0, 0, 0, 4, &l));
    g_assert_false(sgi_kona_gl_pixels_pio_byte(&l, &word, 0, 0, 0, &out));
    g_assert_false(sgi_kona_gl_pixels_pio_byte(&l, &word, 1, 1, 0, &out));
    g_assert_false(sgi_kona_gl_pixels_pio_byte(&l, &word, 1, 0, 4, &out));
    g_assert_cmphex(out, ==, 0xa5);
    header[0] = header[1] = UINT32_MAX;
    header[3] = 6;
    l = before;
    g_assert_false(sgi_kona_gl_pixels_pio_layout(header, 0, 0, 0, 4, &l));
    g_assert_cmpmem(&l, sizeof(l), &before, sizeof(before));
    header[1] = 1;
    g_assert_true(sgi_kona_gl_pixels_pio_layout(header, 0, 0, 0, 4, &l));
    g_assert_cmpuint(l.payload_words, ==, (uint64_t)UINT32_MAX * 4);
}

static void test_pixels_bitmap_layout(void)
{
    uint32_t h[8] = { 9, 2, 11, 7, 3, 0, 0, 0 };
    SGIKonaGLPixelLayout l;
    SGIKonaGLPixelDMALayout d;
    uint64_t offset;

    g_assert_true(sgi_kona_gl_pixels_pio_layout(h, 0, 0, 0, 4, &l));
    g_assert_cmpuint(l.pixel_bytes, ==, 0);
    g_assert_cmpuint(l.row_bytes, ==, 2);
    g_assert_cmpuint(l.stride, ==, 4);
    g_assert_cmpuint(l.phase, ==, 3);
    g_assert_cmpuint(l.payload_words, ==, 3);
    g_assert_true(sgi_kona_gl_pixels_pio_offset(&l, 1, 1, &offset));
    g_assert_cmpuint(offset, ==, 8);
    h[5] = 1;
    g_assert_true(sgi_kona_gl_pixels_dma_layout(h, 0, 0, 0, 4, &d));
    g_assert_cmpuint(d.total_bytes, ==, 12);
    g_assert_cmpuint(d.source_phase, ==, 0);
    g_assert_cmpuint(d.lines, ==, 1);
    h[2] = 12;
    h[4] = 1;
    h[6] = 1;
    g_assert_true(sgi_kona_gl_pixels_dma_layout(h, 24, 1, 3, 4, &d));
    g_assert_cmpuint(d.total_bytes, ==, 4);
    g_assert_cmpuint(d.line_bytes, ==, 2);
    g_assert_cmpuint(d.pitch, ==, 4);
    g_assert_cmpuint(d.source_phase, ==, 1);
    g_assert_cmpuint(d.lines, ==, 2);
    h[2] = 13;
    g_assert_false(sgi_kona_gl_pixels_dma_layout(h, 24, 1, 3, 4, &d));
}

static void test_pixels_dma_contiguous_padding(void)
{
    const uint32_t header[8] = { 1, 3, 4, 1, 2, 1, 0, 0 };
    SGIKonaGLPixelDMALayout d;
    uint64_t offset;

    g_assert_true(sgi_kona_gl_pixels_dma_layout(header, 0, 0, 0, 4, &d));
    g_assert_cmpuint(d.lines, ==, 1);
    g_assert_cmpuint(d.total_bytes, ==, 11);
    g_assert_cmpuint(d.line_bytes, ==, 11);
    g_assert_cmpuint(d.pitch, ==, 11);
    g_assert_cmpuint(d.span, ==, 11);
    g_assert_cmpuint(d.pixels.phase, ==, 2);
    g_assert_cmpuint(d.pixels.payload_words, ==, 4);
    g_assert_true(sgi_kona_gl_pixels_pio_offset(&d.pixels, 2, 2, &offset));
    g_assert_cmpuint(offset, ==, 12);
}

static void test_pixels_dma_strided_skips(void)
{
    const uint32_t header[8] = { 1, 3, 4, 1, 1, 1, 1, 0 };
    const uint64_t positions[][3] = {
        { 2, 3, 4 }, { 8, 9, 10 }, { 14, 15, 16 },
    };
    SGIKonaGLPixelDMALayout d;

    g_assert_true(sgi_kona_gl_pixels_dma_layout(header, 2, 1, 1, 1, &d));
    g_assert_cmpuint(d.lines, ==, 3);
    g_assert_cmpuint(d.total_bytes, ==, 9);
    g_assert_cmpuint(d.line_bytes, ==, 3);
    g_assert_cmpuint(d.pitch, ==, 6);
    g_assert_cmpuint(d.span, ==, 15);
    g_assert_cmpuint(d.pixels.phase, ==, 2);
    g_assert_cmpuint(d.pixels.payload_words, ==, 5);
    for (uint32_t row = 0; row < 3; row++) {
        for (uint32_t byte = 0; byte < 3; byte++) {
            uint64_t offset;

            g_assert_true(sgi_kona_gl_pixels_pio_offset(&d.pixels, row,
                                                        byte, &offset));
            g_assert_cmpuint(offset, ==, positions[row][byte]);
        }
    }
}

static void test_pixels_dma_scalar_widths(void)
{
    const uint32_t sizes[] = { 1, 1, 2, 2, 4, 4, 4 };
    const uint32_t components[] = { 1, 3, 4, 1, 2 };
    uint32_t header[8] = { 2, 3, 3, 0, 3, 1, 0, 0 };
    SGIKonaGLPixelDMALayout d;

    for (header[2] = 3; header[2] <= 7; header[2]++) {
        for (header[3] = 0; header[3] < ARRAY_SIZE(sizes); header[3]++) {
            uint32_t bytes = components[header[2] - 3] * sizes[header[3]];

            g_assert_true(sgi_kona_gl_pixels_dma_layout(header, 0, 0, 0,
                                                        1, &d));
            g_assert_cmpuint(d.total_bytes, ==, 6 * bytes);
            g_assert_cmpuint(d.line_bytes, ==, 6 * bytes);
            g_assert_cmpuint(d.pitch, ==, 6 * bytes);
            g_assert_cmpuint(d.lines, ==, 1);
            g_assert_cmpuint(d.pixels.phase, ==, 3);
            g_assert_cmpuint(d.pixels.pixel_bytes, ==, bytes);
        }
    }
}

static void test_pixels_dma_refusals(void)
{
    const struct {
        uint32_t header[8];
        uint32_t row_length, skip_pixels, alignment;
    } cases[] = {
        { { 1, 1, 5, 1, 0, 0, 0, 0 }, 0, 0, 4 },
        { { 1, 1, 5, 1, 0, 2, 0, 0 }, 0, 0, 4 },
        { { 1, 1, 5, 1, 0, 1, 0, 1 }, 0, 0, 4 },
        { { 1, 1, 5, 1, 4, 1, 0, 0 }, 0, 0, 4 },
        { { 0, 1, 5, 1, 0, 1, 0, 0 }, 0, 0, 4 },
        { { 1, 0, 5, 1, 0, 1, 0, 0 }, 0, 0, 4 },
        { { 1, 1, 5, 1, 0, 1, 0, 0 }, 0, 0, 3 },
        { { 1, 1, 5, 1, 0, 1, 0, 0 }, 0, 1, 4 },
        { { 1, 1, 5, 1, 0, 1, 1, 0 }, 0, 0, 4 },
        { { 2, 1, 5, 1, 0, 1, 1, 0 }, 1, 0, 4 },
        { { 1, 1, 14, 1, 0, 1, 0, 0 }, 0, 0, 4 },
        { { UINT32_MAX, 2, 3, 1, 0, 1, 0, 0 }, 0, 0, 1 },
        { { UINT32_MAX, UINT32_MAX, 5, 6, 0, 1, 0, 0 }, 0, 0, 4 },
        { { 1, 1, 5, 6, 0, 1, 1, 0 }, UINT32_MAX, 0, 4 },
    };
    SGIKonaGLPixelDMALayout before = { .total_bytes = 0x1234 };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        SGIKonaGLPixelDMALayout d = before;

        g_assert_false(sgi_kona_gl_pixels_dma_layout(cases[i].header,
                          cases[i].row_length, 0, cases[i].skip_pixels,
                          cases[i].alignment, &d));
        g_assert_cmpmem(&d, sizeof(d), &before, sizeof(before));
    }
}

static void test_pixels_dma_large_span(void)
{
    uint32_t header[8] = { UINT32_MAX, 1, 3, 1, 0, 1, 0, 0 };
    SGIKonaGLPixelDMALayout d;

    g_assert_true(sgi_kona_gl_pixels_dma_layout(header, 0, 0, 0, 1, &d));
    g_assert_cmpuint(d.total_bytes, ==, UINT32_MAX);
    header[0] = 1;
    header[1] = 2;
    header[6] = 1;
    g_assert_true(sgi_kona_gl_pixels_dma_layout(header, UINT32_MAX, 0, 0,
                                                1, &d));
    g_assert_cmpuint(d.total_bytes, ==, 2);
    g_assert_cmpuint(d.line_bytes, ==, 1);
    g_assert_cmpuint(d.pitch, ==, UINT32_MAX);
    g_assert_cmpuint(d.span, ==, (uint64_t)UINT32_MAX + 1);
}

static void test_pixels_pio_offset_refusals(void)
{
    SGIKonaGLPixelLayout l = {
        .width = 1, .height = 3, .row_bytes = 1, .stride = UINT64_MAX,
    };
    uint64_t offset = 0x1234;

    g_assert_false(sgi_kona_gl_pixels_pio_offset(NULL, 0, 0, &offset));
    g_assert_false(sgi_kona_gl_pixels_pio_offset(&l, 0, 0, NULL));
    g_assert_false(sgi_kona_gl_pixels_pio_offset(&l, 3, 0, &offset));
    g_assert_false(sgi_kona_gl_pixels_pio_offset(&l, 0, 1, &offset));
    g_assert_false(sgi_kona_gl_pixels_pio_offset(&l, 2, 0, &offset));
    l.phase = 1;
    g_assert_false(sgi_kona_gl_pixels_pio_offset(&l, 1, 0, &offset));
    g_assert_cmpuint(offset, ==, 0x1234);
}

static void test_pixel_float_conversion(void)
{
    const uint32_t floats[] = {
        0xbe800000, 0x40000000, 0x3f000000, 0x3f000000,
    };
    const uint32_t swapped[] = {
        0x000080be, 0x00000040, 0x0000003f, 0x0000003f,
    };
    const uint32_t signed_bytes = 0x80ff007f;
    const uint32_t shorts[] = { 0x80008000, 0x80008000 };
    const float before[] = { 9, 8, 7, 6 };
    float rgba[4];

    g_assert_true(sgi_kona_gl_pixel_rgba_float(5, 6, floats, false, rgba));
    g_assert_cmpfloat(rgba[0], ==, -0.25f);
    g_assert_cmpfloat(rgba[1], ==, 2);
    g_assert_cmpfloat(rgba[2], ==, 0.5f);
    g_assert_cmpfloat(rgba[3], ==, 0.5f);
    g_assert_true(sgi_kona_gl_pixel_rgba_float(5, 6, swapped, true, rgba));
    g_assert_cmpfloat(rgba[0], ==, -0.25f);
    g_assert_cmpfloat(rgba[1], ==, 2);
    g_assert_true(sgi_kona_gl_pixel_rgba_float(5, 0, &signed_bytes,
                                              false, rgba));
    g_assert_cmpfloat(rgba[0], ==, -1);
    g_assert_cmpfloat_with_epsilon(rgba[1], -1.0f / 255, 0.0000001f);
    g_assert_cmpfloat_with_epsilon(rgba[2], 1.0f / 255, 0.0000001f);
    g_assert_cmpfloat(rgba[3], ==, 1);
    g_assert_true(sgi_kona_gl_pixel_rgba_float(5, 3, shorts, false, rgba));
    g_assert_cmpfloat_with_epsilon(rgba[3], 32768.0f / 65535, 0.0000001f);
    g_assert_cmpfloat(rgba[3], <, 0.5001f);
    for (uint32_t format = 3; format <= 7; format++) {
        g_assert_true(sgi_kona_gl_pixel_rgba_float(format, 6, floats,
                                                  false, rgba));
        if (format == 3) {
            g_assert_cmpfloat(rgba[0], ==, 0);
            g_assert_cmpfloat(rgba[3], ==, -0.25f);
        } else if (format >= 6) {
            g_assert_cmpfloat(rgba[0], ==, -0.25f);
            g_assert_cmpfloat(rgba[1], ==, -0.25f);
            g_assert_cmpfloat(rgba[2], ==, -0.25f);
            g_assert_cmpfloat(rgba[3], ==, format == 6 ? 1 : 2);
        }
    }
    for (unsigned k = 0; k < 4; k++) {
        uint32_t source[4];

        memcpy(source, floats, sizeof(source));
        source[k] = 0x7f800000;
        memcpy(rgba, before, sizeof(rgba));
        g_assert_false(sgi_kona_gl_pixel_rgba_float(5, 6, source, false, rgba));
        g_assert_cmpmem(rgba, sizeof(rgba), before, sizeof(before));
    }
}

static void test_pixel_double_conversion(void)
{
    const uint32_t source[] = { 0x7fffffff, 0, 0, 0xffffffff };
    double rgba[] = { 1, 1, 1, 0 };
    const double before[] = { 1, 1, 1, 0 };

    g_assert_true(sgi_kona_gl_pixel_rgba_double(5, 5, source, false, rgba));
    g_assert_cmpfloat(rgba[0], <, 0.5);
    g_assert_cmpfloat(rgba[0] * -4294967296.0 + 2147483648.0, ==, 0.5);
    g_assert_cmpfloat(rgba[3], ==, 1);
    memcpy(rgba, before, sizeof(rgba));
    g_assert_false(sgi_kona_gl_pixel_rgba_double(5, 5, NULL, false, rgba));
    g_assert_false(sgi_kona_gl_pixel_rgba_double(11, 5, source, false, rgba));
    g_assert_cmpmem(rgba, sizeof(rgba), before, sizeof(before));
}

static void test_texture_pixel_transfer_order(void)
{
    const uint32_t source[] = {
        0x3f000000, 0xbf800000, 0x40000000, 0x3f000000,
    };
    const float scale[] = { 0.999f, 1, 1, 0.999f };
    const float bias[] = { 0, 1, -1, 0 };
    uint32_t rgba = 0xa5a5a5a5;

    g_assert_true(sgi_kona_gl_texture_pixel_transfer(5, 6, source, false,
                                                     scale, bias, &rgba));
    g_assert_cmphex(rgba, ==, 0x7f00ff7f);
}

static void test_texture_pixel_transfer_expansion(void)
{
    const uint32_t source[] = { 0x3f000000, 0x3f000000, 0x3f000000 };
    const float scale[] = { 0.5f, 1, 2, 0.25f };
    const float bias[] = { 0, 0, 0, 0 };
    const float alpha_bias[] = { 0.25f, 0.5f, 1, 0 };
    uint32_t rgba;

    g_assert_true(sgi_kona_gl_texture_pixel_transfer(6, 6, source, false,
                                                     scale, bias, &rgba));
    g_assert_cmphex(rgba, ==, 0x4080ff40);
    g_assert_true(sgi_kona_gl_texture_pixel_transfer(4, 6, source, false,
                                                     scale, bias, &rgba));
    g_assert_cmphex(rgba, ==, 0x4080ff40);
    g_assert_true(sgi_kona_gl_texture_pixel_transfer(3, 6, source, false,
                                                     scale, alpha_bias, &rgba));
    g_assert_cmphex(rgba, ==, 0x4080ff20);
}

static void test_texture_pixel_transfer_refusals(void)
{
    uint32_t source[] = { 0, 0, 0, 0x3f800000 };
    float scale[] = { 1, 1, 1, 1 };
    float bias[] = { 0, 0, 0, 0 };
    uint32_t rgba = 0xa5a5a5a5;

    for (unsigned k = 0; k < 4; k++) {
        scale[k] = INFINITY;
        g_assert_false(sgi_kona_gl_texture_pixel_transfer(5, 6, source, false,
                                                          scale, bias, &rgba));
        scale[k] = 1;
        bias[k] = NAN;
        g_assert_false(sgi_kona_gl_texture_pixel_transfer(5, 6, source, false,
                                                          scale, bias, &rgba));
        bias[k] = 0;
    }
    source[3] = 0x7fc00000;
    g_assert_false(sgi_kona_gl_texture_pixel_transfer(5, 6, source, false,
                                                      scale, bias, &rgba));
    g_assert_false(sgi_kona_gl_texture_pixel_transfer(11, 6, source, false,
                                                      scale, bias, &rgba));
    g_assert_false(sgi_kona_gl_texture_pixel_transfer(5, 6, NULL, false,
                                                      scale, bias, &rgba));
    g_assert_false(sgi_kona_gl_texture_pixel_transfer(5, 6, source, false,
                                                      NULL, bias, &rgba));
    g_assert_cmphex(rgba, ==, 0xa5a5a5a5);
}

static void test_pixel_single_channel_decode(void)
{
    static const uint32_t full[] = {
        0x7f000000, 0xff000000, 0x7fff0000, 0xffff0000,
        0x7fffffff, 0xffffffff, 0x3f800000,
    };
    static const uint32_t sizes[] = { 1, 1, 2, 2, 4, 4, 4 };

    for (uint32_t format = 0; format < 3; format++) {
        for (uint32_t type = 0; type < ARRAY_SIZE(full); type++) {
            for (unsigned swap = 0; swap < 2; swap++) {
                uint32_t source = full[type];
                double rgba[4];

                if (swap && sizes[type] == 2) {
                    source = bswap16(source >> 16) << 16;
                } else if (swap && sizes[type] == 4) {
                    source = bswap32(source);
                }
                g_assert_cmpuint(sgi_kona_gl_texture_pixel_bytes(format, type),
                                 ==, sizes[type]);
                g_assert_true(sgi_kona_gl_pixel_rgba_double(format, type,
                                                            &source, swap,
                                                            rgba));
                for (unsigned k = 0; k < 4; k++) {
                    g_assert_cmpfloat(rgba[k], ==, k == format || k == 3);
                }
            }
        }
    }
}

static void test_pixel_single_channel_transfer(void)
{
    const uint32_t source = 0x7fffffff;
    const float scale[] = { -4294967296.0f, -4294967296.0f,
                           -4294967296.0f, 0.5f };
    const float bias[] = { 2147483648.0f, 2147483648.0f,
                          2147483648.0f, 0.25f };
    static const uint32_t expected[] = {
        0x80ffffbf, 0xff80ffbf, 0xffff80bf,
    };

    for (uint32_t format = 0; format < 3; format++) {
        uint32_t rgba;

        g_assert_true(sgi_kona_gl_texture_pixel_transfer(format, 5, &source,
                                                        false, scale, bias,
                                                        &rgba));
        g_assert_cmphex(rgba, ==, expected[format]);
    }
}

static void test_pixel_single_channel_pack(void)
{
    const double rgba[] = { 0.125, 0.5, 0.875, 0.25 };
    static const uint8_t expected[] = { 32, 128, 223 };
    uint8_t out[16], full[16];

    for (uint32_t format = 0; format < 3; format++) {
        for (uint32_t type = 0; type < 7; type++) {
            unsigned size = sgi_kona_gl_texture_pixel_bytes(3, type);

            for (unsigned swap = 0; swap < 2; swap++) {
                memset(out, 0xa5, sizeof(out));
                g_assert_true(sgi_kona_gl_pixel_pack_double(5, type, rgba,
                                                           swap, full,
                                                           sizeof(full)));
                g_assert_true(sgi_kona_gl_pixel_pack_double(format, type, rgba,
                                                           swap, out,
                                                           sizeof(out)));
                g_assert_cmpmem(out, size, full + format * size, size);
                for (unsigned k = size; k < sizeof(out); k++) {
                    g_assert_cmphex(out[k], ==, 0xa5);
                }
            }
        }
        g_assert_true(sgi_kona_gl_pixel_pack_double(format, 1, rgba, false,
                                                   out, sizeof(out)));
        g_assert_cmpuint(out[0], ==, expected[format]);
    }
}

static const struct {
    uint32_t type, bytes, bits;
    double rgba[4];
} packed_pixels[] = {
    { 8, 1, 0xa7, { 5.0 / 7, 1.0 / 7, 1, 1 } },
    { 9, 2, 0x1357, { 1.0 / 15, 3.0 / 15, 5.0 / 15, 7.0 / 15 } },
    { 10, 2, (17 << 11) | (9 << 6) | (3 << 1) | 1,
      { 17.0 / 31, 9.0 / 31, 3.0 / 31, 1 } },
    { 11, 4, 0x12345678, { 18.0 / 255, 52.0 / 255,
                          86.0 / 255, 120.0 / 255 } },
    { 12, 4, (513u << 22) | (257 << 12) | (129 << 2) | 2,
      { 513.0 / 1023, 257.0 / 1023, 129.0 / 1023, 2.0 / 3 } },
};

static void test_pixel_index_decode(void)
{
    static const struct {
        uint32_t type, bits;
        double value;
    } cases[] = {
        { 0, 0xff000000, -1 }, { 1, 0xa5000000, 165 },
        { 2, 0xffa50000, -91 }, { 3, 0xa5120000, 42258 },
        { 4, 0xffffffff, -1 }, { 5, 0xffffffff, 4294967295.0 },
        { 6, 0x3f000000, 0.5 }, { 6, 0xbf000000, -0.5 },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        for (unsigned swap = 0; swap < 2; swap++) {
            uint32_t source = cases[i].bits;
            double out = 123;

            if (swap && cases[i].type >= 2) {
                if (cases[i].type < 4) {
                    source = bswap16(source >> 16) << 16;
                } else {
                    source = bswap32(source);
                }
            }
            g_assert_true(sgi_kona_gl_pixel_index(cases[i].type, &source,
                                                   swap, &out));
            g_assert_cmpfloat(out, ==, cases[i].value);
        }
    }
    for (unsigned i = 0; i < 3; i++) {
        uint32_t source = i == 0 ? 0x7fc00000 :
                          i == 1 ? 0x7f800000 : 0xff800000;
        double out = 123;

        g_assert_false(sgi_kona_gl_pixel_index(6, &source, false, &out));
        g_assert_cmpfloat(out, ==, 123);
    }
}

static void test_pixel_index_transfer(void)
{
    static const struct {
        double value;
        int32_t shift, offset;
        uint32_t expected;
    } cases[] = {
        { 18, 2, 3, 75 }, { -1, -1, 0, 0x7fffffff },
        { 0.5, 1, 0, 1 }, { -0.5, 1, 0, 0xffffffff },
        { -0.5, 0, 1, 0 }, { 4294967295.0, 1, 0, 0xfffffffe },
        { 1, 64, 3, 3 }, { -1, -64, -1, 0xffffffff },
        { 1, INT32_MIN, 0, 0 }, { 1, INT32_MAX, 0, 0 },
    };
    uint32_t out = 123;

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        g_assert_true(sgi_kona_gl_pixel_index_transfer(cases[i].value,
            cases[i].shift, cases[i].offset, &out));
        g_assert_cmphex(out, ==, cases[i].expected);
    }
    g_assert_false(sgi_kona_gl_pixel_index_transfer(NAN, 0, 0, &out));
    g_assert_false(sgi_kona_gl_pixel_index_transfer(INFINITY, 0, 0, &out));
}

static void test_pixel_index_round(void)
{
    static const struct {
        double value;
        int32_t shift, offset;
        uint32_t expected;
    } cases[] = {
        { 0.25, 0, 0, 0 }, { 0.5, 0, 0, 1 }, { 2.75, 0, 0, 3 },
        { -0.75, 0, 0, UINT32_MAX }, { -0.5, 0, 0, 0 },
        { -1.75, 0, 0, UINT32_MAX - 1 }, { 0.25, 1, 0, 1 },
        { 1, -1, 0, 1 }, { 0.75, 1, 3, 5 },
        { 4294967295.75, 0, 0, 0 }, { -1, -1, 0, 0x80000000 },
    };
    uint32_t out;

    for (unsigned k = 0; k < ARRAY_SIZE(cases); k++) {
        g_assert_true(sgi_kona_gl_pixel_index_round(cases[k].value,
            cases[k].shift, cases[k].offset, &out));
        g_assert_cmphex(out, ==, cases[k].expected);
    }
    g_assert_false(sgi_kona_gl_pixel_index_round(NAN, 0, 0, &out));
    g_assert_false(sgi_kona_gl_pixel_index_round(INFINITY, 0, 0, &out));
    g_assert_false(sgi_kona_gl_pixel_index_round(1, 0, 0, NULL));
    for (unsigned type = 0; type <= 6; type++) {
        g_assert_cmpuint(sgi_kona_gl_native_pixel_bytes(11, type), ==,
                        type < 2 ? 1 : type < 4 ? 2 : 4);
    }
    g_assert_cmpuint(sgi_kona_gl_native_pixel_bytes(11, 7), ==, 0);
    g_assert_cmpuint(sgi_kona_gl_native_pixel_bytes(11, 8), ==, 0);
}

static void test_pixel_index_pack(void)
{
    static const uint32_t bits[] = {
        0x7f, 0xff, 0x7fff, 0xffff, 0x7fffffff, 0xffffffff, 0x4f800000,
    };

    for (uint32_t type = 0; type <= 6; type++) {
        unsigned bytes = type < 2 ? 1 : type < 4 ? 2 : 4;

        for (unsigned swap = 0; swap < 2; swap++) {
            uint8_t out[8], expected[4];

            memset(out, 0xa5, sizeof(out));
            for (unsigned k = 0; k < bytes; k++) {
                expected[k] = bits[type] >> (8 * (swap ? k : bytes - 1 - k));
            }
            g_assert_true(sgi_kona_gl_pixel_index_pack(type, UINT32_MAX,
                                                       swap, out,
                                                       sizeof(out)));
            g_assert_cmpmem(out, bytes, expected, bytes);
            g_assert_cmphex(out[bytes], ==, 0xa5);
            memset(out, 0xa5, sizeof(out));
            g_assert_false(sgi_kona_gl_pixel_index_pack(type, UINT32_MAX,
                                                       swap, out, bytes - 1));
            for (unsigned k = 0; k < sizeof(out); k++) {
                g_assert_cmphex(out[k], ==, 0xa5);
            }
        }
    }
}

static void test_pixel_packed_decode(void)
{
    for (unsigned n = 0; n < ARRAY_SIZE(packed_pixels); n++) {
        unsigned bytes = packed_pixels[n].bytes;
        unsigned type = packed_pixels[n].type;

        for (unsigned swap = 0; swap < 2; swap++) {
            for (unsigned bgr = 0; bgr < (type == 8 ? 1 : 2); bgr++) {
                unsigned format = type == 8 ? 4 : bgr ? 10 : 5;
                uint32_t source = packed_pixels[n].bits;
                double rgba[4];

                if (swap && bytes == 2) {
                    source = bswap16(source);
                } else if (swap && bytes == 4) {
                    source = bswap32(source);
                }
                source <<= 8 * (4 - bytes);
                g_assert_cmpuint(sgi_kona_gl_texture_pixel_bytes(format, type),
                                 ==, bytes);
                g_assert_true(sgi_kona_gl_pixel_rgba_double(format, type,
                                                           &source, swap,
                                                           rgba));
                for (unsigned k = 0; k < 4; k++) {
                    unsigned component = bgr && k < 3 ? 2 - k : k;

                    g_assert_cmpfloat(rgba[k], ==,
                                      packed_pixels[n].rgba[component]);
                }
            }
        }
    }
}

static void test_pixel_packed_pack(void)
{
    for (unsigned n = 0; n < ARRAY_SIZE(packed_pixels); n++) {
        unsigned bytes = packed_pixels[n].bytes;
        unsigned type = packed_pixels[n].type;

        for (unsigned swap = 0; swap < 2; swap++) {
            for (unsigned bgr = 0; bgr < (type == 8 ? 1 : 2); bgr++) {
                unsigned format = type == 8 ? 4 : bgr ? 10 : 5;
                uint8_t expected[4], out[8];
                double rgba[4];
                uint32_t bits = packed_pixels[n].bits;

                for (unsigned k = 0; k < 4; k++) {
                    rgba[k] = packed_pixels[n].rgba[bgr && k < 3 ? 2 - k : k];
                }
                for (unsigned k = 0; k < bytes; k++) {
                    unsigned shift = 8 * (swap ? k : bytes - 1 - k);

                    expected[k] = bits >> shift;
                }
                memset(out, 0xa5, sizeof(out));
                g_assert_true(sgi_kona_gl_pixel_pack_double(format, type,
                                                           rgba, swap, out,
                                                           sizeof(out)));
                g_assert_cmpmem(out, bytes, expected, bytes);
                for (unsigned k = bytes; k < sizeof(out); k++) {
                    g_assert_cmphex(out[k], ==, 0xa5);
                }
            }
        }
    }
}

static void test_pixel_packed_transfer(void)
{
    const uint32_t source = 0xa7000000;
    const float scale[] = { 0, 1, 1, 0.5 };
    const float bias[] = { 0.25, 0, 0, 0.25 };
    double rgba[] = { -1, 0.5, 2, 0.5 };
    uint32_t pixel;
    uint8_t out[8];

    g_assert_true(sgi_kona_gl_texture_pixel_transfer(4, 8, &source, false,
                                                     scale, bias, &pixel));
    g_assert_cmphex(pixel, ==, 0x4024ffbf);
    g_assert_true(sgi_kona_gl_pixel_pack_double(4, 8, rgba, false,
                                               out, sizeof(out)));
    g_assert_cmphex(out[0], ==, 0x13);
    g_assert_true(sgi_kona_gl_pixel_pack_double(5, 12, rgba, false,
                                               out, sizeof(out)));
    g_assert_cmphex(ldl_be_p(out), ==, (512 << 12) | (1023 << 2) | 2);
    for (unsigned k = 0; k < 4; k++) {
        double bad[4];

        memcpy(bad, rgba, sizeof(bad));
        bad[k] = k & 1 ? INFINITY : NAN;
        memset(out, 0xa5, sizeof(out));
        g_assert_false(sgi_kona_gl_pixel_pack_double(5, 12, bad, false,
                                                    out, sizeof(out)));
        for (unsigned c = 0; c < sizeof(out); c++) {
            g_assert_cmphex(out[c], ==, 0xa5);
        }
    }
}

static void test_pixel_packed_refusal(void)
{
    const double rgba[] = { 0.25, 0.5, 0.75, 1 };
    const uint32_t source = 0x12345678;
    double decoded[4];
    uint8_t out[8];

    for (unsigned n = 0; n < ARRAY_SIZE(packed_pixels); n++) {
        unsigned type = packed_pixels[n].type;
        unsigned format = type == 8 ? 4 : 5;
        unsigned capacity = packed_pixels[n].bytes - 1;

        memset(out, 0xa5, sizeof(out));
        g_assert_false(sgi_kona_gl_pixel_pack_double(format, type, rgba,
                                                    false, out,
                                                    capacity));
        for (unsigned k = 0; k < sizeof(out); k++) {
            g_assert_cmphex(out[k], ==, 0xa5);
        }
        for (unsigned f = 0; f <= 10; f++) {
            if (f == format || (type != 8 && f == 10)) {
                continue;
            }
            g_assert_cmpuint(sgi_kona_gl_texture_pixel_bytes(f, type), ==, 0);
            g_assert_false(sgi_kona_gl_pixel_rgba_double(f, type, &source,
                                                        false, decoded));
            g_assert_false(sgi_kona_gl_pixel_pack_double(f, type, rgba,
                                                        false, out,
                                                        sizeof(out)));
        }
    }
    g_assert_cmpuint(sgi_kona_gl_texture_pixel_bytes(5, 13), ==, 0);
    g_assert_cmpuint(sgi_kona_gl_texture_pixel_bytes(5, 7), ==, 0);
}

static void test_pixel_abgr_decode(void)
{
    const uint32_t integers[] = {
        0x10203040, 0x50607080, 0x90a0b0c0, 0xd0e0f000,
    };
    const uint32_t floats[] = {
        0x3e000000, 0x3f000000, 0x3f600000, 0x3e800000,
    };
    const uint32_t precise[] = {
        0x10000000, 0x20000000, 0x40000000, 0x7fffffff,
    };
    const float scale[] = { -4294967296.0f, 1, 1, 1 };
    const float bias[] = { 2147483648.0f, 0, 0, 0 };
    uint32_t pixel;

    for (uint32_t type = 0; type < 7; type++) {
        for (unsigned swap = 0; swap < 2; swap++) {
            uint32_t source[4];
            double original[4], rgba[4];
            unsigned size = sgi_kona_gl_texture_pixel_bytes(3, type);

            memcpy(source, type == 6 ? floats : integers, sizeof(source));
            if (type == 6 && swap) {
                for (unsigned k = 0; k < 4; k++) {
                    source[k] = bswap32(source[k]);
                }
            }
            g_assert_cmpuint(sgi_kona_gl_texture_pixel_bytes(8, type),
                             ==, 4 * size);
            g_assert_true(sgi_kona_gl_pixel_rgba_double(5, type, source,
                                                       swap, original));
            g_assert_true(sgi_kona_gl_pixel_rgba_double(8, type, source,
                                                       swap, rgba));
            for (unsigned k = 0; k < 4; k++) {
                g_assert_cmpfloat(rgba[k], ==, original[3 - k]);
            }
        }
    }
    g_assert_true(sgi_kona_gl_texture_pixel_rgba(8, 1, integers, false,
                                                 &pixel));
    g_assert_cmphex(pixel, ==, 0x40302010);
    g_assert_true(sgi_kona_gl_texture_pixel_transfer(8, 5, precise, false,
                                                     scale, bias, &pixel));
    g_assert_cmphex(pixel, ==, 0x80402010);
}

static void test_pixel_abgr_pack(void)
{
    const double rgba[] = { 0.125, 0.5, 0.875, 0.25 };
    const uint8_t bytes[] = { 64, 223, 128, 32 };
    uint8_t original[16], out[16];

    for (uint32_t type = 0; type < 7; type++) {
        unsigned size = sgi_kona_gl_texture_pixel_bytes(3, type);

        for (unsigned swap = 0; swap < 2; swap++) {
            g_assert_true(sgi_kona_gl_pixel_pack_double(5, type, rgba, swap,
                                                       original,
                                                       sizeof(original)));
            memset(out, 0xa5, sizeof(out));
            g_assert_true(sgi_kona_gl_pixel_pack_double(8, type, rgba, swap,
                                                       out, sizeof(out)));
            for (unsigned k = 0; k < 4; k++) {
                g_assert_cmpmem(out + k * size, size,
                                original + (3 - k) * size, size);
            }
            for (unsigned k = 4 * size; k < sizeof(out); k++) {
                g_assert_cmphex(out[k], ==, 0xa5);
            }
        }
    }
    g_assert_true(sgi_kona_gl_pixel_pack_double(8, 1, rgba, false,
                                               out, sizeof(out)));
    g_assert_cmpmem(out, sizeof(bytes), bytes, sizeof(bytes));
}

static void test_pixel_bgr_decode(void)
{
    const uint32_t integer_source[] = {
        0x10203040, 0x50607080, 0x90a0b0c0, 0xd0e0f000,
    };
    const uint32_t float_source[] = {
        0x3e000000, 0x3f000000, 0x3f600000, 0x3e800000,
    };
    static const unsigned components[] = { 3, 4 };

    for (uint32_t type = 0; type < 7; type++) {
        const uint32_t *source = type == 6 ? float_source : integer_source;

        for (unsigned swap = 0; swap < (type == 6 ? 1 : 2); swap++) {
            double original[4];

            g_assert_true(sgi_kona_gl_pixel_rgba_double(5, type, source,
                                                       swap, original));
            for (uint32_t format = 9; format <= 10; format++) {
                double rgba[4];
                unsigned size = sgi_kona_gl_texture_pixel_bytes(3, type);

                g_assert_cmpuint(sgi_kona_gl_texture_pixel_bytes(format, type),
                                 ==, components[format - 9] * size);
                g_assert_true(sgi_kona_gl_pixel_rgba_double(format, type,
                                                           source, swap,
                                                           rgba));
                g_assert_cmpfloat(rgba[0], ==, original[2]);
                g_assert_cmpfloat(rgba[1], ==, original[1]);
                g_assert_cmpfloat(rgba[2], ==, original[0]);
                g_assert_cmpfloat(rgba[3], ==, format == 9 ? 1 : original[3]);
            }
        }
    }
}

static void test_pixel_bgr_pack(void)
{
    const double rgba[] = { 0.125, 0.5, 0.875, 0.25 };
    static const unsigned order[] = { 2, 1, 0, 3 };
    uint8_t full[16], out[16];

    for (uint32_t type = 0; type < 7; type++) {
        unsigned size = sgi_kona_gl_texture_pixel_bytes(3, type);

        for (unsigned swap = 0; swap < 2; swap++) {
            g_assert_true(sgi_kona_gl_pixel_pack_double(5, type, rgba, swap,
                                                       full, sizeof(full)));
            for (uint32_t format = 9; format <= 10; format++) {
                unsigned count = format == 9 ? 3 : 4;

                memset(out, 0xa5, sizeof(out));
                g_assert_true(sgi_kona_gl_pixel_pack_double(format, type,
                                                           rgba, swap,
                                                           out, sizeof(out)));
                for (unsigned k = 0; k < count; k++) {
                    g_assert_cmpmem(out + k * size, size,
                                    full + order[k] * size, size);
                }
                for (unsigned k = count * size; k < sizeof(out); k++) {
                    g_assert_cmphex(out[k], ==, 0xa5);
                }
            }
        }
    }
}

static void test_pixel_pack_types(void)
{
    const float rgba[4] = { 0, 0.5f, 1, 0.25f };
    const uint8_t bytes[] = { 0, 0x80, 0xff, 0x40 };
    const uint8_t shorts[] = { 0, 0, 0x80, 0, 0xff, 0xff, 0x40, 0 };
    const uint8_t floats[] = {
        0, 0, 0, 0, 0x3f, 0, 0, 0,
        0x3f, 0x80, 0, 0, 0x3e, 0x80, 0, 0,
    };
    const uint8_t integers[] = {
        0, 0, 0, 0, 0x80, 0, 0, 0,
        0xff, 0xff, 0xff, 0xff, 0x40, 0, 0, 0,
    };
    const uint8_t signed_bytes[] = { 0, 0x3f, 0x7f, 0x1f };
    const uint8_t signed_shorts[] = {
        0, 0, 0x3f, 0xff, 0x7f, 0xff, 0x1f, 0xff,
    };
    const uint8_t signed_ints[] = {
        0, 0, 0, 0, 0x3f, 0xff, 0xff, 0xff,
        0x7f, 0xff, 0xff, 0xff, 0x1f, 0xff, 0xff, 0xff,
    };
    const uint8_t *expected[] = {
        signed_bytes, bytes, signed_shorts, shorts,
        signed_ints, integers, floats,
    };
    const uint32_t types[] = { 0, 1, 2, 3, 4, 5, 6 };
    const unsigned sizes[] = { 1, 1, 2, 2, 4, 4, 4 };

    for (uint32_t format = 4; format <= 5; format++) {
        for (unsigned k = 0; k < ARRAY_SIZE(types); k++) {
            uint8_t out[17];
            unsigned count = (format == 4 ? 3 : 4) * sizes[k];

            memset(out, 0xa5, sizeof(out));
            g_assert_true(sgi_kona_gl_pixel_pack(format, types[k], rgba,
                                                false, out, count));
            g_assert_cmpmem(out, count, expected[k], count);
            for (unsigned i = count; i < sizeof(out); i++) {
                g_assert_cmphex(out[i], ==, 0xa5);
            }
        }
    }
}

static void test_pixel_pack_expansion_and_swap(void)
{
    float rgba[4] = { 0, 0, 0, 1 };
    uint8_t out[16];
    const uint8_t swapped_short[] = { 0, 0x80, 0, 0, 0, 0, 0xff, 0xff };
    const uint8_t swapped_int[] = {
        0, 0, 0, 0x80, 0, 0, 0, 0,
        0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff,
    };
    const uint8_t swapped_signed_short[] = {
        0xff, 0x3f, 0, 0, 0, 0, 0xff, 0x7f,
    };
    const uint8_t swapped_signed_int[] = {
        0xff, 0xff, 0xff, 0x3f, 0, 0, 0, 0,
        0, 0, 0, 0, 0xff, 0xff, 0xff, 0x7f,
    };
    const uint8_t swapped_float[] = {
        0, 0, 0, 0x3f, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0x80, 0x3f,
    };

    for (unsigned v = 0; v < 256; v++) {
        rgba[0] = v / 255.0f;
        g_assert_true(sgi_kona_gl_pixel_pack(5, 3, rgba, false,
                                            out, sizeof(out)));
        g_assert_cmpuint(((uint32_t)out[0] << 8) | out[1], ==, v * 257);
        g_assert_cmphex(out[6], ==, 0xff);
        g_assert_cmphex(out[7], ==, 0xff);
    }
    rgba[0] = 0.5f;
    g_assert_true(sgi_kona_gl_pixel_pack(5, 3, rgba, true, out, sizeof(out)));
    g_assert_cmpmem(out, sizeof(swapped_short), swapped_short,
                    sizeof(swapped_short));
    g_assert_true(sgi_kona_gl_pixel_pack(5, 2, rgba, true, out, sizeof(out)));
    g_assert_cmpmem(out, sizeof(swapped_signed_short), swapped_signed_short,
                    sizeof(swapped_signed_short));
    g_assert_true(sgi_kona_gl_pixel_pack(5, 4, rgba, true, out, sizeof(out)));
    g_assert_cmpmem(out, sizeof(swapped_signed_int), swapped_signed_int,
                    sizeof(swapped_signed_int));
    g_assert_true(sgi_kona_gl_pixel_pack(5, 5, rgba, true, out, sizeof(out)));
    g_assert_cmpmem(out, sizeof(swapped_int), swapped_int, sizeof(swapped_int));
    g_assert_true(sgi_kona_gl_pixel_pack(5, 6, rgba, true, out, sizeof(out)));
    g_assert_cmpmem(out, sizeof(swapped_float), swapped_float,
                    sizeof(swapped_float));
}

static void test_pixel_pack_channels(void)
{
    const float rgba[4] = { -1, 0.75f, 0.75f, 0.25f };
    const uint8_t expected[][8] = {
        { 0x3e, 0x80, 0, 0 },
        { 0x3f, 0, 0, 0 },
        { 0x3f, 0, 0, 0, 0x3e, 0x80, 0, 0 },
    };
    const uint32_t formats[] = { 3, 6, 7 };

    for (unsigned k = 0; k < ARRAY_SIZE(formats); k++) {
        uint8_t out[9];
        unsigned count = k == 2 ? 8 : 4;

        memset(out, 0xa5, sizeof(out));
        g_assert_true(sgi_kona_gl_pixel_pack(formats[k], 6, rgba, false,
                                            out, count));
        g_assert_cmpmem(out, count, expected[k], count);
        g_assert_cmphex(out[count], ==, 0xa5);
    }
}

static void test_pixel_pack_double_precision(void)
{
    double rgba[] = { 0, 0, 0, 1 };
    uint8_t out[16];

    for (uint32_t v = 0; v < 256; v++) {
        uint32_t actual;

        rgba[0] = v / 255.0;
        g_assert_true(sgi_kona_gl_pixel_pack_double(5, 5, rgba, false,
                                                    out, sizeof(out)));
        actual = ((uint32_t)out[0] << 24) | ((uint32_t)out[1] << 16) |
                 ((uint32_t)out[2] << 8) | out[3];
        g_assert_cmpuint(actual, ==, v * 16843009u);
    }
    rgba[0] = 1e76;
    rgba[1] = -1e76;
    rgba[2] = 0.5;
    g_assert_true(sgi_kona_gl_pixel_pack_double(6, 6, rgba, false,
                                                out, sizeof(out)));
    g_assert_cmphex(out[0], ==, 0x3f);
    g_assert_cmphex(out[1], ==, 0);
    g_assert_cmphex(out[2], ==, 0);
    g_assert_cmphex(out[3], ==, 0);
}

static void test_pixel_pack_refusals(void)
{
    float rgba[4] = { 0, 0, 0, 1 };
    const uint8_t before[16] = { 0xa5 };
    uint8_t out[16];

    memcpy(out, before, sizeof(out));
    g_assert_false(sgi_kona_gl_pixel_pack(11, 1, rgba, false,
                                         out, sizeof(out)));
    g_assert_false(sgi_kona_gl_pixel_pack(5, 7, rgba, false, out, sizeof(out)));
    g_assert_false(sgi_kona_gl_pixel_pack(5, 3, rgba, false, out, 7));
    for (unsigned k = 0; k < 4; k++) {
        rgba[k] = INFINITY;
        g_assert_false(sgi_kona_gl_pixel_pack(5, 6, rgba, false,
                                             out, sizeof(out)));
        rgba[k] = k == 3;
    }
    g_assert_cmpmem(out, sizeof(out), before, sizeof(before));
    rgba[0] = -1;
    rgba[1] = 2;
    g_assert_true(sgi_kona_gl_pixel_pack(4, 1, rgba, false, out, sizeof(out)));
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmphex(out[1], ==, 0xff);
    g_assert_cmphex(out[2], ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-kona-gl-decode/texture-shadow-sampler",
                    test_texture_shadow_sampler);
    g_test_add_func("/sgi-kona-gl-decode/texture-depth-pixels",
                    test_texture_pixel_depth);
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
    g_test_add_func("/sgi-kona-gl-decode/copy-pixels-framing",
                    test_copy_pixels_framing);
    g_test_add_func("/sgi-kona-gl-decode/copy-query-framing",
                    test_copy_query_framing);
    g_test_add_func("/sgi-kona-gl-decode/read-pixels-framing",
                    test_read_pixels_framing);
    g_test_add_func("/sgi-kona-gl-decode/result-names", test_result_names);
    g_test_add_func("/sgi-kona-gl-decode/query-inventory",
                    test_query_inventory);
    g_test_add_func("/sgi-kona-gl-decode/query-native-bytes",
                    test_query_native_bytes);
    g_test_add_func("/sgi-kona-gl-decode/query-word-bytes",
                    test_query_word_bytes);
    g_test_add_func("/sgi-kona-gl-decode/texture-transform",
                    test_texture_transform);
    g_test_add_func("/sgi-kona-gl-decode/texture-sampler",
                    test_texture_sampler);
    g_test_add_func("/sgi-kona-gl-decode/texture-dimensions",
                    test_texture_dimensions);
    g_test_add_func("/sgi-kona-gl-decode/texture-sharpen-sampler",
                    test_texture_sharpen_sampler);
    g_test_add_func("/sgi-kona-gl-decode/texture-detail-sampler",
                    test_texture_detail_sampler);
    g_test_add_func("/sgi-kona-gl-decode/texture-volume-sampler",
                    test_texture_volume_sampler);
    g_test_add_func("/sgi-kona-gl-decode/texture-sampler-1d",
                    test_texture_sampler_1d);
    g_test_add_func("/sgi-kona-gl-decode/texture-native-layout",
                    test_texture_native_layout);
    g_test_add_func("/sgi-kona-gl-decode/texture-volume-layout",
                    test_texture_volume_layout);
    g_test_add_func("/sgi-kona-gl-decode/texture-comparison-flag",
                    test_texture_comparison_flag);
    g_test_add_func("/sgi-kona-gl-decode/texture-binding-layout",
                    test_texture_binding_layout);
    g_test_add_func("/sgi-kona-gl-decode/texture-negative-lod",
                    test_texture_negative_lod);
    g_test_add_func("/sgi-kona-gl-decode/texture-pixel-conversion",
                    test_texture_pixel_conversion);
    g_test_add_func("/sgi-kona-gl-decode/texture-format-storage",
                    test_texture_format_storage);
    g_test_add_func("/sgi-kona-gl-decode/texture-internal-format",
                    test_texture_internal_format);
    g_test_add_func("/sgi-kona-gl-decode/texture-integer-conversion",
                    test_texture_integer_conversion);
    g_test_add_func("/sgi-kona-gl-decode/texture-swap-bytes",
                    test_texture_swap_bytes);
    g_test_add_func("/sgi-kona-gl-decode/texture-filter-curve/values",
                    test_texture_filter_curve);
    g_test_add_func("/sgi-kona-gl-decode/texture-filter-curve/refusal",
                    test_texture_filter_curve_refusal);
    g_test_add_func("/sgi-kona-gl-decode/bitmap-pio/contiguous",
                    test_bitmap_pio_contiguous);
    g_test_add_func("/sgi-kona-gl-decode/bitmap-pio/skips",
                    test_bitmap_pio_skips);
    g_test_add_func("/sgi-kona-gl-decode/bitmap-pio/row-padding",
                    test_bitmap_pio_row_padding);
    g_test_add_func("/sgi-kona-gl-decode/bitmap-pio/bit-order",
                    test_bitmap_pio_bit_order);
    g_test_add_func("/sgi-kona-gl-decode/bitmap-pio/refusals",
                    test_bitmap_pio_refusals);
    g_test_add_func("/sgi-kona-gl-decode/bitmap-pio/skip-rows-phase",
                    test_bitmap_pio_skip_rows_phase);
    g_test_add_func("/sgi-kona-gl-decode/bitmap-pio/large-counts",
                    test_bitmap_pio_large_counts);
    g_test_add_func("/sgi-kona-gl-decode/bitmap-dma/contiguous-padding",
                    test_bitmap_dma_contiguous_padding);
    g_test_add_func("/sgi-kona-gl-decode/bitmap-dma/strided-bit-skips",
                    test_bitmap_dma_strided_bit_skips);
    g_test_add_func("/sgi-kona-gl-decode/bitmap-dma/partial-widths",
                    test_bitmap_dma_partial_widths);
    g_test_add_func("/sgi-kona-gl-decode/bitmap-dma/refusals",
                    test_bitmap_dma_refusals);
    g_test_add_func("/sgi-kona-gl-decode/bitmap-dma/large-span",
                    test_bitmap_dma_large_span);
    g_test_add_func("/sgi-kona-gl-decode/bitmap-pio/offset-refusals",
                    test_bitmap_pio_offset_refusals);
    g_test_add_func("/sgi-kona-gl-decode/pixels-pio/contiguous-padding",
                    test_pixels_pio_contiguous_padding);
    g_test_add_func("/sgi-kona-gl-decode/pixels-pio/strided-phases",
                    test_pixels_pio_strided_phases);
    g_test_add_func("/sgi-kona-gl-decode/pixels-pio/skips",
                    test_pixels_pio_skips);
    g_test_add_func("/sgi-kona-gl-decode/pixels-pio/scalar-widths",
                    test_pixels_pio_scalar_widths);
    g_test_add_func("/sgi-kona-gl-decode/pixels-pio/refusals",
                    test_pixels_pio_refusals);
    g_test_add_func("/sgi-kona-gl-decode/pixels-bitmap-layout",
                    test_pixels_bitmap_layout);
    g_test_add_func("/sgi-kona-gl-decode/pixels-dma/contiguous-padding",
                    test_pixels_dma_contiguous_padding);
    g_test_add_func("/sgi-kona-gl-decode/pixels-dma/strided-skips",
                    test_pixels_dma_strided_skips);
    g_test_add_func("/sgi-kona-gl-decode/pixels-dma/scalar-widths",
                    test_pixels_dma_scalar_widths);
    g_test_add_func("/sgi-kona-gl-decode/pixels-dma/refusals",
                    test_pixels_dma_refusals);
    g_test_add_func("/sgi-kona-gl-decode/pixels-dma/large-span",
                    test_pixels_dma_large_span);
    g_test_add_func("/sgi-kona-gl-decode/pixels-pio/offset-refusals",
                    test_pixels_pio_offset_refusals);
    g_test_add_func("/sgi-kona-gl-decode/pixel-float-conversion",
                    test_pixel_float_conversion);
    g_test_add_func("/sgi-kona-gl-decode/pixel-double-conversion",
                    test_pixel_double_conversion);
    g_test_add_func("/sgi-kona-gl-decode/texture-pixel-transfer/order",
                    test_texture_pixel_transfer_order);
    g_test_add_func("/sgi-kona-gl-decode/texture-pixel-transfer/expansion",
                    test_texture_pixel_transfer_expansion);
    g_test_add_func("/sgi-kona-gl-decode/texture-pixel-transfer/refusals",
                    test_texture_pixel_transfer_refusals);
    g_test_add_func("/sgi-kona-gl-decode/single-channel/decode",
                    test_pixel_single_channel_decode);
    g_test_add_func("/sgi-kona-gl-decode/single-channel/transfer",
                    test_pixel_single_channel_transfer);
    g_test_add_func("/sgi-kona-gl-decode/single-channel/pack",
                    test_pixel_single_channel_pack);
    g_test_add_func("/sgi-kona-gl-decode/abgr/decode", test_pixel_abgr_decode);
    g_test_add_func("/sgi-kona-gl-decode/abgr/pack", test_pixel_abgr_pack);
    g_test_add_func("/sgi-kona-gl-decode/bgr/decode", test_pixel_bgr_decode);
    g_test_add_func("/sgi-kona-gl-decode/bgr/pack", test_pixel_bgr_pack);
    g_test_add_func("/sgi-kona-gl-decode/pixel-pack/types",
                    test_pixel_pack_types);
    g_test_add_func("/sgi-kona-gl-decode/pixel-pack/expansion-and-swap",
                    test_pixel_pack_expansion_and_swap);
    g_test_add_func("/sgi-kona-gl-decode/pixel-pack/channels",
                    test_pixel_pack_channels);
    g_test_add_func("/sgi-kona-gl-decode/pixel-pack/double-precision",
                    test_pixel_pack_double_precision);
    g_test_add_func("/sgi-kona-gl-decode/pixel-pack/refusals",
                    test_pixel_pack_refusals);
    g_test_add_func("/sgi-kona-gl-decode/index/decode",
                    test_pixel_index_decode);
    g_test_add_func("/sgi-kona-gl-decode/index/transfer",
                    test_pixel_index_transfer);
    g_test_add_func("/sgi-kona-gl-decode/index/round", test_pixel_index_round);
    g_test_add_func("/sgi-kona-gl-decode/index/pack", test_pixel_index_pack);
    g_test_add_func("/sgi-kona-gl-decode/packed/decode",
                    test_pixel_packed_decode);
    g_test_add_func("/sgi-kona-gl-decode/packed/pack",
                    test_pixel_packed_pack);
    g_test_add_func("/sgi-kona-gl-decode/packed/transfer",
                    test_pixel_packed_transfer);
    g_test_add_func("/sgi-kona-gl-decode/packed/refusal",
                    test_pixel_packed_refusal);
    return g_test_run();
}
