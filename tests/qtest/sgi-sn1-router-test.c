/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qnum.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "sgi-test.h"
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

static void expect_ordinary_vector_refusal(QTestState *s, uint64_t address,
                                           bool write)
{
    const uint64_t lb = UINT64_C(0x20600000);
    const uint64_t sentinel = UINT64_C(0xdeadbeefdeadbeef);
    const uint64_t params = address | (UINT64_C(0x5a) << 40);

    qtest_readq(s, lb + 0xc038);
    qtest_writeq(s, lb + 0xc030, sentinel);
    qtest_writeq(s, lb + 0xc028, sentinel);
    qtest_writeq(s, lb + 0xc008, 0);
    qtest_writeq(s, lb + 0xc010, 0);
    qtest_writeq(s, lb + 0xc000, params | write);
    g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==, 0);
    g_assert_cmphex(qtest_readq(s, lb + 0xc028), ==, sentinel);
    g_assert_cmphex(qtest_readq(s, lb + 0xc030), ==, sentinel);
}

static uint64_t experimental_readq(QTestState *s, unsigned cpu, uint64_t addr)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(s, cpu, addr, bytes, sizeof(bytes)));
    return ldq_be_p(bytes);
}

static void experimental_writeq(QTestState *s, unsigned cpu, uint64_t addr,
                                uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(s, cpu, addr, bytes, sizeof(bytes)));
}

static uint64_t experimental_status_vector_from(QTestState *s, unsigned cpu,
                                                 uint64_t address,
                                                 uint64_t route, bool write,
                                                 uint64_t data, bool accepted)
{
    const uint64_t lb = SPACE_IO + UINT64_C(0x01000000) + 0x600000;
    const uint64_t params = address | (UINT64_C(0x5a) << 40);
    const uint64_t sentinel = UINT64_C(0xdeadbeefdeadbeef);
    uint64_t response;

    experimental_readq(s, cpu, lb + 0xc038);
    experimental_writeq(s, cpu, lb + 0xc030, sentinel);
    experimental_writeq(s, cpu, lb + 0xc028, sentinel);
    experimental_writeq(s, cpu, lb + 0xc008, route);
    experimental_writeq(s, cpu, lb + 0xc010, data);
    experimental_writeq(s, cpu, lb + 0xc000, params | write);
    response = experimental_readq(s, cpu, lb + 0xc020);
    g_assert_cmphex(response, ==, accepted ?
                    ((UINT64_C(1) << 63) | params | write) : 0);
    if (!accepted) {
        g_assert_cmphex(experimental_readq(s, cpu, lb + 0xc028), ==, sentinel);
    }
    response = experimental_readq(s, cpu, lb + 0xc030);
    if (write || !accepted) {
        g_assert_cmphex(response, ==, sentinel);
    }
    return response;
}

static uint64_t experimental_status_vector(QTestState *s, uint64_t address,
                                           uint64_t route, bool write,
                                           uint64_t data, bool accepted)
{
    return experimental_status_vector_from(s, 0, address, route, write, data,
                                           accepted);
}

static void assert_experimental_status(uint64_t status, unsigned ingress,
                                       unsigned disconnected)
{
    static const unsigned state[8] = { 2, 1, 1, 2, 1, 2, 1, 1 };

    g_assert_cmphex(status & 1, ==, 1);
    g_assert_cmphex((status >> 1) & 0x7ff, ==, 0x24);
    g_assert_cmphex((status >> 12) & 0xffff, ==, 0x3017);
    g_assert_cmphex((status >> 28) & 15, ==, 1);
    g_assert_cmphex((status >> 32) & 15, ==, 0);
    for (unsigned p = 1; p <= 8; p++) {
        g_assert_cmphex((status >> (34 + 2 * p)) & 3, ==,
                        p == disconnected ? 1 : state[p - 1]);
    }
    g_assert_cmphex((status >> 52) & 15, ==, ingress);
    g_assert_cmphex(status >> 56, ==, 0);
}

static size_t experimental_router_l1_reply(QTestState *s, uint8_t reply[131])
{
    const uint8_t request[] = { 19, 0x00, 0x04, 0x10, 6, 0, 3, 0 };

    sgi_l1_test_send(s, BASE + 0x240, request, sizeof(request), false);
    return sgi_l1_test_receive(s, BASE + 0x240, reply, 131);
}

