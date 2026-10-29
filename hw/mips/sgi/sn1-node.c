/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI Bedrock node device. See sn1-node.h.
 *
 * The family builder (sn-machine.c) creates one of these from the format-2
 * descriptor's Bedrock node record, links its RAM and CPU clock, sets its
 * populated I/O board (if any) and realizes it; this file owns everything the
 * node needs to run: the PROM/flash policy, its address-space views, CPUs,
 * Bedrock, and L1. The IP45 path also creates the node board's XBridge. The
 * Bedrock's XIO port and
 * the XBridge's port 0xa connect through the XBridge link
 * (hw/mips/sgi/xio-link.c) before either realizes. The IO-8's fixed devices
 * are a separate sgi-baseio (the "io8" preset, hw/mips/sgi/baseio.c) the
 * machine places on the chip's head F, through sn1_node_xbridge_pci_bus()
 * below. An IP35 board has no XBridge: the machine may hand its node one
 * in another brick (sn1_node_supply_xbridge()), which the node joins
 * the same way at the port the cable meets.
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
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qapi/error.h"
#include "block/block-global-state.h"
#include "block/block-io.h"
#include "qobject/qdict.h"
#include "chardev/char.h"
#include "accel/tcg/iommu.h"
#include "exec/page-protection.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/split-irq.h"
#include "hw/mips/sgi/sn-machine.h"
#include "hw/mips/sgi/xio-link.h"
#include "hw/mips/sgi-xio-port.h"
#include "hw/pci-host/sgi-xbridge.h"
#include "hw/pci-host/sgi-pic.h"
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
#include "system/tcg.h"
#include "cpu.h"
#include "cpu-qom.h"
#include "sn1-node.h"
#include "sn1-topology.h"
#include "hw/mips/sgi/initial-flash.h"
#include "hw/mips/sgi/sgi-cpu.h"

/* The Origin 300 IO-8 board, which carries the two-slot option riser. */
#define SN1_IO8_BOARD_PART "030-1673-003"

/*
 * The node's own I/O-space and Hub-register address decode: identical on
 * every Origin 300 measured, so constants here rather than descriptor
 * data repeated for a node kind with exactly one shipped configuration.
 */
#define SN1_SPACE_HUB UINT64_C(0x10000000000)
#define SN1_HUB_LOCAL_BOOT (SN1_SPACE_HUB + UINT64_C(0x18000000))
#define SN1_HUB_LOCAL_HIGH_BOOT (SN1_SPACE_HUB + UINT64_C(0x1fc00000))
#define SN1_HUB_REMOTE_BOOT (SN1_SPACE_HUB + UINT64_C(0x38000000))
#define SN1_HUB_ECC (SN1_SPACE_HUB + UINT64_C(0x140000000))
#define SN1_HUB_ECC_SCRUB (SN1_SPACE_HUB + UINT64_C(0x100000000))
#define SN1_HUB_DIRECTORY (SN1_SPACE_HUB + UINT64_C(0x1c0000000))
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
#define SN1_XBRIDGE_HOST_PORT 0xa

static const struct {
    uint64_t address;
    unsigned mmio;
    const char *name;
} sn1_md_windows[] = {
    { SN1_HUB_DIRECTORY, 5, "sn1.ni-directory" },
    { SN1_HUB_ECC, 6, "sn1.ni-ecc" },
    { SN1_HUB_ECC_SCRUB, 10, "sn1.ni-ecc-scrub" },
    { SN1_MEMORY_SPECIAL_ADDRESS, 9, "sn1.ni-fetchop" },
};

#define SN1_BOARD_ID_OFFSET UINT64_C(0x4000000)

#define SN1_MIPS_HARDWARE_IRQ_OFFSET 2

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
 * forms, which always reach its node whatever its number: the firmware's
 * references to the reset view (its memory references follow the claims;
 * see sn1_reset_local_translate()), the Hub special local space
 * (uncached alias, local registers and L1 UART, local boot window), the
 * Bedrock's register alias in XIO small window 1, and the cached alias,
 * sized from the processor interface's cached-alias register.
 *
 * On one node with number 0 every alias lands where the global map already
 * is, so the one-chassis machine decodes as before.
 */
#define SN1_HUB_LOCAL_FORMS_SIZE UINT64_C(0x20000000)
#define SN1_CACHED_ALIAS_MAX (64 * MiB)

static unsigned int sn1_node_number(SN1NodeState *node)
{
    return sn1_fabric_number(&node->endpoint);
}

static hwaddr sn1_promdata_live_node_base(void *opaque)
{
    SN1NodeState *node = opaque;

    return (hwaddr)sn1_node_number(node) << SN1_FABRIC_ADDRESS_SHIFT;
}

static SN1NodeState *sn1_node_peer(SN1NodeState *node)
{
    SN1FabricEndpoint *peer = sn1_fabric_peer(&node->endpoint);

    return peer ? (SN1NodeState *)object_dynamic_cast(peer->owner,
                                                     TYPE_SGI_SN1_NODE) : NULL;
}

SN1NodeState *sn1_node_claimant(SN1NodeState *node, unsigned int number)
{
    SN1FabricEndpoint *target = sn1_fabric_claimant(&node->endpoint, number);

    /* Directory operations require a processor node, not an I/O endpoint. */
    return target ?
        (SN1NodeState *)object_dynamic_cast(target->owner, TYPE_SGI_SN1_NODE) :
        NULL;
}

static uint64_t sn1_cached_alias_bytes(uint8_t code)
{
    return code ? 4 * KiB << (code - 1) : 0;
}

static void sn1_node_place_claims(SN1NodeState *node)
{
    SN1FabricEndpoint *peer = sn1_fabric_peer(&node->endpoint);
    unsigned int own = sn1_node_number(node);
    unsigned int other = peer ? sn1_fabric_number(peer) : 0;
    unsigned int view;

    if (peer && peer->ready && other == own) {
        qemu_log_mask(LOG_GUEST_ERROR, "sn1: node %u and its peer both carry "
                      "number %u; each reaches only itself there\n",
                      node->nasid, own);
    }
    for (view = 0; view < SN1_NODE_VIEWS; view++) {
        if (view < SN1_NODE_FABRIC_VIEW && view >= node->nr_cpus) {
            continue;
        }
        sn1_fabric_place_view(&node->endpoint, node->claim[view]);
        if (view < SN1_NODE_FABRIC_VIEW) {
            bool typed = sn1_fabric_routed(&node->endpoint) ||
                (peer && peer->ready && peer->isolated_route &&
                 sn1_fabric_number(peer) != own);

            bool direct = typed &&
                sn1_fabric_resolve_named(&node->endpoint, own) ==
                &node->endpoint;

            for (unsigned memory = 0; memory < 2; memory++) {
                hwaddr base = memory ? SN1_HUB_REMOTE_BOOT : SN1_RAM_ADDRESS;

                memory_region_set_address(&node->ni_memory_view[view][memory],
                    base + ((uint64_t)own << SN1_FABRIC_ADDRESS_SHIFT));
                memory_region_set_enabled(&node->ni_memory_view[view][memory],
                                          direct);
            }
            for (unsigned space = 0; space < SN1_NODE_SPACES; space++) {
                memory_region_set_enabled(
                    &node->ni_processor[view][space].region, typed);
                if (typed) {
                    memory_region_set_enabled(&node->claim[view][space][0],
                                              false);
                    memory_region_set_enabled(&node->claim[view][space][1],
                                              false);
                } else {
                    memory_region_set_enabled(&node->claim[view][space][0],
                                              true);
                }
            }
        } else {
            bool typed = sn1_fabric_routed(&node->endpoint) ||
                (peer && peer->ready && peer->isolated_route && other != own);

            memory_region_set_enabled(&node->ni_fabric_memory.region, typed);
            memory_region_set_enabled(&node->claim[view][0][0], !typed);
            if (typed) {
                memory_region_set_enabled(&node->claim[view][0][1], false);
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

static void sn1_node_place_endpoint_claims(SN1FabricEndpoint *endpoint)
{
    sn1_node_place_claims(SN1_NODE(endpoint->owner));
}

void sn1_node_update_claims(SN1NodeState *node)
{
    SGIBedrockState *bedrock = SGI_BEDROCK(node->bedrock);

    if (!node->endpoint.ready) {
        return;
    }
    if (bedrock->net_cable && !node->cable_notifier_added) {
        sgi_net_cable_add_change_notifier(bedrock->net_cable,
                                          &node->cable_notifier);
        node->cable_notifier_added = true;
    }
    SN1FabricEndpoint *peer = sn1_fabric_peer(&node->endpoint);

    sgi_bedrock_io_set_destination(&bedrock->io,
        sn1_fabric_routed(&node->endpoint) || (peer && peer->isolated_route) ?
            sn1_fabric_destination_rw : NULL,
        &node->endpoint);
    sn1_fabric_update_claims(&node->endpoint);
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

        if (node->cpu[i]) {
            r10k_cpu_set_requester_id(node->cpu[i],
                sn1_node_number(node) << 3 | (slot & 1) | (slot & 2) << 1);
        }
    }
}

static void sn1_reset_local_unmap(SN1NodeState *node);

static void sn1_node_number_changed(Notifier *notifier, void *data)
{
    SN1NodeState *node = container_of(notifier, SN1NodeState, node_notifier);

    sn1_node_set_requester_ids(node);
    sn1_node_update_claims(node);
    sn1_memory_mappings_refresh(node->memory_mappings, NULL);
    /* Whether this node is 0 decides its processors' reset memory. */
    sn1_reset_local_unmap(node);
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
    sn1_fabric_init_view(&node->endpoint, view, container, node->claim_floor,
                        node->claim_floor_alias[view], node->claim[view]);
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
    SN1NodeState *node = opaque;
    SN1FabricEndpoint *target = sn1_fabric_claimant(&node->endpoint, number);

    SN1FabricEndpoint *peer = sn1_fabric_peer(&node->endpoint);

    if (sn1_fabric_routed(&node->endpoint) || (peer && peer->isolated_route)) {
        target = sn1_fabric_resolve_named(&node->endpoint, number);
    }
    return target ? &target->bedrock->io : NULL;
}

static void sn1_node_cached_alias_changed(Notifier *notifier, void *data)
{
    SGIBedrockPIState *pi = data;
    SN1NodeState *node = container_of(notifier - pi->interface, SN1NodeState,
                                      cached_alias_notifier[0]);

    sn1_node_update_claims(node);
}

static MemTxResult sn1_node_ni_special_rw(MemoryRegion *mr, hwaddr offset,
    void *buffer, size_t size, bool write, MemTxAttrs attrs)
{
    uint64_t value;
    MemTxResult result;

    if (write) {
        return memory_region_dispatch_write(mr, offset,
                                            ldn_be_p(buffer, size),
                                            size_memop(size) | MO_BE, attrs);
    }
    result = memory_region_dispatch_read(mr, offset, &value,
                                         size_memop(size) | MO_BE, attrs);
    if (result == MEMTX_OK) {
        stn_be_p(buffer, size, value);
    }
    return result;
}

static MemTxResult sn1_node_ni_processor_rw(SN1FabricEndpoint *endpoint,
    unsigned origin, hwaddr address, void *buffer, size_t size, bool write,
    MemTxAttrs attrs)
{
    SN1NodeState *node = SN1_NODE(endpoint->owner);
    hwaddr local = address - ((uint64_t)endpoint->placement << 33);
    AddressSpace *as;
    hwaddr offset;

    for (unsigned i = 0; i < ARRAY_SIZE(sn1_md_windows); i++) {
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(node->bedrock),
                                                 sn1_md_windows[i].mmio);
        uint64_t extent = memory_region_size(mr);

        if (local >= sn1_md_windows[i].address &&
            local - sn1_md_windows[i].address < extent &&
            size <= extent - (local - sn1_md_windows[i].address)) {
            offset = local - sn1_md_windows[i].address;
            return write ? address_space_write(&node->ni_md_as[i], offset,
                                                attrs, buffer, size) :
                           address_space_read(&node->ni_md_as[i], offset,
                                               attrs, buffer, size);
        }
    }
    if (local < memory_region_size(&node->ni_target[0]) &&
        size <= memory_region_size(&node->ni_target[0]) - local) {
        as = &node->ni_ram_as;
        offset = local;
    } else if (local >= SN1_HUB_REMOTE_REGISTERS_ADDRESS +
                        SN1_BOARD_ID_OFFSET &&
               local - (SN1_HUB_REMOTE_REGISTERS_ADDRESS +
                        SN1_BOARD_ID_OFFSET) < 8) {
        offset = local - (SN1_HUB_REMOTE_REGISTERS_ADDRESS +
                          SN1_BOARD_ID_OFFSET);
        if (offset || size != 8) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sn1: remote board identification refuses %zu-byte "
                          "%s at 0x%" HWADDR_PRIx "\n", size,
                          write ? "write" : "read", address);
            return MEMTX_ERROR;
        }
        as = &node->ni_board_id_as;
    } else if (local >= SN1_HUB_REMOTE_REGISTERS_ADDRESS &&
               local - SN1_HUB_REMOTE_REGISTERS_ADDRESS <
                   SN1_HUB_REGISTER_HEAD_SIZE &&
               size <= SN1_HUB_REGISTER_HEAD_SIZE -
                       (local - SN1_HUB_REMOTE_REGISTERS_ADDRESS)) {
        offset = local - SN1_HUB_REMOTE_REGISTERS_ADDRESS;
        return sn1_node_ni_special_rw(&node->hub_unidentified, offset, buffer,
                                      size, write, attrs);
    } else if (local >= SN1_HUB_REMOTE_BOOT &&
               local - SN1_HUB_REMOTE_BOOT < UINT64_C(0x8000000) &&
               size <= UINT64_C(0x8000000) - (local - SN1_HUB_REMOTE_BOOT) &&
               node->ni_boot_initialized) {
        as = &node->ni_boot_as;
        offset = local - SN1_HUB_REMOTE_BOOT;
    } else if (local >= SN1_HUB_REMOTE_REGISTERS_ADDRESS +
                        SN1_HUB_REGISTER_HEAD_SIZE &&
               local < SN1_HUB_REMOTE_REGISTERS_ADDRESS +
                       SN1_HUB_REGISTER_UART_END &&
               size <= SN1_HUB_REMOTE_REGISTERS_ADDRESS +
                       SN1_HUB_REGISTER_UART_END - local) {
        as = &node->ni_uart_as;
        offset = local - SN1_HUB_REMOTE_REGISTERS_ADDRESS;
    } else if (local >= SN1_HUB_REMOTE_REGISTERS_ADDRESS +
                        SN1_HUB_LED_OFFSET &&
               local - (SN1_HUB_REMOTE_REGISTERS_ADDRESS +
                        SN1_HUB_LED_OFFSET) < SN1_HUB_LED_SIZE &&
               size <= SN1_HUB_LED_SIZE -
                       (local - (SN1_HUB_REMOTE_REGISTERS_ADDRESS +
                                 SN1_HUB_LED_OFFSET))) {
        offset = local - (SN1_HUB_REMOTE_REGISTERS_ADDRESS +
                          SN1_HUB_LED_OFFSET);
        return sn1_node_ni_special_rw(&node->hub_leds, offset, buffer,
                                      size, write, attrs);
    } else if (local >= SN1_BEDROCK_NODE_ADDRESS &&
               local - SN1_BEDROCK_NODE_ADDRESS < SN1_BEDROCK_NODE_SIZE &&
               size <= SN1_BEDROCK_NODE_SIZE -
                       (local - SN1_BEDROCK_NODE_ADDRESS)) {
        as = &node->ni_hub_as;
        offset = local - SN1_BEDROCK_NODE_ADDRESS;
    } else if (local >= SN1_IO_SPACE_ADDRESS &&
               local - SN1_IO_SPACE_ADDRESS < SGI_BEDROCK_IO_PIO_SIZE &&
               size <= SGI_BEDROCK_IO_PIO_SIZE -
                       (local - SN1_IO_SPACE_ADDRESS)) {
        as = &node->ni_pio_as;
        offset = local - SN1_IO_SPACE_ADDRESS;
    } else {
        if (local >= SN1_HUB_REMOTE_REGISTERS_ADDRESS &&
            local - SN1_HUB_REMOTE_REGISTERS_ADDRESS <
                SN1_HUB_LOCAL_REGISTERS_SIZE) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sn1: unmapped remote Hub register %zu-byte %s "
                          "at 0x%" HWADDR_PRIx "\n", size,
                          write ? "write" : "read", address);
        }
        return MEMTX_DECODE_ERROR;
    }
    return write ? address_space_write(as, offset, attrs, buffer, size) :
                   address_space_read(as, offset, attrs, buffer, size);
}

