/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define CONTROL 0x40000000000ULL
#define HUB 0x20001000000ULL
#define MEMORY 0x800000ULL
#define LIMIT (1ULL << 40)

static QTestState *start_extra(const char *extra)
{
    return qtest_initf("-M origin2000 "
                      "-device sgi-hub-packet-test,node=/machine/node0 "
                      "-device sgi-hub-packet-test,node=/machine/node1,"
                      "addr=0x40000001000 %s", extra);
}

static QTestState *start(void)
{
    return start_extra("");
}

static uint64_t hub_read(QTestState *q, unsigned node, uint64_t offset)
{
    uint8_t bytes[8];
    g_assert_true(qtest_memread_cpu(q, 2 * node, HUB + offset, bytes, 8));
    return ldq_be_p(bytes);
}

static void hub_write(QTestState *q, unsigned node, uint64_t offset,
                      uint64_t value)
{
    uint8_t bytes[8];
    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(q, 2 * node, HUB + offset, bytes, 8));
}

static uint64_t packet_from(QTestState *q, unsigned source, uint64_t addr,
                            unsigned size, bool write, uint64_t data)
{
    uint64_t control = CONTROL + source * 0x1000;
    qtest_writeq(q, control, addr);
    qtest_writeq(q, control + 8, data);
    qtest_writeq(q, control + 16, size);
    qtest_writeq(q, control + 24, write);
    return qtest_readq(q, control + 32);
}

static uint64_t packet(QTestState *q, uint64_t addr, unsigned size,
                       bool write, uint64_t data)
{
    return packet_from(q, 0, addr, size, write, data);
}

static void test_physical_identity(void)
{
    QTestState *q = start();
    uint64_t probe = (1ULL << 47) | 0x01600000;
    uint64_t status[2];
    unsigned i, bit, size;

    /* Equal node identities must not collapse two physical receivers. */
    hub_write(q, 0, 0x600000, 0);
    hub_write(q, 1, 0x600000, 1 << 18);
    status[0] = hub_read(q, 0, 0x600000);
    status[1] = hub_read(q, 1, 0x600000);
    g_assert_cmphex(status[0], !=, status[1]);
    for (i = 0; i < 2; i++) {
        g_assert_cmphex(packet_from(q, i, probe, 8, false, 0), ==, 0);
        g_assert_cmphex(qtest_readq(q, CONTROL + i * 0x1000 + 8), ==,
                        status[i]);
    }
    hub_write(q, 1, 0x600000, 0x2300);
    g_assert_cmphex(packet_from(q, 1, probe, 8, false, 0), ==, 0);
    g_assert_cmphex(qtest_readq(q, CONTROL + 0x1008), ==,
                    hub_read(q, 1, 0x600000));

    for (bit = 0; bit < 47; bit++) {
        g_assert_cmphex(packet(q, probe ^ (1ULL << bit), 8, false, 0), !=, 0);
    }
    for (size = 1; size < 8; size *= 2) {
        g_assert_cmphex(packet(q, probe, size, false, 0), !=, 0);
    }
    g_assert_cmphex(packet(q, probe, 8, true, 0x7700), !=, 0);
    g_assert_cmphex(hub_read(q, 0, 0x600000), ==, status[0]);
    g_assert_cmphex(packet(q, 0x01600000, 8, true, 0xaabbccdd), ==, 0);
    g_assert_cmphex(packet(q, 0x01600000, 8, false, 0), ==, 0);
    g_assert_cmphex(qtest_readq(q, CONTROL + 8), ==, 0xaabbccdd);
    g_assert_cmphex(packet(q, 4, 4, true, 0x12345678), ==, 0);
    g_assert_cmphex(packet(q, 4, 4, false, 0), ==, 0);
    g_assert_cmphex(qtest_readq(q, CONTROL + 8), ==, 0x12345678);
    qtest_quit(q);
}

