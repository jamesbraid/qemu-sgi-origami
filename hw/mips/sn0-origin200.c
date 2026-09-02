/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 200: the one- or two-chassis SN0 product on the IP29 board.
 * The family machinery this parameterizes lives in sn0.c.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-ioc3.h"
#include "net/net.h"
#include "system/system.h"
#include "system/block-backend.h"
#include "system/address-spaces.h"
#include "sn0.h"
#include "hw/scsi/scsi.h"
#include "hw/scsi/qlogic-isp.h"
#include "system/blockdev.h"

#define SN0_PROM_PI_PROBE_WIDGET 2
#define SN0_PROM_PI_PROBE_OFFSET 0x400208
#define SN0_PROM_PI_PROBE_WINDOW_OFFSET 0x208

/* Preserve the CPU's byte order across the nested Bridge dispatch. */
#define SN0_PROM_PI_PROBE_MEMOP(size) (size_memop(size) | MO_BE)

static char origin200_ioc3_subsystem_id[sizeof("0xffff")];
static GlobalProperty origin200_compat[] = {
    {
        .driver = TYPE_SGI_IOC3,
        .property = "subsystem-id",
        .value = origin200_ioc3_subsystem_id,
    },
};

static MemTxResult sn0_prom_pi_probe_read(void *opaque, hwaddr addr,
                                          uint64_t *data, unsigned size,
                                          MemTxAttrs attrs)
{
    SN0NodeState *node = opaque;

    if (addr == 0 && size == 8) {
        *data = 0;
        sgi_hub_latch_prom_pi_error(&node->hub);
        return MEMTX_ERROR;
    }
    return memory_region_dispatch_read(
        &node->prom_pi_probe_bridge->device_window_mem[1],
        SN0_PROM_PI_PROBE_WINDOW_OFFSET + addr, data,
        SN0_PROM_PI_PROBE_MEMOP(size), attrs);
}

