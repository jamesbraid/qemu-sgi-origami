/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Origin 300 PCI expansion chassis: CPU-free, RAM-free BIC and six PCI heads.
 * Revision 2 reuse is inferred. The caller chooses downstream head pairing.
 * No external NI data route, discovery identity, flash or L1 UART is supplied.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sgi/xio-link.h"
#include "hw/misc/sgi-bedrock.h"
#include "hw/pci-host/sgi-xbridge.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "origin300-pci-expansion.h"
#include "sn1-fabric.h"

struct SGIOrigin300PciExpansionState {
    SysBusDevice parent_obj;
    DeviceState *bic;
    SGIBICBoardInterfaceState *board_interface;
    uint8_t board_attached;
    DeviceState *chip[3];
    SGIXioLink host_link;
    SGIXioLink head_link[4];
    SN1FabricEndpoint endpoint;
    uint8_t ni_route;
    uint8_t intx_enabled;
    uint8_t intx_map[SGI_ORIGIN300_PCI_HEADS * 2 * PCI_NUM_PINS];
    uint32_t intx_raw[SGI_ORIGIN300_PCI_HEADS];
    uint8_t intx_levels[SGI_ORIGIN300_PCI_HEADS];
    qemu_irq *intx_inputs;
    qemu_irq intx_outputs[SGI_ORIGIN300_PCI_HEADS][SGI_XBRIDGE_HEAD_PINS];
    bool ni_initialized;
    AddressSpace bic_registers;
    AddressSpace bic_pio;
    MemoryRegion denied;
    MemoryRegion view[12];
    uint32_t placement;
    uint8_t node_number;
    uint8_t bedrock_revision;
    uint8_t xbridge_revision;
    uint8_t port[4];
    char *bus_prefix;
};

