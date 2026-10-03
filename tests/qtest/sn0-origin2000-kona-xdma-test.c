/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA qtests for the X server's image writes by DMA.
 *
 * konDrawImage sends the clip 0x12b, the position 0x12a, the pixmap GC
 * 0x129, the pixel setup 0x136 with mode bit 0x10, the pixel write 0x139
 * with only its x and y, and then the kernel's DMA packet, 0x1eb and six
 * words, carries the rows from the client's buffer through the XG map
 * RAM; the X server sends the pixel finish 0x13a after the kernel's wait
 * ends. The kernel waits for the mailbox word 0x100 under cause bit 0x10,
 * which only the general vector's handler reads. These cases check that
 * the packet's bytes draw through the open pixel write as its page words
 * would, and that the completion follows them in that order.
 *
 * The fixture is KONA as a -device at the empty slot IO4, crossbow0's
 * widget 11, with fabric-hub-widget=9 so the board reads node 0's memory.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-test.h"

#define SMALL_WINDOW_SIZE UINT64_C(0x1000000)
#define KONA_WIDGET 11
#define KONA_BASE   (SPACE_IO + (uint64_t)KONA_WIDGET * SMALL_WINDOW_SIZE)
#define KONA_REG(offset) (KONA_BASE + (offset))

#define REG_INT_DEST_UPPER  KONA_REG(0x000034)
#define REG_INT_DEST_LOWER  KONA_REG(0x00003c)
#define REG_INT_VECTORS     KONA_REG(0x00005c)
#define REG_MAPRAM(i)       KONA_REG(0x100000 + 4 * (uint64_t)(i))
#define REG_INT_MASK        KONA_REG(0x800230)
#define REG_INT_CAUSE       KONA_REG(0x800238)
#define REG_MAILBOX         KONA_REG(0x800240)
#define REG_KICK            KONA_REG(0xc00108)
#define REG_COMMAND         KONA_REG(0xc10000)

#define CAUSE_MAILBOX       0x10u
#define CAUSE_FATAL         0x100u

/*
 * kona_reset_adaptor's values in the traced run: the general vector 0x10
 * and the geometry-engine vector 0x11, to node 0's Hub at widget 9; and
 * KONAstart's interrupt mask.
 */
#define KERNEL_INT_VECTORS    0x3e2210
#define KERNEL_INT_DEST_UPPER 0x98000
#define KERNEL_INT_DEST_LOWER 0x1800090
#define KERNEL_INT_MASK       0xf7c
#define GENERAL_INTERRUPT \
    "sgi_kona_general_interrupt fabric 0x9800001800090 vector 0x10\n"

#define FB_HEIGHT       1024
#define PLANE_FRONT     0
#define PLANE_FRONT_TAG 2
#define TAG_DIRECT      0
#define TAG_X_INDEX     2

/* A 16 KiB page of node 0's memory for a transfer's buffer. */
#define BUFFER_PAGE     UINT64_C(0x400000)

#define KONA_QOM_PATH "/machine/module0/kona0"

#define MACHINE_ARGS \
    "-M origin2000,nodes=1 -smp 1 -m 64M " \
    "-device sgi-kona,id=kona0,slot=io4,fabric-hub-widget=9"

static uint64_t kona_qom_get_uint(QTestState *s, const char *property)
{
    QDict *resp = qtest_qmp(s,
        "{'execute': 'qom-get', 'arguments': {'path': %s, "
        "'property': %s}}", KONA_QOM_PATH, property);
    uint64_t value;

    if (!qdict_haskey(resp, "return")) {
        g_autoptr(GString) str = qobject_to_json(QOBJECT(resp));
        g_error("qom-get %s failed: %s", property, str->str);
    }
    value = qdict_get_int(resp, "return");
    qobject_unref(resp);
    return value;
}

