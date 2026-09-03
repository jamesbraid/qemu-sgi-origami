/*
 * Widget 0: the I/O bridge answering a second time. Linux reads the small
 * window, OpenBSD the last big window; both paths are asserted. A qtest
 * cannot see a bus error (assertions read what is really there instead),
 * and cannot issue a genuinely wide access (the address space splits it).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define SPACE_XTALK     0x20000000000ULL
#define HUB_LOCAL       (SPACE_XTALK + 0x01000000)

#define IO_BWIN_TABLE   0x400160
#define IO_BWIN_STRIDE  8
#define IO_BWIN_ENTRY(n) (IO_BWIN_TABLE + (n) * IO_BWIN_STRIDE)
#define IO_BWIN_WIDGET(w) ((uint64_t)(w) << 8)
#define IO_BWIN_SLICE(x)  ((uint64_t)(x))

/* The Hub at widget 1: the one widget whose contents differ. */
#define IO_BWIN_HUB_WIDGET  1

/* The firmware's redirection entry, space-select clear per its one source. */
#define FIRMWARE_ENTRY  (IO_BWIN_WIDGET(0) | IO_BWIN_SLICE(0))

#define SMALL_WINDOW_SIZE 0x1000000ULL
#define SMALL_WINDOW(w)   (SPACE_XTALK + (w) * SMALL_WINDOW_SIZE)

/* The widget number SGI's firmware picks for a directly connected bridge. */
#define BRIDGE_WIDGET   8
#define BRIDGE_WINDOW   SMALL_WINDOW(BRIDGE_WIDGET)

#define BIG_WINDOW_SIZE 0x20000000ULL
#define BIG_WINDOW(n)   (SPACE_XTALK + 0x20000000 + (n) * BIG_WINDOW_SIZE)

/* The window the firmware redirects widget 0 onto, and where widget 0 is. */
#define WIDGET0_WINDOW  6
#define WIDGET0         BIG_WINDOW(WIDGET0_WINDOW)

/* Registers sit at their slot's offset plus four, as both guests name. */
#define WIDGET_IDENTIFICATION 0x0000
#define REG_DATA              4

/* A writable register safe to poke, to prove one chip behind two windows. */
#define BRIDGE_REQUEST_TIMEOUT 0x0028
#define REQUEST_TIMEOUT_MASK   0xfffffu

#define ID_REVISION(v)      (((v) >> 28) & 0xf)
#define ID_PART(v)          (((v) >> 12) & 0xffff)
#define ID_MANUFACTURER(v)  (((v) >> 1) & 0x7ff)
#define ID_ALWAYS_ONE(v)    ((v) & 1)

/* What an Origin 200 answers with: the I/O bridge's part and manufacturer. */
#define BRIDGE_PART         0xc002
#define BRIDGE_MANUFACTURER 0x036

/* What a real Crossbow answers with, which is what this must not be. */
#define XBOW_PART           0
#define XBOW_MANUFACTURER   0

static void test_identity_is_the_bridges(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint32_t id = qtest_readl(qts, WIDGET0 + WIDGET_IDENTIFICATION + REG_DATA);

    /* Right part beside a wrong manufacturer is no chip; check both. */
    g_assert_cmphex(ID_PART(id), ==, BRIDGE_PART);
    g_assert_cmphex(ID_MANUFACTURER(id), ==, BRIDGE_MANUFACTURER);

    /* A Crossbow's zero/zero fails no other test here; name it. */
    g_assert_false(ID_PART(id) == XBOW_PART &&
                   ID_MANUFACTURER(id) == XBOW_MANUFACTURER);

    /* The lowest bit of a crosstalk widget's identity is always set. */
    g_assert_cmpuint(ID_ALWAYS_ONE(id), ==, 1);

    /* And a revision, which distinguishes an answer from an absence. */
    g_assert_cmpuint(ID_REVISION(id), !=, 0);

    qtest_quit(qts);
}

