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

/*
 * The Bridge's own interrupt status register, task 42's destination: where the
 * controller's second interrupt has to land once it leaves this chip. Task 44
 * settled that a device window's own byte lanes need no correction for a
 * whole-register access like this one, so this is the same plain address a
 * bridge-side test reads it at.
 */
#define BRIDGE_INT_STATUS (BRIDGE_BASE + 0x100 + REG_DATA)
#define BRIDGE_PIN_SUPERIO (1u << 3)   /* SN0_IOC3_SUPERIO_PIN, restated */

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

static void test_the_second_interrupt_reaches_bridge_pin_3(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * The controller's second interrupt (task 42) — the one this chip's
     * superio_irq output carries — has no PCI INTx of its own to travel on,
     * so nothing here reaches this pin unless the board's own direct wire is
     * modelled. Before that wire exists this test fails exactly the way the
     * task's own bug report does: the bit never sets.
     */
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_SUPERIO,
                    ==, 0);

    /*
     * Arm the local interrupt the same way the acknowledgement test above
     * does — the enable bit alone raises nothing, since int_status is still
     * clear, which is why this line does not yet reach the assertion below.
     */
    qtest_writel(qts, INT_ENABLE_SET, INT_SERIAL_A);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_SUPERIO,
                    ==, 0);

    /*
     * Now the port itself asserts, which is what actually drives superio_irq
     * (int_status & int_enable, both live for the first time). This is the
     * whole chain the task exists to close: a real IOC3 interrupt source, to
     * this chip's own combined status, out its second interrupt pin, across
     * the wire task 42 adds, and into the Bridge's own pin 3.
     */
    qtest_writeb(qts, SERIAL_A + 1, 0x02);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_SUPERIO,
                    ==, BRIDGE_PIN_SUPERIO);

    /*
     * And it is live, not latched, the same as the local status bit already
     * is: the source dropping its level is what clears the pin, with nothing
     * written to either chip's status register.
     */
    qtest_writeb(qts, SERIAL_A + 1, 0x00);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_SUPERIO,
                    ==, 0);

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

/* The PS/2 controller's five registers. */
#define KBC_CONTROL       (IOC3_BASE + 0x09c)
#define KBC_KBD_RECEIVE   (IOC3_BASE + 0x0a0)
#define KBC_AUX_RECEIVE   (IOC3_BASE + 0x0a4)
#define KBC_KBD_TRANSMIT  (IOC3_BASE + 0x0a8)

static void test_the_ps2_ports_read_empty(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * Nothing is plugged into either port, and each receive register carries a
     * valid flag per byte in its top bits. Zero clears all of them, so this is
     * a port that has received nothing rather than one holding three zero
     * bytes -- a distinction a driver acts on, since it reads the flags to
     * decide whether there is anything to take.
     *
     * A write is issued first: these are read-only, so if the model let one
     * through, the following read would return it and the assertion would
     * catch both faults at once.
     */
    qtest_writel(qts, KBC_KBD_RECEIVE, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE), ==, 0);
    qtest_writel(qts, KBC_AUX_RECEIVE, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, KBC_AUX_RECEIVE), ==, 0);

    /*
     * The control register is the one a driver both sets and reads back, so it
     * is asserted in both directions. The bits hardware owns in it report a
     * write in progress and a state machine not yet idle; this model never
     * sets either, which is what lets a driver's poll after a transmit finish
     * on its first look instead of waiting out a timeout.
     */
    qtest_writel(qts, KBC_CONTROL, 0x00000005);
    g_assert_cmphex(qtest_readl(qts, KBC_CONTROL), ==, 0x00000005);

    /* A transmit register takes a byte and carries it nowhere, which must
     * still complete -- a refused write would report a fault where the real
     * chip reports an unplugged port. */
    qtest_writel(qts, KBC_KBD_TRANSMIT, 0xed);
    g_assert_cmphex(qtest_readl(qts, KBC_CONTROL), ==, 0x00000005);

    qtest_quit(qts);
}

