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
#include "qobject/qdict.h"
#include "exec/memattrs.h"
#include "hw/pci/pci_regs.h"
#include "sgi-test.h"
#include "sgi-migrate.h"
#include <sys/socket.h>
#include <netinet/in.h>

#define TOPOLOGY "origin300-numalink-pci-expansion-experiment"
#define MAC "52:54:00:12:34:56"
#define NODE(n) ((uint64_t)(n) << 33)
#define BEDROCK_LOCAL (SPACE_IO + UINT64_C(0x01000000))
#define HUB_ECC (SPACE_HUB + UINT64_C(0x140000000))
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
#define XBRIDGE_DIRECT_SWAP (1U << 19)
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
#define TX_FAULT_DESC 0x0ffffff8

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
    bool rx_descriptor_only;
    bool tx_only;
    unsigned payload_length;
    bool trace_dma;
    bool data_byteswap;
    bool pci_bit20;
    bool clear_desc_word_swap;
    bool set_desc_byte_swap;
    bool payload_identity;
} Placement;

typedef struct {
    QTestState *qts;
    const Placement *placement;
    unsigned pio_cpu;
    uint64_t head;
    uint64_t window;
} Tigon;

typedef struct {
    uint32_t pci_state;
    uint32_t opmode;
    bool expect_event;
} WideSelector;

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
    bool payload = offset == TX_BUF + t->placement->payload_offset ||
                   offset == RX_BUF + t->placement->payload_offset;
    uint64_t prefix = t->placement->swap_clear &&
                      !(t->placement->payload_identity && payload) ?
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

static void legacy_tx_descriptor(Tigon *t, uint32_t offset,
                                 uint32_t buf, uint32_t word,
                                 bool bigendian)
{
    uint8_t pci[12], host[8], tail[4];
    uint64_t address = dma_address(t, buf);

    for (unsigned i = 0; i < 3; i++) {
        uint32_t value = i == 0 ? address >> 32 :
                         i == 1 ? address : word;

        if (bigendian) {
            stl_be_p(pci + i * 4, value);
        } else {
            stl_le_p(pci + i * 4, value);
        }
    }
    for (unsigned i = 0; i < sizeof(host); i++) {
        host[i] = pci[7 - i];
    }
    for (unsigned i = 0; i < sizeof(tail); i++) {
        tail[i] = pci[11 - i];
    }
    memory_write(t, t->placement->destination, offset, host, sizeof(host));
    memory_write(t, t->placement->destination, offset + 12, tail,
                 sizeof(tail));
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
    t->window = t->head + (t->placement->device == 1 ? 0x400000 :
                           0x600000 + (t->placement->device - 2) * 0x100000);
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

static void test_pio_sram_direct_dma_bytes_one(unsigned device)
{
    const Placement p = { .head = 2, .device = device, .destination = 1,
                          .swap_clear = true };
    static const uint32_t words[2] = { 0x17395bc1, 0x2e4068af };
    static const uint8_t card_bytes[2][8] = {
        { 0xc1, 0x5b, 0x39, 0x17, 0xaf, 0x68, 0x40, 0x2e },
        { 0x17, 0x39, 0x5b, 0xc1, 0x2e, 0x40, 0x68, 0xaf },
    };
    static const uint8_t narrow_host_bytes[2][8] = {
        { 0x17, 0x39, 0x5b, 0xc1, 0x2e, 0x40, 0x68, 0xaf },
        { 0xc1, 0x5b, 0x39, 0x17, 0xaf, 0x68, 0x40, 0x2e },
    };
    static const uint8_t wide_host_bytes[2][8] = {
        { 0x2e, 0x40, 0x68, 0xaf, 0x17, 0x39, 0x5b, 0xc1 },
        { 0xaf, 0x68, 0x40, 0x2e, 0xc1, 0x5b, 0x39, 0x17 },
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
    g_assert_cmphex(reg_read(&t, TI_PCI_STATE) & TI_PCI_32BIT_BUS, ==,
                    device == 1 ? 0 : TI_PCI_32BIT_BUS);
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

        memory_write(&t, p.destination, destination - sizeof(sentinel),
                     sentinel, sizeof(sentinel));
        memory_write(&t, p.destination, destination, sentinel,
                     sizeof(sentinel));
        memory_write(&t, p.destination, destination + sizeof(seen), sentinel,
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
        if (device == 1) {
            g_assert_cmpmem(seen, sizeof(seen), wide_host_bytes[swap],
                            sizeof(wide_host_bytes[swap]));
        } else {
            g_assert_cmpmem(seen, sizeof(seen), narrow_host_bytes[swap],
                            sizeof(narrow_host_bytes[swap]));
        }
        memory_read(&t, p.destination, destination - sizeof(sentinel),
                    seen, sizeof(seen));
        g_assert_cmpmem(seen, sizeof(seen), sentinel, sizeof(sentinel));
        memory_read(&t, p.destination, destination + sizeof(seen),
                    seen, sizeof(seen));
        g_assert_cmpmem(seen, sizeof(seen), sentinel, sizeof(sentinel));
        memory_read(&t, p.destination, destination + 0x6000,
                    seen, sizeof(seen));
        g_assert_cmpmem(seen, sizeof(seen), sentinel, sizeof(sentinel));
    }

    qtest_quit(t.qts);
    g_assert_cmpint(unlink(storage), ==, 0);
}

static void test_pio_sram_direct_dma_bytes(void)
{
    test_pio_sram_direct_dma_bytes_one(1);
    test_pio_sram_direct_dma_bytes_one(3);
}

static void test_mem64_write_width_one(bool unaligned_only)
{
    const Placement p = { .head = 2, .device = 1, .destination = 1 };
    const uint8_t initial[8] = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
    };
    const uint8_t payload[2][4] = {
        { 0xcb, 0xba, 0xa9, 0x98 },
        { 0x98, 0xa9, 0xba, 0xcb },
    };
    const uint8_t extended[16] = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
        0x19, 0x2a, 0x3b, 0x4c, 0x5d, 0x6e, 0x7f, 0x80,
    };
    g_autofree char *storage = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = &p };
    uint8_t seen[sizeof(extended)], expected[sizeof(extended)];
    uint64_t mem64 = SPACE_IO + NODE(7) + 0x20000000ULL +
                     ADAPTER_MMIO + 0xa00;
    uint32_t device_control, control;
    int fd;

    fd = g_file_open_tmp("sn1-option-tigon-pio-write-width-XXXXXX",
                         &storage, &error);
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
    reg_write(&t, TI_WINBASE, 0x70000);
    device_control = cpu_readl(t.qts, t.pio_cpu, t.head + 0x20c);
    control = cpu_readl(t.qts, t.pio_cpu, t.head + 0x24);
    g_assert_cmphex(device_control & (1U << 22), ==, 0);
    g_assert_cmphex(control & (1U << 22), ==, 0);
    cpu_writeq(t.qts, t.pio_cpu, SPACE_IO + NODE(7) + 0x01c00160,
               0x0e04);
    if (!unaligned_only) {
        for (unsigned swap = 0; swap < 2; swap++) {
            for (unsigned wide = 0; wide < 2; wide++) {
                for (unsigned half = 0; half < 2; half++) {
                    unsigned target = half ^ (wide && !swap);

                    for (unsigned i = 0; i < sizeof(initial); i++) {
                        g_assert_true(qtest_memwrite_cpu(t.qts, t.pio_cpu,
                            t.window + 0xa00 + (i ^ 3), &initial[i], 1));
                        g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                            t.window + 0xa00 + (i ^ 3), &seen[i], 1));
                    }
                    g_assert_cmpmem(seen, sizeof(initial), initial,
                                    sizeof(initial));
                    cpu_writel(t.qts, t.pio_cpu, t.head + 0x24,
                               (control & ~(1U << 22)) | (swap << 22));
                    cpu_writel(t.qts, t.pio_cpu, t.head + 0x20c,
                               (device_control & ~(1U << 22)) | (wide << 22));
                    cpu_writel(t.qts, t.pio_cpu, mem64 + half * 4, 0x98a9bacb);
                    cpu_writel(t.qts, t.pio_cpu, t.head + 0x20c,
                               device_control);
                    cpu_writel(t.qts, t.pio_cpu, t.head + 0x24, control);
                    for (unsigned i = 0; i < sizeof(initial); i++) {
                        g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                            t.window + 0xa00 + (i ^ 3), &seen[i], 1));
                    }
                    memcpy(expected, initial, sizeof(initial));
                    memcpy(expected + target * 4, payload[swap], 4);
                    g_assert_cmpmem(seen, sizeof(initial), expected,
                                    sizeof(initial));
                }
            }
        }
    }
    for (unsigned i = 0; i < sizeof(extended); i++) {
        g_assert_true(qtest_memwrite_cpu(t.qts, t.pio_cpu,
                      t.window + 0xa00 + (i ^ 3), &extended[i], 1));
    }
    cpu_writel(t.qts, t.pio_cpu, mem64 + 1, 0x98a9bacb);
    for (unsigned i = 0; i < sizeof(extended); i++) {
        g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                      t.window + 0xa00 + (i ^ 3), &seen[i], 1));
    }
    memcpy(expected, extended, sizeof(extended));
    /* The unaligned write is split into bytes; each keeps width eight. */
    memcpy(expected + 3, payload[0], 4);
    g_assert_cmpmem(seen, sizeof(extended), expected, sizeof(extended));

    if (!unaligned_only) {
        for (unsigned i = 0; i < sizeof(initial); i++) {
            g_assert_true(qtest_memwrite_cpu(t.qts, t.pio_cpu,
                          t.window + 0xa00 + (i ^ 3), &initial[i], 1));
        }
        cpu_writel(t.qts, t.pio_cpu, t.head + 0x20c,
                   device_control | (1U << 22));
        cpu_writeq(t.qts, t.pio_cpu, SPACE_IO + NODE(7) + 0x01c00160,
                   0x0e02);
        cpu_writel(t.qts, t.pio_cpu, mem64, 0x98a9bacb);
        cpu_writeq(t.qts, t.pio_cpu, SPACE_IO + NODE(7) + 0x01c00160,
                   0x0e04);
        cpu_writel(t.qts, t.pio_cpu, t.head + 0x20c, device_control);
        for (unsigned i = 0; i < sizeof(initial); i++) {
            g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                          t.window + 0xa00 + (i ^ 3), &seen[i], 1));
        }
        memcpy(expected, initial, sizeof(initial));
        memcpy(expected, payload[0], 4);
        g_assert_cmpmem(seen, sizeof(initial), expected, sizeof(initial));

        for (unsigned i = 0; i < sizeof(initial); i++) {
            g_assert_true(qtest_memwrite_cpu(t.qts, t.pio_cpu,
                          t.window + 0xa00 + (i ^ 3), &initial[i], 1));
        }
        cpu_writel(t.qts, t.pio_cpu, t.window + 0xa00, 0x98a9bacb);
        for (unsigned i = 0; i < sizeof(initial); i++) {
            g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                          t.window + 0xa00 + (i ^ 3), &seen[i], 1));
        }
        g_assert_cmpmem(seen, sizeof(initial), expected, sizeof(initial));

        for (unsigned i = 0; i < sizeof(extended); i++) {
            g_assert_true(qtest_memwrite_cpu(t.qts, t.pio_cpu,
                          t.window + 0xa00 + (i ^ 3), &extended[i], 1));
        }
        cpu_writeq(t.qts, t.pio_cpu, mem64 + 8,
                   UINT64_C(0x0123456789abcdef));
        for (unsigned i = 0; i < sizeof(extended); i++) {
            g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                          t.window + 0xa00 + (i ^ 3), &seen[i], 1));
        }
        memcpy(expected, extended, sizeof(extended));
        memcpy(expected + 8, (uint8_t[]){
            0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01,
        }, 8);
        g_assert_cmpmem(seen, sizeof(extended), expected, sizeof(extended));
    }
    qtest_quit(t.qts);
    g_assert_cmpint(unlink(storage), ==, 0);
}

