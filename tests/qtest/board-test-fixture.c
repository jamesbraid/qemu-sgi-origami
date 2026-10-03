/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * sgi-board-test-fixture: the smallest stand-in for what a machine builder
 * does with an XIO board or sgi-baseio, so a qtest can build one standalone
 * under `-M none` without a product machine.
 *
 * sgi-xio-baseio and sgi-pci-expansion are onboard-only (user_creatable
 * off: a real system never lets a user plug one in), so `-device` cannot
 * create them directly, the way sgi-xio-board-fixture stands in for a
 * module's Crossbow so sgi-xio-test-board can be tested without one. For
 * these, board-type names the class and this fixture links its "module",
 * "slot" and "fabric" properties to a plain sink region this fixture owns
 * (no interrupt packet in this fixture ever needs to land anywhere; only
 * whether one is attempted matters to a test) and realizes it in its own
 * realize() -- ahead of any machine-init-done notifier, the same ordering
 * sgi-xio-test-board relies on. The board is exposed as this fixture's
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
 *
 * "flash-select0-size" and "flash-select1-size" fit stand-in parts on a
 * Bridge's two external-flash chip selects for board-type=sgi-baseio with
 * a Bridge preset. Their distinct byte patterns let a qtest check the
 * window's decode without assigning either strap value to a board.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/core/boards.h"
#include "hw/mips/sgi/baseio.h"
#include "hw/mips/sgi/sgi-synthetic-serial.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/pci-host/sgi-xbridge.h"
#include "hw/misc/sgi-bedrock-io.h"
#include "hw/mips/sgi/xio-link.h"
#include "hw/pci/pci.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "qom/object.h"
#include "../../hw/mips/sgi/origin300-pci-expansion.h"
#include "../../hw/mips/sgi/sn1-fabric.h"
#include "../../hw/mips/sgi/sn1-node.h"
#include "exec/cpu-common.h"
#include "hw/misc/sgi-sn1-router.h"
#include "system/address-spaces.h"

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
    uint32_t flash_select0_size;
    uint32_t flash_select1_size;
    uint32_t placement;
    uint8_t node_number;
    uint8_t bedrock_revision;
    uint8_t xbridge_revision;
    uint8_t expansion_port[4];
    char *bus_prefix;

    MemoryRegion sink;
    MemoryRegion cascade_leaf;
    bool cascade_ii_late;
    SGIXioLink cascade_link;
    MemoryRegion flash_select0;
    SGIXioLink cascade_host_link;
    MemoryRegion flash_select1;
    uint8_t *flash_select0_bytes;
    uint8_t *flash_select1_bytes;
    SN1FabricProcessorView memory_callback;
    AddressSpace memory_callback_as;
    bool memory_callback_ready;
    MemoryRegion memory_cpu_callback;
    MemoryRegion *memory_cpu_root;
    Object *memory_cpu_owner;
    uint64_t memory_value;
    MemTxResult memory_result;
    bool memory_ram;
};

