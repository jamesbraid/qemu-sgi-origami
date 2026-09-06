/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test-only PCI device for the Bridge qtests: page-straddling DMA with a
 * seeded pattern, a held interrupt level, and a settable PCI identity.
 * Like pci-testdev, no machine instantiates it. Retiring it onto an
 * extended iommu-testdev is recorded follow-up work.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_SGI_BRIDGE_DMA_TEST "sgi-bridge-dma-test"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBridgeDMATestState, SGI_BRIDGE_DMA_TEST)

/* Short on purpose: a straddle should cross a boundary by a few bytes. */
#define SGI_BRIDGE_DMA_TEST_MAX 256

/* The allocated Red Hat test-device identity. */
#define SGI_BRIDGE_DMA_TEST_VENDOR PCI_VENDOR_ID_REDHAT
#define SGI_BRIDGE_DMA_TEST_DEVICE PCI_DEVICE_ID_REDHAT_TEST

struct SGIBridgeDMATestState {
    PCIDevice parent_obj;

    MemoryRegion mmio;

    /*
     * A second BAR of 64-bit registers — a read-only constant and a writable
     * scratch — put deliberately behind the Bridge's programmed-access
     * correction, so a test can exercise a 64-bit device window: served one
     * eight-byte cycle, mirrored whole on the slot's swap bit. A fixed value
     * keeps the assertion about the Bridge's byte handling, not this device's
     * storage.
     */
    MemoryRegion wide_mmio;
    uint64_t wide_scratch;

    /*
     * A third BAR in PCI I/O space: one byte-addressable doubleword, so a
     * test can tell which byte of I/O space a bridge's I/O path reached.
     */
    MemoryRegion io;
    uint64_t io_scratch;

    /* 64-bit so a test can exercise the Bridge's 64-bit direct decode. */
    uint64_t address;
    uint32_t length;
    uint32_t seed;

    /* A copy tests the read direction without a second transform. */
    uint64_t source;

    /* The level this device is holding its interrupt pin at. */
    uint32_t interrupt;

    /* The identity this device presents, which a test may override. */
    uint16_t vendor_id;
    uint16_t device_id;

    /*
     * Whether this card tells the bridge it orders its own byte lanes, so a
     * test can stand one in for SGI silicon that does. Off by default,
     * which is what an ordinary card says.
     */
    bool byte_lane_self_ordered;

    /* Settable: the four-onto-eight pin fold needs more than one pin. */
    uint8_t interrupt_pin;
};

#define REG_ADDRESS     0x00
#define REG_LENGTH      0x04
#define REG_SEED        0x08
#define REG_GO          0x0c
/*
 * Drives and holds the pin at the level written: a pulse could not tell a
 * status register that follows from one that latches.
 */
#define REG_INTERRUPT   0x10
#define REG_SOURCE      0x14
/* Address high halves; the window is reached with 32-bit accesses. */
#define REG_ADDRESS_HI  0x18
#define REG_SOURCE_HI   0x1c
#define REG_WINDOW      0x20   /* size of the register block */
#define IO_WINDOW       0x08   /* size of the I/O-space scratch */

/* What a write to REG_GO asks for. */
#define GO_FILL         1      /* write the seeded pattern at REG_ADDRESS */
#define GO_COPY         2      /* read REG_SOURCE, write REG_ADDRESS */

/*
 * The second BAR's registers. The fixed value is asymmetric end to end so a
 * byte-lane-neutral constant cannot hide a wrong swap.
 */
#define WIDE_FIXED      0x00   /* read-only constant */
#define WIDE_SCRATCH    0x08   /* read/write doubleword */
#define WIDE_WINDOW     0x10   /* size of the 64-bit register block */
#define WIDE_FIXED_VALUE 0x0123456789abcdefULL

/* A pattern a test can predict without this file telling it. */
static uint8_t sgi_bridge_dma_test_byte(uint32_t seed, unsigned int i)
{
    return (uint8_t)(seed + i);
}

static void sgi_bridge_dma_test_go(SGIBridgeDMATestState *s)
{
    uint8_t buf[SGI_BRIDGE_DMA_TEST_MAX];
    unsigned int i;

    if (s->length > sizeof(buf)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge-dma-test: asked for %u bytes, which is more "
                      "than this device will transfer\n", s->length);
        return;
    }

    for (i = 0; i < s->length; i++) {
        buf[i] = sgi_bridge_dma_test_byte(s->seed, i);
    }

    /* One call for the whole transfer: the per-page distinction lives here. */
    pci_dma_write(PCI_DEVICE(s), s->address, buf, s->length);
}

