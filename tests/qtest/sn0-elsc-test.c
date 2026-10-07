/*
 * Origin 200 ELSC checksummed ASCII commands and replies through PCF8584.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sn0-user-image.h"
#include "sgi-migrate.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "sgi-test.h"

#define HUB_BY_NODE     (SPACE_IO + 0x01800000)

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
 * checksum bytes. This is the response limit. */
#define RESP_CAP 96

/* An acknowledged "rst" resets the machine a second later. */
#define ELSC_RESET_WAIT_NS NANOSECONDS_PER_SECOND

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

/*
 * One plain origin200 serves every case that names no other command line,
 * started in main(). Cases setting the switches, a second tower, an extra
 * chardev, a migration destination or -no-shutdown start their own.
 */
static QTestState *qts;

/*
 * Open such a case. Reset leaves the adapter idle, which its pending-
 * interrupt bit reports set, and checking that here is what proves the
 * previous case is not still half-way through a frame. The bus-free bit
 * is not checked with it: the controller drives the arbitration ring
 * continuously, so the wire is busy whenever anything is listening.
 */
static void elsc_reset(void)
{
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & CTL_PIN, ==,
                    CTL_PIN);
}

/*
 * A framed response's text against what it must begin with. The bare
 * memcmp assertions this replaces failed with a line number and nothing
 * else; the controller answers in ASCII, so printing both sides is what
 * makes the failure readable.
 */
static void assert_response_begins(const uint8_t *resp, const char *want)
{
    g_autofree char *seen = g_strndup((const char *)&resp[1], strlen(want));

    g_assert_cmpstr(seen, ==, want);
}

/*
 * One reply for one request, against the whole reply text: a request built
 * from an explicit byte count, so an empty request and one carrying a NUL
 * can be sent.
 */
static void assert_reply_bytes(QTestState *qts, const char *text,
                               unsigned text_len, const char *want)
{
    uint8_t req[RESP_CAP], resp[RESP_CAP];
    unsigned resp_len;

    req[0] = text_len;
    memcpy(&req[1], text, text_len);
    req[text_len + 1] = (uint8_t)(-(int)frame_checksum(req, text_len + 1));
    resp_len = elsc_transact(qts, req, text_len + 2, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpmem(&resp[1], resp[0], want, strlen(want));
}

static void assert_reply(QTestState *qts, const char *text, const char *want)
{
    assert_reply_bytes(qts, text, strlen(text), want);
}

static void test_ver_command_succeeds(const void *data)
{
    uint8_t req[8], resp[RESP_CAP];
    const uint8_t *version_end;
    size_t version_len;
    unsigned req_len, resp_len;

    elsc_reset();

    req_len = build_request(req, "ver");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));

    g_assert_cmpuint(resp_len, ==, (unsigned)resp[0] + 2);
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    /* Succeeding takes more than a valid checksum: the text begins "ok". */
    g_assert_cmpuint(resp[0], >=, 4);
    assert_response_begins(resp, "ok ");

    /*
     * Keep the synthetic identity and terminator within the eight-byte
     * compatibility limit.
     */
    version_end = memchr(&resp[4], ' ', resp[0] - 3);
    version_len = version_end ? version_end - &resp[4] : resp[0] - 3;
    g_assert_cmpuint(version_len, >, 0);
    g_assert_cmpuint(version_len + 1, <=, 8);
}

static void test_bad_request_checksum_gets_err_csum(const void *data)
{
    uint8_t req[8], resp[RESP_CAP];
    unsigned req_len, resp_len;

    elsc_reset();

    req_len = build_request(req, "ver");
    req[req_len - 1] ^= 0xff;  /* corrupt only the checksum byte */

    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));

    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 8);
    assert_response_begins(resp, "err csum");
}

static void test_unimplemented_command_gets_err_cmd(const void *data)
{
    uint8_t req[8], resp[RESP_CAP];
    unsigned req_len, resp_len;

    elsc_reset();

    /*
     * An unimplemented mnemonic must complete the transaction rather than
     * stall the bus; the console-candidate loop depends on that.
     */
    req_len = build_request(req, "xyz");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));

    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    assert_response_begins(resp, "err cmd");
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
    assert_response_begins(resp, "ok ff");
    qtest_quit(qts);

    qts = qtest_init("-M origin200,elsc-dip-switches=1");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));

    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 5);
    assert_response_begins(resp, "ok fe");
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
    assert_response_begins(resp, "ok ef");
    qtest_quit(qts);

    qts = qtest_init(
        "-M origin200,topology=origin200-dual -m 128M "
        "-smp 4 "
        "-machine elsc-dip-switches=16");

    resp_len = elsc_transact_at(qts, HUB_LOCAL, req, req_len, resp,
                                sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 5);
    assert_response_begins(resp, "ok ef");

    /* Node 1's own Hub must know its number before anything can name it. */
    sn0_program_nasid(qts, 1);

    resp_len = elsc_transact_at(qts, HUB_BY_NODE + NODE_STRIDE, req, req_len,
                                resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 5);
    assert_response_begins(resp, "ok ef");

    qtest_quit(qts);
}

/*
 * Processor code 0's signal pair, the one code with a real input queue.
 * The wait helpers treat every other code's pulse as "not mine", as
 * firmware does for time slices that are not its own.
 */
#define ARB_SIGNAL1 0xe0
#define ARB_SIGNAL2 0xe8

/*
 * The controller publishes one grant per one-millisecond tick; a fine step
 * anchors a test within ARB_STEP_NS after a tick.
 */
#define ARB_STEP_NS       (10 * 1000)
#define ARB_TICK_NS       (1000 * 1000)
#define ARB_MAX_STEPS     2000

/* A single-module controller's ring length. */
#define ARB_SINGLE_MODULE_POSITIONS 5

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

static void cpu_writeq(QTestState *qts, unsigned int cpu, uint64_t addr,
                       uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, cpu, addr, bytes, sizeof(bytes)));
}

static uint64_t cpu_readq(QTestState *qts, unsigned int cpu, uint64_t addr)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, cpu, addr, bytes, sizeof(bytes)));
    return ldq_be_p(bytes);
}

static void arb_enable_cpu_observer(QTestState *qts, unsigned int cpu,
                                    uint8_t clock)
{
    uint64_t hub = HUB_LOCAL;

    cpu_writeq(qts, cpu, hub + I2C_CTRL, CTL_PIN);
    cpu_writeq(qts, cpu, hub + I2C_DATA, 0x7f);
    cpu_writeq(qts, cpu, hub + I2C_CTRL, CTL_PIN | CTL_ES1);
    cpu_writeq(qts, cpu, hub + I2C_DATA, clock);
    cpu_writeq(qts, cpu, hub + I2C_CTRL, CTL_PIN);
    g_assert_cmphex(cpu_readq(qts, cpu, hub + I2C_DATA), ==, 0x7f);
    cpu_writeq(qts, cpu, hub + I2C_DATA, 0x09);
    cpu_writeq(qts, cpu, hub + I2C_CTRL, CTL_PIN | CTL_ESO);
}

