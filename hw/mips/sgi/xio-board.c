/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SGI XIO board base class; see the header for the ordering contract. */

#include "qemu/osdep.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sgi-xtalk-port.h"
#include "hw/pci-host/sgi-bridge.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "qom/object.h"

/*
 * "module" has no sane default: a board placed by widget number refuses
 * without one, and one placed by slot name takes the slot's own.
 */
#define SGI_XIO_BOARD_UNSET SGI_XIO_BOARD_MODULE_UNSET

/*
 * Every sgi-xtalk-board realized so far, attached or not: this is both the
 * module+slot uniqueness registry and the pool sgi_xio_board_attach_pending()
 * scans. Boards are few (a module has at most a handful of XIO slots), so a
 * linear scan costs nothing a hash table would be worth carrying instead.
 */
static QTAILQ_HEAD(, SGIXioBoardState) sgi_xio_board_all =
    QTAILQ_HEAD_INITIALIZER(sgi_xio_board_all);

static Object *sgi_xio_board_module_container(unsigned int module)
{
    Object *machine = qdev_get_machine();
    g_autofree char *name = g_strdup_printf("module%u", module);
    Object *container = object_resolve_path_component(machine, name);

    if (!container) {
        container = object_new(TYPE_CONTAINER);
        object_property_add_child(machine, name, container);
        object_unref(container);
    }
    return container;
}

/*
 * Re-parent this board under its module, the way PowerNV re-parents a
 * user-created PHB under its chip (pnv_parent_fixup() in
 * hw/pci-host/pnv_phb.c): drop the QOM child property device_add gave it and
 * add the same object back under the new parent, so info qom-tree shows the
 * board where the hardware puts it rather than wherever it was created from.
 * A board's `id=`, when given, survives the move.
 *
 * object_unparent() also drops the device's qdev bus parent as a side effect
 * of removing the old QOM child property (pnv_phb.c's comment on
 * pnv_parent_fixup() explains why), so this restores it to the system bus
 * every sgi-xtalk-board is realized on; qdev_machine_creation_done() asserts
 * every realized device still has one.
 */
static bool sgi_xio_board_reparent(SGIXioBoardState *board, Error **errp)
{
    DeviceState *dev = DEVICE(board);
    Object *child = OBJECT(board);
    Object *parent = sgi_xio_board_module_container(board->module);
    g_autofree char *default_name = g_strdup_printf("slot%u", board->slot);
    const char *name = dev->id ? dev->id : default_name;

    if (child->parent == parent) {
        return true;
    }
    object_ref(child);
    object_unparent(child);
    object_property_add_child(parent, name, child);
    object_unref(child);
    return qdev_set_parent_bus(dev, sysbus_get_default(), errp);
}

/* The fabric a board sends through, or NULL for one with none set. */
static Object *sgi_xio_board_fabric(SGIXioBoardState *board)
{
    Object *obj = OBJECT(board);

    return object_property_find(obj, "fabric") ?
        object_property_get_link(obj, "fabric", &error_abort) : NULL;
}

/*
 * Reject a second board on the same widget of the same fabric, and
 * otherwise register this one in the pool sgi_xio_board_attach_pending()
 * reads. A module's two Crossbows number their widgets alike, so the
 * module and widget alone do not name a position; each position's
 * fabric does.
 */
static bool sgi_xio_board_claim(SGIXioBoardState *board, Error **errp)
{
    SGIXioBoardState *other;
    Object *fabric = sgi_xio_board_fabric(board);

    QTAILQ_FOREACH(other, &sgi_xio_board_all, node) {
        if (other->module == board->module && other->slot == board->slot &&
            sgi_xio_board_fabric(other) == fabric) {
            error_setg(errp, "a crosstalk board already occupies module %u "
                       "slot %u", board->module, board->slot);
            return false;
        }
    }
    QTAILQ_INSERT_TAIL(&sgi_xio_board_all, board, node);
    board->queued = true;
    return true;
}

/* The machine's slot interface, or NULL for a machine with no XIO slots. */
static Object *sgi_xio_board_slots(void)
{
    return object_dynamic_cast(qdev_get_machine(), TYPE_SGI_XIO_SLOTS);
}

