/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The published module wiring and reviewed programmed-route contract. */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define NODE_STRIDE UINT64_C(0x100000000)
#define HUB_BASE UINT64_C(0x20001000000)
#define NI_STATUS 0x600000
#define NI_VECTOR_PARMS 0x600200
#define NI_VECTOR 0x600208
#define NI_VECTOR_DATA 0x600210
#define NI_VECTOR_STATUS 0x600300
#define NI_VECTOR_READ_DATA 0x600310
#define NI_VECTOR_CLEAR 0x600380
#define NI_LOCAL_TABLE 0x638100
#define NI_META_TABLE 0x638000
#define NI_REGION_SIZE (UINT64_C(1) << 17)
#define VECTOR_VALID (UINT64_C(1) << 63)
#define VECTOR_SOURCE_MASK (UINT64_C(0x7ff) << 51)
#define ROUTER_PROTECTION 0x10
#define ROUTER_DIAG_PARMS 0x30
#define ROUTER_SCRATCH0 0x20
#define ROUTER_SCRATCH1 0x28
#define ROUTER_META_TABLE 0x70000
#define ROUTER_LOCAL_TABLE 0x70100
#define MEM_PROBE UINT64_C(0x800000)
/* Architectural Hub-special offset 0x10000000 on its QEMU carrier. */
#define LOCAL_FLASH_VIEW UINT64_C(0x10010000000)
#define RESET_FLASH_VIEW UINT64_C(0x1fc00000)
#define FLASH_MODE_OFFSET 0x64

static const unsigned int module_nasids[] = { 0x11, 0x12, 0x23, 0x24 };

static uint64_t hub_read(QTestState *qts, unsigned int node, uint64_t offset)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, 2 * node, HUB_BASE + offset,
                                    bytes, sizeof(bytes)));
    return ldq_be_p(bytes);
}

static void hub_write(QTestState *qts, unsigned int node, uint64_t offset,
                       uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 2 * node, HUB_BASE + offset,
                                     bytes, sizeof(bytes)));
}

static uint64_t node_memory_address(QTestState *qts, unsigned int node,
                                     uint64_t offset)
{
    uint64_t nasid = (hub_read(qts, node, NI_STATUS) >> 8) & 0xff;

    return nasid * NODE_STRIDE + offset;
}

static void node_write(QTestState *qts, unsigned int node, uint64_t offset,
                        uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 2 * node,
                                     node_memory_address(qts, node, offset),
                                     bytes, sizeof(bytes)));
}

static uint64_t node_read(QTestState *qts, unsigned int node, uint64_t offset)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, 2 * node,
                                    node_memory_address(qts, node, offset),
                                    bytes, sizeof(bytes)));
    return ldq_be_p(bytes);
}

static uint64_t vector_op(QTestState *qts, unsigned int source, uint64_t route,
                           uint64_t address, unsigned int op, uint64_t data)
{
    uint64_t identifiers = ((UINT64_C(0x30) + source + route) << 40) |
                           ((UINT64_C(0x80) + source + op) << 32);
    uint64_t status;

    hub_read(qts, source, NI_VECTOR_CLEAR);
    hub_write(qts, source, NI_VECTOR_DATA, data);
    hub_write(qts, source, NI_VECTOR, route);
    hub_write(qts, source, NI_VECTOR_PARMS,
                 identifiers | address | op);
    status = hub_read(qts, source, NI_VECTOR_STATUS);
    if (status & VECTOR_VALID) {
        g_assert_cmphex(status & (UINT64_C(0x3ffff) << 32), ==, identifiers);
        g_assert_cmphex(status & UINT64_C(0xfffffff8), ==, address & 0xffff8);
    }
    return status;
}

static void vector_write(QTestState *qts, uint64_t route, uint64_t address,
                          uint64_t data)
{
    uint64_t status = vector_op(qts, 0, route, address, 1, data);

    g_assert_cmphex(status & (VECTOR_VALID | 7), ==, VECTOR_VALID | 1);
    g_assert_cmphex(status & VECTOR_SOURCE_MASK, ==, 0);
}