static MemTxResult sn0_prom_pi_probe_write(void *opaque, hwaddr addr,
                                           uint64_t data, unsigned size,
                                           MemTxAttrs attrs)
{
    SN0NodeState *node = opaque;
    return memory_region_dispatch_write(
        &node->prom_pi_probe_bridge->device_window_mem[1],
        SN0_PROM_PI_PROBE_WINDOW_OFFSET + addr, data,
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

static const char *origin200_pci_qom_type(SN0ModelPciKind kind)
{
    switch (kind) {
    case SN0_MODEL_PCI_IOC3:
        return TYPE_SGI_IOC3;
    case SN0_MODEL_PCI_ISP1040:
        return "qlogic-isp1040";
    default:
        g_assert_not_reached();
    }
}

static void origin200_create_ioc3(SN0MachineState *s,
                                  unsigned int node_index,
                                  const SN0ModelPciFunction *function)
{
    PCIDevice *ioc3 = pci_new(PCI_DEVFN(function->device,
                                        function->function),
                              origin200_pci_qom_type(function->kind));
    Chardev *ioc3_a, *ioc3_b;

    /* A child of the node that carries the board, not of the machine. */
    object_property_add_child(OBJECT(s->nodes[node_index]), "ioc3",
                              OBJECT(ioc3));
    /*
     * Every module has its own base I/O controller with its own pair of
     * serial connectors, so each node backs its IOC3 ports A and B from its
     * own -serial lines -- node-major with a fixed stride, node 0 at 0/1 and
     * node 1 at 4/5. Attach only a line that has a backend: an unbacked
     * connector is left with none, the same as the AUX port, so a slot with
     * no cable stays distinguishable from one that silently swallows. This is
     * what lights a slave module's POD console, which was dark when only
     * node 0's controller was backed.
     */
    ioc3_a = serial_hd(SN0_NODE_SERIAL(node_index, SN0_IOC3_SERIAL_A_CHARDEV));
    ioc3_b = serial_hd(SN0_NODE_SERIAL(node_index, SN0_IOC3_SERIAL_B_CHARDEV));
    if (ioc3_a) {
        qdev_prop_set_chr(DEVICE(ioc3), "chardev-a", ioc3_a);
    }
    if (ioc3_b) {
        qdev_prop_set_chr(DEVICE(ioc3), "chardev-b", ioc3_b);
    }

    /*
     * The parallel connector on the same board, backed from -parallel and
     * indexed by node for the same reason the serial lines are: every module
     * has one, and a shared backend would interleave two modules' printing
     * into one stream.
     */
    if (node_index < MAX_PARALLEL_PORTS && parallel_hds[node_index]) {
        qdev_prop_set_chr(DEVICE(ioc3), "parallel-chardev",
                          parallel_hds[node_index]);
    }

    /* Distinct per node, or a guest fails the second as a duplicate. */
    qdev_prop_set_uint64(DEVICE(ioc3), "onewire-serial",
                         function->serial_base +
                         function->serial_stride * node_index);

    /* The timekeeper's battery, optional and per node. */
    {
        g_autofree char *nvram_id = g_strdup_printf("sgi-nvram%u", node_index);
        DriveInfo *nvram_dinfo = drive_get(IF_MTD, 0, node_index);
        BlockBackend *nvram_blk = nvram_dinfo ? blk_by_legacy_dinfo(nvram_dinfo)
                                              : blk_by_name(nvram_id);

        if (nvram_blk) {
            qdev_prop_set_drive(DEVICE(ioc3), "nvram-drive", nvram_blk);
        }
    }

    pci_realize_and_unref(ioc3,
                          PCI_HOST_BRIDGE(s->io_bridge[node_index])->bus,
                          &error_fatal);
    pci_set_byte(ioc3->config + PCI_REVISION_ID, function->pci_revision);
    s->pci_ioc3[node_index] = ioc3;

    /*
     * Board wiring, not PCI INTx: the slot-equals-pin default would land
     * this on the ethernet's pin. Wired even with the bridge window unmapped.
     */
    qdev_connect_gpio_out(DEVICE(ioc3), 0,
                          qdev_get_gpio_in(DEVICE(s->io_bridge[node_index]),
                                           function->irq_input));
}

static void origin200_create_isp(SN0MachineState *s,
                                 unsigned int node_index,
                                 const SN0ModelPciFunction *function,
                                 unsigned int instance)
{
    g_autofree char *name = g_strdup_printf("isp%u", instance);
    PCIDevice *isp;

    g_assert(instance < SN0_MAX_SCSI_ADAPTERS);
    isp = pci_new(PCI_DEVFN(function->device, function->function),
                  origin200_pci_qom_type(function->kind));
    object_property_add_child(OBJECT(s->nodes[node_index]), name,
                              OBJECT(isp));
    pci_realize_and_unref(isp,
                          PCI_HOST_BRIDGE(s->io_bridge[node_index])->bus,
                          &error_fatal);
    pci_set_byte(isp->config + PCI_REVISION_ID, function->pci_revision);
    s->pci_scsi[node_index][instance] = isp;
    qlogic_isp_handle_legacy_cmdline(DEVICE(isp));
}

static void sn0_create_base_io(SN0MachineState *s, unsigned int node_index)
{
    const SN0MachineModel *model = SN0_MACHINE_GET_CLASS(s)->model;
    const SN0ModelBaseIO *base_io = &model->base_io;
    const SN0ModelRegion *bridge = sn0_model_find_region(
        model, SN0_MODEL_REGION_BRIDGE_REGISTERS);
    const SN0ModelPciFunction *ioc3 = NULL;
    unsigned int scsi_index = 0;
    size_t function_index;

    g_assert(base_io->functions || base_io->function_count == 0);
    for (function_index = 0; function_index < base_io->function_count;
         function_index++) {
        const SN0ModelPciFunction *function =
            &base_io->functions[function_index];

        switch (function->kind) {
        case SN0_MODEL_PCI_ISP1040:
            if (s->io_bridge_present) {
                origin200_create_isp(s, node_index, function, scsi_index);
            }
            scsi_index++;
            break;
        case SN0_MODEL_PCI_IOC3:
            g_assert(!ioc3);
            origin200_create_ioc3(s, node_index, function);
            ioc3 = function;
            break;
        default:
            g_assert_not_reached();
        }
    }
    g_assert(ioc3 && bridge);
    g_assert(scsi_index <= SN0_MAX_SCSI_ADAPTERS);

    if (s->io_bridge_present) {
        MemoryRegion *bridge_face =
            sysbus_mmio_get_region(SYS_BUS_DEVICE(s->io_bridge[node_index]), 0);
        unsigned int widget;

        memory_region_add_subregion_overlap(
            &s->nodes[node_index]->xtalk,
            SN0_XTALK_BASE(base_io->widget),
            bridge_face, -1);

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
                                     SGI_BRIDGE_WIDGET_SIZE);
            memory_region_add_subregion(&s->nodes[node_index]->xtalk,
                                        SN0_XTALK_BASE(widget),
                                        &s->widget_alias[node_index][widget]);
        }

        memory_region_init_io(&s->nodes[node_index]->prom_pi_probe,
                              OBJECT(s->nodes[node_index]),
                              &sn0_prom_pi_probe_ops,
                              s->nodes[node_index],
                              "sn0.prom-pi-probe", 8);
        s->nodes[node_index]->prom_pi_probe_bridge =
            s->io_bridge[node_index];
        memory_region_add_subregion_overlap(
            &s->nodes[node_index]->xtalk,
            SN0_XTALK_BASE(SN0_PROM_PI_PROBE_WIDGET) +
            SN0_PROM_PI_PROBE_OFFSET,
            &s->nodes[node_index]->prom_pi_probe, 1);

    } else {
        if (node_index == 0) {
            warn_report(
                "origin200: the base I/O controller is mapped directly "
                "at its crosstalk address; without synthesized firmware "
                "to assign it PCI resources the way real ARCS PROM does, "
                "the I/O bridge cannot yet carry it to a booting guest, "
                "and registers wider than a byte read byte-swapped here "
                "because nothing on this path does the bridge's endianness "
                "translation (an approved interim, see task #43)");
        }
        /* Multi-byte registers read byte-swapped here (the interim above). */
        g_assert(ioc3->bridge_window_base >= bridge->offset);
        memory_region_add_subregion(
            &s->nodes[node_index]->xtalk,
            SN0_XTALK_BASE(base_io->widget) +
            ioc3->bridge_window_base - bridge->offset,
            &SGI_IOC3(s->pci_ioc3[node_index])->bar);
    }
}


