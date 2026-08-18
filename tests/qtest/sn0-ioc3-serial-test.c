/*
 * SGI IOC3 base I/O controller: the two serial ports' DMA engines.
 *
 * Each port has an engine that moves characters between a pair of memory
 * rings and its own 16550, and the firmware's base I/O diagnostic drives both
 * of them. The engines are separate from the byte-wide register files the
 * console uses, so nothing here is on the console's path: a console that works
 * proves none of it, and every assertion in this file is one only a test can
 * make.
 *
 * The rings themselves are the reason this is its own file rather than more of
 * the register suite. Four rings share one base register and are told apart by
 * port and direction alone, which is an arrangement with no read-back: a model
 * that resolved a ring to the wrong address would answer every register
 * correctly and put the characters somewhere else. So the ring addresses are
 * asserted by reading the memory the engine wrote, not by reading a register
 * back.
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
 * The two things a guest's own bus scan would do before the bridge's device
 * window reaches anything: give the controller a PCI address by writing its
 * base address register, and aim the window at that same address. The
 * crosstalk-side offset is fixed by slot number, so IOC3_BASE above still
 * names where to read the result -- only the PCI-side address is ours to pick.
 */
#define BRIDGE_BASE     (SPACE_XTALK + ((uint64_t)CONSOLE_WIDGET << 24))
#define REG_DATA        4         /* registers sit at a slot's upper half */
#define BRIDGE_CFG0     0x020000
#define CFG0_DEVICE_STRIDE 0x1000
#define CFG0(device)    (BRIDGE_BASE + BRIDGE_CFG0 + \
                         (device) * CFG0_DEVICE_STRIDE)
#define BRIDGE_DEVICE(n) (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)
#define DEV_MEMORY_SPACE (1u << 12)

#define PCI_IOC3_DEVICE  2
#define PCI_COMMAND      0x04
#define PCI_COMMAND_MEMORY 0x0002
#define PCI_COMMAND_MASTER 0x0004
#define PCI_BAR0         0x10
#define IOC3_PCI_MMIO    0x10000000u

static void sn0_ioc3_attach(QTestState *qts)
{
    qtest_writel(qts, CFG0(PCI_IOC3_DEVICE) + PCI_BAR0, IOC3_PCI_MMIO);
    qtest_writel(qts, CFG0(PCI_IOC3_DEVICE) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DEVICE(PCI_IOC3_DEVICE),
                 DEV_MEMORY_SPACE | (IOC3_PCI_MMIO >> 20));
}

/* The chip control register, which carries the two ports' SuperIO offsets. */
#define CHIP_CONTROL    (IOC3_BASE + 0x28)

/* The SuperIO interrupt block. */
#define INT_STATUS      (IOC3_BASE + 0x1c)
#define INT_ENABLE_SET  (IOC3_BASE + 0x20)

/*
 * The nine sources a serial port raises, at their offsets from that port's
 * first bit. Port A's block starts at bit 0 and port B's at bit 9.
 */
#define SRC_TX_RING_EMPTY   0
#define SRC_RX_RING_FULL    1
#define SRC_RX_HIGH_WATER   2
#define SRC_RX_TIMER        3
#define SRC_CD_CHANGED      4
#define SRC_CTS_CHANGED     5
#define SRC_PASSTHROUGH     6
#define SRC_TX_DONE         7
#define SRC_PCI_ERROR       8
#define PORT_SOURCE_STRIDE  9

#define SOURCE(port, n) (1u << ((port) * PORT_SOURCE_STRIDE + (n)))

/* The serial DMA block: one shared ring base, then seven registers a port. */
#define SDMA_RING_HI    (IOC3_BASE + 0x0b0)
#define SDMA_RING_LO    (IOC3_BASE + 0x0b4)
#define SDMA_PORT_A     (IOC3_BASE + 0x0b8)
#define SDMA_PORT_B     (IOC3_BASE + 0x0d4)