static MemTxResult expansion_denied_read(void *opaque, hwaddr addr,
                                         uint64_t *data, unsigned size,
                                         MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-pci-expansion-module: %u-byte DMA "
                  "read at 0x%" HWADDR_PRIx " has no NI data route\n",
                  size, addr);
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult expansion_denied_write(void *opaque, hwaddr addr,
                                          uint64_t data, unsigned size,
                                          MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-pci-expansion-module: %u-byte DMA "
                  "write at 0x%" HWADDR_PRIx " has no NI data route\n",
                  size, addr);
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps expansion_denied_ops = {
    .read_with_attrs = expansion_denied_read,
    .write_with_attrs = expansion_denied_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void expansion_view(SGIOrigin300PciExpansionState *s, unsigned index,
                            MemoryRegion *target, uint64_t size)
{
    g_autofree char *name = g_strdup_printf("pci-expansion.view%u", index);

    memory_region_init_alias(&s->view[index], OBJECT(s), name, target, 0, size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->view[index]);
}

/* This structural endpoint installs no NI address claims. */
static void expansion_place_claims(SN1FabricEndpoint *endpoint)
{
}

bool sgi_origin300_pci_expansion_attach_board_interface(
    SGIOrigin300PciExpansionState *s, SGIBICBoardInterfaceState *interface,
    Error **errp)
{
    if (DEVICE(s)->realized || s->board_interface || !interface ||
        !DEVICE(interface)->realized) {
        error_setg(errp, "BIC attachment needs a realized interface and must "
                   "be selected once before assembly realization");
        return false;
    }
    object_ref(OBJECT(interface));
    s->board_interface = interface;
    s->board_attached = true;
    object_property_add_const_link(OBJECT(s), "board-interface",
                                   OBJECT(interface));
    return true;
}

bool sgi_origin300_pci_expansion_enable_ni_route(
    SGIOrigin300PciExpansionState *s, Error **errp)
{
    if (DEVICE(s)->realized) {
        error_setg(errp, "NI composition must be selected before realization");
        return false;
    }
    s->ni_route = true;
    return true;
}

bool sgi_origin300_pci_expansion_set_intx_map(
    SGIOrigin300PciExpansionState *s, const SGIOrigin300PciIntxMap *map,
    Error **errp)
{
    if (DEVICE(s)->realized || s->intx_enabled) {
        error_setg(errp, "PCI INTx board map must be selected once before "
                   "realization");
        return false;
    }
    for (unsigned head = 0; head < SGI_ORIGIN300_PCI_HEADS; head++) {
        for (unsigned device = 0; device < 2; device++) {
            for (unsigned pin = 0; pin < PCI_NUM_PINS; pin++) {
                if (map->pin[head][device][pin] >= SGI_XBRIDGE_HEAD_PINS) {
                    error_setg(errp, "PCI INTx board map head %u device %u "
                               "pin %u has no head input %u", head,
                               device + 1, pin, map->pin[head][device][pin]);
                    return false;
                }
            }
        }
    }
    memcpy(s->intx_map, map->pin, sizeof(s->intx_map));
    s->intx_enabled = 1;
    return true;
}

static uint8_t expansion_intx_levels(SGIOrigin300PciExpansionState *s,
                                     unsigned head)
{
    uint8_t levels = 0;

    for (unsigned device = 1; device <= 2; device++) {
        for (unsigned pin = 0; pin < PCI_NUM_PINS; pin++) {
            unsigned raw = device * PCI_NUM_PINS + pin;
            unsigned index = (head * 2 + device - 1) * PCI_NUM_PINS + pin;

            if (s->intx_raw[head] & (1U << raw)) {
                levels |= 1U << s->intx_map[index];
            }
        }
    }
    return levels;
}

static void expansion_intx(void *opaque, int line, int level)
{
    SGIOrigin300PciExpansionState *s = opaque;
    unsigned head = line / SGI_XBRIDGE_HEAD_RAW_PCI_IRQS;
    unsigned raw = line % SGI_XBRIDGE_HEAD_RAW_PCI_IRQS;
    unsigned device = raw / PCI_NUM_PINS;
    uint8_t levels, changed;

    s->intx_raw[head] = (s->intx_raw[head] & ~(1U << raw)) |
                       (level ? 1U << raw : 0);
    if (device != 1 && device != 2) {
        if (level) {
            qemu_log_mask(LOG_UNIMP, "sgi-pci-expansion-module: PCI head %u "
                          "device index %u has no experiment INTx map\n",
                          head, device);
        }
        return;
    }
    levels = expansion_intx_levels(s, head);
    changed = levels ^ s->intx_levels[head];
    s->intx_levels[head] = levels;
    for (unsigned pin = 0; pin < SGI_XBRIDGE_HEAD_PINS; pin++) {
        if (changed & (1U << pin)) {
            qemu_set_irq(s->intx_outputs[head][pin], !!(levels & (1U << pin)));
        }
    }
}

static void expansion_intx_clear(SGIOrigin300PciExpansionState *s)
{
    memset(s->intx_raw, 0, sizeof(s->intx_raw));
    memset(s->intx_levels, 0, sizeof(s->intx_levels));
    if (s->intx_inputs) {
        for (unsigned head = 0; head < SGI_ORIGIN300_PCI_HEADS; head++) {
            for (unsigned pin = 0; pin < SGI_XBRIDGE_HEAD_PINS; pin++) {
                qemu_set_irq(s->intx_outputs[head][pin], 0);
            }
        }
    }
}

static void expansion_reset_hold(Object *obj, ResetType type)
{
    expansion_intx_clear(SGI_ORIGIN300_PCI_EXPANSION(obj));
}

static void expansion_unrealize(DeviceState *dev)
{
    SGIOrigin300PciExpansionState *s = SGI_ORIGIN300_PCI_EXPANSION(dev);

    expansion_intx_clear(s);
    if (s->intx_inputs) {
        for (unsigned head = 0; head < SGI_ORIGIN300_PCI_HEADS; head++) {
            const char *name = head % 2 ? "head-f-pci-intx" : "head-e-pci-intx";

            for (unsigned raw = 0; raw < SGI_XBRIDGE_HEAD_RAW_PCI_IRQS; raw++) {
                qdev_connect_gpio_out_named(s->chip[head / 2], name, raw, NULL);
            }
        }
        qemu_free_irqs(s->intx_inputs,
                      SGI_ORIGIN300_PCI_HEADS * SGI_XBRIDGE_HEAD_RAW_PCI_IRQS);
        s->intx_inputs = NULL;
    }
}

static int expansion_post_load(void *opaque, int version)
{
    SGIOrigin300PciExpansionState *s = opaque;

    if (version < 3 && s->board_attached) {
        return -EINVAL;
    }
    if (version < 2 && s->intx_enabled) {
        return -EINVAL;
    }
    if (s->intx_enabled) {
        for (unsigned head = 0; head < SGI_ORIGIN300_PCI_HEADS; head++) {
            uint8_t levels = expansion_intx_levels(s, head);

            if (levels != s->intx_levels[head]) {
                return -EINVAL;
            }
            /* Migrated head pins suppress repeated packets on equal levels. */
            for (unsigned pin = 0; pin < SGI_XBRIDGE_HEAD_PINS; pin++) {
                qemu_set_irq(s->intx_outputs[head][pin],
                             !!(levels & (1U << pin)));
            }
        }
    }
    return 0;
}

static MemTxResult expansion_processor_rw(SN1FabricEndpoint *endpoint,
    unsigned origin, hwaddr address, void *buffer, size_t size, bool write,
    MemTxAttrs attrs)
{
    SGIOrigin300PciExpansionState *s =
        SGI_ORIGIN300_PCI_EXPANSION(endpoint->owner);
    hwaddr base = (uint64_t)s->placement << SN1_FABRIC_ADDRESS_SHIFT;
    hwaddr local = address - base;
    hwaddr offset;
    AddressSpace *as;

    if (local >= UINT64_C(0x30034000000) &&
        local < UINT64_C(0x30036000000)) {
        uint64_t value = 0;
        MemTxResult result;

        if (!s->board_interface || size > 8) {
            return MEMTX_DECODE_ERROR;
        }
        offset = local - UINT64_C(0x30000000000);
        if (write) {
            switch (size) {
            case 1:
                value = *(uint8_t *)buffer;
                break;
            case 2:
                value = lduw_be_p(buffer);
                break;
            case 4:
                value = ldl_be_p(buffer);
                break;
            case 8:
                value = ldq_be_p(buffer);
                break;
            }
            return sgi_bic_board_write(s->board_interface, offset, size, value);
        }
        result = sgi_bic_board_read(s->board_interface, offset, size, &value);
        if (result == MEMTX_OK) {
            if (size == 8) {
                stq_be_p(buffer, value);
            } else {
                stw_be_p(buffer, value);
            }
        }
        return result;
    }
    if ((local >= UINT64_C(0x10030000000) &&
         local - UINT64_C(0x10030000000) < SGI_BEDROCK_SIZE) ||
        (local >= UINT64_C(0x20001800000) &&
         local - UINT64_C(0x20001800000) < SGI_BEDROCK_SIZE)) {
        offset = local >= UINT64_C(0x20001800000) ?
            local - UINT64_C(0x20001800000) : local - UINT64_C(0x10030000000);
        /* BIC has no PI. Its LB/NI/II identity and controls remain readable. */
        if (offset < 0x400000 || size > SGI_BEDROCK_SIZE - offset) {
            return MEMTX_DECODE_ERROR;
        }
        as = &s->bic_registers;
    } else if (local >= UINT64_C(0x20000000000) &&
               local - UINT64_C(0x20000000000) < SGI_BEDROCK_IO_PIO_SIZE) {
        offset = local - UINT64_C(0x20000000000);
        if (size > SGI_BEDROCK_IO_PIO_SIZE - offset) {
            return MEMTX_DECODE_ERROR;
        }
        as = &s->bic_pio;
    } else {
        return MEMTX_DECODE_ERROR;
    }
    return write ? address_space_write(as, offset, attrs, buffer, size) :
                   address_space_read(as, offset, attrs, buffer, size);
}

static SGIBedrockIOState *expansion_io_claimant(void *opaque, unsigned number)
{
    SN1FabricEndpoint *target = sn1_fabric_resolve_named(opaque, number);

    if (!target) {
        qemu_log_mask(LOG_GUEST_ERROR,
            "sgi-pci-expansion-module: NI register packet has no node %u\n",
            number);
    }
    return target ? &target->bedrock->io : NULL;
}

static void expansion_realize(DeviceState *dev, Error **errp)
{
    SGIOrigin300PciExpansionState *s = SGI_ORIGIN300_PCI_EXPANSION(dev);
    SGIBedrockState *bic;
    static const unsigned widgets[] = { 0, 8, 9, 0xc, 0xd, 0xe, 0xf };
    unsigned i, j;

    if (s->bedrock_revision != 2 || s->xbridge_revision != 2) {
        error_setg(errp, "PCI expansion requires explicit inferred revision "
                   "2 for Bedrock and XBridge");
        return;
    }
    if (s->placement >= 128 || s->node_number >= 128) {
        error_setg(errp, "PCI expansion needs explicit placement and node "
                   "number below 128");
        return;
    }
    if (!s->bus_prefix || !s->bus_prefix[0]) {
        error_setg(errp, "PCI expansion needs a bus-prefix for its six buses");
        return;
    }
    for (i = 0; i < 4; i++) {
        if (s->port[i] < 8 || s->port[i] > 0xd || s->port[i] == 0xa ||
            s->port[i] == 0xb) {
            error_setg(errp, "PCI expansion head link %u needs an explicit "
                       "U0 port from 8, 9, c or d", i);
            return;
        }
        for (j = 0; j < i; j++) {
            if (s->port[i] == s->port[j]) {
                error_setg(errp, "PCI expansion head links share U0 port %x",
                           s->port[i]);
                return;
            }
        }
    }

    memory_region_init_io(&s->denied, OBJECT(s), &expansion_denied_ops, s,
                          "pci-expansion.no-ni-route", UINT64_MAX);
    s->bic = qdev_new(TYPE_SGI_BEDROCK);
    object_property_add_child(OBJECT(s), "bic", OBJECT(s->bic));
    bic = SGI_BEDROCK(s->bic);
    qdev_prop_set_uint32(s->bic, "processor-count", 0);
    qdev_prop_set_uint32(s->bic, "processor-mask", 0);
    qdev_prop_set_uint32(s->bic, "cpu-enable-initial-mask", 0);
    qdev_prop_set_uint8(s->bic, "installed-bank-mask", 0);
    qdev_prop_set_uint8(s->bic, "revision", s->bedrock_revision);
    qdev_prop_set_uint16(s->bic, "inbound-widget-mask", 0xf300);
    qdev_prop_set_uint32(DEVICE(&bic->lb), "node-number", s->node_number);
    if (!object_property_set_link(OBJECT(s->bic), "dma-memory",
                                  OBJECT(&s->denied), errp)) {
        return;
    }
    for (i = 0; i < 3; i++) {
        g_autofree char *name = g_strdup_printf("u%u", i);
        g_autofree char *prefix = g_strdup_printf("%su%u-", s->bus_prefix, i);

        s->chip[i] = qdev_new(TYPE_SGI_XBRIDGE);
        object_property_add_child(OBJECT(s), name, OBJECT(s->chip[i]));
        if (s->ni_route && !sgi_xbridge_enable_packet_context(
                SGI_XBRIDGE(s->chip[i]), errp)) {
            return;
        }
        qdev_prop_set_string(s->chip[i], "bus-prefix", prefix);
        qdev_prop_set_uint8(s->chip[i], "pci-speed-e", 1);
        qdev_prop_set_uint8(s->chip[i], "pci-speed-f", 1);
        qdev_prop_set_bit(s->chip[i], "superbridge", i != 0);
    }
    qdev_prop_set_uint8(s->chip[0], "host-port", 0xa);
    sgi_xio_link_init(&s->host_link, OBJECT(s), "bic-u0",
                      OBJECT(&bic->io), 0, OBJECT(s->chip[0]), 0xa);
    if (!sgi_xio_link_connect(&s->host_link, errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(s->chip[0]), errp)) {
        return;
    }
    for (i = 0; i < 4; i++) {
        g_autofree char *name = g_strdup_printf("u0-head%u", i);

        sgi_xio_link_init(&s->head_link[i], OBJECT(s), name,
                          OBJECT(s->chip[0]), s->port[i],
                          OBJECT(s->chip[1 + i / 2]), 0xe + i % 2);
        if (!sgi_xio_link_connect(&s->head_link[i], errp)) {
            return;
        }
    }
    for (i = 1; i < 3; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(s->chip[i]), errp)) {
            return;
        }
    }
    if (s->intx_enabled) {
        s->intx_inputs = qemu_allocate_irqs(expansion_intx, s,
            SGI_ORIGIN300_PCI_HEADS * SGI_XBRIDGE_HEAD_RAW_PCI_IRQS);
        for (i = 0; i < SGI_ORIGIN300_PCI_HEADS; i++) {
            const char *input = i % 2 ? "head-f-pin" : "head-e-pin";
            const char *output = i % 2 ? "head-f-pci-intx" : "head-e-pci-intx";

            for (j = 0; j < SGI_XBRIDGE_HEAD_PINS; j++) {
                s->intx_outputs[i][j] = qdev_get_gpio_in_named(
                    s->chip[i / 2], input, j);
            }
            for (j = 0; j < SGI_XBRIDGE_HEAD_RAW_PCI_IRQS; j++) {
                qdev_connect_gpio_out_named(s->chip[i / 2], output, j,
                    s->intx_inputs[i * SGI_XBRIDGE_HEAD_RAW_PCI_IRQS + j]);
            }
        }
    }
    for (i = 0; i < ARRAY_SIZE(widgets); i++) {
        unsigned widget = widgets[i];
        g_autofree char *name = g_strdup_printf("widget-%u", widget);
        MemoryRegion *face = sgi_xbridge_widget_view(
            SGI_XBRIDGE(s->chip[0]), widget);

        if (!object_property_set_link(OBJECT(s->bic), name, OBJECT(face),
                                      errp)) {
            return;
        }
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->bic), errp)) {
        return;
    }
    s->endpoint = (SN1FabricEndpoint) {
        .owner = OBJECT(s),
        .bedrock = bic,
        .placement = s->placement,
        .ready = true,
        .place_claims = expansion_place_claims,
    };
    if (s->ni_route) {
        s->endpoint.isolated_route = true;
        s->endpoint.processor_rw = expansion_processor_rw;
        address_space_init(&s->bic_registers,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(s->bic), 0),
            "pci-expansion.bic-registers");
        address_space_init(&s->bic_pio,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(s->bic), 1),
            "pci-expansion.bic-pio");
        s->ni_initialized = true;
        sgi_bedrock_io_set_claimant(&bic->io, expansion_io_claimant,
                                    &s->endpoint);
        sgi_bedrock_io_set_destination(&bic->io, sn1_fabric_destination_rw,
                                      &s->endpoint);
    }
    expansion_view(s, 0, sysbus_mmio_get_region(SYS_BUS_DEVICE(s->bic), 0),
                   SGI_BEDROCK_SIZE);
    expansion_view(s, 1, sysbus_mmio_get_region(SYS_BUS_DEVICE(s->bic), 1),
                   memory_region_size(sysbus_mmio_get_region(
                       SYS_BUS_DEVICE(s->bic), 1)));
    expansion_view(s, 2, &s->denied, 0x10000);
    for (i = 0; i < SGI_ORIGIN300_PCI_HEADS; i++) {
        MemoryRegion *face = sgi_xbridge_widget_view(
            SGI_XBRIDGE(s->chip[i / 2]), 0xe + i % 2);

        expansion_view(s, 3 + i, face, memory_region_size(face));
    }
    for (i = 1; i < 3; i++) {
        MemoryRegion *face = sysbus_mmio_get_region(
            SYS_BUS_DEVICE(s->chip[i]), 1);

        expansion_view(s, 8 + i, face, memory_region_size(face));
    }
    expansion_view(s, 11,
                   sgi_xbridge_widget_view(SGI_XBRIDGE(s->chip[0]), 0),
                   0x1000000);
}

