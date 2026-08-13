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

    ret = g_test_run();

    return ret;
}