static void kona_qom_set_uint(QTestState *s, const char *property,
                              uint32_t value)
{
    qtest_qmp_assert_success(s,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': %s, 'value': %u}}", KONA_QOM_PATH, property, value);
}

/* A plane's word at X coordinates (x, y), y down from the top line. */
static uint32_t plane_at(QTestState *s, uint32_t plane, uint32_t x,
                         uint32_t y)
{
    kona_qom_set_uint(s, "x-test-plane", plane);
    kona_qom_set_uint(s, "x-test-pixel-x", x);
    kona_qom_set_uint(s, "x-test-pixel-y", FB_HEIGHT - 1 - y);
    return kona_qom_get_uint(s, "x-test-plane-word");
}

/* What the display shows at X coordinates (x, y). */
static uint32_t shown_at(QTestState *s, uint32_t x, uint32_t y)
{
    kona_qom_set_uint(s, "x-test-pixel-x", x);
    kona_qom_set_uint(s, "x-test-pixel-y", FB_HEIGHT - 1 - y);
    return kona_qom_get_uint(s, "x-test-pixel") & 0xffffff;
}

/*
 * A fresh instance tracing every sgi_kona event and logging guest errors
 * and unimplemented accesses to its own file, with the interrupt
 * registers as the kernel leaves them once the X server has started.
 */
static QTestState *kona_init_logged(char **log)
{
    QTestState *s;
    int fd;

    *log = g_strdup_printf("%s/sn0-kona-xdma-XXXXXX.log", g_get_tmp_dir());
    fd = g_mkstemp(*log);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    s = qtest_initf("%s -trace enable=sgi_kona_* -d guest_errors,unimp"
                    " -D %s", MACHINE_ARGS, *log);
    qtest_writel(s, REG_INT_VECTORS, KERNEL_INT_VECTORS);
    qtest_writel(s, REG_INT_DEST_UPPER, KERNEL_INT_DEST_UPPER);
    qtest_writel(s, REG_INT_DEST_LOWER, KERNEL_INT_DEST_LOWER);
    qtest_writel(s, REG_INT_MASK, KERNEL_INT_MASK);
    return s;
}

static char *kona_log_read(const char *log)
{
    char *found = NULL;

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_cmpint(g_unlink(log), ==, 0);
    return found;
}

static unsigned count_occurrences(const char *haystack, const char *needle)
{
    unsigned n = 0;

    for (const char *p = strstr(haystack, needle); p;
         p = strstr(p + 1, needle)) {
        n++;
    }
    return n;
}

/* A command on the +0xc10000 page: the token, then its words. */
static void kona_command(QTestState *s, uint32_t token, const uint32_t *args,
                         unsigned argc)
{
    qtest_writel(s, REG_COMMAND, token);
    for (unsigned i = 0; i < argc; i++) {
        qtest_writel(s, REG_COMMAND, args[i]);
    }
}

/*
 * konDrawImage's words up to its pixel write, for a GC of 13 words and a
 * setup of mode, packings, shift, width, height and pad: the clip and
 * position covering the 1280 by 1024 screen, the GC, the setup and the
 * write at (x, y) with no data words.
 */
static void x_image_open(QTestState *s, const uint32_t gc[13],
                         const uint32_t setup[7], uint32_t x, uint32_t y)
{
    kona_command(s, 0x12b, (uint32_t[]){ 1, 0, 0, 0x500, 0x400 }, 5);
    kona_command(s, 0x12a, (uint32_t[]){ 0, 0, 0x500, 0x400 }, 4);
    kona_command(s, 0x129, gc, 13);
    kona_command(s, 0x136, setup, 7);
    kona_command(s, 0x139, (uint32_t[]){ x, y }, 2);
}

/* The traced run's 8-bit colour-index pixmap GC. */
static const uint32_t gc_index8[13] = {
    0, 0, 0, 0, 0xf00fff, 0, 0x180c, 0x40001, 1, 6, 5, 1, 1,
};

/* The greeter's 24-bit TrueColor pixmap GC. */
static const uint32_t gc_true24[13] = {
    0, 0, 0, 0, 0xffffff, 0xffffff, 0x2804, 0x60001, 1, 6, 8, 0, 0,
};

/* kona_idma's packet in the kernel's bracket, as the run sent it. */
static void dma_packet(QTestState *s, uint32_t io, uint32_t total,
                       uint32_t per_line, uint32_t gap, uint32_t done)
{
    qtest_writel(s, REG_KICK, 0x30000);
    qtest_writel(s, REG_COMMAND, 0x1eb);
    qtest_writel(s, REG_KICK, 0x10000);
    qtest_writel(s, REG_COMMAND, io >> 12);
    qtest_writel(s, REG_COMMAND, (io & 0xfff) | 0x9000);
    qtest_writel(s, REG_COMMAND, total);
    qtest_writel(s, REG_COMMAND, per_line);
    qtest_writel(s, REG_COMMAND, gap);
    qtest_writel(s, REG_COMMAND, done);
    qtest_writel(s, REG_KICK, 0);
}

/* Map RAM entry `entry` maps the 16 KiB host page at `page`. */
static void map_page(QTestState *s, uint32_t entry, uint64_t page)
{
    qtest_writel(s, REG_MAPRAM(entry), 0x20000000 | (uint32_t)(page >> 12));
}

/* The 16 by 4 colour-index image's pixel at column i of row r. */
static uint8_t index_pixel(unsigned i, unsigned r)
{
    return 16 * r + i;
}

static void assert_index_image(QTestState *s, uint32_t x, uint32_t y)
{
    for (unsigned r = 0; r < 4; r++) {
        for (unsigned i = 0; i < 16; i++) {
            g_assert_cmphex(plane_at(s, PLANE_FRONT, x + i, y + r), ==,
                            index_pixel(i, r));
            g_assert_cmphex(plane_at(s, PLANE_FRONT_TAG, x + i, y + r), ==,
                            TAG_X_INDEX);
        }
    }
}

/* The completion: mailbox 0x100 under cause bit 0x10, no fatal bit. */
static void assert_completed(QTestState *s)
{
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) &
                    (CAUSE_MAILBOX | CAUSE_FATAL), ==, CAUSE_MAILBOX);
    g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0x100);
}