#define SDMA_CONTROL    0x00
#define SDMA_TX_PRODUCE 0x04
#define SDMA_TX_CONSUME 0x08
#define SDMA_RX_PRODUCE 0x0c
#define SDMA_RX_CONSUME 0x10
#define SDMA_RX_TIMER   0x14
#define SDMA_SHADOW     0x18

static uint64_t sdma(unsigned int port, unsigned int reg)
{
    return (port ? SDMA_PORT_B : SDMA_PORT_A) + reg;
}

/* The ring base register's low word: a size select and a 4 KB-aligned base. */
#define RING_4K         (1u << 0)

/* The port control register's fields. */
#define CTRL_HIGH_WATER(n) ((n) & 0x1ffu)
#define CTRL_DMA_ENABLE (1u << 28)
#define CTRL_PAUSE      (1u << 29)
#define CTRL_PAUSED     (1u << 30)
#define CTRL_RESET      (1u << 31)

/*
 * A transmit entry's control byte: a two-bit type in the top bits, and an
 * independent request for an interrupt once that byte has gone out.
 */
#define TX_EMPTY        0x00
#define TX_CHARACTER    0x40
#define TX_MODEM        0x80
#define TX_DELAY        0xc0
#define TX_INTERRUPT    0x20

/* A receive entry's status byte. */
#define RX_OVERRUN      0x01
#define RX_PARITY       0x02
#define RX_FRAMING      0x04
#define RX_BREAK        0x08
#define RX_CTS          0x10
#define RX_CD           0x20
#define RX_MODEM_VALID  0x40
#define RX_DATA_VALID   0x80

/* The two 16550 files, and the registers these tests reach in them. */
#define SERIAL_A        (IOC3_BASE + 0x020178)
#define SERIAL_B        (IOC3_BASE + 0x020170)
#define REG_FCR         2
#define REG_LCR         3
#define REG_MCR         4

#define MCR_DTR         0x01
#define MCR_RTS         0x02
#define MCR_LOOPBACK    0x10

static uint64_t serial_port(unsigned int port)
{
    return port ? SERIAL_B : SERIAL_A;
}

/*
 * A read on the port's own socket is how a test sees a character leave the
 * chip, and a model that sends nothing would otherwise hang the suite at that
 * read rather than fail it. A deadline turns the one into the other.
 */
static void serial_socket_deadline(int fd)
{
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };

    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)),
                    ==, 0);
}

/*
 * Where the rings go. Guest RAM, 4 KB aligned as the base register requires,
 * and far enough into memory that nothing the firmware left behind is under
 * it. With 4 KB rings selected the block is four of them: port A transmit,
 * port A receive, port B transmit, port B receive, in that order.
 */
#define RING_BASE       0x00100000u
#define RING_SIZE       0x1000u
#define RING_TX(port)   (RING_BASE + (port) * 2 * RING_SIZE)
#define RING_RX(port)   (RING_TX(port) + RING_SIZE)

/*
 * Put one entry in a transmit ring: four data bytes and the four control
 * bytes that say what each of them is. A caller passing fewer than four
 * characters leaves the remaining control bytes empty, which is what an
 * engine must stop on.
 */
static void write_transmit_entry(QTestState *qts, uint32_t ring,
                                 uint32_t offset,
                                 const uint8_t *data, const uint8_t *control)
{
    qtest_memwrite(qts, ring + offset, data, 4);
    qtest_memwrite(qts, ring + offset + 4, control, 4);
}

/*
 * Bring one port's engine up the way a driver does: aim the shared ring base
 * at guest RAM with 4 KB rings, clear both indices, and enable ring DMA.
 */
static void enable_dma(QTestState *qts, unsigned int port)
{
    qtest_writel(qts, SDMA_RING_HI, 0);
    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);
    qtest_writel(qts, sdma(port, SDMA_TX_PRODUCE), 0);
    qtest_writel(qts, sdma(port, SDMA_RX_CONSUME), 0);
    qtest_writel(qts, sdma(port, SDMA_CONTROL), CTRL_DMA_ENABLE);
}

