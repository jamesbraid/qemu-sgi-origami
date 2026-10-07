/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Test the Origin 2000 module's shared management complex through each Hub
 * adapter.
 */
/*
 * Four node windows share one controller and NVRAM space; no reset, NMI or
 * power path exists, so destructive commands are refused.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"
#include "sgi-wait.h"

#define HUB_BASE        UINT64_C(0x20001000000)
#define MD_ONEWIRE      0x2000a8
#define I2C_DATA        0x220000
#define I2C_CTRL        0x220008

#define HUB_ONEWIRE_RXDATA   (1ULL << 0)
#define HUB_ONEWIRE_DONE     (1ULL << 1)
#define HUB_ONEWIRE_RESET    ((0x31ULL << 20) | (0x31ULL << 27))
#define HUB_ONEWIRE_SHORT    8
#define HUB_ONEWIRE_ZERO     60

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
#define NVRAM_IDENTITY_PAGE 0x700
#define NVRAM_VALID_MARKER (NVRAM_IDENTITY_PAGE + 0)
#define NVRAM_MODULE_NUMBER (NVRAM_IDENTITY_PAGE + 8)
#define NVRAM_PARTITION (NVRAM_IDENTITY_PAGE + 10)
#define NVRAM_VALID_VALUE 0x37
#define INITIAL_MODULE_NUMBER 1
#define ADAPTER_COUNT   4
#define MODULE_SERIAL_LEN 10
#define RESP_CAP        96

/* An address nothing on the module answers, used to resync on the way out. */
#define INVALID_ADDRESS 0x7f
#define OWN_ADDRESS(node)   (0x09 + (node))
#define CLOCK_12MHZ_90KHZ   0x1c

/* One 90 kHz byte takes about 100us; poll well past that before failing. */
#define POLL_STEP_NS    2000
#define POLL_STEPS      400
/*
 * Arbitration is decided in the first microseconds after an action, so poll
 * finely there and coarsely afterwards. A byte is about 100 us and the grant
 * rotation about 10 ms, so a uniform fine step spends fifty round trips on
 * every byte and still cannot outlast a grant.
 */
#define POLL_FINE_STEPS 8
#define POLL_COARSE_NS  50000
/* Sixteen of a byte's eighteen clock edges at 90 kHz (5556 ns each). */
#define BYTE_PRESTEP_NS (16 * 5556)
#define SERVICE_PERIOD_NS   1000000
#define GRANT_SETTLE_NS     500000
#define GRANT_BASE          0xe0

#define MACHINE_ARGS "-M origin2000 -smp 8 -m 256M"

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
 * The shared wire carries a byte over eighteen clock edges rather than in one
 * register write, so every step ends in the firmware's own poll.
 */
static bool wait_done_from(QTestState *qts, unsigned node, unsigned i)
{
    for (; i < POLL_STEPS; i++) {
        uint8_t status = adapter_read(qts, node, I2C_CTRL);

        if (!(status & STS_PIN)) {
            return true;
        }
        if (status & (STS_BER | STS_LAB)) {
            return false;
        }
        qtest_clock_step(qts, i < POLL_FINE_STEPS ? POLL_STEP_NS
                                                  : POLL_COARSE_NS);
    }
    return false;
}

static bool wait_done(QTestState *qts, unsigned node)
{
    return wait_done_from(qts, node, 0);
}

/*
 * A data byte or address already on the wire cannot finish before its
 * eighteen edges, and arbitration was settled by the start that came first.
 * Step most of that time at once and poll coarsely for the rest, rather than
 * spending round trips on fine polls that cannot see anything. A host that
 * schedules the test and QEMU slowly pays for every round trip, so this
 * keeps a transaction's cost near its byte count.
 */
static bool wait_byte_done(QTestState *qts, unsigned node)
{
    qtest_clock_step(qts, BYTE_PRESTEP_NS);
    return wait_done_from(qts, node, POLL_FINE_STEPS);
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
    if (!wait_byte_done(qts, node)) {
        return false;
    }
    return !(adapter_read(qts, node, I2C_CTRL) & STS_LRB);
}

static bool send_byte(QTestState *qts, unsigned node, uint8_t value)
{
    adapter_write(qts, node, I2C_DATA, value);
    if (!wait_byte_done(qts, node)) {
        return false;
    }
    return !(adapter_read(qts, node, I2C_CTRL) & STS_LRB);
}

/* The first read after a read-direction address returns the stale byte. */
static void discard_byte(QTestState *qts, unsigned node)
{
    (void)adapter_read(qts, node, I2C_DATA);
    g_assert_true(wait_byte_done(qts, node));
}

