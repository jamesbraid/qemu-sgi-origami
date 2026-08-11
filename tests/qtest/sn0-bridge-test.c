/*
 * The I/O bridge's register face: where its registers are, and what they hold
 * before anything writes them.
 *
 * Both halves of that are worth a test for the same reason — each has a wrong
 * answer that looks right. A model serving a register at both halves of its
 * eight-byte slot satisfies every guest we boot and is wrong about where the
 * registers are, which surfaces only when something composes an address from
 * one. And a model resetting the chip to zeros comes up reporting widget 0 and
 * little-endian byte order on a big-endian machine, which surfaces a long way
 * from the register that carries it.
 *
 * Where a whole register is compared rather than a field of it, that is
 * deliberate. Most of these registers have one field a guest branches on and
 * several it does not, and asserting the branch-on field is the check that
 * passes for a model that got the rest wrong. Comparing the word leaves nothing
 * weaker to pass: the expected value is built here from the same named fields
 * the chip's specification names, so a reader can see what it is made of, and
 * a term going missing changes the total.
 *
 * These run against the machine with its I/O bridge option turned on, which is
 * off by default because the bridge cannot yet carry the base I/O controller on
 * its PCI bus. One test below asserts the default is genuinely unchanged, so
 * that the option cannot quietly become the machine.
 *
 * Two limits, stated because they look testable and are not, and both are the
 * crossbar test's limits for the same reasons. There is no processor executing
 * in a qtest, so a refused access and an access that returned zero look
 * identical from here — every assertion meaning "this must not answer" is
 * written as "this must not answer *with the thing that is really there*",
 * which catches a model decoding to the wrong place and does not catch a model
 * answering zero instead of faulting. The `-d unimp` log is what shows that
 * difference, and reading it is a step a person takes. And the chip refuses an
 * access wider than a crosstalk double-word, which cannot be shown from here
 * either: a qtest's 64-bit read goes through the address space, which clamps
 * the length to the region's declared maximum and issues two legal 4-byte
 * accesses instead, so the value comes back and the test would fail against a
 * correct model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* Crosstalk space, and the bridge's own widget window inside it. */
#define SPACE_XTALK     0x20000000000ULL
#define BRIDGE_WIDGET   8
#define BRIDGE_BASE     (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))

/*
 * Every register is 32 bits in the low half of an eight-byte slot, so on this
 * big-endian machine the data sits at the slot's offset plus four. That is the
 * address Linux's own bridge header names; OpenBSD names the slot and folds the
 * four bytes in when it accesses it, which is why the two sources look four
 * bytes apart everywhere and are naming the same registers.
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
#define BRIDGE_PCI_TYPE1_CONFIG 0x0c8
#define BRIDGE_DEVICE(n)        (0x200 + 8 * (n))

#define BRIDGE_DEVICES          8

/* The identity a Bridge answers with, and the crossbar identity it must not. */
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
 * The reset state, field by named field. Seven of the control register's fields
 * come out of reset set, which is the whole reason these are written as sums:
 * a total cannot show which of its terms went missing.
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
#define DEV_MEMORY_SPACE        (1u << 12)
#define DEV_WINDOW_BASE         0xfffu
#define DEVICE_RESET            (DEV_ERROR_LOCK | DEV_PAGE_CROSS_CHECK | \
                                 DEV_MEMORY_SPACE)
/* Bits 28 down to 12 are defined, and so are the twelve of the window base. */
#define DEVICE_WRITABLE         0x1fffffffu

/*
 * The machine states one silicon revision for this chip and both registers that
 * report an identity are derived from it, so the test names it once too.
 */
#define BRIDGE_REVISION         3

#define MACHINE_WITH_BRIDGE     "-M origin200,io-bridge=on"

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
 * A card to probe with. Which card does not matter — these tests are about
 * the window rather than what is behind it — so this is an in-tree device
 * needing no backing file and no network.
 *
 * Its identity double-word is 0x00121000, whose four bytes are 0x00, 0x10,
 * 0x12 and 0x00 from the least significant up. Two of them are the same and
 * carry nothing, so the byte-lane assertions below rest on the two that differ
 * and on the two halfword reads, where the vendor and the device identifier
 * have no digits in common at all. Said out loud because "assert all four
 * bytes" reads like four independent checks and is two.
 */
