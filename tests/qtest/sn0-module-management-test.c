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
 * Arbitration is decided in the first microseconds after an action, so
 * poll finely there and coarsely afterwards. A byte is about 100 us and
 * the grant rotation about 10 ms, so a uniform fine step spends fifty
 * round trips on every byte and still cannot outlast a grant.
 */
#define POLL_FINE_STEPS 8
#define POLL_COARSE_NS  50000
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
        qtest_clock_step(qts, i < POLL_FINE_STEPS ? POLL_STEP_NS
                                                  : POLL_COARSE_NS);
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

static bool hub_onewire_slot(QTestState *qts, unsigned pulse)
{
    uint64_t value;

    qtest_writeq(qts, HUB_BASE + MD_ONEWIRE,
                 HUB_ONEWIRE_RESET | ((uint64_t)pulse << 10) |
                 (0x04ULL << 2));
    qtest_clock_step(qts, (pulse + 4) * 1000);
    value = qtest_readq(qts, HUB_BASE + MD_ONEWIRE);
    g_assert_cmphex(value & HUB_ONEWIRE_DONE, ==, HUB_ONEWIRE_DONE);
    return value & HUB_ONEWIRE_RXDATA;
}

static void hub_onewire_write_byte(QTestState *qts, uint8_t value)
{
    for (unsigned bit = 0; bit < 8; bit++) {
        hub_onewire_slot(qts, value & (1u << bit) ?
                             HUB_ONEWIRE_SHORT : HUB_ONEWIRE_ZERO);
    }
}

static uint8_t hub_onewire_read_byte(QTestState *qts)
{
    uint8_t value = 0;

    for (unsigned bit = 0; bit < 8; bit++) {
        value |= hub_onewire_slot(qts, HUB_ONEWIRE_SHORT) << bit;
    }
    return value;
}

static void hub_onewire_select_module(QTestState *qts)
{
    static const uint8_t rom[8] = {
        0x09, 0x09, 0x00, 0x00, 0x00, 0x00, 0x10, 0xc7,
    };

    g_assert_false(hub_onewire_slot(qts, 0x321));
    hub_onewire_write_byte(qts, 0x55);
    for (unsigned int i = 0; i < sizeof(rom); i++) {
        hub_onewire_write_byte(qts, rom[i]);
    }
}

static void onewire_select(QTestState *qts, unsigned node)
{
    g_assert_true(onewire_reset(qts, node));
    onewire_write_byte(qts, node, 0xcc);
}

static void onewire_start_read(QTestState *qts, unsigned node, uint8_t command,
                               uint16_t address)
{
    onewire_select(qts, node);
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

/* A START issued under a newer grant loses when that grant stops. */
static void test_start_loses_to_grant(void)
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
                    (STS_LRB | STS_BER | STS_LAB), ==,
                    STS_LRB | STS_LAB);
    stop_transfer(qts, 0);
    qtest_quit(qts);
}

