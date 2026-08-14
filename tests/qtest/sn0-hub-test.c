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

/*
 * The slot-identity register, and the boot flash's ready signal at both of the
 * bit positions it appears at. Asserting only one would pass against a model
 * that reports ready at that one and hangs firmware polling the other.
 */
#define MD_SLOT_ID_USTAT            0x220048
#define MD_SLOT_FLASH_READY_LOW     (1ULL << 0)
#define MD_SLOT_FLASH_READY_HIGH    (1ULL << 4)

#define PI_REGION_PRESENT   0x000018
#define PI_PROCESSOR_NUMBER 0x000020
#define PI_CALIAS_SIZE      0x000028
#define PI_CALIAS_SIZE_RESET 15
#define PI_CPU_ENABLE_A     0x000050
#define PI_CPU_ENABLE_B     0x000058

/*
 * Where each processor spools error records -- and, on a single-processor
 * machine, where the firmware parks a marker across its own two-pass reset,
 * because processor B's copy belongs to a processor that is not fitted.
 * Bits 31:3; the low three bits of a written value do not survive.
 */
#define PI_ERR_SPOOL_A       0x000418
#define PI_ERR_SPOOL_B       0x000420
#define PI_ERR_SPOOL_MASK    0xfffffff8ULL
#define PI_RT_COUNT         0x030100
#define PI_RT_COMPARE_A     0x000108
#define PI_RT_PENDING_A     0x000120
#define PI_RT_ENABLE_A      0x000140
#define PI_RT_LOCAL_CTRL       0x000160
#define PI_RT_LOCAL_CTRL_MASK  0x100ffULL
#define PI_RT_LOCAL_CTRL_RESET 0x10000ULL

/*
 * The Hub's outgoing lines, by the order it registers them: processor A's two
 * pending banks, both counter comparators, then processor B's two pending
 * banks. Intercepting them is what lets a test see the line itself rather than
 * only the pending bit behind it.
 */
#define HUB_IRQ_LINES       "sysbus-irq"
#define HUB_IRQ_BANK0       0
#define HUB_IRQ_BANK1       1
#define HUB_IRQ_RT_A        2
#define HUB_IRQ_BANK0_B     4
#define HUB_IRQ_BANK1_B     5

/* A millisecond of the counter, which is the unit these tests arm. */
#define PI_RT_MS            (PI_RT_HZ / 1000)
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
#define IO_SCRATCH_0        0x400150
#define IO_SCRATCH_1        0x400158
#define IO_SCRATCH_MASK     0x0000000f00f11fffULL
#define IO_ICRB_C_BASE      0x4004f0  /* slot 7's C register */
#define IO_ICRB_C_STRIDE    0x20
#define IO_ICRB_C_LAST_OFF  (IO_ICRB_C_BASE + 7 * IO_ICRB_C_STRIDE) /* slot 14 */
#define IO_LINK_STATUS      0x400128
#define MD_INDICATOR_0      0x220050
#define MD_INDICATOR_1      0x220058
#define MD_PERF_CNT0        0x210010
#define MD_PERF_CNT_MASK    0x00000000000fffffULL
#define MD_CONFIG           0x200018
#define MD_CONFIG_MASK      0x003fffffffffffffULL
#define MD_CONFIG_RESET     0x001e7fbc3fffffffULL
#define MD_ERROR_BASE       0x200050
#define MD_ERROR_STRIDE     8
#define MD_ERROR_LAST_OFF   (MD_ERROR_BASE + 7 * MD_ERROR_STRIDE) /* MD_MISC_ERROR_CLR */
#define MD_MEM_DIMM_INIT    0x200090
#define MD_DIR_DIMM_INIT    0x200098
#define NI_STATUS           0x600000
#define NI_SCRATCH_0        0x600100
#define NI_SCRATCH_1        0x600108

/*
 * The junk-bus UART: eight registers, the standard 16550 order, one
 * register per eight-byte stride at the top half of the second
 * microcontroller/UART block. Real firmware's own presence probe touches
 * only the interrupt-enable register (index 1) and the scratch register
 * (index 7, the eighth); the other six are untested here, matching what the
 * probe itself exercises.
 */
#define MD_JUNKUART_BASE     0x2200c0
#define MD_JUNKUART_IER      (MD_JUNKUART_BASE + 0x08)  /* index 1 */
#define MD_JUNKUART_SCRATCH  (MD_JUNKUART_BASE + 0x38)  /* index 7 */
#define MD_JUNKUART_BELOW    (MD_JUNKUART_BASE - 0x08)  /* one stride short */
#define MD_JUNKUART_ABOVE    (MD_JUNKUART_BASE + 0x40)  /* one stride past index 7 */

#define NI_LINK_UP      (1ULL << 29)
#define NI_MORE_NODES   (1ULL << 18)
#define NI_REGION_SIZE  (1ULL << 17)
#define NI_NODE_MASK    (0x1ffULL << 8)
#define NI_IDENTITY     0xffULL          /* revision and chip identity */

/*
 * The port-reset register and the three bits that share it. Only the local
 * bit is this machine's own reset -- the other two send a warm reset and a
 * link reset over a fabric an Origin 200 does not have. The three writers
 * disagree about which companion bit rides along with it: Linux's reboot
 * path and real firmware's own early self-reset both write the warm bit
 * alongside it, OpenBSD writes the link bit instead, and the model has to
 * treat all three as a machine reset.
 */
#define NI_PORT_RESET        0x600008
#define NI_PORT_RESET_LOCAL  (1ULL << 0)
#define NI_PORT_RESET_LINK   (1ULL << 1)
#define NI_PORT_RESET_WARM   (1ULL << 7)

/*
 * The real-time counter's rate and width. A nanosecond divides by the rate
 * exactly, so every count below is an exact figure rather than a tolerance —
 * which is the point: a check that the counter merely *moved* would pass a
 * counter running at any rate at all, including one whose ticks a guest
 * computing a delay from them would get wrong by a factor of a thousand.
 */
#define PI_RT_HZ            1250000ULL
#define PI_RT_NS_PER_SEC    1000000000LL
#define PI_RT_MASK          ((((uint64_t)1) << 52) - 1)

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
 * The crossbar sits at widget 0, and the low half of its identification
 * register is what a guest reads to find out what is there. It is read-only,
 * which is why the widget-0 case below is asserted differently from every
 * other window here.
 */
#define WIDGET_ID_LOW           4
#define BWIN_REACHING_NOTHING   0

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

/*
 * The controller is a PCI device on the I/O bridge's bus rather than mapped
 * directly, so the tests below that use it as a target need the bridge
 * turned on and need to do the two things a guest's own bus scan would do
 * before its window reaches anything: give the device a PCI address by
 * writing its BAR, and aim the window at that same address. The window's
 * crosstalk-side offset is fixed by slot number, which is why IOC3_SCRATCH
 * above still names the right place to find it — only the PCI-side address
 * in between is this test's to choose.
 */
