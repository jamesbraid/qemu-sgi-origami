/*
 * SGI IOC3 base I/O controller: the serial console's access contract, the
 * one-wire station-address part, the ethernet engine, the buffer RAM, and
 * the timekeeper. The console's byte stride and width are what a working
 * boot cannot prove; the console address is read out of the configuration
 * graph so descriptor and mapping fail together.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "sn0-user-image.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

/*
 * The modelled minimum between a frame arriving and its completion, in
 * nanoseconds: the adapters' device default. Delivery is published only once
 * this much virtual time has passed, so every wait for a completion has to
 * advance the clock as well as give the main loop a pass.
 */
#define RX_ARRIVAL_LATENCY_NS  200000

/* Crosstalk space, and the chip's window inside the console widget. */
#define CONSOLE_WIDGET  8
#define BRIDGE_DEVICE2  0x600000
#define BRIDGE_DEVICE3  0x700000
#define IOC3_BASE       (SPACE_XTALK + ((uint64_t)CONSOLE_WIDGET << 24) + \
                         BRIDGE_DEVICE2)
#define TEST_IOC3_BASE  (SPACE_XTALK + ((uint64_t)CONSOLE_WIDGET << 24) + \
                         BRIDGE_DEVICE3)

/* A guest's bus scan: write the BAR, aim the bridge's device window at it. */
#define BRIDGE_BASE     (SPACE_XTALK + ((uint64_t)CONSOLE_WIDGET << 24))
#define REG_DATA        4         /* registers sit at a slot's upper half */
#define BRIDGE_CFG0     0x020000
#define CFG0_DEVICE_STRIDE 0x1000
#define CFG0(device)    (BRIDGE_BASE + BRIDGE_CFG0 + (device) * CFG0_DEVICE_STRIDE)
#define BRIDGE_DEVICE(n) (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)

#define PCI_IOC3_DEVICE  2         /* IP29 slot, restated for the test */
#define TEST_IOC3_DEVICE 3
#define IOC3_PCI_MMIO    0x10000000u
#define TEST_IOC3_PCI_MMIO 0x18000000u

/*
 * A node's crosstalk windows repeat node 0's one node stride up, and the
 * second node carries its own bridge and its own controller, so the same
 * attach sequence works there with every address offset by the stride.
 */
static void sn0_ioc3_attach_node(QTestState *qts, uint64_t node)
{
    qtest_writel(qts, node + CFG0(PCI_IOC3_DEVICE) + PCI_BAR0, IOC3_PCI_MMIO);
    qtest_writel(qts, node + CFG0(PCI_IOC3_DEVICE) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY);
    qtest_writel(qts, node + BRIDGE_DEVICE(PCI_IOC3_DEVICE),
                 DEV_MEMORY_SPACE | (IOC3_PCI_MMIO >> 20));
}

static void sn0_ioc3_attach(QTestState *qts)
{
    sn0_ioc3_attach_node(qts, 0);
}

/*
 * One plain origin200 serves every case that asks nothing special of the
 * command line, started in main(). Cases wanting a timekeeper, a second
 * node, an extra card or a migration destination start their own.
 */
static QTestState *qts;

/*
 * Open such a case. Reset turns the chip's memory decode back off, which
 * unmaps its window, so every case re-runs the attach sequence exactly as
 * it did after a boot. Checking the decode bit here is what proves the
 * previous case's attach is gone.
 */
static void ioc3_reset(void)
{
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, CFG0(PCI_IOC3_DEVICE) + PCI_COMMAND) &
                    PCI_COMMAND_MEMORY, ==, 0);
}

static void test_ioc3_attach(QTestState *qts)
{
    qtest_writel(qts, CFG0(TEST_IOC3_DEVICE) + PCI_BAR0,
                 TEST_IOC3_PCI_MMIO);
    qtest_writel(qts, CFG0(TEST_IOC3_DEVICE) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(TEST_IOC3_DEVICE),
                 DEV_MEMORY_SPACE | (TEST_IOC3_PCI_MMIO >> 20));
}

/* The two ports, at their offsets inside the byte-wide SuperIO window. */
#define SERIAL_A        (IOC3_BASE + 0x020178)
#define SERIAL_B        (IOC3_BASE + 0x020170)

/* 16550 registers this test touches, by offset from a port's base. */
/* The SuperIO interrupt block, in the chip's main window. */
#define INT_STATUS      (IOC3_BASE + 0x1c)
#define INT_ENABLE_SET  (IOC3_BASE + 0x20)
#define INT_ENABLE_CLR  (IOC3_BASE + 0x24)

/*
 * The chip control register and the read-only arbiter-status bit our boot's
 * serial setup loops on. Bit 22 reports the PCI arbiter idle; a read that
 * never sets it hangs the boot there.
 */
#define CHIP_CONTROL        (IOC3_BASE + 0x28)
#define CHIP_CTRL_ARB_IDLE  0x00400000u
#define CHIP_CTRL_STROBE    0x00078000u  /* writable, no status bits */

/*
 * The interrupt-output timer configuration at 0x2c. The interval count
 * (bits 0-15) and the normal mode (bits 16-18) are writable and read back;
 * the pin-state readback (bit 31) and the reserved field are not stored.
 * Diagnostic behavior remains outside the model's contract.
 */
#define INT_OUTPUT       (IOC3_BASE + 0x2c)
#define INT_OUT_DIAG     0x40000000u  /* bit 30: diagnostic mode */
#define INT_OUT_PIN      0x80000000u  /* bit 31: read-only pin readback */
#define INT_OUT_MODE_LOW  0x00000000u
#define INT_OUT_MODE_HIGH 0x00040000u
#define INT_OUT_MODE_ONE_SHOT 0x00050000u
#define INT_OUT_MODE_PERIODIC 0x00060000u
#define INT_OUT_MODE_SQUARE 0x00070000u
#define INT_OUT_TICK_NS    7800LL

/* A later, independent deadline exposes a stale interrupt-output timer. */
#define PI_RT_COUNT         (HUB_LOCAL + 0x030100)
#define PI_RT_COMPARE_A     (HUB_LOCAL + 0x000108)
#define PI_RT_PENDING_A     (HUB_LOCAL + 0x000120)
#define PI_RT_SENTINEL_TICKS 125ULL
#define PI_RT_SENTINEL_NS   100000LL

#define INT_SERIAL_A    (1u << 6)
#define INT_SERIAL_B    (1u << 15)
#define INT_KEYBOARD    (1u << 22)   /* a latched bit, for contrast */
#define INT_RT_OUTPUT_PULSE (1u << 27)

#define REG_LCR         3
#define REG_MCR         4
#define REG_SCR         7

/*
 * The Bridge's interrupt status register, where the controller's second
 * interrupt lands; a whole-register access needs no byte-lane correction.
 */
#define BRIDGE_INT_STATUS (BRIDGE_BASE + 0x100 + REG_DATA)
#define BRIDGE_PIN_SUPERIO (1u << 3)   /* IP29 routing, restated here */
/*
 * The ethernet interrupt travels the chip's PCI pin through the Bridge's
 * INTx routing: slot 2, pin A, lands on Bridge pin 2 -- a restatement of
 * sgi_bridge_map_irq() for this slot, not an independent constant.
 */
#define BRIDGE_PIN_ETH (1u << 2)

/*
 * The Bridge's interrupt-forwarding registers and the Hub's own pending
 * register, reached the same way sn0-xframe-test.c reaches them: a slot's
 * host-interrupt destination, the mode bit that decides whether a fall is
 * forwarded, the per-pin enable, and the Hub's PI_INT_PEND0/PEND_MOD pair a
 * driver reads and acknowledges.
 */
#define BRIDGE_INT_ENABLE (BRIDGE_BASE + 0x108 + REG_DATA)
#define BRIDGE_INT_MODE   (BRIDGE_BASE + 0x118 + REG_DATA)
#define BRIDGE_INT_HOST(n) (BRIDGE_BASE + 0x130 + 8 * (n) + REG_DATA)
#define PI_INT_PEND_MOD   0x90
#define IOC3_ETH_IRQ_NUMBER 12

/* The MicroLAN engine and the pin block it shares a window with. */
#define MICROLAN_CONTROL (IOC3_BASE + 0x30)
#define MICROLAN_LEVEL   (1u << 0)
#define MICROLAN_DONE    (1u << 1)

/*
 * OpenBSD's own reset-and-presence timing: 500 us low, 65 us remainder --
 * a real guest's reset slot, not an arbitrary write.
 */
#define MICROLAN_RESET_SLOT ((500u << 10) | (65u << 2))

#define PIN_CONTROL_SET  (IOC3_BASE + 0x34)
#define PIN_CONTROL_CLR  (IOC3_BASE + 0x38)
#define PIN_MICROLAN_ENABLE (1u << 21)

#define PIN_DATA         (IOC3_BASE + 0x3c)
#define PIN_REG(n)       (IOC3_BASE + 0x40 + 4 * (n))

static void test_ports_are_distinct(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * The scratch register holds whatever it is given, so it is the cheapest
     * place to prove the two files are two files. If the ports aliased, the
     * second write would be visible through the first.
     */
    qtest_writeb(qts, SERIAL_A + REG_SCR, 0x5a);
    qtest_writeb(qts, SERIAL_B + REG_SCR, 0xa5);

    g_assert_cmphex(qtest_readb(qts, SERIAL_A + REG_SCR), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, SERIAL_B + REG_SCR), ==, 0xa5);
}

static void test_registers_are_a_byte_apart(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * Different values at consecutive offsets prove the one-byte stride;
     * the LCR value leaves the divisor latch unselected.
     */
    qtest_writeb(qts, SERIAL_A + REG_LCR, 0x03);
    qtest_writeb(qts, SERIAL_A + REG_MCR, 0x05);

    g_assert_cmphex(qtest_readb(qts, SERIAL_A + REG_LCR), ==, 0x03);
    g_assert_cmphex(qtest_readb(qts, SERIAL_A + REG_MCR), ==, 0x05);
}

/*
 * The port's file is byte-wide, and a four-byte access to it is four
 * registers rather than one. Nothing rejects the wide access -- the address
 * space splits it into byte accesses on the way in, because the region says
 * a byte is the widest it takes -- so what this pins is that every lane
 * comes from its own register. A model declaring a four-byte width answers
 * the whole access out of the first register instead, which on the write
 * side would run that register's side effect once for four bytes.
 *
 * The upper half of the port: the four the split reaches, since the split
 * only goes as far as the address is aligned, and the four with no receive
 * register among them, since reading that one would empty the port. The
 * modem control and the scratch register take values of their own, so an
 * access answered from one register cannot match by accident. The two
 * status registers between them are read before the wide access rather
 * than asserted, so that reading the modem status has already cleared its
 * delta bits by the time the wide access reads it again.
 */
static void test_the_port_answers_a_byte_at_a_time(const void *data)
{
    uint8_t lane[4];
    uint32_t wide;
    unsigned int i;

    ioc3_reset();

    sn0_ioc3_attach(qts);

    qtest_writeb(qts, SERIAL_A + REG_MCR, 0x05);
    qtest_writeb(qts, SERIAL_A + REG_SCR, 0x5a);

    for (i = 0; i < ARRAY_SIZE(lane); i++) {
        lane[i] = qtest_readb(qts, SERIAL_A + REG_MCR + i);
    }
    g_assert_cmphex(lane[0], ==, 0x05);
    g_assert_cmphex(lane[3], ==, 0x5a);

    wide = qtest_readl(qts, SERIAL_A + REG_MCR);
    g_assert_cmphex(wide, ==, lane[0] | (lane[1] << 8) | (lane[2] << 16) |
                              ((uint32_t)lane[3] << 24));
}

static void test_the_enable_mask_has_two_write_aliases(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * One mask behind two addresses. The set alias reads back the live
     * mask, which is what a model with two independent words fails.
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
}

static void test_the_arbiter_reports_idle(const void *data)
{
    uint32_t cr;

    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * Bit 22 must report the arbiter idle: the kernel's serial console
     * setup polls it and a clear bit hangs the boot. Pinned here so a
     * masking change fails a test rather than a boot.
     */
    cr = qtest_readl(qts, CHIP_CONTROL);
    g_assert_cmphex(cr & CHIP_CTRL_ARB_IDLE, ==, CHIP_CTRL_ARB_IDLE);

    /*
     * A write with the idle bit clear still reads back set; the writable
     * field beside it keeps what was written.
     */
    qtest_writel(qts, CHIP_CONTROL, CHIP_CTRL_STROBE);
    cr = qtest_readl(qts, CHIP_CONTROL);
    g_assert_cmphex(cr & CHIP_CTRL_ARB_IDLE, ==, CHIP_CTRL_ARB_IDLE);
    g_assert_cmphex(cr & CHIP_CTRL_STROBE, ==, CHIP_CTRL_STROBE);
}

static void test_the_interrupt_output_stores_its_configuration(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /* Reset leaves the interrupt-output timer configuration clear. */
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT), ==, 0);

    /*
     * A supported interval and mode read back; the pin-state readback and
     * reserved field are not stored, so writes there read back clear.
     */
    qtest_writel(qts, INT_OUTPUT,
                 INT_OUT_PIN | 0x00100000u | INT_OUT_MODE_SQUARE | 0xfffe);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT), ==,
                    INT_OUT_MODE_SQUARE | 0xfffe);
}

/*
 * A command with no defined behaviour is still stored, so it reads back as
 * written, but the output carries on with the mode already running.
 */
static void assert_rejected_int_out_keeps_periodic(QTestState *qts,
                                                   uint32_t rejected)
{
    const uint32_t accepted = INT_OUT_MODE_PERIODIC | 0x0009;

    qtest_writel(qts, INT_OUTPUT, accepted);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT), ==,
                    accepted | INT_OUT_PIN);

    qtest_writel(qts, INT_OUTPUT, rejected);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT), ==,
                    rejected | INT_OUT_PIN);

    qtest_clock_step(qts, 3 * INT_OUT_TICK_NS);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT), ==, rejected);

    qtest_clock_step(qts, 7 * INT_OUT_TICK_NS);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT), ==,
                    rejected | INT_OUT_PIN);
}

/*
 * The Origin 300 IO-8 board test writes 0x4007ffff -- diagnostic mode,
 * square wave, count 0xffff -- and requires those bits back under the same
 * mask. None of that has a defined behaviour, and the model used to refuse
 * such a write outright, so it read back zero.
 */
static void test_int_out_stores_a_command_it_does_not_run(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, INT_OUTPUT, 0x4007ffffu);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & 0x4007ffffu, ==,
                    0x4007ffffu);

    /* It started nothing: the output stays low. */
    qtest_clock_step(qts, 0x10000 * INT_OUT_TICK_NS);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==, 0);
}

static void test_undefined_int_out_modes_keep_periodic_output(const void *data)
{
    static const uint32_t undefined_modes[] = {
        0x00010000u,
        0x00020000u,
        0x00030000u,
    };
    size_t i;

    ioc3_reset();

    sn0_ioc3_attach(qts);

    for (i = 0; i < ARRAY_SIZE(undefined_modes); i++) {
        assert_rejected_int_out_keeps_periodic(qts,
                                               undefined_modes[i] | 0x0009);
    }
}

static void test_diagnostic_int_out_keeps_periodic_output(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    assert_rejected_int_out_keeps_periodic(
        qts, INT_OUT_DIAG | INT_OUT_MODE_PERIODIC | 0x0009);
}

static void test_unsupported_int_out_counts_keep_periodic_output(
    const void *data)
{
    static const uint32_t unsupported_counts[] = {
        0x0000,
        0x0001,
        0x0002,
        0xffff,
    };
    size_t i;

    ioc3_reset();

    sn0_ioc3_attach(qts);

    for (i = 0; i < ARRAY_SIZE(unsupported_counts); i++) {
        assert_rejected_int_out_keeps_periodic(
            qts, INT_OUT_MODE_PERIODIC | unsupported_counts[i]);
    }
}

static void test_fixed_interrupt_output_commands_change_the_state(
    const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_HIGH);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==,
                    INT_OUT_PIN);

    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_LOW);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==, 0);
}

/*
 * A fixed-level command's new state is there for the very next read. The
 * Origin 300 IO-8 board test writes 0x80040001 -- output high -- and at
 * once reads the register 500 times, requiring bit 31 set on every read;
 * then the same with 0x00000001 for low.
 */
static void test_a_fixed_level_is_visible_to_the_next_read(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, INT_OUTPUT, 0x80040001u);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT), ==, 0x80040001u);

    qtest_writel(qts, INT_OUTPUT, 0x00000001u);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT), ==, 0x00000001u);
}

/*
 * A read sees the output as the clock says it is, even when the timer that
 * moves it has not been serviced yet. The Origin 300 IO-8 board test writes
 * 0x60005 -- periodic pulses, count 5 -- and at once reads the register 500
 * times, requiring both levels among the reads; under host load the main
 * loop can fall behind a loop like that. Stopping the VM reproduces the gap
 * deterministically: the virtual clock still moves under qtest_clock_step,
 * but no timer callback runs.
 */
static void test_a_read_catches_the_output_up_to_the_clock(const void *data)
{
    const int64_t tick = INT_OUT_TICK_NS;

    ioc3_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_LOW);
    qtest_writel(qts, INT_STATUS, INT_RT_OUTPUT_PULSE);

    /* Period of ten ticks, high for the first three, from 1 ns on. */
    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_PERIODIC | 0x0009);
    qtest_qmp_assert_success(qts, "{'execute': 'stop'}");

    /* Into the third pulse: rises at 1 + 20 ticks, falls at 1 + 23. */
    qtest_clock_step(qts, 1 + 21 * tick);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT), ==,
                    INT_OUT_MODE_PERIODIC | 0x0009 | INT_OUT_PIN);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_RT_OUTPUT_PULSE, ==,
                    INT_RT_OUTPUT_PULSE);

    /* Past its fall, still stopped: the clock sets the phase. */
    qtest_clock_step(qts, 2 * tick);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==, 0);

    /* Running again, the timer does not redo what the reads caught up. */
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_clock_step(qts, 6 * tick);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==, 0);
    qtest_clock_step(qts, tick);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==,
                    INT_OUT_PIN);
}

static void test_one_shot_interrupt_output_has_three_tick_width(
    const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_ONE_SHOT | 0x0002);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==,
                    INT_OUT_PIN);

    qtest_clock_step(qts, 3 * INT_OUT_TICK_NS - 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==,
                    INT_OUT_PIN);

    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==, 0);
}

/*
 * Any rising edge of the output posts the output-pulse source, not only a
 * one-shot's. The Origin 300 IO-8 board test masks every source, clears
 * the status, commands the output high (0x40005) and then low (0x5), and
 * requires bit 27 in the status afterwards.
 */
static void test_a_fixed_high_command_posts_the_output_pulse(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_LOW);
    qtest_clock_step(qts, 1);
    qtest_writel(qts, INT_STATUS, 0xffffffffu);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_RT_OUTPUT_PULSE, ==, 0);

    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_HIGH | 0x0005);
    qtest_clock_step(qts, 1);
    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_LOW | 0x0005);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_RT_OUTPUT_PULSE, ==,
                    INT_RT_OUTPUT_PULSE);

    /*
     * A fall alone posts nothing. The board test cannot tell a rise from a
     * fall; this follows the one-shot contract, where the rise posts.
     */
    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_HIGH);
    qtest_clock_step(qts, 1);
    qtest_writel(qts, INT_STATUS, INT_RT_OUTPUT_PULSE);
    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_LOW);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_RT_OUTPUT_PULSE, ==, 0);
}

static void test_count_zero_output_pulse_interrupt(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /* A prior command may leave the output high before the internal test. */
    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_HIGH);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==,
                    INT_OUT_PIN);
    /* That rising edge posted the source too; clear it to see the strobe's. */
    qtest_writel(qts, INT_STATUS, INT_RT_OUTPUT_PULSE);

    /* The exact internal-test strobe still emits a new rising edge. */
    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_ONE_SHOT);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_RT_OUTPUT_PULSE, ==,
                    INT_RT_OUTPUT_PULSE);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_SUPERIO,
                    ==, 0);

    /* Enabling the pending source raises the existing direct interrupt. */
    qtest_writel(qts, INT_ENABLE_SET, INT_RT_OUTPUT_PULSE);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_SUPERIO,
                    ==, BRIDGE_PIN_SUPERIO);

    /* The output-pulse status is latched and cleared by writing one. */
    qtest_writel(qts, INT_STATUS, INT_RT_OUTPUT_PULSE);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_RT_OUTPUT_PULSE, ==, 0);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_SUPERIO,
                    ==, 0);

    /* Let the first pulse fall, then reset with a second one pending. */
    qtest_clock_step(qts, 3 * INT_OUT_TICK_NS);
    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_ONE_SHOT);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_RT_OUTPUT_PULSE, ==,
                    INT_RT_OUTPUT_PULSE);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_SUPERIO,
                    ==, BRIDGE_PIN_SUPERIO);

    qtest_system_reset(qts);
    sn0_ioc3_attach(qts);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT), ==, 0);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_RT_OUTPUT_PULSE, ==, 0);
    g_assert_cmphex(qtest_readl(qts, INT_ENABLE_SET) & INT_RT_OUTPUT_PULSE, ==,
                    0);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_SUPERIO,
                    ==, 0);
}

static void test_periodic_interrupt_output_min_interval(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_PERIODIC | 0x0003);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==,
                    INT_OUT_PIN);

    qtest_clock_step(qts, 3 * INT_OUT_TICK_NS);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==, 0);

    qtest_clock_step(qts, INT_OUT_TICK_NS - 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==, 0);

    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==,
                    INT_OUT_PIN);

    qtest_clock_step(qts, 3 * INT_OUT_TICK_NS);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==, 0);
}

static void test_periodic_interrupt_output_max_interval(const void *data)
{
    const int64_t interval = 0xffffLL * INT_OUT_TICK_NS;

    ioc3_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_PERIODIC | 0xfffe);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==,
                    INT_OUT_PIN);

    qtest_clock_step(qts, 3 * INT_OUT_TICK_NS);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==, 0);

    qtest_clock_step(qts, interval - 3 * INT_OUT_TICK_NS - 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==, 0);

    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==,
                    INT_OUT_PIN);
}

static void test_square_interrupt_output_max_interval(const void *data)
{
    const int64_t interval = 0xffffLL * INT_OUT_TICK_NS;

    ioc3_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_SQUARE | 0xfffe);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==, 0);

    qtest_clock_step(qts, interval - 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==, 0);

    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==,
                    INT_OUT_PIN);

    qtest_clock_step(qts, interval - 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==,
                    INT_OUT_PIN);

    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==, 0);
}

static void test_fixed_command_cancels_periodic_output(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_PERIODIC | 0x0009);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==,
                    INT_OUT_PIN);

    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_HIGH);
    qtest_clock_step(qts, 1);
    qtest_clock_step(qts, 20 * INT_OUT_TICK_NS);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT) & INT_OUT_PIN, ==,
                    INT_OUT_PIN);
}

static void test_reset_cancels_an_interrupt_output_timer(const void *data)
{
    int64_t now;

    ioc3_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, INT_OUTPUT, INT_OUT_MODE_ONE_SHOT | 0x0002);
    qtest_clock_step(qts, 1);
    qtest_clock_step(qts, INT_OUT_TICK_NS);

    qtest_system_reset(qts);
    sn0_ioc3_attach(qts);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT), ==, 0);

    /*
     * Arm an independent timer after the old pulse deadline. The Hub timer
     * must be the next deadline: no new IOC3 command gets a chance to hide a
     * pre-reset timer which reset failed to delete.
     */
    now = qtest_clock_step(qts, 1);
    qtest_writeq(qts, PI_RT_COUNT, 0);
    qtest_writeq(qts, PI_RT_PENDING_A, 0);
    qtest_writeq(qts, PI_RT_COMPARE_A, PI_RT_SENTINEL_TICKS);
    g_assert_cmpint(qtest_clock_step_next(qts), ==,
                    now + PI_RT_SENTINEL_NS);
    g_assert_cmphex(qtest_readl(qts, INT_OUTPUT), ==, 0);
    g_assert_cmpuint(qtest_readq(qts, PI_RT_PENDING_A), ==, 1);
}