static uint8_t take_byte(QTestState *qts, unsigned node, bool last)
{
    uint8_t value;

    if (last) {
        adapter_write(qts, node, I2C_CTRL, CTL_NAK);
    }
    value = adapter_read(qts, node, I2C_DATA);
    g_assert_true(wait_byte_done(qts, node));
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
 * A controller command and its answer. The grant scheduler owns the wire once
 * a millisecond, so a transaction that loses arbitration is retried rather
 * than failed: firmware does the same.
 */
static bool msc_command_once(QTestState *qts, unsigned node, const char *text,
                             uint8_t *response, unsigned cap,
                             unsigned *response_size)
{
    unsigned i, total, len = strlen(text);
    uint8_t request[16], sum = 0;

    g_assert_cmpuint(len + 2, <=, sizeof(request));
    request[0] = len;
    memcpy(&request[1], text, len);
    for (i = 0; i < len + 1; i++) {
        sum += request[i];
    }
    request[len + 1] = (uint8_t)-(int)sum;

    if (!start_transfer(qts, node, MSC_ADDRESS, false)) {
        stop_transfer(qts, node);
        return false;
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
    *response_size = total;
    return true;
}

static unsigned msc_command(QTestState *qts, unsigned node, const char *text,
                            uint8_t *response, unsigned cap)
{
    unsigned attempt, total;

    for (attempt = 0; attempt < 8; attempt++) {
        if (msc_command_once(qts, node, text, response, cap, &total)) {
            return total;
        }
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

static bool onewire_slot(QTestState *qts, unsigned node, unsigned pulse,
                         unsigned sample, unsigned delay)
{
    g_autofree char *command = g_strdup_printf("nic %x %x %x", pulse,
                                               sample, delay);
    uint8_t response[RESP_CAP];

    msc_command(qts, node, command, response, sizeof(response));
    g_assert_cmpuint(response[0], ==, 4);
    g_assert_cmpmem(&response[1], 3, "ok ", 3);
    g_assert_true(response[4] == '0' || response[4] == '1');
    return response[4] == '1';
}

static bool onewire_reset(QTestState *qts, unsigned node)
{
    return !onewire_slot(qts, node, 520, 65, 500);
}

static void onewire_write_bit(QTestState *qts, unsigned node, bool value)
{
    if (value) {
        onewire_slot(qts, node, 6, 110, 0);
    } else {
        onewire_slot(qts, node, 80, 30, 0);
    }
}

static bool onewire_read_bit(QTestState *qts, unsigned node)
{
    return onewire_slot(qts, node, 6, 13, 100);
}

static void onewire_write_byte(QTestState *qts, unsigned node, uint8_t value)
{
    for (unsigned bit = 0; bit < 8; bit++) {
        onewire_write_bit(qts, node, value & (1u << bit));
    }
}

static uint8_t onewire_read_byte(QTestState *qts, unsigned node)
{
    uint8_t value = 0;

    for (unsigned bit = 0; bit < 8; bit++) {
        value |= onewire_read_bit(qts, node) << bit;
    }
    return value;
}

/* Address one part: the midplane line carries more than one. */
static void onewire_select(QTestState *qts, unsigned node, const uint8_t rom[8])
{
    g_assert_true(onewire_reset(qts, node));
    onewire_write_byte(qts, node, 0x55);
    for (unsigned i = 0; i < 8; i++) {
        onewire_write_byte(qts, node, rom[i]);
    }
}

static void onewire_start_read(QTestState *qts, unsigned node,
                               const uint8_t rom[8], uint8_t command,
                               uint16_t address)
{
    onewire_select(qts, node, rom);
    onewire_write_byte(qts, node, command);
    onewire_write_byte(qts, node, address);
    onewire_write_byte(qts, node, address >> 8);
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

/* @count bytes from @offset in one transaction, as the controller does. */
static void nvram_write_block(QTestState *qts, unsigned node, uint16_t offset,
                              const uint8_t *data, size_t count)
{
    uint8_t address = NVRAM_FIRST + (offset >> 8);
    unsigned attempt;

    g_assert_cmpuint(offset + count, <=, NVRAM_BYTES);
    for (attempt = 0; attempt < 8; attempt++) {
        size_t i = 0;

        if (start_transfer(qts, node, address, false) &&
            send_byte(qts, node, offset & 0xff)) {
            for (i = 0; i < count; i++) {
                if (!send_byte(qts, node, data[i])) {
                    break;
                }
            }
        }
        stop_transfer(qts, node);
        if (i == count) {
            return;
        }
    }
    g_assert_not_reached();
}

/* A caller's 2048-byte store file, in a directory of its own. */
typedef struct NvramStore {
    char *dir;
    char *path;
    char *option;
} NvramStore;

static void nvram_store_create(NvramStore *store, const uint8_t *contents)
{
    uint8_t zero[NVRAM_BYTES] = { };

    store->dir = g_dir_make_tmp("o2000-nvram-XXXXXX", NULL);
    g_assert_nonnull(store->dir);
    store->path = g_build_filename(store->dir, "module0-nvram.raw", NULL);
    g_assert_true(g_file_set_contents(store->path,
                                      (const char *)(contents ?: zero),
                                      NVRAM_BYTES, NULL));
    store->option = g_strdup_printf(
        " -M origin2000,module0-nvram=mnv -blockdev driver=raw,"
        "node-name=mnv,file.driver=file,file.filename=%s", store->path);
}

static void nvram_store_destroy(NvramStore *store)
{
    unlink(store->path);
    g_assert_cmpint(rmdir(store->dir), ==, 0);
    g_free(store->path);
    g_free(store->dir);
    g_free(store->option);
}

/*
 * Four windows, four adapters. Each keeps the address its own node wrote, so
 * a node reaching the complex is not reaching its neighbour's chip, and no
 * node is left without one. Which adapter landed in which node is not
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
 * the test-and-set lock cell: it is taken from one node and every other node
 * finds it already held. A per-node controller would answer another node
 * "ok 0" instead, because its own cell was never taken.
 */
static void test_controller_answers_every_node(void)
{
    QTestState *qts = start_module();
    unsigned node;

    for (node = 0; node < ADAPTER_COUNT; node++) {
        assert_response(qts, node, "ver", "ok");
    }
    assert_response(qts, 0, "tas A", "ok 0");
    for (node = 1; node < ADAPTER_COUNT; node++) {
        assert_response(qts, node, "tas B", "ok A");
    }
    qtest_quit(qts);
}

/*
 * The four adapters share one wire rather than owning four private buses: an
 * open transfer from one node leaves every other node's adapter seeing a busy
 * bus.
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

static void wait_for_global_grant_within(QTestState *qts, uint8_t expected,
                                         unsigned steps)
{
    for (unsigned step = 0; step < steps; step++) {
        bool seen = true;

        for (unsigned node = 0; node < ADAPTER_COUNT; node++) {
            uint8_t status = adapter_read(qts, node, I2C_CTRL);

            g_assert_cmphex(status & (STS_BER | STS_LAB), ==, 0);
            if (!(status & STS_BB) ||
                adapter_read(qts, node, I2C_DATA) != expected) {
                seen = false;
            }
        }
        if (seen) {
            return;
        }
        qtest_clock_step(qts, POLL_STEP_NS);
    }
    g_assert_not_reached();
}

static void wait_for_global_grant(QTestState *qts, uint8_t expected)
{
    wait_for_global_grant_within(qts, expected, 2 * POLL_STEPS);
}

/*
 * The next grant published to every adapter, whatever code it carries. The
 * budget covers a whole eight-code ring, so a caller that has just missed one
 * still sees the one after it.
 */
static uint8_t wait_for_next_global_grant(QTestState *qts, uint8_t previous)
{
    for (unsigned step = 0; step < 20 * POLL_STEPS; step++) {
        uint8_t value = 0;
        bool seen = true;

        for (unsigned node = 0; node < ADAPTER_COUNT; node++) {
            uint8_t status = adapter_read(qts, node, I2C_CTRL);
            uint8_t data;

            g_assert_cmphex(status & (STS_BER | STS_LAB), ==, 0);
            data = adapter_read(qts, node, I2C_DATA);
            if (!(status & STS_BB) || data < GRANT_BASE ||
                data > (GRANT_BASE | 7) || (node && data != value)) {
                seen = false;
                break;
            }
            value = data;
        }
        if (seen && value != previous) {
            return value;
        }
        qtest_clock_step(qts, POLL_STEP_NS);
    }
    g_assert_not_reached();
}

/*
 * Firmware polls for its grant, then writes START. A processor that loses
 * host time between the two writes START while the controller is already
 * publishing the next grant. The adapter must hold that START until the
 * grant's STOP and then run it. Answering it with a lost arbitration is fatal
 * here: the PROM reads the controller block one byte per transaction and does
 * not come back for a byte it was refused, and when that byte is the module
 * number the node boots on a stale one and the nodes disagree.
 */
static void test_late_start_still_reads_the_module_number(void)
{
    QTestState *qts = start_module();
    int64_t now;
    int64_t next_grant;

    now = qtest_clock_step(qts, SERVICE_PERIOD_NS + GRANT_SETTLE_NS);
    g_assert_cmphex(adapter_read(qts, 0, I2C_CTRL) & STS_BB, ==, STS_BB);
    g_assert_cmphex(adapter_read(qts, 0, I2C_DATA), ==, GRANT_BASE);

    /* Twelve microseconds into the next grant's address phase. */
    next_grant = ((now / SERVICE_PERIOD_NS) + 1) * SERVICE_PERIOD_NS;
    qtest_clock_step(qts, next_grant + 6 * POLL_STEP_NS - now);
    g_assert_cmphex(adapter_read(qts, 0, I2C_CTRL) & STS_BB, ==, 0);
    adapter_write(qts, 0, I2C_DATA, (NVRAM_FIRST + 7) << 1);
    adapter_write(qts, 0, I2C_CTRL, CTL_START);
    g_assert_cmphex(adapter_read(qts, 0, I2C_CTRL) &
                    (STS_PIN | STS_BB | STS_BER | STS_LAB), ==, STS_PIN);

    qtest_clock_step(qts, 4 * POLL_STEP_NS);
    g_assert_cmphex(adapter_read(qts, 0, I2C_CTRL) &
                    (STS_PIN | STS_BB | STS_BER | STS_LAB), ==, STS_PIN);

    /* The grant stops; the held START opens the bus and the part answers. */
    g_assert_true(wait_done(qts, 0));
    g_assert_cmphex(adapter_read(qts, 0, I2C_CTRL) &
                    (STS_LRB | STS_BER | STS_LAB | STS_BB), ==, 0);
    g_assert_true(send_byte(qts, 0, NVRAM_MODULE_NUMBER & 0xff));
    g_assert_true(restart_transfer(qts, 0, NVRAM_FIRST + 7, true));
    discard_byte(qts, 0);
    g_assert_cmphex(take_byte(qts, 0, true), ==, INITIAL_MODULE_NUMBER);
    g_assert_false(restart_transfer(qts, 0, INVALID_ADDRESS, false));
    stop_transfer(qts, 0);

    /* The late transfer deferred the ring: no code rolled back or skipped. */
    g_assert_cmphex(wait_for_next_global_grant(qts,
                                               adapter_read(qts, 0, I2C_DATA)),
                    ==, GRANT_BASE | 2);
    qtest_quit(qts);
}

/*
 * The grant ring covers all eight processors through four adapters. Keep the
 * two middle Hubs between the endpoint controls, issue a real controller
 * transaction in every slot, and require the next grant to be consecutive. A
 * retry here would conceal the skipped-slot failure this test is for.
 */
static void test_global_grant_rotation(void)
{
    static const unsigned adapter_by_processor[] = { 0, 0, 1, 1, 2, 2, 3, 3 };
    QTestState *qts = start_module();

    for (unsigned reset = 0; reset < 2; reset++) {
        for (unsigned processor = 0;
             processor < G_N_ELEMENTS(adapter_by_processor); processor++) {
            unsigned node = adapter_by_processor[processor];
            uint8_t response[RESP_CAP];
            unsigned response_size;

            wait_for_global_grant(qts, GRANT_BASE | processor);
            g_assert_true(msc_command_once(qts, node, "ver", response,
                                           sizeof(response), &response_size));
            g_assert_cmpuint(response[0], >=, 2);
            g_assert_cmpmem(&response[1], 2, "ok", 2);
            g_assert_cmphex(adapter_read(qts, node, I2C_CTRL) &
                            (STS_BER | STS_LAB), ==, 0);
        }
        if (!reset) {
            qtest_system_reset(qts);
            for (unsigned node = 0; node < ADAPTER_COUNT; node++) {
                init_adapter(qts, node);
            }
        }
    }
    qtest_quit(qts);
}

/*
 * An adapter that reads the grant it owns and starts nothing must not keep
 * that grant on the wire past the next service tick. Firmware polls the data
 * register on every processor, including for codes it has no transfer for, so
 * a controller that waits for the reader's START freezes the ring. Waiting
 * for code 0 reads it through adapter 0, its owner. A hold timed in host time
 * passes here only if the host stalls that long between the read and the
 * clock step.
 */
static void test_read_grant_keeps_rotating(void)
{
    QTestState *qts = start_module();

    wait_for_global_grant(qts, GRANT_BASE);
    qtest_clock_step(qts, SERVICE_PERIOD_NS + GRANT_SETTLE_NS);
    for (unsigned node = 0; node < ADAPTER_COUNT; node++) {
        g_assert_cmphex(adapter_read(qts, node, I2C_CTRL) & STS_BB, ==,
                        STS_BB);
        g_assert_cmphex(adapter_read(qts, node, I2C_DATA), ==,
                        GRANT_BASE | 1);
    }
    qtest_quit(qts);
}

/*
 * A refused closing address must still accept the following full-byte STOP.
 */
static void test_invalid_address_stop_releases_wire(void)
{
    QTestState *qts = start_module();
    uint8_t status;

    wait_for_global_grant(qts, GRANT_BASE);
    g_assert_true(start_transfer(qts, 0, NVRAM_FIRST + 7, false));
    g_assert_true(send_byte(qts, 0, 1));
    g_assert_true(restart_transfer(qts, 0, NVRAM_FIRST + 7, true));
    discard_byte(qts, 0);
    (void)take_byte(qts, 0, true);

    g_assert_false(restart_transfer(qts, 0, INVALID_ADDRESS, false));
    status = adapter_read(qts, 0, I2C_CTRL);
    g_assert_cmphex(status & (STS_PIN | STS_BER | STS_LAB | STS_LRB | STS_BB),
                    ==, STS_LRB);
    for (unsigned node = 1; node < ADAPTER_COUNT; node++) {
        g_assert_cmphex(adapter_read(qts, node, I2C_CTRL) & STS_BB, ==, 0);
    }

    /*
     * This is the exact fresh control byte, not bits merged with old state.
     */
    adapter_write(qts, 0, I2C_CTRL, 0xc2);
    for (unsigned step = 0; step < POLL_STEPS; step++) {
        bool free = true;

        for (unsigned node = 0; node < ADAPTER_COUNT; node++) {
            if (!(adapter_read(qts, node, I2C_CTRL) & STS_BB)) {
                free = false;
            }
        }
        if (free) {
            wait_for_global_grant(qts, GRANT_BASE | 1);
            qtest_quit(qts);
            return;
        }
        qtest_clock_step(qts, POLL_STEP_NS);
    }
    g_assert_not_reached();
}

/*
 * Stock firmware measures a whole transaction against one deadline and, when
 * it passes with PIN still inactive, writes STOP straight away, with its data
 * byte still on the wire. The STOP must still end the transfer, or the bus
 * stays busy and the controller never publishes another grant, so every
 * node's next grant wait runs out.
 */
static void test_mid_byte_stop_releases_wire(void)
{
    QTestState *qts = start_module();

    wait_for_global_grant(qts, GRANT_BASE);
    g_assert_true(start_transfer(qts, 0, MSC_ADDRESS, false));
    adapter_write(qts, 0, I2C_DATA, 3);
    g_assert_cmphex(adapter_read(qts, 0, I2C_CTRL) & STS_PIN, ==, STS_PIN);
    adapter_write(qts, 0, I2C_CTRL, CTL_STOP);

    for (unsigned step = 0; step < POLL_STEPS; step++) {
        bool free = true;

        for (unsigned node = 0; node < ADAPTER_COUNT; node++) {
            uint8_t status = adapter_read(qts, node, I2C_CTRL);

            g_assert_cmphex(status & (STS_BER | STS_LAB), ==, 0);
            if (!(status & STS_BB)) {
                free = false;
            }
        }
        if (free) {
            wait_for_next_global_grant(qts, GRANT_BASE);
            qtest_quit(qts);
            return;
        }
        qtest_clock_step(qts, POLL_STEP_NS);
    }
    g_assert_not_reached();
}

static bool bus_free_everywhere(QTestState *qts)
{
    for (unsigned node = 0; node < ADAPTER_COUNT; node++) {
        if (!(adapter_read(qts, node, I2C_CTRL) & STS_BB)) {
            return false;
        }
    }
    return true;
}

/*
 * The same deadline STOP, landing after an acknowledged byte of a read from
 * the controller. The acknowledgement tells the controller to send on, and it
 * drives the next byte's first bit, a zero for an ASCII answer, onto SDA. No
 * STOP can form under that until the controller lets go, which it does when
 * its recovery count runs out and it reinitializes. Until then every grant
 * wait on the module fails; after it the bus must be free again and the
 * grants must be moving.
 */
#define RECOVERY_NS         (INT64_C(5000) * SERVICE_PERIOD_NS)
#define RECOVERY_STEP_NS    (10 * SERVICE_PERIOD_NS)

static void test_stop_after_acked_read_byte_recovers(void)
{
    QTestState *qts = start_module();
    static const uint8_t request[] = { 3, 'v', 'e', 'r',
                                       (uint8_t)-(3 + 'v' + 'e' + 'r') };
    int64_t stopped;

    wait_for_global_grant(qts, GRANT_BASE);
    g_assert_true(start_transfer(qts, 0, MSC_ADDRESS, false));
    for (unsigned i = 0; i < sizeof(request); i++) {
        g_assert_true(send_byte(qts, 0, request[i]));
    }
    g_assert_true(restart_transfer(qts, 0, MSC_ADDRESS, true));
    discard_byte(qts, 0);
    g_assert_cmpuint(take_byte(qts, 0, false), >=, 2);
    g_assert_cmphex(take_byte(qts, 0, false), ==, 'o');

    adapter_write(qts, 0, I2C_CTRL, CTL_STOP);
    stopped = qtest_clock_step(qts, SERVICE_PERIOD_NS);
    g_assert_false(bus_free_everywhere(qts));

    for (;;) {
        int64_t now = qtest_clock_step(qts, RECOVERY_STEP_NS);

        for (unsigned node = 0; node < ADAPTER_COUNT; node++) {
            g_assert_cmphex(adapter_read(qts, node, I2C_CTRL) &
                            (STS_BER | STS_LAB), ==, 0);
        }
        if (bus_free_everywhere(qts)) {
            break;
        }
        g_assert_cmpint(now - stopped, <, RECOVERY_NS + RECOVERY_STEP_NS);
    }
    wait_for_next_global_grant(qts, adapter_read(qts, 0, I2C_DATA));
    qtest_quit(qts);
}

/*
 * A held STOP goes out after the byte, not in place of its completion: the
 * host that wrote it still sees PIN go active for that byte, and sees it
 * while the transfer is open, before the STOP reaches the line.
 */
static void test_held_stop_follows_the_finished_byte(void)
{
    QTestState *qts = start_module();

    wait_for_global_grant(qts, GRANT_BASE);
    g_assert_true(start_transfer(qts, 0, NVRAM_FIRST, false));
    adapter_write(qts, 0, I2C_DATA, 0x10);
    adapter_write(qts, 0, I2C_CTRL, CTL_STOP);
    g_assert_cmphex(adapter_read(qts, 0, I2C_CTRL) & (STS_PIN | STS_BB), ==,
                    STS_PIN);

    for (unsigned step = 0; step < POLL_STEPS; step++) {
        uint8_t status = adapter_read(qts, 0, I2C_CTRL);

        g_assert_cmphex(status & (STS_BER | STS_LAB), ==, 0);
        if (!(status & STS_PIN)) {
            /*
             * The byte is done and acknowledged; the STOP is still to come.
             */
            g_assert_cmphex(status & (STS_LRB | STS_BB), ==, 0);
            for (unsigned node = 1; node < ADAPTER_COUNT; node++) {
                g_assert_cmphex(adapter_read(qts, node, I2C_CTRL) & STS_BB,
                                ==, 0);
            }
            for (step = 0; step < POLL_STEPS; step++) {
                if (bus_free_everywhere(qts)) {
                    qtest_quit(qts);
                    return;
                }
                qtest_clock_step(qts, POLL_STEP_NS);
            }
            g_assert_not_reached();
        }
        /* Finer than the STOP's own clock, so the gap cannot be missed. */
        qtest_clock_step(qts, POLL_STEP_NS / 2);
    }
    g_assert_not_reached();
}

/*
 * A fresh transfer from an adapter that once had a STOP held against it: its
 * address byte must leave the transfer open. A leftover held STOP would close
 * the bus right behind that byte.
 */
static void assert_fresh_transfer_stays_open(QTestState *qts, unsigned node)
{
    unsigned other = (node + 1) % ADAPTER_COUNT;

    g_assert_true(start_transfer(qts, node, NVRAM_FIRST, false));
    for (unsigned step = 0; step < 25; step++) {
        g_assert_cmphex(adapter_read(qts, node, I2C_CTRL) &
                        (STS_PIN | STS_BB | STS_BER | STS_LAB), ==, 0);
        g_assert_cmphex(adapter_read(qts, other, I2C_CTRL) & STS_BB, ==, 0);
        qtest_clock_step(qts, POLL_STEP_NS);
    }
    stop_transfer(qts, node);
}

/*
 * A STOP written while the adapter's own address byte is being arbitrated, by
 * an adapter that then loses that byte, belongs to a transfer that never
 * became its own. The loss drops it with the rest of the attempt, so neither
 * the winner's transfer nor the loser's next one is closed by it.
 */
static void test_lost_arbitration_drops_a_held_stop(void)
{
    QTestState *qts = start_module();

    wait_for_global_grant(qts, GRANT_BASE);
    adapter_write(qts, 0, I2C_CTRL, CTL_RUN);
    adapter_write(qts, 1, I2C_CTRL, CTL_RUN);
    /* 0xa0 against 0xa2: adapter 1 sends a one on bit 6 and loses there. */
    adapter_write(qts, 0, I2C_DATA, NVRAM_FIRST << 1);
    adapter_write(qts, 1, I2C_DATA, (NVRAM_FIRST + 1) << 1);
    adapter_write(qts, 0, I2C_CTRL, CTL_START);
    adapter_write(qts, 1, I2C_CTRL, CTL_START);

    /* Past the START hold and into the address bits, well ahead of bit 6. */
    qtest_clock_step(qts, 20000);
    g_assert_cmphex(adapter_read(qts, 1, I2C_CTRL) &
                    (STS_PIN | STS_BB | STS_LAB), ==, STS_PIN);
    adapter_write(qts, 1, I2C_CTRL, CTL_STOP);

    g_assert_true(wait_done(qts, 0));
    g_assert_cmphex(adapter_read(qts, 0, I2C_CTRL) & (STS_LRB | STS_BB), ==, 0);
    g_assert_cmphex(adapter_read(qts, 1, I2C_CTRL) & (STS_LAB | STS_BB), ==,
                    STS_LAB);

    /* The winner's transfer stays open: nothing the loser held reaches it. */
    qtest_clock_step(qts, 25 * POLL_STEP_NS);
    for (unsigned node = 0; node < ADAPTER_COUNT; node++) {
        g_assert_cmphex(adapter_read(qts, node, I2C_CTRL) & STS_BB, ==, 0);
    }
    stop_transfer(qts, 0);

    adapter_write(qts, 1, I2C_CTRL, CTL_RUN);
    wait_for_next_global_grant(qts, GRANT_BASE);
    assert_fresh_transfer_stays_open(qts, 1);
    qtest_quit(qts);
}

/*
 * A STOP from adapter 1 lands in the gap between the owner's STOP releasing
 * its queued START and that START reaching the line. Adapter 0's queued START
 * gets there first, which returns adapter 1's to the queue. The STOP was
 * written against a START that is now waiting, not a transfer adapter 1 owns,
 * and a STOP from a waiting adapter withdraws its request. Adapter 2's STOP
 * is on the line about 5.6 us after it is written, and the released STARTs go
 * out 4.7 us of bus-free time after that, so a STOP from adapter 1 7 us after
 * adapter 2's falls inside the gap. This returns with adapter 0 holding the
 * bus after its address byte.
 */
static void stop_against_a_released_start(QTestState *qts)
{
    wait_for_global_grant(qts, GRANT_BASE);
    g_assert_true(start_transfer(qts, 2, NVRAM_FIRST, false));
    for (unsigned node = 0; node < 2; node++) {
        adapter_write(qts, node, I2C_CTRL, CTL_RUN);
        adapter_write(qts, node, I2C_DATA, NVRAM_FIRST << 1);
        adapter_write(qts, node, I2C_CTRL, CTL_START);
        g_assert_cmphex(adapter_read(qts, node, I2C_CTRL) &
                        (STS_PIN | STS_BB | STS_BER | STS_LAB), ==, STS_PIN);
    }
    adapter_write(qts, 2, I2C_CTRL, CTL_STOP);
    qtest_clock_step(qts, 7000);
    g_assert_cmphex(adapter_read(qts, 1, I2C_CTRL) & STS_BB, ==, STS_BB);
    adapter_write(qts, 1, I2C_CTRL, CTL_STOP);

    g_assert_true(wait_done(qts, 0));
    g_assert_cmphex(adapter_read(qts, 0, I2C_CTRL) &
                    (STS_LRB | STS_BB | STS_BER | STS_LAB), ==, 0);
}

/*
 * Once the bus frees, adapter 1 must not run the START it withdrew: its PIN
 * stays inactive, which it would not if an address byte of its own ran.
 */
static void assert_withdrawn_start_stays_withdrawn(QTestState *qts)
{
    stop_transfer(qts, 0);
    for (unsigned step = 0; step < 150; step++) {
        g_assert_cmphex(adapter_read(qts, 1, I2C_CTRL) &
                        (STS_PIN | STS_BER | STS_LAB), ==, STS_PIN);
        qtest_clock_step(qts, POLL_STEP_NS);
    }
    wait_for_next_global_grant(qts, GRANT_BASE);
    assert_fresh_transfer_stays_open(qts, 1);
}

static void test_stop_withdraws_a_released_start(void)
{
    QTestState *qts = start_module();

    stop_against_a_released_start(qts);
    assert_withdrawn_start_stays_withdrawn(qts);
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
 * One adapter's open transfer must survive another adapter addressing the
 * same part. The part keeps a single address counter, as the hardware does,
 * so a second adapter reaching it would move the counter the first is about
 * to read from. Firmware reads the module number out of this block on four
 * nodes at once, and a moved counter returns whichever byte the other adapter
 * selected instead.
 */
static void test_nvram_read_survives_a_second_adapter(void)
{
    QTestState *qts = start_module();
    uint8_t mine;

    nvram_write(qts, 0, 0x010, 0x5a);
    nvram_write(qts, 0, 0x020, 0xa5);

    /* Adapter 0 opens a read of 0x010 and stops short of taking the byte. */
    g_assert_true(start_transfer(qts, 0, NVRAM_FIRST, false));
    g_assert_true(send_byte(qts, 0, 0x10));
    g_assert_true(restart_transfer(qts, 0, NVRAM_FIRST, true));

    /*
     * Adapter 1 addresses the same part while that transfer is open. The bus
     * is not free, so its START waits rather than reaching the part, and it
     * must leave the open read alone while it waits.
     */
    adapter_write(qts, 1, I2C_CTRL, CTL_RUN);
    adapter_write(qts, 1, I2C_DATA, NVRAM_FIRST << 1);
    adapter_write(qts, 1, I2C_CTRL, CTL_START);
    g_assert_false(wait_done(qts, 1));
    g_assert_cmphex(adapter_read(qts, 1, I2C_CTRL) & (STS_BER | STS_LAB),
                    ==, 0);
    /* Withdraw it, so ending the first transfer is not a race with it. */
    adapter_write(qts, 1, I2C_CTRL, CTL_PIN);

    discard_byte(qts, 0);
    mine = take_byte(qts, 0, true);
    stop_transfer(qts, 0);
    g_assert_cmphex(mine, ==, 0x5a);

    /* That node's own turn still reaches the part it was refused. */
    nvram_write(qts, 1, 0x020, 0x5b);
    g_assert_cmphex(nvram_read(qts, 1, 0x020), ==, 0x5b);
    qtest_quit(qts);
}

/*
 * More precisely: a second adapter starting while the first is part way
 * through a byte, rather than between transfers. The wire resolves
 * arbitration edge by edge, so this is where a loser that does not notice it
 * lost would drive the part's address counter anyway.
 */
static void test_nvram_read_survives_a_mid_byte_start(void)
{
    QTestState *qts = start_module();
    uint8_t mine;

    nvram_write(qts, 0, 0x010, 0x5a);
    nvram_write(qts, 0, 0x020, 0xa5);

    g_assert_true(start_transfer(qts, 0, NVRAM_FIRST, false));
    g_assert_true(send_byte(qts, 0, 0x10));
    g_assert_true(restart_transfer(qts, 0, NVRAM_FIRST, true));

    /*
     * Reading the data register clocks the byte out. Start the throwaway
     * byte, let adapter 1 address the part part way through it, then finish.
     */
    (void)adapter_read(qts, 0, I2C_DATA);
    qtest_clock_step(qts, 3 * 5556);
    adapter_write(qts, 1, I2C_CTRL, CTL_RUN);
    adapter_write(qts, 1, I2C_DATA, (NVRAM_FIRST << 1));
    adapter_write(qts, 1, I2C_CTRL, CTL_START);
    g_assert_true(wait_done(qts, 0));

    mine = take_byte(qts, 0, true);
    g_assert_cmphex(mine, ==, 0x5a);

    qtest_quit(qts);
}

static void test_initial_identity_on_every_node(void)
{
    QTestState *qts = start_module();

    for (unsigned node = 0; node < ADAPTER_COUNT; node++) {
        g_assert_cmphex(nvram_read(qts, node, NVRAM_VALID_MARKER), ==,
                        NVRAM_VALID_VALUE);
        g_assert_cmphex(nvram_read(qts, node, NVRAM_MODULE_NUMBER), ==,
                        INITIAL_MODULE_NUMBER);
        g_assert_cmphex(nvram_read(qts, node, NVRAM_PARTITION), ==, 0);
    }
    qtest_quit(qts);
}

/*
 * A new store holds what the Origin 2000 build of the controller writes at
 * its first power-up: the marker, the password "none", configuration byte 0
 * and the module number the module's model selects. Byte 0x70b, the
 * clock-select default the PROMs read as the domain, stays 0: whether this
 * module is the full-clock type that would write 0x78 is only inferred.
 */
static void test_new_store_contents(void)
{
    static const uint8_t block[16] = {
        NVRAM_VALID_VALUE, 'n', 'o', 'n', 'e', 0, 0, 0,
        INITIAL_MODULE_NUMBER, 0, 0, 0, 0, 0, 0, 0,
    };
    QTestState *qts = start_module();

    for (unsigned i = 0; i < sizeof(block); i++) {
        g_assert_cmphex(nvram_read(qts, i % ADAPTER_COUNT,
                                   NVRAM_IDENTITY_PAGE + i), ==, block[i]);
    }
    g_assert_cmphex(nvram_read(qts, 0, 0), ==, 0);
    qtest_quit(qts);
}

/* The controller's twelve-byte initialization lands in one transaction. */
static void test_twelve_byte_block_write(void)
{
    static const uint8_t block[12] = {
        0x37, 'w', 'x', 'y', 'z', 0x15, 0x16, 0x01, 0x02, 0x03, 0x04, 0x05,
    };
    QTestState *qts = start_module();

    nvram_write_block(qts, 0, NVRAM_IDENTITY_PAGE, block, sizeof(block));
    for (unsigned i = 0; i < sizeof(block); i++) {
        g_assert_cmphex(nvram_read(qts, 3, NVRAM_IDENTITY_PAGE + i), ==,
                        block[i]);
    }
    g_assert_cmphex(nvram_read(qts, 3, NVRAM_IDENTITY_PAGE + 12), ==, 0);
    qtest_quit(qts);
}

/*
 * A caller's image wins over the module number the product model selects,
 * and the bytes written reach the file and the next QEMU process.
 */
static void test_store_persists_across_processes(void)
{
    static const uint8_t record[] = {
        'H', 'U', 'B', ' ', 'n', '0', ' ', '\n', '\r', 0,
    };
    uint8_t image[NVRAM_BYTES] = { };
    g_autofree char *contents = NULL;
    gsize length;
    NvramStore store;
    QTestState *qts;

    image[NVRAM_VALID_MARKER] = NVRAM_VALID_VALUE;
    image[NVRAM_MODULE_NUMBER] = 5;
    nvram_store_create(&store, image);
    qts = start_module_with(store.option);
    for (unsigned node = 0; node < ADAPTER_COUNT; node++) {
        g_assert_cmphex(nvram_read(qts, node, NVRAM_MODULE_NUMBER), ==, 5);
    }
    nvram_write_block(qts, 1, 0, record, sizeof(record));
    nvram_write(qts, 2, NVRAM_PARTITION, 3);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(store.path, &contents, &length, NULL));
    g_assert_cmpuint(length, ==, NVRAM_BYTES);
    g_assert_cmpmem(contents, sizeof(record), record, sizeof(record));
    g_assert_cmphex((uint8_t)contents[NVRAM_PARTITION], ==, 3);

    qts = start_module_with(store.option);
    for (unsigned i = 0; i < sizeof(record); i++) {
        g_assert_cmphex(nvram_read(qts, i % ADAPTER_COUNT, i), ==, record[i]);
    }
    g_assert_cmphex(nvram_read(qts, 3, NVRAM_PARTITION), ==, 3);
    g_assert_cmphex(nvram_read(qts, 3, NVRAM_MODULE_NUMBER), ==, 5);
    qtest_quit(qts);
    nvram_store_destroy(&store);
}

static void test_identity_survives_reset(void)
{
    QTestState *qts = start_module();

    nvram_write(qts, 0, NVRAM_VALID_MARKER, 0x5a);
    nvram_write(qts, 1, NVRAM_MODULE_NUMBER, 7);
    nvram_write(qts, 2, NVRAM_PARTITION, 3);
    qtest_system_reset(qts);
    for (unsigned node = 0; node < ADAPTER_COUNT; node++) {
        init_adapter(qts, node);
        g_assert_cmphex(nvram_read(qts, node, NVRAM_VALID_MARKER), ==, 0x5a);
        g_assert_cmphex(nvram_read(qts, node, NVRAM_MODULE_NUMBER), ==, 7);
        g_assert_cmphex(nvram_read(qts, node, NVRAM_PARTITION), ==, 3);
    }
    qtest_quit(qts);
}

/* Whether QEMU has taken a power-down, under -no-shutdown. */
static bool powered_down(QTestState *qts)
{
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'query-status' }");
    bool down = g_str_equal(qdict_get_str(qdict_get_qdict(rsp, "return"),
                                          "status"), "shutdown");

    qobject_unref(rsp);
    return down;
}

/*
 * Send a command whose action resets the machine and read its "ok". The
 * action fires at the NAK that ends the read, so the adapter may be reset
 * before a helper could wait for that byte or close the transfer: stop at
 * the NAK and let the wire, which runs on the virtual clock, reach it.
 */
static void msc_command_then_reset(QTestState *qts, const char *text)
{
    uint8_t request[16], response[RESP_CAP], sum = 0;
    unsigned i, total, len = strlen(text);

    request[0] = len;
    memcpy(&request[1], text, len);
    for (i = 0; i < len + 1; i++) {
        sum += request[i];
    }
    request[len + 1] = (uint8_t)-(int)sum;

    g_assert_true(start_transfer(qts, 0, MSC_ADDRESS, false));
    for (i = 0; i < len + 2; i++) {
        g_assert_true(send_byte(qts, 0, request[i]));
    }
    g_assert_true(restart_transfer(qts, 0, MSC_ADDRESS, true));
    discard_byte(qts, 0);
    response[0] = take_byte(qts, 0, false);
    total = (unsigned)response[0] + 2;
    g_assert_cmpuint(total, <=, sizeof(response));
    for (i = 1; i < total - 1; i++) {
        response[i] = take_byte(qts, 0, false);
    }
    adapter_write(qts, 0, I2C_CTRL, CTL_NAK);
    response[total - 1] = adapter_read(qts, 0, I2C_DATA);
    g_assert_cmpmem(&response[1], 2, "ok", 2);
    for (i = 0; i < POLL_STEPS; i++) {
        qtest_clock_step(qts, POLL_STEP_NS);
    }
    /* A "rst" waits a second before the reset; "pwr c" does not. */
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    qtest_qmp_eventwait(qts, "RESET");
}

/*
 * The module controller's reset and power outputs reach the machine. IRIX
 * powers an Origin 2000 off with "pwr d 1" to every module controller, so
 * a refused power-down left the guest spinning at halt.
 */
static void test_module_reset_reaches_machine(void)
{
    QTestState *qts = start_module();

    msc_command_then_reset(qts, "rst");
    qtest_quit(qts);
}

static void test_module_power_down_reaches_machine(void)
{
    QTestState *qts = start_module_with(" -no-shutdown");

    assert_response(qts, 0, "pwr d 1", "ok");
    g_assert_false(powered_down(qts));
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    qtest_qmp_eventwait(qts, "SHUTDOWN");
    qtest_quit(qts);
}

/* "pwr c" removes power and restores it: a cold restart, not a power-off. */
static void test_module_power_cycle_reaches_machine(void)
{
    QTestState *qts = start_module_with(" -no-shutdown");

    msc_command_then_reset(qts, "pwr c");
    g_assert_false(powered_down(qts));
    qtest_quit(qts);
}

/*
 * The Origin 2000 build drives the module's eight-character display. A
 * "dsc" position is the digit 1 to 8 for display positions 0 to 7, which
 * is what the PROM sends as 2s-1 and 2s for slot s, and the character is
 * printable and never a space. "dsp" overwrites from the first position,
 * stops at eight and leaves the rest alone. The display is not readable
 * over the bus, so content is read through "info elsc".
 */
static void test_origin2000_display(void)
{
    QTestState *qts = start_module();
    const char *refused[] = { "dsc 0 X", "dsc 9 X", "dsc 4  ", "dsc", "dsp" };
    g_autofree char *after_dsc = NULL;
    g_autofree char *after_dsp = NULL;
    g_autofree char *after_long = NULL;
    g_autofree char *after_spaced = NULL;
    g_autofree char *after_stop = NULL;
    unsigned i;

    assert_response(qts, 0, "dsc 1 A", "ok");
    assert_response(qts, 0, "dsc 8 Z", "ok");
    after_dsc = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(after_dsc, "display: \"A      Z\""));

    for (i = 0; i < ARRAY_SIZE(refused); i++) {
        assert_response(qts, 0, refused[i], "err arg");
    }

    assert_response(qts, 0, "dsp testing", "ok");
    after_dsp = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(after_dsp, "display: \"testingZ\""));

    assert_response(qts, 0, "dsp ninechars", "ok");
    after_long = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(after_long, "display: \"ninechar\""));

    /* Spaces before each argument are skipped. */
    assert_response(qts, 0, "dsc  2   Q", "ok");
    after_spaced = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(after_spaced, "display: \"nQnechar\""));

    /*
     * The message's own leading spaces are characters, and it stops at
     * the first unprintable one, leaving the rest of the display.
     */
    assert_response(qts, 0, "dsp  ab\x01zz", "ok");
    after_stop = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(after_stop, "display: \" abechar\""));

    assert_response(qts, 0, "ver", "ok VER 3.2 ");
    qtest_quit(qts);
}

