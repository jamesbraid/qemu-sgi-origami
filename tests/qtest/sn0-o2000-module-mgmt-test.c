/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Origin 2000 module's shared management complex, reached the way a
 * node's firmware reaches it: through that node's own Hub adapter window.
 * One controller and one NVRAM space sit behind all four windows, and the
 * module declares no reset, NMI or power path, so a destructive command is
 * refused rather than acknowledged.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define HUB_BASE        UINT64_C(0x20001000000)
#define I2C_DATA        0x220000
#define I2C_CTRL        0x220008

#define CTL_PIN         0x80
#define CTL_ESO         0x40
#define CTL_ES1         0x20
#define CTL_STA         0x04
#define CTL_STO         0x02
#define CTL_ACK         0x01

#define CTL_START       (CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK)
#define CTL_RESTART     (CTL_ESO | CTL_STA | CTL_ACK)
#define CTL_RUN         (CTL_PIN | CTL_ESO)
#define CTL_NAK         CTL_ESO
#define CTL_STOP        (CTL_PIN | CTL_ESO | CTL_STO)

#define STS_PIN         0x80
#define STS_BER         0x10
#define STS_LRB         0x08
#define STS_LAB         0x02
#define STS_BB          0x01    /* set while the bus is free */

#define MSC_ADDRESS     0x08
#define NVRAM_FIRST     0x50
#define NVRAM_BYTES     2048
#define ADAPTER_COUNT   4
#define RESP_CAP        96

/* An address nothing on the module answers, used to resync on the way out. */
#define INVALID_ADDRESS 0x7f
#define OWN_ADDRESS(node)   (0x09 + (node))
#define CLOCK_12MHZ_90KHZ   0x1c

/* One 90 kHz byte takes about 100us; poll well past that before failing. */
#define POLL_STEP_NS    2000
#define POLL_STEPS      400
#define SERVICE_PERIOD_NS   1000000
#define GRANT_SETTLE_NS     500000
#define GRANT_BASE          0xe0

#define MACHINE_ARGS "-M origin2000 -smp 8,sockets=4,cores=2 -m 256M"

static uint8_t adapter_read(QTestState *qts, unsigned node, uint64_t offset)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, 2 * node, HUB_BASE + offset,
                                    bytes, sizeof(bytes)));
    return (uint8_t)ldq_be_p(bytes);
}

static void adapter_write(QTestState *qts, unsigned node, uint64_t offset,
                          uint8_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 2 * node, HUB_BASE + offset,
                                     bytes, sizeof(bytes)));
}

/*
 * The shared wire carries a byte over eighteen clock edges rather than in
 * one register write, so every step ends in the firmware's own poll.
 */
static bool wait_done(QTestState *qts, unsigned node)
{
    unsigned i;

    for (i = 0; i < POLL_STEPS; i++) {
        uint8_t status = adapter_read(qts, node, I2C_CTRL);

        if (!(status & STS_PIN)) {
            return true;
        }
        if (status & (STS_BER | STS_LAB)) {
            return false;
        }
        qtest_clock_step(qts, POLL_STEP_NS);
    }
    return false;
}

static void init_adapter(QTestState *qts, unsigned node)
{
    adapter_write(qts, node, I2C_CTRL, CTL_PIN | CTL_ES1);
    adapter_write(qts, node, I2C_DATA, CLOCK_12MHZ_90KHZ);
    adapter_write(qts, node, I2C_CTRL, CTL_PIN);
    adapter_write(qts, node, I2C_DATA, OWN_ADDRESS(node));
    adapter_write(qts, node, I2C_CTRL, CTL_RUN);
}

static QTestState *start_module_with(const char *extra)
{
    g_autofree char *args = g_strdup_printf("%s%s", MACHINE_ARGS, extra);
    QTestState *qts = qtest_init(args);
    unsigned node;

    for (node = 0; node < ADAPTER_COUNT; node++) {
        init_adapter(qts, node);
    }
    return qts;
}

static QTestState *start_module(void)
{
    return start_module_with("");
}

static bool start_transfer(QTestState *qts, unsigned node, uint8_t address,
                           bool recv)
{
    adapter_write(qts, node, I2C_CTRL, CTL_RUN);
    adapter_write(qts, node, I2C_DATA, (address << 1) | recv);
    adapter_write(qts, node, I2C_CTRL, CTL_START);
    if (!wait_done(qts, node)) {
        return false;
    }
    return !(adapter_read(qts, node, I2C_CTRL) & STS_LRB);
}