#define BRIDGE_BASE      SMALL_WINDOW(CONSOLE_WIDGET)
#define BRIDGE_REG_DATA  4        /* registers sit at a slot's upper half */
#define BRIDGE_CFG0_BASE 0x020000
#define BRIDGE_CFG0_DEVICE_STRIDE 0x1000
#define BRIDGE_CFG0(device) (BRIDGE_BASE + BRIDGE_CFG0_BASE + \
                             (device) * BRIDGE_CFG0_DEVICE_STRIDE)
#define BRIDGE_DEVICE(n) (BRIDGE_BASE + 0x200 + 8 * (n) + BRIDGE_REG_DATA)
#define DEV_MEMORY_SPACE (1u << 12)

#define PCI_IOC3_DEVICE     2      /* SN0_IOC3_DEVICE, restated for the test */
#define PCI_COMMAND         0x04
#define PCI_COMMAND_MEMORY  0x0002
#define PCI_BAR0            0x10
#define IOC3_PCI_MMIO       0x10000000u

static void sn0_hub_attach_ioc3(QTestState *qts)
{
    qtest_writel(qts, BRIDGE_CFG0(PCI_IOC3_DEVICE) + PCI_BAR0, IOC3_PCI_MMIO);
    qtest_writel(qts, BRIDGE_CFG0(PCI_IOC3_DEVICE) + PCI_COMMAND,
                PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(PCI_IOC3_DEVICE),
                DEV_MEMORY_SPACE | (IOC3_PCI_MMIO >> 20));
}

/*
 * Real firmware polls the boot flash's ready signal before it touches the
 * flash, and the signal is reported at two bit positions that the register
 * table states are the same signal. Both must read ready out of reset: a model
 * reporting only one leaves firmware polling the other waiting forever, and
 * that failure is a boot that stops with nothing logged rather than an error.
 *
 * Asserting both is the point. Either bit alone is a weaker property than the
 * one that has to hold, and a model setting only the bit a one-sided test names
 * would pass it while still hanging the firmware that reads the other.
 *
 * Nothing else in this register is asserted. The remaining fields sample a
 * second board's master/slave sense, a bus-fault line, a jumper identity and a
 * processor-speed strap — signals this machine does not have — and the register
 * table declares no reset value for any of them, so an assertion here would
 * certify a choice rather than a fact.
 */
static void test_the_flash_ready_signal_reads_ready_at_both_bits(
    const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t slot = qtest_readq(qts, HUB_LOCAL + MD_SLOT_ID_USTAT);

    g_assert_cmphex(slot & MD_SLOT_FLASH_READY_LOW, ==,
                    MD_SLOT_FLASH_READY_LOW);
    g_assert_cmphex(slot & MD_SLOT_FLASH_READY_HIGH, ==,
                    MD_SLOT_FLASH_READY_HIGH);

    qtest_quit(qts);
}

/*
 * The counter runs at 1.25 MHz. Stepping the virtual clock a known distance
 * and asserting the exact number of ticks that fall in it tests the rate, not
 * just that something changed.
 */
static void test_the_real_time_counter_runs_at_its_rate(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Zero it, as a guest does at boot, which puts the count and the virtual
     * clock on one origin and makes every figure below absolute rather than a
     * difference. Stepping the clock by exactly one second must then yield
     * exactly one second of ticks.
     */
    qtest_writeq(qts, HUB_LOCAL + PI_RT_COUNT, 0);
    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_COUNT), ==, 0);

    qtest_clock_step(qts, PI_RT_NS_PER_SEC);
    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_COUNT), ==, PI_RT_HZ);

    qtest_quit(qts);
}

/*
 * Writable, and still running afterwards. Both halves in one test on purpose:
 * a counter that accepts a write and then stops reads back correctly at the
 * instant of the write, so asserting the write alone would pass a frozen
 * counter — which is the failure a guest cannot see, because it arms a
 * deadline against a clock that will never reach it and is told that worked.
 */
static void test_the_real_time_counter_is_writable_and_keeps_running(
    const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    const uint64_t start = 1000;

    qtest_writeq(qts, HUB_LOCAL + PI_RT_COUNT, start);
    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_COUNT), ==, start);

    /* A millisecond is 1250 ticks at 1.25 MHz, exactly. */
    qtest_clock_step(qts, PI_RT_NS_PER_SEC / 1000);
    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_COUNT), ==,
                     start + PI_RT_HZ / 1000);

    qtest_quit(qts);
}

/* Fifty-two bits wide: the top twelve of a written value do not come back. */
static void test_the_real_time_counter_is_fifty_two_bits(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_writeq(qts, HUB_LOCAL + PI_RT_COUNT, ~0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_RT_COUNT), ==, PI_RT_MASK);

    qtest_quit(qts);
}

/*
 * The whole path in one test: arm, wait, the bit pends, the line rises, the
 * handler acknowledges, the line drops. The acknowledgement half is not a
 * flourish — a line that latches high instead of following the pending bit
 * gives a guest an interrupt storm the moment it returns from its handler.
 */
static void test_the_comparator_raises_and_releases_the_line(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_irq_intercept_out_named(qts, "/machine/node0/hub", HUB_IRQ_LINES);

    qtest_writeq(qts, HUB_LOCAL + PI_RT_COUNT, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_RT_ENABLE_A, 1);
    qtest_writeq(qts, HUB_LOCAL + PI_RT_COMPARE_A, PI_RT_MS);

    /* A millisecond short of the deadline, nothing has happened. */
    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_PENDING_A), ==, 0);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_RT_A));

    qtest_clock_step(qts, PI_RT_NS_PER_SEC / 1000);
    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_PENDING_A), ==, 1);
    g_assert_true(qtest_get_irq(qts, HUB_IRQ_RT_A));

    qtest_writeq(qts, HUB_LOCAL + PI_RT_PENDING_A, 0);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_RT_A));

    qtest_quit(qts);
}

/*
 * The enable bit gates the line, it does not gate the match. So a comparator
 * that fires with the interrupt off still leaves the bit pending, and enabling
 * afterwards raises the line with nothing further written to pending — which
 * is what says the model recomputes its outputs rather than raising the line
 * at the instant of the match.
 */
static void test_the_comparator_pends_with_its_interrupt_off(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_irq_intercept_out_named(qts, "/machine/node0/hub", HUB_IRQ_LINES);

    qtest_writeq(qts, HUB_LOCAL + PI_RT_COUNT, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_RT_COMPARE_A, PI_RT_MS);
    qtest_clock_step(qts, PI_RT_NS_PER_SEC / 1000);

    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_PENDING_A), ==, 1);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_RT_A));

    qtest_writeq(qts, HUB_LOCAL + PI_RT_ENABLE_A, 1);
    g_assert_true(qtest_get_irq(qts, HUB_IRQ_RT_A));

    qtest_quit(qts);
}

