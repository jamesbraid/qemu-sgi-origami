/* SPDX-License-Identifier: BSD-3-Clause */
/* Test-only widget-local RAM with a separate CPU observation alias. */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sgi-xio-port.h"
#include "hw/misc/sgi-bedrock-io.h"
#include "hw/misc/sgi-bedrock.h"
#include "hw/pci-host/sgi-pic.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/mips/sgi/xio-link.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/qapi-types-sgi-machines.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "system/qtest.h"

#define TYPE_SGI_XIO_TEST_TARGET "sgi-xio-test-target"
#define PIC_RUNTIME_PROFILE \
    SGI_MODEL_PIC_COMPATIBILITY_PROFILE_PIC_REV3_BEDROCK_EXPERIMENTAL
OBJECT_DECLARE_SIMPLE_TYPE(SGIXioTestTargetState, SGI_XIO_TEST_TARGET)

struct SGIXioTestTargetState {
    SysBusDevice parent_obj;
    MemoryRegion ram;
    MemoryRegion dma_observer;
    MemoryRegion alias;
    MemoryRegion overlap;
    MemoryRegion sparse_dma;
    MemoryRegion sparse_node0;
    MemoryRegion sparse_node1;
    MemoryRegion sparse_node1_ram;
    MemoryRegion sparse_node1_alias;
    MemoryRegion pic_widget_alias;
    char *memory_kind;
    void *backing;
    uint64_t mmio_value;
    uint64_t mmio_reads;
    uint64_t mmio_writes;
    uint64_t mmio_size;
    uint64_t overlap_calls;
    uint32_t overlap_offset;
    uint64_t size;
    Object *target_port;
    SGIBedrockIOState *target_peer;
    uint32_t target_index;
    uint64_t packet_window_base;
    uint8_t source_widget;
    uint8_t source_device;
    MemoryRegion source_window;
    MemoryRegion source_status;
    AddressSpace target_packet_as;
    bool target_packet_as_initialized;
    bool sparse_node_memory;
    MemTxResult result;
    bool destination_callback;
    bool dma_observer_enabled;
    bool dma_observer_fail_read;
    bool dma_observer_fail_write;
    bool dma_observer_nested;
    bool dma_observer_nested_active;
    uint64_t dma_observer_calls;
    uint64_t dma_observer_status;
    uint64_t dma_observer_after_nested_status;
    uint32_t dma_observer_nested_offset;
    char *dma_observer_device;
    uint32_t dma_observer_status_offset;
    uint64_t destination_calls;
    hwaddr destination_address;
    SGIXioSource destination_source;
    AddressSpace ram_as;
    bool ram_as_initialized;
    DeviceState *pic;
    SGIXioLink pic_link;
    bool pic_peer;
    bool pic_linked;
    bool pic_bedrock_interrupts;
    bool pic_runtime_profile;
    bool pic_v5_migration_source;
    bool pic_v6_migration_source;
    bool pic_switched;
    bool pic_test_irq_route;
    bool packet_failure;
    bool nested_dma;
    bool nested_active;
    uint32_t nested_dma_head;
    uint64_t packet_partial;
    uint64_t nested_dma_address;
    uint32_t packet_result;
    uint64_t packet_calls;
    uint64_t packet_size;
    hwaddr packet_address;
    SGIXioSource packet_source;
    uint64_t packet_widget;
    uint64_t packet_device_known;
    uint64_t packet_result_value;
};

/*
 * Read the watched card's register from inside its own transfer. The card is
 * mid-access, so its reentrancy guard would refuse the nested read; this
 * test-only observer lifts the guard for the one read and restores it. That
 * is safe only because the register read has no side effects: it cannot
 * start DMA or re-enter the card. Point dma-observer-status-offset at
 * nothing else. The fixture builds only for tests.
 */
