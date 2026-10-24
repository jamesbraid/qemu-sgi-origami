/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* MACE's selected complete-byte PS/2 connection, using real PS/2 cores. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "memtx.h"
#include "sgi-migrate.h"
#include "hw/mips/sgi/mace-ps2.h"

#define MACE 0x1f000000ULL
#define PS2 (MACE + 0x320000)
#define TX 0
#define RX 8
#define CTRL 16
#define STATUS 24
#define ISTAT (MACE + 0x310010)
#define IMASK (MACE + 0x310018)
#define KIRQ (1U << 9)
#define KPOLL (1U << 10)
#define MIRQ (1U << 11)
#define MPOLL (1U << 12)
#define CRIME 0x14000000ULL
#define CLOCK 0x10
#define ENABLE 0x12
#define RXIE 8
#define TXIE 4
#define RESET 0x20
#define FULL 0x10
#define EMPTY 8

static QTestState *start_extra(const char *extra)
{
    QTestState *s = qtest_initf("-M none -cpu R10000 -nodefaults "
        "-device sgi-mace,id=mace " MEMTX_TEST_DEVICE " %s", extra);

    qtest_sysbus_mmio_map(s, "/machine/peripheral/mace", 0, MACE);
    qtest_irq_intercept_out_named(s, "/machine/peripheral/mace", "crime");
    return s;
}

static uint64_t reg(unsigned port, unsigned reg)
{
    return PS2 + port * 32 + reg;
}

static void control(QTestState *s, unsigned port, uint64_t value)
{
    qtest_writeq(s, reg(port, CTRL), value);
}

static void send_byte(QTestState *s, unsigned port, uint8_t byte)
{
    qtest_writeq(s, reg(port, TX), byte);
}

static uint8_t receive(QTestState *s, unsigned port)
{
    uint64_t value = qtest_readq(s, reg(port, RX));

    g_assert_cmphex(value & ~0xffffULL, ==, 0);
    g_assert_cmphex((value >> 8) & FULL, ==, FULL);
    g_assert_cmphex((value >> 8) & 0xc0, ==, 0);
    return value;
}

static void expect_empty(QTestState *s, unsigned port)
{
    g_assert_cmphex(qtest_readq(s, reg(port, STATUS)) & FULL, ==, 0);
}

static void test_reset_aliases(void)
{
    QTestState *s = start_extra("");

    for (unsigned p = 0; p < 2; p++) {
        g_assert_cmphex(qtest_readq(s, reg(p, TX)), ==, 0);
        g_assert_cmphex(qtest_readq(s, reg(p, CTRL)), ==, CLOCK);
        /* 0x0b is the reviewed byte-model policy, not a silicon claim. */
        g_assert_cmphex(qtest_readq(s, reg(p, STATUS)), ==, 0x0b);
        g_assert_cmphex(qtest_readq(s, reg(p, RX)), ==, 0x0b00);
    }
    control(s, 0, 0x04);
    control(s, 1, 0x18);
    for (unsigned bank = 0; bank < 0x10000; bank += 64) {
        qtest_writeq(s, PS2 + bank + CTRL, 4);
        qtest_writeq(s, PS2 + bank + 32 + CTRL, 0x18);
        g_assert_cmphex(qtest_readq(s, PS2 + bank + CTRL), ==, 4);
        g_assert_cmphex(qtest_readq(s, PS2 + bank + 32 + CTRL), ==, 0x18);
        g_assert_cmphex(qtest_readq(s, PS2 + bank + STATUS), ==, 8);
        g_assert_cmphex(qtest_readq(s, PS2 + bank + 32 + STATUS), ==, 0x0b);
        g_assert_cmphex(qtest_readq(s, PS2 + bank + RX), ==, 0x0800);
        g_assert_cmphex(qtest_readq(s, PS2 + bank + 32 + RX), ==, 0x0b00);
        g_assert_cmphex(qtest_readq(s, PS2 + bank + TX), ==, 0);
        g_assert_cmphex(qtest_readq(s, PS2 + bank + 32 + TX), ==, 0);
    }
    control(s, 0, CLOCK);
    control(s, 1, CLOCK);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(s, ISTAT) & 0x1e00, ==, KPOLL | MPOLL);
    qtest_quit(s);
}

