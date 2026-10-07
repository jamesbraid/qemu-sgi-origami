/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * Synthetic O2 (IP32) register and wiring test profile.
 * This machine pairs catalogued O2 module 030-1247-001 (195 MHz R10000)
 * with a synthetic 64 MiB fit. Reset straps and the DIMM-to-bank-register
 * map are unknown.
 * It has no fitted flash, PCI storage or product catalogue offering.
 * Serial A accepts an explicit host backend; flash write control and
 * migration remain unavailable here.
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/mips/mips.h"
#include "hw/mips/sgi/crime.h"
#include "hw/mips/sgi/o2-rom.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "qemu/module.h"
#include "qemu/notify.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "target/mips/cpu.h"

#define O2_RAM_SIZE (64 * MiB)
#define O2_CRIME_BASE 0x14000000ULL
#define O2_MACE_BASE 0x1f000000ULL
#define O2_ROM_BASE 0x1fc00000ULL

typedef struct O2MachineState {
    MachineState parent;
    char *rom_image;
    uint32_t rom_size;
    uint32_t rom_offset;
    uint16_t rom_pad_byte;
    MemoryRegion cpu_uncached;
    MemoryRegion cpu_uncached_base;
    MemoryRegion cpu_uncached_attr0;
    Clock *cpuclk;
    PCIBus *pci_bus;
    Notifier machine_done;
    bool machine_done_registered;
} O2MachineState;

static void o2_get_cpu_clock_hz(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;
    uint64_t hz = s->cpuclk ? clock_get_hz(s->cpuclk) : 0;

    visit_type_uint64(v, name, &hz, errp);
}

static void o2_note_pci_device(PCIBus *bus, PCIDevice *dev, void *opaque)
{
    *(bool *)opaque = true;
}

static void o2_machine_init_done(Notifier *notifier, void *data)
{
    O2MachineState *s = container_of(notifier, O2MachineState, machine_done);
    bool attached = false;

    pci_for_each_device_under_bus(s->pci_bus, o2_note_pci_device, &attached);
    if (attached) {
        error_report("o2-test does not support attached PCI devices");
        exit(1);
    }
}

static char *o2_get_rom_image(Object *obj, Error **errp)
{
    return g_strdup(((O2MachineState *)obj)->rom_image);
}

static void o2_set_rom_image(Object *obj, const char *value, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    g_free(s->rom_image);
    s->rom_image = g_strdup(value);
}

static void o2_instance_init(Object *obj)
{
    O2MachineState *s = (O2MachineState *)obj;

    s->rom_offset = UINT32_MAX;
    s->rom_pad_byte = UINT16_MAX;
}

static void o2_instance_finalize(Object *obj)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (s->machine_done_registered) {
        qemu_remove_machine_init_done_notifier(&s->machine_done);
    }
    g_free(s->rom_image);
}