static void test_mem64_write_width(void)
{
    test_mem64_write_width_one(false);
}

static void test_mem64_write_unaligned(void)
{
    test_mem64_write_width_one(true);
}

static void test_mem64_device_width(void)
{
    const Placement p = { .head = 2, .device = 1, .destination = 1 };
    const uint8_t card_bytes[16] = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
        0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f,
    };
    const uint32_t expected[2][2][4] = {
        {
            { 0x43322110, 0x87766554, 0xcbbaa998, 0x0ffeeddc },
            { 0x87766554, 0x43322110, 0x0ffeeddc, 0xcbbaa998 },
        },
        {
            { 0x10213243, 0x54657687, 0x98a9bacb, 0xdcedfe0f },
            { 0x10213243, 0x54657687, 0x98a9bacb, 0xdcedfe0f },
        },
    };
    g_autofree char *storage = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = &p };
    uint8_t seen[sizeof(card_bytes)];
    uint64_t mem64;
    uint32_t device_control, control;
    int fd;

    fd = g_file_open_tmp("sn1-option-tigon-pio-width-XXXXXX", &storage,
                         &error);
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
    reg_write(&t, TI_WINBASE, 0x70000);
    for (unsigned i = 0; i < sizeof(card_bytes); i++) {
        g_assert_true(qtest_memwrite_cpu(t.qts, t.pio_cpu,
                      t.window + 0x800 + (i ^ 3), &card_bytes[i], 1));
        g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                      t.window + 0x800 + (i ^ 3), &seen[i], 1));
    }
    g_assert_cmpmem(seen, sizeof(seen), card_bytes, sizeof(card_bytes));
    device_control = cpu_readl(t.qts, t.pio_cpu, t.head + 0x20c);
    control = cpu_readl(t.qts, t.pio_cpu, t.head + 0x24);
    mem64 = SPACE_IO + NODE(7) + 0x20000000ULL + ADAPTER_MMIO + 0x800;
    cpu_writeq(t.qts, t.pio_cpu, SPACE_IO + NODE(7) + 0x01c00160,
               0x0e04);
    g_assert_cmphex(cpu_readq(t.qts, t.pio_cpu, mem64), ==,
                    UINT64_C(0x8776655443322110));
    g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, mem64 + 1), ==,
                    0x76655443);
    cpu_writel(t.qts, t.pio_cpu, mem64 + 0x24, 0x11223344);
    cpu_writeq(t.qts, t.pio_cpu, mem64 + 0x30,
               UINT64_C(0x0123456789abcdef));
    {
        const uint8_t word_bytes[4] = { 0x44, 0x33, 0x22, 0x11 };
        const uint8_t wide_bytes[8] = {
            0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01,
        };

        for (unsigned i = 0; i < sizeof(word_bytes); i++) {
            g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                          t.window + 0x824 + (i ^ 3), &seen[i], 1));
        }
        g_assert_cmpmem(seen, sizeof(word_bytes),
                        word_bytes, sizeof(word_bytes));
        for (unsigned i = 0; i < sizeof(wide_bytes); i++) {
            g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                          t.window + 0x830 + (i ^ 3), &seen[i], 1));
        }
        g_assert_cmpmem(seen, sizeof(wide_bytes),
                        wide_bytes, sizeof(wide_bytes));
    }
    cpu_writel(t.qts, t.pio_cpu,
               t.head + 0x20000 + p.device * 0x1000 + PCI_COMMAND, 0);
    g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, mem64), ==, 0);
    cpu_writel(t.qts, t.pio_cpu,
               t.head + 0x20000 + p.device * 0x1000 + PCI_COMMAND,
               PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    for (unsigned swap = 0; swap < 2; swap++) {
        cpu_writel(t.qts, t.pio_cpu, t.head + 0x24,
                   (control & ~(1U << 22)) | (swap << 22));
        for (unsigned width = 0; width < 2; width++) {
            cpu_writel(t.qts, t.pio_cpu, t.head + 0x20c,
                       (device_control & ~(1U << 22)) | (width << 22));
            cpu_writeq(t.qts, t.pio_cpu,
                       SPACE_IO + NODE(7) + 0x01c00160, 0x0e02);
            for (unsigned i = 0; i < 4; i++) {
                g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu,
                                mem64 + i * 4), ==, expected[swap][0][i]);
            }
            cpu_writeq(t.qts, t.pio_cpu,
                       SPACE_IO + NODE(7) + 0x01c00160, 0x0e04);
            g_assert_cmphex(cpu_readq(t.qts, t.pio_cpu,
                            SPACE_IO + NODE(7) + 0x01c00160), ==, 0x0e04);
            for (unsigned i = 0; i < 4; i++) {
                g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu,
                                mem64 + i * 4), ==,
                                expected[swap][width][i]);
            }
        }
    }
    cpu_writel(t.qts, t.pio_cpu, t.head + 0x20c, device_control);
    cpu_writel(t.qts, t.pio_cpu, t.head + 0x24, control);
    for (unsigned i = 0; i < sizeof(card_bytes); i++) {
        g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                      t.window + 0x800 + (i ^ 3), &seen[i], 1));
    }
    g_assert_cmpmem(seen, sizeof(seen), card_bytes, sizeof(card_bytes));
    qtest_quit(t.qts);
    g_assert_cmpint(unlink(storage), ==, 0);
}

static void test_mem64_other_head(void)
{
    const Placement p = { .head = 3, .device = 1, .destination = 1 };
    const uint8_t card_bytes[8] = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
    };
    const uint8_t payload[4] = { 0xcb, 0xba, 0xa9, 0x98 };
    g_autofree char *storage = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = &p };
    uint64_t mem64 = SPACE_IO + NODE(7) + 0x20000000ULL +
                     ADAPTER_MMIO + 0x800;
    int fd;

    fd = g_file_open_tmp("sn1-option-tigon-other-head-XXXXXX", &storage,
                         &error);
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
    reg_write(&t, TI_WINBASE, 0x70000);
    for (unsigned i = 0; i < sizeof(card_bytes); i++) {
        g_assert_true(qtest_memwrite_cpu(t.qts, t.pio_cpu,
                      t.window + 0x800 + (i ^ 3), &card_bytes[i], 1));
    }
    cpu_writeq(t.qts, t.pio_cpu, SPACE_IO + NODE(7) + 0x01c00160,
               0x0f04);
    g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, mem64), ==, 0x87766554);
    g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, mem64 + 4), ==,
                    0x43322110);
    cpu_writel(t.qts, t.pio_cpu, mem64, 0x98a9bacb);
    for (unsigned i = 0; i < sizeof(card_bytes); i++) {
        uint8_t value;

        g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                      t.window + 0x800 + (i ^ 3), &value, 1));
        g_assert_cmphex(value, ==,
                        i < 4 ? card_bytes[i] :
                        payload[i - 4]);
    }
    qtest_quit(t.qts);
    g_assert_cmpint(unlink(storage), ==, 0);
}

static void test_mem64_overlapping_bars(void)
{
    const Placement p = { .head = 2, .device = 1, .destination = 1 };
    g_autofree char *storage = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = &p };
    uint64_t config2, mem32, mem64;
    uint32_t first, second;
    int fd;

    fd = g_file_open_tmp("sn1-option-tigon-overlap-XXXXXX", &storage,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    t.qts = qtest_initf(
        "-M origin300,topology=" TOPOLOGY
        ",expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s -nodefaults -display none "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device tigon,bus=%s,addr=1,mac=" MAC " "
        "-device sgi-bridge-dma-test-64bit,bus=%s,addr=2",
        prom_path, storage, bus_names[p.head], bus_names[p.head]);
    setup_routes(&t);
    setup_card(&t);
    reg_write(&t, TI_WINBASE, 0x70000);
    cpu_writel(t.qts, t.pio_cpu, t.window + 0x820, 0x11223344);
    cpu_writel(t.qts, t.pio_cpu, t.window + 0x824, 0xaabbccdd);
    config2 = t.head + 0x20000 + 2 * 0x1000;
    cpu_writel(t.qts, t.pio_cpu, config2 + PCI_BASE_ADDRESS_0,
               ADAPTER_MMIO + 0x800);
    cpu_writel(t.qts, t.pio_cpu, config2 + PCI_COMMAND,
               PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    cpu_writeq(t.qts, t.pio_cpu, SPACE_IO + NODE(7) + 0x01c00160,
               0x0e02);
    mem32 = SPACE_IO + NODE(7) + 0x20000000ULL +
            ADAPTER_MMIO + 0x820;
    first = cpu_readl(t.qts, t.pio_cpu, mem32);
    second = cpu_readl(t.qts, t.pio_cpu, mem32 + 4);
    g_assert_cmphex(first, !=, second);
    cpu_writeq(t.qts, t.pio_cpu, SPACE_IO + NODE(7) + 0x01c00160,
               0x0e04);
    mem64 = mem32;
    g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, mem64), ==, second);
    g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, mem64 + 4), ==, first);
    cpu_writel(t.qts, t.pio_cpu, mem64, 0x98a9bacb);
    cpu_writeq(t.qts, t.pio_cpu,
               SPACE_IO + NODE(7) + 0x01c00160, 0x0e02);
    g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, mem32 + 4), ==,
                    0x98a9bacb);
    qtest_quit(t.qts);
    g_assert_cmpint(unlink(storage), ==, 0);
}

