/*
 * SGI SN0 Hub ASIC register-contract tests.
 *
 * A qtest cannot see a bus error, so "this window must not reach the device"
 * is asserted as "this must not return what the device holds", with a
 * distinctive value put in the device first.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

/* Crosstalk space, and the two halves of the crossbar's widget 1 window. */
#define SPACE_XTALK     0x20000000000ULL
#define HUB_LOCAL       (SPACE_XTALK + 0x01000000)
#define HUB_BY_NODE     (SPACE_XTALK + 0x01800000)

/* The boot flash's ready signal, at both positions it appears at. */
#define MD_SLOT_ID_USTAT            0x220048
#define MD_SLOT_FLASH_READY_LOW     (1ULL << 0)
#define MD_SLOT_XJMP_ID             (1ULL << 2)
#define MD_SLOT_FLASH_READY_HIGH    (1ULL << 4)
#define MD_SLOT_MASTER_MODULE       (1ULL << 7)

#define PI_REGION_PRESENT   0x000018
#define PI_PROCESSOR_NUMBER 0x000020
#define PI_CALIAS_SIZE      0x000028
#define PI_CALIAS_SIZE_RESET 15
#define PI_CPU_ENABLE_A     0x000050
#define PI_CPU_ENABLE_B     0x000058

/* Error spool addresses, bits 31:3; firmware's reset marker lives in B's. */
#define PI_ERR_SPOOL_A       0x000418
#define PI_ERR_SPOOL_B       0x000420
#define PI_ERR_SPOOL_MASK    0xfffffff8ULL
#define PI_ERR_PENDING       0x000400
#define PI_ERR_MASK_A        0x000408
#define PI_ERR_MASK_B        0x000410
#define PI_ERR_SPOOL_SIZE    0x000428
#define PI_ERR_STATUS0_A     0x000430
#define PI_ERR_STATUS0_CLR_A 0x000438
#define PI_ERR_STATUS1_A     0x000440
#define PI_ERR_STATUS1_CLR_A 0x000448
#define PI_ERR_STATUS0_B     0x000450
#define PI_ERR_STATUS0_CLR_B 0x000458
#define PI_ERR_STATUS1_B     0x000460
#define PI_ERR_STATUS1_CLR_B 0x000468
#define PI_ERR_MASK          0x01ffffffULL
#define PI_ERR_SPOOL_SIZE_MASK 0xfULL
#define PI_TIMEDOUT_BUF_A    0x000480
#define PI_TIMEDOUT_BUF_B    0x000488
#define PI_RT_COUNT         0x030100
#define PI_RT_COMPARE_A     0x000108
#define PI_RT_PENDING_A     0x000120
#define PI_RT_ENABLE_A      0x000140
#define PI_RT_COMPARE_B     0x000110
#define PI_RT_PENDING_B     0x000128
#define PI_RT_ENABLE_B      0x000148
#define PI_RT_LOCAL_CTRL       0x000160
#define PI_RT_LOCAL_CTRL_MASK  0x100ffULL
#define PI_RT_LOCAL_CTRL_RESET 0x10000ULL

/*
 * The Hub's outgoing lines: one named GPIO list per kind, each indexed by
 * processor slice. qtest intercepts one list per session, so a test that
 * watches two kinds of line runs one session per kind.
 */
#define HUB_IRQ_BANK0_LINES "bank0"
#define HUB_IRQ_BANK1_LINES "bank1"
#define HUB_IRQ_RT_LINES    "rt"
#define HUB_IRQ_SLICE_A     0
#define HUB_IRQ_SLICE_B     1

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
#define IO_ERROR_CLEAR      0x4001f8
#define IO_PIO_DEALLOC      0x400388
#define IO_CRB_DEALLOC      0x400390
#define IO_IOQ_FIFO_DEPTH   0x400398
#define IO_CRB_MGMT         0x4003a8
#define IO_CRB_CONTROL      0x4003b0

/* The engines' six registers, at the base of each 64 KB block. */
#define BTE_BASE(e)         (0x410000 + (e) * 0x10000)
#define BTE_LENGTH(e)       (BTE_BASE(e) + 0x00)
#define BTE_SOURCE(e)       (BTE_BASE(e) + 0x08)
#define BTE_DEST(e)         (BTE_BASE(e) + 0x10)
#define BTE_CONTROL(e)      (BTE_BASE(e) + 0x18)
#define BTE_NOTIFY(e)       (BTE_BASE(e) + 0x20)
#define BTE_INTERRUPT(e)    (BTE_BASE(e) + 0x28)

#define BTE_BUSY            (1ULL << 20)
#define BTE_ERROR           (1ULL << 16)
#define BTE_ADDR_MASK       0x000000ffffffff80ULL  /* bits 39:7 */
#define BTE_LINE            128
#define BTE_CTL_POISON      (1ULL << 8)
#define BTE_CTL_NOTIFY      (1ULL << 4)
#define BTE_CTL_ZFILL       (1ULL << 0)
#define BTE_INT_LEVEL(l)    ((uint64_t)(l) << 16)

/* The cached-window upper bits firmware's own stores carry. */
#define BTE_CACHED_UPPER    0xa800000000000000ULL

/* Scratch RAM for the transfer tests, and one address beyond every bank. */
#define BTE_TEST_SRC        0x00700000ULL
#define BTE_TEST_DST        0x00740000ULL
#define BTE_TEST_NOTIFY     0x00780000ULL
#define BTE_TEST_HOLE       0x10000000ULL
#define MD_INDICATOR_0      0x220050
#define MD_INDICATOR_1      0x220058
#define MD_SN00_INDICATOR_0 0x220080
#define MD_SN00_INDICATOR_1 0x2200a0
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
#define NI_PROTECTION       0x600010
#define NI_SCRATCH_0        0x600100
#define NI_SCRATCH_1        0x600108
#define NI_DIAG_PARMS       0x600110
#define NI_DIAG_PARMS_MASK  0x7ffffULL
#define NI_VECTOR_PARMS     0x600200
#define NI_VECTOR           0x600208
#define NI_VECTOR_DATA      0x600210
#define NI_VECTOR_STATUS    0x600300
#define NI_VECTOR_READ_DATA 0x600310
#define NI_IO_PROTECT       0x600400
#define NI_IO_PROT_OVRRD    0x600408
#define NI_PROT_PASSWORD    0x53474972756c6573ULL   /* "SGIrules" */
#define NI_PORT_PARMS       0x608000
#define NI_PORT_ERROR       0x608008
#define NI_PORT_ERROR_CLEAR 0x608088
#define NI_PORT_PARMS_RESET ((6ULL << 10) | 0x3f0)
#define NI_VECTOR_CLEAR     0x600380
#define NI_META_TABLE       0x638000
#define NI_META_TABLE_LAST  (NI_META_TABLE + 31 * 8)
#define NI_LOCAL_TABLE      0x638100

#define NODE_STRIDE         0x100000000ULL
#define NVP_PIOID_SHIFT     40
#define NVP_WRITEID_SHIFT   32
#define NVS_VALID           (1ULL << 63)
#define NVS_TARGET_SHIFT    51
#define NVS_TARGET_MASK     (0x3ffULL << NVS_TARGET_SHIFT)
#define NVS_TYPE_MASK       0x7ULL
#define NVS_ADDRESS_ERROR   4
#define PIOTYPE_READ        0
#define PIOTYPE_WRITE       1
#define PIOTYPE_EXCHANGE    3

/* The junk-bus UART: 16550 order at an eight-byte stride. */
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

/* Port reset; only the local bit is this machine's own reset. */
#define NI_PORT_RESET        0x600008
#define NI_PORT_RESET_LOCAL  (1ULL << 0)
#define NI_PORT_RESET_LINK   (1ULL << 1)
#define NI_PORT_RESET_WARM   (1ULL << 7)

/* The counter's rate and width; a nanosecond divides by the rate exactly. */
#define PI_RT_HZ            1250000ULL
#define PI_RT_NS_PER_SEC    1000000000LL
#define PI_RT_MASK          ((((uint64_t)1) << 52) - 1)

/* Interrupt modify: the set bit, and the bank split at number 64. */
#define INT_MODIFY_SET      0x100
#define INT_BANK_BITS       64

/* The big-window translation table, entry n at 8n from its base. */
#define IO_BWIN_TABLE   0x400160
#define IO_BWIN_STRIDE  8
#define IO_BWIN_ENTRIES 7
#define IO_BWIN_ENTRY(n) (IO_BWIN_TABLE + (n) * IO_BWIN_STRIDE)
#define IO_BWIN_WIDGET(w) ((uint64_t)(w) << 8)
#define IO_BWIN_SLICE(x)  ((uint64_t)(x))
/* The space select; the two spaces must not both answer. */
#define IO_BWIN_OTHER_SPACE  (1ULL << 12)

/* The widget number SGI's firmware names to turn a window off. */
#define IO_BWIN_INVALID_WIDGET 3

/* The crossbar at widget 0: its read-only identification register. */
#define WIDGET_ID_LOW           4
#define BWIN_REACHING_NOTHING   0

/* The big windows, and the IOC3's serial scratch register as a target. */
#define BIG_WINDOW_BASE 0x20000000
#define BIG_WINDOW_SIZE 0x20000000ULL
#define BIG_WINDOW(n)   (SPACE_XTALK + BIG_WINDOW_BASE + (n) * BIG_WINDOW_SIZE)

#define SMALL_WINDOW(w) (SPACE_XTALK + (uint64_t)(w) * 0x1000000)
#define CONSOLE_WIDGET  8
#define IOC3_SCRATCH    (0x600000 + 0x020178 + 7)

/* The IOC3 is PCI behind the Bridge: give it a BAR and aim the window at it. */
#define BRIDGE_BASE      SMALL_WINDOW(CONSOLE_WIDGET)
#define BRIDGE_REG_DATA  4        /* registers sit at a slot's upper half */
#define BRIDGE_CFG0_BASE 0x020000
#define BRIDGE_CFG0_DEVICE_STRIDE 0x1000
#define BRIDGE_CFG0(device) (BRIDGE_BASE + BRIDGE_CFG0_BASE + \
                             (device) * BRIDGE_CFG0_DEVICE_STRIDE)
#define BRIDGE_DEVICE(n) (BRIDGE_BASE + 0x200 + 8 * (n) + BRIDGE_REG_DATA)
#define DEV_MEMORY_SPACE (1u << 12)

#define PCI_IOC3_DEVICE     2      /* IP29 slot, restated for the test */
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
 * A stock-PROM machine boots with every Hub programmed zero, so a
 * node-one-qualified address names no Hub -- the qtest backdoor rides
 * processor 0's view like any other access. Program node 1's real
 * number the way the PROM's global master does, a vector write over
 * the CrayLink, so the test can reach node 1 by address.
 */
static void assign_node1_number(QTestState *qts)
{
    qtest_writeq(qts, HUB_LOCAL + NI_VECTOR, 0);
    qtest_writeq(qts, HUB_LOCAL + NI_VECTOR_DATA,
                 NI_REGION_SIZE | (1ULL << 8));
    qtest_writeq(qts, HUB_LOCAL + NI_VECTOR_PARMS, NI_STATUS | PIOTYPE_WRITE);
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + NODE_STRIDE + NI_STATUS) &
                    NI_NODE_MASK, ==, 1ULL << 8);
}

/* Both ready bits: firmware polls either; a one-sided test would hang it. */
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

static void assert_dual_node_master_slave_sense(QTestState *qts)
{
    uint64_t node0 = qtest_readq(qts, HUB_BY_NODE + MD_SLOT_ID_USTAT);
    uint64_t node1 = qtest_readq(qts, HUB_BY_NODE + NODE_STRIDE +
                                      MD_SLOT_ID_USTAT);
    uint64_t ready = MD_SLOT_FLASH_READY_LOW | MD_SLOT_FLASH_READY_HIGH;

    g_assert_cmphex(node0 & ready, ==, ready);
    g_assert_cmphex(node1 & ready, ==, ready);
    g_assert_cmphex(node0 & MD_SLOT_XJMP_ID, ==, MD_SLOT_XJMP_ID);
    g_assert_cmphex(node1 & MD_SLOT_XJMP_ID, ==, MD_SLOT_XJMP_ID);
    g_assert_cmphex(node0 & MD_SLOT_MASTER_MODULE, ==,
                    MD_SLOT_MASTER_MODULE);
    g_assert_cmphex(node1 & MD_SLOT_MASTER_MODULE, ==, 0);
}

/* Physical straps on both nodes, node 0 master; reset must not change them. */
static void test_dual_node_master_slave_sense(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200,synthesized-firmware=off -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1");

    assign_node1_number(qts);
    assert_dual_node_master_slave_sense(qts);
    qtest_system_reset(qts);
    /* Reset returns both Hubs to zero; renumber to reach node 1 again. */
    assign_node1_number(qts);
    assert_dual_node_master_slave_sense(qts);

    qtest_quit(qts);
}

/* Exact ticks over a known clock step test the rate, not just movement. */
static void test_the_real_time_counter_runs_at_its_rate(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* Zeroed as a guest does at boot, putting count and clock on one origin. */
    qtest_writeq(qts, HUB_LOCAL + PI_RT_COUNT, 0);
    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_COUNT), ==, 0);

    qtest_clock_step(qts, PI_RT_NS_PER_SEC);
    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_COUNT), ==, PI_RT_HZ);

    qtest_quit(qts);
}

/*
 * A comparator at or behind the count matches at once. Under wrap semantics
 * the pend stayed clear for a 114-year revolution, silently.
 */
static void test_a_behind_comparator_matches_at_once(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_writeq(qts, HUB_LOCAL + PI_RT_COUNT, 0);
    qtest_clock_step(qts, PI_RT_NS_PER_SEC / 1000);

    /* One millisecond in, name a deadline the counter passed long ago. */
    qtest_writeq(qts, HUB_LOCAL + PI_RT_PENDING_A, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_RT_COMPARE_A, 1);
    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_PENDING_A), ==, 1);

    qtest_quit(qts);
}

/*
 * The match is applied inside the write, not queued: Linux's guard rewrites
 * the comparator within a few instructions, which cancels a queued match.
 */
