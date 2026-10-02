/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * Octane (IP30) functional profile with optional textureless SI or VPro.
 * Fitted populations come from the checked IP30 catalogue. Device models
 * retain their documented functional approximations; catalogue data does
 * not establish additional physical behavior.
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
#include "hw/core/irq.h"
#include "hw/audio/sgi-rad1-audio.h"
#include "hw/mips/mips.h"
#include "hw/mips/sgi-xio-port.h"
#include "hw/misc/sgi-board-record.h"
#include "hw/misc/sgi-heart.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/misc/sgi-mgras.h"
#include "hw/misc/sgi-odyssey.h"
#include "hw/misc/sgi-xbow.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/pci/pci.h"
#include "hw/scsi/qlogic-isp.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "system/block-backend.h"
#include "target/mips/cpu.h"
#include "hw/mips/sgi/sn-catalogue.h"

#define OCTANE_SHORT_BASE 0x10000000ULL
#define OCTANE_SHORT_SPAN (16 * MiB)
#define OCTANE_GRAPHICS_WIDGET 11
#define OCTANE_LONG_GRAPHICS (OCTANE_GRAPHICS_WIDGET * (1ULL << 36))
#define OCTANE_LONG_SPAN (1ULL << 36)
#define OCTANE_LONG_BRIDGE (15ULL << 36)
#define OCTANE_FLASH_SIZE (2 * MiB)
#define OCTANE_HEART_REGS 0x0ff00000ULL
#define OCTANE_HEART_WIDGET 0x18000000ULL
#define OCTANE_BANK_UNIT (32 * MiB)
#define OCTANE_PHYSICAL_SPAN (1ULL << 40)
#define OCTANE_ATTR0_BASE (1ULL << 40)
#define OCTANE_LOW_RAM_SIZE 0x4000
#define OCTANE_HIGH_GUARD_BASE 0x20000000ULL
#define OCTANE_HIGH_GUARD_SIZE 0x4000

typedef struct OctaneMachineState {
    MachineState parent;
    MemoryRegion low_ram;
    MemoryRegion high_guard;
    uint64_t bank_size[HEART_BANKS];
    const SGIModelTopology *model;
    const SGIModelNode *node;
    char *topology_name;
    char *graphics_board;
    MemoryRegion graphics_fabric;
    MemoryRegion graphics_route;
    MemoryRegion graphics_long;
    char *flash_profile;
    char *flash_drive;
    uint64_t flash_select;
    uint64_t flash_strap;
} OctaneMachineState;

static char *octane_get_graphics_board(Object *obj, Error **errp)
{
    return g_strdup(((OctaneMachineState *)obj)->graphics_board);
}

static void octane_set_graphics_board(Object *obj, const char *value,
                                      Error **errp)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;

    if (strcmp(value, "none") && strcmp(value, "si") &&
        strcmp(value, "vpro")) {
        error_setg(errp, "octane graphics must be none, si or vpro");
        return;
    }
    g_free(s->graphics_board);
    s->graphics_board = g_strdup(value);
}

static char *octane_get_flash_profile(Object *obj, Error **errp)
{
    return g_strdup(((OctaneMachineState *)obj)->flash_profile);
}

static void octane_set_flash_profile(Object *obj, const char *value,
                                     Error **errp)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;

    g_free(s->flash_profile);
    s->flash_profile = g_strdup(value);
}

static char *octane_get_flash_drive(Object *obj, Error **errp)
{
    return g_strdup(((OctaneMachineState *)obj)->flash_drive);
}

static void octane_set_flash_drive(Object *obj, const char *value,
                                   Error **errp)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;

    g_free(s->flash_drive);
    s->flash_drive = g_strdup(value);
}

static const SGIModelTopology *octane_model(const char *product,
                                           const char *name)
{
    const char *selected = name ? name : sgi_model_default_topology(product);
    const SGIModelTopology *model = selected ?
        sgi_model_find_topology(selected) : NULL;

    if (!model || strcmp(model->product, product) ||
        model->generation != SGI_MODEL_GENERATION_IP30 || !model->nodes ||
        model->nodes->next || !model->nodes->value->heart) {
        error_setg(&error_fatal, "%s requires its checked IP30 topology",
                   product);
    }
    return model;
}

