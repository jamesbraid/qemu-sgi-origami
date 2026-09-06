/*
 * Whether device state comes back correct after a migration, including
 * what a .post_load hook re-derives rather than carries in the stream.
 *
 * Deliberately not on tests/qtest/migration/: that framework proves a
 * running guest survives a live migration, and qtest never executes guest
 * code, so a stopped machine's device state is the whole question. The
 * migrate-and-poll steps follow tests/qtest/nvme-test.c, which stands
 * outside the framework for the same reason.
 *
 * The founding case is the Bridge device window, whose aim is derived
 * from the per-slot register on load: the register's bits surviving
 * (VMSTATE_UINT32_ARRAY guarantees that) is a weaker claim than an access
 * through the window still reaching the card, which only a correct
 * .post_load leaves true.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

/* This file's own copies, the way every sn0-*-test.c stands alone. */
#define SPACE_XTALK             0x20000000000ULL
#define NODE_STRIDE             0x100000000ULL
#define BRIDGE_WIDGET           8
#define BRIDGE_BASE             (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))
#define BRIDGE_BASE_FOR_NODE(n) (BRIDGE_BASE + (uint64_t)(n) * NODE_STRIDE)
#define REG_DATA                4
#define REG(offset)             (BRIDGE_BASE + (offset) + REG_DATA)
#define BRIDGE_DEVICE(n)        (0x200 + 8 * (n))
#define DEV_MEMORY_SPACE        (1u << 12)

#define MACHINE_WITH_BRIDGE     "-M origin200"
#define MACHINE                 "-M origin200"
#define MACHINE_RECORDING       "-M origin200,diagnostic-recording=on"

/* The Hub's boot-flash view and AMD command offsets. */
#define SPACE_HUB               0x10000000000ULL
#define FLASH_RESET             0x1fc00000ULL
#define FLASH_HUB               (SPACE_HUB + 0x10000000)
#define FLASH_UNLOCK1           0x5555
#define FLASH_UNLOCK2           0x2aaa
#define FLASH_PROGRAM_TARGET    0xe0100

#define HUB_LOCAL               (SPACE_XTALK + 0x01000000)
#define NI_SCRATCH_0            0x600100
#define NI_SCRATCH_1            0x600108
#define NI_META_TABLE_LAST      0x6380f8
#define NI_LOCAL_TABLE          0x638100

/* The block transfer engines' registers and fields. */
#define BTE_LENGTH(e)           (0x410000 + (e) * 0x10000)
#define BTE_SOURCE(e)           (BTE_LENGTH(e) + 0x08)
#define BTE_DEST(e)             (BTE_LENGTH(e) + 0x10)
#define BTE_CONTROL(e)          (BTE_LENGTH(e) + 0x18)
#define BTE_NOTIFY(e)           (BTE_LENGTH(e) + 0x20)
#define BTE_INTERRUPT(e)        (BTE_LENGTH(e) + 0x28)
#define BTE_BUSY                (1ULL << 20)
#define BTE_CTL_NOTIFY          (1ULL << 4)
#define BTE_INT_LEVEL(l)        ((uint64_t)(l) << 16)
#define BTE_LINE                128
#define PI_INT_PEND0            0x000098
#define PI_REGION_PRESENT       0x000018

/* PCF8584 data and control registers inside the Hub-local window. */
#define I2C_DATA                0x220000
#define I2C_CTRL                0x220008
#define I2C_CTL_PIN             0x80
#define I2C_CTL_ESO             0x40
#define I2C_CTL_STA             0x04
#define I2C_CTL_STO             0x02
#define I2C_CTL_ACK             0x01
#define I2C_STS_LRB             0x08
#define ELSC_ADDR               0x08

/* PCI type 0 configuration space, and the window each device sits behind. */
#define BRIDGE_CFG0             0x020000
#define CFG0_DEVICE_STRIDE      0x1000
#define CFG0_FUNCTION_STRIDE    0x100
#define CFG0(device, function)  (BRIDGE_BASE + BRIDGE_CFG0 + \
                                 (device) * CFG0_DEVICE_STRIDE + \
                                 (function) * CFG0_FUNCTION_STRIDE)
#define CFG0_FOR_NODE(node, device) \
    (BRIDGE_BASE_FOR_NODE(node) + BRIDGE_CFG0 + \
     (device) * CFG0_DEVICE_STRIDE)

