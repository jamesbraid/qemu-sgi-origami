/*
 * SGI IOC3 base I/O controller: the two serial ports' DMA engines.
 *
 * The engines are off the console's path, so a working console proves
 * none of this. Four rings share one base register with no read-back --
 * a model resolving a ring to the wrong address answers every register
 * correctly -- so ring addresses are asserted by reading the memory the
 * engine wrote. Every test asks for the I/O bridge explicitly; without
 * it nothing answers at the controller's address.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* Crosstalk space, and the chip's window inside the console widget. */
#define SPACE_XTALK     0x20000000000ULL
#define CONSOLE_WIDGET  8
#define BRIDGE_DEVICE2  0x600000
#define IOC3_BASE       (SPACE_XTALK + ((uint64_t)CONSOLE_WIDGET << 24) + \
                         BRIDGE_DEVICE2)

/*
 * A guest's bus scan: assign the controller a PCI address and aim the
 * bridge's device window at it. The crosstalk-side offset stays fixed.
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

/*
 * A crosstalk address one node stride up reaches the same register on
 * another node's controller: each node maps its own copy of the console
 * widget at SN0_SPACE_XTALK + (nasid << 32), the offset the dual-node model
 * uses. node_base is 0 for the console node.
 */
#define SN0_NODE_STRIDE  0x100000000ULL

static void sn0_ioc3_attach_node(QTestState *qts, uint64_t node_base)
{
    qtest_writel(qts, node_base + CFG0(PCI_IOC3_DEVICE) + PCI_BAR0,
                 IOC3_PCI_MMIO);
    qtest_writel(qts, node_base + CFG0(PCI_IOC3_DEVICE) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, node_base + BRIDGE_DEVICE(PCI_IOC3_DEVICE),
                 DEV_MEMORY_SPACE | (IOC3_PCI_MMIO >> 20));
}

static void sn0_ioc3_attach(QTestState *qts)
{
    sn0_ioc3_attach_node(qts, 0);
}

/* The chip control register, which carries the two ports' SuperIO offsets. */
#define CHIP_CONTROL    (IOC3_BASE + 0x28)
/* Bit 22 is the read-only PCI-arbiter-idle status the read always reports. */
#define CHIP_CTRL_ARB_IDLE  0x00400000u

/* The SuperIO interrupt block. */
#define INT_STATUS      (IOC3_BASE + 0x1c)
#define INT_ENABLE_SET  (IOC3_BASE + 0x20)

/* The nine per-port sources: port A at bit 0, port B at bit 9. */
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

/*
 * The 16550 shadow's fields: line status, modem status and the driven
 * modem-control levels, mirrored for ring-side software.
 */
#define SHADOW_DR       (1u << 0)
#define SHADOW_THRE     (1u << 5)
#define SHADOW_TSRE     (1u << 6)
#define SHADOW_CTS      (1u << 20)
#define SHADOW_CD       (1u << 23)
#define SHADOW_DTR      (1u << 24)
#define SHADOW_RTS      (1u << 25)

static uint64_t sdma(unsigned int port, unsigned int reg)
{
    return (port ? SDMA_PORT_B : SDMA_PORT_A) + reg;
}

/* Alias pages: one port's seven registers on a page of their own. */
#define SDMA_ALIAS_A    (IOC3_BASE + 0x08000)
#define SDMA_ALIAS_B    (IOC3_BASE + 0x0c000)

static uint64_t sdma_alias(unsigned int port, unsigned int reg)
{
    return (port ? SDMA_ALIAS_B : SDMA_ALIAS_A) + reg;
}

/* The ring base register's low word: a size select and a 4 KB-aligned base. */
#define RING_4K         (1u << 0)

/* The port control register's fields. */
#define CTRL_HIGH_WATER(n) ((n) & 0x1ffu)
#define CTRL_RX_DRAIN   (1u << 27)
#define CTRL_DMA_ENABLE (1u << 28)
#define CTRL_PAUSE      (1u << 29)
#define CTRL_PAUSED     (1u << 30)
#define CTRL_RESET      (1u << 31)

/* A transmit control byte: a two-bit type plus an interrupt request. */
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
#define REG_RBR         0
#define REG_IER         1
#define REG_FCR         2
#define REG_LCR         3
#define REG_MCR         4
#define REG_LSR         5

#define IER_RDI         0x01
#define LSR_DR          0x01

#define MCR_DTR         0x01
#define MCR_RTS         0x02
#define MCR_LOOPBACK    0x10

/* The receive consume index's top bit arms the port's receive timer. */
#define CONSUME_ARM_TIMER (1u << 31)

/* One tick of the receive timer's 16 kHz clock, in nanoseconds. */
#define RX_TIMER_TICK_NS 62500LL

/* The receive status byte's four line-error flags together. */
#define RX_ERRORS       (RX_OVERRUN | RX_PARITY | RX_FRAMING | RX_BREAK)

/*
 * IRIX 6.5.30's ring-console SuperIO enable mask, read live off a stalled
 * machine: the ring receive sources and the keyboard, not the 16550
 * pass-through.
 */
#define IRIX_RING_ENABLE_MASK 0x0040000c

static uint64_t serial_port(unsigned int port)
{
    return port ? SERIAL_B : SERIAL_A;
}

/*
 * Turns a would-be hang into a failure; 120s is the load-triage deadline
 * (docs/notebooks/ioc3-serial-dma.md in the project tree).
 */
#define IOC3_SERIAL_SOCKET_TIMEOUT_SECS 120

static void serial_socket_deadline(int fd)
{
    struct timeval tv = { .tv_sec = IOC3_SERIAL_SOCKET_TIMEOUT_SECS,
                           .tv_usec = 0 };

    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)),
                    ==, 0);
}

