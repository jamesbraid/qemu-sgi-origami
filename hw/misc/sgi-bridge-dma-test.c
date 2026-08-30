/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A test-only PCI device for two Bridge assertions: per-page translation
 * (only a transfer straddling a page boundary onto non-adjacent physical
 * pages can catch translate-once-per-transfer) and a held interrupt level
 * (a status register that must follow, not latch, cannot be told apart by
 * a pulse). It differs from iommu-testdev in a seeded byte pattern, the
 * held level, and a settable PCI identity for the byte-lane policy;
 * retiring it onto an extended iommu-testdev is recorded follow-up work.
 * No machine instantiates it, and its transfers go through the PCI DMA
 * helpers so the Bridge's translation is honoured.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_SGI_BRIDGE_DMA_TEST "sgi-bridge-dma-test"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBridgeDMATestState, SGI_BRIDGE_DMA_TEST)

/*
 * The longest transfer this device will issue. It only has to be longer than a
 * translation page is not, which is the opposite requirement from a real
 * device's: the transfer must be short enough to straddle a boundary by a few
 * bytes so that the assertion names two pages rather than a range.
 */
#define SGI_BRIDGE_DMA_TEST_MAX 256

/*
 * The identity every QEMU-internal test device wears: an allocated Red Hat
 * id rather than a made-up number that could collide with a real vendor.
 */
#define SGI_BRIDGE_DMA_TEST_VENDOR PCI_VENDOR_ID_REDHAT
#define SGI_BRIDGE_DMA_TEST_DEVICE PCI_DEVICE_ID_REDHAT_TEST

struct SGIBridgeDMATestState {
    PCIDevice parent_obj;

    MemoryRegion mmio;

    /*
     * Where on the PCI side to write, how much, and what. The address is 64 bits
     * so a test can issue a 64-bit address cycle and exercise the Bridge's
     * 64-bit direct decode; a 32-bit register file cannot reach those addresses.
     */
    uint64_t address;
    uint32_t length;
    uint32_t seed;

    /*
     * A copy tests the read direction: exposing the buffer through a
     * register window would put a second transform in the path.
     */
    uint64_t source;

    /* The level this device is holding its interrupt pin at. */
    uint32_t interrupt;

    /* The identity this device presents, which a test may override. */
    uint16_t vendor_id;
    uint16_t device_id;

    /*
     * Settable where silicon is fixed: the four-onto-eight pin fold is
     * what is under test, and one pin cannot show it.
     */
    uint8_t interrupt_pin;
};

#define REG_ADDRESS     0x00
#define REG_LENGTH      0x04
#define REG_SEED        0x08
#define REG_GO          0x0c
/*
 * Writing here drives this device's first interrupt pin to the level written,
 * and holds it there. It is a level and not a pulse deliberately: what the
 * bridge above has to get right is that its status register follows the level
 * and never latches, and a source that could only pulse could not tell a model
 * that follows from one that latches — both would show the bit set.
 */
#define REG_INTERRUPT   0x10
#define REG_SOURCE      0x14
/*
 * The high 32 bits of the destination and source addresses. Each address is set
 * with two 32-bit writes — the low register, then the high register — because
 * the register window is reached with 32-bit accesses.
 */
#define REG_ADDRESS_HI  0x18
#define REG_SOURCE_HI   0x1c
#define REG_WINDOW      0x20   /* size of the register block */

/* What a write to REG_GO asks for. */
#define GO_FILL         1      /* write the seeded pattern at REG_ADDRESS */
#define GO_COPY         2      /* read REG_SOURCE, write REG_ADDRESS */

/*
 * What the device writes: a byte sequence a test can predict without this file
 * telling it, so that an assertion about where the bytes landed does not also
 * depend on agreeing what the bytes were.
 */
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

    /*
     * One call for the whole transfer, which is the part that matters. A host
     * bridge translating once per transfer and one translating once per page
     * differ only here, and only when this call spans a page boundary.
     */
    pci_dma_write(PCI_DEVICE(s), s->address, buf, s->length);
}

/*
 * Read one place and write another, both through the bridge. With the same
 * settings at both ends this is a pure copy and any difference is a bug; with
 * different settings at the two ends exactly one transform sits in the path,
 * which is what lets a test assert the read direction without a second
 * correction cancelling the first.
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

/*
 * The register block is reached through a host bridge's device window, and this
 * machine is big-endian, so a value written through that window arrives here as
 * the number it was written as. Both access sizes are declared because this
 * handler is written for 32-bit accesses and nothing else.
 */
static const MemoryRegionOps sgi_bridge_dma_test_ops = {
    .read = sgi_bridge_dma_test_read,
    .write = sgi_bridge_dma_test_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void sgi_bridge_dma_test_realize(PCIDevice *dev, Error **errp)
{
    SGIBridgeDMATestState *s = SGI_BRIDGE_DMA_TEST(dev);

    /* Say in configuration space which pin this device uses. */
    pci_config_set_interrupt_pin(dev->config, s->interrupt_pin);

    /*
     * And which part it is claiming to be. The class supplies the default;
     * a test overrides it to stand in for a device the bridge treats
     * differently, without needing that device to exist.
     */
    pci_config_set_vendor_id(dev->config, s->vendor_id);
    pci_config_set_device_id(dev->config, s->device_id);

    memory_region_init_io(&s->mmio, OBJECT(s), &sgi_bridge_dma_test_ops, s,
                          "sgi-bridge-dma-test.mmio", REG_WINDOW);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
}

static void sgi_bridge_dma_test_reset_hold(Object *obj, ResetType type)
{
    SGIBridgeDMATestState *s = SGI_BRIDGE_DMA_TEST(obj);

    s->address = 0;
    s->length = 0;
    s->seed = 0;
    s->source = 0;
    s->interrupt = 0;
    pci_set_irq(PCI_DEVICE(s), 0);
}

static const VMStateDescription vmstate_sgi_bridge_dma_test = {
    .name = "sgi-bridge-dma-test",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SGIBridgeDMATestState),
        VMSTATE_UINT64(address, SGIBridgeDMATestState),
        VMSTATE_UINT64(source, SGIBridgeDMATestState),
        VMSTATE_UINT32(length, SGIBridgeDMATestState),
        VMSTATE_UINT32(seed, SGIBridgeDMATestState),
        VMSTATE_UINT32(interrupt, SGIBridgeDMATestState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sgi_bridge_dma_test_properties[] = {
    DEFINE_PROP_UINT8("interrupt-pin", SGIBridgeDMATestState, interrupt_pin, 1),
    /*
     * So a test can make this device wear another part's identity. The bridge
     * decides which devices are exempt from its byte-lane correction by PCI
     * vendor and device id, and a test that proved the exemption by moving a
     * real part into a slot would be testing the slot as much as the rule.
     */
    DEFINE_PROP_UINT16("vendor-id", SGIBridgeDMATestState, vendor_id,
                       SGI_BRIDGE_DMA_TEST_VENDOR),
    DEFINE_PROP_UINT16("device-id", SGIBridgeDMATestState, device_id,
                       SGI_BRIDGE_DMA_TEST_DEVICE),
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
