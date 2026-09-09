/*
 * SGI Origin 200 -- the ELSC system controller's checksummed ASCII
 * command/response channel, driven through the real PCF8584 as firmware
 * would. docs/datasheets/pcf8584-elsc.md is the spec this was built from.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define SPACE_XTALK     0x20000000000ULL
#define HUB_LOCAL       (SPACE_XTALK + 0x01000000)
#define HUB_BY_NODE     (SPACE_XTALK + 0x01800000)
#define NODE_STRIDE     0x100000000ULL

#define I2C_DATA        0x220000
#define I2C_CTRL        0x220008

#define CTL_PIN  0x80
#define CTL_ESO  0x40
#define CTL_ES1  0x20
#define CTL_STA  0x04
#define CTL_STO  0x02
#define CTL_ACK  0x01

#define I2C_PCF_LRB  0x08
#define I2C_PCF_BB  0x01

#define ELSC_ADDR 0x08
#define ELSC_NVRAM_PAGE7_ADDR 0x57

/* A framed response's text is capped at 94 bytes plus its own length and
 * checksum bytes -- the datasheet's own bound on what this device may
 * emit. */
#define RESP_CAP 96

/* Two placeholder serial lines put the AUX chardev at serial_hd(2). */
#define ELSC_AUX_LEADING_SERIALS "-serial none -serial none "

static uint8_t frame_checksum(const uint8_t *buf, unsigned len)
{
    uint8_t sum = 0;
    unsigned i;

    for (i = 0; i < len; i++) {
        sum += buf[i];
    }
    return sum;
}

/* Build a correctly-framed request: length byte, text, checksum. Returns
 * the total byte count. */
static unsigned build_request(uint8_t *buf, const char *text)
{
    unsigned text_len = strlen(text);

    buf[0] = text_len;
    memcpy(&buf[1], text, text_len);
    buf[text_len + 1] = (uint8_t)(-(int)frame_checksum(buf, text_len + 1));
    return text_len + 2;
}

/*
 * One combined transaction; the read discards the chip's stale data
 * register first (hw/i2c/pcf8584.c).
 */
static unsigned elsc_transact_at(QTestState *qts, uint64_t hub,
                                 const uint8_t *req, unsigned req_len,
                                 uint8_t *resp, unsigned resp_cap)
{
    unsigned i, total;

    qtest_writeq(qts, hub + I2C_CTRL, CTL_PIN | CTL_ESO);

    qtest_writeq(qts, hub + I2C_DATA, (ELSC_ADDR << 1) | 0);
    qtest_writeq(qts, hub + I2C_CTRL,
                CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(qtest_readq(qts, hub + I2C_CTRL) & I2C_PCF_LRB,
                    ==, 0);

    for (i = 0; i < req_len; i++) {
        qtest_writeq(qts, hub + I2C_DATA, req[i]);
    }

    /*
     * Repeated start, switching direction: control first, then the
     * address byte, per the discriminator hw/i2c/pcf8584.c uses.
     */
    qtest_writeq(qts, hub + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, hub + I2C_DATA, (ELSC_ADDR << 1) | 1);

    (void)qtest_readq(qts, hub + I2C_DATA);  /* discarded */

    resp[0] = qtest_readq(qts, hub + I2C_DATA);
    total = (unsigned)resp[0] + 2;
    g_assert_cmpuint(total, <=, resp_cap);
    for (i = 1; i < total; i++) {
        if (i == total - 1) {
            qtest_writeq(qts, hub + I2C_CTRL, CTL_ESO);  /* NAK last */
        }
        resp[i] = qtest_readq(qts, hub + I2C_DATA);
    }

    qtest_writeq(qts, hub + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);

    return total;
}

static unsigned elsc_transact(QTestState *qts, const uint8_t *req,
                              unsigned req_len, uint8_t *resp,
                              unsigned resp_cap)
{
    return elsc_transact_at(qts, HUB_LOCAL, req, req_len, resp, resp_cap);
}

static void test_ver_command_succeeds(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[8], resp[RESP_CAP];
    const uint8_t *version_end;
    size_t version_len;
    unsigned req_len, resp_len;

    req_len = build_request(req, "ver");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));

    g_assert_cmpuint(resp_len, ==, (unsigned)resp[0] + 2);
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    /* Succeeding takes more than a valid checksum: the text begins "ok". */
    g_assert_cmpuint(resp[0], >=, 4);
    g_assert(memcmp(&resp[1], "ok ", 3) == 0);

    /*
     * Keep the synthetic identity, including its terminator, within the
     * eight-byte compatibility boundary observed during black-box bring-up.
     */
    version_end = memchr(&resp[4], ' ', resp[0] - 3);
    version_len = version_end ? version_end - &resp[4] : resp[0] - 3;
    g_assert_cmpuint(version_len, >, 0);
    g_assert_cmpuint(version_len + 1, <=, 8);

    qtest_quit(qts);
}

static void test_bad_request_checksum_gets_err_csum(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[8], resp[RESP_CAP];
    unsigned req_len, resp_len;

    req_len = build_request(req, "ver");
    req[req_len - 1] ^= 0xff;  /* corrupt only the checksum byte */

    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));

    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 8);
    g_assert(memcmp(&resp[1], "err csum", 8) == 0);

    qtest_quit(qts);
}

static void test_unimplemented_command_gets_err_cmd(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[8], resp[RESP_CAP];
    unsigned req_len, resp_len;

    /*
     * An unimplemented mnemonic must complete the transaction rather than
     * stall the bus; the console-candidate loop depends on that.
     */
    req_len = build_request(req, "xyz");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));

    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    g_assert(memcmp(&resp[1], "err cmd", 7) == 0);

    qtest_quit(qts);
}

static void test_read_switches_command(const void *data)
{
    QTestState *qts;
    uint8_t req[8], resp[RESP_CAP];
    unsigned req_len, resp_len;

    qts = qtest_init("-M origin200");
    req_len = build_request(req, "rsw");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));

    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 5);
    g_assert(memcmp(&resp[1], "ok ff", 5) == 0);
    qtest_quit(qts);

    qts = qtest_init("-M origin200,elsc-dip-switches=1");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));

    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 5);
    g_assert(memcmp(&resp[1], "ok fe", 5) == 0);
    qtest_quit(qts);
}

/*
 * The machine's elsc-dip-switches value must reach the controller no
 * matter how it is spelled or how many chassis are present. Firmware
 * decodes the switches from rsw's active-low byte, so a switch value
 * that never crossed the machine->hub->controller path leaves the
 * console-selection routine seeing the default and skips the local-POD
 * boot stop entirely. Cover 0x10 through the separate -machine option
 * on both the single chassis and the two-chassis rig, reading each
 * node's own controller.
 */
static void test_read_switches_roundtrip(const void *data)
{
    uint8_t req[8], resp[RESP_CAP];
    unsigned req_len, resp_len;
    QTestState *qts;

    req_len = build_request(req, "rsw");

    qts = qtest_init("-M origin200 -machine elsc-dip-switches=16");
    resp_len = elsc_transact_at(qts, HUB_LOCAL, req, req_len, resp,
                                sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 5);
    g_assert(memcmp(&resp[1], "ok ef", 5) == 0);
    qtest_quit(qts);

    qts = qtest_init(
        "-M origin200 -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1 "
        "-machine elsc-dip-switches=16");

    resp_len = elsc_transact_at(qts, HUB_LOCAL, req, req_len, resp,
                                sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 5);
    g_assert(memcmp(&resp[1], "ok ef", 5) == 0);

    resp_len = elsc_transact_at(qts, HUB_BY_NODE + NODE_STRIDE, req, req_len,
                                resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 5);
    g_assert(memcmp(&resp[1], "ok ef", 5) == 0);

    qtest_quit(qts);
}

/*
 * Processor code 0's signal pair, the one code with a real input queue.
 * The wait helpers treat every other code's pulse as "not mine", as
 * firmware does for time slices that are not its own.
 */
#define ARB_SIGNAL1 0xe0
#define ARB_SIGNAL2 0xe8

#define ARB_STEP_NS       (10 * 1000)
#define ARB_TICK_NS       (1000 * 1000)
#define ARB_RETRY_NS      (20 * 1000)
#define ARB_MAX_STEPS     2000

/* Scan more finely than the one-millisecond publication interval. */
#define ARB_SCAN_STEP_NS  (100 * 1000)

static uint64_t read_status(QTestState *qts)
{
    return qtest_readq(qts, HUB_LOCAL + I2C_CTRL);
}

static uint64_t read_data(QTestState *qts)
{
    return qtest_readq(qts, HUB_LOCAL + I2C_DATA);
}

static void arb_enable_observer_at(QTestState *qts, uint64_t hub)
{
    qtest_writeq(qts, hub + I2C_CTRL, CTL_PIN);
    qtest_writeq(qts, hub + I2C_DATA, 0x7f);
    qtest_writeq(qts, hub + I2C_CTRL, CTL_PIN | CTL_ES1);
    qtest_writeq(qts, hub + I2C_DATA, 0x1c);
    qtest_writeq(qts, hub + I2C_CTRL, CTL_PIN);
    g_assert_cmphex(qtest_readq(qts, hub + I2C_DATA), ==, 0x7f);
    qtest_writeq(qts, hub + I2C_DATA, 0x09);
    qtest_writeq(qts, hub + I2C_CTRL, CTL_PIN | CTL_ESO);
}