static void elsc_request_without_read_cpu(QTestState *qts, unsigned int cpu,
                                          uint64_t hub, const char *text)
{
    uint8_t req[8];
    unsigned int req_len, i;

    req_len = build_request(req, text);
    cpu_writeq(qts, cpu, hub + I2C_DATA, ELSC_ADDR << 1);
    cpu_writeq(qts, cpu, hub + I2C_CTRL,
               CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(cpu_readq(qts, cpu, hub + I2C_CTRL) & I2C_PCF_LRB,
                    ==, 0);
    for (i = 0; i < req_len; i++) {
        cpu_writeq(qts, cpu, hub + I2C_DATA, req[i]);
    }
    cpu_writeq(qts, cpu, hub + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    cpu_writeq(qts, cpu, hub + I2C_DATA, (ELSC_ADDR << 1) | 1);
    cpu_writeq(qts, cpu, hub + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);
}

static unsigned int elsc_read_response_cpu(QTestState *qts,
                                            unsigned int cpu, uint64_t hub,
                                            uint8_t *resp,
                                            unsigned int resp_cap)
{
    unsigned int i, total;

    cpu_writeq(qts, cpu, hub + I2C_DATA, (ELSC_ADDR << 1) | 1);
    cpu_writeq(qts, cpu, hub + I2C_CTRL,
               CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(cpu_readq(qts, cpu, hub + I2C_CTRL) & I2C_PCF_LRB,
                    ==, 0);
    (void)cpu_readq(qts, cpu, hub + I2C_DATA);

    resp[0] = cpu_readq(qts, cpu, hub + I2C_DATA);
    total = (unsigned int)resp[0] + 2;
    g_assert_cmpuint(total, <=, resp_cap);
    for (i = 1; i < total; i++) {
        if (i == total - 1) {
            cpu_writeq(qts, cpu, hub + I2C_CTRL, CTL_ESO);
        }
        resp[i] = cpu_readq(qts, cpu, hub + I2C_DATA);
    }
    cpu_writeq(qts, cpu, hub + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);

    return total;
}

/* Any code's grant, either half of its signal pair. */
static bool arb_is_signal_byte(uint64_t value)
{
    return value >= 0xe0 && value <= 0xef;
}

/* The single-module ring position n ticks after a published grant. */
static unsigned int arb_code_after(uint8_t grant, unsigned int n)
{
    return ((grant & 7) + n) % ARB_SINGLE_MODULE_POSITIONS;
}

/*
 * Step the clock until the ring publishes a code different from the one
 * currently latched, and return the new grant.
 */
static uint8_t arb_next_code(QTestState *qts)
{
    uint64_t start = read_data(qts);
    int i;

    for (i = 0; i < ARB_MAX_STEPS; i++) {
        uint64_t data = read_data(qts);

        if (arb_is_signal_byte(data) && data != start) {
            return (uint8_t)data;
        }
        qtest_clock_step(qts, ARB_SCAN_STEP_NS);
    }
    g_assert_not_reached();
    return 0;
}

/* As arb_next_code, reading one module window through a processor's
 * mapping. */
static uint8_t arb_next_code_at(QTestState *qts, unsigned int cpu, uint64_t hub)
{
    uint64_t start = cpu_readq(qts, cpu, hub + I2C_DATA);
    int i;

    for (i = 0; i < ARB_MAX_STEPS; i++) {
        uint64_t data = cpu_readq(qts, cpu, hub + I2C_DATA);

        if (arb_is_signal_byte(data) && data != start) {
            return (uint8_t)data;
        }
        qtest_clock_step(qts, ARB_SCAN_STEP_NS);
    }
    g_assert_not_reached();
    return 0;
}

/*
 * Both module windows observe one master ring: they publish the same code,
 * in sequence, and the fifth position is neither of the first four codes.
 */
static void assert_shared_five_position_ring(QTestState *qts,
                                             unsigned int master_cpu,
                                             uint64_t master_hub,
                                             unsigned int slave_cpu,
                                             uint64_t slave_hub)
{
    uint8_t master_grant, fifth;
    unsigned int position, i;

    /* Anchor both windows on code 0's signal one. */
    for (position = 0; position < 6; position++) {
        if (cpu_readq(qts, master_cpu, master_hub + I2C_DATA) ==
                ARB_SIGNAL1 &&
            cpu_readq(qts, slave_cpu, slave_hub + I2C_DATA) ==
                ARB_SIGNAL1) {
            break;
        }
        (void)arb_next_code_at(qts, master_cpu, master_hub);
    }
    g_assert_cmpuint(position, <, 6);

    for (position = 1; position < 4; position++) {
        master_grant = arb_next_code_at(qts, master_cpu, master_hub);
        g_assert_cmphex(master_grant, ==, ARB_SIGNAL1 + position);
        g_assert_cmphex(cpu_readq(qts, slave_cpu, slave_hub + I2C_DATA),
                        ==, ARB_SIGNAL1 + position);
    }

    fifth = arb_next_code_at(qts, master_cpu, master_hub);
    g_assert_cmphex(cpu_readq(qts, slave_cpu, slave_hub + I2C_DATA),
                    ==, fifth);
    for (i = 0; i < 4; i++) {
        g_assert_cmphex(fifth, !=, ARB_SIGNAL1 + i);
    }

    /* The ring wraps from the fifth position back to code 0. */
    master_grant = arb_next_code_at(qts, master_cpu, master_hub);
    g_assert_cmphex(master_grant, ==, ARB_SIGNAL1);
    g_assert_cmphex(cpu_readq(qts, slave_cpu, slave_hub + I2C_DATA),
                    ==, ARB_SIGNAL1);
}

/*
 * Loop 1: wait for another processor's grant. Skipping straight to "wait for
 * the signal" passes a model that parks code 0 and never advances. A grant
 * stays readable until the next tick, so a scan finer than the tick catches
 * every code.
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
        qtest_clock_step(qts, ARB_SCAN_STEP_NS);
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
 * A grant is an unmatched address phase, not a payload byte. Its completed
 * address replaces readable S0 while START retains its staged ELSC address.
 */
static void test_arbitration_observes_grant_with_staged_start(const void *data)
{
    uint8_t req[8], resp[RESP_CAP];
    uint8_t signal, grant;
    unsigned req_len, resp_len, i;

    elsc_reset();

    arb_enable_observer(qts);

    arb_wait_for_absence(qts);
    signal = arb_wait_for_signal(qts);

    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, ELSC_ADDR << 1);
    grant = arb_next_code(qts);

    g_assert_true((read_status(qts) & I2C_PCF_BB) != 0);
    g_assert_cmpuint(grant & 7, ==, arb_code_after(signal, 1));
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
    assert_response_begins(resp, "ok");

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
    grant = arb_next_code(qts);
    g_assert_cmpuint(grant & 7, ==, arb_code_after(signal, 1));
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(read_status(qts) & I2C_PCF_LRB, ==, 0);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STO);
    arb_wait_for_absence(qts);
    arb_wait_for_signal(qts);
}

/*
 * Step finely until a new grant appears, leaving the clock no more than
 * ARB_STEP_NS past the tick that published it, and return that grant.
 */
static uint8_t arb_anchor_on_tick_at(QTestState *qts, unsigned int cpu,
                                     uint64_t hub)
{
    uint64_t start = cpu_readq(qts, cpu, hub + I2C_DATA);
    int i;

    for (i = 0; i < 32 * ARB_TICK_NS / ARB_STEP_NS; i++) {
        uint64_t data;

        qtest_clock_step(qts, ARB_STEP_NS);
        data = cpu_readq(qts, cpu, hub + I2C_DATA);
        if (arb_is_signal_byte(data) && data != start) {
            return (uint8_t)data;
        }
    }
    g_assert_not_reached();
    return 0;
}

/*
 * From a point just past one tick, the current grant stands until the next
 * tick one millisecond later, which then publishes the expected grant.
 */
static void arb_assert_next_tick_at(QTestState *qts, unsigned int cpu,
                                    uint64_t hub, uint8_t current,
                                    uint8_t expected)
{
    qtest_clock_step(qts, ARB_TICK_NS - 2 * ARB_STEP_NS);
    g_assert_cmphex(cpu_readq(qts, cpu, hub + I2C_DATA), ==, current);
    qtest_clock_step(qts, 2 * ARB_STEP_NS);
    g_assert_cmphex(cpu_readq(qts, cpu, hub + I2C_DATA), ==, expected);
}

/*
 * Each position lasts exactly one one-millisecond tick: the grant changes on
 * every tick, never sooner and never later, and no position is lengthened.
 */
static void test_arbitration_tick_is_one_millisecond(const void *data)
{
    uint8_t grant;
    unsigned int i;

    elsc_reset();
    arb_enable_observer(qts);

    grant = arb_anchor_on_tick_at(qts, 0, HUB_LOCAL);
    for (i = 0; i < 2 * ARB_SINGLE_MODULE_POSITIONS; i++) {
        uint8_t next = ARB_SIGNAL1 +
                       ((grant & 7) + 1) % ARB_SINGLE_MODULE_POSITIONS;

        arb_assert_next_tick_at(qts, 0, HUB_LOCAL, grant, next);
        grant = next;
    }
}

/*
 * A tick that finds the bus busy publishes nothing and does not advance. The
 * controller then waits for its next periodic tick; a STOP does not publish.
 */
static void test_arbitration_busy_tick_does_not_advance(const void *data)
{
    uint8_t signal;

    elsc_reset();

    arb_enable_observer(qts);
    signal = arb_anchor_on_tick_at(qts, 0, HUB_LOCAL);

    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, ELSC_ADDR << 1);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(read_status(qts) & I2C_PCF_LRB, ==, 0);
    g_assert_cmphex(read_status(qts) & I2C_PCF_BB, ==, 0);

    /* Three eligible ticks pass while the bus is busy. */
    qtest_clock_step(qts, 3 * ARB_TICK_NS + ARB_TICK_NS / 2);
    g_assert_cmphex(read_status(qts) & I2C_PCF_BB, ==, 0);

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STO);
    g_assert_true((read_status(qts) & I2C_PCF_BB) != 0);

    /* The bus is free, but nothing is published before the next tick. */
    qtest_clock_step(qts, ARB_TICK_NS / 2 - 2 * ARB_STEP_NS);
    g_assert_false(arb_is_signal_byte(read_data(qts)));

    /* That tick publishes the code the busy ticks skipped. */
    qtest_clock_step(qts, 2 * ARB_STEP_NS);
    g_assert_cmpuint(read_data(qts) & 7, ==, arb_code_after(signal, 1));
    g_assert_cmpuint(arb_next_code(qts) & 7, ==, arb_code_after(signal, 2));
}

/*
 * A request completed through the direction switch, then abandoned: the
 * transaction ends with the response entirely unread. Not even the
 * discard read runs. This is the window the firmware's slave bring-up
 * waits out: request issued, response pending, the processor parked on
 * its own signal 2 until the controller announces it.
 */
static void elsc_request_without_read(QTestState *qts, const char *text)
{
    uint8_t req[8];
    unsigned req_len, i;

    req_len = build_request(req, text);

    /* Enable the interface: a reset leaves it off, so nothing else may. */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);
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
 * firmware's slave bring-up does exactly that, observed live as a
 * two-node launch parked on a signal 2 no build had ever emitted. A
 * qtest carries no processor context, so the requester attributes as
 * the console processor and the announcement lands in code 0's slot.
 */
static void test_pending_response_announces_signal2(const void *data)
{
    g_autofree char *out = NULL;

    elsc_reset();

    arb_enable_observer(qts);

    elsc_request_without_read(qts, "ver");

    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL2);

    out = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(out, "pending response"));
    g_assert_nonnull(strstr(out, "for processor code 0"));
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
    uint8_t req[8], resp[RESP_CAP];
    unsigned req_len;

    elsc_reset();

    arb_enable_observer(qts);

    req_len = build_request(req, "ver");
    elsc_transact(qts, req, req_len, resp, sizeof(resp));

    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL1);
}

/*
 * The fetch the announcement promises: after the requester's transaction
 * ended unread, the response survives the controller's grant publications,
 * and a read-only
 * transaction returns the frame. Once consumed, the announcement stops.
 */
static void test_announced_response_read_after_stop(const void *data)
{
    uint8_t resp[RESP_CAP];
    unsigned resp_len;

    elsc_reset();

    arb_enable_observer(qts);

    elsc_request_without_read(qts, "ver");

    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL2);

    resp_len = elsc_read_response(qts, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], >=, 4);
    assert_response_begins(resp, "ok ");

    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL1);
}

/*
 * A new request supersedes an unfetched response: the frame is replaced
 * and the old announcement must not outlive it. The superseding
 * command's own response is consumed in full, so nothing is left
 * waiting and the slot reverts to signal 1.
 */
static void test_new_request_supersedes_pending_response(const void *data)
{
    uint8_t req[8], resp[RESP_CAP];
    unsigned req_len, resp_len;

    elsc_reset();

    arb_enable_observer(qts);

    elsc_request_without_read(qts, "ver");

    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL2);

    req_len = build_request(req, "rsw");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    assert_response_begins(resp, "ok ff");

    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL1);
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
    assert_response_begins(resp, "ok 2");

    /*
     * Not qtest_qmp_assert_success_ref(): it unwraps the result as a
     * QDict and silently returns NULL for ringbuf-read's bare string,
     * which surfaces later as an unattributed segfault.
     */
    rb = qtest_qmp(
        qts, "{'execute': 'ringbuf-read', "
             "'arguments': {'device': 'aux', 'size': 64}}");
    if (!qdict_haskey(rb, "return")) {
        g_autoptr(GString) text = qobject_to_json(QOBJECT(rb));

        g_error("ringbuf-read failed: %s", text->str);
    }
    captured = qdict_get_str(rb, "return");
    g_assert_cmpstr(captured, ==, "hi");
    qobject_unref(rb);

    qtest_quit(qts);
}

/*
 * Read one AUX ringbuf over QMP. Not
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
    if (!qdict_haskey(rb, "return")) {
        g_autoptr(GString) text = qobject_to_json(QOBJECT(rb));

        g_error("ringbuf-read failed: %s", text->str);
    }
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
        "-M origin200,topology=origin200-dual -m 128M "
        "-smp 4 "
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
    assert_response_begins(resp, "ok 2");

    sn0_program_nasid(qts, 1);

    req_len = build_request(req, "acp 0 n1");
    resp_len = elsc_transact_at(qts, HUB_BY_NODE + NODE_STRIDE, req,
                                req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    assert_response_begins(resp, "ok 2");

    assert_aux_captured(qts, "aux0", "n0");
    assert_aux_captured(qts, "aux1", "");

    qtest_quit(qts);
}

/*
 * The master owns one five-position ring. Before node numbering, the same
 * explicit-zero address reaches the PCF local to each issuing module. After
 * node 1 is numbered, that module reaches the master at explicit zero and its
 * local PCF at explicit one. Distinct clock-register values prove which PCF
 * each access reached while the data registers prove both see one ring.
 */