/*
 * With different settings at the two ends exactly one transform sits in
 * the path, so the read direction is assertable on its own.
 */
static void sgi_bridge_dma_test_copy(SGIBridgeDMATestState *s)
{
    uint8_t buf[SGI_BRIDGE_DMA_TEST_MAX];

    if (s->length > sizeof(buf)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge-dma-test: asked to copy %u bytes, which is "
                      "more than this device will transfer\n", s->length);
        return;
    }

    pci_dma_read(PCI_DEVICE(s), s->source, buf, s->length);
    pci_dma_write(PCI_DEVICE(s), s->address, buf, s->length);
}

static uint64_t sgi_bridge_dma_test_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    SGIBridgeDMATestState *s = opaque;

    switch (addr) {
    case REG_ADDRESS:
        return (uint32_t)s->address;
    case REG_ADDRESS_HI:
        return s->address >> 32;
    case REG_LENGTH:
        return s->length;
    case REG_SEED:
        return s->seed;
    case REG_SOURCE:
        return (uint32_t)s->source;
    case REG_SOURCE_HI:
        return s->source >> 32;
    case REG_INTERRUPT:
        return s->interrupt;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge-dma-test: read of undefined register 0x%"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void sgi_bridge_dma_test_write(void *opaque, hwaddr addr, uint64_t val,
                                      unsigned size)
{
    SGIBridgeDMATestState *s = opaque;

    switch (addr) {
    case REG_ADDRESS:
        s->address = deposit64(s->address, 0, 32, val);
        break;
    case REG_ADDRESS_HI:
        s->address = deposit64(s->address, 32, 32, val);
        break;
    case REG_LENGTH:
        s->length = val;
        break;
    case REG_SEED:
        s->seed = val;
        break;
    case REG_GO:
        if (val == GO_COPY) {
            sgi_bridge_dma_test_copy(s);
        } else {
            sgi_bridge_dma_test_go(s);
        }
        break;
    case REG_SOURCE:
        s->source = deposit64(s->source, 0, 32, val);
        break;
    case REG_SOURCE_HI:
        s->source = deposit64(s->source, 32, 32, val);
        break;
    case REG_INTERRUPT:
        s->interrupt = val ? 1 : 0;
        pci_set_irq(PCI_DEVICE(s), s->interrupt);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge-dma-test: write of undefined register 0x%"
                      HWADDR_PRIx "\n", addr);
        break;
    }
}

/* 32-bit accesses only; both bounds declared so the core enforces it. */
static const MemoryRegionOps sgi_bridge_dma_test_ops = {
    .read = sgi_bridge_dma_test_read,
    .write = sgi_bridge_dma_test_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * The 64-bit register window: a read-only constant and a writable scratch, each
 * returning the size-byte little-endian slice at the accessed offset, so a split
 * read reassembles the same value a one-cycle read gives. What differs is the
 * Bridge's whole-doubleword swap, which is what a test reads to see. The
 * eight-byte impl size lets a one-cycle doubleword reach the handler whole.
 */
static uint64_t sgi_bridge_dma_test_wide_read(void *opaque, hwaddr addr,
                                              unsigned size)
{
    SGIBridgeDMATestState *s = opaque;
    uint64_t reg = addr < WIDE_SCRATCH ? WIDE_FIXED_VALUE : s->wide_scratch;

    return extract64(reg, (addr & 7) * 8, size * 8);
}

static void sgi_bridge_dma_test_wide_write(void *opaque, hwaddr addr,
                                           uint64_t val, unsigned size)
{
    SGIBridgeDMATestState *s = opaque;

    if (addr < WIDE_SCRATCH) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge-dma-test: write to the read-only 64-bit "
                      "register 0x%" HWADDR_PRIx "\n", addr);
        return;
    }
    s->wide_scratch = deposit64(s->wide_scratch, (addr & 7) * 8, size * 8, val);
}

/*
 * Down to a byte, because the constant's eight bytes all differ: a narrow
 * read through a device window says which byte of the register it reached,
 * which is what makes the bridge's byte-lane addressing assertable. A wide
 * access still arrives whole, so the doubleword tests are unaffected.
 */
static const MemoryRegionOps sgi_bridge_dma_test_wide_ops = {
    .read = sgi_bridge_dma_test_wide_read,
    .write = sgi_bridge_dma_test_wide_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static uint64_t sgi_bridge_dma_test_io_read(void *opaque, hwaddr addr,
                                            unsigned size)
{
    SGIBridgeDMATestState *s = opaque;

    return extract64(s->io_scratch, (addr & 7) * 8, size * 8);
}

static void sgi_bridge_dma_test_io_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned size)
{
    SGIBridgeDMATestState *s = opaque;

    s->io_scratch = deposit64(s->io_scratch, (addr & 7) * 8, size * 8, val);
}

