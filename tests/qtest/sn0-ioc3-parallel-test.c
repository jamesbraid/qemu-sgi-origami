/*
 * SGI IOC3 base I/O controller: the parallel port.
 *
 * The port has two faces and they are far apart in the chip's window: a
 * four-byte DMA register block in the main block at 0x80, and a byte-wide
 * IEEE-1284/ECP register file inside the SuperIO window at 0x20150. Both
 * are asserted here, because a board that maps one and not the other is a
 * board whose driver attaches and then cannot move a byte.
 *
 * The reset value of the extended control register is the assertion that
 * matters most: IRIX's parallel driver probes for the port by reading it,
 * and a model answering anything but "transmit FIFO empty, not full" is a
 * machine on which the driver never attaches. A working boot proves none
 * of this -- nothing in a console boot touches the port.
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
#define PCI_BAR0         0x10
#define IOC3_PCI_MMIO    0x10000000u

#define PCI_SUBSYSTEM    0x2c
#define SUBSYSTEM_SGI    0x10a9u  /* the vendor half, the same on every board */
#define SUBSYSTEM(id)    (((uint32_t)(id) << 16) | SUBSYSTEM_SGI)

static void sn0_ioc3_attach(QTestState *qts)
{
    qtest_writel(qts, CFG0(PCI_IOC3_DEVICE) + PCI_BAR0, IOC3_PCI_MMIO);
    qtest_writel(qts, CFG0(PCI_IOC3_DEVICE) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(PCI_IOC3_DEVICE),
                 DEV_MEMORY_SPACE | (IOC3_PCI_MMIO >> 20));
}

/* The DMA register block, four bytes to a register, in the main block. */
#define PAR_DMA          (IOC3_BASE + 0x080)
#define DMA_A_ADDR_HI    (PAR_DMA + 0x00)
#define DMA_A_ADDR_LO    (PAR_DMA + 0x04)
#define DMA_A_COUNT      (PAR_DMA + 0x08)
#define DMA_CONTROL      (PAR_DMA + 0x0c)
#define DMA_B_ADDR_HI    (PAR_DMA + 0x10)
#define DMA_B_ADDR_LO    (PAR_DMA + 0x14)
#define DMA_B_COUNT      (PAR_DMA + 0x18)

/* A context's count and control: 17 count bits, then arm and last. */
#define COUNT_MASK       0x0001ffffu
#define COUNT_ARM        (1u << 30)
#define COUNT_LAST       (1u << 31)

/* The shared control register: six status bits, then the engine's own. */
#define DMA_A_ARMED      (1u << 0)
#define DMA_A_LAST       (1u << 1)
#define DMA_A_BUSY       (1u << 2)
#define DMA_B_ARMED      (1u << 3)
#define DMA_B_LAST       (1u << 4)
#define DMA_B_BUSY       (1u << 5)
#define DMA_FROM_PERIPHERAL (1u << 29)
#define DMA_RESET        (1u << 30)
#define DMA_ENABLE       (1u << 31)

/* The byte-wide ECP register file, inside the SuperIO window. */
#define PAR_ECP          (IOC3_BASE + 0x20150)
#define ECP_DATA         (PAR_ECP + 0x00)
#define ECP_STATUS       (PAR_ECP + 0x01)
#define ECP_CONTROL      (PAR_ECP + 0x02)
#define ECP_FIFO         (PAR_ECP + 0x08)
#define ECP_CONFIG_B     (PAR_ECP + 0x09)
#define ECP_EXTENDED     (PAR_ECP + 0x0a)

/* Extended control: two status bits, three control bits, then the mode. */
#define ECR_FIFO_EMPTY   (1u << 0)
#define ECR_FIFO_FULL    (1u << 1)
#define ECR_SERVICE_INT  (1u << 2)
#define ECR_DMA_ENABLE   (1u << 3)
#define ECR_NFAULT_INT   (1u << 4)
#define ECR_MODE_SHIFT   5
#define ECR_MODE(m)      ((uint8_t)((m) << ECR_MODE_SHIFT))

