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
    qtest_writeq(s, lb + 0xc008, 0);
    qtest_writeq(s, lb + 0xc010, 0);
    qtest_writeq(s, lb + 0xc000, params | write);
    g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==,
                    (UINT64_C(1) << 63) | params | 4);
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
    experimental_writeq(s, cpu, lb + 0xc008, route);
    experimental_writeq(s, cpu, lb + 0xc010, data);
    experimental_writeq(s, cpu, lb + 0xc000, params | write);
    response = experimental_readq(s, cpu, lb + 0xc020);
    g_assert_cmphex(response, ==, (UINT64_C(1) << 63) | params |
                    (accepted ? write : 4));
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
    route_before = get(s, "route");
    g_assert_cmphex(route_before, ==, 4 | (2 << 8));
    l1_size = experimental_router_l1_reply(s, l1_before);
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
        experimental_status_vector(s, 0, 0, true, values[i], true);
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
            g_assert_cmpuint(get(s, "result"), ==, 2);
            g_assert_cmpuint(get(s, "failure"), ==, 0);
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
    g_assert_cmphex(experimental_status_vector(s, 0x18, 0, false, 0, true),
                    ==, 0);
    experimental_status_vector(s, 0, 0, true, UINT64_MAX, true);
    status = experimental_status_vector(s, 0, 0, false, 0, true);
    assert_experimental_status(status, 1, 0);
    assert_experimental_status(
        experimental_status_vector_from(s, 2, 0, 0, false, 0, true), 6, 0);
    assert_experimental_status(
        experimental_status_vector_from(s, 2, 0x600000, 0, false, 0, true),
        6, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x100), ==, 0);
    experimental_status_vector(s, 0x18, 0, true, 0xabcd, true);
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
    g_assert_cmphex(experimental_status_vector(to, 0x18, 0, false, 0, true),
                    ==, 0xabcd);
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
                               UINT64_C(0x123456789abcdef0), true);
    g_assert_cmphex(experimental_status_vector(to, 0, 0, false, 0, true),
                    ==, status);
    experimental_status_vector(to, 0x600000, 0, true, 0, false);
    qtest_quit(to);
    qtest_quit(s);
    g_assert_cmpint(g_unlink(to_image), ==, 0);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
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

        for (unsigned write = 0; write <= 1; write++) {
            qtest_readq(s, lb + 0xc038);
            qtest_writeq(s, lb + 0xc030, sentinel);
            qtest_writeq(s, lb + 0xc008, 0);
            qtest_writeq(s, lb + 0xc010, UINT64_MAX);
            qtest_writeq(s, lb + 0xc000, params | write);
            g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==,
                            valid | params | (write ? 4 : 0));
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
        g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==,
                        valid | params | 4);
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
        g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==,
                        valid | params | 4);
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
            g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==,
                            valid | params | 4);
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
        g_assert_cmphex(qtest_readq(s, lb + 0xc020), ==,
                        valid | bank[0] | 0x30 | (UINT64_C(0x5a) << 40) | 4);
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
    qtest_add_func("/sgi/sn1-router/error-clear", test_error_clear);
    qtest_add_func("/sgi/sn1-router/error-clear-vector",
                   test_error_clear_vector);
    qtest_add_func("/sgi/sn1-router/port-reset-vector",
                   test_port_reset_vector);
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
    qtest_add_func("/sgi/sn1-router/migration", test_migration);
    qtest_add_func("/sgi/sn1-router/uart", test_uart);
    qtest_add_func("/sgi/sn1-router/uart-adjacent-registers",
                   test_uart_adjacent_registers);
    return g_test_run();
}
