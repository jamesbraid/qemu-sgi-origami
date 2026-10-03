/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qnum.h"
#include "sgi-migrate.h"
#include "sgi-l1-test.h"

#define BASE 0x10000000
#define FIX "/machine/peripheral/test"
#define ARGS "-M none -cpu R10000 -device sgi-sn1-router,id=r " \
    "-device sgi-sn1-router-test,id=test,router=r"

static const uint64_t bank[8] = {
    0x10000, 0x10800, 0x20000, 0x20800,
    0x30000, 0x30800, 0x40000, 0x40800,
};

static void set(QTestState *s, const char *name, uint64_t value)
{
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':%s,'value':%llu}}", FIX, name,
        (unsigned long long)value);
}

static uint64_t get(QTestState *s, const char *name)
{
    QDict *r = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        FIX, name);
    uint64_t value = qnum_get_uint(qobject_to(QNum, qdict_get(r, "return")));

    qobject_unref(r);
    return value;
}

static void test_registers(void)
{
    QTestState *s = qtest_init(ARGS);

    for (unsigned p = 1; p <= 8; p++) {
        set(s, "ingress", p);
        g_assert_cmphex(qtest_readq(s, BASE), ==,
                        ((uint64_t)p << 52) | (UINT64_C(1) << 28) |
                        (UINT64_C(0x3017) << 12));
        g_assert_cmphex(qtest_readq(s, BASE + 0x600000), ==,
                        qtest_readq(s, BASE));
        for (unsigned i = 0; i < 128; i++) {
            qtest_writeq(s, BASE + bank[p - 1] + 0x200 + 8 * i, i + p);
            g_assert_cmphex(qtest_readq(s, BASE + bank[p - 1] + 0x200 + 8 * i),
                            ==, (i + p) & 7);
        }
        for (unsigned i = 0; i < 16; i++) {
            qtest_writeq(s, BASE + bank[p - 1] + 0x100 + 8 * i, i + p);
            g_assert_cmphex(qtest_readq(s, BASE + bank[p - 1] + 0x100 + 8 * i),
                            ==, (i + p) & 7);
        }
        g_assert_cmphex(qtest_readq(s, BASE + bank[p - 1] + 8), ==, 0);
    }
    qtest_writeq(s, BASE + 0x100, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, BASE + 0x100), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, BASE + 0x518), ==, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x518), ==, 1);
    qtest_writeq(s, BASE + 0x118, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x518), ==, 0);
    qtest_readq(s, BASE + 0x600008);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_readq(s, BASE + 0x30);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_quit(s);
}

static void test_routing(void)
{
    QTestState *s = qtest_init(ARGS);

    qtest_writeq(s, BASE + bank[0] + 0x200 + 5 * 8, 7);
    qtest_writeq(s, BASE + bank[3] + 0x200 + 5 * 8, 2);
    set(s, "destination", 5);
    set(s, "turn", 3);
    g_assert_cmphex(get(s, "route"), ==, 4 | (2 << 8));
    set(s, "ingress", 8);
    set(s, "turn", 1);
    g_assert_cmphex(get(s, "route"), ==, 1 | (7 << 8));
    set(s, "turn", 8);
    g_assert_cmphex(get(s, "route"), ==, UINT64_MAX);
    set(s, "turn", 1);
    set(s, "destination", 128);
    g_assert_cmphex(get(s, "route"), ==, UINT64_MAX);
    set(s, "destination", 5);
    qtest_writeq(s, BASE + 0x10, 1 << 8);
    g_assert_cmphex(get(s, "route"), ==, UINT64_MAX);
    qtest_writeq(s, BASE + 0x10, (1 << 8) | (1 << 12));
    g_assert_cmphex(get(s, "route"), ==, 1 | (7 << 8));
    qtest_quit(s);
}