/*
 * A 16 by 4 8-bit image, 64 packed bytes 16r + i in one line: pixel
 * (100 + i, 50 + r) holds index 16r + i; the mailbox reads 0x100 and the
 * cause register has bit 0x10, with one general interrupt, after the
 * bytes; writing 0x10 to the cause register clears the bit; the finish
 * closes the write cleanly.
 */
static void test_index_image_by_dma(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint8_t bytes[64];

    for (unsigned k = 0; k < 64; k++) {
        bytes[k] = index_pixel(k % 16, k / 16);
    }
    qtest_memwrite(s, BUFFER_PAGE, bytes, sizeof(bytes));
    map_page(s, 0, BUFFER_PAGE);
    x_image_open(s, gc_index8, (uint32_t[]){ 0x10, 0, 1, 8, 16, 4, 0 },
                 100, 50);
    dma_packet(s, 0, 64, 64, 0, 0x100);
    assert_completed(s);
    qtest_writel(s, REG_INT_CAUSE, CAUSE_MAILBOX);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & CAUSE_MAILBOX, ==, 0);
    kona_command(s, 0x13a, (uint32_t[]){ 0 }, 1);
    assert_index_image(s, 100, 50);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, GENERAL_INTERRUPT), ==, 1);
    g_assert_true(strstr(found, "sgi_kona_x_image_dma ") <
                  strstr(found, GENERAL_INTERRUPT));
    g_assert_null(strstr(found, " kind 16 "));
    g_assert_null(strstr(found, "not arrived"));
}

/*
 * The same write stopped before the packet: nothing is drawn, the
 * mailbox holds nothing and no interrupt is sent.
 */