/*
 * The base I/O controller is a PCI device and needs a bus to realize onto,
 * so the chip is always built; the io-bridge option gates only mapping its
 * register window. Without it the controller's BAR sits directly at the
 * address the console descriptor names until firmware assigns resources.
 */
static void origin200_create_bridge(SN0MachineState *s,
                                    unsigned int node_index)
{
    const SN0MachineModel *model = SN0_MACHINE_GET_CLASS(s)->model;
    const SN0ModelBaseIO *base_io = &model->base_io;
    const SN0ModelAddressSpace *xtalk = sn0_model_find_space(
        model, SN0_MODEL_SPACE_XTALK);
    MemoryRegion *system = get_system_memory();
    SGIBridgeState *bridge;
    g_autofree char *bridge_name = node_index == 0 ?
        g_strdup("bridge") : g_strdup_printf("bridge%u", node_index);
    g_autofree char *widget0_name = node_index == 0 ?
        g_strdup("sn0.xtalk.widget0") :
        g_strdup_printf("sn0.node%u.xtalk.widget0", node_index);

    g_assert(xtalk);
    g_assert(is_power_of_2(model->node_stride));
    bridge = SGI_BRIDGE(object_new(TYPE_SGI_BRIDGE));
    object_property_add_child(OBJECT(s), bridge_name, OBJECT(bridge));
    object_unref(OBJECT(bridge));
    s->io_bridge[node_index] = bridge;
    /* Real Origin 200s report more than one revision, so selectable. */
    qdev_prop_set_uint32(DEVICE(bridge), "revision", s->widget0_revision);
    /* Per-node one-wire serial, as on a physical BaseIO board. */
    qdev_prop_set_uint64(DEVICE(bridge), "onewire-serial",
                         base_io->serial_base +
                         base_io->serial_stride * node_index);
    if (node_index != 0) {
        g_autofree char *bus_name = g_strdup_printf("pci.%u", node_index);

        qdev_prop_set_string(DEVICE(bridge), "pci-bus-name", bus_name);
    }
    /* Interrupt packets are real transfers, not calls into the Hub. */
    object_property_set_link(OBJECT(bridge), "fabric", OBJECT(system),
                             &error_fatal);
    qdev_prop_set_uint64(DEVICE(bridge), "fabric-base", xtalk->carrier_base);
    qdev_prop_set_uint32(DEVICE(bridge), "fabric-node-shift",
                         ctz64(model->node_stride));

    /*
     * Nothing here says which slots are byte-lane exempt. The controller
     * declares that for itself and the bridge reads it as the card is
     * plugged, so a board that moved the chip -- or a second one added on
     * the command line -- cannot leave a slot number behind here to go
     * stale.
     */
    sysbus_realize(SYS_BUS_DEVICE(bridge), &error_fatal);

    /*
     * Widget 0 is the same chip, aliased so it cannot disagree. Unconditional:
     * OpenBSD panics if widget 0's identification register does not read.
     */
    memory_region_init_alias(&s->widget0[node_index], OBJECT(s), widget0_name,
                             sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0),
                             0, SGI_BRIDGE_WIDGET_SIZE);
    memory_region_add_subregion(&s->nodes[node_index]->xtalk,
                                SN0_XTALK_BASE(base_io->discovery_widget),
                                &s->widget0[node_index]);
}