/* The serial DMA block: the shared ring base, then each port's registers. */
#define SDMA_RING_LO      (IOC3_BASE + 0x0b4)
#define SDMA_A_TX_PRODUCE (IOC3_BASE + 0x0bc)
#define SDMA_A_TX_CONSUME (IOC3_BASE + 0x0c0)
#define SDMA_A_RX_PRODUCE (IOC3_BASE + 0x0c4)
#define SDMA_A_RX_CONSUME (IOC3_BASE + 0x0c8)
#define SDMA_B_TX_PRODUCE (IOC3_BASE + 0x0d8)
#define SDMA_B_TX_CONSUME (IOC3_BASE + 0x0dc)

static void test_the_serial_dma_indices_by_owner(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * The four index registers are the point of this block and they are not
     * alike: software advances the ones it owns and hardware advances the
     * others, so two of the four take a write and two refuse it. They sit
     * adjacent and differ by one word, which is exactly the arrangement a
     * model gets wrong by treating the run of them as uniform storage.
     *
     * Both directions are asserted for the same pair, because a model that
     * made every register writable and one that made every register read-only
     * would each pass half of this and neither would pass both.
     */
    qtest_writel(qts, SDMA_A_TX_PRODUCE, 0x40);
    g_assert_cmphex(qtest_readl(qts, SDMA_A_TX_PRODUCE), ==, 0x40);
    qtest_writel(qts, SDMA_A_RX_CONSUME, 0x48);
    g_assert_cmphex(qtest_readl(qts, SDMA_A_RX_CONSUME), ==, 0x48);

    qtest_writel(qts, SDMA_A_TX_CONSUME, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SDMA_A_TX_CONSUME), ==, 0);
    qtest_writel(qts, SDMA_A_RX_PRODUCE, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SDMA_A_RX_PRODUCE), ==, 0);

    qtest_quit(qts);
}

static void test_the_serial_dma_ports_are_distinct(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * Two ports, one register layout, twenty-eight bytes apart. Writing each
     * port's transmit produce index a different value proves the second block
     * is a second block rather than an alias of the first -- an off-by-one in
     * the port stride would put both writes in the same place, and a model
     * with only one port's registers would answer both from it.
     */
    qtest_writel(qts, SDMA_A_TX_PRODUCE, 0x18);
    qtest_writel(qts, SDMA_B_TX_PRODUCE, 0x28);
    g_assert_cmphex(qtest_readl(qts, SDMA_A_TX_PRODUCE), ==, 0x18);
    g_assert_cmphex(qtest_readl(qts, SDMA_B_TX_PRODUCE), ==, 0x28);

    /* The ownership split repeats on the second port, at its own offsets. */
    qtest_writel(qts, SDMA_B_TX_CONSUME, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SDMA_B_TX_CONSUME), ==, 0);

    /* The ring base is shared by both ports and is software's to set. */
    qtest_writel(qts, SDMA_RING_LO, 0x00003000);
    g_assert_cmphex(qtest_readl(qts, SDMA_RING_LO), ==, 0x00003000);

    qtest_quit(qts);
}

/* The ethernet block's registers, at their offsets in the chip's window. */
#define ENET_INT_STATUS   (IOC3_BASE + 0x0f4)
#define ENET_INT_ENABLE   (IOC3_BASE + 0x0f8)
#define ENET_COUNTERS     (IOC3_BASE + 0x120)
#define ENET_BUFFER_INDEX (IOC3_BASE + 0x124)
#define ENET_MII_CONTROL  (IOC3_BASE + 0x148)
#define ENET_MII_DATA_R   (IOC3_BASE + 0x14c)

static void test_the_ethernet_interrupt_register_acknowledges(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * A write clears the sources it names instead of setting them. This is the
     * one register in the block where storage would look like it worked: a
     * driver writes back the bits it just read, and a model that stored them
     * would leave every source asserted forever while every read still
     * returned something plausible.
     *
     * Asserting from zero is what makes the direction observable. Storage
     * would leave the written value behind; acknowledgement leaves zero, and
     * only one of those two can be true of the same write.
     */
    qtest_writel(qts, ENET_INT_STATUS, 0x00010001);
    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS), ==, 0);

    /*
     * The enable register beside it takes the same bit numbering and is
     * ordinary storage, so the pair is the contrast: identical layout,
     * opposite behaviour on write. A model that treated both the same way
     * would fail exactly one of these two assertions whichever way it chose.
     */
    qtest_writel(qts, ENET_INT_ENABLE, 0x00010001);
    g_assert_cmphex(qtest_readl(qts, ENET_INT_ENABLE), ==, 0x00010001);

    qtest_quit(qts);
}