static void test_pending_transmit(void)
{
    QTestState *s = start_extra("");

    send_byte(s, 0, 0xee);
    send_byte(s, 0, 0xff);
    g_assert_cmphex(qtest_readq(s, reg(0, STATUS)), ==, 3);
    expect_empty(s, 0);
    control(s, 0, ENABLE);
    g_assert_cmphex(receive(s, 0), ==, 0xee);
    expect_empty(s, 0);
    control(s, 1, ENABLE);
    send_byte(s, 1, 0xf2);
    g_assert_cmphex(receive(s, 1), ==, 0xfa);
    g_assert_cmphex(receive(s, 1), ==, 0);
    expect_empty(s, 1);
    qtest_quit(s);
}

static void test_accesses(void)
{
    QTestState *s = start_extra("");
    uint8_t raw[8];
    uint64_t value;

    control(s, 0, ENABLE);
    send_byte(s, 0, 0xee);
    for (unsigned size = 1; size <= 4; size *= 2) {
        for (unsigned r = 0; r < 32; r += 8) {
            memtx_write(s, reg(0, r), UINT64_MAX, size);
            memtx_read(s, reg(0, r), &value, size);
            g_assert_cmphex(value, ==, 0);
        }
    }
    for (unsigned off = 1; off < 8; off++) {
        memtx_write(s, reg(0, CTRL) + off, UINT64_MAX, 8);
        memtx_read(s, reg(0, RX) + off, &value, 8);
        g_assert_cmphex(value, ==, 0);
    }
    qtest_writeq(s, reg(0, RX), 0xff);
    qtest_writeq(s, reg(0, STATUS), 0xff);
    g_assert_cmphex(qtest_readq(s, reg(0, TX)), ==, 0);
    control(s, 0, 0xffffffffffffffd2ULL);
    g_assert_cmphex(qtest_readq(s, reg(0, CTRL)), ==, ENABLE);
    g_assert_cmphex(qtest_readq(s, reg(0, STATUS)), ==, 0x18);
    g_assert_cmpint(memtx_transaction(s, -1, reg(0, RX), raw, 8, false),
                    ==, MEMTX_OK);
    const uint8_t expected[] = { 0, 0, 0, 0, 0, 0, 0x18, 0xee };
    g_assert_cmpmem(raw, sizeof(raw), expected, sizeof(expected));
    expect_empty(s, 0);
    g_assert_cmphex(qtest_readq(s, reg(0, RX)), ==, 0x0bee);
    qtest_quit(s);
}

static void key(QTestState *s, const char *name, bool down)
{
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':"
        "[{'type':'key','data':{'down':%i,'key':"
        "{'type':'qcode','data':%s}}}]}}", down, name);
}

static void motion(QTestState *s, int x, int y, bool down)
{
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'rel','data':{'axis':'x','value':%d}},"
        "{'type':'rel','data':{'axis':'y','value':%d}},"
        "{'type':'btn','data':{'button':'left','down':%i}}]}}", x, y, down);
}

static void command(QTestState *s, unsigned port, uint8_t byte)
{
    send_byte(s, port, byte);
    g_assert_cmphex(receive(s, port), ==, 0xfa);
}

static void test_keyboard_input(void)
{
    QTestState *s = start_extra("");

    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    control(s, 0, ENABLE | RXIE);
    command(s, 0, 0xf0);
    command(s, 0, 2);
    command(s, 0, 0xf4);
    key(s, "a", true);
    key(s, "a", false);
    g_assert_cmphex(receive(s, 0), ==, 0x1c);
    g_assert_cmphex(receive(s, 0), ==, 0xf0);
    g_assert_cmphex(receive(s, 0), ==, 0x1c);
    expect_empty(s, 0);
    command(s, 0, 0xed);
    command(s, 0, 5);
    send_byte(s, 0, 0xf2);
    g_assert_cmphex(receive(s, 0), ==, 0xfa);
    g_assert_cmphex(receive(s, 0), ==, 0xab);
    g_assert_cmphex(receive(s, 0), ==, 0x83);
    command(s, 0, 0xf5);
    key(s, "b", true);
    key(s, "b", false);
    expect_empty(s, 0);
    send_byte(s, 0, 0xff);
    g_assert_cmphex(receive(s, 0), ==, 0xfa);
    g_assert_cmphex(receive(s, 0), ==, 0xaa);
    expect_empty(s, 0);
    expect_empty(s, 1);
    qtest_quit(s);
}

