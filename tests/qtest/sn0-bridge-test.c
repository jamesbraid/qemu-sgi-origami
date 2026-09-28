/*
 * Bridge register tests compare whole words assembled from named fields,
 * including bits without local names.
 * Qtest cannot distinguish refused access from zero without a CPU, and
 * splits wide reads into legal accesses before the chip can reject them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/pci-host/sgi-bridge.h"
#include "libqtest.h"
#include "sn0-user-image.h"
#include "sgi-test.h"

/* Crosstalk space, and the bridge's own widget window inside it. */
#define BRIDGE_WIDGET   8
#define BRIDGE_BASE     (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))

/*
 * Origin 200's onboard Bridge, a fitted part of its node's own System
 * Board: sn0.c creates it directly, as a QOM child of the node object, not
 * wrapped in any board class (the base-io-device step's design; see
 * hw/mips/sgi/sn0.c, sn0_node_build_baseio()).
 */
#define BRIDGE_PATH "/machine/node0/bridge"

/*
 * Registers sit at a slot's offset plus four: Linux names that address,
 * OpenBSD names the slot and folds the four in.
 */
#define REG_DATA        4
#define REG(offset)     (BRIDGE_BASE + (offset) + REG_DATA)

#define BRIDGE_IDENTIFICATION   0x000
#define BRIDGE_STATUS           0x008
#define BRIDGE_CONTROL          0x020
#define BRIDGE_REQUEST_TIMEOUT  0x028
#define BRIDGE_LINK_CONFIG      0x048
#define BRIDGE_TARGET_FLUSH     0x050
#define BRIDGE_DIRECT_MAP       0x080
#define BRIDGE_ARBITRATION      0x0a0
#define BRIDGE_BUS_TIMEOUT      0x0c0

/*
 * The one-wire bus master to the board identity part, and the fields a driver
 * touches: the level sampled off the wire, the flag saying a slot finished,
 * and the two timing fields whose being written is what starts a slot.
 */
#define BRIDGE_NIC              0x0b0
#define NIC_LEVEL               (1u << 0)
#define NIC_DONE                (1u << 1)
#define NIC_SAMPLE(us)          ((uint32_t)(us) << 2)
#define NIC_PULSE(us)           ((uint32_t)(us) << 10)

#define BRIDGE_PCI_TYPE1_CONFIG 0x0c8
#define BRIDGE_DEVICE(n)        (0x200 + 8 * (n))

#define BRIDGE_DEVICES          8

/* The identity a Bridge answers with, and the crossbar identity it must
 * not. */
#define ID_REVISION_SHIFT       28
#define ID_PART_SHIFT           12
#define ID_MANUFACTURER_SHIFT   1
#define ID_ALWAYS_ONE           1
#define BRIDGE_PART             0xc002
#define BRIDGE_MANUFACTURER     0x036
#define XBOW_PART               0
#define XBOW_MANUFACTURER       0

#define ID_PART_OF(v)           (((v) >> ID_PART_SHIFT) & 0xffff)
#define ID_MANUFACTURER_OF(v)   (((v) >> ID_MANUFACTURER_SHIFT) & 0x7ff)

/*
 * The reset state, field by named field. Seven of the control register's
 * fields come out of reset set, which is the whole reason these are written
 * as sums: a total cannot show which of its terms went missing.
 */
#define STATUS_PCI_MODE         (1u << 5)

#define CONTROL_CLOCK_50MHZ     (1u << 30)
#define CONTROL_CLOCK_40MHZ     (1u << 29)
#define CONTROL_CLOCK_33MHZ     (1u << 28)
#define CONTROL_PCI_RESET_PINS  (0xfu << 24)
#define CONTROL_RETRY_CLEAR     (0x3u << 10)
#define CONTROL_CREDIT_RESET    (3u << 12)
#define CONTROL_BIG_ENDIAN      (1u << 9)
#define CONTROL_OUTSTANDING_RESET (0x1fu << 4)
#define CONTROL_WIDGET_RESET    0xfu
#define CONTROL_RESET           (CONTROL_CLOCK_50MHZ | \
                                 CONTROL_CLOCK_40MHZ | \
                                 CONTROL_CLOCK_33MHZ | \
                                 CONTROL_PCI_RESET_PINS | \
                                 CONTROL_CREDIT_RESET | \
                                 CONTROL_BIG_ENDIAN | \
                                 CONTROL_OUTSTANDING_RESET | \
                                 CONTROL_WIDGET_RESET)

#define REQUEST_TIMEOUT_RESET   0xfffffu

#define LINK_CONFIG_RESET       ((0x3ffu << 16) | (0x06u << 10) | 0x010u)

#define DIRECT_ADD_512MB        (1u << 17)
#define DIRECT_MAP_RESET        DIRECT_ADD_512MB

#define ARBITRATION_RESET       ((2u << 16) | (1u << 3))

#define BUS_TIMEOUT_RETRY_HOLD  (0x1fu << 16)
#define BUS_TIMEOUT_GIO         (1u << 12)
#define BUS_TIMEOUT_PCI_RETRY   0x3ffu
#define BUS_TIMEOUT_RESET       (1u << 16)
#define BUS_TIMEOUT_WRITABLE    (BUS_TIMEOUT_RETRY_HOLD | BUS_TIMEOUT_GIO | \
                                 BUS_TIMEOUT_PCI_RETRY)

/* A bus number and a device number, with reserved bits above and below. */
#define TYPE1_BUS_NUMBER        (0xffu << 16)
#define TYPE1_DEVICE_NUMBER     (0x1fu << 11)
#define TYPE1_WRITABLE          (TYPE1_BUS_NUMBER | TYPE1_DEVICE_NUMBER)

#define DEV_ERROR_LOCK          (1u << 28)
#define DEV_PAGE_CROSS_CHECK    (1u << 27)
#define DEV_64BIT_DEVICE        (1u << 22)
#define DEV_SWAP_PROGRAMMED     (1u << 13)
#define DEV_WINDOW_BASE         0xfffu
/*
 * The eight reset values the firmware's sanity diagnostic states, as
 * literals on purpose: the model computes them from a rule, and a test
 * recomputing the rule asserts the formula against itself.
 */
static const uint32_t device_reset[8] = {
    0x18001000, 0x18001002, 0x18001004, 0x18001005,
    0x18001006, 0x18001007, 0x18001008, 0x18001009,
};
/* Bits 28 down to 12 are defined, and so are the twelve of the window
 * base. */
#define DEVICE_WRITABLE         0x1fffffffu

/*
 * The machine states one silicon revision for this chip and both registers
 * that report an identity are derived from it, so the test names it once too.
 */
#define BRIDGE_REVISION         4

#define MACHINE_WITH_BRIDGE     "-M origin200"

/* Physical RAM location for the guest fragment's completion word. */
#define GUEST_MARKER_ADDR       0x30000
#define GUEST_MARKER            0x5a17c0deu

/*
 * PCI type 0 configuration space, and the window each device and function is
 * reached through inside it.
 */
#define BRIDGE_CFG0             0x020000
#define CFG0_DEVICE_STRIDE      0x1000
#define CFG0_FUNCTION_STRIDE    0x100
#define CFG0(device, function)  (BRIDGE_BASE + BRIDGE_CFG0 + \
                                 (device) * CFG0_DEVICE_STRIDE + \
                                 (function) * CFG0_FUNCTION_STRIDE)

/*
 * Any card works; its identity dword repeats two of its four bytes, so the
 * byte-lane assertions rest on the two that differ.
 */
/*
 * Every card names its slot: a test that lets the allocator choose and
 * then reads device 0 asserts allocator behaviour, not this chip's.
 * Devices 0 and 1 hold the machine's own adapters and are never free.
 */
#define PROBE_SLOT              4  /* the card most tests read */
#define PROBE_SLOT_ABOVE        5  /* next device up, for the 4 KB stride */
#define DMA_SLOT                6  /* the bus-mastering card */
#define EMPTY_SLOT              7  /* never populated where emptiness is
 * asserted */

#define PROBE_CARD              " -device lsi53c895a,addr=" stringify(PROBE_SLOT)
#define PROBE_VENDOR            0x1000
#define PROBE_DEVICE            0x0012
#define PROBE_IDENTITY          (PROBE_VENDOR | (PROBE_DEVICE << 16))

/* What a slot with nothing in it answers, which must not be zero. */
#define NO_CARD                 0xffffffffu

/* The configuration registers these tests program on a card. */
#define PCI_COMMAND_IO          0x0001

/* A scratch register: storage behind a card, and the least of the card. */
#define CARD_SCRATCH            0x34

/*
 * Sixteen megabytes apart, wider than any window, so a window
 * reaching a card reached the one it was aimed at.
 */
#define CARD_MMIO(slot)         (0x10000000u + (slot) * 0x1000000u)

/*
 * Stated, not computed: recomputing the model's arithmetic checks that it
 * ran, not that it is right. Two 2 MB windows, then six of 1 MB: the
 * detail every driver gets wrong once.
 */
static const uint64_t window_offset[BRIDGE_DEVICES] = {
    0x200000, 0x400000, 0x600000, 0x700000,
    0x800000, 0x900000, 0xa00000, 0xb00000,
};

#define WINDOW(slot)            (BRIDGE_BASE + window_offset[slot])

/*
 * One plain origin200 serves every case that wants no card and no second
 * node, started in main(). Cases with a card in a slot, two nodes, a
 * firmware image, or a trace file use their own command lines.
 */
static QTestState *qts;

/*
 * Open such a case. The chip's reset restores what these cases read, and
 * checking the control register's reset word here is what proves it, so a
 * case cannot silently inherit the previous case's programming.
 */
static void bridge_reset(void)
{
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_CONTROL)), ==, CONTROL_RESET);
}

static uint32_t bridge_identity(uint32_t revision)
{
    return (revision << ID_REVISION_SHIFT) |
           (BRIDGE_PART << ID_PART_SHIFT) |
           (BRIDGE_MANUFACTURER << ID_MANUFACTURER_SHIFT) |
           ID_ALWAYS_ONE;
}

/*
 * Runs from the reset vector now rather than a directly loaded kernel, so
 * it sets kernel mode (with KX, for the xkphys forms below) itself instead
 * of inheriting it from a firmware handoff.
 */
static void build_reserved_store_image(SN0Image *p)
{
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);

    /* Widget 8 (the bridge) + 0x19c, a reserved data-half register. */
    sn0_emit_li64(p, SN0_R_T0, UINT64_C(0x9200000000000000) | 0x0800019c);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_T0, SN0_R_ZERO, 0);

    sn0_emit_li64(p, SN0_R_T0,
                  UINT64_C(0x9600000000000000) | GUEST_MARKER_ADDR);
    sn0_emit_li(p, SN0_R_T1, GUEST_MARKER);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_T0, SN0_R_T1, 0);
    sn0_emit_park(p);
}

static void test_reserved_data_half_store_completes(const void *data)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *qts;
    uint32_t marker = 0;
    int64_t deadline;

    build_reserved_store_image(image);
    path = sn0_image_write(image, "sn0-bridge-guest");

    g_assert_true(g_setenv("QTEST_QEMU_ARGS", "-accel tcg", true));
    qts = qtest_initf(MACHINE_WITH_BRIDGE " -smp 1 -bios %s", path);
    g_unsetenv("QTEST_QEMU_ARGS");
    unlink(path);

    /*
     * This case runs the guest, so the wait is on the wall clock: stepping
     * the virtual clock executes no instructions. The deadline was one
     * second, which a busy host can miss for a fragment that has run
     * correctly; five is still far inside the suite's budget because the
     * loop leaves as soon as the marker lands.
     */
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (marker != GUEST_MARKER && g_get_monotonic_time() < deadline) {
        marker = qtest_readl(qts, GUEST_MARKER_ADDR);
        g_usleep(1000);
    }

    g_assert_cmphex(marker, ==, GUEST_MARKER);
    qtest_quit(qts);
}

static void test_identity(const void *data)
{
    uint32_t id;

    bridge_reset();

    id = qtest_readl(qts, REG(BRIDGE_IDENTIFICATION));

    /*
     * The whole word: the right part beside a crossbar's manufacturer is a
     * chip that exists nowhere.
     */
    g_assert_cmphex(id, ==, bridge_identity(BRIDGE_REVISION));

    /*
     * Stated separately because it is the specific wrong answer this chip
     * could plausibly give: a crossbar reports part zero and manufacturer
     * zero, and a widget answering that sends both guests down a different
     * probe path.
     */
    g_assert_false(ID_PART_OF(id) == XBOW_PART &&
                   ID_MANUFACTURER_OF(id) == XBOW_MANUFACTURER);
}

static void test_upper_half_of_the_slot(const void *data)
{
    bridge_reset();

    /*
     * The slot's upper half holds nothing, and it is the address a reader
     * lands on by forgetting the plus-four.
     */
    g_assert_cmphex(qtest_readl(qts, BRIDGE_BASE + BRIDGE_IDENTIFICATION), !=,
                    qtest_readl(qts, REG(BRIDGE_IDENTIFICATION)));
}

static void test_reset_state(const void *data)
{
    bridge_reset();

    /*
     * Reset is far more non-zero than a reader would guess: whole words,
     * so an unnamed field still has to be right.
     */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_CONTROL)), ==, CONTROL_RESET);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_REQUEST_TIMEOUT)), ==,
                    REQUEST_TIMEOUT_RESET);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_LINK_CONFIG)), ==,
                    LINK_CONFIG_RESET);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_ARBITRATION)), ==,
                    ARBITRATION_RESET);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_BUS_TIMEOUT)), ==,
                    BUS_TIMEOUT_RESET);

    /*
     * The add-512MB bit resets set; clear, every direct-window transfer
     * moves half a gigabyte without a word.
     */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DIRECT_MAP)), ==,
                    DIRECT_MAP_RESET);

    /*
     * The strap-driven bits are undefined at reset, so the flash select is
     * this model's choice. Assert it so changes are deliberate.
     */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_STATUS)), ==, STATUS_PCI_MODE);
}

/* The slot an IOC3-class device sits in on a base I/O board. */
#define IOC3_SLOT 2

static void test_each_slot_resets_to_its_own_window(const void *data)
{
    int n;

    bridge_reset();

    /*
     * Three bits are set: memory-or-I/O select is the third. All
     * eight rows, because a hand-written table is where one goes missing.
     * Every slot, including the ones a stock PROM's own PCI enumeration
     * goes on to program: nothing in this model does that enumeration on
     * a guest's behalf, so what a fresh machine shows here is the bare
     * reset value across the board.
     */
    for (n = 0; n < BRIDGE_DEVICES; n++) {
        g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(n))), ==,
                        device_reset[n]);
    }
}

static void test_fields_are_the_width_they_should_be(const void *data)
{
    int n;

    bridge_reset();

    /*
     * All-ones and read back: the retry hold is five bits, not four, or a
     * guest waits a sixteenth of the time it asked for. The whole writable
     * mask, so every reserved bit is asserted at once.
     */
    qtest_writel(qts, REG(BRIDGE_BUS_TIMEOUT), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_BUS_TIMEOUT)), ==,
                    BUS_TIMEOUT_WRITABLE);

    /*
     * A bus number and a device number and nothing else, per the chip's own
     * register table.
     */
    qtest_writel(qts, REG(BRIDGE_PCI_TYPE1_CONFIG), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_PCI_TYPE1_CONFIG)), ==,
                    TYPE1_WRITABLE);

    /*
     * The two status bits are answers recomputed by the slot every write
     * runs; masked out here. The writable mask's only coverage.
     */
    qtest_writel(qts, REG(BRIDGE_NIC), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_NIC)) & ~(NIC_LEVEL | NIC_DONE),
                    ==, NIC_PULSE(0x3ff) | NIC_SAMPLE(0xff));

    /* Bits 31:29 of a device register are reserved and read back clear. */
    for (n = 0; n < BRIDGE_DEVICES; n++) {
        qtest_writel(qts, REG(BRIDGE_DEVICE(n)), 0xffffffff);
        g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(n))), ==,
                        DEVICE_WRITABLE);
    }

    /*
     * The control register's two link-retry-counter strobes are written as
     * ones and always read back zero, which is what makes them strobes
     * rather than storage. Everything else in the register takes the write.
     */
    qtest_writel(qts, REG(BRIDGE_CONTROL), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_CONTROL)), ==,
                    0xffffffffu & ~CONTROL_RETRY_CLEAR);
}

static void test_a_device_window_base_survives(const void *data)
{
    uint32_t written = DEV_ERROR_LOCK | DEV_PAGE_CROSS_CHECK | 0x123u;

    bridge_reset();

    /*
     * The I/O choice is written by clearing a bit reset set, which a model
     * holding a reset value behind a mask would fail.
     */
    qtest_writel(qts, REG(BRIDGE_DEVICE(0)), written);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(0))), ==, written);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(0))) &
                    DEV_MEMORY_SPACE, ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(0))) &
                    DEV_WINDOW_BASE, ==, 0x123u);

    /* An untouched slot is unaffected by the write above. */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(EMPTY_SLOT))), ==,
                    device_reset[EMPTY_SLOT]);
}

static void test_a_read_only_register_stays_put(const void *data)
{
    bridge_reset();

    /*
     * The identity is what a guest decides the shape of the machine from, so
     * a model letting a write land on it would let a guest change what
     * machine it thinks it is on.
     */
    qtest_writel(qts, REG(BRIDGE_IDENTIFICATION), 0xdeadbeef);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_IDENTIFICATION)), ==,
                    bridge_identity(BRIDGE_REVISION));
}

static void test_the_target_flush_answers(const void *data)
{
    bridge_reset();

    /*
     * Reading the flush register is the operation and the value means
     * nothing; the one thing it must never do is fault.
     */
    qtest_readl(qts, REG(BRIDGE_TARGET_FLUSH));
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_IDENTIFICATION)), ==,
                    bridge_identity(BRIDGE_REVISION));
}

static void test_the_default_machine_has_the_bridge(const void *data)
{
    bridge_reset();

    /*
     * The default machine must carry the bridge: every other test names the
     * option, so this is the only one that would notice the default going
     * back and silently lose the bus everything hangs off.
     */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_IDENTIFICATION)), ==,
                    bridge_identity(BRIDGE_REVISION));
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_CONTROL)), ==, CONTROL_RESET);
}

static void test_configuration_space_reaches_a_card(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE PROBE_CARD);

    /*
     * A 32-bit read of a slot's first configuration register returns the
     * identity double-word as a number, which is what both guests read first
     * and decide whether a slot is occupied from.
     */
    g_assert_cmphex(qtest_readl(qts, CFG0(PROBE_SLOT, 0)), ==, PROBE_IDENTITY);

    qtest_quit(qts);
}

static void test_configuration_byte_lanes(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE PROBE_CARD);

    /*
     * Linux aims narrow reads by offset xor (4 - width); the halfword
     * assertions carry the teeth.
     */
    g_assert_cmphex(qtest_readw(qts, CFG0(PROBE_SLOT, 0) + 2), ==, PROBE_VENDOR);
    g_assert_cmphex(qtest_readw(qts, CFG0(PROBE_SLOT, 0) + 0), ==, PROBE_DEVICE);

    g_assert_cmphex(qtest_readb(qts, CFG0(PROBE_SLOT, 0) + 1), ==,
                    (PROBE_DEVICE >> 0) & 0xff);
    g_assert_cmphex(qtest_readb(qts, CFG0(PROBE_SLOT, 0) + 2), ==,
                    (PROBE_VENDOR >> 8) & 0xff);

    qtest_quit(qts);
}

static void test_configuration_space_is_laid_out_right(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE PROBE_CARD
                                 " -device lsi53c895a,addr="
                                 stringify(PROBE_SLOT_ABOVE));

    /* A device's configuration space is 4 KB above the one below it. */
    g_assert_cmphex(qtest_readl(qts, CFG0(PROBE_SLOT_ABOVE, 0)), ==,
                    PROBE_IDENTITY);

    /*
     * This card has one function, so function 1 must be empty. A model
     * ignoring the field aliases all eight onto function 0 and no test
     * reading only function 0 could tell.
     */
    g_assert_cmphex(qtest_readl(qts, CFG0(PROBE_SLOT, 1)), ==, NO_CARD);

    /* A slot with no card in it answers all ones, and specifically not
     * zero. */
    g_assert_cmphex(qtest_readl(qts, CFG0(DMA_SLOT, 0)), ==, NO_CARD);
    g_assert_cmphex(qtest_readl(qts, CFG0(EMPTY_SLOT, 0)), ==, NO_CARD);

    qtest_quit(qts);
}