static void test_experimental_status_write(void)
{
    g_autofree char *image = NULL;
    g_autofree char *prom = NULL;
    g_autofree char *to_image = NULL;
    g_autofree char *dir = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *s, *to;
    int fd;
    const uint64_t values[] = { 0, UINT64_MAX,
        UINT64_C(0x123456789abcdef0) };
    uint64_t status;
    uint64_t vector_status;
    uint64_t port_before[8][4];
    uint64_t global_before[5];
    uint64_t route_before;
    uint64_t port_reset_before;
    const uint64_t local0 = bank[0] + 0x200;
    const uint64_t global0 = bank[0] + 0x100;
    const uint64_t global1 = bank[0] + 0x108;
    const uint64_t global8 = bank[7] + 0x178;
    const uint64_t patterns[] = { 0x8, 0x10, 0x800, 0x1000 };
    const uint64_t expected[] = { 0x8, 0x10, 0x800, 0x1000 };
    const uint64_t route_patterns[] = { 0x8, 0x800, 0x1000 };
    QDict *mode_reply;
    uint8_t l1_before[131], l1_after[131];
    size_t l1_size;

    fd = g_file_open_tmp("sn1-router-status-storage-XXXXXX", &image, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * MiB), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    fd = g_file_open_tmp("sn1-router-status-prom-XXXXXX", &prom, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 1476264), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    s = qtest_initf(
        "-M origin300,topology=origin300-numalink-pci-expansion-experiment,"
        "expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device sgi-sn1-router-test,id=test,"
        "router=/machine/module3/router", prom, image);
    qtest_writeq(s, BASE + 0x20, 0x12345678);
    qtest_writeq(s, BASE + 0x28, 0x87654321);
    qtest_writeq(s, BASE + 0x100, 0xaabbccdd);
    qtest_writeq(s, BASE + bank[3] + 0x200 + 5 * 8, 2);
    set(s, "destination", 5);
    set(s, "turn", 3);
    for (unsigned p = 0; p < 8; p++) {
        qtest_writeq(s, BASE + bank[p], 0x1200 + p);
        port_before[p][0] = qtest_readq(s, BASE + bank[p]);
        port_before[p][1] = qtest_readq(s, BASE + bank[p] + 8);
        port_before[p][2] = qtest_readq(s, BASE + bank[p] + 0x10);
        port_before[p][3] = qtest_readq(s, BASE + bank[p] + 0x88);
    }
    global_before[0] = qtest_readq(s, BASE + 0x20);
    global_before[1] = qtest_readq(s, BASE + 0x28);
    global_before[2] = qtest_readq(s, BASE + 0x50);
    global_before[3] = qtest_readq(s, BASE + 0x100);
    global_before[4] = qtest_readq(s, BASE + 0x10);
    /* Reserved nibble bits distinguish the experiment mask from fields. */
    for (unsigned p = 0; p < G_N_ELEMENTS(bank); p++) {
        uint64_t addr = bank[p] + 0x200 + (p & 1 ? 127 * 8 : 0);

        experimental_status_vector(s, addr, 0, true, UINT64_MAX - p, true);
        g_assert_cmphex(experimental_status_vector(s, addr, 0, false, 0,
                                                   true), ==, UINT64_MAX - p);
        g_assert_cmphex(experimental_status_vector(s,
                        bank[p] + 0x200 + (p & 1 ? 126 * 8 : 8), 0,
                        false, 0, true), ==, 0);
        experimental_status_vector(s, addr, 0, true, 0, true);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(patterns); i++) {
        experimental_status_vector(s, local0, 0, true, patterns[i], true);
        g_assert_cmphex(experimental_status_vector(s, local0, 0, false, 0,
                                                   true), ==, expected[i]);
    }
    experimental_status_vector(s, global0, 0, true, UINT64_MAX, true);
    g_assert_cmphex(experimental_status_vector(s, global0, 0, false, 0,
                                               true), ==, UINT64_MAX);
    for (unsigned i = 0; i < G_N_ELEMENTS(patterns); i++) {
        experimental_status_vector(s, global0, 0, true, patterns[i], true);
        g_assert_cmphex(experimental_status_vector(s, global0, 0, false, 0,
                                                   true), ==,
                        patterns[i]);
    }
    experimental_status_vector(s, global0, 0, true, 0x3, true);
    experimental_status_vector(s, global1, 0, true, 0x123a, true);
    experimental_status_vector(s, global8, 0, true, 0x100d, true);
    g_assert_cmphex(experimental_status_vector(s, global0, 0, false, 0,
                                               true), ==, 3);
    g_assert_cmphex(experimental_status_vector(s, global1, 0, false, 0,
                                               true), ==, 0x123a);
    g_assert_cmphex(experimental_status_vector(s, global8, 0, false, 0,
                                               true), ==, 0x100d);
    g_assert_cmphex(experimental_status_vector(s, bank[7] + 0x170, 0,
                                               false, 0, true), ==, 0);
    experimental_status_vector(s, 0x30, 0, true, UINT64_MAX, false);
    qtest_writeq(s, BASE + 0x30, UINT64_MAX);
    g_assert_cmpuint(get(s, "failure"), ==, 5);
    g_assert_cmphex(experimental_status_vector(s, 0x30, 0, false, 0,
                                               true), ==, 0);
    experimental_status_vector(s, local0, 0, true, 0, true);
    qtest_readl(s, BASE + local0);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_readq(s, BASE + local0 + 1);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_readl(s, BASE + global0);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_readq(s, BASE + global0 + 1);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_writel(s, BASE + global0, UINT32_MAX);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_writeq(s, BASE + global0 + 1, UINT64_MAX);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    g_assert_cmphex(experimental_status_vector(s, global0, 0, false, 0,
                                               true), ==, 3);
    mode_reply = qtest_qmp(s,
        "{'execute':'qom-set','arguments':{"
        "'path':'/machine/module3/router',"
        "'property':'synthetic-diagnostics','value':false}}");

    g_assert_true(qdict_haskey(mode_reply, "error"));
    qobject_unref(mode_reply);
    route_before = get(s, "route");
    g_assert_cmphex(route_before, ==, 4 | (2 << 8));
    for (unsigned i = 0; i < G_N_ELEMENTS(route_patterns); i++) {
        experimental_status_vector(s, bank[3] + 0x200 + 5 * 8, 0,
                                   true, 2 | route_patterns[i], true);
        g_assert_cmphex(get(s, "route"), ==, route_before);
    }
    experimental_status_vector(s, bank[3] + 0x200 + 5 * 8, 0,
                               true, 2, true);
    experimental_status_vector(s, 0x10, 0, true, UINT64_MAX, true);
    g_assert_cmphex(experimental_status_vector(s, 0x10, 0, false, 0, true),
                    ==, 0x3fff);
    experimental_status_vector(s, 0x10, 0, true,
                               UINT64_C(0xa5c3000012345a6b), true);
    g_assert_cmphex(experimental_status_vector(s, 0x10, 0, false, 0, true),
                    ==, 0x1a6b);
    experimental_status_vector(s, 0x10, 0, true, 1 << 8, true);
    /* Protection bits are stored but do not alter the bounded route. */
    g_assert_cmphex(get(s, "route"), ==, route_before);
    experimental_status_vector(s, 0x10, 0, true,
                               (1 << 8) | (1 << 12), true);
    g_assert_cmphex(get(s, "route"), ==, route_before);
    experimental_status_vector(s, 0x10, 0, true,
                               UINT64_C(0xa5c3000000001100), true);
    g_assert_cmphex(get(s, "route"), ==, route_before);
    g_assert_cmphex(experimental_status_vector(s, 0x10, 0, false, 0, true),
                    ==, 0x1100);
    experimental_status_vector(s, 0x10, 0, true, 0, true);
    l1_size = experimental_router_l1_reply(s, l1_before);
    experimental_status_vector(s, 0x8000, 0, true, UINT64_MAX, true);
    g_assert_cmphex(experimental_status_vector(s, 0x8000, 0, false, 0,
                                               true), ==, UINT64_MAX);
    g_assert_cmphex(experimental_status_vector(s, 0x8008, 0, false, 0,
                                               true), ==, 0);
    experimental_status_vector(s, 0x80f8, 0, true,
                               UINT64_C(0xa5c30000deadbeef), true);
    g_assert_cmphex(experimental_status_vector(s, 0x80f8, 0, false, 0,
                                               true), ==,
                    UINT64_C(0xa5c30000deadbeef));
    g_assert_cmphex(experimental_status_vector(s, 0x80f0, 0, false, 0,
                                               true), ==, 0);
    g_assert_cmphex(experimental_status_vector(s, 0x8000, 0, false, 0,
                                               true), ==, UINT64_MAX);
    qtest_writel(s, BASE + 0x8000, 0);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_readq(s, BASE + 0x8001);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_writeq(s, BASE + 0x8001, 0);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_readq(s, BASE + 0x8100);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_writeq(s, BASE + 0x8100, 0);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    g_assert_cmphex(qtest_readq(s, BASE + 0x8000), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, BASE + 0x80f8), ==,
                    UINT64_C(0xa5c30000deadbeef));
    qtest_writeq(s, BASE + bank[0] + 0x18, 0xa5);
    port_reset_before = qtest_readq(s, BASE + bank[0] + 0x18);
    for (unsigned bit = 0; bit < 16; bit++) {
        uint64_t pattern = UINT64_C(1) << bit;

        experimental_status_vector(s, 0x18, 0, true, pattern, true);
        g_assert_cmphex(experimental_status_vector(s, 0x18, 0, false, 0,
                                                  true), ==, pattern);
        experimental_status_vector(s, 0x18, 0, true, ~pattern, true);
        g_assert_cmphex(experimental_status_vector(s, 0x18, 0, false, 0,
                                                  true), ==,
                        (~pattern & UINT64_C(0xffff)));
    }
    experimental_status_vector(s, 0x18, 0, true, UINT64_MAX, true);
    g_assert_cmphex(experimental_status_vector(s, 0x18, 0, false, 0, true),
                    ==, 0xffff);
    experimental_status_vector(s, 0x18, 0, true, 0, true);
    g_assert_cmphex(experimental_status_vector(s, 0x18, 0, false, 0, true),
                    ==, 0);
    experimental_status_vector(s, 0x18, 0, true,
                               UINT64_C(0x123456789abcdef0), true);
    g_assert_cmphex(experimental_status_vector(s, 0x18, 0, false, 0, true),
                    ==, 0xdef0);
    g_assert_cmphex(qtest_readq(s, BASE + bank[0] + 0x18), ==,
                    port_reset_before);
    g_assert_cmphex(qtest_readq(s, BASE + 8), ==, 0);
    status = experimental_status_vector(s, 0, 0, false, 0, true);
    vector_status = status;
    assert_experimental_status(status, 1, 0);
    assert_experimental_status(
        experimental_status_vector(s, 0x600000, 0, false, 0, true), 1, 0);
    assert_experimental_status(
        experimental_status_vector_from(s, 2, 0, 0, false, 0, true), 6, 0);
    assert_experimental_status(
        experimental_status_vector_from(s, 2, 0x600000, 0, false, 0, true),
        6, 0);
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/slave-route','property':'down','value':true}}");
    assert_experimental_status(
        experimental_status_vector(s, 0, 0, false, 0, true), 1, 6);
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/slave-route','property':'down','value':false}}");
    g_assert_cmphex(experimental_status_vector(s, 0, 0, false, 0, true),
                    ==, vector_status);
    for (unsigned i = 0; i < G_N_ELEMENTS(values); i++) {
        experimental_status_vector(s, 0, 0, true, values[i], false);
        g_assert_cmphex(experimental_status_vector(s, 0, 0, false, 0, true),
                        ==, status);
        g_assert_cmphex(experimental_status_vector(s, 0x600000, 0, false,
                                                  0, true), ==, status);
        g_assert_cmphex(get(s, "route"), ==, route_before);
        for (unsigned p = 0; p < 8; p++) {
            g_assert_cmphex(qtest_readq(s, BASE + bank[p]), ==,
                            port_before[p][0]);
            g_assert_cmphex(qtest_readq(s, BASE + bank[p] + 8), ==,
                            port_before[p][1]);
            g_assert_cmphex(qtest_readq(s, BASE + bank[p] + 0x10), ==,
                            port_before[p][2]);
            g_assert_cmphex(qtest_readq(s, BASE + bank[p] + 0x88), ==,
                            port_before[p][3]);
        }
        g_assert_cmphex(qtest_readq(s, BASE + 0x20), ==, global_before[0]);
        g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, global_before[1]);
        g_assert_cmphex(qtest_readq(s, BASE + 0x50), ==, global_before[2]);
        g_assert_cmphex(qtest_readq(s, BASE + 0x100), ==, global_before[3]);
        g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, global_before[4]);
    }
    g_assert_cmpuint(experimental_router_l1_reply(s, l1_after), ==,
                     l1_size);
    g_assert_cmpmem(l1_after, l1_size, l1_before, l1_size);
    g_assert_cmphex(experimental_status_vector(s, 0x18, 0, false, 0, true),
                    ==, 0xdef0);
    experimental_status_vector(s, 0x18, 9, true, 0, false);
    experimental_status_vector(s, 0x600018, 0, true, 0, false);
    qtest_writeq(s, BASE + 0x19, 0);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_writel(s, BASE + 0x18, 0);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    experimental_status_vector(s, 0x600000, 0, true, 0, false);
    experimental_status_vector(s, 0, 9, true, 0, false);
    for (unsigned ingress = 1; ingress <= 6; ingress += 5) {
        set(s, "ingress", ingress);
        status = qtest_readq(s, BASE);
        assert_experimental_status(status, ingress, 0);
        g_assert_cmphex(qtest_readq(s, BASE + 0x600000), ==, status);
        for (unsigned i = 0; i < G_N_ELEMENTS(values); i++) {
            qtest_writeq(s, BASE, values[i]);
            g_assert_cmpuint(get(s, "result"), ==, 1);
            g_assert_cmpuint(get(s, "failure"), ==, 4);
            g_assert_cmphex(qtest_readq(s, BASE), ==, status);
        }
    }
    set(s, "ingress", 0);
    qtest_writeq(s, BASE, 0);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    set(s, "ingress", 9);
    qtest_writeq(s, BASE, 0);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    set(s, "ingress", 1);
    qtest_writeq(s, BASE + 1, 0);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_writel(s, BASE, 0);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_writew(s, BASE, 0);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_writeb(s, BASE, 0);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    g_assert_cmphex(experimental_status_vector(s, 0, 0, false, 0, true),
                    ==, vector_status);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmphex(experimental_status_vector(s, 0x10, 0, false, 0, true),
                    ==, 0);
    g_assert_cmphex(experimental_status_vector(s, 0x8000, 0, false, 0,
                                               true), ==, 0);
    g_assert_cmphex(experimental_status_vector(s, 0x80f8, 0, false, 0,
                                               true), ==, 0);
    g_assert_cmphex(experimental_status_vector(s, 0x18, 0, false, 0, true),
                    ==, 0);
    g_assert_cmphex(experimental_status_vector(s, bank[0] + 0x200, 0,
                                               false, 0, true), ==, 0);
    g_assert_cmphex(experimental_status_vector(s, global0, 0, false, 0,
                                               true), ==, 0);
    g_assert_cmphex(experimental_status_vector(s, global1, 0, false, 0,
                                               true), ==, 0);
    g_assert_cmphex(experimental_status_vector(s, global8, 0, false, 0,
                                               true), ==, 0);
    g_assert_cmphex(experimental_status_vector(s,
                    bank[7] + 0x200 + 127 * 8, 0, false, 0, true), ==, 0);
    experimental_status_vector(s, 0, 0, true, UINT64_MAX, false);
    status = experimental_status_vector(s, 0, 0, false, 0, true);
    assert_experimental_status(status, 1, 0);
    assert_experimental_status(
        experimental_status_vector_from(s, 2, 0, 0, false, 0, true), 6, 0);
    assert_experimental_status(
        experimental_status_vector_from(s, 2, 0x600000, 0, false, 0, true),
        6, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x100), ==, 0);
    experimental_status_vector(s, 0x18, 0, true, 0xabcd, true);
    experimental_status_vector(s, 0x10, 0, true,
                               UINT64_C(0xa5c3000012345a6b), true);
    experimental_status_vector(s, 0x8000, 0, true, UINT64_MAX, true);
    experimental_status_vector(s, 0x80f8, 0, true,
                               UINT64_C(0xa5c30000deadbeef), true);
    experimental_status_vector(s, bank[0] + 8, 0, true, 0, false);
    experimental_status_vector(s, bank[5] + 0x88, 0, true, 0, false);
    experimental_status_vector(s, bank[0] + 0x200, 0, true,
                               UINT64_C(0xa5c3000000000803), true);
    experimental_status_vector(s, bank[7] + 0x200 + 127 * 8, 0, true,
                               UINT64_C(0x5a3c000000000bbb), true);
    experimental_status_vector(s, global0, 0, true, UINT64_MAX, true);
    experimental_status_vector(s, global1, 0, true, 0x123a, true);
    experimental_status_vector(s, global8, 0, true, 0x100d, true);
    fd = g_file_open_tmp("sn1-router-status-destination-XXXXXX",
                         &to_image, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * MiB), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    dir = g_dir_make_tmp("sn1-router-status-migrate-XXXXXX", &error);
    g_assert_no_error(error);
    socket = g_build_filename(dir, "socket", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    to = qtest_initf(
        "-M origin300,topology=origin300-numalink-pci-expansion-experiment,"
        "expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device sgi-sn1-router-test,id=test,"
        "router=/machine/module3/router -incoming defer", prom, to_image);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(s,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(s, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(experimental_status_vector(to, 0x10, 0, false, 0, true),
                    ==, 0x1a6b);
    g_assert_cmphex(experimental_status_vector(to, 0x8000, 0, false, 0,
                                               true), ==, UINT64_MAX);
    g_assert_cmphex(experimental_status_vector(to, 0x80f8, 0, false, 0,
                                               true), ==,
                    UINT64_C(0xa5c30000deadbeef));
    g_assert_cmphex(experimental_status_vector(to, 0x18, 0, false, 0, true),
                    ==, 0xabcd);
    g_assert_cmphex(experimental_status_vector(to, bank[0] + 8, 0,
                                               false, 0, true), ==, 0);
    g_assert_cmphex(experimental_status_vector(to, bank[5] + 0x88, 0,
                                               false, 0, true), ==, 0);
    g_assert_cmphex(experimental_status_vector(to, bank[0] + 0x200, 0,
                                               false, 0, true), ==,
                    UINT64_C(0xa5c3000000000803));
    g_assert_cmphex(experimental_status_vector(to, global0, 0, false, 0,
                                               true), ==, UINT64_MAX);
    g_assert_cmphex(experimental_status_vector(to, global1, 0, false, 0,
                                               true), ==, 0x123a);
    g_assert_cmphex(experimental_status_vector(to, global8, 0, false, 0,
                                               true), ==, 0x100d);
    g_assert_cmphex(experimental_status_vector(to,
                    bank[7] + 0x200 + 127 * 8, 0, false, 0, true), ==,
                    UINT64_C(0x5a3c000000000bbb));
    experimental_status_vector(to, bank[0] + 8, 0, true, 0, false);
    experimental_status_vector(to, bank[5] + 0x88, 0, true, 0, false);
    experimental_status_vector(to, bank[0] + 8, 0, true, 1, false);
    experimental_status_vector(to, bank[5] + 0x88, 0, true, 1, false);
    g_assert_cmphex(experimental_status_vector(to, 0, 0, false, 0, true),
                    ==, status);
    assert_experimental_status(status, 1, 0);
    g_assert_cmphex(experimental_status_vector(to, 0x600000, 0, false, 0,
                                              true), ==, status);
    assert_experimental_status(
        experimental_status_vector_from(to, 2, 0, 0, false, 0, true), 6, 0);
    assert_experimental_status(
        experimental_status_vector_from(to, 2, 0x600000, 0, false, 0, true),
        6, 0);
    experimental_status_vector(to, 0, 0, true,
                               UINT64_C(0x123456789abcdef0), false);
    g_assert_cmphex(experimental_status_vector(to, 0, 0, false, 0, true),
                    ==, status);
    experimental_status_vector(to, 0x600000, 0, true, 0, false);
    experimental_status_vector(to, 8, 0, true, 0, false);
    g_assert_cmphex(experimental_status_vector(to, 8, 0, false, 0, true),
                    ==, 0);
    qtest_quit(to);
    qtest_quit(s);
    g_assert_cmpint(g_unlink(to_image), ==, 0);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    g_assert_cmpint(g_unlink(image), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

static void assert_local_reset_state(QTestState *s, uint64_t status)
{
    const unsigned links[] = { 3, 7 };

    g_assert_cmphex(experimental_status_vector(s, 0, 0, false, 0, true),
                    ==, status);
    g_assert_cmphex(experimental_status_vector(s, 0x600000, 0, false, 0,
                                               true), ==, status);
    g_assert_cmphex(experimental_status_vector(s, 0x8, 0, false, 0, true),
                    ==, 0);
    g_assert_cmphex(experimental_status_vector(s, 0x10, 0, false, 0, true),
                    ==, 0);
    g_assert_cmphex(experimental_status_vector(s, 0x18, 0, false, 0, true),
                    ==, 0);
    g_assert_cmphex(experimental_status_vector(s, 0x20, 0, false, 0, true),
                    ==, 0);
    g_assert_cmphex(experimental_status_vector(s, 0x28, 0, false, 0, true),
                    ==, 0);
    g_assert_cmphex(experimental_status_vector(s, 0x38, 0, false, 0, true),
                    ==, UINT64_C(0xffff) << 10);
    g_assert_cmphex(experimental_status_vector(s, 0x40, 0, false, 0, true),
                    ==, 0);
    g_assert_cmphex(experimental_status_vector(s, 0x48, 0, false, 0, true),
                    ==, 0);
    g_assert_cmphex(experimental_status_vector(s, 0x50, 0, false, 0, true),
                    ==, UINT64_C(3) << 20);
    for (unsigned i = 0; i < 5; i++) {
        g_assert_cmphex(experimental_status_vector(s, 0x100 + 8 * i, 0,
                                                   false, 0, true), ==, 0);
    }
    g_assert_cmphex(experimental_status_vector(s, 0x8000, 0, false, 0,
                                               true), ==, 0);
    g_assert_cmphex(experimental_status_vector(s, 0x80f8, 0, false, 0,
                                               true), ==, 0);
    for (unsigned i = 0; i < G_N_ELEMENTS(links); i++) {
        unsigned p = links[i];

        g_assert_cmphex(experimental_status_vector(s, bank[p], 0,
                                                   false, 0, true), ==,
                        0x1200 + p);
        g_assert_cmphex(experimental_status_vector(s, bank[p] + 0x10, 0,
                                                   false, 0, true), ==,
                        0x400 + p);
        g_assert_cmphex(experimental_status_vector(s, bank[p] + 0x18, 0,
                                                   false, 0, true), ==,
                        0x80 + p);
        g_assert_cmphex(experimental_status_vector(s, bank[p] + 0x100, 0,
                                                   false, 0, true), ==,
                        0xa + i);
        g_assert_cmphex(experimental_status_vector(s, bank[p] + 0x200 +
                                                   5 * 8, 0, false, 0,
                                                   true), ==, 2 + i);
    }
    g_assert_cmphex(get(s, "route"), ==, 4 | (2 << 8));
}

static void test_experimental_local_reset(void)
{
    const unsigned links[] = { 3, 7 };
    const uint64_t local_offsets[] = {
        0x10, 0x18, 0x20, 0x28, 0x38, 0x40, 0x48, 0x50,
        0x100, 0x108, 0x110, 0x118, 0x120, 0x8000, 0x80f8,
    };
    const uint64_t local_values[] = {
        0x100, 0x55aa, 0x1234, 0x5678, 0x155, 0x2345, 0x4567, 0xa5,
        0x111, 0x222, 0x333, 1, 1, 0x444, 0x555,
    };
    g_autofree char *image = NULL;
    g_autofree char *prom = NULL;
    g_autofree char *to_image = NULL;
    g_autofree char *dir = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autoptr(GError) error = NULL;
    uint8_t l1_before[131], l1_after[131];
    size_t l1_size;
    QTestState *s, *to, *ordinary;
    uint64_t status;
    int fd;

    ordinary = qtest_init(ARGS);
    qtest_writeq(ordinary, BASE + 0x8, 0);
    g_assert_cmpuint(get(ordinary, "failure"), ==, 4);
    qtest_writeq(ordinary, BASE + 0x8, 1);
    g_assert_cmpuint(get(ordinary, "failure"), ==, 4);
    g_assert_cmphex(qtest_readq(ordinary, BASE + 0x8), ==, 0);
    qtest_quit(ordinary);

    fd = g_file_open_tmp("sn1-router-local-reset-storage-XXXXXX",
                         &image, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * MiB), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    fd = g_file_open_tmp("sn1-router-local-reset-prom-XXXXXX", &prom,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 1476264), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    s = qtest_initf(
        "-M origin300,topology=origin300-numalink-pci-expansion-experiment,"
        "expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device sgi-sn1-router-test,id=test,"
        "router=/machine/module3/router", prom, image);
    set(s, "destination", 5);
    set(s, "turn", 3);
    status = experimental_status_vector(s, 0, 0, false, 0, true);
    g_assert_cmphex(experimental_status_vector(s, 0x600000, 0,
                                               false, 0, true), ==, status);
    l1_size = experimental_router_l1_reply(s, l1_before);
    for (unsigned i = 0; i < G_N_ELEMENTS(local_offsets); i++) {
        experimental_status_vector(s, local_offsets[i], 0, true,
                                   local_values[i], true);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(links); i++) {
        unsigned p = links[i];

        experimental_status_vector(s, bank[p], 0, true, 0x1200 + p, true);
        experimental_status_vector(s, bank[p] + 0x10, 0, true,
                                   0x400 + p, true);
        experimental_status_vector(s, bank[p] + 0x18, 0, true,
                                   0x80 + p, true);
        experimental_status_vector(s, bank[p] + 0x100, 0, true,
                                   0xa + i, true);
        experimental_status_vector(s, bank[p] + 0x200 + 5 * 8, 0,
                                   true, 2 + i, true);
    }
    g_assert_cmphex(get(s, "route"), ==, 4 | (2 << 8));
    experimental_status_vector(s, 0x8, 0, true, 0, false);
    g_assert_cmphex(experimental_status_vector(s, 0x10, 0,
                                               false, 0, true), ==, 0x100);
    experimental_status_vector(s, 0x8, 0, true, 2, false);
    experimental_status_vector(s, 0x8, 0, true, 1 << 9, false);
    experimental_status_vector(s, 0x8, 0, true, 3, false);
    g_assert_cmphex(experimental_status_vector(s, 0x10, 0,
                                               false, 0, true), ==, 0x100);
    qtest_readl(s, BASE + 0x8);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_readq(s, BASE + 0x9);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_writel(s, BASE + 0x8, 1);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_writeq(s, BASE + 0x9, 1);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    g_assert_cmphex(get(s, "route"), ==, 4 | (2 << 8));
    experimental_status_vector(s, 0x8, 0, true, 1, true);
    assert_local_reset_state(s, status);
    g_assert_cmphex(experimental_status_vector(s, 0x518, 0, false, 0,
                                               true), ==, 0);
    g_assert_cmphex(experimental_status_vector(s, 0x118, 0, false, 0,
                                               true), ==, 1);
    g_assert_cmpuint(experimental_router_l1_reply(s, l1_after), ==,
                     l1_size);
    g_assert_cmpmem(l1_after, l1_size, l1_before, l1_size);
    experimental_status_vector(s, 0x100, 0, true, 0xaaa, true);
    experimental_status_vector(s, 0x8, 0, true, 1, true);
    assert_local_reset_state(s, status);

    fd = g_file_open_tmp("sn1-router-local-reset-destination-XXXXXX",
                         &to_image, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * MiB), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    dir = g_dir_make_tmp("sn1-router-local-reset-migrate-XXXXXX", &error);
    g_assert_no_error(error);
    socket = g_build_filename(dir, "socket", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    to = qtest_initf(
        "-M origin300,topology=origin300-numalink-pci-expansion-experiment,"
        "expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device sgi-sn1-router-test,id=test,"
        "router=/machine/module3/router -incoming defer", prom, to_image);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(s,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(s, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
    set(to, "destination", 5);
    set(to, "turn", 3);
    assert_local_reset_state(to, status);
    g_assert_cmphex(experimental_status_vector(to, 0x518, 0, false, 0,
                                               true), ==, 0);
    qtest_qmp_assert_success(to, "{'execute':'system_reset'}");
    g_assert_cmphex(experimental_status_vector(to, bank[3] + 0x200 +
                                               5 * 8, 0, false, 0,
                                               true), ==, 0);
    g_assert_cmphex(experimental_status_vector(to, bank[7] + 0x100, 0,
                                               false, 0, true), ==, 0);
    g_assert_cmphex(experimental_status_vector(to, bank[3], 0, false, 0,
                                               true), ==, 0);
    g_assert_cmphex(get(to, "route"), ==, 4);
    qtest_quit(to);
    qtest_quit(s);
    g_assert_cmpint(g_unlink(to_image), ==, 0);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    g_assert_cmpint(g_unlink(image), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

static void test_experimental_port_errors(void)
{
    const uint64_t lb = SPACE_IO + UINT64_C(0x01000000) + 0x600000;
    const uint64_t params = (bank[0] + 8) | (UINT64_C(0x5a) << 40);
    const uint64_t sentinel = UINT64_C(0xdeadbeefdeadbeef);
    g_autofree char *image = NULL;
    g_autofree char *prom = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *s;
    uint64_t status, route;
    int fd;

    fd = g_file_open_tmp("sn1-router-port-storage-XXXXXX", &image, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * MiB), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    fd = g_file_open_tmp("sn1-router-port-prom-XXXXXX", &prom, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 1476264), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    s = qtest_initf(
        "-M origin300,topology=origin300-numalink-pci-expansion-experiment,"
        "expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device sgi-sn1-router-test,id=test,"
        "router=/machine/module3/router", prom, image);

    for (unsigned phase = 0; phase < 2; phase++) {
        qtest_writeq(s, BASE + 0x20, 0x1234);
        qtest_writeq(s, BASE + bank[0], 0x123);
        qtest_writeq(s, BASE + bank[5], 0x456);
        status = experimental_status_vector(s, 0, 0, false, 0, true);
        route = get(s, "route");
        for (unsigned i = 0; i < 2; i++) {
            uint64_t address = i ? bank[5] + 0x88 : bank[0] + 8;

            g_assert_cmphex(experimental_status_vector(s, address, 0,
                                                        false, 0, true),
                            ==, 0);
            experimental_status_vector(s, address, 0, true, 0, false);
            experimental_status_vector(s, address, 0, true, 1, false);
            experimental_status_vector(s, address, 0, true, UINT64_MAX,
                                       false);
            g_assert_cmphex(experimental_status_vector(s, address, 0,
                                                        false, 0, true),
                            ==, 0);
            qtest_writel(s, BASE + address, 0);
            g_assert_cmpuint(get(s, "failure"), ==, 4);
            qtest_writeq(s, BASE + address + 1, 0);
            g_assert_cmpuint(get(s, "failure"), ==, 4);
            qtest_readl(s, BASE + address);
            g_assert_cmpuint(get(s, "failure"), ==, 4);
            qtest_readq(s, BASE + address + 1);
            g_assert_cmpuint(get(s, "failure"), ==, 4);
        }
        g_assert_cmphex(qtest_readq(s, BASE + bank[0]), ==, 0x123);
        g_assert_cmphex(qtest_readq(s, BASE + bank[5]), ==, 0x456);
        g_assert_cmphex(qtest_readq(s, BASE + 0x20), ==, 0x1234);
        g_assert_cmphex(experimental_status_vector(s, 0, 0, false, 0,
                                                   true), ==, status);
        g_assert_cmphex(get(s, "route"), ==, route);
        qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
            "'path':'/machine/master-route','property':'down','value':true}}");
        experimental_readq(s, 0, lb + 0xc038);
        experimental_writeq(s, 0, lb + 0xc030, sentinel);
        experimental_writeq(s, 0, lb + 0xc008, 0);
        experimental_writeq(s, 0, lb + 0xc010, 0);
        experimental_writeq(s, 0, lb + 0xc000, params | 1);
        g_assert_cmphex(experimental_readq(s, 0, lb + 0xc020), ==, 0);
        g_assert_cmphex(experimental_readq(s, 0, lb + 0xc030), ==, sentinel);
        qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
            "'path':'/machine/master-route','property':'down','value':false}}");
        g_assert_cmphex(experimental_status_vector(s, bank[0] + 8, 0,
                                                   false, 0, true), ==, 0);
        qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    }
    qtest_quit(s);
    g_assert_cmpint(g_unlink(image), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

static void test_experimental_zero_reset_restore(void)
{
    g_autofree char *image = NULL;
    g_autofree char *prom = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *s;
    uint8_t l1_before[131], l1_after[131];
    size_t l1_size;
    uint64_t status, route, diagnostic, port_status, port_error;
    int fd;

    fd = g_file_open_tmp("sn1-router-reset-storage-XXXXXX", &image, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * MiB), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    fd = g_file_open_tmp("sn1-router-reset-prom-XXXXXX", &prom, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 1476264), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    s = qtest_initf(
        "-M origin300,topology=origin300-numalink-pci-expansion-experiment,"
        "expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device sgi-sn1-router-test,id=test,"
        "router=/machine/module3/router", prom, image);
    qtest_writeq(s, BASE + 0x20, 0x1234);
    qtest_writeq(s, BASE + 0x10, 1);
    qtest_writeq(s, BASE + 0x100, 0xabcdef);
    qtest_writeq(s, BASE + 0x38, 0x5a);
    qtest_writeq(s, BASE + 0x8000, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, BASE + 0x8000), ==, UINT64_MAX);
    qtest_writeq(s, BASE + bank[0], 0x123);
    qtest_writeq(s, BASE + bank[0] + 0x10, 0xff);
    qtest_writeq(s, BASE + bank[0] + 0x18, 0xa5);
    experimental_status_vector(s, 0x18, 0, true, 0xbeef, true);
    qtest_writeq(s, BASE + bank[3] + 0x200 + 5 * 8, 2);
    set(s, "destination", 5);
    set(s, "turn", 3);
    route = get(s, "route");
    g_assert_cmphex(route, ==, 4 | (2 << 8));
    status = experimental_status_vector(s, 0, 0, false, 0, true);
    diagnostic = qtest_readq(s, BASE + 0x38);
    port_status = qtest_readq(s, BASE + bank[0] + 8);
    port_error = qtest_readq(s, BASE + bank[0] + 0x88);
    l1_size = experimental_router_l1_reply(s, l1_before);

    experimental_status_vector(s, 8, 0, true, 0, false);
    g_assert_cmphex(experimental_status_vector(s, 8, 0, false, 0, true),
                    ==, 0);
    g_assert_cmphex(qtest_readq(s, BASE + bank[0] + 8), ==, port_status);
    g_assert_cmphex(qtest_readq(s, BASE + bank[0] + 0x88), ==, port_error);
    for (unsigned bit = 1; bit <= 9; bit++) {
        experimental_status_vector(s, 8, 0, true, UINT64_C(1) << bit,
                                   false);
    }
    g_assert_cmphex(experimental_status_vector(s, 8, 0, false, 0, true),
                    ==, 0);
    g_assert_cmphex(experimental_status_vector(s, 0, 0, false, 0, true),
                    ==, status);
    g_assert_cmphex(get(s, "route"), ==, route);
    g_assert_cmphex(qtest_readq(s, BASE + 0x20), ==, 0x1234);
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1);
    g_assert_cmphex(qtest_readq(s, BASE + 0x100), ==, 0xabcdef);
    g_assert_cmphex(qtest_readq(s, BASE + 0x38), ==, diagnostic);
    g_assert_cmphex(qtest_readq(s, BASE + 0x8000), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, BASE + bank[0]), ==, 0x123);
    g_assert_cmphex(qtest_readq(s, BASE + bank[0] + 0x10), ==, 0xff);
    g_assert_cmphex(qtest_readq(s, BASE + bank[0] + 0x18), ==, 0xa5);
    g_assert_cmphex(qtest_readq(s, BASE + bank[0] + 8), ==, port_status);
    g_assert_cmphex(qtest_readq(s, BASE + bank[0] + 0x88), ==, port_error);
    g_assert_cmphex(experimental_status_vector(s, 0x18, 0, false, 0, true),
                    ==, 0xbeef);
    g_assert_cmphex(qtest_readq(s, BASE + bank[3] + 0x200 + 5 * 8), ==, 2);
    g_assert_cmpuint(experimental_router_l1_reply(s, l1_after), ==,
                     l1_size);
    g_assert_cmpmem(l1_after, l1_size, l1_before, l1_size);
    experimental_status_vector(s, 8, 9, true, 0, false);
    experimental_status_vector(s, 0x600008, 0, true, 0, false);
    qtest_writeq(s, BASE + 9, 0);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_writel(s, BASE + 8, 0);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    experimental_status_vector(s, 8, 0, true, 0, false);
    g_assert_cmphex(experimental_status_vector(s, 8, 0, false, 0, true),
                    ==, 0);
    qtest_quit(s);
    g_assert_cmpint(g_unlink(image), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
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
                            ==, i + p);
        }
        for (unsigned i = 0; i < 16; i++) {
            qtest_writeq(s, BASE + bank[p - 1] + 0x100 + 8 * i, i + p);
            g_assert_cmphex(qtest_readq(s, BASE + bank[p - 1] + 0x100 + 8 * i),
                            ==, i + p);
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
    qtest_quit(s);
}

static void test_error_clear(void)
{
    QTestState *s = qtest_init(ARGS);

    qtest_writeq(s, BASE + 0x10, 0x400);
    qtest_writeq(s, BASE + 0x20, 0x5678);
    qtest_writeq(s, BASE + 0x28, 0x9abc);
    qtest_writeq(s, BASE + 0x50, 0x800);
    for (unsigned i = 0; i < 5; i++) {
        qtest_writeq(s, BASE + 0x100 + 8 * i, i < 3 ? 0x1234 + i : 1);
    }
    for (unsigned p = 0; p < 8; p++) {
        uint64_t address = BASE + bank[p] + 0x88;

        qtest_writeq(s, BASE + bank[p], 0x1234000 + p);
        qtest_writeq(s, BASE + bank[p] + 0x100, p);
        qtest_writeq(s, BASE + bank[p] + 0x200, 7 - p);
        qtest_writeq(s, BASE + 0x8008 + 8 * p, p + 1);
        for (unsigned repeat = 0; repeat < 2; repeat++) {
            g_assert_cmphex(qtest_readq(s, address), ==, 0);
            g_assert_cmpuint(get(s, "result"), ==, 2);
            g_assert_cmphex(qtest_readq(s, BASE + bank[p] + 8), ==, 0);
        }
        set(s, "operation", 2);
        qtest_readq(s, address);
        g_assert_cmpuint(get(s, "failure"), ==, 5);
        set(s, "operation", UINT64_MAX);
        qtest_writeq(s, address, UINT64_MAX);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_readq(s, address + 1);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_readl(s, address);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_readw(s, address);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_readb(s, address);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_writeq(s, address + 1, 0);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_writel(s, address, 0);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_readq(s, address - 8);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_readq(s, address + 8);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        g_assert_cmphex(qtest_readq(s, BASE + bank[p]), ==, 0x1234000 + p);
        g_assert_cmphex(qtest_readq(s, BASE + bank[p] + 0x100), ==, p);
        g_assert_cmphex(qtest_readq(s, BASE + bank[p] + 0x200), ==, 7 - p);
        g_assert_cmphex(qtest_readq(s, BASE + 0x8008 + 8 * p), ==, p + 1);
    }
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 0x400);
    g_assert_cmphex(qtest_readq(s, BASE + 0x20), ==, 0x5678);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x9abc);
    g_assert_cmphex(qtest_readq(s, BASE + 0x50), ==, 0x300800);
    for (unsigned i = 0; i < 5; i++) {
        g_assert_cmphex(qtest_readq(s, BASE + 0x100 + 8 * i), ==,
                        i < 3 ? 0x1234 + i : 1);
    }
    for (unsigned p = 0; p < 8; p++) {
        g_assert_cmphex(qtest_readq(s, BASE + bank[p]), ==, 0x1234000 + p);
        g_assert_cmphex(qtest_readq(s, BASE + bank[p] + 0x100), ==, p);
        g_assert_cmphex(qtest_readq(s, BASE + bank[p] + 0x200), ==, 7 - p);
        g_assert_cmphex(qtest_readq(s, BASE + 0x8008 + 8 * p), ==, p + 1);
    }
    g_assert_cmphex(qtest_readq(s, BASE + 8), ==, 0);
    g_assert_cmpuint(get(s, "result"), ==, 2);
    qtest_writeq(s, BASE + 8, 0);
    g_assert_cmpuint(get(s, "failure"), ==, 4);
    qtest_quit(s);
}

static void test_error_clear_vector(void)
{
    const uint64_t lb = UINT64_C(0x20600000);
    const uint64_t sentinel = UINT64_C(0xdeadbeefdeadbeef);
    const uint64_t valid = UINT64_C(1) << 63;
    QTestState *s = qtest_init(ARGS " -m 256M "
        "-object memory-backend-ram,id=dma,size=2M "
        "-device sgi-bedrock,id=a,revision=2,dma-memory=/objects/dma/dma[0] "
        "-device sgi-net-cable,id=c,end0=a,end1=r,end1-index=1");
    uint64_t status;

    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/a','property':'net-cable',"
        "'value':'/machine/peripheral/c'}}");
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/r','property':'port1-cable',"
        "'value':'/machine/peripheral/c'}}");
    qtest_sysbus_mmio_map(s, "/machine/peripheral/a", 0, 0x20000000);
    status = qtest_readq(s, BASE);
    g_assert_true(status & (UINT64_C(1) << 37));
    expect_ordinary_vector_refusal(s, 0, true);
    for (unsigned p = 0; p < 8; p++) {
        uint64_t params = (bank[p] + 0x88) | (UINT64_C(0x5a) << 40);

        expect_ordinary_vector_refusal(s, bank[p] + 8, true);
        expect_ordinary_vector_refusal(s, bank[p] + 0x88, true);
        for (unsigned write = 0; write <= 1; write++) {
            qtest_readq(s, lb + 0xc038);
            qtest_writeq(s, lb + 0xc030, sentinel);
            qtest_writeq(s, lb + 0xc008, 0);
            qtest_writeq(s, lb + 0xc010, UINT64_MAX);
            qtest_writeq(s, lb + 0xc000, params | write);
            g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==,
                            write ? 0 : valid | params);
            g_assert_cmphex(qtest_readq(s, lb + 0xc030), ==,
                            write ? sentinel : 0);
        }
        g_assert_cmphex(qtest_readq(s, BASE), ==, status);
    }
    qtest_quit(s);
}

static void test_port_reset_vector(void)
{
    const uint64_t lb = UINT64_C(0x20600000);
    const uint64_t sentinel = UINT64_C(0xdeadbeefdeadbeef);
    const uint64_t valid = UINT64_C(1) << 63;
    QTestState *s = qtest_init(ARGS " -m 256M "
        "-object memory-backend-ram,id=dma,size=2M "
        "-device sgi-bedrock,id=a,revision=2,dma-memory=/objects/dma/dma[0] "
        "-device sgi-net-cable,id=c,end0=a,end1=r,end1-index=1");
    uint64_t status;

    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/a','property':'net-cable',"
        "'value':'/machine/peripheral/c'}}");
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/r','property':'port1-cable',"
        "'value':'/machine/peripheral/c'}}");
    qtest_sysbus_mmio_map(s, "/machine/peripheral/a", 0, 0x20000000);
    status = qtest_readq(s, BASE);
    g_assert_true(status & (UINT64_C(1) << 37));
    expect_ordinary_vector_refusal(s, 8, true);
    qtest_writeq(s, BASE + 0x100, 0x12345678);
    qtest_writeq(s, BASE + bank[0], 0x1234567);

    for (unsigned phase = 0; phase < 2; phase++) {
        const uint64_t scratch = phase ? 0 : 0x12345678;
        const uint64_t parameter = phase ? 0 : 0x1234567;
        const uint64_t params = 0x8 | (UINT64_C(0x5a) << 40);

        qtest_readq(s, lb + 0xc038);
        qtest_writeq(s, lb + 0xc030, sentinel);
        qtest_writeq(s, lb + 0xc008, 0);
        qtest_writeq(s, lb + 0xc010, UINT64_MAX);
        qtest_writeq(s, lb + 0xc000, params);
        g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==, valid | params);
        g_assert_cmphex(qtest_readq(s, lb + 0xc030), ==, 0);

        qtest_readq(s, BASE + bank[0] + 0x18);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_readq(s, BASE + 9);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_readl(s, BASE + 8);
        g_assert_cmpuint(get(s, "failure"), ==, 4);

        qtest_readq(s, lb + 0xc038);
        qtest_writeq(s, lb + 0xc010, UINT64_C(0x203));
        qtest_writeq(s, lb + 0xc000, params | 1);
        g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==, 0);
        qtest_readq(s, lb + 0xc038);
        qtest_writeq(s, lb + 0xc030, sentinel);
        qtest_writeq(s, lb + 0xc000, params);
        g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==, valid | params);
        g_assert_cmphex(qtest_readq(s, lb + 0xc030), ==, 0);
        g_assert_cmphex(qtest_readq(s, BASE + 8), ==, 0);
        g_assert_cmphex(qtest_readq(s, BASE + 0x100), ==, scratch);
        g_assert_cmphex(qtest_readq(s, BASE + bank[0]), ==, parameter);
        g_assert_cmphex(qtest_readq(s, BASE), ==, status);
        g_assert_cmphex(qtest_readq(s, BASE + bank[0] + 8), ==, 0);
        qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    }
    qtest_quit(s);
}

