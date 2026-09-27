/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Graphics-free Octane (IP30) functional profile.
 * The public board sources fix the widget and PCI topology. The selected
 * RAM descriptor, firmware loading policy, PCI slot assignments, and SuperIO
 * interrupt pin are functional choices; firmware boot remains unverified.
 */
#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/loader.h"
#include "qemu/module.h"
#include "system/reset.h"
#include "qemu/bswap.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/sysbus.h"
#include "hw/audio/sgi-rad1-audio.h"
#include "hw/mips/mips.h"
#include "hw/mips/sgi-xtalk-port.h"
#include "hw/misc/sgi-heart.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/misc/sgi-xbow.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/pci/pci.h"
#include "hw/scsi/qlogic-isp.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "target/mips/cpu.h"

#define OCTANE_SHORT_BASE 0x10000000ULL
#define OCTANE_SHORT_SPAN (16 * MiB)
#define OCTANE_LONG_BRIDGE (15ULL << 36)
#define OCTANE_FLASH_SIZE (2 * MiB)
#define OCTANE_HEART_REGS 0x0ff00000ULL
#define OCTANE_HEART_WIDGET 0x18000000ULL
#define OCTANE_BANK_UNIT (32 * MiB)
#define OCTANE_PHYSICAL_SPAN (1ULL << 40)
#define OCTANE_ATTR0_BASE (1ULL << 40)
#define OCTANE_LOW_RAM_SIZE 0x2000
#define OCTANE_HIGH_GUARD_BASE 0x20000000ULL
#define OCTANE_HIGH_GUARD_SIZE 0x4000

typedef struct OctaneMachineState {
    MachineState parent;
    MemoryRegion low_ram;
    MemoryRegion high_guard;
    uint64_t bank_size[HEART_BANKS];
    bool octane2;
} OctaneMachineState;

static bool octane_valid_banks(OctaneMachineState *s, bool octane2)
{
    uint64_t *bank = s->bank_size;

    if (octane2) {
        return (bank[0] == 256 * MiB && !bank[1] && !bank[2] && !bank[3]) ||
               (bank[0] == 128 * MiB && bank[1] == 128 * MiB &&
                !bank[2] && !bank[3]) ||
               (bank[0] == 64 * MiB && bank[1] == 64 * MiB &&
                bank[2] == 64 * MiB && bank[3] == 64 * MiB);
    }
    return (bank[0] == 128 * MiB && !bank[1] && !bank[2] && !bank[3]) ||
           (bank[0] == 64 * MiB && bank[1] == 64 * MiB &&
            !bank[2] && !bank[3]);
}

static MemTxResult octane_high_guard_read(void *opaque, hwaddr addr,
                                          uint64_t *value, unsigned size,
                                          MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP, "octane: unsupported %u-byte read at "
                  "physical 0x%" HWADDR_PRIx "\n", size,
                  (hwaddr)(OCTANE_HIGH_GUARD_BASE + addr));
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult octane_high_guard_write(void *opaque, hwaddr addr,
                                           uint64_t value, unsigned size,
                                           MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP, "octane: unsupported %u-byte write at "
                  "physical 0x%" HWADDR_PRIx "\n", size,
                  (hwaddr)(OCTANE_HIGH_GUARD_BASE + addr));
    return MEMTX_ERROR;
}

static const MemoryRegionOps octane_high_guard_ops = {
    .read_with_attrs = octane_high_guard_read,
    .write_with_attrs = octane_high_guard_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

static uint64_t octane_flash_read(void *opaque, hwaddr addr, unsigned size)
{
    const uint8_t *image = opaque;

    return lduw_be_p(image + addr);
}

static MemTxResult octane_flash_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size,
                                      MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "octane: write to read-only firmware at 0x%" HWADDR_PRIx
                  "\n", addr);
    return MEMTX_ERROR;
}

static const MemoryRegionOps octane_flash_ops = {
    .read = octane_flash_read,
    .write_with_attrs = octane_flash_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 2,
    .impl.min_access_size = 2,
    .impl.max_access_size = 2,
};

