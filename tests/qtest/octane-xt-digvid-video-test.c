/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * DMediaPro DM2 (XT-DIGVID) output payload: the frame a completed output slot
 * reads from guest memory, in each memory packing, as the DM2's console and
 * as the frames of a file sink. The expectations are the manual's packing
 * diagrams and standard YCbCr, written out in this file, not the model's.
 * See "The data transfer" and "Model choices" in
 * docs/system/mips/sgi/hardware/xt-digvid.rst.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include <math.h>
#include "libqtest.h"
#include "qobject/qdict.h"
#include "memtx.h"

#define BOARD       (0x10000000ULL + (10ULL << 24))
#define RAM         0x20000000ULL
#define PATH        "/machine/module0/dm2"
#define OCTANE2_CPU "R12000,r12000-prid=0xe35,r12000-fpu-id=0x900," \
    "reset-mode=0x1aa683"

#define GIF(off)        (0x800000 + (off))
#define CTL_OUT         GIF(0x14)
#define START_OUT       GIF(0x1c)
#define RING_RESET_OUT  GIF(0x74)
#define TX(k)           GIF(0x3804 + 8 * (k))
#define DOORBELL        GIF(0x38ec)
#define MAP(i)          (0x100000 + 4 * (i))
#define XG_DEST_UPPER   0x34
#define XG_DEST_LOWER   0x3c
#define XG_VECTORS      0x5c
#define XG_CONTROL      0x24

#define RING0_HOST      (RAM + 0x100000)
#define SLOT0_HOST      (RAM + 0x108000)
#define IRQ_SINK        (RAM + 0x110000)
#define MAP_VALID_ENTRY(host)   (0x20000000u | (uint32_t)(((host) >> 14) << 2))
#define F_VALID         0x1u
#define F_TX            0x40000u
#define FIELD_NS        16683333    /* 525 timing, 1001/60000 s */

enum { P8, P10, P10L, P10R, PACKINGS };
static const char *const packing_name[] = { "8", "10", "10in16l", "10in16r" };

static QTestState *qts;
static char *flash_path;
static unsigned ring_idx;

static void wr(uint32_t off, uint32_t value)
{
    qtest_writel(qts, BOARD + off, value);
}

static void ram_set_l(uint64_t addr, uint32_t value)
{
    uint8_t b[4];

    stl_be_p(b, value);
    qtest_memwrite(qts, addr, b, sizeof(b));
}

static uint32_t ram_l(uint64_t addr)
{
    uint8_t b[4];

    qtest_memread(qts, addr, b, sizeof(b));
    return ldl_be_p(b);
}

static uint32_t ring_pop(void)
{
    uint64_t at = RING0_HOST + ring_idx * 0x80 + 4;
    uint32_t flags = ram_l(at);

    if (flags & F_VALID) {
        ram_set_l(at, flags & ~F_VALID);
        ring_idx = (ring_idx + 1) % 16;
    }
    return flags;
}

/* The 525 timing record of a W by H raster, W a multiple of two. */
static void send_record(unsigned w, unsigned field_h)
{
    uint8_t m[24] = {
        18, 0, 0xa2, 6, 0x03, 0x5a, w >> 8, w, 0x02, 0x0d,
        field_h >> 8, field_h, 0x01, 0x0a, field_h >> 8, field_h, 16, 17, 1,
    };

    for (unsigned k = 0; k < 12; k++) {
        wr(TX(k), (m[2 * k] << 8) | m[2 * k + 1]);
    }
    wr(DOORBELL, 1);
    /* The VBOB acknowledges a set message in the output ring. */
    g_assert_cmphex(ring_pop(), ==, F_VALID | F_TX);
}