/* Down to a byte for the same reason as the wide registers. */
static const MemoryRegionOps sgi_bridge_dma_test_io_ops = {
    .read = sgi_bridge_dma_test_io_read,
    .write = sgi_bridge_dma_test_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static void sgi_bridge_dma_test_realize(PCIDevice *dev, Error **errp)
{
    SGIBridgeDMATestState *s = SGI_BRIDGE_DMA_TEST(dev);

    /* Say in configuration space which pin this device uses. */
    pci_config_set_interrupt_pin(dev->config, s->interrupt_pin);

    /* The claimed identity; a test overrides it to stand in for a part. */
    pci_config_set_vendor_id(dev->config, s->vendor_id);
    pci_config_set_device_id(dev->config, s->device_id);

    memory_region_init_io(&s->mmio, OBJECT(s), &sgi_bridge_dma_test_ops, s,
                          "sgi-bridge-dma-test.mmio", REG_WINDOW);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    memory_region_init_io(&s->wide_mmio, OBJECT(s),
                          &sgi_bridge_dma_test_wide_ops, s,
                          "sgi-bridge-dma-test.wide", WIDE_WINDOW);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->wide_mmio);

    memory_region_init_io(&s->io, OBJECT(s), &sgi_bridge_dma_test_io_ops, s,
                          "sgi-bridge-dma-test.io", IO_WINDOW);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
}

static void sgi_bridge_dma_test_reset_hold(Object *obj, ResetType type)
{
    SGIBridgeDMATestState *s = SGI_BRIDGE_DMA_TEST(obj);

    s->address = 0;
    s->length = 0;
    s->seed = 0;
    s->source = 0;
    s->wide_scratch = 0;
    s->io_scratch = 0;
    s->interrupt = 0;
    pci_set_irq(PCI_DEVICE(s), 0);
}

static const VMStateDescription vmstate_sgi_bridge_dma_test = {
    .name = "sgi-bridge-dma-test",
    .version_id = 5,
    .minimum_version_id = 5,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SGIBridgeDMATestState),
        VMSTATE_UINT64(address, SGIBridgeDMATestState),
        VMSTATE_UINT64(source, SGIBridgeDMATestState),
        VMSTATE_UINT64(wide_scratch, SGIBridgeDMATestState),
        VMSTATE_UINT64(io_scratch, SGIBridgeDMATestState),
        VMSTATE_UINT32(length, SGIBridgeDMATestState),
        VMSTATE_UINT32(seed, SGIBridgeDMATestState),
        VMSTATE_UINT32(interrupt, SGIBridgeDMATestState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sgi_bridge_dma_test_properties[] = {
    DEFINE_PROP_UINT8("interrupt-pin", SGIBridgeDMATestState, interrupt_pin, 1),
    /* The part a test stands this card in for is named by its identity. */
    DEFINE_PROP_UINT16("vendor-id", SGIBridgeDMATestState, vendor_id,
                       SGI_BRIDGE_DMA_TEST_VENDOR),
    DEFINE_PROP_UINT16("device-id", SGIBridgeDMATestState, device_id,
                       SGI_BRIDGE_DMA_TEST_DEVICE),
    /* Settable, so one card type covers both sides of the exemption. */
    DEFINE_PROP_BOOL(SGI_BRIDGE_BYTE_LANE_SELF_ORDERED, SGIBridgeDMATestState,
                     byte_lane_self_ordered, false),
};

static void sgi_bridge_dma_test_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    k->realize = sgi_bridge_dma_test_realize;
    k->vendor_id = SGI_BRIDGE_DMA_TEST_VENDOR;
    k->device_id = SGI_BRIDGE_DMA_TEST_DEVICE;
    k->class_id = PCI_CLASS_OTHERS;
    dc->desc = "transfer source for testing a host bridge's translation";
    dc->vmsd = &vmstate_sgi_bridge_dma_test;
    rc->phases.hold = sgi_bridge_dma_test_reset_hold;
    device_class_set_props(dc, sgi_bridge_dma_test_properties);
    /* A command line may ask for it; no machine builds one. */
    dc->user_creatable = true;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo sgi_bridge_dma_test_types[] = {
    {
        .name = TYPE_SGI_BRIDGE_DMA_TEST,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(SGIBridgeDMATestState),
        .class_init = sgi_bridge_dma_test_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(sgi_bridge_dma_test_types)