static bool cpu_read(QTestState *qts, unsigned int cpu, uint64_t address,
                      uint64_t *value)
{
    g_autofree char *command = g_strdup_printf("xp/1gx 0x%" PRIx64, address);
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{ 'execute': 'human-monitor-command', 'arguments': {"
        " 'command-line': %s, 'cpu-index': %u } }", command, cpu);
    const char *dump = qdict_get_str(response, "return");
    const char *colon;

    if (strstr(dump, "Cannot access memory")) {
        return false;
    }
    colon = strchr(dump, ':');
    g_assert_nonnull(colon);
    g_assert_cmpint(sscanf(colon + 1, " 0x%" SCNx64, value), ==, 1);
    return true;
}

static QTestState *start_module(void)
{
    QTestState *qts = qtest_init("-M origin2000 -smp 8,sockets=4,cores=2 "
                                 "-m 256M -d unimp,guest_errors");
    const unsigned int numbers[] = { 0x11, 0x12, 0x21, 0x22 };
    unsigned int i;

    for (i = 0; i < G_N_ELEMENTS(numbers); i++) {
        hub_write(qts, i, NI_STATUS,
                     NI_REGION_SIZE | ((uint64_t)numbers[i] << 8));
        g_assert_cmphex((hub_read(qts, i, NI_STATUS) >> 8) & 0xff,
                        ==, numbers[i]);
    }
    return qts;
}

static void test_vectors(void)
{
    QTestState *qts = start_module();
    const uint64_t paths[] = { 0, 6 };
    unsigned int i;

    for (i = 0; i < G_N_ELEMENTS(paths); i++) {
        uint64_t status = vector_op(qts, 0, paths[i], 0, 0, 0);
        uint64_t value = hub_read(qts, 0, NI_VECTOR_READ_DATA);

        g_assert_cmphex(status & (VECTOR_VALID | VECTOR_SOURCE_MASK | 7),
                        ==, VECTOR_VALID);
        g_assert_cmphex(value & 0xff, ==, 0x11);
        g_assert_cmphex((value >> 46) & 7, ==, i ? 6 : 5);
        vector_write(qts, paths[i], ROUTER_SCRATCH0, 0x123400 + i);
        vector_op(qts, 0, paths[i], ROUTER_SCRATCH0, 0, 0);
        g_assert_cmphex(hub_read(qts, 0, NI_VECTOR_READ_DATA),
                        ==, 0x123400 + i);
    }
    for (i = 0; i < G_N_ELEMENTS(paths); i++) {
        uint64_t status = vector_op(qts, 0, paths[i], ROUTER_SCRATCH1, 3,
                                     0xffff);

        g_assert_cmphex(status & (VECTOR_VALID | VECTOR_SOURCE_MASK | 7),
                        ==, VECTOR_VALID | 3);
        g_assert_cmphex(hub_read(qts, 0, NI_VECTOR_READ_DATA), ==, 0);
        vector_op(qts, 0, paths[i], ROUTER_SCRATCH1, 3, 0x1234);
        g_assert_cmphex(hub_read(qts, 0, NI_VECTOR_READ_DATA), ==,
                        0xffff);
        vector_op(qts, 0, paths[i], ROUTER_SCRATCH1, 0, 0);
        g_assert_cmphex(hub_read(qts, 0, NI_VECTOR_READ_DATA), ==,
                        0xffff);
    }
    g_assert_cmphex(vector_op(qts, 0, 0x606, ROUTER_SCRATCH0, 1, 0) &
                    VECTOR_VALID, ==, 0);
    g_assert_cmphex(vector_op(qts, 0, UINT64_C(0x6666666666666666),
                              ROUTER_SCRATCH0, 1, 0) & VECTOR_VALID, ==, 0);
    vector_op(qts, 0, 0, ROUTER_SCRATCH0, 0, 0);
    g_assert_cmphex(hub_read(qts, 0, NI_VECTOR_READ_DATA), ==, 0x123400);
    /* Both direct self-return and the second Router's node endpoint. */
    for (i = 0; i < 2; i++) {
        uint64_t status = vector_op(qts, 0, i ? 0x56 : 5, 0, 0, 0);

        g_assert_cmphex(status & VECTOR_VALID, ==, VECTOR_VALID);
        g_assert_cmphex((status & VECTOR_SOURCE_MASK) >> 51, ==,
                        i ? 0x21 : 0x11);
    }
    /* An unsupported operation must not alter the Router scratch register. */
    g_assert_cmphex(vector_op(qts, 0, 0, ROUTER_SCRATCH0, 2, 0) &
                    VECTOR_VALID, ==, 0);
    vector_op(qts, 0, 0, ROUTER_SCRATCH0, 0, 0);
    g_assert_cmphex(hub_read(qts, 0, NI_VECTOR_READ_DATA), ==,
                    0x123400);
    g_assert_cmphex(vector_op(qts, 0, 1, 0, 0, 0) & VECTOR_VALID, ==, 0);
    qtest_quit(qts);
}

static void program_remote(QTestState *qts)
{
    /* R1 ingress5 exits6: relative1. R2 ingress6 exits5: relative5. */
    vector_write(qts, 0, ROUTER_PROTECTION, (1 << 11) | (1 << 6));
    vector_write(qts, 6, ROUTER_PROTECTION, (1 << 11) | (2 << 6));
    vector_write(qts, 0, ROUTER_META_TABLE + 2 * 8, UINT64_C(5) << 20);
    vector_write(qts, 6, ROUTER_LOCAL_TABLE + 1 * 8, 0xffffff);
    hub_write(qts, 0, NI_LOCAL_TABLE + 1 * 8, 5);
    hub_write(qts, 0, NI_META_TABLE + 2 * 8, 1);
}

static void test_programmed_routes(void)
{
    QTestState *qts = start_module();
    uint64_t value;
    uint64_t target = 0x21 * NODE_STRIDE + MEM_PROBE;

    node_write(qts, 2, MEM_PROBE, UINT64_C(0x123456789abcdef));
    g_assert_false(cpu_read(qts, 0, target, &value));
    program_remote(qts);
    g_assert_true(cpu_read(qts, 0, target, &value));
    g_assert_cmphex(value, ==, UINT64_C(0x123456789abcdef));
    hub_write(qts, 0, NI_META_TABLE + 2 * 8, 5);
    g_assert_false(cpu_read(qts, 0, target, &value));
    hub_write(qts, 0, NI_META_TABLE + 2 * 8, 1);
    vector_write(qts, 0, ROUTER_META_TABLE + 2 * 8, UINT64_C(4) << 20);
    g_assert_false(cpu_read(qts, 0, target, &value));
    vector_write(qts, 0, ROUTER_META_TABLE + 2 * 8, UINT64_C(15) << 20);
    g_assert_false(cpu_read(qts, 0, target, &value));
    program_remote(qts);
    /* Force-local chooses the local table even for a different meta ID. */
    vector_write(qts, 0, ROUTER_LOCAL_TABLE + 1 * 8, UINT64_C(5) << 20);
    vector_write(qts, 0, ROUTER_META_TABLE + 2 * 8, UINT64_C(15) << 20);
    vector_write(qts, 0, ROUTER_PROTECTION, UINT64_C(1) << 12);
    g_assert_true(cpu_read(qts, 0, target, &value));
    program_remote(qts);
    /* A zero turn can return through ingress; a repeated full state fails. */
    vector_write(qts, 0, ROUTER_META_TABLE + 2 * 8, 0);
    vector_write(qts, 6, ROUTER_LOCAL_TABLE + 1 * 8, 0);
    g_assert_false(cpu_read(qts, 0, target, &value));
    program_remote(qts);
    /* Diagnostic disable invalidates both vectors and the memory cache. */
    vector_write(qts, 0, ROUTER_DIAG_PARMS, UINT64_C(1) << 11);
    g_assert_false(cpu_read(qts, 0, target, &value));
    g_assert_cmphex(vector_op(qts, 0, 6, 0, 0, 0) & VECTOR_VALID, ==, 0);
    vector_write(qts, 0, ROUTER_DIAG_PARMS, 0);
    g_assert_true(cpu_read(qts, 0, target, &value));
    g_assert_cmphex(vector_op(qts, 0, 6, 0, 0, 0) & VECTOR_VALID, ==,
                    VECTOR_VALID);
    /* Disabling the far end of the cable has the same availability rule. */
    vector_write(qts, 6, ROUTER_DIAG_PARMS, UINT64_C(1) << 11);
    g_assert_false(cpu_read(qts, 0, target, &value));
    vector_op(qts, 2, 0, ROUTER_DIAG_PARMS, 1, 0);
    g_assert_true(cpu_read(qts, 0, target, &value));
    vector_write(qts, 0, ROUTER_DIAG_PARMS, UINT64_C(1) << 17);
    vector_op(qts, 0, 0, 0, 0, 0);
    g_assert_cmphex(hub_read(qts, 0, NI_VECTOR_READ_DATA) &
                    (UINT64_C(1) << 45), ==, UINT64_C(1) << 45);
    /* Peer width negotiation is outside the supported local-force model. */
    vector_op(qts, 0, 6, 0, 0, 0);
    g_assert_cmphex(hub_read(qts, 0, NI_VECTOR_READ_DATA) &
                    (UINT64_C(1) << 45), ==, 0);
    vector_write(qts, 0, ROUTER_DIAG_PARMS, 0);
    qtest_qmp_assert_success(qts,
        "{ 'execute': 'qom-set', 'arguments': { 'path': '/machine/fabric',"
        " 'property': 'down-links', 'value': 'r1-r2' } }");
    g_assert_false(cpu_read(qts, 0, target, &value));
    qtest_qmp_assert_success(qts,
        "{ 'execute': 'qom-set', 'arguments': { 'path': '/machine/fabric',"
        " 'property': 'down-links', 'value': '' } }");
    g_assert_true(cpu_read(qts, 0, target, &value));
    /* Two remote physical Hubs claiming one NASID must not choose an owner. */
    hub_write(qts, 3, NI_STATUS, NI_REGION_SIZE | (0x21 << 8));
    g_assert_false(cpu_read(qts, 0, target, &value));
    hub_write(qts, 3, NI_STATUS, NI_REGION_SIZE | (0x22 << 8));
    g_assert_true(cpu_read(qts, 0, target, &value));
    qtest_system_reset(qts);
    g_assert_false(cpu_read(qts, 0, target, &value));
    qtest_quit(qts);
}

static void test_bte_and_interrupt(void)
{
    QTestState *qts = start_module();
    const uint64_t source = UINT64_C(0x11) * NODE_STRIDE + MEM_PROBE;
    const uint64_t dest = UINT64_C(0x21) * NODE_STRIDE + MEM_PROBE;
    const uint64_t notify = dest + 128;

    program_remote(qts);
    for (unsigned int offset = 0; offset < 256; offset += 8) {
        if (offset < 128) {
            node_write(qts, 0, MEM_PROBE + offset,
                        UINT64_C(0xabababababababab));
        }
        node_write(qts, 2, MEM_PROBE + offset, UINT64_C(0xcccccccccccccccc));
    }
    hub_write(qts, 0, 0x410008, source);
    hub_write(qts, 0, 0x410010, dest);
    hub_write(qts, 0, 0x410020, notify);
    hub_write(qts, 0, 0x410000, (1 << 20) | 1);
    hub_write(qts, 0, 0x410018, 1 << 4);
    g_assert_cmphex(hub_read(qts, 0, 0x410000), ==, 0);
    for (unsigned int offset = 0; offset < 128; offset += 8) {
        g_assert_cmphex(node_read(qts, 2, MEM_PROBE + offset), ==,
                        UINT64_C(0xabababababababab));
    }
    g_assert_cmphex(node_read(qts, 2, MEM_PROBE + 128), ==, 0);
    hub_write(qts, 0, 0x410028, (20 << 16) | 0x21);
    hub_write(qts, 0, 0x410000, (1 << 20) | 1);
    hub_write(qts, 0, 0x410018, 0);
    g_assert_cmphex(hub_read(qts, 2, 0x98), ==, 1 << 20);
    qtest_quit(qts);
}

static void test_link_without_routes(void)
{
    QTestState *qts = start_module();
    const uint64_t link_up = UINT64_C(1) << 29;

    g_assert_cmphex(hub_read(qts, 0, NI_STATUS) & link_up, ==, link_up);
    /* The other Hub on R1 can disable and restore this Hub's physical port. */
    vector_op(qts, 1, 0, ROUTER_DIAG_PARMS, 1, UINT64_C(1) << 10);
    g_assert_cmphex(hub_read(qts, 0, NI_STATUS) & link_up, ==, 0);
    vector_op(qts, 1, 0, ROUTER_DIAG_PARMS, 1, 0);
    g_assert_cmphex(hub_read(qts, 0, NI_STATUS) & link_up, ==, link_up);
    vector_op(qts, 1, 0, ROUTER_DIAG_PARMS, 1, UINT64_C(1) << 10);
    qtest_system_reset(qts);
    g_assert_cmphex(hub_read(qts, 0, NI_STATUS) & link_up, ==, link_up);
    qtest_quit(qts);
}

static void set_down_links(QTestState *qts, const char *links)
{
    qtest_qmp_assert_success(qts,
        "{ 'execute': 'qom-set', 'arguments': { 'path': '/machine/fabric',"
        " 'property': 'down-links', 'value': %s } }", links);
}

static void assert_routes(QTestState *qts, const char *expected)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{ 'execute': 'qom-get', 'arguments': { 'path': '/machine/fabric',"
        " 'property': 'routes' } }");

    g_assert_cmpstr(qdict_get_str(response, "return"), ==, expected);
}

static void program_module(QTestState *qts)
{
    /* N1/N3 enter port 5, N2/N4 port 4; port 6 joins the Routers. */
    static const uint8_t first_turns[][4] = {
        { 0, 5, 1, 1 },
        { 1, 0, 2, 2 },
        { 1, 1, 0, 5 },
        { 2, 2, 1, 0 },
    };
    static const uint32_t router_entries[][4] = {
        { 0xffffff, 0xffffff, 0x5fffff, 0x4fffff },
        { 0x5fffff, 0x4fffff, 0xffffff, 0xffffff },
    };
    unsigned int from, to;

    for (from = 0; from < G_N_ELEMENTS(module_nasids); from++) {
        hub_write(qts, from, NI_STATUS,
                     NI_REGION_SIZE | ((uint64_t)module_nasids[from] << 8));
    }
    for (from = 0; from < G_N_ELEMENTS(router_entries); from++) {
        uint64_t route = from ? 6 : 0;

        /* Distinct local IDs let each Router choose both far-side Hubs. */
        vector_write(qts, route, ROUTER_PROTECTION, UINT64_C(1) << 12);
        for (to = 0; to < G_N_ELEMENTS(module_nasids); to++) {
            vector_write(qts, route,
                         ROUTER_LOCAL_TABLE + (module_nasids[to] & 15) * 8,
                         router_entries[from][to]);
        }
    }
    for (from = 0; from < G_N_ELEMENTS(module_nasids); from++) {
        for (to = 0; to < G_N_ELEMENTS(module_nasids); to++) {
            bool local = (module_nasids[from] >> 4) ==
                         (module_nasids[to] >> 4);
            uint64_t entry = local ?
                NI_LOCAL_TABLE + (module_nasids[to] & 15) * 8 :
                NI_META_TABLE + (module_nasids[to] >> 4) * 8;

            hub_write(qts, from, entry, first_turns[from][to]);
        }
    }
}

static const char all_module_routes[] =
    "0>1=0x4 0>2=0x56 0>3=0x46 "
    "1>0=0x5 1>2=0x56 1>3=0x46 "
    "2>0=0x56 2>1=0x46 2>3=0x4 "
    "3>0=0x56 3>1=0x46 3>2=0x5";

static const char split_module_routes[] =
    "0>1=0x4 0>2=- 0>3=- "
    "1>0=0x5 1>2=- 1>3=- "
    "2>0=- 2>1=- 2>3=0x4 "
    "3>0=- 3>1=- 3>2=0x5";

static void initialize_module_memory(QTestState *qts)
{
    for (unsigned int to = 0; to < G_N_ELEMENTS(module_nasids); to++) {
        node_write(qts, to, MEM_PROBE, UINT64_C(0x1234567800) + to);
    }
}

static void assert_module_memory(QTestState *qts, bool split)
{
    unsigned int from, to;

    for (from = 0; from < G_N_ELEMENTS(module_nasids); from++) {
        for (to = 0; to < G_N_ELEMENTS(module_nasids); to++) {
            uint64_t value;
            bool reachable = !split || from / 2 == to / 2;
            uint64_t address = module_nasids[to] * NODE_STRIDE + MEM_PROBE;

            g_test_message("memory %u -> %u, split %u", from, to, split);
            g_assert_cmpint(cpu_read(qts, 2 * from, address, &value), ==,
                            reachable);
            if (reachable) {
                g_assert_cmphex(value, ==, UINT64_C(0x1234567800) + to);
            }
        }
    }
}

static void test_all_module_routes(void)
{
    QTestState *qts = start_module();
    unsigned int from, to;

    program_module(qts);
    assert_routes(qts, all_module_routes);
    initialize_module_memory(qts);
    assert_module_memory(qts, false);
    for (from = 0; from < G_N_ELEMENTS(module_nasids); from++) {
        for (to = 0; to < G_N_ELEMENTS(module_nasids); to++) {
            uint64_t remote = HUB_BASE + module_nasids[to] * NODE_STRIDE;
            uint8_t bytes[8];

            if (from == to) {
                continue;
            }
            stq_be_p(bytes, UINT64_C(0xcafe0000) + from * 16 + to);
            g_assert_true(qtest_memwrite_cpu(qts, 2 * from, remote + 0x600100,
                                            bytes, sizeof(bytes)));
            g_assert_cmphex(hub_read(qts, to, 0x600100), ==,
                            UINT64_C(0xcafe0000) + from * 16 + to);
            stq_be_p(bytes, 0x100 | 20);
            g_assert_true(qtest_memwrite_cpu(qts, 2 * from, remote + 0x90,
                                            bytes, sizeof(bytes)));
            for (unsigned int node = 0; node < 4; node++) {
                g_assert_cmphex(hub_read(qts, node, 0x98), ==,
                                node == to ? 1 << 20 : 0);
            }
            hub_write(qts, to, 0x90, 20);
        }
    }
    qtest_quit(qts);
}

static void test_module_link_isolation(void)
{
    QTestState *qts = start_module();

    program_module(qts);
    initialize_module_memory(qts);
    assert_module_memory(qts, false);
    set_down_links(qts, "r1-r2");
    assert_routes(qts, split_module_routes);
    assert_module_memory(qts, true);
    for (unsigned int node = 0; node < 4; node++) {
        g_assert_cmphex(vector_op(qts, node, 6, 0, 0, 0) & VECTOR_VALID,
                        ==, 0);
        g_assert_cmphex(vector_op(qts, node, 0, 0, 0, 0) & VECTOR_VALID,
                        ==, VECTOR_VALID);
    }
    set_down_links(qts, "");
    assert_routes(qts, all_module_routes);
    assert_module_memory(qts, false);
    qtest_quit(qts);
}

static void migrate_module(QTestState *from, QTestState *to)
{
    g_autofree char *directory = g_dir_make_tmp("origin2000-migrate-XXXXXX",
                                                NULL);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    int64_t deadline = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;

    g_assert_nonnull(directory);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
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
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_migration(void)
{
    QTestState *from = start_module();
    QTestState *to = qtest_init("-M origin2000 -incoming defer");
    const uint64_t target = UINT64_C(0x21) * NODE_STRIDE + MEM_PROBE;
    const uint64_t marker = UINT64_C(0x123456789abcdef);
    uint64_t value;

    program_remote(from);
    node_write(from, 2, MEM_PROBE, marker);
    g_assert_true(cpu_read(from, 0, target, &value));
    migrate_module(from, to);
    g_assert_true(cpu_read(to, 0, target, &value));
    g_assert_cmphex(value, ==, marker);
    vector_write(to, 0, ROUTER_META_TABLE + 2 * 8, UINT64_C(15) << 20);
    g_assert_false(cpu_read(to, 0, target, &value));
    program_remote(to);
    g_assert_true(cpu_read(to, 0, target, &value));
    qtest_quit(from);
    qtest_quit(to);
}

static void test_isolated_migration(void)
{
    QTestState *from = start_module();
    QTestState *to = qtest_init("-M origin2000 -incoming defer");
    const uint64_t marker = UINT64_C(0x123456789abcdef);
    g_autoptr(QDict) response = NULL;
    uint64_t status;

    program_module(from);
    initialize_module_memory(from);
    assert_routes(from, all_module_routes);
    assert_module_memory(from, false);
    set_down_links(from, "r1-r2");
    assert_routes(from, split_module_routes);
    assert_module_memory(from, true);
    vector_write(from, 0, ROUTER_SCRATCH0, marker);
    status = vector_op(from, 0, 0, ROUTER_SCRATCH0, 0, 0);
    g_assert_cmphex(status & (VECTOR_VALID | 7), ==, VECTOR_VALID);
    g_assert_cmphex(hub_read(from, 0, NI_VECTOR_READ_DATA), ==, marker);

    migrate_module(from, to);
    g_assert_cmphex(hub_read(to, 0, NI_VECTOR_STATUS), ==, status);
    g_assert_cmphex(hub_read(to, 0, NI_VECTOR_READ_DATA), ==, marker);
    response = qtest_qmp(to,
        "{ 'execute': 'qom-get', 'arguments': { 'path': '/machine/fabric',"
        " 'property': 'down-links' } }");
    g_assert_cmpstr(qdict_get_str(response, "return"), ==, "r1-r2");
    assert_routes(to, split_module_routes);
    assert_module_memory(to, true);
    set_down_links(to, "");
    assert_routes(to, all_module_routes);
    assert_module_memory(to, false);
    vector_op(to, 0, 0, ROUTER_SCRATCH0, 0, 0);
    g_assert_cmphex(hub_read(to, 0, NI_VECTOR_READ_DATA), ==, marker);
    qtest_quit(from);
    qtest_quit(to);
}

static void test_oversize_firmware(void)
{
    g_autofree char *path = NULL;
    g_autofree char *error_output = NULL;
    int fd = g_file_open_tmp("sn0-origin2000-rom-XXXXXX", &path, NULL);
    int status;
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000", "-display", "none",
        "-S", "-nodefaults", "-bios", NULL, NULL,
    };

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 1024 * 1024 + 1), ==, 0);
    close(fd);
    args[8] = path;
    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &status, NULL));
    unlink(path);
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), !=, 0);
    g_test_message("firmware rejection: %s", error_output);
    g_assert_nonnull(strstr(error_output, "exceeds flash capacity"));
}

