/*
 * IOC3 parallel-port tests: four DMA bytes at main-block 0x80 and the
 * byte-wide IEEE-1284/ECP file at SuperIO 0x20150. Both must be mapped for
 * an attached driver to transfer data. IRIX probes the extended-control
 * reset value; it must report transmit FIFO empty and not full or the driver
 * never attaches. Console boot does not exercise the port.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-test.h"

/* I/O space, and the chip's window inside the console widget. */
#define CONSOLE_WIDGET  8
#define BRIDGE_DEVICE2  0x600000
#define BRIDGE_DEVICE3  0x700000
#define IOC3_BASE       (SPACE_IO + ((uint64_t)CONSOLE_WIDGET << 24) + \
                         BRIDGE_DEVICE2)
#define TEST_IOC3_BASE  (SPACE_IO + ((uint64_t)CONSOLE_WIDGET << 24) + \
                         BRIDGE_DEVICE3)

/*
 * A guest's bus scan: assign the controller a PCI address, let it master
 * the bus so its DMA engines can reach memory, and aim the bridge's device
 * window at it. The XIO-side offset stays fixed.
 */
#define BRIDGE_BASE     (SPACE_IO + ((uint64_t)CONSOLE_WIDGET << 24))
#define REG_DATA        4         /* registers sit at a slot's upper half */
#define BRIDGE_CFG0     0x020000
#define CFG0_DEVICE_STRIDE 0x1000
#define CFG0(device)    (BRIDGE_BASE + BRIDGE_CFG0 + \
                         (device) * CFG0_DEVICE_STRIDE)
#define BRIDGE_DEVICE(n) (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)

#define PCI_IOC3_DEVICE  2
#define TEST_IOC3_DEVICE 3
#define IOC3_PCI_MMIO    0x10000000u
#define TEST_IOC3_PCI_MMIO 0x18000000u

#define PCI_SUBSYSTEM    0x2c
#define SUBSYSTEM_SGI    0x10a9u  /* the vendor half, the same on every board */
#define SUBSYSTEM(id)    (((uint32_t)(id) << 16) | SUBSYSTEM_SGI)

static void sn0_ioc3_attach(QTestState *qts)
{
    qtest_writel(qts, CFG0(PCI_IOC3_DEVICE) + PCI_BAR0, IOC3_PCI_MMIO);
    qtest_writel(qts, CFG0(PCI_IOC3_DEVICE) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DEVICE(PCI_IOC3_DEVICE),
                 DEV_MEMORY_SPACE | (IOC3_PCI_MMIO >> 20));
}

static void test_ioc3_attach(QTestState *qts)
{
    qtest_writel(qts, CFG0(TEST_IOC3_DEVICE) + PCI_BAR0,
                 TEST_IOC3_PCI_MMIO);
    qtest_writel(qts, CFG0(TEST_IOC3_DEVICE) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DEVICE(TEST_IOC3_DEVICE),
                 DEV_MEMORY_SPACE | (TEST_IOC3_PCI_MMIO >> 20));
}

/*
 * The chip's interrupt block, where the port's four sources live: its
 * service interrupt at bit 18, one per DMA context above that, then the
 * DMA fault.
 */
#define INT_STATUS       (IOC3_BASE + 0x01c)
#define INT_PAR_SERVICE  (1u << 18)
#define INT_PAR_CONTEXT_A (1u << 19)
#define INT_PAR_CONTEXT_B (1u << 20)
#define INT_PAR_ERROR    (1u << 21)
#define INT_PARALLEL     (INT_PAR_SERVICE | INT_PAR_CONTEXT_A | \
                          INT_PAR_CONTEXT_B | INT_PAR_ERROR)

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

#define MODE_COMPAT       0    /* compatibility, no FIFO (reset mode) */
#define MODE_BYTE         1
#define MODE_COMPAT_FIFO  2    /* what the driver selects for itself */
#define MODE_ECP          3
#define MODE_CONFIG       7

