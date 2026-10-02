/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test-only widget-local RAM with a separate CPU observation alias. */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sgi-xio-port.h"
#include "hw/misc/sgi-bedrock-io.h"
#include "hw/pci-host/sgi-pic.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/mips/sgi/xio-link.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/units.h"

#define TYPE_SGI_XIO_TEST_TARGET "sgi-xio-test-target"
OBJECT_DECLARE_SIMPLE_TYPE(SGIXioTestTargetState, SGI_XIO_TEST_TARGET)

struct SGIXioTestTargetState {
    SysBusDevice parent_obj;
    MemoryRegion ram;
    MemoryRegion alias;
    MemoryRegion overlap;
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
    uint8_t source_widget;
    uint8_t source_device;
    MemoryRegion source_window;
    MemoryRegion source_status;
    MemTxResult result;
    bool destination_callback;
    uint64_t destination_calls;
    hwaddr destination_address;
    SGIXioSource destination_source;
    AddressSpace ram_as;
    bool ram_as_initialized;
    DeviceState *pic;
    SGIXioLink pic_link;
    bool pic_peer;
    bool pic_linked;
    bool pic_switched;
    bool packet_failure;
    bool nested_dma;
    bool nested_active;
    uint64_t packet_partial;
    uint32_t packet_result;
    uint64_t packet_calls;
    hwaddr packet_address;
    SGIXioSource packet_source;
    uint64_t packet_widget;
    uint64_t packet_device_known;
    uint64_t packet_result_value;
};

static MemTxResult target_packet_rw(Object *port, unsigned int index,
                                    hwaddr address, void *buffer, size_t size,
                                    bool write, const SGIXioSource *source)
{
    SGIXioTestTargetState *s = SGI_XIO_TEST_TARGET(port);
    MemTxResult result;

    if (index || !s->pic_peer) {
        return MEMTX_DECODE_ERROR;
    }
    s->packet_calls++;
    s->packet_address = address;
    s->packet_source = *source;
    s->packet_widget = source->widget;
    s->packet_device_known = source->device_known;
    if (s->nested_dma && !s->nested_active && s->pic && write) {
        PCIDevice *card = pci_find_device(sgi_pic_pci_bus(SGI_PIC(s->pic), 0),
                                          0, PCI_DEVFN(1, 0));
        uint8_t nested[4] = { 0xd1, 0xd2, 0xd3, 0xd4 };

        if (card) {
            s->nested_active = true;
            pci_dma_write(card, 0x80003000, nested, sizeof(nested));
            s->nested_active = false;
        }
    }
    if (s->target_port) {
        if (s->pic_switched) {
            address &= SGI_XIO_PACKET_SIZE - 1;
        }
        result = sgi_xio_port_packet_rw(s->target_port, s->target_index,
                                        address, buffer, size, write, source);
    } else if (address >= s->size || size > s->size - address) {
        result = MEMTX_DECODE_ERROR;
    } else if (write && s->packet_failure) {
        size_t partial = MIN(size, s->packet_partial);

        if (partial) {
            result = address_space_write(&s->ram_as, address,
                MEMTXATTRS_UNSPECIFIED, buffer, partial);
            if (result != MEMTX_OK) {
                s->packet_result_value = result;
                return result;
            }
        }
        result = s->packet_result;
    } else {
        result = write ? address_space_write(&s->ram_as, address,
                    MEMTXATTRS_UNSPECIFIED, buffer, size) :
                         address_space_read(&s->ram_as, address,
                    MEMTXATTRS_UNSPECIFIED, buffer, size);
    }
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
                                       address, buffer, size, write, &source);
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
    bool write)
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
    if (!s->size || s->size > 16 * MiB) {
        error_setg(errp, "XIO test RAM size must be 1 byte to 16 MiB");
        return;
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
    address_space_init(&s->ram_as, &s->ram, "xio-test-target.ram");
    s->ram_as_initialized = true;
    memory_region_init_alias(&s->alias, OBJECT(s), "cpu-alias", &s->ram,
                             0, s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->alias);
    object_property_add_uint64_ptr(OBJECT(s), "x-test-packet-calls",
                                   &s->packet_calls, OBJ_PROP_FLAG_READ);
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
    if (s->target_port) {
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
        s->pic = qdev_new(TYPE_SGI_PIC);
        object_property_add_child(OBJECT(s), "pic", OBJECT(s->pic));
        qdev_prop_set_bit(s->pic, "test-fixture", true);
        qdev_prop_set_bit(s->pic, "linked-transport", s->pic_linked);
        if (s->pic_linked) {
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
    DEFINE_PROP_BOOL("pic-peer", SGIXioTestTargetState, pic_peer, false),
    DEFINE_PROP_BOOL("pic-linked", SGIXioTestTargetState, pic_linked, true),
    DEFINE_PROP_BOOL("pic-switched", SGIXioTestTargetState, pic_switched,
                     false),
    DEFINE_PROP_BOOL("packet-failure", SGIXioTestTargetState,
                     packet_failure, false),
    DEFINE_PROP_BOOL("nested-dma", SGIXioTestTargetState, nested_dma, false),
    DEFINE_PROP_SIZE("packet-partial", SGIXioTestTargetState,
                     packet_partial, 0),
    DEFINE_PROP_UINT32("packet-result", SGIXioTestTargetState,
                       packet_result, MEMTX_ACCESS_ERROR),
    DEFINE_PROP_SIZE("size", SGIXioTestTargetState, size, 64 * KiB),
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
