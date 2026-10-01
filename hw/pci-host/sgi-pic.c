/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * PIC revision 3 compatibility model for an explicitly selected IP53 profile.
 * Reset, DMA, and interrupt effects are emulator rules, not measured silicon.
 * Translated DMA, PCI-X errors, and the PIC's own PCI function are unsupported.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/pci-host/sgi-pic.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define TYPE_SGI_PIC_HEAD "sgi-pic-head"
OBJECT_DECLARE_SIMPLE_TYPE(SGIPICHead, SGI_PIC_HEAD)

#define PIC_HEAD_OFFSET 0x800000
#define PIC_FACE_SIZE 0x800000
#define PIC_WIDGET_SIZE 0x100000000ULL
#define PIC_REG_COUNT (0x640 / 8)
#define PIC_REQ_COUNT 256
#define PIC_PIN_COUNT 8
#define PIC_INTERNAL_SLOTS 4
#define PIC_ACCESS_LOG_LIMIT 32
#define TYPE_SGI_PIC_IOMMU "sgi-pic-iommu"
#define TYPE_SGI_PIC_FIXTURE "sgi-pic-fixture"
OBJECT_DECLARE_SIMPLE_TYPE(SGIPICFixture, SGI_PIC_FIXTURE)

struct SGIPICFixture {
    PCIDevice parent_obj;
    uint16_t device_id;
    MemoryRegion bar;
};

static void pic_fixture_realize(PCIDevice *dev, Error **errp)
{
    SGIPICFixture *s = SGI_PIC_FIXTURE(dev);

    pci_config_set_vendor_id(dev->config, 0xfeed);
    pci_config_set_device_id(dev->config, s->device_id);
    memory_region_init(&s->bar, OBJECT(s), "pic.fixture-bar", 0x1000);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);
}

static const VMStateDescription vmstate_pic_fixture = {
    .name = TYPE_SGI_PIC_FIXTURE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SGIPICFixture),
        VMSTATE_END_OF_LIST()
    }
};

static const Property pic_fixture_properties[] = {
    DEFINE_PROP_UINT16("fixture-id", SGIPICFixture, device_id, 0x3502),
};

static void pic_fixture_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    pc->realize = pic_fixture_realize;
    pc->vendor_id = 0xfeed;
    pc->device_id = 0x3502;
    pc->class_id = PCI_CLASS_BRIDGE_HOST;
    dc->vmsd = &vmstate_pic_fixture;
    dc->user_creatable = false;
    device_class_set_props(dc, pic_fixture_properties);
}

static const TypeInfo pic_fixture_info = {
    .name = TYPE_SGI_PIC_FIXTURE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(SGIPICFixture),
    .class_init = pic_fixture_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

typedef struct SGIPICDMA {
    IOMMUMemoryRegion iommu;
    AddressSpace as;
    SGIPICHead *head;
    unsigned slot;
} SGIPICDMA;

typedef struct SGIPICTarget {
    AddressSpace as;
    MemoryRegion swap;
    AddressSpace swap_as;
} SGIPICTarget;

struct SGIPICHead {
    PCIHostState parent_obj;
    MemoryRegion face;
    MemoryRegion pci_mem;
    MemoryRegion pci_io;
    AddressSpace pci_mem_as;
    uint64_t reg[PIC_REG_COUNT];
    uint64_t req[3][PIC_REQ_COUNT];
    uint8_t pins;
    uint8_t head;
    uint32_t unsupported_logs;
    uint32_t set_requests[PIC_PIN_COUNT];
    uint32_t clear_requests[PIC_PIN_COUNT];
    qemu_irq pci_irq[32];
    SGIPICState *pic;
    SGIPICDMA dma[PIC_INTERNAL_SLOTS + 1];
    MemoryRegion denied;
    AddressSpace denied_as;
};

struct SGIPICState {
    SysBusDevice parent_obj;
    SGIPICHead head[2];
    MemoryRegion widget;
    MemoryRegion pci_range[2];
    MemoryRegion *target[16];
    SGIPICTarget endpoint[16];
    bool test_fixture;
};

#define REG(s, off) ((s)->reg[(off) / 8])

static MemTxResult pic_swap_read(void *opaque, hwaddr addr, uint64_t *value,
                                 unsigned size, MemTxAttrs attrs)
{
    SGIPICTarget *target = opaque;
    uint8_t byte;
    MemTxResult result;

    assert(size == 1);
    result = address_space_read(&target->as, addr ^ 3, attrs, &byte, 1);
    *value = byte;
    return result;
}

static MemTxResult pic_swap_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size, MemTxAttrs attrs)
{
    SGIPICTarget *target = opaque;
    uint8_t byte = value;

    assert(size == 1);
    return address_space_write(&target->as, addr ^ 3, attrs, &byte, 1);
}

