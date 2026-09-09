/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/mips/sn0-origin2000-xtalk.h"

typedef struct Fixture {
    SN0ModelComposition model;
    SN0ModelPhysicalNode nodes[4];
    SN0ModelComponent hubs[4];
    SN0ModelModule module;
    unsigned int module_nodes[4];
    SN0ModelCrossbow crossbows[2];
    SN0ModelXtalkAttachment attachments[4];
    SN0ModelIoBoard board;
    SN0ModelComponent bridge;
    SN0ModelComponent pci[3];
} Fixture;

static void fixture_init(Fixture *f)
{
    *f = (Fixture) { 0 };
    f->model = (SN0ModelComposition) {
        .nodes = f->nodes, .node_count = 4,
        .modules = &f->module, .module_count = 1,
        .crossbows = f->crossbows, .crossbow_count = 2,
        .xtalk_attachments = f->attachments, .xtalk_attachment_count = 4,
        .io_boards = &f->board, .io_board_count = 1,
    };
    f->module.id = "module-1";
    f->module.nodes = f->module_nodes;
    f->module.node_count = 4;
    for (unsigned int i = 0; i < 4; i++) {
        f->module_nodes[i] = i;
        f->nodes[i].module_index = 0;
        f->hubs[i].kind = SN0_COMPONENT_HUB;
        f->nodes[i].hub = &f->hubs[i];
    }
    f->nodes[0].id = "node-board-1";
    f->nodes[1].id = "node-board-2";
    f->nodes[2].id = "node-board-3";
    f->nodes[3].id = "node-board-4";
    f->attachments[0] = (SN0ModelXtalkAttachment) {
        .crossbow_index = 0, .widget = 9, .node_index = 0,
    };
    f->attachments[1] = (SN0ModelXtalkAttachment) {
        .crossbow_index = 0, .widget = 10, .node_index = 2,
    };
    f->attachments[2] = (SN0ModelXtalkAttachment) {
        .crossbow_index = 1, .widget = 9, .node_index = 3,
    };
    f->attachments[3] = (SN0ModelXtalkAttachment) {
        .crossbow_index = 1, .widget = 10, .node_index = 1,
    };
    f->crossbows[0] = (SN0ModelCrossbow) {
        .id = "crossbow-0", .module_index = 0,
        .initially_operational_links = 0x700,
    };
    f->crossbows[1] = (SN0ModelCrossbow) {
        .id = "crossbow-1", .module_index = 0,
        .initially_operational_links = 0x600,
    };
    f->bridge = (SN0ModelComponent) {
        .kind = SN0_COMPONENT_BASE_IO, .widget = 8,
    };
    f->pci[0] = (SN0ModelComponent) {
        .kind = SN0_COMPONENT_ISP1040, .device = 0,
    };
    f->pci[1] = (SN0ModelComponent) {
        .kind = SN0_COMPONENT_ISP1040, .device = 1,
    };
    f->pci[2] = (SN0ModelComponent) {
        .kind = SN0_COMPONENT_IOC3, .device = 2, .irq_input = 4,
    };
    f->board = (SN0ModelIoBoard) {
        .id = "baseio-0", .module_index = 0, .crossbow_index = 0,
        .widget = 8,
        .bridge = &f->bridge, .pci = f->pci, .pci_count = 3,
    };
}

static void assert_invalid(Fixture *f, const char *message)
{
    Error *err = NULL;

    g_assert_false(sn0_origin2000_xtalk_validate(&f->model, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), message));
    error_free(err);
}

static void assert_valid_model(const SN0ModelComposition *model)
{
    Error *err = NULL;

    if (!sn0_origin2000_xtalk_validate(model, &err)) {
        g_error("unexpected validation error: %s", error_get_pretty(err));
    }
    g_assert_null(err);
}

static void test_valid(void)
{
    Fixture f;

    fixture_init(&f);
    assert_valid_model(&f.model);
}

static void test_generated(void)
{
    g_assert_cmpuint(sn0_origin2000_composition_count, ==, 1);
    assert_valid_model(&sn0_origin2000_compositions[0]);
}

