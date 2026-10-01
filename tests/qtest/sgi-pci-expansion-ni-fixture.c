/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synthetic compute/BIC NI composition, without firmware or PCI wiring. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/pci/pci_device.h"
#include "hw/misc/sgi-bedrock.h"
#include "hw/misc/sgi-l1.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qobject/qlist.h"
#include "../../hw/mips/sgi/sn1-fabric.h"
#include "../../hw/mips/sgi/origin300-pci-expansion.h"

#define TYPE_NI_FIXTURE "sgi-pci-expansion-ni-fixture"
OBJECT_DECLARE_SIMPLE_TYPE(NIFixtureState, NI_FIXTURE)
#define TYPE_NI_CARD "sgi-pci-expansion-ni-card"
OBJECT_DECLARE_SIMPLE_TYPE(NICardState, NI_CARD)
#define RAM_BYTES 0x100000
#define NODE_SHIFT 33
#define HUB UINT64_C(0x10030000000)
#define IO UINT64_C(0x20000000000)

struct NICardState {
    PCIDevice parent_obj;
    MemoryRegion bar;
    uint64_t address;
    uint64_t payload;
    uint32_t result;
    uint32_t signature;
    uint8_t pin;
    uint8_t interrupt_pin;
};

static uint64_t card_read(void *opaque, hwaddr offset, unsigned size)
{
    NICardState *s = opaque;

    switch (offset) {
    case 0:
        return s->signature;
    case 8:
        return size == 8 ? s->address : (uint32_t)s->address;
    case 12:
        return s->address >> 32;
    case 16:
        return size == 8 ? s->payload : (uint32_t)s->payload;
    case 20:
        return s->payload >> 32;
    case 24:
        return s->result;
    case 32:
        return s->pin;
    default: return 0;
    }
}

static void card_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    NICardState *s = opaque;
    uint8_t buffer[8];

    switch (offset) {
    case 8:
        s->address = size == 8 ? value :
            (s->address & UINT64_C(0xffffffff00000000)) | (uint32_t)value;
        break;
    case 12:
        s->address = deposit64(s->address, 32, 32, value);
        break;
    case 16:
        s->payload = size == 8 ? value :
            (s->payload & UINT64_C(0xffffffff00000000)) | (uint32_t)value;
        break;
    case 20:
        s->payload = deposit64(s->payload, 32, 32, value);
        break;
    case 24:
        stq_be_p(buffer, s->payload);
        if (value == 1) {
            s->result = pci_dma_write(PCI_DEVICE(s), s->address, buffer, 8);
        } else if (value == 2) {
            s->result = pci_dma_read(PCI_DEVICE(s), s->address, buffer, 8);
            if (s->result == MEMTX_OK) {
                s->payload = ldq_be_p(buffer);
            }
        } else {
            s->result = MEMTX_ACCESS_ERROR;
        }
        break;
    case 32:
        s->pin = value & 1;
        pci_set_irq(PCI_DEVICE(s), s->pin);
        break;
    }
}

static const MemoryRegionOps card_ops = {
    .read = card_read,
    .write = card_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 8 },
};

static void card_reset_hold(Object *obj, ResetType type)
{
    NICardState *s = NI_CARD(obj);

    s->address = 0;
    s->payload = 0;
    s->result = MEMTX_DECODE_ERROR;
    s->pin = 0;
    pci_set_irq(PCI_DEVICE(s), 0);
}

static int card_post_load(void *opaque, int version)
{
    NICardState *s = opaque;

    pci_set_irq(PCI_DEVICE(s), s->pin);
    return 0;
}

static const VMStateDescription card_vmstate = {
    .name = TYPE_NI_CARD,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = card_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, NICardState),
        VMSTATE_UINT64(address, NICardState),
        VMSTATE_UINT64(payload, NICardState),
        VMSTATE_UINT32(result, NICardState),
        VMSTATE_UINT32_EQUAL(signature, NICardState),
        VMSTATE_UINT8_EQUAL(interrupt_pin, NICardState),
        VMSTATE_UINT8(pin, NICardState),
        VMSTATE_END_OF_LIST()
    },
};