static void o2_init(MachineState *machine)
{
    O2MachineState *s = (O2MachineState *)machine;
    Clock *cpuclk;
    MIPSCPU *cpu;
    DeviceState *cpu_dev;
    DeviceState *crime;
    DeviceState *mace;
    DeviceState *rom;
    Error *err = NULL;

    if (machine->ram_size != O2_RAM_SIZE) {
        error_report("o2-test requires 64 MiB of RAM");
        exit(1);
    }
    if (machine->ram_slots || machine->maxram_size != machine->ram_size) {
        error_report("o2-test does not support memory slots or extra RAM");
        exit(1);
    }
    if (machine->smp.cpus != 1) {
        error_report("o2-test requires one CPU");
        exit(1);
    }
    if (machine->usb) {
        error_report("o2-test does not support USB devices");
        exit(1);
    }
    if (serial_hd(1)) {
        error_report("o2-test supports only one host serial backend");
        exit(1);
    }
    if (machine->firmware) {
        error_report("o2-test does not support -bios");
        exit(1);
    }
    if (machine->kernel_filename) {
        error_report("o2-test does not support -kernel");
        exit(1);
    }
    if (machine->initrd_filename) {
        error_report("o2-test does not support -initrd");
        exit(1);
    }
    if (machine->kernel_cmdline && machine->kernel_cmdline[0]) {
        error_report("o2-test does not support -append");
        exit(1);
    }
    if (!s->rom_image && (s->rom_size || s->rom_offset != UINT32_MAX ||
                          s->rom_pad_byte != UINT16_MAX)) {
        error_report("o2-test ROM settings require rom-image");
        exit(1);
    }
    if (s->rom_image && (!s->rom_size || s->rom_offset == UINT32_MAX ||
                         s->rom_pad_byte == UINT16_MAX)) {
        error_report("o2-test ROM requires rom-size, rom-offset and "
                     "rom-pad-byte");
        exit(1);
    }

    /* Catalogued speed; external SysClk and reset straps are unknown. */
    cpuclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(cpuclk, 195 * 1000 * 1000);
    s->cpuclk = cpuclk;
    memory_region_init(&s->cpu_uncached, OBJECT(machine),
                       "o2.cpu-uncached", UINT64_MAX);
    memory_region_init_alias(&s->cpu_uncached_base, OBJECT(machine),
                             "o2.cpu-uncached.base", get_system_memory(),
                             0, 1ULL << MIPS_UNCACHED_WINDOW_SHIFT);
    memory_region_add_subregion(&s->cpu_uncached, 0,
                                &s->cpu_uncached_base);
    /* R10000 uncached attribute 0 adds this internal 1 TiB offset. */
    memory_region_init_alias(&s->cpu_uncached_attr0, OBJECT(machine),
                             "o2.cpu-uncached.attr0", get_system_memory(),
                             0, 1ULL << MIPS_UNCACHED_WINDOW_SHIFT);
    memory_region_add_subregion(&s->cpu_uncached,
                                1ULL << MIPS_UNCACHED_WINDOW_SHIFT,
                                &s->cpu_uncached_attr0);
    cpu_dev = qdev_new(machine->cpu_type);
    qdev_connect_clock_in(cpu_dev, "clk-in", cpuclk);
    object_property_set_bool(OBJECT(cpu_dev), "big-endian", true,
                             &error_fatal);
    object_property_set_link(OBJECT(cpu_dev), "uncached-memory",
                             OBJECT(&s->cpu_uncached), &error_fatal);
    qdev_prop_set_bit(cpu_dev, "cache-data-visibility", true);
    if (!qdev_realize_and_unref(cpu_dev, NULL, &err)) {
        error_report_err(err);
        exit(1);
    }
    cpu = MIPS_CPU(cpu_dev);
    cpu_mips_irq_init_cpu(cpu);
    cpu_mips_clock_init(cpu);

    crime = qdev_new(TYPE_SGI_CRIME);
    object_property_add_child(OBJECT(machine), "crime", OBJECT(crime));
    qdev_prop_set_uint8(crime, "interface-revision", 1);
    object_property_set_link(OBJECT(crime), "ram", OBJECT(machine->ram),
                             &error_fatal);
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(crime), &err)) {
        error_report_err(err);
        exit(1);
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(crime), 0, O2_CRIME_BASE);
    for (unsigned ip = 0; ip < 5; ip++) {
        qdev_connect_gpio_out_named(crime, "cpu-ip", ip,
                                    cpu->env.irq[ip + 2]);
    }

    mace = qdev_new("sgi-mace");
    object_property_add_child(OBJECT(machine), "mace", OBJECT(mace));
    if (serial_hd(0)) {
        qdev_prop_set_chr(mace, "chardev-a", serial_hd(0));
    }
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(mace), &err)) {
        error_report_err(err);
        exit(1);
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(mace), 0, O2_MACE_BASE);
    for (unsigned cause = 0; cause < 16; cause++) {
        qdev_connect_gpio_out_named(mace, "crime", cause,
                                    qdev_get_gpio_in_named(crime, "mace",
                                                           cause));
    }
    s->pci_bus = PCI_HOST_BRIDGE(mace)->bus;
    s->machine_done.notify = o2_machine_init_done;
    qemu_add_machine_init_done_notifier(&s->machine_done);
    s->machine_done_registered = true;

    if (s->rom_image) {
        rom = qdev_new(TYPE_SGI_O2_ROM);
        object_property_add_child(OBJECT(machine), "rom", OBJECT(rom));
        qdev_prop_set_string(rom, "image", s->rom_image);
        qdev_prop_set_uint32(rom, "flash-size", s->rom_size);
        qdev_prop_set_uint32(rom, "image-offset", s->rom_offset);
        qdev_prop_set_uint16(rom, "pad-byte", s->rom_pad_byte);
        if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(rom), &err)) {
            error_report_err(err);
            exit(1);
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(rom), 0, O2_ROM_BASE);
    }
}

static void o2_class_init(ObjectClass *oc, const void *data)
{
    static const char * const cpu_types[] = {
        MIPS_CPU_TYPE_NAME("R10000"), NULL,
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add_str(oc, "rom-image", o2_get_rom_image,
                                  o2_set_rom_image);
    object_class_property_add(oc, "cpu-clock-hz", "uint64",
                              o2_get_cpu_clock_hz, NULL, NULL, NULL);
    object_class_property_add_uint32_ptr(oc, "rom-size",
        offsetof(O2MachineState, rom_size), OBJ_PROP_FLAG_READWRITE);
    object_class_property_add_uint32_ptr(oc, "rom-offset",
        offsetof(O2MachineState, rom_offset), OBJ_PROP_FLAG_READWRITE);
    object_class_property_add_uint16_ptr(oc, "rom-pad-byte",
        offsetof(O2MachineState, rom_pad_byte), OBJ_PROP_FLAG_READWRITE);
    mc->desc = "O2 (IP32) synthetic register test profile";
    mc->init = o2_init;
    mc->valid_cpu_types = cpu_types;
    mc->default_cpu_type = cpu_types[0];
    mc->default_cpus = 1;
    mc->max_cpus = 1;
    mc->default_ram_size = O2_RAM_SIZE;
    mc->default_ram_id = "o2.ram";
    mc->default_display = "none";
    mc->no_serial = true;
    mc->no_parallel = true;
    mc->no_floppy = true;
    mc->no_cdrom = true;
    mc->auto_create_sdcard = false;
}

static const TypeInfo o2_machine_type = {
    .name = MACHINE_TYPE_NAME("o2-test"),
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(O2MachineState),
    .instance_init = o2_instance_init,
    .instance_finalize = o2_instance_finalize,
    .class_init = o2_class_init,
};

static void o2_machine_register(void)
{
    type_register_static(&o2_machine_type);
}

type_init(o2_machine_register)