#define PROBE_CARD              " -device lsi53c895a"
#define PROBE_VENDOR            0x1000
#define PROBE_DEVICE            0x0012
#define PROBE_IDENTITY          (PROBE_VENDOR | (PROBE_DEVICE << 16))

/* What a slot with nothing in it answers, which must not be zero. */
#define NO_CARD                 0xffffffffu

/* The two configuration registers these tests program on a card. */
#define PCI_COMMAND             0x04
#define PCI_COMMAND_MEMORY      0x0002
#define PCI_BAR1                0x14

/*
 * A 32-bit scratch register in the probe card's memory-mapped register file,
 * which is storage and nothing else. The window tests need somewhere behind a
 * card that takes a value and gives it back, and want to depend on as little of
 * that card's behaviour as possible; a scratch register is the least of it.
 */
#define CARD_SCRATCH            0x34

/*
 * Where each card's register file is put in PCI memory space. Sixteen megabytes
 * apart, which is far more than any window is wide, so that a window aimed at
 * one card cannot reach another and a test passing means the window went where
 * it was aimed rather than somewhere that happened to work.
 */
#define CARD_MMIO(slot)         (0x10000000u + (slot) * 0x1000000u)

/*
 * Where each slot's window sits in the widget's address space, written out
 * rather than computed. The model derives these from a base and two sizes; the
 * test states them, because a test that recomputes the implementation's
 * arithmetic checks that the arithmetic was run, not that it was right. The
 * first two windows are 2 MB and the last six are 1 MB, which is the detail
 * every driver gets wrong once.
 */
static const uint64_t window_offset[BRIDGE_DEVICES] = {
    0x200000, 0x400000, 0x600000, 0x700000,
    0x800000, 0x900000, 0xa00000, 0xb00000,
};

#define WINDOW(slot)            (BRIDGE_BASE + window_offset[slot])

static uint32_t bridge_identity(uint32_t revision)
{
    return (revision << ID_REVISION_SHIFT) |
           (BRIDGE_PART << ID_PART_SHIFT) |
           (BRIDGE_MANUFACTURER << ID_MANUFACTURER_SHIFT) |
           ID_ALWAYS_ONE;
}

static void test_identity(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    uint32_t id = qtest_readl(qts, REG(BRIDGE_IDENTIFICATION));

    /*
     * The whole word, not the part number a guest branches on. A model
     * reporting the right part with a crossbar's manufacturer beside it, or
     * with no revision, would pass a part-only check and would be a chip that
     * exists nowhere.
     */
    g_assert_cmphex(id, ==, bridge_identity(BRIDGE_REVISION));

    /*
     * Stated separately because it is the specific wrong answer this chip could
     * plausibly give: a crossbar reports part zero and manufacturer zero, and
     * a widget answering that sends both guests down a different probe path.
     */
    g_assert_false(ID_PART_OF(id) == XBOW_PART &&
                   ID_MANUFACTURER_OF(id) == XBOW_MANUFACTURER);

    qtest_quit(qts);
}

static void test_identity_follows_the_machines_revision(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on,"
                                 "widget0-revision=4");

    /*
     * The revision is a candidate value the machine makes settable, and this
     * chip's identity register is one of the two places the machine reports it.
     * Both are derived from the one setting, so turning it has to reach both;
     * a chip left on its own default would have the machine reporting two
     * different revisions for a chip it names once.
     */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_IDENTIFICATION)), ==,
                    bridge_identity(4));

    qtest_quit(qts);
}

static void test_upper_half_of_the_slot(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /*
     * The register is in the low half of an eight-byte slot and the upper half
     * holds nothing. A model answering at both halves would satisfy every guest
     * we boot, since none of them reads the upper half, and would be wrong
     * about where the register is. The upper half is also the address a reader
     * lands on by forgetting the plus-four, so it should be a message rather
     * than a plausible number.
     */
    g_assert_cmphex(qtest_readl(qts, BRIDGE_BASE + BRIDGE_IDENTIFICATION), !=,
                    qtest_readl(qts, REG(BRIDGE_IDENTIFICATION)));

    qtest_quit(qts);
}

