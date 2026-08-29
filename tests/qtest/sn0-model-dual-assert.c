/*
 * Origin 200 dual-profile model assertions.
 *
 * The generated dual model uses the same public symbol names as the base
 * model, so it lives in this separate translation unit. Only the two test
 * entry points cross that boundary.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#include "sn0-model-dual.h"
#include "sn0-model-dual-assert.h"

#define SN0_NODE_STRIDE UINT64_C(0x100000000)

#define SN0_DUAL_PROFILE_ARGS \
    "-M " SN0_MODEL_MACHINE " " \
    "-m 128M " \
    "-object memory-backend-ram,id=ram0,size=64M " \
    "-object memory-backend-ram,id=ram1,size=64M " \
    "-numa node,nodeid=0,cpus=0-1,memdev=ram0 " \
    "-numa node,nodeid=1,cpus=2-3,memdev=ram1 " \
    "-smp 4,sockets=2,cores=2,threads=1"

static bool mask_graph_divergences(const SN0ModelConfigGraph *graph,
                                   const SN0ModelDivergence *divergences,
                                   const uint8_t *got, bool *excused);

static uint64_t read_field(QTestState *qts, uint64_t at, unsigned int width)
{
    switch (width) {
    case 1:
        return qtest_readb(qts, at);
    case 2:
        return qtest_readw(qts, at);
    case 4:
        return qtest_readl(qts, at);
    case 8:
        return qtest_readq(qts, at);
    default:
        g_assert_not_reached();
    }
}

static const SN0ModelField *find_divergent_field(
    const SN0ModelConfigGraph *graph, const SN0ModelDivergence *divergence)
{
    size_t i;

    for (i = 0; i < graph->field_count; i++) {
        const SN0ModelField *field = &graph->fields[i];

        if (field->offset == divergence->offset &&
            field->width == divergence->width &&
            g_str_equal(field->field, divergence->field)) {
            return field;
        }
    }
    return NULL;
}

static bool mask_graph_divergences(const SN0ModelConfigGraph *graph,
                                   const SN0ModelDivergence *divergences,
                                   const uint8_t *got, bool *excused)
{
    size_t i;

    for (i = 0; divergences[i].field != NULL; i++) {
        const SN0ModelDivergence *divergence = &divergences[i];
        const SN0ModelField *field;
        uint64_t at;
        uint64_t machine = 0;
        unsigned int byte;

        if (divergence->width == 0 || divergence->width > 8 ||
            divergence->offset < graph->base ||
            divergence->width > graph->span ||
            divergence->offset - graph->base >
            graph->span - divergence->width) {
            g_test_message("known divergence %s is outside this graph span",
                           divergence->field);
            return false;
        }
        field = find_divergent_field(graph, divergence);
        if (!field) {
            g_test_message("known divergence %s no longer identifies an "
                           "exact generated field", divergence->field);
            return false;
        }
        if (field->value == divergence->machine_value) {
            g_test_message("known divergence %s is stale: model and machine "
                           "value are both 0x%" PRIx64,
                           divergence->field, field->value);
            return false;
        }

        at = divergence->offset - graph->base;
        for (byte = 0; byte < divergence->width; byte++) {
            machine = (machine << 8) | got[at + byte];
        }
        if (machine != divergence->machine_value) {
            g_test_message("known divergence %s expects machine value 0x%"
                           PRIx64 ", but the machine now has 0x%" PRIx64,
                           divergence->field, divergence->machine_value,
                           machine);
            return false;
        }
        for (byte = 0; byte < divergence->width; byte++) {
            excused[at + byte] = true;
        }
    }
    return true;
}

static uint64_t expected_machine_value(const SN0ModelField *field)
{
    size_t i;

    for (i = 0; sn0_model_divergences[i].field != NULL; i++) {
        if (sn0_model_divergences[i].offset == field->offset) {
            g_assert_cmpuint(sn0_model_divergences[i].width, ==,
                             field->width);
            g_assert_cmpstr(sn0_model_divergences[i].field, ==,
                            field->field);
            g_assert_cmphex(sn0_model_divergences[i].machine_value, !=,
                            field->value);
            return sn0_model_divergences[i].machine_value;
        }
    }
    return field->value;
}

static void assert_graph_fields(QTestState *qts,
                                const SN0ModelConfigGraph *graph)
{
    uint64_t node_base = graph->nasid * SN0_NODE_STRIDE;
    unsigned int memory_records = 0;
    size_t i;

    for (i = 0; i < graph->field_count; i++) {
        const SN0ModelField *field = &graph->fields[i];
        uint64_t got = read_field(qts, node_base + field->offset,
                                  field->width);
        uint64_t wanted = expected_machine_value(field);

        if (got != wanted) {
            g_test_message("node %u %s at 0x%" PRIx64
                           ": model says 0x%" PRIx64
                           ", machine has 0x%" PRIx64,
                           graph->nasid, field->field,
                           node_base + field->offset, wanted, got);
        }
        g_assert_cmphex(got, ==, wanted);
        if (g_str_equal(field->field, "memory_bank.total_mb")) {
            g_assert_cmpuint(field->value, ==, 64);
            memory_records++;
        }
    }
    g_assert_cmpuint(memory_records, ==, 1);
}

static void assert_graph_image(QTestState *qts,
                               const SN0ModelConfigGraph *graph)
{
    uint64_t node_base = graph->nasid * SN0_NODE_STRIDE;
    g_autofree uint8_t *got = g_malloc0(graph->span);
    g_autofree bool *excused = g_malloc0(graph->span * sizeof(bool));
    uint64_t off;

    qtest_memread(qts, node_base + graph->base, got, graph->span);
    g_assert_cmpuint(got[4], ==, 0xbe);
    g_assert_true(mask_graph_divergences(graph, sn0_model_divergences,
                                         got, excused));
    for (off = 0; off < graph->span; off++) {
        if (excused[off]) {
            continue;
        }
        if (got[off] != graph->image[off]) {
            g_test_message("node %u graph byte at 0x%" PRIx64
                           ": model says 0x%x, machine has 0x%x",
                           graph->nasid, node_base + graph->base + off,
                           graph->image[off], got[off]);
        }
        g_assert_cmphex(got[off], ==, graph->image[off]);
    }

    for (off = graph->span; off < graph->size; off += 4096) {
        size_t len = MIN(4096, graph->size - off);
        g_autofree uint8_t *tail = g_malloc0(len);
        size_t i;

        qtest_memread(qts, node_base + graph->base + off, tail, len);
        for (i = 0; i < len; i++) {
            g_assert_cmpuint(tail[i], ==, 0);
        }
    }
}

static unsigned int generated_qom_count(const char *type)
{
    unsigned int count = 0;
    size_t i;

    for (i = 0; i < ARRAY_SIZE(sn0_model_devices); i++) {
        if (g_str_equal(sn0_model_devices[i].qom_type, type)) {
            count++;
        }
    }
    return count;
}

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
        assert_graph_fields(qts, graph);
        assert_graph_image(qts, graph);
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

    g_assert_cmpuint(generated_qom_count("sgi-hub"), ==, 2);
    g_assert_cmpuint(generated_qom_count("sgi-bridge"), ==, 2);
    g_assert_cmpuint(generated_qom_count("sgi-ioc3"), ==, 2);
    g_assert_cmpuint(generated_qom_count("qlogic-isp1040"), ==, 4);
    g_assert_cmpuint(generated_qom_count("R10000-mips64-cpu"), ==, 4);

    sn0_model_assert_object_graph(qts, SN0_MODEL_MACHINE_TYPE,
                                  sn0_model_devices,
                                  ARRAY_SIZE(sn0_model_devices));
    qtest_quit(qts);
}

void sn0_model_test_dual_divergence_masking(const void *data)
{
    SN0ModelField fields[] = {
        { 0x4001, 1, 0xbe, "fixture.signature" },
    };
    const uint8_t image[] = { 0, 0xbe, 0, 0 };
    const SN0ModelConfigGraph graph = {
        0, 0x4000, sizeof(image), sizeof(image),
        fields, ARRAY_SIZE(fields), image,
    };
    const SN0ModelDivergence divergences[] = {
        { 0x4001, 1, 0, "fixture.signature", "fixture owner" },
        { 0, 0, 0, NULL, NULL },
    };
    uint8_t got[] = { 0, 0, 0, 0 };
    bool excused[ARRAY_SIZE(got)] = { false };
    size_t i;

    g_assert_true(mask_graph_divergences(&graph, divergences,
                                         got, excused));
    for (i = 0; i < ARRAY_SIZE(got); i++) {
        g_assert_true(excused[i] || got[i] == image[i]);
    }

    memset(excused, 0, sizeof(excused));
    got[1] = image[1];
    g_assert_false(mask_graph_divergences(&graph, divergences,
                                          got, excused));

    memset(excused, 0, sizeof(excused));
    got[1] = 0;
    fields[0].value = 0;
    g_assert_false(mask_graph_divergences(&graph, divergences,
                                          got, excused));
}
