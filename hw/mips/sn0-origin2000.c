/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Physical Origin 2000 assembly; supplied firmware owns discovery. */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/datadir.h"
#include "qemu/host-utils.h"
#include "qapi/error.h"
#include "qobject/qlist.h"
#include "hw/core/clock.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/1wire/crc.h"
#include "hw/mips/sn0-fabric-routed.h"
#include "hw/misc/sgi-o2000-module-mgmt.h"
#include "hw/misc/sgi-router.h"
#include "system/blockdev.h"
#include "system/system.h"
#include "system/numa.h"
#include "system/qtest.h"
#include "sn0.h"
#include "sn0-origin2000-model-consumer.h"
#include "sn0-origin2000-xtalk.h"

#define ORIGIN2000_HUB_IO_BUS_ERROR 0x02400208
#define ORIGIN2000_HUB_IO_BUS_ERROR_WIDGET 2
#define ORIGIN2000_HUB_IO_BUS_ERROR_XTALK \
    (SN0_XTALK_BASE(ORIGIN2000_HUB_IO_BUS_ERROR_WIDGET) + \
     ORIGIN2000_HUB_IO_BUS_ERROR - \
     SN0_SMALL_WINDOW(ORIGIN2000_HUB_IO_BUS_ERROR_WIDGET))

#define ORIGIN2000_ROUTER_NIC_PAGES 2

static void origin2000_router_nic_page_crc(uint8_t page[32])
{
    uint16_t crc = ~onewire_crc16(0, page, 30);

    page[30] = crc;
    page[31] = crc >> 8;
}

static void origin2000_router_nic_image(const SN0ModelRouter *router,
                                        uint8_t image[64])
{
    static const uint8_t page_a[30] = {
        0x01, ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
        'O', 'R', 'I', 'G', 'A', 'M', 'I', '-', 'R', 'O', 'U', 'T', 'E',
        'R', '-', 'T', 'E', 'S', 'T',
    };
    static const uint8_t page_b[30] = {
        '-', '0', '0', '0', '0', '1', 'A', '0', '0', '1',
        0, 0, 0, 0, 0, 0,
        'O', 'R', 'I', 'G', 'A', 'M', 'I', ' ', 'R', 'O', 'U', 'T', 'E', 'R',
    };
    char serial[11];

    memcpy(image, page_a, sizeof(page_a));
    memcpy(&image[32], page_b, sizeof(page_b));
    g_snprintf(serial, sizeof(serial), "ORTR%06u", router->slot);
    memcpy(&image[1], serial, 10);
    for (unsigned int page = 0; page < ORIGIN2000_ROUTER_NIC_PAGES; page++) {
        origin2000_router_nic_page_crc(&image[page * 32]);
    }
}

static MemTxResult origin2000_bus_error_read(void *opaque, hwaddr addr,
                                              uint64_t *data,
                                              unsigned int size,
                                              MemTxAttrs attrs)
{
    SN0NodeState *node = opaque;
    uint8_t bytes[8] = {};
    MemTxResult result;

    if (addr == 0 && size == 8) {
        *data = 0;
        sgi_hub_latch_prom_pi_error(&node->hub);
        return MEMTX_ERROR;
    }
    result = address_space_read(&node->origin2000_xtalk_as,
                                ORIGIN2000_HUB_IO_BUS_ERROR_XTALK + addr,
                                attrs, bytes, size);
    *data = ldn_be_p(bytes, size);
    return result;
}

static MemTxResult origin2000_bus_error_write(void *opaque, hwaddr addr,
                                               uint64_t data,
                                               unsigned int size,
                                               MemTxAttrs attrs)
{
    SN0NodeState *node = opaque;
    uint8_t bytes[8];

    stn_be_p(bytes, size, data);
    return address_space_write(&node->origin2000_xtalk_as,
                               ORIGIN2000_HUB_IO_BUS_ERROR_XTALK + addr,
                               attrs, bytes, size);
}