/* Any card works for a window test; this one needs no backing or net. */
#define PROBE_SLOT              4
#define PROBE_CARD              " -device lsi53c895a,addr=" stringify(PROBE_SLOT)

#define PCI_COMMAND             0x04
#define PCI_COMMAND_MEMORY      0x0002
#define PCI_BAR0                0x10
#define PCI_BAR1                0x14

/* A 32-bit scratch register in the card's memory-mapped register file. */
#define CARD_SCRATCH            0x34

/* Sixteen megabytes apart, matching sn0-bridge-test.c's own layout. */
#define CARD_MMIO(slot)         (0x10000000u + (slot) * 0x1000000u)

/* Window 1 is the second 2 MB window, and is where slot 4 lands on it. */
#define PROBE_WINDOW             1
#define PROBE_WINDOW_OFFSET      0x400000
#define WINDOW(offset)           (BRIDGE_BASE + (offset))

#define IOC3_SLOT                2
#define IOC3_PCI_MMIO            0x10000000u
#define IOC3_WINDOW_OFFSET       0x600000
#define IOC3_SERIAL_A            0x020178
#define IOC3_SCRATCH             7
#define IOC3_SCRATCH_AT(node)    (BRIDGE_BASE_FOR_NODE(node) + \
                                  IOC3_WINDOW_OFFSET + IOC3_SERIAL_A + \
                                  IOC3_SCRATCH)

/*
 * The IOC3 serial DMA engine on node 0. The ring block sits in guest RAM;
 * with 4 KB rings the port A receive ring is the block's second page.
 */
#define IOC3_REG(offset)         (BRIDGE_BASE_FOR_NODE(0) + \
                                  IOC3_WINDOW_OFFSET + (offset))
#define IOC3_INT_STATUS          IOC3_REG(0x01c)
#define IOC3_SDMA_RING_HI        IOC3_REG(0x0b0)
#define IOC3_SDMA_RING_LO        IOC3_REG(0x0b4)
#define IOC3_SDMA_A_CONTROL      IOC3_REG(0x0b8)
#define IOC3_SDMA_A_TX_PRODUCE   IOC3_REG(0x0bc)
#define IOC3_SDMA_A_RX_PRODUCE   IOC3_REG(0x0c4)
#define IOC3_SDMA_A_RX_CONSUME   IOC3_REG(0x0c8)
#define IOC3_SDMA_A_RX_TIMER     IOC3_REG(0x0cc)
#define IOC3_SERIAL_A_FCR        IOC3_REG(IOC3_SERIAL_A + 2)
#define IOC3_SERIAL_A_LCR        IOC3_REG(IOC3_SERIAL_A + 3)
#define IOC3_SERIAL_A_MCR        IOC3_REG(IOC3_SERIAL_A + 4)
#define IOC3_SRC_A_RX_TIMER      (1u << 3)
#define IOC3_SDMA_ENABLE         (1u << 28)
#define IOC3_CONSUME_ARM_TIMER   (1u << 31)
#define IOC3_RING_4K             (1u << 0)
#define IOC3_RING_RAM            0x00100000u
#define IOC3_RX_RING_RAM         (IOC3_RING_RAM + 0x1000)
#define IOC3_RX_TIMER_TICK_NS    62500LL
#define PCI_COMMAND_MASTER       0x0004

#define DUAL_CHASSIS_TOPOLOGY \
    "-m 128M " \
    "-object memory-backend-ram,id=ram0,size=64M " \
    "-object memory-backend-ram,id=ram1,size=64M " \
    "-numa node,nodeid=0,cpus=0-1,memdev=ram0 " \
    "-numa node,nodeid=1,cpus=2-3,memdev=ram1 " \
    "-smp 4,sockets=2,cores=2,threads=1"
#define MACHINE_DUAL \
    "-M origin200,synthesized-firmware=off " DUAL_CHASSIS_TOPOLOGY

/* The board identity part's one-wire register and slot widths. */
#define BRIDGE_NIC               0x0b0
#define NIC_LEVEL                (1u << 0)
#define NIC_SAMPLE(us)           ((uint32_t)(us) << 2)
#define NIC_PULSE(us)            ((uint32_t)(us) << 10)
#define NIC_ROM_IDENTITY         0x360605040302010bULL
#define NIC_CRC_RESIDUE          0xb001

