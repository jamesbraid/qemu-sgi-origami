/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test-only widget-local RAM with a separate CPU observation alias. */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sgi-xio-port.h"
#include "hw/misc/sgi-bedrock-io.h"
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
};

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
    if (!memory_region_init_ram(&s->ram, OBJECT(s), name, s->size, errp)) {
        return;
    }
    memory_region_init_alias(&s->alias, OBJECT(s), "cpu-alias", &s->ram,
                             0, s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->alias);
    if (s->target_port) {
        memory_region_init_io(&s->source_window, OBJECT(s), &source_ops, s,
                              "explicit-source", 16 * SGI_XIO_PACKET_SIZE);
        memory_region_init_io(&s->source_status, OBJECT(s), &source_status_ops,
                              s, "source-status", 48);
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->source_window);
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->source_status);
    }
}

static const Property target_properties[] = {
    DEFINE_PROP_BOOL("destination-callback", SGIXioTestTargetState,
                      destination_callback, false),
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
    dc->user_creatable = true;
    dc->vmsd = &vmstate_target;
    device_class_set_props(dc, target_properties);
}

static const TypeInfo target_type = {
    .name = TYPE_SGI_XIO_TEST_TARGET,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIXioTestTargetState),
    .class_init = target_class_init,
};

static void target_register_types(void)
{
    type_register_static(&target_type);
}

type_init(target_register_types)