static void test_reset_state(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /*
     * The chip's reset state is far more non-zero than a reader would guess,
     * and knowing two exceptions is not enough — the control register alone
     * comes up with seven fields set. Each of these compares the whole word so
     * that a field this test did not think to name still has to be right.
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
     * The add-512-megabytes bit is the one field of the direct map register
     * that resets set, and it is there to step the direct window over the
     * processor's low compatibility segments. A model resetting it clear moves
     * every direct-window transfer half a gigabyte down without a word.
     */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DIRECT_MAP)), ==,
                    DIRECT_MAP_RESET);

    /*
     * This machine straps PCI rather than the chip's other bus. The whole word
     * is compared, which asserts more than that: the chip's register table
     * gives both strap-driven bits as undefined at reset, so the flash-select
     * bit reading clear is this model's choice of strap and not a reading. It
     * is asserted anyway, so that changing which flash chip the machine straps
     * has to be deliberate.
     */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_STATUS)), ==, STATUS_PCI_MODE);

    qtest_quit(qts);
}

static void test_every_slot_resets_alike(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    int n;

    /*
     * Three bits set and not two: the memory-or-I/O select at bit 12 is the
     * third, so a slot comes up pointing at PCI memory space rather than I/O
     * space. All eight are checked rather than the first, because a table
     * written out by hand is where one row goes missing.
     *
     * The twelve-bit window base has no specified power-up value, and comparing
     * the whole word asserts our model's zero as well as the three specified
     * bits. That is a claim about this model rather than about the chip, and it
     * is made deliberately: an unspecified field that silently changes value is
     * worth a failing test, and the failure says which model changed.
     */
    for (n = 0; n < BRIDGE_DEVICES; n++) {
        g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(n))), ==,
                        DEVICE_RESET);
    }

    qtest_quit(qts);
}

static void test_fields_are_the_width_they_should_be(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    int n;

    /*
     * Writing all ones and reading back is how a register says how wide its
     * fields really are. The bus time-out register's retry hold is five bits at
     * 20:16, not four: a model giving it four reads back a value with its top
     * bit missing, and a guest waits a sixteenth of the time it asked for.
     * Comparing against the whole writable mask asserts every reserved bit in
     * the register at once, rather than the one field this test was written
     * for.
     */
    qtest_writel(qts, REG(BRIDGE_BUS_TIMEOUT), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_BUS_TIMEOUT)), ==,
                    BUS_TIMEOUT_WRITABLE);

    /*
     * The type 1 configuration register carries a bus number and a device
     * number and nothing else. This model gave it a whole writable word until
     * its fields were checked against the chip's own register table, on the
     * strength of a claim that our sources gave no width — they did.
     */
    qtest_writel(qts, REG(BRIDGE_PCI_TYPE1_CONFIG), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_PCI_TYPE1_CONFIG)), ==,
                    TYPE1_WRITABLE);

    /* Bits 31:29 of a device register are reserved and read back clear. */
    for (n = 0; n < BRIDGE_DEVICES; n++) {
        qtest_writel(qts, REG(BRIDGE_DEVICE(n)), 0xffffffff);
        g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(n))), ==,
                        DEVICE_WRITABLE);
    }

    /*
     * The control register's two link-retry-counter strobes are written as ones
     * and always read back zero, which is what makes them strobes rather than
     * storage. Everything else in the register takes the write.
     */
    qtest_writel(qts, REG(BRIDGE_CONTROL), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_CONTROL)), ==,
                    0xffffffffu & ~CONTROL_RETRY_CLEAR);

    qtest_quit(qts);
}

