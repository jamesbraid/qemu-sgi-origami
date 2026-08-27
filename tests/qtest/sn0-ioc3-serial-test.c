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
/* Bit 22 is the read-only PCI-arbiter-idle status the read always reports. */
#define CHIP_CTRL_ARB_IDLE  0x00400000u

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

/*
 * The 16550 shadow register's fields: the port's own status and modem lines,
 * mirrored so software driving the DMA rings never has to touch the byte-wide
 * window. Data-ready and transmit-holding-empty come from the line status
 * register, the clear-to-send and carrier-detect levels from the modem status
 * register, and the driven data-terminal-ready and request-to-send from modem
 * control.
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

/*
 * The alias pages, which expose one port's seven registers on a page of their
 * own so a privileged process can map them without the rest of the chip.
 */
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
 * The SuperIO enable mask IRIX 6.5.30's miniroot console runs with once it
 * has switched onto the rings, read live off the stalled machine: port A
 * receive high-water (bit 2), port A receive timer (bit 3) and the keyboard
 * (bit 22) -- and, pointedly, not bit 6, the port's 16550 pass-through. A
 * guest in this mode hears about input through the ring sources or not at
 * all.
 */
#define IRIX_RING_ENABLE_MASK 0x0040000c

static uint64_t serial_port(unsigned int port)
{
    return port ? SERIAL_B : SERIAL_A;
}

/*
 * The deadline on a socket read or accept in this file. It exists only to
 * turn a would-be hang into a failure, not to bound how long a healthy run
 * takes: the device side runs synchronously inside the MMIO write callback
 * (sgi_ioc3_sdma_run under -accel qtest, which drives no vCPU), so the byte
 * is already in the host socket buffer before qtest_writel returns -- it is
 * never lost, only occasionally slow to be scheduled by the host OS. On an
 * oversubscribed CI host that scheduling delay can run past ten seconds, so
 * a short deadline reads as a flake. 120s is the value triage proved passes
 * 40/40 runs under that load (see docs/notebooks/ioc3-serial-dma.md), while
 * a healthy run still finishes the whole file in 7-17s -- nowhere near the
 * ceiling.
 */
#define IOC3_SERIAL_SOCKET_TIMEOUT_SECS 120

/*
 * A read on the port's own socket is how a test sees a character leave the
 * chip, and a model that sends nothing would otherwise hang the suite at that
 * read rather than fail it. A deadline turns the one into the other.
 */
static void serial_socket_deadline(int fd)
{
    struct timeval tv = { .tv_sec = IOC3_SERIAL_SOCKET_TIMEOUT_SECS,
                           .tv_usec = 0 };

    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)),
                    ==, 0);
}

/*
 * A loopback TCP listener the guest's serial_hd(1) connects out to, so a
 * character leaving port B's own 16550 arrives here. qtest_init_with_serial
 * only ever wires serial_hd(0), and the other port-B tests reach the line
 * through the 16550's internal loopback, so this host socket is the only proof
 * that chardev-b is wired from serial_hd(1) rather than left dark. It follows
 * the same bind-listen-before-init pattern sn0-elsc-test.c uses for the AUX
 * port.
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
 * Collect the first n characters the receive engine has delivered to a
 * port's ring, waiting for them to arrive. A character injected into the
 * guest's chardev crosses a host socket and the main loop before the engine
 * sees it, so arrival is asynchronous and how the bytes group into chunks --
 * and therefore into ring entries -- is the host's choice, not the test's.
 * What is deterministic is the characters themselves, in order, each under a
 * status byte with the data-valid flag: this walks the entries the produce
 * index has published and picks out exactly that, so an assertion built on
 * it cannot flake on TCP segmentation.
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

/*
 * The value the firmware writes to the chip control register: port A's
 * SuperIO offset 0x178 divided by eight in bits 1-7, port B's 0x170 the same
 * way in bits 8-14, and a byte bus strobe length of 4. No guest reads those
 * fields back, so a model that dropped them would look identical from a
 * transcript -- which is why they are asserted here or not at all. The read
 * also reports the arbiter idle in bit 22, which the serial init polls.
 */