static void test_global_port_def_vector(void)
{
    const uint64_t lb = UINT64_C(0x20600000);
    const uint64_t sentinel = UINT64_C(0xdeadbeefdeadbeef);
    const uint64_t valid = UINT64_C(1) << 63;
    const uint64_t params = 0x18 | (UINT64_C(0x5a) << 40);
    QTestState *s = qtest_init(ARGS " -m 256M "
        "-object memory-backend-ram,id=dma,size=2M "
        "-device sgi-bedrock,id=a,revision=2,dma-memory=/objects/dma/dma[0] "
        "-device sgi-net-cable,id=c,end0=a,end1=r,end1-index=1");
    uint64_t status;

    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/a','property':'net-cable',"
        "'value':'/machine/peripheral/c'}}");
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/r','property':'port1-cable',"
        "'value':'/machine/peripheral/c'}}");
    qtest_sysbus_mmio_map(s, "/machine/peripheral/a", 0, 0x20000000);
    status = qtest_readq(s, BASE);
    g_assert_true(status & (UINT64_C(1) << 37));

    for (unsigned phase = 0; phase < 2; phase++) {
        qtest_writeq(s, BASE + 0x100, 0x12345678);
        qtest_writeq(s, BASE + bank[0], 0x1234567);
        qtest_readq(s, lb + 0xc038);
        qtest_writeq(s, lb + 0xc030, sentinel);
        qtest_writeq(s, lb + 0xc008, 0);
        qtest_writeq(s, lb + 0xc010, UINT64_MAX);
        qtest_writeq(s, lb + 0xc000, params);
        g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==, valid | params);
        g_assert_cmphex(qtest_readq(s, lb + 0xc030), ==, 0);

        qtest_readq(s, lb + 0xc038);
        qtest_writeq(s, lb + 0xc010, UINT64_C(0x203));
        qtest_writeq(s, lb + 0xc000, params | 1);
        g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==, 0);
        qtest_readq(s, lb + 0xc038);
        qtest_writeq(s, lb + 0xc030, sentinel);
        qtest_writeq(s, lb + 0xc000, params);
        g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==, valid | params);
        g_assert_cmphex(qtest_readq(s, lb + 0xc030), ==, 0);

        qtest_readq(s, BASE + bank[0] + 0x18);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_readq(s, BASE + 0x19);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_readl(s, BASE + 0x18);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, 0);
        g_assert_cmphex(qtest_readq(s, BASE + 8), ==, 0);
        g_assert_cmphex(qtest_readq(s, BASE + 0x100), ==, 0x12345678);
        g_assert_cmphex(qtest_readq(s, BASE + bank[0]), ==, 0x1234567);
        g_assert_cmphex(qtest_readq(s, BASE), ==, status);
        qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    }
    qtest_quit(s);
}

