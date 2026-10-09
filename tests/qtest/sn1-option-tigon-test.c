/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Real Tigon host-ring traffic through the routed Origin 300 option buses.
 * Register/ring layouts reuse tigon-test.c's public host-interface protocol.
 * This tests the existing NIC model, not execution of diagnostic firmware.
 * The board pin map is the reviewed synthetic experiment, not physical wiring.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "hw/pci/pci_regs.h"
#include "sgi-test.h"
#include "sgi-migrate.h"
#include <sys/socket.h>
#include <netinet/in.h>

#define TOPOLOGY "origin300-numalink-pci-expansion-experiment"
#define MAC "52:54:00:12:34:56"
#define NODE(n) ((uint64_t)(n) << 33)
#define BEDROCK_LOCAL (SPACE_IO + UINT64_C(0x01000000))
#define MD_MEMORY_CONFIG 0x780008
#define MD_INSTALLED_SELECT2 ((UINT64_C(2) << 32) | UINT64_C(0x3300))
#define PI_PENDING_MODIFY 0x90
#define IRQ_BIT 17
#define IRQ_MASK (UINT64_C(1) << IRQ_BIT)
#define XBRIDGE_INT_STATUS 0x100
#define XBRIDGE_INT_ENABLE 0x108
#define XBRIDGE_HOST_REGISTER (1U << 17)
#define XBRIDGE_HOST_NASID_SHIFT 9
#define XBRIDGE_PIO_SWAP (1U << 13)
#define ADAPTER_MMIO 0x10000000
#define DMA_PREFIX UINT64_C(0xa080000000000000)
#define DMA_SWAP_CLEAR_PREFIX UINT64_C(0xa100000000000000)

#define TI_MISC_HOST_CTL 0x040
#define TI_PCI_STATE 0x05c
#define TI_PCI_NO_WORD_SWAP (1U << 18)
#define TI_PCI_32BIT_BUS (1U << 20)
#define TI_WINBASE 0x068
#define TI_DMA_READ_HOST_HI 0x090
#define TI_DMA_READ_HOST_LO 0x094
#define TI_DMA_READ_LENGTH 0x09c
#define TI_DMA_READ_STATE 0x0b0
#define TI_DMA_READ_LOCAL 0x0b4
#define TI_DMA_WRITE_HOST_HI 0x080
#define TI_DMA_WRITE_HOST_LO 0x084
#define TI_DMA_WRITE_STATE 0x0a0
#define TI_DMA_WRITE_LOCAL 0x0a4
#define TI_DMA_WRITE_LENGTH 0x0ac
#define TI_DMA_ACTIVE 0x4
#define TI_DMA_NO_SWAP 0x2
#define TI_DMA_RESET 0x1
#define TI_DMA_NO_MASTER (1U << 20)
#define TI_DMA_INT_ENABLE (1U << 21)
#define TI_DMA_HOST_OVERFLOW (1U << 27)
#define TI_MHC_MASK_INTS (1U << 6)
#define TI_CPU_STATE 0x140
#define TI_MB_HOSTINTR 0x504
#define TI_MB_SENDPROD_IDX 0x514
#define TI_MB_STDRXPROD_IDX 0x51c
#define TI_GCR_GENINFO_HI 0x608
#define TI_GCR_GENINFO_LO 0x60c
#define TI_GCR_OPMODE 0x618
#define TI_GCR_EVENTCONS_IDX 0x628
#define TI_MHC_INTSTATE 1
#define TI_MHC_CLEARINT 2
#define TI_OPMODE_BE 0x16
#define TI_OPMODE_SWAP_CLEAR 0x4000020c
#define TI_RCB_FLAG_HOST_RING 0x0040
#define TI_BDFLAG_END 0x0004
#define TI_BDTYPE_RECV_BD 0x0002
#define GIB 0x200000
#define EVENT_RING 0x210000
#define EVENT_PROD 0x220000
#define TX_CONS 0x230000
#define RETURN_RING 0x240000
#define RETURN_PROD 0x250000
#define STD_RX_RING 0x260000
#define TX_RING 0x270000
#define RX_BUF 0x280000
#define TX_BUF 0x290000

static const char *bus_names[] = {
    "head-e.0", "node1-head-e.0",
    "expansion0-u0-head-e.0", "expansion0-u0-head-f.0",
    "expansion0-u1-head-e.0", "expansion0-u1-head-f.0",
    "expansion0-u2-head-e.0", "expansion0-u2-head-f.0",
};
static const unsigned expansion_widgets[] = { 0xe, 0xf, 8, 9, 0xc, 0xd };

typedef struct {
    unsigned head;
    unsigned device;
    unsigned destination;
    bool swap_clear;
    unsigned payload_offset;
    uint32_t gib_offset;
} Placement;

typedef struct {
    QTestState *qts;
    const Placement *placement;
    unsigned pio_cpu;
    uint64_t head;
    uint64_t window;
} Tigon;

static char *prom_path;

static void cpu_writeq(QTestState *qts, unsigned cpu, uint64_t at,
                       uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, cpu, at, bytes, sizeof(bytes)));
}

static uint64_t cpu_readq(QTestState *qts, unsigned cpu, uint64_t at)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, cpu, at, bytes, sizeof(bytes)));
    return ldq_be_p(bytes);
}

static void cpu_writel(QTestState *qts, unsigned cpu, uint64_t at,
                       uint32_t value)
{
    uint8_t bytes[4];

    stl_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, cpu, at, bytes, sizeof(bytes)));
}