static void test_mem64_failed_read(void)
{
    const Placement p = { .head = 2, .device = 1, .destination = 1 };
    g_autofree char *storage = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = &p };
    uint64_t config, mem64;
    QDict *reply;
    int fd;

    fd = g_file_open_tmp("sn1-option-mem64-fault-XXXXXX", &storage,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    t.qts = qtest_initf(
        "-M origin300,topology=" TOPOLOGY
        ",expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s -nodefaults -display none "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device sgi-bridge-dma-test-64bit,id=fault,bus=%s,addr=1,"
        "x-test-fault-read=on",
        prom_path, storage, bus_names[p.head]);
    setup_routes(&t);
    config = t.head + 0x20000 + p.device * 0x1000;
    cpu_writel(t.qts, t.pio_cpu, config + PCI_BASE_ADDRESS_0,
               ADAPTER_MMIO);
    cpu_writel(t.qts, t.pio_cpu, config + PCI_COMMAND,
               PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    cpu_writel(t.qts, t.pio_cpu, t.head + 0x20c,
               DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20));
    cpu_writeq(t.qts, t.pio_cpu, SPACE_IO + NODE(7) + 0x01c00160,
               0x0e04);
    mem64 = SPACE_IO + NODE(7) + 0x20000000ULL + ADAPTER_MMIO;
    g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, mem64 + 0x20), ==,
                    0x12345678);
    reply = qtest_qmp(t.qts, "{'execute':'qom-get','arguments':"
        "{'path':'/machine/peripheral/fault',"
        "'property':'x-test-bar-read-count'}}");
    g_assert_true(qdict_haskey(reply, "return"));
    g_assert_cmphex(qdict_get_int(reply, "return"), ==, 1);
    qobject_unref(reply);
    g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, mem64 + 0x3c), ==, 0);
    reply = qtest_qmp(t.qts, "{'execute':'qom-get','arguments':"
        "{'path':'/machine/peripheral/fault',"
        "'property':'x-test-bar-read-count'}}");
    g_assert_true(qdict_haskey(reply, "return"));
    g_assert_cmphex(qdict_get_int(reply, "return"), ==, 2);
    qobject_unref(reply);
    reply = qtest_qmp(t.qts, "{'execute':'qom-get','arguments':"
        "{'path':'/machine/peripheral/fault',"
        "'property':'x-test-fault-result'}}");
    g_assert_cmphex(qdict_get_int(reply, "return"), ==, MEMTX_ERROR);
    qobject_unref(reply);
    qtest_quit(t.qts);
    g_assert_cmpint(unlink(storage), ==, 0);
}

static void test_mem64_write_callback(void)
{
    const Placement p = { .head = 2, .device = 1, .destination = 1 };
    g_autofree char *storage = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = &p };
    uint64_t config, mem64;
    QDict *reply;
    int fd;

    fd = g_file_open_tmp("sn1-option-mem64-write-fault-XXXXXX", &storage,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    t.qts = qtest_initf(
        "-M origin300,topology=" TOPOLOGY
        ",expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s -nodefaults -display none "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device sgi-bridge-dma-test-64bit,id=fault,bus=%s,addr=1,"
        "x-test-fault-write=on",
        prom_path, storage, bus_names[p.head]);
    setup_routes(&t);
    config = t.head + 0x20000 + p.device * 0x1000;
    cpu_writel(t.qts, t.pio_cpu, config + PCI_BASE_ADDRESS_0,
               ADAPTER_MMIO);
    cpu_writel(t.qts, t.pio_cpu, config + PCI_COMMAND,
               PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    cpu_writel(t.qts, t.pio_cpu, t.head + 0x20c,
               DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20));
    cpu_writeq(t.qts, t.pio_cpu, SPACE_IO + NODE(7) + 0x01c00160,
               0x0e04);
    mem64 = SPACE_IO + NODE(7) + 0x20000000ULL + ADAPTER_MMIO;
    cpu_writel(t.qts, t.pio_cpu, mem64 + 0x24, 0x98a9bacb);
    reply = qtest_qmp(t.qts, "{'execute':'qom-get','arguments':"
        "{'path':'/machine/peripheral/fault',"
        "'property':'x-test-bar-write-count'}}");
    g_assert_true(qdict_haskey(reply, "return"));
    g_assert_cmphex(qdict_get_int(reply, "return"), ==, 1);
    qobject_unref(reply);
    cpu_writeq(t.qts, t.pio_cpu, SPACE_IO + NODE(7) + 0x01c00160,
               0x0e02);
    g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, mem64 + 0x24), ==,
                    0x98a9bacb);
    cpu_writeq(t.qts, t.pio_cpu, SPACE_IO + NODE(7) + 0x01c00160,
               0x0e04);
    cpu_writel(t.qts, t.pio_cpu, mem64 + 0x3c, 0x11223344);
    reply = qtest_qmp(t.qts, "{'execute':'qom-get','arguments':"
        "{'path':'/machine/peripheral/fault',"
        "'property':'x-test-bar-write-count'}}");
    g_assert_true(qdict_haskey(reply, "return"));
    g_assert_cmphex(qdict_get_int(reply, "return"), ==, 2);
    qobject_unref(reply);
    reply = qtest_qmp(t.qts, "{'execute':'qom-get','arguments':"
        "{'path':'/machine/peripheral/fault',"
        "'property':'x-test-fault-result'}}");
    g_assert_cmphex(qdict_get_int(reply, "return"), ==, MEMTX_ERROR);
    qobject_unref(reply);
    qtest_quit(t.qts);
    g_assert_cmpint(unlink(storage), ==, 0);
}

static void test_mem64_narrow_card(void)
{
    const Placement p = { .head = 2, .device = 1, .destination = 1 };
    g_autofree char *storage = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = &p };
    uint64_t config, mem32, mem64;
    int fd;

    fd = g_file_open_tmp("sn1-option-narrow-pio-width-XXXXXX", &storage,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    t.qts = qtest_initf(
        "-M origin300,topology=" TOPOLOGY
        ",expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s -nodefaults -display none "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device sgi-bridge-dma-test,bus=%s,addr=%u",
        prom_path, storage, bus_names[p.head], p.device);
    setup_routes(&t);
    config = t.head + 0x20000 + p.device * 0x1000;
    cpu_writel(t.qts, t.pio_cpu, config + PCI_BASE_ADDRESS_0,
               ADAPTER_MMIO);
    cpu_writel(t.qts, t.pio_cpu, config + PCI_COMMAND,
               PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    cpu_writel(t.qts, t.pio_cpu, t.head + 0x204 + p.device * 8,
               DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20));
    cpu_writeq(t.qts, t.pio_cpu, SPACE_IO + NODE(7) + 0x01c00160,
               0x0e04);
    mem64 = SPACE_IO + NODE(7) + 0x20000000ULL + ADAPTER_MMIO;
    cpu_writeq(t.qts, t.pio_cpu, SPACE_IO + NODE(7) + 0x01c00160,
               0x0e02);
    mem32 = SPACE_IO + NODE(7) + 0x20000000ULL + ADAPTER_MMIO;
    g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, mem32 + 0x38), ==, 0);
    cpu_writel(t.qts, t.pio_cpu, mem32 + 0x24, 0xa1b2c3d4);
    cpu_writeq(t.qts, t.pio_cpu, SPACE_IO + NODE(7) + 0x01c00160,
               0x0e04);
    for (unsigned bit22 = 0; bit22 < 2; bit22++) {
        cpu_writeq(t.qts, t.pio_cpu,
                   SPACE_IO + NODE(7) + 0x01c00160, 0x0e02);
        cpu_writel(t.qts, t.pio_cpu, mem32 + 0x24, 0xa1b2c3d4);
        cpu_writeq(t.qts, t.pio_cpu,
                   SPACE_IO + NODE(7) + 0x01c00160, 0x0e04);
        cpu_writel(t.qts, t.pio_cpu, t.head + 0x20c,
                   DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20) |
                   (bit22 << 22));
        g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, mem64 + 0x20), ==,
                        0x12345678);
        g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, mem64 + 0x24), ==,
                        0xa1b2c3d4);
        cpu_writel(t.qts, t.pio_cpu, mem64 + 0x24,
                   0x10213243 + bit22);
        cpu_writeq(t.qts, t.pio_cpu,
                   SPACE_IO + NODE(7) + 0x01c00160, 0x0e02);
        g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, mem32 + 0x24), ==,
                        0x10213243 + bit22);
        cpu_writeq(t.qts, t.pio_cpu,
                   SPACE_IO + NODE(7) + 0x01c00160, 0x0e04);
    }
    qtest_quit(t.qts);
    g_assert_cmpint(unlink(storage), ==, 0);
}

static void test_dma_read_sram_window_words(void)
{
    const Placement p = { .head = 2, .device = 1, .destination = 1 };
    const uint8_t source[8] = {
        0xaa, 0xaa, 0xaa, 0xaa, 0x55, 0x55, 0x55, 0x55,
    };
    g_autofree char *storage = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = &p };
    uint64_t address;
    uint8_t seen[sizeof(source)];
    int fd;

    fd = g_file_open_tmp("sn1-option-tigon-window-XXXXXX", &storage, &error);
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
    reg_write(&t, TI_WINBASE, 0x1000);
    memory_write(&t, p.destination, 0x2a1000, source, sizeof(source));
    address = dma_address(&t, 0x2a1000);
    reg_write(&t, TI_DMA_READ_HOST_HI, address >> 32);
    reg_write(&t, TI_DMA_READ_HOST_LO, address);
    reg_write(&t, TI_DMA_READ_LOCAL, 0x1000);
    reg_write(&t, TI_DMA_READ_LENGTH, sizeof(source));
    reg_write(&t, TI_DMA_READ_STATE, TI_DMA_ACTIVE | TI_DMA_NO_SWAP);
    g_assert_cmphex(reg_read(&t, TI_DMA_READ_STATE) & TI_DMA_ACTIVE, ==, 0);
    g_assert_cmphex(reg_read(&t, TI_DMA_READ_LENGTH), ==, 0);
    g_assert_cmphex(reg_read(&t, TI_DMA_READ_LOCAL), ==,
                    0x1000 + sizeof(source));
    for (unsigned i = 0; i < sizeof(seen); i++) {
        g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                                        t.window + 0x800 + (i ^ 3),
                                        &seen[i], 1));
    }
    g_assert_cmpmem(seen, sizeof(seen), source, sizeof(source));
    g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, t.window + 0x800), ==,
                    0xaaaaaaaa);
    g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, t.window + 0x804), ==,
                    0x55555555);
    g_assert_cmphex(cpu_readq(t.qts, t.pio_cpu, t.window + 0x800), ==,
                    UINT64_C(0xaaaaaaaa55555555));
    qtest_quit(t.qts);
    g_assert_cmpint(unlink(storage), ==, 0);
}

