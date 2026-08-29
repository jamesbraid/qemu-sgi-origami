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
#include "qemu/bswap.h"
#include "libqtest.h"

#include <elf.h>

/* Crosstalk space, and the bridge's own widget window inside it. */
#define SPACE_XTALK     0x20000000000ULL
#define NODE_STRIDE     (1ULL << 32)
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
/*
 * What each device register holds at reset, as the firmware's own sanity
 * diagnostic states it: that diagnostic prints the value it expected beside the
 * value it read, for each of the eight devices in turn, and these are the eight
 * it expects.
 *
 * Written out as literals on purpose. The model computes them from a rule --
 * two megabytes of window each for the first two devices and one for the rest,
 * laid end to end -- and a test recomputing that rule would assert the formula
 * against itself and pass whatever both were changed to. These are the numbers
 * a real firmware asked for.
 */
static const uint32_t device_reset[8] = {
    0x18001000, 0x18001002, 0x18001004, 0x18001005,
    0x18001006, 0x18001007, 0x18001008, 0x18001009,
};
/* Bits 28 down to 12 are defined, and so are the twelve of the window base. */
#define DEVICE_WRITABLE         0x1fffffffu

/*
 * The machine states one silicon revision for this chip and both registers that
 * report an identity are derived from it, so the test names it once too.
 */
#define BRIDGE_REVISION         4

#define MACHINE_WITH_BRIDGE     "-M origin200,io-bridge=on"

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
/*
 * Which PCI devices these tests put their cards in, and why they are named
 * rather than left to the allocator.
 *
 * The machine fits two SCSI adapters at devices 0 and 1, because the base I/O
 * board carries two, so those devices are never free. That is the occasion for
 * naming the slots but not the reason: a test that lets the allocator choose
 * and then reads device 0 is asserting where the allocator puts things, which
 * is not this chip's behaviour and was one machine change away from breaking
 * whatever that change happened to be. Every card below says where it goes.
 *
 * The slot each test uses is its own, because every test starts its own
 * machine — a device in one test's slot 7 says nothing about another test's.
 */
#define PROBE_SLOT              4  /* the card most tests read */
#define PROBE_SLOT_ABOVE        5  /* the next device up, for the 4 KB stride */
#define DMA_SLOT                6  /* the bus-mastering card */
#define EMPTY_SLOT              7  /* never populated where emptiness is asserted */

#define PROBE_CARD              " -device lsi53c895a,addr=" stringify(PROBE_SLOT)
#define PROBE_VENDOR            0x1000
#define PROBE_DEVICE            0x0012
#define PROBE_IDENTITY          (PROBE_VENDOR | (PROBE_DEVICE << 16))

/* What a slot with nothing in it answers, which must not be zero. */
#define NO_CARD                 0xffffffffu

/* The configuration registers these tests program on a card. */
#define PCI_COMMAND             0x04
#define PCI_COMMAND_IO          0x0001
#define PCI_COMMAND_MEMORY      0x0002
#define PCI_COMMAND_MASTER      0x0004
#define PCI_BAR0                0x10
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

static char *write_reserved_store_guest(void)
{
    static const uint32_t code[] = {
        0x3c089200,             /* lui t0, 0x9200 */
        0x00000000,             /* nop */
        0x0008403c,             /* dsll32 t0, t0, 0 */
        0x3c090800,             /* lui t1, 0x0800 */
        0x3529019c,             /* ori t1, t1, 0x019c */
        0x01094025,             /* or t0, t0, t1 */
        0xad000000,             /* sw zero, 0(t0) */
        0x3c089600,             /* lui t0, 0x9600 */
        0x0008403c,             /* dsll32 t0, t0, 0 */
        0x3c090003,             /* lui t1, 0x0003 */
        0x01094025,             /* or t0, t0, t1 */
        0x3c095a17,             /* lui t1, 0x5a17 */
        0x3529c0de,             /* ori t1, t1, 0xc0de */
        0xad090000,             /* sw t1, 0(t0) */
        0x1000ffff,             /* b . */
        0x00000000,             /* nop */
    };
    uint8_t image[0x1000 + sizeof(code)] = { 0 };
    const uint64_t entry = 0x8000000000020000ULL;
    GError *err = NULL;
    char *path = NULL;
    int fd;
    size_t i;

    fd = g_file_open_tmp("sn0-bridge-guest-XXXXXX", &path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(fd, >=, 0);

    image[0] = ELFMAG0;
    image[1] = ELFMAG1;
    image[2] = ELFMAG2;
    image[3] = ELFMAG3;
    image[EI_CLASS] = ELFCLASS64;
    image[EI_DATA] = ELFDATA2MSB;
    image[EI_VERSION] = EV_CURRENT;
    stw_be_p(image + 16, ET_EXEC);
    stw_be_p(image + 18, EM_MIPS);
    stl_be_p(image + 20, EV_CURRENT);
    stq_be_p(image + 24, entry);
    stq_be_p(image + 32, 64);
    stw_be_p(image + 52, 64);
    stw_be_p(image + 54, 56);
    stw_be_p(image + 56, 1);

    stl_be_p(image + 64, PT_LOAD);
    stl_be_p(image + 68, PF_R | PF_X);
    stq_be_p(image + 72, 0x1000);
    stq_be_p(image + 80, entry);
    stq_be_p(image + 88, entry);
    stq_be_p(image + 96, sizeof(code));
    stq_be_p(image + 104, sizeof(code));
    stq_be_p(image + 112, 0x1000);

    for (i = 0; i < ARRAY_SIZE(code); i++) {
        stl_be_p(image + 0x1000 + i * sizeof(code[0]), code[i]);
    }

    g_assert_cmpint(write(fd, image, sizeof(image)), ==,
                    (ssize_t)sizeof(image));
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_reserved_data_half_store_completes(const void *data)
{
    g_autofree char *kernel = write_reserved_store_guest();
    QTestState *qts;
    uint32_t marker = 0;

    g_assert_true(g_setenv("QTEST_QEMU_ARGS", "-accel tcg", true));
    qts = qtest_initf(MACHINE_WITH_BRIDGE " -smp 1 -kernel %s", kernel);
    g_unsetenv("QTEST_QEMU_ARGS");

    g_test_timer_start();
    while (marker != GUEST_MARKER && g_test_timer_elapsed() < 1) {
        marker = qtest_readl(qts, GUEST_MARKER_ADDR);
        g_usleep(1000);
    }

    g_assert_cmphex(marker, ==, GUEST_MARKER);
    qtest_quit(qts);
    g_unlink(kernel);
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
                                 "widget0-revision=3");

    /*
     * The revision is a candidate value the machine makes settable, and this
     * chip's identity register is one of the two places the machine reports it.
     * Both are derived from the one setting, so turning it has to reach both;
     * a chip left on its own default would have the machine reporting two
     * different revisions for a chip it names once.
     */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_IDENTIFICATION)), ==,
                    bridge_identity(3));

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

