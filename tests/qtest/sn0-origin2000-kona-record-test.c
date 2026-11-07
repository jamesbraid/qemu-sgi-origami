/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA qtests for the stream recorder.
 *
 * With the pipe's "record" property naming a file, a scripted X image
 * draw by DMA (the same one sn0-origin2000-kona-xdma-test.c checks the
 * pixels of) is recorded, and the file is read back: its header, every
 * store and load in order with the width and value the script used, the
 * guest bytes the DMA read, the interrupt that answered it, the markers
 * and the record count at the end. The format is described in the KONA
 * page of the SGI manual.
 *
 * The fixture is KONA as a -device at the empty slot IO4, crossbow0's
 * widget 11, with fabric-hub-widget=9 so the board reads node 0's memory.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "sgi-test.h"

#define SMALL_WINDOW_SIZE UINT64_C(0x1000000)
#define KONA_WIDGET 11
#define KONA_BASE   (SPACE_IO + (uint64_t)KONA_WIDGET * SMALL_WINDOW_SIZE)

#define OFF_INT_DEST_UPPER  0x000034
#define OFF_INT_DEST_LOWER  0x00003c
#define OFF_INT_VECTORS     0x00005c
#define OFF_MAPRAM(i)       (0x100000 + 4 * (i))
#define OFF_INT_MASK        0x800230
#define OFF_INT_CAUSE       0x800238
#define OFF_MAILBOX         0x800240
#define OFF_KICK            0xc00108
#define OFF_COMMAND         0xc10000

#define CAUSE_MAILBOX       0x10u
#define KERNEL_INT_VECTORS    0x3e2210
#define KERNEL_INT_DEST_UPPER 0x98000
#define KERNEL_INT_DEST_LOWER 0x1800090
#define KERNEL_INT_MASK       0xf7c

/* A 16 KiB page of node 0's memory for the transfer's buffer. */
#define BUFFER_PAGE     UINT64_C(0x400000)

#define KONA_QOM_PATH "/machine/module0/kona0"

#define MACHINE_ARGS \
    "-M origin2000,nodes=1,x-uncatalogued-graphics=on -smp 1 -m 64M " \
    "-device sgi-kona,id=kona0,slot=io4,fabric-hub-widget=9"

/* The recorder's constants as the manual gives them. */
#define REC_VERSION         1
#define SRC_IMAGE           2
#define SRC_LIST            3
#define SRC_PIXELS          4
#define INT_GENERAL         0
#define MARK_RESET          1
#define MARK_FRAME          3
#define MARK_USER           0x100

typedef struct Rec {
    char tag;
    unsigned size;      /* W, R: width; D, O: source; I: kind */
    uint32_t off;       /* W, R: offset; I: vector; M: kind */
    uint64_t val;       /* W, R: value; D, O: address; M: argument */
    uint32_t len;       /* D, O: byte count; I: delivered */
    const uint8_t *data;
} Rec;

/* The accesses the script made, in order. */
static Rec script[256];
static unsigned script_len;

static void script_add(char tag, unsigned size, uint32_t off, uint64_t val)
{
    g_assert_cmpuint(script_len, <, G_N_ELEMENTS(script));
    script[script_len++] = (Rec) { tag, size, off, val };
}

static void wl(QTestState *s, uint32_t off, uint32_t val)
{
    qtest_writel(s, KONA_BASE + off, val);
    script_add('W', 4, off, val);
}

static uint32_t rl(QTestState *s, uint32_t off)
{
    uint32_t val = qtest_readl(s, KONA_BASE + off);

    script_add('R', 4, off, val);
    return val;
}

static void command(QTestState *s, uint32_t token, const uint32_t *args,
                    unsigned argc)
{
    wl(s, OFF_COMMAND, token);
    for (unsigned i = 0; i < argc; i++) {
        wl(s, OFF_COMMAND, args[i]);
    }
}

static const uint32_t gc_index8[13] = {
    0, 0, 0, 0, 0xf00fff, 0, 0x180c, 0x40001, 1, 6, 5, 1, 1,
};