static void test_two_towers_share_master_grant_ring(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200,topology=origin200-dual -m 128M "
        "-smp 4");
    g_autofree char *out = NULL;
    const unsigned int master_cpu = 0;
    const unsigned int slave_cpu = 2;
    const uint64_t explicit_zero = HUB_BY_NODE;
    const uint64_t explicit_one = HUB_BY_NODE + NODE_STRIDE;
    const char *node0, *node1;
    uint8_t resp[RESP_CAP];
    unsigned int resp_len;
    uint8_t grant;

    arb_enable_cpu_observer(qts, master_cpu, 0x1c);
    arb_enable_cpu_observer(qts, slave_cpu, 0x1d);

    cpu_writeq(qts, master_cpu, explicit_zero + I2C_CTRL,
               CTL_PIN | CTL_ES1);
    cpu_writeq(qts, slave_cpu, explicit_zero + I2C_CTRL,
               CTL_PIN | CTL_ES1);
    g_assert_cmphex(cpu_readq(qts, master_cpu, explicit_zero + I2C_DATA),
                    ==, 0x1c);
    g_assert_cmphex(cpu_readq(qts, slave_cpu, explicit_zero + I2C_DATA),
                    ==, 0x1d);
    cpu_writeq(qts, master_cpu, explicit_zero + I2C_CTRL,
               CTL_PIN | CTL_ESO);
    cpu_writeq(qts, slave_cpu, explicit_zero + I2C_CTRL,
               CTL_PIN | CTL_ESO);

    assert_shared_five_position_ring(qts, master_cpu, explicit_zero,
                                     slave_cpu, explicit_zero);

    sn0_program_nasid(qts, 1);

    cpu_writeq(qts, slave_cpu, explicit_zero + I2C_CTRL,
               CTL_PIN | CTL_ES1);
    cpu_writeq(qts, slave_cpu, explicit_one + I2C_CTRL,
               CTL_PIN | CTL_ES1);
    g_assert_cmphex(cpu_readq(qts, slave_cpu, explicit_zero + I2C_DATA),
                    ==, 0x1c);
    g_assert_cmphex(cpu_readq(qts, slave_cpu, explicit_one + I2C_DATA),
                    ==, 0x1d);
    cpu_writeq(qts, slave_cpu, explicit_zero + I2C_CTRL,
               CTL_PIN | CTL_ESO);
    cpu_writeq(qts, slave_cpu, explicit_one + I2C_CTRL,
               CTL_PIN | CTL_ESO);

    /*
     * A slave-local transaction must stall the one shared ring: across many
     * ticks the frozen ring does not advance.
     */
    cpu_writeq(qts, slave_cpu, explicit_one + I2C_DATA, ELSC_ADDR << 1);
    cpu_writeq(qts, slave_cpu, explicit_one + I2C_CTRL,
               CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    qtest_clock_step(qts, 30 * ARB_TICK_NS);
    g_assert_cmphex(cpu_readq(qts, slave_cpu, explicit_zero + I2C_DATA),
                    ==, ARB_SIGNAL1);

    /* The next tick after STOP publishes the skipped code. */
    cpu_writeq(qts, slave_cpu, explicit_one + I2C_CTRL,
               CTL_PIN | CTL_ESO | CTL_STO);
    qtest_clock_step(qts, ARB_TICK_NS);
    g_assert_cmphex(cpu_readq(qts, slave_cpu, explicit_zero + I2C_DATA),
                    ==, ARB_SIGNAL1 + 1);
    g_assert_cmphex(cpu_readq(qts, slave_cpu, explicit_one + I2C_DATA),
                    ==, ARB_SIGNAL1 + 1);

    /* A slave-owned response selects signal 2 in CPU 2's shared slot. */
    elsc_request_without_read_cpu(qts, slave_cpu, explicit_one, "ver");
    out = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(out, "for processor code 2"));
    do {
        grant = arb_next_code_at(qts, slave_cpu, explicit_zero);
    } while ((grant & 7) != 2);
    g_assert_cmphex(grant, ==, ARB_SIGNAL2 + 2);
    g_assert_cmphex(cpu_readq(qts, slave_cpu, explicit_one + I2C_DATA),
                    ==, ARB_SIGNAL2 + 2);

    resp_len = elsc_read_response_cpu(qts, slave_cpu, explicit_one, resp,
                                      sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    /* With the response consumed, CPU 2's next grant reverts to signal 1. */
    do {
        grant = arb_next_code_at(qts, slave_cpu, explicit_zero);
    } while ((grant & 7) != 2);
    g_assert_cmphex(grant, ==, ARB_SIGNAL1 + 2);
    g_assert_cmphex(cpu_readq(qts, slave_cpu, explicit_one + I2C_DATA),
                    ==, ARB_SIGNAL1 + 2);

    /* Reset restores duplicate NASID zero without moving the master role. */
    qtest_system_reset(qts);
    g_clear_pointer(&out, g_free);
    out = qtest_hmp(qts, "info elsc");
    node0 = strstr(out, "/machine/node0/hub/elsc");
    node1 = strstr(out, "/machine/node1/hub/elsc");
    g_assert_nonnull(node0);
    g_assert_nonnull(node1);
    g_assert_true(node0 < node1);
    g_assert_nonnull(g_strstr_len(node0, node1 - node0,
                                 "next position 0 of 5"));
    g_assert_nonnull(strstr(node1, "next position 0 of 5"));

    arb_enable_cpu_observer(qts, master_cpu, 0x1c);
    arb_enable_cpu_observer(qts, slave_cpu, 0x1d);
    cpu_writeq(qts, master_cpu, explicit_zero + I2C_CTRL,
               CTL_PIN | CTL_ES1);
    cpu_writeq(qts, slave_cpu, explicit_zero + I2C_CTRL,
               CTL_PIN | CTL_ES1);
    g_assert_cmphex(cpu_readq(qts, master_cpu, explicit_zero + I2C_DATA),
                    ==, 0x1c);
    g_assert_cmphex(cpu_readq(qts, slave_cpu, explicit_zero + I2C_DATA),
                    ==, 0x1d);
    cpu_writeq(qts, master_cpu, explicit_zero + I2C_CTRL,
               CTL_PIN | CTL_ESO);
    cpu_writeq(qts, slave_cpu, explicit_zero + I2C_CTRL,
               CTL_PIN | CTL_ESO);
    assert_shared_five_position_ring(qts, master_cpu, explicit_zero,
                                     slave_cpu, explicit_zero);

    sn0_program_nasid(qts, 1);
    cpu_writeq(qts, slave_cpu, explicit_zero + I2C_CTRL,
               CTL_PIN | CTL_ES1);
    cpu_writeq(qts, slave_cpu, explicit_one + I2C_CTRL,
               CTL_PIN | CTL_ES1);
    g_assert_cmphex(cpu_readq(qts, slave_cpu, explicit_zero + I2C_DATA),
                    ==, 0x1c);
    g_assert_cmphex(cpu_readq(qts, slave_cpu, explicit_one + I2C_DATA),
                    ==, 0x1d);

    qtest_quit(qts);
}

/*
 * A system-controller reset can arrive while the other module's PCF owns an
 * open transaction. The reset clears that PCF's registers, but it must also
 * release the I2C core transaction or the shared grant timer sees the peer
 * bus busy forever. Poll status before data as firmware does and require CPU
 * 3's own grant to remain reachable after the reset.
 */
static void test_two_towers_reset_keeps_cpu3_grant_live(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200,topology=origin200-dual -m 128M "
        "-smp 4");
    const unsigned int master_cpu = 0;
    const unsigned int slave_cpu = 3;
    const uint64_t explicit_zero = HUB_BY_NODE;
    uint8_t req[16], resp[RESP_CAP];
    unsigned int req_len, resp_len, i;
    bool saw_cpu3_grant = false;

    arb_enable_cpu_observer(qts, master_cpu, 0x1c);
    arb_enable_cpu_observer(qts, slave_cpu, 0x1d);

    /* Leave the slave module's ELSC selected when the reset arrives. */
    cpu_writeq(qts, slave_cpu, explicit_zero + I2C_DATA, ELSC_ADDR << 1);
    cpu_writeq(qts, slave_cpu, explicit_zero + I2C_CTRL,
               CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(cpu_readq(qts, slave_cpu,
                              explicit_zero + I2C_CTRL) & I2C_PCF_LRB,
                    ==, 0);

    req_len = build_request(req, "rst");
    resp_len = elsc_transact_at(qts, explicit_zero, req, req_len, resp,
                                sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    assert_response_begins(resp, "ok");
    qtest_clock_step(qts, ELSC_RESET_WAIT_NS);
    qtest_qmp_eventwait(qts, "RESET");

    arb_enable_cpu_observer(qts, master_cpu, 0x1c);
    arb_enable_cpu_observer(qts, slave_cpu, 0x1d);

    /* Scan well past a warm reset's resumption and several revolutions. */
    for (i = 0; i < 128 * ARB_TICK_NS / ARB_SCAN_STEP_NS; i++) {
        uint64_t status = cpu_readq(qts, slave_cpu,
                                    explicit_zero + I2C_CTRL);
        uint64_t value = cpu_readq(qts, slave_cpu,
                                   explicit_zero + I2C_DATA);

        if ((status & I2C_PCF_BB) &&
            (value == ARB_SIGNAL1 + 3 || value == ARB_SIGNAL2 + 3)) {
            saw_cpu3_grant = true;
            break;
        }
        qtest_clock_step(qts, ARB_SCAN_STEP_NS);
    }
    g_assert_true(saw_cpu3_grant);

    qtest_quit(qts);
}

/*
 * The shared ring turns in five milliseconds: both module windows see codes 0
 * to 3 and the slave-controller position, one per one-millisecond tick, and
 * code 0 again on the sixth tick.
 */
static void test_two_towers_ring_turns_in_five_ticks(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200,topology=origin200-dual -m 128M "
        "-smp 4");
    const unsigned int master_cpu = 0;
    const unsigned int slave_cpu = 2;
    const uint64_t explicit_zero = HUB_BY_NODE;
    uint8_t grant, fifth;
    unsigned int position;

    arb_enable_cpu_observer(qts, master_cpu, 0x1c);
    arb_enable_cpu_observer(qts, slave_cpu, 0x1d);

    do {
        grant = arb_anchor_on_tick_at(qts, master_cpu, explicit_zero);
    } while (grant != ARB_SIGNAL1);
    g_assert_cmphex(cpu_readq(qts, slave_cpu, explicit_zero + I2C_DATA),
                    ==, ARB_SIGNAL1);

    for (position = 1; position < 4; position++) {
        arb_assert_next_tick_at(qts, master_cpu, explicit_zero, grant,
                                ARB_SIGNAL1 + position);
        grant = ARB_SIGNAL1 + position;
        g_assert_cmphex(cpu_readq(qts, slave_cpu, explicit_zero + I2C_DATA),
                        ==, grant);
    }

    /* The fifth position stands for one tick like the others. */
    qtest_clock_step(qts, ARB_TICK_NS);
    fifth = cpu_readq(qts, master_cpu, explicit_zero + I2C_DATA);
    g_assert_true(arb_is_signal_byte(fifth));
    g_assert_cmphex(fifth & 7, >=, 4);
    g_assert_cmphex(cpu_readq(qts, slave_cpu, explicit_zero + I2C_DATA),
                    ==, fifth);

    qtest_clock_step(qts, ARB_TICK_NS);
    g_assert_cmphex(cpu_readq(qts, master_cpu, explicit_zero + I2C_DATA),
                    ==, ARB_SIGNAL1);
    g_assert_cmphex(cpu_readq(qts, slave_cpu, explicit_zero + I2C_DATA),
                    ==, ARB_SIGNAL1);

    qtest_quit(qts);
}