/*
 * Option cards go in devices 5, 6 and 7 (0-2 base I/O, 3 its dual-slot guest,
 * 4 no connector). Each decodes a fitted card. Fit one: only its slot
 * answers, and the others read all-ones. This catches a decode that skipped
 * device numbers or aliased slots, which a single-slot check would miss.
 */
static void test_each_option_slot_decodes_a_card(const void *data)
{
    static const int option_slot[] = { 5, 6, 7 };
    unsigned int fitted, other;

    for (fitted = 0; fitted < ARRAY_SIZE(option_slot); fitted++) {
        g_autofree char *args =
            g_strdup_printf(MACHINE_WITH_BRIDGE " -device lsi53c895a,addr=%d",
                            option_slot[fitted]);
        QTestState *qts = qtest_init(args);

        g_assert_cmphex(qtest_readl(qts, CFG0(option_slot[fitted], 0)), ==,
                        PROBE_IDENTITY);

        for (other = 0; other < ARRAY_SIZE(option_slot); other++) {
            if (other != fitted) {
                g_assert_cmphex(qtest_readl(qts, CFG0(option_slot[other], 0)),
                                ==, NO_CARD);
            }
        }

        qtest_quit(qts);
    }
}

/*
 * A window reaches whatever decodes the address its register supplies.
 * Windows 3 and 7 sit at 0x700000 and 0xb00000 only if the sizes below
 * them are right, so these five rows pin the whole table.
 */
static const struct {
    int window;
    int card_slot;
} window_case[] = {
    { 0, 3 },               /* a 2 MB window, reached from three slots away */
    { 1, PROBE_SLOT },      /* the other 2 MB window */
    { 2, PROBE_SLOT_ABOVE },
    { 3, DMA_SLOT },
    { 7, EMPTY_SLOT },
};
#define WINDOW_CASES            ARRAY_SIZE(window_case)

static void bridge_put_card_in_slot(QTestState *qts, int slot)
{
    qtest_writel(qts, CFG0(slot, 0) + PCI_BAR1, CARD_MMIO(slot));
    qtest_writel(qts, CFG0(slot, 0) + PCI_COMMAND, PCI_COMMAND_MEMORY);
}

static void bridge_aim_window_at_card(QTestState *qts, int window, int slot)
{
    qtest_writel(qts, REG(BRIDGE_DEVICE(window)),
                 DEV_MEMORY_SPACE | (CARD_MMIO(slot) >> 20));
}

static void bridge_aim_window_at_its_card(QTestState *qts, int slot)
{
    bridge_put_card_in_slot(qts, slot);
    bridge_aim_window_at_card(qts, slot, slot);
}

static void test_device_windows_reach_their_cards(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -device lsi53c895a,addr=3"
                                 " -device lsi53c895a,addr="
                                 stringify(PROBE_SLOT)
                                 " -device lsi53c895a,addr="
                                 stringify(PROBE_SLOT_ABOVE)
                                 " -device lsi53c895a,addr="
                                 stringify(DMA_SLOT)
                                 " -device lsi53c895a,addr="
                                 stringify(EMPTY_SLOT));
    unsigned int i;

    for (i = 0; i < WINDOW_CASES; i++) {
        bridge_put_card_in_slot(qts, window_case[i].card_slot);
        bridge_aim_window_at_card(qts, window_case[i].window,
                                  window_case[i].card_slot);
    }

    /*
     * Every window written before any is read: overlapping windows pass a
     * write-then-read-immediately check and fail here.
     */
    for (i = 0; i < WINDOW_CASES; i++) {
        qtest_writel(qts, WINDOW(window_case[i].window) + CARD_SCRATCH,
                     0xc0ffee00 + window_case[i].window);
    }
    for (i = 0; i < WINDOW_CASES; i++) {
        g_assert_cmphex(qtest_readl(qts,
                                    WINDOW(window_case[i].window) +
                                    CARD_SCRATCH),
                        ==, 0xc0ffee00u + window_case[i].window);
    }

    qtest_quit(qts);
}

static void test_a_window_goes_where_its_base_says(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE PROBE_CARD);

    bridge_aim_window_at_its_card(qts, PROBE_SLOT);
    qtest_writel(qts, WINDOW(PROBE_SLOT) + CARD_SCRATCH, 0xc0ffee00);
    g_assert_cmphex(qtest_readl(qts, WINDOW(PROBE_SLOT) + CARD_SCRATCH), ==, 0xc0ffee00);

    /*
     * A megabyte up and the card is out from under it: a model ignoring the
     * base passes everything above.
     */
    qtest_writel(qts, REG(BRIDGE_DEVICE(PROBE_SLOT)),
                 DEV_MEMORY_SPACE | ((CARD_MMIO(PROBE_SLOT) >> 20) + 1));
    g_assert_cmphex(qtest_readl(qts, WINDOW(PROBE_SLOT) + CARD_SCRATCH), !=, 0xc0ffee00);

    /*
     * Selecting I/O space takes the memory-space card out too, which a
     * model storing the bit would miss.
     */
    qtest_writel(qts, REG(BRIDGE_DEVICE(PROBE_SLOT)), CARD_MMIO(PROBE_SLOT) >> 20);
    g_assert_cmphex(qtest_readl(qts, WINDOW(PROBE_SLOT) + CARD_SCRATCH), !=, 0xc0ffee00);

    /* Put it back: the card is there again, so the window moved, not it. */
    qtest_writel(qts, REG(BRIDGE_DEVICE(PROBE_SLOT)),
                 DEV_MEMORY_SPACE | (CARD_MMIO(PROBE_SLOT) >> 20));
    g_assert_cmphex(qtest_readl(qts, WINDOW(PROBE_SLOT) + CARD_SCRATCH), ==, 0xc0ffee00);

    qtest_quit(qts);
}

/*
 * The address translation table and its two views, specified independently of
 * the model's computation.
 */
#define ATE_BASE                0x010000
#define ATE_ENTRIES             128
#define ATE(n)                  (BRIDGE_BASE + ATE_BASE + (n) * 8)
#define ATE_UPPER(n)            (ATE(n) + 4)
#define ATE_LOW_BASE            0x011000
#define ATE_LOW(n)              (BRIDGE_BASE + ATE_LOW_BASE + (n) * 4)

static void test_translation_entries_are_written_whole(const void *data)
{
    static const int entries[] = { 0, 1, 63, 127 };
    unsigned int i;

    bridge_reset();

    /*
     * Writes whole, reads back in halves from two places; all four written
     * before any is read, or an index-ignoring model passes.
     */
    for (i = 0; i < ARRAY_SIZE(entries); i++) {
        qtest_writeq(qts, ATE(entries[i]),
                     (0xa5a50000ull << 32) | (0x5a5a0000ull + entries[i]));
    }
    for (i = 0; i < ARRAY_SIZE(entries); i++) {
        g_assert_cmphex(qtest_readl(qts, ATE_UPPER(entries[i])), ==,
                        0xa5a50000u);
        g_assert_cmphex(qtest_readl(qts, ATE_LOW(entries[i])), ==,
                        0x5a5a0000u + entries[i]);
    }
}

static void test_the_lower_half_view_is_read_only(const void *data)
{
    bridge_reset();

    qtest_writeq(qts, ATE(0), 0x1234567800abcdefull);
    g_assert_cmphex(qtest_readl(qts, ATE_LOW(0)), ==, 0x00abcdef);

    /*
     * A write to the read-only view must not reach the entry, or a driver
     * believes it changed one through the address that cannot.
     */
    qtest_writel(qts, ATE_LOW(0), 0xdeadbeef);
    g_assert_cmphex(qtest_readl(qts, ATE_LOW(0)), ==, 0x00abcdef);
    g_assert_cmphex(qtest_readl(qts, ATE_UPPER(0)), ==, 0x12345678);
}

static void test_translation_entries_reset_clear(const void *data)
{
    unsigned int n;

    bridge_reset();

    /*
     * The whole entry, not the valid bit: a stale page address behind a
     * clear valid bit is a working translation one write away.
     */
    for (n = 0; n < ATE_ENTRIES; n++) {
        g_assert_cmphex(qtest_readl(qts, ATE_UPPER(n)), ==, 0);
        g_assert_cmphex(qtest_readl(qts, ATE_LOW(n)), ==, 0);
    }
}

/*
 * The transfer source: nothing in the tree could issue one transfer of a
 * chosen length and address without claiming MSI or another chip's engine.
 */
#define DMA_CARD                " -device sgi-bridge-dma-test,addr=" \
                                stringify(DMA_SLOT)
#define DMA_REG_ADDRESS         0x00
#define DMA_REG_LENGTH          0x04
#define DMA_REG_SEED            0x08
#define DMA_REG_GO              0x0c
#define DMA_REG_SOURCE          0x14
#define DMA_REG_ADDRESS_HI      0x18   /* high 32 bits of the destination */
#define DMA_REG_SOURCE_HI       0x1c   /* high 32 bits of the copy source */

/*
 * The two-node machine: a node-1 target needs the 64-bit decode, because
 * the 31-bit path keeps bits 0-30 and drops the NASID at bit 32,
 * collapsing node 1 into node 0's RAM.
 */
#define MACHINE_TWO_NODE \
    "-M origin200,topology=origin200-dual -m 128M " \
    "-smp 4"

#define NODE1_PCI_ADDR          0xa000000100bc4000ULL /* widget 0xa, xtalk
 * 0x100bc4000 */
#define NODE1_SYS_ADDR          0x100bc4000ULL        /* node 1 RAM: NASID 1
 * << 32 + off */
#define NODE0_TRUNCATED_ADDR    0x00bc4000ULL         /* where dropping bit 32
 * lands it */
#define DMA64_SEED              0x51
#define DMA64_LEN               16

/* The read direction: a copy's 64-bit source in node 1, its destination in
 * node 0. */
#define COPY_SRC_PCI            0xa000000100c00000ULL /* widget 0xa, xtalk
 * 0x100c00000 */
#define COPY_SRC_SYS            0x100c00000ULL        /* node 1 RAM holding
 * the source */
#define COPY_DST_ADDR           0x00300000u           /* a node-0 local
 * destination */

/*
 * A 64-bit cycle must bypass the offset registers, including the
 * reset-active 512 MB step, where the 31-bit path would add them.
 */
#define DMA64_NODE0_PCI         0xa000000000fbc000ULL /* widget 0xa, xtalk
 * 0xfbc000, NASID 0 */
#define DMA64_NODE0_SYS         0x00fbc000ULL         /* node 0 guest-phys,
 * where it must land */

/* Where the transfer source's own registers are put in PCI memory space. */
#define DMA_CARD_MMIO           CARD_MMIO(DMA_SLOT)

/*
 * The transfer source's second BAR, the 64-bit register window, put one
 * megabyte above its first so the device-size window base (a 1 MB-granular
 * field) names it cleanly. WIDE_FIXED is the read-only register's offset, and
 * the two goldens are its constant read whole and read byte-reversed.
 */
#define WIDE_CARD_MMIO          (DMA_CARD_MMIO + 0x100000u)
#define WIDE_FIXED              0x00
#define WIDE_SCRATCH            0x08
#define WIDE_ROUNDTRIP_VALUE    0xdeadbeefcafef00dULL
#define WIDE_GOLDEN_UNCHANGED   0x0123456789abcdefULL
#define WIDE_GOLDEN_MIRRORED    0xefcdab8967452301ULL

/*
 * The translated window, and the two pages of it this test uses. A page is 4
 * KB because the control register resets choosing that size.
 */
#define PCI_TRANSLATED          0x40000000u
#define PAGE_SIZE_4K            0x1000u

/*
 * The two pages translate to non-adjacent places. This is the point of the
 * test: a translate-once-per-transfer model is right whenever the pages
 * happen to be consecutive, so consecutive pages cannot tell it apart.
 */
#define TARGET_PAGE_A           0x00100000u
#define TARGET_PAGE_B           0x00200000u
#define TARGET_PAGE_AFTER_A     (TARGET_PAGE_A + PAGE_SIZE_4K)

/* An entry naming a page, targeting a widget, and marked valid. */
#define ATE_ENTRY(page, widget) ((uint64_t)(page) | \
                                 ((uint64_t)(widget) << 8) | 1)

/* How far into the first page the transfer starts, and how long it runs. */
#define STRADDLE_HEAD           8
#define STRADDLE_TAIL           8
#define STRADDLE_LENGTH         (STRADDLE_HEAD + STRADDLE_TAIL)
#define STRADDLE_START          (PCI_TRANSLATED + PAGE_SIZE_4K - STRADDLE_HEAD)
#define STRADDLE_SEED           0x40

static void bridge_start_the_transfer_source(QTestState *qts)
{
    /* Give the source its registers, and let it master the bus. */
    qtest_writel(qts, CFG0(DMA_SLOT, 0) + 0x10, DMA_CARD_MMIO);
    qtest_writel(qts, CFG0(DMA_SLOT, 0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | 0x0004);
    qtest_writel(qts, REG(BRIDGE_DEVICE(DMA_SLOT)),
                 DEV_MEMORY_SPACE | (DMA_CARD_MMIO >> 20));
}

static void bridge_transfer(QTestState *qts, uint32_t addr, uint32_t length,
                            uint32_t seed)
{
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_ADDRESS, addr);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_LENGTH, length);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_SEED, seed);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_GO, 1);
}

static void test_a_transfer_straddling_a_page(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE DMA_CARD);
    uint8_t head[STRADDLE_HEAD], tail[STRADDLE_TAIL];
    uint8_t after[STRADDLE_TAIL];
    unsigned int i;

    bridge_start_the_transfer_source(qts);

    /*
     * Two entries differing only in the page named, targeting one widget so
     * the assertion survives however the fabric composes addresses.
     */
    qtest_writeq(qts, ATE(0), ATE_ENTRY(TARGET_PAGE_A, 0));
    qtest_writeq(qts, ATE(1), ATE_ENTRY(TARGET_PAGE_B, 0));

    /* Clear all three places, so a byte found later was put there. */
    qtest_memset(qts, TARGET_PAGE_A, 0, PAGE_SIZE_4K);
    qtest_memset(qts, TARGET_PAGE_B, 0, PAGE_SIZE_4K);
    qtest_memset(qts, TARGET_PAGE_AFTER_A, 0, PAGE_SIZE_4K);

    /* One transfer, starting before the boundary and crossing it. */
    bridge_transfer(qts, STRADDLE_START, STRADDLE_LENGTH, STRADDLE_SEED);

    qtest_memread(qts, TARGET_PAGE_A + PAGE_SIZE_4K - STRADDLE_HEAD,
                  head, sizeof(head));
    qtest_memread(qts, TARGET_PAGE_B, tail, sizeof(tail));
    qtest_memread(qts, TARGET_PAGE_AFTER_A, after, sizeof(after));

    /*
     * The head lands in the page the first entry names. The exclusive-or
     * is the byte-lane permutation, not the subject: it cannot move a byte
     * out of its four-byte word, so it cannot cross the page boundary.
     */
    for (i = 0; i < STRADDLE_HEAD; i++) {
        g_assert_cmpuint(head[i], ==, (uint8_t)(STRADDLE_SEED + (i ^ 3)));
    }

    /*
     * And the tail is in the page the SECOND entry names, which is the whole
     * assertion. A model that translated once for the transfer would have
     * carried on into the page after the first one.
     */
    for (i = 0; i < STRADDLE_TAIL; i++) {
        g_assert_cmpuint(tail[i], ==,
                         (uint8_t)(STRADDLE_SEED + STRADDLE_HEAD + (i ^ 3)));
    }

    /*
     * Stated as its own assertion rather than left implied, because it names
     * the exact wrong answer: the page physically after the first one is
     * where a translate-once model puts the tail, and it must be untouched.
     */
    for (i = 0; i < STRADDLE_TAIL; i++) {
        g_assert_cmpuint(after[i], ==, 0);
    }

    qtest_quit(qts);
}

static void test_an_invalid_entry_transfers_nothing(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE DMA_CARD);
    uint8_t seen[8];
    unsigned int i;

    bridge_start_the_transfer_source(qts);

    /* The same entry as the straddle test's first, without its valid bit. */
    qtest_writeq(qts, ATE(0), ATE_ENTRY(TARGET_PAGE_A, 0) & ~1ull);
    qtest_memset(qts, TARGET_PAGE_A, 0, PAGE_SIZE_4K);

    bridge_transfer(qts, PCI_TRANSLATED, sizeof(seen), STRADDLE_SEED);

    qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
    for (i = 0; i < sizeof(seen); i++) {
        g_assert_cmpuint(seen[i], ==, 0);
    }

    /*
     * The same transfer lands once the entry is valid; the exclusive-or is
     * the incidental byte-lane permutation, not the assertion.
     */
    qtest_writeq(qts, ATE(0), ATE_ENTRY(TARGET_PAGE_A, 0));
    bridge_transfer(qts, PCI_TRANSLATED, sizeof(seen), STRADDLE_SEED);
    qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
    for (i = 0; i < sizeof(seen); i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(STRADDLE_SEED + (i ^ 3)));
    }

    qtest_quit(qts);
}


/*
 * Patterns are eight-plus bytes and never palindromic, so whole-buffer,
 * group and per-word reversals all differ; memory is read raw because the
 * value accessors would manufacture the difference under test. Unchanged
 * assertions run over sentinel fill, since unchanged is also no-transfer.
 */
#define DEV_SWAP_TRANSLATED     (1u << 20)
#define DEV_SWAP_DIRECT         (1u << 19)
#define SWAP_SEED               0x40
#define SWAP_LEN                12
#define SWAP_SENTINEL           0xee

/* The transfer source's window register, with whatever swap bits are
 * wanted. */
static void bridge_source_with_swap_at(QTestState *qts, unsigned int slot,
                                       uint32_t swap_bits)
{
    qtest_writel(qts, REG(BRIDGE_DEVICE(slot)),
                 DEV_MEMORY_SPACE | (CARD_MMIO(slot) >> 20) | swap_bits);
}

static void bridge_source_with_swap(QTestState *qts, uint32_t swap_bits)
{
    bridge_source_with_swap_at(qts, DMA_SLOT, swap_bits);
}

static void bridge_fill_sentinel(QTestState *qts, uint32_t page)
{
    uint8_t fill[PAGE_SIZE_4K];

    memset(fill, SWAP_SENTINEL, sizeof(fill));
    qtest_memwrite(qts, page, fill, sizeof(fill));
}

/*
 * A transfer through the page-mapped window, with that window's swap bit
 * clear, arrives with the bytes of each aligned word reversed and nothing
 * outside the transfer touched.
 */
static void test_dma_page_mapped_swap_clear(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE DMA_CARD);
    uint8_t seen[SWAP_LEN + 4];
    unsigned int i;

    bridge_start_the_transfer_source(qts);
    bridge_source_with_swap(qts, 0);
    qtest_writeq(qts, ATE(0), ATE_ENTRY(TARGET_PAGE_A, 0));
    bridge_fill_sentinel(qts, TARGET_PAGE_A);

    bridge_transfer(qts, PCI_TRANSLATED, SWAP_LEN, SWAP_SEED);

    qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
    for (i = 0; i < SWAP_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(SWAP_SEED + (i ^ 3)));
    }
    /* And the permutation stayed inside the transfer's own words. */
    for (i = SWAP_LEN; i < sizeof(seen); i++) {
        g_assert_cmpuint(seen[i], ==, SWAP_SENTINEL);
    }

    qtest_quit(qts);
}

/*
 * The same transfer with that window's bit SET arrives untouched. Paired with
 * the test above this is what pins the direction of the rule: one of the two
 * has to be a plain byte stream, and it is this one.
 */
static void test_dma_page_mapped_swap_set(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE DMA_CARD);
    uint8_t seen[SWAP_LEN + 4];
    unsigned int i;

    bridge_start_the_transfer_source(qts);
    bridge_source_with_swap(qts, DEV_SWAP_TRANSLATED);
    qtest_writeq(qts, ATE(0), ATE_ENTRY(TARGET_PAGE_A, 0));
    bridge_fill_sentinel(qts, TARGET_PAGE_A);

    bridge_transfer(qts, PCI_TRANSLATED, SWAP_LEN, SWAP_SEED);

    qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
    for (i = 0; i < SWAP_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(SWAP_SEED + i));
    }
    for (i = SWAP_LEN; i < sizeof(seen); i++) {
        g_assert_cmpuint(seen[i], ==, SWAP_SENTINEL);
    }

    qtest_quit(qts);
}