/*
 * A TCP listener serial_hd(1) connects out to: the only proof chardev-b is
 * wired, since qtest_init_with_serial wires only serial_hd(0).
 */
static int portb_listen(uint16_t *port)
{
    struct sockaddr_in addr = { 0 };
    socklen_t addrlen = sizeof(addr);
    int lfd;

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    g_assert(lfd >= 0);
    g_assert(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    g_assert(getsockname(lfd, (struct sockaddr *)&addr, &addrlen) == 0);
    *port = ntohs(addr.sin_port);
    g_assert(listen(lfd, 1) == 0);
    return lfd;
}

static int portb_accept(int lfd)
{
    fd_set readfds;
    struct timeval tv = { .tv_sec = IOC3_SERIAL_SOCKET_TIMEOUT_SECS };
    int fd, val;

    FD_ZERO(&readfds);
    FD_SET(lfd, &readfds);
    g_assert_cmpint(select(lfd + 1, &readfds, NULL, NULL, &tv), ==, 1);
    fd = accept(lfd, NULL, 0);
    g_assert(fd >= 0);
    val = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
    serial_socket_deadline(fd);
    return fd;
}

/* Rings in guest RAM: TX A, RX A, TX B, RX B, in the fixed order. */
#define RING_BASE       0x00100000u
#define RING_SIZE       0x1000u
#define RING_TX(port)   (RING_BASE + (port) * 2 * RING_SIZE)
#define RING_RX(port)   (RING_TX(port) + RING_SIZE)

/* Four data bytes and their control bytes; unfilled slots stay empty. */
static void write_transmit_entry(QTestState *qts, uint32_t ring,
                                 uint32_t offset,
                                 const uint8_t *data, const uint8_t *control)
{
    qtest_memwrite(qts, ring + offset, data, 4);
    qtest_memwrite(qts, ring + offset + 4, control, 4);
}

/* Aim the ring base, clear the indices, enable ring DMA. */
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
 * Collect n characters in order under data-valid flags; entry grouping is
 * the host's chunking, so nothing built on this flakes on segmentation.
 */
static void collect_rx_chars(QTestState *qts, unsigned int port,
                             unsigned int n, uint8_t *data, uint8_t *status)
{
    unsigned int got = 0;

    g_test_timer_start();
    while (got < n) {
        uint32_t produce = qtest_readl(qts, sdma(port, SDMA_RX_PRODUCE)) &
                           0xff8;
        uint32_t offset;

        got = 0;
        for (offset = 0; offset < produce && got < n; offset += 8) {
            uint8_t entry[8];
            unsigned int i;

            qtest_memread(qts, RING_RX(port) + offset, entry, sizeof(entry));
            for (i = 0; i < 4 && got < n; i++) {
                if (entry[4 + i] & RX_DATA_VALID) {
                    data[got] = entry[i];
                    status[got] = entry[4 + i];
                    got++;
                }
            }
        }

        if (got < n) {
            g_assert(g_test_timer_elapsed() < IOC3_SERIAL_SOCKET_TIMEOUT_SECS);
            g_usleep(1000);
        }
    }
}

/* Wait for a SuperIO interrupt source to appear in the status register. */
static void wait_for_source(QTestState *qts, uint32_t bit)
{
    g_test_timer_start();
    while ((qtest_readl(qts, INT_STATUS) & bit) != bit) {
        g_assert(g_test_timer_elapsed() < IOC3_SERIAL_SOCKET_TIMEOUT_SECS);
        g_usleep(1000);
    }
}

/* No guest reads these fields back: asserted here or not at all. */
static void test_the_control_register_round_trips(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    sn0_ioc3_attach(qts);

    qtest_writel(qts, CHIP_CONTROL, 0x00022e5e);
    g_assert_cmphex(qtest_readl(qts, CHIP_CONTROL), ==,
                    0x00022e5e | CHIP_CTRL_ARB_IDLE);

    qtest_quit(qts);
}

/*
 * The pointer's width follows the selected ring size. Every ordinary driver
 * write is already in range, so only writing bits the register does not own
 * tells a masked pointer from a stored whole word.
 */
static void test_an_index_keeps_only_its_pointer_field(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

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
 * Bit 31 arms the receive timer, so this index keeps one bit more than its
 * three siblings.
 */
static void test_the_receive_consume_keeps_its_arming_bit(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    sn0_ioc3_attach(qts);

    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);
    qtest_writel(qts, sdma(0, SDMA_RX_CONSUME), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_RX_CONSUME)), ==,
                    0x800000f8 | 0xf00);

    qtest_quit(qts);
}

/*
 * Twelve-bit reload, count read back in bits 16-27; the ethernet block's
 * timer keeps eleven bits, so a shared width is wrong for one of the two.
 */
static void test_the_receive_timer_keeps_twelve_bits(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint32_t val;

    sn0_ioc3_attach(qts);

    qtest_writel(qts, sdma(0, SDMA_RX_TIMER), 0xffffffff);
    val = qtest_readl(qts, sdma(0, SDMA_RX_TIMER));
    g_assert_cmphex(val & 0xfff, ==, 0xfff);
    g_assert_cmphex(val & ~0x0fff0fffu, ==, 0);

    qtest_quit(qts);
}

/*
 * Views, not storage: a second-copy model passes a write-then-read of the
 * page alone, so both directions are crossed.
 */