static uint64_t dma_observer_card_read(PCIDevice *card, unsigned offset)
{
    MemoryRegion *mr = card->io_regions[0].memory;
    MemReentrancyGuard *guard = &DEVICE(card)->mem_reentrancy_guard;
    bool engaged = guard->engaged_in_io;
    uint64_t value = 0;

    if (mr->ops->read) {
        return mr->ops->read(mr->opaque, offset, 4);
    }
    guard->engaged_in_io = false;
    mr->ops->read_with_attrs(mr->opaque, offset, &value, 4,
                             MEMTXATTRS_UNSPECIFIED);
    guard->engaged_in_io = engaged;
    return value;
}

static MemTxResult dma_observer_access(SGIXioTestTargetState *s,
                                       hwaddr address, void *buffer,
                                       size_t size, bool write)
{
    Object *object;
    PCIDevice *card;
    uint64_t status = 0;
    unsigned status_offset = s->dma_observer_status_offset;

    /* The watched card's status register, read from inside the transfer. */
    object = object_resolve_path_type(s->dma_observer_device ?:
                                      "/machine/peripheral/ioc4",
                                      TYPE_PCI_DEVICE, NULL);
    if (!object) {
        return MEMTX_DECODE_ERROR;
    }
    card = PCI_DEVICE(object);
    status = dma_observer_card_read(card, status_offset);
    s->dma_observer_calls++;
    s->dma_observer_status = status;
    if (write && s->dma_observer_nested && !s->dma_observer_nested_active) {
        s->dma_observer_nested_active = true;
        card->io_regions[0].memory->ops->write(
            card->io_regions[0].memory->opaque,
            s->dma_observer_nested_offset, 8, 4);
        s->dma_observer_nested_active = false;
        s->dma_observer_after_nested_status =
            dma_observer_card_read(card, status_offset);
    }
    if ((write && s->dma_observer_fail_write) ||
        (!write && s->dma_observer_fail_read)) {
        return MEMTX_ACCESS_ERROR;
    }
    if (address > s->size || size > s->size - address) {
        return MEMTX_DECODE_ERROR;
    }
    return address_space_rw(&s->ram_as, address, MEMTXATTRS_UNSPECIFIED,
                            buffer, size, write);
}

static MemTxResult dma_observer_read(void *opaque, hwaddr address,
                                    uint64_t *value, unsigned size,
                                    MemTxAttrs attrs)
{
    uint8_t bytes[8] = { 0 };
    uint8_t *data = bytes + sizeof(bytes) - size;
    MemTxResult result = dma_observer_access(opaque, address, data, size,
                                              false);

    *value = ldq_be_p(bytes);
    return result;
}

static MemTxResult dma_observer_write(void *opaque, hwaddr address,
                                     uint64_t value, unsigned size,
                                     MemTxAttrs attrs)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    return dma_observer_access(opaque, address, bytes + 8 - size, size,
                               true);
}