static void test_global_diag_parms_vector(void)
{
    const uint64_t lb = UINT64_C(0x20600000);
    const uint64_t sentinel = UINT64_C(0xdeadbeefdeadbeef);
    const uint64_t valid = UINT64_C(1) << 63;
    const uint64_t params = 0x30 | (UINT64_C(0x5a) << 40);
    QTestState *s = qtest_init(ARGS " -m 256M "
        "-object memory-backend-ram,id=dma,size=2M "
        "-device sgi-bedrock,id=a,revision=2,dma-memory=/objects/dma/dma[0] "
        "-device sgi-net-cable,id=c,end0=a,end1=r,end1-index=1");
    uint64_t status;

    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/a','property':'net-cable',"
        "'value':'/machine/peripheral/c'}}");
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/r','property':'port1-cable',"
        "'value':'/machine/peripheral/c'}}");
    qtest_sysbus_mmio_map(s, "/machine/peripheral/a", 0, 0x20000000);
    status = qtest_readq(s, BASE);
    g_assert_true(status & (UINT64_C(1) << 37));

    for (unsigned phase = 0; phase < 2; phase++) {
        qtest_writeq(s, BASE + 0x100, 0x12345678);
        qtest_writeq(s, BASE + bank[0], 0x1234567);
        qtest_readq(s, lb + 0xc038);
        qtest_writeq(s, lb + 0xc030, sentinel);
        qtest_writeq(s, lb + 0xc008, 0);
        qtest_writeq(s, lb + 0xc010, UINT64_MAX);
        qtest_writeq(s, lb + 0xc000, params);
        g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==, valid | params);
        g_assert_cmphex(qtest_readq(s, lb + 0xc030), ==, 0);

        for (unsigned value = 0; value < 2; value++) {
            qtest_readq(s, lb + 0xc038);
            qtest_writeq(s, lb + 0xc010, value);
            qtest_writeq(s, lb + 0xc000, params | 1);
            g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==, 0);
            qtest_readq(s, lb + 0xc038);
            qtest_writeq(s, lb + 0xc030, sentinel);
            qtest_writeq(s, lb + 0xc000, params);
            g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==, valid | params);
            g_assert_cmphex(qtest_readq(s, lb + 0xc030), ==, 0);
        }
        for (unsigned offset = 0x38; offset <= 0x48; offset += 8) {
            expect_ordinary_vector_refusal(s, offset, false);
            expect_ordinary_vector_refusal(s, offset, true);
        }
        for (unsigned port = 0; port < G_N_ELEMENTS(bank); port++) {
            for (unsigned offset = 0x30; offset <= 0x38; offset += 8) {
                expect_ordinary_vector_refusal(s, bank[port] + offset,
                                               false);
                expect_ordinary_vector_refusal(s, bank[port] + offset,
                                               true);
            }
        }

        qtest_readq(s, lb + 0xc038);
        qtest_writeq(s, lb + 0xc030, sentinel);
        qtest_writeq(s, lb + 0xc000,
                     (bank[0] + 0x30) | (UINT64_C(0x5a) << 40));
        g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==, 0);
        g_assert_cmphex(qtest_readq(s, lb + 0xc030), ==, sentinel);
        qtest_readq(s, BASE + 0x31);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_readl(s, BASE + 0x30);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        g_assert_cmphex(qtest_readq(s, BASE + 0x30), ==, 0);
        g_assert_cmphex(qtest_readq(s, BASE + 0x100), ==, 0x12345678);
        g_assert_cmphex(qtest_readq(s, BASE + bank[0]), ==, 0x1234567);
        g_assert_cmphex(qtest_readq(s, BASE), ==, status);
        g_assert_cmphex(qtest_readq(s, BASE + bank[0] + 8), ==, 0);
        qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    }
    qtest_quit(s);
}

