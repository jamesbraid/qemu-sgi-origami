/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-board-test-fixture: the smallest stand-in for what a machine builder
 * does with an XIO board or sgi-baseio, so a qtest can build one standalone
 * under `-M none` without a product machine.
 *
 * sgi-xio-baseio and sgi-pci-expansion are onboard-only (user_creatable
 * off: a real system never lets a user plug one in), so `-device` cannot
 * create them directly, the way sgi-xtalk-board-fixture stands in for a
 * module's Crossbow so sgi-xtalk-test-board can be tested without one. For
 * these, board-type names the class and this fixture links its "module",
 * "slot" and "fabric" properties to a plain sink region this fixture owns
 * (no interrupt packet in this fixture ever needs to land anywhere; only
 * whether one is attempted matters to a test) and realizes it in its own
 * realize() -- ahead of any machine-init-done notifier, the same ordering
 * sgi-xtalk-test-board relies on. The board is exposed as this fixture's
 * own "board" QOM child so a qtest can reach its internal Bridge by path.
 *
 * sgi-baseio itself is not a board: it takes a PCI bus by link property,
 * not "module"/"slot"/"fabric" (hw/mips/sgi/baseio.c), the same way every
 * carrier hands it one. board-type=sgi-baseio instead has this fixture
 * build the chip a real carrier would -- a Bridge for the "ip29" and
 * "io6" presets, an XBridge for "io8", cabled to the sink the same way
 * -- and hands sgi-baseio the resulting bus, exposing the chip as this
 * fixture's own "chip" child and sgi-baseio itself as "board", matching
 * every real carrier's own child names (sn0.c, hw/mips/sgi/xio-baseio.c,
 * sn1-node.c).
 *
 * "chardev-a" and "nvram-drive" pass straight through to the same-named
 * property on whichever device this fixture builds against the command
 * line's own chardev and drive backends a second time, inside
 * build_widget()/realize(): a qtest can exercise backend-id resolution
 * (an id a command-line chardev or drive never claimed) the same way a
 * machine builder would hand one down, without needing a whole product
 * machine. Unset by default, like each device's own property.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sgi/baseio.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/pci-host/sgi-xbridge.h"
#include "hw/pci/pci.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qom/object.h"

#define TYPE_SGI_BOARD_TEST_FIXTURE "sgi-board-test-fixture"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBoardTestFixtureState, SGI_BOARD_TEST_FIXTURE)

/* The chip an sgi-baseio preset's own real carrier would build. */
#define IO8_HOST_PORT 0xa
#define IO8_HEAD_F    0xf

struct SGIBoardTestFixtureState {
    SysBusDevice parent_obj;

    char *board_type;
    char *preset;
    uint32_t module;
    uint32_t slot;
    char *chardev_a_id;
    char *nvram_drive_id;

    MemoryRegion sink;
};

/* board-type=sgi-baseio: build the preset's own chip, then sgi-baseio. */
static bool board_test_fixture_realize_baseio(SGIBoardTestFixtureState *s,
                                              Error **errp)
{
    DeviceState *chip, *board;
    PCIBus *bus;

    if (!s->preset) {
        error_setg(errp, "sgi-board-test-fixture requires preset when "
                  "board-type is sgi-baseio");
        return false;
    }

    if (g_str_equal(s->preset, "io8")) {
        chip = qdev_new(TYPE_SGI_XBRIDGE);
        object_property_add_child(OBJECT(s), "chip", OBJECT(chip));
        qdev_prop_set_uint8(chip, "host-port", IO8_HOST_PORT);
        object_property_set_link(OBJECT(chip), "fabric", OBJECT(&s->sink),
                                 &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(chip), errp)) {
            return false;
        }
        bus = sgi_xbridge_pci_bus(SGI_XBRIDGE(chip), IO8_HEAD_F);
    } else {
        const SGIBaseioPresetIdentity *identity =
            sgi_baseio_preset_lookup(s->preset);

        chip = qdev_new(TYPE_SGI_BRIDGE);
        object_property_add_child(OBJECT(s), "chip", OBJECT(chip));
        if (identity) {
            sgi_baseio_preset_set_bridge_identity(chip, identity);
        }
        object_property_set_link(OBJECT(chip), "fabric", OBJECT(&s->sink),
                                 &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(chip), errp)) {
            return false;
        }
        bus = PCI_HOST_BRIDGE(chip)->bus;
    }

    board = qdev_new(TYPE_SGI_BASEIO);
    object_property_add_child(OBJECT(s), "board", OBJECT(board));
    qdev_prop_set_string(board, "preset", s->preset);
    object_property_set_link(OBJECT(board), "pci-bus", OBJECT(bus),
                             &error_abort);
    if (s->chardev_a_id) {
        qdev_prop_set_string(board, "chardev-a", s->chardev_a_id);
    }
    if (s->nvram_drive_id) {
        qdev_prop_set_string(board, "nvram-drive", s->nvram_drive_id);
    }
    return qdev_realize(board, NULL, errp);
}

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

    if (g_str_equal(s->board_type, TYPE_SGI_BASEIO)) {
        board_test_fixture_realize_baseio(s, errp);
        return;
    }

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
    DEFINE_PROP_STRING("preset", SGIBoardTestFixtureState, preset),
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