/*
 * The base I/O controller's slot and the two SCSI adapters'. Firmware assigns
 * each of them PCI resources and aims its window at them after every reset,
 * so these are the slots whose register does not stay at the raw hardware
 * reset value once the machine has actually reset — see the exclusion below.
 *
 * The two adapters were not always in this list. An earlier version of this
 * firmware aimed only the controller's window, on the theory that a guest's
 * own driver aims the other two when it attaches — true of OpenBSD's qlw,
 * which manages the Bridge's per-device window itself, and false of Linux's
 * driver pair for this hardware, whose PCI core trusts firmware to have
 * already placed a reachable value in the BAR and never touches the window
 * register that makes it reachable. A guest of the second kind enumerated
 * the bus, read back an identity that looked right, and then found nothing
 * behind either adapter's BAR — the two mechanisms are that separate. See
 * hw/mips/sn0.h for the full account.
 */
#define IOC3_SLOT 2
#define SCSI0_SLOT 0
#define SCSI1_SLOT 1

/*
 * Where firmware places each device's memory BAR — SN0_PCI_IOC3_MEM and
 * friends in hw/mips/sn0.h, restated here because this file does not include
 * machine headers. Not a hardware fact; see those constants' own comments
 * for why none of them is zero, and see them for why each is the console
 * widget's own base plus that device's window offset rather than an
 * arbitrary low address: OpenBSD's own platform code expects a PCI address
 * inside the bus resource window it declares, not merely a non-zero one, and
 * this is the value that satisfies it. Written as the sum of those two named
 * parts, not folded into one number, so a change to either announces itself
 * here instead of leaving a restated constant silently wrong — which is
 * exactly what a bare 0x100000 did the first time this was written, caught
 * only by this file's own reset-value test failing against a real,
 * live-built machine.
 */
#define IOC3_PCI_MEM ((8 << 24) + 0x600000)
#define SCSI0_PCI_MEM ((8 << 24) + 0x200000)
#define SCSI1_PCI_MEM ((8 << 24) + 0x400000)

static void test_each_slot_resets_to_its_own_window(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    int n;

    /*
     * Three bits set and not two: the memory-or-I/O select at bit 12 is the
     * third, so a slot comes up pointing at PCI memory space rather than I/O
     * space. All eight are checked rather than the first, because a table
     * written out by hand is where one row goes missing.
     *
     * The twelve-bit window base has no power-up value in the specification --
     * its register table refers to a mapping chapter instead of giving one --
     * so the value asserted here comes from the firmware, which states all
     * eight and rejects a Bridge holding anything else.
     *
     * Only five of the eight can be checked here, and that is a real gap rather
     * than a choice: the three slots below are assigned a window by firmware
     * immediately after this same reset, so by the time a qtest can read them
     * the register already holds the assignment and the reset value it briefly
     * had is not observable from here. Their expected values are in the table
     * above for the record, and the assertions for what they become instead are
     * the reassignment tests further down.
     *
     * These three slots are skipped here on purpose, not missed: firmware
     * assigns each of them a window immediately after this same reset, so by
     * the time a qtest can read anything the register already reflects that
     * assignment rather than the raw hardware default every other slot still
     * shows. test_ioc3_slot_is_reassigned_after_reset and
     * test_scsi_slots_are_reassigned_after_reset below are the assertions
     * for what they become instead.
     */
    for (n = 0; n < BRIDGE_DEVICES; n++) {
        if (n == IOC3_SLOT || n == SCSI0_SLOT || n == SCSI1_SLOT) {
            continue;
        }
        g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(n))), ==,
                        device_reset[n]);
    }

    qtest_quit(qts);
}

static void test_ioc3_slot_is_reassigned_after_reset(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /*
     * The other half of the exclusion above. Firmware's assignment is a
     * masked read-modify-write of the base and space-select fields only —
     * the same shape a guest's own write to this register gets — so the
     * error-lock and page-cross-check bits every other slot resets with
     * survive here too, and only the base and space-select fields carry the
     * assignment. A model that overwrote the whole register instead would
     * pass a test asserting only the base field and silently clear those two
     * bits, which is why the whole word is compared rather than a part of it.
     */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(IOC3_SLOT))), ==,
                    DEV_ERROR_LOCK | DEV_PAGE_CROSS_CHECK | DEV_MEMORY_SPACE |
                    (IOC3_PCI_MEM >> 20)); /* base is PCI address bits 31:20 */

    qtest_quit(qts);
}

/*
 * The two adapters' half of the same exclusion, checked together rather than
 * as two copies of the same test: a loop that assigns one slot's address to
 * both, or the second slot's address to neither, produces two rows that look
 * individually plausible and only disagree with each other. Both the device
 * window and the memory BAR it should now answer at are checked for each
 * slot, the same pair the IOC3 test above checks, because firmware's job
 * here is the same two writes for a fixable reason rather than an accident
 * of what the loop happened to do: an aimed window with no assigned BAR
 * behind it decodes to nothing, and an assigned BAR with no aimed window is
 * exactly the defect this test exists to catch a regression of.
 */
static void test_scsi_slots_are_reassigned_after_reset(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(SCSI0_SLOT))), ==,
                    DEV_ERROR_LOCK | DEV_PAGE_CROSS_CHECK | DEV_MEMORY_SPACE |
                    (SCSI0_PCI_MEM >> 20));
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(SCSI1_SLOT))), ==,
                    DEV_ERROR_LOCK | DEV_PAGE_CROSS_CHECK | DEV_MEMORY_SPACE |
                    (SCSI1_PCI_MEM >> 20));

    g_assert_cmphex(qtest_readl(qts, CFG0(SCSI0_SLOT, 0) + PCI_BAR1), ==,
                    SCSI0_PCI_MEM);
    g_assert_cmphex(qtest_readl(qts, CFG0(SCSI1_SLOT, 0) + PCI_BAR1), ==,
                    SCSI1_PCI_MEM);

    /* Same whole-word-then-mask shape as the IOC3 command readback above. */
    g_assert_cmphex(qtest_readl(qts, CFG0(SCSI0_SLOT, 0) + PCI_COMMAND) &
                    0xffff, ==,
                    PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    g_assert_cmphex(qtest_readl(qts, CFG0(SCSI1_SLOT, 0) + PCI_COMMAND) &
                    0xffff, ==,
                    PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    qtest_quit(qts);
}

/*
 * The question the codex review raised at the gate: the assignment above is
 * a legacy reset handler (sn0_assign_pci_resources), registered once from the
 * machine-init-done notifier, and that registration says nothing on its own
 * about whether it keeps running after the PCI devices' own resets on every
 * future reset, or only happened to on this first one.
 *
 * It does not need a second reset path to trust: qemu_system_reset() is the
 * only caller of qemu_devices_reset(), and hw/core/machine.c calls that exact
 * function for the machine's own first reset, immediately after registration
 * finishes and before the guest runs a single instruction. A `system_reset`
 * issued later from the monitor walks the identical, singleton reset
 * container in the identical, append-only order — nothing in this machine
 * ever unregisters a reset handler or hot-plugs a device, so the order this
 * test observes on the second reset is the same order the boot-time
 * assignment above already depends on, not a distinct claim about the
 * future. This test is what turns that reading of the reset code into an
 * observation instead of an inference the datasheet comment would otherwise
 * be making unchecked.
 *
 * Both the bridge's device-window register and the controller's own BAR are
 * checked, because firmware's job after a reset is two writes, not one: the
 * bridge has to be aimed and the device has to answer at the address it is
 * aimed at. A model that reasserted only one of them would still fail a
 * guest's post-reset console write.
 */
static void test_ioc3_slot_survives_a_later_reset(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    qtest_system_reset(qts);

    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(IOC3_SLOT))), ==,
                    DEV_ERROR_LOCK | DEV_PAGE_CROSS_CHECK | DEV_MEMORY_SPACE |
                    (IOC3_PCI_MEM >> 20));
    g_assert_cmphex(qtest_readl(qts, CFG0(IOC3_SLOT, 0) + PCI_BAR0), ==,
                    IOC3_PCI_MEM);
    /*
     * A full-word read, not a halfword one, and that is deliberate: a 16-bit
     * access to a big-endian config-space dword lands on the byte lanes the
     * *other* half occupies unless the offset is exclusive-ored with two
     * first (test_configuration_byte_lanes above is the proof of that, and
     * this file's own header explains why). Command and status share one
     * dword at this offset, so reading the whole thing and masking to the
     * command half sidesteps the byte-lane question rather than getting it
     * wrong a second time in this same file — the command write firmware
     * issues elsewhere in this codebase uses the identical whole-word shape
     * for the identical reason.
     */
    g_assert_cmphex(qtest_readl(qts, CFG0(IOC3_SLOT, 0) + PCI_COMMAND) &
                    0xffff, ==, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

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

    /*
     * The one-wire master's two timing fields are its writable part, and the
     * two status bits below them are not — they are what the model answers
     * with, recomputed on every write because a write runs a bit slot. So the
     * comparison masks them out rather than expecting them clear: they are
     * legitimately set here, by the slot this write just ran.
     *
     * This register reached the sweep late. A one-wire test asserted the same
     * property and could not fail — the slot sets both status bits after the
     * writable mask is applied, so it passed whether the mask worked or not —
     * and removing it left the mask with no coverage at all until here.
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

    /*
     * And writing one slot's register leaves the others alone. Checked
     * against EMPTY_SLOT rather than slot 1: firmware now aims slot 1's own
     * window at boot (it is one of the machine's own SCSI adapters), so its
     * register no longer sits at the bare hardware reset value this
     * assertion wants for an untouched slot, the same reason the window-case
     * table above keeps the machine's own SCSI slots out of its rotation.
     */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_DEVICE(EMPTY_SLOT))), ==,
                    device_reset[EMPTY_SLOT]);

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