static void test_the_alias_pages_are_views_of_the_main_block(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    unsigned int port;

    sn0_ioc3_attach(qts);

    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);

    for (port = 0; port < 2; port++) {
        /* Written through the alias, visible in the main block. */
        qtest_writel(qts, sdma_alias(port, SDMA_TX_PRODUCE), 0x18);
        g_assert_cmphex(qtest_readl(qts, sdma(port, SDMA_TX_PRODUCE)),
                        ==, 0x18);

        /* Written in the main block, visible through the alias. */
        qtest_writel(qts, sdma(port, SDMA_RX_CONSUME), 0x28);
        g_assert_cmphex(qtest_readl(qts, sdma_alias(port, SDMA_RX_CONSUME)),
                        ==, 0x28);

        /* And the control register, which is neither an index nor read-only. */
        qtest_writel(qts, sdma_alias(port, SDMA_CONTROL),
                     CTRL_HIGH_WATER(0x1ff));
        g_assert_cmphex(qtest_readl(qts, sdma(port, SDMA_CONTROL)) & 0x1ff,
                        ==, 0x1ff);
    }

    /* The two pages are two ports, not one page answering twice. */
    qtest_writel(qts, sdma_alias(0, SDMA_TX_PRODUCE), 0x30);
    qtest_writel(qts, sdma_alias(1, SDMA_TX_PRODUCE), 0x38);
    g_assert_cmphex(qtest_readl(qts, sdma_alias(0, SDMA_TX_PRODUCE)), ==, 0x30);
    g_assert_cmphex(qtest_readl(qts, sdma_alias(1, SDMA_TX_PRODUCE)), ==, 0x38);

    qtest_quit(qts);
}

static void test_the_alias_refuses_the_registers_hardware_owns(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    unsigned int port;

    sn0_ioc3_attach(qts);

    for (port = 0; port < 2; port++) {
        uint32_t shadow;

        qtest_writel(qts, sdma_alias(port, SDMA_TX_CONSUME), 0xffffffff);
        g_assert_cmphex(qtest_readl(qts, sdma(port, SDMA_TX_CONSUME)), ==, 0);
        qtest_writel(qts, sdma_alias(port, SDMA_RX_PRODUCE), 0xffffffff);
        g_assert_cmphex(qtest_readl(qts, sdma(port, SDMA_RX_PRODUCE)), ==, 0);

        /* Read-only but live: the write must leave what it reads untouched. */
        shadow = qtest_readl(qts, sdma(port, SDMA_SHADOW));
        qtest_writel(qts, sdma_alias(port, SDMA_SHADOW), 0xffffffff);
        g_assert_cmphex(qtest_readl(qts, sdma(port, SDMA_SHADOW)), ==, shadow);
    }

    qtest_quit(qts);
}

/*
 * The DMA console driver reads carrier from the shadow; a shadow of zero
 * cached a carrier-down at console open and hung the console, and the
 * firmware's loopback diagnostic never reads carrier -- only this does.
 */
static void test_the_shadow_mirrors_the_port(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint32_t shadow;
    unsigned int port;

    sn0_ioc3_attach(qts);

    for (port = 0; port < 2; port++) {
        /* The port's reset modem and line status, reported per port. */
        shadow = qtest_readl(qts, sdma(port, SDMA_SHADOW));
        g_assert_cmphex(shadow & SHADOW_CD, ==, SHADOW_CD);
        g_assert_cmphex(shadow & SHADOW_CTS, ==, SHADOW_CTS);
        g_assert_cmphex(shadow & SHADOW_THRE, ==, SHADOW_THRE);
        g_assert_cmphex(shadow & SHADOW_DR, ==, 0);

        /* The driven modem-control levels appear in the shadow too. */
        qtest_writeb(qts, serial_port(port) + REG_MCR, MCR_DTR | MCR_RTS);
        shadow = qtest_readl(qts, sdma(port, SDMA_SHADOW));
        g_assert_cmphex(shadow & SHADOW_DTR, ==, SHADOW_DTR);
        g_assert_cmphex(shadow & SHADOW_RTS, ==, SHADOW_RTS);
    }

    qtest_quit(qts);
}

/*
 * The characters must reach the line: an engine that only moved indices
 * answers every register read correctly and sends nothing.
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

    qts = qtest_init_with_serial("-M origin200", &sock);
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
 * Separate sources a driver may enable independently: one raised for both
 * conditions never prompts the other watcher.
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

    qts = qtest_init_with_serial("-M origin200", &sock);
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
 * Completion only when asked for: raised unconditionally, a driver takes an
 * interrupt per entry on a queue it meant to complete once.
 */