/* A dark console must not also wedge the guest: "ok <count>" regardless. */
static void test_acp_with_no_backend_still_answers(const void *data)
{
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    elsc_reset();

    req_len = build_request(req, "acp 0 hi");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));

    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    /* "hi" is two characters, accepted count in hex is "2": "ok 2". */
    g_assert_cmpuint(resp[0], ==, 4);
    assert_response_begins(resp, "ok 2");
}

/*
 * Console input needs a live connection so bytes reach the receive
 * callback. A loopback TCP socket stands in for the cable, following the
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
    g_assert_cmpint(lfd, >=, 0);
    g_assert_cmpint(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)),
                    ==, 0);
    g_assert_cmpint(getsockname(lfd, (struct sockaddr *)&addr, &addrlen),
                    ==, 0);
    *port = ntohs(addr.sin_port);
    g_assert_cmpint(listen(lfd, 1), ==, 0);
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
    g_assert_cmpint(fd, >=, 0);
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

    /* Input is announced only once a processor has sent output. */
    assert_reply(qts, "acp 0 ", "ok 0");
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
    assert_response_begins(resp, "acp . hello");

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
    assert_reply(qts, "acp 0 ", "ok 0");
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
    assert_response_begins(resp, "acp . ABCD");

    close(fd);
    close(lfd);
    qtest_quit(qts);
}

/*
 * The controller's supervisor mode belongs to the operator's serial line.
 * Over I2C "pas" is not a command, in any form, and the destructive
 * commands it would gate there run without it: IRIX sends "pwr d" and
 * "rst" and never "pas".
 */
static void test_no_supervisor_on_i2c(const void *data)
{
    const char *pas_forms[] = { "pas", "pas none", "pas ab", "pas s abcd" };
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len, i;

    elsc_reset();

    for (i = 0; i < ARRAY_SIZE(pas_forms); i++) {
        req_len = build_request(req, pas_forms[i]);
        resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
        g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
        g_assert_cmpuint(resp[0], ==, 7);
        assert_response_begins(resp, "err cmd");
    }

    /* A destructive command with no "pas" ever sent. */
    req_len = build_request(req, "rst");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 3);
    assert_response_begins(resp, "ok");
    qtest_clock_step(qts, ELSC_RESET_WAIT_NS);
    qtest_qmp_eventwait(qts, "RESET");
}

/*
 * The Origin 200 build of the controller has no front-panel display: "dsc"
 * and "dsp" are not in its command table and answer "err cmd". The PROM and
 * kernel send both on every machine and ignore the reply, so the refusal is
 * harmless. Its version field is its own.
 */
static void test_origin200_build_commands(const void *data)
{
    const char *display[] = { "dsc 4 E", "dsc 1 A", "dsp testing", "dsp",
                              "dsc" };
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len, i;
    g_autofree char *out = NULL;

    elsc_reset();

    for (i = 0; i < ARRAY_SIZE(display); i++) {
        req_len = build_request(req, display[i]);
        resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
        g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
        g_assert_cmpuint(resp[0], ==, 7);
        assert_response_begins(resp, "err cmd");
    }

    req_len = build_request(req, "ver");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 11);
    g_assert_cmpmem(&resp[1], 11, "ok V 3.1S  ", 11);

    /* Nothing to show: "info elsc" prints no display for this build. */
    out = qtest_hmp(qts, "info elsc");
    g_assert_null(strstr(out, "display:"));
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
 * transaction, then the closing repeated start inside the same transfer
 * to the invalid address, which must NAK, then the stop.
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
 * Firmware acceptance requires sixteen identity-EEPROM reads, starting with
 * marker 0x37, then rsw and a ver reply containing checksum-valid "ok".
 * Arbitrate and close each transaction with the invalid-address step.
 * The parser accepts "ok" only before a space or terminator.
 */
static void test_boot_acceptance_sequence(const void *data)
{
    static const uint8_t initial_block[16] = {
        0x37, 'n', 'o', 'n', 'e', 0, 0, 0x02, 0, 0, 0, 0x78, 0, 0, 0, 0,
    };
    uint8_t req[8], resp[RESP_CAP];
    uint8_t nvram[16];
    unsigned req_len, resp_len, i;

    elsc_reset();

    arb_enable_observer(qts);

    for (i = 0; i < ARRAY_SIZE(nvram); i++) {
        nvram[i] = firmware_style_nvram_read(qts, i);
    }
    /*
     * The controller's block as its first power-up leaves it: the marker,
     * the password "none", automatic power-up, no module number and the
     * full-clock midplane's clock-select byte.
     */
    g_assert_cmpmem(nvram, sizeof(nvram), initial_block,
                    sizeof(initial_block));

    req_len = build_request(req, "rsw");
    resp_len = firmware_style_command(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 5);
    assert_response_begins(resp, "ok ff");

    req_len = build_request(req, "ver");
    resp_len = firmware_style_command(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], >=, 2);
    assert_response_begins(resp, "ok");
    /* Either a bare "ok" or "ok " and a version; asserting one prints it. */
    if (resp[0] != 2) {
        g_assert_cmpint(resp[3], ==, ' ');
    }
}

/*
 * With nothing typed, "info elsc" must name the empty-queue state
 * (signal1, which withholds "get") rather than claim input is waiting.
 */
static void test_info_elsc_dump_empty(const void *data)
{
    g_autofree char *out = NULL;

    elsc_reset();

    out = qtest_hmp(qts, "info elsc");

    g_assert_nonnull(strstr(out, "rx queue: 0 byte(s)"));
    g_assert_nonnull(strstr(out, "pending response: none"));
    g_assert_nonnull(strstr(out, "signal1"));
    g_assert_nonnull(strstr(out, "console input idle"));
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

    /* Input is announced only once a processor has sent output. */
    assert_reply(qts, "acp 0 ", "ok 0");
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
 * a cold start (firmware command contract). The
 * controller answers "ok 1" on the first read in a power cycle and "ok 0"
 * afterwards, clearing the latch on read. (The clear-on-read one-shot is
 * from the 6.5.7 driver and unverified on 6.5.30.)
 */
static void test_pwr_query_reports_cold_power_on(const void *data)
{
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    elsc_reset();

    /* First query after a cold power-on: the latch reads 1. */
    req_len = build_request(req, "pwr q");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    assert_response_begins(resp, "ok 1");

    /* Cleared on read: a second query in the same power cycle reads 0. */
    req_len = build_request(req, "pwr q");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    assert_response_begins(resp, "ok 0");

    /* A letter the controller does not know is a bad argument. */
    req_len = build_request(req, "pwr z");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    assert_response_begins(resp, "err arg");

    /* Trailing text after the letter is discarded. */
    req_len = build_request(req, "pwr q x");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    assert_response_begins(resp, "ok 0");
}

/*
 * "hbt" is the heartbeat / NMI-watchdog control. "hbt" alone sends one
 * heartbeat and "hbt 0" disables monitoring. The firmware reads both
 * arguments as hex: the interval is 5 to 0x258 seconds and the optional
 * second argument is the wait from the NMI to the power cycle. The kernel's
 * own "hbt 30 20" is therefore a 0x30-second interval and is accepted, as
 * a decimal reading would also accept it, while "hbt 1000 5" is refused,
 * which only the hex reading does.
 */
static void test_hbt_watchdog_control(const void *data)
{
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;
    /*
     * The hex reader takes no digits from "xy" and so reads no value: a
     * heartbeat, not a bad argument. The same goes for a second value that
     * is not hex. "hbt 00045" is the four digits 0004 and so too short.
     */
    const char *ok_forms[] = { "hbt", "hbt 0", "hbt 30 20", "hbt 5", "hbt 258",
                               "hbt 1e 0", "hbt xy", "hbt 30 xy",
                               "hbt 1e  14", "hbt 025814" };
    const char *bad_forms[] = { "hbt 4", "hbt 259", "hbt 1000 5",
                                "hbt 00045" };
    unsigned i;

    elsc_reset();

    for (i = 0; i < ARRAY_SIZE(ok_forms); i++) {
        req_len = build_request(req, ok_forms[i]);
        resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
        g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
        g_assert_cmpuint(resp[0], ==, 3);
        assert_response_begins(resp, "ok");
    }

    for (i = 0; i < ARRAY_SIZE(bad_forms); i++) {
        req_len = build_request(req, bad_forms[i]);
        resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
        g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
        g_assert_cmpuint(resp[0], ==, 7);
        assert_response_begins(resp, "err arg");
    }

    /* Junk after the mnemonic with no delimiter is an unknown command. */
    req_len = build_request(req, "hbtx");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    assert_response_begins(resp, "err cmd");
}

/*
 * "rst" is the controller's module reset. It performs a real machine
 * reset, not the no-op the model
 * used to answer. A panic reboot treats "ok" as "the reset happened" and
 * does not fall back to its own hardware reset, so the reset must actually
 * fire; this asserts QEMU's RESET event, the way sn0-hub-test checks a
 * Hub-driven reset, and that the reset cleared controller state.
 */
static void test_rst_performs_machine_reset(const void *data)
{
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    elsc_reset();

    /* Leave a mark the reset must clear: take the test-and-set lock. */
    req_len = build_request(req, "tas A");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    assert_response_begins(resp, "ok 0");

    /* "rst" answers ok and the reset really fires. */
    req_len = build_request(req, "rst");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 3);
    assert_response_begins(resp, "ok");

    /* The reset actually happened, not just an "ok". */
    qtest_clock_step(qts, ELSC_RESET_WAIT_NS);
    qtest_qmp_eventwait(qts, "RESET");

    /* And it cleared controller state: the lock is free again. */
    req_len = build_request(req, "tas B");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    assert_response_begins(resp, "ok 0");
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
    uint8_t resp[RESP_CAP];
    unsigned resp_len;

    elsc_reset();

    /* Write "rst" and end the transaction without reading the response. */
    elsc_request_without_read(qts, "rst");

    /* The ack survives to be fetched in a separate read transaction. */
    resp_len = elsc_read_response(qts, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 3);
    assert_response_begins(resp, "ok");

    /* Only now, once the ack has been read, does the reset fire. */
    qtest_clock_step(qts, ELSC_RESET_WAIT_NS);
    qtest_qmp_eventwait(qts, "RESET");
}