static void test_direct_then_page_dma_readback(void)
{
    const Placement p = { .head = 2, .device = 1, .destination = 0 };
    static const uint8_t card_bytes[2][8] = {
        { 0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87 },
        { 0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f },
    };
    static const uint8_t host_bytes[2][8] = {
        { 0x54, 0x65, 0x76, 0x87, 0x10, 0x21, 0x32, 0x43 },
        { 0xdc, 0xed, 0xfe, 0x0f, 0x98, 0xa9, 0xba, 0xcb },
    };
    static const uint32_t cpu_words[2][2] = {
        { 0x54657687, 0x10213243 },
        { 0xdcedfe0f, 0x98a9bacb },
    };
    static const uint32_t bar_words[2][2] = {
        { 0x43322110, 0x87766554 },
        { 0xcbbaa998, 0x0ffeeddc },
    };
    const uint64_t target = 0x4d4000;
    const uint32_t iova[] = { 0x804d4000, 0x60000000 };
    g_autofree char *storage = NULL;
    g_autofree char *trace_path = NULL;
    g_autofree char *trace = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = &p };
    uint8_t seen[8];
    uint32_t control;
    int fd;

    fd = g_file_open_tmp("sn1-option-tigon-readback-XXXXXX", &storage,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    fd = g_file_open_tmp("sn1-page-read-trace-XXXXXX", &trace_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    t.qts = qtest_initf(
        "-M origin300,topology=" TOPOLOGY
        ",expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s -nodefaults -display none "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device tigon,bus=%s,addr=%u,mac=" MAC " "
        "-trace enable=sgi_xbridge_dma_packet,file=%s",
        prom_path, storage, bus_names[p.head], p.device, trace_path);
    setup_routes(&t);
    setup_card(&t);
    g_assert_cmphex(reg_read(&t, TI_PCI_STATE) & TI_PCI_32BIT_BUS, ==, 0);
    reg_write(&t, TI_PCI_STATE, TI_PCI_NO_WORD_SWAP);
    reg_write(&t, TI_WINBASE, 0x1000);
    control = cpu_readl(t.qts, t.pio_cpu, t.head + 0x204 + p.device * 8);
    cpu_writel(t.qts, t.pio_cpu, t.head + 0x204 + p.device * 8,
               control | XBRIDGE_DIRECT_SWAP);
    cpu_writeq(t.qts, t.pio_cpu, t.head + 0x10000,
               target | (0xa << 8) | 1);

    memory_write(&t, 0, target, (uint8_t[8]) { 0 }, sizeof(seen));

    for (unsigned path = 0; path < G_N_ELEMENTS(iova); path++) {
        for (unsigned i = 0; i < sizeof(seen); i++) {
            g_assert_true(qtest_memwrite_cpu(t.qts, t.pio_cpu,
                t.window + 0x800 + (i ^ 3), &card_bytes[path][i], 1));
            g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                t.window + 0x800 + (i ^ 3), &seen[i], 1));
        }
        g_assert_cmpmem(seen, sizeof(seen), card_bytes[path], sizeof(seen));
        reg_write(&t, TI_DMA_WRITE_HOST_HI, 0);
        reg_write(&t, TI_DMA_WRITE_HOST_LO, iova[path]);
        reg_write(&t, TI_DMA_WRITE_LOCAL, 0x1000);
        reg_write(&t, TI_DMA_WRITE_STATE, TI_DMA_NO_SWAP);
        reg_write(&t, TI_DMA_WRITE_LENGTH, sizeof(seen));
        reg_write(&t, TI_DMA_WRITE_STATE, TI_DMA_ACTIVE | TI_DMA_NO_SWAP);
        g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_STATE) & TI_DMA_ACTIVE,
                        ==, 0);
        g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_LENGTH), ==, 0);
        memory_read(&t, 0, target, seen, sizeof(seen));
        g_assert_cmpmem(seen, sizeof(seen), host_bytes[path], sizeof(seen));
        g_assert_cmphex(cpu_readl(t.qts, 0, target), ==,
                        cpu_words[path][0]);
        g_assert_cmphex(cpu_readl(t.qts, 0, target + 4), ==,
                        cpu_words[path][1]);

        for (unsigned i = 0; i < sizeof(seen); i++) {
            uint8_t zero = 0;

            g_assert_true(qtest_memwrite_cpu(t.qts, t.pio_cpu,
                t.window + 0x800 + (i ^ 3), &zero, 1));
        }
        reg_write(&t, TI_DMA_READ_HOST_HI, 0);
        reg_write(&t, TI_DMA_READ_HOST_LO, iova[path]);
        reg_write(&t, TI_DMA_READ_LOCAL, 0x1000);
        reg_write(&t, TI_DMA_READ_LENGTH, sizeof(seen));
        reg_write(&t, TI_DMA_READ_STATE, TI_DMA_ACTIVE | TI_DMA_NO_SWAP);
        g_assert_cmphex(reg_read(&t, TI_DMA_READ_STATE) & TI_DMA_ACTIVE,
                        ==, 0);
        g_assert_cmphex(reg_read(&t, TI_DMA_READ_LENGTH), ==, 0);
        for (unsigned i = 0; i < sizeof(seen); i++) {
            g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                t.window + 0x800 + (i ^ 3), &seen[i], 1));
        }
        g_assert_cmpmem(seen, sizeof(seen), card_bytes[path], sizeof(seen));
        g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, t.window + 0x800), ==,
                        bar_words[path][0]);
        g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, t.window + 0x804), ==,
                        bar_words[path][1]);
    }
    qtest_quit(t.qts);
    g_assert_true(g_file_get_contents(trace_path, &trace, NULL, &error));
    g_assert_no_error(error);
    g_auto(GStrv) lines = g_strsplit(trace, "\n", -1);
    unsigned reads = 0;

    for (unsigned line = 0; lines[line]; line++) {
        if (strstr(lines[line], "sgi_xbridge_dma_packet") &&
            strstr(lines[line], "write 0")) {
            g_assert_nonnull(strstr(lines[line], "size 8"));
            reads++;
        }
    }
    g_assert_cmpuint(reads, ==, 2);
    g_assert_cmpint(unlink(trace_path), ==, 0);
    g_assert_cmpint(unlink(storage), ==, 0);
}

static void test_direct_dma_write(void)
{
    const Placement p = { .head = 2, .device = 1, .destination = 1,
                          .swap_clear = true };
    g_autofree char *storage = NULL;
    g_autofree char *dest_storage = NULL;
    g_autofree char *trace_path = NULL;
    g_autofree char *trace = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = &p };
    Tigon dest = { .placement = &p };
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    uint8_t source[16], guard[24], seen[24], migrated[24];
    uint8_t chunk_source[272], chunk_raw[272], chunk_expected[272];
    uint8_t chunk_guard[288], chunk_seen[288];
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
    uint32_t device_control;
    uint32_t direct_control;
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
    fd = g_file_open_tmp("sn1-option-tigon-page-chunks-XXXXXX",
                         &trace_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    t.qts = qtest_initf(
        "-M origin300,topology=" TOPOLOGY
        ",expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s -nodefaults -display none "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device tigon,bus=%s,addr=%u,mac=" MAC " "
        "-trace enable=sgi_xbridge_dma_packet,file=%s",
        prom_path, storage, bus_names[p.head], p.device, trace_path);
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

    /* The second DMA chunk must preserve the same eight-byte placement. */
    reg_write(&t, TI_WINBASE, 0x3000);
    for (unsigned i = 0; i < sizeof(chunk_source); i++) {
        chunk_source[i] = i * 29 + (i >> 8) * 71;
    }
    g_assert_true(qtest_memwrite_cpu(t.qts, t.pio_cpu,
                                     t.window + 0x800, chunk_source,
                                     sizeof(chunk_source)));
    memset(chunk_guard, 0x96, sizeof(chunk_guard));
    device_control = cpu_readl(t.qts, t.pio_cpu,
                               t.head + 0x204 + p.device * 8);
    direct_control = cpu_readl(t.qts, t.pio_cpu, t.head + 0x84);
    cpu_writel(t.qts, t.pio_cpu, t.head + 0x84,
               (0xa << 20) | (memory_address(p.destination, 0) >> 31));
    g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, t.head + 0x84), ==,
                    (0xa << 20) | (memory_address(p.destination, 0) >> 31));
    for (unsigned direct_swap = 0; direct_swap < 2; direct_swap++) {
        cpu_writel(t.qts, t.pio_cpu, t.head + 0x204 + p.device * 8,
                   (device_control & ~XBRIDGE_DIRECT_SWAP) |
                   (direct_swap ? XBRIDGE_DIRECT_SWAP : 0));
        g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu,
                                  t.head + 0x204 + p.device * 8) &
                        XBRIDGE_DIRECT_SWAP, ==,
                        direct_swap ? XBRIDGE_DIRECT_SWAP : 0);
        for (unsigned i = 0; i < sizeof(chunk_raw); i++) {
            g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                          t.window + 0x800 + (i ^ 3), &chunk_raw[i], 1));
            g_assert_cmphex(chunk_raw[i], ==, chunk_source[i ^ 3]);
            chunk_expected[i] = chunk_source[direct_swap ? i ^ 7 : i];
        }
        g_assert_cmphex(reg_read(&t, TI_PCI_STATE) &
                        (TI_PCI_NO_WORD_SWAP | TI_PCI_32BIT_BUS), ==,
                        TI_PCI_NO_WORD_SWAP);
        memory_write(&t, p.destination, 0x2a0400 - 8,
                     chunk_guard, sizeof(chunk_guard));
        address = 0x80000000U | 0x2a0400;
        reg_write(&t, TI_DMA_WRITE_HOST_HI, address >> 32);
        reg_write(&t, TI_DMA_WRITE_HOST_LO, address);
        reg_write(&t, TI_DMA_WRITE_LOCAL, 0x3000);
        reg_write(&t, TI_DMA_WRITE_STATE, TI_DMA_NO_SWAP);
        g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_STATE) & TI_DMA_NO_SWAP,
                        ==, TI_DMA_NO_SWAP);
        reg_write(&t, TI_DMA_WRITE_LENGTH, sizeof(chunk_source));
        memory_read(&t, p.destination, 0x2a0400 - 8,
                    chunk_seen, sizeof(chunk_seen));
        g_assert_cmpmem(chunk_seen, sizeof(chunk_seen), chunk_guard,
                        sizeof(chunk_guard));
        reg_write(&t, TI_DMA_WRITE_STATE, TI_DMA_ACTIVE | TI_DMA_NO_SWAP);
        g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_STATE) &
                        (TI_DMA_ACTIVE | TI_DMA_NO_SWAP), ==, TI_DMA_NO_SWAP);
        g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_LENGTH), ==, 0);
        g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_HOST_HI), ==,
                        address >> 32);
        g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_HOST_LO), ==,
                        (uint32_t)address + sizeof(chunk_source));
        g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_LOCAL), ==,
                        0x3000 + sizeof(chunk_source));
        memory_read(&t, p.destination, 0x2a0400 - 8,
                    chunk_seen, sizeof(chunk_seen));
        g_assert_cmpmem(chunk_seen, 8, chunk_guard, 8);
        g_assert_cmpmem(chunk_seen + 8, sizeof(chunk_source),
                        chunk_expected, sizeof(chunk_expected));
        g_assert_cmpmem(chunk_seen + 8 + sizeof(chunk_source), 8,
                        chunk_guard + 8 + sizeof(chunk_source), 8);
    }
    cpu_writel(t.qts, t.pio_cpu, t.head + 0x204 + p.device * 8,
               device_control);
    cpu_writel(t.qts, t.pio_cpu, t.head + 0x84, direct_control);

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

    memory_write(&t, p.destination, 0x2a0180, source, sizeof(source));
    reg_write(&t, TI_WINBASE, 0x2200);
    g_assert_true(qtest_memwrite_cpu(t.qts, t.pio_cpu,
                                     t.window + 0x800, guard,
                                     sizeof(guard)));
    cpu_writeq(t.qts, t.pio_cpu, t.head + 0x10000,
               memory_address(p.destination, 0x2a0000) | (0xa << 8) | 1);
    address = 0x60000180;
    reg_write(&t, TI_DMA_READ_HOST_HI, 0);
    reg_write(&t, TI_DMA_READ_HOST_LO, address);
    reg_write(&t, TI_DMA_READ_LOCAL, 0x2204);
    reg_write(&t, TI_DMA_READ_LENGTH, sizeof(source));
    reg_write(&t, TI_DMA_READ_STATE, TI_DMA_ACTIVE | TI_DMA_NO_SWAP);
    g_assert_cmphex(reg_read(&t, TI_DMA_READ_STATE) & TI_DMA_ACTIVE, ==, 0);
    g_assert_cmphex(reg_read(&t, TI_DMA_READ_LENGTH), ==, 0);
    g_assert_cmphex(reg_read(&t, TI_DMA_READ_HOST_LO), ==,
                    (uint32_t)address + sizeof(source));
    g_assert_cmphex(reg_read(&t, TI_DMA_READ_LOCAL), ==,
                    0x2204 + sizeof(source));
    for (unsigned i = 0; i < sizeof(seen); i++) {
        g_assert_true(qtest_memread_cpu(t.qts, t.pio_cpu,
                                        t.window + 0x800 + (i ^ 3),
                                        &seen[i], 1));
    }
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
    g_assert_true(g_file_get_contents(trace_path, &trace, NULL, &error));
    g_assert_no_error(error);
    g_auto(GStrv) lines = g_strsplit(trace, "\n", -1);
    unsigned page_reads = 0;

    for (unsigned line = 0; lines[line]; line++) {
        if (strstr(lines[line], "sgi_xbridge_dma_packet") &&
            strstr(lines[line], "write 0") &&
            (strstr(lines[line], "2a0180") ||
             strstr(lines[line], "2a0188"))) {
            g_assert_nonnull(strstr(lines[line], "size 8"));
            page_reads++;
        }
    }
    g_assert_cmpuint(page_reads, ==, 2);
    g_assert_cmpint(unlink(trace_path), ==, 0);
    g_rmdir(directory);
    g_assert_cmpint(unlink(storage), ==, 0);
    g_assert_cmpint(unlink(dest_storage), ==, 0);
}