static void assert_launch_rejected(const char *const args[],
                                   const char *message)
{
    const char commands[] = "{\"execute\":\"qmp_capabilities\"}\n"
                            "{\"execute\":\"quit\"}\n";
    char error_output[1024] = { 0 };
    struct sigaction ignore = { .sa_handler = SIG_IGN }, previous;
    GPid pid;
    int input_fd, error_fd, status;
    ssize_t length;

    g_assert_true(g_spawn_async_with_pipes(NULL, (char **)args, NULL,
                    G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL,
                    NULL, NULL, &pid, &input_fd, NULL, &error_fd, NULL));
    /* Quit an incorrectly accepted launch instead of leaving it running. */
    sigemptyset(&ignore.sa_mask);
    g_assert_cmpint(sigaction(SIGPIPE, &ignore, &previous), ==, 0);
    length = qemu_write_full(input_fd, commands, sizeof(commands) - 1);
    g_assert_true(length == sizeof(commands) - 1 ||
                  (length == -1 && errno == EPIPE));
    g_assert_cmpint(sigaction(SIGPIPE, &previous, NULL), ==, 0);
    close(input_fd);
    g_assert_cmpint(waitpid(pid, &status, 0), ==, pid);
    length = read(error_fd, error_output, sizeof(error_output) - 1);
    g_assert_cmpint(length, >=, 0);
    close(error_fd);
    g_spawn_close_pid(pid);
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), !=, 0);
    g_assert_nonnull(strstr(error_output, message));
}

