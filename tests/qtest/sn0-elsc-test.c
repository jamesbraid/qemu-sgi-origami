/*
 * SGI Origin 200 -- the ELSC system controller's checksummed ASCII
 * command/response channel, driven through the real PCF8584 as firmware
 * would. docs/datasheets/pcf8584-elsc.md is the spec this was built from.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define SPACE_XTALK     0x20000000000ULL
#define HUB_LOCAL       (SPACE_XTALK + 0x01000000)

#define I2C_DATA        0x220000
#define I2C_CTRL        0x220008

#define CTL_PIN  0x80
#define CTL_ESO  0x40
#define CTL_STA  0x04
#define CTL_STO  0x02
#define CTL_ACK  0x01

#define STA_LRB  0x08
#define STA_BNB  0x01

#define ELSC_ADDR 0x08

/* A framed response's text is capped at 94 bytes plus its own length and
 * checksum bytes -- the datasheet's own bound on what this device may
 * emit. */
#define RESP_CAP 96

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
 * One combined write-then-read transaction to the ELSC over the real
 * PCF8584 registers. Sends req_len bytes of an already-framed request, then
 * reads back the response into resp (its own length byte first, discovered
 * from the wire rather than assumed) and returns how many bytes that was.
 *
 * The read side discards one byte before the response's own length byte,
 * not because the protocol has a preamble but because the chip's own
 * receive pipeline (hw/i2c/pcf8584.c) fetches one byte ahead and hands it
 * back on the driver's first read -- the same mechanic the closing
 * invalid-address sequence exercises, here driving a real slave instead of
 * a test fixture.
 */
static unsigned elsc_transact(QTestState *qts, const uint8_t *req,
                              unsigned req_len, uint8_t *resp,
                              unsigned resp_cap)
{
    unsigned i, total;

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, (ELSC_ADDR << 1) | 0);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & STA_LRB, ==, 0);

    for (i = 0; i < req_len; i++) {
        qtest_writeq(qts, HUB_LOCAL + I2C_DATA, req[i]);
    }

    /* Repeated start, switching direction: control first, then the
     * address byte, per the discriminator hw/i2c/pcf8584.c uses. */
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

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);

    return total;
}

static void test_ver_command_succeeds(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t req[8], resp[RESP_CAP];
    unsigned req_len, resp_len;

    req_len = build_request(req, "ver");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));

    g_assert_cmpuint(resp_len, ==, (unsigned)resp[0] + 2);
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    /* "the presence probe is defined as did the version command succeed,
     * and succeeding takes more than a valid checksum" -- the response
     * text has to begin "ok". */
    g_assert_cmpuint(resp[0], >=, 2);
    g_assert(memcmp(&resp[1], "ok", 2) == 0);

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

    /* A well-formed, correctly checksummed request for a mnemonic this
     * milestone does not implement -- must still complete the transaction
     * rather than stall it, which is what a DIP-switch read or any other
     * unimplemented command depends on to reach the console candidate
     * loop at all. */
    req_len = build_request(req, "xyz");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));

    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    g_assert_cmpuint(resp[0], ==, 7);
    g_assert(memcmp(&resp[1], "err cmd", 7) == 0);

    qtest_quit(qts);
}

/* Processor 0's signal pair on a single-node, single-processor machine
 * (-M origin200 defaults to one CPU): 0x70 * 2 plus processor_code 0, and
 * signal2 with bit 3 set. */
#define ARB_SIGNAL1 0xe0
#define ARB_SIGNAL2 0xe8

#define ARB_STEP_NS       (10 * 1000)
#define ARB_MAX_STEPS     2000
#define ARB_HOLD_ASSERT_NS (260 * 1000)  /* past the 250us threshold */

static uint64_t read_status(QTestState *qts)
{
    return qtest_readq(qts, HUB_LOCAL + I2C_CTRL);
}

static uint64_t read_data(QTestState *qts)
{
    return qtest_readq(qts, HUB_LOCAL + I2C_DATA);
}

/*
 * Loop 1 of the real wait: step the clock until the bus reports not-busy
 * and the data register holds neither signal value. This is the loop a
 * model that parks a signal byte and never clears it hangs in forever --
 * skipping straight to "wait for the signal" would pass such a model, so
 * this is asserted as its own step rather than folded into the next one.
 */
static void arb_wait_for_absence(QTestState *qts)
{
    int i;

    for (i = 0; i < ARB_MAX_STEPS; i++) {
        uint64_t data = read_data(qts);
        uint64_t status = read_status(qts);

        if ((status & STA_BNB) && data != ARB_SIGNAL1 && data != ARB_SIGNAL2) {
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

        if ((status & STA_BNB) && (data == ARB_SIGNAL1 || data == ARB_SIGNAL2)) {
            return (uint8_t)data;
        }
        qtest_clock_step(qts, ARB_STEP_NS);
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
        g_assert_true((read_status(qts) & STA_BNB) != 0);
        g_assert_cmphex(read_data(qts), ==, value);
    }
}

/*
 * The real three-loop wait, driven exactly as the firmware drives it: an
 * absence has to be observed before a signal is trusted, and the signal
 * then has to hold past the threshold without changing. Deliberately not
 * synchronised to when the rotation's own cycle happens to start -- the
 * test does not know or control that, which is what makes loop 1 a real
 * assertion here rather than a formality: whatever phase realize happened
 * to leave the model in, this has to walk through absence, signal,
 * threshold in that order before it is satisfied.
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
 * The rotation is free-running rather than one-shot: after one full
 * gap-then-hold cycle completes and the value changes away, the same
 * three-loop wait succeeds again on the next cycle. This is what backs the
 * datasheet's "a signal that drops before the hold expires sends the
 * firmware back to waiting, and the wait then completes on the next
 * full-length assertion" -- a model that only ever pulses once would leave
 * this second wait hanging.
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
    qtest_add_data_func("sn0/elsc/arbitration-three-loop-wait", NULL,
                        test_arbitration_three_loop_wait);
    qtest_add_data_func("sn0/elsc/arbitration-repeats", NULL,
                        test_arbitration_repeats);

    ret = g_test_run();

    return ret;
}