static const MemoryRegionOps dma_observer_ops = {
    .read_with_attrs = dma_observer_read,
    .write_with_attrs = dma_observer_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

static MemTxResult target_packet_rw(Object *port, unsigned int index,
                                    hwaddr address, void *buffer, size_t size,
                                    bool write, const SGIXioSource *source,
                                    SGIXioPacketAttrs packet_attrs)
{
    SGIXioTestTargetState *s = SGI_XIO_TEST_TARGET(port);
    MemTxResult result;

    if (index || !s->pic_peer) {
        return MEMTX_DECODE_ERROR;
    }
    s->packet_calls++;
    s->packet_size = size;
    s->packet_address = address;
    s->packet_source = *source;
    s->packet_widget = source->widget;
    s->packet_device_known = source->device_known;
    if (s->nested_dma && !s->nested_active && s->pic && write) {
        PCIDevice *card = pci_find_device(sgi_pic_pci_bus(SGI_PIC(s->pic),
                                                         s->nested_dma_head),
                                          0, PCI_DEVFN(1, 0));
        uint8_t nested[4] = { 0xd1, 0xd2, 0xd3, 0xd4 };

        if (card) {
            s->nested_active = true;
            pci_dma_write(card, s->nested_dma_address, nested,
                          sizeof(nested));
            s->nested_active = false;
        }
    }
    if (s->target_port) {
        if (s->pic_bedrock_interrupts && (address & (UINT64_C(1) << 47))) {
            SGIXioPortClass *pc = SGI_XIO_PORT_GET_CLASS(s->target_port);

            /* Drop the XIO carrier and retain Bedrock's lower-48 address. */
            address &= (UINT64_C(1) << 48) - 1;
            if (!pc->packet_rw) {
                result = MEMTX_ACCESS_ERROR;
            } else {
                result = pc->packet_rw(s->target_port, s->target_index,
                                       address, buffer, size, write, source,
                                       packet_attrs);
            }
        } else if (sgi_xio_port_switched(s->target_port,
                                         s->target_index)) {
            if (!s->target_packet_as_initialized) {
                result = MEMTX_ACCESS_ERROR;
            } else {
                MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;

                /* Keep the observed packet source available to endpoints. */
                attrs.requester_id = (source->widget << 3) |
                    (source->device_known ? source->device : 0);
                result = address_space_rw(&s->target_packet_as, address,
                                          attrs, buffer, size, write);
            }
        } else {
            if (s->pic_switched) {
                address &= SGI_XIO_PACKET_SIZE - 1;
            }
            result = sgi_xio_port_packet_rw(s->target_port, s->target_index,
                                            address, buffer, size, write,
                                            source, packet_attrs);
        }
    } else {
        hwaddr offset;

        if (address < s->size) {
            offset = address;
        } else if (s->packet_window_base &&
                   address >= s->packet_window_base &&
                   address - s->packet_window_base < s->size) {
            offset = address - s->packet_window_base;
        } else {
            result = MEMTX_DECODE_ERROR;
            goto out;
        }
        if (size > s->size - offset) {
            result = MEMTX_DECODE_ERROR;
            goto out;
        }
        if (write && s->packet_failure) {
            size_t partial = MIN(size, s->packet_partial);

            if (partial) {
                result = address_space_write(&s->ram_as, offset,
                    MEMTXATTRS_UNSPECIFIED, buffer, partial);
                if (result != MEMTX_OK) {
                    s->packet_result_value = result;
                    return result;
                }
            }
            result = s->packet_result;
        } else {
            result = write ? address_space_write(&s->ram_as, offset,
                        MEMTXATTRS_UNSPECIFIED, buffer, size) :
                             address_space_read(&s->ram_as, offset,
                        MEMTXATTRS_UNSPECIFIED, buffer, size);
        }
    }
out:
    s->packet_result_value = result;
    return result;
}

static const char *target_xio_describe(Object *port, unsigned int index)
{
    return "an XIO test target";
}

static MemoryRegion *target_xio_face(Object *port, unsigned int index)
{
    SGIXioTestTargetState *s = SGI_XIO_TEST_TARGET(port);

    return index == 0 && s->pic_peer ? &s->alias : NULL;
}

static bool target_xio_switched(Object *port, unsigned int index)
{
    SGIXioTestTargetState *s = SGI_XIO_TEST_TARGET(port);

    return index == 0 && s->pic_switched;
}

static bool target_xio_connect(Object *port, unsigned int index,
                               const SGIXioPeer *peer, Error **errp)
{
    if (index || !SGI_XIO_TEST_TARGET(port)->pic_peer || !peer->packet_face) {
        error_setg(errp, "XIO test target accepts one packet peer");
        return false;
    }
    return true;
}

static void target_unrealize(DeviceState *dev)
{
    SGIXioTestTargetState *s = SGI_XIO_TEST_TARGET(dev);

    if (s->ram_as_initialized) {
        address_space_destroy(&s->ram_as);
        s->ram_as_initialized = false;
    }
    if (s->target_packet_as_initialized) {
        address_space_destroy(&s->target_packet_as);
        s->target_packet_as_initialized = false;
    }
}

static MemTxResult source_access(SGIXioTestTargetState *s, hwaddr address,
                                  void *buffer, unsigned size, bool write)
{
    SGIXioSource source = {
        .widget_known = s->source_widget != UINT8_MAX,
        .device_known = s->source_device != UINT8_MAX,
        .widget = s->source_widget,
        .device = s->source_device,
    };

    s->result = sgi_xio_port_packet_rw(s->target_port, s->target_index,
                                       address, buffer, size, write, &source,
                                       (SGIXioPacketAttrs) { 0 });
    return s->result;
}

static MemTxResult source_read(void *opaque, hwaddr address, uint64_t *value,
                                unsigned size, MemTxAttrs attrs)
{
    uint8_t bytes[8] = { 0 };
    MemTxResult result = source_access(opaque, address, bytes + 8 - size,
                                        size, false);

    *value = ldq_be_p(bytes);
    return result;
}

static MemTxResult source_write(void *opaque, hwaddr address, uint64_t value,
                                 unsigned size, MemTxAttrs attrs)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    return source_access(opaque, address, bytes + 8 - size, size, true);
}