static uint64_t octane_model_ram(const SGIModelNode *node)
{
    uint64_t total = 0;
    uint64List *bank;

    for (bank = node->bank_mib; bank; bank = bank->next) {
        total += bank->value * MiB;
    }
    return total;
}

static const SGIModelBoardRecord *octane_board_record(OctaneMachineState *s,
                                                     const char *role)
{
    for (SGIModelBoardRecordList *item = s->node->heart->board_records;
         item; item = item->next) {
        if (!strcmp(item->value->role, role)) {
            return item->value;
        }
    }
    error_setg(&error_fatal, "IP30 catalogue has no %s board identity", role);
    return NULL;
}

static bool octane_attach_record(OctaneMachineState *s, Object *owner,
                                  const char *child, OneWireBus *bus,
                                  const char *role)
{
    const SGIModelBoardRecord *data = octane_board_record(s, role);
    SGIBoardRecord record = {
        data->barcode, data->part, data->revision, data->name,
    };

    return sgi_board_record_attach(owner, child, bus, &record, data->serial,
                                   role, &error_fatal);
}

static bool octane_valid_banks(OctaneMachineState *s)
{
    for (SGIModelBankLayoutList *item = s->node->accepted_bank_layouts;
         item; item = item->next) {
        uint64List *bank = item->value->bank_mib;
        bool match = true;

        for (unsigned i = 0; i < HEART_BANKS; i++) {
            if (!bank || s->bank_size[i] != bank->value * MiB) {
                match = false;
                break;
            }
            bank = bank->next;
        }
        if (match && !bank) {
            return true;
        }
    }
    return false;
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
    OctaneMachineState *s = (OctaneMachineState *)machine;
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
                         SGI_IOC3_POPULATION_KEYBOARD_MOUSE |
                         SGI_IOC3_POPULATION_PARALLEL |
                         SGI_IOC3_POPULATION_ONEWIRE_IDENTITY);
    {
        DeviceState *parallel = DEVICE(object_resolve_path_component(
            OBJECT(pci), "parallel"));

        /* Rev F is photographed; Octane2 Rev B is inferred from Rev D. */
        qdev_prop_set_uint16(parallel, "parallel-part",
                             SGI_IOC3_PAR_PART_TL16PIR552);
        qdev_prop_set_uint16(parallel, "fifo-depth", 16);
    }
    /* Public IP30 wiring assigns GPIO2 as the read-only product input. */
    qdev_prop_set_uint32(DEVICE(pci), "gpio-input-mask", 1u << 2);
    qdev_prop_set_uint32(DEVICE(pci), "gpio-input-values",
                         s->node->heart->gpio_classic ? 1u << 2 : 0);
    qdev_prop_set_uint8(DEVICE(pci), "timekeeper-kind",
                       SGI_IOC3_TIMEKEEPER_DS1685);
    if (blk_by_name("sgi-nvram0")) {
        qdev_prop_set_drive_err(DEVICE(pci), "ds1685-drive",
                               blk_by_name("sgi-nvram0"), &error_fatal);
    }
    qdev_prop_set_bit(DEVICE(pci), "phy-ics1890", true);
    qdev_prop_set_uint8(DEVICE(pci), "phy-addr", 1);
    if (serial_hd(0)) {
        qdev_prop_set_chr(DEVICE(pci), "chardev-a", serial_hd(0));
    }
    if (serial_hd(1)) {
        qdev_prop_set_chr(DEVICE(pci), "chardev-b", serial_hd(1));
    }
    pci_realize_and_unref(pci, bus, &error_fatal);
    if (!octane_attach_record(s, OBJECT(pci), "frontplane-record",
                               SGI_IOC3(pci)->onewire_bus, "frontplane") ||
        !octane_attach_record(s, OBJECT(pci), "power-supply-record",
                               SGI_IOC3(pci)->onewire_bus, "power-supply")) {
        return;
    }
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
    MemoryRegion *firmware = NULL;
    MemoryRegion *alternate_flash = NULL;
    DeviceState *flash_chip = NULL;
    MemoryRegion *proxy = g_new0(MemoryRegion, 1);
    MemoryRegion *proxy_route = g_new0(MemoryRegion, 1);
    MemoryRegion *short_view = g_new0(MemoryRegion, 1);
    MemoryRegion *heart_local = g_new0(MemoryRegion, 1);
    MemoryRegion *long_view = g_new0(MemoryRegion, 1);
    MIPSCPU *cpu;
    Clock *cpuclk;
    const SGIModelProcessor *processor;
    g_autofree char *path = NULL;
    int64_t size;
    int loaded;
    uint8_t *flash_image = NULL;
    bool sharp_flash = !strcmp(s->flash_profile, "sharp-lh28f800su");
    bool si = !strcmp(s->graphics_board, "si");
    bool vpro = !strcmp(s->graphics_board, "vpro");
    bool graphics = si || vpro;
    uint16_t links = (1 << 8) | (1 << 15);

    s->model = octane_model(octane2 ? "octane2" : "octane",
                             s->topology_name);
    s->node = s->model->nodes->value;
    processor = s->node->cpu_slots->value->assembly;

    if (machine->ram_size != octane_model_ram(s->node) ||
        machine->smp.cpus != 1) {
        error_setg(&error_fatal, "%s supports one CPU and %u MiB of RAM",
                   octane2 ? "octane2" : "octane",
                   (unsigned)(octane_model_ram(s->node) / MiB));
    }
    if (!octane_valid_banks(s) ||
        s->bank_size[0] + s->bank_size[1] + s->bank_size[2] +
        s->bank_size[3] != machine->ram_size) {
        error_setg(&error_fatal, "%s unsupported HEART bank population",
                   octane2 ? "octane2" : "octane");
    }
    if (octane2 && strcmp(machine->cpu_type,
                          MIPS_CPU_TYPE_NAME("R12000"))) {
        error_setg(&error_fatal, "octane2 requires an R12000 CPU");
    }
    if (sharp_flash) {
        DeviceState *chip;
        BlockBackend *blk;

        if (machine->firmware || !s->flash_drive ||
            s->flash_select > 1 || s->flash_strap > 1) {
            error_setg(&error_fatal, "sharp flash requires flash-drive, "
                       "flash-select=0|1 and flash-strap=0|1; "
                       "do not combine it with -bios");
        }
        blk = blk_by_name(s->flash_drive);
        if (!blk) {
            error_setg(&error_fatal, "no flash drive named '%s'",
                       s->flash_drive);
        }
        chip = qdev_new("lh28f800su");
        object_property_add_child(OBJECT(machine), "sharp-flash", OBJECT(chip));
        qdev_prop_set_drive_err(chip, "drive", blk, &error_fatal);
        /* Flash is already out of reset when the CPU starts. */
        qdev_prop_set_bit(chip, "initial-rp-high", true);
        sysbus_realize(SYS_BUS_DEVICE(chip), &error_fatal);
        firmware = sysbus_mmio_get_region(SYS_BUS_DEVICE(chip), 0);
        qemu_set_irq(qdev_get_gpio_in_named(chip, "pins", 0), 1);
        qemu_set_irq(qdev_get_gpio_in_named(chip, "pins", 1), 1);
        qemu_set_irq(qdev_get_gpio_in_named(chip, "pins", 2), 1);
        flash_chip = chip;
    } else {
        if (strcmp(s->flash_profile, "raw") || s->flash_drive ||
            s->flash_select != UINT64_MAX ||
            s->flash_strap != UINT64_MAX) {
            error_setg(&error_fatal, "raw flash cannot take Sharp flash "
                       "properties");
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
        flash_image = g_malloc(OCTANE_FLASH_SIZE);
        memset(flash_image, 0xff, OCTANE_FLASH_SIZE);
        loaded = load_image_size(path, flash_image, OCTANE_FLASH_SIZE);
        if (loaded != size) {
            error_setg(&error_fatal, "octane cannot load firmware image '%s'",
                       machine->firmware);
        }
        firmware = g_new0(MemoryRegion, 1);
        memory_region_init_io(firmware, OBJECT(machine), &octane_flash_ops,
                              flash_image, "octane.flash", OCTANE_FLASH_SIZE);
        alternate_flash = g_new0(MemoryRegion, 1);
        memory_region_init_io(alternate_flash, OBJECT(machine),
                              &octane_blank_flash_ops, NULL,
                              "octane.alternate-flash", OCTANE_FLASH_SIZE);
    }

    cpuclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(cpuclk, processor->clock_hz);
    if (octane2) {
        DeviceState *dev = qdev_new(machine->cpu_type);

        qdev_connect_clock_in(dev, "clk-in", cpuclk);
        object_property_set_bool(OBJECT(dev), "big-endian", true,
                                 &error_fatal);
        /* The required R12000 fields come from the caller's -cpu profile. */
        qdev_realize(dev, NULL, &error_fatal);
        cpu = MIPS_CPU(dev);
    } else {
        DeviceState *dev = qdev_new(machine->cpu_type);

        qdev_connect_clock_in(dev, "clk-in", cpuclk);
        object_property_set_bool(OBJECT(dev), "big-endian", TARGET_BIG_ENDIAN,
                                 &error_fatal);
        object_property_set_uint(OBJECT(dev), "r10000-prid", processor->prid,
                                 &error_fatal);
        qdev_realize(dev, NULL, &error_fatal);
        cpu = MIPS_CPU(dev);
    }
    cpu_mips_irq_init_cpu(cpu);
    cpu_mips_clock_init(cpu);

    memory_region_init(physical, OBJECT(machine), "octane.physical",
                       OCTANE_PHYSICAL_SPAN);
    memory_region_add_subregion(system, 0, physical);
    /* Functional storage approximation for the full 16 KiB low window. */
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
    qdev_prop_set_uint8(heart, "widget-revision",
                         s->node->heart->widget_revision);
    qdev_prop_set_uint64(heart, "widget-id-64",
                        s->node->heart->widget_id_64);
    if (s->node->heart->widget_status_f_measured) {
        qdev_prop_set_bit(heart, "widget-status-f-measured", true);
    }
    object_property_set_link(OBJECT(heart), "cpu-memory", OBJECT(physical),
                             &error_fatal);
    sysbus_realize(SYS_BUS_DEVICE(heart), &error_fatal);
    if (!octane_attach_record(s, OBJECT(heart), "processor-record",
                               SGI_HEART(heart)->onewire_bus, "processor")) {
        return;
    }
    memory_region_init_io(&s->high_guard, OBJECT(machine),
                          &octane_high_guard_ops, s, "octane.high-guard",
                          OCTANE_HIGH_GUARD_SIZE);
    /* Override every bank view while leaving the rest of SDRAM mapped. */
    memory_region_add_subregion_overlap(physical, OCTANE_HIGH_GUARD_BASE,
                                        &s->high_guard, HEART_BANKS + 1);
    memory_region_add_subregion(
        physical, OCTANE_HEART_REGS,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(heart), 0));
    /* HEART levels 0 through 4 reach CPU IP2 through IP6. */
    qdev_connect_gpio_out_named(heart, "processor", 0, cpu->env.irq[2]);
    qdev_connect_gpio_out_named(heart, "processor", 1, cpu->env.irq[3]);
    qdev_connect_gpio_out_named(heart, "processor", 2, cpu->env.irq[4]);
    qdev_connect_gpio_out_named(heart, "processor", 3, cpu->env.irq[5]);
    qdev_connect_gpio_out_named(heart, "processor", 4, cpu->env.irq[6]);

    memory_region_init(proxy, OBJECT(machine), "octane.bridge-fabric",
                       SGI_XBOW_WIDGETS * SGI_XBOW_PACKET_SIZE);
    object_property_add_child(OBJECT(machine), "bridge", OBJECT(bridge));
    qdev_prop_set_string(bridge, "board-barcode",
                         octane_board_record(s, "system-board")->barcode);
    qdev_prop_set_string(bridge, "board-part",
                         octane_board_record(s, "system-board")->part);
    qdev_prop_set_string(bridge, "board-revision",
                         octane_board_record(s, "system-board")->revision);
    qdev_prop_set_string(bridge, "board-name",
                         octane_board_record(s, "system-board")->name);
    qdev_prop_set_uint64(bridge, "onewire-serial",
                         octane_board_record(s, "system-board")->serial);
    object_property_set_link(OBJECT(bridge), "fabric", OBJECT(proxy),
                             &error_fatal);
    if (!sharp_flash || s->flash_select == 0) {
        object_property_set_link(OBJECT(bridge), "flash", OBJECT(firmware),
                                 &error_fatal);
    }
    if (sharp_flash && s->flash_select == 1) {
        object_property_set_link(OBJECT(bridge), "flash-select1",
                                 OBJECT(firmware), &error_fatal);
    } else if (!sharp_flash) {
        object_property_set_link(OBJECT(bridge), "flash-select1",
                                 OBJECT(alternate_flash), &error_fatal);
    }
    if (sharp_flash) {
        qdev_prop_set_bit(bridge, "flash-select", s->flash_strap);
        qdev_prop_set_bit(bridge, "flash-reject-invalid-writes", true);
    }
    sysbus_realize(SYS_BUS_DEVICE(bridge), &error_fatal);
    if (sharp_flash) {
        if (!sgi_bridge_install_flash_device_read_view(
                SGI_BRIDGE(bridge), firmware, &error_fatal)) {
            return;
        }
        qdev_connect_gpio_out_named(flash_chip, "array-read", 0,
            qdev_get_gpio_in_named(bridge, "flash-array-read", 0));
        /* Re-drive the established level after wiring the derived read view. */
        qemu_set_irq(qdev_get_gpio_in_named(flash_chip, "pins", 2), 1);
        object_unref(OBJECT(flash_chip));
    } else if (!sgi_bridge_install_flash_read_view(SGI_BRIDGE(bridge),
                                                  flash_image,
                                                  OCTANE_FLASH_SIZE,
                                                  &error_fatal)) {
        return;
    }

    object_property_add_child(OBJECT(machine), "crossbow", OBJECT(xbow));
    qdev_prop_set_uint32(xbow, "revision",
                          s->model->crossbars->value->widget_revision);
    qdev_prop_set_uint64(xbow, "span", OCTANE_SHORT_SPAN);
    if (graphics) {
        /*
         * Both boards take the graphics slot's widget 11: an Octane IRIX
         * inventory lists its VPro (ODY32) under xtalk/11, where the SI
         * board also sits.
         */
        const char *name = si ? "mgras" : "odyssey";
        DeviceState *board = qdev_new(si ? TYPE_SGI_MGRAS : TYPE_SGI_ODYSSEY);
        MemoryRegion *face;

        memory_region_init(&s->graphics_fabric, OBJECT(machine),
                           "octane.graphics-fabric",
                           SGI_XBOW_WIDGETS * SGI_XBOW_PACKET_SIZE);
        object_property_add_child(OBJECT(machine), name, OBJECT(board));
        board->id = g_strdup(name);
        qdev_prop_set_uint32(board, "module", 0);
        qdev_prop_set_string(board, "slot",
                             G_STRINGIFY(OCTANE_GRAPHICS_WIDGET));
        if (si) {
            qdev_prop_set_string(board, "board", "si");
        }
        object_property_set_link(OBJECT(board), "fabric",
                                 OBJECT(&s->graphics_fabric), &error_fatal);
        sysbus_realize(SYS_BUS_DEVICE(board), &error_fatal);
        face = sysbus_mmio_get_region(SYS_BUS_DEVICE(board), 0);
        if (!sgi_xbow_attach_endpoint(crossbow, OCTANE_GRAPHICS_WIDGET,
                                      face, face, &error_fatal)) {
            return;
        }
        object_unref(OBJECT(board));
        links |= 1 << OCTANE_GRAPHICS_WIDGET;
    }
    qdev_prop_set_uint16(xbow, "links-present", links);
    qdev_prop_set_uint16(xbow, "startup-links-alive", links);
    qdev_prop_set_uint16(xbow, "link-reset-restore-alive", links);
    if (!sgi_xbow_attach_endpoint(crossbow, 8,
            sgi_xio_port_cpu_face(OBJECT(heart), 0),
            sgi_xio_port_packet_face(OBJECT(heart), 0), &error_fatal) ||
        !sgi_xbow_attach_endpoint(crossbow, 15,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0),
            sgi_bridge_packet_face(SGI_BRIDGE(bridge)), &error_fatal)) {
        return;
    }
    sysbus_realize(SYS_BUS_DEVICE(xbow), &error_fatal);

    if (graphics) {
        /* Packet ingress exists only after Crossbow realization. */
        memory_region_init_alias(&s->graphics_route, OBJECT(machine),
                                 "octane.graphics-fabric-route",
                                 sgi_xbow_packet_ingress(crossbow,
                                                        OCTANE_GRAPHICS_WIDGET),
                                 0, SGI_XBOW_WIDGETS * SGI_XBOW_PACKET_SIZE);
        memory_region_add_subregion(&s->graphics_fabric, 0, &s->graphics_route);
        memory_region_init_alias(&s->graphics_long, OBJECT(machine),
                                 "octane.graphics-long",
                                 sgi_xbow_packet_ingress(crossbow, 8),
                                 (uint64_t)OCTANE_GRAPHICS_WIDGET << 48,
                                 OCTANE_LONG_SPAN);
        memory_region_add_subregion(physical, OCTANE_LONG_GRAPHICS,
                                    &s->graphics_long);
    }

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
                             sgi_xio_port_cpu_face(OBJECT(heart), 0), 0,
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

