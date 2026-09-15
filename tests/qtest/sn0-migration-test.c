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
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sn0-user-image.h"

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

/* The Hub's boot-flash view and AMD command offsets. */
#define SPACE_HUB               0x10000000000ULL
#define FLASH_RESET             0x1fc00000ULL
#define FLASH_HUB               (SPACE_HUB + 0x10000000)
#define FLASH_UNLOCK1           0x5555
#define FLASH_UNLOCK2           0x2aaa
#define FLASH_PROGRAM_TARGET    0xe0100

#define HUB_LOCAL               (SPACE_XTALK + 0x01000000)
#define HUB_BY_NODE             (SPACE_XTALK + 0x01800000)
#define HUB_ONEWIRE             (HUB_LOCAL + 0x2000a8)
#define HUB_ONEWIRE_DONE        (1ULL << 1)
#define HUB_ONEWIRE_RX          (1ULL << 0)
#define HUB_ONEWIRE_RESET       ((0x31ULL << 27) | (0x0eULL << 20))
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
#define I2C_CTL_ES1             0x20
#define I2C_CTL_STA             0x04
#define I2C_CTL_STO             0x02
#define I2C_CTL_ACK             0x01
#define I2C_STS_LRB             0x08
#define ELSC_ADDR               0x08
#define ARB_SIGNAL_AFTER_STAGE  0xe1
#define ARB_SIGNAL1             0xe0
#define ARB_TICK_NS             (1000 * 1000)
#define ARB_PARTIAL_NS          (400 * 1000)

#define ELSC_VMSTATE_VERSION        13
#define ELSC_LEGACY_VERSION         12
#define ELSC_RXBUF_VMSTATE_SIZE     97
#define ELSC_TXBUF_VMSTATE_SIZE     96
#define ELSC_ARB_CODE_VMSTATE_OFFSET \
    (1 + ELSC_RXBUF_VMSTATE_SIZE + 4 + ELSC_TXBUF_VMSTATE_SIZE + \
     4 + 4 + 8)
#define QEMU_VM_SECTION_FULL      4

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
#define BRIDGE_INT_STATUS         (BRIDGE_BASE + 0x100 + REG_DATA)
#define BRIDGE_PIN_ETH            (1u << 2)
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
#define IOC3_ENET_MAC_CONTROL    IOC3_REG(0x0f0)
#define IOC3_ENET_INT_STATUS     IOC3_REG(0x0f4)
#define IOC3_ENET_INT_ENABLE     IOC3_REG(0x0f8)
#define IOC3_ENET_TX_RING_HI     IOC3_REG(0x128)
#define IOC3_ENET_TX_RING_LO     IOC3_REG(0x12c)
#define IOC3_ENET_TX_PRODUCE     IOC3_REG(0x134)
#define IOC3_INT_STATUS          IOC3_REG(0x01c)
#define IOC3_INT_OUTPUT          IOC3_REG(0x02c)
#define IOC3_INT_OUT_PERIODIC    0x00060000u
#define IOC3_INT_OUT_COMMAND     0x0007ffffu
#define IOC3_INT_OUT_STATE       0x80000000u
#define IOC3_INT_OUT_TICK_NS     7800LL
#define PI_RT_COUNT              (HUB_LOCAL + 0x030100)
#define PI_RT_COMPARE_A          (HUB_LOCAL + 0x000108)
#define PI_RT_PENDING_A          (HUB_LOCAL + 0x000120)
#define PI_RT_SENTINEL_TICKS     125ULL
#define PI_RT_SENTINEL_NS        100000LL
#define IOC3_SDMA_RING_HI        IOC3_REG(0x0b0)
#define IOC3_SDMA_RING_LO        IOC3_REG(0x0b4)
#define IOC3_SDMA_A_CONTROL      IOC3_REG(0x0b8)
#define IOC3_SDMA_A_TX_PRODUCE   IOC3_REG(0x0bc)
#define IOC3_SDMA_A_TX_CONSUME   IOC3_REG(0x0c0)
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
#define IOC3_TX_CHARACTER        0x40
#define IOC3_MCR_TX_DMA_EN       (1u << 13)
#define IOC3_MCR_TX_EN           (1u << 14)
#define IOC3_ENET_INT_TX_EMPTY   (1u << 16)
#define IOC3_ENET_INT_TX_COMPLETE (1u << 22)
#define IOC3_TXD_INT_DONE        (1u << 12)
#define IOC3_TXD_DATA_VALID      (1u << 16)
#define IOC3_TXD_DATA_OFFSET     24
#define IOC3_TIMEKEEPER          IOC3_REG(0x80000)
#define TIMEKEEPER_STORAGE_SIZE  0x7ff8
#define TIMEKEEPER_CONTROL       (IOC3_TIMEKEEPER + 0x7ff8)
#define TIMEKEEPER_WEEKDAY       (IOC3_TIMEKEEPER + 0x7ffc)
#define TIMEKEEPER_DAY           (IOC3_TIMEKEEPER + 0x7ffd)
#define TIMEKEEPER_MONTH         (IOC3_TIMEKEEPER + 0x7ffe)
#define TIMEKEEPER_YEAR          (IOC3_TIMEKEEPER + 0x7fff)
#define TIMEKEEPER_CSR_READ      0x40
#define TIMEKEEPER_CSR_WRITE     0x80

