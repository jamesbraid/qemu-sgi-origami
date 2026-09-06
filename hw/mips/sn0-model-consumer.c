/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Construct the product devices declared by a compiled SN0 model. */

#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/scsi/qlogic-isp.h"
#include "net/net.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/system.h"
#include "sn0.h"

/* Preserve the CPU's byte order across the nested Bridge dispatch. */
#define SN0_PROM_PI_PROBE_MEMOP(size) (size_memop(size) | MO_BE)

static MemTxResult sn0_prom_pi_probe_read(void *opaque, hwaddr addr,
                                          uint64_t *data, unsigned int size,
                                          MemTxAttrs attrs)
{
    SN0NodeState *node = opaque;
    const SN0ModelComponent *base_io = node->topology->base_io;

    if (addr == 0 && size == 8) {
        *data = 0;
        sgi_hub_latch_prom_pi_error(&node->hub);
        return MEMTX_ERROR;
    }
    return memory_region_dispatch_read(
        &node->prom_pi_probe_bridge->
            device_window_mem[node->prom_pi_probe_device_window],
        base_io->prom_pi_probe_window_offset + addr, data,
        SN0_PROM_PI_PROBE_MEMOP(size), attrs);
}

static MemTxResult sn0_prom_pi_probe_write(void *opaque, hwaddr addr,
                                           uint64_t data, unsigned int size,
                                           MemTxAttrs attrs)
{
    SN0NodeState *node = opaque;
    const SN0ModelComponent *base_io = node->topology->base_io;

    return memory_region_dispatch_write(
        &node->prom_pi_probe_bridge->
            device_window_mem[node->prom_pi_probe_device_window],
        base_io->prom_pi_probe_window_offset + addr, data,
        SN0_PROM_PI_PROBE_MEMOP(size), attrs);
}