static void test_an_entry_that_asked_for_nothing_gets_nothing(const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t out[4] = { 'q', 'u', 'i', 't' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };

    qts = qtest_init_with_serial("-M origin200", &sock);
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
 * Both guests run the console with every one of these registers at zero; an
 * engine running unenabled takes characters out of memory nobody put there.
 */
static void test_a_disabled_engine_walks_nothing(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
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
 * No ring address reads back anywhere, so each ring is pinned by where a
 * character lands: loopback returns a sent character to the same port's
 * receive ring, read out of guest RAM at the fixed-order address.
 */
static void ring_block_one_port(unsigned int port)
{
    QTestState *qts = qtest_init("-M origin200");
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
 * Port B's backend is serial_hd(1) (hw/misc/sgi-hub.h). serial_hd(0) is
 * left empty, so the byte arriving proves the second port's chardev.
 */
static void test_port_b_transmits_through_serial_hd_1(const void *data)
{
    uint16_t port;
    int lfd = portb_listen(&port);
    g_autofree char *args = g_strdup_printf(
        "-M origin200 -serial none "
        "-chardev socket,id=portb,host=127.0.0.1,port=%u,reconnect-ms=10000 "
        "-serial chardev:portb", port);
    QTestState *qts = qtest_init(args);
    int fd = portb_accept(lfd);
    const uint8_t out[4] = { 'p', 'o', 'r', 't' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };
    char got[4];

    sn0_ioc3_attach(qts);
    configure_line(qts, 1);
    enable_dma(qts, 1);

    write_transmit_entry(qts, RING_TX(1), 0, out, control);
    qtest_writel(qts, sdma(1, SDMA_TX_PRODUCE), 8);

    g_assert_cmpint(read(fd, got, sizeof(got)), ==, (int)sizeof(got));
    g_assert_cmpint(memcmp(got, out, sizeof(out)), ==, 0);

    close(fd);
    close(lfd);
    qtest_quit(qts);
}

/*
 * The size-select bit must never reach an address: this fails on a model
 * or-ing the whole base register into its arithmetic.
 */
static void test_the_size_bit_is_not_part_of_the_address(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    const uint8_t out[4] = { 'b', 'a', 's', 'e' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };
    uint8_t landed[4];

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    set_loopback(qts, 0);

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
 * The pause must be acknowledged in the register, or a driver waits forever
 * on a bit that never appears.
 */
static void test_a_pause_is_acknowledged_and_stops_the_walk(const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t out[4] = { 's', 't', 'o', 'p' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };

    qts = qtest_init_with_serial("-M origin200", &sock);
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
 * Reset is an action, not a setting: stored, it holds the engine in reset
 * forever, since every driver writes it once and moves on.
 */
static void test_a_reset_returns_the_channels_to_the_start(const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t out[4] = { 'r', 's', 't', '!' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };

    qts = qtest_init_with_serial("-M origin200", &sock);
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
 * A modem-typed byte reaches the modem control register, not the wire --
 * mistyped, it is a stray console character and a line that never moves.
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

    qts = qtest_init_with_serial("-M origin200", &sock);
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
 * The empty type ends the entry; sending all four slots regardless emits
 * memory garbage after every short write.
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

    qts = qtest_init_with_serial("-M origin200", &sock);
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
 * The pointer wraps at the ring's extent; adding eight without wrapping
 * walks silently into another port's ring.
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

    qts = qtest_init_with_serial("-M origin200", &sock);
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
 * The DMA sources join the existing status/enable contract rather than
 * raising a line of their own.
 */
static void test_a_serial_dma_source_follows_the_enable_mask(const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t out[4] = { 'i', 'r', 'q', '?' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_EMPTY, TX_EMPTY, TX_EMPTY,
    };

    qts = qtest_init_with_serial("-M origin200", &sock);
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

/*
 * IRIX's ring-only console: DMA on, timer armed, only ring sources enabled,
 * the 16550's interrupt enables never written. An arriving character must
 * still reach the ring -- the register state read off a stalled machine,
 * replayed.
 */
static void test_a_ring_only_guest_receives_arriving_characters(
    const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t sent[4] = { 'i', 'r', 'i', 'x' };
    uint8_t got[4], status[4];
    unsigned int i;

    qts = qtest_init_with_serial("-M origin200", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);

    qtest_writel(qts, INT_ENABLE_SET, IRIX_RING_ENABLE_MASK);
    qtest_writel(qts, SDMA_RING_HI, 0);
    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);
    qtest_writel(qts, sdma(0, SDMA_RX_TIMER), 0x140);
    qtest_writel(qts, sdma(0, SDMA_RX_CONSUME), CONSUME_ARM_TIMER);
    /* The miniroot's control value: DMA on, high water four. */
    qtest_writel(qts, sdma(0, SDMA_CONTROL), 0x10100004);

    g_assert_cmpint(write(sock, sent, sizeof(sent)), ==, (int)sizeof(sent));
    collect_rx_chars(qts, 0, sizeof(sent), got, status);

    for (i = 0; i < sizeof(sent); i++) {
        g_assert_cmphex(got[i], ==, sent[i]);
        g_assert_cmphex(status[i] & RX_ERRORS, ==, 0);
        g_assert_cmphex(status[i] & RX_MODEM_VALID, ==, RX_MODEM_VALID);
    }
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_RX_PRODUCE)) & 0xff8, >=, 8);

    /* The pass-through bit stayed clear: delivery came through the engine. */
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_PASSTHROUGH),
                    ==, 0);

    close(sock);
    qtest_quit(qts);
}

/*
 * The timer raises on expiry, not delivery. Under qtest the clock moves
 * only when stepped, so the gap is observable: clear after delivery, full
 * count, then raised and run out after the step.
 */
static void test_the_receive_timer_raises_on_expiry(
    const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t sent = 'y';
    uint8_t got, status;
    uint32_t val;

    qts = qtest_init_with_serial("-M origin200", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);

    qtest_writel(qts, SDMA_RING_HI, 0);
    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);
    qtest_writel(qts, sdma(0, SDMA_RX_TIMER), 0x140);
    qtest_writel(qts, sdma(0, SDMA_RX_CONSUME), CONSUME_ARM_TIMER);
    qtest_writel(qts, sdma(0, SDMA_CONTROL), CTRL_DMA_ENABLE);

    g_assert_cmpint(write(sock, &sent, 1), ==, 1);
    collect_rx_chars(qts, 0, 1, &got, &status);
    g_assert_cmphex(got, ==, sent);

    /* Delivered, but the idle time has not passed: no interrupt yet. */
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_RX_TIMER),
                    ==, 0);

    /* The count field reads the ticks left; none have elapsed yet. */
    val = qtest_readl(qts, sdma(0, SDMA_RX_TIMER));
    g_assert_cmphex(val & 0xfff, ==, 0x140);
    g_assert_cmphex((val >> 16) & 0xfff, ==, 0x140);

    qtest_clock_step(qts, 0x140 * RX_TIMER_TICK_NS + RX_TIMER_TICK_NS);

    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_RX_TIMER),
                    ==, SOURCE(0, SRC_RX_TIMER));
    g_assert_cmphex((qtest_readl(qts, sdma(0, SDMA_RX_TIMER)) >> 16) & 0xfff,
                    ==, 0);

    /* Latched: acknowledging the source clears it. */
    qtest_writel(qts, INT_STATUS, SOURCE(0, SRC_RX_TIMER));
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_RX_TIMER),
                    ==, 0);

    close(sock);
    qtest_quit(qts);
}

