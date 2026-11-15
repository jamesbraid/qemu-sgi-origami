/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI video frames: RGB conversion against hand-computed values, the memory
 * packings against the page's layouts, and the Y4M header and frame.
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

/*
 * One pair, Cb 0x3ab, Y 0x155, Cr 0x2cd, Y 0x0ff, as the bytes the DM2
 * page's Payload section lays out. The 10in16l pad bits repeat the sample's
 * top six bits, the reader rule of the HD I/O guide's diagram conventions;
 * a writer may leave them as garbage, so the unpacker must ignore them.
 */
static void test_unpack_bytes(void)
{
    static const uint8_t p8[] = { 0xea, 0x55, 0xb3, 0x3f };
    static const uint8_t p10[] = { 0xea, 0xd5, 0x5b, 0x34, 0xff };
    static const uint8_t left[] = { 0xea, 0xfa, 0x55, 0x55, 0xb3, 0x6c, 0x3f,
                                    0xcf };
    static const uint8_t left_garbage[] = { 0xea, 0xea, 0x55, 0x40, 0xb3,
                                            0x7f, 0x3f, 0xc1 };
    static const uint8_t right[] = { 0x03, 0xab, 0x01, 0x55, 0x02, 0xcd, 0x00,
                                     0xff };
    static const struct {
        SGIVideoPacking p;
        const uint8_t *bytes;
        size_t len;
        unsigned depth;
    } cases[] = {
        { SGI_PACK_8, p8, sizeof(p8), 8 },
        { SGI_PACK_10, p10, sizeof(p10), 10 },
        { SGI_PACK_10IN16L, left, sizeof(left), 10 },
        { SGI_PACK_10IN16L, left_garbage, sizeof(left_garbage), 10 },
        { SGI_PACK_10IN16R, right, sizeof(right), 10 },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        SGIVideoFormat f = fmt(2, 1, cases[i].depth);
        SGIVideoFrame *fr = sgi_video_frame_new(&f);
        bool eight = cases[i].depth == 8;

        g_assert_cmpuint(sgi_video_packed_row_bytes(cases[i].p, 2), ==,
                         cases[i].len);
        sgi_video_unpack_row(cases[i].p, cases[i].bytes, fr, 0);
        g_assert_cmphex(fr->cb[0], ==, eight ? 0xea : 0x3ab);
        g_assert_cmphex(fr->y[0], ==, eight ? 0x55 : 0x155);
        g_assert_cmphex(fr->cr[0], ==, eight ? 0xb3 : 0x2cd);
        g_assert_cmphex(fr->y[1], ==, eight ? 0x3f : 0x0ff);
        sgi_video_frame_free(fr);
    }
    g_assert_cmpint(sgi_video_packing_parse("3214"), ==, -1);
    g_assert_cmpint(sgi_video_packing_parse("10in16r"), ==, SGI_PACK_10IN16R);
}