static void test_missing_firmware(void)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000", "-display", "none",
        "-S", "-nodefaults", "-qmp", "stdio", NULL,
    };

    /* A normal launch must fail; qtest deliberately permits empty flash. */
    assert_launch_rejected(args, "requires supplied firmware");
}

static void test_flash_fixture_isolation(void)
{
    const struct {
        const char *machine;
        const char *accel;
        unsigned int node;
        const char *error;
    } cases[] = {
        { "none", "tcg", 0, "requires the qtest accelerator" },
        { "none", "qtest", 0, "requires Origin 2000" },
        { "origin2000", "qtest", 4, "needs a populated physical node" },
    };

    for (unsigned int i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree char *device = g_strdup_printf(
            "sn0-origin2000-flash-test,node=%u,word=0", cases[i].node);
        const char *args[] = {
            qtest_qemu_binary(NULL), "-M", cases[i].machine,
            "-display", "none", "-S", "-nodefaults", "-qmp", "stdio",
            "-accel", cases[i].accel, "-device", device, NULL,
        };

        assert_launch_rejected(args, cases[i].error);
    }
}

static void test_local_flash_words(gconstpointer data)
{
    QTestState *qts;
    g_autoptr(GString) args = g_string_new(NULL);
    const uint32_t words[] = { 0x13579bdf, 0x2468ace0, 0x5aa53cc3, 0xc33ca55a };
    const uint64_t views[] = { LOCAL_FLASH_VIEW, RESET_FLASH_VIEW };

    g_string_printf(args, "-M %s -smp 8,sockets=4,cores=2 -m 256M "
                    "-d unimp,guest_errors", (const char *)data);
    /* Mapping fixtures only: these words do not define CPU reset modes. */
    for (unsigned int node = 0; node < G_N_ELEMENTS(words); node++) {
        g_string_append_printf(args, " -device sn0-origin2000-flash-test,"
                               "node=%u,word=%u", node, words[node]);
    }
    qts = qtest_init(args->str);
    for (unsigned int reset = 0; reset < 2; reset++) {
        if (reset) {
            qtest_system_reset(qts);
        }
        for (unsigned int cpu = 0; cpu < 2 * G_N_ELEMENTS(words); cpu++) {
            for (unsigned int view = 0; view < G_N_ELEMENTS(views); view++) {
                uint8_t bytes[4];
                uint32_t actual;

                g_test_message("cpu %u, node %u, view %u, reset %u",
                               cpu, cpu / 2, view, reset);
                g_assert_true(qtest_memread_cpu(qts, cpu,
                                               views[view] + FLASH_MODE_OFFSET,
                                               bytes, sizeof(bytes)));
                actual = ldl_be_p(bytes);
                g_assert_cmphex(actual, ==, words[cpu / 2]);
            }
        }
    }
    qtest_quit(qts);
}

