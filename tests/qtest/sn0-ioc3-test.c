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
#define PCI_COMMAND_MASTER 0x0004
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
/*
 * The ethernet block's interrupt has no direct wire of its own -- unlike the
 * SuperIO one above, it travels this chip's ordinary PCI interrupt pin (PIN_A,
 * config space byte PCI_INTERRUPT_PIN) through the Bridge's default INTx
 * routing. sgi_bridge_map_irq() computes that routing as the slot number for
 * an even (A/C) pin, and the controller sits at PCI slot 2, so this is the
 * pin sgi_bridge_map_irq(dev, 0) gives -- not an independent constant, a
 * restatement of what that routing does for this one slot and pin.
 */
#define BRIDGE_PIN_ETH (1u << 2)

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

static void test_a_one_wire_reset_finds_the_ethernet_address_part(
    const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * Task #13: the bus now carries a part, the ethernet-address memory
     * docs/datasheets/ioc3.md's "The ethernet-address part's read
     * protocol" section specifies. A reset finds it: the wire is
     * open-drain and idles high, and the part answers a reset by pulling
     * it low, which a guest reads as presence. Before this task the same
     * slot reported a high level, which is what "no part" means at this
     * register -- this test now asserts the opposite for the same reason
     * the Bridge's identical test does: a guest that gets this wrong
     * either talks to a bus it should treat as empty or gives up on one
     * that has a part on it.
     */
    qtest_writel(qts, MICROLAN_CONTROL, MICROLAN_RESET_SLOT);
    g_assert_cmphex(qtest_readl(qts, MICROLAN_CONTROL) & MICROLAN_LEVEL, ==,
                    0);

    qtest_quit(qts);
}

static void test_one_wire_slot_preserves_the_timing_written(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    /*
     * A pulse of 5 microseconds -- comfortably below both the write-zero
     * and reset thresholds the datasheet section named above sets out, so
     * this lands as an ordinary short slot rather than colliding with the
     * ethernet-address part's own reset-presence or write-zero handling.
     * Bits 0 and 1 are set in the write to prove hardware overwrites them
     * rather than passing them through; the remaining bits are otherwise
     * arbitrary, to prove the timing field is stored rather than
     * interpreted beyond its pulse-duration meaning.
     */
    uint32_t written = (5u << 10) | (0x9eu << 2) | 0x3u;
    uint32_t timing = written & ~(MICROLAN_LEVEL | MICROLAN_DONE);

    sn0_ioc3_attach(qts);

    /*
     * Bits 0 and 1 are hardware's to set, not a mask the guest's own value
     * happens to already carry — this writes a pattern with both of them
     * set in the guest's own value and checks the readback still shows
     * exactly what hardware decides, not a blind copy of the write. With
     * no reset since the bus attached, an ordinary short slot this early
     * finds the part in its idle phase, which does not drive the wire, so
     * the level reads high exactly as it would if nothing were there --
     * this test is about the timing field, not about what the part
     * answers, which the tests below cover.
     */
    qtest_writel(qts, MICROLAN_CONTROL, written);
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
#define SDMA_A_SHADOW     (IOC3_BASE + 0x0d0)
#define SDMA_B_SHADOW     (IOC3_BASE + 0x0ec)

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

static void test_the_serial_dma_shadow_is_read_only(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /*
     * The 16550 shadow is hardware's own mirror of live UART lines, not
     * software's to set. OpenBSD writes zero to both ports' shadows on
     * every boot while putting the ports into pass-through mode, and the
     * real chip discards that the same way this model does: the write
     * leaves no mark.
     */
    qtest_writel(qts, SDMA_A_SHADOW, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SDMA_A_SHADOW), ==, 0);
    qtest_writel(qts, SDMA_B_SHADOW, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SDMA_B_SHADOW), ==, 0);

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

/* The MAC control register and the two DMA rings it gates. */
#define MAC_CONTROL       (IOC3_BASE + 0x0f0)
#define RX_RING_HI        (IOC3_BASE + 0x100)
#define RX_RING_LO        (IOC3_BASE + 0x104)
#define RX_CONSUME        (IOC3_BASE + 0x10c)
#define RX_PRODUCE        (IOC3_BASE + 0x110)
#define TX_RING_HI        (IOC3_BASE + 0x128)
#define TX_RING_LO        (IOC3_BASE + 0x12c)
#define TX_CONSUME        (IOC3_BASE + 0x130)
#define TX_PRODUCE        (IOC3_BASE + 0x134)

#define MCR_TX_DMA_EN     (1u << 13)
#define MCR_TX_EN         (1u << 14)
#define MCR_RX_DMA_EN     (1u << 15)
#define MCR_RX_EN         (1u << 16)

/* The receive buffer offset field holds the distance from the buffer's
 * start to where the frame begins, in sixteen-bit halfwords -- see
 * ioc3.md's ethernet register table. 33 halfwords (66 bytes) is what
 * OpenBSD's own driver programs: eight bytes for the two header words
 * this engine writes, plus its own alignment padding.
 */
#define MCR_RXOFF_SHIFT   3
#define MCR_RXOFF(halfwords) ((uint32_t)(halfwords) << MCR_RXOFF_SHIFT)

#define ENET_INT_TX_EMPTY    (1u << 16)
#define ENET_INT_TX_COMPLETE (1u << 22)
#define ENET_INT_RX_TIMER     (1u << 0)
#define ENET_INT_RX_THRESHOLD (1u << 1)

/*
 * The good-packet bit in a receive buffer's error word. Linux gates every
 * delivered frame on it and drops the frame without it; OpenBSD reads the
 * error bits beside it instead and never looks at this one. So it is a field
 * only one of the two guests can miss, which is exactly the kind that goes
 * unasserted -- the guest in front of you keeps working.
 */
#define ENET_RXE_GOODPKT      (1u << 30)

/*
 * The receive control register's watermark field and the produce index's
 * arm bit, the two things that decide which source a delivered frame
 * raises. RX_CONTROL is the register OpenBSD calls RCSR. Linux calls it
 * ercsr. Its low nine bits are the watermark.
 */
