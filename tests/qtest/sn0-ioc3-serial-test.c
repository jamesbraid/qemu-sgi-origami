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
#include "qobject/qdict.h"
#include "sn0-user-image.h"
#include "sgi-migrate.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "sgi-test.h"

/* Crosstalk space, and the chip's window inside the console widget. */
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

#define PCI_IOC3_DEVICE  2
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

/*
 * One plain origin200 serves every case that wants no host socket wired
 * to a port, started in main(). A case that writes bytes into a chardev
 * needs qtest_init_with_serial or a named chardev, so it keeps its own.
 */
static QTestState *qts;

/*
 * Open such a case. Reset turns the controller's memory decode back off,
 * which unmaps its window, so every case runs the attach sequence again
 * exactly as it did after a boot.
 */
static void serial_reset(void)
{
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, CFG0(PCI_IOC3_DEVICE) + PCI_COMMAND) &
                    PCI_COMMAND_MEMORY, ==, 0);
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
#define CTRL_FLOW_CONTROL (1u << 17)
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
#define REG_MSR         6
#define REG_SCR         7

#define LCR_DLAB        0x80
#define LCR_8N1         0x03

#define IER_RDI         0x01
#define LSR_DR          0x01
#define LSR_THRE        0x20
#define LSR_TEMT        0x40

#define MCR_DTR         0x01
#define MCR_RTS         0x02
#define MCR_LOOPBACK    0x10
#define MCR_AUTOFLOW    0x20

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

/* Fill the backend's output queue before QEMU inherits this socket end. */
static size_t fill_socket_send_queue(int fd)
{
    uint8_t filler[512];
    size_t filled = 0;
    int sndbuf = 1024;
    int flags;

    memset(filler, 0xa5, sizeof(filler));
    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                              &sndbuf, sizeof(sndbuf)), ==, 0);
    flags = fcntl(fd, F_GETFL);
    g_assert_cmpint(flags, >=, 0);
    g_assert_cmpint(fcntl(fd, F_SETFL, flags | O_NONBLOCK), ==, 0);

    for (;;) {
        ssize_t n = write(fd, filler, sizeof(filler));

        if (n < 0) {
            g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
            break;
        }
        g_assert_cmpint(n, >, 0);
        filled += n;
    }

    return filled;
}

/*
 * fill_socket_send_queue gives up after the first 512-byte chunk that does
 * not fit, which can leave up to 511 bytes of real room behind: enough for
 * a one-byte transmit to slip through and never hit backpressure at all.
 * Top it off one byte at a time so a single subsequent write is guaranteed
 * to see EAGAIN.
 */
static size_t fill_socket_send_queue_exact(int fd)
{
    size_t filled = fill_socket_send_queue(fd);
    uint8_t byte = 0xa5;

    for (;;) {
        ssize_t n = write(fd, &byte, 1);

        if (n < 0) {
            g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
            break;
        }
        g_assert_cmpint(n, ==, 1);
        filled += 1;
    }

    return filled;
}

static void read_exact(int fd, uint8_t *buf, size_t len)
{
    size_t got = 0;

    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        g_assert_cmpint(n, >, 0);
        got += n;
    }
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
    g_assert_cmpint(lfd, >=, 0);
    g_assert_cmpint(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)),
                    ==, 0);
    g_assert_cmpint(getsockname(lfd, (struct sockaddr *)&addr, &addrlen),
                    ==, 0);
    *port = ntohs(addr.sin_port);
    g_assert_cmpint(listen(lfd, 1), ==, 0);
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
    g_assert_cmpint(fd, >=, 0);
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
            if (g_test_timer_elapsed() >= IOC3_SERIAL_SOCKET_TIMEOUT_SECS) {
                g_test_message("port %u's ring held %u of %u characters "
                               "after %d seconds", port, got, n,
                               IOC3_SERIAL_SOCKET_TIMEOUT_SECS);
                g_assert_not_reached();
            }
            g_usleep(1000);
        }
    }
}

/*
 * Wait for a SuperIO interrupt source to appear in the status register.
 * On the wall clock, and it has to be: what raises the source is the main
 * loop reading a host socket, which stepping the virtual clock does not
 * make happen.
 */
static void wait_for_source(QTestState *qts, uint32_t bit)
{
    uint32_t seen;

    g_test_timer_start();
    while (((seen = qtest_readl(qts, INT_STATUS)) & bit) != bit) {
        if (g_test_timer_elapsed() >= IOC3_SERIAL_SOCKET_TIMEOUT_SECS) {
            g_test_message("the interrupt status held 0x%x, without 0x%x, "
                           "after %d seconds", seen, bit,
                           IOC3_SERIAL_SOCKET_TIMEOUT_SECS);
            g_assert_not_reached();
        }
        g_usleep(1000);
    }
}

/*
 * Wait for a character written into a host socket to reach a port. Same
 * reason for the wall clock as wait_for_source() above.
 */
static void wait_for_receive_ready(QTestState *qts, uint64_t port)
{
    uint8_t seen;

    g_test_timer_start();
    while (!((seen = qtest_readb(qts, port + REG_LSR)) & LSR_DR)) {
        if (g_test_timer_elapsed() >= IOC3_SERIAL_SOCKET_TIMEOUT_SECS) {
            g_test_message("the line status at 0x%" PRIx64 " held 0x%x, with "
                           "no character ready, after %d seconds", port, seen,
                           IOC3_SERIAL_SOCKET_TIMEOUT_SECS);
            g_assert_not_reached();
        }
        g_usleep(1000);
    }
}

/* No guest reads these fields back: asserted here or not at all. */
static void test_the_control_register_round_trips(const void *data)
{
    serial_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, CHIP_CONTROL, 0x00022e5e);
    g_assert_cmphex(qtest_readl(qts, CHIP_CONTROL), ==,
                    0x00022e5e | CHIP_CTRL_ARB_IDLE);
}

/*
 * The pointer's width follows the selected ring size. Every ordinary driver
 * write is already in range, so only writing bits the register does not own
 * tells a masked pointer from a stored whole word.
 */
static void test_an_index_keeps_only_its_pointer_field(const void *data)
{
    serial_reset();

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
}

/*
 * Bit 31 arms the receive timer, so this index keeps one bit more than its
 * three siblings.
 */
static void test_the_receive_consume_keeps_its_arming_bit(const void *data)
{
    serial_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);
    qtest_writel(qts, sdma(0, SDMA_RX_CONSUME), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_RX_CONSUME)), ==,
                    0x800000f8 | 0xf00);
}