/*
 * A comparator left behind the counter waits a full revolution rather than
 * matching at once. Fifty-two bits at 1.25 MHz is longer than any run, so the
 * observable effect is that a stale value never fires — and the failure this
 * guards against is the opposite one, where it fires immediately and forever.
 */
static void test_a_comparator_behind_the_counter_does_not_fire(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_writeq(qts, HUB_LOCAL + PI_RT_COUNT, PI_RT_MS);
    qtest_writeq(qts, HUB_LOCAL + PI_RT_ENABLE_A, 1);
    qtest_writeq(qts, HUB_LOCAL + PI_RT_COMPARE_A, PI_RT_MS / 2);

    qtest_clock_step(qts, PI_RT_NS_PER_SEC);
    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_PENDING_A), ==, 0);

    qtest_quit(qts);
}

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

static void test_network_link_presence(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 -global sgi-hub.network-link-up=on");
    uint64_t status = qtest_readq(qts, HUB_LOCAL + NI_STATUS);

    g_assert_cmphex(status & NI_LINK_UP, ==, NI_LINK_UP);

    /* Link state is hardware status, not part of the writable fields. */
    qtest_writeq(qts, HUB_LOCAL + NI_STATUS, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_STATUS) & NI_LINK_UP,
                    ==, NI_LINK_UP);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_STATUS) & NI_LINK_UP,
                    ==, NI_LINK_UP);

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

    /*
     * Real firmware writes here as a logic-analyzer debug convention, not to
     * store a value — nothing on real hardware ever reads it back, since the
     * register can only report which processor is asking. The write must be
     * accepted without disturbing that answer.
     */
    qtest_writeq(qts, HUB_LOCAL + PI_PROCESSOR_NUMBER, 0x1111111111111111ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_PROCESSOR_NUMBER), ==, 0);

    qtest_quit(qts);
}

/*
 * The node's one-wire bus master: what it holds at reset, that a wide value
 * survives it, and that a write does not report the slot finished.
 *
 * There is no bus behind this register yet, and that bounds what these
 * assertions can be worth. The datasheet's checklist also asks that the
 * received-data bit reflect what an attached responder drove, and differ
 * between a responder driving one and zero -- NOT ASSERTED HERE, because
 * nothing is attached to differ. A test that checked only completion would
 * pass a model with no bus at all, which is exactly what this is, so saying
 * which half is covered matters more than usual.
 */
#define MD_ONEWIRE          0x2000a8
#define MD_ONEWIRE_RXDATA   (1ULL << 0)
#define MD_ONEWIRE_DONE     (1ULL << 1)
#define MD_ONEWIRE_PHASE_LO (0x7fULL << 20)
#define MD_ONEWIRE_PHASE_HI (0x7fULL << 27)

/*
 * Both phases at 0x31. The one-wire datasheet is the authority on this
 * register and does not carry the constant while asking that a test assert
 * the actual value; this is the value the memory/directory page records, and
 * the gap is reported rather than hidden behind a weaker assertion.
 */
#define MD_ONEWIRE_RESET    ((0x31ULL << 20) | (0x31ULL << 27))

static void test_the_one_wire_master_resets_to_its_timing(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* The whole word, not merely non-zero, and not merely the phase fields. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_ONEWIRE), ==,
                    MD_ONEWIRE_RESET);

    qtest_quit(qts);
}

/*
 * The upper timing phase reaches bit 33, so a written value genuinely sets
 * bit 32. A model that keeps this register in a 32-bit word passes every
 * other assertion here while rewriting that field from 44 to 12 -- a timing
 * parameter wrong by nearly four, with everything below it intact and
 * plausible. This is the assertion that catches it, and it is the reason the
 * width is a build requirement rather than a note.
 */
static void test_the_one_wire_master_keeps_its_wide_field(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t wide = (44ULL << 27) | (0x2cULL << 20) | (0x321ULL << 10) |
                    (0x04ULL << 2);

    /*
     * 44 in the upper phase is the datasheet's own worked example, and it is
     * chosen because it sets bit 32: truncated to a word it reads back as 12.
     */
    g_assert_cmphex(wide & (1ULL << 32), ==, 1ULL << 32);
    qtest_writeq(qts, HUB_LOCAL + MD_ONEWIRE, wide);

    g_assert_cmpuint((qtest_readq(qts, HUB_LOCAL + MD_ONEWIRE) &
                      MD_ONEWIRE_PHASE_HI) >> 27, ==, 44);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_ONEWIRE) &
                    MD_ONEWIRE_PHASE_LO, ==, 0x2cULL << 20);

    qtest_quit(qts);
}

/*
 * Starting a slot must not report it finished. Asserted as the *not yet*
 * case on purpose: a model that sets completion immediately satisfies any
 * test that only checks completion eventually, and reporting completion
 * without a bus is how firmware is handed a fabricated identity part -- every
 * bit answered by whatever constant we chose. The stall this leaves is loud;
 * that fabrication would be silent.
 */
static void test_starting_a_slot_does_not_report_it_finished(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t after;

    qtest_writeq(qts, HUB_LOCAL + MD_ONEWIRE,
                 MD_ONEWIRE_RESET | (0x321ULL << 10));

    after = qtest_readq(qts, HUB_LOCAL + MD_ONEWIRE);
    g_assert_cmphex(after & MD_ONEWIRE_DONE, ==, 0);
    g_assert_cmphex(after & MD_ONEWIRE_RXDATA, ==, 0);

    /*
     * And it stays clear while the slot is still running. This is *not yet*
     * rather than *never*: the virtual clock does not advance on its own in a
     * qtest, so nothing here moves the slot along, which is exactly what makes
     * this the before case.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_ONEWIRE) &
                    MD_ONEWIRE_DONE, ==, 0);

    qtest_quit(qts);
}

/*
 * An empty bus is a defined state with a defined signature, not an absence of
 * behaviour. The line is open-drain with a pull-up: idle is high, a part
 * signals by pulling it low, so with nothing attached the master drives its
 * pulse, releases, samples a line nothing is holding down, and reads one --
 * and completion signals normally, because the master's own timing ran.
 *
 * **This asserts a deliberate stand-in, not the hardware.** A real Origin 200
 * almost certainly has a part on this line; which part is unestablished, and
 * modelling none is the only statement available that is not a fabrication.
 * When a responder lands, the received bit becomes whatever it drove and this
 * test changes with it -- so it is written to fail loudly at that point rather
 * than to keep passing against a bus that has gained a part.
 */
static void test_an_empty_bus_completes_and_reads_one(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t after;

    qtest_writeq(qts, HUB_LOCAL + MD_ONEWIRE,
                 MD_ONEWIRE_RESET | (0x321ULL << 10));

    /*
     * Run the slot out. The duration is this model's choice, not a hardware
     * figure -- the datasheet states the timing encodings have no established
     * meaning in real time -- so step generously rather than to an exact edge.
     */
    qtest_clock_step(qts, 1000 * 1000);

    after = qtest_readq(qts, HUB_LOCAL + MD_ONEWIRE);
    g_assert_cmphex(after & MD_ONEWIRE_DONE, ==, MD_ONEWIRE_DONE);
    g_assert_cmphex(after & MD_ONEWIRE_RXDATA, ==, MD_ONEWIRE_RXDATA);

    /* The programmed timing is untouched by running a slot. */
    g_assert_cmphex(after & MD_ONEWIRE_PHASE_HI, ==, 0x31ULL << 27);

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

static void test_the_error_spool_registers_hold_a_masked_value(
    const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Zeroed at construction, which is the power-on value the datasheet
     * records -- neither register is a reset value the machine's own reset
     * touches.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_SPOOL_A), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_SPOOL_B), ==, 0);

    /*
     * Bits 31:3 only -- the low three bits of an all-ones write do not
     * survive, and neither do bits above 31.
     */
    qtest_writeq(qts, HUB_LOCAL + PI_ERR_SPOOL_A, ~0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_SPOOL_A), ==,
                    PI_ERR_SPOOL_MASK);

    /* A realistic marker, already aligned, round-trips exactly. */
    qtest_writeq(qts, HUB_LOCAL + PI_ERR_SPOOL_B, 0xdeadbee0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_SPOOL_B), ==,
                    0xdeadbee0ULL);

    qtest_quit(qts);
}

