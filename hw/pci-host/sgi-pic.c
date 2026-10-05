/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * PIC revision 3 compatibility model for an explicitly selected IP53 profile.
 * Reset, DMA, and interrupt effects are emulator rules, not measured silicon.
 * Translated DMA, PCI-X errors, and the PIC's own PCI function are unsupported.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "hw/pci-host/sgi-pic.h"
#include "hw/misc/sgi-xbow.h"
#include "hw/mips/sgi-xio-port.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "qapi/error.h"
#include "qapi/qapi-types-sgi-sn.h"
#include "qapi/qapi-visit-sgi-sn.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "system/qtest.h"

#define TYPE_SGI_PIC_HEAD "sgi-pic-head"
OBJECT_DECLARE_SIMPLE_TYPE(SGIPICHead, SGI_PIC_HEAD)

#define PIC_HEAD_OFFSET 0x800000
#define PIC_FACE_SIZE 0x800000
#define PIC_CPU_FACE_SIZE (2 * PIC_HEAD_OFFSET + 8)
#define PIC_WIDGET_SIZE 0x100000000ULL
#define PIC_REG_COUNT (0x640 / 8)
#define PIC_REQ_COUNT 256
#define PIC_PIN_COUNT 8
#define PIC_INTERNAL_SLOTS 4
#define PIC_ACCESS_LOG_LIMIT 32
#define PIC_SHORT_DEVICE_BASE 0x200
#define TYPE_SGI_PIC_IOMMU "sgi-pic-iommu"
#define TYPE_SGI_PIC_FIXTURE "sgi-pic-fixture"
#define TYPE_SGI_PIC_V1_TEST "sgi-pic-v1-test"
#define TYPE_SGI_PIC_V6_TEST "sgi-pic-v6-test"
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
    MemoryRegion linked;
    AddressSpace linked_as;
    MemoryRegion linked_swap;
    AddressSpace linked_swap_as;
    MemoryRegion direct64;
    AddressSpace direct64_as;
    SGIPICHead *head;
    unsigned slot;
    bool linked_initialized;
    bool direct64_initialized;
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
    AddressSpace pci_io_as;
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
    MemoryRegion cpu_widget;
    MemoryRegion cpu_face;
    SGIXbowState pxbow;
    MemoryRegion pxbow_widget_zero;
    MemoryRegion pxbow_routed_widget;
    MemoryRegion pci_range[2];
    MemoryRegion cpu_pci_range[2];
    MemoryRegion *target[16];
    SGIPICTarget endpoint[16];
    bool test_fixture;
    bool linked;
    bool bedrock_interrupts;
    bool pxbow_enabled;
    bool pxbow_initialized;
    bool pxbow_status_source_field;
    uint8_t pxbow_host_widget;
    uint8_t pxbow_pic_widget;
    uint8_t pxbow_revision;
    SGIModelPicCompatibilityProfile compatibility_profile;
    bool peer_connected;
    bool widget_as_initialized;
    bool migration_linked;
    bool migration_bedrock_interrupts;
    bool migration_pxbow_enabled;
    bool migration_pxbow_status_source_field;
    bool migration_cpu_pio_lanes;
    bool migration_direct64_policy;
    uint32_t migration_direct64_attribute_policy;
    uint8_t migration_pxbow_host_widget;
    uint8_t migration_pxbow_pic_widget;
    uint8_t migration_pxbow_revision;
    uint32_t migration_compatibility_profile;
    uint64_t linked_packet_count;
    uint64_t last_linked_result;
    SGIXioPeer peer;
    AddressSpace widget_as;
};

static bool pic_uses_runtime_profile(const SGIPICState *s)
{
    return s->compatibility_profile ==
        SGI_MODEL_PIC_COMPATIBILITY_PROFILE_PIC_REV3_BEDROCK_EXPERIMENTAL;
}

static bool pic_uses_linked_transport(const SGIPICState *s)
{
    return s->linked || pic_uses_runtime_profile(s);
}

static bool pic_uses_bedrock_interrupts(const SGIPICState *s)
{
    return pic_uses_linked_transport(s) &&
        (s->bedrock_interrupts || pic_uses_runtime_profile(s));
}

static void pic_profile_get(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    SGIPICState *s = SGI_PIC(obj);

    visit_type_SGIModelPicCompatibilityProfile(v, name,
                                                &s->compatibility_profile,
                                                errp);
}

static void pic_profile_set(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    SGIPICState *s = SGI_PIC(obj);

    visit_type_SGIModelPicCompatibilityProfile(v, name,
                                                &s->compatibility_profile,
                                                errp);
}

static const PropertyInfo qdev_prop_sgi_pic_profile = {
    .type = "SGIModelPicCompatibilityProfile",
    .description = "PIC compatibility profile",
    .enum_table = &SGIModelPicCompatibilityProfile_lookup,
    .get = pic_profile_get,
    .set = pic_profile_set,
};

#define REG(s, off) ((s)->reg[(off) / 8])

static uint32_t pic_widget_identity(const SGIPICHead *s)
{
    uint32_t part = s->head ? 0xd112 : 0xd102;

    return (3U << 28) | (part << 12) | (0x24U << 1);
}

static MemTxResult pic_send_typed(SGIPICState *pic, hwaddr address,
                                  void *buffer, size_t size, bool write,
                                  const SGIXioSource *source)
{
    MemTxResult result = sgi_xio_peer_packet_rw(&pic->peer, address, buffer,
                                                size, write, source);

    pic->linked_packet_count++;
    pic->last_linked_result = result;
    return result;
}

static MemTxResult pic_send_interrupt(SGIPICState *pic, hwaddr address,
                                      void *buffer, size_t size,
                                      const SGIXioSource *source)
{
    SGIXioPortClass *pc;
    MemTxResult result;

    if (!source || !source->widget_known || source->widget > 15 ||
        (source->device_known && source->device > 7) ||
        !pic->peer.port) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pic: linked interrupt has no valid source or peer\n");
        result = MEMTX_ACCESS_ERROR;
        goto out;
    }
    pc = SGI_XIO_PORT_GET_CLASS(pic->peer.port);
    if (!pc->packet_rw) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pic: linked interrupt peer has no packet receiver\n");
        result = MEMTX_ACCESS_ERROR;
        goto out;
    }
    if (address >= (UINT64_C(1) << 48) ||
        size > (UINT64_C(1) << 48) - address) {
        result = MEMTX_DECODE_ERROR;
        goto out;
    }

    /* Interrupt destinations carry a full SN1 address, not a widget window. */
    result = pc->packet_rw(pic->peer.port, pic->peer.index, address,
                           buffer, size, true, source);
out:
    pic->linked_packet_count++;
    pic->last_linked_result = result;
    return result;
}