#define DUAL_CHASSIS_TOPOLOGY \
    "-m 128M " \
    "-object memory-backend-ram,id=ram0,size=64M " \
    "-object memory-backend-ram,id=ram1,size=64M " \
    "-numa node,nodeid=0,cpus=0-1,memdev=ram0 " \
    "-numa node,nodeid=1,cpus=2-3,memdev=ram1 " \
    "-smp 4,sockets=2,cores=2,threads=1"
#define MACHINE_DUAL \
    "-M origin200 " DUAL_CHASSIS_TOPOLOGY
#define MACHINE_WITH_NET "-M origin200 -nic user,id=n0"

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

static bool module_onewire_slot(QTestState *qts, unsigned int pulse)
{
    uint64_t value;

    qtest_writeq(qts, HUB_ONEWIRE,
                 HUB_ONEWIRE_RESET | ((uint64_t)pulse << 10) | (4ULL << 2));
    qtest_clock_step(qts, (pulse + 4) * 1000);
    value = qtest_readq(qts, HUB_ONEWIRE);
    g_assert_cmphex(value & HUB_ONEWIRE_DONE, ==, HUB_ONEWIRE_DONE);
    return value & HUB_ONEWIRE_RX;
}

static void module_onewire_write_byte(QTestState *qts, uint8_t value)
{
    for (unsigned int bit = 0; bit < 8; bit++) {
        module_onewire_slot(qts, ((value >> bit) & 1) ? 8 : 60);
    }
}

static uint8_t module_onewire_read_byte(QTestState *qts)
{
    uint8_t value = 0;

    for (unsigned int bit = 0; bit < 8; bit++) {
        value |= module_onewire_slot(qts, 8) << bit;
    }
    return value;
}

static void test_module_identity_page_progress_survives_migration(
    const void *data)
{
    static const uint8_t rom[8] = {
        0x09, 0x09, 0x00, 0x00, 0x00, 0x00, 0x10, 0xc7,
    };
    static const uint8_t page[32] = {
        0x01, 0x4b, 0x4f, 0x52, 0x49, 0x47, 0x41, 0x4d,
        0x49, 0x30, 0x31, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf9, 0x27,
    };
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-module-identity-migration-XXXXXX", &err);
    g_assert_no_error(err);
    g_assert_nonnull(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init("-M origin2000");
    dest_cmdline = g_strdup_printf("-M origin2000 -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    g_assert_false(module_onewire_slot(from, 0x321));
    module_onewire_write_byte(from, 0x55);
    for (unsigned int i = 0; i < sizeof(rom); i++) {
        module_onewire_write_byte(from, rom[i]);
    }
    module_onewire_write_byte(from, 0xc3);
    module_onewire_write_byte(from, 0);
    module_onewire_write_byte(from, 0);
    module_onewire_read_byte(from); /* command/address CRC */
    for (unsigned int i = 0; i < 10; i++) {
        g_assert_cmphex(module_onewire_read_byte(from), ==, page[i]);
    }

    migrate_stopped_vm(from, uri);
    for (unsigned int i = 10; i < sizeof(page); i++) {
        g_assert_cmphex(module_onewire_read_byte(to), ==, page[i]);
    }

    qtest_quit(to);
    qtest_quit(from);
}

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

static void wait_for_incoming_complete(QTestState *qts)
{
    g_test_timer_start();
    for (;;) {
        QDict *ret = qtest_qmp_assert_success_ref(
            qts, "{ 'execute': 'query-migrate' }");
        const char *status = qdict_get_str(ret, "status");

        if (g_str_equal(status, "completed")) {
            qobject_unref(ret);
            return;
        }
        g_assert_false(g_str_equal(status, "failed"));
        qobject_unref(ret);
        g_assert(g_test_timer_elapsed() < 30);
        g_usleep(1000);
    }
}

static void cpu_writeq(QTestState *qts, unsigned int cpu, uint64_t addr,
                       uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, cpu, addr, bytes, sizeof(bytes)));
}

static uint64_t cpu_readq(QTestState *qts, unsigned int cpu, uint64_t addr)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, cpu, addr, bytes, sizeof(bytes)));
    return ldq_be_p(bytes);
}