/* A whole reply, not only its prefix. */
static void assert_exact(QTestState *qts, unsigned node, const char *text,
                         const char *expected)
{
    uint8_t response[RESP_CAP];

    msc_command(qts, node, text, response, sizeof(response));
    g_assert_cmpuint(response[0], ==, strlen(expected));
    g_assert_cmpmem(&response[1], strlen(expected), expected,
                    strlen(expected));
}

/*
 * The Origin 2000 build's own commands: a key in the on position, and
 * margins that are stored and answered. "aut" belongs to the Origin 200
 * build, and "sla" is in this build's table with no handler. "clr" blanks
 * the display.
 */
static void test_origin2000_build_commands(void)
{
    QTestState *qts = start_module();
    g_autofree char *after_clr = NULL;

    assert_exact(qts, 0, "key", "ok on");
    assert_exact(qts, 1, "vlm 5", "ok n");
    assert_exact(qts, 1, "vlm 5h", "ok ");
    assert_exact(qts, 2, "vlm 5", "ok h");
    assert_exact(qts, 0, "vlm 3 l", "ok ");
    assert_exact(qts, 0, "vlm 3", "ok l");
    assert_exact(qts, 0, "vlm v", "ok n");
    assert_exact(qts, 0, "vlm", "err arg");
    assert_exact(qts, 0, "vlm x", "err arg");
    assert_exact(qts, 0, "vlm 5 q", "err arg");
    assert_exact(qts, 0, "aut", "err cmd");
    assert_exact(qts, 0, "mod", "err cmd");
    assert_exact(qts, 0, "pas none", "err cmd");
    assert_exact(qts, 0, "sla o", "ok ");
    assert_exact(qts, 0, "tmp", "ok n");

    assert_exact(qts, 0, "dsp abc", "ok ");
    assert_exact(qts, 0, "clr", "ok ");
    after_clr = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(after_clr, "display: \"        \""));
    assert_exact(qts, 3, "vlm 5", "ok h");
    qtest_quit(qts);
}