static void test_mouse_input(void)
{
    QTestState *s = start_extra("");

    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    control(s, 0, ENABLE);
    control(s, 1, ENABLE | RXIE);
    command(s, 1, 0xf4);
    motion(s, 7, -3, true);
    key(s, "c", true);
    g_assert_cmphex(receive(s, 1), ==, 0x09);
    g_assert_cmphex(receive(s, 1), ==, 7);
    g_assert_cmphex(receive(s, 1), ==, 3);
    motion(s, -4, 2, false);
    g_assert_cmphex(receive(s, 1), ==, 0x38);
    g_assert_cmphex(receive(s, 1), ==, 0xfc);
    g_assert_cmphex(receive(s, 1), ==, 0xfe);
    g_assert_cmphex(receive(s, 0), ==, 0x21);
    key(s, "c", false);
    g_assert_cmphex(receive(s, 0), ==, 0xf0);
    g_assert_cmphex(receive(s, 0), ==, 0x21);
    command(s, 1, 0xf5);
    motion(s, 2, 1, false);
    expect_empty(s, 1);
    command(s, 1, 0xf6);
    command(s, 1, 0xf4);
    command(s, 1, 0xf0);
    motion(s, 5, -2, true);
    expect_empty(s, 1);
    command(s, 1, 0xeb);
    g_assert_cmphex(receive(s, 1), ==, 0x09);
    g_assert_cmphex(receive(s, 1), ==, 5);
    g_assert_cmphex(receive(s, 1), ==, 2);
    expect_empty(s, 1);
    send_byte(s, 1, 0xff);
    g_assert_cmphex(receive(s, 1), ==, 0xfa);
    g_assert_cmphex(receive(s, 1), ==, 0xaa);
    g_assert_cmphex(receive(s, 1), ==, 0);
    qtest_quit(s);
}

static void test_inhibit_queue_priority(void)
{
    QTestState *s = start_extra("");

    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    control(s, 0, 0);
    key(s, "a", true);
    key(s, "a", false);
    key(s, "b", true);
    key(s, "b", false);
    expect_empty(s, 0);
    send_byte(s, 0, 0xee);
    control(s, 0, CLOCK);
    expect_empty(s, 0);
    control(s, 0, ENABLE | 1);
    expect_empty(s, 0);
    g_assert_cmphex(qtest_readq(s, reg(0, STATUS)), ==, EMPTY);
    control(s, 0, ENABLE);
    /* Echo precedes queued scan codes; reentrant IRQs must not duplicate it. */
    const uint8_t expected[] = { 0xee, 0x1c, 0xf0, 0x1c, 0x32, 0xf0, 0x32 };
    for (unsigned i = 0; i < ARRAY_SIZE(expected); i++) {
        g_assert_cmphex(qtest_readq(s, reg(0, STATUS)), ==, 0x18);
        g_assert_cmphex(qtest_readq(s, reg(0, CTRL)), ==, ENABLE);
        g_assert_cmphex(receive(s, 0), ==, expected[i]);
    }
    expect_empty(s, 0);
    send_byte(s, 0, 0xee);
    key(s, "c", true);
    control(s, 0, CLOCK);
    send_byte(s, 0, 0xf2);
    g_assert_cmphex(receive(s, 0), ==, 0xee);
    expect_empty(s, 0);
    control(s, 0, ENABLE);
    const uint8_t priority[] = { 0xfa, 0xab, 0x83, 0x21 };
    for (unsigned i = 0; i < ARRAY_SIZE(priority); i++) {
        g_assert_cmphex(receive(s, 0), ==, priority[i]);
    }
    expect_empty(s, 0);
    qtest_quit(s);
}

