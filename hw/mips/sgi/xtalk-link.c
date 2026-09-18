/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The SN0 crosstalk link kinds; see the header for the contract.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/xtalk-link.h"
#include "hw/mips/sgi-xtalk-port.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-hub.h"
#include "hw/pci-host/sgi-bridge.h"
#include "qapi/error.h"
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
    SGIBridgeState *bridge;
    unsigned int window;

    bridge = SGI_BRIDGE(object_resolve_path_component(widget_port, "bridge"));
    if (!bridge) {
        error_setg(errp, "the PROM PI-error probe needs a \"bridge\" child "
                   "on the widget it targets");
        return false;
    }
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