/* Eight data bits, one stop bit, no parity: the line the firmware sets up. */
static void configure_line(QTestState *qts, unsigned int port)
{
    qtest_writeb(qts, serial_port(port) + REG_LCR, 0x03);
    qtest_writeb(qts, serial_port(port) + REG_FCR, 0x01);
}

static void set_loopback(QTestState *qts, unsigned int port)
{
    qtest_writeb(qts, serial_port(port) + REG_MCR,
                 MCR_LOOPBACK | MCR_DTR | MCR_RTS);
}

/*
 * The value the firmware writes to the chip control register: port A's
 * SuperIO offset 0x178 divided by eight in bits 1-7, port B's 0x170 the same
 * way in bits 8-14, and a byte bus strobe length of 4. It is written on every
 * boot and read by nothing, so a model that dropped it would look identical
 * from a transcript -- which is why it is asserted here or not at all.
 */
static void test_the_control_register_round_trips(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    qtest_writel(qts, CHIP_CONTROL, 0x00022e5e);
    g_assert_cmphex(qtest_readl(qts, CHIP_CONTROL), ==, 0x00022e5e);

    qtest_quit(qts);
}

/*
 * The index registers address the ring in eight-byte units, so the pointer
 * occupies bits three upward and its width follows the ring size currently
 * selected: bits 3-9 for 1 KB rings and bits 3-11 for 4 KB ones. A model
 * storing the whole word answers every ordinary driver write unchanged --
 * every one of them is already in range -- so only a write of the bits the
 * register does not own can tell the two apart.
 */
static void test_an_index_keeps_only_its_pointer_field(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /* 1 KB rings: the pointer is bits 3-9. */
    qtest_writel(qts, SDMA_RING_LO, RING_BASE);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_PRODUCE)), ==, 0x3f8);

    /* 4 KB rings: two bits wider, and the same register. */
    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_PRODUCE)), ==, 0xff8);

    /* The second port's transmit index is a register of its own, not a view. */
    qtest_writel(qts, sdma(1, SDMA_TX_PRODUCE), 0x18);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_PRODUCE)), ==, 0xff8);
    g_assert_cmphex(qtest_readl(qts, sdma(1, SDMA_TX_PRODUCE)), ==, 0x18);

    qtest_quit(qts);
}

/*
 * The receive consume index is the one index that carries something besides a
 * pointer: bit 31 arms the receive timer. So it keeps one bit more than its
 * three siblings, and a model masking all four alike drops the arming bit
 * without any driver noticing until an interrupt fails to arrive.
 */
static void test_the_receive_consume_keeps_its_arming_bit(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);
    qtest_writel(qts, sdma(0, SDMA_RX_CONSUME), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_RX_CONSUME)), ==,
                    0x800000f8 | 0xf00);

    qtest_quit(qts);
}

/*
 * The receive timer holds a twelve-bit reload in its low bits and reads its
 * current count back in bits 16-27. Nothing above bit 27 belongs to it. The
 * ethernet block has a timer at a different offset that keeps eleven bits, so
 * a model sharing one width between them is wrong for one of the two.
 */
static void test_the_receive_timer_keeps_twelve_bits(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    uint32_t val;

    sn0_ioc3_attach(qts);

    qtest_writel(qts, sdma(0, SDMA_RX_TIMER), 0xffffffff);
    val = qtest_readl(qts, sdma(0, SDMA_RX_TIMER));
    g_assert_cmphex(val & 0xfff, ==, 0xfff);
    g_assert_cmphex(val & ~0x0fff0fffu, ==, 0);

    qtest_quit(qts);
}

/*
 * A transmit entry carries four characters and the engine sends all four. The
 * port's own serial line is where they have to arrive: an engine that walked
 * the ring and advanced its index without handing anything to the 16550 moves
 * every register correctly and sends nothing, and no register read tells the
 * two apart.
 */
static void test_a_transmit_entry_sends_its_four_characters(const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t out[4] = { 'I', 'O', 'C', '3' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };
    char got[4];

    qts = qtest_init_with_serial("-M origin200,io-bridge=on", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    enable_dma(qts, 0);

    write_transmit_entry(qts, RING_TX(0), 0, out, control);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 8);

    g_assert_cmpint(read(sock, got, sizeof(got)), ==, (int)sizeof(got));
    g_assert_cmpint(memcmp(got, out, sizeof(out)), ==, 0);

    /* The engine reports where it got to, in the same eight-byte units. */
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_CONSUME)) & 0xff8, ==, 8);

    close(sock);
    qtest_quit(qts);
}

