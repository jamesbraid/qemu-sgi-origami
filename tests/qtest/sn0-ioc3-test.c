/*
 * SGI IOC3 base I/O controller: the serial console's access contract.
 *
 * The console is a pair of byte-wide register files inside a window that is
 * otherwise thirty-two bits wide, and the datasheet says plainly that assuming
 * symmetry with the chip's other blocks produces a console that emits nothing.
 * The stride and the width are what this file checks, because they are the
 * parts a working boot does not prove: a guest that only ever issues byte
 * accesses behaves identically whether or not the model has the width right,
 * so the transcript cannot tell the difference. Only a test issuing what the
 * guest does not can.
 *
 * The last assertion here is the one that ties the device to the firmware. The
 * configuration graph tells a guest where its console is, and the device has to
 * answer at exactly that address. Those two facts live in different files and
 * were wrong together three times before they were right, so the test reads the
 * address out of the graph rather than restating it.
 *
 * The controller is a PCI device on the I/O bridge's bus, so every test here
 * asks for the bridge explicitly: without it there is no bus for the
 * controller to sit on and nothing answers at its crosstalk address at all.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* Crosstalk space, and the chip's window inside the console widget. */
#define SPACE_XTALK     0x20000000000ULL
#define CONSOLE_WIDGET  8
#define BRIDGE_DEVICE2  0x600000
#define IOC3_BASE       (SPACE_XTALK + ((uint64_t)CONSOLE_WIDGET << 24) + \
                         BRIDGE_DEVICE2)

/*
 * The controller is a PCI device now, reached through the bridge's third
 * device window rather than mapped directly, so a test has to do the two
 * things a guest's own bus scan would do before that window reaches
 * anything: give the device a PCI address by writing its BAR, and aim the
 * window at that same address. The window's crosstalk-side offset is fixed
 * by slot number, which is why IOC3_BASE above still names the right place
 * to read the result from — only the PCI-side address in between is this
 * test's to choose.
 */
#define BRIDGE_BASE     (SPACE_XTALK + ((uint64_t)CONSOLE_WIDGET << 24))
#define REG_DATA        4         /* registers sit at a slot's upper half */
#define BRIDGE_CFG0     0x020000
#define CFG0_DEVICE_STRIDE 0x1000
#define CFG0(device)    (BRIDGE_BASE + BRIDGE_CFG0 + (device) * CFG0_DEVICE_STRIDE)
#define BRIDGE_DEVICE(n) (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)
#define DEV_MEMORY_SPACE (1u << 12)

#define PCI_IOC3_DEVICE  2         /* SN0_IOC3_DEVICE, restated for the test */
#define PCI_COMMAND      0x04
#define PCI_COMMAND_MEMORY 0x0002
#define PCI_BAR0         0x10
#define IOC3_PCI_MMIO    0x10000000u

static void sn0_ioc3_attach(QTestState *qts)
{
    qtest_writel(qts, CFG0(PCI_IOC3_DEVICE) + PCI_BAR0, IOC3_PCI_MMIO);
    qtest_writel(qts, CFG0(PCI_IOC3_DEVICE) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(PCI_IOC3_DEVICE),
                 DEV_MEMORY_SPACE | (IOC3_PCI_MMIO >> 20));
}

/* The two ports, at their offsets inside the byte-wide SuperIO window. */
#define SERIAL_A        (IOC3_BASE + 0x020178)
#define SERIAL_B        (IOC3_BASE + 0x020170)

/* 16550 registers this test touches, by offset from a port's base. */
/* The SuperIO interrupt block, in the chip's main window. */
#define INT_STATUS      (IOC3_BASE + 0x1c)
#define INT_ENABLE_SET  (IOC3_BASE + 0x20)
#define INT_ENABLE_CLR  (IOC3_BASE + 0x24)

#define INT_SERIAL_A    (1u << 6)
#define INT_SERIAL_B    (1u << 15)
#define INT_KEYBOARD    (1u << 22)   /* a latched bit, for contrast */

