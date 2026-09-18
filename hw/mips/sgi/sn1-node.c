/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 300 ("SN1", IP35/Bedrock) node device. See sn1-node.h.
 *
 * The family builder (sn-machine.c) creates one of these from the
 * format-2 descriptor's Bedrock node record, links its RAM and CPU
 * clock, sets its populated I/O board (if any) and realizes it; this
 * file owns everything the node needs to become a running Origin 300:
 * the PROM/flash policy, the node's own address-space views, the CPUs,
 * Bedrock, L1 and the XBridge, a fitted part of the IP45 node board
 * itself (docs/designs/sn-machine-model-architecture.md, "Node
 * devices"). The IO-8's fixed devices are a separate sgi-baseio (the
 * "io8" preset, hw/mips/sgi/baseio.c) on the chip's own head F -- see
 * sn1_node_build_io8()'s own comment for why it is built here rather
 * than in sn-machine.c directly.
 *
 * Node strides, window splits and unit offsets are Bedrock hardware
 * facts, not configuration, so the addresses below are constants here
 * rather than descriptor fields; see decision D-0026 (docs/decisions/
 * D-0026-machine-description-ownership.md in the Origami repository)
 * for the rule this follows.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/datadir.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "block/block-global-state.h"
#include "block/block-io.h"
#include "qobject/qdict.h"
#include "chardev/char.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/mips/sgi/baseio.h"
#include "hw/mips/sgi/sn-machine.h"
#include "hw/pci-host/sgi-xbridge.h"
#include "hw/misc/sgi-bedrock.h"
#include "hw/misc/sgi-bedrock-flash.h"
#include "hw/misc/sgi-bedrock-pi.h"
#include "hw/misc/sgi-l1.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/reset.h"
#include "system/system.h"
#include "cpu.h"
#include "cpu-qom.h"
#include "sn1-node.h"
#include "sn1-topology.h"

/*
 * The node's own crosstalk/Hub-register address decode: identical on
 * every Origin 300 measured, so constants here rather than descriptor
 * data repeated for a node kind with exactly one shipped configuration.
 */
#define SN1_SPACE_HUB UINT64_C(0x10000000000)
#define SN1_HUB_LOCAL_BOOT (SN1_SPACE_HUB + UINT64_C(0x18000000))
#define SN1_HUB_LOCAL_HIGH_BOOT (SN1_SPACE_HUB + UINT64_C(0x1fc00000))
#define SN1_HUB_REMOTE_BOOT (SN1_SPACE_HUB + UINT64_C(0x38000000))
#define SN1_HUB_ECC (SN1_SPACE_HUB + UINT64_C(0x140000000))
#define SN1_HUB_DIRECTORY (SN1_SPACE_HUB + UINT64_C(0x1c0000000))
#define SN1_PROM_RESET UINT64_C(0x1fc00000)
#define SN1_PROM_RAM_COPY UINT64_C(0x01a00000)
#define SN1_PROM_RAM_COPY_SIZE UINT64_C(0x00200000)
/*
 * The register window's own split, the same in both forms: a PI head
 * below the L1 UART's 0x80 offset, the UART itself, then a PI tail. See
 * sn1_node_create_bedrock's remote and local construction below.
 */
#define SN1_HUB_REGISTER_HEAD_SIZE UINT64_C(0x80)
#define SN1_HUB_REGISTER_UART_END (SN1_HUB_REGISTER_HEAD_SIZE + SGI_L1_SIZE)
#define SN1_PROM_SECOND_SELECTION (SN1_SPACE_HUB + UINT64_C(0x1a000000))
#define SN1_PROM_SECOND_SELECTION_SIZE UINT64_C(0x800000)
#define SN1_HUB_REMOTE_FLASH (SN1_SPACE_HUB + UINT64_C(0x3a000000))

