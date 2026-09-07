/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/datadir.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sn1.h"
#include "hw/misc/sgi-bedrock.h"
#include "hw/misc/sgi-bedrock-pi.h"
#include "hw/misc/sgi-l1.h"
#include "hw/pci-host/sgi-xbridge.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/system.h"
#include "cpu.h"
#include "cpu-qom.h"

#define SN1_CPU_CLOCK_HZ 500000000
#define SN1_RAM_SIZE (512 * MiB)
#define SN1_HUB_LOCAL_MEMORY_SIZE (256 * MiB)
#define SN1_HUB_SPECIAL_WINDOW_SIZE (128 * MiB)
#define SN1_PROM_SIZE UINT64_C(0x1686a8)

#define SN1_SPACE_HUB UINT64_C(0x10000000000)
#define SN1_SPACE_XTALK UINT64_C(0x20000000000)

#define SN1_BEDROCK_LOCAL (SN1_SPACE_XTALK + UINT64_C(0x01000000))
#define SN1_BEDROCK_NODE0 (SN1_SPACE_XTALK + UINT64_C(0x01800000))

#define SN1_HUB_LOCAL_REGISTERS (SN1_SPACE_HUB + UINT64_C(0x10000000))
#define SN1_HUB_LOCAL_BOOT (SN1_SPACE_HUB + UINT64_C(0x18000000))
#define SN1_HUB_REMOTE_REGISTERS (SN1_SPACE_HUB + UINT64_C(0x30000000))
#define SN1_HUB_REMOTE_BOOT (SN1_SPACE_HUB + UINT64_C(0x38000000))
#define SN1_PROM_RESET UINT64_C(0x1fc00000)

#define SN1_MIPS_HARDWARE_IRQ_OFFSET 2

static const VMStateDescription vmstate_sn1_machine = {
    .name = "sn1-machine",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(cpu_b_enabled, SN1MachineState),
        VMSTATE_END_OF_LIST()
    },
};

static void sn1_cpu_b_enable(void *opaque, int n, int level)
{
    SN1MachineState *s = opaque;
    CPUState *cs = s->cpu[1];

    BQL_LOCK_GUARD();
    if (level && !s->cpu_b_enabled) {
        s->cpu_b_enabled = true;
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HALT);
        cs->halted = 0;
        qemu_cpu_kick(cs);
        if (current_cpu && current_cpu != cs) {
            cpu_exit(current_cpu);
        }
    } else if (!level && s->cpu_b_enabled) {
        s->cpu_b_enabled = false;
        cpu_interrupt(cs, CPU_INTERRUPT_HALT);
    }
}

static void sn1_machine_init(Object *obj)
{
    SN1MachineState *s = SN1_MACHINE(obj);

    qemu_init_irq_child(obj, "cpu-b-enable", &s->cpu_b_enable,
                        sn1_cpu_b_enable, s, 0);
    /* Restore the tracked level before Bedrock replays its output. */
    vmstate_register(NULL, 0, &vmstate_sn1_machine, s);
}

static void sn1_machine_finalize(Object *obj)
{
    SN1MachineState *s = SN1_MACHINE(obj);

    vmstate_unregister(NULL, &vmstate_sn1_machine, s);
}

static void sn1_cpu_reset(void *opaque)
{
    cpu_reset(CPU(opaque));
}

static void sn1_machine_reset(MachineState *machine, ResetType type)
{
    SN1MachineState *s = SN1_MACHINE(machine);
    CPUState *cs;

    /* Busless MIPS CPUs are outside the registered SysBus reset tree. */
    CPU_FOREACH(cs) {
        sn1_cpu_reset(cs);
    }
    s->cpu_b_enabled = false;
    qemu_devices_reset(type);
}

static void sn1_create_cpus(SN1MachineState *s, MachineState *machine)
{
    for (unsigned int i = 0; i < SN1_PROCESSORS; i++) {
        g_autofree char *name = g_strdup_printf("cpu%u", i);
        DeviceState *cpu_dev = qdev_new(machine->cpu_type);
        MIPSCPU *cpu = MIPS_CPU(cpu_dev);

        object_property_add_child(OBJECT(machine), name, OBJECT(cpu_dev));
        object_unref(OBJECT(cpu_dev));
        object_property_set_link(OBJECT(cpu_dev), "memory",
                                 OBJECT(get_system_memory()), &error_abort);
        qdev_connect_clock_in(cpu_dev, "clk-in", s->cpu_refclk);
        object_property_set_bool(OBJECT(cpu_dev), "big-endian", true,
                                 &error_abort);
        object_property_set_bool(OBJECT(cpu_dev), "start-powered-off", i != 0,
                                 &error_abort);
        qdev_realize(cpu_dev, NULL, &error_fatal);
        cpu_mips_irq_init_cpu(cpu);
        cpu_mips_clock_init(cpu);
        s->cpu[i] = CPU(cpu);
    }
}

