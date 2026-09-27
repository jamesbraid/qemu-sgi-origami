/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A crosstalk board that exercises sgi-xio-board without any real
 * board: its widget is the existing sgi-xtalk-test-target, so property
 * validation, re-parenting and the deferred-attach hook all have
 * something to attach that carries no product-specific wiring.
 *
 * The second type here, sgi-xtalk-board-fixture, is not a board. It is
 * the smallest stand-in for what a machine builder does with a module's
 * Crossbow: it creates one unrealized, registers the machine-init-done
 * notifier that calls sgi_xio_board_attach_pending() and realizes the
 * Crossbow once that succeeds, and reports the outcome through two
 * read-only properties so a qtest can check it without a real machine.
 * Its own "retry" property lets a qtest run that same attempt a second
 * time, to see what a board plugged in after the notifier already fired
 * gets: see tests/qtest/sgi-board-test.c.
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
 * hw/misc/sgi-xtalk-test-target.c declares this type entirely inside its own
 * source file; it has no header because nothing before this file has needed
 * to name it. Only the string is needed here, to create one by type name.
 */
#define TYPE_SGI_XTALK_TEST_TARGET "sgi-xtalk-test-target"

#define TYPE_SGI_XTALK_TEST_BOARD "sgi-xtalk-test-board"
OBJECT_DECLARE_SIMPLE_TYPE(SGIXtalkTestBoardState, SGI_XTALK_TEST_BOARD)

struct SGIXtalkTestBoardState {
    SGIXioBoardState parent_obj;
};

static bool sgi_xtalk_test_board_build_widget(SGIXioBoardState *board,
                                              Error **errp)
{
    DeviceState *target = qdev_new(TYPE_SGI_XTALK_TEST_TARGET);
    /*
     * sgi-xtalk-test-target names its RAM block from its own QOM child
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

static void sgi_xtalk_test_board_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIXioBoardClass *bc = SGI_XIO_BOARD_CLASS(oc);

    dc->desc = "Crosstalk test board, widget sgi-xtalk-test-target";
    dc->user_creatable = true;
    bc->build_widget = sgi_xtalk_test_board_build_widget;
}

static const TypeInfo sgi_xtalk_test_board_type = {
    .name = TYPE_SGI_XTALK_TEST_BOARD,
    .parent = TYPE_SGI_XIO_BOARD,
    .instance_size = sizeof(SGIXtalkTestBoardState),
    .class_init = sgi_xtalk_test_board_class_init,
};

#define TYPE_SGI_XTALK_BOARD_FIXTURE "sgi-xtalk-board-fixture"
OBJECT_DECLARE_SIMPLE_TYPE(SGIXtalkBoardFixtureState, SGI_XTALK_BOARD_FIXTURE)

struct SGIXtalkBoardFixtureState {
    SysBusDevice parent_obj;

    uint32_t module;
    bool auto_attach;
    SGIXbowState *crossbow;
    Notifier machine_done;

    /*
     * A successful Crossbow realization stays true across retries; checking a
     * late board cannot undo it. `error` records only the latest attempt's
     * failure.
     */
    bool attached;
    char *error;
};

/*
 * Wide enough to hold the test target's largest RAM (16 MiB, see
 * hw/misc/sgi-xtalk-test-target.c) inside one widget's window.
 */
#define SGI_XTALK_BOARD_FIXTURE_SPAN (32 * MiB)

/*
 * Widget 8 holds the attached board; widget 9 probes it because a link cannot
 * route to itself. "alive" enables port memory regions regardless of whether
 * sgi_xio_board_attach_pending() attached a board at either port.
 */
#define SGI_XTALK_BOARD_FIXTURE_HEALTH ((1u << 8) | (1u << 9))

/*
 * qdev_machine_creation_done() requires every QOM-root device to be realized
 * before machine-init-done notifiers run. The Crossbow cannot be parented in
 * fixture realize(); it remains a floating, singly referenced object visible
 * to leak checks until immediately before its own realization.
 */
static void sgi_xtalk_board_fixture_own_crossbow(SGIXtalkBoardFixtureState *s)
{
    if (!OBJECT(s->crossbow)->parent) {
        object_property_add_child(OBJECT(s), "crossbow", OBJECT(s->crossbow));
        object_unref(OBJECT(s->crossbow));
    }
}

static void sgi_xtalk_board_fixture_attempt(SGIXtalkBoardFixtureState *s)
{
    Error *local_err = NULL;
    bool realized;

    sgi_xtalk_board_fixture_own_crossbow(s);
    realized = DEVICE(s->crossbow)->realized;

    g_free(s->error);
    s->error = NULL;
    if (sgi_xio_board_attach_pending(s->crossbow, s->module, &local_err) &&
        (realized || sysbus_realize(SYS_BUS_DEVICE(s->crossbow),
                                    &local_err))) {
        s->attached = true;
        return;
    }
    s->error = g_strdup(error_get_pretty(local_err));
    error_free(local_err);
}

static void sgi_xtalk_board_fixture_machine_done(Notifier *notifier,
                                                  void *data)
{
    SGIXtalkBoardFixtureState *s =
        container_of(notifier, SGIXtalkBoardFixtureState, machine_done);

    sgi_xtalk_board_fixture_attempt(s);
}