#define REG_LCR         3
#define REG_MCR         4
#define REG_SCR         7

/* The MicroLAN engine and the pin block it shares a window with. */
#define MICROLAN_CONTROL (IOC3_BASE + 0x30)
#define MICROLAN_LEVEL   (1u << 0)
#define MICROLAN_DONE    (1u << 1)

/*
 * The timing OpenBSD's bit-banging routine writes for a bus reset and
 * presence detect: 500 microseconds holding the bus low (bits 10 and up),
 * 65 microseconds of remainder (bits 2 and up) — see docs/datasheets/ioc3.md's
 * "GPIO and MicroLAN" section. A real guest's reset slot, not an arbitrary
 * all-zero write.
 */
#define MICROLAN_RESET_SLOT ((500u << 10) | (65u << 2))

#define PIN_CONTROL_SET  (IOC3_BASE + 0x34)
#define PIN_CONTROL_CLR  (IOC3_BASE + 0x38)
#define PIN_MICROLAN_ENABLE (1u << 21)

#define PIN_DATA         (IOC3_BASE + 0x3c)
#define PIN_REG(n)       (IOC3_BASE + 0x40 + 4 * (n))

/* Where the graph keeps the console descriptor, and its serial-port base. */
#define CONFIG_GRAPH    0x4000
#define GRAPH_CONSOLE   0x18

static void test_ports_are_distinct(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * The scratch register holds whatever it is given, so it is the cheapest
     * place to prove the two files are two files. If the ports aliased, the
     * second write would be visible through the first.
     */
    qtest_writeb(qts, SERIAL_A + REG_SCR, 0x5a);
    qtest_writeb(qts, SERIAL_B + REG_SCR, 0xa5);

    g_assert_cmphex(qtest_readb(qts, SERIAL_A + REG_SCR), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, SERIAL_B + REG_SCR), ==, 0xa5);

    qtest_quit(qts);
}

static void test_registers_are_a_byte_apart(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * Line control and modem control are adjacent and both hold what they are
     * given, so writing different values to consecutive offsets and reading
     * both back proves the stride is one byte. At the four-byte stride the
     * chip's other blocks use, these two writes would land in the same
     * register and the second would win.
     *
     * The line-control value is eight data bits with one stop bit and the
     * divisor latch not selected, which is what a guest writes and what keeps
     * the neighbouring offsets meaning what this test expects.
     */
    qtest_writeb(qts, SERIAL_A + REG_LCR, 0x03);
    qtest_writeb(qts, SERIAL_A + REG_MCR, 0x05);

    g_assert_cmphex(qtest_readb(qts, SERIAL_A + REG_LCR), ==, 0x03);
    g_assert_cmphex(qtest_readb(qts, SERIAL_A + REG_MCR), ==, 0x05);

    qtest_quit(qts);
}

static void test_a_wide_access_is_not_narrowed(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    uint32_t wide;

    sn0_ioc3_attach(qts);

    /*
     * Four distinct bytes across four consecutive registers, then one four-byte
     * read of the same place.
     *
     * What this asserts is negative on purpose. The datasheet records that
     * nothing establishes what the hardware does with an access wider than a
     * byte, so the model refuses it rather than inventing a result, and that
     * refusal is a choice the datasheet marks as a choice. What must not happen
     * is the silent narrowing the register file's own wrapper performs, where a
     * four-byte read returns one register's value as though it were the word.
     * That is an unknown presented as known, the one behaviour with nothing to
     * recommend it.
     *
     * So the test does not assert what a wide read returns. It asserts that the
     * answer is not the narrowed one, which is the only thing that can be said
     * without claiming to know something nobody has observed.
     */
    qtest_writeb(qts, SERIAL_A + REG_LCR, 0x03);
    qtest_writeb(qts, SERIAL_A + REG_MCR, 0x05);

    wide = qtest_readl(qts, SERIAL_A + REG_LCR);
    g_assert_cmphex(wide, !=, 0x03);
    g_assert_cmphex(wide, !=, 0x03000000);

    qtest_quit(qts);
}