/* The X server's image draw opened, then the kernel's DMA packet. */
static void image_by_dma(QTestState *s)
{
    uint8_t bytes[64];

    for (unsigned k = 0; k < sizeof(bytes); k++) {
        bytes[k] = 16 * (k / 16) + k % 16;
    }
    qtest_memwrite(s, BUFFER_PAGE, bytes, sizeof(bytes));
    wl(s, OFF_MAPRAM(0), 0x20000000 | (uint32_t)(BUFFER_PAGE >> 12));
    command(s, 0x12b, (uint32_t[]){ 1, 0, 0, 0x500, 0x400 }, 5);
    command(s, 0x12a, (uint32_t[]){ 0, 0, 0x500, 0x400 }, 4);
    command(s, 0x129, gc_index8, 13);
    command(s, 0x136, (uint32_t[]){ 0x10, 0, 1, 8, 16, 4, 0 }, 7);
    command(s, 0x139, (uint32_t[]){ 100, 50 }, 2);
    wl(s, OFF_KICK, 0x30000);
    wl(s, OFF_COMMAND, 0x1eb);
    wl(s, OFF_KICK, 0x10000);
    wl(s, OFF_COMMAND, 0);
    wl(s, OFF_COMMAND, 0x9000);
    wl(s, OFF_COMMAND, 64);
    wl(s, OFF_COMMAND, 64);
    wl(s, OFF_COMMAND, 0);
    wl(s, OFF_COMMAND, 0x100);
    wl(s, OFF_KICK, 0);
}

static QTestState *recording_init(const char *path)
{
    QTestState *s = qtest_initf("%s,record=%s", MACHINE_ARGS, path);

    wl(s, OFF_INT_VECTORS, KERNEL_INT_VECTORS);
    wl(s, OFF_INT_DEST_UPPER, KERNEL_INT_DEST_UPPER);
    wl(s, OFF_INT_DEST_LOWER, KERNEL_INT_DEST_LOWER);
    wl(s, OFF_INT_MASK, KERNEL_INT_MASK);
    return s;
}

/*
 * Parse a recording's records after its header; returns how many complete
 * ones. A record that runs past the end of the data is a cut file: *cut is
 * set and parsing stops.
 */
static unsigned parse_cut(const uint8_t *buf, size_t len, Rec *out,
                          unsigned max, bool *cut)
{
    size_t i = 12;
    unsigned n = 0;

    g_assert_cmpuint(len, >=, 12);
    g_assert_cmpint(memcmp(buf, "KONAREC\0", 8), ==, 0);
    g_assert_cmpuint(ldl_le_p(buf + 8), ==, REC_VERSION);
    *cut = false;
    while (i < len) {
        Rec *r = &out[n];
        size_t need;

        g_assert_cmpuint(n, <, max);
        *r = (Rec) { .tag = buf[i] };
        switch (r->tag) {
        case 'C':
            need = 18;
            if (i + need <= len) {
                need += buf[i + 17];
            }
            break;
        case 'W':
        case 'R':
            need = 14;
            break;
        case 'D':
        case 'O':
            need = 14;
            if (i + need <= len) {
                need += ldl_le_p(buf + i + 10);
            }
            break;
        case 'I':
            need = 4;
            break;
        case 'M':
            need = 9;
            break;
        case 'E':
            need = 9;
            break;
        default:
            g_error("bad record tag 0x%02x at byte %zu", r->tag, i);
        }
        if (i + need > len) {
            *cut = true;
            break;
        }
        i++;
        switch (r->tag) {
        case 'C':
            r->off = ldl_le_p(buf + i);          /* part */
            r->val = ldl_le_p(buf + i + 4);      /* revision */
            r->len = buf[i + 16];                /* product length */
            break;
        case 'W':
        case 'R':
            r->size = buf[i];
            r->off = ldl_le_p(buf + i + 1);
            r->val = ldq_le_p(buf + i + 5);
            break;
        case 'D':
        case 'O':
            r->size = buf[i];
            r->val = ldq_le_p(buf + i + 1);
            r->len = ldl_le_p(buf + i + 9);
            r->data = buf + i + 13;
            break;
        case 'I':
            r->size = buf[i];
            r->off = buf[i + 1];
            r->len = buf[i + 2];
            break;
        case 'M':
            r->off = ldl_le_p(buf + i);
            r->val = ldl_le_p(buf + i + 4);
            break;
        case 'E':
            r->val = ldq_le_p(buf + i);
            break;
        }
        i += need - 1;
        n++;
    }
    return n;
}

static unsigned parse(const uint8_t *buf, size_t len, Rec *out,
                      unsigned max)
{
    bool cut;
    unsigned n = parse_cut(buf, len, out, max, &cut);

    g_assert_false(cut);
    return n;
}