/*
 * Waiting characters count as published entries times four; the timer stays
 * unarmed, so anything raised came from the mark.
 */
static void test_crossing_the_high_water_mark_raises_its_source(
    const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t first = 'a';
    const uint8_t rest[4] = { 'b', 'c', 'd', 'e' };
    uint8_t got[5], status[5];

    qts = qtest_init_with_serial("-M origin200", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);

    qtest_writel(qts, SDMA_RING_HI, 0);
    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);
    qtest_writel(qts, sdma(0, SDMA_RX_CONSUME), 0);
    qtest_writel(qts, sdma(0, SDMA_CONTROL),
                 CTRL_DMA_ENABLE | CTRL_HIGH_WATER(8));

    /* One character is one entry: four waiting, below a mark of eight. */
    g_assert_cmpint(write(sock, &first, 1), ==, 1);
    collect_rx_chars(qts, 0, 1, got, status);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_RX_HIGH_WATER),
                    ==, 0);

    /* Four more push the waiting count to the mark or past it. */
    g_assert_cmpint(write(sock, rest, sizeof(rest)), ==, (int)sizeof(rest));
    collect_rx_chars(qts, 0, 5, got, status);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_RX_HIGH_WATER),
                    ==, SOURCE(0, SRC_RX_HIGH_WATER));

    /* The timer was never armed, so its source never joined in. */
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_RX_TIMER),
                    ==, 0);

    close(sock);
    qtest_quit(qts);
}

/*
 * A level, not a latch: IRIX never acknowledges it, its service loop reads
 * until the level drops -- a latched raise leaves that loop spinning.
 * Consuming below the mark must clear it unaided.
 */
static void test_consuming_below_the_high_water_mark_drops_it(const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t sent[8] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
    uint8_t got[8], status[8];
    uint32_t produce;

    qts = qtest_init_with_serial("-M origin200", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);

    qtest_writel(qts, SDMA_RING_HI, 0);
    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);
    qtest_writel(qts, sdma(0, SDMA_RX_CONSUME), 0);
    qtest_writel(qts, sdma(0, SDMA_CONTROL),
                 CTRL_DMA_ENABLE | CTRL_HIGH_WATER(8));

    /* Eight characters reach the mark of eight however the host chunks. */
    g_assert_cmpint(write(sock, sent, sizeof(sent)), ==, (int)sizeof(sent));
    collect_rx_chars(qts, 0, sizeof(sent), got, status);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_RX_HIGH_WATER),
                    ==, SOURCE(0, SRC_RX_HIGH_WATER));

    /* One entry left is four waiting or fewer: below the mark, unaided. */
    produce = qtest_readl(qts, sdma(0, SDMA_RX_PRODUCE)) & 0xff8;
    qtest_writel(qts, sdma(0, SDMA_RX_CONSUME), produce - 8);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_RX_HIGH_WATER),
                    ==, 0);

    close(sock);
    qtest_quit(qts);
}

/*
 * Flow control, not loss: the engine refuses the colliding entry, holds
 * characters in packer and FIFO, and delivers all of them in order once
 * software makes room. Consume starts two entries in to make full cheap.
 */
static void test_a_full_ring_holds_characters(
    const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t sent[12] = "flowcontrol!";
    const uint8_t sentinel[8] = { 0xee, 0xee, 0xee, 0xee,
                                  0xee, 0xee, 0xee, 0xee };
    uint8_t landed[8];
    uint8_t got[12], status[12];
    unsigned int i;

    qts = qtest_init_with_serial("-M origin200", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);

    qtest_writel(qts, SDMA_RING_HI, 0);
    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);
    qtest_writel(qts, sdma(0, SDMA_RX_CONSUME), 16);
    qtest_writel(qts, sdma(0, SDMA_CONTROL), CTRL_DMA_ENABLE);

    /* The entry the engine must refuse to fill, marked so a write shows. */
    qtest_memwrite(qts, RING_RX(0) + 8, sentinel, sizeof(sentinel));

    g_assert_cmpint(write(sock, sent, sizeof(sent)), ==, (int)sizeof(sent));
    wait_for_source(qts, SOURCE(0, SRC_RX_RING_FULL));

    /* One entry fit; the next would collide with the consume index. */
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_RX_PRODUCE)) & 0xff8, ==, 8);
    qtest_memread(qts, RING_RX(0) + 8, landed, sizeof(landed));
    g_assert_cmpint(memcmp(landed, sentinel, sizeof(sentinel)), ==, 0);

    /* Chunking is the host's, worst case one character per entry. */
    qtest_writel(qts, sdma(0, SDMA_RX_CONSUME), 128);
    collect_rx_chars(qts, 0, sizeof(sent), got, status);
    for (i = 0; i < sizeof(sent); i++) {
        g_assert_cmphex(got[i], ==, sent[i]);
        g_assert_cmphex(status[i] & RX_ERRORS, ==, 0);
    }

    close(sock);
    qtest_quit(qts);
}

/*
 * OpenBSD's arrangement: rings off, the 16550's own interrupt on. The
 * engine must not steal characters the guest reads itself.
 */
