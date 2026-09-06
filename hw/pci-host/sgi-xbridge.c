/* SPDX-License-Identifier: GPL-2.0-or-later */
/* XBridge PCI head, implemented from the cleared XBridge contract. */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/pci-host/sgi-xbridge.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "qemu/bswap.h"
#include "qapi/error.h"

#define TYPE_XBRIDGE_IOMMU "sgi-xbridge-iommu"

#define REG_COUNT (0x300 / 8)
#define ATE_COUNT 512
#define REG(s, off) ((s)->reg[(off) / 8])

struct SGIXBridgeHeadState {
    PCIHostState parent_obj;
    MemoryRegion widget;
    MemoryRegion pci_mem;
    MemoryRegion pci_io;
    AddressSpace pci_mem_as;
    AddressSpace pci_io_as;
    uint32_t reg[REG_COUNT];
    uint64_t ate[ATE_COUNT];
    uint8_t speed;
    uint8_t pins;
    MemoryRegion *target[16];
    struct XBridgeTarget {
        AddressSpace as;
        MemoryRegion swap;
        AddressSpace swap_as;
    } endpoint[16];
    struct XBridgeDMA {
        IOMMUMemoryRegion iommu;
        AddressSpace as;
        SGIXBridgeHeadState *head;
        unsigned slot;
    } dma[8];
    MemoryRegion denied;
    AddressSpace denied_as;
    MemoryRegion flush;
    AddressSpace flush_as;
};

static void xbridge_packet(SGIXBridgeHeadState *s, unsigned pin, bool set)
{
    uint32_t upper = REG(s, 0x30);
    uint32_t host = REG(s, 0x130 + pin * 8);
    unsigned target = (upper >> 16) & 15;
    uint64_t addr = ((uint64_t)(upper & 0x3f) << 32) | REG(s, 0x38);
    MemTxResult result;

    addr |= (uint64_t)(host >> 8) << 38;
    if (!s->target[target]) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xbridge: interrupt target %x is absent\n", target);
        return;
    }
    address_space_stq_be(&s->endpoint[target].as, addr,
                         (host & 0xff) | (set ? 0x100 : 0),
                         MEMTXATTRS_UNSPECIFIED, &result);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xbridge: interrupt destination refused write\n");
    }
}

static void xbridge_set_pin(void *opaque, int pin, int level)
{
    SGIXBridgeHeadState *s = opaque;
    uint8_t bit = 1U << pin;
    bool old = s->pins & bit;

    s->pins = (s->pins & ~bit) | (level ? bit : 0);
    REG(s, 0x100) = (REG(s, 0x100) & ~0xffU) | s->pins;
    if (old == !!level || !(REG(s, 0x108) & bit)) {
        return;
    }
    if (level || (REG(s, 0x118) & bit)) {
        xbridge_packet(s, pin, level);
    }
}

static MemTxResult xbridge_swap_read(void *opaque, hwaddr addr,
                                     uint64_t *value, unsigned size,
                                     MemTxAttrs attrs)
{
    struct XBridgeTarget *target = opaque;
    uint8_t bytes[4] = { 0 };
    MemTxResult result;

    result = address_space_read(&target->as, addr ^ (4 - size), attrs,
                                bytes, size);
    *value = ldn_be_p(bytes, size);
    return result;
}

static MemTxResult xbridge_swap_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size,
                                      MemTxAttrs attrs)
{
    struct XBridgeTarget *target = opaque;
    uint8_t bytes[4];

    stn_be_p(bytes, size, value);
    return address_space_write(&target->as, addr ^ (4 - size), attrs,
                               bytes, size);
}

static const MemoryRegionOps xbridge_swap_ops = {
    .read_with_attrs = xbridge_swap_read,
    .write_with_attrs = xbridge_swap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static uint64_t xbridge_flush_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void xbridge_flush_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    /* Transfers are synchronous; no buffered writes remain to drain. */
}