static uint32_t cpu_readl(QTestState *qts, unsigned cpu, uint64_t at)
{
    uint8_t bytes[4];

    g_assert_true(qtest_memread_cpu(qts, cpu, at, bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

static void reg_write(Tigon *t, uint64_t at, uint32_t value)
{
    cpu_writel(t->qts, t->pio_cpu, t->window + at, value);
}

static uint32_t reg_read(Tigon *t, uint64_t at)
{
    return cpu_readl(t->qts, t->pio_cpu, t->window + at);
}

static uint64_t memory_address(unsigned node, uint32_t offset)
{
    return NODE(node * 2) + offset;
}

static uint64_t dma_address(Tigon *t, uint32_t offset)
{
    uint64_t prefix = t->placement->swap_clear ?
                      DMA_SWAP_CLEAR_PREFIX : DMA_PREFIX;

    return prefix | memory_address(t->placement->destination, offset);
}

static void memory_write(Tigon *t, unsigned node, uint32_t offset,
                         const void *data, size_t size)
{
    g_assert_true(qtest_memwrite_cpu(t->qts, node * 2,
        memory_address(node, offset), data, size));
}

static void memory_read(Tigon *t, unsigned node, uint32_t offset,
                        void *data, size_t size)
{
    g_assert_true(qtest_memread_cpu(t->qts, node * 2,
        memory_address(node, offset), data, size));
}

static void memory_word(Tigon *t, uint32_t offset, uint32_t value)
{
    cpu_writel(t->qts, t->placement->destination * 2,
               memory_address(t->placement->destination, offset), value);
}

static uint32_t read_memory_word(Tigon *t, uint32_t offset)
{
    return cpu_readl(t->qts, t->placement->destination * 2,
                     memory_address(t->placement->destination, offset));
}

static void host_address(Tigon *t, uint32_t field, uint32_t offset)
{
    uint64_t address = dma_address(t, offset);

    memory_word(t, field, address >> 32);
    memory_word(t, field + 4, address);
}

static void zero_region(Tigon *t, uint32_t offset, size_t size)
{
    g_autofree uint8_t *bytes = g_malloc0(size);

    memory_write(t, t->placement->destination, offset, bytes, size);
}

/*
 * Vector register protocol matches sn-builder-test.c's expansion helper.
 * This topology reaches the expansion endpoint through router output four.
 */
static void expansion_vector(Tigon *t, uint64_t offset, uint64_t value)
{
    uint64_t status;

    cpu_readq(t->qts, 0, BEDROCK_LOCAL + 0x60c038);
    cpu_writeq(t->qts, 0, BEDROCK_LOCAL + 0x60c008, 4);
    cpu_writeq(t->qts, 0, BEDROCK_LOCAL + 0x60c010, value);
    cpu_writeq(t->qts, 0, BEDROCK_LOCAL + 0x60c000,
               (offset & UINT64_C(0xfffff8)) | (UINT64_C(0x1c8) << 40) | 1);
    status = cpu_readq(t->qts, 0, BEDROCK_LOCAL + 0x60c020);
    g_assert_cmphex(status & (UINT64_C(1) << 63), ==, UINT64_C(1) << 63);
    g_assert_cmphex(status & 7, ==, 1);
}

static void setup_routes(Tigon *t)
{
    unsigned index = t->placement->head;

    cpu_writeq(t->qts, 2, BEDROCK_LOCAL + 0x600000, UINT64_C(2) << 32);
    for (unsigned node = 0; node < 2; node++) {
        cpu_writeq(t->qts, node * 2, BEDROCK_LOCAL + MD_MEMORY_CONFIG,
                   MD_INSTALLED_SELECT2);
    }
    /*
     * Named chassis ports: master one, slave six, expansion four.
     * M-mode tables carry turns relative to the ingress port; vector
     * requests above select an absolute output port instead.
     */
    cpu_writeq(t->qts, 0, BEDROCK_LOCAL + 0x681010, 5);
    cpu_writeq(t->qts, 2, BEDROCK_LOCAL + 0x681000, 3);
    cpu_writeq(t->qts, 0, BEDROCK_LOCAL + 0x681038, 3);
    cpu_writeq(t->qts, 2, BEDROCK_LOCAL + 0x681038, 6);
    expansion_vector(t, 0x600000, UINT64_C(7) << 32);
    expansion_vector(t, 0x681000, 5);
    expansion_vector(t, 0x681010, 2);
    t->pio_cpu = index == 1 ? 2 : 0;
    t->head = SPACE_IO + (index < 2 ? NODE(index * 2) : NODE(7)) +
              ((uint64_t)(index < 2 ? 0xe :
                          expansion_widgets[index - 2]) << 24);
    t->window = t->head + (t->placement->device == 1 ? 0x400000 : 0x600000);
}

static void setup_card(Tigon *t)
{
    const unsigned device = t->placement->device;
    uint64_t config = t->head + 0x20000 + device * 0x1000;
    uint32_t control;
    unsigned widget = t->placement->head < 2 ? 0xe :
                      expansion_widgets[t->placement->head - 2];
    unsigned nasid = t->placement->destination * 2;

    g_assert_cmphex(cpu_readl(t->qts, t->pio_cpu, config), ==, 0x000910a9);
    cpu_writel(t->qts, t->pio_cpu, config + PCI_BASE_ADDRESS_0, ADAPTER_MMIO);
    cpu_writel(t->qts, t->pio_cpu, config + PCI_COMMAND,
               PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    cpu_writel(t->qts, t->pio_cpu, t->head + 0x204 + device * 8,
               DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20));
    control = cpu_readl(t->qts, t->pio_cpu, t->head + 0x24);
    cpu_writel(t->qts, t->pio_cpu, t->head + 0x24, (control & ~15U) | widget);
    cpu_writel(t->qts, t->pio_cpu, t->head + 0x84, 0xa << 20);
    cpu_writel(t->qts, t->pio_cpu, t->head + 0x34, 0xa << 16);
    cpu_writel(t->qts, t->pio_cpu, t->head + 0x3c,
               0x1800000 + PI_PENDING_MODIFY);
    cpu_writel(t->qts, t->pio_cpu, t->head + 0x134 + device * 8,
               XBRIDGE_HOST_REGISTER | (nasid << XBRIDGE_HOST_NASID_SHIFT) |
               IRQ_BIT);
    cpu_writel(t->qts, t->pio_cpu, t->head + XBRIDGE_INT_ENABLE + 4,
               1U << device);
}

static void setup_rings(Tigon *t, uint32_t gib)
{
    zero_region(t, gib, 0x490);
    zero_region(t, EVENT_RING, 256 * 8);
    zero_region(t, EVENT_PROD, 8);
    zero_region(t, TX_CONS, 8);
    zero_region(t, RETURN_RING, 2048 * 32);
    zero_region(t, RETURN_PROD, 8);
    zero_region(t, STD_RX_RING, 512 * 32);
    zero_region(t, TX_RING, 512 * 16);
    zero_region(t, RX_BUF, 2048);
    host_address(t, gib + 0x400, EVENT_RING);
    host_address(t, gib + 0x420, TX_RING);
    memory_word(t, gib + 0x428, (512U << 16) | TI_RCB_FLAG_HOST_RING);
    host_address(t, gib + 0x430, STD_RX_RING);
    host_address(t, gib + 0x460, RETURN_RING);
    memory_word(t, gib + 0x468, 2048U << 16);
    host_address(t, gib + 0x470, EVENT_PROD);
    host_address(t, gib + 0x478, RETURN_PROD);
    host_address(t, gib + 0x480, TX_CONS);
    reg_write(t, TI_GCR_OPMODE,
              t->placement->swap_clear ? TI_OPMODE_SWAP_CLEAR : TI_OPMODE_BE);
}

static void expect_interrupt(Tigon *t, bool asserted)
{
    for (unsigned node = 0; node < 2; node++) {
        uint64_t pending = cpu_readq(t->qts, node * 2,
                                     BEDROCK_LOCAL + PI_INT_PEND0);

        g_assert_cmphex(pending & IRQ_MASK, ==,
            asserted && node == t->placement->destination ? IRQ_MASK : 0);
    }
    g_assert_cmphex(reg_read(t, TI_MISC_HOST_CTL) & TI_MHC_INTSTATE, ==,
                    asserted ? TI_MHC_INTSTATE : 0);
    for (unsigned head = 0; head < G_N_ELEMENTS(bus_names); head++) {
        unsigned cpu = head == 1 ? 2 : 0;
        unsigned widget = head < 2 ? 0xe : expansion_widgets[head - 2];
        uint64_t base = SPACE_IO + (head < 2 ? NODE(head * 2) : NODE(7)) +
                        ((uint64_t)widget << 24);

        g_assert_cmphex(cpu_readl(t->qts, cpu,
            base + XBRIDGE_INT_STATUS + 4) & 0xff, ==,
            asserted && head == t->placement->head ?
            1U << t->placement->device : 0);
    }
}

static void acknowledge(Tigon *t)
{
    reg_write(t, TI_MISC_HOST_CTL, TI_MHC_CLEARINT);
    reg_write(t, TI_MB_HOSTINTR, 0);
    for (unsigned node = 0; node < 2; node++) {
        cpu_writeq(t->qts, node * 2, BEDROCK_LOCAL + PI_PENDING_MODIFY,
                   IRQ_BIT);
    }
    expect_interrupt(t, false);
}

static void socket_timeout(int fd)
{
    struct timeval tv = { .tv_sec = 5 };

    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)),
                    ==, 0);
    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)),
                    ==, 0);
}

