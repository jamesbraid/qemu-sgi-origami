/*
 * SGI SN0 Hub ASIC — the registers a guest reads before it has a console.
 *
 * Two of these decide things a guest cannot recover from. It reads the
 * processor-number register to learn which of a node's two processors it is,
 * and the network-interface status register to learn whether nodes are numbered
 * in M mode or N mode, which sets the shift it forms every node-relative
 * address with afterwards. Both have to read zero on an unprogrammed Origin
 * 200, and a model that invented a different default would send the guest down
 * a path it announces in one word of output and gets wrong everywhere
 * underneath.
 *
 * The window is reached twice: once at an address that resolves to whichever
 * node the reading processor is on, which is how firmware reaches its own Hub
 * before node numbers are assigned, and once at an address naming a node. Both
 * have to decode, because a guest uses one to read its processor number and the
 * other to clear interrupt state in the same boot.
 *
 * The big-window tests below have a limit worth stating rather than leaving to
 * be discovered: a qtest cannot see a bus error, because no processor is
 * executing, so an access that reached nothing and an access that was answered
 * with zero are the same thing from here. Every assertion that means "this
 * window must not reach the device" is therefore written as "this must not
 * return what the device holds", which catches a window decoding to the wrong
 * place and does not catch a window that answers zero rather than faulting.
 * Each of them puts a distinctive value in the device first so that "what the
 * device holds" is not itself zero.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* Crosstalk space, and the two halves of the crossbar's widget 1 window. */
#define SPACE_XTALK     0x20000000000ULL
#define HUB_LOCAL       (SPACE_XTALK + 0x01000000)
#define HUB_BY_NODE     (SPACE_XTALK + 0x01800000)

#define PI_REGION_PRESENT   0x000018
#define PI_PROCESSOR_NUMBER 0x000020
#define PI_CALIAS_SIZE      0x000028
#define PI_INT_MODIFY       0x000090
#define PI_INT_PEND0        0x000098
#define PI_INT_PEND1        0x0000a0
#define PI_INT_MASK0_A      0x0000a8
#define PI_INT_MASK1_A      0x0000b0
#define PI_INT_MASK0_B      0x0000b8
#define PI_INT_MASK1_B      0x0000c0
#define IO_WIDGET_ID        0x400000
#define IO_WIDGET_STATUS    0x400008
#define IO_WIDGET_CONTROL   0x400020
#define IO_XTALK_TIMEOUT    0x4003b8
#define IO_XTALK_PRESCALE   0x4003c0
#define IO_LINK_STATUS      0x400128
#define NI_STATUS           0x600000

#define NI_MORE_NODES   (1ULL << 18)
#define NI_REGION_SIZE  (1ULL << 17)
#define NI_NODE_MASK    (0x1ffULL << 8)
#define NI_IDENTITY     0xffULL          /* revision and chip identity */

/*
 * The interrupt-modify register's two fields: the interrupt number, and the bit
 * that says whether to set the named pending bit or clear it. Numbers 0 through
 * 63 are the first pending bank and 64 through 127 the second.
 */
#define INT_MODIFY_SET      0x100
#define INT_BANK_BITS       64

/* The big-window translation table, entry n at 8n from its base. */
#define IO_BWIN_TABLE   0x400160
#define IO_BWIN_STRIDE  8
#define IO_BWIN_ENTRIES 7
#define IO_BWIN_ENTRY(n) (IO_BWIN_TABLE + (n) * IO_BWIN_STRIDE)
#define IO_BWIN_WIDGET(w) ((uint64_t)(w) << 8)
#define IO_BWIN_SLICE(x)  ((uint64_t)(x))
/*
 * The space select. It chooses between the widget's two address spaces, and the
 * machine models the one the redirection uses; a window asking for the other is
 * reported and reaches nothing. Which of the two names that is stays
 * unestablished, so nothing here asserts it — what is asserted is that the two
 * do not both answer.
 */
#define IO_BWIN_OTHER_SPACE  (1ULL << 12)

/* The widget number SGI's firmware names to turn a window off. */
#define IO_BWIN_INVALID_WIDGET 3

/*
 * The seven big windows themselves, and something to find at the far end of
 * one. The base I/O controller sits at the third device window of widget 8, and
 * its serial port A has a scratch register that stores whatever is put in it —
 * a byte that can be written through one window and read back through another,
 * which is what says the two windows reached one device rather than two.
 */