/*
 * Writing the reload loads the count too, and a timer that is not running
 * holds it there. The Origin 300 IO-8 board test resets the channel, writes
 * 0xfff with the timer unarmed and requires 0x0fff0fff back under a mask of
 * 0x0fff0fff: reload and count both. A count that read zero whenever the
 * timer was idle answered 0x00000fff.
 */
static void test_a_reload_write_loads_the_idle_count(const void *data)
{
    serial_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, sdma(0, SDMA_CONTROL), CTRL_RESET);
    qtest_writel(qts, sdma(0, SDMA_CONTROL), 0);
    qtest_writel(qts, sdma(0, SDMA_RX_TIMER), 0xfff);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_RX_TIMER)) & 0x0fff0fff,
                    ==, 0x0fff0fff);

    /* Unarmed, it does not count: time passing leaves it where it was. */
    qtest_clock_step(qts, 0x10 * RX_TIMER_TICK_NS);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_RX_TIMER)) & 0x0fff0fff,
                    ==, 0x0fff0fff);

    /* A new reload replaces it. */
    qtest_writel(qts, sdma(0, SDMA_RX_TIMER), 0x140);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_RX_TIMER)) & 0x0fff0fff,
                    ==, 0x01400140);
}

/*
 * Twelve-bit reload, count read back in bits 16-27; the ethernet block's
 * timer keeps eleven bits, so a shared width is wrong for one of the two.
 */
static void test_the_receive_timer_keeps_twelve_bits(const void *data)
{
    uint32_t val;

    serial_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, sdma(0, SDMA_RX_TIMER), 0xffffffff);
    val = qtest_readl(qts, sdma(0, SDMA_RX_TIMER));
    g_assert_cmphex(val & 0xfff, ==, 0xfff);
    g_assert_cmphex(val & ~0x0fff0fffu, ==, 0);
}

/*
 * Views, not storage: a second-copy model passes a write-then-read of the
 * page alone, so both directions are crossed.
 */
static void test_the_alias_pages_are_views_of_the_main_block(const void *data)
{
    unsigned int port;

    serial_reset();

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
}

static void test_the_alias_refuses_the_registers_hardware_owns(const void *data)
{
    unsigned int port;

    serial_reset();

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
}

/*
 * Modem status takes a write into its four change indicators, and the live
 * line levels above them are untouched by it. Unlike line status, the
 * PC16550D datasheet does not describe a write to this register at all --
 * the Origin 300 board test is the whole of the evidence, and it writes 0x0f
 * and requires those four bits back. A read clears them, which is the
 * datasheet's own rule for the indicators.
 */
static void test_modem_status_takes_a_factory_test_write(const void *data)
{
    unsigned int port;

    serial_reset();

    sn0_ioc3_attach(qts);

    for (port = 0; port < 2; port++) {
        uint64_t msr = serial_port(port) + REG_MSR;
        uint8_t live;

        /* Whatever the lines carry, with no change pending. */
        live = qtest_readb(qts, msr);
        g_assert_cmphex(live & 0x0f, ==, 0);

        qtest_writeb(qts, msr, 0x0f);
        g_assert_cmphex(qtest_readb(qts, msr), ==, (live | 0x0f));

        /* That read took them away again, and left the levels alone. */
        g_assert_cmphex(qtest_readb(qts, msr), ==, live);

        /* The live levels take no write: all ones reaches four bits. */
        qtest_writeb(qts, msr, 0xff);
        g_assert_cmphex(qtest_readb(qts, msr), ==, (live | 0x0f));
        g_assert_cmphex(qtest_readb(qts, msr), ==, live);
    }
}

/*
 * Line status takes a write, which the PC16550D datasheet describes as a
 * factory-test facility and the Origin 300 board test uses: it writes 0x7f
 * and requires all seven low bits back, where the shared 16550 core this
 * chip embeds drops the write entirely. Bit 7 is not one of them.
 *
 * Reading line status is what clears the error bits it reports, so the
 * written bits last exactly one read -- see the datasheet for what that
 * models and what it does not.
 */
static void test_line_status_takes_a_factory_test_write(const void *data)
{
    unsigned int port;

    serial_reset();

    sn0_ioc3_attach(qts);

    for (port = 0; port < 2; port++) {
        uint64_t lsr = serial_port(port) + REG_LSR;

        /* Idle: the transmitter is empty and nothing has arrived. */
        g_assert_cmphex(qtest_readb(qts, lsr), ==, LSR_THRE | LSR_TEMT);

        qtest_writeb(qts, lsr, 0x7f);
        g_assert_cmphex(qtest_readb(qts, lsr), ==, 0x7f);

        /* That read took them away again. */
        g_assert_cmphex(qtest_readb(qts, lsr), ==, LSR_THRE | LSR_TEMT);

        /* Bit 7 stays clear however it is written. */
        qtest_writeb(qts, lsr, 0xff);
        g_assert_cmphex(qtest_readb(qts, lsr), ==, 0x7f);

        /* A zero write leaves the live bits alone. */
        qtest_writeb(qts, lsr, 0);
        g_assert_cmphex(qtest_readb(qts, lsr), ==, LSR_THRE | LSR_TEMT);
    }
}

/*
 * Modem control carries a sixth bit, the automatic-flow-control enable a
 * TI-style 16550 has where National's PC16550D reads a hardwired zero. The
 * Origin 300 board test writes 0x3f and requires all six back, and the
 * shared 16550 core this chip embeds masks that bit away. Bits 6 and 7 stay
 * zero: the test never writes them, so neither does this.
 *
 * Only the bit is modelled, not automatic flow control itself -- see the
 * datasheet for what that leaves out.
 */
static void test_modem_control_holds_the_autoflow_bit(const void *data)
{
    unsigned int port;

    serial_reset();

    sn0_ioc3_attach(qts);

    for (port = 0; port < 2; port++) {
        uint64_t mcr = serial_port(port) + REG_MCR;

        qtest_writeb(qts, mcr, 0x3f);
        g_assert_cmphex(qtest_readb(qts, mcr), ==, 0x3f);

        /* It clears on its own, and clearing it leaves the rest alone. */
        qtest_writeb(qts, mcr, MCR_DTR | MCR_RTS);
        g_assert_cmphex(qtest_readb(qts, mcr), ==, MCR_DTR | MCR_RTS);

        qtest_writeb(qts, mcr, MCR_AUTOFLOW);
        g_assert_cmphex(qtest_readb(qts, mcr), ==, MCR_AUTOFLOW);

        /* An all-ones write settles at the six bits, not eight. */
        qtest_writeb(qts, mcr, 0xff);
        g_assert_cmphex(qtest_readb(qts, mcr), ==, 0x3f);
    }
}