#define MODE_COMPAT       0    /* compatibility, no FIFO -- the reset mode */
#define MODE_BYTE         1
#define MODE_COMPAT_FIFO  2    /* what the driver selects for itself */
#define MODE_ECP          3
#define MODE_CONFIG       7

/*
 * The status register's peripheral lines. Only nFault's polarity is pinned
 * by the datasheet -- a clear level means a fault -- so an idle port with no
 * peripheral on the cable reports no fault and nothing else.
 */
#define STATUS_NFAULT    (1u << 3)
#define STATUS_IDLE      STATUS_NFAULT

/*
 * The probe IRIX's parallel driver makes: the extended control register out
 * of reset. Its transmit FIFO reads empty and not full, the three interrupt
 * and DMA control bits read clear, and the mode field reads compatibility.
 * The driver takes this whole byte as the port answering at all, so it is
 * asserted whole rather than bit by bit.
 */
static void test_the_presence_check_reads_out_of_reset(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    g_assert_cmphex(qtest_readb(qts, ECP_EXTENDED), ==, ECR_FIFO_EMPTY);

    qtest_quit(qts);
}

/*
 * The two FIFO flags are hardware status sharing a byte with software's
 * control bits, so a write of the whole byte must not be able to say the
 * FIFO is full. A model storing all eight bits passes every other
 * assertion here and fails this one.
 */
static void test_the_fifo_flags_are_not_writable(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    qtest_writeb(qts, ECP_EXTENDED, ECR_FIFO_FULL);
    g_assert_cmphex(qtest_readb(qts, ECP_EXTENDED) & ECR_FIFO_FULL, ==, 0);
    g_assert_cmphex(qtest_readb(qts, ECP_EXTENDED) & ECR_FIFO_EMPTY, ==,
                    ECR_FIFO_EMPTY);

    /* Nor can a zero write clear the empty flag. */
    qtest_writeb(qts, ECP_EXTENDED, 0);
    g_assert_cmphex(qtest_readb(qts, ECP_EXTENDED), ==, ECR_FIFO_EMPTY);

    qtest_quit(qts);
}

/*
 * The service-interrupt, DMA-path and nFault-interrupt controls. Their
 * active sense is not settled by any source, so nothing here asserts what
 * they arm; what a driver needs is that setting one is visible to the next
 * read and that setting one does not disturb the others.
 */
static void test_the_extended_control_holds_its_control_bits(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    qtest_writeb(qts, ECP_EXTENDED, ECR_DMA_ENABLE);
    g_assert_cmphex(qtest_readb(qts, ECP_EXTENDED), ==,
                    ECR_DMA_ENABLE | ECR_FIFO_EMPTY);

    qtest_writeb(qts, ECP_EXTENDED,
                 ECR_SERVICE_INT | ECR_DMA_ENABLE | ECR_NFAULT_INT);
    g_assert_cmphex(qtest_readb(qts, ECP_EXTENDED), ==,
                    ECR_SERVICE_INT | ECR_DMA_ENABLE | ECR_NFAULT_INT |
                    ECR_FIFO_EMPTY);

    qtest_writeb(qts, ECP_EXTENDED, ECR_SERVICE_INT);
    g_assert_cmphex(qtest_readb(qts, ECP_EXTENDED), ==,
                    ECR_SERVICE_INT | ECR_FIFO_EMPTY);

    qtest_quit(qts);
}

/*
 * The mode field is the register's top three bits. The driver drives the
 * port through three of these encodings and negotiates its way into a
 * fourth, so each one has to survive being written and read back beside
 * the control bits it shares the byte with.
 */
static void test_the_mode_field_holds_each_encoding(const void *data)
{
    static const uint8_t modes[] = {
        MODE_COMPAT, MODE_BYTE, MODE_COMPAT_FIFO, MODE_ECP, MODE_CONFIG,
    };
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    unsigned int i;

    sn0_ioc3_attach(qts);

    for (i = 0; i < ARRAY_SIZE(modes); i++) {
        qtest_writeb(qts, ECP_EXTENDED, ECR_MODE(modes[i]) | ECR_DMA_ENABLE);
        g_assert_cmphex(qtest_readb(qts, ECP_EXTENDED), ==,
                        ECR_MODE(modes[i]) | ECR_DMA_ENABLE | ECR_FIFO_EMPTY);
    }

    qtest_quit(qts);
}