static void test_inactive_barrier_storage(void)
{
    QTestState *s = qtest_init(ARGS);
    const uint64_t parameter_mask = (UINT64_C(1) << 30) - 1;

    for (unsigned p = 0; p < 8; p++) {
        uint64_t count = BASE + 0x8000 + 8 * (p + 1);

        g_assert_cmphex(qtest_readq(s, BASE + bank[p]), ==, 0);
        g_assert_cmpuint(get(s, "result"), ==, 2);
        g_assert_cmphex(qtest_readq(s, count), ==, 0);
        g_assert_cmpuint(get(s, "result"), ==, 2);
        qtest_writeq(s, BASE + bank[p], UINT64_MAX);
        g_assert_cmphex(qtest_readq(s, BASE + bank[p]), ==, parameter_mask);
        qtest_writeq(s, BASE + bank[p], 0x1234000 + p);
        qtest_writeq(s, count, p + 1);
    }
    for (unsigned p = 0; p < 8; p++) {
        uint64_t count = BASE + 0x8000 + 8 * (p + 1);

        g_assert_cmphex(qtest_readq(s, BASE + bank[p]), ==, 0x1234000 + p);
        g_assert_cmphex(qtest_readq(s, count), ==, p + 1);
        qtest_writeq(s, count, 0);
        g_assert_cmphex(qtest_readq(s, count), ==, 0);
        qtest_writeq(s, count, 63);
        g_assert_cmphex(qtest_readq(s, count), ==, 63);
        for (unsigned bit = 26; bit < 64; bit++) {
            qtest_writeq(s, count, (UINT64_C(1) << bit) | 7);
            g_assert_cmpuint(get(s, "failure"), ==, 5);
            g_assert_cmphex(qtest_readq(s, count), ==, 63);
        }
        qtest_writeq(s, count, (UINT64_MAX & ~((UINT64_C(1) << 26) - 1)) | 9);
        g_assert_cmpuint(get(s, "failure"), ==, 5);
        g_assert_cmphex(qtest_readq(s, count), ==, 63);
        qtest_readq(s, count + 1);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_readl(s, count);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_writeq(s, count + 1, 0);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_writel(s, count, 0);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        g_assert_cmphex(qtest_readq(s, count), ==, 63);
        qtest_readq(s, BASE + bank[p] + 1);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_readl(s, BASE + bank[p]);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_writeq(s, BASE + bank[p] + 1, 0);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_writel(s, BASE + bank[p], 0);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        g_assert_cmphex(qtest_readq(s, BASE + bank[p]), ==, 0x1234000 + p);
    }
    for (unsigned context = 32; context <= 33; context++) {
        qtest_readq(s, BASE + 0x8000 + 8 * context);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_writeq(s, BASE + 0x8000 + 8 * context, 1);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
    }
    qtest_quit(s);
}

