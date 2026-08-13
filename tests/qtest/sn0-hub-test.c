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
#define PI_RT_COUNT         0x030100
#define PI_RT_COMPARE_A     0x000108
#define PI_RT_PENDING_A     0x000120
#define PI_RT_ENABLE_A      0x000140
#define PI_RT_LOCAL_CTRL       0x000160
#define PI_RT_LOCAL_CTRL_MASK  0x100ffULL
#define PI_RT_LOCAL_CTRL_RESET 0x10000ULL

/*
 * The Hub's outgoing lines, by the order it registers them: the two pending
 * banks, then the counter comparator for each processor. Intercepting them is
 * what lets a test see the line itself rather than only the pending bit behind
 * it — the distinction the pending-bank tests have to leave to the boot.
 */
#define HUB_IRQ_LINES       "sysbus-irq"
#define HUB_IRQ_BANK0       0
#define HUB_IRQ_BANK1       1
#define HUB_IRQ_RT_A        2

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
#define MD_I2C_DATA         0x220000
#define MD_I2C_CTRL         0x220008
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
#define I2C_STATUS_IDLE     0x81  /* pending-interrupt-not | bus-not-busy */
#define NI_STATUS           0x600000

#define NI_MORE_NODES   (1ULL << 18)
#define NI_REGION_SIZE  (1ULL << 17)
#define NI_NODE_MASK    (0x1ffULL << 8)
#define NI_IDENTITY     0xffULL          /* revision and chip identity */

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

    qtest_irq_intercept_out_named(qts, "/machine/hub", HUB_IRQ_LINES);

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

    qtest_irq_intercept_out_named(qts, "/machine/hub", HUB_IRQ_LINES);

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

static void test_md_indicator_registers(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Write-only: real firmware's own diagnostics write here and the model
     * has to accept it without faulting, but what the write means is
     * unestablished and nothing stores it. Asserting a read-back would
     * certify readable state the hardware does not have, so this asserts the
     * opposite — the write is accepted, and a read still returns the
     * unimplemented default rather than echoing what was written.
     */
    qtest_writeq(qts, HUB_LOCAL + MD_INDICATOR_0, 0xb4);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_INDICATOR_0), ==, 0);

    qtest_writeq(qts, HUB_LOCAL + MD_INDICATOR_1, 0x1);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_INDICATOR_1), ==, 0);

    qtest_quit(qts);
}

static void test_i2c_block_answers_idle(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The controller chip is real hardware on every Origin 200; what is
     * absent is everything else on the shared bus this model does not
     * simulate. So the status half reads chip-ready-and-bus-free rather
     * than the generic unimplemented default (which a driver's own wait
     * loop would otherwise never see clear), and it does not change no
     * matter what a guest writes to the same register — this is a fixed
     * idle answer, not stored state.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_I2C_CTRL), ==,
                    I2C_STATUS_IDLE);
    qtest_writeq(qts, HUB_LOCAL + MD_I2C_CTRL, 0x44);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_I2C_CTRL), ==,
                    I2C_STATUS_IDLE);

    /*
     * The data register never changes on its own, which is what the real
     * driver's bus-presence probe actually reads it for: consecutive reads
     * differing is how it tells a live device from an empty bus.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_I2C_DATA), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_I2C_DATA), ==, 0);
    qtest_writeq(qts, HUB_LOCAL + MD_I2C_DATA, 0xe0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_I2C_DATA), ==, 0);

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
    qtest_irq_intercept_out_named(qts, "/machine/hub", HUB_IRQ_LINES);
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
    qtest_irq_intercept_out_named(qts, "/machine/hub", HUB_IRQ_LINES);

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
    qtest_add_data_func("sn0/hub/processor-number", NULL,
                        test_processor_number);
    qtest_add_data_func("sn0/hub/reset-values", NULL, test_reset_values);
    qtest_add_data_func("sn0/hub/guest-writes", NULL,
                        test_writes_the_guest_makes);
    qtest_add_data_func("sn0/hub/md-indicator-registers", NULL,
                        test_md_indicator_registers);
    qtest_add_data_func("sn0/hub/i2c-block-answers-idle", NULL,
                        test_i2c_block_answers_idle);
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
    qtest_add_data_func("sn0/hub/pending-banks-reject-writes", NULL,
                        test_the_pending_banks_reject_writes);

    return g_test_run();
}
