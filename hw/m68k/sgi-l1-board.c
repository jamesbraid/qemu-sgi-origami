/*
 * The ColdFire controller board of an Origin 3000 C-, I- or R-brick L1.
 *
 * A stand-alone board: an MCF5206e, a 1 MiB boot flash, a 2 KiB battery
 * backed clock RAM, external static RAM and a four-channel UART, with the
 * management USB controller, the two-wire bus and the other peripherals
 * of the board left as logged, unimplemented windows. It takes the boot
 * flash contents as -bios and has no node attached.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/datadir.h"
#include "target/m68k/cpu.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/or-irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/char/serial-mm.h"
#include "hw/misc/unimp.h"
#include "hw/m68k/mcf5206e.h"
#include "hw/core/loader.h"
#include "migration/blocker.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/system.h"

#define FLASH_SIZE          (1 * MiB)
#define CS_NVRAM            1
#define CS_RAM              2
#define CS_USB0             3
#define CS_QUART            4
#define CS_USB1             5

#define QUART_CLOCK_HZ      12000000
#define QUART_CHANNELS      4
#define QUART_EXT_IRQ       4

#define RAM_SIZE_FULL       (512 * KiB)
#define RAM_SIZE_ALIASED    (128 * KiB)

typedef struct L1Board {
    M68kCPU *cpu;
    MCF5206ESIMState *sim;
    MemoryRegion flash;
    Error *migration_blocker;
} L1Board;

static uint64_t flash_read(void *opaque, hwaddr addr, unsigned size)
{
    L1Board *b = opaque;
    uint8_t *p = memory_region_get_ram_ptr(&b->flash);
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        val = (val << 8) | p[addr + i];
    }
    return val;
}

static void flash_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "sgi-l1-board flash: write%u at 0x%" HWADDR_PRIx
                  " = 0x%" PRIx64 ": the flash command set is not "
                  "modelled, ignored\n", size * 8, addr, val);
}

static const MemoryRegionOps flash_ops = {
    .read = flash_read,
    .write = flash_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void l1_board_load_flash(L1Board *b, const char *firmware,
                                Error **errp)
{
    g_autofree char *path = NULL;
    uint8_t *p;
    int size;

    memory_region_init_rom_device(&b->flash, NULL, &flash_ops, b,
                                  "sgi-l1-board.flash", FLASH_SIZE, errp);
    if (*errp) {
        return;
    }
    p = memory_region_get_ram_ptr(&b->flash);
    /* Without an image the flash is erased and the core has no vectors. */
    memset(p, 0xff, FLASH_SIZE);
    if (!firmware) {
        return;
    }
    path = qemu_find_file(QEMU_FILE_TYPE_BIOS, firmware);
    if (!path) {
        error_setg(errp, "could not find the boot flash image '%s'",
                   firmware);
        return;
    }
    size = load_image_size(path, p, FLASH_SIZE);
    if (size < 0) {
        error_setg(errp, "could not load '%s' (at most %d bytes)", path,
                   (int)FLASH_SIZE);
    }
}

static void l1_board_unimp_window(MemoryRegion *window, const char *name,
                                  uint64_t size)
{
    DeviceState *dev = qdev_new(TYPE_UNIMPLEMENTED_DEVICE);

    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_uint64(dev, "size", size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(window, 0,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));
}

static void l1_board_reset(void *opaque)
{
    L1Board *b = opaque;
    CPUState *cs = CPU(b->cpu);

    /* The SIM has been reset: CS0 is global, so the vectors come from flash. */
    cpu_reset(cs);
    b->cpu->env.aregs[7] = ldl_phys(cs->as, 0);
    b->cpu->env.pc = ldl_phys(cs->as, 4);
}