static void test_a_device_window_base_survives(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    uint32_t written = DEV_ERROR_LOCK | DEV_PAGE_CROSS_CHECK | 0x123u;

    /*
     * A slot's window base and its memory-or-I/O select are the two fields a
     * guest programs here, and the I/O choice is the one written by clearing a
     * bit that reset set — so this writes the register without bit 12 and
     * checks the bit actually cleared, which a model holding a reset value
     * behind a mask would fail.
     */
    qtest_writel(qts, REG(BRIDGE_DEVICE(0)), written);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(0))), ==, written);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(0))) &
                    DEV_MEMORY_SPACE, ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(0))) &
                    DEV_WINDOW_BASE, ==, 0x123u);

    /* And writing one slot's register leaves the other seven alone. */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(1))), ==, DEVICE_RESET);

    qtest_quit(qts);
}

static void test_a_read_only_register_stays_put(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /*
     * The identity is what a guest decides the shape of the machine from, so a
     * model letting a write land on it would let a guest change what machine it
     * thinks it is on.
     */
    qtest_writel(qts, REG(BRIDGE_IDENTIFICATION), 0xdeadbeef);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_IDENTIFICATION)), ==,
                    bridge_identity(BRIDGE_REVISION));

    qtest_quit(qts);
}

static void test_the_target_flush_answers(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /*
     * Reading this register is the operation and the value means nothing: the
     * read completes only once every earlier programmed access to the widget
     * has, which is how a driver makes a configuration change take effect
     * before relying on it. Every driver read for this chip does it after
     * writing configuration, so the one thing it must never do is fault. There
     * is nothing to assert about the answer, and the assertion is that reading
     * it does not take the register block with it.
     */
    qtest_readl(qts, REG(BRIDGE_TARGET_FLUSH));
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_IDENTIFICATION)), ==,
                    bridge_identity(BRIDGE_REVISION));

    qtest_quit(qts);
}

static void test_the_default_machine_has_no_bridge(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The bridge is an option and the machine without it is the machine the
     * contract describes, so this guards the option from quietly becoming the
     * default. It is a weaker assertion than it looks — a qtest cannot tell
     * a refused access from one that returned zero — so it says the address
     * does not answer *with the bridge's identity*, which is what would be
     * there if the chip had been fitted.
     */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_IDENTIFICATION)), !=,
                    bridge_identity(BRIDGE_REVISION));
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_CONTROL)), !=, CONTROL_RESET);

    qtest_quit(qts);
}

static void test_configuration_space_reaches_a_card(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE PROBE_CARD);

    /*
     * A 32-bit read of a slot's first configuration register returns the
     * identity double-word as a number, which is what both guests read first
     * and decide whether a slot is occupied from.
     */
    g_assert_cmphex(qtest_readl(qts, CFG0(0, 0)), ==, PROBE_IDENTITY);

    qtest_quit(qts);
}

static void test_configuration_byte_lanes(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE PROBE_CARD);

    /*
     * A narrow access reaches the byte lanes of the containing double-word,
     * which on a big-endian machine puts the double-word's least significant
     * byte at the highest of its four addresses. Linux aims at exactly those
     * addresses — it exclusive-ors the register offset with four minus the
     * width — so a model that ignored the width and returned the byte at the
     * offset it was given would hand Linux the wrong end of every identifier
     * it reads a byte or a halfword of.
     *
     * The halfwords are the assertions with real teeth here: the vendor and
     * the device identifier share no digits, so reading the wrong one cannot
     * pass.
     */
    g_assert_cmphex(qtest_readw(qts, CFG0(0, 0) + 2), ==, PROBE_VENDOR);
    g_assert_cmphex(qtest_readw(qts, CFG0(0, 0) + 0), ==, PROBE_DEVICE);

    g_assert_cmphex(qtest_readb(qts, CFG0(0, 0) + 1), ==,
                    (PROBE_DEVICE >> 0) & 0xff);
    g_assert_cmphex(qtest_readb(qts, CFG0(0, 0) + 2), ==,
                    (PROBE_VENDOR >> 8) & 0xff);

    qtest_quit(qts);
}

