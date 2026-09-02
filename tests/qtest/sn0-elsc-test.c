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
#define ARB_MAX_STEPS     2000
#define ARB_HOLD_ASSERT_NS (260 * 1000)  /* past the 250us threshold */

/*
 * Crossing the rotation: every position is held for a slot of at
 * least 10ms, so hunting one code's signal can take most of a round
 * (up to 88.2ms with the varying slot lengths). Scan coarsely -- a
 * step this size cannot miss a held signal -- and keep the fine step
 * where a 50us gap must not fall between samples.
 */
#define ARB_SCAN_STEP_NS  (100 * 1000)

static uint64_t read_status(QTestState *qts)
{
    return qtest_readq(qts, HUB_LOCAL + I2C_CTRL);
}

static uint64_t read_data(QTestState *qts)
{
    return qtest_readq(qts, HUB_LOCAL + I2C_DATA);
}

/*
 * Loop 1: wait for neither signal value. Its own step because skipping
 * straight to "wait for the signal" passes a model that parks a signal
 * byte and never clears it.
 */
static void arb_wait_for_absence(QTestState *qts)
{
    int i;

    for (i = 0; i < ARB_MAX_STEPS; i++) {
        uint64_t data = read_data(qts);
        uint64_t status = read_status(qts);

        if ((status & I2C_PCF_BB) &&
            data != ARB_SIGNAL1 && data != ARB_SIGNAL2) {
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

/* Loop 3: step the clock in small increments, asserting the same value
 * holds and the bus stays not-busy at every step, for at least the 250us
 * threshold. A model that only pulses briefly, or that lets some other
 * value leak in mid-hold, fails here rather than in loop 1 or 2. */
static void arb_assert_held(QTestState *qts, uint8_t value, int64_t hold_ns)
{
    int64_t held = 0;

    while (held < hold_ns) {
        qtest_clock_step(qts, ARB_STEP_NS);
        held += ARB_STEP_NS;
        g_assert_true((read_status(qts) & I2C_PCF_BB) != 0);
        g_assert_cmphex(read_data(qts), ==, value);
    }
}

/*
 * Firmware's three-loop wait, deliberately unsynchronised to the
 * rotation's phase -- that is what makes loop 1 a real assertion.
 */
static void test_arbitration_three_loop_wait(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t sig;

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

    arb_wait_for_absence(qts);
    sig = arb_wait_for_signal(qts);
    g_assert(sig == ARB_SIGNAL1 || sig == ARB_SIGNAL2);
    arb_assert_held(qts, sig, ARB_HOLD_ASSERT_NS);

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

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

    arb_wait_for_absence(qts);
    first = arb_wait_for_signal(qts);
    arb_assert_held(qts, first, ARB_HOLD_ASSERT_NS);

    arb_wait_for_absence(qts);
    second = arb_wait_for_signal(qts);
    g_assert_cmphex(second, ==, first);
    arb_assert_held(qts, second, ARB_HOLD_ASSERT_NS);

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

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

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
 * The broadcast must not replace a staged address byte between the two
 * MMIO writes; advancing past the hold makes that boundary deterministic.
 */
static void test_arbitration_preserves_staged_start(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[8], resp[RESP_CAP];
    uint8_t signal;
    unsigned req_len, resp_len, i;

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);
    arb_wait_for_absence(qts);
    signal = arb_wait_for_signal(qts);
    arb_assert_held(qts, signal, ARB_HOLD_ASSERT_NS);

    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, ELSC_ADDR << 1);
    qtest_clock_step(qts, 100 * 1000);

    g_assert_cmphex(read_data(qts), ==, ELSC_ADDR << 1);
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
    arb_wait_for_signal(qts);

    /* Reset also releases an address staged without a following START. */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, ELSC_ADDR << 1);
    qtest_system_reset(qts);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);
    arb_wait_for_absence(qts);
    arb_wait_for_signal(qts);

    /* A benign control change must not abandon the staged address. */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, ELSC_ADDR << 1);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_ACK);
    qtest_clock_step(qts, 100 * 1000);
    g_assert_cmphex(read_data(qts), ==, ELSC_ADDR << 1);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(read_status(qts) & I2C_PCF_LRB, ==, 0);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STO);
    arb_wait_for_absence(qts);
    arb_wait_for_signal(qts);

    qtest_quit(qts);
}

/* Any code's signal, either half of its pair. */
static bool arb_is_signal_byte(uint64_t value)
{
    return value >= 0xe0 && value <= 0xef;
}

/*
 * The adversarial cadence: exactly two positions of a one-shot 350us
 * rotation (50us gap plus 300us hold), so a poll phase-locked against
 * such a rotation samples positions of one parity only and never the
 * code one past the first it observed. 256 polls cover two full rounds
 * of the held rotation with margin.
 */
#define ARB_PHASE_LOCKED_STEP_NS  (700 * 1000)
#define ARB_PHASE_LOCKED_POLLS    256

