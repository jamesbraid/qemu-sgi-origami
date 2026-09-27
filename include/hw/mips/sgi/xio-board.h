/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI XIO board base class: the removable unit that plugs into one of a
 * module's XIO slots (the BaseIO card, the PCI expansion card, a future
 * GIGAchannel cabinet), each carrying a Bridge or XBridge widget of its own.
 * A board that is not removable (IP29's onboard I/O, soldered to the System
 * Board rather than plugged into a slot) is not one of these; its Bridge
 * answers the crosstalk port interface directly (see
 * hw/pci-host/sgi-bridge.c), with no board wrapper at all.
 *
 * Renamed from sgi-xtalk-board (the base-io-device step) once its only
 * onboard-only subclasses (sgi-ip29-io, sgi-io8) stopped existing: what is
 * left is exactly the removable-card scope this name states, plus the test
 * board this header also declares (see xtalk-test-board.c).
 *
 * Every concrete board shares two properties -- a module index and a
 * slot -- and one class method, build_widget(), that builds the board's
 * own widget-facing device.
 *
 * ORDERING CONTRACT
 *
 * A Crossbow needs every endpoint present at startup attached before its
 * own realize builds the routes between them, and sgi_xbow_attach_endpoint()
 * refuses once it is realized. So:
 *
 *   - The machine builder attaches the boards its topology places, through
 *     each board's own crossbow link, before it realizes the Crossbow.
 *   - A `-device` board realizes after the machine's init has returned.
 *     Named by slot (slot=io5), its realize asks the machine
 *     (TYPE_SGI_XIO_SLOTS below) for that empty slot's module, widget and
 *     fabric proxy, validates its properties, builds its widget, claims the
 *     proxy and re-parents itself under /machine/module<N>. It never
 *     touches a Crossbow.
 *   - The machine's init-done notifier then finishes every slot's proxy
 *     against its realized Crossbow and, for a claimed one, attaches the
 *     board's widget live and brings its link alive.
 *   - sgi_xio_board_attach_pending() remains for a machine that realizes
 *     its Crossbow only after cold-plugged boards exist, as a qtest
 *     fixture does; a board plugged in with device_add once the machine
 *     is up still realizes but attaches nowhere.
 */

#ifndef HW_MIPS_SGI_XIO_BOARD_H
#define HW_MIPS_SGI_XIO_BOARD_H

#include "hw/core/sysbus.h"
#include "hw/misc/sgi-xbow.h"
#include "qemu/queue.h"
#include "qom/object.h"

#define TYPE_SGI_XIO_BOARD "sgi-xtalk-board"
OBJECT_DECLARE_TYPE(SGIXioBoardState, SGIXioBoardClass, SGI_XIO_BOARD)

struct SGIXioBoardState {
    SysBusDevice parent_obj;

    /*
     * Properties: which module the board plugs into, and "slot", which
     * XIO position it occupies. "slot" is either an IRIX slot name (io5),
     * which the machine resolves to a module, a Crossbow widget and the
     * fabric the board sends through, or a Crossbow widget number (8 to
     * 15), which needs "module" and the board's own "fabric" set too, as
     * the machine builder and qtest fixtures do.
     */
    uint32_t module;
    char *slot_name;

    /* The Crossbow widget the board occupies, resolved from "slot". */
    uint32_t slot;

    /* Whether "slot" named an XIO slot (io5) rather than a widget number. */
    bool placed_by_name;

    /*
     * For a board placed by name, the slot's IRIX label (io5), whatever
     * module prefix "slot" was typed with.
     */
    char *slot_label;

    /*
     * Set by the concrete class's build_widget(), before this device
     * re-parents: the two faces sgi_xbow_attach_endpoint() takes. A board
     * whose widget answers a processor and a packet at the same address may
     * point both at the same region.
     */
    MemoryRegion *cpu_face;
    MemoryRegion *packet_face;

    /*
     * Whether sgi_xio_board_attach_pending() has already wired this board
     * into a Crossbow.
     */
    bool attached;

    /* < private > */
    bool queued;
    QTAILQ_ENTRY(SGIXioBoardState) node;
};

struct SGIXioBoardClass {
    /* < private > */
    SysBusDeviceClass parent_class;

