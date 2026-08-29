/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 200, the one-or-two-chassis SN0 product on the IP29 board.
 *
 * What makes an Origin 200 an Origin 200 rather than another member of the
 * family: the 180 MHz clocks and the ip27conf words that advertise them, the
 * IP29 board's four memory banks, and the base-module I/O -- one directly
 * connected Bridge per chassis with two SCSI adapters and the base I/O
 * controller on its PCI bus, and no crossbar, so the Bridge's whole widget
 * face answers at every widget address the Hub emits. The family machinery
 * this parameterizes lives in sn0.c.
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

#define SN0_BRIDGE_ONEWIRE_SERIAL_BASE 0x060504030201ULL

/* IP29 has four banks, each made from one matched DIMM pair. */
#define IP29_MEM_BANKS 4

/* One chassis, or two coupled over CrayLink. */
#define SN0_ORIGIN200_MAX_NODES 2

/*
 * Find a deterministic IP29 DIMM population for QEMU's scalar -m value.
 * Physical banks support 64, 128, 256, or 512 MiB and bank zero must be
 * present. Prefer larger banks and fewer partially populated pairs.
 *
 * A 512 MiB bank zero would fill all four of its slots, including the fixed
 * boot-flash and synthesized-firmware addresses at the top of the stride.
 * Until that fixed decode can be represented independently, keep bank zero
 * at 256 MiB or less and reject totals that cannot be expressed in four banks
 * under that constraint. This is a model limitation, not an IP29 DIMM rule.
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
    unsigned int slot;

    ioc3 = pci_new(PCI_DEVFN(SN0_IOC3_DEVICE, 0), TYPE_SGI_IOC3);
    if (node_index == 0) {
        qdev_prop_set_chr(DEVICE(ioc3), "chardev-a",
                          serial_hd(SN0_IOC3_SERIAL_A_CHARDEV));
        qdev_prop_set_chr(DEVICE(ioc3), "chardev-b",
                          serial_hd(SN0_IOC3_SERIAL_B_CHARDEV));
    }

    /*
     * Each base I/O board's controller carries its own laser-programmed
     * one-wire serial, the same as its bridge does. Consecutive only by
     * choice: node 0 keeps the value this machine reported before it grew a
     * second chassis, and node 1 gets the next one so the firmware -- and a
     * guest's one-wire bus -- cannot merge two physical nodes that fall back
     * to their base I/O identities. Without a distinct value per node the
     * two controllers answer alike, and a guest that registers both fails
     * the second as a duplicate.
     */
    qdev_prop_set_uint64(DEVICE(ioc3), "onewire-serial",
                         SGI_IOC3_ONEWIRE_SERIAL + node_index);

    /*
     * The timekeeper's battery, optional and per node: each base I/O board
     * carries its own, so the id is derived from node_index rather than
     * fixed, the same way a two-node machine's two controllers are never
     * confused for each other anywhere else in this file. Unnamed, nothing
     * changes -- the timekeeper stays exactly as ephemeral as it always was.
     */
    {
        g_autofree char *nvram_id = g_strdup_printf("sgi-nvram%u", node_index);
        BlockBackend *nvram_blk = blk_by_name(nvram_id);

        if (nvram_blk) {
            qdev_prop_set_drive(DEVICE(ioc3), "nvram-drive", nvram_blk);
        }
    }

    pci_realize_and_unref(ioc3,
                          PCI_HOST_BRIDGE(s->io_bridge[node_index])->bus,
                          &error_fatal);
    s->pci_ioc3[node_index] = ioc3;

    /*
     * The controller's second interrupt does not reach the Bridge through
     * PCI's own INTx routing: sgi_bridge_map_irq() has no rule for it other
     * than the slot-equals-pin default, which would land it on pin 2
     * alongside the controller's own ethernet interrupt, reached through the
     * chip's ordinary PCI interrupt pin, rather than the pin the board
     * actually wires the second one to. Connected directly to the Bridge's
     * matching input instead, wired regardless of whether the bridge is the
     * machine's default path to the controller, the same way the bridge's
     * fabric link is always set: the wire exists on the real board whether or
     * not a guest currently reaches the controller through the bridge's own
     * window.
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

        /*
         * The bridge answers at every other non-zero widget too, because the
         * machine has no crossbar and the Hub's link reaches it whatever widget
         * number an address carries. Alias its full widget face onto each of the
         * net-new widgets — 2 through 7 and 9 through 15 — leaving widget 0 (its
         * own alias, mapped unconditionally with the bridge), widget 1 (the Hub)
         * and
         * widget 8 (mapped just above) alone. This is what lets the firmware's
         * device pointer stamped with widget 2 reach the bridge's device-1 PIO
         * window instead of faulting, matching the ratified no-crossbar
         * contract. Gated with the bridge because the device windows these
         * expose are only real when the devices behind the bridge are.
         */
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

        /*
         * The two SCSI adapters the base I/O board carries, at the first two
         * PCI devices. They exist only when the bridge is asked for, because
         * a guest can only reach them through the bridge's own per-device
         * decode — unlike the controller above, nothing here gives them a
         * direct fallback mapping, so without the bridge there is nowhere a
         * guest could reach them at.
         *
         * They are created with no drive attached. A machine that invented
         * backing storage would be describing a disk nobody asked for, and the
         * adapters answer their register contract with an empty bus — a disk
         * arrives on the command line, behind the first adapter, when a guest
         * needs to boot from one.
         *
         * sn0_assign_pci_resources assigns each resource at reset rather than
         * here — see that function for why realize is the wrong point to do
         * it.
         */
        for (slot = 0; slot < SN0_SCSI_ADAPTERS; slot++) {
            s->pci_scsi[node_index][slot] = pci_create_simple(
                PCI_HOST_BRIDGE(s->io_bridge[node_index])->bus,
                PCI_DEVFN(SN0_SCSI_DEVICE0 + slot, 0), "qlogic-isp1040");
        }
    } else {
        if (node_index == 0) {
            warn_report(
                "origin200: the base I/O controller is mapped directly "
                "at its crosstalk address; without synthesized firmware "
                "to assign it PCI resources the way real ARCS PROM does, "
                "the I/O bridge cannot yet carry it to a booting guest "
                "(an approved interim, see task #43)");
        }
        memory_region_add_subregion(
            &s->nodes[node_index]->xtalk,
            SN0_XTALK_BASE(SN0_CONSOLE_WIDGET) + SN0_BRIDGE_DEVICE2,
            &SGI_IOC3(ioc3)->bar);
    }
}