/*
 * The page-mapped window follows its OWN bit. Setting the direct-mapped bit
 * instead leaves a page-mapped transfer permuted, which is the assertion that
 * fails against a model reading one bit for both windows.
 */
static void test_dma_page_mapped_ignores_the_direct_bit(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE DMA_CARD);
    uint8_t seen[SWAP_LEN];
    unsigned int i;

    bridge_start_the_transfer_source(qts);
    bridge_source_with_swap(qts, DEV_SWAP_DIRECT);
    qtest_writeq(qts, ATE(0), ATE_ENTRY(TARGET_PAGE_A, 0));
    bridge_fill_sentinel(qts, TARGET_PAGE_A);

    bridge_transfer(qts, PCI_TRANSLATED, SWAP_LEN, SWAP_SEED);

    qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
    for (i = 0; i < SWAP_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(SWAP_SEED + (i ^ 3)));
    }

    qtest_quit(qts);
}

/*
 * An unaligned length: the tail lands at the top of its word and the
 * vacated offsets keep the sentinel. Seven is the length that reaches the
 * one-byte arm; six shows why.
 */
static void test_dma_trailing_partial_word(const void *data)
{
    static const unsigned int lengths[] = { 6, 7 };
    unsigned int n;

    for (n = 0; n < ARRAY_SIZE(lengths); n++) {
        QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE DMA_CARD);
        unsigned int len = lengths[n];
        uint8_t seen[8];
        unsigned int i;

        bridge_start_the_transfer_source(qts);
        bridge_source_with_swap(qts, 0);
        qtest_writeq(qts, ATE(0), ATE_ENTRY(TARGET_PAGE_A, 0));
        bridge_fill_sentinel(qts, TARGET_PAGE_A);

        bridge_transfer(qts, PCI_TRANSLATED, len, SWAP_SEED);

        qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
        for (i = 0; i < sizeof(seen); i++) {
            unsigned int source = i ^ 3;

            if (source < len) {
                g_assert_cmpuint(seen[i], ==, (uint8_t)(SWAP_SEED + source));
            } else {
                /*
                 * Nothing was written here: the byte a model truncating its
                 * tail instead of relocating it would have written.
                 */
                g_assert_cmpuint(seen[i], ==, SWAP_SENTINEL);
            }
        }

        qtest_quit(qts);
    }
}

/*
 * Off a word boundary: the rule keys on each byte's own address, which a
 * model anchoring to the buffer start gets wrong for every byte.
 */
static void test_dma_unaligned_start(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE DMA_CARD);
    const unsigned int start = 1, len = 5;
    uint8_t seen[8];
    unsigned int i;

    bridge_start_the_transfer_source(qts);
    bridge_source_with_swap(qts, 0);
    qtest_writeq(qts, ATE(0), ATE_ENTRY(TARGET_PAGE_A, 0));
    bridge_fill_sentinel(qts, TARGET_PAGE_A);

    bridge_transfer(qts, PCI_TRANSLATED + start, len, SWAP_SEED);

    qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
    for (i = 0; i < sizeof(seen); i++) {
        unsigned int pci = i ^ 3;

        if (pci >= start && pci < start + len) {
            g_assert_cmpuint(seen[i], ==, (uint8_t)(SWAP_SEED + pci - start));
        } else {
            g_assert_cmpuint(seen[i], ==, SWAP_SENTINEL);
        }
    }

    qtest_quit(qts);
}

/*
 * Both swap bits read back clear after reset, which is what makes every test
 * above that does not set them a test of the cleared case rather than of an
 * unknown one.
 */
static void test_dma_swap_bits_reset_clear(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE DMA_CARD);
    unsigned int slot;

    for (slot = 0; slot < BRIDGE_DEVICES; slot++) {
        uint32_t dev = qtest_readl(qts, REG(BRIDGE_DEVICE(slot)));

        g_assert_cmphex(dev & DEV_SWAP_DIRECT, ==, 0);
        g_assert_cmphex(dev & DEV_SWAP_TRANSLATED, ==, 0);
    }

    qtest_quit(qts);
}


/*
 * The property that forces a slot exempt whatever is plugged into it, kept
 * as a test and debug override of what the card itself says. The non-exempt
 * twin is what makes bytes-identical mean something.
 */
#define MASQ_SLOT               7
#define MASQ_CARD               " -device sgi-bridge-dma-test,addr=" \
                                stringify(MASQ_SLOT)
/* Exempt slot 7, bit 7, from the Bridge's byte-lane correction. */
#define MASQ_EXEMPT             " -global sgi-bridge.byte-lane-exempt=0x80"

/* Bus mastering too: these cards initiate transfers as well as answer
 * them. */
static void card_start(QTestState *qts, unsigned int slot)
{
    qtest_writel(qts, CFG0(slot, 0) + 0x10, CARD_MMIO(slot));
    qtest_writel(qts, CFG0(slot, 0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | 0x0004);
    qtest_writel(qts, REG(BRIDGE_DEVICE(slot)),
                 DEV_MEMORY_SPACE | (CARD_MMIO(slot) >> 20));
}

static void card_transfer(QTestState *qts, unsigned int slot, uint32_t addr,
                          uint32_t len)
{
    qtest_writel(qts, WINDOW(slot) + DMA_REG_ADDRESS, addr);
    qtest_writel(qts, WINDOW(slot) + DMA_REG_LENGTH, len);
    qtest_writel(qts, WINDOW(slot) + DMA_REG_SEED, SWAP_SEED);
    qtest_writel(qts, WINDOW(slot) + DMA_REG_GO, 1);
}

static void masq_start(QTestState *qts)
{
    card_start(qts, MASQ_SLOT);
}

static void masq_transfer(QTestState *qts, uint32_t addr, uint32_t len)
{
    card_transfer(qts, MASQ_SLOT, addr, len);
}

static void test_dma_exempt_slot_is_never_swapped(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE MASQ_CARD MASQ_EXEMPT);
    uint8_t seen[SWAP_LEN];
    unsigned int i;
    uint32_t dev;

    masq_start(qts);
    qtest_writeq(qts, ATE(0), ATE_ENTRY(TARGET_PAGE_A, 0));
    bridge_fill_sentinel(qts, TARGET_PAGE_A);

    /* With the bit clear, where a non-exempt device would be permuted. */
    masq_transfer(qts, PCI_TRANSLATED, SWAP_LEN);
    qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
    for (i = 0; i < SWAP_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(SWAP_SEED + i));
    }

    /*
     * And with it set. Read the register back first: without that, a model
     * that silently dropped the write would reach the same bytes by never
     * having changed anything, and this half would pass for the wrong reason.
     */
    bridge_source_with_swap_at(qts, MASQ_SLOT, DEV_SWAP_TRANSLATED);
    dev = qtest_readl(qts, REG(BRIDGE_DEVICE(MASQ_SLOT)));
    g_assert_cmphex(dev & DEV_SWAP_TRANSLATED, ==, DEV_SWAP_TRANSLATED);

    bridge_fill_sentinel(qts, TARGET_PAGE_A);
    masq_transfer(qts, PCI_TRANSLATED, SWAP_LEN);
    qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
    for (i = 0; i < SWAP_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(SWAP_SEED + i));
    }

    qtest_quit(qts);
}

/*
 * Same device and transfer with exemption dropped: permuted, so the
 * exemption above came from the property, not an unexercised correction.
 */
static void test_dma_unexempt_slot_is_swapped(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE MASQ_CARD);
    uint8_t seen[SWAP_LEN];
    unsigned int i;

    masq_start(qts);
    qtest_writeq(qts, ATE(0), ATE_ENTRY(TARGET_PAGE_A, 0));
    bridge_fill_sentinel(qts, TARGET_PAGE_A);

    masq_transfer(qts, PCI_TRANSLATED, SWAP_LEN);

    qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
    for (i = 0; i < SWAP_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(SWAP_SEED + (i ^ 3)));
    }

    qtest_quit(qts);
}


/*
 * The unreproduced configuration logs once, on the write; the two
 * negatives are the test, or a log-on-every-write model passes.
 */
#define UNIMP_FRAGMENT  "exempts from the DMA byte swap"

static bool bridge_log_has_the_message_at(const char *extra_device,
                                          unsigned int slot,
                                          uint32_t swap_bits)
{
    g_autofree char *path = g_strdup_printf("%s/sn0-bridge-swap-XXXXXX.log",
                                            g_get_tmp_dir());
    g_autofree char *found = NULL;
    QTestState *qts;
    bool seen;
    int fd = g_mkstemp(path);

    g_assert_cmpint(fd, >=, 0);
    close(fd);

    qts = qtest_initf("%s%s -d unimp -D %s", MACHINE_WITH_BRIDGE,
                      extra_device, path);
    qtest_writel(qts, REG(BRIDGE_DEVICE(slot)),
                 DEV_MEMORY_SPACE | (CARD_MMIO(slot) >> 20) | swap_bits);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(path, &found, NULL, NULL));
    seen = strstr(found, UNIMP_FRAGMENT) != NULL;
    g_assert_cmpint(g_unlink(path), ==, 0);
    return seen;
}

static bool bridge_log_has_the_message(const char *extra_device,
                                       uint32_t swap_bits)
{
    return bridge_log_has_the_message_at(extra_device, MASQ_SLOT, swap_bits);
}

static void test_dma_swap_on_an_exempt_slot_is_reported(const void *data)
{
    /* The configuration itself. */
    g_assert_true(bridge_log_has_the_message(MASQ_CARD MASQ_EXEMPT,
                                             DEV_SWAP_TRANSLATED));
    g_assert_true(bridge_log_has_the_message(MASQ_CARD MASQ_EXEMPT,
                                             DEV_SWAP_DIRECT));

    /*
     * Same bits, slot, and register write. The model corrects this device.
     * Nothing to report, because this one works.
     */
    g_assert_false(bridge_log_has_the_message(MASQ_CARD,
                                              DEV_SWAP_TRANSLATED));
    g_assert_false(bridge_log_has_the_message(MASQ_CARD,
                                              DEV_SWAP_DIRECT));

    /*
     * And the exempt device again with the swap bits clear, which is what
     * every guest actually does. A model keying on the write rather than on
     * what the write asked for would fail here.
     */
    g_assert_false(bridge_log_has_the_message(MASQ_CARD MASQ_EXEMPT, 0));
}


/*
 * The direct window needs the direct-map register cleared: the add-512MB
 * bit resets set and steps every transfer half a gigabyte down.
 */
#define PCI_DIRECT_32           0x80000000u

static void bridge_clear_the_direct_map(QTestState *qts)
{
    qtest_writel(qts, REG(BRIDGE_DIRECT_MAP), 0);
}

/*
 * Direct window, its bit clear: permuted, exactly as the page-mapped window
 * is with ITS bit clear. This is the window the firmware's own block-RAM
 * download goes through, so it is the branch that carries the boot.
 */
static void test_dma_direct_window_swap_clear(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE DMA_CARD);
    uint8_t seen[SWAP_LEN + 4];
    unsigned int i;

    bridge_start_the_transfer_source(qts);
    bridge_source_with_swap(qts, 0);
    bridge_clear_the_direct_map(qts);
    bridge_fill_sentinel(qts, TARGET_PAGE_A);

    bridge_transfer(qts, PCI_DIRECT_32 | TARGET_PAGE_A, SWAP_LEN, SWAP_SEED);

    qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
    for (i = 0; i < SWAP_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(SWAP_SEED + (i ^ 3)));
    }
    for (i = SWAP_LEN; i < sizeof(seen); i++) {
        g_assert_cmpuint(seen[i], ==, SWAP_SENTINEL);
    }

    qtest_quit(qts);
}

/* And with its bit set, untouched. */
static void test_dma_direct_window_swap_set(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE DMA_CARD);
    uint8_t seen[SWAP_LEN];
    unsigned int i;

    bridge_start_the_transfer_source(qts);
    bridge_source_with_swap(qts, DEV_SWAP_DIRECT);
    bridge_clear_the_direct_map(qts);
    bridge_fill_sentinel(qts, TARGET_PAGE_A);

    bridge_transfer(qts, PCI_DIRECT_32 | TARGET_PAGE_A, SWAP_LEN, SWAP_SEED);

    qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
    for (i = 0; i < SWAP_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(SWAP_SEED + i));
    }

    qtest_quit(qts);
}

/*
 * One register selects opposite byte orders for the two windows. A single
 * swap bit cannot model both; boot paths do not use the page-mapped window.
 */
static void test_dma_the_two_swap_bits_are_independent(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE DMA_CARD);
    uint8_t seen[SWAP_LEN];
    unsigned int i;

    bridge_start_the_transfer_source(qts);
    /* Direct swapping on, page-mapped swapping off, in one register. */
    bridge_source_with_swap(qts, DEV_SWAP_DIRECT);
    bridge_clear_the_direct_map(qts);
    qtest_writeq(qts, ATE(0), ATE_ENTRY(TARGET_PAGE_B, 0));

    bridge_fill_sentinel(qts, TARGET_PAGE_A);
    bridge_fill_sentinel(qts, TARGET_PAGE_B);

    bridge_transfer(qts, PCI_DIRECT_32 | TARGET_PAGE_A, SWAP_LEN, SWAP_SEED);
    bridge_transfer(qts, PCI_TRANSLATED, SWAP_LEN, SWAP_SEED);

    /* Through the direct window, with that bit set: a plain byte stream. */
    qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
    for (i = 0; i < SWAP_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(SWAP_SEED + i));
    }

    /* Through the page-mapped window, whose own bit is clear: permuted. */
    qtest_memread(qts, TARGET_PAGE_B, seen, sizeof(seen));
    for (i = 0; i < SWAP_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(SWAP_SEED + (i ^ 3)));
    }

    qtest_quit(qts);
}

/*
 * Local space is never permuted; no swapper is in its path. Both
 * settings run, or this is indistinguishable from a forgotten window.
 */
static void test_dma_local_space_is_never_swapped(const void *data)
{
    static const uint32_t settings[] = {
        0, DEV_SWAP_DIRECT | DEV_SWAP_TRANSLATED,
    };
    unsigned int n;

    for (n = 0; n < ARRAY_SIZE(settings); n++) {
        QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE DMA_CARD);
        uint8_t seen[SWAP_LEN];
        unsigned int i;

        bridge_start_the_transfer_source(qts);
        bridge_source_with_swap(qts, settings[n]);
        bridge_fill_sentinel(qts, TARGET_PAGE_A);

        /* Below the translated window, so the bridge maps nothing. */
        bridge_transfer(qts, TARGET_PAGE_A, SWAP_LEN, SWAP_SEED);

        qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
        for (i = 0; i < SWAP_LEN; i++) {
            g_assert_cmpuint(seen[i], ==, (uint8_t)(SWAP_SEED + i));
        }

        qtest_quit(qts);
    }
}


/*
 * The exemption belongs to the card, not to the slot. Two cards of the same
 * type in two slots the board's own controller never occupies: one declares
 * that it orders this chip's byte lanes itself and one says nothing. Holding
 * both in one machine is what makes the answer about the declaration rather
 * than about a slot a board happened to name.
 */
#define SELF_SLOT               4
#define PLAIN_SLOT              5
#define SELF_CARD               " -device sgi-bridge-dma-test,addr=" \
                                stringify(SELF_SLOT) \
                                ",byte-lane-self-ordered=on"
#define PLAIN_CARD              " -device sgi-bridge-dma-test,addr=" \
                                stringify(PLAIN_SLOT)
#define BOTH_CARDS              SELF_CARD PLAIN_CARD

/*
 * The direct window, which is where an ethernet engine's descriptors and
 * buffers actually travel. It has to be this window or the translated one:
 * a transfer below the translated base crosses no swapper at all, so a ring
 * put there would pass on a model that never read the declaration.
 */
static void test_dma_a_self_ordered_card_is_never_swapped(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE BOTH_CARDS);
    uint8_t seen[SWAP_LEN];
    unsigned int i;

    card_start(qts, SELF_SLOT);
    card_start(qts, PLAIN_SLOT);
    bridge_clear_the_direct_map(qts);

    /* The card that orders its own bytes: the guest's stream, as written. */
    bridge_fill_sentinel(qts, TARGET_PAGE_A);
    card_transfer(qts, SELF_SLOT, PCI_DIRECT_32 | TARGET_PAGE_A, SWAP_LEN);
    qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
    for (i = 0; i < SWAP_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(SWAP_SEED + i));
    }

    /*
     * And with the guest asking for the swap the model does not reproduce
     * for such a card. The declaration is the card's, so the guest cannot
     * reach past it, and a model keyed on the register would differ here.
     */
    qtest_writel(qts, REG(BRIDGE_DEVICE(SELF_SLOT)),
                 DEV_MEMORY_SPACE | (CARD_MMIO(SELF_SLOT) >> 20) |
                 DEV_SWAP_DIRECT);
    bridge_fill_sentinel(qts, TARGET_PAGE_A);
    card_transfer(qts, SELF_SLOT, PCI_DIRECT_32 | TARGET_PAGE_A, SWAP_LEN);
    qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
    for (i = 0; i < SWAP_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(SWAP_SEED + i));
    }

    /* The card beside it, which said nothing: permuted within each word. */
    bridge_fill_sentinel(qts, TARGET_PAGE_A);
    card_transfer(qts, PLAIN_SLOT, PCI_DIRECT_32 | TARGET_PAGE_A, SWAP_LEN);
    qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
    for (i = 0; i < SWAP_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(SWAP_SEED + (i ^ 3)));
    }

    qtest_quit(qts);
}

/*
 * The same decision governs the register windows, so a card outside the slot
 * the board named had its narrow accesses mis-addressed too. The card's
 * 64-bit register block holds a constant whose bytes all differ, and the
 * slot is left 32-bit so that a byte lands on the narrow path: which byte
 * answers says which offset the bridge addressed. Stated, not computed,
 * the four low bytes of 0x0123456789abcdef, which the byte-lane rule reaches
 * at the offset itself or at offset xor three.
 */
#define CARD_WIDE_MMIO(slot)    (CARD_MMIO(slot) + 0x100000u)
#define WIDE_LOW_BYTES          { 0xef, 0xcd, 0xab, 0x89 }

static void card_aim_at_its_wide_window(QTestState *qts, unsigned int slot)
{
    qtest_writel(qts, CFG0(slot, 0) + PCI_BAR1, CARD_WIDE_MMIO(slot));
    qtest_writel(qts, CFG0(slot, 0) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, REG(BRIDGE_DEVICE(slot)),
                 DEV_MEMORY_SPACE | (CARD_WIDE_MMIO(slot) >> 20));
}

static void test_a_self_ordered_card_reads_narrow_straight(const void *data)
{
    static const uint8_t byte[4] = WIDE_LOW_BYTES;
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE BOTH_CARDS);
    unsigned int i;

    card_aim_at_its_wide_window(qts, SELF_SLOT);
    card_aim_at_its_wide_window(qts, PLAIN_SLOT);

    for (i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readb(qts, WINDOW(SELF_SLOT) + WIDE_FIXED + i),
                        ==, byte[i]);
        g_assert_cmphex(qtest_readb(qts, WINDOW(PLAIN_SLOT) + WIDE_FIXED + i),
                        ==, byte[i ^ 3]);
    }

    /*
     * A word is one whole cycle, so the byte lanes do not move it: both
     * cards read the same word through the same windows. Without this a
     * window aimed somewhere else would look like a byte-lane answer.
     */
    g_assert_cmphex(qtest_readl(qts, WINDOW(SELF_SLOT) + WIDE_FIXED), ==,
                    qtest_readl(qts, WINDOW(PLAIN_SLOT) + WIDE_FIXED));

    qtest_quit(qts);
}

/*
 * And the chip the board actually carries, in a slot no board puts it in.
 * The card above declares itself because a test told it to; this one
 * declares itself because of what it is. An
 * IOC3 added on the command line must be exempt in slot 5 exactly as the
 * board's own is in slot 2. The model reports the decision when a guest
 * asks for a swap it will not reproduce, which is where this reads it.
 */
#define SPARE_IOC3_SLOT         5
#define SPARE_IOC3              " -device sgi-ioc3,addr=" \
                                stringify(SPARE_IOC3_SLOT)