static uint64_t octane_blank_flash_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    return 0xffff;
}

static void octane_blank_flash_write(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "octane: unsupported alternate flash write "
                  "at 0x%" HWADDR_PRIx " value 0x%" PRIx64 "\n",
                  addr, value);
}

static const MemoryRegionOps octane_blank_flash_ops = {
    .read = octane_blank_flash_read,
    .write = octane_blank_flash_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 2 },
    .impl = { .min_access_size = 2, .max_access_size = 2 },
};

static void octane_reset(MachineState *machine, ResetType type)
{
    OctaneMachineState *s = (OctaneMachineState *)machine;
    uint8_t descriptor[4];
    uint32_t base = 0;
    CPUState *cpu;

    qemu_devices_reset(type);
    memset(memory_region_get_ram_ptr(&s->low_ram), 0, OCTANE_LOW_RAM_SIZE);
    memory_region_set_dirty(&s->low_ram, 0, OCTANE_LOW_RAM_SIZE);
    /* Synthetic machine setup; the PROM may replace these descriptors. */
    for (unsigned bank = 0; bank < HEART_BANKS; bank++) {
        uint64_t capacity = s->bank_size[bank];
        uint32_t value = capacity ?
            0x80000000U | ((capacity / OCTANE_BANK_UNIT - 1) << 16) | base : 0;

        stl_be_p(descriptor, value);
        if (address_space_write(&address_space_memory,
                                OCTANE_HEART_REGS + 0x20 + bank * 4,
                                MEMTXATTRS_UNSPECIFIED, descriptor,
                                sizeof(descriptor)) != MEMTX_OK) {
            error_setg(&error_fatal, "octane cannot map HEART bank %u", bank);
        }
        base += capacity / OCTANE_BANK_UNIT;
    }
    CPU_FOREACH(cpu) {
        cpu_reset(cpu);
    }
}

static void octane_fit_pci(MachineState *machine, SGIBridgeState *bridge)
{
    PCIBus *bus = PCI_HOST_BRIDGE(bridge)->bus;
    PCIDevice *pci;

    for (unsigned slot = 0; slot < 2; slot++) {
        pci = pci_new(PCI_DEVFN(slot, 0), TYPE_QLOGIC_ISP1040);
        object_property_add_child(OBJECT(machine),
                                  slot ? "external-scsi" : "internal-scsi",
                                  OBJECT(pci));
        pci_realize_and_unref(pci, bus, &error_fatal);
        qlogic_isp_handle_legacy_cmdline(DEVICE(pci));
    }

    pci = pci_new(PCI_DEVFN(2, 0), TYPE_SGI_IOC3);
    object_property_add_child(OBJECT(machine), "ioc3", OBJECT(pci));
    qdev_prop_set_uint16(DEVICE(pci), "subsystem-id",
                         SGI_IOC3_SUBSYS_IP30_SYSBOARD);
    qdev_prop_set_uint32(DEVICE(pci), "population",
                         SGI_IOC3_POPULATION_SERIAL_A |
                         SGI_IOC3_POPULATION_SERIAL_B |
                         SGI_IOC3_POPULATION_ETHERNET |
                         SGI_IOC3_POPULATION_ONEWIRE_IDENTITY);
    qdev_prop_set_uint8(DEVICE(pci), "timekeeper-kind",
                       SGI_IOC3_TIMEKEEPER_DS1685);
    if (serial_hd(0)) {
        qdev_prop_set_chr(DEVICE(pci), "chardev-a", serial_hd(0));
    }
    if (serial_hd(1)) {
        qdev_prop_set_chr(DEVICE(pci), "chardev-b", serial_hd(1));
    }
    pci_realize_and_unref(pci, bus, &error_fatal);
    /* The fitted SuperIO line is Bridge input 4; Ethernet uses input 2. */
    qdev_connect_gpio_out(DEVICE(pci), 0,
                          qdev_get_gpio_in(DEVICE(bridge), 4));

    pci = pci_new(PCI_DEVFN(3, 0), TYPE_SGI_RAD1_OCTANE);
    object_property_add_child(OBJECT(machine), "rad", OBJECT(pci));
    pci_realize_and_unref(pci, bus, &error_fatal);
}