static void test_the_ethernet_read_only_registers(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * The collision and deferral counters are cleared by being read, which is
     * how both guests zero them during reset. Nothing here has sent a frame so
     * they are zero already; what this pins is that reading them is permitted
     * and answers, since a driver's reset path stops on a fault here.
     */
    g_assert_cmphex(qtest_readl(qts, ENET_COUNTERS), ==, 0);

    /* Software cannot place the block's engines, so a write does not take. */
    qtest_writel(qts, ENET_BUFFER_INDEX, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, ENET_BUFFER_INDEX), ==, 0);

    qtest_quit(qts);
}

static void test_the_media_interface_never_reads_busy(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * A driver starts a read and then polls the busy bit until it clears,
     * giving up with a timeout. Nothing here takes time, so busy must already
     * be clear on the first look: a model that left it set would hang the
     * driver's poll for its whole timeout on every register it reads, and a
     * driver scanning thirty-two addresses does that thirty-two times.
     *
     * The read is issued first rather than asserting on an idle register,
     * because the bit that has to be clear is the one after a read was asked
     * for -- asserting it on an untouched register would pass on a model that
     * sets busy and never clears it.
     */
    qtest_writel(qts, ENET_MII_CONTROL, (1u << 10));
    g_assert_cmphex(qtest_readl(qts, ENET_MII_CONTROL) & (1u << 11), ==, 0);

    /* No physical-layer chip answers, and an absent one reads all-ones. */
    g_assert_cmphex(qtest_readl(qts, ENET_MII_DATA_R), ==, 0xffff);

    qtest_quit(qts);
}

/*
 * The ethernet buffer RAM window, and the offsets the three drivers that size
 * it actually touch. SGI's own driver and OpenBSD's put their second word at
 * the halfway point of the window; mainline Linux puts its a quarter of the
 * way in, which is inside the smaller RAM and so cannot fail on a chip that
 * has one. Both pairs are asserted here because both are issued at us, and
 * the halfway pair is asserted because it is the only one that fails on a
 * window aliasing at its midpoint.
 */
#define SSRAM_FIRST       (IOC3_BASE + 0x40000)
#define SSRAM_LINUX_PAIR  (IOC3_BASE + 0x50000)
#define SSRAM_HALFWAY     (IOC3_BASE + 0x60000)
#define SSRAM_LAST        (IOC3_BASE + 0x7fffc)

/* The pattern OpenBSD's driver writes, and the complement it writes second. */
#define SSRAM_PATTERN     0x55aau
#define SSRAM_COMPLEMENT  (0xffffu ^ SSRAM_PATTERN)

static void test_the_buffer_ram_halves_are_distinct(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * The size probe in full, as OpenBSD performs it: a pattern at the first
     * word, its complement at the halfway word, then both read back. The
     * driver compares the second read as a whole thirty-two-bit word rather
     * than through the data mask, which is why this asserts the whole word
     * too -- anything the model left in the bits above the data would send a
     * driver that finds 128 KB of RAM down the branch for 64 KB.
     */
    qtest_writel(qts, SSRAM_FIRST, SSRAM_PATTERN);
    qtest_writel(qts, SSRAM_HALFWAY, SSRAM_COMPLEMENT);

    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==, SSRAM_PATTERN);
    g_assert_cmphex(qtest_readl(qts, SSRAM_HALFWAY), ==, SSRAM_COMPLEMENT);

    /* Mainline Linux's second word, a quarter of the way in, as well. */
    qtest_writel(qts, SSRAM_LINUX_PAIR, SSRAM_COMPLEMENT);
    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==, SSRAM_PATTERN);
    g_assert_cmphex(qtest_readl(qts, SSRAM_LINUX_PAIR), ==, SSRAM_COMPLEMENT);

    /*
     * The window runs to its end and does not wrap onto itself, which is the
     * same property as the halves being distinct asserted at the other end of
     * the region: a model whose storage stopped short of the top would either
     * fault here or answer with the first word.
     */
    qtest_writel(qts, SSRAM_LAST, 0x1234);
    g_assert_cmphex(qtest_readl(qts, SSRAM_LAST), ==, 0x1234);
    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==, SSRAM_PATTERN);

    qtest_quit(qts);
}

