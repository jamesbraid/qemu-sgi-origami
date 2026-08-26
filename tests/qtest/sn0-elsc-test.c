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

/*
 * The ELSC AUX port is serial_hd(2): indices 0 and 1 are the two IOC3 rear
 * ports (hw/misc/sgi-hub.h, SN0_ELSC_CHARDEV). So two placeholder serial
 * lines come before the chardev standing in for the AUX cable, putting it on
 * the index the model reads for this device.
 */
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
 * One combined write-then-read transaction to the ELSC over the real
 * PCF8584 registers. Sends req_len bytes of an already-framed request, then
 * reads back the response into resp (its own length byte first, discovered
 * from the wire rather than assumed) and returns how many bytes that was.
 *
 * The read side discards one byte before the response's own length byte --
 * not a byte the ELSC sent, but this chip's own data register as it stood
 * before the read-direction start, which is what the driver's first read
 * after such a start always returns (hw/i2c/pcf8584.c). The same mechanic
 * the closing invalid-address sequence exercises, here driving a real
 * slave instead of a test fixture.
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
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & I2C_PCF_LRB,
                    ==, 0);

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
    const uint8_t *version_end;
    size_t version_len;
    unsigned req_len, resp_len;

    req_len = build_request(req, "ver");
    resp_len = elsc_transact(qts, req, req_len, resp, sizeof(resp));

    g_assert_cmpuint(resp_len, ==, (unsigned)resp[0] + 2);
    g_assert_cmphex(frame_checksum(resp, resp_len), ==, 0);
    /* "the presence probe is defined as did the version command succeed,
     * and succeeding takes more than a valid checksum" -- the response
     * text has to begin "ok". */
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
 * Processor code 0's own pair: 0x70 * 2 plus processor_code 0, and signal2
 * as signal1 + 8. This is the one code this device backs with a real
 * input queue (see hw/misc/sgi-elsc.c), and it is one of the eight the
 * broadcast's rotation presents in turn -- the wait helpers below
 * correctly treat every other code's pulse as "not mine" and keep
 * waiting, exactly as firmware does for time slices that are not its own.
 */
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
        g_assert_true((read_status(qts) & I2C_PCF_BB) != 0);
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

/*
 * The rotation has to actually visit more than one processor code, not
 * just eventually settle on the one this file's own helpers wait for --
 * the two tests above only prove code 0's own pair eventually appears,
 * which a model stuck on that one code, exactly like the bug this device
 * once had, would also satisfy. Built from the datasheet's own account of
 * the protocol's addressable space (docs/datasheets/pcf8584-elsc.md): four
 * node slots of two processors each give processor codes 0 through 7, and
 * a correct broadcaster offers all eight regardless of how many this
 * machine populates. Code 1's pair (0xe1/0xe9) is checked concretely
 * rather than "anything other than code 0", so a regression that swaps
 * one wrong fixed code for another still fails this test.
 */
static void test_arbitration_visits_other_processor_codes(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    bool saw_code_0 = false, saw_code_1 = false;
    int i;

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

    /* One full eight-code cycle is at most a few milliseconds; step
     * through several cycles' worth with margin so timing choices inside
     * the model don't make this test fragile. */
    for (i = 0; i < ARB_MAX_STEPS && !(saw_code_0 && saw_code_1); i++) {
        uint64_t observed = read_data(qts);

        if (observed == 0xe0 || observed == 0xe8) {
            saw_code_0 = true;
        }
        if (observed == 0xe1 || observed == 0xe9) {
            saw_code_1 = true;
        }
        qtest_clock_step(qts, ARB_STEP_NS);
    }

    g_assert_true(saw_code_0);
    g_assert_true(saw_code_1);

    qtest_quit(qts);
}