static void test_inactive_barrier_fields(void)
{
    QTestState *s = qtest_init(ARGS);
    const unsigned contexts[] = { 0, 8, 31 };
    const uint64_t mask = (UINT64_C(1) << 26) - 1;

    for (unsigned context = 0; context < 32; context++) {
        uint64_t address = BASE + 0x8000 + 8 * context;

        g_assert_cmphex(qtest_readq(s, address), ==, 0);
        g_assert_cmpuint(get(s, "result"), ==, 2);
        qtest_writeq(s, address, mask - context);
        g_assert_cmpuint(get(s, "result"), ==, 2);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(contexts); i++) {
        unsigned context = contexts[i];
        uint64_t address = BASE + 0x8000 + 8 * context;

        for (unsigned bit = 0; bit < 26; bit++) {
            qtest_writeq(s, address, UINT64_C(1) << bit);
            g_assert_cmpuint(get(s, "result"), ==, 2);
            g_assert_cmphex(qtest_readq(s, address), ==, UINT64_C(1) << bit);
        }
        qtest_writeq(s, address, mask - context);
        for (unsigned bit = 26; bit < 64; bit++) {
            qtest_writeq(s, address, (UINT64_C(1) << bit) | 7);
            g_assert_cmpuint(get(s, "result"), ==, 1);
            g_assert_cmpuint(get(s, "failure"), ==, 5);
            g_assert_cmphex(qtest_readq(s, address), ==, mask - context);
        }
        qtest_readq(s, address + 1);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_readl(s, address);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_readw(s, address);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_readb(s, address);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_writeq(s, address + 1, 0);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_writel(s, address, 0);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_writew(s, address, 0);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
        qtest_writeb(s, address, 0);
        g_assert_cmpuint(get(s, "failure"), ==, 4);
    }
    for (unsigned context = 0; context < 32; context++) {
        uint64_t address = BASE + 0x8000 + 8 * context;

        g_assert_cmphex(qtest_readq(s, address), ==, mask - context);
        qtest_writeq(s, address, 0);
        g_assert_cmphex(qtest_readq(s, address), ==, 0);
    }
    qtest_quit(s);
}