static void start(const char *dm2, const char *extra)
{
    g_autofree char *quoted = g_shell_quote(flash_path);

    qts = qtest_initf(MEMTX_TEST_DEVICE "-M octane2 -m 256 -cpu " OCTANE2_CPU
                      " -bios %s -S -display none -vga none %s "
                      "-device sgi-xt-digvid,slot=widget-10,id=dm2%s%s",
                      quoted, extra, *dm2 ? "," : "", dm2);
    ring_idx = 0;
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'x-test-gif-loaded','value':true}}", PATH);
    /* The driver's attach: interrupt destination, vectors, ring and slot. */
    wr(XG_DEST_UPPER, 8u << 16);
    wr(XG_DEST_LOWER, IRQ_SINK);
    wr(XG_VECTORS, 0x41 | (0x42u << 9) | (0x43u << 18) | 0x20100);
    wr(XG_CONTROL, 0x4200 | 10 | (8u << 22) | (1u << 17));
    wr(0x108000, MAP_VALID_ENTRY(RING0_HOST));
    wr(MAP(0x1400), MAP_VALID_ENTRY(SLOT0_HOST));
}

/* One frame: the bytes in slot 1 (start 0x1000), armed, then its fields. */
static void play(const uint8_t *mem, size_t len, unsigned fields)
{
    qtest_memwrite(qts, SLOT0_HOST + 0x1000, mem, len);
    wr(RING_RESET_OUT, 0);
    ring_idx = 0;
    wr(START_OUT, 0x1000);
    wr(CTL_OUT, (1u << 1) | 0xea040 | 0x10000);
    for (unsigned i = 0; i < fields; i++) {
        qtest_clock_step(qts, FIELD_NS);
        g_assert_cmphex(ring_pop() & 0x80, ==, 0x80);
    }
}

/* ---- Frames: samples in, packed memory out, and the file they become ---- */

typedef struct Frame {
    unsigned w, h, depth;
    uint16_t *y, *cb, *cr;
} Frame;

static Frame *frame_new(unsigned w, unsigned h, unsigned depth, unsigned seed)
{
    Frame *f = g_new0(Frame, 1);
    unsigned mask = (1u << depth) - 1;

    f->w = w;
    f->h = h;
    f->depth = depth;
    f->y = g_new(uint16_t, w * h);
    f->cb = g_new(uint16_t, w * h / 2);
    f->cr = g_new(uint16_t, w * h / 2);
    for (unsigned i = 0; i < w * h; i++) {
        f->y[i] = (i * 37 + seed * 11 + 5) & mask;
    }
    for (unsigned i = 0; i < w * h / 2; i++) {
        f->cb[i] = (i * 53 + seed * 7 + 1) & mask;
        f->cr[i] = (i * 29 + seed * 13 + 3) & mask;
    }
    return f;
}

static void frame_free(Frame *f)
{
    g_free(f->y);
    g_free(f->cb);
    g_free(f->cr);
    g_free(f);
}

/* The manual's CbYCrY diagrams, one pair of pixels at a time. */
static size_t pair_bytes(int packing, unsigned w)
{
    return packing == P8 ? 2 * w : packing == P10 ? 5 * w / 2 : 4 * w;
}

static void put_pair(int packing, const uint16_t s[4], uint8_t *d)
{
    switch (packing) {
    case P8:
        for (int k = 0; k < 4; k++) {
            d[k] = s[k];
        }
        break;
    case P10: {
        uint64_t b = ((uint64_t)s[0] << 30) | ((uint64_t)s[1] << 20) |
                     (s[2] << 10) | s[3];

        for (int k = 0; k < 5; k++) {
            d[k] = b >> (32 - 8 * k);
        }
        break;
    }
    case P10L:
        for (int k = 0; k < 4; k++) {
            stw_be_p(d + 2 * k, (s[k] << 6) | (s[k] >> 4));
        }
        break;
    default:
        for (int k = 0; k < 4; k++) {
            stw_be_p(d + 2 * k, s[k]);
        }
    }
}

static uint8_t *frame_pack(const Frame *f, int packing, size_t *len)
{
    size_t row = pair_bytes(packing, f->w);
    size_t step = row / (f->w / 2);
    uint8_t *mem = g_malloc0(row * f->h);

    for (unsigned y = 0; y < f->h; y++) {
        for (unsigned i = 0; i < f->w / 2; i++) {
            size_t yi = y * f->w + 2 * i, ci = y * (f->w / 2) + i;
            uint16_t s[4] = { f->cb[ci], f->y[yi], f->cr[ci], f->y[yi + 1] };

            put_pair(packing, s, mem + y * row + i * step);
        }
    }
    *len = row * f->h;
    return mem;
}

