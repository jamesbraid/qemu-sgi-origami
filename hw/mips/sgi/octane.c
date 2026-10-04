/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * Octane (IP30) functional profile with optional IMPACT or VPro graphics.
 * Fitted populations come from the checked IP30 catalogue. Device models
 * retain their documented functional approximations; catalogue data does
 * not establish additional physical behavior.
 */
#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
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
#include "hw/block/flash.h"
#include "migration/blocker.h"
#include "migration/register.h"
#include "system/runstate.h"
#include "target/mips/cpu.h"
#include "hw/mips/sgi/sgi-machines.h"

#define OCTANE_SHORT_BASE 0x10000000ULL
#define OCTANE_SHORT_SPAN (16 * MiB)
/*
 * The four Crossbow ports the Octane gives graphics and expansion boards.
 * Published inventories show IMPACT boards at 10, 11 and 12, and the
 * stock IDE probes 9 to 12. Widget 11 stays the default.
 */
#define OCTANE_GRAPHICS_WIDGET_FIRST 9
#define OCTANE_GRAPHICS_WIDGET_LAST 12
#define OCTANE_GRAPHICS_WIDGET 11
#define OCTANE_LONG_SPAN (1ULL << 36)
#define OCTANE_LONG_BRIDGE (15ULL << 36)
#define OCTANE_FLASH_SIZE MiB
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
    uint8_t graphics_widget;
    bool graphics_widget_set;
    MemoryRegion graphics_fabric;
    MemoryRegion graphics_route;
    MemoryRegion graphics_long;
    Error *flash_migration_blocker;
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

    if (strcmp(value, "none") && strcmp(value, "vpro") &&
        !sgi_mgras_board_known(value)) {
        error_setg(errp, "octane graphics must be none, vpro or an MGRAS board: "
                   "si, si-t, ssi, mxi, esi, esi-t, essi or emxi");
        return;
    }
    g_free(s->graphics_board);
    s->graphics_board = g_strdup(value);
}

static void octane_get_graphics_widget(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    uint8_t value = ((OctaneMachineState *)obj)->graphics_widget;

    visit_type_uint8(v, name, &value, errp);
}

