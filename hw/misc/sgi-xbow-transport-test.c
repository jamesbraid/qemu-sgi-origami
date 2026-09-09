/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Sparse endpoints distinguish CPU offsets from full packet addresses. */
#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/boards.h"
#include "hw/misc/sgi-xbow.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/core/irq.h"
#include "io/channel-buffer.h"
#include "migration/qemu-file.h"
#include "system/address-spaces.h"

#define TYPE_XBOW_TRANSPORT_TEST "sgi-xbow-transport-test"
OBJECT_DECLARE_SIMPLE_TYPE(XbowTransportTest, XBOW_TRANSPORT_TEST)

typedef struct Endpoint {
    MemoryRegion mr;
    XbowTransportTest *test;
    unsigned index;
} Endpoint;

struct XbowTransportTest {
    SysBusDevice parent_obj;
    SGIXbowState xbow;
    MemoryRegion control;
    MemoryRegion cpu_alias;
    MemoryRegion packet_alias;
    AddressSpace *packets;
    Endpoint endpoint[4];
    uint64_t count[4];
    uint64_t address[4];
    uint64_t value[4];
    uint64_t attributes[4];
    uint64_t probe;
    uint64_t result;
    uint64_t guards;
    bool stop_on_link_reset;
    uint64_t reset_high;
    uint64_t reset_low;
    AddressSpace *cpus;
    MemoryRegion *held_face;
};

#define TYPE_XBOW_ENDPOINT_LIFE "sgi-xbow-endpoint-life-test"

typedef struct EndpointLife {
    Object parent_obj;
    MemoryRegion mr;
    bool tracked;
} EndpointLife;

static uint64_t endpoint_finalizations;

static void endpoint_life_finalize(Object *obj)
{
    EndpointLife *life = (EndpointLife *)obj;

    if (life->tracked) {
        endpoint_finalizations++;
    }
}

static const TypeInfo endpoint_life_type = {
    .name = TYPE_XBOW_ENDPOINT_LIFE,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(EndpointLife),
    .instance_finalize = endpoint_life_finalize,
};

static void lifetime_probe(XbowTransportTest *s)
{
    EndpointLife *life = (EndpointLife *)object_new(TYPE_XBOW_ENDPOINT_LIFE);
    SGIXbowState *xbow = SGI_XBOW(object_new(TYPE_SGI_XBOW));

    assert(!s->held_face);
    life->tracked = true;
    memory_region_init(&life->mr, OBJECT(life), "endpoint-life", 4096);
    object_property_add_child(OBJECT(s), "temporary-xbow", OBJECT(xbow));
    qdev_prop_set_uint64(DEVICE(xbow), "span", 4096);
    qdev_prop_set_uint16(DEVICE(xbow), "links-present", 0x100);
    qdev_prop_set_uint16(DEVICE(xbow), "startup-links-alive", 0x100);
    assert(sgi_xbow_attach_widget(xbow, 8, &life->mr, &error_abort));
    assert(sysbus_realize(SYS_BUS_DEVICE(xbow), &error_abort));
    memory_region_ref(&life->mr);
    s->held_face = &life->mr;
    object_unref(OBJECT(life));
    sgi_xbow_quiesce(xbow);
    qdev_unrealize(DEVICE(xbow));
    object_unparent(OBJECT(xbow));
    object_unref(OBJECT(xbow));
}

static void unrealized_probe(XbowTransportTest *s)
{
    SGIXbowState *xbow = SGI_XBOW(object_new(TYPE_SGI_XBOW));
    Error *err = NULL;

    object_property_add_child(OBJECT(s), "unrealized-xbow", OBJECT(xbow));
    qdev_prop_set_uint64(DEVICE(xbow), "span", 4096);
    sgi_xbow_quiesce(xbow);
    sgi_xbow_quiesce(xbow);
    s->result = 0;
    if (!sgi_xbow_attach_widget(xbow, 8, &s->endpoint[0].mr, &err) && err) {
        s->result |= 1;
    }
    error_free(err);
    err = NULL;
    if (!sysbus_realize(SYS_BUS_DEVICE(xbow), &err) && err) {
        s->result |= 2;
    }
    error_free(err);
    object_unparent(OBJECT(xbow));
    object_unref(OBJECT(xbow));
}

