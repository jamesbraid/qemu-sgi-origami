/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 300 ("SN1", IP35/Bedrock) node device. See sn1-node.h.
 *
 * The family builder (sn-machine.c) creates one of these from the format-2
 * descriptor's Bedrock node record, links its RAM and CPU clock, sets its
 * populated I/O board (if any) and realizes it; this file owns everything the
 * node needs to become a running Origin 300: the PROM/flash policy, the
 * node's own address-space views, the CPUs, Bedrock, L1 and the XBridge, a
 * fitted part of the IP45 node board itself. The Bedrock's crosstalk port and
 * the XBridge's port 0xa connect through the XBridge link
 * (hw/mips/sgi/xio-link.c) before either realizes. The IO-8's fixed devices
 * are a separate sgi-baseio (the "io8" preset, hw/mips/sgi/baseio.c) the
 * machine places on the chip's head F, through sn1_node_xbridge_pci_bus()
 * below.
 *
 * Node strides, window splits and unit offsets are Bedrock hardware facts,
 * not configuration, so the addresses below are constants here rather than
 * descriptor fields.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/host-utils.h"
#include "qemu/cutils.h"
#include "qemu/datadir.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
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
#include "hw/mips/sgi/xio-link.h"
#include "hw/mips/sgi-xio-port.h"
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
#include "system/cpus.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "cpu.h"
#include "cpu-qom.h"
#include "sn1-node.h"
#include "sn1-topology.h"

/*
 * The node's crosstalk/Hub-register address decode: identical on every Origin
 * 300 measured, so constants here rather than descriptor data repeated for a
 * node kind with exactly one shipped configuration.
 */
#define SN1_SPACE_HUB UINT64_C(0x10000000000)
#define SN1_HUB_LOCAL_BOOT (SN1_SPACE_HUB + UINT64_C(0x18000000))
#define SN1_HUB_LOCAL_HIGH_BOOT (SN1_SPACE_HUB + UINT64_C(0x1fc00000))
#define SN1_HUB_REMOTE_BOOT (SN1_SPACE_HUB + UINT64_C(0x38000000))
#define SN1_HUB_ECC (SN1_SPACE_HUB + UINT64_C(0x140000000))
#define SN1_HUB_ECC_SCRUB (SN1_SPACE_HUB + UINT64_C(0x100000000))
#define SN1_HUB_DIRECTORY (SN1_SPACE_HUB + UINT64_C(0x1c0000000))
#define SN1_PROM_RESET UINT64_C(0x1fc00000)
#define SN1_PROM_RAM_COPY UINT64_C(0x01a00000)
#define SN1_PROM_RAM_COPY_SIZE UINT64_C(0x00200000)
/*
 * The register window's blocks, the same in both forms: an unidentified block
 * below the L1 UART's 0x80 offset, the UART itself, then the per-processor
 * LEDs. See sn1_node_create_bedrock below.
 */
#define SN1_HUB_REGISTER_HEAD_SIZE UINT64_C(0x80)
#define SN1_HUB_REGISTER_UART_END (SN1_HUB_REGISTER_HEAD_SIZE + SGI_L1_SIZE)
/* One eight-byte LED register per processor slice, just above the UART. */
#define SN1_HUB_LED_OFFSET SN1_HUB_REGISTER_UART_END
#define SN1_HUB_LED_SIZE (SGI_SN1_NODE_MAX_CPUS * 8)
#define SN1_PROM_SECOND_SELECTION (SN1_SPACE_HUB + UINT64_C(0x1a000000))
#define SN1_PROM_SECOND_SELECTION_SIZE UINT64_C(0x800000)
#define SN1_HUB_REMOTE_FLASH (SN1_SPACE_HUB + UINT64_C(0x3a000000))

#define SN1_RAM_ADDRESS UINT64_C(0x0)
#define SN1_HUB_MEMORY_ADDRESS SN1_SPACE_HUB
#define SN1_HUB_MEMORY_SIZE UINT64_C(0x10000000)
#define SN1_HUB_LOCAL_REGISTERS_ADDRESS (SN1_SPACE_HUB + UINT64_C(0x10000000))
#define SN1_HUB_LOCAL_REGISTERS_SIZE UINT64_C(0x8000000)
#define SN1_IO_SPACE_ADDRESS UINT64_C(0x20000000000)
#define SN1_MEMORY_SPECIAL_ADDRESS UINT64_C(0x30000000000)
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
/*
 * Bedrock's architectural bank count; matches sn0.h's SN0_MAX_MEMORY_BANKS.
 */
#define SN1_ARCHITECTURAL_MEMORY_BANKS 8

/*
 * Bedrock's node field sits at physical address bit 33 in M mode; every
 * constant above is this node's view, node 0's. A node at a nonzero nasid
 * answers the same views offset by that many multiples of 1 << 33, the same
 * way sn0.c's node_stride shifts the Hub's windows by nasid multiples of its
 * M-mode 1 << 32. Node 0's addresses are therefore unchanged: (0 << 33) is
 * zero.
 */
#define SN1_NODE_ADDRESS_SHIFT 33

static uint64_t sn1_node_address(const SN1NodeState *node, uint64_t address)
{
    uint64_t node_offset;
    uint64_t shifted;

    g_assert(!umul64_overflow(UINT64_C(1) << SN1_NODE_ADDRESS_SHIFT,
                              node->nasid, &node_offset));
    g_assert(!uadd64_overflow(address, node_offset, &shifted));
    return shifted;
}

uint64_t sn1_node_canonical_address(const SN1NodeState *node,
                                    uint64_t address)
{
    return sn1_node_address(node, address);
}

/*
 * The claim layer: which node an access reaches.
 *
 * Every node's regions sit in the global map at its placement, nasid << 33 in
 * each space, where qtests and the machine address them. A processor does not
 * see that map directly. Its view (and the fabric view the Bedrock's DMA and
 * BTE use) hides each space behind a floor that answers a decode error, and
 * over it places one alias of each reachable node's window at the number that
 * node's local block carries: the node itself, and its cabled peer while the
 * link is up and the two numbers differ. A number nobody reachable carries is
 * the decode error. On top, a processor's view maps the reader-relative
 * forms, which always reach its node whatever its number: the reset view, the
 * Hub special local space (uncached alias, local registers and L1 UART, local
 * boot window), the Bedrock's register alias in crosstalk small window 1, and
 * the cached alias, sized from the processor interface's cached-alias
 * register.
 *
 * On one node with number 0 every alias lands where the global map already
 * is, so the one-chassis machine decodes as before.
 */
#define SN1_NODE_WINDOW_SIZE (UINT64_C(1) << SN1_NODE_ADDRESS_SHIFT)
#define SN1_NODE_SPACE_SIZE UINT64_C(0x10000000000)
/* Seven node bits in M mode, the only mode an Origin 300 runs. */
#define SN1_NODE_NUMBER_MASK 0x7f
#define SN1_HUB_LOCAL_FORMS_SIZE UINT64_C(0x20000000)
#define SN1_CACHED_ALIAS_MAX (64 * MiB)

static const uint64_t sn1_node_space_base[SN1_NODE_SPACES] = {
    0, SN1_SPACE_HUB, SN1_IO_SPACE_ADDRESS, SN1_MEMORY_SPECIAL_ADDRESS,
};