PCIBus *sgi_origin300_pci_expansion_bus(SGIOrigin300PciExpansionState *s,
                                       unsigned head)
{
    if (!DEVICE(s)->realized || head >= SGI_ORIGIN300_PCI_HEADS) {
        return NULL;
    }
    return sgi_xbridge_pci_bus(SGI_XBRIDGE(s->chip[head / 2]), 0xe + head % 2);
}

static SN1FabricEndpoint *expansion_endpoint(Object *owner)
{
    return &SGI_ORIGIN300_PCI_EXPANSION(owner)->endpoint;
}

static void expansion_finalize(Object *obj)
{
    SGIOrigin300PciExpansionState *s = SGI_ORIGIN300_PCI_EXPANSION(obj);
    unsigned i;

    if (s->ni_initialized) {
        address_space_destroy(&s->bic_pio);
        address_space_destroy(&s->bic_registers);
    }
    if (s->board_interface) {
        object_unref(OBJECT(s->board_interface));
    }
    g_free(s->host_link.name);
    for (i = 0; i < 4; i++) {
        g_free(s->head_link[i].name);
    }
}

static const Property expansion_properties[] = {
    DEFINE_PROP_UINT32("placement", SGIOrigin300PciExpansionState,
                       placement, UINT32_MAX),
    DEFINE_PROP_UINT8("node-number", SGIOrigin300PciExpansionState,
                      node_number, UINT8_MAX),
    DEFINE_PROP_UINT8("bedrock-revision", SGIOrigin300PciExpansionState,
                      bedrock_revision, 0),
    DEFINE_PROP_UINT8("xbridge-revision", SGIOrigin300PciExpansionState,
                      xbridge_revision, 0),
    DEFINE_PROP_STRING("bus-prefix", SGIOrigin300PciExpansionState, bus_prefix),
    DEFINE_PROP_UINT8("chip1-e-port", SGIOrigin300PciExpansionState,
                      port[0], 0),
    DEFINE_PROP_UINT8("chip1-f-port", SGIOrigin300PciExpansionState,
                      port[1], 0),
    DEFINE_PROP_UINT8("chip2-e-port", SGIOrigin300PciExpansionState,
                      port[2], 0),
    DEFINE_PROP_UINT8("chip2-f-port", SGIOrigin300PciExpansionState,
                      port[3], 0),
};

