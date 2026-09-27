/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Framing of the InfiniteReality OpenGL doorbell decoder. The expected
 * layouts below are written out by hand from the command grammar
 * datasheet's 3D-milestone subset and the catalogue rows it cites, not
 * read back from the generated table, so a regenerated table that
 * changes a count fails here.
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
    return g_test_run();
}