/*
 * Once firmware has observed and held its arbitration signal, it stages the
 * first address byte in the PCF8584 before asserting START.  The controller's
 * free-running broadcast must not replace that staged byte if its nominal
 * hold expires between the two MMIO writes.  Advancing past the remainder of
 * the modeled hold makes that boundary deterministic rather than relying on
 * host scheduling to happen to split the writes.
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

/*
 * The AUX serial port, for "acp" output: a ring-buffer chardev captures
 * whatever this device writes out, which is enough to verify the output
 * direction without needing a live connection -- QMP's ringbuf-write and
 * this device's own chr_write both land in the same buffer, and nothing
 * here needs to feed the device anything, so the asymmetry does not
 * matter for this test.
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
     * Not qtest_qmp_assert_success_ref(): it hands back response["return"]
     * unwrapped as a QDict via qdict_get_qdict(), which is the right tool
     * for a command whose result is itself an object but returns NULL for
     * ringbuf-read's bare string result -- silently, since qobject_ref()
     * on NULL is not a crash by itself. It only became one two lines
     * later, dereferencing that NULL as if it were a dict. Kept as a
     * cautionary comment because the failure was a segfault with no
     * assertion message pointing at the cause.
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
 * With no character device attached to the AUX port -- a rig that forgot to
 * drive the ELSC serial, the failure this whole change exists to make
 * diagnosable -- an "acp" console flush must still complete the transaction
 * and answer "ok <count>" rather than stall the bus. The bytes are discarded
 * (nothing is attached to catch them) and the model logs a single
 * LOG_GUEST_ERROR line naming the missing backend. The log is not observable
 * from qtest, so what this asserts is that the protocol keeps serving with the
 * port dark, which is what keeps a dark console from also wedging the guest.
 */
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
 * The AUX serial port, for console input: a loopback TCP socket standing
 * in for the real cable, following the same host-socket pattern
 * tests/qtest/ipmi-bt-test.c uses for its own external chardev -- bind and
 * listen before qtest_init, then accept once QEMU connects out to the
 * port named on its command line. Unlike the ringbuf above, this
 * direction needs a live connection: bytes have to reach this device's
 * receive callback, not just accumulate in a buffer nothing reads from.
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

static int aux_accept(int lfd)
{
    fd_set readfds;
    struct timeval tv = { .tv_sec = 10 };
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
    /* Wait for the rotation to notice the queue and present signal2 --
     * proof the arbitration byte really does depend on queue state, not
     * just that "get" happens to answer correctly when asked. */
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
 * The escape split: a byte sequence beginning 0x14 is a controller
 * command and is discarded through the next line terminator, never
 * reaching "get"; everything else is queued and does. Asserting only the
 * positive half -- that typed text reaches "get" -- would pass a model
 * that never implements the escape at all, so this checks both.
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

    /* A single \r as the terminator, matching a real terminal in raw
     * serial mode rather than a host line ending -- what specifically
     * ends an escaped command is not stated anywhere read for the
     * datasheet, so this is the model's own choice and the one this test
     * exercises, not a claim about a CRLF pair. */
    aux_write(fd, "AB\x14zzz\rCD");

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);
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
 * Read one byte from the system-controller NVRAM as firmware does: a
 * combined offset-write and data-read transaction, arbitrated first and
 * closed with the invalid-address step. The PCF8584 exposes its stale data
 * register on the first read after changing direction, so that read is
 * deliberately discarded before the EEPROM byte is collected.
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
 * One full ELSC command exactly as firmware drives it, not as
 * elsc_transact() drives it: arbitrate first, then the combined
 * write-then-read transaction, and -- staying in the same transfer, not
 * stopping and reopening one -- the closing repeated start to the invalid
 * address every transaction that completes its data phase ends with,
 * control written before the address, which must NAK. Only then the real
 * stop. elsc_transact() above stops right after reading the response
 * because most of this file's tests are about the response content and
 * the closing step is exercised on its own in sn0-i2c-test.c; this
 * function exists because task 6's actual open question -- would firmware
 * reach this device and accept it -- depends on the whole chain holding
 * together in the order and shape firmware really uses, not on any one
 * piece of it in isolation.
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
 * The question task 6 leaves open until this test answers it: not "does the
 * register contract work" -- the tests above already establish that -- but
 * "if firmware reached this device, would it accept it". Built from
 * docs/datasheets/pcf8584-elsc.md's own account of the forced sequence for
 * a boot that reaches and accepts the system-controller console, not from
 * what this file's own helpers happen to do, because a test written against
 * the implementation can only ever agree with it:
 *
 *   sixteen reads of the identity EEPROM at 0x57, whose first byte is the
 *   required 0x37 validity marker and whose remaining model defaults are
 *   coherent zero values;
 *   then rsw to the ELSC, whose active-low switch byte must parse;
 *   then ver, which must return a correctly framed, correctly checksummed
 *   response beginning "ok".
 *
 * Each real transaction is arbitrated first and closed with the
 * invalid-address step, in that order, because firmware runs the
 * arbitration wait before every real transaction and the closing step
 * after every one that completes its data phase -- not because this test
 * needed them, but because a boot that skipped either would not look like
 * this to the model at all.
 *
 * "Accept" is checked as firmware's own parser checks it, not as "some
 * response arrived": the acceptance condition is a checksum-valid packet
 * whose text begins "ok" followed by a space or a terminator, and nothing
 * weaker than that would have been rejected on real hardware.
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
 * "info elsc" from the monitor dumps the console-input queue and the
 * arbitration signal the controller presents for the console processor. With
 * nothing typed the queue is empty and code 0 presents signal1 -- the value
 * that withholds "get" from the driver -- so the dump has to name that state
 * rather than claim input is waiting.
 */