static MemTxResult pic_linked_rw(void *opaque, hwaddr addr, void *buffer,
                                 size_t size, bool write)
{
    SGIPICDMA *dma = opaque;
    SGIPICHead *head = dma->head;
    SGIPICState *pic = head->pic;
    uint64_t direct = REG(head, 0x80);
    SGIXioSource source = {
        .widget_known = true,
        .device_known = false,
        .widget = REG(head, 0x20) & 0xf,
    };
    hwaddr packet = ((direct >> 20) & 0xf) * SGI_XIO_PACKET_SIZE + addr;

    return pic_send_typed(pic, packet, buffer, size, write, &source);
}

static MemTxResult pic_linked_read(void *opaque, hwaddr addr, uint64_t *value,
                                   unsigned size, MemTxAttrs attrs)
{
    uint8_t bytes[8] = { 0 };
    MemTxResult result = pic_linked_rw(opaque, addr, bytes + 8 - size,
                                       size, false);

    *value = ldq_be_p(bytes);
    return result;
}

static MemTxResult pic_linked_write(void *opaque, hwaddr addr, uint64_t value,
                                    unsigned size, MemTxAttrs attrs)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    return pic_linked_rw(opaque, addr, bytes + 8 - size, size, true);
}

static MemTxResult pic_linked_swap_read(void *opaque, hwaddr addr,
                                        uint64_t *value, unsigned size,
                                        MemTxAttrs attrs)
{
    uint8_t byte;
    MemTxResult result = pic_linked_rw(opaque, addr ^ 3, &byte, 1, false);

    *value = byte;
    return result;
}

static MemTxResult pic_linked_swap_write(void *opaque, hwaddr addr,
                                         uint64_t value, unsigned size,
                                         MemTxAttrs attrs)
{
    uint8_t byte = value;

    return pic_linked_rw(opaque, addr ^ 3, &byte, 1, true);
}

static bool pic_direct64_valid(SGIPICDMA *dma, hwaddr address, size_t size)
{
    const uint64_t unsupported = (UINT64_C(1) << 54) |
                                (UINT64_C(0x3f) << 48);
    SGIPICHead *head = dma->head;
    uint64_t payload = address & ((UINT64_C(1) << 48) - 1);
    const char *reason = NULL;

    if (!pic_uses_runtime_profile(head->pic)) {
        reason = "profile";
    } else if (!(address >> 60)) {
        reason = "target-zero";
    } else if (address & unsupported) {
        reason = "reserved-bits";
    } else if (payload & (UINT64_C(1) << 47)) {
        reason = "register-space";
    } else if (size > (UINT64_C(1) << 47) - payload) {
        reason = "range";
    }
    if (reason) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pic: head %u direct64 %s address 0x%016"
                      PRIx64 " from slot %u\n",
                      head->head, reason, (uint64_t)address, dma->slot);
        return false;
    }
    return true;
}

static MemTxResult pic_direct64_rw(SGIPICDMA *dma, hwaddr address,
                                   void *buffer, size_t size, bool write)
{
    SGIPICHead *head = dma->head;
    SGIPICState *pic = head->pic;
    uint64_t payload = address & ((UINT64_C(1) << 48) - 1);
    uint8_t *bytes = buffer;
    SGIXioSource source = {
        .widget_known = true,
        .device_known = false,
        .widget = REG(head, 0x20) & 0xf,
    };
    unsigned widget = address >> 60;
    size_t i;

    if (!pic_direct64_valid(dma, address, size)) {
        return MEMTX_DECODE_ERROR;
    }
    if (address & (UINT64_C(1) << 55)) {
        return pic_send_typed(pic, (hwaddr)widget * SGI_XIO_PACKET_SIZE +
                              payload, buffer, size, write, &source);
    }

    for (i = 0; i < size; i++) {
        hwaddr packet = (hwaddr)widget * SGI_XIO_PACKET_SIZE +
                        ((payload + i) ^ 3);
        MemTxResult result = pic_send_typed(pic, packet, bytes + i, 1,
                                            write, &source);

        if (result != MEMTX_OK) {
            return result;
        }
    }
    return MEMTX_OK;
}

static MemTxResult pic_direct64_read(void *opaque, hwaddr addr,
                                     uint64_t *value, unsigned size,
                                     MemTxAttrs attrs)
{
    uint8_t bytes[8] = { 0 };
    MemTxResult result = pic_direct64_rw(opaque, addr, bytes + 8 - size,
                                         size, false);

    *value = ldq_be_p(bytes);
    return result;
}

static MemTxResult pic_direct64_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size,
                                      MemTxAttrs attrs)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    return pic_direct64_rw(opaque, addr, bytes + 8 - size, size, true);
}

static const MemoryRegionOps pic_direct64_ops = {
    .read_with_attrs = pic_direct64_read,
    .write_with_attrs = pic_direct64_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.unaligned = true,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static const MemoryRegionOps pic_linked_ops = {
    .read_with_attrs = pic_linked_read,
    .write_with_attrs = pic_linked_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static const MemoryRegionOps pic_linked_swap_ops = {
    .read_with_attrs = pic_linked_swap_read,
    .write_with_attrs = pic_linked_swap_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

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

    if (addr > UINT32_MAX && pic_uses_runtime_profile(s->pic)) {
        if (!pic_direct64_valid(dma, addr, 1)) {
            return entry;
        }
        entry.target_as = &dma->direct64_as;
        entry.translated_addr = addr & ~entry.addr_mask;
        entry.perm = IOMMU_RW;
        return entry;
    }
    if (addr < 0x80000000 || addr > UINT32_MAX) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-pic: head %u unsupported DMA 0x%" HWADDR_PRIx
                      " from slot %u\n", s->head, addr, dma->slot);
        return entry;
    }
    if (!pic_uses_linked_transport(s->pic) &&
        (!s->pic->target[widget] ||
         destination >= memory_region_size(s->pic->target[widget]))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pic: head %u invalid DMA target %x address 0x%"
                      PRIx64 " from slot %u\n",
                      s->head, widget, destination, dma->slot);
        return entry;
    }
    if (pic_uses_linked_transport(s->pic)) {
        entry.target_as = REG(s, 0x200 + (dma->slot - 1) * 8) & (1U << 19) ?
                          &dma->linked_swap_as : &dma->linked_as;
    } else {
        entry.target_as = REG(s, 0x200 + (dma->slot - 1) * 8) & (1U << 19) ?
                          &s->pic->endpoint[widget].swap_as :
                          &s->pic->endpoint[widget].as;
    }
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
    case 0x20:
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

static bool pic_short_window(hwaddr addr, unsigned *window,
                             hwaddr *offset, hwaddr *window_size)
{
    if (addr >= 0x200000 && addr < 0x400000) {
        *window = 0;
        *offset = addr - 0x200000;
        *window_size = 0x200000;
    } else if (addr >= 0x400000 && addr < 0x600000) {
        *window = 1;
        *offset = addr - 0x400000;
        *window_size = 0x200000;
    } else if (addr >= 0x600000 && addr < 0x700000) {
        *window = 2;
        *offset = addr - 0x600000;
        *window_size = 0x100000;
    } else if (addr >= 0x700000 && addr < PIC_FACE_SIZE) {
        *window = 3;
        *offset = addr - 0x700000;
        *window_size = 0x100000;
    } else {
        return false;
    }
    return true;
}

