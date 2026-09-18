/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The two SN0 crosstalk link kinds; see the header for the contract.
 * The Hub-side packet adapters and the processor-side refusal face are
 * moved here unaltered in behaviour from the deleted sn0-origin2000-io.c
 * (by way of hw/mips/sgi/sn-machine.c, where the first machine-model
 * rework had relocated them).
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/xtalk-link.h"
#include "hw/mips/sgi-xtalk-port.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-hub.h"
#include "hw/pci-host/sgi-bridge.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "system/runstate.h"
#include "sn0.h"

QEMU_BUILD_BUG_ON(SGI_XTALK_DIRECT_LINK_MAX_ALIASES != SN0_XTALK_WIDGETS);

/* ---- The direct link ---- */

/*
 * The PROM's fixed target: widget 2 (a synthetic address, not a
 * populated widget) at the Bridge's own device-window offset for its
 * error-status decode.
 */
#define SGI_XTALK_DIRECT_LINK_PROBE_WINDOW_OFFSET 0x208ULL

static MemTxResult sgi_xtalk_direct_link_probe_read(void *opaque, hwaddr addr,
                                                     uint64_t *data,
                                                     unsigned int size,
                                                     MemTxAttrs attrs)
{
    SN0NodeState *node = opaque;

    if (addr == 0 && size == 8) {
        *data = 0;
        sgi_hub_latch_prom_pi_error(&node->hub);
        return MEMTX_ERROR;
    }
    return memory_region_dispatch_read(
        node->prom_pi_probe_window,
        SGI_XTALK_DIRECT_LINK_PROBE_WINDOW_OFFSET + addr, data,
        size_memop(size) | MO_BE, attrs);
}

static MemTxResult sgi_xtalk_direct_link_probe_write(void *opaque, hwaddr addr,
                                                      uint64_t data,
                                                      unsigned int size,
                                                      MemTxAttrs attrs)
{
    SN0NodeState *node = opaque;

    return memory_region_dispatch_write(
        node->prom_pi_probe_window,
        SGI_XTALK_DIRECT_LINK_PROBE_WINDOW_OFFSET + addr, data,
        size_memop(size) | MO_BE, attrs);
}