static void test_the_buffer_ram_is_seventeen_bits_wide(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * Sixteen bits of data and the parity bit stored beside them, and nothing
     * above. No boot can see this: every driver writes values that fit in the
     * data field, so a model storing all thirty-two bits produces an identical
     * transcript. The parity bit is asserted as storage rather than as
     * something computed, because this chip moves no frames here and a
     * diagnostic write is how software places a parity bit at all.
     */
    qtest_writel(qts, SSRAM_FIRST, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==, 0x0001ffff);

    qtest_writel(qts, SSRAM_FIRST, 0x00010000);
    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==, 0x00010000);

    qtest_quit(qts);
}

/*
 * The byte bus's first device select and the real-time clock behind it. The
 * window is byte-wide, unlike every other block on this chip, and the clock is
 * its top eight bytes.
 */
#define BYTEBUS_DEV0    (IOC3_BASE + 0x80000)
#define RTC_CONTROL     (BYTEBUS_DEV0 + 0x7ff8)
#define RTC_SECONDS     (BYTEBUS_DEV0 + 0x7ff9)
#define RTC_MINUTES     (BYTEBUS_DEV0 + 0x7ffa)
#define RTC_HOURS       (BYTEBUS_DEV0 + 0x7ffb)
#define RTC_WEEKDAY     (BYTEBUS_DEV0 + 0x7ffc)
#define RTC_DAY         (BYTEBUS_DEV0 + 0x7ffd)
#define RTC_MONTH       (BYTEBUS_DEV0 + 0x7ffe)
#define RTC_YEAR        (BYTEBUS_DEV0 + 0x7fff)

#define RTC_CSR_READ    0x40
#define RTC_CSR_WRITE   0x80

/*
 * A fixed time to start the clock at, so the fields can be asserted exactly
 * rather than for plausibility. The seconds are deliberately mid-minute: a run
 * takes a moment, and a base on a minute boundary would let the minute roll
 * underneath the test.
 */
#define RTC_ARGS \
    "-M origin200,io-bridge=on -rtc base=2026-08-11T12:30:05,clock=vm"

static void test_the_clock_reads_the_time_it_was_given(const void *data)
{
    QTestState *qts = qtest_init(RTC_ARGS);

    sn0_ioc3_attach(qts);

    /*
     * The read sequence all three drivers perform: freeze, read the fields,
     * release. Asserting the values exactly is the point -- and the year is
     * the assertion that matters most, because it is the one no boot can
     * check. The clock holds two digits counted from 1970, so 2026 is 0x56,
     * and a model presenting the calendar year's last two digits would answer
     * 0x26 here and have a guest report 1996 with nothing else looking wrong.
     */
    qtest_writeb(qts, RTC_CONTROL, RTC_CSR_READ);

    g_assert_cmphex(qtest_readb(qts, RTC_SECONDS), ==, 0x05);
    g_assert_cmphex(qtest_readb(qts, RTC_MINUTES), ==, 0x30);
    g_assert_cmphex(qtest_readb(qts, RTC_HOURS), ==, 0x12);
    g_assert_cmphex(qtest_readb(qts, RTC_DAY), ==, 0x11);
    g_assert_cmphex(qtest_readb(qts, RTC_MONTH), ==, 0x08);
    g_assert_cmphex(qtest_readb(qts, RTC_YEAR), ==, 0x56);

    /*
     * The weekday reads zero, which is not a gap. The register is a counter
     * software sets, and mainline Linux's driver records that the part leaves
     * it alone until it has been set to something non-zero -- so zero on a
     * machine nobody has set the day on is what the part reports. It is
     * storage here, asserted as written in the test that writes it.
     */
    g_assert_cmphex(qtest_readb(qts, RTC_WEEKDAY), ==, 0x00);

    qtest_writeb(qts, RTC_CONTROL, 0);

    qtest_quit(qts);
}

