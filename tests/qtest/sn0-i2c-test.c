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

#define I2C_PCF_PIN  0x80
#define I2C_PCF_LRB  0x08
#define I2C_PCF_AAS  0x04
#define I2C_PCF_BB  0x01

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

/*
 * The programmed own address must make the chip answer as a slave, not
 * just read back: a transfer directed at it is acknowledged and the
 * status register reports AAS. The only master on this machine is the
 * chip itself, so this drives the chip's own master side at its own
 * slave address -- a loopback a real bus permits and QEMU's core
 * matches the same way as any other directed transfer.
 */
static void test_programmed_own_address_answers_as_slave(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* Program S0' = 0x2a, an address nothing else on this bus answers. */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x2a);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x2a);

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

    /* Control: a neighbouring unprogrammed address still finds nobody. */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x2b << 1);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & I2C_PCF_LRB,
                    ==, I2C_PCF_LRB);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);

    /* The programmed address is acknowledged and reports AAS. */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x2a << 1);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & I2C_PCF_LRB,
                    ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & I2C_PCF_AAS,
                    ==, I2C_PCF_AAS);

    /* The PIN written with the stop clears the addressed-as-slave state. */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & I2C_PCF_AAS,
                    ==, 0);

    qtest_quit(qts);
}

/*
 * A byte sent after a refused start goes out on a wire nobody
 * acknowledged: the chip must keep reporting NAK rather than launder
 * the failure into an ACK. QEMU's i2c_send() over the bus's empty
 * matched-device list reports success -- the loop body never runs and
 * the accumulated return value stays zero -- so the model has to
 * remember the refusal instead of asking the core again.
 */
static void test_send_after_refused_start_stays_nacked(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

    /* Start to an address nothing answers. */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x2b << 1);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & I2C_PCF_LRB,
                    ==, I2C_PCF_LRB);

    /* The transaction is open at the chip level: the bus reads busy. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & I2C_PCF_BB,
                    ==, 0);

    /* A data byte into the void must stay NAKed. */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x99);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & I2C_PCF_LRB,
                    ==, I2C_PCF_LRB);

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & I2C_PCF_BB,
                    ==, I2C_PCF_BB);

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
                    I2C_PCF_PIN | I2C_PCF_BB);

    /* And the real address is still there once ESO is cleared to look. */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x0a);

    qtest_quit(qts);
}

static void test_liveness_probe_silent_chip_is_stable(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The probe loop returns once two consecutive reads differ; asserting
     * stability here tells a stable data register from writes leaking
     * straight back out.
     */
    uint64_t first = qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, first);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, first);

    qtest_quit(qts);
}

/*
 * The closing invalid-address step, checked by identity rather than by
 * the ACK bit: an i2c-echo fixture at 0x7f must receive the specific
 * byte 0xfe, control write before address write. The ACK bit alone
 * false-passes -- i2c_send() over an empty matched list reads ACK 0 for
 * bug and no-bug alike -- so three known bytes are sent and read back,
 * which fails whenever the write half missed the device by any
 * mechanism. The payload is a fixture, not a protocol claim.
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
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & I2C_PCF_LRB, ==,
                    I2C_PCF_LRB);                    /* NAKed: empty */

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
     * One discard, then the three echoed bytes: the first read returns the
     * chip's own stale data register, because the fetch attaches to the
     * read rather than the start.
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
    qtest_add_data_func("sn0/i2c/programmed-own-address-answers-as-slave",
                        NULL, test_programmed_own_address_answers_as_slave);
    qtest_add_data_func("sn0/i2c/send-after-refused-start-stays-nacked",
                        NULL, test_send_after_refused_start_stays_nacked);
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
