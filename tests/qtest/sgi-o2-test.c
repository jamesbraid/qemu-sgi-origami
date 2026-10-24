/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Synthetic O2 wiring profile; no board population or firmware is implied. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "memtx.h"
#include "qobject/qobject.h"
#include "qobject/qjson.h"
#include "sgi-launch.h"
#include "sgi-mips-emit.h"
#include "sn0-user-image.h"
#include "sgi-wait.h"
#include "sgi-migrate.h"

#include <sys/socket.h>
#include <netinet/in.h>

#define RAM_FULL 0x40000000ULL
#define RAM_HIGH 0x20000000ULL
#define RAM_NO_ECC 0x80000000ULL
#define RAM_SIZE (64 * MiB)
#define RAM_LARGE_FIT (128 * MiB)
#define RAM_BANK_SIZE (128 * MiB)
#define RAM_APPROX_LIMIT (32 * MiB)
#define CRIME 0x14000000ULL
#define CRIME_RENDER_STATUS 0x15004000ULL
#define GBE 0x16000000ULL
#define GBE_APERTURE_SIZE (16 * MiB)
#define GBE_VT_1 0x010004
#define MACE_PCI 0x1f080000ULL
#define MACE_ETH 0x1f280000ULL
#define AIC_PCI_VIEW 0x1a000000ULL
#define MACE_CFGADDR (MACE_PCI + 0xcf8)
#define MACE_CFGDATA (MACE_PCI + 0xcfc)
#define MACE_PERIPH 0x1f310000ULL
#define MACE_UART_A 0x1f390000ULL
#define MACE_DMA_CARD "/machine/mace-dma-fixture"
#define MACE_NATIVE_DMA 0x40000000ULL
#define O2_PCI_HIGH_VIEW 0x280000000ULL
#define O2_PCI_PIO_TEST_BAR 0x80001000ULL

typedef struct O2StorageTest {
    QTestState *qts;
    char *dir;
    char *rom;
    char *events;
    char *trace;
} O2StorageTest;

static QTestState *o2_start(void)
{
    return qtest_initf(MEMTX_TEST_DEVICE
                       "-M o2-test -cpu R10000 -m 64M -nodefaults "
                       "-accel tcg,thread=single -S");
}

static QTestState *o2_start_banked(void)
{
    return qtest_initf(MEMTX_TEST_DEVICE
                       "-M o2-test,bank-profile=8x128m-synthetic "
                       "-cpu R10000 -m 1G -nodefaults "
                       "-accel tcg,thread=single -S");
}

static QTestState *o2_start_banked_approx(void)
{
    return qtest_initf(MEMTX_TEST_DEVICE
                       "-M o2-test,bank-profile=8x128m-synthetic,"
                       "reset-low-ram-approx=on "
                       "-cpu R10000 -m 1G -nodefaults "
                       "-accel tcg,thread=single -S");
}

static QTestState *o2_start_empty_renderer_status(void)
{
    return qtest_initf(MEMTX_TEST_DEVICE
                       "-M o2-test,bank-profile=8x128m-synthetic,"
                       "reset-low-ram-approx=on,empty-renderer-status=on "
                       "-cpu R10000 -m 1G -nodefaults "
                       "-accel tcg,thread=single -S");
}

static void test_empty_renderer_status(void)
{
    QTestState *qts = o2_start_banked_approx();
    g_autoptr(QDict) machine_change = NULL;
    g_autoptr(QDict) child_change = NULL;
    g_autoptr(QDict) child = NULL;
    uint32_t word;

    g_assert_cmpint(memtx_readl(qts, CRIME_RENDER_STATUS, &word),
                    !=, MEMTX_OK);
    qtest_quit(qts);
    qts = o2_start_empty_renderer_status();
    g_assert_cmpint(memtx_readl(qts, CRIME_RENDER_STATUS, &word),
                    ==, MEMTX_OK);
    g_assert_cmphex(word, ==, 0x1e000000);
    child = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/crime',"
        "'property':'empty-renderer-status'}}");
    g_assert_true(qdict_get_bool(child, "return"));
    machine_change = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'empty-renderer-status','value':false}}");
    g_assert_nonnull(qdict_get_qdict(machine_change, "error"));
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/crime',"
        "'property':'realized','value':false}}");
    child_change = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/crime',"
        "'property':'empty-renderer-status','value':false}}");
    g_assert_nonnull(qdict_get_qdict(child_change, "error"));
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/crime',"
        "'property':'realized','value':true}}");
    qtest_system_reset(qts);
    g_assert_cmpint(memtx_readl(qts, CRIME_RENDER_STATUS, &word),
                    ==, MEMTX_OK);
    g_assert_cmphex(word, ==, 0x1e000000);
    qtest_quit(qts);
}

static void test_banked_approx_reset_ram(void)
{
    static const uint64_t views[] = { 0, RAM_FULL, RAM_NO_ECC };
    QTestState *qts = o2_start_banked_approx();
    uint8_t value;

    qtest_writeb(qts, RAM_FULL, 0x5a);
    qtest_writeb(qts, RAM_FULL + RAM_APPROX_LIMIT - 1, 0xa5);
    for (unsigned i = 0; i < ARRAY_SIZE(views); i++) {
        g_assert_cmphex(qtest_readb(qts, views[i]), ==, 0x5a);
        g_assert_cmphex(qtest_readb(qts,
                        views[i] + RAM_APPROX_LIMIT - 1), ==, 0xa5);
        g_assert_cmpint(memtx_readb(qts, views[i] + RAM_APPROX_LIMIT,
                                    &value), !=, MEMTX_OK);
    }
    g_assert_cmpint(memtx_readb(qts, RAM_HIGH, &value), !=, MEMTX_OK);
    qtest_quit(qts);
}

static void test_banked_approx_first_write(void)
{
    QTestState *qts = o2_start_banked_approx();
    uint8_t value;

    qtest_writeb(qts, RAM_FULL, 0x5a);
    g_assert_cmpint(memtx_writeb(qts, CRIME + 0x208, 0), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(qts, CRIME + 0x209, 0), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL), ==, 0x5a);
    g_assert_cmpint(memtx_writeq(qts, CRIME + 0x208, 0), ==, MEMTX_OK);
    g_assert_cmpint(memtx_readb(qts, RAM_FULL, &value), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, CRIME + 0x208), ==, 0);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL), ==, 0x5a);
    qtest_quit(qts);

    qts = o2_start_banked_approx();
    qtest_writeb(qts, RAM_FULL, 0x68);
    g_assert_cmpint(memtx_writeq(qts, CRIME + 0x210, 1), ==, MEMTX_OK);
    g_assert_cmpint(memtx_readb(qts, RAM_FULL, &value), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(qts, CRIME + 0x208, 0x100), ==, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(qts, CRIME + 0x210, 0x104), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL), ==, 0x68);
    g_assert_cmphex(qtest_readb(qts, RAM_NO_ECC), ==, 0x68);
    qtest_writeq(qts, CRIME + 0x208, 0x104);
    qtest_writeq(qts, CRIME + 0x210, 0x100);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + RAM_BANK_SIZE), ==, 0x68);
    g_assert_cmphex(qtest_readb(qts, RAM_NO_ECC + RAM_BANK_SIZE), ==, 0x68);
    qtest_quit(qts);
}

static void test_banked_approx_bank_one_cpu_error(void)
{
    const uint64_t entry = RAM_FULL + RAM_BANK_SIZE + 0x2000;
    const uint64_t marker = RAM_FULL + RAM_BANK_SIZE + 0x3000;
    const char *expected = "unsupported 16 Mbit bank geometry at "
                           "RAM offset 0x100";
    SN0Image code = { 0 };
    g_autofree char *dir = NULL;
    g_autofree char *log = NULL;
    g_autofree char *contents = NULL;
    SgiWait wait;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    dir = g_dir_make_tmp("o2-bank-one-cpu-XXXXXX", NULL);
    log = g_build_filename(dir, "qemu.log", NULL);
    qts = qtest_initf(MEMTX_TEST_DEVICE
                      "-M o2-test,bank-profile=8x128m-synthetic,"
                      "reset-low-ram-approx=on -cpu R10000 -m 1G "
                      "-nodefaults -accel tcg,thread=single -S "
                      "-d guest_errors -D %s "
                      "-device loader,addr=0xffffffffa8002000,cpu-num=0",
                      log);
    {
        g_autofree char *registers = qtest_hmp(qts, "info registers");

        if (!strstr(registers, "pc=0xffffffffa8002000")) {
            g_error("O2 loader entry lost before execution: %s", registers);
        }
    }
    g_assert_cmpint(memtx_writeq(qts, CRIME + 0x210, 0x104), ==, MEMTX_OK);
    sn0_emit_mtc0(&code, SN0_R_ZERO, SN0_CP0_INDEX);
    sn0_emit_mtc0(&code, SN0_R_ZERO, SN0_CP0_PAGEMASK);
    sn0_emit_li(&code, SN0_R_T0, 0x00400000);
    sn0_emit_mtc0(&code, SN0_R_T0, SN0_CP0_ENTRYHI);
    /* Map UC=0 to bank zero while KSEG1 code and marker live in bank one. */
    sn0_emit_li(&code, SN0_R_T0, 0x01000017);
    sn0_emit_mtc0(&code, SN0_R_T0, SN0_CP0_ENTRYLO0);
    sn0_emit_li(&code, SN0_R_T0, 0x01000057);
    sn0_emit_mtc0(&code, SN0_R_T0, SN0_CP0_ENTRYLO1);
    sn0_emit_tlbwi(&code);
    sn0_emit_li(&code, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(&code, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit(&code, mips_lui(16, 0xa800));
    sn0_emit(&code, mips_ori(14, 0, 1));
    sn0_emit(&code, mips_sw(14, 16, 0x3000));
    sn0_emit(&code, mips_lui(8, 0x0040));
    sn0_emit(&code, mips_lw(10, 8, 0x100));
    sn0_emit(&code, mips_ori(14, 0, 2));
    sn0_emit(&code, mips_sw(14, 16, 0x3000));
    sn0_emit(&code, mips_branch(MIPS_OPC_BEQ, 0, 0, -1));
    sn0_emit_nop(&code);
    qtest_memwrite(qts, entry, code.bytes, code.pos);
    qtest_writel(qts, marker, 0);
    if (g_file_test(log, G_FILE_TEST_EXISTS)) {
        g_assert_true(g_file_get_contents(log, &contents, NULL, NULL));
        g_assert_null(strstr(contents, expected));
    }
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&wait, "O2 bank-one-first UC=0 geometry error");
    while (qtest_readl(qts, marker) != 1) {
        sgi_wait_pause(&wait, 1000);
    }
    for (;;) {
        g_clear_pointer(&contents, g_free);
        if (g_file_get_contents(log, &contents, NULL, NULL) &&
            strstr(contents, expected)) {
            break;
        }
        g_assert_cmphex(qtest_readl(qts, marker), !=, 2);
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    g_assert_cmphex(qtest_readl(qts, marker), ==, 1);
    qtest_quit(qts);
    g_assert_cmpint(unlink(log), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_banked_approx_uncached_ram(void)
{
    const uint64_t entry = RAM_FULL + 0x2000;
    const uint64_t target = RAM_FULL + 0x80;
    const uint64_t done = RAM_FULL + 0x3000;
    SN0Image trampoline = { 0 };
    SN0Image mapped_program = { 0 };
    const uint32_t program[] = {
        mips_lui(8, 0x0040), mips_daddiu(8, 8, 0x1000),
        mips_lui(10, 0x5a5a), mips_ori(10, 10, 0x3c3c),
        mips_sw(10, 8, 0), mips_lw(11, 8, 0),
        mips_lui(9, 0x0050), mips_daddiu(9, 9, 0x1ffc),
        mips_lui(12, 0x6b6b), mips_ori(12, 12, 0x4d4d),
        mips_sw(12, 9, 0), mips_lw(13, 9, 0),
        mips_lui(16, 0xa000), mips_daddiu(16, 16, 0x3000),
        mips_sw(11, 16, 4), mips_sw(13, 16, 8),
        mips_ori(14, 0, 1), mips_sw(14, 16, 0),
        mips_branch(MIPS_OPC_BEQ, 0, 0, -1), 0,
    };
    g_autofree char *jit = NULL;
    const char *metric;
    size_t average, maximum;
    SgiWait wait;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = qtest_initf(MEMTX_TEST_DEVICE
                      "-M o2-test,bank-profile=8x128m-synthetic,"
                      "reset-low-ram-approx=on "
                      "-cpu R10000 -m 1G -nodefaults "
                      "-accel tcg,thread=single -S "
                      "-device loader,addr=0xffffffffa0002000,cpu-num=0");
    sn0_emit_mtc0(&trampoline, SN0_R_ZERO, SN0_CP0_INDEX);
    sn0_emit_mtc0(&trampoline, SN0_R_ZERO, SN0_CP0_PAGEMASK);
    sn0_emit_li(&trampoline, SN0_R_T0, 0x00400000);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_ENTRYHI);
    /* C=2, UC=0: code and data both use CRIME's decoded RAM. */
    sn0_emit_li(&trampoline, SN0_R_T0, 0x01000017);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_ENTRYLO0);
    sn0_emit_li(&trampoline, SN0_R_T0, 0x01000057);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_ENTRYLO1);
    sn0_emit_tlbwi(&trampoline);
    sn0_emit_li(&trampoline, SN0_R_T0, 1);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_INDEX);
    sn0_emit_li(&trampoline, SN0_R_T0, 0x00500000);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_ENTRYHI);
    sn0_emit_li(&trampoline, SN0_R_T0, 0x0107ff97);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_ENTRYLO0);
    sn0_emit_li(&trampoline, SN0_R_T0, 0x0107ffd7);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_ENTRYLO1);
    sn0_emit_tlbwi(&trampoline);
    sn0_emit_li(&trampoline, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit_li(&trampoline, SN0_R_T0, 0x00400080);
    sn0_emit(&trampoline, mips_rtype(SN0_R_T0, 0, 0, 0, 0x08));
    sn0_emit_nop(&trampoline);
    for (unsigned i = 0; i < 64; i++) {
        sn0_emit_nop(&mapped_program);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(program); i++) {
        sn0_emit(&mapped_program, program[i]);
    }
    g_assert_cmpuint(mapped_program.pos, >, trampoline.pos);
    g_assert_cmpuint(mapped_program.pos, <, 4 * KiB - 0x80);
    qtest_memwrite(qts, entry, trampoline.bytes, trampoline.pos);
    qtest_memwrite(qts, target, mapped_program.bytes, mapped_program.pos);
    qtest_writel(qts, done, 0);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&wait, "O2 reset low-RAM UC=0 fetch/store");
    while (qtest_readl(qts, done) != 1) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    g_assert_cmphex(qtest_readl(qts, done + 4), ==, 0x5a5a3c3c);
    g_assert_cmphex(qtest_readl(qts, done + 8), ==, 0x6b6b4d4d);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x1000), ==, 0x5a5a3c3c);
    g_assert_cmphex(qtest_readl(qts, RAM_NO_ECC + 0x1000), ==, 0x5a5a3c3c);
    g_assert_cmphex(qtest_readl(qts, RAM_APPROX_LIMIT - 4), ==, 0x6b6b4d4d);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + RAM_APPROX_LIMIT - 4),
                    ==, 0x6b6b4d4d);
    g_assert_cmphex(qtest_readl(qts, RAM_NO_ECC + RAM_APPROX_LIMIT - 4),
                    ==, 0x6b6b4d4d);
    jit = qtest_hmp(qts, "info jit");
    metric = strstr(jit, "TB avg target size");
    g_assert_nonnull(metric);
    g_assert_cmpint(sscanf(metric, "TB avg target size  %zu max=%zu bytes",
                           &average, &maximum), ==, 2);
    g_assert_cmpuint(maximum, >, trampoline.pos);
    qtest_quit(qts);
}

static void o2_program_banks(QTestState *qts)
{
    for (unsigned bank = 0; bank < 8; bank++) {
        g_assert_cmpint(memtx_writeq(qts, CRIME + 0x208 + 8 * bank,
                                     0x100 | (bank << 2)), ==, MEMTX_OK);
    }
}

static void test_banked_registers(void)
{
    QTestState *qts = o2_start_banked();

    for (unsigned bank = 0; bank < 8; bank++) {
        uint64_t control = CRIME + 0x208 + 8 * bank;

        g_assert_cmphex(qtest_readq(qts, control), ==, bank);
        g_assert_cmpint(memtx_writeq(qts, control, 0x100 | (bank << 2)),
                        ==, MEMTX_OK);
        g_assert_cmphex(qtest_readq(qts, control), ==,
                        0x100 | (bank << 2));
    }
    qtest_writeb(qts, RAM_FULL, 0x51);
    g_assert_cmphex(qtest_readb(qts, RAM_NO_ECC), ==, 0x51);
    qtest_quit(qts);
}

static void test_banked_aliases(void)
{
    QTestState *qts = o2_start_banked();

    o2_program_banks(qts);
    for (unsigned bank = 0; bank < 8; bank++) {
        uint64_t offset = bank * RAM_BANK_SIZE;

        qtest_writeb(qts, RAM_FULL + offset, 0x10 + bank);
        qtest_writeb(qts, RAM_FULL + offset + RAM_BANK_SIZE - 1,
                     0x80 + bank);
    }
    for (unsigned bank = 0; bank < 8; bank++) {
        uint64_t offset = bank * RAM_BANK_SIZE;

        g_assert_cmphex(qtest_readb(qts, RAM_FULL + offset), ==,
                        0x10 + bank);
        g_assert_cmphex(qtest_readb(qts, RAM_NO_ECC + offset), ==,
                        0x10 + bank);
        g_assert_cmphex(qtest_readb(qts,
                        RAM_FULL + offset + RAM_BANK_SIZE - 1), ==,
                        0x80 + bank);
        g_assert_cmphex(qtest_readb(qts,
                        RAM_NO_ECC + offset + RAM_BANK_SIZE - 1), ==,
                        0x80 + bank);
        if (bank < 2) {
            g_assert_cmphex(qtest_readb(qts, offset), ==, 0x10 + bank);
            g_assert_cmphex(qtest_readb(qts,
                            offset + RAM_BANK_SIZE - 1), ==, 0x80 + bank);
        }
        if (bank >= 4) {
            uint64_t high = RAM_HIGH + (bank - 4) * RAM_BANK_SIZE;

            g_assert_cmphex(qtest_readb(qts, high), ==, 0x10 + bank);
            g_assert_cmphex(qtest_readb(qts, high + RAM_BANK_SIZE - 1),
                            ==, 0x80 + bank);
        }
    }
    qtest_quit(qts);
}

static void test_banked_remap(void)
{
    QTestState *qts = o2_start_banked();
    uint64_t status;
    uint64_t address;
    uint8_t byte;

    o2_program_banks(qts);
    qtest_writeb(qts, RAM_FULL, 0xa0);
    qtest_writeb(qts, RAM_FULL + RAM_BANK_SIZE, 0xb1);
    qtest_writeq(qts, CRIME + 0x210, 0x100);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL), ==, 0xa0);
    qtest_writeq(qts, CRIME + 0x208, 0x104);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL), ==, 0xb1);
    g_assert_cmphex(qtest_readb(qts, RAM_NO_ECC), ==, 0xb1);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + RAM_BANK_SIZE), ==, 0xa0);
    g_assert_cmphex(qtest_readb(qts, RAM_BANK_SIZE), ==, 0xa0);

    for (unsigned bank = 0; bank < 8; bank++) {
        uint64_t addr = RAM_FULL + bank * RAM_BANK_SIZE + 0x01000000;

        qtest_writeq(qts, CRIME + 0x208 + 8 * bank,
                     0x100 | (bank << 2));
        qtest_writeq(qts, addr, 0x11000000ULL + bank);
    }
    for (unsigned bank = 0; bank < 8; bank++) {
        qtest_writeq(qts, CRIME + 0x208 + 8 * bank, 0x104);
    }
    g_assert_cmpint(memtx_writeq(qts, RAM_FULL + 0x01000000,
                                 0x123456789abcdef0ULL), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readq(qts, CRIME + 0x250, &status), ==, MEMTX_OK);
    g_assert_cmphex(status & (1ULL << 26), ==, 1ULL << 26);
    g_assert_cmpint(memtx_readq(qts, CRIME + 0x258, &address), ==, MEMTX_OK);
    g_assert_cmphex(address, ==, 0x01000000);
    g_assert_cmpint(memtx_readb(qts, 0x01000000, &byte), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readb(qts, RAM_HIGH + 0x01000000, &byte),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_readb(qts, RAM_NO_ECC + 0x01000000, &byte),
                    !=, MEMTX_OK);
    for (unsigned bank = 0; bank < 8; bank++) {
        uint64_t addr = RAM_FULL + bank * RAM_BANK_SIZE + 0x01000000;

        qtest_writeq(qts, CRIME + 0x208 + 8 * bank,
                     0x100 | (bank << 2));
        g_assert_cmphex(qtest_readq(qts, addr), ==,
                        0x11000000ULL + bank);
    }
    g_assert_cmphex(qtest_readb(qts, RAM_FULL), ==, 0xa0);
    qtest_quit(qts);
}

static void test_banked_reset(void)
{
    QTestState *qts = o2_start_banked();
    uint64_t status;
    uint8_t value;

    o2_program_banks(qts);
    qtest_writeb(qts, RAM_FULL, 0x5a);
    qtest_system_reset(qts);
    for (unsigned bank = 0; bank < 8; bank++) {
        g_assert_cmphex(qtest_readq(qts, CRIME + 0x208 + 8 * bank),
                        ==, bank);
    }
    g_assert_cmpint(memtx_readb(qts, RAM_FULL, &value), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readq(qts, CRIME + 0x250, &status), ==, MEMTX_OK);
    g_assert_cmphex(status & ((1ULL << 25) | (1ULL << 26)), ==, 0);
    qtest_quit(qts);
}

static void test_banked_migration(void)
{
    QTestState *qts = o2_start_banked();
    g_autoptr(QDict) reply;
    QDict *error;

    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/crime',"
        "'property':'realized','value':false}}");
    reply = qtest_qmp(qts,
        "{'execute':'migrate','arguments':{'uri':'file:/dev/null'}}");
    error = qdict_get_qdict(reply, "error");

    g_assert_nonnull(error);
    if (!strstr(qdict_get_str(error, "desc"),
                "State blocked by non-migratable device "
                "'o2-test/eight-bank-profile'")) {
        g_error("unexpected migration error: %s",
                qdict_get_str(error, "desc"));
    }
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/crime',"
        "'property':'realized','value':true}}");
    qtest_quit(qts);
}

static void test_banked_incoming_migration(void)
{
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *qts;
    int64_t deadline;
    int fd;

    fd = g_file_open_tmp("o2-invalid-incoming-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, "invalid", 7), ==, 7);
    g_assert_cmpint(close(fd), ==, 0);
    uri = g_strdup_printf("file:%s", path);
    qts = qtest_initf(MEMTX_TEST_DEVICE
                      "-M o2-test,bank-profile=8x128m-synthetic "
                      "-cpu R10000 -m 1G -nodefaults "
                      "-accel tcg,thread=single -S -incoming defer");
    qtest_qmp_assert_success(qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s,"
        "'exit-on-error':false}}", uri);

    deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    for (;;) {
        g_autoptr(QDict) reply = qtest_qmp_assert_success_ref(qts,
            "{'execute':'query-migrate'}");
        const char *status = qdict_get_str(reply, "status");

        if (g_str_equal(status, "failed")) {
            const char *reason = qdict_get_try_str(reply, "error-desc");

            if (!reason ||
                !strstr(reason, "State blocked by non-migratable device "
                                "'o2-test/eight-bank-profile'")) {
                g_error("incoming migration bypassed O2 state guard: %s",
                        reason ?: "(no error description)");
            }
            break;
        }
        if (g_get_monotonic_time() >= deadline) {
            g_error("incoming migration did not fail: %s", status);
        }
        g_usleep(1000);
    }
    qtest_quit(qts);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_mace_ram_link(void)
{
    QTestState *qts = o2_start();
    g_autoptr(QDict) crime = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/crime',"
        "'property':'ram'}}");
    g_autoptr(QDict) mace = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/mace',"
        "'property':'ram'}}");
    const char *crime_ram = qdict_get_str(crime, "return");

    g_assert_nonnull(crime_ram);
    g_assert_cmpstr(qdict_get_str(mace, "return"), ==, crime_ram);
    qtest_quit(qts);
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

static void test_ram_128m_aliases(void)
{
    QTestState *qts = qtest_initf(MEMTX_TEST_DEVICE
        "-M o2-test -cpu R10000 -m 128M -nodefaults "
        "-accel tcg,thread=single -S");
    const uint64_t bases[] = { 0, RAM_FULL, RAM_NO_ECC };
    uint64_t value;
    uint8_t byte;

    for (unsigned source = 0; source < ARRAY_SIZE(bases); source++) {
        g_assert_cmpint(memtx_writeq(qts, bases[source] + RAM_LARGE_FIT - 8,
                                     0x0123456789abcdefULL + source),
                        ==, MEMTX_OK);
        for (unsigned view = 0; view < ARRAY_SIZE(bases); view++) {
            g_assert_cmpint(memtx_readq(qts, bases[view] +
                                            RAM_LARGE_FIT - 8, &value),
                            ==, MEMTX_OK);
            g_assert_cmphex(value, ==, 0x0123456789abcdefULL + source);
        }
    }
    for (unsigned source = 0; source < ARRAY_SIZE(bases); source++) {
        qtest_writeb(qts, bases[source] + RAM_LARGE_FIT - 1,
                     0xa0 + source);
        for (unsigned view = 0; view < ARRAY_SIZE(bases); view++) {
            g_assert_cmphex(qtest_readb(qts, bases[view] +
                                            RAM_LARGE_FIT - 1), ==,
                            0xa0 + source);
        }
    }
    for (unsigned view = 0; view < ARRAY_SIZE(bases); view++) {
        g_assert_cmpint(memtx_readb(qts, bases[view] + RAM_LARGE_FIT,
                                    &byte), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writeb(qts, bases[view] + RAM_LARGE_FIT,
                                     0xff), !=, MEMTX_OK);
    }
    g_assert_cmpint(memtx_readb(qts, 0x20000000, &byte), !=, MEMTX_OK);
    for (unsigned bank = 0; bank < 8; bank++) {
        g_assert_cmphex(qtest_readq(qts, CRIME + 0x208 + 8 * bank),
                        ==, bank);
        g_assert_cmpint(memtx_writeq(qts, CRIME + 0x208 + 8 * bank,
                                     0x100), !=, MEMTX_OK);
        g_assert_cmphex(qtest_readq(qts, CRIME + 0x208 + 8 * bank),
                        ==, bank);
    }
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + RAM_LARGE_FIT - 1),
                    ==, 0xa2);
    g_assert_cmpint(memtx_readb(qts, RAM_FULL + RAM_LARGE_FIT, &byte),
                    !=, MEMTX_OK);
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

static void assert_interrupt_chain(QTestState *qts)
{
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
}

static void test_interrupt_chain(void)
{
    QTestState *qts = o2_start();

    assert_interrupt_chain(qts);
    qtest_quit(qts);
}

static void test_banked_interrupt_chain(void)
{
    QTestState *qts = o2_start_banked();

    assert_interrupt_chain(qts);
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
        "o2-test,password-clear-input=0,rom-image=%s,rom-size=%u,rom-offset=256,rom-pad-byte=238",
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

static void test_banked_approx_rom_aliases(void)
{
    static const uint8_t image[] = { 0x12, 0x34, 0x56, 0xa7 };
    static const struct {
        uint32_t capacity;
        uint32_t offset;
    } cases[] = {
        { 512 * KiB, 0 },
        { 512 * KiB, 512 * KiB - sizeof(image) },
        { 1 * MiB, 256 },
        { 2 * MiB, 4096 },
    };
    g_autofree char *dir = g_dir_make_tmp("o2-approx-rom-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);

    g_assert_true(g_file_set_contents(path, (const char *)image,
                                      sizeof(image), NULL));
    for (unsigned c = 0; c < ARRAY_SIZE(cases); c++) {
        const uint32_t capacity = cases[c].capacity;
        const uint32_t offset = cases[c].offset;
        g_autofree char *machine = g_strdup_printf(
            "o2-test,bank-profile=8x128m-synthetic,"
            "reset-low-ram-approx=on,password-clear-input=0,rom-image=%s,rom-size=%u,"
            "rom-offset=%u,rom-pad-byte=238", path, capacity, offset);
        QTestState *qts = qtest_initf(MEMTX_TEST_DEVICE
                                      "-M %s -cpu R10000 -m 1G -nodefaults "
                                      "-accel tcg,thread=single -S", machine);

        for (unsigned alias = 0; alias < 4 * MiB / capacity; alias++) {
            uint64_t start = 0x1fc00000ULL + alias * capacity;

            if (offset) {
                g_assert_cmphex(qtest_readb(qts, start), ==, 0xee);
                g_assert_cmphex(qtest_readb(qts, start + offset - 1),
                                ==, 0xee);
            }
            for (unsigned i = 0; i < sizeof(image); i++) {
                g_assert_cmphex(qtest_readb(qts, start + offset + i),
                                ==, image[i]);
            }
            if (offset + sizeof(image) < capacity) {
                g_assert_cmphex(qtest_readb(qts,
                                start + offset + sizeof(image)), ==, 0xee);
                g_assert_cmphex(qtest_readb(qts, start + capacity - 1),
                                ==, 0xee);
            }
        }
        qtest_quit(qts);
    }
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_banked_approx_rom_first_fetch(void)
{
    const uint32_t program[] = {
        mips_lui(8, 0xa000), mips_ori(9, 0, 0x6d),
        mips_sw(9, 8, 0x3000),
        mips_branch(MIPS_OPC_BEQ, 0, 0, -1), 0,
    };
    uint8_t image[sizeof(program)];
    g_autofree char *dir = g_dir_make_tmp("o2-approx-fetch-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);
    g_autofree char *machine = NULL;
    SgiWait wait;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(program); i++) {
        stl_be_p(image + 4 * i, program[i]);
    }
    g_assert_true(g_file_set_contents(path, (const char *)image,
                                      sizeof(image), NULL));
    machine = g_strdup_printf(
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,password-clear-input=0,rom-image=%s,rom-size=524288,"
        "rom-offset=0,rom-pad-byte=238", path);
    qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s -cpu R10000 -m 1G "
                      "-nodefaults -accel tcg,thread=single -S", machine);
    qtest_writel(qts, RAM_FULL + 0x3000, 0);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&wait, "O2 synthetic reset-vector ROM fetch");
    while (qtest_readl(qts, RAM_FULL + 0x3000) != 0x6d) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    g_assert_cmphex(qtest_readl(qts, RAM_NO_ECC + 0x3000), ==, 0x6d);
    qtest_quit(qts);
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_cpu_reset_restarts_rom(void)
{
    const uint32_t program[] = {
        mips_lui(8, 0xa000), mips_ori(9, 0, 0x6d),
        mips_sw(9, 8, 0x3000),
        mips_branch(MIPS_OPC_BEQ, 0, 0, -1), 0,
    };
    uint8_t image[sizeof(program)];
    g_autofree char *dir = g_dir_make_tmp("o2-cpu-reset-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);
    g_autofree char *machine = NULL;
    SgiWait wait;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(program); i++) {
        stl_be_p(image + 4 * i, program[i]);
    }
    g_assert_true(g_file_set_contents(path, (const char *)image,
                                      sizeof(image), NULL));
    machine = g_strdup_printf(
        "o2-test,password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0,"
        "rom-pad-byte=238", path);
    qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s -cpu R10000 -m 64M "
                      "-nodefaults -accel tcg,thread=single -S", machine);
    qtest_writel(qts, RAM_FULL + 0x3000, 0);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&wait, "O2 first reset-ROM pass");
    while (qtest_readl(qts, RAM_FULL + 0x3000) != 0x6d) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    qtest_writel(qts, RAM_FULL + 0x3000, 0);
    qtest_system_reset(qts);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&wait, "O2 reset-ROM pass after QMP reset");
    while (qtest_readl(qts, RAM_FULL + 0x3000) != 0x6d) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_quit(qts);
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void assert_private_window_migration_blocked(QTestState *qts)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'migrate','arguments':{'uri':'file:/dev/null'}}");
    QDict *error = qdict_get_qdict(reply, "error");

    g_assert_nonnull(error);
    g_assert_nonnull(strstr(qdict_get_str(error, "desc"),
                            "o2-test/eight-bank-profile"));
}

static void run_private_window_backing_isolation(uint16_t offset, bool remap)
{
    const uint32_t program[] = {
        mips_mfc0(8, SN0_CP0_STATUS), mips_ori(8, 8, 0x80),
        mips_mtc0(8, SN0_CP0_STATUS), 0, 0, 0, 0,
        mips_ori(9, 0, 0x8000), mips_dsll32(9, 9),
        mips_dsll(9, 9, 16), mips_ori(9, 9, offset),
        mips_lui(16, 0xa000), mips_ori(16, 16, 0x3000),
        mips_lw(13, 9, 0), mips_sw(13, 16, 28),
        mips_ori(14, 0, 9), mips_sw(14, 16, 32),
        mips_lw(15, 16, 36),
        mips_branch(MIPS_OPC_BEQ, 15, 0, -2), 0,
        mips_lui(8, 0x8000), mips_ori(8, 8, offset),
        mips_lw(10, 8, 0), mips_sw(10, 16, 0),
        mips_lui(11, 0x5566), mips_ori(11, 11, 0x7788),
        mips_sw(11, 8, 0), mips_lw(12, 8, 0),
        mips_sw(12, 16, 4),
        mips_lw(13, 9, 0), mips_sw(13, 16, 8),
        mips_ori(14, 0, 1), mips_sw(14, 16, 12),
        mips_lw(15, 16, 16),
        mips_branch(MIPS_OPC_BEQ, 15, 0, -2), 0,
        mips_lw(12, 8, 0), mips_sw(12, 16, 20),
        mips_lw(13, 9, 0), mips_sw(13, 16, 24),
        mips_ori(14, 0, 2), mips_sw(14, 16, 12),
        mips_branch(MIPS_OPC_BEQ, 0, 0, -1), 0,
    };
    uint8_t image[sizeof(program)];
    uint8_t dma_bytes[4];
    uint32_t backing = remap ? 0x91e2d3c4 : 0x11223344;
    g_autofree char *dir = g_dir_make_tmp("o2-private-window-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);
    g_autofree char *log_path = g_build_filename(dir, "phase.log", NULL);
    g_autofree char *log = NULL;
    g_autofree char *machine = NULL;
    int64_t deadline;
    SgiWait wait;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(program); i++) {
        stl_be_p(image + 4 * i, program[i]);
    }
    g_assert_true(g_file_set_contents(path, (const char *)image,
                                      sizeof(image), NULL));
    machine = g_strdup_printf(
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,password-clear-input=0,rom-image=%s,rom-size=524288,"
        "rom-offset=0,rom-pad-byte=238", path);
    qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s -cpu R10000 -m 1G "
                      "-nodefaults -accel tcg,thread=single -S "
                      "-d trace:o2_private_phase -D %s",
                      machine, log_path);
    if (remap) {
        o2_program_banks(qts);
    }
    qtest_writel(qts, offset, 0x11223344);
    assert_private_window_migration_blocked(qts);
    g_assert_cmphex(qtest_readl(qts, offset), ==, 0x11223344);
    qtest_writel(qts, RAM_FULL + 0x300c, 0);
    qtest_writel(qts, RAM_FULL + 0x3010, 0);
    qtest_writel(qts, RAM_FULL + 0x3020, 0);
    qtest_writel(qts, RAM_FULL + 0x3024, 0);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&wait, "O2 nonselected CCA read before private entry");
    while (qtest_readl(qts, RAM_FULL + 0x3020) != 9) {
        sgi_wait_pause(&wait, 1000);
    }
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x301c), ==, 0x11223344);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_null(strstr(log, "event enter"));
    qtest_writel(qts, RAM_FULL + 0x3024, 1);
    sgi_wait_start(&wait, "O2 private cached CPU view");
    while (qtest_readl(qts, RAM_FULL + 0x300c) != 1) {
        sgi_wait_pause(&wait, 1000);
    }
    assert_private_window_migration_blocked(qts);
    if (remap) {
        uint8_t mailbox[40];

        qtest_memread(qts, RAM_FULL + 0x3000, mailbox, sizeof(mailbox));
        qtest_memwrite(qts, RAM_FULL + RAM_BANK_SIZE + 0x3000,
                       mailbox, sizeof(mailbox));
        qtest_writel(qts, RAM_FULL + RAM_BANK_SIZE + offset, backing);
        qtest_writeq(qts, CRIME + 0x210, 0x100);
        qtest_writeq(qts, CRIME + 0x208, 0x104);
    }
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3000), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3004), ==, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3008), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, offset), ==, backing);
    g_assert_cmpint(memtx_transaction(qts, MEMTX_DMA_AS, offset,
                                      dma_bytes, sizeof(dma_bytes), false),
                    ==, MEMTX_OK);
    g_assert_cmphex((uint32_t)ldl_be_p(dma_bytes), ==, backing);
    qtest_writel(qts, offset, 0xaabbccdd);
    g_assert_cmphex(qtest_readl(qts, offset), ==, 0xaabbccdd);
    g_assert_cmpint(memtx_transaction(qts, MEMTX_DMA_AS, offset,
                                      dma_bytes, sizeof(dma_bytes), false),
                    ==, MEMTX_OK);
    g_assert_cmphex((uint32_t)ldl_be_p(dma_bytes), ==, 0xaabbccdd);
    stl_be_p(dma_bytes, 0xcafebabe);
    g_assert_cmpint(memtx_transaction(qts, MEMTX_DMA_AS, offset,
                                      dma_bytes, sizeof(dma_bytes), true),
                    ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(qts, offset), ==, 0xcafebabe);
    qtest_writel(qts, RAM_FULL + 0x3010, 1);
    sgi_wait_start(&wait, "O2 external write beside private CPU view");
    while (qtest_readl(qts, RAM_FULL + 0x300c) != 2) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3014), ==, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3018), ==, 0xcafebabe);
    if (remap) {
        qtest_writeq(qts, CRIME + 0x208, 0x100);
        qtest_writeq(qts, CRIME + 0x210, 0x104);
    }
    qtest_writel(qts, offset, 0x01020304);
    qtest_writel(qts, RAM_FULL + 0x300c, 0);
    qtest_writel(qts, RAM_FULL + 0x3010, 0);
    qtest_writel(qts, RAM_FULL + 0x3020, 0);
    qtest_writel(qts, RAM_FULL + 0x3024, 0);
    qtest_system_reset(qts);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&wait, "O2 nonselected CCA read after reset");
    while (qtest_readl(qts, RAM_FULL + 0x3020) != 9) {
        sgi_wait_pause(&wait, 1000);
    }
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x301c), ==, 0x01020304);
    g_clear_pointer(&log, g_free);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    {
        const char *first_entry = strstr(log, "event enter");

        g_assert_nonnull(first_entry);
        g_assert_null(strstr(first_entry + 1, "event enter"));
    }
    qtest_writel(qts, RAM_FULL + 0x3024, 1);
    sgi_wait_start(&wait, "O2 private view after reset");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (qtest_readl(qts, RAM_FULL + 0x300c) != 1) {
        if (g_get_monotonic_time() >= deadline) {
            g_autofree char *registers = qtest_hmp(qts, "info registers");

            g_clear_pointer(&log, g_free);
            g_file_get_contents(log_path, &log, NULL, NULL);
            g_error("O2 private reset did not reach ROM checkpoint: %s; "
                    "phase log: %s", registers ?: "(no registers)",
                    log ?: "(no phase log)");
        }
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3000), ==, 0x01020304);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3004), ==, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3008), ==, 0x01020304);
    g_assert_cmphex(qtest_readl(qts, offset), ==, 0x01020304);
    qtest_quit(qts);
    g_clear_pointer(&log, g_free);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "event enter"));
    g_assert_nonnull(strstr(log, "unspecified 1"));
    g_assert_cmpint(unlink(log_path), ==, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_private_window_backing_isolation(void)
{
    run_private_window_backing_isolation(0x40, false);
}

static void test_private_window_boundary(void)
{
    run_private_window_backing_isolation(0, false);
    run_private_window_backing_isolation(0xffc, false);
    run_private_window_backing_isolation(0x1000, false);
    run_private_window_backing_isolation(0x1004, false);
}

static void test_private_window_backing_remap(void)
{
    run_private_window_backing_isolation(0x40, true);
}

static void test_private_window_other_cache_algorithms(void)
{
    SN0Image code = { 0 };
    g_autofree char *dir = g_dir_make_tmp("o2-private-cca-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);
    g_autofree char *log_path = g_build_filename(dir, "phase.log", NULL);
    g_autofree char *log = NULL;
    g_autofree char *machine = NULL;
    unsigned wait_loop, branch, program_end;
    SgiWait wait;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    sn0_emit_mfc0(&code, 8, SN0_CP0_STATUS);
    sn0_emit(&code, mips_ori(8, 8, 0x80)); /* Keep BEV and ERL; enable KX. */
    sn0_emit_mtc0(&code, 8, SN0_CP0_STATUS);
    sn0_emit_li64(&code, 16, UINT64_C(0xffffffffa0003000));
    sn0_emit_li64(&code, 8, UINT64_C(0xffffffffbfc00400));
    sn0_emit(&code, mips_rtype(8, 0, 0, 0, 0x08));
    sn0_emit_nop(&code);
    g_assert_cmpuint(code.pos, <=, SN0_VEC_TLB_REFILL);
    code.pos = 0x400;
    sn0_emit_li64(&code, 8, UINT64_C(0x9000000000000040));
    sn0_emit_li(&code, 9, 0x22223333);
    sn0_emit(&code, mips_sw(9, 8, 0));
    sn0_emit(&code, mips_ori(14, 0, 1));
    sn0_emit(&code, mips_sw(14, 16, 0));
    wait_loop = code.pos;
    sn0_emit(&code, mips_lw(15, 16, 4));
    branch = sn0_emit_branch(&code, SN0_OPC_BEQ, 15, 0);
    sn0_patch_branch(&code, branch, wait_loop);
    sn0_emit_li64(&code, 8, UINT64_C(0xb800000000000040));
    sn0_emit_li(&code, 9, 0x33334444);
    sn0_emit(&code, mips_sw(9, 8, 0));
    sn0_emit(&code, mips_ori(14, 0, 2));
    sn0_emit(&code, mips_sw(14, 16, 0));
    wait_loop = code.pos;
    sn0_emit(&code, mips_lw(15, 16, 4));
    sn0_emit(&code, mips_ori(14, 0, 2));
    branch = sn0_emit_branch(&code, SN0_OPC_BNE, 15, 14);
    sn0_patch_branch(&code, branch, wait_loop);
    sn0_emit_li64(&code, 8, UINT64_C(0xffffffff80000040));
    sn0_emit(&code, mips_lw(10, 8, 0));
    sn0_emit(&code, mips_sw(10, 16, 8));
    sn0_emit_li(&code, 11, 0x55556666);
    sn0_emit(&code, mips_sw(11, 8, 0));
    sn0_emit(&code, mips_lw(12, 8, 0));
    sn0_emit(&code, mips_sw(12, 16, 12));
    sn0_emit_li64(&code, 8, UINT64_C(0xffffffffa0000040));
    sn0_emit(&code, mips_lw(13, 8, 0));
    sn0_emit(&code, mips_sw(13, 16, 16));
    for (unsigned cca = 0; cca < 8; cca++) {
        if (cca == 3) {
            continue;
        }
        sn0_emit_li64(&code, 8, UINT64_C(0x8000000000000040) |
                      ((uint64_t)cca << 59));
        sn0_emit_li(&code, 11, 0x66660000 | cca);
        sn0_emit(&code, mips_sw(11, 8, 0));
        sn0_emit(&code, mips_lw(12, 8, 0));
        sn0_emit(&code, mips_sw(12, 16, 32 + 4 * cca));
        sn0_emit_li64(&code, 8, UINT64_C(0xffffffff80000040));
        sn0_emit(&code, mips_lw(12, 8, 0));
        sn0_emit(&code, mips_sw(12, 16, 64 + 4 * cca));
    }
    sn0_emit_li64(&code, 8, UINT64_C(0xffffffff80001008));
    sn0_emit_li(&code, 11, 0x77778888);
    sn0_emit(&code, mips_sw(11, 8, 0));
    sn0_emit_li64(&code, 8, UINT64_C(0xffffffffa0001008));
    sn0_emit(&code, mips_lw(13, 8, 0));
    sn0_emit(&code, mips_sw(13, 16, 20));
    sn0_emit(&code, mips_ori(14, 0, 3));
    sn0_emit(&code, mips_sw(14, 16, 0));
    sn0_emit_park(&code);
    program_end = code.pos;
    code.pos = SN0_VEC_GENERAL;
    sn0_emit_li64(&code, 16, UINT64_C(0xffffffffa0003000));
    sn0_emit_mfc0(&code, 10, SN0_CP0_CAUSE);
    sn0_emit(&code, mips_sw(10, 16, 28));
    sn0_emit(&code, mips_ori(14, 0, 1));
    sn0_emit(&code, mips_sw(14, 16, 24));
    sn0_emit_park(&code);
    g_assert_cmpuint(code.pos, <=, 0x400);
    code.pos = program_end;

    g_assert_true(g_file_set_contents(path, (const char *)code.bytes,
                                      code.pos, NULL));
    machine = g_strdup_printf(
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,password-clear-input=0,rom-image=%s,rom-size=524288,"
        "rom-offset=0,rom-pad-byte=238", path);
    qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s -cpu R10000 -m 1G "
                      "-nodefaults -accel tcg,thread=single -S "
                      "-d trace:o2_private_phase -D %s",
                      machine, log_path);
    qtest_writel(qts, 0x40, 0x11223344);
    qtest_writel(qts, RAM_FULL + 0x3000, 0);
    qtest_writel(qts, RAM_FULL + 0x3004, 0);
    qtest_writel(qts, RAM_FULL + 0x3018, 0);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&wait, "O2 C=2 backing write");
    while (qtest_readl(qts, RAM_FULL + 0x3000) != 1) {
        if (qtest_readl(qts, RAM_FULL + 0x3018)) {
            g_error("O2 cache-algorithm exception: Cause 0x%08x",
                    qtest_readl(qts, RAM_FULL + 0x301c));
        }
        sgi_wait_pause(&wait, 1000);
    }
    g_assert_cmphex(qtest_readl(qts, 0x40), ==, 0x22223333);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_null(strstr(log, "event enter"));
    qtest_writel(qts, RAM_FULL + 0x3004, 1);
    sgi_wait_start(&wait, "O2 C=7 backing write");
    while (qtest_readl(qts, RAM_FULL + 0x3000) != 2) {
        if (qtest_readl(qts, RAM_FULL + 0x3018)) {
            g_error("O2 cache-algorithm exception: Cause 0x%08x",
                    qtest_readl(qts, RAM_FULL + 0x301c));
        }
        sgi_wait_pause(&wait, 1000);
    }
    g_assert_cmphex(qtest_readl(qts, 0x40), ==, 0x33334444);
    g_clear_pointer(&log, g_free);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_null(strstr(log, "event enter"));
    qtest_writel(qts, RAM_FULL + 0x3004, 2);
    sgi_wait_start(&wait, "O2 C=3 entry and byte bound");
    while (qtest_readl(qts, RAM_FULL + 0x3000) != 3) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3008), ==, 0x33334444);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x300c), ==, 0x55556666);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3010), ==, 0x33334444);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3014), ==, 0x77778888);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3018), ==, 0);
    for (unsigned cca = 0; cca < 8; cca++) {
        if (cca == 3) {
            continue;
        }
        g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3020 + 4 * cca),
                        ==, 0x66660000 | cca);
        g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3040 + 4 * cca),
                        ==, 0x55556666);
    }
    g_assert_cmphex(qtest_readl(qts, 0x40), ==, 0x66660007);
    g_assert_cmphex(qtest_readl(qts, 0x1008), ==, 0x77778888);
    qtest_quit(qts);
    g_clear_pointer(&log, g_free);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "event enter"));
    g_assert_cmpint(unlink(log_path), ==, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void run_private_window_missed_rom_trigger(bool wait_for_poll)
{
    SN0Image rom = { 0 };
    SN0Image ram = { 0 };
    g_autofree char *dir = g_dir_make_tmp("o2-private-missed-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);
    g_autofree char *log_path = g_build_filename(dir, "phase.log", NULL);
    g_autofree char *log = NULL;
    g_autofree char *machine = NULL;
    unsigned wait_loop, branch;
    int64_t deadline;
    SgiWait wait;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    sn0_emit_li64(&rom, 8, UINT64_C(0xffffffffa0005000));
    sn0_emit(&rom, mips_rtype(8, 0, 0, 0, 0x08));
    sn0_emit_nop(&rom);
    sn0_emit_li64(&ram, 16, UINT64_C(0xffffffffa0003000));
    if (wait_for_poll) {
        sn0_emit(&ram, mips_ori(14, 0, 1));
        sn0_emit(&ram, mips_sw(14, 16, 4));
        wait_loop = ram.pos;
        sn0_emit(&ram, mips_lw(15, 16, 8));
        branch = sn0_emit_branch(&ram, SN0_OPC_BEQ, 15, 0);
        sn0_patch_branch(&ram, branch, wait_loop);
    }
    sn0_emit_li64(&ram, 8, UINT64_C(0xffffffff80000040));
    sn0_emit_li(&ram, 9, 0x55667788);
    sn0_emit(&ram, mips_sw(9, 8, 0));
    sn0_emit(&ram, mips_lw(10, 8, 0));
    sn0_emit(&ram, mips_sw(10, 16, 0));
    sn0_emit(&ram, mips_ori(14, 0, wait_for_poll ? 2 : 1));
    sn0_emit(&ram, mips_sw(14, 16, 4));
    sn0_emit_park(&ram);
    g_assert_true(g_file_set_contents(path, (const char *)rom.bytes,
                                      rom.pos, NULL));
    machine = g_strdup_printf(
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,password-clear-input=0,rom-image=%s,rom-size=524288,"
        "rom-offset=0,rom-pad-byte=238", path);
    qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s -cpu R10000 -m 1G "
                      "-nodefaults -accel tcg,thread=single -S "
                      "-d trace:o2_private_phase -D %s",
                      machine, log_path);
    qtest_memwrite(qts, RAM_FULL + 0x5000, ram.bytes, ram.pos);
    qtest_writel(qts, 0x40, 0x11223344);
    qtest_writel(qts, RAM_FULL + 0x3004, 0);
    qtest_writel(qts, RAM_FULL + 0x3008, 0);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    if (wait_for_poll) {
        sgi_wait_start(&wait, "O2 outside-ROM checkpoint before C=3");
        while (qtest_readl(qts, RAM_FULL + 0x3004) != 1) {
            sgi_wait_pause(&wait, 1000);
        }
        g_assert_cmphex(qtest_readl(qts, 0x40), ==, 0x11223344);
        sgi_wait_start(&wait, "O2 observed ROM exit before C=3");
        deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
        for (;;) {
            g_clear_pointer(&log, g_free);
            g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
            if (strstr(log, "event missed")) {
                break;
            }
            if (g_get_monotonic_time() >= deadline) {
                g_autofree char *registers = qtest_hmp(qts, "info registers");

                g_error("O2 ROM exit poll was not observed: %s; %s",
                        registers ?: "(no registers)", log);
            }
            sgi_wait_pause(&wait, 1000);
        }
        g_assert_null(strstr(log, "event enter"));
        qtest_writel(qts, RAM_FULL + 0x3008, 1);
    }
    sgi_wait_start(&wait, "O2 C=3 access after leaving reset ROM");
    while (qtest_readl(qts, RAM_FULL + 0x3004) !=
           (wait_for_poll ? 2 : 1)) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3000), ==, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, 0x40), ==, 0x55667788);
    qtest_quit(qts);
    g_clear_pointer(&log, g_free);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "event missed"));
    g_assert_null(strstr(log, "event enter"));
    g_assert_cmpint(unlink(log_path), ==, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_private_window_missed_rom_trigger(void)
{
    run_private_window_missed_rom_trigger(false);
}

static void test_private_window_missed_rom_poll(void)
{
    run_private_window_missed_rom_trigger(true);
}

static void test_private_window_low_fetch_report(void)
{
    SN0Image rom = { 0 };
    SN0Image low = { 0 };
    SN0Image function = { 0 };
    g_autofree char *dir = g_dir_make_tmp("o2-private-fetch-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);
    g_autofree char *log_path = g_build_filename(dir, "phase.log", NULL);
    g_autofree char *log = NULL;
    g_autofree char *machine = NULL;
    unsigned wait_loop, branch;
    int64_t deadline;
    SgiWait wait;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    sn0_emit_li64(&rom, 8, UINT64_C(0xffffffff80000040));
    sn0_emit(&rom, mips_lw(9, 8, 0));
    sn0_emit_li64(&rom, 8, UINT64_C(0xffffffff80000080));
    sn0_emit(&rom, mips_rtype(8, 0, 0, 0, 0x08));
    sn0_emit_nop(&rom);

    sn0_emit_li64(&low, 16, UINT64_C(0xffffffffa0003000));
    sn0_emit_li64(&low, 8, UINT64_C(0xffffffff80000800));
    sn0_emit(&low, mips_rtype(8, 0, 31, 0, 0x09));
    sn0_emit_nop(&low);
    sn0_emit(&low, mips_sw(2, 16, 16));
    sn0_emit(&low, mips_ori(14, 0, 1));
    sn0_emit(&low, mips_sw(14, 16, 0));
    wait_loop = low.pos;
    sn0_emit(&low, mips_lw(15, 16, 4));
    branch = sn0_emit_branch(&low, SN0_OPC_BEQ, 15, 0);
    sn0_patch_branch(&low, branch, wait_loop);
    sn0_emit_li64(&low, 8, UINT64_C(0xffffffff80000800));
    sn0_emit_li(&low, 11, mips_ori(2, 0, 0x22));
    sn0_emit(&low, mips_sw(11, 8, 0));
    sn0_emit(&low, mips_rtype(8, 0, 31, 0, 0x09));
    sn0_emit_nop(&low);
    sn0_emit(&low, mips_sw(2, 16, 20));
    sn0_emit_li64(&low, 8, UINT64_C(0xffffffff80000040));
    sn0_emit_li(&low, 11, 0x55667788);
    sn0_emit(&low, mips_sw(11, 8, 0));
    sn0_emit(&low, mips_lw(12, 8, 0));
    sn0_emit(&low, mips_sw(12, 16, 8));
    sn0_emit_li64(&low, 8, UINT64_C(0xffffffffa0000040));
    sn0_emit(&low, mips_lw(12, 8, 0));
    sn0_emit(&low, mips_sw(12, 16, 12));
    sn0_emit(&low, mips_ori(14, 0, 2));
    sn0_emit(&low, mips_sw(14, 16, 0));
    sn0_emit_park(&low);
    sn0_emit(&function, mips_ori(2, 0, 0x11));
    sn0_emit(&function, mips_rtype(31, 0, 0, 0, 0x08));
    sn0_emit_nop(&function);

    g_assert_true(g_file_set_contents(path, (const char *)rom.bytes,
                                      rom.pos, NULL));
    machine = g_strdup_printf(
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,password-clear-input=0,rom-image=%s,rom-size=524288,"
        "rom-offset=0,rom-pad-byte=238", path);
    qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s -cpu R10000 -m 1G "
                      "-nodefaults -accel tcg,thread=single -S "
                      "-d in_asm,trace:o2_private_phase -D %s",
                      machine, log_path);
    qtest_writel(qts, 0x40, 0x11223344);
    qtest_memwrite(qts, 0x80, low.bytes, low.pos);
    qtest_memwrite(qts, 0x800, function.bytes, function.pos);
    qtest_writel(qts, RAM_FULL + 0x3000, 0);
    qtest_writel(qts, RAM_FULL + 0x3004, 0);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&wait, "O2 private low-window instruction fetch");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (qtest_readl(qts, RAM_FULL + 0x3000) != 1) {
        if (g_get_monotonic_time() >= deadline) {
            g_autofree char *registers = qtest_hmp(qts, "info registers");

            g_error("O2 private low-window fetch did not execute: %s",
                    registers ?: "(registers unavailable)");
        }
        sgi_wait_pause(&wait, 1000);
    }
    sgi_wait_start(&wait, "O2 unsupported private low-window execution");
    for (;;) {
        g_clear_pointer(&log, g_free);
        g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
        if (strstr(log, "event low-exec")) {
            break;
        }
        sgi_wait_pause(&wait, 1000);
    }
    qtest_writel(qts, RAM_FULL + 0x3004, 1);
    sgi_wait_start(&wait, "O2 private view retained after low fetch");
    while (qtest_readl(qts, RAM_FULL + 0x3000) != 2) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3008), ==, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x300c), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, 0x40), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3010), ==, 0x11);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3014), ==, 0x22);
    g_assert_cmphex(qtest_readl(qts, 0x800), ==, mips_ori(2, 0, 0x11));
    qtest_quit(qts);
    g_clear_pointer(&log, g_free);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_null(strstr(log, "event release"));
    {
        g_auto(GStrv) blocks = g_strsplit(log, "----------------\n", -1);
        bool ram_block = false;

        /* A ROM block cannot prove that low private code resolves to RAM. */
        for (unsigned i = 0; blocks[i]; i++) {
            if (strstr(blocks[i], "0xffffffff80000800:") &&
                strstr(blocks[i], "0xffffffff80000804:")) {
                ram_block = true;
                break;
            }
        }
        g_assert_true(ram_block);
    }
    g_assert_cmpint(unlink(log_path), ==, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_private_window_uncached_low_fetch_release(void)
{
    SN0Image rom = { 0 };
    SN0Image low = { 0 };
    g_autofree char *dir = g_dir_make_tmp("o2-private-uncached-fetch-XXXXXX",
                                          NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);
    g_autofree char *log_path = g_build_filename(dir, "phase.log", NULL);
    g_autofree char *log = NULL;
    g_autofree char *machine = NULL;
    unsigned wait_loop, delay_loop, branch;
    int64_t deadline;
    SgiWait wait;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    sn0_emit_li64(&rom, 29, UINT64_C(0xffffffff80001000));
    sn0_emit_li64(&rom, 8, UINT64_C(0xffffffff80000040));
    sn0_emit(&rom, mips_lw(9, 8, 0));
    sn0_emit_li(&rom, 10, 0x55667788);
    sn0_emit(&rom, mips_sw(10, 8, 0));
    sn0_emit_li64(&rom, 13, UINT64_C(0xffffffffa0000800));
    sn0_emit(&rom, mips_rtype(13, 0, 0, 0, 0x08));
    sn0_emit_nop(&rom);

    sn0_emit_li64(&low, 16, UINT64_C(0xffffffffa0003000));
    sn0_emit_li64(&low, 8, UINT64_C(0xffffffff80000040));
    sn0_emit(&low, mips_lw(12, 8, 0));
    sn0_emit(&low, mips_sw(12, 16, 8));
    sn0_emit_li(&low, 17, 1000000);
    delay_loop = low.pos;
    sn0_emit(&low, mips_daddiu(17, 17, -1));
    branch = sn0_emit_branch(&low, SN0_OPC_BNE, 17, 0);
    sn0_patch_branch(&low, branch, delay_loop);
    sn0_emit(&low, mips_ori(14, 0, 1));
    sn0_emit(&low, mips_sw(14, 16, 0));
    wait_loop = low.pos;
    sn0_emit(&low, mips_lw(15, 16, 4));
    sn0_emit(&low, mips_ori(14, 0, 1));
    branch = sn0_emit_branch(&low, SN0_OPC_BNE, 15, 14);
    sn0_patch_branch(&low, branch, wait_loop);
    sn0_emit_li64(&low, 11, UINT64_C(0xffffffffa0000040));
    sn0_emit(&low, mips_sw(10, 11, 0));
    sn0_emit_li(&low, 17, 1000000);
    delay_loop = low.pos;
    sn0_emit(&low, mips_daddiu(17, 17, -1));
    branch = sn0_emit_branch(&low, SN0_OPC_BNE, 17, 0);
    sn0_patch_branch(&low, branch, delay_loop);
    sn0_emit(&low, mips_ori(14, 0, 2));
    sn0_emit(&low, mips_sw(14, 16, 0));
    wait_loop = low.pos;
    sn0_emit(&low, mips_lw(15, 16, 4));
    sn0_emit(&low, mips_ori(14, 0, 2));
    branch = sn0_emit_branch(&low, SN0_OPC_BNE, 15, 14);
    sn0_patch_branch(&low, branch, wait_loop);
    sn0_emit_li64(&low, 29, UINT64_C(0xffffffffa0001000));
    sn0_emit(&low, mips_ori(14, 0, 3));
    sn0_emit(&low, mips_sw(14, 16, 0));
    wait_loop = low.pos;
    sn0_emit(&low, mips_lw(15, 16, 4));
    sn0_emit(&low, mips_ori(14, 0, 3));
    branch = sn0_emit_branch(&low, SN0_OPC_BNE, 15, 14);
    sn0_patch_branch(&low, branch, wait_loop);
    sn0_emit_li64(&low, 8, UINT64_C(0xffffffff80000040));
    sn0_emit_li(&low, 10, 0xaabbccdd);
    sn0_emit(&low, mips_sw(10, 8, 0));
    sn0_emit_li64(&low, 11, UINT64_C(0xffffffffa0000040));
    sn0_emit(&low, mips_lw(12, 11, 0));
    sn0_emit(&low, mips_sw(12, 16, 8));
    sn0_emit(&low, mips_ori(14, 0, 4));
    sn0_emit(&low, mips_sw(14, 16, 0));
    sn0_emit_park(&low);

    g_assert_true(g_file_set_contents(path, (const char *)rom.bytes,
                                      rom.pos, NULL));
    machine = g_strdup_printf(
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,password-clear-input=0,rom-image=%s,rom-size=524288,"
        "rom-offset=0,rom-pad-byte=238", path);
    qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s -cpu R10000 -m 1G "
                      "-nodefaults -accel tcg,thread=single -S "
                      "-icount shift=0,align=off,sleep=off "
                      "-d trace:o2_private_phase -D %s",
                      machine, log_path);
    qtest_writel(qts, 0x40, 0x11223344);
    qtest_memwrite(qts, 0x800, low.bytes, low.pos);
    qtest_writel(qts, RAM_FULL + 0x3000, 0);
    qtest_writel(qts, RAM_FULL + 0x3004, 0);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&wait,
                   "O2 KSEG1 low fetch with mismatched bytes and KSEG0 SP");
    while (qtest_readl(qts, RAM_FULL + 0x3000) != 1) {
        sgi_wait_pause(&wait, 1000);
    }
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3008), ==, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, 0x40), ==, 0x11223344);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "event enter"));
    g_assert_null(strstr(log, "event release"));
    qtest_writel(qts, RAM_FULL + 0x3004, 1);
    sgi_wait_start(&wait, "O2 KSEG1 low fetch with equal bytes and KSEG0 SP");
    while (qtest_readl(qts, RAM_FULL + 0x3000) != 2) {
        sgi_wait_pause(&wait, 1000);
    }
    g_assert_cmphex(qtest_readl(qts, 0x40), ==, 0x55667788);
    g_clear_pointer(&log, g_free);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_null(strstr(log, "event release"));
    qtest_writel(qts, RAM_FULL + 0x3004, 2);
    sgi_wait_start(&wait, "O2 KSEG1 stack switch during KSEG1 low fetch");
    while (qtest_readl(qts, RAM_FULL + 0x3000) != 3) {
        sgi_wait_pause(&wait, 1000);
    }
    sgi_wait_start(&wait, "O2 release during KSEG1 low fetch");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    for (;;) {
        g_clear_pointer(&log, g_free);
        g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
        if (strstr(log, "event release")) {
            break;
        }
        if (g_get_monotonic_time() >= deadline) {
            g_autofree char *registers = qtest_hmp(qts, "info registers");

            g_error("O2 KSEG1 low fetch blocked release: %s; %s",
                    registers ?: "(no registers)", log);
        }
        sgi_wait_pause(&wait, 1000);
    }
    g_assert_null(strstr(log, "event low-exec"));
    g_assert_nonnull(strstr(log, "event release pc 0xffffffffa0000"));
    qtest_writel(qts, RAM_FULL + 0x3004, 3);
    sgi_wait_start(&wait, "O2 backing after uncached low fetch");
    while (qtest_readl(qts, RAM_FULL + 0x3000) != 4) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3008), ==, 0xaabbccdd);
    g_assert_cmphex(qtest_readl(qts, 0x40), ==, 0xaabbccdd);
    qtest_quit(qts);
    g_assert_cmpint(unlink(log_path), ==, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void run_private_window_guest_copy(bool omit_boundary, bool high_sp)
{
    SN0Image code = { 0 };
    const uint32_t ram_code[] = {
        mips_lui(16, 0xa000), mips_ori(16, 16, 0x3000),
        mips_ori(14, 0, 5), mips_sw(14, 16, 16),
        mips_branch(MIPS_OPC_BEQ, 0, 0, -1), 0,
    };
    uint8_t ram_bytes[sizeof(ram_code)];
    uint8_t empty[0x1008] = { 0 };
    g_autofree char *dir = g_dir_make_tmp("o2-private-copy-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);
    g_autofree char *log_path = g_build_filename(dir, "phase.log", NULL);
    g_autofree char *log = NULL;
    g_autofree char *machine = NULL;
    unsigned seed_loop, copy_loop, wait_loop, branch;
    unsigned copied = omit_boundary ? 512 : 513;
    bool check_code = !omit_boundary && !high_sp;
    const char *phase = omit_boundary ? "event incomplete" : "event release";
    int64_t deadline;
    SgiWait wait;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    sn0_emit_li64(&code, 29, UINT64_C(0xffffffff80001000));
    sn0_emit_li64(&code, 8, UINT64_C(0xffffffff80000000));
    sn0_emit_li64(&code, 9, UINT64_C(0xffffffffa0000000));
    sn0_emit_li64(&code, 16, UINT64_C(0xffffffffa0003000));
    sn0_emit_li(&code, 10, 513);
    sn0_emit_li(&code, 11, 0x11223344);
    seed_loop = code.pos;
    sn0_emit(&code, mips_sd(11, 8, 0));
    sn0_emit(&code, mips_daddiu(8, 8, 8));
    sn0_emit(&code, mips_daddiu(11, 11, 1));
    sn0_emit(&code, mips_daddiu(10, 10, -1));
    branch = sn0_emit_branch(&code, SN0_OPC_BNE, 10, 0);
    sn0_patch_branch(&code, branch, seed_loop);
    if (check_code) {
        sn0_emit_li64(&code, 8, UINT64_C(0xffffffff80000800));
        sn0_emit_li(&code, 11, mips_ori(2, 0, 0x11));
        sn0_emit(&code, mips_sw(11, 8, 0));
        sn0_emit_li(&code, 11, mips_rtype(31, 0, 0, 0, 0x08));
        sn0_emit(&code, mips_sw(11, 8, 4));
        sn0_emit(&code, mips_sw(0, 8, 8));
        sn0_emit(&code, mips_rtype(8, 0, 31, 0, 0x09));
        sn0_emit_nop(&code);
        sn0_emit(&code, mips_sw(2, 16, 20));
    }
    sn0_emit(&code, mips_ori(14, 0, 1));
    sn0_emit(&code, mips_sw(14, 16, 0));
    wait_loop = code.pos;
    sn0_emit(&code, mips_lw(15, 16, 4));
    sn0_emit(&code, mips_ori(14, 0, 1));
    branch = sn0_emit_branch(&code, SN0_OPC_BNE, 15, 14);
    sn0_patch_branch(&code, branch, wait_loop);
    sn0_emit_li64(&code, 8, UINT64_C(0xffffffff80000000));
    sn0_emit_li64(&code, 9, UINT64_C(0xffffffffa0000000));
    sn0_emit_li(&code, 10, copied);
    copy_loop = code.pos;
    sn0_emit(&code, mips_ld(12, 8, 0));
    sn0_emit(&code, mips_sd(12, 9, 0));
    sn0_emit(&code, mips_daddiu(8, 8, 8));
    sn0_emit(&code, mips_daddiu(9, 9, 8));
    sn0_emit(&code, mips_daddiu(10, 10, -1));
    branch = sn0_emit_branch(&code, SN0_OPC_BNE, 10, 0);
    sn0_patch_branch(&code, branch, copy_loop);
    sn0_emit(&code, mips_ori(14, 0, 2));
    sn0_emit(&code, mips_sw(14, 16, 0));
    wait_loop = code.pos;
    sn0_emit(&code, mips_lw(15, 16, 4));
    sn0_emit(&code, mips_ori(14, 0, 2));
    branch = sn0_emit_branch(&code, SN0_OPC_BNE, 15, 14);
    sn0_patch_branch(&code, branch, wait_loop);
    sn0_emit_li64(&code, 29, high_sp ?
                  UINT64_C(0xffffffffa2001000) :
                  UINT64_C(0xffffffffa0001000));
    sn0_emit(&code, mips_ori(14, 0, 3));
    sn0_emit(&code, mips_sw(14, 16, 0));
    wait_loop = code.pos;
    sn0_emit(&code, mips_lw(15, 16, 4));
    sn0_emit(&code, mips_ori(14, 0, 3));
    branch = sn0_emit_branch(&code, SN0_OPC_BNE, 15, 14);
    sn0_patch_branch(&code, branch, wait_loop);
    if (check_code) {
        sn0_emit_li64(&code, 8, UINT64_C(0xffffffff80000800));
        sn0_emit(&code, mips_rtype(8, 0, 31, 0, 0x09));
        sn0_emit_nop(&code);
        sn0_emit(&code, mips_sw(2, 16, 24));
    }
    sn0_emit_li64(&code, 8, UINT64_C(0xffffffff80000040));
    sn0_emit_li(&code, 11, 0xaabbccdd);
    sn0_emit(&code, mips_sw(11, 8, 0));
    sn0_emit_li64(&code, 9, UINT64_C(0xffffffffa0000040));
    sn0_emit(&code, mips_lw(12, 9, 0));
    sn0_emit(&code, mips_sw(12, 16, 8));
    sn0_emit_li64(&code, 8, UINT64_C(0xffffffffa0005000));
    sn0_emit(&code, mips_rtype(8, 0, 0, 0, 0x08));
    sn0_emit_nop(&code);

    for (unsigned i = 0; i < ARRAY_SIZE(ram_code); i++) {
        stl_be_p(ram_bytes + 4 * i, ram_code[i]);
    }
    g_assert_true(g_file_set_contents(path, (const char *)code.bytes,
                                      code.pos, NULL));
    machine = g_strdup_printf(
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,password-clear-input=0,rom-image=%s,rom-size=524288,"
        "rom-offset=0,rom-pad-byte=238", path);
    qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s -cpu R10000 -m 1G "
                      "-nodefaults -accel tcg,thread=single -S "
                      "-d trace:o2_private_phase -D %s",
                      machine, log_path);
    if (high_sp) {
        o2_program_banks(qts);
    }
    qtest_memwrite(qts, 0, empty, sizeof(empty));
    qtest_memwrite(qts, RAM_FULL + 0x5000,
                   ram_bytes, sizeof(ram_bytes));
    qtest_writel(qts, RAM_FULL + 0x3000, 0);
    qtest_writel(qts, RAM_FULL + 0x3004, 0);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&wait, "O2 private words seeded by guest");
    while (qtest_readl(qts, RAM_FULL + 0x3000) != 1) {
        sgi_wait_pause(&wait, 1000);
    }
    g_assert_cmphex(qtest_readq(qts, 0), ==, 0);
    g_assert_cmphex(qtest_readq(qts, 0xf10), ==, 0);
    g_assert_cmphex(qtest_readq(qts, 0x1000), ==, 0);
    qtest_writel(qts, RAM_FULL + 0x3004, 1);
    sgi_wait_start(&wait, "O2 guest copy to decoded RAM");
    while (qtest_readl(qts, RAM_FULL + 0x3000) != 2) {
        sgi_wait_pause(&wait, 1000);
    }
    for (unsigned i = 0; i < copied; i++) {
        uint64_t expected = UINT64_C(0x11223344) + i;

        if (check_code && i == 0x800 / 8) {
            expected = UINT64_C(0x3402001103e00008);
        }
        g_assert_cmphex(qtest_readq(qts, i * 8), ==, expected);
    }
    if (omit_boundary) {
        g_assert_cmphex(qtest_readq(qts, 0x1000), ==, 0);
    }
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_null(strstr(log, "event release"));
    qtest_writel(qts, RAM_FULL + 0x3004, 2);
    sgi_wait_start(&wait, "O2 KSEG1 stack switch");
    while (qtest_readl(qts, RAM_FULL + 0x3000) != 3) {
        sgi_wait_pause(&wait, 1000);
    }
    sgi_wait_start(&wait, "O2 private view %s",
                   omit_boundary ? "incomplete copy" : "release");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    for (;;) {
        g_clear_pointer(&log, g_free);
        g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
        if (strstr(log, phase)) {
            break;
        }
        if (g_get_monotonic_time() >= deadline) {
            g_autofree char *registers = qtest_hmp(qts, "info registers");

            g_error("O2 private phase event %s did not arrive: %s; %s",
                    phase, registers ?: "(no registers)", log);
        }
        sgi_wait_pause(&wait, 1000);
    }
    if (check_code) {
        g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3014), ==, 0x11);
        qtest_writel(qts, 0x800, mips_ori(2, 0, 0x22));
    }
    qtest_writel(qts, RAM_FULL + 0x3004, 3);
    sgi_wait_start(&wait, "O2 RAM execution after private phase check");
    while (qtest_readl(qts, RAM_FULL + 0x3010) != 5) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    if (check_code) {
        g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3018), ==, 0x22);
    }
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3008), ==,
                    omit_boundary ? 0 : 0xaabbccdd);
    g_assert_cmphex(qtest_readl(qts, 0x40), ==,
                    omit_boundary ? 0 : 0xaabbccdd);
    if (omit_boundary) {
        g_assert_cmphex(qtest_readq(qts, 0x40), ==,
                        UINT64_C(0x1122334c));
    }
    qtest_quit(qts);
    if (omit_boundary) {
        g_clear_pointer(&log, g_free);
        g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
        g_assert_null(strstr(log, "event release"));
    }
    g_assert_cmpint(unlink(log_path), ==, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_private_window_guest_copy_release(void)
{
    run_private_window_guest_copy(false, false);
}

static void test_private_window_incomplete_copy(void)
{
    run_private_window_guest_copy(true, false);
}

static void test_private_window_high_sp_release(void)
{
    run_private_window_guest_copy(false, true);
}

static void test_coherent_ram(void)
{
    const uint64_t data = RAM_FULL + 0x1000;
    const uint64_t code = RAM_FULL + 0x2000;
    const uint64_t result = RAM_FULL + 0x3000;
    const uint32_t instructions[] = {
        mips_lui(8, 0x8000),
        mips_daddiu(8, 8, 0x1000),
        mips_lw(10, 8, 0),
        mips_lui(11, 0x5566), mips_ori(11, 11, 0x7788),
        mips_sw(11, 8, 0), mips_lw(12, 8, 0),
        mips_lui(9, 0xa000), mips_daddiu(9, 9, 0x1000),
        mips_lw(13, 9, 0),
        mips_lui(16, 0xa000), mips_daddiu(16, 16, 0x3000),
        mips_sw(10, 16, 0), mips_sw(12, 16, 4),
        mips_sw(13, 16, 12),
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

    sgi_wait_start(&wait, "O2 R10000 coherent store");
    while (qtest_readl(qts, result + 8) != 1) {
        sgi_wait_pause(&wait, 1000);
    }
    g_assert_cmphex(qtest_readl(qts, result), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, result + 4), ==, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, result + 12), ==, 0x55667788);
    qtest_memread(qts, data, data_bytes, sizeof(data_bytes));
    g_assert_cmphex(ldl_be_p(data_bytes), ==, 0x55667788);
    qtest_quit(qts);
}

static void run_uncached_mmio(unsigned cca, unsigned uc)
{
    SN0Image rom = { 0 }, code = { 0 };
    g_autofree char *dir = g_dir_make_tmp("o2-uncached-mmio-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);
    uint64_t attributes = ((uint64_t)uc << 62) | (cca << 3) | 7;
    uint32_t outcome;
    SgiWait wait;
    QTestState *qts;

    sn0_emit_park(&rom);
    rom.pos = SN0_VEC_GENERAL;
    sn0_emit_li64(&rom, 16, UINT64_C(0xffffffffa0003000));
    sn0_emit_mfc0(&rom, 10, SN0_CP0_CAUSE);
    sn0_emit(&rom, mips_sw(10, 16, 4));
    sn0_emit_mfc0(&rom, 10, SN0_CP0_EPC);
    sn0_emit(&rom, mips_sw(10, 16, 8));
    sn0_emit(&rom, mips_ori(10, 0, 2));
    sn0_emit(&rom, mips_sw(10, 16, 0));
    sn0_emit_park(&rom);
    g_assert_true(g_file_set_contents(path, (const char *)rom.bytes,
                                      rom.pos, NULL));
    qts = qtest_initf(MEMTX_TEST_DEVICE
        "-M o2-test,bank-profile=8x128m-synthetic,"
        "crime-profile=legacy-software,reset-low-ram-approx=on,"
        "functional-memory-transfer=on,prom-private-window=on,"
        "prom-sb64-reset=on,gbe11-registers=on,prom-id-word=on,"
        "password-clear-input=0,rom-image=%s,rom-size=524288,"
        "rom-offset=0,rom-pad-byte=238 -cpu R10000 -m 1G -nodefaults "
        "-accel tcg,thread=single -S "
        "-device loader,addr=0xffffffffa0002000,cpu-num=0", path);
    g_assert_cmphex(qtest_readl(qts, GBE + 0x58000), ==, 0);
    g_assert_cmphex(qtest_readq(qts, MACE_PERIPH), ==, 1);
    sn0_emit_mtc0(&code, 0, SN0_CP0_INDEX);
    sn0_emit_mtc0(&code, 0, SN0_CP0_PAGEMASK);
    sn0_emit_li(&code, 8, 0x00400000);
    sn0_emit_mtc0(&code, 8, SN0_CP0_ENTRYHI);
    sn0_emit_li64(&code, 8, (UINT64_C(0x16058000) >> 6) | attributes);
    sn0_emit_dmtc0(&code, 8, SN0_CP0_ENTRYLO0);
    sn0_emit_li64(&code, 8, (UINT64_C(0x1f310000) >> 6) | attributes);
    sn0_emit_dmtc0(&code, 8, SN0_CP0_ENTRYLO1);
    sn0_emit_tlbwi(&code);
    sn0_emit_li(&code, 8, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(&code, 8, SN0_CP0_STATUS);
    sn0_emit_li64(&code, 16, UINT64_C(0xffffffffa0003000));
    sn0_emit_li(&code, 8, 0x00400000);
    sn0_emit(&code, mips_lw(10, 8, 0));
    sn0_emit(&code, mips_sw(10, 16, 12));
    sn0_emit(&code, mips_ld(11, 8, 0x1000));
    sn0_emit(&code, mips_sd(11, 16, 16));
    sn0_emit(&code, mips_ori(10, 0, 1));
    sn0_emit(&code, mips_sw(10, 16, 0));
    sn0_emit_park(&code);
    qtest_memwrite(qts, 0x2000, code.bytes, code.pos);
    qtest_writeq(qts, RAM_FULL + 0x1000, UINT64_C(0x91e2d3c456789abc));
    qtest_writel(qts, RAM_FULL + 0x3000, 0);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&wait, "O2 mapped MMIO CCA=%u UC=%u", cca, uc);
    while (!(outcome = qtest_readl(qts, RAM_FULL + 0x3000))) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    if (uc == 1 || uc == 2) {
        g_assert_cmpuint(outcome, ==, 2);
        g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3004) & 0x7c, ==, 7 << 2);
    } else {
        if (outcome != 1) {
            g_error("O2 mapped MMIO CCA=%u UC=%u raised Cause=0x%08x "
                    "EPC=0x%08x", cca, uc,
                    qtest_readl(qts, RAM_FULL + 0x3004),
                    qtest_readl(qts, RAM_FULL + 0x3008));
        }
        g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x300c), ==, 0);
        g_assert_cmphex(qtest_readq(qts, RAM_FULL + 0x3010), ==, 1);
    }
    g_assert_cmphex(qtest_readq(qts, 0x1000), ==,
                    UINT64_C(0x91e2d3c456789abc));
    g_assert_cmphex(qtest_readq(qts, RAM_NO_ECC + 0x1000), ==,
                    UINT64_C(0x91e2d3c456789abc));
    qtest_quit(qts);
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_uncached_mmio(void)
{
    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    for (unsigned cca = 2; cca <= 7; cca += 5) {
        for (unsigned uc = 0; uc < 4; uc++) {
            run_uncached_mmio(cca, uc);
        }
    }
}

static void test_uncached_window(void)
{
    const uint64_t entry = RAM_FULL + 0x2000;
    const uint64_t target = RAM_FULL + 0x80;
    const uint64_t data = RAM_FULL + 0x1000;
    const uint64_t done = RAM_FULL + 0x3000;
    SN0Image trampoline = { 0 };
    const uint32_t program[] = {
        mips_lui(8, 0x0040), mips_daddiu(8, 8, 0x1000),
        mips_lui(10, 0x5a5a), mips_ori(10, 10, 0x3c3c),
        mips_sw(10, 8, 0),
        mips_lui(16, 0xa000), mips_daddiu(16, 16, 0x3000),
        mips_ori(13, 0, 1), mips_sw(13, 16, 0),
        mips_branch(MIPS_OPC_BEQ, 0, 0, -1), 0,
    };
    uint8_t target_bytes[sizeof(program)];
    uint8_t word[4];
    uint64_t beyond;
    int64_t deadline;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = qtest_initf(MEMTX_TEST_DEVICE
                      "-M o2-test -cpu R10000 -m 64M -nodefaults "
                      "-accel tcg,thread=single -S "
                      "-device loader,addr=0xffffffffa0002000,cpu-num=0");
    sn0_emit_mtc0(&trampoline, SN0_R_ZERO, SN0_CP0_INDEX);
    sn0_emit_mtc0(&trampoline, SN0_R_ZERO, SN0_CP0_PAGEMASK);
    sn0_emit_li(&trampoline, SN0_R_T0, 0x00400000);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_ENTRYHI);
    /* PFN=0x40000000/0x40001000, C=2, UC=0, dirty, valid, global. */
    sn0_emit_li(&trampoline, SN0_R_T0, 0x01000017);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_ENTRYLO0);
    sn0_emit_li(&trampoline, SN0_R_T0, 0x01000057);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_ENTRYLO1);
    sn0_emit_tlbwi(&trampoline);
    sn0_emit_li(&trampoline, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit_li(&trampoline, SN0_R_T0, 0x00400080);
    sn0_emit(&trampoline, mips_rtype(SN0_R_T0, 0, 0, 0, 0x08));
    sn0_emit_nop(&trampoline);
    for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
        stl_be_p(target_bytes + 4 * i, program[i]);
    }
    qtest_memwrite(qts, entry, trampoline.bytes, trampoline.pos);
    qtest_memwrite(qts, target, target_bytes, sizeof(target_bytes));
    qtest_writel(qts, done, 0);
    stl_be_p(word, 0x11223344);
    qtest_memwrite(qts, data, word, sizeof(word));
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");

    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (qtest_readl(qts, done) != 1) {
        if (g_get_monotonic_time() >= deadline) {
            g_autofree char *registers = qtest_hmp(qts, "info registers");

            g_error("O2 C=2 RAM fetch/store did not complete: %s",
                    registers ?: "(registers unavailable)");
        }
        g_usleep(1000);
    }
    qtest_memread(qts, data, word, sizeof(word));
    g_assert_cmphex(ldl_be_p(word), ==, 0x5a5a3c3c);
    qtest_memread(qts, 0x1000, word, sizeof(word));
    g_assert_cmphex(ldl_be_p(word), ==, 0x5a5a3c3c);
    g_assert_cmpint(memtx_readq(qts, RAM_FULL + RAM_SIZE, &beyond),
                    !=, MEMTX_OK);
    qtest_quit(qts);
}

static void test_uncached_128m_boundary(void)
{
    const uint64_t entry = RAM_FULL + 0x2000;
    const uint64_t target = RAM_FULL + 0x80;
    const uint64_t done = RAM_FULL + 0x3000;
    SN0Image trampoline = { 0 };
    const uint32_t program[] = {
        mips_lui(8, 0x0040), mips_daddiu(8, 8, 0x1ffc),
        mips_lui(10, 0x5a5a), mips_ori(10, 10, 0x3c3c),
        mips_sw(10, 8, 0), mips_lw(11, 8, 0),
        mips_lui(16, 0xa000), mips_daddiu(16, 16, 0x3000),
        mips_sw(11, 16, 4),
        mips_ori(13, 0, 1), mips_sw(13, 16, 0),
        mips_branch(MIPS_OPC_BEQ, 0, 0, -1), 0,
    };
    uint8_t target_bytes[sizeof(program)];
    int64_t deadline;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = qtest_initf(MEMTX_TEST_DEVICE
                      "-M o2-test -cpu R10000 -m 128M -nodefaults "
                      "-accel tcg,thread=single -S "
                      "-device loader,addr=0xffffffffa0002000,cpu-num=0");
    sn0_emit_mtc0(&trampoline, SN0_R_ZERO, SN0_CP0_INDEX);
    sn0_emit_mtc0(&trampoline, SN0_R_ZERO, SN0_CP0_PAGEMASK);
    sn0_emit_li(&trampoline, SN0_R_T0, 0x00400000);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_ENTRYHI);
    /* UC=0 uses a separate processor view of the last fitted page. */
    sn0_emit_li(&trampoline, SN0_R_T0, 0x01000017);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_ENTRYLO0);
    sn0_emit_li(&trampoline, SN0_R_T0, 0x011fffd7);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_ENTRYLO1);
    sn0_emit_tlbwi(&trampoline);
    sn0_emit_li(&trampoline, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit_li(&trampoline, SN0_R_T0, 0x00400080);
    sn0_emit(&trampoline, mips_rtype(SN0_R_T0, 0, 0, 0, 0x08));
    sn0_emit_nop(&trampoline);
    for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
        stl_be_p(target_bytes + 4 * i, program[i]);
    }
    qtest_memwrite(qts, entry, trampoline.bytes, trampoline.pos);
    qtest_memwrite(qts, target, target_bytes, sizeof(target_bytes));
    qtest_writel(qts, done, 0);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");

    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (qtest_readl(qts, done) != 1) {
        if (g_get_monotonic_time() >= deadline) {
            g_autofree char *registers = qtest_hmp(qts, "info registers");

            g_error("O2 UC=0 RAM boundary access did not complete: %s",
                    registers ?: "(registers unavailable)");
        }
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readl(qts, done + 4), ==, 0x5a5a3c3c);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + RAM_LARGE_FIT - 4),
                    ==, 0x5a5a3c3c);
    g_assert_cmphex(qtest_readl(qts, RAM_LARGE_FIT - 4), ==, 0x5a5a3c3c);
    g_assert_cmphex(qtest_readl(qts, RAM_NO_ECC + RAM_LARGE_FIT - 4),
                    ==, 0x5a5a3c3c);
    qtest_quit(qts);
}

static void assert_banked_ram_mapping(QTestState *qts, bool mapped)
{
    const char *views[] = {
        "low", "high", "full", "no-ecc",
    };

    for (unsigned i = 0; i < ARRAY_SIZE(views); i++) {
        g_autofree char *path = g_strdup_printf(
            "/machine/crime/sgi-crime.%s-ram[0]", views[i]);
        g_autoptr(QDict) reply = qtest_qmp(qts,
            "{'execute':'qom-get','arguments':{'path':%s,"
            "'property':'container'}}", path);

        g_assert_false(qdict_haskey(reply, "error"));
        g_assert_cmpstr(qdict_get_str(reply, "return"), ==,
                        mapped ? "/machine/unattached/system[0]" : "");
    }
}

static void test_banked_uncached_window_with_profile(bool approx)
{
    const char *machine = approx ?
        "o2-test,bank-profile=8x128m-synthetic,reset-low-ram-approx=on" :
        "o2-test,bank-profile=8x128m-synthetic";
    const uint64_t entry = RAM_FULL + 0x2000;
    const uint64_t target = RAM_FULL + 0x80;
    const uint64_t data = RAM_FULL + RAM_BANK_SIZE + 0x1000;
    const uint64_t done = RAM_FULL + 0x3000;
    SN0Image trampoline = { 0 };
    const uint32_t program[] = {
        mips_lui(8, 0x0040), mips_daddiu(8, 8, 0x1000),
        mips_lw(10, 8, 0), mips_sw(10, 8, 4),
        mips_lui(16, 0xa000), mips_daddiu(16, 16, 0x3000),
        mips_sw(10, 16, 0),
        mips_ori(11, 0, 1), mips_sw(11, 16, 8),
        mips_lw(11, 16, 12),
        mips_branch(MIPS_OPC_BEQ, 11, 0, -2), 0,
        mips_lw(10, 8, 0), mips_sw(10, 8, 4),
        mips_sw(10, 16, 4),
        mips_ori(11, 0, 2), mips_sw(11, 16, 8),
        mips_branch(MIPS_OPC_BEQ, 0, 0, -1), 0,
    };
    uint8_t target_bytes[sizeof(program)];
    int64_t deadline;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = qtest_initf(MEMTX_TEST_DEVICE
                      "-M %s "
                      "-cpu R10000 -m 1G -nodefaults "
                      "-accel tcg,thread=single -S "
                      "-device loader,addr=0xffffffffa0002000,cpu-num=0",
                      machine);
    assert_banked_ram_mapping(qts, true);
    if (approx) {
        uint8_t byte;

        g_assert_cmpint(memtx_writeq(qts, CRIME + 0x210, 0x104),
                        ==, MEMTX_OK);
        g_assert_cmpint(memtx_readb(qts, RAM_FULL + 0x100, &byte),
                        !=, MEMTX_OK);
    }
    o2_program_banks(qts);
    sn0_emit_mtc0(&trampoline, SN0_R_ZERO, SN0_CP0_INDEX);
    sn0_emit_mtc0(&trampoline, SN0_R_ZERO, SN0_CP0_PAGEMASK);
    sn0_emit_li(&trampoline, SN0_R_T0, 0x00400000);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_ENTRYHI);
    /* C=2, UC=0: code in bank zero, data in decoder slot one. */
    sn0_emit_li(&trampoline, SN0_R_T0, 0x01000017);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_ENTRYLO0);
    sn0_emit_li(&trampoline, SN0_R_T0, 0x01200057);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_ENTRYLO1);
    sn0_emit_tlbwi(&trampoline);
    sn0_emit_li(&trampoline, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(&trampoline, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit_li(&trampoline, SN0_R_T0, 0x00400080);
    sn0_emit(&trampoline, mips_rtype(SN0_R_T0, 0, 0, 0, 0x08));
    sn0_emit_nop(&trampoline);
    for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
        stl_be_p(target_bytes + 4 * i, program[i]);
    }
    qtest_memwrite(qts, entry, trampoline.bytes, trampoline.pos);
    qtest_memwrite(qts, target, target_bytes, sizeof(target_bytes));
    qtest_writel(qts, done + 8, 0);
    qtest_writel(qts, done + 12, 0);
    qtest_writel(qts, data, 0x11223344);
    qtest_writel(qts, RAM_FULL + 2 * RAM_BANK_SIZE + 0x1000,
                 0x55667788);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");

    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (qtest_readl(qts, done + 8) != 1) {
        if (g_get_monotonic_time() >= deadline) {
            g_autofree char *registers = qtest_hmp(qts, "info registers");

            g_error("O2 banked UC=0 first access did not complete: %s",
                    registers ?: "(registers unavailable)");
        }
        g_usleep(1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    g_assert_cmphex(qtest_readl(qts, done), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, data + 4), ==, 0x11223344);

    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/crime',"
        "'property':'realized','value':false}}");
    assert_banked_ram_mapping(qts, false);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/crime',"
        "'property':'realized','value':true}}");
    assert_banked_ram_mapping(qts, true);

    qtest_writeq(qts, CRIME + 0x210, 0x10c);
    qtest_writeq(qts, CRIME + 0x218, 0x104);
    qtest_writel(qts, done + 12, 1);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");

    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (qtest_readl(qts, done + 8) != 2) {
        if (g_get_monotonic_time() >= deadline) {
            g_autofree char *registers = qtest_hmp(qts, "info registers");

            g_error("O2 banked UC=0 remap did not complete: %s",
                    registers ?: "(registers unavailable)");
        }
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readl(qts, done + 4), ==, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + RAM_BANK_SIZE + 0x1004),
                    ==, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, RAM_NO_ECC + RAM_BANK_SIZE + 0x1004),
                    ==, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 3 * RAM_BANK_SIZE + 0x1004),
                    ==, 0x11223344);
    qtest_quit(qts);
}

static void test_banked_uncached_window(void)
{
    test_banked_uncached_window_with_profile(false);
}

static void test_banked_approx_uncached_remap(void)
{
    test_banked_uncached_window_with_profile(true);
}

static unsigned o2_config0(QTestState *qts)
{
    g_autofree char *registers = qtest_hmp(qts, "info registers");
    const char *field;
    unsigned config;

    g_assert_nonnull(registers);
    field = strstr(registers, "Config0 0x");
    g_assert_nonnull(field);
    g_assert_cmpint(sscanf(field, "Config0 0x%x", &config), ==, 1);
    return config;
}

static void assert_module_profile(QTestState *qts)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'cpu-clock-hz'}}");
    unsigned config = o2_config0(qts);

    if (qdict_haskey(reply, "error")) {
        g_error("O2 clock query: %s",
                qdict_get_str(qdict_get_qdict(reply, "error"), "desc"));
    }
    g_assert_cmpuint(qdict_get_int(reply, "return"), ==, 195000000);
    /* Selected 1 MiB L2 and 128-byte blocks are model values. */
    g_assert_cmphex(config, ==, 0x6c19a603);
}

static void test_module_profile(void)
{
    QTestState *qts = o2_start();

    assert_module_profile(qts);
    qtest_system_reset(qts);
    assert_module_profile(qts);
    qtest_quit(qts);
}

static void test_prom_sb64_reset(void)
{
    const uint32_t program[] = {
        mips_lui(8, 0xa000), mips_mfc0(9, 16),
        mips_andi(9, 9, 0x2000),
        mips_branch(MIPS_OPC_BEQ, 9, 0, 4), 0,
        mips_ori(10, 0, 128),
        mips_branch(MIPS_OPC_BEQ, 0, 0, 3), 0,
        mips_ori(10, 0, 64), 0,
        mips_sw(10, 8, 0x3000),
        mips_branch(MIPS_OPC_BEQ, 0, 0, -1), 0,
    };
    uint8_t image[sizeof(program)];
    g_autofree char *dir = g_dir_make_tmp("o2-sb64-reset-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(program); i++) {
        stl_be_p(image + 4 * i, program[i]);
    }
    g_assert_true(g_file_set_contents(path, (const char *)image,
                                      sizeof(image), NULL));
    for (unsigned selected = 0; selected < 2; selected++) {
        g_autofree char *machine = g_strdup_printf(
            "o2-test,bank-profile=8x128m-synthetic,"
            "reset-low-ram-approx=on,empty-renderer-status=on,"
            "prom-private-window=on,%spassword-clear-input=0,rom-image=%s,rom-size=524288,"
            "rom-offset=0,rom-pad-byte=238",
            selected ? "prom-sb64-reset=on," : "", path);
        QTestState *qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s "
                                      "-cpu R10000 -m 1G -nodefaults "
                                      "-accel tcg,thread=single -S", machine);
        g_autoptr(QDict) property = qtest_qmp(qts,
            "{'execute':'qom-get','arguments':{'path':'/machine',"
            "'property':'prom-sb64-reset'}}");
        g_autoptr(QDict) change = qtest_qmp(qts,
            "{'execute':'qom-set','arguments':{'path':'/machine',"
            "'property':'prom-sb64-reset','value':true}}");

        g_assert_cmpint(qdict_get_bool(property, "return"), ==, selected);
        g_assert_nonnull(qdict_get_qdict(change, "error"));
        for (unsigned pass = 0; pass < 2; pass++) {
            unsigned config;
            SgiWait wait;

            if (pass) {
                qtest_system_reset(qts);
            }
            config = o2_config0(qts);
            g_assert_cmphex(config, ==,
                            selected ? 0x6c198603 : 0x6c19a603);
            g_assert_cmphex(config ^ 0x6c19a603, ==,
                            selected ? 0x2000 : 0);
            g_assert_cmphex((config >> 16) & 7, ==, 1);
            g_assert_cmphex((config >> 15) & 1, ==, 1);
            g_assert_cmphex(config & 7, ==, 3);
            qtest_writel(qts, RAM_FULL + 0x3000, 0);
            qtest_qmp_assert_success(qts, "{'execute':'cont'}");
            sgi_wait_start(&wait, "O2 synthetic SB branch");
            while (qtest_readl(qts, RAM_FULL + 0x3000) == 0) {
                sgi_wait_pause(&wait, 1000);
            }
            qtest_qmp_assert_success(qts, "{'execute':'stop'}");
            g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3000), ==,
                            selected ? 64 : 128);
            for (unsigned byte = 0; byte < 3; byte++) {
                g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x3000 + byte),
                                ==, 0);
            }
            g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x3003), ==,
                            selected ? 64 : 128);
        }
        qtest_quit(qts);
    }
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_serial_a(void)
{
    const char *args = "-M o2-test -cpu R10000 -m 64M -nodefaults "
                       "-accel tcg,thread=single -S";
    const uint8_t sent = 'A';
    uint8_t received;
    struct timeval deadline = { .tv_sec = SGI_WAIT_S };
    SgiWait wait;
    QTestState *qts;
    int sock;

    qts = qtest_init_with_serial(args, &sock);
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/mace',"
        "'property':'chardev-a'}}");
    g_assert_cmpstr(qdict_get_str(reply, "return"), ==, "s0");
    g_assert_cmpint(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                               &deadline, sizeof(deadline)), ==, 0);

    qtest_irq_intercept_out_named(qts, "/machine/crime", "cpu-ip");
    qtest_writeq(qts, MACE_PERIPH, 0);
    qtest_writeq(qts, MACE_PERIPH + 0x18, 1ULL << 20);
    qtest_writeq(qts, CRIME + 0x18, 1ULL << 4);
    qtest_writeb(qts, MACE_UART_A + 0x100, 1);
    g_assert_cmpint(write(sock, &sent, 1), ==, 1);
    sgi_wait_start(&wait, "MACE serial A receive");
    while (!(qtest_readb(qts, MACE_UART_A + 0x500) & 1)) {
        sgi_wait_pause(&wait, 1000);
    }
    g_assert_cmphex(qtest_readq(qts, MACE_PERIPH + 0x10) & (1ULL << 20),
                    ==, 1ULL << 20);
    g_assert_cmphex(qtest_readq(qts, CRIME + 0x28) & (1ULL << 4),
                    ==, 1ULL << 4);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(qtest_readb(qts, MACE_UART_A), ==, sent);
    g_assert_false(qtest_readq(qts, MACE_PERIPH + 0x10) & (1ULL << 20));
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_writeb(qts, MACE_UART_A, 'O');
    g_assert_cmpint(read(sock, &received, 1), ==, 1);
    g_assert_cmphex(received, ==, 'O');
    close(sock);
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

static void assert_o2_banked_rejected(const char *machine, const char *ram,
                                     const char *option, const char *value,
                                     const char *message)
{
    const char *args[16];
    unsigned n = 0;

    args[n++] = qtest_qemu_binary(NULL);
    args[n++] = "-M";
    args[n++] = machine;
    args[n++] = "-m";
    args[n++] = ram;
    args[n++] = "-display";
    args[n++] = "none";
    args[n++] = "-nodefaults";
    args[n++] = "-S";
    args[n++] = "-qmp";
    args[n++] = "stdio";
    if (option) {
        args[n++] = option;
        args[n++] = value;
    }
    args[n] = NULL;
    assert_launch_rejected(args, message);
}

static void test_banked_configuration(void)
{
    const char *banked = "o2-test,bank-profile=8x128m-synthetic";

    assert_o2_rejected("-m", "1G", "o2-test requires 64 or 128 MiB");
    assert_o2_banked_rejected(banked, "128M", NULL, NULL,
                              "eight-bank profile requires 1 GiB");
    assert_o2_banked_rejected(banked, "1G,slots=1,maxmem=2G", NULL, NULL,
                              "does not support memory slots");
    assert_o2_banked_rejected("o2-test,bank-profile=unknown", "1G",
                              NULL, NULL,
                              "supports only bank-profile=8x128m-synthetic");
    assert_o2_banked_rejected(
        "o2-test,bank-profile=8x128m-synthetic,password-clear-input=0,rom-image=unused",
        "1G", NULL, NULL, "does not support ROM images");
    assert_o2_banked_rejected(banked, "1G", "-bios", "unused",
                              "does not support -bios");
    assert_o2_banked_rejected(banked, "1G", "-cpu", "R12000",
                              "Invalid CPU model: R12000");
    assert_o2_banked_rejected(banked, "1G", "-cpu",
                              "R10000,cache-data-visibility=on",
                              "requires cache-data-visibility=off");
    assert_o2_banked_rejected(banked, "1G", "-smp", "2", "max CPUs");
    assert_o2_banked_rejected(banked, "1G", "-device",
                              "e1000,bus=mace.0,addr=1",
                              "slot 1 function 0 not available");
}

static void test_banked_approx_configuration(void)
{
    const char *approx = "o2-test,bank-profile=8x128m-synthetic,"
                         "reset-low-ram-approx=on";

    assert_o2_banked_rejected("o2-test,reset-low-ram-approx=on", "64M",
                              NULL, NULL,
                              "reset-low-ram-approx requires "
                              "bank-profile=8x128m-synthetic");
    assert_o2_banked_rejected("o2-test,reset-low-ram-approx=on", "128M",
                              NULL, NULL,
                              "reset-low-ram-approx requires "
                              "bank-profile=8x128m-synthetic");
    assert_o2_banked_rejected("o2-test,reset-low-ram-approx=on", "1G",
                              NULL, NULL,
                              "reset-low-ram-approx requires "
                              "bank-profile=8x128m-synthetic");
    assert_o2_banked_rejected(approx, "128M", NULL, NULL,
                              "eight-bank profile requires 1 GiB");
    assert_o2_banked_rejected(approx, "1G", "-bios", "unused",
                              "does not support -bios");
}

static void test_empty_renderer_configuration(void)
{
    assert_o2_banked_rejected("o2-test,empty-renderer-status=on",
                              "64M", NULL, NULL,
                              "empty-renderer-status requires "
                              "bank-profile=8x128m-synthetic");
    assert_o2_banked_rejected("o2-test,bank-profile=8x128m-synthetic,"
                              "empty-renderer-status=on", "1G", NULL, NULL,
                              "empty-renderer-status requires "
                              "reset-low-ram-approx=on");
    assert_o2_banked_rejected("o2-test,bank-profile=8x128m-synthetic,"
                              "reset-low-ram-approx=on,"
                              "empty-renderer-status=on", "128M", NULL, NULL,
                              "eight-bank profile requires 1 GiB");
}

static void test_functional_transfer_configuration(void)
{
    static const char image[] = { 0, 0, 0, 0 };
    g_autofree char *dir = g_dir_make_tmp("o2-transfer-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);
    const char *choices[] = {
        "empty-renderer-status", "functional-memory-transfer"
    };

    assert_o2_banked_rejected("o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "functional-memory-transfer=on", "1G", NULL, NULL, "conflicts with");
    assert_o2_banked_rejected("o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,prom-private-window=on", "1G", NULL, NULL,
        "prom-private-window requires");
    g_assert_true(g_file_set_contents(path, image, sizeof(image), NULL));
    for (unsigned i = 0; i < ARRAY_SIZE(choices); i++) {
        g_autofree char *selected = g_strdup_printf(
            "o2-test,bank-profile=8x128m-synthetic,reset-low-ram-approx=on,"
            "%s=on,prom-private-window=on,prom-sb64-reset=on,"
            "gbe11-registers=on,prom-id-word=on,password-clear-input=0,rom-image=%s,"
            "rom-size=524288,rom-offset=0,rom-pad-byte=238", choices[i], path);
        QTestState *qts = qtest_initf(MEMTX_TEST_DEVICE
            "-M %s -cpu R10000 -m 1G -nodefaults -accel tcg,thread=single -S",
            selected);
        g_autoptr(QDict) property = qtest_qmp(qts,
            "{'execute':'qom-get','arguments':{'path':'/machine/crime',"
            "'property':%s}}", choices[i]);
        g_autoptr(QDict) change = qtest_qmp(qts,
            "{'execute':'qom-set','arguments':{'path':'/machine',"
            "'property':%s,'value':false}}", choices[i]);

        g_assert_true(qdict_get_bool(property, "return"));
        g_assert_nonnull(qdict_get_qdict(change, "error"));
        g_assert_cmphex(qtest_readq(qts, CRIME), ==, 0xb1);
        g_assert_cmphex(qtest_readl(qts, CRIME + 4), ==, 0xb1);
        g_assert_cmphex(qtest_readl(qts, CRIME_RENDER_STATUS), ==, 0x1e000000);
        if (i == 1) {
            qtest_writeq(qts, 0x15001700, 0x8000000380000009ULL);
            qtest_writel(qts, 0x15003008, UINT32_MAX);
            qtest_writel(qts, 0x15003018, 0x58);
            qtest_writel(qts, 0x15003030, 7);
            qtest_writel(qts, 0x15003038, 39);
            qtest_writel(qts, 0x15003800, 4U << 2);
            for (unsigned j = 7; j <= 39; j++) {
                g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x3000 + j),
                                ==, 0x58);
            }
        }
        qtest_quit(qts);
    }
    g_unlink(path);
    g_rmdir(dir);
}

static void test_private_window_configuration(void)
{
    const char *selected =
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on";
    QTestState *qts;
    g_autoptr(QDict) get = NULL;
    g_autoptr(QDict) set = NULL;
    g_autoptr(QDict) default_get = NULL;

    assert_o2_banked_rejected("o2-test,prom-private-window=on", "64M",
                              NULL, NULL,
                              "prom-private-window requires the 1 GiB "
                              "eight-bank profile");
    assert_o2_banked_rejected("o2-test,bank-profile=8x128m-synthetic,"
                              "prom-private-window=on", "1G", NULL, NULL,
                              "prom-private-window requires the 1 GiB "
                              "eight-bank profile");
    assert_o2_banked_rejected("o2-test,bank-profile=8x128m-synthetic,"
                              "reset-low-ram-approx=on,"
                              "prom-private-window=on", "1G", NULL, NULL,
                              "empty-renderer-status=on");
    assert_o2_banked_rejected(selected, "128M", NULL, NULL,
                              "eight-bank profile requires 1 GiB");
    assert_o2_banked_rejected(selected, "1G", "-cpu",
                              "R10000,cache-data-visibility=on",
                              "requires cache-data-visibility=off");
    qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s -cpu R10000 -m 1G "
                      "-nodefaults -accel tcg,thread=single -S", selected);
    get = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'prom-private-window'}}");
    g_assert_true(qdict_get_bool(get, "return"));
    set = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'prom-private-window','value':false}}");
    g_assert_nonnull(qdict_get_qdict(set, "error"));
    qtest_quit(qts);
    qts = o2_start();
    default_get = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'prom-private-window'}}");
    g_assert_false(qdict_get_bool(default_get, "return"));
    qtest_quit(qts);
}

static void test_gbe_external_ttl_default(void)
{
    QTestState *qts = o2_start();
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'gbe-external-ttl-hz'}}");

    g_assert_nonnull(qdict_get(reply, "return"));
    g_assert_cmpuint(qdict_get_int(reply, "return"), ==, 0);
    qtest_quit(qts);
}


static uint64_t o2_clock_property(QTestState *qts, const char *path,
                                   const char *property)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        path, property);

    g_assert_nonnull(qdict_get(reply, "return"));
    return qdict_get_int(reply, "return");
}

static QTestState *o2_clock_start(const char *path, uint64_t hz)
{
    return qtest_initf(
        "-M o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,functional-memory-transfer=on,"
        "prom-private-window=on,prom-sb64-reset=on,gbe11-registers=on,"
        "password-clear-input=0,rom-image=%s,rom-size=524288,"
        "rom-offset=0,rom-pad-byte=238,gbe-external-ttl-hz=%" PRIu64
        " -cpu R10000 -m 1G -nodefaults", path, hz);
}

static void test_gbe_external_ttl_board(void)
{
    const char image[] = { 0, 0, 0, 0 };
    g_autofree char *dir = g_dir_make_tmp("o2-clock-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);

    g_assert_true(g_file_set_contents(path, image, sizeof(image), NULL));
    for (unsigned selected = 0; selected < 2; selected++) {
        uint64_t hz = selected ? 23000000 : 0;
        QTestState *qts = o2_clock_start(path, hz);

        g_assert_cmpuint(o2_clock_property(qts, "/machine",
                         "gbe-external-ttl-hz"), ==, hz);
        g_assert_cmpuint(o2_clock_property(qts, "/machine/gbe",
                         "external-ttl-hz"), ==, hz);
        g_assert_cmpuint(o2_clock_property(qts, "/machine/gbe",
                         "reference-hz"), ==, 20000000);
        g_assert_cmpuint(o2_clock_property(qts, "/machine/gbe",
                         "external-differential-hz"), ==, 0);
        qtest_irq_intercept_out_named(qts, "/machine/gbe", "sysbus-irq");
        qtest_writel(qts, GBE + GBE_VT_1, 97 | 67 << 12);
        qtest_writel(qts, GBE + 4, 3 | 3u << 14 | 1u << 20);
        qtest_clock_step(qts, 100);
        g_assert_cmphex(qtest_readl(qts, GBE + 0x10000), ==, 1u << 31);
        qtest_writel(qts, GBE + 0x10000, 0);
        qtest_clock_step(qts, 43);
        g_assert_cmphex(qtest_readl(qts, GBE + 0x10000), ==, 0);
        qtest_clock_step(qts, 1);
        g_assert_cmphex(qtest_readl(qts, GBE + 0x10000), ==, selected ? 1 : 0);
        /* RUN off cannot stop the external input. */
        qtest_writel(qts, GBE + 4, 0);
        qtest_clock_step(qts, 1000);
        g_assert_cmphex(qtest_readl(qts, GBE + 0x10000), ==, selected ? 24 : 0);
        qtest_writel(qts, GBE + 4, 3 | 3u << 14 | 1u << 20);
        qtest_writel(qts, GBE, 3u << 28);
        uint32_t before = qtest_readl(qts, GBE + 0x10000);
        qtest_clock_step(qts, 99);
        g_assert_cmphex(qtest_readl(qts, GBE + 0x10000), ==, before);
        qtest_clock_step(qts, 1);
        g_assert_cmphex(qtest_readl(qts, GBE + 0x10000), ==, before + 1);
        qtest_writel(qts, GBE, 1u << 28);
        before = qtest_readl(qts, GBE + 0x10000);
        qtest_clock_step(qts, 1000);
        g_assert_cmphex(qtest_readl(qts, GBE + 0x10000), ==, before);
        qtest_writel(qts, GBE, 2u << 28);
        qtest_clock_step(qts, 1000);
        g_assert_cmphex(qtest_readl(qts, GBE + 0x10000), ==, before);
        qtest_writel(qts, GBE, 0);
        qtest_writel(qts, GBE + 0x10000, 1u << 31);
        qtest_clock_step(qts, 1000);
        g_assert_cmphex(qtest_readl(qts, GBE + 0x10000), ==, 1u << 31);
        g_autoptr(QDict) machine_change = qtest_qmp_assert_failure_ref(qts,
            "{'execute':'qom-set','arguments':{'path':'/machine',"
            "'property':'gbe-external-ttl-hz','value':23000000}}");
        g_autoptr(QDict) device_change = qtest_qmp_assert_failure_ref(qts,
            "{'execute':'qom-set','arguments':{'path':'/machine/gbe',"
            "'property':'external-ttl-hz','value':23000000}}");
        qtest_system_reset(qts);
        g_assert_cmpuint(o2_clock_property(qts, "/machine",
                         "gbe-external-ttl-hz"), ==, hz);
        g_assert_cmpuint(o2_clock_property(qts, "/machine/gbe",
                         "external-ttl-hz"), ==, hz);
        g_assert_cmphex(qtest_readl(qts, GBE + 0x10000), ==, 1u << 31);
        for (unsigned i = 0; i < 4; i++) {
            g_assert_cmpuint(qtest_get_irq_raise_count(qts, i), ==, 0);
        }
        assert_private_window_migration_blocked(qts);
        qtest_quit(qts);
    }
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gbe_external_ttl_configuration(void)
{
    assert_o2_banked_rejected("o2-test,gbe-external-ttl-hz=23000000",
                              "64M", NULL, NULL, "requires gbe11-registers");
    const char *invalid[] = {
        "-1", "1.5", "1000000001", "18446744073709551616",
    };

    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
        g_autofree char *machine = g_strdup_printf(
            "o2-test,gbe-external-ttl-hz=%s", invalid[i]);

        assert_o2_banked_rejected(machine, "64M", NULL, NULL,
                                  "gbe-external-ttl-hz");
    }
    QTestState *qts = qtest_init(
        "-M o2-test,gbe-external-ttl-hz=0 -m 64M -nodefaults");
    g_assert_cmpuint(o2_clock_property(qts, "/machine",
                     "gbe-external-ttl-hz"), ==, 0);
    qtest_quit(qts);
}


static QTestState *o2_ttl_fixture(const char *extra)
{
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -m 64M -nodefaults "
        "-device sgi-o2-gbe,id=gbe,functional-display=on,"
        "external-ttl-hz=23000000,ram=/objects/ram/ram[0] %s", extra);

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/gbe", 0, GBE);
    return qts;
}

static void test_gbe_external_ttl_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("o2-ttl-migrate-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    g_autofree char *stream = g_build_filename(dir, "state", NULL);
    g_autofree char *file_uri = g_strdup_printf("file:%s", stream);
    QTestState *from = o2_ttl_fixture("");
    QTestState *control = o2_ttl_fixture("");
    QTestState *to = o2_ttl_fixture("-incoming defer");
    QTestState *sources[] = { from, control };

    for (unsigned i = 0; i < ARRAY_SIZE(sources); i++) {
        QTestState *qts = sources[i];

        qtest_irq_intercept_out_named(qts, "/machine/peripheral/gbe",
                                      "sysbus-irq");
        qtest_writel(qts, GBE + GBE_VT_1, 97 | 67 << 12);
        qtest_writel(qts, GBE + 0x10010, 4 | 5 << 12);
        qtest_writel(qts, GBE + 0x10020, 1 << 12);
        qtest_writel(qts, GBE + 0x3000c, 0x40010001);
        qtest_writel(qts, GBE + 0x10000, 0);
        qtest_clock_step(qts, 44);
    }
    qtest_irq_intercept_out_named(to, "/machine/peripheral/gbe", "sysbus-irq");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readl(to, GBE + 0x10000), ==, 1);
    g_assert_cmphex(qtest_readl(to, GBE + 0x30008), ==, 0);
    g_assert_cmphex(qtest_readl(to, GBE + 0x3000c), ==, 0x40010001);
    g_assert_cmpuint(qtest_get_irq_raise_count(to, 0), ==, 0);
    const unsigned steps[] = { 42, 1, 4174, 17000 };
    for (unsigned i = 0; i < ARRAY_SIZE(steps); i++) {
        qtest_clock_step(to, steps[i]);
        qtest_clock_step(control, steps[i]);
        g_assert_cmphex(qtest_readl(to, GBE + 0x10000), ==,
                        qtest_readl(control, GBE + 0x10000));
        g_assert_cmphex(qtest_readl(to, GBE + 0x30008), ==,
                        qtest_readl(control, GBE + 0x30008));
        g_assert_cmpuint(qtest_get_irq_raise_count(to, 0), ==,
                         qtest_get_irq_raise_count(control, 0));
    }
    g_assert_cmphex(qtest_readl(to, GBE + 0x30008), ==, 0x40010001);
    g_assert_cmpuint(qtest_get_irq_raise_count(to, 0), ==, 1);
    qtest_qmp_assert_success(control,
        "{'execute':'migrate','arguments':{'uri':%s}}", file_uri);
    sgi_wait_migration(control, "completed");
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "none", "-cpu", "R10000", "-m", "64M",
        "-nodefaults", "-display", "none", "-accel", "qtest",
        "-device", "sgi-o2-gbe,id=gbe,functional-display=on,"
        "external-ttl-hz=20000000,ram=/objects/ram/ram[0]",
        "-incoming", file_uri, "-qmp", "stdio", NULL,
    };
    assert_launch_rejected(args,
        "device 'sgi-o2-gbe': 1312d00 != 15ef3c0");
    qtest_quit(from);
    qtest_quit(control);
    qtest_quit(to);
    g_unlink(stream);
    g_unlink(sock);
    g_rmdir(dir);
}

static void test_gbe11_registers(void)
{
    static const char image[] = { 0, 0, 0, 0 };
    static const unsigned offsets[] = { 0, 0x14 };
    g_autofree char *dir = g_dir_make_tmp("o2-gbe11-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);
    g_autofree char *log_path = g_build_filename(dir, "guest-errors.txt", NULL);
    g_autofree char *log = NULL;
    g_autofree char *base = NULL;
    g_autofree char *selected = NULL;
    g_autoptr(QDict) property = NULL;
    g_autoptr(QDict) change = NULL;
    QTestState *qts;
    uint8_t byte;
    uint16_t half;
    uint32_t word;
    uint64_t wide;

    g_assert_true(g_file_set_contents(path, image, sizeof(image), NULL));
    base = g_strdup_printf(
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,prom-sb64-reset=on,"
        "password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0,rom-pad-byte=238",
        path);
    selected = g_strdup_printf("%s,gbe11-registers=on", base);
    qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s -cpu R10000 -m 1G "
                      "-nodefaults -accel tcg,thread=single -S", base);
    g_assert_cmpint(memtx_readl(qts, GBE, &word), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readl(qts, GBE + 0x14, &word), !=, MEMTX_OK);
    qtest_quit(qts);

    qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s -cpu R10000 -m 1G "
                      "-nodefaults -accel tcg,thread=single -S "
                      "-d guest_errors -D %s", selected, log_path);
    property = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'gbe11-registers'}}");
    g_assert_true(qdict_get_bool(property, "return"));
    change = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'gbe11-registers','value':false}}");
    g_assert_nonnull(qdict_get_qdict(change, "error"));
    g_assert_cmpint(memtx_readl(qts, GBE, &word), ==, MEMTX_OK);
    g_assert_cmphex(word & 0xf, ==, 1);
    g_assert_cmpint(memtx_readl(qts, GBE + 0x14, &word), ==, MEMTX_OK);
    g_assert_cmphex(word, ==, 0x666);
    g_assert_cmpint(memtx_readl(qts, GBE + GBE_APERTURE_SIZE - 4, &word),
                    ==, MEMTX_OK);
    g_assert_cmpint(memtx_readl(qts, GBE + GBE_APERTURE_SIZE, &word),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_readq(qts, CRIME, &wide), ==, MEMTX_OK);
    g_assert_cmphex(wide, ==, 0xb1);
    g_assert_cmpint(memtx_readq(qts, MACE_PERIPH, &wide), ==, MEMTX_OK);
    g_assert_cmphex(wide & 1, ==, 1);
    g_assert_cmpint(memtx_writel(qts, RAM_FULL + 0x2000, 0x1234abcd),
                    ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x2000), ==, 0x1234abcd);

    for (unsigned i = 0; i < G_N_ELEMENTS(offsets); i++) {
        uint64_t addr = GBE + offsets[i];

        g_assert_cmpint(memtx_readb(qts, addr, &byte), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readw(qts, addr, &half), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readq(qts, addr, &wide), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readl(qts, addr + 1, &word), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writeb(qts, addr, 0xff), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writew(qts, addr, 0xffff), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writeq(qts, addr, UINT64_MAX), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writel(qts, addr + 1, UINT32_MAX),
                        !=, MEMTX_OK);
    }
    g_assert_cmpint(memtx_readl(qts, GBE, &word), ==, MEMTX_OK);
    g_assert_cmphex(word & 0xf, ==, 1);
    g_assert_cmpint(memtx_readl(qts, GBE + 0x14, &word), ==, MEMTX_OK);
    g_assert_cmphex(word, ==, 0x666);
    g_assert_cmpint(memtx_readl(qts, GBE + 0x18, &word), ==, MEMTX_OK);
    g_assert_cmphex(word, ==, UINT32_MAX);

    g_assert_cmpint(memtx_writel(qts, GBE + GBE_VT_1, 0x0056789a),
                    ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(qts, GBE + GBE_VT_1) & 0x00ffffff,
                    ==, 0x0056789a);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, GBE + GBE_VT_1) & 0x00ffffff, ==, 0);
    g_assert_cmphex(qtest_readl(qts, GBE) & 0xf, ==, 1);
    g_assert_cmphex(qtest_readl(qts, GBE + 0x14), ==, 0x666);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log,
                            "sgi-o2-gbe: unsupported access read at 0x000018"));
    g_assert_cmpint(unlink(log_path), ==, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_prom_id_word(void)
{
    const uint32_t program[] = {
        mips_lui(8, 0xb400), mips_lw(9, 8, 4),
        mips_lui(10, 0xa000), mips_sw(9, 10, 0x3000),
        mips_branch(MIPS_OPC_BEQ, 0, 0, -1), 0,
    };
    uint8_t image[sizeof(program)];
    g_autofree char *dir = NULL;
    g_autofree char *path = NULL;
    g_autofree char *base = NULL;
    g_autofree char *selected = NULL;
    g_autoptr(QDict) default_machine = NULL;
    g_autoptr(QDict) default_crime = NULL;
    g_autoptr(QDict) selected_machine = NULL;
    g_autoptr(QDict) selected_crime = NULL;
    QTestState *qts;
    SgiWait wait;
    uint32_t word;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(program); i++) {
        stl_be_p(image + 4 * i, program[i]);
    }
    dir = g_dir_make_tmp("o2-id-word-lane-XXXXXX", NULL);
    path = g_build_filename(dir, "synthetic.bin", NULL);
    g_assert_true(g_file_set_contents(path, (const char *)image,
                                      sizeof(image), NULL));
    base = g_strdup_printf(
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,prom-sb64-reset=on,gbe11-registers=on,"
        "password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0,rom-pad-byte=238",
        path);
    selected = g_strdup_printf("%s,prom-id-word=on", base);

    qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s -cpu R10000 -m 1G "
                      "-nodefaults -accel tcg,thread=single -S", base);
    default_machine = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'prom-id-word'}}");
    default_crime = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/crime',"
        "'property':'prom-id-word'}}");
    g_assert_nonnull(qdict_get(default_machine, "return"));
    g_assert_nonnull(qdict_get(default_crime, "return"));
    g_assert_false(qdict_get_bool(default_machine, "return"));
    g_assert_false(qdict_get_bool(default_crime, "return"));
    g_assert_cmpint(memtx_readl(qts, CRIME + 4, &word), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, CRIME), ==, 0xb1);
    qtest_system_reset(qts);
    g_assert_cmpint(memtx_readl(qts, CRIME + 4, &word), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, CRIME), ==, 0xb1);
    qtest_quit(qts);

    qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s -cpu R10000 -m 1G "
                      "-nodefaults -accel tcg,thread=single -S", selected);
    selected_machine = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'prom-id-word'}}");
    selected_crime = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/crime',"
        "'property':'prom-id-word'}}");
    g_assert_nonnull(qdict_get(selected_machine, "return"));
    g_assert_nonnull(qdict_get(selected_crime, "return"));
    g_assert_true(qdict_get_bool(selected_machine, "return"));
    g_assert_true(qdict_get_bool(selected_crime, "return"));
    g_assert_cmpint(memtx_readl(qts, CRIME + 4, &word), ==, MEMTX_OK);
    g_assert_cmphex(word, ==, 0xb1);
    g_assert_cmphex(qtest_readq(qts, CRIME), ==, 0xb1);
    qtest_writel(qts, RAM_FULL + 0x3000, 0);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&wait, "O2 synthetic uncached CRIME ID word read");
    while (qtest_readl(qts, RAM_FULL + 0x3000) != 0xb1) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    g_assert_cmphex(qtest_readl(qts, RAM_FULL + 0x3000), ==, 0xb1);
    qtest_system_reset(qts);
    g_assert_cmpint(memtx_readl(qts, CRIME + 4, &word), ==, MEMTX_OK);
    g_assert_cmphex(word, ==, 0xb1);
    g_assert_cmphex(qtest_readq(qts, CRIME), ==, 0xb1);
    qtest_quit(qts);
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_prom_id_word_configuration(void)
{
    static const char image[] = { 0, 0, 0, 0 };
    static const char * const incomplete[] = {
        "o2-test,prom-id-word=on",
        "o2-test,bank-profile=8x128m-synthetic,prom-id-word=on",
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,prom-id-word=on",
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-id-word=on",
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,prom-id-word=on",
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,prom-sb64-reset=on,"
        "prom-id-word=on",
    };
    g_autofree char *dir = g_dir_make_tmp("o2-id-lane-reject-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);
    g_autofree char *partial_rom = NULL;
    g_autofree char *selected = NULL;

    g_assert_true(g_file_set_contents(path, image, sizeof(image), NULL));
    for (unsigned i = 0; i < G_N_ELEMENTS(incomplete); i++) {
        assert_o2_banked_rejected(incomplete[i], i ? "1G" : "64M",
                                  NULL, NULL, "prom-id-word requires");
    }
    partial_rom = g_strdup_printf(
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,prom-sb64-reset=on,gbe11-registers=on,"
        "prom-id-word=on,password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0",
        path);
    assert_o2_banked_rejected(partial_rom, "1G", NULL, NULL,
                              "prom-id-word requires");
    selected = g_strdup_printf("%s,rom-pad-byte=238", partial_rom);
    assert_o2_banked_rejected(selected, "128M", NULL, NULL,
                              "prom-id-word requires");
    assert_o2_banked_rejected(selected, "1G", "-smp", "2",
                              "max CPUs");
    assert_o2_banked_rejected(selected, "1G", "-cpu", "R12000",
                              "Invalid CPU model: R12000");
    assert_o2_banked_rejected(selected, "1G", "-cpu",
                              "R10000,cache-data-visibility=on",
                              "requires cache-data-visibility=off");
    assert_o2_banked_rejected("origin200,prom-id-word=on", "64M",
                              NULL, NULL, "prom-id-word");
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_gbe11_registers_configuration(void)
{
    static const char image[] = { 0, 0, 0, 0 };
    static const char * const incomplete[] = {
        "o2-test,gbe11-registers=on",
        "o2-test,bank-profile=8x128m-synthetic,gbe11-registers=on",
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,gbe11-registers=on",
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "gbe11-registers=on",
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,gbe11-registers=on",
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,prom-sb64-reset=on,gbe11-registers=on",
    };
    g_autofree char *dir = g_dir_make_tmp("o2-gbe11-reject-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);
    g_autofree char *partial_rom = NULL;
    g_autofree char *selected = NULL;

    g_assert_true(g_file_set_contents(path, image, sizeof(image), NULL));
    for (unsigned i = 0; i < G_N_ELEMENTS(incomplete); i++) {
        assert_o2_banked_rejected(incomplete[i], i ? "1G" : "64M",
                                  NULL, NULL, "gbe11-registers requires");
    }
    partial_rom = g_strdup_printf(
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,prom-sb64-reset=on,gbe11-registers=on,"
        "password-clear-input=0,rom-image=%s,rom-size=524288,"
        "rom-offset=0", path);
    assert_o2_banked_rejected(partial_rom, "1G", NULL, NULL,
                              "gbe11-registers requires");
    selected = g_strdup_printf(
        "%s,rom-pad-byte=238", partial_rom);
    assert_o2_banked_rejected(selected, "128M", NULL, NULL,
                              "eight-bank profile requires 1 GiB");
    assert_o2_banked_rejected(selected, "1G", "-cpu", "R12000",
                              "Invalid CPU model: R12000");
    assert_o2_banked_rejected(selected, "1G", "-cpu",
                              "R10000,cache-data-visibility=on",
                              "requires cache-data-visibility=off");
    assert_o2_banked_rejected("origin200,gbe11-registers=on", "64M",
                              NULL, NULL, "gbe11-registers");
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_prom_sb64_reset_configuration(void)
{
    static const char image[] = { 0, 0, 0, 0 };
    const char *base =
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on";
    g_autofree char *dir = g_dir_make_tmp("o2-sb64-reject-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);
    g_autofree char *without_private = NULL;
    g_autofree char *without_rom = NULL;
    g_autofree char *selected = NULL;

    g_assert_true(g_file_set_contents(path, image, sizeof(image), NULL));
    without_private = g_strdup_printf(
        "%s,prom-sb64-reset=on,password-clear-input=0,rom-image=%s,rom-size=524288,"
        "rom-offset=0,rom-pad-byte=238", base, path);
    without_rom = g_strdup_printf(
        "%s,prom-private-window=on,prom-sb64-reset=on", base);
    selected = g_strdup_printf(
        "%s,prom-private-window=on,prom-sb64-reset=on,"
        "password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0,rom-pad-byte=238",
        base, path);
    assert_o2_banked_rejected("o2-test,prom-sb64-reset=on", "64M",
                              NULL, NULL,
                              "prom-sb64-reset requires");
    assert_o2_banked_rejected(without_private, "1G", NULL, NULL,
                              "prom-sb64-reset requires");
    assert_o2_banked_rejected(without_rom, "1G", NULL, NULL,
                              "prom-sb64-reset requires");
    assert_o2_banked_rejected(selected, "128M", NULL, NULL,
                              "eight-bank profile requires 1 GiB");
    assert_o2_banked_rejected(selected, "1G", "-cpu", "R12000",
                              "Invalid CPU model: R12000");
    assert_o2_banked_rejected(selected, "1G", "-cpu",
                              "R10000,cache-data-visibility=on",
                              "requires cache-data-visibility=off");
    assert_o2_banked_rejected(selected, "1G", "-cpu",
                              "R10000,reset-mode=0x19a603",
                              "prom-sb64-reset rejects CPU reset-mode");
    assert_o2_banked_rejected("origin200,prom-sb64-reset=on", "64M",
                              NULL, NULL, "prom-sb64-reset");
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_banked_approx_rom_rejected(void)
{
    static const char image[] = { 0x12, 0x34, 0x56, 0x78 };
    const char *approx = "o2-test,bank-profile=8x128m-synthetic,"
                         "reset-low-ram-approx=on";
    g_autofree char *dir = g_dir_make_tmp("o2-approx-not-rom-XXXXXX",
                                          NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.bin", NULL);
    g_autofree char *nonregular = g_strdup_printf(
        "%s,password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0,rom-pad-byte=255",
        approx, dir);
    g_autofree char *too_large = g_strdup_printf(
        "%s,password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=524285,"
        "rom-pad-byte=255", approx, path);
    g_autofree char *bad_capacity = g_strdup_printf(
        "%s,password-clear-input=0,rom-image=%s,rom-size=65536,rom-offset=0,"
        "rom-pad-byte=255", approx, path);
    g_autofree char *bad_padding = g_strdup_printf(
        "%s,password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0,"
        "rom-pad-byte=256", approx, path);

    g_assert_true(g_file_set_contents(path, image, sizeof(image), NULL));
    assert_o2_banked_rejected("o2-test,bank-profile=8x128m-synthetic,"
                              "reset-low-ram-approx=on,rom-size=524288",
                              "1G", NULL, NULL,
                              "o2-test ROM settings require rom-image");
    assert_o2_banked_rejected("o2-test,bank-profile=8x128m-synthetic,"
                              "reset-low-ram-approx=on,password-clear-input=0,rom-image=unused",
                              "1G", NULL, NULL,
                              "o2-test ROM requires rom-size, rom-offset "
                              "and rom-pad-byte");
    assert_o2_banked_rejected(nonregular, "1G", NULL, NULL,
                              "o2-test ROM image must be a regular file");
    assert_o2_banked_rejected(too_large, "1G", NULL, NULL,
                              "does not fit at offset");
    assert_o2_banked_rejected(bad_capacity, "1G", NULL, NULL,
                              "requires flash-size of 512 KiB");
    assert_o2_banked_rejected(bad_padding, "1G", NULL, NULL,
                              "requires image, image-offset and pad-byte");
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_banked_profile_immutable(void)
{
    QTestState *qts = o2_start();
    g_autoptr(QDict) set = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'bank-profile','value':'8x128m-synthetic'}}");
    g_autoptr(QDict) get = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'bank-profile'}}");
    QDict *error = qdict_get_qdict(set, "error");

    g_assert_nonnull(error);
    g_assert_nonnull(strstr(qdict_get_str(error, "desc"),
                            "bank-profile cannot change once the machine "
                            "is built"));
    g_assert_cmpstr(qdict_get_str(get, "return"), ==, "");
    qtest_quit(qts);
}

static void test_banked_rom_immutable(void)
{
    static const struct {
        const char *name;
        unsigned value;
    } numeric[] = {
        { "rom-size", 524288 },
        { "rom-offset", 256 },
        { "rom-pad-byte", 238 },
    };
    QTestState *qts = o2_start_banked();
    g_autoptr(QDict) before = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'rom-image'}}");
    g_autoptr(QDict) set = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'rom-image','value':'unused'}}");
    g_autoptr(QDict) after = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'rom-image'}}");
    QDict *error = qdict_get_qdict(set, "error");

    g_assert_false(qdict_haskey(before, "error"));
    g_assert_false(qdict_haskey(after, "error"));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(qdict_get_str(error, "desc"),
                            "eight-bank ROM settings cannot change"));
    g_assert_true(qobject_is_equal(qdict_get(before, "return"),
                                   qdict_get(after, "return")));
    for (unsigned i = 0; i < ARRAY_SIZE(numeric); i++) {
        g_autoptr(QDict) old = qtest_qmp(qts,
            "{'execute':'qom-get','arguments':{'path':'/machine',"
            "'property':%s}}", numeric[i].name);
        g_autoptr(QDict) write = qtest_qmp(qts,
            "{'execute':'qom-set','arguments':{'path':'/machine',"
            "'property':%s,'value':%u}}",
            numeric[i].name, numeric[i].value);
        g_autoptr(QDict) current = qtest_qmp(qts,
            "{'execute':'qom-get','arguments':{'path':'/machine',"
            "'property':%s}}", numeric[i].name);
        QDict *write_error = qdict_get_qdict(write, "error");

        g_assert_false(qdict_haskey(old, "error"));
        g_assert_false(qdict_haskey(current, "error"));
        g_assert_nonnull(write_error);
        g_assert_nonnull(strstr(qdict_get_str(write_error, "desc"),
                                "eight-bank ROM settings cannot change"));
        g_assert_true(qobject_is_equal(qdict_get(old, "return"),
                                       qdict_get(current, "return")));
    }
    qtest_quit(qts);
}

static void test_banked_approx_immutable(void)
{
    QTestState *qts = o2_start_banked_approx();
    g_autoptr(QDict) set = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'reset-low-ram-approx','value':false}}");
    g_autoptr(QDict) get = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'reset-low-ram-approx'}}");
    g_autoptr(QDict) rom = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'rom-image','value':'unused'}}");
    g_autoptr(QDict) flat_set = NULL;
    g_autoptr(QDict) flat_get = NULL;
    QDict *error = qdict_get_qdict(set, "error");

    g_assert_nonnull(error);
    g_assert_nonnull(strstr(qdict_get_str(error, "desc"),
                            "reset-low-ram-approx cannot change"));
    g_assert_true(qdict_get_bool(get, "return"));
    g_assert_nonnull(qdict_get_qdict(rom, "error"));
    qtest_quit(qts);

    qts = o2_start();
    flat_set = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'reset-low-ram-approx','value':true}}");
    flat_get = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'reset-low-ram-approx'}}");
    g_assert_nonnull(qdict_get_qdict(flat_set, "error"));
    g_assert_false(qdict_get_bool(flat_get, "return"));
    qtest_quit(qts);
}

static void assert_banked_approx_child_immutable(QTestState *qts,
                                                 bool selected)
{
    g_autoptr(QDict) change = NULL;
    g_autoptr(QDict) child = NULL;
    g_autoptr(QDict) machine = NULL;
    QDict *error;
    uint8_t byte;

    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/crime',"
        "'property':'realized','value':false}}");
    change = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/crime',"
        "'property':'reset-low-ram-approx','value':%i}}", !selected);
    error = qdict_get_qdict(change, "error");
    g_assert_nonnull(error);
    if (!strstr(qdict_get_str(error, "desc"),
                "cannot change after initial realization")) {
        g_error("unexpected CRIME option change error: %s",
                qdict_get_str(error, "desc"));
    }
    child = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/crime',"
        "'property':'reset-low-ram-approx'}}");
    machine = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'reset-low-ram-approx'}}");
    g_assert_cmpint(qdict_get_bool(child, "return"), ==, selected);
    g_assert_cmpint(qdict_get_bool(machine, "return"), ==, selected);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/crime',"
        "'property':'realized','value':true}}");
    qtest_system_reset(qts);
    if (selected) {
        qtest_writeb(qts, RAM_FULL, 0x63);
        g_assert_cmphex(qtest_readb(qts, 0), ==, 0x63);
    } else {
        g_assert_cmpint(memtx_readb(qts, RAM_FULL, &byte), !=, MEMTX_OK);
    }
}

static void test_banked_approx_child_immutable(void)
{
    QTestState *qts = o2_start_banked_approx();

    assert_banked_approx_child_immutable(qts, true);
    qtest_quit(qts);
    qts = o2_start_banked();
    assert_banked_approx_child_immutable(qts, false);
    qtest_quit(qts);
}

static void test_banked_approx_migration(void)
{
    QTestState *qts = o2_start_banked_approx();
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'migrate','arguments':{'uri':'file:/dev/null'}}");
    QDict *error = qdict_get_qdict(reply, "error");

    g_assert_nonnull(error);
    g_assert_nonnull(strstr(qdict_get_str(error, "desc"),
                            "State blocked by non-migratable device"));
    qtest_quit(qts);
}

static void test_configuration(void)
{
    const char *usb[] = {
        qtest_qemu_binary(NULL), "-M", "o2-test,usb=on", "-m", "64M",
        "-nodefaults", "-display", "none", "-S", "-qmp", "stdio", NULL,
    };
    const char *pci[] = {
        qtest_qemu_binary(NULL), "-M", "o2-test", "-m", "64M",
        "-nodefaults", "-display", "none", "-S", "-qmp", "stdio",
        "-device", "e1000,bus=mace.0,addr=1", NULL,
    };
    const char *second_serial[] = {
        qtest_qemu_binary(NULL), "-M", "o2-test", "-m", "64M",
        "-nodefaults", "-display", "none", "-S", "-qmp", "stdio",
        "-serial", "null", "-serial", "null", NULL,
    };
    const char *rom_without_image[] = {
        qtest_qemu_binary(NULL), "-M", "o2-test,rom-size=524288", "-m",
        "64M", "-nodefaults", "-display", "none", "-S", "-qmp", "stdio",
        NULL,
    };
    const char *rom_without_layout[] = {
        qtest_qemu_binary(NULL), "-M",
        "o2-test,password-clear-input=0,rom-image=unused", "-m",
        "64M", "-nodefaults", "-display", "none", "-S", "-qmp", "stdio",
        NULL,
    };

    assert_o2_rejected("-m", "32M", "o2-test requires 64 or 128 MiB");
    assert_o2_rejected("-m", "256M", "o2-test requires 64 or 128 MiB");
    assert_o2_rejected("-m", "64M,slots=1,maxmem=128M",
                       "o2-test does not support memory slots");
    assert_o2_rejected("-cpu", "R12000", "Invalid CPU model: R12000");
    assert_o2_rejected("-smp", "2", "max CPUs");
    assert_o2_rejected("-kernel", "unused", "o2-test does not support -kernel");
    assert_o2_rejected("-bios", "unused", "o2-test does not support -bios");
    assert_launch_rejected(usb, "o2-test does not support USB devices");
    assert_launch_rejected(pci, "slot 1 function 0 not available");
    assert_launch_rejected(second_serial,
                           "o2-test supports only one host serial backend");
    assert_launch_rejected(rom_without_image,
                           "o2-test ROM settings require rom-image");
    assert_launch_rejected(rom_without_layout,
                           "o2-test ROM requires rom-size, rom-offset and "
                           "rom-pad-byte");
}

static void test_gbe_one_tile_retired(void)
{
    g_autofree char *dir = g_dir_make_tmp("o2-gbe-retired-XXXXXX", NULL);
    g_autofree char *image = g_build_filename(dir, "synthetic.bin", NULL);
    g_autofree char *selected = NULL;

    g_assert_true(g_file_set_contents(image, "\0", 1, NULL));
    selected = g_strdup_printf(
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,prom-sb64-reset=on,gbe11-registers=on,"
        "password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0,"
        "rom-pad-byte=238,gbe-one-tile-diagnostic=on", image);
    assert_o2_banked_rejected(selected, "1G", NULL, NULL,
                              "gbe-one-tile-diagnostic is superseded");
    assert_o2_banked_rejected("o2-test,gbe-one-tile-diagnostic=on", "64M",
                              NULL, NULL,
                              "gbe-one-tile-diagnostic is superseded");
    g_assert_cmpint(unlink(image), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static uint32_t o2_aic_config_read(QTestState *qts, unsigned slot,
                                   unsigned offset)
{
    qtest_writel(qts, MACE_CFGADDR, slot << 11 | offset);
    return qtest_readl(qts, MACE_CFGDATA);
}

static void o2_aic_config_write(QTestState *qts, unsigned slot,
                                 unsigned offset, uint32_t value)
{
    qtest_writel(qts, MACE_CFGADDR, slot << 11 | offset);
    qtest_writel(qts, MACE_CFGDATA, value);
}

static uint8_t o2_aic_config_readb(QTestState *qts, unsigned slot,
                                   unsigned offset)
{
    qtest_writel(qts, MACE_CFGADDR, slot << 11 | offset);
    return qtest_readb(qts, MACE_CFGDATA + 3 - (offset & 3));
}

static uint16_t o2_aic_config_readw(QTestState *qts, unsigned slot,
                                    unsigned offset)
{
    qtest_writel(qts, MACE_CFGADDR, slot << 11 | offset);
    return qtest_readw(qts, MACE_CFGDATA + 2 - (offset & 2));
}

static void test_mace_pci_unused_aic_config(void)
{
    static const unsigned fields[] = { 0x18, 0x1c, 0x20, 0x24, 0x30 };
    static const uint32_t payloads[] = {
        UINT32_MAX, 0, 0x81000001, 0x52a739c6,
    };
    QTestState *qts = o2_start();
    uint32_t unmapped, error_address;

    qtest_irq_intercept_out_named(qts, "/machine/mace", "crime");
    qtest_writel(qts, MACE_PCI + 8, 0x80000003);
    qtest_set_irq_in(qts, "/machine/mace", "pci", 0, 1);
    g_assert_true(qtest_get_irq(qts, 8));
    g_assert_false(qtest_get_irq(qts, 7));
    g_assert_cmpint(memtx_readl(qts, O2_PCI_HIGH_VIEW + 0x1000000, &unmapped),
                    !=, MEMTX_OK);
    for (unsigned slot = 1; slot <= 2; slot++) {
        uint32_t bar0 = 0x2001 + (slot - 1) * 0x100;
        uint32_t bar1 = 0x80010000 + (slot - 1) * 0x1000;

        g_assert_cmphex(o2_aic_config_read(qts, slot, 0), ==, 0x80789004);
        o2_aic_config_write(qts, slot, 4, 0);
        o2_aic_config_write(qts, slot, 0x10, UINT32_MAX);
        g_assert_cmphex(o2_aic_config_read(qts, slot, 0x10), ==, 0xffffff01);
        o2_aic_config_write(qts, slot, 0x10, bar0);
        o2_aic_config_write(qts, slot, 0x14, UINT32_MAX);
        g_assert_cmphex(o2_aic_config_read(qts, slot, 0x14), ==, 0xfffff000);
        o2_aic_config_write(qts, slot, 0x14, bar1);
        o2_aic_config_write(qts, slot, 4, 6);
        for (unsigned r = 0; r < ARRAY_SIZE(fields); r++) {
            for (unsigned i = 0; i < ARRAY_SIZE(payloads); i++) {
                qtest_writel(qts, MACE_CFGADDR, slot << 11 | fields[r]);
                g_assert_cmpint(memtx_writel(qts, MACE_CFGDATA, payloads[i]),
                                ==, MEMTX_OK);
                for (unsigned lane = 0; lane < 4; lane++) {
                    g_assert_cmpint(memtx_writeb(qts, MACE_CFGDATA + lane,
                                                 payloads[i]), ==, MEMTX_OK);
                }
                for (unsigned lane = 0; lane < 4; lane += 2) {
                    g_assert_cmpint(memtx_writew(qts, MACE_CFGDATA + lane,
                                                 payloads[i]), ==, MEMTX_OK);
                }
                g_assert_cmphex(qtest_readl(qts, MACE_CFGDATA), ==, 0);
                g_assert_cmphex(o2_aic_config_read(qts, slot, 0x10), ==, bar0);
                g_assert_cmphex(o2_aic_config_read(qts, slot, 0x14), ==, bar1);
                g_assert_cmphex(o2_aic_config_readw(qts, slot, 4), ==, 6);
                g_assert_cmphex(qtest_readl(qts, MACE_PCI + 4) &
                                ((1u << 31) | (1u << 19)), ==, 0);
                g_assert_false(qtest_get_irq(qts, 7));
                g_assert_true(qtest_get_irq(qts, 8));
                g_assert_cmpint(memtx_readl(qts, O2_PCI_HIGH_VIEW + 0x1000000,
                                             &unmapped), !=, MEMTX_OK);
            }
        }
        qtest_writel(qts, MACE_CFGADDR, slot << 11 | 4);
        g_assert_cmpint(memtx_writel(qts, MACE_CFGDATA, 0xe), ==, MEMTX_ERROR);
        g_assert_cmphex(o2_aic_config_readw(qts, slot, 4), ==, 6);
    }
    const uint32_t absent[] = { 3u << 11, (1u << 11) | (1u << 8) };
    for (unsigned dev = 0; dev < ARRAY_SIZE(absent); dev++) {
        for (unsigned r = 0; r < ARRAY_SIZE(fields); r++) {
            uint32_t address = absent[dev] | fields[r];

            qtest_writel(qts, MACE_PCI + 4, 0);
            qtest_writel(qts, MACE_CFGADDR, address);
            g_assert_cmphex(qtest_readl(qts, MACE_CFGDATA), ==, UINT32_MAX);
            g_assert_cmpint(memtx_writel(qts, MACE_CFGDATA, 0x63a591e7),
                            ==, MEMTX_OK);
            g_assert_cmphex(qtest_readl(qts, MACE_PCI + 4) &
                            ((1u << 31) | (1u << 19)), ==,
                            (1u << 31) | (1u << 19));
            g_assert_cmphex(qtest_readl(qts, MACE_PCI), ==, address);
            g_assert_true(qtest_get_irq(qts, 7));
            qtest_writel(qts, MACE_PCI + 4, 1u << 31);
            g_assert_true(qtest_get_irq(qts, 7));
            qtest_writel(qts, MACE_PCI + 4, 0);
            g_assert_false(qtest_get_irq(qts, 7));
            g_assert_true(qtest_get_irq(qts, 8));
        }
    }
    qtest_writel(qts, MACE_CFGADDR, absent[1] | fields[0]);
    qtest_readl(qts, MACE_CFGDATA);
    error_address = qtest_readl(qts, MACE_PCI);
    for (unsigned slot = 1; slot <= 2; slot++) {
        for (unsigned r = 0; r < ARRAY_SIZE(fields); r++) {
            qtest_writel(qts, MACE_CFGADDR, slot << 11 | fields[r]);
            g_assert_cmpint(memtx_writel(qts, MACE_CFGDATA, 0xa5c3197e),
                            ==, MEMTX_OK);
            g_assert_cmphex(qtest_readl(qts, MACE_CFGDATA), ==, 0);
            g_assert_cmphex(qtest_readl(qts, MACE_PCI), ==, error_address);
            g_assert_true(qtest_get_irq(qts, 7));
            g_assert_true(qtest_get_irq(qts, 8));
        }
    }
    qtest_quit(qts);
}

static unsigned o2_aic_bus_level(QTestState *qts)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/mace',"
        "'property':'x-test-pci-bus-level'}}");

    return qdict_get_int(reply, "return");
}

static void o2_aic_assert_irqs(QTestState *qts, unsigned bus,
                                unsigned delivered, bool cpu)
{
    uint64_t hardware;

    g_assert_cmphex(o2_aic_bus_level(qts) & 3, ==, bus);
    g_assert_cmpint(memtx_readq(qts, CRIME + 0x28, &hardware), ==, MEMTX_OK);
    g_assert_cmphex((hardware >> 8) & 3, ==, delivered);
    g_assert_cmpint(qtest_get_irq(qts, 1), ==, cpu);
}

static void test_aic_pci_diagnostic(void)
{
    QTestState *qts = qtest_init(MEMTX_TEST_DEVICE
        "-M o2-test,aic-pci-diagnostic=on -cpu R10000 -m 64M "
        "-nodefaults -accel qtest");
    uint8_t byte;
    uint64_t irq;
    g_autoptr(QDict) migrate = NULL;

    qtest_irq_intercept_out_named(qts, "/machine/crime", "cpu-ip");
    for (unsigned slot = 1; slot <= 2; slot++) {
        g_assert_cmphex(o2_aic_config_read(qts, slot, 0), ==, 0x80789004);
        g_assert_cmphex(o2_aic_config_read(qts, slot, 8) & 0xffffff00,
                        ==, 0x01000000);
        g_assert_cmphex(o2_aic_config_readb(qts, slot, 0), ==, 0x04);
        g_assert_cmphex(o2_aic_config_readw(qts, slot, 2), ==, 0x8078);
        o2_aic_config_write(qts, slot, 0x10, UINT32_MAX);
        g_assert_cmphex(o2_aic_config_read(qts, slot, 0x10), ==,
                        0xffffff01);
        o2_aic_config_write(qts, slot, 0x10,
                            0x2001 + (slot - 1) * 0x100);
        g_assert_cmphex(o2_aic_config_read(qts, slot, 0x10), ==,
                        0x2001 + (slot - 1) * 0x100);
        o2_aic_config_write(qts, slot, 0x14, UINT32_MAX);
        g_assert_cmphex(o2_aic_config_read(qts, slot, 0x14), ==,
                        0xfffff000);
        o2_aic_config_write(qts, slot, 0x14,
                            0x80000000 + (slot - 1) * 0x1000);
    }
    g_assert_cmpint(memtx_readb(qts, AIC_PCI_VIEW + 0x87, &byte),
                    !=, MEMTX_OK);
    o2_aic_config_write(qts, 1, 4, 0x00000006);
    o2_aic_config_write(qts, 2, 4, 0x00000006);
    qtest_writel(qts, MACE_CFGADDR, 1u << 11 | 4);
    qtest_writeb(qts, MACE_CFGDATA + 3, 4);
    g_assert_cmphex(o2_aic_config_readb(qts, 1, 4), ==, 4);
    g_assert_cmpint(memtx_readb(qts, AIC_PCI_VIEW + 0x20, &byte),
                    !=, MEMTX_OK);
    qtest_writel(qts, MACE_CFGADDR, 1u << 11 | 4);
    qtest_writew(qts, MACE_CFGDATA + 2, 6);
    g_assert_cmphex(o2_aic_config_readw(qts, 1, 4) & 7, ==, 6);
    qtest_writeb(qts, AIC_PCI_VIEW + 0x20, 0x5a);
    qtest_writeb(qts, AIC_PCI_VIEW + 0x1000 + 0x20, 0xa5);
    g_assert_cmphex(o2_aic_config_read(qts, 1, 4) & 7, ==, 6);
    g_assert_cmphex(o2_aic_config_read(qts, 2, 4) & 7, ==, 6);
    qtest_writel(qts, MACE_CFGADDR, 1u << 11 | 0x18);
    g_assert_cmpint(memtx_writel(qts, MACE_CFGDATA, 0x80003000),
                    ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(qts, MACE_CFGDATA), ==, 0);
    qtest_writel(qts, MACE_CFGADDR, 1u << 11 | 4);
    g_assert_cmpint(memtx_writel(qts, MACE_CFGDATA, 0x00000008),
                    !=, MEMTX_OK);
    g_assert_cmphex(o2_aic_config_read(qts, 1, 4) & 7, ==, 6);
    g_assert_cmphex(qtest_readb(qts, AIC_PCI_VIEW + 0xf20), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, AIC_PCI_VIEW + 0x1f20), ==, 0xa5);
    g_assert_cmpint(memtx_readb(qts, AIC_PCI_VIEW - 1, &byte),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_readb(qts, AIC_PCI_VIEW + 0x2000, &byte),
                    !=, MEMTX_OK);

    o2_aic_config_write(qts, 1, 4, 0x00000004);
    g_assert_cmpint(memtx_readb(qts, AIC_PCI_VIEW + 0x20, &byte),
                    !=, MEMTX_OK);
    o2_aic_config_write(qts, 1, 4, 0x00000006);
    g_assert_cmphex(qtest_readb(qts, AIC_PCI_VIEW + 0x20), ==, 0x5a);
    o2_aic_config_write(qts, 1, 0x14, 0x80002000);
    g_assert_cmpint(memtx_readb(qts, AIC_PCI_VIEW + 0x20, &byte),
                    !=, MEMTX_OK);
    o2_aic_config_write(qts, 1, 0x14, 0x80000000);
    g_assert_cmphex(qtest_readb(qts, AIC_PCI_VIEW + 0x20), ==, 0x5a);

    qtest_writel(qts, MACE_PCI + 8, 3);
    qtest_writeq(qts, CRIME + 0x18, 3ULL << 8);
    qtest_writeb(qts, AIC_PCI_VIEW + 0x87, 0x16);
    g_assert_cmphex(o2_aic_bus_level(qts) & 3, ==, 1);
    g_assert_cmpint(memtx_readq(qts, CRIME + 0x28, &irq), ==, MEMTX_OK);
    g_assert_cmphex(irq & (3ULL << 8), ==, 1ULL << 8);
    g_assert_true(qtest_get_irq(qts, 1));
    qtest_writeb(qts, AIC_PCI_VIEW + 0x1087, 0x16);
    g_assert_cmphex(o2_aic_bus_level(qts) & 3, ==, 3);
    g_assert_cmpint(memtx_readq(qts, CRIME + 0x28, &irq), ==, MEMTX_OK);
    g_assert_cmphex(irq & (3ULL << 8), ==, 3ULL << 8);
    qtest_writeb(qts, AIC_PCI_VIEW + 0x87, 0x06);
    g_assert_cmphex(o2_aic_bus_level(qts) & 3, ==, 2);
    g_assert_cmpint(memtx_readq(qts, CRIME + 0x28, &irq), ==, MEMTX_OK);
    g_assert_cmphex(irq & (3ULL << 8), ==, 1ULL << 9);
    qtest_writeb(qts, AIC_PCI_VIEW + 0x87, 0x16);
    o2_aic_assert_irqs(qts, 3, 3, true);

    qtest_writeb(qts, AIC_PCI_VIEW + 0x87, 0x14);
    o2_aic_assert_irqs(qts, 2, 2, true);
    qtest_writeb(qts, AIC_PCI_VIEW + 0x87, 0x16);
    o2_aic_assert_irqs(qts, 3, 3, true);
    o2_aic_config_write(qts, 1, 4, 2);
    o2_aic_assert_irqs(qts, 2, 2, true);
    o2_aic_config_write(qts, 1, 4, 6);
    o2_aic_assert_irqs(qts, 3, 3, true);
    o2_aic_config_write(qts, 1, 4, 4);
    o2_aic_assert_irqs(qts, 2, 2, true);
    g_assert_cmpint(memtx_readb(qts, AIC_PCI_VIEW + 0x87, &byte),
                    !=, MEMTX_OK);
    o2_aic_config_write(qts, 1, 4, 6);
    o2_aic_assert_irqs(qts, 3, 3, true);
    qtest_writeb(qts, AIC_PCI_VIEW + 0x87, 0x56);
    o2_aic_assert_irqs(qts, 2, 2, true);
    qtest_writeb(qts, AIC_PCI_VIEW + 0x87, 0x16);
    o2_aic_assert_irqs(qts, 3, 3, true);
    o2_aic_config_write(qts, 1, 4, 5);
    o2_aic_assert_irqs(qts, 3, 3, true);
    g_assert_cmpint(memtx_readb(qts, AIC_PCI_VIEW + 0x87, &byte),
                    !=, MEMTX_OK);
    o2_aic_config_write(qts, 1, 4, 6);

    qtest_writel(qts, MACE_PCI + 8, 2);
    o2_aic_assert_irqs(qts, 3, 2, true);
    qtest_writel(qts, MACE_PCI + 8, 1);
    o2_aic_assert_irqs(qts, 3, 1, true);
    qtest_writel(qts, MACE_PCI + 8, 3);
    o2_aic_assert_irqs(qts, 3, 3, true);
    qtest_writeq(qts, CRIME + 0x18, 0);
    o2_aic_assert_irqs(qts, 3, 3, false);
    qtest_writeq(qts, CRIME + 0x18, 1ULL << 8);
    qtest_writel(qts, MACE_PCI + 8, 2);
    o2_aic_assert_irqs(qts, 3, 2, false);
    qtest_writel(qts, MACE_PCI + 8, 3);
    o2_aic_assert_irqs(qts, 3, 3, true);
    qtest_writeq(qts, CRIME + 0x18, 1ULL << 9);
    qtest_writel(qts, MACE_PCI + 8, 1);
    o2_aic_assert_irqs(qts, 3, 1, false);
    qtest_writel(qts, MACE_PCI + 8, 3);
    o2_aic_assert_irqs(qts, 3, 3, true);
    qtest_writeq(qts, CRIME + 0x18, 3ULL << 8);
    o2_aic_assert_irqs(qts, 3, 3, true);
    qtest_writeb(qts, AIC_PCI_VIEW + 0x1087, 0x06);
    o2_aic_assert_irqs(qts, 1, 1, true);
    qtest_writeb(qts, AIC_PCI_VIEW + 0x87, 0x06);
    o2_aic_assert_irqs(qts, 0, 0, false);

    qtest_writel(qts, MACE_CFGADDR, 3u << 11);
    (void)qtest_readl(qts, MACE_CFGDATA);
    g_assert_cmphex(qtest_readl(qts, MACE_PCI + 4) &
                    ((1u << 31) | (1u << 19)), ==,
                    (1u << 31) | (1u << 19));
    g_assert_cmphex(qtest_readl(qts, MACE_PCI + 0x20), ==, UINT32_MAX);

    migrate = qtest_qmp(qts,
        "{'execute':'migrate','arguments':{'uri':'unix:/nonexistent'}}");
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(migrate, "error"),
                                         "desc"),
                            "O2 AIC PCI diagnostic"));
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(o2_aic_config_read(qts, 1, 4) & 7, ==, 0);
    g_assert_cmphex(o2_aic_config_read(qts, 2, 4) & 7, ==, 0);
    g_assert_cmphex(qtest_readl(qts, MACE_PCI + 8) & 3, ==, 0);
    o2_aic_assert_irqs(qts, 0, 0, false);
    qtest_quit(qts);
}

static void test_aic_pci_low_dma_policy_off(void)
{
    QTestState *qts = qtest_init(MEMTX_TEST_DEVICE
        "-M o2-test,aic-pci-diagnostic=on -cpu R10000 -m 64M "
        "-nodefaults -accel qtest");
    uint8_t guarded[32], after[32], write_guard[288], write_after[288];

    g_assert_false(qtest_qom_get_bool(qts, "/machine",
                                       "mace-eth-low-dma-compat"));
    o2_aic_config_write(qts, 1, 0x14, 0x80000000);
    o2_aic_config_write(qts, 1, 4, 6);
    memset(guarded, 0xa5, sizeof(guarded));
    qtest_memwrite(qts, RAM_FULL + 0x100, guarded, sizeof(guarded));
    for (unsigned i = 0; i < 4; i++) {
        qtest_writeb(qts, AIC_PCI_VIEW + 0x88 + i, 0x108 >> (8 * i));
    }
    qtest_writeb(qts, AIC_PCI_VIEW + 0x8c, 7);
    qtest_writeb(qts, AIC_PCI_VIEW + 0x8d, 0);
    qtest_writeb(qts, AIC_PCI_VIEW + 0x8e, 0);
    qtest_writeb(qts, AIC_PCI_VIEW + 0x93, 0x05);
    qtest_writeb(qts, AIC_PCI_VIEW + 0x93, 0x0c);
    g_assert_false(qtest_readb(qts, AIC_PCI_VIEW + 0x94) & 8);
    qtest_memread(qts, RAM_FULL + 0x100, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), guarded, sizeof(guarded));

    qtest_system_reset(qts);
    o2_aic_config_write(qts, 1, 0x14, 0x80000000);
    o2_aic_config_write(qts, 1, 4, 6);
    memset(write_guard, 0xa5, sizeof(write_guard));
    qtest_memwrite(qts, RAM_FULL + 0x1f0, write_guard,
                   sizeof(write_guard));
    for (unsigned i = 0; i < 256; i++) {
        qtest_writeb(qts, AIC_PCI_VIEW + 0x99, i ^ 0x5a);
    }
    for (unsigned i = 0; i < 4; i++) {
        qtest_writeb(qts, AIC_PCI_VIEW + 0x88 + i, 0x200 >> (8 * i));
    }
    qtest_writeb(qts, AIC_PCI_VIEW + 0x8c, 0);
    qtest_writeb(qts, AIC_PCI_VIEW + 0x8d, 1);
    qtest_writeb(qts, AIC_PCI_VIEW + 0x8e, 0);
    qtest_writeb(qts, AIC_PCI_VIEW + 0x93, 0x08);
    g_assert_false(qtest_readb(qts, AIC_PCI_VIEW + 0x94) & 8);
    qtest_memread(qts, RAM_FULL + 0x1f0, write_after,
                  sizeof(write_after));
    g_assert_cmpmem(write_after, sizeof(write_after), write_guard,
                    sizeof(write_guard));
    qtest_quit(qts);
}

static void test_mace_low_dma_option_scope(void)
{
    const char *direct_mace[] = {
        qtest_qemu_binary(NULL), "-M", "none", "-m", "1G",
        "-accel", "qtest", "-nodefaults", "-display", "none",
        "-qmp", "stdio", "-device",
        "sgi-mace,mace-eth-low-dma-compat=on", NULL,
    };
    QTestState *qts;

    assert_launch_rejected(direct_mace,
                           "requires the O2 decoded 1 GiB RAM container");
    assert_o2_banked_rejected("o2-test,mace-eth-low-dma-compat=on",
                              "64M", "-accel", "qtest",
                              "requires the 1 GiB eight-bank profile");
    assert_o2_banked_rejected(
        "o2-test,bank-profile=8x128m-synthetic,"
        "mace-eth-low-dma-compat=on", "128M", "-accel", "qtest",
        "requires the 1 GiB eight-bank profile");
    qts = qtest_init(MEMTX_TEST_DEVICE
        "-M o2-test,bank-profile=8x128m-synthetic,"
        "mace-eth-low-dma-compat=on -cpu R10000 -m 1G "
        "-nodefaults -accel qtest -S");
    g_assert_true(qtest_qom_get_bool(qts, "/machine",
                                      "mace-eth-low-dma-compat"));
    g_assert_true(qtest_qom_get_bool(qts, "/machine/mace",
                                      "mace-eth-low-dma-compat"));
    qtest_quit(qts);
}

static void test_aic_pci_diagnostic_rejections(void)
{
    const char *extra_pci[] = {
        qtest_qemu_binary(NULL), "-M", "o2-test,aic-pci-diagnostic=on",
        "-m", "64M", "-cpu", "R10000", "-accel", "qtest",
        "-nodefaults", "-display", "none", "-qmp", "stdio",
        "-device", "lsi53c895a,bus=mace.0,addr=3", NULL,
    };
    const char *extra_scsi[] = {
        qtest_qemu_binary(NULL), "-M", "o2-test,aic-pci-diagnostic=on",
        "-m", "64M", "-cpu", "R10000", "-accel", "qtest",
        "-nodefaults", "-display", "none", "-qmp", "stdio",
        "-device", "scsi-cd,bus=aic0.0,scsi-id=0", NULL,
    };
    QTestState *qts = o2_start();
    g_autoptr(QDict) reply = NULL;

    g_assert_false(qtest_qom_get_bool(qts, "/machine",
                                       "aic-pci-diagnostic"));
    qtest_writel(qts, MACE_CFGADDR, 1u << 11);
    g_assert_cmphex(qtest_readl(qts, MACE_CFGDATA), ==, 0x80789004);
    g_assert_cmphex(qtest_readl(qts, MACE_PCI + 4) & (1u << 31), ==, 0);
    qtest_quit(qts);

    assert_o2_banked_rejected("o2-test,aic-pci-diagnostic=on", "128M",
                              "-accel", "qtest", "requires QTest, 64 MiB");
    assert_o2_banked_rejected("o2-test,aic-pci-diagnostic=on", "1G",
                              "-accel", "qtest", "requires QTest, 64 MiB");
    assert_o2_banked_rejected("o2-test,aic-pci-diagnostic=on", "64M",
                              "-accel", "tcg", "requires QTest, 64 MiB");
    assert_o2_banked_rejected(
        "o2-test,aic-pci-diagnostic=on,password-clear-input=0,"
        "rom-image=unused", "64M",
        "-accel", "qtest", "requires QTest, 64 MiB RAM and no ROM");
    assert_o2_banked_rejected("origin200,aic-pci-diagnostic=on", "64M",
                              "-accel", "qtest", "aic-pci-diagnostic");
    assert_launch_rejected(extra_pci, "does not support attached PCI");
    assert_launch_rejected(extra_scsi, "rejects SCSI targets");

    qts = qtest_init(MEMTX_TEST_DEVICE
        "-M o2-test,aic-pci-diagnostic=on -cpu R10000 -m 64M "
        "-nodefaults -accel qtest");
    reply = qtest_qmp(qts,
        "{'execute':'device_add','arguments':{'driver':'lsi53c895a',"
        "'bus':'mace.0','addr':'3','id':'extra'}}");
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(reply, "error"),
                                         "desc"),
                            "mace.0' does not support hotplugging"));
    qobject_unref(reply);
    reply = NULL;
    reply = qtest_qmp(qts,
        "{'execute':'device_add','arguments':{'driver':'scsi-cd',"
        "'bus':'aic0.0','scsi-id':0,'id':'target'}}");
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(reply, "error"),
                                         "desc"),
                            "AIC PCI diagnostic rejects PCI and SCSI"));
    qtest_quit(qts);
}

static void o2_mace_card_set(QTestState *qts, const char *property,
                             uint64_t value)
{
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'" MACE_DMA_CARD "',"
        "'property':%s,'value':%" PRIu64 "}}", property, value);
}

static uint64_t o2_mace_card_get(QTestState *qts, const char *property)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'" MACE_DMA_CARD "',"
        "'property':%s}}", property);

    return qdict_get_uint(reply, "return");
}

static uint32_t o2_mace_card_copy(QTestState *qts, uint64_t address)
{
    o2_mace_card_set(qts, "test-source", address);
    o2_mace_card_set(qts, "test-address", address);
    o2_mace_card_set(qts, "test-length", 1);
    o2_mace_card_set(qts, "test-command", 2);
    return o2_mace_card_get(qts, "test-result");
}

static uint32_t o2_mace_card_fill(QTestState *qts, uint64_t address,
                                  uint8_t value)
{
    o2_mace_card_set(qts, "test-address", address);
    o2_mace_card_set(qts, "test-length", 1);
    o2_mace_card_set(qts, "test-seed", value);
    o2_mace_card_set(qts, "test-command", 1);
    return o2_mace_card_get(qts, "test-result");
}

static QTestState *o2_mace_banked_dma_start(void)
{
    return qtest_init(MEMTX_TEST_DEVICE
        "-M o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,mace-bank-dma-fixture=on "
        "-cpu R10000 -m 1G -nodefaults -accel qtest -S");
}

static uint32_t o2_mace_copy_range(QTestState *qts, uint64_t source,
                                   uint64_t destination, unsigned length)
{
    o2_mace_card_set(qts, "test-source", source);
    o2_mace_card_set(qts, "test-address", destination);
    o2_mace_card_set(qts, "test-length", length);
    o2_mace_card_set(qts, "test-command", 2);
    return o2_mace_card_get(qts, "test-result");
}

static void test_mace_pci_fresh_reads(void)
{
    QTestState *qts = o2_mace_banked_dma_start();
    static const uint64_t offsets[] = { 0x17d, RAM_BANK_SIZE - 7 };
    uint8_t source[32], original[32], expected[32], got[32];

    o2_program_banks(qts);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'" MACE_DMA_CARD "',"
        "'property':'test-master-enable','value':true}}");
    for (unsigned c = 0; c < ARRAY_SIZE(offsets); c++) {
        uint64_t offset = offsets[c];
        uint64_t base = (offset & ~3ULL) - 8;
        uint64_t destination = 0x3000 + 8 + (offset & 3);

        for (unsigned i = 0; i < sizeof(source); i++) {
            source[i] = 0x31 + 13 * i;
        }
        qtest_memwrite(qts, RAM_FULL + base, source, sizeof(source));
        for (unsigned phase = 0; phase < 3; phase++) {
            if (phase) {
                for (unsigned i = 0; i < 7; i++) {
                    source[((offset + i) ^ 3) - base] = 0x80 + 19 * i + phase;
                }
                qtest_memwrite(qts, RAM_FULL + base, source, sizeof(source));
            }
            qtest_writel(qts, MACE_PCI + 8, phase == 1 ? 0x8000 : 0);
            if (phase == 2) {
                g_assert_cmpint(memtx_writel(qts, MACE_PCI + 0xc, 0x97ac0123),
                                ==, MEMTX_OK);
            }
            memset(expected, 0xa6, sizeof(expected));
            qtest_memwrite(qts, RAM_FULL + 0x3000, expected, sizeof(expected));
            g_assert_cmpint(o2_mace_copy_range(qts, MACE_NATIVE_DMA + offset,
                MACE_NATIVE_DMA + destination, 7), ==, MEMTX_OK);
            for (unsigned i = 0; i < 7; i++) {
                expected[((destination + i) ^ 3) - 0x3000] =
                    source[((offset + i) ^ 3) - base];
            }
            qtest_memread(qts, RAM_FULL + 0x3000, got, sizeof(got));
            g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
            qtest_memread(qts, RAM_FULL + base, original, sizeof(original));
            g_assert_cmpmem(original, sizeof(original), source, sizeof(source));
        }
    }
    /* A read ending at the decoded bank boundary must not fetch beyond it. */
    qtest_writeq(qts, CRIME + 0x208 + 8, 0x11c);
    g_assert_cmpint(o2_mace_copy_range(qts,
        MACE_NATIVE_DMA + RAM_BANK_SIZE - 7, MACE_NATIVE_DMA + 0x3001, 7),
        ==, MEMTX_OK);
    memset(expected, 0xc9, sizeof(expected));
    qtest_memwrite(qts, RAM_FULL + 0x3000, expected, sizeof(expected));
    g_assert_cmpint(o2_mace_copy_range(qts,
        MACE_NATIVE_DMA + RAM_BANK_SIZE - 7, MACE_NATIVE_DMA + 0x3001, 8),
        ==, MEMTX_ERROR);
    qtest_memread(qts, RAM_FULL + 0x3000, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
    qtest_quit(qts);
}

static void check_mace_native_bytes(QTestState *qts, uint64_t offset,
                                    unsigned length, bool split)
{
    uint64_t base = (offset & ~3ULL) - 8;
    uint8_t source[64], expected[64], actual[64];

    for (unsigned i = 0; i < sizeof(source); i++) {
        source[i] = 0x40 + i;
    }
    memset(expected, 0xb6, sizeof(expected));
    qtest_memwrite(qts, RAM_FULL + base, source, sizeof(source));
    qtest_memwrite(qts, RAM_FULL + 0x1000, expected, sizeof(expected));
    for (unsigned i = 0; i < length; i++) {
        expected[8 + i] = source[((offset + i) ^ 3) - base];
    }
    for (unsigned i = 0; i < length; i += split ? 1 : length) {
        g_assert_cmpuint(o2_mace_copy_range(qts, MACE_NATIVE_DMA + offset + i,
                                          0x1008 + i, split ? 1 : length),
                         ==, MEMTX_OK);
    }
    qtest_memread(qts, RAM_FULL + 0x1000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    qtest_memread(qts, RAM_FULL + base, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), source, sizeof(source));

    for (unsigned i = 0; i < sizeof(source); i++) {
        source[i] = 0x80 + i;
    }
    memset(expected, 0xd7, sizeof(expected));
    qtest_memwrite(qts, RAM_FULL + 0x2000, source, sizeof(source));
    qtest_memwrite(qts, RAM_FULL + base, expected, sizeof(expected));
    for (unsigned i = 0; i < length; i++) {
        expected[((offset + i) ^ 3) - base] = source[8 + i];
    }
    for (unsigned i = 0; i < length; i += split ? 1 : length) {
        g_assert_cmpuint(o2_mace_copy_range(qts, 0x2008 + i,
                                          MACE_NATIVE_DMA + offset + i,
                                          split ? 1 : length), ==, MEMTX_OK);
    }
    qtest_memread(qts, RAM_FULL + base, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    qtest_memread(qts, RAM_FULL + 0x2000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), source, sizeof(source));
}

static void test_mace_native_bytes(void)
{
    static const struct {
        uint64_t offset;
        unsigned length;
    } cases[] = {
        { 0x100, 4 }, { 0x104, 4 }, { 0x104, 8 }, { 0x104, 28 },
        { 0x17c, 28 }, { 0x101, 1 }, { 0x102, 2 }, { 0x103, 3 },
        { 0x17d, 7 }, { 0x0ffffffc, 28 }, { 0x10000104, 28 },
    };
    QTestState *qts = o2_mace_banked_dma_start();

    o2_program_banks(qts);
    g_assert_true(qtest_qom_get_bool(qts, "/machine/mace",
                                      "pci-native-word-swap"));
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'" MACE_DMA_CARD "',"
        "'property':'test-master-enable','value':true}}");
    for (unsigned remap = 0; remap < 2; remap++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
            check_mace_native_bytes(qts, cases[i].offset, cases[i].length,
                                     false);
            check_mace_native_bytes(qts, cases[i].offset, cases[i].length,
                                     true);
        }
        qtest_writeq(qts, CRIME + 0x208 + 2 * 8, 0x100);
        qtest_writeq(qts, CRIME + 0x208, 0x108);
    }
    qtest_quit(qts);
}

static void test_mace_native_failure_prefix(void)
{
    QTestState *qts = o2_mace_banked_dma_start();
    uint8_t source[8], expected[16], actual[16];

    o2_program_banks(qts);
    g_assert_true(qtest_qom_get_bool(qts, "/machine/mace",
                                      "pci-native-word-swap"));
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'" MACE_DMA_CARD "',"
        "'property':'test-master-enable','value':true}}");
    for (unsigned i = 0; i < sizeof(source); i++) {
        source[i] = 0x80 + i;
    }
    memset(expected, 0xb6, sizeof(expected));
    qtest_memwrite(qts, RAM_FULL + 0x2000, source, sizeof(source));
    qtest_memwrite(qts, RAM_FULL + RAM_BANK_SIZE - 8,
                   expected, sizeof(expected));
    /* Move bank one away, leaving its previous decoded range unmatched. */
    qtest_writeq(qts, CRIME + 0x208 + 8, 0x11c);
    g_assert_cmpuint(o2_mace_copy_range(qts, 0x2000,
                                      MACE_NATIVE_DMA + RAM_BANK_SIZE - 4,
                                      sizeof(source)), ==, MEMTX_ERROR);
    for (unsigned i = 0; i < 4; i++) {
        expected[4 + (i ^ 3)] = source[i];
    }
    qtest_writeq(qts, CRIME + 0x208 + 8, 0x104);
    qtest_memread(qts, RAM_FULL + RAM_BANK_SIZE - 8, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));

    memset(expected, 0xd7, sizeof(expected));
    qtest_memwrite(qts, RAM_FULL + 0x3000, expected, sizeof(expected));
    qtest_writeq(qts, CRIME + 0x208 + 8, 0x11c);
    g_assert_cmpuint(o2_mace_copy_range(qts,
                                      MACE_NATIVE_DMA + RAM_BANK_SIZE - 4,
                                      0x3000, sizeof(source)), ==, MEMTX_ERROR);
    qtest_memread(qts, RAM_FULL + 0x3000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    qtest_writeq(qts, CRIME + 0x208 + 8, 0x104);
    qtest_quit(qts);
}

static void test_mace_banked_dma_fixture(void)
{
    QTestState *qts = o2_mace_banked_dma_start();
    g_autoptr(QDict) crime_target = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/crime',"
        "'property':'sgi-crime.decoded-ram[0]'}}");
    g_autoptr(QDict) mace_target = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/mace',"
        "'property':'ram'}}");
    g_autoptr(QDict) direct_target = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'" MACE_DMA_CARD "',"
        "'property':'dma-memory'}}");
    g_autoptr(QDict) raw_view = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'" MACE_DMA_CARD "',"
        "'property':'dma-r32'}}");
    g_autoptr(QDict) enable_raw = NULL;
    g_autoptr(QDict) link_direct = NULL;
    g_autoptr(QDict) latch = NULL;
    g_autoptr(QDict) unlatch = NULL;
    g_autoptr(QDict) master = NULL;
    g_autoptr(QDict) migrate = NULL;
    g_autofree char *save_reply = NULL;
    uint8_t byte;

    g_assert_cmpstr(qdict_get_str(mace_target, "return"), ==,
                    qdict_get_str(crime_target, "return"));
    g_assert_cmpstr(qdict_get_str(direct_target, "return"), ==, "");
    g_assert_false(qdict_get_bool(raw_view, "return"));
    latch = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'" MACE_DMA_CARD "',"
        "'property':'pci-only-latch'}}");
    g_assert_nonnull(qdict_get(latch, "return"));
    g_assert_true(qdict_get_bool(latch, "return"));
    unlatch = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'" MACE_DMA_CARD "',"
        "'property':'pci-only-latch','value':false}}");
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(unlatch, "error"),
                                         "desc"), "locked"));
    enable_raw = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'" MACE_DMA_CARD "',"
        "'property':'dma-r32','value':true}}");
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(enable_raw, "error"),
                                         "desc"), "after it was realized"));
    link_direct = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'" MACE_DMA_CARD "',"
        "'property':'dma-memory','value':%s}}",
        qdict_get_str(crime_target, "return"));
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(link_direct, "error"),
                                         "desc"), "after it was realized"));

    o2_program_banks(qts);
    qtest_writeb(qts, RAM_FULL + 0x123, 0xa0);
    qtest_writeb(qts, RAM_FULL + 2 * RAM_BANK_SIZE + 0x123, 0xc2);
    qtest_writeb(qts, RAM_FULL + 4 * RAM_BANK_SIZE + 0x123, 0xe4);
    qtest_writeb(qts, RAM_FULL + 6 * RAM_BANK_SIZE + 0x123, 0xf6);

    g_assert_cmpuint(o2_mace_card_copy(qts, MACE_NATIVE_DMA + 0x120),
                     !=, MEMTX_OK);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'" MACE_DMA_CARD "',"
        "'property':'test-master-enable','value':true}}");
    master = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'" MACE_DMA_CARD "',"
        "'property':'test-master-enable'}}");
    g_assert_true(qdict_get_bool(master, "return"));
    g_assert_cmpuint(o2_mace_card_copy(qts, MACE_NATIVE_DMA + 0x120),
                     ==, MEMTX_OK);
    g_assert_cmphex(o2_mace_card_get(qts, "test-read-data") & 0xff, ==, 0xa0);
    g_assert_cmpuint(o2_mace_card_copy(qts, 0x123), ==, MEMTX_OK);
    g_assert_cmphex(o2_mace_card_get(qts, "test-read-data") & 0xff, ==, 0xa0);
    g_assert_cmpuint(o2_mace_card_fill(qts, 0x123, 0x99), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x123), ==, 0x99);
    g_assert_cmpuint(o2_mace_card_fill(qts, 0x123, 0xa0), ==, MEMTX_OK);

    qtest_writeq(qts, CRIME + 0x208 + 2 * 8, 0x100);
    qtest_writeq(qts, CRIME + 0x208, 0x108);
    qtest_writeq(qts, CRIME + 0x208 + 6 * 8, 0x110);
    qtest_writeq(qts, CRIME + 0x208 + 4 * 8, 0x118);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x123), ==, 0xc2);
    g_assert_cmphex(qtest_readb(qts, RAM_NO_ECC + 0x123), ==, 0xc2);
    g_assert_cmphex(qtest_readb(qts, 0x123), ==, 0xc2);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x10000123), ==, 0xa0);
    g_assert_cmpint(memtx_readb(qts, 0x10000123, &byte), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x20000123), ==, 0xf6);
    g_assert_cmphex(qtest_readb(qts, RAM_HIGH + 0x123), ==, 0xf6);
    for (unsigned i = 0; i < 3; i++) {
        static const uint64_t addresses[] = {
            0x40000120, 0x50000120, 0x60000120,
        };
        static const uint64_t lower[] = {
            0x00000123, 0x10000123, 0x20000123,
        };
        static const uint8_t expected[] = { 0xc2, 0xa0, 0xf6 };

        g_assert_cmpuint(o2_mace_card_copy(qts, addresses[i]), ==, MEMTX_OK);
        g_assert_cmphex(o2_mace_card_get(qts, "test-read-data") & 0xff,
                        ==, expected[i]);
        g_assert_cmpuint(o2_mace_card_copy(qts, lower[i]), ==, MEMTX_OK);
        g_assert_cmphex(o2_mace_card_get(qts, "test-read-data") & 0xff,
                        ==, expected[i]);
    }
    g_assert_cmpuint(o2_mace_card_fill(qts, 0x50000120, 0x5a), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x10000123), ==, 0x5a);
    o2_program_banks(qts);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x123), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x10000123), ==, 0xc2);

    qtest_writeb(qts, RAM_FULL + 0x38000123, 0x77);
    qtest_writeb(qts, RAM_FULL + 0x3a000123, 0x7b);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x38000123), ==, 0x77);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x3a000123), ==, 0x7b);
    qtest_writeq(qts, CRIME + 0x208 + 7 * 8, 0x1c);
    for (unsigned i = 0; i < 2; i++) {
        static const uint64_t addresses[] = { 0x78000123, 0x7a000123 };
        static const uint64_t lower[] = { 0x38000123, 0x3a000123 };

        g_assert_cmpuint(o2_mace_card_copy(qts, addresses[i]), ==,
                         MEMTX_ERROR);
        g_assert_cmpuint(o2_mace_card_fill(qts, addresses[i], 0x99), ==,
                         MEMTX_ERROR);
        g_assert_cmpuint(o2_mace_card_copy(qts, lower[i]), ==, MEMTX_ERROR);
        g_assert_cmpuint(o2_mace_card_fill(qts, lower[i], 0x99), ==,
                         MEMTX_ERROR);
    }
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x123), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x10000123), ==, 0xc2);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x20000123), ==, 0xe4);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x30000123), ==, 0xf6);
    qtest_writeq(qts, CRIME + 0x208 + 7 * 8, 0x11c);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x38000123), ==, 0x77);
    g_assert_cmphex(qtest_readb(qts, RAM_FULL + 0x3a000123), ==, 0x7b);

    migrate = qtest_qmp(qts,
        "{'execute':'migrate','arguments':{'uri':'unix:/nonexistent'}}");
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(migrate, "error"),
                                         "desc"),
                            "o2-test/eight-bank-profile"));
    save_reply = qtest_hmp(qts, "savevm mace-bank-dma");
    g_assert_nonnull(strstr(save_reply, "o2-test/eight-bank-profile"));
    qtest_quit(qts);
}

static void test_mace_banked_dma_fixture_lifetime(void)
{
    QTestState *qts = o2_mace_banked_dma_start();
    g_autoptr(QDict) crime_target = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/crime',"
        "'property':'sgi-crime.decoded-ram[0]'}}");
    g_autoptr(QDict) unrealize = NULL;

    unrealize = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'" MACE_DMA_CARD "',"
        "'property':'realized','value':false}}");
    if (!qdict_haskey(unrealize, "error")) {
        g_autoptr(QDict) raw_after = qtest_qmp(qts,
            "{'execute':'qom-set','arguments':{'path':'" MACE_DMA_CARD "',"
            "'property':'dma-r32','value':true}}");
        g_autoptr(QDict) link_after = qtest_qmp(qts,
            "{'execute':'qom-set','arguments':{'path':'" MACE_DMA_CARD "',"
            "'property':'dma-memory','value':%s}}",
            qdict_get_str(crime_target, "return"));
        g_autoptr(QDict) latch_after = qtest_qmp(qts,
            "{'execute':'qom-set','arguments':{'path':'" MACE_DMA_CARD "',"
            "'property':'pci-only-latch','value':false}}");

        g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(raw_after,
                                         "error"), "desc"), "locked"));
        g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(link_after,
                                         "error"), "desc"), "locked"));
        g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(latch_after,
                                         "error"), "desc"), "locked"));
        {
            g_autoptr(QDict) again = qtest_qmp(qts,
                "{'execute':'qom-set','arguments':{'path':'" MACE_DMA_CARD "',"
                "'property':'realized','value':true}}");
            g_autoptr(QDict) status = qtest_qmp(qts,
                "{'execute':'query-status'}");
            g_autoptr(QDict) still_locked = qtest_qmp(qts,
                "{'execute':'qom-get','arguments':{'path':'" MACE_DMA_CARD "',"
                "'property':'pci-only-latch'}}");

            g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(again,
                                             "error"), "desc"),
                                    "cannot be realized again"));
            g_assert_nonnull(qdict_get_qdict(status, "return"));
            g_assert_true(qdict_get_bool(still_locked, "return"));
        }
    }
    qtest_quit(qts);
}

static void test_mace_banked_dma_fixture_rejections(void)
{
    const char *extra_card[] = {
        qtest_qemu_binary(NULL), "-M",
        "o2-test,bank-profile=8x128m-synthetic,mace-bank-dma-fixture=on",
        "-m", "1G", "-accel", "qtest", "-S", "-nodefaults",
        "-display", "none", "-qmp", "stdio", "-device",
        "sgi-bridge-dma-test,bus=mace.0,addr=2", NULL,
    };
    const char *default_card[] = {
        qtest_qemu_binary(NULL), "-M",
        "o2-test,bank-profile=8x128m-synthetic", "-m", "1G",
        "-accel", "qtest", "-S", "-nodefaults", "-display", "none",
        "-qmp", "stdio", "-device",
        "sgi-bridge-dma-test,bus=mace.0,addr=3", NULL,
    };
    const char *scsi_target[] = {
        qtest_qemu_binary(NULL), "-M",
        "o2-test,bank-profile=8x128m-synthetic,mace-bank-dma-fixture=on",
        "-m", "1G", "-accel", "qtest", "-S", "-nodefaults",
        "-display", "none", "-qmp", "stdio", "-device",
        "scsi-cd,bus=aic0.0,scsi-id=0", NULL,
    };
    QTestState *qts = qtest_init(MEMTX_TEST_DEVICE
        "-M o2-test,bank-profile=8x128m-synthetic -cpu R10000 "
        "-m 1G -nodefaults -accel qtest -S");
    g_autoptr(QDict) mace = NULL;
    g_autoptr(QDict) crime = NULL;
    g_autoptr(QDict) hotplug = NULL;

    g_assert_false(qtest_qom_get_bool(qts, "/machine",
                                       "mace-bank-dma-fixture"));
    mace = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/mace',"
        "'property':'ram'}}");
    crime = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/crime',"
        "'property':'sgi-crime.decoded-ram[0]'}}");
    g_assert_cmpstr(qdict_get_str(mace, "return"), ==,
                    qdict_get_str(crime, "return"));
    qtest_quit(qts);

    qts = qtest_init(MEMTX_TEST_DEVICE
        "-M o2-test -cpu R10000 -m 128M -nodefaults -accel qtest -S");
    qobject_unref(mace);
    qobject_unref(crime);
    mace = NULL;
    crime = NULL;
    mace = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/mace',"
        "'property':'ram'}}");
    crime = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/crime',"
        "'property':'ram'}}");
    g_assert_cmpstr(qdict_get_str(mace, "return"), ==,
                    qdict_get_str(crime, "return"));
    qtest_quit(qts);

    assert_o2_banked_rejected("o2-test,mace-bank-dma-fixture=on", "64M",
                              "-accel", "qtest", "requires QTest");
    assert_o2_banked_rejected("o2-test,mace-bank-dma-fixture=on", "128M",
                              "-accel", "qtest", "requires QTest");
    assert_o2_banked_rejected("o2-test,mace-bank-dma-fixture=on", "1G",
                              "-accel", "qtest", "requires QTest");
    assert_o2_banked_rejected(
        "o2-test,bank-profile=8x128m-synthetic,mace-bank-dma-fixture=on",
        "1G", "-accel", "tcg", "requires QTest");
    assert_o2_banked_rejected(
        "o2-test,bank-profile=8x128m-synthetic,mace-bank-dma-fixture=on,"
        "password-clear-input=0,rom-image=unused", "1G", "-accel", "qtest",
        "no ROM");
    assert_launch_rejected(default_card, "does not support attached PCI");
    assert_launch_rejected(extra_card, "slot 2 function 0 not available");
    assert_launch_rejected(scsi_target,
                           "MACE bank DMA fixture rejects SCSI targets");

    qts = qtest_init(MEMTX_TEST_DEVICE
        "-M o2-test,bank-profile=8x128m-synthetic,"
        "mace-bank-dma-fixture=on -cpu R10000 -m 1G "
        "-nodefaults -accel qtest -S");
    hotplug = qtest_qmp(qts,
        "{'execute':'device_add','arguments':"
        "{'driver':'sgi-bridge-dma-test','bus':'mace.0',"
        "'addr':'2','id':'extra'}}");
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(hotplug, "error"),
                                         "desc"),
                            "mace.0' does not support hotplugging"));
    qtest_quit(qts);
}

static O2StorageTest o2_storage_start_accel_full(bool disk,
                                                 const char *accel,
                                                 unsigned controller,
                                                 bool low_eth_dma,
                                                 bool trace_scsi,
                                                 const char *disk_path)
{
    static const uint8_t rom[] = { 0, 0, 0, 0 };
    O2StorageTest t = { 0 };
    g_autofree char *target = disk ? g_strdup_printf(
        "-drive if=none,id=disk0,file=%s,format=raw "
        "-device scsi-hd,id=storage0,drive=disk0,bus=aic%u.0,"
        "scsi-id=0,lun=0", disk_path ? disk_path : "null-co://",
        controller) : g_strdup("");
    g_autofree char *trace_arg = NULL;

    t.dir = g_dir_make_tmp("o2-storage-XXXXXX", NULL);
    g_assert_nonnull(t.dir);
    t.rom = g_build_filename(t.dir, "synthetic-rom", NULL);
    t.events = g_build_filename(t.dir, "dma.jsonl", NULL);
    if (trace_scsi) {
        t.trace = g_build_filename(t.dir, "target.trace", NULL);
        trace_arg = g_strdup_printf(
            "-trace enable=scsi_disk_new_request,file=%s", t.trace);
    } else {
        trace_arg = g_strdup("");
    }
    g_assert_true(g_file_set_contents(t.rom, (const char *)rom,
                                      sizeof(rom), NULL));
    t.qts = qtest_initf(MEMTX_TEST_DEVICE
        "-M o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,prom-sb64-reset=on,"
        "gbe11-registers=on,prom-id-word=on,"
        "password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0,rom-pad-byte=238,"
        "diagnostic-lower-pci-byte-identity=on%s,"
        "storage-diagnostic-event-file=%s "
        "-cpu R10000 -m 1G -nodefaults -accel %s -S %s %s",
        t.rom, low_eth_dma ? ",mace-eth-low-dma-compat=on" : "",
        t.events, accel, target, trace_arg);
    return t;
}

static O2StorageTest o2_storage_start_accel_eth(bool disk,
                                                const char *accel,
                                                bool low_eth_dma)
{
    return o2_storage_start_accel_full(disk, accel, 0, low_eth_dma,
                                       false, NULL);
}

static O2StorageTest o2_storage_start_accel(bool disk, const char *accel,
                                          unsigned controller,
                                          bool trace_scsi,
                                          const char *disk_path)
{
    return o2_storage_start_accel_full(disk, accel, controller, false,
                                       trace_scsi, disk_path);
}

static O2StorageTest o2_storage_start(bool disk)
{
    return o2_storage_start_accel(disk, "qtest", 0, false, NULL);
}

static char *o2_storage_stop(O2StorageTest *t)
{
    char *events = NULL;

    qtest_quit(t->qts);
    g_assert_true(g_file_get_contents(t->events, &events, NULL, NULL));
    g_assert_cmpint(g_unlink(t->events), ==, 0);
    g_assert_cmpint(g_unlink(t->rom), ==, 0);
    if (t->trace) {
        g_assert_cmpint(g_unlink(t->trace), ==, 0);
    }
    g_assert_cmpint(g_rmdir(t->dir), ==, 0);
    g_free(t->events);
    g_free(t->rom);
    g_free(t->trace);
    g_free(t->dir);
    return events;
}

static void o2_storage_configure_aic(QTestState *qts, unsigned slot)
{
    o2_aic_config_write(qts, slot, 0x14,
                        0x80000000 + (slot - 1) * 0x1000);
    o2_aic_config_write(qts, slot, 4, 6);
}

static uint64_t o2_storage_pio(uint64_t address)
{
    return address ^ 3;
}

static uint64_t o2_storage_aic(unsigned reg)
{
    return o2_storage_pio(AIC_PCI_VIEW + reg);
}

static uint64_t o2_storage_aic_slot(unsigned slot, unsigned reg)
{
    return o2_storage_pio(AIC_PCI_VIEW + (slot - 1) * 0x1000 + reg);
}

static uint32_t o2_pio_test_last(QTestState *qts, const char *property)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/peripheral/card',"
        "'property':%s}}", property);

    if (qdict_haskey(reply, "error")) {
        g_error("PIO fixture property %s: %s", property,
                qdict_get_str(qdict_get_qdict(reply, "error"), "desc"));
    }
    g_assert_nonnull(qdict_get(reply, "return"));
    return qdict_get_int(reply, "return");
}

static void test_storage_pio_byte_lanes(void)
{
    static const uint64_t views[] = { AIC_PCI_VIEW, O2_PCI_HIGH_VIEW };
    static const unsigned groups[] = { 0, 0x84 };
    uint8_t expected[2][4] = {
        { 0x10, 0x11, 0x12, 0x13 },
        { 0x84, 0x85, 0x86, 0x87 },
    };
    QTestState *qts = qtest_init(MEMTX_TEST_DEVICE
        "-M none -cpu R10000 -accel qtest -S -nodefaults "
        "-device sgi-mace,id=mace,pio-byte-test=on "
        "-device sgi-mace-pio-test,id=card,bus=mace.0,addr=3");
    uint8_t byte;
    uint32_t word, reads, writes;

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/mace", 0,
                          0x1f000000);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/mace", 1,
                          AIC_PCI_VIEW);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/mace", 2,
                          O2_PCI_HIGH_VIEW);
    qtest_writel(qts, MACE_CFGADDR, 3U << 11 | 0x10);
    g_assert_cmpint(memtx_writel(qts, MACE_CFGDATA, O2_PCI_PIO_TEST_BAR),
                    ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(qts, MACE_CFGDATA), ==,
                    O2_PCI_PIO_TEST_BAR);
    qtest_writel(qts, MACE_CFGADDR, 3U << 11 | 4);
    g_assert_cmpint(memtx_writel(qts, MACE_CFGDATA, 2), ==, MEMTX_OK);

    for (unsigned view = 0; view < ARRAY_SIZE(views); view++) {
        for (unsigned group = 0; group < ARRAY_SIZE(groups); group++) {
            uint64_t base = views[view] + 0x1000 + groups[group];

            for (unsigned lane = 0; lane < 4; lane++) {
                unsigned reg = groups[group] + (lane ^ 3);

                g_assert_cmphex(qtest_readb(qts, base + lane), ==,
                                expected[group][lane ^ 3]);
                g_assert_cmpuint(o2_pio_test_last(qts,
                    "test-pio-last-offset"), ==, reg);
                g_assert_cmpuint(o2_pio_test_last(qts,
                    "test-pio-last-size"), ==, 1);
            }
        }
    }
    for (unsigned view = 0; view < ARRAY_SIZE(views); view++) {
        for (unsigned group = 0; group < ARRAY_SIZE(groups); group++) {
            uint64_t base = views[view] + 0x1000 + groups[group];

            for (unsigned lane = 0; lane < 4; lane++) {
                unsigned reg = groups[group] + (lane ^ 3);
                uint8_t value = 0x40 + 8 * view + 4 * group + lane;

                qtest_writeb(qts, base + lane, value);
                g_assert_cmpuint(o2_pio_test_last(qts,
                    "test-pio-last-offset"), ==, reg);
                g_assert_cmpuint(o2_pio_test_last(qts,
                    "test-pio-last-size"), ==, 1);
                expected[group][lane ^ 3] = value;
                for (unsigned other_view = 0;
                     other_view < ARRAY_SIZE(views); other_view++) {
                    for (unsigned other_reg = 0; other_reg < 4;
                         other_reg++) {
                        uint64_t address = views[other_view] + 0x1000 +
                            groups[group] + (other_reg ^ 3);

                        g_assert_cmphex(qtest_readb(qts, address), ==,
                                        expected[group][other_reg]);
                    }
                }
            }
        }
    }
    g_assert_cmphex(qtest_readb(qts, AIC_PCI_VIEW + 0x1007), ==, 0);
    g_assert_cmphex(qtest_readb(qts, AIC_PCI_VIEW + 0x1080), ==, 0);
    g_assert_cmphex(qtest_readb(qts, O2_PCI_HIGH_VIEW + 0x108b), ==, 0);
    reads = o2_pio_test_last(qts, "test-pio-read-count");
    writes = o2_pio_test_last(qts, "test-pio-write-count");

    g_assert_cmpint(memtx_readl(qts, AIC_PCI_VIEW + 0x1080, &word),
                    ==, MEMTX_OK);
    g_assert_cmpuint(o2_pio_test_last(qts, "test-pio-last-offset"),
                     ==, 0x80);
    g_assert_cmpuint(o2_pio_test_last(qts, "test-pio-last-size"), ==, 4);
    g_assert_cmpuint(o2_pio_test_last(qts, "test-pio-read-count"),
                     ==, reads + 1);
    g_assert_cmpuint(o2_pio_test_last(qts, "test-pio-write-count"),
                     ==, writes);
    g_assert_cmpint(memtx_writel(qts, O2_PCI_HIGH_VIEW + 0x1080, 0x12345678),
                    ==, MEMTX_OK);
    g_assert_cmpuint(o2_pio_test_last(qts, "test-pio-last-offset"),
                     ==, 0x80);
    g_assert_cmpuint(o2_pio_test_last(qts, "test-pio-last-size"), ==, 4);
    g_assert_cmpuint(o2_pio_test_last(qts, "test-pio-read-count"),
                     ==, reads + 1);
    g_assert_cmpuint(o2_pio_test_last(qts, "test-pio-write-count"),
                     ==, writes + 1);
    g_assert_cmpint(memtx_readb(qts, AIC_PCI_VIEW + 16 * MiB, &byte),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_readb(qts, O2_PCI_HIGH_VIEW + 2 * GiB, &byte),
                    !=, MEMTX_OK);
    qtest_quit(qts);

    qts = qtest_init(MEMTX_TEST_DEVICE
        "-M none -cpu R10000 -accel qtest -S -nodefaults "
        "-device sgi-mace,id=mace "
        "-device sgi-mace-pio-test,id=card,bus=mace.0,addr=3");
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/mace", 0,
                          0x1f000000);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/mace", 1,
                          AIC_PCI_VIEW);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/mace", 2,
                          O2_PCI_HIGH_VIEW);
    g_assert_false(qtest_qom_get_bool(qts, "/machine/peripheral/mace",
                                      "pio-byte-test"));
    qtest_writel(qts, MACE_CFGADDR, 3U << 11 | 0x10);
    g_assert_cmpint(memtx_writel(qts, MACE_CFGDATA, O2_PCI_PIO_TEST_BAR),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_readb(qts, AIC_PCI_VIEW + 0x1000, &byte),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_readb(qts, O2_PCI_HIGH_VIEW + 0x1000, &byte),
                    !=, MEMTX_OK);
    qtest_quit(qts);
}

static void o2_storage_check_hcntrl(QTestState *qts, uint64_t view,
                                    const uint8_t expected[2])
{
    for (unsigned slot = 1; slot <= 2; slot++) {
        g_assert_cmphex(qtest_readb(qts, view + slot * 0x1000 + 0x84),
                        ==, expected[slot - 1]);
    }
}

static void test_storage_pio_aic_hcntrl(void)
{
    O2StorageTest t = o2_storage_start(false);
    g_autofree char *events = NULL;
    uint8_t expected[2];
    uint8_t byte;
    uint32_t word;

    for (unsigned slot = 1; slot <= 2; slot++) {
        uint64_t offset = 0x1000 * slot;
        uint32_t bar = 0x80000000 + offset;

        o2_aic_config_write(t.qts, slot, 0x14, bar);
        g_assert_cmphex(o2_aic_config_read(t.qts, slot, 0x14), ==, bar);
        o2_aic_config_write(t.qts, slot, 4, 6);
        g_assert_cmphex(o2_aic_config_read(t.qts, slot, 4) & 7, ==, 6);
        expected[slot - 1] = qtest_readb(t.qts,
            O2_PCI_HIGH_VIEW + offset + 0x84);
        g_assert_cmphex(qtest_readb(t.qts,
            AIC_PCI_VIEW + offset + 0x87), ==, 0xff);
    }
    qtest_writeb(t.qts, AIC_PCI_VIEW + 0x1084, 0x14);
    expected[0] = 0x14;
    o2_storage_check_hcntrl(t.qts, O2_PCI_HIGH_VIEW, expected);
    qtest_writeb(t.qts, AIC_PCI_VIEW + 0x2084, 0x06);
    expected[1] = 0x06;
    o2_storage_check_hcntrl(t.qts, O2_PCI_HIGH_VIEW, expected);

    for (unsigned slot = 1; slot <= 2; slot++) {
        uint64_t offset = 0x1000 * slot;

        g_assert_cmpint(memtx_readl(t.qts,
            AIC_PCI_VIEW + offset + 0x84, &word), !=, MEMTX_OK);
        o2_storage_check_hcntrl(t.qts, O2_PCI_HIGH_VIEW, expected);
        g_assert_cmpint(memtx_writel(t.qts,
            O2_PCI_HIGH_VIEW + offset + 0x84, 0), !=, MEMTX_OK);
        o2_storage_check_hcntrl(t.qts, AIC_PCI_VIEW, expected);
    }
    qtest_writeb(t.qts, O2_PCI_HIGH_VIEW + 0x1084, 0x04);
    expected[0] = 0x04;
    o2_storage_check_hcntrl(t.qts, AIC_PCI_VIEW, expected);
    qtest_writeb(t.qts, O2_PCI_HIGH_VIEW + 0x2084, 0x14);
    expected[1] = 0x14;
    o2_storage_check_hcntrl(t.qts, AIC_PCI_VIEW, expected);

    for (unsigned slot = 1; slot <= 2; slot++) {
        uint64_t offset = 0x1000 * slot;

        o2_aic_config_write(t.qts, slot, 4, 4);
        g_assert_cmpint(memtx_readb(t.qts,
            AIC_PCI_VIEW + offset + 0x84, &byte), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readb(t.qts,
            O2_PCI_HIGH_VIEW + offset + 0x84, &byte), !=, MEMTX_OK);
    }
    events = o2_storage_stop(&t);
    g_assert_nonnull(strstr(events, "\"complete\":true"));
}

static void o2_storage_emit(uint8_t *program, size_t *n, uint32_t word)
{
    for (unsigned byte = 0; byte < 4; byte++) {
        program[(*n)++] = word >> (byte * 8);
    }
}

static uint32_t o2_storage_alu_word(uint8_t opcode, uint8_t immediate,
                                    uint8_t source, uint8_t destination)
{
    return immediate | ((uint32_t)source << 8) |
           ((uint32_t)destination << 16) | ((uint32_t)opcode << 25);
}

static uint32_t o2_storage_branch_word(uint8_t opcode, uint8_t immediate,
                                       uint8_t source, unsigned target)
{
    return immediate | ((uint32_t)source << 8) |
           ((uint32_t)target << 16) | ((uint32_t)opcode << 25);
}

static void o2_storage_emit_value(uint8_t *program, size_t *n,
                                  uint8_t value, uint8_t reg)
{
    /* Immediate zero selects ACCUM; AND from ALLZEROS writes literal zero. */
    o2_storage_emit(program, n,
                    o2_storage_alu_word(value ? 0 : 1,
                                        value ? value : 0xff, 0x6a, reg));
}

typedef struct O2QueuedFetchPC {
    unsigned table;
    unsigned prefix;
    unsigned segments;
    unsigned cdb;
    unsigned command;
    unsigned first_segment;
    unsigned second_segment;
    unsigned completion;
    unsigned queue;
    unsigned interrupt;
    unsigned invalid_branch;
    unsigned invalid;
} O2QueuedFetchPC;

static void o2_storage_append_queued_command(uint8_t *program, size_t *n,
                                             O2QueuedFetchPC *pc,
                                             bool write)
{
    size_t terminal_branch;
    size_t early_status_branch;
    size_t cdb_done_branch;
    size_t segment_branches[3];
    size_t skip_second;
    unsigned wait_at;

    o2_storage_emit_value(program, n, 0, 0x93);
    o2_storage_emit_value(program, n, 3, 0x90);
    o2_storage_emit(program, n,
                    o2_storage_alu_word(1, 0xff, 0xa1, 0x24));
    o2_storage_emit(program, n,
                    o2_storage_alu_word(0, 7, 0xa3, 0x05));
    o2_storage_emit(program, n,
                    o2_storage_alu_word(1, 7, 0xa3, 0x26));
    o2_storage_emit_value(program, n, 4, 0x90);
    for (unsigned byte = 0; byte < 4; byte++) {
        o2_storage_emit(program, n,
            o2_storage_alu_word(1, 0xff, 0xa0 + byte, 0x88 + byte));
    }
    for (unsigned byte = 0; byte < 3; byte++) {
        o2_storage_emit(program, n,
            o2_storage_alu_word(1, 0xff, 0xa4 + byte, 0x8c + byte));
        o2_storage_emit(program, n,
            o2_storage_alu_word(1, 0xff, 0xa4 + byte, 0x08 + byte));
    }
    o2_storage_emit_value(program, n, 0x08, 0x01);
    o2_storage_emit_value(program, n, 0, 0x00);
    o2_storage_emit_value(program, n, 0xa0, 0x03);
    o2_storage_emit_value(program, n, 0x5a, 0x00);
    o2_storage_emit(program, n,
                    o2_storage_alu_word(0, 0x80, 0x26, 0x06));
    o2_storage_emit_value(program, n, 0x40, 0x0c);
    o2_storage_emit_value(program, n, 0x80, 0x03);
    o2_storage_emit_value(program, n, 5, 0x90);
    o2_storage_emit_value(program, n, 0x80, 0x9a);
    wait_at = *n / 4;
    cdb_done_branch = *n;
    o2_storage_emit(program, n,
                    o2_storage_branch_word(0xf, 0xff, 0x24, 0));
    o2_storage_emit(program, n,
                    o2_storage_alu_word(1, 0xff, 0xa0, 0x06));
    o2_storage_emit(program, n,
                    o2_storage_alu_word(3, 0xff, 0x24, 0x24));
    o2_storage_emit(program, n,
                    o2_storage_branch_word(8, 0, 0x65, wait_at));
    stl_le_p(program + cdb_done_branch,
             o2_storage_branch_word(0xf, 0xff, 0x24, *n / 4));
    o2_storage_emit_value(program, n, 0, 0x9a);
    pc->command = *n / 4;
    early_status_branch = *n;
    o2_storage_emit(program, n, o2_storage_branch_word(0xd, 2, 0x0b, 0));
    o2_storage_emit_value(program, n, write ? 0 : 0x40, 0x03);
    o2_storage_emit_value(program, n, write ? 0x3c : 0x38, 0x93);

    wait_at = *n / 4;
    o2_storage_emit(program, n,
                    o2_storage_branch_word(0xf, 8, 0x94, wait_at));
    o2_storage_emit_value(program, n, write ? 0x34 : 0x30, 0x93);
    o2_storage_emit_value(program, n, 4, 0x90);
    for (unsigned byte = 0; byte < 3; byte++) {
        segment_branches[byte] = *n;
        o2_storage_emit(program, n,
            o2_storage_branch_word(0xd, 0xff, 0xac + byte, 0));
    }
    skip_second = *n;
    o2_storage_emit(program, n, o2_storage_branch_word(8, 0, 0x65, 0));
    pc->first_segment = *n / 4;
    for (unsigned byte = 0; byte < 3; byte++) {
        stl_le_p(program + segment_branches[byte],
                 o2_storage_branch_word(0xd, 0xff, 0xac + byte, *n / 4));
    }
    for (unsigned byte = 0; byte < 4; byte++) {
        o2_storage_emit(program, n,
            o2_storage_alu_word(1, 0xff, 0xa8 + byte, 0x88 + byte));
    }
    for (unsigned byte = 0; byte < 3; byte++) {
        o2_storage_emit(program, n,
            o2_storage_alu_word(1, 0xff, 0xac + byte, 0x8c + byte));
        o2_storage_emit(program, n,
            o2_storage_alu_word(1, 0xff, 0xac + byte, 0x08 + byte));
    }
    o2_storage_emit_value(program, n, write ? 0x3c : 0x38, 0x93);
    pc->second_segment = *n / 4;
    stl_le_p(program + skip_second,
             o2_storage_branch_word(8, 0, 0x65, *n / 4));

    wait_at = *n / 4;
    stl_le_p(program + early_status_branch,
             o2_storage_branch_word(0xd, 2, 0x0b, wait_at + 5));
    o2_storage_emit(program, n,
                    o2_storage_branch_word(0xd, 2, 0x0b, wait_at + 5));
    o2_storage_emit(program, n,
                    o2_storage_branch_word(0xf, 4, 0x0b, wait_at));
    o2_storage_emit(program, n, o2_storage_alu_word(0, 2, 0x93, 0x93));
    o2_storage_emit(program, n,
                    o2_storage_branch_word(0xf, 8, 0x94, wait_at + 3));
    o2_storage_emit(program, n,
                    o2_storage_branch_word(0xf, 2, 0x0b, wait_at + 4));
    o2_storage_emit_value(program, n, 0xc0, 0x03);
    o2_storage_emit(program, n, o2_storage_alu_word(1, 0xff, 0x06, 0x22));
    o2_storage_emit_value(program, n, 0xe0, 0x03);
    o2_storage_emit(program, n, o2_storage_alu_word(1, 0xff, 0x06, 0x23));
    o2_storage_emit(program, n,
                    o2_storage_branch_word(0xf, 8, 0x0c, wait_at + 9));

    o2_storage_emit_value(program, n, 4, 0x90);
    terminal_branch = *n;
    o2_storage_emit(program, n,
                    o2_storage_branch_word(0xd, 0x80, 0xaf, 0));
    o2_storage_emit_value(program, n, 1, 0x91);
    stl_le_p(program + terminal_branch,
             o2_storage_branch_word(0xd, 0x80, 0xaf, *n / 4));

    o2_storage_emit_value(program, n, 0, 0x93);
    o2_storage_emit_value(program, n, 0, 0x93);
    o2_storage_emit_value(program, n, 0, 0x88);
    for (unsigned byte = 1; byte < 4; byte++) {
        o2_storage_emit(program, n,
            o2_storage_alu_word(1, 0xff, 0x26 + byte, 0x88 + byte));
    }
    o2_storage_emit_value(program, n, 8, 0x8c);
    o2_storage_emit_value(program, n, 0, 0x8d);
    o2_storage_emit_value(program, n, 0, 0x8e);
    o2_storage_emit_value(program, n, 1, 0x93);
    for (unsigned byte = 0; byte < 7; byte++) {
        o2_storage_emit_value(program, n, 0xff, 0x99);
    }
    o2_storage_emit(program, n,
                    o2_storage_alu_word(1, 0xff, 0x20, 0x99));
    o2_storage_emit_value(program, n, 0x0a, 0x93);
    pc->completion = *n / 4;
    o2_storage_emit(program, n,
                    o2_storage_alu_word(1, 0xff, 0x20, 0x9d));
    pc->queue = *n / 4;
    o2_storage_emit_value(program, n, 2, 0x91);
    pc->interrupt = *n / 4;
}

/* The synthetic program owns these host records; the device sees byte DMA. */
static size_t o2_storage_build_queued_fetch(uint8_t *program,
                                            O2QueuedFetchPC *pc,
                                            bool run_command, bool write)
{
    size_t n = 0;
    size_t invalid_branch;

    o2_storage_emit(program, &n, o2_storage_alu_word(1, 0xff, 0x9b, 0x20));
    o2_storage_emit(program, &n, o2_storage_alu_word(5, 0x22, 0x20, 0x88));
    for (unsigned i = 1; i < 4; i++) {
        o2_storage_emit(program, &n,
                        o2_storage_alu_word(1, 0xff, 0x20 + i, 0x88 + i));
    }
    o2_storage_emit_value(program, &n, 4, 0x8c);
    o2_storage_emit_value(program, &n, 1, 0x93);
    o2_storage_emit_value(program, &n, 0x0c, 0x93);
    o2_storage_emit_value(program, &n, 0x04, 0x93);
    for (unsigned i = 0; i < 4; i++) {
        o2_storage_emit(program, &n,
                        o2_storage_alu_word(1, 0xff, 0x99, 0x30 + i));
    }
    pc->table = n / 4;
    invalid_branch = n;
    pc->invalid_branch = n / 4;
    o2_storage_emit(program, &n, o2_storage_branch_word(0xe, 0xff, 0x33, 0));

    for (unsigned i = 0; i < 4; i++) {
        o2_storage_emit(program, &n,
                        o2_storage_alu_word(1, 0xff, 0x30 + i, 0x88 + i));
    }
    o2_storage_emit_value(program, &n, 32, 0x8c);
    o2_storage_emit_value(program, &n, 1, 0x93);
    o2_storage_emit_value(program, &n, 0x0c, 0x93);
    o2_storage_emit_value(program, &n, 0x04, 0x93);
    o2_storage_emit_value(program, &n, 3, 0x90);
    o2_storage_emit_value(program, &n, 0x80, 0x9a);
    for (unsigned i = 0; i < 32; i++) {
        o2_storage_emit(program, &n,
                        o2_storage_alu_word(1, 0xff, 0x99, 0xa0));
    }
    o2_storage_emit_value(program, &n, 0, 0x9a);
    pc->prefix = n / 4;

    for (unsigned i = 0; i < 4; i++) {
        o2_storage_emit(program, &n,
                        o2_storage_alu_word(1, 0xff, 0xa4 + i, 0x88 + i));
    }
    o2_storage_emit_value(program, &n, 16, 0x8c);
    o2_storage_emit_value(program, &n, 1, 0x93);
    o2_storage_emit_value(program, &n, 0x0c, 0x93);
    o2_storage_emit_value(program, &n, 0x04, 0x93);
    o2_storage_emit_value(program, &n, 4, 0x90);
    o2_storage_emit_value(program, &n, 0x80, 0x9a);
    for (unsigned i = 0; i < 16; i++) {
        o2_storage_emit(program, &n,
                        o2_storage_alu_word(1, 0xff, 0x99, 0xa0));
    }
    o2_storage_emit_value(program, &n, 0, 0x9a);
    pc->segments = n / 4;

    o2_storage_emit_value(program, &n, 3, 0x90);
    for (unsigned i = 0; i < 4; i++) {
        o2_storage_emit(program, &n,
                        o2_storage_alu_word(1, 0xff, 0xa8 + i, 0x88 + i));
    }
    o2_storage_emit_value(program, &n, 12, 0x8c);
    o2_storage_emit_value(program, &n, 1, 0x93);
    o2_storage_emit_value(program, &n, 0x0c, 0x93);
    o2_storage_emit_value(program, &n, 0x04, 0x93);
    o2_storage_emit_value(program, &n, 5, 0x90);
    o2_storage_emit_value(program, &n, 0x80, 0x9a);
    for (unsigned i = 0; i < 12; i++) {
        o2_storage_emit(program, &n,
                        o2_storage_alu_word(1, 0xff, 0x99, 0xa0));
    }
    o2_storage_emit_value(program, &n, 0, 0x9a);
    pc->cdb = n / 4;
    if (run_command) {
        o2_storage_append_queued_command(program, &n, pc, write);
    } else {
        o2_storage_emit_value(program, &n, 1, 0x91);
    }

    pc->invalid = n / 4;
    o2_storage_emit_value(program, &n, 0xee, 0x25);
    o2_storage_emit_value(program, &n, 1, 0x91);
    stl_le_p(program + invalid_branch,
             o2_storage_branch_word(0xe, 0xff, 0x33, pc->invalid));
    return n;
}

typedef struct O2CompletionPC {
    unsigned write[3];
    unsigned queue;
    unsigned interrupt;
} O2CompletionPC;

static size_t o2_storage_build_completion_program(uint8_t *program,
                                                  O2CompletionPC *pc)
{
    static const uint8_t ids[] = { 2, 4, 6 };
    static const uint8_t lanes[] = { 7, 3, 7 };
    size_t n = 0;

    for (unsigned record = 0; record < ARRAY_SIZE(ids); record++) {
        o2_storage_emit_value(program, &n, 0, 0x93);
        o2_storage_emit_value(program, &n, record == 2 ? 0xf8 : 0, 0x88);
        for (unsigned byte = 1; byte < 4; byte++) {
            o2_storage_emit(program, &n,
                o2_storage_alu_word(1, 0xff, 0x20 + byte, 0x88 + byte));
        }
        o2_storage_emit_value(program, &n, 8, 0x8c);
        o2_storage_emit_value(program, &n, 1, 0x93);
        for (unsigned byte = 0; byte < 8; byte++) {
            o2_storage_emit_value(program, &n,
                                  byte == lanes[record] ? ids[record] : 0xff,
                                  0x99);
        }
        o2_storage_emit_value(program, &n, 0x0a, 0x93);
        pc->write[record] = n / 4;
    }
    for (unsigned record = 0; record < ARRAY_SIZE(ids); record++) {
        o2_storage_emit_value(program, &n, ids[record], 0x9d);
    }
    pc->queue = n / 4;
    o2_storage_emit_value(program, &n, 2, 0x91);
    pc->interrupt = n / 4;
    return n;
}

static uint16_t o2_storage_seq_pc(QTestState *qts, unsigned slot)
{
    return qtest_readb(qts, o2_storage_aic_slot(slot, 0x62)) |
           qtest_readb(qts, o2_storage_aic_slot(slot, 0x63)) << 8;
}

static void o2_storage_load_stepped_program(QTestState *qts, unsigned slot,
                                             const uint8_t *program, size_t len)
{
    qtest_writeb(qts, o2_storage_aic_slot(slot, 0x87), 4);
    qtest_writeb(qts, o2_storage_aic_slot(slot, 0x60), 0x91);
    qtest_writeb(qts, o2_storage_aic_slot(slot, 0x62), 0);
    qtest_writeb(qts, o2_storage_aic_slot(slot, 0x63), 0);
    for (unsigned i = 0; i < len; i++) {
        qtest_writeb(qts, o2_storage_aic_slot(slot, 0x61), program[i]);
    }
    qtest_writeb(qts, o2_storage_aic_slot(slot, 0x60), 0x94);
    qtest_writeb(qts, o2_storage_aic_slot(slot, 0x62), 0);
    qtest_writeb(qts, o2_storage_aic_slot(slot, 0x63), 0);
    g_assert_cmphex(o2_storage_seq_pc(qts, slot), ==, 0);
}

static void o2_storage_step_once(QTestState *qts, unsigned slot)
{
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

    qtest_writeb(qts, o2_storage_aic_slot(slot, 0x87), 2);
    qtest_clock_timers(qts, true);
    do {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        qtest_clock_step(qts, 1);
    } while (!(qtest_readb(qts, o2_storage_aic_slot(slot, 0x87)) & 4));
}

static void o2_storage_step_to(QTestState *qts, unsigned slot,
                               unsigned target)
{
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

    while (o2_storage_seq_pc(qts, slot) != target) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        o2_storage_step_once(qts, slot);
        g_assert_cmphex(qtest_readb(qts, o2_storage_aic_slot(slot, 0x87)) & 4,
                        ==, 4);
        g_assert_cmphex(qtest_readb(qts, o2_storage_aic_slot(slot, 0x92)),
                        ==, 0);
    }
    g_assert_cmphex(o2_storage_seq_pc(qts, slot), ==, target);
}


static void o2_aic_assert_selection(QTestState *qts, unsigned slot,
                                     uint8_t status0, uint8_t status1,
                                     uint8_t interrupt, uint8_t signals)
{
    g_assert_cmphex(qtest_readb(qts, o2_storage_aic_slot(slot, 0x0b)),
                    ==, status0);
    g_assert_cmphex(qtest_readb(qts, o2_storage_aic_slot(slot, 0x0c)),
                    ==, status1);
    g_assert_cmphex(qtest_readb(qts, o2_storage_aic_slot(slot, 0x91)),
                    ==, interrupt);
    g_assert_cmphex(qtest_readb(qts, o2_storage_aic_slot(slot, 0x03)),
                    ==, signals);
    g_assert_cmphex(qtest_readb(qts, o2_storage_aic_slot(slot, 0x92)), ==, 0);
    g_assert_cmphex(qtest_readb(qts, o2_storage_aic_slot(slot, 0x9e)), ==, 0);
    g_assert_cmphex(qtest_readb(qts, o2_storage_aic_slot(slot, 0x87)) & 4,
                    ==, 4);
}

static void test_storage_high_id_selection_timeout(void)
{
    static const unsigned ids[] = { 8, 15 };
    uint8_t program[4], guarded[32], after[32];
    size_t length = 0;

    o2_storage_emit_value(program, &length, 0x40, 0x00); /* ENSELO */
    for (unsigned i = 0; i < sizeof(guarded); i++) {
        guarded[i] = 7 * i + 3;
    }
    for (unsigned permutation = 0; permutation < 2; permutation++) {
        for (unsigned setting = 0; setting < 4; setting++) {
            QTestState *qts = qtest_init(MEMTX_TEST_DEVICE
                "-M o2-test -cpu R10000 -m 64M -nodefaults -nic none "
                "-accel qtest -S "
                "-device scsi-cd,bus=aic0.0,scsi-id=0");
            int64_t interval = 256000000LL >> setting;
            unsigned first = permutation + 1, second = 3 - first;
            unsigned remaining = 1u << (second - 1);
            int64_t selection_epoch;

            /* -S leaves virtual callbacks disabled until qtest enables them. */
            qtest_clock_timers(qts, true);
            qtest_irq_intercept_out_named(qts, "/machine/crime", "cpu-ip");
            qtest_writel(qts, MACE_PCI + 8, 3);
            qtest_writeq(qts, CRIME + 0x18, 3ULL << 8);
            qtest_memwrite(qts, RAM_FULL + 0x100, guarded, sizeof(guarded));
            for (unsigned slot = 1; slot <= 2; slot++) {
                unsigned target = ids[(slot - 1) ^ permutation];

                o2_storage_configure_aic(qts, slot);
                o2_storage_load_stepped_program(qts, slot, program, length);
                qtest_writeb(qts, o2_storage_aic_slot(slot, 0x05),
                             target << 4 | 7);
                qtest_writeb(qts, o2_storage_aic_slot(slot, 0x02),
                             setting << 3 | 4);
                qtest_writeb(qts, o2_storage_aic_slot(slot, 0x11),
                             slot == 1 ? 0x80 : 0);
            }
            selection_epoch = qtest_clock_step(qts, 1);
            for (unsigned slot = 1; slot <= 2; slot++) {
                qtest_writeb(qts, o2_storage_aic_slot(slot, 0x87), 2);
            }
            qtest_clock_step(qts, 1);
            for (unsigned slot = 1; slot <= 2; slot++) {
                unsigned target = ids[(slot - 1) ^ permutation];

                g_assert_cmphex(qtest_readb(qts,
                                            o2_storage_aic_slot(slot, 0x05)),
                                ==, target << 4 | 7);
                o2_aic_assert_selection(qts, slot, 0x10, 0, 0, 0x08);
            }
            o2_aic_assert_irqs(qts, 0, 0, false);
            qtest_clock_set(qts, selection_epoch + interval - 1);
            for (unsigned slot = 1; slot <= 2; slot++) {
                o2_aic_assert_selection(qts, slot, 0x10, 0, 0, 0x08);
            }
            o2_aic_assert_irqs(qts, 0, 0, false);
            g_assert_cmpuint(qtest_get_irq_raise_count(qts, 1), ==, 0);
            qtest_clock_step(qts, 1);
            o2_aic_assert_selection(qts, 1, 0x10, 0x80, 4, 0);
            o2_aic_assert_selection(qts, 2, 0x10, 0x80, 0, 0);
            o2_aic_assert_irqs(qts, 1, 1, true);
            g_assert_cmpuint(qtest_get_irq_raise_count(qts, 1), ==, 1);
            /* A live source defeats an early host-latch clear. */
            qtest_writeb(qts, o2_storage_aic_slot(1, 0x92), 4);
            o2_aic_assert_selection(qts, 1, 0x10, 0x80, 4, 0);
            o2_aic_assert_irqs(qts, 1, 1, true);
            qtest_writeb(qts, o2_storage_aic_slot(1, 0x87), 4);
            o2_aic_assert_irqs(qts, 0, 0, false);
            qtest_writeb(qts, o2_storage_aic_slot(1, 0x87), 6);
            o2_aic_assert_irqs(qts, 1, 1, true);
            qtest_writel(qts, MACE_PCI + 8, 2);
            o2_aic_assert_irqs(qts, 1, 0, false);
            qtest_writel(qts, MACE_PCI + 8, 3);
            o2_aic_assert_irqs(qts, 1, 1, true);
            qtest_writeq(qts, CRIME + 0x18, 1ULL << 9);
            o2_aic_assert_irqs(qts, 1, 1, false);
            qtest_writeq(qts, CRIME + 0x18, 3ULL << 8);
            o2_aic_assert_irqs(qts, 1, 1, true);
            /* Unmask the second controller's already expired selection. */
            qtest_writeb(qts, o2_storage_aic_slot(2, 0x11), 0x80);
            o2_aic_assert_selection(qts, 2, 0x10, 0x80, 4, 0);
            o2_aic_assert_irqs(qts, 3, 3, true);
            qtest_clock_step(qts, 399);
            for (unsigned slot = 1; slot <= 2; slot++) {
                o2_aic_assert_selection(qts, slot, 0x10, 0x80, 4, 0);
            }
            qtest_clock_step(qts, 1);
            for (unsigned slot = 1; slot <= 2; slot++) {
                qtest_writeb(qts, o2_storage_aic_slot(slot, 0x11), 0x88);
                o2_aic_assert_selection(qts, slot, 0x10, 0x88, 4, 0);
            }
            /* Keep BUSFREE alive after clearing only SELTO on this chip. */
            qtest_writeb(qts, o2_storage_aic_slot(first, 0x0c), 0x80);
            qtest_writeb(qts, o2_storage_aic_slot(first, 0x92), 4);
            o2_aic_assert_selection(qts, first, 0x10, 0x08, 4, 0);
            o2_aic_assert_selection(qts, second, 0x10, 0x88, 4, 0);
            o2_aic_assert_irqs(qts, 3, 3, true);
            qtest_writeb(qts, o2_storage_aic_slot(first, 0x0c), 0x08);
            o2_aic_assert_selection(qts, first, 0x10, 0, 4, 0);
            qtest_writeb(qts, o2_storage_aic_slot(first, 0x0b), 0x10);
            o2_aic_assert_selection(qts, first, 0, 0, 4, 0);
            qtest_writeb(qts, o2_storage_aic_slot(first, 0x92), 4);
            o2_aic_assert_selection(qts, first, 0, 0, 0, 0);
            o2_aic_assert_irqs(qts, remaining, remaining, true);
            /* Reverse source-clear order on the independent controller. */
            qtest_writeb(qts, o2_storage_aic_slot(second, 0x0b), 0x10);
            qtest_writeb(qts, o2_storage_aic_slot(second, 0x0c), 0x08);
            qtest_writeb(qts, o2_storage_aic_slot(second, 0x92), 4);
            o2_aic_assert_selection(qts, second, 0, 0x80, 4, 0);
            o2_aic_assert_irqs(qts, remaining, remaining, true);
            qtest_writeb(qts, o2_storage_aic_slot(second, 0x0c), 0x80);
            o2_aic_assert_selection(qts, second, 0, 0, 4, 0);
            qtest_writeb(qts, o2_storage_aic_slot(second, 0x92), 4);
            o2_aic_assert_selection(qts, second, 0, 0, 0, 0);
            o2_aic_assert_irqs(qts, 0, 0, false);
            unsigned raises = qtest_get_irq_raise_count(qts, 1);
            qtest_clock_step(qts, interval + 400);
            o2_aic_assert_selection(qts, 1, 0, 0, 0, 0);
            o2_aic_assert_selection(qts, 2, 0, 0, 0, 0);
            o2_aic_assert_irqs(qts, 0, 0, false);
            g_assert_cmpuint(qtest_get_irq_raise_count(qts, 1), ==, raises);
            qtest_memread(qts, RAM_FULL + 0x100, after, sizeof(after));
            g_assert_cmpmem(after, sizeof(after), guarded, sizeof(guarded));
            qtest_quit(qts);
        }
    }
}

static void o2_storage_assert_diagnostic_stop(QTestState *qts, unsigned slot)
{
    unsigned failed_pc = o2_storage_seq_pc(qts, slot);

    o2_storage_step_once(qts, slot);
    g_assert_cmphex(o2_storage_seq_pc(qts, slot), ==, failed_pc);
    g_assert_cmphex(qtest_readb(qts, o2_storage_aic_slot(slot, 0x87)) & 4,
                    ==, 4);
    g_assert_cmphex(qtest_readb(qts, o2_storage_aic_slot(slot, 0x92)), ==, 0);
}

static void o2_storage_assert_dma_event(const char *path, unsigned count,
                                        unsigned slot, uint32_t address,
                                        unsigned length, bool to_memory)
{
    g_autofree char *events = NULL;
    g_autofree char *expected = g_strdup_printf(
        "\"requester\":\"00:%02u.0\",\"pci_address\":\"0x%08x\","
        "\"direction\":\"%s\",\"length\":%u,\"result\":0",
        slot, address, to_memory ? "to-memory" : "from-memory", length);
    const char *next;
    unsigned found = 0;

    g_assert_true(g_file_get_contents(path, &events, NULL, NULL));
    g_assert_nonnull(strstr(events, expected));
    next = events;
    while ((next = strstr(next, "\"type\":\"dma\"")) != NULL) {
        found++;
        next++;
    }
    g_assert_cmpuint(found, ==, count);
}

static unsigned o2_storage_assert_dma_span(const char *path, unsigned before,
                                            unsigned slot, uint32_t address,
                                            unsigned length, bool from_memory)
{
    g_autofree char *events = NULL;
    g_autofree char *requester = g_strdup_printf("00:%02u.0", slot);
    g_auto(GStrv) lines = NULL;
    unsigned seen = 0, moved = 0;

    g_assert_true(g_file_get_contents(path, &events, NULL, NULL));
    lines = g_strsplit(events, "\n", -1);
    for (unsigned i = 0; lines[i] && *lines[i]; i++) {
        g_autoptr(QDict) record = qobject_to(QDict,
            qobject_from_json(lines[i], NULL));
        uint64_t actual;
        unsigned count;

        g_assert_nonnull(record);
        if (strcmp(qdict_get_str(record, "type"), "dma")) {
            continue;
        }
        seen++;
        if (seen <= before) {
            continue;
        }
        g_assert_cmpstr(qdict_get_str(record, "requester"), ==, requester);
        g_assert_cmpstr(qdict_get_str(record, "direction"), ==,
                        from_memory ? "from-memory" : "to-memory");
        g_assert_cmpuint(qdict_get_uint(record, "result"), ==, 0);
        g_assert_cmpint(qemu_strtou64(qdict_get_str(record, "pci_address"),
                                      NULL, 0, &actual), ==, 0);
        g_assert_cmpuint(actual, ==, (uint64_t)address + moved);
        count = qdict_get_uint(record, "length");
        g_assert_cmpuint(count, >, 0);
        g_assert_cmpuint(count, <=, length - moved);
        moved += count;
    }
    g_assert_cmpuint(moved, ==, length);
    return seen;
}

static void o2_storage_host_setup(QTestState *qts, uint32_t address,
                                   unsigned count, bool from_memory)
{
    qtest_writeb(qts, o2_storage_aic(0x93), 0);
    for (unsigned byte = 0; byte < 4; byte++) {
        qtest_writeb(qts, o2_storage_aic(0x88 + byte), address >> (8 * byte));
    }
    for (unsigned byte = 0; byte < 3; byte++) {
        qtest_writeb(qts, o2_storage_aic(0x8c + byte), count >> (8 * byte));
    }
    qtest_writeb(qts, o2_storage_aic(0x93), from_memory ? 5 : 1);
}

static uint32_t o2_storage_host_value(QTestState *qts, unsigned reg,
                                      unsigned bytes)
{
    uint32_t value = 0;

    for (unsigned byte = 0; byte < bytes; byte++) {
        value |= (uint32_t)qtest_readb(qts, o2_storage_aic(reg + byte)) <<
                 (8 * byte);
    }
    return value;
}

static void test_storage_host_offset_four(void)
{
    static const uint32_t views[] = { 0x40000000, 0 };
    static const unsigned counts[] = { 4, 8, 12, 13, 28, 255, 256, 257,
                                       512, 65537 };
    O2StorageTest t = o2_storage_start(false);
    g_autofree char *events = NULL;
    g_autofree char *expected = NULL;
    unsigned attempts = 0;

    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    for (unsigned view = 0; view < ARRAY_SIZE(views); view++) {
        for (unsigned c = 0; c < ARRAY_SIZE(counts); c++) {
            unsigned count = counts[c];
            uint32_t logical = 0x10028000;
            uint32_t address = views[view] + logical + 12;
            g_autofree uint8_t *guarded = g_malloc(count + 24);
            g_autofree uint8_t *got = g_malloc(count + 24);
            uint8_t hole;
            unsigned consumed = 0;

            if (!view && count > 512) {
                continue;
            }
            for (unsigned i = 0; i < count + 24; i++) {
                guarded[i] = (17 + i * 29 + c * 43 + view * 7) ^ (i >> 8);
            }
            qtest_memwrite(t.qts, RAM_FULL + logical, guarded, count + 24);
            if (!views[view]) {
                g_assert_cmpint(memtx_readb(t.qts, address, &hole), !=,
                                MEMTX_OK);
            }
            o2_storage_host_setup(t.qts, address, count, true);
            while (consumed < count) {
                unsigned valid = MIN(count - consumed, 256);
                unsigned accepted = consumed + valid;

                qtest_writeb(t.qts, o2_storage_aic(0x93), 0x0c);
                g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x92)),
                                ==, 0);
                g_assert_cmphex(o2_storage_host_value(t.qts, 0x88, 4), ==,
                                address + accepted);
                g_assert_cmphex(o2_storage_host_value(t.qts, 0x8c, 3), ==,
                                count - accepted);
                attempts = o2_storage_assert_dma_span(t.events, attempts, 1,
                                                       address + consumed,
                                                       valid, true);
                qtest_writeb(t.qts, o2_storage_aic(0x93), 4);
                g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x93)) & 0x38,
                                ==, 0);
                for (unsigned i = 0; i < valid; i++) {
                    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x99)),
                                    ==, guarded[12 + consumed + i]);
                }
                g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x94)) & 0x23,
                                ==, 0x21);
                consumed += valid;
            }
            g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x94)) & 8,
                            ==, 8);
            g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)), ==, 0);
            g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 0);
            g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x0b)) & 4,
                            ==, 0);
            qtest_memread(t.qts, RAM_FULL + logical, got, count + 24);
            g_assert_cmpmem(got, count + 24, guarded, count + 24);
        }
    }
    events = o2_storage_stop(&t);
    expected = g_strdup_printf("\"attempts\":%u,\"records\":%u,",
                                attempts, attempts);
    g_assert_nonnull(strstr(events, expected));
}

static void test_storage_host_offset_four_write(void)
{
    O2StorageTest t = o2_storage_start(false);
    uint8_t guarded[44], got[44], pattern[28];
    g_autofree char *events = NULL;

    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    memset(guarded, 0xa5, sizeof(guarded));
    for (unsigned i = 0; i < sizeof(pattern); i++) {
        pattern[i] = i * 23 + 7;
    }
    qtest_memwrite(t.qts, RAM_FULL + 0x10028000, guarded, sizeof(guarded));
    qtest_writeb(t.qts, o2_storage_aic(0x86), 0xc0);
    o2_storage_host_setup(t.qts, 0x10028004, sizeof(pattern), false);
    for (unsigned i = 0; i < sizeof(pattern); i++) {
        qtest_writeb(t.qts, o2_storage_aic(0x99), pattern[i]);
    }
    qtest_writeb(t.qts, o2_storage_aic(0x93), 8);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x92)), ==, 0);
    g_assert_cmphex(o2_storage_host_value(t.qts, 0x8c, 3), ==, 28);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x94)) & 8, ==, 0);
    qtest_writeb(t.qts, o2_storage_aic(0x93), 0x0a);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x92)), ==, 0);
    g_assert_cmphex(o2_storage_host_value(t.qts, 0x88, 4), ==, 0x10028020);
    g_assert_cmphex(o2_storage_host_value(t.qts, 0x8c, 3), ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x94)) & 9, ==, 9);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 0);
    qtest_memread(t.qts, RAM_FULL + 0x10028000, got, sizeof(got));
    g_assert_cmpmem(got, 4, guarded, 4);
    g_assert_cmpmem(got + 4, sizeof(pattern), pattern, sizeof(pattern));
    g_assert_cmpmem(got + 32, 12, guarded + 32, 12);
    o2_storage_assert_dma_span(t.events, 0, 1, 0x10028004, 28, false);
    events = o2_storage_stop(&t);
}

static void test_storage_host_offset_four_refusals(void)
{
    static const struct {
        uint32_t address;
        unsigned count;
        bool reverse;
        bool fault;
    } cases[] = {
        { 0x81028004, 4, false, true },
    };

    for (unsigned c = 0; c < ARRAY_SIZE(cases); c++) {
        O2StorageTest t = o2_storage_start(false);
        g_autofree char *events = NULL;
        uint8_t guarded[32], got[32];

        o2_storage_configure_aic(t.qts, 1);
        o2_program_banks(t.qts);
        for (unsigned i = 0; i < sizeof(guarded); i++) {
            guarded[i] = 0x39 + 13 * i;
        }
        qtest_memwrite(t.qts, RAM_FULL + 0x10028000, guarded, sizeof(guarded));
        o2_storage_host_setup(t.qts, cases[c].address, cases[c].count,
                              !cases[c].reverse);
        if (cases[c].reverse) {
            for (unsigned i = 0; i < cases[c].count; i++) {
                qtest_writeb(t.qts, o2_storage_aic(0x99), 0x71 + 11 * i);
            }
        }
        if (cases[c].reverse) {
            qtest_writeb(t.qts, o2_storage_aic(0x93), 0x0a);
        } else {
            qtest_writeb(t.qts, o2_storage_aic(0x93), 0x0c);
        }
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x92)), ==, 0);
        g_assert_cmphex(o2_storage_host_value(t.qts, 0x88, 4), ==,
                        cases[c].address);
        g_assert_cmphex(o2_storage_host_value(t.qts, 0x8c, 3), ==,
                        cases[c].count);
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x94)) & 8, ==, 0);
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 0);
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 0);
        qtest_writeb(t.qts, o2_storage_aic(0x93), 0);
        if (cases[c].reverse) {
            for (unsigned i = 0; i < cases[c].count; i++) {
                g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x99)), ==,
                                (uint8_t)(0x71 + 11 * i));
            }
        }
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x94)) & 1, ==, 1);
        qtest_memread(t.qts, RAM_FULL + 0x10028000, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), guarded, sizeof(guarded));
        events = o2_storage_stop(&t);
        if (cases[c].fault) {
            g_assert_nonnull(strstr(events,
                "\"requester\":\"00:01.0\",\"pci_address\":\"0x81028004\","
                "\"direction\":\"from-memory\",\"length\":4,\"result\":"));
            g_assert_null(strstr(events, "\"result\":0"));
            g_assert_nonnull(strstr(events, "\"attempts\":1,\"records\":1,"));
        } else {
            g_assert_null(strstr(events, "\"type\":\"dma\""));
        }
    }
}

static void test_storage_host_offset_four_disabled(void)
{
    O2StorageTest t = o2_storage_start(false);
    g_autofree char *during = NULL;
    g_autofree char *events = NULL;

    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    o2_aic_config_write(t.qts, 1, 4, 2);
    o2_storage_host_setup(t.qts, 0x10028004, 4, true);
    qtest_writeb(t.qts, o2_storage_aic(0x93), 0x0c);
    g_assert_cmphex(o2_storage_host_value(t.qts, 0x8c, 3), ==, 4);
    g_assert_true(g_file_get_contents(t.events, &during, NULL, NULL));
    g_assert_cmpstr(during, ==, "");
    g_clear_pointer(&during, g_free);
    o2_aic_config_write(t.qts, 1, 4, 4);
    g_assert_true(g_file_get_contents(t.events, &during, NULL, NULL));
    g_assert_cmpstr(during, ==, "");
    o2_aic_config_write(t.qts, 1, 4, 2);
    g_assert_cmphex(o2_storage_host_value(t.qts, 0x88, 4), ==, 0x10028004);
    g_assert_cmphex(o2_storage_host_value(t.qts, 0x8c, 3), ==, 4);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x94)) & 9, ==, 1);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)), ==, 0);
    events = o2_storage_stop(&t);
    g_assert_null(strstr(events, "\"type\":\"dma\""));
}

static void o2_storage_wait_command(QTestState *qts, unsigned slot,
                                     unsigned dma_wait_pc)
{
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    bool master = o2_aic_config_read(qts, slot, 4) & 4;

    qtest_clock_timers(qts, true);
    for (;;) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        qtest_clock_step(qts, 1000);
        if ((qtest_readb(qts, o2_storage_aic_slot(slot, 0x87)) & 4) ||
            (qtest_readb(qts, o2_storage_aic_slot(slot, 0x91)) & 2) ||
            /* Disabled PCI mastering leaves the program polling HDONE. */
            (!master && o2_storage_seq_pc(qts, slot) == dma_wait_pc)) {
            return;
        }
    }
}

static void o2_storage_command_impl(QTestState *qts, unsigned slot,
                                    const uint8_t *cdb,
                                    size_t cdb_len, uint32_t address,
                                    uint32_t count, bool write,
                                    bool pause_before_completion)
{
    uint8_t program[160] = { 0 };
    size_t n = 0;
    unsigned wait_at;
    uint64_t aic_base = AIC_PCI_VIEW + (slot - 1) * 0x1000;

    g_assert_cmpuint(cdb_len, <=, 16);
    qtest_writeb(qts, o2_storage_pio(aic_base + 0x87), 0x04);
    for (unsigned byte = 0; byte < 4; byte++) {
        qtest_writeb(qts, o2_storage_pio(aic_base + 0x88 + byte),
                     address >> (byte * 8));
    }
    for (unsigned byte = 0; byte < 3; byte++) {
        qtest_writeb(qts, o2_storage_pio(aic_base + 0x8c + byte),
                     count >> (byte * 8));
        qtest_writeb(qts, o2_storage_pio(aic_base + 0x08 + byte),
                     count >> (byte * 8));
    }
    o2_storage_emit_value(program, &n, 0x08, 0x01);
    o2_storage_emit_value(program, &n, 0x07, 0x05);
    o2_storage_emit_value(program, &n, 0, 0x93);
    o2_storage_emit_value(program, &n, 0, 0x00);
    o2_storage_emit_value(program, &n, 0x80, 0x03);
    o2_storage_emit_value(program, &n, 0x40, 0x00);
    for (unsigned i = 0; i < cdb_len; i++) {
        o2_storage_emit_value(program, &n, cdb[i], 0x06);
    }
    o2_storage_emit_value(program, &n, write ? 0 : 0x40, 0x03);
    o2_storage_emit_value(program, &n, write ? 0x3c : 0x38, 0x93);
    wait_at = n / 4;
    /* STATUS may precede data or arrive after host DMA completes. */
    o2_storage_emit(program, &n,
                    o2_storage_branch_word(0xd, 2, 0x0b, wait_at + 5));
    o2_storage_emit(program, &n,
                    o2_storage_branch_word(0xf, 4, 0x0b, wait_at));
    o2_storage_emit(program, &n, o2_storage_alu_word(0, 2, 0x93, 0x93));
    o2_storage_emit(program, &n,
                    o2_storage_branch_word(0xf, 8, 0x94, wait_at + 3));
    o2_storage_emit(program, &n,
                    o2_storage_branch_word(0xf, 2, 0x0b, wait_at + 4));
    o2_storage_emit_value(program, &n, 0xc0, 0x03);
    o2_storage_emit(program, &n, o2_storage_alu_word(1, 0xff, 0x06, 0x22));
    o2_storage_emit_value(program, &n, 0xe0, 0x03);
    o2_storage_emit(program, &n, o2_storage_alu_word(1, 0xff, 0x06, 0x23));
    o2_storage_emit(program, &n,
                    o2_storage_branch_word(0xf, 8, 0x0c, wait_at + 9));
    if (pause_before_completion) {
        o2_storage_emit_value(program, &n, 1, 0x91);
    }
    o2_storage_emit_value(program, &n, 1, 0x9d);
    o2_storage_emit_value(program, &n, 2, 0x91);
    o2_storage_emit(program, &n,
                    o2_storage_branch_word(0x8, 0, 0x65, n / 4));
    qtest_writeb(qts, o2_storage_pio(aic_base + 0x60), 0x91);
    qtest_writeb(qts, o2_storage_pio(aic_base + 0x62), 0);
    qtest_writeb(qts, o2_storage_pio(aic_base + 0x63), 0);
    for (unsigned i = 0; i < n; i++) {
        qtest_writeb(qts, o2_storage_pio(aic_base + 0x61),
                     program[i]);
    }
    qtest_writeb(qts, o2_storage_pio(aic_base + 0x60), 0x90);
    qtest_writeb(qts, o2_storage_pio(aic_base + 0x62), 0);
    qtest_writeb(qts, o2_storage_pio(aic_base + 0x63), 0);
    qtest_writeb(qts, o2_storage_pio(aic_base + 0x87), 0x02);
    o2_storage_wait_command(qts, slot, wait_at + 3);
}

static void o2_storage_command(QTestState *qts, const uint8_t *cdb,
                               size_t cdb_len, uint32_t address,
                               uint32_t count, bool write)
{
    o2_storage_command_impl(qts, 1, cdb, cdb_len, address, count, write,
                            false);
}

static void o2_storage_wait_completion(QTestState *qts, unsigned slot)
{
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

    while (!(qtest_readb(qts, o2_storage_aic_slot(slot, 0x91)) & 2) &&
           !qtest_readb(qts, o2_storage_aic_slot(slot, 0x92)) &&
           !(qtest_readb(qts, o2_storage_aic_slot(slot, 0x87)) & 4)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        qtest_clock_step(qts, 1000);
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readb(qts, o2_storage_aic_slot(slot, 0x92)), ==, 0);
    g_assert_cmphex(qtest_readb(qts, o2_storage_aic_slot(slot, 0x91)) & 2,
                    ==, 2);
}

static void o2_storage_inquiry_len(QTestState *qts, uint32_t address,
                                   uint8_t length)
{
    const uint8_t cdb[] = { 0x12, 0, 0, 0, length, 0 };

    o2_storage_command(qts, cdb, sizeof(cdb), address, length, false);
}

static void o2_storage_inquiry(QTestState *qts, uint32_t address)
{
    o2_storage_inquiry_len(qts, address, 36);
}

static void o2_storage_inquiry_paused(QTestState *qts, uint32_t address)
{
    const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };

    o2_storage_command_impl(qts, 1, cdb, sizeof(cdb), address, 36, false,
                            true);
}

static void o2_storage_ack_completion_slot(QTestState *qts, unsigned slot)
{
    g_assert_cmphex(qtest_readb(qts, o2_storage_aic_slot(slot, 0x9e)), ==, 1);
    g_assert_cmphex(qtest_readb(qts, o2_storage_aic_slot(slot, 0x9d)), ==, 1);
    qtest_writeb(qts, o2_storage_aic_slot(slot, 0x92), 2);
}

static void o2_storage_ack_completion(QTestState *qts)
{
    o2_storage_ack_completion_slot(qts, 1);
}

static void test_storage_diagnostic_pci_parity_readback(void)
{
    static const unsigned unsupported[] = { 0x10, 0x100, 0x400 };
    O2StorageTest t = o2_storage_start(false);
    g_autofree char *events = NULL;
    uint8_t byte;

    for (unsigned slot = 1; slot <= 2; slot++) {
        uint64_t window = AIC_PCI_VIEW + (slot - 1) * 0x1000;

        o2_aic_config_write(t.qts, slot, 4, 0);
        o2_aic_config_write(t.qts, slot, 0x10, UINT32_MAX);
        g_assert_cmphex(o2_aic_config_read(t.qts, slot, 0x10), ==,
                        0xffffff01u);
        o2_aic_config_write(t.qts, slot, 0x14, UINT32_MAX);
        g_assert_cmphex(o2_aic_config_read(t.qts, slot, 0x14), ==,
                        0xfffff000u);
        o2_aic_config_write(t.qts, slot, 0x10,
                            0x2001 + (slot - 1) * 0x100);
        o2_aic_config_write(t.qts, slot, 0x14,
                            0x80000000 + (slot - 1) * 0x1000);
        g_assert_cmpint(memtx_readb(t.qts,
            o2_storage_pio(window + 0x20), &byte),
                        !=, MEMTX_OK);
        o2_aic_config_write(t.qts, slot, 4, 0x46);
        g_assert_cmphex(o2_aic_config_readw(t.qts, slot, 4), ==, 0x46);
        qtest_writeb(t.qts, o2_storage_pio(window + 0x120), 0x50 + slot);
        g_assert_cmphex(qtest_readb(t.qts,
            o2_storage_pio(window + 0x20)), ==, 0x50 + slot);
        g_assert_cmphex(qtest_readb(t.qts,
            o2_storage_pio(window + 0xf20)), ==, 0x50 + slot);
        o2_aic_config_write(t.qts, slot, 4, 0x44);
        g_assert_cmpint(memtx_readb(t.qts,
            o2_storage_pio(window + 0x20), &byte),
                        !=, MEMTX_OK);
        o2_aic_config_write(t.qts, slot, 4, 0x40);
        g_assert_cmphex(o2_aic_config_readw(t.qts, slot, 4), ==, 0x40);
        o2_aic_config_write(t.qts, slot, 4, 0);
        g_assert_cmphex(o2_aic_config_readw(t.qts, slot, 4), ==, 0);
        for (unsigned i = 0; i < ARRAY_SIZE(unsupported); i++) {
            qtest_writel(t.qts, MACE_CFGADDR, slot << 11 | 4);
            g_assert_cmpint(memtx_writel(t.qts, MACE_CFGDATA,
                                         unsupported[i]), !=, MEMTX_OK);
            g_assert_cmphex(o2_aic_config_readw(t.qts, slot, 4), ==, 0);
        }
    }
    events = o2_storage_stop(&t);
    g_assert_cmpstr(events, ==,
                    "{\"type\":\"complete\",\"attempts\":0,"
                    "\"records\":0,\"complete\":true}\n");
}

static void test_storage_diagnostic_apertures(void)
{
    O2StorageTest t = o2_storage_start(false);
    g_autofree char *events = NULL;
    g_autofree char *save_reply = NULL;
    g_autoptr(QDict) hotplug = NULL;
    g_autoptr(QDict) migrate = NULL;
    uint8_t byte;

    for (unsigned slot = 1; slot <= 2; slot++) {
        g_assert_cmphex(o2_aic_config_read(t.qts, slot, 0), ==, 0x80789004);
        o2_aic_config_write(t.qts, slot, 0x14, UINT32_MAX);
        g_assert_cmphex(o2_aic_config_read(t.qts, slot, 0x14), ==,
                        0xfffff000);
        o2_storage_configure_aic(t.qts, slot);
    }
    qtest_writel(t.qts, MACE_CFGADDR, 3U << 11);
    g_assert_cmphex(qtest_readl(t.qts, MACE_CFGDATA), ==, UINT32_MAX);
    g_assert_cmpint(memtx_readb(t.qts, AIC_PCI_VIEW + 0x1000000, &byte),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_readb(t.qts, AIC_PCI_VIEW + 0x1ffffff, &byte),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_readb(t.qts, O2_PCI_HIGH_VIEW - 1, &byte),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_readb(t.qts, O2_PCI_HIGH_VIEW + 2 * GiB, &byte),
                    !=, MEMTX_OK);
    qtest_writeb(t.qts, o2_storage_pio(AIC_PCI_VIEW + 0x20), 0x5a);
    qtest_writeb(t.qts, o2_storage_pio(AIC_PCI_VIEW + 0x1020), 0xa5);
    g_assert_cmphex(qtest_readb(t.qts,
        o2_storage_pio(AIC_PCI_VIEW + 0xf20)), ==, 0x5a);
    g_assert_cmphex(qtest_readb(t.qts,
        o2_storage_pio(O2_PCI_HIGH_VIEW + 0x20)), ==, 0x5a);
    g_assert_cmphex(qtest_readb(t.qts,
        o2_storage_pio(O2_PCI_HIGH_VIEW + 0xf20)), ==, 0x5a);
    g_assert_cmphex(qtest_readb(t.qts,
        o2_storage_pio(O2_PCI_HIGH_VIEW + 0x1020)), ==, 0xa5);
    qtest_writel(t.qts, MACE_PCI + 8, 3);
    qtest_writeq(t.qts, CRIME + 0x18, 3ULL << 8);
    qtest_irq_intercept_out_named(t.qts, "/machine/crime", "cpu-ip");
    qtest_writeb(t.qts, o2_storage_pio(O2_PCI_HIGH_VIEW + 0x87), 0x16);
    o2_aic_assert_irqs(t.qts, 1, 1, true);
    qtest_writeb(t.qts, o2_storage_pio(O2_PCI_HIGH_VIEW + 0x1087), 0x16);
    o2_aic_assert_irqs(t.qts, 3, 3, true);
    hotplug = qtest_qmp(t.qts,
        "{'execute':'device_add','arguments':{'driver':'scsi-hd',"
        "'bus':'aic0.0','scsi-id':0,'lun':0,'id':'extra'}}");
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(hotplug, "error"),
                                         "desc"),
                            "storage diagnostic rejects PCI and SCSI"));
    migrate = qtest_qmp(t.qts,
        "{'execute':'migrate','arguments':{'uri':'unix:/nonexistent'}}");
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(migrate, "error"),
                                         "desc"),
                            "o2-test/eight-bank-profile"));
    save_reply = qtest_hmp(t.qts, "savevm o2-storage");
    g_assert_nonnull(strstr(save_reply, "o2-test/eight-bank-profile"));
    events = o2_storage_stop(&t);
    g_assert_cmpstr(events, ==,
                    "{\"type\":\"complete\",\"attempts\":0,"
                    "\"records\":0,\"complete\":true}\n");
}

static void test_storage_diagnostic_tcg_inventory(void)
{
    O2StorageTest t;
    g_autofree char *events = NULL;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    t = o2_storage_start_accel(false, "tcg,thread=single", 0, false, NULL);
    for (unsigned slot = 1; slot <= 2; slot++) {
        g_assert_cmphex(o2_aic_config_read(t.qts, slot, 0), ==, 0x80789004);
    }
    qtest_writel(t.qts, MACE_CFGADDR, 3U << 11);
    g_assert_cmphex(qtest_readl(t.qts, MACE_CFGDATA), ==, UINT32_MAX);
    g_assert_cmphex(qtest_readl(t.qts, MACE_PCI + 4) & (1U << 31),
                    ==, 1U << 31);
    events = o2_storage_stop(&t);
    g_assert_nonnull(strstr(events, "\"complete\":true"));
}

static void test_storage_host_dfdat_pci_route(void)
{
    /* Synthetic PCI DMA reaches RAM; the numeric CPU address is a hole. */
    const uint32_t pci_addr = 0x10000108;
    const uint64_t ram_addr = RAM_FULL + pci_addr;
    uint8_t guarded[16], got[16], hole;
    O2StorageTest t = o2_storage_start(false);
    g_autofree char *events = NULL;

    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    memset(guarded, 0xa5, sizeof(guarded));
    for (unsigned i = 0; i < 7; i++) {
        guarded[i + 8] = i * 23 + 3;
    }
    qtest_memwrite(t.qts, ram_addr - 8, guarded, sizeof(guarded));
    g_assert_cmpint(memtx_readb(t.qts, pci_addr, &hole), !=, MEMTX_OK);

    for (unsigned i = 0; i < 4; i++) {
        qtest_writeb(t.qts, o2_storage_aic(0x88 + i),
                     pci_addr >> (8 * i));
    }
    for (unsigned i = 0; i < 3; i++) {
        qtest_writeb(t.qts, o2_storage_aic(0x8c + i), 7 >> (8 * i));
    }
    qtest_writeb(t.qts, o2_storage_aic(0x93), 0x05);
    qtest_writeb(t.qts, o2_storage_aic(0x93), 0x0c);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x94)) & 0x29,
                    ==, 0x28);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x88)), ==, 0x0f);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x8c)), ==, 0);
    qtest_writeb(t.qts, o2_storage_aic(0x93), 0x04);
    for (unsigned i = 0; i < 7; i++) {
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x99)), ==,
                        guarded[i + 8]);
    }
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x94)) & 1, ==, 1);
    qtest_memread(t.qts, ram_addr - 8, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), guarded, sizeof(guarded));
    events = o2_storage_stop(&t);
    g_assert_nonnull(strstr(events,
        "\"pci_address\":\"0x10000108\",\"direction\":\"from-memory\","
        "\"length\":7,\"result\":0,"
        "\"policy\":\"diagnostic-byte-identity\""));
    g_assert_nonnull(strstr(events, "\"attempts\":1,\"records\":1,"));
}

static char *o2_storage_low_pci_read_with_eth_mode(bool low_eth_dma)
{
    uint8_t read_area[24], read_after[24];
    O2StorageTest t = o2_storage_start_accel_eth(false, "qtest",
                                                 low_eth_dma);
    char *events;

    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    memset(read_area, 0xa5, sizeof(read_area));
    for (unsigned i = 0; i < 7; i++) {
        read_area[8 + i] = i * 23 + 3;
    }
    qtest_memwrite(t.qts, RAM_FULL + 0x100, read_area,
                   sizeof(read_area));
    for (unsigned i = 0; i < 4; i++) {
        qtest_writeb(t.qts, o2_storage_aic(0x88 + i),
                     0x108 >> (8 * i));
    }
    for (unsigned i = 0; i < 3; i++) {
        qtest_writeb(t.qts, o2_storage_aic(0x8c + i),
                     7 >> (8 * i));
    }
    qtest_writeb(t.qts, o2_storage_aic(0x93), 0x05);
    qtest_writeb(t.qts, o2_storage_aic(0x93), 0x0c);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x94)) & 8, ==, 8);
    qtest_writeb(t.qts, o2_storage_aic(0x93), 0x04);
    for (unsigned i = 0; i < 7; i++) {
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x99)), ==,
                        read_area[8 + i]);
    }
    qtest_memread(t.qts, RAM_FULL + 0x100, read_after,
                  sizeof(read_after));
    g_assert_cmpmem(read_after, sizeof(read_after), read_area,
                    sizeof(read_area));
    events = o2_storage_stop(&t);
    g_assert_nonnull(strstr(events,
        "\"requester\":\"00:01.0\",\"pci_address\":\"0x00000108\","
        "\"direction\":\"from-memory\",\"length\":7,\"result\":0,"
        "\"policy\":\"diagnostic-byte-identity\""));
    g_assert_nonnull(strstr(events, "\"attempts\":1,\"records\":1,"));
    return events;
}

static char *o2_storage_low_pci_write_with_eth_mode(bool low_eth_dma)
{
    O2StorageTest t = o2_storage_start_accel_eth(true, "qtest",
                                                 low_eth_dma);
    uint8_t write_after[64];
    char *events;

    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    qtest_memset(t.qts, RAM_FULL + 0x1f8, 0xa5, sizeof(write_after));
    o2_storage_inquiry(t.qts, 0x200);
    qtest_memread(t.qts, RAM_FULL + 0x1f8, write_after,
                  sizeof(write_after));
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(write_after[i], ==, 0xa5);
    }
    g_assert_cmpmem(write_after + 16, 4, "QEMU", 4);
    for (unsigned i = 8 + 36; i < sizeof(write_after); i++) {
        g_assert_cmphex(write_after[i], ==, 0xa5);
    }
    o2_storage_ack_completion(t.qts);
    events = o2_storage_stop(&t);
    g_assert_nonnull(strstr(events,
        "\"requester\":\"00:01.0\",\"pci_address\":\"0x00000200\","
        "\"direction\":\"to-memory\",\"length\":36,\"result\":0,"
        "\"policy\":\"diagnostic-byte-identity\""));
    g_assert_nonnull(strstr(events, "\"attempts\":1,\"records\":1,"));
    return events;
}

static void test_low_ethernet_dma_preserves_aic_pci(void)
{
    g_autofree char *read_off =
        o2_storage_low_pci_read_with_eth_mode(false);
    g_autofree char *read_on =
        o2_storage_low_pci_read_with_eth_mode(true);
    g_autofree char *write_off =
        o2_storage_low_pci_write_with_eth_mode(false);
    g_autofree char *write_on =
        o2_storage_low_pci_write_with_eth_mode(true);

    /* The separate 64 MiB AIC policy-off profile cannot use banked O2 RAM. */
    g_assert_cmpstr(read_off, ==, read_on);
    g_assert_cmpstr(write_off, ==, write_on);
}

static void check_default_o2_aic(const char *memory, bool full_profile)
{
    static const uint8_t rom[] = { 0, 0, 0, 0 };
    g_autofree char *dir = NULL;
    g_autofree char *rom_path = NULL;
    g_autofree char *machine = NULL;
    QTestState *qts;

    if (full_profile) {
        dir = g_dir_make_tmp("o2-default-aic-XXXXXX", NULL);
        g_assert_nonnull(dir);
        rom_path = g_build_filename(dir, "synthetic-rom", NULL);
        g_assert_true(g_file_set_contents(rom_path, (const char *)rom,
                                          sizeof(rom), NULL));
        machine = g_strdup_printf(
            "o2-test,bank-profile=8x128m-synthetic,"
            "reset-low-ram-approx=on,empty-renderer-status=on,"
            "prom-private-window=on,prom-sb64-reset=on,"
            "gbe11-registers=on,prom-id-word=on,"
            "password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0,rom-pad-byte=238",
            rom_path);
    } else {
        machine = g_strdup("o2-test");
    }
    qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s -cpu R10000 -m %s "
                      "-nodefaults -accel qtest -S -nic none",
                      machine, memory);
    g_assert_cmphex(o2_aic_config_read(qts, 1, 0), ==, 0x80789004);
    g_assert_cmphex(o2_aic_config_read(qts, 2, 0), ==, 0x80789004);
    g_assert_cmphex(o2_aic_config_read(qts, 3, 0), ==, UINT32_MAX);
    for (unsigned slot = 1; slot <= 2; slot++) {
        o2_aic_config_write(qts, slot, 4, 0);
        o2_aic_config_write(qts, slot, 0x14, UINT32_MAX);
        g_assert_cmphex(o2_aic_config_read(qts, slot, 0x14), ==,
                        0xfffff000);
        o2_storage_configure_aic(qts, slot);
        qtest_writeb(qts, o2_storage_aic_slot(slot, 0x27), 0x50 + slot);
        g_assert_cmphex(qtest_readb(qts,
                                    o2_storage_aic_slot(slot, 0x127)),
                        ==, 0x50 + slot);
        o2_aic_config_write(qts, slot, 4, 0x46);
        g_assert_cmphex(o2_aic_config_readw(qts, slot, 4), ==, 0x46);
    }
    qtest_irq_intercept_out_named(qts, "/machine/crime", "cpu-ip");
    qtest_writel(qts, MACE_PCI + 8, 3);
    qtest_writeq(qts, CRIME + 0x18, 3ULL << 8);
    qtest_writeb(qts, o2_storage_aic_slot(1, 0x87), 0x16);
    o2_aic_assert_irqs(qts, 1, 1, true);
    o2_aic_config_write(qts, 1, 4, 0);
    o2_aic_assert_irqs(qts, 0, 0, false);
    o2_aic_config_write(qts, 1, 4, 0x46);
    o2_aic_assert_irqs(qts, 1, 1, true);
    qtest_writeb(qts, o2_storage_aic_slot(2, 0x87), 0x16);
    o2_aic_assert_irqs(qts, 3, 3, true);
    qtest_writeb(qts, o2_storage_aic_slot(1, 0x87), 0x06);
    o2_aic_assert_irqs(qts, 2, 2, true);
    qtest_writeb(qts, o2_storage_aic_slot(2, 0x87), 0x06);
    o2_aic_assert_irqs(qts, 0, 0, false);
    qtest_quit(qts);
    if (full_profile) {
        g_assert_cmpint(g_unlink(rom_path), ==, 0);
        g_assert_cmpint(g_rmdir(dir), ==, 0);
    }
}

static void test_default_aic_64m(void)
{
    check_default_o2_aic("64M", false);
}

static void test_default_aic_128m(void)
{
    check_default_o2_aic("128M", false);
}

static void test_default_aic_full_1g(void)
{
    check_default_o2_aic("1G", true);
}

static void test_default_aic_low_dma_28_bytes(void)
{
    QTestState *qts = o2_start_banked();
    uint8_t ram[36], readback[28], after[36];

    o2_program_banks(qts);
    for (unsigned i = 0; i < 28; i++) {
        ram[i] = 0x30 + i;
    }
    memset(ram + 28, 0xa5, 8);
    qtest_memwrite(qts, RAM_FULL, ram, sizeof(ram));
    o2_storage_configure_aic(qts, 1);

    for (unsigned window = 0; window < 2; window++) {
        uint32_t addr = window ? 0 : 0x40000000;

        qtest_writeb(qts, o2_storage_aic(0x93), 0x04);
        for (unsigned i = 0; i < 4; i++) {
            qtest_writeb(qts, o2_storage_aic(0x88 + i), addr >> (i * 8));
        }
        qtest_writeb(qts, o2_storage_aic(0x8c), 28);
        qtest_writeb(qts, o2_storage_aic(0x8d), 0);
        qtest_writeb(qts, o2_storage_aic(0x8e), 0);
        qtest_writeb(qts, o2_storage_aic(0x93), 0x0d);
        g_assert_cmphex(qtest_readb(qts, o2_storage_aic(0x92)) & 0x40,
                        ==, 0);
        g_assert_cmphex(qtest_readb(qts, o2_storage_aic(0x94)) & 8, ==, 8);
        for (unsigned i = 0; i < sizeof(readback); i++) {
            readback[i] = qtest_readb(qts, o2_storage_aic(0x99));
        }
        for (unsigned i = 0; i < sizeof(readback); i++) {
            g_assert_cmphex(readback[i], ==, ram[window ? i : i ^ 3]);
        }
        qtest_memread(qts, RAM_FULL, after, sizeof(after));
        g_assert_cmpmem(after, sizeof(after), ram, sizeof(ram));
    }
    qtest_quit(qts);
}

static void test_default_aic_low_dma_write(void)
{
    QTestState *qts = o2_start_banked();
    uint8_t ram[48], after[48];

    o2_program_banks(qts);
    memset(ram, 0xa5, sizeof(ram));
    qtest_memwrite(qts, RAM_FULL + 0x100, ram, sizeof(ram));
    o2_storage_configure_aic(qts, 1);
    for (unsigned i = 0; i < 32; i++) {
        qtest_writeb(qts, o2_storage_aic(0x99), 0x50 + i);
        ram[8 + i] = 0x50 + i;
    }
    qtest_writeb(qts, o2_storage_aic(0x88), 0x08);
    qtest_writeb(qts, o2_storage_aic(0x89), 0x01);
    qtest_writeb(qts, o2_storage_aic(0x8a), 0);
    qtest_writeb(qts, o2_storage_aic(0x8b), 0);
    qtest_writeb(qts, o2_storage_aic(0x8c), 32);
    qtest_writeb(qts, o2_storage_aic(0x8d), 0);
    qtest_writeb(qts, o2_storage_aic(0x8e), 0);
    qtest_writeb(qts, o2_storage_aic(0x93), 0x0a);
    g_assert_cmphex(qtest_readb(qts, o2_storage_aic(0x92)) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, o2_storage_aic(0x94)) & 8, ==, 8);
    qtest_memread(qts, RAM_FULL + 0x100, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), ram, sizeof(ram));
    qtest_quit(qts);
}

static void test_full_identity_ethernet_aic(void)
{
    static const uint8_t rom[] = { 0, 0, 0, 0 };
    static const uint8_t tx_command[8] = {
        0, 0, 0, 0, 0x01, 0x44, 0, 0x3b
    };
    static const uint8_t tx_status[8] = {
        0x80, 0, 0, 0, 0, 0x80, 0, 0x3c
    };
    const uint64_t tx = 0x01100000;
    const uint64_t rx = 0x01200000;
    const struct timeval timeout = { .tv_sec = 10 };
    QTestState *qts;
    SgiWait wait;
    g_autofree char *dir = NULL;
    g_autofree char *rom_path = NULL;
    g_autofree char *event_path = NULL;
    g_autofree char *identity = NULL;
    uint8_t identity_bytes[128], tx_entry[128], tx_frame[60];
    uint8_t rx_frame[60], received[60], status[8];
    uint32_t wire_length;
    int sv[2];

    dir = g_dir_make_tmp("o2-full-network-XXXXXX", NULL);
    g_assert_nonnull(dir);
    rom_path = g_build_filename(dir, "synthetic-rom", NULL);
    event_path = g_build_filename(dir, "dma.jsonl", NULL);
    identity = g_build_filename(dir, "synthetic-identity", NULL);
    for (unsigned i = 0; i < sizeof(identity_bytes); i++) {
        identity_bytes[i] = (i * 37) ^ 0xa5;
    }
    g_assert_true(g_file_set_contents(rom_path, (const char *)rom,
                                      sizeof(rom), NULL));
    g_assert_true(g_file_set_contents(identity,
                                      (const char *)identity_bytes,
                                      sizeof(identity_bytes), NULL));
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    g_assert_cmpint(setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO,
                               &timeout, sizeof(timeout)), ==, 0);
    qts = qtest_initf(MEMTX_TEST_DEVICE
        "-M o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,prom-sb64-reset=on,"
        "gbe11-registers=on,prom-id-word=on,"
        "password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0,rom-pad-byte=238,"
        "identity-diagnostic=on,identity-test-serial=1032547698ba,"
        "identity-eprom-file=%s,identity-status=0123456789abcd,"
        "mace-misc-word-pio=on,mace-eth-low-dma-compat=on,"
        "phy-ics1890=on,phy-address=8 "
        "-cpu R10000 -m 1G -nodefaults -accel qtest "
        "-nic socket,id=n0,fd=%d,model=sgi-mace-eth,"
        "mac=02:a1:4c:73:95:e6",
        rom_path, identity, sv[1]);
    g_assert_true(qtest_qom_get_bool(qts, "/machine",
                                      "mace-eth-low-dma-compat"));
    g_assert_true(qtest_qom_get_bool(qts, "/machine",
                                      "identity-diagnostic"));
    g_assert_false(qtest_qom_get_bool(qts, "/machine",
                                       "diagnostic-lower-pci-byte-identity"));
    g_assert_cmphex(o2_aic_config_read(qts, 1, 0), ==, 0x80789004);
    g_assert_cmphex(o2_aic_config_read(qts, 2, 0), ==, 0x80789004);
    o2_storage_configure_aic(qts, 1);
    o2_storage_configure_aic(qts, 2);
    o2_program_banks(qts);
    qtest_irq_intercept_out_named(qts, "/machine/crime", "cpu-ip");
    qtest_writel(qts, MACE_PCI + 8, 3);
    qtest_writeq(qts, CRIME + 0x18, 8 | (3ULL << 8));

    qtest_writeq(qts, MACE_PERIPH + 8, 4);
    qtest_clock_step(qts, 500000);
    qtest_writeq(qts, MACE_PERIPH + 8, 0);
    qtest_clock_step(qts, 520000);
    qtest_writeq(qts, MACE_PERIPH + 8, 4);
    qtest_clock_step(qts, 30000);
    g_assert_cmphex(qtest_readq(qts, MACE_PERIPH + 8) & 8, ==, 0);
    qtest_clock_step(qts, 120000);
    g_assert_cmphex(qtest_readq(qts, MACE_PERIPH + 8) & 8, ==, 8);

    g_assert_true(qtest_qom_get_bool(qts, "/machine", "phy-ics1890"));
    {
        g_autoptr(QDict) phy = qtest_qmp(qts,
            "{'execute':'qom-get','arguments':{'path':'/machine/mace',"
            "'property':'phy-address'}}");

        g_assert_cmpuint(qdict_get_int(phy, "return"), ==, 8);
    }
    qtest_clock_step(qts, 100000000);
    memset(tx_entry, 0xa5, sizeof(tx_entry));
    memcpy(tx_entry, tx_command, sizeof(tx_command));
    for (unsigned i = 0; i < sizeof(tx_frame); i++) {
        tx_frame[i] = i * 7 + 3;
    }
    memcpy(tx_entry + 68, tx_frame, sizeof(tx_frame));
    qtest_memwrite(qts, RAM_FULL + tx, tx_entry, sizeof(tx_entry));
    qtest_writeq(qts, MACE_ETH + 0x00, 0);
    qtest_writeq(qts, MACE_ETH + 0xb8, tx);
    qtest_writeq(qts, MACE_ETH + 0x10, 3);
    qtest_writeq(qts, MACE_ETH + 0x30, 1);
    g_assert_cmpint(recv(sv[0], &wire_length, sizeof(wire_length),
                         MSG_WAITALL), ==, sizeof(wire_length));
    g_assert_cmpuint(ntohl(wire_length), ==, sizeof(tx_frame));
    g_assert_cmpint(recv(sv[0], received, sizeof(received),
                         MSG_WAITALL), ==, sizeof(received));
    g_assert_cmpmem(received, sizeof(received), tx_frame,
                    sizeof(tx_frame));
    qtest_memread(qts, RAM_FULL + tx, status, sizeof(status));
    g_assert_cmpmem(status, sizeof(status), tx_status, sizeof(tx_status));
    g_assert_true(qtest_readq(qts, CRIME + 0x10) & 8);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writeq(qts, MACE_ETH + 0x10, 0);
    qtest_writeq(qts, MACE_ETH + 0x08, 3);
    g_assert_false(qtest_readq(qts, CRIME + 0x10) & 8);

    for (unsigned i = 0; i < sizeof(rx_frame); i++) {
        rx_frame[i] = i * 11 + 9;
    }
    memcpy(rx_frame, (uint8_t[]) { 0x02, 0xa1, 0x4c, 0x73, 0x95, 0xe6 }, 6);
    qtest_memset(qts, RAM_FULL + rx, 0xa5, 4096);
    qtest_writeq(qts, MACE_ETH + 0xa0, UINT64_C(0x02a14c7395e6));
    qtest_writeq(qts, MACE_ETH + 0x100, rx);
    qtest_writeq(qts, MACE_ETH + 0x10,
                 (1U << 15) | (4U << 12) | (1U << 9) | (2U << 4));
    wire_length = htonl(sizeof(rx_frame));
    g_assert_cmpint(send(sv[0], &wire_length, sizeof(wire_length), 0), ==,
                    sizeof(wire_length));
    g_assert_cmpint(send(sv[0], rx_frame, sizeof(rx_frame), 0), ==,
                    sizeof(rx_frame));
    sgi_wait_start(&wait, "O2 combined Ethernet receive");
    while (qtest_readq(qts, MACE_ETH + 0x40) & 31) {
        sgi_wait_pause(&wait, 5000);
        qtest_clock_step(qts, 1000);
    }
    qtest_memread(qts, RAM_FULL + rx + 34, received, sizeof(received));
    g_assert_cmpmem(received, sizeof(received), rx_frame,
                    sizeof(rx_frame));
    g_assert_true(qtest_readq(qts, CRIME + 0x10) & 8);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_writeb(qts, o2_storage_aic_slot(1, 0x87), 0x16);
    g_assert_true(qtest_get_irq(qts, 0));
    o2_aic_assert_irqs(qts, 1, 1, true);
    qtest_writeb(qts, o2_storage_aic_slot(2, 0x87), 0x16);
    o2_aic_assert_irqs(qts, 3, 3, true);
    qtest_writeb(qts, o2_storage_aic_slot(1, 0x87), 0x06);
    o2_aic_assert_irqs(qts, 2, 2, true);
    qtest_writeq(qts, MACE_ETH + 0x10,
                 (1U << 15) | (4U << 12) | (1U << 9));
    qtest_writeq(qts, MACE_ETH + 0x08, 1U << 5);
    g_assert_false(qtest_readq(qts, CRIME + 0x10) & 8);
    g_assert_false(qtest_get_irq(qts, 0));
    o2_aic_assert_irqs(qts, 2, 2, true);
    qtest_writeb(qts, o2_storage_aic_slot(2, 0x87), 0x06);
    o2_aic_assert_irqs(qts, 0, 0, false);
    qtest_memread(qts, RAM_FULL + tx + 68,
                  received, sizeof(received));
    g_assert_cmpmem(received, sizeof(received), tx_frame,
                    sizeof(tx_frame));
    qtest_memread(qts, RAM_FULL + rx + 34,
                  received, sizeof(received));
    g_assert_cmpmem(received, sizeof(received), rx_frame,
                    sizeof(rx_frame));

    g_assert_false(g_file_test(event_path, G_FILE_TEST_EXISTS));
    qtest_quit(qts);
    g_assert_false(g_file_test(event_path, G_FILE_TEST_EXISTS));
    g_assert_cmpint(g_unlink(identity), ==, 0);
    g_assert_cmpint(g_unlink(rom_path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    close(sv[0]);
    close(sv[1]);
}

static void test_storage_diagnostic_dma_events(void)
{
    O2StorageTest t = o2_storage_start(true);
    g_autofree char *events = NULL;
    g_autofree char *during = NULL;
    uint8_t answer[40];
    uint8_t hole;

    g_assert_false(qtest_qom_get_bool(t.qts, "/machine/mace",
                                       "pci-native-word-swap"));
    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    g_assert_cmpint(memtx_readb(t.qts, 0x10000100, &hole), !=, MEMTX_OK);
    qtest_memset(t.qts, RAM_FULL + 0x100, 0xa5, sizeof(answer));
    qtest_writeb(t.qts, RAM_FULL + 2 * RAM_BANK_SIZE + 0x108, 0xe2);
    o2_storage_inquiry_paused(t.qts, 0x100);
    g_assert_true(g_file_get_contents(t.events, &during, NULL, NULL));
    g_assert_cmpstr(during, ==,
        "{\"type\":\"dma\",\"sequence\":1,\"requester\":\"00:01.0\","
        "\"pci_address\":\"0x00000100\",\"direction\":\"to-memory\","
        "\"length\":36,\"result\":0,"
        "\"policy\":\"diagnostic-byte-identity\"}\n");
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x94)) & 8, ==, 8);
    qtest_memread(t.qts, RAM_FULL + 0x100, answer, sizeof(answer));
    g_assert_cmpmem(answer + 8, 4, "QEMU", 4);
    for (unsigned i = 36; i < sizeof(answer); i++) {
        g_assert_cmphex(answer[i], ==, 0xa5);
    }
    g_assert_cmphex(qtest_readb(t.qts, RAM_NO_ECC + 0x100 + 8), ==, 'Q');
    qtest_writeb(t.qts, o2_storage_pio(AIC_PCI_VIEW + 0x92), 1);
    qtest_writeb(t.qts, o2_storage_pio(AIC_PCI_VIEW + 0x87), 2);
    o2_storage_wait_completion(t.qts, 1);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 2);
    o2_storage_ack_completion(t.qts);
    o2_storage_inquiry(t.qts, 0x40000180);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(t.qts, RAM_FULL + 0x180 + 8), ==, 'Q');
    o2_storage_ack_completion(t.qts);
    qtest_writeq(t.qts, CRIME + 0x208 + 2 * 8, 0x100);
    qtest_writeq(t.qts, CRIME + 0x208, 0x108);
    g_assert_cmphex(qtest_readb(t.qts, RAM_FULL + 0x108), ==, 0xe2);
    o2_storage_inquiry(t.qts, 0x100);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(t.qts, RAM_NO_ECC + 0x108), ==, 'Q');
    qtest_writeq(t.qts, CRIME + 0x208 + 2 * 8, 0x108);
    qtest_writeq(t.qts, CRIME + 0x208, 0x100);
    g_assert_cmphex(qtest_readb(t.qts,
                                RAM_FULL + 2 * RAM_BANK_SIZE + 0x108),
                    ==, 'Q');
    o2_storage_ack_completion(t.qts);
    o2_storage_inquiry(t.qts, 0x81000100);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x92)) & 0x40,
                    ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x87)) & 4, ==, 4);
    o2_storage_assert_diagnostic_stop(t.qts, 1);
    events = o2_storage_stop(&t);
    g_assert_nonnull(strstr(events, "\"sequence\":2"));
    g_assert_nonnull(strstr(events, "\"sequence\":3"));
    g_assert_nonnull(strstr(events, "\"sequence\":4"));
    g_assert_nonnull(strstr(events, "\"pci_address\":\"0x40000180\""));
    g_assert_nonnull(strstr(events, "\"pci_address\":\"0x81000100\""));
    g_assert_nonnull(strstr(events, "\"attempts\":4,\"records\":4,"));
}

static void test_storage_queue_before_completion_interrupt(void)
{
    O2StorageTest t = o2_storage_start(true);
    g_autofree char *events = NULL;
    uint8_t answer[40];
    uint16_t queue_pc;

    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    qtest_memset(t.qts, RAM_FULL + 0x100, 0xa5, sizeof(answer));
    o2_storage_inquiry_paused(t.qts, 0x100);
    qtest_memread(t.qts, RAM_FULL + 0x100, answer, sizeof(answer));
    g_assert_cmpmem(answer + 8, 4, "QEMU", 4);
    for (unsigned i = 36; i < sizeof(answer); i++) {
        g_assert_cmphex(answer[i], ==, 0xa5);
    }
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 3, ==, 1);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 0);
    queue_pc = qtest_readb(t.qts, o2_storage_aic(0x62)) |
               qtest_readb(t.qts, o2_storage_aic(0x63)) << 8;

    qtest_writeb(t.qts, o2_storage_aic(0x60), 0x94);
    qtest_writeb(t.qts, o2_storage_aic(0x92), 1);
    o2_storage_step_once(t.qts, 1);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x87)) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x62)) |
                    qtest_readb(t.qts, o2_storage_aic(0x63)) << 8,
                    ==, queue_pc + 1);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 1);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 0);

    o2_storage_step_once(t.qts, 1);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x87)) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 1);
    qtest_writeb(t.qts, o2_storage_aic(0x92), 2);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 1);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9d)), ==, 1);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 0);
    events = o2_storage_stop(&t);
    g_assert_nonnull(strstr(events, "\"attempts\":1,\"records\":1,"));
}

static void check_storage_second_controller_dma_irq(const char *accel)
{
    static const uint8_t inquiry_cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    O2StorageTest t = o2_storage_start_accel(true, accel, 1, false, NULL);
    g_autofree char *events = NULL;
    uint8_t guarded[40];

    o2_storage_configure_aic(t.qts, 1);
    o2_storage_configure_aic(t.qts, 2);
    o2_program_banks(t.qts);
    qtest_irq_intercept_out_named(t.qts, "/machine/crime", "cpu-ip");
    qtest_writel(t.qts, MACE_PCI + 8, 3);
    qtest_writeq(t.qts, CRIME + 0x18, 3ULL << 8);
    memset(guarded, 0xa5, sizeof(guarded));
    qtest_memwrite(t.qts, RAM_FULL + 0x100, guarded, sizeof(guarded));
    o2_storage_command_impl(t.qts, 2, inquiry_cdb, sizeof(inquiry_cdb),
                            0x100, 36, false, false);

    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic_slot(2, 0x22)), ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic_slot(2, 0x9e)), ==, 1);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic_slot(1, 0x9e)), ==, 0);
    o2_aic_assert_irqs(t.qts, 2, 2, true);
    qtest_memread(t.qts, RAM_FULL + 0x100, guarded, sizeof(guarded));
    g_assert_cmpmem(guarded + 8, 4, "QEMU", 4);
    for (unsigned i = 36; i < sizeof(guarded); i++) {
        g_assert_cmphex(guarded[i], ==, 0xa5);
    }

    qtest_writeb(t.qts, o2_storage_aic_slot(2, 0x92), 2);
    o2_aic_assert_irqs(t.qts, 0, 0, false);
    qtest_writeb(t.qts, o2_storage_aic_slot(1, 0x87), 0x16);
    o2_aic_assert_irqs(t.qts, 1, 1, true);
    qtest_writeb(t.qts, o2_storage_aic_slot(1, 0x87), 0x06);
    o2_aic_assert_irqs(t.qts, 0, 0, false);

    events = o2_storage_stop(&t);
    g_assert_nonnull(strstr(events, "\"requester\":\"00:02.0\""));
    g_assert_null(strstr(events, "\"requester\":\"00:01.0\""));
    g_assert_nonnull(strstr(events, "\"attempts\":1,\"records\":1,"));
}

static void test_storage_second_controller_dma_irq(void)
{
    check_storage_second_controller_dma_irq("qtest");
}

static void test_storage_second_controller_dma_irq_tcg(void)
{
    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    check_storage_second_controller_dma_irq("tcg,thread=single");
}

static void test_storage_queued_metadata_fetch(void)
{
    static const unsigned ids[] = { 2, 4, 6 };
    static const uint32_t prefix_addr[] = { 0x40030000, 0x40031000 };
    static const uint32_t segment_addr[] = { 0x40040000, 0x40041000 };
    static const uint32_t cdb_addr[] = { 0x00050000, 0x00051000 };
    static const uint8_t cdb_bytes[2][12] = {
        { 0x12, 0, 0, 0, 36, 0, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee },
        { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0xee, 0xee },
    };
    uint8_t table[48], prefix[2][48], segments[2][32], cdb[2][28];
    uint8_t got[48], program[512] = { 0 };
    O2QueuedFetchPC pc;
    size_t program_len;

    memset(table, 0xa5, sizeof(table));
    memset(table + 8, 0xff, 32);
    stl_le_p(table + 8, 0x40032000);
    stl_le_p(table + 8 + 2 * 4, prefix_addr[0]);
    stl_le_p(table + 8 + 4 * 4, prefix_addr[1]);
    for (unsigned i = 0; i < 2; i++) {
        memset(prefix[i], 0xa5, sizeof(prefix[i]));
        memset(segments[i], 0x5a, sizeof(segments[i]));
        memset(cdb[i], 0xc3, sizeof(cdb[i]));
        for (unsigned byte = 0; byte < 32; byte++) {
            prefix[i][8 + byte] = 0x40 + i * 32 + byte;
        }
        prefix[i][8] = 2;
        prefix[i][9] = i ? 10 : 6;
        prefix[i][10] = 0;
        prefix[i][11] = 0;
        stl_le_p(prefix[i] + 12, segment_addr[i]);
        stl_le_p(prefix[i] + 16, cdb_addr[i]);
        stl_le_p(segments[i] + 8, 0x00060000 + i * 0x2000);
        stl_le_p(segments[i] + 12, 256);
        stl_le_p(segments[i] + 16, 0x00061000 + i * 0x2000);
        stl_le_p(segments[i] + 20, 0x80000100);
        memcpy(cdb[i] + 8, cdb_bytes[i], sizeof(cdb_bytes[i]));
    }
    program_len = o2_storage_build_queued_fetch(program, &pc, false, false);
    g_assert_cmpuint(program_len, <=, sizeof(program));

    for (unsigned case_no = 0; case_no < ARRAY_SIZE(ids); case_no++) {
        O2StorageTest t = o2_storage_start(false);
        g_autofree char *events = NULL;
        unsigned id = ids[case_no];

        o2_storage_configure_aic(t.qts, 1);
        o2_program_banks(t.qts);
        qtest_memwrite(t.qts, RAM_FULL + 0x20000 - 8,
                       table, sizeof(table));
        for (unsigned i = 0; i < 2; i++) {
            qtest_memwrite(t.qts, RAM_FULL +
                           (prefix_addr[i] & 0x3fffffff) - 8,
                           prefix[i], sizeof(prefix[i]));
            qtest_memwrite(t.qts, RAM_FULL +
                           (segment_addr[i] & 0x3fffffff) - 8,
                           segments[i], sizeof(segments[i]));
            qtest_memwrite(t.qts, RAM_FULL + cdb_addr[i] - 8,
                           cdb[i], sizeof(cdb[i]));
        }
        qtest_writeb(t.qts, o2_storage_aic(0x21), 0);
        qtest_writeb(t.qts, o2_storage_aic(0x22), 2);
        qtest_writeb(t.qts, o2_storage_aic(0x23), 0x40);
        o2_storage_load_stepped_program(t.qts, 1, program, program_len);
        g_assert_true(g_file_get_contents(t.events, &events, NULL, NULL));
        g_assert_cmpstr(events, ==, "");
        g_clear_pointer(&events, g_free);

        qtest_writeb(t.qts, o2_storage_aic(0x9b), id);
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9c)), ==, 1);
        o2_storage_step_to(t.qts, 1, pc.table);
        o2_storage_assert_dma_event(t.events, 1, 1,
                                    0x40020000 + id * 4, 4, false);
        for (unsigned byte = 0; byte < 4; byte++) {
            uint32_t address = UINT32_MAX;

            if (case_no < 2) {
                address = prefix_addr[case_no];
            }

            g_assert_cmphex(qtest_readb(t.qts,
                            o2_storage_aic(0x30 + byte)), ==,
                            address >> (byte * 8) & 0xff);
        }
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9c)), ==, 0);

        if (case_no == 2) {
            o2_storage_step_to(t.qts, 1, pc.invalid);
            o2_storage_step_to(t.qts, 1, pc.invalid + 2);
            g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x25)),
                            ==, 0xee);
            g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 3,
                            ==, 1);
            o2_storage_assert_dma_event(t.events, 1, 1,
                                        0x40020000 + id * 4, 4, false);
        } else {
            o2_storage_step_to(t.qts, 1, pc.prefix);
            o2_storage_assert_dma_event(t.events, 2, 1,
                                        prefix_addr[case_no], 32, false);
            qtest_writeb(t.qts, o2_storage_aic(0x90), 3);
            qtest_writeb(t.qts, o2_storage_aic(0x9a), 0);
            for (unsigned byte = 0; byte < 32; byte++) {
                g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0xa0 + byte)),
                                ==, prefix[case_no][8 + byte]);
            }

            o2_storage_step_to(t.qts, 1, pc.segments);
            o2_storage_assert_dma_event(t.events, 3, 1,
                                        segment_addr[case_no], 16, false);
            qtest_writeb(t.qts, o2_storage_aic(0x90), 4);
            qtest_writeb(t.qts, o2_storage_aic(0x9a), 0);
            for (unsigned byte = 0; byte < 16; byte++) {
                g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0xa0 + byte)),
                                ==, segments[case_no][8 + byte]);
            }

            o2_storage_step_to(t.qts, 1, pc.cdb);
            o2_storage_assert_dma_event(t.events, 4, 1,
                                        cdb_addr[case_no], 12, false);
            qtest_writeb(t.qts, o2_storage_aic(0x90), 5);
            qtest_writeb(t.qts, o2_storage_aic(0x9a), 0);
            for (unsigned byte = 0; byte < 12; byte++) {
                g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0xa0 + byte)),
                                ==, cdb_bytes[case_no][byte]);
            }
            o2_storage_step_to(t.qts, 1, pc.cdb + 1);
            g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 3,
                            ==, 1);
        }
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 0);
        qtest_memread(t.qts, RAM_FULL + 0x20000 - 8, got,
                      sizeof(table));
        g_assert_cmpmem(got, sizeof(table), table, sizeof(table));
        for (unsigned i = 0; i < 2; i++) {
            qtest_memread(t.qts, RAM_FULL +
                          (prefix_addr[i] & 0x3fffffff) - 8,
                          got, sizeof(prefix[i]));
            g_assert_cmpmem(got, sizeof(prefix[i]), prefix[i],
                            sizeof(prefix[i]));
            qtest_memread(t.qts, RAM_FULL +
                          (segment_addr[i] & 0x3fffffff) - 8,
                          got, sizeof(segments[i]));
            g_assert_cmpmem(got, sizeof(segments[i]), segments[i],
                            sizeof(segments[i]));
            qtest_memread(t.qts, RAM_FULL + cdb_addr[i] - 8,
                          got, sizeof(cdb[i]));
            g_assert_cmpmem(got, sizeof(cdb[i]), cdb[i], sizeof(cdb[i]));
        }
        events = o2_storage_stop(&t);
        g_assert_nonnull(strstr(events, "\"complete\":true"));
    }
}

static void test_storage_completion_window_before_irq(void)
{
    static const uint8_t ids[] = { 2, 4, 6 };
    static const unsigned physical_offsets[] = { 7, 3, 255 };
    const uint32_t pci_base = 0x00100000;
    const uint64_t ram_base = RAM_FULL + pci_base;
    O2StorageTest t = o2_storage_start(false);
    O2CompletionPC pc;
    uint8_t program[256] = { 0 };
    uint8_t guarded[272], got[272];
    g_autofree char *events = NULL;
    size_t program_len;
    uint8_t consumer = 4;

    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    qtest_irq_intercept_out_named(t.qts, "/machine/crime", "cpu-ip");
    qtest_writel(t.qts, MACE_PCI + 8, 1);
    qtest_writeq(t.qts, CRIME + 0x18, 1ULL << 8);
    memset(guarded, 0xa5, sizeof(guarded));
    memset(guarded + 8, 0xff, 256);
    qtest_memwrite(t.qts, ram_base - 8, guarded, sizeof(guarded));
    qtest_writeb(t.qts, o2_storage_aic(0x21), 0);
    qtest_writeb(t.qts, o2_storage_aic(0x22), 0x10);
    qtest_writeb(t.qts, o2_storage_aic(0x23), 0);
    program_len = o2_storage_build_completion_program(program, &pc);
    g_assert_cmpuint(program_len, <=, sizeof(program));
    o2_storage_load_stepped_program(t.qts, 1, program, program_len);

    for (unsigned record = 0; record < ARRAY_SIZE(ids); record++) {
        uint32_t expected_haddr = pci_base + (record == 2 ? 256 : 8);
        uint32_t haddr = 0;
        unsigned physical = consumer ^ 3;

        o2_storage_step_to(t.qts, 1, pc.write[record]);
        o2_storage_assert_dma_event(t.events, record + 1, 1,
                                    pci_base + (record == 2 ? 248 : 0),
                                    8, true);
        for (unsigned byte = 0; byte < 4; byte++) {
            haddr |= (uint32_t)qtest_readb(t.qts,
                        o2_storage_aic(0x88 + byte)) << (byte * 8);
        }
        g_assert_cmphex(haddr, ==, expected_haddr);
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x8c)), ==, 0);
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x94)) & 8, ==, 8);
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 0);
        g_assert_cmpuint(physical, ==, physical_offsets[record]);
        g_assert_cmphex(qtest_readb(t.qts, ram_base + physical), ==,
                        ids[record]);
        o2_aic_assert_irqs(t.qts, 0, 0, false);
        qtest_memread(t.qts, ram_base - 8, got, sizeof(got));
        for (unsigned byte = 0; byte < sizeof(got); byte++) {
            uint8_t expected = byte == 8 + physical ? ids[record] :
                               guarded[byte];

            g_assert_cmphex(got[byte], ==, expected);
        }
        consumer -= 4;
        if (record < 2) {
            qtest_writeb(t.qts, ram_base + physical, 0xff);
        }
    }
    g_assert_cmphex(consumer, ==, 248);

    o2_storage_step_to(t.qts, 1, pc.queue);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 3);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 0);
    o2_aic_assert_irqs(t.qts, 0, 0, false);
    o2_storage_step_to(t.qts, 1, pc.interrupt);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 2);
    o2_aic_assert_irqs(t.qts, 1, 1, true);
    qtest_writeb(t.qts, o2_storage_aic(0x92), 2);
    o2_aic_assert_irqs(t.qts, 0, 0, false);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 3);
    for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9d)), ==, ids[i]);
    }
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 0);
    events = o2_storage_stop(&t);
    g_assert_nonnull(strstr(events, "\"attempts\":3,\"records\":3,"));
}

static void test_storage_completion_window_failure(void)
{
    for (unsigned loss = 0; loss < 2; loss++) {
        O2StorageTest t = o2_storage_start(false);
        O2CompletionPC pc;
        uint8_t program[256] = { 0 };
        uint8_t guarded[272], got[272];
        g_autofree char *events = NULL;
        size_t program_len;

        o2_storage_configure_aic(t.qts, 1);
        o2_program_banks(t.qts);
        memset(guarded, 0xa5, sizeof(guarded));
        memset(guarded + 8, 0xff, 256);
        qtest_memwrite(t.qts, RAM_FULL + 0x00100000 - 8,
                       guarded, sizeof(guarded));
        qtest_writeb(t.qts, o2_storage_aic(0x21), 0);
        qtest_writeb(t.qts, o2_storage_aic(0x22), 0x10);
        qtest_writeb(t.qts, o2_storage_aic(0x23), loss ? 0 : 0x81);
        if (loss) {
            qtest_qmp_assert_success(t.qts,
                "{'execute':'qom-set','arguments':{'path':'/machine',"
                "'property':'x-test-storage-event-short-write',"
                "'value':true}}");
        }
        program_len = o2_storage_build_completion_program(program, &pc);
        o2_storage_load_stepped_program(t.qts, 1, program, program_len);
        o2_storage_step_to(t.qts, 1, pc.write[0]);
        o2_storage_assert_diagnostic_stop(t.qts, 1);
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x92)) & 0x40,
                        ==, 0);
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x8c)), ==, 8);
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x94)) & 8, ==, 0);
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 0);
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 0);
        if (!loss) {
            qtest_memread(t.qts, RAM_FULL + 0x00100000 - 8,
                          got, sizeof(got));
            g_assert_cmpmem(got, sizeof(got), guarded, sizeof(guarded));
        }
        events = o2_storage_stop(&t);
        if (loss) {
            g_assert_null(strstr(events, "\"complete\":true"));
        } else {
            const char *record = strstr(events,
                "\"pci_address\":\"0x81100000\"");
            const char *end;

            g_assert_nonnull(record);
            end = strchr(record, '\n');
            g_assert_nonnull(end);
            g_assert_nonnull(strstr(events, "\"requester\":\"00:01.0\""));
            g_assert_nonnull(g_strstr_len(record, end - record,
                                          "\"direction\":\"to-memory\""));
            g_assert_nonnull(g_strstr_len(record, end - record,
                                          "\"length\":8"));
            g_assert_null(g_strstr_len(record, end - record,
                                       "\"result\":0"));
            g_assert_nonnull(strstr(events,
                                    "\"attempts\":1,\"records\":1,"));
        }
    }
}

static void test_storage_queued_inquiry_to_completion(void)
{
    static const uint8_t inquiry_cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    const uint32_t table_addr = 0x40020000;
    const uint32_t prefix_addr = 0x40030000;
    const uint32_t segment_addr = 0x40040000;
    const uint32_t cdb_addr = 0x00050000;
    const uint32_t data_addr = 0x00060000;
    const uint32_t completion_addr = 0x00100000;
    O2StorageTest t = o2_storage_start_accel(true, "qtest", 0, true, NULL);
    O2QueuedFetchPC pc;
    uint8_t table[48], prefix[48], segments[32], cdb[28];
    uint8_t data[52], completion[272], program[1024] = { 0 };
    g_autofree char *events = NULL;
    g_autofree char *trace = NULL;
    size_t program_len;

    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    qtest_irq_intercept_out_named(t.qts, "/machine/crime", "cpu-ip");
    qtest_writel(t.qts, MACE_PCI + 8, 1);
    qtest_writeq(t.qts, CRIME + 0x18, 1ULL << 8);
    memset(table, 0xa5, sizeof(table));
    memset(table + 8, 0xff, 32);
    stl_le_p(table + 8, 0x40032000);
    stl_le_p(table + 8 + 2 * 4, prefix_addr);
    memset(prefix, 0xa5, sizeof(prefix));
    memset(prefix + 8, 0x5a, 32);
    prefix[8] = 2;
    prefix[9] = 6;
    prefix[10] = 0;
    prefix[11] = 0;
    stl_le_p(prefix + 12, segment_addr);
    stl_le_p(prefix + 16, cdb_addr);
    memset(segments, 0xa5, sizeof(segments));
    stl_le_p(segments + 8, data_addr);
    stl_le_p(segments + 12, 36);
    stl_le_p(segments + 16, 0);
    stl_le_p(segments + 20, 0x80000000);
    memset(cdb, 0xa5, sizeof(cdb));
    memset(cdb + 8, 0xee, 12);
    memcpy(cdb + 8, inquiry_cdb, sizeof(inquiry_cdb));
    memset(data, 0xa5, sizeof(data));
    memset(completion, 0xa5, sizeof(completion));
    memset(completion + 8, 0xff, 256);
    qtest_memwrite(t.qts, RAM_FULL + (table_addr & 0x3fffffff) - 8,
                   table, sizeof(table));
    qtest_memwrite(t.qts, RAM_FULL + (prefix_addr & 0x3fffffff) - 8,
                   prefix, sizeof(prefix));
    qtest_memwrite(t.qts, RAM_FULL + (segment_addr & 0x3fffffff) - 8,
                   segments, sizeof(segments));
    qtest_memwrite(t.qts, RAM_FULL + cdb_addr - 8, cdb, sizeof(cdb));
    qtest_memwrite(t.qts, RAM_FULL + data_addr - 8, data, sizeof(data));
    qtest_memwrite(t.qts, RAM_FULL + completion_addr - 8,
                   completion, sizeof(completion));
    qtest_writeb(t.qts, o2_storage_aic(0x21), 0);
    qtest_writeb(t.qts, o2_storage_aic(0x22), 2);
    qtest_writeb(t.qts, o2_storage_aic(0x23), 0x40);
    qtest_writeb(t.qts, o2_storage_aic(0x27), 0);
    qtest_writeb(t.qts, o2_storage_aic(0x28), 0x10);
    qtest_writeb(t.qts, o2_storage_aic(0x29), 0);
    program_len = o2_storage_build_queued_fetch(program, &pc, true, false);
    g_assert_cmpuint(program_len, <=, sizeof(program));
    o2_storage_load_stepped_program(t.qts, 1, program, program_len);
    qtest_writeb(t.qts, o2_storage_aic(0x9b), 2);

    o2_storage_step_to(t.qts, 1, pc.table);
    o2_storage_assert_dma_event(t.events, 1, 1, table_addr + 8, 4, false);
    o2_storage_step_to(t.qts, 1, pc.prefix);
    o2_storage_assert_dma_event(t.events, 2, 1, prefix_addr, 32, false);
    o2_storage_step_to(t.qts, 1, pc.segments);
    o2_storage_assert_dma_event(t.events, 3, 1, segment_addr, 16, false);
    o2_storage_step_to(t.qts, 1, pc.cdb);
    o2_storage_assert_dma_event(t.events, 4, 1, cdb_addr, 12, false);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 0);
    o2_storage_step_to(t.qts, 1, pc.command);
    o2_storage_step_to(t.qts, 1, pc.completion);
    o2_storage_assert_dma_event(t.events, 6, 1,
                                completion_addr, 8, true);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x22)), ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x23)), ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x0c)) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 0);
    o2_aic_assert_irqs(t.qts, 0, 0, false);
    qtest_memread(t.qts, RAM_FULL + data_addr - 8, data, sizeof(data));
    g_assert_cmpmem(data + 16, 4, "QEMU", 4);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
        g_assert_cmphex(data[44 + i], ==, 0xa5);
    }
    qtest_memread(t.qts, RAM_FULL + completion_addr - 8,
                  completion, sizeof(completion));
    for (unsigned i = 0; i < sizeof(completion); i++) {
        g_assert_cmphex(completion[i], ==,
                        i == 15 ? 2 : (i >= 8 && i < 264 ? 0xff : 0xa5));
    }

    o2_storage_step_to(t.qts, 1, pc.queue);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 1);
    o2_aic_assert_irqs(t.qts, 0, 0, false);
    o2_storage_step_to(t.qts, 1, pc.interrupt);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 2);
    o2_aic_assert_irqs(t.qts, 1, 1, true);
    qtest_writeb(t.qts, o2_storage_aic(0x92), 2);
    o2_aic_assert_irqs(t.qts, 0, 0, false);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9d)), ==, 2);
    g_assert_true(g_file_get_contents(t.trace, &trace, NULL, NULL));
    g_assert_true(g_regex_match_simple(
        "scsi_disk_new_request[^\\n]*data=12 00 00 00 24 00",
        trace, 0, 0));
    events = o2_storage_stop(&t);
    g_assert_nonnull(strstr(events, "\"attempts\":6,\"records\":6,"));
}

typedef enum O2QueuedOutcome {
    O2_QUEUED_COMPLETE,
    O2_QUEUED_DMA_FAILURE,
    O2_QUEUED_CHECK_CONDITION,
    O2_QUEUED_SCSI_RESET,
    O2_QUEUED_CHIP_RESET,
    O2_QUEUED_PCI_RESET,
} O2QueuedOutcome;

static void check_storage_queued_rw_to_completion(bool write,
                                                  unsigned slot,
                                                  O2QueuedOutcome outcome)
{
    static const uint8_t reset_read[10] = {
        0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0
    };
    static const uint8_t sense[6] = { 0x03, 0, 0, 0, 18, 0 };
    const uint8_t cdb_bytes[10] = {
        write ? 0x2a : 0x28, 0,
        outcome == O2_QUEUED_CHECK_CONDITION ? 0xff : 0,
        outcome == O2_QUEUED_CHECK_CONDITION ? 0xff : 0,
        outcome == O2_QUEUED_CHECK_CONDITION ? 0xff : 0,
        outcome == O2_QUEUED_CHECK_CONDITION ? 0xff : 1,
        0, 0, 1, 0
    };
    const uint32_t table_addr = 0x40020000;
    const uint32_t prefix_addr = 0x40030000;
    const uint32_t segment_addr = 0x40040000;
    const uint32_t cdb_addr = 0x00050000;
    const uint32_t first_addr = 0x00060000;
    const uint32_t second_addr = 0x00062000;
    const uint32_t completion_addr = 0x00100000;
    O2StorageTest t;
    O2QueuedFetchPC pc;
    uint8_t disk_seed[512], table[48], prefix[48], segments[32], cdb[28];
    uint8_t first[272], second[272], completion[272], program[1024] = { 0 };
    g_autofree char *path = NULL;
    g_autofree char *events = NULL;
    g_autofree char *trace = NULL;
    g_autofree char *disk_bytes = NULL;
    gsize disk_size;
    size_t program_len;
    unsigned attempts;
    int fd;

    fd = g_file_open_tmp(write ? "o2-aic-queued-write-XXXXXX" :
                         "o2-aic-queued-read-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 4096), ==, 0);
    for (unsigned i = 0; i < sizeof(disk_seed); i++) {
        disk_seed[i] = i * 37 + 11;
    }
    if (!write) {
        g_assert_cmpint(pwrite(fd, disk_seed, sizeof(disk_seed), 512), ==,
                        sizeof(disk_seed));
    }
    close(fd);

    t = o2_storage_start_accel(true, "qtest", slot - 1, true, path);
    o2_storage_configure_aic(t.qts, slot);
    o2_program_banks(t.qts);
    qtest_irq_intercept_out_named(t.qts, "/machine/crime", "cpu-ip");
    qtest_writel(t.qts, MACE_PCI + 8, 1U << (slot - 1));
    qtest_writeq(t.qts, CRIME + 0x18, 1ULL << (7 + slot));
    o2_storage_command_impl(t.qts, slot, reset_read, sizeof(reset_read),
                            0x100, 512, false, false);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic_slot(slot, 0x22)), ==, 2);
    o2_storage_ack_completion_slot(t.qts, slot);
    o2_storage_command_impl(t.qts, slot, sense, sizeof(sense),
                            0x300, 18, false, false);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic_slot(slot, 0x22)), ==, 0);
    o2_storage_ack_completion_slot(t.qts, slot);
    o2_storage_assert_dma_event(t.events, 1, slot, 0x300, 18, true);

    memset(table, 0xa5, sizeof(table));
    memset(table + 8, 0xff, 32);
    stl_le_p(table + 8, 0x40032000);
    stl_le_p(table + 8 + 2 * 4, prefix_addr);
    memset(prefix, 0xa5, sizeof(prefix));
    memset(prefix + 8, 0x5a, 32);
    prefix[8] = 2;
    prefix[9] = 10;
    prefix[10] = 0;
    prefix[11] = 0;
    stl_le_p(prefix + 12, segment_addr);
    stl_le_p(prefix + 16, cdb_addr);
    memset(segments, 0xa5, sizeof(segments));
    stl_le_p(segments + 8, first_addr);
    stl_le_p(segments + 12, 256);
    stl_le_p(segments + 16,
             outcome == O2_QUEUED_DMA_FAILURE ? 0x81000100 : second_addr);
    stl_le_p(segments + 20, 0x80000100);
    memset(cdb, 0xa5, sizeof(cdb));
    memset(cdb + 8, 0xee, 12);
    memcpy(cdb + 8, cdb_bytes, sizeof(cdb_bytes));
    memset(first, 0xa5, sizeof(first));
    memset(second, 0x5a, sizeof(second));
    if (write) {
        memcpy(first + 8, disk_seed, 256);
        memcpy(second + 8, disk_seed + 256, 256);
    }
    memset(completion, 0xa5, sizeof(completion));
    memset(completion + 8, 0xff, 256);
    qtest_memwrite(t.qts, RAM_FULL + (table_addr & 0x3fffffff) - 8,
                   table, sizeof(table));
    qtest_memwrite(t.qts, RAM_FULL + (prefix_addr & 0x3fffffff) - 8,
                   prefix, sizeof(prefix));
    qtest_memwrite(t.qts, RAM_FULL + (segment_addr & 0x3fffffff) - 8,
                   segments, sizeof(segments));
    qtest_memwrite(t.qts, RAM_FULL + cdb_addr - 8, cdb, sizeof(cdb));
    qtest_memwrite(t.qts, RAM_FULL + first_addr - 8, first, sizeof(first));
    qtest_memwrite(t.qts, RAM_FULL + second_addr - 8,
                   second, sizeof(second));
    qtest_memwrite(t.qts, RAM_FULL + completion_addr - 8,
                   completion, sizeof(completion));
    qtest_writeb(t.qts, o2_storage_aic_slot(slot, 0x21), 0);
    qtest_writeb(t.qts, o2_storage_aic_slot(slot, 0x22), 2);
    qtest_writeb(t.qts, o2_storage_aic_slot(slot, 0x23), 0x40);
    qtest_writeb(t.qts, o2_storage_aic_slot(slot, 0x27), 0);
    qtest_writeb(t.qts, o2_storage_aic_slot(slot, 0x28), 0x10);
    qtest_writeb(t.qts, o2_storage_aic_slot(slot, 0x29), 0);
    program_len = o2_storage_build_queued_fetch(program, &pc, true, write);
    g_assert_cmpuint(program_len, <=, sizeof(program));
    o2_storage_load_stepped_program(t.qts, slot, program, program_len);
    qtest_writeb(t.qts, o2_storage_aic_slot(slot, 0x9b), 2);

    o2_storage_step_to(t.qts, slot, pc.table);
    o2_storage_assert_dma_event(t.events, 2, slot, table_addr + 8, 4, false);
    o2_storage_step_to(t.qts, slot, pc.prefix);
    o2_storage_assert_dma_event(t.events, 3, slot, prefix_addr, 32, false);
    o2_storage_step_to(t.qts, slot, pc.segments);
    o2_storage_assert_dma_event(t.events, 4, slot, segment_addr, 16, false);
    o2_storage_step_to(t.qts, slot, pc.cdb);
    o2_storage_assert_dma_event(t.events, 5, slot, cdb_addr, 12, false);
    o2_storage_step_to(t.qts, slot, pc.command);
    if (outcome == O2_QUEUED_CHECK_CONDITION) {
        o2_storage_step_to(t.qts, slot, pc.completion);
        o2_storage_assert_dma_event(t.events, 6, slot,
                                    completion_addr, 8, true);
        g_assert_cmphex(qtest_readb(t.qts,
                                    o2_storage_aic_slot(slot, 0x22)), ==, 2);
        g_assert_cmphex(qtest_readb(t.qts,
                                    o2_storage_aic_slot(slot, 0x9e)), ==, 0);
        qtest_memread(t.qts, RAM_FULL + first_addr - 8,
                      first, sizeof(first));
        for (unsigned i = 0; i < sizeof(first); i++) {
            g_assert_cmphex(first[i], ==, 0xa5);
        }
        o2_storage_step_to(t.qts, slot, pc.queue);
        g_assert_cmphex(qtest_readb(t.qts,
                                    o2_storage_aic_slot(slot, 0x9e)), ==, 1);
        o2_storage_step_to(t.qts, slot, pc.interrupt);
        o2_aic_assert_irqs(t.qts, slot, slot, true);
        qtest_writeb(t.qts, o2_storage_aic_slot(slot, 0x92), 2);
        o2_aic_assert_irqs(t.qts, 0, 0, false);
        g_assert_cmphex(qtest_readb(t.qts,
                                    o2_storage_aic_slot(slot, 0x9d)), ==, 2);
        g_assert_true(g_file_get_contents(t.trace, &trace, NULL, NULL));
        g_assert_true(g_regex_match_simple(
            "scsi_disk_new_request[^\\n]*"
            "data=28 00 ff ff ff ff 00 00 01 00", trace, 0, 0));
        events = o2_storage_stop(&t);
        g_assert_nonnull(strstr(events,
            "\"attempts\":6,\"records\":6,"));
        unlink(path);
        return;
    }
    o2_storage_step_to(t.qts, slot, pc.first_segment);
    attempts = o2_storage_assert_dma_span(t.events, 5, slot,
                                          first_addr, 256, write);
    if (outcome == O2_QUEUED_CHIP_RESET ||
        outcome == O2_QUEUED_PCI_RESET) {
        qtest_memread(t.qts, RAM_FULL + first_addr - 8,
                      first, sizeof(first));
        g_assert_cmpmem(first + 8, 256, disk_seed, 256);
        qtest_memread(t.qts, RAM_FULL + second_addr - 8,
                      second, sizeof(second));
        for (unsigned i = 0; i < sizeof(second); i++) {
            g_assert_cmphex(second[i], ==, 0x5a);
        }
        if (outcome == O2_QUEUED_CHIP_RESET) {
            qtest_writeb(t.qts, o2_storage_aic_slot(slot, 0x87), 1);
            g_assert_cmphex(o2_aic_config_read(t.qts, slot, 4) & 6,
                            ==, 6);
            g_assert_cmphex(qtest_readb(t.qts,
                                        o2_storage_aic_slot(slot, 0x93)),
                            ==, 0);
            g_assert_cmphex(qtest_readb(t.qts,
                                        o2_storage_aic_slot(slot, 0x9e)),
                            ==, 0);
        } else {
            qtest_system_reset(t.qts);
            g_assert_cmphex(o2_aic_config_read(t.qts, slot, 4) & 6,
                            ==, 0);
            o2_storage_configure_aic(t.qts, slot);
            g_assert_cmphex(qtest_readb(t.qts,
                                        o2_storage_aic_slot(slot, 0x9e)),
                            ==, 0);
        }
        qtest_memread(t.qts, RAM_FULL + first_addr - 8,
                      first, sizeof(first));
        g_assert_cmpmem(first + 8, 256, disk_seed, 256);
        qtest_memread(t.qts, RAM_FULL + completion_addr - 8,
                      completion, sizeof(completion));
        for (unsigned i = 0; i < sizeof(completion); i++) {
            g_assert_cmphex(completion[i], ==,
                            i >= 8 && i < 264 ? 0xff : 0xa5);
        }
        events = o2_storage_stop(&t);
        g_assert_null(strstr(events, "\"pci_address\":\"0x00100000\""));
        unlink(path);
        return;
    }
    if (outcome == O2_QUEUED_DMA_FAILURE) {
        o2_storage_step_to(t.qts, slot, pc.second_segment);
        o2_storage_assert_diagnostic_stop(t.qts, slot);
        g_assert_cmphex(qtest_readb(t.qts,
                                    o2_storage_aic_slot(slot, 0x92)) & 0x40,
                        ==, 0);
        g_assert_cmphex(qtest_readb(t.qts,
                                    o2_storage_aic_slot(slot, 0x91)) & 2,
                        ==, 0);
        g_assert_cmphex(qtest_readb(t.qts,
                                    o2_storage_aic_slot(slot, 0x9e)), ==, 0);
        qtest_memread(t.qts, RAM_FULL + first_addr - 8,
                      first, sizeof(first));
        g_assert_cmpmem(first + 8, 256, disk_seed, 256);
        qtest_memread(t.qts, RAM_FULL + second_addr - 8,
                      second, sizeof(second));
        for (unsigned i = 0; i < sizeof(second); i++) {
            g_assert_cmphex(second[i], ==, 0x5a);
        }
        qtest_memread(t.qts, RAM_FULL + completion_addr - 8,
                      completion, sizeof(completion));
        for (unsigned i = 0; i < sizeof(completion); i++) {
            g_assert_cmphex(completion[i], ==,
                            i >= 8 && i < 264 ? 0xff : 0xa5);
        }
        events = o2_storage_stop(&t);
        g_assert_nonnull(strstr(events,
            "\"pci_address\":\"0x81000100\""));
        g_assert_null(strstr(events,
            "\"pci_address\":\"0x81000100\","
            "\"direction\":\"to-memory\","
            "\"length\":8,\"result\":0"));
        unlink(path);
        return;
    }
    o2_storage_step_to(t.qts, slot, pc.second_segment);
    attempts = o2_storage_assert_dma_span(t.events, attempts, slot,
                                          second_addr, 256, write);
    o2_storage_step_to(t.qts, slot, pc.completion);
    attempts = o2_storage_assert_dma_span(t.events, attempts, slot,
                                          completion_addr, 8, false);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic_slot(slot, 0x22)), ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic_slot(slot, 0x23)), ==, 0);
    g_assert_cmphex(qtest_readb(t.qts,
                                o2_storage_aic_slot(slot, 0x0c)) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic_slot(slot, 0x9e)), ==, 0);
    o2_aic_assert_irqs(t.qts, 0, 0, false);
    qtest_memread(t.qts, RAM_FULL + first_addr - 8, first, sizeof(first));
    qtest_memread(t.qts, RAM_FULL + second_addr - 8,
                  second, sizeof(second));
    g_assert_cmpmem(first + 8, 256, disk_seed, 256);
    g_assert_cmpmem(second + 8, 256, disk_seed + 256, 256);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(first[i], ==, 0xa5);
        g_assert_cmphex(first[264 + i], ==, 0xa5);
        g_assert_cmphex(second[i], ==, 0x5a);
        g_assert_cmphex(second[264 + i], ==, 0x5a);
    }
    qtest_memread(t.qts, RAM_FULL + completion_addr - 8,
                  completion, sizeof(completion));
    for (unsigned i = 0; i < sizeof(completion); i++) {
        g_assert_cmphex(completion[i], ==,
                        i == 15 ? 2 : (i >= 8 && i < 264 ? 0xff : 0xa5));
    }
    o2_storage_step_to(t.qts, slot, pc.queue);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic_slot(slot, 0x9e)), ==, 1);
    if (outcome == O2_QUEUED_SCSI_RESET) {
        qtest_writeb(t.qts, o2_storage_aic_slot(slot, 0x00), 1);
        g_assert_cmphex(qtest_readb(t.qts,
                                    o2_storage_aic_slot(slot, 0x9e)), ==, 1);
        g_assert_cmphex(qtest_readb(t.qts,
                                    o2_storage_aic_slot(slot, 0x91)) & 2,
                        ==, 0);
        qtest_memread(t.qts, RAM_FULL + completion_addr - 8,
                      completion, sizeof(completion));
        g_assert_cmphex(completion[15], ==, 2);
    }
    o2_aic_assert_irqs(t.qts, 0, 0, false);
    o2_storage_step_to(t.qts, slot, pc.interrupt);
    o2_aic_assert_irqs(t.qts, slot, slot, true);
    qtest_writeb(t.qts, o2_storage_aic_slot(slot, 0x92), 2);
    o2_aic_assert_irqs(t.qts, 0, 0, false);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic_slot(slot, 0x9d)), ==, 2);
    g_assert_true(g_file_get_contents(t.trace, &trace, NULL, NULL));
    g_assert_true(g_regex_match_simple(
        write ? "scsi_disk_new_request[^\\n]*"
                "data=2a 00 00 00 00 01 00 00 01 00" :
                "scsi_disk_new_request[^\\n]*"
                "data=28 00 00 00 00 01 00 00 01 00",
        trace, 0, 0));
    events = o2_storage_stop(&t);
    g_autofree char *summary = g_strdup_printf(
        "\"attempts\":%u,\"records\":%u,", attempts, attempts);

    g_assert_nonnull(strstr(events, summary));
    if (write) {
        g_assert_true(g_file_get_contents(path, &disk_bytes, &disk_size,
                                          NULL));
        g_assert_cmpuint(disk_size, >=, 1024);
        g_assert_cmpmem(disk_bytes + 512, 512, disk_seed, sizeof(disk_seed));
    }
    unlink(path);
}

static void test_storage_queued_read_to_completion(void)
{
    check_storage_queued_rw_to_completion(false, 1,
                                          O2_QUEUED_COMPLETE);
}

static void test_storage_queued_write_to_completion(void)
{
    check_storage_queued_rw_to_completion(true, 1,
                                          O2_QUEUED_COMPLETE);
}

static void test_storage_queued_read_second_controller(void)
{
    check_storage_queued_rw_to_completion(false, 2,
                                          O2_QUEUED_COMPLETE);
}

static void test_storage_queued_write_second_controller(void)
{
    check_storage_queued_rw_to_completion(true, 2,
                                          O2_QUEUED_COMPLETE);
}

static void test_storage_queued_second_dma_failure(void)
{
    check_storage_queued_rw_to_completion(false, 1,
                                          O2_QUEUED_DMA_FAILURE);
}

static void test_storage_queued_check_condition(void)
{
    check_storage_queued_rw_to_completion(false, 1,
                                          O2_QUEUED_CHECK_CONDITION);
}

static void test_storage_queued_scsi_reset(void)
{
    check_storage_queued_rw_to_completion(false, 1,
                                          O2_QUEUED_SCSI_RESET);
}

static void test_storage_queued_chip_reset(void)
{
    check_storage_queued_rw_to_completion(false, 1,
                                          O2_QUEUED_CHIP_RESET);
}

static void test_storage_queued_pci_reset(void)
{
    check_storage_queued_rw_to_completion(false, 1,
                                          O2_QUEUED_PCI_RESET);
}

static void test_storage_diagnostic_event_loss(void)
{
    O2StorageTest t = o2_storage_start(true);
    g_autofree char *events = NULL;

    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    qtest_qmp_assert_success(t.qts,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'x-test-storage-event-short-write','value':true}}");
    o2_storage_inquiry(t.qts, 0x100);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 0);
    events = o2_storage_stop(&t);
    g_assert_null(strstr(events, "\"type\":\"complete\""));
}

static void test_storage_diagnostic_target_unplug(void)
{
    O2StorageTest t = o2_storage_start(true);
    g_autoptr(QDict) reply = NULL;
    g_autofree char *events = NULL;

    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    reply = qtest_qmp(t.qts,
        "{'execute':'device_del','arguments':{'id':'storage0'}}");
    g_assert_nonnull(qdict_get_qdict(reply, "error"));
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(reply, "error"),
                                         "desc"),
                            "storage diagnostic target cannot be unplugged"));
    o2_storage_inquiry(t.qts, 0x100);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 2);
    events = o2_storage_stop(&t);
    g_assert_nonnull(strstr(events, "\"attempts\":1,\"records\":1,"));
}

static void test_storage_diagnostic_target_qom_unrealize(void)
{
    O2StorageTest t = o2_storage_start(true);
    g_autoptr(QDict) reply = NULL;
    g_autoptr(QDict) realized = NULL;
    g_autofree char *events = NULL;

    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    reply = qtest_qmp(t.qts,
        "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/storage0',"
        "'property':'realized','value':false}}");
    g_assert_nonnull(qdict_get_qdict(reply, "error"));
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(reply, "error"),
                                         "desc"),
                            "storage diagnostic target cannot be unplugged"));
    realized = qtest_qmp(t.qts,
        "{'execute':'qom-get','arguments':{"
        "'path':'/machine/peripheral/storage0','property':'realized'}}");
    g_assert_true(qdict_get_bool(realized, "return"));
    o2_storage_inquiry(t.qts, 0x100);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 2);
    events = o2_storage_stop(&t);
    g_assert_nonnull(strstr(events, "\"attempts\":1,\"records\":1,"));
}

static void test_storage_diagnostic_final_sync_loss(void)
{
    O2StorageTest t = o2_storage_start(false);
    g_autofree char *events = NULL;

    qtest_qmp_assert_success(t.qts,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'x-test-storage-final-sync-fail','value':true}}");
    qtest_set_expected_status(t.qts, EXIT_FAILURE);
    events = o2_storage_stop(&t);
    g_assert_null(strstr(events, "\"type\":\"complete\""));
}

static void test_storage_diagnostic_master_gate(void)
{
    O2StorageTest t = o2_storage_start(true);
    g_autofree char *events = NULL;
    uint8_t guard[36], after[36];

    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    o2_aic_config_write(t.qts, 1, 4, 2);
    memset(guard, 0xa5, sizeof(guard));
    qtest_memwrite(t.qts, RAM_FULL + 0x100, guard, sizeof(guard));
    o2_storage_inquiry(t.qts, 0x100);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 0x08,
                    ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 0x02,
                    ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x92)), ==, 0);
    g_assert_cmphex(o2_storage_host_value(t.qts, 0x8c, 3), ==, 36);
    qtest_memread(t.qts, RAM_FULL + 0x100, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), guard, sizeof(guard));
    events = o2_storage_stop(&t);
    g_assert_null(strstr(events, "\"type\":\"dma\""));
    g_assert_nonnull(strstr(events, "\"attempts\":0,\"records\":0,"));
}

static void test_storage_diagnostic_dma_bounds(void)
{
    static const struct {
        uint32_t address;
        bool wrong_geometry;
        bool succeeds;
    } cases[] = {
        { 0x38000100, true, false },
        { 0x3a000100, true, false },
        { 0x3ffffffe, false, true },
        { 0x7ffffffe, false, false },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        O2StorageTest t = o2_storage_start(true);
        g_autofree char *events = NULL;
        uint8_t first[36], last[16];

        o2_storage_configure_aic(t.qts, 1);
        o2_program_banks(t.qts);
        qtest_memset(t.qts, RAM_FULL + 0x100, 0x69, sizeof(first));
        qtest_memset(t.qts, RAM_FULL + GiB - sizeof(last),
                     0xa5, sizeof(last));
        if (cases[i].wrong_geometry) {
            qtest_writeq(t.qts, CRIME + 0x208 + 7 * 8, 0x1c);
        }
        g_test_message("PCI destination 0x%08x", cases[i].address);
        o2_storage_inquiry(t.qts, cases[i].address);
        if (cases[i].succeeds) {
            o2_storage_assert_dma_span(t.events, 0, 1, cases[i].address,
                                       36, false);
            g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x92)), ==, 0);
            g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2,
                            ==, 2);
            g_assert_cmphex(o2_storage_host_value(t.qts, 0x88, 4), ==,
                            cases[i].address + 36);
            g_assert_cmphex(o2_storage_host_value(t.qts, 0x8c, 3), ==, 0);
        } else {
            g_auto(GStrv) lines = NULL;
            unsigned credited = 0;
            bool failed = false;

            g_assert_true(g_file_get_contents(t.events, &events, NULL, NULL));
            lines = g_strsplit(events, "\n", -1);
            for (unsigned row = 0; lines[row] && *lines[row]; row++) {
                g_autoptr(QDict) record = qobject_to(QDict,
                    qobject_from_json(lines[row], NULL));
                uint64_t address;
                unsigned count;

                g_assert_nonnull(record);
                if (strcmp(qdict_get_str(record, "type"), "dma")) {
                    continue;
                }
                g_assert_false(failed);
                g_assert_cmpstr(qdict_get_str(record, "requester"), ==,
                                "00:01.0");
                g_assert_cmpstr(qdict_get_str(record, "direction"), ==,
                                "to-memory");
                g_assert_cmpint(qemu_strtou64(
                    qdict_get_str(record, "pci_address"), NULL, 0, &address),
                    ==, 0);
                g_assert_cmphex(address, ==, cases[i].address + credited);
                count = qdict_get_uint(record, "length");
                g_assert_cmpuint(count, >, 0);
                g_assert_cmpuint(count, <=, 36 - credited);
                if (qdict_get_uint(record, "result") == 0) {
                    credited += count;
                } else {
                    failed = true;
                }
            }
            g_assert_true(failed);
            g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2,
                            ==, 0);
            g_assert_cmphex(o2_storage_host_value(t.qts, 0x88, 4), ==,
                            cases[i].address + credited);
            g_assert_cmphex(o2_storage_host_value(t.qts, 0x8c, 3), ==,
                            36 - credited);
            g_clear_pointer(&events, g_free);
        }
        if (cases[i].wrong_geometry) {
            qtest_writeq(t.qts, CRIME + 0x208 + 7 * 8, 0x11c);
        }
        qtest_memread(t.qts, RAM_FULL + 0x100, first, sizeof(first));
        qtest_memread(t.qts, RAM_FULL + GiB - sizeof(last),
                      last, sizeof(last));
        for (unsigned b = 0; b < sizeof(first); b++) {
            g_assert_cmphex(first[b], ==, 0x69);
        }
        unsigned guard_bytes = sizeof(last);

        if (!cases[i].wrong_geometry) {
            guard_bytes -= 4;
        }
        for (unsigned b = 0; b < guard_bytes; b++) {
            g_assert_cmphex(last[b], ==, 0xa5);
        }
        events = o2_storage_stop(&t);

    }
}

static void test_storage_diagnostic_reverse_dma(void)
{
    static const uint8_t write_cdb[10] = {
        0x2a, 0, 0, 0, 0, 0, 0, 0, 1, 0
    };
    static const uint8_t sense_cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    O2StorageTest t = o2_storage_start(true);
    g_autofree char *events = NULL;
    uint8_t source[512], after[512];
    unsigned attempts;

    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    for (unsigned i = 0; i < sizeof(source); i++) {
        source[i] = i * 37 + 11;
    }
    qtest_memwrite(t.qts, RAM_FULL + 0x300, source, sizeof(source));
    o2_storage_command(t.qts, write_cdb, sizeof(write_cdb),
                       0x300, sizeof(source), true);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x22)), ==, 2);
    o2_storage_ack_completion(t.qts);
    o2_storage_command(t.qts, sense_cdb, sizeof(sense_cdb),
                       0x200, 18, false);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x22)), ==, 0);
    attempts = o2_storage_assert_dma_span(t.events, 0, 1, 0x200, 18, false);
    o2_storage_ack_completion(t.qts);
    o2_storage_command(t.qts, write_cdb, sizeof(write_cdb),
                       0x300, sizeof(source), true);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x22)), ==, 0);
    attempts = o2_storage_assert_dma_span(t.events, attempts, 1,
                                          0x300, sizeof(source), true);
    o2_storage_ack_completion(t.qts);
    o2_storage_command(t.qts, write_cdb, sizeof(write_cdb),
                       0x40000300, sizeof(source), true);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x22)), ==, 0);
    attempts = o2_storage_assert_dma_span(t.events, attempts, 1,
                                          0x40000300, sizeof(source), true);
    qtest_memread(t.qts, RAM_NO_ECC + 0x300, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), source, sizeof(source));
    events = o2_storage_stop(&t);
    g_assert_nonnull(strstr(events, "\"direction\":\"from-memory\""));
    g_assert_nonnull(strstr(events, "\"pci_address\":\"0x00000300\""));
    g_assert_nonnull(strstr(events, "\"pci_address\":\"0x40000300\""));
    g_autofree char *summary = g_strdup_printf(
        "\"attempts\":%u,\"records\":%u,", attempts, attempts);

    g_assert_nonnull(strstr(events, summary));
}

static void test_storage_diagnostic_byte_widths(void)
{
    static const uint32_t offsets[] = {
        0x100, 0x103, 0x107, 0x11f, 0x13f, 0x17f,
    };
    O2StorageTest t = o2_storage_start(true);
    g_autofree char *events = NULL;
    uint8_t expected[8];
    uint8_t actual[16];

    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    o2_storage_inquiry_len(t.qts, 0x800, sizeof(expected));
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 2);
    qtest_memread(t.qts, RAM_FULL + 0x800, expected, sizeof(expected));
    o2_storage_ack_completion(t.qts);

    for (unsigned i = 0; i < G_N_ELEMENTS(offsets); i++) {
        for (unsigned length = 1; length <= 8; length *= 2) {
            for (unsigned view = 0; view < 2; view++) {
                uint32_t pci_addr = offsets[i] +
                    (view ? MACE_NATIVE_DMA : 0);

                qtest_memset(t.qts, RAM_FULL + offsets[i] - 4,
                             0xa5, length + 8);
                o2_storage_inquiry_len(t.qts, pci_addr, length);
                g_assert_cmphex(qtest_readb(t.qts,
                                            o2_storage_aic(0x91)) & 2,
                                ==, 2);
                qtest_memread(t.qts, RAM_FULL + offsets[i] - 4,
                              actual, length + 8);
                for (unsigned b = 0; b < 4; b++) {
                    g_assert_cmphex(actual[b], ==, 0xa5);
                    g_assert_cmphex(actual[4 + length + b], ==, 0xa5);
                }
                g_assert_cmpmem(actual + 4, length, expected, length);
                o2_storage_ack_completion(t.qts);
            }
        }
    }
    events = o2_storage_stop(&t);
    g_assert_nonnull(strstr(events,
                            "\"attempts\":49,\"records\":49,"));
}

static void test_storage_diagnostic_window_ends(void)
{
    O2StorageTest t = o2_storage_start(true);
    g_autofree char *events = NULL;

    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    for (unsigned view = 0; view < 2; view++) {
        qtest_writeb(t.qts, RAM_FULL + GiB - 2, 0xb6);
        qtest_writeb(t.qts, RAM_FULL + GiB - 1, 0xa5);
        o2_storage_inquiry_len(t.qts,
            (view ? MACE_NATIVE_DMA : 0) + GiB - 1, 1);
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2,
                        ==, 2);
        g_assert_cmphex(qtest_readb(t.qts, RAM_NO_ECC + GiB - 2),
                        ==, 0xb6);
        g_assert_cmphex(qtest_readb(t.qts, RAM_NO_ECC + GiB - 1),
                        ==, 0);
        o2_storage_ack_completion(t.qts);
    }
    events = o2_storage_stop(&t);
    g_assert_nonnull(strstr(events,
                            "\"attempts\":2,\"records\":2,"));
}

static void test_storage_diagnostic_bank_crossing(void)
{
    for (unsigned view = 0; view < 2; view++) {
        O2StorageTest t = o2_storage_start(true);
        g_autofree char *events = NULL;

        o2_storage_configure_aic(t.qts, 1);
        o2_program_banks(t.qts);
        qtest_writeb(t.qts, RAM_FULL + RAM_BANK_SIZE + 0x10, 0x6b);
        qtest_writeq(t.qts, CRIME + 0x210, 0x10c);
        o2_storage_inquiry_len(t.qts,
            (view ? MACE_NATIVE_DMA : 0) + RAM_BANK_SIZE - 4, 8);
        g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2,
                        ==, 0);
        g_assert_cmphex(qtest_readb(t.qts,
                                    RAM_FULL + 3 * RAM_BANK_SIZE + 0x10),
                        ==, 0x6b);
        events = o2_storage_stop(&t);
        g_assert_null(strstr(events, "\"result\":0"));
        g_assert_nonnull(strstr(events,
                                "\"attempts\":1,\"records\":1,"));
    }
}

static void test_storage_diagnostic_full_transfer_crossing(void)
{
    static const uint8_t read_cdb[10] = {
        0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0
    };
    static const uint8_t sense_cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    O2StorageTest t;
    g_autofree char *path = NULL;
    g_autofree char *events = NULL;
    g_autofree char *summary = NULL;
    uint8_t last[72], first[456], reference[512], zero[512] = { 0 };
    unsigned attempts;
    int fd = g_file_open_tmp("o2-aic-crossing-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 4096), ==, 0);
    g_assert_cmpint(pwrite(fd, zero, sizeof(zero), 0), ==, sizeof(zero));
    close(fd);
    t = o2_storage_start_accel(true, "qtest", 0, false, path);
    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    o2_storage_command(t.qts, read_cdb, sizeof(read_cdb),
                       0x100, 512, false);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x22)), ==, 2);
    o2_storage_ack_completion(t.qts);
    o2_storage_command(t.qts, sense_cdb, sizeof(sense_cdb),
                       0x200, 18, false);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x22)), ==, 0);
    attempts = o2_storage_assert_dma_span(t.events, 0, 1, 0x200, 18, false);
    o2_storage_ack_completion(t.qts);
    o2_storage_command(t.qts, read_cdb, sizeof(read_cdb),
                       0x1000, sizeof(reference), false);
    o2_storage_wait_completion(t.qts, 1);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x22)), ==, 0);
    attempts = o2_storage_assert_dma_span(t.events, attempts, 1,
                                          0x1000, sizeof(reference), false);
    qtest_memread(t.qts, RAM_FULL + 0x1000, reference, sizeof(reference));
    /* Zero-filled target blocks do not select a native PCI byte order. */
    for (unsigned i = 0; i < sizeof(reference); i++) {
        g_assert_cmphex(reference[i], ==, 0);
    }
    o2_storage_ack_completion(t.qts);
    qtest_memset(t.qts, RAM_FULL + GiB - sizeof(last), 0xa5, sizeof(last));
    qtest_memset(t.qts, RAM_FULL, 0x6b, sizeof(first));
    o2_storage_command(t.qts, read_cdb, sizeof(read_cdb),
                       0x3fffffc0, sizeof(reference), false);
    o2_storage_wait_completion(t.qts, 1);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x22)), ==, 0);
    attempts = o2_storage_assert_dma_span(t.events, attempts, 1,
                                          0x3fffffc0, sizeof(reference), false);
    o2_storage_wait_completion(t.qts, 1);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 2);
    g_assert_cmphex(o2_storage_host_value(t.qts, 0x88, 4), ==, 0x400001c0);
    g_assert_cmphex(o2_storage_host_value(t.qts, 0x8c, 3), ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x94)) & 8, ==, 8);
    qtest_memread(t.qts, RAM_FULL + GiB - sizeof(last), last, sizeof(last));
    qtest_memread(t.qts, RAM_FULL, first, sizeof(first));
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(last[i], ==, 0xa5);
        g_assert_cmphex(first[448 + i], ==, 0x6b);
    }
    g_assert_cmpmem(last + 8, 64, reference, 64);
    g_assert_cmpmem(first, 448, reference + 64, 448);
    events = o2_storage_stop(&t);
    summary = g_strdup_printf("\"attempts\":%u,\"records\":%u,",
                               attempts, attempts);
    g_assert_nonnull(strstr(events, summary));
    unlink(path);
}

static void test_storage_second_segment_dma_failure(void)
{
    static const uint8_t read_cdb[10] = {
        0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0
    };
    static const uint8_t sense_cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    O2StorageTest t = o2_storage_start(true);
    g_autofree char *events = NULL;
    uint8_t first[272];
    uint8_t reference[512];
    const char *fault;
    const char *record;
    const char *end;
    const uint32_t bad_addr = 0x81000100;

    o2_storage_configure_aic(t.qts, 1);
    o2_program_banks(t.qts);
    o2_storage_command(t.qts, read_cdb, sizeof(read_cdb),
                       0x100, 512, false);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x22)), ==, 2);
    o2_storage_ack_completion(t.qts);
    o2_storage_command(t.qts, sense_cdb, sizeof(sense_cdb),
                       0x300, 18, false);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x22)), ==, 0);
    o2_storage_ack_completion(t.qts);
    o2_storage_command(t.qts, read_cdb, sizeof(read_cdb),
                       0x1000, 512, false);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x22)), ==, 0);
    o2_storage_ack_completion(t.qts);
    qtest_memread(t.qts, RAM_FULL + 0x1000, reference,
                  sizeof(reference));

    qtest_memset(t.qts, RAM_FULL + 0xf8, 0xa5, sizeof(first));
    o2_storage_command(t.qts, read_cdb, sizeof(read_cdb),
                       0x100, 256, false);
    for (unsigned i = 0; i < 1000; i++) {
        if (!qtest_readb(t.qts, o2_storage_aic(0x8c)) &&
            !qtest_readb(t.qts, o2_storage_aic(0x8d))) {
            break;
        }
        qtest_clock_step(t.qts, 1000);
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x8c)), ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x8d)), ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 0);
    qtest_memread(t.qts, RAM_FULL + 0xf8, first, sizeof(first));
    g_assert_cmpmem(first + 8, 256, reference, 256);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(first[i], ==, 0xa5);
        g_assert_cmphex(first[264 + i], ==, 0xa5);
    }

    qtest_writeb(t.qts, o2_storage_aic(0x93), 0x30);
    for (unsigned i = 0; i < 4; i++) {
        qtest_writeb(t.qts, o2_storage_aic(0x88 + i),
                     bad_addr >> (i * 8));
    }
    qtest_writeb(t.qts, o2_storage_aic(0x8c), 0);
    qtest_writeb(t.qts, o2_storage_aic(0x8d), 1);
    qtest_writeb(t.qts, o2_storage_aic(0x8e), 0);
    qtest_writeb(t.qts, o2_storage_aic(0x08), 0);
    qtest_writeb(t.qts, o2_storage_aic(0x09), 1);
    qtest_writeb(t.qts, o2_storage_aic(0x0a), 0);
    qtest_writeb(t.qts, o2_storage_aic(0x93), 0x38);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x92)) & 0x40,
                    ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x87)) & 4, ==, 4);
    o2_storage_assert_diagnostic_stop(t.qts, 1);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x91)) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x9e)), ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x94)) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(t.qts, o2_storage_aic(0x8d)), ==, 1);
    qtest_memread(t.qts, RAM_FULL + 0xf8, first, sizeof(first));
    g_assert_cmpmem(first + 8, 256, reference, 256);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(first[i], ==, 0xa5);
        g_assert_cmphex(first[264 + i], ==, 0xa5);
    }
    events = o2_storage_stop(&t);
    fault = strstr(events, "\"pci_address\":\"0x81000100\"");
    g_assert_nonnull(fault);
    record = fault;
    while (record > events && record[-1] != '\n') {
        record--;
    }
    end = strchr(fault, '\n');
    g_assert_nonnull(end);
    g_assert_nonnull(g_strstr_len(record, end - record,
                                  "\"requester\":\"00:01.0\""));
    g_assert_nonnull(g_strstr_len(record, end - record,
                                  "\"direction\":\"to-memory\""));
    g_assert_null(g_strstr_len(record, end - record, "\"result\":0"));
}

static void test_storage_diagnostic_rejections(void)
{
    const char *mode = "o2-test,diagnostic-lower-pci-byte-identity=on";
    const uint8_t rom_bytes[] = { 0, 0, 0, 0 };
    const char *direct_mace[] = {
        qtest_qemu_binary(NULL), "-M", "none", "-m", "1G",
        "-accel", "qtest", "-nodefaults", "-display", "none",
        "-qmp", "stdio", "-device", "sgi-mace,o2-storage-diagnostic=on",
        NULL,
    };
    g_autofree char *dir = g_dir_make_tmp("o2-storage-reject-XXXXXX", NULL);
    g_autofree char *rom = NULL;
    g_autofree char *events = NULL;
    g_autofree char *full_mode = NULL;
    g_autofree char *no_sink_mode = NULL;
    g_autofree char *with_aic_fixture = NULL;
    g_autofree char *with_dma_fixture = NULL;
    QTestState *qts = o2_start_banked();
    uint8_t byte;

    g_assert_nonnull(dir);
    rom = g_build_filename(dir, "synthetic-rom", NULL);
    events = g_build_filename(dir, "dma.jsonl", NULL);
    g_assert_true(g_file_set_contents(rom, (const char *)rom_bytes,
                                      sizeof(rom_bytes), NULL));
    no_sink_mode = g_strdup_printf(
        "o2-test,bank-profile=8x128m-synthetic,"
        "reset-low-ram-approx=on,empty-renderer-status=on,"
        "prom-private-window=on,prom-sb64-reset=on,"
        "gbe11-registers=on,prom-id-word=on,"
        "password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0,rom-pad-byte=238,"
        "diagnostic-lower-pci-byte-identity=on", rom);
    full_mode = g_strdup_printf("%s,storage-diagnostic-event-file=%s",
                                no_sink_mode, events);
    with_aic_fixture = g_strdup_printf("%s,aic-pci-diagnostic=on",
                                       full_mode);
    with_dma_fixture = g_strdup_printf("%s,mace-bank-dma-fixture=on",
                                       full_mode);
    g_assert_false(qtest_qom_get_bool(qts, "/machine",
        "diagnostic-lower-pci-byte-identity"));
    g_assert_cmpint(memtx_readb(qts, AIC_PCI_VIEW, &byte), !=, MEMTX_OK);
    qtest_quit(qts);
    assert_o2_banked_rejected(mode, "1G", "-accel", "qtest",
                              "requires the complete 1 GiB");
    assert_o2_banked_rejected(no_sink_mode, "1G", "-accel", "qtest",
                              "requires an absolute storage-diagnostic-event-file");
    assert_o2_banked_rejected(with_aic_fixture, "1G", "-accel", "qtest",
                              "no other PCI diagnostic");
    assert_o2_banked_rejected(with_dma_fixture, "1G", "-accel", "qtest",
                              "no other PCI diagnostic");
    assert_launch_rejected(direct_mace,
                           "requires the O2 decoded RAM view");
    g_assert_true(g_file_set_contents(events, "old", 3, NULL));
    assert_o2_banked_rejected(full_mode, "1G", "-accel", "qtest",
                              "cannot create storage diagnostic event file");
    {
        g_autofree char *unchanged = NULL;

        g_assert_true(g_file_get_contents(events, &unchanged, NULL, NULL));
        g_assert_cmpstr(unchanged, ==, "old");
    }
    g_assert_cmpint(g_unlink(events), ==, 0);
    {
        const char *two_targets[] = {
            qtest_qemu_binary(NULL), "-M", full_mode,
            "-cpu", "R10000", "-m", "1G", "-accel", "qtest",
            "-nodefaults", "-display", "none", "-qmp", "stdio",
            "-drive", "if=none,id=disk0,file=null-co://,format=raw",
            "-drive", "if=none,id=disk1,file=null-co://,format=raw",
            "-device", "scsi-cd,drive=disk0,bus=aic0.0,scsi-id=0,lun=0",
            "-device", "scsi-hd,drive=disk1,bus=aic1.0,scsi-id=1,lun=0",
            NULL,
        };

        assert_launch_rejected(two_targets,
                               "accepts one SCSI CD or disk");
    }
    g_assert_cmpint(g_unlink(events), ==, 0);
    {
        const char *wide_target[] = {
            qtest_qemu_binary(NULL), "-M", full_mode,
            "-cpu", "R10000", "-m", "1G", "-accel", "qtest",
            "-nodefaults", "-display", "none", "-qmp", "stdio",
            "-drive", "if=none,id=disk0,file=null-co://,format=raw",
            "-device", "scsi-cd,drive=disk0,bus=aic0.0,scsi-id=8,lun=0",
            NULL,
        };

        assert_launch_rejected(wide_target,
                               "accepts one SCSI CD or disk");
    }
    g_assert_cmpint(g_unlink(events), ==, 0);
    {
        const char *other_lun[] = {
            qtest_qemu_binary(NULL), "-M", full_mode,
            "-cpu", "R10000", "-m", "1G", "-accel", "qtest",
            "-nodefaults", "-display", "none", "-qmp", "stdio",
            "-drive", "if=none,id=disk0,file=null-co://,format=raw",
            "-device", "scsi-hd,drive=disk0,bus=aic0.0,scsi-id=0,lun=1",
            NULL,
        };

        assert_launch_rejected(other_lun,
                               "accepts one SCSI CD or disk");
    }
    g_assert_cmpint(g_unlink(events), ==, 0);
    g_assert_cmpint(g_unlink(rom), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/o2/mace-pci-functional/unused-aic-config",
                   test_mace_pci_unused_aic_config);
    qtest_add_func("/o2/mace-pci-functional/fresh-reads",
                   test_mace_pci_fresh_reads);
    qtest_add_func("/o2/ram-aliases", test_ram_aliases);
    qtest_add_func("/o2/banked-registers", test_banked_registers);
    qtest_add_func("/o2/banked-approx-reset-ram",
                   test_banked_approx_reset_ram);
    qtest_add_func("/o2/empty-renderer-status",
                   test_empty_renderer_status);
    qtest_add_func("/o2/banked-approx-first-write",
                   test_banked_approx_first_write);
    qtest_add_func("/o2/banked-approx-bank-one-cpu-error",
                   test_banked_approx_bank_one_cpu_error);
    qtest_add_func("/o2/banked-approx-uncached-ram",
                   test_banked_approx_uncached_ram);
    qtest_add_func("/o2/banked-aliases", test_banked_aliases);
    qtest_add_func("/o2/banked-remap", test_banked_remap);
    qtest_add_func("/o2/banked-reset", test_banked_reset);
    qtest_add_func("/o2/banked-migration", test_banked_migration);
    qtest_add_func("/o2/banked-incoming-migration",
                   test_banked_incoming_migration);
    qtest_add_func("/o2/mace-ram-link", test_mace_ram_link);
    qtest_add_func("/o2/ram-128m-aliases", test_ram_128m_aliases);
    qtest_add_func("/o2/reset-registers", test_reset_registers);
    qtest_add_func("/o2/interrupt-chain", test_interrupt_chain);
    qtest_add_func("/o2/banked-interrupt-chain",
                   test_banked_interrupt_chain);
    qtest_add_func("/o2/rom-aliases", test_rom_aliases);
    qtest_add_func("/o2/banked-approx-rom-aliases",
                   test_banked_approx_rom_aliases);
    qtest_add_func("/o2/banked-approx-rom-first-fetch",
                   test_banked_approx_rom_first_fetch);
    qtest_add_func("/o2/cpu-reset-restarts-rom",
                   test_cpu_reset_restarts_rom);
    qtest_add_func("/o2/private-window-backing-isolation",
                   test_private_window_backing_isolation);
    qtest_add_func("/o2/private-window-boundary",
                   test_private_window_boundary);
    qtest_add_func("/o2/private-window-backing-remap",
                   test_private_window_backing_remap);
    qtest_add_func("/o2/private-window-other-cache-algorithms",
                   test_private_window_other_cache_algorithms);
    qtest_add_func("/o2/private-window-missed-rom-trigger",
                   test_private_window_missed_rom_trigger);
    qtest_add_func("/o2/private-window-missed-rom-poll",
                   test_private_window_missed_rom_poll);
    qtest_add_func("/o2/private-window-low-fetch-report",
                   test_private_window_low_fetch_report);
    qtest_add_func("/o2/private-window-uncached-low-fetch-release",
                   test_private_window_uncached_low_fetch_release);
    qtest_add_func("/o2/private-window-guest-copy-release",
                   test_private_window_guest_copy_release);
    qtest_add_func("/o2/private-window-incomplete-copy",
                   test_private_window_incomplete_copy);
    qtest_add_func("/o2/private-window-high-sp-release",
                   test_private_window_high_sp_release);
    qtest_add_func("/o2/coherent-ram", test_coherent_ram);
    qtest_add_func("/o2/uncached-window", test_uncached_window);
    qtest_add_func("/o2/uncached-mmio", test_uncached_mmio);
    qtest_add_func("/o2/uncached-128m-boundary",
                   test_uncached_128m_boundary);
    qtest_add_func("/o2/banked-uncached-window",
                   test_banked_uncached_window);
    qtest_add_func("/o2/banked-approx-uncached-remap",
                   test_banked_approx_uncached_remap);
    qtest_add_func("/o2/module-profile", test_module_profile);
    qtest_add_func("/o2/prom-sb64-reset", test_prom_sb64_reset);
    qtest_add_func("/o2/gbe11-registers", test_gbe11_registers);
    qtest_add_func("/o2/gbe-external-ttl/default",
                   test_gbe_external_ttl_default);
    qtest_add_func("/o2/gbe-external-ttl/board", test_gbe_external_ttl_board);
    qtest_add_func("/o2/gbe-external-ttl/configuration",
                   test_gbe_external_ttl_configuration);
    qtest_add_func("/o2/gbe-external-ttl/migration",
                   test_gbe_external_ttl_migration);
    qtest_add_func("/o2/aic-pci-diagnostic", test_aic_pci_diagnostic);
    qtest_add_func("/o2/aic-pci-low-dma-policy-off",
                   test_aic_pci_low_dma_policy_off);
    qtest_add_func("/o2/mace-low-dma-option-scope",
                   test_mace_low_dma_option_scope);
    qtest_add_func("/o2/aic-pci-diagnostic-rejections",
                   test_aic_pci_diagnostic_rejections);
    qtest_add_func("/o2/mace-native-bytes", test_mace_native_bytes);
    qtest_add_func("/o2/mace-native-failure-prefix",
                   test_mace_native_failure_prefix);
    qtest_add_func("/o2/mace-banked-dma-fixture",
                   test_mace_banked_dma_fixture);
    qtest_add_func("/o2/mace-banked-dma-fixture-lifetime",
                   test_mace_banked_dma_fixture_lifetime);
    qtest_add_func("/o2/mace-banked-dma-fixture-rejections",
                   test_mace_banked_dma_fixture_rejections);
    qtest_add_func("/o2/storage-diagnostic-apertures",
                   test_storage_diagnostic_apertures);
    qtest_add_func("/o2/storage-pio-byte-lanes",
                   test_storage_pio_byte_lanes);
    qtest_add_func("/o2/storage-pio-aic-hcntrl",
                   test_storage_pio_aic_hcntrl);
    qtest_add_func("/o2/storage-diagnostic-pci-parity-readback",
                   test_storage_diagnostic_pci_parity_readback);
    qtest_add_func("/o2/storage-diagnostic-tcg-inventory",
                   test_storage_diagnostic_tcg_inventory);
    qtest_add_func("/o2/storage-host-dfdat-pci-route",
                   test_storage_host_dfdat_pci_route);
    qtest_add_func("/o2/storage-host-offset-four",
                   test_storage_host_offset_four);
    qtest_add_func("/o2/storage-host-offset-four-write",
                   test_storage_host_offset_four_write);
    qtest_add_func("/o2/storage-host-offset-four-refusals",
                   test_storage_host_offset_four_refusals);
    qtest_add_func("/o2/storage-host-offset-four-disabled",
                   test_storage_host_offset_four_disabled);
    qtest_add_func("/o2/storage-diagnostic-dma-events",
                   test_storage_diagnostic_dma_events);
    qtest_add_func("/o2/low-ethernet-dma-preserves-aic-pci",
                   test_low_ethernet_dma_preserves_aic_pci);
    qtest_add_func("/o2/default-aic-64m", test_default_aic_64m);
    qtest_add_func("/o2/default-aic-128m", test_default_aic_128m);
    qtest_add_func("/o2/default-aic-full-1g", test_default_aic_full_1g);
    qtest_add_func("/o2/default-aic-low-dma-28-bytes",
                   test_default_aic_low_dma_28_bytes);
    qtest_add_func("/o2/default-aic-low-dma-write",
                   test_default_aic_low_dma_write);
    qtest_add_func("/o2/full-identity-ethernet-aic",
                   test_full_identity_ethernet_aic);
    qtest_add_func("/o2/storage-queue-before-completion-interrupt",
                   test_storage_queue_before_completion_interrupt);
    qtest_add_func("/o2/storage-high-id-selection-timeout",
                   test_storage_high_id_selection_timeout);
    qtest_add_func("/o2/storage-second-controller-dma-irq",
                   test_storage_second_controller_dma_irq);
    qtest_add_func("/o2/storage-second-controller-dma-irq-tcg",
                   test_storage_second_controller_dma_irq_tcg);
    qtest_add_func("/o2/storage-queued-metadata-fetch",
                   test_storage_queued_metadata_fetch);
    qtest_add_func("/o2/storage-completion-window-before-irq",
                   test_storage_completion_window_before_irq);
    qtest_add_func("/o2/storage-completion-window-failure",
                   test_storage_completion_window_failure);
    qtest_add_func("/o2/storage-queued-inquiry-to-completion",
                   test_storage_queued_inquiry_to_completion);
    qtest_add_func("/o2/storage-queued-read-to-completion",
                   test_storage_queued_read_to_completion);
    qtest_add_func("/o2/storage-queued-write-to-completion",
                   test_storage_queued_write_to_completion);
    qtest_add_func("/o2/storage-queued-read-second-controller",
                   test_storage_queued_read_second_controller);
    qtest_add_func("/o2/storage-queued-write-second-controller",
                   test_storage_queued_write_second_controller);
    qtest_add_func("/o2/storage-queued-second-dma-failure",
                   test_storage_queued_second_dma_failure);
    qtest_add_func("/o2/storage-queued-check-condition",
                   test_storage_queued_check_condition);
    qtest_add_func("/o2/storage-queued-scsi-reset",
                   test_storage_queued_scsi_reset);
    qtest_add_func("/o2/storage-queued-chip-reset",
                   test_storage_queued_chip_reset);
    qtest_add_func("/o2/storage-queued-pci-reset",
                   test_storage_queued_pci_reset);
    qtest_add_func("/o2/storage-diagnostic-event-loss",
                   test_storage_diagnostic_event_loss);
    qtest_add_func("/o2/storage-diagnostic-target-unplug",
                   test_storage_diagnostic_target_unplug);
    qtest_add_func("/o2/storage-diagnostic-target-qom-unrealize",
                   test_storage_diagnostic_target_qom_unrealize);
    qtest_add_func("/o2/storage-diagnostic-final-sync-loss",
                   test_storage_diagnostic_final_sync_loss);
    qtest_add_func("/o2/storage-diagnostic-master-gate",
                   test_storage_diagnostic_master_gate);
    qtest_add_func("/o2/storage-diagnostic-dma-bounds",
                   test_storage_diagnostic_dma_bounds);
    qtest_add_func("/o2/storage-diagnostic-reverse-dma",
                   test_storage_diagnostic_reverse_dma);
    qtest_add_func("/o2/storage-diagnostic-byte-widths",
                   test_storage_diagnostic_byte_widths);
    qtest_add_func("/o2/storage-diagnostic-window-ends",
                   test_storage_diagnostic_window_ends);
    qtest_add_func("/o2/storage-diagnostic-bank-crossing",
                   test_storage_diagnostic_bank_crossing);
    qtest_add_func("/o2/storage-diagnostic-full-transfer-crossing",
                   test_storage_diagnostic_full_transfer_crossing);
    qtest_add_func("/o2/storage-second-segment-dma-failure",
                   test_storage_second_segment_dma_failure);
    qtest_add_func("/o2/storage-diagnostic-rejections",
                   test_storage_diagnostic_rejections);
    qtest_add_func("/o2/gbe-one-tile-retired", test_gbe_one_tile_retired);
    qtest_add_func("/o2/prom-id-word", test_prom_id_word);
    qtest_add_func("/o2/prom-id-word-configuration",
                   test_prom_id_word_configuration);
    qtest_add_func("/o2/gbe11-registers-configuration",
                   test_gbe11_registers_configuration);
    qtest_add_func("/o2/serial-a", test_serial_a);
    qtest_add_func("/o2/configuration", test_configuration);
    qtest_add_func("/o2/banked-configuration", test_banked_configuration);
    qtest_add_func("/o2/banked-approx-configuration",
                   test_banked_approx_configuration);
    qtest_add_func("/o2/empty-renderer-configuration",
                   test_empty_renderer_configuration);
    qtest_add_func("/o2/functional-transfer-configuration",
                   test_functional_transfer_configuration);
    qtest_add_func("/o2/private-window-configuration",
                   test_private_window_configuration);
    qtest_add_func("/o2/prom-sb64-reset-configuration",
                   test_prom_sb64_reset_configuration);
    qtest_add_func("/o2/banked-approx-rom-rejected",
                   test_banked_approx_rom_rejected);
    qtest_add_func("/o2/banked-profile-immutable",
                   test_banked_profile_immutable);
    qtest_add_func("/o2/banked-rom-immutable", test_banked_rom_immutable);
    qtest_add_func("/o2/banked-approx-immutable",
                   test_banked_approx_immutable);
    qtest_add_func("/o2/banked-approx-child-immutable",
                   test_banked_approx_child_immutable);
    qtest_add_func("/o2/banked-approx-migration",
                   test_banked_approx_migration);
    return g_test_run();
}