static const MemoryRegionOps sgi_xtalk_direct_link_probe_ops = {
    .read_with_attrs = sgi_xtalk_direct_link_probe_read,
    .write_with_attrs = sgi_xtalk_direct_link_probe_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static bool sgi_xtalk_direct_link_wire_probe(SN0NodeState *node,
                                             Object *widget_port,
                                             unsigned int probe_widget,
                                             uint64_t probe_offset,
                                             Error **errp)
{
    /*
     * The probe is an Origin 200 (IP29) fact only; its one caller
     * (sn0_node_build_baseio()) always hands the direct link a bare
     * Bridge as widget_port -- a fitted part of the node board, not
     * wrapped in any board class (docs/designs/
     * sn-machine-model-architecture.md, "Node devices") -- so this casts
     * it directly rather than resolving a "bridge" child the way a
     * removable board's own wrapper class used to expose one.
     */
    SGIBridgeState *bridge = SGI_BRIDGE(widget_port);
    unsigned int window;

    for (window = 0; window < SGI_BRIDGE_DEVICES; window++) {
        MemoryRegion *candidate = &bridge->device_window_mem[window];

        if (probe_offset >= candidate->addr &&
            probe_offset - candidate->addr < memory_region_size(candidate)) {
            break;
        }
    }
    if (window == SGI_BRIDGE_DEVICES) {
        error_setg(errp, "the PROM PI-error probe offset falls in none of "
                   "the widget's device windows");
        return false;
    }
    node->prom_pi_probe_window = &bridge->device_window_mem[window];
    memory_region_init_io(&node->prom_pi_probe, OBJECT(node),
                          &sgi_xtalk_direct_link_probe_ops, node,
                          "sn0.prom-pi-probe", 8);
    memory_region_add_subregion_overlap(
        &node->xtalk, SN0_XTALK_BASE(probe_widget) + probe_offset,
        &node->prom_pi_probe, 1);
    return true;
}

bool sgi_xtalk_direct_link_connect(SGIXtalkDirectLink *link, Object *owner,
                                    SN0NodeState *node, Object *widget_port,
                                    unsigned int discovery_widget,
                                    unsigned int widget,
                                    uint32_t decoded_widget_mask,
                                    unsigned int probe_widget,
                                    uint64_t probe_offset, Error **errp)
{
    MemoryRegion *hub_face = sgi_xtalk_port_cpu_face(OBJECT(&node->hub), 0);
    MemoryRegion *widget_face = sgi_xtalk_port_cpu_face(widget_port, 0);
    uint64_t widget_size;
    unsigned int seen = 0;
    unsigned int w;

    if (!hub_face) {
        error_setg(errp, "the Hub's crosstalk port has no container to "
                   "install a widget into");
        return false;
    }
    if (!widget_face || !memory_region_size(widget_face)) {
        error_setg(errp, "a direct link's widget must already answer an "
                   "initialized crosstalk face");
        return false;
    }
    if (discovery_widget >= SN0_XTALK_WIDGETS || widget >= SN0_XTALK_WIDGETS) {
        error_setg(errp, "a direct link's widget numbers must be within "
                   "the crosstalk address space");
        return false;
    }
    widget_size = memory_region_size(widget_face);

    for (w = 0; w < SN0_XTALK_WIDGETS; w++) {
        g_autofree char *name = NULL;

        if (w != discovery_widget && w != widget &&
            !(decoded_widget_mask & (1u << w))) {
            continue;
        }
        if (seen >= ARRAY_SIZE(link->alias)) {
            error_setg(errp, "a direct link cannot alias more than %zu "
                       "widget positions", ARRAY_SIZE(link->alias));
            return false;
        }
        name = g_strdup_printf("sn0.xtalk.widget%u", w);
        memory_region_init_alias(&link->alias[seen], owner, name, widget_face,
                                 0, widget_size);
        memory_region_add_subregion(hub_face, SN0_XTALK_BASE(w),
                                    &link->alias[seen]);
        seen++;
    }
    link->alias_count = seen;

    if (probe_widget) {
        return sgi_xtalk_direct_link_wire_probe(node, widget_port,
                                                probe_widget, probe_offset,
                                                errp);
    }
    return true;
}

/* ---- The crossbow link ---- */

/*
 * The packet-addressed access format a switched fabric expects: bit 47
 * set, the destination widget in bits 51:48 (added by the caller, since
 * only it knows whether that is a link index shifted by
 * SGI_XBOW_PACKET_ADDRESS_BITS or a nasid needing a different shift).
 */
#define SGI_XTALK_CROSSBOW_LINK_PACKET_IO (1ULL << 47)

MemoryRegion *sgi_xtalk_crossbow_link_prepare(SGIXtalkCrossbowLink *link,
                                              Object *owner, const char *name)
{
    memory_region_init(&link->proxy, owner, name,
                       SGI_XBOW_WIDGETS * SGI_XBOW_PACKET_SIZE);
    memory_region_set_enabled(&link->proxy, false);
    return &link->proxy;
}

/*
 * The Hub's big-window "xtalk-io" mode issues a widget-addressed access:
 * a widget number and an offset within one SN0_XTALK_WIDGET_SIZE slice,
 * the same shape a small or big window decodes (hw/misc/sgi-hub.c,
 * IO_BWIN_SPACE). A switched fabric instead expects a packet address,
 * the destination widget in bits 51:48; this translates one into the
 * other before the access ever reaches the proxy sgi_xtalk_crossbow_
 * link_prepare() built. Moved unaltered in behaviour from the deleted
 * sn0-origin2000-io.c's SN0Origin2000HubOutgoingIO.
 */
static bool sgi_xtalk_crossbow_link_outbound_bounded(uint64_t offset,
                                                      unsigned size)
{
    return size && size <= SN0_XTALK_WIDGET_SIZE - offset;
}

static MemTxResult sgi_xtalk_crossbow_link_outbound_read(void *opaque,
                                                          hwaddr addr,
                                                          uint64_t *data,
                                                          unsigned size,
                                                          MemTxAttrs attrs)
{
    SGIXtalkCrossbowLink *link = opaque;
    unsigned int widget = addr >> SN0_XTALK_WIDGET_SHIFT;
    uint64_t offset = addr & (SN0_XTALK_WIDGET_SIZE - 1);
    uint8_t bytes[8] = { 0 };
    MemTxResult result;

    *data = 0;
    if (widget < SGI_XBOW_FIRST_WIDGET || widget >= SGI_XBOW_WIDGETS ||
        !sgi_xtalk_crossbow_link_outbound_bounded(offset, size)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sn0: unsupported outgoing Hub I/O read at 0x%"
                      HWADDR_PRIx " size %u\n", addr, size);
        return MEMTX_ERROR;
    }
    result = address_space_read(
        &link->outbound_packets,
        ((uint64_t)widget << SGI_XBOW_PACKET_ADDRESS_BITS) |
        SGI_XTALK_CROSSBOW_LINK_PACKET_IO | offset, attrs, bytes + 8 - size,
        size);
    *data = ldq_be_p(bytes);
    return result;
}