static void test_a_serial_bit_ignores_an_acknowledgement(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * The serial bits follow the 16550's level; an ack write must not clear
     * one. The port must be asserting first or clearing zero and ignoring
     * zero look the same.
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
}

static void test_the_second_interrupt_reaches_bridge_pin_3(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * The second interrupt travels the board's direct wire, not a PCI
     * INTx; without that wire modelled the pin never sets.
     */
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_SUPERIO,
                    ==, 0);

    /* The enable alone raises nothing while int_status is clear. */
    qtest_writel(qts, INT_ENABLE_SET, INT_SERIAL_A);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_SUPERIO,
                    ==, 0);

    /*
     * The port asserting drives the whole chain: source, combined status,
     * second interrupt pin, board wire, Bridge pin 3.
     */
    qtest_writeb(qts, SERIAL_A + 1, 0x02);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_SUPERIO,
                    ==, BRIDGE_PIN_SUPERIO);

    /*
     * Live, not latched: the source dropping clears the pin with nothing
     * written to either status register.
     */
    qtest_writeb(qts, SERIAL_A + 1, 0x00);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_SUPERIO,
                    ==, 0);
}

static void test_a_one_wire_slot_always_finishes(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * A guest writes slot timing and polls for done; a slot that never
     * finishes is a guest polling forever. Any write must bring the done
     * bit back set, whatever timing was asked.
     */
    g_assert_cmphex(qtest_readl(qts, MICROLAN_CONTROL) & MICROLAN_DONE, ==,
                    0);

    qtest_writel(qts, MICROLAN_CONTROL, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, MICROLAN_CONTROL) & MICROLAN_DONE, ==,
                    MICROLAN_DONE);
}

static void test_a_one_wire_reset_finds_the_ethernet_address_part(
    const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);
    /* Route the MicroLAN engine onto its pin, as a driver does before use. */
    qtest_writel(qts, PIN_CONTROL_SET, PIN_MICROLAN_ENABLE);

    /*
     * The part answers a reset by pulling the wire low; high is what "no
     * part" reads as.
     */
    qtest_writel(qts, MICROLAN_CONTROL, MICROLAN_RESET_SLOT);
    g_assert_cmphex(qtest_readl(qts, MICROLAN_CONTROL) & MICROLAN_LEVEL, ==,
                    0);
}

static void test_the_one_wire_bus_is_dead_until_enabled(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * Pin-control bit 21 routes the MicroLAN engine onto pin 8; until a
     * guest sets it the one-wire bus is disconnected. With the bit clear a
     * reset slot reaches no part: the wire floats high (level set), rather
     * than being pulled low by the station-address part answering.
     */
    qtest_writel(qts, MICROLAN_CONTROL, MICROLAN_RESET_SLOT);
    g_assert_cmphex(qtest_readl(qts, MICROLAN_CONTROL) & MICROLAN_LEVEL, ==,
                    MICROLAN_LEVEL);
}

static void test_one_wire_slot_preserves_the_timing_written(const void *data)
{
    /*
     * A 5 us pulse lands below both slot-kind thresholds, an ordinary
     * short slot. Bits 0 and 1 are set in the write to prove hardware
     * overwrites them; the rest is arbitrary to prove the field stores.
     */
    uint32_t written = (5u << 10) | (0x9eu << 2) | 0x3u;
    uint32_t timing = written & ~(MICROLAN_LEVEL | MICROLAN_DONE);

    ioc3_reset();

    sn0_ioc3_attach(qts);
    /* Route the MicroLAN engine onto its pin, as a driver does before use. */
    qtest_writel(qts, PIN_CONTROL_SET, PIN_MICROLAN_ENABLE);

    /*
     * With no reset yet the part is idle and does not drive the wire, so
     * the level reads high; this pins the timing field's storage, not the
     * part's answers.
     */
    qtest_writel(qts, MICROLAN_CONTROL, written);
    g_assert_cmphex(qtest_readl(qts, MICROLAN_CONTROL), ==,
                    timing | MICROLAN_LEVEL | MICROLAN_DONE);
}

static void test_pin_control_has_two_write_aliases(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * Set/clear aliases over one live mask, the interrupt-enable shape;
     * bit 21 (MicroLAN onto pin 8) is the one both guests use.
     */
    g_assert_cmphex(qtest_readl(qts, PIN_CONTROL_SET), ==, 0);

    qtest_writel(qts, PIN_CONTROL_SET, PIN_MICROLAN_ENABLE);
    g_assert_cmphex(qtest_readl(qts, PIN_CONTROL_SET), ==,
                    PIN_MICROLAN_ENABLE);

    qtest_writel(qts, PIN_CONTROL_SET, 1u << 5);
    g_assert_cmphex(qtest_readl(qts, PIN_CONTROL_SET), ==,
                    PIN_MICROLAN_ENABLE | (1u << 5));

    qtest_writel(qts, PIN_CONTROL_CLR, PIN_MICROLAN_ENABLE);
    g_assert_cmphex(qtest_readl(qts, PIN_CONTROL_SET), ==, 1u << 5);
}

static void test_a_pin_register_reaches_the_shared_data_word(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * Pin register 3 is an alias onto bit 3 of the shared data word: two
     * addresses reaching one place, not two places agreeing.
     */
    qtest_writel(qts, PIN_REG(3), 1);
    g_assert_cmphex(qtest_readl(qts, PIN_DATA), ==, 1u << 3);
    g_assert_cmphex(qtest_readl(qts, PIN_REG(3)), ==, 1);

    /* And the reverse: writing the data register moves the pin register. */
    qtest_writel(qts, PIN_DATA, 0);
    g_assert_cmphex(qtest_readl(qts, PIN_REG(3)), ==, 0);

    /* A neighbouring pin is untouched by either. */
    g_assert_cmphex(qtest_readl(qts, PIN_REG(4)), ==, 0);
}

/* The PS/2 controller's five registers. */
#define KBC_CONTROL       (IOC3_BASE + 0x09c)
#define KBC_KBD_RECEIVE   (IOC3_BASE + 0x0a0)
#define KBC_AUX_RECEIVE   (IOC3_BASE + 0x0a4)
#define KBC_KBD_TRANSMIT  (IOC3_BASE + 0x0a8)
#define KBC_AUX_TRANSMIT  (IOC3_BASE + 0x0ac)

/*
 * Control/status bits, at the positions mainline Linux's own header assigns
 * them (arch/mips/include/asm/sn/ioc3.h). Clamping a port after three
 * received characters is ioc3kbd's only initialisation write to this
 * register: the clamp holds the port's clock line low so a device with more
 * to say stops instead of overrunning the controller's three-character
 * holding.
 */
#define KBC_K_CLAMP_1     0x00100000
#define KBC_M_CLAMP_1     0x00200000
#define KBC_K_CLAMP_3     0x00400000
#define KBC_M_CLAMP_3     0x00800000

/* Receive-register flags, by the datasheet's names for them. */
#define KBC_RX_OVERFLOW   (1u << 28)
#define KBC_RX_VALID(p)   (1u << (29 + (p)))
#define KBC_RX_VALID_ALL  (KBC_RX_VALID(0) | KBC_RX_VALID(1) | KBC_RX_VALID(2))
#define KBC_RX_FLAGS      (KBC_RX_OVERFLOW | KBC_RX_VALID_ALL)

/* One key down puts one raw scan byte on the keyboard's wire. */
static void ps2_key_down(QTestState *qts, const char *key)
{
    QDict *resp = qtest_qmp(qts,
        "{ 'execute': 'input-send-event', 'arguments': "
        "{ 'events': [ { 'type': 'key', 'data': "
        "{ 'down': true, 'key': { 'type': 'qcode', 'data': %s } } } "
        "] } }", key);

    if (!qdict_haskey(resp, "return")) {
        g_autoptr(GString) text = qobject_to_json(QOBJECT(resp));

        g_error("input-send-event %s failed: %s", key, text->str);
    }
    qobject_unref(resp);
}

/* Five characters: two more than the controller can hold at once. */
static void ps2_type_five_keys(QTestState *qts)
{
    static const char *keys[] = { "a", "b", "c", "d", "e" };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(keys); i++) {
        ps2_key_down(qts, keys[i]);
    }
}

static void test_the_ps2_ports_read_empty(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * Zero means every per-byte valid flag clear -- a port that received
     * nothing, not three zero bytes. The write first makes the read catch
     * a model that stores into the read-only register too.
     */
    qtest_writel(qts, KBC_KBD_RECEIVE, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE), ==, 0);
    qtest_writel(qts, KBC_AUX_RECEIVE, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, KBC_AUX_RECEIVE), ==, 0);

    /*
     * The hardware-owned bits report busy states this model never enters,
     * so a driver's post-transmit poll finishes on its first look.
     */
    qtest_writel(qts, KBC_CONTROL, 0x00000005);
    g_assert_cmphex(qtest_readl(qts, KBC_CONTROL), ==, 0x00000005);

    /* A transmit register takes a byte and carries it nowhere, which must
     * still complete -- a refused write would report a fault where the real
     * chip reports an unplugged port. */
    qtest_writel(qts, KBC_KBD_TRANSMIT, 0xed);
    g_assert_cmphex(qtest_readl(qts, KBC_CONTROL), ==, 0x00000005);
}

/*
 * The keyboard reset command exercises transmit, the ps2 reply queue and the
 * receive register's packing in one round trip. The generic ps2 keyboard
 * answers 0xFF with ACK (0xFA) then self-test-pass (0xAA), oldest first.
 */
static void test_the_ps2_keyboard_round_trips(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, KBC_KBD_TRANSMIT, 0xff);

    /*
     * Oldest byte occupies the highest slot: 0xFA in bits 16-23 with its
     * valid flag in bit 31, 0xAA in bits 8-15 with its valid flag in bit 30.
     * No third byte, so bit 29 and bits 0-7 stay clear; the source bit (27)
     * is clear for the keyboard and nothing overflowed (bit 28 clear).
     */
    g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE), ==, 0xc0faaa00);
}

/*
 * The mouse port's own transmit and receive registers exercise the same
 * round trip as the keyboard's, through the generic ps2 mouse device wired
 * behind AUX_TRANSMIT/AUX_RECEIVE. A reset packs one more byte than the
 * keyboard's: the generic ps2 mouse answers 0xFF with ACK (0xFA),
 * self-test-pass (0xAA) and a third mouse-ID byte (0x00), all three valid.
 * The source bit (27) is set: this is the one bit the register contract
 * uses to tell the two ports apart once their bytes reach a shared
 * interrupt.
 */
static void test_the_ps2_mouse_round_trips(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, KBC_AUX_TRANSMIT, 0xff);

    /*
     * Oldest byte in the highest slot: 0xFA in bits 16-23, 0xAA in bits
     * 8-15, the mouse-ID byte 0x00 in bits 0-7, all three valid flags
     * (bits 29-31) set and the source bit (27) set for aux.
     */
    g_assert_cmphex(qtest_readl(qts, KBC_AUX_RECEIVE), ==, 0xe8faaa00);

    /* The keyboard's own receive register is untouched by a mouse byte. */
    g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE), ==, 0);
}

/*
 * IRIX sends the scan-code-set-3 "set all keys" command on every open of the
 * keyboard device. A real keyboard acknowledges the whole family: 0xF7 to
 * 0xFA take no argument, and 0xFB to 0xFD take one key code and acknowledge
 * that too. Answering RESEND (0xFE) instead would tell a driver its command
 * never arrived.
 */
static void test_the_ps2_keyboard_acks_set_key_type(const void *data)
{
    /* One ACK packed alone: bits 16-23 with its valid flag in bit 31. */
    const uint32_t one_ack = 0x80fa0000;
    const uint8_t all_keys[] = { 0xf7, 0xf8, 0xf9, 0xfa };
    const uint8_t one_key[] = { 0xfb, 0xfc, 0xfd };
    unsigned int i;

    ioc3_reset();

    sn0_ioc3_attach(qts);

    for (i = 0; i < ARRAY_SIZE(all_keys); i++) {
        qtest_writel(qts, KBC_KBD_TRANSMIT, all_keys[i]);
        g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE), ==, one_ack);
    }

    for (i = 0; i < ARRAY_SIZE(one_key); i++) {
        qtest_writel(qts, KBC_KBD_TRANSMIT, one_key[i]);
        g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE), ==, one_ack);

        /* The key code that follows is consumed, not read back as a command. */
        qtest_writel(qts, KBC_KBD_TRANSMIT, 0x1c);
        g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE), ==, one_ack);
    }
}

/*
 * The receive register packs "bytes since the last read", so reading it
 * pops the queue and a second read finds nothing left.
 */
static void test_the_ps2_receive_drains_on_read(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, KBC_KBD_TRANSMIT, 0xff);
    g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE), ==, 0xc0faaa00);

    /* Drained: the reads popped both bytes, so nothing remains. */
    g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE), ==, 0);
}

/*
 * The shared keyboard/mouse source (interrupt-status bit 22) is latched: a
 * queued byte sets it, it survives a drain of the receive register, and only
 * a write-one-to-clear of the status register acknowledges it -- the contract
 * both Linux's edge handler and IRIX's acknowledge writes rely on.
 */
static void test_the_ps2_interrupt_latches_until_acknowledged(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /* Nothing queued: the source is clear. */
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_KEYBOARD, ==, 0);

    /* A queued reply latches it set. */
    qtest_writel(qts, KBC_KBD_TRANSMIT, 0xff);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_KEYBOARD, ==,
                    INT_KEYBOARD);

    /*
     * Draining the receive register delivers the bytes but leaves the latch
     * set: the guest has read its data, yet the interrupt stays pending until
     * it acknowledges.
     */
    (void)qtest_readl(qts, KBC_KBD_RECEIVE);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_KEYBOARD, ==,
                    INT_KEYBOARD);

    /* Writing a one to the status register clears the latch. */
    qtest_writel(qts, INT_STATUS, INT_KEYBOARD);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_KEYBOARD, ==, 0);
}

/*
 * The controller holds three received characters and no more. With the
 * clamp-after-three bits armed, a keyboard with more to say is stopped with
 * its clock line held low rather than losing anything, so five keystrokes
 * arrive as three now and the remaining two once the guest reads.
 */
static void test_the_ps2_receive_holds_three_behind_the_clamp(const void *data)
{
    uint32_t first, second;

    ioc3_reset();

    sn0_ioc3_attach(qts);
    qtest_writel(qts, KBC_CONTROL, KBC_K_CLAMP_3 | KBC_M_CLAMP_3);

    ps2_type_five_keys(qts);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_KEYBOARD, ==,
                    INT_KEYBOARD);

    /* Acknowledge first: what follows is then a fresh assertion. */
    qtest_writel(qts, INT_STATUS, INT_KEYBOARD);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_KEYBOARD, ==, 0);

    /* Three characters, all valid, nothing lost: the clamp stopped the rest. */
    first = qtest_readl(qts, KBC_KBD_RECEIVE);
    g_assert_cmphex(first & KBC_RX_FLAGS, ==, KBC_RX_VALID_ALL);

    /*
     * That read emptied the holding and released the clamp, so the stopped
     * keyboard resumed and its next characters moved in. Their arrival is a
     * fresh assertion of the data-available line, so the source is latched
     * again with no other event to help it.
     */
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_KEYBOARD, ==,
                    INT_KEYBOARD);

    /* The two that were held back, and they are not a replay of the first. */
    second = qtest_readl(qts, KBC_KBD_RECEIVE);
    g_assert_cmphex(second & KBC_RX_FLAGS, ==,
                    KBC_RX_VALID(2) | KBC_RX_VALID(1));
    g_assert_cmphex((second >> 16) & 0xff, !=, (first >> 16) & 0xff);

    /* All five delivered: the holding is empty and the line has fallen. */
    qtest_writel(qts, INT_STATUS, INT_KEYBOARD);
    g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_KEYBOARD, ==, 0);
}

/*
 * Mainline Linux acknowledges before its handler reads: the kernel's edge
 * handler calls the chip's acknowledge hook, which writes bit 22 to the
 * status register, and only then runs ioc3kbd's receive-register read. A
 * backlog has to survive that order, and a guest that acknowledges without
 * reading anything must not be handed the same interrupt over and over.
 */
static void test_the_ps2_backlog_survives_an_early_acknowledge(const void *data)
{
    unsigned int i;

    ioc3_reset();

    sn0_ioc3_attach(qts);
    qtest_writel(qts, KBC_CONTROL, KBC_K_CLAMP_3 | KBC_M_CLAMP_3);

    ps2_type_five_keys(qts);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_KEYBOARD, ==,
                    INT_KEYBOARD);

    /*
     * Four acknowledges with nothing read, and the source stays clear after
     * every one. The line is still asserted -- three characters are still
     * held -- but only a character arriving asserts it afresh, and the clamp
     * has stopped the keyboard from sending one. A guest that acknowledges
     * and never reads therefore goes quiet instead of taking the same
     * interrupt forever.
     */
    for (i = 0; i < 4; i++) {
        qtest_writel(qts, INT_STATUS, INT_KEYBOARD);
        g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_KEYBOARD, ==, 0);
    }

    /* The read that follows the acknowledge still finds all three. */
    g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE) & KBC_RX_FLAGS, ==,
                    KBC_RX_VALID_ALL);

    /* And the backlog behind the clamp raises the source a second time. */
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_KEYBOARD, ==,
                    INT_KEYBOARD);
    qtest_writel(qts, INT_STATUS, INT_KEYBOARD);
    g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE) & KBC_RX_FLAGS, ==,
                    KBC_RX_VALID(2) | KBC_RX_VALID(1));

    /* Nothing is left, so nothing asserts the line again. */
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_KEYBOARD, ==, 0);
}

/*
 * OpenBSD takes the other order: iockbcintr_internal reads the receive
 * register until it comes back with no valid character, and ioc_intr_superio
 * acknowledges the source afterwards. A whole backlog drains inside one
 * handler pass, so one acknowledge ends it and the guest is not called again.
 */
static void test_the_ps2_drain_then_acknowledge_takes_one(const void *data)
{
    unsigned int reads = 0;
    unsigned int valid = 0;
    uint32_t val;

    ioc3_reset();

    sn0_ioc3_attach(qts);
    qtest_writel(qts, KBC_CONTROL, KBC_K_CLAMP_3 | KBC_M_CLAMP_3);

    ps2_type_five_keys(qts);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_KEYBOARD, ==,
                    INT_KEYBOARD);

    /* Read until the register reports nothing, counting what came back. */
    do {
        unsigned int slot;

        val = qtest_readl(qts, KBC_KBD_RECEIVE);
        for (slot = 0; slot < 3; slot++) {
            valid += (val & KBC_RX_VALID(slot)) ? 1 : 0;
        }
        reads++;
        g_assert_cmpuint(reads, <=, 4);

        /* The source stays pending: the guest has not answered it yet. */
        g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_KEYBOARD, ==,
                        INT_KEYBOARD);
    } while (val != 0);

    /* Every character the keyboard sent, in three reads: 3, 2, then empty. */
    g_assert_cmpuint(valid, ==, 5);
    g_assert_cmpuint(reads, ==, 3);

    /* One acknowledge ends it, and nothing raises it again. */
    qtest_writel(qts, INT_STATUS, INT_KEYBOARD);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_KEYBOARD, ==, 0);
    g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_KEYBOARD, ==, 0);
}

/*
 * With no clamp armed -- the reset state, before a driver enables the ports
 * -- the controller has nothing to stop the device with, so a fourth
 * character arrives while the holding is full and is lost. Bit 28 reports
 * that, and a read clears it along with the characters it reports.
 */
static void test_the_ps2_receive_overflows_without_a_clamp(const void *data)
{
    static const char *keys[] = { "a", "b", "c", "d" };
    unsigned int i;

    ioc3_reset();

    sn0_ioc3_attach(qts);
    g_assert_cmphex(qtest_readl(qts, KBC_CONTROL), ==, 0);

    for (i = 0; i < ARRAY_SIZE(keys); i++) {
        ps2_key_down(qts, keys[i]);
    }

    /* Three characters held and one lost, reported together. */
    g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE) & KBC_RX_FLAGS, ==,
                    KBC_RX_VALID_ALL | KBC_RX_OVERFLOW);

    /* The fourth is gone, not queued: the next read finds an empty port. */
    g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE), ==, 0);
}

/*
 * Clamping after one character splits a reply the guest would otherwise read
 * whole, which is what pins the order characters come back in: the reset
 * reply's ACK first, then its self-test byte, each alone in the oldest slot.
 */
static void test_the_ps2_clamp_after_one_splits_a_reply(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);
    qtest_writel(qts, KBC_CONTROL, KBC_K_CLAMP_1);

    qtest_writel(qts, KBC_KBD_TRANSMIT, 0xff);

    /* ACK alone in slot 2, bits 16-23, with its valid flag in bit 31. */
    g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE), ==, 0x80fa0000);

    /* The clamp released with the read, so the self-test byte follows. */
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & INT_KEYBOARD, ==,
                    INT_KEYBOARD);
    g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE), ==, 0x80aa0000);
    g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE), ==, 0);
}

/*
 * A migration in the middle of a backlog has to arrive with the interrupt
 * still owed. Everything that decides the next interrupt crosses in the
 * stream -- the characters the controller is holding, the clamp that stopped
 * the keyboard, and the latched source bit -- so the destination carries on
 * where the source left off rather than waiting for a keystroke that has
 * already happened.
 */
static void test_the_ps2_backlog_survives_a_migration(const void *data)
{
    g_autofree char *uri = NULL;
    g_autofree char *dir = NULL;
    QTestState *from, *to;

    dir = g_dir_make_tmp("sn0-ioc3-ps2-XXXXXX", NULL);
    g_assert_nonnull(dir);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);

    from = qtest_init("-M origin200");
    sn0_ioc3_attach(from);
    qtest_writel(from, KBC_CONTROL, KBC_K_CLAMP_3 | KBC_M_CLAMP_3);
    ps2_type_five_keys(from);
    g_assert_cmphex(qtest_readl(from, INT_STATUS) & INT_KEYBOARD, ==,
                    INT_KEYBOARD);

    to = qtest_initf("-M origin200 -incoming %s", uri);
    sgi_migrate_stopped(from, uri);
    qtest_quit(from);

    sn0_ioc3_attach(to);

    /* The clamp and the pending source both came across. */
    g_assert_cmphex(qtest_readl(to, KBC_CONTROL), ==,
                    KBC_K_CLAMP_3 | KBC_M_CLAMP_3);
    g_assert_cmphex(qtest_readl(to, INT_STATUS) & INT_KEYBOARD, ==,
                    INT_KEYBOARD);

    /* Acknowledge and read: three held characters, then the two behind them. */
    qtest_writel(to, INT_STATUS, INT_KEYBOARD);
    g_assert_cmphex(qtest_readl(to, KBC_KBD_RECEIVE) & KBC_RX_FLAGS, ==,
                    KBC_RX_VALID_ALL);
    g_assert_cmphex(qtest_readl(to, INT_STATUS) & INT_KEYBOARD, ==,
                    INT_KEYBOARD);
    qtest_writel(to, INT_STATUS, INT_KEYBOARD);
    g_assert_cmphex(qtest_readl(to, KBC_KBD_RECEIVE) & KBC_RX_FLAGS, ==,
                    KBC_RX_VALID(2) | KBC_RX_VALID(1));
    g_assert_cmphex(qtest_readl(to, INT_STATUS) & INT_KEYBOARD, ==, 0);

    qtest_quit(to);
    g_rmdir(dir);
}

/*
 * The control/status register resets to zero and stores the software control
 * bits behind a mask: the low half, and the four clamp bits above the
 * hardware-status region. The rest of the upper half (write-pending, the
 * timeout reports and the live line states) is hardware-owned, read-only,
 * and reads clear in this synchronous model.
 */
