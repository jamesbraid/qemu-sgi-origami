/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qapi/qapi-visit-common.h"
#include "qapi/visitor.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "chardev/char.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sn1.h"
#include "hw/misc/sgi-bedrock-flash.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/pci/pci.h"
#include "hw/pci-host/sgi-xbridge.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/reset.h"
#include "system/system.h"
#include "cpu.h"
#include "cpu-qom.h"
#include "sn1-model.h"
#include "sn1-node.h"

#define SN1_CONFIG0_RESET_INPUT UINT32_C(0x009aa985)

/*
 * Addresses the generated model does not carry. The model owns the payload
 * views, the register windows and the node's memory; the boot spaces the
 * node's own boot flash answers for, and the machine's PROM-loading
 * fallback below, are still decided here until the model describes them.
 * sgi-sn1-node.c holds the ones its own regions (Bedrock's back doors, the
 * boot flash's own two mmio spaces) use instead.
 */
#define SN1_SPACE_HUB UINT64_C(0x10000000000)
#define SN1_BOARD_ID_OFFSET UINT64_C(0x4000000)
#define SN1_BOARD_ID_VALUE UINT64_C(0x8000)
#define SN1_HUB_LOCAL_HIGH_BOOT (SN1_SPACE_HUB + UINT64_C(0x1fc00000))
#define SN1_HUB_REMOTE_FLASH (SN1_SPACE_HUB + UINT64_C(0x3a000000))
#define SN1_PROM_RESET UINT64_C(0x1fc00000)

/*
 * The controller holds one identification record per file, in order. The
 * machine names the files and claims nothing about what is in them: no
 * default record, and no record implied by the memory size.
 */
typedef struct SN1IdentityRecords {
    const char *name;
    ptrdiff_t offset;
    const char *description;
} SN1IdentityRecords;

static const SN1IdentityRecords sn1_identity_records[] = {
    {
        "chassis-eeprom", offsetof(SN1MachineState, chassis_eeprom),
        "Files holding the chassis identification records, in order",
    },
    {
        "board-eeprom", offsetof(SN1MachineState, board_eeprom),
        "Files holding the board identification records, in order",
    },
    {
        "spd-eeprom", offsetof(SN1MachineState, spd_eeprom),
        "Files holding the memory module presence-detect records, in order",
    },
};

static strList **sn1_identity_slot(Object *obj, void *opaque)
{
    const SN1IdentityRecords *records = opaque;

    return (strList **)((char *)SN1_MACHINE(obj) + records->offset);
}

static void sn1_get_identity_records(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    visit_type_strList(v, name, sn1_identity_slot(obj, opaque), errp);
}

static void sn1_set_identity_records(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    strList **slot = sn1_identity_slot(obj, opaque);
    strList *replacement = NULL;

    if (!visit_type_strList(v, name, &replacement, errp)) {
        return;
    }
    qapi_free_strList(*slot);
    *slot = replacement;
}

void sn1_set_l1_identity_records(SN1MachineState *s, DeviceState *l1)
{
    for (size_t i = 0; i < ARRAY_SIZE(sn1_identity_records); i++) {
        const SN1IdentityRecords *records = &sn1_identity_records[i];
        strList *file = *sn1_identity_slot(OBJECT(s), (void *)records);
        QList *files;

        if (!file) {
            continue;
        }
        files = qlist_new();
        for (; file; file = file->next) {
            qlist_append_str(files, file->value);
        }
        qdev_prop_set_array(l1, records->name, files);
    }
}

static void sn1_machine_finalize(Object *obj)
{
    SN1MachineState *s = SN1_MACHINE(obj);

    g_free(s->prom_views);
    for (size_t i = 0; i < ARRAY_SIZE(sn1_identity_records); i++) {
        qapi_free_strList(*sn1_identity_slot(obj, (void *)
                                             &sn1_identity_records[i]));
    }
}