/* Test-only composition of two XBridge ports, with no graphics device. */
static bool board_test_fixture_realize_cascade(SGIBoardTestFixtureState *s,
                                               bool attach, bool project,
                                               Error **errp)
{
    DeviceState *node = qdev_new(TYPE_SGI_XBRIDGE);
    DeviceState *remote;
    DeviceState *ii = NULL;

    object_property_add_child(OBJECT(s), "chip", OBJECT(node));
    qdev_prop_set_uint8(node, "host-port", 0xa);
    if (project) {
        ii = qdev_new(TYPE_SGI_BEDROCK_IO);
        object_property_add_child(OBJECT(s), "ii", OBJECT(ii));
        object_property_set_link(OBJECT(ii), "dma-memory", OBJECT(&s->sink),
                                 &error_abort);
        object_property_set_link(OBJECT(ii), "hub-registers", OBJECT(&s->sink),
                                 &error_abort);
        sgi_xio_link_init(&s->cascade_host_link, OBJECT(s), "cascade-host",
                          OBJECT(ii), 0, OBJECT(node), 0xa);
        if (!sgi_xio_link_connect(&s->cascade_host_link, errp)) {
            return false;
        }
    } else {
        object_property_set_link(OBJECT(node), "fabric", OBJECT(&s->sink),
                                 &error_abort);
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(node), errp)) {
        return false;
    }
    if (!attach) {
        return true;
    }
    if (project) {
        if (!object_property_set_link(OBJECT(ii), "widget-11",
                OBJECT(sgi_xio_port_cpu_face(OBJECT(node), 0xb)), errp) ||
            (!s->cascade_ii_late &&
             !sysbus_realize(SYS_BUS_DEVICE(ii), errp))) {
            return false;
        }
    }

    remote = qdev_new(TYPE_SGI_XBRIDGE);
    object_property_add_child(OBJECT(s), "remote", OBJECT(remote));
    qdev_prop_set_uint8(remote, "host-port", 0xa);
    if (project) {
        qdev_prop_set_uint8(remote, "xio-port", 8);
    }
    sgi_xio_link_init(&s->cascade_link, OBJECT(s), "cascade-link",
                      OBJECT(node), 0xb, OBJECT(remote), 0xa);
    if (!sgi_xio_link_connect(&s->cascade_link, errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(remote), errp) ||
        !sgi_xio_link_connect(&s->cascade_link, errp)) {
        return false;
    }
    if (!sgi_xio_link_complete(&s->cascade_link)) {
        error_setg(errp, "two-XBridge XIO link is incomplete");
        return false;
    }
    if (project) {
        if (!memory_region_init_ram(&s->cascade_leaf, OBJECT(s),
                                    "cascade-leaf", 8 * MiB, errp) ||
            !sgi_xbridge_attach_xio_port(SGI_XBRIDGE(remote), 8,
                                         &s->cascade_leaf, &s->cascade_leaf,
                                         errp)) {
            return false;
        }
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->cascade_leaf);
        sysbus_init_mmio(SYS_BUS_DEVICE(s),
                         sysbus_mmio_get_region(SYS_BUS_DEVICE(ii), 1));
        sgi_xbridge_project_vbrick_pio(SGI_XBRIDGE(node),
            sgi_xbridge_widget_view(SGI_XBRIDGE(remote), 8),
            sgi_xbridge_widget_view(SGI_XBRIDGE(remote), 0), 8 * MiB);
        if (s->cascade_ii_late &&
            !sysbus_realize(SYS_BUS_DEVICE(ii), errp)) {
            return false;
        }
    }
    return true;
}

/*
 * Each chip select's stand-in part: a byte array behind 16-bit
 * accesses, the width the Bridge's flash port always uses. A RAM region
 * would not do: the port dispatches to the part directly, which only an
 * I/O region answers.
 */
static uint64_t board_test_fixture_flash_read(void *opaque, hwaddr addr,
                                              unsigned size)
{
    uint8_t *bytes = opaque;

    return lduw_be_p(&bytes[addr]);
}

static void board_test_fixture_flash_write(void *opaque, hwaddr addr,
                                           uint64_t value, unsigned size)
{
    uint8_t *bytes = opaque;

    stw_be_p(&bytes[addr], value);
}