static MemTxResult sn1_claim_floor_read(void *opaque, hwaddr offset,
                                        uint64_t *value, unsigned size,
                                        MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR, "sn1: no reachable node carries number "
                  "%u (space offset 0x%" HWADDR_PRIx ")\n",
                  (unsigned)(offset >> SN1_NODE_ADDRESS_SHIFT), offset);
    *value = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult sn1_claim_floor_write(void *opaque, hwaddr offset,
                                         uint64_t value, unsigned size,
                                         MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR, "sn1: write to number %u reaches no "
                  "node (space offset 0x%" HWADDR_PRIx ")\n",
                  (unsigned)(offset >> SN1_NODE_ADDRESS_SHIFT), offset);
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps sn1_claim_floor_ops = {
    .read_with_attrs = sn1_claim_floor_read,
    .write_with_attrs = sn1_claim_floor_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static unsigned int sn1_node_number(SN1NodeState *node)
{
    return sgi_bedrock_node_number(SGI_BEDROCK(node->bedrock)) &
           SN1_NODE_NUMBER_MASK;
}

static SN1NodeState *sn1_node_peer(SN1NodeState *node)
{
    SGIBedrockState *bedrock = SGI_BEDROCK(node->bedrock);
    Object *peer;
    unsigned int index;

    if (!bedrock->net_cable ||
        !sgi_net_cable_neighbor(bedrock->net_cable, OBJECT(bedrock), 0,
                                &peer, &index)) {
        return NULL;
    }
    return (SN1NodeState *)object_dynamic_cast(peer->parent,
                                               TYPE_SGI_SN1_NODE);
}

SN1NodeState *sn1_node_claimant(SN1NodeState *node, unsigned int number)
{
    SN1NodeState *peer = sn1_node_peer(node);
    unsigned int own = sn1_node_number(node);

    if (number == own) {
        return node;
    }
    if (peer && peer->claims_ready && sn1_node_number(peer) == number) {
        return peer;
    }
    return NULL;
}

static uint64_t sn1_cached_alias_bytes(uint8_t code)
{
    return code ? 4 * KiB << (code - 1) : 0;
}

static void sn1_node_place_claims(SN1NodeState *node)
{
    SN1NodeState *peer = sn1_node_peer(node);
    unsigned int own = sn1_node_number(node);
    unsigned int other = peer ? sn1_node_number(peer) : 0;
    bool peer_reachable = peer && peer->claims_ready && other != own;
    unsigned int view, space;

    if (peer && peer->claims_ready && other == own) {
        qemu_log_mask(LOG_GUEST_ERROR, "sn1: node %u and its peer both carry "
                      "number %u; each reaches only itself there\n",
                      node->nasid, own);
    }
    for (view = 0; view < SN1_NODE_VIEWS; view++) {
        if (view < SN1_NODE_FABRIC_VIEW && view >= node->nr_cpus) {
            continue;
        }
        for (space = 0; space < SN1_NODE_SPACES; space++) {
            uint64_t base = sn1_node_space_base[space];
            MemoryRegion *mine = &node->claim[view][space][0];
            MemoryRegion *theirs = &node->claim[view][space][1];

            memory_region_set_address(
                mine, base + ((uint64_t)own << SN1_NODE_ADDRESS_SHIFT));
            memory_region_set_enabled(theirs, peer_reachable);
            if (peer_reachable) {
                memory_region_set_alias_offset(
                    theirs, sn1_node_address(peer, base));
                memory_region_set_address(
                    theirs,
                    base + ((uint64_t)other << SN1_NODE_ADDRESS_SHIFT));
            }
        }
    }
    for (view = 0; view < node->nr_cpus; view++) {
        unsigned physical = sn1_cpu_slot(node->model_node, view);
        SGIBedrockState *bedrock = SGI_BEDROCK(node->bedrock);
        unsigned pi = physical / (SGI_BEDROCK_PROCESSORS / 2);
        uint64_t size = sn1_cached_alias_bytes(bedrock->pi[pi].cached_alias);
        MemoryRegion *calias = &node->local_form[view][3];

        if (size) {
            memory_region_set_size(calias, size);
        }
        memory_region_set_enabled(calias, size != 0);
    }
}

void sn1_node_update_claims(SN1NodeState *node)
{
    SGIBedrockState *bedrock = SGI_BEDROCK(node->bedrock);
    SN1NodeState *peer;

    if (!node->claims_ready) {
        return;
    }
    if (bedrock->net_cable && !node->cable_notifier_added) {
        sgi_net_cable_add_change_notifier(bedrock->net_cable,
                                          &node->cable_notifier);
        node->cable_notifier_added = true;
    }
    memory_region_transaction_begin();
    sn1_node_place_claims(node);
    peer = sn1_node_peer(node);
    if (peer && peer->claims_ready) {
        sn1_node_place_claims(peer);
    }
    memory_region_transaction_commit();
}

/*
 * Give each processor the identity its references carry: its directory
 * pointer, the node's number above three device bits (MD_PTR_NODE_SHFT in
 * the public SN1 header), the device being 0 and 1 for interface 0's two
 * slots and 4 and 5 for interface 1's (MD_PTR_SUBNODE0_MASK and
 * MD_PTR_SUBNODE1_MASK). The number can change, so this runs again when
 * it does.
 */
static void sn1_node_set_requester_ids(SN1NodeState *node)
{
    for (unsigned int i = 0; i < node->nr_cpus; i++) {
        unsigned int slot = sn1_cpu_slot(node->model_node, i);

        if (node->cpu[i] && node->cpu[i]->env.r10k) {
            node->cpu[i]->env.r10k->requester_id =
                sn1_node_number(node) << 3 | (slot & 1) | (slot & 2) << 1;
        }
    }
}

static void sn1_node_number_changed(Notifier *notifier, void *data)
{
    SN1NodeState *node = container_of(notifier, SN1NodeState, node_notifier);

    sn1_node_set_requester_ids(node);
    sn1_node_update_claims(node);
}

static void sn1_node_cable_changed(Notifier *notifier, void *data)
{
    SN1NodeState *node = container_of(notifier, SN1NodeState,
                                      cable_notifier);

    /* A cable going down no longer names the peer; rebuild both ends. */
    sn1_node_update_claims(node);
    if (!sn1_node_peer(node)) {
        memory_region_transaction_begin();
        sn1_node_place_claims(node);
        memory_region_transaction_commit();
    }
}

static void sn1_node_view_claims(SN1NodeState *node, unsigned int view,
                                 MemoryRegion *container)
{
    MemoryRegion *system = get_system_memory();
    unsigned int space;

    for (space = 0; space < SN1_NODE_SPACES; space++) {
        uint64_t base = sn1_node_space_base[space];
        g_autofree char *floor_name =
            g_strdup_printf("sn1.node%u.view%u.floor%u", node->nasid, view,
                            space);
        unsigned int slot;

        memory_region_init_alias(&node->claim_floor_alias[view][space],
                                 OBJECT(node), floor_name,
                                 &node->claim_floor[space], 0,
                                 SN1_NODE_SPACE_SIZE);
        memory_region_add_subregion_overlap(
            container, base, &node->claim_floor_alias[view][space], -1);
        for (slot = 0; slot < SN1_NODE_CLAIMS; slot++) {
            g_autofree char *name =
                g_strdup_printf("sn1.node%u.view%u.claim%u.%s", node->nasid,
                                view, space, slot ? "peer" : "own");

            memory_region_init_alias(&node->claim[view][space][slot],
                                     OBJECT(node), name, system,
                                     sn1_node_address(node, base),
                                     SN1_NODE_WINDOW_SIZE);
            memory_region_set_enabled(&node->claim[view][space][slot],
                                      slot == 0);
            memory_region_add_subregion_overlap(
                container, sn1_node_address(node, base),
                &node->claim[view][space][slot], 0);
        }
    }
}

static void sn1_node_local_form(SN1NodeState *node, unsigned int cpu,
                                unsigned int form, const char *what,
                                uint64_t address, uint64_t size)
{
    g_autofree char *name = g_strdup_printf("sn1.node%u.cpu%u.local-%s",
                                            node->nasid, cpu, what);

    memory_region_init_alias(&node->local_form[cpu][form], OBJECT(node), name,
                             get_system_memory(),
                             sn1_node_address(node, address), size);
    memory_region_add_subregion_overlap(&node->cpu_sysmem[cpu], address,
                                        &node->local_form[cpu][form], 1);
}

/* The I/O interface a register packet or completion for a number reaches. */
static SGIBedrockIOState *sn1_node_io_claimant(void *opaque,
                                               unsigned int number)
{
    SN1NodeState *target = sn1_node_claimant(opaque, number);

    return target ? &SGI_BEDROCK(target->bedrock)->io : NULL;
}

static void sn1_node_cached_alias_changed(Notifier *notifier, void *data)
{
    SGIBedrockPIState *pi = data;
    SN1NodeState *node = container_of(notifier - pi->interface, SN1NodeState,
                                      cached_alias_notifier[0]);

    sn1_node_update_claims(node);
}

static void sn1_node_init_claims(SN1NodeState *node)
{
    unsigned int space, cpu, i;

    for (space = 0; space < SN1_NODE_SPACES; space++) {
        g_autofree char *name = g_strdup_printf("sn1.node%u.floor%u",
                                                node->nasid, space);

        memory_region_init_io(&node->claim_floor[space], OBJECT(node),
                              &sn1_claim_floor_ops, node, name,
                              SN1_NODE_SPACE_SIZE);
    }
    for (cpu = 0; cpu < node->nr_cpus; cpu++) {
        sn1_node_view_claims(node, cpu, &node->cpu_sysmem[cpu]);
        sn1_node_local_form(node, cpu, 0, "reset", SN1_PROM_RESET,
                            node->model_node->flash->size);
        sn1_node_local_form(node, cpu, 1, "hub-special", SN1_SPACE_HUB,
                            SN1_HUB_LOCAL_FORMS_SIZE);
        sn1_node_local_form(node, cpu, 2, "registers",
                            SN1_BEDROCK_LOCAL_ADDRESS, SN1_BEDROCK_NODE_SIZE);
        sn1_node_local_form(node, cpu, 3, "cached-alias", SN1_RAM_ADDRESS,
                            SN1_CACHED_ALIAS_MAX);
        memory_region_set_enabled(&node->local_form[cpu][3], false);
    }
    sn1_node_view_claims(node, SN1_NODE_FABRIC_VIEW, &node->fabric);
    node->node_notifier.notify = sn1_node_number_changed;
    node->cable_notifier.notify = sn1_node_cable_changed;
    sgi_bedrock_lb_add_node_notifier(&SGI_BEDROCK(node->bedrock)->lb,
                                     &node->node_notifier);
    sn1_node_set_requester_ids(node);
    sgi_bedrock_io_set_claimant(&SGI_BEDROCK(node->bedrock)->io,
                                sn1_node_io_claimant, node);
    for (i = 0; i < ARRAY_SIZE(node->cached_alias_notifier); i++) {
        node->cached_alias_notifier[i].notify = sn1_node_cached_alias_changed;
        sgi_bedrock_pi_add_cached_alias_notifier(
            &SGI_BEDROCK(node->bedrock)->pi[i],
            &node->cached_alias_notifier[i]);
    }
    node->claims_ready = true;
    sn1_node_update_claims(node);
}

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
 * processor's read view of its first selection, or NULL with no error when no
 * such drive was given (the caller then falls back to an immutable -bios
 * image). node_index selects the pflash unit the way sgi_machine_
 * load_flash's own per-node override does for the Hub path: unit N for node
 * N, so a second node (once one ships) takes its drive rather than colliding
 * with the first's.
 */
static MemoryRegion *sn1_node_create_boot_flash(SN1NodeState *node,
                                                unsigned int node_index,
                                                Error **errp)
{
    DriveInfo *dinfo = drive_get(IF_PFLASH, 0, node_index);
    DriveInfo *extra = node->last_node ?
        drive_get(IF_PFLASH, 0, node_index + 1) : NULL;
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
    qdev_prop_set_uint32(node->boot_flash, "node", node_index);
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
    sysbus_mmio_map(flash, 0, sn1_node_address(node, SN1_HUB_LOCAL_BOOT));
    sysbus_mmio_map(flash, 1, sn1_node_address(node, SN1_HUB_REMOTE_BOOT));
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
 * monitor's rewrite of the first sector is what the next reset fetches.
 * Without an image the machine keeps the immutable payload it had before.
 */
/*
 * The processor fetches from cacheable ROM devices, not MMIO handlers. The
 * flash's write callback refreshes their copies and invalidates old code. An
 * alias would pass writes through to the part's command interface.
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
 * The reset-latched processor mode field is 25 bits wide (bits 24:0 of
 * CP0_Config0), as on the R10000 family. sn-machine.c uses the same mask for
 * the Hub. Keep it here because this node's reset cannot share the Hub reset
 * order (see sn1_node_full_reset).
 */
#define SN1_PROCESSOR_MODE_MASK 0x01ffffff

void sn1_node_reset_cpus(SN1NodeState *node)
{
    uint32_t mode_bits =
        node->model_node->cpu_slots->value->assembly->mode_bits;
    unsigned int i;

    for (i = 0; i < node->nr_cpus; i++) {
        CPUState *cs = CPU(node->cpu[i]);
        CPUMIPSState *env = &node->cpu[i]->env;

        cpu_reset(cs);
        env->CP0_Config0 = (env->CP0_Config0 & ~(uint32_t)
                           SN1_PROCESSOR_MODE_MASK) |
                           (mode_bits & SN1_PROCESSOR_MODE_MASK);
    }
}

/*
 * Every CPU exists now. Release them back-to-back under BQL; generic vCPU
 * resume can still skew their first fetch, and this is not a claim about
 * physical reset-edge timing.
 */
void sn1_node_release_cpus(SN1NodeState *node)
{
    unsigned int i;

    if (node->boot_flash) {
        sn1_refresh_prom_views(node, true);
    }
    sn1_node_update_claims(node);
    for (i = 0; i < node->nr_cpus; i++) {
        CPUState *cs = CPU(node->cpu[i]);

        cs->halted = false;
        qemu_cpu_kick(cs);
    }
}

void sn1_node_full_reset(SN1NodeState *node, ResetType type)
{
    /*
     * cpu_reset() must run before qemu_devices_reset(): each CPU's
     * "start-powered-off" property re-halts it on every cpu_reset(), not only
     * at creation, so releasing them has to follow both, and
     * qemu_devices_reset() resetting Bedrock/L1/XBridge first would let a CPU
     * race a device that assumes it is not running yet. The Hub takes the
     * opposite order and releases through sgi_hub_set_cpu_enable instead,
     * which carries no such constraint. The release re-derives the claims,
     * which the local block's reset already did through its node-number
     * notifier; doing it again is harmless.
     */
    sn1_node_reset_cpus(node);
    qemu_devices_reset(type);
    sn1_node_release_cpus(node);
}

/*
 * Where each named flash view executes from: a Bedrock hardware fact (the
 * boot space it is wired to), not configuration. "local" and "remote" are
 * answered by the flash part's command decode (sn1_node_create_boot_flash)
 * once one is attached, so only "reset" and "local-high" are mapped as
 * separate SN1PromView aliases in that case; the immutable -bios path below
 * maps every named view here.
 */
/*
 * Every named view a Bedrock node's flash controller answers, in the order
 * the immutable -bios path maps them: fixed by the chip, not carried by the
 * compiled catalogue (see the header on sn1_node_attach_prom's use of this
 * table).
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
    g_autofree char *prom_name = NULL;
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
        /* Node 0 keeps the RAM names its snapshots carry. */
        node->prom_view_names[0] = node->nasid ?
            g_strdup_printf("sn1.node%u.prom.reset", node->nasid) :
            g_strdup("sn1.prom.reset");
        node->prom_view_names[1] = node->nasid ?
            g_strdup_printf("sn1.node%u.prom.local-high", node->nasid) :
            g_strdup("sn1.prom.local-high");
        if (!sn1_init_prom_view(&node->prom_view_reset, OBJECT(node),
                                node->prom_view_names[0], stored, prom_size,
                                errp) ||
            !sn1_init_prom_view(&node->prom_view_local_high, OBJECT(node),
                                node->prom_view_names[1], stored, prom_size,
                                errp)) {
            return false;
        }
        SGI_BEDROCK_FLASH(node->boot_flash)->programmed =
            sn1_prom_flash_programmed;
        SGI_BEDROCK_FLASH(node->boot_flash)->programmed_opaque = node;
        memory_region_add_subregion_overlap(
            system, sn1_node_address(node, SN1_PROM_RESET),
            &node->prom_view_reset.rom, 1);
        /*
         * The high local view sits inside a boot space the flash answers for,
         * so it is overlaid above it. The remote space is left to the flash:
         * its read decode already serves the same bytes at the same offsets,
         * and an overlay at its base would shadow the command addresses, so a
         * monitor could not drive the part through that space at all.
         */
        memory_region_add_subregion_overlap(
            system, sn1_node_address(node, SN1_HUB_LOCAL_HIGH_BOOT),
            &node->prom_view_local_high.rom, 1);
        return true;
    }

    /* Node 0 keeps the name its snapshots carry; later nodes add theirs. */
    prom_name = node->nasid ?
        g_strdup_printf("sn1.prom.node%u", node->nasid) : g_strdup("sn1.prom");

    if (!memory_region_init_ram_flags_nomigrate(&node->prom, OBJECT(node),
                                                prom_name, prom_size, 0,
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
     * addresses, are Bedrock hardware facts, not configuration: every view
     * the compiler ever emitted for a Bedrock node was already sized to the
     * flash chip's total size, so SN1_PROM_VIEW_NAMES names them directly
     * instead of reading a list the compiled catalogue no longer carries.
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
        memory_region_add_subregion_overlap(
            system, sn1_node_address(node, address),
            &node->prom_views[i], 1);
    }

    /* An empty second position answers erased through both boot spaces. */
    memory_region_init_io(&node->prom_flash, OBJECT(node),
                          &sn1_flash_erased_ops, node, "sn1.prom.flash",
                          SN1_PROM_SECOND_SELECTION_SIZE);
    memory_region_add_subregion(
        system, sn1_node_address(node, SN1_PROM_SECOND_SELECTION),
        &node->prom_flash);
    memory_region_init_alias(&node->prom_flash_remote, OBJECT(node),
                             "sn1.prom.flash-remote", &node->prom_flash, 0,
                             SN1_PROM_SECOND_SELECTION_SIZE);
    memory_region_add_subregion(
        system, sn1_node_address(node, SN1_HUB_REMOTE_FLASH),
        &node->prom_flash_remote);
    return true;
}

/*
 * The board-identification register both consumers read: OpenBSD through the
 * remote form and the stock firmware through the local one, each taking the
 * product code from bits 15 through 12. One stateless response serves both.
 * Only the aligned 64-bit read is answered; everything else is refused and
 * said out loud, because nothing establishes what the other bits or widths
 * do.
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

/*
 * The Hub-special register windows hold no processor-interface registers.
 * Below the L1 UART sits a block no public source names: the stock PROM
 * stores sixteen doublewords there through the remote form while it
 * initializes memory, and what the device does with them is not modelled.
 * Accept and ignore the stores, read zero, and log every access so a consumer
 * that depends on the block shows up.
 */
static MemTxResult sn1_hub_unidentified_read(void *opaque, hwaddr offset,
                                             uint64_t *value, unsigned size,
                                             MemTxAttrs attrs)
{
    SN1NodeState *node = opaque;
    SGIBedrockState *bedrock = SGI_BEDROCK(node->bedrock);

    *value = 0;
    if (!sgi_bedrock_hub_registers_permitted(bedrock)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sn1: Hub-special register window read at 0x%"
                      HWADDR_PRIx " denied for region %u\n", offset,
                      (unsigned)bedrock->initiator_region);
        return MEMTX_DECODE_ERROR;
    }
    qemu_log_mask(LOG_UNIMP, "sn1: unidentified Hub-special register "
                  "window block read of %u byte%s at offset 0x%"
                  HWADDR_PRIx " returns 0\n", size, size == 1 ? "" : "s",
                  offset);
    return MEMTX_OK;
}