static bool pic_pio_section_covers(const MemoryRegionSection *section,
                                    hwaddr addr, unsigned size)
{
    return section->mr && section->offset_within_address_space == addr &&
           int128_get64(section->size) >= size;
}

/* Resolve one BAR access through its own aliases and visible subregions. */
static bool pic_pio_region_leaf(MemoryRegion *mr, hwaddr offset,
                                unsigned size, MemoryRegion **leaf,
                                hwaddr *leaf_offset)
{
    MemoryRegion *subregion;

    if (!mr->enabled || offset > int128_get64(mr->size) ||
        size > int128_get64(mr->size) - offset) {
        return false;
    }
    if (mr->alias) {
        return pic_pio_region_leaf(mr->alias, mr->alias_offset + offset,
                                   size, leaf, leaf_offset);
    }
    QTAILQ_FOREACH(subregion, &mr->subregions, subregions_link) {
        hwaddr child_offset;

        if (!subregion->enabled || offset < subregion->addr ||
            offset - subregion->addr >= int128_get64(subregion->size)) {
            continue;
        }
        child_offset = offset - subregion->addr;
        if (pic_pio_region_leaf(subregion, child_offset, size,
                                leaf, leaf_offset)) {
            return true;
        }
    }
    if (!mr->terminates) {
        return false;
    }
    *leaf = mr;
    *leaf_offset = offset;
    return true;
}

static PCIDevice *pic_pio_mapped_function(SGIPICHead *s, AddressSpace *as,
                                          hwaddr addr, unsigned size)
{
    MemoryRegionSection active = memory_region_find(as->root, addr, size);
    PCIBus *bus = PCI_HOST_BRIDGE(s)->bus;
    PCIDevice *result = NULL;
    unsigned devfn, bar;

    if (!pic_pio_section_covers(&active, addr, size)) {
        if (active.mr) {
            memory_region_unref(active.mr);
        }
        return NULL;
    }
    for (devfn = 0; devfn < ARRAY_SIZE(bus->devices) && !result; devfn++) {
        PCIDevice *dev = bus->devices[devfn];

        if (!dev) {
            continue;
        }
        for (bar = 0; bar < PCI_NUM_REGIONS; bar++) {
            PCIIORegion *region = &dev->io_regions[bar];
            MemoryRegion *leaf;
            hwaddr leaf_offset;
            bool is_io = region->type & PCI_BASE_ADDRESS_SPACE_IO;
            hwaddr bar_offset;

            if (!region->memory || region->addr == PCI_BAR_UNMAPPED ||
                is_io != (as == &s->pci_io_as) || addr < region->addr ||
                addr - region->addr > region->size ||
                size > region->size - (addr - region->addr)) {
                continue;
            }
            bar_offset = addr - region->addr;
            if (pic_pio_region_leaf(region->memory, bar_offset, size,
                                    &leaf, &leaf_offset) &&
                leaf == active.mr &&
                leaf_offset == active.offset_within_region) {
                result = dev;
            }
            if (result) {
                break;
            }
        }
    }
    memory_region_unref(active.mr);
    return result;
}

static bool pic_cpu_pio_native_order(SGIPICHead *s, AddressSpace *as,
                                     hwaddr addr, unsigned size)
{
    PCIDevice *dev = pic_pio_mapped_function(s, as, addr, size);

    return dev &&
        object_property_find(OBJECT(dev), SGI_PIC_BYTE_LANE_SELF_ORDERED) &&
        object_property_get_bool(OBJECT(dev),
                                 SGI_PIC_BYTE_LANE_SELF_ORDERED,
                                 &error_abort);
}

static bool pic_cpu_pio_prepare(SGIPICHead *s, AddressSpace *as,
                                hwaddr *addr, unsigned size, bool cpu_pio)
{
    if (!cpu_pio || !pic_uses_runtime_profile(s->pic) ||
        (size != 1 && size != 2 && size != 4) ||
        (*addr & (size - 1))) {
        return false;
    }
    if (pic_cpu_pio_native_order(s, as, *addr, size)) {
        return true;
    }
    if (size < 4) {
        *addr ^= 4 - size;
    }
    return false;
}

static MemTxResult pic_short_transfer(SGIPICHead *s, hwaddr addr,
                                      uint64_t *value, unsigned size,
                                      bool write, bool cpu_pio)
{
    AddressSpace *as;
    hwaddr offset, window_size, pci_addr;
    uint8_t bytes[8];
    bool native_order;
    uint64_t device;
    unsigned window;
    uint32_t base;
    MemTxResult result;

    if (!pic_short_window(addr, &window, &offset, &window_size)) {
        return MEMTX_DECODE_ERROR;
    }
    if (size > window_size - offset) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pic: head %u short window 0x%" HWADDR_PRIx
                      " rejects boundary-crossing %u-byte %s\n",
                      s->head, addr, size, write ? "write" : "read");
        return MEMTX_DECODE_ERROR;
    }

    device = REG(s, PIC_SHORT_DEVICE_BASE + window * 8);
    if (device & ((1ULL << 13) | (1ULL << 22))) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-pic: head %u short window 0x%" HWADDR_PRIx
                      " refuses unsupported Device attributes 0x%" PRIx64
                      " for %u-byte %s\n",
                      s->head, addr,
                      (uint64_t)(device & ((1ULL << 13) | (1ULL << 22))),
                      size, write ? "write" : "read");
        return MEMTX_ACCESS_ERROR;
    }

    base = device & 0xfff;
    as = device & (1ULL << 12) ? &s->pci_mem_as : &s->pci_io_as;
    if (device & (1ULL << 12)) {
        base &= 0x3ff;
    }
    if (window < 2) {
        base &= ~1U;
    }
    pci_addr = ((hwaddr)base << 20) + offset;
    native_order = pic_cpu_pio_prepare(s, as, &pci_addr, size, cpu_pio);
    if (write) {
        native_order ? stn_be_p(bytes, size, *value) :
                       stn_le_p(bytes, size, *value);
        result = address_space_write(as, pci_addr, MEMTXATTRS_UNSPECIFIED,
                                     bytes, size);
    } else {
        result = address_space_read(as, pci_addr, MEMTXATTRS_UNSPECIFIED,
                                    bytes, size);
        *value = native_order ? ldn_be_p(bytes, size) : ldn_le_p(bytes, size);
    }
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pic: head %u short window 0x%" HWADDR_PRIx
                      " PCI %s at 0x%" HWADDR_PRIx " failed for %u bytes\n",
                      s->head, addr, write ? "write" : "read", pci_addr,
                      size);
    }
    return result;
}