static void arb_enable_observer(QTestState *qts)
{
    arb_enable_observer_at(qts, HUB_LOCAL);
}

/* Any code's grant, either half of its signal pair. */
static bool arb_is_signal_byte(uint64_t value)
{
    return value >= 0xe0 && value <= 0xef;
}

/* Every grant address is otherwise unmatched on the Origin 200 I2C bus. */
static void test_arbitration_grant_addresses_are_unmatched(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    unsigned token;

    arb_enable_observer(qts);
    for (token = ARB_SIGNAL1; token <= ARB_SIGNAL2 + 7; token++) {
        qtest_writeq(qts, HUB_LOCAL + I2C_DATA, token);
        qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                     CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
        g_assert_cmphex(read_status(qts) & I2C_PCF_LRB, ==, I2C_PCF_LRB);
        qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                     CTL_PIN | CTL_ESO | CTL_STO);
    }

    qtest_quit(qts);
}

/*
 * Loop 1: wait for another processor's grant. Skipping straight to "wait for
 * the signal" passes a model that parks code 0 and never advances.
 */
static void arb_wait_for_absence(QTestState *qts)
{
    int i;

    for (i = 0; i < ARB_MAX_STEPS; i++) {
        uint64_t data = read_data(qts);
        uint64_t status = read_status(qts);

        if ((status & I2C_PCF_BB) && arb_is_signal_byte(data) &&
            (data & 7) != 0) {
            return;
        }
        qtest_clock_step(qts, ARB_STEP_NS);
    }
    g_assert_not_reached();
}

/* Loop 2: step the clock until the bus reports not-busy and the data
 * register holds signal1 or signal2, and report which. */
static uint8_t arb_wait_for_signal(QTestState *qts)
{
    int i;

    for (i = 0; i < ARB_MAX_STEPS; i++) {
        uint64_t data = read_data(qts);
        uint64_t status = read_status(qts);

        if ((status & I2C_PCF_BB) &&
            (data == ARB_SIGNAL1 || data == ARB_SIGNAL2)) {
            return (uint8_t)data;
        }
        qtest_clock_step(qts, ARB_SCAN_STEP_NS);
    }
    g_assert_not_reached();
    return 0;
}

/*
 * Firmware's three-loop wait, deliberately unsynchronised to the
 * rotation's phase -- that is what makes loop 1 a real assertion.
 */
static void test_arbitration_three_loop_wait(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t sig;

    arb_enable_observer(qts);

    arb_wait_for_absence(qts);
    sig = arb_wait_for_signal(qts);
    g_assert(sig == ARB_SIGNAL1 || sig == ARB_SIGNAL2);

    qtest_quit(qts);
}

/*
 * The rotation is free-running: the same three-loop wait must succeed
 * again on the next cycle. A model that pulses once leaves the second
 * wait hanging.
 */
static void test_arbitration_repeats(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t first, second;

    arb_enable_observer(qts);

    arb_wait_for_absence(qts);
    first = arb_wait_for_signal(qts);

    arb_wait_for_absence(qts);
    second = arb_wait_for_signal(qts);
    g_assert_cmphex(second, ==, first);

    qtest_quit(qts);
}

/*
 * The tests above pass a model stuck on code 0; code 1 is checked
 * concretely so a wrong fixed code still fails.
 */
static void test_arbitration_visits_other_processor_codes(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    bool saw_code_0 = false, saw_code_1 = false;
    int i;

    arb_enable_observer(qts);

    /* Several cycles' worth of steps, with margin over model timing. */
    for (i = 0; i < ARB_MAX_STEPS && !(saw_code_0 && saw_code_1); i++) {
        uint64_t observed = read_data(qts);

        if (observed == 0xe0 || observed == 0xe8) {
            saw_code_0 = true;
        }
        if (observed == 0xe1 || observed == 0xe9) {
            saw_code_1 = true;
        }
        qtest_clock_step(qts, ARB_SCAN_STEP_NS);
    }

    g_assert_true(saw_code_0);
    g_assert_true(saw_code_1);

    qtest_quit(qts);
}

/*
 * A grant is an unmatched address phase, not a payload byte. Its completed
 * address replaces readable S0 while START retains its staged ELSC address.
 */
static void test_arbitration_observes_grant_with_staged_start(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[8], resp[RESP_CAP];
    uint8_t signal;
    unsigned req_len, resp_len, i;

    arb_enable_observer(qts);

    arb_wait_for_absence(qts);
    signal = arb_wait_for_signal(qts);

    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, ELSC_ADDR << 1);
    qtest_clock_step(qts, ARB_TICK_NS);

    g_assert_true((read_status(qts) & I2C_PCF_BB) != 0);
    g_assert_true(arb_is_signal_byte(read_data(qts)));
    g_assert_cmpuint(read_data(qts) & 7, ==, ((signal & 7) + 1) & 7);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(read_status(qts) & I2C_PCF_LRB, ==, 0);

    req_len = build_request(req, "ver");
    for (i = 0; i < req_len; i++) {
        qtest_writeq(qts, HUB_LOCAL + I2C_DATA, req[i]);
    }

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, (ELSC_ADDR << 1) | 1);
    (void)qtest_readq(qts, HUB_LOCAL + I2C_DATA);

    resp[0] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    resp_len = (unsigned)resp[0] + 2;
    g_assert_cmpuint(resp_len, <=, sizeof(resp));
    for (i = 1; i < resp_len; i++) {
        if (i == resp_len - 1) {
            qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO);
        }
        resp[i] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    }
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], >=, 2);
    g_assert(resp[1] == 'o' && resp[2] == 'k');

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STO);
    arb_wait_for_absence(qts);
    signal = arb_wait_for_signal(qts);

    /* Reset also releases an address staged without a following START. */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, ELSC_ADDR << 1);
    qtest_system_reset(qts);
    arb_enable_observer(qts);
    arb_wait_for_absence(qts);
    signal = arb_wait_for_signal(qts);

    /* A benign control change must not abandon the staged address. */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, ELSC_ADDR << 1);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_ACK);
    qtest_clock_step(qts, ARB_TICK_NS);
    g_assert_true(arb_is_signal_byte(read_data(qts)));
    g_assert_cmpuint(read_data(qts) & 7, ==, ((signal & 7) + 1) & 7);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(read_status(qts) & I2C_PCF_LRB, ==, 0);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STO);
    arb_wait_for_absence(qts);
    arb_wait_for_signal(qts);

    qtest_quit(qts);
}

/* A fixed seven-millisecond sampler sees all eight codes modulo eight. */
static void test_arbitration_fixed_cadence_walks_codes(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t value;
    unsigned code;
    int i;

    arb_enable_observer(qts);
    arb_wait_for_absence(qts);
    value = arb_wait_for_signal(qts);
    code = value & 7;

    for (i = 0; i < 8; i++) {
        qtest_clock_step(qts, 7 * ARB_TICK_NS);
        value = read_data(qts);
        code = (code + 7) & 7;
        g_assert_true((read_status(qts) & I2C_PCF_BB) != 0);
        g_assert_true(arb_is_signal_byte(value));
        g_assert_cmpuint(value & 7, ==, code);
    }

    qtest_quit(qts);
}

/* A busy tick retries without consuming a processor's turn. */
static void test_arbitration_busy_tick_does_not_advance(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t signal;

    arb_enable_observer(qts);
    arb_wait_for_absence(qts);
    signal = arb_wait_for_signal(qts);

    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, ELSC_ADDR << 1);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(read_status(qts) & I2C_PCF_LRB, ==, 0);
    g_assert_cmphex(read_status(qts) & I2C_PCF_BB, ==, 0);

    qtest_clock_step(qts, 3 * ARB_TICK_NS);
    g_assert_cmphex(read_status(qts) & I2C_PCF_BB, ==, 0);

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STO);
    qtest_clock_step(qts, ARB_RETRY_NS);
    g_assert_true((read_status(qts) & I2C_PCF_BB) != 0);
    g_assert_cmpuint(read_data(qts) & 7, ==, ((signal & 7) + 1) & 7);

    qtest_clock_step(qts, ARB_TICK_NS);
    g_assert_cmpuint(read_data(qts) & 7, ==, ((signal & 7) + 2) & 7);

    qtest_quit(qts);
}

/* Each eligible millisecond tick publishes the next code with no zero gap. */
static void test_arbitration_advances_each_millisecond(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t value;
    unsigned code;
    int i;

    arb_enable_observer(qts);
    arb_wait_for_absence(qts);
    value = arb_wait_for_signal(qts);
    code = value & 7;

    for (i = 0; i < 16; i++) {
        qtest_clock_step(qts, ARB_TICK_NS);
        value = read_data(qts);
        code = (code + 1) & 7;
        g_assert_true((read_status(qts) & I2C_PCF_BB) != 0);
        g_assert_true(arb_is_signal_byte(value));
        g_assert_cmpuint(value & 7, ==, code);
    }

    qtest_quit(qts);
}

/* One grant stays readable until the following millisecond boundary. */
static void test_arbitration_one_publication_per_tick(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t signal;

    arb_enable_observer(qts);
    arb_wait_for_absence(qts);
    signal = arb_wait_for_signal(qts);

    qtest_clock_step(qts, ARB_TICK_NS - 1);
    g_assert_cmphex(read_data(qts), ==, signal);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(read_data(qts) & 7, ==, ((signal & 7) + 1) & 7);

    qtest_quit(qts);
}