/*
 * An entry whose control byte asks for one raises the explicit transmit
 * completion source, and a ring the engine has walked to its end raises
 * transmit ring empty. They are separate sources and a driver may enable
 * either without the other, so a model raising one for both conditions
 * satisfies a driver watching that one and never prompts the other.
 */
static void test_a_completed_transmit_raises_both_its_sources(const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t out[4] = { 'd', 'm', 'a', '!' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
        TX_CHARACTER | TX_INTERRUPT,
    };
    uint32_t status;

    qts = qtest_init_with_serial("-M origin200,io-bridge=on", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    enable_dma(qts, 0);

    write_transmit_entry(qts, RING_TX(0), 0, out, control);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 8);

    status = qtest_readl(qts, INT_STATUS);
    g_assert_cmphex(status & SOURCE(0, SRC_TX_DONE), ==,
                    SOURCE(0, SRC_TX_DONE));
    g_assert_cmphex(status & SOURCE(0, SRC_TX_RING_EMPTY), ==,
                    SOURCE(0, SRC_TX_RING_EMPTY));

    /* Neither of them belongs to the other port. */
    g_assert_cmphex(status & SOURCE(1, SRC_TX_DONE), ==, 0);
    g_assert_cmphex(status & SOURCE(1, SRC_TX_RING_EMPTY), ==, 0);

    close(sock);
    qtest_quit(qts);
}

/*
 * An entry with no interrupt requested raises the ring-empty source and not
 * the completion one. Asserting only that the completion source appears when
 * asked for passes on a model that raises it unconditionally, which is a
 * driver taking an interrupt per entry on a queue it meant to complete once.
 */
static void test_an_entry_that_asked_for_nothing_gets_nothing(const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t out[4] = { 'q', 'u', 'i', 't' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };

    qts = qtest_init_with_serial("-M origin200,io-bridge=on", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    enable_dma(qts, 0);

    write_transmit_entry(qts, RING_TX(0), 0, out, control);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 8);

    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_TX_DONE),
                    ==, 0);

    close(sock);
    qtest_quit(qts);
}

/*
 * A ring the engine has not been told to walk stays where it is. Enabling DMA
 * is the guest's decision and the console depends on it staying off: the
 * firmware and both guests drive these ports through the byte-wide window
 * with every one of these registers at zero, so an engine that ran without
 * being enabled would take characters out of memory nobody put there.
 */
static void test_a_disabled_engine_walks_nothing(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    const uint8_t out[4] = { 'n', 'o', 'p', 'e' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
        TX_CHARACTER | TX_INTERRUPT,
    };

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);

    qtest_writel(qts, SDMA_RING_HI, 0);
    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);
    write_transmit_entry(qts, RING_TX(0), 0, out, control);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 8);

    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_CONSUME)) & 0xff8, ==, 0);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS), ==, 0);

    qtest_quit(qts);
}

/*
 * The four rings are one allocation told apart by port and direction, and
 * that is the arrangement with no read-back anywhere: every index register
 * answers identically whichever ring the engine resolved. So each ring is
 * pinned by where a character actually lands.
 *
 * With the port's 16550 in loopback, a character the transmit engine sends
 * comes straight back to the same port's receiver, and the receive engine
 * writes it into that port's receive ring. Reading it out of guest RAM at the
 * one address the datasheet's fixed order allows is what fails on a model
 * that resolved the ring anywhere else -- including at the base plus the
 * ring-size select bit, which is the arithmetic slip this order invites.
 */