static void test_the_ps2_control_masks_writes(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    g_assert_cmphex(qtest_readl(qts, KBC_CONTROL), ==, 0);

    qtest_writel(qts, KBC_CONTROL, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, KBC_CONTROL), ==,
                    0x0000ffff | KBC_K_CLAMP_1 | KBC_M_CLAMP_1 |
                    KBC_K_CLAMP_3 | KBC_M_CLAMP_3);
}

/* The serial DMA block: the shared ring base, then each port's registers. */
#define SDMA_RING_LO      (IOC3_BASE + 0x0b4)
#define SDMA_A_TX_PRODUCE (IOC3_BASE + 0x0bc)
#define SDMA_A_TX_CONSUME (IOC3_BASE + 0x0c0)
#define SDMA_A_RX_PRODUCE (IOC3_BASE + 0x0c4)
#define SDMA_A_RX_CONSUME (IOC3_BASE + 0x0c8)
#define SDMA_B_TX_PRODUCE (IOC3_BASE + 0x0d8)
#define SDMA_B_TX_CONSUME (IOC3_BASE + 0x0dc)
#define SDMA_A_SHADOW     (IOC3_BASE + 0x0d0)
#define SDMA_B_SHADOW     (IOC3_BASE + 0x0ec)

static void test_the_serial_dma_indices_take_a_write(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * All four adjacent indices store a write, including the two the
     * engine advances: this chip lets software place them, which is what
     * the Origin 300 board test requires of it. An earlier model refused
     * the engine's two, so both of those are asserted with a value that
     * cannot be mistaken for the zero a refusal leaves.
     */
    qtest_writel(qts, SDMA_A_TX_PRODUCE, 0x40);
    g_assert_cmphex(qtest_readl(qts, SDMA_A_TX_PRODUCE), ==, 0x40);
    qtest_writel(qts, SDMA_A_RX_CONSUME, 0x48);
    g_assert_cmphex(qtest_readl(qts, SDMA_A_RX_CONSUME), ==, 0x48);
    qtest_writel(qts, SDMA_A_TX_CONSUME, 0x50);
    g_assert_cmphex(qtest_readl(qts, SDMA_A_TX_CONSUME), ==, 0x50);
    qtest_writel(qts, SDMA_A_RX_PRODUCE, 0x58);
    g_assert_cmphex(qtest_readl(qts, SDMA_A_RX_PRODUCE), ==, 0x58);

    /*
     * What still separates them is what else the write carries. Only the
     * receive consume index keeps bit 31, the receive timer's arm; an
     * all-ones write to the other three keeps the pointer field alone, so
     * a model that stored the whole word anywhere fails here.
     */
    qtest_writel(qts, SDMA_A_RX_CONSUME, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SDMA_A_RX_CONSUME), ==, 0x800003f8);
    qtest_writel(qts, SDMA_A_TX_PRODUCE, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SDMA_A_TX_PRODUCE), ==, 0x3f8);
    qtest_writel(qts, SDMA_A_TX_CONSUME, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SDMA_A_TX_CONSUME), ==, 0x3f8);
    qtest_writel(qts, SDMA_A_RX_PRODUCE, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SDMA_A_RX_PRODUCE), ==, 0x3f8);
}

static void test_the_serial_dma_ports_are_distinct(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * Different values per port prove the second block is not an alias:
     * a stride off-by-one or a single-port model answers both from one.
     */
    qtest_writel(qts, SDMA_A_TX_PRODUCE, 0x18);
    qtest_writel(qts, SDMA_B_TX_PRODUCE, 0x28);
    g_assert_cmphex(qtest_readl(qts, SDMA_A_TX_PRODUCE), ==, 0x18);
    g_assert_cmphex(qtest_readl(qts, SDMA_B_TX_PRODUCE), ==, 0x28);

    /* The engine's own index is per port too, and placeable on each. */
    qtest_writel(qts, SDMA_A_TX_CONSUME, 0x20);
    qtest_writel(qts, SDMA_B_TX_CONSUME, 0x30);
    g_assert_cmphex(qtest_readl(qts, SDMA_A_TX_CONSUME), ==, 0x20);
    g_assert_cmphex(qtest_readl(qts, SDMA_B_TX_CONSUME), ==, 0x30);

    /* The ring base is shared by both ports and is software's to set. */
    qtest_writel(qts, SDMA_RING_LO, 0x00003000);
    g_assert_cmphex(qtest_readl(qts, SDMA_RING_LO), ==, 0x00003000);
}

static void test_the_serial_dma_shadow_is_read_only(const void *data)
{
    uint32_t shadow_a, shadow_b;

    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * The shadow mirrors live lines; OpenBSD writes zero to it on every
     * boot and the chip discards that. The port comes up with carrier
     * asserted, so the pin is write-leaves-it-unchanged, not reads-zero.
     */
    shadow_a = qtest_readl(qts, SDMA_A_SHADOW);
    qtest_writel(qts, SDMA_A_SHADOW, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SDMA_A_SHADOW), ==, shadow_a);
    shadow_b = qtest_readl(qts, SDMA_B_SHADOW);
    qtest_writel(qts, SDMA_B_SHADOW, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SDMA_B_SHADOW), ==, shadow_b);
}

/* The ethernet block's registers, at their offsets in the chip's window. */
#define ENET_INT_STATUS   (IOC3_BASE + 0x0f4)
#define ENET_INT_ENABLE   (IOC3_BASE + 0x0f8)
#define ENET_COUNTERS     (IOC3_BASE + 0x120)
#define ENET_BUFFER_INDEX (IOC3_BASE + 0x124)
#define ENET_MII_CONTROL  (IOC3_BASE + 0x148)
#define ENET_MII_DATA_R   (IOC3_BASE + 0x14c)

/*
 * The management interface, and the transceiver on the far end of it. A guest
 * reaches the transceiver only through these two registers: it writes an
 * address and a register number here and reads the answer back.
 */
#define MII_CONTROL     (IOC3_BASE + 0x148)
#define MII_DATA_R      (IOC3_BASE + 0x14c)
#define MII_DATA_W      (IOC3_BASE + 0x150)
#define MII_READ        (1u << 10)

#define PHY_ADDRESS     31

static uint16_t phy_read(QTestState *qts, unsigned int phy, unsigned int reg)
{
    qtest_writel(qts, MII_CONTROL, MII_READ | (phy << 5) | reg);
    return qtest_readl(qts, MII_DATA_R) & 0xffff;
}

static void phy_write(QTestState *qts, unsigned int phy, unsigned int reg,
                      uint16_t val)
{
    qtest_writel(qts, MII_DATA_W, val);
    qtest_writel(qts, MII_CONTROL, (phy << 5) | reg);
}

static void test_the_ethernet_interrupt_register_acknowledges(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * A write acknowledges the sources it names. From zero the direction
     * is observable: storage leaves the value behind, acknowledgement
     * leaves zero, and only one can be true of the same write.
     */
    qtest_writel(qts, ENET_INT_STATUS, 0x00010001);
    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS), ==, 0);

    /*
     * The enable beside it is storage: a model treating the pair alike
     * fails exactly one of the two assertions either way.
     */
    qtest_writel(qts, ENET_INT_ENABLE, 0x00010001);
    g_assert_cmphex(qtest_readl(qts, ENET_INT_ENABLE), ==, 0x00010001);
}

static void test_the_ethernet_read_only_registers(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * Read-to-clear counters; nothing sent yet, so this pins only that the
     * read answers -- a driver's reset path stops on a fault here.
     */
    g_assert_cmphex(qtest_readl(qts, ENET_COUNTERS), ==, 0);

    /* Software cannot place the block's engines, so a write does not take. */
    qtest_writel(qts, ENET_BUFFER_INDEX, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, ENET_BUFFER_INDEX), ==, 0);
}

/* The MAC control register and the two DMA rings it gates. */
#define MAC_CONTROL       (IOC3_BASE + 0x0f0)
#define RX_RING_HI        (IOC3_BASE + 0x100)
#define RX_RING_LO        (IOC3_BASE + 0x104)
#define RX_CONSUME        (IOC3_BASE + 0x10c)
#define RX_PRODUCE        (IOC3_BASE + 0x110)
#define TX_RING_HI        (IOC3_BASE + 0x128)
#define TX_RING_LO        (IOC3_BASE + 0x12c)
#define TX_CONSUME        (IOC3_BASE + 0x130)
#define TX_PRODUCE        (IOC3_BASE + 0x134)
#define MAC_ADDR_HI       (IOC3_BASE + 0x138)
#define MAC_ADDR_LO       (IOC3_BASE + 0x13c)
#define HASH_HI           (IOC3_BASE + 0x140)
#define HASH_LO           (IOC3_BASE + 0x144)

#define MCR_PROMISC       (1u << 1)
#define MCR_PAD_EN        (1u << 2)
#define MCR_TX_DMA_EN     (1u << 13)
#define MCR_TX_EN         (1u << 14)
#define MCR_RX_DMA_EN     (1u << 15)
#define MCR_RX_EN         (1u << 16)
#define MCR_LOOPBACK      (1u << 17)
#define MCR_ARB_DIAG_IDLE (1u << 21)
#define MCR_RESET         (1u << 31)

/* Frame offset in halfwords; 33 (66 bytes) is what OpenBSD programs. */
#define MCR_RXOFF_SHIFT   3
#define MCR_RXOFF(halfwords) ((uint32_t)(halfwords) << MCR_RXOFF_SHIFT)

#define ENET_INT_TX_EMPTY    (1u << 16)
#define ENET_INT_TX_COMPLETE (1u << 22)
#define ENET_INT_RX_TIMER     (1u << 0)
#define ENET_INT_RX_THRESHOLD (1u << 1)

/*
 * Linux gates every delivered frame on the good-packet bit; OpenBSD never
 * reads it -- a field only one of the two guests can miss.
 */
#define ENET_RXE_GOODPKT      (1u << 30)

/*
 * The watermark field and the arm bit: the two things that decide which
 * source a delivered frame raises.
 */
#define RX_CONTROL        (IOC3_BASE + 0x0fc)
#define RX_TIMER          (IOC3_BASE + 0x114)
#define RPIR_ARM          (1u << 31)

#define TX_CONSUME_IDLE   (1u << 31)

/* Transmit descriptor command-word bits and buffer-count-word fields. */
#define TXD_INT_DONE      (1u << 12)
#define TXD_DATA_VALID    (1u << 16)
#define TXD_BUF1_VALID    (1u << 17)
#define TXD_BUF2_VALID    (1u << 18)
#define TXD_DOCHECKSUM    (1u << 19)
#define TXD_CHKOFF_SHIFT  20
#define TXD_BUF1_LEN_SHIFT 8
#define TXD_BUF2_LEN_SHIFT 20

/*
 * Offset and size from the drivers, not the model: restating the model's
 * wrong 16 once passed every transmit assertion.
 */
#define TXD_DATA_OFFSET   24
#define TXD_DATA_SIZE     104

/* The inline length field is seven bits, so a guest can name more than the
 * area holds. */
#define TXD_DATA_LEN_MAX  127

/* A region of guest RAM well clear of the firmware's own configuration
 * graph (at 0x4000), used as scratch space for descriptors and buffers.
 */
#define RAM_SCRATCH       0x00100000

/*
 * IRIX's two-step reset handshake: write reset, poll the diagnostic
 * idle, clear reset. The seeded witness is a produce index -- the ring
 * bases survive this reset, so seeding one would show nothing.
 */
static void test_the_ethernet_mac_reset_reports_idle(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);
    qtest_writel(qts, TX_PRODUCE, 128);
    qtest_writel(qts, MAC_CONTROL, MCR_RESET);

    g_assert_cmphex(qtest_readl(qts, MAC_CONTROL), ==,
                    MCR_RESET | MCR_ARB_DIAG_IDLE);
    g_assert_cmphex(qtest_readl(qts, TX_PRODUCE), ==, 0);

    qtest_writel(qts, MAC_CONTROL, 0);
    g_assert_cmphex(qtest_readl(qts, MAC_CONTROL), ==, 0);
}

/*
 * -nic is what qemu_configure_nic_device finds for an on-board controller;
 * also sets bus-master, which only DMA needs.
 */
static QTestState *sn0_ioc3_attach_with_netdev(int host_fd)
{
    QTestState *qts;
    char *args = g_strdup_printf("-M origin200 "
                                 "-nic socket,fd=%d,id=n0", host_fd);

    qts = qtest_init(args);
    g_free(args);
    sn0_ioc3_attach(qts);
    qtest_writel(qts, CFG0(PCI_IOC3_DEVICE) + PCI_COMMAND,
                PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    return qts;
}

/*
 * The one-wire tests need a known MAC to check the part against, which
 * the explicit mac= supplies; the socket backend is otherwise unused.
 */
static QTestState *sn0_ioc3_attach_with_mac(int host_fd, const char *mac)
{
    QTestState *qts;
    char *args = g_strdup_printf("-M origin200 "
                                 "-nic socket,fd=%d,id=n0,mac=%s",
                                 host_fd, mac);

    qts = qtest_init(args);
    g_free(args);
    sn0_ioc3_attach(qts);
    /* Route the MicroLAN engine onto its pin, as a driver does before use. */
    qtest_writel(qts, PIN_CONTROL_SET, PIN_MICROLAN_ENABLE);
    return qts;
}

static const uint8_t onewire_test_mac[6] = { 0x52, 0x54, 0x00, 0x11, 0x22, 0x33 };
#define ONEWIRE_TEST_MAC_STR "52:54:00:11:22:33"

/* One bus slot, returning the level sampled -- the same helper shape
 * sn0-bridge-test.c's nic_slot() uses for the Bridge's identical engine.
 */
static bool onewire_slot_node(QTestState *qts, uint64_t node,
                              unsigned int pulse, unsigned int sample)
{
    uint64_t control = node + MICROLAN_CONTROL;

    qtest_writel(qts, control, (pulse << 10) | (sample << 2));
    return qtest_readl(qts, control) & MICROLAN_LEVEL;
}

/* The same slot on node zero, the only node every other test here builds. */
static bool onewire_slot(QTestState *qts, unsigned int pulse,
                         unsigned int sample)
{
    return onewire_slot_node(qts, 0, pulse, sample);
}

/* The widths OpenBSD's and Linux's one-wire masters both use, per the
 * datasheet's "GPIO and MicroLAN" section: a reset finds presence, a read
 * and a written one share a slot, and a written zero is a longer low pulse.
 */
#define ONEWIRE_RESET(q)      onewire_slot((q), 520, 65)
#define ONEWIRE_READ(q)       onewire_slot((q), 6, 13)
#define ONEWIRE_WRITE_ONE(q)  onewire_slot((q), 6, 13)
#define ONEWIRE_WRITE_ZERO(q) onewire_slot((q), 80, 30)

static void onewire_write_byte(QTestState *qts, uint8_t byte)
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        if ((byte >> i) & 1) {
            ONEWIRE_WRITE_ONE(qts);
        } else {
            ONEWIRE_WRITE_ZERO(qts);
        }
    }
}

static uint8_t onewire_read_byte(QTestState *qts)
{
    unsigned int i;
    uint8_t byte = 0;

    for (i = 0; i < 8; i++) {
        if (ONEWIRE_READ(qts)) {
            byte |= 1u << i;
        }
    }
    return byte;
}

/*
 * The one-wire CRC-8: 0x8c reflected, seeded zero, sent as computed -- a
 * different algorithm and convention from the Bridge file's CRC-16.
 */
static uint8_t onewire_crc8(uint8_t crc, const uint8_t *data, unsigned int len)
{
    unsigned int i, bit;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8c : crc >> 1;
        }
    }
    return crc;
}

/* The standard one-wire sixteen-bit CRC, same algorithm and convention as
 * sn0-bridge-test.c's nic_crc() -- reused here rather than shared across
 * files, since qtest files in this tree do not share helper code.
 */
static uint16_t onewire_crc16(uint16_t crc, const uint8_t *data,
                              unsigned int len)
{
    unsigned int i, bit;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
    }
    return crc;
}

/*
 * The search exchange both guests perform before addressing the part.
 * Family and serial prefix are required; the low serial bits are the
 * model's own choice and not asserted.
 */
static void test_a_search_finds_the_ethernet_address_part(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint64_t rom = 0;
    unsigned int i;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = sn0_ioc3_attach_with_mac(sv[1], ONEWIRE_TEST_MAC_STR);

    g_assert_false(ONEWIRE_RESET(qts));    /* pulled low: the part is there */

    onewire_write_byte(qts, 0xf0);         /* Search ROM */
    for (i = 0; i < 64; i++) {
        bool bit = ONEWIRE_READ(qts);
        bool complement = ONEWIRE_READ(qts);

        g_assert_cmpint(complement, ==, !bit);
        if (bit) {
            rom |= 1ULL << i;
            ONEWIRE_WRITE_ONE(qts);
        } else {
            ONEWIRE_WRITE_ZERO(qts);
        }
    }

    g_assert_cmphex(rom & 0xff, ==, 0x91);              /* family */
    g_assert_cmphex((rom >> 44) & 0xfff, ==, 0x5e7);    /* serial prefix */

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Read ROM, and the identity down to its last bit. Two things no qtest
 * reaches otherwise.
 *
 * Nothing drives 0x33 here at all -- the one-command path a master takes once
 * it knows the bus holds a single part. tests/unit/test-onewire.c does drive
 * it against the shared bus, but neither the pipeline nor tools/pre-flight
 * runs the unit suite, so no check a change must pass before it lands has an
 * opinion about Read ROM.
 *
 * And the identity arrives only partly checked.
 * test_a_search_finds_the_ethernet_address_part pins the family byte and the
 * twelve serial bits OpenBSD's owmac_match() compares;
 * test_each_node_has_its_own_onewire_identity pins the ROM CRC. That leaves
 * thirty-six of the forty-eight serial bits unchecked.
 *
 * Both gaps are measured rather than argued: making Read ROM answer nothing
 * at all, and handing the part a serial the machine never chose, each leave
 * every other test in this file passing.
 *
 * The value pinned is the machine's rather than the chip's.
 * hw/mips/sn0-origin200.c gives each node's chip a serial one higher than the
 * last, so a guest counts two parts instead of rejecting a duplicate, and the
 * chip's own property default is only what a bare -device gets. Pinning what
 * a master reads is what ties that per-node choice to something a guest sees.
 *
 * The arbitrated path is deliberately not repeated here, having been measured
 * as covered: a search that delivers a wrong identity trips the family and
 * prefix checks in test_a_search_finds_the_ethernet_address_part, and one
 * plausible enough to slip past them still fails the CRC
 * test_each_node_has_its_own_onewire_identity computes over the bytes its own
 * search returned.
 */
#define ONEWIRE_NODE0_SERIAL 0x5e7000000001ULL

static void test_read_rom_returns_the_whole_identity(const void *data)
{
    uint8_t expected[8], id[8];
    unsigned int i;

    ioc3_reset();

    sn0_ioc3_attach(qts);
    /* Route the MicroLAN engine onto its pin, as a driver does before use. */
    qtest_writel(qts, PIN_CONTROL_SET, PIN_MICROLAN_ENABLE);

    expected[0] = 0x91;                                  /* family */
    for (i = 0; i < 6; i++) {                            /* serial, LSB first */
        expected[1 + i] = (ONEWIRE_NODE0_SERIAL >> (8 * i)) & 0xff;
    }
    expected[7] = onewire_crc8(0, expected, 7);

    g_assert_false(ONEWIRE_RESET(qts));    /* pulled low: the part is there */
    onewire_write_byte(qts, 0x33);         /* Read ROM */
    for (i = 0; i < 8; i++) {
        id[i] = onewire_read_byte(qts);
    }
    g_assert_cmpmem(id, sizeof(id), expected, sizeof(expected));
}

/*
 * A Match ROM naming another part leaves this one unselected; a
 * Skip-ROM-only model never answers OpenBSD's owmac.
 */
static void test_a_wrong_match_rom_leaves_the_part_unselected(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint64_t wrong_rom = 0x0000005e70000091ULL; /* right family, wrong rest */
    uint8_t leading;
    unsigned int i;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = sn0_ioc3_attach_with_mac(sv[1], ONEWIRE_TEST_MAC_STR);

    ONEWIRE_RESET(qts);
    onewire_write_byte(qts, 0x55);   /* Match ROM */
    for (i = 0; i < 8; i++) {
        onewire_write_byte(qts, (wrong_rom >> (8 * i)) & 0xff);
    }
    onewire_write_byte(qts, 0xf0);   /* Read Memory, if this landed */
    onewire_write_byte(qts, 0x00);
    onewire_write_byte(qts, 0x00);
    leading = onewire_read_byte(qts);
    g_assert_cmphex(leading, ==, 0xff);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The three guests' three selections -- OpenBSD Match ROM + 0xf0, Linux
 * Skip ROM + 0xc3, SGI's PROM straight from Search ROM -- must answer
 * identically.
 */
typedef enum OneWireSelection {
    ONEWIRE_SELECT_MATCH,
    ONEWIRE_SELECT_SKIP,
    ONEWIRE_SELECT_SEARCH,
} OneWireSelection;

static void read_ethernet_address_page(QTestState *qts,
                                       OneWireSelection selection,
                                       uint8_t cmd, uint8_t *leading_crc,
                                       uint8_t page[32], uint8_t *trailing_crc)
{
    unsigned int i;

    ONEWIRE_RESET(qts);
    if (selection != ONEWIRE_SELECT_SKIP) {
        uint64_t rom = 0;

        onewire_write_byte(qts, 0xf0);     /* Search ROM, to learn the id */
        for (i = 0; i < 64; i++) {
            bool bit = ONEWIRE_READ(qts);

            ONEWIRE_READ(qts);              /* the complement, unused here */
            if (bit) {
                rom |= 1ULL << i;
                ONEWIRE_WRITE_ONE(qts);
            } else {
                ONEWIRE_WRITE_ZERO(qts);
            }
        }
        if (selection == ONEWIRE_SELECT_MATCH) {
            ONEWIRE_RESET(qts);
            onewire_write_byte(qts, 0x55);     /* Match ROM */
            for (i = 0; i < 8; i++) {
                onewire_write_byte(qts, (rom >> (8 * i)) & 0xff);
            }
        }
    } else {
        onewire_write_byte(qts, 0xcc);     /* Skip ROM */
    }

    onewire_write_byte(qts, cmd);
    onewire_write_byte(qts, 0x00);         /* target address, low */
    onewire_write_byte(qts, 0x00);         /* target address, high */

    *leading_crc = onewire_read_byte(qts);
    for (i = 0; i < 32; i++) {
        page[i] = onewire_read_byte(qts);
    }
    *trailing_crc = onewire_read_byte(qts);
}

static void assert_ethernet_address_page(QTestState *qts,
                                         OneWireSelection selection,
                                         uint8_t cmd,
                                         const uint8_t expected_mac[6])
{
    uint8_t leading, page[32], trailing;
    uint8_t echo[3] = { cmd, 0x00, 0x00 };
    uint8_t got_mac[6];
    unsigned int i;

    read_ethernet_address_page(qts, selection, cmd, &leading, page,
                               &trailing);

    g_assert_cmphex(leading, ==, onewire_crc8(0, echo, sizeof(echo)));
    g_assert_cmphex(page[0], ==, 0x0a);

    /* Bytes 5..10 hold the station address reversed: byte 10 is the first
     * octet, byte 5 the last -- see the datasheet section named above.
     */
    for (i = 0; i < 6; i++) {
        got_mac[i] = page[10 - i];
    }
    g_assert_cmpmem(got_mac, sizeof(got_mac), expected_mac, sizeof(got_mac));

    g_assert_cmphex(onewire_crc16(0, page, 13), ==, 0xb001);

    /*
     * What follows the thirty-second byte is the whole difference between
     * the two read commands, and they are not interchangeable on this part.
     * 0xc3 closes every 32-byte page with a CRC-8 over that page's data.
     * Plain 0xf0 carries no per-page CRC at all -- its only data CRC comes
     * at the end of memory -- so it runs straight on into the next page,
     * and the part holds one programmed page, the rest unprogrammed EPROM.
     */
    if (cmd == 0xc3) {
        g_assert_cmphex(trailing, ==, onewire_crc8(0, page, 32));
    } else {
        g_assert_cmphex(trailing, ==, 0xff);
    }
}

static void test_reading_the_page_via_read_memory(const void *data)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = sn0_ioc3_attach_with_mac(sv[1], ONEWIRE_TEST_MAC_STR);

    /* OpenBSD's owmac: Match ROM, then the plain Read Memory command. */
    assert_ethernet_address_page(qts, ONEWIRE_SELECT_MATCH, 0xf0,
                                 onewire_test_mac);

    qtest_quit(qts);
    close(sv[0]);
}

static void test_reading_the_page_via_read_memory_crc(const void *data)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = sn0_ioc3_attach_with_mac(sv[1], ONEWIRE_TEST_MAC_STR);

    /* Linux's w1_ds2502_read_page: Skip ROM, then Read Memory-with-CRC. */
    assert_ethernet_address_page(qts, ONEWIRE_SELECT_SKIP, 0xc3,
                                 onewire_test_mac);

    qtest_quit(qts);
    close(sv[0]);
}