/*
 * The status register's peripheral lines. Only nFault's polarity is
 * specified: a clear level means a fault, so an idle port with no peripheral
 * on the cable reports no fault and nothing else.
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
    QTestState *qts = qtest_init("-M origin200");

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
    QTestState *qts = qtest_init("-M origin200");

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
    QTestState *qts = qtest_init("-M origin200");

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
    QTestState *qts = qtest_init("-M origin200");
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
    QTestState *qts = qtest_init("-M origin200");
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
    QTestState *qts = qtest_init("-M origin200");

    sn0_ioc3_attach(qts);

    g_assert_cmphex(qtest_readb(qts, ECP_DATA), ==, 0);

    qtest_writeb(qts, ECP_DATA, 0x5a);
    g_assert_cmphex(qtest_readb(qts, ECP_DATA), ==, 0x5a);

    qtest_quit(qts);
}

/*
 * The status register reflects the cable rather than storing a value, and
 * this machine has nothing on the cable. Only nFault's level is specified,
 * where clear means a fault. Thus an empty cable reports no fault, and
 * nothing else: no peripheral selected, no paper error, and never busy, which
 * is the level a driver polling for the printer to finish would otherwise
 * wait on forever.
 */
static void test_the_status_register_reports_an_empty_cable(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

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
    QTestState *qts = qtest_init("-M origin200");

    sn0_ioc3_attach(qts);

    /* nStrobe, nAutoFeed and nSelectIn together, nInit left clear. */
    qtest_writeb(qts, ECP_CONTROL, 0x0b);
    g_assert_cmphex(qtest_readb(qts, ECP_CONTROL), ==, 0x0b);

    /* Reading from the peripheral, with the handshake lines released. */
    qtest_writeb(qts, ECP_CONTROL, 0x20);
    g_assert_cmphex(qtest_readb(qts, ECP_CONTROL), ==, 0x20);

    qtest_quit(qts);
}

/* The configuration-B register, with no specified mode. */
static void test_the_configuration_b_register_holds_a_byte(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    sn0_ioc3_attach(qts);

    qtest_writeb(qts, ECP_CONFIG_B, 0xa5);
    g_assert_cmphex(qtest_readb(qts, ECP_CONFIG_B), ==, 0xa5);

    qtest_quit(qts);
}

/*
 * One address, two registers: 0x158 is the configuration-A register in
 * configuration mode and the data FIFO in every other mode. A model with
 * one storage word behind the address fails on the second half of this:
 * the FIFO write would be readable back as configuration.
 */