static void test_the_clock_freeze_holds_an_instant(const void *data)
{
    QTestState *qts = qtest_init(RTC_ARGS);

    sn0_ioc3_attach(qts);

    /*
     * The freeze has to be tested against a clock that is actually moving, and
     * the virtual clock only moves when a test moves it. An earlier version of
     * this file asserted that three reads in a row agreed, which passed with
     * the freeze removed entirely -- nothing was advancing between the reads,
     * so the assertion could not fail. Stepping the clock is what turns it
     * into a test.
     */
    qtest_writeb(qts, RTC_CONTROL, RTC_CSR_READ);
    g_assert_cmphex(qtest_readb(qts, RTC_SECONDS), ==, 0x05);

    /*
     * Step across a minute boundary, so the fields above the seconds have to
     * hold as well. Checking the seconds alone would pass on a freeze that
     * held one field and let the rest run.
     */
    qtest_clock_step(qts, 62 * 1000000000LL);
    g_assert_cmphex(qtest_readb(qts, RTC_SECONDS), ==, 0x05);
    g_assert_cmphex(qtest_readb(qts, RTC_MINUTES), ==, 0x30);
    g_assert_cmphex(qtest_readb(qts, RTC_HOURS), ==, 0x12);
    g_assert_cmphex(qtest_readb(qts, RTC_DAY), ==, 0x11);
    g_assert_cmphex(qtest_readb(qts, RTC_MONTH), ==, 0x08);
    g_assert_cmphex(qtest_readb(qts, RTC_YEAR), ==, 0x56);

    /* Released, the clock is where the stepped time put it, not where it was. */
    qtest_writeb(qts, RTC_CONTROL, 0);
    g_assert_cmphex(qtest_readb(qts, RTC_SECONDS), ==, 0x07);
    g_assert_cmphex(qtest_readb(qts, RTC_MINUTES), ==, 0x31);

    qtest_quit(qts);
}

static void test_the_clock_year_wraps_below_its_epoch(const void *data)
{
    /*
     * A clock started before its own epoch. The year cell holds two digits
     * counted from 1970, so 1969 is minus one, and C's remainder keeps the
     * sign of its left operand -- a plain modulo hands the binary-to-BCD
     * conversion a negative number and produces a byte that is not BCD at all.
     *
     * There is no right answer for a date the register cannot express; what
     * there is, is a wrong one, and this is the assertion that catches it. The
     * wrap puts 1969 at 99, which is where two digits counting down from 1970
     * land, and is what the same arithmetic does at the top end in 2070.
     *
     * It has its own machine because the base is the whole point: every other
     * clock test here starts in 2026, where the defect is invisible.
     */
    QTestState *qts = qtest_init("-M origin200,io-bridge=on "
                                 "-rtc base=1969-12-31T23:59:00,clock=vm");

    sn0_ioc3_attach(qts);

    qtest_writeb(qts, RTC_CONTROL, RTC_CSR_READ);
    g_assert_cmphex(qtest_readb(qts, RTC_YEAR), ==, 0x99);
    g_assert_cmphex(qtest_readb(qts, RTC_MONTH), ==, 0x12);
    g_assert_cmphex(qtest_readb(qts, RTC_DAY), ==, 0x31);
    qtest_writeb(qts, RTC_CONTROL, 0);

    qtest_quit(qts);
}

static void test_the_clock_control_register_reads_back(const void *data)
{
    QTestState *qts = qtest_init(RTC_ARGS);

    sn0_ioc3_attach(qts);

    /*
     * A driver reads this register, sets a bit in what it read, writes it
     * back, and later writes back the value it saved. So it has to read as
     * what was written -- a register that answered zero would have every
     * driver clear the other bit by accident on its way out.
     */
    qtest_writeb(qts, RTC_CONTROL, RTC_CSR_READ);
    g_assert_cmphex(qtest_readb(qts, RTC_CONTROL), ==, RTC_CSR_READ);

    qtest_writeb(qts, RTC_CONTROL, RTC_CSR_READ | RTC_CSR_WRITE);
    g_assert_cmphex(qtest_readb(qts, RTC_CONTROL), ==,
                    RTC_CSR_READ | RTC_CSR_WRITE);

    qtest_writeb(qts, RTC_CONTROL, 0);
    g_assert_cmphex(qtest_readb(qts, RTC_CONTROL), ==, 0);

    qtest_quit(qts);
}

