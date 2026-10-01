/*
 * Origin 200 PCF8584 I2C register, selection-matrix, own-address liveness
 * and master-transfer tests; no responder traffic is required.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-test.h"

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
     * but the register-selection matrix requires distinct decoding. */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ES2);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x33);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x33);

    /* ESO=1 ES1=0 ES2=0: the working data register. Outside any transfer,
     * a plain byte store. */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x55);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x55);

    /*
     * ESO=1 ES2=1: still the vector. This is the row a model treating "ESO
     * set"
     * as "always data" gets wrong.
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

    /* A constant-answering register would still pass the half above. */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x0a);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x0a);

    qtest_quit(qts);
}

/*
 * The programmed own address must be answered as a slave, not just read
 * back; the chip's own master side drives it, a loopback real buses allow.
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
 * A general call reaches every slave, the controller's own broadcast receiver
 * included, which is how the ELSC's arbitration broadcast lands in the data
 * register for the firmware to poll. That receive must not raise
 * addressed-as-slave: only a directed hit on the programmed address is an AAS
 * event. When a broadcast raised AAS too, every arbitration signal left the
 * controller looking addressed as a slave, and the firmware's arbitration
 * poll wedged before it ever issued the DIP-switch read. The boot never
 * reached the local-POD stop.
 */
static void test_general_call_does_not_address_as_slave(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* Program an own address so a directed hit would be the AAS case. */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x2a);

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

    /* Start to the general-call address 0x00: it reaches the broadcast RX. */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x00);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);

    /* The general call is acknowledged: a receiver exists... */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & I2C_PCF_LRB,
                    ==, 0);
    /* ...but it must not report the controller addressed as a slave. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & I2C_PCF_AAS,
                    ==, 0);

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);
    qtest_quit(qts);
}

/*
 * After a refused start the chip must keep reporting NAK: i2c_send() over
 * an empty matched list reports success, so the refusal must be remembered.
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

    /* 12 MHz / 90 kHz is the only combination our firmware ever writes. */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x1c);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, 0x1c);

    /*
     * 6 MHz / 11 kHz is a combination nothing we boot ever uses, so a model
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

    /* The nine-write, one-read initialization sequence the PROM driver
     * issues, replayed write for write. */
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

    /* Stability tells a real register from writes leaking straight back. */
    uint64_t first = qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, first);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_DATA), ==, first);

    qtest_quit(qts);
}

/*
 * The closing invalid-address step, checked by identity: the ACK bit alone
 * reads 0 for bug and no-bug alike, so an i2c-echo fixture at 0x7f must
 * hand back the bytes only this test sends.
 */
static void test_closing_invalid_address_shift_and_order(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 -device i2c-echo,address=0x7f,bus=i2c");

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);

    /* Open a transfer to an unanswered address (not 0x08: the ELSC ACKs). */
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0x12);        /* addr 0x09, write */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL) & I2C_PCF_LRB, ==,
                    I2C_PCF_LRB);                    /* NAKed: empty */

    /* Control first with PIN absent, then the address, then the payload. */
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0xfe);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0xaa);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0xbb);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, 0xcc);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);

    /* One discard first: the fetch attaches to the read, not the start. */
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
    qtest_add_data_func("sn0/i2c/general-call-does-not-address-as-slave",
                        NULL, test_general_call_does_not_address_as_slave);
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