static void flash_command(QTestState *qts, uint32_t offset, uint8_t value)
{
    qtest_writeq(qts, FLASH_HUB + 8 * offset, value);
}

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

/*
 * Migrate and block until completed or failed. `-incoming` on the
 * destination's command line has it listening before its QMP handshake,
 * so `from` cannot connect too early.
 */
static void migrate_stopped_vm(QTestState *from, const char *uri)
{
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);

    g_test_timer_start();
    for (;;) {
        QDict *ret = qtest_qmp_assert_success_ref(from,
            "{ 'execute': 'query-migrate' }");
        g_autofree char *status = g_strdup(qdict_get_str(ret, "status"));

        qobject_unref(ret);
        g_assert_cmpstr(status, !=, "failed");
        if (g_str_equal(status, "completed")) {
            break;
        }

        g_assert(g_test_timer_elapsed() < 30);
        g_usleep(1000);
    }
}

static QDict *query_origin_state(QTestState *qts)
{
    return qtest_qmp_assert_success_ref(
        qts, "{'execute': 'x-query-origin-state', "
             "'arguments': {'event-limit': 4096}}");
}

static bool has_hub_write(QDict *state, uint64_t address, uint64_t value)
{
    QDict *recorder = qdict_get_qdict(state, "flight-recorder");
    const QListEntry *entry;

    for (entry = qlist_first(qdict_get_qlist(recorder, "events")); entry;
         entry = qlist_next(entry)) {
        QDict *event = qobject_to(QDict, qlist_entry_obj(entry));

        if (!strcmp(qdict_get_str(event, "component"), "hub") &&
            !strcmp(qdict_get_str(event, "operation"), "mmio-write") &&
            qdict_get_uint(event, "address") == address &&
            qdict_get_uint(event, "value") == value) {
            return true;
        }
    }
    return false;
}

static void attach_node_ioc3(QTestState *qts, unsigned int node)
{
    qtest_writel(qts, CFG0_FOR_NODE(node, IOC3_SLOT) + PCI_BAR0,
                 IOC3_PCI_MMIO);
    qtest_writel(qts, CFG0_FOR_NODE(node, IOC3_SLOT) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_BASE_FOR_NODE(node) + BRIDGE_DEVICE(IOC3_SLOT) +
                       REG_DATA,
                 DEV_MEMORY_SPACE | (IOC3_PCI_MMIO >> 20));
}

/* The Hub's network interface, as far as renumbering a stock machine needs. */
#define NI_STATUS               0x600000
#define NI_VECTOR_PARMS         0x600200
#define NI_VECTOR               0x600208
#define NI_VECTOR_DATA          0x600210
#define NI_REGION_SIZE          (1ULL << 17)
#define NI_NODE_MASK            (0x1ffULL << 8)
#define NI_PIOTYPE_WRITE        1
#define HUB_BY_NODE             (SPACE_XTALK + 0x01800000)

/*
 * A stock-PROM machine boots with every Hub programmed zero, so a
 * node-one-qualified address names no Hub. Program node 1's real number
 * the way the PROM's global master does, a vector write over the
 * CrayLink, so the test can reach node 1's devices by address. The
 * programmed number is Hub state: the destination re-derives its claim
 * from the migrated register, which this test then rides.
 */
static void assign_node1_number(QTestState *qts)
{
    qtest_writeq(qts, HUB_LOCAL + NI_VECTOR, 0);
    qtest_writeq(qts, HUB_LOCAL + NI_VECTOR_DATA,
                 NI_REGION_SIZE | (1ULL << 8));
    qtest_writeq(qts, HUB_LOCAL + NI_VECTOR_PARMS,
                 NI_STATUS | NI_PIOTYPE_WRITE);
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + NODE_STRIDE + NI_STATUS) &
                    NI_NODE_MASK, ==, 1ULL << 8);
}