/* The Y4M file's header, or one frame of it. */
static GString *y4m_header(const Frame *f)
{
    GString *s = g_string_new(NULL);

    g_string_printf(s, "YUV4MPEG2 W%u H%u F30000:1001 It A0:0 C422%s\n",
                    f->w, f->h, f->depth == 10 ? "p10" : "");
    return s;
}

static void y4m_frame(GString *out, const Frame *f)
{
    const uint16_t *plane[3] = { f->y, f->cb, f->cr };
    unsigned count[3] = { f->w * f->h, f->w * f->h / 2, f->w * f->h / 2 };

    g_string_append(out, "FRAME\n");
    for (int p = 0; p < 3; p++) {
        for (unsigned i = 0; i < count[p]; i++) {
            g_string_append_c(out, plane[p][i] & 0xff);
            if (f->depth == 10) {
                g_string_append_c(out, plane[p][i] >> 8);
            }
        }
    }
}

static char *file_sha(const char *path)
{
    g_autofree char *data = NULL;
    gsize len;

    g_assert_true(g_file_get_contents(path, &data, &len, NULL));
    return g_compute_checksum_for_data(G_CHECKSUM_SHA256, (guchar *)data, len);
}

static char *string_sha(const GString *s)
{
    return g_compute_checksum_for_data(G_CHECKSUM_SHA256, (guchar *)s->str,
                                       s->len);
}

static uint64_t sink_get(const char *prop)
{
    QDict *r = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
                         "{'path':'/machine/peripheral/rec',"
                         "'property':%s}}", prop);
    uint64_t v = qdict_get_int(r, "return");

    qobject_unref(r);
    return v;
}

static void sink_wait(uint64_t written)
{
    for (int i = 0; i < 2000 && sink_get("written") < written; i++) {
        g_usleep(5000);
    }
    g_assert_cmpuint(sink_get("written"), ==, written);
}

/* 0x00RRGGBB of each pixel of the DM2's console, from a screendump. */
static void dump_console(unsigned w, unsigned h, uint32_t *rgb)
{
    g_autofree char *header = g_strdup_printf("P6\n%u %u\n255\n", w, h);
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("dm2-dump-XXXXXX", &error);
    g_autofree char *path = g_build_filename(dir, "dump.ppm", NULL);
    g_autofree gchar *data = NULL;
    gsize len = 0;

    g_assert_no_error(error);
    qtest_qmp_assert_success(qts,
        "{'execute':'screendump','arguments':{'filename':%s,'device':'dm2',"
        "'head':0}}", path);
    g_assert_true(g_file_get_contents(path, &data, &len, NULL));
    g_unlink(path);
    g_rmdir(dir);
    g_assert_cmpuint(len, ==, strlen(header) + w * h * 3);
    g_assert_cmpmem(data, strlen(header), header, strlen(header));
    for (unsigned i = 0; i < w * h; i++) {
        const uint8_t *p = (uint8_t *)data + strlen(header) + 3 * i;

        rgb[i] = p[0] << 16 | p[1] << 8 | p[2];
    }
}

static int channel(double v)
{
    return v < 0 ? 0 : v > 255 ? 255 : lround(v);
}

/* ITU-R BT.601 from the standard's coefficients, limited range. */
static void check_picture(const Frame *f, const uint32_t *rgb)
{
    for (unsigned y = 0; y < f->h; y++) {
        for (unsigned x = 0; x < f->w; x++) {
            unsigned sh = f->depth - 8;
            double l = (f->y[y * f->w + x] >> sh) - 16;
            double u = (f->cb[y * f->w / 2 + x / 2] >> sh) - 128;
            double v = (f->cr[y * f->w / 2 + x / 2] >> sh) - 128;
            double r = 255.0 / 219 * l + 255.0 / 224 * 1.402 * v;
            double g = 255.0 / 219 * l - 255.0 / 224 * 0.344136 * u -
                       255.0 / 224 * 0.714136 * v;
            double b = 255.0 / 219 * l + 255.0 / 224 * 1.772 * u;
            uint32_t got = rgb[y * f->w + x];

            g_assert_cmpint(abs((int)(got >> 16) - channel(r)), <=, 1);
            g_assert_cmpint(abs((int)((got >> 8) & 0xff) - channel(g)), <=, 1);
            g_assert_cmpint(abs((int)(got & 0xff) - channel(b)), <=, 1);
        }
    }
}