static bool restart_transfer(QTestState *qts, unsigned node, uint8_t address,
                             bool recv)
{
    adapter_write(qts, node, I2C_CTRL, CTL_RESTART);
    adapter_write(qts, node, I2C_DATA, (address << 1) | recv);
    if (!wait_done(qts, node)) {
        return false;
    }
    return !(adapter_read(qts, node, I2C_CTRL) & STS_LRB);
}

static bool send_byte(QTestState *qts, unsigned node, uint8_t value)
{
    adapter_write(qts, node, I2C_DATA, value);
    if (!wait_done(qts, node)) {
        return false;
    }
    return !(adapter_read(qts, node, I2C_CTRL) & STS_LRB);
}

/* The first read after a read-direction address returns the stale byte. */
static void discard_byte(QTestState *qts, unsigned node)
{
    (void)adapter_read(qts, node, I2C_DATA);
    g_assert_true(wait_done(qts, node));
}

static uint8_t take_byte(QTestState *qts, unsigned node, bool last)
{
    uint8_t value;

    if (last) {
        adapter_write(qts, node, I2C_CTRL, CTL_NAK);
    }
    value = adapter_read(qts, node, I2C_DATA);
    g_assert_true(wait_done(qts, node));
    return value;
}

static void stop_transfer(QTestState *qts, unsigned node)
{
    unsigned i;

    adapter_write(qts, node, I2C_CTRL, CTL_STOP);
    for (i = 0; i < POLL_STEPS; i++) {
        if (adapter_read(qts, node, I2C_CTRL) & STS_BB) {
            return;
        }
        qtest_clock_step(qts, POLL_STEP_NS);
    }
    g_assert_not_reached();
}

/*
 * A controller command and its answer. The grant scheduler owns the wire
 * once a millisecond, so a transaction that loses arbitration is retried
 * rather than failed: firmware does the same.
 */
static unsigned msc_command(QTestState *qts, unsigned node, const char *text,
                            uint8_t *response, unsigned cap)
{
    unsigned attempt, i, total, len = strlen(text);
    uint8_t request[16], sum = 0;

    g_assert_cmpuint(len + 2, <=, sizeof(request));
    request[0] = len;
    memcpy(&request[1], text, len);
    for (i = 0; i < len + 1; i++) {
        sum += request[i];
    }
    request[len + 1] = (uint8_t)-(int)sum;

    for (attempt = 0; attempt < 8; attempt++) {
        if (!start_transfer(qts, node, MSC_ADDRESS, false)) {
            stop_transfer(qts, node);
            continue;
        }
        for (i = 0; i < len + 2; i++) {
            g_assert_true(send_byte(qts, node, request[i]));
        }
        g_assert_true(restart_transfer(qts, node, MSC_ADDRESS, true));
        discard_byte(qts, node);
        response[0] = take_byte(qts, node, false);
        total = (unsigned)response[0] + 2;
        g_assert_cmpuint(total, <=, cap);
        for (i = 1; i < total; i++) {
            response[i] = take_byte(qts, node, i == total - 1);
        }
        g_assert_false(restart_transfer(qts, node, INVALID_ADDRESS, false));
        stop_transfer(qts, node);

        sum = 0;
        for (i = 0; i < total; i++) {
            sum += response[i];
        }
        g_assert_cmphex(sum, ==, 0);
        return total;
    }
    g_assert_not_reached();
}

static void assert_response(QTestState *qts, unsigned node, const char *text,
                            const char *expected)
{
    uint8_t response[RESP_CAP];

    msc_command(qts, node, text, response, sizeof(response));
    g_assert_cmpuint(response[0], >=, strlen(expected));
    g_assert_cmpmem(&response[1], strlen(expected), expected,
                    strlen(expected));
}

static uint8_t nvram_read(QTestState *qts, unsigned node, uint16_t offset)
{
    uint8_t address = NVRAM_FIRST + (offset >> 8);
    unsigned attempt;

    g_assert_cmpuint(offset, <, NVRAM_BYTES);
    for (attempt = 0; attempt < 8; attempt++) {
        uint8_t value;

        if (!start_transfer(qts, node, address, false) ||
            !send_byte(qts, node, offset & 0xff) ||
            !restart_transfer(qts, node, address, true)) {
            stop_transfer(qts, node);
            continue;
        }
        discard_byte(qts, node);
        value = take_byte(qts, node, true);
        stop_transfer(qts, node);
        return value;
    }
    g_assert_not_reached();
}