static void test_wide_64bit_dma_write(void)
{
    const Placement p = { .head = 2, .device = 1, .destination = 0 };
    static const uint8_t expected[2][16] = {
        { 0x46, 0x3f, 0x38, 0x31, 0x62, 0x5b, 0x54, 0x4d,
          0x7e, 0x77, 0x70, 0x69, 0x9a, 0x93, 0x8c, 0x85 },
        { 0x62, 0x5b, 0x54, 0x4d, 0x46, 0x3f, 0x38, 0x31,
          0x9a, 0x93, 0x8c, 0x85, 0x7e, 0x77, 0x70, 0x69 },
    };
    g_autofree char *storage = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = &p };
    uint8_t source[16], seen[16];
    const uint64_t iova = UINT64_C(0xa9800000004d4000);
    uint32_t device;
    int fd;

    fd = g_file_open_tmp("sn1-option-tigon-wide64-XXXXXX", &storage, &error);
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
    device = cpu_readl(t.qts, t.pio_cpu, t.head + 0x20c);
    reg_write(&t, TI_PCI_STATE, TI_PCI_NO_WORD_SWAP);
    reg_write(&t, TI_WINBASE, 0x2000);
    for (unsigned i = 0; i < sizeof(source); i++) {
        source[i] = 0x31 + i * 7;
    }
    g_assert_true(qtest_memwrite_cpu(t.qts, t.pio_cpu, t.window + 0x800,
                                     source, sizeof(source)));
    for (unsigned bit19 = 0; bit19 < 2; bit19++) {
        cpu_writel(t.qts, t.pio_cpu, t.head + 0x20c,
                   (device & ~XBRIDGE_DIRECT_SWAP) |
                   (bit19 ? XBRIDGE_DIRECT_SWAP : 0));
        g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, t.head + 0x20c) &
                        XBRIDGE_DIRECT_SWAP, ==,
                        bit19 ? XBRIDGE_DIRECT_SWAP : 0);
        memory_write(&t, 0, 0x4d4000, (uint8_t[16]) { 0 }, sizeof(seen));
        reg_write(&t, TI_DMA_WRITE_HOST_HI, iova >> 32);
        reg_write(&t, TI_DMA_WRITE_HOST_LO, (uint32_t)iova);
        reg_write(&t, TI_DMA_WRITE_LOCAL, 0x2000);
        reg_write(&t, TI_DMA_WRITE_STATE, TI_DMA_NO_SWAP);
        reg_write(&t, TI_DMA_WRITE_LENGTH, sizeof(source));
        reg_write(&t, TI_DMA_WRITE_STATE, TI_DMA_ACTIVE | TI_DMA_NO_SWAP);
        g_assert_cmphex(reg_read(&t, TI_DMA_WRITE_LENGTH), ==, 0);
        memory_read(&t, 0, 0x4d4000, seen, sizeof(seen));
        g_assert_cmpmem(seen, sizeof(seen), expected[bit19],
                        sizeof(expected[bit19]));
    }
    qtest_quit(t.qts);
    g_assert_cmpint(unlink(storage), ==, 0);
}

static void test_generic_wide_64bit_dma_write(void)
{
    static const struct {
        unsigned head;
        unsigned slot;
        bool bit55;
        bool bit19;
        bool direct32;
        uint8_t expected[16];
        uint64_t bad_iova;
        bool ecc_overlay;
    } cases[] = {
        { 2, 2, true, true, false,
          { 0x14, 0x15, 0x16, 0x17, 0x10, 0x11, 0x12, 0x13,
            0x1c, 0x1d, 0x1e, 0x1f, 0x18, 0x19, 0x1a, 0x1b }, 0 },
        { 2, 2, false, true, false,
          { 0x13, 0x12, 0x11, 0x10, 0x17, 0x16, 0x15, 0x14,
            0x1b, 0x1a, 0x19, 0x18, 0x1f, 0x1e, 0x1d, 0x1c }, 0 },
        { 2, 2, true, false, false,
          { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
            0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f }, 0 },
        { 2, 3, true, true, false,
          { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
            0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f }, 0 },
        { 3, 2, true, true, false,
          { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
            0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f }, 0 },
        { 2, 2, false, true, true,
          { 0x14, 0x15, 0x16, 0x17, 0x10, 0x11, 0x12, 0x13,
            0x1c, 0x1d, 0x1e, 0x1f, 0x18, 0x19, 0x1a, 0x1b }, 0 },
        { .head = 2, .slot = 2, .bit55 = true, .bit19 = true,
          .bad_iova = UINT64_C(0xa9800001fffffffc) },
        { .head = 2, .slot = 2, .bit55 = true, .bit19 = true,
          .bad_iova = UINT64_C(0xa980000200000000) },
        { .head = 2, .slot = 2, .bit55 = true, .bit19 = true,
          .bad_iova = UINT64_C(0xa980000010000000) },
        { .head = 2, .slot = 2, .bit55 = true, .bit19 = true,
          .bad_iova = UINT64_C(0xa980800001800000) },
        { .head = 2, .slot = 2, .bit55 = true, .bit19 = true,
          .bad_iova = UINT64_C(0xa98000000ffffffc) },
        { .head = 2, .slot = 2, .bit55 = true, .bit19 = true,
          .ecc_overlay = true,
          .expected = { 0x10, 0x11, 0x12, 0x13,
                        0x14, 0x15, 0x16, 0x17,
                        0x1c, 0x1d, 0x1e, 0x1f,
                        0x18, 0x19, 0x1a, 0x1b } },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        const Placement p = { .head = cases[i].head,
                              .device = cases[i].slot, .destination = 0 };
        g_autofree char *storage = NULL;
        g_autofree char *trace_path = NULL;
        g_autofree char *trace_arg = NULL;
        g_autofree char *trace = NULL;
        g_autoptr(GError) error = NULL;
        Tigon t = { .placement = &p };
        uint8_t seen[16];
        uint32_t direct_swap = 0;
        uint64_t iova = cases[i].direct32 ? UINT64_C(0x804d4000)
                        : cases[i].bit55 ? UINT64_C(0xa9800000004d4000)
                                         : UINT64_C(0xa9000000004d4000);
        int fd = g_file_open_tmp("sn1-generic-wide64-XXXXXX", &storage,
                                 &error);

        g_assert_no_error(error);
        g_assert_cmpint(fd, >=, 0);
        g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
        g_assert_cmpint(close(fd), ==, 0);
        if (i == 0) {
            fd = g_file_open_tmp("sn1-generic-wide64-trace-XXXXXX",
                                 &trace_path, &error);
            g_assert_no_error(error);
            g_assert_cmpint(fd, >=, 0);
            g_assert_cmpint(close(fd), ==, 0);
            trace_arg = g_strdup_printf(
                "-trace enable=sgi_xbridge_dma_packet,file=%s", trace_path);
        }
        t.qts = qtest_initf(
            "-M origin300,topology=" TOPOLOGY
            ",expansion-storage.0=expansion-enclosure=cfg "
            "-cpu R14000 -smp 4 -m 1G -bios %s -nodefaults -display none "
            "-drive if=none,id=cfg,file=%s,format=raw "
            "-device sgi-bridge-dma-test,bus=%s,addr=%u %s",
            prom_path, storage, bus_names[p.head], p.device,
            trace_arg ? trace_arg : "");
        setup_routes(&t);
        if (cases[i].bad_iova) {
            iova = cases[i].bad_iova;
        }
        cpu_writel(t.qts, t.pio_cpu, t.head + 0x20010 + p.device * 0x1000,
                   (p.device + 1) << 24);
        cpu_writel(t.qts, t.pio_cpu, t.head + 0x20004 + p.device * 0x1000,
                   6);
        if (cases[i].bit19) {
            direct_swap = XBRIDGE_DIRECT_SWAP;
        }
        cpu_writel(t.qts, t.pio_cpu, t.head + 0x204 + p.device * 8,
                   0x1000 | ((p.device + 1) << 4) | direct_swap);
        cpu_writel(t.qts, t.pio_cpu, t.head + 0x84, 0xa << 20);
        memory_write(&t, 0, 0x4d4000, (uint8_t[16]) { 0 }, sizeof(seen));
        if (cases[i].ecc_overlay) {
            /* The ECC byte carrier reverses its low three address bits. */
            qtest_writeb(t.qts, HUB_ECC + (0x4d4000 / 8 ^ 7), 0xff);
        }
        if (cases[i].bad_iova == UINT64_C(0xa98000000ffffffc)) {
            memory_write(&t, 0, 0x0ffffff8,
                         (uint8_t[8]) { 0xee, 0xee, 0xee, 0xee,
                                        0xee, 0xee, 0xee, 0xee }, 8);
        }
        cpu_writel(t.qts, t.pio_cpu, t.window, iova);
        cpu_writel(t.qts, t.pio_cpu, t.window + 0x18, iova >> 32);
        cpu_writel(t.qts, t.pio_cpu, t.window + 4, sizeof(seen));
        cpu_writel(t.qts, t.pio_cpu, t.window + 8, 0x10);
        cpu_writel(t.qts, t.pio_cpu, t.window + 0xc, 1);
        if (cases[i].bad_iova) {
            g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, t.window + 0x28), !=,
                            MEMTX_OK);
        } else {
            g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, t.window + 0x28), ==,
                            MEMTX_OK);
        }
        memory_read(&t, 0, 0x4d4000, seen, sizeof(seen));
        g_assert_cmpmem(seen, sizeof(seen), cases[i].expected,
                        sizeof(cases[i].expected));
        if (cases[i].bad_iova == UINT64_C(0xa98000000ffffffc)) {
            static const uint8_t expected_partial[] = {
                0x10, 0x11, 0x12, 0x13,
            };
            uint8_t partial[4];

            memory_read(&t, 0, 0x0ffffffc, partial, sizeof(partial));
            g_assert_cmpmem(partial, sizeof(partial),
                            expected_partial, sizeof(expected_partial));
        }
        if (i == 0) {
            const uint64_t copy = UINT64_C(0xa9000000004d5000);

            cpu_writel(t.qts, t.pio_cpu, t.window + 0x14, (uint32_t)iova);
            cpu_writel(t.qts, t.pio_cpu, t.window + 0x1c, iova >> 32);
            cpu_writel(t.qts, t.pio_cpu, t.window, (uint32_t)copy);
            cpu_writel(t.qts, t.pio_cpu, t.window + 0x18, copy >> 32);
            cpu_writel(t.qts, t.pio_cpu, t.window + 4, 8);
            cpu_writel(t.qts, t.pio_cpu, t.window + 0xc, 2);
            g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, t.window + 0x28), ==,
                            MEMTX_OK);
            g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, t.window + 0x2c), ==,
                            0x17161514);
            g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, t.window + 0x30), ==,
                            0x13121110);
            for (unsigned length = 8; length <= 256; length *= 32) {
                uint8_t bytes[256];
                uint64_t target = length == 8 ? 0x4d6000 : 0x4d7000;

                memset(bytes, 0, sizeof(bytes));
                memory_write(&t, 0, target, bytes, length);
                cpu_writel(t.qts, t.pio_cpu, t.window, target);
                cpu_writel(t.qts, t.pio_cpu, t.window + 0x18,
                           iova >> 32);
                cpu_writel(t.qts, t.pio_cpu, t.window + 4, length);
                cpu_writel(t.qts, t.pio_cpu, t.window + 8, 0x40);
                cpu_writel(t.qts, t.pio_cpu, t.window + 0xc, 1);
                g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu,
                                          t.window + 0x28), ==, MEMTX_OK);
                memory_read(&t, 0, target, bytes, length);
                for (unsigned j = 0; j < length; j++) {
                    g_assert_cmphex(bytes[j], ==,
                                    (uint8_t)(0x40 + ((j & ~7U) |
                                            ((j & 7U) ^ 4U))));
                }
            }
        }
        qtest_quit(t.qts);
        if (trace_path) {
            unsigned writes = 0, reads = 0;
            char **lines;

            g_assert_true(g_file_get_contents(trace_path, &trace, NULL,
                                              &error));
            g_assert_no_error(error);
            lines = g_strsplit(trace, "\n", -1);
            for (unsigned line = 0; lines[line]; line++) {
                if (!strstr(lines[line], "sgi_xbridge_dma_packet")) {
                    continue;
                }
                g_assert_nonnull(strstr(lines[line], "size 8"));
                g_assert_nonnull(strstr(lines[line], "result 0"));
                if (strstr(lines[line], "write 1")) {
                    writes++;
                } else {
                    g_assert_nonnull(strstr(lines[line], "write 0"));
                    reads++;
                }
            }
            g_strfreev(lines);
            g_assert_cmpuint(writes, ==, 36);
            g_assert_cmpuint(reads, ==, 1);
            g_assert_cmpint(unlink(trace_path), ==, 0);
        }
        g_assert_cmpint(unlink(storage), ==, 0);
    }
}

