/*
 * The libqos PCI bus over the Bridge head.
 *
 * Three routes onto one card stand for everything the bus offers: its
 * configuration space through the type 0 window, its I/O registers through a
 * device window, and the same registers through the window that reaches PCI
 * memory space whole. A transfer engine on the same bus then writes into the
 * memory the allocator hands out, which is the claim a card cannot check for
 * itself -- that an allocated address is one a card can reach.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_regs.h"
#include "libqtest.h"
#include "libqos/libqos-malloc.h"
#include "libqos/pci.h"
#include "libqos/pci-sgi-bridge.h"

/* Where the two cards sit, and a slot left empty for the bus to answer for. */
#define CARD_SLOT       1
#define TRANSFER_SLOT   2
#define EMPTY_SLOT      5

/* The card, and the least of it. */
#define CARD_VENDOR     0x1000
#define CARD_DEVICE     0x0012
#define CARD_SCRATCH    0x34
#define CARD_PATTERN    0x8b7a6950u

/* The transfer engine's registers, and what a write to the last one starts. */
#define XFER_ADDRESS    0x00
#define XFER_LENGTH     0x04
#define XFER_SEED       0x08
#define XFER_GO         0x0c
#define XFER_GO_FILL    1
#define XFER_LENGTH_VAL 64
#define XFER_SEED_VAL   0x31

static QTestState *head_start(void)
{
    return qtest_initf("-M none " QPCI_SGI_BRIDGE_HEAD
                       "-device lsi53c895a,bus=pci,addr=%d "
                       "-device sgi-bridge-dma-test,bus=pci,addr=%d",
                       CARD_SLOT, TRANSFER_SLOT);
}

/*
 * The identity read every driver takes first, whole and in halves and bytes:
 * a bus that aimed a narrow access at the wrong byte would still answer the
 * whole double word correctly.
 */
static void test_configuration_space(void)
{
    QTestState *qts = head_start();
    QPCIBus *bus = qpci_init_sgi_bridge(qts, NULL);
    QPCIDevice *dev = qpci_device_find(bus, QPCI_DEVFN(CARD_SLOT, 0));

    g_assert_nonnull(dev);
    g_assert_cmphex(qpci_config_readw(dev, PCI_VENDOR_ID), ==, CARD_VENDOR);
    g_assert_cmphex(qpci_config_readw(dev, PCI_DEVICE_ID), ==, CARD_DEVICE);
    g_assert_cmphex(qpci_config_readl(dev, PCI_VENDOR_ID), ==,
                    CARD_VENDOR | (CARD_DEVICE << 16));
    g_assert_cmphex(qpci_config_readb(dev, PCI_VENDOR_ID), ==,
                    CARD_VENDOR & 0xff);
    g_assert_cmphex(qpci_config_readb(dev, PCI_VENDOR_ID + 1), ==,
                    CARD_VENDOR >> 8);

    g_free(dev);
    qpci_free_sgi_bridge(bus);
    qtest_quit(qts);
}

/* A slot with nothing in it has no device to find. */
static void test_empty_slot_has_no_device(void)
{
    QTestState *qts = head_start();
    QPCIBus *bus = qpci_init_sgi_bridge(qts, NULL);

    g_assert_null(qpci_device_find(bus, QPCI_DEVFN(EMPTY_SLOT, 0)));

    qpci_free_sgi_bridge(bus);
    qtest_quit(qts);
}

/*
 * The card's I/O BAR and its memory BAR reach one scratch register, so a
 * value written through either is the value read back through the other.
 * Read again a halfword and a byte at a time, which is where a bus that got
 * the byte order right only for whole words comes apart.
 */
static void test_both_bars_reach_one_register(void)
{
    QTestState *qts = head_start();
    QPCIBus *bus = qpci_init_sgi_bridge(qts, NULL);
    QPCIDevice *dev = qpci_device_find(bus, QPCI_DEVFN(CARD_SLOT, 0));
    QPCIBar io, mmio;
    unsigned int i;

    g_assert_nonnull(dev);
    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);
    mmio = qpci_iomap(dev, 1, NULL);
    g_assert_true(io.is_io);
    g_assert_false(mmio.is_io);

    qpci_io_writel(dev, mmio, CARD_SCRATCH, CARD_PATTERN);
    g_assert_cmphex(qpci_io_readl(dev, io, CARD_SCRATCH), ==, CARD_PATTERN);

    qpci_io_writel(dev, io, CARD_SCRATCH, ~CARD_PATTERN);
    g_assert_cmphex(qpci_io_readl(dev, mmio, CARD_SCRATCH), ==, ~CARD_PATTERN);

    g_assert_cmphex(qpci_io_readw(dev, io, CARD_SCRATCH), ==,
                    ~CARD_PATTERN & 0xffff);
    g_assert_cmphex(qpci_io_readw(dev, mmio, CARD_SCRATCH + 2), ==,
                    (~CARD_PATTERN >> 16) & 0xffff);

    for (i = 0; i < 4; i++) {
        g_assert_cmphex(qpci_io_readb(dev, io, CARD_SCRATCH + i), ==,
                        (~CARD_PATTERN >> (8 * i)) & 0xff);
        g_assert_cmphex(qpci_io_readb(dev, mmio, CARD_SCRATCH + i), ==,
                        (~CARD_PATTERN >> (8 * i)) & 0xff);
    }

    g_free(dev);
    qpci_free_sgi_bridge(bus);
    qtest_quit(qts);
}

/*
 * A card transfers into a page the allocator handed out, and the same address
 * reads back the pattern. Nothing else here proves that an address good for
 * qtest is an address a card can name.
 */
static void test_transfer_reaches_allocated_memory(void)
{
    QTestState *qts = head_start();
    QGuestAllocator alloc;
    QPCIBus *bus;
    QPCIDevice *dev;
    QPCIBar mmio;
    uint8_t seen[XFER_LENGTH_VAL];
    uint64_t at;
    unsigned int i;

    qpci_sgi_bridge_alloc_init(&alloc, ALLOC_NO_FLAGS);
    bus = qpci_init_sgi_bridge(qts, &alloc);
    dev = qpci_device_find(bus, QPCI_DEVFN(TRANSFER_SLOT, 0));
    g_assert_nonnull(dev);
    qpci_device_enable(dev);
    mmio = qpci_iomap(dev, 0, NULL);

    at = guest_alloc(&alloc, 4096);
    qpci_io_writel(dev, mmio, XFER_ADDRESS, at);
    qpci_io_writel(dev, mmio, XFER_LENGTH, XFER_LENGTH_VAL);
    qpci_io_writel(dev, mmio, XFER_SEED, XFER_SEED_VAL);
    qpci_io_writel(dev, mmio, XFER_GO, XFER_GO_FILL);

    qtest_memread(qts, at, seen, sizeof(seen));
    for (i = 0; i < sizeof(seen); i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(XFER_SEED_VAL + i));
    }

    guest_free(&alloc, at);
    g_free(dev);
    qpci_free_sgi_bridge(bus);
    alloc_destroy(&alloc);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sgi-bridge-bus/configuration-space",
                   test_configuration_space);
    qtest_add_func("/sgi-bridge-bus/empty-slot-has-no-device",
                   test_empty_slot_has_no_device);
    qtest_add_func("/sgi-bridge-bus/both-bars-reach-one-register",
                   test_both_bars_reach_one_register);
    qtest_add_func("/sgi-bridge-bus/transfer-reaches-allocated-memory",
                   test_transfer_reaches_allocated_memory);

    return g_test_run();
}