static void test_info_elsc_dump_empty(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    g_autofree char *out = qtest_hmp(qts, "info elsc");

    g_assert_nonnull(strstr(out, "rx queue: 0 byte(s)"));
    g_assert_nonnull(strstr(out, "signal1"));
    g_assert_nonnull(strstr(out, "console input idle"));

    qtest_quit(qts);
}

/*
 * Once input is typed on the AUX line the same dump must show the queued bytes
 * and report code 0 presenting signal2, the value that tells the driver a
 * "get" will find something. Driving the arbitration wait to signal2 first
 * makes the byte's arrival at the receive path deterministic, exactly as
 * firmware waits before it polls.
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
    qtest_add_data_func("sn0/elsc/arbitration-three-loop-wait", NULL,
                        test_arbitration_three_loop_wait);
    qtest_add_data_func("sn0/elsc/arbitration-repeats", NULL,
                        test_arbitration_repeats);
    qtest_add_data_func("sn0/elsc/arbitration-visits-other-processor-codes",
                        NULL, test_arbitration_visits_other_processor_codes);
    qtest_add_data_func("sn0/elsc/arbitration-preserves-staged-start", NULL,
                        test_arbitration_preserves_staged_start);
    qtest_add_data_func("sn0/elsc/acp-command-writes-to-console", NULL,
                        test_acp_command_writes_to_console);
    qtest_add_data_func("sn0/elsc/acp-with-no-backend-still-answers", NULL,
                        test_acp_with_no_backend_still_answers);
    qtest_add_data_func("sn0/elsc/get-returns-queued-console-input", NULL,
                        test_get_returns_queued_console_input);
    qtest_add_data_func("sn0/elsc/escape-byte-is-not-queued", NULL,
                        test_escape_byte_is_not_queued);
    qtest_add_data_func("sn0/elsc/supervisor-gating", NULL,
                        test_supervisor_gating);
    qtest_add_data_func("sn0/elsc/boot-acceptance-sequence", NULL,
                        test_boot_acceptance_sequence);
    qtest_add_data_func("sn0/elsc/info-elsc-dump-empty", NULL,
                        test_info_elsc_dump_empty);
    qtest_add_data_func("sn0/elsc/info-elsc-dump-shows-queued-input", NULL,
                        test_info_elsc_dump_shows_queued_input);

    ret = g_test_run();

    return ret;
}