/*
 * A module controller's message is announced in every processor code's
 * grant, and a processor that fetches it stops seeing it.
 */
static void test_module_message_reaches_every_code(void)
{
    QTestState *qts = start_module_with(" -no-shutdown");

    assert_exact(qts, 0, "pwr d 8", "ok ");
    /* A whole eight-code ring is about eight milliseconds. */
    wait_for_global_grant_within(qts, GRANT_BASE | 0x08 | 3, 20 * POLL_STEPS);
    assert_exact(qts, 0, "get", "pd remote");
    assert_exact(qts, 0, "get", "acp . ");
    wait_for_global_grant_within(qts, GRANT_BASE | 0x08 | 5, 20 * POLL_STEPS);
    wait_for_global_grant_within(qts, GRANT_BASE | 0, 20 * POLL_STEPS);
    assert_exact(qts, 0, "clr", "ok ");
    qtest_quit(qts);
}

/*
 * A controller message the module announces lasts its second there and
 * in the controller alike.
 */
static void test_module_message_expires(void)
{
    QTestState *qts = start_module();
    g_autofree char *posted = NULL;
    g_autofree char *before = NULL;
    g_autofree char *after = NULL;

    assert_exact(qts, 0, "pwr d 30", "ok ");
    posted = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(posted, "controller message: \"pd remote\""));
    qtest_clock_step(qts, 900 * (int64_t)SCALE_MS);
    before = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(before, "controller message: \"pd remote\""));
    qtest_clock_step(qts, 100 * (int64_t)SCALE_MS);
    after = qtest_hmp(qts, "info elsc");
    g_assert_null(strstr(after, "controller message"));
    qtest_quit(qts);
}

