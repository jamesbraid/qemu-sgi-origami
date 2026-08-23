/*
 * The I/O bridge answers at every non-zero crosstalk widget, not only at the
 * widget SGI's firmware picks for it.
 *
 * An Origin 200 has no crossbar. The Hub's one crosstalk link ends at the
 * bridge, and with nothing in between to route by widget number, every widget
 * address the Hub emits reaches the same chip. Firmware relies on this: it is
 * free to stamp a device pointer with whichever widget it likes, and the IRIX
 * 6.5.30 miniroot holds one naming widget 2 at the bridge's device-1 PIO
 * window. A model that decodes the bridge at widget 8 alone answers that load
 * with a bus error, and the miniroot's exception handler resumes the same load
 * forever — the data-bus-error storm this decode was widened to end.
 *
 * These assert the widened decode: the bridge answers at the net-new widgets
 * 2 through 7 and 9 through 15, at the same registers, as one chip, while
 * widget 0 keeps its own alias, widget 1 stays the Hub, and widget 8 is
 * unchanged.
 *
 * The limits the widget-0 test records hold here too. A qtest has no executing
 * processor, so a refused access and an access that returned zero look alike
 * from here: every "reaches the bridge" assertion below is written as "reads
 * what the bridge really holds" — a non-zero identity, or a value first written
 * through a window known to work — which catches a decode that lands nowhere.
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

/*
 * Every register is 32 bits in the low half of an eight-byte slot, so on this
 * big-endian machine the data sits at the slot offset plus four.
 */
#define WIDGET_IDENTIFICATION  0x0000
#define REG_DATA               4

/*
 * A read/write register of the bridge's, used to show that two windows reach
 * one chip rather than two chips agreeing on a constant. The request-timeout
 * register holds a twenty-bit reload value and writing it does nothing
 * observable here.
 */
#define BRIDGE_REQUEST_TIMEOUT 0x0028
#define REQUEST_TIMEOUT_MASK   0xfffffu

/*
 * The exact address the storm faulted on: crosstalk widget 2, register offset
 * 0x400208 — the bridge's device-1 PIO window (0x400000) plus 0x208. The guest
 * issued it as an eight-byte load of virtual 0x9200000002400208, physical
 * 0x20002400208, which is SMALL_WINDOW(2) + this offset.
 */
#define DEVICE1_PIO_WINDOW     0x400000
#define STORM_OFFSET           (DEVICE1_PIO_WINDOW + 0x208)

/*
 * Where the guest fragment writes its completion word, and the value, matching
 * the convention sn0-bridge-test.c uses for the same kind of check.
 */
#define GUEST_MARKER_ADDR      0x30000
#define GUEST_MARKER           0x5a17c0deu

#define ID_PART(v)          (((v) >> 12) & 0xffff)
#define ID_MANUFACTURER(v)  (((v) >> 1) & 0x7ff)

/* What an Origin 200 answers with: the I/O bridge's part and manufacturer. */
#define BRIDGE_PART         0xc002
#define BRIDGE_MANUFACTURER 0x036

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

    /*
     * Widget 8, the firmware's own choice, still answers — the widen adds the
     * others and must not disturb it.
     */
    uint32_t at8 = widget_identity(qts, BRIDGE_WIDGET);
    g_assert_cmphex(ID_PART(at8), ==, BRIDGE_PART);
    g_assert_cmphex(ID_MANUFACTURER(at8), ==, BRIDGE_MANUFACTURER);

    /*
     * And each net-new widget reads the same identity, which is the whole
     * point: on a crossbar-less machine they are all the one bridge. The part
     * alone would pass for a model reporting it beside a wrong manufacturer, so
     * assert the pair.
     */
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

    /*
     * Reading one identity through many windows cannot tell one chip from many
     * configured alike. Writing a register through the firmware's widget and
     * reading it back through each net-new widget can: state put in through one
     * has to come out of the others only if they are the same chip.
     */
    reset = qtest_readl(qts, SMALL_WINDOW(BRIDGE_WIDGET) +
                        BRIDGE_REQUEST_TIMEOUT + REG_DATA);

    /*
     * A value that is not the reset value, so the reads below cannot pass by
     * finding what was already there. Masked to the field's width because the
     * register keeps only twenty bits and a wider write would read back
     * truncated.
     */
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
 * A guest fragment that issues the exact storm access — an eight-byte load of
 * the device-1 window at offset 0x400208 — through two net-new widgets, 2 and
 * 0xa, and writes a completion marker to RAM only if both loads returned. If
 * the decode does not reach the widget, or the window refuses the doubleword,
 * the load raises a bus error, the marker never lands, and the test times out.
 * The addresses are built in registers because they do not fit an immediate:
 * 0x9200000002400208 and 0x920000000a400208 are uncached (0x92) widget windows.
 */
