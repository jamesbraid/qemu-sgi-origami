/*
 * The I/O bridge answers at every non-zero crosstalk widget, not only at the
 * widget SGI's firmware picks for it.
 *
 * With no crossbar every widget address reaches the bridge, and the IRIX
 * 6.5.30 miniroot relies on it (it names widget 2 at a bridge window).
 * Widgets 2-7 and 9-15 answer as one chip; widget 0 keeps its alias;
 * widget 1 stays the Hub.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#include <elf.h>

#define SPACE_XTALK       0x20000000000ULL

#define SMALL_WINDOW_SIZE 0x1000000ULL
#define SMALL_WINDOW(w)   (SPACE_XTALK + (w) * SMALL_WINDOW_SIZE)

/* The widget SGI's firmware picks for a directly connected bridge. */
#define BRIDGE_WIDGET     8

/* Widget 1 is the Hub, reached through the same small-window layout. */
#define HUB_WIDGET        1

/* Registers sit at their slot's offset plus four. */
#define WIDGET_IDENTIFICATION  0x0000
#define REG_DATA               4

/* A writable register safe to poke, to prove one chip behind the windows. */
#define BRIDGE_REQUEST_TIMEOUT 0x0028
#define REQUEST_TIMEOUT_MASK   0xfffffu

/* The exact address the miniroot's storm faulted on. */
#define DEVICE1_PIO_WINDOW     0x400000
#define STORM_OFFSET           (DEVICE1_PIO_WINDOW + 0x208)

/* The completion word, sn0-bridge-test.c's convention. */
#define GUEST_MARKER_ADDR      0x30000
#define GUEST_MARKER           0x5a17c0deu

#define ID_PART(v)          (((v) >> 12) & 0xffff)
#define ID_MANUFACTURER(v)  (((v) >> 1) & 0x7ff)

/* What an Origin 200 answers with: the I/O bridge's part and manufacturer. */
#define BRIDGE_PART         0xc002
#define BRIDGE_MANUFACTURER 0x036

/* PCI plumbing for the split-order test, mirroring sn0-bridge-test.c. */
#define BRIDGE_BASE            SMALL_WINDOW(BRIDGE_WIDGET)
#define REG(off)               (BRIDGE_BASE + (off) + REG_DATA)
#define BRIDGE_DEVICE(n)       (0x200 + 8 * (n))
#define DEV_MEMORY_SPACE       (1u << 12)
#define BRIDGE_CFG0            0x020000
#define CFG0(dev, fn)          (BRIDGE_BASE + BRIDGE_CFG0 + \
                                (dev) * 0x1000 + (fn) * 0x100)
#define PCI_BAR1               0x14
#define PCI_COMMAND            0x04
#define PCI_COMMAND_MEMORY     0x0002
#define CARD_MMIO(slot)        (0x10000000u + (slot) * 0x1000000u)
#define ORDER_SLOT             4          /* a free PCI slot for the scratch card */
#define ORDER_WINDOW           0x800000   /* the device-4 window's widget offset */
/* lsi53c895a SCRATCHB/SCRATCHC: adjacent, side-effect-free, spannable. */
#define LSI_SCRATCHB           0x5c

/* The widgets the widen adds: every non-zero one but the Hub's and the
 * bridge's own. */
static const unsigned int new_widgets[] = {
    2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15,
};

static uint32_t widget_identity(QTestState *qts, unsigned int widget)
{
    return qtest_readl(qts,
                       SMALL_WINDOW(widget) + WIDGET_IDENTIFICATION + REG_DATA);
}