static void test_pio_sram_direct_dma_bytes(void)
{
    const Placement p = { .head = 2, .device = 1, .destination = 1,
                          .swap_clear = true };
    static const uint32_t words[2] = { 0x17395bc1, 0x2e4068af };
    static const uint8_t card_bytes[2][8] = {
        { 0xc1, 0x5b, 0x39, 0x17, 0xaf, 0x68, 0x40, 0x2e },
        { 0x17, 0x39, 0x5b, 0xc1, 0x2e, 0x40, 0x68, 0xaf },
    };
    static const uint8_t host_bytes[2][8] = {
        { 0x17, 0x39, 0x5b, 0xc1, 0x2e, 0x40, 0x68, 0xaf },
        { 0xc1, 0x5b, 0x39, 0x17, 0xaf, 0x68, 0x40, 0x2e },
    };
    const uint8_t sentinel[8] = {
        0xd3, 0x08, 0x95, 0x4a, 0xe1, 0x76, 0x2b, 0xbc,
    };
    g_autofree char *storage = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = &p };
    uint8_t seen[8];
    uint32_t device_control;
    int fd;

    fd = g_file_open_tmp("sn1-option-tigon-pio-dma-XXXXXX", &storage, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    t.qts = qtest_initf(
        "-M origin300,topology=" TOPOLOGY
        ",expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s -nodefaults -display none "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device tigon,bus=%s,addr=%u,mac=" MAC,
        prom_path, storage, bus_names[p.head], p.device);
    setup_routes(&t);
    setup_card(&t);
    reg_write(&t, TI_PCI_STATE, TI_PCI_NO_WORD_SWAP);
    device_control = cpu_readl(t.qts, t.pio_cpu,
                               t.head + 0x204 + p.device * 8);

    for (unsigned swap = 0; swap < 2; swap++) {
        uint32_t local = 0x3000 + swap * 0x100;
        uint32_t destination = 0x2b0000 + swap * 0x100;
        uint64_t address = dma_address(&t, destination);

        reg_write(&t, TI_WINBASE, local);
        cpu_writel(t.qts, t.pio_cpu, t.head + 0x204 + p.device * 8,
                   device_control | (swap ? XBRIDGE_PIO_SWAP : 0));
        g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu,
                        t.head + 0x204 + p.device * 8) & XBRIDGE_PIO_SWAP,
                        ==, swap ? XBRIDGE_PIO_SWAP : 0);
        for (unsigned i = 0; i < G_N_ELEMENTS(words); i++) {
            cpu_writel(t.qts, t.pio_cpu, t.window + 0x800 + i * 4,
                       words[i]);
        }
        cpu_writel(t.qts, t.pio_cpu, t.head + 0x204 + p.device * 8,
                   device_control);

        /* Clear-swap byte PIO addresses expose each card SRAM byte. */
        for (unsigned i = 0; i < sizeof(seen); i++) {
            g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                          t.window + 0x800 + (i ^ 3), &seen[i], 1));
        }
        g_test_message("XBridge PIO swap %u: card SRAM "
                       "%02x %02x %02x %02x %02x %02x %02x %02x",
                       swap, seen[0], seen[1], seen[2], seen[3],
                       seen[4], seen[5], seen[6], seen[7]);
        g_assert_cmpmem(seen, sizeof(seen), card_bytes[swap],
                        sizeof(card_bytes[swap]));

        memory_write(&t, p.destination, destination, sentinel,
                     sizeof(sentinel));
        memory_write(&t, p.destination, destination + 0x6000, sentinel,
                     sizeof(sentinel));
        reg_write(&t, TI_DMA_WRITE_HOST_HI, address >> 32);
        reg_write(&t, TI_DMA_WRITE_HOST_LO, address);
        reg_write(&t, TI_DMA_WRITE_LOCAL, local);
        reg_write(&t, TI_DMA_WRITE_STATE, TI_DMA_NO_SWAP);
        reg_write(&t, TI_DMA_WRITE_LENGTH, sizeof(seen));
        reg_write(&t, TI_DMA_WRITE_STATE, TI_DMA_ACTIVE | TI_DMA_NO_SWAP);
        g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_STATE) & TI_DMA_ACTIVE,
                        ==, 0);
        g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_LENGTH), ==, 0);
        memory_read(&t, p.destination, destination, seen, sizeof(seen));
        g_test_message("XBridge PIO swap %u: host DMA "
                       "%02x %02x %02x %02x %02x %02x %02x %02x",
                       swap, seen[0], seen[1], seen[2], seen[3],
                       seen[4], seen[5], seen[6], seen[7]);
        g_assert_cmpmem(seen, sizeof(seen), host_bytes[swap],
                        sizeof(host_bytes[swap]));
        memory_read(&t, p.destination, destination + 0x6000,
                    seen, sizeof(seen));
        g_assert_cmpmem(seen, sizeof(seen), sentinel, sizeof(sentinel));
    }

    qtest_quit(t.qts);
    g_assert_cmpint(unlink(storage), ==, 0);
}