/*
 * The I/O bridge. It carries a real PCI bus, and the base I/O controller
 * below is a PCI device that cannot exist without one to realize onto —
 * so the chip is always built, whether or not this machine is asked for
 * an I/O bridge. What "asked for" gates is whether the chip's own
 * register window is mapped into crosstalk space at all.
 *
 * That neutrality is about guest reachability specifically, not about
 * the machine as a whole: unmapped, the chip is exactly as unreachable
 * as it always was, because nothing reaches a MemoryRegion nobody
 * subregioned into anything the guest can address. The topology is not
 * neutral the same way. The bridge and the controller now always exist
 * in QOM and always take part in reset, whether or not a guest can see
 * them — a qtest, `info qtree`, or anything else that introspects the
 * machine sees the always-realized bus even with the option off, which
 * it did not before this change.
 *
 * That has one consequence worth stating plainly because migration is a
 * core workflow: a snapshot taken on a build from before this change
 * cannot be loaded on a build from after it, in either direction. Both
 * devices' own VMStateDescriptions gate this correctly — the bridge and
 * the controller take part in a machine snapshot only once they exist as
 * QOM devices, so their state was simply absent from any snapshot taken
 * before this commit, and the controller's own version bumps since
 * (task #47) set minimum_version_id equal to version_id, which QEMU
 * treats as an unconditional reject rather than a best-effort load. This
 * is not a smaller or larger snapshot; it is an incompatible one.
 *
 * The base I/O controller used to be mapped directly at its crosstalk
 * address, and that direct mapping shadowed the bridge's own window for
 * the third device on its bus — a guest programming that window to reach
 * a different device (the first SCSI adapter's own window landed there)
 * had its store swallowed by the controller's overlay instead. Putting
 * the controller on the bus as a real PCI device retires that overlay
 * when the bridge is asked for: the window belongs to whichever device
 * is actually there, discovered the same way a guest discovers any other
 * PCI device, rather than being reserved for one of them in advance.
 *
 * The controller's crosstalk-side address does not move either way: the
 * bridge aims each slot's window by slot number, not by the PCI address
 * a guest assigns the device's BAR, so the third slot's window sits at
 * the same offset the configuration graph's console descriptor already
 * names. With the bridge unasked-for, the controller's BAR is instead
 * subregioned into that same address directly, bypassing the bridge's
 * own per-device decode entirely — the same placement a system-bus
 * device's region would get, and the reason this is not yet a
 * self-contained fix: task #35's own boot test found that no guest here
 * assigns PCI resources for itself before it needs its console, so
 * without this direct placement a guest reached through the bridge
 * cannot find the controller at all until firmware does that assignment
 * (task #43). This whole branch — the direct subregion, and the warning
 * naming it an interim — is deleted once firmware can.
 */