static MemTxResult sn1_node_ni_destination_rw(SN1FabricEndpoint *endpoint,
    const SGIXioSource *source, hwaddr address, void *buffer, size_t size,
    bool write, SGIXioPacketAttrs packet_attrs)
{
    SN1NodeState *node = SN1_NODE(endpoint->owner);
    AddressSpace *as;
    hwaddr offset;

    if (address & (UINT64_C(1) << 47)) {
        hwaddr window = address & ((UINT64_C(1) << 39) - 1);

        if ((window & ~UINT64_C(0x7fffff)) != UINT64_C(0x1800000)) {
            return MEMTX_DECODE_ERROR;
        }
        offset = window & 0x7fffff;
        if (offset >= 0x400000 || size > 0x400000 - offset) {
            return MEMTX_DECODE_ERROR;
        }
        as = &SGI_BEDROCK(node->bedrock)->io.hub_as;
    } else {
        offset = address & (SN1_FABRIC_WINDOW_SIZE - 1);
        if (offset >= SGI_BEDROCK_MD_MEMORY_SIZE ||
            size > SGI_BEDROCK_MD_MEMORY_SIZE - offset) {
            return MEMTX_DECODE_ERROR;
        }
        as = &node->ni_ram_as;
    }
    return sn1_fabric_packet_target_rw(as, offset, buffer, size, write,
                                       packet_attrs);
}

static bool sn1_node_bte_local_rw(void *opaque, hwaddr address,
                                 void *buffer, size_t size, bool write,
                                 MemTxResult *result)
{
    SN1NodeState *node = opaque;
    unsigned number;

    if (address >= (UINT64_C(1) << 40)) {
        return false;
    }
    number = (address >> SN1_FABRIC_ADDRESS_SHIFT) & 0x7f;
    if (number != sn1_node_number(node)) {
        return false;
    }

    *result = sn1_node_ni_destination_rw(&node->endpoint, NULL, address,
                                         buffer, size, write,
                                         (SGIXioPacketAttrs) { 0 });
    return true;
}

static void sn1_node_ni_target_as(SN1NodeState *node, unsigned index,
    MemoryRegion *target, AddressSpace *as, const char *name)
{
    uint64_t size = memory_region_size(target);
    uint64_t split = size / 2;

    /* A complete alias can flatten to the target's global placement. */
    memory_region_init(&node->ni_target[index], OBJECT(node), name, size);
    for (unsigned i = 0; i < 2; i++) {
        uint64_t offset = i ? split : 0;
        uint64_t bytes = i ? size - split : split;
        g_autofree char *part = g_strdup_printf("%s.%u", name, i);

        memory_region_init_alias(&node->ni_target_part[index][i], OBJECT(node),
                                 part, target, offset, bytes);
        memory_region_add_subregion(&node->ni_target[index], offset,
                                    &node->ni_target_part[index][i]);
    }
    address_space_init(as, &node->ni_target[index], name);
}

static void sn1_node_init_claims(SN1NodeState *node)
{
    unsigned int cpu, i;

    node->endpoint.owner = OBJECT(node);
    node->endpoint.bedrock = SGI_BEDROCK(node->bedrock);
    node->endpoint.placement = node->nasid;
    node->endpoint.processor_origins = node->nr_cpus;
    node->endpoint.place_claims = sn1_node_place_endpoint_claims;
    node->endpoint.processor_rw = sn1_node_ni_processor_rw;
    node->endpoint.destination_rw = sn1_node_ni_destination_rw;
    /* Incoming memory uses the same bank decoder and overlays as CPUs. */
    sn1_node_ni_target_as(node, 0, &SGI_BEDROCK(node->bedrock)->md.memory_mr,
                          &node->ni_ram_as, "sn1.ni-ram");
    sgi_bedrock_io_set_bte_local_rw(&SGI_BEDROCK(node->bedrock)->io,
                                    sn1_node_bte_local_rw, node);
    address_space_init(&node->ni_hub_as,
                      &SGI_BEDROCK(node->bedrock)->hub_window, "sn1.ni-hub");
    sn1_node_ni_target_as(node, 4, sysbus_mmio_get_region(
        SYS_BUS_DEVICE(node->bedrock), 3), &node->ni_uart_as, "sn1.ni-uart");
    sn1_node_ni_target_as(node, 2, &node->board_id, &node->ni_board_id_as,
                          "sn1.ni-board-id");
    sn1_node_ni_target_as(node, 1, sysbus_mmio_get_region(
        SYS_BUS_DEVICE(node->bedrock), 1), &node->ni_pio_as, "sn1.ni-pio");
    for (i = 0; i < ARRAY_SIZE(sn1_md_windows); i++) {
        sn1_node_ni_target_as(node, 5 + i, sysbus_mmio_get_region(
            SYS_BUS_DEVICE(node->bedrock), sn1_md_windows[i].mmio),
            &node->ni_md_as[i], sn1_md_windows[i].name);
    }
    sn1_fabric_init_floor(&node->endpoint, node->claim_floor);
    for (cpu = 0; cpu < node->nr_cpus; cpu++) {
        sn1_node_view_claims(node, cpu, &node->cpu_sysmem[cpu]);
        /* Keep instruction memory on the existing RAM/ROM and MD aliases. */
        for (unsigned memory = 0; memory < 2; memory++) {
            hwaddr base = memory ? SN1_HUB_REMOTE_BOOT : SN1_RAM_ADDRESS;
            uint64_t size = memory ? UINT64_C(0x8000000) :
                                    SN1_FABRIC_WINDOW_SIZE;
            g_autofree char *name = g_strdup_printf(
                "sn1.node%u.cpu%u.direct-%s", node->nasid, cpu,
                memory ? "boot" : "memory");

            memory_region_init_alias(&node->ni_memory_view[cpu][memory],
                OBJECT(node), name, get_system_memory(),
                sn1_node_address(node, base), size);
            memory_region_set_enabled(&node->ni_memory_view[cpu][memory],
                                      false);
            memory_region_add_subregion_overlap(&node->cpu_sysmem[cpu], base,
                &node->ni_memory_view[cpu][memory], 1);
        }
        for (unsigned space = 0; space < SN1_NODE_SPACES; space++) {
            sn1_fabric_init_processor_view(&node->ni_processor[cpu][space],
                &node->endpoint, cpu, space, &node->cpu_sysmem[cpu]);
        }
        sn1_node_local_form(node, cpu, 1, "hub-special", SN1_SPACE_HUB,
                            SN1_HUB_LOCAL_FORMS_SIZE);
        sn1_node_local_form(node, cpu, 2, "registers",
                            SN1_BEDROCK_LOCAL_ADDRESS, SN1_BEDROCK_NODE_SIZE);
        sn1_node_local_form(node, cpu, 3, "cached-alias", SN1_RAM_ADDRESS,
                            SN1_CACHED_ALIAS_MAX);
        memory_region_set_enabled(&node->local_form[cpu][3], false);
    }
    sn1_node_view_claims(node, SN1_NODE_FABRIC_VIEW, &node->fabric);
    /* DMA keeps its attributes and reaches only the named memory space. */
    sn1_fabric_init_processor_view(&node->ni_fabric_memory, &node->endpoint,
        SN1_NODE_FABRIC_VIEW, 0, &node->fabric);
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
    node->endpoint.ready = true;
    sn1_node_update_claims(node);
}


/*
 * Opens the part's two 8 MiB positions over @backing and hands them to the
 * flash device. The legacy pflash backend @blk, when given, is moved onto
 * the lower position so the drive stays claimed; a bound block node is
 * instead named directly.
 */
static bool sn1_node_attach_flash_slices(SN1NodeState *node,
                                         BlockDriverState *backing,
                                         BlockBackend *blk, Error **errp)
{
    int flags = bdrv_get_flags(backing) & (BDRV_O_RDWR | BDRV_O_INACTIVE);
    BlockDriverState *lower, *upper;
    bool ok;

    lower = sgi_bedrock_flash_open_slice(backing, 0, flags, errp);
    if (!lower) {
        return false;
    }
    upper = sgi_bedrock_flash_open_slice(backing, SGI_BEDROCK_FLASH_PART_SIZE,
                                         flags, errp);
    if (!upper) {
        bdrv_unref(lower);
        return false;
    }
    if (blk) {
        ok = blk_replace_bs(blk, lower, errp) == 0 &&
             qdev_prop_set_drive_err(node->boot_flash, "drive0", blk, errp);
    } else {
        ok = object_property_set_str(OBJECT(node->boot_flash), "drive0",
                                     bdrv_get_node_name(lower), errp);
    }
    ok = ok && object_property_set_str(OBJECT(node->boot_flash), "drive1",
                                       bdrv_get_node_name(upper), errp);
    bdrv_unref(upper);
    bdrv_unref(lower);
    return ok;
}