static void test_the_clock_takes_a_time_a_guest_sets(const void *data)
{
    QTestState *qts = qtest_init(RTC_ARGS);

    sn0_ioc3_attach(qts);

    /*
     * The write sequence: arm, write the fields, release. The year written is
     * 0x40 -- 1970 plus forty -- so reading back 0x40 rather than 0x10 is what
     * says the epoch runs the same way in both directions. A model that
     * applied the epoch on read alone would pass the read test above and fail
     * here.
     */
    /*
     * Move the virtual clock well away from the host's before setting the
     * time. The two are the same clock by default and this test asks for the
     * virtual one, so any arithmetic that mixes them shows up here as a
     * read-back off by however far apart they are -- which is what made this
     * test fail intermittently by one second before the model was fixed, on
     * runs where real time happened to cross a second boundary while the
     * virtual clock stood still. Stepping first turns that coincidence into a
     * certainty: a model differencing against the wrong clock now fails every
     * time rather than one run in fifty.
     */
    qtest_clock_step(qts, 5 * 1000000000LL);

    qtest_writeb(qts, RTC_CONTROL, RTC_CSR_WRITE);
    qtest_writeb(qts, RTC_SECONDS, 0x45);
    qtest_writeb(qts, RTC_MINUTES, 0x21);
    qtest_writeb(qts, RTC_HOURS, 0x09);
    qtest_writeb(qts, RTC_WEEKDAY, 0x05);
    qtest_writeb(qts, RTC_DAY, 0x07);
    qtest_writeb(qts, RTC_MONTH, 0x03);
    qtest_writeb(qts, RTC_YEAR, 0x40);
    qtest_writeb(qts, RTC_CONTROL, 0);

    qtest_writeb(qts, RTC_CONTROL, RTC_CSR_READ);
    /*
     * Every field the guest wrote, including the two it would be easiest to
     * drop: the seconds, which a staging bug can zero while the rest of the
     * date survives, and the weekday, which nothing on any of our paths reads
     * and which was silently discarded until a review asked what happened to
     * it.
     */
    g_assert_cmphex(qtest_readb(qts, RTC_SECONDS), ==, 0x45);
    g_assert_cmphex(qtest_readb(qts, RTC_WEEKDAY), ==, 0x05);
    g_assert_cmphex(qtest_readb(qts, RTC_MINUTES), ==, 0x21);
    g_assert_cmphex(qtest_readb(qts, RTC_HOURS), ==, 0x09);
    g_assert_cmphex(qtest_readb(qts, RTC_DAY), ==, 0x07);
    g_assert_cmphex(qtest_readb(qts, RTC_MONTH), ==, 0x03);
    g_assert_cmphex(qtest_readb(qts, RTC_YEAR), ==, 0x40);
    qtest_writeb(qts, RTC_CONTROL, 0);

    qtest_quit(qts);
}