static void test_reading_the_page_after_search(const void *data)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = sn0_ioc3_attach_with_mac(sv[1], ONEWIRE_TEST_MAC_STR);

    /* SGI's PROM continues directly from Search ROM to Read Memory. */
    assert_ethernet_address_page(qts, ONEWIRE_SELECT_SEARCH, 0xf0,
                                 onewire_test_mac);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A backend with no mac= must get an SGI address: the PROM reads the
 * one-wire part before enabling the engine, and that is the
 * firmware-visible contract deciding whether netboot starts.
 */
static void test_the_default_station_address_is_an_sgi_address(const void *data)
{
    static const uint8_t expected[6] = {
        0x08, 0x00, 0x69, 0x12, 0x34, 0x56,
    };
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);
    /* Route the MicroLAN engine onto its pin, as a driver does before use. */
    qtest_writel(qts, PIN_CONTROL_SET, PIN_MICROLAN_ENABLE);

    assert_ethernet_address_page(qts, ONEWIRE_SELECT_SEARCH, 0xf0, expected);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The converse: mac= is the operator's word, never rewritten -- even
 * inside the generator's own range, the one range address bytes cannot
 * attribute.
 */
static const uint8_t explicit_prefix_mac[6] = {
    0x52, 0x54, 0x00, 0x12, 0x34, 0x56
};
#define EXPLICIT_PREFIX_MAC_STR "52:54:00:12:34:56"

static void test_an_explicit_mac_in_the_generator_range_stays(const void *data)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = sn0_ioc3_attach_with_mac(sv[1], EXPLICIT_PREFIX_MAC_STR);

    assert_ethernet_address_page(qts, ONEWIRE_SELECT_MATCH, 0xf0,
                                 explicit_prefix_mac);

    qtest_quit(qts);
    close(sv[0]);
}

/* The same promise on the -device path, where no -nic parsing intervenes. */
static void test_a_device_supplied_mac_survives(const void *data)
{
    QTestState *qts;
    QDict *resp;

    qts = qtest_init("-M origin200 "
                     "-device sgi-ioc3,id=spare,mac=" EXPLICIT_PREFIX_MAC_STR);

    resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                     "{ 'path': '/machine/peripheral/spare', "
                     "'property': 'mac' } }");
    if (!qdict_haskey(resp, "return")) {
        g_autoptr(GString) text = qobject_to_json(QOBJECT(resp));

        g_error("qom-get of the spare chip's mac failed: %s", text->str);
    }
    g_assert_cmpstr(qdict_get_str(resp, "return"), ==,
                    EXPLICIT_PREFIX_MAC_STR);
    qobject_unref(resp);

    qtest_quit(qts);
}

/*
 * The redirection bytes OpenBSD reads before trusting a page: it starts a
 * Read Status at 0x0001, page zero's redirection byte, and takes four --
 * one per page. A redirection byte of 0xff means "this page is where it
 * says it is", and it is what ends owmem_redirect()'s walk rather than
 * failing it.
 *
 * The value is pinned here because nothing in the model produces it on
 * purpose: 0xff is what an unprogrammed EPROM byte reads as, and the
 * station-address record programs only page zero's data, never the status
 * field. An initialisation that filled status memory with anything else
 * would leave every other one-wire test passing and stop OpenBSD dead.
 */
static void test_read_status_returns_no_redirection(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t leading;
    uint8_t status[4];
    uint8_t echo[3] = { 0xaa, 0x01, 0x00 };
    unsigned int i;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = sn0_ioc3_attach_with_mac(sv[1], ONEWIRE_TEST_MAC_STR);

    ONEWIRE_RESET(qts);
    onewire_write_byte(qts, 0xcc);   /* one part on the bus */
    onewire_write_byte(qts, 0xaa);   /* Read Status */
    onewire_write_byte(qts, 0x01);   /* the offset owmac always asks for */
    onewire_write_byte(qts, 0x00);

    leading = onewire_read_byte(qts);
    g_assert_cmphex(leading, ==, onewire_crc8(0, echo, sizeof(echo)));

    for (i = 0; i < 4; i++) {
        status[i] = onewire_read_byte(qts);
    }
    /* The first is page zero's, the page the station address lives in. */
    g_assert_cmphex(status[0], ==, 0xff);
    g_assert_cmphex(status[1], ==, 0xff);
    g_assert_cmphex(status[2], ==, 0xff);
    g_assert_cmphex(status[3], ==, 0xff);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The whole status field, which is not flat. A DS2502-class part serves
 * eight bytes -- the write-protect byte, four redirection bytes, two
 * reserved bytes, and a factory-programmed 0x00 at 0x0007 -- and then a
 * CRC-8 over the bytes it just sent. Only 0x0007 is programmed, so the
 * seven before it read as unprogrammed EPROM; a readout that runs to the
 * end of the field is therefore never all ones.
 */
static void test_read_status_serves_the_whole_field(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t leading, crc;
    uint8_t status[8];
    uint8_t echo[3] = { 0xaa, 0x00, 0x00 };
    unsigned int i;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = sn0_ioc3_attach_with_mac(sv[1], ONEWIRE_TEST_MAC_STR);

    ONEWIRE_RESET(qts);
    onewire_write_byte(qts, 0xcc);   /* one part on the bus */
    onewire_write_byte(qts, 0xaa);   /* Read Status */
    onewire_write_byte(qts, 0x00);   /* from the first byte of the field */
    onewire_write_byte(qts, 0x00);

    leading = onewire_read_byte(qts);
    g_assert_cmphex(leading, ==, onewire_crc8(0, echo, sizeof(echo)));

    for (i = 0; i < sizeof(status); i++) {
        status[i] = onewire_read_byte(qts);
    }
    crc = onewire_read_byte(qts);

    for (i = 0; i < 7; i++) {
        g_assert_cmphex(status[i], ==, 0xff);
    }
    g_assert_cmphex(status[7], ==, 0x00);

    g_assert_cmphex(crc, ==, onewire_crc8(0, status, sizeof(status)));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A bound on blocking for a late frame: delivery may still be in flight
 * through the main loop when the triggering write returns.
 */
static void net_socket_set_recv_timeout(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };

    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)),
                    ==, 0);
}

/* The framing net_socket_send uses: a four-byte network-order length ahead
 * of each packet, in both directions over the connected socket.
 */
static void net_socket_send_frame(int fd, const uint8_t *buf, size_t len)
{
    uint32_t hdr = htonl((uint32_t)len);

    g_assert_cmpint(send(fd, &hdr, sizeof(hdr), 0), ==, (int)sizeof(hdr));
    g_assert_cmpint(send(fd, buf, len, 0), ==, (int)len);
}

static size_t net_socket_recv_frame(int fd, uint8_t *buf, size_t buf_size)
{
    uint32_t hdr;
    size_t len;

    g_assert_cmpint(recv(fd, &hdr, sizeof(hdr), 0), ==, (int)sizeof(hdr));
    len = ntohl(hdr);
    g_assert_cmpint(len, <=, buf_size);
    g_assert_cmpint(recv(fd, buf, len, 0), ==, (int)len);
    return len;
}

/*
 * For asserting a frame was NOT sent: an absence is waited for in full,
 * so the timeout is short -- but long enough for a frame already in
 * flight to arrive, or the test passes for the wrong reason.
 */
static bool net_socket_frame_arrived(int fd)
{
    uint32_t hdr;
    ssize_t got;

    net_socket_set_recv_timeout(fd, 1);
    got = recv(fd, &hdr, sizeof(hdr), 0);
    net_socket_set_recv_timeout(fd, 10);

    return got == (ssize_t)sizeof(hdr);
}

static void test_the_ethernet_transmit_sends_inline_frame(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[14] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x08, 0x00 };
    uint8_t received[64];
    size_t got;
    uint32_t desc = RAM_SCRATCH;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN);

    /*
     * The buffer-count word's low seven bits repeat the inline length,
     * which is what the engine actually reads.
     */
    qtest_writel(qts, desc + 0, sizeof(frame) | TXD_DATA_VALID | TXD_INT_DONE);
    qtest_writel(qts, desc + 4, sizeof(frame));
    qtest_memwrite(qts, desc + TXD_DATA_OFFSET, frame, sizeof(frame));

    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);

    /* The doorbell: one descriptor further than the ring base. */
    qtest_writel(qts, TX_PRODUCE, 128);

    got = net_socket_recv_frame(sv[0], received, sizeof(received));
    g_assert_cmpint(got, ==, sizeof(frame));
    g_assert_cmpmem(received, got, frame, sizeof(frame));

    g_assert_cmphex(qtest_readl(qts, TX_CONSUME), ==,
                    (128u | TX_CONSUME_IDLE));
    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) &
                    (ENET_INT_TX_EMPTY | ENET_INT_TX_COMPLETE), ==,
                    ENET_INT_TX_EMPTY | ENET_INT_TX_COMPLETE);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Enabling transmit is a start, not only a gate: the diagnostic rings the
 * doorbell first and enables afterwards, an order no guest driver uses.
 */
static void test_transmit_starts_when_enabled_late(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[14] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x08, 0x00 };
    uint8_t received[64];
    size_t got;
    uint32_t desc = RAM_SCRATCH;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, desc + 0, sizeof(frame) | TXD_DATA_VALID | TXD_INT_DONE);
    qtest_writel(qts, desc + 4, sizeof(frame));
    qtest_memwrite(qts, desc + TXD_DATA_OFFSET, frame, sizeof(frame));

    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);

    /* The doorbell, with transmit still off. Nothing may go out yet. */
    qtest_writel(qts, TX_PRODUCE, 128);
    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) & ENET_INT_TX_EMPTY, ==,
                    0);

    /* Turning transmit on is what starts it. */
    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN);

    got = net_socket_recv_frame(sv[0], received, sizeof(received));
    g_assert_cmpint(got, ==, sizeof(frame));
    g_assert_cmpmem(received, got, frame, sizeof(frame));

    g_assert_cmphex(qtest_readl(qts, TX_CONSUME), ==, (128u | TX_CONSUME_IDLE));
    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) & ENET_INT_TX_EMPTY, ==,
                    ENET_INT_TX_EMPTY);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A second MAC control write must not resend the ring: the firmware
 * touches the register twice more adding the receive enables.
 */
static void test_a_second_enable_write_does_not_resend(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[14] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x08, 0x00 };
    uint8_t received[64];
    uint32_t desc = RAM_SCRATCH;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, desc + 0, sizeof(frame) | TXD_DATA_VALID);
    qtest_writel(qts, desc + 4, sizeof(frame));
    qtest_memwrite(qts, desc + TXD_DATA_OFFSET, frame, sizeof(frame));

    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);
    qtest_writel(qts, TX_PRODUCE, 128);

    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN);
    g_assert_cmpint(net_socket_recv_frame(sv[0], received, sizeof(received)),
                    ==, sizeof(frame));

    /* The receive enables arrive next, exactly as the firmware writes them. */
    qtest_writel(qts, MAC_CONTROL,
                 MCR_TX_DMA_EN | MCR_TX_EN | MCR_RX_DMA_EN | MCR_RX_EN);

    /* Nothing further reaches the backend. */
    g_assert_false(net_socket_frame_arrived(sv[0]));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The seven-bit inline length can name more than the 104-byte area; an
 * engine trusting it sends bytes past its own descriptor copy. Payload
 * bytes carry their offsets so a misplaced start fails on content too.
 */
static void test_the_ethernet_transmit_bounds_the_inline_area(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t area[TXD_DATA_SIZE];
    uint8_t received[256];
    size_t got;
    unsigned int i;
    uint32_t desc = RAM_SCRATCH;

    for (i = 0; i < sizeof(area); i++) {
        area[i] = (uint8_t)(TXD_DATA_OFFSET + i);
    }

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN);

    qtest_writel(qts, desc + 0, TXD_DATA_LEN_MAX | TXD_DATA_VALID);
    qtest_writel(qts, desc + 4, TXD_DATA_LEN_MAX);
    qtest_memwrite(qts, desc + TXD_DATA_OFFSET, area, sizeof(area));

    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);
    qtest_writel(qts, TX_PRODUCE, 128);

    got = net_socket_recv_frame(sv[0], received, sizeof(received));
    g_assert_cmpint(got, ==, TXD_DATA_SIZE);
    g_assert_cmpmem(received, got, area, sizeof(area));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The walk exits on index equality, so a produce index the steps never
 * land on is a hang from two register writes; a model without the bound
 * fails as a timeout, the only way a hang can fail.
 */
static void test_an_unaligned_produce_index_terminates(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint32_t desc = RAM_SCRATCH;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN);
    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);

    /* Both indices written raw, neither on a descriptor boundary. */
    qtest_writel(qts, TX_CONSUME, 0);
    qtest_writel(qts, TX_PRODUCE, 1);

    /*
     * One byte is below a descriptor, so there is nothing to send and the
     * engine comes to rest where it started, idle.
     */
    g_assert_cmphex(qtest_readl(qts, TX_CONSUME), ==, TX_CONSUME_IDLE);

    /*
     * Eight tells a correct bound from eight-byte rounding; every other
     * value here rounds identically under both rules.
     */
    qtest_writel(qts, TX_CONSUME, 0);
    qtest_writel(qts, TX_PRODUCE, 8);
    g_assert_cmphex(qtest_readl(qts, TX_CONSUME), ==, TX_CONSUME_IDLE);

    /* And it is still a working engine afterwards rather than a wedged one. */
    qtest_writel(qts, TX_CONSUME, 3);
    qtest_writel(qts, TX_PRODUCE, 128 + 5);
    g_assert_cmphex(qtest_readl(qts, TX_CONSUME), ==, (128u | TX_CONSUME_IDLE));

    qtest_quit(qts);
    close(sv[0]);
}

static void test_the_ethernet_transmit_honours_total_byte_count(
    const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t inline_data[18] = {
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x08, 0x00, 0xde, 0xad, 0xbe, 0xef,
    };
    uint8_t received[64];
    size_t got;
    uint32_t desc = RAM_SCRATCH;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN);

    /* The segment includes padding, but the command gives the wire length. */
    qtest_writel(qts, desc + 0, 14 | TXD_DATA_VALID);
    qtest_writel(qts, desc + 4, sizeof(inline_data));
    qtest_memwrite(qts, desc + TXD_DATA_OFFSET,
                   inline_data, sizeof(inline_data));
    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);
    qtest_writel(qts, TX_PRODUCE, 128);

    got = net_socket_recv_frame(sv[0], received, sizeof(received));
    g_assert_cmpint(got, ==, 14);
    g_assert_cmpmem(received, got, inline_data, 14);

    qtest_quit(qts);
    close(sv[0]);
}

static void test_the_ethernet_transmit_gathers_two_buffers(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t buf1[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    uint8_t buf2[4] = { 0x0a, 0x0b, 0x0c, 0x0d };
    uint8_t expect[10];
    uint8_t received[64];
    size_t got;
    uint32_t desc = RAM_SCRATCH;
    uint32_t buf1_addr = RAM_SCRATCH + 0x1000;
    uint32_t buf2_addr = RAM_SCRATCH + 0x2000;

    memcpy(expect, buf1, sizeof(buf1));
    memcpy(expect + sizeof(buf1), buf2, sizeof(buf2));

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN);

    qtest_memwrite(qts, buf1_addr, buf1, sizeof(buf1));
    qtest_memwrite(qts, buf2_addr, buf2, sizeof(buf2));

    /*
     * No inline data: both validity flags name out-of-line buffers, read
     * in flag order.
     */
    qtest_writel(qts, desc + 0, sizeof(expect) | TXD_BUF1_VALID |
                TXD_BUF2_VALID);
    qtest_writel(qts, desc + 4, (sizeof(buf1) << TXD_BUF1_LEN_SHIFT) |
                (sizeof(buf2) << TXD_BUF2_LEN_SHIFT));
    qtest_writel(qts, desc + 8, 0);
    qtest_writel(qts, desc + 12, buf1_addr);
    qtest_writel(qts, desc + 16, 0);
    qtest_writel(qts, desc + 20, buf2_addr);

    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);
    qtest_writel(qts, TX_PRODUCE, 128);

    got = net_socket_recv_frame(sv[0], received, sizeof(received));
    g_assert_cmpint(got, ==, sizeof(expect));
    g_assert_cmpmem(received, got, expect, sizeof(expect));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * RFC 1071, hand rolled: the assertion is against an independent
 * computation, not two copies of one code agreeing.
 */
static uint32_t inet_sum_add(uint32_t sum, const uint8_t *data, size_t len)
{
    size_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)data[i] << 8) | data[i + 1];
    }
    if (len & 1) {
        sum += (uint32_t)data[len - 1] << 8;
    }
    return sum;
}

static uint16_t inet_sum_fold(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/*
 * The RFC 793/768 transport checksum over pseudo header plus segment with
 * its checksum field zeroed. The caller's buffer is untouched, so this
 * runs against a segment still carrying a deliberately wrong checksum.
 */
static uint16_t inet_transport_checksum(uint8_t proto,
                                        const uint8_t src_ip[4],
                                        const uint8_t dst_ip[4],
                                        const uint8_t *segment,
                                        size_t seg_len, size_t csum_off)
{
    uint8_t pseudo[12];
    uint8_t zeroed[64];
    uint32_t sum;

    g_assert_cmpuint(seg_len, <=, sizeof(zeroed));
    memcpy(zeroed, segment, seg_len);
    zeroed[csum_off] = 0;
    zeroed[csum_off + 1] = 0;

    memcpy(pseudo, src_ip, 4);
    memcpy(pseudo + 4, dst_ip, 4);
    pseudo[8] = 0;
    pseudo[9] = proto;
    pseudo[10] = (uint8_t)(seg_len >> 8);
    pseudo[11] = (uint8_t)seg_len;

    sum = inet_sum_add(0, pseudo, sizeof(pseudo));
    sum = inet_sum_add(sum, zeroed, seg_len);
    return inet_sum_fold(sum);
}

/*
 * Insert-checksum must leave a correct UDP checksum, not the driver's
 * pseudo-header partial; the frame carries a deliberately wrong field so
 * leaving it alone fails.
 */
static void test_the_ethernet_transmit_inserts_udp_checksum(const void *data)
{
    int sv[2];
    QTestState *qts;
    static const uint8_t src_ip[4] = { 10, 0, 2, 15 };
    static const uint8_t dst_ip[4] = { 10, 0, 2, 3 };
    uint8_t frame[46] = {
        /* Ethernet header: destination, source, ethertype IPv4. */
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x08, 0x00,
        /* IPv4 header: version/IHL, DSCP, total length 32, id 0,
         * flags/frag 0, ttl 64, protocol UDP, header checksum 0 (the
         * driver's own, untouched by this engine), source 10.0.2.15,
         * destination 10.0.2.3. */
        0x45, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
        64, 17, 0x00, 0x00, 10, 0, 2, 15, 10, 0, 2, 3,
        /* UDP header: source port 1024, destination port 53 (DNS), length
         * 12, and a checksum that is deliberately wrong. */
        0x04, 0x00, 0x00, 0x35, 0x00, 0x0c, 0xde, 0xad,
        /* Payload. */
        'p', 'i', 'n', 'g',
    };
    uint8_t received[128];
    uint16_t correct_csum;
    size_t got;
    uint32_t desc = RAM_SCRATCH;
    const unsigned int udp_off = 34;
    const unsigned int udp_len = 12;
    const unsigned int csoff = udp_off + 6;

    correct_csum = inet_transport_checksum(17, src_ip, dst_ip,
                                           frame + udp_off, udp_len, 6);
    /*
     * Guard the test's own premise: if the payload happened to checksum to
     * the same value the field already (wrongly) holds, a model that never
     * touches the field would pass by accident.
     */
    g_assert_cmphex(correct_csum, !=, 0xdead);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN);

    qtest_writel(qts, desc + 0, sizeof(frame) | TXD_DATA_VALID |
                TXD_DOCHECKSUM | (csoff << TXD_CHKOFF_SHIFT));
    qtest_writel(qts, desc + 4, sizeof(frame));
    qtest_memwrite(qts, desc + TXD_DATA_OFFSET, frame, sizeof(frame));

    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);
    qtest_writel(qts, TX_PRODUCE, 128);

    got = net_socket_recv_frame(sv[0], received, sizeof(received));
    g_assert_cmpint(got, ==, sizeof(frame));

    /* Everything but the two checksum bytes must be untouched. */
    g_assert_cmpmem(received, csoff, frame, csoff);
    g_assert_cmpmem(received + csoff + 2, got - csoff - 2,
                    frame + csoff + 2, sizeof(frame) - csoff - 2);

    g_assert_cmphex(received[csoff], ==, (uint8_t)(correct_csum >> 8));
    g_assert_cmphex(received[csoff + 1], ==, (uint8_t)correct_csum);

    qtest_quit(qts);
    close(sv[0]);
}

/* The TCP case, alongside UDP's -- both are ETXD_DOCHECKSUM's transport. */
static void test_the_ethernet_transmit_inserts_tcp_checksum(const void *data)
{
    int sv[2];
    QTestState *qts;
    static const uint8_t src_ip[4] = { 10, 0, 2, 15 };
    static const uint8_t dst_ip[4] = { 10, 0, 2, 3 };
    uint8_t frame[54] = {
        /* Ethernet header: destination, source, ethertype IPv4. */
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x08, 0x00,
        /* IPv4 header: version/IHL, DSCP, total length 40, id 0,
         * flags/frag 0, ttl 64, protocol TCP, header checksum 0 (the
         * driver's own, untouched by this engine), source 10.0.2.15,
         * destination 10.0.2.3. */
        0x45, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00,
        64, 6, 0x00, 0x00, 10, 0, 2, 15, 10, 0, 2, 3,
        /* TCP header, no options: source port 4096, destination port 80,
         * sequence number 1, no ack, data offset 5 with SYN set, window
         * 8192, a deliberately wrong checksum, no urgent pointer. */
        0x10, 0x00, 0x00, 0x50,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00,
        0x50, 0x02, 0x20, 0x00,
        0xbe, 0xef, 0x00, 0x00,
    };
    uint8_t received[128];
    uint16_t correct_csum;
    size_t got;
    uint32_t desc = RAM_SCRATCH;
    const unsigned int tcp_off = 34;
    const unsigned int tcp_len = 20;
    const unsigned int csoff = tcp_off + 16;

    correct_csum = inet_transport_checksum(6, src_ip, dst_ip,
                                           frame + tcp_off, tcp_len, 16);
    g_assert_cmphex(correct_csum, !=, 0xbeef);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN);

    qtest_writel(qts, desc + 0, sizeof(frame) | TXD_DATA_VALID |
                TXD_DOCHECKSUM | (csoff << TXD_CHKOFF_SHIFT));
    qtest_writel(qts, desc + 4, sizeof(frame));
    qtest_memwrite(qts, desc + TXD_DATA_OFFSET, frame, sizeof(frame));

    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);
    qtest_writel(qts, TX_PRODUCE, 128);

    got = net_socket_recv_frame(sv[0], received, sizeof(received));
    g_assert_cmpint(got, ==, sizeof(frame));

    g_assert_cmpmem(received, csoff, frame, csoff);
    g_assert_cmpmem(received + csoff + 2, got - csoff - 2,
                    frame + csoff + 2, sizeof(frame) - csoff - 2);

    g_assert_cmphex(received[csoff], ==, (uint8_t)(correct_csum >> 8));
    g_assert_cmphex(received[csoff + 1], ==, (uint8_t)correct_csum);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Without the bit the engine must not touch the checksum -- what tells
 * the positives from a model recomputing every frame regardless.
 */