static char *octane_get_topology(Object *obj, Error **errp)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;

    return g_strdup(s->topology_name ? s->topology_name : s->model->name);
}

static void octane_set_topology(Object *obj, const char *name, Error **errp)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;

    g_free(s->topology_name);
    s->topology_name = g_strdup(name);
}

static void octane_instance_init_common(Object *obj, const char *product)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;
    uint64List *default_bank;

    s->model = octane_model(product, NULL);
    s->node = s->model->nodes->value;
    object_property_add_str(obj, "topology", octane_get_topology,
                            octane_set_topology);
    s->graphics_board = g_strdup("none");
    object_property_add_str(obj, "graphics-board", octane_get_graphics_board,
                            octane_set_graphics_board);
    object_property_set_description(obj, "graphics-board",
                                    "Optional graphics board: none, si "
                                    "(textureless SI) or vpro");
    s->flash_profile = g_strdup("raw");
    s->flash_select = UINT64_MAX;
    s->flash_strap = UINT64_MAX;
    object_property_add_str(obj, "flash-profile", octane_get_flash_profile,
                            octane_set_flash_profile);
    object_property_add_str(obj, "flash-drive", octane_get_flash_drive,
                            octane_set_flash_drive);
    object_property_add_uint64_ptr(obj, "flash-select", &s->flash_select,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "flash-strap", &s->flash_strap,
                                   OBJ_PROP_FLAG_READWRITE);
    default_bank = s->node->bank_mib;

    for (unsigned bank = 0; bank < HEART_BANKS; bank++) {
        g_autofree char *name = g_strdup_printf("bank%u-size", bank);

        g_assert(default_bank);
        s->bank_size[bank] = default_bank->value * MiB;
        default_bank = default_bank->next;
        object_property_add_uint64_ptr(obj, name, &s->bank_size[bank],
                                       OBJ_PROP_FLAG_READWRITE);
        object_property_set_description(obj, name,
                                        "Capacity of the selected HEART bank in bytes");
    }
}