static MemTxResult sn1_hub_unidentified_write(void *opaque, hwaddr offset,
                                              uint64_t value, unsigned size,
                                              MemTxAttrs attrs)
{
    SN1NodeState *node = opaque;
    SGIBedrockState *bedrock = SGI_BEDROCK(node->bedrock);

    if (!sgi_bedrock_hub_registers_permitted(bedrock)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sn1: Hub-special register window write at 0x%"
                      HWADDR_PRIx " denied for region %u\n", offset,
                      (unsigned)bedrock->initiator_region);
        return MEMTX_DECODE_ERROR;
    }
    qemu_log_mask(LOG_UNIMP, "sn1: unidentified Hub-special register "
                  "window block ignores a %u-byte write of 0x%" PRIx64
                  " at offset 0x%" HWADDR_PRIx "\n", size, value, offset);
    return MEMTX_OK;
}

static const MemoryRegionOps sn1_hub_unidentified_ops = {
    .read_with_attrs = sn1_hub_unidentified_read,
    .write_with_attrs = sn1_hub_unidentified_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/*
 * One LED register per processor slice at 0xc0 + 8 * slice of either register
 * window: the public SN1 kernel's heartbeat LED, the stock PROM's progress
 * code, and IRIX's heartbeat. The LED bank is eight bits wide, so a store
 * keeps bits 7:0. What a load returns on hardware is unknown; the model
 * returns the kept byte. Only the aligned eight-byte access every observed
 * consumer makes is answered.
 */
static MemTxResult sn1_hub_led_check(SN1NodeState *node, hwaddr offset,
                                     unsigned size, const char *access)
{
    SGIBedrockState *bedrock = SGI_BEDROCK(node->bedrock);

    if (!sgi_bedrock_hub_registers_permitted(bedrock)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sn1: processor LED %s denied for region %u\n",
                      access, (unsigned)bedrock->initiator_region);
        return MEMTX_DECODE_ERROR;
    }
    if ((offset & 7) || size != 8) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sn1: processor LEDs answer only an aligned eight-byte "
                      "access; refusing a %u-byte %s at offset 0x%"
                      HWADDR_PRIx "\n", size, access, offset);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult sn1_hub_led_read(void *opaque, hwaddr offset,
                                    uint64_t *value, unsigned size,
                                    MemTxAttrs attrs)
{
    SN1NodeState *node = opaque;
    MemTxResult result = sn1_hub_led_check(node, offset, size, "read");

    *value = result == MEMTX_OK ? node->led[offset / 8] : 0;
    return result;
}