/*
 * Creates the node's boot-flash part from the machine's node<N>-flash
 * binding or a -drive if=pflash and returns the processor's read view of
 * its first selection, or NULL with no error when neither was given (the
 * caller then falls back to an immutable -bios image). node_index selects
 * the pflash unit the way sgi_machine_load_flash's own per-node override
 * does for the Hub path: unit N for node N, so a second node takes its
 * drive rather than colliding with the first's.
 */
static MemoryRegion *sn1_node_create_boot_flash(SN1NodeState *node,
                                                unsigned int node_index,
                                                Error **errp)
{
    const char *machine = MACHINE_GET_CLASS(qdev_get_machine())->name;
    DriveInfo *dinfo = drive_get(IF_PFLASH, 0, node_index);
    DriveInfo *extra = node->last_node ?
        drive_get(IF_PFLASH, 0, node_index + 1) : NULL;
    BlockBackend *blk = NULL;
    BlockDriverState *backing;
    SysBusDevice *flash;

    if (node->flash_node) {
        if (dinfo) {
            error_setg(errp, "%s: node%u-flash and -drive if=pflash,unit=%u "
                       "both back node %u's flash", machine, node_index,
                       node_index, node_index);
            return NULL;
        }
        if (extra) {
            error_setg(errp, "%s accepts one 16 MiB pflash image per node",
                       machine);
            return NULL;
        }
        /*
         * The machine checked the binding's size against the catalogue.
         * Any node will do, as for the Hub and Octane flash: the two
         * positions are raw slices over it.
         */
        backing = bdrv_lookup_bs(node->flash_node, node->flash_node, errp);
        if (!backing) {
            return NULL;
        }
    } else {
        if (!dinfo && !extra) {
            return NULL;
        }
        if (!dinfo || extra) {
            error_setg(errp, "%s accepts one 16 MiB pflash image per node",
                       machine);
            return NULL;
        }
        blk = blk_by_legacy_dinfo(dinfo);
        backing = blk_bs(blk);
        if (blk_getlength(blk) != SGI_BEDROCK_FLASH_SIZE) {
            error_setg(errp, "%s pflash image must be exactly 16 MiB",
                       machine);
            return NULL;
        }
        if (strcmp(bdrv_get_format_name(backing), "raw")) {
            error_setg(errp, "%s pflash image must use raw format", machine);
            return NULL;
        }
    }

    node->boot_flash = qdev_new(TYPE_SGI_BEDROCK_FLASH);
    object_property_add_child(OBJECT(node), "boot-flash",
                              OBJECT(node->boot_flash));
    qdev_prop_set_uint32(node->boot_flash, "node", node_index);
    if (!sn1_node_attach_flash_slices(node, backing, blk, errp)) {
        return NULL;
    }
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

/*
 * Match the loaded image's configuration block to this node's product and
 * processor module, as qemu-sgi-machine-init does for a new machine. A
 * memory node has no processor module to describe, so its image stays
 * as supplied.
 */
static void sn1_apply_cpu_config(SN1NodeState *node, uint8_t *payload)
{
    SGIIP35FlashValues values;

    if (!node->nr_cpus) {
        return;
    }
    values = (SGIIP35FlashValues) {
        .machine_type = node->firmware_machine_type,
        .cpu_mode_word = node->firmware_mode_word,
        .cpu_clock_hz = sn1_cpu_processor(node->model_node, 0)->clock_hz,
    };
    sgi_ip35_flash_configure(payload, &values);
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
    uint64_t size = memory_region_size(&node->prom_view_reset.rom);
    MemoryRegion *backing = node->prom_view_reset.backing;
    g_autofree DirtyBitmapSnapshot *snapshot =
        memory_region_snapshot_and_clear_dirty(backing, 0, size,
                                               DIRTY_MEMORY_VGA);

    sn1_prom_view_apply(&node->prom_view_reset, everything ? NULL : snapshot,
                       size);
    sn1_prom_view_apply(&node->prom_view_local_high,
                       everything ? NULL : snapshot, size);
}

static void sn1_prom_flash_programmed(void *opaque, unsigned position)
{
    if (position == 0) {
        sn1_refresh_prom_views(opaque, false);
    }
}

void sn1_node_reset_cpus(SN1NodeState *node)
{
    unsigned int i;

    for (i = 0; i < node->nr_cpus; i++) {
        cpu_reset(CPU(node->cpu[i]));
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
#define TYPE_SN1_RESET_ROUTER "sn1-reset-router"
#define TYPE_SN1_RESET_LOCAL_ROUTER "sn1-reset-local-router"

enum {
    SN1_RESET_MEMORY,
    SN1_RESET_PROM,
    SN1_RESET_INDEXES,
};

/*
 * Compatibility policy: the stock PROM still executes noncoherently and
 * through KSEG1 here after enabling RAM. Coherent and explicit uncached
 * memory accesses must see RAM. These bounded firmware views reserve PROM.
 */
static int sn1_reset_attrs_to_index(IOMMUMemoryRegion *iommu, MemTxAttrs attrs)
{
    /* MIPS supplies mips_cca even when attrs.unspecified remains set. */
    return attrs.mips_cca == 3 || attrs.mips_kseg1 ?
           SN1_RESET_PROM : SN1_RESET_MEMORY;
}

static int sn1_reset_num_indexes(IOMMUMemoryRegion *iommu)
{
    return SN1_RESET_INDEXES;
}

/* A reference sn1_reset_attrs_to_index() sends to @index. */
static MemTxAttrs sn1_reset_index_attrs(int index)
{
    return (MemTxAttrs) { .mips_cca = index == SN1_RESET_PROM ? 3 : 5 };
}

/*
 * TCG adds a vCPU's notifier to the router the first time that vCPU fills a
 * TLB entry through it, from the vCPU thread and without the BQL, so two
 * first fills at once corrupt the shared list. sn1_register_reset_notifiers()
 * adds every one before any vCPU runs; a later first registration means one
 * was missed.
 */
static int sn1_reset_notify_flag_changed(IOMMUMemoryRegion *iommu,
                                         IOMMUNotifierFlag old,
                                         IOMMUNotifierFlag new, Error **errp)
{
    if (!bql_locked()) {
        error_setg(errp, "%s: notifier registered by a running vCPU",
                   memory_region_name(MEMORY_REGION(iommu)));
        return -EINVAL;
    }
    return 0;
}

static IOMMUTLBEntry sn1_reset_translate(IOMMUMemoryRegion *iommu,
                                         hwaddr addr, IOMMUAccessFlags flags,
                                         int index)
{
    SN1NodeState *node = container_of(iommu, SN1NodeState, reset_router);

    return (IOMMUTLBEntry) {
        .target_as = index == SN1_RESET_PROM ?
                     &node->reset_prom_as : &node->reset_memory_as,
        .iova = addr & ~UINT64_C(0xfff),
        .translated_addr = addr & ~UINT64_C(0xfff),
        .addr_mask = 0xfff,
        .perm = IOMMU_RW,
    };
}

static void sn1_reset_local_unmap(SN1NodeState *node)
{
    IOMMUTLBEvent event = {
        .type = IOMMU_NOTIFIER_UNMAP,
        .entry = {
            .target_as = &node->reset_claim_as,
            .iova = 0,
            .addr_mask = UINT64_MAX,
            .perm = IOMMU_NONE,
        },
    };

    if (node->reset_router_ready) {
        memory_region_notify_iommu(&node->reset_local_router,
                                   SN1_RESET_MEMORY, event);
    }
}

/*
 * The processors' reset view at node field zero. The firmware's references
 * (sn1_reset_attrs_to_index()) reach the reader's own PROM: every node's
 * processors fetch their own reset vector, and the stock PROM keeps
 * executing there noncoherently. Any other reference is a memory reference,
 * which names memory by node number. While this node carries number 0 it
 * takes this node's own reset view, banks or PROM, as before any node is
 * renumbered. Otherwise it takes the claim path the node's own DMA uses
 * (sn1_fabric_init_view()), which decodes number 0 through the cable, the NI
 * route table and the routers as for any other node-field-zero address, and
 * answers a decode error where number 0 is out of reach.
 */
static IOMMUTLBEntry sn1_reset_local_translate(IOMMUMemoryRegion *iommu,
                                               hwaddr addr,
                                               IOMMUAccessFlags flags,
                                               int index)
{
    SN1NodeState *node = container_of(iommu, SN1NodeState,
                                      reset_local_router);
    IOMMUTLBEntry entry = {
        .target_as = &node->reset_prom_as,
        .iova = addr & ~UINT64_C(0xfff),
        .translated_addr = addr & ~UINT64_C(0xfff),
        .addr_mask = 0xfff,
        .perm = IOMMU_RW,
    };

    if (index == SN1_RESET_PROM) {
        return entry;
    }
    if (sn1_node_number(node) == 0) {
        entry.target_as = &node->reset_memory_as;
        return entry;
    }
    entry.target_as = &node->reset_claim_as;
    entry.translated_addr += SN1_PROM_RESET;
    return entry;
}

/* The claim path moved: what number 0 reaches may have changed with it. */
static void sn1_reset_claim_changed(MemoryListener *listener,
                                    MemoryRegionSection *section)
{
    SN1NodeState *node = container_of(listener, SN1NodeState,
                                      reset_claim_listener);

    node->reset_claim_changed = true;
}

static void sn1_reset_claim_commit(MemoryListener *listener)
{
    SN1NodeState *node = container_of(listener, SN1NodeState,
                                      reset_claim_listener);

    if (node->reset_claim_changed) {
        node->reset_claim_changed = false;
        sn1_reset_local_unmap(node);
    }
}

static void sn1_reset_memory_changed(MemoryListener *listener,
                                      MemoryRegionSection *section)
{
    SN1NodeState *node = container_of(listener, SN1NodeState,
                                      reset_memory_listener);

    node->reset_memory_changed = true;
}

static void sn1_reset_memory_commit(MemoryListener *listener)
{
    SN1NodeState *node = container_of(listener, SN1NodeState,
                                      reset_memory_listener);
    IOMMUTLBEvent event = {
        .type = IOMMU_NOTIFIER_UNMAP,
        .entry = {
            .target_as = &node->reset_memory_as,
            .iova = 0,
            .addr_mask = UINT64_MAX,
            .perm = IOMMU_NONE,
        },
    };

    if (node->reset_memory_changed) {
        node->reset_memory_changed = false;
        memory_region_notify_iommu(&node->reset_router, SN1_RESET_MEMORY,
                                    event);
        /* This node's processors reach the same banks while it is 0. */
        sn1_reset_local_unmap(node);
    }
}

static void sn1_node_map_reset(SN1NodeState *node, MemoryRegion *prom)
{
    uint64_t size = memory_region_size(prom);

    memory_region_init_iommu(&node->reset_router, sizeof(node->reset_router),
                             TYPE_SN1_RESET_ROUTER, OBJECT(node),
                             "sn1.reset-router", size);
    memory_region_init(&node->reset_memory, OBJECT(node),
                       "sn1.reset-memory", size);
    memory_region_init_alias(&node->reset_prom_alias, OBJECT(node),
                             "sn1.reset-memory.prom", prom, 0, size);
    memory_region_init_alias(&node->reset_ram_alias, OBJECT(node),
                             "sn1.reset-memory.ram",
                             &SGI_BEDROCK(node->bedrock)->md.memory_live,
                             SN1_PROM_RESET, size);
    memory_region_add_subregion(&node->reset_memory, 0,
                                &node->reset_prom_alias);
    memory_region_add_subregion_overlap(&node->reset_memory, 0,
                                        &node->reset_ram_alias, 1);
    address_space_init(&node->reset_prom_as, prom, "sn1.reset-prom");
    address_space_init(&node->reset_memory_as, &node->reset_memory,
                       "sn1.reset-memory");
    node->reset_memory_listener = (MemoryListener) {
        .name = "sn1.reset-memory",
        .region_add = sn1_reset_memory_changed,
        .region_del = sn1_reset_memory_changed,
        .commit = sn1_reset_memory_commit,
    };
    memory_listener_register(&node->reset_memory_listener,
                              &node->reset_memory_as);
    node->reset_router_ready = true;
    /* Exact bounds retain QEMU's subpage dispatch at the payload's tail. */
    memory_region_add_subregion_overlap(
        get_system_memory(), sn1_node_address(node, SN1_PROM_RESET),
        MEMORY_REGION(&node->reset_router), 1);

    memory_region_init_iommu(&node->reset_local_router,
                             sizeof(node->reset_local_router),
                             TYPE_SN1_RESET_LOCAL_ROUTER, OBJECT(node),
                             "sn1.reset-local-router", size);
    memory_region_init(&node->reset_local_holder, OBJECT(node),
                       "sn1.reset-local", size);
    memory_region_add_subregion(&node->reset_local_holder, 0,
                                MEMORY_REGION(&node->reset_local_router));
    for (unsigned int cpu = 0; cpu < node->nr_cpus; cpu++) {
        g_autofree char *name = g_strdup_printf("sn1.node%u.cpu%u.local-reset",
                                                node->nasid, cpu);

        memory_region_init_alias(&node->local_form[cpu][0], OBJECT(node),
                                 name,
                                 MEMORY_REGION(&node->reset_local_router), 0,
                                 size);
        memory_region_add_subregion_overlap(&node->cpu_sysmem[cpu],
                                            SN1_PROM_RESET,
                                            &node->local_form[cpu][0], 1);
    }
    address_space_init(&node->reset_claim_as, &node->fabric,
                       "sn1.reset-claim");
    node->reset_claim_listener = (MemoryListener) {
        .name = "sn1.reset-claim",
        .region_add = sn1_reset_claim_changed,
        .region_del = sn1_reset_claim_changed,
        .commit = sn1_reset_claim_commit,
    };
    memory_listener_register(&node->reset_claim_listener,
                             &node->reset_claim_as);
}

static void sn1_reset_router_class_init(ObjectClass *klass, const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->attrs_to_index = sn1_reset_attrs_to_index;
    imrc->num_indexes = sn1_reset_num_indexes;
    imrc->translate = sn1_reset_translate;
    imrc->notify_flag_changed = sn1_reset_notify_flag_changed;
}

static void sn1_reset_local_router_class_init(ObjectClass *klass,
                                              const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->attrs_to_index = sn1_reset_attrs_to_index;
    imrc->num_indexes = sn1_reset_num_indexes;
    imrc->translate = sn1_reset_local_translate;
    imrc->notify_flag_changed = sn1_reset_notify_flag_changed;
}

/*
 * Where each router is mapped for the moment registration takes: above any
 * physical address a processor can issue, so nothing there is displaced.
 */
#define SN1_RESET_PROBE_BASE (UINT64_C(1) << 60)
#define SN1_RESET_PROBE_STRIDE (UINT64_C(1) << 36)

/*
 * Give every processor its notifier on every node's reset router, under the
 * BQL and before any vCPU runs, so that TCG's later fills find them present
 * and never write the router's notifier list (sn1_reset_notify_flag_changed).
 * A processor can reach a router other than its own node's through a claim on
 * another node's space once the PROM numbers the nodes, so registering at the
 * local reset window alone would not cover it. The only interface that
 * registers is a TLB fill, which needs the router in the processor's view:
 * each router is mapped into the system space at a probe address, filled once
 * per index from each address space of each processor, and unmapped again.
 * The fills write no TLB entry.
 */
void sn1_register_reset_notifiers(SN1NodeState *const *nodes,
                                  unsigned int count)
{
    unsigned int expected = 0;
    unsigned int i;
    CPUState *cs;

    if (!tcg_enabled()) {
        return;
    }
    CPU_FOREACH(cs) {
        expected += SN1_RESET_INDEXES;
    }

    /* Each node's global router, then its processors' node-zero form. */
    for (unsigned int kind = 0; kind < 2; kind++) {
        memory_region_transaction_begin();
        for (i = 0; i < count; i++) {
            SN1NodeState *node = nodes[i];
            MemoryRegion *router = kind ?
                MEMORY_REGION(&node->reset_local_router) :
                MEMORY_REGION(&node->reset_router);
            MemoryRegion *probe = kind ? &node->reset_local_probe :
                                         &node->reset_probe;

            if (!node->reset_router_ready) {
                continue;
            }
            memory_region_init_alias(probe, OBJECT(node),
                                     kind ? "sn1.reset-local-router.probe" :
                                            "sn1.reset-router.probe",
                                     router, 0, memory_region_size(router));
            memory_region_add_subregion(get_system_memory(),
                                        SN1_RESET_PROBE_BASE +
                                        i * SN1_RESET_PROBE_STRIDE,
                                        probe);
        }
        memory_region_transaction_commit();

        for (i = 0; i < count; i++) {
            SN1NodeState *node = nodes[i];
            IOMMUMemoryRegion *router = kind ? &node->reset_local_router :
                                               &node->reset_router;
            unsigned int registered = 0;
            IOMMUNotifier *n;

            if (!node->reset_router_ready) {
                continue;
            }
            CPU_FOREACH(cs) {
                for (int asidx = 0; asidx <= cs->cc->max_as; asidx++) {
                    if (!cpu_get_address_space(cs, asidx)) {
                        continue;
                    }
                    for (int index = 0; index < SN1_RESET_INDEXES; index++) {
                        MemTxAttrs attrs = sn1_reset_index_attrs(index);
                        int prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
                        hwaddr xlat, plen = 1;

                        g_assert(sn1_reset_attrs_to_index(router, attrs) ==
                                 index);
                        WITH_RCU_READ_LOCK_GUARD() {
                            address_space_translate_for_iotlb(cs, asidx,
                                SN1_RESET_PROBE_BASE +
                                i * SN1_RESET_PROBE_STRIDE,
                                &xlat, &plen, attrs, &prot);
                        }
                    }
                }
            }
            IOMMU_NOTIFIER_FOREACH(n, router) {
                registered++;
            }
            /* One per processor and index, however many views reached it. */
            if (registered != expected) {
                error_report("%s: %u reset router notifiers registered, "
                             "expected %u",
                             memory_region_name(MEMORY_REGION(router)),
                             registered, expected);
                exit(1);
            }
        }

        memory_region_transaction_begin();
        for (i = 0; i < count; i++) {
            MemoryRegion *probe = kind ? &nodes[i]->reset_local_probe :
                                         &nodes[i]->reset_probe;

            if (nodes[i]->reset_router_ready) {
                memory_region_del_subregion(get_system_memory(), probe);
                object_unparent(OBJECT(probe));
            }
        }
        memory_region_transaction_commit();
    }
}

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

    if (contents && length != prom_size) {
        error_setg(errp, "%s: PROM image must be exactly %" PRIu64 " bytes",
                   MACHINE_GET_CLASS(qdev_get_machine())->name, prom_size);
        return false;
    }

    stored = sn1_node_create_boot_flash(node, node_index, errp);
    if (errp && *errp) {
        return false;
    }
    if (!stored && !contents) {
        error_setg(errp, "%s requires -bios with a stock PROM image, or a "
                   "boot flash for node %u",
                   MACHINE_GET_CLASS(qdev_get_machine())->name, node_index);
        return false;
    }
    if (stored) {
        /*
         * With a part attached the views answer through handlers over its
         * storage: they report the bytes it holds now, including a change the
         * monitor has just programmed, and refuse a write instead of passing
         * it to the part as a command cycle.
         */
        /* A backed IP53 image already has its authored configuration header. */
        if (!node->ip53_assembly_validated) {
            sn1_apply_cpu_config(node, memory_region_get_ram_ptr(stored));
        }
        memory_region_set_log(stored, true, DIRTY_MEMORY_VGA);
        /* Node 0 keeps the RAM names its snapshots carry. */
        node->prom_view_names[0] = node->nasid ?
            g_strdup_printf("sn1.node%u.prom.reset", node->nasid) :
            g_strdup("sn1.prom.reset");
        node->prom_view_names[1] = node->nasid ?
            g_strdup_printf("sn1.node%u.prom.local-high", node->nasid) :
            g_strdup("sn1.prom.local-high");
        if (!sn1_init_prom_view(&node->prom_view_reset, OBJECT(node),
                                node->prom_view_names[0], stored,
                                prom_size, errp) ||
            !sn1_init_prom_view(&node->prom_view_local_high, OBJECT(node),
                                node->prom_view_names[1], stored,
                                prom_size, errp)) {
            return false;
        }
        SGI_BEDROCK_FLASH(node->boot_flash)->programmed =
            sn1_prom_flash_programmed;
        SGI_BEDROCK_FLASH(node->boot_flash)->programmed_opaque = node;
        sn1_node_map_reset(node, &node->prom_view_reset.rom);
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
        sn1_node_ni_target_as(node, 3,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(node->boot_flash), 1),
            &node->ni_boot_as, "sn1.ni-boot");
        node->ni_boot_initialized = true;
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
    sn1_apply_cpu_config(node, memory_region_get_ram_ptr(&node->prom));
    memory_region_set_dirty(&node->prom, 0, prom_size);

    node->prom_view_names[0] = node->nasid ?
        g_strdup_printf("sn1.node%u.prom.reset", node->nasid) :
        g_strdup("sn1.prom.reset");
    if (!sn1_init_prom_view(&node->prom_view_reset, OBJECT(node),
                            node->prom_view_names[0], &node->prom,
                            prom_size, errp)) {
        return false;
    }

    /*
     * Which named views this node's flash controller answers, and their
     * addresses, are Bedrock hardware facts, not configuration: every view
     * the compiler ever emitted for a Bedrock node was already sized to the
     * flash chip's total size, so SN1_PROM_VIEW_NAMES names them directly
     * instead of reading a list the compiled catalogue no longer carries.
     */
    node->prom_views = g_new0(MemoryRegion, ARRAY_SIZE(sn1_prom_view_names));
    for (i = 1; i < ARRAY_SIZE(sn1_prom_view_names); i++) {
        const char *name = sn1_prom_view_names[i];
        uint64_t address;

        if (!sn1_node_flash_view_address(name, &address)) {
            error_setg(errp, "%s: flash view '%s' names no known boot space",
                       MACHINE_GET_CLASS(qdev_get_machine())->name, name);
            return false;
        }
        memory_region_init_alias(&node->prom_views[i], OBJECT(node),
                                 name, &node->prom, 0, prom_size);
        memory_region_add_subregion_overlap(
            system, sn1_node_address(node, address),
            &node->prom_views[i], 1);
    }
    sn1_node_map_reset(node, &node->prom_view_reset.rom);

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
    /* This stateless erased response is also reached from the CPU view. */
    node->prom_flash.disable_reentrancy_guard = true;
    memory_region_init(&node->ni_target[3], OBJECT(node), "sn1.ni-boot",
                       UINT64_C(0x8000000));
    memory_region_init_alias(&node->ni_target_part[3][0], OBJECT(node),
                             "sn1.ni-boot.payload", &node->prom, 0, prom_size);
    memory_region_add_subregion(&node->ni_target[3], 0,
                                &node->ni_target_part[3][0]);
    memory_region_init_alias(&node->ni_target_part[3][1], OBJECT(node),
                             "sn1.ni-boot.second", &node->prom_flash, 0,
                             SN1_PROM_SECOND_SELECTION_SIZE);
    memory_region_add_subregion(&node->ni_target[3],
                                SN1_HUB_REMOTE_FLASH - SN1_HUB_REMOTE_BOOT,
                                &node->ni_target_part[3][1]);
    address_space_init(&node->ni_boot_as, &node->ni_target[3], "sn1.ni-boot");
    node->ni_boot_initialized = true;
    return true;
}

/*
 * The board-identification register both consumers read: OpenBSD through the
 * remote form and the stock firmware through the local one, each taking the
 * product code from bits 15 through 12; the stock firmware also takes a
 * CPU-configuration selector from the low nibble. Fuel's catalogue word
 * carries that code and the selector its recorded runs used, because the rest
 * of its physical reset value is unknown.
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
    *value = node->board_id_word;
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

static void sn1_node_map_board_id(SN1NodeState *node, MemoryRegion *system)
{
    memory_region_init_io(&node->board_id, OBJECT(node), &sn1_board_id_ops,
                          node, "sn1.board-id", 8);
    /* Stateless response can be reached from this node's processor view. */
    node->board_id.disable_reentrancy_guard = true;
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

    sn1_node_map_board_id(node, system);
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
    for (unsigned view = 0; view < node->nr_cpus; view++) {
        if (mr == &node->ni_processor[view][1].region) {
            hwaddr local = offset & (SN1_FABRIC_WINDOW_SIZE - 1);
            hwaddr ecc = SN1_HUB_ECC - SN1_SPACE_HUB;

            if (local >= ecc &&
                local - ecc < memory_region_size(&bedrock->md.ecc_mmio)) {
                /* Select from the full register before the typed write. */
                return sgi_bedrock_md_ecc_store_value(local - ecc, value,
                                                     size);
            }
            break;
        }
    }
    return value;
}


/*
 * A processor's secondary-cache instruction, dispatched from its bus
 * interface and passed to the memory unit of the node whose memory it names. An
 * index instruction names a set rather than a line, so it goes to this node
 * and its one cabled peer, all an Origin 300 pair has; a line homed on any
 * other node would be missed. It runs outside any I/O access, so it takes
 * the lock itself, and only once a memory unit is watching anything.
 */
static bool sn1_node_bus_request(void *opaque, uint16_t requester_id,
                                 R10KSystemRequest request,
                                 hwaddr paddr, unsigned int set_bits)
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
    uint64_t index_mask = ((UINT64_C(1) << set_bits) - 1) * 128;
    uint64_t address = paddr & (SGI_BEDROCK_MD_MEMORY_SIZE - 1);
    bool found = false;

    if (request >= ARRAY_SIZE(md_request)) {
        return false;
    }
    /*
     * A cache instruction names the line at its address, on that line's
     * home node, so a later read of the line's word watches its page.
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
        found |= sgi_bedrock_md_processor_request(md, requester_id,
                                                  md_request[request],
                                                  address, index_mask);
    }
    return found;
}

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

static bool sn1_node_is_ip35(const SN1NodeState *node);
static bool sn1_node_is_cx(const SN1NodeState *node);

/*
 * Whether this node's processors are the experimental R16000 model: an
 * Origin 350-style IP53 board under an authorized runtime profile, or a
 * Cx-brick node board, which has no such profile.
 */
static bool sn1_node_uses_r16000(const SN1NodeState *node,
                                 MachineState *machine)
{
    return sn1_node_is_cx(node) ||
        (node->ip53_assembly_validated &&
         sgi_machine_ip53_experiment_authorized(machine));
}

static bool sn1_node_validate_processors(SN1NodeState *node,
                                        MachineState *machine, Error **errp)
{
    bool r16000 = sn1_node_uses_r16000(node, machine);

    if (r16000) {
        bool onyx350 = g_strcmp0(SGI_MACHINE_GET_CLASS(machine)->product,
                                 "onyx350") == 0;
        bool onyx4_host = g_strcmp0(SGI_MACHINE_GET_CLASS(machine)->product,
                                    "onyx4-host") == 0;
        bool tezro = g_strcmp0(SGI_MACHINE_GET_CLASS(machine)->product,
                               "tezro") == 0;
        uint32_t expected_mode = onyx4_host ? 0x009dad85 :
                                 onyx350 ? 0x009caf85 : 0x009cad85;
        uint64_t expected_clock = onyx4_host ? 1000000000 :
                                  onyx350 ? 800000000 : 700000000;
        unsigned int expected_cache_mib = onyx4_host ? 16 : tezro ? 4 : 8;

        if (tezro) {
            expected_mode = 0x009bad85;
        }

        if (strcmp(machine->cpu_type, MIPS_CPU_TYPE_NAME("R16000"))) {
            error_setg(errp, TYPE_SGI_SN1_NODE " IP53 experiment requires "
                       "R16000, requested %s", machine->cpu_type);
            return false;
        }
        for (unsigned int i = 0; i < node->nr_cpus; i++) {
            const SGIModelProcessor *processor =
                sn1_cpu_processor(node->model_node, i);

            /*
             * The explicit CPU type supplies omitted IDs; authored IDs must
             * agree with that type rather than silently selecting a variant.
             */
            if (strcmp(processor->kind, "R16000") ||
                processor->clock_hz != expected_clock ||
                !node->cpuclk || clock_get_hz(node->cpuclk) != expected_clock ||
                processor->cache_mib != expected_cache_mib ||
                !processor->has_mode_bits ||
                processor->mode_bits != expected_mode ||
                (processor->has_prid && processor->prid != 0x00000f50) ||
                (processor->has_fpu_prid &&
                 processor->fpu_prid != 0x00000900)) {
                error_setg(errp, TYPE_SGI_SN1_NODE " IP53 processor %u "
                           "does not match the authorized %u MHz, %u MiB, "
                           "reset-mode 0x%08x R16000 profile", i,
                           (unsigned int)(expected_clock / 1000000),
                           expected_cache_mib, expected_mode);
                return false;
            }
        }
        return true;
    }

    /*
     * A C-brick PIMM carries an R12000 or an R14000 (108-0266-001 printed
     * page 28); the Origin 3200's selected fit is a 400 MHz R12000.
     */
    if (sn1_node_is_ip35(node) && node->nr_cpus &&
        !strcmp(sn1_cpu_processor(node->model_node, 0)->kind, "R12000")) {
        if (strcmp(machine->cpu_type, MIPS_CPU_TYPE_NAME("R12000"))) {
            error_setg(errp, TYPE_SGI_SN1_NODE " R12000 board requires "
                       "R12000, requested %s", machine->cpu_type);
            return false;
        }
        for (unsigned int i = 1; i < node->nr_cpus; i++) {
            const SGIModelProcessor *processor =
                sn1_cpu_processor(node->model_node, i);

            if (strcmp(processor->kind, "R12000")) {
                error_setg(errp, TYPE_SGI_SN1_NODE " processor %u is %s; "
                           "the node's first is an R12000", i,
                           processor->kind);
                return false;
            }
        }
        return true;
    }
    if (strcmp(machine->cpu_type, MIPS_CPU_TYPE_NAME("R14000"))) {
        error_setg(errp, TYPE_SGI_SN1_NODE " requires R14000, requested %s",
                   machine->cpu_type);
        return false;
    }
    for (unsigned int i = 0; i < node->nr_cpus; i++) {
        uint32_t prid = sn1_cpu_processor(node->model_node, i)->prid;

        if (prid != 0x0f23 && prid != 0x0f24) {
            error_setg(errp, TYPE_SGI_SN1_NODE " processor %u has unsupported "
                       "R14000 PRId 0x%08x", i, prid);
            return false;
        }
    }
    return true;
}

static bool sn1_node_create_cpus(SN1NodeState *node, MachineState *machine,
                                 Error **errp)
{
    MemoryRegion *system = get_system_memory();
    uint64_t prom_size = node->model_node->flash->size;
    unsigned int i;

    if (node->promdata_filter) {
        sn_promdata_poll_init(&node->promdata_poll, node->promdata,
                              SGI_SN1_NODE_MAX_CPUS, NULL, NULL);
    }

    for (i = 0; i < node->nr_cpus; i++) {
        g_autofree char *name = g_strdup_printf("cpu%u", i);
        g_autofree char *alias_name =
            g_strdup_printf("sn1.node%u.cpu%u.system", node->nasid, i);
        g_autofree char *sysmem_name =
            g_strdup_printf("sn1.node%u.cpu%u.memory", node->nasid, i);
        const SGIModelProcessor *processor =
            sn1_cpu_processor(node->model_node, i);
        bool r16000 = sn1_node_uses_r16000(node, machine);
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
        if (!r16000) {
            if (!sgi_cpu_apply_processor(cpu_dev, processor, errp)) {
                return false;
            }
            /*
             * The reset-latched processor mode field is Config0 bits 24:0.
             * An R12000 takes it with the rest of its profile above.
             */
            if (!object_property_find(OBJECT(cpu_dev), "r12000-reset-mode")) {
                object_property_set_uint(OBJECT(cpu_dev), "reset-mode",
                    node->model_node->cpu_slots->value->assembly->mode_bits &
                    0x01ffffff, &error_abort);
            }
        } else {
            const GlobalProperty *prop;
            bool experimental;
            uint64_t reset_mode, scache_bytes;

            prop = qdev_find_global_prop(OBJECT(cpu_dev),
                                         "r16000-experimental");
            experimental = object_property_get_bool(
                OBJECT(cpu_dev), "r16000-experimental", &error_abort);
            if (prop && !experimental) {
                error_setg(errp, "IP53 CPU property r16000-experimental "
                           "conflicts with the authorized descriptor");
                return false;
            }
            prop = qdev_find_global_prop(OBJECT(cpu_dev),
                                         "r16000-reset-mode");
            reset_mode = object_property_get_uint(
                OBJECT(cpu_dev), "r16000-reset-mode", &error_abort);
            if (prop && reset_mode != processor->mode_bits) {
                error_setg(errp, "IP53 CPU property r16000-reset-mode "
                           "conflicts with the authorized descriptor");
                return false;
            }
            prop = qdev_find_global_prop(OBJECT(cpu_dev),
                                         "r16000-scache-bytes");
            scache_bytes = object_property_get_uint(
                OBJECT(cpu_dev), "r16000-scache-bytes", &error_abort);
            if (prop && scache_bytes != (uint64_t)processor->cache_mib * MiB) {
                error_setg(errp, "IP53 CPU property r16000-scache-bytes "
                           "conflicts with the authorized descriptor");
                return false;
            }
            object_property_set_bool(OBJECT(cpu_dev), "r16000-experimental",
                                     true, &error_abort);
            object_property_set_uint(OBJECT(cpu_dev), "r16000-reset-mode",
                                     processor->mode_bits, &error_abort);
            object_property_set_uint(OBJECT(cpu_dev), "r16000-scache-bytes",
                                     (uint64_t)processor->cache_mib * MiB,
                                     &error_abort);
        }
        /* Release both vCPUs from the machine reset handler, not realize. */
        object_property_set_bool(OBJECT(cpu_dev), "start-powered-off", true,
                                 &error_abort);
        cpu->store_transaction = sn1_node_store_transaction;
        cpu->store_transaction_opaque = node;
        if (!(node->promdata_filter && node->promdata_fast)) {
            /*
             * The processor's uncached view, where its Bedrock's I/O-space
             * path goes once the Bedrock exists (sn1_node_create_bedrock):
             * everything the physical view holds, for now. The PROM data
             * filter builds its own when it runs fast.
             */
            g_autofree char *uncached_name =
                g_strdup_printf("sn1.node%u.cpu%u.uncached", node->nasid, i);
            g_autofree char *uncached_alias_name =
                g_strdup_printf("sn1.node%u.cpu%u.uncached.memory",
                                node->nasid, i);

            memory_region_init(&node->cpu_uncached[i], OBJECT(node),
                               uncached_name, UINT64_MAX);
            memory_region_init_alias(&node->cpu_uncached_alias[i],
                                     OBJECT(node), uncached_alias_name,
                                     &node->cpu_sysmem[i], 0, UINT64_MAX);
            memory_region_add_subregion_overlap(&node->cpu_uncached[i], 0,
                                                &node->cpu_uncached_alias[i],
                                                0);
            object_property_set_link(OBJECT(cpu_dev), "uncached-memory",
                                     OBJECT(&node->cpu_uncached[i]),
                                     &error_abort);
            node->cpu_uncached_ready[i] = true;
        }
        qdev_realize(cpu_dev, NULL, &error_fatal);
        r10k_cpu_set_bus_handler(cpu, sn1_node_bus_request, node);
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
            /* Classification follows identity; backing stays at placement. */
            node->promdata[i].live_node_base = sn1_promdata_live_node_base;
            node->promdata[i].node_opaque = node;
            if (node->promdata_fast) {
                node->cpu_uncached_ready[i] = true;
            }
        }
    }
    return true;
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
 * reset.
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

bool sn1_node_set_option_intx_map(SN1NodeState *node,
                                 const uint8_t map[SGI_PCI_INTX_MAP_SIZE],
                                 Error **errp)
{
    if (DEVICE(node)->realized || node->option_intx_selected) {
        error_setg(errp, "compute option INTx map must be selected once "
                   "before realization");
        return false;
    }
    for (unsigned i = 0; i < SGI_PCI_INTX_MAP_SIZE; i++) {
        if (map[i] >= SGI_PCI_INTX_INPUTS) {
            error_setg(errp, "compute option INTx map has no head input %u",
                       map[i]);
            return false;
        }
    }
    memcpy(node->option_intx_map, map, sizeof(node->option_intx_map));
    node->option_intx_selected = 1;
    return true;
}

static void sn1_option_intx(void *opaque, int raw, int level)
{
    SN1NodeState *node = opaque;
    unsigned device = raw / PCI_NUM_PINS;
    uint8_t levels;

    node->option_intx_raw = (node->option_intx_raw & ~(1U << raw)) |
                           (level ? 1U << raw : 0);
    if (device != 1 && device != 2) {
        if (level) {
            qemu_log_mask(LOG_UNIMP, "sn1-node: compute option device index "
                          "%u has no experiment INTx map\n", device);
        }
        return;
    }
    levels = sgi_pci_intx_levels(node->option_intx_map, node->option_intx_raw);
    sgi_pci_intx_drive(node->option_intx_outputs,
                       node->option_intx_levels, levels);
    node->option_intx_levels = levels;
}

static void sn1_option_intx_clear(SN1NodeState *node)
{
    node->option_intx_raw = 0;
    node->option_intx_levels = 0;
    if (node->option_intx_inputs) {
        for (unsigned pin = 0; pin < SGI_PCI_INTX_INPUTS; pin++) {
            qemu_set_irq(node->option_intx_outputs[pin], 0);
        }
    }
}

static void sn1_option_intx_connect(SN1NodeState *node)
{
    if (!node->option_intx_selected) {
        return;
    }
    node->option_intx_inputs = qemu_allocate_irqs(sn1_option_intx, node,
                                                 SGI_XBRIDGE_HEAD_RAW_PCI_IRQS);
    for (unsigned pin = 0; pin < SGI_PCI_INTX_INPUTS; pin++) {
        node->option_intx_outputs[pin] = qdev_get_gpio_in_named(
            node->xbridge, "head-e-pin", pin);
    }
    for (unsigned raw = 0; raw < SGI_XBRIDGE_HEAD_RAW_PCI_IRQS; raw++) {
        qdev_connect_gpio_out_named(node->xbridge, "head-e-pci-intx", raw,
                                    node->option_intx_inputs[raw]);
    }
}

static bool sn1_node_is_cx(const SN1NodeState *node)
{
    return node->model_node && node->model_node->has_board_kind &&
           node->model_node->board_kind == SGI_MODEL_BOARD_KIND_IP53_CX;
}

/*
 * A board whose Bedrock has no XBridge of its own and reaches I/O over an
 * XIO cable: the C-brick's IP35 board, or a Cx-brick's IP53 node board.
 */
static bool sn1_node_is_ip35(const SN1NodeState *node)
{
    return sn1_node_is_cx(node) ||
           (node->model_node && node->model_node->has_board_kind &&
            node->model_node->board_kind == SGI_MODEL_BOARD_KIND_IP35);
}

static bool sn1_node_create_ip45_xbridge(SN1NodeState *node, Error **errp)
{
    size_t i;

    /*
     * The XBridge is fitted to the IP45 node board alongside the L1 and
     * boot flash. This file creates it directly, the family builder's
     * own device like every other product's onboard base I/O. An XIO
     * link joins it to the Bedrock's own XIO port before either
     * realizes: the link (hw/mips/sgi/xio-link.c) hands the XBridge the
     * Bedrock's packet face as the chip's "fabric" -- what its own
     * widget-0 conditions and both heads' interrupts reach the host
     * through. The IO-8's fixed devices
     * are a separate sgi-baseio (the "io8" preset) the machine places on
     * the chip's own head F, through the same board path every other
     * product's base I/O goes through (sgi_machine_instantiate_io_board()
     * in sn-machine.c, via sn1_node_xbridge_pci_bus() below) once the
     * chip itself has realized.
     */
    node->xbridge = qdev_new(TYPE_SGI_XBRIDGE);
    object_property_add_child(OBJECT(node), "xbridge", OBJECT(node->xbridge));
    qdev_prop_set_uint8(node->xbridge, "host-port", SN1_XBRIDGE_HOST_PORT);
    qdev_prop_set_uint8(node->xbridge, "xio-port", node->xbridge_xio_port);
    if (node->ni_route &&
        !sgi_xbridge_enable_packet_context(SGI_XBRIDGE(node->xbridge),
                                           errp)) {
        return false;
    }
    /*
     * The first node's buses keep the names they have always had; a second
     * node's carry its index, since PCI devices on same-named buses collide
     * in the migration stream.
     */
    if (node->nasid) {
        g_autofree char *prefix = g_strdup_printf("node%u-", node->nasid);

        qdev_prop_set_string(node->xbridge, "bus-prefix", prefix);
    }
    /*
     * The PCI riser card's two option slots, head E devices 1 and 2, are
     * sold as 64-bit universal slots, so they are taken to drive REQ64#
     * (inferred). The riser connects the IP45 motherboard to the IO-8
     * (007-4365-002, printed page 12), so the model fits it with the IO-8
     * at widget 15. Head F's own devices are left 32-bit, an
     * approximation: no held document gives that bus's width.
     */
    if (node->io_board_part && node->io_board_widget == 15 &&
        !strcmp(node->io_board_part, SN1_IO8_BOARD_PART)) {
        qdev_prop_set_uint8(node->xbridge, "req64-slots-e", BIT(1) | BIT(2));
    }
    if (node->io_board_part) {
        g_assert(node->io_board_widget == 14 || node->io_board_widget == 15);
        qdev_prop_set_string(node->xbridge,
                             node->io_board_widget == 14 ?
                             "board-part-e" : "board-part-f",
                             node->io_board_part);
        /* Each chassis's IO-8 answers with its own serial. */
        qdev_prop_set_uint64(node->xbridge,
                             node->io_board_widget == 14 ?
                             "onewire-serial-e" : "onewire-serial-f",
                             node->io_board_serial);
    }
    sgi_xio_link_init(&node->xbridge_link, OBJECT(node), "xbridge-link",
                      OBJECT(&SGI_BEDROCK(node->bedrock)->io), 0,
                      OBJECT(node->xbridge), SN1_XBRIDGE_HOST_PORT);
    if (!sgi_xio_link_connect(&node->xbridge_link, errp)) {
        return false;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(node->xbridge), errp)) {
        return false;
    }
    sn1_option_intx_connect(node);

    /*
     * Every widget the crossbar decodes: the host's local view (widget zero)
     * plus one per XIO attachment, including a board slot with nothing
     * plugged in; its head still answers "no PCI device here" through the
     * chip itself, which a widget left undecoded could not. Read through the
     * XIO port interface, as any other widget attachment in this tree
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
     * Origin 300 has a rear XIO cable connector (007-4365-002, page 8).
     * Fuel's rear-panel diagram (007-4480-001, page 6) shows none, but its
     * graphics board answers at widget 13, which the machine selects as
     * the external port when its topology fits one. Without a graphics
     * board the shared model still decodes the default external port
     * (SGI_XBRIDGE_XIO_PORT_DEFAULT) through Bedrock; Fuel's unused-port
     * wiring and register response are unknown.
     */
    {
        unsigned int port = sgi_xbridge_xio_port(SGI_XBRIDGE(node->xbridge));
        g_autofree char *property = g_strdup_printf("widget-%u", port);

        object_property_set_link(OBJECT(node->bedrock), property,
                                 OBJECT(sgi_xio_port_cpu_face(
                                     OBJECT(node->xbridge), port)),
                                 &error_abort);
    }
    /*
     * A widget scan addresses the board by the physical XIO port
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

    return true;
}

/*
 * The IP35 path: join the XBridge an XIO cable reaches in another brick.
 * The machine created that chip in its brick's module and set its
 * host-port to the port the cable meets, its other Crosstown2 port as
 * its xio-port, and its bus prefix, so the node writes none of them. It
 * keeps the IP45 path's link, realize and widget decode, with the cable's
 * port in place of SN1_XBRIDGE_HOST_PORT. Nothing here is IP45 riser or
 * IO-8 wiring, and no Bedrock widget is linked to the chip's other
 * Crosstown2 port: a port the cables leave empty decodes nothing.
 */
static bool sn1_node_join_supplied_xbridge(SN1NodeState *node, Error **errp)
{
    DeviceState *xbridge = node->supplied_xbridge;
    unsigned int host_port = object_property_get_uint(OBJECT(xbridge),
                                                      "host-port",
                                                      &error_abort);
    g_autofree char *host_port_property = g_strdup_printf("widget-%u",
                                                          host_port);
    size_t i;

    sgi_xio_link_init(&node->xbridge_link, OBJECT(node), "xbridge-link",
                      OBJECT(&SGI_BEDROCK(node->bedrock)->io), 0,
                      OBJECT(xbridge), host_port);
    if (!sgi_xio_link_connect(&node->xbridge_link, errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(xbridge), errp)) {
        return false;
    }
    for (i = 0; i < node->xbridge_widget_count; i++) {
        g_autofree char *property =
            g_strdup_printf("widget-%u", node->xbridge_widgets[i]);
        MemoryRegion *face = sgi_xio_port_cpu_face(OBJECT(xbridge),
                                                   node->xbridge_widgets[i]);

        if (!face) {
            error_setg(errp, "XBridge widget %u has no XIO face",
                       node->xbridge_widgets[i]);
            return false;
        }
        object_property_set_link(OBJECT(node->bedrock), property,
                                 OBJECT(face), &error_abort);
    }
    object_property_set_link(
        OBJECT(node->bedrock), host_port_property,
        OBJECT(sgi_xbridge_widget_view(SGI_XBRIDGE(xbridge), 0)),
        &error_abort);
    return true;
}

/*
 * The IP35 path for an IX-brick or PX-brick: join the middle PIC whose
 * integrated crossbar the cable reaches at port 10. The crossbar does the
 * routing, so every widget number goes to it: widget 0 to its registers,
 * the others through its host port, where a number with no live link
 * takes the crossbar's own unreachable or illegal-destination path.
 */
static bool sn1_node_join_supplied_pic_crossbar(SN1NodeState *node,
                                                Error **errp)
{
    SGIPICState *pic = SGI_PIC(node->supplied_pic_crossbar);

    sgi_xio_link_init(&node->xbridge_link, OBJECT(node), "pic-crossbar-link",
                      OBJECT(&SGI_BEDROCK(node->bedrock)->io), 0,
                      OBJECT(pic), 10);
    if (!sgi_xio_link_connect(&node->xbridge_link, errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(pic), errp)) {
        return false;
    }
    for (unsigned int widget = 0; widget < SGI_XIO_WIDGETS; widget++) {
        g_autofree char *property = g_strdup_printf("widget-%u", widget);
        MemoryRegion *view = sgi_pic_enclosure_widget_view(pic, widget, errp);

        if (!view) {
            return false;
        }
        object_property_set_link(OBJECT(node->bedrock), property,
                                 OBJECT(view), &error_abort);
    }
    return true;
}

bool sn1_node_supply_pic_crossbar(SN1NodeState *node, DeviceState *pic,
                                  Error **errp)
{
    if (qdev_is_realized(DEVICE(node)) || node->supplied_xbridge ||
        node->supplied_pic_crossbar) {
        error_setg(errp, "an enclosure crossbar is supplied once, before the "
                   "node realizes");
        return false;
    }
    object_property_add_link(OBJECT(node), "pic-crossbar", TYPE_SGI_PIC,
                             (Object **)&node->supplied_pic_crossbar,
                             qdev_prop_allow_set_link_before_realize,
                             OBJ_PROP_LINK_STRONG);
    return object_property_set_link(OBJECT(node), "pic-crossbar",
                                    OBJECT(pic), errp);
}

bool sn1_node_supply_xbridge(SN1NodeState *node, DeviceState *xbridge,
                             Error **errp)
{
    if (qdev_is_realized(DEVICE(node)) || node->supplied_xbridge ||
        node->supplied_pic_crossbar) {
        error_setg(errp, "an XBridge is supplied once, before the node "
                   "realizes");
        return false;
    }
    /*
     * Added here rather than with the class's properties: an IP45 node's
     * own XBridge is its child of the same name.
     */
    object_property_add_link(OBJECT(node), "xbridge", TYPE_SGI_XBRIDGE,
                             (Object **)&node->supplied_xbridge,
                             qdev_prop_allow_set_link_before_realize,
                             OBJ_PROP_LINK_STRONG);
    return object_property_set_link(OBJECT(node), "xbridge",
                                    OBJECT(xbridge), errp);
}

bool sn1_node_supply_l1(SN1NodeState *node, DeviceState *l1,
                        unsigned int link, Error **errp)
{
    if (qdev_is_realized(DEVICE(node)) || node->supplied_l1) {
        error_setg(errp, "an L1 is supplied once, before the node realizes");
        return false;
    }
    if (link >= SGI_L1_CX_BOARDS || (link && !qdev_is_realized(l1))) {
        error_setg(errp, "an unrealized L1 is supplied on link 0 only");
        return false;
    }
    node->l1_link = link;
    /*
     * Added here rather than with the class's properties: a node's own L1
     * is its child of the same name.
     */
    object_property_add_link(OBJECT(node), "l1", TYPE_SGI_L1,
                             (Object **)&node->supplied_l1,
                             qdev_prop_allow_set_link_before_realize,
                             OBJ_PROP_LINK_STRONG);
    return object_property_set_link(OBJECT(node), "l1", OBJECT(l1), errp);
}

static bool sn1_node_create_l1(SN1NodeState *node, MachineState *machine,
                               DeviceState **l1_uart_out,
                               MemoryRegion **l1_uart_region,
                               Error **errp)
{
    Chardev *l1_console = serial_hd(sgi_sn1_serial_line(node->nasid, 0));
    DeviceState *l1_uart_dev;

    /*
     * A realized supplied L1 is shared with the module's other nodes: the
     * machine configured it, and this node only adds its own UART window
     * on its board's link.
     */
    if (node->supplied_l1 && qdev_is_realized(node->supplied_l1)) {
        node->l1 = node->supplied_l1;
        goto window;
    }
    /*
     * A module-owned L1 is configured exactly as the node's own would be;
     * only its place in the tree differs.
     */
    if (node->supplied_l1) {
        node->l1 = node->supplied_l1;
    } else {
        node->l1 = qdev_new(TYPE_SGI_L1);
        object_property_add_child(OBJECT(node), "l1", OBJECT(node->l1));
    }
    if (node->l1_brick_type) {
        /*
         * A C-brick's L1 profile owns its type letter and takes no record
         * files or firmware revision. It is named by its module's slot.
         */
        if (node->l1_type_code || node->l1_revision[0] >= 0 ||
            node->l1_mac_eeprom) {
            error_setg(errp, TYPE_SGI_SN1_NODE " L1 profile '%s' takes no "
                       "type code, revision or MAC EEPROM",
                       node->l1_brick_type);
            goto fail_l1;
        }
        qdev_prop_set_string(node->l1, "brick-type", node->l1_brick_type);
        if (l1_console) {
            qdev_prop_set_chr(node->l1, "chardev", l1_console);
        }
        if (node->l1_bay) {
            qdev_prop_set_uint32(node->l1, "bay", node->l1_bay);
            qdev_prop_set_bit(node->l1, "named", true);
        }
        if (!qdev_realize(node->l1, NULL, errp)) {
            goto fail_l1;
        }
        goto uart;
    }
    qdev_prop_set_uint32(node->l1, "brick-type-code", node->l1_type_code);
    if (!g_strcmp0(SGI_MACHINE_GET_CLASS(machine)->product, "origin300") &&
        node->l1_type_code == 0x34 &&
        object_property_get_bool(OBJECT(machine), "origin300-l1cmd-compat",
                                 &error_abort)) {
        qdev_prop_set_bit(node->l1, "o300-l1cmd-compat", true);
    }
    if (node->model_node->has_board_kind &&
        node->model_node->board_kind == SGI_MODEL_BOARD_KIND_IP53 &&
        node->l1_irouter_compat) {
        qdev_prop_set_bit(node->l1, "irouter-compat", true);
        qdev_prop_set_uint32(node->l1, "port-speed-first", 1);
    }
    if (node->l1_mac_eeprom) {
        qdev_prop_set_string(node->l1, "fuel-mac-eeprom", node->l1_mac_eeprom);
    }
    if (l1_console) {
        qdev_prop_set_chr(node->l1, "chardev", l1_console);
    }
    if (!sgi_machine_set_l1_identity_records(
            machine, node->l1,
            sgi_machine_bedrock_node_index(machine, DEVICE(node)), errp)) {
        goto fail_l1;
    }
    /*
     * The chassis's brick slot, the operator's setting the L1 keeps in its
     * non-volatile memory (a two-chassis slave carries 02); zero leaves the
     * controller's default.
     */
    if (node->l1_bay) {
        qdev_prop_set_uint32(node->l1, "bay", node->l1_bay);
    }
    qdev_prop_set_int32(node->l1, "revision-major", node->l1_revision[0]);
    qdev_prop_set_int32(node->l1, "revision-minor", node->l1_revision[1]);
    qdev_prop_set_int32(node->l1, "revision-patch", node->l1_revision[2]);
    if (!qdev_realize(node->l1, NULL, errp)) {
        goto fail_l1;
    }

uart:
window:
    l1_uart_dev = qdev_new(TYPE_SGI_L1_UART);
    object_property_add_child(OBJECT(node), "l1-uart", OBJECT(l1_uart_dev));
    qdev_prop_set_uint8(l1_uart_dev, "transport-index", node->l1_link);
    object_property_set_link(OBJECT(l1_uart_dev), "controller",
                             OBJECT(node->l1), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(l1_uart_dev), errp)) {
        object_unparent(OBJECT(l1_uart_dev));
        object_unref(OBJECT(l1_uart_dev));
        goto fail_l1;
    }
    object_unref(OBJECT(l1_uart_dev));
    if (!node->supplied_l1) {
        object_unref(OBJECT(node->l1));
    }
    *l1_uart_region = sysbus_mmio_get_region(SYS_BUS_DEVICE(l1_uart_dev), 0);
    *l1_uart_out = l1_uart_dev;
    return true;

fail_l1:
    if (!node->supplied_l1) {
        object_unparent(OBJECT(node->l1));
        object_unref(OBJECT(node->l1));
    }
    node->l1 = NULL;
    return false;
}