static uint64_t pic_short_read(SGIPICHead *s, hwaddr addr, unsigned size,
                               bool cpu_pio)
{
    uint64_t value = 0;

    if (pic_short_transfer(s, addr, &value, size, false, cpu_pio) !=
        MEMTX_OK) {
        return 0;
    }
    return value;
}

static void pic_short_write(SGIPICHead *s, hwaddr addr, uint64_t value,
                            unsigned size, bool cpu_pio)
{
    pic_short_transfer(s, addr, &value, size, true, cpu_pio);
}

static bool pic_access_valid(void *opaque, hwaddr addr, unsigned size,
                             bool is_write, MemTxAttrs attrs)
{
    unsigned window;
    hwaddr offset, window_size;

    if (pic_short_window(addr, &window, &offset, &window_size)) {
        return true;
    }
    (void)window;
    (void)offset;
    (void)window_size;
    return !(addr & (size - 1));
}

static void pic_packet(SGIPICHead *s, unsigned pin, bool set)
{
    uint64_t destination = REG(s, 0x130 + pin * 8);
    uint16_t level = destination >> 48;
    hwaddr address = destination & ((1ULL << 48) - 1);
    unsigned widget = (destination >> 44) & 15;
    hwaddr offset = destination & ((1ULL << 44) - 1);
    bool linked = pic_uses_linked_transport(s->pic);
    bool bedrock = pic_uses_bedrock_interrupts(s->pic);
    MemTxResult result;

    if (set) {
        s->set_requests[pin]++;
    } else {
        s->clear_requests[pin]++;
    }
    if (bedrock && (level < 7 || level > 127)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pic: head %u pin %u rejected Bedrock interrupt "
                      "level 0x%x\n", s->head, pin, level);
        return;
    }
    qemu_log_mask(LOG_UNIMP,
                  "sgi-pic: head %u pin %u %s request destination 0x%"
                  PRIx64 "\n", s->head, pin, set ? "set" : "clear",
                  destination);
    if (linked) {
        uint8_t packet[8];
        SGIXioSource source = {
            .widget_known = true,
            .device_known = false,
            .widget = REG(s, 0x20) & 0xf,
        };

        stq_be_p(packet, bedrock ?
                 level | (set ? 0x100 : 0) :
                 level | (set ? 0x10000 : 0));
        result = pic_send_interrupt(s->pic, address, packet, sizeof(packet),
                                    &source);
    } else if (!s->pic->target[widget]) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pic: head %u interrupt target %x is absent\n",
                      s->head, widget);
        return;
    } else {
        address_space_stq_be(&s->pic->endpoint[widget].as, offset,
                             level | (set ? (bedrock ?
                                              0x100 : 0x10000) : 0),
                             MEMTXATTRS_UNSPECIFIED, &result);
    }
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

    if (pic_short_window(addr, &(unsigned){ 0 }, &(hwaddr){ 0 },
                         &(hwaddr){ 0 })) {
        return pic_short_read(s, addr, size, false);
    }

    if (addr == 0 && size == 8) {
        return pic_widget_identity(s);
    }
    if (addr == 4 && size == 4) {
        return pic_widget_identity(s);
    }
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

    if (pic_short_window(addr, &(unsigned){ 0 }, &(hwaddr){ 0 },
                         &(hwaddr){ 0 })) {
        pic_short_write(s, addr, value, size, false);
        return;
    }

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
    case 0x20:
        if (value & ~0xfULL) {
            pic_unsupported(s, addr, size, value, true);
            value &= 0xf;
        }
        REG(s, addr) = value;
        return;
    case 0x80: {
        /* Experimental software mask; other hardware fields are unknown. */
        const uint64_t supported = 0x1ffff | (0xfULL << 20);

        if (pic_uses_runtime_profile(s->pic) && (value & ~supported)) {
            pic_unsupported(s, addr, size, value, true);
            value &= supported;
        }
        REG(s, addr) = value;
        return;
    }
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
            (value & ~((1U << 19) | (1U << 20) | (1U << 13) |
                       (1U << 12) | (1U << 22) | 0xfff))) {
            pic_unsupported(s, addr, size, value, true);
            value &= (1U << 19) | (1U << 20) | (1U << 13) |
                     (1U << 12) | (1U << 22) | 0xfff;
        }
        REG(s, addr) = value;
        return;
    }
}

static bool pic_cpu_word_slot(SGIPICHead *s, hwaddr addr, hwaddr *slot)
{
    if ((addr & 7) != 4) {
        return false;
    }

    *slot = addr - 4;
    if (*slot >= 0x18000 && *slot < 0x19800) {
        return true;
    }
    return pic_defined(*slot) && *slot < sizeof(s->reg);
}

static uint64_t pic_cpu_word_read(SGIPICHead *s, hwaddr addr)
{
    hwaddr slot;

    if (!pic_cpu_word_slot(s, addr, &slot) || slot == 0x50) {
        pic_unsupported(s, addr, 4, 0, false);
        return 0;
    }
    if (slot == 0) {
        return pic_widget_identity(s);
    }
    if (slot >= 0x18000) {
        unsigned bank = (slot - 0x18000) / 0x800;
        unsigned index = ((slot - 0x18000) % 0x800) / 8;

        return s->req[bank][index];
    }
    return REG(s, slot);
}

static void pic_cpu_word_write(SGIPICHead *s, hwaddr addr, uint32_t value)
{
    hwaddr slot;
    uint64_t merged;

    if (!pic_cpu_word_slot(s, addr, &slot) || slot == 0 ||
        slot == 0x50 || slot == 0x100 || slot == 0x120) {
        pic_unsupported(s, addr, 4, value, true);
        return;
    }
    if (slot >= 0x18000) {
        unsigned bank = (slot - 0x18000) / 0x800;
        unsigned index = ((slot - 0x18000) % 0x800) / 8;

        s->req[bank][index] =
            (s->req[bank][index] & UINT64_C(0xffffffff00000000)) | value;
        return;
    }
    if (slot == 0x110) {
        pic_write(s, slot, value, 8);
        return;
    }
    merged = (REG(s, slot) & UINT64_C(0xffffffff00000000)) | value;
    pic_write(s, slot, merged, 8);
}

static const MemoryRegionOps pic_ops = {
    .read = pic_read,
    .write = pic_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .valid.accepts = pic_access_valid,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .impl.unaligned = true,
};

static bool pic_cpu_access_valid(void *opaque, hwaddr addr, unsigned size,
                                 bool is_write, MemTxAttrs attrs)
{
    unsigned window;
    hwaddr offset, window_size;

    if (addr < 2 * PIC_HEAD_OFFSET &&
        pic_short_window(addr % PIC_HEAD_OFFSET, &window, &offset,
                         &window_size)) {
        return true;
    }
    (void)window;
    (void)offset;
    (void)window_size;
    return !(addr & (size - 1));
}