static void ring_block_one_port(unsigned int port)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    const uint8_t out[4] = { 'r', 'i', 'n', 'g' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };
    uint8_t landed[8];
    unsigned int i;

    sn0_ioc3_attach(qts);
    configure_line(qts, port);
    set_loopback(qts, port);
    enable_dma(qts, port);

    write_transmit_entry(qts, RING_TX(port), 0, out, control);
    qtest_writel(qts, sdma(port, SDMA_TX_PRODUCE), 8);

    /* The engine says it filled one entry, in eight-byte units. */
    g_assert_cmphex(qtest_readl(qts, sdma(port, SDMA_RX_PRODUCE)) & 0xff8,
                    ==, 8);

    qtest_memread(qts, RING_RX(port), landed, sizeof(landed));
    for (i = 0; i < 4; i++) {
        g_assert_cmphex(landed[i], ==, out[i]);
        g_assert_cmphex(landed[4 + i] & RX_DATA_VALID, ==, RX_DATA_VALID);
    }

    qtest_quit(qts);
}

static void test_port_a_uses_the_first_two_rings(const void *data)
{
    ring_block_one_port(0);
}

static void test_port_b_uses_the_second_two_rings(const void *data)
{
    ring_block_one_port(1);
}

/*
 * The ring-size select is bit 0 of the base register and the base is bits 12
 * upward, so the select bit must never reach an address. A base written with
 * the bit set resolves to the same rings as one written without it, and this
 * is the assertion that fails on a model or-ing the whole register into its
 * arithmetic.
 */
static void test_the_size_bit_is_not_part_of_the_address(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    const uint8_t out[4] = { 'b', 'a', 's', 'e' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };
    uint8_t landed[4];

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    set_loopback(qts, 0);

    /*
     * RING_4K is bit 0 and RING_BASE is 4 KB aligned, so the sum is the
     * register's full contents and the address is the base alone.
     */
    qtest_writel(qts, SDMA_RING_HI, 0);
    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);
    qtest_writel(qts, sdma(0, SDMA_CONTROL), CTRL_DMA_ENABLE);

    write_transmit_entry(qts, RING_TX(0), 0, out, control);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 8);

    qtest_memread(qts, RING_RX(0), landed, sizeof(landed));
    g_assert_cmpint(memcmp(landed, out, sizeof(out)), ==, 0);

    qtest_quit(qts);
}

/*
 * A paused engine reports the pause and stops walking, and reads its own
 * acknowledgement back rather than only the bit the guest wrote. A model
 * storing the pause request and never answering the acknowledgement leaves a
 * driver waiting for a bit that never appears.
 */
static void test_a_pause_is_acknowledged_and_stops_the_walk(const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t out[4] = { 's', 't', 'o', 'p' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };

    qts = qtest_init_with_serial("-M origin200,io-bridge=on", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    enable_dma(qts, 0);

    qtest_writel(qts, sdma(0, SDMA_CONTROL), CTRL_DMA_ENABLE | CTRL_PAUSE);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_CONTROL)) & CTRL_PAUSED,
                    ==, CTRL_PAUSED);

    write_transmit_entry(qts, RING_TX(0), 0, out, control);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 8);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_CONSUME)) & 0xff8, ==, 0);

    /* Releasing the pause lets the entry the guest already handed over go. */
    qtest_writel(qts, sdma(0, SDMA_CONTROL), CTRL_DMA_ENABLE);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_CONTROL)) & CTRL_PAUSED,
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_CONSUME)) & 0xff8, ==, 8);

    close(sock);
    qtest_quit(qts);
}

/*
 * A reset puts both of the port's channels back at the start of their rings.
 * It is the one control bit that is an action rather than a setting, and a
 * model storing it like the rest leaves the engine held in reset for as long
 * as the guest leaves the bit set -- which every driver does, because it
 * writes the bit once and moves on.
 */
static void test_a_reset_returns_the_channels_to_the_start(const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t out[4] = { 'r', 's', 't', '!' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };

    qts = qtest_init_with_serial("-M origin200,io-bridge=on", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    enable_dma(qts, 0);

    write_transmit_entry(qts, RING_TX(0), 0, out, control);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 8);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_CONSUME)) & 0xff8, ==, 8);

    qtest_writel(qts, sdma(0, SDMA_CONTROL), CTRL_DMA_ENABLE | CTRL_RESET);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_CONSUME)) & 0xff8, ==, 0);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_PRODUCE)) & 0xff8, ==, 0);

    close(sock);
    qtest_quit(qts);
}