/* The module controller's "nmi" pulses the module's NMI output. */
static void test_module_nmi_reaches_output(void)
{
    QTestState *qts = start_module();

    qtest_irq_intercept_out_named(qts, "/machine/module-management", "nmi");
    assert_exact(qts, 2, "nmi", "ok ");
    qtest_clock_step(qts, 990 * (int64_t)SCALE_MS);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 0), ==, 0);
    qtest_clock_step(qts, 20 * (int64_t)SCALE_MS);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 0), ==, 1);
    qtest_clock_step(qts, 100 * (int64_t)SCALE_MS);
    g_assert_false(qtest_get_irq(qts, 0));
    assert_exact(qts, 0, "ver", "ok VER 3.2 ");
    qtest_quit(qts);
}

/* The module's MODULEID part, letter Y at index 0. */
static const uint8_t module_rom[8] = {
    0x09, 0x00, 0x59, 0x55, 0x4d, 0x45, 0x51, 0x33,
};

/* Its 8P12_MPLN midplane part, letter L at index 0. */
static const uint8_t midplane_rom[8] = {
    0x0b, 0x00, 0x4c, 0x55, 0x4d, 0x45, 0x51, 0xe7,
};

static const uint8_t module_pages[64] = {
    0x01, 0x4b, 0x4f, 0x52, 0x49, 0x47, 0x41, 0x4d,
    0x49, 0x30, 0x31, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf9, 0x27,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x4d, 0x4f, 0x44, 0x55, 0x4c, 0x45, 0x49, 0x44,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x4b, 0x59,
};