static void test_the_default_machine_has_the_bridge(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The machine with nothing said about it, which is now the machine with
     * the bridge fitted. Every other test here asks for it explicitly, so this
     * is the only one that would notice the default going back.
     *
     * This assertion used to run the other way, guarding the option from
     * quietly becoming the default while the bridge was being built. It is
     * inverted rather than deleted because the risk did not go away, it
     * reversed: what would now be silent is the machine losing the bus every
     * device on it hangs off, and the suite staying green because every other
     * test names the option.
     *
     * It is also a stronger assertion inverted than it was before. Reading the
     * identity as the bridge's own whole word can only happen if the chip is
     * there; the old form could only say the address did not answer with that
     * word, which a refused access and a zero both satisfy.
     */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_IDENTIFICATION)), ==,
                    bridge_identity(BRIDGE_REVISION));
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_CONTROL)), ==, CONTROL_RESET);

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
    g_assert_cmphex(qtest_readl(qts, CFG0(PROBE_SLOT, 0)), ==, PROBE_IDENTITY);

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
     * And a function's is 256 bytes above the one below it. This card has one
     * function, so the assertion is that its function 1 is *empty* —
     * which is what says the function field decodes at all. A model ignoring
     * it would alias every function onto function 0 and answer with the card's
     * identity eight times over, and no test that only reads function 0 could
     * tell the difference.
     */
    g_assert_cmphex(qtest_readl(qts, CFG0(PROBE_SLOT, 1)), ==, NO_CARD);

    /* A slot with no card in it answers all ones, and specifically not zero. */
    g_assert_cmphex(qtest_readl(qts, CFG0(DMA_SLOT, 0)), ==, NO_CARD);
    g_assert_cmphex(qtest_readl(qts, CFG0(EMPTY_SLOT, 0)), ==, NO_CARD);

    qtest_quit(qts);
}

/*
 * Which window each card is reached through, and why those are two separate
 * things.
 *
 * A window belongs to a PCI slot, but what it points at is a PCI address that
 * slot's register supplies, and any card decoding that address answers. So the
 * window a test drives and the slot its card sits in are independent, and this
 * table aims a window at a card somewhere else on purpose.
 *
 * That independence is what keeps the two large windows covered. The layout is
 * two 2 MB windows then six 1 MB ones, and devices 0 and 1 are the large
 * pair — where the machine's own SCSI adapters sit, so no test card can go
 * there.
 * Reaching those windows through cards elsewhere tests them anyway. An earlier
 * version of this table could not, because it read "the window belongs to the
 * slot" as "the window reaches the slot" and so gave the two large windows up.
 *
 * These five pin the whole eight-window table. Both large windows are here
 * because a wrong size shows up in them first. Windows 2, 3 and 7 are here
 * because their offsets encode the sizes of every window below them: window 3
 * sits at 0x700000 and window 7 at 0xb00000 only if windows 0 and 1 are 2 MB
 * and window 2 is 1 MB, so a model with evenly spaced windows puts both of them
 * somewhere else.
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
     * Every window is written before any is read back, which is the part that
     * makes this a test of five separate windows rather than of one. Windows
     * that overlapped in the widget's space, or that all pointed at the same
     * place in PCI space, would each pass a write-then-read-immediately check
     * and fail here, because the last write would be the only one left.
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
     * Move the window a megabyte up and the card is no longer under it.
     * Without this, a model that ignored the base field entirely — mapping
     * every window onto whatever the card's address happened to be — would
     * pass everything above, because every card is aimed where it lives.
     */
    qtest_writel(qts, REG(BRIDGE_DEVICE(PROBE_SLOT)),
                 DEV_MEMORY_SPACE | ((CARD_MMIO(PROBE_SLOT) >> 20) + 1));
    g_assert_cmphex(qtest_readl(qts, WINDOW(PROBE_SLOT) + CARD_SCRATCH), !=, 0xc0ffee00);

    /*
     * And selecting I/O space takes the card out from under it too, since this
     * card's register file is in memory space. A model treating the
     * memory-or-I/O bit as storage would answer here as though it were still
     * memory space, which is the state a slot resets into and so the state
     * every other test in this file leaves it in.
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
 * The two-node machine, and the 64-bit DMA that only reaches its second node
 * when the direct window decodes 64-bit addresses. A DMA from the base I/O
 * bridge (node 0) to node 1's RAM issues a 64-bit address cycle: bits 63:60 are
 * the target widget, bits 47:0 the crosstalk address, and bit 32 of that is the
 * NASID. The 32-bit direct path keeps only bits 0-30, dropping bit 32, so a
 * node-1 target collapses into node 0's RAM. Node 1's RAM lives at NASID<<32.
 */
#define MACHINE_TWO_NODE \
    "-M origin200,io-bridge=on -m 128M " \
    "-object memory-backend-ram,id=ram0,size=64M " \
    "-object memory-backend-ram,id=ram1,size=64M " \
    "-numa node,nodeid=0,cpus=0-1,memdev=ram0 " \
    "-numa node,nodeid=1,cpus=2-3,memdev=ram1 " \
    "-smp 4,sockets=2,cores=2,threads=1"