static void test_traffic(const void *opaque)
{
    const Placement *p = opaque;
    g_autofree char *storage = NULL;
    g_autofree char *trace_path = NULL;
    g_autofree char *trace_arg = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = p };
    uint8_t tx[60], rx[64], received[64], sentinel[64], seen[64];
    uint8_t guarded[80];
    const uint8_t mac[] = { 0x52, 0x54, 0, 0x12, 0x34, 0x56 };
    const uint8_t tx_before[8] = { 0xd1, 0xc2, 0xb3, 0xa4,
                                   0x95, 0x86, 0x77, 0x68 };
    const uint8_t tx_after[8] = { 0x19, 0x2a, 0x3b, 0x4c,
                                  0x5d, 0x6e, 0x7f, 0x80 };
    const uint32_t checked[] = {
        EVENT_RING, EVENT_PROD, TX_CONS, RETURN_RING, RETURN_PROD, RX_BUF,
    };
    unsigned tx_len = p->payload_length ? p->payload_length :
                      p->swap_clear ? 42 : sizeof(tx);
    unsigned rx_len = p->payload_length ? p->payload_length :
                      p->swap_clear ? 42 : sizeof(rx);
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
    if (p->trace_dma) {
        fd = g_file_open_tmp("sn1-option-tigon-trace-XXXXXX", &trace_path,
                             &error);
        g_assert_no_error(error);
        g_assert_cmpint(fd, >=, 0);
        g_assert_cmpint(close(fd), ==, 0);
        trace_arg = g_strdup_printf("-trace enable=tigon_dma,file=%s",
                                    trace_path);
    }
    t.qts = qtest_initf(
        "-M origin300,topology=" TOPOLOGY
        ",expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s -nodefaults -display none "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-netdev socket,id=n0,fd=%d -device tigon,bus=%s,addr=%u,"
        "netdev=n0,mac=" MAC " %s",
        prom_path, storage, sv[1], bus_names[p->head], p->device,
        trace_arg ? trace_arg : "");
    close(sv[1]);
    socket_timeout(sv[0]);
    setup_routes(&t);
    setup_card(&t);
    setup_rings(&t, gib);
    if (p->swap_clear) {
        g_assert_cmphex(reg_read(&t, TI_PCI_STATE) & TI_PCI_32BIT_BUS,
                        ==, 0);
        reg_write(&t, TI_PCI_STATE, TI_PCI_NO_WORD_SWAP);
    }
    memory_word(&t, EVENT_RING - 4, 0x5a6b7c8d);
    memory_word(&t, EVENT_RING + 8, 0x1728394a);
    memory_word(&t, EVENT_PROD - 4, 0x4a392817);
    memory_word(&t, EVENT_PROD + 4, 0x8d7c6b5a);
    memset(sentinel, 0x96, sizeof(sentinel));
    for (unsigned i = 0; i < G_N_ELEMENTS(checked); i++) {
        memory_write(&t, other, checked[i], sentinel, sizeof(sentinel));
    }

    address = dma_address(&t, gib);
    if (p->gib_offset) {
        g_assert_cmphex(address, ==, UINT64_C(0xa100000001a00000));
        g_assert_cmphex(read_memory_word(&t, gib + 0x400), ==,
                        UINT64_C(0xa1000000));
        g_assert_cmphex(read_memory_word(&t, gib + 0x404), ==,
                        EVENT_RING);
    }
    reg_write(&t, TI_GCR_GENINFO_HI, address >> 32);
    reg_write(&t, TI_GCR_GENINFO_LO, address);
    reg_write(&t, TI_CPU_STATE, 0);
    memory_read(&t, p->destination, EVENT_RING, seen, 8);
    g_assert_cmphex(ldl_be_p(seen), ==, 0x01000000);
    g_assert_cmphex(ldl_be_p(seen + 4), ==, 0);
    g_assert_cmphex(read_memory_word(&t, EVENT_RING - 4), ==, 0x5a6b7c8d);
    g_assert_cmphex(read_memory_word(&t, EVENT_RING + 8), ==, 0x1728394a);
    g_assert_cmphex(read_memory_word(&t, EVENT_PROD), ==, 1);
    g_assert_cmphex(read_memory_word(&t, EVENT_PROD - 4), ==, 0x4a392817);
    g_assert_cmphex(read_memory_word(&t, EVENT_PROD + 4), ==, 0x8d7c6b5a);
    expect_interrupt(&t, true);
    reg_write(&t, TI_GCR_EVENTCONS_IDX, 1);
    acknowledge(&t);
    if (p->data_byteswap || p->pci_bit20 || p->clear_desc_word_swap ||
        p->set_desc_byte_swap) {
        uint32_t opmode = TI_OPMODE_SWAP_CLEAR;

        if (p->data_byteswap) {
            opmode |= 0x10;
        }
        if (p->clear_desc_word_swap) {
            opmode &= ~4U;
        }
        if (p->set_desc_byte_swap) {
            opmode |= 2;
        }
        reg_write(&t, TI_GCR_OPMODE, opmode);
        if (p->pci_bit20) {
            reg_write(&t, TI_PCI_STATE,
                      TI_PCI_NO_WORD_SWAP | TI_PCI_32BIT_BUS);
        }
    }

    if (p->rx_descriptor_only) {
        goto receive;
    }
    for (unsigned i = 0; i < sizeof(tx); i++) {
        tx[i] = p->payload_length ? 0x20 + i :
                i ^ (0x35 + p->head + p->device + p->destination);
    }
    if (!p->payload_length) {
        memset(tx, 0xff, 6);
        memcpy(tx + 6, mac, sizeof(mac));
        tx[12] = 8;
        tx[13] = 6;
    }
    memory_write(&t, p->destination, tx_buf - 8, tx_before,
                 sizeof(tx_before));
    memory_write(&t, p->destination, tx_buf, tx, tx_len);
    memory_write(&t, p->destination, tx_buf + tx_len, tx_after,
                 sizeof(tx_after));
    if (p->pci_bit20 || p->clear_desc_word_swap || p->set_desc_byte_swap) {
        legacy_tx_descriptor(&t, TX_RING, tx_buf,
                             (tx_len << 16) | TI_BDFLAG_END,
                             p->set_desc_byte_swap);
    } else {
        host_address(&t, TX_RING, tx_buf);
        memory_word(&t, TX_RING + 8, (tx_len << 16) | TI_BDFLAG_END);
    }
    reg_write(&t, TI_MB_SENDPROD_IDX, 1);
    g_assert_cmpint(recv(sv[0], &length, sizeof(length), MSG_WAITALL), ==, 4);
    g_assert_cmpuint(ntohl(length), ==, tx_len);
    g_assert_cmpint(recv(sv[0], received, tx_len, MSG_WAITALL), ==, tx_len);
    if (memcmp(received, tx, tx_len)) {
        g_autoptr(GString) expected = g_string_new(NULL);
        g_autoptr(GString) actual = g_string_new(NULL);

        for (unsigned i = 0; i < tx_len; i++) {
            g_string_append_printf(expected, "%02x", tx[i]);
            g_string_append_printf(actual, "%02x", received[i]);
        }
        g_test_message("TX controls: PCI State 0x%08x, OPMODE 0x%08x, "
                       "DMA address 0x%016" PRIx64,
                       reg_read(&t, TI_PCI_STATE),
                       reg_read(&t, TI_GCR_OPMODE),
                       dma_address(&t, tx_buf));
        g_test_message("TX expected %s", expected->str);
        g_test_message("TX observed %s", actual->str);
    }
    if (p->data_byteswap || p->pci_bit20 || p->payload_identity) {
        for (unsigned i = 0; i < tx_len; i++) {
            unsigned source = p->data_byteswap ? 7 - i :
                              p->payload_identity ? i ^ 3 : i ^ 4;
            uint8_t expected = p->payload_identity && tx_len == 9 && i == 8 ?
                               tx_after[2] : tx[source];

            g_assert_cmphex(received[i], ==, expected);
        }
    } else {
        g_assert_cmpmem(received, tx_len, tx, tx_len);
    }
    memory_read(&t, p->destination, tx_buf - 8, seen, sizeof(tx_before));
    g_assert_cmpmem(seen, sizeof(tx_before), tx_before, sizeof(tx_before));
    memory_read(&t, p->destination, tx_buf + tx_len, seen,
                sizeof(tx_after));
    g_assert_cmpmem(seen, sizeof(tx_after), tx_after, sizeof(tx_after));
    if (p->pci_bit20 || p->clear_desc_word_swap || p->set_desc_byte_swap) {
        g_assert_cmphex(read_memory_word(&t, TX_CONS), ==, 0);
    } else {
        g_assert_cmphex(read_memory_word(&t, TX_CONS), ==, 1);
    }
    expect_interrupt(&t, true);
    acknowledge(&t);
    if (p->tx_only) {
        qtest_quit(t.qts);
        close(sv[0]);
        g_assert_cmpint(unlink(storage), ==, 0);
        return;
    }