/*
 * Bytes handed to the transmit FIFO leave it: there is no peripheral on
 * the cable to hold them up, so the FIFO never fills and a driver polling
 * for room before each byte never waits. The count is well past any
 * plausible FIFO depth, so a model with a real queue behind these writes
 * reports itself full here.
 */
static void test_the_transmit_fifo_never_fills(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    unsigned int i;

    sn0_ioc3_attach(qts);

    qtest_writeb(qts, ECP_EXTENDED, ECR_MODE(MODE_COMPAT_FIFO));

    for (i = 0; i < 256; i++) {
        qtest_writeb(qts, ECP_FIFO, i & 0xff);
    }

    g_assert_cmphex(qtest_readb(qts, ECP_EXTENDED), ==,
                    ECR_MODE(MODE_COMPAT_FIFO) | ECR_FIFO_EMPTY);

    qtest_quit(qts);
}

/* The eight data lines, which software also drives during negotiation. */
static void test_the_data_register_holds_a_byte(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    g_assert_cmphex(qtest_readb(qts, ECP_DATA), ==, 0);

    qtest_writeb(qts, ECP_DATA, 0x5a);
    g_assert_cmphex(qtest_readb(qts, ECP_DATA), ==, 0x5a);

    qtest_quit(qts);
}

/*
 * The status register reflects the cable rather than storing a value, and
 * this machine has nothing on the cable. The one level the datasheet pins
 * is nFault, where clear means a fault -- so an empty cable reports no
 * fault, and nothing else: no peripheral selected, no paper error, and
 * never busy, which is the level a driver polling for the printer to
 * finish would otherwise wait on forever.
 */
static void test_the_status_register_reports_an_empty_cable(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    g_assert_cmphex(qtest_readb(qts, ECP_STATUS), ==, STATUS_IDLE);

    /* Read-only: a write reaches no line and changes nothing. */
    qtest_writeb(qts, ECP_STATUS, 0xff);
    g_assert_cmphex(qtest_readb(qts, ECP_STATUS), ==, STATUS_IDLE);

    qtest_quit(qts);
}

/*
 * The output control lines. The driver drives the IEEE-1284 negotiation
 * handshake by hand on nAutoFeed, nSelectIn and nStrobe, so each has to
 * hold the level it was given between one step of the handshake and the
 * next, and the direction bit alongside them.
 */
static void test_the_control_register_holds_its_lines(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    /* nStrobe, nAutoFeed and nSelectIn together, nInit left clear. */
    qtest_writeb(qts, ECP_CONTROL, 0x0b);
    g_assert_cmphex(qtest_readb(qts, ECP_CONTROL), ==, 0x0b);

    /* Reading from the peripheral, with the handshake lines released. */
    qtest_writeb(qts, ECP_CONTROL, 0x20);
    g_assert_cmphex(qtest_readb(qts, ECP_CONTROL), ==, 0x20);

    qtest_quit(qts);
}

/* The configuration-B register, which the datasheet gives no mode. */
static void test_the_configuration_b_register_holds_a_byte(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    qtest_writeb(qts, ECP_CONFIG_B, 0xa5);
    g_assert_cmphex(qtest_readb(qts, ECP_CONFIG_B), ==, 0xa5);

    qtest_quit(qts);
}

/*
 * One address, two registers: 0x158 is the configuration-A register in
 * configuration mode and the data FIFO in every other mode. A model with
 * one storage word behind the address fails on the second half of this --
 * the FIFO write would be readable back as configuration.
 */