static void test_configuration_space_is_laid_out_right(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE PROBE_CARD
                                 " -device lsi53c895a,addr=1");

    /* A device's configuration space is 4 KB above the one below it. */
    g_assert_cmphex(qtest_readl(qts, CFG0(1, 0)), ==, PROBE_IDENTITY);

    /*
     * And a function's is 256 bytes above the one below it. This card has one
     * function, so the assertion is that function 1 of slot 0 is *empty* —
     * which is what says the function field decodes at all. A model ignoring
     * it would alias every function onto function 0 and answer with the card's
     * identity eight times over, and no test that only reads function 0 could
     * tell the difference.
     */
    g_assert_cmphex(qtest_readl(qts, CFG0(0, 1)), ==, NO_CARD);

    /* A slot with no card in it answers all ones, and specifically not zero. */
    g_assert_cmphex(qtest_readl(qts, CFG0(2, 0)), ==, NO_CARD);
    g_assert_cmphex(qtest_readl(qts, CFG0(7, 0)), ==, NO_CARD);

    qtest_quit(qts);
}

/*
 * The four slots the window tests populate: both large windows, and two of the
 * small ones far enough apart that a wrong window size shows up as a wrong
 * address rather than as an overlap.
 */
static const int populated[] = { 0, 1, 2, 7 };
#define POPULATED_SLOTS         ARRAY_SIZE(populated)

static void bridge_aim_window_at_its_card(QTestState *qts, int slot)
{
    qtest_writel(qts, CFG0(slot, 0) + PCI_BAR1, CARD_MMIO(slot));
    qtest_writel(qts, CFG0(slot, 0) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, REG(BRIDGE_DEVICE(slot)),
                 DEV_MEMORY_SPACE | (CARD_MMIO(slot) >> 20));
}

static void test_device_windows_reach_their_cards(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -device lsi53c895a,addr=0"
                                 " -device lsi53c895a,addr=1"
                                 " -device lsi53c895a,addr=2"
                                 " -device lsi53c895a,addr=7");
    unsigned int i;

    for (i = 0; i < POPULATED_SLOTS; i++) {
        bridge_aim_window_at_its_card(qts, populated[i]);
    }

    /*
     * Every window is written before any is read back, which is the part that
     * makes this a test of eight separate windows rather than of one. Windows
     * that overlapped in the widget's space, or that all pointed at the same
     * place in PCI space, would each pass a write-then-read-immediately check
     * and fail here, because the last write would be the only one left.
     */
    for (i = 0; i < POPULATED_SLOTS; i++) {
        qtest_writel(qts, WINDOW(populated[i]) + CARD_SCRATCH,
                     0xc0ffee00 + populated[i]);
    }
    for (i = 0; i < POPULATED_SLOTS; i++) {
        g_assert_cmphex(qtest_readl(qts, WINDOW(populated[i]) + CARD_SCRATCH),
                        ==, 0xc0ffee00u + populated[i]);
    }

    qtest_quit(qts);
}

static void test_a_window_goes_where_its_base_says(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE PROBE_CARD);

    bridge_aim_window_at_its_card(qts, 0);
    qtest_writel(qts, WINDOW(0) + CARD_SCRATCH, 0xc0ffee00);
    g_assert_cmphex(qtest_readl(qts, WINDOW(0) + CARD_SCRATCH), ==, 0xc0ffee00);

    /*
     * Move the window a megabyte up and the card is no longer under it.
     * Without this, a model that ignored the base field entirely — mapping
     * every window onto whatever the card's address happened to be — would
     * pass everything above, because every card is aimed where it lives.
     */
    qtest_writel(qts, REG(BRIDGE_DEVICE(0)),
                 DEV_MEMORY_SPACE | ((CARD_MMIO(0) >> 20) + 1));
    g_assert_cmphex(qtest_readl(qts, WINDOW(0) + CARD_SCRATCH), !=, 0xc0ffee00);

    /*
     * And selecting I/O space takes the card out from under it too, since this
     * card's register file is in memory space. A model treating the
     * memory-or-I/O bit as storage would answer here as though it were still
     * memory space, which is the state a slot resets into and so the state
     * every other test in this file leaves it in.
     */
    qtest_writel(qts, REG(BRIDGE_DEVICE(0)), CARD_MMIO(0) >> 20);
    g_assert_cmphex(qtest_readl(qts, WINDOW(0) + CARD_SCRATCH), !=, 0xc0ffee00);

    /* Put it back: the card is there again, so the window moved, not it. */
    qtest_writel(qts, REG(BRIDGE_DEVICE(0)),
                 DEV_MEMORY_SPACE | (CARD_MMIO(0) >> 20));
    g_assert_cmphex(qtest_readl(qts, WINDOW(0) + CARD_SCRATCH), ==, 0xc0ffee00);

    qtest_quit(qts);
}