static void octane_instance_init(Object *obj)
{
    octane_instance_init_common(obj, "octane");
}

static void octane2_instance_init(Object *obj)
{
    octane_instance_init_common(obj, "octane2");
}

static void octane_instance_finalize(Object *obj)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;

    g_free(s->topology_name);
    g_free(s->graphics_board);
    g_free(s->flash_profile);
    g_free(s->flash_drive);
}

static void octane_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "SGI Octane (IP30), functional profile";
    mc->init = octane_init;
    mc->reset = octane_reset;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("R10000");
    mc->default_cpus = 1;
    mc->max_cpus = 1;
    mc->default_ram_size = octane_model_ram(
        octane_model("octane", NULL)->nodes->value);
    mc->no_floppy = true;
    mc->no_cdrom = true;
    mc->no_parallel = true;
}

static void octane2_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    octane_class_init(oc, data);
    mc->desc = "SGI Octane2 (IP30), functional profile";
    mc->init = octane2_init;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("R12000");
    mc->default_ram_size = octane_model_ram(
        octane_model("octane2", NULL)->nodes->value);
}

static const TypeInfo octane2_machine = {
    .name = MACHINE_TYPE_NAME("octane2"),
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(OctaneMachineState),
    .instance_init = octane2_instance_init,
    .instance_finalize = octane_instance_finalize,
    .class_init = octane2_class_init,
};

static const TypeInfo octane_machine = {
    .name = MACHINE_TYPE_NAME("octane"),
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(OctaneMachineState),
    .instance_init = octane_instance_init,
    .instance_finalize = octane_instance_finalize,
    .class_init = octane_class_init,
};

static void octane_register_types(void)
{
    type_register_static(&octane_machine);
    type_register_static(&octane2_machine);
}

type_init(octane_register_types)