static void test_a_behind_match_survives_an_immediate_rewrite(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_writeq(qts, HUB_LOCAL + PI_RT_COUNT, 0);
    qtest_clock_step(qts, PI_RT_NS_PER_SEC / 1000);

    qtest_writeq(qts, HUB_LOCAL + PI_RT_PENDING_A, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_RT_COMPARE_A, 1);
    /* The guard's retry, arriving before any queued callback could run. */
    qtest_writeq(qts, HUB_LOCAL + PI_RT_COMPARE_A, PI_RT_HZ);

    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_PENDING_A), ==, 1);

    qtest_quit(qts);
}

/* A maximum-interval arm is a tick behind in 52 bits, so it matches at once. */
static void test_a_maximum_interval_arm_matches_at_once(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t count;

    qtest_writeq(qts, HUB_LOCAL + PI_RT_COUNT, 0);
    qtest_clock_step(qts, PI_RT_NS_PER_SEC / 1000);
    count = qtest_readq(qts, HUB_LOCAL + PI_RT_COUNT);

    qtest_writeq(qts, HUB_LOCAL + PI_RT_PENDING_A, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_RT_COMPARE_A,
                 (count + PI_RT_MASK) & PI_RT_MASK);
    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_PENDING_A), ==, 1);

    qtest_quit(qts);
}

/* Moving the counter past a reset-zero comparator pends nothing. */
static void test_moving_the_counter_past_a_comparator_pends_nothing(
    const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_writeq(qts, HUB_LOCAL + PI_RT_PENDING_A, 0);
    /* Comparators still at their reset value; step the origin past them. */
    qtest_writeq(qts, HUB_LOCAL + PI_RT_COUNT, PI_RT_HZ);

    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_PENDING_A), ==, 0);

    qtest_quit(qts);
}

/*
 * Writable, and still running afterwards: asserting the write alone passes a
 * frozen counter, the failure a guest cannot see.
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
 * Arm, pend, line up, acknowledge, line down. A line that latched high
 * instead of following the pending bit storms the guest on return.
 */
static void test_the_comparator_raises_and_releases_the_line(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_irq_intercept_out_named(qts, "/machine/node0/hub",
                                  HUB_IRQ_RT_LINES);

    qtest_writeq(qts, HUB_LOCAL + PI_RT_COUNT, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_RT_ENABLE_A, 1);
    qtest_writeq(qts, HUB_LOCAL + PI_RT_COMPARE_A, PI_RT_MS);

    /* A millisecond short of the deadline, nothing has happened. */
    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_PENDING_A), ==, 0);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_SLICE_A));

    qtest_clock_step(qts, PI_RT_NS_PER_SEC / 1000);
    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_PENDING_A), ==, 1);
    g_assert_true(qtest_get_irq(qts, HUB_IRQ_SLICE_A));

    qtest_writeq(qts, HUB_LOCAL + PI_RT_PENDING_A, 0);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_SLICE_A));

    qtest_quit(qts);
}

/* The second processor's comparator drives its own slice's line. */
static void test_the_second_processors_comparator_raises_its_own_line(
    const void *data)
{
    /* Two processors, so slice B is populated and its comparator arms. */
    QTestState *qts = qtest_init("-M origin200 -smp 2");

    qtest_irq_intercept_out_named(qts, "/machine/node0/hub",
                                  HUB_IRQ_RT_LINES);

    qtest_writeq(qts, HUB_LOCAL + PI_RT_COUNT, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_RT_ENABLE_B, 1);
    qtest_writeq(qts, HUB_LOCAL + PI_RT_COMPARE_B, PI_RT_MS);

    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_PENDING_B), ==, 0);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_SLICE_B));

    qtest_clock_step(qts, PI_RT_NS_PER_SEC / 1000);
    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_PENDING_B), ==, 1);
    g_assert_true(qtest_get_irq(qts, HUB_IRQ_SLICE_B));

    /* The first processor's line must not follow the second's. */
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_SLICE_A));

    qtest_writeq(qts, HUB_LOCAL + PI_RT_PENDING_B, 0);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_SLICE_B));

    qtest_quit(qts);
}

/*
 * Enable gates the line, not the match: enabling afterwards raises the line
 * with nothing written to pending, so the outputs are recomputed.
 */
static void test_the_comparator_pends_with_its_interrupt_off(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_irq_intercept_out_named(qts, "/machine/node0/hub",
                                  HUB_IRQ_RT_LINES);

    qtest_writeq(qts, HUB_LOCAL + PI_RT_COUNT, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_RT_COMPARE_A, PI_RT_MS);
    qtest_clock_step(qts, PI_RT_NS_PER_SEC / 1000);

    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_PENDING_A), ==, 1);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_SLICE_A));

    qtest_writeq(qts, HUB_LOCAL + PI_RT_ENABLE_A, 1);
    g_assert_true(qtest_get_irq(qts, HUB_IRQ_SLICE_A));

    qtest_quit(qts);
}

/*
 * A behind comparator matches once: nothing re-arms, so a model firing
 * immediately and repeatedly passes the matches-at-once test and fails this.
 */
static void test_a_behind_comparator_matches_once_not_repeatedly(
    const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_writeq(qts, HUB_LOCAL + PI_RT_COUNT, PI_RT_MS);
    qtest_writeq(qts, HUB_LOCAL + PI_RT_ENABLE_A, 1);
    qtest_writeq(qts, HUB_LOCAL + PI_RT_COMPARE_A, PI_RT_MS / 2);

    /* The match is immediate. */
    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_PENDING_A), ==, 1);

    /* Acknowledge, then let a full second of counter run past it. */
    qtest_writeq(qts, HUB_LOCAL + PI_RT_PENDING_A, 0);
    qtest_clock_step(qts, PI_RT_NS_PER_SEC);
    g_assert_cmpuint(qtest_readq(qts, HUB_LOCAL + PI_RT_PENDING_A), ==, 0);

    qtest_quit(qts);
}

static void test_addressing_mode(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t status = qtest_readq(qts, HUB_LOCAL + NI_STATUS);

    /* Zero in the more-nodes bit is M mode. */
    g_assert_cmphex(status & NI_MORE_NODES, ==, 0);

    /* Node zero; Linux takes its node number from here. */
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

    /* The first processor; everything per-processor is selected from this. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_PROCESSOR_NUMBER), ==, 0);

    /* Firmware writes here as a debug convention; the answer must not move. */
    qtest_writeq(qts, HUB_LOCAL + PI_PROCESSOR_NUMBER, 0x1111111111111111ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_PROCESSOR_NUMBER), ==, 0);

    qtest_quit(qts);
}

/* The one-wire master; no bus is attached, so no responder is asserted. */
#define MD_ONEWIRE          0x2000a8
#define MD_ONEWIRE_RXDATA   (1ULL << 0)
#define MD_ONEWIRE_DONE     (1ULL << 1)
#define MD_ONEWIRE_PHASE_LO (0x7fULL << 20)
#define MD_ONEWIRE_PHASE_HI (0x7fULL << 27)

/* Both phases at 0x31, the memory/directory datasheet's value. */
#define MD_ONEWIRE_RESET    ((0x31ULL << 20) | (0x31ULL << 27))

/* A slot armed before reset must not complete after it. */
static void test_a_reset_cancels_a_running_slot(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_writeq(qts, HUB_LOCAL + MD_ONEWIRE, MD_ONEWIRE_RESET);
    qtest_system_reset(qts);

    /* Step past the slot deadline; nothing may report done. */
    qtest_clock_step(qts, 10000);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_ONEWIRE), ==,
                    MD_ONEWIRE_RESET);

    qtest_quit(qts);
}

static void test_the_one_wire_master_resets_to_its_timing(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* The whole word, not merely non-zero, and not merely the phase fields. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_ONEWIRE), ==,
                    MD_ONEWIRE_RESET);

    qtest_quit(qts);
}

/*
 * The upper phase reaches bit 33: a model keeping this register in a 32-bit
 * word passes every other assertion here while reading 44 back as 12.
 */
static void test_the_one_wire_master_keeps_its_wide_field(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t wide = (44ULL << 27) | (0x2cULL << 20) | (0x321ULL << 10) |
                    (0x04ULL << 2);

    /* 44 sets bit 32; truncated to a word it reads back as 12. */
    g_assert_cmphex(wide & (1ULL << 32), ==, 1ULL << 32);
    qtest_writeq(qts, HUB_LOCAL + MD_ONEWIRE, wide);

    g_assert_cmpuint((qtest_readq(qts, HUB_LOCAL + MD_ONEWIRE) &
                      MD_ONEWIRE_PHASE_HI) >> 27, ==, 44);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_ONEWIRE) &
                    MD_ONEWIRE_PHASE_LO, ==, 0x2cULL << 20);

    qtest_quit(qts);
}

/*
 * Starting a slot must not report it finished: instant completion passes any
 * eventually-complete check and hands firmware a fabricated identity part.
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

    /* Still clear: the virtual clock does not advance on its own in a qtest. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_ONEWIRE) &
                    MD_ONEWIRE_DONE, ==, 0);

    qtest_quit(qts);
}

/*
 * The bus carries the PIMM manufacturing-record part (the hub one-wire
 * datasheet's settled responder contract): a DS2505-compatible responder
 * whose ROM identity the headless bring-up reads as the remote NIC. The
 * pulse thresholds and the synthetic serial mirror the model's choices;
 * the serial expression is the machine's per-node wiring.
 */
#define ONEWIRE_PULSE_RESET   0x321  /* long low: a bus reset */
#define ONEWIRE_PULSE_SHORT   8      /* a read slot, or writing a one */
#define ONEWIRE_PULSE_ZERO    60     /* writing a zero */
#define ONEWIRE_CMD_SEARCH    0xf0
#define ONEWIRE_FAMILY_DS2505 0x0b
#define HUB_ONEWIRE_SERIAL(nasid) (0x0c0b0a090807ULL + (nasid))

/* The Dallas CRC-8 over the low bytes, as the public DS2505 sheet has it. */
static uint8_t onewire_crc8(uint64_t data, unsigned int bytes)
{
    uint8_t crc = 0;
    unsigned int i, bit;

    for (i = 0; i < bytes; i++) {
        crc ^= data & 0xff;
        data >>= 8;
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8c : crc >> 1;
        }
    }
    return crc;
}

static uint64_t onewire_expected_identity(uint64_t serial)
{
    uint64_t identity = ONEWIRE_FAMILY_DS2505 | (serial << 8);

    return identity | ((uint64_t)onewire_crc8(identity, 7) << 56);
}

/* One bit slot: program the timing, run the slot, return the sampled bit. */
static uint64_t onewire_slot(QTestState *qts, uint64_t hub,
                             unsigned int pulse)
{
    uint64_t after;

    qtest_writeq(qts, hub + MD_ONEWIRE,
                 MD_ONEWIRE_RESET | ((uint64_t)pulse << 10) |
                 (0x04ULL << 2));
    qtest_clock_step(qts, 2000);

    after = qtest_readq(qts, hub + MD_ONEWIRE);
    g_assert_cmphex(after & MD_ONEWIRE_DONE, ==, MD_ONEWIRE_DONE);
    return after & MD_ONEWIRE_RXDATA;
}

static void onewire_write_bit(QTestState *qts, uint64_t hub, unsigned int bit)
{
    onewire_slot(qts, hub, bit ? ONEWIRE_PULSE_SHORT : ONEWIRE_PULSE_ZERO);
}

/*
 * The family's search algorithm, as the firmware runs it: each round reads
 * a ROM bit, reads its complement, then writes the chosen direction back.
 */
static uint64_t onewire_search_identity(QTestState *qts, uint64_t hub)
{
    uint64_t identity = 0;
    unsigned int i;

    /* A reset slot: the part pulls the line down, the presence pulse. */
    g_assert_cmphex(onewire_slot(qts, hub, ONEWIRE_PULSE_RESET), ==, 0);

    for (i = 0; i < 8; i++) {
        onewire_write_bit(qts, hub, (ONEWIRE_CMD_SEARCH >> i) & 1);
    }

    for (i = 0; i < 64; i++) {
        uint64_t bit = onewire_slot(qts, hub, ONEWIRE_PULSE_SHORT);
        uint64_t complement = onewire_slot(qts, hub, ONEWIRE_PULSE_SHORT);

        /* One part: the complement is always the bit's inverse. */
        g_assert_cmphex(complement, ==, bit ^ 1);
        onewire_write_bit(qts, hub, bit);
        identity |= bit << i;
    }
    return identity;
}

/*
 * A bus reset answers with a presence pulse; a bit slot outside any
 * transaction is not driven and samples one. The pre-responder model
 * sampled one on every slot, reset included, and that absence is what
 * the firmware reported as an unreadable remote NIC.
 */
static void test_a_bus_reset_reports_presence(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    g_assert_cmphex(onewire_slot(qts, HUB_LOCAL, ONEWIRE_PULSE_RESET),
                    ==, 0);
    g_assert_cmphex(onewire_slot(qts, HUB_LOCAL, ONEWIRE_PULSE_SHORT),
                    ==, MD_ONEWIRE_RXDATA);

    /* The programmed timing is untouched by running the slots. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_ONEWIRE) &
                    MD_ONEWIRE_PHASE_HI, ==, 0x31ULL << 27);

    qtest_quit(qts);
}

/* The search yields the synthetic identity, family and check byte valid. */
static void test_the_search_reads_a_valid_identity(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t identity = onewire_search_identity(qts, HUB_LOCAL);

    g_assert_cmphex(identity & 0xff, ==, ONEWIRE_FAMILY_DS2505);
    g_assert_cmphex(identity >> 56, ==, onewire_crc8(identity, 7));
    g_assert_cmphex(identity, ==,
                    onewire_expected_identity(HUB_ONEWIRE_SERIAL(0)));

    qtest_quit(qts);
}

/* Per-unit identity: each node's part carries its own serial. */
static void test_each_hub_carries_its_own_identity(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1");
    uint64_t node0 = onewire_search_identity(qts, HUB_BY_NODE);
    uint64_t node1 = onewire_search_identity(qts, HUB_BY_NODE + NODE_STRIDE);

    g_assert_cmphex(node0, ==,
                    onewire_expected_identity(HUB_ONEWIRE_SERIAL(0)));
    g_assert_cmphex(node1, ==,
                    onewire_expected_identity(HUB_ONEWIRE_SERIAL(1)));
    g_assert_cmphex(node0, !=, node1);

    qtest_quit(qts);
}

/*
 * The memory-function reads, per the public DS2505 datasheet: an
 * unprogrammed part serves 0xff everywhere -- a redirection byte of
 * 0xff means "no redirection, data valid" by the part's own spec --
 * behind correct CRC16 framing. The firmware's headless bring-up
 * failed here next: "invalid crc16 reading redirection map page 0",
 * because the selected part went silent and the frame read all-ones.
 */
#define ONEWIRE_CMD_SKIP        0xcc
#define ONEWIRE_CMD_READ_MEM    0xf0
#define ONEWIRE_CMD_READ_STATUS 0xaa
#define ONEWIRE_CMD_READ_EXT    0xa5

/* The Dallas CRC16; the part transmits its complement, low byte first. */
static uint16_t onewire_crc16(uint16_t crc, const uint8_t *data,
                              unsigned int len)
{
    unsigned int i, bit;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
    }
    return crc;
}