static void test_an_ioc3_is_exempt_in_any_slot(const void *data)
{
    g_assert_true(bridge_log_has_the_message_at(SPARE_IOC3, SPARE_IOC3_SLOT,
                                                DEV_SWAP_DIRECT));
    g_assert_true(bridge_log_has_the_message_at(SPARE_IOC3, SPARE_IOC3_SLOT,
                                                DEV_SWAP_TRANSLATED));

    /*
     * And the board's own controller, in the slot the board really uses.
     * Nothing on the machine's side names that slot any more, so this is
     * where the default configuration's exemption is now pinned.
     */
    g_assert_true(bridge_log_has_the_message_at("", IOC3_SLOT,
                                                DEV_SWAP_DIRECT));

    /*
     * The same slot with an ordinary card in it, so this is about the chip
     * and not about slot 5 having become exempt some other way.
     */
    g_assert_false(bridge_log_has_the_message_at(PLAIN_CARD, PLAIN_SLOT,
                                                 DEV_SWAP_DIRECT));
}

/*
 * The 64-bit decode: a node-1 write must land in node 1's RAM and leave
 * node 0's low RAM, where the dropped NASID bit would have put it,
 * untouched. For a write, the miss is silent corruption.
 */
static void test_dma_direct_window_reaches_the_second_node(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_TWO_NODE
                                 " -device sgi-bridge-dma-test,addr=6,bus=pci");
    uint8_t seen[DMA64_LEN], node0[DMA64_LEN];
    unsigned int i;

    /* No PROM ran to assign node 1 its number; this DMA needs it claimed. */
    sn0_program_nasid(qts, 1);

    bridge_start_the_transfer_source(qts);
    bridge_source_with_swap(qts, DEV_SWAP_DIRECT);

    /* Offset zero and the 512 MB step cleared, so the address decodes to
     * itself. */
    qtest_writel(qts, REG(BRIDGE_DIRECT_MAP), 0);

    /* Program the 64-bit destination: the low half, then the high half. */
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_ADDRESS,
                 (uint32_t)NODE1_PCI_ADDR);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_ADDRESS_HI,
                 NODE1_PCI_ADDR >> 32);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_LENGTH, DMA64_LEN);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_SEED, DMA64_SEED);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_GO, 1);

    /* It reached node 1's RAM at the full address, not truncated into node
     * 0. */
    qtest_memread(qts, NODE1_SYS_ADDR, seen, sizeof(seen));
    for (i = 0; i < DMA64_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(DMA64_SEED + i));
    }

    /* Node 0's low RAM, where dropping bit 32 would have landed it, is
     * clean. */
    qtest_memread(qts, NODE0_TRUNCATED_ADDR, node0, sizeof(node0));
    for (i = 0; i < DMA64_LEN; i++) {
        g_assert_cmpuint(node0[i], ==, 0);
    }

    qtest_quit(qts);
}

/*
 * The read direction: a 64-bit source above 4 GiB must be fetched from
 * node 1, not truncated into node 0.
 */
static void test_dma_direct_window_reads_the_second_node(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_TWO_NODE
                                 " -device sgi-bridge-dma-test,addr=6,bus=pci");
    uint8_t pattern[DMA64_LEN], seen[DMA64_LEN];
    unsigned int i;

    /* No PROM ran to assign node 1 its number; this DMA needs it claimed. */
    sn0_program_nasid(qts, 1);

    bridge_start_the_transfer_source(qts);
    bridge_source_with_swap(qts, DEV_SWAP_DIRECT);
    qtest_writel(qts, REG(BRIDGE_DIRECT_MAP), 0);

    /* Seed node 1's RAM at the 64-bit source; a truncated read would miss
     * it. */
    for (i = 0; i < DMA64_LEN; i++) {
        pattern[i] = (uint8_t)(DMA64_SEED + i);
    }
    qtest_memwrite(qts, COPY_SRC_SYS, pattern, sizeof(pattern));

    /* Copy from the 64-bit node-1 source to a node-0 local destination. */
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_SOURCE, (uint32_t)COPY_SRC_PCI);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_SOURCE_HI, COPY_SRC_PCI >> 32);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_ADDRESS, COPY_DST_ADDR);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_ADDRESS_HI, 0);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_LENGTH, DMA64_LEN);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_GO, 2);   /* GO_COPY */

    /* The destination holds node 1's bytes, proving the read reached node
     * 1. */
    qtest_memread(qts, COPY_DST_ADDR, seen, sizeof(seen));
    for (i = 0; i < DMA64_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(DMA64_SEED + i));
    }

    qtest_quit(qts);
}

/*
 * A 64-bit cycle ignores the offset registers: DIRECT_MAP stays at reset
 * and the transfer must land at the bare crosstalk offset, not stepped
 * half a gigabyte into unbacked memory.
 */
static void test_dma_direct_window_64bit_ignores_the_offset_step(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_TWO_NODE
                                 " -device sgi-bridge-dma-test,addr=6,bus=pci");
    uint8_t seen[DMA64_LEN];
    unsigned int i;

    bridge_start_the_transfer_source(qts);
    bridge_source_with_swap(qts, DEV_SWAP_DIRECT);
    /* DIRECT_MAP left at its reset value: the 512 MB step is active. */

    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_ADDRESS,
                 (uint32_t)DMA64_NODE0_PCI);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_ADDRESS_HI,
                 DMA64_NODE0_PCI >> 32);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_LENGTH, DMA64_LEN);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_SEED, DMA64_SEED);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_GO, 1);

    /* It landed at the bare crosstalk offset, not 512 MB above it. */
    qtest_memread(qts, DMA64_NODE0_SYS, seen, sizeof(seen));
    for (i = 0; i < DMA64_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(DMA64_SEED + i));
    }

    qtest_quit(qts);
}

/*
 * A 64-bit device window: with the device-size bit set the Bridge serves an
 * eight-byte access as one cycle and mirrors the doubleword on the
 * programmed-access swap bit. Read a fixed register whole under both
 * settings (unchanged clear, byte-reversed set), confirm a 32-bit slot still
 * splits and never mirrors, and round-trip the scratch through the write
 * path.
 */
static void test_dma_device_window_64bit_doubleword(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE DMA_CARD);

    /* Put the 64-bit register BAR where a device window can name it, and let
     * the card decode memory. */
    qtest_writel(qts, CFG0(DMA_SLOT, 0) + PCI_BAR1, WIDE_CARD_MMIO);
    qtest_writel(qts, CFG0(DMA_SLOT, 0) + PCI_COMMAND, PCI_COMMAND_MEMORY);

    /* 64-bit device, programmed-access swap clear: the doubleword is
     * unchanged. */
    qtest_writel(qts, REG(BRIDGE_DEVICE(DMA_SLOT)),
                 DEV_MEMORY_SPACE | DEV_64BIT_DEVICE | (WIDE_CARD_MMIO >> 20));
    g_assert_cmphex(qtest_readq(qts, WINDOW(DMA_SLOT) + WIDE_FIXED), ==,
                    WIDE_GOLDEN_UNCHANGED);

    /* Same slot, programmed-access swap set: the doubleword is reversed end
     * to end. */
    qtest_writel(qts, REG(BRIDGE_DEVICE(DMA_SLOT)),
                 DEV_MEMORY_SPACE | DEV_64BIT_DEVICE | DEV_SWAP_PROGRAMMED |
                 (WIDE_CARD_MMIO >> 20));
    g_assert_cmphex(qtest_readq(qts, WINDOW(DMA_SLOT) + WIDE_FIXED), ==,
                    WIDE_GOLDEN_MIRRORED);

    /* Clear the device-size bit: a 32-bit slot splits the doubleword into
     * two word cycles (big-endian high word at the lower offset) and does
     * not consult the programmed-access swap bit, so the read is the natural
     * doubleword, the same as the one-cycle read, whether that bit is set or
     * not. */
    qtest_writel(qts, REG(BRIDGE_DEVICE(DMA_SLOT)),
                 DEV_MEMORY_SPACE | (WIDE_CARD_MMIO >> 20));
    g_assert_cmphex(qtest_readq(qts, WINDOW(DMA_SLOT) + WIDE_FIXED), ==,
                    WIDE_GOLDEN_UNCHANGED);
    qtest_writel(qts, REG(BRIDGE_DEVICE(DMA_SLOT)),
                 DEV_MEMORY_SPACE | DEV_SWAP_PROGRAMMED | (WIDE_CARD_MMIO >> 20));
    g_assert_cmphex(qtest_readq(qts, WINDOW(DMA_SLOT) + WIDE_FIXED), ==,
                    WIDE_GOLDEN_UNCHANGED);

    /*
     * The write direction round-trips a doubleword to the scratch. The swap
     * bit mirrors both directions, so the value returns unchanged either
     * way; each setting confirms the eight-byte write reaches the device
     * whole.
     */
    qtest_writel(qts, REG(BRIDGE_DEVICE(DMA_SLOT)),
                 DEV_MEMORY_SPACE | DEV_64BIT_DEVICE | (WIDE_CARD_MMIO >> 20));
    qtest_writeq(qts, WINDOW(DMA_SLOT) + WIDE_SCRATCH, WIDE_ROUNDTRIP_VALUE);
    g_assert_cmphex(qtest_readq(qts, WINDOW(DMA_SLOT) + WIDE_SCRATCH), ==,
                    WIDE_ROUNDTRIP_VALUE);

    qtest_writel(qts, REG(BRIDGE_DEVICE(DMA_SLOT)),
                 DEV_MEMORY_SPACE | DEV_64BIT_DEVICE | DEV_SWAP_PROGRAMMED |
                 (WIDE_CARD_MMIO >> 20));
    qtest_writeq(qts, WINDOW(DMA_SLOT) + WIDE_SCRATCH, WIDE_ROUNDTRIP_VALUE);
    g_assert_cmphex(qtest_readq(qts, WINDOW(DMA_SLOT) + WIDE_SCRATCH), ==,
                    WIDE_ROUNDTRIP_VALUE);

    /*
     * The 32-bit-slot split has its own write reassembly, uncovered by the
     * round-trip above, which drives both directions through the one-cycle
     * path. Exercise it across paths: write the scratch through the split,
     * read it back through the one-cycle path. A transposed split write
     * surfaces here as a halves-swapped one-cycle read; it returns the value
     * only when both directions place the big-endian high word at the lower
     * offset.
     */
    qtest_writel(qts, REG(BRIDGE_DEVICE(DMA_SLOT)),
                 DEV_MEMORY_SPACE | (WIDE_CARD_MMIO >> 20));
    qtest_writeq(qts, WINDOW(DMA_SLOT) + WIDE_SCRATCH, WIDE_ROUNDTRIP_VALUE);
    qtest_writel(qts, REG(BRIDGE_DEVICE(DMA_SLOT)),
                 DEV_MEMORY_SPACE | DEV_64BIT_DEVICE | (WIDE_CARD_MMIO >> 20));
    g_assert_cmphex(qtest_readq(qts, WINDOW(DMA_SLOT) + WIDE_SCRATCH), ==,
                    WIDE_ROUNDTRIP_VALUE);

    qtest_quit(qts);
}

/* The interrupt registers, and the fields these tests drive. */
#define BRIDGE_INT_STATUS       0x100
#define BRIDGE_INT_ENABLE       0x108
#define BRIDGE_INT_MODE         0x118
#define BRIDGE_INT_DEVICE       0x120
#define BRIDGE_INT_HOST(n)      (0x130 + 8 * (n))

#define DMA_REG_INTERRUPT       0x10

/*
 * Delivery: a pin edge becomes a crosstalk write to the Hub's
 * interrupt-modify register, read back through the Hub's local window.
 */
#define HUB_LOCAL_WINDOW        (SPACE_XTALK + 0x01000000)
#define PI_INT_PEND_MOD         0x90
#define INT_TEST_HOST_NODE      0
#define INT_TEST_HOST_NUMBER    42


/*
 * Slot 3 drives pin 3, which the base I/O controller's second
 * interrupt uses, not the convenient one.
 */
#define INT_TEST_SLOT           3

/*
 * A timing write runs a slot finished by the time it returns; wrong is a
 * hang, not a wrong value: the guest spins on the finished flag.
 */
static void test_a_write_runs_a_bit_slot(const void *data)
{
    uint32_t nic;

    bridge_reset();

    /* The widths mainline uses to reset the bus. */
    qtest_writel(qts, REG(BRIDGE_NIC), NIC_PULSE(520) | NIC_SAMPLE(65));

    nic = qtest_readl(qts, REG(BRIDGE_NIC));
    g_assert_cmphex(nic & NIC_DONE, ==, NIC_DONE);

    /* And the timings read back, so a driver can see what it asked for. */
    g_assert_cmphex(nic & (NIC_PULSE(0x3ff) | NIC_SAMPLE(0xff)), ==,
                    NIC_PULSE(520) | NIC_SAMPLE(65));
}

/*
 * Asserted together because each alone passes a constant-answering model;
 * only the difference says the timings are read.
 */
/*
 * One slot, mainline's widths: long resets, short writes a one or reads,
 * middling writes a zero. A read and a written one are one slot.
 */
static bool nic_slot(QTestState *qts, unsigned int pulse, unsigned int sample)
{
    qtest_writel(qts, REG(BRIDGE_NIC), NIC_PULSE(pulse) | NIC_SAMPLE(sample));
    return qtest_readl(qts, REG(BRIDGE_NIC)) & NIC_LEVEL;
}

#define NIC_RESET(q)    nic_slot((q), 520, 65)
#define NIC_READ(q)     nic_slot((q), 6, 13)
#define NIC_WRITE_ONE(q)  nic_slot((q), 6, 13)
#define NIC_WRITE_ZERO(q) nic_slot((q), 80, 30)

/*
 * Drive a whole search and read the identity out of it, compared whole:
 * asserting only the family code passes a model that answers the family
 * and then anything at all.
 */
static void test_a_search_returns_the_identity(const void *data)
{
    uint64_t rom = 0;
    unsigned int i;

    bridge_reset();

    g_assert_false(NIC_RESET(qts));         /* low: part present */

    /* The search command, least-significant bit first. */
    for (i = 0; i < 8; i++) {
        if ((0xf0u >> i) & 1) {
            NIC_WRITE_ONE(qts);
        } else {
            NIC_WRITE_ZERO(qts);
        }
    }

    /*
     * Each identity bit takes three slots: the bit, its complement, and the
     * choice written back. The complement is checked every time, because a
     * model returning a constant would satisfy the bit alone.
     */
    for (i = 0; i < 64; i++) {
        bool bit = NIC_READ(qts);
        bool complement = NIC_READ(qts);

        g_assert_cmpint(complement, ==, !bit);
        if (bit) {
            rom |= 1ULL << i;
            NIC_WRITE_ONE(qts);
        } else {
            NIC_WRITE_ZERO(qts);
        }
    }

    g_assert_cmphex(rom, ==, 0x360605040302010bULL);
}

/* Shift a byte out to the part, least-significant bit first. */
static void nic_write_byte(QTestState *qts, uint8_t byte)
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        if ((byte >> i) & 1) {
            NIC_WRITE_ONE(qts);
        } else {
            NIC_WRITE_ZERO(qts);
        }
    }
}

/* And read one back the same way. */
static uint8_t nic_read_byte(QTestState *qts)
{
    unsigned int i;
    uint8_t byte = 0;

    for (i = 0; i < 8; i++) {
        if (NIC_READ(qts)) {
            byte |= 1u << i;
        }
    }
    return byte;
}

/* The check a guest runs across a whole exchange, and what it requires. */
static uint16_t nic_crc(uint16_t crc, const uint8_t *data, unsigned int len)
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
#define NIC_CRC_RESIDUE 0xb001

/*
 * The IP29 board's identity record, page 0 then page 1, check values
 * included, restated as bytes so the comparison does not share the model's
 * construction.
 */
static const uint8_t ip29_record[64] = {
    0x01, 0x4f, 0x49, 0x50, 0x32, 0x39, 0x30, 0x30,
    0x30, 0x30, 0x31, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x30, 0x33, 0x30,
    0x2d, 0x31, 0x32, 0x34, 0x34, 0x2d, 0x34, 0x1e,
    0x20, 0x20, 0x20, 0x30, 0x30, 0x31, 0x48, 0x20,
    0x20, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x49, 0x50, 0x32, 0x39, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0xe3, 0x24,
};

/*
 * All three residue checks a guest runs; only the stored-bytes one proves
 * the embedded check. Framing passes whatever the last two bytes hold.
 */
/*
 * The part number as the positional reader derives it, not as a string
 * somebody expected.
 */
static void partnum_from_page(const uint8_t *page, char *out)
{
    unsigned int i, j = 0;

    for (i = 0; i < 19; i++) {
        if (page[i + 11] != ' ') {
            out[j++] = page[i + 11];
        }
    }
    for (i = 0; i < 6; i++) {
        if (page[i + 32] != ' ') {
            out[j++] = page[i + 32];
        }
    }
    out[j] = 0;
}

static void test_a_page_reads_back_with_its_checks(const void *data)
{
    uint8_t exchange[6];
    uint8_t page[34];
    unsigned int i;

    bridge_reset();

    g_assert_false(NIC_RESET(qts));

    /* One part on the bus, so the guest skips addressing it. */
    nic_write_byte(qts, 0xcc);

    /* Read memory, from the first page. */
    exchange[0] = 0xa5;
    exchange[1] = 0x00;
    exchange[2] = 0x00;
    for (i = 0; i < 3; i++) {
        nic_write_byte(qts, exchange[i]);
    }

    /* The redirection byte and the check over request and answer together. */
    for (i = 3; i < 6; i++) {
        exchange[i] = nic_read_byte(qts);
    }
    g_assert_cmphex(exchange[3], ==, 0xff);     /* this page is not moved */
    g_assert_cmphex(nic_crc(0, exchange, 6), ==, NIC_CRC_RESIDUE);

    /* Then the block and the two framing bytes the read protocol appends. */
    for (i = 0; i < sizeof(page); i++) {
        page[i] = nic_read_byte(qts);
    }
    g_assert_cmphex(nic_crc(0, page, sizeof(page)), ==, NIC_CRC_RESIDUE);

    /*
     * The stored bytes alone: the residue a part-number read requires and
     * the framing check cannot see.
     */
    g_assert_cmphex(nic_crc(0, page, sizeof(page) - 2), ==, NIC_CRC_RESIDUE);

    /* The stored page itself, every field at its fixed offset. */
    g_assert_cmpmem(page, sizeof(page) - 2, ip29_record, sizeof(page) - 2);
}

/* Drive a search and return the identity the part hands back. */
static uint64_t nic_search_for_identity(QTestState *qts)
{
    uint64_t rom = 0;
    unsigned int i;

    nic_write_byte(qts, 0xf0);
    for (i = 0; i < 64; i++) {
        bool bit = NIC_READ(qts);
        bool complement = NIC_READ(qts);

        g_assert_cmpint(complement, ==, !bit);
        if (bit) {
            rom |= 1ULL << i;
            NIC_WRITE_ONE(qts);
        } else {
            NIC_WRITE_ZERO(qts);
        }
    }
    return rom;
}

/*
 * The same search through a specific physical node's Bridge window. A
 * dual-chassis Origin 200 has one board identity part per Bridge, and the
 * firmware uses that identity when the Hub's own part cannot be read.
 */
static bool nic_slot_at(QTestState *qts, uint64_t bridge, unsigned int pulse,
                        unsigned int sample)
{
    uint64_t reg = bridge + BRIDGE_NIC + REG_DATA;

    qtest_writel(qts, reg, NIC_PULSE(pulse) | NIC_SAMPLE(sample));
    return qtest_readl(qts, reg) & NIC_LEVEL;
}

static void nic_write_byte_at(QTestState *qts, uint64_t bridge, uint8_t byte)
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        nic_slot_at(qts, bridge, (byte >> i) & 1 ? 6 : 80,
                    (byte >> i) & 1 ? 13 : 30);
    }
}

static uint64_t nic_search_for_identity_at(QTestState *qts, uint64_t bridge)
{
    uint64_t rom = 0;
    unsigned int i;

    g_assert_false(nic_slot_at(qts, bridge, 520, 65));
    nic_write_byte_at(qts, bridge, 0xf0);
    for (i = 0; i < 64; i++) {
        bool bit = nic_slot_at(qts, bridge, 6, 13);
        bool complement = nic_slot_at(qts, bridge, 6, 13);

        g_assert_cmpint(complement, ==, !bit);
        if (bit) {
            rom |= 1ULL << i;
        }
        nic_slot_at(qts, bridge, bit ? 6 : 80, bit ? 13 : 30);
    }
    return rom;
}