static uint64_t pic_cpu_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIPICState *s = opaque;
    SGIPICHead *head;

    if (addr >= 2 * PIC_HEAD_OFFSET) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pic: short-window guard read at 0x%" HWADDR_PRIx
                      " refused for %u bytes\n", addr, size);
        return 0;
    }
    head = &s->head[addr / PIC_HEAD_OFFSET];
    addr %= PIC_HEAD_OFFSET;
    if (pic_uses_runtime_profile(s) &&
        pic_short_window(addr, &(unsigned){ 0 }, &(hwaddr){ 0 },
                         &(hwaddr){ 0 })) {
        return pic_short_read(head, addr, size, true);
    }
    if (pic_uses_runtime_profile(s) && addr >= 0x20000 && addr < 0x29000 &&
        size <= 4) {
        return pic_read(head, addr, size);
    }
    if (pic_uses_runtime_profile(s) && size == 4 && (addr & 7) == 4 &&
        !pic_short_window(addr, &(unsigned){ 0 }, &(hwaddr){ 0 },
                          &(hwaddr){ 0 })) {
        return pic_cpu_word_read(head, addr);
    }
    return pic_read(head, addr, size);
}

static void pic_cpu_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    SGIPICState *s = opaque;
    SGIPICHead *head;

    if (addr >= 2 * PIC_HEAD_OFFSET) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pic: short-window guard write at 0x%" HWADDR_PRIx
                      " refused for %u bytes\n", addr, size);
        return;
    }
    head = &s->head[addr / PIC_HEAD_OFFSET];
    addr %= PIC_HEAD_OFFSET;
    if (pic_uses_runtime_profile(s) &&
        pic_short_window(addr, &(unsigned){ 0 }, &(hwaddr){ 0 },
                         &(hwaddr){ 0 })) {
        pic_short_write(head, addr, value, size, true);
        return;
    }
    if (pic_uses_runtime_profile(s) && addr >= 0x20000 && addr < 0x29000 &&
        size <= 4) {
        pic_write(head, addr, value, size);
        return;
    }
    if (pic_uses_runtime_profile(s) && size == 4 && (addr & 7) == 4 &&
        !pic_short_window(addr, &(unsigned){ 0 }, &(hwaddr){ 0 },
                          &(hwaddr){ 0 })) {
        pic_cpu_word_write(head, addr, value);
        return;
    }
    pic_write(head, addr, value, size);
}

static const MemoryRegionOps pic_cpu_ops = {
    .read = pic_cpu_read,
    .write = pic_cpu_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .valid.accepts = pic_cpu_access_valid,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .impl.unaligned = true,
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
    REG(s, 0x00) = pic_widget_identity(s);
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
    const char *pic_component = object_get_canonical_path_component(
        OBJECT(s->pic));
    g_autofree char *name = NULL;
    unsigned i;

    if (!pic_component || !*pic_component) {
        error_setg(errp, "PIC head %u has no stable parent component",
                   s->head);
        return;
    }
    name = g_strdup_printf("%s-head-%u.0", pic_component, s->head);

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
    address_space_init(&s->pci_io_as, &s->pci_io, "pic.pci-io");
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
        if (pic_uses_linked_transport(s->pic)) {
            memory_region_init_io(&s->dma[i].linked, OBJECT(s),
                                  &pic_linked_ops, &s->dma[i],
                                  "pic.linked-dma", UINT64_MAX);
            s->dma[i].linked.disable_reentrancy_guard = true;
            address_space_init(&s->dma[i].linked_as, &s->dma[i].linked,
                               "pic.linked-dma");
            memory_region_init_io(&s->dma[i].linked_swap, OBJECT(s),
                                  &pic_linked_swap_ops, &s->dma[i],
                                  "pic.linked-dma-swap", UINT64_MAX);
            s->dma[i].linked_swap.disable_reentrancy_guard = true;
            address_space_init(&s->dma[i].linked_swap_as,
                               &s->dma[i].linked_swap,
                               "pic.linked-dma-swap");
            s->dma[i].linked_initialized = true;
        }
        if (pic_uses_runtime_profile(s->pic)) {
            memory_region_init_io(&s->dma[i].direct64, OBJECT(s),
                                  &pic_direct64_ops, &s->dma[i],
                                  "pic.direct64-dma", UINT64_MAX);
            s->dma[i].direct64.disable_reentrancy_guard = true;
            address_space_init(&s->dma[i].direct64_as,
                               &s->dma[i].direct64,
                               "pic.direct64-dma");
            s->dma[i].direct64_initialized = true;
        }
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
        if (s->dma[i].direct64_initialized) {
            address_space_destroy(&s->dma[i].direct64_as);
        }
        if (s->dma[i].linked_initialized) {
            address_space_destroy(&s->dma[i].linked_swap_as);
            address_space_destroy(&s->dma[i].linked_as);
        }
        address_space_destroy(&s->dma[i].as);
    }
    address_space_destroy(&s->denied_as);
    address_space_destroy(&s->pci_mem_as);
    address_space_destroy(&s->pci_io_as);
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

static uint64_t pic_range_read_common(void *opaque, hwaddr addr,
                                      unsigned size, bool cpu_pio)
{
    SGIPICHead *s = opaque;
    uint8_t bytes[8] = { 0 };
    MemTxResult result;
    bool native_order;

    native_order = pic_cpu_pio_prepare(s, &s->pci_mem_as, &addr, size,
                                       cpu_pio);
    result = address_space_read(&s->pci_mem_as, addr,
                                MEMTXATTRS_UNSPECIFIED, bytes, size);
    if (result != MEMTX_OK) {
        pic_unsupported(s, 0x40000000 + addr, size, 0, false);
    }
    return native_order ? ldn_be_p(bytes, size) : ldn_le_p(bytes, size);
}

static uint64_t pic_range_read(void *opaque, hwaddr addr, unsigned size)
{
    return pic_range_read_common(opaque, addr, size, false);
}

static uint64_t pic_cpu_range_read(void *opaque, hwaddr addr, unsigned size)
{
    return pic_range_read_common(opaque, addr, size, true);
}

static void pic_range_write_common(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size, bool cpu_pio)
{
    SGIPICHead *s = opaque;
    uint8_t bytes[8];
    MemTxResult result;
    bool native_order;

    native_order = pic_cpu_pio_prepare(s, &s->pci_mem_as, &addr, size,
                                       cpu_pio);
    native_order ? stn_be_p(bytes, size, value) :
                   stn_le_p(bytes, size, value);
    result = address_space_write(&s->pci_mem_as, addr,
                                 MEMTXATTRS_UNSPECIFIED, bytes, size);
    if (result != MEMTX_OK) {
        pic_unsupported(s, 0x40000000 + addr, size, value, true);
    }
}

static void pic_range_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    pic_range_write_common(opaque, addr, value, size, false);
}

static void pic_cpu_range_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    pic_range_write_common(opaque, addr, value, size, true);
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