static void test_cpu_enable_registers_store_what_is_written(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* The fitted boot processor is enabled; the empty socket is not. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CPU_ENABLE_A), ==, 1);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CPU_ENABLE_B), ==, 0);

    qtest_writeq(qts, HUB_LOCAL + PI_CPU_ENABLE_A, 1);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CPU_ENABLE_A), ==, 1);
    qtest_writeq(qts, HUB_LOCAL + PI_CPU_ENABLE_A, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CPU_ENABLE_A), ==, 0);

    /*
     * Processor B is not fitted on an Origin 200, so clearing its enable bit
     * -- which is exactly what the firmware's halt routine does as its last
     * act -- reaches for a CPU that qemu_get_cpu() returns NULL for. This is
     * the realistic case, not an edge one, and it must not crash the model.
     */
    qtest_writeq(qts, HUB_LOCAL + PI_CPU_ENABLE_B, 1);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CPU_ENABLE_B), ==, 1);
    qtest_writeq(qts, HUB_LOCAL + PI_CPU_ENABLE_B, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CPU_ENABLE_B), ==, 0);

    qtest_quit(qts);
}

static void test_md_indicator_registers(const void *data)
{
    /*
     * THE VALUES BELOW ARE DELIBERATELY MEANINGLESS, AND THAT IS THE POINT.
     * sgi_hub_led_decode() decodes 0x00-0x60 as progress and 0x81-0xb5 as
     * failures, so 0xff and 0xfe decode to nothing and cannot be mistaken
     * for a code any firmware ever emitted.
     *
     * This test used to write 0xb4, which names a real failure -- whether
     * that failure still fires is a live question elsewhere, and it is a
     * question answered by searching logs. Once the codes were shown by
     * default, every run of the suite printed it, so a search for it found a
     * register test rather than a firmware. An artifact containing the
     * string it exists to test for corrupts every search over itself, and a
     * bare code in a log carries no hint of where it came from -- which is
     * exactly what a search returns.
     *
     * A value with no meaning cannot collide however visible the output
     * later becomes, which is why the value changed rather than the
     * visibility. The register is write-only and stores nothing, so what is
     * written is irrelevant to what this test checks; the test below that
     * does need a real code asserts on it in a private file.
     *
     * debug-leds=off stays as well, for the narrower reason that this test
     * covers what the register does rather than what the machine displays,
     * and the suite log is better with no indicator lines in it at all.
     */
    QTestState *qts = qtest_init("-M origin200,debug-leds=off");

    /*
     * Write-only: real firmware's own diagnostics write here and the model
     * has to accept it without faulting, but what the write means is
     * unestablished and nothing stores it. Asserting a read-back would
     * certify readable state the hardware does not have, so this asserts the
     * opposite — the write is accepted, and a read still returns the
     * unimplemented default rather than echoing what was written.
     */
    qtest_writeq(qts, HUB_LOCAL + MD_INDICATOR_0, 0xff);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_INDICATOR_0), ==, 0);

    qtest_writeq(qts, HUB_LOCAL + MD_INDICATOR_1, 0xfe);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_INDICATOR_1), ==, 0);

    qtest_quit(qts);
}

/*
 * The boot-progress indicator is shown without being asked for.
 *
 * WHY THIS ASSERTS THE OUTPUT AND NOT THE MACHINE PROPERTY. Reading back
 * debug-leds=on would prove only that a bool survived being set. What has to
 * hold is that a decoded line actually arrives, so that is what this checks.
 *
 * WHY -D DOES NOT HAND THE TEST ITS ANSWER. -D chooses where the codes go,
 * nothing more: it cannot enable the sgi_hub_led trace event, and that event
 * is the only thing standing between this machine and a silent boot. So a
 * build whose machine has stopped arming the event fails here even with -D
 * present. That was worth establishing rather than assuming -- an earlier
 * draft of this file also raised the log level from the machine, and this
 * test passed with that removed, because the log level is already set on
 * every run. The redundant call went; this test kept its teeth.
 *
 * NOTE WHAT IS ABSENT FROM THE COMMAND LINE: no -d and no -trace. If this
 * test ever needs one to pass, the default has regressed, which is the point.
 * qemu_log_unlock() flushes each line, so the file can be read while the
 * machine is still up.
 */
static void test_md_indicator_shown_by_default(const void *data)
{
    g_autofree char *log = g_strdup_printf("%s/sn0-led-XXXXXX.log", g_get_tmp_dir());
    g_autofree char *found = NULL;
    g_autoptr(GError) err = NULL;
    QTestState *qts;
    int fd = g_mkstemp(log);

    g_assert_cmpint(fd, >=, 0);
    close(fd);

    qts = qtest_initf("-M origin200 -D %s", log);
    qtest_writeq(qts, HUB_LOCAL + MD_INDICATOR_0, 0x01);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log, &found, NULL, &err));
    g_assert_no_error(err);

    /*
     * The decoded text, not merely the code: the decode is what made this
     * signal worth showing by default, and a line reading only the raw byte
     * would satisfy a substring check on "0x01" while having lost it.
     */
    g_assert_nonnull(strstr(found, "sgi_hub_led CPU 0: 0x01 "
                                   "(progress: initializing the CPU)"));

    g_assert_cmpint(g_unlink(log), ==, 0);
}

/*
 * ... and stays silent when told to be. The option is the escape hatch for
 * anyone who wants a quiet stderr, so it has to actually silence the thing.
 */