static MemTxResult endpoint_read(void *opaque, hwaddr addr, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    Endpoint *e = opaque;
    XbowTransportTest *s = e->test;

    s->count[e->index]++;
    s->address[e->index] = addr;
    s->attributes[e->index] = attrs.requester_id;
    *data = s->value[e->index];
    return addr == 0xdead0 ? MEMTX_ERROR : MEMTX_OK;
}

static MemTxResult endpoint_write(void *opaque, hwaddr addr, uint64_t data,
                                  unsigned size, MemTxAttrs attrs)
{
    Endpoint *e = opaque;
    XbowTransportTest *s = e->test;
    uint64_t ignored;
    MemTxResult result = endpoint_read(opaque, addr, &ignored, size, attrs);

    if (result == MEMTX_OK) {
        s->value[e->index] = data;
    }
    return result;
}

static const MemoryRegionOps endpoint_ops = {
    .read_with_attrs = endpoint_read,
    .write_with_attrs = endpoint_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static void reset_observed(void *opaque, int n, int level)
{
    XbowTransportTest *s = opaque;

    if (level) {
        s->reset_high++;
    } else {
        s->reset_low++;
    }
}

static unsigned pending_resets(XbowTransportTest *s)
{
    unsigned mask = 0;
    unsigned i;

    for (i = 0; i < SGI_XBOW_LINKS; i++) {
        if (timer_pending(s->xbow.link_reset_timer[i])) {
            mask |= 1U << i;
        }
    }
    return mask;
}

/* Exercise the real device VMState, including failure before post_load. */
static void load_reset_state(XbowTransportTest *s, bool truncate)
{
    const VMStateDescription *vmsd = DEVICE_GET_CLASS(&s->xbow)->vmsd;
    QIOChannelBuffer *buffer = qio_channel_buffer_new(1024);
    QIOChannelBuffer *input;
    QEMUFile *file = qemu_file_new_output(QIO_CHANNEL(buffer));
    Error *err = NULL;
    int ret;

    s->xbow.link_reset_asserted = 1;
    timer_mod(s->xbow.link_reset_timer[0],
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    ret = vmstate_save_state(file, vmsd, &s->xbow, NULL, &err);
    assert(ret == 0);
    assert(!err);
    qemu_put_byte(file, 0);
    qemu_fflush(file);
    input = qio_channel_buffer_new(buffer->usage);
    memcpy(input->data, buffer->data, buffer->usage);
    input->usage = buffer->usage;
    qemu_fclose(file);
    s->xbow.link_reset_asserted = 0;
    timer_del(s->xbow.link_reset_timer[0]);
    /* Pre-load must cancel existing destination activity as well. */
    timer_mod(s->xbow.link_reset_timer[7],
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2000000);
    if (truncate) {
        /* Timer/deadline array is last, followed by the subsection marker. */
        assert(input->usage > 65);
        input->usage -= 57;
    }
    file = qemu_file_new_input(QIO_CHANNEL(input));
    s->result = vmstate_load_state(file, vmsd, &s->xbow,
                                   vmsd->version_id, &err);
    error_free(err);
    qemu_fclose(file);
    object_unref(OBJECT(buffer));
    object_unref(OBJECT(input));
}

static uint64_t control_read(void *opaque, hwaddr addr, unsigned size)
{
    XbowTransportTest *s = opaque;
    unsigned i = addr / 32;

    if (i < 4) {
        switch (addr % 32) {
        case 0: return s->count[i];
        case 8: return s->address[i];
        case 16: return s->value[i];
        case 24: return s->attributes[i];
        }
    }
    switch (addr) {
    case 0x90:
        return s->xbow.link_alive | ((uint64_t)s->xbow.link_initializing << 8) |
               ((uint64_t)s->xbow.link_eight_bit << 16) |
               ((uint64_t)s->xbow.link_reset_asserted << 24);
    case 0x98: return pending_resets(s);
    case 0xa0: return s->reset_high;
    case 0xa8: return s->reset_low;
    case 0xb0: return timer_expire_time_ns(s->xbow.link_reset_timer[0]);
    case 0xe0: return endpoint_finalizations;
    default: return addr == 0x80 ? s->result : s->guards;
    }
}

static void control_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    XbowTransportTest *s = opaque;
    MemTxAttrs attrs = { .requester_id = 0x1234 };
    uint8_t bytes[8] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe };

    if (addr == 0x80) {
        s->probe = value;
    } else if (addr == 0xb8) {
        sgi_xbow_quiesce(&s->xbow);
    } else if (addr == 0xc0) {
        uint8_t word[4] = { 0, 0, 0, 1 };
        s->result = address_space_write(value ? s->packets : s->cpus,
                                        s->probe, attrs, word, sizeof(word));
    } else if (addr == 0xc8) {
        unsigned i;

        for (i = 0; i < SGI_XBOW_LINKS; i++) {
            qdev_connect_gpio_out_named(DEVICE(&s->xbow), "link-reset", i,
                                        NULL);
        }
    } else if (addr == 0xd0) {
        load_reset_state(s, value);
    } else if (addr == 0xe0) {
        if (value) {
            lifetime_probe(s);
        } else if (s->held_face) {
            memory_region_unref(s->held_face);
            s->held_face = NULL;
        }
    } else if (addr == 0xe8) {
        unrealized_probe(s);
    } else if (addr == 0xd8) {
        /* A stale/imported timer must not emit output in refusal mode. */
        timer_mod(s->xbow.link_reset_timer[0],
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
    } else if (addr == 0x88) {
        s->result = address_space_rw(s->packets, s->probe, attrs,
                                     bytes, sizeof(bytes), value == 2);
    }
}

static const MemoryRegionOps control_ops = {
    .read = control_read,
    .write = control_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void record_refusal(XbowTransportTest *s, bool accepted,
                            Error **errp, unsigned bit)
{
    if (!accepted && *errp) {
        s->guards |= 1ULL << bit;
    }
    error_free(*errp);
    *errp = NULL;
}

static void transport_realize(DeviceState *dev, Error **errp)
{
    XbowTransportTest *s = XBOW_TRANSPORT_TEST(dev);
    unsigned i;
    Error *err = NULL;
    MemoryRegion uninitialized = { 0 };

    qdev_prop_set_bit(DEVICE(&s->xbow), "stop-on-link-reset",
                      s->stop_on_link_reset);
    qdev_init_gpio_in_named(dev, reset_observed, "reset-observed", 8);
    for (i = 0; i < 8; i++) {
        qdev_connect_gpio_out_named(DEVICE(&s->xbow), "link-reset", i,
            qdev_get_gpio_in_named(dev, "reset-observed", i));
    }
    qdev_prop_set_uint64(DEVICE(&s->xbow), "span", 1ULL << 34);
    qdev_prop_set_uint16(DEVICE(&s->xbow), "links-present", 0x700);
    qdev_prop_set_uint16(DEVICE(&s->xbow), "startup-links-alive", 0x700);
    for (i = 0; i < 4; i++) {
        g_autofree char *name = g_strdup_printf("endpoint%u", i);
        s->endpoint[i].test = s;
        s->endpoint[i].index = i;
        s->value[i] = 0x1122334400000000ULL + i;
        memory_region_init_io(&s->endpoint[i].mr, OBJECT(s), &endpoint_ops,
                              &s->endpoint[i], name,
                              i < 2 ? (4096ULL << i) :
                              1ULL << (i % 2 ? 48 : 34));
    }
    record_refusal(s, sgi_xbow_attach_endpoint(&s->xbow, 7,
                   &s->endpoint[0].mr, &s->endpoint[1].mr, &err), &err, 0);
    record_refusal(s, sgi_xbow_attach_endpoint(&s->xbow, 8,
                   NULL, &s->endpoint[1].mr, &err), &err, 1);
    record_refusal(s, sgi_xbow_attach_endpoint(&s->xbow, 8,
                   &s->endpoint[0].mr, NULL, &err), &err, 2);
    record_refusal(s, sgi_xbow_attach_endpoint(&s->xbow, 8,
                   &uninitialized, &s->endpoint[1].mr, &err), &err, 3);
    record_refusal(s, sgi_xbow_attach_widget(&s->xbow, 14, NULL, &err),
                   &err, 5);
    record_refusal(s, sgi_xbow_attach_endpoint(&s->xbow, 16,
                   &s->endpoint[0].mr, &s->endpoint[1].mr, &err), &err, 7);
    for (i = 0; i < 2; i++) {
        if (!sgi_xbow_attach_endpoint(&s->xbow, 8 + 2 * i,
                                      &s->endpoint[2 * i].mr,
                                      &s->endpoint[2 * i + 1].mr, errp)) {
            return;
        }
    }
    record_refusal(s, sgi_xbow_attach_endpoint(&s->xbow, 8,
                   &s->endpoint[2].mr, &s->endpoint[3].mr, &err), &err, 4);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->xbow), errp)) {
        return;
    }
    record_refusal(s, sgi_xbow_attach_endpoint(&s->xbow, 11,
                   &s->endpoint[0].mr, &s->endpoint[1].mr, &err), &err, 6);
    memory_region_init_io(&s->control, OBJECT(s), &control_ops, s,
                          "transport.control", 0x100);
    /* The control callback deliberately makes a nested test transaction. */
    s->control.disable_reentrancy_guard = true;
    memory_region_init_alias(&s->cpu_alias, OBJECT(s), "transport.cpu",
                             sgi_xbow_ingress(&s->xbow, 9), 0, 1ULL << 38);
    memory_region_init_alias(&s->packet_alias, OBJECT(s), "transport.packet",
                             sgi_xbow_packet_ingress(&s->xbow, 9),
                             0, 1ULL << 52);
    s->cpus = g_new0(AddressSpace, 1);
    s->packets = g_new0(AddressSpace, 1);
    address_space_init(s->cpus, &s->cpu_alias, "transport.cpu-probes");
    address_space_init(s->packets, &s->packet_alias, "transport.probes");
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->control);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->cpu_alias);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->packet_alias);
}