#define RX_CONTROL        (IOC3_BASE + 0x0fc)
#define RX_TIMER          (IOC3_BASE + 0x114)
#define RPIR_ARM          (1u << 31)

#define TX_CONSUME_IDLE   (1u << 31)

/* Transmit descriptor command-word bits and buffer-count-word fields. */
#define TXD_INT_DONE      (1u << 12)
#define TXD_DATA_VALID    (1u << 16)
#define TXD_BUF1_VALID    (1u << 17)
#define TXD_BUF2_VALID    (1u << 18)
#define TXD_BUF1_LEN_SHIFT 8
#define TXD_BUF2_LEN_SHIFT 20

/*
 * Where a descriptor's inline data area starts and how big it is, taken from
 * the two drivers rather than from the model, which is the only thing that
 * makes these assertions able to fail. OpenBSD's struct iec_txdesc is a
 * command word, a length word, two eight-byte pointers, then the buffer --
 * its own IEC_TXD_BUFOFFSET computes 24 and IEC_TXD_BUFSIZE computes 104.
 * Linux's struct ioc3_etxd is the same four fields followed by data[104].
 * Two unrelated drivers, both numbers, so this is the descriptor's layout
 * and not one driver's habit.
 *
 * This file carried 16 here for as long as the model did. Both were wrong
 * and they were wrong together, so every transmit assertion passed while
 * the frames leaving the engine had eight bytes of the second pointer in
 * front of them.
 */
#define TXD_DATA_OFFSET   24
#define TXD_DATA_SIZE     104

/* The inline length field is seven bits, so a guest can name more than the
 * area holds. */
#define TXD_DATA_LEN_MAX  127

/* A region of guest RAM well clear of the firmware's own configuration
 * graph (at 0x4000), used as scratch space for descriptors and buffers.
 */
#define RAM_SCRATCH       0x00100000

/*
 * Attach the controller with a live network backend: a connected socket
 * pair, one end handed to the guest as -netdev, the other kept by the test.
 * -nic rather than -netdev is what makes qemu_configure_nic_device's legacy
 * lookup find it, the same mechanism a real command line uses to hand this
 * on-board controller its backend.
 *
 * The plain register tests never need the bus-master bit -- a guest CPU
 * reading or writing a register does not go through it, only a device's own
 * DMA does, and sn0_ioc3_attach was written before this file had any. Real
 * firmware sets both bits together (see the datasheet's console bring-up
 * section), so this sets the one sn0_ioc3_attach leaves out.
 */
static QTestState *sn0_ioc3_attach_with_netdev(int host_fd)
{
    QTestState *qts;
    char *args = g_strdup_printf("-M origin200,io-bridge=on "
                                 "-nic socket,fd=%d,id=n0", host_fd);

    qts = qtest_init(args);
    g_free(args);
    sn0_ioc3_attach(qts);
    qtest_writel(qts, CFG0(PCI_IOC3_DEVICE) + PCI_COMMAND,
                PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    return qts;
}

/*
 * The ethernet-address one-wire part: task #13, docs/datasheets/ioc3.md's
 * "The ethernet-address part's read protocol". This bus is fixed to carry
 * one part, so a plain -nic attach is enough -- these tests need a known
 * MAC address to check the part against, which is what the explicit mac=
 * gives them; the socket backend is otherwise unused, the same pattern
 * sn0_ioc3_attach_with_netdev above uses for the ethernet ring tests.
 */
static QTestState *sn0_ioc3_attach_with_mac(int host_fd, const char *mac)
{
    QTestState *qts;
    char *args = g_strdup_printf("-M origin200,io-bridge=on "
                                 "-nic socket,fd=%d,id=n0,mac=%s",
                                 host_fd, mac);

    qts = qtest_init(args);
    g_free(args);
    sn0_ioc3_attach(qts);
    return qts;
}

static const uint8_t onewire_test_mac[6] = { 0x52, 0x54, 0x00, 0x11, 0x22, 0x33 };
#define ONEWIRE_TEST_MAC_STR "52:54:00:11:22:33"

/* One bus slot, returning the level sampled -- the same helper shape
 * sn0-bridge-test.c's nic_slot() uses for the Bridge's identical engine.
 */
static bool onewire_slot(QTestState *qts, unsigned int pulse,
                         unsigned int sample)
{
    qtest_writel(qts, MICROLAN_CONTROL, (pulse << 10) | (sample << 2));
    return qtest_readl(qts, MICROLAN_CONTROL) & MICROLAN_LEVEL;
}

/* The widths OpenBSD's and Linux's one-wire masters both use, per the
 * datasheet's "GPIO and MicroLAN" section: a reset finds presence, a read
 * and a written one share a slot, and a written zero is a longer low pulse.
 */
#define ONEWIRE_RESET(q)      onewire_slot((q), 520, 65)
#define ONEWIRE_READ(q)       onewire_slot((q), 6, 13)
#define ONEWIRE_WRITE_ONE(q)  onewire_slot((q), 6, 13)
#define ONEWIRE_WRITE_ZERO(q) onewire_slot((q), 80, 30)

static void onewire_write_byte(QTestState *qts, uint8_t byte)
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        if ((byte >> i) & 1) {
            ONEWIRE_WRITE_ONE(qts);
        } else {
            ONEWIRE_WRITE_ZERO(qts);
        }
    }
}

static uint8_t onewire_read_byte(QTestState *qts)
{
    unsigned int i;
    uint8_t byte = 0;

    for (i = 0; i < 8; i++) {
        if (ONEWIRE_READ(qts)) {
            byte |= 1u << i;
        }
    }
    return byte;
}

/* The standard Dallas/Maxim one-wire eight-bit CRC: polynomial 0x8c
 * reflected, seeded at zero, sent as computed -- see the datasheet section
 * named above. A different algorithm and a different transmission
 * convention from the Bridge test file's sixteen-bit nic_crc(), which is
 * why this is its own function rather than a shared one.
 */
static uint8_t onewire_crc8(uint8_t crc, const uint8_t *data, unsigned int len)
{
    unsigned int i, bit;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8c : crc >> 1;
        }
    }
    return crc;
}