#define NODE1_PCI_ADDR          0xa000000100bc4000ULL /* widget 0xa, xtalk 0x100bc4000 */
#define NODE1_SYS_ADDR          0x100bc4000ULL        /* node 1 RAM: NASID 1 << 32 + off */
#define NODE0_TRUNCATED_ADDR    0x00bc4000ULL         /* where dropping bit 32 lands it */
#define DMA64_SEED              0x51
#define DMA64_LEN               16

/* The read direction: a copy's 64-bit source in node 1, its destination in node 0. */
#define COPY_SRC_PCI            0xa000000100c00000ULL /* widget 0xa, xtalk 0x100c00000 */
#define COPY_SRC_SYS            0x100c00000ULL        /* node 1 RAM holding the source */
#define COPY_DST_ADDR           0x00300000u           /* a node-0 local destination */

/*
 * A node-0 64-bit cycle whose low crosstalk offset would collide with the direct
 * window's 512 MB step if the offset registers were consulted. A device that
 * addresses by crosstalk leaves the direct window at reset (the step active) and
 * issues a 64-bit address; the 64-bit path must ignore the step, where the 31-bit
 * direct path would add it and miss the target.
 */
#define DMA64_NODE0_PCI         0xa000000000fbc000ULL /* widget 0xa, xtalk 0xfbc000, NASID 0 */
#define DMA64_NODE0_SYS         0x00fbc000ULL         /* node 0 guest-phys, where it must land */

/* Where the transfer source's own registers are put in PCI memory space. */
#define DMA_CARD_MMIO           CARD_MMIO(DMA_SLOT)

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

    /*
     * The head is in the page the first entry names.
     *
     * The exclusive-or is the byte-lane permutation, not this test's subject.
     * The transfer source's swap bits are clear at reset and it transfers
     * through the page-mapped window, so the Bridge reverses the bytes of each
     * aligned word on the way past: the byte written at word offset i arrives
     * at word offset i-exclusive-or-three. The transfer starts word-aligned,
     * so that is the whole of it. What this test is actually about survives
     * unchanged, because the permutation cannot move a byte out of its own
     * four-byte word and therefore cannot move one across the page boundary.
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

    /*
     * And the same transfer lands once the entry is made valid. The
     * exclusive-or is the byte-lane permutation the transfer source gets for
     * having its swap bits clear, the same one the straddle test carries; it
     * is incidental here, and the assertion this test exists for is that the
     * bytes arrive at all.
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
 * The DMA byte-lane swap.
 *
 * Patterns here are at least eight bytes and never palindromic, so that a
 * whole-buffer reversal, an eight-byte-group reversal and the per-word one this
 * chip performs are all distinguishable from each other. Memory is read with
 * qtest_memread rather than a word accessor, because the value accessors apply
 * their own target-versus-host correction and would manufacture exactly the
 * difference under test.
 *
 * Every assertion that bytes are UNCHANGED is made over memory pre-filled with
 * a sentinel, because unchanged is also what a transfer that never happened
 * produces, and a test that cannot tell those apart is not testing anything.
 */
#define DEV_SWAP_TRANSLATED     (1u << 20)
#define DEV_SWAP_DIRECT         (1u << 19)
#define SWAP_SEED               0x40
#define SWAP_LEN                12
#define SWAP_SENTINEL           0xee

/* The transfer source's window register, with whatever swap bits are wanted. */
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
 * A length that is not a multiple of four. The trailing bytes land at the top
 * of their aligned word -- past where the transfer nominally ends -- and the
 * offsets they would have occupied are left holding the sentinel.
 *
 * Seven is the length that matters and six is here to show why. Six leaves a
 * two-byte tail; seven leaves three, which is the only cheap shape that
 * reaches the one-byte arm of the correction.
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
                 * Nothing was written here. For length seven that is offset
                 * four, the byte the transfer names last and the permutation
                 * moves away from -- the one a model truncating its tail
                 * instead of relocating it would have written.
                 */
                g_assert_cmpuint(seen[i], ==, SWAP_SENTINEL);
            }
        }

        qtest_quit(qts);
    }
}

/*
 * A transfer starting off a word boundary. The rule keys on each byte's own
 * address, so the permutation is the same one and it still never leaves the
 * word -- which a model anchoring to the start of the buffer gets wrong for
 * every byte.
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
 * The exemption, set as a per-slot board property rather than probed by
 * identity.
 *
 * The Bridge exempts a slot from the DMA correction when the board's
 * byte-lane-exempt bitmask carries that slot's bit. The test puts one plain
 * device in the slot and exempts it with -global, then runs the same device in
 * the same slot unexempted -- the non-exempt twin is what makes the exempt
 * result mean something, since bytes-identical is also what a test that never
 * wrote the swap bit, or a transfer that never happened, would produce.
 */
#define MASQ_SLOT               7
#define MASQ_CARD               " -device sgi-bridge-dma-test,addr=" \
                                stringify(MASQ_SLOT)
/* Exempt slot 7 -- bit 7 -- from the Bridge's byte-lane correction. */
#define MASQ_EXEMPT             " -global sgi-bridge.byte-lane-exempt=0x80"