#define SN1_RAM_ADDRESS UINT64_C(0x0)
#define SN1_HUB_MEMORY_ADDRESS SN1_SPACE_HUB
#define SN1_HUB_MEMORY_SIZE UINT64_C(0x10000000)
#define SN1_HUB_LOCAL_REGISTERS_ADDRESS (SN1_SPACE_HUB + UINT64_C(0x10000000))
#define SN1_HUB_LOCAL_REGISTERS_SIZE UINT64_C(0x8000000)
#define SN1_XTALK_ADDRESS UINT64_C(0x20000000000)
#define SN1_BEDROCK_LOCAL_ADDRESS UINT64_C(0x20001000000)
#define SN1_BEDROCK_NODE_ADDRESS UINT64_C(0x20001800000)
#define SN1_BEDROCK_NODE_SIZE UINT64_C(0x800000)
#define SN1_HUB_REMOTE_REGISTERS_ADDRESS UINT64_C(0x10030000000)
#define SN1_BEDROCK_REVISION 2
#define SN1_BEDROCK_LOCAL_REGION 0
#define SN1_BEDROCK_INITIATOR_REGION 0
#define SN1_XBRIDGE_HOST_PORT 0xa

#define SN1_BOARD_ID_OFFSET UINT64_C(0x4000000)
/* Bits 15:12 are the product code; the low byte selects the 500 MHz PSC. */
#define SN1_BOARD_ID_VALUE UINT64_C(0x8001)

#define SN1_MIPS_HARDWARE_IRQ_OFFSET 2

#define SN1_INSTALLED_BANK_MASK 0xc
/* Bedrock's own architectural bank count; matches sn0.h's SN0_MAX_MEMORY_BANKS. */
#define SN1_ARCHITECTURAL_MEMORY_BANKS 8
/*
 * The shipped Origin 300 inventory: two 256 MiB banks at locational
 * banks two and three. See sn1_node_create_bedrock's comment on why
 * this stays a literal instead of node->model_node->bank_mib.
 */
static const uint16_t sn1_installed_bank_mib[SN1_ARCHITECTURAL_MEMORY_BANKS] = {
    0, 0, 256, 256, 0, 0, 0, 0,
};

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

/*
 * Creates the node's boot-flash part from -drive if=pflash and returns the
 * processor's read view of its first selection, or NULL with no error when
 * no such drive was given (the caller then falls back to an immutable
 * -bios image). node_index selects the pflash unit the way sgi_machine_
 * load_flash's own per-node override does for the Hub path: unit N for
 * node N, so a second node (once one ships) takes its own drive rather
 * than colliding with the first's.
 */
