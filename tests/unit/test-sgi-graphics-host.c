/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The one-pipe-per-node rule, on hand-built node maps: no topology in the
 * catalogue puts two pipe slots under one node, so no machine reaches it.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/mips/sgi/sgi-graphics.h"

static void add(GPtrArray *fitted, const char *name, const char *host)
{
    SGIGraphicsFitted *pipe = g_new0(SGIGraphicsFitted, 1);

    pipe->name = g_strdup(name);
    pipe->host = g_strdup(host);
    g_ptr_array_add(fitted, pipe);
}

static void fitted_free(gpointer data)
{
    SGIGraphicsFitted *pipe = data;

    g_free(pipe->name);
    g_free(pipe->host);
    g_free(pipe);
}

/* A second pipe on a node that already controls one is refused. */
static void test_second_pipe_on_a_node(void)
{
    g_autoptr(GPtrArray) fitted = g_ptr_array_new_with_free_func(fitted_free);
    Error *err = NULL;

    add(fitted, "io3", "node-board-1-hub");
    add(fitted, "io7", "node-board-2-hub");
    g_assert_false(sgi_graphics_check_host("onyx2", fitted, "io2",
                                           "node-board-1-hub", &err));
    g_assert_cmpstr(error_get_pretty(err), ==,
                    "onyx2: graphics slot io2 is controlled by node "
                    "node-board-1-hub, which already controls the pipe in "
                    "io3, and a node controls at most one pipe");
    error_free(err);
}

/* Pipes on distinct nodes fit, whatever order they come in. */
static void test_distinct_nodes(void)
{
    g_autoptr(GPtrArray) fitted = g_ptr_array_new_with_free_func(fitted_free);

    g_assert_true(sgi_graphics_check_host("onyx2", fitted, "io3",
                                          "node-board-1-hub", &error_abort));
    add(fitted, "io3", "node-board-1-hub");
    g_assert_true(sgi_graphics_check_host("onyx2", fitted, "io7",
                                          "node-board-2-hub", &error_abort));
    add(fitted, "io7", "node-board-2-hub");
    g_assert_true(sgi_graphics_check_host("onyx2", fitted, "io9",
                                          "node-board-3-hub", &error_abort));
}

/* A second pipe in a fitted slot is the slot's to refuse, not the node's. */
static void test_same_slot(void)
{
    g_autoptr(GPtrArray) fitted = g_ptr_array_new_with_free_func(fitted_free);

    add(fitted, "io3", "node-board-1-hub");
    g_assert_true(sgi_graphics_check_host("onyx2", fitted, "io3",
                                          "node-board-1-hub", &error_abort));
}

/* A slot or a fitted pipe with no known node never conflicts. */
static void test_unknown_node(void)
{
    g_autoptr(GPtrArray) fitted = g_ptr_array_new_with_free_func(fitted_free);

    add(fitted, "widget-9", NULL);
    g_assert_true(sgi_graphics_check_host("octane", fitted, "widget-10",
                                          NULL, &error_abort));
    g_assert_true(sgi_graphics_check_host("octane", fitted, "widget-10",
                                          "node", &error_abort));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-graphics-host/second-pipe-on-a-node",
                    test_second_pipe_on_a_node);
    g_test_add_func("/sgi-graphics-host/distinct-nodes", test_distinct_nodes);
    g_test_add_func("/sgi-graphics-host/same-slot", test_same_slot);
    g_test_add_func("/sgi-graphics-host/unknown-node", test_unknown_node);
    return g_test_run();
}