static void test_memory(void)
{
    QTestState *q = start();
    uint8_t bytes[8];
    unsigned size;
    for (size = 1; size <= 8; size *= 2) {
        uint64_t value = 0x123456789abcdef0ULL >> (64 - 8 * size);
        g_assert_cmphex(packet(q, MEMORY, size, true, value), ==, 0);
        g_assert_cmphex(packet(q, MEMORY, size, false, 0), ==, 0);
        g_assert_cmphex(qtest_readq(q, CONTROL + 8), ==, value);
    }
    g_assert_true(qtest_memread_cpu(q, 0, MEMORY, bytes, 8));
    g_assert_cmphex(ldq_be_p(bytes), ==, 0x123456789abcdef0ULL);
    qtest_quit(q);
}

static void test_irq(void)
{
    QTestState *q = start();
    uint64_t addr = (1ULL << 47) | 0x01800090;
    g_assert_cmphex(packet(q, addr, 8, true, 0x140), ==, 0);
    g_assert_cmphex(hub_read(q, 0, 0xa0), ==, 1);
    g_assert_cmphex(hub_read(q, 1, 0xa0), ==, 0);
    g_assert_cmphex(packet(q, addr, 8, true, 0x40), ==, 0);
    g_assert_cmphex(hub_read(q, 0, 0xa0), ==, 0);
    hub_write(q, 0, 0x600000, 0x1700);
    g_assert_cmphex(packet(q, addr | (0x17ULL << 38), 8, true, 0x10c), ==, 0);
    g_assert_cmphex(hub_read(q, 0, 0x98), ==, 1 << 12);
    g_assert_cmphex(packet(q, addr, 8, true, 0x10d), !=, 0);
    qtest_quit(q);
}

static void test_rejections(void)
{
    QTestState *q = start();
    uint64_t irq = (1ULL << 47) | 0x01800090;
    const uint64_t addresses[] = {
        LIMIT, LIMIT - 4, 0x20001800090ULL, irq | (1ULL << 46),
        irq + 8, UINT64_MAX - 3,
    };
    unsigned i, size;
    for (i = 0; i < G_N_ELEMENTS(addresses); i++) {
        g_assert_cmphex(packet(q, addresses[i], 8, true, 0x10c), !=, 0);
        g_assert_cmphex(packet(q, addresses[i], 8, false, 0), !=, 0);
    }
    for (size = 1; size < 8; size *= 2) {
        g_assert_cmphex(packet(q, irq, size, true, 0x10c), !=, 0);
    }
    g_assert_cmphex(packet(q, irq, 8, false, 0), !=, 0);
    g_assert_cmphex(packet(q, 0x4000000, 8, true, 0), !=, 0);
    g_assert_cmphex(packet(q, 0x4000000, 8, false, 0), !=, 0);
    g_assert_cmphex(hub_read(q, 0, 0x98), ==, 0);
    qtest_quit(q);
}