/*
 * A board placed by slot name: ask the machine where that slot is, and
 * hand the board the fabric the machine holds for it. The board's own
 * "fabric" is left for the machine to choose; a switched fabric carries
 * the destination widget in bits 51:48 of a Bridge's packet address.
 */
static bool sgi_xio_board_place_by_name(SGIXioBoardState *board,
                                        Error **errp)
{
    Object *obj = OBJECT(board);
    Object *slots = sgi_xio_board_slots();
    uint32_t module = board->module;
    MemoryRegion *fabric;

    if (!slots) {
        error_setg(errp, "this machine has no XIO slot named '%s'",
                   board->slot_name);
        return false;
    }
    if (!object_property_find(obj, "fabric")) {
        error_setg(errp, "this board cannot be placed by slot name");
        return false;
    }
    if (object_property_get_link(obj, "fabric", &error_abort)) {
        error_setg(errp, "slot=%s places the board itself; do not also "
                   "give it a fabric", board->slot_name);
        return false;
    }
    fabric = SGI_XIO_SLOTS_GET_CLASS(slots)->claim(
        slots, board->slot_name, &module, &board->slot, errp);
    if (!fabric) {
        return false;
    }
    board->module = module;
    /* A board fitted by slot name is known by that name, unless given one. */
    if (!DEVICE(board)->id) {
        DEVICE(board)->id = g_strdup(board->slot_name);
    }
    if (!object_property_set_link(obj, "fabric", OBJECT(fabric), errp)) {
        return false;
    }
    if (object_property_find(obj, "fabric-widget-shift")) {
        object_property_set_uint(obj, "fabric-widget-shift",
                                 SGI_BRIDGE_PACKET_ADDRESS_BITS, &error_abort);
    }
    return true;
}

/*
 * A board placed by widget number: the caller supplies module and
 * fabric. When that fabric is one of the machine's own slot proxies, the
 * machine holds the board to that slot's module and widget.
 */
static bool sgi_xio_board_place_by_widget(SGIXioBoardState *board,
                                          unsigned int widget, Error **errp)
{
    Object *obj = OBJECT(board);
    Object *slots = sgi_xio_board_slots();
    Object *fabric;

    if (board->module == SGI_XIO_BOARD_UNSET) {
        error_setg(errp, "a crosstalk board placed by widget number requires "
                   "a module index");
        return false;
    }
    if (widget < SGI_XBOW_FIRST_WIDGET || widget >= SGI_XBOW_WIDGETS) {
        error_setg(errp, "a crosstalk board slot must name a Crossbow "
                   "widget, %u to %u, or an XIO slot such as io5",
                   SGI_XBOW_FIRST_WIDGET, SGI_XBOW_WIDGETS - 1);
        return false;
    }
    board->slot = widget;
    fabric = object_property_find(obj, "fabric") ?
        object_property_get_link(obj, "fabric", &error_abort) : NULL;
    if (slots && fabric && object_dynamic_cast(fabric, TYPE_MEMORY_REGION)) {
        return SGI_XIO_SLOTS_GET_CLASS(slots)->claim_fabric(
            slots, MEMORY_REGION(fabric), board->module, widget, errp);
    }
    return true;
}

static void sgi_xio_board_realize(DeviceState *dev, Error **errp)
{
    SGIXioBoardState *board = SGI_XIO_BOARD(dev);
    SGIXioBoardClass *bc = SGI_XIO_BOARD_GET_CLASS(board);
    unsigned int widget;

    if (!board->slot_name) {
        error_setg(errp, "a crosstalk board requires a slot");
        return;
    }
    if (qemu_strtoui(board->slot_name, NULL, 0, &widget) == 0) {
        if (!sgi_xio_board_place_by_widget(board, widget, errp)) {
            return;
        }
    } else if (!sgi_xio_board_place_by_name(board, errp)) {
        return;
    }
    if (!sgi_xio_board_claim(board, errp)) {
        return;
    }
    g_assert(bc->build_widget);
    if (!bc->build_widget(board, errp) ||
        !sgi_xio_board_reparent(board, errp) ||
        (bc->finish_realize && !bc->finish_realize(board, errp))) {
        QTAILQ_REMOVE(&sgi_xio_board_all, board, node);
        board->queued = false;
        return;
    }
}