static void test_no_pixels_before_bytes(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    map_page(s, 0, BUFFER_PAGE);
    x_image_open(s, gc_index8, (uint32_t[]){ 0x10, 0, 1, 8, 16, 4, 0 },
                 100, 50);
    for (unsigned r = 0; r < 4; r++) {
        for (unsigned i = 0; i < 16; i++) {
            g_assert_cmphex(plane_at(s, PLANE_FRONT, 100 + i, 50 + r), ==,
                            0);
        }
    }
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & CAUSE_MAILBOX, ==, 0);
    g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_null(strstr(found, "sgi_kona_general_interrupt "));
}

/*
 * The same image's rows 32 bytes apart in the client's buffer, each
 * followed by 16 bytes that are not the image: the packet's per-line
 * count is the row and its gap the rest. The same pixels result.
 */
static void test_gathered_rows(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint8_t bytes[128];

    memset(bytes, 0xee, sizeof(bytes));
    for (unsigned k = 0; k < 64; k++) {
        bytes[(k / 16) * 32 + k % 16] = index_pixel(k % 16, k / 16);
    }
    qtest_memwrite(s, BUFFER_PAGE, bytes, sizeof(bytes));
    map_page(s, 0, BUFFER_PAGE);
    x_image_open(s, gc_index8, (uint32_t[]){ 0x10, 0, 1, 8, 16, 4, 0 },
                 100, 50);
    dma_packet(s, 0, 64, 16, 16, 0x100);
    assert_completed(s);
    kona_command(s, 0x13a, (uint32_t[]){ 0 }, 1);
    assert_index_image(s, 100, 50);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, GENERAL_INTERRUPT), ==, 1);
}

/*
 * The 16 by 4 image from a buffer one byte into a word: pad 8 bits, so
 * each row is five words, its first byte 0xee drawn nowhere. The same
 * pixels result and the pixel left of each row is untouched.
 */
static void test_pad(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint8_t bytes[80];

    memset(bytes, 0xee, sizeof(bytes));
    for (unsigned k = 0; k < 64; k++) {
        bytes[(k / 16) * 20 + 1 + k % 16] = index_pixel(k % 16, k / 16);
    }
    qtest_memwrite(s, BUFFER_PAGE, bytes, sizeof(bytes));
    map_page(s, 0, BUFFER_PAGE);
    x_image_open(s, gc_index8, (uint32_t[]){ 0x10, 0, 1, 8, 16, 4, 8 },
                 100, 50);
    dma_packet(s, 0, 80, 80, 0, 0x100);
    assert_completed(s);
    kona_command(s, 0x13a, (uint32_t[]){ 0 }, 1);
    assert_index_image(s, 100, 50);
    for (unsigned r = 0; r < 4; r++) {
        g_assert_cmphex(plane_at(s, PLANE_FRONT, 99, 50 + r), ==, 0);
    }
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_null(strstr(found, "past its rows"));
}

/*
 * A native TrueColor image retains its known DMA payload framing. Its
 * unreviewed conversion must leave canonical words and tags unchanged.
 */
static void test_truecolour_image(void)
{
    static const uint32_t words[8] = {
        0x00ff8000, 0x0000ff00, 0x000000ff, 0x00123456,
        0x00808080, 0x00ffffff, 0x00010203, 0x00fedcba,
    };
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint8_t bytes[32];

    for (unsigned k = 0; k < 8; k++) {
        stl_be_p(bytes + 4 * k, words[k]);
    }
    qtest_memwrite(s, BUFFER_PAGE, bytes, sizeof(bytes));
    map_page(s, 0, BUFFER_PAGE);
    x_image_open(s, gc_true24, (uint32_t[]){ 0x10, 2, 2, 8, 4, 2, 0 },
                 10, 10);
    dma_packet(s, 0, 32, 32, 0, 0x100);
    assert_completed(s);
    kona_command(s, 0x13a, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(shown_at(s, 10, 10), ==, 0);
    for (unsigned k = 0; k < 8; k++) {
        g_assert_cmphex(shown_at(s, 10 + k % 4, 10 + k / 4), ==, 0);
        g_assert_cmphex(plane_at(s, PLANE_FRONT_TAG, 10 + k % 4,
                                 10 + k / 4), ==, TAG_DIRECT);
    }
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, GENERAL_INTERRUPT), ==, 1);
}