static uint64_t source_status_read(void *opaque, hwaddr address, unsigned size)
{
    SGIXioTestTargetState *s = opaque;

    switch (address) {
    case 0: return s->result;
    case 8: return s->destination_calls;
    case 16: return s->destination_address;
    case 24: return s->destination_source.widget;
    case 32: return s->destination_source.device;
    case 40: return s->destination_source.device_known;
    case 48: return s->mmio_reads;
    case 56: return s->mmio_writes;
    case 64: return s->mmio_size;
    case 72: return s->overlap_calls;
    default: return 0;
    }
}

static const MemoryRegionOps source_ops = {
    .read_with_attrs = source_read,
    .write_with_attrs = source_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static const MemoryRegionOps source_status_ops = {
    .read = source_status_read,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static SGIBedrockIOState *source_claimant(void *opaque, unsigned number)
{
    SGIXioTestTargetState *s = opaque;

    return s->target_peer && s->target_peer->node_number == number ?
           s->target_peer : NULL;
}

static MemTxResult source_destination(void *opaque,
    const SGIXioSource *source, hwaddr address, void *buffer, size_t size,
    bool write, SGIXioPacketAttrs packet_attrs)
{
    SGIXioTestTargetState *s = opaque;

    s->destination_calls++;
    s->destination_address = address;
    s->destination_source = *source;
    return MEMTX_ACCESS_ERROR;
}

/* Wide MMIO deliberately overlaps a one-byte higher-priority region. */
static uint64_t target_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIXioTestTargetState *s = opaque;

    s->mmio_reads++;
    s->mmio_size = size;
    return extract64(s->mmio_value, (addr & 7) * 8, size * 8);
}

static void target_mmio_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    SGIXioTestTargetState *s = opaque;

    s->mmio_writes++;
    s->mmio_size = size;
    s->mmio_value = deposit64(s->mmio_value, (addr & 7) * 8, size * 8, value);
}

static uint64_t target_overlap_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIXioTestTargetState *s = opaque;

    s->overlap_calls++;
    return 0xfe;
}

static void target_overlap_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    SGIXioTestTargetState *s = opaque;

    s->overlap_calls++;
}

