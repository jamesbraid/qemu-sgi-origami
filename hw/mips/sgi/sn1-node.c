/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 300 ("SN1", IP35/Bedrock) node device. See sn1-node.h.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "block/block-global-state.h"
#include "block/block-io.h"
#include "qobject/qdict.h"
#include "chardev/char.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/mips/sn1.h"
#include "hw/misc/sgi-bedrock.h"
#include "hw/misc/sgi-bedrock-flash.h"
#include "hw/misc/sgi-bedrock-pi.h"
#include "hw/misc/sgi-l1.h"
#include "hw/pci-host/sgi-xbridge.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/system.h"
#include "cpu.h"
#include "cpu-qom.h"
#include "sn1-model.h"
#include "sn1-node.h"

/*
 * Mirrors sn1.c's own private addresses until the board-devices step
 * moves the rest of the node's address decode here too.
 */
#define SN1_SPACE_HUB UINT64_C(0x10000000000)
#define SN1_HUB_LOCAL_BOOT (SN1_SPACE_HUB + UINT64_C(0x18000000))
#define SN1_HUB_REMOTE_BOOT (SN1_SPACE_HUB + UINT64_C(0x38000000))
#define SN1_HUB_ECC (SN1_SPACE_HUB + UINT64_C(0x140000000))
#define SN1_HUB_DIRECTORY (SN1_SPACE_HUB + UINT64_C(0x1c0000000))
#define SN1_PROM_RESET UINT64_C(0x1fc00000)
#define SN1_PROM_RAM_COPY UINT64_C(0x01a00000)
#define SN1_PROM_RAM_COPY_SIZE UINT64_C(0x00200000)
#define SN1_HUB_REMOTE_PI_SIZE UINT64_C(0x80)
#define SN1_HUB_REMOTE_UART_END (SN1_HUB_REMOTE_PI_SIZE + SGI_L1_SIZE)

#define SN1_MIPS_HARDWARE_IRQ_OFFSET 2

static BlockDriverState *sn1_node_open_flash_slice(BlockDriverState *backing,
                                                    int64_t offset,
                                                    int flags, Error **errp)
{
    QDict *options = qdict_new();

    qdict_put_str(options, "driver", "raw");
    qdict_put_str(options, "file", bdrv_get_node_name(backing));
    qdict_put_int(options, "offset", offset);
    qdict_put_int(options, "size", SGI_BEDROCK_FLASH_PART_SIZE);
    return bdrv_open(NULL, NULL, options, flags, errp);
}

MemoryRegion *sn1_node_create_boot_flash(SN1NodeState *node,
                                         MachineState *machine,
                                         Error **errp)
{
    DriveInfo *dinfo = drive_get(IF_PFLASH, 0, 0);
    DriveInfo *extra = drive_get(IF_PFLASH, 0, 1);
    BlockBackend *blk;
    BlockDriverState *backing;
    BlockDriverState *lower;
    BlockDriverState *upper;
    int flags;
    SysBusDevice *flash;

    if (!dinfo && !extra) {
        return NULL;
    }
    if (!dinfo || extra) {
        error_setg(errp, "origin300 accepts one 16 MiB pflash image");
        return NULL;
    }

    blk = blk_by_legacy_dinfo(dinfo);
    backing = blk_bs(blk);
    if (strcmp(bdrv_get_format_name(backing), "raw")) {
        error_setg(errp, "origin300 pflash image must use raw format");
        return NULL;
    }
    if (blk_getlength(blk) != SGI_BEDROCK_FLASH_SIZE) {
        error_setg(errp, "origin300 pflash image must be exactly 16 MiB");
        return NULL;
    }

    flags = bdrv_get_flags(backing) & (BDRV_O_RDWR | BDRV_O_INACTIVE);
    lower = sn1_node_open_flash_slice(backing, 0, flags, errp);
    if (!lower) {
        return NULL;
    }
    upper = sn1_node_open_flash_slice(backing, SGI_BEDROCK_FLASH_PART_SIZE,
                                      flags, errp);
    if (!upper) {
        bdrv_unref(lower);
        return NULL;
    }

    if (blk_replace_bs(blk, lower, errp) < 0) {
        bdrv_unref(upper);
        bdrv_unref(lower);
        return NULL;
    }
    bdrv_unref(lower);

    node->boot_flash = qdev_new(TYPE_SGI_BEDROCK_FLASH);
    object_property_add_child(OBJECT(node), "boot-flash",
                              OBJECT(node->boot_flash));
    if (!qdev_prop_set_drive_err(node->boot_flash, "drive0", blk, errp) ||
        !object_property_set_str(OBJECT(node->boot_flash), "drive1",
                                 bdrv_get_node_name(upper), errp)) {
        bdrv_unref(upper);
        return NULL;
    }
    bdrv_unref(upper);
    flash = SYS_BUS_DEVICE(node->boot_flash);
    if (!sysbus_realize_and_unref(flash, errp)) {
        return NULL;
    }

    /*
     * One region per boot space, each the whole 128 MiB window rather than a
     * 16 MiB selection: reads and writes decode differently inside it, so the
     * alternate selection is a read window within the space and a store is
     * addressed from the space's base. Mapping the selections separately
     * would put every write offset 32 MiB out.
     */
    sysbus_mmio_map(flash, 0, SN1_HUB_LOCAL_BOOT);
    sysbus_mmio_map(flash, 1, SN1_HUB_REMOTE_BOOT);
    /* The processor executes position zero through read-only views. */
    return SGI_BEDROCK_FLASH(node->boot_flash)->flash_mmio[0];
}