static const MemoryRegionOps xbridge_flush_ops = {
    .read = xbridge_flush_read,
    .write = xbridge_flush_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static IOMMUTLBEntry xbridge_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                      IOMMUAccessFlags flag, int iommu_idx)
{
    struct XBridgeDMA *dma = container_of(iommu, struct XBridgeDMA, iommu);
    SGIXBridgeHeadState *s = dma->head;
    IOMMUTLBEntry entry = {
        .iova = addr & ~0xfffULL,
        .addr_mask = 0xfff,
        .perm = IOMMU_NONE,
    };
    unsigned widget;
    uint64_t destination;
    bool swap;

    if (addr >= 0x3fff0000 && addr < 0x40000000) {
        entry.target_as = &s->flush_as;
        entry.translated_addr = addr & 0xf000;
        entry.perm = IOMMU_RW;
        return entry;
    }
    if (addr >= 0x40000000 && addr <= 0x407fffff) {
        unsigned shift = (REG(s, 0x20) & (1U << 21)) ? 14 : 12;
        unsigned index = (addr & 0x3fffffff) >> shift;
        uint64_t ate;

        entry.addr_mask = (1ULL << shift) - 1;
        entry.iova = addr & ~entry.addr_mask;
        if (index >= G_N_ELEMENTS(s->ate) || !(s->ate[index] & 1)) {
            REG(s, 0x100) |= 1U << 30;
            REG(s, 0x170) |= 1U << 30;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-xbridge: invalid page DMA at 0x%"
                          HWADDR_PRIx " (entry %u)\n", addr, index);
            return entry;
        }
        ate = s->ate[index];
        widget = (ate >> 8) & 15;
        destination = ate & 0x0000fffffffff000ULL;
        swap = ate & (1U << 5);
    } else if (addr >= 0x1000000000000000ULL) {
        widget = addr >> 60;
        destination = addr & 0x0000ffffffffffffULL;
        swap = addr & (1ULL << 55);
    } else if (addr >= 0x80000000 && addr <= UINT32_MAX) {
        uint32_t direct = REG(s, 0x80);

        widget = (direct >> 20) & 15;
        destination = ((uint64_t)(direct & 0x1ffff) << 31) |
                       (addr & 0x7fffffff);
        if (!(direct & 0x1ffff) && (direct & (1U << 17))) {
            destination += 0x20000000;
        }
        swap = REG(s, 0x200 + dma->slot * 8) & (1U << 19);
    } else {
        qemu_log_mask(addr >= 0x40000000 && addr < 0x80000000 ?
                      LOG_UNIMP : LOG_GUEST_ERROR,
                      "sgi-xbridge: unsupported DMA address 0x%"
                      HWADDR_PRIx "\n", addr);
        return entry;
    }
    if (!s->target[widget]) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xbridge: DMA target %x is absent\n", widget);
        return entry;
    }
    entry.target_as = swap ? &s->endpoint[widget].as :
                             &s->endpoint[widget].swap_as;
    entry.translated_addr = destination & ~entry.addr_mask;
    entry.perm = IOMMU_RW;
    return entry;
}

static AddressSpace *xbridge_dma_as(PCIBus *bus, void *opaque, int devfn)
{
    SGIXBridgeHeadState *s = opaque;

    if (devfn < 0 || PCI_SLOT(devfn) >= G_N_ELEMENTS(s->dma)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xbridge: unsupported requester devfn %x\n", devfn);
        return &s->denied_as;
    }
    return &s->dma[PCI_SLOT(devfn)].as;
}

static const PCIIOMMUOps xbridge_iommu_ops = {
    .get_address_space = xbridge_dma_as,
};

static int xbridge_notify_flags(IOMMUMemoryRegion *iommu,
                                IOMMUNotifierFlag old_flags,
                                IOMMUNotifierFlag new_flags, Error **errp)
{
    if (new_flags != IOMMU_NOTIFIER_NONE) {
        error_setg(errp, "XBridge does not support cached DMA mappings");
        return -EINVAL;
    }
    return 0;
}

static void xbridge_iommu_class_init(ObjectClass *oc, const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(oc);

    imrc->translate = xbridge_translate;
    imrc->notify_flag_changed = xbridge_notify_flags;
}

static void xbridge_invalid(SGIXBridgeHeadState *s, hwaddr addr)
{
    REG(s, 0x100) |= 1U << 24;
    REG(s, 0x170) |= 1U << 24;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-xbridge: invalid access at 0x%" HWADDR_PRIx "\n", addr);
}