static void test_inactive_barrier_no_effects(void)
{
    QTestState *s = qtest_init(ARGS " -device sgi-sn1-router,id=peer "
        "-device sgi-net-cable,id=c,end0=r,end0-index=8,"
        "end1=peer,end1-index=7");
    uint64_t status;

    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/r','property':'port8-cable',"
        "'value':'/machine/peripheral/c'}}");
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/peer','property':'port7-cable',"
        "'value':'/machine/peripheral/c'}}");
    status = qtest_readq(s, BASE);
    g_assert_true(status & (UINT64_C(1) << 51));
    for (unsigned i = 0; i < 5; i++) {
        qtest_writeq(s, BASE + 0x100 + 8 * i, i < 3 ? 0x1200 + i : 1);
    }
    set(s, "vector", 8);
    qtest_writeq(s, BASE + 0x100, 0x5678);
    set(s, "vector", 0);
    qtest_writeq(s, BASE + bank[3] + 0x200 + 5 * 8, 2);
    set(s, "destination", 5);
    set(s, "turn", 3);
    for (unsigned bit = 28; bit <= 29; bit++) {
        for (unsigned p = 0; p < 8; p++) {
            uint64_t count = BASE + 0x8000 + 8 * (p + 1);

            qtest_writeq(s, count, p + 1);
            qtest_writeq(s, BASE + bank[p], UINT64_C(1) << bit);
            g_assert_cmphex(qtest_readq(s, BASE + bank[p]), ==,
                            UINT64_C(1) << bit);
            g_assert_cmphex(qtest_readq(s, BASE), ==, status);
            g_assert_cmphex(qtest_readq(s, BASE + bank[p] + 8), ==, 0);
            g_assert_cmphex(qtest_readq(s, count), ==, p + 1);
            g_assert_cmphex(get(s, "route"), ==, 4 | (2 << 8));
            for (unsigned i = 0; i < 5; i++) {
                g_assert_cmphex(qtest_readq(s, BASE + 0x100 + 8 * i), ==,
                                i < 3 ? 0x1200 + i : 1);
            }
            qtest_writeq(s, count, (UINT64_C(1) << 26) | 17);
            g_assert_cmpuint(get(s, "failure"), ==, 5);
            g_assert_cmphex(qtest_readq(s, count), ==, p + 1);
            set(s, "vector", 8);
            g_assert_cmphex(qtest_readq(s, BASE + 0x100), ==, 0x5678);
            g_assert_cmpuint(get(s, "result"), ==, 2);
            g_assert_cmphex(get(s, "reply"), ==, 1);
            set(s, "vector", 0);
        }
    }
    for (unsigned context = 0; context < 32; context++) {
        uint64_t address = BASE + 0x8000 + 8 * context;
        uint64_t value = ((UINT64_C(1) << 26) - 1) - context;

        for (unsigned attempt = 0; attempt < 4; attempt++) {
            uint64_t data = value;

            if (attempt) {
                data |= UINT64_C(1) << (attempt == 3 ? 63 : 25 + attempt);
            }
            qtest_writeq(s, address, data);
            g_assert_cmpuint(get(s, "result"), ==, attempt ? 1 : 2);
            g_assert_cmpuint(get(s, "failure"), ==, attempt ? 5 : 0);
            g_assert_cmphex(qtest_readq(s, address), ==, value);
            g_assert_cmphex(qtest_readq(s, BASE), ==, status);
            g_assert_cmphex(get(s, "route"), ==, 4 | (2 << 8));
            for (unsigned p = 0; p < 8; p++) {
                g_assert_cmphex(qtest_readq(s, BASE + bank[p] + 8), ==, 0);
                g_assert_cmphex(qtest_readq(s, BASE + bank[p]), ==,
                                UINT64_C(1) << 29);
            }
            for (unsigned i = 0; i < 5; i++) {
                g_assert_cmphex(qtest_readq(s, BASE + 0x100 + 8 * i), ==,
                                i < 3 ? 0x1200 + i : 1);
            }
            set(s, "vector", 8);
            g_assert_cmphex(qtest_readq(s, BASE + 0x100), ==, 0x5678);
            g_assert_cmpuint(get(s, "result"), ==, 2);
            g_assert_cmphex(get(s, "reply"), ==, 1);
            set(s, "vector", 0);
        }
    }
    qtest_quit(s);
}

static void test_position(void)
{
    QTestState *s = qtest_init(ARGS);
    const uint64_t inputs = UINT64_C(3) << 20;
    const uint64_t values[] = {
        0, UINT64_MAX & ~inputs, UINT64_C(1) << 22,
        UINT64_C(1) << 23, UINT64_C(0x123456789abcdef) & ~inputs,
    };

    g_assert_cmphex(qtest_readq(s, BASE + 0x50), ==, inputs);
    for (unsigned i = 0; i < G_N_ELEMENTS(values); i++) {
        qtest_writeq(s, BASE + 0x50, values[i]);
        g_assert_cmphex(qtest_readq(s, BASE + 0x50), ==, values[i] | inputs);
    }
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(s, BASE + 0x50), ==, inputs);
    qtest_writeq(s, BASE + 0x50, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x50), ==, inputs);
    qtest_quit(s);
}

static void test_routing(void)
{
    QTestState *s = qtest_init(ARGS);

    qtest_writeq(s, BASE + bank[0] + 0x200 + 5 * 8,
                 UINT64_C(0xa5c3f00d17654327));
    qtest_writeq(s, BASE + bank[3] + 0x200 + 5 * 8,
                 UINT64_C(0x5a3c0ff0fedcba9a));
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
    g_assert_cmphex(get(s, "route"), ==, 1 | (7 << 8));
    qtest_writeq(s, BASE + 0x10, (1 << 8) | (1 << 12));
    g_assert_cmphex(get(s, "route"), ==, 1 | (7 << 8));
    qtest_writeq(s, BASE + 0x10, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, UINT64_MAX);
    g_assert_cmphex(get(s, "route"), ==, 1 | (7 << 8));
    qtest_writeq(s, BASE + 0x10, UINT64_C(0xa5c3000000001100));
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==,
                    UINT64_C(0xa5c3000000001100));
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
    for (unsigned p = 0; p < 8; p++) {
        qtest_writeq(s, BASE + bank[p], UINT64_MAX);
    }
    for (unsigned context = 0; context < 32; context++) {
        qtest_writeq(s, BASE + 0x8000 + 8 * context,
                     ((UINT64_C(1) << 26) - 1) - context);
    }
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(s, BASE + 0x100), ==, 0);
    g_assert_cmphex(qtest_readq(s, BASE + bank[7] + 0x200), ==, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x518), ==, 0);
    g_assert_cmphex((qtest_readq(s, BASE) >> 28) & 15, ==, 1);
    for (unsigned p = 0; p < 8; p++) {
        g_assert_cmphex(qtest_readq(s, BASE + bank[p]), ==, 0);
    }
    for (unsigned context = 0; context < 32; context++) {
        g_assert_cmphex(qtest_readq(s, BASE + 0x8000 + 8 * context), ==, 0);
    }
    qtest_quit(s);
}

static void test_route_table_words(void)
{
    QTestState *s = qtest_init(ARGS);
    const uint64_t first = UINT64_C(0xa5c3f00d17654321);
    const uint64_t second = UINT64_C(0x5a3c0ff0fedcba98);
    const uint64_t global1 = BASE + bank[0] + 0x100 + 3 * 8;
    const uint64_t local1 = BASE + bank[0] + 0x200 + 5 * 8;
    const uint64_t global8 = BASE + bank[7] + 0x100 + 15 * 8;
    const uint64_t local8 = BASE + bank[7] + 0x200 + 127 * 8;

    qtest_writeq(s, global1, first);
    qtest_writeq(s, local1, second);
    qtest_writeq(s, global8, second);
    qtest_writeq(s, local8, first);
    g_assert_cmphex(qtest_readq(s, global1), ==, first);
    g_assert_cmphex(qtest_readq(s, local1), ==, second);
    g_assert_cmphex(qtest_readq(s, global8), ==, second);
    g_assert_cmphex(qtest_readq(s, local8), ==, first);
    g_assert_cmphex(qtest_readq(s, global1 + 8), ==, 0);
    g_assert_cmphex(qtest_readq(s, local1 + 8), ==, 0);
    /* The test adapter's refusal says nothing about a vector error code. */
    qtest_writeq(s, BASE + bank[0] + 0x180, UINT64_MAX);
    g_assert_cmpuint(get(s, "result"), ==, 1);
    g_assert_cmphex(qtest_readq(s, global1), ==, first);
    qtest_writeq(s, BASE + bank[7] + 0x600, UINT64_MAX);
    g_assert_cmpuint(get(s, "result"), ==, 1);
    g_assert_cmphex(qtest_readq(s, local8), ==, first);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(s, global1), ==, 0);
    g_assert_cmphex(qtest_readq(s, local1), ==, 0);
    g_assert_cmphex(qtest_readq(s, global8), ==, 0);
    g_assert_cmphex(qtest_readq(s, local8), ==, 0);
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
    qtest_writeq(from, BASE + bank[0] + 0x100 + 3 * 8,
                 UINT64_C(0xa5c3f00d17654321));
    qtest_writeq(from, BASE + bank[7] + 0x200 + 127 * 8,
                 UINT64_C(0x5a3c0ff0fedcba98));
    for (unsigned p = 0; p < 8; p++) {
        qtest_writeq(from, BASE + bank[p] + 0x200, p);
        qtest_writeq(from, BASE + bank[p] + 0x100, 7 - p);
        qtest_writeq(from, BASE + bank[p], 0x31234000 + p);
    }
    for (unsigned context = 0; context < 32; context++) {
        qtest_writeq(from, BASE + 0x8000 + 8 * context,
                     ((UINT64_C(1) << 26) - 1) - context);
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
    g_assert_cmphex(qtest_readq(to, BASE + bank[0] + 0x100 + 3 * 8), ==,
                    UINT64_C(0xa5c3f00d17654321));
    g_assert_cmphex(qtest_readq(to, BASE + bank[7] + 0x200 + 127 * 8), ==,
                    UINT64_C(0x5a3c0ff0fedcba98));
    for (unsigned p = 0; p < 8; p++) {
        g_assert_cmphex(qtest_readq(to, BASE + bank[p] + 0x200), ==, p);
        g_assert_cmphex(qtest_readq(to, BASE + bank[p] + 0x100), ==, 7 - p);
        g_assert_cmphex(qtest_readq(to, BASE + bank[p]), ==, 0x31234000 + p);
    }
    for (unsigned context = 0; context < 32; context++) {
        g_assert_cmphex(qtest_readq(to, BASE + 0x8000 + 8 * context), ==,
                        ((UINT64_C(1) << 26) - 1) - context);
    }
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    rmdir(dir);
}