static void test_the_ethernet_transmit_leaves_checksum_alone_when_not_asked(
    const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[46] = {
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x08, 0x00,
        0x45, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
        64, 17, 0x00, 0x00, 10, 0, 2, 15, 10, 0, 2, 3,
        0x04, 0x00, 0x00, 0x35, 0x00, 0x0c, 0xde, 0xad,
        'p', 'i', 'n', 'g',
    };
    uint8_t received[128];
    size_t got;
    uint32_t desc = RAM_SCRATCH;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN);

    /* No TXD_DOCHECKSUM this time. */
    qtest_writel(qts, desc + 0, sizeof(frame) | TXD_DATA_VALID);
    qtest_writel(qts, desc + 4, sizeof(frame));
    qtest_memwrite(qts, desc + TXD_DATA_OFFSET, frame, sizeof(frame));

    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);
    qtest_writel(qts, TX_PRODUCE, 128);

    got = net_socket_recv_frame(sv[0], received, sizeof(received));
    g_assert_cmpint(got, ==, sizeof(frame));
    g_assert_cmpmem(received, got, frame, sizeof(frame));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Pad-enable must extend a short frame to sixty zero bytes before the
 * FCS: IRIX and its netboot PROM never pad in software.
 */
static void test_the_ethernet_transmit_pads_short_frame(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[42];
    uint8_t expect[60];
    uint8_t received[128];
    size_t got;
    unsigned int i;
    uint32_t desc = RAM_SCRATCH;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 1);
    }
    memcpy(expect, frame, sizeof(frame));
    memset(expect + sizeof(frame), 0, sizeof(expect) - sizeof(frame));

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN | MCR_PAD_EN);

    qtest_writel(qts, desc + 0, sizeof(frame) | TXD_DATA_VALID);
    qtest_writel(qts, desc + 4, sizeof(frame));
    qtest_memwrite(qts, desc + TXD_DATA_OFFSET, frame, sizeof(frame));

    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);
    qtest_writel(qts, TX_PRODUCE, 128);

    got = net_socket_recv_frame(sv[0], received, sizeof(received));
    g_assert_cmpint(got, ==, sizeof(expect));
    g_assert_cmpmem(received, got, expect, sizeof(expect));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Pad-enable clear leaves the short frame at its assembled length -- the
 * case that catches unconditional padding.
 */
static void test_the_ethernet_transmit_leaves_short_frame_alone_when_pad_off(
    const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[42];
    uint8_t received[128];
    size_t got;
    unsigned int i;
    uint32_t desc = RAM_SCRATCH;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 1);
    }

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    /* Pad-enable clear this time. */
    qtest_writel(qts, MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN);

    qtest_writel(qts, desc + 0, sizeof(frame) | TXD_DATA_VALID);
    qtest_writel(qts, desc + 4, sizeof(frame));
    qtest_memwrite(qts, desc + TXD_DATA_OFFSET, frame, sizeof(frame));

    qtest_writel(qts, TX_RING_HI, 0);
    qtest_writel(qts, TX_RING_LO, desc & 0xffffc000u);
    qtest_writel(qts, TX_PRODUCE, 128);

    got = net_socket_recv_frame(sv[0], received, sizeof(received));
    g_assert_cmpint(got, ==, sizeof(frame));
    g_assert_cmpmem(received, got, frame, sizeof(frame));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The block reset keeps the ring bases -- the diagnostic never rewrites
 * them -- and must clear the indices, or preserving everything passes.
 */
static void test_the_block_reset_keeps_the_ring_addresses(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    qtest_writel(qts, TX_RING_HI, 0xa0000000);
    qtest_writel(qts, TX_RING_LO, 0x01500001);
    qtest_writel(qts, RX_RING_HI, 0xa0000000);
    qtest_writel(qts, RX_RING_LO, 0x01521000);
    qtest_writel(qts, TX_PRODUCE, 128);

    qtest_writel(qts, MAC_CONTROL, MCR_RESET);

    g_assert_cmphex(qtest_readl(qts, TX_RING_HI), ==, 0xa0000000);
    g_assert_cmphex(qtest_readl(qts, TX_RING_LO), ==, 0x01500001);
    g_assert_cmphex(qtest_readl(qts, RX_RING_HI), ==, 0xa0000000);
    g_assert_cmphex(qtest_readl(qts, RX_RING_LO), ==, 0x01521000);

    /* And the engine's own state is gone. */
    g_assert_cmphex(qtest_readl(qts, TX_PRODUCE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS), ==, 0);
}

/*
 * The checksum-offload partial: these tests replicate Linux's own
 * reconstruction over the bytes it reads, requiring 0xffff for a good
 * frame and never for a corrupt one.
 */
static uint16_t rxcsum_fold(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return sum;
}

static uint32_t rxcsum_be16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}

/* One's-complement 16-bit sum over a byte range, trailing odd byte padded high
 * -- the ordinary Internet checksum accumulator, not yet complemented. */
static uint16_t rxcsum_ones(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += (data[i] << 8) | data[i + 1];
    }
    if (i < len) {
        sum += data[i] << 8;
    }
    return rxcsum_fold(sum);
}

/* The value an IPv4 header checksum field carries. */
static uint16_t rxcsum_ip(const uint8_t *iphdr, size_t ihl)
{
    return ~rxcsum_ones(iphdr, ihl) & 0xffff;
}

/* The value a TCP or UDP checksum field carries: the transport segment summed
 * under its IPv4 pseudo-header, with the checksum field itself zero. */
static uint16_t rxcsum_transport(const uint8_t *saddr, const uint8_t *daddr,
                                 uint8_t proto, const uint8_t *seg,
                                 size_t seglen)
{
    uint32_t sum = rxcsum_be16(saddr) + rxcsum_be16(saddr + 2)
                 + rxcsum_be16(daddr) + rxcsum_be16(daddr + 2)
                 + proto + seglen
                 + rxcsum_ones(seg, seglen);
    return ~rxcsum_fold(sum) & 0xffff;
}

/*
 * Linux's ioc3_tcpudp_checksum() arithmetic replicated; big-endian words
 * are read explicitly because the driver's CPU is big-endian and the test
 * host may not be.
 */
static uint32_t rxcsum_driver(const uint8_t *eh, int len, uint32_t hwsum)
{
    const uint8_t *ih = eh + 14;                 /* ETH_HLEN */
    int ihl = (ih[0] & 0x0f) << 2;
    const uint8_t *cp = eh + len;                /* where the driver reads CRC */
    uint32_t csum, ehsum;

    csum = hwsum
         + (rxcsum_be16(ih + 2) - ihl)           /* tot_len - IP header length */
         + ih[9]                                 /* htons(proto), big-endian */
         + rxcsum_be16(ih + 12) + rxcsum_be16(ih + 14)   /* source address */
         + rxcsum_be16(ih + 16) + rxcsum_be16(ih + 18);  /* dest address */

    ehsum = rxcsum_be16(eh) + rxcsum_be16(eh + 2) + rxcsum_be16(eh + 4)
          + rxcsum_be16(eh + 6) + rxcsum_be16(eh + 8) + rxcsum_be16(eh + 10)
          + rxcsum_be16(eh + 12);
    ehsum = (ehsum & 0xffff) + (ehsum >> 16);
    ehsum = (ehsum & 0xffff) + (ehsum >> 16);
    csum += 0xffff ^ ehsum;                      /* subtract the MAC header */

    if (len & 1) {
        csum += 0xffff ^ (uint16_t)((cp[1] << 8) | cp[0]);
        csum += 0xffff ^ (uint16_t)((cp[3] << 8) | cp[2]);
    } else {
        csum += 0xffff ^ (uint16_t)((cp[0] << 8) | cp[1]);
        csum += 0xffff ^ (uint16_t)((cp[2] << 8) | cp[3]);
    }

    csum = (csum & 0xffff) + (csum >> 16);
    csum = (csum & 0xffff) + (csum >> 16);
    return csum;
}

/*
 * A well-formed IPv4 frame with correct checksums; corrupting a byte
 * after the fact is what the offload has to refuse.
 */
static size_t rxcsum_build_frame(uint8_t *out, uint8_t proto,
                                 const uint8_t *payload, size_t payload_len)
{
    static const uint8_t dst[6] = { 0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc };
    static const uint8_t src[6] = { 0x52, 0x54, 0x00, 0x11, 0x22, 0x33 };
    static const uint8_t sip[4] = { 192, 168, 0, 2 };
    static const uint8_t dip[4] = { 192, 168, 0, 1 };
    size_t l4hdr = (proto == 6) ? 20 : 8;
    size_t seglen = l4hdr + payload_len;
    size_t iptot = 20 + seglen;
    uint8_t *eh = out;
    uint8_t *ih = out + 14;
    uint8_t *seg = ih + 20;
    uint16_t c;

    memset(out, 0, 14 + iptot);
    memcpy(eh, dst, 6);
    memcpy(eh + 6, src, 6);
    eh[12] = 0x08;                               /* ethertype IPv4 */
    eh[13] = 0x00;

    ih[0] = 0x45;                                /* version 4, header 5 words */
    ih[2] = iptot >> 8;
    ih[3] = iptot & 0xff;
    ih[8] = 64;                                  /* time to live */
    ih[9] = proto;
    memcpy(ih + 12, sip, 4);
    memcpy(ih + 16, dip, 4);
    c = rxcsum_ip(ih, 20);
    ih[10] = c >> 8;
    ih[11] = c & 0xff;

    if (proto == 6) {                            /* TCP */
        seg[0] = 0xc0;                           /* source port */
        seg[2] = 0x00;                           /* dest port 80 */
        seg[3] = 0x50;
        seg[12] = 0x50;                          /* data offset 5 words */
        seg[13] = 0x10;                          /* ACK */
        seg[14] = 0x20;                          /* window */
    } else {                                     /* UDP */
        seg[0] = 0xc0;                           /* source port */
        seg[2] = 0x00;                           /* dest port 53 */
        seg[3] = 0x35;
        seg[4] = seglen >> 8;                    /* UDP length */
        seg[5] = seglen & 0xff;
    }
    memcpy(seg + l4hdr, payload, payload_len);

    c = rxcsum_transport(sip, dip, proto, seg, seglen);
    if (proto == 6) {
        seg[16] = c >> 8;                        /* TCP checksum */
        seg[17] = c & 0xff;
    } else {
        seg[6] = c >> 8;                         /* UDP checksum */
        seg[7] = c & 0xff;
    }
    return 14 + iptot;
}

/*
 * Deliver one frame and return Linux's folded reconstruction; only the
 * framing is asserted so a failure points at the checksum.
 */
static uint32_t rxcsum_deliver(const uint8_t *frame, size_t size)
{
    int sv[2];
    QTestState *qts;
    uint32_t ring = RAM_SCRATCH;
    uint32_t rxbuf = RAM_SCRATCH + 0x4000;
    uint8_t back[128];
    uint32_t word0, hwsum, csum;
    int len, attempt;

    g_assert_cmpuint(size + 4, <=, sizeof(back));
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    /*
     * These frames test the offload partial, not filtering; promiscuous
     * mode keeps the address filter out of their way.
     */
    qtest_writel(qts, MAC_CONTROL,
                MCR_PROMISC | MCR_RX_DMA_EN | MCR_RX_EN | MCR_RXOFF(33));
    qtest_writel(qts, RX_RING_HI, 0);
    qtest_writel(qts, RX_RING_LO, ring & 0xfffff000u);
    qtest_writel(qts, ring + 0, 0);
    qtest_writel(qts, ring + 4, rxbuf);
    qtest_writel(qts, RX_PRODUCE, 8);

    net_socket_send_frame(sv[0], frame, size);

    for (attempt = 0; attempt < 50; attempt++) {
        if (qtest_readl(qts, RX_CONSUME) != 0) {
            break;
        }
        g_usleep(10000);
        qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS / 4);
    }
    g_assert_cmphex(qtest_readl(qts, RX_CONSUME), ==, 8);

    word0 = qtest_readl(qts, rxbuf + 0);
    g_assert_cmphex(word0 & 0x80000000u, ==, 0x80000000u);    /* valid */
    len = (int)((word0 >> 16) & 0x7ff) - 4;
    g_assert_cmpint(len, ==, (int)size);
    hwsum = word0 & 0xffffu;

    /* The frame and the four FCS bytes the byte count above accounts for. */
    qtest_memread(qts, rxbuf + 66, back, size + 4);
    csum = rxcsum_driver(back, len, hwsum);

    qtest_quit(qts);
    close(sv[0]);
    return csum;
}

static void test_the_ethernet_receive_verifies_a_tcp_checksum(const void *data)
{
    /* Nine payload bytes make the frame odd, exercising the driver's odd-CRC
     * branch and the hardware sum's high-padded final byte. */
    static const uint8_t payload[9] = "checksum!";
    uint8_t frame[128];
    size_t size = rxcsum_build_frame(frame, 6, payload, sizeof(payload));

    g_assert_cmphex(rxcsum_deliver(frame, size), ==, 0xffff);
}

static void test_the_ethernet_receive_verifies_a_udp_checksum(const void *data)
{
    static const uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t frame[128];
    size_t size = rxcsum_build_frame(frame, 17, payload, sizeof(payload));

    g_assert_cmphex(rxcsum_deliver(frame, size), ==, 0xffff);
}

static void test_the_ethernet_receive_refuses_a_corrupt_checksum(const void *data)
{
    static const uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t frame[128];
    size_t size = rxcsum_build_frame(frame, 6, payload, sizeof(payload));

    /* Corrupt a transport payload byte after the checksums were computed: the
     * stored checksum no longer matches, and the reconstruction must not fold
     * to 0xffff -- a corrupt packet is never wrongly reported verified. */
    frame[14 + 20 + 20 + 2] ^= 0xff;

    g_assert_cmphex(rxcsum_deliver(frame, size), !=, 0xffff);
}

/*
 * Address filtering, with the station address programmed the way Linux's
 * __ioc3_set_mac_address() writes it.
 */
#define FILTER_STATION_HI 0x00005544u
#define FILTER_STATION_LO 0x33221100u

static void sn0_ioc3_filter_setup(QTestState *qts, uint32_t ring,
                                  uint32_t rxbuf, uint32_t mcr_extra)
{
    qtest_writel(qts, MAC_ADDR_HI, FILTER_STATION_HI);
    qtest_writel(qts, MAC_ADDR_LO, FILTER_STATION_LO);
    qtest_writel(qts, RX_RING_HI, 0);
    qtest_writel(qts, RX_RING_LO, ring & 0xfffff000u);
    qtest_writel(qts, ring + 0, 0);
    qtest_writel(qts, ring + 4, rxbuf);
    qtest_writel(qts, RX_PRODUCE, 8);
    qtest_writel(qts, MAC_CONTROL,
                mcr_extra | MCR_RX_DMA_EN | MCR_RX_EN | MCR_RXOFF(33));
}

/* Waited for rather than checked once: delivery happens inside the main
 * loop noticing the socket is readable, not synchronously with the send
 * that triggers it.
 */
static void sn0_ioc3_wait_for_consume(QTestState *qts)
{
    int attempt;

    for (attempt = 0; attempt < 50; attempt++) {
        if (qtest_readl(qts, RX_CONSUME) != 0) {
            break;
        }
        g_usleep(10000);
        qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS / 4);
    }
}

/*
 * A frame addressed to the station address the guest programmed lands in
 * the ring -- the ordinary case a filter must not break.
 */
static void test_address_filter_accepts_station_address(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x08, 0x06, 0xf0, 0x0d };
    uint32_t ring = RAM_SCRATCH + 0x10000;
    uint32_t rxbuf = RAM_SCRATCH + 0x14000;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    sn0_ioc3_filter_setup(qts, ring, rxbuf, 0);
    net_socket_send_frame(sv[0], frame, sizeof(frame));
    sn0_ioc3_wait_for_consume(qts);

    g_assert_cmphex(qtest_readl(qts, RX_CONSUME), ==, 8);
    g_assert_cmphex(qtest_readl(qts, rxbuf + 0) & 0x80000000u, ==, 0x80000000u);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A foreign unicast frame never reaches a buffer. Against an
 * accept-everything receive path, RX_CONSUME advances here exactly as in
 * the station-address test, and this fails.
 */
static void test_address_filter_drops_foreign_unicast(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x56,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x08, 0x06, 0xf0, 0x0d };
    uint32_t ring = RAM_SCRATCH + 0x18000;
    uint32_t rxbuf = RAM_SCRATCH + 0x1c000;
    int attempt;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    sn0_ioc3_filter_setup(qts, ring, rxbuf, 0);
    net_socket_send_frame(sv[0], frame, sizeof(frame));

    /*
     * Delivery is not synchronous with the send; the index staying put
     * through the wait is the assertion.
     */
    for (attempt = 0; attempt < 20; attempt++) {
        g_usleep(10000);
        qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS);
    }
    g_assert_cmphex(qtest_readl(qts, RX_CONSUME), ==, 0);
    g_assert_cmphex(qtest_readl(qts, rxbuf + 0), ==, 0);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A broadcast frame is delivered regardless of the station address -- the
 * filter's broadcast clause must not regress once station-address filtering
 * exists alongside it.
 */
static void test_the_address_filter_accepts_broadcast(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[16] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x08, 0x06, 0xf0, 0x0d };
    uint32_t ring = RAM_SCRATCH + 0x20000;
    uint32_t rxbuf = RAM_SCRATCH + 0x24000;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    sn0_ioc3_filter_setup(qts, ring, rxbuf, 0);
    net_socket_send_frame(sv[0], frame, sizeof(frame));
    sn0_ioc3_wait_for_consume(qts);

    g_assert_cmphex(qtest_readl(qts, RX_CONSUME), ==, 8);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The promiscuous bit overrides the filter entirely: the same foreign
 * unicast address the filter drops above is delivered once it is set.
 */
static void test_promiscuous_accepts_a_foreign_unicast_address(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x56,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x08, 0x06, 0xf0, 0x0d };
    uint32_t ring = RAM_SCRATCH + 0x28000;
    uint32_t rxbuf = RAM_SCRATCH + 0x2c000;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    sn0_ioc3_filter_setup(qts, ring, rxbuf, MCR_PROMISC);
    net_socket_send_frame(sv[0], frame, sizeof(frame));
    sn0_ioc3_wait_for_consume(qts);

    g_assert_cmphex(qtest_readl(qts, RX_CONSUME), ==, 8);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The hash's two halves against Linux's own ioc3_hash(): an IPv4
 * multicast address lands in the low register, an IPv6 one in the high,
 * proving both are wired to the filter.
 */
static void test_multicast_hash_accepts_a_set_bit_in_the_low_register(
    const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[16] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x08, 0x00, 0xf0, 0x0d };
    uint32_t ring = RAM_SCRATCH + 0x30000;
    uint32_t rxbuf = RAM_SCRATCH + 0x34000;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    sn0_ioc3_filter_setup(qts, ring, rxbuf, 0);
    qtest_writel(qts, HASH_LO, 1u << 31);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    sn0_ioc3_wait_for_consume(qts);

    g_assert_cmphex(qtest_readl(qts, RX_CONSUME), ==, 8);

    qtest_quit(qts);
    close(sv[0]);
}

static void test_multicast_hash_accepts_a_set_bit_in_the_high_register(
    const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[16] = { 0x33, 0x33, 0x00, 0x00, 0x00, 0x01,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x86, 0xdd, 0xf0, 0x0d };
    uint32_t ring = RAM_SCRATCH + 0x38000;
    uint32_t rxbuf = RAM_SCRATCH + 0x3c000;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    sn0_ioc3_filter_setup(qts, ring, rxbuf, 0);
    qtest_writel(qts, HASH_HI, 1u << 30);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    sn0_ioc3_wait_for_consume(qts);

    g_assert_cmphex(qtest_readl(qts, RX_CONSUME), ==, 8);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The same multicast address as the low-register test above, but with the
 * hash filter left clear: nothing selects its bit, so it is dropped just as
 * a foreign unicast address is.
 */
static void test_multicast_hash_drops_an_unset_bit(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[16] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x08, 0x00, 0xf0, 0x0d };
    uint32_t ring = RAM_SCRATCH + 0x40000;
    uint32_t rxbuf = RAM_SCRATCH + 0x44000;
    int attempt;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    sn0_ioc3_filter_setup(qts, ring, rxbuf, 0);
    net_socket_send_frame(sv[0], frame, sizeof(frame));

    for (attempt = 0; attempt < 20; attempt++) {
        g_usleep(10000);
        qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS);
    }
    g_assert_cmphex(qtest_readl(qts, RX_CONSUME), ==, 0);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Which source a delivered frame raises: three runs differing only in the
 * timer arming and the watermark. Linux enables the timer source and no
 * other, so announcing every frame on the threshold source leaves a Linux
 * guest with status & enable == 0 on every frame.
 */
static void sn0_ioc3_rx_one_frame(QTestState *qts, int host_fd, uint32_t ring,
                                  uint32_t rxbuf, uint32_t produce,
                                  uint32_t threshold)
{
    uint8_t frame[16] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x08, 0x06, 0xde, 0xad };
    unsigned int i;
    int attempt;

    qtest_writel(qts, MAC_CONTROL,
                MCR_RX_DMA_EN | MCR_RX_EN | MCR_RXOFF(33));
    qtest_writel(qts, RX_RING_HI, 0);
    qtest_writel(qts, RX_RING_LO, ring & 0xfffff000u);
    qtest_writel(qts, RX_TIMER, 0);
    qtest_writel(qts, RX_CONTROL, threshold);

    /*
     * Four buffers, one page apart, so a frame has somewhere to land and
     * the supply is deep enough for a watermark to sit below it.
     */
    for (i = 0; i < 4; i++) {
        qtest_writel(qts, ring + i * 8, 0);
        qtest_writel(qts, ring + i * 8 + 4, rxbuf + i * 0x1000);
    }

    qtest_writel(qts, RX_PRODUCE, produce);

    net_socket_send_frame(host_fd, frame, sizeof(frame));

    for (attempt = 0; attempt < 50; attempt++) {
        if (qtest_readl(qts, RX_CONSUME) != 0) {
            break;
        }
        g_usleep(10000);
        qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS / 4);
    }

    /*
     * Delivered, in every one of the three runs. An assertion about which
     * source was raised means nothing without this.
     */
    g_assert_cmphex(qtest_readl(qts, RX_CONSUME), ==, 8);
    g_assert_cmphex(qtest_readl(qts, rxbuf + 0) & 0x80000000u, ==, 0x80000000u);
}

/*
 * Armed, with the immediate reload both drivers write, and three buffers
 * still available against a watermark of none. The timer source is raised
 * and the threshold source is not.
 */
static void test_an_armed_receive_timer_raises_its_own_source(const void *data)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    sn0_ioc3_rx_one_frame(qts, sv[0], RAM_SCRATCH, RAM_SCRATCH + 0x4000,
                          (4 << 3) | RPIR_ARM, 0);

    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) &
                    (ENET_INT_RX_TIMER | ENET_INT_RX_THRESHOLD), ==,
                    ENET_INT_RX_TIMER);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Timer unarmed, supply above the mark: nothing raised -- the run that
 * fails an announce-everything engine.
 */