static void enable_cpu_pcf_observer(QTestState *qts, unsigned int cpu)
{
    cpu_writeq(qts, cpu, HUB_LOCAL + I2C_CTRL, I2C_CTL_PIN);
    cpu_writeq(qts, cpu, HUB_LOCAL + I2C_DATA, 0x7f);
    cpu_writeq(qts, cpu, HUB_LOCAL + I2C_CTRL,
               I2C_CTL_PIN | I2C_CTL_ES1);
    cpu_writeq(qts, cpu, HUB_LOCAL + I2C_DATA, 0x1c);
    cpu_writeq(qts, cpu, HUB_LOCAL + I2C_CTRL, I2C_CTL_PIN);
    g_assert_cmphex(cpu_readq(qts, cpu, HUB_LOCAL + I2C_DATA), ==, 0x7f);
    cpu_writeq(qts, cpu, HUB_LOCAL + I2C_DATA, 0x09);
    cpu_writeq(qts, cpu, HUB_LOCAL + I2C_CTRL,
               I2C_CTL_PIN | I2C_CTL_ESO);
}

static void patch_master_elsc_stream(const char *path,
                                     uint32_t expected_version,
                                     uint32_t version, uint32_t arb_code)
{
    static const char name[] = "sgi-elsc";
    g_autofree gchar *contents = NULL;
    GError *err = NULL;
    gsize length, i;
    bool found = false;

    g_assert_true(g_file_get_contents(path, &contents, &length, &err));
    g_assert_no_error(err);

    for (i = 6;
         i + sizeof(name) - 1 + 8 + ELSC_ARB_CODE_VMSTATE_OFFSET + 4 <
             length;
         i++) {
        uint8_t *p = (uint8_t *)contents + i;
        uint8_t *versionp;
        uint8_t *datap;

        if (p[-6] != QEMU_VM_SECTION_FULL ||
            p[-1] != sizeof(name) - 1 ||
            memcmp(p, name, sizeof(name) - 1) || ldl_be_p(p + 8) != 0) {
            continue;
        }

        versionp = p + sizeof(name) - 1 + 4;
        datap = versionp + 4;
        g_assert_cmpuint(ldl_be_p(versionp), ==, expected_version);
        stl_be_p(versionp, version);
        stl_be_p(datap + ELSC_ARB_CODE_VMSTATE_OFFSET, arb_code);
        found = true;
        break;
    }
    g_assert_true(found);
    g_assert_true(g_file_set_contents(path, contents, length, &err));
    g_assert_no_error(err);
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

static void queue_ioc3_ethernet_tx(QTestState *qts, uint32_t desc,
                                   uint32_t produce)
{
    static const uint8_t frame[14] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x08, 0x00,
    };

    qtest_writel(qts, desc, sizeof(frame) | IOC3_TXD_DATA_VALID |
                 IOC3_TXD_INT_DONE);
    qtest_writel(qts, desc + 4, sizeof(frame));
    qtest_memwrite(qts, desc + IOC3_TXD_DATA_OFFSET, frame, sizeof(frame));
    qtest_writel(qts, IOC3_ENET_TX_PRODUCE, produce);
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
    sn0_program_nasid(from, 1);
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

/* The entry cursor and the 16550's pending bytes cross together. */
static void test_blocked_serial_transmit_survives_migration(const void *data)
{
    static const uint8_t entry[8] = {
        'm', 'i', 'g', '!',
        IOC3_TX_CHARACTER, IOC3_TX_CHARACTER,
        IOC3_TX_CHARACTER, IOC3_TX_CHARACTER,
    };
    struct timeval tv = { .tv_sec = 2 };
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    uint8_t filler[512];
    uint8_t got[4];
    size_t remaining;
    GError *err = NULL;
    QTestState *from, *to;
    int source_sv[2], dest_sv[2];

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert_no_error(err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, source_sv), ==, 0);
    g_assert_cmpuint(fill_socket_send_queue(source_sv[1]), >, 0);
    from = qtest_initf(
        MACHINE_WITH_BRIDGE " -chardev socket,id=ioc3-a,fd=%d "
        "-serial chardev:ioc3-a", source_sv[1]);
    g_assert_cmpint(close(source_sv[1]), ==, 0);

    attach_node_ioc3(from, 0);
    qtest_writel(from, CFG0_FOR_NODE(0, IOC3_SLOT) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writeb(from, IOC3_SERIAL_A_LCR, 0x03);
    qtest_writeb(from, IOC3_SERIAL_A_FCR, 0x01);
    qtest_writel(from, IOC3_SDMA_RING_HI, 0);
    qtest_writel(from, IOC3_SDMA_RING_LO, IOC3_RING_RAM | IOC3_RING_4K);
    qtest_writel(from, IOC3_SDMA_A_CONTROL, IOC3_SDMA_ENABLE);
    qtest_memwrite(from, IOC3_RING_RAM, entry, sizeof(entry));
    qtest_writel(from, IOC3_SDMA_A_TX_PRODUCE, sizeof(entry));
    g_assert_cmphex(qtest_readl(from, IOC3_SDMA_A_TX_CONSUME) & 0xff8,
                    ==, 0);

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, dest_sv), ==, 0);
    remaining = fill_socket_send_queue(dest_sv[1]);
    g_assert_cmpuint(remaining, >, 0);
    g_assert_cmpint(setsockopt(dest_sv[0], SOL_SOCKET, SO_RCVTIMEO,
                              &tv, sizeof(tv)), ==, 0);
    dest_cmdline = g_strdup_printf(
        MACHINE_WITH_BRIDGE " -chardev socket,id=ioc3-a,fd=%d "
        "-serial chardev:ioc3-a -incoming %s", dest_sv[1], uri);
    to = qtest_init(dest_cmdline);
    g_assert_cmpint(close(dest_sv[1]), ==, 0);

    migrate_stopped_vm(from, uri);
    while (remaining) {
        size_t chunk = MIN(remaining, sizeof(filler));

        read_exact(dest_sv[0], filler, chunk);
        remaining -= chunk;
    }
    read_exact(dest_sv[0], got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), entry, sizeof(got));
    g_assert_cmphex(qtest_readl(to, IOC3_SDMA_A_TX_CONSUME) & 0xff8,
                    ==, sizeof(entry));

    close(dest_sv[0]);
    close(source_sv[0]);
    qtest_quit(to);
    qtest_quit(from);
}