/* Change only the serialized profile, leaving the machine layout identical. */
static void change_stream_router_profile(const char *path, uint8_t profile)
{
    static const char router_name[] = "sgi-sn1-router";
    g_autoptr(GError) error = NULL;
    GMappedFile *mapped = g_mapped_file_new(path, false, &error);
    const char *stream;
    gsize size;
    unsigned matches = 0;
    int fd;

    g_assert_no_error(error);
    g_assert_nonnull(mapped);
    stream = g_mapped_file_get_contents(mapped);
    size = g_mapped_file_get_length(mapped);
    fd = open(path, O_WRONLY);
    g_assert_cmpint(fd, >=, 0);
    for (gsize i = 0; i + 14 < size; i++) {
        uint8_t name_len;
        gsize payload;

        if ((uint8_t)stream[i] != 4) {
            continue;
        }
        name_len = (uint8_t)stream[i + 5];
        payload = i + 6 + name_len + 8;
        if (name_len < sizeof(router_name) - 1 || payload + 2 > size) {
            continue;
        }
        if (memcmp(stream + i + 6 + name_len - sizeof(router_name) + 1,
                   router_name, sizeof(router_name) - 1) ||
            ldl_be_p(stream + payload - 4) != 4 ||
            (uint8_t)stream[payload] != 1) {
            continue;
        }
        g_assert_cmpuint((uint8_t)stream[payload + 1], ==, !profile);
        g_assert_cmpint(pwrite(fd, &profile, 1, payload + 1), ==, 1);
        matches++;
    }
    g_assert_cmpuint(matches, ==, 1);
    g_assert_cmpint(close(fd), ==, 0);
    g_mapped_file_unref(mapped);
}

static void test_migration_profile_mismatch(const void *data)
{
    bool synthetic = GPOINTER_TO_INT(data);
    g_autofree char *dir = g_dir_make_tmp("sn1-router-profile-XXXXXX", NULL);
    g_autofree char *stream = g_build_filename(dir, "stream", NULL);
    g_autofree char *save_uri = g_strdup_printf("exec:cat > %s", stream);
    g_autofree char *load_uri = g_strdup_printf("exec:cat %s", stream);
    g_autofree char *prom = g_build_filename(dir, "prom", NULL);
    g_autofree char *image = g_build_filename(dir, "storage", NULL);
    g_autofree char *dest_image = g_build_filename(dir, "dest-storage", NULL);
    g_autofree char *args = NULL;
    QTestState *from, *to;
    QDict *status;
    int fd;

    if (synthetic) {
        fd = open(prom, O_CREAT | O_EXCL | O_RDWR, 0600);
        g_assert_cmpint(fd, >=, 0);
        g_assert_cmpint(ftruncate(fd, 1476264), ==, 0);
        g_assert_cmpint(close(fd), ==, 0);
        fd = open(image, O_CREAT | O_EXCL | O_RDWR, 0600);
        g_assert_cmpint(fd, >=, 0);
        g_assert_cmpint(ftruncate(fd, 16 * MiB), ==, 0);
        g_assert_cmpint(close(fd), ==, 0);
        fd = open(dest_image, O_CREAT | O_EXCL | O_RDWR, 0600);
        g_assert_cmpint(fd, >=, 0);
        g_assert_cmpint(ftruncate(fd, 16 * MiB), ==, 0);
        g_assert_cmpint(close(fd), ==, 0);
        args = g_strdup_printf(
            "-M origin300,topology=origin300-numalink-pci-expansion-experiment,"
            "expansion-storage.0=expansion-enclosure=cfg "
            "-cpu R14000 -smp 4 -m 1G -bios %s "
            "-drive if=none,id=cfg,file=%s,format=raw "
            "-device sgi-sn1-router-test,id=test,"
            "router=/machine/module3/router", prom, image);
    } else {
        args = g_strdup(ARGS);
    }
    from = qtest_init(args);
    qtest_writeq(from, BASE + 0x100, 0x12345678);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", save_uri);
    sgi_wait_migration(from, "completed");
    qtest_quit(from);
    change_stream_router_profile(stream, !synthetic);

    if (synthetic) {
        g_free(args);
        args = g_strdup_printf(
            "-M origin300,topology=origin300-numalink-pci-expansion-experiment,"
            "expansion-storage.0=expansion-enclosure=cfg "
            "-cpu R14000 -smp 4 -m 1G -bios %s "
            "-drive if=none,id=cfg,file=%s,format=raw "
            "-device sgi-sn1-router-test,id=test,"
            "router=/machine/module3/router -incoming defer", prom,
            dest_image);
    } else {
        char *incoming = g_strconcat(args, " -incoming defer", NULL);

        g_free(args);
        args = incoming;
    }
    to = qtest_init(args);
    qtest_writeq(to, BASE + 0x100, 0xabcdef);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s,"
        "'exit-on-error':false}}", load_uri);
    sgi_wait_migration(to, "failed");
    status = qtest_qmp_assert_success_ref(to,
        "{'execute':'query-migrate'}");
    g_assert_nonnull(strstr(qdict_get_str(status, "error-desc"),
                            "device 'sgi-sn1-router'"));
    g_assert_nonnull(strstr(qdict_get_str(status, "error-desc"),
                            synthetic ? "1 != 0" : "0 != 1"));
    qobject_unref(status);
    g_assert_cmphex(qtest_readq(to, BASE + 0x100), ==, 0xabcdef);
    qtest_quit(to);
    g_unlink(stream);
    if (synthetic) {
        g_unlink(prom);
        g_unlink(image);
        g_unlink(dest_image);
    }
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_revision_range(void)
{
    if (!g_test_subprocess()) {
        QTestState *s = qtest_init(ARGS);

        g_assert_cmphex((qtest_readq(s, BASE) >> 28) & 0xff, ==, 1);
        qtest_quit(s);
        g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*supports only inferred chip revision one*");
        return;
    }
    qtest_quit(qtest_init("-M none -cpu R10000 "
                          "-device sgi-sn1-router,chip-revision=255"));
}

static void test_revision_overflow(void)
{
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*uint8_t*");
        return;
    }
    qtest_quit(qtest_init("-M none -cpu R10000 "
                          "-device sgi-sn1-router,chip-revision=256"));
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

static void test_uart_adjacent_registers(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 "
        "-device sgi-l1,id=l1,brick-type=router "
        "-device sgi-l1-uart,id=uart,controller=l1 "
        "-device sgi-sn1-router,id=r,l1-uart=uart "
        "-device sgi-sn1-router-test,id=test,router=r");

    qtest_writeq(s, BASE + 0x258, 0x80);
    qtest_writeq(s, BASE + 0x240, 0x12ab);
    qtest_writeq(s, BASE + 0x248, 0x34cd);
    g_assert_cmphex(qtest_readq(s, BASE + 0x240), ==, 0xab);
    g_assert_cmphex(qtest_readq(s, BASE + 0x248), ==, 0xcd);
    g_assert_cmphex(qtest_readq(s, BASE + 0x258), ==, 0x80);

    qtest_writeq(s, BASE + 0x240, 0x56ef);
    g_assert_cmphex(qtest_readq(s, BASE + 0x240), ==, 0xef);
    g_assert_cmphex(qtest_readq(s, BASE + 0x248), ==, 0xcd);
    qtest_writeq(s, BASE + 0x248, 0x789a);
    g_assert_cmphex(qtest_readq(s, BASE + 0x240), ==, 0xef);
    g_assert_cmphex(qtest_readq(s, BASE + 0x248), ==, 0x9a);
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/sn1-router/registers", test_registers);
    qtest_add_func("/sgi/sn1-router/experimental-status-write",
                   test_experimental_status_write);
    qtest_add_func("/sgi/sn1-router/experimental-local-reset",
                   test_experimental_local_reset);
    qtest_add_func("/sgi/sn1-router/error-clear", test_error_clear);
    qtest_add_func("/sgi/sn1-router/error-clear-vector",
                   test_error_clear_vector);
    qtest_add_func("/sgi/sn1-router/port-reset-vector",
                   test_port_reset_vector);
    qtest_add_func("/sgi/sn1-router/experimental-zero-reset-restore",
                   test_experimental_zero_reset_restore);
    qtest_add_func("/sgi/sn1-router/experimental-port-errors",
                   test_experimental_port_errors);
    qtest_add_func("/sgi/sn1-router/global-port-def-vector",
                   test_global_port_def_vector);
    qtest_add_func("/sgi/sn1-router/global-diag-parms-vector",
                   test_global_diag_parms_vector);
    qtest_add_func("/sgi/sn1-router/inactive-barrier-storage",
                   test_inactive_barrier_storage);
    qtest_add_func("/sgi/sn1-router/inactive-barrier-fields",
                   test_inactive_barrier_fields);
    qtest_add_func("/sgi/sn1-router/inactive-barrier-no-effects",
                   test_inactive_barrier_no_effects);
    qtest_add_func("/sgi/sn1-router/position", test_position);
    qtest_add_func("/sgi/sn1-router/routing", test_routing);
    qtest_add_func("/sgi/sn1-router/vector-links", test_vector_links);
    qtest_add_func("/sgi/sn1-router/silent-peer", test_silent_peer);
    qtest_add_func("/sgi/sn1-router/refusals", test_refusals);
    qtest_add_func("/sgi/sn1-router/reset", test_reset);
    qtest_add_func("/sgi/sn1-router/route-table-words",
                   test_route_table_words);
    qtest_add_func("/sgi/sn1-router/migration", test_migration);
    qtest_add_data_func("/sgi/sn1-router/migration-profile/ordinary-destination",
                        GINT_TO_POINTER(false), test_migration_profile_mismatch);
    qtest_add_data_func("/sgi/sn1-router/migration-profile/synthetic-destination",
                        GINT_TO_POINTER(true), test_migration_profile_mismatch);
    qtest_add_func("/sgi/sn1-router/revision-range", test_revision_range);
    qtest_add_func("/sgi/sn1-router/revision-overflow",
                   test_revision_overflow);
    qtest_add_func("/sgi/sn1-router/uart", test_uart);
    qtest_add_func("/sgi/sn1-router/uart-adjacent-registers",
                   test_uart_adjacent_registers);
    return g_test_run();
}