/*
 * The line the port drives is the engine's to set as well as the byte-wide
 * window's: an entry whose control byte types its data byte as modem control
 * reaches the 16550's modem control register rather than its transmitter. A
 * model treating every entry as a character sends the modem byte down the
 * wire, which is a stray character on the console and a modem line that never
 * moves.
 */
static void test_a_modem_entry_reaches_the_modem_register(const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t out[4] = { MCR_LOOPBACK | MCR_DTR | MCR_RTS, 0, 0, 0 };
    const uint8_t control[4] = { TX_MODEM, TX_EMPTY, TX_EMPTY, TX_EMPTY };
    const uint8_t after[4] = { 'x', 0, 0, 0 };
    const uint8_t after_control[4] = {
        TX_CHARACTER, TX_EMPTY, TX_EMPTY, TX_EMPTY,
    };
    uint8_t landed[8];

    qts = qtest_init_with_serial("-M origin200,io-bridge=on", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    enable_dma(qts, 0);

    /* The modem entry turns loopback on through the engine alone. */
    write_transmit_entry(qts, RING_TX(0), 0, out, control);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 8);

    /* So the character in the next entry comes back rather than going out. */
    write_transmit_entry(qts, RING_TX(0), 8, after, after_control);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 16);

    qtest_memread(qts, RING_RX(0), landed, sizeof(landed));
    g_assert_cmphex(landed[0], ==, 'x');
    g_assert_cmphex(landed[4] & RX_DATA_VALID, ==, RX_DATA_VALID);

    close(sock);
    qtest_quit(qts);
}

/*
 * An empty entry ends the walk. The type field's both-bits-clear value means
 * the entry holds nothing, and a driver fills only as many of an entry's four
 * slots as it has characters for. A model sending all four regardless emits
 * three bytes of whatever was in memory after every short write, which on a
 * console is visible garbage and in a test is the only way to see it.
 */
static void test_an_empty_slot_ends_the_entry(const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t out[4] = { 'h', 'i', 0xff, 0xff };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_EMPTY, TX_EMPTY,
    };
    char got[3];
    ssize_t n;

    qts = qtest_init_with_serial("-M origin200,io-bridge=on", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    enable_dma(qts, 0);

    write_transmit_entry(qts, RING_TX(0), 0, out, control);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 8);

    g_assert_cmpint(read(sock, got, 2), ==, 2);
    g_assert_cmpint(memcmp(got, out, 2), ==, 0);

    /* Nothing follows the two characters the entry actually carried. */
    g_assert_cmpint(fcntl(sock, F_SETFL, O_NONBLOCK), ==, 0);
    n = read(sock, got, sizeof(got));
    g_assert_true(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));

    close(sock);
    qtest_quit(qts);
}

/*
 * The engine wraps at the end of its ring rather than running past it. The
 * pointer field is the ring's whole extent, so the entry after the last one
 * is the first, and a model adding eight without wrapping walks into the next
 * ring in the block -- which is another port's, and silently.
 */
static void test_the_walk_wraps_at_the_end_of_the_ring(const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t out[4] = { 'w', 'r', 'a', 'p' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };
    char got[4];

    qts = qtest_init_with_serial("-M origin200,io-bridge=on", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    enable_dma(qts, 0);

    /* Start both indices at the ring's last entry. */
    qtest_writel(qts, sdma(0, SDMA_CONTROL), CTRL_DMA_ENABLE | CTRL_RESET);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), RING_SIZE - 8);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_CONSUME)) & 0xff8,
                    ==, RING_SIZE - 8);

    /* One more entry takes the engine round to the ring's first. */
    write_transmit_entry(qts, RING_TX(0), RING_SIZE - 8, out, control);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 0);

    g_assert_cmpint(read(sock, got, sizeof(got)), ==, (int)sizeof(got));
    g_assert_cmpint(memcmp(got, out, sizeof(out)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_CONSUME)) & 0xff8, ==, 0);

    close(sock);
    qtest_quit(qts);
}