static MemoryRegion *sn1_node_create_boot_flash(SN1NodeState *node,
                                                unsigned int node_index,
                                                Error **errp)
{
    DriveInfo *dinfo = drive_get(IF_PFLASH, 0, node_index);
    DriveInfo *extra = drive_get(IF_PFLASH, 0, node_index + 1);
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

/* Match the loaded CPU-module image to this machine's reset configuration. */
static void sn1_apply_cpu_config(uint8_t *payload)
{
    stl_be_p(payload + 0x64, UINT32_C(0x549aa985));
    stl_be_p(payload + 0x74, UINT32_C(500000000));
    stl_be_p(payload + 0x90, 1);
    stl_be_p(payload + 0x94, UINT32_C(0x9c));
    stl_be_p(payload + 0x98, 1);
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
 * The processor fetches from cacheable ROM devices, not MMIO handlers. The
 * flash's write callback refreshes their copies and invalidates old code.
 * An alias would pass writes through to the part's command interface.
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

static void sn1_prom_view_apply(SN1PromView *view, DirtyBitmapSnapshot *dirty,
                                uint64_t size)
{
    uint8_t *dst = memory_region_get_ram_ptr(&view->rom);
    const uint8_t *src = memory_region_get_ram_ptr(view->backing);
    uint64_t offset;

    for (offset = 0; offset < size; offset += 4 * KiB) {
        uint64_t length = MIN(4 * KiB, size - offset);

        if (dirty && !memory_region_snapshot_get_dirty(view->backing, dirty,
                                                        offset, length)) {
            continue;
        }
        memcpy(dst + offset, src + offset, length);
        memory_region_flush_rom_device(&view->rom, offset, length);
    }
}

static bool sn1_init_prom_view(SN1PromView *view, Object *owner,
                               const char *name, MemoryRegion *backing,
                               uint64_t size, Error **errp)
{
    view->name = name;
    view->backing = backing;
    if (!memory_region_init_rom_device(&view->rom, owner, &sn1_prom_view_ops,
                                       view, name, size, errp)) {
        return false;
    }
    sn1_prom_view_apply(view, NULL, size);
    return true;
}

static void sn1_refresh_prom_views(SN1NodeState *node, bool everything)
{
    uint64_t prom_size = node->model_node->flash->size;
    MemoryRegion *backing = node->prom_view_reset.backing;
    g_autofree DirtyBitmapSnapshot *snapshot =
        memory_region_snapshot_and_clear_dirty(backing, 0, prom_size,
                                               DIRTY_MEMORY_VGA);

    sn1_prom_view_apply(&node->prom_view_reset, everything ? NULL : snapshot,
                       prom_size);
    sn1_prom_view_apply(&node->prom_view_local_high,
                       everything ? NULL : snapshot, prom_size);
}

static void sn1_prom_flash_programmed(void *opaque, unsigned position)
{
    if (position == 0) {
        sn1_refresh_prom_views(opaque, false);
    }
}

/*
 * The 25-bit reset-latched processor mode field (bits 24:0 of
 * CP0_Config0), the R10000 family's own architectural width -- the
 * same mask sn-machine.c's shared prefix applies for the Hub, kept as
 * its own constant here because this node's reset cannot share that
 * prefix's ordering (see sn1_node_full_reset).
 */
#define SN1_PROCESSOR_MODE_MASK 0x01ffffff

void sn1_node_full_reset(SN1NodeState *node, ResetType type)
{
    uint32_t mode_bits =
        node->model_node->cpu_slots->value->assembly->mode_bits;
    unsigned int i;

    /*
     * cpu_reset() must run before qemu_devices_reset(): each CPU's
     * "start-powered-off" property re-halts it on every cpu_reset(),
     * not only at creation, so releasing them (below) has to follow
     * both, and qemu_devices_reset() resetting Bedrock/L1/XBridge
     * first would let a CPU race a device that assumes it is not
     * running yet. The Hub takes the opposite order and releases
     * through sgi_hub_set_cpu_enable instead, which carries no such
     * constraint.
     */
    for (i = 0; i < node->nr_cpus; i++) {
        CPUState *cs = CPU(node->cpu[i]);
        CPUMIPSState *env = &node->cpu[i]->env;

        cpu_reset(cs);
        env->CP0_Config0 = (env->CP0_Config0 & ~(uint32_t)
                           SN1_PROCESSOR_MODE_MASK) |
                           (mode_bits & SN1_PROCESSOR_MODE_MASK);
    }
    qemu_devices_reset(type);
    if (node->boot_flash) {
        sn1_refresh_prom_views(node, true);
    }

    /*
     * Every CPU exists now. Release them back-to-back under BQL; generic
     * vCPU resume can still skew their first fetch, and this is not a
     * claim about physical reset-edge timing.
     */
    for (i = 0; i < node->nr_cpus; i++) {
        CPUState *cs = CPU(node->cpu[i]);

        cs->halted = false;
        qemu_cpu_kick(cs);
    }
}

/*
 * Where each named flash view executes from: a Bedrock hardware fact
 * (the boot space it is wired to), not configuration. "local" and
 * "remote" are answered by the flash part's own command decode
 * (sn1_node_create_boot_flash) once one is attached, so only "reset"
 * and "local-high" are mapped as separate SN1PromView aliases in that
 * case; the immutable -bios path below maps every named view here.
 */
/*
 * Every named view a Bedrock node's flash controller answers, in the
 * order the immutable -bios path maps them: fixed by the chip, not
 * carried by the compiled catalogue (see the header comment on
 * sn1_node_attach_prom's own use of this table).
 */
static const char * const sn1_prom_view_names[] = {
    "sn1.prom.reset", "sn1.prom.local", "sn1.prom.local-high",
    "sn1.prom.remote",
};

static bool sn1_node_flash_view_address(const char *name, uint64_t *address)
{
    if (!strcmp(name, "sn1.prom.reset")) {
        *address = SN1_PROM_RESET;
    } else if (!strcmp(name, "sn1.prom.local")) {
        *address = SN1_HUB_LOCAL_BOOT;
    } else if (!strcmp(name, "sn1.prom.local-high")) {
        *address = SN1_HUB_LOCAL_HIGH_BOOT;
    } else if (!strcmp(name, "sn1.prom.remote")) {
        *address = SN1_HUB_REMOTE_BOOT;
    } else {
        return false;
    }
    return true;
}

bool sn1_node_attach_prom(SN1NodeState *node, unsigned int node_index,
                          const uint8_t *contents, uint64_t length,
                          Error **errp)
{
    uint64_t prom_size = node->model_node->flash->size;
    MemoryRegion *system = get_system_memory();
    MemoryRegion *stored;
    size_t i;

    if (length != prom_size) {
        error_setg(errp,
                   "origin300: PROM image must be exactly %" PRIu64 " bytes",
                   prom_size);
        return false;
    }

    stored = sn1_node_create_boot_flash(node, node_index, errp);
    if (errp && *errp) {
        return false;
    }
    if (stored) {
        /*
         * With a part attached the views answer through handlers over its
         * storage: they report the bytes it holds now, including a change the
         * monitor has just programmed, and refuse a write instead of passing
         * it to the part as a command cycle.
         */
        sn1_apply_cpu_config(memory_region_get_ram_ptr(stored));
        memory_region_set_log(stored, true, DIRTY_MEMORY_VGA);
        if (!sn1_init_prom_view(&node->prom_view_reset, OBJECT(node),
                                "sn1.prom.reset", stored, prom_size, errp) ||
            !sn1_init_prom_view(&node->prom_view_local_high, OBJECT(node),
                                "sn1.prom.local-high", stored, prom_size,
                                errp)) {
            return false;
        }
        SGI_BEDROCK_FLASH(node->boot_flash)->programmed =
            sn1_prom_flash_programmed;
        SGI_BEDROCK_FLASH(node->boot_flash)->programmed_opaque = node;
        memory_region_add_subregion_overlap(system, SN1_PROM_RESET,
                                            &node->prom_view_reset.rom, 1);
        /*
         * The high local view sits inside a boot space the flash answers for,
         * so it is overlaid above it. The remote space is left to the flash:
         * its read decode already serves the same bytes at the same offsets,
         * and an overlay at its base would shadow the command addresses, so a
         * monitor could not drive the part through that space at all.
         */
        memory_region_add_subregion_overlap(system, SN1_HUB_LOCAL_HIGH_BOOT,
                                            &node->prom_view_local_high.rom,
                                            1);
        return true;
    }

    if (!memory_region_init_ram_flags_nomigrate(&node->prom, OBJECT(node),
                                                "sn1.prom", prom_size, 0,
                                                errp)) {
        return false;
    }
    memory_region_set_readonly(&node->prom, true);
    /* The stock PROM bytes are part of snapshots despite being immutable. */
    vmstate_register_ram_global(&node->prom);
    memcpy(memory_region_get_ram_ptr(&node->prom), contents, prom_size);
    sn1_apply_cpu_config(memory_region_get_ram_ptr(&node->prom));
    memory_region_set_dirty(&node->prom, 0, prom_size);

    /*
     * Which named views this node's flash controller answers, and their
     * addresses, are Bedrock hardware facts (decision D-0026), not
     * configuration: every view the compiler ever emitted for a Bedrock
     * node was already sized to the flash chip's own total size, so
     * SN1_PROM_VIEW_NAMES names them directly instead of reading a list
     * the compiled catalogue no longer carries.
     */
    node->prom_views = g_new0(MemoryRegion, ARRAY_SIZE(sn1_prom_view_names));
    for (i = 0; i < ARRAY_SIZE(sn1_prom_view_names); i++) {
        const char *name = sn1_prom_view_names[i];
        uint64_t address;

        if (!sn1_node_flash_view_address(name, &address)) {
            error_setg(errp, "origin300: flash view '%s' names no known "
                      "boot space", name);
            return false;
        }
        memory_region_init_alias(&node->prom_views[i], OBJECT(node),
                                 name, &node->prom, 0, prom_size);
        /* PROM views take precedence where their carriers overlap RAM. */
        memory_region_add_subregion_overlap(system, address,
                                            &node->prom_views[i], 1);
    }

    /* An empty second position answers erased through both boot spaces. */
    memory_region_init_io(&node->prom_flash, OBJECT(node),
                          &sn1_flash_erased_ops, node, "sn1.prom.flash",
                          SN1_PROM_SECOND_SELECTION_SIZE);
    memory_region_add_subregion(system, SN1_PROM_SECOND_SELECTION,
                                &node->prom_flash);
    memory_region_init_alias(&node->prom_flash_remote, OBJECT(node),
                             "sn1.prom.flash-remote", &node->prom_flash, 0,
                             SN1_PROM_SECOND_SELECTION_SIZE);
    memory_region_add_subregion(system, SN1_HUB_REMOTE_FLASH,
                                &node->prom_flash_remote);
    return true;
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
    SN1NodeState *node = opaque;
    SGIBedrockState *bedrock = SGI_BEDROCK(node->bedrock);

    if (!sgi_bedrock_hub_registers_permitted(bedrock)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sn1: board identification read denied for region %u\n",
                      (unsigned)bedrock->initiator_region);
        *value = 0;
        return MEMTX_DECODE_ERROR;
    }
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
    SN1NodeState *node = opaque;
    SGIBedrockState *bedrock = SGI_BEDROCK(node->bedrock);

    if (!sgi_bedrock_hub_registers_permitted(bedrock)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sn1: board identification write denied for region %u\n",
                      (unsigned)bedrock->initiator_region);
        return MEMTX_DECODE_ERROR;
    }
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

static void sn1_node_map_memory(SN1NodeState *node)
{
    MemoryRegion *system = get_system_memory();

    memory_region_add_subregion(system, SN1_RAM_ADDRESS, node->ram);
    memory_region_init_alias(&node->hub_memory, OBJECT(node),
                             "sn1.hub.local-memory", node->ram, 0,
                             SN1_HUB_MEMORY_SIZE);
    memory_region_add_subregion(system, SN1_HUB_MEMORY_ADDRESS,
                                &node->hub_memory);

    memory_region_init(&node->hub_local_registers, OBJECT(node),
                       "sn1.hub.local-registers",
                       SN1_HUB_LOCAL_REGISTERS_SIZE);
    memory_region_add_subregion(system, SN1_HUB_LOCAL_REGISTERS_ADDRESS,
                                &node->hub_local_registers);

    memory_region_init_io(&node->board_id, OBJECT(node), &sn1_board_id_ops,
                          node, "sn1.board-id", 8);
    memory_region_add_subregion(&node->hub_local_registers,
                                SN1_BOARD_ID_OFFSET, &node->board_id);
    /* The remote form reaches the same response above the Bedrock window. */
    memory_region_init_alias(&node->board_id_remote, OBJECT(node),
                             "sn1.board-id.remote", &node->board_id, 0, 8);
    memory_region_add_subregion_overlap(
        system, SN1_HUB_REMOTE_REGISTERS_ADDRESS + SN1_BOARD_ID_OFFSET,
        &node->board_id_remote, 1);
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

static void sn1_node_create_cpus(SN1NodeState *node, MachineState *machine)
{
    MemoryRegion *system = get_system_memory();
    uint64_t prom_size = node->model_node->flash->size;
    unsigned int i;

    if (node->promdata_filter) {
        sn_promdata_poll_init(&node->promdata_poll, node->promdata,
                              SGI_SN1_NODE_MAX_CPUS, node->promdata_fast,
                              NULL, NULL);
    }

    for (i = 0; i < node->nr_cpus; i++) {
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
                             0, memory_region_size(node->ram), SN1_PROM_RESET,
                             prom_size, SN1_PROM_RAM_COPY,
                             SN1_PROM_RAM_COPY_SIZE,
                             &node->cpu_sysmem[i], 0,
                             &node->cpu_uncached[i],
                             &node->cpu_uncached_alias[i],
                             promdata_prefix);
        }
    }
}