/*
 * Word 6 of 0: the pixels are drawn and nothing is posted, no mailbox
 * word, no cause bit and no interrupt.
 */
static void test_no_completion_requested(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint8_t bytes[64];

    for (unsigned k = 0; k < 64; k++) {
        bytes[k] = index_pixel(k % 16, k / 16);
    }
    qtest_memwrite(s, BUFFER_PAGE, bytes, sizeof(bytes));
    map_page(s, 0, BUFFER_PAGE);
    x_image_open(s, gc_index8, (uint32_t[]){ 0x10, 0, 1, 8, 16, 4, 0 },
                 100, 50);
    dma_packet(s, 0, 64, 64, 0, 0);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, 0);
    g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0);
    kona_command(s, 0x13a, (uint32_t[]){ 0 }, 1);
    assert_index_image(s, 100, 50);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_null(strstr(found, "sgi_kona_general_interrupt "));
}

/*
 * The run's own transfer: a 256 by 256 8-bit image at (0, 0) in one
 * 64 KiB line at device address 0x179c8, map RAM entry 5 at offset
 * 0x39c8, the line crossing into entries 6 to 9, each mapping a host page
 * away from the one before. Every byte lands as the pixel it is: the
 * first and last rows, the rows where the line crosses a page and a
 * diagonal are checked. One interrupt, and no fatal bit.
 */
static void test_whole_line_64k(void)
{
    static const uint64_t pages[5] = {
        0x610000, 0x604000, 0x618000, 0x608000, 0x620000,
    };
    const uint32_t io = 0x179c8;
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    g_autofree uint8_t *image = g_malloc(0x10000);
    QTestState *s = kona_init_logged(&log);
    uint32_t done = 0;
    /* The line crosses a page in rows 6, 70, 134 and 198. */
    unsigned rows[] = { 0, 6, 70, 134, 198, 255 };

    for (unsigned k = 0; k < 0x10000; k++) {
        image[k] = (k * 7 + k / 256) & 0xff;
    }
    for (unsigned e = 0; e < 5; e++) {
        uint32_t start = e ? 0 : io & 0x3fff;
        uint32_t n = MIN(0x4000 - start, 0x10000 - done);

        map_page(s, 5 + e, pages[e]);
        qtest_memwrite(s, pages[e] + start, image + done, n);
        done += n;
    }
    g_assert_cmpuint(done, ==, 0x10000);
    x_image_open(s, gc_index8,
                 (uint32_t[]){ 0x10, 0, 1, 8, 0x100, 0x100, 0 }, 0, 0);
    dma_packet(s, io, 0x10000, 0x10000, 0, 0x100);
    assert_completed(s);
    kona_command(s, 0x13a, (uint32_t[]){ 0 }, 1);
    for (unsigned j = 0; j < ARRAY_SIZE(rows); j++) {
        for (unsigned x = 0; x < 256; x++) {
            g_assert_cmphex(plane_at(s, PLANE_FRONT, x, rows[j]), ==,
                            image[rows[j] * 256 + x]);
        }
    }
    for (unsigned d = 0; d < 256; d += 5) {
        g_assert_cmphex(plane_at(s, PLANE_FRONT, d, d), ==,
                        image[d * 256 + d]);
    }
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, GENERAL_INTERRUPT), ==, 1);
    g_assert_null(strstr(found, "refusing"));
}

/*
 * Six image writes in a row, each acknowledged as the driver does: one
 * read of the mailbox, then 0x10 written to the cause register, with no
 * mailbox write. Each completes with 0x100 and the acknowledgment clears
 * the bit; none is lost to a mailbox the earlier ones filled.
 */