static void test_the_control_register_round_trips(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    qtest_writel(qts, CHIP_CONTROL, 0x00022e5e);
    g_assert_cmphex(qtest_readl(qts, CHIP_CONTROL), ==,
                    0x00022e5e | CHIP_CTRL_ARB_IDLE);

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
 * The alias pages are additional views of the same seven registers, not
 * storage of their own, and three of the seven are read-only through the
 * alias where they are not through the main block -- the two indices hardware
 * advances and the 16550 shadow. A model implementing the page as a second
 * copy passes a write-then-read of the page alone, which is why both
 * directions are crossed here.
 */
static void test_the_alias_pages_are_views_of_the_main_block(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
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
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    unsigned int port;

    sn0_ioc3_attach(qts);

    for (port = 0; port < 2; port++) {
        uint32_t shadow;

        qtest_writel(qts, sdma_alias(port, SDMA_TX_CONSUME), 0xffffffff);
        g_assert_cmphex(qtest_readl(qts, sdma(port, SDMA_TX_CONSUME)), ==, 0);
        qtest_writel(qts, sdma_alias(port, SDMA_RX_PRODUCE), 0xffffffff);
        g_assert_cmphex(qtest_readl(qts, sdma(port, SDMA_RX_PRODUCE)), ==, 0);

        /*
         * The shadow is read-only too, but it is not a stored zero: it mirrors
         * the port's live status and modem lines, so a write must leave what it
         * reads untouched rather than land in it.
         */
        shadow = qtest_readl(qts, sdma(port, SDMA_SHADOW));
        qtest_writel(qts, sdma_alias(port, SDMA_SHADOW), 0xffffffff);
        g_assert_cmphex(qtest_readl(qts, sdma(port, SDMA_SHADOW)), ==, shadow);
    }

    qtest_quit(qts);
}

/*
 * The 16550 shadow register is a read-only mirror of the port's own status and
 * modem lines, and the DMA-side console driver reads carrier-detect and
 * clear-to-send from it rather than from the byte-wide modem-status register.
 * The port comes up with carrier and clear-to-send asserted and the
 * transmitter empty, so a shadow that read zero tells that driver the line is
 * dead -- which is what latched a carrier-down at console open and hung the
 * console. A boot cannot catch this: the firmware's own DMA diagnostic runs in
 * loopback and never reads carrier, so a shadow of zero passes it. Only a
 * driver reading the shadow sees the difference, which is why it is asserted
 * here.
 */
static void test_the_shadow_mirrors_the_port(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    uint32_t shadow;
    unsigned int port;

    sn0_ioc3_attach(qts);

    for (port = 0; port < 2; port++) {
        /*
         * Carrier and clear-to-send present, transmitter empty, nothing
         * received -- the port's reset modem and line status, which each
         * port reports for itself rather than sharing one register.
         */
        shadow = qtest_readl(qts, sdma(port, SDMA_SHADOW));
        g_assert_cmphex(shadow & SHADOW_CD, ==, SHADOW_CD);
        g_assert_cmphex(shadow & SHADOW_CTS, ==, SHADOW_CTS);
        g_assert_cmphex(shadow & SHADOW_THRE, ==, SHADOW_THRE);
        g_assert_cmphex(shadow & SHADOW_DR, ==, 0);

        /*
         * The driven modem-control levels appear in the shadow too: assert
         * data-terminal-ready and request-to-send by hand through the
         * byte-wide window and read them back on the DMA side.
         */
        qtest_writeb(qts, serial_port(port) + REG_MCR, MCR_DTR | MCR_RTS);
        shadow = qtest_readl(qts, sdma(port, SDMA_SHADOW));
        g_assert_cmphex(shadow & SHADOW_DTR, ==, SHADOW_DTR);
        g_assert_cmphex(shadow & SHADOW_RTS, ==, SHADOW_RTS);
    }

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
 * Port B's backend comes from serial_hd(1): the two IOC3 rear ports are
 * serial_hd(0) and serial_hd(1) (hw/misc/sgi-hub.h). A character the port-B
 * transmit engine sends -- with the port out of loopback, so it goes out the
 * line rather than back to its own receiver -- has to arrive on the host
 * socket attached to serial_hd(1). Before chardev-b was wired the property was
 * defined but never set, so every character port B sent went to a NULL backend
 * and this read would time out. serial_hd(0) is left empty here, so the byte
 * arriving proves the second port's chardev, not the first's.
 */
static void test_port_b_transmits_through_serial_hd_1(const void *data)
{
    uint16_t port;
    int lfd = portb_listen(&port);
    g_autofree char *args = g_strdup_printf(
        "-M origin200,io-bridge=on -serial none "
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

/*
 * A guest that receives through the rings alone. IRIX 6.5.30's miniroot
 * console runs this way once it raises the line rate: ring DMA on, the
 * receive timer armed, the SuperIO enable mask carrying only the ring
 * receive sources, and the 16550's own interrupt-enable register never
 * written -- so the port's interrupt line, the engine's old only prompt,
 * never rises. A character arriving from the chardev must still be packed
 * into the receive ring and advance the produce index, or the console is
 * deaf for the whole interactive install. This is the register state the
 * live probe read off the stalled machine, replayed as a test.
 */
static void test_a_ring_only_guest_receives_arriving_characters(
    const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t sent[4] = { 'i', 'r', 'i', 'x' };
    uint8_t got[4], status[4];
    unsigned int i;

    qts = qtest_init_with_serial("-M origin200,io-bridge=on", &sock);
    serial_socket_deadline(sock);

    sn0_ioc3_attach(qts);
    configure_line(qts, 0);

    qtest_writel(qts, INT_ENABLE_SET, IRIX_RING_ENABLE_MASK);
    qtest_writel(qts, SDMA_RING_HI, 0);
    qtest_writel(qts, SDMA_RING_LO, RING_BASE | RING_4K);
    qtest_writel(qts, sdma(0, SDMA_RX_TIMER), 0x140);
    qtest_writel(qts, sdma(0, SDMA_RX_CONSUME), CONSUME_ARM_TIMER);
    /*
     * The control value the miniroot programs: ring DMA enabled, quadruple
     * speed, a receive high-water mark of four characters.
     */
    qtest_writel(qts, sdma(0, SDMA_CONTROL), 0x10100004);

    g_assert_cmpint(write(sock, sent, sizeof(sent)), ==, (int)sizeof(sent));
    collect_rx_chars(qts, 0, sizeof(sent), got, status);

    for (i = 0; i < sizeof(sent); i++) {
        g_assert_cmphex(got[i], ==, sent[i]);
        g_assert_cmphex(status[i] & RX_ERRORS, ==, 0);
        g_assert_cmphex(status[i] & RX_MODEM_VALID, ==, RX_MODEM_VALID);
    }
    g_assert_cmphex(qtest_readl(qts, sdma(0, SDMA_RX_PRODUCE)) & 0xff8, >=, 8);

    /*
     * The pass-through source stayed out of it: the 16550's own interrupt
     * enables were never written, so its line never rose, and the bit that
     * follows it stays clear. Delivery came through the engine alone.
     */
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & SOURCE(0, SRC_PASSTHROUGH),
                    ==, 0);

    close(sock);
    qtest_quit(qts);
}

/*
 * The receive timer: armed through bit 31 of the consume index, wound to its
 * reload by a delivery, counting receive idleness at 16 kHz, and raising its
 * source on expiry -- not on the delivery itself. Under the qtest
 * accelerator the virtual clock moves only when the test steps it, so the
 * gap between "character delivered" and "interrupt raised" is observable
 * here in a way no boot can pin down: the source must still be clear after
 * delivery, the count field must read the full reload, and stepping the
 * clock past the deadline must raise the source and run the count out.
 */
static void test_the_receive_timer_raises_on_expiry(
    const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t sent = 'y';
    uint8_t got, status;
    uint32_t val;

    qts = qtest_init_with_serial("-M origin200,io-bridge=on", &sock);
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
 * The receive high-water mark: a supply warning that fires once the
 * characters waiting for software reach the mark the control register sets.
 * It is separate from the timer source -- the timer stays unarmed here, so
 * anything that appears came from the mark -- and the model counts waiting
 * characters as published entries times four, so the mark is set two entries
 * deep and crossed with the fifth character.
 */
static void test_crossing_the_high_water_mark_raises_its_source(
    const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t first = 'a';
    const uint8_t rest[4] = { 'b', 'c', 'd', 'e' };
    uint8_t got[5], status[5];

    qts = qtest_init_with_serial("-M origin200,io-bridge=on", &sock);
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
 * A full receive ring is flow control, not data loss. The engine stops
 * writing when the next entry it would fill is the one software has not
 * consumed yet, raises the ring-full source, and holds what it cannot place
 * -- in its packer and behind that in the port's FIFO, whose fullness is
 * what finally stops the chardev. Nothing is overwritten and nothing is
 * dropped: when software advances the consume index, every held character
 * arrives in order. The consume index starts two entries in, which shrinks
 * the ring's capacity to one entry and makes the full condition cheap to
 * reach.
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

    qts = qtest_init_with_serial("-M origin200,io-bridge=on", &sock);
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

    /*
     * Software makes room, and everything held arrives in order. The room
     * is granted in entries and how the characters group into entries is
     * the host's chunking, worst case one character each -- eleven may
     * still be in flight -- so the consume index moves fourteen entries
     * ahead rather than the two a four-per-entry packing would need.
     */
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
 * Pass-through mode still works with the engine present: with ring DMA off
 * and the 16550's own receive interrupt enabled -- OpenBSD's arrangement --
 * an arriving character stays in the port's FIFO for the byte-wide window,
 * raises the pass-through source, and never touches the receive ring. The
 * engine running on every port event must not steal characters a guest
 * means to read itself.
 */
static void test_pass_through_receives_while_the_rings_stay_off(
    const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t sent = 'p';

    qts = qtest_init_with_serial("-M origin200,io-bridge=on", &sock);
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
 * The receive drain, control bit 27, is a command rather than a setting: it
 * moves whatever the port is holding into the receive ring, and it reads
 * back clear once that has happened. IRIX's DMA-ring console writes enable
 * plus drain while reconfiguring the port and then polls the control
 * register until the drain bit clears -- against a model that stored the
 * bit it burned a million polls, tore the console down and retried forever,
 * and the boot never returned. The read-back is the load-bearing half of
 * the contract; the delivery half is that a character already waiting in
 * the port when the drain is written ends up in the ring.
 */
static void test_the_receive_drain_acts_and_reads_back_clear(const void *data)
{
    QTestState *qts;
    int sock;
    const uint8_t sent = 'd';
    uint8_t got, status;
    uint32_t control;

    qts = qtest_init_with_serial("-M origin200,io-bridge=on", &sock);
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
    qtest_add_data_func("sn0/ioc3-serial/full-ring-holds-characters", NULL,
                        test_a_full_ring_holds_characters);
    qtest_add_data_func("sn0/ioc3-serial/pass-through-still-receives", NULL,
                        test_pass_through_receives_while_the_rings_stay_off);
    qtest_add_data_func("sn0/ioc3-serial/receive-drain-reads-back-clear", NULL,
                        test_the_receive_drain_acts_and_reads_back_clear);

    return g_test_run();
}