static void test_vector_links(void)
{
    QTestState *s = qtest_init(ARGS " -device sgi-sn1-router,id=peer "
        "-device sgi-net-cable,id=c,end0=r,end0-index=8,"
        "end1=peer,end1-index=7");

    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/r','property':'port8-cable',"
        "'value':'/machine/peripheral/c'}}");
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/peer','property':'port7-cable',"
        "'value':'/machine/peripheral/c'}}");
    g_assert_true(qtest_readq(s, BASE) & (UINT64_C(1) << 51));
    set(s, "vector", 8);
    g_assert_cmphex(qtest_readq(s, BASE) >> 52, ==, 7);
    g_assert_cmphex(get(s, "reply"), ==, 1);
    set(s, "hops", 15);
    qtest_readq(s, BASE);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    set(s, "hops", 0);
    qtest_writeq(s, BASE + 0x108, 0xaabbccdd);
    g_assert_cmphex(qtest_readq(s, BASE + 0x108), ==, 0xaabbccdd);
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/c','property':'down','value':true}}");
    qtest_readq(s, BASE);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    set(s, "vector", 9);
    qtest_readq(s, BASE);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_quit(s);
}

static void test_silent_peer(void)
{
    QTestState *s = qtest_init(ARGS
        " -device sgi-sn1-router-test-peer,id=peer "
        "-device sgi-net-cable,id=c,end0=r,end0-index=8,"
        "end1=peer,end1-index=0");

    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/r','property':'port8-cable',"
        "'value':'/machine/peripheral/c'}}");
    set(s, "vector", 8);
    qtest_readq(s, BASE);
    g_assert_cmpuint(get(s, "result"), ==, 1);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    g_assert_cmphex(get(s, "reply"), ==, 1);
    qtest_quit(s);
}

static void test_refusals(void)
{
    QTestState *s = qtest_init(ARGS);

    set(s, "operation", 2);
    qtest_readq(s, BASE);
    g_assert_cmpuint(get(s, "failure"), ==, 5);
    set(s, "operation", 3);
    qtest_readq(s, BASE);
    g_assert_cmpuint(get(s, "failure"), ==, 5);
    set(s, "operation", UINT64_MAX);
    qtest_readl(s, BASE);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_readq(s, BASE + 1);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    set(s, "ingress", 0);
    qtest_readq(s, BASE);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    set(s, "ingress", UINT64_C(1) << 32);
    qtest_readq(s, BASE);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    g_assert_cmphex(get(s, "route"), ==, UINT64_MAX);
    set(s, "ingress", 1);
    qtest_writeq(s, BASE, 1);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_writeq(s, BASE + 0x518, 1);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_writeq(s, BASE + 0x120, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, BASE + 0x120), ==, 1);
    qtest_writeq(s, BASE + 0x120, 2);
    g_assert_cmphex(qtest_readq(s, BASE + 0x120), ==, 0);
    qtest_quit(s);
}

static void test_reset(void)
{
    QTestState *s = qtest_init(ARGS);

    qtest_writeq(s, BASE + 0x100, UINT64_MAX);
    qtest_writeq(s, BASE + bank[7] + 0x200, 7);
    qtest_readq(s, BASE + 0x518);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(s, BASE + 0x100), ==, 0);
    g_assert_cmphex(qtest_readq(s, BASE + bank[7] + 0x200), ==, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x518), ==, 0);
    g_assert_cmphex((qtest_readq(s, BASE) >> 28) & 15, ==, 1);
    qtest_quit(s);
}

static void test_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("sn1-router-migrate-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "socket", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = qtest_init(ARGS);
    QTestState *to = qtest_init(ARGS " -incoming defer");

    qtest_writeq(from, BASE + 0x100, 0x123456789abcdef0);
    qtest_readq(from, BASE + 0x518);
    qtest_writeq(from, BASE + 0x10, 1 << 12);
    for (unsigned p = 0; p < 8; p++) {
        qtest_writeq(from, BASE + bank[p] + 0x200, p);
        qtest_writeq(from, BASE + bank[p] + 0x100, 7 - p);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readq(to, BASE + 0x100), ==, 0x123456789abcdef0);
    g_assert_cmphex(qtest_readq(to, BASE + 0x518), ==, 1);
    g_assert_cmphex(qtest_readq(to, BASE + 0x10), ==, 1 << 12);
    for (unsigned p = 0; p < 8; p++) {
        g_assert_cmphex(qtest_readq(to, BASE + bank[p] + 0x200), ==, p);
        g_assert_cmphex(qtest_readq(to, BASE + bank[p] + 0x100), ==, 7 - p);
    }
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    rmdir(dir);
}