static void masq_start(QTestState *qts)
{
    qtest_writel(qts, CFG0(MASQ_SLOT, 0) + 0x10, CARD_MMIO(MASQ_SLOT));
    qtest_writel(qts, CFG0(MASQ_SLOT, 0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | 0x0004);
    qtest_writel(qts, REG(BRIDGE_DEVICE(MASQ_SLOT)),
                 DEV_MEMORY_SPACE | (CARD_MMIO(MASQ_SLOT) >> 20));
}

static void masq_transfer(QTestState *qts, uint32_t addr, uint32_t len)
{
    qtest_writel(qts, WINDOW(MASQ_SLOT) + DMA_REG_ADDRESS, addr);
    qtest_writel(qts, WINDOW(MASQ_SLOT) + DMA_REG_LENGTH, len);
    qtest_writel(qts, WINDOW(MASQ_SLOT) + DMA_REG_SEED, SWAP_SEED);
    qtest_writel(qts, WINDOW(MASQ_SLOT) + DMA_REG_GO, 1);
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

    /*
     * With the bit clear, where a non-exempt device would be permuted.
     */
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
 * The same device, same slot, same transfer, with only the exemption dropped.
 * This one IS permuted, which is what says the exemption above came from the
 * board property rather than from the test never having exercised the
 * correction.
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
 * The one configuration this model does not reproduce, and the message that
 * says so.
 *
 * A guest enabling the DMA swap for a device this model exempts is asking for
 * something the model will not do, so it says so once, on the register write,
 * where the guest's own action is in hand. No guest we boot creates it.
 *
 * The two negatives are the test. A model that logged on every device-register
 * write, or on every write that set any bit, would satisfy the positive alone
 * and be useless -- so the same message must be absent when a non-exempt
 * device gets the same bits, and absent when the exempt device gets a write
 * with those bits clear.
 */
#define UNIMP_FRAGMENT  "exempts from the DMA byte swap"

static bool bridge_log_has_the_message(const char *extra_device,
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
    qtest_writel(qts, REG(BRIDGE_DEVICE(MASQ_SLOT)),
                 DEV_MEMORY_SPACE | (CARD_MMIO(MASQ_SLOT) >> 20) | swap_bits);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(path, &found, NULL, NULL));
    seen = strstr(found, UNIMP_FRAGMENT) != NULL;
    g_assert_cmpint(g_unlink(path), ==, 0);
    return seen;
}

static void test_dma_swap_on_an_exempt_slot_is_reported(const void *data)
{
    /* The configuration itself. */
    g_assert_true(bridge_log_has_the_message(MASQ_CARD MASQ_EXEMPT,
                                             DEV_SWAP_TRANSLATED));
    g_assert_true(bridge_log_has_the_message(MASQ_CARD MASQ_EXEMPT,
                                             DEV_SWAP_DIRECT));

    /*
     * Same bits, same slot, same register write -- a device the model does
     * correct. Nothing to report, because this one works.
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
 * The direct window, which is the one every guest actually drives.
 *
 * Reaching it needs the direct-map register cleared: the add-512-megabytes bit
 * resets SET, and left alone it steps every direct transfer half a gigabyte
 * down from where these tests look for it.
 */
#define PCI_DIRECT_32           0x80000000u

static void bridge_clear_the_direct_map(QTestState *qts)
{
    qtest_writel(qts, REG(BRIDGE_DIRECT_MAP), 0);
}

/*
 * Direct window, its bit clear: permuted, exactly as the page-mapped window is
 * with ITS bit clear. This is the window the firmware's own block-RAM download
 * goes through, so it is the branch that carries the boot.
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
 * THE ONE THAT MATTERS MOST. One device, one setting of its register, two
 * transfers -- one through each window -- coming out in OPPOSITE orders.
 *
 * Three tests here catch a model reading a single swap bit for the whole
 * device -- this one and the two page-mapped tests that name a bit. Measured
 * by building that model rather than reasoned about, after an earlier draft of
 * this comment claimed to be the only one and was wrong.
 *
 * It is still the sharpest of the three, because it is the only one where both
 * orders come out of ONE device under ONE register setting, so it cannot be
 * satisfied by a model that is merely inconsistent between runs. It is also the
 * shape Linux presents, which sets the direct bit and never the page-mapped
 * one.
 *
 * A red here means the model does not match the chip, and is worth stopping
 * for -- but it is not a guest regression, because nothing that boots drives
 * the page-mapped window.
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
 * Local space is never permuted, whichever way the bits are set, because the
 * chip does not carry it to crosstalk at all and so no swapper is in its path.
 * Both settings run: with the bits clear this would be indistinguishable from
 * a model that simply forgot the window.
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
 * The 64-bit direct decode, the assertion this change exists for. A DMA from
 * node 0's bridge to a node-1 address above 4 GiB must land in node 1's RAM.
 * Bit 32 of the crosstalk address is the NASID; the 32-bit direct path keeps
 * only bits 0-30 and drops it, collapsing a node-1 target into node 0's RAM.
 * For a write that is silent corruption of another node's memory, so this
 * asserts both that node 1 received the bytes and that node 0's low RAM — where
 * the dropped bit would have put them — is untouched.
 */
static void test_dma_direct_window_reaches_the_second_node(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_TWO_NODE
                                 " -device sgi-bridge-dma-test,addr=6,bus=pci");
    uint8_t seen[DMA64_LEN], node0[DMA64_LEN];
    unsigned int i;

    bridge_start_the_transfer_source(qts);
    bridge_source_with_swap(qts, DEV_SWAP_DIRECT);

    /* Offset zero and the 512 MB step cleared, so the address decodes to itself. */
    qtest_writel(qts, REG(BRIDGE_DIRECT_MAP), 0);

    /* Program the 64-bit destination: the low half, then the high half. */
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_ADDRESS,
                 (uint32_t)NODE1_PCI_ADDR);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_ADDRESS_HI,
                 NODE1_PCI_ADDR >> 32);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_LENGTH, DMA64_LEN);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_SEED, DMA64_SEED);
    qtest_writel(qts, WINDOW(DMA_SLOT) + DMA_REG_GO, 1);

    /* It reached node 1's RAM at the full address, not truncated into node 0. */
    qtest_memread(qts, NODE1_SYS_ADDR, seen, sizeof(seen));
    for (i = 0; i < DMA64_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(DMA64_SEED + i));
    }

    /* Node 0's low RAM, where dropping bit 32 would have landed it, is clean. */
    qtest_memread(qts, NODE0_TRUNCATED_ADDR, node0, sizeof(node0));
    for (i = 0; i < DMA64_LEN; i++) {
        g_assert_cmpuint(node0[i], ==, 0);
    }

    qtest_quit(qts);
}

/*
 * The read direction of the same decode. A copy reads its source through the
 * bridge, so a 64-bit source above 4 GiB must be fetched from node 1's RAM, not
 * truncated into node 0's. Seed node 1's RAM, copy from that 64-bit source to a
 * node-0 local destination, and the destination holds node 1's bytes only if
 * the read reached node 1.
 */
static void test_dma_direct_window_reads_the_second_node(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_TWO_NODE
                                 " -device sgi-bridge-dma-test,addr=6,bus=pci");
    uint8_t pattern[DMA64_LEN], seen[DMA64_LEN];
    unsigned int i;

    bridge_start_the_transfer_source(qts);
    bridge_source_with_swap(qts, DEV_SWAP_DIRECT);
    qtest_writel(qts, REG(BRIDGE_DIRECT_MAP), 0);

    /* Seed node 1's RAM at the 64-bit source; a truncated read would miss it. */
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

    /* The destination holds node 1's bytes, proving the read reached node 1. */
    qtest_memread(qts, COPY_DST_ADDR, seen, sizeof(seen));
    for (i = 0; i < DMA64_LEN; i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(DMA64_SEED + i));
    }

    qtest_quit(qts);
}

/*
 * A 64-bit cycle must ignore the direct-window offset registers, even the 512 MB
 * step that resets active. A device addressing by crosstalk leaves the direct
 * window alone and issues a 64-bit address; the 31-bit direct path would add the
 * step and send a low offset half a gigabyte away, into unbacked memory that
 * reads back empty. This leaves DIRECT_MAP at reset and asserts the transfer
 * lands at the bare crosstalk offset, not offset by the step.
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

/* The interrupt registers, and the fields these tests drive. */
#define BRIDGE_INT_STATUS       0x100
#define BRIDGE_INT_ENABLE       0x108
#define BRIDGE_INT_MODE         0x118
#define BRIDGE_INT_DEVICE       0x120
#define BRIDGE_INT_HOST(n)      (0x130 + 8 * (n))

#define DMA_REG_INTERRUPT       0x10

/*
 * Where a delivered interrupt lands. The Bridge turns a pin edge into a
 * crosstalk write to the destination Hub's interrupt-modify register; the Hub
 * sets or clears the named bit in its pending bank. The delivery tests program
 * a pin to route to node 0 with a number in the guest range, then read the
 * pending bank back through the Hub's local window — the same window the Hub's
 * own tests read pending through.
 */
#define HUB_LOCAL_WINDOW        (SPACE_XTALK + 0x01000000)
#define PI_INT_PEND0            0x98
#define INT_TEST_HOST_NODE      0
#define INT_TEST_HOST_NUMBER    42


/*
 * The pin the tests drive. A slot's first interrupt pin goes to the chip pin of
 * the same number, so a source in slot 3 drives pin 3 — which is also the pin
 * the real machine's base I/O controller uses for its second interrupt, so the
 * test exercises the one that matters rather than the convenient one.
 */
#define INT_TEST_SLOT           3