static void test_md_indicator_suppressed(const void *data)
{
    g_autofree char *log = g_strdup_printf("%s/sn0-led-off-XXXXXX.log", g_get_tmp_dir());
    g_autofree char *found = NULL;
    QTestState *qts;
    int fd = g_mkstemp(log);

    g_assert_cmpint(fd, >=, 0);
    close(fd);

    qts = qtest_initf("-M origin200,debug-leds=off -D %s", log);
    qtest_writeq(qts, HUB_LOCAL + MD_INDICATOR_0, 0x01);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_null(strstr(found, "sgi_hub_led"));

    g_assert_cmpint(g_unlink(log), ==, 0);
}

/*
 * Real firmware's own presence test: write 0x55 to the scratch register,
 * write zero to the interrupt-enable register so the bus is not left
 * floating, and read the scratch register back. Both accesses are 64 bits
 * wide -- the firmware's own instruction encoding, not a narrow access a
 * memory system widened -- so the write and the read-back use the same
 * width real firmware does, which is what lets the read-back distinguish a
 * model whose implementation access width is undeclared (called twice per
 * access) from a correct one.
 */
static void junkuart_presence_probe(QTestState *qts, uint8_t *readback)
{
    qtest_writeq(qts, HUB_LOCAL + MD_JUNKUART_SCRATCH, 0x55);
    qtest_writeq(qts, HUB_LOCAL + MD_JUNKUART_IER, 0x00);
    qtest_memread(qts, HUB_LOCAL + MD_JUNKUART_SCRATCH, readback, 8);
}

/*
 * The default machine has no such part -- a shipped Origin 200 does not
 * either -- and real firmware's console-selection routine treats this port
 * as its highest-priority candidate, stopping at the first one that
 * answers. A model that let the probe pass by accident would silently hide
 * the two real console candidates behind it, so the default machine must
 * fail this exactly the way real hardware without the part does.
 *
 * Asserted as the eight raw bytes of the read-back rather than through a
 * numeric helper, and paired with the fitted case below: a model with the
 * wrong declared endianness answers all zeroes on a fitted port too, the
 * same answer this absent case gives, so this test alone cannot tell a
 * correct model from a byte-swapped one.
 */
static void test_junk_uart_absent_by_default(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t readback[8];
    static const uint8_t absent[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

    junkuart_presence_probe(qts, readback);
    g_assert_cmpmem(readback, sizeof(readback), absent, sizeof(absent));

    qtest_quit(qts);
}

/*
 * Asked for by name, the port answers exactly the way real firmware's probe
 * checks: the byte written sits at the bottom of the 64-bit word, which on
 * this big-endian target is the last of the eight raw bytes. This is the
 * direction that fails if the region were declared little-endian -- the
 * 16550's write path keeps the low byte of what it is handed, and a
 * byte-swapped 0x55 has none, so a byte-swapped model would answer all
 * zeroes here instead.
 */
static void test_junk_uart_fitted_answers_present(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,junk-uart=on");
    uint8_t readback[8];
    static const uint8_t present[8] = { 0, 0, 0, 0, 0, 0, 0, 0x55 };

    junkuart_presence_probe(qts, readback);
    g_assert_cmpmem(readback, sizeof(readback), present, sizeof(present));

    qtest_quit(qts);
}

/*
 * One register-stride short of the block, and one stride past its eighth
 * register, both belong to the surrounding, unimplemented part of this
 * register group rather than to the port. A region drawn one entry too wide
 * would answer correctly everywhere the presence probe above looks while
 * also covering an address that belongs to something else, and neither
 * direction of that probe would catch it.
 */
static void test_junk_uart_range_is_exact(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,junk-uart=on");

    qtest_writeq(qts, HUB_LOCAL + MD_JUNKUART_BELOW, 0x55);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_JUNKUART_BELOW), ==, 0);

    qtest_writeq(qts, HUB_LOCAL + MD_JUNKUART_ABOVE, 0x55);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_JUNKUART_ABOVE), ==, 0);

    qtest_quit(qts);
}

/*
 * Every device that claims a character device claims a different one.
 *
 * **This is the one thing about the bring-up port no other case in this file
 * can see, and the reason is structural rather than an oversight.** Every
 * other case starts a machine with no character devices at all, so a device
 * asking for one gets nothing, two devices asking for the *same* one both get
 * nothing, and the conflict never forms. This suite stayed green throughout a
 * period when fitting the optional port and supplying the backends aborted the
 * machine before it started.
 *
 * So this case supplies them: three null backends, which is what the machine
 * wants with the optional port fitted -- the base I/O controller's port A, the
 * system controller, and the bring-up port. If any two devices name the same
 * index, QEMU refuses at realize and qtest_init never returns a machine, so
 * reaching the assertion at all is most of the result. The assertion then only
 * has to show the port is genuinely there rather than quietly skipped.
 *
 * It has to be a test rather than a rule because the defect exists at no
 * single call site: each device names an index that is correct on its own, and
 * the fault is only the relationship between two of them, in two different
 * files -- which is precisely what reading either one cannot show.
 */
static void test_every_chardev_claim_is_distinct(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,junk-uart=on "
                                 "-serial null -serial null -serial null");
    uint8_t readback[8];
    static const uint8_t present[8] = { 0, 0, 0, 0, 0, 0, 0, 0x55 };

    junkuart_presence_probe(qts, readback);
    g_assert_cmpmem(readback, sizeof(readback), present, sizeof(present));

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

/*
 * The local-reset bit is a machine reset regardless of which companion bit
 * rides with it -- Linux and real firmware's own early self-reset both send
 * 0x81 (warm plus local), OpenBSD sends 0x03 (link plus local) and arms the
 * protection register first, which this model must not require. Both are
 * exercised on the same machine, one after the other, because a model keyed
 * to either value alone would ignore the other guest.
 *
 * The cached-alias size is the sentinel for "the machine actually reset":
 * it is dirtied to a value its own reset never produces, and a plain write
 * that only stored the register would leave it dirty. The error-spool
 * register is the opposite sentinel -- the firmware's own reset marker, and
 * the one register in the whole Hub that a reset must *not* touch, because
 * the firmware writes it immediately before triggering the reset expressly
 * so the restarted machine can read it back.
 */
static void test_a_local_port_reset_resets_the_machine(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_writeq(qts, HUB_LOCAL + PI_CALIAS_SIZE, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_ERR_SPOOL_B, 0xdeadbee0ULL);

    qtest_writeq(qts, HUB_LOCAL + NI_PORT_RESET,
                NI_PORT_RESET_WARM | NI_PORT_RESET_LOCAL);
    qtest_qmp_eventwait(qts, "RESET");

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CALIAS_SIZE), ==,
                    PI_CALIAS_SIZE_RESET);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_SPOOL_B), ==,
                    0xdeadbee0ULL);

    /*
     * OpenBSD's companion bit and its unarmed protection register, on the
     * same machine, after the first reset already fired.
     */
    qtest_writeq(qts, HUB_LOCAL + PI_CALIAS_SIZE, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_ERR_SPOOL_B, 0x600df00ULL);

    qtest_writeq(qts, HUB_LOCAL + NI_PORT_RESET,
                NI_PORT_RESET_LINK | NI_PORT_RESET_LOCAL);
    qtest_qmp_eventwait(qts, "RESET");

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CALIAS_SIZE), ==,
                    PI_CALIAS_SIZE_RESET);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_SPOOL_B), ==,
                    0x600df00ULL);

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
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_hub_attach_ioc3(qts);

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
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_hub_attach_ioc3(qts);

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