static void card_realize(PCIDevice *dev, Error **errp)
{
    NICardState *s = NI_CARD(dev);

    if (s->interrupt_pin < 1 || s->interrupt_pin > PCI_NUM_PINS) {
        error_setg(errp, "NI card needs an interrupt pin from 1 to 4");
        return;
    }
    dev->config[PCI_INTERRUPT_PIN] = s->interrupt_pin;
    memory_region_init_io(&s->bar, OBJECT(s), &card_ops, s,
                          "ni-card.bar", 4096);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);
}

static const Property card_properties[] = {
    DEFINE_PROP_UINT8("interrupt-pin", NICardState, interrupt_pin, 1),
    DEFINE_PROP_UINT32("signature", NICardState, signature, 0),
    DEFINE_PROP_UINT64("dma-address", NICardState, address, 0),
    DEFINE_PROP_UINT64("dma-payload", NICardState, payload, 0),
};

static void card_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    pc->realize = card_realize;
    pc->vendor_id = 0x1b36;
    pc->device_id = 0x0010;
    pc->class_id = PCI_CLASS_OTHERS;
    dc->vmsd = &card_vmstate;
    dc->user_creatable = false;
    rc->phases.hold = card_reset_hold;
    device_class_set_props(dc, card_properties);
}

typedef struct NIRequester {
    NIFixtureState *fixture;
    MemoryRegion view;
    unsigned origin;
    uint32_t result;
} NIRequester;

struct NIFixtureState {
    SysBusDevice parent_obj;
    DeviceState *compute;
    DeviceState *chassis;
    DeviceState *cable;
    DeviceState *l1[2];
    DeviceState *uart[2];
    MemoryRegion uart_view[2];
    bool management;
    char *record[2];
    MemoryRegion management_control;
    SGIL1QueryOwner owners[2];
    SGIL1QueryAssociation associations[2];
    SGIL1QueryAttachment attachment;
    SGIL1QueryRouter router;
    SN1FabricEndpoint endpoint;
    MemoryRegion ram;
    MemoryRegion ram_observation;
    MemoryRegion compute_observation;
    AddressSpace ram_as;
    AddressSpace hub_as;
    NIRequester requester[2];
    MemoryRegion status;
    uint8_t compute_number;
    uint8_t bic_number;
    bool connected;
    uint8_t intx_map;
    uint8_t isp_head;
    uint64_t compute_device_mask;
    uint64_t interrupt_count;
    uint64_t interrupt_address;
    uint64_t interrupt_data;
    uint64_t interrupt_source;
};

static MemTxResult fixture_processor_rw(SN1FabricEndpoint *endpoint,
    unsigned origin, hwaddr address, void *buffer, size_t size, bool write,
    MemTxAttrs attrs)
{
    NIFixtureState *s = NI_FIXTURE(endpoint->owner);
    uint64_t base = (uint64_t)endpoint->placement << NODE_SHIFT;
    AddressSpace *as;
    hwaddr offset;

    if (address >= base && address - base < RAM_BYTES &&
        size <= RAM_BYTES - (address - base)) {
        as = &s->ram_as;
        offset = address - base;
    } else if (address >= HUB + base &&
               address - HUB - base < SGI_BEDROCK_SIZE &&
               size <= SGI_BEDROCK_SIZE - (address - HUB - base)) {
        as = &s->hub_as;
        offset = address - HUB - base;
    } else if (address >= UINT64_C(0x20001800000) + base &&
               address - UINT64_C(0x20001800000) - base < SGI_BEDROCK_SIZE &&
               size <= SGI_BEDROCK_SIZE -
                       (address - UINT64_C(0x20001800000) - base)) {
        as = &s->hub_as;
        offset = address - UINT64_C(0x20001800000) - base;
    } else {
        return MEMTX_DECODE_ERROR;
    }
    return write ? address_space_write(as, offset, attrs, buffer, size) :
                   address_space_read(as, offset, attrs, buffer, size);
}