/*
 * What the alias refuses, the main block takes. The engine advances these
 * two indexes, but software can still place them, and the Origin 300 board
 * test is what says so: it writes the transmit consume index and reads it
 * back, with the produce index cleared and nothing transmitting, so no
 * engine advance could have put the value there. Only the pointer field
 * stores -- the bits above it report engine state and a write cannot forge
 * them. Both sizes, because the pointer is as wide as the ring.
 */
static void test_the_main_block_places_the_engine_indexes(const void *data)
{
    static const uint32_t ring[] = { 0, RING_4K };
    unsigned int size;
    unsigned int port;

    serial_reset();

    sn0_ioc3_attach(qts);

    for (size = 0; size < ARRAY_SIZE(ring); size++) {
        uint32_t pointer = ring[size] ? 0xff8 : 0x3f8;

        qtest_writel(qts, SDMA_RING_LO, ring[size]);

        for (port = 0; port < 2; port++) {
            qtest_writel(qts, sdma(port, SDMA_TX_CONSUME), 0xffffffff);
            g_assert_cmphex(qtest_readl(qts, sdma(port, SDMA_TX_CONSUME)),
                            ==, pointer);
            qtest_writel(qts, sdma(port, SDMA_RX_PRODUCE), 0xffffffff);
            g_assert_cmphex(qtest_readl(qts, sdma(port, SDMA_RX_PRODUCE))
                            & 0xffffff, ==, pointer);

            qtest_writel(qts, sdma(port, SDMA_TX_CONSUME), 0);
            g_assert_cmphex(qtest_readl(qts, sdma(port, SDMA_TX_CONSUME)),
                            ==, 0);
            qtest_writel(qts, sdma(port, SDMA_RX_PRODUCE), 0);
            g_assert_cmphex(qtest_readl(qts, sdma(port, SDMA_RX_PRODUCE))
                            & 0xffffff, ==, 0);
        }
    }
}

/*
 * The DMA console driver reads carrier from the shadow; a shadow of zero
 * cached a carrier-down at console open and hung the console, and the
 * firmware's loopback diagnostic never reads carrier -- only this does.
 */
static void test_the_shadow_mirrors_the_port(const void *data)
{
    uint32_t shadow;
    unsigned int port;

    serial_reset();

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
}

/*
 * Looped back with hardware flow control on, a port reports clear-to-send
 * although software left request-to-send clear. The Origin 300 IO-8 board
 * test sets exactly that -- modem control 0x11, data-terminal-ready and
 * loopback, then control 0x10120020 -- sends "abcd" and requires each
 * status byte to read 0xd0 once carrier-detect is masked away. Without the
 * flow-control bit the port is an ordinary 16550 in loopback, where
 * clear-to-send follows request-to-send, and that half pins the condition
 * rather than a port that always claims clear-to-send.
 */
static void test_loopback_flow_control_reports_clear_to_send(const void *data)
{
    static const uint8_t out[4] = { 'a', 'b', 'c', 'd' };
    static const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };
    uint8_t status[4];
    uint8_t got[4];
    unsigned int i;
    int flow;

    for (flow = 0; flow < 2; flow++) {
        uint8_t cts = flow ? RX_CTS : 0;

        serial_reset();

        sn0_ioc3_attach(qts);
        configure_line(qts, 0);
        qtest_writeb(qts, serial_port(0) + REG_MCR, MCR_LOOPBACK | MCR_DTR);
        enable_dma(qts, 0);
        qtest_writel(qts, sdma(0, SDMA_CONTROL),
                     CTRL_DMA_ENABLE | (flow ? CTRL_FLOW_CONTROL : 0));

        write_transmit_entry(qts, RING_TX(0), 0, out, control);
        qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 8);
        collect_rx_chars(qts, 0, sizeof(got), got, status);

        g_assert_cmpint(memcmp(got, out, sizeof(out)), ==, 0);
        for (i = 0; i < 4; i++) {
            g_assert_cmphex(status[i] & ~RX_CD, ==,
                            RX_DATA_VALID | RX_MODEM_VALID | cts);
        }

        /* The two other views of the same line agree with the ring. */
        g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_SHADOW)) & SHADOW_CTS,
                        ==, flow ? SHADOW_CTS : 0);
        g_assert_cmphex(qtest_readb(qts, serial_port(0) + REG_MSR) & 0x10,
                        ==, cts);

        /* Software's request-to-send is left as it wrote it. */
        g_assert_cmphex(qtest_readb(qts, serial_port(0) + REG_MCR) & MCR_RTS,
                        ==, 0);
    }
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
 * A blocked backend must leave the entry with the engine. Advancing the ring
 * while the 16550 is blocked overruns its small FIFO and loses the middle of
 * this burst.
 */
static void test_a_blocked_backend_keeps_every_transmit_character(
    const void *data)
{
    static const uint8_t out[] =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!?";
    static const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };
    static const uint8_t final_control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
        TX_CHARACTER | TX_INTERRUPT,
    };
    struct timeval tv = { .tv_sec = 2 };
    uint8_t filler[512];
    uint8_t got[sizeof(out) - 1];
    size_t remaining;
    size_t offset;
    QTestState *qts;
    int sv[2];

    g_assert_cmpuint(sizeof(got) % 4, ==, 0);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    remaining = fill_socket_send_queue(sv[1]);
    g_assert_cmpuint(remaining, >, 0);

    qts = qtest_initf(
        "-M origin200 -chardev socket,id=ioc3-a,fd=%d "
        "-serial chardev:ioc3-a", sv[1]);
    g_assert_cmpint(close(sv[1]), ==, 0);
    g_assert_cmpint(setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO,
                              &tv, sizeof(tv)), ==, 0);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    enable_dma(qts, 0);

    for (offset = 0; offset < sizeof(got); offset += 4) {
        write_transmit_entry(qts, RING_TX(0), offset * 2,
                             out + offset,
                             offset + 4 == sizeof(got) ? final_control :
                             control);
    }
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), sizeof(got) * 2);

    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_CONSUME)) & 0xff8,
                    <, sizeof(got) * 2);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) &
                    (SOURCE(0, SRC_TX_DONE) |
                     SOURCE(0, SRC_TX_RING_EMPTY)), ==, 0);

    while (remaining) {
        size_t chunk = MIN(remaining, sizeof(filler));

        read_exact(sv[0], filler, chunk);
        remaining -= chunk;
    }
    read_exact(sv[0], got, sizeof(got));
    for (offset = 0; offset < sizeof(got); offset++) {
        g_assert_cmpuint(got[offset], ==, out[offset]);
    }

    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_CONSUME)) & 0xff8,
                    ==, sizeof(got) * 2);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) &
                    (SOURCE(0, SRC_TX_DONE) |
                     SOURCE(0, SRC_TX_RING_EMPTY)), ==,
                    SOURCE(0, SRC_TX_DONE) |
                    SOURCE(0, SRC_TX_RING_EMPTY));

    close(sv[0]);
    qtest_quit(qts);
}