static void assert_cpu_inventory(QTestState *qts)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute': 'query-cpus-fast'}");
    QList *cpus = qdict_get_qlist(response, "return");
    const QListEntry *entry;
    unsigned int seen = 0;

    g_assert_cmpuint(qlist_size(cpus), ==, 8);
    QLIST_FOREACH_ENTRY(cpus, entry) {
        QDict *cpu = qobject_to(QDict, qlist_entry_obj(entry));
        QDict *props = qdict_get_qdict(cpu, "props");
        unsigned int index = qdict_get_int(cpu, "cpu-index");
        g_autofree char *path = g_strdup_printf("/machine/node%u/cpu%u",
                                                index / 2, index % 2);

        g_assert_cmpuint(index, <, 8);
        g_assert_false(seen & (1U << index));
        seen |= 1U << index;
        g_assert_cmpint(qdict_get_int(props, "node-id"), ==, index / 2);
        g_assert_cmpint(qdict_get_int(props, "socket-id"), ==, index / 2);
        g_assert_cmpint(qdict_get_int(props, "core-id"), ==, index % 2);
        g_assert_cmpstr(qdict_get_str(cpu, "qom-path"), ==, path);
        if (!(index % 2)) {
            g_autofree char *hub = g_strdup_printf("/machine/node%u/hub",
                                                   index / 2);
            g_autoptr(QDict) identity = qtest_qmp(qts,
                "{'execute': 'qom-get', 'arguments': {'path': %s,"
                "'property': 'onewire-serial'}}", hub);

            g_autoptr(QDict) base = qtest_qmp(qts,
                "{'execute': 'qom-get', 'arguments': {'path': %s,"
                "'property': 'cpu-base'}}", hub);

            g_assert_cmpint(qdict_get_int(base, "return"), ==, index);
            g_assert_cmphex(qdict_get_int(identity, "return"), ==,
                            UINT64_C(0x100000000001) + index / 2);
        }
    }
    g_assert_cmphex(seen, ==, 0xff);
    for (unsigned int router = 0; router < 2; router++) {
        g_autofree char *path = g_strdup_printf("/machine/router%u", router);
        g_autoptr(QDict) identity = qtest_qmp(qts,
            "{'execute': 'qom-get', 'arguments': {'path': %s,"
            "'property': 'nic-serial'}}", path);

        g_assert_cmphex(qdict_get_int(identity, "return"), ==,
                        UINT64_C(0x100000000005) + router);
    }
}