/* The standard one-wire sixteen-bit CRC, same algorithm and convention as
 * sn0-bridge-test.c's nic_crc() -- reused here rather than shared across
 * files, since qtest files in this tree do not share helper code.
 */
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

/*
 * Drive a whole search and read the part's identity out of it -- the same
 * exchange OpenBSD's onewire(4) and the Linux w1 core both perform before
 * they will address the part at all. The family byte and the serial
 * number's required top twelve bits are checked; the low bits of the
 * serial number are this model's own choice and are not asserted.
 */
static void test_a_search_finds_the_ethernet_address_part(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint64_t rom = 0;
    unsigned int i;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = sn0_ioc3_attach_with_mac(sv[1], ONEWIRE_TEST_MAC_STR);

    g_assert_false(ONEWIRE_RESET(qts));    /* pulled low: the part is there */

    onewire_write_byte(qts, 0xf0);         /* Search ROM */
    for (i = 0; i < 64; i++) {
        bool bit = ONEWIRE_READ(qts);
        bool complement = ONEWIRE_READ(qts);

        g_assert_cmpint(complement, ==, !bit);
        if (bit) {
            rom |= 1ULL << i;
            ONEWIRE_WRITE_ONE(qts);
        } else {
            ONEWIRE_WRITE_ZERO(qts);
        }
    }

    g_assert_cmphex(rom & 0xff, ==, 0x91);              /* family */
    g_assert_cmphex((rom >> 44) & 0xfff, ==, 0x5e7);    /* serial prefix */

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A Match ROM that names a different part must leave this one unselected --
 * OpenBSD's owmac always addresses the part this way, never by Skip ROM
 * (unlike Linux, which only ever sees one device and takes the Skip ROM
 * path), so a model that only handles Skip ROM would work for one guest and
 * silently never answer the other. The unselected part's reply to a
 * function command that follows is the bus's own idle-high byte, not a
 * real CRC-8 answer -- 0xff, distinguishable from a genuine leading CRC-8
 * only by knowing what a genuine one is, which the next test establishes.
 */
static void test_a_wrong_match_rom_leaves_the_part_unselected(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint64_t wrong_rom = 0x0000005e70000091ULL; /* right family, wrong rest */
    uint8_t leading;
    unsigned int i;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = sn0_ioc3_attach_with_mac(sv[1], ONEWIRE_TEST_MAC_STR);

    ONEWIRE_RESET(qts);
    onewire_write_byte(qts, 0x55);   /* Match ROM */
    for (i = 0; i < 8; i++) {
        onewire_write_byte(qts, (wrong_rom >> (8 * i)) & 0xff);
    }
    onewire_write_byte(qts, 0xf0);   /* Read Memory, if this landed */
    onewire_write_byte(qts, 0x00);
    onewire_write_byte(qts, 0x00);
    leading = onewire_read_byte(qts);
    g_assert_cmphex(leading, ==, 0xff);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Read the page holding the station address, the way each guest actually
 * does it: OpenBSD's owmac_read_page() selects with a Match ROM and reads
 * memory with the plain Read Memory command (0xf0); Linux's
 * w1_ds2502_read_page() selects with a Skip ROM (its w1 core takes that
 * path whenever only one device is registered) and reads memory with the
 * Read Memory-with-CRC command (0xc3). Both must answer identically.
 */
static void read_ethernet_address_page(QTestState *qts, bool use_match_rom,
                                       uint8_t cmd, uint8_t *leading_crc,
                                       uint8_t page[32], uint8_t *trailing_crc)
{
    unsigned int i;

    ONEWIRE_RESET(qts);
    if (use_match_rom) {
        uint64_t rom = 0;

        onewire_write_byte(qts, 0xf0);     /* Search ROM, to learn the id */
        for (i = 0; i < 64; i++) {
            bool bit = ONEWIRE_READ(qts);

            ONEWIRE_READ(qts);              /* the complement, unused here */
            if (bit) {
                rom |= 1ULL << i;
                ONEWIRE_WRITE_ONE(qts);
            } else {
                ONEWIRE_WRITE_ZERO(qts);
            }
        }
        ONEWIRE_RESET(qts);
        onewire_write_byte(qts, 0x55);     /* Match ROM */
        for (i = 0; i < 8; i++) {
            onewire_write_byte(qts, (rom >> (8 * i)) & 0xff);
        }
    } else {
        onewire_write_byte(qts, 0xcc);     /* Skip ROM */
    }

    onewire_write_byte(qts, cmd);
    onewire_write_byte(qts, 0x00);         /* target address, low */
    onewire_write_byte(qts, 0x00);         /* target address, high */

    *leading_crc = onewire_read_byte(qts);
    for (i = 0; i < 32; i++) {
        page[i] = onewire_read_byte(qts);
    }
    *trailing_crc = onewire_read_byte(qts);
}

static void assert_ethernet_address_page(QTestState *qts, bool use_match_rom,
                                         uint8_t cmd)
{
    uint8_t leading, page[32], trailing;
    uint8_t echo[3] = { cmd, 0x00, 0x00 };
    uint8_t got_mac[6];
    unsigned int i;

    read_ethernet_address_page(qts, use_match_rom, cmd, &leading, page,
                               &trailing);

    g_assert_cmphex(leading, ==, onewire_crc8(0, echo, sizeof(echo)));
    g_assert_cmphex(page[0], ==, 0x0a);

    /* Bytes 5..10 hold the station address reversed: byte 10 is the first
     * octet, byte 5 the last -- see the datasheet section named above.
     */
    for (i = 0; i < 6; i++) {
        got_mac[i] = page[10 - i];
    }
    g_assert_cmpmem(got_mac, sizeof(got_mac), onewire_test_mac,
                    sizeof(onewire_test_mac));

    g_assert_cmphex(onewire_crc16(0, page, 13), ==, 0xb001);
    g_assert_cmphex(trailing, ==, onewire_crc8(0, page, 32));
}

static void test_reading_the_page_via_read_memory(const void *data)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = sn0_ioc3_attach_with_mac(sv[1], ONEWIRE_TEST_MAC_STR);

    /* OpenBSD's owmac: Match ROM, then the plain Read Memory command. */
    assert_ethernet_address_page(qts, true, 0xf0);

    qtest_quit(qts);
    close(sv[0]);
}

static void test_reading_the_page_via_read_memory_crc(const void *data)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = sn0_ioc3_attach_with_mac(sv[1], ONEWIRE_TEST_MAC_STR);

    /* Linux's w1_ds2502_read_page: Skip ROM, then Read Memory-with-CRC. */
    assert_ethernet_address_page(qts, false, 0xc3);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The redirection table owmac_read_redirect() reads before it trusts a
 * page: OpenBSD always asks for status offset 0x0001, and the part answers
 * with four bytes of 0xff, which is what tells owmem_redirect()'s loop to
 * stop immediately rather than fail -- see the datasheet section named
 * above for why the model does not track real redirection at all.
 */
static void test_read_status_returns_no_redirection(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t leading;
    uint8_t status[4];
    uint8_t echo[3] = { 0xaa, 0x01, 0x00 };
    unsigned int i;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = sn0_ioc3_attach_with_mac(sv[1], ONEWIRE_TEST_MAC_STR);

    ONEWIRE_RESET(qts);
    onewire_write_byte(qts, 0xcc);   /* one part on the bus */
    onewire_write_byte(qts, 0xaa);   /* Read Status */
    onewire_write_byte(qts, 0x01);   /* the offset owmac always asks for */
    onewire_write_byte(qts, 0x00);

    leading = onewire_read_byte(qts);
    g_assert_cmphex(leading, ==, onewire_crc8(0, echo, sizeof(echo)));

    for (i = 0; i < 4; i++) {
        status[i] = onewire_read_byte(qts);
    }
    g_assert_cmphex(status[0], ==, 0xff);
    g_assert_cmphex(status[1], ==, 0xff);
    g_assert_cmphex(status[2], ==, 0xff);
    g_assert_cmphex(status[3], ==, 0xff);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A bound on how long the test's end of the pair will block for a frame
 * that is late rather than never coming. qemu_send_packet on this backend
 * does not guarantee the write reaches the socket inside the same call
 * that triggered it, so a plain recv could block on a delivery that is
 * still in flight through the main loop rather than one that failed.
 */
static void net_socket_set_recv_timeout(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };

    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)),
                    ==, 0);
}