/*
 * A backend that is connected but never reads -- the qualification harness
 * attaches its console reader after resuming the guest (docs/notebooks or
 * tools/origami_lib/guest.py's shell_session, opened after the "cont" in
 * irix_network_test.py), so bytes can land before anyone is listening --
 * gives the byte-wide UART's transmit path no fd event to wake it up: the
 * write returns EAGAIN, a watch is armed, and nothing ever signals it.
 * Real hardware never waits on a listener either: a 16550 clocks each byte
 * out at the programmed baud rate whatever is on the other end of the wire.
 * The transmitter must still go empty within bounded time.
 */
#define XMIT_GIVE_UP_NS (2LL * 1000 * 1000 * 1000)

static void test_a_never_drained_backend_completes_the_byte_wide_write(
    const void *data)
{
    QTestState *qts;
    int sv[2];
    size_t remaining;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    remaining = fill_socket_send_queue_exact(sv[1]);
    g_assert_cmpuint(remaining, >, 0);

    qts = qtest_initf(
        "-M origin200 -chardev socket,id=ioc3-a,fd=%d "
        "-serial chardev:ioc3-a", sv[1]);
    g_assert_cmpint(close(sv[1]), ==, 0);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);

    /* sv[0] is never read: the console reader that has not attached yet. */
    qtest_writeb(qts, serial_port(0) + REG_RBR, 'x');
    g_assert_cmphex(qtest_readb(qts, serial_port(0) + REG_LSR) & LSR_TEMT,
                    ==, 0);

    qtest_clock_step(qts, XMIT_GIVE_UP_NS + 1LL * 1000 * 1000 * 1000);

    g_assert_cmphex(qtest_readb(qts, serial_port(0) + REG_LSR) & LSR_TEMT,
                    ==, LSR_TEMT);

    close(sv[0]);
    qtest_quit(qts);
}

/*
 * Once one byte has given up waiting on a backend that never drains, the
 * device treats it as a dead wire: further bytes must not each cost their
 * own XMIT_GIVE_UP_NS wait, or a run of them would still stall a polling
 * guest for minutes. They complete immediately, dropped, with no clock
 * step at all needed between them.
 */
static void test_a_dead_backend_completes_further_writes_at_once(
    const void *data)
{
    QTestState *qts;
    int sv[2];
    size_t remaining;
    unsigned int i;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    remaining = fill_socket_send_queue_exact(sv[1]);
    g_assert_cmpuint(remaining, >, 0);

    qts = qtest_initf(
        "-M origin200 -chardev socket,id=ioc3-a,fd=%d "
        "-serial chardev:ioc3-a", sv[1]);
    g_assert_cmpint(close(sv[1]), ==, 0);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);

    /* One held byte, given up on: this is what marks the backend dead. */
    qtest_writeb(qts, serial_port(0) + REG_RBR, 'x');
    g_assert_cmphex(qtest_readb(qts, serial_port(0) + REG_LSR) & LSR_TEMT,
                    ==, 0);
    qtest_clock_step(qts, XMIT_GIVE_UP_NS + 1LL * 1000 * 1000 * 1000);
    g_assert_cmphex(qtest_readb(qts, serial_port(0) + REG_LSR) & LSR_TEMT,
                    ==, LSR_TEMT);

    /*
     * No further clock step anywhere below: each write must already read
     * back empty by the time this returns, or the model is still paying
     * the full wait per byte.
     */
    for (i = 0; i < 64; i++) {
        qtest_writeb(qts, serial_port(0) + REG_RBR, 'a' + (i % 26));
        g_assert_cmphex(qtest_readb(qts, serial_port(0) + REG_LSR) & LSR_TEMT,
                        ==, LSR_TEMT);
    }

    close(sv[0]);
    qtest_quit(qts);
}

/*
 * The dead latch lifts once the backend proves it is writable again: a
 * byte written after that must reach the reader, not join the ones the
 * dead latch was dropping.
 */
static void test_a_recovered_backend_resumes_delivery(const void *data)
{
    QTestState *qts;
    int sv[2];
    size_t remaining;
    uint8_t filler[4096];
    uint8_t got;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    remaining = fill_socket_send_queue_exact(sv[1]);
    g_assert_cmpuint(remaining, >, 0);

    qts = qtest_initf(
        "-M origin200 -chardev socket,id=ioc3-a,fd=%d "
        "-serial chardev:ioc3-a", sv[1]);
    g_assert_cmpint(close(sv[1]), ==, 0);
    serial_socket_deadline(sv[0]);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);

    /* One held byte, given up on, marks the backend dead. */
    qtest_writeb(qts, serial_port(0) + REG_RBR, 'x');
    qtest_clock_step(qts, XMIT_GIVE_UP_NS + 1LL * 1000 * 1000 * 1000);
    g_assert_cmphex(qtest_readb(qts, serial_port(0) + REG_LSR) & LSR_TEMT,
                    ==, LSR_TEMT);

    /* Drain the backend: the recovery watch should see it writable again. */
    while (remaining) {
        size_t chunk = MIN(remaining, sizeof(filler));

        read_exact(sv[0], filler, chunk);
        remaining -= chunk;
    }

    /* A byte written now must actually reach the reader. */
    qtest_writeb(qts, serial_port(0) + REG_RBR, 'Z');
    read_exact(sv[0], &got, 1);
    g_assert_cmphex(got, ==, 'Z');

    close(sv[0]);
    qtest_quit(qts);
}