static bool sn1_load_prom(SN1MachineState *s, MachineState *machine,
                          Error **errp)
{
    MemoryRegion *system = get_system_memory();
    g_autoptr(GError) local_error = NULL;
    g_autofree char *contents = NULL;
    g_autofree char *path = NULL;
    gsize length;

    if (!machine->firmware) {
        error_setg(errp,
                   "origin300 requires -bios with a raw stock PROM payload");
        return false;
    }
    path = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
    if (!path) {
        error_setg(errp, "origin300: cannot find PROM image '%s'",
                   machine->firmware);
        return false;
    }
    if (!g_file_get_contents(path, &contents, &length, &local_error)) {
        error_setg(errp, "origin300: cannot read PROM image '%s': %s",
                   machine->firmware, local_error->message);
        return false;
    }
    if (length != SN1_PROM_SIZE) {
        error_setg(errp,
                   "origin300: PROM image must be exactly 1476264 bytes");
        return false;
    }
    if (machine->kernel_filename) {
        error_setg(errp, "origin300 supports stock-PROM boot only");
        return false;
    }

    if (!memory_region_init_ram_flags_nomigrate(&s->prom, OBJECT(machine),
                                                "sn1.prom", SN1_PROM_SIZE, 0,
                                                errp)) {
        return false;
    }
    memory_region_set_readonly(&s->prom, true);
    /* The stock PROM bytes are part of snapshots despite being immutable. */
    vmstate_register_ram_global(&s->prom);
    memcpy(memory_region_get_ram_ptr(&s->prom), contents, SN1_PROM_SIZE);
    memory_region_set_dirty(&s->prom, 0, SN1_PROM_SIZE);

    memory_region_init_alias(&s->prom_reset, OBJECT(machine),
                             "sn1.prom.reset", &s->prom, 0, SN1_PROM_SIZE);
    memory_region_init_alias(&s->prom_local, OBJECT(machine),
                             "sn1.prom.local", &s->prom, 0, SN1_PROM_SIZE);
    memory_region_init_alias(&s->prom_remote, OBJECT(machine),
                             "sn1.prom.remote", &s->prom, 0, SN1_PROM_SIZE);
    memory_region_add_subregion_overlap(system, SN1_PROM_RESET,
                                        &s->prom_reset, 1);
    memory_region_add_subregion(system, SN1_HUB_LOCAL_BOOT, &s->prom_local);
    memory_region_add_subregion(system, SN1_HUB_REMOTE_BOOT, &s->prom_remote);
    return true;
}

