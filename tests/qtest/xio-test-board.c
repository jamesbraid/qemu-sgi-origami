/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * An XIO board that exercises sgi-xio-board without any real board: its
 * widget is the existing sgi-xio-test-target, so property validation and
 * re-parenting have something to build that carries no product-specific
 * wiring. How a board links to its Crossbow port is the real machine's,
 * and tests/qtest/sn-builder-test.c covers it through -M origin2000.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-xbow.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qom/object.h"
#include "system/system.h"

/*
 * hw/misc/sgi-xio-test-target.c declares this type entirely inside its own
 * source file; it has no header because nothing before this file has needed
 * to name it. Only the string is needed here, to create one by type name.
 */
#define TYPE_SGI_XIO_TEST_TARGET "sgi-xio-test-target"

#define TYPE_SGI_XIO_TEST_BOARD "sgi-xio-test-board"
OBJECT_DECLARE_SIMPLE_TYPE(SGIXioTestBoardState, SGI_XIO_TEST_BOARD)

struct SGIXioTestBoardState {
    SGIXioBoardState parent_obj;
};

static bool sgi_xio_test_board_build_widget(SGIXioBoardState *board,
                                            Error **errp)
{
    DeviceState *target = qdev_new(TYPE_SGI_XIO_TEST_TARGET);
    /*
     * sgi-xio-test-target names its RAM block from its own QOM child
     * name, and RAM block names are unique machine-wide, not merely
     * per-parent -- two boards both naming their widget "widget" collide
     * the instant a second board exists. module+slot together are what
     * sgi_xio_board_claim() already guarantees unique.
     */
    g_autofree char *name =
        g_strdup_printf("widget-module%u-slot%u", board->module, board->slot);

    object_property_add_child(OBJECT(board), name, OBJECT(target));
    if (!sysbus_realize(SYS_BUS_DEVICE(target), errp)) {
        object_unparent(OBJECT(target));
        return false;
    }
    board->cpu_face = sysbus_mmio_get_region(SYS_BUS_DEVICE(target), 0);
    board->packet_face = board->cpu_face;
    return true;
}

static void sgi_xio_test_board_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIXioBoardClass *bc = SGI_XIO_BOARD_CLASS(oc);

    dc->desc = "XIO test board, widget sgi-xio-test-target";
    dc->user_creatable = true;
    bc->build_widget = sgi_xio_test_board_build_widget;
}

static const TypeInfo sgi_xio_test_board_type = {
    .name = TYPE_SGI_XIO_TEST_BOARD,
    .parent = TYPE_SGI_XIO_BOARD,
    .instance_size = sizeof(SGIXioTestBoardState),
    .class_init = sgi_xio_test_board_class_init,
};

static void sgi_xio_test_board_register_types(void)
{
    type_register_static(&sgi_xio_test_board_type);
}

type_init(sgi_xio_test_board_register_types)