static const MemoryRegionOps pic_swap_ops = {
    .read_with_attrs = pic_swap_read,
    .write_with_attrs = pic_swap_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static IOMMUTLBEntry pic_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                   IOMMUAccessFlags flag, int iommu_idx)
{
    SGIPICDMA *dma = container_of(iommu, SGIPICDMA, iommu);
    SGIPICHead *s = dma->head;
    IOMMUTLBEntry entry = {
        .iova = addr & ~0xfffULL,
        .addr_mask = 0xfff,
        .perm = IOMMU_NONE,
    };
    uint64_t direct = REG(s, 0x80);
    unsigned widget = (direct >> 20) & 15;
    uint64_t destination = ((direct & 0x1ffff) << 31) |
                           (addr & 0x7fffffff);

    if (addr < 0x80000000 || addr > UINT32_MAX) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-pic: head %u unsupported DMA 0x%" HWADDR_PRIx
                      " from slot %u\n", s->head, addr, dma->slot);
        return entry;
    }
    if (!s->pic->target[widget] ||
        destination >= memory_region_size(s->pic->target[widget])) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pic: head %u invalid DMA target %x address 0x%"
                      PRIx64 " from slot %u\n",
                      s->head, widget, destination, dma->slot);
        return entry;
    }
    entry.target_as = REG(s, 0x200 + (dma->slot - 1) * 8) & (1U << 19) ?
                      &s->pic->endpoint[widget].swap_as :
                      &s->pic->endpoint[widget].as;
    entry.translated_addr = destination & ~entry.addr_mask;
    entry.perm = IOMMU_RW;
    return entry;
}

static AddressSpace *pic_dma_as(PCIBus *bus, void *opaque, int devfn)
{
    SGIPICHead *s = opaque;
    unsigned slot = PCI_SLOT(devfn);

    if (!slot) {
        return &s->denied_as;
    }
    if (slot > PIC_INTERNAL_SLOTS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pic: head %u unsupported requester devfn %x\n",
                      s->head, devfn);
        return &s->denied_as;
    }
    return &s->dma[slot].as;
}

static const PCIIOMMUOps pic_iommu_ops = {
    .get_address_space = pic_dma_as,
};

static int pic_notify_flags(IOMMUMemoryRegion *iommu,
                            IOMMUNotifierFlag old_flags,
                            IOMMUNotifierFlag new_flags, Error **errp)
{
    if (new_flags != IOMMU_NOTIFIER_NONE) {
        error_setg(errp, "PIC does not support cached DMA mappings");
        return -EINVAL;
    }
    return 0;
}

static void pic_iommu_class_init(ObjectClass *klass, const void *data)
{
    IOMMUMemoryRegionClass *ic = IOMMU_MEMORY_REGION_CLASS(klass);

    ic->translate = pic_translate;
    ic->notify_flag_changed = pic_notify_flags;
}

static const TypeInfo pic_iommu_info = {
    .name = TYPE_SGI_PIC_IOMMU,
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .class_init = pic_iommu_class_init,
};

static void pic_unsupported(SGIPICHead *s, hwaddr addr, unsigned size,
                            uint64_t value, bool write)
{
    const char *origin = addr >= 0x40000000 ? "pci-memory-window" :
                         "widget-mmio";

    if (s->unsupported_logs >= PIC_ACCESS_LOG_LIMIT) {
        return;
    }
    s->unsupported_logs++;
    qemu_log_mask(LOG_UNIMP,
                  "sgi-pic: head %u origin=%s %s offset 0x%" HWADDR_PRIx
                  " size %u value 0x%" PRIx64 " unsupported\n",
                  s->head, origin, write ? "write" : "read", addr,
                  size, value);
    if (s->unsupported_logs == PIC_ACCESS_LOG_LIMIT) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-pic: head %u further unsupported accesses"
                      " suppressed\n", s->head);
    }
}