#define BIG_WINDOW_BASE 0x20000000
#define BIG_WINDOW_SIZE 0x20000000ULL
#define BIG_WINDOW(n)   (SPACE_XTALK + BIG_WINDOW_BASE + (n) * BIG_WINDOW_SIZE)

#define SMALL_WINDOW(w) (SPACE_XTALK + (uint64_t)(w) * 0x1000000)
#define CONSOLE_WIDGET  8
#define IOC3_SCRATCH    (0x600000 + 0x020178 + 7)

static void test_addressing_mode(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t status = qtest_readq(qts, HUB_LOCAL + NI_STATUS);

    /*
     * Zero in the more-nodes bit is M mode. One would make the guest print that
     * it is in N mode and compute every node-relative address with a different
     * shift.
     */
    g_assert_cmphex(status & NI_MORE_NODES, ==, 0);

    /*
     * This Hub is node zero. OpenBSD does not read this field, but Linux takes
     * its node number from it, so it has to be right for both.
     */
    g_assert_cmphex(status & NI_NODE_MASK, ==, 0);

    /* One region per node, which is what the field resets to. */
    g_assert_cmphex(status & NI_REGION_SIZE, ==, NI_REGION_SIZE);

    qtest_quit(qts);
}

static void test_processor_number(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The first processor of the node. Everything per-processor is selected
     * from this.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_PROCESSOR_NUMBER), ==, 0);

    qtest_quit(qts);
}

static void test_reset_values(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* An unprogrammed Hub claims every memory region exists. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_REGION_PRESENT), ==,
                    ~0ULL);

    /* The cached alias resets to its largest size code. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CALIAS_SIZE), ==, 15);

    /* Nothing is pending before anything is wired up. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND1), ==, 0);

    qtest_quit(qts);
}

static void test_writes_the_guest_makes(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    const uint64_t masks[] = {
        PI_INT_MASK0_A, PI_INT_MASK1_A, PI_INT_MASK0_B, PI_INT_MASK1_B,
    };
    unsigned i;

    /*
     * The four interrupt masks: a guest writes zero to all of them during setup
     * and later writes real masks, so they have to remember what was written.
     * The IRIX register table calls them read-only, which the Hub datasheet
     * records as wrong; a read-only model would silently drop every interrupt a
     * guest tried to enable.
     */
    for (i = 0; i < ARRAY_SIZE(masks); i++) {
        qtest_writeq(qts, HUB_LOCAL + masks[i], 0);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + masks[i]), ==, 0);

        qtest_writeq(qts, HUB_LOCAL + masks[i], 0x0123456789abcdefULL);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + masks[i]), ==,
                        0x0123456789abcdefULL);
    }

    /* A guest writes all ones here, and zero to the alias size. */
    qtest_writeq(qts, HUB_LOCAL + PI_REGION_PRESENT, ~0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_REGION_PRESENT), ==,
                    ~0ULL);

    qtest_writeq(qts, HUB_LOCAL + PI_CALIAS_SIZE, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CALIAS_SIZE), ==, 0);

    qtest_quit(qts);
}

static void test_status_field_access(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t status;

    /*
     * Firmware assigns node numbers through this register, so the field is
     * writable.
     */
    qtest_writeq(qts, HUB_LOCAL + NI_STATUS, NI_NODE_MASK);
    status = qtest_readq(qts, HUB_LOCAL + NI_STATUS);
    g_assert_cmphex(status & NI_NODE_MASK, ==, NI_NODE_MASK);

    /* What the hardware reports about itself is not writable. */
    g_assert_cmphex(status & NI_IDENTITY, ==, 2 << 4);

    qtest_quit(qts);
}

static void test_both_windows(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The same Hub either way on a single-node machine, and both halves have to
     * decode. A write through one has to be visible through the other, which is
     * what says they are one device rather than two.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + NI_STATUS), ==,
                    qtest_readq(qts, HUB_LOCAL + NI_STATUS));
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + PI_PROCESSOR_NUMBER), ==,
                    0);

    qtest_writeq(qts, HUB_BY_NODE + PI_INT_MASK0_A, 0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_MASK0_A), ==,
                    0xfedcba9876543210ULL);

    qtest_quit(qts);
}

