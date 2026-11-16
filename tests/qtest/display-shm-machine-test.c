/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The display protocol version 4 producer on real sources: the SGI scanout,
 * which resolves its lines into the console surface, and a 2D PCI device,
 * which owns a surface of its own. Each frame is checked against what
 * QMP screendump reads from the same console, and consoles the producer
 * cannot publish are announced as unsupported. POSIX hosts only.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "display-shm-viewer.h"

#define OCTANE2_CPU "R12000,r12000-prid=0xe24,r12000-fpu-id=0x7654," \
    "reset-mode=0x1aa683"
#define VPRO_SHORT 0x1b000000ULL
#define VPRO_PATH "/machine/module0/widget-11"
#define ODSY_BACKEND_SELECT 0x400000
#define ODSY_BACKEND_HDR(index, count) \
    (0x30000000u | ((uint32_t)(index) << 14) | (count))
#define ODSY_DISPMODE_RGB 0x005215a6u
#define ODSY_CURSOR_POSITION 0x2580

/* The scanout comes up after the producer's machine-done step. */
#define SGI_HEAD 1

static char *sock_dir;
static char *sock_path;
static char *prom_path;

static void backend_push(QTestState *qts, uint32_t word)
{
    qtest_writel(qts, VPRO_SHORT + ODSY_BACKEND_SELECT, word);
}

/* An RGB screen with an identity colour table: a written pixel shows as is. */
static void display_identity(QTestState *qts)
{
    backend_push(qts, ODSY_BACKEND_HDR(0x2900, 1));
    backend_push(qts, ODSY_DISPMODE_RGB);
    backend_push(qts, ODSY_BACKEND_HDR(0x1a00, 0x600));
    for (uint32_t i = 0; i < 0x600; i++) {
        uint32_t v = i < 0x200 ? i >> 2 :
                     i < 0x400 ? ((i - 0x200) >> 1) + 0x80 :
                     (i - 0x300) + 0x100;

        backend_push(qts, v << 20 | v << 10 | v);
    }
}

static void set_test_pixel(QTestState *qts, uint32_t x, uint32_t y,
                           uint32_t value)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", VPRO_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", VPRO_PATH, y);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel', 'value': %u}}", VPRO_PATH, value);
}

/* ---- screendump -------------------------------------------------------- */

typedef struct Dump {
    unsigned w, h;
    uint8_t *rgb;
} Dump;

static void dump_take(QTestState *qts, Dump *d)
{
    g_autofree char *path = NULL;
    g_autofree char *data = NULL;
    gsize len;
    int fd = g_file_open_tmp("display-shm-dump-XXXXXX.ppm", &path, NULL);
    unsigned maxv, off;

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    qtest_qmp_assert_success(qts,
        "{'execute': 'screendump', 'arguments': {'filename': %s}}", path);
    g_assert_true(g_file_get_contents(path, &data, &len, NULL));
    unlink(path);
    g_assert_cmpint(sscanf(data, "P6\n%u %u\n%u\n%n", &d->w, &d->h, &maxv,
                           (int *)&off), ==, 3);
    g_assert_cmpuint(maxv, ==, 255);
    g_assert_cmpuint(len - off, ==, (gsize)d->w * d->h * 3);
    d->rgb = g_memdup2(data + off, len - off);
}

static uint32_t dump_px(const Dump *d, unsigned x, unsigned y)
{
    const uint8_t *p = d->rgb + ((size_t)y * d->w + x) * 3;

    return p[0] << 16 | p[1] << 8 | p[2];
}

/* The frame is the console's picture, X bytes aside. */
static void check_against_dump(Viewer *v, const VFrame *f, const Dump *d)
{
    const VHead *h = &v->head[f->head];

    g_assert_cmpuint(h->width, ==, d->w);
    g_assert_cmpuint(h->height, ==, d->h);
    for (unsigned y = 0; y < h->height; y++) {
        for (unsigned x = 0; x < h->width; x++) {
            g_assert_cmphex(v_px(v, f, x, y), ==, dump_px(d, x, y));
        }
    }
}