static void origin200_create_bridge(SN0MachineState *s,
                                    unsigned int node_index)
{
    MemoryRegion *system = get_system_memory();
    SGIBridgeState *bridge;
    g_autofree char *bridge_name = node_index == 0 ?
        g_strdup("bridge") : g_strdup_printf("bridge%u", node_index);
    g_autofree char *widget0_name = node_index == 0 ?
        g_strdup("sn0.xtalk.widget0") :
        g_strdup_printf("sn0.node%u.xtalk.widget0", node_index);

    bridge = SGI_BRIDGE(object_new(TYPE_SGI_BRIDGE));
    object_property_add_child(OBJECT(s), bridge_name, OBJECT(bridge));
    object_unref(OBJECT(bridge));
    s->io_bridge[node_index] = bridge;
    /*
     * The silicon revision of the I/O bridge installed in this configuration.
     * Real Origin 200 systems report more than one revision, so the command
     * line can select a different captured configuration.
     *
     * It reaches widget 0 as well as widget 8 without being written twice,
     * because those are one chip and this is the one place the machine says
     * which chip revision is installed.
     */
    qdev_prop_set_uint32(DEVICE(bridge), "revision", s->widget0_revision);
    /*
     * Every physical BaseIO board has its own laser-programmed one-wire
     * serial. These stable model values are consecutive only by choice; the
     * first preserves the identity this machine exposed before it supported a
     * second chassis, while the second prevents firmware from merging two
     * physical nodes that fall back to their BaseIO identities.
     */
    qdev_prop_set_uint64(DEVICE(bridge), "onewire-serial",
                         SN0_BRIDGE_ONEWIRE_SERIAL_BASE + node_index);
    if (node_index != 0) {
        g_autofree char *bus_name = g_strdup_printf("pci.%u", node_index);

        qdev_prop_set_string(DEVICE(bridge), "pci-bus-name", bus_name);
    }
    /*
     * The fabric its interrupt packets travel, and how this machine encodes
     * a crosstalk destination in it. A pin rise becomes a write to the
     * Hub's interrupt-modify register on the named node, and it goes
     * through the machine's own address space rather than by calling into
     * the Hub — the packet is a real transfer across the bus and modelling
     * it as a function call would invent a wire and step over the fabric.
     *
     * The chip knows the packet's format; this knows where a node's Hub
     * lives, which is why the encoding arrives from here. Set even when the
     * bridge is not asked for, because realize requires it and nothing here
     * ever drives a packet through an unmapped chip's fabric link anyway.
     */
    object_property_set_link(OBJECT(bridge), "fabric", OBJECT(system),
                             &error_fatal);
    qdev_prop_set_uint64(DEVICE(bridge), "fabric-base", SN0_SPACE_XTALK);
    qdev_prop_set_uint32(DEVICE(bridge), "fabric-node-shift", SN0_NODE_SHIFT);
    sysbus_realize(SYS_BUS_DEVICE(bridge), &error_fatal);

    /*
     * Widget 0, which is this same bridge and not a second device.
     *
     * A real Origin 200 has no crossbar. The Hub has one crosstalk link with
     * the bridge on the far end of it, and with no switch in between there is
     * nothing to route by widget number — so every widget address the Hub
     * emits arrives at the same chip. SGI's own firmware says so where it
     * probes that link: a directly connected bridge can be given whichever
     * widget number the firmware chooses, and it chooses 8, avoiding 0 because
     * something about error handling there is a problem. Widget 8 is therefore
     * an agreement between two firmware routines rather than a position.
     *
     * So this is an alias and not a device. Every guest reads widget 0's
     * identification register first and decides what kind of machine it is on
     * from the answer, and a separate responder holding a copy of the bridge's
     * identity would be two objects asserting one chip's — which cannot be
     * kept in step by inspection, only by remembering to. An alias cannot
     * disagree: there is nothing there to hold a second opinion.
     *
     * Unconditional, unlike the mapping at widget 8 below. The bridge chip is
     * always built and the machine option decides only whether its own widget
     * answers, so widget 0 keeps answering with the option off. That matters:
     * OpenBSD panics outright if widget 0's identification register does not
     * read, so gating this would turn a development option into a machine no
     * guest can boot.
     *
     * The other non-zero widgets answer too, on the argument this once used to
     * refuse them: a crossbar-less machine reaches the bridge at every widget
     * number, and the firmware relies on it — the IRIX miniroot dereferences a
     * device pointer stamped with widget 2. Those aliases go in beside the
     * widget-8 mapping in sn0_create_base_io, gated on the bridge being fitted;
     * widget 0 here is unconditional for the reason above, and widget 1 stays
     * the Hub.
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

    /*
     * One chassis or two coupled over CrayLink, two processor slots each. A
     * stock Origin 200 runs its R10000 at 180 MHz and its Hub and secondary
     * cache at 90 MHz.
     */
    smc->max_nodes = SN0_ORIGIN200_MAX_NODES;
    mc->max_cpus = SN0_CPUS_PER_NODE * smc->max_nodes;
    smc->cpu_hz = 180000000;
    smc->hub_hz = 90000000;
    smc->rtc_hz = 1250000;          /* an 800 ns tick */
    smc->scache_clock_mhz = 90;
    smc->fw_machine_type = 1;       /* 0 is Origin 2000, 1 is Origin 200 */
    smc->fprom_cyc = 15;
    smc->fprom_wr = 4;
    smc->processor_mode_bits = 0x1211a785;
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