static void test_configuration_a_needs_its_own_mode(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

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
    QTestState *qts = qtest_init("-M origin200");

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
 * rather than one. Nothing rejects the wide access. The address space
 * splits it into byte reads on the way in, because the region says a byte
 * is the widest it takes. Thus what this asserts is that every lane comes
 * from its own register. A model declaring a four-byte implementation
 * width answers the whole access out of the first register instead, which
 * on the write side would run the FIFO's side effect once for four bytes.
 */
static void test_the_ecp_file_answers_a_byte_at_a_time(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
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
    QTestState *qts = qtest_init("-M origin200");

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
    QTestState *qts = qtest_init("-M origin200");

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
    QTestState *qts = qtest_init("-M origin200");

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
 * the engine enable. The reset level remains held,
 * so it reads back while it is held rather than clearing itself.
 */
static void test_the_shared_control_holds_its_own_bits(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

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
 * The engine takes a context only when both enables are set, and the
 * shared control register's own enable is one of two: the other is the DMA
 * path bit in the extended control register, which is untouched here. So
 * the contexts stay armed and the busy flags stay clear.
 *
 * A driver reads the busy flags to find which context was active when a
 * transfer failed, and a model reporting one busy forever would send it to
 * recover a transfer that never started.
 */
static void test_the_engine_never_reports_a_busy_context(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

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
 * A buffer in low memory, at the address the serial DMA tests use for their
 * rings, and the nine bytes IRIX writes when a shell redirects an echo to
 * the port.
 */
#define PLP_BUFFER       0x00100000u
#define PLP_PAYLOAD      "PLPHELLO\n"
#define PLP_PAYLOAD_LEN  (sizeof(PLP_PAYLOAD) - 1)

/*
 * The port with a file behind it, so a test can read what was printed. The
 * caller owns the returned path and unlinks it.
 */
static QTestState *plp_start_with_a_file(char **path)
{
    g_autofree char *args = NULL;
    int fd = g_file_open_tmp("qtest-sn0-plp-XXXXXX", path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);

    args = g_strdup_printf("-M origin200 "
                           "-chardev file,id=plp0,path=%s "
                           "-parallel chardev:plp0", *path);
    return qtest_init(args);
}

static void plp_assert_the_file_holds(const char *path, const char *expected)
{
    g_autofree char *printed = NULL;
    gsize len = 0;

    g_assert_true(g_file_get_contents(path, &printed, &len, NULL));
    g_assert_cmpint(len, ==, strlen(expected));
    g_assert_cmpint(memcmp(printed, expected, len), ==, 0);
}

/*
 * The transfer IRIX programs for a nine-byte write, register for register
 * and in its order: the buffer address, a count armed and marked last,
 * compatibility-FIFO mode with the DMA path and the fault interrupt
 * enabled, and the engine enabled. The driver then waits. So the engine has
 * to retire the context by itself, and what it must leave behind is the
 * count consumed, the context disarmed, the bytes out of the port, and the
 * service interrupt raised.
 *
 * A model that stores these registers and moves nothing is the machine on
 * which that write never returns; a model that moves the bytes and raises
 * nothing prints them and never returns either, which is measured rather
 * than argued: IRIX stayed blocked with this buffer already on the host.
 */
static void test_the_engine_retires_an_armed_transfer(const void *data)
{
    g_autofree char *path = NULL;
    QTestState *qts = plp_start_with_a_file(&path);

    sn0_ioc3_attach(qts);

    qtest_memwrite(qts, PLP_BUFFER, PLP_PAYLOAD, PLP_PAYLOAD_LEN);

    qtest_writel(qts, DMA_A_ADDR_HI, 0);
    qtest_writel(qts, DMA_A_ADDR_LO, PLP_BUFFER);
    qtest_writel(qts, DMA_A_COUNT, COUNT_ARM | COUNT_LAST | PLP_PAYLOAD_LEN);
    qtest_writel(qts, DMA_CONTROL, DMA_ENABLE);
    qtest_writeb(qts, ECP_EXTENDED,
                 ECR_MODE(MODE_COMPAT_FIFO) | ECR_DMA_ENABLE | ECR_NFAULT_INT);

    /* Nothing outstanding: the count field reads back what is left. */
    g_assert_cmphex(qtest_readl(qts, DMA_A_COUNT) & COUNT_MASK, ==, 0);

    /* Disarmed, from both addresses that report it. */
    g_assert_cmphex(qtest_readl(qts, DMA_A_COUNT) & COUNT_ARM, ==, 0);
    g_assert_cmphex(qtest_readl(qts, DMA_CONTROL) & (DMA_A_ARMED | DMA_A_BUSY),
                    ==, 0);

    /* The bytes are on the far side of the port, in order. */
    plp_assert_the_file_holds(path, PLP_PAYLOAD);

    /* The chain ended here, so both its own source and the service one. */
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_PARALLEL, ==,
                    INT_PAR_SERVICE | INT_PAR_CONTEXT_A);

    /*
     * The driver's acknowledgement, in its order. The context's source
     * latches and a write to the status register clears it; the service
     * source is a level and a write there cannot, which is why the driver
     * quiets the port's own register first.
     */
    qtest_writel(qts, INT_STATUS, INT_PAR_CONTEXT_A | INT_PAR_SERVICE);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_PARALLEL, ==,
                    INT_PAR_SERVICE);

    qtest_writeb(qts, ECP_EXTENDED, ECR_MODE(MODE_COMPAT_FIFO));
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_PARALLEL, ==, 0);

    qtest_quit(qts);
    unlink(path);
}

/*
 * The service interrupt reports the end of a chain, not the end of any one
 * context: a descriptor that is not marked last retires and raises its own
 * source alone. A model raising the service interrupt per descriptor would
 * wake the driver while its transfer still had buffers to go.
 */
static void test_only_the_last_context_completes_the_chain(const void *data)
{
    g_autofree char *path = NULL;
    QTestState *qts = plp_start_with_a_file(&path);

    sn0_ioc3_attach(qts);

    qtest_memwrite(qts, PLP_BUFFER, PLP_PAYLOAD, PLP_PAYLOAD_LEN);

    qtest_writel(qts, DMA_A_ADDR_LO, PLP_BUFFER);
    qtest_writel(qts, DMA_A_COUNT, COUNT_ARM | PLP_PAYLOAD_LEN);
    qtest_writel(qts, DMA_CONTROL, DMA_ENABLE);
    qtest_writeb(qts, ECP_EXTENDED,
                 ECR_MODE(MODE_COMPAT_FIFO) | ECR_DMA_ENABLE);

    g_assert_cmphex(qtest_readl(qts, DMA_A_COUNT) & COUNT_ARM, ==, 0);
    plp_assert_the_file_holds(path, PLP_PAYLOAD);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_PARALLEL, ==,
                    INT_PAR_CONTEXT_A);

    qtest_quit(qts);
    unlink(path);
}