/* A guest-set timing record cannot ask for a raster past the clamp. */
static void test_format_clamp(void)
{
    SGIVideoFormat f = fmt(SGI_VIDEO_MAX_WIDTH, SGI_VIDEO_MAX_HEIGHT, 10);

    g_assert_true(sgi_video_format_valid(&f));
    f.width += 2;
    g_assert_false(sgi_video_format_valid(&f));
    f.width = 720;
    f.height = SGI_VIDEO_MAX_HEIGHT + 1;
    g_assert_false(sgi_video_format_valid(&f));
    f.height = 0;
    g_assert_false(sgi_video_format_valid(&f));
    f.height = 486;
    f.width = 719;
    g_assert_false(sgi_video_format_valid(&f));
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

static void test_timecode(void)
{
    SGIVideoFormat f = fmt(720, 486, 8), f25 = fmt(720, 576, 8);
    char tc[12];
    bool drop;

    f25.rate_num = 25;
    f25.rate_den = 1;
    /* Non-drop counts every number. */
    sgi_tc_format(1800, &f, false, tc);
    g_assert_cmpstr(tc, ==, "00:01:00:00");
    sgi_tc_format(25 * 3600 * 2 + 7, &f25, false, tc);
    g_assert_cmpstr(tc, ==, "02:00:00:07");
    /* Drop-frame skips 00 and 01 at each minute but not each tenth. */
    sgi_tc_format(1799, &f, true, tc);
    g_assert_cmpstr(tc, ==, "00:00:59;29");
    sgi_tc_format(1800, &f, true, tc);
    g_assert_cmpstr(tc, ==, "00:01:00;02");
    sgi_tc_format(17981, &f, true, tc);
    g_assert_cmpstr(tc, ==, "00:09:59;29");
    sgi_tc_format(17982, &f, true, tc);
    g_assert_cmpstr(tc, ==, "00:10:00;00");
    sgi_tc_format(17983, &f, true, tc);
    g_assert_cmpstr(tc, ==, "00:10:00;01");
    /* An hour of 29.97 is 107892 frames. */
    sgi_tc_format(107892, &f, true, tc);
    g_assert_cmpstr(tc, ==, "01:00:00;00");
    /* Parsing is the inverse, and refuses labels that do not exist. */
    g_assert_cmpint(sgi_tc_parse("00:01:00;02", &f, &drop), ==, 1800);
    g_assert_true(drop);
    g_assert_cmpint(sgi_tc_parse("01:00:00;00", &f, &drop), ==, 107892);
    g_assert_cmpint(sgi_tc_parse("00:10:00;00", &f, &drop), ==, 17982);
    g_assert_cmpint(sgi_tc_parse("00:01:00;00", &f, &drop), ==, -1);
    g_assert_cmpint(sgi_tc_parse("00:00:00;00", &f, &drop), ==, 0);
    g_assert_cmpint(sgi_tc_parse("00:00:01:00", &f, &drop), ==, 30);
    g_assert_false(drop);
    g_assert_cmpint(sgi_tc_parse("00:00:00;00", &f25, &drop), ==, -1);
    g_assert_cmpint(sgi_tc_parse("00:00:00:25", &f25, &drop), ==, -1);
    g_assert_cmpint(sgi_tc_parse("garbage", &f, &drop), ==, -1);
}

static void test_y4m_frame_get(void)
{
    SGIVideoFormat f10 = fmt(4, 2, 10);
    SGIVideoFrame *a = sgi_video_frame_new(&f10);
    SGIVideoFrame *b = sgi_video_frame_new(&f10);
    g_autofree uint8_t *buf = g_malloc(6 + sgi_video_frame_bytes(&f10));

    for (unsigned i = 0; i < 8; i++) {
        a->y[i] = 100 + 111 * i;
    }
    for (unsigned i = 0; i < 4; i++) {
        a->cb[i] = 3 + 250 * i;
        a->cr[i] = 1000 - 17 * i;
    }
    sgi_y4m_frame_put(a, buf);
    sgi_y4m_frame_get(b, buf + 6);
    g_assert_cmpmem(a->y, 8 * 2 * 2, b->y, 8 * 2 * 2);
    g_assert_true(sgi_y4m_parse_frame_line("FRAME", &error_abort));
    g_assert_false(sgi_y4m_parse_frame_line("FRAME Ixyz", NULL));
    g_assert_false(sgi_y4m_parse_frame_line("FRAMES", NULL));
    sgi_video_frame_free(a);
    sgi_video_frame_free(b);
}

/* The packer writes the pad bits as copies of the sample's top bits. */
static void test_pack_bytes(void)
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
    uint8_t out[8];

    fr->cb[0] = 0x3ab;
    fr->y[0] = 0x155;
    fr->cr[0] = 0x2cd;
    fr->y[1] = 0x0ff;
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        memset(out, 0, sizeof(out));
        sgi_video_pack_row(cases[i].p, fr, 0, out);
        g_assert_cmpmem(out, cases[i].len, cases[i].bytes, cases[i].len);
    }
    /* The 8-bit packing of a 10-bit frame drops the two low bits. */
    sgi_video_pack_row(SGI_PACK_8, fr, 0, out);
    g_assert_cmpmem(out, sizeof(p8), p8, sizeof(p8));
    /* An 8-bit frame packed at 10 bits shifts up. */
    sgi_video_unpack_row(SGI_PACK_8, p8, f8r, 0);
    sgi_video_pack_row(SGI_PACK_10IN16R, f8r, 0, out);
    g_assert_cmpuint(lduw_be_p(out + 2), ==, 0x55 << 2);
    sgi_video_frame_free(fr);
    sgi_video_frame_free(f8r);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-video-port/rgb", test_rgb);
    g_test_add_func("/sgi-video-port/chroma-repeat", test_chroma_repeat);
    g_test_add_func("/sgi-video-port/unpack-bytes", test_unpack_bytes);
    g_test_add_func("/sgi-video-port/format-clamp", test_format_clamp);
    g_test_add_func("/sgi-video-port/pack-bytes", test_pack_bytes);
    g_test_add_func("/sgi-video-port/packing-round-trip",
                    test_packing_round_trip);
    g_test_add_func("/sgi-video-port/y4m-parse-errors", test_y4m_parse_errors);
    g_test_add_func("/sgi-video-port/timecode", test_timecode);
    g_test_add_func("/sgi-video-port/y4m-frame-get", test_y4m_frame_get);
    g_test_add_func("/sgi-video-port/y4m-header", test_y4m_header);
    g_test_add_func("/sgi-video-port/y4m-frame", test_y4m_frame);
    return g_test_run();
}