static void onewire_write_byte(QTestState *qts, uint64_t hub, uint8_t byte)
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        onewire_write_bit(qts, hub, (byte >> i) & 1);
    }
}

static uint8_t onewire_read_byte(QTestState *qts, uint64_t hub)
{
    uint8_t byte = 0;
    unsigned int i;

    for (i = 0; i < 8; i++) {
        byte |= onewire_slot(qts, hub, ONEWIRE_PULSE_SHORT) << i;
    }
    return byte;
}

/* Reset, presence, and a skip: the single part is selected. */
static void onewire_select(QTestState *qts, uint64_t hub)
{
    g_assert_cmphex(onewire_slot(qts, hub, ONEWIRE_PULSE_RESET), ==, 0);
    onewire_write_byte(qts, hub, ONEWIRE_CMD_SKIP);
}

/* The received CRC16 pair must complement the locally computed one. */
static void onewire_assert_crc16(QTestState *qts, uint64_t hub, uint16_t crc)
{
    uint16_t sent = onewire_read_byte(qts, hub);

    sent |= (uint16_t)onewire_read_byte(qts, hub) << 8;
    g_assert_cmphex(sent, ==, (uint16_t)~crc);
}

/*
 * The redirection map, as the firmware reads it: a status read at
 * 0x100, an 8-byte page of 0xff, then the CRC16 of command, address
 * and data. The next page follows with a CRC16 of its data alone.
 */
static void test_a_status_read_frames_the_redirection_map(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t request[3] = { ONEWIRE_CMD_READ_STATUS, 0x00, 0x01 };
    uint8_t page[8];
    uint16_t crc;
    unsigned int i;

    onewire_select(qts, HUB_LOCAL);
    onewire_write_byte(qts, HUB_LOCAL, request[0]);
    onewire_write_byte(qts, HUB_LOCAL, request[1]);
    onewire_write_byte(qts, HUB_LOCAL, request[2]);

    for (i = 0; i < 8; i++) {
        page[i] = onewire_read_byte(qts, HUB_LOCAL);
        g_assert_cmphex(page[i], ==, 0xff);
    }
    crc = onewire_crc16(0, request, sizeof(request));
    onewire_assert_crc16(qts, HUB_LOCAL,
                         onewire_crc16(crc, page, sizeof(page)));

    /* The following page frames its own bytes only. */
    for (i = 0; i < 8; i++) {
        page[i] = onewire_read_byte(qts, HUB_LOCAL);
        g_assert_cmphex(page[i], ==, 0xff);
    }
    onewire_assert_crc16(qts, HUB_LOCAL,
                         onewire_crc16(0, page, sizeof(page)));

    qtest_quit(qts);
}

/*
 * An extended read of page 0 on a blank part: the redirection byte
 * (0xff, no redirection), its request-covering CRC16, the 32-byte
 * page of 0xff, and the page's own CRC16.
 */
static void test_an_extended_read_serves_a_blank_page(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t request[4] = { ONEWIRE_CMD_READ_EXT, 0x00, 0x00, 0 };
    uint8_t page[32];
    unsigned int i;

    onewire_select(qts, HUB_LOCAL);
    onewire_write_byte(qts, HUB_LOCAL, request[0]);
    onewire_write_byte(qts, HUB_LOCAL, request[1]);
    onewire_write_byte(qts, HUB_LOCAL, request[2]);

    request[3] = onewire_read_byte(qts, HUB_LOCAL);
    g_assert_cmphex(request[3], ==, 0xff);
    onewire_assert_crc16(qts, HUB_LOCAL,
                         onewire_crc16(0, request, sizeof(request)));

    for (i = 0; i < 32; i++) {
        page[i] = onewire_read_byte(qts, HUB_LOCAL);
        g_assert_cmphex(page[i], ==, 0xff);
    }
    onewire_assert_crc16(qts, HUB_LOCAL,
                         onewire_crc16(0, page, sizeof(page)));

    qtest_quit(qts);
}

/*
 * A plain memory read streams to the end of the data field, then
 * sixteen further slots carry the CRC16 of command, address and every
 * data byte served. Started on the last eight bytes to keep the slot
 * count small.
 */
static void test_a_memory_read_ends_with_its_crc(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t request[3] = { ONEWIRE_CMD_READ_MEM, 0xf8, 0x07 };
    uint8_t tail[8];
    uint16_t crc;
    unsigned int i;

    onewire_select(qts, HUB_LOCAL);
    onewire_write_byte(qts, HUB_LOCAL, request[0]);
    onewire_write_byte(qts, HUB_LOCAL, request[1]);
    onewire_write_byte(qts, HUB_LOCAL, request[2]);

    for (i = 0; i < 8; i++) {
        tail[i] = onewire_read_byte(qts, HUB_LOCAL);
        g_assert_cmphex(tail[i], ==, 0xff);
    }
    crc = onewire_crc16(0, request, sizeof(request));
    onewire_assert_crc16(qts, HUB_LOCAL,
                         onewire_crc16(crc, tail, sizeof(tail)));

    /* Past the CRC the part has left the bus and the line reads one. */
    g_assert_cmphex(onewire_read_byte(qts, HUB_LOCAL), ==, 0xff);

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

    /* The masks store what is written; the IRIX table's read-only is wrong. */
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

    /* Zeroed at construction; the machine's own reset does not touch them. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_SPOOL_A), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_SPOOL_B), ==, 0);

    /* Bits 31:3 only. */
    qtest_writeq(qts, HUB_LOCAL + PI_ERR_SPOOL_A, ~0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_SPOOL_A), ==,
                    PI_ERR_SPOOL_MASK);

    /* A realistic marker, already aligned, round-trips exactly. */
    qtest_writeq(qts, HUB_LOCAL + PI_ERR_SPOOL_B, 0xdeadbee0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_SPOOL_B), ==,
                    0xdeadbee0ULL);

    qtest_quit(qts);
}

static void test_pi_error_register_shell(const void *data)
{
    static const uint64_t status[] = {
        PI_ERR_STATUS0_A, PI_ERR_STATUS0_CLR_A,
        PI_ERR_STATUS1_A, PI_ERR_STATUS1_CLR_A,
        PI_ERR_STATUS0_B, PI_ERR_STATUS0_CLR_B,
        PI_ERR_STATUS1_B, PI_ERR_STATUS1_CLR_B,
    };
    QTestState *qts = qtest_init("-M origin200");
    unsigned int i;

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_PENDING), ==, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_ERR_PENDING, ~0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_PENDING), ==, 0);

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_MASK_A), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_MASK_B), ==, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_ERR_MASK_A, ~0ULL);
    qtest_writeq(qts, HUB_LOCAL + PI_ERR_MASK_B, 0x0123456ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_MASK_A), ==,
                    PI_ERR_MASK);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_MASK_B), ==,
                    0x0123456ULL);

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_SPOOL_SIZE), ==, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_ERR_SPOOL_SIZE, ~0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_SPOOL_SIZE), ==,
                    PI_ERR_SPOOL_SIZE_MASK);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_MASK_A), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_MASK_B), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_SPOOL_SIZE), ==, 0);

    for (i = 0; i < ARRAY_SIZE(status); i++) {
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + status[i]), ==, 0);
    }

    qtest_quit(qts);
}

/* Nothing times out here: both read zero and a write has nothing to clear. */
static void test_pi_timedout_buffers(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    static const uint64_t regs[] = {
        PI_TIMEDOUT_BUF_A, PI_TIMEDOUT_BUF_B,
    };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(regs); i++) {
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + regs[i]), ==, 0);
        qtest_writeq(qts, HUB_LOCAL + regs[i], ~0ULL);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + regs[i]), ==, 0);
    }

    qtest_quit(qts);
}

/* Nothing in flight: all zero; the deallocation polls need the bit clear. */
static void test_io_queue_registers(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    static const uint64_t regs[] = {
        IO_PIO_DEALLOC, IO_CRB_DEALLOC, IO_IOQ_FIFO_DEPTH,
        IO_CRB_MGMT, IO_CRB_CONTROL,
    };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(regs); i++) {
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + regs[i]), ==, 0);
        qtest_writeq(qts, HUB_LOCAL + regs[i], ~0ULL);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + regs[i]), ==, 0);
    }

    qtest_quit(qts);
}

static void test_cpu_enable_registers_store_what_is_written(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* The installed boot processor is enabled; the empty socket is not. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CPU_ENABLE_A), ==, 1);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CPU_ENABLE_B), ==, 0);

    qtest_writeq(qts, HUB_LOCAL + PI_CPU_ENABLE_A, 1);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CPU_ENABLE_A), ==, 1);
    qtest_writeq(qts, HUB_LOCAL + PI_CPU_ENABLE_A, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CPU_ENABLE_A), ==, 0);

    /* Processor B is not installed; clearing its enable must not crash. */
    qtest_writeq(qts, HUB_LOCAL + PI_CPU_ENABLE_B, 1);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CPU_ENABLE_B), ==, 1);
    qtest_writeq(qts, HUB_LOCAL + PI_CPU_ENABLE_B, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CPU_ENABLE_B), ==, 0);

    qtest_quit(qts);
}

static void test_md_indicator_registers(const void *data)
{
    /*
     * Meaningless values on purpose: a real code in the suite log would
     * corrupt every search for that code.
     */
    QTestState *qts = qtest_init("-M origin200,debug-leds=off");

    /* Write-only: the write is accepted and nothing is stored. */
    qtest_writeq(qts, HUB_LOCAL + MD_INDICATOR_0, 0xff);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_INDICATOR_0), ==, 0);

    qtest_writeq(qts, HUB_LOCAL + MD_INDICATOR_1, 0xfe);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_INDICATOR_1), ==, 0);

    qtest_quit(qts);
}

/* The decoded line arrives with no -d or -trace; -D cannot arm the event. */
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

    /* The decoded text, not merely the code. */
    g_assert_nonnull(strstr(found, "sgi_hub_led CPU 0: 0x01 "
                                   "(progress: initializing the CPU)"));

    g_assert_cmpint(g_unlink(log), ==, 0);
}

static void test_sn00_indicator_shown_by_default(const void *data)
{
    g_autofree char *log = g_strdup_printf("%s/sn0-sn00-led-XXXXXX.log",
                                           g_get_tmp_dir());
    g_autofree char *found = NULL;
    g_autoptr(GError) err = NULL;
    QTestState *qts;
    int fd = g_mkstemp(log);

    g_assert_cmpint(fd, >=, 0);
    close(fd);

    qts = qtest_initf("-M origin200 -D %s", log);
    qtest_writeq(qts, HUB_LOCAL + MD_SN00_INDICATOR_0, 0x01);
    qtest_writeq(qts, HUB_LOCAL + MD_SN00_INDICATOR_1, 0x02);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_SN00_INDICATOR_0), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_SN00_INDICATOR_1), ==, 0);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log, &found, NULL, &err));
    g_assert_no_error(err);
    g_assert_nonnull(strstr(found, "sgi_hub_led CPU 0: 0x01 "
                                   "(progress: initializing the CPU)"));
    g_assert_nonnull(strstr(found,
                            "sgi_hub_led CPU 1: 0x02 "
                            "(progress: testing the floating-point unit)"));

    g_assert_cmpint(g_unlink(log), ==, 0);
}

/* ... and stays silent when told to be. */
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
 * Firmware's presence probe at its own 64-bit width: the read-back separates
 * an undeclared implementation width (called twice) from a correct one.
 */
static void junkuart_presence_probe(QTestState *qts, uint8_t *readback)
{
    qtest_writeq(qts, HUB_LOCAL + MD_JUNKUART_SCRATCH, 0x55);
    qtest_writeq(qts, HUB_LOCAL + MD_JUNKUART_IER, 0x00);
    qtest_memread(qts, HUB_LOCAL + MD_JUNKUART_SCRATCH, readback, 8);
}

/*
 * No part by default, or the probe would hide the real console candidates.
 * Raw bytes, paired with the fitted case: a byte-swapped model answers all
 * zeroes on a fitted port too.
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
 * The byte lands in the last of the eight raw bytes on this big-endian
 * target; a little-endian region would answer all zeroes.
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
 * The fitted port is the Hub's own QOM child, not an unattached device.
 * The child is "uart", beside the "junk-uart" bool that fits it.
 */
static void test_junk_uart_is_a_child_of_the_hub(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,junk-uart=on");
    QDict *response = qtest_qmp(qts, "{ 'execute': 'qom-list',"
                                     " 'arguments': { 'path': %s } }",
                                     "/machine/node0/hub");
    QList *children;
    const QListEntry *entry;
    bool found = false;

    g_assert_true(qdict_haskey(response, "return"));
    children = qdict_get_qlist(response, "return");
    for (entry = qlist_first(children); entry; entry = qlist_next(entry)) {
        QDict *child = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(child, "name"), "uart")) {
            g_assert_cmpstr(qdict_get_try_str(child, "type"), ==,
                            "child<serial-mm>");
            found = true;
        }
    }
    g_assert_true(found);

    qobject_unref(response);
    qtest_quit(qts);
}

/* One stride either side of the block belongs to the unimplemented default. */
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
 * Every chardev claim is distinct. Only a machine given four backends can
 * show a collision; QEMU refuses at realize and qtest_init never returns.
 */
static void test_every_chardev_claim_is_distinct(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,junk-uart=on "
                                 "-serial null -serial null "
                                 "-serial null -serial null");
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

    /* Firmware assigns node numbers through this register. */
    qtest_writeq(qts, HUB_LOCAL + NI_STATUS, NI_NODE_MASK);
    status = qtest_readq(qts, HUB_LOCAL + NI_STATUS);
    g_assert_cmphex(status & NI_NODE_MASK, ==, NI_NODE_MASK);

    /* The machine's configured revision is fixed and is not writable. */
    g_assert_cmphex(status & NI_IDENTITY, ==, 3 << 4);

    qtest_quit(qts);
}