static target_ulong sn1_node_store_transaction(MIPSCPU *cpu, MemoryRegion *mr,
                                               hwaddr offset,
                                               target_ulong value,
                                               unsigned size, void *opaque)
{
    SN1NodeState *node = opaque;
    SGIBedrockState *bedrock = SGI_BEDROCK(node->bedrock);

    (void)cpu;
    if (mr == &bedrock->md.ecc_mmio) {
        return sgi_bedrock_md_ecc_store_value(offset, value, size);
    }
    return value;
}

static unsigned sn1_cpu_slot(const SN1MachineModel *model, unsigned logical)
{
    for (unsigned physical = 0; physical < 4; physical++) {
        if (model->cpu_population_mask & (1U << physical)) {
            if (logical == 0) {
                return physical;
            }
            logical--;
        }
    }
    g_assert_not_reached();
}

static void sn1_node_create_cpus(SN1NodeState *node, MachineState *machine)
{
    MemoryRegion *system = get_system_memory();
    const SN1MachineModel *model = &sn1_origin300_model;
    unsigned int i;

    if (node->promdata_filter) {
        sn_promdata_poll_init(&node->promdata_poll, node->promdata,
                              SGI_SN1_NODE_MAX_CPUS, node->promdata_fast,
                              NULL, NULL);
    }

    for (i = 0; i < model->cpu_count; i++) {
        g_autofree char *name = g_strdup_printf("cpu%u", i);
        g_autofree char *alias_name =
            g_strdup_printf("sn1.node0.cpu%u.system", i);
        g_autofree char *sysmem_name =
            g_strdup_printf("sn1.node0.cpu%u.memory", i);
        DeviceState *cpu_dev = qdev_new(machine->cpu_type);
        MIPSCPU *cpu = MIPS_CPU(cpu_dev);

        /*
         * Every existing address still resolves through this alias exactly
         * as it did through the shared system region directly; only the
         * private window below adds anything on top, at higher priority.
         */
        memory_region_init(&node->cpu_sysmem[i], OBJECT(node), sysmem_name,
                           UINT64_MAX);
        memory_region_init_alias(&node->sysmem_alias[i], OBJECT(node),
                                 alias_name, system, 0, UINT64_MAX);
        memory_region_add_subregion_overlap(&node->cpu_sysmem[i], 0,
                                            &node->sysmem_alias[i], -2);

        object_property_add_child(OBJECT(node), name, OBJECT(cpu_dev));
        object_unref(OBJECT(cpu_dev));
        object_property_set_link(OBJECT(cpu_dev), "memory",
                                 OBJECT(&node->cpu_sysmem[i]), &error_abort);
        qdev_connect_clock_in(cpu_dev, "clk-in", node->cpuclk);
        object_property_set_bool(OBJECT(cpu_dev), "big-endian", true,
                                 &error_abort);
        /* Release both vCPUs from the machine reset handler, not realize. */
        object_property_set_bool(OBJECT(cpu_dev), "start-powered-off", true,
                                 &error_abort);
        cpu->store_transaction = sn1_node_store_transaction;
        cpu->store_transaction_opaque = node;
        qdev_realize(cpu_dev, NULL, &error_fatal);
        cpu_mips_irq_init_cpu(cpu);
        cpu_mips_clock_init(cpu);
        node->cpu[i] = cpu;

        if (node->promdata_filter) {
            g_autofree char *promdata_prefix =
                g_strdup_printf("sn1.node0.cpu%u", i);

            /* The IP35 PROM's RAM copy executes within one 2 MiB TLB span. */
            sn_promdata_init(&node->promdata[i], OBJECT(node), cpu,
                             &node->promdata_poll, node->promdata_fast,
                             0, model->ram_size, SN1_PROM_RESET,
                             model->prom_size, SN1_PROM_RAM_COPY,
                             SN1_PROM_RAM_COPY_SIZE,
                             &node->cpu_sysmem[i], 0,
                             &node->cpu_uncached[i],
                             &node->cpu_uncached_alias[i],
                             promdata_prefix);
        }
    }
}

