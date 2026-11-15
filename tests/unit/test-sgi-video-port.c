/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI video frames: RGB conversion against hand-computed values, the memory
 * packings against the manual's bit diagrams, and the Y4M and WAV headers.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "hw/display/sgi/video-port.h"

static SGIVideoFormat fmt(unsigned w, unsigned h, unsigned depth)
{
    return (SGIVideoFormat){ .width = w, .height = h, .rate_num = 30000,
                             .rate_den = 1001, .depth = depth };
}

static uint32_t rgb_of(unsigned w, unsigned y, unsigned cb, unsigned cr)
{
    SGIVideoFormat f = fmt(w, 2, 8);
    SGIVideoFrame *fr = sgi_video_frame_new(&f);
    uint32_t row[1920];

    fr->y[0] = y;
    fr->cb[0] = cb;
    fr->cr[0] = cr;
    sgi_video_row_rgb(fr, 0, row);
    sgi_video_frame_free(fr);
    return row[0];
}

/* Limited range: Y 16 is black, 235 white; each matrix on a few colours. */
static void test_rgb(void)
{
    g_assert_cmphex(rgb_of(720, 235, 128, 128), ==, 0xffffff);
    g_assert_cmphex(rgb_of(720, 16, 128, 128), ==, 0x000000);
    g_assert_cmphex(rgb_of(720, 126, 128, 128), ==, 0x808080);
    /* Below black and above white clamp. */
    g_assert_cmphex(rgb_of(720, 0, 128, 128), ==, 0x000000);
    g_assert_cmphex(rgb_of(720, 255, 128, 128), ==, 0xffffff);
    /* BT.601 for standard definition. */
    g_assert_cmphex(rgb_of(720, 145, 54, 34), ==, 0x00ff01);
    g_assert_cmphex(rgb_of(720, 41, 240, 110), ==, 0x0000ff);
    g_assert_cmphex(rgb_of(720, 81, 90, 240), ==, 0xff0000);
    /* BT.709 for high definition: the same samples, other colours. */
    g_assert_cmphex(rgb_of(1920, 145, 54, 34), ==, 0x00d800);
    g_assert_cmphex(rgb_of(1920, 41, 240, 110), ==, 0x000fff);
    g_assert_cmphex(rgb_of(1920, 81, 90, 240), ==, 0xff1800);
}

/* A pixel pair shares its chroma; chroma rows are per line. */
static void test_chroma_repeat(void)
{
    SGIVideoFormat f = fmt(4, 2, 8);
    SGIVideoFrame *fr = sgi_video_frame_new(&f);
    uint32_t row[4];

    for (int i = 0; i < 8; i++) {
        fr->y[i] = 126;
    }
    fr->cr[0] = 240;    /* line 0, pair 0 only */
    sgi_video_row_rgb(fr, 0, row);
    g_assert_cmphex(row[0], ==, row[1]);
    g_assert_cmphex(row[2], ==, 0x808080);
    g_assert_cmphex(row[0], !=, 0x808080);
    sgi_video_row_rgb(fr, 1, row);
    g_assert_cmphex(row[0], ==, 0x808080);
    sgi_video_frame_free(fr);
}