receive:
    for (unsigned i = 0; i < sizeof(rx); i++) {
        rx[i] = p->payload_length ? 0x80 + i :
                (i * 13) ^ (0xa7 + p->head + p->device + p->destination);
    }
    memcpy(rx, mac, sizeof(mac));
    if (!p->payload_length) {
        rx[12] = 8;
        rx[13] = 6;
    }
    for (unsigned i = 0; i < sizeof(guarded); i++) {
        guarded[i] = 0x80 + i;
    }
    memory_write(&t, p->destination, rx_buf - 8, guarded, sizeof(guarded));
    memory_word(&t, RETURN_RING - 4, 0x395a7b9c);
    memory_word(&t, RETURN_RING + 32, 0x1d3e5f70);
    memory_word(&t, RETURN_PROD - 4, 0x738495a6);
    memory_word(&t, RETURN_PROD + 4, 0xb6c7d8e9);
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
    if (p->rx_descriptor_only) {
        const uint32_t expected[8] = {
            dma_address(&t, rx_buf) >> 32,
            dma_address(&t, rx_buf),
            rx_len,
            (TI_BDTYPE_RECV_BD << 16) | TI_BDFLAG_END | 0x20,
            0, 0, 0, 0,
        };

        for (unsigned i = 0; i < G_N_ELEMENTS(expected); i++) {
            g_assert_cmphex(read_memory_word(&t, RETURN_RING + i * 4), ==,
                            expected[i]);
        }
        g_assert_cmphex(read_memory_word(&t, RETURN_RING - 4), ==,
                        0x395a7b9c);
        g_assert_cmphex(read_memory_word(&t, RETURN_RING + 32), ==,
                        0x1d3e5f70);
        g_assert_cmphex(read_memory_word(&t, RETURN_PROD - 4), ==,
                        0x738495a6);
        g_assert_cmphex(read_memory_word(&t, RETURN_PROD + 4), ==,
                        0xb6c7d8e9);
        qtest_quit(t.qts);
        close(sv[0]);
        g_assert_cmpint(unlink(storage), ==, 0);
        return;
    }
    memory_read(&t, p->destination, rx_buf, received, rx_len);
    if (p->data_byteswap || p->payload_identity) {
        for (unsigned i = 0; i < rx_len; i++) {
            uint8_t expected = p->payload_identity && rx_len == 9 && i == 8 ?
                               guarded[16] :
                               rx[p->payload_identity ? i ^ 3 : 7 - i];

            g_assert_cmphex(received[i], ==,
                            expected);
        }
    } else {
        g_assert_cmpmem(received, rx_len, rx, rx_len);
    }
    memory_read(&t, p->destination, rx_buf - 8, seen, 8);
    g_assert_cmpmem(seen, 8, guarded, 8);
    memory_read(&t, p->destination, rx_buf + rx_len, seen, 8);
    if (p->payload_identity && rx_len == 9) {
        uint8_t expected[8];

        memcpy(expected, guarded + 8 + rx_len, sizeof(expected));
        expected[2] = rx[8];
        g_assert_cmpmem(seen, sizeof(expected), expected, sizeof(expected));
    } else {
        g_assert_cmpmem(seen, 8, guarded + 8 + rx_len, 8);
    }
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
    if (p->trace_dma) {
        g_autofree char *trace = NULL;

        g_assert_true(g_file_get_contents(trace_path, &trace, NULL, &error));
        g_assert_no_error(error);
        g_assert_nonnull(strstr(trace,
            "frame read 0xa100000000290000 1 bytes result 0"));
        g_assert_nonnull(strstr(trace,
            "frame read 0xa100000000290008 8 bytes result 0"));
        g_assert_nonnull(strstr(trace,
            "frame write 0xa100000000280000 1 bytes result 0"));
        g_assert_nonnull(strstr(trace,
            "frame write 0xa100000000280008 8 bytes result 0"));
        g_assert_cmpint(unlink(trace_path), ==, 0);
    }
    close(sv[0]);
    g_assert_cmpint(unlink(storage), ==, 0);
}

static void test_wide_selector(const void *opaque)
{
    const WideSelector *selector = opaque;
    const Placement p = { .head = 0, .device = 1, .destination = 0,
                          .swap_clear = true, .gib_offset = 0x01a00000 };
    g_autofree char *storage = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = &p };
    uint8_t event[8];
    int fd;

    fd = g_file_open_tmp("sn1-option-tigon-selector-XXXXXX", &storage,
                         &error);
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
    setup_rings(&t, p.gib_offset);
    g_assert_cmphex(reg_read(&t, TI_PCI_STATE) & TI_PCI_32BIT_BUS, ==, 0);
    reg_write(&t, TI_PCI_STATE, selector->pci_state);
    reg_write(&t, TI_GCR_OPMODE, selector->opmode);
    g_assert_cmphex(reg_read(&t, TI_PCI_STATE), ==, selector->pci_state);
    g_assert_cmphex(reg_read(&t, TI_GCR_OPMODE), ==, selector->opmode);
    g_assert_cmphex(read_memory_word(&t, p.gib_offset + 0x400), ==,
                    UINT32_C(0xa1000000));
    g_assert_cmphex(read_memory_word(&t, p.gib_offset + 0x404), ==,
                    EVENT_RING);
    memory_word(&t, EVENT_RING - 4, 0x5a6b7c8d);
    memory_word(&t, EVENT_RING + 8, 0x1728394a);
    memory_word(&t, EVENT_PROD - 4, 0x4a392817);
    memory_word(&t, EVENT_PROD + 4, 0x8d7c6b5a);
    reg_write(&t, TI_GCR_GENINFO_HI, dma_address(&t, p.gib_offset) >> 32);
    reg_write(&t, TI_GCR_GENINFO_LO, dma_address(&t, p.gib_offset));
    reg_write(&t, TI_CPU_STATE, 0);
    memory_read(&t, p.destination, EVENT_RING, event, sizeof(event));
    g_assert_cmphex(ldl_be_p(event), ==,
                    selector->expect_event ? 0x01000000 : 0);
    g_assert_cmphex(ldl_be_p(event + 4), ==, 0);
    g_assert_cmphex(read_memory_word(&t, EVENT_PROD), ==,
                    selector->expect_event ? 1 : 0);
    g_assert_cmphex(read_memory_word(&t, EVENT_RING - 4), ==, 0x5a6b7c8d);
    g_assert_cmphex(read_memory_word(&t, EVENT_RING + 8), ==, 0x1728394a);
    g_assert_cmphex(read_memory_word(&t, EVENT_PROD - 4), ==, 0x4a392817);
    g_assert_cmphex(read_memory_word(&t, EVENT_PROD + 4), ==, 0x8d7c6b5a);
    qtest_quit(t.qts);
    g_assert_cmpint(unlink(storage), ==, 0);
}

static void test_wide_faulted_tail(void)
{
    const Placement p = { .head = 0, .device = 1, .destination = 0,
                          .swap_clear = true };
    g_autofree char *storage = NULL;
    g_autoptr(GError) error = NULL;
    Tigon t = { .placement = &p };
    int fd;

    fd = g_file_open_tmp("sn1-option-tigon-tail-XXXXXX", &storage, &error);
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
    setup_rings(&t, GIB);
    reg_write(&t, TI_PCI_STATE, TI_PCI_NO_WORD_SWAP);
    g_assert_cmphex(reg_read(&t, TI_PCI_STATE), ==, TI_PCI_NO_WORD_SWAP);
    host_address(&t, GIB + 0x420, TX_FAULT_DESC);
    {
        uint8_t words[8];

        stl_be_p(words, UINT32_C(0xa1000000));
        stl_be_p(words + 4, TX_BUF);
        qtest_memwrite(t.qts, TX_FAULT_DESC, words, sizeof(words));
        memset(words, 0, sizeof(words));
        qtest_memread(t.qts, TX_FAULT_DESC, words, sizeof(words));
        g_assert_cmphex((uint32_t)ldl_be_p(words), ==,
                        UINT32_C(0xa1000000));
        g_assert_cmphex((uint32_t)ldl_be_p(words + 4), ==, TX_BUF);
    }
    reg_write(&t, TI_GCR_GENINFO_HI, dma_address(&t, GIB) >> 32);
    reg_write(&t, TI_GCR_GENINFO_LO, dma_address(&t, GIB));
    reg_write(&t, TI_CPU_STATE, 0);
    g_assert_cmphex(read_memory_word(&t, EVENT_PROD), ==, 1);
    g_assert_cmphex(read_memory_word(&t, TX_CONS), ==, 0);
    reg_write(&t, TI_MB_SENDPROD_IDX, 1);
    g_assert_cmphex(read_memory_word(&t, TX_CONS), ==, 0);
    g_assert_cmphex(read_memory_word(&t, EVENT_PROD), ==, 1);
    qtest_quit(t.qts);
    g_assert_cmpint(unlink(storage), ==, 0);
}