static MemTxResult sn1_hub_led_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size,
                                     MemTxAttrs attrs)
{
    SN1NodeState *node = opaque;
    MemTxResult result = sn1_hub_led_check(node, offset, size, "write");

    if (result == MEMTX_OK) {
        node->led[offset / 8] = value;
    }
    return result;
}

static const MemoryRegionOps sn1_hub_led_ops = {
    .read_with_attrs = sn1_hub_led_read,
    .write_with_attrs = sn1_hub_led_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void sn1_node_map_memory(SN1NodeState *node)
{
    MemoryRegion *system = get_system_memory();

    memory_region_add_subregion(system, sn1_node_address(node, SN1_RAM_ADDRESS),
                                node->ram);
    memory_region_init_alias(&node->hub_memory, OBJECT(node),
                             "sn1.hub.local-memory", node->ram, 0,
                             SN1_HUB_MEMORY_SIZE);
    memory_region_add_subregion(
        system, sn1_node_address(node, SN1_HUB_MEMORY_ADDRESS),
        &node->hub_memory);

    memory_region_init(&node->hub_local_registers, OBJECT(node),
                       "sn1.hub.local-registers",
                       SN1_HUB_LOCAL_REGISTERS_SIZE);
    memory_region_add_subregion(
        system, sn1_node_address(node, SN1_HUB_LOCAL_REGISTERS_ADDRESS),
        &node->hub_local_registers);

    memory_region_init_io(&node->board_id, OBJECT(node), &sn1_board_id_ops,
                          node, "sn1.board-id", 8);
    memory_region_add_subregion(&node->hub_local_registers,
                                SN1_BOARD_ID_OFFSET, &node->board_id);
    /* The remote form reaches the same response above the Bedrock window. */
    memory_region_init_alias(&node->board_id_remote, OBJECT(node),
                             "sn1.board-id.remote", &node->board_id, 0, 8);
    memory_region_add_subregion_overlap(
        system,
        sn1_node_address(node, SN1_HUB_REMOTE_REGISTERS_ADDRESS +
                                SN1_BOARD_ID_OFFSET),
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


/*
 * A processor's secondary-cache instruction or prefetch, dispatched to the
 * node's system-request region (R10K_SYSTEM_REQUEST_* in target/mips/r10k.h)
 * and passed to the memory unit of the node whose memory it names. An index
 * instruction names a set rather than a line, so it goes to this node and
 * its one cabled peer, all an Origin 300 pair has; a line homed on any
 * other node would be missed. It runs outside any I/O access, so it takes
 * the lock itself, and only once a memory unit is watching anything.
 */
static MemTxResult sn1_node_system_request(void *opaque, hwaddr offset,
                                           uint64_t *value, unsigned size,
                                           MemTxAttrs attrs)
{
    static const SGIBedrockMDProcessorRequest md_request[] = {
        [R10K_REQUEST_INDEX_WRITEBACK_INVALIDATE] =
            SGI_BEDROCK_MD_INDEX_WRITEBACK_INVALIDATE,
        [R10K_REQUEST_HIT_WRITEBACK_INVALIDATE] =
            SGI_BEDROCK_MD_HIT_WRITEBACK_INVALIDATE,
        [R10K_REQUEST_HIT_INVALIDATE] = SGI_BEDROCK_MD_HIT_INVALIDATE,
        [R10K_REQUEST_PREFETCH_LOAD] = SGI_BEDROCK_MD_PREFETCH_LOAD,
        [R10K_REQUEST_PREFETCH_STORE] = SGI_BEDROCK_MD_PREFETCH_STORE,
    };
    SN1NodeState *node = opaque;
    SN1NodeState *target[2] = { NULL, NULL };
    uint64_t paddr = extract64(offset, 0, R10K_SYSTEM_REQUEST_PADDR_BITS);
    unsigned int request = extract64(offset, R10K_SYSTEM_REQUEST_KIND_SHIFT,
                                     4);
    unsigned int set_bits = extract64(offset,
                                      R10K_SYSTEM_REQUEST_SET_BITS_SHIFT, 6);
    uint64_t index_mask = ((UINT64_C(1) << set_bits) - 1) * 128;
    uint64_t address = paddr & (SGI_BEDROCK_MD_MEMORY_SIZE - 1);
    bool found = false;

    *value = 0;
    if (request >= ARRAY_SIZE(md_request)) {
        return MEMTX_OK;
    }
    /*
     * A cache instruction names the line at its address, on that line's
     * home node, so a later read of the line's word watches its page. A
     * prefetch names nothing: copy loops issue them by the thousand.
     */
    if (request != R10K_REQUEST_PREFETCH_LOAD &&
        request != R10K_REQUEST_PREFETCH_STORE) {
        SN1NodeState *home = sn1_node_claimant(node, paddr >> 33);

        if (home) {
            sgi_bedrock_md_name_line(&SGI_BEDROCK(home->bedrock)->md,
                                     address);
        }
    }
    if (request == R10K_REQUEST_INDEX_WRITEBACK_INVALIDATE) {
        target[0] = node;
        target[1] = sn1_node_peer(node);
    } else {
        target[0] = sn1_node_claimant(node, paddr >> 33);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(target); i++) {
        SGIBedrockMDState *md;

        if (!target[i]) {
            continue;
        }
        md = &SGI_BEDROCK(target[i]->bedrock)->md;
        if (!sgi_bedrock_md_following(md)) {
            continue;
        }
        BQL_LOCK_GUARD();
        found |= sgi_bedrock_md_processor_request(md, attrs.requester_id,
                                                  md_request[request],
                                                  address, index_mask);
    }
    *value = found;
    return MEMTX_OK;
}

static const MemoryRegionOps sn1_node_system_request_ops = {
    .read_with_attrs = sn1_node_system_request,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

/*
 * Whether the processor a directory pointer names is fitted: the node that
 * number reaches from this one carries a processor in that slot. Device
 * bit 1 names an I/O owner (MD_PTR_OWNER_IO in the public SN1 header).
 */
static bool sn1_node_pointer_present(void *opaque, uint16_t pointer)
{
    SN1NodeState *owner = sn1_node_claimant(opaque, pointer >> 3);
    unsigned int slot = (pointer & 1) | ((pointer >> 2) & 1) << 1;

    if (!owner || (pointer & 2)) {
        return false;
    }
    for (unsigned int i = 0; i < owner->nr_cpus; i++) {
        if (sn1_cpu_slot(owner->model_node, i) == slot) {
            return true;
        }
    }
    return false;
}

static void sn1_node_create_cpus(SN1NodeState *node, MachineState *machine)
{
    MemoryRegion *system = get_system_memory();
    uint64_t prom_size = node->model_node->flash->size;
    unsigned int i;

    if (node->promdata_filter) {
        sn_promdata_poll_init(&node->promdata_poll, node->promdata,
                              SGI_SN1_NODE_MAX_CPUS, NULL, NULL);
    }

    memory_region_init_io(&node->system_requests, OBJECT(node),
                          &sn1_node_system_request_ops, node,
                          "sn1.system-requests", R10K_SYSTEM_REQUEST_SIZE);
    /*
     * Processors dispatch requests here from their own threads without
     * the BQL. The reentrancy guard is one unlocked flag shared by every
     * region the node owns, so it would take a request in flight for
     * reentry: another processor's read of the board ID then fails as a
     * bus error, and a second concurrent request is dropped. Nothing here
     * dispatches back into the node, and the handler takes the BQL itself
     * before touching the memory unit's state.
     */
    node->system_requests.disable_reentrancy_guard = true;
    for (i = 0; i < node->nr_cpus; i++) {
        g_autofree char *name = g_strdup_printf("cpu%u", i);
        g_autofree char *alias_name =
            g_strdup_printf("sn1.node%u.cpu%u.system", node->nasid, i);
        g_autofree char *sysmem_name =
            g_strdup_printf("sn1.node%u.cpu%u.memory", node->nasid, i);
        DeviceState *cpu_dev = qdev_new(machine->cpu_type);
        MIPSCPU *cpu = MIPS_CPU(cpu_dev);

        /*
         * Every existing address still resolves through this alias exactly as
         * it did through the shared system region directly; only the private
         * window below adds anything on top, at higher priority.
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
        if (cpu->env.r10k) {
            cpu->env.r10k->system_requests = &node->system_requests;
        }
        cpu_mips_irq_init_cpu(cpu);
        cpu_mips_clock_init(cpu);
        node->cpu[i] = cpu;

        if (node->promdata_filter) {
            g_autofree char *promdata_prefix =
                g_strdup_printf("sn1.node%u.cpu%u", node->nasid, i);

            /*
             * The IP35 PROM's RAM copy executes within one 2 MiB TLB span.
             * The phase forwards to and aliases the node's memory, at its
             * placement in the global map: a second node's is not at zero.
             * The window runs before any node number is assigned, so a
             * processor names its memory with node field zero then.
             */
            sn_promdata_init(&node->promdata[i], OBJECT(node), cpu,
                             &node->promdata_poll, node->promdata_fast,
                             sn1_node_address(node, SN1_RAM_ADDRESS),
                             memory_region_size(node->ram), SN1_PROM_RESET,
                             prom_size, SN1_PROM_RAM_COPY,
                             SN1_PROM_RAM_COPY_SIZE,
                             &node->cpu_sysmem[i], 0,
                             &node->cpu_uncached[i],
                             &node->cpu_uncached_alias[i],
                             promdata_prefix);
        }
    }
}

/*
 * Ask for the reset and stop every processor, from queued work on the
 * processor that asked for it -- the work runs at its next translation-block
 * boundary, outside the execution loop, where a processor may be stopped;
 * sgi_hub_request_machine_reset() in hw/misc/sgi-hub.c says why stopping
 * them there matters.
 */
static void sn1_node_request_reset(CPUState *cs, run_on_cpu_data data)
{
    CPUState *cpu;

    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    CPU_FOREACH(cpu) {
        cpu_pause(cpu);
    }
}

/*
 * A Bedrock local reset restarts the node's processors at the reset
 * vector, so the firmware runs again. No register description says so;
 * every consumer that reboots relies on it and never jumps to the firmware
 * itself. The stock IP35 PROM prints "Resetting the system...", writes the
 * network interface's reset enable and then local plus port reset, and
 * branches back into a loop; IRIX 6.5.30's restart path repeats the same
 * two writes every 3 s; OpenBSD resets its IP35 nodes with local plus link
 * reset. docs/datasheets/bedrock-network-interface.md carries the evidence.
 *
 * The model restarts the whole machine, as the Origin 200's Hub local reset
 * and the L1's reset command already do: the node's I/O, its L1 and, on a
 * cabled pair, the other node start again with it. What else a local reset
 * reaches on hardware is not established. IRIX and OpenBSD write every node,
 * so restarting them all matches what they ask for; the PROM's own reset
 * writes only its node, and the PROM clears the peer's reset enable after
 * discovery, so on a cabled pair restarting the peer may over-reach.
 */
static void sn1_node_bedrock_reset(void *opaque, int n, int level)
{
    if (!level) {
        return;
    }
    if (current_cpu) {
        async_run_on_cpu(current_cpu, sn1_node_request_reset, RUN_ON_CPU_NULL);
    } else {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static bool sn1_node_create_bedrock(SN1NodeState *node, MachineState *machine,
                                    Error **errp)
{
    /* The builder numbers nodes in order, so nasid is this node's index. */
    Chardev *l1_console = serial_hd(sgi_sn1_serial_line(node->nasid, 0));
    MemoryRegion *system = get_system_memory();
    MemoryRegion *l1_uart;
    DeviceState *l1_uart_dev;
    SysBusDevice *bedrock;
    unsigned int cpu;
    uint32_t slot_order;
    size_t i;

    node->l1 = qdev_new(TYPE_SGI_L1);
    object_property_add_child(OBJECT(node), "l1", OBJECT(node->l1));
    if (l1_console) {
        qdev_prop_set_chr(node->l1, "chardev", l1_console);
    }
    if (!sgi_machine_set_l1_identity_records(machine, node->l1, node->nasid,
                                             errp)) {
        return false;
    }
    /*
     * The chassis's brick slot, the operator's setting the L1 keeps in its
     * non-volatile memory (a two-chassis slave carries 02); zero leaves the
     * controller's default.
     */
    if (node->l1_bay) {
        qdev_prop_set_uint32(node->l1, "bay", node->l1_bay);
    }
    /* The L1 revision Origin 300 systems report alongside PROM 6.210. */
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
    /*
     * The Bedrock tells its processors apart by QEMU CPU index; a second
     * node's processors are numbered after the first node's.
     */
    if (node->nr_cpus) {
        qdev_prop_set_uint32(node->bedrock, "cpu-base",
                             CPU(node->cpu[0])->cpu_index);
    }
    qdev_prop_set_uint32(node->bedrock, "processor-mask",
                        sn1_cpu_population_mask(node->model_node,
                                               node->nr_cpus));
    /*
     * Which slot each processor is in, two bits apiece. The mask above says
     * which slots are fitted; with all four fitted it no longer says which
     * processor went where, and the board's population order (one processor
     * per interface before a second on either) is not ascending slot order.
     * Without this the processor in interface 0's second slot read its CPU
     * number as interface 0's first slot and ran the stock PROM's boot-master
     * path beside the real first processor.
     */
    slot_order = 0;
    for (cpu = 0; cpu < node->nr_cpus; cpu++) {
        slot_order |= sn1_cpu_slot(node->model_node, cpu) << (2 * cpu);
    }
    qdev_prop_set_uint32(node->bedrock, "processor-slot-order", slot_order);
    /*
     * Every fitted slot comes out of reset with its bus grant. This is a
     * model approximation; the silicon's reset value is unknown. What the
     * stock PROM does with these registers is measured: on its boot path it
     * only ever clears one, for a partner that fails its early liveness
     * check, and never sets one (only an operator's enable command in its
     * monitor does), so a fitted processor it counts must already be granted.
     * The store loop once read as the boot master granting interface 0 writes
     * the Hub-special register window, not these registers, so without this
     * the BaseIO PROM finds only the processors on interface 1.
     */
    if (node->nr_cpus > 1) {
        uint32_t grant = 0;

        for (cpu = 0; cpu < node->nr_cpus; cpu++) {
            grant |= 1U << sn1_cpu_slot(node->model_node, cpu);
        }
        qdev_prop_set_uint32(node->bedrock, "cpu-enable-initial-mask", grant);
    }
    qdev_prop_set_uint8(node->bedrock, "revision", SN1_BEDROCK_REVISION);
    qdev_prop_set_uint8(node->bedrock, "local-region",
                        SN1_BEDROCK_LOCAL_REGION);
    qdev_prop_set_uint8(node->bedrock, "initiator-region",
                        SN1_BEDROCK_INITIATOR_REGION);
    /*
     * The shipped Origin 300 inventory installs two 256 MiB banks at
     * locational banks two and three; the compiled origin300 topology's node
     * bank-mib now carries that placement, since bank placement is
     * fitted-part data and belongs in the descriptor, not a literal here.
     * installed-bank-mask stays a literal: which banks are wired for the one
     * shipped configuration is not a fact the catalogue carries on its, only
     * recoverable from bank-mib's zero entries, so restating it directly is
     * clearer than deriving it.
     */
    qdev_prop_set_uint8(node->bedrock, "installed-bank-mask",
                        SN1_INSTALLED_BANK_MASK);
    {
        const uint64List *bank_entry = node->model_node->bank_mib;

        for (i = 0; i < SN1_ARCHITECTURAL_MEMORY_BANKS; i++) {
            g_autofree char *property =
                g_strdup_printf("installed-bank%zu-mib", i);

            qdev_prop_set_uint16(node->bedrock, property, bank_entry->value);
            bank_entry = bank_entry->next;
        }
    }
    /*
     * The Bedrock's DMA and BTE reach nodes by number, as a processor does,
     * but without the reader-relative forms: the fabric view.
     */
    memory_region_init(&node->fabric, OBJECT(node), "sn1.node.fabric",
                       UINT64_MAX);
    memory_region_init_alias(&node->fabric_sysmem, OBJECT(node),
                             "sn1.node.fabric.system", system, 0, UINT64_MAX);
    memory_region_add_subregion_overlap(&node->fabric, 0,
                                        &node->fabric_sysmem, -2);
    object_property_set_link(OBJECT(node->bedrock), "dma-memory",
                             OBJECT(&node->fabric), &error_abort);
    object_property_set_link(OBJECT(node->bedrock), "node-memory",
                             OBJECT(node->ram), &error_abort);
    object_property_set_link(OBJECT(node->bedrock), "l1-uart",
                             OBJECT(l1_uart), &error_abort);
    /*
     * The register window above only carries the L1 UART's content; its
     * receive/THRE interrupt output (hw/misc/sgi-l1.c) still needs its path
     * to a consumer. Bedrock pending bank 0 bit 4 is that consumer, reached
     * through the composite's "uart-irq" input, which broadcasts to both PI0
     * and PI1 (hw/misc/sgi-bedrock.c).
     */
    qdev_connect_gpio_out_named(l1_uart_dev, "uart", 0,
                                qdev_get_gpio_in_named(node->bedrock,
                                                       "uart-irq", 0));
    qdev_connect_gpio_out_named(node->bedrock, "node-reset", 0,
                                qdev_get_gpio_in_named(DEVICE(node),
                                                       "bedrock-reset", 0));

    /*
     * The XBridge is a fitted part of the IP45 node board itself, as the L1
     * and the boot flash are: this file creates it directly, the family
     * builder's device like every other product's onboard base I/O. The
     * crosstalk port experiment's link pattern joins it to the Bedrock's
     * crosstalk port before either realizes: the XBridge link
     * (hw/mips/sgi/xio-link.c) installs the Bedrock's packet face as the
     * chip's "fabric": what its widget-0 conditions and both heads'
     * interrupts reach the host through. The IO-8's fixed devices are a
     * separate sgi-baseio (the "io8" preset) the machine places on the chip's
     * own head F, through the same board path every other product's base I/O
     * goes through (sgi_machine_instantiate_io_board() in sn-machine.c, via
     * sn1_node_xbridge_pci_bus() below) once the chip itself has realized.
     */
    node->xbridge = qdev_new(TYPE_SGI_XBRIDGE);
    object_property_add_child(OBJECT(node), "xbridge", OBJECT(node->xbridge));
    qdev_prop_set_uint8(node->xbridge, "host-port", SN1_XBRIDGE_HOST_PORT);
    /*
     * The first node's buses keep the names they have always had; a second
     * node's carry its index, since PCI devices on same-named buses collide
     * in the migration stream.
     */
    if (node->nasid) {
        g_autofree char *prefix = g_strdup_printf("node%u-", node->nasid);

        qdev_prop_set_string(node->xbridge, "bus-prefix", prefix);
    }
    qdev_prop_set_string(node->xbridge, "board-part-f",
                         sgi_baseio_preset_lookup("io8")->board_part);
    qdev_prop_set_uint64(node->xbridge, "onewire-serial-f",
                         SGI_XBRIDGE_NIC_PLACEHOLDER_SERIAL);
    if (!sgi_xio_xbridge_link_connect(
            OBJECT(&SGI_BEDROCK(node->bedrock)->io), node->xbridge, errp)) {
        return false;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(node->xbridge), errp)) {
        return false;
    }

    /*
     * Every widget the crossbar decodes: the host's local view (widget zero)
     * plus one per xtalk attachment, including a board slot with nothing
     * plugged in; its head still answers "no PCI device here" through the
     * chip itself, which a widget left undecoded could not. Read through the
     * crosstalk port interface, as any other widget attachment in this tree
     * does, rather than the chip's view accessor directly.
     */
    for (i = 0; i < node->xbridge_widget_count; i++) {
        g_autofree char *property =
            g_strdup_printf("widget-%u", node->xbridge_widgets[i]);
        MemoryRegion *face = sgi_xio_port_cpu_face(OBJECT(node->xbridge),
                                                   node->xbridge_widgets[i]);

        if (!face) {
            error_setg(errp, "XBridge widget %u has no XIO face",
                       node->xbridge_widgets[i]);
            return false;
        }
        object_property_set_link(OBJECT(node->bedrock), property,
                                 OBJECT(face), &error_abort);
    }
    /*
     * A widget scan addresses the board by the physical crosstalk port
     * Bedrock actually attached it to (SN1_XBRIDGE_HOST_PORT), not only by
     * the widget-zero local-CPU shortcut wired above; both name the same
     * chip, seen through its single upstream link either way. Without this, a
     * probe of Bedrock's port SN1_XBRIDGE_HOST_PORT decodes to nothing
     * (sgi-bedrock-io.c's widget_as_initialized stays false there), so
     * firmware and diagnostics that walk Bedrock's ports rather than using
     * the shortcut never learn the IO-8 is present.
     */
    {
        g_autofree char *host_port_property =
            g_strdup_printf("widget-%u", SN1_XBRIDGE_HOST_PORT);

        object_property_set_link(
            OBJECT(node->bedrock), host_port_property,
            OBJECT(sgi_xbridge_widget_view(SGI_XBRIDGE(node->xbridge), 0)),
            &error_abort);
    }

    sysbus_realize(SYS_BUS_DEVICE(node->bedrock), &error_fatal);
    object_unref(OBJECT(node->bedrock));
    bedrock = SYS_BUS_DEVICE(node->bedrock);
    SGI_BEDROCK(node->bedrock)->md.pointer_present = sn1_node_pointer_present;
    SGI_BEDROCK(node->bedrock)->md.requester_opaque = node;

    memory_region_add_subregion(
        system, sn1_node_address(node, SN1_IO_SPACE_ADDRESS),
        sysbus_mmio_get_region(bedrock, 1));
    memory_region_add_subregion_overlap(
        system, sn1_node_address(node, SN1_BEDROCK_LOCAL_ADDRESS),
        sysbus_mmio_get_region(bedrock, 0), 1);
    memory_region_init_alias(&node->bedrock_node, OBJECT(node),
                             "sn1.bedrock.node0",
                             sysbus_mmio_get_region(bedrock, 0), 0,
                             SN1_BEDROCK_NODE_SIZE);
    memory_region_add_subregion_overlap(
        system, sn1_node_address(node, SN1_BEDROCK_NODE_ADDRESS),
        &node->bedrock_node, 1);
    /*
     * Both register-window forms hold the same three blocks around the
     * board-identification register: the unidentified block below 0x80, the
     * L1 UART's region at 0x80, and the per-processor LEDs at 0xc0. The UART
     * is the one sMDK, the Origin 300 field-diagnostic kernel, polls through
     * the local form under the stock PROM's console=d. Nothing in either
     * window reaches the processor-interface registers: that is the crosstalk
     * form's job alone, and the LED offsets used to land on PI interrupt mask
     * 1 B and the cross-call- pending registers when it did. Every other
     * offset is left undecoded.
     */
    memory_region_init_io(&node->hub_unidentified, OBJECT(node),
                          &sn1_hub_unidentified_ops, node,
                          "sn1.hub.unidentified", SN1_HUB_REGISTER_HEAD_SIZE);
    memory_region_add_subregion(&node->hub_local_registers, 0,
                                &node->hub_unidentified);
    memory_region_add_subregion(&node->hub_local_registers,
                                SN1_HUB_REGISTER_HEAD_SIZE,
                                sysbus_mmio_get_region(bedrock, 8));
    memory_region_init_io(&node->hub_leds, OBJECT(node), &sn1_hub_led_ops,
                          node, "sn1.hub.leds", SN1_HUB_LED_SIZE);
    memory_region_add_subregion(&node->hub_local_registers,
                                SN1_HUB_LED_OFFSET, &node->hub_leds);
    /*
     * OpenBSD expects local PI accesses to select the requester's interface.
     * Every realized processor whose physical slot sits on processor
     * interface 1 needs its PI1 overlay, not only a logical CPU 1 sitting
     * there: the two-processor population fits one (C, at logical index 1 in
     * population order; see sn1_cpu_slot's comment), the four-processor
     * population fits two (C and D, logical indices 1 and 3).
     */
    for (cpu = 0; cpu < node->nr_cpus; cpu++) {
        unsigned physical = sn1_cpu_slot(node->model_node, cpu);
        SGIBedrockState *bedrock_state;
        MemoryRegion *pi1;

        /*
         * Bedrock's two processor interfaces split the physical slot
         * numbering into two halves of SGI_BEDROCK_PROCESSORS / 2 each
         * (sgi-bedrock.c's own (mask >> (2 * i)) & 3 unpacking uses the same
         * split); interface 0's slots need no overlay; the machine-wide PI0
         * window below already serves them.
         */
        if (physical < SGI_BEDROCK_PROCESSORS / 2) {
            continue;
        }
        bedrock_state = SGI_BEDROCK(node->bedrock);
        pi1 = sysbus_mmio_get_region(SYS_BUS_DEVICE(&bedrock_state->pi[1]), 0);

        memory_region_init_alias(&node->cpu_local_pi_xio[cpu], OBJECT(node),
                                 "sn1.cpu.local-pi-xio", pi1, 0,
                                 SGI_BEDROCK_PI_SIZE);
        /* Above the claim layer's reader-local register form (priority 1). */
        memory_region_add_subregion_overlap(&node->cpu_sysmem[cpu],
                                            SN1_BEDROCK_LOCAL_ADDRESS,
                                            &node->cpu_local_pi_xio[cpu], 2);
    }
    memory_region_add_subregion(
        system, sn1_node_address(node, SN1_HUB_REMOTE_REGISTERS_ADDRESS),
        sysbus_mmio_get_region(bedrock, 3));
    /* The remote form keeps its L1 UART decoder between the two. */
    memory_region_init_alias(&node->hub_unidentified_remote, OBJECT(node),
                             "sn1.hub.unidentified-remote",
                             &node->hub_unidentified, 0,
                             SN1_HUB_REGISTER_HEAD_SIZE);
    memory_region_add_subregion_overlap(
        system, sn1_node_address(node, SN1_HUB_REMOTE_REGISTERS_ADDRESS),
        &node->hub_unidentified_remote, 1);
    memory_region_init_alias(&node->hub_leds_remote, OBJECT(node),
                             "sn1.hub.leds-remote", &node->hub_leds, 0,
                             SN1_HUB_LED_SIZE);
    memory_region_add_subregion_overlap(
        system,
        sn1_node_address(node, SN1_HUB_REMOTE_REGISTERS_ADDRESS +
                              SN1_HUB_LED_OFFSET),
        &node->hub_leds_remote, 1);
    /*
     * The directory back door is the last part of the node's address space,
     * one word per fitted memory line. Firmware initialises it before it
     * trusts memory, so nothing boots without it.
     */
    memory_region_add_subregion(
        system, sn1_node_address(node, SN1_HUB_DIRECTORY),
        sysbus_mmio_get_region(bedrock, 5));
    /*
     * The ECC back door sits below the directory in the same back-door area,
     * one byte per eight bytes of node memory. Both apertures take their
     * extent from the memory linked above, so neither answers anything until
     * that link is set.
     */
    memory_region_add_subregion(
        system, sn1_node_address(node, SN1_HUB_ECC),
        sysbus_mmio_get_region(bedrock, 6));
    /* The scrub carrier indexes the same bytes from four eighths. */
    memory_region_add_subregion(
        system, sn1_node_address(node, SN1_HUB_ECC_SCRUB),
        sysbus_mmio_get_region(bedrock, 10));
    memory_region_add_subregion_overlap(
        system, sn1_node_address(node, SN1_RAM_ADDRESS),
        sysbus_mmio_get_region(bedrock, 7), 0);
    /*
     * The memory-special space (uncached attribute 2, the processor's third
     * window) reaches the same node memory through the Bedrock's fetch-and-op
     * operations instead of plain loads and stores.
     */
    memory_region_add_subregion(
        system, sn1_node_address(node, SN1_MEMORY_SPECIAL_ADDRESS),
        sysbus_mmio_get_region(bedrock, 9));

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

PCIBus *sn1_node_xbridge_pci_bus(SN1NodeState *node, unsigned int widget)
{
    return sgi_xbridge_pci_bus(SGI_XBRIDGE(node->xbridge), widget);
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
    sn1_node_init_claims(node);
}

static void sn1_node_unrealize(DeviceState *dev)
{
    SN1NodeState *node = SN1_NODE(dev);

    sn_promdata_poll_free(&node->promdata_poll);
}

/*
 * The per-CPU cache-as-RAM private window (sn-promdata.c, shared with
 * sgi-sn0-node) is per-slot state the node owns; the array-level wrapper
 * below is per node type because it names SN1NodeState's promdata[] field,
 * the same way sn0.c's vmstate_sn0_flash names SN0NodeState's. There is no
 * prior wire format to preserve here: this is the node's first
 * VMStateDescription.
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

static bool sn1_leds_needed(void *opaque)
{
    SN1NodeState *node = opaque;

    return !buffer_is_zero(node->led, sizeof(node->led));
}

static const VMStateDescription vmstate_sn1_node_leds = {
    .name = "sn1-node/leds",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sn1_leds_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(led, SN1NodeState, SGI_SN1_NODE_MAX_CPUS),
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
        &vmstate_sn1_node_leds,
        NULL
    }
};

static void sn1_node_instance_init(Object *obj)
{
    SN1NodeState *node = SN1_NODE(obj);

    node->cpuclk = qdev_init_clock_in(DEVICE(obj), "cpu-refclk", NULL, NULL,
                                      0);
    qdev_init_gpio_in_named(DEVICE(obj), sn1_node_bedrock_reset,
                            "bedrock-reset", 1);
}

static void sn1_node_instance_finalize(Object *obj)
{
    SN1NodeState *node = SN1_NODE(obj);

    g_free(node->prom_views);
    g_free(node->xbridge_widgets);
    g_free(node->prom_view_names[0]);
    g_free(node->prom_view_names[1]);
}

static const Property sn1_node_properties[] = {
    DEFINE_PROP_LINK("ram", SN1NodeState, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT32("nr-cpus", SN1NodeState, nr_cpus, 0),
    DEFINE_PROP_UINT32("nasid", SN1NodeState, nasid, 0),
    DEFINE_PROP_UINT32("l1-bay", SN1NodeState, l1_bay, 0),
    /* Set on a machine's last node: a pflash unit past it has no node. */
    DEFINE_PROP_BOOL("last-node", SN1NodeState, last_node, true),
    DEFINE_PROP_BOOL("promdata-filter", SN1NodeState, promdata_filter, true),
    /*
     * Off by default: the fast path (sn-promdata.c) is proven on sgi-sn0-
     * node's boot and migration coverage, but nothing here yet exercises it
     * under Bedrock. Sharing the implementation lets it be turned on once
     * that evidence exists, without a second copy.
     */
    DEFINE_PROP_BOOL("promdata-fast", SN1NodeState, promdata_fast, false),
};

/* No source gives the LEDs' reset value; start them dark. */
static void sn1_node_reset_hold(Object *obj, ResetType type)
{
    SN1NodeState *node = SN1_NODE(obj);

    memset(node->led, 0, sizeof(node->led));
}

static void sn1_node_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = sn1_node_realize;
    dc->unrealize = sn1_node_unrealize;
    dc->vmsd = &vmstate_sn1_node;
    device_class_set_props(dc, sn1_node_properties);
    /* The machine wires a node's RAM and clock; -device cannot. */
    dc->user_creatable = false;
    rc->phases.hold = sn1_node_reset_hold;
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