/*
 * The deferred reset is tied to the specific "rst" ack. If the ack is left
 * unread and a new request supersedes the pending response, the reset is
 * dropped. Otherwise consuming the new, unrelated response would fire it.
 * A held test-and-set lock survives, proving no reset ran.
 */
static void test_rst_reset_dropped_on_supersession(const void *data)
{
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    elsc_reset();

    req_len = build_request(req, "tas A");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    assert_response_begins(resp, "ok 0");

    /* Issue "rst" but leave its ack unread. */
    elsc_request_without_read(qts, "rst");

    /* A new request supersedes the ack; its own response reads back clean. */
    req_len = build_request(req, "ver");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    assert_response_begins(resp, "ok ");

    /* No reset ran: the lock is still held. */
    req_len = build_request(req, "tas B");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    assert_response_begins(resp, "ok A");
}

/*
 * The reset fires when the ack is fully read, not at a terminal bus event.
 * A caller that reads the whole ack and then NACKs and issues a same-address
 * repeated START (rather than STOP) gets no I2C finish, yet the reset must
 * still fire. This reads the whole ack and issues no STOP at all, then
 * expects the RESET event, proving the firing is tied to consumption.
 */
static void test_rst_reset_fires_without_stop(const void *data)
{
    uint8_t resp[RESP_CAP];
    unsigned i, total;

    elsc_reset();

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
    assert_response_begins(resp, "ok");

    /* Reading the last ack byte fired the reset; no STOP was issued. */
    qtest_clock_step(qts, ELSC_RESET_WAIT_NS);
    qtest_qmp_eventwait(qts, "RESET");
}

/*
 * NACK on the last ack text byte, before checksum, then same-address repeated
 * START to an unrelated request without STOP. I2C_NACK arrives while
 * txpos < txlen, before checksum fetch; I2C_FINISH never arrives. A commit on
 * either event loses the reset. The new request makes elsc_request_complete
 * cancel the ARMED action although the guest already read "ok". Fetching
 * the final text byte advances txpos to txlen and commits before that NACK.
 */
static void test_rst_reset_survives_last_text_byte_nack(const void *data)
{
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, i;

    elsc_reset();

    /* Issue "rst"; the ack is left pending (ARMED). */
    elsc_request_without_read(qts, "rst");

    /*
     * Read the ack up to and including the last text byte, NACKing it. The
     * frame is len,'o','k',' ',checksum: the space is the last text byte, and
     * the read that returns it advances txpos to txlen. The checksum is left
     * unread.
     */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, (ELSC_ADDR << 1) | 1);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(read_status(qts) & I2C_PCF_LRB, ==, 0);
    (void)qtest_readq(qts, HUB_LOCAL + I2C_DATA);       /* discard stale */
    resp[0] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);   /* length */
    g_assert_cmpuint(resp[0], ==, 3);
    resp[1] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);   /* 'o' */
    resp[2] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);   /* 'k' */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO);   /* NAK next byte */
    resp[3] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);   /* ' ' */
    /* The whole ack was delivered, though the checksum stays unread. */
    assert_response_begins(resp, "ok");

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
     * The reset committed on the last text-byte fetch fires; the unrelated
     * request did not cancel it.
     */
    qtest_clock_step(qts, ELSC_RESET_WAIT_NS);
    qtest_qmp_eventwait(qts, "RESET");
}

/*
 * A "rst" whose ack the master ends by NACKing the last byte with no STOP
 * leaves this controller matched on the I2C bus: the STOP that would call
 * i2c_end_transfer never comes. The machine reset the "rst" triggers clears
 * the PCF8584's own transfer state but not the bus's slave match, so unless
 * the reset also releases the bus the first post-reset transaction is
 * misrouted to this controller. An identity-EEPROM read at page 7 (address
 * 0x57) is answered by the ELSC instead, its first byte an ELSC response
 * length rather than the EEPROM's 0x37 marker. This drives that exact
 * sequence and reads page 7 directly (without arbitration: the stale
 * match also
 * wedges the bus permanently busy, which stalls the grant rotation)
 * and requires the EEPROM's own byte back.
 */
static void test_rst_reset_releases_i2c_bus(const void *data)
{
    uint8_t resp[RESP_CAP];
    unsigned i, total;
    uint8_t page7_byte0;

    elsc_reset();

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
    assert_response_begins(resp, "ok");

    /* Reading the last ack byte fired the reset. */
    qtest_clock_step(qts, ELSC_RESET_WAIT_NS);
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
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0);   /* byte address in page */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, (ELSC_NVRAM_PAGE7_ADDR << 1) | 1);
    (void)qtest_readq(qts, HUB_LOCAL + I2C_DATA);   /* discard stale */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO);
    page7_byte0 = qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0xfe);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);

    g_assert_cmphex(page7_byte0, ==, 0x37);
}

/*
 * A "rst" accepted but not yet acknowledged is ARMED: its migrated state
 * carries what to do and that the ack is still unread. Migrating in that
 * window must NOT fire the reset on the destination. post_load re-arms the
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
    uint8_t resp[RESP_CAP];
    unsigned resp_len;

    tmpfs = g_dir_make_tmp("sn0-elsc-test-XXXXXX", &err);
    g_assert_nonnull(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init("-M origin200");

    /* Accept "rst" but leave its ack unread: the reset is ARMED, not
     * fired. */
    elsc_request_without_read(from, "rst");

    dest_cmdline = g_strdup_printf("-M origin200 -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    sgi_migrate_stopped(from, uri);

    /*
     * The ARMED reset survived migration without firing early: the ack is
     * still there to read, and reading it (its terminal STOP) commits and
     * fires the reset on the destination.
     */
    resp_len = elsc_read_response(to, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 3);
    assert_response_begins(resp, "ok");

    qtest_clock_step(to, ELSC_RESET_WAIT_NS);
    qtest_qmp_eventwait(to, "RESET");

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * Migrate after the last text byte commits the reset but before the guest
 * reads the checksum or closes the response. The source has not scheduled
 * the bottom half, so deferred_scheduled is clear; post_load must leave it
 * unscheduled until the destination STOP closes the frame. A post_load keyed
 * on committed instead resets PCF8584 while checksum sits in read-ahead;
 * the reset value breaks the checksum. Read through "k" on the source, then
 * verify the intact checksum on the destination before STOP and RESET.
 */
static void test_rst_reset_open_frame_defers_across_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint8_t resp[RESP_CAP];

    tmpfs = g_dir_make_tmp("sn0-elsc-test-XXXXXX", &err);
    g_assert_nonnull(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init("-M origin200");

    /*
     * Accept "rst" (ARMED), then open a read and take len, 'o', 'k', ' '
     * (the last text byte, whose fetch commits the reset). The
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
    g_assert_cmpuint(resp[0], ==, 3);
    resp[1] = qtest_readq(from, HUB_LOCAL + I2C_DATA);   /* 'o' */
    resp[2] = qtest_readq(from, HUB_LOCAL + I2C_DATA);   /* 'k' */
    resp[3] = qtest_readq(from, HUB_LOCAL + I2C_DATA);   /* ' ', commits */
    assert_response_begins(resp, "ok");

    dest_cmdline = g_strdup_printf("-M origin200 -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    sgi_migrate_stopped(from, uri);

    /*
     * On the destination the open frame is intact: the checksum reads back
     * its migrated value (post_load did not reset the chip out from under
     * it), and the whole frame still sums to zero.
     */
    resp[4] = qtest_readq(to, HUB_LOCAL + I2C_DATA);     /* checksum */
    qtest_writeq(to, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);
    g_assert_cmphex(frame_checksum(resp, 5), ==, 0);

    /* The closing STOP is the boundary that carries out the committed
     * reset. */
    qtest_clock_step(to, ELSC_RESET_WAIT_NS);
    qtest_qmp_eventwait(to, "RESET");

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * "tas <ch>" / "tas 0" is the test-and-set lock cell (firmware command
 * contract), driven by the 6.5.30 firmware in its arbitration path. It is a
 * distinct byte, not the bus arbitration. <ch> is a LITERAL character: the
 * client compares the owner byte for byte, so the response echoes the exact
 * byte held before the request, and any single non-'0' character is a valid
 * channel. Acquiring a free cell reads back "ok 0"; a caller polls until it
 * reads 0 (was free) or its own byte. ASCII '0' releases. (Literal-byte
 * round-trip; the ambiguous "hex" description is flagged for the source
 * owner, and the pre-write value is 6.5.7-derived, unverified on 6.5.30.)
 */
static void test_tas_lock_cell(const void *data)
{
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    elsc_reset();

    /* Free cell: the pre-acquisition value is 0, then 'A' installs. */
    req_len = build_request(req, "tas A");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    assert_response_begins(resp, "ok 0");

    /* Re-acquiring returns the literal owner: "ok A", not "ok a". */
    req_len = build_request(req, "tas A");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    assert_response_begins(resp, "ok A");

    /* Held by 'A': a different caller reads the holder, not its own byte. */
    req_len = build_request(req, "tas z");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    assert_response_begins(resp, "ok A");

    /* Release: the pre-release value is the holder, and the cell frees. */
    req_len = build_request(req, "tas 0");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    assert_response_begins(resp, "ok A");

    /* 'z' is a valid non-hex channel: the free cell reads 0 and installs
     * it. */
    req_len = build_request(req, "tas z");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    assert_response_begins(resp, "ok 0");

    /* And it reads back as itself. */
    req_len = build_request(req, "tas z");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    assert_response_begins(resp, "ok z");

    /* No argument only reports the holder. */
    assert_reply(qts, "tas", "ok z");
    assert_reply(qts, "tas 0", "ok z");
    assert_reply(qts, "tas", "ok 0");

    /*
     * The owner is the next non-space character and the rest is discarded,
     * so "tas  AB" offers 'A'. Another caller's "tas B" then reads 'A' and
     * leaves it holding.
     */
    assert_reply(qts, "tas  AB", "ok 0");
    assert_reply(qts, "tas B", "ok A");
    assert_reply(qts, "tas", "ok A");
    assert_reply(qts, "tas 0", "ok A");

    /* Junk after the mnemonic with no delimiter is an unknown command. */
    req_len = build_request(req, "tasx");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    assert_response_begins(resp, "err cmd");
}

/*
 * A NUL (0x00) owner is a legal channel and must not be read as the free
 * cell. Acquiring the free cell with owner 0x00 leaves it HELD; a later
 * caller then reads the 0x00 owner back rather than acquiring, and that
 * owner byte round-trips through the response frame. A strlen-framed
 * "ok \0" reply would truncate to "ok ". (firmware command contract; any
 * byte but the release channel '0' is a valid owner.)
 */
static void test_tas_lock_nul_owner(const void *data)
{
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    elsc_reset();

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

    /* Acquire the free cell: the pre-acquisition value is the free
     * sentinel. */
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    assert_response_begins(resp, "ok 0");

    /*
     * A different caller now finds the cell HELD. The buggy free-sentinel
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
    assert_response_begins(resp, "ok 0");
}

/*
 * The remaining power subcommands (firmware command contract), none
 * boot-driven. This covers the refusals, which take no action. The
 * destructive "pwr d" and "pwr c" actions are checked in the shutdown and
 * power-cycle tests below.
 */
static void test_pwr_control_subcommands(const void *data)
{
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    elsc_reset();

    /*
     * A delay above 0x258 is err arg, as is a power-cycle interval outside
     * 5 to 0x258, and so is "pwr r": the redundant-supply query is later
     * firmware's, and neither modelled build has it. None of these arm an
     * action.
     */
    {
        const char *bad_forms[] = { "pwr d 259", "pwr d ffff",
                                    "pwr c 4", "pwr c 259", "pwr r" };
        unsigned i;

        for (i = 0; i < ARRAY_SIZE(bad_forms); i++) {
            req_len = build_request(req, bad_forms[i]);
            resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
            g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
            g_assert_cmpuint(resp[0], ==, 7);
            assert_response_begins(resp, "err arg");
        }
    }
}

/*
 * "pwr d <sec>" answers "ok" and then, once the ack is read,
 * shuts the machine down, verified by QEMU's SHUTDOWN event. "-no-shutdown"
 * keeps QEMU alive in the shutdown state so the event can be observed.
 */
static void test_pwr_down_shuts_down(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -no-shutdown");
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    /* Immediate power-down: "ok", then the shutdown fires. */
    req_len = build_request(req, "pwr d 0");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 3);
    assert_response_begins(resp, "ok");

    qtest_qmp_eventwait(qts, "SHUTDOWN");

    qtest_quit(qts);
}

/*
 * "pwr c" answers "ok" and then, once the ack is read,
 * power-cycles the machine: a machine reset, verified by the RESET event.
 */
static void test_pwr_cycle_resets(const void *data)
{
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    elsc_reset();

    req_len = build_request(req, "pwr c");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 3);
    assert_response_begins(resp, "ok");

    qtest_qmp_eventwait(qts, "RESET");
}

/* Whether QEMU has taken the guest's power-down, under -no-shutdown. */
static bool powered_down(QTestState *qts)
{
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'query-status' }");
    bool down = g_str_equal(qdict_get_str(qdict_get_qdict(rsp, "return"),
                                          "status"), "shutdown");

    qobject_unref(rsp);
    return down;
}

static void pwr_down_request(QTestState *qts, const char *text)
{
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    req_len = build_request(req, text);
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 3);
    assert_response_begins(resp, "ok");
}