/* One pair, Cb 0x3ab, Y 0x155, Cr 0x2cd, Y 0x0ff, as the diagrams lay it. */
static void test_packing_bytes(void)
{
    static const uint8_t p8[] = { 0xea, 0x55, 0xb3, 0x3f };
    static const uint8_t p10[] = { 0xea, 0xd5, 0x5b, 0x34, 0xff };
    static const uint8_t left[] = { 0xea, 0xfa, 0x55, 0x55, 0xb3, 0x6c, 0x3f,
                                    0xcf };
    static const uint8_t right[] = { 0x03, 0xab, 0x01, 0x55, 0x02, 0xcd, 0x00,
                                     0xff };
    static const struct {
        SGIVideoPacking p;
        const uint8_t *bytes;
        size_t len;
    } cases[] = {
        { SGI_PACK_10, p10, sizeof(p10) },
        { SGI_PACK_10IN16L, left, sizeof(left) },
        { SGI_PACK_10IN16R, right, sizeof(right) },
    };
    SGIVideoFormat f10 = fmt(2, 1, 10), f8 = fmt(2, 1, 8);
    SGIVideoFrame *fr = sgi_video_frame_new(&f10);
    SGIVideoFrame *f8r = sgi_video_frame_new(&f8);
    uint8_t out[8], in8[4];

    fr->cb[0] = 0x3ab;
    fr->y[0] = 0x155;
    fr->cr[0] = 0x2cd;
    fr->y[1] = 0x0ff;
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        g_assert_cmpuint(sgi_video_packed_row_bytes(cases[i].p, 2), ==,
                         cases[i].len);
        memset(out, 0, sizeof(out));
        sgi_video_pack_row(cases[i].p, fr, 0, out);
        g_assert_cmpmem(out, cases[i].len, cases[i].bytes, cases[i].len);
    }
    /* The 8-bit packing of a 10-bit frame drops the two low bits. */
    sgi_video_pack_row(SGI_PACK_8, fr, 0, out);
    g_assert_cmpmem(out, sizeof(p8), p8, sizeof(p8));
    /* An 8-bit frame packed at 10 bits shifts up. */
    memcpy(in8, p8, sizeof(in8));
    sgi_video_unpack_row(SGI_PACK_8, in8, f8r, 0);
    g_assert_cmpuint(f8r->y[0], ==, 0x55);
    sgi_video_pack_row(SGI_PACK_10IN16R, f8r, 0, out);
    g_assert_cmpuint(lduw_be_p(out + 2), ==, 0x55 << 2);
    sgi_video_frame_free(fr);
    sgi_video_frame_free(f8r);
}

static void test_packing_round_trip(void)
{
    for (int p = 0; p < SGI_PACK_COUNT; p++) {
        SGIVideoFormat f = fmt(8, 3, sgi_video_packing_depth(p));
        SGIVideoFrame *a = sgi_video_frame_new(&f);
        SGIVideoFrame *b = sgi_video_frame_new(&f);
        size_t row = sgi_video_packed_row_bytes(p, 8);
        g_autofree uint8_t *mem = g_malloc(row * 3);

        for (unsigned i = 0; i < 8 * 3; i++) {
            a->y[i] = g_test_rand_int_range(0, 1 << f.depth);
        }
        for (unsigned i = 0; i < 4 * 3; i++) {
            a->cb[i] = g_test_rand_int_range(0, 1 << f.depth);
            a->cr[i] = g_test_rand_int_range(0, 1 << f.depth);
        }
        for (unsigned y = 0; y < 3; y++) {
            sgi_video_pack_row(p, a, y, mem + y * row);
        }
        for (unsigned y = 0; y < 3; y++) {
            sgi_video_unpack_row(p, mem + y * row, b, y);
        }
        g_assert_cmpmem(a->y, 8 * 3 * 2, b->y, 8 * 3 * 2);
        g_assert_cmpmem(a->cb, 4 * 3 * 2, b->cb, 4 * 3 * 2);
        g_assert_cmpmem(a->cr, 4 * 3 * 2, b->cr, 4 * 3 * 2);
        g_assert_cmpint(sgi_video_packing_parse(sgi_video_packing_name(p)), ==,
                        p);
        sgi_video_frame_free(a);
        sgi_video_frame_free(b);
    }
    g_assert_cmpint(sgi_video_packing_parse("3214"), ==, -1);
}

static void test_y4m_header(void)
{
    static const struct {
        unsigned w, h, num, den, depth;
        SGIVideoFields fields;
        const char *line;
    } cases[] = {
        { 720, 486, 30000, 1001, 10, SGI_VIDEO_TFF,
          "YUV4MPEG2 W720 H486 F30000:1001 It A0:0 C422p10\n" },
        { 720, 576, 25, 1, 8, SGI_VIDEO_BFF,
          "YUV4MPEG2 W720 H576 F25:1 Ib A0:0 C422\n" },
        { 1920, 1080, 24000, 1001, 10, SGI_VIDEO_PROGRESSIVE,
          "YUV4MPEG2 W1920 H1080 F24000:1001 Ip A0:0 C422p10\n" },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        SGIVideoFormat f = fmt(cases[i].w, cases[i].h, cases[i].depth), g;
        g_autofree char *line = NULL;

        f.rate_num = cases[i].num;
        f.rate_den = cases[i].den;
        f.fields = cases[i].fields;
        line = sgi_y4m_header(&f);
        g_assert_cmpstr(line, ==, cases[i].line);
        g_assert_true(sgi_y4m_parse_header(line, &g, &error_abort));
        g_assert_cmpmem(&f, sizeof(f), &g, sizeof(g));
    }
}

