/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 200: the one- or two-chassis SN0 product on the IP29 board.
 * The family machinery this parameterizes lives in sn0.c.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
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

#define SN0_BRIDGE_ONEWIRE_SERIAL_BASE 0x060504030201ULL

/* IP29 has four banks, each made from one matched DIMM pair. */
#define IP29_MEM_BANKS 4

/* One chassis, or two coupled over CrayLink. */
#define SN0_ORIGIN200_MAX_NODES 2

/*
 * Derive a deterministic DIMM population from -m. Bank zero stays at 256 MiB
 * or less -- a larger one would cover the fixed boot-flash addresses at the
 * top of its stride; a model limitation, not an IP29 rule.
 */
static bool sn0_ip29_layout_from(uint64_t remaining_mb, unsigned int bank,
                                 uint16_t bank_mb[SN0_MEM_BANKS])
{
    static const uint16_t supported_mb[] = { 512, 256, 128, 64 };
    unsigned int i;

    if (remaining_mb == 0) {
        return bank > 0;
    }
    if (bank == IP29_MEM_BANKS) {
        return false;
    }

    for (i = 0; i < G_N_ELEMENTS(supported_mb); i++) {
        uint16_t size_mb = supported_mb[i];

        if ((bank == 0 && size_mb > 256) || size_mb > remaining_mb) {
            continue;
        }
        bank_mb[bank] = size_mb;
        if (sn0_ip29_layout_from(remaining_mb - size_mb, bank + 1,
                                 bank_mb)) {
            return true;
        }
        bank_mb[bank] = 0;
    }
    return false;
}

static bool sn0_ip29_memory_layout(uint64_t ram_size,
                                   uint16_t bank_mb[SN0_MEM_BANKS],
                                   unsigned int *populated_banks)
{
    unsigned int i;

    memset(bank_mb, 0, sizeof(uint16_t) * SN0_MEM_BANKS);
    if (ram_size == 0 || ram_size % MiB != 0 ||
        !sn0_ip29_layout_from(ram_size / MiB, 0, bank_mb)) {
        return false;
    }

    i = 0;
    while (i < SN0_MEM_BANKS && bank_mb[i] != 0) {
        i++;
    }
    *populated_banks = i;
    return true;
}

#define SN0_PROM_PI_PROBE_WIDGET 2
#define SN0_PROM_PI_PROBE_OFFSET 0x400208
#define SN0_PROM_PI_PROBE_WINDOW_OFFSET 0x208

/* Preserve the CPU's byte order across the nested Bridge dispatch. */
#define SN0_PROM_PI_PROBE_MEMOP(size) (size_memop(size) | MO_BE)

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

