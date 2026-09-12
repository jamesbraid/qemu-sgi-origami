/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qapi/qapi-visit-common.h"
#include "qapi/visitor.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "block/block-global-state.h"
#include "block/block-io.h"
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
#include "hw/misc/sgi-bedrock.h"
#include "hw/misc/sgi-bedrock-flash.h"
#include "hw/misc/sgi-bedrock-pi.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/misc/sgi-l1.h"
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

#define SN1_CONFIG0_RESET_INPUT UINT32_C(0x009aa985)

/*
 * Addresses the generated model does not carry. The model owns the payload
 * views, the register windows and the node's memory; the boot spaces and the
 * two back doors below are the machine's own, so they stay here until the
 * model describes them.
 */
#define SN1_SPACE_HUB UINT64_C(0x10000000000)
#define SN1_HUB_LOCAL_BOOT (SN1_SPACE_HUB + UINT64_C(0x18000000))
#define SN1_BOARD_ID_OFFSET UINT64_C(0x4000000)
#define SN1_BOARD_ID_VALUE UINT64_C(0x8000)
#define SN1_HUB_LOCAL_HIGH_BOOT (SN1_SPACE_HUB + UINT64_C(0x1fc00000))
#define SN1_HUB_REMOTE_BOOT (SN1_SPACE_HUB + UINT64_C(0x38000000))
#define SN1_HUB_REMOTE_FLASH (SN1_SPACE_HUB + UINT64_C(0x3a000000))
#define SN1_HUB_ECC (SN1_SPACE_HUB + UINT64_C(0x140000000))
#define SN1_HUB_DIRECTORY (SN1_SPACE_HUB + UINT64_C(0x1c0000000))
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

static void sn1_cpu_unhalt(CPUState *cs, run_on_cpu_data data)
{
    cpu_reset_interrupt(cs, CPU_INTERRUPT_HALT);
    cs->halted = 0;
}

static void sn1_cpu_halt(CPUState *cs, run_on_cpu_data data)
{
    cpu_interrupt(cs, CPU_INTERRUPT_HALT);
}

static void sn1_cpu_b_enable(void *opaque, int n, int level)
{
    SN1MachineState *s = opaque;
    CPUState *cs = s->cpu[1];

    BQL_LOCK_GUARD();
    if (level && !s->cpu_b_enabled) {
        s->cpu_b_enabled = true;
        async_safe_run_on_cpu(cs, sn1_cpu_unhalt, RUN_ON_CPU_NULL);
    } else if (!level && s->cpu_b_enabled) {
        s->cpu_b_enabled = false;
        /*
         * Queue both edges on CPU B.  Work-list order prevents an older
         * unhalt from clearing this newer halt request.
         */
        async_run_on_cpu(cs, sn1_cpu_halt, RUN_ON_CPU_NULL);
    }
}

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

static void sn1_set_l1_identity_records(SN1MachineState *s)
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
        qdev_prop_set_array(s->l1, records->name, files);
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
    g_free(s->cpu);
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

