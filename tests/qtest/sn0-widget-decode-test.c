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
#include "sn0-user-image.h"
#include "sgi-test.h"

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
#define BRIDGE_CFG0            0x020000
#define CFG0(dev, fn)          (BRIDGE_BASE + BRIDGE_CFG0 + \
                                (dev) * 0x1000 + (fn) * 0x100)
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
 * probe, tested separately. Runs from the reset vector now rather than a
 * directly loaded kernel, so it sets kernel mode (with KX, for the xkphys
 * forms below) itself instead of inheriting it from a firmware handoff.
 */
static void build_doubleword_load_image(SN0Image *p)
{
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);

    /*
     * widget 0xa + 0x400208, the same xkphys form as sn0-bridge-test.c's
     * PROM PI probe: an 8-byte load, not a qtest chunked read.
     */
    sn0_emit_li64(p, SN0_R_T0, UINT64_C(0x9200000000000000) | 0x0a400208);
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_T0, SN0_R_T2, 0);

    sn0_emit_li64(p, SN0_R_T0,
                  UINT64_C(0x9600000000000000) | GUEST_MARKER_ADDR);
    sn0_emit_li(p, SN0_R_T1, GUEST_MARKER);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_T0, SN0_R_T1, 0);
    sn0_emit_park(p);
}

static void test_other_widget_doubleword_load_completes(const void *data)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *qts;
    uint32_t marker = 0;

    build_doubleword_load_image(image);
    path = sn0_image_write(image, "sn0-widget-decode-guest");

    /*
     * Must run a real ld: a qtest wide read is chunked into legal four-byte
     * accesses and passes even at a four-byte cap. Both dimensions were
     * watched go red by reverting each fix alone.
     */
    g_assert_true(g_setenv("QTEST_QEMU_ARGS", "-accel tcg", true));
    qts = qtest_initf("-M origin200 -smp 1 -bios %s", path);
    g_unsetenv("QTEST_QEMU_ARGS");
    unlink(path);

    g_test_timer_start();
    while (marker != GUEST_MARKER && g_test_timer_elapsed() < 1) {
        marker = qtest_readl(qts, GUEST_MARKER_ADDR);
        g_usleep(1000);
    }

    g_assert_cmphex(marker, ==, GUEST_MARKER);
    qtest_quit(qts);
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