static const MemoryRegionOps target_mmio_ops = {
    .read = target_mmio_read,
    .write = target_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static const MemoryRegionOps target_overlap_ops = {
    .read = target_overlap_read,
    .write = target_overlap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
    .impl = { .min_access_size = 1, .max_access_size = 1 },
};

static void target_realize(DeviceState *dev, Error **errp)
{
    SGIXioTestTargetState *s = SGI_XIO_TEST_TARGET(dev);
    g_autofree char *name = g_strdup_printf("%s.ram",
        object_get_canonical_path_component(OBJECT(dev)));

    if (s->pic_runtime_profile && !qtest_enabled()) {
        error_setg(errp, "PIC runtime profile is available only in qtest");
        return;
    }
    if (s->pic_v5_migration_source &&
        (!qtest_enabled() || !s->pic_runtime_profile)) {
        error_setg(errp, "PIC version-5 source is available only for "
                   "runtime-profile qtests");
        return;
    }
    if (s->pic_v6_migration_source &&
        (!qtest_enabled() || !s->pic_runtime_profile)) {
        error_setg(errp, "PIC version-6 source is available only for "
                   "runtime-profile qtests");
        return;
    }
    if (s->pic_test_irq_route && !s->pic_peer) {
        error_setg(errp, "PIC test IRQ route requires PIC peer mode");
        return;
    }
    if (s->dma_observer_enabled && !qtest_enabled()) {
        error_setg(errp, "DMA observer is available only in qtest");
        return;
    }
    if (s->nested_dma_head > 1) {
        error_setg(errp, "nested PIC DMA head must be 0 or 1");
        return;
    }
    if (!s->size || s->size > 16 * MiB) {
        error_setg(errp, "XIO test RAM size must be 1 byte to 16 MiB");
        return;
    }
    if (s->packet_window_base &&
        (s->target_port || s->packet_window_base < s->size ||
         s->packet_window_base >= SGI_XIO_PACKET_SIZE ||
         s->size > SGI_XIO_PACKET_SIZE - s->packet_window_base)) {
        error_setg(errp, "invalid XIO test packet window base");
        return;
    }
    if (s->target_peer || s->destination_callback) {
        SGIBedrockIOState *ii = (SGIBedrockIOState *)object_dynamic_cast(
            s->target_port, TYPE_SGI_BEDROCK_IO);

        if (!ii) {
            error_setg(errp, "test peer requires a Bedrock II target port");
            return;
        }
        if (s->destination_callback) {
            sgi_bedrock_io_set_destination(ii, source_destination, s);
        } else {
            sgi_bedrock_io_set_claimant(ii, source_claimant, s);
        }
    }
    if (!s->memory_kind) {
        s->memory_kind = g_strdup("ram");
    }
    s->mmio_value = 0x0123456789abcdefULL;
    if (!strcmp(s->memory_kind, "overlap-mmio")) {
        memory_region_init_io(&s->ram, OBJECT(s), &target_mmio_ops, s,
                              name, s->size);
        memory_region_init_io(&s->overlap, OBJECT(s), &target_overlap_ops, s,
                              "overlap", 1);
        memory_region_add_subregion_overlap(&s->ram, s->overlap_offset,
                                            &s->overlap, 1);
    } else if (!strcmp(s->memory_kind, "ram-device")) {
        s->backing = g_malloc0(s->size);
        memory_region_init_ram_device_ptr(&s->ram, OBJECT(s), name, s->size,
                                          s->backing);
    } else if (!strcmp(s->memory_kind, "rom-device")) {
        if (!memory_region_init_rom_device(&s->ram, OBJECT(s), &target_mmio_ops,
                                           s, name, s->size, errp)) {
            return;
        }
    } else if (!strcmp(s->memory_kind, "ram") ||
               !strcmp(s->memory_kind, "readonly")) {
        if (!memory_region_init_ram(&s->ram, OBJECT(s), name, s->size, errp)) {
            return;
        }
        memory_region_set_readonly(&s->ram,
                                   !strcmp(s->memory_kind, "readonly"));
    } else {
        error_setg(errp, "unknown XIO test memory kind '%s'", s->memory_kind);
        return;
    }
    if (strcmp(s->memory_kind, "ram") &&
        strcmp(s->memory_kind, "overlap-mmio")) {
        uint8_t *bytes = memory_region_get_ram_ptr(&s->ram);

        for (uint64_t i = 0; i < s->size; i++) {
            bytes[i] = i + 0x40;
        }
    }
    if (s->sparse_node_memory) {
        g_autofree char *node1_name = g_strdup_printf("%s.node1.ram",
            object_get_canonical_path_component(OBJECT(dev)));

        if (!memory_region_init_ram(&s->sparse_node1_ram, OBJECT(s),
                                    node1_name, s->size, errp)) {
            return;
        }
        memory_region_init(&s->sparse_dma, OBJECT(s), "sparse-dma",
                           UINT64_C(1) << 34);
        memory_region_init_alias(&s->sparse_node0, OBJECT(s),
                                 "sparse-node0", &s->ram, 0, s->size);
        memory_region_init_alias(&s->sparse_node1, OBJECT(s),
                                 "sparse-node1", &s->sparse_node1_ram,
                                 0, s->size);
        memory_region_add_subregion(&s->sparse_dma, 0, &s->sparse_node0);
        memory_region_add_subregion(&s->sparse_dma, UINT64_C(1) << 33,
                                    &s->sparse_node1);
        memory_region_init_alias(&s->sparse_node1_alias, OBJECT(s),
                                 "sparse-node1-cpu", &s->sparse_node1_ram,
                                 0, s->size);
    }
    address_space_init(&s->ram_as, &s->ram, "xio-test-target.ram");
    s->ram_as_initialized = true;
    if (s->dma_observer_enabled) {
        memory_region_init_io(&s->dma_observer, OBJECT(s),
                              &dma_observer_ops, s, "dma-observer", s->size);
        s->dma_observer.disable_reentrancy_guard = true;
    }
    memory_region_init_alias(&s->alias, OBJECT(s), "cpu-alias", &s->ram,
                             0, s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->alias);
    if (s->sparse_node_memory) {
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->sparse_node1_alias);
    }
    object_property_add_uint64_ptr(OBJECT(s), "x-test-packet-calls",
                                   &s->packet_calls, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), "x-test-packet-size",
                                   &s->packet_size, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), "x-test-packet-address",
                                   &s->packet_address, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), "x-test-packet-widget",
                                   &s->packet_widget, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), "x-test-packet-device-known",
                                   &s->packet_device_known,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), "x-test-packet-result",
                                   &s->packet_result_value,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), "x-test-dma-observer-calls",
                                   &s->dma_observer_calls,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), "x-test-dma-observer-status",
                                   &s->dma_observer_status,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s),
                                   "x-test-dma-observer-after-nested-status",
                                   &s->dma_observer_after_nested_status,
                                   OBJ_PROP_FLAG_READ);
    if (s->target_port) {
        if (sgi_xio_port_switched(s->target_port, s->target_index)) {
            MemoryRegion *face = sgi_xio_port_packet_face(s->target_port,
                                                           s->target_index);

            if (!face) {
                error_setg(errp, "switched XIO target has no packet face");
                return;
            }
            address_space_init(&s->target_packet_as, face,
                               "xio-test-target.packet");
            s->target_packet_as_initialized = true;
        }
        memory_region_init_io(&s->source_window, OBJECT(s), &source_ops, s,
                              "explicit-source", 16 * SGI_XIO_PACKET_SIZE);
        memory_region_init_io(&s->source_status, OBJECT(s), &source_status_ops,
                              s, "source-status", 80);
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->source_window);
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->source_status);
    } else if (strcmp(s->memory_kind, "ram")) {
        memory_region_init_io(&s->source_status, OBJECT(s), &source_status_ops,
                              s, "memory-status", 80);
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->source_status);
    }
    if (s->pic_peer) {
        Error *local_err = NULL;

        if (s->destination_callback) {
            error_setg(errp, "PIC peer mode cannot use a destination callback");
            return;
        }
        s->pic = qdev_new(s->pic_v6_migration_source ? "sgi-pic-v6-test" :
                          s->pic_v5_migration_source ?
                          TYPE_SGI_PIC_V5_TEST : TYPE_SGI_PIC);
        object_property_add_child(OBJECT(s), "pic", OBJECT(s->pic));
        if (s->pic_runtime_profile) {
            qdev_prop_set_enum(s->pic, "compatibility-profile",
                               PIC_RUNTIME_PROFILE);
            qdev_prop_set_bit(s->pic, "test-fixture", false);
            qdev_prop_set_bit(s->pic, "linked-transport", false);
            qdev_prop_set_bit(s->pic, "bedrock-interrupts", false);
        } else {
            qdev_prop_set_bit(s->pic, "test-fixture", true);
            qdev_prop_set_bit(s->pic, "linked-transport", s->pic_linked);
            qdev_prop_set_bit(s->pic, "bedrock-interrupts",
                              s->pic_bedrock_interrupts);
        }
        if (s->pic_linked || s->pic_runtime_profile) {
            sgi_xio_link_init(&s->pic_link, OBJECT(s), "pic-peer",
                              OBJECT(s->pic), 0, OBJECT(s), 0);
            if (!sgi_xio_link_connect(&s->pic_link, &local_err)) {
                error_propagate(errp, local_err);
                return;
            }
        } else if (!object_property_set_link(OBJECT(s->pic), "target-0",
                                             OBJECT(&s->ram), &local_err)) {
            error_propagate(errp, local_err);
            return;
        }
        if (!sysbus_realize(SYS_BUS_DEVICE(s->pic), &local_err)) {
            error_propagate(errp, local_err);
            return;
        }
        if (s->pic_runtime_profile && s->target_port) {
            SGIBedrockIOState *ii = SGI_BEDROCK_IO(object_dynamic_cast(
                s->target_port, TYPE_SGI_BEDROCK_IO));

            if (ii && ii->bedrock) {
                SGIBedrockState *bedrock = SGI_BEDROCK(ii->bedrock);

                sgi_pic_set_bedrock_pi(SGI_PIC(s->pic), 0,
                                       &bedrock->pi[0], 0);
                sgi_pic_set_bedrock_pi(SGI_PIC(s->pic), 1,
                                       &bedrock->pi[1], 0);
            }
        }
        if (s->pic_test_irq_route) {
            DeviceState *head = sgi_pic_head_device(SGI_PIC(s->pic), 0);

            qdev_connect_gpio_out_named(head, "pci-intx", 4,
                                        qdev_get_gpio_in(head, 0));
        }
        memory_region_init_alias(&s->pic_widget_alias, OBJECT(s),
                                 "pic-widget", sgi_pic_widget(SGI_PIC(s->pic)),
                                 0, memory_region_size(sgi_pic_widget(
                                     SGI_PIC(s->pic))));
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->pic_widget_alias);
    }
}