static target_ulong sn1_store_transaction(MIPSCPU *cpu, MemoryRegion *mr,
                                          hwaddr offset,
                                          target_ulong value, unsigned size,
                                          void *opaque)
{
    SN1MachineState *s = opaque;
    SGIBedrockState *bedrock = SGI_BEDROCK(s->bedrock);

    (void)cpu;
    if (mr == &bedrock->md.ecc_mmio) {
        return sgi_bedrock_md_ecc_store_value(&bedrock->md, offset,
                                               value, size);
    }
    return value;
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
    s->cpu = g_new0(CPUState *, sn1_origin300_model.cpu_count);
    for (unsigned int i = 0; i < sn1_origin300_model.cpu_count; i++) {
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
        cpu->store_transaction = sn1_store_transaction;
        cpu->store_transaction_opaque = s;
        qdev_realize(cpu_dev, NULL, &error_fatal);
        cpu_mips_irq_init_cpu(cpu);
        cpu_mips_clock_init(cpu);
        s->cpu[i] = CPU(cpu);
    }
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

static BlockDriverState *sn1_open_flash_slice(BlockDriverState *backing,
                                               int64_t offset, int flags,
                                               Error **errp)
{
    QDict *options = qdict_new();

    qdict_put_str(options, "driver", "raw");
    qdict_put_str(options, "file", bdrv_get_node_name(backing));
    qdict_put_int(options, "offset", offset);
    qdict_put_int(options, "size", SGI_BEDROCK_FLASH_PART_SIZE);
    return bdrv_open(NULL, NULL, options, flags, errp);
}

static MemoryRegion *sn1_create_boot_flash(SN1MachineState *s,
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
    lower = sn1_open_flash_slice(backing, 0, flags, errp);
    if (!lower) {
        return NULL;
    }
    upper = sn1_open_flash_slice(backing, SGI_BEDROCK_FLASH_PART_SIZE, flags,
                                 errp);
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

    s->boot_flash = qdev_new(TYPE_SGI_BEDROCK_FLASH);
    object_property_add_child(OBJECT(machine), "boot-flash",
                              OBJECT(s->boot_flash));
    if (!qdev_prop_set_drive_err(s->boot_flash, "drive0", blk, errp) ||
        !object_property_set_str(OBJECT(s->boot_flash), "drive1",
                                 bdrv_get_node_name(upper), errp)) {
        bdrv_unref(upper);
        return NULL;
    }
    bdrv_unref(upper);
    flash = SYS_BUS_DEVICE(s->boot_flash);
    if (!sysbus_realize_and_unref(flash, errp)) {
        return NULL;
    }

    /*
     * One region per boot space, each the whole 128 MiB window rather than a
     * 16 MiB selection: reads and writes decode differently inside it, so the
     * alternate selection is a read window within the space and a store is
     * addressed from the space's base. Mapping the selections separately would
     * put every write offset 32 MiB out.
     */
    sysbus_mmio_map(flash, 0, SN1_HUB_LOCAL_BOOT);
    sysbus_mmio_map(flash, 1, SN1_HUB_REMOTE_BOOT);
    /* The processor executes position zero through read-only views. */
    return SGI_BEDROCK_FLASH(s->boot_flash)->flash_mmio[0];
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

    stored = sn1_create_boot_flash(s, machine, errp);
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

static void sn1_create_bedrock(SN1MachineState *s, MachineState *machine)
{
    const SN1MachineModel *model = &sn1_origin300_model;
    Chardev *l1_console = serial_hd(0);
    MemoryRegion *system = get_system_memory();
    MemoryRegion *l1_uart;
    DeviceState *l1_uart_dev;
    MemoryRegion *bedrock_widget;
    SysBusDevice *bedrock;

    s->l1 = qdev_new(TYPE_SGI_L1);
    object_property_add_child(OBJECT(machine), "l1", OBJECT(s->l1));
    if (l1_console) {
        qdev_prop_set_chr(s->l1, "chardev", l1_console);
    }
    sn1_set_l1_identity_records(s);
    qdev_realize(s->l1, NULL, &error_fatal);
    object_unref(OBJECT(s->l1));

    l1_uart_dev = qdev_new(TYPE_SGI_L1_UART);
    object_property_add_child(OBJECT(machine), "l1-uart",
                              OBJECT(l1_uart_dev));
    object_property_set_link(OBJECT(l1_uart_dev), "controller",
                             OBJECT(s->l1), &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(l1_uart_dev), &error_fatal);
    object_unref(OBJECT(l1_uart_dev));
    l1_uart = sysbus_mmio_get_region(SYS_BUS_DEVICE(l1_uart_dev), 0);

    s->bedrock = qdev_new(TYPE_SGI_BEDROCK);
    object_property_add_child(OBJECT(machine), "bedrock", OBJECT(s->bedrock));
    qdev_prop_set_uint32(s->bedrock, "processor-count", model->cpu_count);
    qdev_prop_set_uint8(s->bedrock, "revision", model->bedrock_revision);
    qdev_prop_set_uint8(s->bedrock, "local-region",
                        model->bedrock_local_region);
    qdev_prop_set_uint8(s->bedrock, "initiator-region",
                        model->bedrock_initiator_region);
    object_property_set_link(OBJECT(s->bedrock), "dma-memory", OBJECT(system),
                             &error_abort);
    object_property_set_link(OBJECT(s->bedrock), "node-memory",
                             OBJECT(machine->ram), &error_abort);
    object_property_set_link(OBJECT(s->bedrock), "l1-uart", OBJECT(l1_uart),
                             &error_abort);

    s->xbridge = qdev_new(TYPE_SGI_XBRIDGE);
    object_property_add_child(OBJECT(machine), "xbridge", OBJECT(s->xbridge));
    qdev_prop_set_uint8(s->xbridge, "host-port", model->xbridge_host_port);
    bedrock_widget = sysbus_mmio_get_region(SYS_BUS_DEVICE(s->bedrock), 2);
    object_property_set_link(OBJECT(s->xbridge), "fabric",
                             OBJECT(bedrock_widget), &error_abort);
    for (size_t i = 0; i < model->num_xbridge_widgets; i++) {
        unsigned int widget = model->xbridge_widgets[i];
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

    memory_region_add_subregion(system, model->xtalk_address,
                                sysbus_mmio_get_region(bedrock, 1));
    memory_region_add_subregion_overlap(system, model->bedrock_local_address,
                                        sysbus_mmio_get_region(bedrock, 0), 1);
    memory_region_init_alias(&s->bedrock_node, OBJECT(machine),
                             "sn1.bedrock.node0",
                             sysbus_mmio_get_region(bedrock, 0), 0,
                             model->bedrock_node.size);
    memory_region_add_subregion_overlap(system, model->bedrock_node.address,
                                        &s->bedrock_node, 1);
    memory_region_add_subregion(system, model->hub_remote_registers_address,
                                sysbus_mmio_get_region(bedrock, 3));
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
        MIPSCPU *mips_cpu = MIPS_CPU(s->cpu[cpu]);

        qdev_connect_gpio_out_named(
            s->bedrock, "nmi", cpu,
            qdev_get_gpio_in_named(DEVICE(mips_cpu), "nmi", 0));
        for (unsigned int line = 0; line < SGI_BEDROCK_PI_HW_IRQS; line++) {
            qdev_connect_gpio_out_named(
                s->bedrock, "hardware-irq",
                cpu * SGI_BEDROCK_PI_HW_IRQS + line,
                mips_cpu->env.irq[SN1_MIPS_HARDWARE_IRQ_OFFSET + line]);
        }
    }
}

static void sn1_configure_ioc3(PCIDevice *ioc3, const SN1ModelPciDevice *model)
{
    DriveInfo *nvram_dinfo = drive_get(IF_MTD, 0, 0);
    BlockBackend *nvram_blk = nvram_dinfo ?
        blk_by_legacy_dinfo(nvram_dinfo) : blk_by_name("sgi-nvram0");
    Chardev *serial_a = serial_hd(1);
    Chardev *serial_b = serial_hd(2);

    if (serial_a) {
        qdev_prop_set_chr(DEVICE(ioc3), "chardev-a", serial_a);
    }
    if (serial_b) {
        qdev_prop_set_chr(DEVICE(ioc3), "chardev-b", serial_b);
    }
    if (parallel_hds[0]) {
        qdev_prop_set_chr(DEVICE(ioc3), "parallel-chardev",
                          parallel_hds[0]);
    }
    qdev_prop_set_uint16(DEVICE(ioc3), "subsystem-id", model->subsystem_id);
    qdev_prop_set_uint32(DEVICE(ioc3), "population", model->population);
    qdev_prop_set_uint8(DEVICE(ioc3), "timekeeper-kind",
                        model->timekeeper_kind);
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
            s->xbridge, intx, entry->device * PCI_NUM_PINS,
            qdev_get_gpio_in_named(s->xbridge, pin, entry->interrupt_pin));
        pci_realize_and_unref(dev,
                              sgi_xbridge_pci_bus(SGI_XBRIDGE(s->xbridge),
                                                  entry->widget),
                              &error_fatal);
        pci_set_byte(dev->config + PCI_REVISION_ID, entry->revision);
        pci_set_word(dev->config + PCI_SUBSYSTEM_VENDOR_ID,
                     entry->subsystem_vendor_id);
        pci_set_word(dev->config + PCI_SUBSYSTEM_ID, entry->subsystem_id);
        if (entry->kind == SN1_PCI_IOC3) {
            qdev_connect_gpio_out(
                DEVICE(dev), 0,
                qdev_get_gpio_in_named(s->xbridge, pin, entry->superio_pin));
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

    sn1_map_memory(s, machine);
    if (!sn1_load_prom(s, machine, &error_fatal)) {
        return;
    }
    sn1_create_cpus(s, machine);
    sn1_create_bedrock(s, machine);
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
    .instance_init = sn1_machine_init,
    .instance_finalize = sn1_machine_finalize,
    .class_init = origin300_class_init,
};

static void origin300_machine_register_types(void)
{
    type_register_static(&origin300_machine_type);
}

type_init(origin300_machine_register_types)