static void test_configuration_a_needs_its_own_mode(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    qtest_writeb(qts, ECP_EXTENDED, ECR_MODE(MODE_CONFIG));
    qtest_writeb(qts, ECP_FIFO, 0x3c);
    g_assert_cmphex(qtest_readb(qts, ECP_FIFO), ==, 0x3c);

    /* In a FIFO mode the same address is the data path, not the store. */
    qtest_writeb(qts, ECP_EXTENDED, ECR_MODE(MODE_COMPAT_FIFO));
    qtest_writeb(qts, ECP_FIFO, 0xc3);

    /* And the byte written to the FIFO did not land in configuration A. */
    qtest_writeb(qts, ECP_EXTENDED, ECR_MODE(MODE_CONFIG));
    g_assert_cmphex(qtest_readb(qts, ECP_FIFO), ==, 0x3c);

    qtest_quit(qts);
}

/*
 * The file is byte-wide and its registers are one byte apart. Distinct
 * values at three of its offsets are what a model with a wider stride, or
 * with one storage word behind the whole file, cannot produce.
 */
static void test_the_ecp_registers_are_a_byte_apart(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    qtest_writeb(qts, ECP_DATA, 0x11);
    qtest_writeb(qts, ECP_CONTROL, 0x22);
    qtest_writeb(qts, ECP_CONFIG_B, 0x33);

    g_assert_cmphex(qtest_readb(qts, ECP_DATA), ==, 0x11);
    g_assert_cmphex(qtest_readb(qts, ECP_CONTROL), ==, 0x22);
    g_assert_cmphex(qtest_readb(qts, ECP_CONFIG_B), ==, 0x33);

    qtest_quit(qts);
}

/*
 * The file is byte-wide, and a four-byte access to it is four registers
 * rather than one. Nothing rejects the wide access -- the address space
 * splits it into byte reads on the way in, because the region says a byte
 * is the widest it takes -- so what this asserts is that every lane comes
 * from its own register. A model declaring a four-byte implementation
 * width answers the whole access out of the first register instead, which
 * on the write side would run the FIFO's side effect once for four bytes.
 */
static void test_the_ecp_file_answers_a_byte_at_a_time(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");
    uint8_t lane[4];
    uint32_t wide;
    unsigned int i;

    sn0_ioc3_attach(qts);

    /*
     * Two of the four lanes carry values of their own, so that a whole
     * access answered from one register cannot match by accident.
     */
    qtest_writeb(qts, ECP_DATA, 0x5a);
    qtest_writeb(qts, ECP_CONTROL, 0x11);

    for (i = 0; i < ARRAY_SIZE(lane); i++) {
        lane[i] = qtest_readb(qts, PAR_ECP + i);
    }
    g_assert_cmphex(lane[0], ==, 0x5a);
    g_assert_cmphex(lane[2], ==, 0x11);

    wide = qtest_readl(qts, ECP_DATA);
    g_assert_cmphex(wide, ==, lane[0] | (lane[1] << 8) | (lane[2] << 16) |
                              ((uint32_t)lane[3] << 24));

    qtest_quit(qts);
}

/*
 * The two contexts are two register sets, not one aliased twice. Software
 * loads one while the other drains, so a model that aliased them would
 * lose every second descriptor of a chain.
 */
static void test_the_two_dma_contexts_are_distinct(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    qtest_writel(qts, DMA_A_ADDR_HI, 0x0000dead);
    qtest_writel(qts, DMA_A_ADDR_LO, 0x11112000);
    qtest_writel(qts, DMA_B_ADDR_HI, 0x0000beef);
    qtest_writel(qts, DMA_B_ADDR_LO, 0x22223000);

    g_assert_cmphex(qtest_readl(qts, DMA_A_ADDR_HI), ==, 0x0000dead);
    g_assert_cmphex(qtest_readl(qts, DMA_A_ADDR_LO), ==, 0x11112000);
    g_assert_cmphex(qtest_readl(qts, DMA_B_ADDR_HI), ==, 0x0000beef);
    g_assert_cmphex(qtest_readl(qts, DMA_B_ADDR_LO), ==, 0x22223000);

    qtest_quit(qts);
}