static void transport_unrealize(DeviceState *dev)
{
    XbowTransportTest *s = XBOW_TRANSPORT_TEST(dev);

    address_space_destroy_free(s->cpus);
    address_space_destroy_free(s->packets);
    s->cpus = NULL;
    s->packets = NULL;
    qdev_unrealize(DEVICE(&s->xbow));
}

static void transport_init(Object *obj)
{
    XbowTransportTest *s = XBOW_TRANSPORT_TEST(obj);

    object_initialize_child(obj, "xbow", &s->xbow, TYPE_SGI_XBOW);
}

static const VMStateDescription transport_vmstate = {
    .name = TYPE_XBOW_TRANSPORT_TEST,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(count, XbowTransportTest, 4),
        VMSTATE_UINT64_ARRAY(address, XbowTransportTest, 4),
        VMSTATE_UINT64_ARRAY(value, XbowTransportTest, 4),
        VMSTATE_UINT64_ARRAY(attributes, XbowTransportTest, 4),
        VMSTATE_END_OF_LIST()
    },
};

static const Property transport_properties[] = {
    DEFINE_PROP_BOOL("stop-on-link-reset", XbowTransportTest,
                     stop_on_link_reset, false),
};

static void transport_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = transport_realize;
    dc->unrealize = transport_unrealize;
    dc->vmsd = &transport_vmstate;
    device_class_set_props(dc, transport_properties);
    dc->user_creatable = true;
}

