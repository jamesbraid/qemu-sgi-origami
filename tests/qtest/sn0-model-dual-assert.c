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
    "-M origin200 " \
    "-m 128M " \
    "-object memory-backend-ram,id=ram0,size=64M " \
    "-object memory-backend-ram,id=ram1,size=64M " \
    "-numa node,nodeid=0,cpus=0-1,memdev=ram0 " \
    "-numa node,nodeid=1,cpus=2-3,memdev=ram1 " \
    "-smp 4,sockets=2,cores=2,threads=1"

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

static uint64_t expected_machine_value(const SN0ModelField *field)
{
    size_t i;

    for (i = 0; sn0_model_divergences[i].field != NULL; i++) {
        if (sn0_model_divergences[i].offset == field->offset) {
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
    uint64_t off;

    qtest_memread(qts, node_base + graph->base, got, graph->span);
    g_assert_cmpuint(got[4], ==, 0xbe);
    for (off = 0; off < graph->span; off++) {
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
