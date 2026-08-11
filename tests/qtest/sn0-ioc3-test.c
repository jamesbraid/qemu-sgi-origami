/*
 * SGI IOC3 base I/O controller: the serial console's access contract.
 *
 * The console is a pair of byte-wide register files inside a window that is
 * otherwise thirty-two bits wide, and the datasheet says plainly that assuming
 * symmetry with the chip's other blocks produces a console that emits nothing.
 * The stride and the width are what this file checks, because they are the
 * parts a working boot does not prove: a guest that only ever issues byte
 * accesses behaves identically whether or not the model has the width right,
 * so the transcript cannot tell the difference. Only a test issuing what the
 * guest does not can.
 *
 * The last assertion here is the one that ties the device to the firmware. The
 * configuration graph tells a guest where its console is, and the device has to
 * answer at exactly that address. Those two facts live in different files and
 * were wrong together three times before they were right, so the test reads the
 * address out of the graph rather than restating it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* Crosstalk space, and the chip's window inside the console widget. */
#define SPACE_XTALK     0x20000000000ULL
#define CONSOLE_WIDGET  8
#define BRIDGE_DEVICE2  0x600000
#define IOC3_BASE       (SPACE_XTALK + ((uint64_t)CONSOLE_WIDGET << 24) + \
                         BRIDGE_DEVICE2)

/* The two ports, at their offsets inside the byte-wide SuperIO window. */
#define SERIAL_A        (IOC3_BASE + 0x020178)
#define SERIAL_B        (IOC3_BASE + 0x020170)

/* 16550 registers this test touches, by offset from a port's base. */
/* The SuperIO interrupt block, in the chip's main window. */
#define INT_STATUS      (IOC3_BASE + 0x1c)
#define INT_ENABLE_SET  (IOC3_BASE + 0x20)
#define INT_ENABLE_CLR  (IOC3_BASE + 0x24)

#define INT_SERIAL_A    (1u << 6)
#define INT_SERIAL_B    (1u << 15)
#define INT_KEYBOARD    (1u << 22)   /* a latched bit, for contrast */

#define REG_LCR         3
#define REG_MCR         4
#define REG_SCR         7

/* Where the graph keeps the console descriptor, and its serial-port base. */
#define CONFIG_GRAPH    0x4000
#define GRAPH_CONSOLE   0x18

static void test_ports_are_distinct(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The scratch register holds whatever it is given, so it is the cheapest
     * place to prove the two files are two files. If the ports aliased, the
     * second write would be visible through the first.
     */
    qtest_writeb(qts, SERIAL_A + REG_SCR, 0x5a);
    qtest_writeb(qts, SERIAL_B + REG_SCR, 0xa5);

    g_assert_cmphex(qtest_readb(qts, SERIAL_A + REG_SCR), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, SERIAL_B + REG_SCR), ==, 0xa5);

    qtest_quit(qts);
}

static void test_registers_are_a_byte_apart(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Line control and modem control are adjacent and both hold what they are
     * given, so writing different values to consecutive offsets and reading
     * both back proves the stride is one byte. At the four-byte stride the
     * chip's other blocks use, these two writes would land in the same
     * register and the second would win.
     *
     * The line-control value is eight data bits with one stop bit and the
     * divisor latch not selected, which is what a guest writes and what keeps
     * the neighbouring offsets meaning what this test expects.
     */
    qtest_writeb(qts, SERIAL_A + REG_LCR, 0x03);
    qtest_writeb(qts, SERIAL_A + REG_MCR, 0x05);

    g_assert_cmphex(qtest_readb(qts, SERIAL_A + REG_LCR), ==, 0x03);
    g_assert_cmphex(qtest_readb(qts, SERIAL_A + REG_MCR), ==, 0x05);

    qtest_quit(qts);
}

static void test_a_wide_access_is_not_narrowed(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint32_t wide;

    /*
     * Four distinct bytes across four consecutive registers, then one four-byte
     * read of the same place.
     *
     * What this asserts is negative on purpose. The datasheet records that
     * nothing establishes what the hardware does with an access wider than a
     * byte, so the model refuses it rather than inventing a result, and that
     * refusal is a choice the datasheet marks as a choice. What must not happen
     * is the silent narrowing the register file's own wrapper performs, where a
     * four-byte read returns one register's value as though it were the word.
     * That is an unknown presented as known, the one behaviour with nothing to
     * recommend it.
     *
     * So the test does not assert what a wide read returns. It asserts that the
     * answer is not the narrowed one, which is the only thing that can be said
     * without claiming to know something nobody has observed.
     */
    qtest_writeb(qts, SERIAL_A + REG_LCR, 0x03);
    qtest_writeb(qts, SERIAL_A + REG_MCR, 0x05);

    wide = qtest_readl(qts, SERIAL_A + REG_LCR);
    g_assert_cmphex(wide, !=, 0x03);
    g_assert_cmphex(wide, !=, 0x03000000);

    qtest_quit(qts);
}