static void l1_board_init(MachineState *machine)
{
    L1Board *b = g_new0(L1Board, 1);
    DeviceState *sim_dev, *or_dev;
    MemoryRegion *quart_window;
    uint64_t ram_size = machine->ram_size;
    int i;

    if (ram_size != RAM_SIZE_FULL && ram_size != RAM_SIZE_ALIASED) {
        error_report("sgi-l1-board RAM is 512 KiB, or 128 KiB where the "
                     "size probe finds the part aliased; got %" PRIu64
                     " bytes", ram_size);
        exit(1);
    }

    b->cpu = M68K_CPU(cpu_create(machine->cpu_type));
    l1_board_load_flash(b, machine->firmware, &error_fatal);

    sim_dev = qdev_new(TYPE_MCF5206E_SIM);
    object_property_set_link(OBJECT(sim_dev), "m68k-cpu", OBJECT(b->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(sim_dev), "boot-rom", OBJECT(&b->flash),
                             &error_abort);
    /* On-chip UART 1 is the power-bay link; serial ports 0 to 3 are below. */
    if (serial_hd(4)) {
        qdev_prop_set_chr(sim_dev, "uart1-chardev", serial_hd(4));
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sim_dev), &error_fatal);
    b->sim = MCF5206E_SIM(sim_dev);

    /* CS1: clock RAM. */
    {
        DeviceState *rtc = qdev_new("sysbus-m48t02");

        sysbus_realize_and_unref(SYS_BUS_DEVICE(rtc), &error_fatal);
        memory_region_add_subregion(mcf5206e_sim_cs_window(b->sim, CS_NVRAM), 0,
                                    sysbus_mmio_get_region(SYS_BUS_DEVICE(rtc),
                                                           0));
    }

    /* CS2: external RAM, repeated through the 512 KiB window if smaller. */
    {
        MemoryRegion *window = mcf5206e_sim_cs_window(b->sim, CS_RAM);

        memory_region_add_subregion(window, 0, machine->ram);
        for (i = 1; i * ram_size < RAM_SIZE_FULL; i++) {
            MemoryRegion *alias = g_new0(MemoryRegion, 1);

            memory_region_init_alias(alias, NULL, "sgi-l1-board.ram.alias",
                                     machine->ram, 0, ram_size);
            memory_region_add_subregion(window, i * ram_size, alias);
        }
    }

    /* CS3 and CS5: management USB controller slots. CS6 and CS7 are unused. */
    l1_board_unimp_window(mcf5206e_sim_cs_window(b->sim, CS_USB0),
                          "usb-controller-slot0", 64 * KiB);
    l1_board_unimp_window(mcf5206e_sim_cs_window(b->sim, CS_USB1),
                          "cs5-device", 64 * KiB);

    /*
     * CS4: the four-channel UART. One interrupt line serves all four
     * channels, which the board presents as external level 4. Serial port
     * 0 is channel 2, the operator console; 1 is channel 3 (the node link)
     * and 2 and 3 are channels 0 and 1 (the peer links).
     */
    quart_window = mcf5206e_sim_cs_window(b->sim, CS_QUART);
    or_dev = qdev_new(TYPE_OR_IRQ);
    object_property_set_int(OBJECT(or_dev), "num-lines", QUART_CHANNELS,
                            &error_abort);
    qdev_realize_and_unref(or_dev, NULL, &error_fatal);
    qdev_connect_gpio_out(or_dev, 0,
                          qdev_get_gpio_in_named(sim_dev, "ext-irq",
                                                 QUART_EXT_IRQ));
    {
        static const int channel_serial[QUART_CHANNELS] = { 2, 3, 0, 1 };

        for (i = 0; i < QUART_CHANNELS; i++) {
            serial_mm_init(quart_window, i * 8, 0,
                           qdev_get_gpio_in(or_dev, i), QUART_CLOCK_HZ / 16,
                           serial_hd(channel_serial[i]), DEVICE_BIG_ENDIAN);
        }
    }

    error_setg(&b->migration_blocker, "sgi-l1-board does not support "
               "snapshots or migration");
    if (migrate_add_blocker(&b->migration_blocker, &error_fatal) < 0) {
        exit(1);
    }

    qemu_register_reset(l1_board_reset, b);
}

static void l1_board_machine_init(MachineClass *mc)
{
    mc->desc = "SGI L1 controller board, C-, I- and R-brick (ColdFire)";
    mc->init = l1_board_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m5206");
    mc->default_ram_id = "sgi-l1-board.ram";
    mc->default_ram_size = RAM_SIZE_FULL;
    mc->no_parallel = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
}

DEFINE_MACHINE("sgi-l1-board", l1_board_machine_init)