static void test_pass_through_receives_while_the_rings_stay_off(
    const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t sent = 'p';

    qts = qtest_init_with_serial("-M origin200", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    qtest_writeb(qts, serial_port(0) + REG_IER, IER_RDI);

    g_assert_cmpint(write(sock, &sent, 1), ==, 1);

    g_test_timer_start();
    while (!(qtest_readb(qts, serial_port(0) + REG_LSR) & LSR_DR)) {
        g_assert(g_test_timer_elapsed() < IOC3_SERIAL_SOCKET_TIMEOUT_SECS);
        g_usleep(1000);
    }

    /* The level source follows the port, and the byte is the guest's. */
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_PASSTHROUGH),
                    ==, SOURCE(0, SRC_PASSTHROUGH));
    g_assert_cmphex(qtest_readb(qts, serial_port(0) + REG_RBR), ==, sent);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_PASSTHROUGH),
                    ==, 0);

    /* The engine, disabled, took nothing. */
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_RX_PRODUCE)), ==, 0);

    close(sock);
    qtest_quit(qts);
}

/*
 * The drain is a command that reads back clear once done: IRIX polls it to
 * zero, and against a stored bit tore the console down forever. The other
 * half is that a waiting character ends up in the ring.
 */
static void test_the_receive_drain_acts_and_reads_back_clear(const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t sent = 'd';
    uint8_t got, status;
    uint32_t control;

    qts = qtest_init_with_serial("-M origin200", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    qtest_writel(qts, SDMA_RING_HI, 0);
    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);

    /* A character arrives while ring DMA is still off: it waits in the port. */
    g_assert_cmpint(write(sock, &sent, 1), ==, 1);
    g_test_timer_start();
    while (!(qtest_readb(qts, serial_port(0) + REG_LSR) & LSR_DR)) {
        g_assert(g_test_timer_elapsed() < IOC3_SERIAL_SOCKET_TIMEOUT_SECS);
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_RX_PRODUCE)), ==, 0);

    /* The reconfiguration write: enable plus drain, in one store. */
    qtest_writel(qts, sdma(0, SDMA_CONTROL),
                 CTRL_DMA_ENABLE | CTRL_RX_DRAIN);
    control = qtest_readl(qts, sdma(0, SDMA_CONTROL));
    g_assert_cmphex(control & CTRL_RX_DRAIN, ==, 0);
    g_assert_cmphex(control & CTRL_DMA_ENABLE, ==, CTRL_DMA_ENABLE);

    /* And the waiting character was drained into the ring. */
    collect_rx_chars(qts, 0, 1, &got, &status);
    g_assert_cmphex(got, ==, sent);
    g_assert_cmphex(status & RX_DATA_VALID, ==, RX_DATA_VALID);

    close(sock);
    qtest_quit(qts);
}

/*
 * The consume index a reset zeroes is the timer's arming, so the reset must
 * stop the timer or it expires into a ring the reset just emptied.
 */
static void test_a_channel_reset_stops_the_receive_timer(const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t sent = 'z';
    uint8_t got, status;

    qts = qtest_init_with_serial("-M origin200", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);

    qtest_writel(qts, SDMA_RING_HI, 0);
    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);
    qtest_writel(qts, sdma(0, SDMA_RX_TIMER), 0x140);
    qtest_writel(qts, sdma(0, SDMA_RX_CONSUME), CONSUME_ARM_TIMER);
    qtest_writel(qts, sdma(0, SDMA_CONTROL), CTRL_DMA_ENABLE);

    /* A delivery winds the timer, so it is running with a count. */
    g_assert_cmpint(write(sock, &sent, 1), ==, 1);
    collect_rx_chars(qts, 0, 1, &got, &status);
    g_assert_cmphex((qtest_readl(qts, sdma(0, SDMA_RX_TIMER)) >> 16) & 0xfff,
                    ==, 0x140);

    /* The channel reset zeroes the indices, which disarms and stops it. */
    qtest_writel(qts, sdma(0, SDMA_CONTROL), CTRL_DMA_ENABLE | CTRL_RESET);
    g_assert_cmphex((qtest_readl(qts, sdma(0, SDMA_RX_TIMER)) >> 16) & 0xfff,
                    ==, 0);

    /* And it never expires: stepping past the old deadline raises nothing. */
    qtest_clock_step(qts, (0x140 + 1) * RX_TIMER_TICK_NS);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_RX_TIMER),
                    ==, 0);

    close(sock);
    qtest_quit(qts);
}

/*
 * Unfilled slots carry no validity flag. Loopback cannot chunk, so a full
 * entry then a single character pins it deterministically.
 */
static void test_an_unfilled_slot_carries_no_validity_flag(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    const uint8_t full[4] = { 'f', 'u', 'l', 'l' };
    const uint8_t full_ctrl[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };
    const uint8_t one[4] = { 'x', 0, 0, 0 };
    const uint8_t one_ctrl[4] = { TX_CHARACTER, TX_EMPTY, TX_EMPTY, TX_EMPTY };
    uint8_t landed[8];
    unsigned int i;

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    set_loopback(qts, 0);
    enable_dma(qts, 0);

    /* A full four-character entry, so the packer has held four valid slots. */
    write_transmit_entry(qts, RING_TX(0), 0, full, full_ctrl);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 8);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_RX_PRODUCE)) & 0xff8, ==, 8);

    /* Then a single character. Its entry is the ring's second. */
    write_transmit_entry(qts, RING_TX(0), 8, one, one_ctrl);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 16);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_RX_PRODUCE)) & 0xff8, ==, 16);

    qtest_memread(qts, RING_RX(0) + 8, landed, sizeof(landed));
    g_assert_cmphex(landed[0], ==, 'x');
    g_assert_cmphex(landed[4] & RX_DATA_VALID, ==, RX_DATA_VALID);
    for (i = 1; i < 4; i++) {
        g_assert_cmphex(landed[4 + i] & (RX_DATA_VALID | RX_MODEM_VALID), ==, 0);
    }

    qtest_quit(qts);
}