/*
 * The address translation table and its two views, stated here as the datasheet
 * states them rather than as the model computes them.
 */
#define ATE_BASE                0x010000
#define ATE_ENTRIES             128
#define ATE(n)                  (BRIDGE_BASE + ATE_BASE + (n) * 8)
#define ATE_UPPER(n)            (ATE(n) + 4)
#define ATE_LOW_BASE            0x011000
#define ATE_LOW(n)              (BRIDGE_BASE + ATE_LOW_BASE + (n) * 4)

static void test_translation_entries_are_written_whole(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    static const int entries[] = { 0, 1, 63, 127 };
    unsigned int i;

    /*
     * An entry is written as one 64-bit quantity and read back in halves from
     * two different places: the upper half four bytes into the entry's own
     * slot, and the lower half through a second region that exists for nothing
     * else. Both halves of every entry are checked, because a model decoding
     * only the first region would leave the second dead and a driver reading an
     * entry back would get nothing.
     *
     * All four entries are written before any is read, which is what makes this
     * a test of an indexed table rather than of one register: a model ignoring
     * the index would pass a write-then-read-immediately check on every entry
     * and fail here on all but the last.
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

    qtest_quit(qts);
}

static void test_the_lower_half_view_is_read_only(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    qtest_writeq(qts, ATE(0), 0x1234567800abcdefull);
    g_assert_cmphex(qtest_readl(qts, ATE_LOW(0)), ==, 0x00abcdef);

    /*
     * A write to the lower-half view must not reach the entry. A chip that
     * swallowed it would let a driver believe it had changed an entry through
     * the one address that cannot change one, and the entry it thought it had
     * changed would go on translating to the old page.
     */
    qtest_writel(qts, ATE_LOW(0), 0xdeadbeef);
    g_assert_cmphex(qtest_readl(qts, ATE_LOW(0)), ==, 0x00abcdef);
    g_assert_cmphex(qtest_readl(qts, ATE_UPPER(0)), ==, 0x12345678);

    qtest_quit(qts);
}

static void test_translation_entries_reset_clear(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    unsigned int n;

    /*
     * Every entry comes up wholly clear, which matters for one bit of it: the
     * valid bit at the bottom, which says a translation through this entry must
     * fault rather than reach somewhere. The whole entry is asserted rather
     * than that bit, because an entry holding a stale page address behind a
     * clear valid bit is a working translation one write away, and a check on
     * the valid bit alone cannot see it.
     */
    for (n = 0; n < ATE_ENTRIES; n++) {
        g_assert_cmphex(qtest_readl(qts, ATE_UPPER(n)), ==, 0);
        g_assert_cmphex(qtest_readl(qts, ATE_LOW(n)), ==, 0);
    }

    qtest_quit(qts);
}

/*
 * The transfer source, and the registers it is driven through. It is a device
 * of this fork's own, built because nothing in the tree could issue one
 * transfer of a length and address a test chooses without either claiming this
 * machine supports message-signalled interrupts or testing the translation
 * table through another chip's script engine.
 */
#define DMA_CARD                " -device sgi-bridge-dma-test"
#define DMA_REG_ADDRESS         0x00
#define DMA_REG_LENGTH          0x04
#define DMA_REG_SEED            0x08
#define DMA_REG_GO              0x0c

/* Where the transfer source's own registers are put in PCI memory space. */
#define DMA_CARD_MMIO           0x10000000u

/*
 * The translated window, and the two pages of it this test uses. A page is 4 KB
 * because the control register resets choosing that size.
 */