/*
 * A processor polling at a fixed cadence must find its own signal no
 * matter how that cadence phases against the rotation -- both run on
 * the same deterministic virtual clock, so an unlucky phase repeats
 * forever rather than drifting apart as it would on hardware. A signal
 * presented as a one-shot fails this: the poll below never samples the
 * target code's slot, which is how a two-node IRIX boot left one
 * processor waiting for its signal indefinitely. The signal must be
 * held until answered for every cadence to find it.
 */
static void test_arbitration_survives_phase_locked_poll(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t value = 0;
    unsigned target_code;
    bool saw_own_signal = false;
    int i;

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

    /* Sync to the rotation: catch the first presented signal. */
    for (i = 0; i < ARB_MAX_STEPS; i++) {
        value = read_data(qts);
        if ((read_status(qts) & I2C_PCF_BB) && arb_is_signal_byte(value)) {
            break;
        }
        qtest_clock_step(qts, ARB_STEP_NS);
    }
    g_assert_true(arb_is_signal_byte(value));
    /* The low three bits of either signal byte are the processor code. */
    target_code = ((value & 7) + 1) % 8;

    /* Sample mid-window, away from the presentation instants. */
    qtest_clock_step(qts, 150 * 1000);

    for (i = 0; i < ARB_PHASE_LOCKED_POLLS; i++) {
        value = read_data(qts);
        if ((read_status(qts) & I2C_PCF_BB) && arb_is_signal_byte(value) &&
            (value & 7) == target_code) {
            saw_own_signal = true;
            break;
        }
        qtest_clock_step(qts, ARB_PHASE_LOCKED_STEP_NS);
    }
    g_assert_true(saw_own_signal);

    qtest_quit(qts);
}

/*
 * No bus traffic ends a hold: every slot runs its full length. Any
 * consume-on-traffic edge starved a waiting processor -- its neighbor's
 * dialogue stole the held slot, and its own periodic command retries,
 * indistinguishable from an answer, withdrew the signal microseconds
 * after each presentation, before its poll could sample it. The hold
 * must survive the transaction below and still be found by a later
 * poll; only the slot length may end it.
 */
static void test_arbitration_foreign_traffic_leaves_hold(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[8], resp[RESP_CAP];
    unsigned req_len;
    uint8_t held;
    bool saw_same = false;
    int i;

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);
    arb_wait_for_absence(qts);
    held = arb_wait_for_signal(qts);

    /* Traffic mid-hold: whoever this was, the slot must not end. */
    req_len = build_request(req, "ver");
    elsc_transact(qts, req, req_len, resp, sizeof(resp));

    /*
     * The owner's later poll must still find its signal, promptly
     * re-presented; the ring advancing to code 1 is the theft.
     */
    for (i = 0; i < 100; i++) {
        uint64_t value = read_data(qts);

        if (read_status(qts) & I2C_PCF_BB) {
            g_assert_cmphex(value, !=, ARB_SIGNAL1 + 1);
            if (value == held) {
                saw_same = true;
                break;
            }
        }
        qtest_clock_step(qts, ARB_STEP_NS);
    }
    g_assert_true(saw_same);

    qtest_quit(qts);
}

/*
 * When a slot runs its length -- the only way a hold ends -- the next
 * code's signal appears only behind a fresh gap: the next processor's
 * trust check needs to see its signal absent before it appears.
 */
static void test_arbitration_hold_end_gaps_before_next(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t next_signal = 0;
    bool saw_gap = false;
    int i;

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);
    arb_wait_for_absence(qts);
    arb_wait_for_signal(qts);

    /* Fine steps across the longest slot and its end. */
    for (i = 0; i < 1400; i++) {
        uint64_t value = read_data(qts);

        if (read_status(qts) & I2C_PCF_BB) {
            if (value == 0) {
                saw_gap = true;
            } else if (saw_gap && arb_is_signal_byte(value)) {
                next_signal = value;
                break;
            }
        }
        qtest_clock_step(qts, ARB_STEP_NS);
    }
    g_assert_true(saw_gap);
    g_assert_cmphex(next_signal, ==, ARB_SIGNAL1 + 1);

    qtest_quit(qts);
}

/*
 * The commensurate trap the varying slot lengths exist to break: a
 * sampler whose period exactly matches a metronomic ring reads the
 * same slot at the same phase forever -- observed live as a poll
 * returning one code on 22 consecutive traps while a passive read of
 * the same register cycled through all eight. Slot lengths vary per
 * (code, revolution), so a sampler at the old constant ring period
 * slips 5.0-7.8ms per revolution against the ring; every slot spans
 * at least 10ms, so the drifting phase cannot step over a slot and
 * visits every code within one wrap -- at most 18 samples. 64 give
 * threefold margin.
 */
#define ARB_METRONOME_STEP_NS   (80400 * 1000)
#define ARB_METRONOME_SAMPLES   64