static bool pic_defined(hwaddr addr)
{
    switch (addr) {
    case 0x00:
    case 0x50:
    case 0x80:
    case 0xc8:
    case 0x100:
    case 0x108:
    case 0x110:
    case 0x118:
    case 0x120:
    case 0x130 ... 0x168:
    case 0x200 ... 0x218:
        return true;
    default:
        return false;
    }
}

static void pic_packet(SGIPICHead *s, unsigned pin, bool set)
{
    uint64_t destination = REG(s, 0x130 + pin * 8);
    uint16_t level = destination >> 48;
    unsigned widget = (destination >> 44) & 15;
    hwaddr offset = destination & ((1ULL << 44) - 1);
    MemTxResult result;

    if (set) {
        s->set_requests[pin]++;
    } else {
        s->clear_requests[pin]++;
    }
    qemu_log_mask(LOG_UNIMP,
                  "sgi-pic: head %u pin %u %s request destination 0x%"
                  PRIx64 " (packet payload is a compatibility choice)\n",
                  s->head, pin, set ? "set" : "clear", destination);
    if (!s->pic->target[widget]) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pic: head %u interrupt target %x is absent\n",
                      s->head, widget);
        return;
    }
    address_space_stq_be(&s->pic->endpoint[widget].as, offset,
                         level | (set ? 0x10000 : 0),
                         MEMTXATTRS_UNSPECIFIED, &result);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pic: head %u interrupt destination 0x%"
                      PRIx64 " refused write\n", s->head, destination);
    }
}

static uint32_t pic_config_address(SGIPICHead *s, hwaddr addr, unsigned size)
{
    unsigned bus = 0;
    unsigned dev;
    unsigned fn = (addr >> 8) & 7;

    if (addr < 0x28000) {
        dev = (addr - 0x20000) >> 12;
    } else {
        bus = (REG(s, 0xc8) >> 16) & 0xff;
        dev = (REG(s, 0xc8) >> 11) & 0x1f;
    }
    return (bus << 16) | (PCI_DEVFN(dev, fn) << 8) |
           ((addr & 0xff) ^ (4 - size));
}

static uint64_t pic_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIPICHead *s = opaque;
    uint32_t cfg;

    if (addr >= 0x20000 && addr < 0x29000 && size <= 4) {
        cfg = pic_config_address(s, addr, size);
        if (!pci_find_device(PCI_HOST_BRIDGE(s)->bus, cfg >> 16,
                             (cfg >> 8) & 0xff)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-pic: head %u configuration miss at 0x%"
                          HWADDR_PRIx "\n", s->head, addr);
        }
        return pci_data_read(PCI_HOST_BRIDGE(s)->bus, cfg, size);
    }
    if (size == 8 && !(addr & 7) && pic_defined(addr) &&
        addr < sizeof(s->reg)) {
        return REG(s, addr);
    }
    if (size == 8 && !(addr & 7) && addr >= 0x18000 && addr < 0x19800) {
        unsigned bank = (addr - 0x18000) / 0x800;
        unsigned index = ((addr - 0x18000) % 0x800) / 8;

        return s->req[bank][index];
    }
    pic_unsupported(s, addr, size, 0, false);
    return 0;
}

