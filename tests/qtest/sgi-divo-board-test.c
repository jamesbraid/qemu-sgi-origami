/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * QTest for the DIVO XIO board: its two LINC functions on the device each
 * part number fixes, the part and revision on the Bridge's ID PROM record, and
 * the refusal of a product that is not modelled. The LINC's own register
 * contract is in sgi-divo-test.c. See docs/system/mips/sgi/hardware/divo.rst.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-test.h"
#include "sgi-launch.h"

static QTestState *board_start(const char *args)
{
    return qtest_initf("-M onyx2 -display none -device sgi-divo,id=divo,"
                       "slot=io2%s", args);
}

static uint32_t qom_u32(QTestState *qts, const char *path, const char *prop)
{
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': {"
                           " 'path': %s, 'property': %s } }", path, prop);
    uint32_t val;

    g_assert(qdict_haskey(rsp, "return"));
    val = qdict_get_int(rsp, "return");
    qobject_unref(rsp);
    return val;
}

static char *qom_str(QTestState *qts, const char *path, const char *prop)
{
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': {"
                           " 'path': %s, 'property': %s } }", path, prop);
    char *val;

    g_assert(qdict_haskey(rsp, "return"));
    val = g_strdup(qdict_get_str(rsp, "return"));
    qobject_unref(rsp);
    return val;
}

/* The default board is 030-1305-001 with its second LINC on device 2. */
static void test_default(void)
{
    QTestState *qts = board_start("");
    g_autofree char *part = qom_str(qts, "/machine/module0/divo/bridge",
                                    "board-part");
    g_autofree char *revision = qom_str(qts, "/machine/module0/divo/bridge",
                                        "board-revision");
    g_autofree char *product = qom_str(qts, "/machine/module0/divo",
                                       "product");

    g_assert_cmpstr(part, ==, "030-1305-001");
    g_assert_cmpstr(revision, ==, "E");
    g_assert_cmpstr(product, ==, "divo");
    g_assert_cmpuint(qom_u32(qts, "/machine/module0/divo/linc0", "addr"), ==,
                     0);
    g_assert_cmpuint(qom_u32(qts, "/machine/module0/divo/linc1", "addr"), ==,
                     2 << 3);
    qtest_quit(qts);
}

/* The 030-1046-002 board has its second LINC in slot 1. */
static void test_1046(void)
{
    QTestState *qts = board_start(",product=divo-1046");
    g_autofree char *part = qom_str(qts, "/machine/module0/divo/bridge",
                                    "board-part");

    g_assert_cmpstr(part, ==, "030-1046-002");
    g_assert_cmpuint(qom_u32(qts, "/machine/module0/divo/linc0", "addr"), ==,
                     0);
    g_assert_cmpuint(qom_u32(qts, "/machine/module0/divo/linc1", "addr"), ==,
                     1 << 3);
    qtest_quit(qts);
}

/* The DIVO-DVC board is not modelled, and an unknown product is refused. */
static void test_unknown_product(void)
{
    const char *const args[] = {
        qtest_qemu_binary(NULL), "-M", "onyx2", "-display", "none",
        "-accel", "qtest", "-qmp", "stdio",
        "-device", "sgi-divo,slot=io2,product=divo-dvc", NULL,
    };

    assert_launch_rejected(args, "unknown DIVO board product");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("sgi-divo-board/default", test_default);
    qtest_add_func("sgi-divo-board/1046", test_1046);
    qtest_add_func("sgi-divo-board/unknown-product", test_unknown_product);
    return g_test_run();
}