/* Two equal devfns on different root buses must migrate independently. */
static void test_dual_base_io_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE_DUAL);
    assign_node1_number(from);
    attach_node_ioc3(from, 0);
    attach_node_ioc3(from, 1);
    qtest_writeb(from, IOC3_SCRATCH_AT(0), 0x5a);
    qtest_writeb(from, IOC3_SCRATCH_AT(1), 0xa5);
    g_assert_cmphex(qtest_readb(from, IOC3_SCRATCH_AT(0)), ==, 0x5a);
    g_assert_cmphex(qtest_readb(from, IOC3_SCRATCH_AT(1)), ==, 0xa5);

    dest_cmdline = g_strdup_printf(MACHINE_DUAL " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    g_assert_cmphex(qtest_readb(to, IOC3_SCRATCH_AT(0)), ==, 0x5a);
    g_assert_cmphex(qtest_readb(to, IOC3_SCRATCH_AT(1)), ==, 0xa5);

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * A wound receive timer's deadline lives in a QEMUTimer, which crosses
 * only if the timer is in the stream; lost in transit it is an interrupt
 * that never fires and a console that waits forever. The far side must
 * show the count still pending and still expire past the deadline.
 */
static void test_a_wound_receive_timer_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    static const uint8_t entry[8] = {
        'm', 'i', 'g', 'r', 0x40, 0x40, 0x40, 0x40,
    };
    uint8_t landed[8];
    unsigned int i;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE_WITH_BRIDGE);
    attach_node_ioc3(from, 0);
    /* Ring DMA is bus-mastering, which the register tests never need. */
    qtest_writel(from, CFG0_FOR_NODE(0, IOC3_SLOT) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    /*
     * Loopback delivers the transmit entry to the same port's receive
     * ring with no chardev in it; the armed timer is wound by delivery.
     */
    qtest_writeb(from, IOC3_SERIAL_A_LCR, 0x03);
    qtest_writeb(from, IOC3_SERIAL_A_FCR, 0x01);
    qtest_writeb(from, IOC3_SERIAL_A_MCR, 0x13);
    qtest_writel(from, IOC3_SDMA_RING_HI, 0);
    qtest_writel(from, IOC3_SDMA_RING_LO, IOC3_RING_RAM | IOC3_RING_4K);
    qtest_writel(from, IOC3_SDMA_A_RX_TIMER, 0x123);
    qtest_writel(from, IOC3_SDMA_A_RX_CONSUME, IOC3_CONSUME_ARM_TIMER);
    qtest_writel(from, IOC3_SDMA_A_CONTROL, IOC3_SDMA_ENABLE);
    qtest_memwrite(from, IOC3_RING_RAM, entry, sizeof(entry));
    qtest_writel(from, IOC3_SDMA_A_TX_PRODUCE, 8);

    /* Delivered and wound, and the idle time has not passed yet. */
    g_assert_cmphex(qtest_readl(from, IOC3_SDMA_A_RX_PRODUCE) & 0xff8, ==, 8);
    g_assert_cmphex((qtest_readl(from, IOC3_SDMA_A_RX_TIMER) >> 16) & 0xfff,
                    ==, 0x123);
    g_assert_cmphex(qtest_readl(from, IOC3_INT_STATUS) & IOC3_SRC_A_RX_TIMER,
                    ==, 0);

    dest_cmdline = g_strdup_printf(MACHINE_WITH_BRIDGE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    /* The ring, the index, and the still-counting timer all crossed. */
    qtest_memread(to, IOC3_RX_RING_RAM, landed, sizeof(landed));
    for (i = 0; i < 4; i++) {
        g_assert_cmphex(landed[i], ==, entry[i]);
        g_assert_cmphex(landed[4 + i] & 0x80, ==, 0x80);
    }
    g_assert_cmphex(qtest_readl(to, IOC3_SDMA_A_RX_PRODUCE) & 0xff8, ==, 8);
    g_assert_cmphex((qtest_readl(to, IOC3_SDMA_A_RX_TIMER) >> 16) & 0xfff,
                    ==, 0x123);
    g_assert_cmphex(qtest_readl(to, IOC3_INT_STATUS) & IOC3_SRC_A_RX_TIMER,
                    ==, 0);

    /* And it still expires: the deadline crossed, not just the count. */
    qtest_clock_step(to, (0x123 + 1) * IOC3_RX_TIMER_TICK_NS);
    g_assert_cmphex(qtest_readl(to, IOC3_INT_STATUS) & IOC3_SRC_A_RX_TIMER,
                    ==, IOC3_SRC_A_RX_TIMER);

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * Aim a window at a card, migrate, and require the same access to still
 * reach the card: without sgi_bridge_post_load() re-deriving the window's
 * aim, the destination comes up over the wrong PCI space -- observed by
 * mutating post_load to a no-op -- and the read returns the bus's
 * unanswered value.
 */
static void test_a_device_window_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE_WITH_BRIDGE PROBE_CARD);
    bridge_put_card_in_slot(from, PROBE_SLOT);
    bridge_aim_window_at_card(from, PROBE_WINDOW, PROBE_SLOT);

    qtest_writeq(from, HUB_LOCAL + NI_SCRATCH_0, 0x0123456789abcdefULL);
    qtest_writeq(from, HUB_LOCAL + NI_SCRATCH_1, 0xfedcba9876543210ULL);

    qtest_writel(from, WINDOW(PROBE_WINDOW_OFFSET) + CARD_SCRATCH, 0xc0ffee00);
    g_assert_cmphex(qtest_readl(from, WINDOW(PROBE_WINDOW_OFFSET) +
                                CARD_SCRATCH), ==, 0xc0ffee00);

    /* Same devices as the source, listening before its QMP handshake. */
    dest_cmdline = g_strdup_printf(MACHINE_WITH_BRIDGE PROBE_CARD
                                   " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    migrate_stopped_vm(from, uri);

    /*
     * The claim: the access still reaches the card, which only holds if
     * the derived state came back, not just the register.
     */
    g_assert_cmphex(qtest_readl(to, WINDOW(PROBE_WINDOW_OFFSET) +
                                CARD_SCRATCH), ==, 0xc0ffee00);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + NI_SCRATCH_0), ==,
                    0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + NI_SCRATCH_1), ==,
                    0xfedcba9876543210ULL);

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * Route entries are guest-programmed network state. Their initial value is
 * also zero, so a migration test must write non-zero values before the move;
 * otherwise a destination that silently drops the version-16 arrays passes.
 */