/*
 * A big window whose entry names widget 0. Every other window test here uses a
 * non-zero widget, so this case has no coverage — and it is the one OpenBSD's
 * only route to the crossbar rests on. The translation entries reset to zero,
 * zero spells widget 0 with memory space and offset 0, and the window has to be
 * enabled and aimed at widget 0 as a result rather than treated as unused.
 *
 * Reading a zero widget field as "no widget here" is a change someone could
 * make believing it correct, because that is exactly how a guest reads the same
 * field when it hunts for a free entry. It would pass every Linux test and
 * every Linux boot, because Linux reaches the crossbar through the small window
 * and never through this one.
 *
 * The form is weaker than the console window's write-and-read-back above and
 * cannot be otherwise: widget 0's only register is read-only, so there is no
 * value to put in through one window and take out through another. What is
 * asserted instead is that a window explicitly aimed at widget 0 answers the
 * same as the window that reaches widget 0 by reset alone, and that neither is
 * answering with what a window reaching no widget returns.
 */
static void test_a_big_window_may_name_widget_zero(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint32_t named, by_reset;

    qtest_writeq(qts, HUB_LOCAL + IO_BWIN_ENTRY(0),
                 IO_BWIN_WIDGET(0) | IO_BWIN_SLICE(0));

    named = qtest_readl(qts, BIG_WINDOW(0) + WIDGET_ID_LOW);
    by_reset = qtest_readl(qts, BIG_WINDOW(IO_BWIN_ENTRIES - 1) + WIDGET_ID_LOW);

    g_assert_cmphex(named, ==, by_reset);
    g_assert_cmphex(named, !=, BWIN_REACHING_NOTHING);

    qtest_quit(qts);
}

static void test_big_window_naming_an_absent_widget(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_hub_attach_ioc3(qts);

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
    qtest_irq_intercept_out_named(qts, "/machine/node0/hub", HUB_IRQ_LINES);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK1_A, ~0ULL);

    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, INT_MODIFY_SET | 70);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND1), ==,
                    1ULL << (70 - INT_BANK_BITS));
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 0);

    /*
     * And it is the second bank's line that rises. The register check above
     * would pass a model that put the bit in the right bank and drove the
     * wrong line, which is the same wrong-handler symptom by a different
     * route — so assert the two together rather than trusting one for both.
     */
    g_assert_true(qtest_get_irq(qts, HUB_IRQ_BANK1));
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_BANK0));

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
     * This test used to assert only the pending state and say the line itself
     * was left to the boot. It is not: intercepting the Hub's outgoing sysbus
     * lines shows the level directly, so the half that was named as missing is
     * asserted here.
     */
    qtest_irq_intercept_out_named(qts, "/machine/node0/hub", HUB_IRQ_LINES);

    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK0_A, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, INT_MODIFY_SET | 12);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 1ULL << 12);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_BANK0));

    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK0_A, 1ULL << 12);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 1ULL << 12);
    g_assert_true(qtest_get_irq(qts, HUB_IRQ_BANK0));

    /* Clearing the last bit the mask can see drops the line rather than
     * latching it, which is what a level-triggered output means. */
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, 12);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_BANK0));

    qtest_quit(qts);
}

static void test_pending_banks_are_masked_per_processor(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -smp 2");

    qtest_irq_intercept_out_named(qts, "/machine/node0/hub", HUB_IRQ_LINES);

    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK0_A, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK0_B, 1ULL << 12);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, INT_MODIFY_SET | 12);

    g_assert_false(qtest_get_irq(qts, HUB_IRQ_BANK0));
    g_assert_true(qtest_get_irq(qts, HUB_IRQ_BANK0_B));
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_BANK1_B));

    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, 12);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_BANK0_B));

    /*
     * The PROM tests processor B in the opposite order: make an interrupt
     * pending while it is masked, then unmask it.  The mask write must
     * recompute B's output just as the equivalent processor A write does.
     */
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK0_B, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, INT_MODIFY_SET | 12);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_BANK0_B));

    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK0_B, 1ULL << 12);
    g_assert_true(qtest_get_irq(qts, HUB_IRQ_BANK0_B));

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

static void test_a_guest_can_write_the_link_enabled_bit(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t link;

    /*
     * Unset at reset. Real firmware operates this bit itself
     * (hubii_llp_enable(), an ordinary load-modify-store through this
     * register once a real part is identified on the link) rather than
     * finding it already set, so presetting it here would model the
     * firmware's own write for it.
     */
    link = qtest_readq(qts, HUB_LOCAL + IO_LINK_STATUS);
    g_assert_cmphex(link & (1ULL << 10), ==, 0);

    /*
     * Writable on its own: this is the property the removed couldn't-fail
     * assertion never covered, because nothing previously distinguished a
     * model that stored this bit from one that discarded it.
     */
    qtest_writeq(qts, HUB_LOCAL + IO_LINK_STATUS, 1ULL << 10);
    link = qtest_readq(qts, HUB_LOCAL + IO_LINK_STATUS);
    g_assert_cmphex(link & (1ULL << 10), ==, 1ULL << 10);

    /*
     * And it is the only newly-writable thing. A guest writing the enabled
     * bit alongside the state field and the eight-bit-mode bit must not
     * move either of those — firmware may enable the link, but nothing may
     * claim the link came up.
     */
    qtest_writeq(qts, HUB_LOCAL + IO_LINK_STATUS,
                (1ULL << 10) | (1ULL << 11) | (3ULL << 12));
    link = qtest_readq(qts, HUB_LOCAL + IO_LINK_STATUS);
    g_assert_cmphex(link & (1ULL << 10), ==, 1ULL << 10);
    g_assert_cmphex(link & (1ULL << 11), ==, 0);
    g_assert_cmpuint((link >> 12) & 3, >=, 2);

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

    /*
     * Real firmware clears this register during power-on. The model has no bit
     * to clear, so the write is refused — and it must not disturb the answer,
     * which is what a guest reading back after its own clear would check. All
     * ones rather than the zero firmware writes, so that a refusal is what the
     * assertion actually measures.
     */
    qtest_writeq(qts, HUB_LOCAL + IO_WIDGET_STATUS, ~0ULL);
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

static void test_io_scratch_registers(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Real firmware's own boot reads register 0 234 times against 6 writes —
     * the most-touched register this Hub had no model for at all. Plain
     * masked storage: SGI's own register table and mainline Linux's header
     * agree on the mask byte-for-byte, and nothing establishes what firmware
     * keeps there, only where the bits are.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_SCRATCH_0), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_SCRATCH_1), ==, 0);

    qtest_writeq(qts, HUB_LOCAL + IO_SCRATCH_0, ~0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_SCRATCH_0), ==,
                    IO_SCRATCH_MASK);

    qtest_writeq(qts, HUB_LOCAL + IO_SCRATCH_1, ~0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_SCRATCH_1), ==,
                    IO_SCRATCH_MASK);

    /* The two are independent storage, not aliases of one register. */
    qtest_writeq(qts, HUB_LOCAL + IO_SCRATCH_0, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_SCRATCH_0), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_SCRATCH_1), ==,
                    IO_SCRATCH_MASK);

    qtest_quit(qts);
}