/*
 * The same backend, reached through the DMA ring PR #529 taught to wait for
 * the UART's transmit-ready level: a hold that never expires there is the
 * PROM base I/O diagnostic's stall, since its loopback exercise sends
 * through this path before a console reader has ever read anything.
 */
static void test_a_never_drained_backend_still_advances_the_ring(
    const void *data)
{
    static const uint8_t out[4] = { 'w', 'x', 'y', 'z' };
    static const uint8_t control[4] = {
        /*
         * The engine's tx-ready signal follows LSR.THRE, which the UART
         * sets again the moment a byte leaves the holding register for the
         * shift register -- before that byte is actually delivered. The
         * first character always slips through on that transient high, and
         * a second character sits in the holding register behind it
         * without its own retry ever starting; only a third character
         * finds tx-ready genuinely low, so this needs all four slots
         * filled.
         */
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER | TX_INTERRUPT,
    };
    QTestState *qts;
    int sv[2];
    size_t remaining;
    unsigned int i;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    remaining = fill_socket_send_queue_exact(sv[1]);
    g_assert_cmpuint(remaining, >, 0);

    qts = qtest_initf(
        "-M origin200 -chardev socket,id=ioc3-a,fd=%d "
        "-serial chardev:ioc3-a", sv[1]);
    g_assert_cmpint(close(sv[1]), ==, 0);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    enable_dma(qts, 0);

    write_transmit_entry(qts, RING_TX(0), 0, out, control);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 8);

    /* Backpressure: the walk has not consumed the entry yet. */
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_CONSUME)) & 0xff8,
                    <, 8);

    /*
     * Giving up on one held character only starts the next one's own
     * retry (it was sitting in the holding register, never yet attempted),
     * so the walk needs one give-up per character still waiting on
     * tx-ready.
     */
    for (i = 0; i < 4; i++) {
        qtest_clock_step(qts, XMIT_GIVE_UP_NS + 1LL * 1000 * 1000 * 1000);
    }

    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_CONSUME)) & 0xff8,
                    ==, 8);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_TX_DONE),
                    ==, SOURCE(0, SRC_TX_DONE));

    close(sv[0]);
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
    const uint8_t out[4] = { 'n', 'o', 'p', 'e' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
        TX_CHARACTER | TX_INTERRUPT,
    };

    serial_reset();

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);

    qtest_writel(qts, SDMA_RING_HI, 0);
    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);
    write_transmit_entry(qts, RING_TX(0), 0, out, control);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 8);

    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_CONSUME)) & 0xff8, ==, 0);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS), ==, 0);
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
    const uint8_t out[4] = { 'b', 'a', 's', 'e' };
    const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };
    uint8_t landed[4];

    serial_reset();

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
 * And it ends the entry wherever it sits, not only after the last character:
 * the slots behind an empty one are not sent, and the walk goes on with the
 * next entry. The Origin 300 IO-8 board test's DMA check hands the engine
 * an entry typed character, empty, character, character, and expects the
 * next entry's first character straight after the first slot's.
 */
static void test_an_empty_slot_ends_the_entry_mid_way(const void *data)
{
    static const uint8_t first[4] = { 'a', 'b', 'c', 'd' };
    static const uint8_t first_ctrl[4] = {
        TX_CHARACTER, TX_EMPTY, TX_CHARACTER, TX_CHARACTER,
    };
    static const uint8_t second[4] = { 'e', 'f', 'g', 'h' };
    static const uint8_t second_ctrl[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };
    static const uint8_t sent[5] = { 'a', 'e', 'f', 'g', 'h' };
    uint8_t status[5];
    uint8_t got[5];

    serial_reset();

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    set_loopback(qts, 0);
    enable_dma(qts, 0);

    write_transmit_entry(qts, RING_TX(0), 0, first, first_ctrl);
    write_transmit_entry(qts, RING_TX(0), 8, second, second_ctrl);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 16);
    collect_rx_chars(qts, 0, sizeof(got), got, status);

    g_assert_cmpmem(got, sizeof(got), sent, sizeof(sent));
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_CONSUME)) & 0xff8, ==, 16);
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

    wait_for_receive_ready(qts, serial_port(0));

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
    wait_for_receive_ready(qts, serial_port(0));
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
    const uint8_t full[4] = { 'f', 'u', 'l', 'l' };
    const uint8_t full_ctrl[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };
    const uint8_t one[4] = { 'x', 0, 0, 0 };
    const uint8_t one_ctrl[4] = { TX_CHARACTER, TX_EMPTY, TX_EMPTY, TX_EMPTY };
    uint8_t landed[8];
    unsigned int i;

    serial_reset();

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
    serial_reset();

    hold_two_in_the_packer(qts, 16);

    qtest_writel(qts, sdma(0, SDMA_CONTROL), CTRL_DMA_ENABLE | CTRL_RESET);
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_RX_PRODUCE)), ==, 0);
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
    "-M origin200,topology=origin200-dual " \
    "-m 128M " \
    "-smp 4"

/*
 * Node 1's base I/O controller must be backed by its own serial line. A
 * slave module drops its POD console onto its own IOC3 port A, which sits on
 * -serial line 4 under the node-major stride-4 scheme (line 0 is node 0's).
 * When only node 0's controller was backed, node 1's port had no chardev and
 * this console was dark. The host sends a byte down line 6, the first line of
 * node 1's block, and it must reach node 1's port A: the plain 16550
 * pass-through path, rings left off, so this proves the backend alone.
 * serial_hd(0..5) are left empty, so a byte arriving at node 1's port cannot
 * have come from any node-0 line.
 */