static void origin200_populate_io(SN0MachineState *s, unsigned int node_index)
{
    origin200_create_bridge(s, node_index);
    sn0_create_base_io(s, node_index);
}

static void origin200_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SN0MachineClass *smc = SN0_MACHINE_CLASS(oc);
    const SN0ModelProfile *default_profile;
    const SN0ModelPciFunction *ioc3;
    uint64_t default_ram_mib = 0;
    unsigned int default_cpus = 0;
    size_t node_index;

    smc->model = &sn0_origin200_model;
    g_assert(smc->model->profile_count > 0);
    default_profile = &smc->model->profiles[0];
    ioc3 = sn0_model_find_pci_function(&smc->model->base_io,
                                       SN0_MODEL_PCI_IOC3, 0);
    g_assert(ioc3);
    for (node_index = 0; node_index < default_profile->node_count;
         node_index++) {
        const SN0ModelNodeInstance *node =
            sn0_model_profile_node(default_profile, node_index);

        g_assert(default_ram_mib <= UINT64_MAX - node->default_memory_mib);
        default_ram_mib += node->default_memory_mib;
        g_assert(default_cpus <= UINT_MAX - node->cpu_count);
        default_cpus += node->cpu_count;
    }
    g_snprintf(origin200_ioc3_subsystem_id,
               sizeof(origin200_ioc3_subsystem_id), "0x%04x",
               (unsigned int)ioc3->subsystem_id);
    compat_props_add(mc->compat_props, origin200_compat,
                     ARRAY_SIZE(origin200_compat));
    mc->desc = smc->model->description;
    g_assert(default_ram_mib <= UINT64_MAX / MiB);
    mc->default_ram_size = default_ram_mib * MiB;
    mc->default_cpus = default_cpus;
    mc->default_ram_id = "sn0.node0.ram";
    /* -hda and if=default drives belong on the first SCSI adapter. */
    mc->block_default_type = IF_SCSI;
    mc->default_nic = "sgi-ioc3-eth";
    /* Node memory is memdev-backed -numa nodes; bare mem= has no backend. */
    mc->numa_mem_supported = false;

    mc->max_cpus = sn0_model_max_cpus(smc->model);
    smc->populate_io = origin200_populate_io;
}

static const TypeInfo origin200_machine_types[] = {
    {
        .name       = MACHINE_TYPE_NAME("origin200"),
        .parent     = TYPE_SN0_MACHINE,
        .class_init = origin200_class_init,
    },
};

DEFINE_TYPES(origin200_machine_types)