static void pic_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    SGIPICHead *s = opaque;
    uint32_t cfg;

    if (addr >= 0x20000 && addr < 0x29000 && size <= 4) {
        cfg = pic_config_address(s, addr, size);
        if (!pci_find_device(PCI_HOST_BRIDGE(s)->bus, cfg >> 16,
                             (cfg >> 8) & 0xff)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-pic: head %u configuration write miss at 0x%"
                          HWADDR_PRIx "\n", s->head, addr);
        }
        pci_data_write(PCI_HOST_BRIDGE(s)->bus, cfg, value, size);
        return;
    }
    if (size != 8 || (addr & 7)) {
        pic_unsupported(s, addr, size, value, true);
        return;
    }
    if (addr >= 0x18000 && addr < 0x19800) {
        unsigned bank = (addr - 0x18000) / 0x800;
        unsigned index = ((addr - 0x18000) % 0x800) / 8;

        s->req[bank][index] = value;
        return;
    }
    if (!pic_defined(addr) || addr >= sizeof(s->reg)) {
        pic_unsupported(s, addr, size, value, true);
        return;
    }
    switch (addr) {
    case 0x00:
    case 0x50:
    case 0x100:
        pic_unsupported(s, addr, size, value, true);
        return;
    case 0x108: {
        uint64_t newly_enabled = value & ~REG(s, 0x108);
        unsigned pin;

        REG(s, 0x108) = value;
        for (pin = 0; pin < PIC_PIN_COUNT; pin++) {
            if ((newly_enabled & REG(s, 0x100) & s->pins) & (1U << pin)) {
                pic_packet(s, pin, true);
            }
        }
        return;
    }
    case 0x110:
        REG(s, 0x100) &= ~value;
        REG(s, 0x100) |= s->pins;
        for (unsigned pin = 0; pin < PIC_PIN_COUNT; pin++) {
            if ((value & s->pins & REG(s, 0x108)) & (1U << pin)) {
                pic_packet(s, pin, true);
            }
        }
        return;
    case 0x120:
        pic_unsupported(s, addr, size, value, true);
        REG(s, addr) = value;
        return;
    default:
        if (addr >= 0x200 && addr <= 0x218 &&
            (value & ~((1U << 19) | (1U << 20)))) {
            pic_unsupported(s, addr, size, value, true);
            value &= (1U << 19) | (1U << 20);
        }
        REG(s, addr) = value;
        return;
    }
}

static const MemoryRegionOps pic_ops = {
    .read = pic_read,
    .write = pic_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static int pic_map_irq(PCIDevice *dev, int pin)
{
    unsigned slot = PCI_SLOT(dev->devfn);

    return slot >= 1 && slot <= PIC_INTERNAL_SLOTS ?
           slot * 4 + pin : 32;
}

static void pic_pci_irq(void *opaque, int line, int level)
{
    SGIPICHead *s = opaque;

    if (line >= 0 && line < 32) {
        if (level && !qemu_irq_is_connected(s->pci_irq[line])) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-pic: head %u PCI interrupt source %d has"
                          " no board wiring\n", s->head, line);
        }
        qemu_set_irq(s->pci_irq[line], level);
    }
}

static void pic_set_pin(void *opaque, int pin, int level)
{
    SGIPICHead *s = opaque;
    unsigned bit = 1U << pin;
    bool was_high = s->pins & bit;

    s->pins = (s->pins & ~bit) | (level ? bit : 0);
    if (level && !was_high) {
        REG(s, 0x100) |= bit;
    }
    if (!was_high && level && (REG(s, 0x108) & bit)) {
        pic_packet(s, pin, true);
    }
    if (was_high && !level && (REG(s, 0x118) & bit)) {
        pic_packet(s, pin, false);
    }
}

static void pic_head_reset_hold(Object *obj, ResetType type)
{
    SGIPICHead *s = SGI_PIC_HEAD(obj);

    memset(s->reg, 0, sizeof(s->reg));
    memset(s->req, 0, sizeof(s->req));
    s->pins = 0;
    s->unsupported_logs = 0;
    memset(s->set_requests, 0, sizeof(s->set_requests));
    memset(s->clear_requests, 0, sizeof(s->clear_requests));
    /* Synthetic profile ID. The physical second-head raw ID is unknown. */
    REG(s, 0x00) = s->head ? 0x3d112024 : 0x3d102024;
}

static const VMStateDescription vmstate_pic_head = {
    .name = TYPE_SGI_PIC_HEAD,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(pins, SGIPICHead),
        VMSTATE_UINT32(unsupported_logs, SGIPICHead),
        VMSTATE_UINT32_ARRAY(set_requests, SGIPICHead, PIC_PIN_COUNT),
        VMSTATE_UINT32_ARRAY(clear_requests, SGIPICHead, PIC_PIN_COUNT),
        VMSTATE_UINT64_ARRAY(reg, SGIPICHead, PIC_REG_COUNT),
        VMSTATE_UINT64_2DARRAY(req, SGIPICHead, 3, PIC_REQ_COUNT),
        VMSTATE_END_OF_LIST()
    }
};