static void test_reordered(void)
{
    Fixture f;
    SN0ModelPhysicalNode node;
    SN0ModelCrossbow crossbow;
    SN0ModelXtalkAttachment attachment;
    SN0ModelComponent pci;

    fixture_init(&f);

    node = f.nodes[0];
    f.nodes[0] = f.nodes[2];
    f.nodes[2] = node;
    f.attachments[0].node_index = 2;
    f.attachments[1].node_index = 0;

    crossbow = f.crossbows[0];
    f.crossbows[0] = f.crossbows[1];
    f.crossbows[1] = crossbow;
    for (unsigned int i = 0; i < G_N_ELEMENTS(f.attachments); i++) {
        f.attachments[i].crossbow_index ^= 1;
    }
    f.board.crossbow_index = 1;

    attachment = f.attachments[0];
    f.attachments[0] = f.attachments[3];
    f.attachments[3] = attachment;
    pci = f.pci[0];
    f.pci[0] = f.pci[2];
    f.pci[2] = pci;

    assert_valid_model(&f.model);
}

static void test_duplicate_widget(void)
{
    Fixture f;

    fixture_init(&f);
    f.attachments[1].widget = f.attachments[0].widget;
    assert_invalid(&f, "widget");
}

static void test_duplicate_node(void)
{
    Fixture f;

    fixture_init(&f);
    f.attachments[1].node_index = f.attachments[0].node_index;
    assert_invalid(&f, "node");
}

static void test_board_widget_collision(void)
{
    Fixture f;

    fixture_init(&f);
    f.board.widget = f.attachments[0].widget;
    f.bridge.widget = f.board.widget;
    assert_invalid(&f, "widget");
}

static void test_widget_range(void)
{
    Fixture f;

    fixture_init(&f);
    f.attachments[0].widget = 7;
    assert_invalid(&f, "widget");
}

static void test_health_mask(void)
{
    Fixture f;

    fixture_init(&f);
    f.crossbows[1].initially_operational_links |= 1U << 8;
    assert_invalid(&f, "mask");
}

static void test_reference(void)
{
    Fixture f;

    fixture_init(&f);
    f.attachments[0].crossbow_index = f.model.crossbow_count;
    assert_invalid(&f, "Crossbow");

    fixture_init(&f);
    f.attachments[0].node_index = f.model.node_count;
    assert_invalid(&f, "node");

    fixture_init(&f);
    f.board.module_index = f.model.module_count;
    assert_invalid(&f, "module");
}

static void test_module_membership(void)
{
    Fixture f;

    fixture_init(&f);
    f.module_nodes[3] = f.module_nodes[2];
    assert_invalid(&f, "module");
}

static void test_bridge_widget(void)
{
    Fixture f;

    fixture_init(&f);
    f.bridge.widget++;
    assert_invalid(&f, "Bridge widget");
}

static void test_pci_devfn(void)
{
    Fixture f;

    fixture_init(&f);
    f.pci[1].device = f.pci[0].device;
    f.pci[1].function = f.pci[0].function;
    assert_invalid(&f, "PCI devfn");
}

static void test_pci_reference(void)
{
    Fixture f;

    fixture_init(&f);
    f.pci[0].device = 32;
    assert_invalid(&f, "PCI devfn");

    fixture_init(&f);
    f.pci[0].function = 8;
    assert_invalid(&f, "PCI devfn");
}

static void test_count_pointer(void)
{
    Fixture f;

    fixture_init(&f);
    f.model.crossbows = NULL;
    assert_invalid(&f, "Crossbow");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sn0/xtalk-model/valid", test_valid);
    g_test_add_func("/sn0/xtalk-model/generated", test_generated);
    g_test_add_func("/sn0/xtalk-model/reordered", test_reordered);
    g_test_add_func("/sn0/xtalk-model/duplicate-widget", test_duplicate_widget);
    g_test_add_func("/sn0/xtalk-model/duplicate-node", test_duplicate_node);
    g_test_add_func("/sn0/xtalk-model/board-widget-collision",
                    test_board_widget_collision);
    g_test_add_func("/sn0/xtalk-model/widget-range", test_widget_range);
    g_test_add_func("/sn0/xtalk-model/health-mask", test_health_mask);
    g_test_add_func("/sn0/xtalk-model/reference", test_reference);
    g_test_add_func("/sn0/xtalk-model/module-membership",
                    test_module_membership);
    g_test_add_func("/sn0/xtalk-model/bridge-widget", test_bridge_widget);
    g_test_add_func("/sn0/xtalk-model/pci-devfn", test_pci_devfn);
    g_test_add_func("/sn0/xtalk-model/pci-reference", test_pci_reference);
    g_test_add_func("/sn0/xtalk-model/count-pointer", test_count_pointer);
    return g_test_run();
}