static const MemoryRegionOps board_test_fixture_flash_ops = {
    .read = board_test_fixture_flash_read,
    .write = board_test_fixture_flash_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 2,
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
            sgi_baseio_preset_set_bridge_identity(
                chip, identity, SGI_SYNTHETIC_ONEWIRE_SERIAL('B', 0));
        }
        if (s->flash_select0_size) {
            uint32_t i;

            s->flash_select0_bytes = g_malloc(s->flash_select0_size);
            for (i = 0; i < s->flash_select0_size; i++) {
                s->flash_select0_bytes[i] = (i & 0xff) ^ 0x80;
            }
            memory_region_init_io(&s->flash_select0, OBJECT(s),
                                  &board_test_fixture_flash_ops,
                                  s->flash_select0_bytes,
                                  "sgi-board-test-fixture.flash0",
                                  s->flash_select0_size);
            object_property_set_link(OBJECT(chip), "flash",
                                     OBJECT(&s->flash_select0), &error_abort);
        }
        if (s->flash_select1_size) {
            uint32_t i;

            s->flash_select1_bytes = g_malloc(s->flash_select1_size);
            for (i = 0; i < s->flash_select1_size; i++) {
                s->flash_select1_bytes[i] = i & 0xff;
            }
            memory_region_init_io(&s->flash_select1, OBJECT(s),
                                  &board_test_fixture_flash_ops,
                                  s->flash_select1_bytes,
                                  "sgi-board-test-fixture.flash1",
                                  s->flash_select1_size);
            object_property_set_link(OBJECT(chip), "flash-select1",
                                     OBJECT(&s->flash_select1),
                                     &error_abort);
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

static void board_test_fixture_build(DeviceState *dev, Error **errp)
{
    SGIBoardTestFixtureState *s = SGI_BOARD_TEST_FIXTURE(dev);
    DeviceState *board;
    g_autofree char *slot = NULL;

    if (!s->board_type) {
        error_setg(errp, "sgi-board-test-fixture requires board-type");
        return;
    }

    /* Widget-addressed like a link's fabric: bits 51:48 name the widget. */
    memory_region_init(&s->sink, OBJECT(s), "sgi-board-test-fixture.sink",
                       1ULL << 52);

    if (g_str_equal(s->board_type, "sn1-memory")) {
        SN1NodeState *node = SN1_NODE(object_resolve_path("/machine/node1",
                                                       NULL));

        sn1_fabric_init_processor_view(&s->memory_callback, &node->endpoint,
                                       0, 0, &s->sink);
        memory_region_set_enabled(&s->memory_callback.region, true);
        address_space_init(&s->memory_callback_as, &s->sink,
                           "sn1-memory-callback-comparison");
        s->memory_callback_ready = true;
        return;
    }

    if (g_str_equal(s->board_type, "xbridge-cascade") ||
        g_str_equal(s->board_type, "xbridge-cascade-empty") ||
        g_str_equal(s->board_type, "xbridge-cascade-projection")) {
        board_test_fixture_realize_cascade(s,
            !g_str_equal(s->board_type, "xbridge-cascade-empty"),
            g_str_equal(s->board_type, "xbridge-cascade-projection"), errp);
        return;
    }

    if (g_str_equal(s->board_type, TYPE_SGI_BASEIO)) {
        board_test_fixture_realize_baseio(s, errp);
        return;
    }

    if (g_str_equal(s->board_type, TYPE_SGI_ORIGIN300_PCI_EXPANSION)) {
        static const char *const ports[] = {
            "chip1-e-port", "chip1-f-port", "chip2-e-port", "chip2-f-port",
        };
        unsigned i;

        board = qdev_new(s->board_type);
        object_property_add_child(OBJECT(s), "board", OBJECT(board));
        qdev_prop_set_uint32(board, "placement", s->placement);
        qdev_prop_set_uint8(board, "node-number", s->node_number);
        qdev_prop_set_uint8(board, "bedrock-revision", s->bedrock_revision);
        qdev_prop_set_uint8(board, "xbridge-revision", s->xbridge_revision);
        if (s->bus_prefix) {
            qdev_prop_set_string(board, "bus-prefix", s->bus_prefix);
        }
        for (i = 0; i < 4; i++) {
            qdev_prop_set_uint8(board, ports[i], s->expansion_port[i]);
        }
        if (sysbus_realize(SYS_BUS_DEVICE(board), errp)) {
            SN1FabricClass *fc = SGI_SN1_FABRIC_GET_CLASS(board);
            SN1FabricEndpoint *endpoint = fc->endpoint(OBJECT(board));
            SGIOrigin300PciExpansionState *chassis =
                SGI_ORIGIN300_PCI_EXPANSION(board);

            if (endpoint->owner != OBJECT(board) || !endpoint->ready ||
                OBJECT(endpoint->bedrock)->parent != OBJECT(board) ||
                endpoint->placement != s->placement) {
                error_setg(errp, "PCI expansion endpoint ownership is invalid");
                return;
            }
            for (i = 0; i < SGI_ORIGIN300_PCI_HEADS; i++) {
                PCIBus *bus = sgi_origin300_pci_expansion_bus(chassis, i);
                unsigned j;

                if (!bus) {
                    error_setg(errp, "PCI expansion head %u has no bus", i);
                    return;
                }
                for (j = 0; j < i; j++) {
                    if (bus == sgi_origin300_pci_expansion_bus(chassis, j)) {
                        error_setg(errp, "PCI expansion heads share a bus");
                        return;
                    }
                }
            }
            if (sgi_origin300_pci_expansion_bus(chassis,
                                                SGI_ORIGIN300_PCI_HEADS)) {
                error_setg(errp, "PCI expansion exposes an extra bus");
            }
        }
        return;
    }

    board = qdev_new(s->board_type);
    object_property_add_child(OBJECT(s), "board", OBJECT(board));
    qdev_prop_set_uint32(board, "module", s->module);
    slot = g_strdup_printf("%u", s->slot);
    qdev_prop_set_string(board, "slot", slot);
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

/* Build the fixture in one memory transaction, as a machine's builder does. */
static void board_test_fixture_realize(DeviceState *dev, Error **errp)
{
    memory_region_transaction_begin();
    board_test_fixture_build(dev, errp);
    memory_region_transaction_commit();
}

static const Property board_test_fixture_properties[] = {
    DEFINE_PROP_BOOL("cascade-ii-late", SGIBoardTestFixtureState,
                     cascade_ii_late, false),
    DEFINE_PROP_UINT32("placement", SGIBoardTestFixtureState,
                       placement, UINT32_MAX),
    DEFINE_PROP_UINT8("node-number", SGIBoardTestFixtureState,
                      node_number, UINT8_MAX),
    DEFINE_PROP_UINT8("bedrock-revision", SGIBoardTestFixtureState,
                      bedrock_revision, 0),
    DEFINE_PROP_UINT8("xbridge-revision", SGIBoardTestFixtureState,
                      xbridge_revision, 0),
    DEFINE_PROP_STRING("bus-prefix", SGIBoardTestFixtureState, bus_prefix),
    DEFINE_PROP_UINT8("chip1-e-port", SGIBoardTestFixtureState,
                      expansion_port[0], 0),
    DEFINE_PROP_UINT8("chip1-f-port", SGIBoardTestFixtureState,
                      expansion_port[1], 0),
    DEFINE_PROP_UINT8("chip2-e-port", SGIBoardTestFixtureState,
                      expansion_port[2], 0),
    DEFINE_PROP_UINT8("chip2-f-port", SGIBoardTestFixtureState,
                      expansion_port[3], 0),
    DEFINE_PROP_STRING("board-type", SGIBoardTestFixtureState, board_type),
    DEFINE_PROP_STRING("preset", SGIBoardTestFixtureState, preset),
    DEFINE_PROP_UINT32("module", SGIBoardTestFixtureState, module, 0),
    DEFINE_PROP_UINT32("slot", SGIBoardTestFixtureState, slot, 8),
    DEFINE_PROP_STRING("chardev-a", SGIBoardTestFixtureState, chardev_a_id),
    DEFINE_PROP_STRING("nvram-drive", SGIBoardTestFixtureState,
                       nvram_drive_id),
    DEFINE_PROP_UINT32("flash-select0-size", SGIBoardTestFixtureState,
                       flash_select0_size, 0),
    DEFINE_PROP_UINT32("flash-select1-size", SGIBoardTestFixtureState,
                       flash_select1_size, 0),
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

/* Expose transaction status as well as bytes; qtest memory commands omit it. */
static void board_test_fixture_memory_request(Object *obj, const char *request,
                                               Error **errp)
{
    SGIBoardTestFixtureState *s = SGI_BOARD_TEST_FIXTURE(obj);
    uint64_t address, value;
    unsigned size, mode, cca;
    uint8_t bytes[8];
    MemTxAttrs attrs = { .requester_id = 16 };
    AddressSpace *as;

    if (!s->memory_callback_ready ||
        sscanf(request, "%" SCNx64 " %u %" SCNx64 " %u %u",
               &address, &size, &value, &mode, &cca) != 5 ||
        !size || size > sizeof(bytes) || mode > 8 || cca > 7) {
        error_setg(errp, "invalid sn1 memory comparison request");
        return;
    }
    if (mode == 8) {
        uint64_t page = address & ~UINT64_C(0xfff);

        if (s->memory_cpu_root) {
            error_setg(errp, "CPU callback page is already installed");
            return;
        }
        as = cpu_get_address_space(qemu_get_cpu(2), 0);
        s->memory_cpu_root = as->root;
        s->memory_cpu_owner = memory_region_owner(as->root);
        object_ref(s->memory_cpu_owner);
        memory_region_init_alias(&s->memory_cpu_callback, obj,
                                  "sn1-memory-cpu-callback",
                                  &s->memory_callback.region, page, 0x1000);
        memory_region_add_subregion_overlap(as->root, page,
                                             &s->memory_cpu_callback, 2);
        s->memory_result = MEMTX_OK;
        return;
    }
    if (mode == 7) {
        SN1NodeState *node = SN1_NODE(object_resolve_path("/machine/node0",
                                                       NULL));

        sgi_bedrock_md_name_line(&SGI_BEDROCK(node->bedrock)->md, address);
        s->memory_result = MEMTX_OK;
        return;
    }
    if (mode == 6) {
        SGIBedrockState *bic = SGI_BEDROCK(object_resolve_path(
            "/machine/module2/assembly/bic", NULL));

        s->memory_result = sgi_bedrock_lb_hub_access(&bic->lb, address,
                                                     &value, true);
        if (s->memory_result == MEMTX_OK) {
            s->memory_result = sgi_bedrock_lb_hub_access(&bic->lb, address,
                                                         &value, false);
            s->memory_value = value;
        }
        return;
    }
    if (mode >= 4) {
        SGISN1RouterState *router = SGI_SN1_ROUTER(
            object_resolve_path(mode == 4 ? "/machine/module3/router" :
                                "/machine/module4/router", NULL));
        SGINetPacket packet = { .addr = address, .data = value,
                                .op = SGI_NET_PACKET_WRITE };

        s->memory_result = sgi_sn1_router_access(router, 1, &packet);
        return;
    }
    attrs.mips_cca = cca;
    attrs.mips_uncached = cca == 2;
    as = mode & 2 ? &s->memory_callback_as :
        cpu_get_address_space(qemu_get_cpu(2), 0);
    {
        MemoryRegionSection section = memory_region_find(as->root, address,
                                                         size);

        s->memory_ram = section.mr && memory_region_is_ram(section.mr);
        memory_region_unref(section.mr);
    }
    stq_be_p(bytes, value);
    s->memory_result = address_space_rw(as, address, attrs, bytes, size,
                                        mode & 1);
    s->memory_value = ldq_be_p(bytes);
}

static char *board_test_fixture_memory_response(Object *obj, Error **errp)
{
    SGIBoardTestFixtureState *s = SGI_BOARD_TEST_FIXTURE(obj);

    return g_strdup_printf("%u %016" PRIx64 " %u",
                           s->memory_result, s->memory_value, s->memory_ram);
}

static void board_test_fixture_unrealize(DeviceState *dev)
{
    SGIBoardTestFixtureState *s = SGI_BOARD_TEST_FIXTURE(dev);

    if (s->memory_cpu_root) {
        memory_region_del_subregion(s->memory_cpu_root,
                                     &s->memory_cpu_callback);
        object_unparent(OBJECT(&s->memory_cpu_callback));
        s->memory_cpu_root = NULL;
        object_unref(s->memory_cpu_owner);
        s->memory_cpu_owner = NULL;
    }
    if (s->memory_callback_ready) {
        address_space_destroy(&s->memory_callback_as);
        memory_region_del_subregion(&s->sink, &s->memory_callback.region);
        object_unparent(OBJECT(&s->memory_callback.region));
        s->memory_callback_ready = false;
    }
}

static void board_test_fixture_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Board class construction test fixture";
    dc->realize = board_test_fixture_realize;
    dc->unrealize = board_test_fixture_unrealize;
    object_class_property_add_str(oc, "memory-request", NULL,
                                 board_test_fixture_memory_request);
    object_class_property_add_str(oc, "memory-response",
                                 board_test_fixture_memory_response, NULL);
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

/* Keep the probe admission confined to an explicit test machine. */
static bool memory_test_activate;
static void (*memory_test_parent_init)(MachineState *machine);

static void memory_test_set_activate(Object *obj, bool value, Error **errp)
{
    memory_test_activate = value;
}

static void memory_test_init(MachineState *machine)
{
    if (memory_test_activate) {
        memory_test_parent_init(machine);
    }
}

static void memory_test_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    memory_test_parent_init = mc->init;
    mc->init = memory_test_init;
    mc->desc = "Routed CPU memory test fixture";
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_SGI_BOARD_TEST_FIXTURE);
    object_class_property_add_bool(oc, "activate", NULL,
                                   memory_test_set_activate);
}

static const TypeInfo memory_test_type = {
    .name = MACHINE_TYPE_NAME("sn1-memory-test"),
    .parent = MACHINE_TYPE_NAME("origin300"),
    .class_init = memory_test_class_init,
};

static void board_test_fixture_register_types(void)
{
    type_register_static(&board_test_fixture_type);
    type_register_static(&memory_test_type);
}

type_init(board_test_fixture_register_types)