static bool sn1_node_create_bedrock(SN1NodeState *node, MachineState *machine,
                                    Error **errp)
{
    MemoryRegion *system = get_system_memory();
    MemoryRegion *l1_uart;
    DeviceState *l1_uart_dev;
    SysBusDevice *bedrock;
    unsigned int cpu;
    uint32_t slot_order;
    uint32_t grant = 0;
    size_t i;

    if (!sn1_node_create_l1(node, machine, &l1_uart_dev, &l1_uart, errp)) {
        return false;
    }

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
     * Give every fitted slot an initial bus grant. Origin 300 PROM runs
     * support this model approximation; neither it nor Fuel has a known
     * silicon cold-reset value for CPU-enable. Guest writes may change it,
     * and the register survives soft reset. See the Bedrock PI contract.
     */
    for (cpu = 0; cpu < node->nr_cpus; cpu++) {
        grant |= 1U << sn1_cpu_slot(node->model_node, cpu);
    }
    qdev_prop_set_uint32(node->bedrock, "cpu-enable-initial-mask", grant);
    qdev_prop_set_uint8(node->bedrock, "revision", node->bedrock_revision);
    /*
     * A published PROM trace of a four-processor Onyx 350 module prints the
     * Bedrock attached as widget 0xb; Fuel, Origin 300 and Tezro traces print
     * 0xa. No Origin 350 server trace exists, so only the Onyx 350 moves.
     */
    if (node->ip53_assembly_validated &&
        !g_strcmp0(SGI_MACHINE_GET_CLASS(machine)->product, "onyx350")) {
        qdev_prop_set_uint8(node->bedrock, "host-widget", 0xb);
    }
    qdev_prop_set_uint8(node->bedrock, "local-region", node->local_region);
    qdev_prop_set_uint8(node->bedrock, "initiator-region",
                        node->initiator_region);
    qdev_prop_set_uint8(node->bedrock, "installed-bank-mask",
                        node->installed_bank_mask);
    /*
     * An IP35 board's II is a rear connector: with no XIO cable nothing
     * answers on it, and its link reads down (the C-brick datasheet's
     * inference; no physical C-brick has been read uncabled).
     */
    if (sn1_node_is_ip35(node)) {
        qdev_prop_set_bit(node->bedrock, "xio-peer", !!node->supplied_xbridge);
    }
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

    if (node->supplied_xbridge) {
        if (!sn1_node_join_supplied_xbridge(node, errp)) {
            return false;
        }
    } else if (node->supplied_pic_crossbar) {
        if (!sn1_node_join_supplied_pic_crossbar(node, errp)) {
            return false;
        }
    } else if (!node->ip53_assembly_validated &&
               !sn1_node_is_ip35(node) &&
               !sn1_node_create_ip45_xbridge(node, errp)) {
        return false;
    }


    /*
     * Each processor's own view, for the Bedrock's processor I/O path; the
     * path's offsets start at the I/O space.
     */
    for (cpu = 0; cpu < node->nr_cpus; cpu++) {
        g_autofree char *view = g_strdup_printf(
            "cpu-sysmem%u", sn1_cpu_slot(node->model_node, cpu));

        object_property_set_link(OBJECT(node->bedrock), view,
                                 OBJECT(&node->cpu_sysmem[cpu]),
                                 &error_abort);
    }
    qdev_prop_set_uint64(node->bedrock, "io-space-base", SN1_IO_SPACE_ADDRESS);
    for (i = 0; i < node->pic_widget_count; i++) {
        g_autofree char *property =
            g_strdup_printf("widget-%u", node->pic_widgets[i].widget);

        object_property_set_link(OBJECT(node->bedrock), property,
                                 OBJECT(node->pic_widgets[i].face),
                                 &error_abort);
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(node->bedrock), errp)) {
        object_unparent(OBJECT(node->bedrock));
        object_unref(OBJECT(node->bedrock));
        node->bedrock = NULL;
        return false;
    }
    object_unref(OBJECT(node->bedrock));
    bedrock = SYS_BUS_DEVICE(node->bedrock);
    SGI_BEDROCK(node->bedrock)->md.pointer_present = sn1_node_pointer_present;
    SGI_BEDROCK(node->bedrock)->md.requester_opaque = node;

    /*
     * Every uncached I/O-space request a processor makes goes through its
     * own processor interface, which counts graphics credits on the way.
     */
    for (cpu = 0; cpu < node->nr_cpus; cpu++) {
        MemoryRegion *io = sgi_bedrock_processor_io(
            SGI_BEDROCK(node->bedrock), sn1_cpu_slot(node->model_node, cpu));

        if (io && node->cpu_uncached_ready[cpu]) {
            memory_region_add_subregion_overlap(&node->cpu_uncached[cpu],
                                                SN1_IO_SPACE_ADDRESS, io, 2);
        }
    }

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
     * window reaches the processor-interface registers: that is the XIO
     * form's job alone, and the LED offsets used to land on PI interrupt mask
     * 1 B and the cross-call- pending registers when it did. Every other
     * offset is left undecoded.
     */
    memory_region_init_io(&node->hub_unidentified, OBJECT(node),
                          &sn1_hub_unidentified_ops, node,
                          "sn1.hub.unidentified", SN1_HUB_REGISTER_HEAD_SIZE);
    /* These leaf registers can be reached through this node's NI view. */
    node->hub_unidentified.disable_reentrancy_guard = true;
    memory_region_add_subregion(&node->hub_local_registers, 0,
                                &node->hub_unidentified);
    memory_region_add_subregion(&node->hub_local_registers,
                                SN1_HUB_REGISTER_HEAD_SIZE,
                                sysbus_mmio_get_region(bedrock, 8));
    memory_region_init_io(&node->hub_leds, OBJECT(node), &sn1_hub_led_ops,
                          node, "sn1.hub.leds", SN1_HUB_LED_SIZE);
    node->hub_leds.disable_reentrancy_guard = true;
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

    /*
     * The L1's "nmi" output reaches every present processor through the
     * composite's "pi-nmi" inputs, the same path a processor interface's NMI
     * store takes. Whether the real L1 interrupts all processors or only
     * enabled ones, and whether it goes through the processor interface at
     * all, is not established; docs/system/mips/sgi/hardware/bedrock.rst records this as an
     * approximation. The L1 gates the output on node power itself.
     */
    if (node->nr_cpus) {
        DeviceState *nmi_split = qdev_new(TYPE_SPLIT_IRQ);

        qdev_prop_set_uint32(nmi_split, "num-lines", node->nr_cpus);
        object_property_add_child(OBJECT(node), "l1-nmi-split",
                                  OBJECT(nmi_split));
        qdev_realize_and_unref(nmi_split, NULL, &error_fatal);
        qdev_connect_gpio_out_named(node->l1, "nmi", node->l1_link,
                                    qdev_get_gpio_in(nmi_split, 0));
        for (cpu = 0; cpu < node->nr_cpus; cpu++) {
            qdev_connect_gpio_out(
                nmi_split, cpu,
                qdev_get_gpio_in_named(
                    node->bedrock, "pi-nmi",
                    sn1_cpu_slot(node->model_node, cpu)));
        }
    }

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
    return node->xbridge ?
        sgi_xbridge_pci_bus(SGI_XBRIDGE(node->xbridge), widget) : NULL;
}