static const Property target_properties[] = {
    DEFINE_PROP_STRING("memory-kind", SGIXioTestTargetState, memory_kind),
    DEFINE_PROP_UINT32("overlap-offset", SGIXioTestTargetState,
                       overlap_offset, 0x8004),
    DEFINE_PROP_BOOL("destination-callback", SGIXioTestTargetState,
                      destination_callback, false),
    DEFINE_PROP_BOOL("dma-observer", SGIXioTestTargetState,
                     dma_observer_enabled, false),
    DEFINE_PROP_BOOL("dma-observer-fail-read", SGIXioTestTargetState,
                     dma_observer_fail_read, false),
    DEFINE_PROP_BOOL("dma-observer-fail-write", SGIXioTestTargetState,
                     dma_observer_fail_write, false),
    DEFINE_PROP_BOOL("dma-observer-nested", SGIXioTestTargetState,
                     dma_observer_nested, false),
    DEFINE_PROP_UINT32("dma-observer-nested-offset", SGIXioTestTargetState,
                       dma_observer_nested_offset, 0x330),
    DEFINE_PROP_STRING("dma-observer-device", SGIXioTestTargetState,
                       dma_observer_device),
    DEFINE_PROP_UINT32("dma-observer-status-offset", SGIXioTestTargetState,
                       dma_observer_status_offset, 0x20),
    DEFINE_PROP_BOOL("pic-peer", SGIXioTestTargetState, pic_peer, false),
    DEFINE_PROP_BOOL("pic-linked", SGIXioTestTargetState, pic_linked, true),
    DEFINE_PROP_BOOL("pic-bedrock-interrupts", SGIXioTestTargetState,
                      pic_bedrock_interrupts, false),
    DEFINE_PROP_BOOL("pic-runtime-profile", SGIXioTestTargetState,
                     pic_runtime_profile, false),
    DEFINE_PROP_BOOL("pic-v5-migration-source", SGIXioTestTargetState,
                     pic_v5_migration_source, false),
    DEFINE_PROP_BOOL("pic-v6-migration-source", SGIXioTestTargetState,
                     pic_v6_migration_source, false),
    DEFINE_PROP_BOOL("pic-switched", SGIXioTestTargetState, pic_switched,
                     false),
    DEFINE_PROP_BOOL("pic-test-irq-route", SGIXioTestTargetState,
                     pic_test_irq_route, false),
    DEFINE_PROP_BOOL("packet-failure", SGIXioTestTargetState,
                     packet_failure, false),
    DEFINE_PROP_BOOL("nested-dma", SGIXioTestTargetState, nested_dma, false),
    DEFINE_PROP_UINT32("nested-dma-head", SGIXioTestTargetState,
                       nested_dma_head, 0),
    DEFINE_PROP_UINT64("nested-dma-address", SGIXioTestTargetState,
                       nested_dma_address, 0x80003000),
    DEFINE_PROP_SIZE("packet-partial", SGIXioTestTargetState,
                     packet_partial, 0),
    DEFINE_PROP_UINT32("packet-result", SGIXioTestTargetState,
                       packet_result, MEMTX_ACCESS_ERROR),
    DEFINE_PROP_SIZE("size", SGIXioTestTargetState, size, 64 * KiB),
    DEFINE_PROP_BOOL("sparse-node-memory", SGIXioTestTargetState,
                     sparse_node_memory, false),
    DEFINE_PROP_UINT64("packet-window-base", SGIXioTestTargetState,
                       packet_window_base, 0),
    DEFINE_PROP_LINK("target-port", SGIXioTestTargetState, target_port,
                     TYPE_SGI_XIO_PORT, Object *),
    DEFINE_PROP_LINK("target-peer", SGIXioTestTargetState, target_peer,
                     TYPE_SGI_BEDROCK_IO, SGIBedrockIOState *),
    DEFINE_PROP_UINT32("target-index", SGIXioTestTargetState, target_index, 0),
    DEFINE_PROP_UINT8("source-widget", SGIXioTestTargetState, source_widget,
                      UINT8_MAX),
    DEFINE_PROP_UINT8("source-device", SGIXioTestTargetState, source_device,
                      UINT8_MAX),
};

static const VMStateDescription vmstate_target = {
    .name = TYPE_SGI_XIO_TEST_TARGET,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void target_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = target_realize;
    dc->unrealize = target_unrealize;
    dc->user_creatable = true;
    dc->vmsd = &vmstate_target;
    device_class_set_props(dc, target_properties);
    {
        SGIXioPortClass *pc = SGI_XIO_PORT_CLASS(oc);

        pc->describe = target_xio_describe;
        pc->cpu_face = target_xio_face;
        pc->packet_face = target_xio_face;
        pc->packet_rw = target_packet_rw;
        pc->switched = target_xio_switched;
        pc->connect = target_xio_connect;
    }
}

static void target_finalize(Object *obj)
{
    SGIXioTestTargetState *s = SGI_XIO_TEST_TARGET(obj);

    g_free(s->backing);
}

static const TypeInfo target_type = {
    .name = TYPE_SGI_XIO_TEST_TARGET,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIXioTestTargetState),
    .class_init = target_class_init,
    .instance_finalize = target_finalize,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_SGI_XIO_PORT },
        { }
    },
};

static void target_register_types(void)
{
    type_register_static(&target_type);
}

type_init(target_register_types)