static const MemoryRegionOps origin2000_bus_error_ops = {
    .read_with_attrs = origin2000_bus_error_read,
    .write_with_attrs = origin2000_bus_error_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .impl.unaligned = true,
};

static void origin2000_install_bus_error_aperture(SN0NodeState *node)
{
    const SN0ModelAddressSpace *xtalk = &node->physical->map.xtalk;
    g_autofree char *name = g_strdup_printf(
        "sn0.node%u.origin2000-xtalk", node->nasid);

    address_space_init(&node->origin2000_xtalk_as, &node->xtalk, name);
    node->origin2000_xtalk_as_ready = true;

    for (unsigned int slice = 0; slice < node->physical->cpu_count; slice++) {
        g_autofree char *region_name = g_strdup_printf(
            "sn0.node%u.cpu%u.hub-io-bus-error", node->nasid, slice);

        memory_region_init_io(&node->origin2000_bus_error[slice],
                              OBJECT(node), &origin2000_bus_error_ops, node,
                              region_name, 8);
        memory_region_add_subregion_overlap(
            &node->cpu_sysmem[slice],
            xtalk->carrier_base + ORIGIN2000_HUB_IO_BUS_ERROR,
            &node->origin2000_bus_error[slice], 1);
    }
}

static unsigned int origin2000_cpu_count(const SN0ModelComposition *c)
{
    unsigned int count = 0;
    size_t i;

    for (i = 0; i < c->node_count; i++) {
        count += c->nodes[i].cpu_count;
    }
    return count;
}

static uint64_t origin2000_memory_size(const SN0ModelComposition *c)
{
    uint64_t size = 0;
    size_t i;

    for (i = 0; i < c->node_count; i++) {
        size += c->nodes[i].memory_mib * MiB;
    }
    return size;
}

static const CPUArchIdList *origin2000_possible_cpus(MachineState *machine)
{
    const SN0ModelComposition *c = SN0_MACHINE(machine)->composition;
    unsigned int total;
    size_t i;

    if (machine->possible_cpus) {
        return machine->possible_cpus;
    }
    if (!sn0_origin2000_validate(c, &error_fatal)) {
        return NULL;
    }
    total = origin2000_cpu_count(c);
    machine->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                       sizeof(CPUArchId) * total);
    machine->possible_cpus->len = total;
    for (i = 0; i < c->node_count; i++) {
        unsigned int slice;

        for (slice = 0; slice < c->nodes[i].cpu_count; slice++) {
            CPUArchId *slot = &machine->possible_cpus->cpus[
                c->nodes[i].cpu_first_id + slice];

            slot->type = machine->cpu_type;
            slot->vcpus_count = 1;
            slot->arch_id = c->nodes[i].cpu_first_id + slice;
            slot->props.has_socket_id = true;
            slot->props.socket_id = c->nodes[i].slot - 1;
            slot->props.has_core_id = true;
            slot->props.core_id = slice;
            slot->props.has_thread_id = true;
            slot->props.thread_id = 0;
            slot->props.has_node_id = true;
            slot->props.node_id = c->nodes[i].slot - 1;
        }
    }
    return machine->possible_cpus;
}

static CpuInstanceProperties origin2000_cpu_props(MachineState *machine,
                                                   unsigned int cpu)
{
    const CPUArchIdList *list = origin2000_possible_cpus(machine);

    g_assert(cpu < list->len);
    return list->cpus[cpu].props;
}

static int64_t origin2000_cpu_node(const MachineState *machine, int cpu)
{
    const SN0ModelComposition *c = SN0_MACHINE(machine)->composition;
    size_t i;

    for (i = 0; i < c->node_count; i++) {
        if (cpu >= c->nodes[i].cpu_first_id &&
            cpu - c->nodes[i].cpu_first_id < c->nodes[i].cpu_count) {
            return c->nodes[i].slot - 1;
        }
    }
    return -1;
}

static char *origin2000_get_composition(Object *obj, Error **errp)
{
    return g_strdup(SN0_MACHINE(obj)->composition->name);
}