/* A complete revolution publishes all eight codes in sequence. */
static void test_arbitration_walks_all_codes(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t value;
    unsigned code;
    unsigned i;

    arb_enable_observer(qts);
    arb_wait_for_absence(qts);
    value = arb_wait_for_signal(qts);
    code = value & 7;

    for (i = 0; i < 8; i++) {
        qtest_clock_step(qts, ARB_TICK_NS);
        value = read_data(qts);
        code = (code + 1) & 7;
        g_assert_true((read_status(qts) & I2C_PCF_BB) != 0);
        g_assert_true(arb_is_signal_byte(value));
        g_assert_cmpuint(value & 7, ==, code);
    }

    qtest_quit(qts);
}

/*
 * A request completed through the direction switch, then abandoned: the
 * transaction ends with the response entirely unread -- not even the
 * discard read runs. This is the window the firmware's slave bring-up
 * waits out: request issued, response pending, the processor parked on
 * its own signal 2 until the controller announces it.
 */
static void elsc_request_without_read(QTestState *qts, const char *text)
{
    uint8_t req[8];
    unsigned req_len, i;

    req_len = build_request(req, text);

    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, (ELSC_ADDR << 1) | 0);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(read_status(qts) & I2C_PCF_LRB, ==, 0);

    for (i = 0; i < req_len; i++) {
        qtest_writeq(qts, HUB_LOCAL + I2C_DATA, req[i]);
    }

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, (ELSC_ADDR << 1) | 1);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);
}

/* A read-only transaction: fetch a response with no request written. */
static unsigned elsc_read_response(QTestState *qts, uint8_t *resp,
                                   unsigned resp_cap)
{
    unsigned i, total;

    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, (ELSC_ADDR << 1) | 1);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(read_status(qts) & I2C_PCF_LRB, ==, 0);

    (void)qtest_readq(qts, HUB_LOCAL + I2C_DATA);  /* discarded */

    resp[0] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    total = (unsigned)resp[0] + 2;
    g_assert_cmpuint(total, <=, resp_cap);
    for (i = 1; i < total; i++) {
        if (i == total - 1) {
            qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO);  /* NAK last */
        }
        resp[i] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    }

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);

    return total;
}

/*
 * Signal 2 is "your slot, something waiting for you", and a pending
 * response is something waiting: on a one-bus controller a response must
 * be announced to its requester, or a processor that ends its request
 * transaction before reading polls for its own signal 2 forever. The
 * firmware's slave bring-up does exactly that -- observed live as a
 * two-node launch parked on a signal 2 no build had ever emitted. A
 * qtest carries no processor context, so the requester attributes as
 * the console processor and the announcement lands in code 0's slot.
 */
static void test_pending_response_announces_signal2(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    g_autofree char *out = NULL;

    arb_enable_observer(qts);

    elsc_request_without_read(qts, "ver");

    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL2);

    out = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(out, "pending response"));
    g_assert_nonnull(strstr(out, "for processor code 0"));

    qtest_quit(qts);
}

/*
 * The announcement ends with the response: a request whose response was
 * read to its end leaves nothing waiting, so the slot reverts to
 * signal 1. This is the half a model that never announces already gets
 * right; it pins the announcement to the pending response rather than
 * to the request having happened.
 */
static void test_consumed_response_reverts_to_signal1(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[8], resp[RESP_CAP];
    unsigned req_len;

    arb_enable_observer(qts);

    req_len = build_request(req, "ver");
    elsc_transact(qts, req, req_len, resp, sizeof(resp));

    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL1);

    qtest_quit(qts);
}

/*
 * The fetch the announcement promises: after the requester's transaction
 * ended unread, the response survives the controller's grant publications,
 * and a read-only
 * transaction returns the frame. Once consumed, the announcement stops.
 */
static void test_announced_response_read_after_stop(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t resp[RESP_CAP];
    unsigned resp_len;

    arb_enable_observer(qts);

    elsc_request_without_read(qts, "ver");

    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL2);

    resp_len = elsc_read_response(qts, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], >=, 4);
    g_assert(memcmp(&resp[1], "ok ", 3) == 0);

    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL1);

    qtest_quit(qts);
}

/*
 * A new request supersedes an unfetched response: the frame is replaced
 * and the old announcement must not outlive it. The superseding
 * command's own response is consumed in full, so nothing is left
 * waiting and the slot reverts to signal 1.
 */
static void test_new_request_supersedes_pending_response(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[8], resp[RESP_CAP];
    unsigned req_len, resp_len;

    arb_enable_observer(qts);

    elsc_request_without_read(qts, "ver");

    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL2);

    req_len = build_request(req, "rsw");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert(memcmp(&resp[1], "ok ff", 5) == 0);

    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL1);

    qtest_quit(qts);
}

/*
 * "acp" output: a ring-buffer chardev captures what the device writes,
 * enough for the output direction with no live connection.
 */
static void test_acp_command_writes_to_console(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 " ELSC_AUX_LEADING_SERIALS
        "-chardev ringbuf,id=aux,size=4096 -serial chardev:aux");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;
    QDict *rb;
    const char *captured;

    req_len = build_request(req, "acp 0 hi");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));

    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    /* "hi" is two characters, accepted count in hex is "2": "ok 2". */
    g_assert_cmpuint(resp[0], ==, 4);
    g_assert(memcmp(&resp[1], "ok 2", 4) == 0);

    /*
     * Not qtest_qmp_assert_success_ref(): it unwraps the result as a
     * QDict and silently returns NULL for ringbuf-read's bare string,
     * which surfaces later as an unattributed segfault.
     */
    rb = qtest_qmp(
        qts, "{'execute': 'ringbuf-read', "
             "'arguments': {'device': 'aux', 'size': 64}}");
    g_assert(qdict_haskey(rb, "return"));
    captured = qdict_get_str(rb, "return");
    g_assert_cmpstr(captured, ==, "hi");
    qobject_unref(rb);

    qtest_quit(qts);
}

/*
 * What one AUX ringbuf captured, read back over QMP. Not
 * qtest_qmp_assert_success_ref(): it unwraps the result as a QDict and
 * silently returns NULL for ringbuf-read's bare string.
 */
static void assert_aux_captured(QTestState *qts, const char *id,
                                const char *expect)
{
    QDict *rb;

    rb = qtest_qmp(
        qts, "{'execute': 'ringbuf-read', "
             "'arguments': {'device': %s, 'size': 64}}", id);
    g_assert(qdict_haskey(rb, "return"));
    g_assert_cmpstr(qdict_get_str(rb, "return"), ==, expect);
    qobject_unref(rb);
}

/*
 * The Origin 200 external AUX port reaches only the master module.
 * Provisioning a backend at node 1's node-major slot must leave it idle.
 *
 * The second transaction preserves QEMU's existing slave-local ELSC command
 * path. That is a model regression, not a hardware claim: the Origin 200
 * contract does not establish slave-local I2C or arbitration behavior.
 */
static void test_two_towers_only_master_aux_active(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1 "
        "-serial null -serial null "
        "-chardev ringbuf,id=aux0,size=4096 -serial chardev:aux0 "
        "-serial null "
        "-serial null -serial null "
        "-chardev ringbuf,id=aux1,size=4096 -serial chardev:aux1");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    req_len = build_request(req, "acp 0 n0");
    resp_len = elsc_transact_at(qts, HUB_LOCAL, req, req_len, resp,
                                sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert(memcmp(&resp[1], "ok 2", 4) == 0);

    req_len = build_request(req, "acp 0 n1");
    resp_len = elsc_transact_at(qts, HUB_BY_NODE + NODE_STRIDE, req,
                                req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert(memcmp(&resp[1], "ok 2", 4) == 0);

    assert_aux_captured(qts, "aux0", "n0");
    assert_aux_captured(qts, "aux1", "");

    qtest_quit(qts);
}

/* Only the master module's controller publishes processor grants. */
static void test_two_towers_only_master_publishes_grants(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1");
    uint64_t slave_hub = HUB_BY_NODE + NODE_STRIDE;

    arb_enable_observer(qts);
    arb_enable_observer_at(qts, slave_hub);
    qtest_clock_step(qts, 2 * ARB_TICK_NS);

    g_assert_true(arb_is_signal_byte(read_data(qts)));
    g_assert_cmphex(qtest_readq(qts, slave_hub + I2C_DATA), ==, 0);

    qtest_quit(qts);
}

/* A dark console must not also wedge the guest: "ok <count>" regardless. */
static void test_acp_with_no_backend_still_answers(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    req_len = build_request(req, "acp 0 hi");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));

    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    /* "hi" is two characters, accepted count in hex is "2": "ok 2". */
    g_assert_cmpuint(resp[0], ==, 4);
    g_assert(memcmp(&resp[1], "ok 2", 4) == 0);

    qtest_quit(qts);
}

/*
 * Console input needs a live connection -- bytes must reach the receive
 * callback -- so a loopback TCP socket stands in for the cable, the
 * ipmi-bt-test pattern: listen before qtest_init, accept after.
 */