/* The framing net_socket_send uses: a four-byte network-order length ahead
 * of each packet, in both directions over the connected socket.
 */
static void net_socket_send_frame(int fd, const uint8_t *buf, size_t len)
{
    uint32_t hdr = htonl((uint32_t)len);

    g_assert_cmpint(send(fd, &hdr, sizeof(hdr), 0), ==, (int)sizeof(hdr));
    g_assert_cmpint(send(fd, buf, len, 0), ==, (int)len);
}

static size_t net_socket_recv_frame(int fd, uint8_t *buf, size_t buf_size)
{
    uint32_t hdr;
    size_t len;

    g_assert_cmpint(recv(fd, &hdr, sizeof(hdr), 0), ==, (int)sizeof(hdr));
    len = ntohl(hdr);
    g_assert_cmpint(len, <=, buf_size);
    g_assert_cmpint(recv(fd, buf, len, 0), ==, (int)len);
    return len;
}

static void test_the_ethernet_transmit_sends_inline_frame(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[14] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x08, 0x00 };
    uint8_t received[64];
    size_t got;
    uint32_t desc = RAM_SCRATCH;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN);

    /*
     * One descriptor: the command word says fourteen bytes, all of them
     * inline, and asks for a completion interrupt; the buffer-count word's
     * low seven bits repeat the inline length, which is what the engine
     * actually reads to know how much of the inline area to take.
     */
    qtest_writel(qts, desc + 0, sizeof(frame) | TXD_DATA_VALID | TXD_INT_DONE);
    qtest_writel(qts, desc + 4, sizeof(frame));
    qtest_memwrite(qts, desc + TXD_DATA_OFFSET, frame, sizeof(frame));

    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);

    /* The doorbell: one descriptor further than the ring base. */
    qtest_writel(qts, TX_PRODUCE, 128);

    got = net_socket_recv_frame(sv[0], received, sizeof(received));
    g_assert_cmpint(got, ==, sizeof(frame));
    g_assert_cmpmem(received, got, frame, sizeof(frame));

    g_assert_cmphex(qtest_readl(qts, TX_CONSUME), ==,
                    (128u | TX_CONSUME_IDLE));
    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) &
                    (ENET_INT_TX_EMPTY | ENET_INT_TX_COMPLETE), ==,
                    ENET_INT_TX_EMPTY | ENET_INT_TX_COMPLETE);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A guest may name more inline data than the descriptor's area holds: the
 * length field is seven bits and the area is 104 bytes. The engine has to
 * take the smaller of the two, and the frame that reaches the backend is
 * where that shows -- an engine that trusted the field would send twenty
 * three bytes of whatever follows its own copy of the descriptor.
 *
 * The payload is filled with the descriptor offset of each byte, so a frame
 * that starts in the wrong place fails on content and not only on length.
 */
static void test_the_ethernet_transmit_bounds_the_inline_area(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t area[TXD_DATA_SIZE];
    uint8_t received[256];
    size_t got;
    unsigned int i;
    uint32_t desc = RAM_SCRATCH;

    for (i = 0; i < sizeof(area); i++) {
        area[i] = (uint8_t)(TXD_DATA_OFFSET + i);
    }

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN);

    qtest_writel(qts, desc + 0, TXD_DATA_LEN_MAX | TXD_DATA_VALID);
    qtest_writel(qts, desc + 4, TXD_DATA_LEN_MAX);
    qtest_memwrite(qts, desc + TXD_DATA_OFFSET, area, sizeof(area));

    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);
    qtest_writel(qts, TX_PRODUCE, 128);

    got = net_socket_recv_frame(sv[0], received, sizeof(received));
    g_assert_cmpint(got, ==, TXD_DATA_SIZE);
    g_assert_cmpmem(received, got, area, sizeof(area));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A guest can hang the emulator with two register writes, and this is the
 * shape of it: the walk steps a whole descriptor at a time and stops when the
 * consume index equals the produce index, so a produce index the steps can
 * never land on is a loop with no way out. The indices are byte offsets given
 * as a descriptor number shifted left by seven, so nothing a driver writes has
 * the low bits set -- which is exactly why no test written against a driver's
 * behaviour would ever find this.
 *
 * The assertion that matters here is that the test finishes at all. A model
 * without the bound never returns from the doorbell write and this fails as a
 * timeout rather than a comparison, which is the only way a hang can fail.
 */
