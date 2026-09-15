/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI crosstalk board base class.
 *
 * A crosstalk board is the removable unit that plugs into one of a
 * module's XIO slots: a Bridge or XBridge carrying PCI functions on a
 * shipped board, or the existing crosstalk test target on the test board
 * this header also declares (see xtalk-board-test.c). Every concrete
 * board shares two properties -- a module index and a slot, the
 * crosstalk widget number the module's Crossbow assigns that position,
 * matching the product's own printed slot (IO1 through IO12 on an
 * Origin 2000 module) -- and one class method, build_widget(), that
 * builds the board's own widget-facing device.
 *
 * ORDERING CONTRACT
 *
 * A `-device` board realizes once the machine's own init path has
 * returned, but a Crossbow needs every one of its endpoints attached
 * before its own realize builds the routes between them --
 * sgi_xbow_attach_endpoint() refuses once the Crossbow is realized. So
 * attachment cannot happen at a board's own realize. Instead:
 *
 *   - A board's realize only validates its properties, builds its
 *     widget, re-parents itself under /machine/module<N> (so info
 *     qom-tree shows it where the hardware puts it), and registers
 *     itself as pending. It never touches a Crossbow.
 *   - The machine builder that owns a module's Crossbow creates it
 *     unrealized, builds every board the topology places directly (order
 *     does not matter: boards attach in build order, not realize order),
 *     then registers a machine-init-done notifier
 *     (qemu_add_machine_init_done_notifier()) that calls
 *     sgi_xtalk_board_attach_pending() once for that Crossbow and
 *     module -- by then every `-device` board has also realized, since
 *     cold-plugged devices are created before machine init is done -- and
 *     realizes the Crossbow only once that call succeeds.
 *   - A board built directly by the machine (onboard I/O, not
 *     user-creatable) can skip the pending list: nothing stops it calling
 *     sgi_xbow_attach_endpoint() itself before the Crossbow realizes, the
 *     way onboard I/O has always attached.
 *   - A board plugged in after the notifier has already run (a
 *     `device_add` sent once the machine is up) still realizes -- its
 *     widget builds and it re-parents under its module -- but nothing
 *     attaches it: the notifier fires once, and calling
 *     sgi_xtalk_board_attach_pending() again against an already-realized
 *     Crossbow fails on the first pending board it finds, the same way a
 *     direct sgi_xbow_attach_endpoint() call would.
 */

#ifndef HW_MIPS_SGI_XTALK_BOARD_H
#define HW_MIPS_SGI_XTALK_BOARD_H

#include "hw/core/sysbus.h"
#include "hw/misc/sgi-xbow.h"
#include "qemu/queue.h"
#include "qom/object.h"

#define TYPE_SGI_XTALK_BOARD "sgi-xtalk-board"
OBJECT_DECLARE_TYPE(SGIXtalkBoardState, SGIXtalkBoardClass, SGI_XTALK_BOARD)

struct SGIXtalkBoardState {
    SysBusDevice parent_obj;

    /*
     * Properties: which module the board plugs into, and which of the
     * module's Crossbow widgets (8 to 15) it occupies. Both are required;
     * a board realized without one refuses.
     */
    uint32_t module;
    uint32_t slot;

    /*
     * Set by the concrete class's build_widget(), before this device
     * re-parents: the two faces sgi_xbow_attach_endpoint() takes. A
     * board whose widget answers a processor and a packet at the same
     * address may point both at the same region.
     */
    MemoryRegion *cpu_face;
    MemoryRegion *packet_face;

    /*
     * Whether sgi_xtalk_board_attach_pending() has already wired this
     * board into a Crossbow.
     */
    bool attached;

    /*< private >*/
    bool queued;
    QTAILQ_ENTRY(SGIXtalkBoardState) node;
};

struct SGIXtalkBoardClass {
    /*< private >*/
    SysBusDeviceClass parent_class;

    /*< public >*/
    /*
     * Build the board's own widget-facing device and record its faces on
     * `board->cpu_face` (and `board->packet_face`, which may be the same
     * region). Called once, from the base class's own realize, before
     * this device re-parents under its module. A widget built here is
     * owned by the board (add it as a QOM child of `board`) so it is
     * released along with a board that never attaches.
     */
    bool (*build_widget)(SGIXtalkBoardState *board, Error **errp);
};

/*
 * Attach every not-yet-attached board registered under `module` to
 * `crossbow`, each at its own slot, and leave the rest for the caller to
 * realize. Stops and returns false on the first board that refuses --
 * ordinarily because `crossbow` is already realized -- exactly the
 * failure sgi_xbow_attach_endpoint() itself reports; a board already
 * marked attached from an earlier call is skipped. See the ordering
 * contract above for when to call this and what a late board sees.
 */
bool sgi_xtalk_board_attach_pending(SGIXbowState *crossbow,
                                    unsigned int module, Error **errp);

#endif /* HW_MIPS_SGI_XTALK_BOARD_H */