static uint8_t nic_crc8(uint8_t crc, uint64_t rom)
{
    unsigned int byte, bit;

    for (byte = 0; byte < 8; byte++) {
        crc ^= rom >> (byte * 8);
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8c : crc >> 1;
        }
    }
    return crc;
}

/* The Hub's network interface, as far as renumbering a stock machine
 * needs. */
#define HUB_NI_STATUS           0x600000
#define HUB_NI_VECTOR_PARMS     0x600200
#define HUB_NI_VECTOR           0x600208
#define HUB_NI_VECTOR_DATA      0x600210
#define HUB_NI_REGION_SIZE      (1ULL << 17)
#define HUB_NI_NODE_MASK        (0x1ffULL << 8)
#define HUB_NI_PIOTYPE_WRITE    1
#define HUB_NODE_WINDOW         (SPACE_XTALK + 0x01800000)

/*
 * A stock-PROM machine boots with every Hub programmed zero, so a
 * node-one-qualified address names no Hub. Program node 1's real number
 * the way the PROM's global master does, a vector write over the
 * CrayLink, so the test can reach node 1's bridge by address.
 */
static void assign_node1_number(QTestState *qts)
{
    qtest_writeq(qts, HUB_LOCAL_WINDOW + HUB_NI_VECTOR, 0);
    qtest_writeq(qts, HUB_LOCAL_WINDOW + HUB_NI_VECTOR_DATA,
                 HUB_NI_REGION_SIZE | (1ULL << 8));
    qtest_writeq(qts, HUB_LOCAL_WINDOW + HUB_NI_VECTOR_PARMS,
                 HUB_NI_STATUS | HUB_NI_PIOTYPE_WRITE);
    g_assert_cmphex(qtest_readq(qts, HUB_NODE_WINDOW + NODE_STRIDE +
                                     HUB_NI_STATUS) & HUB_NI_NODE_MASK,
                    ==, 1ULL << 8);
}

static void test_each_node_has_its_own_board_identity(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200,topology=origin200-dual -m 128M "
        "-smp 4");
    uint64_t node0;
    uint64_t node1;

    assign_node1_number(qts);
    node0 = nic_search_for_identity_at(qts, BRIDGE_BASE);
    node1 = nic_search_for_identity_at(qts, BRIDGE_BASE + NODE_STRIDE);

    g_assert_cmphex(node0 & 0xff, ==, 0x0b);
    g_assert_cmphex(node1 & 0xff, ==, 0x0b);
    g_assert_cmphex(nic_crc8(0, node0), ==, 0);
    g_assert_cmphex(nic_crc8(0, node1), ==, 0);
    g_assert_cmphex(node0, !=, node1);

    qtest_quit(qts);
}

/* Name an identity, sixty-four bits of it, least-significant first. */
static void nic_match_identity(QTestState *qts, uint64_t rom)
{
    unsigned int i;

    nic_write_byte(qts, 0x55);
    for (i = 0; i < 64; i++) {
        if ((rom >> i) & 1) {
            NIC_WRITE_ONE(qts);
        } else {
            NIC_WRITE_ZERO(qts);
        }
    }
}

/*
 * Same page, same residues as the skip-addressed read; what varies is the
 * selection, which is the thing under test.
 */
static void nic_read_first_page(QTestState *qts)
{
    uint8_t exchange[6];
    uint8_t page[34];
    unsigned int i;

    exchange[0] = 0xa5;
    exchange[1] = 0x00;
    exchange[2] = 0x00;
    for (i = 0; i < 3; i++) {
        nic_write_byte(qts, exchange[i]);
    }

    for (i = 3; i < 6; i++) {
        exchange[i] = nic_read_byte(qts);
    }
    g_assert_cmphex(exchange[3], ==, 0xff);
    g_assert_cmphex(nic_crc(0, exchange, 6), ==, NIC_CRC_RESIDUE);

    for (i = 0; i < sizeof(page); i++) {
        page[i] = nic_read_byte(qts);
    }
    g_assert_cmphex(nic_crc(0, page, sizeof(page)), ==, NIC_CRC_RESIDUE);
    g_assert_cmphex(nic_crc(0, page, sizeof(page) - 2), ==, NIC_CRC_RESIDUE);
    g_assert_cmpmem(page, sizeof(page) - 2, ip29_record, sizeof(page) - 2);
}

/*
 * Firmware's exchange end to end: search, match, read. The match is fed
 * the identity the search returned, so a model offering one identity and
 * accepting another fails.
 */
static void test_a_named_part_reads_back_its_page(const void *data)
{
    uint64_t rom;

    bridge_reset();

    g_assert_false(NIC_RESET(qts));
    rom = nic_search_for_identity(qts);
    g_assert_cmphex(rom, ==, 0x360605040302010bULL);

    /* The reset between the search and the match, as firmware sends it. */
    g_assert_false(NIC_RESET(qts));
    nic_match_identity(qts, rom);

    nic_read_first_page(qts);
}

/*
 * A wrong Match ROM deselects the part until reset. This stops
 * the test above passing a model that selects whatever names it. The
 * check is the wire idling high from the function byte on.
 */
static void test_naming_another_part_deselects_this_one(const void *data)
{
    unsigned int i;

    bridge_reset();

    g_assert_false(NIC_RESET(qts));

    /* One bit different from this part's identity, in the serial number. */
    nic_match_identity(qts, 0x360605040302010bULL ^ (1ULL << 40));

    /* Ask the deselected part to read a page anyway. */
    nic_write_byte(qts, 0xa5);
    nic_write_byte(qts, 0x00);
    nic_write_byte(qts, 0x00);

    /*
     * A part that answered would put a zero somewhere in this; an undriven
     * open-drain wire reads high for every one.
     */
    for (i = 0; i < 40; i++) {
        g_assert_cmphex(nic_read_byte(qts), ==, 0xff);
    }
}

/*
 * The bytes alone pass an address-ignoring model; the check pairs are the
 * test. Mid-page, the two checks have different bases; a model using
 * one basis for both fails whichever it did not choose.
 */
static void test_the_status_memory_reads_back_by_the_page(const void *data)
{
    uint8_t first[9];
    uint8_t next[10];
    unsigned int i;

    bridge_reset();

    g_assert_false(NIC_RESET(qts));
    nic_write_byte(qts, 0xcc);

    /*
     * A non-zero high address byte: with every data byte identical, the
     * address is visible only through the check.
     */
    first[0] = 0xaa;
    first[1] = 0x04;
    first[2] = 0x01;
    for (i = 0; i < 3; i++) {
        nic_write_byte(qts, first[i]);
    }

    /* Four bytes to the end of the page, then the check over all of it. */
    for (i = 3; i < 7; i++) {
        first[i] = nic_read_byte(qts);
        g_assert_cmphex(first[i], ==, 0xff);
    }
    first[7] = nic_read_byte(qts);
    first[8] = nic_read_byte(qts);
    g_assert_cmphex(nic_crc(0, first, sizeof(first)), ==, NIC_CRC_RESIDUE);

    /* The part carries on into the next page for as long as a guest reads. */
    for (i = 0; i < 8; i++) {
        next[i] = nic_read_byte(qts);
        g_assert_cmphex(next[i], ==, 0xff);
    }
    next[8] = nic_read_byte(qts);
    next[9] = nic_read_byte(qts);
    g_assert_cmphex(nic_crc(0, next, sizeof(next)), ==, NIC_CRC_RESIDUE);
}

/* A skip-addressed status read from one address, keeping the request. */
static void nic_status_request(QTestState *qts, uint16_t addr, uint8_t *hdr)
{
    unsigned int i;

    g_assert_false(NIC_RESET(qts));
    nic_write_byte(qts, 0xcc);

    hdr[0] = 0xaa;
    hdr[1] = addr & 0xff;
    hdr[2] = addr >> 8;
    for (i = 0; i < 3; i++) {
        nic_write_byte(qts, hdr[i]);
    }
}

/*
 * One whole eight-byte status page and its check from a page-aligned
 * start, then ten further slots: a whole page and check pair. All ones
 * across those ten says the stream ended here rather than carrying on: a
 * page that really followed would close with 0xbe 0x7b, not with ones.
 * The masking and the 0xff fill are both stated in the part
 * specification; that the stream TERMINATES at the top of the field
 * rather than running on is inferred from the data memory's stated end,
 * and a literal statement about a status read's end of field, or a guest
 * that reads this far, would settle it.
 *
 * `masked` is the address the part reads from, which differs from `addr`
 * only above the top. The check covers the masked address, so where the
 * two differ the residue over what the master sent does not come out,
 * that mismatch is how a master learns its start address was out of range
 * instead of being handed the aliased page as the one it asked for.
 */
static void nic_status_final_page(QTestState *qts, uint16_t addr,
                                  uint16_t masked)
{
    uint8_t sent[13];
    uint8_t read_from[13];
    unsigned int i;

    nic_status_request(qts, addr, sent);
    for (i = 3; i < 11; i++) {
        sent[i] = nic_read_byte(qts);
        g_assert_cmphex(sent[i], ==, 0xff);
    }
    sent[11] = nic_read_byte(qts);
    sent[12] = nic_read_byte(qts);

    memcpy(read_from, sent, sizeof(read_from));
    read_from[1] = masked & 0xff;
    read_from[2] = masked >> 8;
    g_assert_cmphex(nic_crc(0, read_from, sizeof(read_from)), ==,
                    NIC_CRC_RESIDUE);
    if (masked != addr) {
        g_assert_cmphex(nic_crc(0, sent, sizeof(sent)), !=, NIC_CRC_RESIDUE);
    }

    for (i = 0; i < 10; i++) {
        g_assert_cmphex(nic_read_byte(qts), ==, 0xff);
    }
}

/*
 * The status field is decoded across the same 0x7ff of address the data
 * memory is, and only four small regions inside it are ever programmed.
 * Every other address reads unprogrammed ones, still framed by a check
 * every eight bytes, so past the programmed regions the part keeps
 * answering instead of going quiet, which is the one thing a guest cannot
 * tell apart from a part that is not fitted. The 0x7ff masking and the
 * 0xff fill are stated in the part specification; where the stream stops
 * is not, and is inferred from the data memory's stated end.
 */
static void test_status_past_the_programmed_regions(const void *data)
{
    uint8_t first[13];
    uint8_t next[10];
    unsigned int i;

    bridge_reset();

    /* Past every programmed region, and well inside the address field. */
    nic_status_request(qts, 0x200, first);
    for (i = 3; i < 11; i++) {
        first[i] = nic_read_byte(qts);
        g_assert_cmphex(first[i], ==, 0xff);
    }
    first[11] = nic_read_byte(qts);
    first[12] = nic_read_byte(qts);
    g_assert_cmphex(nic_crc(0, first, sizeof(first)), ==, NIC_CRC_RESIDUE);

    /* And on into the next page, whose check starts from a cleared basis. */
    for (i = 0; i < 8; i++) {
        next[i] = nic_read_byte(qts);
        g_assert_cmphex(next[i], ==, 0xff);
    }
    next[8] = nic_read_byte(qts);
    next[9] = nic_read_byte(qts);
    g_assert_cmphex(nic_crc(0, next, sizeof(next)), ==, NIC_CRC_RESIDUE);

    /*
     * The top page of the field, then that same page named from above the
     * top: a start address past 0x7ff has its high address bits forced to
     * zero, so both end after one page rather than being refused or
     * running on.
     */
    nic_status_final_page(qts, 0x7f8, 0x7f8);
    nic_status_final_page(qts, 0xff8, 0x7f8);
}

/* Read one page the framed way and keep the thirty-two stored bytes. */
static void nic_extended_page(QTestState *qts, uint16_t offset, uint8_t *out)
{
    unsigned int i;

    g_assert_false(NIC_RESET(qts));
    nic_write_byte(qts, 0xcc);
    nic_write_byte(qts, 0xa5);
    nic_write_byte(qts, offset & 0xff);
    nic_write_byte(qts, offset >> 8);

    for (i = 0; i < 3; i++) {
        nic_read_byte(qts);             /* redirection byte and its check */
    }
    for (i = 0; i < 32; i++) {
        out[i] = nic_read_byte(qts);
    }
}

/*
 * Whole pages: a model producing the check as framing satisfies the framed
 * read and hands this one text with holes. Crossing the page boundary
 * proves the stream puts nothing between pages.
 */
static void test_the_unframed_read_matches_the_framed_one(const void *data)
{
    uint8_t framed[64];
    uint8_t plain[64];
    char partnum[26];
    unsigned int i;

    bridge_reset();

    nic_extended_page(qts, 0, &framed[0]);
    nic_extended_page(qts, 32, &framed[32]);

    /*
     * Addressed by name, as the firmware does: 0xf0 and 0x55 mean opposite
     * things before and after a part is addressed, and this is the exchange
     * where the reuse is actually ambiguous.
     */
    g_assert_false(NIC_RESET(qts));
    nic_match_identity(qts, 0x360605040302010bULL);
    nic_write_byte(qts, 0xf0);
    nic_write_byte(qts, 0x00);
    nic_write_byte(qts, 0x00);
    for (i = 0; i < sizeof(plain); i++) {
        plain[i] = nic_read_byte(qts);
    }

    g_assert_cmpmem(plain, sizeof(plain), framed, sizeof(framed));

    /*
     * The vendor reader rejects the record unless page A starts with
     * format 1. Without it the field parser sees no BaseIO identity.
     */
    g_assert_cmphex(plain[0], ==, 0x01);

    /*
     * Linux assembles the part number from bytes 11-29 and 32-37 with the
     * spaces dropped; the record splits it around page 0's check.
     */
    partnum_from_page(plain, partnum);
    g_assert_cmpstr(partnum, ==, "030-1244-001");

    g_assert_cmpmem(plain, sizeof(plain), ip29_record, sizeof(ip29_record));
    g_assert_cmphex(nic_crc(0, &plain[0], 32), ==, NIC_CRC_RESIDUE);
    g_assert_cmphex(nic_crc(0, &plain[32], 32), ==, NIC_CRC_RESIDUE);
}

static void test_the_model_board_part_overrides_class_global(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 "
        "-global sgi-bridge.board-part=123-4567-890");
    uint8_t image[64];
    char partnum[26];

    nic_extended_page(qts, 0, &image[0]);
    nic_extended_page(qts, 32, &image[32]);

    partnum_from_page(image, partnum);
    g_assert_cmpstr(partnum, ==, "030-1244-001");
    g_assert_cmpmem(image, sizeof(image), ip29_record, sizeof(ip29_record));
    g_assert_cmphex(nic_crc(0, &image[0], 32), ==, NIC_CRC_RESIDUE);
    g_assert_cmphex(nic_crc(0, &image[32], 32), ==, NIC_CRC_RESIDUE);

    qtest_quit(qts);
}

typedef struct BridgeBoardPartCase {
    const char *part;
    const char *message;
} BridgeBoardPartCase;

static const BridgeBoardPartCase invalid_board_parts[] = {
    {
        NULL,
        "board-part property is required",
    },
    {
        "123-4567-8900",
        "board part must be 12 characters without a semicolon",
    },
    {
        "123;4567-890",
        "board part must be 12 characters without a semicolon",
    },
};

static void test_invalid_board_part_is_rejected(const void *data)
{
    const BridgeBoardPartCase *test = data;

    g_assert_cmpstr(sgi_bridge_board_part_error(test->part), ==,
                    test->message);
}

/*
 * Past the programmed pages the memory reads unprogrammed. It does not wrap,
 * which would hand a guest a spurious second part number.
 */
static void test_the_data_past_the_image_is_unprogrammed(const void *data)
{
    unsigned int i;

    bridge_reset();

    g_assert_false(NIC_RESET(qts));
    nic_write_byte(qts, 0xcc);
    nic_write_byte(qts, 0xf0);
    nic_write_byte(qts, 0x40);          /* the first page after the image */
    nic_write_byte(qts, 0x00);

    for (i = 0; i < 32; i++) {
        g_assert_cmphex(nic_read_byte(qts), ==, 0xff);
    }
}

/*
 * And the framed read pads the same way. It used to serve page zero's
 * bytes for an address the image does not cover, which hands a guest a
 * second, spurious copy of the part number at an address that has none.
 * The framing is asserted alongside the bytes, so a model that padded by
 * falling silent fails too.
 */
static void test_the_framed_read_past_the_image_is_unprogrammed(
    const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t exchange[6];
    uint8_t page[34];
    unsigned int i;

    g_assert_false(NIC_RESET(qts));
    nic_write_byte(qts, 0xcc);

    exchange[0] = 0xa5;
    exchange[1] = 0x40;                 /* the first page after the image */
    exchange[2] = 0x00;
    for (i = 0; i < 3; i++) {
        nic_write_byte(qts, exchange[i]);
    }

    for (i = 3; i < 6; i++) {
        exchange[i] = nic_read_byte(qts);
    }
    g_assert_cmphex(exchange[3], ==, 0xff);
    g_assert_cmphex(nic_crc(0, exchange, 6), ==, NIC_CRC_RESIDUE);

    for (i = 0; i < sizeof(page); i++) {
        page[i] = nic_read_byte(qts);
    }
    for (i = 0; i < 32; i++) {
        g_assert_cmphex(page[i], ==, 0xff);
    }
    g_assert_cmphex(nic_crc(0, page, sizeof(page)), ==, NIC_CRC_RESIDUE);

    qtest_quit(qts);
}

/*
 * A plain read that runs through the last byte of the memory is followed
 * by a check over the request and every byte read, the one check the
 * unframed path has, and the reason a guest can trust an unframed read at
 * all. Started near the top so the exchange is short.
 */
static void test_the_unframed_read_ends_with_its_check(const void *data)
{
    uint8_t exchange[13];
    unsigned int i;

    bridge_reset();

    g_assert_false(NIC_RESET(qts));
    nic_write_byte(qts, 0xcc);

    exchange[0] = 0xf0;
    exchange[1] = 0xf8;                 /* 0x07f8: the last eight bytes */
    exchange[2] = 0x07;
    for (i = 0; i < 3; i++) {
        nic_write_byte(qts, exchange[i]);
    }

    for (i = 3; i < 11; i++) {
        exchange[i] = nic_read_byte(qts);
        g_assert_cmphex(exchange[i], ==, 0xff);
    }
    exchange[11] = nic_read_byte(qts);
    exchange[12] = nic_read_byte(qts);
    g_assert_cmphex(nic_crc(0, exchange, sizeof(exchange)), ==,
                    NIC_CRC_RESIDUE);
}

static void test_a_reset_finds_the_part(const void *data)
{
    bridge_reset();

    /* The width mainline holds the wire down for to reset it. */
    qtest_writel(qts, REG(BRIDGE_NIC), NIC_PULSE(520) | NIC_SAMPLE(65));
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_NIC)) & NIC_LEVEL, ==, 0);

    /* And the width it uses to read a bit, which nothing answers yet. */
    qtest_writel(qts, REG(BRIDGE_NIC), NIC_PULSE(6) | NIC_SAMPLE(13));
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_NIC)) & NIC_LEVEL, ==,
                    NIC_LEVEL);
}

static void bridge_source_in_slot_three(QTestState *qts)
{
    qtest_writel(qts, CFG0(INT_TEST_SLOT, 0) + 0x10,
                 CARD_MMIO(INT_TEST_SLOT));
    qtest_writel(qts, CFG0(INT_TEST_SLOT, 0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY);
    qtest_writel(qts, REG(BRIDGE_DEVICE(INT_TEST_SLOT)),
                 DEV_MEMORY_SPACE | (CARD_MMIO(INT_TEST_SLOT) >> 20));
}

static void bridge_drive_pin(QTestState *qts, int level)
{
    qtest_writel(qts, WINDOW(INT_TEST_SLOT) + DMA_REG_INTERRUPT, level);
}

static void test_the_status_register_follows_the_level(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -device sgi-bridge-dma-test,addr=3");
    unsigned int i;

    bridge_source_in_slot_three(qts);

    /* Nothing is asserting, so nothing reads set. */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==, 0);

    bridge_drive_pin(qts, 1);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==,
                    1u << INT_TEST_SLOT);

    /*
     * The pin bit must survive repeated reads: guests re-arm by re-reading,
     * and a latch-and-clear model passes one read and hangs the machine.
     */
    for (i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==,
                        1u << INT_TEST_SLOT);
    }

    /*
     * And writing the register does not clear it either. A chip whose status
     * could be written would give a guest a way to disagree with its own
     * hardware about whether a device is still asking to be serviced.
     */
    qtest_writel(qts, REG(BRIDGE_INT_STATUS), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==,
                    1u << INT_TEST_SLOT);

    /* Only the device dropping its level clears it. */
    bridge_drive_pin(qts, 0);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==, 0);

    qtest_quit(qts);
}