static void test_a_frame_below_the_watermark_raises_nothing(const void *data)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    sn0_ioc3_rx_one_frame(qts, sv[0], RAM_SCRATCH, RAM_SCRATCH + 0x4000,
                          4 << 3, 0);

    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) &
                    (ENET_INT_RX_TIMER | ENET_INT_RX_THRESHOLD), ==, 0);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The same again with the watermark raised to where this frame's delivery
 * puts the supply -- four buffers, one consumed, three left, watermark
 * three. The threshold source is raised and the unarmed timer is not.
 */
static void test_reaching_the_watermark_raises_the_threshold(const void *data)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    sn0_ioc3_rx_one_frame(qts, sv[0], RAM_SCRATCH, RAM_SCRATCH + 0x4000,
                          4 << 3, 3);

    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) &
                    (ENET_INT_RX_TIMER | ENET_INT_RX_THRESHOLD), ==,
                    ENET_INT_RX_THRESHOLD);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Past the watermark, not merely on it: equality raises once and goes
 * silent; at-or-below raises on both, which a low watermark asks for.
 */
static void test_falling_past_the_watermark_keeps_raising(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[16] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x08, 0x06, 0xbe, 0xef };
    uint32_t ring = RAM_SCRATCH;
    uint32_t rxbuf = RAM_SCRATCH + 0x4000;
    unsigned int i;
    int attempt;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    qtest_writel(qts, MAC_CONTROL,
                MCR_RX_DMA_EN | MCR_RX_EN | MCR_RXOFF(33));
    qtest_writel(qts, RX_RING_HI, 0);
    qtest_writel(qts, RX_RING_LO, ring & 0xfffff000u);
    qtest_writel(qts, RX_TIMER, 0);
    qtest_writel(qts, RX_CONTROL, 3);
    for (i = 0; i < 4; i++) {
        qtest_writel(qts, ring + i * 8, 0);
        qtest_writel(qts, ring + i * 8 + 4, rxbuf + i * 0x1000);
    }
    qtest_writel(qts, RX_PRODUCE, 4 << 3);

    for (i = 0; i < 2; i++) {
        qtest_writel(qts, ENET_INT_STATUS, 0xffffffffu);
        net_socket_send_frame(sv[0], frame, sizeof(frame));
        for (attempt = 0; attempt < 200; attempt++) {
            if (qtest_readl(qts, RX_CONSUME) == (i + 1) * 8) {
                break;
            }
            g_usleep(5000);
            qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS / 4);
        }
        g_assert_cmphex(qtest_readl(qts, RX_CONSUME), ==, (i + 1) * 8);

        /*
         * Both deliveries. The second is the one an equality test misses:
         * available has fallen to two and the watermark is three, so the
         * supply is past it rather than on it.
         */
        g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) &
                        ENET_INT_RX_THRESHOLD, ==, ENET_INT_RX_THRESHOLD);
    }

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Arming is a command and the index storage: SGI's driver writes the
 * index without the bit from its interrupt path.
 */
static void test_a_bare_produce_write_leaves_the_timer_armed(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[16] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                          0x08, 0x06, 0xbe, 0xef };
    int attempt;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    sn0_ioc3_rx_one_frame(qts, sv[0], RAM_SCRATCH, RAM_SCRATCH + 0x4000,
                          (4 << 3) | RPIR_ARM, 0);
    qtest_writel(qts, ENET_INT_STATUS, ENET_INT_RX_TIMER);
    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) & ENET_INT_RX_TIMER,
                    ==, 0);

    /* A bare index write, the shape SGI's driver uses to return buffers. */
    qtest_writel(qts, RX_PRODUCE, 8 << 3);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    for (attempt = 0; attempt < 50; attempt++) {
        if (qtest_readl(qts, RX_CONSUME) != 8) {
            break;
        }
        g_usleep(10000);
        qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS / 4);
    }
    g_assert_cmphex(qtest_readl(qts, RX_CONSUME), ==, 16);

    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) & ENET_INT_RX_TIMER,
                    ==, ENET_INT_RX_TIMER);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A source set while masked must raise the line when enabled -- the order
 * every bring-up takes (OpenBSD writes the enable register last). The
 * Bridge's own pin is read live rather than the model's account of it.
 */
static void test_enabling_a_set_source_raises_the_line(const void *data)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = sn0_ioc3_attach_with_netdev(sv[1]);

    /*
     * Interrupts off, then a frame delivered: its source sets and the line
     * must stay low, because nothing is enabled.
     */
    qtest_writel(qts, ENET_INT_ENABLE, 0);
    sn0_ioc3_rx_one_frame(qts, sv[0], RAM_SCRATCH, RAM_SCRATCH + 0x4000,
                          (4 << 3) | RPIR_ARM, 0);

    g_assert_cmphex(qtest_readl(qts, ENET_INT_STATUS) & ENET_INT_RX_TIMER,
                    ==, ENET_INT_RX_TIMER);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_ETH,
                    ==, 0);

    /* Enabling it is the only thing that happens next. */
    qtest_writel(qts, ENET_INT_ENABLE, ENET_INT_RX_TIMER);

    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BRIDGE_PIN_ETH,
                    ==, BRIDGE_PIN_ETH);

    qtest_quit(qts);
    close(sv[0]);
}

static void test_the_media_interface_never_reads_busy(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * Busy must already be clear on the first look after a read is asked
     * for -- asserted on an untouched register it would pass on a model
     * that sets busy and never clears it.
     */
    qtest_writel(qts, ENET_MII_CONTROL, (1u << 10));
    g_assert_cmphex(qtest_readl(qts, ENET_MII_CONTROL) & (1u << 11), ==, 0);

    /* No physical-layer chip answers, and an absent one reads all-ones. */
    g_assert_cmphex(qtest_readl(qts, ENET_MII_DATA_R), ==, 0xffff);
}

/*
 * The buffer RAM offsets the three sizing drivers touch; the halfway pair
 * is the only one that fails on a window aliasing at its midpoint.
 */
#define SSRAM_FIRST       (IOC3_BASE + 0x40000)
#define SSRAM_LINUX_PAIR  (IOC3_BASE + 0x50000)
#define SSRAM_HALFWAY     (IOC3_BASE + 0x60000)
#define SSRAM_LAST        (IOC3_BASE + 0x7fffc)

/* The pattern OpenBSD's driver writes, and the complement it writes second. */
#define SSRAM_PATTERN     0x55aau
#define SSRAM_COMPLEMENT  (0xffffu ^ SSRAM_PATTERN)

static void test_the_buffer_ram_halves_are_distinct(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * OpenBSD's size probe, comparing the second read as a whole word:
     * anything above the data bits sends a driver down the 64 KB branch.
     */
    qtest_writel(qts, SSRAM_FIRST, SSRAM_PATTERN);
    qtest_writel(qts, SSRAM_HALFWAY, SSRAM_COMPLEMENT);

    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==, SSRAM_PATTERN);
    g_assert_cmphex(qtest_readl(qts, SSRAM_HALFWAY), ==, SSRAM_COMPLEMENT);

    /* Mainline Linux's second word, a quarter of the way in, as well. */
    qtest_writel(qts, SSRAM_LINUX_PAIR, SSRAM_COMPLEMENT);
    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==, SSRAM_PATTERN);
    g_assert_cmphex(qtest_readl(qts, SSRAM_LINUX_PAIR), ==, SSRAM_COMPLEMENT);

    /* The window runs to its end and does not wrap onto itself. */
    qtest_writel(qts, SSRAM_LAST, 0x1234);
    g_assert_cmphex(qtest_readl(qts, SSRAM_LAST), ==, 0x1234);
    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==, SSRAM_PATTERN);
}

static void test_the_buffer_ram_is_seventeen_bits_wide(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * Seventeen bits of storage, nothing above. Parity is storage, not
     * computed: no frames move here and a diagnostic write is how software
     * places a parity bit at all.
     */
    qtest_writel(qts, SSRAM_FIRST, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==, 0x0001ffff);

    qtest_writel(qts, SSRAM_FIRST, 0x00010000);
    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==, 0x00010000);
}

/*
 * Bit 17 is the checker's answer, not storage: even parity over the
 * seventeen stored bits. The four words are the firmware diagnostic's own
 * writes, good and bad parity both -- always-set passes the bad pair and
 * fails the diagnostic's first two writes.
 */
#define SSRAM_PARITY_ERROR    0x00020000u
#define MAC_CONTROL_RAM_PARITY 0x00000200u

/* Written by the diagnostic. The first two carry even parity, the last
 * two odd. */
#define SSRAM_GOOD_EVEN_DATA  0x0000aa55u
#define SSRAM_GOOD_ODD_DATA   0x000157aau
#define SSRAM_BAD_EVEN_DATA   0x0000bad1u
#define SSRAM_BAD_ODD_DATA    0x0001bad0u

static void test_the_buffer_ram_reports_bad_parity(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * Checking is off until enabled, asserted absent first -- a bit
     * reported unconditionally lands in OpenBSD's whole-word size probe.
     */
    qtest_writel(qts, SSRAM_FIRST, SSRAM_BAD_EVEN_DATA);
    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==, SSRAM_BAD_EVEN_DATA);

    qtest_writel(qts, MAC_CONTROL, MAC_CONTROL_RAM_PARITY);

    /* Even parity across the seventeen stored bits: no report. */
    qtest_writel(qts, SSRAM_FIRST, SSRAM_GOOD_EVEN_DATA);
    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==, SSRAM_GOOD_EVEN_DATA);

    qtest_writel(qts, SSRAM_FIRST, SSRAM_GOOD_ODD_DATA);
    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==, SSRAM_GOOD_ODD_DATA);

    /* Odd parity: the report, and the stored seventeen bits unchanged. */
    qtest_writel(qts, SSRAM_FIRST, SSRAM_BAD_EVEN_DATA);
    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==,
                    SSRAM_BAD_EVEN_DATA | SSRAM_PARITY_ERROR);

    qtest_writel(qts, SSRAM_FIRST, SSRAM_BAD_ODD_DATA);
    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==,
                    SSRAM_BAD_ODD_DATA | SSRAM_PARITY_ERROR);

    /*
     * The report is derived on every read rather than latched at the write, so
     * clearing the enable takes it away again and the storage underneath it is
     * untouched.
     */
    qtest_writel(qts, MAC_CONTROL, 0);
    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==, SSRAM_BAD_ODD_DATA);
}

static void test_the_buffer_ram_ignores_a_written_parity_report(
    const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * A write to the report is ignored: stored, the checker would agree
     * with the guest instead of the data.
     */
    qtest_writel(qts, MAC_CONTROL, MAC_CONTROL_RAM_PARITY);

    qtest_writel(qts, SSRAM_FIRST, SSRAM_PARITY_ERROR | 0x0101u);
    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==, 0x0101u);
}

static void test_the_size_probe_survives_parity_checking(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * The 64-vs-128 KB probe with checking on, as the firmware runs it: a
     * report in these words puts the driver on the wrong size branch.
     */
    qtest_writel(qts, MAC_CONTROL, MAC_CONTROL_RAM_PARITY);

    qtest_writel(qts, SSRAM_FIRST, SSRAM_PATTERN);
    qtest_writel(qts, SSRAM_HALFWAY, SSRAM_COMPLEMENT);

    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==, SSRAM_PATTERN);
    g_assert_cmphex(qtest_readl(qts, SSRAM_HALFWAY), ==, SSRAM_COMPLEMENT);

    /* And the firmware's own pair, which it writes with checking enabled. */
    qtest_writel(qts, SSRAM_FIRST, 0x5555u);
    qtest_writel(qts, SSRAM_HALFWAY, 0xaaaau);

    g_assert_cmphex(qtest_readl(qts, SSRAM_FIRST), ==, 0x5555u);
    g_assert_cmphex(qtest_readl(qts, SSRAM_HALFWAY), ==, 0xaaaau);
}

/*
 * The byte bus's first device select and the real-time clock behind it. The
 * window is byte-wide, unlike every other block on this chip, and the clock is
 * its top eight bytes.
 */
#define BYTEBUS_DEV0    (IOC3_BASE + 0x80000)
#define RTC_CONTROL     (BYTEBUS_DEV0 + 0x7ff8)
#define RTC_SECONDS     (BYTEBUS_DEV0 + 0x7ff9)
#define RTC_MINUTES     (BYTEBUS_DEV0 + 0x7ffa)
#define RTC_HOURS       (BYTEBUS_DEV0 + 0x7ffb)
#define RTC_WEEKDAY     (BYTEBUS_DEV0 + 0x7ffc)
#define RTC_DAY         (BYTEBUS_DEV0 + 0x7ffd)
#define RTC_MONTH       (BYTEBUS_DEV0 + 0x7ffe)
#define RTC_YEAR        (BYTEBUS_DEV0 + 0x7fff)

#define RTC_CSR_READ    0x40
#define RTC_CSR_WRITE   0x80
#define RTC_CHECKSUM    (BYTEBUS_DEV0 + 0x04bc)
#define RTC_LEAP_CYCLE  (BYTEBUS_DEV0 + 0x3883)

/*
 * A fixed base, mid-minute, so the fields assert exactly and the minute
 * cannot roll underneath the test.
 */
#define RTC_ARGS \
    "-M origin200 -rtc base=2026-08-11T12:30:05,clock=vm"

static void test_the_clock_reads_the_time_it_was_given(const void *data)
{
    QTestState *qts = qtest_init(RTC_ARGS);

    sn0_ioc3_attach(qts);

    /*
     * Freeze, read, release. The year is the assertion no boot can check:
     * two digits from 1970, so 2026 is 0x56 -- calendar digits would answer
     * 0x26 and a guest reports 1996 with nothing else looking wrong.
     */
    qtest_writeb(qts, RTC_CONTROL, RTC_CSR_READ);

    g_assert_cmphex(qtest_readb(qts, RTC_SECONDS), ==, 0x05);
    g_assert_cmphex(qtest_readb(qts, RTC_MINUTES), ==, 0x30);
    g_assert_cmphex(qtest_readb(qts, RTC_HOURS), ==, 0x12);
    g_assert_cmphex(qtest_readb(qts, RTC_DAY), ==, 0x11);
    g_assert_cmphex(qtest_readb(qts, RTC_MONTH), ==, 0x08);
    g_assert_cmphex(qtest_readb(qts, RTC_YEAR), ==, 0x56);

    /*
     * Weekday zero is the part's answer for a day nobody set -- Linux's
     * driver records that the part leaves it alone until written.
     */
    g_assert_cmphex(qtest_readb(qts, RTC_WEEKDAY), ==, 0x00);

    qtest_writeb(qts, RTC_CONTROL, 0);

    qtest_quit(qts);
}

static void test_a_fresh_clock_has_the_date_derived_factory_image(
    const void *data)
{
    QTestState *qts = qtest_init(RTC_ARGS);

    sn0_ioc3_attach(qts);
    g_assert_cmphex(qtest_readb(qts, RTC_LEAP_CYCLE), ==, 1);
    g_assert_cmphex(qtest_readb(qts, RTC_CHECKSUM), ==, 0x6d);
    qtest_quit(qts);

    qts = qtest_init("-M origin200 "
                     "-rtc base=2024-04-11T12:30:05,clock=vm");
    sn0_ioc3_attach(qts);
    g_assert_cmphex(qtest_readb(qts, RTC_LEAP_CYCLE), ==, 2);
    g_assert_cmphex(qtest_readb(qts, RTC_CHECKSUM), ==, 0xad);
    qtest_quit(qts);
}

static void test_the_clock_freeze_holds_an_instant(const void *data)
{
    QTestState *qts = qtest_init(RTC_ARGS);

    sn0_ioc3_attach(qts);

    /*
     * The clock must actually move: three agreeing reads passed with the
     * freeze removed, since nothing advanced between them. Stepping the
     * clock is what makes this a test.
     */
    qtest_writeb(qts, RTC_CONTROL, RTC_CSR_READ);
    g_assert_cmphex(qtest_readb(qts, RTC_SECONDS), ==, 0x05);

    /*
     * Step across a minute boundary, so the fields above the seconds have to
     * hold as well. Checking the seconds alone would pass on a freeze that
     * held one field and let the rest run.
     */
    qtest_clock_step(qts, 62 * 1000000000LL);
    g_assert_cmphex(qtest_readb(qts, RTC_SECONDS), ==, 0x05);
    g_assert_cmphex(qtest_readb(qts, RTC_MINUTES), ==, 0x30);
    g_assert_cmphex(qtest_readb(qts, RTC_HOURS), ==, 0x12);
    g_assert_cmphex(qtest_readb(qts, RTC_DAY), ==, 0x11);
    g_assert_cmphex(qtest_readb(qts, RTC_MONTH), ==, 0x08);
    g_assert_cmphex(qtest_readb(qts, RTC_YEAR), ==, 0x56);

    /* Released, the clock is where the stepped time put it, not where it was. */
    qtest_writeb(qts, RTC_CONTROL, 0);
    g_assert_cmphex(qtest_readb(qts, RTC_SECONDS), ==, 0x07);
    g_assert_cmphex(qtest_readb(qts, RTC_MINUTES), ==, 0x31);

    qtest_quit(qts);
}

static void test_the_clock_year_wraps_below_its_epoch(const void *data)
{
    /*
     * A clock started before its epoch: 1969 is minus one, C's remainder
     * keeps the sign, and to_bcd() of a negative is not BCD. The wrap puts
     * 1969 at 99. Its own machine, because every other clock test starts
     * where the defect is invisible.
     */
    QTestState *qts = qtest_init("-M origin200 "
                                 "-rtc base=1969-12-31T23:59:00,clock=vm");

    sn0_ioc3_attach(qts);

    qtest_writeb(qts, RTC_CONTROL, RTC_CSR_READ);
    g_assert_cmphex(qtest_readb(qts, RTC_YEAR), ==, 0x99);
    g_assert_cmphex(qtest_readb(qts, RTC_MONTH), ==, 0x12);
    g_assert_cmphex(qtest_readb(qts, RTC_DAY), ==, 0x31);
    qtest_writeb(qts, RTC_CONTROL, 0);

    qtest_quit(qts);
}

static void test_the_clock_control_register_reads_back(const void *data)
{
    QTestState *qts = qtest_init(RTC_ARGS);

    sn0_ioc3_attach(qts);

    /*
     * Drivers read-modify-write this register; answering zero has every
     * driver clear the other bit on its way out.
     */
    qtest_writeb(qts, RTC_CONTROL, RTC_CSR_READ);
    g_assert_cmphex(qtest_readb(qts, RTC_CONTROL), ==, RTC_CSR_READ);

    qtest_writeb(qts, RTC_CONTROL, RTC_CSR_READ | RTC_CSR_WRITE);
    g_assert_cmphex(qtest_readb(qts, RTC_CONTROL), ==,
                    RTC_CSR_READ | RTC_CSR_WRITE);

    qtest_writeb(qts, RTC_CONTROL, 0);
    g_assert_cmphex(qtest_readb(qts, RTC_CONTROL), ==, 0);

    qtest_quit(qts);
}

static void test_the_clock_takes_a_time_a_guest_sets(const void *data)
{
    QTestState *qts = qtest_init(RTC_ARGS);

    sn0_ioc3_attach(qts);

    /*
     * Arm, write, release. The year written is 0x40, so reading 0x40 back
     * says the epoch runs the same way in both directions.
     */
    /*
     * Step the virtual clock away from the host's first: a model
     * differencing against the wrong clock then fails every time instead
     * of only on runs where real time crosses a second boundary.
     */
    qtest_clock_step(qts, 5 * 1000000000LL);

    qtest_writeb(qts, RTC_CONTROL, RTC_CSR_WRITE);
    qtest_writeb(qts, RTC_SECONDS, 0x45);
    qtest_writeb(qts, RTC_MINUTES, 0x21);
    qtest_writeb(qts, RTC_HOURS, 0x09);
    qtest_writeb(qts, RTC_WEEKDAY, 0x05);
    qtest_writeb(qts, RTC_DAY, 0x07);
    qtest_writeb(qts, RTC_MONTH, 0x03);
    qtest_writeb(qts, RTC_YEAR, 0x40);
    qtest_writeb(qts, RTC_CONTROL, 0);

    qtest_writeb(qts, RTC_CONTROL, RTC_CSR_READ);
    /*
     * Every field, including the two easiest to drop: seconds, which a
     * staging bug zeroes while the date survives, and the weekday, which
     * nothing on our paths reads back.
     */
    g_assert_cmphex(qtest_readb(qts, RTC_SECONDS), ==, 0x45);
    g_assert_cmphex(qtest_readb(qts, RTC_WEEKDAY), ==, 0x05);
    g_assert_cmphex(qtest_readb(qts, RTC_MINUTES), ==, 0x21);
    g_assert_cmphex(qtest_readb(qts, RTC_HOURS), ==, 0x09);
    g_assert_cmphex(qtest_readb(qts, RTC_DAY), ==, 0x07);
    g_assert_cmphex(qtest_readb(qts, RTC_MONTH), ==, 0x03);
    g_assert_cmphex(qtest_readb(qts, RTC_YEAR), ==, 0x40);
    qtest_writeb(qts, RTC_CONTROL, 0);

    qtest_quit(qts);
}

static void test_the_clock_storage_holds_a_write(const void *data)
{
    QTestState *qts = qtest_init(RTC_ARGS);

    sn0_ioc3_attach(qts);

    /*
     * The cells below the clock are real storage: SGI's driver probes the
     * part type through them, and zero answers identify a Dallas part with
     * a register set this device does not have.
     */
    qtest_writeb(qts, BYTEBUS_DEV0 + 0x0b, 0x00);
    qtest_writeb(qts, BYTEBUS_DEV0 + 0x06, 0xff);
    qtest_writeb(qts, BYTEBUS_DEV0 + 0x0b, 0x80);
    g_assert_cmphex(qtest_readb(qts, BYTEBUS_DEV0 + 0x06), ==, 0xff);

    /* The ends of the storage, so a window that stops short is caught here. */
    qtest_writeb(qts, BYTEBUS_DEV0 + 0x0000, 0x5a);
    qtest_writeb(qts, BYTEBUS_DEV0 + 0x7ff7, 0xa5);
    g_assert_cmphex(qtest_readb(qts, BYTEBUS_DEV0 + 0x0000), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, BYTEBUS_DEV0 + 0x7ff7), ==, 0xa5);

    /*
     * Consecutive cells are distinct, and the storage does not alias the clock
     * above it: the byte below the control register keeps its own value while
     * the control register holds another.
     */
    qtest_writeb(qts, RTC_CONTROL, RTC_CSR_READ);
    g_assert_cmphex(qtest_readb(qts, BYTEBUS_DEV0 + 0x7ff7), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, RTC_CONTROL), ==, RTC_CSR_READ);
    qtest_writeb(qts, RTC_CONTROL, 0);

    qtest_quit(qts);
}

/*
 * Battery power outlasts a process: given a drive, storage must read back
 * across a second, independent QEMU process.
 */
static void test_the_storage_persists_across_a_process_with_a_drive(
    const void *data)
{
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn0-ioc3-nvram-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    /* The device requires an exact match to its own storage size, 32 KB. */
    g_assert_cmpint(ftruncate(fd, 0x8000), ==, 0);
    close(fd);

    args = g_strdup_printf("%s -drive if=none,id=sgi-nvram0,file=%s,"
                           "format=raw", RTC_ARGS, path);

    qts = qtest_init(args);
    sn0_ioc3_attach(qts);
    /* Supplying an all-zero image is data, not permission to provision it. */
    g_assert_cmphex(qtest_readb(qts, RTC_LEAP_CYCLE), ==, 0);
    g_assert_cmphex(qtest_readb(qts, RTC_CHECKSUM), ==, 0);
    qtest_writeb(qts, BYTEBUS_DEV0 + 0x0100, 0xc3);
    qtest_quit(qts);

    /*
     * A second process, not a reset inside the first -- the property this
     * test exists to check.
     */
    qts = qtest_init(args);
    sn0_ioc3_attach(qts);
    g_assert_cmphex(qtest_readb(qts, BYTEBUS_DEV0 + 0x0100), ==, 0xc3);
    qtest_quit(qts);

    unlink(path);
}