static void test_translation_entries_decode(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    unsigned n;

    /*
     * Seven entries, eight bytes apart, each its own register. Writing a
     * different value to every one and reading them all back afterwards is what
     * separates seven registers from one register decoded seven ways — a model
     * that aliased them would return the last value written for all of them and
     * would pass any test that wrote and read one at a time.
     */
    for (n = 0; n < IO_BWIN_ENTRIES; n++) {
        qtest_writeq(qts, HUB_LOCAL + IO_BWIN_ENTRY(n),
                     IO_BWIN_WIDGET(8 + n) |
                     IO_BWIN_SLICE(n));
    }
    for (n = 0; n < IO_BWIN_ENTRIES; n++) {
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_BWIN_ENTRY(n)), ==,
                        IO_BWIN_WIDGET(8 + n) |
                        IO_BWIN_SLICE(n));
    }

    /*
     * There is no eighth. SGI's manuals number these seven entries one through
     * seven, so the register one stride past the last is the one someone
     * counting from the manual would write; it is not a translation entry and
     * must not behave like one.
     */
    qtest_writeq(qts, HUB_LOCAL + IO_BWIN_ENTRY(IO_BWIN_ENTRIES), ~0ULL);
    g_assert_cmphex(qtest_readq(qts,
                                HUB_LOCAL + IO_BWIN_ENTRY(IO_BWIN_ENTRIES)),
                    ==, 0);

    qtest_quit(qts);
}

static void test_translation_entry_width(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Thirteen bits and nothing above them. The entry has no valid bit — a
     * window is turned off by naming a widget that is not there — so there is
     * no bit above 12 for a model to invent one in, and a guest writing a wider
     * value must not find it stored.
     */
    qtest_writeq(qts, HUB_LOCAL + IO_BWIN_ENTRY(0), ~0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_BWIN_ENTRY(0)), ==, 0x1fff);

    qtest_quit(qts);
}

static void test_big_window_follows_its_entry(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Aim big window 0 at the widget the base I/O controller is on, at that
     * widget's first slice, and the controller has to answer through the window
     * as well as through its small one. Writing through the small window and
     * reading through the big one is the assertion: equal values from two
     * addresses say one device, where a read alone would be satisfied by any
     * model that answered plausibly at both.
     */
    qtest_writeq(qts, HUB_LOCAL + IO_BWIN_ENTRY(0),
                 IO_BWIN_WIDGET(CONSOLE_WIDGET) |
                 IO_BWIN_SLICE(0));

    qtest_writeb(qts, SMALL_WINDOW(CONSOLE_WIDGET) + IOC3_SCRATCH, 0x5a);
    g_assert_cmphex(qtest_readb(qts, BIG_WINDOW(0) + IOC3_SCRATCH), ==, 0x5a);

    /* And the other way, so neither is the direction that happens to work. */
    qtest_writeb(qts, BIG_WINDOW(0) + IOC3_SCRATCH, 0xa5);
    g_assert_cmphex(qtest_readb(qts, SMALL_WINDOW(CONSOLE_WIDGET) +
                                IOC3_SCRATCH), ==, 0xa5);

    qtest_quit(qts);
}

static void test_big_window_follows_the_slice_too(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The entry's low five bits choose which 512 MB slice of the widget the
     * window lands on, and they are the half of the entry a model can drop
     * without any of the tests above noticing: name the right widget, ignore
     * the slice, and every access still reaches the right device.
     *
     * So aim a window at the same widget one slice further up, where nothing
     * is, and require that the controller does *not* answer. Asserting where a
     * window does not reach is the only form of this that a model ignoring the
     * field cannot pass.
     */
    qtest_writeb(qts, SMALL_WINDOW(CONSOLE_WIDGET) + IOC3_SCRATCH, 0x3c);

    qtest_writeq(qts, HUB_LOCAL + IO_BWIN_ENTRY(1),
                 IO_BWIN_WIDGET(CONSOLE_WIDGET) |
                 IO_BWIN_SLICE(1));
    g_assert_cmphex(qtest_readb(qts, BIG_WINDOW(1) + IOC3_SCRATCH), !=, 0x3c);

    /* Move it back to the first slice and the same address reaches the chip. */
    qtest_writeq(qts, HUB_LOCAL + IO_BWIN_ENTRY(1),
                 IO_BWIN_WIDGET(CONSOLE_WIDGET) |
                 IO_BWIN_SLICE(0));
    g_assert_cmphex(qtest_readb(qts, BIG_WINDOW(1) + IOC3_SCRATCH), ==, 0x3c);

    qtest_quit(qts);
}