/*
 * Offset 0 is bits 63:32. A model handing a 32-bit read the low half at
 * offset 0 passes every 64-bit test here.
 */
static void test_a_32_bit_access_selects_the_big_endian_half(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t status = qtest_readq(qts, HUB_LOCAL + NI_STATUS);

    /* The reset value is all in the low word, so the halves cannot swap. */
    g_assert_cmphex(status, !=, 0);
    g_assert_cmphex(qtest_readl(qts, HUB_LOCAL + NI_STATUS), ==,
                    status >> 32);
    g_assert_cmphex(qtest_readl(qts, HUB_LOCAL + NI_STATUS + 4), ==,
                    (uint32_t)status);

    /* A register with reset bits in both halves separates them sharply. */
    g_assert_cmphex(qtest_readl(qts, HUB_LOCAL + MD_CONFIG), ==,
                    MD_CONFIG_RESET >> 32);
    g_assert_cmphex(qtest_readl(qts, HUB_LOCAL + MD_CONFIG + 4), ==,
                    (uint32_t)MD_CONFIG_RESET);

    /* A 32-bit write replaces its own half and leaves the other alone. */
    qtest_writeq(qts, HUB_LOCAL + NI_SCRATCH_0, 0x1111222233334444ULL);
    qtest_writel(qts, HUB_LOCAL + NI_SCRATCH_0, 0xaaaabbbb);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_SCRATCH_0), ==,
                    0xaaaabbbb33334444ULL);
    qtest_writel(qts, HUB_LOCAL + NI_SCRATCH_0 + 4, 0xccccdddd);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_SCRATCH_0), ==,
                    0xaaaabbbbccccddddULL);

    /* A half write still passes through the field mask (bits 31:3). */
    qtest_writel(qts, HUB_LOCAL + PI_ERR_SPOOL_A + 4, 0xdeadbeef);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_SPOOL_A), ==,
                    0xdeadbee8);
    qtest_writel(qts, HUB_LOCAL + PI_ERR_SPOOL_A, 0xffffffff);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_SPOOL_A), ==,
                    0xdeadbee8);

    qtest_quit(qts);
}


/*
 * The local bit resets the machine with either companion, 0x81 or 0x03. The
 * cached-alias size is the reset sentinel; the error-spool must survive.
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

    /* OpenBSD's companion bit, on the same machine. */
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

    /* One device through both halves: a write via one is read via the other. */
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

    /* Distinct values in all seven separate seven registers from one alias. */
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

    /* No eighth: the manual numbers them one through seven. */
    qtest_writeq(qts, HUB_LOCAL + IO_BWIN_ENTRY(IO_BWIN_ENTRIES), ~0ULL);
    g_assert_cmphex(qtest_readq(qts,
                                HUB_LOCAL + IO_BWIN_ENTRY(IO_BWIN_ENTRIES)),
                    ==, 0);

    qtest_quit(qts);
}

static void test_translation_entry_width(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* Thirteen bits; there is no valid bit above them to invent. */
    qtest_writeq(qts, HUB_LOCAL + IO_BWIN_ENTRY(0), ~0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_BWIN_ENTRY(0)), ==, 0x1fff);

    qtest_quit(qts);
}

static void test_big_window_follows_its_entry(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_hub_attach_ioc3(qts);

    /* Write through the small window, read through the big one: one device. */
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
     * The slice bits are the half a model can drop unnoticed: aim one slice
     * up, where nothing is, and require no answer.
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
 * Widget 0, OpenBSD's only route to the crossbar. Its register is read-only,
 * so: aimed at 0 answers as the reset-aimed window, and neither as nothing.
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
     * Disabled by naming widget three, written directly: the guest disable
     * helper's arguments are crossed in most releases. The last read shows
     * the model still answering after reaching nothing.
     */
    qtest_writeq(qts, HUB_LOCAL + IO_BWIN_ENTRY(2),
                 IO_BWIN_WIDGET(IO_BWIN_INVALID_WIDGET));
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_BWIN_ENTRY(2)), ==,
                    IO_BWIN_WIDGET(IO_BWIN_INVALID_WIDGET));

    qtest_readb(qts, BIG_WINDOW(2));

    /* The other way to reach nothing: the right widget, unmodelled space. */
    qtest_writeb(qts, SMALL_WINDOW(CONSOLE_WIDGET) + IOC3_SCRATCH, 0x77);
    qtest_writeq(qts, HUB_LOCAL + IO_BWIN_ENTRY(3),
                 IO_BWIN_WIDGET(CONSOLE_WIDGET) | IO_BWIN_OTHER_SPACE);
    g_assert_cmphex(qtest_readb(qts, BIG_WINDOW(3) + IOC3_SCRATCH), !=, 0x77);

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_PROCESSOR_NUMBER), ==, 0);

    qtest_quit(qts);
}

/*
 * An eight-byte access to an empty window must not be chunked into two
 * four-byte ones; the -d unimp log line names the access size.
 */
static void test_big_window_empty_access_is_not_chunked(const void *data)
{
    g_autofree char *log = g_strdup_printf("%s/sn0-hub-bwin-empty-XXXXXX.log",
                                           g_get_tmp_dir());
    g_autofree char *found = NULL;
    QTestState *qts;
    int fd = g_mkstemp(log);

    g_assert_cmpint(fd, >=, 0);
    close(fd);

    qts = qtest_initf("-M origin200,io-bridge=on -d unimp -D %s", log);

    /* The unmodelled space; io-bridge=on leaves no gap for an absent widget. */
    qtest_writeq(qts, HUB_LOCAL + IO_BWIN_ENTRY(5),
                 IO_BWIN_WIDGET(CONSOLE_WIDGET) | IO_BWIN_OTHER_SPACE);
    qtest_readq(qts, BIG_WINDOW(5));
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_nonnull(strstr(found, "read of 8 bytes"));
    g_assert_null(strstr(found, "read of 4 bytes"));

    g_assert_cmpint(g_unlink(log), ==, 0);
}

static void test_interrupt_modify_sets_and_clears(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* The only way a bit gets set or cleared; nothing reads it back. */
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

    /* Number 70 is bit 6 of the second bank, not of the first. */
    qtest_irq_intercept_out_named(qts, "/machine/node0/hub",
                                  HUB_IRQ_BANK1_LINES);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK1_A, ~0ULL);

    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, INT_MODIFY_SET | 70);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND1), ==,
                    1ULL << (70 - INT_BANK_BITS));
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 0);

    /* The second bank's line rises. */
    g_assert_true(qtest_get_irq(qts, HUB_IRQ_SLICE_A));

    qtest_quit(qts);

    /*
     * And not the first's: the same stimulus with the first bank's mask
     * wide open must leave that bank's line down.
     */
    qts = qtest_init("-M origin200");
    qtest_irq_intercept_out_named(qts, "/machine/node0/hub",
                                  HUB_IRQ_BANK0_LINES);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK0_A, ~0ULL);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK1_A, ~0ULL);

    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, INT_MODIFY_SET | 70);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 0);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_SLICE_A));

    qtest_quit(qts);
}

static void test_a_masked_pending_bit_raises_nothing(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Masked pending is invisible; unmasking must raise the line with nothing
     * written to pending, which separates recomputing from raise-on-set.
     */
    qtest_irq_intercept_out_named(qts, "/machine/node0/hub",
                                  HUB_IRQ_BANK0_LINES);

    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK0_A, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, INT_MODIFY_SET | 12);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 1ULL << 12);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_SLICE_A));

    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK0_A, 1ULL << 12);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 1ULL << 12);
    g_assert_true(qtest_get_irq(qts, HUB_IRQ_SLICE_A));

    /* Clearing the last visible bit drops the line: level-triggered. */
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, 12);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_SLICE_A));

    qtest_quit(qts);
}

static void test_pending_banks_are_masked_per_processor(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -smp 2");

    qtest_irq_intercept_out_named(qts, "/machine/node0/hub",
                                  HUB_IRQ_BANK0_LINES);

    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK0_A, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK0_B, 1ULL << 12);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, INT_MODIFY_SET | 12);

    g_assert_false(qtest_get_irq(qts, HUB_IRQ_SLICE_A));
    g_assert_true(qtest_get_irq(qts, HUB_IRQ_SLICE_B));

    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, 12);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_SLICE_B));

    /* The PROM's order for B: pend while masked, then unmask. */
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK0_B, 0);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, INT_MODIFY_SET | 12);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_SLICE_B));

    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK0_B, 1ULL << 12);
    g_assert_true(qtest_get_irq(qts, HUB_IRQ_SLICE_B));

    qtest_quit(qts);

    /*
     * B's other bank must not follow bank 0's pending bit, even with its
     * mask wide open.
     */
    qts = qtest_init("-M origin200 -smp 2");
    qtest_irq_intercept_out_named(qts, "/machine/node0/hub",
                                  HUB_IRQ_BANK1_LINES);

    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK1_B, ~0ULL);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, INT_MODIFY_SET | 12);
    g_assert_false(qtest_get_irq(qts, HUB_IRQ_SLICE_B));

    qtest_quit(qts);
}

static void test_the_pending_banks_reject_writes(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* Read-only; a writable model passes any modify-register-only guest. */
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

    /* Linux drops its probe on a clear high bit; two or three both mean up. */
    g_assert_cmpuint(state, >=, 2);
    g_assert_cmphex(link & (1ULL << 13), ==, 1ULL << 13);

    /* No guest we boot reads this; only this assertion watches it. */
    g_assert_cmphex(link & 0x3fc00000000ULL, ==, 0x3fc00000000ULL);
    g_assert_cmphex(link & 0x3ff0000ULL, ==, 0x3ff0000ULL);
    g_assert_cmphex(link & 0x3fULL, ==, 0xa);

    qtest_quit(qts);
}

static void test_a_guest_cannot_write_the_link_state(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* A guest must not be able to tell this machine its link is down. */
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

    /* Unset at reset; firmware sets it itself in hubii_llp_enable(). */
    link = qtest_readq(qts, HUB_LOCAL + IO_LINK_STATUS);
    g_assert_cmphex(link & (1ULL << 10), ==, 0);

    /* Writable on its own. */
    qtest_writeq(qts, HUB_LOCAL + IO_LINK_STATUS, 1ULL << 10);
    link = qtest_readq(qts, HUB_LOCAL + IO_LINK_STATUS);
    g_assert_cmphex(link & (1ULL << 10), ==, 1ULL << 10);

    /* And nothing else moves: enabled, but never claiming the link came up. */
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

    /* Present and identifiable to anything walking the crossbar. */
    g_assert_cmphex((id >> 12) & 0xffff, ==, 0xc101);
    g_assert_cmpuint((id >> 28) & 0xf, !=, 0);

    /* The low bit is the crosstalk contract's, inherited deliberately. */
    g_assert_cmphex(id & 1, ==, 1);

    /* The two sources disagree, so what is asserted is the machine's choice. */
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

    /* Widget 1, the address map's counterpart; no guest reads it. */
    g_assert_cmphex(control & 0xf, ==, 1);

    /* And the crossbar credit, the other reset value that is not zero. */
    g_assert_cmphex((control >> 13) & 7, ==, 2);

    /* Nothing to report; bit 4 is undefined in SGI's table. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_WIDGET_STATUS), ==, 0);

    /* Firmware clears this at power-on; all ones so a refusal is measured. */
    qtest_writeq(qts, HUB_LOCAL + IO_WIDGET_STATUS, ~0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_WIDGET_STATUS), ==, 0);

    qtest_quit(qts);
}

static void test_the_crosstalk_timeouts(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* Linux's first two writes must be accepted at reset, with no unlock. */
    qtest_writeq(qts, HUB_LOCAL + IO_XTALK_PRESCALE, 0x800);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_XTALK_PRESCALE), ==, 0x800);

    qtest_writeq(qts, HUB_LOCAL + IO_XTALK_TIMEOUT, 0xff);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + IO_XTALK_TIMEOUT), ==, 0xff);

    qtest_quit(qts);
}

static void test_io_scratch_registers(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* Masked storage; both public sources agree on the mask. */
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

    /* Identity and discovery state; an unimplemented zero looks headless. */
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

static uint64_t vector_operation(QTestState *qts, uint64_t hub,
                                 uint64_t address, uint64_t type)
{
    uint64_t parms = (123ULL << NVP_PIOID_SHIFT) |
                     (45ULL << NVP_WRITEID_SHIFT) | address | type;

    qtest_writeq(qts, hub + NI_VECTOR, 0);
    qtest_writeq(qts, hub + NI_VECTOR_PARMS, parms);
    return qtest_readq(qts, hub + NI_VECTOR_STATUS);
}

static void test_vector_pio_reaches_the_direct_peer(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1");
    uint64_t node0 = HUB_BY_NODE;
    uint64_t node1 = HUB_BY_NODE + NODE_STRIDE;
    uint64_t status;

    qtest_writeq(qts, node1 + NI_SCRATCH_0, 0x0123456789abcdefULL);
    status = vector_operation(qts, node0, NI_SCRATCH_0, PIOTYPE_READ);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_READ);
    g_assert_cmphex(qtest_readq(qts, node0 + NI_VECTOR_READ_DATA), ==,
                    0x0123456789abcdefULL);

    qtest_readq(qts, node0 + NI_VECTOR_CLEAR);
    g_assert_cmphex(qtest_readq(qts, node0 + NI_VECTOR_STATUS) & NVS_VALID,
                    ==, 0);

    qtest_writeq(qts, node0 + NI_VECTOR_DATA, 0xfedcba9876543210ULL);
    status = vector_operation(qts, node0, NI_SCRATCH_1, PIOTYPE_WRITE);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_WRITE);
    g_assert_cmphex(qtest_readq(qts, node1 + NI_SCRATCH_1), ==,
                    0xfedcba9876543210ULL);

    qtest_writeq(qts, node1 + NI_SCRATCH_1, 0);
    qtest_writeq(qts, node0 + NI_VECTOR_DATA, 0x1122334455667788ULL);
    status = vector_operation(qts, node0, NI_SCRATCH_1, PIOTYPE_EXCHANGE);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_EXCHANGE);
    g_assert_cmphex(qtest_readq(qts, node0 + NI_VECTOR_READ_DATA), ==, 0);
    g_assert_cmphex(qtest_readq(qts, node1 + NI_SCRATCH_1), ==,
                    0x1122334455667788ULL);

    qtest_writeq(qts, node0 + NI_VECTOR_DATA, 0x8877665544332211ULL);
    vector_operation(qts, node0, NI_SCRATCH_1, PIOTYPE_EXCHANGE);
    g_assert_cmphex(qtest_readq(qts, node0 + NI_VECTOR_READ_DATA), ==,
                    0x1122334455667788ULL);
    g_assert_cmphex(qtest_readq(qts, node1 + NI_SCRATCH_1), ==,
                    0x1122334455667788ULL);

    g_assert_cmphex(qtest_readq(qts, node1 + NI_LOCAL_TABLE), ==, 0);
    qtest_writeq(qts, node0 + NI_VECTOR_DATA, 0x1b);
    status = vector_operation(qts, node0, NI_LOCAL_TABLE, PIOTYPE_WRITE);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_WRITE);
    g_assert_cmphex(qtest_readq(qts, node1 + NI_LOCAL_TABLE), ==, 0xb);

    status = vector_operation(qts, node0, NI_LOCAL_TABLE, PIOTYPE_READ);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_READ);
    g_assert_cmphex(qtest_readq(qts, node0 + NI_VECTOR_READ_DATA), ==, 0xb);

    g_assert_cmphex(qtest_readq(qts, node1 + NI_META_TABLE_LAST), ==, 0);
    qtest_writeq(qts, node0 + NI_VECTOR_DATA, 0x16);
    status = vector_operation(qts, node0, NI_META_TABLE_LAST, PIOTYPE_WRITE);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_WRITE);
    g_assert_cmphex(qtest_readq(qts, node1 + NI_META_TABLE_LAST), ==, 6);

    qtest_quit(qts);
}