static const MemoryRegionOps pic_cpu_range_ops = {
    .read = pic_cpu_range_read,
    .write = pic_cpu_range_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static const char *pic_xio_describe(Object *port, unsigned int index)
{
    return "a PIC widget";
}

static MemoryRegion *pic_xio_cpu_face(Object *port, unsigned int index)
{
    SGIPICState *s = SGI_PIC(port);

    return index == 0 ? &s->cpu_widget : NULL;
}

static MemoryRegion *pic_xio_packet_face(Object *port, unsigned int index)
{
    SGIPICState *s = SGI_PIC(port);

    return index == 0 ? &s->widget : NULL;
}

static bool pic_xio_connect(Object *port, unsigned int index,
                            const SGIXioPeer *peer, Error **errp)
{
    SGIPICState *s = SGI_PIC(port);
    SGIXioPortClass *pc;

    if (index || DEVICE(s)->realized ||
        !pic_uses_linked_transport(s) || s->peer_connected ||
        !peer->port || !peer->packet_face) {
        error_setg(errp, "PIC linked transport needs one typed peer before "
                   "realization");
        return false;
    }
    pc = SGI_XIO_PORT_GET_CLASS(peer->port);
    if (!pc->packet_rw) {
        error_setg(errp, "PIC linked transport peer has no typed receiver");
        return false;
    }
    s->peer = *peer;
    s->peer_connected = true;
    return true;
}

static MemTxResult pic_xio_packet_rw(Object *port, unsigned int index,
                                     hwaddr address, void *buffer,
                                     size_t size, bool write,
                                     const SGIXioSource *source)
{
    SGIPICState *s = SGI_PIC(port);

    if (index || !pic_uses_linked_transport(s) ||
        !s->widget_as_initialized) {
        return MEMTX_DECODE_ERROR;
    }
    return write ? address_space_write(&s->widget_as, address,
                                       MEMTXATTRS_UNSPECIFIED, buffer, size) :
                   address_space_read(&s->widget_as, address,
                                      MEMTXATTRS_UNSPECIFIED, buffer, size);
}

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

bool sgi_pic_enable_pxbow_experiment(SGIPICState *s, uint64_t span,
                                     Error **errp)
{
    uint16_t widgets;

    if (DEVICE(s)->realized || s->pxbow_enabled ||
        !pic_uses_runtime_profile(s)) {
        error_setg(errp, "PIC PXBOW composition requires an unused runtime "
                   "profile before realization");
        return false;
    }
    if (s->pxbow_host_widget < SGI_XBOW_FIRST_WIDGET ||
        s->pxbow_host_widget >= SGI_XBOW_WIDGETS ||
        s->pxbow_pic_widget < SGI_XBOW_FIRST_WIDGET ||
        s->pxbow_pic_widget >= SGI_XBOW_WIDGETS ||
        s->pxbow_host_widget == s->pxbow_pic_widget ||
        s->pxbow_revision > 0xf || span < PIC_WIDGET_SIZE ||
        span > SGI_XBOW_PACKET_SIZE) {
        error_setg(errp, "PIC PXBOW composition has invalid widgets, "
                   "revision, or XIO span");
        return false;
    }

    object_initialize_child(OBJECT(s), "pxbow", &s->pxbow, TYPE_SGI_XBOW);
    s->pxbow_initialized = true;
    if (!sgi_xbow_enable_status_source_field(&s->pxbow, errp)) {
        return false;
    }
    s->pxbow_status_source_field = true;
    widgets = BIT(s->pxbow_host_widget) | BIT(s->pxbow_pic_widget);
    qdev_prop_set_uint64(DEVICE(&s->pxbow), "span", span);
    qdev_prop_set_uint32(DEVICE(&s->pxbow), "revision", s->pxbow_revision);
    qdev_prop_set_uint32(DEVICE(&s->pxbow), "part", 0xd100);
    qdev_prop_set_uint32(DEVICE(&s->pxbow), "manufacturer", 0);
    qdev_prop_set_uint16(DEVICE(&s->pxbow), "links-present", widgets);
    qdev_prop_set_uint16(DEVICE(&s->pxbow), "startup-links-alive", widgets);
    qdev_prop_set_uint16(DEVICE(&s->pxbow), "link-reset-restore-alive",
                         widgets);
    if (!sgi_xbow_attach_endpoint(&s->pxbow, s->pxbow_pic_widget,
                                  &s->cpu_widget, &s->widget, errp)) {
        return false;
    }

    memory_region_init_alias(
        &s->pxbow_widget_zero, OBJECT(s), "pic.pxbow-widget-zero",
        sgi_xbow_packet_ingress(&s->pxbow, s->pxbow_host_widget), 0, span);
    memory_region_init_alias(
        &s->pxbow_routed_widget, OBJECT(s), "pic.pxbow-routed-widget",
        sgi_xbow_ingress(&s->pxbow, s->pxbow_host_widget),
        (uint64_t)s->pxbow_pic_widget * span, PIC_WIDGET_SIZE);
    s->pxbow_enabled = true;
    return true;
}

bool sgi_pic_attach_pxbow_packet_endpoint(SGIPICState *s,
                                          MemoryRegion *packet_face,
                                          Error **errp)
{
    if (!s->pxbow_enabled || DEVICE(s)->realized) {
        error_setg(errp, "PIC PXBOW packet endpoint must be attached before "
                   "PIC realization");
        return false;
    }
    return sgi_xbow_attach_endpoint(&s->pxbow, s->pxbow_host_widget, NULL,
                                    packet_face, errp);
}

MemoryRegion *sgi_pic_pxbow_widget_zero(SGIPICState *s)
{
    return s->pxbow_enabled ? &s->pxbow_widget_zero : NULL;
}

MemoryRegion *sgi_pic_routed_widget(SGIPICState *s)
{
    return s->pxbow_enabled ? &s->pxbow_routed_widget : NULL;
}

uint8_t sgi_pic_pxbow_revision(SGIPICState *s)
{
    g_assert(s->pxbow_initialized);
    return s->pxbow.revision;
}

static int pic_pre_save(void *opaque)
{
    SGIPICState *s = opaque;

    s->migration_linked = pic_uses_linked_transport(s);
    s->migration_bedrock_interrupts = pic_uses_bedrock_interrupts(s);
    s->migration_pxbow_enabled = s->pxbow_enabled;
    s->migration_pxbow_status_source_field =
        s->pxbow_status_source_field;
    s->migration_pxbow_host_widget = s->pxbow_host_widget;
    s->migration_pxbow_pic_widget = s->pxbow_pic_widget;
    s->migration_pxbow_revision = s->pxbow_revision;
    s->migration_compatibility_profile = s->compatibility_profile;
    s->migration_cpu_pio_lanes = pic_uses_runtime_profile(s);
    s->migration_direct64_policy = pic_uses_runtime_profile(s);
    s->migration_direct64_attribute_policy =
        pic_uses_runtime_profile(s) ? 2 : 0;
    return 0;
}

static int pic_post_load(void *opaque, int version_id)
{
    SGIPICState *s = opaque;

    if (version_id < 2) {
        /* Version 1 predates the Bedrock interrupt packet mode. */
        s->migration_bedrock_interrupts = false;
    }
    if (version_id < 3) {
        s->migration_compatibility_profile =
            SGI_MODEL_PIC_COMPATIBILITY_PROFILE_PIC_REV3_BEDROCK;
    }
    if (version_id < 4) {
        s->migration_pxbow_enabled = false;
        s->migration_pxbow_host_widget = 0;
        s->migration_pxbow_pic_widget = 0;
        s->migration_pxbow_revision = 0;
    }
    if (version_id < 5) {
        s->migration_pxbow_status_source_field = false;
    }
    if (version_id < 6) {
        s->migration_cpu_pio_lanes = false;
    }
    if (version_id < 7) {
        s->migration_direct64_policy = false;
    }
    if (version_id < 8) {
        s->migration_direct64_attribute_policy =
            s->migration_direct64_policy ? 1 : 0;
    }
    if (s->migration_linked != pic_uses_linked_transport(s)) {
        error_report("PIC migration transport mode does not match destination");
        return -EINVAL;
    }
    if (s->migration_bedrock_interrupts !=
        pic_uses_bedrock_interrupts(s)) {
        error_report("PIC migration interrupt mode does not match destination");
        return -EINVAL;
    }
    if (s->migration_compatibility_profile != s->compatibility_profile) {
        error_report("PIC migration compatibility profile does not match "
                     "destination");
        return -EINVAL;
    }
    if (s->migration_pxbow_enabled != s->pxbow_enabled ||
        (s->pxbow_enabled &&
         (s->migration_pxbow_host_widget != s->pxbow_host_widget ||
          s->migration_pxbow_pic_widget != s->pxbow_pic_widget ||
          s->migration_pxbow_revision != s->pxbow_revision))) {
        error_report("PIC PXBOW composition does not match destination");
        return -EINVAL;
    }
    if (s->migration_pxbow_status_source_field !=
        s->pxbow_status_source_field) {
        error_report("PIC PXBOW status source field does not match "
                     "destination");
        return -EINVAL;
    }
    if (s->migration_cpu_pio_lanes != pic_uses_runtime_profile(s)) {
        error_report("PIC migration CPU PIO policy does not match destination");
        return -EINVAL;
    }
    if (s->migration_direct64_policy != pic_uses_runtime_profile(s)) {
        error_report("PIC direct64 policy mismatch");
        return -EINVAL;
    }
    if (s->migration_direct64_attribute_policy !=
        (pic_uses_runtime_profile(s) ? 2 : 0)) {
        error_report("PIC direct64 attribute policy mismatch");
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_pic = {
    .name = TYPE_SGI_PIC,
    .version_id = 8,
    .minimum_version_id = 1,
    .pre_save = pic_pre_save,
    .post_load = pic_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(migration_linked, SGIPICState),
        VMSTATE_BOOL_V(migration_bedrock_interrupts, SGIPICState, 2),
        VMSTATE_UINT32_V(migration_compatibility_profile, SGIPICState, 3),
        VMSTATE_BOOL_V(migration_pxbow_enabled, SGIPICState, 4),
        VMSTATE_UINT8_V(migration_pxbow_host_widget, SGIPICState, 4),
        VMSTATE_UINT8_V(migration_pxbow_pic_widget, SGIPICState, 4),
        VMSTATE_UINT8_V(migration_pxbow_revision, SGIPICState, 4),
        VMSTATE_BOOL_V(migration_pxbow_status_source_field, SGIPICState, 5),
        VMSTATE_BOOL_V(migration_cpu_pio_lanes, SGIPICState, 6),
        VMSTATE_BOOL_V(migration_direct64_policy, SGIPICState, 7),
        VMSTATE_UINT32_V(migration_direct64_attribute_policy, SGIPICState, 8),
        VMSTATE_END_OF_LIST()
    }
};

static void pic_realize(DeviceState *dev, Error **errp)
{
    SGIPICState *s = SGI_PIC(dev);
    unsigned i;

    if (s->compatibility_profile ==
        SGI_MODEL_PIC_COMPATIBILITY_PROFILE_PIC_REV3_BEDROCK &&
        (!s->test_fixture || !qtest_enabled())) {
        error_setg(errp, "PIC revision 3 PCI device 0 identity is unresolved; "
                   "test-fixture=on is only for synthetic register tests; "
                   "pic-rev3-bedrock is only for qtest fixtures");
        return;
    }
    if (pic_uses_runtime_profile(s) && s->test_fixture) {
        error_setg(errp, "PIC runtime profile cannot enable test-fixture");
        return;
    }
    if (s->migration_bedrock_interrupts && !qtest_enabled()) {
        error_setg(errp, "PIC migration test state is available only in qtest");
        return;
    }
    if (pic_uses_linked_transport(s) && !s->peer_connected) {
        error_setg(errp, "PIC linked transport requires a typed peer before "
                   "realization");
        return;
    }
    if (s->pxbow_enabled) {
        if (!s->pxbow_initialized) {
            error_setg(errp, "PIC PXBOW composition has no Crossbow child");
            return;
        }
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->pxbow), errp)) {
            return;
        }
    }
    if (pic_uses_linked_transport(s)) {
        object_property_add_uint64_ptr(OBJECT(s),
            "x-test-linked-packet-count", &s->linked_packet_count,
            OBJ_PROP_FLAG_READ);
        object_property_add_uint64_ptr(OBJECT(s),
            "x-test-linked-last-result", &s->last_linked_result,
            OBJ_PROP_FLAG_READ);
    }
    for (i = 0; i < G_N_ELEMENTS(s->target); i++) {
        SGIPICTarget *endpoint = &s->endpoint[i];
        g_autofree char *name = g_strdup_printf("pic.target-%x", i);

        if (pic_uses_linked_transport(s) || !s->target[i]) {
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
        memory_region_init_io(&s->cpu_pci_range[i], OBJECT(s),
                              &pic_cpu_range_ops, &s->head[i],
                              "pic.cpu-pci-range", 1ULL << 30);
        memory_region_add_subregion(&s->cpu_widget,
                                    i ? 0xc0000000 : 0x40000000,
                                    &s->cpu_pci_range[i]);
    }
    if (pic_uses_linked_transport(s)) {
        address_space_init(&s->widget_as, &s->widget, "pic.widget-inbound");
        s->widget_as_initialized = true;
    }
}