static void test_the_clock_storage_holds_a_write(const void *data)
{
    QTestState *qts = qtest_init(RTC_ARGS);

    sn0_ioc3_attach(qts);

    /*
     * The cells below the clock are real storage, and that is load-bearing
     * rather than incidental: SGI's own driver decides which of two clock
     * parts is fitted by writing 0xff to the byte at offset 0x6 with the byte
     * at 0xB set to zero, then reading 0x6 back. A model that answered those
     * as zero, or answered only the clock block, would be identified as a
     * Dallas part and driven through a register set this device does not have.
     */
    qtest_writeb(qts, BYTEBUS_DEV0 + 0x0b, 0x00);
    qtest_writeb(qts, BYTEBUS_DEV0 + 0x06, 0xff);
    qtest_writeb(qts, BYTEBUS_DEV0 + 0x0b, 0x80);
    g_assert_cmphex(qtest_readb(qts, BYTEBUS_DEV0 + 0x06), ==, 0xff);

    /* The ends of the storage, so a window that stops short is caught here. */
    qtest_writeb(qts, BYTEBUS_DEV0 + 0x0000, 0x5a);
    qtest_writeb(qts, BYTEBUS_DEV0 + 0x7ff7, 0xa5);
    g_assert_cmphex(qtest_readb(qts, BYTEBUS_DEV0 + 0x0000), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, BYTEBUS_DEV0 + 0x7ff7), ==, 0xa5);

    /*
     * Consecutive cells are distinct, and the storage does not alias the clock
     * above it: the byte below the control register keeps its own value while
     * the control register holds another.
     */
    qtest_writeb(qts, RTC_CONTROL, RTC_CSR_READ);
    g_assert_cmphex(qtest_readb(qts, BYTEBUS_DEV0 + 0x7ff7), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, RTC_CONTROL), ==, RTC_CSR_READ);
    qtest_writeb(qts, RTC_CONTROL, 0);

    qtest_quit(qts);
}

/*
 * The management interface, and the transceiver on the far end of it. A guest
 * reaches the transceiver only through these two registers: it writes an
 * address and a register number here and reads the answer back.
 */
#define MII_CONTROL     (IOC3_BASE + 0x148)
#define MII_DATA_R      (IOC3_BASE + 0x14c)
#define MII_DATA_W      (IOC3_BASE + 0x150)
#define MII_READ        (1u << 10)

#define PHY_ADDRESS     31

static uint16_t phy_read(QTestState *qts, unsigned int phy, unsigned int reg)
{
    qtest_writel(qts, MII_CONTROL, MII_READ | (phy << 5) | reg);
    return qtest_readl(qts, MII_DATA_R) & 0xffff;
}

static void phy_write(QTestState *qts, unsigned int phy, unsigned int reg,
                      uint16_t val)
{
    qtest_writel(qts, MII_DATA_W, val);
    qtest_writel(qts, MII_CONTROL, (phy << 5) | reg);
}

static void test_the_transceiver_answers_at_one_address(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    unsigned int phy, answered = 0;

    sn0_ioc3_attach(qts);

    /*
     * Every address, not just the right one. Both drivers walk all thirty-two
     * and stop at the first that answers, so a model answering everywhere is
     * found at address zero and the guest prints an address no real machine
     * prints. A boot cannot catch that -- it stops at the first answer and
     * carries on happily -- so the count is the assertion.
     */
    for (phy = 0; phy < 32; phy++) {
        if (phy_read(qts, phy, 2) != 0xffff) {
            g_assert_cmpuint(phy, ==, PHY_ADDRESS);
            answered++;
        }
    }

    g_assert_cmpuint(answered, ==, 1);

    qtest_quit(qts);
}

static void test_the_transceiver_identity(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * The exact words, not merely something plausible. An identity that only
     * passes the cruder of the two scans still fails the table match that
     * decides which driver binds, and the failure shows up as a console line
     * naming the wrong part -- or naming none.
     */
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 2), ==, 0x2000);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 3), ==, 0x5c01);

    qtest_quit(qts);
}

static void test_the_transceiver_reset_is_self_clearing(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * A driver writes reset and then polls for the bit to clear. Left set, it
     * holds the attach in a loop until the timeout expires.
     */
    phy_write(qts, PHY_ADDRESS, 0, 0x8000);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 0) & 0x8000, ==, 0);

    qtest_quit(qts);
}

static void test_the_transceiver_reports_a_usable_link(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    uint16_t status, advertise, partner;

    sn0_ioc3_attach(qts);

    /*
     * The driver's whole route to a media type, in the order it takes it. Each
     * step can be satisfied on its own by a model that still leaves the guest
     * reporting no media at all, which is why they are asserted together
     * rather than one standing for the rest.
     */
    phy_write(qts, PHY_ADDRESS, 0, 0x1000);   /* enable autonegotiation */

    /* Read twice, as the driver does, and assert the union has a link. */
    status = phy_read(qts, PHY_ADDRESS, 1) | phy_read(qts, PHY_ADDRESS, 1);
    g_assert_cmphex(status & 0x0004, ==, 0x0004);   /* link */
    g_assert_cmphex(status & 0x0020, ==, 0x0020);   /* autoneg complete */

    /* The far end can autonegotiate, which is the route the driver trusts. */
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 6) & 0x0001, ==, 0x0001);

    /*
     * And the two advertisements have a full-duplex hundred-megabit link in
     * common. The driver intersects them and reads the media out of the
     * result, so an empty intersection is a link with no usable media on it.
     */
    advertise = phy_read(qts, PHY_ADDRESS, 4);
    partner = phy_read(qts, PHY_ADDRESS, 5);
    g_assert_cmphex(advertise & partner & 0x0100, ==, 0x0100);

    qtest_quit(qts);
}