static void wait_for_global_grant(QTestState *qts, uint8_t expected)
{
    for (unsigned step = 0; step < 2 * POLL_STEPS; step++) {
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
 * The grant ring covers all eight processors through four adapters. Keep the
 * two middle Hubs between the endpoint controls, issue a real controller
 * transaction in every slot, and require the next grant to be consecutive.
 * A retry here would conceal the skipped-slot failure this test is for.
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

/* A refused closing address must still accept the following full-byte STOP. */
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

    /* This is the exact fresh control byte, not bits merged with old state. */
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
 * nodes at once, and a moved counter returns whichever byte the other
 * adapter selected instead.
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
 * The same question one level finer: a second adapter starting while the
 * first is part way through a byte, rather than between transfers. The wire
 * resolves arbitration edge by edge, so this is where a loser that does not
 * notice it lost would drive the part's address counter anyway.
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

static const uint8_t module_rom[8] = {
    0x09, 0x4f, 0x52, 0x49, 0x47, 0x41, 0x4d, 0x57,
};

static const uint8_t module_pages[64] = {
    0x01, 0x4b, 0x4f, 0x52, 0x49, 0x47, 0x41, 0x4d,
    0x49, 0x30, 0x31, 0x4f, 0x52, 0x49, 0x47, 0x41,
    0x4d, 0x49, 0x2d, 0x4d, 0x4f, 0x44, 0x55, 0x4c,
    0x45, 0x2d, 0x53, 0x45, 0x52, 0x49, 0x80, 0x45,
    0x41, 0x4c, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x4d, 0x4f, 0x44, 0x55, 0x4c, 0x45, 0x49, 0x44,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0e, 0xcc,
};

static void read_rom(QTestState *qts, unsigned node, uint8_t rom[8])
{
    g_assert_true(onewire_reset(qts, node));
    onewire_write_byte(qts, node, 0x33);
    for (unsigned i = 0; i < 8; i++) {
        rom[i] = onewire_read_byte(qts, node);
    }
}

static void test_module_serial_rom_on_every_node(void)
{
    QTestState *qts = start_module();

    for (unsigned node = 0; node < ADAPTER_COUNT; node++) {
        uint8_t rom[8];

        read_rom(qts, node, rom);
        g_assert_cmpmem(rom, sizeof(rom), module_rom, sizeof(module_rom));
    }
    qtest_quit(qts);
}

static void test_module_serial_search_and_match(void)
{
    QTestState *qts = start_module();
    uint8_t found[8] = { 0 };

    g_assert_true(onewire_reset(qts, 0));
    onewire_write_byte(qts, 0, 0xf0);
    for (unsigned bit = 0; bit < 64; bit++) {
        bool value = onewire_read_bit(qts, 0);
        bool complement = onewire_read_bit(qts, 0);

        g_assert_cmpint(value, !=, complement);
        found[bit / 8] |= value << (bit % 8);
        onewire_write_bit(qts, 0, value);
    }
    g_assert_cmpmem(found, sizeof(found), module_rom, sizeof(module_rom));

    g_assert_true(onewire_reset(qts, 0));
    onewire_write_byte(qts, 0, 0x55);
    for (unsigned i = 0; i < sizeof(module_rom); i++) {
        onewire_write_byte(qts, 0, module_rom[i] ^ (i == 0));
    }
    onewire_write_byte(qts, 0, 0xf0);
    g_assert_cmphex(onewire_read_byte(qts, 0), ==, 0xff);
    qtest_quit(qts);
}

static void test_module_serial_memory_and_crc(void)
{
    QTestState *qts = start_module();
    uint8_t bytes[128];

    onewire_start_read(qts, 1, 0xf0, 0);
    g_assert_cmphex(onewire_read_byte(qts, 1), ==, 0x8d);
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        bytes[i] = onewire_read_byte(qts, 1);
    }
    g_assert_cmpmem(bytes, sizeof(module_pages), module_pages,
                    sizeof(module_pages));
    for (unsigned i = sizeof(module_pages); i < sizeof(bytes); i++) {
        g_assert_cmphex(bytes[i], ==, 0xff);
    }
    g_assert_cmphex(onewire_read_byte(qts, 1), ==, 0xf6);

    onewire_start_read(qts, 1, 0xf0, 0x20);
    g_assert_cmphex(onewire_read_byte(qts, 1), ==, 0x4c);
    for (unsigned i = 0; i < 32; i++) {
        g_assert_cmphex(onewire_read_byte(qts, 1), ==, module_pages[32 + i]);
    }
    qtest_quit(qts);
}

static void test_module_serial_generated_crc_and_status(void)
{
    static const uint8_t status[8] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
    };
    QTestState *qts = start_module();

    onewire_start_read(qts, 2, 0xc3, 0);
    g_assert_cmphex(onewire_read_byte(qts, 2), ==, 0xb7);
    for (unsigned i = 0; i < 32; i++) {
        g_assert_cmphex(onewire_read_byte(qts, 2), ==, module_pages[i]);
    }
    g_assert_cmphex(onewire_read_byte(qts, 2), ==, 0xa6);
    for (unsigned i = 32; i < 64; i++) {
        g_assert_cmphex(onewire_read_byte(qts, 2), ==, module_pages[i]);
    }
    g_assert_cmphex(onewire_read_byte(qts, 2), ==, 0x4d);

    onewire_start_read(qts, 2, 0xaa, 0);
    g_assert_cmphex(onewire_read_byte(qts, 2), ==, 0x9c);
    for (unsigned i = 0; i < sizeof(status); i++) {
        g_assert_cmphex(onewire_read_byte(qts, 2), ==, status[i]);
    }
    g_assert_cmphex(onewire_read_byte(qts, 2), ==, 0xfc);
    qtest_quit(qts);
}

static void test_module_serial_follows_generated_model(void)
{
    static const char *serials[] = { "KORIGAMI01", "TESTMODEL2" };

    for (unsigned int serial_index = 0;
         serial_index < G_N_ELEMENTS(serials); serial_index++) {
        const char *serial = serials[serial_index];
        g_autofree char *extra = g_strdup_printf(
            " -M origin2000-model-test,module-serial=%s", serial);
        QTestState *qts = start_module_with(extra);
        uint8_t controller[1 + MODULE_SERIAL_LEN];
        uint8_t hub[1 + MODULE_SERIAL_LEN];

        onewire_start_read(qts, 0, 0xc3, 0);
        (void)onewire_read_byte(qts, 0);
        for (unsigned int i = 0; i < sizeof(controller); i++) {
            controller[i] = onewire_read_byte(qts, 0);
        }

        hub_onewire_select_module(qts);
        hub_onewire_write_byte(qts, 0xc3);
        hub_onewire_write_byte(qts, 0);
        hub_onewire_write_byte(qts, 0);
        (void)hub_onewire_read_byte(qts);
        for (unsigned int i = 0; i < sizeof(hub); i++) {
            hub[i] = hub_onewire_read_byte(qts);
        }

        g_assert_cmphex(controller[0], ==, 0x01);
        g_assert_cmpmem(&controller[1], MODULE_SERIAL_LEN,
                        serial, MODULE_SERIAL_LEN);
        g_assert_cmpmem(controller, sizeof(controller), hub, sizeof(hub));
        qtest_quit(qts);
    }
}