static void origin2000_set_composition(Object *obj, const char *name,
                                        Error **errp)
{
    SN0MachineState *s = SN0_MACHINE(obj);
    const SN0ModelComposition *c = sn0_origin2000_find_composition(name);

    if (!c) {
        error_setg(errp, "unknown Origin 2000 composition '%s'", name);
        return;
    }
    if (s->num_nodes || MACHINE(s)->possible_cpus) {
        error_setg(errp,
                   "the composition must be selected before construction");
        return;
    }
    if (!sn0_origin2000_validate(c, errp)) {
        return;
    }
    s->composition = c;
}

static SN0FabricVertex origin2000_vertex(const SN0ModelComposition *c,
                                          SN0ModelFabricVertex vertex)
{
    SN0FabricVertex result = {
        .index = vertex.index,
        .port = vertex.port,
    };

    switch (vertex.kind) {
    case SN0_MODEL_FABRIC_HUB:
        result.kind = SN0_FABRIC_VERTEX_HUB;
        result.index = c->nodes[vertex.index].slot - 1;
        break;
    case SN0_MODEL_FABRIC_ROUTER:
        result.kind = SN0_FABRIC_VERTEX_ROUTER;
        result.index = c->routers[vertex.index].slot - 1;
        break;
    default:
        g_assert_not_reached();
    }
    return result;
}

static void origin2000_load_flash(MachineState *machine)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    g_autofree char *path = NULL;
    int64_t image_size = 0;
    size_t i;

    if (machine->firmware) {
        path = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
        if (!path) {
            error_setg(&error_fatal, "cannot find firmware image '%s'",
                       machine->firmware);
        }
        image_size = get_image_size(path, &error_fatal);
        if (image_size == 0) {
            error_setg(&error_fatal, "firmware image '%s' is empty",
                       machine->firmware);
        }
    }
    for (i = 0; i < s->num_nodes; i++) {
        SN0NodeState *node = s->nodes[i];
        MemoryRegion *chip =
            sysbus_mmio_get_region(SYS_BUS_DEVICE(node->flash), 0);
        uint64_t size = memory_region_size(chip);
        uint8_t *image = memory_region_get_ram_ptr(chip);

        if (node->flash_drive) {
            continue;
        }
        if ((uint64_t)image_size > size) {
            error_setg(&error_fatal,
                       "firmware image '%s' exceeds flash capacity",
                       machine->firmware);
        }
        memset(image, 0xff, size);
        if (path && load_image_size(path, image, size) != image_size) {
            error_setg(&error_fatal, "cannot load firmware image '%s'",
                       machine->firmware);
        }
        memory_region_set_dirty(chip, 0, size);
    }
}

