/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A machine with one SHub 1, one processor, a 16550 on the junk bus and
 * the PROM flash window, for
 * the device's qtests. It is a test fixture, not an SGI machine.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "hw/ia64/sgi-shub.h"
#include "hw/char/serial-mm.h"

#define TYPE_SHUB_TEST_MACHINE MACHINE_TYPE_NAME("x-shub1-test")

static void shub_test_init(MachineState *machine)
{
    MemoryRegion *memory = get_system_memory();
    DeviceState *dev = qdev_new(TYPE_SGI_SHUB);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    DeviceState *uart = qdev_new(TYPE_SERIAL_MM);

    cpu_create(machine->cpu_type);
    sysbus_realize_and_unref(sbd, &error_fatal);
    memory_region_add_subregion(memory, SGI_SHUB_LOCAL_MMR_BASE,
                                sysbus_mmio_get_region(sbd, 0));
    memory_region_add_subregion(memory, SGI_SHUB_GLOBAL_MMR_BASE,
                                sysbus_mmio_get_region(sbd, 1));
    qdev_prop_set_uint8(uart, "regshift", 3);
    qdev_prop_set_uint32(uart, "baudbase", 115200);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), &error_fatal);
    sgi_shub_map_junk_bus(SGI_SHUB(dev), 4,
                          sysbus_mmio_get_region(SYS_BUS_DEVICE(uart), 0),
                          &error_fatal);
    sgi_shub_add_prom_flash(SGI_SHUB(dev), NULL, NULL, 0, &error_fatal);
}

static void shub_test_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "SGI SHub 1 test fixture";
    mc->init = shub_test_init;
    mc->default_cpu_type = "madison-1600-3m-ia64-cpu";
    mc->default_cpus = 1;
    mc->min_cpus = 1;
    mc->max_cpus = 1;
    mc->default_ram_size = 0;
    mc->no_floppy = true;
    mc->no_cdrom = true;
    mc->no_parallel = true;
    mc->no_serial = true;
}

static const TypeInfo shub_test_type = {
    .name = TYPE_SHUB_TEST_MACHINE,
    .parent = TYPE_MACHINE,
    .class_init = shub_test_class_init,
};

static void shub_test_register_types(void)
{
    type_register_static(&shub_test_type);
}

type_init(shub_test_register_types)