/*
 * A "pwr d" delay from 1 through 0x258 seconds is accepted: IRIX sends
 * "pwr d 1" at halt and "pwr d 8" while extending a shutdown. Above 0x258
 * is "err arg". The accepted requests only arm a countdown, which the
 * reset that opens the next case clears.
 */
static void test_pwr_down_range_checked(const void *data)
{
    const char *accepted[] = { "pwr d 1", "pwr d 4", "pwr d 258" };
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len, i;

    elsc_reset();

    for (i = 0; i < ARRAY_SIZE(accepted); i++) {
        pwr_down_request(qts, accepted[i]);
    }
    req_len = build_request(req, "pwr d 259");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    assert_response_begins(resp, "err arg");
    elsc_reset();
}

/*
 * IRIX's halt: "pwr d 1", then power goes about a second later, with no
 * further request.
 */
static void test_pwr_down_countdown_expires(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -no-shutdown");

    pwr_down_request(qts, "pwr d 1");
    qtest_clock_step(qts, 999 * (int64_t)SCALE_MS);
    g_assert_false(powered_down(qts));
    qtest_clock_step(qts, SCALE_MS);
    qtest_qmp_eventwait(qts, "SHUTDOWN");

    qtest_quit(qts);
}

/*
 * Each timed "pwr d" reloads the countdown from that request: repeating
 * "pwr d 8" keeps power on, and a shorter delay pulls the deadline in.
 */
static void test_pwr_down_countdown_reloads(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -no-shutdown");

    pwr_down_request(qts, "pwr d 8");
    qtest_clock_step(qts, 7 * NANOSECONDS_PER_SECOND);
    pwr_down_request(qts, "pwr d 8");
    /* The deadline is now 15 s, not the first request's 8 s. */
    qtest_clock_step(qts, 7999 * (int64_t)SCALE_MS);
    g_assert_false(powered_down(qts));
    qtest_clock_step(qts, SCALE_MS);
    qtest_qmp_eventwait(qts, "SHUTDOWN");
    qtest_quit(qts);

    qts = qtest_init("-M origin200 -no-shutdown");
    pwr_down_request(qts, "pwr d 8");
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    pwr_down_request(qts, "pwr d 2");
    /* Pulled in to 3 s from 8 s. */
    qtest_clock_step(qts, 1999 * (int64_t)SCALE_MS);
    g_assert_false(powered_down(qts));
    qtest_clock_step(qts, SCALE_MS);
    qtest_qmp_eventwait(qts, "SHUTDOWN");
    qtest_quit(qts);
}

/*
 * Argument parsing: one separator of any kind, then at most four hex
 * digits up to the first non-hex character. "pwr d-1x" is a one-second
 * countdown. "pwr d 12345" reads 0x1234, above the limit, so it is
 * "err arg" and, like any refused delay, arms nothing. "pwr d 00001" reads
 * 0000 and powers down at once.
 */
static void test_pwr_down_parsing(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -no-shutdown");
    const char *refused[] = { "pwr d 12345", "pwr d 259" };
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len, i;

    for (i = 0; i < ARRAY_SIZE(refused); i++) {
        req_len = build_request(req, refused[i]);
        resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
        g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
        g_assert_cmpuint(resp[0], ==, 7);
        assert_response_begins(resp, "err arg");
    }
    qtest_clock_step(qts, 700 * NANOSECONDS_PER_SECOND);
    g_assert_false(powered_down(qts));

    pwr_down_request(qts, "pwr d-1x");
    qtest_clock_step(qts, 999 * (int64_t)SCALE_MS);
    g_assert_false(powered_down(qts));
    qtest_clock_step(qts, SCALE_MS);
    qtest_qmp_eventwait(qts, "SHUTDOWN");
    qtest_quit(qts);

    qts = qtest_init("-M origin200 -no-shutdown");
    /* No clock step: a one-second countdown would never fire here. */
    pwr_down_request(qts, "pwr d 00001");
    qtest_qmp_eventwait(qts, "SHUTDOWN");
    qtest_quit(qts);
}

/*
 * "pwr d" with no delay: a three-second countdown when none is pending,
 * and immediate power-down when one is. Text that is not hex counts as no
 * delay.
 */
static void test_pwr_down_without_delay(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -no-shutdown");

    pwr_down_request(qts, "pwr d");
    qtest_clock_step(qts, 2 * NANOSECONDS_PER_SECOND);
    g_assert_false(powered_down(qts));
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    qtest_qmp_eventwait(qts, "SHUTDOWN");
    qtest_quit(qts);

    qts = qtest_init("-M origin200 -no-shutdown");
    pwr_down_request(qts, "pwr d 3c");
    g_assert_false(powered_down(qts));
    pwr_down_request(qts, "pwr d zz");
    qtest_qmp_eventwait(qts, "SHUTDOWN");
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
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    elsc_reset();

    /* Clear the cold-power latch: the first query reads 1, the next 0. */
    req_len = build_request(req, "pwr q");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    assert_response_begins(resp, "ok 1");
    req_len = build_request(req, "pwr q");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    assert_response_begins(resp, "ok 0");

    /* Power-cycle. */
    req_len = build_request(req, "pwr c");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    assert_response_begins(resp, "ok");
    qtest_qmp_eventwait(qts, "RESET");

    /* The cycle re-armed the latch: the first post-cycle query reads 1. */
    req_len = build_request(req, "pwr q");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    assert_response_begins(resp, "ok 1");
}

/*
 * "nic <p> <s> <d>" is the one-wire identity passthrough (firmware command
 * contract), an interactive-monitor command, not boot-driven. Three hex
 * arguments bit-bang the midplane one-wire line; it answers "ok <hex>" with
 * the sampled bit. No one-wire device is modelled, so the open-drain line
 * idles high and every sample reads 1.
 */
static void test_nic_onewire_identity(const void *data)
{
    uint8_t req[16], resp[RESP_CAP];
    unsigned req_len, resp_len;

    elsc_reset();

    /* Three hex arguments: the idle line samples high. */
    req_len = build_request(req, "nic 1 2 3");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 4);
    assert_response_begins(resp, "ok 1");

    /* Too few arguments to a known command is a bad argument. */
    req_len = build_request(req, "nic 1 2");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    assert_response_begins(resp, "err arg");

    /* A non-hex argument. */
    req_len = build_request(req, "nic 1 2 zz");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    assert_response_begins(resp, "err arg");

    /*
     * Each value shorter than four digits consumes the character after it,
     * so a second space leaves the next value absent.
     */
    req_len = build_request(req, "nic 1 2  3");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    assert_response_begins(resp, "err arg");

    /*
     * A four-digit value consumes nothing after it: the next value follows
     * with no separator, and a space there is a missing value.
     */
    req_len = build_request(req, "nic 000100020003");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    assert_response_begins(resp, "ok 1");
    req_len = build_request(req, "nic 0001 2 3");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    assert_response_begins(resp, "err arg");

    /* Junk after the mnemonic with no delimiter is an unknown command. */
    req_len = build_request(req, "nicx");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    assert_response_begins(resp, "err cmd");
}


/*
 * The request grammar every command shares: a three-letter lowercase
 * mnemonic and a space or the end of the text, then arguments the
 * command's readers take, with anything left over discarded.
 */
static void test_request_grammar(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,elsc-dip-switches=240");

    /* The mnemonic and its separator. */
    assert_reply_bytes(qts, "", 0, "err cmd");
    assert_reply(qts, "ve", "err cmd");
    assert_reply(qts, "VER", "err cmd");
    assert_reply(qts, "verx", "err cmd");
    assert_reply(qts, "ver", "ok V 3.1S  ");
    assert_reply(qts, "ver junk", "ok V 3.1S  ");

    /* Hex replies carry no leading zero. */
    assert_reply(qts, "rsw", "ok f");
    assert_reply(qts, "rsw 12", "ok f");

    /* The character reader skips spaces and leaves the rest unread. */
    assert_reply(qts, "pwr   q", "ok 1");
    assert_reply(qts, "pwr q   junk", "ok 0");

    /*
     * An Origin 200 module is powered whenever the machine runs: "pwr"
     * reports it and "pwr u" has nothing to do.
     */
    assert_reply(qts, "pwr", "ok u");
    assert_reply(qts, "pwr u", "ok ");
    assert_reply(qts, "pwr q", "ok 0");
    qtest_quit(qts);
}

/*
 * The commands that only keep a flag: "fan", "fsc", "tst", "tmp", "sla"
 * and "clr", byte for byte, with what each leaves for the next query.
 */