static void sn1_node_create_bedrock(SN1NodeState *node,
                                    MachineState *machine)
{
    const SN1MachineModel *model = &sn1_origin300_model;
    Chardev *l1_console = serial_hd(0);
    MemoryRegion *system = get_system_memory();
    MemoryRegion *l1_uart;
    DeviceState *l1_uart_dev;
    MemoryRegion *bedrock_widget;
    MemoryRegion *hub_registers;
    SysBusDevice *bedrock;

    node->l1 = qdev_new(TYPE_SGI_L1);
    object_property_add_child(OBJECT(node), "l1", OBJECT(node->l1));
    if (l1_console) {
        qdev_prop_set_chr(node->l1, "chardev", l1_console);
    }
    sn1_set_l1_identity_records(SN1_MACHINE(machine), node->l1);
    /* The revision every retained Origin 300 capture pairs with PROM 6.210. */
    qdev_prop_set_int32(node->l1, "revision-major", 1);
    qdev_prop_set_int32(node->l1, "revision-minor", 18);
    qdev_prop_set_int32(node->l1, "revision-patch", 5);
    qdev_realize(node->l1, NULL, &error_fatal);
    object_unref(OBJECT(node->l1));

    l1_uart_dev = qdev_new(TYPE_SGI_L1_UART);
    object_property_add_child(OBJECT(node), "l1-uart", OBJECT(l1_uart_dev));
    object_property_set_link(OBJECT(l1_uart_dev), "controller",
                             OBJECT(node->l1), &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(l1_uart_dev), &error_fatal);
    object_unref(OBJECT(l1_uart_dev));
    l1_uart = sysbus_mmio_get_region(SYS_BUS_DEVICE(l1_uart_dev), 0);

    node->bedrock = qdev_new(TYPE_SGI_BEDROCK);
    object_property_add_child(OBJECT(node), "bedrock",
                              OBJECT(node->bedrock));
    qdev_prop_set_uint32(node->bedrock, "processor-count", model->cpu_count);
    qdev_prop_set_uint32(node->bedrock, "processor-mask",
                        model->cpu_population_mask);
    qdev_prop_set_uint8(node->bedrock, "revision", model->bedrock_revision);
    qdev_prop_set_uint8(node->bedrock, "local-region",
                        model->bedrock_local_region);
    qdev_prop_set_uint8(node->bedrock, "initiator-region",
                        model->bedrock_initiator_region);
    object_property_set_link(OBJECT(node->bedrock), "dma-memory",
                             OBJECT(system), &error_abort);
    object_property_set_link(OBJECT(node->bedrock), "node-memory",
                             OBJECT(node->ram), &error_abort);
    object_property_set_link(OBJECT(node->bedrock), "l1-uart",
                             OBJECT(l1_uart), &error_abort);

    node->xbridge = qdev_new(TYPE_SGI_XBRIDGE);
    object_property_add_child(OBJECT(node), "xbridge",
                              OBJECT(node->xbridge));
    qdev_prop_set_uint8(node->xbridge, "host-port", model->xbridge_host_port);
    /* Widget 15's responder is a compatibility choice, not observed wiring. */
    qdev_prop_set_string(node->xbridge, "board-part-f",
                         SGI_XBRIDGE_NIC_PLACEHOLDER_PART);
    bedrock_widget = sysbus_mmio_get_region(SYS_BUS_DEVICE(node->bedrock), 2);
    object_property_set_link(OBJECT(node->xbridge), "fabric",
                             OBJECT(bedrock_widget), &error_abort);
    for (size_t i = 0; i < model->num_xbridge_widgets; i++) {
        unsigned int widget = model->xbridge_widgets[i];
        g_autofree char *property = g_strdup_printf("widget-%u", widget);

        object_property_set_link(OBJECT(node->bedrock), property,
                                 OBJECT(sgi_xbridge_widget_view(
                                     SGI_XBRIDGE(node->xbridge), widget)),
                                 &error_abort);
    }

    sysbus_realize(SYS_BUS_DEVICE(node->bedrock), &error_fatal);
    object_unref(OBJECT(node->bedrock));
    sysbus_realize(SYS_BUS_DEVICE(node->xbridge), &error_fatal);
    object_unref(OBJECT(node->xbridge));
    bedrock = SYS_BUS_DEVICE(node->bedrock);
    hub_registers = &SGI_BEDROCK(node->bedrock)->hub_special_registers;

    memory_region_add_subregion(system, model->xtalk_address,
                                sysbus_mmio_get_region(bedrock, 1));
    memory_region_add_subregion_overlap(system, model->bedrock_local_address,
                                        sysbus_mmio_get_region(bedrock, 0),
                                        1);
    memory_region_init_alias(&node->bedrock_node, OBJECT(node),
                             "sn1.bedrock.node0",
                             sysbus_mmio_get_region(bedrock, 0), 0,
                             model->bedrock_node.size);
    memory_region_add_subregion_overlap(system, model->bedrock_node.address,
                                        &node->bedrock_node, 1);
    memory_region_init_alias(&node->hub_local_register_block, OBJECT(node),
                             "sn1.hub.local-register-block",
                             hub_registers, 0,
                             SGI_BEDROCK_SIZE);
    memory_region_add_subregion(&SN1_MACHINE(machine)->hub_local_registers, 0,
                                &node->hub_local_register_block);
    /* OpenBSD expects local PI accesses to select the requester's interface. */
    if (sn1_cpu_slot(model, 1) == 2) {
        SGIBedrockState *bedrock_state = SGI_BEDROCK(node->bedrock);
        MemoryRegion *pi1 = sysbus_mmio_get_region(
            SYS_BUS_DEVICE(&bedrock_state->pi[1]), 0);

        memory_region_init_alias(&node->cpu_c_local_pi_xtalk, OBJECT(node),
                                 "sn1.cpu-c.local-pi-xtalk", pi1, 0,
                                 SGI_BEDROCK_PI_SIZE);
        memory_region_add_subregion_overlap(&node->cpu_sysmem[1],
                                            model->bedrock_local_address,
                                            &node->cpu_c_local_pi_xtalk, 1);
        memory_region_init_alias(&node->cpu_c_local_pi_hub, OBJECT(node),
                                 "sn1.cpu-c.local-pi-hub", pi1, 0,
                                 SGI_BEDROCK_PI_SIZE);
        memory_region_add_subregion_overlap(&node->cpu_sysmem[1],
                                            model->hub_local_registers.address,
                                            &node->cpu_c_local_pi_hub, 1);
    }
    memory_region_add_subregion(system, model->hub_remote_registers_address,
                                sysbus_mmio_get_region(bedrock, 3));
    /* Preserve the remote L1 UART decoder between the register aliases. */
    memory_region_init_alias(&node->hub_remote_register_head, OBJECT(node),
                             "sn1.hub.remote-register-head",
                             hub_registers, 0,
                             SN1_HUB_REMOTE_PI_SIZE);
    memory_region_add_subregion_overlap(
        system, model->hub_remote_registers_address,
        &node->hub_remote_register_head, 1);
    memory_region_init_alias(&node->hub_remote_register_tail, OBJECT(node),
                             "sn1.hub.remote-register-tail",
                             hub_registers,
                             SN1_HUB_REMOTE_UART_END,
                             SGI_BEDROCK_SIZE - SN1_HUB_REMOTE_UART_END);
    memory_region_add_subregion_overlap(
        system, model->hub_remote_registers_address +
                SN1_HUB_REMOTE_UART_END,
        &node->hub_remote_register_tail, 1);
    /*
     * The directory back door is the last part of the node's address space,
     * one word per fitted memory line. Firmware initialises it before it
     * trusts memory, so nothing boots without it.
     */
    memory_region_add_subregion(system, SN1_HUB_DIRECTORY,
                                sysbus_mmio_get_region(bedrock, 5));
    /*
     * The ECC back door sits below the directory in the same back-door area,
     * one byte per eight bytes of node memory. Both apertures take their
     * extent from the memory linked above, so neither answers anything until
     * that link is set.
     */
    memory_region_add_subregion(system, SN1_HUB_ECC,
                                sysbus_mmio_get_region(bedrock, 6));

    for (unsigned int cpu = 0; cpu < model->cpu_count; cpu++) {
        MIPSCPU *mips_cpu = node->cpu[cpu];
        unsigned physical = sn1_cpu_slot(model, cpu);

        qdev_connect_gpio_out_named(
            node->bedrock, "nmi", physical,
            qdev_get_gpio_in_named(DEVICE(mips_cpu), "nmi", 0));
        for (unsigned int line = 0; line < SGI_BEDROCK_PI_HW_IRQS; line++) {
            qdev_connect_gpio_out_named(
                node->bedrock, "hardware-irq",
                physical * SGI_BEDROCK_PI_HW_IRQS + line,
                mips_cpu->env.irq[SN1_MIPS_HARDWARE_IRQ_OFFSET + line]);
        }
    }
}