static void test_local_reset(void)
{
    QTestState *s = start_extra("");

    control(s, 0, ENABLE);
    control(s, 1, ENABLE);
    send_byte(s, 0, 0xf0);
    send_byte(s, 1, 0xf2);
    control(s, 0, RESET | ENABLE);
    control(s, 1, RESET | ENABLE);
    for (unsigned p = 0; p < 2; p++) {
        g_assert_cmphex(qtest_readq(s, reg(p, STATUS)), ==, 0);
        g_assert_cmphex(qtest_readq(s, reg(p, RX)), ==, 0);
        send_byte(s, p, 0xff);
        control(s, p, RESET | ENABLE);
        g_assert_cmphex(qtest_readq(s, reg(p, STATUS)), ==, 0);
        control(s, p, ENABLE);
    }
    command(s, 0, 3);
    command(s, 0, 0xf0);
    command(s, 0, 0);
    g_assert_cmphex(receive(s, 0), ==, 3);
    g_assert_cmphex(receive(s, 1), ==, 0);
    control(s, 0, 0);
    send_byte(s, 0, 0xff);
    control(s, 0, RESET | ENABLE);
    control(s, 0, ENABLE);
    expect_empty(s, 0);
    control(s, 1, ENABLE | 1);
    send_byte(s, 1, 0xf2);
    control(s, 1, RESET | ENABLE | 1);
    control(s, 1, ENABLE);
    g_assert_cmphex(receive(s, 1), ==, 0xfa);
    g_assert_cmphex(receive(s, 1), ==, 0);
    send_byte(s, 0, 0xf2);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    for (unsigned p = 0; p < 2; p++) {
        g_assert_cmphex(qtest_readq(s, reg(p, STATUS)), ==, 0x0b);
        g_assert_cmphex(qtest_readq(s, reg(p, RX)), ==, 0x0b00);
        control(s, p, ENABLE);
        expect_empty(s, p);
    }
    qtest_quit(s);
}

static void test_irq_poll(void)
{
    QTestState *s = start_extra("");

    qtest_writeq(s, IMASK, KPOLL | MPOLL);
    g_assert_cmphex(qtest_readq(s, ISTAT) & 0x1e00, ==, KPOLL | MPOLL);
    g_assert_false(qtest_get_irq(s, 5));
    control(s, 0, CLOCK | TXIE);
    control(s, 1, CLOCK | TXIE);
    g_assert_cmphex(qtest_readq(s, ISTAT) & 0x1e00, ==, 0x1e00);
    g_assert_false(qtest_get_irq(s, 5));
    qtest_writeq(s, IMASK, KIRQ | MIRQ | KPOLL | MPOLL);
    g_assert_true(qtest_get_irq(s, 5));
    send_byte(s, 0, 0xee);
    g_assert_cmphex(qtest_readq(s, ISTAT) & (KIRQ | KPOLL), ==, 0);
    control(s, 1, RESET);
    g_assert_false(qtest_get_irq(s, 5));
    control(s, 0, ENABLE | RXIE);
    g_assert_true(qtest_get_irq(s, 5));
    qtest_writeq(s, IMASK, 0);
    g_assert_false(qtest_get_irq(s, 5));
    qtest_writeq(s, IMASK, KIRQ);
    g_assert_true(qtest_get_irq(s, 5));
    qtest_writeq(s, ISTAT, 0);
    qtest_writeq(s, ISTAT, 0x1e00);
    g_assert_true(qtest_get_irq(s, 5));
    control(s, 0, ENABLE | RXIE | TXIE);
    g_assert_cmphex(receive(s, 0), ==, 0xee);
    g_assert_true(qtest_get_irq(s, 5));
    control(s, 0, ENABLE | RXIE);
    g_assert_false(qtest_get_irq(s, 5));
    qtest_writeq(s, IMASK, KIRQ | MIRQ | (1U << 13));
    qtest_writeq(s, MACE + 0x340008, 3);
    qtest_clock_step(s, 3 * 960);
    control(s, 0, ENABLE | TXIE);
    control(s, 1, ENABLE | TXIE);
    control(s, 0, RESET);
    g_assert_true(qtest_readq(s, ISTAT) & MIRQ);
    control(s, 1, RESET);
    g_assert_true(qtest_get_irq(s, 5));
    g_assert_true(qtest_readq(s, ISTAT) & (1U << 13));
    qtest_writeq(s, MACE + 0x340008, 100);
    g_assert_false(qtest_get_irq(s, 5));
    qtest_quit(s);
}