static void test_interrupt_output_phase_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    int64_t source_time;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE_WITH_BRIDGE);
    attach_node_ioc3(from, 0);
    qtest_writel(from, IOC3_INT_OUTPUT, IOC3_INT_OUT_PERIODIC | 0x0009);
    qtest_clock_step(from, 1);
    source_time = qtest_clock_step(from, IOC3_INT_OUT_TICK_NS);
    g_assert_cmphex(qtest_readl(from, IOC3_INT_OUTPUT) & IOC3_INT_OUT_STATE,
                    ==, IOC3_INT_OUT_STATE);

    dest_cmdline = g_strdup_printf(MACHINE_WITH_BRIDGE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);
    qtest_clock_set(to, source_time);

    g_assert_cmphex(qtest_readl(to, IOC3_INT_OUTPUT) & IOC3_INT_OUT_STATE,
                    ==, IOC3_INT_OUT_STATE);
    qtest_clock_step(to, 2 * IOC3_INT_OUT_TICK_NS - 1);
    g_assert_cmphex(qtest_readl(to, IOC3_INT_OUTPUT) & IOC3_INT_OUT_STATE,
                    ==, IOC3_INT_OUT_STATE);
    qtest_clock_step(to, 1);
    g_assert_cmphex(qtest_readl(to, IOC3_INT_OUTPUT) & IOC3_INT_OUT_STATE,
                    ==, 0);

    qtest_clock_step(to, 7 * IOC3_INT_OUT_TICK_NS - 1);
    g_assert_cmphex(qtest_readl(to, IOC3_INT_OUTPUT) & IOC3_INT_OUT_STATE,
                    ==, 0);
    qtest_clock_step(to, 1);
    g_assert_cmphex(qtest_readl(to, IOC3_INT_OUTPUT) & IOC3_INT_OUT_STATE,
                    ==, IOC3_INT_OUT_STATE);

    qtest_quit(to);
    qtest_quit(from);
}

static void assert_no_ioc3_timer_before_sentinel(QTestState *qts,
                                                  int64_t now)
{
    qtest_writeq(qts, PI_RT_COUNT, 0);
    qtest_writeq(qts, PI_RT_PENDING_A, 0);
    qtest_writeq(qts, PI_RT_COMPARE_A, PI_RT_SENTINEL_TICKS);
    g_assert_cmpint(qtest_clock_step_next(qts), ==,
                    now + PI_RT_SENTINEL_NS);
    g_assert_cmpuint(qtest_readq(qts, PI_RT_PENDING_A), ==, 1);
}

static void assert_legacy_count2_readback(QTestState *qts)
{
    g_assert_cmphex(qtest_readl(qts, IOC3_INT_OUTPUT) &
                    IOC3_INT_OUT_COMMAND, ==,
                    IOC3_INT_OUT_PERIODIC | 0x0002);
}