static MemTxResult sgi_xtalk_crossbow_link_outbound_write(void *opaque,
                                                           hwaddr addr,
                                                           uint64_t data,
                                                           unsigned size,
                                                           MemTxAttrs attrs)
{
    SGIXtalkCrossbowLink *link = opaque;
    unsigned int widget = addr >> SN0_XTALK_WIDGET_SHIFT;
    uint64_t offset = addr & (SN0_XTALK_WIDGET_SIZE - 1);
    uint8_t bytes[8];

    if (widget < SGI_XBOW_FIRST_WIDGET || widget >= SGI_XBOW_WIDGETS ||
        !sgi_xtalk_crossbow_link_outbound_bounded(offset, size)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sn0: unsupported outgoing Hub I/O write at 0x%"
                      HWADDR_PRIx " size %u\n", addr, size);
        return MEMTX_ERROR;
    }
    stq_be_p(bytes, data);
    return address_space_write(
        &link->outbound_packets,
        ((uint64_t)widget << SGI_XBOW_PACKET_ADDRESS_BITS) |
        SGI_XTALK_CROSSBOW_LINK_PACKET_IO | offset, attrs, bytes + 8 - size,
        size);
}

static const MemoryRegionOps sgi_xtalk_crossbow_link_outbound_ops = {
    .read_with_attrs = sgi_xtalk_crossbow_link_outbound_read,
    .write_with_attrs = sgi_xtalk_crossbow_link_outbound_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

bool sgi_xtalk_crossbow_link_prepare_hub(SGIXtalkCrossbowLink *link,
                                         Object *owner, const char *name,
                                         SN0NodeState *node, Error **errp)
{
    MemoryRegion *proxy;
    g_autofree char *packets_name =
        g_strdup_printf("%s-outgoing-packets", name);
    g_autofree char *face_name = g_strdup_printf("%s-outgoing-io", name);

    link->is_hub = true;
    link->node = node;
    proxy = sgi_xtalk_crossbow_link_prepare(link, owner, name);
    address_space_init(&link->outbound_packets, proxy, packets_name);
    memory_region_init_io(&link->outbound_face, owner,
                          &sgi_xtalk_crossbow_link_outbound_ops, link,
                          face_name, 1ULL << 38);
    return sgi_hub_set_xtalk_io(&node->hub, &link->outbound_face, errp);
}

/*
 * ---- The Hub-side packet adapters ----
 *
 * Moved unaltered in behaviour from the deleted sn0-origin2000-io.c.
 *
 * The inbound face answers a Crossbow's view of this node's widget
 * position: a packet-space write addressed at the Hub's
 * interrupt-modify register, nasid embedded per the Bridge datasheet's
 * INT_HOST field, decodes to that nasid's physical offset and forwards
 * through this node's device_view, so a remote nasid reaches its own Hub
 * as a direct-attach interrupt does and a local one loops back into this
 * same node.
 *
 * The processor-side face is deliberately narrow: the Hub's own widget-
 * identification word answers its one cleared short read; every other
 * access stops rather than fabricating a value.
 */

#define SGI_XTALK_CROSSBOW_LINK_NASID_SHIFT         38
#define SGI_XTALK_CROSSBOW_LINK_MODIFY_OFFSET       0x90ULL
#define SGI_XTALK_CROSSBOW_LINK_IDENTITY            0x01600000ULL
#define SGI_XTALK_CROSSBOW_LINK_HUB_NETWORK_IDENTITY 0x600000
#define SGI_XTALK_CROSSBOW_LINK_SUPPORTED_NODE_STRIDE (1ULL << 32)
#define SGI_XTALK_CROSSBOW_LINK_SUPPORTED_NASIDS    256

#define SGI_XTALK_CROSSBOW_LINK_HUB_WIDGET_ID       0x400000
#define SGI_XTALK_CROSSBOW_LINK_WIDGET_REGISTER_DATA 4

static bool sgi_xtalk_crossbow_link_packet_address(SGIXtalkCrossbowLink *link,
                                                    hwaddr addr, unsigned size,
                                                    bool write, hwaddr *target)
{
    unsigned nasid = (addr >> SGI_XTALK_CROSSBOW_LINK_NASID_SHIFT) & 0xff;
    uint64_t interrupt = SGI_XTALK_CROSSBOW_LINK_PACKET_IO |
        ((uint64_t)nasid << SGI_XTALK_CROSSBOW_LINK_NASID_SHIFT) |
        link->inbound_modify_offset;

    if (size && addr < link->inbound_memory_size &&
        size <= link->inbound_memory_size - addr) {
        *target = addr;
        return true;
    }
    if (write && size == 8 && addr == interrupt) {
        *target = link->inbound_xtalk_base +
                 (uint64_t)nasid * link->inbound_node_stride +
                 link->inbound_modify_offset;
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sn0: unsupported Hub packet %s at 0x%" HWADDR_PRIx
                  " size %u\n", write ? "write" : "read", addr, size);
    return false;
}

static MemTxResult sgi_xtalk_crossbow_link_inbound_read(void *opaque,
                                                         hwaddr addr,
                                                         uint64_t *data,
                                                         unsigned size,
                                                         MemTxAttrs attrs)
{
    SGIXtalkCrossbowLink *link = opaque;
    uint8_t bytes[8] = { 0 };
    hwaddr target;
    MemTxResult result;
    AddressSpace *space = &link->inbound_target;

    *data = 0;
    if (size == 8 && addr == (SGI_XTALK_CROSSBOW_LINK_PACKET_IO |
                              SGI_XTALK_CROSSBOW_LINK_IDENTITY)) {
        space = &link->inbound_local_registers;
        target = 0;
    } else if (!sgi_xtalk_crossbow_link_packet_address(link, addr, size,
                                                        false, &target)) {
        return MEMTX_ERROR;
    }
    result = address_space_read(space, target, attrs, bytes + 8 - size, size);
    *data = ldq_be_p(bytes);
    return result;
}

static MemTxResult sgi_xtalk_crossbow_link_inbound_write(void *opaque,
                                                          hwaddr addr,
                                                          uint64_t data,
                                                          unsigned size,
                                                          MemTxAttrs attrs)
{
    SGIXtalkCrossbowLink *link = opaque;
    uint8_t bytes[8];
    hwaddr target;

    if (!sgi_xtalk_crossbow_link_packet_address(link, addr, size, true,
                                                &target)) {
        return MEMTX_ERROR;
    }
    stq_be_p(bytes, data);
    return address_space_write(&link->inbound_target, target, attrs,
                               bytes + 8 - size, size);
}

static const MemoryRegionOps sgi_xtalk_crossbow_link_inbound_ops = {
    .read_with_attrs = sgi_xtalk_crossbow_link_inbound_read,
    .write_with_attrs = sgi_xtalk_crossbow_link_inbound_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static MemTxResult sgi_xtalk_crossbow_link_refusal(
    void *opaque, hwaddr addr, uint64_t *data, unsigned size, bool is_write,
    MemTxAttrs attrs)
{
    SGIXtalkCrossbowLink *link = opaque;

    assert(bql_locked());
    if (!is_write && addr == SGI_XTALK_CROSSBOW_LINK_WIDGET_REGISTER_DATA &&
        size == 4) {
        return memory_region_dispatch_read(
            &link->node->hub.regs,
            SGI_XTALK_CROSSBOW_LINK_HUB_WIDGET_ID + addr, data, MO_32 | MO_BE,
            attrs);
    }
    error_report("Origin 2000 node %u: unsupported %u-byte %s at 0x%"
                 HWADDR_PRIx " on the processor-side Hub face, which is "
                 "outside its cleared decode; stopping instead of "
                 "answering", link->node->nasid, size,
                 is_write ? "write" : "read", addr);
    qemu_system_vmstop_request_prepare();
    qemu_system_vmstop_request(RUN_STATE_INTERNAL_ERROR);
    if (data) {
        *data = 0;
    }
    return MEMTX_ERROR;
}

static MemTxResult sgi_xtalk_crossbow_link_refusal_read(void *opaque,
                                                         hwaddr addr,
                                                         uint64_t *data,
                                                         unsigned size,
                                                         MemTxAttrs attrs)
{
    return sgi_xtalk_crossbow_link_refusal(opaque, addr, data, size, false,
                                           attrs);
}

static MemTxResult sgi_xtalk_crossbow_link_refusal_write(void *opaque,
                                                          hwaddr addr,
                                                          uint64_t data,
                                                          unsigned size,
                                                          MemTxAttrs attrs)
{
    return sgi_xtalk_crossbow_link_refusal(opaque, addr, NULL, size, true,
                                           attrs);
}

static const MemoryRegionOps sgi_xtalk_crossbow_link_refusal_ops = {
    .read_with_attrs = sgi_xtalk_crossbow_link_refusal_read,
    .write_with_attrs = sgi_xtalk_crossbow_link_refusal_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

bool sgi_xtalk_crossbow_link_attach_hub(SGIXtalkCrossbowLink *link,
                                        Object *owner, const char *name,
                                        SGIXbowState *xbow, unsigned int widget,
                                        Error **errp)
{
    SN0NodeState *node = link->node;
    const SN0ModelAddressMap *map;
    g_autofree char *refusal_name = g_strdup_printf("%s-cpu-refusal", name);

    if (!node || !DEVICE(node)->realized || !node->model_node) {
        error_setg(errp,
                   "a Hub crossbow attachment requires a realized node");
        return false;
    }
    map = &sn0_hub_address_map;
    if (map->memory.carrier_base != 0 ||
        map->node_stride != SGI_XTALK_CROSSBOW_LINK_SUPPORTED_NODE_STRIDE ||
        map->xtalk.carrier_base != (2ULL << 40) ||
        map->hub_registers_node.offset != 0x01800000 ||
        map->hub_registers_node.size < 0x98) {
        error_setg(errp, "unsupported Hub packet address geometry");
        return false;
    }

    link->inbound_memory_size =
        SGI_XTALK_CROSSBOW_LINK_SUPPORTED_NASIDS * map->node_stride;
    link->inbound_xtalk_base = map->xtalk.carrier_base;
    link->inbound_node_stride = map->node_stride;
    link->inbound_modify_offset = map->hub_registers_node.offset +
        SGI_XTALK_CROSSBOW_LINK_MODIFY_OFFSET;
    memory_region_init_io(&link->inbound_face, owner,
                          &sgi_xtalk_crossbow_link_inbound_ops, link,
                          "sn0.hub.packet", 1ULL << 48);
    address_space_init(&link->inbound_target, &node->device_view,
                       "sn0.hub.packet.target");
    memory_region_init_alias(&link->inbound_local_register_view, owner,
                             "sn0.hub.packet.local-register-view",
                             &node->hub.regs,
                             SGI_XTALK_CROSSBOW_LINK_HUB_NETWORK_IDENTITY, 8);
    address_space_init(&link->inbound_local_registers,
                       &link->inbound_local_register_view,
                       "sn0.hub.packet.local-registers");

    memory_region_init_io(&link->cpu_refusal, owner,
                          &sgi_xtalk_crossbow_link_refusal_ops, link,
                          refusal_name, SN0_XTALK_WIDGET_SIZE);

    return sgi_xbow_attach_endpoint(xbow, widget, &link->cpu_refusal,
                                    &link->inbound_face, errp);
}

bool sgi_xtalk_crossbow_link_attach_widget(SGIXtalkCrossbowLink *link,
                                           Object *widget_port,
                                           SGIXbowState *xbow,
                                           unsigned int widget, Error **errp)
{
    MemoryRegion *cpu_face = sgi_xtalk_port_cpu_face(widget_port, 0);
    MemoryRegion *packet_face = sgi_xtalk_port_packet_face(widget_port, 0);

    link->is_hub = false;
    if (!cpu_face || !packet_face) {
        error_setg(errp, "a widget crossbow attachment must already answer "
                   "an initialized crosstalk face");
        return false;
    }
    return sgi_xbow_attach_endpoint(xbow, widget, cpu_face, packet_face,
                                    errp);
}

bool sgi_xtalk_crossbow_link_finish(SGIXtalkCrossbowLink *link, Object *owner,
                                    const char *name, SGIXbowState *xbow,
                                    unsigned int widget, Error **errp)
{
    MemoryRegion *packet = sgi_xtalk_port_packet_face(OBJECT(xbow), widget);
    g_autofree char *packet_name = g_strdup_printf("%s-packet-ingress", name);

    if (!packet) {
        error_setg(errp, "the crossbar has no packet ingress for widget %u "
                   "yet", widget);
        return false;
    }
    memory_region_init_alias(&link->proxy_alias, owner, packet_name, packet,
                             0, memory_region_size(packet));
    memory_region_add_subregion(&link->proxy, 0, &link->proxy_alias);
    memory_region_set_enabled(&link->proxy, true);

    if (link->is_hub) {
        MemoryRegion *cpu = sgi_xtalk_port_cpu_face(OBJECT(xbow), widget);
        g_autofree char *cpu_name = g_strdup_printf("%s-cpu-ingress", name);

        if (!cpu) {
            error_setg(errp, "the crossbar has no cpu ingress for widget "
                       "%u yet", widget);
            return false;
        }
        memory_region_init_alias(&link->cpu_alias, owner, cpu_name, cpu, 0,
                                 memory_region_size(cpu));
        memory_region_add_subregion(&link->node->xtalk, 0, &link->cpu_alias);
    }
    return true;
}