static void test_the_transceiver_configuration_register(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * Read-modify-written once on a media change and never read back for a
     * decision, so storage is the whole contract -- and the only place it is
     * checked at all, since no boot depends on it.
     */
    phy_write(qts, PHY_ADDRESS, 0x17, 0x0526);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 0x17), ==, 0x0526);

    /* Its low bits report the transceiver's own address. */
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 0x19) & 0x1f, ==, PHY_ADDRESS);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/ioc3/phy-answers-at-one-address", NULL,
                        test_the_transceiver_answers_at_one_address);
    qtest_add_data_func("sn0/ioc3/phy-identity", NULL,
                        test_the_transceiver_identity);
    qtest_add_data_func("sn0/ioc3/phy-reset-self-clearing", NULL,
                        test_the_transceiver_reset_is_self_clearing);
    qtest_add_data_func("sn0/ioc3/phy-reports-a-usable-link", NULL,
                        test_the_transceiver_reports_a_usable_link);
    qtest_add_data_func("sn0/ioc3/phy-configuration-register", NULL,
                        test_the_transceiver_configuration_register);

    qtest_add_data_func("sn0/ioc3/ps2-ports-read-empty", NULL,
                        test_the_ps2_ports_read_empty);
    qtest_add_data_func("sn0/ioc3/serial-dma-indices-by-owner", NULL,
                        test_the_serial_dma_indices_by_owner);
    qtest_add_data_func("sn0/ioc3/serial-dma-ports-are-distinct", NULL,
                        test_the_serial_dma_ports_are_distinct);
    qtest_add_data_func("sn0/ioc3/ethernet-interrupt-acknowledges", NULL,
                        test_the_ethernet_interrupt_register_acknowledges);
    qtest_add_data_func("sn0/ioc3/ethernet-read-only-registers", NULL,
                        test_the_ethernet_read_only_registers);
    qtest_add_data_func("sn0/ioc3/media-interface-never-busy", NULL,
                        test_the_media_interface_never_reads_busy);
    qtest_add_data_func("sn0/ioc3/buffer-ram-halves-are-distinct", NULL,
                        test_the_buffer_ram_halves_are_distinct);
    qtest_add_data_func("sn0/ioc3/buffer-ram-is-seventeen-bits-wide", NULL,
                        test_the_buffer_ram_is_seventeen_bits_wide);
    qtest_add_data_func("sn0/ioc3/clock-reads-the-time-it-was-given", NULL,
                        test_the_clock_reads_the_time_it_was_given);
    qtest_add_data_func("sn0/ioc3/clock-freeze-holds-an-instant", NULL,
                        test_the_clock_freeze_holds_an_instant);
    qtest_add_data_func("sn0/ioc3/clock-year-wraps-below-its-epoch", NULL,
                        test_the_clock_year_wraps_below_its_epoch);
    qtest_add_data_func("sn0/ioc3/clock-control-reads-back", NULL,
                        test_the_clock_control_register_reads_back);
    qtest_add_data_func("sn0/ioc3/clock-takes-a-time-a-guest-sets", NULL,
                        test_the_clock_takes_a_time_a_guest_sets);
    qtest_add_data_func("sn0/ioc3/clock-storage-holds-a-write", NULL,
                        test_the_clock_storage_holds_a_write);
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
    qtest_add_data_func("sn0/ioc3/second-interrupt-reaches-bridge-pin-3",
                        NULL,
                        test_the_second_interrupt_reaches_bridge_pin_3);
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