static void test_node1_ioc3_a_receives_through_serial_hd_6(const void *data)
{
    uint16_t port;
    int lfd = portb_listen(&port);
    g_autofree char *args = g_strdup_printf(
        DUAL_IO_ARGS
        " -serial none -serial none -serial none -serial none "
        "-serial none -serial none "
        "-chardev socket,id=n1a,host=127.0.0.1,port=%u,reconnect-ms=10000 "
        "-serial chardev:n1a", port);
    QTestState *qts = qtest_init(args);
    int fd = portb_accept(lfd);
    const uint64_t node1 = SN0_NODE_STRIDE;
    const uint64_t serial_a = node1 + SERIAL_A;
    const uint8_t sent = 'N';

    /* No PROM ran to assign node 1 its number; this Bridge needs it claimed. */
    sn0_program_nasid(qts, 1);

    /* Bring up node 1's controller and its port A, ring DMA left off. */
    sn0_ioc3_attach_node(qts, node1);
    qtest_writeb(qts, serial_a + REG_LCR, 0x03);
    qtest_writeb(qts, serial_a + REG_FCR, 0x01);
    qtest_writeb(qts, serial_a + REG_IER, IER_RDI);

    g_assert_cmpint(write(fd, &sent, 1), ==, 1);

    wait_for_receive_ready(qts, serial_a);

    /* The pass-through source follows the port, and the byte is node 1's. */
    g_assert_cmphex(qtest_readl(qts, node1 + INT_STATUS) &
                    SOURCE(0, SRC_PASSTHROUGH), ==, SOURCE(0, SRC_PASSTHROUGH));
    g_assert_cmphex(qtest_readb(qts, serial_a + REG_RBR), ==, sent);

    close(fd);
    close(lfd);
    qtest_quit(qts);
}

/*
 * The transmit record: a console consumer that stops reading holds up the
 * port's own backend and, once the transmitter's hold bound gives up, costs
 * the socket peer those characters. The record backend is not on that path,
 * so what the guest transmitted survives whatever the consumer does.
 */
#define RECORD_SERIAL_ARGS \
    "-serial none -serial none -serial none " \
    "-chardev file,id=ioc3-a-record,path=%s -serial chardev:ioc3-a-record"

static char *record_path(const char *name)
{
    char *path = g_strdup_printf("sn0-ioc3-serial-%s.record", name);

    g_unlink(path);
    return path;
}

static void assert_record_is(const char *path, const uint8_t *want,
                             size_t want_len)
{
    g_autofree char *got = NULL;
    gsize got_len = 0;

    g_assert_true(g_file_get_contents(path, &got, &got_len, NULL));
    g_assert_cmpuint(got_len, ==, want_len);
    g_assert_cmpint(memcmp(got, want, want_len), ==, 0);
}

static void test_a_stalled_consumer_keeps_the_ring_record(const void *data)
{
    static const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };
    enum { CHARS = 1024 };
    g_autofree char *path = record_path("ring");
    uint8_t payload[CHARS];
    QTestState *qts;
    unsigned int i;
    int sv[2];

    for (i = 0; i < CHARS; i++) {
        payload[i] = 'A' + (i % 26);
    }

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    g_assert_cmpuint(fill_socket_send_queue_exact(sv[1]), >, 0);

    qts = qtest_initf(
        "-M origin200 -chardev socket,id=ioc3-a,fd=%d "
        "-serial chardev:ioc3-a " RECORD_SERIAL_ARGS, sv[1], path);
    g_assert_cmpint(close(sv[1]), ==, 0);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    enable_dma(qts, 0);

    for (i = 0; i < CHARS; i += 4) {
        write_transmit_entry(qts, RING_TX(0), i * 2, payload + i, control);
    }
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), CHARS * 2);

    /* Each held character costs one give-up before the next is attempted. */
    for (i = 0; i < 8; i++) {
        qtest_clock_step(qts, XMIT_GIVE_UP_NS + 1LL * 1000 * 1000 * 1000);
    }

    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_TX_CONSUME)) & 0xff8,
                    ==, CHARS * 2);

    close(sv[0]);
    qtest_quit(qts);

    assert_record_is(path, payload, CHARS);
    g_unlink(path);
}

static void test_a_stalled_consumer_keeps_the_byte_wide_record(
    const void *data)
{
    static const uint8_t want[] = { 'x', 'Z' };
    g_autofree char *path = record_path("byte-wide");
    QTestState *qts;
    int sv[2];

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    g_assert_cmpuint(fill_socket_send_queue_exact(sv[1]), >, 0);

    qts = qtest_initf(
        "-M origin200 -chardev socket,id=ioc3-a,fd=%d "
        "-serial chardev:ioc3-a " RECORD_SERIAL_ARGS, sv[1], path);
    g_assert_cmpint(close(sv[1]), ==, 0);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);

    /* sv[0] is never read, so the hold bound gives up on 'x'. */
    qtest_writeb(qts, serial_port(0) + REG_RBR, 'x');
    qtest_clock_step(qts, XMIT_GIVE_UP_NS + 1LL * 1000 * 1000 * 1000);
    g_assert_cmphex(qtest_readb(qts, serial_port(0) + REG_LSR) & LSR_TEMT,
                    ==, LSR_TEMT);

    qtest_writeb(qts, serial_port(0) + REG_RBR, 'Z');

    close(sv[0]);
    qtest_quit(qts);

    assert_record_is(path, want, sizeof(want));
    g_unlink(path);
}

/* Loopback returns the character to the port, so no wire carried it. */
static void test_a_loopback_character_stays_out_of_the_record(
    const void *data)
{
    static const uint8_t out[4] = { 'l', 'o', 'o', 'p' };
    static const uint8_t control[4] = {
        TX_CHARACTER, TX_CHARACTER, TX_CHARACTER, TX_CHARACTER,
    };
    g_autofree char *path = record_path("loopback");
    QTestState *qts;
    uint8_t status[4];
    uint8_t got[4];

    qts = qtest_initf("-M origin200 " RECORD_SERIAL_ARGS
                      " -serial none", path);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);
    set_loopback(qts, 0);
    enable_dma(qts, 0);

    write_transmit_entry(qts, RING_TX(0), 0, out, control);
    qtest_writel(qts, sdma(0, SDMA_TX_PRODUCE), 8);
    collect_rx_chars(qts, 0, sizeof(got), got, status);
    g_assert_cmpint(memcmp(got, out, sizeof(out)), ==, 0);

    qtest_quit(qts);

    assert_record_is(path, out, 0);
    g_unlink(path);
}

/*
 * docs/datasheets/ioc3.md, "IOC3 overloads the scratch register": while a
 * port's divisor latch is selected, a write to its scratch register (offset
 * 7) programs the 16550's clock pre-divider instead of storing a scratch
 * byte. The PROM and IRIX write 6, for a divide of 3. A model that treats
 * offset 7 as plain storage still boots -- the guest only ever reads back
 * what it wrote -- so the only thing that can hold this contract is a real
 * baud rate landing on the far side of a backend that cares what it is.
 *
 * -chardev wctablet is that backend: chardev/wctablet.c answers nothing at
 * all unless the frontend's computed line speed is exactly 9600, gating
 * every reply on the very number this overload feeds. A predivider of 6
 * with a divisor of 48 lands on 9600 only if the reference clock behind the
 * divisor is the datasheet's inferred 22.1184 MHz crystal, divided by three;
 * left unprogrammed, the 115200 Hz default gives 2400 instead and the
 * chardev stays silent. Everything below runs synchronously inside a single
 * qtest_writeb(): the wctablet backend answers a THR write by calling back
 * into the port's receive path before that write returns, so no clock step
 * is needed to observe a reply.
 */