static uint32_t xbridge_config_address(SGIXBridgeHeadState *s,
                                       hwaddr addr, unsigned size)
{
    uint32_t devfn;
    unsigned reg = addr & 0xff;
    unsigned bus = 0;

    if (addr < 0x28000) {
        devfn = PCI_DEVFN((addr - 0x20000) >> 12, (addr >> 8) & 7);
    } else {
        bus = (REG(s, 0xc8) >> 16) & 0xff;
        devfn = (REG(s, 0xc8) >> 8) & 0xf8;
        devfn |= (addr >> 8) & 7;
    }
    reg ^= 4 - size;
    return (bus << 16) | (devfn << 8) | reg;
}

static MemTxResult xbridge_pio(SGIXBridgeHeadState *s, hwaddr addr,
                               uint8_t *bytes, unsigned size, bool write)
{
    unsigned slot;
    hwaddr offset, base;
    uint32_t dev;
    unsigned width, i;
    AddressSpace *as;
    MemTxResult result;

    if (addr < 0x600000) {
        slot = (addr - 0x200000) >> 21;
        offset = addr & 0x1fffff;
    } else {
        slot = 2 + ((addr - 0x600000) >> 20);
        offset = addr & 0xfffff;
    }
    dev = REG(s, 0x200 + slot * 8);
    width = dev & (1U << 22) ? 8 : 4;
    if (size > width) {
        result = xbridge_pio(s, addr, bytes, 4, write);
        return result | xbridge_pio(s, addr + 4, bytes + 4, 4, write);
    }
    base = dev & (dev & (1U << 12) ? 0x3ff : 0xfff);
    if (slot < 2) {
        base &= ~1ULL;
    }
    base <<= 20;
    as = dev & (1U << 12) ? &s->pci_mem_as : &s->pci_io_as;
    if (!(dev & (1U << 13))) {
        offset ^= width - size;
        if (write) {
            for (i = 0; i < size / 2; i++) {
                uint8_t v = bytes[i];
                bytes[i] = bytes[size - 1 - i];
                bytes[size - 1 - i] = v;
            }
        }
    }
    result = address_space_rw(as, base + offset, MEMTXATTRS_UNSPECIFIED,
                              bytes, size, write);
    if (!(dev & (1U << 13)) && !write) {
        for (i = 0; i < size / 2; i++) {
            uint8_t v = bytes[i];
            bytes[i] = bytes[size - 1 - i];
            bytes[size - 1 - i] = v;
        }
    }
    return result;
}

static uint64_t xbridge_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIXBridgeHeadState *s = opaque;
    uint8_t bytes[8] = { 0 };

    if (addr >= 0x20000 && addr < 0x29000 && size <= 4) {
        uint32_t cfg = xbridge_config_address(s, addr, size);
        PCIBus *bus = PCI_HOST_BRIDGE(s)->bus;

        if (!pci_find_device(bus, cfg >> 16, (cfg >> 8) & 0xff)) {
            REG(s, 0x170) |= 1U << 11;
        }
        return pci_data_read(PCI_HOST_BRIDGE(s)->bus, cfg, size);
    }
    if (addr >= 0x200000 && addr < 0xc00000) {
        xbridge_pio(s, addr, bytes, size, false);
        return ldn_be_p(bytes, size);
    }
    if (size == 4 && (addr & 7) == 4) {
        if (addr >= 0x10000 && addr < 0x11000) {
            return s->ate[(addr - 0x10000) / 8] >> 32;
        }
        if (addr >= 0x11000 && addr < 0x12000) {
            return (uint32_t)s->ate[(addr - 0x11000) / 8];
        }
        if (addr < 0x300) {
            return REG(s, addr);
        }
    }
    xbridge_invalid(s, addr);
    return 0;
}