static void test_arbitration_defeats_ring_period_sampler(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    bool saw_code_2 = false;
    int i;

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);
    arb_wait_for_absence(qts);
    arb_wait_for_signal(qts);

    /* A phase deep inside code 0's slot in every revolution. */
    qtest_clock_step(qts, 5000 * 1000);

    for (i = 0; i < ARB_METRONOME_SAMPLES; i++) {
        uint64_t value = read_data(qts);

        if ((read_status(qts) & I2C_PCF_BB) && value == 0xe2) {
            saw_code_2 = true;
            break;
        }
        qtest_clock_step(qts, ARB_METRONOME_STEP_NS);
    }
    g_assert_true(saw_code_2);

    qtest_quit(qts);
}

/* Base slot minus the walk's 10us sampling grain, asserted held. */
#define ARB_SLOT_HELD_ASSERT_NS  (9900 * 1000)

/*
 * One revolution walked code by code: after each 50us gap the next
 * code's signal appears and stands at least the base slot. The
 * protocol is a multi-code handshake -- the accept path loops back
 * into the poll -- so a guest must be able to observe the codes in
 * sequence, not merely find its own eventually.
 */
static void test_arbitration_walks_all_codes_full_slots(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    unsigned int code;
    int i;

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

    /* The rotation begins with code 0's gap; the register reads 0. */
    g_assert_cmphex(read_data(qts), ==, 0);

    for (code = 0; code < 8; code++) {
        uint64_t value = 0;

        /* Leave the gap: the next signal begins within 50us. */
        for (i = 0; i < 200; i++) {
            value = read_data(qts);
            if (value != 0) {
                break;
            }
            qtest_clock_step(qts, ARB_STEP_NS);
        }
        g_assert_true(arb_is_signal_byte(value));
        g_assert_cmpuint(value & 7, ==, code);

        arb_assert_held(qts, (uint8_t)value, ARB_SLOT_HELD_ASSERT_NS);

        /* Step out the slot's remainder to the next gap. */
        for (i = 0; i < 400; i++) {
            if (read_data(qts) == 0) {
                break;
            }
            qtest_clock_step(qts, ARB_STEP_NS);
        }
        g_assert_cmphex(read_data(qts), ==, 0);
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

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

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

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

    req_len = build_request(req, "ver");
    elsc_transact(qts, req, req_len, resp, sizeof(resp));

    arb_wait_for_absence(qts);
    g_assert_cmphex(arb_wait_for_signal(qts), ==, ARB_SIGNAL1);

    qtest_quit(qts);
}

/*
 * The fetch the announcement promises: after the requester's transaction
 * ended unread, the response survives -- the ELSC's own broadcasts
 * deliver finish events too and must not discard it -- and a read-only
 * transaction returns the frame. Once consumed, the announcement stops.
 */
static void test_announced_response_read_after_stop(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t resp[RESP_CAP];
    unsigned resp_len;

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

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

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

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
 * Each tower carries its own physical AUX connector, so a two-chassis
 * machine has two system controllers and a backend may be provisioned
 * for each: node 0's on serial index 2, node 1's on index 4 (index 3
 * stays the bring-up UART). The model once handed index 2 to every
 * node's controller, so a two-node launch with the shipped consoles
 * provisioned -- the four-processor rig's exact configuration -- died
 * in chardev init ("chardev 'elsc' is already in use") before a byte
 * of console output. Launching at all is this test's first assertion;
 * the acp round-trips pin that each node's controller kept its own
 * backend and no capture leaked to the other tower's ringbuf.
 */
static void test_two_towers_take_two_aux_backends(const void *data)
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
    assert_aux_captured(qts, "aux1", "n1");

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

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);
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

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);
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

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

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

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);
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
    qtest_add_data_func("sn0/elsc/arbitration-preserves-staged-start", NULL,
                        test_arbitration_preserves_staged_start);
    qtest_add_data_func("sn0/elsc/arbitration-survives-phase-locked-poll",
                        NULL, test_arbitration_survives_phase_locked_poll);
    qtest_add_data_func("sn0/elsc/arbitration-foreign-traffic-leaves-hold",
                        NULL, test_arbitration_foreign_traffic_leaves_hold);
    qtest_add_data_func("sn0/elsc/arbitration-hold-end-gaps-before-next",
                        NULL, test_arbitration_hold_end_gaps_before_next);
    qtest_add_data_func("sn0/elsc/arbitration-defeats-ring-period-sampler",
                        NULL, test_arbitration_defeats_ring_period_sampler);
    qtest_add_data_func("sn0/elsc/arbitration-walks-all-codes-full-slots",
                        NULL, test_arbitration_walks_all_codes_full_slots);
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
    qtest_add_data_func("sn0/elsc/two-towers-take-two-aux-backends", NULL,
                        test_two_towers_take_two_aux_backends);
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

    ret = g_test_run();

    return ret;
}