static void test_stored_flag_commands(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -no-shutdown");

    /* A healthy machine answers "ok n" until the host asks for high. */
    assert_reply(qts, "fan", "ok n");
    assert_reply(qts, "fan h", "ok ");
    assert_reply(qts, "fan", "ok h");
    assert_reply(qts, "fan H", "err arg");
    assert_reply(qts, "fan x", "err arg");
    assert_reply(qts, "fan", "ok h");
    assert_reply(qts, "fan  n junk", "ok ");
    assert_reply(qts, "fan", "ok n");

    /* The "fsc" query loses its answer over I2C. */
    assert_reply(qts, "fsc", "ok ");
    assert_reply(qts, "fsc 1", "ok ");
    assert_reply(qts, "fsc", "ok ");
    assert_reply(qts, "fsc 0", "ok ");
    /* Any other argument is the query: a bare "ok ", flag unchanged. */
    assert_reply(qts, "fsc x", "ok ");

    /* "tst" needs a character; "1" sets and anything else clears. */
    assert_reply(qts, "tst", "err arg");
    assert_reply(qts, "tst 1", "ok ");
    assert_reply(qts, "tst x", "ok ");

    /* No thermal model: normal, and the slave's letter once it reports. */
    assert_reply(qts, "tmp", "ok n");
    assert_reply(qts, "tmp junk", "ok n");
    assert_reply(qts, "sla h", "ok ");
    assert_reply(qts, "tmp", "ok n");
    assert_reply(qts, "sla \x07", "ok ");
    assert_reply(qts, "tmp", "ok n h");
    assert_reply(qts, "sla n", "ok ");
    assert_reply(qts, "tmp", "ok n n");

    /* The Origin 2000 build's own commands are not in this one's table. */
    assert_reply(qts, "key", "err cmd");
    assert_reply(qts, "vlm 5", "err cmd");
    assert_reply(qts, "mod", "err cmd");
    assert_reply(qts, "fre", "err cmd");

    /* "clr" drops the fan request and a pending power-down countdown. */
    assert_reply(qts, "fan h", "ok ");
    assert_reply(qts, "pwr d 2", "ok ");
    assert_reply(qts, "clr", "ok ");
    assert_reply(qts, "fan", "ok n");
    qtest_clock_step(qts, 3 * NANOSECONDS_PER_SECOND);
    g_assert_false(powered_down(qts));
    qtest_quit(qts);
}

/*
 * "aut", Origin 200 build: the automatic power-on bit, 0x02 of the
 * configuration byte at offset 7 of the controller's NVRAM page. The page
 * starts as a controller that initialised it leaves it, with the bit set
 * and the password "none".
 */
static void test_aut_reads_and_writes_nvram(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    arb_enable_observer(qts);
    assert_reply(qts, "aut", "ok 1");
    assert_reply(qts, "aut 0", "ok ");
    assert_reply(qts, "aut", "ok 0");
    assert_reply(qts, "aut 1", "ok ");
    assert_reply(qts, "aut", "ok 1");
    g_assert_cmphex(firmware_style_nvram_read(qts, 7), ==, 0x02);
    /* The password's last byte, not the configuration byte. */
    g_assert_cmphex(firmware_style_nvram_read(qts, 4), ==, 'e');
    assert_reply(qts, "aut x", "err arg");
    assert_reply(qts, "aut", "ok 1");

    /* The NVRAM, not the controller, holds it: a reset leaves it set. */
    qtest_system_reset(qts);
    arb_enable_observer(qts);
    assert_reply(qts, "aut", "ok 1");
    assert_reply(qts, "aut  0 junk", "ok ");
    assert_reply(qts, "aut", "ok 0");
    g_assert_cmphex(firmware_style_nvram_read(qts, 7), ==, 0);
    g_assert_cmphex(firmware_style_nvram_read(qts, 0), ==, 0x37);
    qtest_quit(qts);
}

/*
 * A timed or soft "pwr d" queues "pd remote" for the Hubs. It is announced
 * with signal 2, "get" returns its bare text once per processor, and the
 * announcement lapses after about a second. A refused delay and an
 * immediate power-down queue nothing.
 */
static void test_pwr_down_posts_pd_remote(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -no-shutdown");
    g_autofree char *before = NULL;
    g_autofree char *soft = NULL;

    arb_enable_observer(qts);
    assert_reply(qts, "pwr d 259", "err arg");
    before = qtest_hmp(qts, "info elsc");
    g_assert_null(strstr(before, "controller message"));

    assert_reply(qts, "pwr d 8", "ok ");
    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL2);
    assert_reply(qts, "get", "pd remote");
    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL1);
    assert_reply(qts, "get", "acp . ");

    /* Unfetched, it lapses after a second. */
    assert_reply(qts, "pwr d 8", "ok ");
    qtest_clock_step(qts, 1000 * (int64_t)SCALE_MS);
    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL1);
    assert_reply(qts, "get", "acp . ");

    /* The soft power-down tells the Hubs too. */
    assert_reply(qts, "clr", "ok ");
    assert_reply(qts, "pwr d", "ok ");
    soft = qtest_hmp(qts, "info elsc");
    g_assert_nonnull(strstr(soft, "controller message: \"pd remote\""));
    assert_reply(qts, "clr", "ok ");
    qtest_quit(qts);
}

/*
 * An acknowledged "rst" resets the module a second later, and another "rst"
 * inside that second starts the wait again. The controller keeps running
 * through the reset it caused: the lock is freed, while the fan request
 * and a pending power-down countdown survive. A reset from outside the
 * controller is its own start and clears them.
 */
static void test_rst_waits_one_second(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -no-shutdown");

    assert_reply(qts, "tas A", "ok 0");
    assert_reply(qts, "fan h", "ok ");
    assert_reply(qts, "rst", "ok ");
    qtest_clock_step(qts, 999 * (int64_t)SCALE_MS);
    assert_reply(qts, "tas", "ok A");
    assert_reply(qts, "rst", "ok ");
    qtest_clock_step(qts, 999 * (int64_t)SCALE_MS);
    assert_reply(qts, "tas", "ok A");
    qtest_clock_step(qts, SCALE_MS);
    qtest_qmp_eventwait(qts, "RESET");
    assert_reply(qts, "tas", "ok 0");
    assert_reply(qts, "fan", "ok h");

    /* The countdown keeps running through the reset. */
    assert_reply(qts, "pwr d 3", "ok ");
    assert_reply(qts, "rst", "ok ");
    qtest_clock_step(qts, ELSC_RESET_WAIT_NS);
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_false(powered_down(qts));
    qtest_clock_step(qts, 2 * NANOSECONDS_PER_SECOND);
    qtest_qmp_eventwait(qts, "SHUTDOWN");
    qtest_quit(qts);

    qts = qtest_init("-M origin200");
    assert_reply(qts, "fan h", "ok ");
    qtest_system_reset(qts);
    assert_reply(qts, "fan", "ok n");

    /*
     * A power cycle inside the wait does not cancel it: the reset still
     * comes a second after the "rst".
     */
    assert_reply(qts, "rst", "ok ");
    qtest_clock_step(qts, 300 * (int64_t)SCALE_MS);
    assert_reply(qts, "pwr c", "ok ");
    qtest_qmp_eventwait(qts, "RESET");
    assert_reply(qts, "tas A", "ok 0");
    qtest_clock_step(qts, 699 * (int64_t)SCALE_MS);
    assert_reply(qts, "tas", "ok A");
    qtest_clock_step(qts, SCALE_MS);
    qtest_qmp_eventwait(qts, "RESET");
    assert_reply(qts, "tas", "ok 0");
    qtest_quit(qts);

    /*
     * A reset the controller asked for that the machine turns into a
     * shutdown never happens; the next reset, from outside, is the
     * controller's start.
     */
    qts = qtest_init("-M origin200 -action reboot=shutdown,shutdown=pause");
    assert_reply(qts, "fan h", "ok ");
    assert_reply(qts, "rst", "ok ");
    qtest_clock_step(qts, ELSC_RESET_WAIT_NS);
    qtest_qmp_eventwait(qts, "SHUTDOWN");
    qtest_qmp_assert_success(qts, "{ 'execute': 'set-action', "
                             "'arguments': { 'reboot': 'reset' } }");
    qtest_system_reset(qts);
    assert_reply(qts, "fan", "ok n");
    qtest_quit(qts);
}

/*
 * "nmi" pulses the node's processors' NMI a second after the request, for
 * 100 ms, and another "nmi" inside the second starts the wait again. The
 * lock is freed when the pulse ends.
 */
static void test_nmi_pulses_after_one_second(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_irq_intercept_out_named(qts, "/machine/node0/hub/elsc", "nmi");
    assert_reply(qts, "tas A", "ok 0");
    assert_reply(qts, "nmi", "ok ");
    qtest_clock_step(qts, 999 * (int64_t)SCALE_MS);
    assert_reply(qts, "nmi junk", "ok ");
    qtest_clock_step(qts, 999 * (int64_t)SCALE_MS);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 0), ==, 0);
    qtest_clock_step(qts, SCALE_MS);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 0), ==, 1);
    g_assert_true(qtest_get_irq(qts, 0));
    assert_reply(qts, "tas", "ok A");
    qtest_clock_step(qts, 99 * (int64_t)SCALE_MS);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, SCALE_MS);
    g_assert_false(qtest_get_irq(qts, 0));
    assert_reply(qts, "tas", "ok 0");
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 0), ==, 1);

    /* A power cycle inside the wait does not cancel it. */
    assert_reply(qts, "nmi", "ok ");
    qtest_clock_step(qts, 500 * (int64_t)SCALE_MS);
    assert_reply(qts, "pwr c", "ok ");
    qtest_qmp_eventwait(qts, "RESET");
    qtest_clock_step(qts, 500 * (int64_t)SCALE_MS);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 0), ==, 2);
    qtest_quit(qts);
}

/*
 * "sel" and "see": the console input selection and output filter, their
 * reports, the processor names, and what each setting leaves for "get".
 */
static void test_console_selection_commands(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 " ELSC_AUX_LEADING_SERIALS
        "-chardev ringbuf,id=aux,size=4096 -serial chardev:aux");

    assert_reply(qts, "sel", "ok auto");
    assert_reply(qts, "see", "ok all");
    assert_reply(qts, "get", "acp . ");

    assert_reply(qts, "sel 2b", "ok ");
    assert_reply(qts, "sel", "ok 2b");
    assert_reply(qts, "get", "acp 3 ");
    assert_reply(qts, "sel all", "ok ");
    assert_reply(qts, "sel", "ok none");
    assert_reply(qts, "get", "acp / ");
    assert_reply(qts, "sel  4 A", "ok ");
    assert_reply(qts, "sel", "ok 4a");
    assert_reply(qts, "sel AUTO", "ok ");
    assert_reply(qts, "sel", "ok auto");
    assert_reply(qts, "sel no", "ok ");
    assert_reply(qts, "sel", "ok none");

    /* A bad name changes nothing. */
    assert_reply(qts, "sel 5a", "err arg");
    assert_reply(qts, "sel 1", "err arg");
    assert_reply(qts, "sel 1c", "err arg");
    assert_reply(qts, "sel x", "err arg");
    assert_reply(qts, "see au", "err arg");
    assert_reply(qts, "sel", "ok none");

    /* "see" with a processor filters output and selects that input. */
    assert_reply(qts, "see 1b", "ok ");
    assert_reply(qts, "see", "ok 1b");
    assert_reply(qts, "sel", "ok 1b");
    /* qtest output belongs to processor 0: counted, not copied. */
    assert_reply(qts, "acp 0 hi", "ok 2");
    assert_aux_captured(qts, "aux", "");
    /* The filter goes by the frame's selector, "1" being processor 1. */
    assert_reply(qts, "acp 1 ab", "ok 2");
    assert_aux_captured(qts, "aux", "ab");
    assert_reply(qts, "see 1a", "ok ");
    assert_reply(qts, "acp 0 yo", "ok 2");
    assert_aux_captured(qts, "aux", "yo");
    assert_reply(qts, "see none", "ok ");
    assert_reply(qts, "see", "ok all");
    assert_reply(qts, "sel", "ok 1a");

    /* "clr" leaves the selection; an outside reset restores the default. */
    assert_reply(qts, "clr", "ok ");
    assert_reply(qts, "sel", "ok 1a");
    qtest_system_reset(qts);
    assert_reply(qts, "sel", "ok auto");
    qtest_quit(qts);
}