static void test_an_unaligned_produce_index_terminates(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint32_t desc = RAM_SCRATCH;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN);
    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);

    /* Both indices written raw, neither on a descriptor boundary. */
    qtest_writel(qts, TX_CONSUME, 0);
    qtest_writel(qts, TX_PRODUCE, 1);

    /*
     * One byte is below a descriptor, so there is nothing to send and the
     * engine comes to rest where it started, idle.
     */
    g_assert_cmphex(qtest_readl(qts, TX_CONSUME), ==, TX_CONSUME_IDLE);

    /* And it is still a working engine afterwards rather than a wedged one. */
    qtest_writel(qts, TX_CONSUME, 3);
    qtest_writel(qts, TX_PRODUCE, 128 + 5);
    g_assert_cmphex(qtest_readl(qts, TX_CONSUME), ==, (128u | TX_CONSUME_IDLE));

    qtest_quit(qts);
    close(sv[0]);
}

static void test_the_ethernet_transmit_gathers_two_buffers(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t buf1[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    uint8_t buf2[4] = { 0x0a, 0x0b, 0x0c, 0x0d };
    uint8_t expect[10];
    uint8_t received[64];
    size_t got;
    uint32_t desc = RAM_SCRATCH;
    uint32_t buf1_addr = RAM_SCRATCH + 0x1000;
    uint32_t buf2_addr = RAM_SCRATCH + 0x2000;

    memcpy(expect, buf1, sizeof(buf1));
    memcpy(expect + sizeof(buf1), buf2, sizeof(buf2));

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN);

    qtest_memwrite(qts, buf1_addr, buf1, sizeof(buf1));
    qtest_memwrite(qts, buf2_addr, buf2, sizeof(buf2));

    /*
     * No inline data this time -- both validity flags name an out-of-line
     * buffer instead, which is the path a frame too large for the inline
     * area alone takes. Buffer one is read before buffer two, matching the
     * order the two validity flags are given in.
     */
    qtest_writel(qts, desc + 0, sizeof(expect) | TXD_BUF1_VALID |
                TXD_BUF2_VALID);
    qtest_writel(qts, desc + 4, (sizeof(buf1) << TXD_BUF1_LEN_SHIFT) |
                (sizeof(buf2) << TXD_BUF2_LEN_SHIFT));
    qtest_writel(qts, desc + 8, 0);
    qtest_writel(qts, desc + 12, buf1_addr);
    qtest_writel(qts, desc + 16, 0);
    qtest_writel(qts, desc + 20, buf2_addr);

    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);
    qtest_writel(qts, TX_PRODUCE, 128);

    got = net_socket_recv_frame(sv[0], received, sizeof(received));
    g_assert_cmpint(got, ==, sizeof(expect));
    g_assert_cmpmem(received, got, expect, sizeof(expect));

    qtest_quit(qts);
    close(sv[0]);
}

static void test_the_ethernet_receive_delivers_a_frame(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[16] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x08, 0x06, 0xde, 0xad };
    uint8_t landed[sizeof(frame)];
    uint32_t ring = RAM_SCRATCH;
    uint32_t rxbuf = RAM_SCRATCH + 0x4000;
    uint32_t word0, word1;
    int attempt;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, MAC_CONTROL,
                MCR_RX_DMA_EN | MCR_RX_EN | MCR_RXOFF(33));
    qtest_writel(qts, RX_RING_HI, 0);
    qtest_writel(qts, RX_RING_LO, ring & 0xfffff000u);

    /* One buffer address at the ring's first entry. */
    qtest_writel(qts, ring + 0, 0);
    qtest_writel(qts, ring + 4, rxbuf);

    /* Supplying it is what makes the engine willing to receive at all. */
    qtest_writel(qts, RX_PRODUCE, 8);

    net_socket_send_frame(sv[0], frame, sizeof(frame));

    /*
     * Delivery happens on the main loop noticing the socket is readable,
     * which is not synchronous with the send above; poll a live register
     * with a generous bound rather than assume the first look already
     * shows it.
     */
    for (attempt = 0; attempt < 50; attempt++) {
        if (qtest_readl(qts, RX_CONSUME) != 0) {
            break;
        }
        g_usleep(10000);
    }
    g_assert_cmphex(qtest_readl(qts, RX_CONSUME), ==, 8);

    word0 = qtest_readl(qts, rxbuf + 0);
    word1 = qtest_readl(qts, rxbuf + 4);
    qtest_memread(qts, rxbuf + 66, landed, sizeof(landed));

    /*
     * Valid, and the eleven-bit byte count matches what was sent plus the
     * four-byte FCS a real MAC would have captured alongside it -- QEMU's
     * net layer never delivers one, but both guests' drivers read this
     * field as the wire length including it (OpenBSD's iec_rxintr checks
     * it against ETHER_MIN_LEN before ever subtracting ETHER_CRC_LEN), so
     * the model has to add the four phantom bytes back to match what real
     * hardware would report here.
     */
    g_assert_cmphex(word0 & 0x80000000u, ==, 0x80000000u);
    g_assert_cmphex((word0 >> 16) & 0x7ff, ==, sizeof(frame) + 4);

    /* The split length field reassembles to the same count. */
    g_assert_cmphex((((word1 >> 16) & 0x3ff) << 3) | ((word1 >> 12) & 0x7),
                    ==, sizeof(frame) + 4);

    /* An all-ones destination is reported as broadcast. */
    g_assert_cmphex(word1 & (1u << 27), ==, (1u << 27));

    /* And the frame is reported good, which is the bit Linux gates on. */
    g_assert_cmphex(word1 & ENET_RXE_GOODPKT, ==, ENET_RXE_GOODPKT);

    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));

    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) &
                    ENET_INT_RX_THRESHOLD, ==, ENET_INT_RX_THRESHOLD);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Which source a delivered frame raises, and what has to be true for each.
 * Three runs of one arrangement -- four buffers supplied, one frame sent --
 * differing only in the receive timer's arming and the watermark in the
 * receive control register, because those are the two things that decide
 * the answer and a test that varied anything else could not say so.
 *
 * The frame is delivered in all three. What changes is what the guest is
 * told about it, which is the whole of the contract here: Linux enables the
 * timer source and no other receive source at all, so an engine that
 * announces every frame on the threshold source leaves a Linux guest with
 * status & enable == 0 on every frame it receives.
 */