static void test_completions_acknowledged(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint8_t bytes[64];

    for (unsigned k = 0; k < 64; k++) {
        bytes[k] = index_pixel(k % 16, k / 16);
    }
    qtest_memwrite(s, BUFFER_PAGE, bytes, sizeof(bytes));
    map_page(s, 0, BUFFER_PAGE);
    for (unsigned n = 0; n < 6; n++) {
        x_image_open(s, gc_index8,
                     (uint32_t[]){ 0x10, 0, 1, 8, 16, 4, 0 }, 100, 50 + n);
        dma_packet(s, 0, 64, 64, 0, 0x100);
        assert_completed(s);
        qtest_writel(s, REG_INT_CAUSE, CAUSE_MAILBOX);
        g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & CAUSE_MAILBOX, ==,
                        0);
        kona_command(s, 0x13a, (uint32_t[]){ 0 }, 1);
    }
    assert_index_image(s, 100, 55);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, GENERAL_INTERRUPT), ==, 6);
    g_assert_null(strstr(found, "mailbox full"));
}

/*
 * A packet with neither an X pixel write waiting nor a list record has
 * no destination a guest command names. It must leave pixels and guest
 * memory unchanged, without posting success or a fabricated fatal error.
 */
static void test_packet_without_destination(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint8_t before[64], after[64];

    memset(before, 0xa5, sizeof(before));
    qtest_memwrite(s, BUFFER_PAGE, before, sizeof(before));
    map_page(s, 0, BUFFER_PAGE);
    dma_packet(s, BUFFER_PAGE, 64, 64, 0, 0x100);
    for (unsigned x = 0; x < 16; x++) {
        g_assert_cmphex(plane_at(s, PLANE_FRONT, x, 0), ==, 0);
    }
    qtest_memread(s, BUFFER_PAGE, after, sizeof(after));
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, 0);
    g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "no list record and no X pixel write"
                                   " waiting; no pixels or completion"));
    g_assert_null(strstr(found, "sgi_kona_x_image_dma "));
}

/* The payload may finish before a later declared source read fails. */
static void test_late_unmapped_discards_completed_payload(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    qtest_memwrite(s, BUFFER_PAGE, (uint8_t[]){ 0x11, 0x22, 0x33, 0x44 }, 4);
    map_page(s, 0, BUFFER_PAGE);
    x_image_open(s, gc_index8, (uint32_t[]){ 0x10, 0, 1, 8, 4, 1, 0 },
                 70, 80);
    dma_packet(s, 0, 8, 4, 0x3ffc, 0x100);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & CAUSE_MAILBOX, ==, 0);
    kona_command(s, 0x13a, (uint32_t[]){ 0 }, 1);
    for (unsigned x = 0; x < 4; x++) {
        g_assert_cmphex(plane_at(s, PLANE_FRONT, 70 + x, 80), ==, 0);
        g_assert_cmphex(plane_at(s, PLANE_FRONT_TAG, 70 + x, 80), ==,
                        TAG_DIRECT);
    }
    x_image_open(s, gc_index8, (uint32_t[]){ 0x10, 0, 1, 8, 4, 1, 0 },
                 90, 80);
    dma_packet(s, 0, 4, 4, 0, 0x100);
    assert_completed(s);
    kona_command(s, 0x13a, (uint32_t[]){ 0 }, 1);
    for (unsigned x = 0; x < 4; x++) {
        g_assert_cmphex(plane_at(s, PLANE_FRONT, 90 + x, 80), ==,
                        0x11 * (x + 1));
        g_assert_cmphex(plane_at(s, PLANE_FRONT_TAG, 90 + x, 80), ==,
                        TAG_X_INDEX);
    }
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "maps nothing"));
}