static void pic_unrealize(DeviceState *dev)
{
    SGIPICState *s = SGI_PIC(dev);
    unsigned i;

    for (i = 0; !pic_uses_linked_transport(s) &&
         i < G_N_ELEMENTS(s->target); i++) {
        if (s->target[i]) {
            address_space_destroy(&s->endpoint[i].as);
            address_space_destroy(&s->endpoint[i].swap_as);
        }
    }
    if (s->widget_as_initialized) {
        address_space_destroy(&s->widget_as);
        s->widget_as_initialized = false;
    }
    if (s->pxbow_initialized && qdev_is_realized(DEVICE(&s->pxbow))) {
        qdev_unrealize(DEVICE(&s->pxbow));
    }
    if (s->peer_connected) {
        s->peer = (SGIXioPeer) { 0 };
        s->peer_connected = false;
    }
}

static void pic_init(Object *obj)
{
    SGIPICState *s = SGI_PIC(obj);
    unsigned i;

    s->pxbow_host_widget = 10;
    s->pxbow_pic_widget = 15;
    s->pxbow_revision = 5;
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
    memory_region_init(&s->cpu_widget, obj, "pic.cpu-widget",
                       PIC_WIDGET_SIZE);
    memory_region_init_io(&s->cpu_face, obj, &pic_cpu_ops, s,
                          "pic.cpu-face", PIC_CPU_FACE_SIZE);
    memory_region_add_subregion(&s->cpu_widget, 0, &s->cpu_face);
    sysbus_init_mmio(SYS_BUS_DEVICE(s),
                     sgi_xio_port_cpu_face(obj, 0));
}