static void xbridge_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    SGIXBridgeHeadState *s = opaque;
    unsigned off = addr & ~7;
    uint32_t mask;
    uint8_t bytes[8];

    if (addr >= 0x20000 && addr < 0x29000 && size <= 4) {
        pci_data_write(PCI_HOST_BRIDGE(s)->bus,
                       xbridge_config_address(s, addr, size), value, size);
        return;
    }
    if (addr >= 0x200000 && addr < 0xc00000) {
        stn_be_p(bytes, size, value);
        xbridge_pio(s, addr, bytes, size, true);
        return;
    }
    if (addr >= 0x11000 && addr < 0x12000) {
        return;
    }
    if (size == 8 && !(addr & 7) &&
        addr >= 0x10000 && addr < 0x11000) {
        s->ate[(addr - 0x10000) / 8] = value & 0x0000ffffffffffffULL;
        return;
    }
    if (size != 4 || (addr & 7) != 4 || addr >= 0x300) {
        xbridge_invalid(s, addr);
        return;
    }
    if (off == 0x110 && (value & (1U << 1))) {
        REG(s, 0x100) &= ~(1U << 30);
        REG(s, 0x170) &= ~(1U << 30);
    }
    if (off >= 0x180 && off <= 0x1f8) {
        unsigned pin = ((off - 0x180) / 8) & 7;

        if (!(REG(s, 0x108) & (1U << pin))) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-xbridge: masked force semantics unresolved\n");
        } else if (off < 0x1c0 || (s->pins & (1U << pin))) {
            xbridge_packet(s, pin, true);
        }
        return;
    }
    switch (off) {
    case 0x20:
        mask = 0x0fe1f20f;
        value = (value & mask) | ((uint32_t)s->speed << 4);
        break;
    case 0x30:
        value &= 0xfffff;
        break;
    case 0x38:
        break;
    case 0x100:
        return;
    case 0x118:
        value &= 0xff;
        break;
    case 0x28:
        value &= 0xfffff;
        break;
    case 0x80:
        value &= 0xf3ffff;
        break;
    case 0xc8:
        value &= 0x00fff800;
        break;
    case 0x110:
        if (value & (1U << 6)) {
            REG(s, 0x170) = 0;
        }
        return;
    case 0x108:
        value &= 0x7ffefeff;
        break;
    default:
        if (off >= 0x130 && off <= 0x168) {
            value &= 0x3ffff;
            break;
        }
        if (off >= 0x200 && off <= 0x238) {
            value &= 0x1eefffff;
            break;
        }
        qemu_log_mask(LOG_UNIMP,
                      "sgi-xbridge: unimplemented write at 0x%x\n", off);
        return;
    }
    REG(s, off) = value;
}

static const MemoryRegionOps xbridge_ops = {
    .read = xbridge_read,
    .write = xbridge_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static void xbridge_reset_hold(Object *obj, ResetType type)
{
    SGIXBridgeHeadState *s = SGI_XBRIDGE_HEAD(obj);
    unsigned i;

    s->pins = 0;
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->ate, 0, sizeof(s->ate));
    REG(s, 0) = 0x2d002049;
    REG(s, 8) = 0x20;
    REG(s, 0x20) = 0x0f00320f | ((uint32_t)s->speed << 4);
    REG(s, 0x28) = 0xfffff;
    REG(s, 0x48) = 0x03ff1810;
    REG(s, 0x80) = 0x20000;
    for (i = 0; i < 8; i++) {
        REG(s, 0x200 + i * 8) = 0x18001000;
    }
}