static void test_it_is_where_the_firmware_put_it(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Pins the constant only: the redirection writes the reset value, so
     * no register assertion can tell that firmware wrote.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_BWIN_ENTRY(WIDGET0_WINDOW)),
                    ==, FIRMWARE_ENTRY);

    qtest_quit(qts);
}

static void test_upper_half_is_not_the_register(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* Both halves answering would misplace the register. */
    g_assert_cmphex(qtest_readl(qts, WIDGET0 + WIDGET_IDENTIFICATION), !=,
                    qtest_readl(qts, WIDGET0 + WIDGET_IDENTIFICATION + REG_DATA));

    qtest_quit(qts);
}

static void test_both_windows_reach_one_device(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint32_t through_small, through_big;

    /* Not that either answers: that they answer the same. */
    through_small = qtest_readl(qts, SPACE_XTALK + WIDGET_IDENTIFICATION +
                                REG_DATA);
    through_big = qtest_readl(qts, WIDGET0 + WIDGET_IDENTIFICATION + REG_DATA);

    g_assert_cmphex(through_small, ==, through_big);

    /* And it is the identity, rather than the two agreeing on a wrong value. */
    g_assert_cmphex(ID_PART(through_small), ==, BRIDGE_PART);
    g_assert_cmphex(ID_MANUFACTURER(through_small), ==, BRIDGE_MANUFACTURER);

    /*
     * The negative: agreement is also what one over-broad mapping gives,
     * so retarget the entry and require the answer to change.
     */
    qtest_writeq(qts, HUB_LOCAL + IO_BWIN_ENTRY(WIDGET0_WINDOW),
                 IO_BWIN_WIDGET(IO_BWIN_HUB_WIDGET));
    g_assert_cmphex(qtest_readl(qts, WIDGET0 + WIDGET_IDENTIFICATION + REG_DATA),
                    !=, through_big);

    /* The small window is unaffected: it does not go through a translation. */
    g_assert_cmphex(qtest_readl(qts, SPACE_XTALK + WIDGET_IDENTIFICATION +
                                REG_DATA), ==, through_small);

    qtest_quit(qts);
}

static void test_widget_0_is_the_bridge_itself(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint32_t reset, written;

    /* Written state through one window, read through the other: one chip. */
    reset = qtest_readl(qts, BRIDGE_WINDOW + BRIDGE_REQUEST_TIMEOUT + REG_DATA);
    g_assert_cmphex(reset, ==,
                    qtest_readl(qts, SPACE_XTALK + BRIDGE_REQUEST_TIMEOUT +
                                REG_DATA));

    /* Not the reset value; masked so truncation cannot fail it wrongly. */
    written = (reset ^ 0x5a5a5u) & REQUEST_TIMEOUT_MASK;
    g_assert_cmphex(written, !=, reset);

    qtest_writel(qts, BRIDGE_WINDOW + BRIDGE_REQUEST_TIMEOUT + REG_DATA,
                 written);

    g_assert_cmphex(qtest_readl(qts, SPACE_XTALK + BRIDGE_REQUEST_TIMEOUT +
                                REG_DATA), ==, written);

    /* And through the big window: all three addresses are one register. */
    g_assert_cmphex(qtest_readl(qts, WIDGET0 + BRIDGE_REQUEST_TIMEOUT +
                                REG_DATA), ==, written);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/widget0/identity-is-the-bridges", NULL,
                        test_identity_is_the_bridges);
    qtest_add_data_func("sn0/widget0/is-the-bridge-itself", NULL,
                        test_widget_0_is_the_bridge_itself);
    qtest_add_data_func("sn0/widget0/reached-through-the-last-big-window", NULL,
                        test_it_is_where_the_firmware_put_it);
    qtest_add_data_func("sn0/widget0/both-windows-reach-one-device", NULL,
                        test_both_windows_reach_one_device);
    qtest_add_data_func("sn0/widget0/upper-half-of-the-slot", NULL,
                        test_upper_half_is_not_the_register);
    return g_test_run();
}