/*
 * Queued console input is announced to the selected processor only. An
 * explicit selection of processor 1 leaves code 0 at signal 1 with input
 * waiting; a new selection discards the queue; selecting processor 0
 * announces fresh input there again.
 */
static void test_console_selection_announcement(const void *data)
{
    uint16_t port;
    int lfd = aux_listen(&port);
    g_autofree char *args = g_strdup_printf(
        "-M origin200 " ELSC_AUX_LEADING_SERIALS
        "-chardev socket,id=aux,host=127.0.0.1,port=%u,reconnect-ms=10000 "
        "-serial chardev:aux", port);
    QTestState *qts = qtest_init(args);
    int fd = aux_accept(lfd);

    arb_enable_observer(qts);
    assert_reply(qts, "sel 1b", "ok ");
    aux_write(fd, "hi");
    aux_wait_for_queued(qts, 2);
    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL1);

    assert_reply(qts, "sel 1a", "ok ");
    aux_wait_for_queued(qts, 0);
    aux_write(fd, "yo");
    aux_wait_for_queued(qts, 2);
    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL2);
    assert_reply(qts, "get", "acp 0 yo");

    /*
     * Automatic selection follows the selector in the last output frame,
     * not the processor that sent it: every qtest request comes from
     * processor 0, yet output naming processor 1 moves the announcement
     * off code 0. A frame with a corrupted checksum still counts.
     */
    assert_reply(qts, "sel auto", "ok ");
    /* No output yet since the start: nobody is announced to. */
    aux_write(fd, "zz");
    aux_wait_for_queued(qts, 2);
    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL1);
    assert_reply(qts, "acp 0 ", "ok 0");
    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL2);
    {
        uint8_t req[16], resp[RESP_CAP];
        unsigned req_len = build_request(req, "acp 1 ");

        req[req_len - 1] ^= 0xff;
        elsc_transact(qts, req, req_len, resp, sizeof(resp));
        assert_response_begins(resp, "err csum");
    }
    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL1);

    close(fd);
    close(lfd);
    qtest_quit(qts);
}

#define ELSC_PATH "/machine/node0/hub/elsc"

/*
 * The heartbeat watchdog's stages: the interval runs out into a five-second
 * warning, then an immediate NMI, then the delay to a power cycle, which
 * sets the cold-power latch and leaves the watchdog idle.
 */
static void test_hbt_watchdog_stages(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_irq_intercept_out_named(qts, ELSC_PATH, "nmi");
    assert_reply(qts, "pwr q", "ok 1");
    assert_reply(qts, "hbt 5 a", "ok ");
    qtest_clock_step(qts, 9999 * (int64_t)SCALE_MS);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 0), ==, 0);
    qtest_clock_step(qts, SCALE_MS);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 0), ==, 1);
    assert_reply(qts, "pwr q", "ok 0");

    /* The second value, 0xa, is the delay to the power cycle. */
    qtest_clock_step(qts, 9999 * (int64_t)SCALE_MS);
    assert_reply(qts, "pwr q", "ok 0");
    qtest_clock_step(qts, SCALE_MS);
    qtest_qmp_eventwait(qts, "RESET");
    assert_reply(qts, "pwr q", "ok 1");

    /* Idle now: neither time nor a heartbeat brings another NMI. */
    qtest_clock_step(qts, 60 * NANOSECONDS_PER_SECOND);
    assert_reply(qts, "hbt", "ok ");
    qtest_clock_step(qts, 60 * NANOSECONDS_PER_SECOND);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 0), ==, 1);
    assert_reply(qts, "pwr q", "ok 0");
    qtest_quit(qts);
}

/*
 * A heartbeat in the five-second warning postpones the NMI by an interval
 * rather than cancelling it. A delay of zero never power-cycles.
 */
static void test_hbt_heartbeat_postpones(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_irq_intercept_out_named(qts, ELSC_PATH, "nmi");
    assert_reply(qts, "pwr q", "ok 1");
    assert_reply(qts, "hbt 5 0", "ok ");
    qtest_clock_step(qts, 6 * NANOSECONDS_PER_SECOND);
    assert_reply(qts, "hbt", "ok ");
    qtest_clock_step(qts, 4999 * (int64_t)SCALE_MS);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 0), ==, 0);
    qtest_clock_step(qts, SCALE_MS);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 0), ==, 1);
    qtest_clock_step(qts, 120 * NANOSECONDS_PER_SECOND);
    assert_reply(qts, "pwr q", "ok 0");
    qtest_quit(qts);
}

/*
 * "clr" and "hbt 0" stop the watchdog; a "rst" does not, because the
 * controller keeps running through the reset it causes.
 */
static void test_hbt_stopped_only_by_clr_or_zero(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_irq_intercept_out_named(qts, ELSC_PATH, "nmi");
    assert_reply(qts, "hbt 5", "ok ");
    qtest_clock_step(qts, 3 * NANOSECONDS_PER_SECOND);
    assert_reply(qts, "clr", "ok ");
    qtest_clock_step(qts, 20 * NANOSECONDS_PER_SECOND);
    assert_reply(qts, "hbt 5", "ok ");
    assert_reply(qts, "hbt 0", "ok ");
    qtest_clock_step(qts, 20 * NANOSECONDS_PER_SECOND);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 0), ==, 0);

    assert_reply(qts, "hbt 5", "ok ");
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    assert_reply(qts, "rst", "ok ");
    qtest_clock_step(qts, ELSC_RESET_WAIT_NS);
    qtest_qmp_eventwait(qts, "RESET");
    qtest_clock_step(qts, 7999 * (int64_t)SCALE_MS);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 0), ==, 0);
    qtest_clock_step(qts, SCALE_MS);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 0), ==, 1);
    assert_reply(qts, "hbt 0", "ok ");
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qts = qtest_init("-M origin200");

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
    qtest_add_data_func("sn0/elsc/arbitration-observes-grant-with-staged-start",
                        NULL,
                        test_arbitration_observes_grant_with_staged_start);
    qtest_add_data_func("sn0/elsc/arbitration-tick-is-one-millisecond",
                        NULL, test_arbitration_tick_is_one_millisecond);
    qtest_add_data_func("sn0/elsc/arbitration-busy-tick-does-not-advance",
                        NULL, test_arbitration_busy_tick_does_not_advance);
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
    qtest_add_data_func("sn0/elsc/two-towers-share-master-grant-ring",
                        NULL, test_two_towers_share_master_grant_ring);
    qtest_add_data_func("sn0/elsc/two-towers-reset-keeps-cpu3-grant-live",
                        NULL, test_two_towers_reset_keeps_cpu3_grant_live);
    qtest_add_data_func("sn0/elsc/two-towers-ring-turns-in-five-ticks",
                        NULL, test_two_towers_ring_turns_in_five_ticks);
    qtest_add_data_func("sn0/elsc/acp-with-no-backend-still-answers", NULL,
                        test_acp_with_no_backend_still_answers);
    qtest_add_data_func("sn0/elsc/get-returns-queued-console-input", NULL,
                        test_get_returns_queued_console_input);
    qtest_add_data_func("sn0/elsc/escape-byte-is-not-queued", NULL,
                        test_escape_byte_is_not_queued);
    qtest_add_data_func("sn0/elsc/no-supervisor-on-i2c", NULL,
                        test_no_supervisor_on_i2c);
    qtest_add_data_func("sn0/elsc/origin200-build-commands", NULL,
                        test_origin200_build_commands);
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
    qtest_add_data_func("sn0/elsc/pwr-down-range-checked", NULL,
                        test_pwr_down_range_checked);
    qtest_add_data_func("sn0/elsc/pwr-down-countdown-expires", NULL,
                        test_pwr_down_countdown_expires);
    qtest_add_data_func("sn0/elsc/pwr-down-countdown-reloads", NULL,
                        test_pwr_down_countdown_reloads);
    qtest_add_data_func("sn0/elsc/pwr-down-without-delay", NULL,
                        test_pwr_down_without_delay);
    qtest_add_data_func("sn0/elsc/pwr-down-parsing", NULL,
                        test_pwr_down_parsing);
    qtest_add_data_func("sn0/elsc/pwr-cycle-rearms-cold-power", NULL,
                        test_pwr_cycle_rearms_cold_power);
    qtest_add_data_func("sn0/elsc/hbt-watchdog-stages", NULL,
                        test_hbt_watchdog_stages);
    qtest_add_data_func("sn0/elsc/hbt-heartbeat-postpones", NULL,
                        test_hbt_heartbeat_postpones);
    qtest_add_data_func("sn0/elsc/hbt-stopped-only-by-clr-or-zero", NULL,
                        test_hbt_stopped_only_by_clr_or_zero);
    qtest_add_data_func("sn0/elsc/console-selection-commands", NULL,
                        test_console_selection_commands);
    qtest_add_data_func("sn0/elsc/console-selection-announcement", NULL,
                        test_console_selection_announcement);
    qtest_add_data_func("sn0/elsc/nmi-pulses-after-one-second", NULL,
                        test_nmi_pulses_after_one_second);
    qtest_add_data_func("sn0/elsc/rst-waits-one-second", NULL,
                        test_rst_waits_one_second);
    qtest_add_data_func("sn0/elsc/pwr-down-posts-pd-remote", NULL,
                        test_pwr_down_posts_pd_remote);
    qtest_add_data_func("sn0/elsc/aut-reads-and-writes-nvram", NULL,
                        test_aut_reads_and_writes_nvram);
    qtest_add_data_func("sn0/elsc/stored-flag-commands", NULL,
                        test_stored_flag_commands);
    qtest_add_data_func("sn0/elsc/request-grammar", NULL,
                        test_request_grammar);
    qtest_add_data_func("sn0/elsc/nic-onewire-identity", NULL,
                        test_nic_onewire_identity);

    ret = g_test_run();
    qtest_quit(qts);

    return ret;
}
