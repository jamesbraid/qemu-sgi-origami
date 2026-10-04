/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Run qemu-sgi-machine-init as a launcher does and check what it accepts,
 * what it refuses and what it leaves behind. All images are synthetic.
 */
#include "qemu/osdep.h"

typedef struct ToolRun {
    int status;
    char *out;
    char *err;
} ToolRun;

static const char *tool;
static char *scratch;

static void tool_run_clear(ToolRun *run)
{
    g_free(run->out);
    g_free(run->err);
}

/* Run the tool with the NULL-terminated arguments after argv[0]. */
static ToolRun run_tool(const char *first, ...)
{
    g_autoptr(GPtrArray) argv = g_ptr_array_new();
    g_autoptr(GError) error = NULL;
    ToolRun run = { 0 };
    const char *arg;
    va_list ap;

    g_ptr_array_add(argv, (gpointer)tool);
    va_start(ap, first);
    for (arg = first; arg; arg = va_arg(ap, const char *)) {
        g_ptr_array_add(argv, (gpointer)arg);
    }
    va_end(ap);
    g_ptr_array_add(argv, NULL);

    g_assert_true(g_spawn_sync(NULL, (char **)argv->pdata, NULL,
                               G_SPAWN_DEFAULT, NULL, NULL, &run.out,
                               &run.err, &run.status, &error));
    g_assert_no_error(error);
    return run;
}

static bool run_succeeded(const ToolRun *run)
{
    return WIFEXITED(run->status) && WEXITSTATUS(run->status) == 0;
}

/* The tool failed, said @message and printed nothing on stdout. */
static void assert_refused(const ToolRun *run, const char *message)
{
    if (!strstr(run->err, message)) {
        g_printerr("expected '%s' in:\n%s", message, run->err);
    }
    g_assert_false(run_succeeded(run));
    g_assert_nonnull(strstr(run->err, message));
    g_assert_cmpstr(run->out, ==, "");
}

static char *scratch_path(const char *name)
{
    return g_build_filename(scratch, name, NULL);
}

static void test_help(void)
{
    ToolRun run = run_tool("--help", NULL);

    g_assert_true(run_succeeded(&run));
    g_assert_nonnull(strstr(run.out, "--machine OPTIONS"));
    tool_run_clear(&run);
}

/* Every listed selection appears once, and nothing else is printed. */
static void test_machine_help(void)
{
    static const char *const listed[] = {
        "origin200,topology=origin200,nodes=1",
        "origin200,topology=origin200-gigachannel,nodes=1",
        "origin2000,topology=origin2000-deskside,nodes=2",
        "origin300,topology=origin300,nodes=1",
        "fuel,topology=fuel,nodes=1",
        "octane,topology=octane",
    };
    ToolRun run = run_tool("--machine", "help", NULL);
    g_auto(GStrv) lines = NULL;
    size_t i;

    g_assert_true(run_succeeded(&run));
    g_assert_cmpstr(run.err, ==, "");
    lines = g_strsplit(run.out, "\n", -1);
    for (i = 0; i < ARRAY_SIZE(listed); i++) {
        g_autofree char *prefix = g_strdup_printf("%s ", listed[i]);
        unsigned int count = 0;
        char **line;

        for (line = lines; *line; line++) {
            count += g_str_has_prefix(*line, prefix);
        }
        g_assert_cmpuint(count, ==, 1);
    }
    tool_run_clear(&run);
}

static void test_usage_errors(void)
{
    g_autofree char *dir = scratch_path("usage");
    ToolRun run;

    run = run_tool(dir, NULL);
    assert_refused(&run, "--machine is required");
    tool_run_clear(&run);

    run = run_tool("--machine", "help", dir, NULL);
    assert_refused(&run, "--machine help takes no other arguments");
    tool_run_clear(&run);

    run = run_tool("--machine", "origin200,topology=origin200,nodes=1",
                   "--boot-prom", "prom", NULL);
    assert_refused(&run, "no directory named");
    tool_run_clear(&run);

    run = run_tool("--machine", "origin200,topology=origin200,nodes=1",
                   "--boot-prom", "prom", dir, "extra", NULL);
    assert_refused(&run, "unexpected argument 'extra'");
    tool_run_clear(&run);

    run = run_tool("--frobnicate", NULL);
    assert_refused(&run, "--help");
    tool_run_clear(&run);
    g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
}

static void test_unknown_machine(void)
{
    g_autofree char *dir = scratch_path("unknown");
    ToolRun run;

    run = run_tool("--machine", "vax", "--boot-prom", "prom", dir, NULL);
    assert_refused(&run, "unknown machine 'vax'");
    tool_run_clear(&run);

    /* A selection the machine resolves, but not in the catalogue's words. */
    run = run_tool("--machine", "origin200", "--boot-prom", "prom", dir,
                   NULL);
    assert_refused(&run, "'origin200' is not a listed machine");
    g_assert_nonnull(strstr(run.err,
                            "It selects 'origin200,topology=origin200,"
                            "nodes=1'"));
    tool_run_clear(&run);

    run = run_tool("--machine",
                   "origin200,topology=origin200,nodes=1,memory=64",
                   "--boot-prom", "prom", dir, NULL);
    assert_refused(&run, "machine option 'memory' is not part of a "
                   "catalogue selection");
    tool_run_clear(&run);

    run = run_tool("--machine", "origin200,topology=origin200,nodes=3",
                   "--boot-prom", "prom", dir, NULL);
    assert_refused(&run, "topology 'origin200' fits 1 node board, not 3");
    tool_run_clear(&run);
    g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
}

static void test_inputs_checked(void)
{
    g_autofree char *dir = scratch_path("inputs");
    ToolRun run;

    run = run_tool("--machine", "origin200,topology=origin200,nodes=1", dir,
                   NULL);
    assert_refused(&run, "origin200,topology=origin200,nodes=1 requires "
                   "--boot-prom");
    tool_run_clear(&run);

    run = run_tool("--machine", "origin2000,topology=origin2000-deskside,"
                   "nodes=2", "--boot-prom", "prom", dir, NULL);
    assert_refused(&run, "requires --io-prom");
    tool_run_clear(&run);

    run = run_tool("--machine", "origin200,topology=origin200,nodes=1",
                   "--boot-prom", "prom", "--io-prom", "io", dir, NULL);
    assert_refused(&run, "origin200,topology=origin200,nodes=1 takes no "
                   "--io-prom");
    tool_run_clear(&run);
    g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
}

int main(int argc, char **argv)
{
    g_autoptr(GError) error = NULL;
    int ret;

    g_test_init(&argc, &argv, NULL);
    tool = g_getenv("QEMU_SGI_MACHINE_INIT");
    g_assert_nonnull(tool);
    scratch = g_dir_make_tmp("sgi-machine-init-XXXXXX", &error);
    g_assert_no_error(error);

    g_test_add_func("/sgi-machine-init/help", test_help);
    g_test_add_func("/sgi-machine-init/machine-help", test_machine_help);
    g_test_add_func("/sgi-machine-init/usage-errors", test_usage_errors);
    g_test_add_func("/sgi-machine-init/unknown-machine",
                    test_unknown_machine);
    g_test_add_func("/sgi-machine-init/inputs-checked", test_inputs_checked);
    ret = g_test_run();

    g_assert_cmpint(g_rmdir(scratch), ==, 0);
    g_free(scratch);
    return ret;
}