static void test_module_serial_reset_preserves_identity(void)
{
    QTestState *qts = start_module();
    uint8_t rom[8];

    onewire_select(qts, 3);
    onewire_write_byte(qts, 3, 0xf0);
    qtest_system_reset(qts);
    for (unsigned node = 0; node < ADAPTER_COUNT; node++) {
        init_adapter(qts, node);
    }
    read_rom(qts, 3, rom);
    g_assert_cmpmem(rom, sizeof(rom), module_rom, sizeof(module_rom));
    qtest_quit(qts);
}

static void test_module_serial_ignores_mutable_module_number(void)
{
    QTestState *qts = start_module();
    uint8_t before[8], after[8];

    read_rom(qts, 0, before);
    nvram_write(qts, 1, NVRAM_MODULE_NUMBER, 7);
    g_assert_cmphex(nvram_read(qts, 2, NVRAM_MODULE_NUMBER), ==, 7);
    read_rom(qts, 3, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
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

static void wait_for_migration(QTestState *qts)
{
    int64_t deadline = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;

    for (;;) {
        g_autoptr(QDict) response = qtest_qmp(qts,
                                              "{'execute':'query-migrate'}");
        QDict *result = qdict_get_qdict(response, "return");
        const char *status = qdict_get_str(result, "status");

        if (!strcmp(status, "completed")) {
            return;
        }
        g_assert_cmpstr(status, !=, "failed");
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
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
    wait_for_migration(from);
    /* The source completes when its state is sent, not when it is loaded. */
    wait_for_migration(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

/* A delayed service tick publishes once and resumes its period from now. */
static void test_late_grant_service(void)
{
    QTestState *from = start_module();
    QTestState *to = qtest_init(MACHINE_ARGS " -incoming defer");
    int64_t departed;

    for (unsigned processor = 0; processor <= 3; processor++) {
        wait_for_global_grant(from, GRANT_BASE | processor);
    }
    departed = qtest_clock_step(from, 1);

    /* Loading an old absolute deadline makes its first delivery late. */
    qtest_clock_set(to, departed + INT64_C(10) * 1000 * SERVICE_PERIOD_NS);
    migrate_module(from, to);
    wait_until_running(to);

    wait_for_global_grant(to, GRANT_BASE | 4);
    qtest_quit(from);
    qtest_quit(to);
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

    onewire_start_read(from, 0, 0xf0, 0);
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
    qtest_add_func("/sn0/o2000-module-management/start-loses-to-grant",
                   test_start_loses_to_grant);
    qtest_add_func("/sn0/o2000-module-management/global-grant-rotation",
                   test_global_grant_rotation);
    qtest_add_func("/sn0/o2000-module-management/invalid-address-stop",
                   test_invalid_address_stop_releases_wire);
    qtest_add_func("/sn0/o2000-module-management/shared-nvram",
                   test_nvram_is_shared);
    qtest_add_func("/sn0/o2000-module-management/initial-identity",
                   test_initial_identity_on_every_node);
    qtest_add_func("/sn0/o2000-module-management/identity-reset",
                   test_identity_survives_reset);
    qtest_add_func("/sn0/o2000-module-management/destructive-refused",
                   test_destructive_commands_refused);
    qtest_add_func("/sn0/o2000-module-management/dip-switches",
                   test_dip_switches_reach_the_controller);
    qtest_add_func("/sn0/o2000-module-management/module-serial-rom",
                   test_module_serial_rom_on_every_node);
    qtest_add_func("/sn0/o2000-module-management/module-serial-search",
                   test_module_serial_search_and_match);
    qtest_add_func("/sn0/o2000-module-management/module-serial-memory",
                   test_module_serial_memory_and_crc);
    qtest_add_func("/sn0/o2000-module-management/module-serial-crc-status",
                   test_module_serial_generated_crc_and_status);
    qtest_add_func("/sn0/o2000-module-management/module-serial-model",
                   test_module_serial_follows_generated_model);
    qtest_add_func("/sn0/o2000-module-management/module-serial-reset",
                   test_module_serial_reset_preserves_identity);
    qtest_add_func("/sn0/o2000-module-management/module-serial-module-number",
                   test_module_serial_ignores_mutable_module_number);
    qtest_add_func("/sn0/o2000-module-management/nvram-read-vs-mid-byte-start",
                   test_nvram_read_survives_a_mid_byte_start);
    qtest_add_func("/sn0/o2000-module-management/nvram-read-vs-second-adapter",
                   test_nvram_read_survives_a_second_adapter);
    qtest_add_func("/sn0/o2000-module-management/late-grant-service",
                   test_late_grant_service);
    qtest_add_func("/sn0/o2000-module-management/migration", test_migration);
    qtest_add_func("/sn0/o2000-module-management/late-grant-service",
                   test_late_grant_service);
    qtest_add_func("/sn0/o2000-module-management/module-serial-migration",
                   test_module_serial_migration_in_progress);
    return g_test_run();
}
