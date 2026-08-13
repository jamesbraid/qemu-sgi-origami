/*
 * SGI Origin 200 -- the PCF8584 I2C-bus controller's own register contract.
 *
 * Everything here is testable with no traffic from the system-controller
 * responder: the register-selection matrix, the own-address liveness check
 * firmware's init sequence depends on, and the transaction machinery a
 * master transfer drives. docs/datasheets/pcf8584-elsc.md is the spec this
 * was built from.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* Crosstalk space, and the half of the crossbar's widget 1 window this
 * processor's own node resolves to -- same constants sn0-hub-test.c uses. */
#define SPACE_XTALK     0x20000000000ULL
#define HUB_LOCAL       (SPACE_XTALK + 0x01000000)

#define I2C_DATA        0x220000  /* S0/S0'/S2/S3, selected by control */
#define I2C_CTRL        0x220008  /* S1: control bits, or status once ESO */

#define CTL_PIN  0x80
#define CTL_ESO  0x40
#define CTL_ES1  0x20
#define CTL_ES2  0x10
#define CTL_ENI  0x08
#define CTL_STA  0x04
#define CTL_STO  0x02
#define CTL_ACK  0x01

#define STA_PIN  0x80
#define STA_LRB  0x08
#define STA_BNB  0x01

static void test_register_selection_matrix(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* ESO=0 ES1=0 ES2=0: own address. */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x11);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x11);

    /* ESO=0 ES1=1 ES2=0: clock control. A model conflating this with own
     * address would return 0x11 here instead. */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ES1);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x1c);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x1c);

    /* ESO=0 ES1=0 ES2=1: interrupt vector. Never touched by our firmware,
     * but the datasheet's own matrix requires it decode distinctly. */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ES2);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x33);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x33);

    /* ESO=1 ES1=0 ES2=0: the working data register -- outside any transfer,
     * a plain byte store. */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x55);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x55);

    /*
     * ESO=1 ES2=1: still the interrupt vector, not the data register. This
     * is the row a model that treats "ESO set" as "this address is always
     * the data register" gets wrong -- it would answer 0x55 here instead of
     * the vector's own 0x33, and the two registers would in fact be the
     * same storage rather than genuinely independent.
     */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_ES2);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x33);

    /* Own address and clock control are undisturbed by any of the above. */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x11);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ES1);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x1c);

    qtest_quit(qts);
}

static void test_own_address_round_trip(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN);

    /* The placeholder firmware's init sequence actually reads back. */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x7f);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x7f);

    /*
     * The slot-derived real address, which firmware writes but never reads
     * back. Asserted anyway: a register that only ever answers a constant
     * would still pass the placeholder half above.
     */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x0a);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x0a);

    qtest_quit(qts);
}

static void test_clock_control_independent_fields(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ES1);

    /* 12 MHz / 90 kHz -- the only combination our firmware ever writes. */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x1c);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x1c);

    /*
     * 6 MHz / 11 kHz -- a combination nothing we boot ever uses, so a model
     * that hard-coded 0x1c rather than storing what was written would still
     * pass the line above and only fail here.
     */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x0a);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x0a);

    qtest_quit(qts);
}

static void test_init_sequence_leaves_eso_and_address(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* The nine-write, one-read sequence docs/datasheets/pcf8584-elsc.md
     * transcribes from the PROM driver, replayed write for write. */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN);                 /* 1 */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x7f);                    /* 2 */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ES1);       /* 3 */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x1c);                    /* 4 */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN);                 /* 5 */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x7f); /* 6 */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN);                 /* 7 */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x0a);                    /* 8 */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);       /* 9 */

    /* The serial interface is enabled: the control/status address now
     * answers the status half, idle. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL), ==,
                    STA_PIN | STA_BNB);

    /* And the real address is still there once ESO is cleared to look. */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x0a);

    qtest_quit(qts);
}

static void test_liveness_probe_silent_chip_is_stable(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Nothing on this machine broadcasts yet at this milestone. The real
     * driver's probe reads this register in a tight loop and returns only
     * once two consecutive reads differ; asserting it does not here is what
     * distinguishes "the chip answers a stable data register" from "the
     * chip answers whatever a guest wrote," which a model that let writes
     * leak straight back out during a probe would fail silently.
     */
    uint64_t first = qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, first);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, first);

    qtest_quit(qts);
}