static int aux_listen(uint16_t *port)
{
    struct sockaddr_in addr = { 0 };
    socklen_t addrlen = sizeof(addr);
    int lfd;

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    g_assert(lfd >= 0);
    g_assert(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    g_assert(getsockname(lfd, (struct sockaddr *)&addr, &addrlen) == 0);
    *port = ntohs(addr.sin_port);
    g_assert(listen(lfd, 1) == 0);
    return lfd;
}

/*
 * A loaded runner can take many seconds to get QEMU connected; the budget
 * matches sn0-ioc3-serial-test's rather than picking a fresh number.
 */
#define AUX_SOCKET_TIMEOUT_SECS 120

static int aux_accept(int lfd)
{
    fd_set readfds;
    struct timeval tv = { .tv_sec = AUX_SOCKET_TIMEOUT_SECS };
    int fd, val;

    FD_ZERO(&readfds);
    FD_SET(lfd, &readfds);
    g_assert_cmpint(select(lfd + 1, &readfds, NULL, NULL, &tv), ==, 1);
    fd = accept(lfd, NULL, 0);
    g_assert(fd >= 0);
    val = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
    return fd;
}

static void aux_write(int fd, const char *text)
{
    size_t len = strlen(text);

    g_assert_cmpint(write(fd, text, len), ==, (ssize_t)len);
}

/*
 * Score no signal until the bytes reached the queue: the write lands in
 * host time, the scoring runs in virtual time, and an empty queue
 * legitimately presents signal1. Bounded so a model that never queues
 * fails rather than spins.
 */
static void aux_wait_for_queued(QTestState *qts, unsigned len)
{
    g_autofree char *needle = g_strdup_printf("rx queue: %u byte(s)", len);
    int i;

    for (i = 0; i < ARB_MAX_STEPS; i++) {
        g_autofree char *out = qtest_hmp(qts, "info elsc");

        if (strstr(out, needle)) {
            return;
        }
        qtest_clock_step(qts, ARB_STEP_NS);
    }
    g_assert_not_reached();
}

static void test_get_returns_queued_console_input(const void *data)
{
    uint16_t port;
    int lfd = aux_listen(&port);
    g_autofree char *args = g_strdup_printf(
        "-M origin200 " ELSC_AUX_LEADING_SERIALS
        "-chardev socket,id=aux,host=127.0.0.1,port=%u,reconnect-ms=10000 "
        "-serial chardev:aux", port);
    QTestState *qts = qtest_init(args);
    int fd = aux_accept(lfd);
    uint8_t req[8], resp[RESP_CAP];
    unsigned req_len, resp_len;

    aux_write(fd, "hello");

    arb_enable_observer(qts);
    /*
     * signal2 must follow from queue state, not just "get" answering; the
     * bytes must have arrived first or the scoring races their delivery.
     */
    aux_wait_for_queued(qts, 5);
    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL2);

    req_len = build_request(req, "get");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));

    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], >=, 6 + 5);
    g_assert(memcmp(&resp[1], "acp 0 hello", 11) == 0);

    close(fd);
    close(lfd);
    qtest_quit(qts);
}

/*
 * Both halves of the escape split: the positive half alone passes a
 * model with no escape at all.
 */
static void test_escape_byte_is_not_queued(const void *data)
{
    uint16_t port;
    int lfd = aux_listen(&port);
    g_autofree char *args = g_strdup_printf(
        "-M origin200 " ELSC_AUX_LEADING_SERIALS
        "-chardev socket,id=aux,host=127.0.0.1,port=%u,reconnect-ms=10000 "
        "-serial chardev:aux", port);
    QTestState *qts = qtest_init(args);
    int fd = aux_accept(lfd);
    uint8_t req[8], resp[RESP_CAP];
    unsigned req_len, resp_len;

    /*
     * A single \r terminator: what ends an escaped command is the model's
     * own choice, and this exercises that choice, not a CRLF claim.
     */
    aux_write(fd, "AB\x14zzz\rCD");

    arb_enable_observer(qts);
    /* "AB" and "CD" survive; the escaped "\x14zzz\r" is dropped, so four
     * bytes end up queued. Wait for all four before scoring the signal. */
    aux_wait_for_queued(qts, 4);
    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL2);

    req_len = build_request(req, "get");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));

    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 6 + 4);
    g_assert(memcmp(&resp[1], "acp 0 ABCD", 10) == 0);

    close(fd);
    close(lfd);
    qtest_quit(qts);
}

/*
 * Supervisor gating: a protected command answers "err perm" before "pas",
 * and succeeds once the correct password has been given; "pas" itself
 * rejects a password that is not exactly four characters.
 */
static void test_supervisor_gating(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    /* Protected before authenticating. */
    req_len = build_request(req, "rst");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 8);
    g_assert(memcmp(&resp[1], "err perm", 8) == 0);

    /* A password of the wrong length. */
    req_len = build_request(req, "pas ab");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    g_assert(memcmp(&resp[1], "err arg", 7) == 0);

    /* The factory-default password, exactly four characters. */
    req_len = build_request(req, "pas none");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    /* Protected command now succeeds. */
    req_len = build_request(req, "rst");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    qtest_quit(qts);
}

/*
 * "dsc N C" sets one character of the eight-character alphanumeric
 * display (hardware guide, MSC command table). The second node's PROM
 * slave writes its per-node status character with exactly this command
 * during bring-up, and "err cmd" left it retrying forever with
 * processors 2 and 3 never starting. The display is not guest-readable
 * through any documented command, so content is asserted through
 * "info elsc", the way the suite already reads queue state.
 */
static void test_dsc_sets_display_character(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;
    g_autofree char *out = NULL;

    req_len = build_request(req, "dsc 4 E");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    out = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(out, "display: \"    E   \""));

    qtest_quit(qts);
}

/* A position past the eight-character display is an argument error. */
static void test_dsc_bad_position_gets_err_arg(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    req_len = build_request(req, "dsc 8 E");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    g_assert(memcmp(&resp[1], "err arg", 7) == 0);

    qtest_quit(qts);
}

/*
 * The character may be any ASCII other than nul -- a space included --
 * so the parse must be positional, not split on whitespace.
 */
static void test_dsc_space_character_is_legal(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;
    g_autofree char *out = NULL;

    req_len = build_request(req, "dsp AAAAAAAA");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    req_len = build_request(req, "dsc 4  ");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    out = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(out, "display: \"AAAA AAA\""));

    qtest_quit(qts);
}

/*
 * "dsp M" overwrites from position 0 and leaves the remaining
 * positions unaltered: the hardware guide's own example, "dsp testing"
 * replacing seven characters and leaving the eighth. A message longer
 * than the display is rejected and must leave the content untouched.
 */
static void test_dsp_overwrites_leaving_the_tail(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;
    g_autofree char *after_dsp = NULL;
    g_autofree char *after_reject = NULL;

    req_len = build_request(req, "dsc 7 Z");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    req_len = build_request(req, "dsp testing");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    after_dsp = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(after_dsp, "display: \"testingZ\""));

    req_len = build_request(req, "dsp ninechars");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    g_assert(memcmp(&resp[1], "err arg", 7) == 0);

    after_reject = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(after_reject, "display: \"testingZ\""));

    qtest_quit(qts);
}

/*
 * One NVRAM byte as firmware reads it: arbitrated, offset-write then
 * data-read, closed with the invalid-address step, the stale first read
 * discarded.
 */
static uint8_t firmware_style_nvram_read(QTestState *qts, uint8_t offset)
{
    uint8_t value;

    arb_wait_for_absence(qts);
    arb_wait_for_signal(qts);

    qtest_writeq(qts, HUB_LOCAL + I2C_DATA,
                 (ELSC_NVRAM_PAGE7_ADDR << 1) | 0);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & I2C_PCF_LRB,
                    ==, 0);

    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, offset);

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA,
                 (ELSC_NVRAM_PAGE7_ADDR << 1) | 1);

    (void)qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO);
    value = qtest_readq(qts, HUB_LOCAL + I2C_DATA);

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0xfe);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & I2C_PCF_LRB,
                    ==, I2C_PCF_LRB);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);

    return value;
}

/*
 * One full command exactly as firmware drives it: arbitrate, the combined
 * transaction, then -- inside the same transfer -- the closing repeated
 * start to the invalid address, which must NAK, then the stop.
 * elsc_transact() stops after the response; this exists because the
 * acceptance question depends on the whole chain in firmware's order, not
 * any piece in isolation.
 */
static unsigned firmware_style_command(QTestState *qts, const uint8_t *req,
                                       unsigned req_len, uint8_t *resp,
                                       unsigned resp_cap)
{
    unsigned i, total;

    arb_wait_for_absence(qts);
    arb_wait_for_signal(qts);

    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, (ELSC_ADDR << 1) | 0);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & I2C_PCF_LRB, ==, 0);

    for (i = 0; i < req_len; i++) {
        qtest_writeq(qts, HUB_LOCAL + I2C_DATA, req[i]);
    }

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, (ELSC_ADDR << 1) | 1);

    (void)qtest_readq(qts, HUB_LOCAL + I2C_DATA);  /* discarded */

    resp[0] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    total = (unsigned)resp[0] + 2;
    g_assert_cmpuint(total, <=, resp_cap);
    for (i = 1; i < total; i++) {
        if (i == total - 1) {
            qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO);  /* NAK last */
        }
        resp[i] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    }

    /* The closing invalid-address step, still inside this transfer:
     * control first, PIN deliberately absent, then the address byte. */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0xfe);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & I2C_PCF_LRB, ==,
                    I2C_PCF_LRB);

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);

    return total;
}

/*
 * Not "does the register contract work" but "would firmware accept this
 * device". Built from the datasheet's forced boot sequence, not this
 * file's own helpers -- a test written against the implementation can
 * only agree with it: sixteen identity-EEPROM reads (first byte the
 * required 0x37 marker), then rsw, then ver answering a checksum-valid
 * "ok". Every transaction arbitrated first and closed with the
 * invalid-address step, and "accept" checked as firmware's parser checks
 * it -- "ok" followed by a space or terminator, nothing weaker.
 */
