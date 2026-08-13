/*
 * The bring-up serial port on the Hub's junk bus, and the firmware probe that
 * decides whether the machine has one.
 *
 * Real firmware picks its console by trying candidates in a caller-supplied
 * order and stopping at the first that answers present. This port heads every
 * one of those orders, so the answer it gives here decides which console the
 * whole boot runs on: answer present and the console is this port, answer
 * absent and the firmware goes on through the base I/O controller and the
 * module system controller, which is what a real Origin 200 does because a
 * real Origin 200 has no such part fitted. What follows this port varies
 * between the lists; that this port comes first does not.
 *
 * So both answers have to be tested, and neither test means anything without
 * the other. A machine that has no register at this address at all also
 * "answers absent", and an absent-only test passes against it — against a
 * region drawn too narrow, a device that failed to realize, an address off by
 * a window. The fitted case is what proves the absent case was measured at the
 * right place rather than into empty space.
 *
 * The sequence tested is the one the firmware executes, which is not the one
 * the interface's driver contains. The driver writes 0x55, reads it back, and
 * only then repeats the whole thing with 0xAA — so on a machine without the
 * port the 0xAA half never runs, and a check written as "0x55 then 0xAA" can
 * only pass where the answer is already yes. What discriminates is the first
 * write, the interrupt-enable write beside it, the read-back, and the value
 * the read-back returns.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* Crosstalk space, and the two halves of the crossbar's widget 1 window. */
#define SPACE_XTALK     0x20000000000ULL
#define HUB_LOCAL       (SPACE_XTALK + 0x01000000)
#define HUB_BY_NODE     (SPACE_XTALK + 0x01800000)

/*
 * The port's eight registers, at the Hub-local offsets the public Linux header
 * arch/mips/include/asm/sn/sn0/hubmd.h gives for them: one register per eight
 * bytes, starting at MD_UREG1_8. The two the probe touches are the scratch
 * register, which is index 7 and so the eighth and last in the standard 16550
 * order, and the interrupt-enable register, which is index 1.
 */
#define JUNK_UART_BASE      0x2200c0
#define JUNK_UART_STRIDE    8
#define JUNK_UART_REG(n)    (JUNK_UART_BASE + (n) * JUNK_UART_STRIDE)
#define JUNK_UART_IER       JUNK_UART_REG(1)
#define JUNK_UART_SCRATCH   JUNK_UART_REG(7)

/* What the firmware's probe writes, and what it requires to read back. */
#define PROBE_PATTERN       0x55

/*
 * Run the probe exactly as the firmware runs it and hand back the eight raw
 * bytes the read-back produced. Raw bytes rather than a decoded number,
 * because two of the three things that can be wrong here are about which byte
 * of the eight carries the register, and a helper that decodes a number has
 * already thrown that away.
 */
static void run_the_probe(QTestState *qts, uint64_t window, uint8_t out[8])
{
    qtest_writeq(qts, window + JUNK_UART_SCRATCH, PROBE_PATTERN);
    qtest_writeq(qts, window + JUNK_UART_IER, 0);
    qtest_memread(qts, window + JUNK_UART_SCRATCH, out, 8);
}

/*
 * The default machine has no such port, and says so the way real hardware
 * says so: the read-back does not return what was written.
 */
static void test_the_default_machine_has_no_bring_up_port(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint8_t got[8];

    run_the_probe(qts, HUB_LOCAL, got);

    for (int i = 0; i < 8; i++) {
        g_assert_cmphex(got[i], ==, 0x00);
    }

    qtest_quit(qts);
}

/*
 * Fitted, the probe succeeds — and the whole eight bytes are asserted, rather
 * than the low byte of them.
 *
 * Three different defects put a wrong value at this address, and each puts a
 * different one. A model that let the memory system split the guest's 64-bit
 * access into two 32-bit calls answers with the byte twice, once in each half,
 * because both calls decode to the same register and are assembled into
 * different halves of the result — which is how this test counts handler
 * dispatches without needing to watch the handler. A model declaring the wrong
 * endianness answers all zeroes, because the 16550's write path keeps the low
 * byte of what it is handed and a byte-swapped 0x55 has none. And a model with
 * nothing behind the address answers all zeroes too.
 *
 * Those last two give the same answer, which is why this test and the absent
 * one are a pair rather than two tests: the absent test cannot tell a correct
 * model from a byte-swapped one, and this one can.
 */
