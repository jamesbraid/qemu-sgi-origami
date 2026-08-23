/*
 * Crosstalk widget 0, which on an Origin 200 is the I/O bridge answering a
 * second time rather than a device of its own.
 *
 * One register in it is load-bearing for something larger than a register:
 * every guest reads widget 0's identification register first and decides what
 * kind of machine it is on from the answer. An Origin 200 has no crossbar, so
 * what answers is the bridge, and a guest reading a crossbar's identity instead
 * takes a different probe path entirely. These assertions are about the
 * machine's identity as much as about a device.
 *
 * Both windows onto widget 0 are exercised, because the two guests use
 * different ones. Linux reads the small window directly; OpenBSD redirects to
 * the last big window. Testing one path only would prove the register right and
 * the machine unreachable for whichever guest uses the other.
 *
 * What puts the big window there is the translation table's reset value, not
 * the firmware's redirection, which writes the value the entry already holds.
 * No assertion below can tell those apart, and one of them says so.
 *
 * One limit of these tests, stated because it would otherwise be assumed away:
 * where the machine is supposed to raise a bus error, a qtest cannot see one.
 * There is no processor executing, so a refused access and an access that
 * returned zero look identical from here, and every assertion below that means
 * "this must not answer" is written as "this must not answer *with the thing
 * that is really there*". That catches a model decoding to the wrong place,
 * which is the failure worth catching, and does not catch a model that answers
 * zero instead of faulting. The `-d unimp` log is what shows the difference,
 * and reading it is a step a person takes.
 *
 * There is a second limit, and it is sharper because it looks testable and is
 * not. The chip refuses an access wider than a crosstalk double-word, and a
 * qtest cannot show that either — but for a different reason, and one worth
 * writing down because the obvious test passes for the wrong reason rather than
 * failing. A guest's 64-bit load reaches the region as one 8-byte dispatch and
 * is refused on width. A qtest's 64-bit read does not: it goes through the
 * address space, which clamps the length to the region's declared maximum and
 * issues *two* 4-byte accesses instead. So the identity comes back in the low
 * half of the answer, from a perfectly legal narrow access, and a test
 * asserting "a wide read must not return the identity" fails against a
 * correct model. The
 * assertion was written, failed exactly that way, and is recorded here rather
 * than reworded into something that passes.
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
 * A widget to aim the big window at that is not the bridge, used below to prove
 * the translation entry steers the window. This machine has no crossbar, so
 * every non-zero widget but the Hub's answers as the bridge with widget 0's
 * identity; the Hub at widget 1 is the one target whose contents differ.
 */
#define IO_BWIN_HUB_WIDGET  1

/*
 * The entry the firmware writes for this redirection, term by term rather than
 * as a total. The space-select bit at 12 is clear: which of a widget's two
 * address spaces this redirection uses is unestablished, and this is the value
 * the only source addressing the case states.
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

/*
 * Every register is 32 bits in the low half of an eight-byte slot, so on this
 * big-endian machine the data sits at the slot's offset plus four, which is
 * the address both guests' headers name and the address the boot was measured
 * reading.
 */
#define WIDGET_IDENTIFICATION 0x0000
#define REG_DATA              4

/*
 * A read/write register of the bridge's, used to show that both windows reach
 * one chip rather than two chips agreeing. The request time-out register holds
 * a twenty-bit reload value and running it does nothing observable here, which
 * is what makes it safe to write from a test.
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
     * The part number is what each guest branches on. Assert the whole
     * identity rather than the part alone: a model reporting the right part
     * with a crossbar's manufacturer beside it would pass a part-only check and
     * would be a chip that exists nowhere.
     */
    g_assert_cmphex(ID_PART(id), ==, BRIDGE_PART);
    g_assert_cmphex(ID_MANUFACTURER(id), ==, BRIDGE_MANUFACTURER);

    /*
     * Stated as its own assertion because it is the specific wrong answer this
     * machine could plausibly give: a Crossbow reports part zero and
     * manufacturer zero, and a widget 0 answering that sends both guests down
     * the full-crossbar probe path, which reaches the same bridge by a
     * different route and so fails no other test here.
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
     * The identity above is only reachable because the firmware aimed the last
     * big window at widget 0, so this asserts the entry itself rather than
     * trusting that the read above went where it looks like it went. Without
     * this, a machine that happened to map widget 0 somewhere else would pass
     * the identity test and would not be the machine either guest expects.
     *
     * The whole entry is compared rather than the widget field, because the
     * space bit is the field this was wrong in once already.
     *
     * **This cannot tell that the firmware wrote anything.** The value the
     * sources give is widget zero, first slice, and the space select clear —
     * which is bit for bit what the register resets to, so a machine whose
     * firmware never ran passes this. That is not a hole to be plugged, it is
     * a fact about the hardware: the redirection writes the value the entry
     * already holds, and no assertion on the register can separate the two.
     * What this is worth is a guard on the constant, so that a later change to
     * either the entry number or the value has to be deliberate.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_BWIN_ENTRY(WIDGET0_WINDOW)),
                    ==, FIRMWARE_ENTRY);

    qtest_quit(qts);
}

static void test_upper_half_is_not_the_register(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The identity is in the low half of an eight-byte slot. A model that
     * answered with it at both halves would satisfy every guest we boot, since
     * none of them reads the upper half — and would be wrong about where the
     * register is, which is the kind of thing that only surfaces when something
     * later composes an address from it.
     */
    g_assert_cmphex(qtest_readl(qts, WIDGET0 + WIDGET_IDENTIFICATION), !=,
                    qtest_readl(qts, WIDGET0 + WIDGET_IDENTIFICATION + REG_DATA));

    qtest_quit(qts);
}