/*
 * A one-entry ring: six loopback characters post one entry and leave two
 * held in the packer, reported in bits 26:24 of the produce index.
 */
static void hold_two_in_the_packer(QTestState *qts, uint32_t consume)
{
    const uint8_t four[4] = { 'a', 'b', 'c', 'd' };
    const uint8_t four_ctrl[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };
    const uint8_t two[4] = { 'e', 'f', 0, 0 };
    const uint8_t two_ctrl[4] = { TX_CHARACTER, TX_CHARACTER, TX_EMPTY, TX_EMPTY };

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    set_loopback(qts, 0);
    qtest_writel(qts, SDMA_RING_HI, 0);
    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);
    qtest_writel(qts, sdma(0, SDMA_RX_TIMER), 0x140);
    qtest_writel(qts, sdma(0, SDMA_RX_CONSUME), consume);
    qtest_writel(qts, sdma(0, SDMA_CONTROL), CTRL_DMA_ENABLE);

    write_transmit_entry(qts, RING_TX(0), 0, four, four_ctrl);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 8);
    write_transmit_entry(qts, RING_TX(0), 8, two, two_ctrl);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 16);

    g_assert_cmphex((qtest_readl(qts, sdma(0, SDMA_RX_PRODUCE)) >> 24) & 0x7,
                    ==, 2);
}

/*
 * Reset clears the packer too, or the enable's own run flushes a stranded
 * entry the guest never sent.
 */
static void test_a_channel_reset_clears_the_held_packer(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    hold_two_in_the_packer(qts, 16);

    qtest_writel(qts, sdma(0, SDMA_CONTROL), CTRL_DMA_ENABLE | CTRL_RESET);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_RX_PRODUCE)), ==, 0);

    qtest_quit(qts);
}

/*
 * The system-reset hold phase clears the packer and stops the timer, not
 * only the register block.
 */
static void test_a_system_reset_clears_the_held_packer_and_the_timer(
    const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    hold_two_in_the_packer(qts, 16 | CONSUME_ARM_TIMER);

    /* The posted entry wound the armed timer, so it is counting. */
    g_assert_cmphex((qtest_readl(qts, sdma(0, SDMA_RX_TIMER)) >> 16) & 0xfff,
                    ==, 0x140);

    qtest_system_reset(qts);
    sn0_ioc3_attach(qts);

    g_assert_cmphex((qtest_readl(qts, sdma(0, SDMA_RX_PRODUCE)) >> 24) & 0x7,
                    ==, 0);
    g_assert_cmphex((qtest_readl(qts, sdma(0, SDMA_RX_TIMER)) >> 16) & 0xfff,
                    ==, 0);

    qtest_quit(qts);
}

/* Past the 64 MB default population: the fabric access fails. */
#define UNBACKED_RING_BASE 0x07000000u

/*
 * A failed entry fetch raises the port's PCI error source and leaves the
 * consume index put -- not a walk over stack garbage.
 */
static void test_an_unbacked_transmit_ring_raises_the_pci_error(
    const void *data)
{
    QTestState *qts;
    int sock;

    qts = qtest_init_with_serial("-M origin200", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);

    qtest_writel(qts, SDMA_RING_HI, 0);
    qtest_writel(qts, SDMA_RING_LO, UNBACKED_RING_BASE | RING_4K);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 0);
    qtest_writel(qts, sdma(0, SDMA_RX_CONSUME), 0);
    qtest_writel(qts, sdma(0, SDMA_CONTROL), CTRL_DMA_ENABLE);

    /* Publishing an entry starts the walk; its fetch has nothing to read. */
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 8);

    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_PCI_ERROR),
                    ==, SOURCE(0, SRC_PCI_ERROR));
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_CONSUME)) & 0xff8,
                    ==, 0);

    close(sock);
    qtest_quit(qts);
}

/*
 * The failed ring write must not advance produce: an index past an
 * unwritten entry points the driver at garbage.
 */
static void test_an_unbacked_receive_ring_raises_the_pci_error(
    const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t sent[4] = { 'x', 'x', 'x', 'x' };

    qts = qtest_init_with_serial("-M origin200", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);

    qtest_writel(qts, SDMA_RING_HI, 0);
    qtest_writel(qts, SDMA_RING_LO, UNBACKED_RING_BASE | RING_4K);
    qtest_writel(qts, sdma(0, SDMA_RX_TIMER), 0x140);
    qtest_writel(qts, sdma(0, SDMA_RX_CONSUME), CONSUME_ARM_TIMER);
    /* High water of four drains the packer without the clock moving. */
    qtest_writel(qts, sdma(0, SDMA_CONTROL), 0x10100004);

    g_assert_cmpint(write(sock, sent, sizeof(sent)), ==, (int)sizeof(sent));
    wait_for_source(qts, SOURCE(0, SRC_PCI_ERROR));

    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_RX_PRODUCE)) & 0xff8,
                    ==, 0);

    close(sock);
    qtest_quit(qts);
}

/*
 * The two-tower profile: two nodes, so the slave module's base I/O controller
 * exists and its IOC3 ports A and B take -serial lines 4 and 5. Same topology
 * as the dual-node model test.
 */
#define DUAL_IO_ARGS \
    "-M origin200 " \
    "-m 128M " \
    "-object memory-backend-ram,id=ram0,size=64M " \
    "-object memory-backend-ram,id=ram1,size=64M " \
    "-numa node,nodeid=0,cpus=0-1,memdev=ram0 " \
    "-numa node,nodeid=1,cpus=2-3,memdev=ram1 " \
    "-smp 4,sockets=2,cores=2,threads=1"