static void test_the_fitted_port_answers_the_probe(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,junk-uart=on");
    uint8_t got[8];

    run_the_probe(qts, HUB_LOCAL, got);

    for (int i = 0; i < 7; i++) {
        g_assert_cmphex(got[i], ==, 0x00);
    }
    g_assert_cmphex(got[7], ==, PROBE_PATTERN);

    qtest_quit(qts);
}

/*
 * The port sits inside the Hub's register window, so it is reached through
 * both of the addresses that window answers at: the one resolving to whichever
 * node the reading processor is on, and the one naming a node. A chip on the
 * Hub's local bus is reachable both ways, and a model that placed the port
 * beside the window in the address space instead of inside it would answer at
 * one and not the other.
 *
 * Nothing in a boot would catch that. Real firmware only ever uses the
 * node-local address for this port, so every test that follows the firmware
 * passes against a machine where the other view is empty.
 */
static void test_the_fitted_port_answers_through_both_windows(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,junk-uart=on");
    uint8_t got[8];

    run_the_probe(qts, HUB_BY_NODE, got);

    for (int i = 0; i < 7; i++) {
        g_assert_cmphex(got[i], ==, 0x00);
    }
    g_assert_cmphex(got[7], ==, PROBE_PATTERN);

    qtest_quit(qts);
}

/*
 * Both ends of the port, which is what keeps it from swallowing a neighbour.
 * Eight registers at eight bytes each occupy 0x22_00c0 through 0x22_00ff; the
 * register below that range and the register above it are the Hub's, and the
 * Hub implements neither, so both answer zero. Writing a pattern to each and
 * reading it back must not return the pattern.
 *
 * Both ends, because a region can be misplaced in either direction and the
 * probe test cannot see it happen. A region drawn one register low still
 * answers the scratch register — at the offset the probe uses, from the wrong
 * place — and one drawn too large answers correctly everywhere the probe
 * looks while covering an address belonging to somebody else. Testing one end
 * catches one of those.
 */
static void test_the_fitted_port_stays_inside_its_own_range(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,junk-uart=on");
    uint64_t below = JUNK_UART_BASE - JUNK_UART_STRIDE;
    uint64_t above = JUNK_UART_BASE + 8 * JUNK_UART_STRIDE;

    qtest_writeq(qts, HUB_LOCAL + below, PROBE_PATTERN);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + below), ==, 0);

    qtest_writeq(qts, HUB_LOCAL + above, PROBE_PATTERN);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + above), ==, 0);

    qtest_quit(qts);
}

/*
 * A register the probe never touches is the port's too.
 *
 * The probe reads only the scratch register, which is the last of the eight,
 * and it writes the interrupt-enable register without ever reading it back. So
 * the probe test alone is satisfied by a model that answers at one offset, and
 * this reads a second register that holds what was written to it. It is one
 * more register, not the whole file: registers 0 and 2 through 6 are still
 * untested here, and the ones with side effects want a character device behind
 * the port before there is anything worth asserting about them.
 */
static void test_the_fitted_port_answers_a_second_register(const void *d)
{
    QTestState *qts = qtest_init("-M origin200,junk-uart=on");
    uint8_t got[8];

    /*
     * Two of the interrupt-enable register's low four bits. The 16550 keeps
     * the low four and drops the rest, so the value has to live in them; it is
     * neither zero nor all ones, and its low nibble differs from the probe
     * pattern's, so a model that leaked the scratch register's value here
     * would not pass. The modem-status bit is left out because setting it
     * sends the 16550 to ask the character device about modem lines, which is
     * a side trip this test has no reason to take.
     */
    qtest_writeq(qts, HUB_LOCAL + JUNK_UART_IER, 0x06);
    qtest_memread(qts, HUB_LOCAL + JUNK_UART_IER, got, 8);

    for (int i = 0; i < 7; i++) {
        g_assert_cmphex(got[i], ==, 0x00);
    }
    g_assert_cmphex(got[7], ==, 0x06);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/junk-uart/absent-by-default", NULL,
                        test_the_default_machine_has_no_bring_up_port);
    qtest_add_data_func("sn0/junk-uart/fitted-answers-the-probe", NULL,
                        test_the_fitted_port_answers_the_probe);
    qtest_add_data_func("sn0/junk-uart/fitted-through-both-windows", NULL,
                        test_the_fitted_port_answers_through_both_windows);
    qtest_add_data_func("sn0/junk-uart/stays-inside-its-range", NULL,
                        test_the_fitted_port_stays_inside_its_own_range);
    qtest_add_data_func("sn0/junk-uart/answers-a-second-register", NULL,
                        test_the_fitted_port_answers_a_second_register);

    return g_test_run();
}