/*
 * One pin has two sources: a routed INTx and a directly wired line. Each
 * must hold the pin on its own: a last-writer-wins level word drops a
 * held PCI level the moment the wire beside it falls.
 */
static void test_the_pin_ors_its_wire_and_pci_sources(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -device sgi-bridge-dma-test,addr=3");

    bridge_source_in_slot_three(qts);

    /* The PCI function holds its level. */
    bridge_drive_pin(qts, 1);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==,
                    1u << INT_TEST_SLOT);

    /* The wire beside it rises and falls; the held PCI level stays. */
    qtest_set_irq_in(qts, BRIDGE_PATH, NULL, INT_TEST_SLOT, 1);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==,
                    1u << INT_TEST_SLOT);
    qtest_set_irq_in(qts, BRIDGE_PATH, NULL, INT_TEST_SLOT, 0);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==,
                    1u << INT_TEST_SLOT);

    /* And the wire holds after the function lets go. */
    qtest_set_irq_in(qts, BRIDGE_PATH, NULL, INT_TEST_SLOT, 1);
    bridge_drive_pin(qts, 0);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==,
                    1u << INT_TEST_SLOT);

    /* Both down, the pin is down. */
    qtest_set_irq_in(qts, BRIDGE_PATH, NULL, INT_TEST_SLOT, 0);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==, 0);

    qtest_quit(qts);
}

/* Route the test pin at node 0 with a number in the guest range. */
static void bridge_route_pin(QTestState *qts)
{
    qtest_writel(qts, REG(BRIDGE_INT_HOST(INT_TEST_SLOT)),
                 (INT_TEST_HOST_NODE << 8) | INT_TEST_HOST_NUMBER);
}

static bool hub_pends_the_test_number(QTestState *qts)
{
    uint64_t pend = qtest_readq(qts, HUB_LOCAL_WINDOW + PI_INT_PEND0);

    return (pend & (1ULL << INT_TEST_HOST_NUMBER)) != 0;
}

/*
 * End to end: a routed, enabled pin edge sets the number's bit in the
 * Hub's pending bank. Nothing else joins the two register suites.
 */
static void test_an_enabled_pin_edge_reaches_the_hub(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -device sgi-bridge-dma-test,addr=3");

    bridge_source_in_slot_three(qts);
    bridge_route_pin(qts);
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 1u << INT_TEST_SLOT);

    /* Nothing has fired yet. */
    g_assert_false(hub_pends_the_test_number(qts));

    bridge_drive_pin(qts, 1);
    g_assert_true(hub_pends_the_test_number(qts));

    qtest_quit(qts);
}

/*
 * A routed pin with its enable clear sends nothing. Linux
 * leaves a disabled pin in, with the host address still programmed.
 */
static void test_a_disabled_pin_edge_sends_no_packet(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -device sgi-bridge-dma-test,addr=3");

    bridge_source_in_slot_three(qts);
    bridge_route_pin(qts);
    /* INT_ENABLE deliberately left at its reset value of zero. */

    bridge_drive_pin(qts, 1);
    g_assert_false(hub_pends_the_test_number(qts));

    qtest_quit(qts);
}

/* Enabling a pin that is already high must deliver its pending request. */
static void test_enabling_an_asserted_pin_reaches_the_hub(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -device sgi-bridge-dma-test,addr=3");

    bridge_source_in_slot_three(qts);
    bridge_route_pin(qts);

    bridge_drive_pin(qts, 1);
    g_assert_false(hub_pends_the_test_number(qts));

    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 1u << INT_TEST_SLOT);
    g_assert_true(hub_pends_the_test_number(qts));

    qtest_writeq(qts, HUB_LOCAL_WINDOW + PI_INT_PEND_MOD,
                 INT_TEST_HOST_NUMBER);
    g_assert_false(hub_pends_the_test_number(qts));

    /* Rewriting the same enable value must not replay the request. */
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 1u << INT_TEST_SLOT);
    g_assert_false(hub_pends_the_test_number(qts));

    /* A new disabled-to-enabled transition replays the held request once. */
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 0);
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 1u << INT_TEST_SLOT);
    g_assert_true(hub_pends_the_test_number(qts));

    qtest_quit(qts);
}

/*
 * With the mode bit set the falling edge sends a clearing packet, so the
 * guest need not clear the Hub bit in its own handler. Without this the
 * Hub's pending bit never clears and the processor re-enters the handler
 * forever.
 */
static void test_a_falling_edge_clears_the_hub_pending_bit(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -device sgi-bridge-dma-test,addr=3");

    bridge_source_in_slot_three(qts);
    bridge_route_pin(qts);
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 1u << INT_TEST_SLOT);
    qtest_writel(qts, REG(BRIDGE_INT_MODE), 1u << INT_TEST_SLOT);

    bridge_drive_pin(qts, 1);
    g_assert_true(hub_pends_the_test_number(qts));

    bridge_drive_pin(qts, 0);
    g_assert_false(hub_pends_the_test_number(qts));

    qtest_quit(qts);
}

static void test_a_disabled_falling_edge_sends_no_clear(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -device sgi-bridge-dma-test,addr=3");

    bridge_source_in_slot_three(qts);
    bridge_route_pin(qts);
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 1u << INT_TEST_SLOT);
    qtest_writel(qts, REG(BRIDGE_INT_MODE), 1u << INT_TEST_SLOT);

    bridge_drive_pin(qts, 1);
    g_assert_true(hub_pends_the_test_number(qts));

    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 0);
    bridge_drive_pin(qts, 0);
    g_assert_true(hub_pends_the_test_number(qts));

    qtest_quit(qts);
}

static void test_the_enable_register_has_no_bit_31(const void *data)
{
    bridge_reset();

    /*
     * The multiple-error flag has no enable; the table starts at bit 30.
     * The bit must refuse to be set at all.
     */
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_ENABLE)), ==, 0x7fffffff);
}

static void test_mode_and_device_keep_what_was_written(const void *data)
{
    unsigned int n;

    bridge_reset();

    /*
     * Established one at a time and checked at the end: a read-as-zero
     * model wipes every earlier pin.
     */
    for (n = 0; n < BRIDGE_DEVICES; n++) {
        uint32_t mode = qtest_readl(qts, REG(BRIDGE_INT_MODE));
        uint32_t device = qtest_readl(qts, REG(BRIDGE_INT_DEVICE));

        qtest_writel(qts, REG(BRIDGE_INT_MODE), mode | (1u << n));
        qtest_writel(qts, REG(BRIDGE_INT_DEVICE), device | (n << (3 * n)));
    }

    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_MODE)), ==, 0xff);
    for (n = 0; n < BRIDGE_DEVICES; n++) {
        g_assert_cmpuint((qtest_readl(qts, REG(BRIDGE_INT_DEVICE)) >> (3 * n))
                         & 7, ==, n);
    }

    /* Three bits per pin and eight pins, so the top byte stays clear. */
    qtest_writel(qts, REG(BRIDGE_INT_DEVICE), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_DEVICE)), ==, 0xffffff);
}

static void test_the_per_pin_host_registers(const void *data)
{
    unsigned int n;

    bridge_reset();

    /*
     * All eight written before any is read, so one register serving all
     * eight pins fails rather than passing on the last.
     */
    for (n = 0; n < BRIDGE_DEVICES; n++) {
        qtest_writel(qts, REG(BRIDGE_INT_HOST(n)), 0xffffffff);
    }
    for (n = 0; n < BRIDGE_DEVICES; n++) {
        /* Bits 31:18 are reserved, so all ones reads back as the two
         * fields. */
        g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_HOST(n))), ==, 0x3ffff);
    }

    for (n = 0; n < BRIDGE_DEVICES; n++) {
        qtest_writel(qts, REG(BRIDGE_INT_HOST(n)), 0x100 + n);
    }
    for (n = 0; n < BRIDGE_DEVICES; n++) {
        g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_HOST(n))), ==,
                        0x100 + n);
    }
}

/*
 * The four-onto-eight fold: A and C reach the slot's pin, B and D flip
 * bit two. Both guests compute it; nothing that boots asserts B or C.
 */
static void test_the_interrupt_pins_fold(const void *data)
{
    static const struct {
        const char *pin;        /* what the card declares, A through D */
        int slot;
        int expect;             /* the chip pin it must reach */
    } fold[] = {
        { "1", 3, 3 },          /* A -> the slot */
        { "2", 3, 7 },          /* B -> the slot with bit two flipped */
        { "3", 3, 3 },          /* C -> the slot again */
        { "4", 3, 7 },          /* D -> flipped again */
        { "2", 6, 2 },          /* and the flip runs downward too */
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(fold); i++) {
        g_autofree char *cmd =
            g_strdup_printf("%s -device sgi-bridge-dma-test,addr=%d,"
                            "interrupt-pin=%s", MACHINE_WITH_BRIDGE,
                            fold[i].slot, fold[i].pin);
        QTestState *qts = qtest_init(cmd);

        /*
         * Reach the card through its own slot's window and raise its pin. The
         * transfer source keeps its registers in the first base address
         * register, not the second the probe card uses.
         */
        qtest_writel(qts, CFG0(fold[i].slot, 0) + PCI_BAR0,
                     CARD_MMIO(fold[i].slot));
        qtest_writel(qts, CFG0(fold[i].slot, 0) + PCI_COMMAND,
                     PCI_COMMAND_MEMORY);
        qtest_writel(qts, REG(BRIDGE_DEVICE(fold[i].slot)),
                     DEV_MEMORY_SPACE | (CARD_MMIO(fold[i].slot) >> 20));

        g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==, 0);

        qtest_writel(qts, WINDOW(fold[i].slot) + DMA_REG_INTERRUPT, 1);

        /*
         * The whole status word, not the expected bit alone. A model that lit
         * the right pin *and* another would pass a mask check and would be
         * delivering one device's interrupt to two handlers.
         */
        g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==,
                        1u << fold[i].expect);

        qtest_quit(qts);
    }
}

/*
 * The crossbar's per-link reset, arriving. Revision D drives the chip's
 * core-logic reset from it, and IRIX rebuilds the chip afterwards rather
 * than trusting anything it had programmed.
 */
#define ATE_TEST_ENTRY          5

static void bridge_link_reset(QTestState *qts)
{
    qtest_set_irq_in(qts, BRIDGE_PATH, "link-reset", 0, 1);
    qtest_set_irq_in(qts, BRIDGE_PATH, "link-reset", 0, 0);
}

static void test_an_inbound_link_reset_clears_the_core(const void *data)
{
    bridge_reset();

    /*
     * One register per group of the file, every one away from its reset,
     * and a translation entry beside them. All written before any is read,
     * so a model that reset one register and left the rest fails here
     * rather than on whichever assertion happens to come last.
     */
    qtest_writel(qts, REG(BRIDGE_CONTROL), 0);
    qtest_writel(qts, REG(BRIDGE_REQUEST_TIMEOUT), 0);
    qtest_writel(qts, REG(BRIDGE_LINK_CONFIG), 0);
    qtest_writel(qts, REG(BRIDGE_DIRECT_MAP), 0);
    qtest_writel(qts, REG(BRIDGE_ARBITRATION), 0);
    qtest_writel(qts, REG(BRIDGE_BUS_TIMEOUT), 0);
    qtest_writel(qts, REG(BRIDGE_DEVICE(PROBE_SLOT)), 0);
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 0xff);
    qtest_writel(qts, REG(BRIDGE_INT_DEVICE), 0xffffff);
    qtest_writel(qts, REG(BRIDGE_INT_HOST(0)), 0x100);
    qtest_writeq(qts, ATE(ATE_TEST_ENTRY), 0xa5a5000012345678ull);

    /* The core goes when the line rises, not when the pulse ends. */
    qtest_set_irq_in(qts, BRIDGE_PATH, "link-reset", 0, 1);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_CONTROL)), ==, CONTROL_RESET);
    qtest_set_irq_in(qts, BRIDGE_PATH, "link-reset", 0, 0);

    /*
     * The two registers IRIX saves across the pulse are the two it cannot
     * recompute, so they are exactly the ones that have to go: a model
     * that kept either makes that save and restore pointless.
     */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_CONTROL)), ==, CONTROL_RESET);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_LINK_CONFIG)), ==,
                    LINK_CONFIG_RESET);

    /* And the rest, which IRIX reinitializes rather than saving. */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_REQUEST_TIMEOUT)), ==,
                    REQUEST_TIMEOUT_RESET);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DIRECT_MAP)), ==,
                    DIRECT_MAP_RESET);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_ARBITRATION)), ==,
                    ARBITRATION_RESET);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_BUS_TIMEOUT)), ==,
                    BUS_TIMEOUT_RESET);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(PROBE_SLOT))), ==,
                    device_reset[PROBE_SLOT]);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_ENABLE)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_DEVICE)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_HOST(0))), ==, 0);

    /* The whole entry, the way a cold reset clears it. */
    g_assert_cmphex(qtest_readl(qts, ATE_UPPER(ATE_TEST_ENTRY)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, ATE_LOW(ATE_TEST_ENTRY)), ==, 0);

    /* Composed on every reset, not a table row, so it is still right. */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_IDENTIFICATION)), ==,
                    bridge_identity(BRIDGE_REVISION));
}

/*
 * And it stops at the chip. No source documents Bridge pulsing its four
 * external bus-reset outputs or a downstream device resetting, so a card
 * keeps its own programming; and the pin it is holding stays up, because a
 * level cleared here would never rise again and the device would go on
 * asking without being heard.
 */
static void test_an_inbound_link_reset_stops_at_the_pci_side(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -device sgi-bridge-dma-test,addr=3");

    bridge_source_in_slot_three(qts);
    bridge_drive_pin(qts, 1);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==,
                    1u << INT_TEST_SLOT);

    bridge_link_reset(qts);

    /* The card is untouched: its base address and its command still stand. */
    g_assert_cmphex(qtest_readl(qts, CFG0(INT_TEST_SLOT, 0) + PCI_BAR0), ==,
                    CARD_MMIO(INT_TEST_SLOT));
    g_assert_cmphex(qtest_readl(qts, CFG0(INT_TEST_SLOT, 0) + PCI_COMMAND) &
                    0xffff, ==, PCI_COMMAND_MEMORY);

    /* And the pin it is still driving reads set. */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==,
                    1u << INT_TEST_SLOT);

    /*
     * The slot's window went with the rest of the core programming, so
     * reaching the card again takes a fresh aim, and then the card, which
     * never reset, is the only thing that can drop the pin.
     */
    qtest_writel(qts, REG(BRIDGE_DEVICE(INT_TEST_SLOT)),
                 DEV_MEMORY_SPACE | (CARD_MMIO(INT_TEST_SLOT) >> 20));
    bridge_drive_pin(qts, 0);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==, 0);

    qtest_quit(qts);
}

/* Shared by the widget-addressing and PIO-translated-window cases below. */
#define BRIDGE          "/machine/peripheral/bridge"
#define BOARD_PART      "030-1244-001"
#define CARD_SLOT       0

/*
 * The chip's crosstalk widget-number addressing, exercised on a head built
 * here out of test-only parts rather than the machine above: a QEMU address
 * space is flat with no widget field, so a machine with more than one peer
 * to reach spends four bits of the address on the widget number the real
 * chip carries in its packet header, and a machine with a single peer
 * spends none. The two arrangements differ by that one property alone.
 */
/* The bridge's own 16 MB widget face, put clear of every target below. */
#define WIDGET_BRIDGE_BASE     0x40000000ULL

/*
 * Registers sit at a slot's offset plus four: Linux names that address,
 * OpenBSD names the slot and folds the four in.
 */
#define WIDGET_REG(offset)     (WIDGET_BRIDGE_BASE + (offset) + 4)

#define BRIDGE_INT_DEST_UPPER   0x030
#define BRIDGE_INT_DEST_LOWER   0x038
#define BRIDGE_HOST_ERR_FIELD   0x128

/* An undefined slot inside the register block: the invalid-address
 * provoke. */
#define UNDEFINED_REG           0x07c
#define INT_INVALID_ADDR        (1u << 24)

/* The windows the chip decodes above its register block. */
#define WIDGET_ATE(n)          (WIDGET_BRIDGE_BASE + 0x010000 + (n) * 8)
#define WIDGET_CFG0(slot)      (WIDGET_BRIDGE_BASE + 0x020000 + (slot) * 0x1000)
#define WIDGET_WINDOW0         (WIDGET_BRIDGE_BASE + 0x200000)

/* The transfer source, its registers, and where they are put in PCI space. */
#define WIDGET_CARD_MMIO               0x01000000u
#define PCI_COMMAND_MEMORY_MASTER 0x0006

/* Enough of the identity register to say a head came up and answered. */
#define ID_PART                 0xc002

/*
 * The fabric this head writes into is plain system memory, so an address the
 * chip composes is an address these tests can look at. With no widget field
 * in it every transfer lands at its bare offset, so one target with room for
 * all three of them stands in for the whole fabric.
 */
#define PLAIN_BASE      0x00010000ULL

/*
 * The three transfer routes. Each names a widget of its own and lands at an
 * offset of its own, so a landing says which route put it there and whether
 * the widget it named changed anything.
 */
#define DIRECT_WIDGET   3
#define ATE_WIDGET      5
#define WIDE_WIDGET     0xb
#define DIRECT_OFF      0x18000
#define ATE_OFF         0x19000
#define WIDE_OFF        0x1a000

/* Packet routing keeps all 48 address bits below the widget field. */
#define WIDGET_SHIFT    48

/*
 * The Hub window an interrupt packet is addressed into with no widget field,
 * and the node stride its destination's node number is multiplied by.
 */
#define HUB_WINDOW      0x01800000ULL
#define HUB_INT_MODIFY  0x90
#define NODE_SHIFT      32
#define NODE1_BASE      (HUB_WINDOW + (1ULL << NODE_SHIFT))

/* Where the interrupt packets land, clear of the transfers above. */
#define PIN_OFF         0x02000
#define ERR_OFF         0x1c000

/*
 * The destination's upper register holds crosstalk address bits 47:32. Its
 * top ten are the ones a pin's own host-address register replaces, so the
 * tests fill those to tell replacing from merging, and set one below them so
 * a dropped upper half is a wrong address rather than the same one. Both sit
 * clear of the widget field at either shift.
 */
#define DEST_REPLACED   0xffc0u         /* upper 15:6, address 47:38 */
#define DEST_CARRIED    0x0002u         /* upper 1, address 33 */
#define DEST_UPPER      (DEST_REPLACED | DEST_CARRIED)
#define CARRIED_BIT     (1ULL << 33)
#define REPLACED_BIT    (1ULL << 38)

/* The pin these tests drive, and the bits its packets carry. */
#define PIN             3
#define PIN_NUMBER      0x45    /* the host bit a device interrupt sets */
#define ERR_NUMBER      0x53    /* and the one the chip's own error sets */
#define INT_SET         0x100   /* data bit 8: raise rather than clear */

/* A word nothing here writes, so "no packet" is an assertion of its own. */
#define SENTINEL        0xfeedfacecafebeefULL

#define DMA_LEN         16
#define DIRECT_SEED     0x30
#define ATE_SEED        0x50
#define WIDE_SEED       0x70

/*
 * Where the head under test keeps a widget number. head_start_with is its
 * only writer, so every address below is composed the way that head does.
 */
static unsigned int fabric_shift;

static uint64_t target(unsigned int widget)
{
    return (uint64_t)widget << fabric_shift;
}

/*
 * The two places a composed destination lands: the address the guest
 * programmed under its own widget, and the same with the top ten address
 * bits carrying what a pin's host-address register put there.
 */
static uint64_t dest_base(void)
{
    return target(WIDE_WIDGET) + CARRIED_BIT;
}

static uint64_t replaced_base(void)
{
    return dest_base() + REPLACED_BIT;
}