static void test_the_revision_is_settable(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,widget0-revision=4");

    /*
     * The revision is a candidate value — asserted from a captured boot of this
     * configuration, while another Origin 200's firmware announces a different
     * one — so it is settable, and two runs can differ in it and in nothing
     * else. A property nothing reads is a knob that reports success while
     * changing nothing, so this asserts that turning it reaches the register a
     * guest reads rather than only the machine's own state.
     */
    uint32_t id = qtest_readl(qts, WIDGET0 + WIDGET_IDENTIFICATION + REG_DATA);

    g_assert_cmpuint(ID_REVISION(id), ==, 4);

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
     * The two guests reach widget 0 by different routes and the machine owes
     * both. Linux reads the small window directly, using the address macro
     * whose name says raw rather than the substituting helper; OpenBSD
     * redirects to the last big window through the one function both its
     * identification paths call. Each uses the path the other does not.
     *
     * So the assertion is not that either window answers — it is that they
     * answer *the same*. A machine that mapped one and forgot the other passes
     * every single-window check and is broken for exactly one guest, which is
     * the failure this project has already shipped once here: the small window
     * was left unmapped on the strength of a claim about both guests that was
     * only ever true of one, and Linux found it with a bus error.
     */
    through_small = qtest_readl(qts, SPACE_XTALK + WIDGET_IDENTIFICATION +
                                REG_DATA);
    through_big = qtest_readl(qts, WIDGET0 + WIDGET_IDENTIFICATION + REG_DATA);

    g_assert_cmphex(through_small, ==, through_big);

    /* And it is the identity, rather than the two agreeing on a wrong value. */
    g_assert_cmphex(ID_PART(through_small), ==, BRIDGE_PART);
    g_assert_cmphex(ID_MANUFACTURER(through_small), ==, BRIDGE_MANUFACTURER);

    /*
     * The negative, without which the comparison above proves less than it
     * looks. Two addresses returning the same value is also what happens if
     * both land inside one over-broad mapping and the translation entry is
     * doing nothing at all — so aim entry 6 at a different widget and require
     * the big window to stop answering as widget 0. If it keeps answering, the
     * entry was never steering it and the agreement above was a coincidence of
     * layout.
     *
     * The target has to be the Hub. This machine has no crossbar, so every
     * non-zero widget but the Hub's is the bridge answering again with widget
     * 0's identity — aiming the window at one of those would change nothing and
     * prove nothing. Widget 1 is the one target whose contents differ.
     *
     * This also pins the thing OpenBSD depends on and nothing else here can
     * see: that big window 6 reaches widget 0 *because of the entry's
     * contents*. Our entries reset to zero, and zero spells widget 0, memory
     * space, offset 0 — so the path OpenBSD needs works with no firmware write
     * at all, which is a property of the reset value rather than of anything
     * that runs. A change satisfying the reset-to-zero requirement another way,
     * such as zeroing the entries but leaving the windows disabled, would pass
     * every Linux test and every Linux boot while OpenBSD's only route to
     * widget 0 went dark.
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
     * The assertion the earlier tests could not make, and the reason they could
     * not: comparing one *read-only* value through two windows cannot show that
     * the two windows reach one device. Two devices configured to report the
     * same identity pass it. Widget 0 used to be a separate model with a single
     * read-only register, so nothing a test could put in through one window was
     * readable through the other, and the limitation was recorded rather than
     * closed.
     *
     * Widget 0 is now the I/O bridge's own widget space, seen a second time, so
     * the bridge's writable registers are reachable through it and state put in
     * through one window has to come out of the other. That is the property
     * that distinguishes one chip from two dressed alike.
     *
     * Widget 8 is where the firmware puts the bridge and widget 0 is the same
     * chip, so this writes through one and reads through the other. Which
     * direction hardly matters; using the same window for both would assert
     * nothing.
     */
    reset = qtest_readl(qts, BRIDGE_WINDOW + BRIDGE_REQUEST_TIMEOUT + REG_DATA);
    g_assert_cmphex(reset, ==,
                    qtest_readl(qts, SPACE_XTALK + BRIDGE_REQUEST_TIMEOUT +
                                REG_DATA));

    /*
     * A value that is not the reset value, so the read below cannot pass by
     * finding what was already there. Masked to the field's width because the
     * register keeps only twenty bits and a wider write would read back
     * truncated, which would fail this test for a reason that is not the one it
     * is asking about.
     */
    written = (reset ^ 0x5a5a5u) & REQUEST_TIMEOUT_MASK;
    g_assert_cmphex(written, !=, reset);

    qtest_writel(qts, BRIDGE_WINDOW + BRIDGE_REQUEST_TIMEOUT + REG_DATA,
                 written);

    g_assert_cmphex(qtest_readl(qts, SPACE_XTALK + BRIDGE_REQUEST_TIMEOUT +
                                REG_DATA), ==, written);

    /*
     * And through the big window as well, which is the path OpenBSD takes. All
     * three addresses are one register.
     */
    g_assert_cmphex(qtest_readl(qts, WIDGET0 + BRIDGE_REQUEST_TIMEOUT +
                                REG_DATA), ==, written);

    qtest_quit(qts);
}

static void test_widget_0_answers_without_the_bridge_option(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=off");

    /*
     * The machine option decides whether the bridge answers at its own widget.
     * It must not decide whether widget 0 answers, because OpenBSD panics
     * outright when widget 0's identification register does not read — so
     * gating the alias on it would turn a development option into a machine no
     * guest can boot, and nothing else here would notice.
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