static void test_boot_acceptance_sequence(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[8], resp[RESP_CAP];
    uint8_t nvram[16];
    unsigned req_len, resp_len, i;

    arb_enable_observer(qts);

    for (i = 0; i < ARRAY_SIZE(nvram); i++) {
        nvram[i] = firmware_style_nvram_read(qts, i);
    }
    g_assert_cmphex(nvram[0], ==, 0x37);
    for (i = 1; i < ARRAY_SIZE(nvram); i++) {
        g_assert_cmphex(nvram[i], ==, 0);
    }

    req_len = build_request(req, "rsw");
    resp_len = firmware_style_command(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 5);
    g_assert(memcmp(&resp[1], "ok ff", 5) == 0);

    req_len = build_request(req, "ver");
    resp_len = firmware_style_command(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], >=, 2);
    g_assert(resp[1] == 'o' && resp[2] == 'k');
    g_assert(resp[0] == 2 || resp[3] == ' ');

    qtest_quit(qts);
}

/*
 * With nothing typed, "info elsc" must name the empty-queue state --
 * signal1, the value that withholds "get" -- not claim input is waiting.
 */
static void test_info_elsc_dump_empty(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    g_autofree char *out = qtest_hmp(qts, "info elsc");

    g_assert_nonnull(strstr(out, "rx queue: 0 byte(s)"));
    g_assert_nonnull(strstr(out, "pending response: none"));
    g_assert_nonnull(strstr(out, "signal1"));
    g_assert_nonnull(strstr(out, "console input idle"));

    qtest_quit(qts);
}

/*
 * With input typed, the dump must show the bytes and signal2. Waiting for
 * signal2 first makes the bytes' arrival deterministic.
 */
static void test_info_elsc_dump_shows_queued_input(const void *data)
{
    uint16_t port;
    int lfd = aux_listen(&port);
    g_autofree char *args = g_strdup_printf(
        "-M origin200 " ELSC_AUX_LEADING_SERIALS
        "-chardev socket,id=aux,host=127.0.0.1,port=%u,reconnect-ms=10000 "
        "-serial chardev:aux", port);
    QTestState *qts = qtest_init(args);
    int fd = aux_accept(lfd);
    g_autofree char *out = NULL;

    aux_write(fd, "hello");

    arb_enable_observer(qts);
    aux_wait_for_queued(qts, 5);
    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL2);

    out = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(out, "rx queue: 5 byte(s)"));
    g_assert_nonnull(strstr(out, "hello"));
    g_assert_nonnull(strstr(out, "signal2"));
    g_assert_nonnull(strstr(out, "console input waiting"));

    close(fd);
    close(lfd);
    qtest_quit(qts);
}

/*
 * "pwr q" is the power-on query the PROM issues once at power-on to detect
 * a cold start (datasheet: firmware-facing command contracts). The
 * controller answers "ok 1" on the first read in a power cycle and "ok 0"
 * afterwards, clearing the latch on read. (The clear-on-read one-shot is
 * from the 6.5.7 driver and unverified on 6.5.30.)
 */
static void test_pwr_query_reports_cold_power_on(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    /* First query after a cold power-on: the latch reads 1. */
    req_len = build_request(req, "pwr q");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    g_assert(memcmp(&resp[1], "ok 1", 4) == 0);

    /* Cleared on read: a second query in the same power cycle reads 0. */
    req_len = build_request(req, "pwr q");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    g_assert(memcmp(&resp[1], "ok 0", 4) == 0);

    /* An unknown power subcommand is an unknown command, not a bad arg. */
    req_len = build_request(req, "pwr z");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    g_assert(memcmp(&resp[1], "err cmd", 7) == 0);

    /* A known subcommand letter with a bad argument is a bad argument. */
    req_len = build_request(req, "pwr q x");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    g_assert(memcmp(&resp[1], "err arg", 7) == 0);

    qtest_quit(qts);
}

/*
 * "hbt" is the heartbeat / NMI-watchdog control the kernel enables during
 * system-controller init (datasheet: firmware-facing command contracts).
 * "hbt <interval> <delay>" enables it, "hbt 0" disables it, and "hbt" alone
 * sends one heartbeat; all answer a plain "ok". The two arguments are
 * decimal, unlike the hex arguments elsewhere, so a malformed argument is
 * "err arg".
 */
static void test_hbt_watchdog_control(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;
    const char *ok_forms[] = { "hbt", "hbt 0", "hbt 1000 5" };
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(ok_forms); i++) {
        req_len = build_request(req, ok_forms[i]);
        resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
        g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
        g_assert_cmpuint(resp[0], ==, 2);
        g_assert(memcmp(&resp[1], "ok", 2) == 0);
    }

    /* A non-decimal argument is rejected. */
    req_len = build_request(req, "hbt xy");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    g_assert(memcmp(&resp[1], "err arg", 7) == 0);

    /* A single non-zero argument is not a valid form -- only "hbt 0" is. */
    req_len = build_request(req, "hbt 5");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    g_assert(memcmp(&resp[1], "err arg", 7) == 0);

    /*
     * A two-argument form with a non-decimal delay reaches the two-arg
     * validator and is rejected.
     */
    req_len = build_request(req, "hbt 1000 xy");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    g_assert(memcmp(&resp[1], "err arg", 7) == 0);

    /* Junk after the mnemonic with no delimiter is an unknown command. */
    req_len = build_request(req, "hbtx");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    g_assert(memcmp(&resp[1], "err cmd", 7) == 0);

    qtest_quit(qts);
}

/*
 * "rst" is the controller's module reset, supervisor-gated. Once
 * authenticated it performs a real machine reset -- not the no-op the model
 * used to answer. A panic reboot treats "ok" as "the reset happened" and
 * does not fall back to its own hardware reset, so the reset must actually
 * fire; this asserts QEMU's RESET event, the way sn0-hub-test checks a
 * Hub-driven reset, and that the reset cleared controller state.
 */
static void test_rst_performs_machine_reset(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;
    g_autofree char *out = NULL;

    /* Leave a mark on the display the reset must clear. */
    req_len = build_request(req, "dsc 4 E");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    /* Refused before authenticating. */
    req_len = build_request(req, "rst");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 8);
    g_assert(memcmp(&resp[1], "err perm", 8) == 0);

    /* Authenticate with the factory-default password. */
    req_len = build_request(req, "pas none");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    /* Now "rst" answers ok and the reset really fires. */
    req_len = build_request(req, "rst");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    /* The reset actually happened, not just an "ok". */
    qtest_qmp_eventwait(qts, "RESET");

    /* And it cleared controller state: the display is blank again. */
    out = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(out, "display: \"        \""));

    qtest_quit(qts);
}

/*
 * The deferred reset must wait for the "ok" to be read, not merely for the
 * request's write transaction to end. A requester may end its write with
 * the ack unread and fetch it in a later read-only transaction (the
 * response outlives its transaction); firing the reset at the write's STOP
 * would reset the controller and discard the ack the caller is about to
 * fetch.
 */
static void test_rst_reset_waits_for_ack_read(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    req_len = build_request(req, "pas none");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    /* Write "rst" and end the transaction without reading the response. */
    elsc_request_without_read(qts, "rst");

    /* The ack survives to be fetched in a separate read transaction. */
    resp_len = elsc_read_response(qts, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    /* Only now, once the ack has been read, does the reset fire. */
    qtest_qmp_eventwait(qts, "RESET");

    qtest_quit(qts);
}

/*
 * The deferred reset is tied to the specific "rst" ack. If the ack is left
 * unread and a new request supersedes the pending response, the reset is
 * dropped -- otherwise consuming the new, unrelated response would fire it.
 * A mark left on the display survives, proving no reset ran.
 */
static void test_rst_reset_dropped_on_supersession(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;
    g_autofree char *out = NULL;

    req_len = build_request(req, "dsc 4 E");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    req_len = build_request(req, "pas none");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    /* Issue "rst" but leave its ack unread. */
    elsc_request_without_read(qts, "rst");

    /* A new request supersedes the ack; its own response reads back clean. */
    req_len = build_request(req, "ver");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert(memcmp(&resp[1], "ok ", 3) == 0);

    /* No reset ran: the display mark is still there. */
    out = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(out, "display: \"    E   \""));

    qtest_quit(qts);
}

/*
 * The reset fires when the ack is fully read, not at a terminal bus event.
 * A caller that reads the whole ack and then NACKs and issues a same-address
 * repeated START (rather than STOP) gets no I2C finish, yet the reset must
 * still fire. This reads the whole ack and issues no STOP at all, then
 * expects the RESET event -- proving the firing is tied to consumption.
 */
static void test_rst_reset_fires_without_stop(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, i, total;

    req_len = build_request(req, "pas none");
    elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    /* Issue "rst"; the ack is left pending. */
    elsc_request_without_read(qts, "rst");

    /* Read the whole ack in a read transaction, issuing no STOP. */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, (ELSC_ADDR << 1) | 1);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(read_status(qts) & I2C_PCF_LRB, ==, 0);
    (void)qtest_readq(qts, HUB_LOCAL + I2C_DATA);   /* discard stale */
    resp[0] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    total = (unsigned)resp[0] + 2;
    g_assert_cmpuint(total, <=, sizeof(resp));
    for (i = 1; i < total; i++) {
        if (i == total - 1) {
            qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO);   /* NAK last */
        }
        resp[i] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    }
    g_assert_cmphex(frame_checksum(resp, total), ==, 0);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    /* Reading the last ack byte fired the reset; no STOP was issued. */
    qtest_qmp_eventwait(qts, "RESET");

    qtest_quit(qts);
}