static const uint8_t midplane_pages[64] = {
    0x01, 0x45, 0x4d, 0x55, 0x4c, 0x30, 0x30, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x30, 0x33, 0x30,
    0x2d, 0x30, 0x37, 0x36, 0x32, 0x2d, 0xd8, 0x16,
    0x20, 0x20, 0x20, 0x30, 0x30, 0x36, 0x48, 0x20,
    0x20, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x38, 0x50, 0x31, 0x32, 0x5f, 0x4d, 0x50, 0x4c,
    0x4e, 0x20, 0x20, 0x20, 0x20, 0x20, 0x78, 0x3e,
};

/* Every ROM on the controller's midplane line, lowest first. */
static unsigned search_all(QTestState *qts, unsigned node,
                           uint8_t roms[][8], unsigned max)
{
    uint8_t rom[8] = { 0 };
    int last_conflict = -1;
    unsigned found = 0;

    do {
        int conflict = -1;

        g_assert_true(onewire_reset(qts, node));
        onewire_write_byte(qts, node, 0xf0);
        for (int i = 0; i < 64; i++) {
            bool bit = onewire_read_bit(qts, node);
            bool complement = onewire_read_bit(qts, node);
            bool direction;

            g_assert_false(bit && complement);
            if (bit != complement) {
                direction = bit;
            } else {
                direction = i < last_conflict ?
                            (rom[i / 8] >> (i % 8)) & 1 : i == last_conflict;
                if (!direction) {
                    conflict = i;
                }
            }
            rom[i / 8] = (rom[i / 8] & ~(1u << (i % 8))) |
                         (direction << (i % 8));
            onewire_write_bit(qts, node, direction);
        }
        g_assert_cmpuint(found, <, max);
        memcpy(roms[found++], rom, sizeof(rom));
        last_conflict = conflict;
    } while (last_conflict >= 0);
    return found;
}