static void pic_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SGIXioPortClass *pc = SGI_XIO_PORT_CLASS(klass);
    static const Property properties[] = {
        DEFINE_PROP("compatibility-profile", SGIPICState,
                    compatibility_profile, qdev_prop_sgi_pic_profile,
                    SGIModelPicCompatibilityProfile),
        DEFINE_PROP_BOOL("test-fixture", SGIPICState, test_fixture, false),
        DEFINE_PROP_BOOL("linked-transport", SGIPICState, linked, false),
        DEFINE_PROP_BOOL("bedrock-interrupts", SGIPICState,
                         bedrock_interrupts, false),
        DEFINE_PROP_BOOL("x-test-migration-bedrock-interrupts", SGIPICState,
                         migration_bedrock_interrupts, false),
        DEFINE_PROP_UINT8("pxbow-host-widget", SGIPICState,
                          pxbow_host_widget, 10),
        DEFINE_PROP_UINT8("pxbow-pic-widget", SGIPICState,
                          pxbow_pic_widget, 15),
        DEFINE_PROP_UINT8("pxbow-revision", SGIPICState,
                          pxbow_revision, 5),
    };

    dc->realize = pic_realize;
    dc->unrealize = pic_unrealize;
    dc->vmsd = &vmstate_pic;
    dc->user_creatable = true;
    device_class_set_props(dc, properties);
    pc->describe = pic_xio_describe;
    pc->cpu_face = pic_xio_cpu_face;
    pc->packet_face = pic_xio_packet_face;
    pc->packet_rw = pic_xio_packet_rw;
    pc->connect = pic_xio_connect;
}

static const TypeInfo pic_info = {
    .name = TYPE_SGI_PIC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIPICState),
    .instance_init = pic_init,
    .class_init = pic_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_SGI_XIO_PORT },
        { }
    },
};

/* Emit the pre-mode PIC VMState layout for a legacy migration test source. */
static const VMStateDescription vmstate_pic_v1_test = {
    .name = TYPE_SGI_PIC,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = pic_pre_save,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(migration_linked, SGIPICState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_pic_v5_test = {
    .name = TYPE_SGI_PIC,
    .version_id = 5,
    .minimum_version_id = 1,
    .pre_save = pic_pre_save,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(migration_linked, SGIPICState),
        VMSTATE_BOOL_V(migration_bedrock_interrupts, SGIPICState, 2),
        VMSTATE_UINT32_V(migration_compatibility_profile, SGIPICState, 3),
        VMSTATE_BOOL_V(migration_pxbow_enabled, SGIPICState, 4),
        VMSTATE_UINT8_V(migration_pxbow_host_widget, SGIPICState, 4),
        VMSTATE_UINT8_V(migration_pxbow_pic_widget, SGIPICState, 4),
        VMSTATE_UINT8_V(migration_pxbow_revision, SGIPICState, 4),
        VMSTATE_BOOL_V(migration_pxbow_status_source_field, SGIPICState, 5),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_pic_v6_test = {
    .name = TYPE_SGI_PIC,
    .version_id = 6,
    .minimum_version_id = 1,
    .pre_save = pic_pre_save,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(migration_linked, SGIPICState),
        VMSTATE_BOOL_V(migration_bedrock_interrupts, SGIPICState, 2),
        VMSTATE_UINT32_V(migration_compatibility_profile, SGIPICState, 3),
        VMSTATE_BOOL_V(migration_pxbow_enabled, SGIPICState, 4),
        VMSTATE_UINT8_V(migration_pxbow_host_widget, SGIPICState, 4),
        VMSTATE_UINT8_V(migration_pxbow_pic_widget, SGIPICState, 4),
        VMSTATE_UINT8_V(migration_pxbow_revision, SGIPICState, 4),
        VMSTATE_BOOL_V(migration_pxbow_status_source_field, SGIPICState, 5),
        VMSTATE_BOOL_V(migration_cpu_pio_lanes, SGIPICState, 6),
        VMSTATE_END_OF_LIST()
    }
};

static void pic_v1_test_realize(DeviceState *dev, Error **errp)
{
    if (!qtest_enabled()) {
        error_setg(errp, "legacy PIC migration fixture is available "
                   "only in qtest");
        return;
    }
    pic_realize(dev, errp);
}

static void pic_v1_test_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pic_v1_test_realize;
    dc->vmsd = &vmstate_pic_v1_test;
    dc->user_creatable = true;
}

static const TypeInfo pic_v1_test_info = {
    .name = TYPE_SGI_PIC_V1_TEST,
    .parent = TYPE_SGI_PIC,
    .class_init = pic_v1_test_class_init,
};

static void pic_v5_test_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pic_v1_test_realize;
    dc->vmsd = &vmstate_pic_v5_test;
    dc->user_creatable = true;
}

static const TypeInfo pic_v5_test_info = {
    .name = TYPE_SGI_PIC_V5_TEST,
    .parent = TYPE_SGI_PIC,
    .class_init = pic_v5_test_class_init,
};

static void pic_v6_test_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pic_v1_test_realize;
    dc->vmsd = &vmstate_pic_v6_test;
    dc->user_creatable = true;
}

static const TypeInfo pic_v6_test_info = {
    .name = TYPE_SGI_PIC_V6_TEST,
    .parent = TYPE_SGI_PIC,
    .class_init = pic_v6_test_class_init,
};

static void pic_register_types(void)
{
    type_register_static(&pic_fixture_info);
    type_register_static(&pic_iommu_info);
    type_register_static(&pic_head_info);
    type_register_static(&pic_info);
    type_register_static(&pic_v1_test_info);
    type_register_static(&pic_v5_test_info);
    type_register_static(&pic_v6_test_info);
}
type_init(pic_register_types)