/*
 * The last-text-byte NACK race. A master clears ACK on the last TEXT byte of
 * the ack -- the byte before the checksum -- and then, without a STOP, issues
 * a same-address repeated START carrying an unrelated request. The chip fires
 * I2C_NACK while txpos is still below txlen (the checksum has not been fetched
 * yet), so a commit tied to the NACK misses it; and because the master issues
 * a repeated START instead of a STOP, no I2C_FINISH follows either. A commit
 * tied to those terminal events would therefore never happen, and
 * elsc_request_complete would then cancel the still-ARMED reset when the
 * unrelated request arrives -- silently dropping a reset the guest was already
 * promised, since it has received the whole "ok". Committing at the fetch seam
 * (the read that advances txpos to txlen, which is exactly the read that hands
 * over the last text byte) keeps the reset. RED before the seam commit, GREEN
 * after.
 */
static void test_rst_reset_survives_last_text_byte_nack(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, i;

    req_len = build_request(req, "pas none");
    elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    /* Issue "rst"; the ack is left pending (ARMED). */
    elsc_request_without_read(qts, "rst");

    /*
     * Read the ack up to and including the last text byte, NACKing it. The
     * frame is len,'o','k',checksum: 'k' is the last text byte, and the read
     * that returns it is the one whose fetch advances txpos to txlen -- the
     * fetch seam. The checksum is left unread.
     */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, (ELSC_ADDR << 1) | 1);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(read_status(qts) & I2C_PCF_LRB, ==, 0);
    (void)qtest_readq(qts, HUB_LOCAL + I2C_DATA);       /* discard stale */
    resp[0] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);   /* length */
    g_assert_cmpuint(resp[0], ==, 2);
    resp[1] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);   /* 'o' */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO);   /* NAK the next byte */
    resp[2] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);   /* 'k' */
    /* The whole ack was delivered, though the checksum stays unread. */
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    /*
     * No STOP: a same-address repeated START carrying an unrelated "ver".
     * Under a terminal-event commit, the direction switch below cancels the
     * still-ARMED reset.
     */
    req_len = build_request(req, "ver");
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, (ELSC_ADDR << 1) | 0);
    for (i = 0; i < req_len; i++) {
        qtest_writeq(qts, HUB_LOCAL + I2C_DATA, req[i]);
    }
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, (ELSC_ADDR << 1) | 1);

    /*
     * The reset committed at the fetch seam fires; the unrelated request did
     * not cancel it.
     */
    qtest_qmp_eventwait(qts, "RESET");

    qtest_quit(qts);
}

/*
 * A "rst" whose ack the master ends by NACKing the last byte with no STOP
 * leaves this controller matched on the I2C bus: the STOP that would call
 * i2c_end_transfer never comes. The machine reset the "rst" triggers clears
 * the PCF8584's own transfer state but not the bus's slave match, so unless
 * the reset also releases the bus the first post-reset transaction is
 * misrouted to this controller -- an identity-EEPROM read at page 7 (address
 * 0x57) is answered by the ELSC instead, its first byte an ELSC response
 * length rather than the EEPROM's 0x37 marker. This drives that exact
 * sequence and reads page 7 directly (no arbitration -- the stale match also
 * wedges the bus permanently busy, which stalls the grant rotation)
 * and requires the EEPROM's own byte back.
 */
static void test_rst_reset_releases_i2c_bus(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, i, total;
    uint8_t page7_byte0;

    req_len = build_request(req, "pas none");
    elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    /* Issue "rst"; leave the ack pending. */
    elsc_request_without_read(qts, "rst");

    /* Read the whole ack, NACK the last byte, issue no STOP. */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, (ELSC_ADDR << 1) | 1);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(read_status(qts) & I2C_PCF_LRB, ==, 0);
    (void)qtest_readq(qts, HUB_LOCAL + I2C_DATA);   /* discard stale */
    resp[0] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    total = (unsigned)resp[0] + 2;
    g_assert_cmpuint(total, <=, sizeof(resp));
    for (i = 1; i < total; i++) {
        if (i == total - 1) {
            qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO);   /* NAK last */
        }
        resp[i] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    }
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    /* Reading the last ack byte fired the reset. */
    qtest_qmp_eventwait(qts, "RESET");

    /*
     * Re-enable the controller and read identity-EEPROM page 7, offset 0,
     * directly. With the bus released this routes to the EEPROM and byte 0
     * is the 0x37 marker; with the ELSC left matched it is misrouted and
     * answers as the ELSC.
     */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, (ELSC_NVRAM_PAGE7_ADDR << 1) | 0);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0);   /* byte-address within page */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, (ELSC_NVRAM_PAGE7_ADDR << 1) | 1);
    (void)qtest_readq(qts, HUB_LOCAL + I2C_DATA);   /* discard stale */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO);
    page7_byte0 = qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0xfe);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);

    g_assert_cmphex(page7_byte0, ==, 0x37);

    qtest_quit(qts);
}

/* Migrate a stopped source to a listening destination; block until done. */
static void migrate_stopped_vm(QTestState *from, const char *uri)
{
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);

    g_test_timer_start();
    for (;;) {
        QDict *ret = qtest_qmp_assert_success_ref(from,
            "{ 'execute': 'query-migrate' }");
        g_autofree char *status = g_strdup(qdict_get_str(ret, "status"));

        qobject_unref(ret);
        g_assert_cmpstr(status, !=, "failed");
        if (g_str_equal(status, "completed")) {
            break;
        }
        g_assert(g_test_timer_elapsed() < 30);
        g_usleep(1000);
    }
}

/*
 * A "rst" accepted but not yet acknowledged is ARMED: its migrated state
 * carries what to do and that the ack is still unread. Migrating in that
 * window must NOT fire the reset on the destination -- post_load re-arms the
 * bottom half only for a COMMITTED action, so an ARMED one holds its "ok" for
 * the guest to read. The destination reads the ack back intact (a premature
 * reset would have discarded it and cleared the response), and reading it
 * commits the action and fires the reset there. This is the migration half of
 * the deferred-action state machine; the committed-then-migrate window is a
 * sub-instruction race qtest cannot stage, so it is covered by construction.
 */