static void octane_init_common(MachineState *machine, bool octane2)
{
    OctaneMachineState *s = (OctaneMachineState *)machine;
    DeviceState *heart = qdev_new(TYPE_SGI_HEART);
    DeviceState *xbow = qdev_new(TYPE_SGI_XBOW);
    DeviceState *bridge = qdev_new(TYPE_SGI_BRIDGE);
    SGIXbowState *crossbow = SGI_XBOW(xbow);
    MemoryRegion *system = get_system_memory();
    MemoryRegion *physical = g_new0(MemoryRegion, 1);
    MemoryRegion *attr0 = g_new0(MemoryRegion, 1);
    MemoryRegion *firmware = g_new0(MemoryRegion, 1);
    MemoryRegion *alternate_flash = g_new0(MemoryRegion, 1);
    MemoryRegion *proxy = g_new0(MemoryRegion, 1);
    MemoryRegion *proxy_route = g_new0(MemoryRegion, 1);
    MemoryRegion *short_view = g_new0(MemoryRegion, 1);
    MemoryRegion *heart_local = g_new0(MemoryRegion, 1);
    MemoryRegion *long_view = g_new0(MemoryRegion, 1);
    MIPSCPU *cpu;
    Clock *cpuclk;
    g_autofree char *path = NULL;
    int64_t size;
    int loaded;
    uint8_t *flash_image = g_malloc(OCTANE_FLASH_SIZE);

    s->octane2 = octane2;
    if (machine->ram_size != (octane2 ? 256 : 128) * MiB ||
        machine->smp.cpus != 1) {
        error_setg(&error_fatal, "%s supports one CPU and %u MiB of RAM",
                   octane2 ? "octane2" : "octane", octane2 ? 256 : 128);
    }
    if (!octane_valid_banks(s, octane2) ||
        s->bank_size[0] + s->bank_size[1] + s->bank_size[2] +
        s->bank_size[3] != machine->ram_size) {
        error_setg(&error_fatal, "%s unsupported HEART bank population",
                   octane2 ? "octane2" : "octane");
    }
    if (octane2 && strcmp(machine->cpu_type,
                          MIPS_CPU_TYPE_NAME("R12000"))) {
        error_setg(&error_fatal, "octane2 requires an R12000 CPU");
    }
    if (!machine->firmware) {
        error_setg(&error_fatal, "octane requires -bios with a raw image "
                                 "of at most 2 MiB");
    }
    path = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
    size = path ? get_image_size(path, NULL) : -1;
    if (size < 0) {
        error_setg(&error_fatal, "octane cannot read firmware image '%s'",
                   machine->firmware);
    }
    if (size > OCTANE_FLASH_SIZE) {
        error_setg(&error_fatal, "octane firmware image '%s' exceeds 2 MiB",
                   machine->firmware);
    }

    memset(flash_image, 0xff, OCTANE_FLASH_SIZE);
    loaded = load_image_size(path, flash_image, OCTANE_FLASH_SIZE);
    if (loaded != size) {
        error_setg(&error_fatal, "octane cannot load firmware image '%s'",
                   machine->firmware);
    }
    /* Bridge dispatches into this chip while its own I/O guard is active. */
    memory_region_init_io(firmware, OBJECT(machine), &octane_flash_ops,
                          flash_image, "octane.flash", OCTANE_FLASH_SIZE);
    memory_region_init_io(alternate_flash, OBJECT(machine),
                          &octane_blank_flash_ops, NULL,
                          "octane.alternate-flash", OCTANE_FLASH_SIZE);

    cpuclk = clock_new(OBJECT(machine), "cpu-refclk");
    /* The fitted Octane2 R12000 is reported at 360 MHz. */
    clock_set_hz(cpuclk, octane2 ? 360000000 : 100000000);
    if (octane2) {
        DeviceState *dev = qdev_new(machine->cpu_type);

        qdev_connect_clock_in(dev, "clk-in", cpuclk);
        object_property_set_bool(OBJECT(dev), "big-endian", true,
                                 &error_fatal);
        /* The required R12000 fields come from the caller's -cpu profile. */
        qdev_realize(dev, NULL, &error_fatal);
        cpu = MIPS_CPU(dev);
    } else {
        cpu = mips_cpu_create_with_clock(machine->cpu_type, cpuclk,
                                         TARGET_BIG_ENDIAN);
    }
    cpu_mips_irq_init_cpu(cpu);
    cpu_mips_clock_init(cpu);

    memory_region_init(physical, OBJECT(machine), "octane.physical",
                       OCTANE_PHYSICAL_SPAN);
    memory_region_add_subregion(system, 0, physical);
    memory_region_init_ram(&s->low_ram, NULL, "octane.low-ram",
                           OCTANE_LOW_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(physical, 0, &s->low_ram);
    memory_region_init_alias(attr0, OBJECT(machine), "octane.attr0",
                             physical, 0, OCTANE_PHYSICAL_SPAN);
    memory_region_add_subregion(system, OCTANE_ATTR0_BASE, attr0);

    object_property_add_child(OBJECT(machine), "heart", OBJECT(heart));
    for (unsigned bank = 0; bank < HEART_BANKS; bank++) {
        g_autofree char *name = g_strdup_printf("bank%u-size", bank);

        qdev_prop_set_uint64(heart, name, s->bank_size[bank]);
    }
    qdev_prop_set_uint8(heart, "widget-revision", octane2 ? 6 : 4);
    object_property_set_link(OBJECT(heart), "cpu-memory", OBJECT(physical),
                             &error_fatal);
    sysbus_realize(SYS_BUS_DEVICE(heart), &error_fatal);
    memory_region_init_io(&s->high_guard, OBJECT(machine),
                          &octane_high_guard_ops, s, "octane.high-guard",
                          OCTANE_HIGH_GUARD_SIZE);
    /* Override every bank view while leaving the rest of SDRAM mapped. */
    memory_region_add_subregion_overlap(physical, OCTANE_HIGH_GUARD_BASE,
                                        &s->high_guard, HEART_BANKS + 1);
    memory_region_add_subregion(
        physical, OCTANE_HEART_REGS,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(heart), 0));
    /* HEART levels 0, 1 and 2 reach CPU IP2, IP3 and IP4. */
    qdev_connect_gpio_out_named(heart, "processor", 0, cpu->env.irq[2]);
    qdev_connect_gpio_out_named(heart, "processor", 1, cpu->env.irq[3]);
    qdev_connect_gpio_out_named(heart, "processor", 2, cpu->env.irq[4]);

    memory_region_init(proxy, OBJECT(machine), "octane.bridge-fabric",
                       SGI_XBOW_WIDGETS * SGI_XBOW_PACKET_SIZE);
    object_property_add_child(OBJECT(machine), "bridge", OBJECT(bridge));
    qdev_prop_set_bit(bridge, "no-board-identity", true);
    object_property_set_link(OBJECT(bridge), "fabric", OBJECT(proxy),
                             &error_fatal);
    qdev_prop_set_uint32(bridge, "fabric-widget-shift", 48);
    object_property_set_link(OBJECT(bridge), "flash", OBJECT(firmware),
                             &error_fatal);
    object_property_set_link(OBJECT(bridge), "flash-select1",
                             OBJECT(alternate_flash), &error_fatal);
    sysbus_realize(SYS_BUS_DEVICE(bridge), &error_fatal);
    if (!sgi_bridge_install_flash_read_view(SGI_BRIDGE(bridge), flash_image,
                                            OCTANE_FLASH_SIZE, &error_fatal)) {
        return;
    }

    object_property_add_child(OBJECT(machine), "crossbow", OBJECT(xbow));
    qdev_prop_set_uint64(xbow, "span", OCTANE_SHORT_SPAN);
    qdev_prop_set_uint16(xbow, "links-present", (1 << 8) | (1 << 15));
    qdev_prop_set_uint16(xbow, "startup-links-alive", (1 << 8) | (1 << 15));
    if (!sgi_xbow_attach_endpoint(crossbow, 8,
            sgi_xtalk_port_cpu_face(OBJECT(heart), 0),
            sgi_xtalk_port_packet_face(OBJECT(heart), 0), &error_fatal) ||
        !sgi_xbow_attach_endpoint(crossbow, 15,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0),
            sgi_bridge_packet_face(SGI_BRIDGE(bridge)), &error_fatal)) {
        return;
    }
    sysbus_realize(SYS_BUS_DEVICE(xbow), &error_fatal);

    memory_region_init_alias(proxy_route, OBJECT(machine),
                             "octane.bridge-fabric-route",
                             sgi_xbow_packet_ingress(crossbow, 15), 0,
                             SGI_XBOW_WIDGETS * SGI_XBOW_PACKET_SIZE);
    memory_region_add_subregion(proxy, 0, proxy_route);
    memory_region_init_alias(short_view, OBJECT(machine), "octane.short",
                             sgi_xbow_ingress(crossbow, 8), 0,
                             SGI_XBOW_WIDGETS * OCTANE_SHORT_SPAN);
    memory_region_add_subregion(physical, OCTANE_SHORT_BASE, short_view);
    /* The CPU addresses local HEART without a Crossbow loopback. */
    memory_region_init_alias(heart_local, OBJECT(machine),
                             "octane.heart-local",
                             sgi_xtalk_port_cpu_face(OBJECT(heart), 0), 0,
                             0x1000);
    memory_region_add_subregion_overlap(physical, OCTANE_HEART_WIDGET,
                                        heart_local, 1);
    memory_region_init_alias(long_view, OBJECT(machine), "octane.bridge-long",
                             sgi_xbow_packet_ingress(crossbow, 8),
                             (15ULL << 48) | (1ULL << 47),
                             SGI_BRIDGE_WIDGET_SIZE);
    memory_region_add_subregion(physical, OCTANE_LONG_BRIDGE, long_view);

    octane_fit_pci(machine, SGI_BRIDGE(bridge));
}