static void test_big_window_naming_an_absent_widget(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * How the guests disable a window: there is no valid bit, so they program
     * an invalid widget number instead, and SGI's firmware uses widget three
     * for it. The entry has to accept it, and an access through the window has
     * to reach nothing.
     *
     * This writes the register directly rather than imitating a guest's disable
     * helper, and that is deliberate. **The helper's arguments are crossed in
     * most releases** — it passes the space constant where the window number
     * goes and the window number where the space bit goes — so a guest asking
     * to
     * disable window n actually writes entry zero, leaves window n alone, and
     * spills n across the space select. IRIX 6.5 and 6.5.5f both have it
     * crossed and 6.5.5f calls it live; 6.5.30 has it fixed; Linux inherited
     * the
     * crossed form. A test asserting that disabling window n leaves entry n
     * naming widget three would encode what the macro meant rather than what
     * most guests do, and would fail against a real IRIX while the model was
     * right. What is asserted here is the register's own contract, which is
     * what the model owes either way.
     *
     * Reaching nothing must not fault, which is the part of this a qtest cannot
     * check and the part most easily got wrong. A disabled window is the
     * ordinary state of six of the seven — every entry resets that way — so a
     * gap in crosstalk space would turn each of them into a bus error on the
     * first read. What this can check is that the model is still answering
     * afterwards, which the last read does.
     */
    qtest_writeq(qts, HUB_LOCAL + IO_BWIN_ENTRY(2),
                 IO_BWIN_WIDGET(IO_BWIN_INVALID_WIDGET));
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_BWIN_ENTRY(2)), ==,
                    IO_BWIN_WIDGET(IO_BWIN_INVALID_WIDGET));

    qtest_readb(qts, BIG_WINDOW(2));

    /*
     * The other way to reach nothing, and the one that looks like it should
     * work: the right widget, asking for the address space this machine does
     * not model. A window there is as disabled as one naming a widget that is
     * not there, and for the same reason — nothing is mapped where it points.
     */
    qtest_writeb(qts, SMALL_WINDOW(CONSOLE_WIDGET) + IOC3_SCRATCH, 0x77);
    qtest_writeq(qts, HUB_LOCAL + IO_BWIN_ENTRY(3),
                 IO_BWIN_WIDGET(CONSOLE_WIDGET) | IO_BWIN_OTHER_SPACE);
    g_assert_cmphex(qtest_readb(qts, BIG_WINDOW(3) + IOC3_SCRATCH), !=, 0x77);

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_PROCESSOR_NUMBER), ==, 0);

    qtest_quit(qts);
}

static void test_interrupt_modify_sets_and_clears(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The pending banks are read-only, so this register is the only way a bit
     * gets set or cleared, and it is where a device interrupt arrives. A model
     * that discarded these writes would look correct until the second time a
     * device needed attention — nothing reads the register back, so no
     * assertion anywhere else can notice.
     */
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, INT_MODIFY_SET | 12);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 1ULL << 12);

    /* And nothing else: not a neighbouring bit, and not the other bank. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND1), ==, 0);

    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, 12);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 0);

    qtest_quit(qts);
}

static void test_interrupt_number_selects_the_bank(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The number's top bit is what makes 64 through 127 the second bank rather
     * than a separate register. Number 70 is bit 6 of the second bank, and a
     * model masking the number to six bits would put it at bit 6 of the first —
     * a plausible-looking bit in the wrong bank, which the guest experiences as
     * an interrupt delivered to the wrong handler.
     */
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, INT_MODIFY_SET | 70);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND1), ==,
                    1ULL << (70 - INT_BANK_BITS));
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 0);

    qtest_quit(qts);
}