/*
 * The head, and whatever else the caller wants on the command line. A zero
 * shift asks for no widget field at all; the targets are laid out as though
 * one were there regardless, so a test can say a transfer did not reach them.
 */
static QTestState *head_start_with(unsigned int widget_shift, const char *extra)
{
    g_autofree char *option = widget_shift ?
        g_strdup_printf(",fabric-widget-shift=%u", widget_shift) : g_strdup("");
    QTestState *qts;

    fabric_shift = widget_shift ? widget_shift : WIDGET_SHIFT;
    qts = qtest_initf("-M none -cpu R10000 -S "
        "-device sgi-xtalk-test-target,id=plain,size=1M "
        "-device sgi-xtalk-test-target,id=hub,size=1M "
        "-device sgi-xtalk-test-target,id=node1,size=1M "
        "-device sgi-xtalk-test-target,id=w3,size=1M "
        "-device sgi-xtalk-test-target,id=w5,size=1M "
        "-device sgi-xtalk-test-target,id=wb,size=1M "
        "-device sgi-xtalk-test-target,id=pin,size=1M "
        "-device sgi-xtalk-test-target,id=replaced,size=1M "
        "-device sgi-bridge,id=bridge,board-part=" BOARD_PART ","
        "fabric=/machine/unattached/system[0],"
        "fabric-node-shift=" stringify(NODE_SHIFT) "%s "
        "-device sgi-bridge-dma-test,bus=pci,addr=%d %s",
        option, CARD_SLOT, extra);

    qtest_sysbus_mmio_map(qts, BRIDGE, 0, WIDGET_BRIDGE_BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/plain", 0, PLAIN_BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/hub", 0, HUB_WINDOW);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/node1", 0, NODE1_BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/w3", 0,
                          target(DIRECT_WIDGET));
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/w5", 0,
                          target(ATE_WIDGET));
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/wb", 0,
                          target(WIDE_WIDGET));
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/pin", 0, dest_base());
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/replaced", 0,
                          replaced_base());
    return qts;
}

static QTestState *head_start(unsigned int widget_shift)
{
    return head_start_with(widget_shift, "");
}

/*
 * Give the card its registers, let it master the bus, and aim slot 0's
 * window at them. Both byte-lane bits are set so a transfer carries the
 * bytes it was given: what these tests read is the address, not the order.
 */
static void widget_card_start(QTestState *qts)
{
    qtest_writel(qts, WIDGET_CFG0(CARD_SLOT) + PCI_BAR0, WIDGET_CARD_MMIO);
    qtest_writel(qts, WIDGET_CFG0(CARD_SLOT) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY_MASTER);
    qtest_writel(qts, WIDGET_REG(BRIDGE_DEVICE(CARD_SLOT)),
                 DEV_SWAP_TRANSLATED | DEV_SWAP_DIRECT | DEV_MEMORY_SPACE |
                 (WIDGET_CARD_MMIO >> 20));
}

/*
 * The direct window takes its widget from bits 23:20 of the direct map
 * register; with the offset field zero and the 512 MB step cleared a PCI
 * address passes through as its own low 31 bits. The one translation entry
 * names a page, a widget of its own, and is marked valid.
 */
static void aim_the_routes(QTestState *qts)
{
    qtest_writel(qts, WIDGET_REG(BRIDGE_DIRECT_MAP), DIRECT_WIDGET << 20);
    qtest_writeq(qts, WIDGET_ATE(0),
                 (uint64_t)ATE_OFF | ((uint64_t)ATE_WIDGET << 8) | 1);
}

static void widget_card_transfer(QTestState *qts, uint64_t pci_addr, uint32_t seed)
{
    qtest_writel(qts, WIDGET_WINDOW0 + DMA_REG_ADDRESS, (uint32_t)pci_addr);
    qtest_writel(qts, WIDGET_WINDOW0 + DMA_REG_ADDRESS_HI, pci_addr >> 32);
    qtest_writel(qts, WIDGET_WINDOW0 + DMA_REG_LENGTH, DMA_LEN);
    qtest_writel(qts, WIDGET_WINDOW0 + DMA_REG_SEED, seed);
    qtest_writel(qts, WIDGET_WINDOW0 + DMA_REG_GO, 1);
}

/* One transfer down each of the three routes, at a seed apiece. */
static void run_the_routes(QTestState *qts)
{
    widget_card_transfer(qts, 0x80000000ULL | DIRECT_OFF, DIRECT_SEED);
    widget_card_transfer(qts, 0x40000000ULL, ATE_SEED);
    widget_card_transfer(qts, ((uint64_t)WIDE_WIDGET << 60) | WIDE_OFF, WIDE_SEED);
}

static void assert_landed(QTestState *qts, uint64_t at, uint32_t seed)
{
    uint8_t seen[DMA_LEN];
    unsigned int i;

    qtest_memread(qts, at, seen, sizeof(seen));
    for (i = 0; i < sizeof(seen); i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(seed + i));
    }
}

static void assert_untouched(QTestState *qts, uint64_t at)
{
    uint8_t seen[DMA_LEN];
    unsigned int i;

    qtest_memread(qts, at, seen, sizeof(seen));
    for (i = 0; i < sizeof(seen); i++) {
        g_assert_cmpuint(seen[i], ==, 0);
    }
}

/*
 * The three windows a device transfer can take, on a head that keeps no
 * widget field: each lands at its bare offset and the widget each of them
 * names changes nothing about where it went.
 */
static void test_transfers_land_at_their_offset(void)
{
    QTestState *qts = head_start(0);

    widget_card_start(qts);
    aim_the_routes(qts);
    run_the_routes(qts);

    assert_landed(qts, DIRECT_OFF, DIRECT_SEED);
    assert_landed(qts, ATE_OFF, ATE_SEED);
    assert_landed(qts, WIDE_OFF, WIDE_SEED);

    assert_untouched(qts, target(DIRECT_WIDGET) + DIRECT_OFF);
    assert_untouched(qts, target(ATE_WIDGET) + ATE_OFF);
    assert_untouched(qts, target(WIDE_WIDGET) + WIDE_OFF);

    qtest_quit(qts);
}

/*
 * The same three transfers with a widget field in the address: each reaches
 * the widget its own route named, and only that one.
 */
static void assert_transfers_carry_their_widget(unsigned int shift)
{
    QTestState *qts = head_start(shift);

    widget_card_start(qts);
    aim_the_routes(qts);
    run_the_routes(qts);

    assert_landed(qts, target(DIRECT_WIDGET) + DIRECT_OFF, DIRECT_SEED);
    assert_landed(qts, target(ATE_WIDGET) + ATE_OFF, ATE_SEED);
    assert_landed(qts, target(WIDE_WIDGET) + WIDE_OFF, WIDE_SEED);

    /* Nothing landed at the bare offset, and no route reached another's. */
    assert_untouched(qts, DIRECT_OFF);
    assert_untouched(qts, ATE_OFF);
    assert_untouched(qts, WIDE_OFF);
    assert_untouched(qts, target(DIRECT_WIDGET) + ATE_OFF);
    assert_untouched(qts, target(ATE_WIDGET) + WIDE_OFF);
    assert_untouched(qts, target(WIDE_WIDGET) + DIRECT_OFF);

    qtest_quit(qts);
}

static void test_transfers_carry_their_target_widget(void)
{
    assert_transfers_carry_their_widget(WIDGET_SHIFT);
}

/* And at the shift the machine's own address map uses. */
/*
 * One pin's own host address: the bit it is to set, under a field that
 * selects a node on a head with no widget field and replaces the
 * destination's top ten address bits on one that has.
 */
static void route_the_pin(QTestState *qts, unsigned int address)
{
    qtest_writel(qts, WIDGET_REG(BRIDGE_INT_HOST(PIN)), (address << 8) | PIN_NUMBER);
}

/* The widget-level destination, upper address bits and all. */
static void aim_the_destination(QTestState *qts, unsigned int widget,
                                unsigned int upper, uint32_t lower)
{
    qtest_writel(qts, WIDGET_REG(BRIDGE_INT_DEST_UPPER), (widget << 16) | upper);
    qtest_writel(qts, WIDGET_REG(BRIDGE_INT_DEST_LOWER), lower);
}

static void drive_the_pin(QTestState *qts, int level)
{
    qtest_set_irq_in(qts, BRIDGE, NULL, PIN, level);
}

/*
 * A pin's packet is addressed into the Hub window of the node its own
 * host-address register names, and carries that register's bit number with
 * data bit 8 set.
 */
static void test_a_pin_packet_addresses_the_named_node(void)
{
    QTestState *qts = head_start(0);

    route_the_pin(qts, 0);
    qtest_writel(qts, WIDGET_REG(BRIDGE_INT_ENABLE), 1u << PIN);

    /* Nothing has been sent yet. */
    g_assert_cmphex(qtest_readq(qts, HUB_WINDOW + HUB_INT_MODIFY), ==, 0);

    drive_the_pin(qts, 1);
    g_assert_cmphex(qtest_readq(qts, HUB_WINDOW + HUB_INT_MODIFY), ==,
                    INT_SET | PIN_NUMBER);

    /* And the status register follows the pin rather than the packet. */
    g_assert_cmphex(qtest_readl(qts, WIDGET_REG(BRIDGE_INT_STATUS)) & (1u << PIN),
                    ==, 1u << PIN);

    qtest_quit(qts);
}

/* The node the register names is multiplied by the node stride, not
 * ignored. */
static void test_a_pin_host_address_selects_a_node(void)
{
    QTestState *qts = head_start(0);

    route_the_pin(qts, 1);
    qtest_writel(qts, WIDGET_REG(BRIDGE_INT_ENABLE), 1u << PIN);

    drive_the_pin(qts, 1);
    g_assert_cmphex(qtest_readq(qts, NODE1_BASE + HUB_INT_MODIFY), ==,
                    INT_SET | PIN_NUMBER);
    g_assert_cmphex(qtest_readq(qts, HUB_WINDOW + HUB_INT_MODIFY), ==, 0);

    qtest_quit(qts);
}

/*
 * The chip's own error interrupt takes its number from the host-error field
 * and its address from the widget-level destination pair. The eight pin
 * registers are the devices' destinations, so one is programmed here to a
 * different node and number and has to stay unused.
 */
static void provoke_the_error(QTestState *qts, unsigned int widget,
                              unsigned int upper)
{
    aim_the_destination(qts, widget, upper, ERR_OFF);
    qtest_writel(qts, WIDGET_REG(BRIDGE_HOST_ERR_FIELD), ERR_NUMBER);
    route_the_pin(qts, 1);
    qtest_writel(qts, WIDGET_REG(BRIDGE_INT_ENABLE), INT_INVALID_ADDR);

    /* A write to an undefined slot in the register block posts and
     * reports. */
    qtest_writel(qts, WIDGET_BRIDGE_BASE + UNDEFINED_REG, 0);
    g_assert_cmphex(qtest_readl(qts, WIDGET_REG(BRIDGE_INT_STATUS)) &
                    INT_INVALID_ADDR, ==, INT_INVALID_ADDR);
}

static void test_a_chip_error_uses_the_widget_destination(void)
{
    QTestState *qts = head_start(0);

    provoke_the_error(qts, 0, 0);
    g_assert_cmphex(qtest_readq(qts, ERR_OFF), ==, INT_SET | ERR_NUMBER);

    /* Not the pin register's node, and not the pin register's number. */
    g_assert_cmphex(qtest_readq(qts, NODE1_BASE + HUB_INT_MODIFY), ==, 0);

    qtest_quit(qts);
}

/*
 * With a widget field in the address, a pin's packet goes to the widget-level
 * destination under the target widget that pair names, carrying the pin's own
 * bit number with data bit 8 set.
 */
static void test_a_pin_packet_carries_its_target_widget(void)
{
    QTestState *qts = head_start(WIDGET_SHIFT);

    aim_the_destination(qts, WIDE_WIDGET, DEST_UPPER, PIN_OFF);
    route_the_pin(qts, 0);
    qtest_writel(qts, WIDGET_REG(BRIDGE_INT_ENABLE), 1u << PIN);

    /* Nothing has been sent yet. */
    g_assert_cmphex(qtest_readq(qts, dest_base() + PIN_OFF), ==, 0);

    drive_the_pin(qts, 1);
    g_assert_cmphex(qtest_readq(qts, dest_base() + PIN_OFF), ==,
                    INT_SET | PIN_NUMBER);

    /* The status register follows the pin whichever way the packet went. */
    g_assert_cmphex(qtest_readl(qts, WIDGET_REG(BRIDGE_INT_STATUS)) & (1u << PIN),
                    ==, 1u << PIN);

    /* And the Hub window a head with no widget field addresses is not it. */
    g_assert_cmphex(qtest_readq(qts, HUB_WINDOW + HUB_INT_MODIFY), ==, 0);

    qtest_quit(qts);
}

/* The pin's host-address bits replace the destination's, they do not join. */
static void test_a_pin_host_address_replaces_the_destination_bits(void)
{
    QTestState *qts = head_start(WIDGET_SHIFT);

    aim_the_destination(qts, WIDE_WIDGET, DEST_UPPER, PIN_OFF);
    route_the_pin(qts, 1);
    qtest_writel(qts, WIDGET_REG(BRIDGE_INT_ENABLE), 1u << PIN);

    drive_the_pin(qts, 1);
    g_assert_cmphex(qtest_readq(qts, replaced_base() + PIN_OFF), ==,
                    INT_SET | PIN_NUMBER);
    g_assert_cmphex(qtest_readq(qts, dest_base() + PIN_OFF), ==, 0);

    qtest_quit(qts);
}

/* A pin whose enable bit is clear sends nothing, however it is routed. */
static void test_a_masked_pin_sends_no_packet(void)
{
    QTestState *qts = head_start(WIDGET_SHIFT);

    aim_the_destination(qts, WIDE_WIDGET, DEST_UPPER, PIN_OFF);
    route_the_pin(qts, 0);
    /* The enable register is deliberately left at its reset value of zero. */
    qtest_writeq(qts, dest_base() + PIN_OFF, SENTINEL);

    drive_the_pin(qts, 1);
    g_assert_cmphex(qtest_readq(qts, dest_base() + PIN_OFF), ==, SENTINEL);

    /* The pin is up all the same; only delivery was masked. */
    g_assert_cmphex(qtest_readl(qts, WIDGET_REG(BRIDGE_INT_STATUS)) & (1u << PIN),
                    ==, 1u << PIN);

    qtest_quit(qts);
}

/*
 * With the mode bit set the falling edge sends a clearing packet to the same
 * address, told apart from the raise by data bit 8 alone.
 */
static void test_a_falling_pin_clears_through_the_same_address(void)
{
    QTestState *qts = head_start(WIDGET_SHIFT);

    aim_the_destination(qts, WIDE_WIDGET, DEST_UPPER, PIN_OFF);
    route_the_pin(qts, 0);
    qtest_writel(qts, WIDGET_REG(BRIDGE_INT_ENABLE), 1u << PIN);
    qtest_writel(qts, WIDGET_REG(BRIDGE_INT_MODE), 1u << PIN);

    drive_the_pin(qts, 1);
    g_assert_cmphex(qtest_readq(qts, dest_base() + PIN_OFF), ==,
                    INT_SET | PIN_NUMBER);

    drive_the_pin(qts, 0);
    g_assert_cmphex(qtest_readq(qts, dest_base() + PIN_OFF), ==, PIN_NUMBER);

    qtest_quit(qts);
}

/*
 * Without that bit the guest clears the host itself and the chip sends none.
 * The destination is overwritten between the two edges: the raise left the
 * exact word a second raise would write, and a cell still holding it cannot
 * tell nothing-sent from sent-again.
 */
static void test_a_falling_pin_sends_nothing_without_the_mode_bit(void)
{
    QTestState *qts = head_start(WIDGET_SHIFT);

    aim_the_destination(qts, WIDE_WIDGET, DEST_UPPER, PIN_OFF);
    route_the_pin(qts, 0);
    qtest_writel(qts, WIDGET_REG(BRIDGE_INT_ENABLE), 1u << PIN);
    /* The mode register is deliberately left at its reset value of zero. */

    drive_the_pin(qts, 1);
    g_assert_cmphex(qtest_readq(qts, dest_base() + PIN_OFF), ==,
                    INT_SET | PIN_NUMBER);

    qtest_writeq(qts, dest_base() + PIN_OFF, SENTINEL);
    drive_the_pin(qts, 0);
    g_assert_cmphex(qtest_readq(qts, dest_base() + PIN_OFF), ==, SENTINEL);

    qtest_quit(qts);
}

/* The chip's own error keeps taking the widget-level pair, not a pin's. */
static void test_a_chip_error_carries_the_widget_destination(void)
{
    QTestState *qts = head_start(WIDGET_SHIFT);

    qtest_writeq(qts, replaced_base() + ERR_OFF, SENTINEL);
    provoke_the_error(qts, WIDE_WIDGET, DEST_CARRIED);
    g_assert_cmphex(qtest_readq(qts, dest_base() + ERR_OFF), ==,
                    INT_SET | ERR_NUMBER);

    /* The address the programmed pin register would have made is
     * untouched. */
    g_assert_cmphex(qtest_readq(qts, replaced_base() + ERR_OFF), ==, SENTINEL);

    qtest_quit(qts);
}

/*
 * A widget number is four bits wide, so 60 is the furthest one can be shifted
 * and still be inside a 64-bit address. Past that the machine has to be
 * refused where it is built, rather than have the number fall off the top.
 * The refusal lands before there is a qtest socket to ask, and a sysbus
 * device cannot be added to a machine already running, so this one runs the
 * emulator itself and reads what it said on the way out.
 */
static void test_a_shift_past_the_address_is_refused(void)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "none,accel=qtest", "-display", "none",
        "-device", "sgi-bridge,board-part=" BOARD_PART
        ",fabric=/machine/unattached/system[0],fabric-widget-shift=61", NULL
    };
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                               NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_nonnull(strstr(error_output, "fabric widget shift 61"));
}

/* A valid head quits through QMP; invalid geometry must fail realization. */
static void assert_geometry_refused(const char *properties)
{
    g_autofree char *device = g_strdup_printf(
        "sgi-bridge,board-part=" BOARD_PART
        ",fabric=/machine/unattached/system[0],%s", properties);
    const char *args[] = {
        "/bin/sh", "-c",
        "printf '%s\n' '{\"execute\":\"qmp_capabilities\"}' "
        "'{\"execute\":\"quit\"}' | \"$@\"", "bridge-geometry",
        qtest_qemu_binary(NULL), "-M", "none,accel=qtest", "-display", "none",
        "-qmp", "stdio", "-device", device, NULL,
    };
    g_autofree char *error_output = NULL;
    g_autofree char *output = NULL;
    int status;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              &output, &error_output, &status, NULL));
    g_assert_false(g_spawn_check_exit_status(status, NULL));
    g_assert_nonnull(strstr(error_output, "packet fabric"));
}

static void test_packet_geometry(void)
{
    assert_geometry_refused("fabric-widget-shift=34");
    assert_geometry_refused("fabric-widget-shift=1");
    assert_geometry_refused("fabric-widget-shift=60");
    assert_geometry_refused("fabric-widget-shift=48,fabric-base=1");
    assert_geometry_refused("fabric-widget-shift=48,"
                            "fabric-base=0xffffffffffffffff");
}