#define PCI_TRANSLATED          0x40000000u
#define PAGE_SIZE_4K            0x1000u

/*
 * The two places the two pages are translated to, and the point of the whole
 * test: they are NOT adjacent. A model translating once per transfer instead of
 * once per page puts the tail of a straddling transfer in the page physically
 * after the first one, which is right only when the two happen to be
 * consecutive — so a test whose two pages are consecutive cannot tell the two
 * models apart, and is the test that was missing from a neighbouring emulator
 * for years.
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
    qtest_writel(qts, CFG0(0, 0) + 0x10, DMA_CARD_MMIO);
    qtest_writel(qts, CFG0(0, 0) + PCI_COMMAND, PCI_COMMAND_MEMORY | 0x0004);
    qtest_writel(qts, REG(BRIDGE_DEVICE(0)),
                 DEV_MEMORY_SPACE | (DMA_CARD_MMIO >> 20));
}

static void bridge_transfer(QTestState *qts, uint32_t addr, uint32_t length,
                            uint32_t seed)
{
    qtest_writel(qts, WINDOW(0) + DMA_REG_ADDRESS, addr);
    qtest_writel(qts, WINDOW(0) + DMA_REG_LENGTH, length);
    qtest_writel(qts, WINDOW(0) + DMA_REG_SEED, seed);
    qtest_writel(qts, WINDOW(0) + DMA_REG_GO, 1);
}

static void test_a_transfer_straddling_a_page(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE DMA_CARD);
    uint8_t head[STRADDLE_HEAD], tail[STRADDLE_TAIL];
    uint8_t after[STRADDLE_TAIL];
    unsigned int i;

    bridge_start_the_transfer_source(qts);

    /*
     * Two entries differing in nothing but the page they name. They target the
     * same widget deliberately, so that what this test asserts stays true
     * however the machine ends up composing a widget and an offset into a
     * fabric address — the question that composition answers is not this
     * test's.
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

    /* The head is in the page the first entry names. */
    for (i = 0; i < STRADDLE_HEAD; i++) {
        g_assert_cmpuint(head[i], ==, (uint8_t)(STRADDLE_SEED + i));
    }

    /*
     * And the tail is in the page the SECOND entry names, which is the whole
     * assertion. A model that translated once for the transfer would have
     * carried on into the page after the first one.
     */
    for (i = 0; i < STRADDLE_TAIL; i++) {
        g_assert_cmpuint(tail[i], ==,
                         (uint8_t)(STRADDLE_SEED + STRADDLE_HEAD + i));
    }

    /*
     * Stated as its own assertion rather than left implied, because it names
     * the exact wrong answer: the page physically after the first one is where
     * a translate-once model puts the tail, and it must be untouched.
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

    /* And the same transfer lands once the entry is made valid. */
    qtest_writeq(qts, ATE(0), ATE_ENTRY(TARGET_PAGE_A, 0));
    bridge_transfer(qts, PCI_TRANSLATED, sizeof(seen), STRADDLE_SEED);
    qtest_memread(qts, TARGET_PAGE_A, seen, sizeof(seen));
    for (i = 0; i < sizeof(seen); i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(STRADDLE_SEED + i));
    }

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
 * The pin the tests drive. A slot's first interrupt pin goes to the chip pin of
 * the same number, so a source in slot 3 drives pin 3 — which is also the pin
 * the real machine's base I/O controller uses for its second interrupt, so the
 * test exercises the one that matters rather than the convenient one.
 */
#define INT_TEST_SLOT           3

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
     * THE assertion this chip's datasheet leads with. The guest re-arms
     * interrupts in software: at the end of every interrupt it services it
     * re-reads this register, and acts on its own pin still being set. So the
     * bit must survive being read, and read again, and read again — a model
     * that latched it and cleared it on service would tell the guest a device
     * with a second condition pending had gone quiet, and that device would
     * never be serviced again. Such a model passes a single read and then
     * hangs a machine, so the repetition here is the test itself rather than
     * belt and braces.
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