/*
 * Battery-backed: firmware initializes storage, warm-resets, and expects
 * its records on the next pass. What a reset must clear is only an
 * in-flight transaction, or the next guest inherits a stopped clock.
 */
static void test_the_storage_survives_a_system_reset(const void *data)
{
    QTestState *qts = qtest_init(RTC_ARGS);

    sn0_ioc3_attach(qts);

    qtest_writeb(qts, BYTEBUS_DEV0 + 0x0100, 0xc3);
    qtest_writeb(qts, BYTEBUS_DEV0 + 0x7ff7, 0x3c);
    qtest_writeb(qts, RTC_CONTROL, RTC_CSR_READ);

    qtest_system_reset(qts);
    /*
     * The reset takes the bridge's window aim and the BAR with it, so the
     * window is re-aimed the same way a fresh guest would; what is being
     * asserted is the cells behind it, not the aim.
     */
    sn0_ioc3_attach(qts);

    g_assert_cmphex(qtest_readb(qts, BYTEBUS_DEV0 + 0x0100), ==, 0xc3);
    g_assert_cmphex(qtest_readb(qts, BYTEBUS_DEV0 + 0x7ff7), ==, 0x3c);
    g_assert_cmphex(qtest_readb(qts, RTC_CONTROL)
                    & (RTC_CSR_READ | RTC_CSR_WRITE), ==, 0);

    qtest_quit(qts);
}

/* A Search ROM at whichever node's MicroLAN engine the caller names. */
static uint64_t onewire_search_rom_node(QTestState *qts, uint64_t node)
{
    uint64_t rom = 0;
    unsigned int i;

    g_assert_false(onewire_slot_node(qts, node, 520, 65)); /* reset: present */

    for (i = 0; i < 8; i++) {                              /* Search ROM 0xf0 */
        bool one = (0xf0u >> i) & 1;

        onewire_slot_node(qts, node, one ? 6 : 80, one ? 13 : 30);
    }
    for (i = 0; i < 64; i++) {
        bool bit = onewire_slot_node(qts, node, 6, 13);
        bool complement = onewire_slot_node(qts, node, 6, 13);

        g_assert_cmpint(complement, ==, !bit);
        if (bit) {
            rom |= 1ULL << i;
        }
        onewire_slot_node(qts, node, bit ? 6 : 80, bit ? 13 : 30);
    }
    return rom;
}

/*
 * Two boards with one serial merge two nodes in the firmware's eyes; the
 * parts must differ while each stays well-formed.
 */
static void test_each_node_has_its_own_onewire_identity(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200,topology=origin200-dual -m 128 "
        "-smp 4");
    uint64_t node0, node1;
    uint8_t bytes[8];
    unsigned int i;

    sn0_program_nasid(qts, 1);
    sn0_ioc3_attach_node(qts, 0);
    sn0_ioc3_attach_node(qts, NODE_STRIDE);
    /* Each node's engine needs routing onto its pin before it answers. */
    qtest_writel(qts, PIN_CONTROL_SET, PIN_MICROLAN_ENABLE);
    qtest_writel(qts, NODE_STRIDE + PIN_CONTROL_SET, PIN_MICROLAN_ENABLE);

    node0 = onewire_search_rom_node(qts, 0);
    node1 = onewire_search_rom_node(qts, NODE_STRIDE);

    /* Each is a well-formed part before they are compared: right family, the
     * required serial prefix, and a ROM CRC that closes over all eight bytes.
     */
    g_assert_cmphex(node0 & 0xff, ==, 0x91);            /* family */
    g_assert_cmphex(node1 & 0xff, ==, 0x91);
    g_assert_cmphex((node0 >> 44) & 0xfff, ==, 0x5e7);  /* serial prefix */
    g_assert_cmphex((node1 >> 44) & 0xfff, ==, 0x5e7);
    for (i = 0; i < 8; i++) {
        bytes[i] = (node0 >> (8 * i)) & 0xff;
    }
    g_assert_cmphex(onewire_crc8(0, bytes, 8), ==, 0);
    for (i = 0; i < 8; i++) {
        bytes[i] = (node1 >> (8 * i)) & 0xff;
    }
    g_assert_cmphex(onewire_crc8(0, bytes, 8), ==, 0);

    /* The point of the test: the two boards are not the same board. */
    g_assert_cmphex(node0, !=, node1);

    qtest_quit(qts);
}

/*
 * Two nodes, two timekeepers, no aliasing: one part behind both windows
 * shows phantom records on the other node while every single-node test
 * passes.
 */
static void test_each_node_has_its_own_storage(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200,topology=origin200-dual -m 128 "
        "-smp 4");

    sn0_program_nasid(qts, 1);
    sn0_ioc3_attach_node(qts, 0);
    sn0_ioc3_attach_node(qts, NODE_STRIDE);

    /* A write through node 0's window must not appear in node 1's. */
    qtest_writeb(qts, BYTEBUS_DEV0 + 0x0123, 0x5a);
    g_assert_cmphex(qtest_readb(qts, NODE_STRIDE + BYTEBUS_DEV0 + 0x0123),
                    ==, 0x00);

    /* And each keeps its own value once both are written. */
    qtest_writeb(qts, NODE_STRIDE + BYTEBUS_DEV0 + 0x0123, 0xa5);
    g_assert_cmphex(qtest_readb(qts, BYTEBUS_DEV0 + 0x0123), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, NODE_STRIDE + BYTEBUS_DEV0 + 0x0123),
                    ==, 0xa5);

    qtest_quit(qts);
}


static void test_the_transceiver_answers_at_one_address(const void *data)
{
    unsigned int phy, answered = 0;

    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * Every address: both drivers stop at the first that answers, so a
     * model answering everywhere is found at address zero and no boot
     * notices. The count is the assertion.
     */
    for (phy = 0; phy < 32; phy++) {
        if (phy_read(qts, phy, 2) != 0xffff) {
            g_assert_cmpuint(phy, ==, PHY_ADDRESS);
            answered++;
        }
    }

    g_assert_cmpuint(answered, ==, 1);
}

static void test_the_transceiver_identity(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * The exact words: an identity passing the cruder scan still fails the
     * table match that decides which driver binds.
     */
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 2), ==, 0x2000);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 3), ==, 0x5c01);
}

static void test_the_transceiver_reset_is_self_clearing(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * A driver writes reset and then polls for the bit to clear. Left set, it
     * holds the attach in a loop until the timeout expires.
     */
    phy_write(qts, PHY_ADDRESS, 0, 0x8000);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 0) & 0x8000, ==, 0);
}

static void test_the_transceiver_reports_a_usable_link(const void *data)
{
    uint16_t status, advertise, partner;

    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * The driver's whole route to a media type, asserted together: each
     * step alone can pass with the guest reporting no media.
     */
    phy_write(qts, PHY_ADDRESS, 0, 0x1000);   /* enable autonegotiation */

    /* Read twice, as the driver does, and assert the union has a link. */
    status = phy_read(qts, PHY_ADDRESS, 1) | phy_read(qts, PHY_ADDRESS, 1);
    g_assert_cmphex(status & 0x0004, ==, 0x0004);   /* link */
    g_assert_cmphex(status & 0x0020, ==, 0x0020);   /* autoneg complete */

    /* The far end can autonegotiate, which is the route the driver trusts. */
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 6) & 0x0001, ==, 0x0001);

    /*
     * And the two advertisements have a full-duplex hundred-megabit link in
     * common. The driver intersects them and reads the media out of the
     * result, so an empty intersection is a link with no usable media on it.
     */
    advertise = phy_read(qts, PHY_ADDRESS, 4);
    partner = phy_read(qts, PHY_ADDRESS, 5);
    g_assert_cmphex(advertise & partner & 0x0100, ==, 0x0100);
}

static void test_the_link_settles_when_polled(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * phylib never writes the control register -- the link must come up
     * unprompted. The first read finds it settling, the bench state the
     * reset-value diagnostic depends on, and arms the settle.
     */
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 1) & 0x0024, ==, 0x0000);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 5), ==, 0x0000);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 6) & 0x0001, ==, 0x0000);

    /* Wait past the settle, as a polling driver does. */
    qtest_clock_step(qts, 2500 * 1000000LL);

    /*
     * Now the link is up: status reports link and completed autonegotiation,
     * and the partner and expansion registers answer, all without a control
     * register write having happened.
     */
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 1) & 0x0024, ==, 0x0024);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 6) & 0x0001, ==, 0x0001);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 5) & 0x0100, ==, 0x0100);
}

static void test_the_transceiver_configuration_register(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /*
     * Read-modify-written once on a media change and never read back for a
     * decision, so storage is the whole contract -- and the only place it is
     * checked at all, since no boot depends on it.
     */
    phy_write(qts, PHY_ADDRESS, 0x17, 0x0526);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 0x17), ==, 0x0526);

    /* Its low bits report the transceiver's own address. */
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 0x19) & 0x1f, ==, PHY_ADDRESS);
}

/*
 * The reset values SGI's transceiver test compares, which are also the
 * public DP83840A datasheet's defaults -- two independent sources.
 * Asserted through the diagnostic's masks: bits outside them are
 * don't-care and nothing has checked them.
 */
static void test_the_transceiver_reset_values(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    /* Control: a hundred megabits, full duplex, autonegotiating. */
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 0x00) & 0xff80, ==, 0x3100);

    /* Configuration: NRZI coding on, and the good-link override off. */
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 0x17) & 0xb8d6, ==, 0x8040);

    /* Ten-megabit config: link pulses, heartbeat, twisted pair, jabber. */
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 0x1c) & 0x003d, ==, 0x0039);

    /*
     * Read before any control write, as the diagnostic reads them: no
     * link, no negotiation, no partner -- a bench with no cable. A model
     * with the link already live answered these wrong.
     */
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 0x01) & 0xf83f, ==, 0x7809);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 0x05) & 0xe3ff, ==, 0x0000);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 0x06) & 0x001f, ==, 0x0000);
}

/*
 * Only the firmware's transceiver test reaches this register, so this is
 * the only place its contract is checked.
 */
static void test_the_transceiver_loopback_register(const void *data)
{
    ioc3_reset();

    sn0_ioc3_attach(qts);

    phy_write(qts, PHY_ADDRESS, 0x18, 0x0b00);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 0x18) & 0x0b00, ==, 0x0b00);

    /*
     * And the clear, which the diagnostic issues next. A model answering with
     * a constant that happened to carry these bits would pass the write above
     * and fail here, and the firmware's failure line names only the set.
     */
    phy_write(qts, PHY_ADDRESS, 0x18, 0x0000);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 0x18) & 0x0b00, ==, 0x0000);
}

static void test_pci_status_reset_value(const void *data)
{
    ioc3_reset();

    /*
     * IRIX compares this dword whole and fails the slot on a zero Status
     * half; only Status is asserted -- the board pre-enables Command and
     * the miniroot clears it first.
     */
    g_assert_cmphex(qtest_readl(qts, CFG0(PCI_IOC3_DEVICE) + PCI_COMMAND) >> 16,
                    ==, 0x0280u);

    /*
     * Capability bits read-only, error latches W1C from zero: all-ones
     * leaves Status exactly 0x0280.
     */
    qtest_writel(qts, CFG0(PCI_IOC3_DEVICE) + PCI_COMMAND, 0xffffffffu);
    g_assert_cmphex(qtest_readl(qts, CFG0(PCI_IOC3_DEVICE) + PCI_COMMAND) >> 16,
                    ==, 0x0280u);
}

/*
 * The board this chip is fitted to, which a guest learns from the subsystem
 * dword at config offset 0x2c. Mainline Linux's IOC3 driver dispatches on it
 * and registers a different set of sub-devices for each board
 * (drivers/mfd/ioc3.c, v7.2-rc7, commit db2ddb8), so the identifier is the
 * whole of what selects a population.
 */
#define PCI_SUBSYSTEM    0x2c
#define SUBSYSTEM_SGI    0x10a9u  /* the vendor half, the same on every board */
#define SUBSYSTEM(id)    (((uint32_t)(id) << 16) | SUBSYSTEM_SGI)

static void test_the_default_board_is_the_system_board(const void *data)
{
    ioc3_reset();

    /*
     * Asserted as the whole dword rather than its top half: the driver
     * matches all 32 bits, so a model with the vendor half wrong falls
     * through to the catch-all population with nothing else looking amiss.
     */
    g_assert_cmphex(qtest_readl(qts, CFG0(PCI_IOC3_DEVICE) + PCI_SUBSYSTEM),
                    ==, SUBSYSTEM(0xc303));
}

static void test_bare_device_keeps_system_board_default(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 "
                                 "-device sgi-ioc3,addr=3");

    g_assert_cmphex(qtest_readl(qts, CFG0(3) + PCI_SUBSYSTEM), ==,
                    SUBSYSTEM(0xc303));

    qtest_quit(qts);
}

static void test_the_board_is_a_property(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 "
                                 "-device sgi-ioc3,id=test-ioc3,addr=3,"
                                 "subsystem-id=0xc305");

    g_assert_cmphex(qtest_readl(qts, CFG0(TEST_IOC3_DEVICE) + PCI_SUBSYSTEM),
                    ==, SUBSYSTEM(0xc305));

    qtest_quit(qts);
}

/*
 * What a board fits, as the bus can see it. Each probe writes a register the
 * sub-device stores in and reads it back: a window with nothing behind it
 * swallows the write and reads zero, so one probe answers both ways round and
 * neither answer is a bare "reads zero", which half the live registers here
 * also do.
 */
static bool serial_port_is_fitted(QTestState *qts, uint64_t port)
{
    qtest_writeb(qts, port + REG_SCR, 0x5a);
    return qtest_readb(qts, port + REG_SCR) == 0x5a;
}

static bool ps2_is_fitted(QTestState *qts, uint64_t base)
{
    qtest_writel(qts, base + 0x09c, 0xffffffff);
    return qtest_readl(qts, base + 0x09c) ==
           (0x0000ffff | KBC_K_CLAMP_1 | KBC_M_CLAMP_1 |
            KBC_K_CLAMP_3 | KBC_M_CLAMP_3);
}

static bool timekeeper_is_fitted(QTestState *qts, uint64_t base)
{
    qtest_writeb(qts, base + 0x80100, 0xc3);
    return qtest_readb(qts, base + 0x80100) == 0xc3;
}

static bool ethernet_is_fitted(QTestState *qts, uint64_t base)
{
    qtest_writel(qts, base + 0x0f8, 0x00010001);
    return qtest_readl(qts, base + 0x0f8) == 0x00010001;
}

/*
 * The station-address part on the one-wire bus. Its presence can be selected
 * independently, but it takes its station address from the embedded ethernet
 * device, so a valid population always fits ethernet with it. A fitted part
 * answers a reset slot by pulling the wire low; an empty bus reads high.
 */
static bool one_wire_part_is_fitted(QTestState *qts, uint64_t base)
{
    qtest_writel(qts, base + 0x34, PIN_MICROLAN_ENABLE);
    qtest_writel(qts, base + 0x30, MICROLAN_RESET_SLOT);
    return (qtest_readl(qts, base + 0x30) & MICROLAN_LEVEL) == 0;
}

static bool ioc3_has_child(QTestState *qts, const char *path,
                           const char *name)
{
    QDict *response = qtest_qmp(qts, "{ 'execute': 'qom-list',"
                                     " 'arguments': { 'path': %s } }",
                                     path);
    QList *children;
    const QListEntry *entry;
    bool found = false;

    g_assert_true(qdict_haskey(response, "return"));
    children = qdict_get_qlist(response, "return");
    for (entry = qlist_first(children); entry; entry = qlist_next(entry)) {
        QDict *child = qobject_to(QDict, qlist_entry_obj(entry));
        const char *type = qdict_get_try_str(child, "type");

        if (type && g_str_has_prefix(type, "child<") &&
            g_str_equal(qdict_get_str(child, "name"), name)) {
            found = true;
        }
    }

    qobject_unref(response);
    return found;
}

static void assert_ioc3_population(QTestState *qts, bool eth, bool serial_a,
                                   bool serial_b, bool kbc, bool parallel,
                                   bool onewire, const char *timekeeper)
{
    const char *path = "/machine/peripheral/test-ioc3";

    g_assert_cmpint(ioc3_has_child(qts, path, "eth"), ==, eth);
    g_assert_cmpint(ioc3_has_child(qts, path, "serial[0]"), ==, serial_a);
    g_assert_cmpint(ioc3_has_child(qts, path, "serial[1]"), ==, serial_b);
    g_assert_cmpint(ioc3_has_child(qts, path, "kbc"), ==, kbc);
    g_assert_cmpint(ioc3_has_child(qts, path, "parallel"), ==, parallel);
    g_assert_cmpint(ioc3_has_child(qts, path, "onewire-part"), ==, onewire);
    g_assert_cmpint(ioc3_has_child(qts, path, "rtc"), ==,
                    g_str_equal(timekeeper, "rtc"));
    g_assert_cmpint(ioc3_has_child(qts, path, "ds1685"), ==,
                    g_str_equal(timekeeper, "ds1685"));
}

/*
 * The semantic population is instance data.  Keep the MENET4 identity while
 * asking for a population that its subsystem lookup would never select.
 */
static void test_an_explicit_population_is_independent_of_identity(
    const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 "
        "-device sgi-ioc3,id=test-ioc3,addr=3,subsystem-id=0xc306,"
        "population=0x23,timekeeper-kind=1");

    test_ioc3_attach(qts);
    g_assert_cmphex(qtest_readl(qts, CFG0(TEST_IOC3_DEVICE) + PCI_SUBSYSTEM),
                    ==, SUBSYSTEM(0xc306));
    assert_ioc3_population(qts, true, true, false, false, false, true,
                           "rtc");

    qtest_quit(qts);
}

static void test_an_explicit_population_selects_the_other_children(
    const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 "
        "-device sgi-ioc3,id=test-ioc3,addr=3,subsystem-id=0xc306,"
        "population=0x1c,timekeeper-kind=2");

    assert_ioc3_population(qts, false, false, true, true, true, false,
                           "ds1685");
    qtest_quit(qts);
}

static void assert_qemu_rejects(const char *const args[], const char *message)
{
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_nonnull(strstr(error_output, message));
}

static void test_population_rejects_onewire_without_ethernet(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-display", "none",
        "-device", "sgi-ioc3,addr=3,population=0x20",
        "-qtest", "stdio", "-accel", "qtest", NULL
    };

    assert_qemu_rejects(args,
                        "IOC3 one-wire identity requires ethernet");
}

static void test_population_rejects_unknown_bits(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-display", "none",
        "-device", "sgi-ioc3,addr=3,population=0x40",
        "-qtest", "stdio", "-accel", "qtest", NULL
    };

    assert_qemu_rejects(args, "IOC3 population has unknown bits 0x40");
}

static void test_population_rejects_an_unknown_timekeeper(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-display", "none",
        "-device", "sgi-ioc3,addr=3,population=0,timekeeper-kind=3",
        "-qtest", "stdio", "-accel", "qtest", NULL
    };

    assert_qemu_rejects(args, "IOC3 timekeeper kind 3 is unsupported");
}

static void test_auto_population_rejects_a_timekeeper_override(
    const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-display", "none",
        "-device", "sgi-ioc3,addr=3,timekeeper-kind=1",
        "-qtest", "stdio", "-accel", "qtest", NULL
    };

    assert_qemu_rejects(
        args, "IOC3 timekeeper kind 1 requires an explicit population");
}

static void test_auto_population_validates_the_timekeeper_kind(
    const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-display", "none",
        "-device", "sgi-ioc3,addr=3,timekeeper-kind=3",
        "-qtest", "stdio", "-accel", "qtest", NULL
    };

    assert_qemu_rejects(args, "IOC3 timekeeper kind 3 is unsupported");
}

typedef struct BoardPopulation {
    const char *name;
    uint16_t subsystem_id;
    bool eth;           /* the ethernet block, and the one-wire part with it */
    bool serial_a;
    bool serial_b;
    bool ps2;
    bool timekeeper;    /* the M48T35 at byte-bus device 0 */
} BoardPopulation;

/*
 * Every board mainline Linux names, and the catch-all it uses for the rest
 * (drivers/mfd/ioc3.c, v7.2-rc7, commit db2ddb8). The Octane's own clock is a
 * DS1685 at a different pair of byte-bus selects, not the M48T35 modelled
 * here, so its timekeeper column is false: what the model must not do is
 * answer that window with the wrong part.
 */
static const BoardPopulation ioc3_boards[] = {
    { "ip27-baseio6g", 0xc300, true,  true,  true,  true,  true  },
    { "ip27-mio",      0xc301, false, true,  true,  true,  false },
    { "ip27-baseio",   0xc302, true,  true,  true,  false, true  },
    { "ip29-sysboard", 0xc303, true,  true,  true,  true,  true  },
    { "ip30-sysboard", 0xc304, true,  true,  true,  true,  false },
    { "menet",         0xc305, true,  true,  true,  false, false },
    { "menet4",        0xc306, true,  false, false, false, false },
    /*
     * Two unmatched identifiers, because the fall-through is the contract
     * rather than a row: 0xc307 is a real later SGI board the driver has no
     * entry for, and 0xbeef is nobody's.
     */
    { "cad-duo-io7",   0xc307, true,  false, false, true,  false },
    { "cad-duo-other", 0xbeef, true,  false, false, true,  false },
};

static void test_a_board_fits_its_own_sub_devices(const void *data)
{
    const BoardPopulation *board = data;
    g_autofree char *args =
        g_strdup_printf("-M origin200 "
                        "-device sgi-ioc3,id=test-ioc3,addr=3,"
                        "subsystem-id=0x%04x",
                        board->subsystem_id);
    QTestState *qts = qtest_init(args);

    test_ioc3_attach(qts);

    /* The identifier a guest dispatches on, before what it selects. */
    g_assert_cmphex(qtest_readl(qts, CFG0(TEST_IOC3_DEVICE) + PCI_SUBSYSTEM),
                    ==, SUBSYSTEM(board->subsystem_id));

    g_assert_cmpint(ethernet_is_fitted(qts, TEST_IOC3_BASE), ==, board->eth);
    g_assert_cmpint(one_wire_part_is_fitted(qts, TEST_IOC3_BASE), ==,
                    board->eth);
    g_assert_cmpint(serial_port_is_fitted(qts, TEST_IOC3_BASE + 0x020178), ==,
                    board->serial_a);
    g_assert_cmpint(serial_port_is_fitted(qts, TEST_IOC3_BASE + 0x020170), ==,
                    board->serial_b);
    g_assert_cmpint(ps2_is_fitted(qts, TEST_IOC3_BASE), ==, board->ps2);
    g_assert_cmpint(timekeeper_is_fitted(qts, TEST_IOC3_BASE), ==,
                    board->timekeeper);

    qtest_quit(qts);
}

/*
 * TEST_IOC3_DEVICE above is a synthetic slot: the Origin 200's real PCI
 * backplane carries option slots 5 through 7 only (docs/machines/
 * origin200.md), with devices 0-2 fixed to the two onboard SCSI adapters
 * and the system-board IOC3. This group instead attaches at device 6, a
 * real free option slot, to exercise the add-in card this project's own
 * datasheet documents (docs/datasheets/sgi-cad-duo-pci-card.md) the way it
 * is actually meant to be used: alongside the native IOC3 the machine
 * always builds, not in place of it.
 */
#define ADDIN_IOC3_DEVICE   6
#define BRIDGE_DEVICE6      0xa00000  /* device 2's window plus 4 x 1 MB */
#define ADDIN_IOC3_BASE     (SPACE_XTALK + \
                            ((uint64_t)CONSOLE_WIDGET << 24) + BRIDGE_DEVICE6)
#define ADDIN_IOC3_PCI_MMIO 0x1c000000u

#define ADDIN_KBC_KBD_RECEIVE  (ADDIN_IOC3_BASE + 0x0a0)
#define ADDIN_KBC_AUX_RECEIVE  (ADDIN_IOC3_BASE + 0x0a4)
#define ADDIN_KBC_KBD_TRANSMIT (ADDIN_IOC3_BASE + 0x0a8)
#define ADDIN_KBC_AUX_TRANSMIT (ADDIN_IOC3_BASE + 0x0ac)