static void test_legacy_count2_before_first_expiry_migrates(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    int64_t source_time;

    if (!g_getenv("QTEST_QEMU_BINARY_IOC3_V15")) {
        g_test_skip("QTEST_QEMU_BINARY_IOC3_V15 is not set");
        return;
    }

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert_no_error(err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init_ext("QTEST_QEMU_BINARY_IOC3_V15",
                          MACHINE_WITH_BRIDGE, NULL, true);
    attach_node_ioc3(from, 0);
    qtest_writel(from, IOC3_INT_OUTPUT, IOC3_INT_OUT_PERIODIC | 0x0002);
    source_time = qtest_clock_set(from, 0);
    g_assert_cmphex(qtest_readl(from, IOC3_INT_OUTPUT), ==,
                    IOC3_INT_OUT_PERIODIC | 0x0002);

    dest_cmdline = g_strdup_printf(MACHINE_WITH_BRIDGE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);
    qtest_clock_set(to, source_time);

    g_assert_cmphex(qtest_readl(to, IOC3_INT_OUTPUT), ==,
                    IOC3_INT_OUT_PERIODIC | 0x0002);
    source_time = qtest_clock_step(to, 1);
    g_assert_cmphex(qtest_readl(to, IOC3_INT_OUTPUT), ==,
                    IOC3_INT_OUT_PERIODIC | 0x0002 | IOC3_INT_OUT_STATE);
    assert_legacy_count2_readback(to);
    assert_no_ioc3_timer_before_sentinel(to, source_time);
    g_assert_cmphex(qtest_readl(to, IOC3_INT_OUTPUT), ==,
                    IOC3_INT_OUT_PERIODIC | 0x0002 | IOC3_INT_OUT_STATE);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static void test_legacy_count2_high_migrates_onward(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *onward_uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    g_autofree gchar *onward_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to, *onward;
    int64_t source_time;

    if (!g_getenv("QTEST_QEMU_BINARY_IOC3_V15")) {
        g_test_skip("QTEST_QEMU_BINARY_IOC3_V15 is not set");
        return;
    }

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert_no_error(err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    onward_uri = g_strdup_printf("unix:%s/onward.sock", tmpfs);

    from = qtest_init_ext("QTEST_QEMU_BINARY_IOC3_V15",
                          MACHINE_WITH_BRIDGE, NULL, true);
    attach_node_ioc3(from, 0);
    qtest_writel(from, IOC3_INT_OUTPUT, IOC3_INT_OUT_PERIODIC | 0x0002);
    source_time = qtest_clock_step(from, 1);
    g_assert_cmphex(qtest_readl(from, IOC3_INT_OUTPUT), ==,
                    IOC3_INT_OUT_PERIODIC | 0x0002 | IOC3_INT_OUT_STATE);

    dest_cmdline = g_strdup_printf(MACHINE_WITH_BRIDGE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);
    qtest_clock_set(to, source_time);

    g_assert_cmphex(qtest_readl(to, IOC3_INT_OUTPUT), ==,
                    IOC3_INT_OUT_PERIODIC | 0x0002 | IOC3_INT_OUT_STATE);
    assert_legacy_count2_readback(to);
    assert_no_ioc3_timer_before_sentinel(to, source_time);
    g_assert_cmphex(qtest_readl(to, IOC3_INT_OUTPUT), ==,
                    IOC3_INT_OUT_PERIODIC | 0x0002 | IOC3_INT_OUT_STATE);

    source_time += PI_RT_SENTINEL_NS;
    onward_cmdline = g_strdup_printf(MACHINE_WITH_BRIDGE " -incoming %s",
                                     onward_uri);
    onward = qtest_init(onward_cmdline);
    migrate_stopped_vm(to, onward_uri);
    qtest_clock_set(onward, source_time);

    g_assert_cmphex(qtest_readl(onward, IOC3_INT_OUTPUT), ==,
                    IOC3_INT_OUT_PERIODIC | 0x0002 | IOC3_INT_OUT_STATE);
    assert_legacy_count2_readback(onward);
    qtest_writel(onward, IOC3_INT_OUTPUT, 0);
    source_time = qtest_clock_step(onward, 1);
    g_assert_cmphex(qtest_readl(onward, IOC3_INT_OUTPUT), ==, 0);
    assert_no_ioc3_timer_before_sentinel(onward, source_time);
    g_assert_cmphex(qtest_readl(onward, IOC3_INT_OUTPUT), ==, 0);

    qtest_quit(onward);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
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
 * that interval after code 0's grant and part of code 1's one-millisecond
 * interval. Migration must preserve both the remaining interval and next
 * code. Code 1 then replaces readable S0 exactly at its deadline, while the
 * separately staged address still makes START reach the ELSC.
 */
static void test_staged_i2c_start_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    int64_t source_time;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE);
    qtest_writeq(from, HUB_LOCAL + I2C_CTRL, I2C_CTL_PIN);
    qtest_writeq(from, HUB_LOCAL + I2C_DATA, 0x7f);
    qtest_writeq(from, HUB_LOCAL + I2C_CTRL, I2C_CTL_PIN | I2C_CTL_ES1);
    qtest_writeq(from, HUB_LOCAL + I2C_DATA, 0x1c);
    qtest_writeq(from, HUB_LOCAL + I2C_CTRL, I2C_CTL_PIN);
    g_assert_cmphex(qtest_readq(from, HUB_LOCAL + I2C_DATA), ==, 0x7f);
    qtest_writeq(from, HUB_LOCAL + I2C_DATA, 0x09);
    qtest_writeq(from, HUB_LOCAL + I2C_CTRL, I2C_CTL_PIN | I2C_CTL_ESO);
    source_time = qtest_clock_step(from, ARB_PARTIAL_NS);
    qtest_writeq(from, HUB_LOCAL + I2C_DATA, ELSC_ADDR << 1);
    g_assert_cmphex(qtest_readq(from, HUB_LOCAL + I2C_DATA), ==,
                    ELSC_ADDR << 1);

    dest_cmdline = g_strdup_printf(MACHINE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);
    qtest_clock_set(to, source_time);

    qtest_clock_step(to, ARB_TICK_NS - ARB_PARTIAL_NS - 1);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + I2C_DATA), ==,
                    ELSC_ADDR << 1);
    qtest_clock_step(to, 1);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + I2C_DATA), ==,
                    ARB_SIGNAL_AFTER_STAGE);
    qtest_writeq(to, HUB_LOCAL + I2C_CTRL,
                 I2C_CTL_PIN | I2C_CTL_ESO | I2C_CTL_STA | I2C_CTL_ACK);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + I2C_CTRL) & I2C_STS_LRB,
                    ==, 0);
    qtest_writeq(to, HUB_LOCAL + I2C_CTRL,
                 I2C_CTL_PIN | I2C_CTL_ESO | I2C_CTL_STO);

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * Version 12 allowed all eight old rotation codes. A two-module destination
 * has five positions, so an old code 7 must resume at position 2. The same
 * value in the narrowed version 13 domain is a corrupt stream and must fail.
 */