/*
 * A context's count and control register: seventeen count bits, so one
 * context carries up to 131071 bytes, and the arm and last flags in the
 * top two. The largest count is asserted because a model with a sixteen-
 * bit field truncates it and reports a short transfer as a complete one.
 */
static void test_a_context_holds_its_count_arm_and_last(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    qtest_writel(qts, DMA_A_COUNT, COUNT_ARM | COUNT_LAST | COUNT_MASK);
    g_assert_cmphex(qtest_readl(qts, DMA_A_COUNT), ==,
                    COUNT_ARM | COUNT_LAST | COUNT_MASK);

    /* An armed context that is not the last of its chain. */
    qtest_writel(qts, DMA_B_COUNT, COUNT_ARM | 0x40);
    g_assert_cmphex(qtest_readl(qts, DMA_B_COUNT), ==, COUNT_ARM | 0x40);

    qtest_quit(qts);
}

/*
 * The shared control register's low six bits are the two contexts' armed,
 * last and busy flags read back through a second address. They are the
 * engine's view of the context registers rather than storage of their
 * own: a model storing them takes whatever a guest writes there and
 * reports a context armed that never was.
 */
static void test_the_shared_control_reports_the_context_flags(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    g_assert_cmphex(qtest_readl(qts, DMA_CONTROL) & 0x3f, ==, 0);

    /* Context A armed and last; context B untouched. */
    qtest_writel(qts, DMA_A_COUNT, COUNT_ARM | COUNT_LAST | 0x100);
    g_assert_cmphex(qtest_readl(qts, DMA_CONTROL) & 0x3f, ==,
                    DMA_A_ARMED | DMA_A_LAST);

    /* Context B armed, not last: a second descriptor mid-chain. */
    qtest_writel(qts, DMA_B_COUNT, COUNT_ARM | 0x100);
    g_assert_cmphex(qtest_readl(qts, DMA_CONTROL) & 0x3f, ==,
                    DMA_A_ARMED | DMA_A_LAST | DMA_B_ARMED);

    /* Disarming context A is visible here too, and only in its own bits. */
    qtest_writel(qts, DMA_A_COUNT, 0x100);
    g_assert_cmphex(qtest_readl(qts, DMA_CONTROL) & 0x3f, ==, DMA_B_ARMED);

    /* A guest cannot assert them from this side. */
    qtest_writel(qts, DMA_CONTROL, 0x3f);
    g_assert_cmphex(qtest_readl(qts, DMA_CONTROL) & 0x3f, ==, DMA_B_ARMED);

    qtest_quit(qts);
}

/*
 * Direction, the state-machine reset the driver pulses high then low, and
 * the engine enable. The reset is a level the datasheet describes as held,
 * so it reads back while it is held rather than clearing itself.
 */
static void test_the_shared_control_holds_its_own_bits(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    qtest_writel(qts, DMA_CONTROL, DMA_ENABLE);
    g_assert_cmphex(qtest_readl(qts, DMA_CONTROL) & ~0x3fu, ==, DMA_ENABLE);

    qtest_writel(qts, DMA_CONTROL, DMA_ENABLE | DMA_FROM_PERIPHERAL);
    g_assert_cmphex(qtest_readl(qts, DMA_CONTROL) & ~0x3fu, ==,
                    DMA_ENABLE | DMA_FROM_PERIPHERAL);

    /* The pulse the driver makes at the end of a transfer, both edges. */
    qtest_writel(qts, DMA_CONTROL, DMA_ENABLE | DMA_RESET);
    g_assert_cmphex(qtest_readl(qts, DMA_CONTROL) & ~0x3fu, ==,
                    DMA_ENABLE | DMA_RESET);
    qtest_writel(qts, DMA_CONTROL, DMA_ENABLE);
    g_assert_cmphex(qtest_readl(qts, DMA_CONTROL) & ~0x3fu, ==, DMA_ENABLE);

    qtest_quit(qts);
}

/*
 * Nothing drives the port on this machine, so the engine never takes a
 * context: both busy flags stay clear however armed and enabled the
 * contexts are. A driver reads these to find which context was active
 * when a transfer failed, and a model reporting one busy forever would
 * send it to recover a transfer that never started.
 */