static void test_rst_reset_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    tmpfs = g_dir_make_tmp("sn0-elsc-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init("-M origin200");

    req_len = build_request(req, "pas none");
    elsc_transact(from, req, req_len, resp, sizeof(resp));
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    /* Accept "rst" but leave its ack unread: the reset is ARMED, not fired. */
    elsc_request_without_read(from, "rst");

    dest_cmdline = g_strdup_printf("-M origin200 -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    /*
     * The ARMED reset survived migration without firing early: the ack is
     * still there to read, and reading it (its terminal STOP) commits and
     * fires the reset on the destination.
     */
    resp_len = elsc_read_response(to, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    qtest_qmp_eventwait(to, "RESET");

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * A committed action whose response frame is still OPEN at migration -- the
 * guest read through the last text byte (the reset committed at the seam) but
 * has not yet taken the trailing checksum -- must not fire on the destination
 * until that frame is finished. The bottom half was never scheduled on the
 * source (no boundary yet), so deferred_scheduled is clear and post_load leaves
 * it alone; the destination's own closing STOP schedules it.
 *
 * The hazard this pins is post_load firing the reset on load: that resets the
 * PCF8584 while the checksum the guest is about to read still sits in its
 * read-ahead, corrupting the frame. So the source reads up to and including
 * "k" (committing), migrates with the checksum unread, and the destination
 * reads that checksum back INTACT and then STOPs -- and only then does the
 * reset fire. With post_load keyed on committed rather than on
 * deferred_scheduled, it fired on load and the checksum came back as the reset
 * value, failing the frame checksum.
 */
static void test_rst_reset_open_frame_defers_across_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len;

    tmpfs = g_dir_make_tmp("sn0-elsc-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init("-M origin200");

    req_len = build_request(req, "pas none");
    elsc_transact(from, req, req_len, resp, sizeof(resp));
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    /*
     * Accept "rst" (ARMED), then open a read and take len, 'o', 'k' -- the
     * last text byte, whose fetch reaches the seam and commits the reset. The
     * checksum is prefetched into the chip but left unread; no STOP, so the
     * frame stays open (txlen != 0) and the bottom half is not yet scheduled.
     */
    elsc_request_without_read(from, "rst");
    qtest_writeq(from, HUB_LOCAL + I2C_DATA, (ELSC_ADDR << 1) | 1);
    qtest_writeq(from, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(read_status(from) & I2C_PCF_LRB, ==, 0);
    (void)qtest_readq(from, HUB_LOCAL + I2C_DATA);       /* discard stale */
    resp[0] = qtest_readq(from, HUB_LOCAL + I2C_DATA);   /* length */
    g_assert_cmpuint(resp[0], ==, 2);
    resp[1] = qtest_readq(from, HUB_LOCAL + I2C_DATA);   /* 'o' */
    resp[2] = qtest_readq(from, HUB_LOCAL + I2C_DATA);   /* 'k', seam commits */
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    dest_cmdline = g_strdup_printf("-M origin200 -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    /*
     * On the destination the open frame is intact: the checksum reads back its
     * migrated value (post_load did not reset the chip out from under it), and
     * the whole frame still sums to zero.
     */
    resp[3] = qtest_readq(to, HUB_LOCAL + I2C_DATA);     /* checksum */
    qtest_writeq(to, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);
    g_assert_cmphex(frame_checksum(resp, 4), ==, 0);

    /* The closing STOP is the boundary that carries out the committed reset. */
    qtest_qmp_eventwait(to, "RESET");

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * "tas <ch>" / "tas 0" is the test-and-set lock cell (datasheet:
 * firmware-facing command contracts), driven by the 6.5.30 firmware in its
 * arbitration path -- a distinct byte, not the bus arbitration. <ch> is a
 * LITERAL character: the client compares the owner byte for byte, so the
 * response echoes the exact byte held before the request, and any single
 * non-'0' character is a valid channel. Acquiring a free cell reads back
 * "ok 0"; a caller polls until it reads 0 (was free) or its own byte. ASCII
 * '0' releases. (Literal-byte round-trip; the datasheet's "hex" wording is
 * flagged for the source owner, and the pre-write value is 6.5.7-derived,
 * unverified on 6.5.30.)
 */
static void test_tas_lock_cell(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    /* Free cell: the pre-acquisition value is 0, then 'A' installs. */
    req_len = build_request(req, "tas A");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    g_assert(memcmp(&resp[1], "ok 0", 4) == 0);

    /* Re-acquiring reads the owner back literally -- "ok A", not "ok a". */
    req_len = build_request(req, "tas A");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    g_assert(memcmp(&resp[1], "ok A", 4) == 0);

    /* Held by 'A': a different caller reads the holder, not its own byte. */
    req_len = build_request(req, "tas z");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    g_assert(memcmp(&resp[1], "ok A", 4) == 0);

    /* Release: the pre-release value is the holder, and the cell frees. */
    req_len = build_request(req, "tas 0");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    g_assert(memcmp(&resp[1], "ok A", 4) == 0);

    /* 'z' is a valid non-hex channel: the free cell reads 0 and installs it. */
    req_len = build_request(req, "tas z");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    g_assert(memcmp(&resp[1], "ok 0", 4) == 0);

    /* And it reads back as itself. */
    req_len = build_request(req, "tas z");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    g_assert(memcmp(&resp[1], "ok z", 4) == 0);

    /* More than one channel character is a bad argument. */
    req_len = build_request(req, "tas zz");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    g_assert(memcmp(&resp[1], "err arg", 7) == 0);

    /* Junk after the mnemonic with no delimiter is an unknown command. */
    req_len = build_request(req, "tasx");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    g_assert(memcmp(&resp[1], "err cmd", 7) == 0);

    qtest_quit(qts);
}

/*
 * A NUL (0x00) owner is a legal channel and must not be read as the free
 * cell. Acquiring the free cell with owner 0x00 leaves it HELD; a later
 * caller then reads the 0x00 owner back rather than acquiring, and that
 * owner byte round-trips through the response frame -- which a strlen-framed
 * "ok \0" reply would truncate to "ok ". (datasheet: firmware-facing command
 * contracts; any byte but the release channel '0' is a valid owner.)
 */
static void test_tas_lock_nul_owner(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    /*
     * "tas " with a NUL owner byte: text length 5. Built by hand because
     * build_request() takes a C string and would stop at the NUL.
     */
    req[0] = 5;
    req[1] = 't';
    req[2] = 'a';
    req[3] = 's';
    req[4] = ' ';
    req[5] = 0x00;
    req[6] = (uint8_t)(-(int)frame_checksum(req, 6));
    req_len = 7;

    /* Acquire the free cell: the pre-acquisition value is the free sentinel. */
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    g_assert(memcmp(&resp[1], "ok 0", 4) == 0);

    /*
     * A different caller now finds the cell HELD -- the buggy free-sentinel
     * conflation would let it acquire and read "ok 0" instead. It reads the
     * 0x00 owner back, and that owner byte survives the response framing.
     */
    req_len = build_request(req, "tas z");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    g_assert_cmpuint(resp[1], ==, 'o');
    g_assert_cmpuint(resp[2], ==, 'k');
    g_assert_cmpuint(resp[3], ==, ' ');
    g_assert_cmpuint(resp[4], ==, 0x00);

    /*
     * And 'z' did not steal it: the NUL owner still holds. Re-reading with
     * the NUL owner reads its own byte back, so the cell is occupied.
     */
    req[0] = 5;
    req[1] = 't';
    req[2] = 'a';
    req[3] = 's';
    req[4] = ' ';
    req[5] = 0x00;
    req[6] = (uint8_t)(-(int)frame_checksum(req, 6));
    req_len = 7;
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    g_assert_cmpuint(resp[4], ==, 0x00);

    /* Release frees it; the pre-release value is the NUL owner. */
    req_len = build_request(req, "tas 0");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    g_assert_cmpuint(resp[4], ==, 0x00);

    /* Free again: 'z' acquires and reads the free sentinel back. */
    req_len = build_request(req, "tas z");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    g_assert(memcmp(&resp[1], "ok 0", 4) == 0);

    qtest_quit(qts);
}

/*
 * The remaining power subcommands (datasheet: firmware-facing command
 * contracts), none boot-driven. This covers the response contract with no
 * real action: the read-only "pwr r" (redundant-supply query) is ungated,
 * the destructive "pwr d <sec>"/"pwr c" are supervisor-gated like "rst", and
 * out-of-range or extra arguments are "err arg". The actions themselves are
 * checked in the shutdown and power-cycle tests below.
 */
static void test_pwr_control_subcommands(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    /*
     * The read-only supply query needs no supervisor. The wire response is a
     * per-module record per supply, tagged tag:present:health --
     * "ok m:y:ok s:y:ok" with both supplies modelled present and healthy.
     */
    req_len = build_request(req, "pwr r");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 16);
    g_assert(memcmp(&resp[1], "ok m:y:ok s:y:ok", 16) == 0);

    /* Destructive power-down/cycle are refused before authenticating. */
    req_len = build_request(req, "pwr d 3c");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 8);
    g_assert(memcmp(&resp[1], "err perm", 8) == 0);

    req_len = build_request(req, "pwr c");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmpuint(resp[0], ==, 8);
    g_assert(memcmp(&resp[1], "err perm", 8) == 0);

    /* Authenticate with the factory-default password. */
    req_len = build_request(req, "pas none");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    /*
     * Out-of-range (below 5, above 0x258) and non-hex delays are err arg, as
     * are extra arguments to a known subcommand letter (a bad argument, not
     * an unknown command). None of these arm an action.
     */
    {
        const char *bad_forms[] = { "pwr d 1", "pwr d 4", "pwr d 259",
                                    "pwr d zz", "pwr c extra", "pwr r x" };
        unsigned i;

        for (i = 0; i < ARRAY_SIZE(bad_forms); i++) {
            req_len = build_request(req, bad_forms[i]);
            resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
            g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
            g_assert_cmpuint(resp[0], ==, 7);
            g_assert(memcmp(&resp[1], "err arg", 7) == 0);
        }
    }

    qtest_quit(qts);
}

/*
 * A supervised "pwr d <sec>" answers "ok" and then, once the ack is read,
 * shuts the machine down -- verified by QEMU's SHUTDOWN event. "-no-shutdown"
 * keeps QEMU alive in the shutdown state so the event can be observed.
 */
static void test_pwr_down_shuts_down(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -no-shutdown");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    req_len = build_request(req, "pas none");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    /* Immediate power-down: "ok", then the shutdown fires. */
    req_len = build_request(req, "pwr d 0");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    qtest_qmp_eventwait(qts, "SHUTDOWN");

    qtest_quit(qts);
}

/*
 * A supervised "pwr c" answers "ok" and then, once the ack is read,
 * power-cycles the machine -- a machine reset, verified by the RESET event.
 */
static void test_pwr_cycle_resets(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    req_len = build_request(req, "pas none");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    req_len = build_request(req, "pwr c");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

    qtest_qmp_eventwait(qts, "RESET");

    qtest_quit(qts);
}

/*
 * "pwr d" validates the delay range before the supervisor gate, consistent
 * with the non-hex delay check. So an out-of-range delay is "err arg" whether
 * or not the caller has authenticated -- a bad argument is not disguised as
 * "err perm". An in-range delay from an unauthenticated caller is still
 * "err perm". Boundaries: 5..0x258 inclusive is in range, and 0 (immediate)
 * skips the range check.
 */
static void test_pwr_down_range_checked_before_auth(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len, i;
    /* Below 5 or above 0x258: err arg, before the gate is even consulted. */
    const char *out_of_range[] = { "pwr d 1", "pwr d 4", "pwr d 259" };
    /* In range (and 0), so the gate applies: err perm without a password. */
    const char *in_range[] = { "pwr d 0", "pwr d 5", "pwr d 258" };

    for (i = 0; i < ARRAY_SIZE(out_of_range); i++) {
        req_len = build_request(req, out_of_range[i]);
        resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
        g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
        g_assert_cmpuint(resp[0], ==, 7);
        g_assert(memcmp(&resp[1], "err arg", 7) == 0);
    }
    for (i = 0; i < ARRAY_SIZE(in_range); i++) {
        req_len = build_request(req, in_range[i]);
        resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
        g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
        g_assert_cmpuint(resp[0], ==, 8);
        g_assert(memcmp(&resp[1], "err perm", 8) == 0);
    }

    qtest_quit(qts);
}

/*
 * A "pwr c" power-cycle is a cold restart, not a warm reset: it re-arms the
 * cold-power latch, so the first "pwr q" after the cycle reads "ok 1" even
 * though an earlier query had cleared the latch. A warm "rst" leaves it
 * cleared.
 */
static void test_pwr_cycle_rearms_cold_power(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    /* Clear the cold-power latch: the first query reads 1, the next 0. */
    req_len = build_request(req, "pwr q");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert(memcmp(&resp[1], "ok 1", 4) == 0);
    req_len = build_request(req, "pwr q");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert(memcmp(&resp[1], "ok 0", 4) == 0);

    /* Authenticate and power-cycle. */
    req_len = build_request(req, "pas none");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert(memcmp(&resp[1], "ok", 2) == 0);
    req_len = build_request(req, "pwr c");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert(memcmp(&resp[1], "ok", 2) == 0);
    qtest_qmp_eventwait(qts, "RESET");

    /* The cycle re-armed the latch: the first post-cycle query reads 1. */
    req_len = build_request(req, "pwr q");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    g_assert(memcmp(&resp[1], "ok 1", 4) == 0);

    qtest_quit(qts);
}

/*
 * "nic <p> <s> <d>" is the one-wire identity passthrough (datasheet:
 * firmware-facing command contracts), an interactive-monitor command, not
 * boot-driven. Three hex arguments bit-bang the midplane one-wire line; it
 * answers "ok <hex>" with the sampled bit. No one-wire device is modelled,
 * so the open-drain line idles high and every sample reads 1.
 */
static void test_nic_onewire_identity(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    /* Three hex arguments: the idle line samples high. */
    req_len = build_request(req, "nic 1 2 3");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    g_assert(memcmp(&resp[1], "ok 1", 4) == 0);

    /* Too few arguments to a known command is a bad argument. */
    req_len = build_request(req, "nic 1 2");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    g_assert(memcmp(&resp[1], "err arg", 7) == 0);

    /* A non-hex argument. */
    req_len = build_request(req, "nic 1 2 zz");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    g_assert(memcmp(&resp[1], "err arg", 7) == 0);

    /* Junk after the mnemonic with no delimiter is an unknown command. */
    req_len = build_request(req, "nicx");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    g_assert(memcmp(&resp[1], "err cmd", 7) == 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/elsc/ver-command-succeeds", NULL,
                        test_ver_command_succeeds);
    qtest_add_data_func("sn0/elsc/bad-request-checksum-gets-err-csum", NULL,
                        test_bad_request_checksum_gets_err_csum);
    qtest_add_data_func("sn0/elsc/unimplemented-command-gets-err-cmd", NULL,
                        test_unimplemented_command_gets_err_cmd);
    qtest_add_data_func("sn0/elsc/read-switches-command", NULL,
                        test_read_switches_command);
    qtest_add_data_func("sn0/elsc/read-switches-roundtrip", NULL,
                        test_read_switches_roundtrip);
    qtest_add_data_func("sn0/elsc/arbitration-three-loop-wait", NULL,
                        test_arbitration_three_loop_wait);
    qtest_add_data_func("sn0/elsc/arbitration-repeats", NULL,
                        test_arbitration_repeats);
    qtest_add_data_func("sn0/elsc/arbitration-visits-other-processor-codes",
                        NULL, test_arbitration_visits_other_processor_codes);
    qtest_add_data_func("sn0/elsc/arbitration-grant-addresses-are-unmatched",
                        NULL, test_arbitration_grant_addresses_are_unmatched);
    qtest_add_data_func("sn0/elsc/arbitration-observes-grant-with-staged-start",
                        NULL,
                        test_arbitration_observes_grant_with_staged_start);
    qtest_add_data_func("sn0/elsc/arbitration-fixed-cadence-walks-codes",
                        NULL, test_arbitration_fixed_cadence_walks_codes);
    qtest_add_data_func("sn0/elsc/arbitration-busy-tick-does-not-advance",
                        NULL, test_arbitration_busy_tick_does_not_advance);
    qtest_add_data_func("sn0/elsc/arbitration-advances-each-millisecond",
                        NULL, test_arbitration_advances_each_millisecond);
    qtest_add_data_func("sn0/elsc/arbitration-one-publication-per-tick",
                        NULL, test_arbitration_one_publication_per_tick);
    qtest_add_data_func("sn0/elsc/arbitration-walks-all-codes",
                        NULL, test_arbitration_walks_all_codes);
    qtest_add_data_func("sn0/elsc/pending-response-announces-signal2", NULL,
                        test_pending_response_announces_signal2);
    qtest_add_data_func("sn0/elsc/consumed-response-reverts-to-signal1", NULL,
                        test_consumed_response_reverts_to_signal1);
    qtest_add_data_func("sn0/elsc/announced-response-read-after-stop", NULL,
                        test_announced_response_read_after_stop);
    qtest_add_data_func("sn0/elsc/new-request-supersedes-pending-response",
                        NULL, test_new_request_supersedes_pending_response);
    qtest_add_data_func("sn0/elsc/acp-command-writes-to-console", NULL,
                        test_acp_command_writes_to_console);
    qtest_add_data_func("sn0/elsc/two-towers-only-master-aux-active", NULL,
                        test_two_towers_only_master_aux_active);
    qtest_add_data_func("sn0/elsc/two-towers-only-master-publishes-grants",
                        NULL, test_two_towers_only_master_publishes_grants);
    qtest_add_data_func("sn0/elsc/acp-with-no-backend-still-answers", NULL,
                        test_acp_with_no_backend_still_answers);
    qtest_add_data_func("sn0/elsc/get-returns-queued-console-input", NULL,
                        test_get_returns_queued_console_input);
    qtest_add_data_func("sn0/elsc/escape-byte-is-not-queued", NULL,
                        test_escape_byte_is_not_queued);
    qtest_add_data_func("sn0/elsc/supervisor-gating", NULL,
                        test_supervisor_gating);
    qtest_add_data_func("sn0/elsc/dsc-sets-display-character", NULL,
                        test_dsc_sets_display_character);
    qtest_add_data_func("sn0/elsc/dsc-bad-position-gets-err-arg", NULL,
                        test_dsc_bad_position_gets_err_arg);
    qtest_add_data_func("sn0/elsc/dsc-space-character-is-legal", NULL,
                        test_dsc_space_character_is_legal);
    qtest_add_data_func("sn0/elsc/dsp-overwrites-leaving-the-tail", NULL,
                        test_dsp_overwrites_leaving_the_tail);
    qtest_add_data_func("sn0/elsc/boot-acceptance-sequence", NULL,
                        test_boot_acceptance_sequence);
    qtest_add_data_func("sn0/elsc/info-elsc-dump-empty", NULL,
                        test_info_elsc_dump_empty);
    qtest_add_data_func("sn0/elsc/info-elsc-dump-shows-queued-input", NULL,
                        test_info_elsc_dump_shows_queued_input);
    qtest_add_data_func("sn0/elsc/pwr-query-reports-cold-power-on", NULL,
                        test_pwr_query_reports_cold_power_on);
    qtest_add_data_func("sn0/elsc/hbt-watchdog-control", NULL,
                        test_hbt_watchdog_control);
    qtest_add_data_func("sn0/elsc/rst-performs-machine-reset", NULL,
                        test_rst_performs_machine_reset);
    qtest_add_data_func("sn0/elsc/rst-reset-waits-for-ack-read", NULL,
                        test_rst_reset_waits_for_ack_read);
    qtest_add_data_func("sn0/elsc/rst-reset-dropped-on-supersession", NULL,
                        test_rst_reset_dropped_on_supersession);
    qtest_add_data_func("sn0/elsc/rst-reset-fires-without-stop", NULL,
                        test_rst_reset_fires_without_stop);
    qtest_add_data_func("sn0/elsc/rst-reset-survives-last-text-byte-nack",
                        NULL, test_rst_reset_survives_last_text_byte_nack);
    qtest_add_data_func("sn0/elsc/rst-reset-releases-i2c-bus", NULL,
                        test_rst_reset_releases_i2c_bus);
    qtest_add_data_func("sn0/elsc/rst-reset-survives-migration", NULL,
                        test_rst_reset_survives_migration);
    qtest_add_data_func(
        "sn0/elsc/rst-reset-open-frame-defers-across-migration",
        NULL, test_rst_reset_open_frame_defers_across_migration);
    qtest_add_data_func("sn0/elsc/tas-lock-cell", NULL,
                        test_tas_lock_cell);
    qtest_add_data_func("sn0/elsc/tas-lock-nul-owner", NULL,
                        test_tas_lock_nul_owner);
    qtest_add_data_func("sn0/elsc/pwr-control-subcommands", NULL,
                        test_pwr_control_subcommands);
    qtest_add_data_func("sn0/elsc/pwr-down-shuts-down", NULL,
                        test_pwr_down_shuts_down);
    qtest_add_data_func("sn0/elsc/pwr-cycle-resets", NULL,
                        test_pwr_cycle_resets);
    qtest_add_data_func("sn0/elsc/pwr-down-range-checked-before-auth", NULL,
                        test_pwr_down_range_checked_before_auth);
    qtest_add_data_func("sn0/elsc/pwr-cycle-rearms-cold-power", NULL,
                        test_pwr_cycle_rearms_cold_power);
    qtest_add_data_func("sn0/elsc/nic-onewire-identity", NULL,
                        test_nic_onewire_identity);

    ret = g_test_run();

    return ret;
}