/*
 * Both enables, separately. The engine sits still with either one missing,
 * so a model that ran on the shared control register alone would start a
 * transfer the driver has not finished setting up.
 */
static void test_the_engine_needs_both_enables(const void *data)
{
    g_autofree char *path = NULL;
    QTestState *qts = plp_start_with_a_file(&path);

    sn0_ioc3_attach(qts);

    qtest_memwrite(qts, PLP_BUFFER, PLP_PAYLOAD, PLP_PAYLOAD_LEN);

    /* The DMA path enabled in the register file, the engine not enabled. */
    qtest_writel(qts, DMA_A_ADDR_LO, PLP_BUFFER);
    qtest_writel(qts, DMA_A_COUNT, COUNT_ARM | COUNT_LAST | PLP_PAYLOAD_LEN);
    qtest_writeb(qts, ECP_EXTENDED,
                 ECR_MODE(MODE_COMPAT_FIFO) | ECR_DMA_ENABLE);
    g_assert_cmphex(qtest_readl(qts, DMA_A_COUNT), ==,
                    COUNT_ARM | COUNT_LAST | PLP_PAYLOAD_LEN);

    /* The engine enabled, the DMA path taken back out of the register file. */
    qtest_writeb(qts, ECP_EXTENDED, ECR_MODE(MODE_COMPAT_FIFO));
    qtest_writel(qts, DMA_CONTROL, DMA_ENABLE);
    g_assert_cmphex(qtest_readl(qts, DMA_A_COUNT), ==,
                    COUNT_ARM | COUNT_LAST | PLP_PAYLOAD_LEN);

    /* Nothing left the port while either enable was missing. */
    plp_assert_the_file_holds(path, "");

    /* Both, and it goes. */
    qtest_writeb(qts, ECP_EXTENDED,
                 ECR_MODE(MODE_COMPAT_FIFO) | ECR_DMA_ENABLE);
    g_assert_cmphex(qtest_readl(qts, DMA_A_COUNT) & COUNT_ARM, ==, 0);
    plp_assert_the_file_holds(path, PLP_PAYLOAD);

    qtest_quit(qts);
    unlink(path);
}

/*
 * A chain across both contexts, which is how the driver moves anything
 * longer than one descriptor: it loads one context while the other drains
 * and marks only the end of the chain last. The engine has to take them in
 * the order they were loaded, or a printed buffer comes out scrambled.
 */