static bool sgi_xtalk_board_fixture_get_attached(Object *obj, Error **errp)
{
    return SGI_XTALK_BOARD_FIXTURE(obj)->attached;
}

static char *sgi_xtalk_board_fixture_get_error(Object *obj, Error **errp)
{
    SGIXtalkBoardFixtureState *s = SGI_XTALK_BOARD_FIXTURE(obj);

    return g_strdup(s->error ? s->error : "");
}

static void sgi_xtalk_board_fixture_set_retry(Object *obj, bool value,
                                              Error **errp)
{
    sgi_xtalk_board_fixture_attempt(SGI_XTALK_BOARD_FIXTURE(obj));
}

/*
 * Direct realization skips sgi_xio_board_attach_pending() to reach an
 * already-realized, unattached Crossbow. -M none rejects the late-board
 * hotplug that would otherwise reach it. Use with auto-attach=false and
 * "retry".
 */
static void sgi_xtalk_board_fixture_set_realize_now(Object *obj, bool value,
                                                     Error **errp)
{
    SGIXtalkBoardFixtureState *s = SGI_XTALK_BOARD_FIXTURE(obj);
    Error *local_err = NULL;

    sgi_xtalk_board_fixture_own_crossbow(s);
    g_free(s->error);
    s->error = NULL;
    if (sysbus_realize(SYS_BUS_DEVICE(s->crossbow), &local_err)) {
        s->attached = true;
        return;
    }
    s->error = g_strdup(error_get_pretty(local_err));
    error_free(local_err);
}

static void sgi_xtalk_board_fixture_instance_init(Object *obj)
{
    object_property_add_bool(obj, "attached",
                             sgi_xtalk_board_fixture_get_attached, NULL);
    object_property_add_str(obj, "error",
                            sgi_xtalk_board_fixture_get_error, NULL);
    object_property_add_bool(obj, "retry", NULL,
                             sgi_xtalk_board_fixture_set_retry);
    object_property_add_bool(obj, "realize-now", NULL,
                             sgi_xtalk_board_fixture_set_realize_now);
}

static void sgi_xtalk_board_fixture_realize(DeviceState *dev, Error **errp)
{
    SGIXtalkBoardFixtureState *s = SGI_XTALK_BOARD_FIXTURE(dev);
    DeviceState *crossbow = qdev_new(TYPE_SGI_XBOW);

    /*
     * Built but deliberately not yet a QOM child of anything: see
     * sgi_xtalk_board_fixture_own_crossbow(), which parents it right before
     * it is actually realized, never here.
     */
    s->crossbow = SGI_XBOW(crossbow);

    qdev_prop_set_uint64(crossbow, "span", SGI_XTALK_BOARD_FIXTURE_SPAN);
    qdev_prop_set_uint16(crossbow, "links-present",
                         SGI_XTALK_BOARD_FIXTURE_HEALTH);
    qdev_prop_set_uint16(crossbow, "startup-links-alive",
                         SGI_XTALK_BOARD_FIXTURE_HEALTH);

    /*
     * Defer Crossbow realization to machine-init-done attach_pending, as a
     * real builder does. With auto-attach=false, only qtest "realize-now" and
     * "retry" run either step. This reaches an already-realized, unattached
     * Crossbow with all devices on the initial command line.
     */
    if (s->auto_attach) {
        s->machine_done.notify = sgi_xtalk_board_fixture_machine_done;
        qemu_add_machine_init_done_notifier(&s->machine_done);
    }
}

static const Property sgi_xtalk_board_fixture_properties[] = {
    DEFINE_PROP_UINT32("module", SGIXtalkBoardFixtureState, module, 0),
    DEFINE_PROP_BOOL("auto-attach", SGIXtalkBoardFixtureState, auto_attach,
                     true),
};

/*
 * Test-only harness state, not guest-visible; see the same reasoning on
 * sgi-xio-board's own empty description. The embedded Crossbow carries its
 * own.
 */
static const VMStateDescription vmstate_sgi_xtalk_board_fixture = {
    .name = TYPE_SGI_XTALK_BOARD_FIXTURE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void sgi_xtalk_board_fixture_class_init(ObjectClass *oc,
                                               const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Crosstalk board deferred-attach test fixture";
    dc->realize = sgi_xtalk_board_fixture_realize;
    dc->vmsd = &vmstate_sgi_xtalk_board_fixture;
    dc->user_creatable = true;
    device_class_set_props(dc, sgi_xtalk_board_fixture_properties);
}

static const TypeInfo sgi_xtalk_board_fixture_type = {
    .name = TYPE_SGI_XTALK_BOARD_FIXTURE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIXtalkBoardFixtureState),
    .instance_init = sgi_xtalk_board_fixture_instance_init,
    .class_init = sgi_xtalk_board_fixture_class_init,
};

static void sgi_xtalk_test_board_register_types(void)
{
    type_register_static(&sgi_xtalk_test_board_type);
    type_register_static(&sgi_xtalk_board_fixture_type);
}

type_init(sgi_xtalk_test_board_register_types)