static const VMStateDescription vmstate_expansion = {
    .name = TYPE_SGI_ORIGIN300_PCI_EXPANSION,
    .version_id = 3,
    .minimum_version_id = 1,
    .post_load = expansion_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SINGLE(board_attached, SGIOrigin300PciExpansionState, 3,
                       vmstate_info_uint8_equal, uint8_t),
        VMSTATE_UINT8_EQUAL(ni_route, SGIOrigin300PciExpansionState),
        VMSTATE_UINT32_EQUAL(placement, SGIOrigin300PciExpansionState),
        VMSTATE_UINT8_EQUAL(node_number, SGIOrigin300PciExpansionState),
        VMSTATE_UINT8_EQUAL(bedrock_revision, SGIOrigin300PciExpansionState),
        VMSTATE_UINT8_EQUAL(xbridge_revision, SGIOrigin300PciExpansionState),
        VMSTATE_ARRAY(port, SGIOrigin300PciExpansionState, 4, 0,
                      vmstate_info_uint8_equal, uint8_t),
        VMSTATE_SINGLE(intx_enabled, SGIOrigin300PciExpansionState, 2,
                       vmstate_info_uint8_equal, uint8_t),
        VMSTATE_ARRAY(intx_map, SGIOrigin300PciExpansionState,
                      SGI_ORIGIN300_PCI_HEADS * 2 * PCI_NUM_PINS, 2,
                      vmstate_info_uint8_equal, uint8_t),
        VMSTATE_UINT32_ARRAY_V(intx_raw, SGIOrigin300PciExpansionState,
                              SGI_ORIGIN300_PCI_HEADS, 2),
        VMSTATE_UINT8_ARRAY_V(intx_levels, SGIOrigin300PciExpansionState,
                             SGI_ORIGIN300_PCI_HEADS, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void expansion_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SN1FabricClass *fc = SGI_SN1_FABRIC_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "Origin 300 PCI expansion, inferred revision 2, "
               "no NI data route";
    dc->realize = expansion_realize;
    dc->unrealize = expansion_unrealize;
    rc->phases.hold = expansion_reset_hold;
    dc->vmsd = &vmstate_expansion;
    dc->user_creatable = false;
    device_class_set_props(dc, expansion_properties);
    fc->endpoint = expansion_endpoint;
}

static const TypeInfo expansion_type = {
    .name = TYPE_SGI_ORIGIN300_PCI_EXPANSION,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIOrigin300PciExpansionState),
    .instance_finalize = expansion_finalize,
    .class_init = expansion_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_SGI_SN1_FABRIC },
        { }
    },
};

static void expansion_register_types(void)
{
    type_register_static(&expansion_type);
}

type_init(expansion_register_types)