/*
 * No peer: nothing latches and the poll times out -- the datasheet's proposal
 * for an unobserved case, pinned as a choice, not silicon.
 */
static void test_vector_launch_with_no_fabric_stays_silent(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t status;

    status = vector_operation(qts, HUB_LOCAL, NI_SCRATCH_0, PIOTYPE_READ);
    g_assert_cmphex(status & NVS_VALID, ==, 0);

    /* Nothing latched at all, not merely a status with valid clear. */
    qtest_writeq(qts, HUB_LOCAL + NI_VECTOR_DATA, 0x0123456789abcdefULL);
    status = vector_operation(qts, HUB_LOCAL, NI_SCRATCH_1, PIOTYPE_WRITE);
    g_assert_cmphex(status, ==, 0);

    qtest_quit(qts);
}

/*
 * The LLP port registers, through plain PIO. The parameters register is
 * sixteen-bit storage whose reset value carries the null-packet timeout
 * in bits 15:10 and the maximum burst in bits 9:0; the error register
 * and its clearing alias read zero as this machine's stated modeling
 * choice -- the hardware leaves the register undefined at reset and no
 * fabric exists here to latch an error -- so reading the clearing alias
 * neither faults nor changes anything.
 */
static void test_llp_port_registers(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t parms = qtest_readq(qts, HUB_LOCAL + NI_PORT_PARMS);

    g_assert_cmphex(parms, ==, NI_PORT_PARMS_RESET);
    g_assert_cmphex((parms >> 10) & 0x3f, ==, 6);
    g_assert_cmphex(parms & 0x3ff, ==, 0x3f0);

    /* Sixteen-bit storage: the write keeps its low word and no more. */
    qtest_writeq(qts, HUB_LOCAL + NI_PORT_PARMS, 0xabcd5a5aULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_PORT_PARMS), ==,
                    0x5a5a);

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_PORT_ERROR), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_PORT_ERROR_CLEAR),
                    ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_PORT_ERROR), ==, 0);

    qtest_quit(qts);
}

/*
 * The same three registers through the remote vector decode. The stock
 * PROM's link diagnostic vector-reads the peer hub's port error
 * register; a decoder that rejects it answers with the address-error
 * completion, which the diagnostic counts as a link error on every
 * two-chassis boot. Success completions with the right data are the
 * pin.
 */
static void test_llp_port_registers_answer_the_vector_decode(
    const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1");
    uint64_t node0 = HUB_BY_NODE;
    uint64_t status;

    status = vector_operation(qts, node0, NI_PORT_ERROR, PIOTYPE_READ);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_READ);
    g_assert_cmphex(qtest_readq(qts, node0 + NI_VECTOR_READ_DATA), ==, 0);

    status = vector_operation(qts, node0, NI_PORT_ERROR_CLEAR,
                              PIOTYPE_READ);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_READ);
    g_assert_cmphex(qtest_readq(qts, node0 + NI_VECTOR_READ_DATA), ==, 0);

    status = vector_operation(qts, node0, NI_PORT_PARMS, PIOTYPE_READ);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_READ);
    g_assert_cmphex(qtest_readq(qts, node0 + NI_VECTOR_READ_DATA), ==,
                    NI_PORT_PARMS_RESET);

    qtest_quit(qts);
}

/*
 * A vector write lands only on a row that can store. The port error
 * register takes the write and stores nothing -- the datasheet says the
 * register is writable, and no fabric here ever latches a count for the
 * write to clear -- while its clearing alias, which no source documents
 * as writable, rejects the write the way an undecoded register does.
 * Before the decode checked what a row can store, both writes fell
 * through the raw store to state offset zero and corrupted the target
 * hub's object header behind a success completion.
 */
static void test_vector_writes_land_only_on_rows_that_store(
    const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1");
    uint64_t node0 = HUB_BY_NODE;
    uint64_t node1 = HUB_BY_NODE + NODE_STRIDE;
    uint64_t status;

    qtest_writeq(qts, node0 + NI_VECTOR_DATA, ~0ULL);
    status = vector_operation(qts, node0, NI_PORT_ERROR, PIOTYPE_WRITE);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_WRITE);
    g_assert_cmphex(qtest_readq(qts, node1 + NI_PORT_ERROR), ==, 0);

    qtest_writeq(qts, node0 + NI_VECTOR_DATA, ~0ULL);
    status = vector_operation(qts, node0, NI_PORT_ERROR_CLEAR,
                              PIOTYPE_WRITE);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | NVS_ADDRESS_ERROR);

    qtest_quit(qts);
}

/*
 * The PIO-protection register, through plain PIO: full-width storage
 * the model never consults, resetting all ones -- one permissive bit
 * per region. The override register beside it is write-only, and its
 * only documented semantic is the password unlock, a no-op on a model
 * that never enforces the protection: the write is accepted and stores
 * nothing.
 */
static void test_io_protect_registers(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_IO_PROTECT), ==,
                    ~0ULL);

    qtest_writeq(qts, HUB_LOCAL + NI_IO_PROTECT, 0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_IO_PROTECT), ==,
                    0x0123456789abcdefULL);

    /* The unlock is accepted and stores nothing beside it. */
    qtest_writeq(qts, HUB_LOCAL + NI_IO_PROT_OVRRD, NI_PROT_PASSWORD);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_IO_PROTECT), ==,
                    0x0123456789abcdefULL);

    qtest_quit(qts);
}

/*
 * The same two registers through the remote vector decode. The stock
 * PROM's link diagnostic's FIRST vector operation is a write of the
 * peer's PIO-protection register, granting itself cross-link PIO
 * access; with the register undecoded on the vector path the
 * completion carries the address-error response the diagnostic counts
 * as a phantom link error. The override's vector read stays rejected:
 * the register is write-only, so the read keeps the undecoded
 * register's answer.
 */
static void test_io_protect_registers_answer_the_vector_decode(
    const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1");
    uint64_t node0 = HUB_BY_NODE;
    uint64_t node1 = HUB_BY_NODE + NODE_STRIDE;
    uint64_t status;

    status = vector_operation(qts, node0, NI_IO_PROTECT, PIOTYPE_READ);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_READ);
    g_assert_cmphex(qtest_readq(qts, node0 + NI_VECTOR_READ_DATA), ==,
                    ~0ULL);

    qtest_writeq(qts, node0 + NI_VECTOR_DATA, 0xfedcba9876543210ULL);
    status = vector_operation(qts, node0, NI_IO_PROTECT, PIOTYPE_WRITE);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_WRITE);
    g_assert_cmphex(qtest_readq(qts, node1 + NI_IO_PROTECT), ==,
                    0xfedcba9876543210ULL);

    qtest_writeq(qts, node0 + NI_VECTOR_DATA, NI_PROT_PASSWORD);
    status = vector_operation(qts, node0, NI_IO_PROT_OVRRD,
                              PIOTYPE_WRITE);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_WRITE);

    /* A non-password write's effect is unstated; it still completes. */
    qtest_writeq(qts, node0 + NI_VECTOR_DATA, 0x1234);
    status = vector_operation(qts, node0, NI_IO_PROT_OVRRD,
                              PIOTYPE_WRITE);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_WRITE);

    status = vector_operation(qts, node0, NI_IO_PROT_OVRRD,
                              PIOTYPE_READ);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | NVS_ADDRESS_ERROR);

    qtest_quit(qts);
}

/*
 * The protection-configuration register: one bit that permits reset,
 * resetting permissive. Storage the model never consults -- the
 * port-reset path must not require it armed, because Linux never arms
 * it -- but OpenBSD arms it before rebooting, so it must store and
 * read back.
 */
static void test_protection_config_register(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_PROTECTION), ==, 1);

    qtest_writeq(qts, HUB_LOCAL + NI_PROTECTION, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_PROTECTION), ==, 0);

    /* One-bit storage. */
    qtest_writeq(qts, HUB_LOCAL + NI_PROTECTION, 0xffULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_PROTECTION), ==, 1);

    qtest_quit(qts);
}

/*
 * The diagnostic-parameters register: nineteen bits of link fault
 * injection and tuning this machine stores and never acts on. The
 * datasheet defines no reset value for the port-timeout bit and no
 * nonzero reset for the rest, so reading zero at reset is the model's
 * stated choice, the port-error precedent.
 */
static void test_diag_parms_register(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_DIAG_PARMS), ==, 0);

    /* Nineteen-bit storage. */
    qtest_writeq(qts, HUB_LOCAL + NI_DIAG_PARMS, 0xfffa5a5aULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_DIAG_PARMS), ==,
                    0xfffa5a5aULL & NI_DIAG_PARMS_MASK);

    qtest_quit(qts);
}

/*
 * Both registers through the remote vector decode. IRIX 6.5.30's
 * fail-soft burst vector-reads the diagnostic parameters twice and
 * vector-writes them once at every boot; on a two-chassis machine
 * those launches reach the real peer, and an address-error completion
 * there is a phantom failure in the OS rather than the PROM's
 * diagnostic.
 */
static void test_ni_config_registers_answer_the_vector_decode(
    const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1");
    uint64_t node0 = HUB_BY_NODE;
    uint64_t node1 = HUB_BY_NODE + NODE_STRIDE;
    uint64_t status;

    status = vector_operation(qts, node0, NI_PROTECTION, PIOTYPE_READ);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_READ);
    g_assert_cmphex(qtest_readq(qts, node0 + NI_VECTOR_READ_DATA), ==, 1);

    qtest_writeq(qts, node0 + NI_VECTOR_DATA, 0);
    status = vector_operation(qts, node0, NI_PROTECTION, PIOTYPE_WRITE);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_WRITE);
    g_assert_cmphex(qtest_readq(qts, node1 + NI_PROTECTION), ==, 0);

    status = vector_operation(qts, node0, NI_DIAG_PARMS, PIOTYPE_READ);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_READ);
    g_assert_cmphex(qtest_readq(qts, node0 + NI_VECTOR_READ_DATA), ==, 0);

    qtest_writeq(qts, node0 + NI_VECTOR_DATA, 0x1041);
    status = vector_operation(qts, node0, NI_DIAG_PARMS, PIOTYPE_WRITE);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_WRITE);
    g_assert_cmphex(qtest_readq(qts, node1 + NI_DIAG_PARMS), ==, 0x1041);

    qtest_quit(qts);
}

/*
 * A half read of a read-to-clear register is unobserved on hardware; the
 * conservative choice pinned here is that only a full-width read clears.
 */
static void test_a_32_bit_vector_clear_read_leaves_the_status(
    const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1");
    uint64_t node0 = HUB_BY_NODE;
    uint64_t status;

    status = vector_operation(qts, node0, NI_SCRATCH_0, PIOTYPE_READ);
    g_assert_cmphex(status & NVS_VALID, ==, NVS_VALID);

    /* Each half read returns its half and clears nothing. */
    g_assert_cmphex(qtest_readl(qts, node0 + NI_VECTOR_CLEAR), ==,
                    status >> 32);
    g_assert_cmphex(qtest_readq(qts, node0 + NI_VECTOR_STATUS), ==, status);
    g_assert_cmphex(qtest_readl(qts, node0 + NI_VECTOR_CLEAR + 4), ==,
                    (uint32_t)status);
    g_assert_cmphex(qtest_readq(qts, node0 + NI_VECTOR_STATUS), ==, status);

    /* The full-width read is the clear. */
    qtest_readq(qts, node0 + NI_VECTOR_CLEAR);
    g_assert_cmphex(qtest_readq(qts, node0 + NI_VECTOR_STATUS) & NVS_VALID,
                    ==, 0);

    qtest_quit(qts);
}

/* Route tables must clear on a later reset, not only at power-on. */
static void test_network_routes_reset_to_zero(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_writeq(qts, HUB_LOCAL + NI_LOCAL_TABLE, 0x1b);
    qtest_writeq(qts, HUB_LOCAL + NI_META_TABLE_LAST, 0x16);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_LOCAL_TABLE), ==, 0xb);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_META_TABLE_LAST), ==, 6);

    qtest_system_reset(qts);

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_LOCAL_TABLE), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_META_TABLE_LAST), ==, 0);

    qtest_quit(qts);
}