enum { W = 16, H = 16 };

/*
 * Every packing: the console shows the colours of the samples the guest
 * stored, and the sink's file holds those samples, byte for byte.
 */
static void test_packings(void)
{
    for (int p = 0; p < PACKINGS; p++) {
        unsigned depth = p == P8 ? 8 : 10;
        Frame *f = frame_new(W, H, depth, p);
        g_autofree char *dir = g_dir_make_tmp("dm2-video-XXXXXX", NULL);
        g_autofree char *path = g_build_filename(dir, "out.y4m", NULL);
        g_autofree char *cmd = g_strdup_printf(
            "-device sgi-video-file-sink,id=rec,path=%s", path);
        g_autofree char *props = g_strdup_printf(
            "out-sink=rec,x-packing=%s", packing_name[p]);
        g_autofree uint8_t *mem = NULL;
        g_autofree char *want = NULL, *got = NULL;
        GString *file = y4m_header(f);
        uint32_t rgb[W * H];
        size_t len;

        g_test_message("packing %s", packing_name[p]);
        start(props, cmd);
        send_record(W, H / 2);
        mem = frame_pack(f, p, &len);
        play(mem, len, 2);
        dump_console(W, H, rgb);
        check_picture(f, rgb);
        sink_wait(1);
        y4m_frame(file, f);
        want = string_sha(file);
        got = file_sha(path);
        g_assert_cmpstr(got, ==, want);
        g_assert_cmpuint(sink_get("frames"), ==, 1);
        g_assert_cmpuint(sink_get("dropped"), ==, 0);
        qtest_quit(qts);
        g_string_free(file, true);
        frame_free(f);
        g_unlink(path);
        g_rmdir(dir);
    }
}

/*
 * The packing is a property because no register shows it: memory laid out
 * as 8-bit bytes but unpacked as 10-bit gives another picture.
 */
static void test_mismatched_packing(void)
{
    Frame *f = frame_new(W, H, 8, 3);
    g_autofree uint8_t *mem = NULL;
    uint32_t rgb8[W * H], rgb10[W * H];
    size_t len;

    mem = frame_pack(f, P8, &len);
    start("x-packing=8", "");
    send_record(W, H / 2);
    play(mem, len, 2);
    dump_console(W, H, rgb8);
    check_picture(f, rgb8);
    qtest_quit(qts);
    start("x-packing=10", "");
    send_record(W, H / 2);
    play(mem, len, 2);
    dump_console(W, H, rgb10);
    g_assert_cmpint(memcmp(rgb8, rgb10, sizeof(rgb8)), !=, 0);
    qtest_quit(qts);
    frame_free(f);
}

/*
 * A sink writes exactly the frames sent, in order: 100 distinct frames,
 * one file, one checksum; the optional WAV is a valid empty PCM file.
 */
static void test_sink_exact_frames(void)
{
    enum { FRAMES = 100 };
    g_autofree char *dir = g_dir_make_tmp("dm2-video-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "out.y4m", NULL);
    g_autofree char *wav = g_build_filename(dir, "out.wav", NULL);
    g_autofree char *cmd = g_strdup_printf(
        "-device sgi-video-file-sink,id=rec,path=%s,audio=%s", path, wav);
    g_autofree char *want = NULL, *got = NULL, *wavdata = NULL;
    GString *file = NULL;
    gsize wavlen;

    start("out-sink=rec", cmd);
    send_record(W, H / 2);
    for (unsigned n = 0; n < FRAMES; n++) {
        Frame *f = frame_new(W, H, 8, n);
        g_autofree uint8_t *mem = NULL;
        size_t len;

        if (!file) {
            file = y4m_header(f);
        }
        mem = frame_pack(f, P8, &len);
        play(mem, len, 2);
        y4m_frame(file, f);
        frame_free(f);
        sink_wait(n + 1);
    }
    g_assert_cmpuint(sink_get("frames"), ==, FRAMES);
    g_assert_cmpuint(sink_get("dropped"), ==, 0);
    want = string_sha(file);
    got = file_sha(path);
    g_assert_cmpstr(got, ==, want);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(wav, &wavdata, &wavlen, NULL));
    g_assert_cmpuint(wavlen, ==, 44);
    g_assert_cmpmem(wavdata, 4, "RIFF", 4);
    g_assert_cmpuint(ldl_le_p(wavdata + 4), ==, 36);
    g_assert_cmpuint(ldl_le_p(wavdata + 40), ==, 0);
    g_string_free(file, true);
    g_unlink(path);
    g_unlink(wav);
    g_rmdir(dir);
}