static void test_network_scratch_registers(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Firmware publishes this Hub's identity through scratch register 0 and
     * its discovery state through scratch register 1. Both reset to zero and
     * retain all 64 bits independently; treating an unimplemented read as
     * zero makes the local Hub look headless as soon as discovery reads back
     * the identity it just wrote.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_SCRATCH_0), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_SCRATCH_1), ==, 0);

    qtest_writeq(qts, HUB_LOCAL + NI_SCRATCH_0, 0x0123456789abcdefULL);
    qtest_writeq(qts, HUB_LOCAL + NI_SCRATCH_1, 0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_SCRATCH_0), ==,
                    0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_SCRATCH_1), ==,
                    0xfedcba9876543210ULL);

    qtest_quit(qts);
}

static void test_md_perf_cnt0(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Real firmware's own boot reads this specifically 144 times against 23
     * writes — repeated resets and re-samples, a benchmark or timed
     * diagnostic loop. This model counts no real hardware event, so a write
     * is stored and read back rather than incremented; a self-test that
     * resets the counter and checks the reset stuck gets a truthful answer,
     * one that expects the count to advance on its own does not.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_PERF_CNT0), ==, 0);

    qtest_writeq(qts, HUB_LOCAL + MD_PERF_CNT0, ~0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_PERF_CNT0), ==,
                    MD_PERF_CNT_MASK);

    qtest_quit(qts);
}

static void test_md_config(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The reset value is the fact that matters here: it is not zero, and a
     * model that zeroed it would misreport eight empty banks, an enabled
     * error-correction bit the hardware leaves ignored, a cleared
     * directory-format bit that resets set, and a dropped reply-guarantee
     * count, all at once. Real firmware both reads and writes this register
     * (measured, three distinct values each, this session) so a plain
     * masked round trip is asserted alongside the reset value.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_CONFIG), ==,
                    MD_CONFIG_RESET);

    qtest_writeq(qts, HUB_LOCAL + MD_CONFIG, ~0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_CONFIG), ==,
                    MD_CONFIG_MASK);

    qtest_writeq(qts, HUB_LOCAL + MD_CONFIG, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_CONFIG), ==, 0);

    qtest_quit(qts);
}

static void test_md_error_cluster(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t addr;

    /*
     * The four error registers and their four clear aliases, evenly strided
     * eight bytes apart. mtest_basic reads all four aliases before running a
     * memory or directory test and aborts if any valid bit is set, so these
     * eight must read zero unconditionally -- this model never raises any
     * of the errors they would report, so zero is also the honest answer,
     * not merely the one the test needs. Writes are refused (there is
     * nothing to keep) without changing the read.
     */
    for (addr = MD_ERROR_BASE; addr <= MD_ERROR_LAST_OFF;
         addr += MD_ERROR_STRIDE) {
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + addr), ==, 0);
        qtest_writeq(qts, HUB_LOCAL + addr, ~0ULL);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + addr), ==, 0);
    }

    qtest_quit(qts);
}

static void test_md_dimm_init(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Write-only in both dual sources, no readable bits declared at all, so
     * there is no state a qtest can read back to confirm. What this asserts
     * is that both addresses are handled (accepted, not routed to some
     * other case or left to the generic unimplemented path by mistake) and
     * that writing the exact pattern real firmware's per-DIMM sweep uses
     * does not abort the machine.
     */
    qtest_writeq(qts, HUB_LOCAL + MD_MEM_DIMM_INIT, 0xf00000029ULL);
    qtest_writeq(qts, HUB_LOCAL + MD_DIR_DIMM_INIT, 0xf00000029ULL);

    qtest_quit(qts);
}

static void test_pi_rt_local_ctrl(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Bit 16 (internal clock select) resets set; bit 15 and bits 14:8
     * (network-clock observation) are read-only and this model has no
     * network-wide clock to observe, so they are outside the writable mask
     * and always read zero. Real firmware writes 0x10000 (bit 16 alone) and
     * 0x10047 (bit 16, a maximum count of 35 at bits 7:1, and bit 0
     * enabling the network clock) -- both round-trip through the mask.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_RT_LOCAL_CTRL), ==,
                    PI_RT_LOCAL_CTRL_RESET);

    qtest_writeq(qts, HUB_LOCAL + PI_RT_LOCAL_CTRL, 0x10047ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_RT_LOCAL_CTRL), ==,
                    0x10047ULL);

    qtest_writeq(qts, HUB_LOCAL + PI_RT_LOCAL_CTRL, ~0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_RT_LOCAL_CTRL), ==,
                    PI_RT_LOCAL_CTRL_MASK);

    qtest_quit(qts);
}

