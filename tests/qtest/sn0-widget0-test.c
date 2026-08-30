/*
 * Crosstalk widget 0: on an Origin 200, the I/O bridge answering a second
 * time. Guests read its identification register first and decide the
 * machine's shape from it, and the two guests reach it by different windows
 * -- Linux the small window, OpenBSD the last big window -- so both paths
 * are asserted.
 *
 * Two limits. A qtest cannot see a bus error, so "must not answer" is
 * written as "must not answer with the thing that is really there"; the
 * -d unimp log tells refusal from zero. And the width refusal is untestable
 * from here for a sharper reason: a qtest's wide read is split into legal
 * narrow accesses by the address space, so the obvious wide-read assertion
 * fails against a correct model.
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

/*
 * A retarget for the big window: the Hub at widget 1 is the one widget whose
 * contents differ from the bridge's on this crossbar-less machine.
 */
#define IO_BWIN_HUB_WIDGET  1

/*
 * The firmware's redirection entry; the clear space-select bit is the value
 * the one source addressing the case states.
 */
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

/*
 * A writable bridge register safe to poke from a test, used to prove the
 * windows reach one chip rather than two chips agreeing.
 */
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

    /*
     * The whole identity, not the part alone: the right part beside a
     * crossbar's manufacturer is a chip that exists nowhere.
     */
    g_assert_cmphex(ID_PART(id), ==, BRIDGE_PART);
    g_assert_cmphex(ID_MANUFACTURER(id), ==, BRIDGE_MANUFACTURER);

    /*
     * The specific wrong answer: a Crossbow's zero/zero sends both guests
     * down the crossbar probe path and fails no other test here.
     */
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
     * The whole entry, space bit included. This cannot tell that firmware
     * wrote anything -- the redirection writes the reset value -- so what
     * it pins is the constant: a change to entry or value must be
     * deliberate.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_BWIN_ENTRY(WIDGET0_WINDOW)),
                    ==, FIRMWARE_ENTRY);

    qtest_quit(qts);
}

static void test_upper_half_is_not_the_register(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Both halves answering the identity would satisfy every guest we boot
     * and be wrong about where the register is.
     */
    g_assert_cmphex(qtest_readl(qts, WIDGET0 + WIDGET_IDENTIFICATION), !=,
                    qtest_readl(qts, WIDGET0 + WIDGET_IDENTIFICATION + REG_DATA));

    qtest_quit(qts);
}

static void test_the_revision_is_settable(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,widget0-revision=3");

    /*
     * The revision is a candidate value, so it is settable -- and a property
     * nothing reads is a knob reporting success while changing nothing, so
     * turning it must reach the register a guest reads.
     */
    uint32_t id = qtest_readl(qts, WIDGET0 + WIDGET_IDENTIFICATION + REG_DATA);

    g_assert_cmpuint(ID_REVISION(id), ==, 3);

    /* And the rest of the identity is unmoved: one value changed, not two. */
    g_assert_cmphex(ID_PART(id), ==, BRIDGE_PART);
    g_assert_cmphex(ID_MANUFACTURER(id), ==, BRIDGE_MANUFACTURER);

    qtest_quit(qts);
}

static void test_both_windows_reach_one_device(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint32_t through_small, through_big;

    /*
     * Not that either window answers -- that they answer the same. A machine
     * mapping one and forgetting the other passes every single-window check
     * and is broken for exactly one guest.
     */
    through_small = qtest_readl(qts, SPACE_XTALK + WIDGET_IDENTIFICATION +
                                REG_DATA);
    through_big = qtest_readl(qts, WIDGET0 + WIDGET_IDENTIFICATION + REG_DATA);

    g_assert_cmphex(through_small, ==, through_big);

    /* And it is the identity, rather than the two agreeing on a wrong value. */
    g_assert_cmphex(ID_PART(through_small), ==, BRIDGE_PART);
    g_assert_cmphex(ID_MANUFACTURER(through_small), ==, BRIDGE_MANUFACTURER);

    /*
     * The negative: agreement alone is also what one over-broad mapping
     * gives. Retarget the entry at the Hub -- the one widget whose contents
     * differ -- and require the big window to stop answering as widget 0.
     * This is what pins OpenBSD's route existing because of the entry's
     * reset value: entries zeroed but windows disabled would pass every
     * Linux test while OpenBSD's only route went dark.
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

    /*
     * A read-only value agreeing through two windows cannot tell one chip
     * from two dressed alike; state written through one window and read
     * through the other can. Same window both ways would assert nothing.
     */
    reset = qtest_readl(qts, BRIDGE_WINDOW + BRIDGE_REQUEST_TIMEOUT + REG_DATA);
    g_assert_cmphex(reset, ==,
                    qtest_readl(qts, SPACE_XTALK + BRIDGE_REQUEST_TIMEOUT +
                                REG_DATA));

    /*
     * Not the reset value, so the read cannot pass by finding what was
     * there; masked to the field so truncation cannot fail it for the
     * wrong reason.
     */
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

static void test_widget_0_answers_without_the_bridge_option(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=off");

    /*
     * io-bridge=off must not silence widget 0: OpenBSD panics when its
     * identification register does not read, and nothing else would notice.
     */
    uint32_t id = qtest_readl(qts, SPACE_XTALK + WIDGET_IDENTIFICATION +
                              REG_DATA);

    g_assert_cmphex(ID_PART(id), ==, BRIDGE_PART);
    g_assert_cmphex(ID_MANUFACTURER(id), ==, BRIDGE_MANUFACTURER);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/widget0/identity-is-the-bridges", NULL,
                        test_identity_is_the_bridges);
    qtest_add_data_func("sn0/widget0/is-the-bridge-itself", NULL,
                        test_widget_0_is_the_bridge_itself);
    qtest_add_data_func("sn0/widget0/answers-without-the-bridge-option", NULL,
                        test_widget_0_answers_without_the_bridge_option);
    qtest_add_data_func("sn0/widget0/reached-through-the-last-big-window", NULL,
                        test_it_is_where_the_firmware_put_it);
    qtest_add_data_func("sn0/widget0/both-windows-reach-one-device", NULL,
                        test_both_windows_reach_one_device);
    qtest_add_data_func("sn0/widget0/upper-half-of-the-slot", NULL,
                        test_upper_half_is_not_the_register);
    qtest_add_data_func("sn0/widget0/revision-is-settable", NULL,
                        test_the_revision_is_settable);

    return g_test_run();
}