static void sgi_xio_board_unrealize(DeviceState *dev)
{
    SGIXioBoardState *board = SGI_XIO_BOARD(dev);

    if (board->queued) {
        QTAILQ_REMOVE(&sgi_xio_board_all, board, node);
        board->queued = false;
    }
}

void sgi_xio_board_name_pci_bus(SGIXioBoardState *board, DeviceState *bridge,
                                const char *explicit)
{
    g_autofree char *name = NULL;

    if (explicit) {
        name = g_strdup(explicit);
    } else if (DEVICE(board)->id) {
        name = g_strdup_printf("%s.0", DEVICE(board)->id);
    } else {
        return;
    }
    qdev_prop_set_string(bridge, "pci-bus-name", name);
}

bool sgi_xio_board_attach_pending(SGIXbowState *crossbow,
                                    unsigned int module, Error **errp)
{
    SGIXioBoardState *board;

    QTAILQ_FOREACH(board, &sgi_xio_board_all, node) {
        if (board->module != module || board->attached) {
            continue;
        }
        if (!sgi_xbow_attach_endpoint(crossbow, board->slot, board->cpu_face,
                                      board->packet_face, errp)) {
            return false;
        }
        board->attached = true;
    }
    return true;
}

static const Property sgi_xio_board_properties[] = {
    DEFINE_PROP_UINT32("module", SGIXioBoardState, module,
                       SGI_XIO_BOARD_UNSET),
    DEFINE_PROP_STRING("slot", SGIXioBoardState, slot_name),
};

/*
 * Every field on this base is either a fixed construction-time property
 * (module, slot) or bookkeeping this process derives the same way on every
 * boot (attached, queued, the pending-list linkage); none of it is
 * guest-visible state a migration stream needs to carry. An empty, named
 * description says so explicitly rather than leaving dc->vmsd unset, matching
 * hw/misc/sgi-xtalk-test-target.c.
 */
static const VMStateDescription vmstate_sgi_xio_board = {
    .name = TYPE_SGI_XIO_BOARD,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

/*
 * ---- The crosstalk port interface ----
 *
 * A board is always a leaf: its widget answers a processor and a packet
 * access at the one place build_widget() records, so both faces are whatever
 * it set on cpu_face/packet_face. Both are NULL before this board realizes.
 * index is unused: a board occupies exactly one widget, named by its "slot"
 * property instead.
 */

static const char *sgi_xio_board_xtalk_port_describe(Object *port,
                                                        unsigned int index)
{
    return "a crosstalk board's widget";
}

static MemoryRegion *sgi_xio_board_xtalk_port_cpu_face(Object *port,
                                                          unsigned int index)
{
    return SGI_XIO_BOARD(port)->cpu_face;
}

static MemoryRegion *sgi_xio_board_xtalk_port_packet_face(Object *port,
                                                             unsigned int index)
{
    return SGI_XIO_BOARD(port)->packet_face;
}

static void sgi_xio_board_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIXtalkPortClass *pc = SGI_XTALK_PORT_CLASS(oc);

    dc->desc = "SGI crosstalk board (abstract)";
    dc->realize = sgi_xio_board_realize;
    dc->unrealize = sgi_xio_board_unrealize;
    dc->vmsd = &vmstate_sgi_xio_board;
    device_class_set_props(dc, sgi_xio_board_properties);
    /* Only a concrete subclass is ever instantiated directly. */
    dc->user_creatable = false;
    pc->describe = sgi_xio_board_xtalk_port_describe;
    pc->cpu_face = sgi_xio_board_xtalk_port_cpu_face;
    pc->packet_face = sgi_xio_board_xtalk_port_packet_face;
}

static const TypeInfo sgi_xio_board_types[] = {
    {
        .name = TYPE_SGI_XIO_SLOTS,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(SGIXioSlotsClass),
    },
    {
        .name = TYPE_SGI_XIO_BOARD,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SGIXioBoardState),
        .class_size = sizeof(SGIXioBoardClass),
        .class_init = sgi_xio_board_class_init,
        .abstract = true,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_SGI_XTALK_PORT },
            { }
        },
    },
};

DEFINE_TYPES(sgi_xio_board_types)