static void test_reordered_model(void)
{
    QTestState *qts = qtest_init("-M origin2000-model-test");

    assert_cpu_inventory(qts);
    program_module(qts);
    assert_routes(qts, all_module_routes);
    initialize_module_memory(qts);
    assert_module_memory(qts, false);
    set_down_links(qts, "r1-r2");
    assert_routes(qts, split_module_routes);
    assert_module_memory(qts, true);
    set_down_links(qts, "");
    assert_routes(qts, all_module_routes);
    qtest_quit(qts);
}

static void test_invalid_model(gconstpointer data)
{
    const char *fault = data;
    g_autofree char *machine = g_strdup_printf(
        "origin2000-model-test,fault=%s", fault);
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", machine,
        "-display", "none", "-S", "-nodefaults", "-qmp", "stdio",
        "-accel", "qtest", NULL,
    };

    assert_launch_rejected(args, "Origin 2000");
}

static void assert_cpu_interrupts(QTestState *qts, unsigned int target,
                                   unsigned int slice, uint32_t expected)
{
    for (unsigned int cpu = 0; cpu < 8; cpu++) {
        g_autoptr(QDict) response = qtest_qmp(qts,
            "{'execute': 'human-monitor-command', 'arguments': {"
            "'command-line': 'info registers', 'cpu-index': %u}}", cpu);
        const char *field = strstr(qdict_get_str(response, "return"), "Cause");
        uint32_t cause;

        g_assert_nonnull(field);
        g_assert_cmpint(sscanf(field, "Cause %" SCNx32, &cause), ==, 1);
        g_assert_cmphex(cause & 0xc00, ==,
                        cpu == target * 2 + slice ? expected : 0);
    }
}