static void test_unaligned_count_refuses_completion(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    qtest_memwrite(s, BUFFER_PAGE, (uint8_t[]){ 0x11, 0x22, 0x33, 0x44 }, 4);
    map_page(s, 0, BUFFER_PAGE);
    x_image_open(s, gc_index8, (uint32_t[]){ 0x10, 0, 1, 8, 4, 1, 0 },
                 70, 80);
    dma_packet(s, 0, 3, 3, 0, 0x100);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & CAUSE_MAILBOX, ==, 0);
    kona_command(s, 0x13a, (uint32_t[]){ 0 }, 1);
    for (unsigned x = 0; x < 4; x++) {
        g_assert_cmphex(plane_at(s, PLANE_FRONT, 70 + x, 80), ==, 0);
        g_assert_cmphex(plane_at(s, PLANE_FRONT_TAG, 70 + x, 80), ==,
                        TAG_DIRECT);
    }
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "not whole lines of whole words"));
}

static void test_chunked_extra_source(gconstpointer opaque)
{
    bool fail = GPOINTER_TO_UINT(opaque);
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint8_t bytes[8192];
    uint32_t io = fail ? 0x3000 : 0;

    memset(bytes, 0xee, sizeof(bytes));
    memcpy(bytes, (uint8_t[]){ 0x11, 0x22, 0x33, 0x44 }, 4);
    qtest_memwrite(s, BUFFER_PAGE + io, bytes, sizeof(bytes));
    map_page(s, 0, BUFFER_PAGE);
    x_image_open(s, gc_index8, (uint32_t[]){ 0x10, 0, 1, 8, 4, 1, 0 },
                 70, 80);
    dma_packet(s, io, sizeof(bytes), sizeof(bytes), 0, 0x100);
    if (fail) {
        g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & CAUSE_MAILBOX, ==, 0);
    } else {
        assert_completed(s);
    }
    for (unsigned x = 0; x < 4; x++) {
        g_assert_cmphex(plane_at(s, PLANE_FRONT, 70 + x, 80), ==, 0);
    }
    kona_command(s, 0x13a, (uint32_t[]){ 0 }, 1);
    for (unsigned x = 0; x < 4; x++) {
        g_assert_cmphex(plane_at(s, PLANE_FRONT, 70 + x, 80), ==,
                       fail ? 0 : 0x11 * (x + 1));
        g_assert_cmphex(plane_at(s, PLANE_FRONT_TAG, 70 + x, 80), ==,
                       fail ? TAG_DIRECT : TAG_X_INDEX);
    }
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, fail ? "maps nothing" :
                           "carried 8188 bytes past its rows"));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("sn0/origin2000/kona-xdma/chunks/read-all-extras",
        GUINT_TO_POINTER(0), test_chunked_extra_source);
    qtest_add_data_func("sn0/origin2000/kona-xdma/chunks/late-extra-unmapped",
        GUINT_TO_POINTER(1), test_chunked_extra_source);

    qtest_add_func("sn0/origin2000/kona-xdma/late-unmapped-after-payload",
                   test_late_unmapped_discards_completed_payload);
    qtest_add_func("sn0/origin2000/kona-xdma/unaligned-count",
                   test_unaligned_count_refuses_completion);

    qtest_add_func("sn0/origin2000/kona-xdma/index-image",
                   test_index_image_by_dma);
    qtest_add_func("sn0/origin2000/kona-xdma/no-pixels-before-bytes",
                   test_no_pixels_before_bytes);
    qtest_add_func("sn0/origin2000/kona-xdma/gathered-rows",
                   test_gathered_rows);
    qtest_add_func("sn0/origin2000/kona-xdma/pad", test_pad);
    qtest_add_func("sn0/origin2000/kona-xdma/truecolour-image",
                   test_truecolour_image);
    qtest_add_func("sn0/origin2000/kona-xdma/no-completion-requested",
                   test_no_completion_requested);
    qtest_add_func("sn0/origin2000/kona-xdma/whole-line-64k",
                   test_whole_line_64k);
    qtest_add_func("sn0/origin2000/kona-xdma/completions-acknowledged",
                   test_completions_acknowledged);
    qtest_add_func("sn0/origin2000/kona-xdma/packet-without-destination",
                   test_packet_without_destination);

    return g_test_run();
}