static void sn0_ioc3_rx_one_frame(QTestState *qts, int host_fd, uint32_t ring,
                                  uint32_t rxbuf, uint32_t produce,
                                  uint32_t threshold)
{
    uint8_t frame[16] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x08, 0x06, 0xde, 0xad };
    unsigned int i;
    int attempt;

    qtest_writel(qts, MAC_CONTROL,
                MCR_RX_DMA_EN | MCR_RX_EN | MCR_RXOFF(33));
    qtest_writel(qts, RX_RING_HI, 0);
    qtest_writel(qts, RX_RING_LO, ring & 0xfffff000u);
    qtest_writel(qts, RX_TIMER, 0);
    qtest_writel(qts, RX_CONTROL, threshold);

    /*
     * Four buffers, one page apart, so a frame has somewhere to land and
     * the supply is deep enough for a watermark to sit below it.
     */
    for (i = 0; i < 4; i++) {
        qtest_writel(qts, ring + i * 8, 0);
        qtest_writel(qts, ring + i * 8 + 4, rxbuf + i * 0x1000);
    }

    qtest_writel(qts, RX_PRODUCE, produce);

    net_socket_send_frame(host_fd, frame, sizeof(frame));

    for (attempt = 0; attempt < 50; attempt++) {
        if (qtest_readl(qts, RX_CONSUME) != 0) {
            break;
        }
        g_usleep(10000);
    }

    /*
     * Delivered, in every one of the three runs. An assertion about which
     * source was raised means nothing without this.
     */
    g_assert_cmphex(qtest_readl(qts, RX_CONSUME), ==, 8);
    g_assert_cmphex(qtest_readl(qts, rxbuf + 0) & 0x80000000u, ==, 0x80000000u);
}

/*
 * Armed, with the immediate reload both drivers write, and three buffers
 * still available against a watermark of none. The timer source is raised
 * and the threshold source is not.
 */
static void test_an_armed_receive_timer_raises_its_own_source(const void *data)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    sn0_ioc3_rx_one_frame(qts, sv[0], RAM_SCRATCH, RAM_SCRATCH + 0x4000,
                          (4 << 3) | RPIR_ARM, 0);

    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) &
                    (ENET_INT_RX_TIMER | ENET_INT_RX_THRESHOLD), ==,
                    ENET_INT_RX_TIMER);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The same frame with the timer never armed and the supply still above the
 * watermark. Nothing is raised. This is the run that fails on an engine
 * announcing every frame on the threshold source, and the one above is what
 * stops it being satisfied by an engine that raises nothing at all.
 */
static void test_a_frame_below_the_watermark_raises_nothing(const void *data)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    sn0_ioc3_rx_one_frame(qts, sv[0], RAM_SCRATCH, RAM_SCRATCH + 0x4000,
                          4 << 3, 0);

    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) &
                    (ENET_INT_RX_TIMER | ENET_INT_RX_THRESHOLD), ==, 0);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The same again with the watermark raised to where this frame's delivery
 * puts the supply -- four buffers, one consumed, three left, watermark
 * three. The threshold source is raised and the unarmed timer is not.
 */
static void test_reaching_the_watermark_raises_the_threshold(const void *data)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    sn0_ioc3_rx_one_frame(qts, sv[0], RAM_SCRATCH, RAM_SCRATCH + 0x4000,
                          4 << 3, 3);

    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) &
                    (ENET_INT_RX_TIMER | ENET_INT_RX_THRESHOLD), ==,
                    ENET_INT_RX_THRESHOLD);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Arming is a command and the index is storage. SGI's own driver writes the
 * index without the arm bit from its interrupt path, so an engine that took
 * the bit's absence as a disarm would deliver one frame with an interrupt
 * and every frame after it in silence.
 */
static void test_a_bare_produce_write_leaves_the_timer_armed(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[16] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x08, 0x06, 0xbe, 0xef };
    int attempt;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    sn0_ioc3_rx_one_frame(qts, sv[0], RAM_SCRATCH, RAM_SCRATCH + 0x4000,
                          (4 << 3) | RPIR_ARM, 0);
    qtest_writel(qts, ENET_INT_STATUS, ENET_INT_RX_TIMER);
    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) & ENET_INT_RX_TIMER,
                    ==, 0);

    /* A bare index write, the shape SGI's driver uses to return buffers. */
    qtest_writel(qts, RX_PRODUCE, 8 << 3);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    for (attempt = 0; attempt < 50; attempt++) {
        if (qtest_readl(qts, RX_CONSUME) != 8) {
            break;
        }
        g_usleep(10000);
    }
    g_assert_cmphex(qtest_readl(qts, RX_CONSUME), ==, 16);

    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) & ENET_INT_RX_TIMER,
                    ==, ENET_INT_RX_TIMER);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A source that set while its enable was clear has to raise the line when the
 * guest enables it. The line is level-driven from status & enable, so the
 * enable register decides it as much as the status register does -- and the
 * order this test uses is not contrived, it is the order every interface
 * bring-up takes: OpenBSD's iec_init stops the interface by writing zero to
 * the enable register, programs the receive enables in the control register,
 * and writes the enable register last.
 *
 * The Bridge's own pin is what gets asserted, so that is what this reads,
 * live, rather than trusting the model's own account of its line.
 */