static void test_the_engine_takes_a_chain_in_order(const void *data)
{
    g_autofree char *path = NULL;
    QTestState *qts = plp_start_with_a_file(&path);

    sn0_ioc3_attach(qts);

    qtest_memwrite(qts, PLP_BUFFER, "FIRST", 5);
    qtest_memwrite(qts, PLP_BUFFER + 0x1000, "SECOND", 6);

    qtest_writel(qts, DMA_A_ADDR_LO, PLP_BUFFER);
    qtest_writel(qts, DMA_A_COUNT, COUNT_ARM | 5);
    qtest_writel(qts, DMA_B_ADDR_LO, PLP_BUFFER + 0x1000);
    qtest_writel(qts, DMA_B_COUNT, COUNT_ARM | COUNT_LAST | 6);
    qtest_writel(qts, DMA_CONTROL, DMA_ENABLE);
    qtest_writeb(qts, ECP_EXTENDED,
                 ECR_MODE(MODE_COMPAT_FIFO) | ECR_DMA_ENABLE);

    g_assert_cmphex(qtest_readl(qts, DMA_CONTROL) & 0x3f, ==, DMA_B_LAST);
    plp_assert_the_file_holds(path, "FIRSTSECOND");

    /* Both contexts drained, and the last of them ended the chain. */
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_PARALLEL, ==,
                    INT_PAR_SERVICE | INT_PAR_CONTEXT_A | INT_PAR_CONTEXT_B);

    qtest_quit(qts);
    unlink(path);
}

/*
 * A descriptor pointing where no memory answers. The engine refuses the
 * fetch rather than printing whatever it read, raises the port's DMA fault,
 * and leaves the context armed with its count intact. This is the
 * residual the driver recovers a failed transfer from.
 */
static void test_an_unreadable_buffer_faults_and_keeps_its_count(
    const void *data)
{
    g_autofree char *path = NULL;
    QTestState *qts = plp_start_with_a_file(&path);

    sn0_ioc3_attach(qts);

    /* Past the far end of this machine's memory. */
    qtest_writel(qts, DMA_A_ADDR_HI, 0x0000ffff);
    qtest_writel(qts, DMA_A_ADDR_LO, 0xffff0000);
    qtest_writel(qts, DMA_A_COUNT, COUNT_ARM | COUNT_LAST | PLP_PAYLOAD_LEN);
    qtest_writel(qts, DMA_CONTROL, DMA_ENABLE);
    qtest_writeb(qts, ECP_EXTENDED,
                 ECR_MODE(MODE_COMPAT_FIFO) | ECR_DMA_ENABLE);

    g_assert_cmphex(qtest_readl(qts, DMA_A_COUNT), ==,
                    COUNT_ARM | COUNT_LAST | PLP_PAYLOAD_LEN);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_PARALLEL, ==,
                    INT_PAR_ERROR);
    plp_assert_the_file_holds(path, "");

    qtest_quit(qts);
    unlink(path);
}

/*
 * The DMA registers are four bytes apart in a block whose neighbours,
 * the pin registers below it and the PS/2 controller above, are live
 * registers of their own. Distinct values at three consecutive offsets
 * prove the stride, and the read back through the PS/2 controller's first
 * register proves the block stops where it should.
 */
static void test_the_dma_registers_are_four_bytes_apart(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

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
        g_strdup_printf("-M origin200 "
                        "-device sgi-ioc3,addr=3,subsystem-id=0x%04x",
                        board->subsystem_id);
    QTestState *qts = qtest_init(args);

    test_ioc3_attach(qts);

    /* The identifier a guest dispatches on, before what it selects. */
    g_assert_cmphex(qtest_readl(qts, CFG0(TEST_IOC3_DEVICE) + PCI_SUBSYSTEM),
                    ==, SUBSYSTEM(board->subsystem_id));

    qtest_writeb(qts, TEST_IOC3_BASE + 0x20150, 0x5a);
    g_assert_cmpint(qtest_readb(qts, TEST_IOC3_BASE + 0x20150) == 0x5a,
                    ==, board->parallel);

    qtest_writel(qts, TEST_IOC3_BASE + 0x084, 0x11112000);
    g_assert_cmpint(qtest_readl(qts, TEST_IOC3_BASE + 0x084) == 0x11112000,
                    ==, board->parallel);

    qtest_quit(qts);
}