/*
 * A write to the timings runs a bit slot, and the slot has finished by the
 * time the write returns. What the slot then answers with depends on where the
 * conversation has got to; this asserts only that it ran and reported itself
 * finished.
 *
 * That ordering is the whole contract for a driver, and getting it wrong is
 * not a wrong value but a hang: every path in the guest's master writes the
 * timings and then spins reading this register until the finished flag
 * appears, so a model that decodes the register and never sets that flag
 * leaves the guest in a loop with nothing to report. An undecoded register
 * would at least fault and say so.
 *
 * The level is not asserted here at all, and that is the division of labour
 * between this test and the ones below it: what the part answers with is their
 * subject, and this one's is only that a slot ran and said so. An earlier
 * version did assert the level, from when nothing was on the bus and the idle
 * high was the whole answer.
 */
static void test_a_write_runs_a_bit_slot(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    uint32_t nic;

    /* The widths mainline uses to reset the bus. */
    qtest_writel(qts, REG(BRIDGE_NIC), NIC_PULSE(520) | NIC_SAMPLE(65));

    nic = qtest_readl(qts, REG(BRIDGE_NIC));
    g_assert_cmphex(nic & NIC_DONE, ==, NIC_DONE);

    /* And the timings read back, so a driver can see what it asked for. */
    g_assert_cmphex(nic & (NIC_PULSE(0x3ff) | NIC_SAMPLE(0xff)), ==,
                    NIC_PULSE(520) | NIC_SAMPLE(65));

    qtest_quit(qts);
}

/*
 * A reset finds the part, and a bit slot does not.
 *
 * This is the assertion that says something is on the bus at all: the wire is
 * open-drain and idles high, so a slot reads one unless the part pulls it
 * down, and the part answers a reset by doing exactly that. A guest reads that
 * zero as presence and will not talk to a bus that does not give it.
 *
 * The two slots are asserted together and that is the point of the test. Each
 * alone is satisfied by a model that answers every slot the same way — one
 * that always reads zero looks like a part answering a reset, and one that
 * always reads one looks like an empty bus. Only the difference between them
 * says the model is reading the timings rather than returning a constant.
 */
/*
 * One slot on the identity bus, and what the wire read. The widths are the
 * ones mainline uses: a long hold resets, a short one writes a one or reads,
 * and a middling one writes a zero. A read and a written one are the same slot
 * — the guest lets the wire go and samples what is there — which is why one
 * helper serves both.
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
 * Drive a whole search and read the part's identity out of it.
 *
 * This is the exchange a guest performs before it will talk to the part at
 * all, and it is asserted here rather than left to the boot because the boot
 * proves it works on one guest with one kernel configuration. The identity
 * that comes back is compared whole: the family code is what a guest matches
 * on to decide the part is a memory it can read, and asserting only that would
 * pass a model that answered the family and then anything at all.
 */