static MemTxResult fixture_destination_rw(SN1FabricEndpoint *endpoint,
    const SGIXioSource *source, hwaddr address, void *buffer, size_t size,
    bool write)
{
    NIFixtureState *s = NI_FIXTURE(endpoint->owner);
    AddressSpace *as;
    hwaddr offset;

    if (address & (UINT64_C(1) << 47)) {
        hwaddr window = address & ((UINT64_C(1) << 39) - 1);

        if ((window & ~UINT64_C(0x7fffff)) != UINT64_C(0x1800000)) {
            return MEMTX_DECODE_ERROR;
        }
        offset = window & 0x7fffff;
        /* Only PI0/PI1, never BIC-local memory or unsupported units. */
        if (offset >= 0x400000 || size > 0x400000 - offset) {
            return MEMTX_DECODE_ERROR;
        }
        if (write && size == 8) {
            s->interrupt_count++;
            s->interrupt_address = address;
            s->interrupt_data = ldq_be_p(buffer);
            s->interrupt_source = source->widget |
                ((uint64_t)source->widget_known << 8) |
                ((uint64_t)source->device_known << 9);
        }
        as = &SGI_BEDROCK(s->compute)->io.hub_as;
    } else {
        offset = address & ((UINT64_C(1) << NODE_SHIFT) - 1);
        if (offset >= RAM_BYTES || size > RAM_BYTES - offset) {
            return MEMTX_DECODE_ERROR;
        }
        as = &s->ram_as;
    }
    return write ? address_space_write(as, offset, MEMTXATTRS_UNSPECIFIED,
                                      buffer, size) :
                   address_space_read(as, offset, MEMTXATTRS_UNSPECIFIED,
                                     buffer, size);
}

static MemTxResult requester_read(void *opaque, hwaddr address, uint64_t *value,
                                  unsigned size, MemTxAttrs attrs)
{
    NIRequester *r = opaque;
    uint8_t buffer[8] = { 0 };

    r->result = sn1_fabric_processor_rw(&r->fixture->endpoint, r->origin,
                                      address, buffer, size, false, attrs);
    switch (size) {
    case 1:
        *value = buffer[0];
        break;
    case 2:
        *value = lduw_be_p(buffer);
        break;
    case 4:
        *value = ldl_be_p(buffer);
        break;
    case 8:
        *value = ldq_be_p(buffer);
        break;
    }
    return r->result;
}

static MemTxResult requester_write(void *opaque, hwaddr address, uint64_t value,
                                   unsigned size, MemTxAttrs attrs)
{
    NIRequester *r = opaque;
    uint8_t buffer[8];

    switch (size) {
    case 1:
        buffer[0] = value;
        break;
    case 2:
        stw_be_p(buffer, value);
        break;
    case 4:
        stl_be_p(buffer, value);
        break;
    case 8:
        stq_be_p(buffer, value);
        break;
    default: return MEMTX_DECODE_ERROR;
    }
    r->result = sn1_fabric_processor_rw(&r->fixture->endpoint, r->origin,
                                      address, buffer, size, true, attrs);
    return r->result;
}

static const MemoryRegionOps requester_ops = {
    .read_with_attrs = requester_read,
    .write_with_attrs = requester_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static uint64_t status_read(void *opaque, hwaddr offset, unsigned size)
{
    NIFixtureState *s = opaque;

    switch (offset / 8) {
    case 0:
    case 1:
        return s->requester[offset / 8].result;
    case 2:
        return s->interrupt_count;
    case 3:
        return s->interrupt_address;
    case 4:
        return s->interrupt_data;
    case 5:
        return s->interrupt_source;
    default:
        return 0;
    }
}

static const MemoryRegionOps status_ops = {
    .read = status_read,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 8 },
};