/*
 * The Origin 2000 BaseIO-G, whose parallel port is on the MediaIO
 * daughtercard's IOC3 at Bridge device 6 rather than on the board IOC3 at
 * device 2. The BaseIO's Bridge answers at the same widget as the Origin
 * 200's, so the same XIO addresses reach it.
 */
#define IO6G_MACHINE     "-M origin2000,baseio-preset=io6g -smp 8 " \
                         "-display none"
#define IO6_MACHINE      "-M origin2000 -smp 8 -display none"
#define MIO_IOC3_DEVICE  6
#define MIO_IOC3_PCI_MMIO 0x11000000u
#define BRIDGE_DEVICE6   0xa00000
#define MIO_IOC3_BASE    (BRIDGE_BASE + BRIDGE_DEVICE6)
#define MIO_ECP_FIFO     (MIO_IOC3_BASE + 0x20150 + 0x08)
#define MIO_ECP_EXTENDED (MIO_IOC3_BASE + 0x20150 + 0x0a)

static void mio_ioc3_attach(QTestState *qts)
{
    qtest_writel(qts, CFG0(MIO_IOC3_DEVICE) + PCI_BAR0, MIO_IOC3_PCI_MMIO);
    qtest_writel(qts, CFG0(MIO_IOC3_DEVICE) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DEVICE(MIO_IOC3_DEVICE),
                 DEV_MEMORY_SPACE | (MIO_IOC3_PCI_MMIO >> 20));
}

/* The driver's probe, answered by the MediaIO chip. */
static void test_io6g_mio_presence_check_reads_out_of_reset(const void *data)
{
    QTestState *qts = qtest_init(IO6G_MACHINE);

    mio_ioc3_attach(qts);

    g_assert_cmphex(qtest_readb(qts, MIO_ECP_EXTENDED), ==, ECR_FIFO_EMPTY);

    qtest_quit(qts);
}

/* The same hardware-owned FIFO flags as the Origin 200's port. */
static void test_io6g_mio_fifo_flags_are_not_writable(const void *data)
{
    QTestState *qts = qtest_init(IO6G_MACHINE);

    mio_ioc3_attach(qts);

    qtest_writeb(qts, MIO_ECP_EXTENDED, ECR_FIFO_FULL);
    g_assert_cmphex(qtest_readb(qts, MIO_ECP_EXTENDED), ==, ECR_FIFO_EMPTY);

    qtest_writeb(qts, MIO_ECP_EXTENDED, 0);
    g_assert_cmphex(qtest_readb(qts, MIO_ECP_EXTENDED), ==, ECR_FIFO_EMPTY);

    qtest_quit(qts);
}

/*
 * "-parallel" reaches the MediaIO chip's port: bytes written to its
 * transmit FIFO land in the host file. Without the machine binding the
 * port has no backend and the file stays empty.
 */
static void test_io6g_mio_fifo_reaches_the_host(const void *data)
{
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    int fd = g_file_open_tmp("qtest-sn0-mio-plp-XXXXXX", &path, NULL);
    QTestState *qts;
    size_t i;

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    args = g_strdup_printf(IO6G_MACHINE " -chardev file,id=plp0,path=%s "
                           "-parallel chardev:plp0", path);
    qts = qtest_init(args);

    mio_ioc3_attach(qts);
    qtest_writeb(qts, MIO_ECP_EXTENDED, ECR_MODE(MODE_COMPAT_FIFO));
    for (i = 0; i < PLP_PAYLOAD_LEN; i++) {
        qtest_writeb(qts, MIO_ECP_FIFO, PLP_PAYLOAD[i]);
    }

    plp_assert_the_file_holds(path, PLP_PAYLOAD);

    qtest_quit(qts);
    unlink(path);
}