static void test_port_answers_where_the_graph_says(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t advertised = qtest_readq(qts, CONFIG_GRAPH + GRAPH_CONSOLE);
    uint64_t reached;

    /*
     * The single assertion joining this device to the firmware that describes
     * it. The graph's console descriptor is what a guest reads to find its
     * console, and it has been wrong three times: once holding the value that
     * merely identifies the controller generation, once holding the right
     * register offset with no device window above it, and once right. Each
     * wrong value produced a machine that identified its console correctly and
     * then wrote to something that was not one.
     *
     * Reading the address out of the graph rather than restating it keeps the
     * two from drifting apart: if either the descriptor or the mapping moves,
     * this fails. Restating the constant here would have passed against every
     * one of the three wrong values.
     */
    /*
     * The two are in different address spaces and the difference is the point.
     * The graph holds an address a *guest* issues: an unmapped 64-bit window
     * reference carrying the uncached attribute that routes it to a crosstalk
     * widget. A test pokes the machine's own bus, where that attribute has
     * already been turned into a window. So the descriptor is decoded here the
     * way the processor decodes it, attribute 1 selecting the crosstalk window
     * and the address below it being the node offset, and the result has to be
     * where the device was mapped.
     *
     * Comparing them undecoded would fail on a correct machine, and asserting
     * only the raw value would pass on a machine whose device sat somewhere
     * else entirely. Decoding is what makes this one assertion cover both the
     * firmware's claim and the machine's placement.
     */
    g_assert_cmphex(advertised >> 62, ==, 2);            /* unmapped window */
    g_assert_cmphex((advertised >> 57) & 3, ==, 1);      /* crosstalk I/O */
    reached = SPACE_XTALK | (advertised & ((1ULL << 40) - 1));
    g_assert_cmphex(reached, ==, SERIAL_A);

    /* And something answers there, which no assertion about the graph shows. */
    qtest_writeb(qts, reached + REG_SCR, 0x3c);
    g_assert_cmphex(qtest_readb(qts, reached + REG_SCR), ==, 0x3c);

    qtest_quit(qts);
}

static void test_the_enable_mask_has_two_write_aliases(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * One mask behind two addresses, so a guest can turn a source on or off
     * without a read-modify-write. The set alias reads back the live mask,
     * which is what says they are two views of one register rather than two
     * registers — a model with two independent words would pass any test that
     * only ever wrote and read the same address.
     */
    g_assert_cmphex(qtest_readl(qts, INT_ENABLE_SET), ==, 0);

    qtest_writel(qts, INT_ENABLE_SET, INT_SERIAL_A | INT_SERIAL_B);
    g_assert_cmphex(qtest_readl(qts, INT_ENABLE_SET), ==,
                    INT_SERIAL_A | INT_SERIAL_B);

    /* Clearing through the other alias reaches the same mask. */
    qtest_writel(qts, INT_ENABLE_CLR, INT_SERIAL_B);
    g_assert_cmphex(qtest_readl(qts, INT_ENABLE_SET), ==, INT_SERIAL_A);

    /* And a one in the set alias does not disturb what is already there. */
    qtest_writel(qts, INT_ENABLE_SET, INT_KEYBOARD);
    g_assert_cmphex(qtest_readl(qts, INT_ENABLE_SET), ==,
                    INT_SERIAL_A | INT_KEYBOARD);

    qtest_quit(qts);
}

static void test_a_serial_bit_ignores_an_acknowledgement(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The two serial bits are pass-throughs: they follow the 16550's own
     * interrupt level rather than latching, so writing one back cannot clear
     * it. This is the assertion that catches a model treating the whole status
     * register as write-one-to-clear — which looks right until a guest's
     * handler writes back everything it serviced, drops a level that is still
     * asserted, and never takes that interrupt again.
     *
     * The port has to be actually asserting for this to test anything, and the
     * first version of this test did not arrange that: with the port quiet the
     * bit is zero, clearing zero and ignoring zero are the same, and the test
     * passed against a model that cleared the bit. It was written with a
     * comment explaining why the bit might be zero, which is the tell — an
     * assertion whose own comment excuses the case that makes it vacuous.
     *
     * So the port is made to assert first. Enabling the transmitter-empty
     * interrupt raises the 16550 immediately, because its holding register is
     * empty from reset and stays empty with nothing being sent.
     */
    qtest_writeb(qts, SERIAL_A + 1, 0x02);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_SERIAL_A, ==,
                    INT_SERIAL_A);

    /* Now the acknowledgement, which must not touch it. */
    qtest_writel(qts, INT_STATUS, INT_SERIAL_A);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_SERIAL_A, ==,
                    INT_SERIAL_A);

    /*
     * And what does quiet it is the port, not this register: turning the
     * 16550's own interrupt off drops the level, and the pass-through bit
     * follows it down with nothing written here at all.
     */
    qtest_writeb(qts, SERIAL_A + 1, 0x00);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_SERIAL_A, ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/ioc3/ports-are-distinct", NULL,
                        test_ports_are_distinct);
    qtest_add_data_func("sn0/ioc3/registers-are-a-byte-apart", NULL,
                        test_registers_are_a_byte_apart);
    qtest_add_data_func("sn0/ioc3/wide-access-is-not-narrowed", NULL,
                        test_a_wide_access_is_not_narrowed);
    qtest_add_data_func("sn0/ioc3/enable-mask-two-aliases", NULL,
                        test_the_enable_mask_has_two_write_aliases);
    qtest_add_data_func("sn0/ioc3/serial-bit-ignores-acknowledgement", NULL,
                        test_a_serial_bit_ignores_an_acknowledgement);
    qtest_add_data_func("sn0/ioc3/port-answers-where-the-graph-says", NULL,
                        test_port_answers_where_the_graph_says);

    return g_test_run();
}