static void test_io_icrb_c_family(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t addr;

    /*
     * Slots 7 through 14's C register — the only part of the fifteen-slot,
     * four-register Incoming Crosstalk Request Buffer array real firmware's
     * own boot measurably reads (four times each, a diagnostic sweep, not a
     * wait on any one). No slot has ever tracked a real incoming crosstalk
     * transaction, so every field a real C register would hold reads zero;
     * writing one is refused, matching real hardware, where this state comes
     * from a transaction in flight rather than from a guest store.
     *
     * Firmware does write here — it clears the whole array at power-on, zero
     * to every register — so the refusal is about where the state comes from,
     * not about software never storing to it. All ones is the value to write
     * in this assertion rather than the zero firmware writes, because zero
     * into a register that already reads zero would pass whether or not the
     * write was refused.
     */
    for (addr = IO_ICRB_C_BASE; addr <= IO_ICRB_C_LAST_OFF;
         addr += IO_ICRB_C_STRIDE) {
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + addr), ==, 0);
        qtest_writeq(qts, HUB_LOCAL + addr, ~0ULL);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + addr), ==, 0);
    }

    /*
     * An address one stride short of the family, and one stride past it,
     * both read zero too — but that is the unimplemented-register default,
     * not evidence either is part of the modelled family, since both paths
     * answer zero. Nothing observable through this interface distinguishes
     * them; the boundary itself is asserted by sgi_hub_icrb_c_entry()'s own
     * range check, exercised here only by covering the family it accepts.
     */

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/hub/addressing-mode", NULL, test_addressing_mode);
    qtest_add_data_func("sn0/hub/network-link-presence", NULL,
                        test_network_link_presence);
    qtest_add_data_func("sn0/hub/processor-number", NULL,
                        test_processor_number);
    qtest_add_data_func("sn0/hub/reset-values", NULL, test_reset_values);
    qtest_add_data_func("sn0/hub/guest-writes", NULL,
                        test_writes_the_guest_makes);
    qtest_add_data_func("sn0/hub/error-spool-masked-value", NULL,
                        test_the_error_spool_registers_hold_a_masked_value);
    qtest_add_data_func("sn0/hub/cpu-enable-stores-what-is-written", NULL,
                        test_cpu_enable_registers_store_what_is_written);
    qtest_add_data_func("sn0/hub/md-indicator-registers", NULL,
                        test_md_indicator_registers);
    qtest_add_data_func("sn0/hub/md-indicator-shown-by-default", NULL,
                        test_md_indicator_shown_by_default);
    qtest_add_data_func("sn0/hub/md-indicator-suppressed", NULL,
                        test_md_indicator_suppressed);
    qtest_add_data_func("sn0/hub/junk-uart-absent-by-default", NULL,
                        test_junk_uart_absent_by_default);
    qtest_add_data_func("sn0/hub/junk-uart-fitted-answers-present", NULL,
                        test_junk_uart_fitted_answers_present);
    qtest_add_data_func("sn0/hub/junk-uart-range-is-exact", NULL,
                        test_junk_uart_range_is_exact);
    qtest_add_data_func("sn0/hub/chardev-claims-are-distinct", NULL,
                        test_every_chardev_claim_is_distinct);
    qtest_add_data_func("sn0/hub/status-field-access", NULL,
                        test_status_field_access);
    qtest_add_data_func("sn0/hub/local-port-reset-resets-the-machine", NULL,
                        test_a_local_port_reset_resets_the_machine);
    qtest_add_data_func("sn0/hub/both-windows", NULL, test_both_windows);
    qtest_add_data_func("sn0/hub/translation-entries-decode", NULL,
                        test_translation_entries_decode);
    qtest_add_data_func("sn0/hub/translation-entry-width", NULL,
                        test_translation_entry_width);
    qtest_add_data_func("sn0/hub/big-window-follows-its-entry", NULL,
                        test_big_window_follows_its_entry);
    qtest_add_data_func("sn0/hub/big-window-follows-the-slice", NULL,
                        test_big_window_follows_the_slice_too);
    qtest_add_data_func("sn0/hub/big-window-may-name-widget-zero", NULL,
                        test_a_big_window_may_name_widget_zero);
    qtest_add_data_func("sn0/hub/big-window-absent-widget", NULL,
                        test_big_window_naming_an_absent_widget);
    qtest_add_data_func("sn0/hub/identifies-itself", NULL,
                        test_the_hub_identifies_itself_on_the_crossbar);
    qtest_add_data_func("sn0/hub/widget-control-reset", NULL,
                        test_the_widget_control_reset_values);
    qtest_add_data_func("sn0/hub/flash-ready-at-both-bits", NULL,
                        test_the_flash_ready_signal_reads_ready_at_both_bits);
    qtest_add_data_func("sn0/hub/crosstalk-timeouts", NULL,
                        test_the_crosstalk_timeouts);
    qtest_add_data_func("sn0/hub/io-scratch-registers", NULL,
                        test_io_scratch_registers);
    qtest_add_data_func("sn0/hub/network-scratch-registers", NULL,
                        test_network_scratch_registers);
    qtest_add_data_func("sn0/hub/md-perf-cnt0", NULL, test_md_perf_cnt0);
    qtest_add_data_func("sn0/hub/md-config", NULL, test_md_config);
    qtest_add_data_func("sn0/hub/md-error-cluster", NULL, test_md_error_cluster);
    qtest_add_data_func("sn0/hub/md-dimm-init", NULL, test_md_dimm_init);
    qtest_add_data_func("sn0/hub/pi-rt-local-ctrl", NULL, test_pi_rt_local_ctrl);
    qtest_add_data_func("sn0/hub/io-icrb-c-family", NULL,
                        test_io_icrb_c_family);
    qtest_add_data_func("sn0/hub/crosstalk-link-reports-up", NULL,
                        test_the_crosstalk_link_reports_up);
    qtest_add_data_func("sn0/hub/link-state-is-not-writable", NULL,
                        test_a_guest_cannot_write_the_link_state);
    qtest_add_data_func("sn0/hub/link-enabled-bit-is-writable", NULL,
                        test_a_guest_can_write_the_link_enabled_bit);
    qtest_add_data_func("sn0/hub/rt-counter-rate", NULL,
                        test_the_real_time_counter_runs_at_its_rate);
    qtest_add_data_func("sn0/hub/rt-counter-writable", NULL,
                        test_the_real_time_counter_is_writable_and_keeps_running);
    qtest_add_data_func("sn0/hub/rt-counter-width", NULL,
                        test_the_real_time_counter_is_fifty_two_bits);
    qtest_add_data_func("sn0/hub/rt-comparator-raises-line", NULL,
                        test_the_comparator_raises_and_releases_the_line);
    qtest_add_data_func("sn0/hub/rt-comparator-pends-when-off", NULL,
                        test_the_comparator_pends_with_its_interrupt_off);
    qtest_add_data_func("sn0/hub/rt-comparator-behind-waits", NULL,
                        test_a_comparator_behind_the_counter_does_not_fire);
    qtest_add_data_func("sn0/hub/interrupt-modify", NULL,
                        test_interrupt_modify_sets_and_clears);
    qtest_add_data_func("sn0/hub/interrupt-number-selects-bank", NULL,
                        test_interrupt_number_selects_the_bank);
    qtest_add_data_func("sn0/hub/masked-pending-raises-nothing", NULL,
                        test_a_masked_pending_bit_raises_nothing);
    qtest_add_data_func("sn0/hub/pending-banks-masked-per-processor", NULL,
                        test_pending_banks_are_masked_per_processor);
    qtest_add_data_func("sn0/hub/one-wire-resets-to-its-timing", NULL,
                        test_the_one_wire_master_resets_to_its_timing);
    qtest_add_data_func("sn0/hub/one-wire-keeps-its-wide-field", NULL,
                        test_the_one_wire_master_keeps_its_wide_field);
    qtest_add_data_func("sn0/hub/one-wire-start-is-not-finish", NULL,
                        test_starting_a_slot_does_not_report_it_finished);
    qtest_add_data_func("sn0/hub/one-wire-empty-bus-reads-one", NULL,
                        test_an_empty_bus_completes_and_reads_one);
    qtest_add_data_func("sn0/hub/pending-banks-reject-writes", NULL,
                        test_the_pending_banks_reject_writes);

    return g_test_run();
}
