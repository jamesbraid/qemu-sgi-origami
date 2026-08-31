/*
 * Origin 200 dual-profile model assertions.
 *
 * The generated dual model uses the same public symbol names as the base
 * model, so it lives in this separate translation unit.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#include "sn0-model-dual.h"
#include "sn0-model-dual-assert.h"

#define SN0_DUAL_PROFILE_ARGS \
    "-M " SN0_MODEL_MACHINE " " \
    "-m 128M " \
    "-object memory-backend-ram,id=ram0,size=64M " \
    "-object memory-backend-ram,id=ram1,size=64M " \
    "-numa node,nodeid=0,cpus=0-1,memdev=ram0 " \
    "-numa node,nodeid=1,cpus=2-3,memdev=ram1 " \
    "-smp 4,sockets=2,cores=2,threads=1"

void sn0_model_test_dual_config_graph(const void *data)
{
    QTestState *qts = qtest_init(SN0_DUAL_PROFILE_ARGS);
    size_t i;

    g_assert_cmpuint(SN0_MODEL_RAM_MB, ==, 128);
    g_assert_cmpuint(SN0_MODEL_CONFIG_GRAPH_COUNT, ==, 2);
    g_assert_cmpuint(ARRAY_SIZE(sn0_model_config_graphs), ==, 2);
    for (i = 0; i < ARRAY_SIZE(sn0_model_config_graphs); i++) {
        const SN0ModelConfigGraph *graph = &sn0_model_config_graphs[i];

        g_assert_cmpuint(graph->nasid, ==, i);
        sn0_model_assert_graph_fields(qts, graph,
                                      sn0_model_divergences);
        sn0_model_assert_graph_image(qts, graph,
                                     sn0_model_divergences);
    }

    qtest_quit(qts);
}

void sn0_model_test_dual_device_identity(const void *data)
{
    const SN0ModelDeviceIdentity expected =
        SN0_MODEL_DEVICE_IDENTITY_INIT;
    QTestState *qts = qtest_init(SN0_DUAL_PROFILE_ARGS);
    unsigned int node;

    for (node = 0; node < 2; node++) {
        sn0_model_assert_device_identity(qts, node, &expected);
    }
    qtest_quit(qts);
}

void sn0_model_test_dual_object_graph(const void *data)
{
    QTestState *qts = qtest_init(SN0_DUAL_PROFILE_ARGS);

    sn0_model_assert_object_graph(qts, SN0_MODEL_MACHINE_TYPE,
                                  sn0_model_devices,
                                  ARRAY_SIZE(sn0_model_devices));
    qtest_quit(qts);
}

void sn0_model_test_dual_divergence_masking(const void *data)
{
    SN0ModelField fields[] = {
        { 0x4001, 1, 0xbe, "fixture.signature" },
        { 0x4001, 1, 0xbe, "fixture.overlap" },
    };
    const uint8_t model_image[] = { 0, 0xbe, 0, 0 };
    SN0ModelConfigGraph graph = {
        0, 0x4000, sizeof(model_image), sizeof(model_image),
        fields, 1,
    };
    const SN0ModelDivergence divergences[] = {
        { 0x4001, 1, 0, "fixture.signature", "fixture owner" },
        { 0, 0, 0, NULL, NULL },
    };
    uint8_t got[] = { 0, 0, 0, 0 };
    uint8_t expected[ARRAY_SIZE(got)];

    g_assert_true(sn0_model_reconstruct_graph(&graph, expected,
                                               sizeof(expected)));
    g_assert_cmpmem(expected, sizeof(expected),
                    model_image, sizeof(model_image));
    g_assert_true(sn0_model_apply_graph_divergences(&graph, divergences,
                                                     got, expected));
    g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));

    got[1] = model_image[1];
    g_assert_false(sn0_model_apply_graph_divergences(&graph, divergences,
                                                      got, expected));

    got[1] = 0;
    fields[0].value = 0;
    g_assert_false(sn0_model_apply_graph_divergences(&graph, divergences,
                                                      got, expected));

    fields[0].value = 0xbe;
    fields[0].width = 3;
    g_assert_false(sn0_model_reconstruct_graph(&graph, expected,
                                                sizeof(expected)));

    fields[0].width = 1;
    fields[0].offset = 0x4004;
    g_assert_false(sn0_model_reconstruct_graph(&graph, expected,
                                                sizeof(expected)));

    fields[0].offset = 0x4001;
    fields[0].value = 0x100;
    g_assert_false(sn0_model_reconstruct_graph(&graph, expected,
                                                sizeof(expected)));

    fields[0].value = 0xbe;
    graph.field_count = ARRAY_SIZE(fields);
    g_assert_false(sn0_model_reconstruct_graph(&graph, expected,
                                                sizeof(expected)));
}