static void test_stock_prom_hubs_start_unnumbered(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200,synthesized-firmware=off -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1");
    uint64_t node0 = HUB_BY_NODE;
    uint64_t node1 = HUB_BY_NODE + NODE_STRIDE;
    uint64_t peer_status;
    uint64_t status;

    g_assert_cmphex(qtest_readq(qts, node0 + NI_STATUS) & NI_NODE_MASK,
                    ==, 0);

    /*
     * The peer's own field: while it is unnumbered no qualified address
     * reaches it, so the instrument is a vector read over the link.
     */
    status = vector_operation(qts, node0, NI_STATUS, PIOTYPE_READ);
    g_assert_cmphex(status & NVS_TARGET_MASK, ==, 0);
    peer_status = qtest_readq(qts, node0 + NI_VECTOR_READ_DATA);
    g_assert_cmphex(peer_status & NI_NODE_MASK, ==, 0);

    qtest_writeq(qts, node0 + NI_VECTOR_DATA, peer_status | (1ULL << 8));
    status = vector_operation(qts, node0, NI_STATUS, PIOTYPE_WRITE);
    g_assert_cmphex(status & NVS_TARGET_MASK, ==,
                    1ULL << NVS_TARGET_SHIFT);
    g_assert_cmphex(qtest_readq(qts, node1 + NI_STATUS) & NI_NODE_MASK,
                    ==, 1ULL << 8);

    status = vector_operation(qts, node0, NI_STATUS, PIOTYPE_READ);
    g_assert_cmphex(status & NVS_TARGET_MASK, ==, 1ULL << NVS_TARGET_SHIFT);

    qtest_quit(qts);
}

/*
 * The claim decode: a Hub answers the node-qualified stride whose number
 * equals its programmed node number, not its board position. These tests
 * read through a processor's own address space -- the claim overlays are
 * per-view, so the always-correct global space cannot substitute for it.
 * The processor must be named in the same request: each
 * human-monitor-command gets its own monitor, so an "xp" after a separate
 * "cpu" command reads processor zero forever. Unlike the qtest protocol,
 * this path also surfaces a failed completion, as "Cannot access memory".
 */
static bool cpu_view_readq(QTestState *qts, unsigned int cpu, uint64_t phys,
                           uint64_t *value)
{
    g_autofree char *command = g_strdup_printf("xp/1gx 0x%" PRIx64, phys);
    g_autoptr(QDict) response = NULL;
    const char *dumped;
    const char *hex;

    response = qtest_qmp(qts,
                         "{ 'execute': 'human-monitor-command',"
                         "  'arguments': { 'command-line': %s,"
                         "                 'cpu-index': %u } }",
                         command, cpu);
    g_assert(qdict_haskey(response, "return"));
    dumped = qdict_get_str(response, "return");
    if (strstr(dumped, "Cannot access memory")) {
        return false;
    }
    hex = strchr(dumped, ':');
    g_assert_nonnull(hex);
    g_assert_cmpint(sscanf(hex + 1, " 0x%" SCNx64, value), ==, 1);
    return true;
}

static uint64_t cpu_view_word64(QTestState *qts, unsigned int cpu,
                                uint64_t phys)
{
    uint64_t value = 0;

    g_assert_true(cpu_view_readq(qts, cpu, phys, &value));
    return value;
}

/* The two-node machine, Hubs seeded with their real numbers. */
static QTestState *qtest_init_dual_node(void)
{
    return qtest_init(
        "-M origin200 -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1");
}

/* Node 1's processor slice A: processors 0-1 are node 0's. */
#define NODE1_CPU 2

/* Distinguishable plants for the link-diagnostics store, 19 bits wide. */
#define DIAG_PLANT_NODE0 0x10001ULL
#define DIAG_PLANT_NODE1 0x20002ULL

/* Clear of the firmware structures, the promdata window and the PROM copy. */
#define MEM_PROBE        0x00800000ULL
#define MEM_PLANT_NODE0  0x1111222233334444ULL
#define MEM_PLANT_NODE1  0x5555666677778888ULL

/*
 * A node-qualified Hub register access resolves by the programmed node
 * number. Reprogramming node 1's Hub to zero -- the value every Hub resets
 * to under a stock PROM -- must make a node-zero-qualified access from
 * node 1's processor land on node 1's own Hub, exactly as each node's
 * pre-assignment self-addressed init requires; restoring the real number
 * must send the same access back to the peer.
 */
static void test_qualified_window_follows_the_programmed_number(
    const void *data)
{
    QTestState *qts = qtest_init_dual_node();
    uint64_t hub0 = HUB_BY_NODE;
    uint64_t hub1 = HUB_BY_NODE + NODE_STRIDE;

    /* Distinguishable state in each Hub, planted by board position. */
    qtest_writeq(qts, HUB_LOCAL + NI_DIAG_PARMS, DIAG_PLANT_NODE0);
    qtest_writeq(qts, NODE_STRIDE + HUB_LOCAL + NI_DIAG_PARMS,
                 DIAG_PLANT_NODE1);

    /* Seeded at the real numbers, the claim is the static placement. */
    g_assert_cmphex(cpu_view_word64(qts, 0, hub0 + NI_DIAG_PARMS), ==,
                    DIAG_PLANT_NODE0);
    g_assert_cmphex(cpu_view_word64(qts, 0, hub1 + NI_DIAG_PARMS), ==,
                    DIAG_PLANT_NODE1);
    g_assert_cmphex(cpu_view_word64(qts, NODE1_CPU, hub0 + NI_DIAG_PARMS),
                    ==, DIAG_PLANT_NODE0);
    g_assert_cmphex(cpu_view_word64(qts, NODE1_CPU, hub1 + NI_DIAG_PARMS),
                    ==, DIAG_PLANT_NODE1);

    /* Node 1's Hub renumbered to zero: the stock PROM's reset state. */
    qtest_writeq(qts, NODE_STRIDE + HUB_LOCAL + NI_STATUS, 0);

    /* Node 1's processor now reaches its own Hub through node zero. */
    g_assert_cmphex(cpu_view_word64(qts, NODE1_CPU, hub0 + NI_DIAG_PARMS),
                    ==, DIAG_PLANT_NODE1);
    /* And its own Hub's status shows the renumbering took. */
    g_assert_cmphex(cpu_view_word64(qts, NODE1_CPU, hub0 + NI_STATUS) &
                    NI_NODE_MASK, ==, 0);
    /* Node 0's Hub still claims zero for its own processors. */
    g_assert_cmphex(cpu_view_word64(qts, 0, hub0 + NI_DIAG_PARMS), ==,
                    DIAG_PLANT_NODE0);

    /* The real number restored: the same address reaches the peer again. */
    assign_node1_number(qts);
    g_assert_cmphex(cpu_view_word64(qts, NODE1_CPU, hub0 + NI_DIAG_PARMS),
                    ==, DIAG_PLANT_NODE0);
    g_assert_cmphex(cpu_view_word64(qts, NODE1_CPU, hub1 + NI_DIAG_PARMS),
                    ==, DIAG_PLANT_NODE1);

    qtest_quit(qts);
}

/*
 * A stride claimed by neither Hub fails rather than reaching either --
 * discovery counts on a probe of an absent node not completing -- and the
 * failure is not sticky.
 */
static void test_unclaimed_node_number_fails(const void *data)
{
    QTestState *qts = qtest_init_dual_node();
    uint64_t hub1 = HUB_BY_NODE + NODE_STRIDE;
    uint64_t value;

    qtest_writeq(qts, NODE_STRIDE + HUB_LOCAL + NI_DIAG_PARMS,
                 DIAG_PLANT_NODE1);

    /* Both Hubs at zero: node-one-qualified addresses name no Hub. */
    qtest_writeq(qts, NODE_STRIDE + HUB_LOCAL + NI_STATUS, 0);
    g_assert_false(cpu_view_readq(qts, NODE1_CPU, hub1 + NI_DIAG_PARMS,
                                  &value));
    g_assert_false(cpu_view_readq(qts, 0, hub1 + NI_DIAG_PARMS, &value));

    /* Not sticky: reclaimed over the link, the address answers again. */
    assign_node1_number(qts);
    g_assert_cmphex(cpu_view_word64(qts, NODE1_CPU, hub1 + NI_DIAG_PARMS),
                    ==, DIAG_PLANT_NODE1);

    qtest_quit(qts);
}

/*
 * The memory windows follow the same claim: a node-qualified memory read
 * never returns the other node's word, in any programmed-number state.
 * The complement of the address-map suite's two-node agreement case.
 * Node-field-zero reads from node 1's own processor stay under the
 * nameless local windows, which name the reader's node throughout.
 */
static void test_qualified_memory_follows_the_programmed_number(
    const void *data)
{
    QTestState *qts = qtest_init_dual_node();
    uint64_t value;

    /* A distinct word in each node's memory, planted by board position. */
    qtest_writeq(qts, MEM_PROBE, MEM_PLANT_NODE0);
    qtest_writeq(qts, NODE_STRIDE + MEM_PROBE, MEM_PLANT_NODE1);

    /* Seeded at the real numbers, every view agrees with the placement. */
    g_assert_cmphex(cpu_view_word64(qts, 0, MEM_PROBE), ==,
                    MEM_PLANT_NODE0);
    g_assert_cmphex(cpu_view_word64(qts, 0, NODE_STRIDE + MEM_PROBE), ==,
                    MEM_PLANT_NODE1);
    g_assert_cmphex(cpu_view_word64(qts, NODE1_CPU, NODE_STRIDE + MEM_PROBE),
                    ==, MEM_PLANT_NODE1);

    /*
     * Node 1's Hub at zero: its early init addresses its own memory as
     * node zero, and node one's stride names no Hub at all.
     */
    qtest_writeq(qts, NODE_STRIDE + HUB_LOCAL + NI_STATUS, 0);
    g_assert_cmphex(cpu_view_word64(qts, NODE1_CPU, MEM_PROBE), ==,
                    MEM_PLANT_NODE1);
    g_assert_false(cpu_view_readq(qts, NODE1_CPU, NODE_STRIDE + MEM_PROBE,
                                  &value));
    g_assert_false(cpu_view_readq(qts, 0, NODE_STRIDE + MEM_PROBE, &value));

    /* The real number restored: the qualified stride reaches node 1. */
    assign_node1_number(qts);
    g_assert_cmphex(cpu_view_word64(qts, NODE1_CPU, NODE_STRIDE + MEM_PROBE),
                    ==, MEM_PLANT_NODE1);
    g_assert_cmphex(cpu_view_word64(qts, 0, NODE_STRIDE + MEM_PROBE), ==,
                    MEM_PLANT_NODE1);

    qtest_quit(qts);
}

static void test_md_perf_cnt0(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* Storage that never counts; a self-test's reset-and-check is honest. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_PERF_CNT0), ==, 0);

    qtest_writeq(qts, HUB_LOCAL + MD_PERF_CNT0, ~0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + MD_PERF_CNT0), ==,
                    MD_PERF_CNT_MASK);

    qtest_quit(qts);
}

static void test_md_config(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* The non-zero reset value is the fact; firmware also round-trips it. */
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

    /* All eight read zero, as firmware's memory test needs; writes refused. */
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

    /* Write-only: nothing to read back, so only acceptance is asserted. */
    qtest_writeq(qts, HUB_LOCAL + MD_MEM_DIMM_INIT, 0xf00000029ULL);
    qtest_writeq(qts, HUB_LOCAL + MD_DIR_DIMM_INIT, 0xf00000029ULL);

    qtest_quit(qts);
}

static void test_pi_rt_local_ctrl(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* Bit 16 resets set; the network-clock bits stay outside the mask. */
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
     * Slots 7-14's C register reads zero and refuses writes; all ones, since
     * zero into a zero register passes whether or not refused.
     */
    for (addr = IO_ICRB_C_BASE; addr <= IO_ICRB_C_LAST_OFF;
         addr += IO_ICRB_C_STRIDE) {
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + addr), ==, 0);
        qtest_writeq(qts, HUB_LOCAL + addr, ~0ULL);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + addr), ==, 0);
    }

    qtest_quit(qts);
}

static void test_bte_registers_reset_to_zero(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    unsigned int e;
    uint64_t reg;

    /* Zero at reset, and back to zero after a reset from a programmed state. */
    for (e = 0; e < 2; e++) {
        for (reg = BTE_LENGTH(e); reg <= BTE_INTERRUPT(e); reg += 8) {
            g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + reg), ==, 0);
        }
    }

    for (e = 0; e < 2; e++) {
        /* Control first while busy is clear, so nothing launches. */
        qtest_writeq(qts, HUB_LOCAL + BTE_CONTROL(e),
                     BTE_CTL_POISON | BTE_CTL_NOTIFY | BTE_CTL_ZFILL);
        qtest_writeq(qts, HUB_LOCAL + BTE_LENGTH(e), BTE_BUSY | 0x123);
        qtest_writeq(qts, HUB_LOCAL + BTE_SOURCE(e), BTE_TEST_SRC);
        qtest_writeq(qts, HUB_LOCAL + BTE_DEST(e), BTE_TEST_DST);
        qtest_writeq(qts, HUB_LOCAL + BTE_NOTIFY(e), BTE_TEST_NOTIFY);
        qtest_writeq(qts, HUB_LOCAL + BTE_INTERRUPT(e),
                     BTE_INT_LEVEL(9) | 0);
    }

    qtest_system_reset(qts);

    for (e = 0; e < 2; e++) {
        for (reg = BTE_LENGTH(e); reg <= BTE_INTERRUPT(e); reg += 8) {
            g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + reg), ==, 0);
        }
    }

    qtest_quit(qts);
}

static void test_bte_length_status_field_access(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    unsigned int e;

    /*
     * Masked storage, not writable-everything. The error bit is left out:
     * whether a store can set it is open.
     */
    for (e = 0; e < 2; e++) {
        qtest_writeq(qts, HUB_LOCAL + BTE_LENGTH(e), ~0ULL & ~BTE_ERROR);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + BTE_LENGTH(e)), ==,
                        BTE_BUSY | 0xffff);

        qtest_writeq(qts, HUB_LOCAL + BTE_LENGTH(e), 0);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + BTE_LENGTH(e)), ==, 0);
    }

    qtest_quit(qts);
}

static void test_bte_address_truncation(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    static const uint64_t regs[] = { 0x08, 0x10, 0x20 }; /* src, dst, notify */
    unsigned int e, i;

    /* Bits 39:7 survive; the PROM stores cached-window addresses. */
    for (e = 0; e < 2; e++) {
        for (i = 0; i < G_N_ELEMENTS(regs); i++) {
            uint64_t reg = BTE_BASE(e) + regs[i];

            qtest_writeq(qts, HUB_LOCAL + reg,
                         BTE_CACHED_UPPER | 0x01500000ULL | 0x7f);
            g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + reg), ==,
                            0x01500000ULL);

            qtest_writeq(qts, HUB_LOCAL + reg, ~0ULL);
            g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + reg), ==,
                            BTE_ADDR_MASK);
        }
    }

    qtest_quit(qts);
}