/*
 * The plain BaseIO's only IOC3 fits no parallel port, so its register
 * file swallows a write and reads zero. Its
 * serial A scratch register holding a byte first shows the window is
 * attached, so the zero is the missing port and not a missing chip.
 */
#define IO6_SERIAL_A_SCRATCH (IOC3_BASE + 0x020178 + 7)

static void test_io6_board_ioc3_fits_no_port(const void *data)
{
    QTestState *qts = qtest_init(IO6_MACHINE);

    sn0_ioc3_attach(qts);

    qtest_writeb(qts, IO6_SERIAL_A_SCRATCH, 0xa5);
    g_assert_cmphex(qtest_readb(qts, IO6_SERIAL_A_SCRATCH), ==, 0xa5);

    qtest_writeb(qts, IOC3_BASE + 0x20150, 0x5a);
    g_assert_cmphex(qtest_readb(qts, IOC3_BASE + 0x20150), ==, 0);

    qtest_quit(qts);
}

/*
 * A host backend aimed at that missing port is refused rather than left
 * connected to nothing.
 */
static void test_io6_refuses_a_parallel_backend(const void *data)
{
    QTestState *qts;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*fits no parallel port*");
        return;
    }
    qts = qtest_init(IO6_MACHINE " -chardev null,id=plp0 "
                     "-global sgi-xio-baseio.parallel-chardev=plp0 -S");
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
    qtest_add_data_func("sn0/ioc3/parallel-engine-retires-a-transfer", NULL,
                        test_the_engine_retires_an_armed_transfer);
    qtest_add_data_func("sn0/ioc3/parallel-engine-needs-both-enables", NULL,
                        test_the_engine_needs_both_enables);
    qtest_add_data_func("sn0/ioc3/parallel-engine-takes-a-chain-in-order",
                        NULL, test_the_engine_takes_a_chain_in_order);
    qtest_add_data_func("sn0/ioc3/parallel-only-the-last-context-completes",
                        NULL, test_only_the_last_context_completes_the_chain);
    qtest_add_data_func("sn0/ioc3/parallel-unreadable-buffer-faults", NULL,
                        test_an_unreadable_buffer_faults_and_keeps_its_count);
    qtest_add_data_func("sn0/ioc3/parallel-dma-registers-four-bytes-apart",
                        NULL, test_the_dma_registers_are_four_bytes_apart);

    qtest_add_data_func("sn0/ioc3/parallel-io6g-mio-presence-check", NULL,
                        test_io6g_mio_presence_check_reads_out_of_reset);
    qtest_add_data_func("sn0/ioc3/parallel-io6g-mio-fifo-flags", NULL,
                        test_io6g_mio_fifo_flags_are_not_writable);
    qtest_add_data_func("sn0/ioc3/parallel-io6g-mio-fifo-reaches-the-host",
                        NULL, test_io6g_mio_fifo_reaches_the_host);
    qtest_add_data_func("sn0/ioc3/parallel-io6-board-ioc3-fits-no-port", NULL,
                        test_io6_board_ioc3_fits_no_port);
    qtest_add_data_func("sn0/ioc3/parallel-io6-refuses-a-backend", NULL,
                        test_io6_refuses_a_parallel_backend);

    for (i = 0; i < ARRAY_SIZE(ioc3_boards); i++) {
        g_autofree char *path =
            g_strdup_printf("sn0/ioc3/parallel-board-%s", ioc3_boards[i].name);

        qtest_add_data_func(path, &ioc3_boards[i],
                            test_a_board_fits_the_port_or_does_not);
    }

    return g_test_run();
}