static void test_irq_matrix(void)
{
    for (unsigned p = 0; p < 2; p++) {
        QTestState *s = start_extra("");
        uint32_t irq = p ? MIRQ : KIRQ;
        uint32_t poll = p ? MPOLL : KPOLL;
        uint32_t other_poll = p ? KPOLL : MPOLL;

        control(s, p, ENABLE);
        for (unsigned state = 0; state < 3; state++) {
            if (state == 1) {
                send_byte(s, p, p ? 0xf2 : 0xee);
            } else if (state == 2) {
                send_byte(s, p, p ? 0xf2 : 0xee);
            }
            bool tx = state != 2;
            bool rx = state != 0;

            for (unsigned local = 0; local < 4; local++) {
                bool request = (tx && (local & 1)) || (rx && (local & 2));

                control(s, p, ENABLE | (local << 2));
                for (unsigned mask = 0; mask < 4; mask++) {
                    uint32_t bits = (mask & 1 ? KIRQ : 0) |
                                    (mask & 2 ? MIRQ : 0) | KPOLL | MPOLL;
                    qtest_writeq(s, IMASK, bits);
                    g_assert_cmphex(qtest_readq(s, ISTAT) & 0x1e00, ==,
                        other_poll | poll | (request ? irq : 0));
                    g_assert_cmpint(qtest_get_irq(s, 5), ==,
                                     request && (bits & irq));
                }
            }
        }
        control(s, p, RESET | ENABLE | TXIE | RXIE);
        g_assert_cmphex(qtest_readq(s, ISTAT) & (irq | poll), ==, 0);
        g_assert_false(qtest_get_irq(s, 5));
        qtest_quit(s);
    }
}

static void migration_case(bool inhibited)
{
    g_autofree char *dir = g_dir_make_tmp("mace-ps2-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "migrate", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = start_extra("");
    QTestState *to = start_extra("-incoming defer");

    control(from, 0, ENABLE | RXIE);
    send_byte(from, 0, 0xf0);
    send_byte(from, 0, 0);
    control(from, 1, inhibited ? ENABLE | 1 : 0);
    send_byte(from, 1, 0xf2);
    qtest_writeq(from, IMASK, KIRQ | MIRQ);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_true(qtest_get_irq(to, 5));
    g_assert_cmphex(qtest_readq(to, reg(0, STATUS)), ==, FULL);
    g_assert_cmphex(qtest_readq(to, reg(1, STATUS)), ==, inhibited ? EMPTY : 0);
    g_assert_cmphex(receive(to, 0), ==, 0xfa);
    g_assert_cmphex(receive(to, 0), ==, 0xfa);
    g_assert_cmphex(receive(to, 0), ==, 2);
    expect_empty(to, 0);
    control(to, 1, ENABLE | RXIE | 1);
    expect_empty(to, 1);
    control(to, 1, ENABLE | RXIE);
    g_assert_cmphex(receive(to, 1), ==, 0xfa);
    g_assert_cmphex(receive(to, 1), ==, 0);
    expect_empty(to, 1);
    qtest_quit(from);
    qtest_quit(to);
    rmdir(dir);
}

static void test_migration(void)
{
    migration_case(false);
    migration_case(true);
}

static void test_board_route(void)
{
    QTestState *s = qtest_init("-M o2-test -m 64M -nodefaults");

    qtest_irq_intercept_out_named(s, "/machine/crime", "cpu-ip");
    qtest_writeq(s, IMASK, KIRQ | MIRQ | KPOLL | MPOLL);
    qtest_writeq(s, CRIME + 0x18, 1U << 5);
    g_assert_false(qtest_get_irq(s, 0));
    control(s, 0, ENABLE | RXIE);
    send_byte(s, 0, 0xee);
    g_assert_true(qtest_readq(s, CRIME + 0x10) & (1U << 5));
    g_assert_true(qtest_get_irq(s, 0));
    control(s, 1, ENABLE | RXIE);
    send_byte(s, 1, 0xf2);
    g_assert_cmphex(receive(s, 0), ==, 0xee);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, CRIME + 0x18, 0);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_writeq(s, CRIME + 0x18, 1U << 5);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, CRIME + 0x10, 0);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_cmphex(receive(s, 1), ==, 0xfa);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_cmphex(receive(s, 1), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_cmphex(qtest_readq(s, ISTAT) & 0x1e00, ==, KPOLL | MPOLL);
    qtest_quit(s);
}