bool sn1_node_set_ip53_assembly_validated(SN1NodeState *node, Error **errp)
{
    const SGIModelNode *model_node = node->model_node;

    if (qdev_is_realized(DEVICE(node)) || node->bedrock || node->xbridge ||
        node->cpu[0] || node->endpoint.ready ||
        node->ip53_assembly_validated) {
        error_setg(errp, "cannot validate IP53 assembly after node "
                   "construction has started");
        return false;
    }
    if (!model_node || !model_node->has_board_kind ||
        model_node->board_kind != SGI_MODEL_BOARD_KIND_IP53 ||
        model_node->asic_kind != SGI_MODEL_ASIC_KIND_BEDROCK) {
        error_setg(errp, "IP53 assembly validation requires a Bedrock node "
                   "record selecting board kind 'ip53'");
        return false;
    }
    node->ip53_assembly_validated = true;
    return true;
}

bool sn1_node_set_ip53_pic_widget(SN1NodeState *node, unsigned int widget,
                                  MemoryRegion *face, Error **errp)
{
    size_t i;

    if (!node->ip53_assembly_validated || !face ||
        widget >= SGI_XIO_WIDGETS) {
        error_setg(errp, "IP53 widget mapping has an invalid assembly, "
                   "widget, or face");
        return false;
    }
    if (qdev_is_realized(DEVICE(node))) {
        SGIBedrockIOState *io = sn1_node_bedrock_io(node);

        if (!io || node->xbridge) {
            error_setg(errp, "realized IP53 node cannot attach a PIC widget "
                       "without Bedrock I/O or with an XBridge");
            return false;
        }
        return sgi_bedrock_io_set_widget(io, widget, face, errp);
    }
    if (node->bedrock || node->xbridge || node->cpu[0] ||
        node->endpoint.ready) {
        error_setg(errp, "cannot map a PIC widget after node construction "
                   "has started");
        return false;
    }
    if (!face || widget >= SGI_XIO_WIDGETS) {
        error_setg(errp, "IP53 widget mapping has an invalid widget or "
                   "face");
        return false;
    }
    for (i = 0; i < node->pic_widget_count; i++) {
        if (node->pic_widgets[i].widget == widget) {
            error_setg(errp, "IP53 node already has a PIC mapped at widget %u",
                       widget);
            return false;
        }
    }
    node->pic_widgets = g_renew(SN1PicWidget, node->pic_widgets,
                                node->pic_widget_count + 1);
    node->pic_widgets[node->pic_widget_count++] = (SN1PicWidget) {
        .widget = widget,
        .face = face,
    };
    return true;
}