/*
 * A source raised while the engine is enabled reaches the SuperIO line only
 * once the guest has enabled it, and the line follows the intersection rather
 * than the event. The serial DMA sources are new members of a register that
 * already had this contract, so what is asserted here is that they join it
 * rather than raising a line of their own.
 */
static void test_a_serial_dma_source_follows_the_enable_mask(const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t out[4] = { 'i', 'r', 'q', '?' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_EMPTY, TX_EMPTY, TX_EMPTY,
    };

    qts = qtest_init_with_serial("-M origin200,io-bridge=on", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    enable_dma(qts, 0);

    write_transmit_entry(qts, RING_TX(0), 0, out, control);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 8);

    /* Latched: it is set in the status register with nothing enabled. */
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_TX_RING_EMPTY),
                    ==, SOURCE(0, SRC_TX_RING_EMPTY));

    /* And it acknowledges, which the two pass-through bits do not. */
    qtest_writel(qts, INT_STATUS, SOURCE(0, SRC_TX_RING_EMPTY));
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_TX_RING_EMPTY),
                    ==, 0);

    close(sock);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/ioc3-serial/control-register-round-trips", NULL,
                        test_the_control_register_round_trips);
    qtest_add_data_func("sn0/ioc3-serial/index-keeps-its-pointer-field", NULL,
                        test_an_index_keeps_only_its_pointer_field);
    qtest_add_data_func("sn0/ioc3-serial/receive-consume-keeps-arming-bit",
                        NULL,
                        test_the_receive_consume_keeps_its_arming_bit);
    qtest_add_data_func("sn0/ioc3-serial/receive-timer-keeps-twelve-bits", NULL,
                        test_the_receive_timer_keeps_twelve_bits);
    qtest_add_data_func("sn0/ioc3-serial/transmit-entry-sends-four", NULL,
                        test_a_transmit_entry_sends_its_four_characters);
    qtest_add_data_func("sn0/ioc3-serial/completed-transmit-raises-sources",
                        NULL,
                        test_a_completed_transmit_raises_both_its_sources);
    qtest_add_data_func("sn0/ioc3-serial/no-interrupt-asked-none-given", NULL,
                        test_an_entry_that_asked_for_nothing_gets_nothing);
    qtest_add_data_func("sn0/ioc3-serial/disabled-engine-walks-nothing", NULL,
                        test_a_disabled_engine_walks_nothing);
    qtest_add_data_func("sn0/ioc3-serial/port-a-uses-the-first-two-rings", NULL,
                        test_port_a_uses_the_first_two_rings);
    qtest_add_data_func("sn0/ioc3-serial/port-b-uses-the-second-two-rings",
                        NULL, test_port_b_uses_the_second_two_rings);
    qtest_add_data_func("sn0/ioc3-serial/size-bit-is-not-an-address", NULL,
                        test_the_size_bit_is_not_part_of_the_address);
    qtest_add_data_func("sn0/ioc3-serial/pause-is-acknowledged", NULL,
                        test_a_pause_is_acknowledged_and_stops_the_walk);
    qtest_add_data_func("sn0/ioc3-serial/reset-returns-to-the-start", NULL,
                        test_a_reset_returns_the_channels_to_the_start);
    qtest_add_data_func("sn0/ioc3-serial/modem-entry-reaches-modem-register",
                        NULL, test_a_modem_entry_reaches_the_modem_register);
    qtest_add_data_func("sn0/ioc3-serial/empty-slot-ends-the-entry", NULL,
                        test_an_empty_slot_ends_the_entry);
    qtest_add_data_func("sn0/ioc3-serial/walk-wraps-at-the-ring-end", NULL,
                        test_the_walk_wraps_at_the_end_of_the_ring);
    qtest_add_data_func("sn0/ioc3-serial/source-follows-the-enable-mask", NULL,
                        test_a_serial_dma_source_follows_the_enable_mask);

    return g_test_run();
}