static bool fresh(Viewer *v, unsigned head, uint64_t seen, VFrame *f)
{
    g_usleep(10000);
    v_request(v, head, DSHM_REQ_FRAME, seen, 0, DSHM_REQ_SAMPLE_NOW);
    return v_frame(v, f, V_WAIT_MS);
}

/* ---- The SGI scanout ---------------------------------------------------- */

static QTestState *start_sgi(void)
{
    return qtest_initf("-M octane2 -m 256M -cpu %s -bios %s -S -display none "
                       "-serial none -monitor none "
                       "-device display-shm,path=%s,x-test=on",
                       OCTANE2_CPU, prom_path, sock_path);
}

static Viewer *open_sgi(void)
{
    return v_open(sock_path, (VOpts) { .credit = 1, .test = true,
                                       .steer = true,
                                       .features = DSHM_FEAT_INPUT });
}

/* One changed pixel is one line of damage, and the frame is the screendump. */
static void test_sgi_resolve(void)
{
    QTestState *qts = start_sgi();
    Viewer *v = open_sgi();
    VFrame f, g;
    Dump d;
    uint64_t n, produced;

    g_assert_true(v->head[SGI_HEAD].present);
    g_assert_cmpuint(v->head[SGI_HEAD].pointer_mode, ==, DSHM_POINTER_STEERED);
    g_assert_true(v->head[0].present);
    display_identity(qts);
    g_assert_true(fresh(v, SGI_HEAD, 0, &f));
    g_assert_cmpuint(f.damage_kind, ==, DSHM_DAMAGE_WHOLE);
    g_assert_cmpuint(v->head[SGI_HEAD].width, >, 0);
    g_assert_cmpuint(v_px(v, &f, 5, 100), ==, 0);
    n = f.serial;
    v_release(v, &f);

    produced = v_stats(v).lines_produced;
    set_test_pixel(qts, 5, 100, 0x11223300);
    g_assert_true(fresh(v, SGI_HEAD, n, &g));
    /* the VPro resolved its whole screen for the one pixel */
    g_assert_cmpuint(v_stats(v).lines_produced - produced, >=,
                     v->head[SGI_HEAD].height);
    g_assert_cmpuint(g.serial, >, n);
    g_assert_cmphex(v_px(v, &g, 5, 100), ==, 0x112233);
    g_assert_cmpuint(g.damage_kind, ==, DSHM_DAMAGE_RECTS);
    g_assert_cmpuint(g.damage_count, ==, 1);
    g_assert_cmpuint(le32(g.damage + 4), ==, 100);
    g_assert_cmpuint(le32(g.damage + 12), ==, 1);
    n = g.serial;
    v_release(v, &g);

    /* the same pixel again re-resolves every line and changes none */
    set_test_pixel(qts, 5, 100, 0x11223300);
    g_usleep(100000);
    v_request(v, SGI_HEAD, DSHM_REQ_FRAME, n, 0, DSHM_REQ_SAMPLE_NOW);
    g_assert_false(v_frame(v, &g, V_QUIET_MS));

    set_test_pixel(qts, 5, 100, 0x44556600);
    g_assert_true(fresh(v, SGI_HEAD, n, &g));
    dump_take(qts, &d);
    check_against_dump(v, &g, &d);
    g_assert_cmphex(v_px(v, &g, 5, 100), !=, 0x112233);
    g_free(d.rgb);
    v_close(v);
    qtest_quit(qts);
}

/* The VPro reports its glyph's top-left; steering reads it from the block. */
static void test_sgi_cursor_block(void)
{
    QTestState *qts = start_sgi();
    Viewer *v = open_sgi();
    const uint8_t *blk = v->state + v->steer_block_off + 32 * SGI_HEAD;

    g_assert_cmpuint(le32(blk + 4), ==, 0);
    backend_push(qts, ODSY_BACKEND_HDR(ODSY_CURSOR_POSITION, 1));
    backend_push(qts, 380u << 16 | 372);
    v_stats(v);
    g_assert_cmpuint(le32(blk + 4), ==, 2);
    g_assert_cmpint((int32_t)le32(blk + 8), ==, 380);
    g_assert_cmpint((int32_t)le32(blk + 12), ==, 372);
    v_close(v);
    qtest_quit(qts);
}