SGIBedrockIOState *sn1_node_bedrock_io(SN1NodeState *node)
{
    if (!node->bedrock || !qdev_is_realized(node->bedrock)) {
        return NULL;
    }
    return &SGI_BEDROCK(node->bedrock)->io;
}

static void sn1_node_realize(DeviceState *dev, Error **errp)
{
    SN1NodeState *node = SN1_NODE(dev);
    MachineState *machine = MACHINE(qdev_get_machine());
    bool ip53 = node->model_node && node->model_node->has_board_kind &&
        node->model_node->board_kind == SGI_MODEL_BOARD_KIND_IP53;

    if (ip53 && !node->ip53_assembly_validated) {
        error_setg(errp, "sgi-sn1-node cannot realize board kind 'ip53'");
        return;
    }
    if (node->ip53_assembly_validated &&
        (!ip53 || node->model_node->asic_kind != SGI_MODEL_ASIC_KIND_BEDROCK)) {
        error_setg(errp, "sgi-sn1-node requires machine validation for an "
                   "IP53 assembly");
        return;
    }
    /* An IP35 board has no XBridge, so none of its riser or IO-8 wiring. */
    if ((node->supplied_xbridge || node->supplied_pic_crossbar) &&
        !sn1_node_is_ip35(node)) {
        error_setg(errp, "sgi-sn1-node takes a supplied XBridge only on "
                   "board kind 'ip35'");
        return;
    }
    if (sn1_node_is_ip35(node) &&
        (node->io_board_part || node->option_intx_selected || node->ni_route ||
         node->xbridge_xio_port != SGI_XBRIDGE_XIO_PORT_DEFAULT)) {
        error_setg(errp, "sgi-sn1-node board kind 'ip35' takes no IO-8, "
                   "option slot, routed XBridge or external port settings");
        return;
    }
    if (!sn1_node_validate_processors(node, machine, errp)) {
        return;
    }
    sn1_node_map_memory(node);
    if (!sn1_node_create_cpus(node, machine, errp)) {
        return;
    }
    if (!sn1_node_create_bedrock(node, machine, errp)) {
        return;
    }
    sn1_node_init_claims(node);
}

