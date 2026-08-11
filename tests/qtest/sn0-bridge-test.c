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

    /* This machine straps PCI rather than the chip's other bus. */
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

    return g_test_run();
}