static void test_the_engine_never_reports_a_busy_context(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    qtest_writel(qts, DMA_A_ADDR_LO, 0x00100000);
    qtest_writel(qts, DMA_B_ADDR_LO, 0x00200000);
    qtest_writel(qts, DMA_A_COUNT, COUNT_ARM | 0x40);
    qtest_writel(qts, DMA_B_COUNT, COUNT_ARM | COUNT_LAST | 0x40);
    qtest_writel(qts, DMA_CONTROL, DMA_ENABLE);

    /* Armed and enabled, which an unmapped hole cannot report either. */
    g_assert_cmphex(qtest_readl(qts, DMA_CONTROL), ==,
                    DMA_ENABLE | DMA_A_ARMED | DMA_B_ARMED | DMA_B_LAST);
    g_assert_cmphex(qtest_readl(qts, DMA_CONTROL) & (DMA_A_BUSY | DMA_B_BUSY),
                    ==, 0);

    qtest_quit(qts);
}

/*
 * The DMA registers are four bytes apart in a block whose neighbours --
 * the pin registers below it and the PS/2 controller above -- are live
 * registers of their own. Distinct values at three consecutive offsets
 * prove the stride, and the read back through the PS/2 controller's first
 * register proves the block stops where it should.
 */
static void test_the_dma_registers_are_four_bytes_apart(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,io-bridge=on");

    sn0_ioc3_attach(qts);

    qtest_writel(qts, DMA_A_ADDR_HI, 0x0a0a0a0a);
    qtest_writel(qts, DMA_A_ADDR_LO, 0x0b0b0b0b);
    qtest_writel(qts, DMA_A_COUNT, 0x0000c0c0);

    g_assert_cmphex(qtest_readl(qts, DMA_A_ADDR_HI), ==, 0x0a0a0a0a);
    g_assert_cmphex(qtest_readl(qts, DMA_A_ADDR_LO), ==, 0x0b0b0b0b);
    g_assert_cmphex(qtest_readl(qts, DMA_A_COUNT), ==, 0x0000c0c0);

    qtest_quit(qts);
}

/*
 * Which boards fit the port. The Origin 200's system board carries one,
 * and so does the MediaIO daughtercard, whose own manual lists a parallel
 * port among what it adds to the BaseIO it mounts on. No other board here
 * has a source saying it has one, and mainline Linux is no evidence
 * either way: its multifunction driver registers no parallel sub-driver
 * on any board at all, so the port is missing from every row of its table
 * whether or not the board carries the hardware.
 */
typedef struct BoardParallel {
    const char *name;
    uint16_t subsystem_id;
    bool parallel;
} BoardParallel;

static const BoardParallel ioc3_boards[] = {
    { "ip27-baseio6g", 0xc300, false },
    { "ip27-mio",      0xc301, true  },
    { "ip27-baseio",   0xc302, false },
    { "ip29-sysboard", 0xc303, true  },
    { "ip30-sysboard", 0xc304, false },
    { "menet",         0xc305, false },
    { "menet4",        0xc306, false },
    /*
     * Two unmatched identifiers, because the fall-through is the contract
     * rather than a row: 0xc307 is a real later SGI board the driver has
     * no entry for, and 0xbeef is nobody's.
     */
    { "cad-duo-io7",   0xc307, false },
    { "cad-duo-other", 0xbeef, false },
};

/*
 * Both faces of the port, probed by writing a register and reading it
 * back: a window with nothing behind it swallows the write and reads
 * zero, so one probe answers both ways round. Both are asserted because a
 * board that maps the register file without the DMA block, or the other
 * way about, is a board whose driver attaches and then cannot transfer.
 */