static void test_port_answers_where_the_graph_says(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    uint64_t advertised = qtest_readq(qts, CONFIG_GRAPH + GRAPH_CONSOLE);
    uint64_t reached;

    sn0_ioc3_attach(qts);

    /*
     * The single assertion joining this device to the firmware that describes
     * it. The graph's console descriptor is what a guest reads to find its
     * console, and it has been wrong three times: once holding the value that
     * merely identifies the controller generation, once holding the right
     * register offset with no device window above it, and once right. Each
     * wrong value produced a machine that identified its console correctly and
     * then wrote to something that was not one.
     *
     * Reading the address out of the graph rather than restating it keeps the
     * two from drifting apart: if either the descriptor or the mapping moves,
     * this fails. Restating the constant here would have passed against every
     * one of the three wrong values.
     */
    /*
     * The two are in different address spaces and the difference is the point.
     * The graph holds an address a *guest* issues: an unmapped 64-bit window
     * reference carrying the uncached attribute that routes it to a crosstalk
     * widget. A test pokes the machine's own bus, where that attribute has
     * already been turned into a window. So the descriptor is decoded here the
     * way the processor decodes it, attribute 1 selecting the crosstalk window
     * and the address below it being the node offset, and the result has to be
     * where the device was mapped.
     *
     * Comparing them undecoded would fail on a correct machine, and asserting
     * only the raw value would pass on a machine whose device sat somewhere
     * else entirely. Decoding is what makes this one assertion cover both the
     * firmware's claim and the machine's placement.
     */
    g_assert_cmphex(advertised >> 62, ==, 2);            /* unmapped window */
    g_assert_cmphex((advertised >> 57) & 3, ==, 1);      /* crosstalk I/O */
    reached = SPACE_XTALK | (advertised & ((1ULL << 40) - 1));
    g_assert_cmphex(reached, ==, SERIAL_A);

    /* And something answers there, which no assertion about the graph shows. */
    qtest_writeb(qts, reached + REG_SCR, 0x3c);
    g_assert_cmphex(qtest_readb(qts, reached + REG_SCR), ==, 0x3c);

    qtest_quit(qts);
}

static void test_the_enable_mask_has_two_write_aliases(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * One mask behind two addresses, so a guest can turn a source on or off
     * without a read-modify-write. The set alias reads back the live mask,
     * which is what says they are two views of one register rather than two
     * registers — a model with two independent words would pass any test that
     * only ever wrote and read the same address.
     */
    g_assert_cmphex(qtest_readl(qts, INT_ENABLE_SET), ==, 0);

    qtest_writel(qts, INT_ENABLE_SET, INT_SERIAL_A | INT_SERIAL_B);
    g_assert_cmphex(qtest_readl(qts, INT_ENABLE_SET), ==,
                    INT_SERIAL_A | INT_SERIAL_B);

    /* Clearing through the other alias reaches the same mask. */
    qtest_writel(qts, INT_ENABLE_CLR, INT_SERIAL_B);
    g_assert_cmphex(qtest_readl(qts, INT_ENABLE_SET), ==, INT_SERIAL_A);

    /* And a one in the set alias does not disturb what is already there. */
    qtest_writel(qts, INT_ENABLE_SET, INT_KEYBOARD);
    g_assert_cmphex(qtest_readl(qts, INT_ENABLE_SET), ==,
                    INT_SERIAL_A | INT_KEYBOARD);

    qtest_quit(qts);
}