/*
 * 0xffff is SGI_IOC3_SUBSYS_CAD_DUO_PCI_CARD_UNCAPTURED
 * (include/hw/misc/sgi-ioc3.h): the real card's own subsystem identity has
 * never been captured, so this is the explicit "not observed" placeholder
 * rather than a guessed value or the class's IP29 System Board default,
 * which would misrepresent the add-in card as a second system board.
 */
#define ADDIN_CARD_DEVICE_ARG \
    "-device sgi-ioc3,id=cad-duo,addr=" G_STRINGIFY(ADDIN_IOC3_DEVICE) \
    ",subsystem-id=0xffff"

static void addin_ioc3_attach(QTestState *qts)
{
    qtest_writel(qts, CFG0(ADDIN_IOC3_DEVICE) + PCI_BAR0, ADDIN_IOC3_PCI_MMIO);
    qtest_writel(qts, CFG0(ADDIN_IOC3_DEVICE) + PCI_COMMAND,
                PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(ADDIN_IOC3_DEVICE),
                DEV_MEMORY_SPACE | (ADDIN_IOC3_PCI_MMIO >> 20));
}

static void test_the_addin_card_answers_at_a_free_option_slot(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 " ADDIN_CARD_DEVICE_ARG);

    addin_ioc3_attach(qts);

    g_assert_cmphex(qtest_readl(qts, CFG0(ADDIN_IOC3_DEVICE) + PCI_SUBSYSTEM),
                    ==, SUBSYSTEM(0xffff));

    /* The CAD DUO population: Ethernet, one-wire and PS/2, nothing else. */
    g_assert_cmpint(ethernet_is_fitted(qts, ADDIN_IOC3_BASE), ==, true);
    g_assert_cmpint(one_wire_part_is_fitted(qts, ADDIN_IOC3_BASE), ==, true);
    g_assert_cmpint(ps2_is_fitted(qts, ADDIN_IOC3_BASE), ==, true);
    g_assert_cmpint(serial_port_is_fitted(qts, ADDIN_IOC3_BASE + 0x020178),
                    ==, false);
    g_assert_cmpint(serial_port_is_fitted(qts, ADDIN_IOC3_BASE + 0x020170),
                    ==, false);
    g_assert_cmpint(timekeeper_is_fitted(qts, ADDIN_IOC3_BASE), ==, false);

    qtest_quit(qts);
}

/*
 * The point of an add-in keyboard/mouse card: a guest can have the native
 * system-board keyboard/mouse and this card's own keyboard/mouse at once,
 * each its own independent PS/2 line reachable at its own PCI slot. Round
 * trips a port on each chip in the same machine and checks neither leaks
 * into the other's receive register.
 */
static void test_the_addin_card_ps2_ports_are_independent_of_the_native_ones(
    const void *data)
{
    QTestState *qts = qtest_init("-M origin200 " ADDIN_CARD_DEVICE_ARG);

    sn0_ioc3_attach(qts);
    addin_ioc3_attach(qts);

    qtest_writel(qts, KBC_KBD_TRANSMIT, 0xff);
    qtest_writel(qts, ADDIN_KBC_AUX_TRANSMIT, 0xff);

    /* The native board's own keyboard reset reply, untouched by the card. */
    g_assert_cmphex(qtest_readl(qts, KBC_KBD_RECEIVE), ==, 0xc0faaa00);
    /* The add-in card's own mouse reset reply, its own three bytes. */
    g_assert_cmphex(qtest_readl(qts, ADDIN_KBC_AUX_RECEIVE), ==, 0xe8faaa00);

    /* Neither chip's other port saw a byte that belongs to the other chip. */
    g_assert_cmphex(qtest_readl(qts, KBC_AUX_RECEIVE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, ADDIN_KBC_KBD_RECEIVE), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    unsigned int i;
    int ret;

    g_test_init(&argc, &argv, NULL);

    qts = qtest_init("-M origin200");

    for (i = 0; i < ARRAY_SIZE(ioc3_boards); i++) {
        g_autofree char *path = g_strdup_printf("sn0/ioc3/board-%s",
                                                ioc3_boards[i].name);

        qtest_add_data_func(path, &ioc3_boards[i],
                            test_a_board_fits_its_own_sub_devices);
    }

    qtest_add_data_func(
        "sn0/ioc3/addin-card-answers-at-a-free-option-slot", NULL,
        test_the_addin_card_answers_at_a_free_option_slot);
    qtest_add_data_func(
        "sn0/ioc3/addin-card-ps2-ports-are-independent-of-the-native-ones",
        NULL, test_the_addin_card_ps2_ports_are_independent_of_the_native_ones);

    qtest_add_data_func("sn0/ioc3/pci-status-reset-value", NULL,
                        test_pci_status_reset_value);
    qtest_add_data_func("sn0/ioc3/default-board-is-the-system-board", NULL,
                        test_the_default_board_is_the_system_board);
    qtest_add_data_func("sn0/ioc3/bare-added-device-keeps-default", NULL,
                        test_bare_device_keeps_system_board_default);
    qtest_add_data_func("sn0/ioc3/board-is-a-property", NULL,
                        test_the_board_is_a_property);
    qtest_add_data_func("sn0/ioc3/population-is-instance-data", NULL,
                        test_an_explicit_population_is_independent_of_identity);
    qtest_add_data_func("sn0/ioc3/population-selects-other-children", NULL,
                        test_an_explicit_population_selects_the_other_children);
    qtest_add_data_func("sn0/ioc3/population-rejects-onewire-without-ethernet",
                        NULL,
                        test_population_rejects_onewire_without_ethernet);
    qtest_add_data_func("sn0/ioc3/population-rejects-unknown-bits", NULL,
                        test_population_rejects_unknown_bits);
    qtest_add_data_func("sn0/ioc3/population-rejects-unknown-timekeeper", NULL,
                        test_population_rejects_an_unknown_timekeeper);
    qtest_add_data_func("sn0/ioc3/auto-population-rejects-timekeeper-override",
                        NULL,
                        test_auto_population_rejects_a_timekeeper_override);
    qtest_add_data_func("sn0/ioc3/auto-population-validates-timekeeper-kind",
                        NULL,
                        test_auto_population_validates_the_timekeeper_kind);
    qtest_add_data_func("sn0/ioc3/phy-answers-at-one-address", NULL,
                        test_the_transceiver_answers_at_one_address);
    qtest_add_data_func("sn0/ioc3/phy-identity", NULL,
                        test_the_transceiver_identity);
    qtest_add_data_func("sn0/ioc3/phy-reset-self-clearing", NULL,
                        test_the_transceiver_reset_is_self_clearing);
    qtest_add_data_func("sn0/ioc3/phy-reports-a-usable-link", NULL,
                        test_the_transceiver_reports_a_usable_link);
    qtest_add_data_func("sn0/ioc3/phy-link-settles-when-polled", NULL,
                        test_the_link_settles_when_polled);
    qtest_add_data_func("sn0/ioc3/phy-configuration-register", NULL,
                        test_the_transceiver_configuration_register);
    qtest_add_data_func("sn0/ioc3/phy-reset-values", NULL,
                        test_the_transceiver_reset_values);
    qtest_add_data_func("sn0/ioc3/phy-loopback-register", NULL,
                        test_the_transceiver_loopback_register);

    qtest_add_data_func("sn0/ioc3/ps2-ports-read-empty", NULL,
                        test_the_ps2_ports_read_empty);
    qtest_add_data_func("sn0/ioc3/ps2-keyboard-round-trip", NULL,
                        test_the_ps2_keyboard_round_trips);
    qtest_add_data_func("sn0/ioc3/ps2-mouse-round-trip", NULL,
                        test_the_ps2_mouse_round_trips);
    qtest_add_data_func("sn0/ioc3/ps2-keyboard-acks-set-key-type", NULL,
                        test_the_ps2_keyboard_acks_set_key_type);
    qtest_add_data_func("sn0/ioc3/ps2-receive-drains-on-read", NULL,
                        test_the_ps2_receive_drains_on_read);
    qtest_add_data_func("sn0/ioc3/ps2-interrupt-latches", NULL,
                        test_the_ps2_interrupt_latches_until_acknowledged);
    qtest_add_data_func("sn0/ioc3/ps2-control-masks-writes", NULL,
                        test_the_ps2_control_masks_writes);
    qtest_add_data_func("sn0/ioc3/ps2-receive-holds-three-behind-the-clamp",
                        NULL,
                        test_the_ps2_receive_holds_three_behind_the_clamp);
    qtest_add_data_func("sn0/ioc3/ps2-backlog-survives-an-early-acknowledge",
                        NULL,
                        test_the_ps2_backlog_survives_an_early_acknowledge);
    qtest_add_data_func("sn0/ioc3/ps2-drain-then-acknowledge-takes-one",
                        NULL,
                        test_the_ps2_drain_then_acknowledge_takes_one);
    qtest_add_data_func("sn0/ioc3/ps2-receive-overflows-without-a-clamp", NULL,
                        test_the_ps2_receive_overflows_without_a_clamp);
    qtest_add_data_func("sn0/ioc3/ps2-clamp-after-one-splits-a-reply", NULL,
                        test_the_ps2_clamp_after_one_splits_a_reply);
    qtest_add_data_func("sn0/ioc3/ps2-backlog-survives-a-migration", NULL,
                        test_the_ps2_backlog_survives_a_migration);
    qtest_add_data_func("sn0/ioc3/serial-dma-indices-take-a-write", NULL,
                        test_the_serial_dma_indices_take_a_write);
    qtest_add_data_func("sn0/ioc3/serial-dma-ports-are-distinct", NULL,
                        test_the_serial_dma_ports_are_distinct);
    qtest_add_data_func("sn0/ioc3/serial-dma-shadow-is-read-only", NULL,
                        test_the_serial_dma_shadow_is_read_only);
    qtest_add_data_func("sn0/ioc3/ethernet-interrupt-acknowledges", NULL,
                        test_the_ethernet_interrupt_register_acknowledges);
    qtest_add_data_func("sn0/ioc3/ethernet-read-only-registers", NULL,
                        test_the_ethernet_read_only_registers);
    qtest_add_data_func("sn0/ioc3/ethernet-mac-reset-reports-idle", NULL,
                        test_the_ethernet_mac_reset_reports_idle);
    qtest_add_data_func("sn0/ioc3/ethernet-transmit-sends-inline-frame", NULL,
                        test_the_ethernet_transmit_sends_inline_frame);
    qtest_add_data_func("sn0/ioc3/transmit-starts-when-enabled-after-doorbell",
                        NULL, test_transmit_starts_when_enabled_late);
    qtest_add_data_func("sn0/ioc3/second-enable-write-does-not-resend", NULL,
                        test_a_second_enable_write_does_not_resend);
    qtest_add_data_func("sn0/ioc3/block-reset-keeps-ring-addresses", NULL,
                        test_the_block_reset_keeps_the_ring_addresses);
    qtest_add_data_func("sn0/ioc3/ethernet-transmit-bounds-the-inline-area",
                        NULL,
                        test_the_ethernet_transmit_bounds_the_inline_area);
    qtest_add_data_func("sn0/ioc3/ethernet-unaligned-produce-terminates",
                        NULL, test_an_unaligned_produce_index_terminates);
    qtest_add_data_func("sn0/ioc3/ethernet-transmit-honours-total-byte-count",
                        NULL,
                        test_the_ethernet_transmit_honours_total_byte_count);
    qtest_add_data_func("sn0/ioc3/ethernet-transmit-gathers-two-buffers",
                        NULL, test_the_ethernet_transmit_gathers_two_buffers);
    qtest_add_data_func("sn0/ioc3/ethernet-transmit-inserts-udp-checksum",
                        NULL, test_the_ethernet_transmit_inserts_udp_checksum);
    qtest_add_data_func("sn0/ioc3/ethernet-transmit-inserts-tcp-checksum",
                        NULL, test_the_ethernet_transmit_inserts_tcp_checksum);
    qtest_add_data_func(
        "sn0/ioc3/ethernet-transmit-leaves-checksum-alone-when-not-asked",
        NULL,
        test_the_ethernet_transmit_leaves_checksum_alone_when_not_asked);
    qtest_add_data_func("sn0/ioc3/ethernet-transmit-pads-short-frame", NULL,
                        test_the_ethernet_transmit_pads_short_frame);
    qtest_add_data_func(
        "sn0/ioc3/ethernet-transmit-leaves-short-frame-alone-when-pad-disabled",
        NULL,
        test_the_ethernet_transmit_leaves_short_frame_alone_when_pad_off);
    qtest_add_data_func("sn0/ioc3/ethernet-receive-verifies-a-tcp-checksum",
                        NULL,
                        test_the_ethernet_receive_verifies_a_tcp_checksum);
    qtest_add_data_func("sn0/ioc3/ethernet-receive-verifies-a-udp-checksum",
                        NULL,
                        test_the_ethernet_receive_verifies_a_udp_checksum);
    qtest_add_data_func("sn0/ioc3/ethernet-receive-refuses-a-corrupt-checksum",
                        NULL,
                        test_the_ethernet_receive_refuses_a_corrupt_checksum);
    qtest_add_data_func("sn0/ioc3/address-filter-accepts-station-address",
                        NULL,
                        test_address_filter_accepts_station_address);
    qtest_add_data_func("sn0/ioc3/address-filter-drops-foreign-unicast", NULL,
                        test_address_filter_drops_foreign_unicast);
    qtest_add_data_func("sn0/ioc3/address-filter-accepts-broadcast", NULL,
                        test_the_address_filter_accepts_broadcast);
    qtest_add_data_func("sn0/ioc3/promiscuous-accepts-foreign-unicast", NULL,
                        test_promiscuous_accepts_a_foreign_unicast_address);
    qtest_add_data_func(
        "sn0/ioc3/multicast-hash-accepts-set-bit-low-register", NULL,
        test_multicast_hash_accepts_a_set_bit_in_the_low_register);
    qtest_add_data_func(
        "sn0/ioc3/multicast-hash-accepts-set-bit-high-register", NULL,
        test_multicast_hash_accepts_a_set_bit_in_the_high_register);
    qtest_add_data_func("sn0/ioc3/multicast-hash-drops-unset-bit", NULL,
                        test_multicast_hash_drops_an_unset_bit);
    qtest_add_data_func("sn0/ioc3/ethernet-armed-timer-raises-its-source",
                        NULL,
                        test_an_armed_receive_timer_raises_its_own_source);
    qtest_add_data_func("sn0/ioc3/ethernet-below-watermark-raises-nothing",
                        NULL, test_a_frame_below_the_watermark_raises_nothing);
    qtest_add_data_func("sn0/ioc3/ethernet-past-watermark-keeps-raising", NULL,
                        test_falling_past_the_watermark_keeps_raising);
    qtest_add_data_func("sn0/ioc3/ethernet-watermark-raises-threshold", NULL,
                        test_reaching_the_watermark_raises_the_threshold);
    qtest_add_data_func("sn0/ioc3/ethernet-bare-produce-keeps-timer-armed",
                        NULL, test_a_bare_produce_write_leaves_the_timer_armed);
    qtest_add_data_func("sn0/ioc3/ethernet-enable-raises-a-set-source", NULL,
                        test_enabling_a_set_source_raises_the_line);
    qtest_add_data_func("sn0/ioc3/media-interface-never-busy", NULL,
                        test_the_media_interface_never_reads_busy);
    qtest_add_data_func("sn0/ioc3/buffer-ram-halves-are-distinct", NULL,
                        test_the_buffer_ram_halves_are_distinct);
    qtest_add_data_func("sn0/ioc3/buffer-ram-is-seventeen-bits-wide", NULL,
                        test_the_buffer_ram_is_seventeen_bits_wide);
    qtest_add_data_func("sn0/ioc3/buffer-ram-reports-bad-parity", NULL,
                        test_the_buffer_ram_reports_bad_parity);
    qtest_add_data_func("sn0/ioc3/buffer-ram-ignores-written-parity", NULL,
                        test_the_buffer_ram_ignores_a_written_parity_report);
    qtest_add_data_func("sn0/ioc3/size-probe-survives-parity-checking", NULL,
                        test_the_size_probe_survives_parity_checking);
    qtest_add_data_func("sn0/ioc3/clock-reads-the-time-it-was-given", NULL,
                        test_the_clock_reads_the_time_it_was_given);
    qtest_add_data_func("sn0/ioc3/clock-fresh-image-follows-date", NULL,
                        test_a_fresh_clock_has_the_date_derived_factory_image);
    qtest_add_data_func("sn0/ioc3/clock-freeze-holds-an-instant", NULL,
                        test_the_clock_freeze_holds_an_instant);
    qtest_add_data_func("sn0/ioc3/clock-year-wraps-below-its-epoch", NULL,
                        test_the_clock_year_wraps_below_its_epoch);
    qtest_add_data_func("sn0/ioc3/clock-control-reads-back", NULL,
                        test_the_clock_control_register_reads_back);
    qtest_add_data_func("sn0/ioc3/clock-takes-a-time-a-guest-sets", NULL,
                        test_the_clock_takes_a_time_a_guest_sets);
    qtest_add_data_func("sn0/ioc3/clock-storage-survives-a-system-reset", NULL,
                        test_the_storage_survives_a_system_reset);
    qtest_add_data_func("sn0/ioc3/clock-storage-persists-with-a-drive", NULL,
                        test_the_storage_persists_across_a_process_with_a_drive);
    qtest_add_data_func("sn0/ioc3/each-node-has-its-own-storage", NULL,
                        test_each_node_has_its_own_storage);
    qtest_add_data_func("sn0/ioc3/clock-storage-holds-a-write", NULL,
                        test_the_clock_storage_holds_a_write);
    qtest_add_data_func("sn0/ioc3/ports-are-distinct", NULL,
                        test_ports_are_distinct);
    qtest_add_data_func("sn0/ioc3/registers-are-a-byte-apart", NULL,
                        test_registers_are_a_byte_apart);
    qtest_add_data_func("sn0/ioc3/port-answers-a-byte-at-a-time", NULL,
                        test_the_port_answers_a_byte_at_a_time);
    qtest_add_data_func("sn0/ioc3/enable-mask-two-aliases", NULL,
                        test_the_enable_mask_has_two_write_aliases);
    qtest_add_data_func("sn0/ioc3/arbiter-reports-idle", NULL,
                        test_the_arbiter_reports_idle);
    qtest_add_data_func("sn0/ioc3/interrupt-output-stores-configuration", NULL,
                        test_the_interrupt_output_stores_its_configuration);
    qtest_add_data_func("sn0/ioc3/interrupt-output-stores-unrun-command", NULL,
                        test_int_out_stores_a_command_it_does_not_run);
    qtest_add_data_func("sn0/ioc3/undefined-output-mode-keeps-periodic", NULL,
                        test_undefined_int_out_modes_keep_periodic_output);
    qtest_add_data_func("sn0/ioc3/diagnostic-output-keeps-periodic", NULL,
                        test_diagnostic_int_out_keeps_periodic_output);
    qtest_add_data_func("sn0/ioc3/unsupported-output-count-keeps-periodic",
                        NULL,
                        test_unsupported_int_out_counts_keep_periodic_output);
    qtest_add_data_func("sn0/ioc3/fixed-output-level-visible-at-once", NULL,
                        test_a_fixed_level_is_visible_to_the_next_read);
    qtest_add_data_func("sn0/ioc3/read-catches-output-up-to-clock", NULL,
                        test_a_read_catches_the_output_up_to_the_clock);
    qtest_add_data_func("sn0/ioc3/fixed-interrupt-output-commands", NULL,
                        test_fixed_interrupt_output_commands_change_the_state);
    qtest_add_data_func("sn0/ioc3/one-shot-interrupt-output-width", NULL,
                        test_one_shot_interrupt_output_has_three_tick_width);
    qtest_add_data_func("sn0/ioc3/fixed-high-posts-output-pulse", NULL,
                        test_a_fixed_high_command_posts_the_output_pulse);
    qtest_add_data_func("sn0/ioc3/count-zero-output-pulse-interrupt", NULL,
                        test_count_zero_output_pulse_interrupt);
    qtest_add_data_func("sn0/ioc3/periodic-interrupt-output-min-interval",
                        NULL,
                        test_periodic_interrupt_output_min_interval);
    qtest_add_data_func("sn0/ioc3/periodic-interrupt-output-max-interval",
                        NULL,
                        test_periodic_interrupt_output_max_interval);
    qtest_add_data_func("sn0/ioc3/square-interrupt-output-max-interval", NULL,
                        test_square_interrupt_output_max_interval);
    qtest_add_data_func("sn0/ioc3/fixed-command-cancels-periodic-output", NULL,
                        test_fixed_command_cancels_periodic_output);
    qtest_add_data_func("sn0/ioc3/reset-cancels-interrupt-output-timer", NULL,
                        test_reset_cancels_an_interrupt_output_timer);
    qtest_add_data_func("sn0/ioc3/serial-bit-ignores-acknowledgement", NULL,
                        test_a_serial_bit_ignores_an_acknowledgement);
    qtest_add_data_func("sn0/ioc3/second-interrupt-reaches-bridge-pin-3",
                        NULL,
                        test_the_second_interrupt_reaches_bridge_pin_3);
    qtest_add_data_func("sn0/ioc3/one-wire-slot-always-finishes", NULL,
                        test_a_one_wire_slot_always_finishes);
    qtest_add_data_func("sn0/ioc3/one-wire-reset-finds-the-part", NULL,
                        test_a_one_wire_reset_finds_the_ethernet_address_part);
    qtest_add_data_func("sn0/ioc3/one-wire-dead-until-enabled", NULL,
                        test_the_one_wire_bus_is_dead_until_enabled);
    qtest_add_data_func("sn0/ioc3/one-wire-slot-preserves-timing", NULL,
                        test_one_wire_slot_preserves_the_timing_written);
    qtest_add_data_func("sn0/ioc3/one-wire-search-finds-the-part", NULL,
                        test_a_search_finds_the_ethernet_address_part);
    qtest_add_data_func("sn0/ioc3/one-wire-read-rom-whole-identity", NULL,
                        test_read_rom_returns_the_whole_identity);
    qtest_add_data_func("sn0/ioc3/one-wire-each-node-distinct-identity", NULL,
                        test_each_node_has_its_own_onewire_identity);
    qtest_add_data_func("sn0/ioc3/one-wire-wrong-match-rom-unselects", NULL,
                        test_a_wrong_match_rom_leaves_the_part_unselected);
    qtest_add_data_func("sn0/ioc3/one-wire-read-memory", NULL,
                        test_reading_the_page_via_read_memory);
    qtest_add_data_func("sn0/ioc3/one-wire-read-memory-crc", NULL,
                        test_reading_the_page_via_read_memory_crc);
    qtest_add_data_func("sn0/ioc3/one-wire-read-memory-after-search", NULL,
                        test_reading_the_page_after_search);
    qtest_add_data_func("sn0/ioc3/one-wire-default-mac-is-sgi", NULL,
                        test_the_default_station_address_is_an_sgi_address);
    qtest_add_data_func("sn0/ioc3/one-wire-explicit-mac-survives", NULL,
                        test_an_explicit_mac_in_the_generator_range_stays);
    qtest_add_data_func("sn0/ioc3/device-supplied-mac-survives", NULL,
                        test_a_device_supplied_mac_survives);
    qtest_add_data_func("sn0/ioc3/one-wire-read-status-no-redirection", NULL,
                        test_read_status_returns_no_redirection);
    qtest_add_data_func("sn0/ioc3/one-wire-read-status-whole-field", NULL,
                        test_read_status_serves_the_whole_field);
    qtest_add_data_func("sn0/ioc3/pin-control-two-aliases", NULL,
                        test_pin_control_has_two_write_aliases);
    qtest_add_data_func("sn0/ioc3/pin-register-reaches-data-word", NULL,
                        test_a_pin_register_reaches_the_shared_data_word);

    ret = g_test_run();
    qtest_quit(qts);

    return ret;
}