static void test_frames(void)
{
    MACEPS2Port p = { .control = MACE_PS2_CLOCK };
    const uint8_t data[] = { 0, 0xff, 0x37, 0x80 };

    for (unsigned i = 0; i < ARRAY_SIZE(data); i++) {
        bool parity = !(ctpop8(data[i]) & 1);

        for (unsigned fault = 0; fault < 4; fault++) {
            p.rx_full = false;
            g_assert_true(mace_ps2_receive_frame(&p, data[i], fault == 2,
                parity ^ (fault == 1), fault != 3));
            g_assert_true(p.rx_full);
            g_assert_cmphex(p.rx, ==, data[i]);
            g_assert_cmphex(p.errors, ==, fault == 1 ? MACE_PS2_PARITY :
                              fault >= 2 ? MACE_PS2_FRAMING : 0);
            g_assert_false(mace_ps2_receive_frame(&p, 0x55, false, true, true));
            g_assert_cmphex(p.rx, ==, data[i]);
            uint8_t errors = p.errors;
            uint16_t word = mace_ps2_receive(&p);

            g_assert_cmphex(word, ==, data[i] |
                ((MACE_PS2_TXEMPTY | MACE_PS2_RXFULL | errors) << 8));
            g_assert_false(p.rx_full);
            g_assert_cmphex(mace_ps2_status(&p), ==, 0x0b | errors);
            p.control = 0;
            g_assert_false(mace_ps2_receive_frame(&p, 0x55, false, true, true));
            p.control = MACE_PS2_CLOCK | MACE_PS2_RESET;
            g_assert_false(mace_ps2_receive_frame(&p, 0x55, false, true, true));
            p.control = MACE_PS2_CLOCK;
            p.inhibited = true;
            g_assert_false(mace_ps2_receive_frame(&p, 0x55, false, true, true));
            p.inhibited = false;
            p.tx_pending = true;
            g_assert_false(mace_ps2_receive_frame(&p, 0x55, false, true, true));
            p.tx_pending = false;
            g_assert_cmphex(p.errors, ==, errors);
            g_assert_true(mace_ps2_receive_frame(&p, 0x55, false, true, true));
            g_assert_cmphex(p.errors, ==, 0);
            p.errors = MACE_PS2_PARITY | MACE_PS2_FRAMING;
            mace_ps2_clear(&p);
            g_assert_cmphex(mace_ps2_status(&p), ==, 0x0b);
            g_assert_cmphex(mace_ps2_receive(&p), ==, 0x0b00);
        }
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/mace-ps2/reset-aliases", test_reset_aliases);
    qtest_add_func("/sgi/mace-ps2/pending-transmit", test_pending_transmit);
    qtest_add_func("/sgi/mace-ps2/accesses", test_accesses);
    qtest_add_func("/sgi/mace-ps2/keyboard-input", test_keyboard_input);
    qtest_add_func("/sgi/mace-ps2/mouse-input", test_mouse_input);
    qtest_add_func("/sgi/mace-ps2/inhibit-queue-priority",
                   test_inhibit_queue_priority);
    qtest_add_func("/sgi/mace-ps2/local-reset", test_local_reset);
    qtest_add_func("/sgi/mace-ps2/irq-poll", test_irq_poll);
    qtest_add_func("/sgi/mace-ps2/irq-matrix", test_irq_matrix);
    qtest_add_func("/sgi/mace-ps2/migration", test_migration);
    if (qtest_has_machine("o2-test")) {
        qtest_add_func("/sgi/mace-ps2/board-route", test_board_route);
    }
    qtest_add_func("/sgi/mace-ps2/frames", test_frames);
    return g_test_run();
}