static void sn1_cpu_reset(void *opaque)
{
    MIPSCPU *cpu = MIPS_CPU(opaque);

    cpu_reset(CPU(cpu));
    cpu->env.CP0_Config0 = deposit32(cpu->env.CP0_Config0, 0, 25,
                                    SN1_CONFIG0_RESET_INPUT);
}

static void sn1_machine_reset(MachineState *machine, ResetType type)
{
    CPUState *cs;

    /* Busless MIPS CPUs are outside the registered SysBus reset tree. */
    CPU_FOREACH(cs) {
        sn1_cpu_reset(cs);
    }
    qemu_devices_reset(type);
}

static void sn1_flash_reject_write(hwaddr offset, uint64_t value)
{
    qemu_log_mask(LOG_UNIMP,
                  "sn1: flash programming unsupported, dropping 0x%" PRIx64
                  " at flash offset 0x%" HWADDR_PRIx "\n", value, offset);
}

static uint64_t sn1_flash_erased_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    return MAKE_64BIT_MASK(0, size * 8);
}

static void sn1_flash_erased_write(void *opaque, hwaddr offset, uint64_t value,
                                   unsigned size)
{
    sn1_flash_reject_write(offset, value);
}

static const MemoryRegionOps sn1_flash_erased_ops = {
    .read = sn1_flash_erased_read,
    .write = sn1_flash_erased_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

/*
 * A run that supplies a flash image gets a writable part: both software
 * selections reach it, and every bounded view reads its stored bytes, so the
 * monitor's own rewrite of the first sector is what the next reset fetches.
 * Without an image the machine keeps the immutable payload it had before.
 */
/*
 * The processor's own views of the boot flash. A read handler rather than an
 * alias of the part's region: an alias forwards a write to its target even
 * when the alias is marked read-only, which would let a native command
 * sequence reach the part without passing through Bedrock's translation. A
 * copied region is no better, because it would own an unrelated RAM block that
 * the part's own invalidation never reaches, so an instruction translated from
 * it could survive a program that changed the byte underneath. The dispatch
 * cost on the firmware's fetch path is accepted for that.
 */
static MemTxResult sn1_prom_view_read(void *opaque, hwaddr offset,
                                      uint64_t *value, unsigned size,
                                      MemTxAttrs attrs)
{
    SN1PromView *view = opaque;
    uint64_t high, low;
    MemTxResult result;

    if (size == 8) {
        /* The part answers at most four bytes per cycle. */
        result = memory_region_dispatch_read(view->backing, offset, &high,
                                             MO_32 | MO_BE, attrs);
        result |= memory_region_dispatch_read(view->backing, offset + 4, &low,
                                              MO_32 | MO_BE, attrs);
        *value = (high << 32) | (uint32_t)low;
        return result;
    }
    return memory_region_dispatch_read(view->backing, offset, value,
                                       size_memop(size) | MO_BE, attrs);
}

static MemTxResult sn1_prom_view_write(void *opaque, hwaddr offset,
                                       uint64_t value, unsigned size,
                                       MemTxAttrs attrs)
{
    SN1PromView *view = opaque;

    qemu_log_mask(LOG_GUEST_ERROR, "sn1: %s execution view: refusing a %u-byte "
                  "write of 0x%" PRIx64 " at offset 0x%" HWADDR_PRIx "; the "
                  "boot flash takes commands through a Bedrock boot space, "
                  "not through a view the processor executes from\n",
                  view->name, size, value, offset);
    return MEMTX_ERROR;
}

static const MemoryRegionOps sn1_prom_view_ops = {
    .read_with_attrs = sn1_prom_view_read,
    .write_with_attrs = sn1_prom_view_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void sn1_init_prom_view(SN1PromView *view, MachineState *machine,
                               const char *name, MemoryRegion *backing)
{
    view->name = name;
    view->backing = backing;
    memory_region_init_io(&view->mmio, OBJECT(machine), &sn1_prom_view_ops,
                          view, name, sn1_origin300_model.prom_size);
}

/* Match the loaded CPU-module image to this machine's reset configuration. */
static void sn1_apply_cpu_config(uint8_t *payload)
{
    stl_be_p(payload + 0x64, UINT32_C(0x549aa985));
    stl_be_p(payload + 0x74, UINT32_C(500000000));
    stl_be_p(payload + 0x90, 1);
    stl_be_p(payload + 0x94, UINT32_C(0x9c));
    stl_be_p(payload + 0x98, 1);
}

static bool sn1_load_prom(SN1MachineState *s, MachineState *machine,
                          Error **errp)
{
    const SN1MachineModel *model = &sn1_origin300_model;
    MemoryRegion *system = get_system_memory();
    g_autoptr(GError) local_error = NULL;
    g_autofree char *contents = NULL;
    g_autofree char *path = NULL;
    MemoryRegion *stored;
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
    if (length != model->prom_size) {
        error_setg(errp,
                   "origin300: PROM image must be exactly %" PRIu64 " bytes",
                   model->prom_size);
        return false;
    }
    if (machine->kernel_filename) {
        error_setg(errp, "origin300 supports stock-PROM boot only");
        return false;
    }

    if (model->prom_second_selection.size != SGI_BEDROCK_FLASH_PART_SIZE) {
        error_setg(errp,
                   "origin300: second PROM selection size does not match "
                   "the Bedrock flash part size");
        return false;
    }

    stored = sn1_node_create_boot_flash(SN1_NODE(s->node), machine, errp);
    if (errp && *errp) {
        return false;
    }
    if (stored) {
        /*
         * With a part attached the views answer through handlers over its
         * storage: they report the bytes it holds now, including a change the
         * monitor has just programmed, and refuse a write instead of passing
         * it to the part as a command cycle. The generated table describes
         * the payload carrier and is used for that carrier only.
         */
        sn1_apply_cpu_config(memory_region_get_ram_ptr(stored));
        sn1_init_prom_view(&s->prom_view_reset, machine, "sn1.prom.reset",
                           stored);
        sn1_init_prom_view(&s->prom_view_local_high, machine,
                           "sn1.prom.local-high", stored);
        memory_region_add_subregion_overlap(system, SN1_PROM_RESET,
                                            &s->prom_view_reset.mmio, 1);
        /*
         * The high local view sits inside a boot space the flash answers for,
         * so it is overlaid above it. The remote space is left to the flash:
         * its read decode already serves the same bytes at the same offsets,
         * and an overlay at its base would shadow the command addresses, so a
         * monitor could not drive the part through that space at all.
         */
        memory_region_add_subregion_overlap(system, SN1_HUB_LOCAL_HIGH_BOOT,
                                            &s->prom_view_local_high.mmio, 1);
        return true;
    }

    if (!memory_region_init_ram_flags_nomigrate(&s->prom, OBJECT(machine),
                                                "sn1.prom", model->prom_size, 0,
                                                errp)) {
        return false;
    }
    memory_region_set_readonly(&s->prom, true);
    /* The stock PROM bytes are part of snapshots despite being immutable. */
    vmstate_register_ram_global(&s->prom);
    memcpy(memory_region_get_ram_ptr(&s->prom), contents, model->prom_size);
    sn1_apply_cpu_config(memory_region_get_ram_ptr(&s->prom));
    memory_region_set_dirty(&s->prom, 0, model->prom_size);

    s->prom_views = g_new0(MemoryRegion, model->num_prom_views);
    for (size_t i = 0; i < model->num_prom_views; i++) {
        const SN1ModelPromView *view = &model->prom_views[i];

        memory_region_init_alias(&s->prom_views[i], OBJECT(machine),
                                 view->name, &s->prom, view->offset,
                                 view->size);
        /* PROM views take precedence where their carriers overlap RAM. */
        memory_region_add_subregion_overlap(system, view->address,
                                            &s->prom_views[i], 1);
    }

    /* An empty second position answers erased through both boot spaces. */
    memory_region_init_io(&s->prom_flash, OBJECT(machine),
                          &sn1_flash_erased_ops, s, "sn1.prom.flash",
                          model->prom_second_selection.size);
    memory_region_add_subregion(system, model->prom_second_selection.address,
                                &s->prom_flash);
    memory_region_init_alias(&s->prom_flash_remote, OBJECT(machine),
                             "sn1.prom.flash-remote", &s->prom_flash, 0,
                             model->prom_second_selection.size);
    memory_region_add_subregion(system, SN1_HUB_REMOTE_FLASH,
                                &s->prom_flash_remote);
    return true;
}


static void sn1_configure_ioc3(PCIDevice *ioc3, const SN1ModelPciDevice *model)
{
    DriveInfo *nvram_dinfo = drive_get(IF_MTD, 0, 0);
    BlockBackend *nvram_blk = nvram_dinfo ?
        blk_by_legacy_dinfo(nvram_dinfo) : blk_by_name("sgi-nvram0");
    Chardev *serial_a = serial_hd(1);
    Chardev *serial_b = serial_hd(2);
    Chardev *record_a = serial_hd(3);
    Chardev *record_b = serial_hd(4);

    if (serial_a) {
        qdev_prop_set_chr(DEVICE(ioc3), "chardev-a", serial_a);
    }
    if (serial_b) {
        qdev_prop_set_chr(DEVICE(ioc3), "chardev-b", serial_b);
    }
    if (record_a) {
        qdev_prop_set_chr(DEVICE(ioc3), "record-a", record_a);
    }
    if (record_b) {
        qdev_prop_set_chr(DEVICE(ioc3), "record-b", record_b);
    }
    if (parallel_hds[0]) {
        qdev_prop_set_chr(DEVICE(ioc3), "parallel-chardev",
                          parallel_hds[0]);
    }
    qdev_prop_set_uint16(DEVICE(ioc3), "subsystem-id", model->subsystem_id);
    qdev_prop_set_uint32(DEVICE(ioc3), "population", model->population);
    qdev_prop_set_uint8(DEVICE(ioc3), "timekeeper-kind",
                        model->timekeeper_kind);
    /*
     * The IO8 board's transceiver answers at address 1, not the DP83840
     * model's Origin-200 default of 31: the BASEIO PROM's MII helpers
     * hard-code that address, and a real board's OpenBSD capture confirms
     * it (docs/datasheets/dp83840.md).
     */
    qdev_prop_set_uint8(DEVICE(ioc3), "phy-addr", model->phy_addr);
    /*
     * The IO8 board's transceiver is a DP83843, not the DP83840A model's
     * default part: its register 0x17 selects loopback differently and its
     * identity register reads differently, both of which the BASEIO PROM's
     * Twister loopback diagnostic depends on (docs/datasheets/dp83840.md).
     */
    object_property_set_bool(OBJECT(ioc3), "phy-dp83843", model->phy_dp83843,
                             &error_abort);
    if (nvram_blk) {
        qdev_prop_set_drive(DEVICE(ioc3), "nvram-drive", nvram_blk);
    }
}

static const char *sn1_pci_type(SN1ModelPciKind kind)
{
    switch (kind) {
    case SN1_PCI_ISP12160:
        return "qlogic-isp12160";
    case SN1_PCI_IOC3:
        return TYPE_SGI_IOC3;
    case SN1_PCI_LUCENT_OHCI:
        return "lucent-ohci";
    default:
        g_assert_not_reached();
    }
}

static void sn1_create_pci_devices(SN1MachineState *s, MachineState *machine)
{
    const SN1MachineModel *model = &sn1_origin300_model;
    DeviceState *xbridge = SN1_NODE(s->node)->xbridge;

    for (size_t i = 0; i < model->num_pci_devices; i++) {
        const SN1ModelPciDevice *entry = &model->pci_devices[i];
        PCIDevice *dev = pci_new(PCI_DEVFN(entry->device, entry->function),
                                  sn1_pci_type(entry->kind));
        g_autofree char *intx = g_strdup_printf("head-%x-pci-intx",
                                                entry->widget);
        g_autofree char *pin = g_strdup_printf("head-%x-pin", entry->widget);

        object_property_add_child(OBJECT(machine), entry->name, OBJECT(dev));
        if (entry->kind == SN1_PCI_IOC3) {
            sn1_configure_ioc3(dev, entry);
        }
        qdev_connect_gpio_out_named(
            xbridge, intx, entry->device * PCI_NUM_PINS,
            qdev_get_gpio_in_named(xbridge, pin, entry->interrupt_pin));
        pci_realize_and_unref(dev,
                              sgi_xbridge_pci_bus(SGI_XBRIDGE(xbridge),
                                                  entry->widget),
                              &error_fatal);
        pci_set_byte(dev->config + PCI_REVISION_ID, entry->revision);
        pci_set_word(dev->config + PCI_SUBSYSTEM_VENDOR_ID,
                     entry->subsystem_vendor_id);
        pci_set_word(dev->config + PCI_SUBSYSTEM_ID, entry->subsystem_id);
        if (entry->kind == SN1_PCI_IOC3) {
            qdev_connect_gpio_out(
                DEVICE(dev), 0,
                qdev_get_gpio_in_named(xbridge, pin, entry->superio_pin));
        }
    }
}

/*
 * The board-identification register both consumers read: OpenBSD through the
 * remote form and the stock firmware through the local one, each taking the
 * product code from bits 15 through 12. One stateless response serves both.
 * Only the aligned 64-bit read is answered; everything else is refused and
 * said out loud, because nothing establishes what the other bits or widths do.
 */
static MemTxResult sn1_board_id_read(void *opaque, hwaddr offset,
                                     uint64_t *value, unsigned size,
                                     MemTxAttrs attrs)
{
    if (offset != 0 || size != 8) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sn1: board identification answers only an aligned "
                      "eight-byte read; refusing %u byte%s at offset 0x%"
                      HWADDR_PRIx "\n", size, size == 1 ? "" : "s", offset);
        *value = 0;
        return MEMTX_ERROR;
    }
    *value = SN1_BOARD_ID_VALUE;
    return MEMTX_OK;
}