static void test_y4m_parse_errors(void)
{
    static const char *const bad[] = {
        "RIFF W720 H486 F30:1 Ip C422\n",
        "YUV4MPEG2 W720 H486 F30:1 Ip C420jpeg\n",
        "YUV4MPEG2 W720 H486 F30:1 Ip\n",
        "YUV4MPEG2 W721 H486 F30:1 Ip C422\n",
        "YUV4MPEG2 W720 H486 F0:1 Ip C422\n",
    };
    SGIVideoFormat f;

    for (unsigned i = 0; i < ARRAY_SIZE(bad); i++) {
        Error *err = NULL;

        g_assert_false(sgi_y4m_parse_header(bad[i], &f, &err));
        g_assert_nonnull(err);
        error_free(err);
    }
    /* Unknown tags, such as an X comment, are ignored. */
    g_assert_true(sgi_y4m_parse_header(
        "YUV4MPEG2 W720 H486 F30:1 Ip C422 XYSCSS=ST124\n", &f, &error_abort));
}

static void test_y4m_frame(void)
{
    SGIVideoFormat f10 = fmt(2, 1, 10), f8 = fmt(2, 1, 8);
    SGIVideoFrame *a = sgi_video_frame_new(&f10), *b = sgi_video_frame_new(&f8);
    uint8_t out[6 + 8];
    static const uint8_t want10[] = { 'F', 'R', 'A', 'M', 'E', '\n',
                                      0x55, 0x01, 0xff, 0x00,
                                      0xab, 0x03, 0xcd, 0x02 };

    a->y[0] = 0x155;
    a->y[1] = 0xff;
    a->cb[0] = 0x3ab;
    a->cr[0] = 0x2cd;
    g_assert_cmpuint(sgi_y4m_frame_put(a, out), ==, sizeof(want10));
    g_assert_cmpuint(sgi_video_frame_bytes(&f10), ==, 8);
    g_assert_cmpmem(out, sizeof(want10), want10, sizeof(want10));
    b->y[0] = 1;
    b->y[1] = 2;
    b->cb[0] = 3;
    b->cr[0] = 4;
    g_assert_cmpuint(sgi_y4m_frame_put(b, out), ==, 6 + 4);
    g_assert_cmpuint(out[6], ==, 1);
    g_assert_cmpuint(out[7], ==, 2);
    g_assert_cmpuint(out[8], ==, 3);
    g_assert_cmpuint(out[9], ==, 4);
    sgi_video_frame_free(a);
    sgi_video_frame_free(b);
}

static void test_wav_header(void)
{
    uint8_t h[SGI_WAV_HEADER_BYTES];

    sgi_wav_header(h, 48000, 2, 1000);
    g_assert_cmpmem(h, 4, "RIFF", 4);
    g_assert_cmpuint(ldl_le_p(h + 4), ==, 1036);
    g_assert_cmpmem(h + 8, 8, "WAVEfmt ", 8);
    g_assert_cmpuint(lduw_le_p(h + 20), ==, 1);
    g_assert_cmpuint(lduw_le_p(h + 22), ==, 2);
    g_assert_cmpuint(ldl_le_p(h + 24), ==, 48000);
    g_assert_cmpuint(ldl_le_p(h + 28), ==, 192000);
    g_assert_cmpuint(lduw_le_p(h + 32), ==, 4);
    g_assert_cmpuint(lduw_le_p(h + 34), ==, 16);
    g_assert_cmpmem(h + 36, 4, "data", 4);
    g_assert_cmpuint(ldl_le_p(h + 40), ==, 1000);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-video-port/rgb", test_rgb);
    g_test_add_func("/sgi-video-port/chroma-repeat", test_chroma_repeat);
    g_test_add_func("/sgi-video-port/packing-bytes", test_packing_bytes);
    g_test_add_func("/sgi-video-port/packing-round-trip",
                    test_packing_round_trip);
    g_test_add_func("/sgi-video-port/y4m-header", test_y4m_header);
    g_test_add_func("/sgi-video-port/y4m-parse-errors", test_y4m_parse_errors);
    g_test_add_func("/sgi-video-port/y4m-frame", test_y4m_frame);
    g_test_add_func("/sgi-video-port/wav-header", test_wav_header);
    return g_test_run();
}