static void test_bte_interrupt_address_fields(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    unsigned int e;

    /* Level at bits 22:16 and node at 8:0 hold; reserved bits read zero. */
    for (e = 0; e < 2; e++) {
        qtest_writeq(qts, HUB_LOCAL + BTE_INTERRUPT(e), ~0ULL);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + BTE_INTERRUPT(e)), ==,
                        BTE_INT_LEVEL(0x7f) | 0x1ff);

        /* The diagnostic's own value for this engine round-trips whole. */
        qtest_writeq(qts, HUB_LOCAL + BTE_INTERRUPT(e),
                     BTE_INT_LEVEL(63 - e));
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + BTE_INTERRUPT(e)), ==,
                        BTE_INT_LEVEL(63 - e));
    }

    qtest_quit(qts);
}

static void test_bte_control_mode_bits(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    unsigned int e;

    /* Mode bits hold, reserved bits read zero, and a load returns the bits. */
    for (e = 0; e < 2; e++) {
        qtest_writeq(qts, HUB_LOCAL + BTE_CONTROL(e), ~0ULL);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + BTE_CONTROL(e)), ==,
                        BTE_CTL_POISON | BTE_CTL_NOTIFY | BTE_CTL_ZFILL);

        qtest_writeq(qts, HUB_LOCAL + BTE_CONTROL(e), 0);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + BTE_CONTROL(e)), ==, 0);
    }

    qtest_quit(qts);
}

static void test_bte_interrupt_mode_copy(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    enum { LINES = 3, SIZE = LINES * BTE_LINE };
    uint8_t expect[SIZE], got[SIZE], notify[8];
    unsigned int i;

    for (i = 0; i < SIZE; i++) {
        expect[i] = 0xc3 ^ i;
    }
    qtest_bufwrite(qts, BTE_TEST_SRC, expect, SIZE);
    qtest_memset(qts, BTE_TEST_DST, 0x11, SIZE);
    qtest_memset(qts, BTE_TEST_NOTIFY, 0xff, sizeof(notify));

    /* An arbitrary level: 63 and 62 are the diagnostic's choice. */
    qtest_writeq(qts, HUB_LOCAL + BTE_LENGTH(0), BTE_BUSY | LINES);
    qtest_writeq(qts, HUB_LOCAL + BTE_SOURCE(0),
                 BTE_CACHED_UPPER | BTE_TEST_SRC);
    qtest_writeq(qts, HUB_LOCAL + BTE_DEST(0),
                 BTE_CACHED_UPPER | BTE_TEST_DST);
    qtest_writeq(qts, HUB_LOCAL + BTE_NOTIFY(0), BTE_TEST_NOTIFY);
    qtest_writeq(qts, HUB_LOCAL + BTE_INTERRUPT(0), BTE_INT_LEVEL(20));
    qtest_writeq(qts, HUB_LOCAL + BTE_CONTROL(0), 0);

    /* Busy cleared and the count drained by the time the guest can look. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + BTE_LENGTH(0)), ==, 0);

    qtest_bufread(qts, BTE_TEST_DST, got, SIZE);
    g_assert_cmpmem(got, SIZE, expect, SIZE);

    /* The programmed pending bit, and the notification path silent. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==,
                    1ULL << 20);
    qtest_bufread(qts, BTE_TEST_NOTIFY, notify, sizeof(notify));
    for (i = 0; i < sizeof(notify); i++) {
        g_assert_cmphex(notify[i], ==, 0xff);
    }

    /* The interrupt-modify register clears it, as any device interrupt. */
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, 20);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 0);

    /* Level 70 lands in the second bank. */
    qtest_writeq(qts, HUB_LOCAL + BTE_LENGTH(0), BTE_BUSY | LINES);
    qtest_writeq(qts, HUB_LOCAL + BTE_INTERRUPT(0), BTE_INT_LEVEL(70));
    qtest_writeq(qts, HUB_LOCAL + BTE_CONTROL(0), 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND1), ==,
                    1ULL << 6);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 0);

    qtest_quit(qts);
}

static void test_bte_both_engines_the_diagnostic_shape(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    enum { SIZE = 0x80000 };  /* 4,096 cache lines, 512 KB */
    static const uint64_t src[2] = { 0x01500000, 0x01580000 };
    static const uint64_t dst[2] = { 0x01600000, 0x01680000 };
    static const uint8_t fill[2] = { 0x5a, 0xa5 };
    g_autofree uint8_t *got = g_malloc(SIZE);
    g_autofree uint8_t *expect = g_malloc(SIZE);
    uint64_t pend;
    unsigned int e;

    /* The diagnostic's sequence: it tests both pending bits together, once. */
    for (e = 0; e < 2; e++) {
        qtest_memset(qts, src[e], fill[e], SIZE);
        qtest_memset(qts, dst[e], 0xcc, SIZE);
    }
    for (e = 0; e < 2; e++) {
        qtest_writeq(qts, HUB_LOCAL + BTE_LENGTH(e), 0x101000);
        qtest_writeq(qts, HUB_LOCAL + BTE_SOURCE(e),
                     BTE_CACHED_UPPER | src[e]);
        qtest_writeq(qts, HUB_LOCAL + BTE_DEST(e),
                     BTE_CACHED_UPPER | dst[e]);
        qtest_writeq(qts, HUB_LOCAL + BTE_INTERRUPT(e),
                     e == 0 ? 0x3f0000 : 0x3e0000);
    }
    qtest_writeq(qts, HUB_LOCAL + BTE_CONTROL(0), 0);
    qtest_writeq(qts, HUB_LOCAL + BTE_CONTROL(1), 0);

    /* Both bits pending together, in one read, and nothing else. */
    pend = qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0);
    g_assert_cmphex(pend, ==, (1ULL << 63) | (1ULL << 62));

    for (e = 0; e < 2; e++) {
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + BTE_LENGTH(e)), ==, 0);
        memset(expect, fill[e], SIZE);
        qtest_bufread(qts, dst[e], got, SIZE);
        g_assert_cmpmem(got, SIZE, expect, SIZE);
    }

    /* And the diagnostic's own clear path: the interrupt-modify stores. */
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, 63);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY, 62);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 0);

    qtest_quit(qts);
}

static void test_bte_notify_mode_copy(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    enum { LINES = 2, SIZE = LINES * BTE_LINE };
    uint8_t expect[SIZE], got[SIZE], line[16];
    unsigned int i;

    for (i = 0; i < SIZE; i++) {
        expect[i] = 0x35 + i;
    }
    qtest_bufwrite(qts, BTE_TEST_SRC, expect, SIZE);
    qtest_memset(qts, BTE_TEST_DST, 0x22, SIZE);

    /* The kernel's protocol: exactly eight bytes land on the notify line. */
    qtest_memset(qts, BTE_TEST_NOTIFY, 0xff, sizeof(line));

    qtest_writeq(qts, HUB_LOCAL + BTE_LENGTH(0), BTE_BUSY | LINES);
    qtest_writeq(qts, HUB_LOCAL + BTE_SOURCE(0), BTE_TEST_SRC);
    qtest_writeq(qts, HUB_LOCAL + BTE_DEST(0), BTE_TEST_DST);
    qtest_writeq(qts, HUB_LOCAL + BTE_NOTIFY(0), BTE_TEST_NOTIFY);
    qtest_writeq(qts, HUB_LOCAL + BTE_CONTROL(0), BTE_CTL_NOTIFY);

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + BTE_LENGTH(0)), ==, 0);

    qtest_bufread(qts, BTE_TEST_NOTIFY, line, sizeof(line));
    for (i = 0; i < 8; i++) {
        g_assert_cmphex(line[i], ==, 0);
    }
    for (i = 8; i < sizeof(line); i++) {
        g_assert_cmphex(line[i], ==, 0xff);
    }

    /* Exactly one completion signal, chosen by the notify bit. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND1), ==, 0);

    qtest_bufread(qts, BTE_TEST_DST, got, SIZE);
    g_assert_cmpmem(got, SIZE, expect, SIZE);

    qtest_quit(qts);
}

static void test_bte_zero_fill(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    enum { LINES = 2, SIZE = LINES * BTE_LINE };
    uint8_t got[SIZE], zeros[SIZE];

    /*
     * The source is zeroed first so the test holds whether or not zero-fill
     * reads it, which is open.
     */
    qtest_memset(qts, BTE_TEST_SRC, 0x00, SIZE);
    qtest_memset(qts, BTE_TEST_DST, 0x77, SIZE);
    qtest_memset(qts, BTE_TEST_NOTIFY, 0xff, 8);

    qtest_writeq(qts, HUB_LOCAL + BTE_LENGTH(0), BTE_BUSY | LINES);
    qtest_writeq(qts, HUB_LOCAL + BTE_SOURCE(0), BTE_TEST_SRC);
    qtest_writeq(qts, HUB_LOCAL + BTE_DEST(0), BTE_TEST_DST);
    qtest_writeq(qts, HUB_LOCAL + BTE_NOTIFY(0), BTE_TEST_NOTIFY);
    qtest_writeq(qts, HUB_LOCAL + BTE_CONTROL(0),
                 BTE_CTL_NOTIFY | BTE_CTL_ZFILL);

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + BTE_LENGTH(0)), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BTE_TEST_NOTIFY), ==, 0);

    memset(zeros, 0, SIZE);
    qtest_bufread(qts, BTE_TEST_DST, got, SIZE);
    g_assert_cmpmem(got, SIZE, zeros, SIZE);

    qtest_quit(qts);
}

static void test_bte_error_latches_and_no_store_clears_it(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    enum { LINES = 4 };
    uint8_t notify[8];
    uint64_t status;
    unsigned int i;

    qtest_memset(qts, BTE_TEST_SRC, 0x44, LINES * BTE_LINE);
    qtest_memset(qts, BTE_TEST_NOTIFY, 0xff, sizeof(notify));

    /* Beyond every bank: error latches beside busy; no completion signals. */
    qtest_writeq(qts, HUB_LOCAL + BTE_LENGTH(0), BTE_BUSY | LINES);
    qtest_writeq(qts, HUB_LOCAL + BTE_SOURCE(0), BTE_TEST_SRC);
    qtest_writeq(qts, HUB_LOCAL + BTE_DEST(0), BTE_TEST_HOLE);
    qtest_writeq(qts, HUB_LOCAL + BTE_NOTIFY(0), BTE_TEST_NOTIFY);
    qtest_writeq(qts, HUB_LOCAL + BTE_CONTROL(0), BTE_CTL_NOTIFY);

    status = qtest_readq(qts, HUB_LOCAL + BTE_LENGTH(0));
    g_assert_cmphex(status, ==, BTE_BUSY | BTE_ERROR | LINES);

    qtest_bufread(qts, BTE_TEST_NOTIFY, notify, sizeof(notify));
    for (i = 0; i < sizeof(notify); i++) {
        g_assert_cmphex(notify[i], ==, 0xff);
    }

    /* A zero store clears busy and length but never a set error bit. */
    qtest_writeq(qts, HUB_LOCAL + BTE_LENGTH(0), 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + BTE_LENGTH(0)), ==,
                    BTE_ERROR);

    /* A later good transfer reports the stale error; byte order shows here. */
    qtest_writeq(qts, HUB_LOCAL + BTE_LENGTH(0), BTE_BUSY | LINES);
    qtest_writeq(qts, HUB_LOCAL + BTE_DEST(0), BTE_TEST_DST);
    qtest_writeq(qts, HUB_LOCAL + BTE_CONTROL(0), BTE_CTL_NOTIFY);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + BTE_LENGTH(0)), ==,
                    BTE_ERROR);
    g_assert_cmphex(qtest_readq(qts, BTE_TEST_NOTIFY), ==, BTE_ERROR);

    /* Only the engine's own error-clear bit absolves it. */
    qtest_writeq(qts, HUB_LOCAL + IO_ERROR_CLEAR, 1ULL << 18);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + BTE_LENGTH(0)), ==,
                    BTE_ERROR);
    qtest_writeq(qts, HUB_LOCAL + IO_ERROR_CLEAR, 1ULL << 17);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + BTE_LENGTH(0)), ==, 0);

    qtest_quit(qts);
}