static void sn1_create_bedrock(SN1MachineState *s, MachineState *machine)
{
    static const unsigned int xbridge_widgets[] = { 0, 0xe, 0xf };
    Chardev *l1_console = serial_hd(0);
    MemoryRegion *system = get_system_memory();
    MemoryRegion *l1_uart;
    MemoryRegion *bedrock_widget;
    SysBusDevice *bedrock;

    s->l1 = qdev_new(TYPE_SGI_L1);
    object_property_add_child(OBJECT(machine), "l1", OBJECT(s->l1));
    if (l1_console) {
        qdev_prop_set_chr(s->l1, "chardev", l1_console);
    }
    sysbus_realize(SYS_BUS_DEVICE(s->l1), &error_fatal);
    object_unref(OBJECT(s->l1));
    l1_uart = sysbus_mmio_get_region(SYS_BUS_DEVICE(s->l1), 0);

    s->bedrock = qdev_new(TYPE_SGI_BEDROCK);
    object_property_add_child(OBJECT(machine), "bedrock", OBJECT(s->bedrock));
    qdev_prop_set_uint32(s->bedrock, "processor-count", SN1_PROCESSORS);
    qdev_prop_set_uint8(s->bedrock, "revision", 2);
    qdev_prop_set_uint8(s->bedrock, "local-region", 0);
    qdev_prop_set_uint8(s->bedrock, "initiator-region", 0);
    object_property_set_link(OBJECT(s->bedrock), "dma-memory", OBJECT(system),
                             &error_abort);
    object_property_set_link(OBJECT(s->bedrock), "l1-uart", OBJECT(l1_uart),
                             &error_abort);

    s->xbridge = qdev_new(TYPE_SGI_XBRIDGE);
    object_property_add_child(OBJECT(machine), "xbridge", OBJECT(s->xbridge));
    qdev_prop_set_uint8(s->xbridge, "host-port", 0xa);
    bedrock_widget = sysbus_mmio_get_region(SYS_BUS_DEVICE(s->bedrock), 2);
    object_property_set_link(OBJECT(s->xbridge), "fabric",
                             OBJECT(bedrock_widget), &error_abort);
    for (size_t i = 0; i < ARRAY_SIZE(xbridge_widgets); i++) {
        unsigned int widget = xbridge_widgets[i];
        g_autofree char *property = g_strdup_printf("widget-%u", widget);

        object_property_set_link(OBJECT(s->bedrock), property,
                                 OBJECT(sgi_xbridge_widget_view(
                                     SGI_XBRIDGE(s->xbridge), widget)),
                                 &error_abort);
    }

    sysbus_realize(SYS_BUS_DEVICE(s->bedrock), &error_fatal);
    object_unref(OBJECT(s->bedrock));
    sysbus_realize(SYS_BUS_DEVICE(s->xbridge), &error_fatal);
    object_unref(OBJECT(s->xbridge));
    bedrock = SYS_BUS_DEVICE(s->bedrock);

    qdev_connect_gpio_out_named(s->bedrock, "cpu-enable", 1,
                                &s->cpu_b_enable);

    memory_region_add_subregion(system, SN1_SPACE_XTALK,
                                sysbus_mmio_get_region(bedrock, 1));
    memory_region_add_subregion_overlap(system, SN1_BEDROCK_LOCAL,
                                        sysbus_mmio_get_region(bedrock, 0), 1);
    memory_region_init_alias(&s->bedrock_node, OBJECT(machine),
                             "sn1.bedrock.node0",
                             sysbus_mmio_get_region(bedrock, 0), 0,
                             SGI_BEDROCK_SIZE);
    memory_region_add_subregion_overlap(system, SN1_BEDROCK_NODE0,
                                        &s->bedrock_node, 1);
    memory_region_add_subregion(system, SN1_HUB_REMOTE_REGISTERS,
                                sysbus_mmio_get_region(bedrock, 3));

    for (unsigned int cpu = 0; cpu < SN1_PROCESSORS; cpu++) {
        MIPSCPU *mips_cpu = MIPS_CPU(s->cpu[cpu]);

        for (unsigned int line = 0; line < SGI_BEDROCK_PI_HW_IRQS; line++) {
            qdev_connect_gpio_out_named(
                s->bedrock, "hardware-irq",
                cpu * SGI_BEDROCK_PI_HW_IRQS + line,
                mips_cpu->env.irq[SN1_MIPS_HARDWARE_IRQ_OFFSET + line]);
        }
    }
}

static void sn1_map_memory(SN1MachineState *s, MachineState *machine)
{
    MemoryRegion *system = get_system_memory();

    memory_region_add_subregion(system, 0, machine->ram);
    memory_region_init_alias(&s->hub_memory, OBJECT(machine),
                             "sn1.hub.local-memory", machine->ram, 0,
                             SN1_HUB_LOCAL_MEMORY_SIZE);
    memory_region_add_subregion(system, SN1_SPACE_HUB, &s->hub_memory);

    memory_region_init(&s->hub_local_registers, OBJECT(machine),
                       "sn1.hub.local-registers",
                       SN1_HUB_SPECIAL_WINDOW_SIZE);
    memory_region_add_subregion(system, SN1_HUB_LOCAL_REGISTERS,
                                &s->hub_local_registers);
}

static void origin300_init(MachineState *machine)
{
    SN1MachineState *s = SN1_MACHINE(machine);

    if (machine->ram_size != SN1_RAM_SIZE) {
        error_setg(&error_fatal, "origin300 requires 512 MiB of RAM");
        return;
    }
    if (machine->smp.cpus != SN1_PROCESSORS) {
        error_setg(&error_fatal, "origin300 requires two processors");
        return;
    }

    s->cpu_refclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(s->cpu_refclk, SN1_CPU_CLOCK_HZ);

    sn1_map_memory(s, machine);
    if (!sn1_load_prom(s, machine, &error_fatal)) {
        return;
    }
    sn1_create_cpus(s, machine);
    sn1_create_bedrock(s, machine);
}

static void origin300_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        MIPS_CPU_TYPE_NAME("R14000"),
        NULL,
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "SGI Origin 300 (IP45)";
    mc->init = origin300_init;
    mc->reset = sn1_machine_reset;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("R14000");
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_cpus = 2;
    mc->min_cpus = 2;
    mc->max_cpus = 2;
    mc->default_ram_size = SN1_RAM_SIZE;
    mc->default_ram_id = "sn1.ram";
}

static const TypeInfo origin300_machine_type = {
    .name = TYPE_SN1_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(SN1MachineState),
    .instance_init = sn1_machine_init,
    .instance_finalize = sn1_machine_finalize,
    .class_init = origin300_class_init,
};

static void origin300_machine_register_types(void)
{
    type_register_static(&origin300_machine_type);
}

type_init(origin300_machine_register_types)