static void test_round_trip(void)
{
    g_autofree char *path = g_strdup_printf("%s/kona-record-XXXXXX.rec",
                                            g_get_tmp_dir());
    g_autofree char *contents = NULL;
    static Rec recs[512];
    unsigned n, w = 0, image_bytes = 0, interrupt = 0, last_dma = 0;
    unsigned first_load = 0, user_mark = 0;
    uint8_t image[64];
    QTestState *s;
    QDict *resp;
    size_t len;
    int fd = g_mkstemp(path);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    script_len = 0;
    s = recording_init(path);
    image_by_dma(s);
    g_assert_cmphex(rl(s, OFF_INT_CAUSE) & CAUSE_MAILBOX, ==, CAUSE_MAILBOX);
    g_assert_cmphex(rl(s, OFF_MAILBOX), ==, 0x100);
    /* The same load again is not kept; the same load after a store is. */
    g_assert_cmphex(qtest_readl(s, KONA_BASE + OFF_MAILBOX), ==, 0x100);
    wl(s, OFF_INT_CAUSE, CAUSE_MAILBOX);
    rl(s, OFF_MAILBOX);
    qtest_writeq(s, KONA_BASE + OFF_INT_MASK, KERNEL_INT_MASK);
    script_add('W', 8, OFF_INT_MASK, KERNEL_INT_MASK);
    qtest_qmp_assert_success(s,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-record-mark', 'value': 7}}", KONA_QOM_PATH);
    qtest_quit(s);

    g_assert_true(g_file_get_contents(path, &contents, &len, NULL));
    g_assert_cmpint(g_unlink(path), ==, 0);
    n = parse((const uint8_t *)contents, len, recs, G_N_ELEMENTS(recs));

    /* The configuration, then the reset that started the machine. */
    g_assert_cmpuint(n, >, 3);
    g_assert_cmpint(recs[0].tag, ==, 'C');
    g_assert_cmphex(recs[0].off, ==, 0xc102);
    g_assert_cmpint(recs[1].tag, ==, 'M');
    g_assert_cmpuint(recs[1].off, ==, MARK_RESET);

    /* The end record counts every record before it. */
    g_assert_cmpint(recs[n - 1].tag, ==, 'E');
    g_assert_cmpuint(recs[n - 1].val, ==, n - 1);

    /* Stores and loads: the script's, in order, width and value. */
    for (unsigned i = 0; i < n; i++) {
        switch (recs[i].tag) {
        case 'W':
        case 'R':
            g_assert_cmpuint(w, <, script_len);
            g_assert_cmpint(recs[i].tag, ==, script[w].tag);
            g_assert_cmpuint(recs[i].size, ==, script[w].size);
            g_assert_cmphex(recs[i].off, ==, script[w].off);
            g_assert_cmphex(recs[i].val, ==, script[w].val);
            if (recs[i].tag == 'R' && !first_load) {
                first_load = i;
            }
            w++;
            break;
        case 'D':
            g_assert_cmpuint(recs[i].size, ==, SRC_IMAGE);
            g_assert_cmpuint(image_bytes + recs[i].len, <=, sizeof(image));
            memcpy(image + image_bytes, recs[i].data, recs[i].len);
            image_bytes += recs[i].len;
            last_dma = i;
            break;
        case 'I':
            g_assert_cmpuint(recs[i].size, ==, INT_GENERAL);
            g_assert_cmphex(recs[i].off, ==, 0x10);
            g_assert_cmpuint(recs[i].len, ==, 1);
            interrupt++;
            g_assert_cmpuint(last_dma, !=, 0);
            g_assert_cmpuint(i, >, last_dma);
            break;
        case 'M':
            if (recs[i].off == MARK_USER) {
                g_assert_cmpuint(recs[i].val, ==, 7);
                user_mark = i;
            }
            break;
        default:
            break;
        }
    }
    g_assert_cmpuint(w, ==, script_len);

    /* The bytes the DMA read are the buffer's, and the board then answered. */
    g_assert_cmpuint(image_bytes, ==, sizeof(image));
    for (unsigned k = 0; k < sizeof(image); k++) {
        g_assert_cmpuint(image[k], ==, 16 * (k / 16) + k % 16);
    }
    g_assert_cmpuint(interrupt, ==, 1);
    g_assert_cmpuint(last_dma, <, first_load);
    g_assert_cmpuint(user_mark, ==, n - 2);

    /* Without a record file there is nothing to mark. */
    s = qtest_init(MACHINE_ARGS);
    resp = qtest_qmp(s,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-record-mark', 'value': 1}}", KONA_QOM_PATH);
    g_assert_true(qdict_haskey(resp, "error"));
    qobject_unref(resp);
    qtest_quit(s);
}

/* Start a machine recording to a new temporary file; no scripted stores. */
static QTestState *start_plain(char **path)
{
    int fd;

    *path = g_strdup_printf("%s/kona-record-XXXXXX.rec", g_get_tmp_dir());
    fd = g_mkstemp(*path);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    return qtest_initf("%s,record=%s", MACHINE_ARGS, *path);
}

/* Stop the machine, read the file back and delete it. */
static uint8_t *finish_plain(QTestState *s, char *path, size_t *len)
{
    gchar *contents = NULL;

    qtest_quit(s);
    g_assert_true(g_file_get_contents(path, &contents, len, NULL));
    g_assert_cmpint(g_unlink(path), ==, 0);
    return (uint8_t *)contents;
}

static const Rec *find(const Rec *recs, unsigned n, char tag, unsigned src,
                       uint64_t addr)
{
    for (unsigned i = 0; i < n; i++) {
        if (recs[i].tag == tag && recs[i].size == src &&
            recs[i].val == addr) {
            return &recs[i];
        }
    }
    return NULL;
}

#define REG_DMA_PUSH     0x80007c

static void push(QTestState *s, uint32_t word)
{
    qtest_writel(s, KONA_BASE + REG_DMA_PUSH, word);
}

/* A DMA packet through the push register, as the DMA qtest sends it. */
static void dma_packet(QTestState *s, uint64_t addr, uint32_t total)
{
    qtest_writel(s, KONA_BASE + OFF_KICK, 0x30000);
    push(s, 0x1eb);
    qtest_writel(s, KONA_BASE + OFF_KICK, 0x10000);
    push(s, addr >> 12);
    push(s, (addr & 0xfff) | 0x9000);
    push(s, total);
    push(s, total);
    push(s, 0);
    push(s, 0x100);
    qtest_writel(s, KONA_BASE + OFF_KICK, 0);
}

/*
 * A pixel transfer to the framebuffer and back: its rows are read from
 * guest memory (D, source 4), then written to it (O, source 4), at the row's
 * offset within the transfer's buffer.
 */
static void test_pixel_rows(void)
{
    static const uint8_t pixels[16] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
    };
    static Rec recs[64];
    g_autofree char *path = NULL;
    g_autofree uint8_t *contents = NULL;
    const Rec *d, *o;
    uint8_t back[16];
    QTestState *s = start_plain(&path);
    size_t len;
    unsigned n;

    qtest_qom_set_bool(s, KONA_QOM_PATH, "x-test-dma-framebuffer", true);
    qtest_memwrite(s, 0x9004, pixels, sizeof(pixels));
    dma_packet(s, 0x9004, 16);
    qtest_qom_set_bool(s, KONA_QOM_PATH, "x-test-dma-reverse", true);
    dma_packet(s, 0x13000, 16);
    qtest_memread(s, 0x13000, back, sizeof(back));
    g_assert_cmpint(memcmp(back, pixels, sizeof(back)), ==, 0);
    contents = finish_plain(s, path, &len);
    n = parse(contents, len, recs, G_N_ELEMENTS(recs));

    /* The first buffer starts 4 bytes into its page: row 0 is at offset 4. */
    d = find(recs, n, 'D', SRC_PIXELS, 4);
    o = find(recs, n, 'O', SRC_PIXELS, 0);
    g_assert_nonnull(d);
    g_assert_nonnull(o);
    g_assert_cmpuint(d->len, ==, 16);
    g_assert_cmpuint(o->len, ==, 16);
    g_assert_cmpint(memcmp(d->data, pixels, 16), ==, 0);
    g_assert_cmpint(memcmp(o->data, pixels, 16), ==, 0);
    g_assert_cmpuint(d - recs, <, o - recs);
}

/* A list pulled from a pool as two ranges: one D, source 3, for each. */
static void test_list_ranges(void)
{
    static const uint32_t list_words[] = {
        1, 0x3, 5, 0x11 << 2, 0x22 << 2, 0x33 << 2,
        1, 0x2, 5,
        1, 0x6, 40, 60,
    };
    static const uint32_t command[] = { 0x171, 2, 0x100, 6, 0x800, 7 };
    static const uint32_t pool[] = { 0x1ed, 0x40, 0x0000 | 0x9000, 0x4000 };
    static Rec recs[64];
    g_autofree char *path = NULL;
    g_autofree uint8_t *contents = NULL;
    const Rec *first, *second;
    uint8_t expect[13 * 4];
    QTestState *s = start_plain(&path);
    size_t len;
    unsigned n;

    for (unsigned i = 0; i < 13; i++) {
        stl_be_p(expect + 4 * i, list_words[i]);
    }
    qtest_memwrite(s, 0x40100, expect, 6 * 4);
    qtest_memwrite(s, 0x40800, expect + 6 * 4, 7 * 4);
    qtest_writel(s, KONA_BASE + OFF_MAPRAM(0x40000 / 0x4000),
                 0x20000000 | (0x40000 >> 12));
    for (unsigned i = 0; i < G_N_ELEMENTS(pool); i++) {
        qtest_writel(s, KONA_BASE + OFF_COMMAND, pool[i]);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(command); i++) {
        qtest_writel(s, KONA_BASE + OFF_COMMAND, command[i]);
    }
    contents = finish_plain(s, path, &len);
    n = parse(contents, len, recs, G_N_ELEMENTS(recs));

    first = find(recs, n, 'D', SRC_LIST, 0x40100);
    second = find(recs, n, 'D', SRC_LIST, 0x40800);
    g_assert_nonnull(first);
    g_assert_nonnull(second);
    g_assert_cmpuint(first->len, ==, 6 * 4);
    g_assert_cmpuint(second->len, ==, 7 * 4);
    g_assert_cmpint(memcmp(first->data, expect, 6 * 4), ==, 0);
    g_assert_cmpint(memcmp(second->data, expect + 6 * 4, 7 * 4), ==, 0);
}

/*
 * A retrace request completes at the next retrace: a frame marker with the
 * request's tag. The same file cut short at the end record, and in the
 * middle of it, reads as cut.
 */
static void test_frame_marker_and_cut_file(void)
{
    static Rec recs[256], cutrecs[256];
    g_autofree char *path = NULL;
    g_autofree uint8_t *contents = NULL;
    QTestState *s = start_plain(&path);
    bool cut;
    size_t len;
    unsigned n, nc, frames = 0;

    qtest_writel(s, KONA_BASE + OFF_KICK, 0x30000);
    qtest_writel(s, KONA_BASE + OFF_COMMAND, 0x1e7);
    qtest_writel(s, KONA_BASE + OFF_KICK, 0x10000);
    qtest_writel(s, KONA_BASE + OFF_COMMAND, 5);
    qtest_writel(s, KONA_BASE + OFF_KICK, 0);
    qtest_clock_step(s, 20 * 1000 * 1000);
    contents = finish_plain(s, path, &len);
    n = parse(contents, len, recs, G_N_ELEMENTS(recs));

    for (unsigned i = 0; i < n; i++) {
        if (recs[i].tag == 'M' && recs[i].off == MARK_FRAME) {
            g_assert_cmpuint(recs[i].val, ==, 5);
            frames++;
        }
    }
    g_assert_cmpuint(frames, ==, 1);
    g_assert_cmpint(recs[n - 1].tag, ==, 'E');

    /* Without its end record the file is complete but not closed. */
    nc = parse_cut(contents, len - 9, cutrecs, G_N_ELEMENTS(cutrecs), &cut);
    g_assert_false(cut);
    g_assert_cmpuint(nc, ==, n - 1);
    /* In the middle of a record, the partial one is not returned. */
    nc = parse_cut(contents, len - 4, cutrecs, G_N_ELEMENTS(cutrecs), &cut);
    g_assert_true(cut);
    g_assert_cmpuint(nc, ==, n - 1);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn0/kona/record/round-trip", test_round_trip);
    qtest_add_func("/sn0/kona/record/pixel-rows", test_pixel_rows);
    qtest_add_func("/sn0/kona/record/list-ranges", test_list_ranges);
    qtest_add_func("/sn0/kona/record/frame-marker-and-cut-file",
                   test_frame_marker_and_cut_file);
    return g_test_run();
}