void sn0_origin2000_prepare_nodes(MachineState *machine)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    const SN0ModelComposition *c = s->composition;
    uint64_t ram_offset = 0;
    unsigned int cpu_index;
    BlockBackend *flash_drives[4] = { NULL };
    unsigned int flash_count = 0;
    size_t i;

    g_assert(!s->nodes);
    if (!sn0_origin2000_validate(c, &error_fatal)) {
        return;
    }
    if (machine->smp.cpus != origin2000_cpu_count(c) ||
        machine->smp.max_cpus != machine->smp.cpus) {
        error_setg(&error_fatal, "composition '%s' installs %u CPUs", c->name,
                   origin2000_cpu_count(c));
    }
    if (machine->smp.threads != 1 ||
        !((machine->smp.sockets == c->node_count &&
           machine->smp.cores == c->hardware->cpu_slots_per_node) ||
          (machine->smp.sockets == 1 &&
           machine->smp.cores == machine->smp.cpus))) {
        error_setg(&error_fatal,
                   "composition '%s' requires %zu sockets with %u cores each",
                   c->name, c->node_count, c->hardware->cpu_slots_per_node);
    }
    machine->smp.sockets = c->node_count;
    machine->smp.cores = c->hardware->cpu_slots_per_node;
    if (machine->ram_size != origin2000_memory_size(c)) {
        error_setg(&error_fatal, "composition '%s' installs %" PRIu64 " MiB",
                   c->name, origin2000_memory_size(c) / MiB);
    }
    for (i = 0; i < c->node_count; i++) {
        DriveInfo *drive = drive_get_by_index(IF_PFLASH, i);

        if (drive) {
            flash_drives[i] = blk_by_legacy_dinfo(drive);
            flash_count++;
        }
    }
    if (flash_count && flash_count != c->node_count) {
        error_setg(&error_fatal,
                   "Origin 2000 requires all four node pflash drives");
    }
    if (flash_count && machine->firmware) {
        error_setg(&error_fatal,
                   "Origin 2000 cannot combine -bios with node pflash drives");
    }
    /* Register tests deliberately construct the machine without vendor bits. */
    if (machine->kernel_filename || s->synthesized_firmware ||
        (!machine->firmware && !flash_count && !qtest_enabled())) {
        error_setg(&error_fatal, "Origin 2000 requires supplied firmware");
    }
    if (machine->numa_state && machine->numa_state->num_nodes) {
        error_setg(&error_fatal,
                   "composition '%s' defines its memory population",
                   c->name);
    }
    origin2000_possible_cpus(machine);
    s->num_nodes = c->node_count;
    s->nodes = g_new0(SN0NodeState *, s->num_nodes);
    s->cpu_refclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(s->cpu_refclk, c->hardware->cpu_clock_hz);
    s->fabric = SN0_FABRIC(qdev_new(TYPE_SN0_FABRIC_ROUTED));
    object_property_add_child(OBJECT(machine), "fabric", OBJECT(s->fabric));

    for (i = 0; i < c->node_count; i++) {
        g_autofree char *name = g_strdup_printf("node%zu", i);
        SN0NodeState *node = SN0_NODE(object_new(TYPE_SN0_NODE));

        object_property_add_child(OBJECT(machine), name, OBJECT(node));
        object_unref(OBJECT(node));
        s->nodes[i] = node;
        sn0_fabric_add_endpoint(s->fabric, &node->hub, i);
    }
    for (unsigned int slot = 1; slot <= c->router_count; slot++) {
        const SN0ModelRouter *physical = NULL;
        g_autofree char *name = g_strdup_printf("router%u", slot - 1);
        DeviceState *router = qdev_new(TYPE_SGI_ROUTER);
        uint8_t nic_image[64];

        for (i = 0; i < c->router_count; i++) {
            if (c->routers[i].slot == slot) {
                physical = &c->routers[i];
                break;
            }
        }
        g_assert(physical);
        object_property_add_child(OBJECT(machine), name, OBJECT(router));
        qdev_prop_set_uint8(router, "chip-inputs", physical->chip_inputs);
        qdev_prop_set_uint64(router, "nic-clock-hz", c->hardware->hub_clock_hz);
        qdev_prop_set_uint64(router, "nic-serial", physical->serial);
        origin2000_router_nic_image(physical, nic_image);
        sgi_router_set_nic_image(SGI_ROUTER(router), nic_image,
                                 sizeof(nic_image));
        sn0_fabric_routed_add_router(s->fabric, SGI_ROUTER(router));
        qdev_realize_and_unref(router, NULL, &error_fatal);
    }
    for (i = 0; i < c->link_count; i++) {
        sn0_fabric_routed_add_link(s->fabric, c->links[i].name,
                                    origin2000_vertex(c, c->links[i].ends[0]),
                                    origin2000_vertex(c, c->links[i].ends[1]));
    }
    qdev_realize_and_unref(DEVICE(s->fabric), NULL, &error_fatal);
    /* QEMU assigns CPU indices at realize, so consume explicit CPU IDs. */
    for (cpu_index = 0; cpu_index < machine->smp.cpus; cpu_index += 2) {
        const SN0ModelPhysicalNode *physical = NULL;
        SN0NodeState *node;
        QList *banks = qlist_new();
        unsigned int bank, node_index;

        for (i = 0; i < c->node_count; i++) {
            if (c->nodes[i].cpu_first_id == cpu_index) {
                physical = &c->nodes[i];
                break;
            }
        }
        g_assert(physical);
        node_index = physical->slot - 1;
        node = s->nodes[node_index];
        node->physical = physical;
        node->flash_drive = flash_drives[node_index];
        node->hardware = c->hardware;
        qdev_prop_set_uint32(DEVICE(node), "nasid", node_index);
        qdev_prop_set_uint32(DEVICE(node), "cpu-base", physical->cpu_first_id);
        qdev_prop_set_uint32(DEVICE(node), "nr-cpus", physical->cpu_count);
        qdev_prop_set_string(DEVICE(node), "cpu-type", machine->cpu_type);
        object_property_set_link(OBJECT(node), "ram", OBJECT(machine->ram),
                                 &error_fatal);
        qdev_prop_set_uint64(DEVICE(node), "ram-start", ram_offset);
        qdev_prop_set_uint64(DEVICE(node), "local-memory-base",
                             sn0_model_node_address(&physical->map,
                                                      &physical->map.memory,
                                                      node_index, 0));
        for (bank = 0; bank < SN0_MAX_MEMORY_BANKS; bank++) {
            if (physical->bank_mib[bank]) {
                qlist_append_int(banks, physical->bank_mib[bank]);
            }
        }
        qdev_prop_set_array(DEVICE(node), "banks-mb", banks);
        qdev_connect_clock_in(DEVICE(node), "cpu-refclk", s->cpu_refclk);
        object_property_set_link(OBJECT(&node->hub), "fabric",
                                 OBJECT(s->fabric), &error_fatal);
        qdev_prop_set_uint32(DEVICE(&node->hub), "fabric-endpoint", node_index);
        if (physical->module_identity) {
            SGIHubModuleIdentity identity;

            memcpy(identity.rom, physical->module_identity->rom,
                   sizeof(identity.rom));
            memcpy(identity.page, physical->module_identity->page,
                   sizeof(identity.page));
            if (!sgi_hub_attach_module_identity(&node->hub, &identity,
                                                &error_fatal)) {
                return;
            }
        }
        ram_offset += physical->memory_mib * MiB;
    }
}