static void test_a_search_returns_the_identity(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    uint64_t rom = 0;
    unsigned int i;

    g_assert_false(NIC_RESET(qts));         /* pulled low: the part is there */

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

    qtest_quit(qts);
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
 * Read a page the way a guest does, and check it every way a guest does.
 *
 * There are three residue checks and a model can pass some while failing
 * others, so all three are asserted. The first covers the request together
 * with the part's redirection answer, catching a part that answered a page
 * nobody asked for. The second covers the block plus the two framing bytes the
 * read protocol appends, which is what the one-wire slave driver validates as
 * it caches the page. The third covers the thirty-two stored bytes alone: the
 * block carries its own check in its last two bytes, and this is the check the
 * board-identity reader runs over the stored bytes before it will trust the
 * part number. The second passes whatever those last two stored bytes hold, so
 * only the third proves the block carries the embedded check a part-number read
 * needs — the case a page of plain data with correct framing silently fails,
 * which is what left the real guest unable to read the part number. Each check
 * requires a fixed residue rather than a value, which is what makes the part
 * send its check bytes inverted — sending them plain lands on a different
 * residue and a guest rejects the page.
 */
/*
 * Assemble the part number the way the positional reader does: two fixed
 * ranges, spaces dropped, terminated. Written out rather than hand-computed so
 * that a change to the page is checked against the derivation a guest performs
 * and not against a string somebody expected it to produce.
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
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    uint8_t exchange[6];
    uint8_t page[34];
    unsigned int i;

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
     * The thirty-two stored bytes on their own — the residue the part-number
     * reader requires. A block of plain data with no embedded check still
     * passes the framing check above, so this is the assertion that catches a
     * part number a guest could read framing-correct but never validate.
     */
    g_assert_cmphex(nic_crc(0, page, sizeof(page) - 2), ==, NIC_CRC_RESIDUE);

    /*
     * The page has two readers and each is asserted separately, because a
     * layout can satisfy one and be invisible to the other — which is the
     * state this page was in.
     *
     * The positional reader spans both pages, so its derivation is reproduced
     * in the test that reads the whole memory rather than here, where only the
     * first page is in hand.
     */
    /*
     * The scanning reader, which finds the part number only through a field
     * name. Asserted at its offset because that offset is the whole point:
     * the name is five bytes and has to end where the positional read begins,
     * or one of the two readers loses.
     */
    g_assert_cmpmem(&page[6], 5, "Part:", 5);
    g_assert_cmpint(page[11 + 12], ==, ';');

    qtest_quit(qts);
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

static void test_each_node_has_its_own_board_identity(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200,io-bridge=on,synthesized-firmware=off -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1");
    uint64_t node0 = nic_search_for_identity_at(qts, BRIDGE_BASE);
    uint64_t node1 = nic_search_for_identity_at(qts,
                                                 BRIDGE_BASE + NODE_STRIDE);

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
 * Read the first page from a part that is already selected, and check it every
 * way a guest does. This is the same page and the same three residues the
 * skip-addressed read asserts; what varies between the two callers is how the
 * part was selected, which is the thing under test.
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
    g_assert_cmpmem(&page[11], 12, "030-1244-001", 12);
}

/*
 * The whole exchange real firmware performs, end to end: find the part, name
 * it, and read its page.
 *
 * The page read this asserts is the same one the skip-addressed test asserts,
 * and that is deliberate rather than duplication — the two differ only in how
 * the part was selected, and selecting it the other way is what the firmware
 * does and what this model did not answer. A suite holding only the
 * skip-addressed read establishes that a page can be read when the part is
 * addressed without being named, which is true and weaker than the property
 * that has to hold.
 *
 * The identity fed to the match is the one the search just returned rather
 * than a literal, so this also asserts the two halves agree: a model that
 * offered one identity to a search and accepted a different one from a match
 * would pass each of them separately.
 */
static void test_a_named_part_reads_back_its_page(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    uint64_t rom;

    g_assert_false(NIC_RESET(qts));
    rom = nic_search_for_identity(qts);
    g_assert_cmphex(rom, ==, 0x360605040302010bULL);

    /* The reset between the search and the match, as firmware sends it. */
    g_assert_false(NIC_RESET(qts));
    nic_match_identity(qts, rom);

    nic_read_first_page(qts);

    qtest_quit(qts);
}

/*
 * Naming an identity that is not this part's leaves it deselected until the
 * next reset.
 *
 * This is what stops the test above from passing a model that never compares
 * the sixty-four bits at all and selects whatever names it. That model reads
 * back a correct page here too, so the pair is what carries the property and
 * neither half does alone. The check is that the wire idles high for the
 * function byte and every byte after it, which is what an unselected part
 * leaves behind.
 */
static void test_naming_another_part_deselects_this_one(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    unsigned int i;

    g_assert_false(NIC_RESET(qts));

    /* One bit different from this part's identity, in the serial number. */
    nic_match_identity(qts, 0x360605040302010bULL ^ (1ULL << 40));

    /* Ask the deselected part to read a page anyway. */
    nic_write_byte(qts, 0xa5);
    nic_write_byte(qts, 0x00);
    nic_write_byte(qts, 0x00);

    /*
     * Six bytes of redirection and check pair, then the whole block: a part
     * that answered would put a zero somewhere in all of that, because the
     * page carries a part number and two checks over it. An open-drain wire
     * with nothing driving it reads high for every one.
     */
    for (i = 0; i < 40; i++) {
        g_assert_cmphex(nic_read_byte(qts), ==, 0xff);
    }

    qtest_quit(qts);
}

/*
 * Read the part's status memory, which is the first thing real firmware asks
 * this part for and is a separate memory from the one holding the part number.
 *
 * Every byte of it reads as unprogrammed, so the bytes alone would pass
 * against a model that ignored the address entirely and against one that put
 * the page boundary in the wrong place. What separates those is the check
 * pair, and it is asserted twice for two different reasons.
 *
 * The read starts partway into a page, so the first check covers four bytes
 * and not eight -- a model ending the page anywhere else lands on a different
 * residue. And the first check covers the command and both address bytes as
 * well as the data while the second covers its page's data alone, because the
 * part clears its check generator at each page boundary. A model using one
 * basis for both fails whichever of the two it did not choose.
 */
static void test_the_status_memory_reads_back_by_the_page(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    uint8_t first[9];
    uint8_t next[10];
    unsigned int i;

    g_assert_false(NIC_RESET(qts));
    nic_write_byte(qts, 0xcc);

    /*
     * Read status, from four bytes into the page that holds the redirection
     * byte for the first data page. A non-zero high address byte is deliberate:
     * with every data byte reading the same, the address is visible only
     * through the check, so a model dropping the high byte passes everything
     * else.
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

    qtest_quit(qts);
}

/*
 * Past the end of the status memory there is nothing to send, and the part
 * leaves the wire alone. This is the same silence an absent part gives, which
 * is the right answer here and is why it is asserted rather than assumed: a
 * model that answered a page for an address it does not have would hand a
 * guest eight plausible bytes and a check that agreed with them.
 */
static void test_status_past_the_end_answers_nothing(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    unsigned int i;

    g_assert_false(NIC_RESET(qts));
    nic_write_byte(qts, 0xcc);

    nic_write_byte(qts, 0xaa);
    nic_write_byte(qts, 0x00);
    nic_write_byte(qts, 0x02);          /* 0x200, past the last status byte */

    for (i = 0; i < 12; i++) {
        g_assert_cmphex(nic_read_byte(qts), ==, 0xff);
    }

    qtest_quit(qts);
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
 * The unframed read returns the same bytes the framed one does.
 *
 * This is the property, and it is asserted as whole pages rather than as
 * something about them, because every weaker version of it was already true of
 * a model that got it wrong. The check each page carries in its own last two
 * bytes is what a board-identity reader validates before trusting a part
 * number, and it belongs to the stored data: the framed command wraps the page
 * in a redirection byte and a check pair, the unframed one sends the bytes and
 * nothing else, and a page's embedded check has to survive both. A model
 * producing that check while framing an answer satisfies the framed read
 * completely and hands the unframed read the raw text with holes where the
 * check belongs.
 *
 * Both pages are compared because the part number spans the first and the
 * second page carries the rest of the assembled string, and because reading
 * straight from one page into the next is what proves the unframed stream puts
 * nothing between them.
 */
/*
 * The scanning reader's own loop, transcribed rather than described: compare a
 * field name at the current position, and on a miss skip past the next
 * semicolon and compare again, until the page runs out.
 *
 * THIS IS THE ASSERTION THE BYTE COMPARISONS CANNOT MAKE, and the page shipped
 * broken for exactly that reason. Every other check here asks whether the right
 * characters are at the right offsets, and they all passed on a page whose
 * field name was unreachable: the first miss at position zero skipped past
 * the terminator at twenty-three and resumed at twenty-four, so offset six
 * was never compared and the lookup returned nothing. A check on where a
 * thing sits cannot express whether the consumer can walk to it, because
 * that depends on what comes before it.
 */
static bool page_field(const uint8_t *page, unsigned int len,
                       const char *name, char *out)
{
    unsigned int pos = 0, n = strlen(name);

    while (pos < len) {
        unsigned int i;

        if (pos + n <= len && memcmp(&page[pos], name, n) == 0) {
            unsigned int j = 0;

            for (i = pos + n; i < len && page[i] != ';'; i++) {
                out[j++] = page[i];
            }
            out[j] = 0;
            return true;
        }
        for (i = pos; i < len && page[i] != ';'; i++) {
            /* skip to the next delimiter */
        }
        if (i >= len) {
            return false;
        }
        pos = i + 1;
    }
    return false;
}

static void test_the_unframed_read_matches_the_framed_one(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    uint8_t framed[64];
    uint8_t plain[64];
    char partnum[26];
    unsigned int i;

    nic_extended_page(qts, 0, &framed[0]);
    nic_extended_page(qts, 32, &framed[32]);

    /*
     * Addressed by name, which is how the firmware does it and which is the
     * case that distinguishes a part tracking the conversation from one
     * dispatching on the byte. 0xf0 means SEARCH ROM before a part is
     * addressed and READ MEMORY after, and 0x55 is MATCH ROM before and a
     * status write after -- the same two bytes, opposite meanings, decided by
     * where in the exchange they arrive. Reaching this read through a skip
     * would exercise a path this firmware never takes and would leave the
     * reused byte untested at the point it is actually ambiguous.
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
     * The vendor manufacturing-data reader rejects the whole record unless
     * page A starts with format 1. Without this byte it returns "page A data
     * not available" before constructing the Part field, so the later field
     * parser sees no BaseIO identity even though the positional bytes below
     * look correct.
     */
    g_assert_cmphex(plain[0], ==, 0x01);

    /*
     * And what both commands return satisfies both of the page's readers.
     *
     * The positional reader is reproduced rather than described: its two fixed
     * ranges, its dropped spaces, and the prefix it compares. Running the
     * derivation is what makes this a test of the value a guest arrives at
     * rather than of bytes sitting where somebody expected them, and the two
     * ranges span both pages, which is why it is asserted here.
     *
     * SAID PLAINLY BECAUSE IT WAS TRIED: this assertion has never been watched
     * failing. Every corruption that would trip it — a displaced part number,
     * a wrong byte within it, a field string moved to the start of the page —
     * trips one of the byte comparisons above or in the page-read test first,
     * and the suite stops there. So it is not the tripwire for this layout and
     * should not be read as one. It earns its place by expressing the guest's
     * own computation, which stays correct if the layout is ever rearranged,
     * where a byte-offset comparison would quietly need rewriting to match.
     */
    partnum_from_page(plain, partnum);
    g_assert_true(g_str_has_prefix(partnum, "030-1244-"));

    /*
     * The scanning reader finds the part number only through a field name, and
     * the name has to end exactly where the positional read begins or one of
     * the two readers loses. That is the arithmetic this page is built on, so
     * it is asserted at its offset rather than searched for.
     */
    g_assert_cmpmem(&plain[6], 5, "Part:", 5);
    g_assert_cmpint(plain[23], ==, ';');

    /*
     * And the scanning reader can actually reach it. Byte five is a semicolon
     * so that this reader's first miss resumes at six rather than skipping the
     * field entirely -- which is the difference between this page working and
     * only looking as though it does, and is invisible to every assertion
     * above.
     */
    g_assert_true(page_field(plain, 30, "Part:", partnum));
    g_assert_cmpstr(partnum, ==, "030-1244-001");

    g_assert_cmpmem(&plain[11], 12, "030-1244-001", 12);
    g_assert_cmphex(nic_crc(0, &plain[0], 32), ==, NIC_CRC_RESIDUE);
    g_assert_cmphex(nic_crc(0, &plain[32], 32), ==, NIC_CRC_RESIDUE);

    qtest_quit(qts);
}

/*
 * Past the pages this part has programmed, the data memory reads as an EPROM
 * nobody wrote to. Asserted because the alternative a model falls into is
 * wrapping back to the start, which would hand a guest a second, spurious copy
 * of the part number at an address that does not hold one.
 */
static void test_the_data_past_the_image_is_unprogrammed(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    unsigned int i;

    g_assert_false(NIC_RESET(qts));
    nic_write_byte(qts, 0xcc);
    nic_write_byte(qts, 0xf0);
    nic_write_byte(qts, 0x40);          /* the first page after the image */
    nic_write_byte(qts, 0x00);

    for (i = 0; i < 32; i++) {
        g_assert_cmphex(nic_read_byte(qts), ==, 0xff);
    }

    qtest_quit(qts);
}

static void test_a_reset_finds_the_part(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    /* The width mainline holds the wire down for to reset it. */
    qtest_writel(qts, REG(BRIDGE_NIC), NIC_PULSE(520) | NIC_SAMPLE(65));
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_NIC)) & NIC_LEVEL, ==, 0);

    /* And the width it uses to read a bit, which nothing answers yet. */
    qtest_writel(qts, REG(BRIDGE_NIC), NIC_PULSE(6) | NIC_SAMPLE(13));
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_NIC)) & NIC_LEVEL, ==,
                    NIC_LEVEL);

    qtest_quit(qts);
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