static void test_a_masked_pending_bit_raises_nothing(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Pending is node-wide and masking is per processor, so a bit set with its
     * mask clear is pending and invisible. Setting the mask afterwards has to
     * raise the line with no further write to the pending state — which is the
     * assertion that separates a model recomputing its outputs from one that
     * raises a line at the moment a pending bit is written.
     *
     * What a qtest can see of that is the pending state rather than the line
     * itself; the line is checked by the boot, where a guest that never gets
     * its interrupt is the symptom. So this asserts the half that is visible
     * and is named here as half.
     */
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK0_A, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, INT_MODIFY_SET | 12);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 1ULL << 12);

    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK0_A, 1ULL << 12);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 1ULL << 12);

    qtest_quit(qts);
}

static void test_the_pending_banks_reject_writes(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Read-only, and worth asserting rather than assuming: a model that let a
     * guest write them directly would work for any guest that only ever used
     * the modify register, and would silently accept a driver that did not.
     */
    qtest_writeq(qts, HUB_LOCAL + PI_INT_PEND0, ~0ULL);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_PEND1, ~0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND1), ==, 0);

    qtest_quit(qts);
}

static void test_the_crosstalk_link_reports_up(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t link = qtest_readq(qts, HUB_LOCAL + IO_LINK_STATUS);
    unsigned int state = (link >> 12) & 3;

    /*
     * The assertion that keeps mainline Linux's crosstalk probe alive, and it
     * deliberately contradicts the register table's reset of zero. Linux reads
     * this register, tests the high bit of the link-state field, and if it is
     * clear returns immediately with no retry, no timeout and no message — so
     * a machine reporting its link down has no crossbar, no bridge, no PCI, no
     * console and no disk, and nothing anywhere saying why.
     *
     * Written as "two or three" rather than as a constant because both mean
     * working and nothing we have distinguishes them; what must hold is that
     * the bit Linux tests is set. A test asserting the exact value would be
     * asserting a choice this model made, and a test asserting only "non-zero"
     * would pass on one, which means the link never came up.
     */
    g_assert_cmpuint(state, >=, 2);
    g_assert_cmphex(link & (1ULL << 13), ==, 1ULL << 13);

    /*
     * No guest we boot reads this. OpenBSD never touches the register, so this
     * assertion is the only thing in the project watching a value that decides
     * whether a different guest sees any hardware at all.
     */
    g_assert_cmphex(link & 0x3fc00000000ULL, ==, 0x3fc00000000ULL);
    g_assert_cmphex(link & 0x3ff0000ULL, ==, 0x3ff0000ULL);
    g_assert_cmphex(link & 0x3fULL, ==, 0xa);

    qtest_quit(qts);
}

static void test_a_guest_cannot_write_the_link_state(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The link state is what the hardware reports about itself. A model that
     * let it be written would let a guest tell this machine its own link was
     * down, which is the one value here that must not be settable — and a
     * writable-everything model passes every read-back test.
     */
    qtest_writeq(qts, HUB_LOCAL + IO_LINK_STATUS, 0);
    g_assert_cmpuint((qtest_readq(qts, HUB_LOCAL + IO_LINK_STATUS) >> 12) & 3,
                     >=, 2);

    /* What a guest may change, it changes. */
    qtest_writeq(qts, HUB_LOCAL + IO_LINK_STATUS, 0x15);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_LINK_STATUS) & 0x3f, ==,
                    0x15);

    qtest_quit(qts);
}

static void test_the_hub_identifies_itself_on_the_crossbar(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t id = qtest_readq(qts, HUB_LOCAL + IO_WIDGET_ID);

    /*
     * The one assertion that proves the Hub is present and identifiable to
     * anything walking the crossbar, which reads this register first.
     */
    g_assert_cmphex((id >> 12) & 0xffff, ==, 0xc101);
    g_assert_cmpuint((id >> 28) & 0xf, !=, 0);

    /*
     * The lowest bit is set from the crosstalk contract every widget shares
     * rather than from any Hub source — no Hub document declares it, and it is
     * here because the identification format requires it. Asserted so that the
     * inheritance is deliberate rather than incidental.
     */
    g_assert_cmphex(id & 1, ==, 1);

    /*
     * The manufacturer is deliberately *not* compared against a constant. Two
     * sources name it and they disagree, nothing on any path we boot reads it,
     * and the datasheet asks for it to be a choice the machine description
     * makes. So what is asserted is that the machine's choice reaches the
     * register — a constant here would harden one of two disputed readings into
     * a test, which is how a guess becomes a fact nobody remembers choosing.
     */
    qtest_quit(qts);

    qts = qtest_init("-M origin200 -global sgi-hub.manufacturer=0x24");
    id = qtest_readq(qts, HUB_LOCAL + IO_WIDGET_ID);
    g_assert_cmphex((id >> 1) & 0x7ff, ==, 0x24);
    g_assert_cmphex((id >> 12) & 0xffff, ==, 0xc101);

    qtest_quit(qts);
}