    /* < public > */
    /*
     * Build the board's widget-facing device and record its faces on
     * `board->cpu_face` (and `board->packet_face`, which may be the same
     * region). Called once, from the base class's realize, before this device
     * re-parents under its module. A widget built here is owned by the board
     * (add it as a QOM child of `board`) so it is released along with a board
     * that never attaches.
     *
     * Do not create a qdev bus directly on `board` here (onewire_create_bus(
     * DEVICE(board), ...) or similar): the base class's realize still has to
     * move this device under its module after build_widget() returns, and
     * object_unparent() tears down every child bus a device owns as a side
     * effect of that move. Use finish_realize() instead for anything that
     * needs a child bus of its own.
     */
    bool (*build_widget)(SGIXioBoardState *board, Error **errp);

    /*
     * Optional. Runs once build_widget() has succeeded and this device has
     * already re-parented under its module, so a qdev bus created here
     * (again, DEVICE(board) is now a safe parent) is never subject to the
     * child-bus teardown a later re-parent would otherwise inflict on it.
     * There is only ever one. Most boards need nothing here; BaseIO's Bridge
     * child, for one, already owns its one-wire bus on itself rather than on
     * the board, so it never needed this hook.
     */
    bool (*finish_realize)(SGIXioBoardState *board, Error **errp);
};

/*
 * The machine side of a board placed by slot name: a machine whose
 * topology names XIO slots implements this interface, and a board's
 * realize asks it, before build_widget(), where the named slot is and
 * what the board sends through. claim() reserves an empty slot by name,
 * filling in an SGIXioSlotPlace and returning the fabric proxy the
 * machine holds for it; it refuses a slot the topology does not have,
 * one the topology already fills and one another board has claimed.
 * claim_fabric() covers a board given its fabric outright: when that
 * fabric is one of the machine's slot proxies it must match the module
 * and widget the board names and must not already be claimed; any other
 * fabric (a qtest fixture's sink) passes.
 */
#define TYPE_SGI_XIO_SLOTS "sgi-xio-slots"

/* claim()'s module when the board named none: any module's slot will do. */
#define SGI_XIO_BOARD_MODULE_UNSET UINT32_MAX
typedef struct SGIXioSlotsClass SGIXioSlotsClass;
DECLARE_CLASS_CHECKERS(SGIXioSlotsClass, SGI_XIO_SLOTS, TYPE_SGI_XIO_SLOTS)

/*
 * Where a claimed slot is. `module` goes in as the module the board
 * named, or SGI_XIO_BOARD_MODULE_UNSET, and comes back as the slot's own.
 * `position` numbers the machine's XIO slots uniquely, where the widget
 * does not: a module's two Crossbows number their widgets alike. `name`
 * is the slot's machine-wide name (io5, or m<N>-io5 where two modules
 * share the label) and `label` its IRIX label (io5); the caller frees
 * both.
 */
typedef struct SGIXioSlotPlace {
    uint32_t module;
    uint32_t widget;
    uint32_t position;
    char *name;
    char *label;
} SGIXioSlotPlace;

struct SGIXioSlotsClass {
    /*< private >*/
    InterfaceClass parent_class;

    /*< public >*/
    MemoryRegion *(*claim)(Object *slots, const char *name,
                           SGIXioSlotPlace *place, Error **errp);
    bool (*claim_fabric)(Object *slots, MemoryRegion *fabric,
                         uint32_t module, uint32_t widget, Error **errp);
};

/*
 * Name the PCI bus a board's own Bridge creates: `explicit` when the board
 * was given a "pci-bus-name", else QEMU's child-bus idiom <id>.0 from the
 * board's own qdev id (io5.0 for a board in IO5), else the Bridge's own
 * default.
 */
void sgi_xio_board_name_pci_bus(SGIXioBoardState *board, DeviceState *bridge,
                                const char *explicit);

/*
 * Attach every not-yet-attached board registered under `module` to
 * `crossbow`, each at its slot, and leave the rest for the caller to realize.
 * Stops and returns false on the first board that refuses, ordinarily because
 * `crossbow` is already realized. This is the failure
 * sgi_xbow_attach_endpoint() itself reports; a board already marked attached
 * from an earlier call is skipped. See the ordering contract above for when
 * to call this and what a late board sees.
 */
bool sgi_xio_board_attach_pending(SGIXbowState *crossbow,
                                    unsigned int module, Error **errp);

#endif /* HW_MIPS_SGI_XIO_BOARD_H */