static void test_access_units(void)
{
    g_autofree char *directory =
        g_dir_make_tmp("hub-packet-trace-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(directory, "accesses.txt", NULL);
    g_autofree char *options =
        g_strdup_printf("-trace enable=memory_region_ops_write,file=%s", path);
    g_autofree char *trace = NULL;
    const char *match;
    const char *crossing = "addr 0xfffffffffc value 0x1234 size 8 "
                           "name 'sn0.hub.packet'";
    QTestState *q = start_extra(options);

    qtest_writeq(q, CONTROL + 40, 0);
    g_assert_cmphex(packet(q, LIMIT - 4, 8, true, 0x1234), !=, 0);
    g_assert_cmpuint(qtest_readq(q, CONTROL + 40), ==, 0);
    /* A DMA buffer is decomposed: its first admitted access is forwarded. */
    g_assert_cmphex(packet(q, LIMIT - 8, 16, true, 0x1234), !=, 0);
    g_assert_cmpuint(qtest_readq(q, CONTROL + 40), ==, 1);
    qtest_writeq(q, CONTROL + 40, 0);
    g_assert_cmphex(packet(q, MEMORY + 4, 8, true, 0x12345678), ==, 0);
    g_assert_cmpuint(qtest_readq(q, CONTROL + 40), ==, 1);
    g_assert_cmphex(qtest_readq(q, CONTROL + 48), ==, 0x11234);
    g_assert_cmphex(packet(q, MEMORY + 4, 8, false, 0), ==, 0);
    g_assert_cmphex(qtest_readq(q, CONTROL + 8), ==, 0x12345678);
    qtest_quit(q);
    /* Observe the guard itself, not only the absence of target writes. */
    g_assert_true(g_file_get_contents(path, &trace, NULL, NULL));
    match = strstr(trace, crossing);
    g_assert_nonnull(match);
    g_assert_null(strstr(match + strlen(crossing), crossing));
    unlink(path);
    rmdir(directory);
}

static void router_write(QTestState *q, uint64_t route, uint64_t reg,
                          uint64_t value)
{
    hub_read(q, 0, 0x600380);
    hub_write(q, 0, 0x600210, value);
    hub_write(q, 0, 0x600208, route);
    hub_write(q, 0, 0x600200, reg | 1);
    g_assert_cmphex(hub_read(q, 0, 0x600300) & (1ULL << 63), !=, 0);
}

static void test_claims(void)
{
    QTestState *q = start();
    uint64_t irq = (1ULL << 47) | 0x01800090;
    const unsigned nasids[] = { 0x11, 0x12, 0x21, 0x22 };
    uint64_t remote = (0x21ULL << 32) | MEMORY;
    unsigned i;
    /* Duplicate reset NASIDs resolve relative to the physical ingress Hub. */
    g_assert_cmphex(packet_from(q, 1, irq, 8, true, 0x10e), ==, 0);
    g_assert_cmphex(hub_read(q, 1, 0x98), ==, 1 << 14);
    g_assert_cmphex(hub_read(q, 0, 0x98), ==, 0);
    for (i = 0; i < 4; i++) {
        hub_write(q, i, 0x600000, (1 << 17) | (nasids[i] << 8));
    }
    g_assert_cmphex(packet(q, remote, 8, true, 0x12345678), !=, 0);
    /* Program the same source-Hub / Router route used by the core qtest. */
    router_write(q, 0, 0x10, (1 << 11) | (1 << 6));
    router_write(q, 6, 0x10, (1 << 11) | (2 << 6));
    router_write(q, 0, 0x70010, 5ULL << 20);
    router_write(q, 6, 0x70108, 0xffffff);
    hub_write(q, 0, 0x638108, 5);
    hub_write(q, 0, 0x638010, 1);
    g_assert_cmphex(packet(q, remote, 8, true, 0x12345678), ==, 0);
    g_assert_cmphex(packet(q, remote, 8, false, 0), ==, 0);
    g_assert_cmphex(qtest_readq(q, CONTROL + 8), ==, 0x12345678);
    g_assert_cmphex(packet(q, irq | (0x21ULL << 38), 8, true, 0x147), ==, 0);
    g_assert_cmphex(hub_read(q, 2, 0xa0), ==, 1 << 7);
    hub_write(q, 0, 0x638010, 7);
    g_assert_cmphex(packet(q, remote, 8, false, 0), !=, 0);
    g_assert_cmphex(packet(q, irq | (0x21ULL << 38), 8, true, 0x47), !=, 0);
    g_assert_cmphex(hub_read(q, 2, 0xa0), ==, 1 << 7);
    hub_write(q, 0, 0x638010, 1);
    g_assert_cmphex(packet(q, irq | (0x21ULL << 38), 8, true, 0x47), ==, 0);
    g_assert_cmphex(hub_read(q, 2, 0xa0), ==, 0);
    qtest_quit(q);
}

static void test_reset(void)
{
    QTestState *q = start();
    uint64_t irq = (1ULL << 47) | 0x01800090;
    unsigned node, reg;
    for (node = 0; node < 4; node++) {
        hub_write(q, node, 0x90, 0x10c);
        hub_write(q, node, 0x90, 0x147);
        for (reg = 0xa8; reg <= 0xc0; reg += 8) {
            hub_write(q, node, reg, UINT64_MAX);
        }
    }
    g_assert_cmphex(packet(q, irq, 8, true, 0x10d), ==, 0);
    qtest_system_reset(q);
    qtest_clock_step(q, 1000000000);
    for (node = 0; node < 4; node++) {
        for (reg = 0x98; reg <= 0xc0; reg += 8) {
            g_assert_cmphex(hub_read(q, node, reg), ==, 0);
        }
    }
    g_assert_cmphex(packet(q, irq, 8, true, 0x10d), ==, 0);
    g_assert_cmphex(hub_read(q, 0, 0x98), ==, 1 << 13);
    g_assert_cmphex(packet(q, irq, 8, true, 0x0d), ==, 0);
    g_assert_cmphex(hub_read(q, 0, 0x98), ==, 0);
    qtest_quit(q);
}

static void test_migration(void)
{
    g_autofree char *directory = g_dir_make_tmp("hub-packet-migrate-XXXXXX",
                                               NULL);
    g_autofree char *socket =
        g_build_filename(directory, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = start();
    QTestState *to = start_extra("-incoming defer");
    uint64_t irq = (1ULL << 47) | (0x17ULL << 38) | 0x01800090;
    uint64_t memory = (0x17ULL << 32) | MEMORY;
    int64_t deadline = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;

    hub_write(from, 0, 0x600000, 0x1700);
    g_assert_cmphex(packet(from, irq, 8, true, 0x147), ==, 0);
    g_assert_cmphex(packet(from, memory, 8, true, 0x76543210), ==, 0);
    hub_write(from, 0, 0xb0, 1 << 7);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    for (;;) {
        g_autoptr(QDict) response = qtest_qmp(from,
                                            "{'execute':'query-migrate'}");
        QDict *result = qdict_get_qdict(response, "return");
        const char *status = qdict_get_str(result, "status");
        if (!strcmp(status, "completed")) {
            break;
        }
        g_assert_cmpstr(status, !=, "failed");
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    g_assert_cmphex(hub_read(to, 0, 0xa0), ==, 1 << 7);
    g_assert_cmphex(hub_read(to, 0, 0xb0), ==, 1 << 7);
    g_assert_cmphex(packet(to, memory, 8, false, 0), ==, 0);
    g_assert_cmphex(qtest_readq(to, CONTROL + 8), ==, 0x76543210);
    g_assert_cmphex(packet(to, (1ULL << 47) | 0x01600000, 8, false, 0), ==, 0);
    g_assert_cmphex(qtest_readq(to, CONTROL + 8), ==,
                    hub_read(to, 0, 0x600000));
    hub_write(to, 0, 0x600000, 0x1900);
    g_assert_cmphex(packet(to, (1ULL << 47) | 0x01600000, 8, false, 0), ==, 0);
    g_assert_cmphex(qtest_readq(to, CONTROL + 8), ==,
                    hub_read(to, 0, 0x600000));
    g_assert_cmphex(packet(to, irq, 8, true, 0x47), !=, 0);
    irq = (1ULL << 47) | (0x19ULL << 38) | 0x01800090;
    g_assert_cmphex(packet(to, irq, 8, true, 0x47), ==, 0);
    g_assert_cmphex(hub_read(to, 0, 0xa0), ==, 0);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    rmdir(directory);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn0/hub-packet/physical-identity", test_physical_identity);
    qtest_add_func("/sn0/hub-packet/memory", test_memory);
    qtest_add_func("/sn0/hub-packet/interrupt", test_irq);
    qtest_add_func("/sn0/hub-packet/reject", test_rejections);
    qtest_add_func("/sn0/hub-packet/access-units", test_access_units);
    qtest_add_func("/sn0/hub-packet/claims", test_claims);
    qtest_add_func("/sn0/hub-packet/reset", test_reset);
    qtest_add_func("/sn0/hub-packet/migration", test_migration);
    return g_test_run();
}