static void test_the_enable_register_has_no_bit_31(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /*
     * The multiple-error flag at bit 31 of the status word has no enable: the
     * chip's enable table starts at bit 30. A model implementing the enable as
     * a plain 32-bit mask over the status word would let a multiple-error
     * report through a mask a real chip would not, so the bit must refuse to
     * be set at all.
     */
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_ENABLE)), ==, 0x7fffffff);

    qtest_quit(qts);
}

static void test_mode_and_device_keep_what_was_written(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    unsigned int n;

    /*
     * The guest read-modify-writes both of these when it establishes an
     * interrupt: it reads the current value, sets the bits for the pin it is
     * attaching, and writes it back. A model whose read gave zero rather than
     * the last value written would wipe every previously configured pin each
     * time, so only the most recently attached device would work — and the
     * failure would look like a driver bug in whichever attached first.
     *
     * So this establishes all eight pins one at a time, the way a guest does,
     * and checks the accumulated value at the end rather than after each.
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

    qtest_quit(qts);
}

static void test_the_per_pin_host_registers(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    unsigned int n;

    /*
     * Each pin has its own: ten bits replacing part of the destination address
     * and eight naming the bit to set in the host's interrupt register. All
     * eight are written before any is read, so a model holding one register for
     * all eight pins fails rather than passing on the last one written.
     */
    for (n = 0; n < BRIDGE_DEVICES; n++) {
        qtest_writel(qts, REG(BRIDGE_INT_HOST(n)), 0xffffffff);
    }
    for (n = 0; n < BRIDGE_DEVICES; n++) {
        /* Bits 31:18 are reserved, so all ones reads back as the two fields. */
        g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_HOST(n))), ==, 0x3ffff);
    }

    for (n = 0; n < BRIDGE_DEVICES; n++) {
        qtest_writel(qts, REG(BRIDGE_INT_HOST(n)), 0x100 + n);
    }
    for (n = 0; n < BRIDGE_DEVICES; n++) {
        g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_HOST(n))), ==,
                        0x100 + n);
    }

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/bridge/identity", NULL, test_identity);
    qtest_add_data_func("sn0/bridge/identity-follows-the-machine", NULL,
                        test_identity_follows_the_machines_revision);
    qtest_add_data_func("sn0/bridge/upper-half-of-the-slot", NULL,
                        test_upper_half_of_the_slot);
    qtest_add_data_func("sn0/bridge/reset-state", NULL, test_reset_state);
    qtest_add_data_func("sn0/bridge/every-slot-resets-alike", NULL,
                        test_every_slot_resets_alike);
    qtest_add_data_func("sn0/bridge/field-widths", NULL,
                        test_fields_are_the_width_they_should_be);
    qtest_add_data_func("sn0/bridge/device-window-base", NULL,
                        test_a_device_window_base_survives);
    qtest_add_data_func("sn0/bridge/read-only-registers", NULL,
                        test_a_read_only_register_stays_put);
    qtest_add_data_func("sn0/bridge/target-flush", NULL,
                        test_the_target_flush_answers);
    qtest_add_data_func("sn0/bridge/absent-by-default", NULL,
                        test_the_default_machine_has_no_bridge);
    qtest_add_data_func("sn0/bridge/configuration-space", NULL,
                        test_configuration_space_reaches_a_card);
    qtest_add_data_func("sn0/bridge/configuration-byte-lanes", NULL,
                        test_configuration_byte_lanes);
    qtest_add_data_func("sn0/bridge/configuration-space-layout", NULL,
                        test_configuration_space_is_laid_out_right);
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
    qtest_add_data_func("sn0/bridge/status-follows-the-level", NULL,
                        test_the_status_register_follows_the_level);
    qtest_add_data_func("sn0/bridge/enable-has-no-bit-31", NULL,
                        test_the_enable_register_has_no_bit_31);
    qtest_add_data_func("sn0/bridge/mode-and-device-keep-writes", NULL,
                        test_mode_and_device_keep_what_was_written);
    qtest_add_data_func("sn0/bridge/per-pin-host-registers", NULL,
                        test_the_per_pin_host_registers);

    return g_test_run();
}