static void sn1_node_realize(DeviceState *dev, Error **errp)
{
    SN1NodeState *node = SN1_NODE(dev);
    MachineState *machine = MACHINE(qdev_get_machine());

    sn1_node_create_cpus(node, machine);
    sn1_node_create_bedrock(node, machine);
}

static void sn1_node_unrealize(DeviceState *dev)
{
    SN1NodeState *node = SN1_NODE(dev);

    sn_promdata_poll_free(&node->promdata_poll);
}

/*
 * The per-CPU cache-as-RAM private window (sn-promdata.c, shared with
 * sgi-sn0-node) is per-slot state the node owns; the array-level wrapper
 * below is per node type because it names SN1NodeState's own promdata[]
 * field, the same way sn0.c's vmstate_sn0_flash names SN0NodeState's.
 * There is no prior wire format to preserve here: this is the node's
 * first VMStateDescription.
 */
static bool sn1_promdata_needed(void *opaque)
{
    SN1NodeState *node = opaque;

    return node->promdata[0].present;
}

static int sn1_promdata_post_load(void *opaque, int version_id)
{
    SN1NodeState *node = opaque;
    unsigned int slice;

    for (slice = 0; slice < SGI_SN1_NODE_MAX_CPUS; slice++) {
        sn_promdata_apply_load(&node->promdata[slice]);
    }
    return 0;
}