/*
 * One management complex for the whole module: a controller, a shared NVRAM
 * space and four adapters, one appearing in each node's Hub register window
 * where that node's own adapter sits on an Origin 200. The descriptor names
 * the node and offset for each, and the composition validator has already
 * rejected any node that also asks for a private controller.
 *
 * No action capability is declared. Nothing in this machine receives a
 * module reset, NMI or power line, and the module refuses those commands
 * rather than answering for a line that reaches nothing.
 */
static void origin2000_build_management(SN0MachineState *s)
{
    const SN0ModelComposition *c = s->composition;
    const SN0ModelModule *module = &c->modules[0];
    const SN0ModelModuleManagement *management = module->management;
    DeviceState *dev = qdev_new(TYPE_SGI_O2000_MODULE_MGMT);
    Chardev *aux = serial_hd(SN0_NODE_SERIAL(0, SN0_ELSC_CHARDEV));
    size_t i;

    g_assert(management->adapter_count == SGI_O2000_ADAPTER_COUNT);
    object_property_add_child(OBJECT(s), "module-management", OBJECT(dev));
    qdev_prop_set_uint8(dev, "module-number", module->initial_module_number);
    qdev_prop_set_uint8(dev, "dip-switches", s->elsc_dip_switches);
    if (aux) {
        qdev_prop_set_chr(dev, "chardev", aux);
    }
    qdev_realize_and_unref(dev, NULL, &error_fatal);

    for (i = 0; i < management->adapter_count; i++) {
        const SN0ModelPhysicalNode *physical =
            &c->nodes[management->adapter_nodes[i]];
        SGIHubState *hub = &s->nodes[physical->slot - 1]->hub;
        SysBusDevice *adapter = SYS_BUS_DEVICE(
            sgi_o2000_module_mgmt_get_adapter(SGI_O2000_MODULE_MGMT(dev), i));

        /*
         * The descriptor names adapters by their position in its node array
         * while the machine keys nodes by physical slot, so the two indices
         * pass through the node. They agree in the shipped composition, and
         * a guest could not tell them apart anyway: every adapter sits at
         * the same window offset on one shared wire, so permuting which
         * adapter lands in which node changes nothing a guest can observe.
         *
         * The offset names the data register, and the control register
         * follows it by the adapter's A0 spacing inside the same 16 bytes
         * the composition validator bounds.
         */
        memory_region_add_subregion_overlap(&hub->regs,
                                            management->adapter_offsets[i],
                                            sysbus_mmio_get_region(adapter, 0),
                                            1);
    }
}