/* All DMA decoders retain bit 47 and the last bytes of the 48-bit slot. */
static void test_full_packet_dma(void)
{
    unsigned plain, big, lane;

    for (plain = 0; plain < 2; plain++) {
        for (big = 0; big < 2; big++) {
            for (lane = 0; lane < 2; lane++) {
                g_autofree char *extra = g_strdup_printf(
                    "-device sgi-xtalk-test-target,id=full,size=1M "
                    "-global sgi-bridge-dma-test.plain-dma=%s",
                    plain ? "on" : "off");
                QTestState *qts = head_start_with(48, extra);
                uint64_t end = 1ULL << 48;
                uint64_t last = end - DMA_LEN;
                uint64_t page = big ? 16384 : 4096;
                uint64_t at = (15ULL << 48) | last;
                uint64_t routes[] = {
                    0xfffffff0ULL,
                    0x40000000ULL | (page - DMA_LEN),
                    (15ULL << 60) | last,
                };
                unsigned route, i;
                uint8_t bytes[DMA_LEN];

                qtest_sysbus_mmio_map(qts, "/machine/peripheral/full", 0,
                                     (15ULL << 48) | (end - (1ULL << 20)));
                widget_card_start(qts);
                if (lane) {
                    qtest_writel(qts, WIDGET_REG(BRIDGE_DEVICE(CARD_SLOT)),
                                 DEV_MEMORY_SPACE | (WIDGET_CARD_MMIO >> 20));
                }
                if (big) {
                    uint32_t control = qtest_readl(qts, WIDGET_REG(0x20));
                    qtest_writel(qts, WIDGET_REG(0x20), control | (1U << 21));
                }
                qtest_writel(qts, WIDGET_REG(BRIDGE_DIRECT_MAP),
                             (15U << 20) | 0x1ffff);
                qtest_writeq(qts, WIDGET_ATE(0), (end - page) | (15U << 8) | 1);
                for (route = 0; route < G_N_ELEMENTS(routes); route++) {
                    uint8_t seed = 0x40 + route * 0x20;
                    widget_card_transfer(qts, routes[route], seed);
                    qtest_memread(qts, at, bytes, sizeof(bytes));
                    for (i = 0; i < sizeof(bytes); i++) {
                        unsigned index = lane && !plain ? i ^ 3 : i;
                        g_assert_cmpuint(bytes[i], ==, seed + index);
                    }
                    /* Read through the same decoder into a distinct range. */
                    qtest_writel(qts, WIDGET_WINDOW0 + DMA_REG_SOURCE,
                                 (uint32_t)routes[route]);
                    qtest_writel(qts, WIDGET_WINDOW0 + DMA_REG_SOURCE_HI,
                                 routes[route] >> 32);
                    qtest_writel(qts, WIDGET_WINDOW0 + DMA_REG_ADDRESS,
                                 (uint32_t)(routes[route] - DMA_LEN));
                    qtest_writel(qts, WIDGET_WINDOW0 + DMA_REG_ADDRESS_HI,
                                 (routes[route] - DMA_LEN) >> 32);
                    qtest_writel(qts, WIDGET_WINDOW0 + DMA_REG_GO, 2);
                    qtest_memread(qts, at - DMA_LEN, bytes, sizeof(bytes));
                    for (i = 0; i < sizeof(bytes); i++) {
                        unsigned index = lane && !plain ? i ^ 3 : i;
                        g_assert_cmpuint(bytes[i], ==, seed + index);
                    }
                }
                qtest_quit(qts);
            }
        }
    }
}

static void test_full_packet_interrupts(void)
{
    QTestState *qts = head_start(48);
    uint64_t pin = target(WIDE_WIDGET) | (0x3ffULL << 38) | CARRIED_BIT;
    uint64_t error = target(WIDE_WIDGET) | (0xffffULL << 32);

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/pin", 0, pin);
    aim_the_destination(qts, WIDE_WIDGET, DEST_UPPER, PIN_OFF);
    route_the_pin(qts, 0x3ff);
    qtest_writel(qts, WIDGET_REG(BRIDGE_INT_ENABLE), 1U << PIN);
    drive_the_pin(qts, 1);
    g_assert_cmphex(qtest_readq(qts, pin + PIN_OFF), ==, INT_SET | PIN_NUMBER);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/pin", 0, error);
    provoke_the_error(qts, WIDE_WIDGET, 0xffff);
    g_assert_cmphex(qtest_readq(qts, error + ERR_OFF), ==,
                    INT_SET | ERR_NUMBER);
    qtest_quit(qts);
}

/*
 * The inbound PIO translated window: byte-lane-corrected access onto PCI
 * memory space at PCI address zero, exercised directly against the
 * bridge's sysbus face rather than through a Hub big window or a
 * processor. This tests the chip's address decode, not the fabric path a
 * booted kernel takes to reach it.
 */
/* The windows the chip decodes above its register block. */
#define PIOX_CFG0(slot)      (0x020000 + (slot) * 0x1000)
#define WINDOW0         0x200000

/*
 * Registers sit at a slot's offset plus four: Linux names that address,
 * OpenBSD names the slot and folds the four in.
 */
#define PIOX_REG(offset)             ((offset) + 4)

/* A card in slot 0, and where firmware would put its BAR in PCI space. */
#define PIOX_CARD_MMIO               0x10000000u

/*
 * Restated from the header rather than included from it. See the comment
 * on SGI_BRIDGE_PIO_XLATE_BASE: byte-lane-corrected access onto PCI memory
 * space at PCI address 0, at this fixed offset inside the widget.
 */
#define PIO_XLATE_BASE          0x40000000ULL
#define PIO_XLATE_SIZE          0x40000000ULL
#define PIO_XLATE(pci_addr)     (PIO_XLATE_BASE + (pci_addr))

#define PATTERN_THROUGH_WINDOW  0xa5c3f00du
#define PATTERN_THROUGH_XLATE   0x5a3c0ff0u

static QTestState *bridge_start(void)
{
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -S "
        "-device sgi-bridge,id=bridge,board-part=" BOARD_PART ","
        "fabric=/machine/unattached/system[0] "
        "-device lsi53c895a,bus=pci,addr=" stringify(CARD_SLOT));

    qtest_sysbus_mmio_map(qts, BRIDGE, 0, 0);
    return qts;
}

/*
 * Give the card its BAR, let its memory space answer, and aim its window
 * at it.
 */
static void piox_card_start(QTestState *qts)
{
    qtest_writel(qts, PIOX_CFG0(CARD_SLOT) + PCI_BAR1, PIOX_CARD_MMIO);
    qtest_writel(qts, PIOX_CFG0(CARD_SLOT) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, PIOX_REG(BRIDGE_DEVICE(CARD_SLOT)),
                 DEV_MEMORY_SPACE | (PIOX_CARD_MMIO >> 20));
}

/*
 * A value that lands in PCI memory space through the card's own small
 * window (the ordinary, already-tested path) reads back through the new
 * inbound window, and a write through the new window lands where the small
 * window can see it. Either direction, on its own, is evidence of a
 * wired-up register; both together are what a genuine path onto PCI memory
 * space requires and a one-way passthrough would fail.
 *
 * The small window corrects PCI's little-endian byte order for the
 * big-endian CPU it faces (sgi_bridge_window_correct, sgi-bridge.c); the
 * translated window applies the very same correction, since it crosses the
 * same big-endian-processor-onto-little-endian-PCI-bus boundary, just
 * without a slot's own programmed base. The two windows therefore agree on
 * the value exactly, not up to a byte swap.
 */
static void test_pio_xlate_reaches_pci_memory(void)
{
    QTestState *qts = bridge_start();
    uint64_t pci_addr = PIOX_CARD_MMIO + CARD_SCRATCH;

    piox_card_start(qts);

    qtest_writel(qts, WINDOW0 + CARD_SCRATCH, PATTERN_THROUGH_WINDOW);
    g_assert_cmphex(qtest_readl(qts, WINDOW0 + CARD_SCRATCH), ==,
                    PATTERN_THROUGH_WINDOW);
    g_assert_cmphex(qtest_readl(qts, PIO_XLATE(pci_addr)), ==,
                    PATTERN_THROUGH_WINDOW);

    qtest_writel(qts, PIO_XLATE(pci_addr), PATTERN_THROUGH_XLATE);
    g_assert_cmphex(qtest_readl(qts, WINDOW0 + CARD_SCRATCH), ==,
                    PATTERN_THROUGH_XLATE);

    qtest_quit(qts);
}

/*
 * One byte past the window's declared size is no longer backed by this
 * window. It is a hole in the widget's own address space; every
 * unclaimed offset there reads back zero.
 */
static void test_pio_xlate_stops_at_its_declared_size(void)
{
    QTestState *qts = bridge_start();
    uint64_t pci_addr = PIOX_CARD_MMIO + CARD_SCRATCH;

    piox_card_start(qts);
    qtest_writel(qts, WINDOW0 + CARD_SCRATCH, PATTERN_THROUGH_WINDOW);
    g_assert_cmphex(qtest_readl(qts, PIO_XLATE(pci_addr)), ==,
                    PATTERN_THROUGH_WINDOW);

    g_assert_cmphex(qtest_readl(qts, PIO_XLATE_BASE + PIO_XLATE_SIZE), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qts = qtest_init(MACHINE_WITH_BRIDGE);

    qtest_add_data_func("sn0/bridge/identity", NULL, test_identity);
    qtest_add_data_func("sn0/bridge/upper-half-of-the-slot", NULL,
                        test_upper_half_of_the_slot);
    qtest_add_data_func("sn0/bridge/reserved-data-half-store", NULL,
                        test_reserved_data_half_store_completes);
    qtest_add_data_func("sn0/bridge/reset-state", NULL, test_reset_state);
    qtest_add_data_func("sn0/bridge/each-slot-resets-to-its-own-window", NULL,
                        test_each_slot_resets_to_its_own_window);
    qtest_add_data_func("sn0/bridge/field-widths", NULL,
                        test_fields_are_the_width_they_should_be);
    qtest_add_data_func("sn0/bridge/device-window-base", NULL,
                        test_a_device_window_base_survives);
    qtest_add_data_func("sn0/bridge/read-only-registers", NULL,
                        test_a_read_only_register_stays_put);
    qtest_add_data_func("sn0/bridge/target-flush", NULL,
                        test_the_target_flush_answers);
    qtest_add_data_func("sn0/bridge/fitted-by-default", NULL,
                        test_the_default_machine_has_the_bridge);
    qtest_add_data_func("sn0/bridge/configuration-space", NULL,
                        test_configuration_space_reaches_a_card);
    qtest_add_data_func("sn0/bridge/configuration-byte-lanes", NULL,
                        test_configuration_byte_lanes);
    qtest_add_data_func("sn0/bridge/configuration-space-layout", NULL,
                        test_configuration_space_is_laid_out_right);
    qtest_add_data_func("sn0/bridge/each-option-slot-decodes-a-card", NULL,
                        test_each_option_slot_decodes_a_card);
    qtest_add_data_func("sn0/bridge/device-windows", NULL,
                        test_device_windows_reach_their_cards);
    qtest_add_data_func("sn0/bridge/window-follows-its-base", NULL,
                        test_a_window_goes_where_its_base_says);
    qtest_add_data_func("sn0/bridge/translation-entries", NULL,
                        test_translation_entries_are_written_whole);
    qtest_add_data_func("sn0/bridge/translation-lower-half-read-only", NULL,
                        test_the_lower_half_view_is_read_only);
    qtest_add_data_func("sn0/bridge/translation-entries-reset-clear", NULL,
                        test_translation_entries_reset_clear);
    qtest_add_data_func("sn0/bridge/transfer-straddling-a-page", NULL,
                        test_a_transfer_straddling_a_page);
    qtest_add_data_func("sn0/bridge/invalid-entry-transfers-nothing", NULL,
                        test_an_invalid_entry_transfers_nothing);
    qtest_add_data_func("sn0/bridge/interrupt-pins-fold", NULL,
                        test_the_interrupt_pins_fold);
    qtest_add_data_func("sn0/bridge/status-follows-the-level", NULL,
                        test_the_status_register_follows_the_level);
    qtest_add_data_func("sn0/bridge/pin-ors-wire-and-pci-sources", NULL,
                        test_the_pin_ors_its_wire_and_pci_sources);
    qtest_add_data_func("sn0/bridge/enabled-pin-edge-reaches-the-hub", NULL,
                        test_an_enabled_pin_edge_reaches_the_hub);
    qtest_add_data_func("sn0/bridge/disabled-pin-edge-sends-no-packet", NULL,
                        test_a_disabled_pin_edge_sends_no_packet);
    qtest_add_data_func("sn0/bridge/enabling-held-pin-reaches-hub", NULL,
                        test_enabling_an_asserted_pin_reaches_the_hub);
    qtest_add_data_func("sn0/bridge/falling-edge-clears-hub-pending", NULL,
                        test_a_falling_edge_clears_the_hub_pending_bit);
    qtest_add_data_func("sn0/bridge/disabled-falling-edge-sends-no-clear", NULL,
                        test_a_disabled_falling_edge_sends_no_clear);
    qtest_add_data_func("sn0/bridge/enable-has-no-bit-31", NULL,
                        test_the_enable_register_has_no_bit_31);
    qtest_add_data_func("sn0/bridge/mode-and-device-keep-writes", NULL,
                        test_mode_and_device_keep_what_was_written);
    qtest_add_data_func("sn0/bridge/one-wire-write-runs-a-slot", NULL,
                        test_a_write_runs_a_bit_slot);
    qtest_add_data_func("sn0/bridge/one-wire-reset-finds-the-part", NULL,
                        test_a_reset_finds_the_part);
    qtest_add_data_func("sn0/bridge/one-wire-search-returns-identity", NULL,
                        test_a_search_returns_the_identity);
    qtest_add_data_func("sn0/bridge/one-wire-identifies-each-node", NULL,
                        test_each_node_has_its_own_board_identity);
    qtest_add_data_func("sn0/bridge/one-wire-page-reads-back", NULL,
                        test_a_page_reads_back_with_its_checks);
    qtest_add_data_func("sn0/bridge/one-wire-named-part-reads-back", NULL,
                        test_a_named_part_reads_back_its_page);
    qtest_add_data_func("sn0/bridge/one-wire-naming-another-deselects", NULL,
                        test_naming_another_part_deselects_this_one);
    qtest_add_data_func("sn0/bridge/one-wire-status-reads-by-page", NULL,
                        test_the_status_memory_reads_back_by_the_page);
    qtest_add_data_func(
        "sn0/bridge/one-wire-status-past-the-programmed-regions", NULL,
        test_status_past_the_programmed_regions);
    qtest_add_data_func("sn0/bridge/one-wire-unframed-matches-framed", NULL,
                        test_the_unframed_read_matches_the_framed_one);
    qtest_add_data_func("sn0/bridge/model-board-part-overrides-class-global",
                        NULL,
                        test_the_model_board_part_overrides_class_global);
    qtest_add_data_func("sn0/bridge/reject-missing-board-part",
                        &invalid_board_parts[0],
                        test_invalid_board_part_is_rejected);
    qtest_add_data_func("sn0/bridge/reject-overlong-board-part",
                        &invalid_board_parts[1],
                        test_invalid_board_part_is_rejected);
    qtest_add_data_func("sn0/bridge/reject-semicolon-in-board-part",
                        &invalid_board_parts[2],
                        test_invalid_board_part_is_rejected);
    qtest_add_data_func("sn0/bridge/one-wire-unframed-read-ends-with-its-check",
                        NULL, test_the_unframed_read_ends_with_its_check);
    qtest_add_data_func("sn0/bridge/one-wire-data-past-the-image", NULL,
                        test_the_data_past_the_image_is_unprogrammed);
    qtest_add_data_func("sn0/bridge/one-wire-framed-read-past-the-image", NULL,
                        test_the_framed_read_past_the_image_is_unprogrammed);
    qtest_add_data_func("sn0/bridge/dma-page-mapped-swap-clear", NULL,
                        test_dma_page_mapped_swap_clear);
    qtest_add_data_func("sn0/bridge/dma-page-mapped-swap-set", NULL,
                        test_dma_page_mapped_swap_set);
    qtest_add_data_func("sn0/bridge/dma-page-mapped-ignores-the-direct-bit",
                        NULL, test_dma_page_mapped_ignores_the_direct_bit);
    qtest_add_data_func("sn0/bridge/dma-trailing-partial-word", NULL,
                        test_dma_trailing_partial_word);
    qtest_add_data_func("sn0/bridge/dma-unaligned-start", NULL,
                        test_dma_unaligned_start);
    qtest_add_data_func("sn0/bridge/dma-swap-bits-reset-clear", NULL,
                        test_dma_swap_bits_reset_clear);
    qtest_add_data_func("sn0/bridge/dma-exempt-slot-is-never-swapped", NULL,
                        test_dma_exempt_slot_is_never_swapped);
    qtest_add_data_func("sn0/bridge/dma-unexempt-slot-is-swapped",
                        NULL, test_dma_unexempt_slot_is_swapped);
    qtest_add_data_func("sn0/bridge/dma-swap-on-an-exempt-slot-is-reported",
                        NULL, test_dma_swap_on_an_exempt_slot_is_reported);
    qtest_add_data_func("sn0/bridge/dma-direct-window-swap-clear", NULL,
                        test_dma_direct_window_swap_clear);
    qtest_add_data_func("sn0/bridge/dma-direct-window-swap-set", NULL,
                        test_dma_direct_window_swap_set);
    qtest_add_data_func("sn0/bridge/dma-the-two-swap-bits-are-independent",
                        NULL, test_dma_the_two_swap_bits_are_independent);
    qtest_add_data_func("sn0/bridge/dma-local-space-is-never-swapped", NULL,
                        test_dma_local_space_is_never_swapped);
    qtest_add_data_func("sn0/bridge/dma-a-self-ordered-card-is-never-swapped",
                        NULL, test_dma_a_self_ordered_card_is_never_swapped);
    qtest_add_data_func("sn0/bridge/self-ordered-card-narrow-reads-straight",
                        NULL,
                        test_a_self_ordered_card_reads_narrow_straight);
    qtest_add_data_func("sn0/bridge/an-ioc3-is-exempt-in-any-slot", NULL,
                        test_an_ioc3_is_exempt_in_any_slot);
    qtest_add_data_func("sn0/bridge/dma-direct-window-reaches-the-second-node",
                        NULL, test_dma_direct_window_reaches_the_second_node);
    qtest_add_data_func("sn0/bridge/dma-direct-window-reads-the-second-node",
                        NULL, test_dma_direct_window_reads_the_second_node);
    qtest_add_data_func("sn0/bridge/dma-direct-window-64bit-ignores-the-offset-step",
                        NULL, test_dma_direct_window_64bit_ignores_the_offset_step);
    qtest_add_data_func("sn0/bridge/device-window-64bit-doubleword", NULL,
                        test_dma_device_window_64bit_doubleword);
    qtest_add_data_func("sn0/bridge/per-pin-host-registers", NULL,
                        test_the_per_pin_host_registers);
    qtest_add_data_func("sn0/bridge/inbound-link-reset-clears-the-core", NULL,
                        test_an_inbound_link_reset_clears_the_core);
    qtest_add_data_func("sn0/bridge/inbound-link-reset-stops-at-the-pci-side",
                        NULL,
                        test_an_inbound_link_reset_stops_at_the_pci_side);

    qtest_add_func("sn0/bridge/widget/full-packet-dma", test_full_packet_dma);
    qtest_add_func("sn0/bridge/widget/full-packet-interrupts",
                    test_full_packet_interrupts);
    qtest_add_func("sn0/bridge/widget/packet-geometry", test_packet_geometry);
    qtest_add_func("sn0/bridge/widget/transfer-offsets",
                    test_transfers_land_at_their_offset);
    qtest_add_func("sn0/bridge/widget/transfer-widgets",
                    test_transfers_carry_their_target_widget);
    qtest_add_func("sn0/bridge/widget/pin-node",
                    test_a_pin_packet_addresses_the_named_node);
    qtest_add_func("sn0/bridge/widget/pin-node-stride",
                    test_a_pin_host_address_selects_a_node);
    qtest_add_func("sn0/bridge/widget/error-destination",
                    test_a_chip_error_uses_the_widget_destination);
    qtest_add_func("sn0/bridge/widget/pin-widget",
                    test_a_pin_packet_carries_its_target_widget);
    qtest_add_func("sn0/bridge/widget/pin-host-replaces",
                    test_a_pin_host_address_replaces_the_destination_bits);
    qtest_add_func("sn0/bridge/widget/pin-masked",
                    test_a_masked_pin_sends_no_packet);
    qtest_add_func("sn0/bridge/widget/pin-falling",
                    test_a_falling_pin_clears_through_the_same_address);
    qtest_add_func("sn0/bridge/widget/pin-falling-unarmed",
                    test_a_falling_pin_sends_nothing_without_the_mode_bit);
    qtest_add_func("sn0/bridge/widget/error-widget",
                    test_a_chip_error_carries_the_widget_destination);
    qtest_add_func("sn0/bridge/widget/shift-limit",
                    test_a_shift_past_the_address_is_refused);
    qtest_add_func("sn0/bridge/pio-xlate/reaches-pci-memory",
                    test_pio_xlate_reaches_pci_memory);
    qtest_add_func("sn0/bridge/pio-xlate/stops-at-its-declared-size",
                    test_pio_xlate_stops_at_its_declared_size);

    ret = g_test_run();
    qtest_quit(qts);

    return ret;
}