static const MemoryRegionOps sn0_prom_pi_probe_ops = {
    .read_with_attrs = sn0_prom_pi_probe_read,
    .write_with_attrs = sn0_prom_pi_probe_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

void sn0_model_configure_diagnostic(SN0MachineState *s,
                                    unsigned int node_index)
{
    const SN0MachineModel *model = SN0_MACHINE_GET_CLASS(s)->model;
    SN0NodeState *node = s->nodes[node_index];

    qdev_prop_set_bit(DEVICE(&node->hub), "junk-uart",
                      node_index == model->junk_uart_node &&
                      s->junk_uart_present);
    if (node_index == model->junk_uart_node && s->junk_uart_present &&
        serial_hd(SN0_NODE_SERIAL(model->junk_uart_node,
                                  SN0_JUNK_UART_CHARDEV))) {
        object_property_set_link(
            OBJECT(&node->hub), "junk-uart-chardev",
            OBJECT(serial_hd(SN0_NODE_SERIAL(model->junk_uart_node,
                                             SN0_JUNK_UART_CHARDEV))),
            &error_fatal);
    }
}

static uint32_t sn0_model_ioc3_population(
    const SN0ModelIoc3Population *population)
{
    uint32_t mask = 0;

    mask |= population->ethernet ? SGI_IOC3_POPULATION_ETHERNET : 0;
    mask |= population->serial_a ? SGI_IOC3_POPULATION_SERIAL_A : 0;
    mask |= population->serial_b ? SGI_IOC3_POPULATION_SERIAL_B : 0;
    mask |= population->keyboard_mouse ?
        SGI_IOC3_POPULATION_KEYBOARD_MOUSE : 0;
    mask |= population->parallel ? SGI_IOC3_POPULATION_PARALLEL : 0;
    mask |= population->onewire_identity ?
        SGI_IOC3_POPULATION_ONEWIRE_IDENTITY : 0;
    return mask;
}

static uint8_t sn0_model_ioc3_timekeeper(bool present)
{
    return present ? SGI_IOC3_TIMEKEEPER_M48T35 :
        SGI_IOC3_TIMEKEEPER_NONE;
}

static PCIDevice *sn0_model_create_ioc3(SN0MachineState *s,
                                        unsigned int node_index,
                                        const SN0ModelComponent *function)
{
    const SN0ModelIoc3Population *population =
        &s->nodes[node_index]->topology->ioc3_population;
    PCIDevice *ioc3 = pci_new(PCI_DEVFN(function->device,
                                        function->function),
                              TYPE_SGI_IOC3);
    Chardev *ioc3_a;
    Chardev *ioc3_b;

    /* A child of the node that carries the board, not of the machine. */
    object_property_add_child(OBJECT(s->nodes[node_index]), "ioc3",
                              OBJECT(ioc3));

    ioc3_a = serial_hd(SN0_NODE_SERIAL(node_index,
                                      SN0_IOC3_SERIAL_A_CHARDEV));
    ioc3_b = serial_hd(SN0_NODE_SERIAL(node_index,
                                      SN0_IOC3_SERIAL_B_CHARDEV));
    if (ioc3_a) {
        qdev_prop_set_chr(DEVICE(ioc3), "chardev-a", ioc3_a);
    }
    if (ioc3_b) {
        qdev_prop_set_chr(DEVICE(ioc3), "chardev-b", ioc3_b);
    }
    if (node_index < MAX_PARALLEL_PORTS && parallel_hds[node_index]) {
        qdev_prop_set_chr(DEVICE(ioc3), "parallel-chardev",
                          parallel_hds[node_index]);
    }

    qdev_prop_set_uint16(DEVICE(ioc3), "subsystem-id",
                         function->subsystem_id);
    qdev_prop_set_uint32(DEVICE(ioc3), "population",
                         sn0_model_ioc3_population(population));
    qdev_prop_set_uint8(DEVICE(ioc3), "timekeeper-kind",
                        sn0_model_ioc3_timekeeper(population->timekeeper));
    qdev_prop_set_uint64(DEVICE(ioc3), "onewire-serial",
                         s->nodes[node_index]->instance->ioc3_serial);

    {
        g_autofree char *nvram_id = g_strdup_printf("sgi-nvram%u", node_index);
        DriveInfo *nvram_dinfo = drive_get(IF_MTD, 0, node_index);
        BlockBackend *nvram_blk = nvram_dinfo ?
            blk_by_legacy_dinfo(nvram_dinfo) : blk_by_name(nvram_id);

        if (nvram_blk) {
            qdev_prop_set_drive(DEVICE(ioc3), "nvram-drive", nvram_blk);
        }
    }

    pci_realize_and_unref(ioc3,
                          PCI_HOST_BRIDGE(s->io_bridge[node_index])->bus,
                          &error_fatal);
    pci_set_byte(ioc3->config + PCI_REVISION_ID, function->revision);

    /* Board wiring, not PCI INTx. */
    qdev_connect_gpio_out(DEVICE(ioc3), 0,
                          qdev_get_gpio_in(DEVICE(s->io_bridge[node_index]),
                                           function->irq_input));
    return ioc3;
}

static PCIDevice *sn0_model_create_isp(SN0MachineState *s,
                                       unsigned int node_index,
                                       const SN0ModelComponent *function,
                                       unsigned int instance)
{
    g_autofree char *name = g_strdup_printf("isp%u", instance);
    PCIDevice *isp;

    isp = pci_new(PCI_DEVFN(function->device, function->function),
                  "qlogic-isp1040");
    object_property_add_child(OBJECT(s->nodes[node_index]), name,
                              OBJECT(isp));
    pci_realize_and_unref(isp,
                          PCI_HOST_BRIDGE(s->io_bridge[node_index])->bus,
                          &error_fatal);
    pci_set_byte(isp->config + PCI_REVISION_ID, function->revision);
    qlogic_isp_handle_legacy_cmdline(DEVICE(isp));
    return isp;
}

static void sn0_model_create_bridge(SN0MachineState *s,
                                    unsigned int node_index)
{
    SN0NodeState *node = s->nodes[node_index];
    const SN0ModelComponent *base_io = node->topology->base_io;
    const SN0ModelAddressMap *map = &node->topology->map;
    const SN0ModelAddressSpace *xtalk = &map->xtalk;
    const SN0ModelRegion *bridge_region = &map->bridge_registers;
    MemoryRegion *bridge_face;
    SGIBridgeState *bridge;
    g_autofree char *bridge_name = node_index == 0 ?
        g_strdup("bridge") : g_strdup_printf("bridge%u", node_index);
    g_autofree char *bus_name = node_index == 0 ?
        g_strdup("pci") : g_strdup_printf("pci.%u", node_index);
    g_autofree char *widget0_name = node_index == 0 ?
        g_strdup("sn0.xtalk.widget0") :
        g_strdup_printf("sn0.node%u.xtalk.widget0", node_index);

    bridge = SGI_BRIDGE(object_new(TYPE_SGI_BRIDGE));
    object_property_add_child(OBJECT(s), bridge_name, OBJECT(bridge));
    object_unref(OBJECT(bridge));
    s->io_bridge[node_index] = bridge;
    qdev_prop_set_uint32(DEVICE(bridge), "revision",
                         base_io->revision);
    qdev_prop_set_uint64(DEVICE(bridge), "onewire-serial",
                         node->instance->base_io_serial);
    qdev_prop_set_string(DEVICE(bridge), "board-part",
                         base_io->part);
    qdev_prop_set_string(DEVICE(bridge), "pci-bus-name", bus_name);
    /*
     * Interrupt packets are real transfers, not calls into the Hub, so they
     * travel the route a processor of this node travels: its claim view,
     * where a node-qualified address resolves by programmed number. The
     * canonical map resolves the same address by the board's position
     * instead, which agrees only while every board's programmed number
     * happens to equal its position. Node zero satisfies that by accident
     * and node one does not, so before firmware hands out numbers -- both
     * Hubs still reading zero -- node one's packet for "node zero" landed
     * on node zero's Hub instead of staying home.
     *
     * Every processor's view of a node claims the same strides, so view
     * zero speaks for the node; a node always has a processor.
     */
    object_property_set_link(OBJECT(bridge), "fabric",
                             OBJECT(&node->claim_view[0]), &error_fatal);
    qdev_prop_set_uint64(DEVICE(bridge), "fabric-base", xtalk->carrier_base);
    qdev_prop_set_uint32(DEVICE(bridge), "fabric-node-shift",
                         ctz64(map->node_stride));

    sysbus_realize(SYS_BUS_DEVICE(bridge), &error_fatal);
    bridge_face = sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0);
    g_assert(memory_region_size(bridge_face) == bridge_region->size);

    memory_region_init_alias(&s->widget0[node_index], OBJECT(s), widget0_name,
                             bridge_face, 0, bridge_region->size);
    memory_region_add_subregion(&node->xtalk,
                                SN0_XTALK_BASE(base_io->discovery_widget),
                                &s->widget0[node_index]);
}