static void test_network_routes_survive_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE);
    qtest_writeq(from, HUB_LOCAL + NI_LOCAL_TABLE, 0x1b);
    qtest_writeq(from, HUB_LOCAL + NI_META_TABLE_LAST, 0x16);
    g_assert_cmphex(qtest_readq(from, HUB_LOCAL + NI_LOCAL_TABLE), ==, 0xb);
    g_assert_cmphex(qtest_readq(from, HUB_LOCAL + NI_META_TABLE_LAST), ==, 6);

    dest_cmdline = g_strdup_printf(MACHINE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + NI_LOCAL_TABLE), ==, 0xb);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + NI_META_TABLE_LAST), ==, 6);

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * A first START is split across two guest writes: the address reaches the
 * PCF8584 data register before the control write asserts START.  Migrate in
 * that interval, then advance the destination far enough for the ELSC's
 * arbitration timer to fire.  The staged-address state must move with the
 * byte or the next broadcast replaces it before START consumes it.
 */
static void test_staged_i2c_start_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE);
    qtest_writeq(from, HUB_LOCAL + I2C_CTRL, I2C_CTL_PIN | I2C_CTL_ESO);
    qtest_writeq(from, HUB_LOCAL + I2C_DATA, ELSC_ADDR << 1);
    g_assert_cmphex(qtest_readq(from, HUB_LOCAL + I2C_DATA), ==,
                    ELSC_ADDR << 1);

    dest_cmdline = g_strdup_printf(MACHINE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    qtest_clock_step(to, 500 * 1000);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + I2C_DATA), ==,
                    ELSC_ADDR << 1);
    qtest_writeq(to, HUB_LOCAL + I2C_CTRL,
                 I2C_CTL_PIN | I2C_CTL_ESO | I2C_CTL_STA | I2C_CTL_ACK);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + I2C_CTRL) & I2C_STS_LRB,
                    ==, 0);
    qtest_writeq(to, HUB_LOCAL + I2C_CTRL,
                 I2C_CTL_PIN | I2C_CTL_ESO | I2C_CTL_STO);

    qtest_quit(to);
    qtest_quit(from);
}

static bool nic_slot(QTestState *qts, unsigned int pulse, unsigned int sample)
{
    qtest_writel(qts, REG(BRIDGE_NIC), NIC_PULSE(pulse) | NIC_SAMPLE(sample));
    return qtest_readl(qts, REG(BRIDGE_NIC)) & NIC_LEVEL;
}

#define NIC_RESET(q)      nic_slot((q), 520, 65)
#define NIC_READ(q)       nic_slot((q), 6, 13)
#define NIC_WRITE_ONE(q)  nic_slot((q), 6, 13)
#define NIC_WRITE_ZERO(q) nic_slot((q), 80, 30)

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