static bool sn1_node_create_bedrock(SN1NodeState *node, MachineState *machine,
                                    Error **errp)
{
    Chardev *l1_console = serial_hd(0);
    MemoryRegion *system = get_system_memory();
    MemoryRegion *l1_uart;
    DeviceState *l1_uart_dev;
    MemoryRegion *hub_registers;
    SysBusDevice *bedrock;
    unsigned int cpu;
    size_t i;

    node->l1 = qdev_new(TYPE_SGI_L1);
    object_property_add_child(OBJECT(node), "l1", OBJECT(node->l1));
    if (l1_console) {
        qdev_prop_set_chr(node->l1, "chardev", l1_console);
    }
    sgi_machine_set_l1_identity_records(machine, node->l1);
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
    object_property_add_child(OBJECT(node), "bedrock", OBJECT(node->bedrock));
    qdev_prop_set_uint32(node->bedrock, "processor-count", node->nr_cpus);
    qdev_prop_set_uint32(node->bedrock, "processor-mask",
                        sn1_cpu_population_mask(node->model_node));
    /* Initial C grant is a model approximation. Silicon reset is unknown. */
    if (node->nr_cpus == 2 &&
        sn1_cpu_population_mask(node->model_node) == 0x5) {
        qdev_prop_set_uint32(node->bedrock, "cpu-enable-initial-mask", 0x4);
    }
    qdev_prop_set_uint8(node->bedrock, "revision", SN1_BEDROCK_REVISION);
    qdev_prop_set_uint8(node->bedrock, "local-region",
                        SN1_BEDROCK_LOCAL_REGION);
    qdev_prop_set_uint8(node->bedrock, "initiator-region",
                        SN1_BEDROCK_INITIATOR_REGION);
    /*
     * The shipped Origin 300 inventory installs two 256 MiB banks at
     * locational banks two and three, not the single dense bank zero the
     * compiled origin300 topology's node bank-mib still carries (its
     * 512 MiB total is right; its bank placement predates this fact).
     * Kept as an origin300-specific literal here, like this file's own
     * address-map constants above, until the catalogue's fixture is
     * corrected.
     */
    qdev_prop_set_uint8(node->bedrock, "installed-bank-mask",
                        SN1_INSTALLED_BANK_MASK);
    for (i = 0; i < SN1_ARCHITECTURAL_MEMORY_BANKS; i++) {
        g_autofree char *property = g_strdup_printf("installed-bank%zu-mib",
                                                     i);

        qdev_prop_set_uint16(node->bedrock, property,
                            sn1_installed_bank_mib[i]);
    }
    object_property_set_link(OBJECT(node->bedrock), "dma-memory",
                             OBJECT(system), &error_abort);
    object_property_set_link(OBJECT(node->bedrock), "node-memory",
                             OBJECT(node->ram), &error_abort);
    object_property_set_link(OBJECT(node->bedrock), "l1-uart",
                             OBJECT(l1_uart), &error_abort);
    /*
     * The register window above only carries the L1 UART's content; its
     * own receive/THRE interrupt output (hw/misc/sgi-l1.c) still needs its
     * own path to a consumer. Bedrock pending bank 0 bit 4 is that consumer
     * (docs/datasheets/bedrock-processor-interface.md), reached through the
     * composite's own "uart-irq" input, which broadcasts to both PI0 and
     * PI1 (hw/misc/sgi-bedrock.c).
     */
    qdev_connect_gpio_out_named(l1_uart_dev, "uart", 0,
                                qdev_get_gpio_in_named(node->bedrock,
                                                       "uart-irq", 0));

    /*
     * The XBridge is a fitted part of the IP45 node board itself, as the
     * L1 and the boot flash are (docs/designs/sn-machine-model-architecture.md,
     * "Node devices"): this file creates it directly, the family builder's
     * own device like every other product's onboard base I/O, against
     * Bedrock's own crosstalk egress region (sysbus_init_mmio() has
     * already given the not-yet-realized Bedrock device that region).
     * The IO-8's fixed devices are a separate sgi-baseio (the "io8"
     * preset) the machine places on the chip's own head F, through the
     * same board path every other product's base I/O goes through
     * (sgi_machine_instantiate_io_board() in sn-machine.c); this file
     * only exposes that head's PCI bus once the chip itself has
     * realized.
     */
    node->xbridge = qdev_new(TYPE_SGI_XBRIDGE);
    object_property_add_child(OBJECT(node), "xbridge", OBJECT(node->xbridge));
    qdev_prop_set_uint8(node->xbridge, "host-port", SN1_XBRIDGE_HOST_PORT);
    qdev_prop_set_string(node->xbridge, "board-part-f",
                         sgi_baseio_preset_lookup("io8")->board_part);
    qdev_prop_set_uint64(node->xbridge, "onewire-serial-f",
                         SGI_XBRIDGE_NIC_PLACEHOLDER_SERIAL);
    object_property_set_link(
        OBJECT(node->xbridge), "fabric",
        OBJECT(sysbus_mmio_get_region(SYS_BUS_DEVICE(node->bedrock), 2)),
        &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(node->xbridge), errp)) {
        return false;
    }
    /*
     * The head-F PCI bus, as a link property: the machine places the
     * IO-8's fixed devices on it once every node this run builds has
     * realized (sgi_machine_instantiate_io_board(), the same board path
     * every other product's base I/O goes through), not before.
     */
    object_property_add_const_link(
        OBJECT(node), "io8-pci-bus",
        OBJECT(sgi_xbridge_pci_bus(SGI_XBRIDGE(node->xbridge), 0xf)));

    /*
     * Every widget the crossbar decodes: the host's own local view (widget
     * zero) plus one per xtalk attachment, including a board slot with
     * nothing plugged in -- its head still answers "no PCI device here"
     * through the chip itself, which a widget left undecoded could not.
     */
    for (i = 0; i < node->xbridge_widget_count; i++) {
        g_autofree char *property =
            g_strdup_printf("widget-%u", node->xbridge_widgets[i]);

        object_property_set_link(
            OBJECT(node->bedrock), property,
            OBJECT(sgi_xbridge_widget_view(SGI_XBRIDGE(node->xbridge),
                                           node->xbridge_widgets[i])),
            &error_abort);
    }

    sysbus_realize(SYS_BUS_DEVICE(node->bedrock), &error_fatal);
    object_unref(OBJECT(node->bedrock));
    bedrock = SYS_BUS_DEVICE(node->bedrock);
    hub_registers = &SGI_BEDROCK(node->bedrock)->hub_special_registers;

    memory_region_add_subregion(system, SN1_XTALK_ADDRESS,
                                sysbus_mmio_get_region(bedrock, 1));
    memory_region_add_subregion_overlap(system, SN1_BEDROCK_LOCAL_ADDRESS,
                                        sysbus_mmio_get_region(bedrock, 0),
                                        1);
    memory_region_init_alias(&node->bedrock_node, OBJECT(node),
                             "sn1.bedrock.node0",
                             sysbus_mmio_get_region(bedrock, 0), 0,
                             SN1_BEDROCK_NODE_SIZE);
    memory_region_add_subregion_overlap(system, SN1_BEDROCK_NODE_ADDRESS,
                                        &node->bedrock_node, 1);
    /*
     * The local register window is a plain, non-overlapping partition of
     * the same three parts the remote window already answers below: a PI
     * head, the L1 UART's own dedicated region at 0x80
     * (docs/datasheets/l1-controller.md), and a PI tail. sMDK, the Origin
     * 300 field-diagnostic kernel, polls the UART's line status through
     * this local form under the stock PROM's console=d
     * (docs/datasheets/ip35-address-map.md); before this split, the whole
     * window aliased the general PI register file straight through, so
     * the same offset served a PI mask register instead and sMDK's console
     * write loop never saw the ready bit. SN1_HUB_REGISTER_HEAD_SIZE and
     * SN1_HUB_REGISTER_UART_END are the same constants the remote window's
     * own head/tail split below uses, so the two forms cannot drift apart.
     */
    memory_region_init_alias(&node->hub_local_register_head, OBJECT(node),
                             "sn1.hub.local-register-head",
                             hub_registers, 0,
                             SN1_HUB_REGISTER_HEAD_SIZE);
    memory_region_add_subregion(&node->hub_local_registers, 0,
                                &node->hub_local_register_head);
    memory_region_add_subregion(&node->hub_local_registers,
                                SN1_HUB_REGISTER_HEAD_SIZE,
                                sysbus_mmio_get_region(bedrock, 8));
    memory_region_init_alias(&node->hub_local_register_tail, OBJECT(node),
                             "sn1.hub.local-register-tail",
                             hub_registers,
                             SN1_HUB_REGISTER_UART_END,
                             SGI_BEDROCK_SIZE - SN1_HUB_REGISTER_UART_END);
    memory_region_add_subregion(&node->hub_local_registers,
                                SN1_HUB_REGISTER_UART_END,
                                &node->hub_local_register_tail);
    /* OpenBSD expects local PI accesses to select the requester's interface. */
    if (node->nr_cpus > 1 && sn1_cpu_slot(node->model_node, 1) == 2) {
        SGIBedrockState *bedrock_state = SGI_BEDROCK(node->bedrock);
        MemoryRegion *pi1 = sysbus_mmio_get_region(
            SYS_BUS_DEVICE(&bedrock_state->pi[1]), 0);

        memory_region_init_alias(&node->cpu_c_local_pi_xtalk, OBJECT(node),
                                 "sn1.cpu-c.local-pi-xtalk", pi1, 0,
                                 SGI_BEDROCK_PI_SIZE);
        memory_region_add_subregion_overlap(&node->cpu_sysmem[1],
                                            SN1_BEDROCK_LOCAL_ADDRESS,
                                            &node->cpu_c_local_pi_xtalk, 1);
        /*
         * CPU C's own local-window PI1 overlay is split the same way as
         * the machine-wide window above, but leaves the 0x80-0xbf gap
         * between its head and tail unclaimed instead of overlaying its
         * own UART alias there: with nothing at priority 1 over that one
         * range, CPU C's access falls through to the lower-priority
         * system alias underneath, which reaches the same machine-wide
         * container and its UART region above, the same as every other
         * processor.
         */
        memory_region_init_alias(&node->cpu_c_local_pi_hub_head, OBJECT(node),
                                 "sn1.cpu-c.local-pi-hub-head", pi1, 0,
                                 SN1_HUB_REGISTER_HEAD_SIZE);
        memory_region_add_subregion_overlap(&node->cpu_sysmem[1],
                                            SN1_HUB_LOCAL_REGISTERS_ADDRESS,
                                            &node->cpu_c_local_pi_hub_head, 1);
        memory_region_init_alias(&node->cpu_c_local_pi_hub_tail, OBJECT(node),
                                 "sn1.cpu-c.local-pi-hub-tail", pi1,
                                 SN1_HUB_REGISTER_UART_END,
                                 SGI_BEDROCK_PI_SIZE -
                                 SN1_HUB_REGISTER_UART_END);
        memory_region_add_subregion_overlap(
            &node->cpu_sysmem[1],
            SN1_HUB_LOCAL_REGISTERS_ADDRESS + SN1_HUB_REGISTER_UART_END,
            &node->cpu_c_local_pi_hub_tail, 1);
    }
    memory_region_add_subregion(system, SN1_HUB_REMOTE_REGISTERS_ADDRESS,
                                sysbus_mmio_get_region(bedrock, 3));
    /* Preserve the remote L1 UART decoder between the register aliases. */
    memory_region_init_alias(&node->hub_remote_register_head, OBJECT(node),
                             "sn1.hub.remote-register-head",
                             hub_registers, 0,
                             SN1_HUB_REGISTER_HEAD_SIZE);
    memory_region_add_subregion_overlap(
        system, SN1_HUB_REMOTE_REGISTERS_ADDRESS,
        &node->hub_remote_register_head, 1);
    memory_region_init_alias(&node->hub_remote_register_tail, OBJECT(node),
                             "sn1.hub.remote-register-tail",
                             hub_registers,
                             SN1_HUB_REGISTER_UART_END,
                             SGI_BEDROCK_SIZE - SN1_HUB_REGISTER_UART_END);
    memory_region_add_subregion_overlap(
        system, SN1_HUB_REMOTE_REGISTERS_ADDRESS + SN1_HUB_REGISTER_UART_END,
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
    memory_region_add_subregion_overlap(
        system, SN1_RAM_ADDRESS, sysbus_mmio_get_region(bedrock, 7), 0);

    for (cpu = 0; cpu < node->nr_cpus; cpu++) {
        MIPSCPU *mips_cpu = node->cpu[cpu];
        unsigned physical = sn1_cpu_slot(node->model_node, cpu);

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
    return true;
}

static void sn1_node_realize(DeviceState *dev, Error **errp)
{
    SN1NodeState *node = SN1_NODE(dev);
    MachineState *machine = MACHINE(qdev_get_machine());

    sn1_node_map_memory(node);
    sn1_node_create_cpus(node, machine);
    if (!sn1_node_create_bedrock(node, machine, errp)) {
        return;
    }
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

static void sn1_node_instance_finalize(Object *obj)
{
    SN1NodeState *node = SN1_NODE(obj);

    g_free(node->prom_views);
    g_free(node->xbridge_widgets);
}

static const Property sn1_node_properties[] = {
    DEFINE_PROP_LINK("ram", SN1NodeState, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT32("nr-cpus", SN1NodeState, nr_cpus, 0),
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
        .instance_finalize = sn1_node_instance_finalize,
        .class_init        = sn1_node_class_init,
    },
};

DEFINE_TYPES(sn1_node_types)