void sn0_model_populate_node(SN0MachineState *s, unsigned int node_index)
{
    SN0NodeState *node = s->nodes[node_index];
    const SN0ModelComponent *base_io = node->topology->base_io;
    const SN0ModelRegion *bridge = &node->topology->map.bridge_registers;
    MemoryRegion *bridge_face;
    unsigned int scsi_index = 0;
    unsigned int device_window;
    unsigned int widget;
    size_t component_index;

    sn0_model_create_bridge(s, node_index);
    s->pci_devices[node_index] =
        g_new0(PCIDevice *, node->topology->pci_count);

    for (component_index = 0;
         component_index < node->topology->pci_count; component_index++) {
        const SN0ModelComponent *component =
            &node->topology->pci[component_index];

        switch (component->kind) {
        case SN0_COMPONENT_ISP1040:
            s->pci_devices[node_index][component_index] =
                sn0_model_create_isp(s, node_index, component, scsi_index++);
            break;
        case SN0_COMPONENT_IOC3:
            s->pci_devices[node_index][component_index] =
                sn0_model_create_ioc3(s, node_index, component);
            break;
        default:
            g_assert_not_reached();
        }
    }

    bridge_face = sysbus_mmio_get_region(
        SYS_BUS_DEVICE(s->io_bridge[node_index]), 0);
    memory_region_add_subregion_overlap(
        &node->xtalk, SN0_XTALK_BASE(base_io->widget), bridge_face, -1);

    /* No crossbar: alias the bridge's face at every free widget number. */
    for (widget = 0; widget < SN0_XTALK_WIDGETS; widget++) {
        g_autofree char *name = NULL;

        if (!(base_io->decoded_widget_mask & (1u << widget)) ||
            widget == base_io->discovery_widget ||
            widget == base_io->widget) {
            continue;
        }

        name = g_strdup_printf("sn0.node%u.xtalk.widget%u",
                               node_index, widget);
        memory_region_init_alias(&s->widget_alias[node_index][widget],
                                 OBJECT(s), name, bridge_face, 0,
                                 bridge->size);
        memory_region_add_subregion(&node->xtalk, SN0_XTALK_BASE(widget),
                                    &s->widget_alias[node_index][widget]);
    }

    memory_region_init_io(&node->prom_pi_probe, OBJECT(node),
                          &sn0_prom_pi_probe_ops, node,
                          "sn0.prom-pi-probe", 8);
    node->prom_pi_probe_bridge = s->io_bridge[node_index];
    for (device_window = 0; device_window < SGI_BRIDGE_DEVICES;
         device_window++) {
        MemoryRegion *window = &node->prom_pi_probe_bridge->
                                   device_window_mem[device_window];

        if (base_io->prom_pi_probe_offset >= window->addr &&
            base_io->prom_pi_probe_offset - window->addr <
                memory_region_size(window)) {
            break;
        }
    }
    g_assert(device_window < SGI_BRIDGE_DEVICES);
    g_assert(base_io->prom_pi_probe_offset -
             node->prom_pi_probe_bridge->
                 device_window_mem[device_window].addr ==
             base_io->prom_pi_probe_window_offset);
    node->prom_pi_probe_device_window = device_window;
    memory_region_add_subregion_overlap(
        &node->xtalk,
        SN0_XTALK_BASE(base_io->prom_pi_probe_widget) +
        base_io->prom_pi_probe_offset,
        &node->prom_pi_probe, 1);
}