static void test_bridge_answers_at_every_widget(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* Widget 8 still answers; the widen must not disturb it. */
    uint32_t at8 = widget_identity(qts, BRIDGE_WIDGET);
    g_assert_cmphex(ID_PART(at8), ==, BRIDGE_PART);
    g_assert_cmphex(ID_MANUFACTURER(at8), ==, BRIDGE_MANUFACTURER);

    /* Each net-new widget reads the same identity pair. */
    for (size_t i = 0; i < ARRAY_SIZE(new_widgets); i++) {
        uint32_t id = widget_identity(qts, new_widgets[i]);

        g_assert_cmphex(ID_PART(id), ==, BRIDGE_PART);
        g_assert_cmphex(ID_MANUFACTURER(id), ==, BRIDGE_MANUFACTURER);
    }

    qtest_quit(qts);
}

static void test_new_widgets_are_one_bridge(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint32_t reset, written;

    /* Written state, not a shared constant, is what proves one chip. */
    reset = qtest_readl(qts, SMALL_WINDOW(BRIDGE_WIDGET) +
                        BRIDGE_REQUEST_TIMEOUT + REG_DATA);

    /* Not the reset value; masked so truncation cannot fail it wrongly. */
    written = (reset ^ 0x5a5a5u) & REQUEST_TIMEOUT_MASK;
    g_assert_cmphex(written, !=, reset);

    qtest_writel(qts, SMALL_WINDOW(BRIDGE_WIDGET) + BRIDGE_REQUEST_TIMEOUT +
                 REG_DATA, written);

    for (size_t i = 0; i < ARRAY_SIZE(new_widgets); i++) {
        g_assert_cmphex(qtest_readl(qts, SMALL_WINDOW(new_widgets[i]) +
                                    BRIDGE_REQUEST_TIMEOUT + REG_DATA),
                        ==, written);
    }

    /* And the write is visible back through the window it went in by. */
    g_assert_cmphex(qtest_readl(qts, SMALL_WINDOW(BRIDGE_WIDGET) +
                                BRIDGE_REQUEST_TIMEOUT + REG_DATA),
                    ==, written);

    qtest_quit(qts);
}

/*
 * An eight-byte load through net-new widget 0xa; the marker lands only if
 * it returned. Widget 2 at this offset is the PROM's deliberate PI error
 * probe, tested separately.
 */
