/*
 * The crossbar's register block, which answers at crosstalk widget 0.
 *
 * Only one register in it is load-bearing, and it is load-bearing for something
 * larger than a register: both guests read widget 0's identification register
 * first and decide what kind of machine they are on from the answer. An Origin
 * 200 does not report a crossbar there, it reports the I/O bridge's identity,
 * and a guest that reads a crossbar's identity instead takes a different probe
 * path entirely. So these assertions are about the machine's identity as much
 * as about a device.
 *
 * Widget 0 is reached only through a big window here. Its small window is the
 * one a Hub defect makes unreliable on real hardware, so the machine leaves it
 * unmapped and the firmware aims the last big window at widget 0 instead —
 * which is the arrangement both guests expect, and the arrangement these tests
 * exercise, because testing the identity through a path no guest uses would
 * prove the register right and the machine unreachable.
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
 * The entry the firmware writes for this redirection, term by term rather than
 * as a total. The space-select bit at 12 is clear: which of a widget's two
 * address spaces this redirection uses is unestablished, and this is the value
 * the only source addressing the case states.
 */
#define FIRMWARE_ENTRY  (IO_BWIN_WIDGET(0) | IO_BWIN_SLICE(0))

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
#define XBOW_IDENTIFICATION 0x0000
#define REG_DATA            4

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
    uint32_t id = qtest_readl(qts, WIDGET0 + XBOW_IDENTIFICATION + REG_DATA);

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
     * space bit is the field this was wrong in: a window on widget 0's memory
     * space reads plausible values that are not its registers, and every other
     * assertion in this file would still pass if the identity happened to be
     * reachable anyway.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_BWIN_ENTRY(WIDGET0_WINDOW)),
                    ==, FIRMWARE_ENTRY);

    qtest_quit(qts);
}

static void test_small_window_zero_is_not_the_way_in(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint32_t through_the_window =
        qtest_readl(qts, WIDGET0 + XBOW_IDENTIFICATION + REG_DATA);

    /*
     * Widget 0's small window is the one the Hub defect makes unreliable, and
     * the machine leaves it unmapped so that a guest reaching for it says so
     * instead of quietly working. If it answered, this model would be a machine
     * on which the workaround both guests carry is unnecessary — which is a
     * different machine, and one that would hide a missing big window.
     */
    g_assert_cmphex(qtest_readl(qts, SPACE_XTALK + XBOW_IDENTIFICATION +
                                REG_DATA), !=, through_the_window);

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
    g_assert_cmphex(qtest_readl(qts, WIDGET0 + XBOW_IDENTIFICATION), !=,
                    qtest_readl(qts, WIDGET0 + XBOW_IDENTIFICATION + REG_DATA));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/xbow/identity-is-the-bridges", NULL,
                        test_identity_is_the_bridges);
    qtest_add_data_func("sn0/xbow/reached-through-the-last-big-window", NULL,
                        test_it_is_where_the_firmware_put_it);
    qtest_add_data_func("sn0/xbow/small-window-zero-is-not-the-way-in", NULL,
                        test_small_window_zero_is_not_the_way_in);
    qtest_add_data_func("sn0/xbow/upper-half-of-the-slot", NULL,
                        test_upper_half_is_not_the_register);

    return g_test_run();
}