static void fixture_place_claims(SN1FabricEndpoint *endpoint)
{
}

static SN1FabricEndpoint *fixture_endpoint(Object *obj)
{
    return &NI_FIXTURE(obj)->endpoint;
}

/* Composer controls, not registers of a physical management transport. */
static uint64_t management_read(void *opaque, hwaddr offset, unsigned size)
{
    NIFixtureState *s = opaque;

    return offset == 0 ? s->associations[0].reachable |
                        (s->associations[1].reachable << 1) : 0;
}

static void management_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    NIFixtureState *s = opaque;

    if (offset == 0) {
        s->associations[0].reachable = value & 1;
        s->associations[1].reachable = value & 2;
    } else if (offset == 8 && value < 2) {
        resettable_reset(OBJECT(s->l1[value]), RESET_TYPE_COLD);
    }
}

static const MemoryRegionOps management_ops = {
    .read = management_read,
    .write = management_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void fixture_reset_hold(Object *obj, ResetType type)
{
    NIFixtureState *s = NI_FIXTURE(obj);

    s->router.active = false;
    s->associations[0].reachable = true;
    s->associations[1].reachable = true;
}

static void fixture_unrealize(DeviceState *dev)
{
    sgi_l1_query_router_destroy(&NI_FIXTURE(dev)->router);
}

static int fixture_pre_save(void *opaque)
{
    return NI_FIXTURE(opaque)->router.active ? -EBUSY : 0;
}

static const VMStateDescription vmstate_management_owner = {
    .name = TYPE_NI_FIXTURE "/management-owner",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_EQUAL(identity, SGIL1QueryOwner),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_management_association = {
    .name = TYPE_NI_FIXTURE "/management-association",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(from, SGIL1QueryAssociation),
        VMSTATE_UINT32_EQUAL(to, SGIL1QueryAssociation),
        VMSTATE_BOOL(reachable, SGIL1QueryAssociation),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_management_attachment = {
    .name = TYPE_NI_FIXTURE "/management-attachment",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(issuer, SGIL1QueryAttachment),
        VMSTATE_UINT32_EQUAL(owner, SGIL1QueryAttachment),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_fixture = {
    .name = TYPE_NI_FIXTURE,
    .version_id = 2,
    .minimum_version_id = 2,
    .pre_save = fixture_pre_save,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_EQUAL(isp_head, NIFixtureState),
        VMSTATE_SINGLE(management, NIFixtureState, 0,
                       vmstate_info_uint8_equal, bool),
        VMSTATE_STRUCT_ARRAY(owners, NIFixtureState, 2, 0,
                             vmstate_management_owner, SGIL1QueryOwner),
        VMSTATE_STRUCT_ARRAY(associations, NIFixtureState, 2, 0,
                             vmstate_management_association,
                             SGIL1QueryAssociation),
        VMSTATE_STRUCT(attachment, NIFixtureState, 0,
                       vmstate_management_attachment, SGIL1QueryAttachment),
        VMSTATE_END_OF_LIST()
    },
};

static void fixture_realize(DeviceState *dev, Error **errp)
{
    NIFixtureState *s = NI_FIXTURE(dev);
    SGIBedrockState *compute;
    Error *local_error = NULL;
    SN1FabricEndpoint *bic_endpoint;
    static const char *const port_props[] = {
        "chip1-e-port", "chip1-f-port", "chip2-e-port", "chip2-f-port",
    };
    static const uint8_t ports[] = { 8, 9, 0xc, 0xd };
    unsigned i;

    if (s->isp_head != UINT8_MAX && s->isp_head >= SGI_ORIGIN300_PCI_HEADS) {
        error_setg(errp, "Invalid ISP1040 experiment head %u", s->isp_head);
        return;
    }
    if (!memory_region_init_ram(&s->ram, OBJECT(s), "ni-fixture.ram",
                                RAM_BYTES, errp)) {
        return;
    }
    address_space_init(&s->ram_as, &s->ram, "ni-fixture.ram");
    s->compute = qdev_new(TYPE_SGI_BEDROCK);
    object_property_add_child(OBJECT(s), "compute", OBJECT(s->compute));
    compute = SGI_BEDROCK(s->compute);
    qdev_prop_set_uint8(s->compute, "revision", 2);
    qdev_prop_set_uint64(s->compute, "device-mask-reset",
                         s->compute_device_mask);
    qdev_prop_set_uint32(s->compute, "processor-count", 2);
    qdev_prop_set_uint32(DEVICE(&compute->lb), "node-number",
                         s->compute_number);
    if (!object_property_set_link(OBJECT(s->compute), "dma-memory",
                                  OBJECT(&s->ram), errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(s->compute), errp)) {
        return;
    }
    address_space_init(&s->hub_as, sysbus_mmio_get_region(
        SYS_BUS_DEVICE(s->compute), 0), "ni-fixture.hub");
    s->endpoint = (SN1FabricEndpoint) {
        .owner = OBJECT(s),
        .bedrock = compute,
        .placement = 3,
        .ready = true,
        .place_claims = fixture_place_claims,
        .processor_rw = fixture_processor_rw,
        .destination_rw = fixture_destination_rw,
    };
    s->chassis = qdev_new(TYPE_SGI_ORIGIN300_PCI_EXPANSION);
    object_property_add_child(OBJECT(s), "chassis", OBJECT(s->chassis));
    qdev_prop_set_uint32(s->chassis, "placement", 42);
    qdev_prop_set_uint8(s->chassis, "node-number", s->bic_number);
    qdev_prop_set_uint8(s->chassis, "bedrock-revision", 2);
    qdev_prop_set_uint8(s->chassis, "xbridge-revision", 2);
    qdev_prop_set_string(s->chassis, "bus-prefix", "ni-fixture-");
    for (i = 0; i < 4; i++) {
        qdev_prop_set_uint8(s->chassis, port_props[i], ports[i]);
    }
    if (s->intx_map) {
        SGIOrigin300PciIntxMap map;

        for (unsigned head = 0; head < SGI_ORIGIN300_PCI_HEADS; head++) {
            for (unsigned device = 0; device < 2; device++) {
                map.pin[head][device][0] = device + 1;
                map.pin[head][device][1] = device + 5;
                map.pin[head][device][2] = device + 1;
                map.pin[head][device][3] = device + 5;
            }
        }
        if (s->intx_map == 2) {
            map.pin[0][0][0] = 3;
            map.pin[0][0][2] = 3;
        }
        if (!sgi_origin300_pci_expansion_set_intx_map(
                SGI_ORIGIN300_PCI_EXPANSION(s->chassis), &map, errp)) {
            return;
        }
    }
    if (!sgi_origin300_pci_expansion_enable_ni_route(
            SGI_ORIGIN300_PCI_EXPANSION(s->chassis), errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(s->chassis), errp)) {
        return;
    }
    bic_endpoint = SGI_SN1_FABRIC_GET_CLASS(s->chassis)->endpoint(
        OBJECT(s->chassis));
    if (s->connected) {
        s->cable = qdev_new(TYPE_SGI_NET_CABLE);
        object_property_add_child(OBJECT(s), "cable", OBJECT(s->cable));
        sgi_net_cable_set_end(SGI_NET_CABLE(s->cable), 0, OBJECT(compute), 0);
        sgi_net_cable_set_end(SGI_NET_CABLE(s->cable), 1,
                             OBJECT(bic_endpoint->bedrock), 0);
        if (!qdev_realize(s->cable, NULL, errp) ||
            !object_property_set_link(OBJECT(compute), "net-cable",
                                      OBJECT(s->cable), errp) ||
            !object_property_set_link(OBJECT(bic_endpoint->bedrock),
                                      "net-cable", OBJECT(s->cable), errp)) {
            return;
        }
    }
    for (i = 0; i < 6; i++) {
        bool isp = i == s->isp_head;
        DeviceState *card = qdev_new(isp ? "qlogic-isp1040" : TYPE_NI_CARD);
        PCIBus *bus = sgi_origin300_pci_expansion_bus(
            SGI_ORIGIN300_PCI_EXPANSION(s->chassis), i);
        g_autofree char *name = g_strdup_printf("card%u", i);

        object_property_add_child(OBJECT(s), name, OBJECT(card));
        if (!isp) {
            qdev_prop_set_uint32(card, "signature", 0xabc00000 + i);
        }
        qdev_prop_set_uint32(card, "addr", PCI_DEVFN(1, 0));
        if (s->intx_map && !isp) {
            qdev_prop_set_bit(card, "multifunction", true);
        }
        if (!qdev_realize(card, BUS(bus), errp)) {
            return;
        }
    }
    if (s->intx_map) {
        for (i = 0; i < SGI_ORIGIN300_PCI_HEADS; i++) {
            PCIBus *bus = sgi_origin300_pci_expansion_bus(
                SGI_ORIGIN300_PCI_EXPANSION(s->chassis), i);

            if (i == s->isp_head) {
                continue;
            }
            for (unsigned device = 1;
                 device <= (s->intx_map == 3 ? 3 : 2); device++) {
                for (unsigned pin = 0; pin < PCI_NUM_PINS; pin++) {
                    DeviceState *card;
                    g_autofree char *name = NULL;

                    if (device == 1 && pin == 0) {
                        continue;
                    }
                    card = qdev_new(TYPE_NI_CARD);
                    name = g_strdup_printf("card%u-dev%u-pin%u",
                                           i, device, pin);
                    object_property_add_child(OBJECT(s), name, OBJECT(card));
                    qdev_prop_set_uint32(card, "signature", 0xabc00000 + i);
                    qdev_prop_set_uint8(card, "interrupt-pin", pin + 1);
                    qdev_prop_set_bit(card, "multifunction", true);
                    qdev_prop_set_uint32(card, "addr", PCI_DEVFN(device, pin));
                    if (!qdev_realize(card, BUS(bus), errp)) {
                        return;
                    }
                }
            }
        }
    }
    for (i = 0; i < 2; i++) {
        NIRequester *r = &s->requester[i];
        g_autofree char *name = g_strdup_printf("ni-fixture.requester%u", i);

        r->fixture = s;
        r->origin = i;
        memory_region_init_io(&r->view, OBJECT(s), &requester_ops, r,
                              name, UINT64_C(1) << 42);
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &r->view);
    }
    memory_region_init_io(&s->status, OBJECT(s), &status_ops, s,
                          "ni-fixture.status", 48);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->status);
    memory_region_init_alias(&s->ram_observation, OBJECT(s),
        "ni-fixture.ram-observation", &s->ram, 0, RAM_BYTES);
    memory_region_init_alias(&s->compute_observation, OBJECT(s),
        "ni-fixture.compute-observation", sysbus_mmio_get_region(
            SYS_BUS_DEVICE(s->compute), 0), 0, SGI_BEDROCK_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->ram_observation);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->compute_observation);
    if (s->management) {
        for (i = 0; i < 2; i++) {
            g_autofree char *controller = g_strdup_printf("l1%u", i);
            g_autofree char *uart = g_strdup_printf("uart%u", i);
            g_autofree char *view = g_strdup_printf("ni-fixture.uart%u", i);

            s->l1[i] = qdev_new(TYPE_SGI_L1);
            object_property_add_child(OBJECT(s), controller, OBJECT(s->l1[i]));
            qdev_prop_set_string(s->l1[i], "brick-type",
                                 i ? "pci-expansion" : "ip45");
            qdev_prop_set_uint32(s->l1[i], "rack", 1);
            qdev_prop_set_uint32(s->l1[i], "bay", i + 1);
            qdev_prop_set_bit(s->l1[i], "auto-power", false);
            qdev_prop_set_bit(s->l1[i], "network-422", false);
            if (s->record[i]) {
                QList *records = qlist_new();

                qlist_append_str(records, s->record[i]);
                qdev_prop_set_array(s->l1[i], "board-record-eeprom", records);
            }
            if (!qdev_realize(s->l1[i], NULL, errp)) {
                return;
            }
            s->owners[i].controller = SGI_L1(s->l1[i]);
            s->uart[i] = qdev_new(TYPE_SGI_L1_UART);
            object_property_add_child(OBJECT(s), uart, OBJECT(s->uart[i]));
            if (!object_property_set_link(OBJECT(s->uart[i]), "controller",
                                          OBJECT(s->l1[i]), errp) ||
                !sysbus_realize(SYS_BUS_DEVICE(s->uart[i]), errp)) {
                return;
            }
            /* Observation port, not a claimed BIC guest UART aperture. */
            memory_region_init_alias(&s->uart_view[i], OBJECT(s), view,
                sysbus_mmio_get_region(SYS_BUS_DEVICE(s->uart[i]), 0),
                0, SGI_L1_SIZE);
            sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->uart_view[i]);
        }
        s->associations[0] = (SGIL1QueryAssociation) { 0, 1, true };
        s->associations[1] = (SGIL1QueryAssociation) { 1, 0, true };
        sgi_l1_query_router_init(&s->router, s->owners, 2,
                                s->associations, 2, &local_error);
        if (local_error) {
            error_propagate(errp, local_error);
            return;
        }
        if (!sgi_l1_query_router_set_attached(&s->router, &s->attachment,
                                              1, errp)) {
            sgi_l1_query_router_destroy(&s->router);
            return;
        }
        memory_region_init_io(&s->management_control, OBJECT(s),
                              &management_ops, s, "ni-fixture.management", 16);
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->management_control);
    }
}

