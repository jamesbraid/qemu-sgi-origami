/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Synthetic O2 wiring profile; no board population or firmware is implied. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "memtx.h"
#include "sgi-launch.h"
#include "sgi-mips-emit.h"
#include "sgi-wait.h"

#define RAM_FULL 0x40000000ULL
#define RAM_NO_ECC 0x80000000ULL
#define RAM_SIZE (64 * MiB)
#define CRIME 0x14000000ULL
#define MACE_PCI 0x1f080000ULL
#define MACE_PERIPH 0x1f310000ULL

static QTestState *o2_start(void)
{
    return qtest_initf(MEMTX_TEST_DEVICE
                       "-M o2-test -cpu R10000 -m 64M -nodefaults "
                       "-accel tcg,thread=single -S");
}

static void test_ram_aliases(void)
{
    QTestState *qts = o2_start();
    const uint64_t offset = 0x1238;
    uint64_t value;

    g_assert_cmpint(memtx_writeq(qts, RAM_FULL + offset,
                                 0x0123456789abcdefULL), ==, MEMTX_OK);
    g_assert_cmpint(memtx_readq(qts, offset, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0x0123456789abcdefULL);
    g_assert_cmpint(memtx_readq(qts, RAM_NO_ECC + offset, &value),
                    ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0x0123456789abcdefULL);

    g_assert_cmpint(memtx_writeq(qts, RAM_NO_ECC + offset,
                                 0xfedcba9876543210ULL), ==, MEMTX_OK);
    g_assert_cmpint(memtx_readq(qts, RAM_FULL + offset, &value),
                    ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0xfedcba9876543210ULL);

    /* Absent RAM must fail, including the upper-half alias for this fit. */
    g_assert_cmpint(memtx_readq(qts, RAM_SIZE, &value), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readq(qts, RAM_FULL + RAM_SIZE, &value),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_readq(qts, RAM_NO_ECC + RAM_SIZE, &value),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_readq(qts, 0x20000000ULL, &value), !=, MEMTX_OK);
    {
        uint8_t byte;

        g_assert_cmpint(memtx_readb(qts, 0x1fc00000ULL, &byte), !=, MEMTX_OK);
    }
    qtest_quit(qts);
}

static void test_reset_registers(void)
{
    QTestState *qts = o2_start();
    uint64_t value;
    uint32_t pci;

    g_assert_cmpint(memtx_readq(qts, CRIME, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0xb1);
    g_assert_cmpint(memtx_readq(qts, CRIME + 0x18, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0);
    g_assert_cmpint(memtx_readq(qts, CRIME + 0x20, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0);
    g_assert_cmpint(memtx_readq(qts, MACE_PERIPH, &value), ==, MEMTX_OK);
    g_assert_cmphex(value & 1, ==, 1);
    g_assert_cmpint(memtx_readq(qts, MACE_PERIPH + 0x10, &value),
                    ==, MEMTX_OK);
    g_assert_cmphex(value & (7ULL << 13), ==, 0);

    g_assert_cmpint(memtx_writeq(qts, CRIME + 0x18, 1ULL << 5),
                    ==, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(qts, MACE_PERIPH + 0x18, 1ULL << 8),
                    ==, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(qts, MACE_PERIPH, 0), ==, MEMTX_OK);
    qtest_system_reset(qts);
    g_assert_cmpint(memtx_readq(qts, CRIME + 0x18, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0);
    g_assert_cmpint(memtx_readq(qts, MACE_PERIPH + 0x18, &value),
                    ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0);
    g_assert_cmpint(memtx_readq(qts, MACE_PERIPH, &value), ==, MEMTX_OK);
    g_assert_cmphex(value & 1, ==, 1);
    g_assert_cmpint(memtx_readl(qts, MACE_PCI + 0xc, &pci), ==, MEMTX_OK);
    g_assert_cmphex(pci, ==, 1);
    g_assert_cmpint(memtx_readl(qts, MACE_PCI + 8, &pci), ==, MEMTX_OK);
    g_assert_cmphex(pci & 0xff, ==, 0);
    qtest_quit(qts);
}

static void test_interrupt_chain(void)
{
    QTestState *qts = o2_start();
    uint64_t value;

    qtest_irq_intercept_out_named(qts, "/machine/crime", "cpu-ip");
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_set_irq_in(qts, "/machine/mace", "rtc", 0, 1);
    g_assert_cmpint(memtx_readq(qts, MACE_PERIPH + 0x10, &value),
                    ==, MEMTX_OK);
    g_assert_cmphex(value & (1ULL << 8), ==, 1ULL << 8);
    g_assert_cmpint(memtx_readq(qts, CRIME + 0x28, &value), ==, MEMTX_OK);
    g_assert_cmphex(value & (1ULL << 5), ==, 0);
    g_assert_cmpint(memtx_writeq(qts, MACE_PERIPH + 0x18, 1ULL << 8),
                    ==, MEMTX_OK);
    g_assert_cmpint(memtx_readq(qts, CRIME + 0x28, &value), ==, MEMTX_OK);
    g_assert_cmphex(value & (1ULL << 5), ==, 1ULL << 5);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmpint(memtx_writeq(qts, CRIME + 0x18, 1ULL << 5),
                    ==, MEMTX_OK);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_set_irq_in(qts, "/machine/mace", "uart", 0, 1);
    g_assert_cmpint(memtx_writeq(qts, MACE_PERIPH + 0x18,
                                 (1ULL << 8) | (1ULL << 20)), ==, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(qts, CRIME + 0x18,
                                 (1ULL << 5) | (1ULL << 4)), ==, MEMTX_OK);
    g_assert_cmpint(memtx_readq(qts, MACE_PERIPH + 0x10, &value),
                    ==, MEMTX_OK);
    g_assert_cmphex(value & (1ULL << 20), ==, 1ULL << 20);
    g_assert_cmpint(memtx_readq(qts, CRIME + 0x28, &value), ==, MEMTX_OK);
    g_assert_cmphex(value & (1ULL << 4), ==, 1ULL << 4);
    qtest_set_irq_in(qts, "/machine/mace", "rtc", 0, 0);
    g_assert_cmpint(memtx_readq(qts, CRIME + 0x28, &value), ==, MEMTX_OK);
    g_assert_cmphex(value & (1ULL << 5), ==, 0);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_set_irq_in(qts, "/machine/mace", "uart", 0, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    /* PCI inputs have no storage functions in this profile. */
    qtest_set_irq_in(qts, "/machine/mace", "pci", 0, 1);
    qtest_set_irq_in(qts, "/machine/mace", "pci", 1, 1);
    g_assert_cmpint(memtx_readq(qts, CRIME + 0x28, &value), ==, MEMTX_OK);
    g_assert_cmphex(value & (3ULL << 8), ==, 0);
    g_assert_cmpint(memtx_writel(qts, MACE_PCI + 8, 3), ==, MEMTX_OK);
    g_assert_cmpint(memtx_readq(qts, CRIME + 0x28, &value), ==, MEMTX_OK);
    g_assert_cmphex(value & (3ULL << 8), ==, 3ULL << 8);
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_cmpint(memtx_writeq(qts, CRIME + 0x18, 3ULL << 8),
                    ==, MEMTX_OK);
    g_assert_true(qtest_get_irq(qts, 1));
    qtest_set_irq_in(qts, "/machine/mace", "pci", 0, 0);
    g_assert_true(qtest_get_irq(qts, 1));
    qtest_set_irq_in(qts, "/machine/mace", "pci", 1, 0);
    g_assert_false(qtest_get_irq(qts, 1));
    qtest_quit(qts);
}

static void test_rom_aliases(void)
{
    static const uint8_t image[] = { 0x12, 0x34, 0x56, 0xa7 };
    const uint64_t base = 0x1fc00000ULL;
    const uint32_t capacity = 512 * KiB;
    g_autofree char *dir = g_dir_make_tmp("o2-rom-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);
    g_autofree char *machine = g_strdup_printf(
        "o2-test,rom-image=%s,rom-size=%u,rom-offset=256,rom-pad-byte=238",
        path, capacity);
    QTestState *qts;

    g_assert_true(g_file_set_contents(path, (const char *)image,
                                      sizeof(image), NULL));
    qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s -m 64M -nodefaults "
                      "-accel tcg,thread=single -S",
                      machine);
    for (unsigned alias = 0; alias < 8; alias++) {
        uint64_t start = base + alias * capacity;

        g_assert_cmphex(qtest_readb(qts, start), ==, 0xee);
        g_assert_cmphex(qtest_readb(qts, start + 255), ==, 0xee);
        for (unsigned i = 0; i < sizeof(image); i++) {
            g_assert_cmphex(qtest_readb(qts, start + 256 + i), ==, image[i]);
        }
        g_assert_cmphex(qtest_readb(qts, start + 260), ==, 0xee);
        g_assert_cmphex(qtest_readb(qts, start + capacity - 1), ==, 0xee);
    }
    qtest_writeb(qts, base + 256, 0xa0);
    g_assert_cmphex(qtest_readb(qts, base + 256), ==, image[0]);
    qtest_quit(qts);
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_cached_data(void)
{
    const uint64_t data = RAM_FULL + 0x1000;
    const uint64_t code = RAM_FULL + 0x2000;
    const uint64_t result = RAM_FULL + 0x3000;
    const uint32_t instructions[] = {
        mips_mfc0(8, 12), mips_ori(8, 8, 0x80), mips_mtc0(8, 12),
        0, 0, 0, 0,
        mips_lui(8, 0x9800), mips_dsll32(8, 8),
        mips_lui(9, 0x4000), mips_daddu(8, 8, 9),
        mips_daddiu(8, 8, 0x1000),
        mips_lw(10, 8, 0),
        mips_lui(11, 0x5566), mips_ori(11, 11, 0x7788),
        mips_sw(11, 8, 0), mips_lw(12, 8, 0),
        mips_lui(16, 0x9000), mips_dsll32(16, 16),
        mips_daddu(16, 16, 9), mips_daddiu(16, 16, 0x3000),
        mips_sw(10, 16, 0), mips_sw(12, 16, 4),
        mips_ori(13, 0, 1), mips_sw(13, 16, 8),
        mips_branch(MIPS_OPC_BEQ, 0, 0, -1), 0,
    };
    uint8_t code_bytes[sizeof(instructions)];
    uint8_t data_bytes[4];
    SgiWait wait;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = qtest_initf("-M o2-test -cpu R10000 -m 64M -nodefaults "
                      "-accel tcg,thread=single -S "
                      "-device loader,addr=0xffffffffa0002000,cpu-num=0");
    for (unsigned i = 0; i < G_N_ELEMENTS(instructions); i++) {
        stl_be_p(code_bytes + 4 * i, instructions[i]);
    }
    qtest_memwrite(qts, code, code_bytes, sizeof(code_bytes));
    stl_be_p(data_bytes, 0x11223344);
    qtest_memwrite(qts, data, data_bytes, sizeof(data_bytes));
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");

    sgi_wait_start(&wait, "O2 R10000 cached store");
    while (qtest_readl(qts, result + 8) != 1) {
        sgi_wait_pause(&wait, 1000);
    }
    g_assert_cmphex(qtest_readl(qts, result), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, result + 4), ==, 0x55667788);
    qtest_memread(qts, data, data_bytes, sizeof(data_bytes));
    g_assert_cmphex(ldl_be_p(data_bytes), ==, 0x11223344);
    qtest_quit(qts);
}

static void assert_o2_rejected(const char *option, const char *value,
                               const char *message)
{
    const char *args[14];
    unsigned n = 0;

    args[n++] = qtest_qemu_binary(NULL);
    args[n++] = "-M";
    args[n++] = "o2-test";
    args[n++] = "-m";
    args[n++] = g_str_equal(option, "-m") ? value : "64M";
    args[n++] = "-display";
    args[n++] = "none";
    args[n++] = "-nodefaults";
    args[n++] = "-S";
    args[n++] = "-qmp";
    args[n++] = "stdio";
    if (!g_str_equal(option, "-m")) {
        args[n++] = option;
        args[n++] = value;
    }
    args[n] = NULL;

    assert_launch_rejected(args, message);
}

static void test_configuration(void)
{
    const char *usb[] = {
        qtest_qemu_binary(NULL), "-M", "o2-test,usb=on", "-m", "64M",
        "-nodefaults", "-display", "none", "-S", "-qmp", "stdio", NULL,
    };
    const char *rom_without_image[] = {
        qtest_qemu_binary(NULL), "-M", "o2-test,rom-size=524288", "-m",
        "64M", "-nodefaults", "-display", "none", "-S", "-qmp", "stdio",
        NULL,
    };
    const char *rom_without_layout[] = {
        qtest_qemu_binary(NULL), "-M", "o2-test,rom-image=unused", "-m",
        "64M", "-nodefaults", "-display", "none", "-S", "-qmp", "stdio",
        NULL,
    };

    assert_o2_rejected("-m", "128M", "o2-test requires 64 MiB");
    assert_o2_rejected("-m", "64M,slots=1,maxmem=128M",
                       "o2-test does not support memory slots");
    assert_o2_rejected("-cpu", "R12000", "Invalid CPU model: R12000");
    assert_o2_rejected("-smp", "2", "max CPUs");
    assert_o2_rejected("-kernel", "unused", "o2-test does not support -kernel");
    assert_o2_rejected("-bios", "unused", "o2-test does not support -bios");
    assert_launch_rejected(usb, "o2-test does not support USB devices");
    assert_launch_rejected(rom_without_image,
                           "o2-test ROM settings require rom-image");
    assert_launch_rejected(rom_without_layout,
                           "o2-test ROM requires rom-size, rom-offset and "
                           "rom-pad-byte");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/o2/ram-aliases", test_ram_aliases);
    qtest_add_func("/o2/reset-registers", test_reset_registers);
    qtest_add_func("/o2/interrupt-chain", test_interrupt_chain);
    qtest_add_func("/o2/rom-aliases", test_rom_aliases);
    qtest_add_func("/o2/cached-data", test_cached_data);
    qtest_add_func("/o2/configuration", test_configuration);
    return g_test_run();
}