static void sn0_create_base_io(SN0MachineState *s, unsigned int node_index)
{
    PCIDevice *ioc3;
    Chardev *ioc3_a, *ioc3_b;
    unsigned int slot;

    ioc3 = pci_new(PCI_DEVFN(SN0_IOC3_DEVICE, 0), TYPE_SGI_IOC3);
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

    /* Distinct per node, or a guest fails the second as a duplicate. */
    qdev_prop_set_uint64(DEVICE(ioc3), "onewire-serial",
                         SGI_IOC3_ONEWIRE_SERIAL + node_index);

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
    s->pci_ioc3[node_index] = ioc3;

    /*
     * Board wiring, not PCI INTx: the slot-equals-pin default would land
     * this on the ethernet's pin. Wired even with the bridge window unmapped.
     */
    qdev_connect_gpio_out(DEVICE(ioc3), 0,
                          qdev_get_gpio_in(DEVICE(s->io_bridge[node_index]),
                                           SN0_IOC3_SUPERIO_PIN));

    if (s->io_bridge_present) {
        MemoryRegion *bridge_face =
            sysbus_mmio_get_region(SYS_BUS_DEVICE(s->io_bridge[node_index]), 0);
        unsigned int widget;

        memory_region_add_subregion_overlap(
            &s->nodes[node_index]->xtalk, SN0_XTALK_BASE(SN0_CONSOLE_WIDGET),
            bridge_face, -1);

        /* No crossbar: alias the bridge's face at every free widget number. */
        for (widget = SN0_WIDGET0 + 2; widget < SN0_XTALK_WIDGETS; widget++) {
            g_autofree char *name = NULL;

            if (widget == SN0_CONSOLE_WIDGET) {
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

        /* Bridge-only reachable; disks arrive on the command line. */
        for (slot = 0; slot < SN0_SCSI_ADAPTERS; slot++) {
            g_autofree char *isp_name = g_strdup_printf("isp%u", slot);
            PCIDevice *isp = pci_new(
                PCI_DEVFN(SN0_SCSI_DEVICE0 + slot, 0), "qlogic-isp1040");

            /* Under the node that carries the board, as the controller is. */
            object_property_add_child(OBJECT(s->nodes[node_index]), isp_name,
                                      OBJECT(isp));
            pci_realize_and_unref(
                isp, PCI_HOST_BRIDGE(s->io_bridge[node_index])->bus,
                &error_fatal);
            s->pci_scsi[node_index][slot] = isp;

            /* A bus never offered the command line's drives fails startup. */
            qlogic_isp_handle_legacy_cmdline(
                DEVICE(s->pci_scsi[node_index][slot]));
        }
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
        memory_region_add_subregion(
            &s->nodes[node_index]->xtalk,
            SN0_XTALK_BASE(SN0_CONSOLE_WIDGET) + SN0_BRIDGE_DEVICE2,
            &SGI_IOC3(ioc3)->bar);
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
    MemoryRegion *system = get_system_memory();
    SGIBridgeState *bridge;
    uint64_t exempt;
    g_autofree char *bridge_name = node_index == 0 ?
        g_strdup("bridge") : g_strdup_printf("bridge%u", node_index);
    g_autofree char *widget0_name = node_index == 0 ?
        g_strdup("sn0.xtalk.widget0") :
        g_strdup_printf("sn0.node%u.xtalk.widget0", node_index);

    bridge = SGI_BRIDGE(object_new(TYPE_SGI_BRIDGE));
    object_property_add_child(OBJECT(s), bridge_name, OBJECT(bridge));
    object_unref(OBJECT(bridge));
    s->io_bridge[node_index] = bridge;
    /* Real Origin 200s report more than one revision, so selectable. */
    qdev_prop_set_uint32(DEVICE(bridge), "revision", s->widget0_revision);
    /* Per-node one-wire serial, as on a physical BaseIO board. */
    qdev_prop_set_uint64(DEVICE(bridge), "onewire-serial",
                         SN0_BRIDGE_ONEWIRE_SERIAL_BASE + node_index);
    if (node_index != 0) {
        g_autofree char *bus_name = g_strdup_printf("pci.%u", node_index);

        qdev_prop_set_string(DEVICE(bridge), "pci-bus-name", bus_name);
    }
    /* Interrupt packets are real transfers, not calls into the Hub. */
    object_property_set_link(OBJECT(bridge), "fabric", OBJECT(system),
                             &error_fatal);
    qdev_prop_set_uint64(DEVICE(bridge), "fabric-base", SN0_SPACE_XTALK);
    qdev_prop_set_uint32(DEVICE(bridge), "fabric-node-shift", SN0_NODE_SHIFT);

    /* The IOC3 is byte-lane exempt; OR, so a -global exemption survives. */
    exempt = object_property_get_uint(OBJECT(bridge), "byte-lane-exempt",
                                      &error_abort);
    object_property_set_uint(OBJECT(bridge), "byte-lane-exempt",
                             exempt | (1u << SN0_IOC3_DEVICE), &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(bridge), &error_fatal);

    /*
     * Widget 0 is the same chip, aliased so it cannot disagree. Unconditional:
     * OpenBSD panics if widget 0's identification register does not read.
     */
    memory_region_init_alias(&s->widget0[node_index], OBJECT(s), widget0_name,
                             sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0),
                             0, SGI_BRIDGE_WIDGET_SIZE);
    memory_region_add_subregion(&s->nodes[node_index]->xtalk,
                                SN0_XTALK_BASE(SN0_WIDGET0),
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

    mc->desc = "SGI Origin 200 (IP27)";
    mc->default_ram_size = 64 * MiB;
    mc->default_ram_id = "sn0.node0.ram";
    /* -hda and if=default drives belong on the first SCSI adapter. */
    mc->block_default_type = IF_SCSI;
    mc->default_nic = "sgi-ioc3-eth";
    /* Node memory is memdev-backed -numa nodes; bare mem= has no backend. */
    mc->numa_mem_supported = false;

    /* One chassis or two coupled over CrayLink, two processor slots each. */
    smc->max_nodes = SN0_ORIGIN200_MAX_NODES;
    mc->max_cpus = SN0_CPUS_PER_NODE * smc->max_nodes;
    smc->hub_hz = 90000000;
    smc->rtc_hz = 1250000;          /* an 800 ns tick */
    smc->fw_machine_type = 1;       /* 0 is Origin 2000, 1 is Origin 200 */
    smc->fprom_cyc = 15;
    smc->fprom_wr = 4;

    /* A captured 180 MHz R10000 module population. */
    smc->cpu_module.clock_hz = 180000000;
    smc->cpu_module.scache_clock_mhz = 90;
    smc->cpu_module.mode_bits = 0x1211a785;
    smc->node_memory_layout = sn0_ip29_memory_layout;
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
