/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Exercise a Sharp x16 chip behind Bridge without assigning IP30 wiring. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "memtx.h"

#define BRIDGE_BASE UINT64_C(0x10000000)
#define FLASH (BRIDGE_BASE + 0xc00000)
#define FLASH_SELECT_SIZE (2 * 1024 * 1024)
#define BRIDGE_STATUS (BRIDGE_BASE + 0xc)
#define BRIDGE_CONTROL (BRIDGE_BASE + 0x24)
#define STATUS_FLASH_SELECT (1u << 6)
#define FLASH_WRITE_ENABLE (1u << 31)
#define CHIP_SIZE (1024 * 1024)
#define PROGRAM_NS 8000
#define CHIP_PATH "/machine/peripheral/flash-fixture/chip"

static QTestState *start(bool second_select, bool strap)
{
    const char *chip = second_select ?
        "/machine/peripheral/flash-fixture1/chip" : CHIP_PATH;
    QTestState *qts = qtest_initf(
        MEMTX_TEST_DEVICE
        "-M none -cpu R10000 "
        "-device lh28f800su-test-device,id=flash-fixture%s "
        "-device sgi-bridge,id=bridge,no-board-identity=on,"
        "fabric=/machine/unattached/system[0]%s%s",
        second_select ? "1" : "",
        second_select ? ",flash-select1=/machine/peripheral/"
                        "flash-fixture1/chip/lh28f800su[0]" :
                        ",flash=/machine/peripheral/"
                        "flash-fixture/chip/lh28f800su[0]",
        strap ? ",flash-select=on" : "");

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/bridge", 0,
                          BRIDGE_BASE);
    /* Supply and protection inputs are fixture choices, not IP30 wiring. */
    qtest_set_irq_in(qts, chip, "pins", 0, 1); /* VPP valid */
    qtest_set_irq_in(qts, chip, "pins", 1, 1); /* WP high */
    qtest_set_irq_in(qts, chip, "pins", 2, 1); /* RP high */
    qtest_clock_step(qts, 400);
    return qts;
}

static void test_id_through_bridge(void)
{
    QTestState *qts = start(false, false);

    g_assert_cmphex(qtest_readw(qts, FLASH), ==, 0xffff);
    qtest_writew(qts, FLASH, 0x9090);
    g_assert_cmphex(qtest_readw(qts, FLASH), ==, 0xffff);
    qtest_writel(qts, BRIDGE_CONTROL,
                 qtest_readl(qts, BRIDGE_CONTROL) | FLASH_WRITE_ENABLE);
    qtest_writew(qts, FLASH, 0x9090);
    g_assert_cmphex(qtest_readw(qts, FLASH), ==, 0x00b0);
    g_assert_cmphex(qtest_readw(qts, FLASH + 2), ==, 0x66a8);

    qtest_quit(qts);
}

static void test_status_and_program_through_bridge(void)
{
    QTestState *qts = start(false, false);
    const uint64_t target = FLASH + 0x40;

    qtest_writel(qts, BRIDGE_CONTROL,
                 qtest_readl(qts, BRIDGE_CONTROL) | FLASH_WRITE_ENABLE);
    qtest_writew(qts, target, 0x4040);
    qtest_writew(qts, target, 0x1234);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0x0000);
    qtest_clock_step(qts, PROGRAM_NS);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0x0080);
    qtest_writew(qts, target, 0xffff);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0x1234);

    qtest_quit(qts);
}

static void test_select_repeats_chip_in_fixture(void)
{
    QTestState *qts = start(false, false);
    const uint64_t target = FLASH + 0x40;

    /*
     * Bridge currently wraps the 1 MiB part within its 2 MiB select.
     * This is model behavior, not evidence of wiring on an IP30 board.
     */
    qtest_writel(qts, BRIDGE_CONTROL,
                 qtest_readl(qts, BRIDGE_CONTROL) | FLASH_WRITE_ENABLE);
    qtest_writew(qts, target, 0x4040);
    qtest_writew(qts, target, 0x5678);
    qtest_clock_step(qts, PROGRAM_NS);
    qtest_writew(qts, target, 0xffff);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0x5678);
    g_assert_cmphex(qtest_readw(qts, target + CHIP_SIZE), ==, 0x5678);

    qtest_quit(qts);
}

static void test_second_select(gconstpointer data)
{
    bool strap = GPOINTER_TO_INT(data);
    QTestState *qts = start(true, strap);
    uint64_t selected = FLASH + (strap ? 0 : FLASH_SELECT_SIZE);
    uint64_t empty = FLASH + (strap ? FLASH_SELECT_SIZE : 0);
    uint16_t word;

    g_assert_cmphex(qtest_readl(qts, BRIDGE_STATUS) & STATUS_FLASH_SELECT,
                    ==, strap ? STATUS_FLASH_SELECT : 0);
    g_assert_cmpuint(memtx_readw(qts, empty + 0x40, &word), ==,
                     MEMTX_DECODE_ERROR);
    g_assert_cmphex(qtest_readw(qts, selected + 0x40), ==, 0xffff);

    qtest_writel(qts, BRIDGE_CONTROL,
                 qtest_readl(qts, BRIDGE_CONTROL) | FLASH_WRITE_ENABLE);
    qtest_writew(qts, selected, 0x9090);
    g_assert_cmphex(qtest_readw(qts, selected), ==, 0x00b0);
    g_assert_cmphex(qtest_readw(qts, selected + 2), ==, 0x66a8);
    qtest_writew(qts, selected + 0x40, 0x4040);
    qtest_writew(qts, selected + 0x40, 0x3456);
    g_assert_cmphex(qtest_readw(qts, selected + 0x40), ==, 0x0000);
    qtest_clock_step(qts, PROGRAM_NS);
    g_assert_cmphex(qtest_readw(qts, selected + 0x40), ==, 0x0080);
    qtest_writew(qts, selected, 0xffff);
    g_assert_cmphex(qtest_readw(qts, selected + 0x40), ==, 0x3456);
    g_assert_cmphex(qtest_readw(qts, selected + CHIP_SIZE + 0x40), ==,
                    0x3456);
    g_assert_cmpuint(memtx_readw(qts, empty + 0x40, &word), ==,
                     MEMTX_DECODE_ERROR);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi-bridge/sharp-flash/id-through-bridge",
                   test_id_through_bridge);
    qtest_add_func("/sgi-bridge/sharp-flash/status-and-program",
                   test_status_and_program_through_bridge);
    qtest_add_func("/sgi-bridge/sharp-flash/select-repeats-chip-fixture",
                   test_select_repeats_chip_in_fixture);
    qtest_add_data_func("/sgi-bridge/sharp-flash/second-select",
                        GINT_TO_POINTER(false), test_second_select);
    qtest_add_data_func("/sgi-bridge/sharp-flash/second-select-strapped",
                        GINT_TO_POINTER(true), test_second_select);
    return g_test_run();
}