static void pic_head_realize(DeviceState *dev, Error **errp)
{
    SGIPICHead *s = SGI_PIC_HEAD(dev);
    g_autofree char *name = g_strdup_printf("pic-head-%u.0", s->head);
    unsigned i;

    qdev_init_gpio_in(dev, pic_set_pin, PIC_PIN_COUNT);
    qdev_init_gpio_out_named(dev, s->pci_irq, "pci-intx", 32);
    if (s->pic->test_fixture) {
        for (i = 0; i < PIC_PIN_COUNT; i++) {
            g_autofree char *set_name = g_strdup_printf(
                "x-test-set-requests-%u", i);
            g_autofree char *clear_name = g_strdup_printf(
                "x-test-clear-requests-%u", i);

            object_property_add_uint32_ptr(OBJECT(s), set_name,
                &s->set_requests[i], OBJ_PROP_FLAG_READ);
            object_property_add_uint32_ptr(OBJECT(s), clear_name,
                &s->clear_requests[i], OBJ_PROP_FLAG_READ);
        }
    }
    memory_region_init(&s->pci_mem, OBJECT(s), "pic.pci-mem", UINT64_MAX);
    memory_region_init(&s->pci_io, OBJECT(s), "pic.pci-io", UINT64_MAX);
    address_space_init(&s->pci_mem_as, &s->pci_mem, "pic.pci-mem");
    PCI_HOST_BRIDGE(s)->bus = pci_register_root_bus(dev, name,
        pic_pci_irq, pic_map_irq, s, &s->pci_mem, &s->pci_io,
        0, 33, TYPE_PCI_BUS);
    pci_bus_set_slot_reserved_mask(PCI_HOST_BRIDGE(s)->bus,
                                   s->pic->test_fixture ? ~0x1fU : ~0x1eU);
    if (s->pic->test_fixture) {
        PCIDevice *fixture = pci_new(PCI_DEVFN(0, 0), TYPE_SGI_PIC_FIXTURE);

        qdev_prop_set_uint16(DEVICE(fixture), "fixture-id", 0x3502 + s->head);
        if (!pci_realize_and_unref(fixture, PCI_HOST_BRIDGE(s)->bus, errp)) {
            return;
        }
    }
    memory_region_init(&s->denied, OBJECT(s), "pic.denied", UINT64_MAX);
    address_space_init(&s->denied_as, &s->denied, "pic.denied");
    for (i = 0; i < G_N_ELEMENTS(s->dma); i++) {
        g_autofree char *dma_name = g_strdup_printf("pic-%u.dma-%u",
                                                    s->head, i);

        s->dma[i].head = s;
        s->dma[i].slot = i;
        memory_region_init_iommu(&s->dma[i].iommu,
                                 sizeof(s->dma[i].iommu),
                                 TYPE_SGI_PIC_IOMMU, OBJECT(s), dma_name,
                                 UINT64_MAX);
        address_space_init(&s->dma[i].as, MEMORY_REGION(&s->dma[i].iommu),
                           dma_name);
    }
    pci_setup_iommu(PCI_HOST_BRIDGE(s)->bus, &pic_iommu_ops, s);
    memory_region_init_io(&s->face, OBJECT(s), &pic_ops, s,
                          "pic.face", PIC_FACE_SIZE);
}

static void pic_head_unrealize(DeviceState *dev)
{
    SGIPICHead *s = SGI_PIC_HEAD(dev);
    unsigned i;

    for (i = 0; i < G_N_ELEMENTS(s->dma); i++) {
        address_space_destroy(&s->dma[i].as);
    }
    address_space_destroy(&s->denied_as);
    address_space_destroy(&s->pci_mem_as);
}

static void pic_head_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = pic_head_realize;
    dc->unrealize = pic_head_unrealize;
    dc->vmsd = &vmstate_pic_head;
    rc->phases.hold = pic_head_reset_hold;
    dc->user_creatable = false;
}

static const TypeInfo pic_head_info = {
    .name = TYPE_SGI_PIC_HEAD,
    .parent = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(SGIPICHead),
    .class_init = pic_head_class_init,
};

static uint64_t pic_range_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIPICHead *s = opaque;
    uint8_t bytes[8] = { 0 };
    MemTxResult result;

    result = address_space_read(&s->pci_mem_as, addr,
                                MEMTXATTRS_UNSPECIFIED, bytes, size);
    if (result != MEMTX_OK) {
        pic_unsupported(s, 0x40000000 + addr, size, 0, false);
    }
    return ldn_le_p(bytes, size);
}