static void test_bte_engines_see_their_own_node(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200,synthesized-firmware=off -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1");
    const uint64_t node1 = NODE_STRIDE;
    const uint64_t hub1 = HUB_BY_NODE + NODE_STRIDE;
    uint8_t got[BTE_LINE];
    unsigned int i;

    assign_node1_number(qts);

    /*
     * Node-less addresses name the launching engine's own node, as they do
     * for its processors; distinct fills show which node moved.
     */
    qtest_memset(qts, 0x01500000, 0xaa, BTE_LINE);
    qtest_memset(qts, node1 + 0x01500000, 0xbb, BTE_LINE);
    qtest_memset(qts, 0x01600000, 0xcc, BTE_LINE);
    qtest_memset(qts, node1 + 0x01600000, 0xcc, BTE_LINE);

    qtest_writeq(qts, hub1 + BTE_LENGTH(0), BTE_BUSY | 1);
    qtest_writeq(qts, hub1 + BTE_SOURCE(0), BTE_CACHED_UPPER | 0x01500000);
    qtest_writeq(qts, hub1 + BTE_DEST(0), BTE_CACHED_UPPER | 0x01600000);
    qtest_writeq(qts, hub1 + BTE_INTERRUPT(0), BTE_INT_LEVEL(20) | 1);
    qtest_writeq(qts, hub1 + BTE_CONTROL(0), 0);

    g_assert_cmphex(qtest_readq(qts, hub1 + BTE_LENGTH(0)), ==, 0);
    qtest_bufread(qts, node1 + 0x01600000, got, sizeof(got));
    for (i = 0; i < sizeof(got); i++) {
        g_assert_cmphex(got[i], ==, 0xbb);
    }
    qtest_bufread(qts, 0x01600000, got, sizeof(got));
    for (i = 0; i < sizeof(got); i++) {
        g_assert_cmphex(got[i], ==, 0xcc);
    }

    /* An interrupt aimed at the engine's programmed number stays home. */
    g_assert_cmphex(qtest_readq(qts, hub1 + PI_INT_PEND0), ==, 1ULL << 20);
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + PI_INT_PEND0), ==, 0);

    /* The notification address follows the same rule. */
    qtest_memset(qts, 0x01700000, 0xff, 8);
    qtest_memset(qts, node1 + 0x01700000, 0xff, 8);
    qtest_writeq(qts, hub1 + BTE_LENGTH(1), BTE_BUSY | 1);
    qtest_writeq(qts, hub1 + BTE_SOURCE(1), 0x01500000);
    qtest_writeq(qts, hub1 + BTE_DEST(1), 0x01680000);
    qtest_writeq(qts, hub1 + BTE_NOTIFY(1), 0x01700000);
    qtest_writeq(qts, hub1 + BTE_CONTROL(1), BTE_CTL_NOTIFY);
    g_assert_cmphex(qtest_readq(qts, node1 + 0x01700000), ==, 0);
    g_assert_cmphex(qtest_readq(qts, 0x01700000), ==, ~0ULL);

    /* And the mirror: node 0's engines leave node 1's memory alone. */
    qtest_memset(qts, node1 + 0x01600000, 0xcc, BTE_LINE);
    qtest_writeq(qts, HUB_BY_NODE + BTE_LENGTH(0), BTE_BUSY | 1);
    qtest_writeq(qts, HUB_BY_NODE + BTE_SOURCE(0), 0x01500000);
    qtest_writeq(qts, HUB_BY_NODE + BTE_DEST(0), 0x01600000);
    qtest_writeq(qts, HUB_BY_NODE + BTE_INTERRUPT(0), BTE_INT_LEVEL(21));
    qtest_writeq(qts, HUB_BY_NODE + BTE_CONTROL(0), 0);
    qtest_bufread(qts, 0x01600000, got, sizeof(got));
    for (i = 0; i < sizeof(got); i++) {
        g_assert_cmphex(got[i], ==, 0xaa);
    }
    qtest_bufread(qts, node1 + 0x01600000, got, sizeof(got));
    for (i = 0; i < sizeof(got); i++) {
        g_assert_cmphex(got[i], ==, 0xcc);
    }
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + PI_INT_PEND0), ==,
                    1ULL << 21);

    /* An explicit node number still reaches the other node. */
    qtest_memset(qts, 0x01600000, 0xcc, BTE_LINE);
    qtest_writeq(qts, HUB_BY_NODE + BTE_LENGTH(0), BTE_BUSY | 1);
    qtest_writeq(qts, HUB_BY_NODE + BTE_SOURCE(0), node1 + 0x01500000);
    qtest_writeq(qts, HUB_BY_NODE + BTE_DEST(0), 0x01600000);
    qtest_writeq(qts, HUB_BY_NODE + BTE_CONTROL(0), 0);
    qtest_bufread(qts, 0x01600000, got, sizeof(got));
    for (i = 0; i < sizeof(got); i++) {
        g_assert_cmphex(got[i], ==, 0xbb);
    }

    qtest_quit(qts);
}

static void test_bte_zero_length_and_misalignment(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t got[BTE_LINE];
    unsigned int i;

    /* Misaligned programming must not crash; bits 39:7 survive. */
    qtest_writeq(qts, HUB_LOCAL + BTE_SOURCE(0), BTE_TEST_SRC | 0x44);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + BTE_SOURCE(0)), ==,
                    BTE_TEST_SRC);

    /* An armed zero-length launch moves nothing and never wedges: a choice. */
    qtest_memset(qts, BTE_TEST_SRC, 0x99, BTE_LINE);
    qtest_memset(qts, BTE_TEST_DST, 0x66, BTE_LINE);
    qtest_writeq(qts, HUB_LOCAL + BTE_LENGTH(0), BTE_BUSY);
    qtest_writeq(qts, HUB_LOCAL + BTE_SOURCE(0), BTE_TEST_SRC);
    qtest_writeq(qts, HUB_LOCAL + BTE_DEST(0), BTE_TEST_DST);
    qtest_writeq(qts, HUB_LOCAL + BTE_INTERRUPT(0), BTE_INT_LEVEL(5));
    qtest_writeq(qts, HUB_LOCAL + BTE_CONTROL(0), 0);

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + BTE_LENGTH(0)), ==, 0);
    qtest_bufread(qts, BTE_TEST_DST, got, sizeof(got));
    for (i = 0; i < sizeof(got); i++) {
        g_assert_cmphex(got[i], ==, 0x66);
    }

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
    qtest_add_data_func("sn0/hub/sn00-indicator-shown-by-default", NULL,
                        test_sn00_indicator_shown_by_default);
    qtest_add_data_func("sn0/hub/md-indicator-suppressed", NULL,
                        test_md_indicator_suppressed);
    qtest_add_data_func("sn0/hub/junk-uart-absent-by-default", NULL,
                        test_junk_uart_absent_by_default);
    qtest_add_data_func("sn0/hub/junk-uart-fitted-answers-present", NULL,
                        test_junk_uart_fitted_answers_present);
    qtest_add_data_func("sn0/hub/junk-uart-range-is-exact", NULL,
                        test_junk_uart_range_is_exact);
    qtest_add_data_func("sn0/hub/junk-uart-is-a-child-of-the-hub", NULL,
                        test_junk_uart_is_a_child_of_the_hub);
    qtest_add_data_func("sn0/hub/chardev-claims-are-distinct", NULL,
                        test_every_chardev_claim_is_distinct);
    qtest_add_data_func("sn0/hub/status-field-access", NULL,
                        test_status_field_access);
    qtest_add_data_func("sn0/hub/32-bit-access-selects-the-big-endian-half",
                        NULL,
                        test_a_32_bit_access_selects_the_big_endian_half);
    qtest_add_data_func("sn0/hub/32-bit-vector-clear-read-leaves-the-status",
                        NULL,
                        test_a_32_bit_vector_clear_read_leaves_the_status);
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
    qtest_add_data_func("sn0/hub/big-window-empty-access-not-chunked", NULL,
                        test_big_window_empty_access_is_not_chunked);
    qtest_add_data_func("sn0/hub/identifies-itself", NULL,
                        test_the_hub_identifies_itself_on_the_crossbar);
    qtest_add_data_func("sn0/hub/widget-control-reset", NULL,
                        test_the_widget_control_reset_values);
    qtest_add_data_func("sn0/hub/flash-ready-at-both-bits", NULL,
                        test_the_flash_ready_signal_reads_ready_at_both_bits);
    qtest_add_data_func("sn0/hub/dual-node-master-slave-sense", NULL,
                        test_dual_node_master_slave_sense);
    qtest_add_data_func("sn0/hub/crosstalk-timeouts", NULL,
                        test_the_crosstalk_timeouts);
    qtest_add_data_func("sn0/hub/io-scratch-registers", NULL,
                        test_io_scratch_registers);
    qtest_add_data_func("sn0/hub/network-scratch-registers", NULL,
                        test_network_scratch_registers);
    qtest_add_data_func("sn0/hub/vector-pio-direct-peer", NULL,
                        test_vector_pio_reaches_the_direct_peer);
    qtest_add_data_func("sn0/hub/vector-launch-no-fabric-stays-silent", NULL,
                        test_vector_launch_with_no_fabric_stays_silent);
    qtest_add_data_func("sn0/hub/llp-port-registers", NULL,
                        test_llp_port_registers);
    qtest_add_data_func("sn0/hub/llp-port-registers-answer-vector-decode",
                        NULL,
                        test_llp_port_registers_answer_the_vector_decode);
    qtest_add_data_func("sn0/hub/vector-writes-land-only-on-rows-that-store",
                        NULL,
                        test_vector_writes_land_only_on_rows_that_store);
    qtest_add_data_func("sn0/hub/io-protect-registers", NULL,
                        test_io_protect_registers);
    qtest_add_data_func("sn0/hub/io-protect-registers-answer-vector-decode",
                        NULL,
                        test_io_protect_registers_answer_the_vector_decode);
    qtest_add_data_func("sn0/hub/ni-protection-config", NULL,
                        test_protection_config_register);
    qtest_add_data_func("sn0/hub/ni-diag-parms", NULL,
                        test_diag_parms_register);
    qtest_add_data_func("sn0/hub/ni-config-registers-answer-vector-decode",
                        NULL,
                        test_ni_config_registers_answer_the_vector_decode);
    qtest_add_data_func("sn0/hub/network-routes-reset-to-zero", NULL,
                        test_network_routes_reset_to_zero);
    qtest_add_data_func("sn0/hub/stock-prom-hubs-start-unnumbered", NULL,
                        test_stock_prom_hubs_start_unnumbered);
    qtest_add_data_func(
        "sn0/hub/qualified-window-follows-the-programmed-number", NULL,
        test_qualified_window_follows_the_programmed_number);
    qtest_add_data_func("sn0/hub/unclaimed-node-number-fails", NULL,
                        test_unclaimed_node_number_fails);
    qtest_add_data_func(
        "sn0/hub/qualified-memory-follows-the-programmed-number", NULL,
        test_qualified_memory_follows_the_programmed_number);
    qtest_add_data_func("sn0/hub/md-perf-cnt0", NULL, test_md_perf_cnt0);
    qtest_add_data_func("sn0/hub/md-config", NULL, test_md_config);
    qtest_add_data_func("sn0/hub/md-error-cluster", NULL, test_md_error_cluster);
    qtest_add_data_func("sn0/hub/md-dimm-init", NULL, test_md_dimm_init);
    qtest_add_data_func("sn0/hub/pi-rt-local-ctrl", NULL, test_pi_rt_local_ctrl);
    qtest_add_data_func("sn0/hub/pi-error-register-shell", NULL,
                        test_pi_error_register_shell);
    qtest_add_data_func("sn0/hub/pi-timedout-buffers", NULL,
                        test_pi_timedout_buffers);
    qtest_add_data_func("sn0/hub/io-queue-registers", NULL,
                        test_io_queue_registers);
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
    qtest_add_data_func("sn0/hub/behind-comparator-matches-at-once", NULL,
                        test_a_behind_comparator_matches_at_once);
    qtest_add_data_func("sn0/hub/behind-match-survives-rewrite", NULL,
                        test_a_behind_match_survives_an_immediate_rewrite);
    qtest_add_data_func("sn0/hub/maximum-interval-arm-matches", NULL,
                        test_a_maximum_interval_arm_matches_at_once);
    qtest_add_data_func("sn0/hub/counter-move-pends-nothing", NULL,
                        test_moving_the_counter_past_a_comparator_pends_nothing);
    qtest_add_data_func("sn0/hub/rt-comparator-raises-line", NULL,
                        test_the_comparator_raises_and_releases_the_line);
    qtest_add_data_func("sn0/hub/rt-second-processor-line", NULL,
                        test_the_second_processors_comparator_raises_its_own_line);
    qtest_add_data_func("sn0/hub/rt-comparator-pends-when-off", NULL,
                        test_the_comparator_pends_with_its_interrupt_off);
    qtest_add_data_func("sn0/hub/behind-comparator-matches-once", NULL,
                        test_a_behind_comparator_matches_once_not_repeatedly);
    qtest_add_data_func("sn0/hub/interrupt-modify", NULL,
                        test_interrupt_modify_sets_and_clears);
    qtest_add_data_func("sn0/hub/interrupt-number-selects-bank", NULL,
                        test_interrupt_number_selects_the_bank);
    qtest_add_data_func("sn0/hub/masked-pending-raises-nothing", NULL,
                        test_a_masked_pending_bit_raises_nothing);
    qtest_add_data_func("sn0/hub/pending-banks-masked-per-processor", NULL,
                        test_pending_banks_are_masked_per_processor);
    qtest_add_data_func("sn0/hub/one-wire-reset-cancels-a-slot", NULL,
                        test_a_reset_cancels_a_running_slot);
    qtest_add_data_func("sn0/hub/one-wire-resets-to-its-timing", NULL,
                        test_the_one_wire_master_resets_to_its_timing);
    qtest_add_data_func("sn0/hub/one-wire-keeps-its-wide-field", NULL,
                        test_the_one_wire_master_keeps_its_wide_field);
    qtest_add_data_func("sn0/hub/one-wire-start-is-not-finish", NULL,
                        test_starting_a_slot_does_not_report_it_finished);
    qtest_add_data_func("sn0/hub/one-wire-reset-reports-presence", NULL,
                        test_a_bus_reset_reports_presence);
    qtest_add_data_func("sn0/hub/one-wire-search-reads-identity", NULL,
                        test_the_search_reads_a_valid_identity);
    qtest_add_data_func("sn0/hub/one-wire-identity-is-per-unit", NULL,
                        test_each_hub_carries_its_own_identity);
    qtest_add_data_func("sn0/hub/one-wire-status-frames-redirection-map",
                        NULL, test_a_status_read_frames_the_redirection_map);
    qtest_add_data_func("sn0/hub/one-wire-extended-read-serves-blank-page",
                        NULL, test_an_extended_read_serves_a_blank_page);
    qtest_add_data_func("sn0/hub/one-wire-memory-read-ends-with-crc",
                        NULL, test_a_memory_read_ends_with_its_crc);
    qtest_add_data_func("sn0/hub/pending-banks-reject-writes", NULL,
                        test_the_pending_banks_reject_writes);
    qtest_add_data_func("sn0/hub/bte-registers-reset-to-zero", NULL,
                        test_bte_registers_reset_to_zero);
    qtest_add_data_func("sn0/hub/bte-length-status-field-access", NULL,
                        test_bte_length_status_field_access);
    qtest_add_data_func("sn0/hub/bte-address-truncation", NULL,
                        test_bte_address_truncation);
    qtest_add_data_func("sn0/hub/bte-interrupt-address-fields", NULL,
                        test_bte_interrupt_address_fields);
    qtest_add_data_func("sn0/hub/bte-control-mode-bits", NULL,
                        test_bte_control_mode_bits);
    qtest_add_data_func("sn0/hub/bte-interrupt-mode-copy", NULL,
                        test_bte_interrupt_mode_copy);
    qtest_add_data_func("sn0/hub/bte-diagnostic-shape", NULL,
                        test_bte_both_engines_the_diagnostic_shape);
    qtest_add_data_func("sn0/hub/bte-notify-mode-copy", NULL,
                        test_bte_notify_mode_copy);
    qtest_add_data_func("sn0/hub/bte-zero-fill", NULL, test_bte_zero_fill);
    qtest_add_data_func("sn0/hub/bte-error-not-cleared-by-store", NULL,
                        test_bte_error_latches_and_no_store_clears_it);
    qtest_add_data_func("sn0/hub/bte-zero-length-and-misalignment", NULL,
                        test_bte_zero_length_and_misalignment);
    qtest_add_data_func("sn0/hub/bte-engines-see-their-own-node", NULL,
                        test_bte_engines_see_their_own_node);

    return g_test_run();
}