#define WCTABLET_PREDIVIDE_BY_3 6
#define WCTABLET_DIVISOR_9600   48
#define WCTABLET_IDENTITY       "~#CT-0045R,V1.3-5,"

/*
 * Selects the divisor latch, writes the pre-divider unless it is 0 (the
 * "left unprogrammed" case), then the divisor, then returns the port to
 * 8N1 with the divisor latch cleared. Asserts the scratch register reads
 * back what it was given while the latch is still selected, the only time
 * the datasheet's overload leaves the guest anything to read back against.
 */
static void configure_line_at_divisor(QTestState *qts, unsigned int port,
                                      uint8_t predivider, uint16_t divisor)
{
    qtest_writeb(qts, serial_port(port) + REG_LCR, LCR_DLAB | LCR_8N1);
    if (predivider) {
        qtest_writeb(qts, serial_port(port) + REG_SCR, predivider);
        g_assert_cmphex(qtest_readb(qts, serial_port(port) + REG_SCR), ==,
                        predivider);
    }
    qtest_writeb(qts, serial_port(port) + REG_RBR, divisor & 0xff);
    qtest_writeb(qts, serial_port(port) + REG_IER, divisor >> 8);
    qtest_writeb(qts, serial_port(port) + REG_LCR, LCR_8N1);
}

static void send_bytes(QTestState *qts, unsigned int port,
                       const uint8_t *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        qtest_writeb(qts, serial_port(port) + REG_RBR, data[i]);
    }
}

/*
 * No FIFO is enabled, so hw/char/serial.c's serial_can_receive() advertises
 * room for exactly one more byte, and only once LSR_DR is clear: the
 * backend's next queued byte arrives only when the RBR read that clears
 * LSR_DR runs qemu_chr_fe_accept_input(). Everything is synchronous, so
 * the first read with LSR_DR unset means the backend has nothing further
 * queued right now, not that a reply is still coming.
 */
static size_t read_reply(QTestState *qts, unsigned int port,
                         uint8_t *buf, size_t max)
{
    size_t got = 0;

    while (got < max &&
          (qtest_readb(qts, serial_port(port) + REG_LSR) & LSR_DR)) {
        buf[got++] = qtest_readb(qts, serial_port(port) + REG_RBR);
    }
    return got;
}

static void test_the_scratch_predivider_reaches_the_backends_baud_rate(
    const void *data)
{
    QTestState *qts;
    uint8_t reply[sizeof(WCTABLET_IDENTITY) - 1];
    size_t got;

    qts = qtest_initf("-M origin200 -chardev wctablet,id=tab "
                      "-serial chardev:tab");
    sn0_ioc3_attach(qts);

    configure_line_at_divisor(qts, 0, WCTABLET_PREDIVIDE_BY_3,
                              WCTABLET_DIVISOR_9600);
    send_bytes(qts, 0, (const uint8_t *)"~#", 2);
    got = read_reply(qts, 0, reply, sizeof(reply));

    g_assert_cmpuint(got, ==, sizeof(reply));
    g_assert_cmpint(memcmp(reply, WCTABLET_IDENTITY, sizeof(reply)), ==, 0);

    qtest_quit(qts);
}

static void test_a_left_unprogrammed_predivider_leaves_the_backend_silent(
    const void *data)
{
    QTestState *qts;
    uint8_t reply[sizeof(WCTABLET_IDENTITY) - 1];

    qts = qtest_initf("-M origin200 -chardev wctablet,id=tab "
                      "-serial chardev:tab");
    sn0_ioc3_attach(qts);

    configure_line_at_divisor(qts, 0, 0, WCTABLET_DIVISOR_9600);
    send_bytes(qts, 0, (const uint8_t *)"~#", 2);

    g_assert_cmpuint(read_reply(qts, 0, reply, sizeof(reply)), ==, 0);

    qtest_quit(qts);
}

/*
 * The pre-divider is a migrated field, but the baudbase it derives is a
 * realize-time property of the embedded SerialState, not itself a vmstate
 * field: vmstate_serial's own post_load runs first, nested inside the
 * struct-array field load above sgi_ioc3_post_load, and computes the
 * port's parameters from the 16550 core's stock default before
 * sgi_ioc3_post_load gets a chance to rederive anything. A snapshot taken
 * after the guest programmed a pre-divider otherwise comes back at the
 * wrong baud rate until the guest touches the scratch register again --
 * invisible to a register readback, so this drives it through the same
 * -chardev wctablet destination as the two tests above, but checks the
 * destination of a migration instead of the machine that was configured.
 */