static char *write_doubleword_load_guest(void)
{
    static const uint32_t code[] = {
        0x3c089200,             /* lui    t0, 0x9200                        */
        0x0008403c,             /* dsll32 t0, t0, 0                         */
        0x3c090a40,             /* lui    t1, 0x0a40                        */
        0x35290208,             /* ori    t1, t1, 0x0208 -> 0x0a40_0208     */
        0x01094025,             /* or     t0, t0, t1  -> widget 0xa + 0x400208 */
        0xdd0a0000,             /* ld     t2, 0(t0)   -- 8-byte load, w0xa   */
        0x3c089600,             /* lui    t0, 0x9600                        */
        0x0008403c,             /* dsll32 t0, t0, 0                         */
        0x3c090003,             /* lui    t1, 0x0003                        */
        0x01094025,             /* or     t0, t0, t1  -> phys 0x30000        */
        0x3c095a17,             /* lui    t1, 0x5a17                        */
        0x3529c0de,             /* ori    t1, t1, 0xc0de -> 0x5a17c0de       */
        0xad090000,             /* sw     t1, 0(t0)   -- load returned       */
        0x1000ffff,             /* b      .                                 */
        0x00000000,             /* nop                                      */
    };
    uint8_t image[0x1000 + sizeof(code)] = { 0 };
    const uint64_t entry = 0x8000000000020000ULL;
    GError *err = NULL;
    char *path = NULL;
    int fd;
    size_t i;

    fd = g_file_open_tmp("sn0-widget-decode-guest-XXXXXX", &path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(fd, >=, 0);

    image[0] = ELFMAG0;
    image[1] = ELFMAG1;
    image[2] = ELFMAG2;
    image[3] = ELFMAG3;
    image[EI_CLASS] = ELFCLASS64;
    image[EI_DATA] = ELFDATA2MSB;
    image[EI_VERSION] = EV_CURRENT;
    stw_be_p(image + 16, ET_EXEC);
    stw_be_p(image + 18, EM_MIPS);
    stl_be_p(image + 20, EV_CURRENT);
    stq_be_p(image + 24, entry);
    stq_be_p(image + 32, 64);
    stw_be_p(image + 52, 64);
    stw_be_p(image + 54, 56);
    stw_be_p(image + 56, 1);

    stl_be_p(image + 64, PT_LOAD);
    stl_be_p(image + 68, PF_R | PF_X);
    stq_be_p(image + 72, 0x1000);
    stq_be_p(image + 80, entry);
    stq_be_p(image + 88, entry);
    stq_be_p(image + 96, sizeof(code));
    stq_be_p(image + 104, sizeof(code));
    stq_be_p(image + 112, 0x1000);

    for (i = 0; i < ARRAY_SIZE(code); i++) {
        stl_be_p(image + 0x1000 + i * sizeof(code[0]), code[i]);
    }

    g_assert_cmpint(write(fd, image, sizeof(image)), ==, (ssize_t)sizeof(image));
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_other_widget_doubleword_load_completes(const void *data)
{
    g_autofree char *kernel = write_doubleword_load_guest();
    QTestState *qts;
    uint32_t marker = 0;

    /*
     * Must run a real ld: a qtest wide read is chunked into legal four-byte
     * accesses and passes even at a four-byte cap. Both dimensions were
     * watched go red by reverting each fix alone.
     */
    g_assert_true(g_setenv("QTEST_QEMU_ARGS", "-accel tcg", true));
    qts = qtest_initf("-M origin200 -smp 1 -kernel %s", kernel);
    g_unsetenv("QTEST_QEMU_ARGS");

    g_test_timer_start();
    while (marker != GUEST_MARKER && g_test_timer_elapsed() < 1) {
        marker = qtest_readl(qts, GUEST_MARKER_ADDR);
        g_usleep(1000);
    }

    g_assert_cmphex(marker, ==, GUEST_MARKER);
    qtest_quit(qts);
    g_unlink(kernel);
}

static void test_doubleword_split_order(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 "
                                 "-device lsi53c895a,addr=4");
    uint64_t at2 = SMALL_WINDOW(2) + ORDER_WINDOW + LSI_SCRATCHB;

    /*
     * The split order alone: high word at the lower offset. Reversed, the
     * two half reads below would swap.
     */
    qtest_writel(qts, CFG0(ORDER_SLOT, 0) + PCI_BAR1, CARD_MMIO(ORDER_SLOT));
    qtest_writel(qts, CFG0(ORDER_SLOT, 0) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, REG(BRIDGE_DEVICE(ORDER_SLOT)),
                 DEV_MEMORY_SPACE | (CARD_MMIO(ORDER_SLOT) >> 20));

    qtest_writeq(qts, at2, 0x1122334455667788ULL);

    g_assert_cmphex(qtest_readl(qts, at2), ==, 0x11223344u);
    g_assert_cmphex(qtest_readl(qts, at2 + 4), ==, 0x55667788u);

    qtest_quit(qts);
}

static void test_widget_1_stays_the_hub(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* Widget 1 must stay the Hub -- a real read, not the zero of absence. */
    g_assert_cmphex(ID_PART(widget_identity(qts, HUB_WIDGET)), !=, BRIDGE_PART);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/widget-decode/bridge-answers-at-every-widget",
                        NULL, test_bridge_answers_at_every_widget);
    qtest_add_data_func("sn0/widget-decode/new-widgets-are-one-bridge",
                        NULL, test_new_widgets_are_one_bridge);
    qtest_add_data_func("sn0/widget-decode/other-widget-doubleword-completes",
                        NULL, test_other_widget_doubleword_load_completes);
    qtest_add_data_func("sn0/widget-decode/doubleword-split-order",
                        NULL, test_doubleword_split_order);
    qtest_add_data_func("sn0/widget-decode/widget-1-stays-the-hub",
                        NULL, test_widget_1_stays_the_hub);
    return g_test_run();
}