static void test_elsc_legacy_rotation_position_migrates(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *stream = NULL;
    g_autofree gchar *uri = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert_no_error(err);
    stream = g_build_filename(tmpfs, "migration", NULL);
    uri = g_strdup_printf("file:%s", stream);

    from = qtest_init(MACHINE_DUAL);
    enable_cpu_pcf_observer(from, 0);
    enable_cpu_pcf_observer(from, 2);
    migrate_stopped_vm(from, uri);
    qtest_quit(from);

    patch_master_elsc_stream(stream, ELSC_VMSTATE_VERSION,
                             ELSC_LEGACY_VERSION, 7);
    to = qtest_init(MACHINE_DUAL " -incoming defer");
    qtest_qmp_assert_success(
        to, "{ 'execute': 'migrate-incoming', "
            "'arguments': { 'uri': %s } }", uri);
    wait_for_incoming_complete(to);
    qtest_clock_step(to, ARB_TICK_NS);
    g_assert_cmphex(cpu_readq(to, 0, HUB_LOCAL + I2C_DATA), ==,
                    ARB_SIGNAL1 + 2);
    g_assert_cmphex(cpu_readq(to, 2, HUB_LOCAL + I2C_DATA), ==,
                    ARB_SIGNAL1 + 2);
    qtest_quit(to);

    patch_master_elsc_stream(stream, ELSC_LEGACY_VERSION,
                             ELSC_VMSTATE_VERSION, 7);
    to = qtest_init(MACHINE_DUAL " -incoming defer");
    qtest_set_expected_status(to, 1);
    qtest_qmp_assert_success(
        to, "{ 'execute': 'migrate-incoming', "
            "'arguments': { 'uri': %s } }", uri);
    qtest_wait_qemu(to);
    qtest_quit(to);

    g_assert_cmpint(g_unlink(stream), ==, 0);
    g_assert_cmpint(g_rmdir(tmpfs), ==, 0);
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
    sn0_program_nasid(from, 1);
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

/* Node 0's ECC and directory back-door windows in Hub-special space. */
#define BACKDOOR_ECC_HUB        (SPACE_HUB + 0x80000000ULL)
#define BACKDOOR_DIR_HUB        (SPACE_HUB + 0xc0000000ULL)

/*
 * The back-door storage is the backing of a ROM device, so it crosses in
 * the RAM stream -- if it ever stopped doing so the destination would
 * read its realize-time zero fill. The low sixteen bits are the real
 * storage.
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

/*
 * The Bridge restores its PCI pin state from the stream. Re-driving an
 * asserted child while that restoration is still pending increments the PCI
 * core's aggregate count, so one acknowledgement cannot deassert the pin.
 */
static void test_ethernet_interrupt_reasserts_after_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint32_t desc = 0x00180000;
    uint32_t sources = IOC3_ENET_INT_TX_EMPTY | IOC3_ENET_INT_TX_COMPLETE;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE_WITH_NET);
    attach_node_ioc3(from, 0);
    qtest_writel(from, CFG0_FOR_NODE(0, IOC3_SLOT) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(from, IOC3_ENET_MAC_CONTROL,
                 IOC3_MCR_TX_DMA_EN | IOC3_MCR_TX_EN);
    qtest_writel(from, IOC3_ENET_INT_ENABLE, sources);
    qtest_writel(from, IOC3_ENET_TX_RING_HI, 0);
    qtest_writel(from, IOC3_ENET_TX_RING_LO, desc & 0xffffc000u);
    queue_ioc3_ethernet_tx(from, desc, 128);
    g_assert_cmphex(qtest_readl(from, BRIDGE_INT_STATUS) & BRIDGE_PIN_ETH,
                    ==, BRIDGE_PIN_ETH);

    dest_cmdline = g_strdup_printf(MACHINE_WITH_NET " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    g_assert_cmphex(qtest_readl(to, BRIDGE_INT_STATUS) & BRIDGE_PIN_ETH,
                    ==, BRIDGE_PIN_ETH);
    qtest_writel(to, IOC3_ENET_INT_STATUS, sources);
    g_assert_cmphex(qtest_readl(to, BRIDGE_INT_STATUS) & BRIDGE_PIN_ETH,
                    ==, 0);

    queue_ioc3_ethernet_tx(to, desc + 128, 256);
    g_assert_cmphex(qtest_readl(to, BRIDGE_INT_STATUS) & BRIDGE_PIN_ETH,
                    ==, BRIDGE_PIN_ETH);

    qtest_quit(to);
    qtest_quit(from);
}