static void assert_module_parts(QTestState *qts, unsigned node)
{
    uint8_t roms[3][8];

    g_assert_cmpuint(search_all(qts, node, roms, 3), ==, 2);
    g_assert_cmpmem(roms[0], 8, module_rom, sizeof(module_rom));
    g_assert_cmpmem(roms[1], 8, midplane_rom, sizeof(midplane_rom));
}

/* Both parts still answer to their ROMs, read by the cheaper match. */
static void assert_parts_answer(QTestState *qts, unsigned node)
{
    onewire_start_read(qts, node, module_rom, 0xf0, 0);
    g_assert_cmphex(onewire_read_byte(qts, node), ==, 0x8d);
    g_assert_cmphex(onewire_read_byte(qts, node), ==, module_pages[0]);
    onewire_start_read(qts, node, midplane_rom, 0xf0, 0);
    g_assert_cmphex(onewire_read_byte(qts, node), ==, midplane_pages[0]);
}

/*
 * The controller's nic line drives the module's midplane net, which carries
 * the module's MODULEID part and its 8P12_MPLN midplane part, the same two
 * the n1 node's Hub bus reaches, and nothing else: the module-management
 * chip has no part of its own on it. Each adapter reaches the one controller.
 */
static void test_module_parts_through_nic(void)
{
    QTestState *qts = start_module();

    assert_module_parts(qts, 0);
    for (unsigned node = 1; node < ADAPTER_COUNT; node++) {
        onewire_start_read(qts, node, midplane_rom, 0xf0, 0);
        g_assert_cmphex(onewire_read_byte(qts, node), ==, midplane_pages[0]);
    }
    qtest_quit(qts);
}

/*
 * Keep the machine's Hub relay and both identity pages covered here.
 * test-onewire covers search/match and DS2502 stream CRC and status.
 */
static void test_module_serial_memory_pages(void)
{
    QTestState *qts = start_module();
    uint8_t bytes[sizeof(module_pages)];

    onewire_start_read(qts, 1, module_rom, 0xf0, 0);
    g_assert_cmphex(onewire_read_byte(qts, 1), ==, 0x8d);
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        bytes[i] = onewire_read_byte(qts, 1);
    }
    g_assert_cmpmem(bytes, sizeof(bytes), module_pages,
                    sizeof(module_pages));

    /* The midplane part's Read Memory carries no header CRC. */
    onewire_start_read(qts, 2, midplane_rom, 0xf0, 0);
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        bytes[i] = onewire_read_byte(qts, 2);
    }
    g_assert_cmpmem(bytes, sizeof(bytes), midplane_pages,
                    sizeof(midplane_pages));
    qtest_quit(qts);
}

/*
 * A prior fixture (origin2000-model-test, MACHINE_TYPE_NAME
 * "origin2000-model-test") drove this same read pair against a module whose
 * serial its module-serial= property could override, confirming the read
 * follows the descriptor's serial rather than a hardcoded one. That fixture
 * deep-copied the deleted per-product descriptor (SN0ModelComposition) to
 * make the override; format 2 carries no settable copy for a test to reach
 * into. The mechanism it checked is still generic:
 * sgi_sn0_module_management_identity_pages in
 * hw/misc/sgi-sn0-module-management.c derives both one-wire pages from the
 * module's own serial field, not a literal. The fixture also
 * test_module_serial_memory_pages above reads both pages through the Hub
 * relay for the shipped module; only the check that a second serial value
 * also round-trips is not ported.
 */

static void test_module_serial_reset_preserves_identity(void)
{
    QTestState *qts = start_module();

    onewire_select(qts, 3, module_rom);
    onewire_write_byte(qts, 3, 0xf0);
    qtest_system_reset(qts);
    for (unsigned node = 0; node < ADAPTER_COUNT; node++) {
        init_adapter(qts, node);
    }
    assert_parts_answer(qts, 3);
    qtest_quit(qts);
}

/* The module number is NVRAM state; no part's ROM serial follows it. */
static void test_module_serial_ignores_mutable_module_number(void)
{
    QTestState *qts = start_module();

    nvram_write(qts, 1, NVRAM_MODULE_NUMBER, 7);
    g_assert_cmphex(nvram_read(qts, 2, NVRAM_MODULE_NUMBER), ==, 7);
    assert_parts_answer(qts, 3);
    qtest_quit(qts);
}

static bool is_running(QTestState *qts)
{
    g_autoptr(QDict) response = qtest_qmp(qts, "{'execute':'query-status'}");

    return qdict_get_bool(qdict_get_qdict(response, "return"), "running");
}

/*
 * A completed migration on the source does not mean the destination has
 * started. Its virtual clock stays stopped until it does, and a stopped clock
 * runs no timers, so the shared wire would never clock a bit.
 */
static void wait_until_running(QTestState *qts)
{
    SgiWait wait;

    sgi_wait_start(&wait, "migration destination running");
    while (!is_running(qts)) {
        sgi_wait_pause(&wait, 1000);
    }
}

/*
 * The module's DIP switch bank belongs to the whole module, and the machine
 * setting must reach the one controller behind the four windows. The
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

    g_assert_nonnull(directory);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(from, "completed");
    /* The source completes when its state is sent, not when it is loaded. */
    sgi_wait_migration(to, "completed");
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