void sn0_origin2000_realize_nodes(MachineState *machine)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    size_t i;

    g_assert(s->nodes);
    for (i = 0; i < s->num_nodes; i++) {
        SN0NodeState *node = s->nodes[i];
        const SN0ModelPhysicalNode *physical = node->physical;
        unsigned int slice;

        g_assert(!DEVICE(node)->realized);
        sn0_diagnostic_attach(s, i);
        sysbus_realize(SYS_BUS_DEVICE(node), &error_fatal);
        origin2000_install_bus_error_aperture(node);
        for (slice = 0; slice < physical->cpu_count; slice++) {
            machine->possible_cpus->cpus[physical->cpu_first_id + slice].cpu =
                CPU(node->cpu[slice]);
        }
    }
    origin2000_build_management(s);
    sn0_fabric_build(s->fabric);
    origin2000_load_flash(machine);
}

static void origin2000_init(MachineState *machine)
{
    SN0MachineState *s = SN0_MACHINE(machine);

    sn0_origin2000_prepare_nodes(machine);
    if (!sn0_origin2000_xtalk_prepare(s, &error_fatal)) {
        return;
    }
    sn0_origin2000_realize_nodes(machine);
    sn0_origin2000_xtalk_attach(s, &error_fatal);
}

static const char * const origin2000_cpu_types[] = {
    MIPS_CPU_TYPE_NAME("R10000"), NULL,
};

static void origin2000_class_init(ObjectClass *klass, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);
    SN0MachineClass *smc = SN0_MACHINE_CLASS(klass);
    const SN0ModelComposition *c =
        sn0_origin2000_find_composition(sn0_origin2000_default_composition);

    if (!sn0_origin2000_validate(c, &error_fatal)) {
        return;
    }
    smc->default_composition = c;
    mc->desc = "SGI Origin 2000";
    mc->init = origin2000_init;
    mc->possible_cpu_arch_ids = origin2000_possible_cpus;
    mc->cpu_index_to_instance_props = origin2000_cpu_props;
    mc->get_default_cpu_node_id = origin2000_cpu_node;
    mc->valid_cpu_types = origin2000_cpu_types;
    mc->default_cpus = origin2000_cpu_count(c);
    mc->max_cpus = mc->default_cpus;
    mc->default_ram_size = origin2000_memory_size(c);
    mc->default_ram_id = "sn0.ram";
    mc->block_default_type = IF_SCSI;
    mc->default_nic = "sgi-ioc3-eth";
    object_class_property_add_str(klass, "composition",
                                  origin2000_get_composition,
                                  origin2000_set_composition);
}

static void origin2000_instance_init(Object *obj)
{
    MachineState *machine = MACHINE(obj);
    const SN0ModelComposition *c = SN0_MACHINE(obj)->composition;

    /* The core defaults the CPU count, leaving topology levels at one. */
    machine->smp.sockets = c->node_count;
    machine->smp.cores = c->hardware->cpu_slots_per_node;
}

static const TypeInfo origin2000_type = {
    .name = MACHINE_TYPE_NAME("origin2000"),
    .parent = TYPE_SN0_MACHINE,
    .instance_init = origin2000_instance_init,
    .class_init = origin2000_class_init,
};

static void origin2000_register_types(void)
{
    type_register_static(&origin2000_type);
}

type_init(origin2000_register_types)