static void migrate_timekeeper_image(uint8_t fill)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    g_autofree uint8_t *expected = g_malloc(TIMEKEEPER_STORAGE_SIZE);
    g_autofree uint8_t *actual = g_malloc(TIMEKEEPER_STORAGE_SIZE);
    GError *err = NULL;
    QTestState *from, *to;
    size_t i;

    for (i = 0; i < TIMEKEEPER_STORAGE_SIZE; i++) {
        expected[i] = fill ? (uint8_t)(fill ^ i ^ (i >> 8)) : 0;
    }

    tmpfs = g_dir_make_tmp("sn0-timekeeper-migration-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE);
    attach_node_ioc3(from, 0);
    qtest_memwrite(from, IOC3_TIMEKEEPER, expected,
                   TIMEKEEPER_STORAGE_SIZE);
    qtest_writeb(from, TIMEKEEPER_CONTROL, TIMEKEEPER_CSR_WRITE);
    qtest_writeb(from, TIMEKEEPER_WEEKDAY, 5);
    qtest_writeb(from, TIMEKEEPER_DAY, 7);
    qtest_writeb(from, TIMEKEEPER_MONTH, 3);
    qtest_writeb(from, TIMEKEEPER_YEAR, 0x40);
    qtest_writeb(from, TIMEKEEPER_CONTROL, 0x15);

    dest_cmdline = g_strdup_printf(MACHINE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    /* This is the destination's first access to the restored device. */
    qtest_memread(to, IOC3_TIMEKEEPER, actual, TIMEKEEPER_STORAGE_SIZE);
    g_assert_cmpmem(actual, TIMEKEEPER_STORAGE_SIZE,
                    expected, TIMEKEEPER_STORAGE_SIZE);
    g_assert_cmphex(qtest_readb(to, TIMEKEEPER_CONTROL), ==, 0x15);
    qtest_writeb(to, TIMEKEEPER_CONTROL, 0x15 | TIMEKEEPER_CSR_READ);
    g_assert_cmphex(qtest_readb(to, TIMEKEEPER_WEEKDAY), ==, 5);
    g_assert_cmphex(qtest_readb(to, TIMEKEEPER_DAY), ==, 7);
    g_assert_cmphex(qtest_readb(to, TIMEKEEPER_MONTH), ==, 3);
    g_assert_cmphex(qtest_readb(to, TIMEKEEPER_YEAR), ==, 0x40);

    qtest_clock_step(to, 1000000000LL);
    qtest_memread(to, IOC3_TIMEKEEPER, actual, TIMEKEEPER_STORAGE_SIZE);
    g_assert_cmpmem(actual, TIMEKEEPER_STORAGE_SIZE,
                    expected, TIMEKEEPER_STORAGE_SIZE);

    qtest_system_reset(to);
    attach_node_ioc3(to, 0);
    g_assert_cmphex(qtest_readb(to, TIMEKEEPER_CONTROL), ==, 0x15);
    g_assert_cmphex(qtest_readb(to, TIMEKEEPER_WEEKDAY), ==, 5);
    qtest_memread(to, IOC3_TIMEKEEPER, actual, TIMEKEEPER_STORAGE_SIZE);
    g_assert_cmpmem(actual, TIMEKEEPER_STORAGE_SIZE,
                    expected, TIMEKEEPER_STORAGE_SIZE);

    qtest_quit(to);
    qtest_quit(from);
}

static void test_timekeeper_images_survive_migration(const void *data)
{
    migrate_timekeeper_image(0);
    migrate_timekeeper_image(0xa5);
}

/*
 * The per-CPU cache-as-RAM private window (sn-promdata.c, shared with the
 * Origin 300 node since this filter moved out of sn0.c) forwards instead
 * of serving from its own buffer whenever no processor is in the PROM's
 * cache-only phase -- true throughout a qtest run, which never executes
 * guest code, so the phase is never entered here. A forwarded access
 * still traces, and that trace is what tells a broken post-load apart
 * from a working one: if migration left the filter disabled or removed,
 * the access would fall straight through to whatever the window
 * overlays instead of tracing a forward, silently.
 */
#define PROMDATA_WINDOW_PHYS UINT64_C(0x100000)

static void promdata_cpu_probe(QTestState *qts, unsigned int cpu)
{
    g_autofree char *command =
        g_strdup_printf("xp/1wx 0x%" PRIx64, PROMDATA_WINDOW_PHYS);
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{ 'execute': 'human-monitor-command',"
        "  'arguments': { 'command-line': %s, 'cpu-index': %u } }",
        command, cpu);

    g_assert(qdict_haskey(response, "return"));
}

static bool promdata_forward_traced(const char *path)
{
    g_autofree char *contents = NULL;

    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    return strstr(contents, "sn_promdata_forward") != NULL;
}

static void test_promdata_filter_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *from_trace = NULL;
    g_autofree gchar *to_trace = NULL;
    g_autofree gchar *from_cmdline = NULL;
    g_autofree gchar *to_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-promdata-migration-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from_trace = g_strdup_printf("%s/from.trace", tmpfs);
    to_trace = g_strdup_printf("%s/to.trace", tmpfs);

    from_cmdline = g_strdup_printf(
        MACHINE " -trace enable=sn_promdata_forward,file=%s", from_trace);
    from = qtest_init(from_cmdline);
    promdata_cpu_probe(from, 0);
    g_assert_true(promdata_forward_traced(from_trace));

    to_cmdline = g_strdup_printf(
        MACHINE " -trace enable=sn_promdata_forward,file=%s -incoming %s",
        to_trace, uri);
    to = qtest_init(to_cmdline);
    migrate_stopped_vm(from, uri);

    promdata_cpu_probe(to, 0);
    g_assert_true(promdata_forward_traced(to_trace));

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
    qtest_add_data_func("sn0/migration/blocked-serial-transmit-survives",
                        NULL,
                        test_blocked_serial_transmit_survives_migration);
    qtest_add_data_func("sn0/migration/interrupt-output-phase-survives",
                        NULL, test_interrupt_output_phase_survives_migration);
    qtest_add_data_func("sn0/migration/legacy-count2-before-first-expiry",
                        NULL,
                        test_legacy_count2_before_first_expiry_migrates);
    qtest_add_data_func("sn0/migration/legacy-count2-high-migrates-onward",
                        NULL, test_legacy_count2_high_migrates_onward);
    qtest_add_data_func("sn0/migration/dual-base-io-survives-migration",
                        NULL, test_dual_base_io_survives_migration);
    qtest_add_data_func("sn0/migration/network-routes-survive-migration",
                        NULL, test_network_routes_survive_migration);
    qtest_add_data_func("sn0/migration/staged-i2c-start-survives-migration",
                        NULL, test_staged_i2c_start_survives_migration);
    qtest_add_data_func("sn0/migration/elsc-legacy-rotation-position",
                        NULL,
                        test_elsc_legacy_rotation_position_migrates);
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
    qtest_add_data_func("sn0/migration/backdoor-storage-survives-migration",
                        NULL, test_backdoor_storage_survives_migration);
    qtest_add_data_func("sn0/migration/ethernet-interrupt-reasserts",
                        NULL,
                        test_ethernet_interrupt_reasserts_after_migration);
    qtest_add_data_func("sn0/migration/timekeeper-images-survive", NULL,
                        test_timekeeper_images_survive_migration);
    qtest_add_data_func("sn0/migration/module-identity-page-progress",
                        NULL,
                        test_module_identity_page_progress_survives_migration);
    qtest_add_data_func("sn0/migration/promdata-filter-survives-migration",
                        NULL, test_promdata_filter_survives_migration);

    return g_test_run();
}
