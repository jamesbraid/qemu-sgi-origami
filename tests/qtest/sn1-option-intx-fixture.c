/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/pci/pci.h"
#include "migration/vmstate.h"
#include "migration/qemu-file.h"
#include "io/channel-buffer.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "../../hw/mips/sgi/sn1-node.h"
#include "sgi-pci-expansion-ni-fixture.h"

/* Attach real synthetic PCI endpoints to the composed compute option buses. */
#define TYPE_OPTION_FIXTURE "sgi-compute-option-intx-fixture"
OBJECT_DECLARE_SIMPLE_TYPE(OptionFixtureState, OPTION_FIXTURE)

struct OptionFixtureState {
    SysBusDevice parent_obj;
    MemoryRegion control;
    SN1NodeState *node[2];
    DeviceState *cards[2][3][PCI_NUM_PINS];
    uint64_t migration_result;
    uint64_t offset;
    bool alternate_map;
};

static bool option_fixture_restore(OptionFixtureState *s, unsigned node,
                                   unsigned mismatch)
{
    SN1NodeState *n = s->node[node];
    const VMStateDescription *vmsd = qdev_get_vmsd(DEVICE(n));
    QIOChannelBuffer *channel = qio_channel_buffer_new(4096);
    QEMUFile *output = qemu_file_new_output(QIO_CHANNEL(channel));
    QEMUFile *input;
    Error *error = NULL;
    uint8_t map = n->option_intx_map[0];
    uint8_t selected = n->option_intx_selected;
    bool loaded;

    g_assert_true(vmstate_save_vmsd(output, vmsd, n, NULL, &error));
    g_assert_null(error);
    /* Subsection loading peeks at the following section marker. */
    qemu_put_byte(output, 0);
    g_assert_cmpint(qemu_fflush(output), ==, 0);
    if (mismatch == 1) {
        n->option_intx_map[0] ^= 1;
    } else if (mismatch == 2) {
        n->option_intx_selected ^= 1;
    } else {
        device_cold_reset(DEVICE(n));
    }
    channel->offset = 0;
    input = qemu_file_new_input(QIO_CHANNEL(channel));
    loaded = vmstate_load_vmsd(input, vmsd, n, vmsd->version_id, &error);
    n->option_intx_map[0] = map;
    n->option_intx_selected = selected;
    if (error) {
        error_report_err(error);
    }
    qemu_fclose(input);
    qemu_fclose(output);
    object_unref(OBJECT(channel));
    return loaded;
}

static uint64_t option_fixture_read(void *opaque, hwaddr offset, unsigned size)
{
    OptionFixtureState *s = opaque;
    unsigned node = offset / 0x1000;
    unsigned at = offset % 0x1000;

    if (node >= 2) {
        return 0;
    }
    if (at == 0) {
        return s->node[node]->option_intx_selected;
    }
    if (at == 8) {
        return s->migration_result;
    }
    if (at >= 0x400) {
        unsigned device = (at - 0x400) / 0x100;
        unsigned pin = ((at - 0x400) % 0x100) / 0x40;
        unsigned reg = at % 0x40;

        if (device < 3 && pin < PCI_NUM_PINS) {
            return sgi_ni_card_read(s->cards[node][device][pin], reg, size);
        }
    }
    return 0;
}

static void option_fixture_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size)
{
    OptionFixtureState *s = opaque;
    unsigned node = offset / 0x1000;
    unsigned at = offset % 0x1000;

    if (node >= 2) {
        return;
    }
    if (at == 8 && value <= 2) {
        s->migration_result = option_fixture_restore(s, node, value);
    } else if (at >= 0x400) {
        unsigned device = (at - 0x400) / 0x100;
        unsigned pin = ((at - 0x400) % 0x100) / 0x40;
        unsigned reg = at % 0x40;

        if (device < 3 && pin < PCI_NUM_PINS) {
            sgi_ni_card_write(s->cards[node][device][pin], reg, value, size);
        }
    }
}

static const MemoryRegionOps option_fixture_ops = {
    .read = option_fixture_read,
    .write = option_fixture_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void option_fixture_realize(DeviceState *dev, Error **errp)
{
    OptionFixtureState *s = OPTION_FIXTURE(dev);

    for (unsigned node = 0; node < 2; node++) {
        g_autofree char *path = g_strdup_printf("/machine/node%u", node);
        Object *owner = object_resolve_path(path, NULL);
        PCIBus *bus;

        if (!owner || !object_dynamic_cast(owner, TYPE_SGI_SN1_NODE)) {
            error_setg(errp, "compute option fixture requires two SN1 nodes");
            return;
        }
        s->node[node] = SN1_NODE(owner);
        /* A test-only perturbation distinguishes map data from fixed wiring. */
        if (s->alternate_map && node == 0) {
            if (!s->node[node]->option_intx_selected) {
                error_setg(errp, "alternate map requires "
                           "selected option board");
                return;
            }
            s->node[node]->option_intx_map[0] = 3;
        }
        bus = sn1_node_xbridge_pci_bus(s->node[node], 0xe);
        for (unsigned device = 1; device <= 3; device++) {
            for (unsigned pin = 0; pin < PCI_NUM_PINS; pin++) {
                DeviceState *card = qdev_new("sgi-pci-expansion-ni-card");
                g_autofree char *name = g_strdup_printf(
                    "node%u-device%u-pin%u", node, device, pin);

                object_property_add_child(OBJECT(s), name, OBJECT(card));
                s->cards[node][device - 1][pin] = card;
                qdev_prop_set_uint8(card, "interrupt-pin", pin + 1);
                qdev_prop_set_bit(card, "multifunction", true);
                qdev_prop_set_uint32(card, "addr", PCI_DEVFN(device, pin));
                if (!qdev_realize(card, BUS(bus), errp)) {
                    return;
                }
            }
        }
    }
    memory_region_init_io(&s->control, OBJECT(s), &option_fixture_ops, s,
                          "compute-option-fixture.control", 0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->control);
}

static void option_fixture_offset(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    visit_type_uint64(v, name, &OPTION_FIXTURE(obj)->offset, errp);
}

static void option_fixture_get_value(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    OptionFixtureState *s = OPTION_FIXTURE(obj);
    uint64_t value = option_fixture_read(s, s->offset, 8);

    visit_type_uint64(v, name, &value, errp);
}

static void option_fixture_set_value(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    OptionFixtureState *s = OPTION_FIXTURE(obj);
    uint64_t value;

    if (visit_type_uint64(v, name, &value, errp)) {
        option_fixture_write(s, s->offset, value, 8);
    }
}

static const Property option_fixture_properties[] = {
    DEFINE_PROP_BOOL("alternate-map", OptionFixtureState, alternate_map, false),
};

static void option_fixture_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    object_class_property_add(oc, "offset", "uint64", option_fixture_offset,
                              option_fixture_offset, NULL, NULL);
    object_class_property_add(oc, "value", "uint64", option_fixture_get_value,
                              option_fixture_set_value, NULL, NULL);
    dc->realize = option_fixture_realize;
    dc->user_creatable = true;
    dc->desc = "Synthetic composed compute option PCI endpoints";
    device_class_set_props(dc, option_fixture_properties);
}

static const TypeInfo option_fixture_type = {
    .name = TYPE_OPTION_FIXTURE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OptionFixtureState),
    .class_init = option_fixture_class_init,
};

static void option_fixture_register(void)
{
    type_register_static(&option_fixture_type);
}

type_init(option_fixture_register)
