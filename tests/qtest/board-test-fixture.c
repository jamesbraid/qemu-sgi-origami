/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-board-test-fixture: the smallest stand-in for what a machine builder
 * does with one of the four board classes, so a qtest can build one of
 * them standalone under `-M none` without a product machine.
 *
 * Every one of sgi-ip29-io, sgi-baseio and sgi-io8 is onboard-only
 * (user_creatable off: a real system never lets a user plug one in), so
 * `-device` cannot create them directly, the way sgi-xtalk-board-fixture
 * stands in for a module's Crossbow so sgi-xtalk-test-board can be tested
 * without one. This fixture plays the same role for the board classes
 * themselves: given a board type name and the module/slot pair a machine
 * would assign it, it builds one, links its required fabric property to a
 * plain sink region this fixture owns (no interrupt packet in this fixture
 * ever needs to land anywhere; only whether one is attempted matters to a
 * test), and realizes it in its own realize() -- ahead of any
 * machine-init-done notifier, the same ordering sgi-xtalk-test-board relies
 * on. The board is exposed as this fixture's own "board" QOM child so a
 * qtest can reach its internal Bridge or XBridge chip by path.
 *
 * "chardev-a" and "nvram-drive" pass straight through to the same-named
 * property on sgi-ip29-io and sgi-baseio, the two board classes that
 * carry them: a qtest can exercise backend-id resolution (an id a
 * command-line chardev or drive never claimed) the same way a machine
 * builder would hand one down, without needing a whole product machine.
 * Unset by default, like the board's own property.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qom/object.h"

#define TYPE_SGI_BOARD_TEST_FIXTURE "sgi-board-test-fixture"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBoardTestFixtureState, SGI_BOARD_TEST_FIXTURE)

struct SGIBoardTestFixtureState {
    SysBusDevice parent_obj;

    char *board_type;
    uint32_t module;
    uint32_t slot;
    char *chardev_a_id;
    char *nvram_drive_id;

    MemoryRegion sink;
};

static void board_test_fixture_realize(DeviceState *dev, Error **errp)
{
    SGIBoardTestFixtureState *s = SGI_BOARD_TEST_FIXTURE(dev);
    DeviceState *board;

    if (!s->board_type) {
        error_setg(errp, "sgi-board-test-fixture requires board-type");
        return;
    }

    memory_region_init(&s->sink, OBJECT(s), "sgi-board-test-fixture.sink",
                       64 * KiB);

    board = qdev_new(s->board_type);
    object_property_add_child(OBJECT(s), "board", OBJECT(board));
    qdev_prop_set_uint32(board, "module", s->module);
    qdev_prop_set_uint32(board, "slot", s->slot);
    object_property_set_link(OBJECT(board), "fabric", OBJECT(&s->sink),
                             &error_abort);
    if (s->chardev_a_id) {
        qdev_prop_set_string(board, "chardev-a", s->chardev_a_id);
    }
    if (s->nvram_drive_id) {
        qdev_prop_set_string(board, "nvram-drive", s->nvram_drive_id);
    }
    sysbus_realize(SYS_BUS_DEVICE(board), errp);
}

static const Property board_test_fixture_properties[] = {
    DEFINE_PROP_STRING("board-type", SGIBoardTestFixtureState, board_type),
    DEFINE_PROP_UINT32("module", SGIBoardTestFixtureState, module, 0),
    DEFINE_PROP_UINT32("slot", SGIBoardTestFixtureState, slot, 8),
    DEFINE_PROP_STRING("chardev-a", SGIBoardTestFixtureState, chardev_a_id),
    DEFINE_PROP_STRING("nvram-drive", SGIBoardTestFixtureState,
                       nvram_drive_id),
};

/* Test-only harness state, not guest-visible; the board carries its own. */
static const VMStateDescription vmstate_board_test_fixture = {
    .name = TYPE_SGI_BOARD_TEST_FIXTURE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void board_test_fixture_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Board class construction test fixture";
    dc->realize = board_test_fixture_realize;
    dc->vmsd = &vmstate_board_test_fixture;
    dc->user_creatable = true;
    device_class_set_props(dc, board_test_fixture_properties);
}

static const TypeInfo board_test_fixture_type = {
    .name = TYPE_SGI_BOARD_TEST_FIXTURE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIBoardTestFixtureState),
    .class_init = board_test_fixture_class_init,
};

static void board_test_fixture_register_types(void)
{
    type_register_static(&board_test_fixture_type);
}

type_init(board_test_fixture_register_types)