static uint8_t nic_read_byte(QTestState *qts)
{
    uint8_t byte = 0;
    unsigned int i;

    for (i = 0; i < 8; i++) {
        if (NIC_READ(qts)) {
            byte |= 1u << i;
        }
    }
    return byte;
}

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

/*
 * The board identity part is driven one bit per register write, so its
 * conversation state lives between accesses and in no register -- a
 * snapshot at rest restores it correctly by accident. Read half a page on
 * the source and half on the destination, checked as one page: the part
 * number spans the boundary and the page's check covers all of it, so a
 * destination that forgot the conversation answers the idle wire (ones)
 * and cannot pass. Snapshot-at-rest-only migration testing certifies a
 * weaker property than the one that must hold.
 */
static void test_a_conversation_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint8_t page[32];
    unsigned int i;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE_WITH_BRIDGE);

    /* Address the part by name, then ask for the first page. */
    g_assert_false(NIC_RESET(from));
    nic_write_byte(from, 0x55);
    for (i = 0; i < 64; i++) {
        if ((NIC_ROM_IDENTITY >> i) & 1) {
            NIC_WRITE_ONE(from);
        } else {
            NIC_WRITE_ZERO(from);
        }
    }
    nic_write_byte(from, 0xf0);
    nic_write_byte(from, 0x00);
    nic_write_byte(from, 0x00);

    /* Half the page here... */
    for (i = 0; i < 16; i++) {
        page[i] = nic_read_byte(from);
    }

    dest_cmdline = g_strdup_printf(MACHINE_WITH_BRIDGE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    migrate_stopped_vm(from, uri);

    /* ...and the rest there, mid-byte-stream, with nothing re-sent. */
    for (i = 16; i < sizeof(page); i++) {
        page[i] = nic_read_byte(to);
    }

    /* One page, not bytes: the whole value rather than a property of it. */
    g_assert_cmpmem(&page[11], 12, "030-1244-001", 12);
    g_assert_cmphex(nic_crc(0, page, sizeof(page)), ==, NIC_CRC_RESIDUE);

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * BTE registers are the whole of an engine's state, but bit-pattern
 * survival is the weak half: an engine armed on the source must launch on
 * the destination from nothing but its migrated registers, so control is
 * stored only on the far side.
 */
static void test_bte_state_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    enum { LINES = 2, SIZE = LINES * BTE_LINE };
    uint8_t expect[SIZE], got[SIZE];
    unsigned int i;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE);

    for (i = 0; i < SIZE; i++) {
        expect[i] = 0x5c ^ i;
    }
    qtest_bufwrite(from, 0x00700000, expect, SIZE);
    qtest_memset(from, 0x00740000, 0x3d, SIZE);

    /* Engine 1 carries plain register values, distinct in every field. */
    qtest_writeq(from, HUB_LOCAL + BTE_CONTROL(1), BTE_CTL_NOTIFY);
    qtest_writeq(from, HUB_LOCAL + BTE_LENGTH(1), 0x0abc);
    qtest_writeq(from, HUB_LOCAL + BTE_SOURCE(1), 0x00600000);
    qtest_writeq(from, HUB_LOCAL + BTE_DEST(1), 0x00610000);
    qtest_writeq(from, HUB_LOCAL + BTE_NOTIFY(1), 0x00620000);
    qtest_writeq(from, HUB_LOCAL + BTE_INTERRUPT(1), BTE_INT_LEVEL(41) | 3);

    /* Engine 0 is armed and not yet launched. */
    qtest_writeq(from, HUB_LOCAL + BTE_LENGTH(0), BTE_BUSY | LINES);
    qtest_writeq(from, HUB_LOCAL + BTE_SOURCE(0), 0x00700000);
    qtest_writeq(from, HUB_LOCAL + BTE_DEST(0), 0x00740000);
    qtest_writeq(from, HUB_LOCAL + BTE_INTERRUPT(0), BTE_INT_LEVEL(20));

    dest_cmdline = g_strdup_printf(MACHINE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + BTE_CONTROL(1)), ==,
                    BTE_CTL_NOTIFY);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + BTE_LENGTH(1)), ==, 0x0abc);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + BTE_SOURCE(1)), ==,
                    0x00600000);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + BTE_DEST(1)), ==,
                    0x00610000);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + BTE_NOTIFY(1)), ==,
                    0x00620000);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + BTE_INTERRUPT(1)), ==,
                    BTE_INT_LEVEL(41) | 3);

    /* The armed engine launches on the destination. */
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + BTE_LENGTH(0)), ==,
                    BTE_BUSY | LINES);
    qtest_writeq(to, HUB_LOCAL + BTE_CONTROL(0), 0);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + BTE_LENGTH(0)), ==, 0);
    qtest_bufread(to, 0x00740000, got, SIZE);
    g_assert_cmpmem(got, SIZE, expect, SIZE);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + PI_INT_PEND0), ==,
                    1ULL << 20);

    qtest_quit(to);
    qtest_quit(from);
}