/*
 * On a running machine a sample waits for the head's next retrace; when the
 * machine stops it runs at once.
 */
static void test_sgi_retrace_pacing(void)
{
    QTestState *qts = start_sgi();
    Viewer *v = open_sgi();
    VFrame f, g;
    uint64_t n;

    display_identity(qts);
    g_assert_true(fresh(v, SGI_HEAD, 0, &f));
    n = f.serial;
    v_release(v, &f);
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* the retrace timer runs on guest time, which only the test advances */
    qtest_clock_step(qts, 100000000);

    v_request(v, SGI_HEAD, DSHM_REQ_FRAME, n, 0, 0);
    set_test_pixel(qts, 5, 100, 0x11223300);
    g_assert_false(v_frame(v, &g, V_QUIET_MS));
    qtest_clock_step(qts, 20000000);
    g_assert_true(v_frame(v, &g, V_WAIT_MS));
    g_assert_cmphex(v_px(v, &g, 5, 100), ==, 0x112233);
    n = g.serial;
    v_release(v, &g);

    v_request(v, SGI_HEAD, DSHM_REQ_FRAME, n, 0, 0);
    set_test_pixel(qts, 6, 101, 0x44556600);
    g_assert_false(v_frame(v, &g, V_QUIET_MS));
    qtest_qmp_assert_success(qts, "{'execute': 'stop'}");
    g_assert_true(v_frame(v, &g, V_WAIT_MS));
    g_assert_cmpuint(g.serial, >, n);
    v_close(v);
    qtest_quit(qts);
}

/* ---- A 2D PCI device --------------------------------------------------- */

static QTestState *start_malta(void)
{
    return qtest_initf("-M malta -vga none -device VGA,romfile= "
                       "-device ati-vga,romfile= -display none -S -nodefaults "
                       "-device display-shm,path=%s,x-test=on", sock_path);
}

/* A VGA is a surface head; a console the producer cannot take is not one. */
static void test_vga_head(void)
{
    QTestState *qts = start_malta();
    Viewer *v = v_open(sock_path, (VOpts) { .credit = 1, .test = true });
    VFrame f;
    Dump d;

    g_assert_true(v->head[0].present);
    g_assert_cmpuint(v->head[0].flags & DSHM_HEAD_UNSUPPORTED, ==, 0);
    g_assert_true(v->head[1].present);
    g_assert_cmpuint(v->head[1].flags & DSHM_HEAD_UNSUPPORTED, ==,
                     DSHM_HEAD_UNSUPPORTED);
    g_assert_cmpuint(v->head[1].n_slots, ==, 0);
    g_assert_cmpuint(v->head[1].width, ==, 0);
    v_request(v, 1, DSHM_REQ_FRAME, 0, 0, 0);
    g_assert_false(v_frame(v, &f, V_QUIET_MS));

    g_assert_true(fresh(v, 0, 0, &f));
    g_assert_cmpuint(f.damage_kind, ==, DSHM_DAMAGE_WHOLE);
    g_assert_cmpuint(v->head[0].width, >, 0);
    dump_take(qts, &d);
    check_against_dump(v, &f, &d);
    g_free(d.rgb);
    v_close(v);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    const uint8_t erased[] = { 0xff, 0xff, 0xff, 0xff };
    int fd, ret;

    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("display-shm-prom-XXXXXX", &prom_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, erased, sizeof(erased)), ==, sizeof(erased));
    close(fd);
    sock_dir = g_dir_make_tmp("display-shm-m-XXXXXX", NULL);
    g_assert(sock_dir);
    sock_path = g_build_filename(sock_dir, "d.sock", NULL);

    qtest_add_func("display-shm-machine/sgi-resolve", test_sgi_resolve);
    qtest_add_func("display-shm-machine/sgi-cursor-block",
                   test_sgi_cursor_block);
    qtest_add_func("display-shm-machine/sgi-retrace-pacing",
                   test_sgi_retrace_pacing);
    qtest_add_func("display-shm-machine/vga-head", test_vga_head);
    ret = g_test_run();

    unlink(prom_path);
    rmdir(sock_dir);
    return ret;
}