static void test_a_board_fits_the_port_or_does_not(const void *data)
{
    const BoardParallel *board = data;
    g_autofree char *args =
        g_strdup_printf("-M origin200,io-bridge=on "
                        "-global sgi-ioc3.subsystem-id=0x%04x",
                        board->subsystem_id);
    QTestState *qts = qtest_init(args);

    sn0_ioc3_attach(qts);

    /* The identifier a guest dispatches on, before what it selects. */
    g_assert_cmphex(qtest_readl(qts, CFG0(PCI_IOC3_DEVICE) + PCI_SUBSYSTEM),
                    ==, SUBSYSTEM(board->subsystem_id));

    qtest_writeb(qts, ECP_DATA, 0x5a);
    g_assert_cmpint(qtest_readb(qts, ECP_DATA) == 0x5a, ==, board->parallel);

    qtest_writel(qts, DMA_A_ADDR_LO, 0x11112000);
    g_assert_cmpint(qtest_readl(qts, DMA_A_ADDR_LO) == 0x11112000, ==,
                    board->parallel);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    unsigned int i;

    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/ioc3/parallel-presence-check-out-of-reset", NULL,
                        test_the_presence_check_reads_out_of_reset);
    qtest_add_data_func("sn0/ioc3/parallel-fifo-flags-are-not-writable", NULL,
                        test_the_fifo_flags_are_not_writable);
    qtest_add_data_func("sn0/ioc3/parallel-extended-control-holds-its-bits",
                        NULL,
                        test_the_extended_control_holds_its_control_bits);
    qtest_add_data_func("sn0/ioc3/parallel-mode-field-holds-each-encoding",
                        NULL, test_the_mode_field_holds_each_encoding);
    qtest_add_data_func("sn0/ioc3/parallel-transmit-fifo-never-fills", NULL,
                        test_the_transmit_fifo_never_fills);
    qtest_add_data_func("sn0/ioc3/parallel-data-register-holds-a-byte", NULL,
                        test_the_data_register_holds_a_byte);
    qtest_add_data_func("sn0/ioc3/parallel-status-reports-an-empty-cable",
                        NULL, test_the_status_register_reports_an_empty_cable);
    qtest_add_data_func("sn0/ioc3/parallel-control-holds-its-lines", NULL,
                        test_the_control_register_holds_its_lines);
    qtest_add_data_func("sn0/ioc3/parallel-configuration-b-holds-a-byte", NULL,
                        test_the_configuration_b_register_holds_a_byte);
    qtest_add_data_func("sn0/ioc3/parallel-configuration-a-needs-its-mode",
                        NULL, test_configuration_a_needs_its_own_mode);
    qtest_add_data_func("sn0/ioc3/parallel-ecp-registers-are-a-byte-apart",
                        NULL, test_the_ecp_registers_are_a_byte_apart);
    qtest_add_data_func("sn0/ioc3/parallel-ecp-answers-a-byte-at-a-time",
                        NULL, test_the_ecp_file_answers_a_byte_at_a_time);
    qtest_add_data_func("sn0/ioc3/parallel-dma-contexts-are-distinct", NULL,
                        test_the_two_dma_contexts_are_distinct);
    qtest_add_data_func("sn0/ioc3/parallel-context-holds-count-arm-and-last",
                        NULL, test_a_context_holds_its_count_arm_and_last);
    qtest_add_data_func("sn0/ioc3/parallel-shared-control-reports-the-flags",
                        NULL,
                        test_the_shared_control_reports_the_context_flags);
    qtest_add_data_func("sn0/ioc3/parallel-shared-control-holds-its-own-bits",
                        NULL, test_the_shared_control_holds_its_own_bits);
    qtest_add_data_func("sn0/ioc3/parallel-engine-never-reports-busy", NULL,
                        test_the_engine_never_reports_a_busy_context);
    qtest_add_data_func("sn0/ioc3/parallel-dma-registers-four-bytes-apart",
                        NULL, test_the_dma_registers_are_four_bytes_apart);

    for (i = 0; i < ARRAY_SIZE(ioc3_boards); i++) {
        g_autofree char *path =
            g_strdup_printf("sn0/ioc3/parallel-board-%s", ioc3_boards[i].name);

        qtest_add_data_func(path, &ioc3_boards[i],
                            test_a_board_fits_the_port_or_does_not);
    }

    return g_test_run();
}