static void test_bte_physical_routing_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    const uint64_t hub = HUB_BY_NODE + NODE_STRIDE;
    const uint64_t src = 0x00800000;
    const uint64_t dst = 0x00900000;
    const uint64_t notify = 0x00a00000;
    unsigned int engine;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MACHINE_DUAL);
    assign_node1_number(from);
    qtest_memset(from, src, 0xaa, BTE_LINE);
    qtest_memset(from, NODE_STRIDE + src, 0xbb, BTE_LINE);
    qtest_memset(from, dst, 0xcc, 2 * BTE_LINE);
    qtest_memset(from, NODE_STRIDE + dst, 0xcc, 2 * BTE_LINE);
    qtest_memset(from, notify, 0xff, 2 * BTE_LINE);
    qtest_memset(from, NODE_STRIDE + notify, 0xff, 2 * BTE_LINE);
    for (engine = 0; engine < 2; engine++) {
        qtest_writeq(from, hub + BTE_SOURCE(engine), src);
        qtest_writeq(from, hub + BTE_DEST(engine), dst + engine * BTE_LINE);
        qtest_writeq(from, hub + BTE_NOTIFY(engine),
                     notify + engine * BTE_LINE);
        qtest_writeq(from, hub + BTE_LENGTH(engine), BTE_BUSY | 1);
    }
    dest_cmdline = g_strdup_printf(MACHINE_DUAL " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);
    for (engine = 0; engine < 2; engine++) {
        qtest_writeq(to, hub + BTE_CONTROL(engine), BTE_CTL_NOTIFY);
        g_assert_cmphex(qtest_readq(to, hub + BTE_LENGTH(engine)), ==, 0);
        g_assert_cmphex(qtest_readq(to, dst + engine * BTE_LINE),
                        ==, 0xaaaaaaaaaaaaaaaaULL);
        g_assert_cmphex(qtest_readq(to,
                        NODE_STRIDE + dst + engine * BTE_LINE),
                        ==, 0xccccccccccccccccULL);
        g_assert_cmphex(qtest_readq(to, notify + engine * BTE_LINE), ==, 0);
        g_assert_cmphex(qtest_readq(to,
                        NODE_STRIDE + notify + engine * BTE_LINE), ==, ~0ULL);
    }
    qtest_quit(to);
    qtest_quit(from);
}

static void test_flash_autoselect_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE);
    flash_command(from, FLASH_UNLOCK1, 0xaa);
    flash_command(from, FLASH_UNLOCK2, 0x55);
    flash_command(from, FLASH_UNLOCK1, 0x90);
    /*
     * Only the two identity bytes are pinned; what the chip answers past
     * them follows the flash device model, the same narrowing
     * test_flash_autoselect makes.
     */
    g_assert_cmphex(qtest_readb(from, FLASH_HUB), ==, 0x01);
    g_assert_cmphex(qtest_readb(from, FLASH_HUB + 1), ==, 0xd5);

    dest_cmdline = g_strdup_printf(MACHINE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    g_assert_cmphex(qtest_readb(to, FLASH_RESET), ==, 0x01);
    g_assert_cmphex(qtest_readb(to, FLASH_RESET + 1), ==, 0xd5);

    qtest_quit(to);
    qtest_quit(from);
}

static void test_flash_program_command_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE);
    flash_command(from, FLASH_UNLOCK1, 0xaa);
    flash_command(from, FLASH_UNLOCK2, 0x55);
    flash_command(from, FLASH_UNLOCK1, 0xa0);

    dest_cmdline = g_strdup_printf(MACHINE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    flash_command(to, FLASH_PROGRAM_TARGET, 0x5a);
    g_assert_cmphex(qtest_readb(to, FLASH_RESET + FLASH_PROGRAM_TARGET), ==,
                    0x5a);

    qtest_quit(to);
    qtest_quit(from);
}