static void test_remote_cpu_interrupts(void)
{
    QTestState *qts = start_module();
    const uint64_t masks[] = { 0xa8, 0xb8 };
    uint8_t bytes[8];

    program_module(qts);
    for (unsigned int target = 0; target < 4; target++) {
        unsigned int source = (target + 2) % 4;
        uint64_t remote = HUB_BASE + module_nasids[target] * NODE_STRIDE;

        for (unsigned int slice = 0; slice < 2; slice++) {
            stq_be_p(bytes, 0x100 | 20);
            g_assert_true(qtest_memwrite_cpu(qts, source * 2, remote + 0x90,
                                            bytes, sizeof(bytes)));
            assert_cpu_interrupts(qts, target, slice, 0);
            hub_write(qts, target, masks[slice], 1 << 20);
            assert_cpu_interrupts(qts, target, slice, 0x400);
            hub_write(qts, target, masks[slice], 0);
            assert_cpu_interrupts(qts, target, slice, 0);
            hub_write(qts, target, masks[slice], 1 << 20);
            stq_be_p(bytes, 20);
            g_assert_true(qtest_memwrite_cpu(qts, source * 2, remote + 0x90,
                                            bytes, sizeof(bytes)));
            assert_cpu_interrupts(qts, target, slice, 0);
            hub_write(qts, target, masks[slice], 0);
        }
    }
    qtest_quit(qts);
}

static void test_mutated_reset(void)
{
    QTestState *qts = start_module();
    uint64_t value;

    program_module(qts);
    vector_write(qts, 0, ROUTER_PROTECTION, 0x1000);
    vector_write(qts, 6, ROUTER_PROTECTION, 0x1000);
    vector_write(qts, 0, 0x10018, 0);
    vector_write(qts, 6, 0x60018, 1);
    vector_write(qts, 0, ROUTER_DIAG_PARMS, UINT64_C(1) << 17);
    set_down_links(qts, "r1-r2");
    qtest_system_reset(qts);
    assert_cpu_inventory(qts);
    for (unsigned int node = 0; node < 4; node++) {
        g_assert_cmphex((hub_read(qts, node, NI_STATUS) >> 8) & 0xff, ==, 0);
        g_assert_cmphex(vector_op(qts, node, 0, ROUTER_PROTECTION, 0, 0) &
                        VECTOR_VALID, ==, VECTOR_VALID);
        g_assert_cmphex(hub_read(qts, node, NI_VECTOR_READ_DATA), ==, 0x3f);
        for (unsigned int port = 1; port <= 6; port++) {
            vector_op(qts, node, 0, (port << 16) + 0x18, 0, 0);
            g_assert_cmphex(hub_read(qts, node, NI_VECTOR_READ_DATA), ==, 0x3f);
        }
        vector_op(qts, node, 0, ROUTER_DIAG_PARMS, 0, 0);
        g_assert_cmphex(hub_read(qts, node, NI_VECTOR_READ_DATA), ==, 0);
        g_assert_cmphex(vector_op(qts, node, 6, 0, 0, 0) & VECTOR_VALID, ==, 0);
    }
    /* A simulated disconnected cable stays disconnected across device reset. */
    set_down_links(qts, "");
    for (unsigned int node = 0; node < 4; node++) {
        hub_write(qts, node, NI_STATUS,
                     NI_REGION_SIZE | ((uint64_t)module_nasids[node] << 8));
    }
    g_assert_false(cpu_read(qts, 0,
        module_nasids[1] * NODE_STRIDE + MEM_PROBE, &value));
    /* First-turn programming alone cannot resurrect Router table validity. */
    hub_write(qts, 0, NI_LOCAL_TABLE + 2 * 8, 5);
    vector_write(qts, 0, ROUTER_PROTECTION, 0x1000);
    g_assert_false(cpu_read(qts, 0,
        module_nasids[1] * NODE_STRIDE + MEM_PROBE, &value));
    program_module(qts);
    assert_routes(qts, all_module_routes);
    qtest_quit(qts);
}