/*
 * The same pin can carry two sources on this machine: a PCI function's INTx
 * routed by the core, and a directly wired line the machine drives through
 * the chip's GPIO inputs — the base I/O controller's second interrupt travels
 * that way. Each source must hold the pin on its own. A model that kept one
 * level word and let the last writer win would drop a held PCI level the
 * moment the wire beside it fell, and the guest would never service the
 * device still asking.
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
    qtest_set_irq_in(qts, "/machine/bridge", NULL, INT_TEST_SLOT, 1);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==,
                    1u << INT_TEST_SLOT);
    qtest_set_irq_in(qts, "/machine/bridge", NULL, INT_TEST_SLOT, 0);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==,
                    1u << INT_TEST_SLOT);

    /* And the wire holds after the function lets go. */
    qtest_set_irq_in(qts, "/machine/bridge", NULL, INT_TEST_SLOT, 1);
    bridge_drive_pin(qts, 0);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)), ==,
                    1u << INT_TEST_SLOT);

    /* Both down, the pin is down. */
    qtest_set_irq_in(qts, "/machine/bridge", NULL, INT_TEST_SLOT, 0);
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
 * The whole path end to end: a pin's rising edge, with the pin routed and
 * enabled, becomes a crosstalk write that sets the number's bit in the
 * destination Hub's pending bank. The register tests stop at the Bridge and the
 * Hub tests start at its interrupt-modify register; nothing joined the two.
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
 * The enable bit gates delivery. A pin that is routed but whose enable bit is
 * clear sends no packet, however hard the device asserts. This is the state
 * Linux leaves a pin in when it disables it — it clears the enable bit and
 * leaves the host-address register programmed — so a still-asserting device
 * must not keep setting the Hub's pending bit behind the guest's back.
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

/*
 * With the mode bit set the falling edge sends a clearing packet, so the guest
 * need not clear the Hub bit in its own handler. Without this the Hub's pending
 * bit never clears and the processor re-enters the handler forever.
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

/*
 * The fold from a slot's four bus interrupt pins onto the chip's eight.
 *
 * Pins A and C reach the chip pin numbered as the slot; pins B and D reach that
 * number with bit two flipped. Both guests compute this rather than reading it,
 * and they agree: one folds the five legal pin values through a switch onto a
 * two-entry table holding the slot and the slot exclusive-ored with four, the
 * other tests the pin's low bit and picks between the same two. Neither shares
 * code with the other, which is what makes it a fact about the hardware rather
 * than one project's convention.
 *
 * Nothing on this board asserts B or C — the disk controllers use pin A only,
 * and the I/O controller's second interrupt does not travel as a bus interrupt
 * at all — so a model folding every pin onto the slot passes every boot this
 * fork runs. It is still wrong, and a card that used pin B would deliver to the
 * pin its neighbour four slots away owns.
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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/bridge/identity", NULL, test_identity);
    qtest_add_data_func("sn0/bridge/identity-follows-the-machine", NULL,
                        test_identity_follows_the_machines_revision);
    qtest_add_data_func("sn0/bridge/upper-half-of-the-slot", NULL,
                        test_upper_half_of_the_slot);
    qtest_add_data_func("sn0/bridge/reserved-data-half-store", NULL,
                        test_reserved_data_half_store_completes);
    qtest_add_data_func("sn0/bridge/reset-state", NULL, test_reset_state);
    qtest_add_data_func("sn0/bridge/each-slot-resets-to-its-own-window", NULL,
                        test_each_slot_resets_to_its_own_window);
    qtest_add_data_func("sn0/bridge/ioc3-slot-reassigned-after-reset", NULL,
                        test_ioc3_slot_is_reassigned_after_reset);
    qtest_add_data_func("sn0/bridge/scsi-slots-reassigned-after-reset", NULL,
                        test_scsi_slots_are_reassigned_after_reset);
    qtest_add_data_func("sn0/bridge/ioc3-slot-survives-a-later-reset", NULL,
                        test_ioc3_slot_survives_a_later_reset);
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
    qtest_add_data_func("sn0/bridge/one-wire-status-past-the-end", NULL,
                        test_status_past_the_end_answers_nothing);
    qtest_add_data_func("sn0/bridge/one-wire-unframed-matches-framed", NULL,
                        test_the_unframed_read_matches_the_framed_one);
    qtest_add_data_func("sn0/bridge/one-wire-data-past-the-image", NULL,
                        test_the_data_past_the_image_is_unprogrammed);
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
    qtest_add_data_func("sn0/bridge/dma-direct-window-reaches-the-second-node",
                        NULL, test_dma_direct_window_reaches_the_second_node);
    qtest_add_data_func("sn0/bridge/dma-direct-window-reads-the-second-node",
                        NULL, test_dma_direct_window_reads_the_second_node);
    qtest_add_data_func("sn0/bridge/dma-direct-window-64bit-ignores-the-offset-step",
                        NULL, test_dma_direct_window_64bit_ignores_the_offset_step);
    qtest_add_data_func("sn0/bridge/per-pin-host-registers", NULL,
                        test_the_per_pin_host_registers);

    return g_test_run();
}