static void xbridge_realize(DeviceState *dev, Error **errp)
{
    SGIXBridgeHeadState *s = SGI_XBRIDGE_HEAD(dev);

    unsigned i;

    if (s->speed > 3) {
        error_setg(errp, "XBridge PCI speed encoding must be at most 3");
        return;
    }
    for (i = 0; i < 16; i++) {
        struct XBridgeTarget *endpoint = &s->endpoint[i];
        g_autofree char *name = g_strdup_printf("xbridge.target-%x", i);

        if (!s->target[i]) {
            continue;
        }
        address_space_init(&endpoint->as, s->target[i], name);
        memory_region_init_io(&endpoint->swap, OBJECT(s), &xbridge_swap_ops,
                              endpoint, name, UINT64_MAX);
        endpoint->swap.disable_reentrancy_guard = true;
        address_space_init(&endpoint->swap_as, &endpoint->swap, name);
    }
    memory_region_init(&s->denied, OBJECT(s), "xbridge.denied", UINT64_MAX);
    address_space_init(&s->denied_as, &s->denied, "xbridge.denied");
    memory_region_init_io(&s->flush, OBJECT(s), &xbridge_flush_ops, s,
                          "xbridge.flush", 0x10000);
    s->flush.disable_reentrancy_guard = true;
    address_space_init(&s->flush_as, &s->flush, "xbridge.flush");
    for (i = 0; i < 8; i++) {
        g_autofree char *name = g_strdup_printf("xbridge.dma-%u", i);

        s->dma[i].head = s;
        s->dma[i].slot = i;
        memory_region_init_iommu(&s->dma[i].iommu, sizeof(s->dma[i].iommu),
                                 TYPE_XBRIDGE_IOMMU, OBJECT(s), name,
                                 UINT64_MAX);
        address_space_init(&s->dma[i].as, MEMORY_REGION(&s->dma[i].iommu),
                           name);
    }
    qdev_init_gpio_in(dev, xbridge_set_pin, 8);
    memory_region_init(&s->pci_mem, OBJECT(s), "xbridge.pci-mem", UINT64_MAX);
    memory_region_init(&s->pci_io, OBJECT(s), "xbridge.pci-io", UINT64_MAX);
    address_space_init(&s->pci_mem_as, &s->pci_mem, "xbridge.pci-mem");
    address_space_init(&s->pci_io_as, &s->pci_io, "xbridge.pci-io");
    PCI_HOST_BRIDGE(s)->bus = pci_register_root_bus(dev, "head.0", NULL, NULL,
        s, &s->pci_mem, &s->pci_io, 0, 0, TYPE_PCI_BUS);
    pci_setup_iommu(PCI_HOST_BRIDGE(s)->bus, &xbridge_iommu_ops, s);
    pci_bus_set_slot_reserved_mask(PCI_HOST_BRIDGE(s)->bus, ~0xffU);
    memory_region_init_io(&s->widget, OBJECT(s), &xbridge_ops, s,
                          TYPE_SGI_XBRIDGE_HEAD, 0x1000000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->widget);
}

static void xbridge_unrealize(DeviceState *dev)
{
    SGIXBridgeHeadState *s = SGI_XBRIDGE_HEAD(dev);

    unsigned i;

    for (i = 0; i < 16; i++) {
        if (s->target[i]) {
            address_space_destroy(&s->endpoint[i].as);
            address_space_destroy(&s->endpoint[i].swap_as);
        }
    }
    for (i = 0; i < 8; i++) {
        address_space_destroy(&s->dma[i].as);
    }
    address_space_destroy(&s->denied_as);
    address_space_destroy(&s->flush_as);
    address_space_destroy(&s->pci_mem_as);
    address_space_destroy(&s->pci_io_as);
}

static const VMStateDescription vmstate_xbridge = {
    .name = TYPE_SGI_XBRIDGE_HEAD,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(pins, SGIXBridgeHeadState),
        VMSTATE_UINT32_ARRAY(reg, SGIXBridgeHeadState, REG_COUNT),
        VMSTATE_UINT64_ARRAY(ate, SGIXBridgeHeadState, ATE_COUNT),
        VMSTATE_END_OF_LIST()
    }
};

static const Property xbridge_properties[] = {
    DEFINE_PROP_UINT8("pci-speed", SGIXBridgeHeadState, speed, 0),
};

static void xbridge_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI XBridge PCI head";
    dc->user_creatable = true;
    dc->realize = xbridge_realize;
    dc->unrealize = xbridge_unrealize;
    dc->vmsd = &vmstate_xbridge;
    rc->phases.hold = xbridge_reset_hold;
    device_class_set_props(dc, xbridge_properties);
}

static void xbridge_init(Object *obj)
{
    SGIXBridgeHeadState *s = SGI_XBRIDGE_HEAD(obj);
    unsigned i;

    for (i = 0; i < 16; i++) {
        g_autofree char *name = g_strdup_printf("target-%x", i);

        object_property_add_link(obj, name, TYPE_MEMORY_REGION,
                                 (Object **)&s->target[i],
                                 qdev_prop_allow_set_link_before_realize, 0);
    }
}

static const TypeInfo xbridge_type = {
    .name = TYPE_SGI_XBRIDGE_HEAD,
    .parent = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(SGIXBridgeHeadState),
    .class_init = xbridge_class_init,
    .instance_init = xbridge_init,
};

static void xbridge_register_types(void)
{
    static const TypeInfo iommu_type = {
        .name = TYPE_XBRIDGE_IOMMU,
        .parent = TYPE_IOMMU_MEMORY_REGION,
        .class_init = xbridge_iommu_class_init,
    };

    type_register_static(&iommu_type);
    type_register_static(&xbridge_type);
}

type_init(xbridge_register_types)