static void nvram_write(QTestState *qts, unsigned node, uint16_t offset,
                        uint8_t value)
{
    uint8_t address = NVRAM_FIRST + (offset >> 8);
    unsigned attempt;

    g_assert_cmpuint(offset, <, NVRAM_BYTES);
    for (attempt = 0; attempt < 8; attempt++) {
        if (!start_transfer(qts, node, address, false) ||
            !send_byte(qts, node, offset & 0xff) ||
            !send_byte(qts, node, value)) {
            stop_transfer(qts, node);
            continue;
        }
        stop_transfer(qts, node);
        return;
    }
    g_assert_not_reached();
}

/*
 * Four windows, four adapters. Each keeps the address its own node wrote,
 * so a node reaching the complex is not reaching its neighbour's chip, and
 * no node is left without one. Which adapter landed in which node is not
 * asserted, because nothing a guest can reach tells them apart: they sit at
 * the same offset on one shared wire.
 */
static void test_windows_are_separate_adapters(void)
{
    QTestState *qts = start_module();
    unsigned node;

    for (node = 0; node < ADAPTER_COUNT; node++) {
        adapter_write(qts, node, I2C_CTRL, CTL_PIN);
        g_assert_cmphex(adapter_read(qts, node, I2C_DATA), ==,
                        OWN_ADDRESS(node));
        adapter_write(qts, node, I2C_CTRL, CTL_RUN);
    }
    qtest_quit(qts);
}

/*
 * One controller, answering whichever node's window asks. Four controllers,
 * one per node, would answer "ver" just as happily, so the discriminator is
 * the supervisor latch: it is opened from one node and every other node
 * finds it already open. A per-node controller would answer the second node
 * "err perm" instead, because that one was never given the password.
 */
static void test_controller_answers_every_node(void)
{
    QTestState *qts = start_module();
    unsigned node;

    for (node = 0; node < ADAPTER_COUNT; node++) {
        assert_response(qts, node, "ver", "ok");
    }
    assert_response(qts, 0, "pas none", "ok");
    for (node = 1; node < ADAPTER_COUNT; node++) {
        assert_response(qts, node, "rst", "err cmd");
    }
    qtest_quit(qts);
}

/*
 * The four adapters share one wire rather than owning four private buses:
 * an open transfer from one node leaves every other node's adapter seeing
 * a busy bus.
 */
static void test_windows_share_one_wire(void)
{
    QTestState *qts = start_module();
    unsigned node;

    g_assert_true(start_transfer(qts, 0, MSC_ADDRESS, false));
    for (node = 1; node < ADAPTER_COUNT; node++) {
        g_assert_cmphex(adapter_read(qts, node, I2C_CTRL) & STS_BB, ==, 0);
    }
    stop_transfer(qts, 0);
    for (node = 1; node < ADAPTER_COUNT; node++) {
        g_assert_cmphex(adapter_read(qts, node, I2C_CTRL) & STS_BB, ==,
                        STS_BB);
    }
    qtest_quit(qts);
}

/* A START based on a stale free-bus sample waits for the grant's STOP. */
static void test_start_waits_for_grant(void)
{
    QTestState *qts = start_module();
    int64_t now;
    int64_t next_grant;

    now = qtest_clock_step(qts, SERVICE_PERIOD_NS + GRANT_SETTLE_NS);
    g_assert_cmphex(adapter_read(qts, 0, I2C_CTRL) & STS_BB, ==, STS_BB);
    g_assert_cmphex(adapter_read(qts, 0, I2C_DATA), ==, GRANT_BASE);

    next_grant = ((now / SERVICE_PERIOD_NS) + 1) * SERVICE_PERIOD_NS;
    qtest_clock_step(qts, next_grant + 6 * POLL_STEP_NS - now);
    adapter_write(qts, 0, I2C_DATA, NVRAM_FIRST << 1);
    adapter_write(qts, 0, I2C_CTRL, CTL_START);
    g_assert_cmphex(adapter_read(qts, 0, I2C_CTRL) &
                    (STS_PIN | STS_BB | STS_BER | STS_LAB), ==, STS_PIN);

    qtest_clock_step(qts, 4 * POLL_STEP_NS);
    g_assert_cmphex(adapter_read(qts, 0, I2C_CTRL) &
                    (STS_PIN | STS_BB | STS_BER | STS_LAB), ==, STS_PIN);
    qtest_clock_step(qts, GRANT_SETTLE_NS);
    g_assert_true(wait_done(qts, 0));
    g_assert_cmphex(adapter_read(qts, 0, I2C_CTRL) &
                    (STS_LRB | STS_BER | STS_LAB), ==, 0);
    stop_transfer(qts, 0);
    qtest_quit(qts);
}