static MemTxResult sn1_board_id_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size,
                                      MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sn1: board identification is fixed; refusing a %u-byte "
                  "write of 0x%" PRIx64 " at offset 0x%" HWADDR_PRIx "\n",
                  size, value, offset);
    return MEMTX_ERROR;
}

static const MemoryRegionOps sn1_board_id_ops = {
    .read_with_attrs = sn1_board_id_read,
    .write_with_attrs = sn1_board_id_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void sn1_map_memory(SN1MachineState *s, MachineState *machine)
{
    const SN1MachineModel *model = &sn1_origin300_model;
    MemoryRegion *system = get_system_memory();

    memory_region_add_subregion(system, model->ram_address, machine->ram);
    memory_region_init_alias(&s->hub_memory, OBJECT(machine),
                             "sn1.hub.local-memory", machine->ram, 0,
                             model->hub_memory.size);
    memory_region_add_subregion(system, model->hub_memory.address,
                                &s->hub_memory);

    memory_region_init(&s->hub_local_registers, OBJECT(machine),
                       "sn1.hub.local-registers",
                       model->hub_local_registers.size);
    memory_region_add_subregion(system, model->hub_local_registers.address,
                                &s->hub_local_registers);

    memory_region_init_io(&s->board_id, OBJECT(machine), &sn1_board_id_ops, s,
                          "sn1.board-id", 8);
    memory_region_add_subregion(&s->hub_local_registers, SN1_BOARD_ID_OFFSET,
                                &s->board_id);
    /* The remote form reaches the same response above the Bedrock window. */
    memory_region_init_alias(&s->board_id_remote, OBJECT(machine),
                             "sn1.board-id.remote", &s->board_id, 0, 8);
    memory_region_add_subregion_overlap(system,
                                        sn1_origin300_model
                                            .hub_remote_registers_address +
                                        SN1_BOARD_ID_OFFSET,
                                        &s->board_id_remote, 1);
}

static void origin300_init(MachineState *machine)
{
    SN1MachineState *s = SN1_MACHINE(machine);
    const SN1MachineModel *model = &sn1_origin300_model;

    if (machine->ram_size != model->ram_size) {
        error_setg(&error_fatal, "origin300 requires %" PRIu64 " MiB of RAM",
                   model->ram_size / MiB);
        return;
    }
    if (machine->smp.cpus != model->cpu_count) {
        error_setg(&error_fatal, "origin300 requires %u processors",
                   model->cpu_count);
        return;
    }

    s->cpu_refclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(s->cpu_refclk, model->cpu_clock_hz);

    /*
     * Created (but not yet realized) ahead of sn1_load_prom: a flash part
     * is the node's own boot-flash device, parented here, while whether
     * one is attached at all is the machine's -drive question, decided in
     * load_prom as it always has been.
     */
    s->node = DEVICE(object_new(TYPE_SGI_SN1_NODE));
    object_property_add_child(OBJECT(machine), "node0", OBJECT(s->node));
    object_unref(OBJECT(s->node));
    object_property_set_link(OBJECT(s->node), "ram", OBJECT(machine->ram),
                             &error_abort);
    qdev_connect_clock_in(s->node, "cpu-refclk", s->cpu_refclk);

    sn1_map_memory(s, machine);
    if (!sn1_load_prom(s, machine, &error_fatal)) {
        return;
    }
    sysbus_realize(SYS_BUS_DEVICE(s->node), &error_fatal);
    sn1_create_pci_devices(s, machine);
}

static const char *sn1_cpu_type(SN1ModelCpuKind kind)
{
    switch (kind) {
    case SN1_CPU_R14000:
        return MIPS_CPU_TYPE_NAME("R14000");
    default:
        g_assert_not_reached();
    }
}

static void origin300_class_init(ObjectClass *oc, const void *data)
{
    static const char *valid_cpu_types[2];
    const SN1MachineModel *model = &sn1_origin300_model;
    MachineClass *mc = MACHINE_CLASS(oc);

    valid_cpu_types[0] = sn1_cpu_type(model->cpu_kind);
    mc->desc = model->description;
    mc->init = origin300_init;
    mc->reset = sn1_machine_reset;
    mc->default_cpu_type = valid_cpu_types[0];
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_cpus = model->cpu_count;
    mc->min_cpus = model->cpu_count;
    mc->max_cpus = model->cpu_count;
    mc->default_ram_size = model->ram_size;
    mc->default_ram_id = "sn1.ram";

    for (size_t i = 0; i < ARRAY_SIZE(sn1_identity_records); i++) {
        const SN1IdentityRecords *records = &sn1_identity_records[i];

        object_class_property_add(oc, records->name, "strList",
                                  sn1_get_identity_records,
                                  sn1_set_identity_records, NULL,
                                  (void *)records);
        object_class_property_set_description(oc, records->name,
                                              records->description);
    }
}

static const TypeInfo origin300_machine_type = {
    .name = TYPE_SN1_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(SN1MachineState),
    .instance_finalize = sn1_machine_finalize,
    .class_init = origin300_class_init,
};

static void origin300_machine_register_types(void)
{
    type_register_static(&origin300_machine_type);
}

type_init(origin300_machine_register_types)