static void test_uart(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 "
        "-device sgi-l1,id=l1,brick-type=router "
        "-device sgi-l1-uart,id=uart,controller=l1 "
        "-device sgi-sn1-router,id=r,l1-uart=uart "
        "-device sgi-sn1-router-test,id=test,router=r "
        "-device sgi-l1,id=l2,brick-type=router -device sgi-l1-uart,id=uart2,controller=l2 "
        "-device sgi-sn1-router,id=peer,l1-uart=uart2 "
        "-device sgi-net-cable,id=c,end0=r,end0-index=8,"
        "end1=peer,end1-index=7");

    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/r','property':'port8-cable',"
        "'value':'/machine/peripheral/c'}}");
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/peer','property':'port7-cable',"
        "'value':'/machine/peripheral/c'}}");
    g_assert_cmphex(qtest_readq(s, BASE + 0x268), ==, 0x60);
    qtest_writeq(s, BASE + 0x258, 0x80);
    qtest_writeq(s, BASE + 0x240, 0x12ab);
    qtest_writeq(s, BASE + 0x248, 0x34cd);
    g_assert_cmphex(qtest_readq(s, BASE + 0x240), ==, 0xab);
    g_assert_cmphex(qtest_readq(s, BASE + 0x248), ==, 0xcd);
    set(s, "vector", 8);
    g_assert_cmphex(qtest_readq(s, BASE + 0x258), ==, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x268), ==, 0x60);
    qtest_writeq(s, BASE + 0x258, 0x80);
    qtest_writeq(s, BASE + 0x240, 0x33);
    g_assert_cmphex(qtest_readq(s, BASE + 0x240), ==, 0x33);
    set(s, "vector", 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x240), ==, 0xab);
    qtest_writeq(s, BASE + 0x258, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x268), ==, 0x60);
    const uint8_t request[] = { 19, 0x00, 0x04, 0x10, 6, 0, 3, 0 };
    uint8_t reply[131];

    sgi_l1_test_send(s, BASE + 0x240, request, sizeof(request), false);
    set(s, "vector", 8);
    g_assert_cmphex(qtest_readq(s, BASE + 0x268), ==, 0x60);
    qtest_writeq(s, BASE + 0x258, 0);
    sgi_l1_test_send(s, BASE + 0x240, request, sizeof(request), false);
    g_assert_cmpuint(sgi_l1_test_receive(s, BASE + 0x240, reply,
                                       sizeof(reply)), ==, 16);
    g_assert_cmphex(reply[0], ==, 0x20 | 19);
    g_assert_cmphex(reply[15], ==, 0x52);
    set(s, "vector", 0);
    g_assert_cmpuint(sgi_l1_test_receive(s, BASE + 0x240, reply,
                                       sizeof(reply)), ==, 16);
    g_assert_cmphex(reply[0], ==, 0x20 | 19);
    g_assert_cmphex(reply[15], ==, 0x52);
    g_assert_cmphex(qtest_readq(s, BASE + 0x268), ==, 0x60);
    set(s, "vector", 8);
    g_assert_cmphex(qtest_readq(s, BASE + 0x268), ==, 0x60);
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/sn1-router/registers", test_registers);
    qtest_add_func("/sgi/sn1-router/routing", test_routing);
    qtest_add_func("/sgi/sn1-router/vector-links", test_vector_links);
    qtest_add_func("/sgi/sn1-router/silent-peer", test_silent_peer);
    qtest_add_func("/sgi/sn1-router/refusals", test_refusals);
    qtest_add_func("/sgi/sn1-router/reset", test_reset);
    qtest_add_func("/sgi/sn1-router/migration", test_migration);
    qtest_add_func("/sgi/sn1-router/uart", test_uart);
    return g_test_run();
}