static void sn1_node_unrealize(DeviceState *dev)
{
    SN1NodeState *node = SN1_NODE(dev);

    sn1_memory_mappings_stop(node->memory_mappings);
    sn1_option_intx_clear(node);
    if (node->option_intx_inputs) {
        for (unsigned raw = 0; raw < SGI_XBRIDGE_HEAD_RAW_PCI_IRQS; raw++) {
            qdev_connect_gpio_out_named(node->xbridge, "head-e-pci-intx",
                                        raw, NULL);
        }
        qemu_free_irqs(node->option_intx_inputs,
                      SGI_XBRIDGE_HEAD_RAW_PCI_IRQS);
        node->option_intx_inputs = NULL;
    }


    if (node->reset_router_ready) {
        memory_listener_unregister(&node->reset_claim_listener);
        address_space_destroy(&node->reset_claim_as);
        memory_listener_unregister(&node->reset_memory_listener);
        memory_region_del_subregion(get_system_memory(),
                                    MEMORY_REGION(&node->reset_router));
        address_space_destroy(&node->reset_memory_as);
        address_space_destroy(&node->reset_prom_as);
    }
    if (node->ni_boot_initialized) {
        address_space_destroy(&node->ni_boot_as);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(node->ni_md_as); i++) {
        address_space_destroy(&node->ni_md_as[i]);
    }
    address_space_destroy(&node->ni_pio_as);
    address_space_destroy(&node->ni_board_id_as);
    address_space_destroy(&node->ni_uart_as);
    address_space_destroy(&node->ni_hub_as);
    address_space_destroy(&node->ni_ram_as);
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

static int sn1_node_post_load(void *opaque, int version)
{
    SN1NodeState *node = opaque;
    uint8_t levels;

    (void)version;
    if (!node->option_intx_selected) {
        return 0;
    }
    levels = sgi_pci_intx_levels(node->option_intx_map, node->option_intx_raw);
    if (levels != node->option_intx_levels) {
        return -EINVAL;
    }
    /* Equal migrated head levels do not generate a second packet. */
    for (unsigned pin = 0; pin < SGI_PCI_INTX_INPUTS; pin++) {
        qemu_set_irq(node->option_intx_outputs[pin], !!(levels & (1U << pin)));
    }
    return 0;
}

static const VMStateDescription vmstate_sn1_node = {
    .name = "sn1-node",
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = sn1_node_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(board_id_word, SN1NodeState),
        VMSTATE_SINGLE(option_intx_selected, SN1NodeState, 0,
                       vmstate_info_uint8_equal, uint8_t),
        VMSTATE_ARRAY(option_intx_map, SN1NodeState, SGI_PCI_INTX_MAP_SIZE, 0,
                      vmstate_info_uint8_equal, uint8_t),
        VMSTATE_UINT32(option_intx_raw, SN1NodeState),
        VMSTATE_UINT8(option_intx_levels, SN1NodeState),
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
    g_free(node->pic_widgets);
    g_free(node->prom_view_names[0]);
    g_free(node->prom_view_names[1]);
}

static const Property sn1_node_properties[] = {
    DEFINE_PROP_LINK("ram", SN1NodeState, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT32("nr-cpus", SN1NodeState, nr_cpus, 0),
    DEFINE_PROP_UINT32("nasid", SN1NodeState, nasid, 0),
    DEFINE_PROP_UINT32("l1-bay", SN1NodeState, l1_bay, 0),
    DEFINE_PROP_UINT64("board-id-word", SN1NodeState, board_id_word, 0),
    /*
     * The machine sets both from the catalogue's ip35-firmware record;
     * these defaults, Origin 300's, serve only builder test fixtures.
     */
    DEFINE_PROP_UINT32("firmware-machine-type", SN1NodeState,
                       firmware_machine_type, 1),
    DEFINE_PROP_UINT32("firmware-mode-word", SN1NodeState,
                       firmware_mode_word, 0x549aa985),
    /*
     * The catalogue's IP53 runtime profile says whether an IP53 node's L1
     * answers the router-compatible commands and reports a first port
     * speed.
     */
    DEFINE_PROP_BOOL("l1-irouter-compat", SN1NodeState, l1_irouter_compat,
                     false),
    DEFINE_PROP_UINT32("l1-type-code", SN1NodeState, l1_type_code, 0),
    DEFINE_PROP_INT32("l1-revision-major", SN1NodeState, l1_revision[0], -1),
    DEFINE_PROP_INT32("l1-revision-minor", SN1NodeState, l1_revision[1], -1),
    DEFINE_PROP_INT32("l1-revision-patch", SN1NodeState, l1_revision[2], -1),
    DEFINE_PROP_STRING("l1-mac-eeprom", SN1NodeState, l1_mac_eeprom),
    DEFINE_PROP_STRING("l1-brick-type", SN1NodeState, l1_brick_type),
    DEFINE_PROP_UINT8("bedrock-revision", SN1NodeState, bedrock_revision, 0),
    DEFINE_PROP_UINT8("local-region", SN1NodeState, local_region, 0),
    DEFINE_PROP_UINT8("initiator-region", SN1NodeState, initiator_region, 0),
    DEFINE_PROP_UINT8("installed-bank-mask", SN1NodeState,
                      installed_bank_mask, 0),
    DEFINE_PROP_UINT8("io-board-widget", SN1NodeState, io_board_widget, 0),
    DEFINE_PROP_STRING("io-board-part", SN1NodeState, io_board_part),
    DEFINE_PROP_UINT64("io-board-serial", SN1NodeState, io_board_serial,
                       SGI_XBRIDGE_NIC_PLACEHOLDER_SERIAL),
    DEFINE_PROP_UINT8("xbridge-xio-port", SN1NodeState, xbridge_xio_port,
                      SGI_XBRIDGE_XIO_PORT_DEFAULT),
    DEFINE_PROP_STRING("flash-node", SN1NodeState, flash_node),
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
    sn1_option_intx_clear(node);
}

static SN1FabricEndpoint *sn1_node_endpoint(Object *owner)
{
    return &SN1_NODE(owner)->endpoint;
}

static void sn1_node_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    SN1FabricClass *fc = SGI_SN1_FABRIC_CLASS(oc);

    fc->endpoint = sn1_node_endpoint;
    dc->realize = sn1_node_realize;
    dc->unrealize = sn1_node_unrealize;
    dc->vmsd = &vmstate_sn1_node;
    device_class_set_props(dc, sn1_node_properties);
    /* The machine wires a node's RAM and clock; -device cannot. */
    dc->user_creatable = false;
    dc->hotpluggable = false;
    rc->phases.hold = sn1_node_reset_hold;
}

static const TypeInfo sn1_node_types[] = {
    {
        .name = TYPE_SN1_RESET_ROUTER,
        .parent = TYPE_IOMMU_MEMORY_REGION,
        .instance_size = sizeof(IOMMUMemoryRegion),
        .class_init = sn1_reset_router_class_init,
    },
    {
        .name = TYPE_SN1_RESET_LOCAL_ROUTER,
        .parent = TYPE_IOMMU_MEMORY_REGION,
        .instance_size = sizeof(IOMMUMemoryRegion),
        .class_init = sn1_reset_local_router_class_init,
    },
    {
        .name              = TYPE_SGI_SN1_NODE,
        .parent            = TYPE_SYS_BUS_DEVICE,
        .instance_size     = sizeof(SN1NodeState),
        .instance_init     = sn1_node_instance_init,
        .instance_finalize = sn1_node_instance_finalize,
        .class_init        = sn1_node_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_SGI_SN1_FABRIC },
            { }
        },
    },
};

DEFINE_TYPES(sn1_node_types)