static const TypeInfo transport_type = {
    .name = TYPE_XBOW_TRANSPORT_TEST,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(XbowTransportTest),
    .instance_init = transport_init,
    .class_init = transport_class_init,
};

static void transport_machine_init(MachineState *machine)
{
    SysBusDevice *sbd;

    assert(cpu_create(machine->cpu_type));
    memory_region_add_subregion(get_system_memory(), 0, machine->ram);
    sbd = SYS_BUS_DEVICE(qdev_new(TYPE_XBOW_TRANSPORT_TEST));
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0x10000000);
    sysbus_mmio_map(sbd, 1, 1ULL << 54);
    sysbus_mmio_map(sbd, 2, 2ULL << 54);
}

static void transport_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Crossbow transport execution test machine";
    mc->init = transport_machine_init;
    mc->default_cpu_type = "R10000-mips64-cpu";
    mc->default_ram_size = 1 * MiB;
    mc->default_ram_id = "ram";
    mc->max_cpus = 1;
}

static const TypeInfo transport_machine_type = {
    .name = MACHINE_TYPE_NAME("sgi-xbow-transport-test"),
    .parent = TYPE_MACHINE,
    .class_init = transport_machine_class_init,
};

static void transport_register(void)
{
    type_register_static(&endpoint_life_type);
    type_register_static(&transport_type);
    type_register_static(&transport_machine_type);
}

type_init(transport_register)