/* One logical NVRAM space, not one per node. */
static void test_nvram_is_shared(void)
{
    QTestState *qts = start_module();
    const uint16_t offsets[] = { 0x000, 0x123, 0x400 };
    unsigned i;

    for (i = 0; i < G_N_ELEMENTS(offsets); i++) {
        uint8_t value = 0xa0 + i;

        nvram_write(qts, i, offsets[i], value);
        g_assert_cmphex(nvram_read(qts, ADAPTER_COUNT - 1, offsets[i]), ==,
                        value);
    }
    qtest_quit(qts);
}

/*
 * Nothing in this machine receives a module reset or a power sequence, so
 * the controller refuses those commands. Answering "ok" would tell firmware
 * the reset happened and stop it falling back to its own.
 */
static void test_destructive_commands_refused(void)
{
    QTestState *qts = start_module();

    assert_response(qts, 0, "pas none", "ok");
    assert_response(qts, 0, "rst", "err cmd");
    assert_response(qts, 0, "pwr d 0", "err cmd");
    assert_response(qts, 0, "pwr c", "err cmd");
    /* A command the module does carry out still answers normally. */
    assert_response(qts, 0, "ver", "ok");
    qtest_quit(qts);
}

static bool is_running(QTestState *qts)
{
    g_autoptr(QDict) response = qtest_qmp(qts, "{'execute':'query-status'}");

    return qdict_get_bool(qdict_get_qdict(response, "return"), "running");
}

/*
 * A completed migration on the source does not mean the destination has
 * started. Its virtual clock stays stopped until it does, and a stopped
 * clock runs no timers, so the shared wire would never clock a bit.
 */
static void wait_until_running(QTestState *qts)
{
    int64_t deadline = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;

    while (!is_running(qts)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

/*
 * The module's DIP switch bank belongs to the whole module, and the machine
 * setting has to reach the one controller behind the four windows. The
 * controller reports it active-low, so 0x10 comes back as "ef".
 */
static void test_dip_switches_reach_the_controller(void)
{
    QTestState *qts = start_module_with(" -machine elsc-dip-switches=16");

    assert_response(qts, 2, "rsw", "ok ef");
    qtest_quit(qts);
}

static void migrate_module(QTestState *from, QTestState *to)
{
    g_autofree char *directory = g_dir_make_tmp("o2000-management-XXXXXX",
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

/*
 * The complex sits on no bus and carries its own migration state. What
 * survives is the shared store and a controller that still talks, on a
 * destination whose adapters were never programmed by a guest.
 *
 * The qtest accelerator's virtual clock is the test's own counter and does
 * not travel with the migration stream, so the destination starts at zero
 * holding the source's absolute deadlines: the scheduler's next tick, and
 * the bus-free time each adapter must see before a fresh START. Put the
 * destination clock where the source left it, and everything the stream
 * restored is due rather than far in the future.
 */
static void test_migration(void)
{
    QTestState *from = start_module();
    QTestState *to = qtest_init(MACHINE_ARGS " -incoming defer");
    int64_t departed;

    nvram_write(from, 2, 0x321, 0x5a);
    /* A zero step is refused, so read the clock by taking the smallest. */
    departed = qtest_clock_step(from, 1);
    migrate_module(from, to);
    wait_until_running(to);
    qtest_clock_set(to, departed);
    g_assert_cmphex(nvram_read(to, 0, 0x321), ==, 0x5a);
    assert_response(to, 3, "ver", "ok");
    qtest_quit(from);
    qtest_quit(to);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (!qtest_has_machine("origin2000")) {
        return 0;
    }
    qtest_add_func("/sn0/o2000-module-management/separate-adapters",
                   test_windows_are_separate_adapters);
    qtest_add_func("/sn0/o2000-module-management/one-controller",
                   test_controller_answers_every_node);
    qtest_add_func("/sn0/o2000-module-management/one-wire",
                   test_windows_share_one_wire);
    qtest_add_func("/sn0/o2000-module-management/start-waits-for-grant",
                   test_start_waits_for_grant);
    qtest_add_func("/sn0/o2000-module-management/shared-nvram",
                   test_nvram_is_shared);
    qtest_add_func("/sn0/o2000-module-management/destructive-refused",
                   test_destructive_commands_refused);
    qtest_add_func("/sn0/o2000-module-management/dip-switches",
                   test_dip_switches_reach_the_controller);
    qtest_add_func("/sn0/o2000-module-management/migration", test_migration);
    return g_test_run();
}