static void check_mem64_write_lifecycle(Tigon *t, uint64_t mem64,
                                        uint32_t device_control,
                                        uint32_t control)
{
    const uint8_t initial[8] = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
    };
    const uint8_t payload[2][4] = {
        { 0xcb, 0xba, 0xa9, 0x98 },
        { 0x98, 0xa9, 0xba, 0xcb },
    };
    uint8_t seen[sizeof(initial)], expected[sizeof(initial)];

    device_control &= ~(1U << 22);
    control &= ~(1U << 22);
    for (unsigned swap = 0; swap < 2; swap++) {
        for (unsigned wide = 0; wide < 2; wide++) {
            cpu_writel(t->qts, t->pio_cpu, t->head + 0x20c,
                       device_control);
            cpu_writel(t->qts, t->pio_cpu, t->head + 0x24, control);
            for (unsigned i = 0; i < sizeof(initial); i++) {
                g_assert_true(qtest_memwrite_cpu(t->qts, t->pio_cpu,
                    t->window + 0x800 + (i ^ 3), &initial[i], 1));
            }
            cpu_writel(t->qts, t->pio_cpu, t->head + 0x24,
                       control | (swap << 22));
            cpu_writel(t->qts, t->pio_cpu, t->head + 0x20c,
                       device_control | (wide << 22));
            cpu_writel(t->qts, t->pio_cpu, mem64, 0x98a9bacb);
            cpu_writel(t->qts, t->pio_cpu, t->head + 0x20c,
                       device_control);
            cpu_writel(t->qts, t->pio_cpu, t->head + 0x24, control);
            for (unsigned i = 0; i < sizeof(seen); i++) {
                g_assert_true(qtest_memread_cpu(t->qts, t->pio_cpu,
                    t->window + 0x800 + (i ^ 3), &seen[i], 1));
            }
            memcpy(expected, initial, sizeof(expected));
            memcpy(expected + (wide && !swap ? 4 : 0), payload[swap], 4);
            g_assert_cmpmem(seen, sizeof(seen), expected, sizeof(expected));
        }
    }
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
    const uint8_t card_bytes[8] = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
    };
    uint64_t mem64 = SPACE_IO + NODE(7) + 0x20000000ULL +
                     ADAPTER_MMIO + 0x800;
    uint32_t control;
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
    reg_write(&t, TI_WINBASE, 0x70000);
    for (unsigned i = 0; i < sizeof(card_bytes); i++) {
        g_assert_true(qtest_memwrite_cpu(t.qts, t.pio_cpu,
                      t.window + 0x800 + (i ^ 3), &card_bytes[i], 1));
    }
    cpu_writeq(t.qts, t.pio_cpu, SPACE_IO + NODE(7) + 0x01c00160,
               0x0e04);
    control = cpu_readl(t.qts, t.pio_cpu, t.head + 0x24);
    cpu_writel(t.qts, t.pio_cpu, t.head + 0x24,
               control | (1U << 22));
    g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, mem64), ==, 0x10213243);
    g_assert_cmphex(cpu_readl(t.qts, t.pio_cpu, mem64 + 4), ==,
                    0x54657687);

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
    dest.head = t.head;
    dest.window = t.window;
    g_assert_cmphex(reg_read(&dest, TI_PCI_STATE) & TI_PCI_32BIT_BUS, ==, 0);
    g_assert_cmphex(reg_read(&dest, TI_WINBASE), ==, 0x70000);
    g_assert_cmphex(cpu_readq(dest.qts, dest.pio_cpu,
                    SPACE_IO + NODE(7) + 0x01c00160), ==, 0x0e04);
    g_assert_cmphex(cpu_readl(dest.qts, dest.pio_cpu, dest.head + 0x20c) &
                    (1U << 22), ==, 0);
    g_assert_cmphex(cpu_readl(dest.qts, dest.pio_cpu, dest.head + 0x24) &
                    (1U << 22), ==, 1U << 22);
    g_assert_cmphex(cpu_readl(dest.qts, dest.pio_cpu, mem64), ==,
                    0x10213243);
    g_assert_cmphex(cpu_readl(dest.qts, dest.pio_cpu, mem64 + 4), ==,
                    0x54657687);
    cpu_writel(dest.qts, dest.pio_cpu, dest.head + 0x24,
               control & ~(1U << 22));
    g_assert_cmphex(cpu_readl(dest.qts, dest.pio_cpu, mem64), ==,
                    0x43322110);
    g_assert_cmphex(cpu_readl(dest.qts, dest.pio_cpu, mem64 + 4), ==,
                    0x87766554);
    cpu_writel(dest.qts, dest.pio_cpu, dest.head + 0x20c,
               DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20) | (1U << 22));
    g_assert_cmphex(cpu_readl(dest.qts, dest.pio_cpu, mem64), ==,
                    0x87766554);
    g_assert_cmphex(cpu_readl(dest.qts, dest.pio_cpu, mem64 + 4), ==,
                    0x43322110);
    cpu_writel(dest.qts, dest.pio_cpu, dest.head + 0x24,
               control | (1U << 22));
    g_assert_cmphex(cpu_readl(dest.qts, dest.pio_cpu, mem64), ==,
                    0x10213243);
    check_mem64_write_lifecycle(&dest, mem64,
        cpu_readl(dest.qts, dest.pio_cpu, dest.head + 0x20c), control);

    qtest_system_reset(dest.qts);
    setup_routes(&dest);
    setup_card(&dest);
    reg_write(&dest, TI_WINBASE, 0x70000);
    for (unsigned i = 0; i < sizeof(card_bytes); i++) {
        g_assert_true(qtest_memwrite_cpu(dest.qts, dest.pio_cpu,
                      dest.window + 0x800 + (i ^ 3), &card_bytes[i], 1));
    }
    cpu_writeq(dest.qts, dest.pio_cpu,
               SPACE_IO + NODE(7) + 0x01c00160, 0x0e04);
    g_assert_cmphex(cpu_readl(dest.qts, dest.pio_cpu, dest.head + 0x20c) &
                    (1U << 22), ==, 0);
    g_assert_cmphex(cpu_readl(dest.qts, dest.pio_cpu, dest.head + 0x24) &
                    (1U << 22), ==, 0);
    g_assert_cmphex(cpu_readl(dest.qts, dest.pio_cpu, mem64), ==,
                    0x43322110);
    cpu_writel(dest.qts, dest.pio_cpu, dest.head + 0x20c,
               DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20) | (1U << 22));
    g_assert_cmphex(cpu_readl(dest.qts, dest.pio_cpu, mem64), ==,
                    0x87766554);
    g_assert_cmphex(cpu_readl(dest.qts, dest.pio_cpu, mem64 + 4), ==,
                    0x43322110);
    cpu_writel(dest.qts, dest.pio_cpu, dest.head + 0x24,
               control | (1U << 22));
    g_assert_cmphex(cpu_readl(dest.qts, dest.pio_cpu, mem64), ==,
                    0x10213243);
    g_assert_cmphex(cpu_readl(dest.qts, dest.pio_cpu, mem64 + 4), ==,
                    0x54657687);
    check_mem64_write_lifecycle(&dest, mem64,
        cpu_readl(dest.qts, dest.pio_cpu, dest.head + 0x20c), control);

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
    static const WideSelector selectors[] = {
        { TI_PCI_NO_WORD_SWAP, TI_OPMODE_SWAP_CLEAR, true },
        { 0, TI_OPMODE_SWAP_CLEAR, false },
        { TI_PCI_NO_WORD_SWAP | TI_PCI_32BIT_BUS,
          TI_OPMODE_SWAP_CLEAR, false },
        { TI_PCI_NO_WORD_SWAP, TI_OPMODE_SWAP_CLEAR & ~4U, false },
        { TI_PCI_NO_WORD_SWAP, TI_OPMODE_SWAP_CLEAR | 2U, false },
    };
    static const char *selector_names[] = {
        "selected", "no-pci-bit18", "pci-bit20", "no-opmode-bit2",
        "opmode-bit1",
    };
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
    static Placement wide_rx = {
        .head = 0,
        .device = 1,
        .swap_clear = true,
        .gib_offset = 0x01a00000,
        .rx_descriptor_only = true,
    };
    static Placement payload_cases[22];
    static Placement payload_controls[] = {
        { .head = 0, .device = 1, .swap_clear = true,
          .payload_length = 8, .data_byteswap = true },
        { .head = 0, .device = 1, .swap_clear = true,
          .payload_length = 8, .pci_bit20 = true, .tx_only = true },
        { .head = 0, .device = 1, .swap_clear = true,
          .payload_length = 8, .clear_desc_word_swap = true,
          .tx_only = true },
        { .head = 0, .device = 1, .swap_clear = true,
          .payload_length = 8, .set_desc_byte_swap = true,
          .tx_only = true },
        { .head = 0, .device = 1, .swap_clear = true,
          .payload_length = 8, .payload_identity = true },
        { .head = 0, .device = 1, .swap_clear = true,
          .payload_length = 9, .payload_identity = true },
    };
    static const char *payload_control_names[] = {
        "data-byteswap-set", "pci-bit20", "no-desc-bit2",
        "desc-byte-bit1", "payload-identity", "payload-identity-edge",
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
    for (unsigned i = 0; i < G_N_ELEMENTS(payload_cases); i++) {
        Placement *p = &payload_cases[i];
        unsigned offset, length;
        g_autofree char *name = NULL;

        if (i < 8) {
            offset = i;
            length = 42;
        } else if (i < 17) {
            offset = (i - 8) & 7;
            length = i - 7;
        } else if (i < 19) {
            offset = i == 17 ? 3 : 5;
            length = i == 17 ? 41 : 43;
        } else if (i < 21) {
            offset = i == 19 ? 0xffc : 0xfff;
            length = i == 19 ? 41 : 43;
        } else {
            offset = 7;
            length = 9;
        }
        *p = (Placement) {
            .head = 0,
            .device = 1,
            .swap_clear = true,
            .payload_offset = offset,
            .payload_length = length,
            .tx_only = length < 6,
            .trace_dma = i == 21,
        };
        name = g_strdup_printf(
            "/sn1/option-tigon/wide-payload/offset%u-length%u",
            offset, length);
        qtest_add_data_func(name, p, test_traffic);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(payload_controls); i++) {
        g_autofree char *name = g_strdup_printf(
            "/sn1/option-tigon/wide-payload-control/%s",
            payload_control_names[i]);

        qtest_add_data_func(name, &payload_controls[i], test_traffic);
    }
    qtest_add_data_func("/sn1/option-tigon/wide-rx-descriptor", &wide_rx,
                        test_traffic);
    qtest_add_func("/sn1/option-tigon/wide-faulted-tail",
                   test_wide_faulted_tail);
    for (unsigned i = 0; i < G_N_ELEMENTS(selectors); i++) {
        g_autofree char *name = g_strdup_printf(
            "/sn1/option-tigon/wide-selector/%s", selector_names[i]);

        qtest_add_data_func(name, &selectors[i], test_wide_selector);
    }
    qtest_add_func("/sn1/option-tigon/pci-state-width",
                   test_pci_state_width);
    qtest_add_func("/sn1/option-tigon/pci-state-width-migration",
                   test_pci_state_width_migration);
    qtest_add_func("/sn1/option-tigon/direct-then-page-dma-readback",
                   test_direct_then_page_dma_readback);
    qtest_add_func("/sn1/option-tigon/direct-dma-write",
                   test_direct_dma_write);
    qtest_add_func("/sn1/option-tigon/wide-64bit-dma-write",
                   test_wide_64bit_dma_write);
    qtest_add_func("/sn1/option-tigon/generic-wide-64bit-dma-write",
                   test_generic_wide_64bit_dma_write);
    qtest_add_func("/sn1/option-tigon/pio-sram-direct-dma-bytes",
                   test_pio_sram_direct_dma_bytes);
    qtest_add_func("/sn1/option-tigon/dma-read-sram-window-words",
                   test_dma_read_sram_window_words);
    qtest_add_func("/sn1/option-tigon/mem64-write-width",
                   test_mem64_write_width);
    qtest_add_func("/sn1/option-tigon/mem64-write-unaligned",
                   test_mem64_write_unaligned);
    qtest_add_func("/sn1/option-tigon/mem64-device-width",
                   test_mem64_device_width);
    qtest_add_func("/sn1/option-tigon/mem64-write-callback",
                   test_mem64_write_callback);
    qtest_add_func("/sn1/option-tigon/mem64-narrow-card",
                   test_mem64_narrow_card);
    qtest_add_func("/sn1/option-tigon/mem64-other-head",
                   test_mem64_other_head);
    qtest_add_func("/sn1/option-tigon/mem64-overlapping-bars",
                   test_mem64_overlapping_bars);
    qtest_add_func("/sn1/option-tigon/mem64-failed-read",
                   test_mem64_failed_read);
    result = g_test_run();
    g_assert_cmpint(unlink(prom_path), ==, 0);
    g_free(prom_path);
    return result;
}