static void test_direct_dma_write(void)
{
    const Placement p = { .head = 2, .device = 1, .destination = 1,
                          .swap_clear = true };
    g_autofree char *storage = NULL;
    g_autofree char *dest_storage = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = &p };
    Tigon dest = { .placement = &p };
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    uint8_t source[16], guard[24], seen[24], migrated[24];
    /* This expansion slot selects the model's eight-byte DMA placement. */
    static const uint8_t wide_no_swap[16] = {
        0x4d, 0x54, 0x5b, 0x62, 0x31, 0x38, 0x3f, 0x46,
        0x85, 0x8c, 0x93, 0x9a, 0x69, 0x70, 0x77, 0x7e,
    };
    static const uint8_t swapped[16] = {
        0x62, 0x5b, 0x54, 0x4d, 0x46, 0x3f, 0x38, 0x31,
        0x9a, 0x93, 0x8c, 0x85, 0x7e, 0x77, 0x70, 0x69,
    };
    uint64_t address;
    int fd;

    fd = g_file_open_tmp("sn1-option-tigon-dma-XXXXXX", &storage, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    fd = g_file_open_tmp("sn1-option-tigon-dma-dest-XXXXXX",
                         &dest_storage, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    t.qts = qtest_initf(
        "-M origin300,topology=" TOPOLOGY
        ",expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s -nodefaults -display none "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device tigon,bus=%s,addr=%u,mac=" MAC,
        prom_path, storage, bus_names[p.head], p.device);
    setup_routes(&t);
    setup_card(&t);
    g_assert_cmphex(reg_read(&t, TI_PCI_STATE) & TI_PCI_32BIT_BUS, ==, 0);
    reg_write(&t, TI_DMA_WRITE_STATE, (1U << 28) | (1U << 22) |
              (1U << 21) | (1U << 12));
    g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_STATE) &
                    ((1U << 28) | (1U << 22) | (1U << 12)), ==, 0);
    g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_STATE) & (1U << 21), ==,
                    1U << 21);
    reg_write(&t, TI_DMA_READ_STATE,
              (1U << 28) | (1U << 22) | (1U << 21));
    g_assert_cmphex(reg_read(&t, TI_DMA_READ_STATE) &
                    ((1U << 28) | (1U << 22) | (1U << 21)), ==, 0);
    for (unsigned i = 0; i < sizeof(source); i++) {
        source[i] = 0x31 + i * 7;
    }
    memset(guard, 0x96, sizeof(guard));
    reg_write(&t, TI_PCI_STATE, TI_PCI_NO_WORD_SWAP);
    reg_write(&t, TI_WINBASE, 0x2000);
    g_assert_true(qtest_memwrite_cpu(t.qts, t.pio_cpu,
                                     t.window + 0x800, source,
                                     sizeof(source)));
    g_assert_cmphex(reg_read(&t, TI_PCI_STATE) & TI_PCI_NO_WORD_SWAP,
                    ==, TI_PCI_NO_WORD_SWAP);
    g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                                    t.window + 0x800, seen,
                                    sizeof(source)));
    g_assert_cmpmem(seen, sizeof(source), source, sizeof(source));
    memory_write(&t, p.destination, 0x2a0000 - 4,
                 guard, sizeof(guard));
    address = dma_address(&t, 0x2a0000);
    reg_write(&t, TI_DMA_WRITE_HOST_HI, address >> 32);
    reg_write(&t, TI_DMA_WRITE_HOST_LO, address);
    reg_write(&t, TI_DMA_WRITE_LOCAL, 0x2000);
    /* The manual conflicts on direct activation order; pin this model order. */
    reg_write(&t, TI_DMA_WRITE_STATE, TI_DMA_NO_SWAP);
    reg_write(&t, TI_DMA_WRITE_LENGTH, sizeof(source));
    memory_read(&t, p.destination, 0x2a0000 - 4, seen, sizeof(seen));
    g_assert_cmpmem(seen, sizeof(seen), guard, sizeof(guard));
    reg_write(&t, TI_DMA_WRITE_STATE, TI_DMA_ACTIVE | TI_DMA_NO_SWAP);
    g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_STATE) & TI_DMA_ACTIVE, ==, 0);
    g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_LENGTH), ==, 0);
    g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_HOST_HI), ==, address >> 32);
    g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_HOST_LO), ==,
                    (uint32_t)address + sizeof(source));
    g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_LOCAL), ==,
                    0x2000 + sizeof(source));
    memory_read(&t, p.destination, 0x2a0000 - 4, seen, sizeof(seen));
    g_assert_cmpmem(seen, 4, guard, 4);
    g_assert_cmpmem(seen + 4, sizeof(wide_no_swap), wide_no_swap,
                    sizeof(wide_no_swap));
    g_assert_cmpmem(seen + 20, 4, guard + 20, 4);

    memory_write(&t, p.destination, 0x2a0080 - 4,
                 guard, sizeof(guard));
    address = dma_address(&t, 0x2a0080);
    reg_write(&t, TI_DMA_WRITE_HOST_HI, address >> 32);
    reg_write(&t, TI_DMA_WRITE_HOST_LO, address);
    reg_write(&t, TI_DMA_WRITE_LOCAL, 0x2000);
    reg_write(&t, TI_DMA_WRITE_LENGTH, sizeof(source));
    reg_write(&t, TI_DMA_WRITE_STATE, TI_DMA_ACTIVE);
    memory_read(&t, p.destination, 0x2a0080 - 4, seen, sizeof(seen));
    g_assert_cmpmem(seen, 4, guard, 4);
    g_assert_cmpmem(seen + 4, sizeof(swapped), swapped, sizeof(swapped));
    g_assert_cmpmem(seen + 20, 4, guard + 20, 4);

    memory_write(&t, p.destination, 0x2a0100, source, sizeof(source));
    reg_write(&t, TI_WINBASE, 0x2100);
    g_assert_true(qtest_memwrite_cpu(t.qts, t.pio_cpu,
                                     t.window + 0x800, guard,
                                     sizeof(guard)));
    address = dma_address(&t, 0x2a0100);
    reg_write(&t, TI_DMA_READ_HOST_HI, address >> 32);
    reg_write(&t, TI_DMA_READ_HOST_LO, address);
    reg_write(&t, TI_DMA_READ_LOCAL, 0x2104);
    reg_write(&t, TI_DMA_READ_LENGTH, sizeof(source));
    reg_write(&t, TI_DMA_READ_STATE, TI_DMA_ACTIVE | TI_DMA_NO_SWAP);
    g_assert_cmphex(reg_read(&t, TI_DMA_READ_STATE) & TI_DMA_ACTIVE, ==, 0);
    g_assert_cmphex(reg_read(&t, TI_DMA_READ_LENGTH), ==, 0);
    g_assert_cmphex(reg_read(&t, TI_DMA_READ_HOST_HI), ==, address >> 32);
    g_assert_cmphex(reg_read(&t, TI_DMA_READ_HOST_LO), ==,
                    (uint32_t)address + sizeof(source));
    g_assert_cmphex(reg_read(&t, TI_DMA_READ_LOCAL), ==,
                    0x2104 + sizeof(source));
    g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                                    t.window + 0x800, seen,
                                    sizeof(seen)));
    g_assert_cmpmem(seen, 4, guard, 4);
    g_assert_cmpmem(seen + 4, sizeof(wide_no_swap), wide_no_swap,
                    sizeof(wide_no_swap));
    g_assert_cmpmem(seen + 20, 4, guard + 20, 4);

    memory_write(&t, p.destination, 0x2a0200 - 4,
                 guard, sizeof(guard));
    address = dma_address(&t, 0x2a0200);
    reg_write(&t, TI_DMA_WRITE_HOST_HI, address >> 32);
    reg_write(&t, TI_DMA_WRITE_HOST_LO, address);
    reg_write(&t, TI_DMA_WRITE_LOCAL, 0x100000);
    reg_write(&t, TI_DMA_WRITE_LENGTH, sizeof(source));
    reg_write(&t, TI_DMA_WRITE_STATE, TI_DMA_ACTIVE | TI_DMA_NO_SWAP);
    g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_LENGTH), ==, sizeof(source));
    memory_read(&t, p.destination, 0x2a0200 - 4, seen, sizeof(seen));
    g_assert_cmpmem(seen, sizeof(seen), guard, sizeof(guard));
    reg_write(&t, TI_DMA_WRITE_STATE, TI_DMA_RESET);
    g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_STATE) & TI_DMA_ACTIVE, ==, 0);

    reg_write(&t, TI_DMA_WRITE_HOST_LO, UINT32_MAX - 7);
    reg_write(&t, TI_DMA_WRITE_LOCAL, 0x2000);
    reg_write(&t, TI_DMA_WRITE_LENGTH, sizeof(source));
    reg_write(&t, TI_DMA_WRITE_STATE, TI_DMA_ACTIVE | TI_DMA_NO_SWAP);
    g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_STATE) &
                    TI_DMA_HOST_OVERFLOW, ==, TI_DMA_HOST_OVERFLOW);
    g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_LENGTH), ==, sizeof(source));
    reg_write(&t, TI_DMA_WRITE_STATE, TI_DMA_RESET);

    reg_write(&t, TI_DMA_WRITE_HOST_LO, address);
    reg_write(&t, TI_DMA_WRITE_LOCAL, 0x2000);
    reg_write(&t, TI_DMA_WRITE_LENGTH, sizeof(source));
    reg_write(&t, TI_DMA_WRITE_STATE,
              TI_DMA_ACTIVE | TI_DMA_NO_SWAP | TI_DMA_NO_MASTER);
    g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_LENGTH), ==, sizeof(source));
    memory_read(&t, p.destination, 0x2a0200 - 4, seen, sizeof(seen));
    g_assert_cmpmem(seen, sizeof(seen), guard, sizeof(guard));
    reg_write(&t, TI_DMA_WRITE_STATE, TI_DMA_RESET);

    reg_write(&t, TI_MISC_HOST_CTL, TI_MHC_MASK_INTS);
    address = dma_address(&t, 0x2a0200);
    reg_write(&t, TI_DMA_WRITE_HOST_HI, address >> 32);
    reg_write(&t, TI_DMA_WRITE_HOST_LO, address);
    reg_write(&t, TI_DMA_WRITE_LOCAL, 0x2000);
    reg_write(&t, TI_DMA_WRITE_LENGTH, sizeof(source));
    reg_write(&t, TI_DMA_WRITE_STATE,
              TI_DMA_ACTIVE | TI_DMA_NO_SWAP | TI_DMA_INT_ENABLE);
    expect_interrupt(&t, false);
    reg_write(&t, TI_MISC_HOST_CTL, 0);
    expect_interrupt(&t, true);
    acknowledge(&t);

    /* Byte PIO addresses on this XBridge head select the opposite word lane. */
    reg_write(&t, TI_WINBASE, 0x2000);
    for (unsigned i = 0; i < sizeof(source); i++) {
        uint8_t byte;

        g_assert_true(qtest_memwrite_cpu(t.qts, t.pio_cpu,
                         t.window + 0x800 + (i ^ 3), &source[i], 1));
        g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                         t.window + 0x800 + (i ^ 3), &byte, 1));
        g_assert_cmphex(byte, ==, source[i]);
    }
    memory_write(&t, p.destination, 0x2a0300, guard, sizeof(guard));
    address = DMA_PREFIX | memory_address(p.destination, 0x2a0301);
    reg_write(&t, TI_DMA_WRITE_HOST_HI, address >> 32);
    reg_write(&t, TI_DMA_WRITE_HOST_LO, address);
    reg_write(&t, TI_DMA_WRITE_LOCAL, 0x2001);
    reg_write(&t, TI_DMA_WRITE_LENGTH, 7);
    reg_write(&t, TI_DMA_WRITE_STATE, TI_DMA_ACTIVE | TI_DMA_NO_SWAP);
    g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_STATE) & TI_DMA_ACTIVE, ==, 0);
    memory_read(&t, p.destination, 0x2a0300, seen, sizeof(seen));
    g_assert_cmpmem(seen, 1, guard, 1);
    g_assert_cmpmem(seen + 1, 7, source + 1, 7);
    g_assert_cmpmem(seen + 8, sizeof(seen) - 8,
                    guard + 8, sizeof(guard) - 8);
    g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                                    t.window + 0x800, migrated,
                                    sizeof(migrated)));
    g_assert_cmphex(reg_read(&t, TI_PCI_STATE) & TI_PCI_32BIT_BUS, ==, 0);

    directory = g_dir_make_tmp("sn1-option-tigon-migration-XXXXXX", &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    dest.qts = qtest_initf(
        "-M origin300,topology=" TOPOLOGY
        ",expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s -nodefaults -display none "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device tigon,bus=%s,addr=%u,mac=" MAC " -incoming defer",
        prom_path, dest_storage, bus_names[p.head], p.device);
    qtest_qmp_assert_success(dest.qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(t.qts, uri);
    sgi_wait_migration(dest.qts, "completed");
    dest.pio_cpu = t.pio_cpu;
    dest.window = t.window;
    g_assert_cmphex(reg_read(&dest, TI_PCI_STATE) & TI_PCI_32BIT_BUS, ==, 0);
    g_assert_true(qtest_memread_cpu(dest.qts, dest.pio_cpu,
                                    dest.window + 0x800, seen,
                                    sizeof(seen)));
    g_assert_cmpmem(seen, sizeof(seen), migrated, sizeof(migrated));
    qtest_quit(dest.qts);
    qtest_quit(t.qts);
    g_rmdir(directory);
    g_assert_cmpint(unlink(storage), ==, 0);
    g_assert_cmpint(unlink(dest_storage), ==, 0);
}

static void test_traffic(const void *opaque)
{
    const Placement *p = opaque;
    g_autofree char *storage = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = p };
    uint8_t tx[60], rx[64], received[64], sentinel[64], seen[64];
    uint8_t guarded[80];
    const uint8_t mac[] = { 0x52, 0x54, 0, 0x12, 0x34, 0x56 };
    const uint32_t checked[] = {
        EVENT_RING, EVENT_PROD, TX_CONS, RETURN_RING, RETURN_PROD, RX_BUF,
    };
    unsigned tx_len = p->swap_clear ? 42 : sizeof(tx);
    unsigned rx_len = p->swap_clear ? 42 : sizeof(rx);
    unsigned tx_buf = TX_BUF + p->payload_offset;
    unsigned rx_buf = RX_BUF + p->payload_offset;
    uint32_t length;
    uint64_t address;
    uint32_t gib = p->gib_offset ? p->gib_offset : GIB;
    int fd, sv[2];
    unsigned other = 1 - p->destination;

    fd = g_file_open_tmp("sn1-option-tigon-storage-XXXXXX", &storage, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    t.qts = qtest_initf(
        "-M origin300,topology=" TOPOLOGY
        ",expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s -nodefaults -display none "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-netdev socket,id=n0,fd=%d -device tigon,bus=%s,addr=%u,"
        "netdev=n0,mac=" MAC,
        prom_path, storage, sv[1], bus_names[p->head], p->device);
    close(sv[1]);
    socket_timeout(sv[0]);
    setup_routes(&t);
    setup_card(&t);
    setup_rings(&t, gib);
    memset(sentinel, 0x96, sizeof(sentinel));
    for (unsigned i = 0; i < G_N_ELEMENTS(checked); i++) {
        memory_write(&t, other, checked[i], sentinel, sizeof(sentinel));
    }

    address = dma_address(&t, gib);
    if (p->gib_offset) {
        g_assert_cmphex(address, ==, UINT64_C(0xa100000001a00000));
    }
    reg_write(&t, TI_GCR_GENINFO_HI, address >> 32);
    reg_write(&t, TI_GCR_GENINFO_LO, address);
    reg_write(&t, TI_CPU_STATE, 0);
    memory_read(&t, p->destination, EVENT_RING, seen, 8);
    g_assert_cmphex(ldl_be_p(seen), ==, 0x01000000);
    g_assert_cmphex(read_memory_word(&t, EVENT_PROD), ==, 1);
    expect_interrupt(&t, true);
    reg_write(&t, TI_GCR_EVENTCONS_IDX, 1);
    acknowledge(&t);

    for (unsigned i = 0; i < sizeof(tx); i++) {
        tx[i] = i ^ (0x35 + p->head + p->device + p->destination);
    }
    memset(tx, 0xff, 6);
    memcpy(tx + 6, mac, sizeof(mac));
    tx[12] = 8;
    tx[13] = 6;
    memory_write(&t, p->destination, tx_buf, tx, tx_len);
    host_address(&t, TX_RING, tx_buf);
    memory_word(&t, TX_RING + 8, (tx_len << 16) | TI_BDFLAG_END);
    reg_write(&t, TI_MB_SENDPROD_IDX, 1);
    g_assert_cmpint(recv(sv[0], &length, sizeof(length), MSG_WAITALL), ==, 4);
    g_assert_cmpuint(ntohl(length), ==, tx_len);
    g_assert_cmpint(recv(sv[0], received, tx_len, MSG_WAITALL), ==, tx_len);
    g_assert_cmpmem(received, tx_len, tx, tx_len);
    g_assert_cmphex(read_memory_word(&t, TX_CONS), ==, 1);
    expect_interrupt(&t, true);
    acknowledge(&t);

    for (unsigned i = 0; i < sizeof(rx); i++) {
        rx[i] = (i * 13) ^ (0xa7 + p->head + p->device + p->destination);
    }
    memcpy(rx, mac, sizeof(mac));
    rx[12] = 8;
    rx[13] = 6;
    memset(guarded, 0x96, sizeof(guarded));
    memory_write(&t, p->destination, RX_BUF - 4, guarded, sizeof(guarded));
    host_address(&t, STD_RX_RING, rx_buf);
    memory_word(&t, STD_RX_RING + 8, 2048);
    memory_word(&t, STD_RX_RING + 12, TI_BDTYPE_RECV_BD << 16);
    reg_write(&t, TI_MB_STDRXPROD_IDX, 1);
    length = htonl(rx_len);
    g_assert_cmpint(send(sv[0], &length, sizeof(length), 0), ==, 4);
    g_assert_cmpint(send(sv[0], rx, rx_len, 0), ==, rx_len);
    for (unsigned attempt = 0; attempt < 200; attempt++) {
        if (read_memory_word(&t, RETURN_PROD) != 0) {
            break;
        }
        g_usleep(5000);
        qtest_clock_step(t.qts, 50000);
    }
    g_assert_cmphex(read_memory_word(&t, RETURN_PROD), ==, 1);
    g_assert_cmphex(read_memory_word(&t, RETURN_RING + 8), ==, rx_len);
    memory_read(&t, p->destination, rx_buf, received, rx_len);
    g_assert_cmpmem(received, rx_len, rx, rx_len);
    memory_read(&t, p->destination, RX_BUF - 4, seen,
                4 + p->payload_offset);
    g_assert_cmpmem(seen, 4 + p->payload_offset, guarded,
                    4 + p->payload_offset);
    memory_read(&t, p->destination, rx_buf + rx_len, seen, 2);
    g_assert_cmpmem(seen, 2, guarded, 2);
    expect_interrupt(&t, true);
    acknowledge(&t);
    for (unsigned i = 0; i < G_N_ELEMENTS(checked); i++) {
        memory_read(&t, other, checked[i], seen, sizeof(seen));
        g_assert_cmpmem(seen, sizeof(seen), sentinel, sizeof(sentinel));
    }
    g_test_message("%s device%u -> NASID%u TX/RX bytes, ring DMA, "
                   "IRQ isolation OK",
                   bus_names[p->head], p->device, p->destination * 2);
    qtest_quit(t.qts);
    close(sv[0]);
    g_assert_cmpint(unlink(storage), ==, 0);
}

static void test_pci_state_width_migration(void)
{
    const Placement p = { .head = 2, .device = 1, .destination = 1 };
    g_autofree char *storage = NULL;
    g_autofree char *dest_storage = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = &p };
    Tigon dest = { .placement = &p };
    int fd;

    fd = g_file_open_tmp("sn1-option-tigon-width-src-XXXXXX",
                         &storage, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    fd = g_file_open_tmp("sn1-option-tigon-width-dest-XXXXXX",
                         &dest_storage, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);

    t.qts = qtest_initf(
        "-M origin300,topology=" TOPOLOGY
        ",expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s -nodefaults -display none "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device tigon,bus=%s,addr=%u,mac=" MAC,
        prom_path, storage, bus_names[p.head], p.device);
    setup_routes(&t);
    setup_card(&t);
    g_assert_cmphex(reg_read(&t, TI_PCI_STATE) & TI_PCI_32BIT_BUS, ==, 0);

    directory = g_dir_make_tmp("sn1-option-tigon-width-migration-XXXXXX",
                               &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    dest.qts = qtest_initf(
        "-M origin300,topology=" TOPOLOGY
        ",expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s -nodefaults -display none "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device tigon,bus=%s,addr=%u,mac=" MAC " -incoming defer",
        prom_path, dest_storage, bus_names[p.head], p.device);
    qtest_qmp_assert_success(dest.qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(t.qts, uri);
    sgi_wait_migration(dest.qts, "completed");
    dest.pio_cpu = t.pio_cpu;
    dest.window = t.window;
    g_assert_cmphex(reg_read(&dest, TI_PCI_STATE) & TI_PCI_32BIT_BUS, ==, 0);

    qtest_quit(dest.qts);
    qtest_quit(t.qts);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_assert_cmpint(unlink(storage), ==, 0);
    g_assert_cmpint(unlink(dest_storage), ==, 0);
}

static void test_pci_state_width(void)
{
    const Placement p = { .head = 2, .device = 1, .destination = 1 };
    g_autofree char *storage = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = &p };
    int fd;

    fd = g_file_open_tmp("sn1-option-tigon-width-XXXXXX", &storage, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    t.qts = qtest_initf(
        "-M origin300,topology=" TOPOLOGY
        ",expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s -nodefaults -display none "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device tigon,bus=%s,addr=%u,mac=" MAC,
        prom_path, storage, bus_names[p.head], p.device);
    setup_routes(&t);
    setup_card(&t);
    g_assert_cmphex(reg_read(&t, TI_PCI_STATE) & TI_PCI_32BIT_BUS, ==, 0);
    reg_write(&t, TI_PCI_STATE, TI_PCI_32BIT_BUS);
    g_assert_cmphex(reg_read(&t, TI_PCI_STATE) & TI_PCI_32BIT_BUS, ==,
                    TI_PCI_32BIT_BUS);
    qtest_system_reset(t.qts);
    setup_routes(&t);
    setup_card(&t);
    g_assert_cmphex(reg_read(&t, TI_PCI_STATE) & TI_PCI_32BIT_BUS, ==, 0);
    qtest_quit(t.qts);
    g_assert_cmpint(unlink(storage), ==, 0);
}

int main(int argc, char **argv)
{
    static Placement placements[] = {
        { .head = 0, .device = 1, .destination = 0 },
        { .head = 1, .device = 2, .destination = 1 },
        { .head = 2, .device = 1, .destination = 1 },
        { .head = 3, .device = 2, .destination = 0 },
        { .head = 4, .device = 1, .destination = 0 },
        { .head = 5, .device = 2, .destination = 1 },
        { .head = 6, .device = 1, .destination = 1 },
        { .head = 7, .device = 2, .destination = 0 },
    };
    static Placement swap_clear[2][2];
    static Placement geninfo_64bit = {
        .head = 0,
        .device = 1,
        .swap_clear = true,
        .gib_offset = 0x01a00000,
    };
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *prom = g_malloc0(PROM_SIZE);
    int fd, result;

    g_test_init(&argc, &argv, NULL);
    if (!qtest_has_device("tigon")) {
        return 0;
    }
    fd = g_file_open_tmp("sn1-option-tigon-prom-XXXXXX", &prom_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, prom, PROM_SIZE), ==, PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    for (unsigned i = 0; i < G_N_ELEMENTS(placements); i++) {
        Placement *p = &placements[i];
        g_autofree char *name = g_strdup_printf(
            "/sn1/option-tigon/head%u/device%u/node%u",
            p->head, p->device, p->destination);

        qtest_add_data_func(name, p, test_traffic);
    }
    for (unsigned destination = 0; destination < 2; destination++) {
        for (unsigned offset = 0; offset <= 2; offset += 2) {
            Placement *p = &swap_clear[destination][offset / 2];
            g_autofree char *name = g_strdup_printf(
                "/sn1/option-tigon/swap-clear/%s/offset%u",
                destination ? "remote" : "local", offset);

            *p = (Placement) {
                .head = 0,
                .device = 1,
                .destination = destination,
                .swap_clear = true,
                .payload_offset = offset,
            };
            qtest_add_data_func(name, p, test_traffic);
        }
    }
    qtest_add_data_func("/sn1/option-tigon/geninfo-64bit", &geninfo_64bit,
                        test_traffic);
    qtest_add_func("/sn1/option-tigon/pci-state-width",
                   test_pci_state_width);
    qtest_add_func("/sn1/option-tigon/pci-state-width-migration",
                   test_pci_state_width_migration);
    qtest_add_func("/sn1/option-tigon/direct-dma-write",
                   test_direct_dma_write);
    qtest_add_func("/sn1/option-tigon/pio-sram-direct-dma-bytes",
                   test_pio_sram_direct_dma_bytes);
    result = g_test_run();
    g_assert_cmpint(unlink(prom_path), ==, 0);
    g_free(prom_path);
    return result;
}