/*
 * Node 1's base I/O controller must be backed by its own serial line. A
 * slave module drops its POD console onto its own IOC3 port A, which sits on
 * -serial line 4 under the node-major stride-4 scheme (line 0 is node 0's).
 * When only node 0's controller was backed, node 1's port had no chardev and
 * this console was dark. The host sends a byte down line 4 and it must reach
 * node 1's port A: the plain 16550 pass-through path, rings left off, so this
 * proves the backend alone. serial_hd(0..3) are left empty, so a byte
 * arriving at node 1's port cannot have come from any node-0 line.
 */
static void test_node1_ioc3_a_receives_through_serial_hd_4(const void *data)
{
    uint16_t port;
    int lfd = portb_listen(&port);
    g_autofree char *args = g_strdup_printf(
        DUAL_IO_ARGS
        " -serial none -serial none -serial none -serial none "
        "-chardev socket,id=n1a,host=127.0.0.1,port=%u,reconnect-ms=10000 "
        "-serial chardev:n1a", port);
    QTestState *qts = qtest_init(args);
    int fd = portb_accept(lfd);
    const uint64_t node1 = SN0_NODE_STRIDE;
    const uint64_t serial_a = node1 + SERIAL_A;
    const uint8_t sent = 'N';

    /* Bring up node 1's controller and its port A, ring DMA left off. */
    sn0_ioc3_attach_node(qts, node1);
    qtest_writeb(qts, serial_a + REG_LCR, 0x03);
    qtest_writeb(qts, serial_a + REG_FCR, 0x01);
    qtest_writeb(qts, serial_a + REG_IER, IER_RDI);

    g_assert_cmpint(write(fd, &sent, 1), ==, 1);

    g_test_timer_start();
    while (!(qtest_readb(qts, serial_a + REG_LSR) & LSR_DR)) {
        g_assert(g_test_timer_elapsed() < IOC3_SERIAL_SOCKET_TIMEOUT_SECS);
        g_usleep(1000);
    }

    /* The pass-through source follows the port, and the byte is node 1's. */
    g_assert_cmphex(qtest_readl(qts, node1 + INT_STATUS) &
                    SOURCE(0, SRC_PASSTHROUGH), ==, SOURCE(0, SRC_PASSTHROUGH));
    g_assert_cmphex(qtest_readb(qts, serial_a + REG_RBR), ==, sent);

    close(fd);
    close(lfd);
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
    qtest_add_data_func("sn0/ioc3-serial/alias-pages-are-views", NULL,
                        test_the_alias_pages_are_views_of_the_main_block);
    qtest_add_data_func("sn0/ioc3-serial/alias-refuses-hardware-registers",
                        NULL,
                        test_the_alias_refuses_the_registers_hardware_owns);
    qtest_add_data_func("sn0/ioc3-serial/shadow-mirrors-the-port", NULL,
                        test_the_shadow_mirrors_the_port);
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
    qtest_add_data_func("sn0/ioc3-serial/port-b-transmits-through-serial-hd-1",
                        NULL, test_port_b_transmits_through_serial_hd_1);
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
    qtest_add_data_func("sn0/ioc3-serial/ring-only-guest-receives", NULL,
                        test_a_ring_only_guest_receives_arriving_characters);
    qtest_add_data_func("sn0/ioc3-serial/receive-timer-counts-and-raises",
                        NULL,
                        test_the_receive_timer_raises_on_expiry);
    qtest_add_data_func("sn0/ioc3-serial/high-water-mark-raises", NULL,
                        test_crossing_the_high_water_mark_raises_its_source);
    qtest_add_data_func("sn0/ioc3-serial/high-water-mark-drops-when-consumed",
                        NULL,
                        test_consuming_below_the_high_water_mark_drops_it);
    qtest_add_data_func("sn0/ioc3-serial/full-ring-holds-characters", NULL,
                        test_a_full_ring_holds_characters);
    qtest_add_data_func("sn0/ioc3-serial/pass-through-still-receives", NULL,
                        test_pass_through_receives_while_the_rings_stay_off);
    qtest_add_data_func("sn0/ioc3-serial/receive-drain-reads-back-clear", NULL,
                        test_the_receive_drain_acts_and_reads_back_clear);
    qtest_add_data_func("sn0/ioc3-serial/channel-reset-stops-receive-timer",
                        NULL,
                        test_a_channel_reset_stops_the_receive_timer);
    qtest_add_data_func("sn0/ioc3-serial/unfilled-slot-has-no-validity-flag",
                        NULL,
                        test_an_unfilled_slot_carries_no_validity_flag);
    qtest_add_data_func("sn0/ioc3-serial/channel-reset-clears-held-packer",
                        NULL,
                        test_a_channel_reset_clears_the_held_packer);
    qtest_add_data_func("sn0/ioc3-serial/system-reset-clears-held-packer",
                        NULL,
                        test_a_system_reset_clears_the_held_packer_and_the_timer);
    qtest_add_data_func("sn0/ioc3-serial/unbacked-transmit-ring-raises-error",
                        NULL,
                        test_an_unbacked_transmit_ring_raises_the_pci_error);
    qtest_add_data_func("sn0/ioc3-serial/unbacked-receive-ring-raises-error",
                        NULL,
                        test_an_unbacked_receive_ring_raises_the_pci_error);
    qtest_add_data_func("sn0/ioc3-serial/node1-port-a-receives-on-serial-hd-4",
                        NULL,
                        test_node1_ioc3_a_receives_through_serial_hd_4);

    return g_test_run();
}