static void octane_set_graphics_widget(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;
    uint8_t value;

    if (!visit_type_uint8(v, name, &value, errp)) {
        return;
    }
    if (value < OCTANE_GRAPHICS_WIDGET_FIRST ||
        value > OCTANE_GRAPHICS_WIDGET_LAST) {
        error_setg(errp, "octane graphics widget must be %d to %d",
                   OCTANE_GRAPHICS_WIDGET_FIRST, OCTANE_GRAPHICS_WIDGET_LAST);
        return;
    }
    s->graphics_widget = value;
    s->graphics_widget_set = true;
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

static int octane_flash_load_setup(QEMUFile *f, void *opaque, Error **errp)
{
    error_setg(errp,
               "octane: migration with persistent boot flash is not supported");
    return -EINVAL;
}

/* loadvm does not consult migration blockers, even for existing snapshots. */
static const SaveVMHandlers octane_flash_load_handlers = {
    .load_setup = octane_flash_load_setup,
};

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
    bool mgras = sgi_mgras_board_known(s->graphics_board);
    bool vpro = !strcmp(s->graphics_board, "vpro");
    bool graphics = mgras || vpro;
    unsigned int widget = mgras ? s->graphics_widget : OCTANE_GRAPHICS_WIDGET;
    uint16_t links = (1 << 8) | (1 << 15);

    s->model = octane_model(octane2 ? "octane2" : "octane",
                             s->topology_name);
    s->node = s->model->nodes->value;
    processor = s->node->cpu_slots->value->assembly;

    if (s->graphics_widget_set && !mgras) {
        error_setg(&error_fatal,
                   "graphics-widget requires an MGRAS graphics-board");
    }

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
    if (s->flash_select > 1 || s->flash_strap > 1) {
        error_setg(&error_fatal,
                   "octane flash-select and flash-strap must be 0 or 1");
    }
    if (!!machine->firmware == !!s->flash_drive) {
        error_setg(&error_fatal, "octane requires either -bios or flash-drive");
    }
    flash_chip = qdev_new(TYPE_PFLASH_CFI01);
    object_property_add_child(OBJECT(machine), "boot-flash",
                              OBJECT(flash_chip));
    /* Compatibility geometry and identifiers; the fitted part is unknown. */
    qdev_prop_set_string(flash_chip, "name", "octane.boot-flash");
    qdev_prop_set_uint32(flash_chip, "num-blocks", 16);
    qdev_prop_set_uint64(flash_chip, "sector-length", 64 * KiB);
    qdev_prop_set_uint8(flash_chip, "width", 2);
    qdev_prop_set_uint8(flash_chip, "device-width", 2);
    qdev_prop_set_bit(flash_chip, "big-endian", true);
    qdev_prop_set_uint16(flash_chip, "id0", 0x00b0);
    qdev_prop_set_uint16(flash_chip, "id1", 0x66a8);
    if (s->flash_drive) {
        BlockBackend *blk = blk_by_name(s->flash_drive);

        if (runstate_check(RUN_STATE_INMIGRATE)) {
            error_setg(&error_fatal,
                       "octane: migration with persistent boot flash is not supported");
        }
        if (!blk) {
            error_setg(&error_fatal, "no flash drive named '%s'",
                       s->flash_drive);
        }
        if (blk_getlength(blk) != OCTANE_FLASH_SIZE) {
            error_setg(&error_fatal, "octane boot flash must be exactly 1 MiB");
        }
        qdev_prop_set_drive_err(flash_chip, "drive", blk, &error_fatal);
        error_setg(&s->flash_migration_blocker,
                   "octane: migration with persistent boot flash is not supported");
        migrate_add_blocker(&s->flash_migration_blocker, &error_fatal);
        register_savevm_live("octane-flash-storage",
                             VMSTATE_INSTANCE_ID_ANY, 0,
                             &octane_flash_load_handlers, s);
    }
    sgi_bridge_connect_cfi_flash(SGI_BRIDGE(bridge), PFLASH_CFI01(flash_chip));
    sysbus_realize(SYS_BUS_DEVICE(flash_chip), &error_fatal);
    firmware = sysbus_mmio_get_region(SYS_BUS_DEVICE(flash_chip), 0);
    if (machine->firmware) {
        uint8_t *storage = memory_region_get_ram_ptr(firmware);

        path = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
        size = path ? get_image_size(path, NULL) : -1;
        if (size <= 0 || size > OCTANE_FLASH_SIZE) {
            error_setg(&error_fatal, "octane firmware '%s' must contain "
                       "1 to 1 MiB bytes",
                       machine->firmware);
        }
        memset(storage, 0xff, OCTANE_FLASH_SIZE);
        loaded = load_image_size(path, storage, OCTANE_FLASH_SIZE);
        if (loaded != size) {
            error_setg(&error_fatal, "octane cannot load firmware image '%s'",
                       machine->firmware);
        }
        memory_region_flush_rom_device(firmware, 0, OCTANE_FLASH_SIZE);
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
    object_property_set_link(OBJECT(bridge),
                             s->flash_select ? "flash-select1" : "flash",
                             OBJECT(firmware), &error_fatal);
    qdev_prop_set_bit(bridge, "flash-select", s->flash_strap);
    qdev_prop_set_bit(bridge, "flash-reject-invalid-writes", true);
    sysbus_realize(SYS_BUS_DEVICE(bridge), &error_fatal);
    object_unref(OBJECT(flash_chip));

    object_property_add_child(OBJECT(machine), "crossbow", OBJECT(xbow));
    qdev_prop_set_uint32(xbow, "revision",
                          s->model->crossbars->value->widget_revision);
    qdev_prop_set_uint64(xbow, "span", OCTANE_SHORT_SPAN);
    if (graphics) {
        const char *name = mgras ? "mgras" : "odyssey";
        DeviceState *board = qdev_new(mgras ? TYPE_SGI_MGRAS : TYPE_SGI_ODYSSEY);
        MemoryRegion *face;
        g_autofree char *slot = g_strdup_printf("%u", widget);

        memory_region_init(&s->graphics_fabric, OBJECT(machine),
                           "octane.graphics-fabric",
                           SGI_XBOW_WIDGETS * SGI_XBOW_PACKET_SIZE);
        object_property_add_child(OBJECT(machine), name, OBJECT(board));
        board->id = g_strdup(name);

        qdev_prop_set_uint32(board, "module", 0);
        qdev_prop_set_string(board, "slot", slot);
        if (mgras) {
            qdev_prop_set_string(board, "board", s->graphics_board);
            /*
             * The PROM names the board from its serial-number record. Barcode
             * and ROM serial are fictitious, like the other IP30 identities.
             */
            qdev_prop_set_bit(board, "board-record", true);
            qdev_prop_set_string(board, "board-record-barcode",
                                 octane2 ? "SIM3020005" : "SIM3010005");
            qdev_prop_set_uint64(board, "board-record-serial",
                                 octane2 ? 0x7a3002000005ULL :
                                           0x7a3001000005ULL);
        }
        object_property_set_link(OBJECT(board), "fabric",
                                 OBJECT(&s->graphics_fabric), &error_fatal);
        sysbus_realize(SYS_BUS_DEVICE(board), &error_fatal);
        face = sysbus_mmio_get_region(SYS_BUS_DEVICE(board), 0);
        if (!sgi_xbow_attach_endpoint(crossbow, widget,
                                      face, face, &error_fatal)) {
            return;
        }
        object_unref(OBJECT(board));
        links |= 1 << widget;
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
                                 sgi_xbow_packet_ingress(crossbow, widget),
                                 0, SGI_XBOW_WIDGETS * SGI_XBOW_PACKET_SIZE);
        memory_region_add_subregion(&s->graphics_fabric, 0, &s->graphics_route);
        memory_region_init_alias(&s->graphics_long, OBJECT(machine),
                                 "octane.graphics-long",
                                 sgi_xbow_packet_ingress(crossbow, 8),
                                 (uint64_t)widget << 48,
                                 OCTANE_LONG_SPAN);
        memory_region_add_subregion(physical, widget * OCTANE_LONG_SPAN,
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
                                    "Optional graphics board: none, vpro, si, "
                                    "si-t, ssi, mxi, esi, esi-t, essi or emxi");
    s->graphics_widget = OCTANE_GRAPHICS_WIDGET;
    object_property_add(obj, "graphics-widget", "uint8",
                        octane_get_graphics_widget,
                        octane_set_graphics_widget, NULL, NULL);
    object_property_set_description(obj, "graphics-widget",
                                    "Crossbow port of the graphics board, "
                                    "9 to 12 (default 11)");
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
    unregister_savevm(NULL, "octane-flash-storage", s);
    migrate_del_blocker(&s->flash_migration_blocker);
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