static void test_a_serial_bit_ignores_an_acknowledgement(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * The two serial bits are pass-throughs: they follow the 16550's own
     * interrupt level rather than latching, so writing one back cannot clear
     * it. This is the assertion that catches a model treating the whole status
     * register as write-one-to-clear — which looks right until a guest's
     * handler writes back everything it serviced, drops a level that is still
     * asserted, and never takes that interrupt again.
     *
     * The port has to be actually asserting for this to test anything, and the
     * first version of this test did not arrange that: with the port quiet the
     * bit is zero, clearing zero and ignoring zero are the same, and the test
     * passed against a model that cleared the bit. It was written with a
     * comment explaining why the bit might be zero, which is the tell — an
     * assertion whose own comment excuses the case that makes it vacuous.
     *
     * So the port is made to assert first. Enabling the transmitter-empty
     * interrupt raises the 16550 immediately, because its holding register is
     * empty from reset and stays empty with nothing being sent.
     */
    qtest_writeb(qts, SERIAL_A + 1, 0x02);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_SERIAL_A, ==,
                    INT_SERIAL_A);

    /* Now the acknowledgement, which must not touch it. */
    qtest_writel(qts, INT_STATUS, INT_SERIAL_A);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_SERIAL_A, ==,
                    INT_SERIAL_A);

    /*
     * And what does quiet it is the port, not this register: turning the
     * 16550's own interrupt off drops the level, and the pass-through bit
     * follows it down with nothing written here at all.
     */
    qtest_writeb(qts, SERIAL_A + 1, 0x00);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_SERIAL_A, ==, 0);

    qtest_quit(qts);
}

static void test_a_one_wire_slot_always_finishes(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * This is the assertion #47 exists for. A guest issues a one-wire time
     * slot by writing the hold and remainder timing into this register and
     * then polls it for the done bit — real hardware takes microseconds,
     * this model takes none, but either way a slot that never finishes is a
     * guest that polls forever. Reset value has the bit clear; any write at
     * all must bring it back with both control bits set, regardless of what
     * timing the guest asked for.
     */
    g_assert_cmphex(qtest_readl(qts, MICROLAN_CONTROL) & MICROLAN_DONE, ==,
                    0);

    qtest_writel(qts, MICROLAN_CONTROL, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, MICROLAN_CONTROL) & MICROLAN_DONE, ==,
                    MICROLAN_DONE);

    qtest_quit(qts);
}

static void test_a_one_wire_slot_reports_no_part(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * No part answers this bus yet (task #13 is the separate work of
     * fitting one), and the level a guest samples is how "no part" reaches
     * a driver: an idle one-wire bus stays high, so every slot — reset,
     * read, write — has to report a high level rather than one that
     * happens to spell "a device pulled the line low", which a guest's own
     * presence check would read as a device being there.
     *
     * The slot written is a real reset-and-presence-detect timing, not an
     * all-zero write. The model as written sets the level unconditionally
     * regardless of the timing bits, so an all-zero write cannot tell this
     * test apart from one that classified the pulse width and got a reset
     * slot right for the wrong reason — it would keep passing if the model
     * grew that classification and got only the all-zero case right. Using
     * the encoding a real guest issues (docs/datasheets/ioc3.md's "GPIO
     * and MicroLAN" section) is the version of this test that stays
     * meaningful if the model ever does more than answer unconditionally.
     */
    qtest_writel(qts, MICROLAN_CONTROL, MICROLAN_RESET_SLOT);
    g_assert_cmphex(qtest_readl(qts, MICROLAN_CONTROL) & MICROLAN_LEVEL, ==,
                    MICROLAN_LEVEL);

    qtest_quit(qts);
}

static void test_one_wire_slot_preserves_the_timing_written(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    uint32_t timing = 0x12345678 & ~(MICROLAN_LEVEL | MICROLAN_DONE);

    sn0_ioc3_attach(qts);

    /*
     * Bits 0 and 1 are hardware's to set, not a mask the guest's own value
     * happens to already carry — this writes a pattern with both of them
     * set in the guest's own value and checks the readback still shows
     * exactly what hardware decides, not a blind copy of the write.
     */
    qtest_writel(qts, MICROLAN_CONTROL, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, MICROLAN_CONTROL), ==,
                    timing | MICROLAN_LEVEL | MICROLAN_DONE);

    qtest_quit(qts);
}