static char *write_doubleword_load_guest(void)
{
    static const uint32_t code[] = {
        0x3c089200,             /* lui    t0, 0x9200                        */
        0x0008403c,             /* dsll32 t0, t0, 0   -> 0x9200_0000_...    */
        0x3c090240,             /* lui    t1, 0x0240                        */
        0x35290208,             /* ori    t1, t1, 0x0208 -> 0x0240_0208     */
        0x01094025,             /* or     t0, t0, t1  -> widget 2 + 0x400208 */
        0xdd0a0000,             /* ld     t2, 0(t0)   -- 8-byte load, w2     */
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
        0xad090000,             /* sw     t1, 0(t0)   -- both loads returned */
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

static void test_the_storm_doubleword_load_completes(const void *data)
{
    g_autofree char *kernel = write_doubleword_load_guest();
    QTestState *qts;
    uint32_t marker = 0;

    /*
     * The load-bearing check, and it must run a real instruction. The storm is
     * an eight-byte load, and its two dimensions were separate defects: the
     * decode has to reach widget 2 (the widen) and the device window has to
     * accept a doubleword (valid.max_access_size = 8). A qtest wide read cannot
     * test the second — qtest_readq goes through address_space_read, which
     * chunks a wide read into four-byte accesses and so passes even at
     * valid.max = 4, the same chunking sn0-widget0-test.c documents. Only a
     * single ld issues the one eight-byte dispatch that faults when either
     * dimension is missing, so the load has to come from an executing
     * processor. The marker lands only if the doubleword loads at widgets 2 and
     * 0xa both returned; a bus error on either leaves it absent and this times
     * out. The 32-bit identity reads above are breadth, not this — they are the
     * weaker property the first draft of this test mistook for the check.
     */
    g_assert_true(g_setenv("QTEST_QEMU_ARGS", "-accel tcg", true));
    qts = qtest_initf("-M origin200,io-bridge=on -smp 1 -kernel %s", kernel);
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

static void test_widget_1_stays_the_hub(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The Hub keeps widget 1. Its small window is where the widen would do
     * harm if the loop reached too low, so require that widget 1 does not read
     * as the bridge. The Hub answers here with its own registers, so this is a
     * real read of a present device, not the zero of an absence.
     */
    g_assert_cmphex(ID_PART(widget_identity(qts, HUB_WIDGET)), !=, BRIDGE_PART);

    qtest_quit(qts);
}

static void test_widgets_gated_with_the_bridge(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=off");

    /*
     * With the bridge option off the net-new widgets go quiet, matching the
     * bridge's own widget 8, which also stops answering. Only widget 0 keeps
     * the bridge's identity, because OpenBSD panics without it — the same
     * unconditional alias the widget-0 test pins. This locks the choice that
     * the option means "bridge maximally absent", not present at a scatter of
     * widgets while its own is gone.
     */
    uint32_t at0 = widget_identity(qts, 0);
    g_assert_cmphex(ID_PART(at0), ==, BRIDGE_PART);

    g_assert_cmphex(ID_PART(widget_identity(qts, 2)), !=, BRIDGE_PART);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/widget-decode/bridge-answers-at-every-widget",
                        NULL, test_bridge_answers_at_every_widget);
    qtest_add_data_func("sn0/widget-decode/new-widgets-are-one-bridge",
                        NULL, test_new_widgets_are_one_bridge);
    qtest_add_data_func("sn0/widget-decode/storm-doubleword-load-completes",
                        NULL, test_the_storm_doubleword_load_completes);
    qtest_add_data_func("sn0/widget-decode/widget-1-stays-the-hub",
                        NULL, test_widget_1_stays_the_hub);
    qtest_add_data_func("sn0/widget-decode/gated-with-the-bridge",
                        NULL, test_widgets_gated_with_the_bridge);

    return g_test_run();
}