static void test_the_widget_control_reset_values(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t control = qtest_readq(qts, HUB_LOCAL + IO_WIDGET_CONTROL);

    /*
     * The widget number resetting to one is the register-level counterpart of
     * the address map putting the Hub at widget 1. A model resetting this to
     * zero would contradict its own decode, and nothing else in the machine
     * would notice — no guest reads it on a single-node board.
     */
    g_assert_cmphex(control & 0xf, ==, 1);

    /* And the crossbar credit, the other reset value that is not zero. */
    g_assert_cmphex((control >> 13) & 7, ==, 2);

    /*
     * The status register reports nothing, because this model never has an
     * error, a retry or an outstanding request. Bit 4 is undefined in SGI's own
     * table and reads zero rather than being invented.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_WIDGET_STATUS), ==, 0);

    qtest_quit(qts);
}

static void test_the_crosstalk_timeouts(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The first two registers mainline Linux writes when it initialises a Hub,
     * with the values it writes. Both must accept a write straight out of reset
     * with no unlock step — a model requiring one would fail a guest at the
     * very start of its Hub setup, before anything it printed could say why.
     */
    qtest_writeq(qts, HUB_LOCAL + IO_XTALK_PRESCALE, 0x800);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_XTALK_PRESCALE), ==, 0x800);

    qtest_writeq(qts, HUB_LOCAL + IO_XTALK_TIMEOUT, 0xff);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_XTALK_TIMEOUT), ==, 0xff);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/hub/addressing-mode", NULL, test_addressing_mode);
    qtest_add_data_func("sn0/hub/processor-number", NULL,
                        test_processor_number);
    qtest_add_data_func("sn0/hub/reset-values", NULL, test_reset_values);
    qtest_add_data_func("sn0/hub/guest-writes", NULL,
                        test_writes_the_guest_makes);
    qtest_add_data_func("sn0/hub/status-field-access", NULL,
                        test_status_field_access);
    qtest_add_data_func("sn0/hub/both-windows", NULL, test_both_windows);
    qtest_add_data_func("sn0/hub/translation-entries-decode", NULL,
                        test_translation_entries_decode);
    qtest_add_data_func("sn0/hub/translation-entry-width", NULL,
                        test_translation_entry_width);
    qtest_add_data_func("sn0/hub/big-window-follows-its-entry", NULL,
                        test_big_window_follows_its_entry);
    qtest_add_data_func("sn0/hub/big-window-follows-the-slice", NULL,
                        test_big_window_follows_the_slice_too);
    qtest_add_data_func("sn0/hub/big-window-absent-widget", NULL,
                        test_big_window_naming_an_absent_widget);
    qtest_add_data_func("sn0/hub/identifies-itself", NULL,
                        test_the_hub_identifies_itself_on_the_crossbar);
    qtest_add_data_func("sn0/hub/widget-control-reset", NULL,
                        test_the_widget_control_reset_values);
    qtest_add_data_func("sn0/hub/crosstalk-timeouts", NULL,
                        test_the_crosstalk_timeouts);
    qtest_add_data_func("sn0/hub/crosstalk-link-reports-up", NULL,
                        test_the_crosstalk_link_reports_up);
    qtest_add_data_func("sn0/hub/link-state-is-not-writable", NULL,
                        test_a_guest_cannot_write_the_link_state);
    qtest_add_data_func("sn0/hub/interrupt-modify", NULL,
                        test_interrupt_modify_sets_and_clears);
    qtest_add_data_func("sn0/hub/interrupt-number-selects-bank", NULL,
                        test_interrupt_number_selects_the_bank);
    qtest_add_data_func("sn0/hub/masked-pending-raises-nothing", NULL,
                        test_a_masked_pending_bit_raises_nothing);
    qtest_add_data_func("sn0/hub/pending-banks-reject-writes", NULL,
                        test_the_pending_banks_reject_writes);

    return g_test_run();
}