static void octane_init(MachineState *machine)
{
    octane_init_common(machine, false);
}

static void octane2_init(MachineState *machine)
{
    octane_init_common(machine, true);
}

static void octane_instance_init_common(Object *obj, uint64_t default_bank)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;

    s->bank_size[0] = default_bank;
    for (unsigned bank = 0; bank < HEART_BANKS; bank++) {
        g_autofree char *name = g_strdup_printf("bank%u-size", bank);

        object_property_add_uint64_ptr(obj, name, &s->bank_size[bank],
                                       OBJ_PROP_FLAG_READWRITE);
        object_property_set_description(obj, name,
                                        "Capacity of the selected HEART bank in bytes");
    }
}

static void octane_instance_init(Object *obj)
{
    octane_instance_init_common(obj, 128 * MiB);
}

static void octane2_instance_init(Object *obj)
{
    octane_instance_init_common(obj, 256 * MiB);
}

static void octane_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "SGI Octane (IP30), graphics-free functional profile";
    mc->init = octane_init;
    mc->reset = octane_reset;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("R10000");
    mc->default_cpus = 1;
    mc->max_cpus = 1;
    mc->default_ram_size = 128 * MiB;
    mc->no_floppy = true;
    mc->no_cdrom = true;
    mc->no_parallel = true;
}

static void octane2_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    octane_class_init(oc, data);
    mc->desc = "SGI Octane2 (IP30), graphics-free functional profile";
    mc->init = octane2_init;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("R12000");
    mc->default_ram_size = 256 * MiB;
}

static const TypeInfo octane2_machine = {
    .name = MACHINE_TYPE_NAME("octane2"),
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(OctaneMachineState),
    .instance_init = octane2_instance_init,
    .class_init = octane2_class_init,
};

static const TypeInfo octane_machine = {
    .name = MACHINE_TYPE_NAME("octane"),
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(OctaneMachineState),
    .instance_init = octane_instance_init,
    .class_init = octane_class_init,
};

static void octane_register_types(void)
{
    type_register_static(&octane_machine);
    type_register_static(&octane2_machine);
}

type_init(octane_register_types)