static void test_pin_control_has_two_write_aliases(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * The same shape as the interrupt-enable pair: a guest sets or clears
     * bits without a read-modify-write, and the set alias reads back the
     * live mask. Bit 21 is the one both guests actually use — routing the
     * MicroLAN engine onto pin 8 — but the test exercises the mechanism
     * generally rather than just that one bit, the same reasoning the
     * interrupt-enable test already applies.
     */
    g_assert_cmphex(qtest_readl(qts, PIN_CONTROL_SET), ==, 0);

    qtest_writel(qts, PIN_CONTROL_SET, PIN_MICROLAN_ENABLE);
    g_assert_cmphex(qtest_readl(qts, PIN_CONTROL_SET), ==,
                    PIN_MICROLAN_ENABLE);

    qtest_writel(qts, PIN_CONTROL_SET, 1u << 5);
    g_assert_cmphex(qtest_readl(qts, PIN_CONTROL_SET), ==,
                    PIN_MICROLAN_ENABLE | (1u << 5));

    qtest_writel(qts, PIN_CONTROL_CLR, PIN_MICROLAN_ENABLE);
    g_assert_cmphex(qtest_readl(qts, PIN_CONTROL_SET), ==, 1u << 5);

    qtest_quit(qts);
}

static void test_a_pin_register_reaches_the_shared_data_word(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * Pin register 3 is a convenience alias onto bit 3 of the shared data
     * word, not storage of its own — writing through it and reading back
     * through the data register is what proves that, the same reasoning
     * the ports-are-distinct test applies to the two serial files: two
     * addresses reaching one place, not two places that happen to agree.
     */
    qtest_writel(qts, PIN_REG(3), 1);
    g_assert_cmphex(qtest_readl(qts, PIN_DATA), ==, 1u << 3);
    g_assert_cmphex(qtest_readl(qts, PIN_REG(3)), ==, 1);

    /* And the reverse: writing the data register moves the pin register. */
    qtest_writel(qts, PIN_DATA, 0);
    g_assert_cmphex(qtest_readl(qts, PIN_REG(3)), ==, 0);

    /* A neighbouring pin is untouched by either. */
    g_assert_cmphex(qtest_readl(qts, PIN_REG(4)), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/ioc3/ports-are-distinct", NULL,
                        test_ports_are_distinct);
    qtest_add_data_func("sn0/ioc3/registers-are-a-byte-apart", NULL,
                        test_registers_are_a_byte_apart);
    qtest_add_data_func("sn0/ioc3/wide-access-is-not-narrowed", NULL,
                        test_a_wide_access_is_not_narrowed);
    qtest_add_data_func("sn0/ioc3/enable-mask-two-aliases", NULL,
                        test_the_enable_mask_has_two_write_aliases);
    qtest_add_data_func("sn0/ioc3/serial-bit-ignores-acknowledgement", NULL,
                        test_a_serial_bit_ignores_an_acknowledgement);
    qtest_add_data_func("sn0/ioc3/port-answers-where-the-graph-says", NULL,
                        test_port_answers_where_the_graph_says);
    qtest_add_data_func("sn0/ioc3/one-wire-slot-always-finishes", NULL,
                        test_a_one_wire_slot_always_finishes);
    qtest_add_data_func("sn0/ioc3/one-wire-slot-reports-no-part", NULL,
                        test_a_one_wire_slot_reports_no_part);
    qtest_add_data_func("sn0/ioc3/one-wire-slot-preserves-timing", NULL,
                        test_one_wire_slot_preserves_the_timing_written);
    qtest_add_data_func("sn0/ioc3/pin-control-two-aliases", NULL,
                        test_pin_control_has_two_write_aliases);
    qtest_add_data_func("sn0/ioc3/pin-register-reaches-data-word", NULL,
                        test_a_pin_register_reaches_the_shared_data_word);

    return g_test_run();
}