static void test_enabling_a_set_source_raises_the_line(const void *data)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    /*
     * Interrupts off, then a frame delivered: its source sets and the line
     * must stay low, because nothing is enabled.
     */
    qtest_writel(qts, ENET_INT_ENABLE, 0);
    sn0_ioc3_rx_one_frame(qts, sv[0], RAM_SCRATCH, RAM_SCRATCH + 0x4000,
                          (4 << 3) | RPIR_ARM, 0);

    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) & ENET_INT_RX_TIMER,
                    ==, ENET_INT_RX_TIMER);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_ETH,
                    ==, 0);

    /* Enabling it is the only thing that happens next. */
    qtest_writel(qts, ENET_INT_ENABLE, ENET_INT_RX_TIMER);

    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_ETH,
                    ==, BRIDGE_PIN_ETH);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The register-only ack test above checks that a write clears the status bits
 * it names; it does not check that the line those bits gate actually follows.
 * A model that cleared the register without recomputing the line would pass
 * that test and still leave a real guest's interrupt permanently asserted --
 * the driver's own acknowledge write clears the source, the line stays high
 * from whatever set it, and the very next dispatch finds an asserted line
 * with nothing left in the register to explain it, which every guest here
 * reports as a spurious interrupt rather than diagnosing as a stuck one. This
 * sends a real inline frame requesting a completion interrupt (the same path
 * test_the_ethernet_transmit_sends_inline_frame uses to reach the same bits),
 * then checks the Bridge's own interrupt status for the pin that path's own
 * PCI routing lands on -- live, the same way the SuperIO line's test checks
 * it -- before and after the guest's acknowledge write.
 */
static void test_the_ethernet_interrupt_line_drops_when_acknowledged(
    const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[14] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x08, 0x00 };
    uint32_t desc = RAM_SCRATCH;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN);
    qtest_writel(qts, ENET_INT_ENABLE, ENET_INT_TX_EMPTY | ENET_INT_TX_COMPLETE);

    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_ETH,
                    ==, 0);

    qtest_writel(qts, desc + 0, sizeof(frame) | TXD_DATA_VALID | TXD_INT_DONE);
    qtest_writel(qts, desc + 4, sizeof(frame));
    qtest_memwrite(qts, desc + TXD_DATA_OFFSET, frame, sizeof(frame));
    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);
    qtest_writel(qts, TX_PRODUCE, 128);

    /* The frame the engine sent is drained so the socket does not hold it
     * open across the assertion below; its content is already covered by
     * test_the_ethernet_transmit_sends_inline_frame. */
    {
        uint8_t drained[64];
        net_socket_recv_frame(sv[0], drained, sizeof(drained));
    }

    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_ETH,
                    ==, BRIDGE_PIN_ETH);

    qtest_writel(qts, ENET_INT_STATUS,
                ENET_INT_TX_EMPTY | ENET_INT_TX_COMPLETE);

    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_ETH,
                    ==, 0);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A frame that arrives while the transmit walk is still running must keep
 * its own interrupt source. Nothing in the tests above can see this: they
 * drive one direction at a time, and the two directions only collide when
 * the backend answers a transmitted frame from inside the very call that
 * sent it. That is not a contrived ordering -- it is what every request
 * and reply on QEMU's user-mode network does, because slirp builds its
 * answer synchronously and hands it straight back to this engine's own
 * receive path, several stack frames below the register write that started
 * the transmit.
 *
 * The frame here is an ARP request for the address user-mode networking
 * answers on. Its reply comes back inside the doorbell write, so by the
 * time that write returns, both directions have already run: the consume
 * index has advanced over a delivered frame, and the interrupt status
 * register has to name the receive source as well as the two transmit
 * ones. An engine that read the status register into a local before the
 * walk and wrote that local back afterwards reports only the transmit
 * sources, and a driver dispatching on them never collects the frame.
 */
static QTestState *sn0_ioc3_attach_with_user_net(void)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on "
                                 "-nic user,id=n0");

    sn0_ioc3_attach(qts);
    qtest_writel(qts, CFG0(PCI_IOC3_DEVICE) + PCI_COMMAND,
                PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    return qts;
}