static void pic_range_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    SGIPICHead *s = opaque;
    uint8_t bytes[8];
    MemTxResult result;

    stn_le_p(bytes, size, value);
    result = address_space_write(&s->pci_mem_as, addr,
                                 MEMTXATTRS_UNSPECIFIED, bytes, size);
    if (result != MEMTX_OK) {
        pic_unsupported(s, 0x40000000 + addr, size, value, true);
    }
}

static const MemoryRegionOps pic_range_ops = {
    .read = pic_range_read,
    .write = pic_range_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

MemoryRegion *sgi_pic_widget(SGIPICState *s)
{
    return &s->widget;
}

PCIBus *sgi_pic_pci_bus(SGIPICState *s, unsigned head)
{
    g_assert(head < 2);
    return PCI_HOST_BRIDGE(&s->head[head])->bus;
}

DeviceState *sgi_pic_head_device(SGIPICState *s, unsigned head)
{
    g_assert(head < 2);
    return DEVICE(&s->head[head]);
}

static void pic_realize(DeviceState *dev, Error **errp)
{
    SGIPICState *s = SGI_PIC(dev);
    unsigned i;

    if (!s->test_fixture) {
        error_setg(errp, "PIC revision 3 PCI device 0 identity is unresolved; "
                   "test-fixture=on is only for synthetic register tests");
        return;
    }
    for (i = 0; i < G_N_ELEMENTS(s->target); i++) {
        SGIPICTarget *endpoint = &s->endpoint[i];
        g_autofree char *name = g_strdup_printf("pic.target-%x", i);

        if (!s->target[i]) {
            continue;
        }
        address_space_init(&endpoint->as, s->target[i], name);
        memory_region_init_io(&endpoint->swap, OBJECT(s), &pic_swap_ops,
                              endpoint, name, UINT64_MAX);
        endpoint->swap.disable_reentrancy_guard = true;
        address_space_init(&endpoint->swap_as, &endpoint->swap, name);
    }
    for (i = 0; i < 2; i++) {
        s->head[i].head = i;
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->head[i]), errp)) {
            return;
        }
        memory_region_add_subregion(&s->widget, i * PIC_HEAD_OFFSET,
                                    &s->head[i].face);
        memory_region_init_io(&s->pci_range[i], OBJECT(s), &pic_range_ops,
                              &s->head[i], "pic.pci-range", 1ULL << 30);
        memory_region_add_subregion(&s->widget,
                                    i ? 0xc0000000 : 0x40000000,
                                    &s->pci_range[i]);
    }
}

static void pic_unrealize(DeviceState *dev)
{
    SGIPICState *s = SGI_PIC(dev);
    unsigned i;

    for (i = 0; i < G_N_ELEMENTS(s->target); i++) {
        if (s->target[i]) {
            address_space_destroy(&s->endpoint[i].as);
            address_space_destroy(&s->endpoint[i].swap_as);
        }
    }
}

static void pic_init(Object *obj)
{
    SGIPICState *s = SGI_PIC(obj);
    unsigned i;

    object_initialize_child(obj, "head-0", &s->head[0], TYPE_SGI_PIC_HEAD);
    object_initialize_child(obj, "head-1", &s->head[1], TYPE_SGI_PIC_HEAD);
    for (i = 0; i < 2; i++) {
        s->head[i].pic = s;
    }
    for (i = 0; i < G_N_ELEMENTS(s->target); i++) {
        g_autofree char *name = g_strdup_printf("target-%x", i);

        object_property_add_link(obj, name, TYPE_MEMORY_REGION,
                                 (Object **)&s->target[i],
                                 qdev_prop_allow_set_link_before_realize, 0);
    }
    memory_region_init(&s->widget, obj, "pic.widget", PIC_WIDGET_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->widget);
}

static void pic_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    static const Property properties[] = {
        DEFINE_PROP_BOOL("test-fixture", SGIPICState, test_fixture, false),
    };

    dc->realize = pic_realize;
    dc->unrealize = pic_unrealize;
    dc->user_creatable = true;
    device_class_set_props(dc, properties);
}

static const TypeInfo pic_info = {
    .name = TYPE_SGI_PIC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIPICState),
    .instance_init = pic_init,
    .class_init = pic_class_init,
};

static void pic_register_types(void)
{
    type_register_static(&pic_fixture_info);
    type_register_static(&pic_iommu_info);
    type_register_static(&pic_head_info);
    type_register_static(&pic_info);
}
type_init(pic_register_types)