static void test_enabled_recorder_remains_wired_after_migration(
    const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    QDict *source_state;
    QDict *destination_state;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE_RECORDING);
    qtest_writeq(from, HUB_LOCAL + PI_REGION_PRESENT,
                 0x0123456789abcdefULL);
    source_state = query_origin_state(from);
    g_assert_true(has_hub_write(source_state, PI_REGION_PRESENT,
                                0x0123456789abcdefULL));

    dest_cmdline = g_strdup_printf(MACHINE_RECORDING " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    destination_state = query_origin_state(to);
    g_assert_true(qdict_get_bool(qdict_get_qdict(destination_state,
                                                "flight-recorder"),
                                 "enabled"));
    g_assert_false(has_hub_write(destination_state, PI_REGION_PRESENT,
                                 0x0123456789abcdefULL));
    qobject_unref(destination_state);

    qtest_writeq(to, HUB_LOCAL + PI_REGION_PRESENT,
                 0xfedcba9876543210ULL);
    destination_state = query_origin_state(to);
    g_assert_true(has_hub_write(destination_state, PI_REGION_PRESENT,
                                0xfedcba9876543210ULL));

    qobject_unref(destination_state);
    qobject_unref(source_state);
    qtest_quit(to);
    qtest_quit(from);
}

/* Node 0's ECC and directory back-door windows in Hub-special space. */
#define BACKDOOR_ECC_HUB        (SPACE_HUB + 0x80000000ULL)
#define BACKDOOR_DIR_HUB        (SPACE_HUB + 0xc0000000ULL)

/*
 * The back-door storage is a heap buffer, not a RAM MemoryRegion, so the
 * node's vmstate must carry it -- without that the destination reads its
 * realize-time zero fill. The low sixteen bits are the real storage.
 */
static void test_backdoor_storage_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE);
    qtest_writeq(from, BACKDOOR_ECC_HUB, 0x000000000000cafeULL);
    qtest_writeq(from, BACKDOOR_DIR_HUB, 0x000000000000babeULL);
    g_assert_cmphex(qtest_readq(from, BACKDOOR_ECC_HUB) & 0xffff, ==, 0xcafe);
    g_assert_cmphex(qtest_readq(from, BACKDOOR_DIR_HUB) & 0xffff, ==, 0xbabe);

    dest_cmdline = g_strdup_printf(MACHINE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    g_assert_cmphex(qtest_readq(to, BACKDOOR_ECC_HUB) & 0xffff, ==, 0xcafe);
    g_assert_cmphex(qtest_readq(to, BACKDOOR_DIR_HUB) & 0xffff, ==, 0xbabe);

    qtest_quit(to);
    qtest_quit(from);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/migration/device-window-survives-migration",
                        NULL, test_a_device_window_survives_migration);
    qtest_add_data_func("sn0/migration/wound-receive-timer-survives",
                        NULL, test_a_wound_receive_timer_survives_migration);
    qtest_add_data_func("sn0/migration/dual-base-io-survives-migration",
                        NULL, test_dual_base_io_survives_migration);
    qtest_add_data_func("sn0/migration/network-routes-survive-migration",
                        NULL, test_network_routes_survive_migration);
    qtest_add_data_func("sn0/migration/staged-i2c-start-survives-migration",
                        NULL, test_staged_i2c_start_survives_migration);
    qtest_add_data_func("sn0/migration/conversation-survives-migration",
                        NULL, test_a_conversation_survives_migration);
    qtest_add_data_func("sn0/migration/bte-state-survives-migration",
                        NULL, test_bte_state_survives_migration);
    qtest_add_data_func("sn0/migration/bte-physical-routing-survives-migration",
                        NULL, test_bte_physical_routing_survives_migration);
    qtest_add_data_func("sn0/migration/flash-autoselect-survives-migration",
                        NULL, test_flash_autoselect_survives_migration);
    qtest_add_data_func("sn0/migration/flash-program-command-"
                        "survives-migration",
                        NULL, test_flash_program_command_survives_migration);
    qtest_add_data_func("sn0/migration/enabled-recorder-remains-wired",
                        NULL,
                        test_enabled_recorder_remains_wired_after_migration);
    qtest_add_data_func("sn0/migration/backdoor-storage-survives-migration",
                        NULL, test_backdoor_storage_survives_migration);

    return g_test_run();
}