/*
 * The closing invalid-address step: every transaction that moves data ends
 * with a repeated start to 0x7f, control register written before the
 * address, which every real PCF8584 NAKs. Testing "nothing answers" only
 * proves an empty bus stays empty; to prove the *specific* byte 0xfe (0x7f
 * shifted left with a clear direction bit) reaches the wire, and that it
 * reaches it only once the control write precedes the address write, this
 * attaches a generic i2c-echo responder at 0x7f as a test fixture and
 * checks it by identity, not by the model's own ACK bit.
 *
 * The ACK bit alone is not that check: an earlier version of this test
 * asserted only the status register's NAK bit after the sequence, and it
 * kept passing against a model that always started fresh on the control
 * write, using whatever stale byte the data register still held from the
 * transaction being closed rather than the 0xfe written after it. That
 * model reached no device at all, but qtest_readq(I2C_CTRL) & STA_LRB read
 * 0 anyway -- QEMU's i2c_send() loops over the bus's currently-matched
 * device list, and an empty list makes the loop body never run, so its
 * accumulated return value stays the zero it was initialised to. Bug and
 * no-bug produce the identical ACK reading, for unrelated reasons, and the
 * two-variant check the datasheet asks for is exactly what caught it.
 *
 * So this sends three known bytes after the address instead of stopping
 * right away -- more than real firmware ever does at this point, since the
 * datasheet's own close is address-then-stop, but the payload is a test
 * fixture, not a claim about the protocol -- then opens a fresh, ordinary
 * read of the same address and checks those three bytes come back. i2c-echo
 * can only return what it was actually sent, so this fails whenever the
 * write half missed the device, by any mechanism, not only the one already
 * caught once.
 */
static void test_closing_invalid_address_shift_and_order(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 -device i2c-echo,address=0x7f,bus=i2c");

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

    /* Open an ordinary transaction to some other, unanswered address, so a
     * transfer is active -- the precondition every repeated start needs.
     * 0x08 is the ELSC's own address now that it exists on the bus and
     * would ACK, which would defeat this precondition, so this uses an
     * address nothing on the machine answers. */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x12);        /* addr 0x09, write */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & STA_LRB, ==,
                    STA_LRB);                              /* NAKed: empty */

    /* The closing repeated start: control first, with PIN deliberately
     * absent, then the address byte -- then three bytes only this test
     * sends, to identify who actually received the start. */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0xfe);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0xaa);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0xbb);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0xcc);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);

    /*
     * A fresh, ordinary read of 0x7f. i2c-echo answers with exactly the
     * three bytes it was sent, in order. The discarded first read returns
     * this chip's own data register as it stood before this start -- the
     * 0xff address-and-direction byte just written below -- and it is that
     * read, not the start condition, that fetches the first real byte
     * (docs/datasheets/pcf8584-elsc.md: "the driver reads the data
     * register once and throws the byte away"; hw/i2c/pcf8584.c attaches
     * the fetch to the read, per the Philips product specification). So
     * the sequence is one discard of stale chip state, then all three
     * echoed bytes in order.
     */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0xff);        /* addr 0x7f, read */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    (void)qtest_readq(qts, HUB_LOCAL + I2C_DATA);          /* discarded: 0xff */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0xaa);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0xbb);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO);      /* clear ACK: last byte */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0xcc);

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/i2c/register-selection-matrix", NULL,
                        test_register_selection_matrix);
    qtest_add_data_func("sn0/i2c/own-address-round-trip", NULL,
                        test_own_address_round_trip);
    qtest_add_data_func("sn0/i2c/clock-control-independent-fields", NULL,
                        test_clock_control_independent_fields);
    qtest_add_data_func("sn0/i2c/init-sequence-leaves-eso-and-address", NULL,
                        test_init_sequence_leaves_eso_and_address);
    qtest_add_data_func("sn0/i2c/liveness-probe-silent-chip-is-stable", NULL,
                        test_liveness_probe_silent_chip_is_stable);
    qtest_add_data_func("sn0/i2c/closing-invalid-address-shift-and-order",
                        NULL, test_closing_invalid_address_shift_and_order);

    ret = g_test_run();

    return ret;
}
