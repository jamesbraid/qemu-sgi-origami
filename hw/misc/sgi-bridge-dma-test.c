/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A PCI device that exists to make two assertions about the I/O bridge
 * possible: one about how it translates a transfer, and one about how it
 * reports an interrupt pin.
 *
 * The bridge's address translation table is the thing a device transfer goes
 * through, and the defect it must not have is translating once per transfer
 * instead of once per page. That defect is invisible to every transfer fitting
 * inside one page, which is nearly all of them, so the only test that catches
 * it
 * is a transfer crossing a page boundary where the two pages map to physical
 * addresses that are not adjacent. Making that assertion needs a device on the
 * bridge's bus that issues one transfer of a length and address a test chooses.
 * Nothing in the tree could do it. The in-tree educational device is the
 * natural
 * candidate and is gated on a build option meaning the interrupt controller
 * supports message-signalled interrupts, which this machine's bridge does not
 * do — it turns a level on a pin into a write across the crossbar — so
 * selecting that option to obtain a test device would put a false claim about
 * the hardware
 * in the machine's configuration. The SCSI controller in the tree does transfer
 * data, and driving its script engine would test the translation table through
 * another chip's correctness. So this is a device of our own, deliberately the
 * smallest thing that makes the assertion.
 *
 * It is for tests and nothing else: no machine instantiates it, it appears
 * only when a command line asks for it by name, and it models no real
 * hardware. If it
 * ever seems useful for something other than exercising a host bridge's
 * translation, that is a sign something else should be built instead.
 * The transfer is one call, issued while the register write that asked for it
 * is
 * still being served, and it goes through the PCI DMA helpers so that whatever
 * translation the bridge above imposes is honoured. That is the whole point:
 * a transfer that bypassed the translation would prove nothing about it.
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

/* An identifier for a device that exists only in this fork's tests. */
#define SGI_BRIDGE_DMA_TEST_VENDOR 0x1234
#define SGI_BRIDGE_DMA_TEST_DEVICE 0x0d1a

struct SGIBridgeDMATestState {
    PCIDevice parent_obj;

    MemoryRegion mmio;

    /* Where on the PCI side to write, how much, and what. */
    uint32_t address;
    uint32_t length;
    uint32_t seed;

    /* The level this device is holding its interrupt pin at. */
    uint32_t interrupt;

    /*
     * Which of the four legacy interrupt pins this device declares, A through
     * D as one through four. A real device has this fixed in silicon; here it
     * is settable, because the thing a bridge test needs to vary is which pin
     * arrives — the fold from four bus pins onto the chip's eight is what is
     * under test, and one pin cannot show it.
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
#define REG_WINDOW      0x20   /* size of the register block */

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

static uint64_t sgi_bridge_dma_test_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    SGIBridgeDMATestState *s = opaque;

    switch (addr) {
    case REG_ADDRESS:
        return s->address;
    case REG_LENGTH:
        return s->length;
    case REG_SEED:
        return s->seed;
    case REG_INTERRUPT:
        return s->interrupt;
    default:
        return 0;
    }
}

static void sgi_bridge_dma_test_write(void *opaque, hwaddr addr, uint64_t val,
                                      unsigned size)
{
    SGIBridgeDMATestState *s = opaque;

    switch (addr) {
    case REG_ADDRESS:
        s->address = val;
        break;
    case REG_LENGTH:
        s->length = val;
        break;
    case REG_SEED:
        s->seed = val;
        break;
    case REG_GO:
        sgi_bridge_dma_test_go(s);
        break;
    case REG_INTERRUPT:
        s->interrupt = val ? 1 : 0;
        pci_set_irq(PCI_DEVICE(s), s->interrupt);
        break;
    default:
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
    .endianness = DEVICE_BIG_ENDIAN,
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
    s->interrupt = 0;
    pci_set_irq(PCI_DEVICE(s), 0);
}

static const VMStateDescription vmstate_sgi_bridge_dma_test = {
    .name = "sgi-bridge-dma-test",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SGIBridgeDMATestState),
        VMSTATE_UINT32(address, SGIBridgeDMATestState),
        VMSTATE_UINT32(length, SGIBridgeDMATestState),
        VMSTATE_UINT32(seed, SGIBridgeDMATestState),
        VMSTATE_UINT32(interrupt, SGIBridgeDMATestState),
        VMSTATE_UINT8(interrupt_pin, SGIBridgeDMATestState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sgi_bridge_dma_test_properties[] = {
    DEFINE_PROP_UINT8("interrupt-pin", SGIBridgeDMATestState, interrupt_pin, 1),
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
    /*
     * A command line may ask for it; no machine builds one. It is not on any
     * category list, because it belongs to none of them.
     */
    dc->user_creatable = true;
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