static const VMStateDescription vmstate_sn1_node_promdata = {
    .name = "sn1-node/promdata",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sn1_promdata_needed,
    .post_load = sn1_promdata_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(promdata, SN1NodeState, SGI_SN1_NODE_MAX_CPUS, 1,
                             vmstate_sn_promdata_slice, SNPromDataState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sn1_node = {
    .name = "sn1-node",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sn1_node_promdata,
        NULL
    }
};

static void sn1_node_instance_init(Object *obj)
{
    SN1NodeState *node = SN1_NODE(obj);

    node->cpuclk = qdev_init_clock_in(DEVICE(obj), "cpu-refclk", NULL, NULL,
                                      0);
}

static const Property sn1_node_properties[] = {
    DEFINE_PROP_LINK("ram", SN1NodeState, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_BOOL("promdata-filter", SN1NodeState, promdata_filter, true),
    /*
     * Off by default: the fast path (sn-promdata.c) is proven on sgi-sn0-
     * node's own boot and migration coverage, but nothing here yet
     * exercises it under Bedrock. Sharing the implementation lets it be
     * turned on once that evidence exists, without a second copy.
     */
    DEFINE_PROP_BOOL("promdata-fast", SN1NodeState, promdata_fast, false),
};

static void sn1_node_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = sn1_node_realize;
    dc->unrealize = sn1_node_unrealize;
    dc->vmsd = &vmstate_sn1_node;
    device_class_set_props(dc, sn1_node_properties);
    /* The machine wires a node's RAM and clock; -device cannot. */
    dc->user_creatable = false;
}

static const TypeInfo sn1_node_types[] = {
    {
        .name              = TYPE_SGI_SN1_NODE,
        .parent            = TYPE_SYS_BUS_DEVICE,
        .instance_size     = sizeof(SN1NodeState),
        .instance_init     = sn1_node_instance_init,
        .class_init        = sn1_node_class_init,
    },
};

DEFINE_TYPES(sn1_node_types)