static const Property fixture_properties[] = {
    DEFINE_PROP_BOOL("management", NIFixtureState, management, false),
    DEFINE_PROP_STRING("record0", NIFixtureState, record[0]),
    DEFINE_PROP_STRING("record1", NIFixtureState, record[1]),
    DEFINE_PROP_UINT64("identity0", NIFixtureState, owners[0].identity, 101),
    DEFINE_PROP_UINT64("identity1", NIFixtureState, owners[1].identity, 102),
    DEFINE_PROP_UINT32("attached-issuer", NIFixtureState, attachment.issuer, 0),
    DEFINE_PROP_UINT32("attached-owner", NIFixtureState, attachment.owner, 1),
    DEFINE_PROP_UINT8("intx-map", NIFixtureState, intx_map, 0),
    DEFINE_PROP_UINT8("isp-head", NIFixtureState, isp_head, UINT8_MAX),
    DEFINE_PROP_UINT8("compute-number", NIFixtureState, compute_number, 3),
    DEFINE_PROP_UINT8("bic-number", NIFixtureState, bic_number, 7),
    DEFINE_PROP_BOOL("connected", NIFixtureState, connected, true),
    DEFINE_PROP_UINT64("compute-device-mask", NIFixtureState,
                       compute_device_mask, UINT64_MAX),
};

static void fixture_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SN1FabricClass *fc = SGI_SN1_FABRIC_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = fixture_realize;
    dc->unrealize = fixture_unrealize;
    dc->vmsd = &vmstate_fixture;
    rc->phases.hold = fixture_reset_hold;
    dc->desc = "Synthetic compute/BIC direct NI fixture";
    dc->user_creatable = true;
    device_class_set_props(dc, fixture_properties);
    fc->endpoint = fixture_endpoint;
}

static const TypeInfo fixture_types[] = {
    {
        .name = TYPE_NI_CARD,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(NICardState),
        .class_init = card_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { }
        },
    },
    {
        .name = TYPE_NI_FIXTURE,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(NIFixtureState),
        .class_init = fixture_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_SGI_SN1_FABRIC },
            { }
        },
    },
};

DEFINE_TYPES(fixture_types)
