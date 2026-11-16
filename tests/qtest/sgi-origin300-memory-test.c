/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Where the Origin 300's two 256 MiB DIMMs sit. The guide says a system
 * with one DIMM pair fits banks 0 and 1, and the stock PROM swaps banks
 * when only banks 2 and 3 are fitted, which OpenBSD does not follow. The
 * Bedrock must therefore see the DIMMs at banks 0 and 1 on every node of
 * every Origin 300 topology.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define SN1_PROM_SIZE 0x1686a8

static char *directory;

/* The PROM's contents do not matter: the machine is only inspected. */
static char *make_prom(void)
{
    g_autofree char *bytes = g_malloc0(SN1_PROM_SIZE);
    char *path = g_build_filename(directory, "ip35-prom", NULL);

    g_assert_true(g_file_set_contents(path, bytes, SN1_PROM_SIZE, NULL));
    return path;
}

static uint64_t qom_get_uint(QTestState *qts, const char *path,
                             const char *prop)
{
    QDict *r = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                         "{'path': %s, 'property': %s}}", path, prop);
    uint64_t value = qdict_get_uint(r, "return");

    qobject_unref(r);
    return value;
}

/* Banks 0 and 1 hold 256 MiB each; the other six are empty. */
static void check_node(QTestState *qts, const char *bedrock)
{
    for (unsigned int bank = 0; bank < 8; bank++) {
        g_autofree char *property =
            g_strdup_printf("installed-bank%u-mib", bank);

        g_assert_cmpuint(qom_get_uint(qts, bedrock, property), ==,
                         bank < 2 ? 256 : 0);
    }
    g_assert_cmphex(qom_get_uint(qts, bedrock, "installed-bank-mask"), ==,
                    0x3);
}

static void test_one_chassis(void)
{
    g_autofree char *prom = make_prom();
    QTestState *qts = qtest_initf("-M origin300 -cpu R14000 -smp 2 "
                                  "-display none -nodefaults -S -bios %s",
                                  prom);

    check_node(qts, "/machine/node0/bedrock");
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

static void test_two_chassis(void)
{
    g_autofree char *prom = make_prom();
    QTestState *qts = qtest_initf("-M origin300,topology=origin300-two-chassis"
                                  " -cpu R14000 -smp 4 -m 1024 "
                                  "-display none -nodefaults -S -bios %s",
                                  prom);

    check_node(qts, "/machine/node0/bedrock");
    check_node(qts, "/machine/node1/bedrock");
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

int main(int argc, char **argv)
{
    g_autoptr(GError) error = NULL;
    int ret;

    g_test_init(&argc, &argv, NULL);
    directory = g_dir_make_tmp("sgi-origin300-memory-XXXXXX", &error);
    g_assert_no_error(error);

    qtest_add_func("/sgi-origin300-memory/one-chassis", test_one_chassis);
    qtest_add_func("/sgi-origin300-memory/two-chassis", test_two_chassis);

    ret = g_test_run();
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_free(directory);
    return ret;
}