static void test_a_reply_during_transmit_keeps_its_interrupt(const void *data)
{
    QTestState *qts;
    /*
     * Destination broadcast, our own address as the sender, then the
     * hardware and protocol types, the request opcode, and the two
     * address pairs: 10.0.2.15 asking who has 10.0.2.2, which is the
     * address user-mode networking answers for itself.
     */
    uint8_t arp_request[42] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x52, 0x54, 0x00, 0x12, 0x34, 0x56,
        0x08, 0x06,
        0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01,
        0x52, 0x54, 0x00, 0x12, 0x34, 0x56, 10, 0, 2, 15,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 10, 0, 2, 2
    };
    uint8_t landed[42];
    uint32_t desc = RAM_SCRATCH;
    uint32_t ring = RAM_SCRATCH + 0x8000;
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;
    uint32_t word0, word1, status;
    unsigned int i;

    qts = sn0_ioc3_attach_with_user_net();

    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN |
                MCR_RX_DMA_EN | MCR_RX_EN | MCR_RXOFF(33));

    /*
     * Four receive buffers with the timer armed and its reload zero, and the
     * watermark set where this one frame's delivery reaches it, so the reply
     * raises BOTH receive sources and both have to survive the walk.
     *
     * Arming matters more than it looks. An earlier version of this test
     * supplied a bare produce index, so the timer was never in service, no
     * timer source was ever raised, and the whole assertion rode on the
     * threshold source surviving. An engine that preserved the threshold and
     * dropped a timer source raised during the walk would have passed --
     * and the timer source is the only receive source Linux enables, so this
     * milestone's own regression test was guarding the guest that already
     * worked rather than the one the fix was largely for.
     */
    qtest_writel(qts, RX_RING_HI, 0);
    qtest_writel(qts, RX_RING_LO, ring & 0xfffff000u);
    for (i = 0; i < 4; i++) {
        qtest_writel(qts, ring + i * 8, 0);
        qtest_writel(qts, ring + i * 8 + 4, rxbuf + i * 0x1000);
    }
    qtest_writel(qts, RX_TIMER, 0);
    qtest_writel(qts, RX_CONTROL, 3);
    qtest_writel(qts, RX_PRODUCE, (4 << 3) | RPIR_ARM);

    /*
     * One transmit descriptor carrying the request inline, asking for a
     * completion interrupt so both transmit sources are raised too.
     */
    qtest_writel(qts, desc + 0,
                sizeof(arp_request) | TXD_DATA_VALID | TXD_INT_DONE);
    qtest_writel(qts, desc + 4, sizeof(arp_request));
    qtest_memwrite(qts, desc + TXD_DATA_OFFSET, arp_request,
                   sizeof(arp_request));
    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);

    qtest_writel(qts, TX_PRODUCE, 128);

    /*
     * No polling loop here, deliberately. The reply is delivered inside
     * the doorbell write above rather than on a later main-loop pass, and
     * that is the whole condition being tested -- a test that waited for
     * it would also pass on a delivery that arrived afterwards, which is
     * the case the engine already gets right.
     */
    g_assert_cmphex(qtest_readl(qts, RX_CONSUME), ==, 8);

    word0 = qtest_readl(qts, rxbuf + 0);
    g_assert_cmphex(word0 & 0x80000000u, ==, 0x80000000u);

    /*
     * And reported good. Linux gates every delivered frame on this bit and
     * drops the frame without it, while OpenBSD reads the error bits beside
     * it instead -- so an engine that stopped setting it would keep working
     * for one guest and become unable to receive at all for the other.
     */
    word1 = qtest_readl(qts, rxbuf + 4);
    g_assert_cmphex(word1 & ENET_RXE_GOODPKT, ==, ENET_RXE_GOODPKT);

    /* An ARP reply, so what landed is the answer to what was sent. */
    qtest_memread(qts, rxbuf + 66, landed, sizeof(landed));
    g_assert_cmphex(landed[12], ==, 0x08);
    g_assert_cmphex(landed[13], ==, 0x06);
    g_assert_cmphex(landed[21], ==, 0x02);

    status = qtest_readl(qts, ENET_INT_STATUS);
    g_assert_cmphex(status & (ENET_INT_RX_TIMER | ENET_INT_RX_THRESHOLD |
                              ENET_INT_TX_EMPTY | ENET_INT_TX_COMPLETE), ==,
                    ENET_INT_RX_TIMER | ENET_INT_RX_THRESHOLD |
                    ENET_INT_TX_EMPTY | ENET_INT_TX_COMPLETE);

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
    qtest_add_data_func("sn0/ioc3/serial-dma-shadow-is-read-only", NULL,
                        test_the_serial_dma_shadow_is_read_only);
    qtest_add_data_func("sn0/ioc3/ethernet-interrupt-acknowledges", NULL,
                        test_the_ethernet_interrupt_register_acknowledges);
    qtest_add_data_func("sn0/ioc3/ethernet-read-only-registers", NULL,
                        test_the_ethernet_read_only_registers);
    qtest_add_data_func("sn0/ioc3/ethernet-transmit-sends-inline-frame", NULL,
                        test_the_ethernet_transmit_sends_inline_frame);
    qtest_add_data_func("sn0/ioc3/ethernet-transmit-bounds-the-inline-area",
                        NULL,
                        test_the_ethernet_transmit_bounds_the_inline_area);
    qtest_add_data_func("sn0/ioc3/ethernet-unaligned-produce-terminates",
                        NULL, test_an_unaligned_produce_index_terminates);
    qtest_add_data_func("sn0/ioc3/ethernet-transmit-gathers-two-buffers",
                        NULL, test_the_ethernet_transmit_gathers_two_buffers);
    qtest_add_data_func("sn0/ioc3/ethernet-receive-delivers-a-frame", NULL,
                        test_the_ethernet_receive_delivers_a_frame);
    qtest_add_data_func("sn0/ioc3/ethernet-armed-timer-raises-its-source",
                        NULL,
                        test_an_armed_receive_timer_raises_its_own_source);
    qtest_add_data_func("sn0/ioc3/ethernet-below-watermark-raises-nothing",
                        NULL, test_a_frame_below_the_watermark_raises_nothing);
    qtest_add_data_func("sn0/ioc3/ethernet-watermark-raises-threshold", NULL,
                        test_reaching_the_watermark_raises_the_threshold);
    qtest_add_data_func("sn0/ioc3/ethernet-bare-produce-keeps-timer-armed",
                        NULL, test_a_bare_produce_write_leaves_the_timer_armed);
    qtest_add_data_func("sn0/ioc3/ethernet-enable-raises-a-set-source", NULL,
                        test_enabling_a_set_source_raises_the_line);
    qtest_add_data_func("sn0/ioc3/ethernet-interrupt-line-drops-on-ack", NULL,
                        test_the_ethernet_interrupt_line_drops_when_acknowledged);
    qtest_add_data_func("sn0/ioc3/ethernet-reply-during-transmit", NULL,
                        test_a_reply_during_transmit_keeps_its_interrupt);
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
    qtest_add_data_func("sn0/ioc3/one-wire-reset-finds-the-part", NULL,
                        test_a_one_wire_reset_finds_the_ethernet_address_part);
    qtest_add_data_func("sn0/ioc3/one-wire-slot-preserves-timing", NULL,
                        test_one_wire_slot_preserves_the_timing_written);
    qtest_add_data_func("sn0/ioc3/one-wire-search-finds-the-part", NULL,
                        test_a_search_finds_the_ethernet_address_part);
    qtest_add_data_func("sn0/ioc3/one-wire-wrong-match-rom-unselects", NULL,
                        test_a_wrong_match_rom_leaves_the_part_unselected);
    qtest_add_data_func("sn0/ioc3/one-wire-read-memory", NULL,
                        test_reading_the_page_via_read_memory);
    qtest_add_data_func("sn0/ioc3/one-wire-read-memory-crc", NULL,
                        test_reading_the_page_via_read_memory_crc);
    qtest_add_data_func("sn0/ioc3/one-wire-read-status-no-redirection", NULL,
                        test_read_status_returns_no_redirection);
    qtest_add_data_func("sn0/ioc3/pin-control-two-aliases", NULL,
                        test_pin_control_has_two_write_aliases);
    qtest_add_data_func("sn0/ioc3/pin-register-reaches-data-word", NULL,
                        test_a_pin_register_reaches_the_shared_data_word);

    return g_test_run();
}