/*
 * A writer that cannot keep up costs frames, not time: a FIFO nobody
 * reads blocks the writer, the queue fills and further frames are counted
 * as dropped. Every frame is either written or dropped once it drains.
 */
static void test_sink_drops_when_behind(void)
{
    enum { FRAMES = 40, WIDE = 64 };
    g_autofree char *dir = g_dir_make_tmp("dm2-video-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "out.fifo", NULL);
    g_autofree char *cmd = g_strdup_printf(
        "-device sgi-video-file-sink,id=rec,path=%s", path);
    char drain[8192 + 64];
    int rfd;

    g_assert_cmpint(mkfifo(path, 0600), ==, 0);
    /* A reader open lets QEMU's open succeed; it reads nothing yet. */
    rfd = open(path, O_RDONLY | O_NONBLOCK);
    g_assert_cmpint(rfd, >=, 0);
    start("out-sink=rec", cmd);
    send_record(WIDE, WIDE / 2);
    for (unsigned n = 0; n < FRAMES; n++) {
        Frame *f = frame_new(WIDE, WIDE, 8, n);
        g_autofree uint8_t *mem = NULL;
        size_t len;

        mem = frame_pack(f, P8, &len);
        play(mem, len, 2);
        frame_free(f);
    }
    g_assert_cmpuint(sink_get("frames"), ==, FRAMES);
    g_assert_cmpuint(sink_get("dropped"), >, 0);
    /* The reader drains; the rest of the queue is written. */
    for (int i = 0; i < 4000 &&
         sink_get("written") + sink_get("dropped") < FRAMES; i++) {
        if (read(rfd, drain, sizeof(drain)) <= 0) {
            g_usleep(2000);
        }
    }
    g_assert_cmpuint(sink_get("written") + sink_get("dropped"), ==, FRAMES);
    qtest_quit(qts);
    close(rfd);
    g_unlink(path);
    g_rmdir(dir);
}

/* No timing record from the VBOB, no raster: nothing is played out. */
static void test_no_timing_no_frame(void)
{
    g_autofree char *dir = g_dir_make_tmp("dm2-video-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "out.y4m", NULL);
    g_autofree char *cmd = g_strdup_printf(
        "-device sgi-video-file-sink,id=rec,path=%s", path);
    uint8_t mem[64] = { 0 };

    start("out-sink=rec", cmd);
    /* Without a record a buffer is one field. */
    play(mem, sizeof(mem), 1);
    g_assert_cmpuint(sink_get("frames"), ==, 0);
    qtest_quit(qts);
    g_unlink(path);
    g_rmdir(dir);
}

int main(int argc, char **argv)
{
    const uint8_t erased[] = { 0xff, 0xff, 0xff, 0xff };
    int fd, result;

    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("octane-xt-digvid-video-prom-XXXXXX", &flash_path,
                         NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, erased, sizeof(erased)), ==, sizeof(erased));
    close(fd);
    qtest_add_func("/octane2/xt-digvid-video/packings", test_packings);
    qtest_add_func("/octane2/xt-digvid-video/mismatched-packing",
                   test_mismatched_packing);
    qtest_add_func("/octane2/xt-digvid-video/sink-exact-frames",
                   test_sink_exact_frames);
    qtest_add_func("/octane2/xt-digvid-video/sink-drops-when-behind",
                   test_sink_drops_when_behind);
    qtest_add_func("/octane2/xt-digvid-video/no-timing-no-frame",
                   test_no_timing_no_frame);
    result = g_test_run();
    g_assert_cmpint(unlink(flash_path), ==, 0);
    g_free(flash_path);
    return result;
}