static void test_the_scratch_predivider_survives_migration(const void *data)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint8_t reply[sizeof(WCTABLET_IDENTITY) - 1];
    size_t got;

    tmpfs = g_dir_make_tmp("sn0-ioc3-serial-migration-XXXXXX", &err);
    g_assert_no_error(err);
    g_assert_nonnull(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_initf("-M origin200 -chardev wctablet,id=tab "
                       "-serial chardev:tab");
    sn0_ioc3_attach(from);
    configure_line_at_divisor(from, 0, WCTABLET_PREDIVIDE_BY_3,
                              WCTABLET_DIVISOR_9600);

    /* Same devices as the source, listening before its QMP handshake. */
    dest_cmdline = g_strdup_printf("-M origin200 -chardev wctablet,id=tab "
                                   "-serial chardev:tab -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    sgi_migrate_stopped(from, uri);

    send_bytes(to, 0, (const uint8_t *)"~#", 2);
    got = read_reply(to, 0, reply, sizeof(reply));

    g_assert_cmpuint(got, ==, sizeof(reply));
    g_assert_cmpint(memcmp(reply, WCTABLET_IDENTITY, sizeof(reply)), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * sgi_ioc3_reset_hold() does not touch serial_predivider[], and the 16550
 * core's own reset does not touch baudbase either, so a system reset --
 * unlike migration's nested vmstate_serial post_load -- has nothing that
 * would recompute the port's baud rate from the wrong default in the first
 * place. This is the datasheet's stated approximation from the other
 * direction: reprogram only the divisor and line control across a reset,
 * the way a guest that assumes its earlier pre-divider write survived
 * would, and the backend should still answer at the pre-divider's baud
 * rate rather than at whatever a cleared pre-divider would fall back to.
 */
static void test_the_scratch_predivider_survives_a_system_reset(
    const void *data)
{
    QTestState *qts;
    uint8_t reply[sizeof(WCTABLET_IDENTITY) - 1];
    size_t got;

    qts = qtest_initf("-M origin200 -chardev wctablet,id=tab "
                      "-serial chardev:tab");
    sn0_ioc3_attach(qts);

    configure_line_at_divisor(qts, 0, WCTABLET_PREDIVIDE_BY_3,
                              WCTABLET_DIVISOR_9600);

    qtest_system_reset(qts);
    sn0_ioc3_attach(qts);

    /* Only the divisor and line control, as a guest assuming survival would. */
    configure_line_at_divisor(qts, 0, 0, WCTABLET_DIVISOR_9600);
    send_bytes(qts, 0, (const uint8_t *)"~#", 2);
    got = read_reply(qts, 0, reply, sizeof(reply));

    g_assert_cmpuint(got, ==, sizeof(reply));
    g_assert_cmpint(memcmp(reply, WCTABLET_IDENTITY, sizeof(reply)), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qts = qtest_init("-M origin200");

    qtest_add_data_func("sn0/ioc3-serial/control-register-round-trips", NULL,
                        test_the_control_register_round_trips);
    qtest_add_data_func("sn0/ioc3-serial/index-keeps-its-pointer-field", NULL,
                        test_an_index_keeps_only_its_pointer_field);
    qtest_add_data_func("sn0/ioc3-serial/receive-consume-keeps-arming-bit",
                        NULL,
                        test_the_receive_consume_keeps_its_arming_bit);
    qtest_add_data_func("sn0/ioc3-serial/reload-write-loads-idle-count", NULL,
                        test_a_reload_write_loads_the_idle_count);
    qtest_add_data_func("sn0/ioc3-serial/receive-timer-keeps-twelve-bits", NULL,
                        test_the_receive_timer_keeps_twelve_bits);
    qtest_add_data_func("sn0/ioc3-serial/alias-pages-are-views", NULL,
                        test_the_alias_pages_are_views_of_the_main_block);
    qtest_add_data_func("sn0/ioc3-serial/modem-status-factory-test-write",
                        NULL, test_modem_status_takes_a_factory_test_write);
    qtest_add_data_func("sn0/ioc3-serial/line-status-factory-test-write",
                        NULL, test_line_status_takes_a_factory_test_write);
    qtest_add_data_func("sn0/ioc3-serial/modem-control-holds-autoflow", NULL,
                        test_modem_control_holds_the_autoflow_bit);
    qtest_add_data_func("sn0/ioc3-serial/main-block-places-engine-indexes",
                        NULL, test_the_main_block_places_the_engine_indexes);
    qtest_add_data_func("sn0/ioc3-serial/alias-refuses-hardware-registers",
                        NULL,
                        test_the_alias_refuses_the_registers_hardware_owns);
    qtest_add_data_func("sn0/ioc3-serial/shadow-mirrors-the-port", NULL,
                        test_the_shadow_mirrors_the_port);
    qtest_add_data_func("sn0/ioc3-serial/loopback-flow-control-reports-cts",
                        NULL, test_loopback_flow_control_reports_clear_to_send);
    qtest_add_data_func("sn0/ioc3-serial/transmit-entry-sends-four", NULL,
                        test_a_transmit_entry_sends_its_four_characters);
    qtest_add_data_func("sn0/ioc3-serial/blocked-backend-keeps-every-character",
                        NULL,
                        test_a_blocked_backend_keeps_every_transmit_character);
    qtest_add_data_func(
        "sn0/ioc3-serial/never-drained-backend-completes-byte-wide-write",
        NULL,
        test_a_never_drained_backend_completes_the_byte_wide_write);
    qtest_add_data_func(
        "sn0/ioc3-serial/never-drained-backend-still-advances-the-ring",
        NULL,
        test_a_never_drained_backend_still_advances_the_ring);
    qtest_add_data_func(
        "sn0/ioc3-serial/dead-backend-completes-further-writes-at-once",
        NULL,
        test_a_dead_backend_completes_further_writes_at_once);
    qtest_add_data_func(
        "sn0/ioc3-serial/recovered-backend-resumes-delivery",
        NULL,
        test_a_recovered_backend_resumes_delivery);
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
    qtest_add_data_func("sn0/ioc3-serial/empty-slot-ends-the-entry-mid-way",
                        NULL, test_an_empty_slot_ends_the_entry_mid_way);
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
    qtest_add_data_func("sn0/ioc3-serial/node1-port-a-receives-on-serial-hd-6",
                        NULL,
                        test_node1_ioc3_a_receives_through_serial_hd_6);

    qtest_add_data_func("sn0/ioc3-serial/stalled-consumer-keeps-ring-record",
                        NULL,
                        test_a_stalled_consumer_keeps_the_ring_record);
    qtest_add_data_func(
        "sn0/ioc3-serial/stalled-consumer-keeps-byte-wide-record", NULL,
        test_a_stalled_consumer_keeps_the_byte_wide_record);
    qtest_add_data_func("sn0/ioc3-serial/loopback-stays-out-of-record",
                        NULL,
                        test_a_loopback_character_stays_out_of_the_record);
    qtest_add_data_func(
        "sn0/ioc3-serial/scratch-predivider-reaches-backend-baud-rate", NULL,
        test_the_scratch_predivider_reaches_the_backends_baud_rate);
    qtest_add_data_func(
        "sn0/ioc3-serial/unprogrammed-predivider-leaves-backend-silent",
        NULL,
        test_a_left_unprogrammed_predivider_leaves_the_backend_silent);
    qtest_add_data_func(
        "sn0/ioc3-serial/scratch-predivider-survives-migration", NULL,
        test_the_scratch_predivider_survives_migration);
    qtest_add_data_func(
        "sn0/ioc3-serial/scratch-predivider-survives-a-system-reset", NULL,
        test_the_scratch_predivider_survives_a_system_reset);

    ret = g_test_run();
    qtest_quit(qts);

    return ret;
}