static void test_distinct_table_migration(void)
{
    QTestState *from = start_module();
    QTestState *to = qtest_init("-M origin2000 -incoming defer");
    const uint64_t local = UINT64_C(0x12) * NODE_STRIDE + MEM_PROBE;
    const uint64_t meta = UINT64_C(0x31) * NODE_STRIDE + MEM_PROBE;
    const uint64_t unwritten = UINT64_C(0x32) * NODE_STRIDE + MEM_PROBE;
    const uint64_t marker = UINT64_C(0xfeedcafe12345678);
    uint64_t value, status;

    hub_write(from, 2, NI_STATUS, NI_REGION_SIZE | (0x31 << 8));
    hub_write(from, 3, NI_STATUS, NI_REGION_SIZE | (0x32 << 8));
    vector_write(from, 0, ROUTER_PROTECTION, (1 << 11) | (1 << 6));
    vector_write(from, 6, ROUTER_PROTECTION, (1 << 11) | (3 << 6));
    vector_write(from, 0, ROUTER_LOCAL_TABLE + 2 * 8, 0xffffff);
    vector_write(from, 0, ROUTER_META_TABLE + 3 * 8, 0x5fffff);
    vector_write(from, 6, ROUTER_LOCAL_TABLE + 1 * 8, 0xffffff);
    hub_write(from, 0, NI_LOCAL_TABLE + 2 * 8, 5);
    hub_write(from, 0, NI_META_TABLE + 3 * 8, 1);
    node_write(from, 1, MEM_PROBE, marker + 1);
    node_write(from, 2, MEM_PROBE, marker + 2);
    node_write(from, 3, MEM_PROBE, marker + 3);
    g_assert_true(cpu_read(from, 0, local, &value));
    g_assert_cmphex(value, ==, marker + 1);
    g_assert_true(cpu_read(from, 0, meta, &value));
    g_assert_cmphex(value, ==, marker + 2);
    vector_write(from, 0, 0x10018, 0x15);
    vector_write(from, 6, 0x60018, 0x2a);
    set_down_links(from, "r1-node2");
    g_assert_false(cpu_read(from, 0, local, &value));
    g_assert_true(cpu_read(from, 0, meta, &value));
    vector_write(from, 0, ROUTER_SCRATCH0, marker);
    status = vector_op(from, 0, 0, ROUTER_SCRATCH0, 0, 0);
    migrate_module(from, to);
    g_assert_cmphex(hub_read(to, 0, NI_VECTOR_STATUS), ==, status);
    g_assert_cmphex(hub_read(to, 0, NI_VECTOR_READ_DATA), ==, marker);
    g_assert_false(cpu_read(to, 0, local, &value));
    g_assert_true(cpu_read(to, 0, meta, &value));
    g_assert_cmphex(value, ==, marker + 2);
    vector_op(to, 0, 0, 0x10018, 0, 0);
    g_assert_cmphex(hub_read(to, 0, NI_VECTOR_READ_DATA), ==, 0x15);
    vector_op(to, 0, 6, 0x60018, 0, 0);
    g_assert_cmphex(hub_read(to, 0, NI_VECTOR_READ_DATA), ==, 0x2a);
    set_down_links(to, "");
    g_assert_true(cpu_read(to, 0, local, &value));
    g_assert_cmphex(value, ==, marker + 1);
    /* R2's local[2] is unwritten although its zero word would terminate. */
    vector_write(to, 0, ROUTER_META_TABLE + 3 * 8, 0x4fffff);
    g_assert_false(cpu_read(to, 0, unwritten, &value));
    vector_write(to, 6, ROUTER_LOCAL_TABLE + 2 * 8, 0);
    g_assert_true(cpu_read(to, 0, unwritten, &value));
    g_assert_cmphex(value, ==, marker + 3);
    /* A local validity bit must not be mistaken for a meta validity bit. */
    vector_write(to, 0, ROUTER_PROTECTION, (1 << 11) | (3 << 6));
    vector_write(to, 0, ROUTER_LOCAL_TABLE + 2 * 8, 0x4fffff);
    g_assert_false(cpu_read(to, 0, local, &value));
    vector_write(to, 0, ROUTER_META_TABLE + 1 * 8, 0xffffff);
    g_assert_true(cpu_read(to, 0, local, &value));
    qtest_quit(from);
    qtest_quit(to);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (qtest_has_machine("origin2000-model-test")) {
        static const char *faults[] = {
            "composition", "hardware", "routers", "router-module",
            "router-slot", "router-identity", "links", "link-count",
            "link-endpoint", "link-port", "link-kind", "link-duplicate",
            "link-name", "cpu-range", "cpu-duplicate", "cpu-count",
            "memory-total", "memory-geometry", "memory-bank", "address-map",
            "xtalk-reference",
        };

        qtest_add_func("sn0/origin2000/reordered-model", test_reordered_model);
        for (unsigned int i = 0; i < G_N_ELEMENTS(faults); i++) {
            g_autofree char *name = g_strdup_printf(
                "sn0/origin2000/reject-model/%s", faults[i]);

            qtest_add_data_func(name, faults[i], test_invalid_model);
        }
    }
    qtest_add_func("sn0/origin2000/remote-cpu-interrupts",
                   test_remote_cpu_interrupts);
    qtest_add_func("sn0/origin2000/mutated-reset", test_mutated_reset);
    qtest_add_func("sn0/origin2000/distinct-table-migration",
                   test_distinct_table_migration);
    if (qtest_has_device("sn0-origin2000-flash-test")) {
        qtest_add_data_func("sn0/origin2000/local-flash-words", "origin2000",
                            test_local_flash_words);
        qtest_add_data_func("sn0/origin2000/reordered-flash-words",
                            "origin2000-model-test", test_local_flash_words);
        qtest_add_func("sn0/origin2000/flash-fixture-isolation",
                       test_flash_fixture_isolation);
    }
    qtest_add_func("sn0/origin2000/vectors", test_vectors);
    qtest_add_func("sn0/origin2000/programmed-routes", test_programmed_routes);
    qtest_add_func("sn0/origin2000/bte-interrupt", test_bte_and_interrupt);
    qtest_add_func("sn0/origin2000/oversize-firmware", test_oversize_firmware);
    qtest_add_func("sn0/origin2000/missing-firmware", test_missing_firmware);
    qtest_add_func("sn0/origin2000/migration", test_migration);
    qtest_add_func("sn0/origin2000/link-without-routes",
                    test_link_without_routes);
    qtest_add_func("sn0/origin2000/all-module-routes", test_all_module_routes);
    qtest_add_func("sn0/origin2000/module-link-isolation",
                    test_module_link_isolation);
    qtest_add_func("sn0/origin2000/isolated-migration",
                    test_isolated_migration);
    return g_test_run();
}