/*
 * The complex sits on no bus and carries its own migration state. What
 * survives is the shared store and a controller that still talks, on a
 * destination whose adapters were never programmed by a guest. The qtest
 * accelerator's virtual clock is the test's own counter and does not travel
 * with the migration stream, so the destination starts at zero holding the
 * source's absolute deadlines: the scheduler's next tick, and the bus-free
 * time each adapter must see before a fresh START. Put the destination clock
 * where the source left it, and everything the stream restored is due rather
 * than far in the future.
 */
/*
 * A destination that loads a service deadline older than its own clock must
 * still publish. Whatever the controller does about the periods that went by
 * while the state was in flight, the grants resume and the code keeps
 * advancing by one, so a processor's turn still comes round.
 */
static void test_late_grant_service(void)
{
    QTestState *from = start_module();
    QTestState *to = qtest_init(MACHINE_ARGS " -incoming defer");
    int64_t departed;
    uint8_t first, second;

    for (unsigned processor = 0; processor <= 3; processor++) {
        wait_for_global_grant(from, GRANT_BASE | processor);
    }
    departed = qtest_clock_step(from, 1);

    /* Loading an old absolute deadline makes its first delivery late. */
    qtest_clock_set(to, departed + INT64_C(10) * 1000 * SERVICE_PERIOD_NS);
    migrate_module(from, to);
    wait_until_running(to);

    first = wait_for_next_global_grant(to, 0);
    second = wait_for_next_global_grant(to, first);
    g_assert_cmphex(second, ==, GRANT_BASE | ((first + 1) & 7));
    qtest_quit(from);
    qtest_quit(to);
}

static void test_migration(void)
{
    QTestState *from = start_module();
    QTestState *to = qtest_init(MACHINE_ARGS " -incoming defer");
    int64_t departed;

    nvram_write(from, 0, NVRAM_VALID_MARKER, 0x5a);
    nvram_write(from, 1, NVRAM_MODULE_NUMBER, 7);
    nvram_write(from, 2, NVRAM_PARTITION, 3);
    nvram_write(from, 2, 0x321, 0xa5);
    /* A zero step is refused, so read the clock by taking the smallest. */
    departed = qtest_clock_step(from, 1);
    migrate_module(from, to);
    wait_until_running(to);
    qtest_clock_set(to, departed);
    for (unsigned node = 0; node < ADAPTER_COUNT; node++) {
        g_assert_cmphex(nvram_read(to, node, NVRAM_VALID_MARKER), ==, 0x5a);
        g_assert_cmphex(nvram_read(to, node, NVRAM_MODULE_NUMBER), ==, 7);
        g_assert_cmphex(nvram_read(to, node, NVRAM_PARTITION), ==, 3);
        g_assert_cmphex(nvram_read(to, node, 0x321), ==, 0xa5);
    }
    assert_response(to, 3, "ver", "ok");
    qtest_quit(from);
    qtest_quit(to);
}

static void test_module_serial_migration_in_progress(void)
{
    QTestState *from = start_module();
    QTestState *to = qtest_init(MACHINE_ARGS " -incoming defer");
    int64_t departed;

    onewire_start_read(from, 0, module_rom, 0xf0, 0);
    g_assert_cmphex(onewire_read_byte(from, 0), ==, 0x8d);
    for (unsigned i = 0; i < 7; i++) {
        g_assert_cmphex(onewire_read_byte(from, 0), ==, module_pages[i]);
    }
    departed = qtest_clock_step(from, 1);
    migrate_module(from, to);
    wait_until_running(to);
    qtest_clock_set(to, departed);
    for (unsigned i = 7; i < sizeof(module_pages); i++) {
        g_assert_cmphex(onewire_read_byte(to, 0), ==, module_pages[i]);
    }
    qtest_quit(from);
    qtest_quit(to);
}

/*
 * The state a released START leaves behind when a STOP was written against it
 * must be one the destination accepts, and must carry on there as it would
 * have at home.
 */
static void test_withdrawn_start_migration(void)
{
    QTestState *from = start_module();
    QTestState *to = qtest_init(MACHINE_ARGS " -incoming defer");
    int64_t departed;

    stop_against_a_released_start(from);
    departed = qtest_clock_step(from, 1);
    migrate_module(from, to);
    wait_until_running(to);
    qtest_clock_set(to, departed);
    assert_withdrawn_start_stays_withdrawn(to);
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
    qtest_add_func("/sn0/o2000-module-management/late-start-reads-module",
                   test_late_start_still_reads_the_module_number);
    qtest_add_func("/sn0/o2000-module-management/global-grant-rotation",
                   test_global_grant_rotation);
    qtest_add_func("/sn0/o2000-module-management/read-grant-keeps-rotating",
                   test_read_grant_keeps_rotating);
    qtest_add_func("/sn0/o2000-module-management/invalid-address-stop",
                   test_invalid_address_stop_releases_wire);
    qtest_add_func("/sn0/o2000-module-management/mid-byte-stop",
                   test_mid_byte_stop_releases_wire);
    qtest_add_func("/sn0/o2000-module-management/stop-after-acked-read-byte",
                   test_stop_after_acked_read_byte_recovers);
    qtest_add_func("/sn0/o2000-module-management/held-stop-after-byte",
                   test_held_stop_follows_the_finished_byte);
    qtest_add_func("/sn0/o2000-module-management/lost-arbitration-held-stop",
                   test_lost_arbitration_drops_a_held_stop);
    qtest_add_func("/sn0/o2000-module-management/stop-withdraws-released-start",
                   test_stop_withdraws_a_released_start);
    qtest_add_func("/sn0/o2000-module-management/shared-nvram",
                   test_nvram_is_shared);
    qtest_add_func("/sn0/o2000-module-management/new-nvram-store",
                   test_new_store_contents);
    qtest_add_func("/sn0/o2000-module-management/nvram-twelve-byte-write",
                   test_twelve_byte_block_write);
    qtest_add_func("/sn0/o2000-module-management/nvram-store-persists",
                   test_store_persists_across_processes);
    qtest_add_func("/sn0/o2000-module-management/initial-identity",
                   test_initial_identity_on_every_node);
    qtest_add_func("/sn0/o2000-module-management/identity-reset",
                   test_identity_survives_reset);
    qtest_add_func("/sn0/o2000-module-management/origin2000-display",
                   test_origin2000_display);
    qtest_add_func("/sn0/o2000-module-management/origin2000-build-commands",
                   test_origin2000_build_commands);
    qtest_add_func("/sn0/o2000-module-management/message-every-code",
                   test_module_message_reaches_every_code);
    qtest_add_func("/sn0/o2000-module-management/message-expires",
                   test_module_message_expires);
    qtest_add_func("/sn0/o2000-module-management/nmi-reaches-output",
                   test_module_nmi_reaches_output);
    qtest_add_func("/sn0/o2000-module-management/module-reset",
                   test_module_reset_reaches_machine);
    qtest_add_func("/sn0/o2000-module-management/module-power-down",
                   test_module_power_down_reaches_machine);
    qtest_add_func("/sn0/o2000-module-management/module-power-cycle",
                   test_module_power_cycle_reaches_machine);
    qtest_add_func("/sn0/o2000-module-management/dip-switches",
                   test_dip_switches_reach_the_controller);
    qtest_add_func("/sn0/o2000-module-management/module-parts-through-nic",
                   test_module_parts_through_nic);
    qtest_add_func("/sn0/o2000-module-management/module-serial-memory",
                   test_module_serial_memory_pages);
    qtest_add_func("/sn0/o2000-module-management/module-serial-reset",
                   test_module_serial_reset_preserves_identity);
    qtest_add_func("/sn0/o2000-module-management/module-serial-module-number",
                   test_module_serial_ignores_mutable_module_number);
    qtest_add_func("/sn0/o2000-module-management/nvram-read-vs-mid-byte-start",
                   test_nvram_read_survives_a_mid_byte_start);
    qtest_add_func("/sn0/o2000-module-management/nvram-read-vs-second-adapter",
                   test_nvram_read_survives_a_second_adapter);
    qtest_add_func("/sn0/o2000-module-management/migration", test_migration);
    qtest_add_func("/sn0/o2000-module-management/late-grant-service",
                   test_late_grant_service);
    qtest_add_func("/sn0/o2000-module-management/module-serial-migration",
                   test_module_serial_migration_in_progress);
    qtest_add_func("/sn0/o2000-module-management/withdrawn-start-migration",
                   test_withdrawn_start_migration);
    return g_test_run();
}
