/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Register tests for a standalone AIC-7880 PCI function. */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include <gio/gio.h>
#include "libqtest.h"
#include "memtx.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"
#include "qapi/error.h"
#include "sgi-migrate.h"

#define PCI_DEVFN       0x20
#define PCI_CONFIG_ADDR 0xcf8
#define PCI_CONFIG_DATA 0xcfc
#define REG_BASE        0xfeb00000u

static uint32_t config_readl(QTestState *qts, unsigned int offset)
{
    qtest_outl(qts, PCI_CONFIG_ADDR,
               0x80000000u | (PCI_DEVFN << 8) | offset);
    return qtest_inl(qts, PCI_CONFIG_DATA);
}

static void config_writel(QTestState *qts, unsigned int offset, uint32_t value)
{
    qtest_outl(qts, PCI_CONFIG_ADDR,
               0x80000000u | (PCI_DEVFN << 8) | offset);
    qtest_outl(qts, PCI_CONFIG_DATA, value);
}

static void config_writew(QTestState *qts, unsigned int offset, uint16_t value)
{
    qtest_outl(qts, PCI_CONFIG_ADDR,
               0x80000000u | (PCI_DEVFN << 8) | offset);
    qtest_outw(qts, PCI_CONFIG_DATA + (offset & 2), value);
}

static void wait_register(QTestState *qts, unsigned reg, uint8_t mask,
                           uint8_t expected)
{
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

    qtest_clock_timers(qts, true);
    do {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        qtest_clock_step(qts, 1);
    } while ((qtest_readb(qts, REG_BASE + reg) & mask) != expected);
}

static void wait_paused(QTestState *qts)
{
    wait_register(qts, 0x87, 4, 4);
}

static int64_t resume_program(QTestState *qts, uint8_t control)
{
    qtest_clock_timers(qts, true);
    qtest_writeb(qts, REG_BASE + 0x87, control);
    /* A positive step dispatches work already due, then advances one ns. */
    return qtest_clock_step(qts, 1) - 1;
}

static void step_instruction(QTestState *qts)
{
    qtest_writeb(qts, REG_BASE + 0x87, 2);
    wait_paused(qts);
}

static QTestState *start_aic_with_options(const char *options)
{
    QTestState *qts = qtest_initf("-M pc -m 128M -nodefaults %s "
                                  "-device aic7880,id=hba,bus=pci.0,addr=04.0",
                                  options);

    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    return qts;
}

static QTestState *start_aic(void)
{
    return start_aic_with_options("");
}

static QTestState *start_clocked_aic(void)
{
    return start_aic_with_options("-global aic7880.x-sequencer-clocked=on");
}

static QTestState *start_external_scb(unsigned size, unsigned width,
                                      const char *extra)
{
    QTestState *qts = qtest_initf(
        "-M pc -m 128M -nodefaults %s "
        "-device aic7880,id=hba,bus=pci.0,addr=04.0,"
        "external-scb-ram=on,external-scb-size=%u,external-scb-width=%u",
        extra, size, width);

    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 6);
    return qts;
}

static void external_scb_write(QTestState *qts, uint8_t page, uint8_t offset,
                                uint8_t value)
{
    qtest_writeb(qts, REG_BASE + 0x90, page);
    qtest_writeb(qts, REG_BASE + 0xa0 + offset, value);
}

static uint8_t external_scb_read(QTestState *qts, uint8_t page, uint8_t offset)
{
    qtest_writeb(qts, REG_BASE + 0x90, page);
    return qtest_readb(qts, REG_BASE + 0xa0 + offset);
}

static void test_external_scb_capacity(void)
{
    static const unsigned sizes[] = { 1024, 2048, 4096, 8192 };

    for (unsigned i = 0; i < ARRAY_SIZE(sizes); i++) {
        unsigned pages = sizes[i] / 32;
        QTestState *qts = start_external_scb(sizes[i], 8, "");

        g_assert_cmphex(config_readl(qts, 0x40) & 0x600, ==, 0x600);
        qtest_writeb(qts, REG_BASE + 0x90, 0xff);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x90), ==, 0xff);
        external_scb_write(qts, 0, 0, 0x11);
        external_scb_write(qts, pages - 1, 0, 0x22);
        g_assert_cmphex(external_scb_read(qts, 0, 0), ==, 0x11);
        g_assert_cmphex(external_scb_read(qts, pages - 1, 0), ==, 0x22);
        if (pages < 256) {
            external_scb_write(qts, pages, 0, 0x33);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x90), ==, pages);
            g_assert_cmphex(external_scb_read(qts, 0, 0), ==, 0x33);
            g_assert_cmphex(external_scb_read(qts, pages, 0), ==, 0x33);
            g_assert_cmphex(external_scb_read(qts, pages - 1, 0), ==, 0x22);
        } else {
            external_scb_write(qts, 128, 0, 0x44);
            g_assert_cmphex(external_scb_read(qts, 0, 0), ==, 0x11);
            g_assert_cmphex(external_scb_read(qts, 128, 0), ==, 0x44);
            g_assert_cmphex(external_scb_read(qts, 255, 0), ==, 0x22);
        }
        config_writel(qts, 0x40, 0);
        g_assert_cmphex(config_readl(qts, 0x40) & 0x600, ==, 0x600);
        qtest_writeb(qts, REG_BASE + 0x87, 1);
        g_assert_cmphex(config_readl(qts, 0x40) & 0x600, ==, 0x600);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x90), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9a), ==, 0);
        external_scb_write(qts, 0, 0, 0x5a);
        g_assert_cmphex(external_scb_read(qts, 0, 0), ==, 0x5a);
        qtest_system_reset(qts);
        config_writel(qts, 0x14, REG_BASE);
        config_writew(qts, 4, 6);
        g_assert_cmphex(config_readl(qts, 0x40) & 0x600, ==, 0x600);
        external_scb_write(qts, 0, 0, 0xa5);
        g_assert_cmphex(external_scb_read(qts, 0, 0), ==, 0xa5);
        qtest_quit(qts);
    }
}

static QTestState *start_aic_logged(const char *log_path)
{
    QTestState *qts = qtest_initf(
        "-M pc -m 128M -nodefaults -d unimp,guest_errors -D %s "
        "-device aic7880,id=hba,bus=pci.0,addr=04.0", log_path);

    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    return qts;
}

static void assert_no_aic_log(const char *log_path)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log = NULL;

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_null(strstr(log, "aic7880:"));
}

static void test_identity(void)
{
    QTestState *qts = start_aic();

    g_assert_cmphex(config_readl(qts, 0x00), ==, 0x80789004u);
    g_assert_cmphex(config_readl(qts, 0x08) & 0xffffff00u, ==,
                    0x01000000u);
    config_writel(qts, 0x10, 0xc001);
    config_writew(qts, 0x04, 0x0007);
    qtest_outb(qts, 0xc020, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
    qtest_writeb(qts, REG_BASE + 0x21, 0xa5);
    g_assert_cmphex(qtest_inb(qts, 0xc021), ==, 0xa5);
    qtest_quit(qts);
}

static void test_host_dfdat_byte_port(void)
{
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x93, 0x01);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 1, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x99, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
    qtest_quit(qts);
}

static void host_dma_program(QTestState *qts, uint32_t addr, uint32_t count)
{
    for (unsigned i = 0; i < 4; i++) {
        qtest_writeb(qts, REG_BASE + 0x88 + i, addr >> (8 * i));
    }
    for (unsigned i = 0; i < 3; i++) {
        qtest_writeb(qts, REG_BASE + 0x8c + i, count >> (8 * i));
    }
}

static uint32_t host_dma_read_bytes(QTestState *qts, unsigned reg, unsigned n)
{
    uint32_t value = 0;

    for (unsigned i = 0; i < n; i++) {
        value |= (uint32_t)qtest_readb(qts, REG_BASE + reg + i) << (8 * i);
    }
    return value;
}

static void test_host_address_loads_scsi_address(void)
{
    static const uint8_t address[] = { 0x89, 0xab, 0xcd, 0x12 };
    static const uint8_t bytes[] = { 0x19, 0x36, 0x5b, 0x7e,
                                     0xa2, 0xc5, 0xe8, 0x0d };
    uint8_t guarded[24], got[24];
    QTestState *qts = start_aic();

    for (unsigned i = 0; i < sizeof(address); i++) {
        qtest_writeb(qts, REG_BASE + 0x88 + i, address[i]);
    }
    for (unsigned i = 0; i < sizeof(address); i++) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x14 + i), ==,
                        address[i]);
    }
    qtest_writeb(qts, REG_BASE + 0x14, 0x42);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x88), ==, address[0]);

    memset(guarded, 0xa5, sizeof(guarded));
    memcpy(guarded + 8, bytes, sizeof(bytes));
    qtest_memwrite(qts, 0x10000, guarded, sizeof(guarded));
    qtest_writeb(qts, REG_BASE + 0x08, 7);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 0x38, ==, 0);
    host_dma_program(qts, 0x10008, sizeof(bytes));
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x10008);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x10008);
    qtest_writeb(qts, REG_BASE + 0x93, 0x04);
    qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x10010);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x10008);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 7);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_memread(qts, 0x10000, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), guarded, sizeof(guarded));
    qtest_quit(qts);
}

static void test_reserved_host_count_byte(void)
{
    const uint8_t source = 0x5a;
    QTestState *qts = start_aic();
    uint32_t address, count;
    uint8_t status;

    config_writel(qts, 0x10, 0xc001);
    config_writew(qts, 0x04, 0x0007);
    host_dma_program(qts, 0x10100, 0x123456);
    address = host_dma_read_bytes(qts, 0x88, 4);
    count = host_dma_read_bytes(qts, 0x8c, 3);
    status = qtest_readb(qts, REG_BASE + 0x94);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8f), ==, 0);
    g_assert_cmphex(qtest_inb(qts, 0xc08f), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x8f, 0xa5);
    qtest_outb(qts, 0xc08f, 0xff);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8f), ==, 0);
    g_assert_cmphex(qtest_inb(qts, 0xc08f), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, address);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, count);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94), ==, status);

    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8f), ==, 0);
    qtest_system_reset(qts);
    config_writel(qts, 0x14, REG_BASE);
    config_writel(qts, 0x10, 0xc001);
    config_writew(qts, 0x04, 0x0007);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8f), ==, 0);
    g_assert_cmphex(qtest_inb(qts, 0xc08f), ==, 0);

    qtest_memwrite(qts, 0x10000, &source, 1);
    host_dma_program(qts, 0x10000, 1);
    qtest_writeb(qts, REG_BASE + 0x93, 0x05);
    qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 8);
    qtest_writeb(qts, REG_BASE + 0x93, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 8, ==, 0);
    address = host_dma_read_bytes(qts, 0x88, 4);
    count = host_dma_read_bytes(qts, 0x8c, 3);
    status = qtest_readb(qts, REG_BASE + 0x94);
    qtest_writeb(qts, REG_BASE + 0x8f, 0xff);
    qtest_outb(qts, 0xc08f, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8f), ==, 0);
    g_assert_cmphex(qtest_inb(qts, 0xc08f), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, address);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, count);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94), ==, status);
    qtest_quit(qts);
}

static void test_host_dma_memory_to_fifo(void)
{
    uint8_t guarded[272], got[272];
    QTestState *qts = start_aic();

    memset(guarded, 0xa5, sizeof(guarded));
    for (unsigned i = 0; i < 256; i++) {
        guarded[i + 8] = i * 37 + 11;
    }
    qtest_memwrite(qts, 0x10000, guarded, sizeof(guarded));
    host_dma_program(qts, 0x10008, 256);
    qtest_writeb(qts, REG_BASE + 0x93, 0x05);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x10108);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x0b, ==, 0x0a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 8, ==, 0);
    for (unsigned i = 0; i < 256; i++) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==, guarded[i + 8]);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 3, ==, 1);
    qtest_memread(qts, 0x10000, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), guarded, sizeof(guarded));
    qtest_quit(qts);
}

static void test_host_dma_four_byte_offset(void)
{
    uint8_t guarded[32], got[32];
    const uint32_t base = 0x18000;
    const unsigned offset = 12;
    QTestState *qts = start_aic();

    for (unsigned pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < sizeof(guarded); i++) {
            guarded[i] = 17 + i * 29 + pass * 43;
        }
        qtest_writeb(qts, REG_BASE + 0x93, 0x04);
        qtest_memwrite(qts, base + pass * 64, guarded, sizeof(guarded));
        qtest_writeb(qts, REG_BASE + 0x08, 7);
        host_dma_program(qts, base + pass * 64 + offset, 4);
        qtest_writeb(qts, REG_BASE + 0x93, 0x05);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 0x38, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
        qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==,
                        base + pass * 64 + offset + 4);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x2b, ==, 0x28);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==,
                        base + pass * 64 + offset);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 7);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        qtest_writeb(qts, REG_BASE + 0x93, 0x04);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 0x38, ==, 0);
        for (unsigned i = 0; i < 4; i++) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==,
                            guarded[offset + i]);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x2b, ==, 0x29);
        qtest_memread(qts, base + pass * 64, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), guarded, sizeof(guarded));
    }
    qtest_quit(qts);
}

static void test_host_dma_offset_four_28(void)
{
    uint8_t guarded[52], got[52];
    QTestState *qts = start_aic();

    for (unsigned pass = 0; pass < 2; pass++) {
        uint32_t address = 0x00248004 + pass * 128;

        for (unsigned i = 0; i < sizeof(guarded); i++) {
            guarded[i] = 11 + i * 37 + pass * 53;
        }
        qtest_writeb(qts, REG_BASE + 0x93, 4);
        qtest_memwrite(qts, address - 12, guarded, sizeof(guarded));
        qtest_writeb(qts, REG_BASE + 0x08, 0x37);
        host_dma_program(qts, address, 28);
        qtest_writeb(qts, REG_BASE + 0x93, 5);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 0x38, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
        qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, address + 28);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, address);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0x37);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x2b, ==, 8);
        qtest_writeb(qts, REG_BASE + 0x93, 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 0x38, ==, 0);
        for (unsigned i = 0; i < 28; i++) {
            if (i == 0 || i == 4 || i == 12 || i == 20) {
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x23,
                                ==, 0);
            }
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==,
                            guarded[12 + i]);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x2b, ==, 0x29);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        qtest_memread(qts, address - 12, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), guarded, sizeof(guarded));
    }
    qtest_quit(qts);
}

static void test_host_fifo_partial_words(void)
{
    QTestState *qts = start_aic();

    for (unsigned count = 8; count <= 13; count += 5) {
        host_dma_program(qts, 0x11004, 0);
        qtest_writeb(qts, REG_BASE + 0x93, 5);
        for (unsigned i = 0; i < count; i++) {
            qtest_writeb(qts, REG_BASE + 0x99, 0x19 + i * 23);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x23, ==,
                        count == 8 ? 0x20 : 0);
        for (unsigned i = 0; i < count; i++) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==,
                            (uint8_t)(0x19 + i * 23));
            if (count == 13 && i == 11) {
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x23,
                                ==, 0x20);
            }
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x23, ==, 0x21);
    }
    qtest_quit(qts);
}

static void check_host_fifo_status(QTestState *qts, unsigned read,
                                    unsigned valid, bool done)
{
    bool occupied[256] = { false };
    bool complete = false;
    uint8_t expected = done ? 8 : 0;

    for (unsigned i = 0; i < valid; i++) {
        occupied[(read + i) & 255] = true;
    }
    for (unsigned word = 0; word < 32; word++) {
        bool full = true;

        for (unsigned byte = 0; byte < 8; byte++) {
            full &= occupied[word * 8 + byte];
        }
        complete |= full;
    }
    if (!complete) {
        expected |= 0x20;
    }
    if (!valid) {
        expected |= 1;
    }
    if (valid == 256) {
        expected |= 2;
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x2b, ==, expected);
}

static void check_host_dma_read_stream(QTestState *qts, unsigned offset,
                                       unsigned count)
{
    uint32_t address = 0x310000 + offset;
    g_autofree uint8_t *guarded = g_malloc(count + 24);
    g_autofree uint8_t *got = g_malloc(count + 24);
    unsigned consumed = 0;

    for (unsigned i = 0; i < count + 24; i++) {
        guarded[i] = (11 + i * 37) ^ (i >> 8) ^ (i >> 16);
    }
    qtest_writeb(qts, REG_BASE + 0x93, 4);
    qtest_memwrite(qts, address - 12, guarded, count + 24);
    qtest_writeb(qts, REG_BASE + 0x08, 0x37);
    host_dma_program(qts, address, count);
    qtest_writeb(qts, REG_BASE + 0x93, 5);
    while (consumed < count) {
        unsigned valid = MIN(count - consumed, 256);
        unsigned accepted = consumed + valid;
        bool done = accepted == count;

        qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==,
                        address + accepted);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==,
                        count - accepted);
        check_host_fifo_status(qts, (offset + consumed) & 255, valid, done);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x10, ==, 0);
        qtest_writeb(qts, REG_BASE + 0x93, 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 0x38, ==, 0);
        for (unsigned i = 0; i < valid; i++) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==,
                            guarded[12 + consumed + i]);
            if (count <= 32 || ((offset + consumed + i + 1) & 7) == 0 ||
                i + 1 == valid) {
                check_host_fifo_status(qts, (offset + consumed + i + 1) & 255,
                                       valid - i - 1, done);
            }
        }
        consumed += valid;
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==,
                        address + consumed);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, address);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0x37);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 0);
    }
    qtest_memread(qts, address - 12, got, count + 24);
    g_assert_cmpmem(got, count + 24, guarded, count + 24);
}

static void test_host_dma_read_offsets(void)
{
    QTestState *qts = start_aic();

    for (unsigned offset = 0; offset < 8; offset++) {
        unsigned counts[] = { 1, 8 - offset, 9 - offset, 28 };

        for (unsigned i = 0; i < ARRAY_SIZE(counts); i++) {
            check_host_dma_read_stream(qts, offset, counts[i]);
        }
    }
    for (unsigned count = 4; count <= 13; count += count == 12 ? 1 : 4) {
        check_host_dma_read_stream(qts, 4, count);
    }
    qtest_quit(qts);
}

static void test_host_dma_read_capacity(void)
{
    static const unsigned counts[] = { 255, 256, 257, 512, 65537 };
    QTestState *qts = start_aic();

    for (unsigned i = 0; i < ARRAY_SIZE(counts); i++) {
        check_host_dma_read_stream(qts, 0, counts[i]);
    }
    qtest_quit(qts);
}

static void test_fifo_start_thresholds(void)
{
    static const struct {
        unsigned receive_min[2];
        unsigned transmit_max[2];
    } levels[] = {
        { { 24, 28 }, { 232, 228 } },
        { { 128, 132 }, { 128, 124 } },
        { { 192, 196 }, { 64, 60 } },
        { { 256, 256 }, { 0, 0 } },
    };

    for (unsigned selector = 0; selector < ARRAY_SIZE(levels); selector++) {
        for (unsigned from_memory = 0; from_memory < 2; from_memory++) {
            for (unsigned offset = 0; offset <= 4; offset += 4) {
                QTestState *qts = start_aic();
                unsigned boundary = from_memory ?
                    levels[selector].transmit_max[offset / 4] :
                    levels[selector].receive_min[offset / 4];
                unsigned end = MIN(boundary + 1, 256);

                host_dma_program(qts, 0x10000 + offset, 0);
                qtest_writeb(qts, REG_BASE + 0x93,
                             (from_memory ? 4 : 0) | 1);
                qtest_writeb(qts, REG_BASE + 0x86, selector << 6);
                for (unsigned stored = 0; stored <= end; stored++) {
                    bool ready = from_memory ? stored <= boundary :
                                               stored >= boundary;

                    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x14,
                                    ==, ready ? 4 : 0);
                    if (stored < end) {
                        qtest_writeb(qts, REG_BASE + 0x99, stored);
                    }
                }
                g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==,
                                0x10000 + offset);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
                qtest_quit(qts);
            }
        }
    }
}

static void test_fifo_threshold_pending_admission(void)
{
    static const unsigned words[] = { 3, 16, 24, 32 };

    for (unsigned selector = 0; selector < ARRAY_SIZE(words); selector++) {
        for (unsigned from_memory = 0; from_memory < 2; from_memory++) {
            QTestState *qts = start_aic();
            uint8_t pattern[256], seed[256], guarded[272], got[272];
            unsigned initial = from_memory ? (32 - words[selector]) * 8 :
                                             words[selector] * 8;
            unsigned count = from_memory ? 255 - initial : initial;

            for (unsigned i = 0; i < sizeof(pattern); i++) {
                pattern[i] = i * 53 + 9;
                seed[i] = i * 17 + 3;
            }
            memset(guarded, 0xa5, sizeof(guarded));
            if (from_memory) {
                memcpy(guarded + 8, pattern, count);
            }
            qtest_memwrite(qts, 0x10000, guarded, sizeof(guarded));
            host_dma_program(qts, 0x10008, count);
            qtest_writeb(qts, REG_BASE + 0x93,
                         (from_memory ? 4 : 0) | 1);
            qtest_writeb(qts, REG_BASE + 0x86, selector << 6);
            for (unsigned i = 0; i < initial; i++) {
                qtest_writeb(qts, REG_BASE + 0x99, seed[i]);
            }
            config_writew(qts, 4, 2);
            qtest_writeb(qts, REG_BASE + 0x93, from_memory ? 0x0c : 8);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x14,
                            ==, 0x14);
            /* PCI admission is off, leaving one active FIFO port. */
            if (from_memory) {
                qtest_writeb(qts, REG_BASE + 0x99, 0x7d);
            } else {
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==, seed[0]);
            }
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x14,
                            ==, 0x10);
            g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x10008);
            g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, count);
            qtest_memread(qts, 0x10000, got, sizeof(got));
            g_assert_cmpmem(got, sizeof(got), guarded, sizeof(guarded));
            config_writew(qts, 4, 6);
            if (from_memory) {
                g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x1a,
                                ==, 0x0a);
                qtest_writeb(qts, REG_BASE + 0x93, 4);
                for (unsigned i = 0; i < initial; i++) {
                    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==,
                                    seed[i]);
                }
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==, 0x7d);
                for (unsigned i = 0; i < count; i++) {
                    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==,
                                    pattern[i]);
                }
            } else {
                g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 1);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x19,
                                ==, 1);
                qtest_memread(qts, 0x10000, got, sizeof(got));
                g_assert_cmpmem(got + 8, count - 1, seed + 1, count - 1);
                qtest_writeb(qts, REG_BASE + 0x99, 0x7d);
                g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 1);
                qtest_writeb(qts, REG_BASE + 0x93, 0x0a);
                g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
                qtest_memread(qts, 0x10000, got, sizeof(got));
                g_assert_cmpmem(got + 8, count - 1, seed + 1, count - 1);
                g_assert_cmphex(got[8 + count - 1], ==, 0x7d);
                g_assert_cmpmem(got, 8, guarded, 8);
                g_assert_cmpmem(got + 8 + count, sizeof(got) - 8 - count,
                                guarded + 8 + count,
                                sizeof(guarded) - 8 - count);
            }
            if (from_memory) {
                qtest_memread(qts, 0x10000, got, sizeof(got));
                g_assert_cmpmem(got, sizeof(got), guarded, sizeof(guarded));
            }
            g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x10008);
            g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
            qtest_quit(qts);
        }
    }
}

static void test_fifo_threshold_migrates(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-threshold-XXXXXX", &error);
    g_autofree char *socket_path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket_path = g_build_filename(dir, "migrate.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket_path);
    from = start_aic();
    to = qtest_init("-M pc -m 128M -nodefaults -S -incoming defer "
                    "-device aic7880,id=hba,bus=pci.0,addr=04.0");
    host_dma_program(from, 0x10000, 0);
    qtest_writeb(from, REG_BASE + 0x93, 1);
    qtest_writeb(from, REG_BASE + 0x86, 0x80);
    for (unsigned i = 0; i < 200; i++) {
        qtest_writeb(from, REG_BASE + 0x99, i * 37 + 11);
    }
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x94) & 0x14, ==, 4);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x86), ==, 0x80);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x94) & 0x14, ==, 4);
    for (unsigned i = 0; i < 200; i++) {
        g_assert_cmphex(qtest_readb(to, REG_BASE + 0x99), ==,
                        (uint8_t)(i * 37 + 11));
        g_assert_cmphex(qtest_readb(to, REG_BASE + 0x94) & 0x14, ==,
                        i < 8 ? 4 : 0);
    }
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x94) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x92), ==, 0);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket_path);
    g_rmdir(dir);
}

static void test_fifo_threshold_selector_change(void)
{
    QTestState *qts = start_aic();
    uint8_t expected[144], got[144];

    memset(expected, 0xa5, sizeof(expected));
    qtest_memwrite(qts, 0x10000, expected, sizeof(expected));
    host_dma_program(qts, 0x10008, 128);
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    qtest_writeb(qts, REG_BASE + 0x86, 0xc0);
    for (unsigned i = 0; i < 128; i++) {
        expected[8 + i] = i * 7 + 1;
        qtest_writeb(qts, REG_BASE + 0x99, expected[8 + i]);
    }
    qtest_writeb(qts, REG_BASE + 0x93, 8);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 128);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x14, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x86, 0x40);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x19, ==, 9);
    qtest_memread(qts, 0x10000, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
    qtest_quit(qts);
}

static void test_host_dma_fifo_to_memory(void)
{
    uint8_t pattern[256], guarded[272], got[272];
    QTestState *qts = start_aic();

    memset(guarded, 0xa5, sizeof(guarded));
    for (unsigned i = 0; i < sizeof(pattern); i++) {
        pattern[i] = i * 53 + 7;
    }
    qtest_memwrite(qts, 0x10000, guarded, sizeof(guarded));
    host_dma_program(qts, 0x10008, 256);
    qtest_writeb(qts, REG_BASE + 0x93, 0x01);
    for (unsigned i = 0; i < sizeof(pattern); i++) {
        qtest_writeb(qts, REG_BASE + 0x99, pattern[i]);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 3, ==, 2);
    qtest_writeb(qts, REG_BASE + 0x93, 0x08);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x10108);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x0b, ==, 9);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    qtest_memread(qts, 0x10000, got, sizeof(got));
    g_assert_cmpmem(got, 8, guarded, 8);
    g_assert_cmpmem(got + 8, 256, pattern, 256);
    g_assert_cmpmem(got + 264, 8, guarded + 264, 8);
    qtest_quit(qts);
}

static void test_host_dma_short_word_flush(void)
{
    static const uint8_t word[8] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x37
    };
    uint8_t guarded[24], got[24];
    QTestState *qts = start_aic();

    memset(guarded, 0xa5, sizeof(guarded));
    qtest_memwrite(qts, 0x10000, guarded, sizeof(guarded));
    host_dma_program(qts, 0x10008, sizeof(word));
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    for (unsigned i = 0; i < sizeof(word); i++) {
        qtest_writeb(qts, REG_BASE + 0x99, word[i]);
    }
    qtest_writeb(qts, REG_BASE + 0x93, 0x0a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x10010);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 9, ==, 9);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_memread(qts, 0x10000, got, sizeof(got));
    g_assert_cmpmem(got, 8, guarded, 8);
    g_assert_cmpmem(got + 8, sizeof(word), word, sizeof(word));
    g_assert_cmpmem(got + 16, 8, guarded + 16, 8);
    qtest_quit(qts);
}

static void test_host_dma_short_word_fault(void)
{
    QTestState *qts = start_aic();

    host_dma_program(qts, 0x70000000, 8);
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    for (unsigned i = 0; i < 8; i++) {
        qtest_writeb(qts, REG_BASE + 0x99, i * 17 + 3);
    }
    qtest_writeb(qts, REG_BASE + 0x93, 0x0a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x70000000);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 9, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    qtest_quit(qts);
}

static void test_host_dma_short_word_master_enable(void)
{
    uint8_t guarded[24], got[24];
    QTestState *qts = start_aic();

    memset(guarded, 0xa5, sizeof(guarded));
    qtest_memwrite(qts, 0x10000, guarded, sizeof(guarded));
    config_writew(qts, 0x04, 0x0002);
    host_dma_program(qts, 0x10008, 8);
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    for (unsigned i = 0; i < 8; i++) {
        qtest_writeb(qts, REG_BASE + 0x99, i * 17 + 3);
    }
    qtest_writeb(qts, REG_BASE + 0x93, 0x0a);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 2);
    qtest_memread(qts, 0x10000, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), guarded, sizeof(guarded));

    config_writew(qts, 0x04, 0x0006);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 9, ==, 9);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    qtest_memread(qts, 0x10000, got, sizeof(got));
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(got[i], ==, 0xa5);
        g_assert_cmphex(got[8 + i], ==, i * 17 + 3);
        g_assert_cmphex(got[16 + i], ==, 0xa5);
    }
    qtest_quit(qts);
}

static void check_host_dma_write_stream(unsigned offset, unsigned count)
{
    g_autofree uint8_t *pattern = g_malloc(count);
    g_autofree uint8_t *guarded = g_malloc(count + 16);
    g_autofree uint8_t *got = g_malloc(count + 16);
    uint32_t address = 0x400000 + offset;
    unsigned sent = 0;
    QTestState *qts = start_aic();

    memset(guarded, 0xa5, count + 16);
    for (unsigned i = 0; i < count; i++) {
        pattern[i] = (11 + i * 37) ^ (i >> 8) ^ (i >> 16);
    }
    qtest_memwrite(qts, address - 8, guarded, count + 16);
    host_dma_program(qts, address, count);
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    /* Keep this stream test on full-FIFO start and explicit tail flush. */
    qtest_writeb(qts, REG_BASE + 0x86, 0xc0);
    qtest_writeb(qts, REG_BASE + 0x93, 8);
    while (sent < count) {
        unsigned block = MIN(count - sent, 256);

        for (unsigned i = 0; i < block; i++) {
            qtest_writeb(qts, REG_BASE + 0x99, pattern[sent + i]);
            if (i + 1 < 256 && (!(i & 7) || i + 1 == block)) {
                g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==,
                                address + sent);
                g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==,
                                count - sent);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8,
                                ==, 0);
            }
        }
        if (block < 256) {
            qtest_writeb(qts, REG_BASE + 0x93, 0x0a);
        }
        sent += block;
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==,
                        address + sent);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, count - sent);
        check_host_fifo_status(qts, (offset + sent) & 255, 0, sent == count);
    }
    g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, address);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_memread(qts, address - 8, got, count + 16);
    g_assert_cmpmem(got, 8, guarded, 8);
    g_assert_cmpmem(got + 8, count, pattern, count);
    g_assert_cmpmem(got + 8 + count, 8, guarded + 8 + count, 8);
    qtest_quit(qts);
}

static void test_host_dma_write_offsets(void)
{
    for (unsigned offset = 0; offset < 8; offset++) {
        unsigned counts[] = { 1, 8 - offset, 9 - offset, 28 };

        for (unsigned i = 0; i < G_N_ELEMENTS(counts); i++) {
            check_host_dma_write_stream(offset, counts[i]);
        }
    }
    check_host_dma_write_stream(4, 4);
    check_host_dma_write_stream(4, 8);
    check_host_dma_write_stream(4, 12);
    check_host_dma_write_stream(4, 13);
}

static void test_host_dma_write_capacity(void)
{
    unsigned counts[] = { 255, 256, 257, 512, 65537 };

    for (unsigned i = 0; i < G_N_ELEMENTS(counts); i++) {
        check_host_dma_write_stream(0, counts[i]);
    }
}

static void test_host_dma_write_failed_prefix(void)
{
    uint8_t pattern[28], prefix[20], guard[8], got[20];
    QTestState *qts = qtest_init(
        "-M pc -m 128M -nodefaults "
        "-device aic7880,id=hba,bus=pci.0,addr=04.0 "
        "-device memory-buffer-testdev,id=prefix,size=20 "
        "-device memory-buffer-testdev,id=guard,size=8");

    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/prefix", 2, 0x50000000);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/guard", 2, 0x50000020);
    memset(prefix, 0xa5, sizeof(prefix));
    memset(guard, 0x5a, sizeof(guard));
    for (unsigned i = 0; i < sizeof(pattern); i++) {
        pattern[i] = i * 23 + 7;
    }
    qtest_memwrite(qts, 0x50000000, prefix, sizeof(prefix));
    qtest_memwrite(qts, 0x50000020, guard, sizeof(guard));
    host_dma_program(qts, 0x50000004, sizeof(pattern));
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    for (unsigned i = 0; i < sizeof(pattern); i++) {
        qtest_writeb(qts, REG_BASE + 0x99, pattern[i]);
    }
    qtest_writeb(qts, REG_BASE + 0x93, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 0x0a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x50000004);
    g_assert_cmpuint(host_dma_read_bytes(qts, 0x8c, 3), ==, 28);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    qtest_memread(qts, 0x50000000, got, sizeof(got));
    g_assert_cmpmem(got, 4, prefix, 4);
    g_assert_cmpmem(got + 4, 16, pattern, 16);
    qtest_memread(qts, 0x50000020, got, sizeof(guard));
    g_assert_cmpmem(got, sizeof(guard), guard, sizeof(guard));
    qtest_writeb(qts, REG_BASE + 0x93, 0);
    for (unsigned i = 0; i < sizeof(pattern); i++) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==, pattern[i]);
    }
    qtest_quit(qts);
}

static void test_host_dma_short_reads(void)
{
    uint8_t source[16], got[16];

    for (unsigned n = 1; n <= 7; n++) {
        QTestState *qts = start_aic();

        memset(source, 0xa5, sizeof(source));
        for (unsigned i = 0; i < 7; i++) {
            source[i + 8] = i * 23 + 3;
        }
        qtest_memwrite(qts, 0x10000, source, sizeof(source));
        host_dma_program(qts, 0x10008, n);
        qtest_writeb(qts, REG_BASE + 0x93, 0x05);
        qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x10008 + n);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x29, ==, 0x28);
        qtest_writeb(qts, REG_BASE + 0x93, 0x04);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 8, ==, 0);
        for (unsigned i = 0; i < n; i++) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==,
                            source[i + 8]);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x21, ==, 0x21);
        qtest_memread(qts, 0x10000, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), source, sizeof(source));
        qtest_quit(qts);
    }
}

static void test_host_dma_address_endpoint(void)
{
    static const struct {
        uint32_t address;
        unsigned count;
        bool accepted;
    } cases[] = {
        { 0xfffffffe, 1, true },
        { 0xffffffff, 1, false },
        { 0xfffffffe, 3, false },
    };

    for (unsigned c = 0; c < G_N_ELEMENTS(cases); c++) {
        uint8_t calls[16];
        QTestState *qts = qtest_init(
            "-M pc -m 128M -nodefaults "
            "-device aic7880,id=hba,bus=pci.0,addr=04.0 "
            "-device memory-buffer-testdev,id=endpoint,size=16");

        config_writel(qts, 0x14, REG_BASE);
        config_writew(qts, 0x04, 0x0006);
        qtest_sysbus_mmio_map(qts, "/machine/peripheral/endpoint", 0,
                             0xfffffff0);
        qtest_sysbus_mmio_map(qts, "/machine/peripheral/endpoint", 1,
                             0x50000000);
        host_dma_program(qts, cases[c].address, cases[c].count);
        qtest_writeb(qts, REG_BASE + 0x93, 5);
        qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
        qtest_memread(qts, 0x50000000, calls, sizeof(calls));
        if (cases[c].accepted) {
            g_assert_cmpuint(ldq_be_p(calls) + ldq_be_p(calls + 8), >, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
            g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==,
                            UINT32_MAX);
            g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 8);
        } else {
            g_assert_cmpuint(ldq_be_p(calls) + ldq_be_p(calls + 8), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40,
                            ==, 0);
            g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==,
                            cases[c].address);
            g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==,
                            cases[c].count);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 9, ==, 1);
        }
        qtest_quit(qts);
    }
}

static void test_host_dma_zero_count(void)
{
    uint8_t before[16], after[16];
    QTestState *qts = start_aic();

    memset(before, 0xa5, sizeof(before));
    qtest_memwrite(qts, 0x10000, before, sizeof(before));
    host_dma_program(qts, 0x10008, 0);
    g_test_message("HDONE after zero HCNT: %u",
                   !!(qtest_readb(qts, REG_BASE + 0x94) & 8));
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 0x05);
    g_test_message("HDONE after FIFORESET: %u",
                   !!(qtest_readb(qts, REG_BASE + 0x94) & 8));
    qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
    g_test_message("HDONE after zero-count HDMAEN: %u",
                   !!(qtest_readb(qts, REG_BASE + 0x94) & 8));
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x10008);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    qtest_memread(qts, 0x10000, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), before, sizeof(before));
    qtest_quit(qts);
}

static void test_host_dma_pci_command_inhibits(void)
{
    uint8_t source[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    QTestState *qts = start_aic();

    qtest_memwrite(qts, 0x10000, source, sizeof(source));
    host_dma_program(qts, 0x10000, sizeof(source));
    qtest_writeb(qts, REG_BASE + 0x93, 0x05);
    config_writew(qts, 0x04, 0x0002);
    qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x10000);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    config_writew(qts, 0x04, 0x0004);
    g_assert_cmphex(config_readl(qts, 0x04) & 7, ==, 4);
    config_writew(qts, 0x04, 0x0006);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x10008);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 8);
    qtest_quit(qts);
}

static void test_host_dma_fault(void)
{
    QTestState *qts = start_aic();

    host_dma_program(qts, 0x70000000, 8);
    qtest_writeb(qts, REG_BASE + 0x93, 0x05);
    qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x70000000);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_quit(qts);
}

static void test_host_dma_reset_pending(void)
{
    uint8_t before[256], after[256];
    QTestState *qts = start_aic();

    memset(before, 0xa5, sizeof(before));
    qtest_memwrite(qts, 0x10000, before, sizeof(before));
    host_dma_program(qts, 0x10000, sizeof(before));
    qtest_writeb(qts, REG_BASE + 0x93, 0x01);
    for (unsigned i = 0; i < sizeof(before); i++) {
        qtest_writeb(qts, REG_BASE + 0x99, i);
    }
    config_writew(qts, 0x04, 0x0002);
    qtest_writeb(qts, REG_BASE + 0x93, 0x08);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    config_writew(qts, 0x04, 0x0006);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x29, ==, 0x21);
    qtest_memread(qts, 0x10000, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), before, sizeof(before));
    qtest_quit(qts);
}

static void test_host_dma_pci_reset_pending(void)
{
    uint8_t before[256], after[256];
    QTestState *qts = start_aic();

    memset(before, 0xa5, sizeof(before));
    qtest_memwrite(qts, 0x10000, before, sizeof(before));
    host_dma_program(qts, 0x10000, sizeof(before));
    qtest_writeb(qts, REG_BASE + 0x93, 0x01);
    for (unsigned i = 0; i < sizeof(before); i++) {
        qtest_writeb(qts, REG_BASE + 0x99, i);
    }
    config_writew(qts, 0x04, 0x0002);
    qtest_writeb(qts, REG_BASE + 0x93, 0x08);
    qtest_system_reset(qts);
    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x29, ==, 0x21);
    qtest_memread(qts, 0x10000, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), before, sizeof(before));
    qtest_quit(qts);
}


static void test_host_dfdat_rejects_mixed_scsi(void)
{
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x93, 0x20);
    qtest_writeb(qts, REG_BASE + 0x99, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
    qtest_quit(qts);
}

static void test_host_dfdat_rejects_underrun(void)
{
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x93, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
    qtest_quit(qts);
}

static void test_host_dfdat_rejects_overrun(void)
{
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x93, 1);
    for (unsigned i = 0; i < 256; i++) {
        qtest_writeb(qts, REG_BASE + 0x99, i);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 2, ==, 2);
    qtest_writeb(qts, REG_BASE + 0x99, 0xee);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 2, ==, 2);
    qtest_quit(qts);
}

static void test_host_dma_done_clears(void)
{
    const uint8_t source = 0x5a;

    for (unsigned reset_fifo = 0; reset_fifo < 2; reset_fifo++) {
        QTestState *qts = start_aic();

        qtest_memwrite(qts, 0x10000, &source, 1);
        host_dma_program(qts, 0x10000, 1);
        qtest_writeb(qts, REG_BASE + 0x93, 0x05);
        qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 8);
        if (reset_fifo) {
            qtest_writeb(qts, REG_BASE + 0x93, 0x05);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
        } else {
            qtest_writeb(qts, REG_BASE + 0x8c, 1);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
        qtest_quit(qts);
    }
}

static void test_host_dma_rejects_high_address_selection(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-host-high-XXXXXX", &error);
    g_autofree char *path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;

    g_assert_no_error(error);
    path = g_build_filename(dir, "aic.log", NULL);
    qts = start_aic_logged(path);
    qtest_writeb(qts, REG_BASE + 0x85, 1);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "unsupported register 0x85"));
    unlink(path);
    g_rmdir(dir);
}

static void test_host_dfdat_rejects_sfunct(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-host-sfunct-XXXXXX",
                                        &error);
    g_autofree char *path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;

    g_assert_no_error(error);
    path = g_build_filename(dir, "aic.log", NULL);
    qts = start_aic_logged(path);
    qtest_writeb(qts, REG_BASE + 0x9f, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9f), ==, 0xff);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "write to unsupported register 0x9f"));
    g_assert_nonnull(strstr(log, "read of unsupported register 0x9f"));
    unlink(path);
    g_rmdir(dir);
}

static void test_host_dfdat_migrates(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-host-fifo-XXXXXX", &error);
    g_autofree char *socket_path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket_path = g_strdup_printf("%s/migrate.sock", dir);
    uri = g_strdup_printf("unix:%s", socket_path);
    from = qtest_init("-M pc -m 128M -nodefaults -S "
                      "-device aic7880,bus=pci.0,addr=04.0");
    to = qtest_init("-M pc -m 128M -nodefaults -S -incoming defer "
                    "-device aic7880,bus=pci.0,addr=04.0");
    config_writel(from, 0x14, REG_BASE);
    config_writew(from, 0x04, 0x0006);
    host_dma_program(from, 0x10004, 0);
    qtest_writeb(from, REG_BASE + 0x93, 1);
    qtest_writeb(from, REG_BASE + 0x99, 0x13);
    qtest_writeb(from, REG_BASE + 0x99, 0x57);
    qtest_writeb(from, REG_BASE + 0x99, 0x9b);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x94) & 0x21, ==, 0x20);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x99), ==, 0x13);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x99), ==, 0x57);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x99), ==, 0x9b);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x94) & 0x21, ==, 0x21);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket_path);
    g_rmdir(dir);
}

static void test_host_dma_read_migrates(void)
{
    g_autofree char *dir = g_dir_make_tmp(
        "aic7880-stream-migrate-XXXXXX", NULL);
    g_autofree char *socket_path = g_build_filename(dir, "migrate.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket_path);
    QTestState *from = start_aic();
    QTestState *to = qtest_init("-M pc -m 128M -nodefaults -S -incoming defer "
                               "-device aic7880,bus=pci.0,addr=04.0");
    uint8_t bytes[28];

    for (unsigned i = 0; i < sizeof(bytes); i++) {
        bytes[i] = 0x1d + i * 43;
    }
    qtest_memwrite(from, 0x340004, bytes, sizeof(bytes));
    host_dma_program(from, 0x340004, sizeof(bytes));
    qtest_writeb(from, REG_BASE + 0x93, 5);
    qtest_writeb(from, REG_BASE + 0x93, 0x0c);
    qtest_writeb(from, REG_BASE + 0x93, 4);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x93) & 8, ==, 0);
    for (unsigned i = 0; i < 11; i++) {
        g_assert_cmphex(qtest_readb(from, REG_BASE + 0x99), ==, bytes[i]);
    }
    check_host_fifo_status(from, 15, 17, true);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(host_dma_read_bytes(to, 0x88, 4), ==, 0x340020);
    g_assert_cmphex(host_dma_read_bytes(to, 0x8c, 3), ==, 0);
    check_host_fifo_status(to, 15, 17, true);
    for (unsigned i = 11; i < sizeof(bytes); i++) {
        g_assert_cmphex(qtest_readb(to, REG_BASE + 0x99), ==, bytes[i]);
    }
    check_host_fifo_status(to, 32, 0, true);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket_path);
    g_rmdir(dir);
}

static void test_host_dma_active_migration_refused(void)
{
    g_autofree char *dir = g_dir_make_tmp(
        "aic7880-active-migrate-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    g_autofree char *uri = g_strdup_printf("file:%s", path);
    QTestState *qts = start_aic();

    qtest_memset(qts, 0x350000, 0x6b, 257);
    host_dma_program(qts, 0x350000, 257);
    qtest_writeb(qts, REG_BASE + 0x93, 5);
    qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x350100);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 1);
    qtest_qmp_assert_success(qts,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration(qts, "failed");
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 1);
    check_host_fifo_status(qts, 0, 256, false);
    qtest_quit(qts);
    unlink(path);
    g_rmdir(dir);
}

static void test_sequencer_upload(void)
{
    static const uint8_t program[] = { 0x11, 0x22, 0x33, 0x44 };
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 0x04);
    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x63, 0);
    for (size_t i = 0; i < sizeof(program); i++) {
        qtest_writeb(qts, REG_BASE + 0x61, program[i]);
    }
    qtest_writeb(qts, REG_BASE + 0x60, 0x90);
    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x63, 0);
    for (size_t i = 0; i < sizeof(program); i++) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x61), ==, program[i]);
    }
    qtest_quit(qts);
}

static void test_sequencer_exec(void)
{
    /* OR 1 from ALLZEROS into INTSTAT; the sequencer must request service. */
    static const uint8_t program[] = { 0x01, 0x6a, 0x91, 0x00 };
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x63, 0);
    for (size_t i = 0; i < sizeof(program); i++) {
        qtest_writeb(qts, REG_BASE + 0x61, program[i]);
    }
    qtest_writeb(qts, REG_BASE + 0x60, 0x90);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x63, 0);
    resume_program(qts, 2);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 0);
    qtest_quit(qts);
}

static void test_old_sequencer_format(void)
{
    static const struct {
        uint8_t word[4];
        uint8_t dest;
    } cases[] = {
        { { 0xff, 0x6a, 0x65, 0x02 }, 0x65 },
        { { 0xff, 0x6a, 0x64, 0x02 }, 0x64 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        QTestState *qts = start_aic();

        qtest_writeb(qts, REG_BASE + cases[i].dest, 0x30);
        qtest_writeb(qts, REG_BASE + 0x60, 0x91);
        qtest_writeb(qts, REG_BASE + 0x62, 0);
        qtest_writeb(qts, REG_BASE + 0x63, 0);
        for (size_t byte = 0; byte < sizeof(cases[i].word); byte++) {
            qtest_writeb(qts, REG_BASE + 0x61, cases[i].word[byte]);
        }
        qtest_writeb(qts, REG_BASE + 0x60, 0x90);
        qtest_writeb(qts, REG_BASE + 0x60, 0x91);
        qtest_writeb(qts, REG_BASE + 0x62, 0);
        qtest_writeb(qts, REG_BASE + 0x63, 0);
        for (size_t byte = 0; byte < sizeof(cases[i].word); byte++) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x61), ==,
                            cases[i].word[byte]);
        }
        qtest_writeb(qts, REG_BASE + 0x60, 0x94);
        qtest_writeb(qts, REG_BASE + 0x62, 0);
        qtest_writeb(qts, REG_BASE + 0x63, 0);
        qtest_writeb(qts, REG_BASE + 0x87, 0);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + cases[i].dest), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 0x04);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x63), ==, 0);
        qtest_quit(qts);
    }
}

static QTestState *start_disk_mode(const char *extra)
{
    QTestState *qts = qtest_initf("-M pc -m 128M -nodefaults "
                                 "-drive if=none,id=disk0,file=null-co://,"
                                 "format=raw "
                                 "-device aic7880,id=hba,bus=pci.0,addr=04.0 "
                                 "-device scsi-hd,drive=disk0,bus=hba.0,"
                                 "scsi-id=0,lun=0 %s", extra);

    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    qtest_writeb(qts, REG_BASE + 0x01, 8);
    return qts;
}

static QTestState *start_disk(void)
{
    return start_disk_mode("");
}

static QTestState *start_selected_disk(const char *trace_path)
{
    g_autofree char *trace = trace_path ?
        g_strdup_printf("-trace enable=scsi_disk_new_request "
                        "-trace enable=aic7880_dma,file=%s",
                        trace_path) : g_strdup("");
    QTestState *qts = qtest_initf(
        "-M pc -m 128M -nodefaults -d guest_errors %s "
        "-drive if=none,id=disk0,file=null-co://,format=raw "
        "-device aic7880,id=hba,bus=pci.0,addr=04.0 "
        "-device scsi-hd,drive=disk0,bus=hba.0,scsi-id=0,lun=0,"
        "vendor=LABTEST,product=SEGMENTED-DISK,ver=1.0,scsi_version=5,"
        "serial=123456789ABCDEF", trace);

    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    return qts;
}

static QTestState *start_peer_disk(void)
{
    QTestState *qts = qtest_init("-M pc -m 128M -nodefaults "
                                 "-drive if=none,id=disk0,file=null-co://,"
                                 "format=raw "
                                 "-device aic7880,id=hba,bus=pci.0,addr=04.0,"
                                 "x-test-peer=on "
                                 "-device scsi-hd,drive=disk0,bus=hba.0,"
                                 "scsi-id=0,lun=0");

    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    qtest_writeb(qts, REG_BASE + 0x01, 8);
    return qts;
}

static void peer_event(QTestState *qts, const char *event)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/hba',"
        "'property':'x-test-peer-event','value':%s}}", event);

    if (qdict_haskey(reply, "error")) {
        g_error("peer event %s: %s", event,
                qdict_get_str(qdict_get_qdict(reply, "error"), "desc"));
    }
}

static void peer_event_rejected(QTestState *qts, const char *event)
{
    g_autoptr(QDict) reply = qtest_qmp_assert_failure_ref(qts,
        "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/hba',"
        "'property':'x-test-peer-event','value':%s}}", event);
}

static void emit(uint8_t *program, size_t *n, uint32_t word)
{
    for (unsigned byte = 0; byte < 4; byte++) {
        program[(*n)++] = word >> (byte * 8);
    }
}

static uint32_t alu_word(uint8_t op, uint8_t imm, uint8_t src,
                         uint8_t dst, bool ret)
{
    return imm | ((uint32_t)src << 8) | ((uint32_t)dst << 16) |
           ((uint32_t)ret << 24) | ((uint32_t)op << 25);
}

static uint32_t branch_word(uint8_t op, uint8_t imm, uint8_t src,
                            uint16_t target)
{
    return imm | ((uint32_t)src << 8) | ((uint32_t)target << 16) |
           ((uint32_t)op << 25);
}

static void emit_or(uint8_t *program, size_t *n, uint8_t value, uint8_t dest)
{
    emit(program, n, alu_word(0, value, 0x6a, dest, false));
}

static void emit_cdb_completion(uint8_t *program, size_t *n,
                                 const uint8_t *cdb, size_t cdb_len,
                                 bool write, bool host_dma)
{
    unsigned wait_at, free_at, idle_at;

    g_assert_cmpuint(cdb_len, <=, 16);
    emit_or(program, n, 0x80, 0x03);
    for (size_t i = 0; i < cdb_len; i++) {
        emit_or(program, n, cdb[i], 0x06);
    }
    emit_or(program, n, (write ? 0x34 : 0x30) | (host_dma ? 10 : 0), 0x93);
    wait_at = *n / 4;
    emit(program, n, branch_word(0xf, 2, 0x0b, wait_at));
    emit_or(program, n, 0xc0, 0x03);
    emit(program, n, alu_word(0, 0, 0x06, 0x22, false)); /* status */
    emit_or(program, n, 0xe0, 0x03);
    emit(program, n, alu_word(0, 0, 0x06, 0x23, false)); /* message */
    free_at = *n / 4;
    emit(program, n, branch_word(0xf, 8, 0x0c, free_at));
    emit_or(program, n, 1, 0x9d);
    emit_or(program, n, 2, 0x91);
    idle_at = *n / 4;
    emit(program, n, branch_word(8, 0, 0x65, idle_at));
}

static size_t build_command_program_mode(uint8_t *program, const uint8_t *cdb,
                                         size_t cdb_len, bool write,
                                         bool host_dma, int identify)
{
    size_t n = 0;

    emit_or(program, &n, 0x08, 0x01); /* automatic PIO */
    emit_or(program, &n, 0x07, 0x05); /* initiator 7, target 0 */
    emit_or(program, &n, 0, 0x93); /* disable the old data path */
    emit_or(program, &n, write ? 4 : 0, 0x93);
    emit(program, &n, alu_word(1, 0xff, 0x6a, 0x00, false));
    emit_or(program, &n, identify >= 0 ? 0xa0 : 0x80, 0x03);
    emit_or(program, &n, identify >= 0 ? 0x48 : 0x40, 0x00);
    if (identify >= 0) {
        emit_or(program, &n, identify, 0x06);
        emit_or(program, &n, 0x40, 0x0c);
    }
    emit_cdb_completion(program, &n, cdb, cdb_len, write, host_dma);
    return n;
}

static size_t build_command_program(uint8_t *program, const uint8_t *cdb,
                                    size_t cdb_len, bool write)
{
    return build_command_program_mode(program, cdb, cdb_len, write, true, -1);
}

static void load_program(QTestState *qts, const uint8_t *program, size_t len)
{
    qtest_writeb(qts, REG_BASE + 0x87, 4);
    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x63, 0);
    for (size_t i = 0; i < len; i++) {
        qtest_writeb(qts, REG_BASE + 0x61, program[i]);
    }
    qtest_writeb(qts, REG_BASE + 0x60, 0x90);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x63, 0);
}

static void start_program_async(QTestState *qts, const uint8_t *program,
                                size_t len)
{
    load_program(qts, program, len);
    qtest_writeb(qts, REG_BASE + 0x87, 2);
}

static int64_t start_program(QTestState *qts, const uint8_t *program,
                             size_t len)
{
    load_program(qts, program, len);
    return resume_program(qts, 2);
}

static void mark_internal_scb_canaries(QTestState *qts)
{
    uint8_t program[8];
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x90, 0);
    qtest_writeb(qts, REG_BASE + 0xa0, 0x11);
    qtest_writeb(qts, REG_BASE + 0x90, 15);
    qtest_writeb(qts, REG_BASE + 0xa0, 0x22);
    emit_or(program, &n, 0x0d, 0x9d);
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    wait_paused(qts);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    qtest_writeb(qts, REG_BASE + 0x9b, 0x0c);
}

static void check_internal_scb_canaries(QTestState *qts)
{
    qtest_writeb(qts, REG_BASE + 0x90, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0), ==, 0x11);
    qtest_writeb(qts, REG_BASE + 0x90, 15);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0), ==, 0x22);
    g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
}

static void test_function_constants(void)
{
    for (unsigned reset = 0; reset < 3; reset++) {
        g_autofree char *dir = g_dir_make_tmp("aic7880-constants-XXXXXX", NULL);
        g_autofree char *path = g_build_filename(dir, "qemu.log", NULL);
        QTestState *qts = start_aic_logged(path);
        uint8_t program[16];
        size_t n = 0;

        if (reset == 1) {
            qtest_writeb(qts, REG_BASE + 0x87, 1);
        } else if (reset == 2) {
            qtest_system_reset(qts);
            config_writel(qts, 0x14, REG_BASE);
            config_writew(qts, 4, 6);
        }
        mark_internal_scb_canaries(qts);
        qtest_writeb(qts, REG_BASE + 0x47, 0xa5);
        emit(program, &n, alu_word(3, 1, 0x69, 0x6a, false));
        emit_or(program, &n, 1, 0x91);
        start_program(qts, program, n);
        wait_paused(qts);
        qtest_writeb(qts, REG_BASE + 0x92, 1);
        for (unsigned i = 0; i < 8; i++) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x69), ==, 0xff);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6a), ==, 0);
            qtest_writeb(qts, REG_BASE + 0x6a, 0x80 + i);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x47), ==, 0xa5);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
            g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);
            g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
        }
        /* Also prove host constant reads did not latch diagnostic stop. */
        qtest_writeb(qts, REG_BASE + 0x20, 0xa5);
        qtest_writeb(qts, REG_BASE + 0x21, 0xc3);
        n = 0;
        emit(program, &n, alu_word(1, 0xff, 0x69, 0x20, false));
        emit(program, &n, alu_word(1, 0xff, 0x6a, 0x21, false));
        emit_or(program, &n, 1, 0x91);
        start_program(qts, program, n);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0xff);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x47), ==, 0xa5);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        check_internal_scb_canaries(qts);
        qtest_quit(qts);
        assert_no_aic_log(path);
        unlink(path);
        g_rmdir(dir);
    }
}

static void test_function1_selectors(void)
{
    QTestState *qts = start_aic();
    uint8_t program[16];

    qtest_writeb(qts, REG_BASE + 0x64, 0);
    qtest_writeb(qts, REG_BASE + 0x47, 0xa5);
    for (unsigned selector = 0; selector < 8; selector++) {
        for (unsigned outside = 0; outside < 32; outside++) {
            unsigned input = (selector << 4) | (outside & 15) |
                             ((outside & 16) << 3);
            unsigned expected = 1 << selector;
            size_t n = 0;

            qtest_writeb(qts, REG_BASE + 0x6e, input);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6e), ==, expected);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6e), ==, expected);
            emit_or(program, &n, input, 0x6e);
            emit(program, &n, alu_word(1, 0xff, 0x6e, 0x20, false));
            emit(program, &n, alu_word(1, 0xff, 0x6e, 0x21, false));
            emit_or(program, &n, 1, 0x91);
            start_program(qts, program, n);
            wait_paused(qts);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, expected);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, expected);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6e), ==, expected);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x47), ==, 0xa5);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 0);
            qtest_writeb(qts, REG_BASE + 0x92, 1);
        }
    }
    qtest_quit(qts);
}

static void test_function1_alu(void)
{
    for (unsigned carry = 0; carry < 2; carry++) {
        QTestState *qts = start_aic();
        uint8_t program[16];
        size_t n = 0;

        qtest_writeb(qts, REG_BASE + 0x6e, 0x30);
        emit(program, &n, alu_word(3, carry ? 0xf8 : 0x10,
                                   0x6e, 0x6e, false));
        emit(program, &n, alu_word(1, 0xff, 0x6e, 0x20, false));
        emit(program, &n, alu_word(1, 0xff, 0x6b, 0x21, false));
        emit_or(program, &n, 1, 0x91);
        start_program(qts, program, n);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, carry ? 1 : 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, carry ? 3 : 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6e), ==, carry ? 1 : 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        qtest_quit(qts);
    }
}

static void test_function_none_return(void)
{
    QTestState *qts = start_aic();
    uint8_t program[20];
    size_t n = 0;

    mark_internal_scb_canaries(qts);
    qtest_writeb(qts, REG_BASE + 0x23, 0xa5);
    emit(program, &n, branch_word(0x0b, 0, 0x6a, 3));
    emit_or(program, &n, 0x55, 0x22);
    emit_or(program, &n, 1, 0x91);
    emit(program, &n, alu_word(3, 1, 0x69, 0x6a, true));
    emit_or(program, &n, 0xee, 0x23);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x22), ==, 0x55);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x23), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6a), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    check_internal_scb_canaries(qts);
    qtest_quit(qts);
}

static void test_function_access(void)
{
    QTestState *qts = start_aic();
    uint8_t program[20];
    uint8_t flags;
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x47, 0xa5);
    emit_or(program, &n, 0x35, 0x46);
    emit(program, &n, branch_word(8, 0, 0x6a, 1));
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x46), ==, 0x35);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 0);
    flags = qtest_readb(qts, REG_BASE + 0x6b);
    qtest_writeb(qts, REG_BASE + 0x6e, 0xaf);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6e), ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x69), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6a), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x6a, 0xff);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x47), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, flags);
    qtest_writeb(qts, REG_BASE + 0x87, 4);

    config_writel(qts, 0x10, 0x1001);
    config_writew(qts, 4, 7);
    qtest_outb(qts, 0x106e, 0x5f);
    g_assert_cmphex(qtest_inb(qts, 0x106e), ==, 0x20);
    g_assert_cmphex(qtest_inb(qts, 0x1069), ==, 0xff);
    g_assert_cmphex(qtest_inb(qts, 0x106a), ==, 0);
    /* The existing byte-only region rejects typed word I/O in the core. */
    g_assert_cmphex(qtest_inw(qts, 0x1069), ==, 0);
    g_assert_cmphex(qtest_inw(qts, 0x106e), ==, 0);
    qtest_outw(qts, 0x106e, 0x00e0);
    g_assert_cmphex(qtest_inb(qts, 0x106e), ==, 0x20);

    n = 0;
    emit_or(program, &n, 0x6e, 0x66);
    emit_or(program, &n, 0xbf, 0x6d);
    emit_or(program, &n, 0x6e, 0x65);
    emit(program, &n, alu_word(1, 0xff, 0x6c, 0x20, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6e), ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x6f);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0x6f);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_function_refusals(void)
{
    for (unsigned sequencer = 0; sequencer < 2; sequencer++) {
        QTestState *qts = start_aic();
        uint8_t program[12];
        size_t n = 0;

        mark_internal_scb_canaries(qts);
        qtest_writeb(qts, REG_BASE + 0x47, 0xa5);
        qtest_writeb(qts, REG_BASE + 0x6e, 0x5f);
        if (sequencer) {
            emit_or(program, &n, 0xa5, 0x69);
        } else {
            qtest_writeb(qts, REG_BASE + 0x69, 0xa5);
        }
        emit_or(program, &n, 0x5a, 0x47);
        emit_or(program, &n, 1, 0x91);
        start_program(qts, program, n);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x47), ==, 0xa5);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x69), ==, 0xff);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6a), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6e), ==, 0x20);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        check_internal_scb_canaries(qts);
        qtest_quit(qts);
    }
    for (unsigned access = 0; access < 4; access++) {
        QTestState *qts = start_external_scb(1024, 8, MEMTX_TEST_DEVICE);
        uint64_t ignored;

        qtest_writeb(qts, REG_BASE + 0x6e, 0x5f);
        if (access < 2) {
            g_assert_cmpint(memtx_read(qts, REG_BASE + (access ? 0x6e : 0x69),
                                      &ignored, access ? 4 : 2), !=, MEMTX_OK);
        } else {
            g_assert_cmpint(memtx_write(qts, REG_BASE + (access == 2 ? 0x6e :
                                                        0x6a),
                                       0xffff, access == 2 ? 2 : 4),
                            !=, MEMTX_OK);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6e), ==, 0x20);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x69), ==, 0xff);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6a), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        qtest_quit(qts);
    }
}

static void check_function1_stream(const char *stream, uint8_t input)
{
    g_auto(GStrv) argv = g_new0(char *, 5);
    g_autofree char *output = NULL, *errors = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(QDict) state = NULL;
    const QDictEntry *section;
    unsigned found = 0;
    int status;
    bool spawned;

    g_assert_nonnull(g_getenv("PYTHON"));
    g_assert_nonnull(g_getenv("G_TEST_SRCDIR"));
    argv[0] = g_strdup(g_getenv("PYTHON"));
    argv[1] = g_build_filename(g_getenv("G_TEST_SRCDIR"), "..", "..",
                              "scripts", "analyze-migration.py", NULL);
    argv[2] = g_strdup("-f");
    argv[3] = g_strdup(stream);
    spawned = g_spawn_sync(NULL, argv, NULL, 0, NULL, NULL, &output, &errors,
                           &status, &error);
    g_assert_no_error(error);
    g_assert_true(spawned);
    if (!g_spawn_check_exit_status(status, &error)) {
        g_test_message("migration analyzer: %s", errors);
    }
    g_assert_no_error(error);
    state = qobject_to(QDict, qobject_from_json(output, &error_abort));
    g_assert_nonnull(state);
    for (section = qdict_first(state); section;
         section = qdict_next(state, section)) {
        QDict *device;
        QList *regs;
        const QListEntry *reg;
        unsigned index = 0;

        if (!strstr(qdict_entry_key(section), "aic7880")) {
            continue;
        }
        device = qobject_to(QDict, qdict_entry_value(section));
        g_assert_nonnull(device);
        regs = qdict_get_qlist(device, "regs");
        g_assert_cmpuint(qlist_size(regs), ==, 256);
        QLIST_FOREACH_ENTRY(regs, reg) {
            if (index == 0x69 || index == 0x6a || index == 0x6e) {
                QString *text = qobject_to(QString, qlist_entry_obj(reg));
                uint64_t value;

                g_assert_nonnull(text);
                g_assert_cmpint(qemu_strtou64(qstring_get_str(text), NULL, 0,
                                             &value), ==, 0);
                g_assert_cmphex(value, ==, index == 0x6e ? input : 0);
            }
            index++;
        }
        found++;
    }
    g_assert_cmpuint(found, ==, 1);
}

static void test_function1_state(void)
{
    g_autofree char *dir = g_dir_make_tmp(
        "aic7880-function-state-XXXXXX", NULL);
    g_autofree char *first = g_build_filename(dir, "first", NULL);
    g_autofree char *second = g_build_filename(dir, "second", NULL);
    g_autofree char *first_uri = g_strdup_printf("file:%s", first);
    g_autofree char *second_uri = g_strdup_printf("file:%s", second);
    QTestState *from = start_aic_with_options("-S");
    QTestState *to = start_aic_with_options("-S -incoming defer");

    qtest_writeb(from, REG_BASE + 0x6e, 0xdf);
    qtest_writeb(from, REG_BASE + 0x6a, 0xff);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x6e), ==, 0x20);
    sgi_migrate_stopped(from, first_uri);
    check_function1_stream(first, 0xdf);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", first_uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x6e), ==, 0x20);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x69), ==, 0xff);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x6a), ==, 0);
    sgi_migrate_stopped(to, second_uri);
    check_function1_stream(second, 0xdf);
    qtest_quit(to);
    qtest_quit(from);
    unlink(first);
    unlink(second);
    for (unsigned pci_reset = 0; pci_reset < 2; pci_reset++) {
        QTestState *qts = start_aic_with_options("-S");

        qtest_writeb(qts, REG_BASE + 0x6e, 0xdf);
        if (pci_reset) {
            qtest_system_reset(qts);
            config_writel(qts, 0x14, REG_BASE);
            config_writew(qts, 4, 6);
        } else {
            qtest_writeb(qts, REG_BASE + 0x87, 1);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6e), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x69), ==, 0xff);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6a), ==, 0);
        sgi_migrate_stopped(qts, first_uri);
        check_function1_stream(first, 0);
        qtest_quit(qts);
        unlink(first);
    }
    g_rmdir(dir);
}

static void test_internal_scb_pointer_migrates(void)
{
    g_autofree char *dir = g_dir_make_tmp("aic7880-pointer-XXXXXX", NULL);
    g_autofree char *socket = g_strdup_printf("%s/migrate.sock", dir);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = start_aic_with_options("-S");
    QTestState *to = start_aic_with_options("-S -incoming defer");
    uint8_t program[8];
    size_t n = 0;

    mark_internal_scb_canaries(from);
    qtest_writeb(from, REG_BASE + 0x90, 1);
    qtest_writeb(from, REG_BASE + 0xbf, 0x88);
    qtest_writeb(from, REG_BASE + 0x90, 16);
    qtest_writeb(from, REG_BASE + 0xbf, 0x31);
    qtest_writeb(from, REG_BASE + 0x90, 255);
    qtest_writeb(from, REG_BASE + 0xbf, 0xf1);
    qtest_writeb(from, REG_BASE + 0x90, 0xff);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x90), ==, 0xff);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x91), ==, 0);
    check_internal_scb_canaries(from);
    qtest_writeb(from, REG_BASE + 0x90, 0xff);
    /* A high pointer write alone must not put the model in diagnostic stop. */
    qtest_writeb(from, REG_BASE + 0x20, 0xa5);
    emit_or(program, &n, 0x5a, 0x20);
    emit_or(program, &n, 1, 0x91);
    start_program(from, program, n);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x20), ==, 0x5a);
    qtest_writeb(from, REG_BASE + 0x92, 1);
    qtest_writeb(from, REG_BASE + 0x9a, 0x9f);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x90), ==, 0xff);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x91), ==, 0);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9a), ==, 0x9f);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0xa0), ==, 0xf1);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0xbf), ==, 0x22);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9a), ==, 0x81);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x90), ==, 255);
    qtest_writeb(to, REG_BASE + 0x9a, 0);
    check_internal_scb_canaries(to);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0xbf), ==, 0xf1);
    qtest_writeb(to, REG_BASE + 0x90, 0);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0xbf), ==, 0x31);
    qtest_writeb(to, REG_BASE + 0x90, 1);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0xbf), ==, 0x88);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9d), ==, 0x0d);
    n = 0;
    emit(program, &n, alu_word(1, 0xff, 0x9b, 0x21, false));
    emit_or(program, &n, 1, 0x91);
    start_program(to, program, n);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x21), ==, 0x0c);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket);
    g_rmdir(dir);
}

static void test_internal_scb_pointer_reset(void)
{
    for (unsigned pci_reset = 0; pci_reset < 2; pci_reset++) {
        QTestState *qts = start_aic();

        mark_internal_scb_canaries(qts);
        qtest_writeb(qts, REG_BASE + 0x90, 0xff);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x90), ==, 0xff);
        if (pci_reset) {
            qtest_system_reset(qts);
            config_writel(qts, 0x14, REG_BASE);
            config_writew(qts, 4, 6);
        } else {
            qtest_writeb(qts, REG_BASE + 0x87, 1);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x90), ==, 0);
        g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9c), ==, 0);
        g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        mark_internal_scb_canaries(qts);
        check_internal_scb_canaries(qts);
        qtest_writeb(qts, REG_BASE + 0x90, 16);
        qtest_writeb(qts, REG_BASE + 0xbf, 0x73);
        qtest_writeb(qts, REG_BASE + 0x90, 255);
        qtest_writeb(qts, REG_BASE + 0xbf, 0x84);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x90), ==, 255);
        qtest_writeb(qts, REG_BASE + 0x90, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xbf), ==, 0x73);
        qtest_writeb(qts, REG_BASE + 0x90, 15);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xbf), ==, 0x84);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
        qtest_quit(qts);
    }
}

static void test_internal_scb_alias_host(void)
{
    g_autofree char *dir = g_dir_make_tmp("aic7880-scb-alias-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "qemu.log", NULL);
    QTestState *qts = start_aic_logged(path);
    static const uint8_t pages[] = { 0, 15, 1, 14 };
    static const uint8_t values[] = { 0x20, 0x80, 0xc0, 0xe0 };

    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    mark_internal_scb_canaries(qts);
    for (unsigned page = 0; page < ARRAY_SIZE(pages); page++) {
        qtest_writeb(qts, REG_BASE + 0x90, pages[page]);
        for (unsigned byte = 0; byte < 32; byte++) {
            qtest_writeb(qts, REG_BASE + 0xa0 + byte, values[page] + byte);
        }
    }
    for (unsigned alias = 0; alias < 2; alias++) {
        unsigned high = alias ? 255 : 16;
        unsigned low = alias ? 15 : 0;
        unsigned initial = alias ? 0x80 : 0x20;
        unsigned changed = alias ? 0xa0 : 0x40;

        qtest_writeb(qts, REG_BASE + 0x90, high);
        for (unsigned byte = 0; byte < 32; byte++) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0 + byte), ==,
                            initial + byte);
            qtest_writeb(qts, REG_BASE + 0xa0 + byte, changed + byte);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x90), ==, high);
        qtest_writeb(qts, REG_BASE + 0x90, low);
        for (unsigned byte = 0; byte < 32; byte++) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0 + byte), ==,
                            changed + byte);
        }
    }
    /* The I/O BAR selects the same byte array and full pointer latch. */
    config_writel(qts, 0x10, 0x1001);
    config_writew(qts, 4, 7);
    qtest_outb(qts, 0x1090, 255);
    qtest_outb(qts, 0x10bf, 0x7e);
    g_assert_cmphex(qtest_inb(qts, 0x1090), ==, 255);
    qtest_writeb(qts, REG_BASE + 0x90, 15);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xbf), ==, 0x7e);
    for (unsigned guard = 2; guard < ARRAY_SIZE(pages); guard++) {
        qtest_writeb(qts, REG_BASE + 0x90, pages[guard]);
        for (unsigned byte = 0; byte < 32; byte++) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0 + byte), ==,
                            values[guard] + byte);
        }
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    g_assert_false(qtest_get_irq(qts, 11));
    g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, 0x0d);
    qtest_quit(qts);
    assert_no_aic_log(path);
    unlink(path);
    g_rmdir(dir);
}

static void test_internal_scb_alias_sequencer(void)
{
    g_autofree char *dir = g_dir_make_tmp("aic7880-scb-seq-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "qemu.log", NULL);
    QTestState *qts = start_aic_logged(path);
    uint8_t program[64];
    size_t n = 0;
    static const uint8_t pages[] = { 0, 15, 1, 14 };

    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    mark_internal_scb_canaries(qts);
    for (unsigned page = 0; page < ARRAY_SIZE(pages); page++) {
        qtest_writeb(qts, REG_BASE + 0x90, pages[page]);
        qtest_writeb(qts, REG_BASE + 0xa0, 0x11 + page * 0x33);
        qtest_writeb(qts, REG_BASE + 0xa5, 0x22 + page * 0x33);
        qtest_writeb(qts, REG_BASE + 0xbf, 0x33 + page * 0x33);
    }
    emit_or(program, &n, 16, 0x90);
    emit(program, &n, alu_word(1, 0xff, 0xa0, 0x20, false));
    emit(program, &n, alu_word(1, 0xff, 0xbf, 0x21, false));
    emit_or(program, &n, 0x9a, 0xa5);
    emit_or(program, &n, 255, 0x90);
    emit(program, &n, alu_word(1, 0xff, 0xa0, 0x22, false));
    emit(program, &n, alu_word(1, 0xff, 0xbf, 0x23, false));
    emit_or(program, &n, 0x6b, 0xbf);
    emit_or(program, &n, 0xa0, 0x66);
    emit_or(program, &n, 0x7c, 0x6d);
    emit_or(program, &n, 0xbf, 0x65);
    emit(program, &n, alu_word(1, 0xff, 0x6c, 0x24, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x90), ==, 255);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x11);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, 0x33);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x22), ==, 0x44);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x23), ==, 0x66);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x24), ==, 0x6b);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    g_assert_false(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0x90, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa5), ==, 0x9a);
    qtest_writeb(qts, REG_BASE + 0x90, 15);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0), ==, 0x7c);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xbf), ==, 0x6b);
    for (unsigned page = 2; page < ARRAY_SIZE(pages); page++) {
        qtest_writeb(qts, REG_BASE + 0x90, pages[page]);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0), ==,
                        0x11 + page * 0x33);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa5), ==,
                        0x22 + page * 0x33);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xbf), ==,
                        0x33 + page * 0x33);
    }
    g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, 0x0d);
    qtest_quit(qts);
    assert_no_aic_log(path);
    unlink(path);
    g_rmdir(dir);
}

static void test_internal_scb_alias_auto(void)
{
    g_autofree char *dir = g_dir_make_tmp("aic7880-scb-auto-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "qemu.log", NULL);
    QTestState *qts = start_aic_logged(path);
    uint8_t program[16];
    size_t n = 0;

    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    mark_internal_scb_canaries(qts);
    for (unsigned page = 0; page < 2; page++) {
        qtest_writeb(qts, REG_BASE + 0x90, page);
        for (unsigned byte = 0; byte < 32; byte++) {
            qtest_writeb(qts, REG_BASE + 0xa0 + byte,
                         (page ? 0x80 : 0x20) + byte);
        }
    }
    qtest_writeb(qts, REG_BASE + 0x90, 16);
    qtest_writeb(qts, REG_BASE + 0x9a, 0x9f);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa4), ==, 0x3f);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xbf), ==, 0x20);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9a), ==, 0x81);
    qtest_writeb(qts, REG_BASE + 0x9a, 0x9f);
    qtest_writeb(qts, REG_BASE + 0xbf, 0x55);
    qtest_writeb(qts, REG_BASE + 0xa0, 0x66);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9a), ==, 0x81);
    qtest_writeb(qts, REG_BASE + 0x9a, 0x9f);
    emit(program, &n, alu_word(1, 0xff, 0xa0, 0x20, false));
    emit(program, &n, alu_word(1, 0xff, 0xbf, 0x21, false));
    emit_or(program, &n, 0x77, 0xaf);
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x90), ==, 16);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9a), ==, 0x82);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x55);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, 0x66);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    g_assert_false(qtest_get_irq(qts, 11));
    g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    qtest_writeb(qts, REG_BASE + 0x9a, 0);
    for (unsigned page = 0; page < 2; page++) {
        qtest_writeb(qts, REG_BASE + 0x90, page);
        for (unsigned byte = 0; byte < 32; byte++) {
            unsigned expected = (page ? 0x80 : 0x20) + byte;

            if (!page) {
                expected = byte == 31 ? 0x55 : byte == 0 ? 0x66 :
                           byte == 1 ? 0x77 : expected;
            }
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0 + byte), ==,
                            expected);
        }
    }
    qtest_quit(qts);
    assert_no_aic_log(path);
    unlink(path);
    g_rmdir(dir);
}

static void test_internal_scb_alias_bounds(void)
{
    static const unsigned outside[] = { 0x9f, 0xc0 };

    for (unsigned access = 0; access < 4; access++) {
        g_autofree char *dir = g_dir_make_tmp(
            "aic7880-scb-bounds-XXXXXX", NULL);
        g_autofree char *path = g_build_filename(dir, "qemu.log", NULL);
        g_autofree char *log = NULL;
        QTestState *qts = start_aic_logged(path);

        for (unsigned page = 0; page < 16; page++) {
            qtest_writeb(qts, REG_BASE + 0x90, page);
            for (unsigned byte = 0; byte < 32; byte++) {
                qtest_writeb(qts, REG_BASE + 0xa0 + byte, page * 7 + byte);
            }
        }
        qtest_writeb(qts, REG_BASE + 0x90, 255);
        qtest_writeb(qts, REG_BASE + 0x9a, 0x9f);
        if (access & 1) {
            qtest_writeb(qts, REG_BASE + outside[access / 2], 0xee);
        } else {
            qtest_readb(qts, REG_BASE + outside[access / 2]);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9a), ==, 0x9f);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x90), ==, 255);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
        qtest_writeb(qts, REG_BASE + 0x9a, 0);
        for (unsigned page = 0; page < 16; page++) {
            qtest_writeb(qts, REG_BASE + 0x90, page);
            for (unsigned byte = 0; byte < 32; byte++) {
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0 + byte), ==,
                                page * 7 + byte);
            }
        }
        qtest_quit(qts);
        g_assert_true(g_file_get_contents(path, &log, NULL, NULL));
        g_assert_nonnull(strstr(log, "unsupported register"));
        unlink(path);
        g_rmdir(dir);
    }
}

static void test_internal_scb_alias_queue_limits(void)
{
    for (unsigned scenario = 0; scenario < 4; scenario++) {
        QTestState *qts = start_aic();
        uint8_t program[136];
        size_t n = 0;

        qtest_writeb(qts, REG_BASE + 0x90, 255);
        if (scenario == 0) {
            qtest_writeb(qts, REG_BASE + 0x9b, 16);
        } else if (scenario == 1) {
            for (unsigned id = 0; id < 16; id++) {
                qtest_writeb(qts, REG_BASE + 0x9b, id);
            }
            qtest_writeb(qts, REG_BASE + 0x9b, 0);
        } else {
            if (scenario == 3) {
                for (unsigned id = 0; id < 16; id++) {
                    qtest_writeb(qts, REG_BASE + 0x9b, id);
                    emit(program, &n, alu_word(1, 0xff, 0x9b, 0x20, false));
                    emit(program, &n, alu_word(0, 0, 0x20, 0x9d, false));
                }
            }
            emit_or(program, &n, scenario == 2 ? 16 : 0, 0x9d);
            emit_or(program, &n, 1, 0x91);
            start_program(qts, program, n);
        }
        g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9c), ==,
                         scenario == 1 ? 16 : 0);
        g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9e), ==,
                         scenario == 3 ? 16 : 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x90), ==, 255);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
        if (scenario == 3) {
            for (unsigned id = 0; id < 16; id++) {
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, id);
            }
        }
        qtest_quit(qts);
    }
}

static void invert_scb_parity(QTestState *qts, unsigned page, unsigned offset)
{
    g_autofree char *fault = g_strdup_printf("invert:%u:%u", page, offset);

    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/hba',"
        "'property':'x-test-scb-parity','value':%s}}", fault);
}

static void test_external_scb_parity(void)
{
    QTestState *qts = start_external_scb(1024, 9, "");

    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    external_scb_write(qts, 0, 0, 0xa5);
    invert_scb_parity(qts, 32, 0);
    /* Neither enable alone checks the stored parity. */
    config_writel(qts, 0x40, 0x30);
    g_assert_cmphex(config_readl(qts, 0x40) & 0x630, ==, 0x630);
    g_assert_cmphex(external_scb_read(qts, 0, 0), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x20, ==, 0);
    config_writel(qts, 0x40, 0x20);
    qtest_writeb(qts, REG_BASE + 0x84, 0x20);
    external_scb_read(qts, 0, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x20, ==, 0);
    config_writel(qts, 0x40, 0x30);
    external_scb_read(qts, 0, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x20, ==, 0x20);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    g_assert_false(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0x87, 6);
    g_assert_true(qtest_get_irq(qts, 11));
    config_writew(qts, 4, 2);
    g_assert_false(qtest_get_irq(qts, 11));
    config_writew(qts, 4, 6);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0x92, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
    invert_scb_parity(qts, 0, 0);
    qtest_writeb(qts, REG_BASE + 0x92, 0x10);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x20, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 0);
    g_assert_false(qtest_get_irq(qts, 11));
    /* FAILDIS records the error without introducing a break or pause. */
    qtest_writeb(qts, REG_BASE + 0x60, 0xb1);
    resume_program(qts, 2);
    invert_scb_parity(qts, 0, 0);
    external_scb_read(qts, 0, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x20, ==, 0x20);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 4);
    external_scb_write(qts, 0, 0, 0xa5);
    qtest_writeb(qts, REG_BASE + 0x92, 0x10);
    external_scb_read(qts, 0, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x20, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(config_readl(qts, 0x40) & 0x30, ==, 0x30);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x84) & 0x20, ==, 0);
    qtest_system_reset(qts);
    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 4, 6);
    g_assert_cmphex(config_readl(qts, 0x40) & 0x630, ==, 0x600);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x84) & 0x20, ==, 0);
    qtest_quit(qts);
}

static void test_external_scb_parity_rmw(void)
{
    for (unsigned scenario = 0; scenario < 4; scenario++) {
        g_autofree char *dir = g_dir_make_tmp(
            "aic7880-parity-rmw-XXXXXX", NULL);
        g_autofree char *path = g_build_filename(dir, "qemu.log", NULL);
        g_autofree char *extra = g_strdup_printf("-d guest_errors -D %s", path);
        g_autofree char *log = NULL;
        QTestState *qts = start_external_scb(8192, 9, extra);
        uint8_t program[16];
        size_t n = 0;

        qtest_irq_intercept_in(qts, "ioapic");
        qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
        qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
        emit_or(program, &n, 0xe1, 0x9d);
        emit_or(program, &n, 1, 0x91);
        start_program(qts, program, n);
        qtest_writeb(qts, REG_BASE + 0x92, 1);
        qtest_writeb(qts, REG_BASE + 0x9b, 0x8c);
        external_scb_write(qts, 254, 31, 0x66);
        external_scb_write(qts, 255, 0, 0x22);
        external_scb_write(qts, 255, 31, 0x11);
        config_writel(qts, 0x40, 0x10);
        qtest_writeb(qts, REG_BASE + 0x84, 0x20);
        qtest_writeb(qts, REG_BASE + 0x20, 0xa5);
        if (scenario != 2) {
            invert_scb_parity(qts, 255, 31);
        }
        qtest_writeb(qts, REG_BASE + 0x9a, scenario == 3 ? 0 : 0x9f);
        n = 0;
        if (scenario == 3) {
            emit(program, &n, alu_word(1, 0xff, 0xbf, 0x20, false));
        } else {
            emit(program, &n, alu_word(0, 0x80, 0xa0, 0xa0, false));
            emit(program, &n, alu_word(1, 0xff, 0xa0, 0x20, false));
        }
        emit_or(program, &n, 1, 0x91);
        /* Upload while paused so FAILDIS is selected before execution. */
        qtest_writeb(qts, REG_BASE + 0x60, 0x91);
        qtest_writeb(qts, REG_BASE + 0x62, 0);
        for (size_t i = 0; i < n; i++) {
            qtest_writeb(qts, REG_BASE + 0x61, program[i]);
        }
        qtest_writeb(qts, REG_BASE + 0x60, scenario == 1 ? 0xb0 : 0x90);
        qtest_writeb(qts, REG_BASE + 0x62, 0);
        resume_program(qts, 2);
        if (scenario < 2) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9a), ==, 0x9f);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x90), ==, 255);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0xa5);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
            g_assert_false(qtest_get_irq(qts, 11));
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
            qtest_writeb(qts, REG_BASE + 0x92, 0x18);
            resume_program(qts, 2);
            qtest_clock_step(qts, 1000000);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0xa5);
        } else if (scenario == 2) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9a), ==, 0x81);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x22);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        } else {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9a), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0x20);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
            g_assert_true(qtest_get_irq(qts, 11));
            invert_scb_parity(qts, 255, 31);
            qtest_writeb(qts, REG_BASE + 0x92, 0x10);
            resume_program(qts, 2);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
        }
        g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);
        g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, 0xe1);
        qtest_writeb(qts, REG_BASE + 0x84, 0);
        qtest_writeb(qts, REG_BASE + 0x9a, 0);
        g_assert_cmphex(external_scb_read(qts, 255, 31), ==,
                        scenario == 2 ? 0x91 : 0x11);
        g_assert_cmphex(external_scb_read(qts, 255, 0), ==, 0x22);
        g_assert_cmphex(external_scb_read(qts, 254, 31), ==, 0x66);
        if (scenario < 2) {
            /* The refusal must not repair or rewrite the corrupted parity. */
            qtest_writeb(qts, REG_BASE + 0x60, 0x90);
            qtest_writeb(qts, REG_BASE + 0x84, 0x20);
            external_scb_read(qts, 255, 31);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x20, ==, 0x20);
            qtest_writeb(qts, REG_BASE + 0x87, 1);
            n = 0;
            emit_or(program, &n, 0x5a, 0x20);
            emit_or(program, &n, 1, 0x91);
            start_program(qts, program, n);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
        }
        qtest_quit(qts);
        g_assert_true(g_file_get_contents(path, &log, NULL, NULL));
        if (scenario < 2) {
            g_assert_nonnull(strstr(log,
                "parity-faulting automatic SCB read-modify-write"));
        } else {
            g_assert_cmpstr(log, ==, "");
        }
        unlink(path);
        g_rmdir(dir);
    }
}

static void test_external_scb_parity_holds_pause(void)
{
    QTestState *qts = start_external_scb(8192, 9, "");
    uint8_t program[8];
    size_t n = 0;

    external_scb_write(qts, 255, 31, 0x73);
    config_writel(qts, 0x40, 0x10);
    qtest_writeb(qts, REG_BASE + 0x84, 0x20);
    qtest_writeb(qts, REG_BASE + 0x20, 0xa5);
    emit_or(program, &n, 0x5a, 0x20);
    emit_or(program, &n, 1, 0x91);
    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    for (size_t i = 0; i < n; i++) {
        qtest_writeb(qts, REG_BASE + 0x61, program[i]);
    }
    qtest_writeb(qts, REG_BASE + 0x60, 0x90);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    invert_scb_parity(qts, 255, 31);
    external_scb_read(qts, 255, 31);
    resume_program(qts, 2);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 0);
    invert_scb_parity(qts, 255, 31);
    qtest_writeb(qts, REG_BASE + 0x92, 0x10);
    resume_program(qts, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
    qtest_quit(qts);
}

static void test_external_scb_parity_other_errors(void)
{
    static const uint8_t invalid_opcode[] = { 0, 0, 0, 0x0e };

    for (unsigned pci_error = 0; pci_error < 2; pci_error++) {
        QTestState *qts = start_external_scb(8192, 9, "");

        external_scb_write(qts, 255, 31, 0x73);
        config_writel(qts, 0x40, 0x10);
        qtest_writeb(qts, REG_BASE + 0x84, 0x20);
        if (pci_error) {
            qtest_qmp_assert_success(qts,
                "{'execute':'qom-set','arguments':{"
                "'path':'/machine/peripheral/hba','property':'x-test-pci-error',"
                "'value':'received-master-abort'}}");
        } else {
            start_program(qts, invalid_opcode, sizeof(invalid_opcode));
        }
        invert_scb_parity(qts, 255, 31);
        external_scb_read(qts, 255, 31);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==,
                        pci_error ? 0x60 : 0x24);
        invert_scb_parity(qts, 255, 31);
        qtest_writeb(qts, REG_BASE + 0x92, 0x10);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==,
                        pci_error ? 0x40 : 0x04);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
        if (pci_error) {
            g_assert_cmphex(config_readl(qts, 4) & 0x20000000, ==, 0x20000000);
        }
        qtest_quit(qts);
    }
}

static void test_external_scb_migrates(const void *opaque)
{
    g_autofree char *dir = g_dir_make_tmp("aic7880-scb-migrate-XXXXXX", NULL);
    g_autofree char *socket_path = g_strdup_printf("%s/migrate.sock", dir);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket_path);
    unsigned capacity = GPOINTER_TO_UINT(opaque);
    QTestState *from = start_external_scb(capacity, 9, "-S");
    QTestState *to = start_external_scb(capacity, 9, "-S -incoming defer");
    uint8_t program[16];
    size_t n = 0;

    external_scb_write(from, 255, 31, 0x73);
    external_scb_write(from, 255, 0, 0x64);
    external_scb_write(from, 128, 0, 0x55);
    invert_scb_parity(from, 255, 31);
    config_writel(from, 0x40, 0x30);
    qtest_writeb(from, REG_BASE + 0x84, 0x20);
    emit_or(program, &n, 0xfe, 0x9d);
    emit_or(program, &n, 0xff, 0x9d);
    emit_or(program, &n, 1, 0x91);
    start_program(from, program, n);
    qtest_writeb(from, REG_BASE + 0x9b, 0x80);
    qtest_writeb(from, REG_BASE + 0x9b, 0xff);
    qtest_writeb(from, REG_BASE + 0x90, 255);
    qtest_writeb(from, REG_BASE + 0x9a, 0x9f);
    qtest_readb(from, REG_BASE + 0xa0);
    qtest_writeb(from, REG_BASE + 0x9a, 0x9f);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(config_readl(to, 0x40) & 0x630, ==, 0x630);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x84), ==, 0x20);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x90), ==, 255);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9a), ==, 0x9f);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x92) & 0x20, ==, 0x20);
    qtest_writeb(to, REG_BASE + 0x92, 8);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x91) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0xa0), ==, 0x73);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x92) & 0x20, ==, 0x20);
    invert_scb_parity(to, 255, 31);
    qtest_writeb(to, REG_BASE + 0x92, 0x11);
    qtest_writeb(to, REG_BASE + 0x9a, 0);
    g_assert_cmphex(external_scb_read(to, 255, 0), ==, 0x64);
    g_assert_cmphex(external_scb_read(to, 128, 0), ==, 0x55);
    if (capacity == 1024) {
        g_assert_cmphex(external_scb_read(to, 31, 0), ==, 0x64);
        g_assert_cmphex(external_scb_read(to, 0, 0), ==, 0x55);
    }
    g_assert_cmpuint(qtest_readb(to, REG_BASE + 0x9e), ==, 2);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9d), ==, 0xfe);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9d), ==, 0xff);
    g_assert_cmpuint(qtest_readb(to, REG_BASE + 0x9c), ==, 2);
    n = 0;
    emit(program, &n, alu_word(1, 0xff, 0x9b, 0x20, false));
    emit(program, &n, alu_word(1, 0xff, 0x9b, 0x21, false));
    emit_or(program, &n, 1, 0x91);
    start_program(to, program, n);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x20), ==, 0x80);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x21), ==, 0xff);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket_path);
    g_rmdir(dir);
}

static void test_external_scb_reset(void)
{
    for (unsigned pci_reset = 0; pci_reset < 2; pci_reset++) {
        QTestState *qts = start_external_scb(1024, 9, "");
        uint8_t program[8];
        size_t n = 0;

        emit_or(program, &n, 0xff, 0x9d);
        emit_or(program, &n, 1, 0x91);
        start_program(qts, program, n);
        qtest_writeb(qts, REG_BASE + 0x9b, 0x80);
        qtest_writeb(qts, REG_BASE + 0x90, 0xff);
        qtest_writeb(qts, REG_BASE + 0x9a, 0x9f);
        g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);
        g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
        if (pci_reset) {
            qtest_system_reset(qts);
            config_writel(qts, 0x14, REG_BASE);
            config_writew(qts, 4, 6);
        } else {
            qtest_writeb(qts, REG_BASE + 0x87, 1);
        }
        g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9c), ==, 0);
        g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x90), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9a), ==, 0);
        external_scb_write(qts, 31, 0, 0x5a);
        g_assert_cmphex(external_scb_read(qts, 255, 0), ==, 0x5a);
        qtest_quit(qts);
    }
}

static void test_external_scb_migration_mismatch(void)
{
    for (unsigned kind = 0; kind < 2; kind++) {
        g_autofree char *dir = g_dir_make_tmp(
            "aic7880-scb-mismatch-XXXXXX", NULL);
        g_autofree char *socket = g_strdup_printf("%s/migrate.sock", dir);
        g_autofree char *uri = g_strdup_printf("unix:%s", socket);
        QTestState *from = start_external_scb(8192, 9, "-S");
        QTestState *to = start_external_scb(kind ? 8192 : 1024,
                                            kind ? 8 : 9,
                                            "-S -incoming defer");

        qtest_set_expected_status(to, EXIT_FAILURE);
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        qtest_qmp_assert_success(from,
            "{'execute':'migrate','arguments':{'uri':%s}}", uri);
        qtest_wait_qemu(to);
        qtest_quit(to);
        qtest_quit(from);
        unlink(socket);
        g_rmdir(dir);
    }
}

static void test_external_scb_configuration(void)
{
    static const char *invalid[] = {
        "'external-scb-ram':true",
        "'external-scb-ram':true,'external-scb-size':512",
        "'external-scb-ram':true,'external-scb-size':16384",
        "'external-scb-ram':true,'external-scb-size':1024,'external-scb-width':16",
        "'external-scb-ram':true,'external-scb-size':1024,'external-scb-shared':true",
        "'external-scb-size':1024",
        "'external-scb-width':9",
        "'external-scb-shared':true",
    };
    QTestState *qts = qtest_init("-M pc -m 128M -nodefaults");

    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
        g_autofree char *request = g_strdup_printf(
            "{'execute':'device_add','arguments':{'driver':'aic7880',"
            "'id':'invalid','bus':'pci.0','addr':'04.0',%s}}", invalid[i]);
        g_autoptr(QDict) command = qobject_to(
            QDict, qobject_from_json(request, &error_abort));
        g_autoptr(QDict) reply = qtest_qmp_assert_failure_ref(
            qts, "%p", g_steal_pointer(&command));

        g_assert_nonnull(strstr(qdict_get_str(reply, "desc"), "external"));
    }
    qtest_quit(qts);
    qts = start_aic();
    config_writel(qts, 0x40, 0x600);
    g_assert_cmphex(config_readl(qts, 0x40) & 0x600, ==, 0);
    qtest_quit(qts);
    qts = start_external_scb(1024, 8, "");
    external_scb_write(qts, 0, 0, 0x43);
    config_writel(qts, 0x40, 0x80);
    g_assert_cmphex(config_readl(qts, 0x40) & 0x600, ==, 0x600);
    g_assert_cmphex(external_scb_read(qts, 32, 0), ==, 0x43);
    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    resume_program(qts, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 0);
    config_writel(qts, 0x40, 0x10);
    g_assert_cmphex(config_readl(qts, 0x40) & 0x10, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    qtest_quit(qts);
    qts = start_external_scb(1024, 9, "");
    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    resume_program(qts, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 0);
    config_writel(qts, 0x40, 0x10000);
    g_assert_cmphex(config_readl(qts, 0x40) & 0x10000, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    qtest_quit(qts);
}

static void test_external_scb_widths(void)
{
    for (unsigned mode = 0; mode < 5; mode++) {
        QTestState *qts = start_external_scb(8192, 8, MEMTX_TEST_DEVICE);
        uint64_t ignored;

        external_scb_write(qts, 255, 0, 0x12);
        external_scb_write(qts, 255, 1, 0x34);
        external_scb_write(qts, 255, 2, 0x56);
        external_scb_write(qts, 255, 3, 0x78);
        qtest_writeb(qts, REG_BASE + 0x9a, mode == 2 ? 0 : 0x80);
        if (mode == 0) {
            g_assert_cmpint(memtx_write(qts, REG_BASE + 0xa0, 0xbeef, 2),
                            !=, MEMTX_OK);
        } else if (mode == 1) {
            g_assert_cmpint(memtx_read(qts, REG_BASE + 0xa0, &ignored, 4),
                            !=, MEMTX_OK);
        } else {
            g_assert_cmpint(memtx_write(qts, REG_BASE + 0xa0 +
                                       (mode == 3 ? 1 : mode == 4 ? 31 : 0),
                                       0xdeadbeef, 4), !=, MEMTX_OK);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9a), ==,
                        mode == 2 ? 0 : 0x80);
        qtest_writeb(qts, REG_BASE + 0x9a, 0);
        g_assert_cmphex(external_scb_read(qts, 255, 0), ==, 0x12);
        g_assert_cmphex(external_scb_read(qts, 255, 1), ==, 0x34);
        g_assert_cmphex(external_scb_read(qts, 255, 2), ==, 0x56);
        g_assert_cmphex(external_scb_read(qts, 255, 3), ==, 0x78);
        qtest_quit(qts);
    }
}

static void test_external_scb_auto(void)
{
    QTestState *qts = start_external_scb(8192, 8, "");
    uint8_t program[16];
    size_t n = 0;

    external_scb_write(qts, 0, 0, 0x56);
    external_scb_write(qts, 255, 30, 0x78);
    external_scb_write(qts, 255, 3, 0x9a);
    qtest_writeb(qts, REG_BASE + 0x9a, 0xff);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9a), ==, 0x9f);
    qtest_writel(qts, REG_BASE + 0xa0, 0x44332211);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9a), ==, 0x83);
    qtest_writeb(qts, REG_BASE + 0x9a, 0);
    g_assert_cmphex(external_scb_read(qts, 255, 31), ==, 0x11);
    g_assert_cmphex(external_scb_read(qts, 255, 0), ==, 0x22);
    g_assert_cmphex(external_scb_read(qts, 255, 1), ==, 0x33);
    g_assert_cmphex(external_scb_read(qts, 255, 2), ==, 0x44);
    g_assert_cmphex(external_scb_read(qts, 255, 3), ==, 0x9a);
    g_assert_cmphex(external_scb_read(qts, 255, 30), ==, 0x78);
    g_assert_cmphex(external_scb_read(qts, 0, 0), ==, 0x56);
    config_writel(qts, 0x10, 0x1001);
    config_writew(qts, 4, 7);
    qtest_outb(qts, 0x1090, 255);
    qtest_outb(qts, 0x109a, 0x9f);
    qtest_outl(qts, 0x10a0, 0x44332211);
    g_assert_cmphex(qtest_inb(qts, 0x109a), ==, 0x83);
    qtest_outb(qts, 0x109a, 0x9f);
    g_assert_cmphex(qtest_inb(qts, 0x10a0), ==, 0x11);
    g_assert_cmphex(qtest_inb(qts, 0x10bf), ==, 0x22);
    g_assert_cmphex(qtest_inb(qts, 0x109a), ==, 0x81);
    qtest_writeb(qts, REG_BASE + 0x9a, 0x9f);
    /* A sequencer RMW owns byte 31 and advances only once. */
    emit(program, &n, alu_word(0, 0x80, 0xa0, 0xa0, false));
    emit(program, &n, alu_word(1, 0xff, 0xa0, 0x20, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x22);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9a), ==, 0x81);
    qtest_writeb(qts, REG_BASE + 0x9a, 0);
    g_assert_cmphex(external_scb_read(qts, 255, 31), ==, 0x91);
    g_assert_cmphex(external_scb_read(qts, 255, 0), ==, 0x22);
    g_assert_cmphex(external_scb_read(qts, 255, 30), ==, 0x78);
    qtest_quit(qts);
}

static void test_external_scb_queues(void)
{
    static const unsigned sizes[] = { 1024, 8192 };

    for (unsigned size = 0; size < ARRAY_SIZE(sizes); size++) {
        QTestState *qts = start_external_scb(sizes[size], 8, "");
        uint8_t program[2048];
        size_t n = 0;

        for (unsigned i = 0; i < 255; i++) {
            qtest_writeb(qts, REG_BASE + 0x9b, 0x80 + i);
            emit(program, &n, alu_word(1, 0xff, 0x9b, 0x20, false));
            emit(program, &n, alu_word(0, 0, 0x20, 0x9d, false));
        }
        g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9c), ==, 255);
        qtest_writeb(qts, REG_BASE + 0x9b, 0x42);
        g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9c), ==, 255);
        emit_or(program, &n, 0x42, 0x9d);
        emit_or(program, &n, 1, 0x91);
        start_program(qts, program, n);
        for (unsigned i = 0; i < 10; i++) {
            qtest_clock_step(qts, 1000000);
        }
        g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9c), ==, 0);
        g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9e), ==, 255);
        for (unsigned i = 0; i < 255; i++) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==,
                            (uint8_t)(0x80 + i));
        }
        qtest_readb(qts, REG_BASE + 0x9d);
        g_assert_cmpuint(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        qtest_writeb(qts, REG_BASE + 0x92, 1);
        qtest_writeb(qts, REG_BASE + 0x9b, 0xfe);
        qtest_writeb(qts, REG_BASE + 0x9b, 0xff);
        n = 0;
        for (unsigned i = 0; i < 2; i++) {
            emit(program, &n, alu_word(1, 0xff, 0x9b, 0x20, false));
            emit(program, &n, alu_word(0, 0, 0x20, 0x9d, false));
        }
        /* An empty sequencer read must leave the controller running. */
        emit(program, &n, alu_word(1, 0xff, 0x9b, 0x20, false));
        emit_or(program, &n, 1, 0x91);
        start_program(qts, program, n);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, 0xfe);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, 0xff);
        qtest_quit(qts);
    }
}

static size_t build_selected_inquiry_identify(uint8_t *program,
                                             uint8_t identify)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };
    size_t n = 0;

    emit_or(program, &n, 0x08, 0x01);
    emit_or(program, &n, 0x07, 0x05);
    emit_or(program, &n, 0, 0x93);
    emit_or(program, &n, 0xa0, 0x03);
    emit_or(program, &n, 0x5a, 0x00);
    emit_or(program, &n, identify, 0x06);
    emit_or(program, &n, 0x40, 0x0c);
    emit_or(program, &n, 0x80, 0x03);
    for (size_t i = 0; i < sizeof(cdb); i++) {
        if (cdb[i]) {
            emit_or(program, &n, cdb[i], 0x06);
        } else {
            emit(program, &n, alu_word(1, 0xff, 0x6a, 0x06, false));
        }
    }
    emit_or(program, &n, 0x30, 0x93);
    emit_or(program, &n, 1, 0x91);
    return n;
}

static size_t build_selected_inquiry_program(uint8_t *program)
{
    return build_selected_inquiry_identify(program, 0x80);
}

static void test_workload_upload(size_t bytes, uint8_t first, uint8_t last)
{
    size_t slots = bytes / 4;
    g_autofree uint8_t *program = g_new(uint8_t, bytes);
    QTestState *qts = start_aic();
    size_t n = 0;

    g_assert_cmpuint(bytes % 4, ==, 0);
    g_assert_cmpuint(slots, <, 512);
    for (size_t slot = 0; slot < slots; slot++) {
        uint32_t word = alu_word(1, 0xff, 0x6a, 0x6a, false);

        if (slot == 0) {
            word = alu_word(0, first, 0x6a, 0x20, false);
        } else if (slot == slots - 1) {
            word = alu_word(0, last, 0x6a, 0x21, false);
        }
        emit(program, &n, word);
    }
    g_assert_cmpuint(n, ==, bytes);
    qtest_writeb(qts, REG_BASE + 0x20, 0xa5);
    qtest_writeb(qts, REG_BASE + 0x21, 0x5a);
    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x63, 0);
    for (size_t i = 0; i < bytes; i++) {
        qtest_writeb(qts, REG_BASE + 0x61, program[i]);
        if (i == 1023) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x63), ==, 1);
        }
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, slots & 0xff);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x63), ==, slots >> 8);

    qtest_writeb(qts, REG_BASE + 0x60, 0x90);
    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x63, 0);
    for (size_t i = 0; i < bytes; i++) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x61), ==, program[i]);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, slots & 0xff);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x63), ==, slots >> 8);

    qtest_writeb(qts, REG_BASE + 0x60, 0x94);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x63, 0);
    step_instruction(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, first);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x63), ==, 0);

    qtest_writeb(qts, REG_BASE + 0x62, (slots - 1) & 0xff);
    qtest_writeb(qts, REG_BASE + 0x63, (slots - 1) >> 8);
    step_instruction(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, first);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, last);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, slots & 0xff);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x63), ==, slots >> 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 0);
    qtest_quit(qts);
}

static void test_workload_upload_1696(void)
{
    test_workload_upload(1696, 0x31, 0x73);
}

static void test_workload_upload_1708(void)
{
    test_workload_upload(1708, 0x42, 0x85);
}

static void workload_wait_hdma_disabled(QTestState *qts)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (!(qtest_readb(qts, REG_BASE + 0x93) & 8)) {
            return;
        }
    }
    g_assert_not_reached();
}

static void test_workload_dma_scb_branch(uint8_t tail)
{
    const uint32_t first_addr = 0x10008;
    const uint32_t second_addr = 0x10208;
    static const uint8_t first_bytes[4] = { 0x11, 0x37, 0x59, 0x7b };
    uint8_t first[20], second[44], got_first[20], got_second[44];
    uint8_t expected[32], program[48 * 4] = { 0 };
    QTestState *qts = start_aic();
    size_t branch_at, jump_at, set_pc, common_pc, n = 0;
    uint8_t source_canary = tail == 0x20 ? 0x76 : 0x8b;
    uint8_t changed_dest = tail == 0x20 ? 0x22 : 0x23;
    uint8_t other_dest = tail == 0x20 ? 0x23 : 0x22;

    memset(first, 0xe7, sizeof(first));
    memset(second, 0xd6, sizeof(second));
    memcpy(first + 8, first_bytes, sizeof(first_bytes));
    for (unsigned i = 0; i < 28; i++) {
        second[8 + i] = 0x41 + 7 * i;
    }
    second[8 + 3] = tail;
    memcpy(expected, first + 8, 4);
    memcpy(expected + 4, second + 8, 28);
    qtest_memwrite(qts, first_addr - 8, first, sizeof(first));
    qtest_memwrite(qts, second_addr - 8, second, sizeof(second));

    for (unsigned i = 0; i < 4; i++) {
        emit(program, &n, alu_word(1, 0xff, 0x99, 0xa0, false));
    }
    emit_or(program, &n, 1, 0x91);
    for (unsigned i = 0; i < 28; i++) {
        emit(program, &n, alu_word(1, 0xff, 0x99, 0xa0, false));
    }
    emit_or(program, &n, 1, 0x91);
    branch_at = n;
    emit(program, &n, branch_word(0xd, 1, 0xa7, 0));
    emit_or(program, &n, 0x20, 0x65);
    emit_or(program, &n, 0x22, 0x66);
    jump_at = n;
    emit(program, &n, branch_word(8, 0, 0x65, 0));
    set_pc = n / 4;
    emit_or(program, &n, 0x21, 0x65);
    emit_or(program, &n, 0x23, 0x66);
    common_pc = n / 4;
    emit(program, &n, alu_word(1, 0xff, 0x6c, 0x24, false));
    emit_or(program, &n, 0xa5, 0x6d);
    emit_or(program, &n, 1, 0x91);
    stl_le_p(program + branch_at, branch_word(0xd, 1, 0xa7, set_pc));
    stl_le_p(program + jump_at, branch_word(8, 0, 0x65, common_pc));
    g_assert_cmpuint(n, <=, sizeof(program));

    qtest_writeb(qts, REG_BASE + 0x90, 3);
    qtest_writeb(qts, REG_BASE + 0x9a, 0x80);
    qtest_writeb(qts, REG_BASE + 0x20, 0x76);
    qtest_writeb(qts, REG_BASE + 0x21, 0x8b);
    qtest_writeb(qts, REG_BASE + 0x22, 0x3c);
    qtest_writeb(qts, REG_BASE + 0x23, 0xc3);
    qtest_writeb(qts, REG_BASE + 0x24, 0xd2);
    qtest_writeb(qts, REG_BASE + 0x25, 0x6d);
    qtest_writeb(qts, REG_BASE + 0x65, 0x25);
    qtest_writeb(qts, REG_BASE + 0x66, 0x26);

    host_dma_program(qts, first_addr, 4);
    qtest_writeb(qts, REG_BASE + 0x93, 0x05);
    qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 8);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, first_addr + 4);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 0x04);
    workload_wait_hdma_disabled(qts);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9a), ==, 0x84);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x25);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0x26);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);

    host_dma_program(qts, second_addr, 28);
    qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 8);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, second_addr + 28);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 0x04);
    workload_wait_hdma_disabled(qts);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    resume_program(qts, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 34);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9a), ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
    qtest_memread(qts, first_addr - 8, got_first, sizeof(got_first));
    qtest_memread(qts, second_addr - 8, got_second, sizeof(got_second));
    g_assert_cmpmem(got_first, sizeof(got_first), first, sizeof(first));
    g_assert_cmpmem(got_second, sizeof(got_second), second, sizeof(second));

    qtest_writeb(qts, REG_BASE + 0x9a, 0);
    for (unsigned i = 0; i < sizeof(expected); i++) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0 + i), ==,
                        expected[i]);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa7), ==, tail);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    resume_program(qts, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x76);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, 0x8b);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x24), ==, source_canary);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + changed_dest), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + other_dest), ==,
                    tail == 0x20 ? 0xc3 : 0x3c);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x25), ==, 0x6d);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==,
                    tail == 0x20 ? 0x21 : 0x22);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==,
                    tail == 0x20 ? 0x23 : 0x24);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x42, ==, 0);
    qtest_quit(qts);
}

static void test_workload_dma_scb_branch_clear(void)
{
    test_workload_dma_scb_branch(0x20);
}

static void test_workload_dma_scb_branch_set(void)
{
    test_workload_dma_scb_branch(0x21);
}

static void test_internal_refusal_keeps_pci_status_clear(void)
{
    enum {
        HOST_WRITE,
        HOST_READ,
        HOST_WORD_READ,
        HOST_WORD_WRITE,
        SEQ_READ,
        SEQ_WRITE,
        SEQ_RETURN,
    };
    static const struct RefusalCase {
        unsigned kind;
        uint8_t reg;
        uint8_t value;
        const char *diagnostic;
    } cases[] = {
        { HOST_WRITE, 2, 0x40, "unsupported SXFRCTL1" },
        { HOST_READ, 0x6d, 0, "host read of DINDIR" },
        { HOST_READ, 0x99, 0, "DFDAT read from empty FIFO" },
        { HOST_WRITE, 0x9b, 0x5a, "unsupported queue-in entry" },
        { HOST_WRITE, 0x93, 0x40, "unsupported DFCNTRL" },
        { HOST_WORD_READ, 0x86, 0, "unsupported DSPCISTATUS width" },
        { HOST_WORD_WRITE, 0x86, 0xbf, "unsupported DSPCISTATUS width" },
        { SEQ_READ, 0x92, 0, "host-only register" },
        { SEQ_READ, 0x91, 0, "host-only register" },
        { SEQ_READ, 0x7e, 0, "read of unsupported register" },
        { SEQ_READ, 0x9b, 0, "empty queue-in FIFO" },
        { SEQ_WRITE, 0x87, 1, "host-only HCNTRL" },
        { SEQ_RETURN, 0x20, 1, "return without call" },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        const struct RefusalCase *test = &cases[i];
        g_autofree char *path = NULL;
        g_autofree char *log = NULL;
        uint8_t program[8];
        QTestState *qts;
        size_t n = 0;
        int fd = g_file_open_tmp("aic7880-refusal-XXXXXX", &path, NULL);

        g_assert_cmpint(fd, >=, 0);
        close(fd);
        qts = start_aic_logged(path);
        qtest_irq_intercept_in(qts, "ioapic");
        qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
        qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
        qtest_writeb(qts, REG_BASE + 0x87, 6);
        qtest_writeb(qts, REG_BASE + 0x20, 0x5a);
        qtest_writeb(qts, REG_BASE + 0x41, 0xa5);
        qtest_writeb(qts, REG_BASE + 0x86, 0x80);
        host_dma_program(qts, 0x10000, 8);
        if (test->kind == HOST_WRITE) {
            qtest_writeb(qts, REG_BASE + test->reg, test->value);
        } else if (test->kind == HOST_READ) {
            qtest_readb(qts, REG_BASE + test->reg);
        } else if (test->kind == HOST_WORD_READ) {
            qtest_readw(qts, REG_BASE + test->reg);
        } else if (test->kind == HOST_WORD_WRITE) {
            qtest_writew(qts, REG_BASE + test->reg, 0xbf00 | test->value);
        } else {
            if (test->kind == SEQ_READ) {
                emit(program, &n, alu_word(0, 1, test->reg, 0x20, false));
            } else {
                emit(program, &n, alu_word(0, test->value, 0x6a, test->reg,
                                           test->kind == SEQ_RETURN));
            }
            emit_or(program, &n, 0x33, 0x41);
            start_program(qts, program, n);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
        g_assert_cmphex(config_readl(qts, 4) & 0xf9000000u, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0xa5);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86), ==, 0x80);
        g_assert_false(qtest_get_irq(qts, 11));
        qtest_writeb(qts, REG_BASE + 0x92, 8);
        qtest_writeb(qts, REG_BASE + 0x93, 0x0d);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x10000);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 8);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
        qtest_quit(qts);
        g_assert_true(g_file_get_contents(path, &log, NULL, NULL));
        g_assert_nonnull(strstr(log, test->diagnostic));
        g_assert_cmpint(g_unlink(path), ==, 0);
    }
}

static void test_pci_error_status_survives_chip_reset(void)
{
    QTestState *qts = start_aic();

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x86, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86), ==, 0x80);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/hba','property':'x-test-pci-error',"
        "'value':'received-master-abort'}}");
    g_assert_cmphex(config_readl(qts, 0x04) >> 16 & 0x2000, ==, 0x2000);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86), ==, 0x84);
    qtest_writeb(qts, REG_BASE + 0x86, 0xbf);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86), ==, 0x84);
    qtest_writeb(qts, REG_BASE + 0x92, 0x1f);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0x40);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86), ==, 4);
    g_assert_cmphex(config_readl(qts, 0x04) >> 16 & 0x2000, ==, 0x2000);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0x40);
    config_writew(qts, 0x06, 0x2000);
    g_assert_cmphex(config_readl(qts, 0x04) >> 16 & 0x2000, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86), ==, 0);
    qtest_quit(qts);
}

static void test_host_faildis_readback(void)
{
    for (unsigned perrordis = 0; perrordis < 2; perrordis++) {
        g_autofree char *dir = g_dir_make_tmp("aic7880-faildis-XXXXXX", NULL);
        g_autofree char *path = g_build_filename(dir, "qemu.log", NULL);
        QTestState *qts = start_aic_logged(path);
        uint8_t program[4];
        uint8_t control = 0x30 | (perrordis << 7);
        size_t n = 0;

        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x60), ==, 0x90);
        qtest_writeb(qts, REG_BASE + 0x60, control | 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x60), ==, control);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);
        qtest_writeb(qts, REG_BASE + 0x60, control | 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x60), ==, control | 1);
        emit(program, &n, alu_word(1, 0xff, 0x6a, 0x24, false));
        for (unsigned byte = 0; byte < sizeof(program); byte++) {
            qtest_writeb(qts, REG_BASE + 0x61, program[byte]);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
        qtest_writeb(qts, REG_BASE + 0x60, control);
        qtest_writeb(qts, REG_BASE + 0x60, control | 1);
        qtest_writeb(qts, REG_BASE + 0x62, 0);
        qtest_writeb(qts, REG_BASE + 0x63, 0);
        for (unsigned byte = 0; byte < sizeof(program); byte++) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x61), ==,
                            program[byte]);
        }
        qtest_writeb(qts, REG_BASE + 0x60, control);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x60), ==, control);
        qtest_writeb(qts, REG_BASE + 0x60, control | 0x0c);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x60), ==, control | 0x0c);
        qtest_writeb(qts, REG_BASE + 0x60, control & ~0x20);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x60), ==, control & ~0x20);
        qtest_writeb(qts, REG_BASE + 0x60, control);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        qtest_writeb(qts, REG_BASE + 0x87, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x60), ==, 0x90);
        qtest_writeb(qts, REG_BASE + 0x60, control);
        qobject_unref(qtest_qmp(qts, "{'execute':'system_reset'}"));
        qtest_qmp_eventwait(qts, "RESET");
        config_writel(qts, 0x14, REG_BASE);
        config_writew(qts, 0x04, 0x0006);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x60), ==, 0x90);
        qtest_quit(qts);
        assert_no_aic_log(path);
        g_assert_cmpint(g_unlink(path), ==, 0);
        g_assert_cmpint(g_rmdir(dir), ==, 0);
    }
}

static void test_pci_error_faildis(void)
{
    for (unsigned faildis = 0; faildis < 2; faildis++) {
        for (unsigned brkinten = 0; brkinten < 2; brkinten++) {
            QTestState *qts = start_aic();
            uint8_t program[4];
            size_t n = 0;

            qtest_irq_intercept_in(qts, "ioapic");
            qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
            qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
            emit(program, &n, branch_word(8, 0, 0x65, 0));
            start_program(qts, program, n);
            qtest_writeb(qts, REG_BASE + 0x87, 6);
            qtest_writeb(qts, REG_BASE + 0x60,
                         0x90 | (faildis << 5) | (brkinten << 3));
            resume_program(qts, 2);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
            g_assert_false(qtest_get_irq(qts, 11));
            qtest_qmp_assert_success(qts,
                "{'execute':'qom-set','arguments':{"
                "'path':'/machine/peripheral/hba','property':'x-test-pci-error',"
                "'value':'received-master-abort'}}");
            g_assert_cmphex(config_readl(qts, 0x04) >> 16 & 0x2000, ==,
                            0x2000);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0x40);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86), ==, 4);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==,
                            faildis ? 0 : 8);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==,
                            faildis ? 0 : 4);
            g_assert_cmpint(qtest_get_irq(qts, 11), ==, !faildis && brkinten);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03), ==, 0);
            qtest_writeb(qts, REG_BASE + 0x92, 8);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
            g_assert_false(qtest_get_irq(qts, 11));
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0x40);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86), ==, 4);
            config_writew(qts, 0x06, 0x2000);
            g_assert_cmphex(config_readl(qts, 0x04) >> 16 & 0x2000, ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86), ==, 0);
            qtest_quit(qts);
        }
    }
}

static void test_host_pausedis_refusal(void)
{
    static const uint8_t controls[] = { 0x40, 0x60 };

    for (unsigned faildis = 0; faildis < 2; faildis++) {
        for (unsigned i = 0; i < ARRAY_SIZE(controls); i++) {
            g_autofree char *dir = g_dir_make_tmp(
                "aic7880-seqctl-XXXXXX", NULL);
            g_autofree char *path = g_build_filename(dir, "qemu.log", NULL);
            g_autofree char *log = NULL;
            QTestState *qts = start_aic_logged(path);
            uint8_t program[8];
            uint8_t control = 0x90 | (faildis << 5);
            size_t n = 0;

            emit(program, &n, branch_word(8, 0, 0x65, 0));
            start_program(qts, program, n);
            qtest_writeb(qts, REG_BASE + 0x87, 4);
            qtest_writeb(qts, REG_BASE + 0x60, control);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x60), ==, control);
            qtest_writeb(qts, REG_BASE + 0x60, control | controls[i] | 4);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x60), ==, control);
            resume_program(qts, 2);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
            qtest_writeb(qts, REG_BASE + 0x87, 4);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
            qtest_writeb(qts, REG_BASE + 0x87, 1);
            n = 0;
            emit_or(program, &n, 0x5a, 0x20);
            emit_or(program, &n, 1, 0x91);
            start_program(qts, program, n);
            wait_paused(qts);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
            qtest_quit(qts);
            g_assert_true(g_file_get_contents(path, &log, NULL, NULL));
            g_assert_nonnull(strstr(log, "unsupported host SEQCTL control"));
            g_assert_cmpint(g_unlink(path), ==, 0);
            g_assert_cmpint(g_rmdir(dir), ==, 0);
        }
    }
}

static void test_internal_completion_ids(void)
{
    for (unsigned id = 0; id < 16; id++) {
        uint8_t program[8];
        QTestState *qts = start_aic();
        size_t n = 0;

        emit(program, &n, alu_word(1, id, 0x69, 0x9d, false));
        emit_or(program, &n, 1, 0x91);
        start_program(qts, program, n);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, id);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        qtest_quit(qts);
    }
    for (unsigned indirect = 0; indirect < 2; indirect++) {
        uint8_t program[8];
        QTestState *qts = start_aic();
        size_t n = 0;

        qtest_writeb(qts, REG_BASE + 0x66, 0x9d);
        emit_or(program, &n, 0x5a, indirect ? 0x6d : 0x9d);
        emit_or(program, &n, 2, 0x91);
        start_program(qts, program, n);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0x9d);
        qtest_quit(qts);
    }
}

static void test_diagnostic_unpause_cannot_complete(void)
{
    for (unsigned dma = 0; dma < 2; dma++) {
        QTestState *qts = start_aic();
        uint8_t program[12];
        size_t n = 0;

        if (dma) {
            host_dma_program(qts, 0x70000000, 1);
            qtest_writeb(qts, REG_BASE + 0x99, 0x5a);
        }
        emit_or(program, &n, dma ? 0x0a : 1, dma ? 0x93 : 0x07);
        emit_or(program, &n, 1, 0x9d);
        emit_or(program, &n, 2, 0x91);
        start_program(qts, program, n);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
        qtest_writeb(qts, REG_BASE + 0x92, 0x0f);
        resume_program(qts, 2);
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        if (dma) {
            g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x70000000);
            g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 1);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==, 0x5a);
        }
        qtest_quit(qts);
    }
}

static void test_deferred_dma_preserves_registers(void)
{
    QTestState *qts = start_aic();
    uint8_t program[16];
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x20, 0xa5);
    qtest_writeb(qts, REG_BASE + 0x21, 0);
    host_dma_program(qts, REG_BASE + 0x20, 1);
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    qtest_writeb(qts, REG_BASE + 0x99, 0x5a);
    emit(program, &n, branch_word(0x0f, 1, 0x21, 0));
    emit_or(program, &n, 0x0a, 0x93);
    emit_or(program, &n, 3, 0x9d);
    emit_or(program, &n, 2, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, REG_BASE + 0x20);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    /* Inspection wakes the sequencer. Let the unchanged condition park it. */
    qtest_clock_step(qts, 1);
    qtest_writeb(qts, REG_BASE + 0x21, 1);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0xa5);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, REG_BASE + 0x20);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==, 0x5a);
    qtest_quit(qts);
}

static void test_sequencer_host_completion_memory(void)
{
    QTestState *qts = start_aic();
    uint8_t program[128], expected[272], got[272];
    size_t n = 0;
    unsigned dma_pc, qout_pc, interrupt_pc;

    /* Scratch holds an arbitrary byte-array base, index and identifier. */
    qtest_writeb(qts, REG_BASE + 0x20, 4);
    qtest_writeb(qts, REG_BASE + 0x21, 0x20);
    qtest_writeb(qts, REG_BASE + 0x22, 1);
    qtest_writeb(qts, REG_BASE + 0x23, 0);
    qtest_writeb(qts, REG_BASE + 0x24, 0xff);
    memset(expected, 0xa5, sizeof(expected));
    qtest_memwrite(qts, 0x12000, expected, sizeof(expected));
    emit_or(program, &n, 1, 0x93); /* Empty the host FIFO. */
    emit(program, &n, alu_word(1, 0xff, 0x24, 0x64, false));
    emit(program, &n, alu_word(3, 0, 0x20, 0x88, false));
    emit(program, &n, alu_word(2, 0, 0x64, 0x64, false));
    for (unsigned i = 1; i < 4; i++) {
        emit(program, &n, alu_word(4, 0, 0x20 + i, 0x88 + i, false));
    }
    emit_or(program, &n, 1, 0x8c);
    emit_or(program, &n, 0, 0x8d);
    emit_or(program, &n, 0, 0x8e);
    emit(program, &n, alu_word(1, 0xff, 0x25, 0x99, false));
    dma_pc = n / 4;
    emit_or(program, &n, 0x0a, 0x93); /* Flush the one-byte tail to RAM. */
    emit(program, &n, branch_word(0x0f, 8, 0x94, n / 4));
    qout_pc = n / 4;
    emit(program, &n, alu_word(1, 0xff, 0x25, 0x9d, false));
    interrupt_pc = n / 4;
    emit_or(program, &n, 2, 0x91);
    emit(program, &n, alu_word(3, 1, 0x24, 0x24, false));

    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    for (size_t i = 0; i < n; i++) {
        qtest_writeb(qts, REG_BASE + 0x61, program[i]);
    }
    qtest_writeb(qts, REG_BASE + 0x60, 0x94);
    for (unsigned completion = 0; completion < 3; completion++) {
        unsigned index = (completion + 255) & 0xff;
        uint8_t id = 5 + completion;

        qtest_writeb(qts, REG_BASE + 0x25, id);
        qtest_writeb(qts, REG_BASE + 0x62, 0);
        qtest_writeb(qts, REG_BASE + 0x63, 0);
        for (unsigned pc = 0; pc < n / 4; pc++) {
            if (pc == dma_pc) {
                qtest_memread(qts, 0x12000, got, sizeof(got));
                g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
            }
            step_instruction(qts);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, pc + 1);
            if (pc == dma_pc) {
                expected[4 + index] = id;
                qtest_memread(qts, 0x12000, got, sizeof(got));
                g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8,
                                ==, 8);
            }
            if (pc == qout_pc) {
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
            }
            if (pc == interrupt_pc) {
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 2);
                qtest_writeb(qts, REG_BASE + 0x92, 2);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, id);
            }
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x24),
                        ==, (index + 1) & 0xff);
    }
    /* With PCI admission inhibited, the firmware waits before publishing. */
    config_writew(qts, 0x04, 2);
    qtest_writeb(qts, REG_BASE + 0x25, 8);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x63, 0);
    for (unsigned pc = 0; pc <= dma_pc + 1; pc++) {
        step_instruction(qts);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, dma_pc + 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    qtest_memread(qts, 0x12000, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    config_writew(qts, 0x04, 6);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    qtest_memread(qts, 0x12000, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
    qtest_quit(qts);
}

static void test_workload_host_only_negative(void)
{
    uint8_t program[8];
    g_autofree char *dir = g_dir_make_tmp("aic7880-intstat-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "qemu.log", NULL);
    g_autofree char *log = NULL;
    QTestState *qts = start_aic_logged(path);
    uint32_t pci_status = config_readl(qts, 0x04);
    size_t n = 0;

    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    qtest_writeb(qts, REG_BASE + 0x65, 0x91);
    qtest_writeb(qts, REG_BASE + 0x25, 0xa5);
    emit(program, &n, alu_word(1, 0xff, 0x6c, 0x25, false));
    emit_or(program, &n, 1, 0x91);
    qtest_writeb(qts, REG_BASE + 0x60, 0x99);
    for (size_t i = 0; i < n; i++) {
        qtest_writeb(qts, REG_BASE + 0x61, program[i]);
    }
    qtest_writeb(qts, REG_BASE + 0x60, 0x98);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x63, 0);
    resume_program(qts, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x25), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 6, ==, 6);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(config_readl(qts, 0x04), ==, pci_status);
    g_assert_false(qtest_get_irq(qts, 11));
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "unsupported sequencer read at pc 0x000 "
                               "of host-only register 0x91"));
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void attention_select(QTestState *qts, uint8_t control)
{
    uint8_t program[16] = { 0 };
    size_t n = 0;

    emit_or(program, &n, 0x07, 0x05);
    emit_or(program, &n, control & 8 ? 0xa0 : 0x80, 0x03);
    emit_or(program, &n, control, 0x00);
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
}

static void attention_write_raw(QTestState *qts, uint8_t reg, uint8_t value)
{
    uint8_t program[8] = { 0 };
    size_t n = 0;

    if (value) {
        emit_or(program, &n, value, reg);
    } else {
        emit(program, &n, alu_word(1, 0xff, 0x6a, reg, false));
    }
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
}

static void attention_write(QTestState *qts, uint8_t reg, uint8_t value)
{
    if (reg == 0x06) {
        uint8_t phase = qtest_readb(qts, REG_BASE + 0x03) & 0xe0;

        if (phase == 0x80 || phase == 0xa0) {
            qtest_writeb(qts, REG_BASE + 0x03, phase);
        }
    }
    attention_write_raw(qts, reg, value);
}

static void attention_complete_test_unit_ready(QTestState *qts)
{
    uint8_t command[8 * 4] = { 0 };
    uint8_t finish[7 * 4] = { 0 };
    size_t n = 0;

    emit_or(command, &n, 0x80, 0x03);
    for (unsigned i = 0; i < 6; i++) {
        emit(command, &n, alu_word(1, 0xff, 0x6a, 0x06, false));
    }
    emit_or(command, &n, 1, 0x91);
    start_program(qts, command, n);
    for (unsigned i = 0; i < 1000; i++) {
        if ((qtest_readb(qts, REG_BASE + 0x03) & 0xe0) == 0xc0) {
            break;
        }
        qtest_clock_step(qts, 1000);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0xc0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    n = 0;
    emit_or(finish, &n, 0xc0, 0x03);
    emit(finish, &n, alu_word(0, 0, 0x06, 0x22, false));
    emit_or(finish, &n, 0xe0, 0x03);
    emit(finish, &n, alu_word(0, 0, 0x06, 0x23, false));
    emit_or(finish, &n, 1, 0x9d);
    emit_or(finish, &n, 2, 0x91);
    emit_or(finish, &n, 1, 0x91);
    start_program(qts, finish, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x23), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 2);
}

static void test_reserved_host_count_sequencer_reads(void)
{
    uint8_t program[8] = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-reserved-read-XXXXXX",
                                        &error);
    g_autofree char *log_path = NULL;
    QTestState *qts;
    uint8_t status;
    size_t n = 0;

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    qts = start_aic_logged(log_path);
    host_dma_program(qts, 0x10000, 3);
    status = qtest_readb(qts, REG_BASE + 0x94);
    qtest_writeb(qts, REG_BASE + 0x20, 0xa5);
    qtest_writeb(qts, REG_BASE + 0x65, 0x8f);
    emit(program, &n, alu_word(1, 0xff, 0x6c, 0x20, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x90);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);

    n = 0;
    qtest_writeb(qts, REG_BASE + 0x21, 0xa5);
    emit(program, &n, alu_word(1, 0xff, 0x8f, 0x21, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x90);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x10000);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94), ==, status);
    qtest_quit(qts);
    assert_no_aic_log(log_path);
    unlink(log_path);
    g_rmdir(dir);
}

static void test_reserved_host_count_sequencer_writes(void)
{
    uint8_t program[8] = { 0 };
    QTestState *qts = start_aic();
    size_t n = 0;

    emit(program, &n, alu_word(1, 0xa5, 0x69, 0x8f, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 0);
    qtest_quit(qts);

    qts = start_aic();
    n = 0;
    qtest_writeb(qts, REG_BASE + 0x66, 0x8f);
    emit(program, &n, alu_word(1, 0xa5, 0x69, 0x6d, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 0);
    qtest_quit(qts);
}

static void test_host_dfdat_sequencer_port(void)
{
    uint8_t program[12] = { 0 };
    size_t n = 0;
    QTestState *qts = start_aic();

    emit_or(program, &n, 0x5a, 0x99);
    emit(program, &n, alu_word(0, 0, 0x99, 0x20, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
    qtest_quit(qts);
}

static void test_host_fifo_rejects_scsi_selection(void)
{
    uint8_t program[12] = { 0 };
    size_t n = 0;
    QTestState *qts = start_disk();

    qtest_writeb(qts, REG_BASE + 0x93, 1);
    qtest_writeb(qts, REG_BASE + 0x99, 0x5a);
    emit_or(program, &n, 0x07, 0x05);
    emit_or(program, &n, 0x40, 0x00);
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x42, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 0);
    qtest_quit(qts);
}

static void check_old_shift(uint8_t control, uint8_t dest,
                            uint8_t canary, uint8_t expected)
{
    uint8_t program[8] = { 0 };
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x30, 0x81);
    qtest_writeb(qts, REG_BASE + dest, canary);
    qtest_writeb(qts, REG_BASE + 0x64, 0x7c);
    emit(program, &n, alu_word(5, control, 0x30, dest, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + dest), ==, expected);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 0x81);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x04, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
    qtest_quit(qts);
}

static void test_old_shift_one(void)
{
    static const struct {
        uint8_t control, dest, canary, expected;
    } cases[] = {
        { 0x11, 0x40, 0xa1, 0x02 }, /* SHL */
        { 0x1f, 0x41, 0xa2, 0x40 }, /* SHR */
        { 0x01, 0x42, 0xa3, 0x03 }, /* ROL */
        { 0x0f, 0x43, 0xa4, 0xc0 }, /* ROR */
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        check_old_shift(cases[i].control, cases[i].dest,
                        cases[i].canary, cases[i].expected);
    }
}

static void test_old_rol_two(void)
{
    check_old_shift(0x02, 0x44, 0xa5, 0x06);
}

static void test_old_shift_eight(void)
{
    static const struct {
        uint8_t control, dest, canary, expected;
    } cases[] = {
        { 0xf0, 0x45, 0xa6, 0x00 }, /* SHL */
        { 0xf8, 0x46, 0xa7, 0x00 }, /* SHR */
        { 0x00, 0x47, 0xa8, 0x81 }, /* ROL */
        { 0x08, 0x48, 0xa9, 0x81 }, /* ROR */
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        check_old_shift(cases[i].control, cases[i].dest,
                        cases[i].canary, cases[i].expected);
    }
}

static void test_old_shift_return(void)
{
    uint8_t program[5 * 4] = { 0 };
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x30, 0x81);
    qtest_writeb(qts, REG_BASE + 0x49, 0xaa);
    qtest_writeb(qts, REG_BASE + 0x4a, 0xbb);
    emit(program, &n, branch_word(0xb, 0, 0x65, 3));
    emit_or(program, &n, 1, 0x91);
    emit_or(program, &n, 0x55, 0x4a);
    emit(program, &n, alu_word(5, 0x01, 0x30, 0x49, true));
    emit_or(program, &n, 0x66, 0x4a);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x49), ==, 0x03);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 0x81);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x4a), ==, 0xbb);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x04, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
    qtest_quit(qts);
}

static void test_old_shift_unsupported_control(void)
{
    uint8_t program[8] = { 0 };
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x30, 0x81);
    qtest_writeb(qts, REG_BASE + 0x4b, 0xcc);
    emit(program, &n, alu_word(5, 0x80, 0x30, 0x4b, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x4b), ==, 0xcc);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 0x81);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 0);
    qtest_quit(qts);
}

static void test_old_shift_rejected_source(void)
{
    uint8_t program[8] = { 0 };
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x30, 0x81);
    qtest_writeb(qts, REG_BASE + 0x4c, 0xdd);
    qtest_writeb(qts, REG_BASE + 0x65, 0x30);
    emit(program, &n, alu_word(5, 0x80, 0x6c, 0x4c, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x30);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 0x81);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x4c), ==, 0xdd);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 0);
    qtest_quit(qts);
}

static void run_old_alu(QTestState *qts, uint8_t op, uint8_t operand,
                        uint8_t source, uint8_t dest)
{
    uint8_t program[8] = { 0 };
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    emit(program, &n, alu_word(op, operand, source, dest, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x44, ==, 0);
}

static void test_old_alu_bytes(void)
{
    static const struct {
        uint8_t op, operand, source, dest, canary, expected;
    } cases[] = {
        { 2, 0x0f, 0x5a, 0x50, 0xa1, 0x55 },
        { 3, 0x01, 0xff, 0x51, 0xa2, 0x00 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        QTestState *qts = start_aic();

        qtest_writeb(qts, REG_BASE + 0x30, cases[i].source);
        qtest_writeb(qts, REG_BASE + cases[i].dest, cases[i].canary);
        run_old_alu(qts, cases[i].op, cases[i].operand, 0x30,
                    cases[i].dest);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + cases[i].dest), ==,
                        cases[i].expected);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==,
                        cases[i].source);
        qtest_quit(qts);
    }
}

static void test_old_alu_accumulator(void)
{
    static const struct {
        uint8_t op, source, accum, dest, expected;
    } cases[] = {
        { 2, 0x5a, 0x0f, 0x50, 0x55 },
        { 3, 0xff, 0x01, 0x51, 0x00 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        QTestState *qts = start_aic();

        qtest_writeb(qts, REG_BASE + 0x30, cases[i].source);
        qtest_writeb(qts, REG_BASE + 0x64, cases[i].accum);
        qtest_writeb(qts, REG_BASE + cases[i].dest, 0xa5);
        run_old_alu(qts, cases[i].op, 0, 0x30, cases[i].dest);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + cases[i].dest), ==,
                        cases[i].expected);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==,
                        cases[i].source);
        qtest_quit(qts);
    }
}

static void test_old_xor_return(void)
{
    uint8_t program[5 * 4] = { 0 };
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x30, 0x5a);
    qtest_writeb(qts, REG_BASE + 0x52, 0xa5);
    qtest_writeb(qts, REG_BASE + 0x53, 0xb6);
    emit(program, &n, branch_word(0xb, 0, 0x65, 3));
    emit_or(program, &n, 1, 0x91);
    emit_or(program, &n, 0x55, 0x53);
    emit(program, &n, alu_word(2, 0x0f, 0x30, 0x52, true));
    emit_or(program, &n, 0x66, 0x53);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x52), ==, 0x55);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x53), ==, 0xb6);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 1);
    qtest_quit(qts);
}

static void test_old_alu_flags(void)
{
    QTestState *qts = start_aic();

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 0);
    run_old_alu(qts, 3, 1, 0x69, 0x54); /* STC */
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x54), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);

    qtest_writeb(qts, REG_BASE + 0x30, 0x5a);
    run_old_alu(qts, 2, 0x0f, 0x30, 0x55);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x55), ==, 0x55);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 1);
    run_old_alu(qts, 2, 0x5a, 0x30, 0x56);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x56), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);

    run_old_alu(qts, 3, 0xff, 0x6a, 0x57); /* CLC */
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x57), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x31, 0x7f);
    run_old_alu(qts, 3, 1, 0x31, 0x58);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x58), ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x32, 0xff);
    run_old_alu(qts, 3, 1, 0x32, 0x59);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x59), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);

    run_old_alu(qts, 2, 1, 0x6b, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x5a), ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 0x5a);
    qtest_quit(qts);
}

static void test_old_adc_carry(void)
{
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x30, 0x10);
    run_old_alu(qts, 4, 1, 0x30, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0x11);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 0);
    run_old_alu(qts, 3, 1, 0x69, 0x41); /* STC */
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
    run_old_alu(qts, 4, 1, 0x30, 0x42);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x42), ==, 0x12);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 0);
    run_old_alu(qts, 3, 1, 0x69, 0x43); /* STC */
    qtest_writeb(qts, REG_BASE + 0x31, 0xfe);
    run_old_alu(qts, 4, 1, 0x31, 0x44);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x44), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
    qtest_writeb(qts, REG_BASE + 0x64, 1);
    run_old_alu(qts, 4, 0, 0x30, 0x45);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x45), ==, 0x12);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 0);
    qtest_quit(qts);
}

static void test_old_flags_reset_and_host_write(void)
{
    QTestState *qts = start_aic();

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 0);
    run_old_alu(qts, 3, 1, 0x69, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
    qtest_writeb(qts, REG_BASE + 0x6b, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 1); /* CHIPRST */
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 0);
    run_old_alu(qts, 3, 1, 0x69, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
    qtest_system_reset(qts);
    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 0);
    qtest_quit(qts);
}

static void test_old_flags_sequencer_write(void)
{
    uint8_t program[8] = { 0 };
    QTestState *qts = start_aic();
    size_t n = 0;

    run_old_alu(qts, 3, 1, 0x69, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
    emit_or(program, &n, 1, 0x6b);
    emit_or(program, &n, 1, 0x91);
    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 0);
    qtest_quit(qts);
}

static void test_old_flags_migrate(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-flags-XXXXXX", &error);
    g_autofree char *uri = NULL;
    g_autofree char *socket_path = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket_path = g_strdup_printf("%s/migrate.sock", dir);
    uri = g_strdup_printf("unix:%s", socket_path);
    from = start_aic();
    to = qtest_init("-M pc -m 128M -nodefaults -S -incoming defer "
                    "-device aic7880,id=hba,bus=pci.0,addr=04.0");
    run_old_alu(from, 3, 1, 0x69, 0x40); /* STC */
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x6b), ==, 3);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x6b), ==, 3);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x6b), ==, 3);
    qtest_writeb(to, REG_BASE + 0x30, 0x10);
    run_old_alu(to, 4, 1, 0x30, 0x41);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x41), ==, 0x12);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x6b), ==, 0);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket_path);
    g_rmdir(dir);
}

static void test_old_alu_none_flags(void)
{
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x47, 0xa5);
    run_old_alu(qts, 3, 1, 0x69, 0x6a); /* STC to NONE */
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x47), ==, 0xa5);
    run_old_alu(qts, 3, 0xff, 0x6a, 0x6a); /* CLC to NONE */
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x47), ==, 0xa5);
    qtest_quit(qts);
}

static void test_old_alu_host_only_source(void)
{
    for (uint8_t op = 2; op <= 4; op++) {
        uint8_t complete[8] = { 0 };
        uint8_t rejected[8] = { 0 };
        QTestState *qts = start_aic();
        size_t n = 0;

        run_old_alu(qts, 3, 1, 0x69, 0x47); /* STC */
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
        qtest_writeb(qts, REG_BASE + 0x87, 0x04);
        qtest_writeb(qts, REG_BASE + 0x92, 1);
        emit_or(complete, &n, 0x0d, 0x9d);
        emit_or(complete, &n, 1, 0x91);
        start_program(qts, complete, n);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
        qtest_writeb(qts, REG_BASE + 0x46, 0xa5);
        qtest_writeb(qts, REG_BASE + 0x87, 0x04);
        qtest_writeb(qts, REG_BASE + 0x92, 1);
        n = 0;
        emit(rejected, &n, alu_word(op, 2, 0x9d, 0x46, false));
        emit_or(rejected, &n, 1, 0x91);
        start_program(qts, rejected, n);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x46), ==, 0xa5);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, 0x0d);
        qtest_quit(qts);
    }
}

static void test_old_alu_readonly_destination(void)
{
    for (uint8_t op = 2; op <= 4; op++) {
        uint8_t program[8] = { 0 };
        QTestState *qts = start_aic();
        size_t n = 0;

        run_old_alu(qts, 3, 1, 0x69, 0x47); /* STC */
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
        qtest_writeb(qts, REG_BASE + 0x30, 0x81);
        qtest_writeb(qts, REG_BASE + 0x65, 0x30);
        qtest_writeb(qts, REG_BASE + 0x87, 0x04);
        qtest_writeb(qts, REG_BASE + 0x92, 1);
        emit(program, &n, alu_word(op, 1, 0x6c, 0x6b, false));
        emit_or(program, &n, 1, 0x91);
        start_program(qts, program, n);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x30);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 0x81);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 0);
        qtest_quit(qts);
    }
}

static void test_public_sequencer_format(void)
{
    uint8_t program[8];
    QTestState *qts = start_aic();
    size_t n = 0;

    emit(program, &n, alu_word(0, 0x5a, 0x6a, 0x20, false));
    emit(program, &n, alu_word(0, 1, 0x6a, 0x91, false));
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_sequencer_clocked_resume(void)
{
    uint8_t program[3 * 4];
    QTestState *qts = start_clocked_aic();
    size_t n = 0;
    int64_t first;

    emit(program, &n, branch_word(0xc, 1, 0x9c, 2));
    emit(program, &n, alu_word(1, 1, 0x69, 0x91, false));
    emit(program, &n, branch_word(0xc, 1, 0x6a, 0));
    start_program_async(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);
    first = qtest_clock_step_next(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    g_assert_cmpint(qtest_clock_step_next(qts) - first, ==, 100);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 0);

    qtest_writeb(qts, REG_BASE + 0x87, 6);
    qtest_writeb(qts, REG_BASE + 0x9b, 3);
    qtest_writeb(qts, REG_BASE + 0x87, 2);
    qtest_clock_step(qts, 199);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 2);
    qtest_quit(qts);
}

static void start_self_branch_program(QTestState *qts)
{
    uint8_t program[2 * 4];
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x20, 0);
    qtest_writeb(qts, REG_BASE + 0x21, 0);
    qtest_writeb(qts, REG_BASE + 0x22, 1);
    qtest_writeb(qts, REG_BASE + 0x65, 0x20);
    emit(program, &n, branch_word(0xc, 1, 0x6c, 0));
    emit(program, &n, alu_word(1, 1, 0x69, 0x91, false));
    start_program_async(qts, program, n);
}

static void test_sequencer_self_branch_resumes(void)
{
    QTestState *qts = start_clocked_aic();
    int64_t first;

    start_self_branch_program(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x20);
    first = qtest_clock_step_next(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x21);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    g_assert_cmpint(qtest_clock_step_next(qts) - first, ==, 100);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x22);
    qtest_clock_step(qts, 100);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x23);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    qtest_clock_step(qts, 100);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x63), ==, 0);
    qtest_quit(qts);
}

static void test_sequencer_self_branch_cancel(void)
{
    for (unsigned mode = 0; mode < 2; mode++) {
        QTestState *qts = start_clocked_aic();
        uint8_t index;

        start_self_branch_program(qts);
        qtest_clock_step_next(qts);
        qtest_writeb(qts, REG_BASE + 0x87, mode ? 0x01 : 0x06);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 4);
        index = qtest_readb(qts, REG_BASE + 0x65);
        if (!mode) {
            g_assert_cmphex(index, ==, 0x21);
        }
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, index);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x09, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        qtest_quit(qts);
    }
}

static void test_sequencer_queue_wakes(void)
{
    uint8_t program[3 * 4];
    QTestState *qts = start_aic();
    size_t n = 0;

    emit(program, &n, branch_word(0xc, 1, 0x9c, 2));
    emit(program, &n, alu_word(1, 1, 0x69, 0x91, false));
    emit(program, &n, branch_word(0xc, 1, 0x6a, 0));
    start_program(qts, program, n);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x9b, 3);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x09, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 2);
    qtest_quit(qts);
}

static void test_sequencer_clocked_cancel(void)
{
    uint8_t program[3 * 4];
    size_t n = 0;

    emit(program, &n, branch_word(0xc, 1, 0x9c, 2));
    emit(program, &n, alu_word(1, 1, 0x69, 0x91, false));
    emit(program, &n, branch_word(0xc, 1, 0x6a, 0));
    for (unsigned mode = 0; mode < 4; mode++) {
        QTestState *qts = start_clocked_aic();

        start_program_async(qts, program, n);
        qtest_clock_step(qts, 99);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);
        if (mode == 0) {
            qtest_writeb(qts, REG_BASE + 0x87, 6);
        } else if (mode == 1) {
            qtest_writeb(qts, REG_BASE + 0x60, 0x91);
        } else if (mode == 2) {
            qtest_writeb(qts, REG_BASE + 0x87, 1);
        } else {
            qtest_system_reset(qts);
            config_writel(qts, 0x14, REG_BASE);
            config_writew(qts, 0x04, 6);
        }
        qtest_clock_step(qts, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);
        qtest_clock_step(qts, 999);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);
        if (mode == 1) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x60) & 1, ==, 1);
        } else {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
        }
        qtest_quit(qts);
    }
}

static void test_sequencer_reset_restarts_after_pause(void)
{
    uint8_t program[3 * 4];
    QTestState *qts = start_clocked_aic();
    size_t n = 0;

    emit(program, &n, branch_word(0xc, 1, 0x9c, 2));
    emit(program, &n, alu_word(1, 1, 0x69, 0x91, false));
    emit(program, &n, branch_word(0xc, 1, 0x6a, 0));
    start_program_async(qts, program, n);
    qtest_clock_step_next(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 2);
    qtest_writeb(qts, REG_BASE + 0x87, 6);
    qtest_writeb(qts, REG_BASE + 0x60, 0x92);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    qtest_writeb(qts, REG_BASE + 0x60, 0x90);
    qtest_writeb(qts, REG_BASE + 0x87, 2);
    qtest_clock_step(qts, 99);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_sequencer_clocked_keeps_pc(void)
{
    uint8_t program[4 * 4];
    QTestState *qts = start_clocked_aic();
    size_t n = 0;
    int64_t first;

    emit_or(program, &n, 0x0a, 0x9d);
    emit(program, &n, branch_word(0xc, 1, 0x9c, 3));
    emit(program, &n, alu_word(1, 1, 0x69, 0x91, false));
    emit(program, &n, branch_word(0xc, 1, 0x6a, 1));
    start_program_async(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    first = qtest_clock_step_next(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmpint(qtest_clock_step_next(qts) - first, ==, 100);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 3);
    qtest_clock_step(qts, 100);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_sequencer_clocked_migrates(void)
{
    uint8_t program[4 * 4];
    g_autofree char *dir = g_dir_make_tmp("aic7880-clock-XXXXXX", NULL);
    g_autofree char *socket_path = g_build_filename(dir, "migrate.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket_path);
    QTestState *from = start_clocked_aic();
    QTestState *to = qtest_init(
        "-M pc -m 128M -nodefaults -S -incoming defer "
        "-device aic7880,id=hba,bus=pci.0,addr=04.0,x-sequencer-clocked=on");
    size_t n = 0;
    int64_t source_time;

    emit_or(program, &n, 0x0a, 0x9d);
    emit(program, &n, branch_word(0xc, 1, 0x9c, 3));
    emit(program, &n, alu_word(1, 1, 0x69, 0x91, false));
    emit(program, &n, branch_word(0xc, 1, 0x6a, 1));
    start_program_async(from, program, n);
    source_time = qtest_clock_step(from, 150);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x62), ==, 1);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x9e), ==, 1);
    qtest_qmp_assert_success(from, "{'execute':'stop'}");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    qtest_clock_set(to, source_time);
    sgi_migrate_resume(to);
    g_assert_cmphex(config_readl(to, 0x14) & ~0x0fu, ==, REG_BASE);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x62), ==, 1);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9e), ==, 1);
    qtest_clock_step(to, 49);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x62), ==, 1);
    qtest_clock_step(to, 1);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x62), ==, 3);
    qtest_clock_step(to, 100);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x62), ==, 1);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9e), ==, 1);
    qtest_writeb(to, REG_BASE + 0x9b, 3);
    qtest_clock_step(to, 200);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9c), ==, 1);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x91), ==, 1);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9d), ==, 0x0a);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9e), ==, 0);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket_path);
    g_rmdir(dir);
}

static void test_old_branch_high_address(void)
{
    uint8_t program[0x103 * 4] = { 0 };
    QTestState *qts = start_aic();
    size_t n = 0;

    emit(program, &n, branch_word(8, 0, 0x65, 0x101));
    emit_or(program, &n, 1, 0x91);
    n = 0x101 * 4;
    emit_or(program, &n, 0x5a, 0x20);
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_or_return(void)
{
    for (unsigned accumulator = 0; accumulator < 2; accumulator++) {
        uint8_t program[5 * 4];
        QTestState *qts = start_aic();
        size_t n = 0;

        qtest_writeb(qts, REG_BASE + 0x31, 0x12);
        qtest_writeb(qts, REG_BASE + 0x64, 0x44);
        qtest_writeb(qts, REG_BASE + 0x65, 0x24);
        emit(program, &n, branch_word(0xb, 0, 0x65, 3));
        emit_or(program, &n, 0x11, 0x40);
        emit_or(program, &n, 1, 0x91);
        emit(program, &n,
             alu_word(0, accumulator ? 0 : 3, 0x31, 0x20, true));
        emit_or(program, &n, 0x55, 0x41);
        start_program(qts, program, n);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==,
                        accumulator ? 0x56 : 0x13);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0x11);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 3);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x64), ==, 0x44);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x31), ==, 0x12);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        qtest_quit(qts);
    }
}

static void test_or_return_pauses_at_caller(void)
{
    uint8_t program[5 * 4];
    QTestState *qts = start_aic();
    size_t n = 0;

    emit(program, &n, branch_word(0xb, 0, 0x65, 3));
    emit_or(program, &n, 0x11, 0x40);
    emit_or(program, &n, 1, 0x91);
    emit(program, &n, alu_word(0, 1, 0x6a, 0x91, true));
    emit_or(program, &n, 0x55, 0x41);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    resume_program(qts, 2);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0x11);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_sequencer_interrupt_code(void)
{
    for (unsigned pci_reset = 0; pci_reset < 2; pci_reset++) {
        uint8_t program[8 * 4];
        QTestState *qts = start_aic();
        size_t n = 0;

        qtest_irq_intercept_in(qts, "ioapic");
        qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
        qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
        emit_or(program, &n, 0xa1, 0x91);
        emit_or(program, &n, 0x11, 0x40);
        emit_or(program, &n, 5, 0x9d);
        emit_or(program, &n, 0x32, 0x91);
        emit_or(program, &n, 0x22, 0x41);
        emit(program, &n, branch_word(8, 0, 0x65, 5));
        emit_or(program, &n, 0xb1, 0x91);
        emit_or(program, &n, 0x33, 0x42);
        start_program(qts, program, n);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0xa1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0);
        g_assert_true(qtest_get_irq(qts, 11));

        qtest_writeb(qts, REG_BASE + 0x92, 1);
        resume_program(qts, 2);
        wait_register(qts, 0x91, 3, 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0xa2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0x11);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0x22);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
        qtest_writeb(qts, REG_BASE + 0x87, 6);
        qtest_writeb(qts, REG_BASE + 0x62, 6);
        resume_program(qts, 2);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0xb3);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 7);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x42), ==, 0);
        qtest_writeb(qts, REG_BASE + 0x92, 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0xb1);
        g_assert_true(qtest_get_irq(qts, 11));
        if (pci_reset) {
            qobject_unref(qtest_qmp(qts, "{'execute': 'system_reset'}"));
            qtest_qmp_eventwait(qts, "RESET");
            config_writel(qts, 0x14, REG_BASE);
            config_writew(qts, 0x04, 0x0006);
        } else {
            qtest_writeb(qts, REG_BASE + 0x87, 1);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
        g_assert_false(qtest_get_irq(qts, 11));
        qtest_quit(qts);
    }
}

static void test_or_return_without_call(void)
{
    static const uint8_t program[] = { 0x5a, 0x6a, 0x20, 0x01 };
    QTestState *qts = start_aic();

    for (unsigned byte = 0; byte < 8; byte++) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6f), ==, 0);
    }
    load_program(qts, program, sizeof(program));
    qtest_writeb(qts, REG_BASE + 0x60, 0x94);
    step_instruction(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x63), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    qtest_quit(qts);
}

static QTestState *start_pending_status(void)
{
    static const uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t program[128] = { 0 };
    size_t wait_at = 9 + sizeof(cdb);
    uint32_t jump = branch_word(8, 0, 0x65, wait_at);
    QTestState *qts = start_disk_mode("-d guest_errors");

    qtest_writeb(qts, REG_BASE + 0x88, 0x00);
    qtest_writeb(qts, REG_BASE + 0x89, 0x00);
    qtest_writeb(qts, REG_BASE + 0x8a, 0x01);
    qtest_writeb(qts, REG_BASE + 0x8c, 36);
    qtest_writeb(qts, REG_BASE + 0x08, 36);
    build_command_program(program, cdb, sizeof(cdb), false);
    for (unsigned byte = 0; byte < 4; byte++) {
        program[wait_at * 4 + byte] = jump >> (byte * 8);
    }
    start_program(qts, program, (wait_at + 1) * 4);
    for (unsigned i = 0; i < 1000; i++) {
        if ((qtest_readb(qts, REG_BASE + 0x03) & 0xe6) == 0xc6) {
            break;
        }
        qtest_clock_step(qts, 1000);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe6, ==, 0xc6);
    return qts;
}

static void test_status_message_phases(void)
{
    uint8_t status_read[8] = { 0 };
    uint8_t finish[16] = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-phase-XXXXXX", &error);
    g_autofree char *socket_path = NULL;
    g_autofree char *uri = NULL;
    QTestState *qts, *to;
    size_t n;

    g_assert_no_error(error);
    socket_path = g_strdup_printf("%s/migrate.sock", dir);
    uri = g_strdup_printf("unix:%s", socket_path);
    qts = start_pending_status();
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x08, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);

    to = qtest_init("-M pc -m 128M -nodefaults -S -incoming defer "
                    "-drive if=none,id=disk0,file=null-co://,format=raw "
                    "-device aic7880,id=hba,bus=pci.0,addr=04.0 "
                    "-device scsi-hd,drive=disk0,bus=hba.0,"
                    "scsi-id=0,lun=0");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    sgi_migrate_stopped(qts, uri);
    sgi_wait_migration(to, "completed");
    qtest_quit(qts);
    qts = to;
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe6, ==, 0xc6);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x08, ==, 0);

    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    n = 0;
    emit(status_read, &n, alu_word(0, 0, 0x06, 0x22, false));
    emit(status_read, &n, branch_word(8, 0, 0x65, 1));
    qtest_writeb(qts, REG_BASE + 0x03, 0xc0);
    start_program(qts, status_read, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x22), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe6, ==, 0xe6);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x08, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);

    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    n = 0;
    emit(finish, &n, alu_word(0, 0, 0x06, 0x23, false));
    emit_or(finish, &n, 1, 0x9d);
    emit_or(finish, &n, 2, 0x91);
    emit(finish, &n, branch_word(8, 0, 0x65, 3));
    qtest_writeb(qts, REG_BASE + 0x03, 0xe0);
    start_program(qts, finish, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x23), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x08, ==, 0x08);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x02, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x10, 0x02);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x04, ==, 0);
    qtest_quit(qts);
    unlink(socket_path);
    g_rmdir(dir);
}

static void test_pio_request_phase_qualification(void)
{
    for (unsigned message = 0; message < 2; message++) {
        QTestState *qts = start_disk();
        uint8_t phase = message ? 0xa0 : 0x80;

        qtest_writeb(qts, REG_BASE + 0x01, 8);
        attention_select(qts, message ? 0x48 : 0x40);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 2, ==, 2);
        qtest_writeb(qts, REG_BASE + 0x03, phase ^ 0x20);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 0x11);
        qtest_writeb(qts, REG_BASE + 0x03, phase);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 1);
        qtest_writeb(qts, REG_BASE + 0x0b, 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 2, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 1, ==, 1);
        qtest_writeb(qts, REG_BASE + 0x0c, 1);
        for (unsigned i = 0; i < 3; i++) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 2, ==, 0);
        }
        qtest_writeb(qts, REG_BASE + 0x01, 0);
        qtest_writeb(qts, REG_BASE + 0x01, 8);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 2, ==, 2);
        qtest_writeb(qts, REG_BASE + 0x03, phase ^ 0x20);
        qtest_writeb(qts, REG_BASE + 0x92, 1);
        attention_write_raw(qts, 0x06, message ? 0x80 : 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 0x11);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        qtest_writeb(qts, REG_BASE + 0x0c, 1);
        attention_write_raw(qts, 0x06, message ? 0x80 : 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 1, ==, 1);
        qtest_writeb(qts, REG_BASE + 0x87, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 2, ==, 0);
        qtest_quit(qts);
    }
}

static void test_message_negotiation_reject(void)
{
    static const uint8_t messages[][5] = {
        { 1, 3, 1, 25, 8 },
        { 1, 2, 3, 1, 0 },
    };

    for (unsigned kind = 0; kind < ARRAY_SIZE(messages); kind++) {
        QTestState *qts = start_disk();
        unsigned length = messages[kind][1] + 2;

        qtest_writeb(qts, REG_BASE + 0x04, 0x84);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x04), ==, 0x84);
        attention_select(qts, 0x48);
        qtest_writeb(qts, REG_BASE + 0x06, 0xc0);
        for (unsigned i = 0; i < length; i++) {
            qtest_writeb(qts, REG_BASE + 0x06, messages[kind][i]);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0,
                            ==, 0xa0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        }
        qtest_writeb(qts, REG_BASE + 0x0c, 0x40);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0xe0);
        for (unsigned i = 0; i < 3; i++) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x12), ==, 7);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 1, ==, 1);
        }
        qtest_writeb(qts, REG_BASE + 0x03, 0xc0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 0x11);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x06), ==, 7);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0xe0);
        qtest_writeb(qts, REG_BASE + 0x0c, 1);
        qtest_writeb(qts, REG_BASE + 0x0b, 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x06), ==, 7);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0x80);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x04), ==, 0x84);
        qtest_writeb(qts, REG_BASE + 0x03, 0x80);
        qtest_writeb(qts, REG_BASE + 0x04, 0);
        attention_complete_test_unit_ready(qts);
        qtest_quit(qts);
    }
}

static void test_software_attention_precommand(void)
{
    QTestState *qts;
    static const uint8_t wdtr[] = { 1, 2, 3, 1 };

    for (unsigned sequencer = 0; sequencer < 2; sequencer++) {
        qts = start_disk();
        attention_select(qts, 0x40);
        if (sequencer) {
            attention_write(qts, 0x03, 0xb0);
        } else {
            qtest_writeb(qts, REG_BASE + 0x03, 0xb0);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0xb0);
        qtest_writeb(qts, REG_BASE + 0x03, 0xa0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0x10, ==, 0x10);
        for (unsigned i = 0; i < sizeof(wdtr); i++) {
            qtest_writeb(qts, REG_BASE + 0x06, wdtr[i]);
        }
        qtest_writeb(qts, REG_BASE + 0x0c, 0x40);
        qtest_writeb(qts, REG_BASE + 0x03, 0xe0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x06), ==, 7);
        qtest_writeb(qts, REG_BASE + 0x03, 0x80);
        attention_complete_test_unit_ready(qts);
        qtest_quit(qts);
    }

    qts = start_disk();
    attention_select(qts, 0x40);
    attention_write(qts, 0x06, 0);
    qtest_writeb(qts, REG_BASE + 0x03, 0xb0);
    /* Attention is unsupported after delivery of the first CDB byte. */
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    qtest_quit(qts);
}

static int64_t pio_read_to_scratch(QTestState *qts)
{
    uint8_t program[8];
    size_t n = 0;

    emit(program, &n, alu_word(1, 0xff, 0x06, 0x30, false));
    emit_or(program, &n, 1, 0x91);
    return start_program(qts, program, n);
}

static void test_pio_incoming_phase_qualification(void)
{
    QTestState *qts = start_disk();

    qtest_writeb(qts, REG_BASE + 0x08, 0x37);
    host_dma_program(qts, 0x12000, 0);
    attention_select(qts, 0x40);
    for (unsigned i = 0; i < 6; i++) {
        attention_write(qts, 0x06, 0);
    }
    for (unsigned i = 0; i < 1000 &&
         (qtest_readb(qts, REG_BASE + 0x03) & 0xe0) != 0xc0; i++) {
        qtest_clock_step(qts, 1000);
    }
    for (unsigned message = 0; message < 2; message++) {
        uint8_t phase = message ? 0xe0 : 0xc0;

        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, phase);
        for (unsigned i = 0; i < 3; i++) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x12),
                            ==, message ? 0 : 2);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 1, ==, 1);
        }
        qtest_writeb(qts, REG_BASE + 0x03, phase);
        qtest_writeb(qts, REG_BASE + 0x01, 0);
        pio_read_to_scratch(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, phase);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 2, ==, 0);
        qtest_writeb(qts, REG_BASE + 0x01, 2);
        qtest_writeb(qts, REG_BASE + 0x01, 8);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 2, ==, 2);
        qtest_writeb(qts, REG_BASE + 0x03, phase ^ 0x20);
        pio_read_to_scratch(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, phase);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 0x11);
        qtest_writeb(qts, REG_BASE + 0x03, phase);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 1);
        qtest_writeb(qts, REG_BASE + 0x0c, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 2, ==, 2);
        qtest_writeb(qts, REG_BASE + 0x0b, 2);
        pio_read_to_scratch(qts);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0x37);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x12000);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 0);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 0);
    qtest_quit(qts);
}

static void manual_command(QTestState *qts, const uint8_t *cdb, size_t len)
{
    uint8_t program[96];
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x87, 4);
    emit(program, &n, alu_word(1, 0xff, 0x6a, 0x93, false));
    emit(program, &n, alu_word(1, 0xff, 0x6a, 0x00, false));
    emit_or(program, &n, 8, 0x01);
    emit_or(program, &n, 7, 0x05);
    emit_or(program, &n, 0x80, 0x03);
    emit_or(program, &n, 0x40, 0x00);
    for (size_t i = 0; i < len; i++) {
        if (cdb[i]) {
            emit_or(program, &n, cdb[i], 0x06);
        } else {
            emit(program, &n, alu_word(1, 0xff, 0x6a, 0x06, false));
        }
    }
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    qtest_writeb(qts, REG_BASE + 0x01, 0);
}

static void manual_wait_phase(QTestState *qts, uint8_t phase)
{
    for (unsigned i = 0; i < 1000; i++) {
        if ((qtest_readb(qts, REG_BASE + 3) & 0xe0) == phase) {
            break;
        }
        qtest_clock_step(qts, 1000);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe0, ==, phase);
    qtest_writeb(qts, REG_BASE + 3, phase);
}

static void test_manual_status_ack(void)
{
    static const uint8_t cdb[6] = { 0 };
    QTestState *qts = start_disk();
    uint8_t status;

    manual_command(qts, cdb, sizeof(cdb));
    manual_wait_phase(qts, 0xc0);
    status = qtest_readb(qts, REG_BASE + 0x12);
    /* The null disk's initial TEST UNIT READY reports CHECK CONDITION. */
    g_assert_cmphex(status, ==, 2);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, status);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe3, ==, 0xc2);
    }
    qtest_writeb(qts, REG_BASE + 3, 0xc1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe3, ==, 0xc1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, status);
    qtest_writeb(qts, REG_BASE + 3, 0xc1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe3, ==, 0xc1);
    qtest_writeb(qts, REG_BASE + 3, 0xc0);
    manual_wait_phase(qts, 0xe0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, 0);
    qtest_writeb(qts, REG_BASE + 3, 0xe1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe3, ==, 0xe1);
    qtest_writeb(qts, REG_BASE + 3, 0xe0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_quit(qts);
}

static void test_pending_reject_migrates(void)
{
    for (unsigned cleared = 0; cleared < 2; cleared++) {
        g_autofree char *dir = g_dir_make_tmp("aic7880-message-XXXXXX", NULL);
        g_autofree char *socket = g_build_filename(dir, "migrate.sock", NULL);
        g_autofree char *uri = g_strdup_printf("unix:%s", socket);
        QTestState *from = start_disk();
        QTestState *to = start_disk_mode("-S -incoming defer");
        static const uint8_t message[] = { 0x80, 1, 2, 3, 1 };

        attention_select(from, 0x48);
        for (unsigned i = 0; i < sizeof(message); i++) {
            qtest_writeb(from, REG_BASE + 0x06, message[i]);
        }
        qtest_writeb(from, REG_BASE + 0x0c, 0x40);
        qtest_writeb(from, REG_BASE + 0x03, 0xc0);
        qtest_writeb(from, REG_BASE + (cleared ? 0x0b : 0x0c),
                     cleared ? 2 : 1);
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration(to, "completed");
        g_assert_cmphex(qtest_readb(to, REG_BASE + 0x0c) & 0x11,
                        ==, cleared ? 0x11 : 0);
        g_assert_cmphex(qtest_readb(to, REG_BASE + 0x0b) & 2,
                        ==, cleared ? 0 : 2);
        g_assert_cmphex(qtest_readb(to, REG_BASE + 0x12), ==, 7);
        qtest_writeb(to, REG_BASE + 0x03, 0xe0);
        g_assert_cmphex(qtest_readb(to, REG_BASE + 0x06), ==, 7);
        g_assert_cmphex(qtest_readb(to, REG_BASE + 0x03) & 0xe0, ==, 0x80);
        qtest_writeb(to, REG_BASE + 0x87, 1);
        g_assert_cmphex(qtest_readb(to, REG_BASE + 0x0c) & 0x11, ==, 0);
        g_assert_cmphex(qtest_readb(to, REG_BASE + 0x0b) & 2, ==, 0);
        qtest_quit(to);
        qtest_quit(from);
        g_unlink(socket);
        g_rmdir(dir);
    }
}

static void test_peer_disconnected_nexus(void)
{
    static const uint8_t inquiry[] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t program[128], expected[36], guarded[52], got[52];
    QTestState *qts = start_disk();

    host_dma_program(qts, 0x10008, 36);
    qtest_writeb(qts, REG_BASE + 0x08, 36);
    start_program(qts, program,
                  build_command_program(program, inquiry, sizeof(inquiry),
                                        false));
    for (unsigned i = 0; i < 1000 &&
         !qtest_readb(qts, REG_BASE + 0x9e); i++) {
        qtest_clock_step(qts, 1000);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    qtest_memread(qts, 0x10008, expected, sizeof(expected));
    qtest_quit(qts);

    for (unsigned save = 0; save < 2; save++) {
        qts = start_peer_disk();
        memset(guarded, 0xa5, sizeof(guarded));
        qtest_memwrite(qts, 0x10000, guarded, sizeof(guarded));
        host_dma_program(qts, 0x10008, 17);
        qtest_writeb(qts, REG_BASE + 0x08, 17);
        start_program(qts, program,
                      build_selected_inquiry_identify(program, 0xc0));
        qtest_writeb(qts, REG_BASE + 0x93, 0x38);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x10019);
        qtest_writeb(qts, REG_BASE + 0x93, 0);
        qtest_writeb(qts, REG_BASE + 0x9b, 3);
        attention_write(qts, 0x9d, 2);
        if (save) {
            peer_event(qts, "save:0:0");
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x12), ==, 2);
            qtest_writeb(qts, REG_BASE + 0x03, 0xe0);
            pio_read_to_scratch(qts);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 2);
        }
        {
            size_t n = 0;

            for (unsigned i = 0; i < 4; i++) {
                emit(program, &n, alu_word(1, 0xff, 0x14 + i, 0x40 + i,
                                         false));
            }
            emit_or(program, &n, 1, 0x91);
            start_program(qts, program, n);
        }
        peer_event(qts, "disconnect:0:0");
        qtest_writeb(qts, REG_BASE + 0x03, 0xe0);
        int64_t free_epoch = pio_read_to_scratch(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03), ==, 0);
        qtest_clock_set(qts, free_epoch + 399);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 8, ==, 0);
        qtest_clock_step(qts, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 8, ==, 8);
        host_dma_program(qts, 0x70000000, 19);
        {
            size_t n = 0;

            for (unsigned i = 0; i < 4; i++) {
                emit(program, &n, alu_word(1, 0xff, 0x40 + i, 0x88 + i,
                                         false));
            }
            emit_or(program, &n, 19, 0x08);
            emit_or(program, &n, 1, 0x91);
            start_program(qts, program, n);
        }
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x10019);
        qtest_writeb(qts, REG_BASE + 0x93, 0x38);
        qtest_memread(qts, 0x10000, got, sizeof(got));
        memcpy(guarded + 8, expected, 17);
        g_assert_cmpmem(got, sizeof(got), guarded, sizeof(guarded));
        peer_event_rejected(qts, "reselect:1:7:0");
        peer_event_rejected(qts, "reselect:0:7:3");
        attention_write(qts, 0x00, 0);
        peer_event_rejected(qts, "reselect:0:7:0");
        attention_write(qts, 0x00, 0x10);
        if (save) {
            qtest_writeb(qts, REG_BASE + 0x93, 0);
        }
        peer_event(qts, "reselect:0:7:0");
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0xa0, ==, 0x20);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x12), ==, 0x80);
        qtest_writeb(qts, REG_BASE + 0x03, 0xe0);
        pio_read_to_scratch(qts);
        peer_event_rejected(qts, "reselect:0:7:0");
        if (save) {
            peer_event(qts, "restore:0:0");
            qtest_writeb(qts, REG_BASE + 0x03, 0xe0);
            pio_read_to_scratch(qts);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 3);
            qtest_writeb(qts, REG_BASE + 0x93, 0x38);
        } else {
            peer_event_rejected(qts, "restore:0:0");
            qtest_writeb(qts, REG_BASE + 0x93, 0x38);
        }
        for (unsigned i = 0; i < 1000 &&
             (qtest_readb(qts, REG_BASE + 0x03) & 0xe0) != 0xc0; i++) {
            qtest_clock_step(qts, 1000);
        }
        qtest_memread(qts, 0x10000, got, sizeof(got));
        memcpy(guarded + 8, expected, sizeof(expected));
        g_assert_cmpmem(got, sizeof(got), guarded, sizeof(guarded));
        g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x1002c);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
        qtest_writeb(qts, REG_BASE + 0x03, 0xc0);
        pio_read_to_scratch(qts);
        qtest_writeb(qts, REG_BASE + 0x03, 0xe0);
        pio_read_to_scratch(qts);
        qtest_quit(qts);
    }
}

static void test_peer_disconnect_boundaries(void)
{
    for (unsigned reset = 0; reset < 3; reset++) {
        QTestState *qts = start_peer_disk();
        uint8_t program[128], guard[52], got[52];

        memset(guard, 0xa5, sizeof(guard));
        qtest_memwrite(qts, 0x10000, guard, sizeof(guard));
        host_dma_program(qts, 0x10008, 17);
        qtest_writeb(qts, REG_BASE + 0x08, 17);
        start_program(qts, program,
                      build_selected_inquiry_identify(program,
                                                      reset ? 0xc0 : 0x80));
        qtest_writeb(qts, REG_BASE + 0x93, 0x38);
        qtest_writeb(qts, REG_BASE + 0x93, 0);
        if (!reset) {
            peer_event_rejected(qts, "disconnect:0:0");
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0,
                            ==, 0x40);
        } else {
            peer_event_rejected(qts, "save:0:3");
            peer_event_rejected(qts, "restore:0:0");
            peer_event(qts, "disconnect:0:0");
            qtest_writeb(qts, REG_BASE + 0x03, 0xe0);
            pio_read_to_scratch(qts);
            peer_event_rejected(qts, "disconnect:0:0");
            if (reset == 1) {
                g_autofree char *dir = g_dir_make_tmp(
                    "aic7880-disconnected-XXXXXX", NULL);
                g_autofree char *path = g_build_filename(dir, "state", NULL);
                g_autofree char *uri = g_strdup_printf("file:%s", path);

                qtest_qmp_assert_success(qts,
                    "{'execute':'migrate','arguments':{'uri':%s}}", uri);
                sgi_wait_migration(qts, "failed");
                g_unlink(path);
                g_rmdir(dir);
            }
        }
        qtest_memread(qts, 0x10000, guard, sizeof(guard));
        if (reset == 2) {
            qtest_system_reset(qts);
            config_writel(qts, 0x14, REG_BASE);
            config_writew(qts, 0x04, 6);
        } else if (reset == 1) {
            attention_write(qts, 0x00, 1);
        } else {
            qtest_writeb(qts, REG_BASE + 0x87, 1);
        }
        peer_event_rejected(qts, "reselect:0:7:0");
        qtest_writeb(qts, REG_BASE + 0x93, 0x38);
        qtest_memread(qts, 0x10000, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), guard, sizeof(guard));
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
        qtest_quit(qts);
    }
}

static void test_message_refused_lengths(void)
{
    static const uint8_t messages[][4] = {
        { 0x80, 1, 0, 0 }, { 0x80, 1, 4, 0 },
        { 0x80, 1, 2, 1 }, { 0x80, 0x80, 0, 0 },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(messages); i++) {
        QTestState *qts = start_disk();

        attention_select(qts, 0x48);
        for (unsigned j = 0; j < (i < 3 ? 3 : 2); j++) {
            qtest_writeb(qts, REG_BASE + 0x06, messages[i][j]);
        }
        if (i == 2) {
            qtest_writeb(qts, REG_BASE + 0x06, messages[i][3]);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0xa0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        qtest_writeb(qts, REG_BASE + 0x87, 1);
        qtest_writeb(qts, REG_BASE + 0x01, 8);
        attention_select(qts, 0x48);
        qtest_writeb(qts, REG_BASE + 0x06, 0x80);
        qtest_writeb(qts, REG_BASE + 0x0c, 0x40);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0x80);
        qtest_quit(qts);
    }
}

static void test_pio_wrong_direction(void)
{
    for (unsigned message = 0; message < 2; message++) {
        QTestState *qts = start_disk();
        uint8_t phase = message ? 0xa0 : 0x80;

        attention_select(qts, message ? 0x48 : 0x40);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x06), ==, 0xff);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, phase);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 1, ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        qtest_quit(qts);
    }
}

static void test_nonzero_rate_refuses_data(void)
{
    QTestState *qts = start_disk();
    static const uint8_t inquiry[] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t program[128], guard[52], got[52];

    memset(guard, 0xa5, sizeof(guard));
    qtest_memwrite(qts, 0x10000, guard, sizeof(guard));
    host_dma_program(qts, 0x10008, 36);
    qtest_writeb(qts, REG_BASE + 0x08, 36);
    qtest_writeb(qts, REG_BASE + 0x04, 1);
    start_program(qts, program,
                  build_command_program(program, inquiry, sizeof(inquiry),
                                        false));
    qtest_memread(qts, 0x10000, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), guard, sizeof(guard));
    g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x10008);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 36);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_rate_change_refuses_pending_data(void)
{
    static const uint8_t rates[] = { 1, 0x80, 0x81 };

    for (unsigned rate = 0; rate < ARRAY_SIZE(rates); rate++) {
        QTestState *qts = start_selected_disk(NULL);
        uint8_t program[128], guard[35], got[35];

        host_dma_program(qts, 0x10008, 17);
        qtest_writeb(qts, REG_BASE + 0x08, 17);
        start_program(qts, program, build_selected_inquiry_program(program));
        qtest_writeb(qts, REG_BASE + 0x93, 0x38);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x10019);
        qtest_writeb(qts, REG_BASE + 0x93, 0x30);
        memset(guard, 0xa5, sizeof(guard));
        qtest_memwrite(qts, 0x10200, guard, sizeof(guard));
        host_dma_program(qts, 0x10208, 19);
        qtest_writeb(qts, REG_BASE + 0x08, 19);
        qtest_writeb(qts, REG_BASE + 0x04, rates[rate]);
        qtest_writeb(qts, REG_BASE + 0x93, 0x38);
        qtest_memread(qts, 0x10200, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), guard, sizeof(guard));
        g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x10208);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 19);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 19);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        qtest_quit(qts);
    }
}

static void test_expected_phase_clears_on_reset_and_bus_free(void)
{
    for (unsigned reset = 0; reset < 5; reset++) {
        QTestState *qts = reset == 4 ? start_peer_disk() : start_disk();
        uint8_t program[20];
        size_t n = 0;

        emit_or(program, &n, 8, 0x01);
        emit_or(program, &n, 7, 0x05);
        emit_or(program, &n, 0, 0x00);
        emit_or(program, &n, 0x40, 0x00);
        emit_or(program, &n, 1, 0x91);
        if (reset) {
            qtest_writeb(qts, REG_BASE + 0x03, 0x80);
        }
        if (reset == 1) {
            qtest_writeb(qts, REG_BASE + 0x87, 1);
        } else if (reset == 2) {
            qtest_writeb(qts, REG_BASE + 0x00, 1);
        } else if (reset == 4) {
            qtest_writeb(qts, REG_BASE + 0x05, 7);
            qtest_writeb(qts, REG_BASE + 0x00, 0x10);
            peer_event(qts, "reselect:0:7");
            peer_event(qts, "release");
        } else if (reset == 3) {
            start_program(qts, program, n);
            for (unsigned i = 0; i < 6; i++) {
                attention_write(qts, 0x06, 0);
            }
            for (unsigned i = 0; i < 1000 &&
                 (qtest_readb(qts, REG_BASE + 0x03) & 0xe0) != 0xc0; i++) {
                qtest_clock_step(qts, 1000);
            }
            qtest_writeb(qts, REG_BASE + 0x03, 0xc0);
            pio_read_to_scratch(qts);
            qtest_writeb(qts, REG_BASE + 0x03, 0x80);
            qtest_writeb(qts, REG_BASE + 0x0c, 1);
            pio_read_to_scratch(qts);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03), ==, 0);
        }
        start_program(qts, program, n);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 0x11);
        qtest_writeb(qts, REG_BASE + 0x03, 0x80);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 1);
        qtest_quit(qts);
    }
}

static void test_attention_selection_phase(void)
{
    static const uint8_t controls[] = { 0x40, 0x48 };

    for (unsigned i = 0; i < ARRAY_SIZE(controls); i++) {
        uint8_t program[12] = { 0 };
        QTestState *qts = start_disk();
        size_t n = 0;

        emit_or(program, &n, 0x07, 0x05);
        emit_or(program, &n, controls[i], 0x00);
        emit_or(program, &n, 1, 0x91);
        start_program(qts, program, n);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00), ==, controls[i]);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x42, ==, 0x42);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==,
                        i ? 0xa0 : 0x80);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0x10, ==,
                        i ? 0x10 : 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        qtest_quit(qts);
    }
}

static void test_attention_identify_clear_order(void)
{
    for (unsigned clear_first = 0; clear_first < 2; clear_first++) {
        QTestState *qts = start_disk();

        attention_select(qts, 0x48);
        qtest_writeb(qts, REG_BASE + 0x92, 1);
        if (clear_first) {
            attention_write(qts, 0x0c, 0x40);
        } else {
            attention_write(qts, 0x06, 0x80);
            qtest_writeb(qts, REG_BASE + 0x92, 1);
            attention_write(qts, 0x00, 0x40);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00), ==, 0x40);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==,
                        0xa0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0x10, ==,
                        clear_first ? 0 : 0x10);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        qtest_writeb(qts, REG_BASE + 0x92, 1);

        if (clear_first) {
            attention_write(qts, 0x06, 0x80);
        } else {
            attention_write(qts, 0x0c, 0x40);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==,
                        0x80);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0x10, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        qtest_quit(qts);
    }
}

static void test_attention_identify_notifies_pio_ready(void)
{
    uint8_t program[8] = { 0 };
    QTestState *qts = start_disk();
    size_t n = 0;

    attention_select(qts, 0x48);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    attention_write(qts, 0x0c, 0x40);
    qtest_writeb(qts, REG_BASE + 0x0b, 0x02);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 2, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x10, 0x02);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 4, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    emit_or(program, &n, 0x80, 0x06);
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    qtest_quit(qts);
}

static void test_attention_rearm_after_bus_free(void)
{
    QTestState *qts = start_disk();

    qtest_writeb(qts, REG_BASE + 0x01, 0x08);
    attention_select(qts, 0x48);
    attention_write(qts, 0x06, 0x80);
    attention_write(qts, 0x0c, 0x40);
    attention_complete_test_unit_ready(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0x10, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x92, 3);
    attention_write(qts, 0x00, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    attention_write(qts, 0x00, 0);
    attention_write(qts, 0x00, 0x48);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0xb0);
    attention_write(qts, 0x00, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0xb0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_attention_full_template_without_peer(void)
{
    QTestState *qts = start_disk();

    qtest_writeb(qts, REG_BASE + 0x01, 0x08);
    attention_select(qts, 0x48);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    attention_write(qts, 0x00, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0xb0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x20, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x04, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    attention_write(qts, 0x06, 0x80);
    attention_write(qts, 0x0c, 0x40);
    attention_complete_test_unit_ready(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x20, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x04, ==, 0);
    qtest_quit(qts);
}

static void test_peer_reselection_status_and_irq(void)
{
    QTestState *qts = start_peer_disk();
    int irq = -1;

    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    qtest_writeb(qts, REG_BASE + 0x05, 0x07);
    qtest_writeb(qts, REG_BASE + 0x10, 0x20);
    qtest_writeb(qts, REG_BASE + 0x87, 0x06);
    peer_event_rejected(qts, "reselect:0:7");
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0xe0, ==, 0);
    attention_write(qts, 0x00, 0x10);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00), ==, 0x10);
    peer_event_rejected(qts, "reselect:1:7");
    peer_event_rejected(qts, "reselect:0:0");
    peer_event(qts, "reselect:0:7");
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0xe0, ==, 0x20);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    for (int pin = 0; pin < 24; pin++) {
        if (qtest_get_irq(qts, pin)) {
            irq = pin;
            break;
        }
    }
    g_assert_cmpint(irq, >=, 0);
    g_assert_true(qtest_get_irq(qts, irq));
    peer_event_rejected(qts, "continue");
    qtest_writeb(qts, REG_BASE + 0x0b, 0x20);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x20, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 4, ==, 4);
    qtest_writeb(qts, REG_BASE + 0x92, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 4, ==, 0);
    g_assert_false(qtest_get_irq(qts, irq));
    peer_event(qts, "release");
    attention_write(qts, 0x00, 0);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    peer_event_rejected(qts, "reselect:0:7");
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x20, ==, 0);
    qtest_quit(qts);
}

static QTestState *start_peer_data_in(bool check_parity, bool auto_atn,
                                     bool interrupt_mask)
{
    static const uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t command[7 * 4] = { 0 };
    QTestState *qts = start_peer_disk();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x01, 0x08);
    qtest_writeb(qts, REG_BASE + 0x02, check_parity ? 0x20 : 0);
    qtest_writeb(qts, REG_BASE + 0x11, interrupt_mask ? 0x04 : 0);
    peer_event(qts, "hold-data-in");
    attention_select(qts, auto_atn ? 0x5a : 0x58);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    attention_write(qts, 0x06, 0x80);
    attention_write(qts, 0x0c, 0x40);
    for (unsigned i = 0; i < sizeof(inquiry); i++) {
        emit_or(command, &n, inquiry[i], 0x06);
    }
    emit(command, &n, branch_word(8, 0, 0x65, 6));
    qtest_writeb(qts, REG_BASE + 0x03, 0x80);
    start_program(qts, command, n);
    for (unsigned i = 0; i < 1000; i++) {
        if ((qtest_readb(qts, REG_BASE + 0x03) & 0xe0) == 0x40) {
            break;
        }
        qtest_clock_step(qts, 1000);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0x40);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    return qts;
}

static void test_peer_data_in_parity(void)
{
    for (unsigned check = 0; check < 2; check++) {
        for (unsigned auto_atn = 0; auto_atn < 2; auto_atn++) {
            for (unsigned mask = 0; mask < 2; mask++) {
                QTestState *qts = start_peer_data_in(check, auto_atn, mask);

                peer_event_rejected(qts, "bad:status:0:00");
                peer_event_rejected(qts, "bad:data-in:1:00");
                peer_event_rejected(qts, "bad:data-in:0:ff");
                peer_event(qts, "bad:data-in:0:00");
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0,
                                ==, 0x40);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x14,
                                ==, check ? 0x04 : 0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0x10,
                                ==, check && auto_atn ? 0x10 : 0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 4,
                                ==, check && mask ? 4 : 0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4,
                                ==, check && mask ? 4 : 0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
                qtest_writeb(qts, REG_BASE + 0x0c, 0x04);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 4,
                                ==, check ? 4 : 0);
                peer_event(qts, "good:data-in:1:00");
                qtest_writeb(qts, REG_BASE + 0x0c, 0x04);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 4,
                                ==, 0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0x10,
                                ==, check && auto_atn ? 0x10 : 0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 4,
                                ==, check && mask ? 4 : 0);
                qtest_writeb(qts, REG_BASE + 0x0c, 0x40);
                qtest_writeb(qts, REG_BASE + 0x92, 4);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0x10,
                                ==, 0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 4,
                                ==, 0);
                peer_event(qts, "release");
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x0c,
                                ==, 0x08);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
                qtest_quit(qts);
            }
        }
    }
}

static void test_peer_parity_latches_across_bus_free(void)
{
    QTestState *qts = start_peer_data_in(true, true, true);

    peer_event(qts, "bad:data-in:0:00");
    peer_event(qts, "good:data-in:1:00");
    peer_event(qts, "release");
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0x10, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x0c, ==, 0x0c);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x0c, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 4, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 4, ==, 4);
    qtest_writeb(qts, REG_BASE + 0x92, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 4, ==, 0);
    qtest_quit(qts);
}

static void test_peer_channel_clear_preserves_held_data(void)
{
    QTestState *qts = start_peer_data_in(true, true, true);

    qtest_writeb(qts, REG_BASE + 0x87, 6);
    qtest_writeb(qts, REG_BASE + 0x01, 0x0a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0x40);
    peer_event(qts, "release");
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_quit(qts);
}

static void test_peer_bad_sample_survives_bus_free(void)
{
    QTestState *qts = start_peer_data_in(true, true, true);

    peer_event(qts, "bad:data-in:0:00");
    peer_event(qts, "release");
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0x10, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x0c, ==, 0x0c);
    qtest_writeb(qts, REG_BASE + 0x0c, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x04, ==, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x04, ==, 0);
    qtest_quit(qts);
}

static void test_peer_reselection_rejects_selection_and_wide_id(void)
{
    QTestState *qts = start_peer_disk();

    qtest_writeb(qts, REG_BASE + 0x05, 0x08);
    attention_write(qts, 0x00, 0x10);
    peer_event_rejected(qts, "reselect:0:8");
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x20, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x05, 0x07);
    peer_event(qts, "reselect:0:7");
    attention_write(qts, 0x00, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x60, ==, 0x20);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    peer_event(qts, "release");
    qtest_quit(qts);
}

static void test_peer_reselection_mask_and_busy(void)
{
    QTestState *qts = start_peer_disk();

    qtest_writeb(qts, REG_BASE + 0x05, 0x07);
    attention_write(qts, 0x00, 0x10);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    peer_event(qts, "reselect:0:7");
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0xe0, ==, 0x20);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 4, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x19), ==, 0xff);
    peer_event_rejected(qts, "reselect:0:7");
    qtest_writeb(qts, REG_BASE + 0x06, 0x12);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    qtest_writeb(qts, REG_BASE + 0x05, 0x07);
    qtest_writeb(qts, REG_BASE + 0x01, 0x08);
    attention_select(qts, 0x5a);
    peer_event_rejected(qts, "reselect:0:7");
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0xb0);
    qtest_quit(qts);

    qts = start_disk();
    peer_event_rejected(qts, "reselect:0:7");
    qtest_quit(qts);
}

static void test_peer_parity_irq_and_resets(void)
{
    QTestState *qts = start_peer_data_in(true, true, true);
    int irq = -1;

    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    peer_event(qts, "bad:data-in:0:00");
    peer_event(qts, "good:data-in:1:00");
    qtest_writeb(qts, REG_BASE + 0x87, 0x06);
    for (int pin = 0; pin < 24; pin++) {
        if (qtest_get_irq(qts, pin)) {
            irq = pin;
            break;
        }
    }
    g_assert_cmpint(irq, >=, 0);
    config_writew(qts, 0x04, 0x0002);
    g_assert_false(qtest_get_irq(qts, irq));
    config_writew(qts, 0x04, 0x0006);
    qtest_writeb(qts, REG_BASE + 0x92, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 4, ==, 4);
    config_writew(qts, 0x04, 0x0004);
    g_assert_false(qtest_get_irq(qts, irq));
    config_writew(qts, 0x04, 0x0006);
    qtest_writeb(qts, REG_BASE + 0x87, 0x16);
    config_writew(qts, 0x04, 0x0004);
    g_assert_false(qtest_get_irq(qts, irq));
    config_writew(qts, 0x04, 0x0006);
    g_assert_true(qtest_get_irq(qts, irq));
    qtest_writeb(qts, REG_BASE + 0x87, 0x06);
    config_writew(qts, 0x04, 0x0004);
    g_assert_false(qtest_get_irq(qts, irq));
    config_writew(qts, 0x04, 0x0006);
    g_assert_true(qtest_get_irq(qts, irq));
    qtest_writeb(qts, REG_BASE + 0x87, 0x46);
    g_assert_false(qtest_get_irq(qts, irq));
    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    g_assert_false(qtest_get_irq(qts, irq));
    qtest_writeb(qts, REG_BASE + 0x87, 0x06);
    g_assert_true(qtest_get_irq(qts, irq));
    qtest_writeb(qts, REG_BASE + 0x00, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00) & 0x5a, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0x10, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 4, ==, 4);
    qtest_writeb(qts, REG_BASE + 0x0c, 4);
    qtest_writeb(qts, REG_BASE + 0x92, 4);
    g_assert_false(qtest_get_irq(qts, irq));
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 4, ==, 0);
    qtest_quit(qts);

    qts = start_peer_data_in(true, true, true);
    peer_event(qts, "bad:data-in:0:00");
    qtest_system_reset(qts);
    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 4, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0x10, ==, 0);
    qtest_quit(qts);
}

static void test_peer_status_fault_stops_completion(void)
{
    uint8_t command[7 * 4] = { 0 };
    uint8_t finish[3 * 4] = { 0 };
    QTestState *qts = start_peer_disk();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x01, 0x08);
    qtest_writeb(qts, REG_BASE + 0x02, 0x20);
    attention_select(qts, 0x5a);
    peer_event_rejected(qts, "bad:msg-out:0:80");
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    attention_write(qts, 0x06, 0x80);
    attention_write(qts, 0x0c, 0x40);
    for (unsigned i = 0; i < 6; i++) {
        emit_or(command, &n, 0, 0x06);
    }
    emit(command, &n, branch_word(8, 0, 0x65, 6));
    qtest_writeb(qts, REG_BASE + 0x03, 0x80);
    start_program(qts, command, n);
    for (unsigned i = 0; i < 1000; i++) {
        if ((qtest_readb(qts, REG_BASE + 0x03) & 0xe0) == 0xc0) {
            break;
        }
        qtest_clock_step(qts, 1000);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0xc0);
    peer_event_rejected(qts, "bad:msg-in:0:00");
    peer_event(qts, "bad:status:0:02");
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 4, ==, 4);
    peer_event(qts, "good:status:0:02");
    qtest_writeb(qts, REG_BASE + 0x0c, 0x44);
    qtest_writeb(qts, REG_BASE + 0x92, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 4, ==, 0);
    n = 0;
    emit(finish, &n, alu_word(0, 0, 0x06, 0x22, false));
    emit_or(finish, &n, 1, 0x9d);
    emit_or(finish, &n, 2, 0x91);
    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x03, 0xc0);
    start_program(qts, finish, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_quit(qts);
}

static void test_peer_message_in_parity(void)
{
    uint8_t command[7 * 4] = { 0 };
    uint8_t read_status[2 * 4] = { 0 };
    uint8_t read_message[2 * 4] = { 0 };
    QTestState *qts = start_peer_disk();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x01, 0x08);
    qtest_writeb(qts, REG_BASE + 0x02, 0x20);
    attention_select(qts, 0x5a);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    attention_write(qts, 0x06, 0x80);
    attention_write(qts, 0x0c, 0x40);
    for (unsigned i = 0; i < 6; i++) {
        emit_or(command, &n, 0, 0x06);
    }
    emit(command, &n, branch_word(8, 0, 0x65, 6));
    qtest_writeb(qts, REG_BASE + 0x03, 0x80);
    start_program(qts, command, n);
    for (unsigned i = 0; i < 1000; i++) {
        if ((qtest_readb(qts, REG_BASE + 0x03) & 0xe0) == 0xc0) {
            break;
        }
        qtest_clock_step(qts, 1000);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0xc0);
    n = 0;
    emit(read_status, &n, alu_word(0, 0, 0x06, 0x22, false));
    emit(read_status, &n, branch_word(8, 0, 0x65, 1));
    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x03, 0xc0);
    start_program(qts, read_status, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0xe0);
    qtest_writeb(qts, REG_BASE + 0x87, 4);
    qtest_writeb(qts, REG_BASE + 0x03, 0xe0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x10, ==, 0);
    peer_event(qts, "bad:msg-in:0:00");
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x14, ==, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0xf0);
    peer_event(qts, "good:msg-in:0:00");
    qtest_writeb(qts, REG_BASE + 0x0c, 0x44);
    n = 0;
    emit(read_message, &n, alu_word(0, 0, 0x06, 0x23, false));
    emit_or(read_message, &n, 1, 0x9d);
    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x03, 0xe0);
    start_program(qts, read_message, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_quit(qts);
}

static void assert_peer_migration_refused(QTestState *qts)
{
    g_autofree char *dir = g_dir_make_tmp("aic7880-peer-migrate-XXXXXX",
                                        NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    g_autofree char *uri = g_strdup_printf("file:%s", path);

    qtest_qmp_assert_success(qts,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    for (unsigned i = 0; i < 30000; i++) {
        g_autoptr(QDict) reply = qtest_qmp_assert_success_ref(qts,
            "{'execute':'query-migrate'}");
        const char *status = qdict_get_str(reply, "status");

        if (g_str_equal(status, "failed")) {
            unlink(path);
            g_rmdir(dir);
            return;
        }
        g_assert_cmpstr(status, !=, "completed");
        g_usleep(1000);
    }
    g_assert_not_reached();
}

static void test_peer_migration_boundary(void)
{
    g_autofree char *dir = g_dir_make_tmp("aic7880-peer-idle-XXXXXX",
                                        NULL);
    g_autofree char *socket_path = g_build_filename(dir, "migrate.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket_path);
    QTestState *qts, *to;

    qts = start_peer_disk();
    qtest_writeb(qts, REG_BASE + 0x05, 0x07);
    attention_write(qts, 0x00, 0x10);
    peer_event(qts, "reselect:0:7");
    assert_peer_migration_refused(qts);
    qtest_quit(qts);

    qts = start_peer_data_in(true, true, true);
    peer_event(qts, "bad:data-in:0:00");
    peer_event(qts, "good:data-in:1:00");
    peer_event(qts, "release");
    assert_peer_migration_refused(qts);
    qtest_quit(qts);

    qts = start_peer_disk();
    qtest_writeb(qts, REG_BASE + 0x01, 0x08);
    attention_select(qts, 0x5a);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    attention_write(qts, 0x06, 0x80);
    attention_write(qts, 0x0c, 0x40);
    attention_complete_test_unit_ready(qts);
    qtest_writeb(qts, REG_BASE + 0x92, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00), ==, 0x5a);
    to = qtest_init("-M pc -m 128M -nodefaults -S -incoming defer "
                    "-drive if=none,id=disk0,file=null-co://,format=raw "
                    "-device aic7880,id=hba,bus=pci.0,addr=04.0,"
                    "x-test-peer=on "
                    "-device scsi-hd,drive=disk0,bus=hba.0,"
                    "scsi-id=0,lun=0");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    sgi_migrate_stopped(qts, uri);
    sgi_wait_migration(to, "completed");
    qtest_quit(qts);
    config_writel(to, 0x14, REG_BASE);
    config_writew(to, 0x04, 0x0006);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x00), ==, 0x5a);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x0b) & 0x20, ==, 0);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x0c) & 0x04, ==, 0);
    qtest_quit(to);
    unlink(socket_path);
    g_rmdir(dir);
}

static void test_attention_rejects_messages(void)
{
    static const uint8_t messages[] = { 0x88, 0xa0, 0x01, 0x12 };

    for (unsigned i = 0; i < ARRAY_SIZE(messages); i++) {
        QTestState *qts = start_disk();

        attention_select(qts, 0x48);
        qtest_writeb(qts, REG_BASE + 0x92, 1);
        attention_write(qts, 0x06, messages[i]);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==,
                        0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==,
                        0xb0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 3, ==, 0);
        qtest_quit(qts);
    }
}

static void test_attention_rejects_controls(void)
{
    static const uint8_t controls[] = {
        0x4c, 0x68, 0xc8
    };

    for (unsigned i = 0; i < ARRAY_SIZE(controls); i++) {
        QTestState *qts = start_disk();

        attention_select(qts, controls[i]);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==,
                        0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        qtest_quit(qts);
    }
    {
        QTestState *qts = start_disk();

        qtest_writeb(qts, REG_BASE + 0x00, 0x48);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==,
                        0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0);
        qtest_quit(qts);
    }
    {
        QTestState *qts = start_disk();

        qtest_writeb(qts, REG_BASE + 0x02, 0x20);
        attention_select(qts, 0x48);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00), ==, 0x48);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==,
                        0xb0);
        qtest_quit(qts);
    }
    {
        QTestState *qts = start_disk();

        attention_select(qts, 0x48);
        qtest_writeb(qts, REG_BASE + 0x02, 0x20);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x02) & 0x20, ==, 0x20);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==,
                        0xb0);
        qtest_quit(qts);
    }
    {
        QTestState *qts = start_disk();

        attention_select(qts, 0x48);
        attention_write(qts, 0x00, 0x40);
        qtest_writeb(qts, REG_BASE + 0x02, 0x20);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x02) & 0x20, ==, 0x20);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==,
                        0xb0);
        qtest_quit(qts);
    }
}

static void test_attention_no_target_and_resets(void)
{
    QTestState *qts = start_aic();

    attention_select(qts, 0x48);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x10, ==, 0x10);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_quit(qts);

    qts = start_disk();
    attention_select(qts, 0x48);
    qtest_writeb(qts, REG_BASE + 0x01, 0x02);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0xb0);
    qtest_writeb(qts, REG_BASE + 0x01, 8);
    qtest_writeb(qts, REG_BASE + 0x03, 0xa0);
    qtest_writeb(qts, REG_BASE + 0x06, 0x80);
    qtest_writeb(qts, REG_BASE + 0x0c, 0x40);
    attention_complete_test_unit_ready(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x22), ==, 2);
    qtest_quit(qts);

    qts = start_disk();
    attention_select(qts, 0x48);
    qtest_writeb(qts, REG_BASE + 0x00, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00) & 0x48, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0);
    qtest_system_reset(qts);
    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0);
    qtest_quit(qts);
}

static void test_attention_pending_migration_refused(void)
{
    g_autofree char *dir = g_dir_make_tmp("aic7880-attn-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    g_autofree char *uri = g_strdup_printf("file:%s", path);
    QTestState *qts = start_disk();

    attention_select(qts, 0x48);
    qtest_qmp_assert_success(qts,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    {
        int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

        for (;;) {
            QDict *reply = qtest_qmp_assert_success_ref(qts,
                "{'execute':'query-migrate'}");
            g_autofree char *status = g_strdup(qdict_get_str(reply,
                                                               "status"));

            qobject_unref(reply);
            if (g_str_equal(status, "failed")) {
                break;
            }
            g_assert_cmpstr(status, !=, "completed");
            g_assert_cmpint(g_get_monotonic_time(), <, deadline);
            g_usleep(1000);
        }
    }
    qtest_quit(qts);
    unlink(path);
    g_rmdir(dir);
}

static void test_spioen_gates_input_phases(void)
{
    uint8_t status_read[8] = { 0 };
    uint8_t message_read[8] = { 0 };
    QTestState *qts = start_pending_status();
    size_t n = 0;

    emit(status_read, &n, alu_word(0, 0, 0x06, 0x22, false));
    emit(status_read, &n, branch_word(8, 0, 0x65, 1));
    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x93, 0);
    qtest_writeb(qts, REG_BASE + 0x01, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0x80);
    qtest_writeb(qts, REG_BASE + 0x03, 0xc0);
    start_program(qts, status_read, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe6, ==, 0xc6);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x02, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x08, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0);

    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x92, 0x08);
    qtest_writeb(qts, REG_BASE + 0x01, 0x82);
    qtest_writeb(qts, REG_BASE + 0x01, 0x88);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0x88);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x02, ==, 0x02);
    qtest_writeb(qts, REG_BASE + 0x03, 0xc0);
    start_program(qts, status_read, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe6, ==, 0xe6);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x08, ==, 0);

    n = 0;
    emit(message_read, &n, alu_word(0, 0, 0x06, 0x23, false));
    emit(message_read, &n, branch_word(8, 0, 0x65, 1));
    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x92, 0x08);
    qtest_writeb(qts, REG_BASE + 0x01, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0x80);
    qtest_writeb(qts, REG_BASE + 0x03, 0xe0);
    start_program(qts, message_read, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe6, ==, 0xe6);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x02, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x08, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0);

    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x92, 0x08);
    qtest_writeb(qts, REG_BASE + 0x01, 0x82);
    qtest_writeb(qts, REG_BASE + 0x01, 0x88);
    qtest_writeb(qts, REG_BASE + 0x03, 0xe0);
    start_program(qts, message_read, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe6, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x08, ==, 0x08);
    qtest_quit(qts);
}

static void test_dfon_latch_and_reset(void)
{
    QTestState *qts = start_aic();

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x01, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x01, 0x88);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0x88);
    qtest_writeb(qts, REG_BASE + 0x01, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0x80);
    qtest_writeb(qts, REG_BASE + 0x01, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0);

    qtest_writeb(qts, REG_BASE + 0x01, 0x80);
    qtest_writeb(qts, REG_BASE + 0x87, 1); /* CHIPRST */
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x01, 0x80);
    qobject_unref(qtest_qmp(qts, "{'execute': 'system_reset'}"));
    qtest_qmp_eventwait(qts, "RESET");
    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0);
    qtest_quit(qts);
}

static void test_dfon_rejects_other_controls(void)
{
    static const uint8_t unsupported[] = { 0x40, 0x20, 0x04, 0x01 };

    for (size_t i = 0; i < ARRAY_SIZE(unsupported); i++) {
        QTestState *qts = start_aic();

        qtest_writeb(qts, REG_BASE + 0x01, 0x80);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0x80);
        qtest_writeb(qts, REG_BASE + 0x01, 0x80 | unsupported[i]);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0x80);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0);
        qtest_quit(qts);
    }
}

static void test_setup_control_readback_and_reset(void)
{
    QTestState *qts = start_aic();

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x02), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x11), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x02, 0x27);
    qtest_writeb(qts, REG_BASE + 0x11, 0xa4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x02), ==, 0x27);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x11), ==, 0xa4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x04, ==, 0);

    qtest_writeb(qts, REG_BASE + 0x87, 1); /* CHIPRST */
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x02), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x11), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x02, 0x27);
    qtest_writeb(qts, REG_BASE + 0x11, 0xa4);
    qobject_unref(qtest_qmp(qts, "{'execute': 'system_reset'}"));
    qtest_qmp_eventwait(qts, "RESET");
    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x02), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x11), ==, 0);
    qtest_quit(qts);
}

static void test_setup_selection_timeout_mask(void)
{
    for (unsigned enabled = 0; enabled < 2; enabled++) {
        uint8_t program[12] = { 0 };
        size_t n = 0;
        QTestState *qts = start_aic();

        emit_or(program, &n, 0x07, 0x05);
        emit_or(program, &n, 0x40, 0x00);
        emit(program, &n, branch_word(8, 0, 0x65, 2));
        qtest_writeb(qts, REG_BASE + 0x02, 0x27);
        qtest_writeb(qts, REG_BASE + 0x11, enabled ? 0xa4 : 0x24);
        start_program(qts, program, n);
        qtest_clock_step(qts, 256000000);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0x80);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x04, ==,
                        enabled ? 0x04 : 0);
        qtest_writeb(qts, REG_BASE + 0x0c, 0x80); /* CLRSINT1.SELTO */
        qtest_writeb(qts, REG_BASE + 0x92, 0x04); /* CLRINT.CLRSCSIINT */
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x04, ==, 0);
        qtest_quit(qts);
    }
}

static void test_selection_timeout_scsiint_latch(void)
{
    uint8_t program[12] = { 0 };
    size_t n = 0;
    QTestState *qts = start_aic();

    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    emit_or(program, &n, 0x07, 0x05);
    emit_or(program, &n, 0x40, 0x00);
    emit(program, &n, branch_word(8, 0, 0x65, 2));
    qtest_writeb(qts, REG_BASE + 0x02, 0x04);
    qtest_writeb(qts, REG_BASE + 0x11, 0x80);
    start_program(qts, program, n);
    qtest_clock_step(qts, 256000000);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x04, ==, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 0x04);
    g_assert_true(qtest_get_irq(qts, 11));

    qtest_writeb(qts, REG_BASE + 0x11, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x04, ==, 0x04);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0x0c, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x04, ==, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 0x04);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0x92, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x04, ==, 0);
    g_assert_false(qtest_get_irq(qts, 11));
    qtest_quit(qts);
}

static void test_bus_free_scsiint_latch(void)
{
    QTestState *qts = start_aic();

    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    qtest_writeb(qts, REG_BASE + 0x11, 0x08);
    resume_program(qts, 2);
    qtest_writeb(qts, REG_BASE + 0x00, 0x01);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x08, ==, 0x08);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x04, ==, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 0x04);
    g_assert_true(qtest_get_irq(qts, 11));

    qtest_writeb(qts, REG_BASE + 0x0c, 0x08);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x08, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x04, ==, 0x04);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0x92, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x04, ==, 0);
    g_assert_false(qtest_get_irq(qts, 11));
    qtest_quit(qts);
}

static void test_selection_timeout_scsiint_latch_migrates(void)
{
    uint8_t program[12] = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-scsiint-XXXXXX", &error);
    g_autofree char *socket_path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;
    size_t n = 0;

    g_assert_no_error(error);
    socket_path = g_build_filename(dir, "migrate.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket_path);
    from = start_aic();
    to = qtest_init("-M pc -m 128M -nodefaults -S -incoming defer "
                    "-device aic7880,id=hba,bus=pci.0,addr=04.0");
    qtest_irq_intercept_in(from, "ioapic");
    qtest_irq_intercept_in(to, "ioapic");
    qtest_outl(from, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(from, PCI_CONFIG_DATA + 3, 11);
    emit_or(program, &n, 0x07, 0x05);
    emit_or(program, &n, 0x40, 0x00);
    emit(program, &n, branch_word(8, 0, 0x65, 2));
    qtest_writeb(from, REG_BASE + 0x02, 0x04);
    qtest_writeb(from, REG_BASE + 0x11, 0x80);
    start_program(from, program, n);
    qtest_clock_step(from, 256000000);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x0c) & 0x80, ==, 0x80);
    qtest_writeb(from, REG_BASE + 0x11, 0);
    qtest_writeb(from, REG_BASE + 0x0c, 0x80);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x0c) & 0x80, ==, 0);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x91) & 0x04, ==, 0x04);
    g_assert_cmphex(config_readl(from, 0x04) & 0x00080000, ==, 0x00080000);
    g_assert_true(qtest_get_irq(from, 11));

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x11), ==, 0);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x0c) & 0x80, ==, 0);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x91) & 0x04, ==, 0x04);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x87) & 0x06, ==, 0x06);
    g_assert_cmphex(config_readl(to, 0x04) & 0xffff, ==, 0x0006);
    g_assert_cmphex(config_readl(to, 0x04) & 0x00080000, ==, 0x00080000);
    /* qtest observes IRQ changes, so drive a new transition after loading. */
    qtest_writeb(to, REG_BASE + 0x87, 0x04);
    g_assert_false(qtest_get_irq(to, 11));
    qtest_writeb(to, REG_BASE + 0x87, 0x06);
    g_assert_true(qtest_get_irq(to, 11));
    qtest_writeb(to, REG_BASE + 0x92, 0x04);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x91) & 0x04, ==, 0);
    g_assert_cmphex(config_readl(to, 0x04) & 0x00080000, ==, 0);
    g_assert_false(qtest_get_irq(to, 11));
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket_path);
    g_rmdir(dir);
}

static void test_unmasked_selection_timeout(void)
{
    uint8_t program[12] = { 0 };
    size_t n = 0;
    QTestState *qts = start_aic();

    emit_or(program, &n, 0x07, 0x05);
    emit_or(program, &n, 0x40, 0x00);
    emit(program, &n, branch_word(8, 0, 0x65, 2));
    qtest_writeb(qts, REG_BASE + 0x02, 0x04);
    start_program(qts, program, n);
    qtest_clock_step(qts, 256000000);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x04, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x11, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x04, ==, 0x04);
    qtest_writeb(qts, REG_BASE + 0x0c, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x04, ==, 0x04);
    qtest_writeb(qts, REG_BASE + 0x92, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x04, ==, 0);
    qtest_quit(qts);
}

static int64_t start_missing_selection(QTestState *qts)
{
    uint8_t program[4 * 4];
    size_t n = 0;

    emit_or(program, &n, 0x27, 0x05);
    emit_or(program, &n, 0x40, 0x00);
    emit_or(program, &n, 1, 0x91);
    emit(program, &n, branch_word(8, 0, 0x65, 3));
    int64_t epoch = start_program(qts, program, n);

    qtest_writeb(qts, REG_BASE + 0x92, 1);
    return epoch;
}

static void test_selection_timer_intervals(void)
{
    for (unsigned period = 0; period < 4; period++) {
        for (unsigned masked = 0; masked < 2; masked++) {
            QTestState *qts = start_aic();
            int64_t interval = 256000000LL >> period;

            qtest_irq_intercept_in(qts, "ioapic");
            qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
            qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
            qtest_writeb(qts, REG_BASE + 0x02, 4 | (period << 3));
            qtest_writeb(qts, REG_BASE + 0x11, masked ? 0x80 : 0);
            int64_t epoch = start_missing_selection(qts);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x50,
                            ==, 0x10);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 8, ==, 8);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x88, ==, 0);
            g_assert_false(qtest_get_irq(qts, 11));
            qtest_clock_set(qts, epoch + interval - 1);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0);
            qtest_writeb(qts, REG_BASE + 0x87, 2);
            qtest_clock_step(qts, 1);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x88,
                            ==, 0x80);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 8, ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4,
                            ==, masked ? 4 : 0);
            g_assert_cmpint(qtest_get_irq(qts, 11), ==, masked);
            qtest_writeb(qts, REG_BASE + 0x87, 6);
            qtest_clock_step(qts, 399);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 8, ==, 0);
            qtest_clock_step(qts, 1);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x88,
                            ==, 0x88);
            qtest_writeb(qts, REG_BASE + 0x92, 4);
            g_assert_cmpint(qtest_get_irq(qts, 11), ==, masked);
            qtest_writeb(qts, REG_BASE + 0x0c, 0x80);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x88, ==, 8);
            qtest_writeb(qts, REG_BASE + 0x92, 4);
            g_assert_false(qtest_get_irq(qts, 11));
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x50,
                            ==, 0x10);
            qtest_writeb(qts, REG_BASE + 0x0b, 0x10);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x50, ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
            qtest_quit(qts);
        }
    }
}

static void test_selection_timer_late_enable(void)
{
    QTestState *qts = start_aic();

    start_missing_selection(qts);
    qtest_clock_step(qts, 1000000000);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 8, ==, 8);
    qtest_writeb(qts, REG_BASE + 0x02, 4);
    qtest_clock_step(qts, 64000000);
    qtest_writeb(qts, REG_BASE + 0x02, 4);
    qtest_clock_step(qts, 191999999);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0x80);
    qtest_quit(qts);
}

static void test_selection_timer_reconfigure(void)
{
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x02, 4);
    start_missing_selection(qts);
    qtest_clock_step(qts, 200000000);
    qtest_writeb(qts, REG_BASE + 0x02, 0x0c);
    qtest_clock_step(qts, 127999999);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x02, 8);
    qtest_clock_step(qts, 1000000000);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x02, 0x0c);
    qtest_clock_step(qts, 127999999);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0x80);
    qtest_quit(qts);
}

static void test_selection_timer_cancel(void)
{
    for (unsigned reset = 0; reset < 4; reset++) {
        QTestState *qts = start_aic();

        qtest_writeb(qts, REG_BASE + 0x02, 4);
        start_missing_selection(qts);
        qtest_clock_step(qts, 64000000);
        if (reset == 0) {
            attention_write(qts, 0, 0);
        } else if (reset == 1) {
            qtest_writeb(qts, REG_BASE + 0x87, 1);
        } else if (reset == 2) {
            qobject_unref(qtest_qmp(qts, "{'execute': 'system_reset'}"));
            qtest_qmp_eventwait(qts, "RESET");
            config_writel(qts, 0x14, REG_BASE);
            config_writew(qts, 0x04, 0x0006);
        } else {
            attention_write(qts, 0, 1);
        }
        qtest_clock_step(qts, 1000000000);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 8, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        qtest_quit(qts);
    }
    QTestState *qts = start_disk();

    qtest_writeb(qts, REG_BASE + 0x02, 4);
    attention_select(qts, 0x40);
    qtest_clock_step(qts, 1000000000);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x50, ==, 0x40);
    qtest_quit(qts);
}

static void test_selection_timer_migrates(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-select-XXXXXX", &error);
    g_autofree char *socket_path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;
    int64_t source_time, epoch;

    g_assert_no_error(error);
    socket_path = g_build_filename(dir, "migrate.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket_path);
    from = start_aic();
    to = qtest_init("-M pc -m 128M -nodefaults -S -incoming defer "
                    "-device aic7880,id=hba,bus=pci.0,addr=04.0");
    qtest_writeb(from, REG_BASE + 0x02, 4);
    epoch = start_missing_selection(from);
    source_time = qtest_clock_step(from, 64000000);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x0c) & 0x80, ==, 0);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_migrate_resume(to);
    /* QTest's destination clock starts at zero, independently of migration. */
    qtest_clock_set(to, source_time);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x03) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x0b) & 0x10, ==, 0x10);
    qtest_clock_set(to, epoch + 256000000 - 1);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x0c) & 0x80, ==, 0);
    qtest_clock_step(to, 1);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x0c) & 0x80, ==, 0x80);
    qtest_writeb(to, REG_BASE + 0x0c, 0x80);
    qtest_clock_step(to, 1000000000);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x0c) & 0x80, ==, 0);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket_path);
    g_rmdir(dir);
}

static void test_direction_latches_while_dma_enabled(void)
{
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x93, 0x38);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93), ==, 0x38);
    qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93), ==, 0x38);
    qtest_writeb(qts, REG_BASE + 0x93, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93), ==, 0x3c);
    qtest_quit(qts);
}

static void test_transfer_control_resets(void)
{
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x08, 0x56);
    qtest_writeb(qts, REG_BASE + 0x09, 0x34);
    qtest_writeb(qts, REG_BASE + 0x0a, 0x12);
    qtest_writeb(qts, REG_BASE + 0x14, 0x44);
    qtest_writeb(qts, REG_BASE + 0x15, 0x33);
    qtest_writeb(qts, REG_BASE + 0x16, 0x22);
    qtest_writeb(qts, REG_BASE + 0x17, 0x11);
    qtest_writeb(qts, REG_BASE + 0x01, 0x10);
    for (unsigned reg = 0x08; reg <= 0x0a; reg++) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + reg), ==, 0);
    }
    for (unsigned reg = 0x14; reg <= 0x17; reg++) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + reg), ==, 0);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);

    qtest_writeb(qts, REG_BASE + 0x08, 0x21);
    qtest_writeb(qts, REG_BASE + 0x14, 0x43);
    qtest_writeb(qts, REG_BASE + 0x01, 0x02);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 0x21);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x14), ==, 0x43);
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
    qtest_writeb(qts, REG_BASE + 0x93, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    qtest_quit(qts);
}

static QTestState *start_pending_fifo(uint32_t guest_addr)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t command[128] = { 0 };
    uint8_t bytes[42];
    size_t wait_at = 9 + sizeof(cdb);
    QTestState *qts = start_disk();
    uint32_t pause = alu_word(0, 1, 0x6a, 0x91, false);

    memset(bytes, 0xa5, sizeof(bytes));
    qtest_memwrite(qts, 0x10000, bytes, sizeof(bytes));
    qtest_writeb(qts, REG_BASE + 0x1f, 0x22);
    /* Hold this short response below the ordinary DMA start level. */
    qtest_writeb(qts, REG_BASE + 0x86, 0xc0);
    for (unsigned byte = 0; byte < 4; byte++) {
        qtest_writeb(qts, REG_BASE + 0x88 + byte,
                     guest_addr >> (byte * 8));
    }
    qtest_writeb(qts, REG_BASE + 0x8c, 36);
    qtest_writeb(qts, REG_BASE + 0x08, 36);
    build_command_program(command, cdb, sizeof(cdb), false);
    for (unsigned byte = 0; byte < 4; byte++) {
        command[wait_at * 4 + byte] = pause >> (byte * 8);
    }
    start_program(qts, command, (wait_at + 1) * 4);
    for (unsigned i = 0; i < 1000; i++) {
        if ((qtest_readb(qts, REG_BASE + 0x03) & 0xe6) == 0x46 &&
            !(qtest_readb(qts, REG_BASE + 0x94) & 1)) {
            break;
        }
        qtest_clock_step(qts, 1000);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe6, ==, 0x46);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x14), ==,
                    (guest_addr + 36) & 0xff);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x04, ==, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 36);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x88), ==,
                    guest_addr & 0xff);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x89), ==,
                    (guest_addr >> 8) & 0xff);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8a), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x08, ==, 0);
    qtest_memread(qts, 0x10000, bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); i++) {
        g_assert_cmphex(bytes[i], ==, 0xa5);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    return qts;
}

static void test_partial_fifo_flush(void)
{
    uint8_t finish[10 * 4];
    uint8_t bytes[36];
    QTestState *qts = start_pending_fifo(0x10000);
    size_t n = 0;

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x21, ==, 0);

    emit_or(finish, &n, 0x3a, 0x93);
    emit(finish, &n, branch_word(0xf, 2, 0x0b, 1));
    emit_or(finish, &n, 0xc0, 0x03);
    emit(finish, &n, alu_word(0, 0, 0x06, 0x22, false));
    emit_or(finish, &n, 0xe0, 0x03);
    emit(finish, &n, alu_word(0, 0, 0x06, 0x23, false));
    emit(finish, &n, branch_word(0xf, 8, 0x0c, 6));
    emit_or(finish, &n, 1, 0x9d);
    emit_or(finish, &n, 2, 0x91);
    emit(finish, &n, branch_word(8, 0, 0x65, 9));
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    start_program(qts, finish, n);
    for (unsigned i = 0; i < 1000; i++) {
        if (qtest_readb(qts, REG_BASE + 0x9e) == 1) {
            break;
        }
        qtest_clock_step(qts, 1000);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x88), ==, 36);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x14), ==, 36);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x21, ==, 0x21);
    qtest_memread(qts, 0x10000, bytes, sizeof(bytes));
    g_assert_cmphex(bytes[0], ==, 0);
    g_assert_cmpmem(bytes + 8, 4, "QEMU", 4);
    qtest_quit(qts);
}

static void manual_inquiry_bytes(uint8_t bytes[36])
{
    QTestState *qts = start_pending_fifo(0x10003);

    qtest_writeb(qts, REG_BASE + 0x93, 0x3a);
    qtest_memread(qts, 0x10003, bytes, 36);
    qtest_quit(qts);
}

static size_t build_auto_flush_program(uint8_t *program,
                                       uint8_t expected_phase, bool host_dma)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };
    size_t n = 0;
    unsigned wait_at, free_at, done_at;

    emit_or(program, &n, 0x08, 0x01);
    emit_or(program, &n, 0x07, 0x05);
    emit_or(program, &n, 0, 0x93);
    emit_or(program, &n, 0x80, 0x03);
    emit_or(program, &n, 0x40, 0x00);
    for (size_t i = 0; i < sizeof(cdb); i++) {
        emit_or(program, &n, cdb[i], 0x06);
    }
    emit_or(program, &n, expected_phase == 0xff ? 0xc0 : expected_phase,
            0x03);
    emit_or(program, &n, host_dma ? 0x38 : 0x30, 0x93);
    if (host_dma) {
        emit_or(program, &n, 1, 0x91);
    }
    wait_at = n / 4;
    emit(program, &n, branch_word(0xf, 0x08, 0x94, wait_at));
    emit_or(program, &n, 0xc0, 0x03);
    emit(program, &n, alu_word(0, 0, 0x06, 0x22, false));
    emit_or(program, &n, 0xe0, 0x03);
    emit(program, &n, alu_word(0, 0, 0x06, 0x23, false));
    free_at = n / 4;
    emit(program, &n, branch_word(0xf, 0x08, 0x0c, free_at));
    emit_or(program, &n, 1, 0x9d);
    emit_or(program, &n, 2, 0x91);
    done_at = n / 4;
    emit(program, &n, branch_word(8, 0, 0x65, done_at));
    return n;
}

static void prepare_short_inquiry(QTestState *qts, uint32_t addr,
                                  uint8_t stcnt, bool disabled)
{
    uint8_t guarded[42];

    memset(guarded, 0xa5, sizeof(guarded));
    qtest_memwrite(qts, 0x10000, guarded, sizeof(guarded));
    for (unsigned i = 0; i < 4; i++) {
        qtest_writeb(qts, REG_BASE + 0x88 + i, addr >> (8 * i));
    }
    qtest_writeb(qts, REG_BASE + 0x8c, 36);
    qtest_writeb(qts, REG_BASE + 0x08, stcnt);
    qtest_writeb(qts, REG_BASE + 0x1f, disabled ? 0x22 : 2);
    /* Distinguish automatic/manual flush from a threshold-started burst. */
    qtest_writeb(qts, REG_BASE + 0x86, 0xc0);
}

static QTestState *start_auto_flush_request(bool disabled, uint32_t addr)
{
    uint8_t program[128] = { 0 };
    uint8_t guarded[42];
    QTestState *qts = start_disk();

    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    prepare_short_inquiry(qts, addr, 36, disabled);
    start_program(qts, program,
                  build_auto_flush_program(program, 0xff, false));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x04, ==, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x14), ==, (addr + 36) & 0xff);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 36);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x88), ==, addr & 0xff);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x09, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    g_assert_false(qtest_get_irq(qts, 11));
    qtest_memread(qts, 0x10000, guarded, sizeof(guarded));
    for (size_t i = 0; i < sizeof(guarded); i++) {
        g_assert_cmphex(guarded[i], ==, 0xa5);
    }
    return qts;
}

static void test_auto_fifo_flush_after_host_dma_resumes(void)
{
    uint8_t guarded[42];
    uint8_t expected[36];
    QTestState *qts = start_auto_flush_request(false, 0x10003);

    manual_inquiry_bytes(expected);

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f) & 0x20, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 2);
    qtest_writeb(qts, REG_BASE + 0x93, 0x38);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x88), ==, 0x27);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x09, ==, 0x09);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    qtest_memread(qts, 0x10000, guarded, sizeof(guarded));
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(guarded[i], ==, 0xa5);
        g_assert_cmphex(guarded[39 + i], ==, 0xa5);
    }
    g_assert_cmphex(guarded[3], ==, 0);
    g_assert_cmpmem(guarded + 11, 4, "QEMU", 4);
    g_assert_cmpmem(guarded + 3, 36, expected, sizeof(expected));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 2);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_quit(qts);
}

static void test_auto_flush_disable_keeps_manual_flush(void)
{
    uint8_t guarded[42];
    QTestState *qts = start_auto_flush_request(true, 0x10003);

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f) & 0x20, ==, 0x20);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 0x38);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 36);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_memread(qts, 0x10000, guarded, sizeof(guarded));
    for (size_t i = 0; i < sizeof(guarded); i++) {
        g_assert_cmphex(guarded[i], ==, 0xa5);
    }
    qtest_writeb(qts, REG_BASE + 0x93, 0x3a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 2);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_quit(qts);
}

static void test_auto_flush_failed_pci_write(void)
{
    uint8_t guarded[42];
    QTestState *qts = start_auto_flush_request(false, 0x70000003);

    qtest_writeb(qts, REG_BASE + 0x93, 0x38);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 36);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x88), ==, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    qtest_memread(qts, 0x10000, guarded, sizeof(guarded));
    for (size_t i = 0; i < sizeof(guarded); i++) {
        g_assert_cmphex(guarded[i], ==, 0xa5);
    }
    qtest_quit(qts);
}

static void test_phase_mismatch_requests_fifo_flush(void)
{
    uint8_t program[128] = { 0 };
    uint8_t guarded[42];
    uint8_t expected[36];
    g_autofree char *dir = g_strdup("aic7880-autoflush-XXXXXX");
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *qts = start_disk();

    g_assert_nonnull(g_mkdtemp(dir));
    path = g_strdup_printf("%s/state", dir);
    uri = g_strdup_printf("file:%s", path);
    manual_inquiry_bytes(expected);
    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);

    prepare_short_inquiry(qts, 0x10003, 40, false);
    start_program(qts, program,
                  build_auto_flush_program(program, 0x40, false));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe6, ==, 0xc6);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x10, ==, 0x10);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x14), ==, 39);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 36);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_false(qtest_get_irq(qts, 11));
    qtest_memread(qts, 0x10000, guarded, sizeof(guarded));
    for (size_t i = 0; i < sizeof(guarded); i++) {
        g_assert_cmphex(guarded[i], ==, 0xa5);
    }
    qtest_qmp_assert_success(qts,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration(qts, "failed");
    unlink(path);
    g_rmdir(dir);
    qtest_writeb(qts, REG_BASE + 0x93, 0x38);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    qtest_memread(qts, 0x10000, guarded, sizeof(guarded));
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(guarded[i], ==, 0xa5);
        g_assert_cmphex(guarded[39 + i], ==, 0xa5);
    }
    g_assert_cmphex(guarded[3], ==, 0);
    g_assert_cmpmem(guarded + 11, 4, "QEMU", 4);
    g_assert_cmpmem(guarded + 3, 36, expected, sizeof(expected));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 2);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_quit(qts);
}

static void test_phase_mismatch_disable_requires_manual_flush(void)
{
    uint8_t program[128] = { 0 };
    uint8_t expected[36], guarded[42];
    QTestState *qts = start_disk();

    manual_inquiry_bytes(expected);
    prepare_short_inquiry(qts, 0x10003, 40, true);
    start_program(qts, program,
                  build_auto_flush_program(program, 0x40, false));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x10, ==, 0x10);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 0x38);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 0x3a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    qtest_memread(qts, 0x10000, guarded, sizeof(guarded));
    g_assert_cmpmem(guarded + 3, 36, expected, sizeof(expected));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 2);
    qtest_quit(qts);
}

static void test_phase_mismatch_flushes_with_host_dma_active(void)
{
    uint8_t program[128] = { 0 };
    uint8_t expected[36], guarded[42];
    QTestState *qts = start_disk();

    manual_inquiry_bytes(expected);
    prepare_short_inquiry(qts, 0x10003, 40, false);
    start_program(qts, program,
                  build_auto_flush_program(program, 0x40, true));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x10, ==, 0x10);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x88), ==, 0x27);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    qtest_memread(qts, 0x10000, guarded, sizeof(guarded));
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(guarded[i], ==, 0xa5);
        g_assert_cmphex(guarded[39 + i], ==, 0xa5);
    }
    g_assert_cmpmem(guarded + 3, 36, expected, sizeof(expected));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    resume_program(qts, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 2);
    qtest_quit(qts);
}

static void test_auto_flush_disable_reset(void)
{
    QTestState *qts = start_aic();

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f) & 0x20, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x1f, 0x22);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f) & 0x20, ==, 0x20);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f) & 0x20, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x1f, 0x22);
    qobject_unref(qtest_qmp(qts, "{'execute': 'system_reset'}"));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f) & 0x20, ==, 0);
    qtest_quit(qts);
}

static void test_phase_change_without_mismatch_is_unsupported(void)
{
    uint8_t program[128] = { 0 };
    QTestState *qts = start_disk();

    prepare_short_inquiry(qts, 0x10003, 40, false);
    start_program(qts, program,
                  build_auto_flush_program(program, 0xc0, false));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe6, ==, 0xc6);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x10, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 36);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    qtest_quit(qts);
}

static void test_partial_fifo_unaligned_host_address(void)
{
    uint8_t expected[36], guarded[42];
    QTestState *qts = start_pending_fifo(0x10000);

    qtest_writeb(qts, REG_BASE + 0x93, 0x3a);
    qtest_memread(qts, 0x10000, expected, sizeof(expected));
    g_assert_cmphex(expected[0], ==, 0);
    g_assert_cmpmem(expected + 8, 4, "QEMU", 4);
    qtest_quit(qts);

    qts = start_pending_fifo(0x10003);
    qtest_writeb(qts, REG_BASE + 0x93, 0x3a);
    qtest_memread(qts, 0x10000, guarded, sizeof(guarded));
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(guarded[i], ==, 0xa5);
        g_assert_cmphex(guarded[39 + i], ==, 0xa5);
    }
    g_assert_cmpmem(guarded + 3, 36, expected, sizeof(expected));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x88), ==, 0x27);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x14), ==, 39);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 9, ==, 9);
    qtest_quit(qts);
}

static void test_partial_fifo_flush_while_dma_disabled(void)
{
    uint8_t bytes[36];
    QTestState *qts = start_pending_fifo(0x10000);

    qtest_writeb(qts, REG_BASE + 0x93, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 36);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x14), ==, 36);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_memread(qts, 0x10000, bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); i++) {
        g_assert_cmphex(bytes[i], ==, 0xa5);
    }
    qtest_writeb(qts, REG_BASE + 0x93, 0x38);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 9, ==, 9);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 0);
    qtest_memread(qts, 0x10000, bytes, sizeof(bytes));
    g_assert_cmphex(bytes[0], ==, 0);
    g_assert_cmpmem(bytes + 8, 4, "QEMU", 4);
    qtest_quit(qts);
}

static void test_partial_fifo_flush_after_direction_change(void)
{
    uint8_t bytes[36];
    QTestState *qts = start_pending_fifo(0x10000);

    qtest_writeb(qts, REG_BASE + 0x93, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 4, ==, 4);
    qtest_writeb(qts, REG_BASE + 0x93, 6);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 36);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x14), ==, 36);
    qtest_memread(qts, 0x10000, bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); i++) {
        g_assert_cmphex(bytes[i], ==, 0xa5);
    }
    qtest_quit(qts);
}

static void test_partial_fifo_flush_without_bus_master(void)
{
    uint8_t bytes[36];
    QTestState *qts = start_pending_fifo(0x10000);

    config_writew(qts, 0x04, 0x0002);
    qtest_writeb(qts, REG_BASE + 0x93, 0x3a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x02, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 36);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x88), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x14), ==, 36);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    qtest_memread(qts, 0x10000, bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); i++) {
        g_assert_cmphex(bytes[i], ==, 0xa5);
    }
    qtest_quit(qts);
}

static void test_partial_fifo_reset(void)
{
    uint8_t bytes[36];
    QTestState *qts = start_pending_fifo(0x10000);

    qtest_writeb(qts, REG_BASE + 0x93, 0x39);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 1, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x14), ==, 36);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_memread(qts, 0x10000, bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); i++) {
        g_assert_cmphex(bytes[i], ==, 0xa5);
    }
    qtest_quit(qts);
}

static void test_pending_fifo_chip_reset(void)
{
    uint8_t bytes[36];
    QTestState *qts = start_pending_fifo(0x10000);

    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x88), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x14), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_memread(qts, 0x10000, bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); i++) {
        g_assert_cmphex(bytes[i], ==, 0xa5);
    }
    qtest_quit(qts);
}

static void test_partial_fifo_clear_channel(void)
{
    uint8_t bytes[36];
    QTestState *qts = start_pending_fifo(0x10000);
    uint8_t control = qtest_readb(qts, REG_BASE + 0x93);
    uint8_t status = qtest_readb(qts, REG_BASE + 0x94);

    /* An active SCSI DMA owner refuses channel clear without side effects. */
    qtest_writeb(qts, REG_BASE + 0x01, 0x0a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0x08);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93), ==, control);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94), ==, status);
    qtest_writeb(qts, REG_BASE + 0x93, 0);
    status = qtest_readb(qts, REG_BASE + 0x94);
    qtest_writeb(qts, REG_BASE + 0x01, 0x0a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0x08);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94), ==, status);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x14), ==, 36);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_memread(qts, 0x10000, bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); i++) {
        g_assert_cmphex(bytes[i], ==, 0xa5);
    }
    qtest_quit(qts);
}

static void test_unsupported_scsi_controls(void)
{
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x00, 0x20);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00) & 0x20, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    qtest_writeb(qts, REG_BASE + 0x93, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    qtest_writeb(qts, REG_BASE + 0x93, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 0x80, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    qtest_writeb(qts, REG_BASE + 0x10, 0x10);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x10), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    qtest_writeb(qts, REG_BASE + 0x11, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x11), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    qtest_writeb(qts, REG_BASE + 0x02, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x02), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x07), ==, 0xff);
    qtest_quit(qts);
}

static void test_manual_command_path_is_unsupported(void)
{
    QTestState *qts = start_disk();

    qtest_writeb(qts, REG_BASE + 0x05, 0x07);
    qtest_writeb(qts, REG_BASE + 0x00, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_quit(qts);
}

static void test_scb_pages_and_sequencer_access(void)
{
    uint8_t program[8] = { 0 };
    size_t n = 0;
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x90, 2);
    qtest_writeb(qts, REG_BASE + 0xa0, 0x35);
    qtest_writeb(qts, REG_BASE + 0xbf, 0x79);
    qtest_writeb(qts, REG_BASE + 0x90, 3);
    qtest_writeb(qts, REG_BASE + 0xa0, 0x46);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0), ==, 0x46);
    qtest_writeb(qts, REG_BASE + 0x90, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0), ==, 0x35);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xbf), ==, 0x79);

    /* Copy an SCB byte through the sequencer's register bus. */
    emit(program, &n, alu_word(0, 0, 0xa0, 0x20, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x35);

    qtest_writeb(qts, REG_BASE + 0x90, 16);
    qtest_writeb(qts, REG_BASE + 0xa0, 0xee);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0), ==, 0xee);
    qtest_writeb(qts, REG_BASE + 0x90, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0), ==, 0xee);
    qtest_writeb(qts, REG_BASE + 0x90, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0), ==, 0x35);
    qtest_quit(qts);
}

static void test_scb_auto_addressing(void)
{
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x90, 2);
    qtest_writeb(qts, REG_BASE + 0xbe, 0x11);
    qtest_writeb(qts, REG_BASE + 0xbf, 0x22);
    qtest_writeb(qts, REG_BASE + 0xa0, 0x33);
    qtest_writeb(qts, REG_BASE + 0x9a, 0x9e);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa5), ==, 0x11);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa5), ==, 0x22);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa5), ==, 0x33);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9a), ==, 0x81);
    qtest_writeb(qts, REG_BASE + 0xa5, 0x44);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9a), ==, 0x82);
    qtest_writeb(qts, REG_BASE + 0x9a, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa1), ==, 0x44);
    qtest_writeb(qts, REG_BASE + 0x90, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa1), ==, 0);
    qtest_quit(qts);
}

static void test_queue_in_trace(void)
{
    uint8_t program[8] = { 0 };
    g_autofree char *dir = g_dir_make_tmp("aic7880-qin-trace-XXXXXX", NULL);
    g_autofree char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    size_t n = 0;

    g_assert_nonnull(dir);
    log_path = g_build_filename(dir, "qemu.trace", NULL);
    qts = qtest_initf("-M pc -m 128M -nodefaults -D %s "
                      "-trace enable=aic7880_* "
                      "-device aic7880,id=hba,bus=pci.0,addr=04.0",
                      log_path);
    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    qtest_writeb(qts, REG_BASE + 0x9b, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);
    qtest_writeb(qts, REG_BASE + 0x21, 0xa5);
    emit(program, &n, alu_word(1, 0xff, 0x9b, 0x20, false));
    emit(program, &n, alu_word(1, 0xff, 0x9b, 0x21, false));
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x9b, 16);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 0);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "aic7880_qinfifo devfn 0x20 enqueue 1 "
                            "id 0x03 before 0 after 1 paused 1 running 0 "
                            "accepted 1"));
    g_assert_nonnull(strstr(log, "aic7880_qinfifo devfn 0x20 enqueue 0 "
                            "id 0x03 before 1 after 0 paused 0 running 1 "
                            "accepted 1"));
    g_assert_nonnull(strstr(log, "aic7880_qinfifo devfn 0x20 enqueue 0 "
                            "id 0xff before 0 after 0 paused 0 running 1 "
                            "accepted 0"));
    g_assert_nonnull(strstr(log, "aic7880_qinfifo devfn 0x20 enqueue 1 "
                            "id 0x10 before 0 after 0 paused 1 running 0 "
                            "accepted 0"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_queue_in_requires_sequencer(void)
{
    uint8_t program[12];
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x90, 2);
    qtest_writeb(qts, REG_BASE + 0xa0, 0xa1);
    qtest_writeb(qts, REG_BASE + 0x90, 5);
    qtest_writeb(qts, REG_BASE + 0xa0, 0xb2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x9b, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);
    qtest_writeb(qts, REG_BASE + 0x9b, 5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);

    emit(program, &n, alu_word(0, 0, 0x9b, 0x20, false));
    emit(program, &n, alu_word(0, 0, 0x9b, 0x21, false));
    emit(program, &n, alu_word(0, 1, 0x6a, 0x91, false));
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, 5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x90, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0), ==, 0xa1);
    qtest_writeb(qts, REG_BASE + 0x90, 5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0), ==, 0xb2);
    qtest_writeb(qts, REG_BASE + 0x9b, 16);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    qtest_quit(qts);
}

static void test_indirect_source_advances_index(void)
{
    uint8_t program[12];
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x30, 0x12);
    qtest_writeb(qts, REG_BASE + 0x31, 0x34);
    qtest_writeb(qts, REG_BASE + 0x65, 0x30);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x30);
    emit(program, &n, alu_word(0, 0, 0x6c, 0x40, false));
    emit(program, &n, alu_word(0, 0, 0x6c, 0x41, false));
    emit(program, &n, alu_word(0, 1, 0x6a, 0x91, false));
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0x12);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0x34);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x32);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x65, 0x30);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6c), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x30);
    qtest_quit(qts);
}

static void test_indirect_sequencer_sources(void)
{
    uint8_t program[8];
    QTestState *qts = start_aic();
    size_t n = 0;

    emit(program, &n, alu_word(0, 0, 0x6c, 0x40, false));
    emit_or(program, &n, 1, 0x91);
    qtest_writeb(qts, REG_BASE + 0x40, 0xa5);
    qtest_writeb(qts, REG_BASE + 0x9b, 2);
    qtest_writeb(qts, REG_BASE + 0x65, 0x6a);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x6b);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);

    qtest_writeb(qts, REG_BASE + 0x65, 0x9b);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x9c);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x65, 0xff);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    qtest_writeb(qts, REG_BASE + 0x65, 0x6c);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x6d);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    qtest_quit(qts);
}

static void test_sequencer_fault_trace(void)
{
    uint8_t program[8];
    g_autofree char *dir = g_dir_make_tmp("aic7880-seq-trace-XXXXXX",
                                          NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.trace", NULL);
    g_autofree char *log = NULL;
    QTestState *qts;
    size_t n = 0;

    g_assert_nonnull(dir);
    qts = qtest_initf("-M pc -m 128M -nodefaults -D %s "
        "-trace enable=aic7880_seq_data "
        "-trace enable=aic7880_seq_index_write "
        "-trace enable=aic7880_seq_indirect "
        "-trace enable=aic7880_seq_host_only "
        "-device aic7880,id=hba,bus=pci.0,addr=04.0", log_path);
    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    qtest_writeb(qts, REG_BASE + 0x90, 0);
    qtest_writeb(qts, REG_BASE + 0xa0, 0x91);
    emit(program, &n, alu_word(1, 0xff, 0xa0, 0x65, false));
    emit(program, &n, alu_word(0, 0, 0x6c, 0x40, false));
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "aic7880_seq_data devfn 0x20 pc 0x000 "
                            "insn 0x0265a0ff reg 0xa0 value 0x91"));
    g_assert_nonnull(strstr(log, "aic7880_seq_index_write devfn 0x20 "
                            "pc 0x000 insn 0x0265a0ff reg 0x65 value 0x91"));
    g_assert_nonnull(strstr(log, "aic7880_seq_indirect devfn 0x20 "
                            "pc 0x001 insn 0x00406c00 sindex 0x91"));
    g_assert_nonnull(strstr(log, "aic7880_seq_host_only devfn 0x20 "
                            "pc 0x001 insn 0x00406c00 src 0x6c dst 0x40 "
                            "effective 0x91"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_dindex_latch(void)
{
    uint8_t program[12] = { 0 };
    QTestState *qts = start_aic();
    size_t n = 0;

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x65, 0x53);
    qtest_writeb(qts, REG_BASE + 0x66, 0x20);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0x20);
    config_writel(qts, 0x10, 0xc001);
    config_writew(qts, 0x04, 0x0007);
    g_assert_cmphex(qtest_inb(qts, 0xc066), ==, 0x20);
    qtest_outb(qts, 0xc066, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x53);

    emit_or(program, &n, 0x55, 0x66);
    emit(program, &n, alu_word(1, 0xff, 0x66, 0x30, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0x55);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 0x55);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x53);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_dindir_scratch_writes(void)
{
    uint8_t program[12] = { 0 };
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x65, 0x53);
    qtest_writeb(qts, REG_BASE + 0x66, 0x20);
    qtest_writeb(qts, REG_BASE + 0x20, 0x91);
    qtest_writeb(qts, REG_BASE + 0x21, 0x92);
    qtest_writeb(qts, REG_BASE + 0x22, 0x93);
    qtest_writeb(qts, REG_BASE + 0x23, 0x94);
    emit(program, &n, alu_word(1, 0x5a, 0x69, 0x6d, false));
    emit(program, &n, alu_word(1, 0xa5, 0x69, 0x6d, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x22), ==, 0x93);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x23), ==, 0x94);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0x22);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x53);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 1);
    qtest_quit(qts);
}

static void test_dindir_independent_pointers(void)
{
    uint8_t program[8] = { 0 };
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x30, 0x6e);
    qtest_writeb(qts, REG_BASE + 0x31, 0x77);
    qtest_writeb(qts, REG_BASE + 0x40, 0xa5);
    qtest_writeb(qts, REG_BASE + 0x41, 0xb6);
    qtest_writeb(qts, REG_BASE + 0x65, 0x30);
    qtest_writeb(qts, REG_BASE + 0x66, 0x40);
    emit(program, &n, alu_word(1, 0xff, 0x6c, 0x6d, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 0x6e);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x31), ==, 0x77);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0x6e);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0xb6);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x31);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0x41);
    qtest_writeb(qts, REG_BASE + 0x66, 0x42);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x31);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_dindir_equal_scratch_indexes(void)
{
    uint8_t program[8] = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-equal-scratch-XXXXXX",
                                         &error);
    g_autofree char *log_path = NULL;
    QTestState *qts;
    size_t n = 0;

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    qts = start_aic_logged(log_path);
    qtest_writeb(qts, REG_BASE + 0x20, 0x7e);
    qtest_writeb(qts, REG_BASE + 0x21, 0x5a);
    qtest_writeb(qts, REG_BASE + 0x22, 0x3c);
    qtest_writeb(qts, REG_BASE + 0x65, 0x21);
    qtest_writeb(qts, REG_BASE + 0x66, 0x21);
    emit(program, &n, alu_word(1, 0x0f, 0x6c, 0x6d, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x7e);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, 0x0a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x22), ==, 0x3c);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x22);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0x22);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 1);
    qtest_quit(qts);
    assert_no_aic_log(log_path);
    unlink(log_path);
    g_rmdir(dir);
}

static void test_dindir_equal_haddr_indexes(void)
{
    uint8_t program[8] = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-equal-haddr-XXXXXX", &error);
    g_autofree char *log_path = NULL;
    QTestState *qts;
    size_t n = 0;

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    qts = start_aic_logged(log_path);
    /* Reset selects the modeled fixed low HADDR view. */
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 0x38, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x88, 0xc3);
    qtest_writeb(qts, REG_BASE + 0x89, 0x5a);
    qtest_writeb(qts, REG_BASE + 0x8a, 0x7e);
    qtest_writeb(qts, REG_BASE + 0x65, 0x89);
    qtest_writeb(qts, REG_BASE + 0x66, 0x89);
    emit(program, &n, alu_word(1, 0x0f, 0x6c, 0x6d, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x88), ==, 0xc3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x89), ==, 0x0a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8a), ==, 0x7e);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x8a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0x8a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 1);
    qtest_quit(qts);
    assert_no_aic_log(log_path);
    unlink(log_path);
    g_rmdir(dir);
}

static void test_dindir_equal_sblkctl_unsupported(void)
{
    uint8_t program[8] = { 0 };
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x1f, 0x22);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f) & 0x20, ==, 0x20);
    qtest_writeb(qts, REG_BASE + 0x65, 0x1f);
    qtest_writeb(qts, REG_BASE + 0x66, 0x1f);
    emit(program, &n, alu_word(1, 0x0f, 0x6c, 0x6d, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f) & 0x20, ==, 0x20);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 0);
    qtest_quit(qts);
}

static void test_dindir_equal_hcnt_unsupported(void)
{
    uint8_t program[8] = { 0 };
    QTestState *qts = start_aic();
    size_t n = 0;

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 0x38, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x8c, 0x5a);
    qtest_writeb(qts, REG_BASE + 0x8d, 0x7e);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 0x5a);
    qtest_writeb(qts, REG_BASE + 0x65, 0x8c);
    qtest_writeb(qts, REG_BASE + 0x66, 0x8c);
    emit(program, &n, alu_word(1, 0x0f, 0x6c, 0x6d, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8d), ==, 0x7e);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 0);
    qtest_quit(qts);
}

static void test_dindir_special_destinations(void)
{
    uint8_t program[8] = { 0 };
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x66, 0x91);
    emit(program, &n, alu_word(1, 1, 0x69, 0x6d, false));
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0x92);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);

    qts = start_aic();
    n = 0;
    qtest_writeb(qts, REG_BASE + 0x66, 0x9d);
    emit(program, &n, alu_word(1, 0x0a, 0x69, 0x6d, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, 0x0a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0x9e);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_dindir_access_restrictions(void)
{
    uint8_t program[8] = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-dindir-host-XXXXXX", &error);
    g_autofree char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    size_t n = 0;

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    qts = qtest_initf("-M pc -m 128M -nodefaults -d unimp -D %s "
                      "-device aic7880,id=hba,bus=pci.0,addr=04.0",
                      log_path);
    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    qtest_writeb(qts, REG_BASE + 0x66, 0x20);
    qtest_writeb(qts, REG_BASE + 0x20, 0xa5);
    (void)qtest_readb(qts, REG_BASE + 0x6d);
    qtest_writeb(qts, REG_BASE + 0x6d, 0x55);
    config_writel(qts, 0x10, 0xc001);
    config_writew(qts, 0x04, 0x0007);
    (void)qtest_inb(qts, 0xc06d);
    qtest_outb(qts, 0xc06d, 0x66);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0x20);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "host read of DINDIR is unsupported"));
    g_assert_nonnull(strstr(log, "host write to DINDIR is unsupported"));

    qts = start_aic();
    qtest_writeb(qts, REG_BASE + 0x66, 0x20);
    qtest_writeb(qts, REG_BASE + 0x30, 0xb6);
    emit(program, &n, alu_word(1, 0xff, 0x6d, 0x30, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 0xb6);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0x20);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 0);
    qtest_quit(qts);
    unlink(log_path);
    g_rmdir(dir);
}

static void test_dindir_rejects_sindex_target(void)
{
    uint8_t program[8] = { 0 };
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x65, 0x53);
    qtest_writeb(qts, REG_BASE + 0x66, 0x65);
    qtest_writeb(qts, REG_BASE + 0x20, 0xa5);
    emit(program, &n, alu_word(1, 0x5a, 0x69, 0x6d, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 0);
    qtest_quit(qts);
}

static void test_dindir_rejects_dindex_target(void)
{
    uint8_t program[8] = { 0 };
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x65, 0x53);
    qtest_writeb(qts, REG_BASE + 0x66, 0x66);
    qtest_writeb(qts, REG_BASE + 0x20, 0xa5);
    emit(program, &n, alu_word(1, 0x5a, 0x69, 0x6d, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 0);
    qtest_quit(qts);
}

static void test_dindir_unmodeled_target(void)
{
    uint8_t program[8] = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-dindir-target-XXXXXX",
                                         &error);
    g_autofree char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    size_t n = 0;

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    qts = qtest_initf("-M pc -m 128M -nodefaults -d guest_errors -D %s "
                      "-device aic7880,id=hba,bus=pci.0,addr=04.0",
                      log_path);
    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    run_old_alu(qts, 3, 1, 0x69, 0x47); /* STC */
    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    qtest_writeb(qts, REG_BASE + 0x66, 0x6b); /* read-only FLAGS */
    qtest_writeb(qts, REG_BASE + 0x30, 0xa5);
    emit(program, &n, alu_word(1, 0x5a, 0x69, 0x6d, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 0);
    g_test_message("DINDEX=0x%02x ERROR=0x%02x",
                   qtest_readb(qts, REG_BASE + 0x66),
                   qtest_readb(qts, REG_BASE + 0x92));
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "unsupported DINDIR destination"));
    unlink(log_path);
    g_rmdir(dir);
}

static void test_dindex_resets(void)
{
    QTestState *qts = start_aic();

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x66, 0x44);
    qtest_writeb(qts, REG_BASE + 0x87, 1); /* CHIPRST */
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x66, 0x55);
    qtest_system_reset(qts);
    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0);
    qtest_quit(qts);
}

static void test_dindir_migrates(void)
{
    uint8_t first[8] = { 0 };
    uint8_t next[8] = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-dindir-XXXXXX", &error);
    g_autofree char *uri = NULL;
    g_autofree char *socket_path = NULL;
    QTestState *from, *to;
    size_t n = 0;

    g_assert_no_error(error);
    socket_path = g_strdup_printf("%s/migrate.sock", dir);
    uri = g_strdup_printf("unix:%s", socket_path);
    from = start_aic();
    to = qtest_init("-M pc -m 128M -nodefaults -S -incoming defer "
                    "-device aic7880,id=hba,bus=pci.0,addr=04.0");
    qtest_writeb(from, REG_BASE + 0x66, 0x20);
    qtest_writeb(from, REG_BASE + 0x20, 0x91);
    qtest_writeb(from, REG_BASE + 0x21, 0x92);
    qtest_writeb(from, REG_BASE + 0x22, 0x93);
    emit(first, &n, alu_word(1, 0x5a, 0x69, 0x6d, false));
    emit_or(first, &n, 1, 0x91);
    start_program(from, first, n);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x20), ==, 0x5a);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x66), ==, 0x21);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x66), ==, 0x21);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x20), ==, 0x5a);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x21), ==, 0x92);
    qtest_writeb(to, REG_BASE + 0x87, 0x04);
    qtest_writeb(to, REG_BASE + 0x92, 1);
    n = 0;
    emit(next, &n, alu_word(1, 0xa5, 0x69, 0x6d, false));
    emit_or(next, &n, 1, 0x91);
    start_program(to, next, n);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x21), ==, 0xa5);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x22), ==, 0x93);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x66), ==, 0x22);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x92), ==, 0);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket_path);
    g_rmdir(dir);
}

static void test_queue_reset_and_reuse(void)
{
    uint8_t program[8];
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x9b, 2);
    qtest_writeb(qts, REG_BASE + 0x9b, 5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 2);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x9b, 7);
    emit(program, &n, alu_word(0, 0, 0x9b, 0x20, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 7);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void check_unconditional_branch_transfer(uint8_t op)
{
    static const struct {
        uint8_t source, immediate, result, index;
    } cases[] = {
        { 0x30, 0, 0x24, 0x56 },     /* MOV */
        { 0x6a, 0x49, 0x49, 0x56 },  /* MVI */
        { 0x30, 0x81, 0xa5, 0x56 },  /* OR */
        { 0x6a, 0, 0, 0x56 },        /* Literal zero, despite nonzero ACCUM. */
        { 0x65, 0, 0x56, 0x56 },     /* Plain branch. */
        { 0x90, 0, 5, 0x56 },
        { 0x6c, 0, 0x3c, 0x56 },
        { 0x6c, 0, 0x66, 0x65 }, /* Read the incremented index itself. */
    };

    for (unsigned carry = 0; carry < 2; carry++) {
        for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
            uint8_t program[7 * 4];
            QTestState *qts = start_aic();
            size_t n = 0;
            uint8_t flags = carry;

            if (!cases[i].result) {
                flags |= 2;
            }
            qtest_writeb(qts, REG_BASE + 0x30, 0x24);
            qtest_writeb(qts, REG_BASE + 0x56, 0x3c);
            qtest_writeb(qts, REG_BASE + 0x65, cases[i].index);
            qtest_writeb(qts, REG_BASE + 0x64, 0xa3);
            qtest_writeb(qts, REG_BASE + 0x90, 5);
            qtest_writeb(qts, REG_BASE + 0x40, 0xa0);
            qtest_writeb(qts, REG_BASE + 0x41, 0xb1);
            qtest_writeb(qts, REG_BASE + 0x42, 0xc2);
            qtest_writeb(qts, REG_BASE + 0x67, 4);
            qtest_writeb(qts, REG_BASE + 0x68, 0);
            emit(program, &n, alu_word(3, 1, carry ? 0x69 : 0x6a,
                                       0x57, false));
            emit(program, &n, branch_word(op, cases[i].immediate,
                                          cases[i].source, 4));
            emit_or(program, &n, 0x71, 0x41);
            emit_or(program, &n, 1, 0x91);
            emit_or(program, &n, 0x62, 0x40);
            if (op == 0x0b) {
                emit(program, &n, alu_word(1, 0xff, 0x6a, 0x58, true));
            } else {
                emit_or(program, &n, 1, 0x91);
            }
            emit_or(program, &n, 0x93, 0x42);
            start_program(qts, program, n);
            wait_paused(qts);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 4);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==,
                            cases[i].result);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==,
                            flags);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x64), ==, 0xa3);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 0x24);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x56), ==, 0x3c);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x90), ==, 5);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0xa0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0xb1);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
            qtest_writeb(qts, REG_BASE + 0x68, 0x80);
            qtest_writeb(qts, REG_BASE + 0x92, 8);
            resume_program(qts, 2);
            wait_paused(qts);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0x62);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==,
                            op == 0x0b ? 0x71 : 0xb1);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x42), ==, 0xc2);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 1);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
            qtest_quit(qts);
        }
    }
}

static void test_jmp_transfer(void)
{
    check_unconditional_branch_transfer(8);
}

static void test_call_transfer(void)
{
    check_unconditional_branch_transfer(0x0b);
}

static void test_branch_host_only_source(void)
{
    static const uint8_t ops[] = { 8, 9, 0x0a, 0x0b };

    for (unsigned i = 0; i < ARRAY_SIZE(ops); i++) {
        uint8_t program[4 * 4];
        QTestState *qts = start_aic();
        size_t n = 0;

        qtest_writeb(qts, REG_BASE + 0x65, 0x56);
        qtest_writeb(qts, REG_BASE + 0x40, 0xa5);
        emit(program, &n, alu_word(3, 1, 0x69, 0x57, false));
        emit(program, &n, branch_word(ops[i], 0, 0x91, 3));
        emit_or(program, &n, 1, 0x91);
        emit_or(program, &n, 0x7b, 0x40);
        start_program(qts, program, n);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x56);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0xa5);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        qtest_quit(qts);
    }
}

static void test_call_pointer_arithmetic(void)
{
    static const uint32_t bases[] = { 0x2ff80, 0x2fff0 };
    static const uint8_t entry[] = { 0x27, 0x54, 0x81, 0xae };
    const unsigned id = 10;

    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        uint8_t table[64], got[64], program[18 * 4];
        QTestState *qts = start_aic();
        size_t n = 0;

        memset(table, 0xa5, sizeof(table));
        memcpy(table + id * 4, entry, sizeof(entry));
        qtest_memwrite(qts, bases[i], table, sizeof(table));
        qtest_writeb(qts, REG_BASE + 0x90, id);
        qtest_writeb(qts, REG_BASE + 0x65, 0x23);
        qtest_writeb(qts, REG_BASE + 0x64, 0xa3);
        for (unsigned byte = 0; byte < 4; byte++) {
            qtest_writeb(qts, REG_BASE + 0x34 + byte,
                         bases[i] >> (byte * 8));
        }
        qtest_writeb(qts, REG_BASE + 0x67, 1);
        qtest_writeb(qts, REG_BASE + 0x68, 0);
        emit(program, &n, branch_word(0x0b, 0, 0x90, 12));
        emit_or(program, &n, 4, 0x8c);
        emit_or(program, &n, 1, 0x93);
        emit_or(program, &n, 4, 0x93);
        emit_or(program, &n, 0x0c, 0x93);
        for (unsigned byte = 0; byte < 4; byte++) {
            emit(program, &n, alu_word(1, 0xff, 0x99, 0x44 + byte, false));
        }
        emit_or(program, &n, 1, 0x91);
        emit_or(program, &n, 0x7b, 0x40);
        emit_or(program, &n, 0x8c, 0x41);
        emit(program, &n, alu_word(5, 0x22, 0x65, 0x64, false));
        emit(program, &n, alu_word(3, 0, 0x34, 0x88, false));
        emit(program, &n, alu_word(1, 0xff, 0x6a, 0x64, false));
        for (unsigned byte = 1; byte < 4; byte++) {
            emit(program, &n, alu_word(4, 0, 0x34 + byte, 0x88 + byte,
                                       byte == 3));
        }
        start_program(qts, program, n);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==,
                        bases[i] + id * 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, id);
        qtest_writeb(qts, REG_BASE + 0x68, 0x80);
        qtest_writeb(qts, REG_BASE + 0x92, 8);
        resume_program(qts, 2);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 1);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==,
                        bases[i] + id * 4 + 4);
        for (unsigned byte = 0; byte < 4; byte++) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x44 + byte), ==,
                            entry[byte]);
        }
        qtest_memread(qts, bases[i], got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), table, sizeof(table));
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        qtest_quit(qts);
    }
}

static void load_seq_word(QTestState *qts, uint16_t pc, uint32_t word)
{
    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    qtest_writeb(qts, REG_BASE + 0x62, pc);
    qtest_writeb(qts, REG_BASE + 0x63, pc >> 8);
    for (unsigned byte = 0; byte < 4; byte++) {
        qtest_writeb(qts, REG_BASE + 0x61, word >> (byte * 8));
    }
    qtest_writeb(qts, REG_BASE + 0x60, 0x90);
}

static void check_carry_branch_transfer(uint8_t opcode)
{
    static const struct CarryCase {
        bool carry;
        bool zero_before;
        uint8_t source;
        uint8_t source_value;
        uint8_t immediate;
        uint8_t index_before;
        uint8_t index_after;
        bool jc_taken, jnc_taken;
    } cases[] = {
        { false, true,  0x31, 0x5a, 0,    0x12, 0x5a, false, true  },
        { true,  false, 0x31, 0,    0,    0x12, 0,    true,  false },
        { false, false, 0x31, 0,    0,    0x12, 0,    false, true  },
        { true,  false, 0x31, 0x30, 0x04, 0x12, 0x34, true,  false },
        { false, true,  0x65, 0,    0,    0x7d, 0x7d, false, true  },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        const struct CarryCase *test = &cases[i];
        QTestState *qts = start_aic();
        bool taken = opcode == 9 ? test->jc_taken : test->jnc_taken;
        uint8_t flags_before = (test->carry ? 1 : 0) |
                               (test->zero_before ? 2 : 0);
        uint8_t flags_after = (test->carry ? 1 : 0) |
                              (test->index_after == 0 ? 2 : 0);

        qtest_writeb(qts, REG_BASE + 0x30, 0x5a);
        qtest_writeb(qts, REG_BASE + 0x31, test->source_value);
        qtest_writeb(qts, REG_BASE + 0x64, 0x7e);
        qtest_writeb(qts, REG_BASE + 0x65, test->index_before);
        run_old_alu(qts, 3, (test->carry ? 1 : 0xff),
                    (test->carry ? 0x69 : 0x6a), 0x54);
        run_old_alu(qts, 2, (test->zero_before ? 0x5a : 1),
                    0x30, 0x55);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==,
                        flags_before);

        load_seq_word(qts, 255, branch_word(opcode, test->immediate,
                                           test->source, 259));
        load_seq_word(qts, 256, alu_word(0, 0xa1, 0x6a, 0x40, false));
        load_seq_word(qts, 257, alu_word(0, 1, 0x6a, 0x91, false));
        load_seq_word(qts, 259, alu_word(0, 0xb2, 0x6a, 0x41, false));
        load_seq_word(qts, 260, alu_word(0, 1, 0x6a, 0x91, false));
        qtest_writeb(qts, REG_BASE + 0x92, 1);
        qtest_writeb(qts, REG_BASE + 0x62, 255);
        qtest_writeb(qts, REG_BASE + 0x63, 0);
        resume_program(qts, 2);
        wait_paused(qts);

        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==,
                        (taken ? 0 : 0xa1));
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==,
                        (taken ? 0xb2 : 0));
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==,
                        (taken ? 5 : 2));
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x63), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==,
                        test->index_after);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==,
                        flags_after);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x64), ==, 0x7e);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x31), ==,
                        test->source_value);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 4, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
        qtest_quit(qts);
    }
}

static void test_jc_branch_transfer(void)
{
    check_carry_branch_transfer(9);
}

static void test_jnc_branch_transfer(void)
{
    check_carry_branch_transfer(0x0a);
}

static void test_conditional_branches(void)
{
    uint8_t program[13 * 4];
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x30, 0x5a);
    qtest_writeb(qts, REG_BASE + 0x31, 0x0f);
    emit(program, &n, branch_word(0xc, 0x5a, 0x30, 2));
    emit_or(program, &n, 1, 0x40);
    emit(program, &n, branch_word(0xc, 0x33, 0x30, 4));
    emit_or(program, &n, 1, 0x41);
    emit(program, &n, branch_word(0xd, 0xf0, 0x31, 6));
    emit_or(program, &n, 2, 0x42);
    emit(program, &n, branch_word(0xd, 1, 0x31, 8));
    emit_or(program, &n, 2, 0x43);
    emit(program, &n, branch_word(0xe, 0x33, 0x30, 10));
    emit_or(program, &n, 3, 0x44);
    emit(program, &n, branch_word(0xe, 0x5a, 0x30, 12));
    emit_or(program, &n, 3, 0x45);
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x42), ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x43), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x44), ==, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x45), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_accumulator_immediate_source(void)
{
    uint8_t program[5 * 4];
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x64, 0x33);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x64), ==, 0x33);
    qtest_writeb(qts, REG_BASE + 0x30, 0x5a);
    qtest_writeb(qts, REG_BASE + 0x31, 0x0f);
    emit(program, &n, alu_word(0, 0x5a, 0x6a, 0x64, false));
    emit(program, &n, alu_word(1, 0, 0x31, 0x40, false));
    emit(program, &n, branch_word(0xe, 0, 0x30, 4));
    emit_or(program, &n, 0x99, 0x41);
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x64), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0x0a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_rejected_branch_does_not_read_source(void)
{
    uint8_t program[4];
    QTestState *qts = start_aic();
    uint32_t word = branch_word(7, 1, 0x9b, 0);

    /* Opcode 7 must refuse before consuming QINFIFO. */

    for (unsigned byte = 0; byte < sizeof(program); byte++) {
        program[byte] = word >> (byte * 8);
    }
    qtest_writeb(qts, REG_BASE + 0x9b, 2);
    qtest_writeb(qts, REG_BASE + 0x65, 0x30);
    start_program(qts, program, sizeof(program));
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x30);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    qtest_quit(qts);
}

static void test_return_write_pauses_at_caller(void)
{
    uint8_t program[5 * 4];
    QTestState *qts = start_aic();
    size_t n = 0;

    emit(program, &n, branch_word(0xb, 0, 0x65, 3));
    emit_or(program, &n, 0x11, 0x40);
    emit_or(program, &n, 1, 0x91);
    emit(program, &n, alu_word(1, 0xff, 0x69, 0x91, true));
    emit_or(program, &n, 0x55, 0x41);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x92, 0x0f);
    resume_program(qts, 2);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0x11);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_failed_return_keeps_subroutine_pc(void)
{
    for (unsigned op = 0; op <= 1; op++) {
        uint8_t program[5 * 4];
        QTestState *qts = start_aic();
        size_t n = 0;

        qtest_writeb(qts, REG_BASE + 0x11, 0x40);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
        qtest_writeb(qts, REG_BASE + 0x92, 8);
        qtest_writeb(qts, REG_BASE + 0x87, 1);
        emit(program, &n, branch_word(0xb, 0, 0x65, 3));
        emit_or(program, &n, 0x11, 0x40);
        emit_or(program, &n, 1, 0x91);
        emit(program, &n, alu_word(op, 0x80, 0x69, 0x93, true));
        emit_or(program, &n, 0x55, 0x41);
        start_program(qts, program, n);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
        qtest_quit(qts);
    }
}

static void test_return_cannot_reset_host_control(void)
{
    uint8_t program[5 * 4];
    QTestState *qts = start_aic();
    size_t n = 0;

    emit(program, &n, branch_word(0xb, 0, 0x65, 3));
    emit_or(program, &n, 0x11, 0x40);
    emit_or(program, &n, 1, 0x91);
    emit(program, &n, alu_word(1, 1, 0x69, 0x87, true));
    emit_or(program, &n, 0x55, 0x41);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 5, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_quit(qts);
}

static void test_sequencer_cannot_pop_host_completion(void)
{
    for (unsigned indirect = 0; indirect < 2; indirect++) {
        uint8_t complete[8];
        uint8_t read_program[8];
        QTestState *qts = start_aic();
        size_t n = 0;

        emit_or(complete, &n, 5, 0x9d);
        emit_or(complete, &n, 1, 0x91);
        start_program(qts, complete, n);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
        qtest_writeb(qts, REG_BASE + 0x92, 1);
        qtest_writeb(qts, REG_BASE + 0x20, 0xa5);
        if (indirect) {
            qtest_writeb(qts, REG_BASE + 0x65, 0x9d);
        }
        n = 0;
        emit(read_program, &n,
             alu_word(0, 0, indirect ? 0x6c : 0x9d, 0x20, false));
        emit(read_program, &n, branch_word(8, 0, 0x65, 1));
        start_program(qts, read_program, n);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0xa5);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==,
                        0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, 5);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        qtest_quit(qts);
    }
}

static void test_other_host_only_sequencer_sources(void)
{
    static const uint8_t sources[] = { 0x87, 0x91, 0x92, 0x9e };

    for (size_t i = 0; i < ARRAY_SIZE(sources); i++) {
        uint8_t program[8];
        QTestState *qts = start_aic();
        size_t n = 0;

        qtest_writeb(qts, REG_BASE + 0x20, 0xa5);
        emit(program, &n, alu_word(0, 0, sources[i], 0x20, false));
        emit(program, &n, branch_word(8, 0, 0x65, 1));
        start_program(qts, program, n);
        wait_paused(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0xa5);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==,
                        0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
        qtest_quit(qts);
    }
}

static void test_sequencer_cannot_clear_host_interrupt(void)
{
    uint8_t cause[4];
    uint8_t clear_program[8];
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    emit_or(cause, &n, 1, 0x91);
    start_program(qts, cause, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
    g_assert_true(qtest_get_irq(qts, 11));

    n = 0;
    emit_or(clear_program, &n, 1, 0x92);
    emit(clear_program, &n, branch_word(8, 0, 0x65, 1));
    start_program(qts, clear_program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0x92, 9);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 0);
    g_assert_false(qtest_get_irq(qts, 11));
    qtest_quit(qts);
}

static void test_assembler_return_to_none(void)
{
    uint8_t program[5 * 4];
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-ret-XXXXXX", &error);
    g_autofree char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    size_t n = 0;

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    qts = qtest_initf("-M pc -m 128M -nodefaults -d unimp -D %s "
                      "-device aic7880,id=hba,bus=pci.0,addr=04.0",
                      log_path);
    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    emit(program, &n, branch_word(0xb, 0, 0x65, 3));
    emit_or(program, &n, 0x11, 0x40);
    emit_or(program, &n, 1, 0x91);
    emit(program, &n, alu_word(1, 0xff, 0x6a, 0x6a, true));
    emit_or(program, &n, 0x55, 0x41);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0x11);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x07, 0x12);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "unsupported register 0x07"));
    g_assert_null(strstr(log, "unsupported register 0x6a"));
    unlink(log_path);
    g_rmdir(dir);
}

static void test_four_nested_calls(void)
{
    uint8_t program[19 * 4];
    QTestState *qts = start_aic();
    size_t n = 0;

    emit(program, &n, branch_word(0xb, 0, 0x65, 4));
    emit_or(program, &n, 0x11, 0x40);
    emit_or(program, &n, 1, 0x91);
    emit_or(program, &n, 0x91, 0x49);
    emit(program, &n, branch_word(0xb, 0, 0x65, 8));
    emit_or(program, &n, 0x22, 0x41);
    emit(program, &n, alu_word(1, 0xff, 0x41, 0x42, true));
    emit_or(program, &n, 0x92, 0x4a);
    emit(program, &n, branch_word(0xb, 0, 0x65, 12));
    emit_or(program, &n, 0x33, 0x43);
    emit(program, &n, alu_word(1, 0xff, 0x43, 0x44, true));
    emit_or(program, &n, 0x93, 0x4b);
    emit(program, &n, branch_word(0xb, 0, 0x65, 16));
    emit_or(program, &n, 0x44, 0x45);
    emit(program, &n, alu_word(1, 0xff, 0x45, 0x46, true));
    emit_or(program, &n, 0x94, 0x4c);
    emit_or(program, &n, 0x55, 0x47);
    emit(program, &n, alu_word(1, 0x0f, 0x47, 0x48, true));
    emit_or(program, &n, 0x95, 0x4d);
    start_program(qts, program, n);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0x11);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0x22);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x42), ==, 0x22);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x43), ==, 0x33);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x44), ==, 0x33);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x45), ==, 0x44);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x46), ==, 0x44);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x47), ==, 0x55);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x48), ==, 0x05);
    for (unsigned reg = 0x49; reg <= 0x4d; reg++) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + reg), ==, 0);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_circular_call_stack(void)
{
    static const uint16_t next[] = { 0x101, 0x102, 0x103, 0x104, 0x105 };
    static const uint16_t retained[] = { 0x105, 0x104, 0x103, 0x102 };
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x87, 4);
    load_seq_word(qts, 0, branch_word(0xb, 0, 0x65, next[0]));
    for (unsigned i = 1; i < ARRAY_SIZE(next); i++) {
        load_seq_word(qts, next[i - 1],
                      branch_word(0xb, 0, 0x65, next[i]));
    }
    load_seq_word(qts, 0x180, alu_word(0, 0x5a, 0x6a, 0x20, true));
    qtest_writeb(qts, REG_BASE + 0x60, 0x94);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x63, 0);
    for (unsigned i = 0; i < ARRAY_SIZE(next); i++) {
        step_instruction(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62) |
                        qtest_readb(qts, REG_BASE + 0x63) << 8, ==, next[i]);
    }
    /* Reading all four low/high pairs restores the physical stack pointer. */
    for (unsigned pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < ARRAY_SIZE(retained); i++) {
            uint16_t word = qtest_readb(qts, REG_BASE + 0x6f);

            word |= qtest_readb(qts, REG_BASE + 0x6f) << 8;
            g_assert_cmphex(word, ==, retained[i]);
        }
    }
    for (unsigned i = 0; i <= ARRAY_SIZE(retained); i++) {
        qtest_writeb(qts, REG_BASE + 0x62, 0x80);
        qtest_writeb(qts, REG_BASE + 0x63, 1);
        step_instruction(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62) |
                        qtest_readb(qts, REG_BASE + 0x63) << 8,
                        ==, retained[i % ARRAY_SIZE(retained)]);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    }
    qtest_quit(qts);
}

static void test_paused_call_migrates(void)
{
    uint8_t program[6 * 4];
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-call-XXXXXX", &error);
    g_autofree char *uri = NULL;
    g_autofree char *socket_path = NULL;
    QTestState *from, *to;
    size_t n = 0;

    g_assert_no_error(error);
    socket_path = g_strdup_printf("%s/migrate.sock", dir);
    uri = g_strdup_printf("unix:%s", socket_path);
    from = start_aic();
    to = qtest_init("-M pc -m 128M -nodefaults -S -incoming defer "
                    "-device aic7880,id=hba,bus=pci.0,addr=04.0");
    emit(program, &n, branch_word(0xb, 0, 0x65, 3));
    emit_or(program, &n, 0x11, 0x20);
    emit_or(program, &n, 1, 0x91);
    emit_or(program, &n, 1, 0x91);
    emit(program, &n, alu_word(1, 0xff, 0x6a, 0x21, true));
    emit_or(program, &n, 0x55, 0x22);
    start_program(from, program, n);
    wait_paused(from);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x62), ==, 4);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x91) & 1, ==, 1);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x62), ==, 4);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x20), ==, 0);
    qtest_writeb(to, REG_BASE + 0x92, 1);
    resume_program(to, 2);
    wait_paused(to);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x20), ==, 0x11);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x22), ==, 0);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x92), ==, 0);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket_path);
    g_rmdir(dir);
}

static void test_breakpoint_pause_and_interrupt(void)
{
    uint8_t program[8];
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    emit_or(program, &n, 0x5a, 0x20);
    emit_or(program, &n, 1, 0x91);
    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    for (size_t i = 0; i < n; i++) {
        qtest_writeb(qts, REG_BASE + 0x61, program[i]);
    }
    qtest_writeb(qts, REG_BASE + 0x60, 0x90);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x67, 0);
    qtest_writeb(qts, REG_BASE + 0x68, 0);
    resume_program(qts, 2);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_false(qtest_get_irq(qts, 11));

    qtest_writeb(qts, REG_BASE + 0x92, 8);
    qtest_writeb(qts, REG_BASE + 0x60, 0x98);
    resume_program(qts, 2);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0x11, 8);
    qtest_writeb(qts, REG_BASE + 0x00, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x0c, ==, 0x0c);
    qtest_writeb(qts, REG_BASE + 0x92, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x0c, ==, 4);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0x0c, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x0c, ==, 4);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0x92, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x0c, ==, 0);
    g_assert_false(qtest_get_irq(qts, 11));

    qtest_writeb(qts, REG_BASE + 0x68, 0x80);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    resume_program(qts, 2);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x68), ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x68, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x68), ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    qtest_quit(qts);
}

static void test_breakpoint_mask_after_error(void)
{
    uint8_t program[4];
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    qtest_writeb(qts, REG_BASE + 0x87, 6);
    qtest_writeb(qts, REG_BASE + 0x60, 0x98);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/hba','property':'x-test-pci-error',"
        "'value':'received-master-abort'}}");
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0x92, 8);
    g_assert_false(qtest_get_irq(qts, 11));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);

    emit_or(program, &n, 1, 0x91);
    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    for (size_t i = 0; i < n; i++) {
        qtest_writeb(qts, REG_BASE + 0x61, program[i]);
    }
    qtest_writeb(qts, REG_BASE + 0x60, 0x90);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x68, 0);
    resume_program(qts, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
    g_assert_false(qtest_get_irq(qts, 11));

    qtest_writeb(qts, REG_BASE + 0x11, 8);
    qtest_writeb(qts, REG_BASE + 0x00, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x0c, ==, 0x0c);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0x0c, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x0c, ==, 0x0c);
    qtest_writeb(qts, REG_BASE + 0x92, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x0c, ==, 8);
    g_assert_false(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0x60, 0x98);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0x92, 8);
    g_assert_false(qtest_get_irq(qts, 11));
    qtest_quit(qts);
}

static void test_breakpoint_address_bits(void)
{
    const uint16_t breakpoints[] = { 3, 0x101 };

    for (size_t point = 0; point < ARRAY_SIZE(breakpoints); point++) {
        uint8_t program[12];
        QTestState *qts = start_aic();
        uint16_t address = breakpoints[point];
        size_t n = 0;

        qtest_irq_intercept_in(qts, "ioapic");
        qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
        qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
        emit_or(program, &n, 0x11, 0x20);
        emit_or(program, &n, 0x22, 0x21);
        emit_or(program, &n, 1, 0x91);
        qtest_writeb(qts, REG_BASE + 0x60, 0x91);
        qtest_writeb(qts, REG_BASE + 0x62, (address - 1) & 0xff);
        qtest_writeb(qts, REG_BASE + 0x63, (address - 1) >> 8);
        for (size_t i = 0; i < n; i++) {
            qtest_writeb(qts, REG_BASE + 0x61, program[i]);
        }
        qtest_writeb(qts, REG_BASE + 0x60, 0x98);
        qtest_writeb(qts, REG_BASE + 0x62, (address - 1) & 0xff);
        qtest_writeb(qts, REG_BASE + 0x63, (address - 1) >> 8);
        qtest_writeb(qts, REG_BASE + 0x67, address & 0xff);
        qtest_writeb(qts, REG_BASE + 0x68, address >> 8);
        resume_program(qts, 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x11);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==,
                        address & 0xff);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x63), ==, address >> 8);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_true(qtest_get_irq(qts, 11));
        qtest_writeb(qts, REG_BASE + 0x92, 8);
        qtest_writeb(qts, REG_BASE + 0x68, 0x80 | (address >> 8));
        resume_program(qts, 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, 0x22);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
        qtest_writeb(qts, REG_BASE + 0x92, 1);
        g_assert_false(qtest_get_irq(qts, 11));
        qtest_quit(qts);
    }
}

static void check_maskable_break_migrates(bool pci_error)
{
    uint8_t program[4];
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-brk-XXXXXX", &error);
    g_autofree char *uri = NULL;
    g_autofree char *socket_path = NULL;
    QTestState *from, *to;
    size_t n = 0;

    g_assert_no_error(error);
    socket_path = g_strdup_printf("%s/migrate.sock", dir);
    uri = g_strdup_printf("unix:%s", socket_path);
    from = start_aic();
    to = qtest_init("-M pc -m 128M -nodefaults -S -incoming defer "
                    "-device aic7880,id=hba,bus=pci.0,addr=04.0");
    qtest_outl(from, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(from, PCI_CONFIG_DATA + 3, 11);
    if (pci_error) {
        emit(program, &n, branch_word(8, 0, 0x65, 0));
        start_program(from, program, n);
        qtest_qmp_assert_success(from,
            "{'execute':'qom-set','arguments':{"
            "'path':'/machine/peripheral/hba','property':'x-test-pci-error',"
            "'value':'received-master-abort'}}");
    } else {
        qtest_writeb(from, REG_BASE + 0x68, 0);
        emit_or(program, &n, 1, 0x91);
        start_program(from, program, n);
    }
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x91) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x92), ==,
                    pci_error ? 0x40 : 0);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    qtest_irq_intercept_in(to, "ioapic");
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x91) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x92), ==, pci_error ? 0x40 : 0);
    g_assert_false(qtest_get_irq(to, 11));
    qtest_writeb(to, REG_BASE + 0x60, 0x98);
    g_assert_true(qtest_get_irq(to, 11));
    qtest_writeb(to, REG_BASE + 0x92, 8);
    g_assert_false(qtest_get_irq(to, 11));
    if (pci_error) {
        g_assert_cmphex(qtest_readb(to, REG_BASE + 0x92), ==, 0x40);
        config_writew(to, 0x06, 0x2000);
        g_assert_cmphex(qtest_readb(to, REG_BASE + 0x92), ==, 0);
    }
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket_path);
    g_rmdir(dir);
}

static void test_breakpoint_mask_migrates(void)
{
    check_maskable_break_migrates(false);
}

static void test_pci_error_mask_migrates(void)
{
    check_maskable_break_migrates(true);
}

static QTestState *start_lun_targets(const char *extra)
{
    QTestState *qts = qtest_initf(
        "-M pc -m 128M -nodefaults -d guest_errors %s "
        "-drive if=none,id=l0,file=null-co://,format=raw "
        "-drive if=none,id=l3,file=null-co://,format=raw "
        "-device aic7880,id=hba,bus=pci.0,addr=04.0 "
        "-device scsi-hd,drive=l0,bus=hba.0,scsi-id=0,lun=0,"
        "product='SYNTHETIC LUN0' "
        "-device scsi-hd,drive=l3,bus=hba.0,scsi-id=0,lun=3,"
        "product='SYNTHETIC LUN3'", extra);

    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    return qts;
}

static void check_lun_inquiry(QTestState *qts, unsigned lun)
{
    uint8_t got[52];
    uint8_t guard[8];

    for (unsigned i = 0; i < 1000; i++) {
        if (qtest_readb(qts, REG_BASE + 0x91) & 2) {
            break;
        }
        qtest_clock_step(qts, 1000);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x20, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x22), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x23), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x1002c);
    qtest_memread(qts, 0x10000, got, sizeof(got));
    memset(guard, 0xa5, sizeof(guard));
    g_assert_cmpmem(got, 8, guard, 8);
    g_assert_cmpmem(got + 44, 8, guard, 8);
    g_assert_cmphex(got[8], ==, lun == 2 ? 0x7f : 0);
    if (lun != 2) {
        const char *product = lun == 3 ? "SYNTHETIC LUN3" : "SYNTHETIC LUN0";

        g_assert_cmpmem(got + 24, strlen(product), product, strlen(product));
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, 1);
}

static void test_identify_lun_routing(void)
{
    static const unsigned luns[] = { 3, 0, 2 };
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };

    for (unsigned i = 0; i < ARRAY_SIZE(luns); i++) {
        for (unsigned disconnect = 0; disconnect < 2; disconnect++) {
            QTestState *qts = start_lun_targets("");
            uint8_t program[128], guard[52];
            uint8_t identify = 0x80 | luns[i] | (disconnect ? 0x40 : 0);
            size_t n;

            memset(guard, 0xa5, sizeof(guard));
            qtest_memwrite(qts, 0x10000, guard, sizeof(guard));
            host_dma_program(qts, 0x10008, 36);
            qtest_writeb(qts, REG_BASE + 0x08, 36);
            n = build_command_program_mode(program, cdb, sizeof(cdb),
                                            false, true, identify);
            start_program(qts, program, n);
            check_lun_inquiry(qts, luns[i]);
            qtest_quit(qts);
        }
    }
}

static void test_narrow_connection_id_eight_times_out(void)
{
    QTestState *qts = start_disk();
    uint8_t program[16];
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x1f, 0);
    qtest_writeb(qts, REG_BASE + 0x02, 4);
    qtest_writeb(qts, REG_BASE + 0x41, 0xa5);
    emit_or(program, &n, 0x87, 0x05);
    emit_or(program, &n, 0x40, 0x00);
    emit_or(program, &n, 0x33, 0x41);
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    qtest_clock_step(qts, 256000000);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x50, ==, 0x10);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0x33);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_quit(qts);
}

static void test_lun_nexus_migrates_before_cdb(void)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-lun-XXXXXX", &error);
    g_autofree char *socket_path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;
    uint8_t program[128], guard[52];
    size_t n = 0;

    g_assert_no_error(error);
    socket_path = g_build_filename(dir, "migrate.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket_path);
    from = start_lun_targets("");
    to = start_lun_targets("-S -incoming defer");
    qtest_writeb(from, REG_BASE + 0x01, 8);
    attention_select(from, 0x48);
    attention_write(from, 6, 0xc3);
    attention_write(from, 0x0c, 0x40);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 3) & 0xe0, ==, 0x80);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    memset(guard, 0xa5, sizeof(guard));
    qtest_memwrite(to, 0x10000, guard, sizeof(guard));
    host_dma_program(to, 0x10008, 36);
    qtest_writeb(to, REG_BASE + 0x08, 36);
    qtest_writeb(to, REG_BASE + 0x92, 1);
    emit_or(program, &n, 0, 0x93);
    emit_cdb_completion(program, &n, cdb, sizeof(cdb), false, true);
    start_program(to, program, n);
    check_lun_inquiry(to, 3);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket_path);
    g_rmdir(dir);
}

static void test_inquiry_via_sequencer(void)
{
    uint8_t program[128] = { 0 };
    uint8_t answer[40];
    uint8_t fill[sizeof(answer)];
    size_t n;
    QTestState *qts = start_disk();
    const uint32_t guest_addr = 0x10000;
    const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };

    memset(fill, 0xa5, sizeof(fill));
    qtest_memwrite(qts, guest_addr, fill, sizeof(fill));
    for (unsigned byte = 0; byte < 4; byte++) {
        qtest_writeb(qts, REG_BASE + 0x88 + byte,
                     guest_addr >> (byte * 8));
    }
    qtest_writeb(qts, REG_BASE + 0x8c, 36);
    qtest_writeb(qts, REG_BASE + 0x08, 36);
    qtest_writeb(qts, REG_BASE + 0x14, 0x00);
    qtest_writeb(qts, REG_BASE + 0x15, 0x00);
    qtest_writeb(qts, REG_BASE + 0x16, 0x02);

    n = build_command_program(program, cdb, sizeof(cdb), false);
    start_program(qts, program, n);
    wait_register(qts, 0x91, 2, 2);

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 2);
    qtest_memread(qts, guest_addr, answer, sizeof(answer));
    g_assert_cmphex(answer[0], ==, 0); /* direct-access disk */
    g_assert_cmpint(memcmp(answer + 8, "QEMU", 4), ==, 0);
    for (unsigned i = 36; i < sizeof(answer); i++) {
        g_assert_cmphex(answer[i], ==, 0xa5);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x14), ==, 36);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x16), ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 4);
    qtest_quit(qts);
}

static void test_selected_inquiry_first_segment(void)
{
    uint8_t program[64] = { 0 };
    uint8_t guarded[33];
    QTestState *qts = start_selected_disk(NULL);

    memset(guarded, 0xa5, sizeof(guarded));
    qtest_memwrite(qts, 0x10000, guarded, sizeof(guarded));
    host_dma_program(qts, 0x10008, 17);
    qtest_writeb(qts, REG_BASE + 0x08, 17);
    qtest_writeb(qts, REG_BASE + 0x1f, 2);
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    start_program(qts, program, build_selected_inquiry_program(program));

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x0a, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe6, ==, 0x46);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x14), ==, 25);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x10008);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 17);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_memread(qts, 0x10000, guarded, sizeof(guarded));
    for (size_t i = 0; i < sizeof(guarded); i++) {
        g_assert_cmphex(guarded[i], ==, 0xa5);
    }
    qtest_quit(qts);
}

static void test_selected_inquiry_buffer_migration_refused(void)
{
    uint8_t program[64] = { 0 };
    uint8_t guarded[33];
    g_autofree char *dir =
        g_dir_make_tmp("aic7880-selected-migrate-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    g_autofree char *uri = g_strdup_printf("file:%s", path);
    QTestState *qts = start_selected_disk(NULL);

    memset(guarded, 0xa5, sizeof(guarded));
    qtest_memwrite(qts, 0x10000, guarded, sizeof(guarded));
    host_dma_program(qts, 0x10008, 17);
    qtest_writeb(qts, REG_BASE + 0x08, 17);
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    start_program(qts, program, build_selected_inquiry_program(program));
    g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 17);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 0);
    qtest_qmp_assert_success(qts,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration(qts, "failed");
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_memread(qts, 0x10000, guarded, sizeof(guarded));
    for (unsigned i = 0; i < sizeof(guarded); i++) {
        g_assert_cmphex(guarded[i], ==, 0xa5);
    }
    qtest_quit(qts);
    unlink(path);
    g_rmdir(dir);
}

static void test_selected_inquiry_second_segment_fault(void)
{
    static const uint8_t first_answer[17] = {
        0, 0, 5, 0x12, 31, 0, 0, 0x10,
        'L', 'A', 'B', 'T', 'E', 'S', 'T', ' ', 'S'
    };
    uint8_t program[64] = { 0 };
    uint8_t guarded[33];
    QTestState *qts = start_selected_disk(NULL);

    memset(guarded, 0xa5, sizeof(guarded));
    qtest_memwrite(qts, 0x10000, guarded, sizeof(guarded));
    host_dma_program(qts, 0x10008, 17);
    qtest_writeb(qts, REG_BASE + 0x08, 17);
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    start_program(qts, program, build_selected_inquiry_program(program));
    g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 2);
    qtest_writeb(qts, REG_BASE + 0x93, 0x38);
    qtest_memread(qts, 0x10000, guarded, sizeof(guarded));
    g_assert_cmpmem(guarded + 8, 17, first_answer, 17);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);

    qtest_writeb(qts, REG_BASE + 0x93, 0x30);
    workload_wait_hdma_disabled(qts);
    host_dma_program(qts, 0x70000000, 19);
    qtest_writeb(qts, REG_BASE + 0x08, 19);
    qtest_writeb(qts, REG_BASE + 0x93, 0x38);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 19);
    qtest_memread(qts, 0x10000, guarded, sizeof(guarded));
    g_assert_cmpmem(guarded + 8, 17, first_answer, 17);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(guarded[i], ==, 0xa5);
        g_assert_cmphex(guarded[25 + i], ==, 0xa5);
    }
    qtest_quit(qts);
}

static void test_selected_inquiry_two_segments(void)
{
    static const uint8_t inquiry[36] = {
        0, 0, 5, 0x12, 31, 0, 0, 0x10,
        'L', 'A', 'B', 'T', 'E', 'S', 'T', ' ',
        'S', 'E', 'G', 'M', 'E', 'N', 'T', 'E',
        'D', '-', 'D', 'I', 'S', 'K', ' ', ' ',
        '1', '.', '0', 0
    };
    uint8_t program[64] = { 0 };
    uint8_t first[33], second[35];
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-selected-inquiry-XXXXXX",
                                         &error);
    g_autofree char *trace_path = NULL;
    g_autofree char *trace = NULL;
    QTestState *qts;

    g_assert_no_error(error);
    trace_path = g_build_filename(dir, "target.trace", NULL);
    qts = start_selected_disk(trace_path);
    memset(first, 0xa5, sizeof(first));
    memset(second, 0x5a, sizeof(second));
    qtest_memwrite(qts, 0x10000, first, sizeof(first));
    qtest_memwrite(qts, 0x10200, second, sizeof(second));
    host_dma_program(qts, 0x10008, 17);
    qtest_writeb(qts, REG_BASE + 0x08, 17);
    qtest_writeb(qts, REG_BASE + 0x1f, 2);
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    start_program(qts, program, build_selected_inquiry_program(program));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 17);

    qtest_writeb(qts, REG_BASE + 0x93, 0x38);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x10008 + 17);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 9, ==, 9);
    qtest_memread(qts, 0x10000, first, sizeof(first));
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(first[i], ==, 0xa5);
        g_assert_cmphex(first[25 + i], ==, 0xa5);
    }
    g_assert_cmpmem(first + 8, 17, inquiry, 17);

    qtest_writeb(qts, REG_BASE + 0x93, 0x30);
    workload_wait_hdma_disabled(qts);
    host_dma_program(qts, 0x10208, 19);
    qtest_writeb(qts, REG_BASE + 0x08, 19);
    qtest_writeb(qts, REG_BASE + 0x93, 0x30);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x14), ==, 27);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x10208);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 19);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    qtest_memread(qts, 0x10200, second, sizeof(second));
    for (unsigned i = 0; i < sizeof(second); i++) {
        g_assert_cmphex(second[i], ==, 0x5a);
    }

    qtest_writeb(qts, REG_BASE + 0x93, 0x38);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 9, ==, 9);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x10208 + 19);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x14), ==, 27);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe6, ==, 0xc6);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    qtest_memread(qts, 0x10200, second, sizeof(second));
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(second[i], ==, 0x5a);
        g_assert_cmphex(second[27 + i], ==, 0x5a);
    }
    g_assert_cmpmem(second + 8, 19, inquiry + 17, 19);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(trace_path, &trace, NULL, &error));
    g_assert_no_error(error);
    g_assert_true(g_regex_match_simple(
        "scsi_disk_new_request[^\\n]*data=12 00 00 00 24 00",
        trace, 0, 0));
    unlink(trace_path);
    g_rmdir(dir);
}

static void test_short_inquiry_phase_mismatch_flush(void)
{
    static const uint8_t answer[19] = {
        0, 0, 5, 0x12, 31, 0, 0, 0x10,
        'S', 'H', 'O', 'R', 'T', ' ', ' ', ' ',
        'A', 'I', 'C'
    };
    uint8_t program[64] = { 0 };
    uint8_t first[33], second[35];
    QTestState *qts = qtest_init(
        "-M pc -m 128M -nodefaults "
        "-device aic7880,id=hba,bus=pci.0,addr=04.0 "
        "-device aic7880-short-inquiry-target,id=short0,"
        "bus=hba.0,scsi-id=0,lun=0");

    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    memset(first, 0xa5, sizeof(first));
    memset(second, 0x5a, sizeof(second));
    qtest_memwrite(qts, 0x10000, first, sizeof(first));
    qtest_memwrite(qts, 0x10200, second, sizeof(second));
    host_dma_program(qts, 0x10008, 17);
    qtest_writeb(qts, REG_BASE + 0x08, 17);
    qtest_writeb(qts, REG_BASE + 0x1f, 2);
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    start_program(qts, program, build_selected_inquiry_program(program));
    g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 17);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    qtest_memread(qts, 0x10000, first, sizeof(first));
    for (unsigned i = 0; i < sizeof(first); i++) {
        g_assert_cmphex(first[i], ==, 0xa5);
    }

    qtest_writeb(qts, REG_BASE + 0x93, 0x38);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 9, ==, 9);
    qtest_memread(qts, 0x10000, first, sizeof(first));
    g_assert_cmpmem(first + 8, 17, answer, 17);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(first[i], ==, 0xa5);
        g_assert_cmphex(first[25 + i], ==, 0xa5);
    }

    qtest_writeb(qts, REG_BASE + 0x93, 0x30);
    workload_wait_hdma_disabled(qts);
    host_dma_program(qts, 0x10208, 19);
    qtest_writeb(qts, REG_BASE + 0x08, 19);
    qtest_writeb(qts, REG_BASE + 0x93, 0x30);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 17);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 19);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    qtest_memread(qts, 0x10200, second, sizeof(second));
    for (unsigned i = 0; i < sizeof(second); i++) {
        g_assert_cmphex(second[i], ==, 0x5a);
    }

    qtest_writeb(qts, REG_BASE + 0x03, 0x40);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/short0',"
        "'property':'x-test-finish','value':'status'}}");
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x10, ==, 0x10);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe2, ==, 0xc2);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 17);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 19);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 9, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);

    qtest_writeb(qts, REG_BASE + 0x93, 0x38);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x1020a);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 17);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 17);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    qtest_memread(qts, 0x10200, second, sizeof(second));
    g_assert_cmpmem(second + 8, 2, answer + 17, 2);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(second[i], ==, 0x5a);
    }
    for (unsigned i = 10; i < sizeof(second); i++) {
        g_assert_cmphex(second[i], ==, 0x5a);
    }
    qtest_quit(qts);
}

static void test_short_inquiry_reset_reuses_target(void)
{
    uint8_t program[64] = { 0 };
    QTestState *qts = qtest_init(
        "-M pc -m 128M -nodefaults "
        "-device aic7880,id=hba,bus=pci.0,addr=04.0 "
        "-device aic7880-short-inquiry-target,id=short0,"
        "bus=hba.0,scsi-id=0,lun=0");

    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    host_dma_program(qts, 0x10008, 17);
    qtest_writeb(qts, REG_BASE + 0x08, 17);
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    start_program(qts, program, build_selected_inquiry_program(program));
    g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 2);

    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    host_dma_program(qts, 0x10208, 17);
    qtest_writeb(qts, REG_BASE + 0x08, 17);
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    start_program(qts, program, build_selected_inquiry_program(program));
    g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 2);
    qtest_quit(qts);
}

static void test_reset_and_interrupt_level(void)
{
    static const uint8_t program[] = { 0x01, 0x6a, 0x91, 0x00 };
    QTestState *qts = start_aic();
    int irq = -1;

    qtest_irq_intercept_in(qts, "ioapic");
    /*
     * i440FX slot 4 INTA uses PIRQD. Without firmware in qtest, route it
     * through PIIX3 to ISA/IOAPIC IRQ 11.
     */
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    qtest_writeb(qts, REG_BASE + 0x20, 0x5a);
    qtest_writeb(qts, REG_BASE + 0x88, 0x34);
    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x63, 0);
    for (size_t i = 0; i < sizeof(program); i++) {
        qtest_writeb(qts, REG_BASE + 0x61, program[i]);
    }
    qtest_writeb(qts, REG_BASE + 0x60, 0x90);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x63, 0);
    resume_program(qts, 2);
    for (int pin = 0; pin < 24; pin++) {
        if (qtest_get_irq(qts, pin)) {
            irq = pin;
            break;
        }
    }
    g_assert_cmpint(irq, >=, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    g_assert_false(qtest_get_irq(qts, irq));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
    qtest_writeb(qts, REG_BASE + 0x87, 0x06);
    g_assert_true(qtest_get_irq(qts, irq));
    config_writew(qts, 0x04, 0x0002);
    g_assert_false(qtest_get_irq(qts, irq));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
    config_writew(qts, 0x04, 0x0006);
    g_assert_true(qtest_get_irq(qts, irq));
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    g_assert_false(qtest_get_irq(qts, irq));

    qtest_writeb(qts, REG_BASE + 0x87, 1); /* chip reset */
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87), ==, 0x05);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x60), ==, 0x90);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x88), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    g_assert_cmphex(config_readl(qts, 0x04) & 0xffff, ==, 0x0006);
    g_assert_cmphex(config_readl(qts, 0x14) & ~0x0fu, ==, REG_BASE);

    qobject_unref(qtest_qmp(qts, "{'execute': 'system_reset'}"));
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_cmphex(config_readl(qts, 0x04) & 0xffff, ==, 0);
    g_assert_cmphex(config_readl(qts, 0x14) & ~0x0fu, ==, 0);
    qtest_quit(qts);
}

static void test_reset_discards_posted_completion(void)
{
    for (unsigned pci_reset = 0; pci_reset < 2; pci_reset++) {
        uint8_t program[8];
        QTestState *qts = start_aic();
        size_t n = 0;

        qtest_irq_intercept_in(qts, "ioapic");
        qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
        qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
        emit_or(program, &n, 5, 0x9d);
        emit_or(program, &n, 3, 0x91);
        start_program(qts, program, n);
        qtest_writeb(qts, REG_BASE + 0x87, 6);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 2);
        g_assert_true(qtest_get_irq(qts, 11));

        if (pci_reset) {
            qobject_unref(qtest_qmp(qts, "{'execute': 'system_reset'}"));
            qtest_qmp_eventwait(qts, "RESET");
            config_writel(qts, 0x14, REG_BASE);
            config_writew(qts, 0x04, 0x0006);
        } else {
            qtest_writeb(qts, REG_BASE + 0x87, 1);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
        g_assert_false(qtest_get_irq(qts, 11));
        qtest_quit(qts);
    }
}

static void test_missing_target(void)
{
    uint8_t program[12] = { 0 };
    size_t n = 0;
    QTestState *qts = start_aic();

    emit_or(program, &n, 0x07, 0x05);
    emit_or(program, &n, 0x40, 0x00);
    emit(program, &n, branch_word(8, 0, 0x65, 2));
    qtest_writeb(qts, REG_BASE + 0x02, 0x04);
    qtest_writeb(qts, REG_BASE + 0x11, 0x80);
    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    for (size_t i = 0; i < n; i++) {
        qtest_writeb(qts, REG_BASE + 0x61, program[i]);
    }
    qtest_writeb(qts, REG_BASE + 0x60, 0x90);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    resume_program(qts, 2);
    qtest_clock_step(qts, 256000000);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x04, ==, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x02, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 0x04);
    qtest_writeb(qts, REG_BASE + 0x0c, 0x80); /* clear selection timeout */
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x92, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x04, ==, 0);
    qtest_quit(qts);
}

static void test_dma_requires_bus_master(void)
{
    static const uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t program[128] = { 0 };
    uint8_t before[36], after[36];
    QTestState *qts = start_disk();
    size_t n = 0;

    config_writew(qts, 0x04, 0x0002); /* MMIO on, bus mastering off. */
    memset(before, 0xa5, sizeof(before));
    qtest_memwrite(qts, 0x10000, before, sizeof(before));
    qtest_writeb(qts, REG_BASE + 0x88, 0x00);
    qtest_writeb(qts, REG_BASE + 0x89, 0x00);
    qtest_writeb(qts, REG_BASE + 0x8a, 0x01);
    qtest_writeb(qts, REG_BASE + 0x8c, 36);
    qtest_writeb(qts, REG_BASE + 0x08, 36);

    n = build_command_program(program, cdb, sizeof(cdb), false);
    start_program(qts, program, n);

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x02, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_memread(qts, 0x10000, after, sizeof(after));
    g_assert_cmpint(memcmp(before, after, sizeof(before)), ==, 0);
    qtest_quit(qts);
}

static void test_bad_sequencer_opcode(void)
{
    /* Opcode 7 is outside the reviewed old-format instruction set. */
    static const uint8_t program[] = { 0, 0, 0, 0x0e };
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    for (size_t i = 0; i < sizeof(program); i++) {
        qtest_writeb(qts, REG_BASE + 0x61, program[i]);
    }
    qtest_writeb(qts, REG_BASE + 0x60, 0x90);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    resume_program(qts, 2);
    wait_paused(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x04, ==, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0x08);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 0x04);
    qtest_writeb(qts, REG_BASE + 0x92, 0x08);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    qtest_quit(qts);
}

static QTestState *start_file_target(const char *path, const char *trace_path,
                                    const char *device, const char *extra)
{
    g_autofree char *trace = trace_path ?
        g_strdup_printf("-trace enable=scsi_disk_new_request "
                        "-trace enable=aic7880_dma,file=%s",
                        trace_path) : g_strdup("");
    QTestState *qts = qtest_initf(
        "-M pc -m 128M -nodefaults %s "
        "-drive if=none,id=disk0,file=%s,format=raw "
        "-device aic7880,id=hba,bus=pci.0,addr=04.0 "
        "-device %s,drive=disk0,bus=hba.0,scsi-id=0,lun=0 %s",
        trace, path, device, extra);

    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    return qts;
}

static QTestState *start_file_disk(const char *path, const char *trace_path)
{
    return start_file_target(path, trace_path, "scsi-hd", "");
}

static QTestState *start_throttled_disk(const char *path)
{
    QTestState *qts = qtest_initf(
        "-M pc -m 128M -nodefaults "
        "-object throttle-group,id=tg,x-bps-read=1024 "
        "-blockdev driver=file,node-name=f0,filename=%s "
        "-blockdev driver=throttle,node-name=t0,throttle-group=tg,file=f0 "
        "-device aic7880,id=hba,bus=pci.0,addr=04.0 "
        "-device scsi-hd,drive=t0,bus=hba.0,scsi-id=0,lun=0", path);

    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    return qts;
}

static uint8_t run_disk_cdb(QTestState *qts, const uint8_t *cdb, size_t cdb_len,
                         uint32_t guest_addr, uint32_t count, bool write)
{
    uint8_t program[128] = { 0 };
    size_t n;
    uint8_t status;

    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    for (unsigned byte = 0; byte < 4; byte++) {
        qtest_writeb(qts, REG_BASE + 0x88 + byte,
                     guest_addr >> (byte * 8));
    }
    for (unsigned byte = 0; byte < 3; byte++) {
        qtest_writeb(qts, REG_BASE + 0x8c + byte,
                     count >> (byte * 8));
        qtest_writeb(qts, REG_BASE + 0x08 + byte,
                     count >> (byte * 8));
    }

    n = build_command_program(program, cdb, cdb_len, write);
    start_program(qts, program, n);

    for (unsigned i = 0; i < 1000; i++) {
        if (qtest_readb(qts, REG_BASE + 0x91) & 2) {
            break;
        }
        qtest_clock_step(qts, 1000);
        g_usleep(1000); /* The block throttle uses host time. */
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 2);
    status = qtest_readb(qts, REG_BASE + 0x22);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x23), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, 1);
    if (status == 0) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x8c), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 0);
    }
    qtest_writeb(qts, REG_BASE + 0x92, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    return status;
}

static void prepare_coupled_target(QTestState *qts, unsigned block_size)
{
    static const uint8_t read_cdb[10] = {
        0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0
    };
    static const uint8_t sense_cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    static const uint8_t capacity_cdb[10] = { 0x25 };
    uint8_t capacity[8];

    g_assert_cmphex(run_disk_cdb(qts, read_cdb, sizeof(read_cdb),
                                  0x10000, block_size, false), ==, 2);
    g_assert_cmphex(run_disk_cdb(qts, sense_cdb, sizeof(sense_cdb),
                                  0x11000, 18, false), ==, 0);
    g_assert_cmphex(run_disk_cdb(qts, capacity_cdb, sizeof(capacity_cdb),
                                  0x12000, sizeof(capacity), false), ==, 0);
    qtest_memread(qts, 0x12000, capacity, sizeof(capacity));
    g_assert_cmpuint(ldl_be_p(capacity + 4), ==, block_size);
}

static void coupled_wait(QTestState *qts, uint8_t reg, uint8_t mask)
{
    for (unsigned i = 0; i < 1000; i++) {
        if ((qtest_readb(qts, REG_BASE + reg) & mask) ||
            qtest_readb(qts, REG_BASE + 0x92)) {
            break;
        }
        qtest_clock_step(qts, 1000);
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + reg) & mask, ==, mask);
}

static void check_coupled_read_host_stall(bool cd, bool reset_fifo)
{
    static const uint8_t read_cdb[10] = {
        0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0
    };
    unsigned count = cd ? 2048 : 512;
    uint8_t program[128] = { 0 }, pattern[2048], guarded[2064], got[2064];
    g_autofree char *dir = g_dir_make_tmp("aic7880-coupled-migrate-XXXXXX",
                                        NULL);
    g_autofree char *state = g_build_filename(dir, "state", NULL);
    g_autofree char *uri = g_strdup_printf("file:%s", state);
    g_autofree char *path = NULL;
    QTestState *qts;
    int fd = g_file_open_tmp("aic7880-coupled-read-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 4096), ==, 0);
    for (unsigned i = 0; i < count; i++) {
        pattern[i] = (i * 37 + 11) ^ (i >> 8);
    }
    g_assert_cmpint(pwrite(fd, pattern, count, 0), ==,
                    count);
    close(fd);
    qts = start_file_target(path, NULL, cd ? "scsi-cd" : "scsi-hd", "");
    prepare_coupled_target(qts, count);
    memset(guarded, 0xa5, sizeof(guarded));
    qtest_memwrite(qts, 0x20000, guarded, count + 16);
    qtest_writeb(qts, REG_BASE + 0x87, 4);
    qtest_writeb(qts, REG_BASE + 0x93, 0);
    host_dma_program(qts, 0x20008, count);
    qtest_writeb(qts, REG_BASE + 0x08, 0);
    qtest_writeb(qts, REG_BASE + 0x09, count >> 8);
    qtest_writeb(qts, REG_BASE + 0x0a, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    start_program(qts, program,
        build_command_program_mode(program, read_cdb, sizeof(read_cdb),
                                   false, false, -1));
    coupled_wait(qts, 0x94, 2);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x20008);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, count);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    qtest_memread(qts, 0x20000, got, count + 16);
    g_assert_cmpmem(got, count + 16, guarded, count + 16);
    if (reset_fifo) {
        qtest_writeb(qts, REG_BASE + 0x93, 0);
        qtest_writeb(qts, REG_BASE + 0x93, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x2b, ==, 0x21);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        qtest_memread(qts, 0x20000, got, count + 16);
        g_assert_cmpmem(got, count + 16, guarded, count + 16);
    } else {
        qtest_qmp_assert_success(qts,
            "{'execute':'migrate','arguments':{'uri':%s}}", uri);
        sgi_wait_migration(qts, "failed");
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, count);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 2, ==, 2);
        qtest_writeb(qts, REG_BASE + 0x93, 0x38);
        coupled_wait(qts, 0x91, 2);
        qtest_writeb(qts, REG_BASE + 0x93, 0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x20008 + count);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x20008 + count);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 9, ==, 9);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
        qtest_memread(qts, 0x20000, got, count + 16);
        g_assert_cmpmem(got, 8, guarded, 8);
        g_assert_cmpmem(got + 8, count, pattern, count);
        g_assert_cmpmem(got + 8 + count, 8, guarded + 8 + count, 8);
    }
    qtest_quit(qts);
    unlink(path);
    unlink(state);
    g_rmdir(dir);
}

static void test_coupled_read_host_stall(void)
{
    check_coupled_read_host_stall(false, false);
}

static void test_coupled_cd_host_stall(void)
{
    check_coupled_read_host_stall(true, false);
}

static void test_coupled_fifo_reset_pending(void)
{
    check_coupled_read_host_stall(false, true);
}

static void check_coupled_admission_trace(const char *path, bool write,
                                           unsigned expected)
{
    g_autofree char *trace = NULL;
    g_auto(GStrv) lines = NULL;
    unsigned credited = 0;

    g_assert_true(g_file_get_contents(path, &trace, NULL, NULL));
    lines = g_strsplit(trace, "\n", -1);
    for (unsigned i = 0; lines[i]; i++) {
        const char *event = strstr(lines[i], "aic7880_dma ");
        unsigned devfn, address, count, direction;
        int result;

        if (!event || sscanf(event,
            "aic7880_dma devfn 0x%x addr 0x%x len %u to_memory %u result %d",
            &devfn, &address, &count, &direction, &result) != 5 ||
            address < 0x20008 || address >= 0x20208) {
            continue;
        }
        g_assert_cmphex(devfn, ==, PCI_DEVFN);
        g_assert_cmpuint(direction, ==, !write);
        g_assert_cmpint(result, ==, MEMTX_OK);
        g_assert_cmphex(address, ==, 0x20008 + credited);
        g_assert_cmpuint(count, >, 0);
        credited += count;
        g_assert_cmpuint(credited, <=, expected);
    }
    g_assert_cmpuint(credited, ==, expected);
}

static void test_pci_admission_keeps_abort_pending(void)
{
    uint8_t pattern[257];
    g_autofree char *path = NULL;
    g_autofree char *trace_path = NULL;
    QTestState *qts;
    int fd = g_file_open_tmp("aic7880-abort-admission-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 4096), ==, 0);
    close(fd);
    trace_path = g_strdup_printf("%s.trace", path);
    qts = start_file_disk(path, trace_path);
    for (unsigned i = 0; i < sizeof(pattern); i++) {
        pattern[i] = i * 19 + 3;
    }
    qtest_memwrite(qts, 0x20008, pattern, sizeof(pattern));
    host_dma_program(qts, 0x20008, sizeof(pattern));
    qtest_writeb(qts, REG_BASE + 0x93, 0x0d);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 1);
    qtest_writeb(qts, REG_BASE + 0x93, 4);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==, pattern[i]);
    }
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/hba','property':'x-test-pci-error',"
        "'value':'received-master-abort'}}");
    qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
    config_writew(qts, 4, 2);
    config_writew(qts, 4, 4);
    config_writew(qts, 4, 2);
    config_writew(qts, 4, 6);
    g_assert_cmphex(config_readl(qts, 4) >> 16 & 0x2000, ==, 0x2000);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0x40);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x20108);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    check_coupled_admission_trace(trace_path, true, 256);
    qtest_writeb(qts, REG_BASE + 0x93, 4);
    for (unsigned i = 8; i < 256; i++) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==, pattern[i]);
    }
    qtest_quit(qts);
    unlink(trace_path);
    unlink(path);
}

static void test_coupled_pci_admission(void)
{
    for (unsigned mode = 0; mode < 4; mode++) {
        bool write = mode & 1;
        bool io = mode & 2;
        uint8_t cdb[10] = { write ? 0x2a : 0x28, 0, 0, 0, 0,
                            write ? 1 : 0, 0, 0, 1, 0 };
        uint8_t program[128] = { 0 }, pattern[512], guarded[528], got[528];
        g_autofree char *path = NULL;
        g_autofree char *trace_path = NULL;
        QTestState *qts;
        uint32_t status;
        int fd = g_file_open_tmp("aic7880-admission-XXXXXX", &path, NULL);

        g_test_message("DATA %s, resume through %s decode",
                       write ? "OUT" : "IN", io ? "I/O" : "memory");
        g_assert_cmpint(fd, >=, 0);
        g_assert_cmpint(ftruncate(fd, 4096), ==, 0);
        for (unsigned i = 0; i < sizeof(pattern); i++) {
            pattern[i] = (i * 37 + 11) ^ (i >> 8);
        }
        g_assert_cmpint(pwrite(fd, pattern, sizeof(pattern), 0), ==,
                        sizeof(pattern));
        close(fd);
        trace_path = g_strdup_printf("%s.trace", path);
        qts = start_file_disk(path, trace_path);
        config_writel(qts, 0x10, 0xc001);
        prepare_coupled_target(qts, 512);
        status = config_readl(qts, 4) & 0xffff0000;
        memset(guarded, 0xa5, sizeof(guarded));
        if (write) {
            memcpy(guarded + 8, pattern, sizeof(pattern));
        }
        qtest_memwrite(qts, 0x20000, guarded, sizeof(guarded));
        qtest_writeb(qts, REG_BASE + 0x87, 4);
        host_dma_program(qts, 0x20008, 512);
        qtest_writeb(qts, REG_BASE + 0x08, 0);
        qtest_writeb(qts, REG_BASE + 0x09, 2);
        qtest_writeb(qts, REG_BASE + 0x0a, 0);
        qtest_writeb(qts, REG_BASE + 0x93, write ? 5 : 1);
        start_program(qts, program,
            build_command_program_mode(program, cdb, sizeof(cdb),
                                       write, false, -1));
        if (!write) {
            coupled_wait(qts, 0x94, 2);
        }
        qtest_writeb(qts, REG_BASE + 0x93, write ? 4 : 0);
        qtest_writeb(qts, REG_BASE + 0x93, write ? 0x0c : 8);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x20108);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 256);
        check_coupled_admission_trace(trace_path, write, 256);
        config_writew(qts, 4, 2);
        qtest_writeb(qts, REG_BASE + 0x93, write ? 0x3c : 0x38);
        if (!write) {
            coupled_wait(qts, 0x94, 2);
        }
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x20108);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 256);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x0b, ==,
                        write ? 1 : 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x0a, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        g_assert_cmphex(config_readl(qts, 4) & 0xffff0000, ==, status);
        check_coupled_admission_trace(trace_path, write, 256);
        config_writew(qts, 4, 4);
        qtest_clock_step(qts, 100000);
        check_coupled_admission_trace(trace_path, write, 256);
        config_writew(qts, 4, 2);
        g_assert_cmphex(config_readl(qts, 4) & 0xffff0000, ==, status);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x0a, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x20108);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 256);
        config_writew(qts, 4, io ? 5 : 6);
        for (unsigned i = 0; i < 1000; i++) {
            uint8_t intstat = io ? qtest_inb(qts, 0xc091) :
                                  qtest_readb(qts, REG_BASE + 0x91);
            if (intstat & 2) {
                break;
            }
            qtest_clock_step(qts, 1000);
            g_usleep(1000);
        }
        config_writew(qts, 4, 7);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x0a, ==, 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x20208);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 9, ==, 9);
        check_coupled_admission_trace(trace_path, write, 512);
        qtest_memread(qts, 0x20000, got, sizeof(got));
        g_assert_cmpmem(got, 8, guarded, 8);
        g_assert_cmpmem(got + 8, 512, pattern, 512);
        g_assert_cmpmem(got + 520, 8, guarded + 520, 8);
        qtest_quit(qts);
        if (write) {
            fd = open(path, O_RDONLY);
            g_assert_cmpint(fd, >=, 0);
            g_assert_cmpint(pread(fd, got, 512, 512), ==, 512);
            g_assert_cmpmem(got, 512, pattern, 512);
            close(fd);
        }
        unlink(trace_path);
        unlink(path);
    }
}

static void test_coupled_write_prefetch(void)
{
    static const uint8_t write_cdb[10] = {
        0x2a, 0, 0, 0, 0, 1, 0, 0, 1, 0
    };
    static const unsigned lengths[] = { 17, 19, 476 };
    static const uint32_t addresses[] = { 0x21004, 0x22003, 0x23005 };
    uint8_t program[128] = { 0 }, pattern[512], guarded[492], got[512];
    g_autofree char *path = NULL;
    unsigned sent = 0;
    QTestState *qts;
    int fd = g_file_open_tmp("aic7880-coupled-write-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 4096), ==, 0);
    close(fd);
    for (unsigned i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (i * 53 + 7) ^ (i >> 8);
    }
    qts = start_file_disk(path, NULL);
    prepare_coupled_target(qts, 512);
    qtest_writeb(qts, REG_BASE + 0x87, 4);
    for (unsigned segment = 0; segment < G_N_ELEMENTS(lengths); segment++) {
        unsigned count = lengths[segment];
        uint32_t address = addresses[segment];

        memset(guarded, 0xa5, sizeof(guarded));
        memcpy(guarded + 8, pattern + sent, count);
        qtest_memwrite(qts, address - 8, guarded, count + 16);
        qtest_writeb(qts, REG_BASE + 0x93, 4);
        host_dma_program(qts, address, count);
        for (unsigned i = 0; i < 3; i++) {
            qtest_writeb(qts, REG_BASE + 0x08 + i, count >> (8 * i));
        }
        qtest_writeb(qts, REG_BASE + 0x93, 5);
        if (!segment) {
            start_program(qts, program,
                build_command_program_mode(program, write_cdb,
                                           sizeof(write_cdb), true, false, -1));
            g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, count);
            qtest_writeb(qts, REG_BASE + 0x93, 4);
            qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
            g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==,
                            address + count);
            g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
            g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, address);
            g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, count);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 8);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
        }
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        coupled_wait(qts, segment == 2 ? 0x91 : 0x0b,
                          segment == 2 ? 2 : 4);
        qtest_writeb(qts, REG_BASE + 0x93, 4);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, address + count);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0);
        qtest_memread(qts, address - 8, got, count + 16);
        g_assert_cmpmem(got, count + 16, guarded, count + 16);
        sent += count;
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    qtest_quit(qts);
    fd = open(path, O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, got, sizeof(got), 512), ==, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), pattern, sizeof(pattern));
    close(fd);
    unlink(path);
}

static void test_coupled_read_failed_prefix(void)
{
    static const uint8_t read_cdb[10] = {
        0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0
    };
    uint8_t program[128] = { 0 }, pattern[512], got[256], guard[8];
    g_autofree char *path = NULL;
    g_autofree char *trace_path = NULL;
    g_autofree char *trace = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-coupled-fault-XXXXXX",
                                        NULL);
    g_autofree char *state = g_build_filename(dir, "state", NULL);
    g_autofree char *uri = g_strdup_printf("file:%s", state);
    g_auto(GStrv) lines = NULL;
    unsigned credited = 0;
    bool failed = false;
    QTestState *qts;
    int fd = g_file_open_tmp("aic7880-coupled-fault-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 4096), ==, 0);
    for (unsigned i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (i * 71 + 13) ^ (i >> 8);
    }
    g_assert_cmpint(pwrite(fd, pattern, sizeof(pattern), 0), ==,
                    sizeof(pattern));
    close(fd);
    trace_path = g_strdup_printf("%s.trace", path);
    qts = start_file_target(path, trace_path, "scsi-hd",
        "-device memory-buffer-testdev,id=first,size=256 "
        "-device memory-buffer-testdev,id=tail,size=20 "
        "-device memory-buffer-testdev,id=guard,size=8");
    prepare_coupled_target(qts, 512);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/first", 2, 0x50000000);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/tail", 2, 0x50000100);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/guard", 2, 0x50000200);
    memset(guard, 0xa5, sizeof(guard));
    qtest_memwrite(qts, 0x50000200, guard, sizeof(guard));
    qtest_writeb(qts, REG_BASE + 0x87, 4);
    host_dma_program(qts, 0x50000000, 512);
    qtest_writeb(qts, REG_BASE + 0x08, 0);
    qtest_writeb(qts, REG_BASE + 0x09, 2);
    qtest_writeb(qts, REG_BASE + 0x0a, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    start_program(qts, program,
        build_command_program(program, read_cdb, sizeof(read_cdb), false));
    for (unsigned i = 0; i < 1000; i++) {
        if (qtest_readb(qts, REG_BASE + 0x92)) {
            break;
        }
        qtest_clock_step(qts, 1000);
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_true(g_file_get_contents(trace_path, &trace, NULL, NULL));
    lines = g_strsplit(trace, "\n", -1);
    for (unsigned i = 0; lines[i]; i++) {
        const char *event = strstr(lines[i], "aic7880_dma ");
        unsigned devfn, address, count, direction;
        int result;

        if (!event || sscanf(event,
            "aic7880_dma devfn 0x%x addr 0x%x len %u to_memory %u result %d",
            &devfn, &address, &count, &direction, &result) != 5 ||
            address < 0x50000000 || address >= 0x50000200) {
            continue;
        }
        g_assert_false(failed);
        g_assert_cmphex(devfn, ==, PCI_DEVFN);
        g_assert_cmpuint(direction, ==, 1);
        g_assert_cmphex(address, ==, 0x50000000 + credited);
        g_assert_cmpuint(count, >, 0);
        if (result == MEMTX_OK) {
            credited += count;
        } else {
            failed = true;
        }
    }
    g_assert_true(failed);
    g_assert_cmpuint(credited, >=, 256);
    g_assert_cmpuint(credited, <=, 276);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==,
                    0x50000000 + credited);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 512 - credited);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 9, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_memread(qts, 0x50000000, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), pattern, sizeof(got));
    qtest_memread(qts, 0x50000100, got, 20);
    g_assert_cmpmem(got, 20, pattern + 256, 20);
    qtest_memread(qts, 0x50000200, got, sizeof(guard));
    g_assert_cmpmem(got, sizeof(guard), guard, sizeof(guard));
    config_writew(qts, 4, 2);
    config_writew(qts, 4, 4);
    config_writew(qts, 4, 2);
    config_writew(qts, 4, 6);
    {
        g_autofree char *after = NULL;

        g_assert_true(g_file_get_contents(trace_path, &after, NULL, NULL));
        g_assert_cmpstr(after, ==, trace);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==,
                    0x50000000 + credited);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 512 - credited);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 0);
    qtest_qmp_assert_success(qts,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration(qts, "failed");
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 512 - credited);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 9, ==, 0);
    qtest_quit(qts);
    unlink(path);
    unlink(trace_path);
    unlink(state);
    g_rmdir(dir);
}

static void test_coupled_write_failed_read(void)
{
    static const uint8_t write_cdb[10] = {
        0x2a, 0, 0, 0, 0, 1, 0, 0, 1, 0
    };
    uint8_t program[128] = { 0 }, pattern[256], got[512];
    g_autofree char *path = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-coupled-out-fault-XXXXXX",
                                        NULL);
    g_autofree char *state = g_build_filename(dir, "state", NULL);
    g_autofree char *uri = g_strdup_printf("file:%s", state);
    QTestState *qts;
    int fd = g_file_open_tmp("aic7880-coupled-out-fault-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 4096), ==, 0);
    close(fd);
    for (unsigned i = 0; i < sizeof(pattern); i++) {
        pattern[i] = i * 83 + 17;
    }
    qts = start_file_target(path, NULL, "scsi-hd",
        "-device memory-buffer-testdev,id=source,size=256");
    prepare_coupled_target(qts, 512);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/source", 2, 0x50000000);
    qtest_memwrite(qts, 0x50000000, pattern, sizeof(pattern));
    qtest_writeb(qts, REG_BASE + 0x87, 4);
    host_dma_program(qts, 0x50000000, 512);
    qtest_writeb(qts, REG_BASE + 0x08, 0);
    qtest_writeb(qts, REG_BASE + 0x09, 2);
    qtest_writeb(qts, REG_BASE + 0x0a, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 5);
    start_program(qts, program,
        build_command_program(program, write_cdb, sizeof(write_cdb), true));
    for (unsigned i = 0; i < 1000; i++) {
        if (qtest_readb(qts, REG_BASE + 0x92)) {
            break;
        }
        qtest_clock_step(qts, 1000);
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 4);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x50000100);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 256);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x50000100);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 256);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 9, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_memread(qts, 0x50000000, got, sizeof(pattern));
    g_assert_cmpmem(got, sizeof(pattern), pattern, sizeof(pattern));
    qtest_qmp_assert_success(qts,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration(qts, "failed");
    qtest_quit(qts);
    fd = open(path, O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, got, sizeof(got), 512), ==, sizeof(got));
    for (unsigned i = 0; i < sizeof(got); i++) {
        g_assert_cmphex(got[i], ==, 0);
    }
    close(fd);
    unlink(path);
    unlink(state);
    g_rmdir(dir);
}

static void test_coupled_zero_data_keeps_host_count(void)
{
    static const uint8_t ready[6] = { 0 };
    uint8_t program[128] = { 0 }, guarded[44], got[44];
    g_autofree char *path = NULL;
    g_autofree char *trace_path = NULL;
    g_autofree char *trace = NULL;
    QTestState *qts;
    int fd = g_file_open_tmp("aic7880-zero-data-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 4096), ==, 0);
    close(fd);
    trace_path = g_strdup_printf("%s.trace", path);
    qts = start_file_target(path, trace_path, "scsi-hd", "");
    prepare_coupled_target(qts, 512);
    for (unsigned from_memory = 0; from_memory < 2; from_memory++) {
        uint32_t address = 0x26004 + 64 * from_memory;

        for (unsigned i = 0; i < sizeof(guarded); i++) {
            guarded[i] = i * 23 + 7;
        }
        qtest_memwrite(qts, address - 8, guarded, sizeof(guarded));
        qtest_writeb(qts, REG_BASE + 0x87, 4);
        qtest_writeb(qts, REG_BASE + 0x93, 0);
        host_dma_program(qts, address, 0);
        for (unsigned i = 0; i < 3; i++) {
            qtest_writeb(qts, REG_BASE + 0x08 + i, i ? 0 : 28);
        }
        qtest_writeb(qts, REG_BASE + 0x93, from_memory ? 5 : 1);
        start_program(qts, program,
            build_command_program(program, ready, sizeof(ready), from_memory));
        coupled_wait(qts, 0x91, 2);
        qtest_writeb(qts, REG_BASE + 0x93, from_memory ? 4 : 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x22), ==, 0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, address);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, address);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 28);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 9, ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, 1);
        qtest_writeb(qts, REG_BASE + 0x92, 2);
        qtest_memread(qts, address - 8, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), guarded, sizeof(guarded));
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(trace_path, &trace, NULL, NULL));
    g_assert_null(strstr(trace, "addr 0x00026004"));
    g_assert_null(strstr(trace, "addr 0x00026044"));
    unlink(path);
    unlink(trace_path);
}

static void test_scsi_reset_out(void)
{
    static const uint8_t read_cdb[10] = {
        0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0
    };
    static const uint8_t sense_cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    QTestState *qts = start_disk();

    g_assert_cmphex(run_disk_cdb(qts, read_cdb, sizeof(read_cdb),
                                  0x10000, 512, false), ==, 2);
    g_assert_cmphex(run_disk_cdb(qts, sense_cdb, sizeof(sense_cdb),
                                  0x11000, 18, false), ==, 0);
    g_assert_cmphex(run_disk_cdb(qts, read_cdb, sizeof(read_cdb),
                                  0x10000, 512, false), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x00, 0x01);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00) & 1, ==, 1);
    qtest_writeb(qts, REG_BASE + 0x00, 0);
    g_assert_cmphex(run_disk_cdb(qts, read_cdb, sizeof(read_cdb),
                                  0x10000, 512, false), ==, 2);
    qtest_quit(qts);
}

static void test_checkpoint_expires_at_backend_buffer(void)
{
    static const uint8_t ready[6] = { 0 };
    static const uint8_t sense[6] = { 3, 0, 0, 0, 18, 0 };
    static const uint8_t read[10] = { 0x28, 0, 0, 0, 0, 0, 0, 2, 0, 0 };
    QTestState *qts = start_peer_disk();
    uint8_t program[128];
    size_t n = 0;
    unsigned remaining = 128 * 1024 - 17 + 23;

    g_assert_cmphex(run_disk_cdb(qts, ready, sizeof(ready), 0, 0, false),
                    ==, 2);
    g_assert_cmphex(run_disk_cdb(qts, sense, sizeof(sense), 0x11000, 18, false),
                    ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 4);
    host_dma_program(qts, 0x10000, 17);
    qtest_writeb(qts, REG_BASE + 0x08, 17);
    emit_or(program, &n, 0, 0x00);
    emit_or(program, &n, 7, 0x05);
    emit_or(program, &n, 0, 0x93);
    emit_or(program, &n, 0xa0, 0x03);
    emit_or(program, &n, 0x58, 0x00);
    emit_or(program, &n, 0xc0, 0x06);
    emit_or(program, &n, 0x40, 0x0c);
    emit_or(program, &n, 0x80, 0x03);
    for (unsigned i = 0; i < sizeof(read); i++) {
        emit_or(program, &n, read[i], 0x06);
    }
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    qtest_writeb(qts, REG_BASE + 0x93, 0x38);
    for (unsigned i = 0; i < 1000 &&
         host_dma_read_bytes(qts, 0x08, 3); i++) {
        qtest_clock_step(qts, 1000);
        g_usleep(1000);
    }
    g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x10011);
    qtest_writeb(qts, REG_BASE + 0x93, 0);
    peer_event(qts, "save:0:0");
    qtest_writeb(qts, REG_BASE + 0x03, 0xe0);
    pio_read_to_scratch(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0x40);
    host_dma_program(qts, 0x10011, remaining);
    for (unsigned i = 0; i < 3; i++) {
        qtest_writeb(qts, REG_BASE + 0x08 + i, remaining >> (8 * i));
    }
    qtest_writeb(qts, REG_BASE + 0x93, 0x38);
    for (unsigned i = 0; i < 1000 &&
         host_dma_read_bytes(qts, 0x08, 3); i++) {
        qtest_clock_step(qts, 1000);
        g_usleep(1000);
    }
    g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x30017);
    qtest_writeb(qts, REG_BASE + 0x93, 0);
    peer_event_rejected(qts, "restore:0:0");
    g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x30017);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_quit(qts);
}

static void test_read_write_via_sequencer(void)
{
    static const uint8_t read_cdb[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0 };
    static const uint8_t write_cdb[10] = { 0x2a, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
    uint8_t disk_seed[512], write_data[512], got[512], guard[516];
    g_autofree char *path = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("aic7880-disk-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 4096), ==, 0);
    for (unsigned i = 0; i < sizeof(disk_seed); i++) {
        disk_seed[i] = i * 37 + 11;
        write_data[i] = i * 91 + 5;
    }
    g_assert_cmpint(pwrite(fd, disk_seed, sizeof(disk_seed), 0), ==,
                    sizeof(disk_seed));
    close(fd);

    qts = start_file_disk(path, NULL);
    memset(guard, 0xa5, sizeof(guard));
    qtest_memwrite(qts, 0x10100, guard, sizeof(guard));
    /*
     * QEMU's disk first reports POWER ON unit attention. Recover through
     * sequencer-issued REQUEST SENSE, then retry the same READ.
     */
    g_assert_cmphex(run_disk_cdb(qts, read_cdb, sizeof(read_cdb),
                                  0x10103, 512, false), ==, 2);
    {
        static const uint8_t sense_cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
        static const uint8_t expected_sense[18] = {
            0x70, 0, 0x06, 0, 0, 0, 0, 0x0a, 0, 0, 0, 0, 0x29,
            0, 0, 0, 0, 0
        };
        uint8_t sense[18];

        g_assert_cmphex(run_disk_cdb(qts, sense_cdb, sizeof(sense_cdb),
                                      0x11000, 18, false), ==, 0);
        qtest_memread(qts, 0x11000, sense, sizeof(sense));
        g_assert_cmpmem(sense, sizeof(sense),
                        expected_sense, sizeof(expected_sense));
    }
    g_assert_cmphex(run_disk_cdb(qts, read_cdb, sizeof(read_cdb),
                                  0x10103, 512, false), ==, 0);
    qtest_memread(qts, 0x10100, guard, sizeof(guard));
    g_assert_cmpint(memcmp(guard + 3, disk_seed, sizeof(disk_seed)), ==, 0);
    g_assert_cmphex(guard[0], ==, 0xa5);
    g_assert_cmphex(guard[1], ==, 0xa5);
    g_assert_cmphex(guard[2], ==, 0xa5);
    g_assert_cmphex(guard[515], ==, 0xa5);

    qtest_memwrite(qts, 0x10205, write_data, sizeof(write_data));
    g_assert_cmphex(run_disk_cdb(qts, write_cdb, sizeof(write_cdb),
                                  0x10205, 512, true), ==, 0);
    qtest_quit(qts);

    fd = open(path, O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, got, sizeof(got), 512), ==, sizeof(got));
    g_assert_cmpint(memcmp(got, write_data, sizeof(got)), ==, 0);
    close(fd);
    unlink(path);
}

static void check_read_callback_buffers_stream(unsigned first_count)
{
    static const uint8_t read_cdb[10] = {
        0x28, 0, 0, 0, 0, 0, 0, 1, 1, 0
    };
    const unsigned total = 257 * 512;
    const unsigned remaining = total - first_count;
    uint8_t program[128] = { 0 };
    g_autofree uint8_t *first = g_malloc(first_count + 16);
    g_autofree uint8_t *pattern = g_malloc(total);
    g_autofree uint8_t *second = g_malloc(remaining + 16);
    g_autofree uint8_t *got = g_malloc(total + 16);
    g_autofree char *path = NULL;
    QTestState *qts;
    int fd = g_file_open_tmp("aic7880-callback-buffers-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    for (unsigned i = 0; i < total; i++) {
        pattern[i] = (i * 37 + 11) ^ (i >> 8) ^ (i >> 16);
    }
    g_assert_cmpint(pwrite(fd, pattern, total, 0), ==, total);
    close(fd);
    qts = start_file_disk(path, NULL);
    prepare_coupled_target(qts, 512);
    /* The target's observed 128 KiB buffer is smaller than this read. */
    g_test_message("synthetic read: 257 x 512 = %u bytes; "
                   "scsi-hd buffer limit: 128 KiB; first segment: %u",
                   total, first_count);
    memset(first, 0xa5, first_count + 16);
    memset(second, 0x5a, remaining + 16);
    qtest_memwrite(qts, 0x20000, first, first_count + 16);
    qtest_memwrite(qts, 0x80000, second, remaining + 16);
    qtest_writeb(qts, REG_BASE + 0x87, 4);
    qtest_writeb(qts, REG_BASE + 0x93, 0);
    host_dma_program(qts, 0x20008, first_count);
    for (unsigned i = 0; i < 3; i++) {
        qtest_writeb(qts, REG_BASE + 0x08 + i, first_count >> (i * 8));
    }
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    start_program(qts, program,
                  build_command_program(program, read_cdb,
                                        sizeof(read_cdb), false));
    coupled_wait(qts, 0x94, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 0x38, ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0);
    host_dma_program(qts, 0x80008, remaining);
    for (unsigned i = 0; i < 3; i++) {
        qtest_writeb(qts, REG_BASE + 0x08 + i, remaining >> (i * 8));
    }
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    qtest_writeb(qts, REG_BASE + 0x93, 0x38);
    coupled_wait(qts, 0x91, 2);
    qtest_writeb(qts, REG_BASE + 0x93, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x22), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==,
                    0x80008 + remaining);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==,
                    0x80008 + remaining);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 9, ==, 9);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    qtest_memread(qts, 0x20000, got, first_count + 16);
    g_assert_cmpmem(got, 8, first, 8);
    g_assert_cmpmem(got + 8, first_count, pattern, first_count);
    g_assert_cmpmem(got + 8 + first_count, 8, first + 8 + first_count, 8);
    qtest_memread(qts, 0x80000, got, remaining + 16);
    g_assert_cmpmem(got, 8, second, 8);
    g_assert_cmpmem(got + 8, remaining, pattern + first_count, remaining);
    g_assert_cmpmem(got + 8 + remaining, 8, second + 8 + remaining, 8);
    qtest_quit(qts);
    unlink(path);
}

static void test_read_callback_buffers_stream(void)
{
    check_read_callback_buffers_stream(17);
    check_read_callback_buffers_stream(128 * 1024);
}

static void run_disk_cdb_two_segments(QTestState *qts, const uint8_t *cdb,
                                      uint32_t first, uint32_t second,
                                      bool write)
{
    uint8_t program[128] = { 0 };

    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    host_dma_program(qts, first, 256);
    qtest_writeb(qts, REG_BASE + 0x08, 0);
    qtest_writeb(qts, REG_BASE + 0x09, 1);
    qtest_writeb(qts, REG_BASE + 0x0a, 0);
    start_program(qts, program,
                  build_command_program(program, cdb, 10, write));
    for (unsigned i = 0; i < 1000; i++) {
        if (!host_dma_read_bytes(qts, 0x8c, 3)) {
            break;
        }
        qtest_clock_step(qts, 1000);
        g_usleep(1000);
    }
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, first + 256);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);

    qtest_writeb(qts, REG_BASE + 0x93, write ? 0x34 : 0x30);
    workload_wait_hdma_disabled(qts);
    host_dma_program(qts, second, 256);
    qtest_writeb(qts, REG_BASE + 0x08, 0);
    qtest_writeb(qts, REG_BASE + 0x09, 1);
    qtest_writeb(qts, REG_BASE + 0x0a, 0);
    qtest_writeb(qts, REG_BASE + 0x93, write ? 0x3c : 0x38);
    for (unsigned i = 0; i < 1000; i++) {
        if (qtest_readb(qts, REG_BASE + 0x91) & 2) {
            break;
        }
        qtest_clock_step(qts, 1000);
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x22), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, second + 256);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x92, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, 1);
}

static void test_read_write_two_segments(void)
{
    static const uint8_t read_cdb[10] = {
        0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0
    };
    static const uint8_t write_cdb[10] = {
        0x2a, 0, 0, 0, 0, 1, 0, 0, 1, 0
    };
    static const uint8_t sense_cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    uint8_t disk_seed[512], write_data[512], first[272], second[272];
    g_autofree char *path = NULL;
    g_autofree char *trace_path = NULL;
    g_autofree char *trace = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("aic7880-two-segments-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 4096), ==, 0);
    for (unsigned i = 0; i < sizeof(disk_seed); i++) {
        disk_seed[i] = i * 37 + 11;
        write_data[i] = i * 91 + 5;
    }
    g_assert_cmpint(pwrite(fd, disk_seed, sizeof(disk_seed), 0), ==,
                    sizeof(disk_seed));
    close(fd);

    trace_path = g_strdup_printf("%s.trace", path);
    qts = start_file_disk(path, trace_path);
    g_assert_cmphex(run_disk_cdb(qts, read_cdb, sizeof(read_cdb),
                                  0x10000, 512, false), ==, 2);
    g_assert_cmphex(run_disk_cdb(qts, sense_cdb, sizeof(sense_cdb),
                                  0x11000, 18, false), ==, 0);
    memset(first, 0xa5, sizeof(first));
    memset(second, 0x5a, sizeof(second));
    qtest_memwrite(qts, 0x20000, first, sizeof(first));
    qtest_memwrite(qts, 0x30000, second, sizeof(second));
    run_disk_cdb_two_segments(qts, read_cdb, 0x20008, 0x30008, false);
    qtest_memread(qts, 0x20000, first, sizeof(first));
    qtest_memread(qts, 0x30000, second, sizeof(second));
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(first[i], ==, 0xa5);
        g_assert_cmphex(first[264 + i], ==, 0xa5);
        g_assert_cmphex(second[i], ==, 0x5a);
        g_assert_cmphex(second[264 + i], ==, 0x5a);
    }
    g_assert_cmpmem(first + 8, 256, disk_seed, 256);
    g_assert_cmpmem(second + 8, 256, disk_seed + 256, 256);

    memcpy(first + 8, write_data, 256);
    memcpy(second + 8, write_data + 256, 256);
    qtest_memwrite(qts, 0x40000, first, sizeof(first));
    qtest_memwrite(qts, 0x50000, second, sizeof(second));
    run_disk_cdb_two_segments(qts, write_cdb, 0x40008, 0x50008, true);
    qtest_memread(qts, 0x40000, first, sizeof(first));
    qtest_memread(qts, 0x50000, second, sizeof(second));
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(first[i], ==, 0xa5);
        g_assert_cmphex(first[264 + i], ==, 0xa5);
        g_assert_cmphex(second[i], ==, 0x5a);
        g_assert_cmphex(second[264 + i], ==, 0x5a);
    }
    g_assert_cmpmem(first + 8, 256, write_data, 256);
    g_assert_cmpmem(second + 8, 256, write_data + 256, 256);
    qtest_quit(qts);

    fd = open(path, O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, disk_seed, sizeof(disk_seed), 512), ==,
                    sizeof(disk_seed));
    g_assert_cmpmem(disk_seed, sizeof(disk_seed), write_data,
                    sizeof(write_data));
    close(fd);
    g_assert_true(g_file_get_contents(trace_path, &trace, NULL, NULL));
    g_assert_true(g_regex_match_simple(
        "scsi_disk_new_request[^\\n]*data=28 00 00 00 00 00 00 00 01 00",
        trace, 0, 0));
    g_assert_true(g_regex_match_simple(
        "scsi_disk_new_request[^\\n]*data=2a 00 00 00 00 01 00 00 01 00",
        trace, 0, 0));
    unlink(trace_path);
    unlink(path);
}

static void test_empty_fifo_flush_during_request(void)
{
    static const uint8_t read_cdb[10] = {
        0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0
    };
    static const uint8_t sense_cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    uint8_t program[128] = { 0 };
    uint8_t guard[528], after[sizeof(guard)];
    QTestState *qts = start_peer_disk();

    g_assert_cmphex(run_disk_cdb(qts, read_cdb, sizeof(read_cdb),
                                  0x10000, 512, false), ==, 2);
    g_assert_cmphex(run_disk_cdb(qts, sense_cdb, sizeof(sense_cdb),
                                  0x11000, 18, false), ==, 0);

    /* Retain the real request before delivered data reaches the FIFO. */
    peer_event(qts, "hold-data-in");
    memset(guard, 0xa5, sizeof(guard));
    qtest_memwrite(qts, 0x12000, guard, sizeof(guard));
    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x88, 0x03);
    qtest_writeb(qts, REG_BASE + 0x89, 0x20);
    qtest_writeb(qts, REG_BASE + 0x8a, 0x01);
    qtest_writeb(qts, REG_BASE + 0x8b, 0);
    qtest_writeb(qts, REG_BASE + 0x8c, 0);
    qtest_writeb(qts, REG_BASE + 0x8d, 2);
    qtest_writeb(qts, REG_BASE + 0x8e, 0);
    qtest_writeb(qts, REG_BASE + 0x08, 0);
    qtest_writeb(qts, REG_BASE + 0x09, 2);
    qtest_writeb(qts, REG_BASE + 0x0a, 0);
    start_program(qts, program,
                  build_command_program(program, read_cdb,
                                        sizeof(read_cdb), false));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    for (unsigned i = 0; i < 1000; i++) {
        if ((qtest_readb(qts, REG_BASE + 0x03) & 0xe0) == 0x40) {
            break;
        }
        qtest_clock_step(qts, 1000);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x09), ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 0x3a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x08, 3), ==, 512);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 512);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_memread(qts, 0x12000, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), guard, sizeof(guard));
    qtest_quit(qts);
}

static void check_reset_cancels_busy_request(bool pci_reset)
{
    static const uint8_t read_cdb[10] = {
        0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0
    };
    static const uint8_t sense_cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    uint8_t program[128] = { 0 };
    uint8_t guard[512], after[512];
    g_autofree char *path = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("aic7880-busy-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 4096), ==, 0);
    close(fd);
    qts = start_throttled_disk(path);
    g_assert_cmphex(run_disk_cdb(qts, read_cdb, sizeof(read_cdb),
                                  0x10000, 512, false), ==, 2);
    g_assert_cmphex(run_disk_cdb(qts, sense_cdb, sizeof(sense_cdb),
                                  0x11000, 18, false), ==, 0);
    g_assert_cmphex(run_disk_cdb(qts, read_cdb, sizeof(read_cdb),
                                  0x10000, 512, false), ==, 0);

    memset(guard, 0xa5, sizeof(guard));
    qtest_memwrite(qts, 0x12000, guard, sizeof(guard));
    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x88, 0x00);
    qtest_writeb(qts, REG_BASE + 0x89, 0x20);
    qtest_writeb(qts, REG_BASE + 0x8a, 0x01);
    qtest_writeb(qts, REG_BASE + 0x8c, 0x00);
    qtest_writeb(qts, REG_BASE + 0x8d, 0x02);
    qtest_writeb(qts, REG_BASE + 0x08, 0x00);
    qtest_writeb(qts, REG_BASE + 0x09, 0x02);
    start_program(qts, program,
                  build_command_program(program, read_cdb,
                                        sizeof(read_cdb), false));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x00, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0);
    if (pci_reset) {
        qobject_unref(qtest_qmp(qts, "{'execute': 'system_reset'}"));
        qtest_qmp_eventwait(qts, "RESET");
        config_writel(qts, 0x14, REG_BASE);
        config_writew(qts, 0x04, 0x0006);
    } else {
        qtest_writeb(qts, REG_BASE + 0x87, 1);
    }
    g_usleep(1500000);
    qtest_clock_step(qts, 1000000000);
    qtest_memread(qts, 0x12000, after, sizeof(after));
    g_assert_cmpint(memcmp(guard, after, sizeof(guard)), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_quit(qts);
    unlink(path);
}

static void test_reset_cancels_busy_request(void)
{
    check_reset_cancels_busy_request(false);
}

static void test_pci_reset_cancels_busy_request(void)
{
    check_reset_cancels_busy_request(true);
}

static void test_independent_functions(void)
{
    QTestState *qts = qtest_init("-M pc -m 128M -nodefaults "
                                 "-device aic7880,bus=pci.0,addr=04.0 "
                                 "-device aic7880,bus=pci.0,addr=05.0");

    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80002814u);
    qtest_outl(qts, PCI_CONFIG_DATA, REG_BASE + 0x1000);
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80002804u);
    qtest_outw(qts, PCI_CONFIG_DATA, 0x0006);
    qtest_writeb(qts, REG_BASE + 0x20, 0x5a);
    qtest_writeb(qts, REG_BASE + 0x1000 + 0x20, 0xa5);
    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA, 10);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    qtest_writeb(qts, REG_BASE + 0x87, 0x16);
    qtest_writeb(qts, REG_BASE + 0x1000 + 0x87, 0x16);
    g_assert_true(qtest_get_irq(qts, 11));
    g_assert_true(qtest_get_irq(qts, 10));
    qtest_writeb(qts, REG_BASE + 0x87, 0x06);
    g_assert_false(qtest_get_irq(qts, 11));
    g_assert_true(qtest_get_irq(qts, 10));
    qtest_writeb(qts, REG_BASE + 0x1000 + 0x87, 0x06);
    g_assert_false(qtest_get_irq(qts, 10));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1000 + 0x20), ==, 0xa5);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1000 + 0x20), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1000 + 0x87), ==, 0x06);
    qtest_quit(qts);
}

static void test_paused_state_migrates(void)
{
    static const uint8_t program[] = { 0xb1, 0x6a, 0x91, 0x00 };
    uint8_t pre_program[8];
    uint8_t queue_program[12];
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-migrate-XXXXXX", &error);
    g_autofree char *uri = NULL;
    g_autofree char *socket_path = NULL;
    QTestState *from, *to;
    size_t n = 0;
    size_t pre_n = 0;

    g_assert_no_error(error);
    socket_path = g_strdup_printf("%s/migrate.sock", dir);
    uri = g_strdup_printf("unix:%s", socket_path);
    from = qtest_init("-M pc -m 128M -nodefaults -S "
                      "-device aic7880,bus=pci.0,addr=04.0");
    to = qtest_init("-M pc -m 128M -nodefaults -S -incoming defer "
                    "-device aic7880,bus=pci.0,addr=04.0");
    config_writel(from, 0x14, REG_BASE);
    config_writew(from, 0x04, 0x0006);
    qtest_writeb(from, REG_BASE + 0x20, 0x5a);
    qtest_writeb(from, REG_BASE + 0x90, 2);
    qtest_writeb(from, REG_BASE + 0xa0, 0x35);
    qtest_writeb(from, REG_BASE + 0x9b, 2);
    qtest_writeb(from, REG_BASE + 0x9b, 5);
    emit(pre_program, &pre_n, alu_word(0, 0, 0x9b, 0x24, false));
    emit_or(pre_program, &pre_n, 1, 0x91);
    start_program(from, pre_program, pre_n);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x24), ==, 2);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x9c), ==, 1);
    qtest_writeb(from, REG_BASE + 0x65, 0x30);
    qtest_writeb(from, REG_BASE + 0x60, 0x91);
    qtest_writeb(from, REG_BASE + 0x62, 0);
    for (size_t i = 0; i < sizeof(program); i++) {
        qtest_writeb(from, REG_BASE + 0x61, program[i]);
    }
    qtest_writeb(from, REG_BASE + 0x60, 0x90);
    qtest_writeb(from, REG_BASE + 0x62, 0);
    resume_program(from, 2);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x91), ==, 0xb1);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(config_readl(to, 0x14) & ~0x0fu, ==, REG_BASE);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x20), ==, 0x5a);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x90), ==, 2);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0xa0), ==, 0x35);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9c), ==, 1);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x65), ==, 0x30);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x91), ==, 0xb1);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x87) & 4, ==, 4);
    qtest_writeb(to, REG_BASE + 0x60, 0x91);
    qtest_writeb(to, REG_BASE + 0x62, 0);
    for (size_t i = 0; i < sizeof(program); i++) {
        g_assert_cmphex(qtest_readb(to, REG_BASE + 0x61), ==, program[i]);
    }
    qtest_writeb(to, REG_BASE + 0x9b, 7);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9c), ==, 2);
    emit(queue_program, &n, alu_word(0, 0, 0x9b, 0x20, false));
    emit(queue_program, &n, alu_word(0, 0, 0x9b, 0x21, false));
    emit(queue_program, &n, alu_word(0, 1, 0x6a, 0x91, false));
    start_program(to, queue_program, n);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x20), ==, 5);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x21), ==, 7);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9c), ==, 0);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket_path);
    g_rmdir(dir);
}

static void test_dspcistatus_selector(void)
{
    QTestState *qts = start_aic();
    uint8_t low = qtest_readb(qts, REG_BASE + 0x86) & 0x3f;

    config_writel(qts, 0x10, 0xc001);
    config_writew(qts, 0x04, 0x0007);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86) & 0xc0, ==, 0);
    g_assert_cmphex(qtest_inb(qts, 0xc086) & 0xc0, ==, 0);

    qtest_outb(qts, 0xc086, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86), ==, 0x80 | low);
    g_assert_cmphex(qtest_inb(qts, 0xc086), ==, 0x80 | low);
    qtest_writeb(qts, REG_BASE + 0x86, 0x40);
    g_assert_cmphex(qtest_inb(qts, 0xc086), ==, 0x40 | low);
    qtest_outb(qts, 0xc086, 0xc0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86), ==, 0xc0 | low);
    qtest_writeb(qts, REG_BASE + 0x86, 0);
    g_assert_cmphex(qtest_inb(qts, 0xc086), ==, low);

    qtest_writeb(qts, REG_BASE + 0x86, 0x80);
    qtest_outb(qts, 0xc086, 0xbf);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86), ==, 0x80 | low);
    g_assert_cmphex(qtest_inb(qts, 0xc086), ==, 0x80 | low);
    qtest_quit(qts);
}

static void test_dspcistatus_resets(void)
{
    QTestState *qts = start_aic();
    uint32_t pci_command;

    config_writel(qts, 0x10, 0xc001);
    config_writew(qts, 0x04, 0x0007);
    pci_command = config_readl(qts, 0x04);
    qtest_writeb(qts, REG_BASE + 0x86, 0xc0);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86) & 0xc0, ==, 0);
    g_assert_cmphex(config_readl(qts, 0x04), ==, pci_command);

    qtest_writeb(qts, REG_BASE + 0x86, 0x40);
    qtest_outb(qts, 0xc087, 1);
    g_assert_cmphex(qtest_inb(qts, 0xc086) & 0xc0, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x86, 0x40);
    qtest_system_reset(qts);
    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86) & 0xc0, ==, 0);
    qtest_quit(qts);
}

static void test_dspcistatus_migrates(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-dspcistatus-XXXXXX",
                                         &error);
    g_autofree char *socket_path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket_path = g_build_filename(dir, "migrate.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket_path);
    from = qtest_init("-M pc -m 128M -nodefaults -S "
                      "-device aic7880,bus=pci.0,addr=04.0");
    to = qtest_init("-M pc -m 128M -nodefaults -S -incoming defer "
                    "-device aic7880,bus=pci.0,addr=04.0");
    config_writel(from, 0x14, REG_BASE);
    config_writew(from, 0x04, 0x0006);
    qtest_writeb(from, REG_BASE + 0x86, 0x80);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x86) & 0xc0, ==, 0x80);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(config_readl(to, 0x14) & ~0x0fu, ==, REG_BASE);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x86) & 0xc0, ==, 0x80);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket_path);
    g_rmdir(dir);
}

static void test_dspcistatus_rejects_sequencer_access(void)
{
    for (unsigned write = 0; write < 2; write++) {
        uint8_t program[8];
        QTestState *qts = start_aic();
        uint32_t pci_status;
        size_t n = 0;

        qtest_writeb(qts, REG_BASE + 0x86, 0x80);
        qtest_writeb(qts, REG_BASE + 0x20, 0x5a);
        pci_status = config_readl(qts, 0x04) >> 16;
        if (write) {
            emit_or(program, &n, 0xc0, 0x86);
        } else {
            emit(program, &n, alu_word(1, 0xff, 0x86, 0x20, false));
        }
        emit_or(program, &n, 1, 0x91);
        start_program(qts, program, n);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86) & 0xc0, ==, 0x80);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
        g_assert_cmphex(config_readl(qts, 0x04) >> 16, ==, pci_status);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 0);
        qtest_quit(qts);
    }
}

static void test_dspcistatus_rejects_host_words(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-dspcistatus-word-XXXXXX",
                                         &error);
    g_autofree char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    uint8_t hcntrl;
    uint16_t half;

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    qts = qtest_initf("-M pc -m 128M -nodefaults -d guest_errors -D %s "
                      "-device aic7880,id=hba,bus=pci.0,addr=04.0"
                      MEMTX_TEST_DEVICE, log_path);
    config_writel(qts, 0x14, REG_BASE);
    config_writel(qts, 0x10, 0xc001);
    config_writew(qts, 0x04, 0x0007);
    qtest_writeb(qts, REG_BASE + 0x86, 0xc0);
    hcntrl = qtest_readb(qts, REG_BASE + 0x87);

    /* Other registers retain the existing byte-split word behavior. */
    g_assert_cmpint(memtx_writew(qts, REG_BASE + 0x20, 0xa55a), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, 0xa5);

    g_assert_cmpint(memtx_readw(qts, REG_BASE + 0x86, &half), !=, MEMTX_OK);
    g_test_message("PCI STATUS after memory word read: 0x%04x",
                   config_readl(qts, 0x04) >> 16);
    g_assert_cmpint(memtx_writew(qts, REG_BASE + 0x86, 0), !=, MEMTX_OK);
    g_test_message("PCI STATUS after memory word write: 0x%04x",
                   config_readl(qts, 0x04) >> 16);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86) & 0xc0, ==, 0xc0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87), ==, hcntrl);

    qtest_inw(qts, 0xc086);
    g_test_message("PCI STATUS after I/O word read: 0x%04x",
                   config_readl(qts, 0x04) >> 16);
    qtest_outw(qts, 0xc086, 0);
    g_test_message("PCI STATUS after I/O word write: 0x%04x",
                   config_readl(qts, 0x04) >> 16);
    g_assert_cmphex(qtest_inb(qts, 0xc086) & 0xc0, ==, 0xc0);
    g_assert_cmphex(qtest_inb(qts, 0xc087), ==, hcntrl);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    {
        const char *at = log;
        unsigned refusals = 0;

        while ((at = strstr(at, "unsupported DSPCISTATUS width"))) {
            refusals++;
            at++;
        }
        g_assert_cmpuint(refusals, ==, 4);
    }
    unlink(log_path);
    g_rmdir(dir);
}

static void test_o2_sibling_policy_default(void)
{
    QTestState *qts = start_aic();
    int irq = -1;

    g_assert_false(qtest_qom_get_bool(qts, "/machine/peripheral/hba",
                                       "o2-pci-diagnostic"));
    g_assert_false(qtest_qom_get_bool(qts, "/machine/peripheral/hba",
                                       "o2-storage-diagnostic"));
    config_writew(qts, 0x04, 0x0046);
    g_assert_cmphex(config_readl(qts, 0x04) & 0xffff, ==, 0x0046);
    config_writel(qts, 0x14, UINT32_MAX);
    g_assert_cmphex(config_readl(qts, 0x14), ==, 0xfffff000u);
    config_writel(qts, 0x14, REG_BASE);
    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    qtest_writeb(qts, REG_BASE + 0x87, 0x16);
    for (int pin = 0; pin < 24; pin++) {
        if (qtest_get_irq(qts, pin)) {
            irq = pin;
            break;
        }
    }
    g_assert_cmpint(irq, >=, 0);
    config_writew(qts, 0x04, 0x0004);
    g_assert_false(qtest_get_irq(qts, irq));
    config_writew(qts, 0x04, 0x0006);
    g_assert_true(qtest_get_irq(qts, irq));
    qtest_writeb(qts, REG_BASE + 0x87, 0x06);
    g_assert_false(qtest_get_irq(qts, irq));
    qtest_quit(qts);
}

static void test_o2_sibling_bar_repeats_last_slice(void)
{
    QTestState *qts = qtest_init(MEMTX_TEST_DEVICE
                                 "-M pc -m 128M -nodefaults "
                                 "-device aic7880,bus=pci.0,addr=04.0,"
                                 "o2-pci-diagnostic=on");
    uint8_t hcntrl;
    uint16_t half;

    config_writel(qts, 0x14, UINT32_MAX);
    g_assert_cmphex(config_readl(qts, 0x14), ==, 0xfffff000u);
    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    qtest_writeb(qts, REG_BASE + 0x20, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xf20), ==, 0x5a);
    qtest_writeb(qts, REG_BASE + 0xf21, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
    qtest_writeb(qts, REG_BASE + 0xf86, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86) & 0xc0, ==, 0x80);
    qtest_writeb(qts, REG_BASE + 0x86, 0xc0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xf86) & 0xc0, ==, 0xc0);
    hcntrl = qtest_readb(qts, REG_BASE + 0xf87);
    g_assert_cmpint(memtx_readw(qts, REG_BASE + 0xf86, &half), !=, MEMTX_OK);
    g_test_message("PCI STATUS after last-slice word read: 0x%04x",
                   config_readl(qts, 0x04) >> 16);
    g_assert_cmpint(memtx_writew(qts, REG_BASE + 0xf86, 0), !=, MEMTX_OK);
    g_test_message("PCI STATUS after last-slice word write: 0x%04x",
                   config_readl(qts, 0x04) >> 16);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xf86) & 0xc0, ==, 0xc0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xf87), ==, hcntrl);
    qtest_writeb(qts, REG_BASE + 0xf87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x86) & 0xc0, ==, 0);
    qtest_quit(qts);
}

static void assert_o2_sibling_scratch_aliases(QTestState *qts, uint8_t value)
{
    g_assert_cmphex(qtest_inb(qts, 0xc020), ==, value);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, value);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x120), ==, value);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xf20), ==, value);
}

static void test_o2_sibling_command_parity_readback(void)
{
    QTestState *qts = qtest_init("-M pc -m 128M -nodefaults "
                                 "-device aic7880,bus=pci.0,addr=04.0,"
                                 "o2-pci-diagnostic=on");

    config_writew(qts, 0x04, 0);
    config_writel(qts, 0x10, UINT32_MAX);
    g_assert_cmphex(config_readl(qts, 0x10), ==, 0xffffff01u);
    config_writel(qts, 0x14, UINT32_MAX);
    g_assert_cmphex(config_readl(qts, 0x14), ==, 0xfffff000u);
    config_writel(qts, 0x10, 0xc001);
    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0047);
    qtest_outb(qts, 0xc020, 0x3c);
    assert_o2_sibling_scratch_aliases(qts, 0x3c);
    qtest_writeb(qts, REG_BASE + 0x120, 0x5a);
    assert_o2_sibling_scratch_aliases(qts, 0x5a);
    config_writew(qts, 0x04, 0x0046);
    g_assert_cmphex(config_readl(qts, 0x04) & 0xffff, ==, 0x0046);
    qtest_outb(qts, 0xc020, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
    config_writew(qts, 0x04, 0x0045);
    g_assert_cmphex(config_readl(qts, 0x04) & 0xffff, ==, 0x0045);
    qtest_writeb(qts, REG_BASE + 0x120, 0x6d);
    g_assert_cmphex(qtest_inb(qts, 0xc020), ==, 0x5a);
    config_writew(qts, 0x04, 0x0040);
    g_assert_cmphex(config_readl(qts, 0x04) & 0xffff, ==, 0x0040);
    config_writew(qts, 0x04, 0);
    g_assert_cmphex(config_readl(qts, 0x04) & 0xffff, ==, 0);
    qtest_quit(qts);
}

static void test_o2_sibling_policy_requires_qtest(void)
{
    const char *argv[] = {
        qtest_qemu_binary(NULL), "-M", "pc", "-accel", "tcg", "-S",
        "-display", "none", "-nodefaults", "-qmp", "stdio", "-device",
        "aic7880,o2-pci-diagnostic=on", NULL,
    };
    g_autoptr(GSubprocess) proc = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *out = NULL;
    g_autofree char *err = NULL;

    proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE |
                            G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                            G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
    g_assert_no_error(error);
    g_assert_true(g_subprocess_communicate_utf8(proc,
        "{\"execute\":\"qmp_capabilities\"}\n"
        "{\"execute\":\"quit\"}\n", NULL, &out, &err, &error));
    g_assert_no_error(error);
    g_assert_false(g_subprocess_get_successful(proc));
    g_assert_nonnull(strstr(err, "AIC PCI diagnostic requires QTest"));
}

static void test_short_inquiry_target_requires_qtest(void)
{
    const char *argv[] = {
        qtest_qemu_binary(NULL), "-M", "pc", "-accel", "tcg", "-S",
        "-display", "none", "-nodefaults", "-device",
        "aic7880,id=hba,bus=pci.0,addr=04.0", "-device",
        "aic7880-short-inquiry-target,bus=hba.0,scsi-id=0,lun=0", NULL,
    };
    g_autoptr(GSubprocess) proc = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *out = NULL;
    g_autofree char *err = NULL;

    proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                            G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
    g_assert_no_error(error);
    g_assert_true(g_subprocess_communicate_utf8(proc, NULL, NULL,
                                                &out, &err, &error));
    g_assert_no_error(error);
    g_assert_false(g_subprocess_get_successful(proc));
    g_assert_nonnull(strstr(err,
                            "AIC-7880 short INQUIRY target requires QTest"));
}

static void test_o2_storage_diagnostic_requires_machine(void)
{
    const char *argv[] = {
        qtest_qemu_binary(NULL), "-M", "pc", "-accel", "tcg", "-S",
        "-display", "none", "-nodefaults", "-device",
        "aic7880,o2-storage-diagnostic=on", NULL,
    };
    g_autoptr(GSubprocess) proc = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *out = NULL;
    g_autofree char *err = NULL;

    proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE |
                            G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                            G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
    g_assert_no_error(error);
    g_assert_true(g_subprocess_communicate_utf8(proc, NULL, NULL,
                                                &out, &err, &error));
    g_assert_no_error(error);
    g_assert_false(g_subprocess_get_successful(proc));
    g_assert_nonnull(strstr(err, "AIC storage diagnostic requires O2"));
}

static void test_o2_diagnostics_are_exclusive(void)
{
    const char *argv[] = {
        qtest_qemu_binary(NULL), "-M", "pc", "-accel", "qtest", "-S",
        "-display", "none", "-nodefaults", "-device",
        "aic7880,o2-pci-diagnostic=on,o2-storage-diagnostic=on", NULL,
    };
    g_autoptr(GSubprocess) proc = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *out = NULL;
    g_autofree char *err = NULL;

    proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE |
                            G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                            G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
    g_assert_no_error(error);
    g_assert_true(g_subprocess_communicate_utf8(proc, NULL, NULL,
                                                &out, &err, &error));
    g_assert_no_error(error);
    g_assert_false(g_subprocess_get_successful(proc));
    g_assert_nonnull(strstr(err,
                            "AIC PCI and storage diagnostics are exclusive"));
}

/* Manual transfers follow the independently reviewed public contract. */
static const uint8_t manual_guard_regs[] = {
    0x01, 0x03, 0x04, 0x05, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
    0x10, 0x11, 0x14, 0x15, 0x16, 0x17,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e,
    0x91, 0x93, 0x94, 0x9c, 0x9e,
};

typedef struct ManualGuard {
    uint8_t regs[sizeof(manual_guard_regs)];
    uint8_t ram[32];
} ManualGuard;

static void manual_capture(QTestState *qts, ManualGuard *guard)
{
    for (unsigned i = 0; i < sizeof(manual_guard_regs); i++) {
        guard->regs[i] = qtest_readb(qts, REG_BASE + manual_guard_regs[i]);
    }
    qtest_memread(qts, 0x18000, guard->ram, sizeof(guard->ram));
}

static void manual_unchanged(QTestState *qts, const ManualGuard *before)
{
    ManualGuard after;

    manual_capture(qts, &after);
    for (unsigned i = 0; i < sizeof(manual_guard_regs); i++) {
        if (after.regs[i] != before->regs[i]) {
            g_test_message("preserve register 0x%02x", manual_guard_regs[i]);
        }
        g_assert_cmphex(after.regs[i], ==, before->regs[i]);
    }
    g_assert_cmpmem(after.ram, sizeof(after.ram), before->ram,
                    sizeof(before->ram));
}

static void manual_counters(QTestState *qts)
{
    uint8_t ram[32];

    memset(ram, 0xa7, sizeof(ram));
    qtest_memwrite(qts, 0x18000, ram, sizeof(ram));
    host_dma_program(qts, 0x18008, 0x2345);
    for (unsigned i = 0; i < 4; i++) {
        qtest_writeb(qts, REG_BASE + 0x14 + i, 0x12345678 >> (8 * i));
    }
    for (unsigned i = 0; i < 3; i++) {
        qtest_writeb(qts, REG_BASE + 8 + i, 0x345678 >> (8 * i));
    }
}

static void manual_check_counters(QTestState *qts)
{
    uint8_t ram[32], expected[32];

    g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, 0x345678);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x12345678);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x18008);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0x2345);
    qtest_memread(qts, 0x18000, ram, sizeof(ram));
    memset(expected, 0xa7, sizeof(expected));
    g_assert_cmpmem(ram, sizeof(ram), expected, sizeof(expected));
}

static void manual_signal_write(QTestState *qts, uint8_t value, bool sequencer)
{
    if (sequencer) {
        attention_write_raw(qts, 3, value);
        qtest_writeb(qts, REG_BASE + 0x92, 1);
    } else {
        qtest_writeb(qts, REG_BASE + 3, value);
    }
}

static void manual_ack(QTestState *qts, uint8_t phase, uint8_t byte,
                       bool incoming, bool sequencer)
{
    uint8_t signal = qtest_readb(qts, REG_BASE + 3);
    uint8_t sdone = qtest_readb(qts, REG_BASE + 0x0b) & 4;
    uint8_t hdone = qtest_readb(qts, REG_BASE + 0x94) & 8;

    g_assert_cmphex(signal & 0xe3, ==, phase | 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 2, ==, 0);
    manual_signal_write(qts, phase | 1, sequencer);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe3, ==, phase | 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 0);
    if (incoming) {
        for (unsigned i = 0; i < 3; i++) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, byte);
        }
    }
    manual_signal_write(qts, phase | 1, sequencer);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe3, ==, phase | 1);
    manual_signal_write(qts, phase, sequencer);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 4, ==, sdone);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, hdone);
}

static uint8_t manual_incoming(QTestState *qts, uint8_t phase, bool sequencer)
{
    uint8_t byte;
    ManualGuard before;

    manual_wait_phase(qts, phase);
    byte = qtest_readb(qts, REG_BASE + 0x12);
    manual_capture(qts, &before);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x12), ==, byte);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, byte);
        manual_unchanged(qts, &before);
    }
    manual_ack(qts, phase, byte, true, sequencer);
    return byte;
}

static void manual_finish(QTestState *qts, uint8_t status)
{
    g_assert_cmphex(manual_incoming(qts, 0xc0, false), ==, status);
    g_assert_cmphex(manual_incoming(qts, 0xe0, false), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
}

static QTestState *manual_status(void)
{
    static const uint8_t cdb[6] = { 0 };
    QTestState *qts = start_disk();

    manual_command(qts, cdb, sizeof(cdb));
    manual_wait_phase(qts, 0xc0);
    return qts;
}

static const uint8_t manual_fifo_payload[36] = {
    0, 0, 5, 0x12, 31, 0, 0, 0x10,
    'L', 'A', 'B', 'T', 'E', 'S', 'T', ' ',
    'S', 'E', 'G', 'M', 'E', 'N', 'T', 'E',
    'D', '-', 'D', 'I', 'S', 'K', ' ', ' ',
    '1', '.', '0', 0
};

static QTestState *manual_status_fifo(void)
{
    uint8_t program[64];
    QTestState *qts = start_selected_disk(NULL);

    host_dma_program(qts, 0x10000, 40);
    qtest_writeb(qts, REG_BASE + 8, 40);
    qtest_writeb(qts, REG_BASE + 0x1f, 0x22);
    start_program(qts, program, build_selected_inquiry_program(program));
    manual_wait_phase(qts, 0xc0);
    qtest_writeb(qts, REG_BASE + 0x93, 0);
    qtest_writeb(qts, REG_BASE + 1, 0);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 0);
    return qts;
}

static void manual_check_fifo(QTestState *qts)
{
    uint8_t got[sizeof(manual_fifo_payload)];

    for (unsigned i = 0; i < sizeof(got); i++) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 0);
        got[i] = qtest_readb(qts, REG_BASE + 0x99);
    }
    g_assert_cmpmem(got, sizeof(got), manual_fifo_payload,
                    sizeof(manual_fifo_payload));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
}

static void test_manual_signals(void)
{
    for (unsigned sequencer = 0; sequencer < 2; sequencer++) {
        QTestState *qts = manual_status();
        uint8_t status = qtest_readb(qts, REG_BASE + 0x12);

        manual_signal_write(qts, 0xcc, sequencer);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xef, ==, 0xce);
        manual_signal_write(qts, 0xc2, sequencer);
        /* The selected target keeps BSY asserted after BSYO falls. */
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xef, ==, 0xc6);
        manual_signal_write(qts, 0xcc, sequencer);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xef, ==, 0xce);
        qtest_writeb(qts, REG_BASE + 1, 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xef, ==, 0xce);
        qtest_writeb(qts, REG_BASE + 0x87, 6);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xef, ==, 0xce);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x12), ==, status);
        manual_finish(qts, status);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
        qtest_quit(qts);
    }
}

static void test_manual_inquiry(void)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t expected[36], got[36];
    QTestState *qts = start_selected_disk(NULL);

    /* Derive the target's response through its existing DMA transport. */
    g_assert_cmphex(run_disk_cdb(qts, cdb, sizeof(cdb), 0x10000, 36,
                                  false), ==, 0);
    qtest_memread(qts, 0x10000, expected, sizeof(expected));
    qtest_quit(qts);
    for (unsigned sequencer = 0; sequencer < 2; sequencer++) {
        qts = start_selected_disk(NULL);
        manual_command(qts, cdb, sizeof(cdb));
        manual_wait_phase(qts, 0x40);
        manual_counters(qts);
        for (unsigned i = 0; i < sizeof(got); i++) {
            got[i] = manual_incoming(qts, 0x40, sequencer);
            manual_check_counters(qts);
            if (!sequencer && i + 1 < sizeof(got)) {
                ManualGuard before;

                manual_capture(qts, &before);
                /* The held latch did not sample the next byte. */
                manual_signal_write(qts, 0x41, sequencer);
                manual_unchanged(qts, &before);
            }
        }
        g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
        manual_finish(qts, 0);
        manual_check_counters(qts);
        qtest_quit(qts);
    }
}

static void test_manual_modes(void)
{
    for (unsigned message = 0; message < 2; message++) {
        for (unsigned automatic = 0; automatic < 2; automatic++) {
            QTestState *qts = manual_status();
            uint8_t phase = message ? 0xe0 : 0xc0;
            uint8_t byte = message ? 0 : 2;

            if (message) {
                g_assert_cmphex(manual_incoming(qts, 0xc0, false), ==, 2);
            }
            manual_wait_phase(qts, phase);
            manual_counters(qts);
            if (automatic) {
                qtest_writeb(qts, REG_BASE + 1, 8);
            } else {
                qtest_writeb(qts, REG_BASE + 1, 8);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x12), ==, byte);
                qtest_writeb(qts, REG_BASE + 1, 0);
            }
            for (unsigned i = 0; i < 3; i++) {
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x12), ==, byte);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe3,
                                ==, phase | 2);
            }
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, byte);
            if (!automatic) {
                manual_ack(qts, phase, byte, true, false);
            }
            qtest_writeb(qts, REG_BASE + 1, 0);
            if (!message) {
                g_assert_cmphex(manual_incoming(qts, 0xe0, false), ==, 0);
            }
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
            manual_check_counters(qts);
            qtest_quit(qts);
        }
    }
}

static void test_manual_mode_exclusion(void)
{
    static const uint8_t inquiry[] = { 0x12, 0, 0, 0, 36, 0 };

    for (unsigned data = 0; data < 2; data++) {
        for (unsigned high = 0; high < 2; high++) {
            QTestState *qts = data ? start_selected_disk(NULL) :
                                    manual_status();
            uint8_t phase = data ? 0x40 : 0xc0;
            uint8_t byte;
            ManualGuard before;

            if (data) {
                manual_command(qts, inquiry, sizeof(inquiry));
                manual_wait_phase(qts, phase);
            }
            manual_counters(qts);
            byte = qtest_readb(qts, REG_BASE + 6);
            if (high) {
                qtest_writeb(qts, REG_BASE + 3, phase | 1);
            }
            manual_capture(qts, &before);
            qtest_writeb(qts, REG_BASE + 1, 8);
            manual_unchanged(qts, &before);
            qtest_writeb(qts, REG_BASE + 1, 10);
            manual_unchanged(qts, &before);
            qtest_writeb(qts, REG_BASE + 0x93, 0x30);
            manual_unchanged(qts, &before);
            qtest_writeb(qts, REG_BASE + 0x93, 0x08);
            manual_unchanged(qts, &before);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, byte);
            if (high) {
                qtest_writeb(qts, REG_BASE + 3, phase);
            } else {
                manual_ack(qts, phase, byte, true, false);
            }
            if (data) {
                for (unsigned i = 1; i < 36; i++) {
                    manual_incoming(qts, 0x40, false);
                }
                manual_finish(qts, 0);
            } else {
                g_assert_cmphex(manual_incoming(qts, 0xe0, false), ==, 0);
            }
            manual_check_counters(qts);
            qtest_quit(qts);
        }
    }
}

static void test_manual_sample_channel_clear(void)
{
    static const uint8_t inquiry[] = { 0x12, 0, 0, 0, 36, 0 };

    for (unsigned data = 0; data < 2; data++) {
        for (unsigned automatic = 0; automatic < 2; automatic++) {
            QTestState *qts = data ? start_selected_disk(NULL) :
                                    manual_status_fifo();
            uint8_t phase = data ? 0x40 : 0xc0;
            uint8_t byte;
            ManualGuard before;

            if (data) {
                manual_command(qts, inquiry, sizeof(inquiry));
                manual_wait_phase(qts, phase);
            }
            manual_counters(qts);
            byte = qtest_readb(qts, REG_BASE + 6);
            manual_capture(qts, &before);
            qtest_writeb(qts, REG_BASE + 1, 2);
            manual_unchanged(qts, &before);
            /* Channel clear discarded the local sample, not the target byte. */
            if (!automatic) {
                qtest_writeb(qts, REG_BASE + 3, phase | 1);
                manual_unchanged(qts, &before);
            }
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x12), ==, byte);
            if (automatic && !data) {
                qtest_writeb(qts, REG_BASE + 1, 8);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, byte);
                qtest_writeb(qts, REG_BASE + 1, 0);
            } else if (automatic) {
                uint8_t got[52], guard[52];

                memset(guard, 0xa5, sizeof(guard));
                qtest_memwrite(qts, 0x10000, guard, sizeof(guard));
                host_dma_program(qts, 0x10008, 36);
                qtest_writeb(qts, REG_BASE + 8, 36);
                qtest_writeb(qts, REG_BASE + 9, 0);
                qtest_writeb(qts, REG_BASE + 10, 0);
                qtest_writeb(qts, REG_BASE + 0x93, 0x38);
                for (unsigned i = 0; i < 1000 &&
                     host_dma_read_bytes(qts, 8, 3); i++) {
                    qtest_clock_step(qts, 1000);
                }
                g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, 0);
                g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
                g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x1002c);
                qtest_memread(qts, 0x10000, got, sizeof(got));
                g_assert_cmpmem(got, 8, guard, 8);
                g_assert_cmpmem(got + 44, 8, guard + 44, 8);
                g_assert_cmphex(got[8], ==, byte);
                g_assert_cmpmem(got + 16, 7, "LABTEST", 7);
                qtest_writeb(qts, REG_BASE + 0x93, 0);
            } else {
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, byte);
                manual_ack(qts, phase, byte, true, false);
            }
            if (data) {
                if (!automatic) {
                    for (unsigned i = 1; i < 36; i++) {
                        manual_incoming(qts, 0x40, false);
                    }
                    manual_check_counters(qts);
                }
                manual_finish(qts, 0);
            } else {
                g_assert_cmphex(manual_incoming(qts, 0xe0, false), ==, 0);
                manual_check_counters(qts);
                manual_check_fifo(qts);
            }
            qtest_quit(qts);
        }
    }
}

static void test_manual_file_data(void)
{
    static const uint8_t read_cdb[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0 };
    static const uint8_t write_cdb[10] = { 0x2a, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
    uint8_t seed[512], payload[512], got[512];
    g_autofree char *path = NULL;
    QTestState *qts;
    int fd = g_file_open_tmp("aic7880-manual-disk-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 4096), ==, 0);
    for (unsigned i = 0; i < sizeof(seed); i++) {
        seed[i] = (i * 37 + 0x19) ^ (i >> 3);
        payload[i] = (i * 91 + 0x53) ^ (i >> 5);
    }
    g_assert_cmpint(pwrite(fd, seed, sizeof(seed), 0), ==, sizeof(seed));
    close(fd);
    qts = start_file_disk(path, NULL);
    prepare_coupled_target(qts, 512);
    manual_command(qts, read_cdb, sizeof(read_cdb));
    manual_wait_phase(qts, 0x40);
    manual_counters(qts);
    for (unsigned i = 0; i < sizeof(seed); i++) {
        got[i] = manual_incoming(qts, 0x40, false);
    }
    g_assert_cmpmem(got, sizeof(got), seed, sizeof(seed));
    manual_check_counters(qts);
    manual_finish(qts, 0);
    manual_command(qts, write_cdb, sizeof(write_cdb));
    manual_wait_phase(qts, 0);
    manual_counters(qts);
    for (unsigned i = 0; i < sizeof(payload); i++) {
        ManualGuard before;

        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe3, ==, 2);
        qtest_writeb(qts, REG_BASE + 6, payload[i] ^ 0xff);
        qtest_writeb(qts, REG_BASE + 6, payload[i]);
        if (i == 4) {
            uint8_t idle[4];
            size_t n = 0;

            emit(idle, &n, branch_word(8, 0, 0x65, 0));
            start_program(qts, idle, n);
            qtest_writeb(qts, REG_BASE + 1, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 0);
            manual_capture(qts, &before);
            qtest_writeb(qts, REG_BASE + 6, payload[i] ^ 0x69);
            manual_unchanged(qts, &before);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
        }
        if (i == 5) {
            manual_capture(qts, &before);
            qtest_writeb(qts, REG_BASE + 1, 8);
            manual_unchanged(qts, &before);
            qtest_writeb(qts, REG_BASE + 1, 10);
            manual_unchanged(qts, &before);
            qtest_writeb(qts, REG_BASE + 0x93, 0x34);
            manual_unchanged(qts, &before);
            qtest_writeb(qts, REG_BASE + 1, 2);
            qtest_writeb(qts, REG_BASE + 3, 1);
            manual_unchanged(qts, &before);
            /* Channel clear releases the stage; supply it again. */
            qtest_writeb(qts, REG_BASE + 6, payload[i]);
        }
        if (i < 4) {
            if (i >= 2) {
                qtest_writeb(qts, REG_BASE + 0x0c, 1);
            }
            manual_signal_write(qts, 0x41, i & 1);
            if (i < 2) {
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 3, ==, 2);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11,
                                ==, 0x11);
                manual_signal_write(qts, 1, i & 1);
            }
        } else {
            qtest_writeb(qts, REG_BASE + 3, 1);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 3, ==, 1);
        manual_capture(qts, &before);
        if (i >= 4) {
            qtest_writeb(qts, REG_BASE + 6, payload[i] ^ 0x69);
            manual_unchanged(qts, &before);
        }
        qtest_writeb(qts, REG_BASE + 3, 1);
        manual_unchanged(qts, &before);
        if (i >= 4) {
            qtest_writeb(qts, REG_BASE + 1, 2);
            manual_unchanged(qts, &before);
        }
        qtest_writeb(qts, REG_BASE + 3, 0);
    }
    manual_finish(qts, 0);
    manual_check_counters(qts);
    qtest_quit(qts);
    fd = open(path, O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, got, sizeof(got), 512), ==, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), payload, sizeof(payload));
    g_assert_cmpint(pread(fd, got, sizeof(got), 0), ==, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), seed, sizeof(seed));
    close(fd);
    unlink(path);
}

static void test_manual_phase_ack_order(void)
{
    for (unsigned sequencer = 0; sequencer < 2; sequencer++) {
        for (unsigned cleared = 0; cleared < 2; cleared++) {
            QTestState *qts = manual_status();
            uint8_t byte = qtest_readb(qts, REG_BASE + 6);
            ManualGuard before;

            manual_counters(qts);
            if (cleared) {
                qtest_writeb(qts, REG_BASE + 0x0c, 1);
            }
            manual_signal_write(qts, 0xe1, sequencer);
            if (cleared) {
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe3,
                                ==, 0xc1);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, byte);
                manual_signal_write(qts, 0xe0, sequencer);
            } else {
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe3,
                                ==, 0xc2);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11,
                                ==, 0x11);
                manual_capture(qts, &before);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, byte);
                manual_unchanged(qts, &before);
                manual_signal_write(qts, 0xc1, sequencer);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe3,
                                ==, 0xc1);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11,
                                ==, 0);
                manual_signal_write(qts, 0xc0, sequencer);
            }
            g_assert_cmphex(manual_incoming(qts, 0xe0, sequencer), ==, 0);
            manual_check_counters(qts);
            qtest_quit(qts);
        }
    }
}

static void test_manual_interrupts(void)
{
    for (unsigned mismatch = 0; mismatch < 2; mismatch++) {
        QTestState *qts = manual_status();
        ManualGuard before;

        qtest_irq_intercept_in(qts, "ioapic");
        qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
        qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
        qtest_writeb(qts, REG_BASE + 0x87, 6);
        qtest_writeb(qts, REG_BASE + 0x10, 2);
        g_assert_false(qtest_get_irq(qts, 11));
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 2, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 2, ==, 2);
        if (mismatch) {
            qtest_writeb(qts, REG_BASE + 3, 0xe0);
        }
        qtest_writeb(qts, REG_BASE + 0x11, mismatch ? 0x10 : 1);
        g_assert_true(qtest_get_irq(qts, 11));
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 4, ==, 4);
        qtest_writeb(qts, REG_BASE + 0x92, 4);
        g_assert_true(qtest_get_irq(qts, 11));
        config_writew(qts, 4, 2);
        g_assert_false(qtest_get_irq(qts, 11));
        config_writew(qts, 4, 6);
        g_assert_true(qtest_get_irq(qts, 11));
        qtest_writeb(qts, REG_BASE + 0x87, 4);
        g_assert_false(qtest_get_irq(qts, 11));
        qtest_writeb(qts, REG_BASE + 0x87, 6);
        g_assert_true(qtest_get_irq(qts, 11));
        manual_capture(qts, &before);
        qtest_writeb(qts, REG_BASE + 1, 2);
        manual_unchanged(qts, &before);
        if (mismatch) {
            qtest_writeb(qts, REG_BASE + 3, 0xc0);
        } else {
            qtest_writeb(qts, REG_BASE + 0x0c, 1);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe3, ==, 0xc2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 4, ==, 4);
        qtest_writeb(qts, REG_BASE + 0x92, 4);
        g_assert_false(qtest_get_irq(qts, 11));
        qtest_writeb(qts, REG_BASE + 0x11, 0);
        manual_finish(qts, 2);
        qtest_quit(qts);
    }
}

static void test_manual_channel_counters(void)
{
    QTestState *qts = manual_status_fifo();
    ManualGuard before;

    manual_counters(qts);
    qtest_writeb(qts, REG_BASE + 3, 0xcc);
    qtest_writeb(qts, REG_BASE + 0x9b, 7);
    manual_capture(qts, &before);
    qtest_writeb(qts, REG_BASE + 1, 2);
    manual_unchanged(qts, &before);
    manual_check_counters(qts);
    qtest_writeb(qts, REG_BASE + 1, 0x10);
    g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x18008);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0x2345);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);
    manual_finish(qts, 0);
    manual_check_fifo(qts);
    qtest_quit(qts);

    qts = manual_status_fifo();
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
    manual_finish(qts, 0);
    qtest_quit(qts);
}

static void test_automatic_channel_clear(void)
{
    static const uint8_t phases[] = { 0x80, 0xa0, 0xc0, 0xe0 };

    for (unsigned kind = 0; kind < ARRAY_SIZE(phases); kind++) {
        for (unsigned indication = 0; indication < 4; indication++) {
            uint8_t phase = phases[kind];
            QTestState *qts;
            ManualGuard before;

            if (kind < 2) {
                qts = start_disk();
                attention_select(qts, kind ? 0x48 : 0x40);
                qtest_writeb(qts, REG_BASE + 0x92, 1);
            } else {
                qts = manual_status();
                if (kind == 3) {
                    g_assert_cmphex(manual_incoming(qts, 0xc0, false), ==, 2);
                }
                qtest_writeb(qts, REG_BASE + 1, 8);
            }
            qtest_writeb(qts, REG_BASE + 3, phase);
            if (indication == 1) {
                qtest_writeb(qts, REG_BASE + 0x0b, 2);
            } else if (indication == 2) {
                qtest_writeb(qts, REG_BASE + 0x0c, 1);
            } else if (indication == 3) {
                qtest_writeb(qts, REG_BASE + 3, phase ^ 0x20);
            }
            qtest_writeb(qts, REG_BASE + 0x11, 0x11);
            manual_counters(qts);
            manual_capture(qts, &before);
            qtest_writeb(qts, REG_BASE + 1, 10);
            manual_unchanged(qts, &before);
            if (kind >= 2) {
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x12),
                                ==, kind == 2 ? 2 : 0);
            }
            if (indication == 3) {
                if (kind >= 2) {
                    qtest_readb(qts, REG_BASE + 6);
                } else {
                    attention_write_raw(qts, 6, kind ? 0x80 : 0);
                    qtest_writeb(qts, REG_BASE + 0x92, 1);
                }
                manual_unchanged(qts, &before);
                qtest_writeb(qts, REG_BASE + 3, phase);
            }
            qtest_writeb(qts, REG_BASE + 0x11, 0);
            qtest_writeb(qts, REG_BASE + 0x92, 4);
            if (kind >= 2) {
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 6),
                                ==, kind == 2 ? 2 : 0);
                qtest_writeb(qts, REG_BASE + 1, 0);
                if (kind == 2) {
                    g_assert_cmphex(manual_incoming(qts, 0xe0, false), ==, 0);
                }
            } else {
                if (kind) {
                    attention_write(qts, 6, 0x80);
                    attention_write(qts, 0x0c, 0x40);
                    qtest_writeb(qts, REG_BASE + 3, 0x80);
                }
                for (unsigned i = 0; i < 6; i++) {
                    attention_write(qts, 6, 0);
                }
                qtest_writeb(qts, REG_BASE + 0x92, 1);
                qtest_writeb(qts, REG_BASE + 1, 0);
                manual_finish(qts, 2);
            }
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
            manual_check_counters(qts);
            qtest_quit(qts);
        }
    }
}

static void test_automatic_channel_mode_change(void)
{
    for (unsigned message = 0; message < 2; message++) {
        QTestState *qts = manual_status();
        uint8_t phase = message ? 0xe0 : 0xc0;
        uint8_t byte = message ? 0 : 2;

        if (message) {
            manual_incoming(qts, 0xc0, false);
        }
        manual_wait_phase(qts, phase);
        qtest_writeb(qts, REG_BASE + 1, 8);
        qtest_writeb(qts, REG_BASE + 0x0b, 2);
        qtest_writeb(qts, REG_BASE + 0x0c, 1);
        qtest_writeb(qts, REG_BASE + 1, 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 1), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 2, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x12), ==, byte);
        qtest_writeb(qts, REG_BASE + 1, 10);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 1), ==, 8);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 2, ==, 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x12), ==, byte);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, byte);
        qtest_writeb(qts, REG_BASE + 1, 0);
        if (!message) {
            manual_incoming(qts, 0xe0, false);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
        qtest_quit(qts);
    }
}

static void test_manual_refusals(void)
{
    static const uint8_t inquiry[] = { 0x12, 0, 0, 0, 36, 0 };
    QTestState *qts = start_selected_disk(NULL);
    ManualGuard before;
    uint8_t byte;

    manual_command(qts, inquiry, sizeof(inquiry));
    manual_wait_phase(qts, 0x40);
    manual_counters(qts);
    byte = qtest_readb(qts, REG_BASE + 0x12);
    manual_capture(qts, &before);
    qtest_writeb(qts, REG_BASE + 3, 0x41);
    manual_unchanged(qts, &before);
    qtest_writeb(qts, REG_BASE + 6, 0x93);
    manual_unchanged(qts, &before);
    qtest_writew(qts, REG_BASE + 6, 0x37c9);
    manual_unchanged(qts, &before);
    qtest_readw(qts, REG_BASE + 6);
    manual_unchanged(qts, &before);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x12), ==, byte);
    for (unsigned i = 0; i < 36; i++) {
        manual_incoming(qts, 0x40, false);
    }
    manual_wait_phase(qts, 0xc0);
    manual_capture(qts, &before);
    qtest_writeb(qts, REG_BASE + 6, 0xf1);
    manual_unchanged(qts, &before);
    manual_finish(qts, 0);
    manual_capture(qts, &before);
    qtest_writeb(qts, REG_BASE + 6, 0x55);
    qtest_readb(qts, REG_BASE + 6);
    qtest_writeb(qts, REG_BASE + 3, 1);
    manual_unchanged(qts, &before);
    manual_check_counters(qts);
    qtest_quit(qts);
}

static void test_manual_rate_refusals(void)
{
    static const uint8_t inquiry[] = { 0x12, 0, 0, 0, 36, 0 };
    static const uint8_t rates[] = { 1, 0x80 };

    for (unsigned r = 0; r < ARRAY_SIZE(rates); r++) {
        QTestState *qts = start_selected_disk(NULL);
        ManualGuard before;

        manual_command(qts, inquiry, sizeof(inquiry));
        manual_wait_phase(qts, 0x40);
        qtest_writeb(qts, REG_BASE + 4, rates[r]);
        manual_counters(qts);
        manual_capture(qts, &before);
        qtest_readb(qts, REG_BASE + 6);
        qtest_writeb(qts, REG_BASE + 3, 0x41);
        manual_unchanged(qts, &before);
        qtest_writeb(qts, REG_BASE + 4, 0);
        for (unsigned i = 0; i < 36; i++) {
            manual_incoming(qts, 0x40, false);
        }
        manual_finish(qts, 0);
        manual_check_counters(qts);
        qtest_quit(qts);
    }
}

static void test_manual_unpaused_host(void)
{
    for (unsigned operation = 0; operation < 3; operation++) {
        QTestState *qts = manual_status();
        uint8_t program[4];
        ManualGuard before;
        size_t n = 0;

        manual_counters(qts);
        emit(program, &n, branch_word(8, 0, 0x65, 0));
        start_program(qts, program, n);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 0);
        qtest_writeb(qts, REG_BASE + 1, 0x80);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 1), ==, 0x80);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe3, ==, 0xc2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 2, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 0);
        manual_capture(qts, &before);
        if (operation == 0) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, 0xff);
        } else if (operation == 1) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x12), ==, 0xff);
        } else {
            qtest_writeb(qts, REG_BASE + 3, 0xc8);
        }
        manual_unchanged(qts, &before);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
        manual_finish(qts, 2);
        manual_check_counters(qts);
        qtest_quit(qts);
    }
}

static void test_manual_target_delay(void)
{
    QTestState *qts = start_peer_data_in(false, false, false);
    ManualGuard before;

    qtest_writeb(qts, REG_BASE + 0x87, 6);
    qtest_writeb(qts, REG_BASE + 0x93, 0);
    qtest_writeb(qts, REG_BASE + 1, 0);
    qtest_writeb(qts, REG_BASE + 3, 0x40);
    manual_counters(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe3, ==, 0x40);
    manual_capture(qts, &before);
    qtest_readb(qts, REG_BASE + 6);
    qtest_writeb(qts, REG_BASE + 3, 0x41);
    qtest_clock_step(qts, 1000000);
    manual_unchanged(qts, &before);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 8, ==, 0);
    qtest_writeb(qts, REG_BASE + 1, 2);
    manual_unchanged(qts, &before);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_memread(qts, 0x18000, before.ram, sizeof(before.ram));
    for (unsigned i = 0; i < sizeof(before.ram); i++) {
        g_assert_cmphex(before.ram[i], ==, 0xa7);
    }
    qtest_quit(qts);
}

static void test_manual_resets(void)
{
    for (unsigned reset = 0; reset < 3; reset++) {
        for (unsigned high = 0; high < 2; high++) {
            QTestState *qts = manual_status();
            ManualGuard before;

            manual_counters(qts);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, 2);
            qtest_writeb(qts, REG_BASE + 3, high ? 0xcd : 0xcc);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 1, ==, high);
            manual_capture(qts, &before);
            qtest_writeb(qts, REG_BASE + 0x87, 6);
            manual_unchanged(qts, &before);
            if (reset == 0) {
                qtest_writeb(qts, REG_BASE + 0x87, 1);
            } else if (reset == 1) {
                qtest_system_reset(qts);
                config_writel(qts, 0x14, REG_BASE);
                config_writew(qts, 4, 6);
            } else {
                qtest_writeb(qts, REG_BASE + 0, 1);
                qtest_writeb(qts, REG_BASE + 0, 0);
                g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4),
                                ==, 0x18008);
                g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3),
                                ==, 0x2345);
                g_assert_cmphex(config_readl(qts, 0x14) & ~15, ==, REG_BASE);
            }
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
            manual_capture(qts, &before);
            qtest_writeb(qts, REG_BASE + 3, 1);
            qtest_readb(qts, REG_BASE + 6);
            manual_unchanged(qts, &before);
            qtest_quit(qts);
        }
    }
}

static void test_manual_pending_migration(void)
{
    for (unsigned message = 0; message < 2; message++) {
        for (unsigned sampled = 0; sampled < 2; sampled++) {
            g_autofree char *dir =
                g_dir_make_tmp("aic7880-manual-XXXXXX", NULL);
            g_autofree char *socket =
                g_build_filename(dir, "migrate.sock", NULL);
            g_autofree char *uri = g_strdup_printf("unix:%s", socket);
            QTestState *from = manual_status();
            QTestState *to = start_disk_mode("-S -incoming defer");
            uint8_t phase = message ? 0xe0 : 0xc0;
            uint8_t byte = message ? 0 : 2;
            ManualGuard before;

            if (message) {
                manual_incoming(from, 0xc0, false);
            }
            manual_wait_phase(from, phase);
            manual_counters(from);
            if (sampled) {
                g_assert_cmphex(qtest_readb(from, REG_BASE + 6), ==, byte);
            }
            qtest_writeb(from, REG_BASE + 3, phase ^ 0x20);
            qtest_writeb(from, REG_BASE + 0x11, 0x11);
            manual_capture(from, &before);
            qtest_qmp_assert_success(to,
                "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
            sgi_migrate_stopped(from, uri);
            sgi_wait_migration(to, "completed");
            manual_unchanged(to, &before);
            g_assert_cmphex(qtest_readb(to, REG_BASE + 0x12), ==, byte);
            qtest_writeb(to, REG_BASE + 0x11, 0);
            qtest_writeb(to, REG_BASE + 0x92, 4);
            qtest_writeb(to, REG_BASE + 3, phase);
            if (!sampled) {
                manual_capture(to, &before);
                qtest_writeb(to, REG_BASE + 3, phase | 1);
                manual_unchanged(to, &before);
                g_assert_cmphex(qtest_readb(to, REG_BASE + 6), ==, byte);
            }
            manual_ack(to, phase, byte, true, false);
            if (!message) {
                manual_incoming(to, 0xe0, false);
            }
            manual_check_counters(to);
            g_assert_cmphex(qtest_readb(to, REG_BASE + 3), ==, 0);
            qtest_quit(to);
            qtest_quit(from);
            g_unlink(socket);
            g_rmdir(dir);
        }
    }
}

static void test_manual_host_command_refused(void)
{
    for (unsigned automatic = 0; automatic < 2; automatic++) {
        QTestState *qts = start_disk();
        ManualGuard before;

        attention_select(qts, 0x40);
        qtest_writeb(qts, REG_BASE + 1, automatic ? 8 : 0);
        manual_capture(qts, &before);
        for (unsigned i = 0; i < 6; i++) {
            qtest_writeb(qts, REG_BASE + 6, 0);
        }
        manual_unchanged(qts, &before);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe0, ==, 0x80);
        qtest_quit(qts);
    }
}

static void test_manual_dma_owned_refused(void)
{
    QTestState *qts = start_pending_fifo(0x10000);
    ManualGuard before;

    qtest_writeb(qts, REG_BASE + 1, 0);
    manual_capture(qts, &before);
    qtest_readb(qts, REG_BASE + 6);
    qtest_writeb(qts, REG_BASE + 6, 0x71);
    qtest_writeb(qts, REG_BASE + 3, 0x41);
    manual_unchanged(qts, &before);
    qtest_quit(qts);
}

static void test_manual_rate_before_callback(void)
{
    static const uint8_t inquiry[] = { 0x12, 0, 0, 0, 36, 0 };
    static const uint8_t rates[] = { 1, 0x80 };

    for (unsigned r = 0; r < ARRAY_SIZE(rates); r++) {
        QTestState *qts = start_selected_disk(NULL);
        ManualGuard before;
        uint8_t got[36];

        qtest_writeb(qts, REG_BASE + 4, rates[r]);
        manual_command(qts, inquiry, sizeof(inquiry));
        manual_wait_phase(qts, 0x40);
        manual_counters(qts);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 4), ==, rates[r]);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe3, ==, 0x42);
        manual_capture(qts, &before);
        qtest_readb(qts, REG_BASE + 6);
        qtest_writeb(qts, REG_BASE + 3, 0x41);
        manual_unchanged(qts, &before);
        qtest_writeb(qts, REG_BASE + 4, 0);
        for (unsigned i = 0; i < sizeof(got); i++) {
            got[i] = manual_incoming(qts, 0x40, false);
        }
        g_assert_cmpmem(got, sizeof(got), manual_fifo_payload,
                        sizeof(manual_fifo_payload));
        manual_finish(qts, 0);
        manual_check_counters(qts);
        qtest_quit(qts);
    }
}

static void test_manual_unrelated_interrupt(void)
{
    static const uint8_t cdb[6] = { 0 };
    QTestState *qts = start_disk();

    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    qtest_writeb(qts, REG_BASE + 2, 4);
    start_missing_selection(qts);
    qtest_clock_step(qts, 256000400);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0x80);
    manual_command(qts, cdb, sizeof(cdb));
    manual_wait_phase(qts, 0xc0);
    qtest_writeb(qts, REG_BASE + 0x11, 0x81);
    g_assert_true(qtest_get_irq(qts, 11));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, 2);
    qtest_writeb(qts, REG_BASE + 3, 0xc1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x91, ==, 0x80);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0x92, 4);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0x0c, 0x80);
    qtest_writeb(qts, REG_BASE + 0x92, 4);
    g_assert_false(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 3, 0xc0);
    g_assert_cmphex(manual_incoming(qts, 0xe0, false), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x92, 4);
    g_assert_false(qtest_get_irq(qts, 11));
    qtest_quit(qts);
}

static void test_manual_disconnected_refused(void)
{
    uint8_t program[64];
    QTestState *qts = start_peer_disk();
    ManualGuard before;

    start_program(qts, program, build_selected_inquiry_identify(program, 0xc0));
    qtest_writeb(qts, REG_BASE + 0x93, 0);
    qtest_writeb(qts, REG_BASE + 1, 0);
    qtest_writeb(qts, REG_BASE + 0x92, 1);
    manual_wait_phase(qts, 0x40);
    peer_event(qts, "disconnect:0:0");
    g_assert_cmphex(manual_incoming(qts, 0xe0, false), ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
    manual_counters(qts);
    manual_capture(qts, &before);
    qtest_readb(qts, REG_BASE + 6);
    qtest_writeb(qts, REG_BASE + 6, 0x63);
    qtest_writeb(qts, REG_BASE + 3, 1);
    manual_unchanged(qts, &before);
    qtest_quit(qts);
}

static void test_manual_nonempty_fifo_refused(void)
{
    QTestState *qts = start_pending_fifo(0x10000);
    ManualGuard before;

    qtest_writeb(qts, REG_BASE + 0x93, 0);
    qtest_writeb(qts, REG_BASE + 1, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 0);
    manual_capture(qts, &before);
    qtest_readb(qts, REG_BASE + 6);
    qtest_writeb(qts, REG_BASE + 3, 0x41);
    manual_unchanged(qts, &before);
    qtest_quit(qts);
}

static void test_manual_selection_bus_free(void)
{
    QTestState *qts = start_disk();

    qtest_writeb(qts, REG_BASE + 2, 4);
    start_missing_selection(qts);
    qtest_writeb(qts, REG_BASE + 3, 0x0c);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0x0c, ==, 0x0c);
    qtest_clock_step(qts, 256000400);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x88, ==, 0x88);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0x1f, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    qtest_quit(qts);
}

static void test_manual_attention_atomic(void)
{
    QTestState *qts = manual_status();
    ManualGuard before;

    manual_counters(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, 2);
    qtest_writeb(qts, REG_BASE + 0x11, 0x11);
    manual_capture(qts, &before);
    /* Forbidden attention cannot commit an otherwise legal STATUS ACK. */
    qtest_writeb(qts, REG_BASE + 3, 0xdd);
    manual_unchanged(qts, &before);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x12), ==, 2);
    manual_ack(qts, 0xc0, 2, true, false);
    g_assert_cmphex(manual_incoming(qts, 0xe0, false), ==, 0);
    manual_check_counters(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
    qtest_quit(qts);
}

static void manual_attention_irq(QTestState *qts)
{
    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    qtest_writeb(qts, REG_BASE + 0x87, 6);
    qtest_writeb(qts, REG_BASE + 0x11, 0x11);
}

static void manual_attention_signal(QTestState *qts, uint8_t value,
                                    bool sequencer)
{
    manual_signal_write(qts, value, sequencer);
    if (sequencer) {
        /* The refused first instruction ran; its following SEQINT did not. */
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x63), ==, 0);
    }
}

static void manual_attention_byte(QTestState *qts, uint8_t phase, uint8_t byte,
                                 unsigned variant, bool sequencer)
{
    uint8_t expected = variant ? 0x80 : phase;
    uint8_t word = expected | 0x1d;
    ManualGuard before;
    bool irq;

    if (variant == 2) {
        qtest_writeb(qts, REG_BASE + 0x0c, 1);
    }
    manual_attention_irq(qts);
    manual_capture(qts, &before);
    irq = qtest_get_irq(qts, 11);
    manual_attention_signal(qts, word, sequencer);
    manual_unchanged(qts, &before);
    g_assert_cmpint(qtest_get_irq(qts, 11), ==, irq);
    if (phase) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x12), ==, byte);
    }

    /* A refused sequencer instruction stops it; the paused host can recover. */
    qtest_writeb(qts, REG_BASE + 3, word & ~0x10);
    if (variant == 1) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xf3, ==, phase | 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 0x11);
        if (phase) {
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, byte);
        }
        qtest_writeb(qts, REG_BASE + 3, phase | 0x0d);
    }
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, phase | 0x0d);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x11, ==, 0);
    if (phase) {
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, byte);
    }
    qtest_writeb(qts, REG_BASE + 3, phase | 0x0d);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe3, ==, phase | 1);
    qtest_writeb(qts, REG_BASE + 3, phase);
    qtest_writeb(qts, REG_BASE + 0x11, 0);
    qtest_writeb(qts, REG_BASE + 0x92, 4);
    manual_check_counters(qts);
}

static void test_manual_attention_precommand(void)
{
    for (unsigned message = 0; message < 2; message++) {
        for (unsigned sequencer = 0; sequencer < 2; sequencer++) {
            QTestState *qts = start_disk();
            ManualGuard before;

            attention_select(qts, message ? 0x48 : 0x40);
            qtest_writeb(qts, REG_BASE + 0x92, 1);
            if (message) {
                qtest_writeb(qts, REG_BASE + 0x0c, 0x40);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xf0,
                                ==, 0xa0);
            }
            manual_counters(qts);
            manual_signal_write(qts, 0xb0, sequencer);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xf0, ==, 0xb0);
            manual_capture(qts, &before);
            manual_signal_write(qts, 0xb0, sequencer);
            manual_unchanged(qts, &before);
            manual_signal_write(qts, 0xa0, sequencer);
            manual_unchanged(qts, &before);
            if (message) {
                qtest_writeb(qts, REG_BASE + 6, 0x80);
            } else {
                static const uint8_t negotiation[] = { 1, 2, 3, 1 };

                for (unsigned i = 0; i < sizeof(negotiation); i++) {
                    qtest_writeb(qts, REG_BASE + 6, negotiation[i]);
                }
            }
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xf0, ==, 0xb0);
            manual_capture(qts, &before);
            /* A retained one cannot restart an already complete message. */
            manual_signal_write(qts, 0xb0, sequencer);
            manual_unchanged(qts, &before);
            manual_signal_write(qts, 0xa0, sequencer);
            manual_unchanged(qts, &before);
            qtest_writeb(qts, REG_BASE + 0x0c, 0x40);
            if (!message) {
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xf0,
                                ==, 0xe0);
                qtest_writeb(qts, REG_BASE + 3, 0xe0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x12), ==, 7);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, 7);
            }
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xf0, ==, 0x80);
            for (unsigned i = 0; i < 6; i++) {
                attention_write(qts, 6, 0);
            }
            qtest_writeb(qts, REG_BASE + 0x92, 1);
            qtest_writeb(qts, REG_BASE + 1, 0);
            manual_finish(qts, 2);
            manual_check_counters(qts);
            qtest_quit(qts);
        }
    }
}

static void test_manual_attention_incoming(void)
{
    static const uint8_t inquiry[] = { 0x12, 0, 0, 0, 36, 0 };
    static const uint8_t phases[] = { 0x40, 0xc0, 0xe0 };

    for (unsigned kind = 0; kind < ARRAY_SIZE(phases); kind++) {
        for (unsigned variant = 0; variant < 3; variant++) {
            for (unsigned sequencer = 0; sequencer < 2; sequencer++) {
                QTestState *qts;
                uint8_t phase = phases[kind];
                uint8_t byte;

                g_test_message("phase=0x%02x variant=%u sequencer=%u",
                               phase, variant, sequencer);
                if (kind == 0) {
                    qts = start_selected_disk(NULL);
                    manual_command(qts, inquiry, sizeof(inquiry));
                } else if (kind == 1) {
                    qts = manual_status_fifo();
                } else {
                    qts = manual_status();
                    g_assert_cmphex(manual_incoming(qts, 0xc0, false), ==, 2);
                }
                manual_wait_phase(qts, phase);
                manual_counters(qts);
                byte = qtest_readb(qts, REG_BASE + 6);
                manual_attention_byte(qts, phase, byte, variant, sequencer);
                if (kind == 0) {
                    g_assert_cmphex(byte, ==, manual_fifo_payload[0]);
                    for (unsigned i = 1; i < sizeof(manual_fifo_payload); i++) {
                        g_assert_cmphex(manual_incoming(qts, 0x40, false),
                                        ==, manual_fifo_payload[i]);
                    }
                    manual_finish(qts, 0);
                } else if (kind == 1) {
                    g_assert_cmphex(byte, ==, 0);
                    g_assert_cmphex(manual_incoming(qts, 0xe0, false), ==, 0);
                    manual_check_fifo(qts);
                } else {
                    g_assert_cmphex(byte, ==, 0);
                }
                manual_check_counters(qts);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
                qtest_quit(qts);
            }
        }
    }
}

static void test_manual_attention_outgoing(void)
{
    static const uint8_t cdb[10] = { 0x2a, 0, 0, 0, 0, 1, 0, 0, 1, 0 };

    for (unsigned variant = 0; variant < 3; variant++) {
        for (unsigned sequencer = 0; sequencer < 2; sequencer++) {
            g_autofree char *path = NULL;
            uint8_t payload[512], got[512];
            QTestState *qts;
            int fd = g_file_open_tmp("aic7880-attention-XXXXXX", &path, NULL);

            g_test_message("DATA OUT variant=%u sequencer=%u",
                           variant, sequencer);
            g_assert_cmpint(fd, >=, 0);
            g_assert_cmpint(ftruncate(fd, 4096), ==, 0);
            close(fd);
            qts = start_file_disk(path, NULL);
            prepare_coupled_target(qts, 512);
            manual_command(qts, cdb, sizeof(cdb));
            manual_wait_phase(qts, 0);
            manual_counters(qts);
            for (unsigned i = 0; i < sizeof(payload); i++) {
                payload[i] = (i * 61 + 0x37 + variant) ^ (i >> 4);
            }
            qtest_writeb(qts, REG_BASE + 6, payload[0]);
            manual_attention_byte(qts, 0, payload[0], variant, sequencer);
            for (unsigned i = 1; i < sizeof(payload); i++) {
                qtest_writeb(qts, REG_BASE + 6, payload[i]);
                manual_ack(qts, 0, payload[i], false, false);
            }
            manual_finish(qts, 0);
            manual_check_counters(qts);
            qtest_quit(qts);
            fd = open(path, O_RDONLY);
            g_assert_cmpint(fd, >=, 0);
            g_assert_cmpint(pread(fd, got, sizeof(got), 512), ==, sizeof(got));
            g_assert_cmpmem(got, sizeof(got), payload, sizeof(payload));
            close(fd);
            g_unlink(path);
        }
    }
}

static void test_manual_attention_late_cdb(void)
{
    for (unsigned sequencer = 0; sequencer < 2; sequencer++) {
        QTestState *qts = start_disk();
        ManualGuard before;
        bool irq;

        attention_select(qts, 0x40);
        attention_write(qts, 6, 0);
        qtest_writeb(qts, REG_BASE + 0x92, 1);
        manual_counters(qts);
        manual_attention_irq(qts);
        manual_capture(qts, &before);
        irq = qtest_get_irq(qts, 11);
        /* Message Out phase bits cannot admit attention after CDB byte 0. */
        manual_attention_signal(qts, 0xbc, sequencer);
        manual_unchanged(qts, &before);
        g_assert_cmpint(qtest_get_irq(qts, 11), ==, irq);
        manual_check_counters(qts);
        qtest_quit(qts);
    }
}

static void test_manual_attention_disconnected(void)
{
    for (unsigned ack = 0; ack < 2; ack++) {
        for (unsigned sequencer = 0; sequencer < 2; sequencer++) {
            uint8_t program[64];
            QTestState *qts = start_peer_disk();
            ManualGuard before;
            bool irq;

            start_program(qts, program,
                          build_selected_inquiry_identify(program, 0xc0));
            qtest_writeb(qts, REG_BASE + 0x93, 0);
            qtest_writeb(qts, REG_BASE + 1, 0);
            qtest_writeb(qts, REG_BASE + 0x92, 1);
            manual_wait_phase(qts, 0x40);
            peer_event(qts, "disconnect:0:0");
            g_assert_cmphex(manual_incoming(qts, 0xe0, false), ==, 4);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
            manual_counters(qts);
            manual_attention_irq(qts);
            manual_capture(qts, &before);
            irq = qtest_get_irq(qts, 11);
            manual_attention_signal(qts, 0xbc | ack, sequencer);
            manual_unchanged(qts, &before);
            g_assert_cmpint(qtest_get_irq(qts, 11), ==, irq);
            manual_check_counters(qts);
            qtest_quit(qts);
        }
    }
}

static void test_manual_attention_ack_refused(void)
{
    for (unsigned message = 0; message < 2; message++) {
        for (unsigned sequencer = 0; sequencer < 2; sequencer++) {
            QTestState *qts = start_disk();
            ManualGuard before;
            bool irq;

            attention_select(qts, message ? 0x48 : 0x40);
            qtest_writeb(qts, REG_BASE + 0x92, 1);
            if (message) {
                qtest_writeb(qts, REG_BASE + 0x0c, 0x40);
            }
            qtest_writeb(qts, REG_BASE + 3, message ? 0xa0 : 0x80);
            qtest_writeb(qts, REG_BASE + 1, 0);
            manual_counters(qts);
            manual_attention_irq(qts);
            manual_capture(qts, &before);
            irq = qtest_get_irq(qts, 11);
            /* New attention is admissible, but manual ACK is not. */
            manual_attention_signal(qts, 0x5d, sequencer);
            manual_unchanged(qts, &before);
            g_assert_cmpint(qtest_get_irq(qts, 11), ==, irq);
            qtest_writeb(qts, REG_BASE + 3, 0xb0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xf0, ==, 0xb0);
            manual_check_counters(qts);
            qtest_quit(qts);
        }
    }
}

static void test_manual_attention_resets(void)
{
    for (unsigned reset = 0; reset < 3; reset++) {
        for (unsigned sequencer = 0; sequencer < 2; sequencer++) {
            QTestState *qts = start_disk();
            ManualGuard before;

            attention_select(qts, 0x40);
            qtest_writeb(qts, REG_BASE + 0x92, 1);
            manual_signal_write(qts, 0xbc, sequencer);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xfc, ==, 0xbc);
            manual_counters(qts);
            manual_capture(qts, &before);
            qtest_writeb(qts, REG_BASE + 1, 10);
            manual_unchanged(qts, &before);
            qtest_writeb(qts, REG_BASE + 0x87, 6);
            manual_unchanged(qts, &before);
            if (reset == 0) {
                qtest_writeb(qts, REG_BASE + 0x87, 1);
            } else if (reset == 1) {
                qtest_system_reset(qts);
                config_writel(qts, 0x14, REG_BASE);
                config_writew(qts, 4, 6);
            } else {
                qtest_writeb(qts, REG_BASE, 1);
                qtest_writeb(qts, REG_BASE, 0);
            }
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
            qtest_memread(qts, 0x18000, before.ram, sizeof(before.ram));
            for (unsigned i = 0; i < sizeof(before.ram); i++) {
                g_assert_cmphex(before.ram[i], ==, 0xa7);
            }
            qtest_quit(qts);
        }
    }
}

static QTestState *start_wide_targets(uint16_t mask, const char *extra)
{
    g_autoptr(GString) options = g_string_new(extra);

    for (unsigned id = 0; id < 16; id++) {
        if (mask & (1u << id)) {
            g_string_append_printf(options,
                " -drive if=none,id=d%u,file=null-co://,format=raw "
                "-device scsi-hd,drive=d%u,bus=hba.0,scsi-id=%u,lun=0,"
                "product=SYNTHETIC-ID%02u", id, id, id, id);
        }
    }
    QTestState *qts = qtest_initf(
        "-M pc -m 128M -nodefaults "
        "-device aic7880,id=hba,bus=pci.0,addr=04.0 %s", options->str);

    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 4, 6);
    return qts;
}

static void check_selection_identity(QTestState *qts, const char *expected)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{"
        "'path':'/machine/peripheral/hba','property':'x-test-selection'}}");

    g_assert_true(qdict_haskey(reply, "return"));
    g_assert_cmpstr(qdict_get_str(reply, "return"), ==, expected);
}

static int64_t wide_select(QTestState *qts, unsigned target, unsigned own,
                           bool attention)
{
    uint8_t program[20];
    size_t n = 0;

    qtest_writeb(qts, REG_BASE, 0);
    qtest_writeb(qts, REG_BASE + 5, target << 4 | own);
    emit_or(program, &n, 8, 1);
    emit_or(program, &n, attention ? 0xa0 : 0x80, 3);
    emit_or(program, &n, attention ? 0x48 : 0x40, 0);
    emit_or(program, &n, 1, 0x91);
    int64_t epoch = start_program(qts, program, n);

    qtest_writeb(qts, REG_BASE + 0x92, 1);
    return epoch;
}

static uint8_t wide_command(QTestState *qts, unsigned target, unsigned own,
                            unsigned lun, bool identify, const uint8_t *cdb,
                            size_t cdb_len, uint32_t count, bool write,
                            int reprogram)
{
    uint8_t program[256];
    g_autofree char *identity = g_strdup_printf("%u:%u:%u:%u", target, own, lun,
                         !!(qtest_readb(qts, REG_BASE + 0x1f) & 2));
    size_t n = 0;
    uint8_t status;

    qtest_writeb(qts, REG_BASE + 0x93, 0);
    host_dma_program(qts, 0x20008, count);
    for (unsigned byte = 0; byte < 3; byte++) {
        qtest_writeb(qts, REG_BASE + 8 + byte, count >> (byte * 8));
    }
    wide_select(qts, target, own, identify);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x50, ==, 0x40);
    if (identify) {
        attention_write(qts, 6, 0xc0 | lun);
        attention_write(qts, 0x0c, 0x40);
    }
    check_selection_identity(qts, identity);
    if (reprogram >= 0) {
        qtest_writeb(qts, REG_BASE + 5, reprogram);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 5), ==, reprogram);
        check_selection_identity(qts, identity);
    }
    emit_cdb_completion(program, &n, cdb, cdb_len, write, true);
    start_program(qts, program, n);
    coupled_wait(qts, 0x91, 2);
    status = qtest_readb(qts, REG_BASE + 0x22);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x23), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80,
                                ==, 0);
    check_selection_identity(qts, "none");
    qtest_writeb(qts, REG_BASE + 0x87, 4);
    qtest_writeb(qts, REG_BASE + 0x92, 2);
    return status;
}

static void test_wide_target_routing(void)
{
    static const uint8_t inquiry[] = { 0x12, 0, 0, 0, 36, 0 };
    QTestState *qts = start_wide_targets(0xffff, "");
    uint8_t guard[52], got[52];

    memset(guard, 0xa5, sizeof(guard));
    for (unsigned id = 0; id < 16; id++) {
        g_autofree char *product = g_strdup_printf("SYNTHETIC-ID%02u", id);
        unsigned own = id == 7 ? 6 : 7;

        qtest_memwrite(qts, 0x20000, guard, sizeof(guard));
        g_assert_cmphex(wide_command(qts, id, own, 0, id & 1, inquiry,
                                     sizeof(inquiry), 36, false, -1), ==, 0);
        qtest_memread(qts, 0x20000, got, sizeof(got));
        g_assert_cmpmem(got, 8, guard, 8);
        g_assert_cmpmem(got + 44, 8, guard, 8);
        g_assert_cmpmem(got + 24, strlen(product), product, strlen(product));
        qtest_clock_step(qts, 1000000000);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80,
                                ==, 0);
    }
    qtest_quit(qts);
}

static void test_wide_own_id(void)
{
    QTestState *qts = start_wide_targets(0xffff, "");

    for (unsigned own = 0; own < 16; own++) {
        qtest_writeb(qts, REG_BASE + 0x87, 1);
        qtest_writeb(qts, REG_BASE + 2, 4);
        wide_select(qts, own, own, false);
        qtest_clock_step(qts, 1000000000);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 3);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        check_selection_identity(qts, "none");
    }
    qtest_quit(qts);
}

static void test_wide_registers(void)
{
    QTestState *qts = start_aic();

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f), ==, 0xc2);
    for (unsigned value = 0; value < 256; value++) {
        qtest_writeb(qts, REG_BASE + 5, value);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 5), ==, value);
        attention_write(qts, 5, value);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 5), ==, value);
    }
    for (unsigned value = 0; value < 256; value++) {
        if (value & 0x15) {
            continue;
        }
        qtest_writeb(qts, REG_BASE + 0x1f, value);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f), ==, value & 0xe2);
        attention_write(qts, 0x1f, value);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f), ==, value & 0xe2);
    }
    qtest_quit(qts);
}

static void check_wide_pending(QTestState *qts)
{
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xee, ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x50, ==, 0x10);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x88, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
}

static void test_wide_selection_intervals(void)
{
    static const unsigned ids[] = { 0, 7, 8, 15 };

    for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
        unsigned id = ids[i];
        unsigned own = id == 7 ? 6 : 7;

        for (unsigned period = 0; period < 4; period++) {
            for (unsigned masked = 0; masked < 2; masked++) {
                QTestState *qts = start_wide_targets(0x81 & ~(1u << id), "");
                int64_t interval = 256000000LL >> period;
                uint8_t guard[32], got[32];
                g_autofree char *identity =
                    g_strdup_printf("%u:%u:0:1", id, own);

                memset(guard, 0xa7, sizeof(guard));
                qtest_memwrite(qts, 0x20000, guard, sizeof(guard));
                host_dma_program(qts, 0x20008, 16);
                qtest_irq_intercept_in(qts, "ioapic");
                qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
                qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
                qtest_writeb(qts, REG_BASE + 2, 4 | period << 3);
                qtest_writeb(qts, REG_BASE + 0x11, masked ? 0 : 0x88);
                /* Prospective wide DATA must not reject an absent target. */
                qtest_writeb(qts, REG_BASE + 4, 0x80);
                int64_t epoch = wide_select(qts, id, own, false);
                check_wide_pending(qts);
                check_selection_identity(qts, identity);
                qtest_clock_set(qts, epoch + interval - 1);
                check_wide_pending(qts);
                qtest_writeb(qts, REG_BASE + 0x87, 2);
                qtest_clock_step(qts, 1);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0x0c, ==, 0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x88,
                                ==, 0x80);
                g_assert_cmpint(qtest_get_irq(qts, 11), ==, !masked);
                qtest_clock_step(qts, 399);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 8, ==, 0);
                qtest_clock_step(qts, 1);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x88,
                                ==, 0x88);
                check_selection_identity(qts, "none");
                qtest_writeb(qts, REG_BASE + 0x11, 0x88);
                g_assert_true(qtest_get_irq(qts, 11));
                qtest_writeb(qts, REG_BASE + 0x92, 4);
                g_assert_true(qtest_get_irq(qts, 11));
                qtest_writeb(qts, REG_BASE + 0x0c, 0x80);
                qtest_writeb(qts, REG_BASE + 0x92, 4);
                g_assert_true(qtest_get_irq(qts, 11));
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x88,
                                ==, 8);
                qtest_writeb(qts, REG_BASE + 0x0c, 8);
                qtest_writeb(qts, REG_BASE + 0x92, 4);
                g_assert_false(qtest_get_irq(qts, 11));
                qtest_writeb(qts, REG_BASE + 0x0b, 0x10);
                qtest_clock_step(qts, interval);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x88,
                                ==, 0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x50,
                                ==, 0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
                qtest_memread(qts, 0x20000, got, sizeof(got));
                g_assert_cmpmem(got, sizeof(got), guard, sizeof(guard));
                qtest_quit(qts);
            }
        }
    }
}

static void test_wide_timer_controls(void)
{
    static const unsigned ids[] = { 8, 15 };

    for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
        QTestState *qts = start_wide_targets(0x81, "");
        g_autofree char *identity = g_strdup_printf("%u:7:0:1", ids[i]);

        wide_select(qts, ids[i], 7, false);
        qtest_clock_step(qts, 1000000000);
        check_wide_pending(qts);
        qtest_writeb(qts, REG_BASE + 2, 4);
        qtest_clock_step(qts, 64000000);
        qtest_writeb(qts, REG_BASE + 2, 4);
        qtest_writeb(qts, REG_BASE + 5, 0x10);
        qtest_writeb(qts, REG_BASE + 1, 10);
        check_selection_identity(qts, identity);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 5), ==, 0x10);
        qtest_clock_step(qts, 191999999);
        check_wide_pending(qts);
        qtest_clock_step(qts, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0x80);
        qtest_quit(qts);

        qts = start_wide_targets(0x81, "");
        qtest_writeb(qts, REG_BASE + 2, 4);
        wide_select(qts, ids[i], 7, false);
        qtest_clock_step(qts, 200000000);
        qtest_writeb(qts, REG_BASE + 2, 0x0c);
        qtest_clock_step(qts, 127999999);
        check_wide_pending(qts);
        qtest_writeb(qts, REG_BASE + 2, 8);
        qtest_clock_step(qts, 1000000000);
        check_wide_pending(qts);
        qtest_writeb(qts, REG_BASE + 2, 0x0c);
        qtest_clock_step(qts, 127999999);
        check_wide_pending(qts);
        qtest_clock_step(qts, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80, ==, 0x80);
        qtest_quit(qts);
    }
}

static void test_wide_connection(void)
{
    static const uint8_t inquiry[] = { 0x12, 0, 0, 0, 36, 0 };
    static const unsigned ids[] = { 8, 15 };

    for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
        QTestState *qts = start_wide_targets(0x8181, "");
        g_autofree char *identity = g_strdup_printf("%u:7:0:0", ids[i]);

        qtest_writeb(qts, REG_BASE + 0x1f, 0xe8);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f), ==, 0xe0);
        qtest_writeb(qts, REG_BASE + 2, 0x1c);
        int64_t epoch = wide_select(qts, ids[i], 7, false);

        check_wide_pending(qts);
        check_selection_identity(qts, identity);
        qtest_clock_set(qts, epoch + 31999999);
        check_wide_pending(qts);
        qtest_clock_step(qts, 401);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x88, ==, 0x88);
        check_selection_identity(qts, "none");
        qtest_writeb(qts, REG_BASE + 0x0c, 0x88);
        qtest_writeb(qts, REG_BASE + 0x0b, 0x10);
        qtest_writeb(qts, REG_BASE + 0x1f, 0xc2);
        g_assert_cmphex(wide_command(qts, ids[i], 7, 0, false, inquiry,
                                     sizeof(inquiry), 36, false, -1), ==, 0);
        qtest_clock_step(qts, 1000000000);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80,
                                ==, 0);
        qtest_quit(qts);

        for (unsigned present = 0; present < 2; present++) {
            qts = start_wide_targets(present ? 0x8181 : 0x81, "");
            wide_select(qts, ids[i], 7, false);
            qtest_writeb(qts, REG_BASE + 0x1f, 0x20);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f), ==, 0xc2);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
            g_clear_pointer(&identity, g_free);
            identity = g_strdup_printf("%u:7:0:1", ids[i]);
            check_selection_identity(qts, identity);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x50,
                            ==, present ? 0x40 : 0x10);
            qtest_quit(qts);
            g_clear_pointer(&identity, g_free);
        }
    }
    QTestState *qts = start_wide_targets(1, "");

    qtest_writeb(qts, REG_BASE + 0x1f, 0);
    wide_select(qts, 0, 8, false);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 3);
    check_selection_identity(qts, "none");
    qtest_quit(qts);
}

static void wide_reset(QTestState *qts, unsigned reset)
{
    if (reset == 0) {
        qtest_writeb(qts, REG_BASE + 0x87, 1);
    } else if (reset == 1) {
        qtest_system_reset(qts);
        config_writel(qts, 0x14, REG_BASE);
        config_writew(qts, 4, 6);
    } else {
        qtest_writeb(qts, REG_BASE, 1);
        qtest_writeb(qts, REG_BASE, 0);
    }
}

static void test_wide_resets(void)
{
    static const unsigned ids[] = { 8, 15 };

    for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
        for (unsigned reset = 0; reset < 3; reset++) {
            for (unsigned narrow = 0; narrow < 2; narrow++) {
                QTestState *qts = start_wide_targets(0x81, "");
                uint8_t programming = ids[i] << 4 | 7;
                uint8_t connection = narrow ? 0xe0 : 0xe2;

                qtest_writeb(qts, REG_BASE + 0x1f, connection);
                qtest_writeb(qts, REG_BASE + 2, 4);
                wide_select(qts, ids[i], 7, false);
                qtest_clock_step(qts, 64000000);
                wide_reset(qts, reset);
                check_selection_identity(qts, "none");
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 5),
                                ==, reset == 2 ? programming : 0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f),
                                ==, reset == 2 ? connection : 0xc2);
                qtest_clock_step(qts, 1000000000);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x80,
                                ==, 0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
                qtest_quit(qts);
            }
        }
    }
}

static void test_wide_pending_migration(void)
{
    static const unsigned ids[] = { 8, 15 };

    for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
        for (unsigned period = 0; period < 4; period++) {
            for (unsigned reset = 0; reset < 4; reset++) {
                g_autofree char *dir = g_dir_make_tmp("aic-wide-migrate-XXXXXX",
                                                     NULL);
                g_autofree char *socket = g_build_filename(dir, "migrate.sock",
                                                          NULL);
                g_autofree char *uri = g_strdup_printf("unix:%s", socket);
                g_autofree char *identity = g_strdup_printf("%u:7:0:1", ids[i]);
                QTestState *from = start_wide_targets(0x81, "");
                QTestState *to = start_wide_targets(0x81, "-S -incoming defer");
                int64_t interval = 256000000LL >> period;
                int64_t source_time;

                qtest_writeb(from, REG_BASE + 2, 4 | period << 3);
                int64_t epoch = wide_select(from, ids[i], 7, false);
                source_time = qtest_clock_step(from, interval / 4);
                qtest_writeb(from, REG_BASE + 5, 0x10);
                qtest_qmp_assert_success(to,
                    "{'execute':'migrate-incoming','arguments':{'uri':%s}}",
                    uri);
                sgi_migrate_stopped(from, uri);
                sgi_migrate_resume(to);
                qtest_clock_set(to, source_time);
                check_selection_identity(to, identity);
                g_assert_cmphex(qtest_readb(to, REG_BASE + 5), ==, 0x10);
                g_assert_cmphex(qtest_readb(to, REG_BASE + 0x1f), ==, 0xc2);
                if (reset) {
                    wide_reset(to, reset - 1);
                    qtest_clock_step(to, 1000000000);
                    check_selection_identity(to, "none");
                    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x0c) & 0x80,
                                    ==, 0);
                } else {
                    qtest_clock_set(to, epoch + interval - 1);
                    check_wide_pending(to);
                    qtest_clock_step(to, 1);
                    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x0c) & 0x88,
                                    ==, 0x80);
                    qtest_clock_step(to, 400);
                    check_selection_identity(to, "none");
                    qtest_writeb(to, REG_BASE + 0x11, 0x88);
                    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x91) & 4,
                                    ==, 4);
                    qtest_writeb(to, REG_BASE + 0x92, 4);
                    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x91) & 4,
                                    ==, 4);
                    qtest_writeb(to, REG_BASE + 0x0c, 0x88);
                    qtest_writeb(to, REG_BASE + 0x92, 4);
                    qtest_clock_step(to, interval);
                    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x0c) & 0x88,
                                    ==, 0);
                }
                g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9e), ==, 0);
                qtest_quit(to);
                qtest_quit(from);
                g_unlink(socket);
                g_rmdir(dir);
            }
        }
    }
}

static void test_wide_topology_limits(void)
{
    static const char *invalid[] = {
        "'scsi-id':16", "'scsi-id':31", "'scsi-id':255",
        "'scsi-id':8,'channel':1", "'scsi-id':15,'lun':8",
    };
    QTestState *qts = start_aic_with_options(MEMTX_TEST_DEVICE
        " -drive if=none,id=d,file=null-co://,format=raw");

    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
        g_autofree char *request = g_strdup_printf(
            "{'execute':'device_add','arguments':{'driver':'scsi-hd',"
            "'drive':'d','id':'invalid','bus':'hba.0',%s}}", invalid[i]);
        g_autoptr(QDict) command = qobject_to(
            QDict, qobject_from_json(request, &error_abort));
        g_autoptr(QDict) reply = qtest_qmp_assert_failure_ref(
            qts, "%p", g_steal_pointer(&command));

        g_assert_nonnull(strstr(qdict_get_str(reply, "desc"), "bad scsi"));
        check_selection_identity(qts, "none");
    }
    qtest_writeb(qts, REG_BASE + 5, 0xf7);
    for (unsigned size = 2; size <= 8; size *= 2) {
        uint64_t ignored;

        g_assert_cmpint(memtx_store(qts, REG_BASE + 5, 0x87, size),
                        !=, MEMTX_OK);
        g_assert_cmpint(memtx_load(qts, REG_BASE + 4, &ignored, size),
                        !=, MEMTX_OK);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 5), ==, 0xf7);
    }
    check_selection_identity(qts, "none");
    qtest_quit(qts);
}

static void test_wide_selected_reprogramming(void)
{
    static const uint8_t inquiry[] = { 0x12, 0, 0, 0, 36, 0 };
    static const unsigned ids[] = { 8, 15 };

    for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
        for (unsigned self = 0; self < 2; self++) {
            QTestState *qts = start_wide_targets(0x8181, "");
            uint8_t got[36];
            uint8_t programming = self ? 0x77 : 0x76;
            g_autofree char *product =
                g_strdup_printf("SYNTHETIC-ID%02u", ids[i]);

            g_assert_cmphex(wide_command(qts, ids[i], 7, 0, true, inquiry,
                                         sizeof(inquiry), 36, false,
                                         programming), ==, 0);
            qtest_memread(qts, 0x20008, got, sizeof(got));
            g_assert_cmpmem(got + 16, strlen(product),
                            product, strlen(product));
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 5), ==, programming);
            attention_write(qts, 0, 0);
            attention_write(qts, 0, 0x40);
            if (self) {
                check_selection_identity(qts, "none");
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
            } else {
                check_selection_identity(qts, "7:6:0:1");
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x50,
                                ==, 0x40);
            }
            qtest_quit(qts);
        }
    }
    QTestState *qts = start_wide_targets(1, "");

    qtest_writeb(qts, REG_BASE + 0x1f, 0);
    g_assert_cmphex(wide_command(qts, 0, 7, 0, false, inquiry,
                                 sizeof(inquiry), 36, false, -1), ==, 0);
    qtest_quit(qts);
    qts = start_wide_targets(1u << 8, "");
    g_assert_cmphex(wide_command(qts, 8, 15, 0, true, inquiry,
                                 sizeof(inquiry), 36, false, -1), ==, 0);
    qtest_quit(qts);
}

static void test_wide_lun_routing(void)
{
    static const uint8_t inquiry[] = { 0x12, 0, 0, 0, 36, 0 };
    static const uint8_t ready[6];
    static const uint8_t sense[] = { 3, 0, 0, 0, 18, 0 };
    static const unsigned ids[] = { 8, 15 };
    static const unsigned luns[] = { 3, 0, 2 };

    for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
        g_autofree char *extra = g_strdup_printf(
            "-drive if=none,id=l3,file=null-co://,format=raw "
            "-device scsi-hd,drive=l3,bus=hba.0,scsi-id=%u,lun=3,"
            "product=SYNTHETIC-LUN3", ids[i]);
        QTestState *qts = start_wide_targets(1u << ids[i], extra);
        uint8_t got[36];

        for (unsigned lun = 0; lun < ARRAY_SIZE(luns); lun++) {
            g_assert_cmphex(wide_command(qts, ids[i], 7, luns[lun], true,
                                         inquiry, sizeof(inquiry), 36,
                                         false, -1), ==, 0);
            qtest_memread(qts, 0x20008, got, sizeof(got));
            if (luns[lun] == 3) {
                g_assert_cmpmem(got + 16, 14, "SYNTHETIC-LUN3", 14);
            } else {
                g_assert_cmphex(got[0], ==, luns[lun] == 2 ? 0x7f : 0);
            }
        }
        g_assert_cmphex(wide_command(qts, ids[i], 7, 2, true, ready,
                                     sizeof(ready), 0, false, -1), ==, 2);
        g_assert_cmphex(wide_command(qts, ids[i], 7, 2, true, sense,
                                     sizeof(sense), 18, false, -1), ==, 0);
        qtest_memread(qts, 0x20008, got, 18);
        g_assert_cmphex(got[2] & 15, ==, 5);
        g_assert_cmphex(got[12], ==, 0x25);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x20, ==, 0);
        qtest_quit(qts);
    }
}

static void test_wide_file_data(void)
{
    static const uint8_t read0[] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0 };
    static const uint8_t read1[] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
    static const uint8_t write1[] = { 0x2a, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
    static const uint8_t sense[] = { 3, 0, 0, 0, 18, 0 };
    static const uint8_t capacity[10] = { 0x25 };
    static const unsigned ids[] = { 8, 15 };

    for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
        g_autofree char *path = NULL;
        uint8_t pattern[512], guard[528], got[528], written[512];
        int fd = g_file_open_tmp("aic-wide-data-XXXXXX", &path, NULL);
        QTestState *qts;

        g_assert_cmpint(fd, >=, 0);
        g_assert_cmpint(ftruncate(fd, 4096), ==, 0);
        for (unsigned byte = 0; byte < sizeof(pattern); byte++) {
            pattern[byte] = (byte * 37 + ids[i]) ^ (byte >> 8);
        }
        g_assert_cmpint(pwrite(fd, pattern, sizeof(pattern), 0), ==,
                        sizeof(pattern));
        qts = qtest_initf(
            "-M pc -m 128M -nodefaults "
            "-drive if=none,id=d,file=%s,format=raw "
            "-device aic7880,id=hba,bus=pci.0,addr=04.0 "
            "-device scsi-hd,drive=d,bus=hba.0,scsi-id=%u,lun=0", path, ids[i]);
        config_writel(qts, 0x14, REG_BASE);
        config_writew(qts, 4, 6);
        g_assert_cmphex(wide_command(qts, ids[i], 7, 0, true, read0,
                                     sizeof(read0), 512, false, -1), ==, 2);
        g_assert_cmphex(wide_command(qts, ids[i], 7, 0, true, sense,
                                     sizeof(sense), 18, false, -1), ==, 0);
        g_assert_cmphex(wide_command(qts, ids[i], 7, 0, false, capacity,
                                     sizeof(capacity), 8, false, -1), ==, 0);
        memset(guard, 0xa5, sizeof(guard));
        qtest_memwrite(qts, 0x20000, guard, sizeof(guard));
        g_assert_cmphex(wide_command(qts, ids[i], 7, 0, false, read0,
                                     sizeof(read0), 512, false, -1), ==, 0);
        qtest_memread(qts, 0x20000, got, sizeof(got));
        g_assert_cmpmem(got, 8, guard, 8);
        g_assert_cmpmem(got + 520, 8, guard, 8);
        g_assert_cmpmem(got + 8, 512, pattern, 512);
        for (unsigned byte = 0; byte < sizeof(pattern); byte++) {
            pattern[byte] ^= 0x5a;
        }
        memcpy(guard + 8, pattern, sizeof(pattern));
        qtest_memwrite(qts, 0x20000, guard, sizeof(guard));
        g_assert_cmphex(wide_command(qts, ids[i], 7, 0, true, write1,
                                     sizeof(write1), 512, true, -1), ==, 0);
        g_assert_cmpint(pread(fd, written, sizeof(written), 512), ==,
                        sizeof(written));
        g_assert_cmpmem(written, sizeof(written), pattern, sizeof(pattern));
        qtest_memread(qts, 0x20000, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), guard, sizeof(guard));
        memset(guard, 0xa5, sizeof(guard));
        qtest_memwrite(qts, 0x20000, guard, sizeof(guard));
        g_assert_cmphex(wide_command(qts, ids[i], 7, 0, true, read1,
                                     sizeof(read1), 512, false, -1), ==, 0);
        qtest_memread(qts, 0x20000, got, sizeof(got));
        g_assert_cmpmem(got, 8, guard, 8);
        g_assert_cmpmem(got + 520, 8, guard, 8);
        g_assert_cmpmem(got + 8, 512, pattern, 512);
        qtest_quit(qts);
        close(fd);
        g_unlink(path);
    }
}

static void test_wide_negotiation(void)
{
    static const uint8_t wdtr[] = { 1, 2, 3, 1 };
    static const uint8_t inquiry[] = { 0x12, 0, 0, 0, 36, 0 };
    static const unsigned ids[] = { 8, 15 };

    for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
        QTestState *qts = start_wide_targets(1u << ids[i], "");
        g_autofree char *identity = g_strdup_printf("%u:7:0:1", ids[i]);
        uint8_t program[256], got[36];
        size_t n = 0;

        host_dma_program(qts, 0x20008, 36);
        qtest_writeb(qts, REG_BASE + 8, 36);
        wide_select(qts, ids[i], 7, true);
        attention_write(qts, 6, 0xc0);
        for (unsigned byte = 0; byte < sizeof(wdtr); byte++) {
            attention_write(qts, 6, wdtr[byte]);
        }
        attention_write(qts, 0x0c, 0x40);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe0, ==, 0xe0);
        qtest_writeb(qts, REG_BASE + 3, 0xe0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, 7);
        check_selection_identity(qts, identity);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 4), ==, 0);
        emit_cdb_completion(program, &n, inquiry, sizeof(inquiry), false, true);
        start_program(qts, program, n);
        coupled_wait(qts, 0x91, 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x22), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x23), ==, 0);
        qtest_memread(qts, 0x20008, got, sizeof(got));
        g_assert_cmphex(got[0], ==, 0);
        check_selection_identity(qts, "none");
        qtest_quit(qts);
    }
}

static void test_wide_data_refusal(void)
{
    static const uint8_t inquiry[] = { 0x12, 0, 0, 0, 36, 0 };
    static const unsigned ids[] = { 8, 15 };
    static const uint8_t rates[] = { 0x80, 1 };

    for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
        for (unsigned mode = 0; mode < ARRAY_SIZE(rates); mode++) {
            QTestState *qts = start_wide_targets(1u << ids[i], "");
            uint8_t program[128], guard[52], got[52];
            g_autofree char *identity = g_strdup_printf("%u:7:0:1", ids[i]);
            size_t n = 0;

            memset(guard, 0xa7, sizeof(guard));
            qtest_memwrite(qts, 0x20000, guard, sizeof(guard));
            host_dma_program(qts, 0x20008, 36);
            qtest_writeb(qts, REG_BASE + 8, 36);
            qtest_writeb(qts, REG_BASE + 4, rates[mode]);
            wide_select(qts, ids[i], 7, false);
            for (unsigned byte = 0; byte < sizeof(inquiry); byte++) {
                emit_or(program, &n, inquiry[byte], 6);
            }
            emit_or(program, &n, 0x3a, 0x93);
            emit_or(program, &n, 0xa5, 0x41);
            start_program(qts, program, n);
            qtest_clock_step(qts, 1000000);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
            g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 36);
            qtest_memread(qts, 0x20000, got, sizeof(got));
            g_assert_cmpmem(got, sizeof(got), guard, sizeof(guard));
            check_selection_identity(qts, identity);
            qtest_quit(qts);
        }
    }
}

static void test_wide_status_migration(void)
{
    static const unsigned ids[] = { 8, 15 };

    for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
        for (unsigned message = 0; message < 2; message++) {
            g_autofree char *dir =
                g_dir_make_tmp("aic-wide-status-XXXXXX", NULL);
            g_autofree char *socket =
                g_build_filename(dir, "migrate.sock", NULL);
            g_autofree char *uri = g_strdup_printf("unix:%s", socket);
            g_autofree char *identity = g_strdup_printf("%u:7:0:1", ids[i]);
            QTestState *from = start_wide_targets(1u << ids[i], "");
            QTestState *to = start_wide_targets(1u << ids[i],
                                               "-S -incoming defer");

            wide_select(from, ids[i], 7, false);
            for (unsigned byte = 0; byte < 6; byte++) {
                attention_write(from, 6, 0);
            }
            manual_wait_phase(from, 0xc0);
            qtest_writeb(from, REG_BASE + 5, 0x76);
            qtest_writeb(from, REG_BASE + 3, 0xc0);
            g_assert_cmphex(qtest_readb(from, REG_BASE + 0x12), ==, 2);
            if (message) {
                g_assert_cmphex(qtest_readb(from, REG_BASE + 6), ==, 2);
                qtest_writeb(from, REG_BASE + 3, 0xe0);
            }
            qtest_qmp_assert_success(to,
                "{'execute':'migrate-incoming','arguments':{'uri':%s}}",
                    uri);
            sgi_migrate_stopped(from, uri);
            sgi_wait_migration(to, "completed");
            check_selection_identity(to, identity);
            g_assert_cmphex(qtest_readb(to, REG_BASE + 5), ==, 0x76);
            g_assert_cmphex(qtest_readb(to, REG_BASE + 6), ==, message ? 0 : 2);
            if (!message) {
                qtest_writeb(to, REG_BASE + 3, 0xe0);
                g_assert_cmphex(qtest_readb(to, REG_BASE + 6), ==, 0);
            }
            check_selection_identity(to, "none");
            g_assert_cmphex(qtest_readb(to, REG_BASE + 0x0c) & 8, ==, 8);
            qtest_quit(to);
            qtest_quit(from);
            g_unlink(socket);
            g_rmdir(dir);
        }
    }
}

static void test_wide_controller_isolation(void)
{
    for (unsigned other = 0; other < 2; other++) {
        QTestState *qts = qtest_initf(
            "-M pc -m 128M -nodefaults "
            "-device aic7880,id=hba,bus=pci.0,addr=04.0 "
            "-device aic7880,id=other,bus=pci.0,addr=05.0 "
            "-drive if=none,id=d,file=null-co://,format=raw "
            "-device scsi-hd,drive=d,bus=%s.0,scsi-id=8,lun=0",
            other ? "hba" : "other");
        unsigned devfn = other ? 0x28 : PCI_DEVFN;

        qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000014u | devfn << 8);
        qtest_outl(qts, PCI_CONFIG_DATA, REG_BASE);
        qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000004u | devfn << 8);
        qtest_outw(qts, PCI_CONFIG_DATA, 6);
        qtest_writeb(qts, REG_BASE + 2, 0x1c);
        wide_select(qts, 8, 7, false);
        check_wide_pending(qts);
        qtest_clock_step(qts, 32000400);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x88, ==, 0x88);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        qtest_quit(qts);
    }
}

static void corrupt_selection_stream(const char *source, const char *dest,
                                     const char *field, unsigned value)
{
    static const char script[] =
        "import contextlib, io, runpy, sys\n"
        "tool, source, dest, field, value = sys.argv[1:]\n"
        "sys.argv = [tool, '-f', source]\n"
        "with contextlib.redirect_stdout(io.StringIO()):\n"
        "    ns = runpy.run_path(tool)\n"
        "offsets, active = [], [False]\n"
        "base = ns['VMSDFieldGeneric']\n"
        "read = base.read\n"
        "def capture(self):\n"
        "    if active[0] and self.desc.get('name') == field:\n"
        "        assert self.desc['size'] == 1\n"
        "        offsets.append(self.file.tell())\n"
        "    return read(self)\n"
        "base.read = capture\n"
        "section = ns['VMSDSection']\n"
        "section_read = section.read\n"
        "def capture_section(self):\n"
        "    active[0] = 'aic7880' in self.section_key[0]\n"
        "    try:\n"
        "        return section_read(self)\n"
        "    finally:\n"
        "        active[0] = False\n"
        "section.read = capture_section\n"
        "ns['MigrationDump'](source).read()\n"
        "assert len(offsets) == 1, offsets\n"
        "with open(source, 'rb') as f:\n"
        "    data = bytearray(f.read())\n"
        "data[offsets[0]] = int(value)\n"
        "with open(dest, 'wb') as f:\n"
        "    f.write(data)\n";
    g_autofree char *analyzer = g_build_filename(g_getenv("G_TEST_SRCDIR"),
        "..", "..", "scripts", "analyze-migration.py", NULL);
    g_autofree char *number = g_strdup_printf("%u", value);
    g_autofree char *errors = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) argv = g_new0(char *, 9);
    int status;

    argv[0] = g_strdup(g_getenv("PYTHON"));
    argv[1] = g_strdup("-c");
    argv[2] = g_strdup(script);
    argv[3] = g_strdup(analyzer);
    argv[4] = g_strdup(source);
    argv[5] = g_strdup(dest);
    argv[6] = g_strdup(field);
    argv[7] = g_strdup(number);
    /* Locate the field through QEMU's stream decoder, not a fixed offset. */
    g_assert_true(g_spawn_sync(NULL, argv, NULL, 0, NULL, NULL, NULL, &errors,
                               &status, &error));
    g_assert_no_error(error);
    if (!g_spawn_check_exit_status(status, &error)) {
        g_test_message("migration mutation: %s", errors);
    }
    g_assert_no_error(error);
}

static void test_wide_invalid_migration(void)
{
    static const struct {
        const char *field;
        uint8_t value;
    } invalid[] = {
        { "target", 16 }, { "target", 31 }, { "target", 255 },
        { "initiator", 16 }, { "initiator", 31 }, { "initiator", 255 },
        { "target", 7 }, { "lun", 8 }, { "identity_valid", 0 },
        { "selection_wide", 0 }, { "selected", 1 },
    };
    g_autofree char *dir = g_dir_make_tmp("aic-wide-invalid-XXXXXX", NULL);
    g_autofree char *source = g_build_filename(dir, "source", NULL);
    g_autofree char *dest = g_build_filename(dir, "invalid", NULL);
    g_autofree char *source_uri = g_strdup_printf("file:%s", source);
    g_autofree char *dest_uri = g_strdup_printf("file:%s", dest);
    QTestState *from = start_aic();

    qtest_writeb(from, REG_BASE + 2, 0x1c);
    wide_select(from, 8, 7, false);
    qtest_writeb(from, REG_BASE + 5, 0x76);
    sgi_migrate_stopped(from, source_uri);
    qtest_quit(from);
    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
        QTestState *to;

        corrupt_selection_stream(source, dest, invalid[i].field,
                                  invalid[i].value);
        to = start_aic_with_options("-S -incoming defer");
        qtest_set_expected_status(to, EXIT_FAILURE);
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", dest_uri);
        qtest_wait_qemu(to);
        qtest_quit(to);
        g_unlink(dest);
    }
    g_unlink(source);
    g_rmdir(dir);
}

static void test_wide_unselected_and_incoming(void)
{
    QTestState *qts = start_wide_targets(0x8181,
                                        "-global aic7880.x-test-peer=on");

    for (unsigned id = 0; id < 16; id++) {
        qtest_writeb(qts, REG_BASE + 5, id << 4 | 7);
        qtest_writeb(qts, REG_BASE, 0x10);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 5), ==, id << 4 | 7);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        check_selection_identity(qts, "none");
    }
    qtest_writeb(qts, REG_BASE + 5, 0x87);
    peer_event_rejected(qts, "reselect:8:7");
    qtest_writeb(qts, REG_BASE + 5, 0xf7);
    peer_event_rejected(qts, "reselect:15:7");
    check_selection_identity(qts, "none");
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x20, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x1f, 0xe2);
    for (unsigned bit = 0; bit < 5; bit++) {
        if (!(0x15 & (1u << bit))) {
            continue;
        }
        qtest_writeb(qts, REG_BASE + 0x1f, 1u << bit);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f), ==, 0xe2);
    }
    qtest_quit(qts);
}

static void test_wide_width_atomicity(void)
{
    static const struct {
        uint8_t reg;
        uint8_t size;
    } accesses[] = {
        { 3, 2 }, { 3, 4 }, { 3, 8 },
        { 4, 2 }, { 4, 4 }, { 4, 8 },
        { 5, 2 }, { 5, 4 }, { 5, 8 },
        { 0x1b, 8 }, { 0x1d, 4 }, { 0x1d, 8 },
        { 0x1e, 2 }, { 0x1e, 4 }, { 0x1e, 8 },
        { 0x1f, 2 }, { 0x1f, 4 }, { 0x1f, 8 },
    };

    for (unsigned external = 0; external < 2; external++) {
        QTestState *qts = external ?
            start_external_scb(1024, 8, MEMTX_TEST_DEVICE) :
            start_aic_with_options(MEMTX_TEST_DEVICE);
        ManualGuard before;
        uint8_t scratch[8], after[8];
        uint32_t pci_command;

        qtest_writeb(qts, REG_BASE + 4, 0x33);
        qtest_writeb(qts, REG_BASE + 5, 0xf7);
        qtest_writeb(qts, REG_BASE + 0x1f, 0xe2);
        manual_counters(qts);
        for (unsigned byte = 0; byte < sizeof(scratch); byte++) {
            scratch[byte] = 0xa0 + byte;
            qtest_writeb(qts, REG_BASE + 0x20 + byte, scratch[byte]);
        }
        manual_capture(qts, &before);
        pci_command = config_readl(qts, 4);
        for (unsigned alias = 0; alias < 16; alias++) {
            for (unsigned i = 0; i < ARRAY_SIZE(accesses); i++) {
                uint64_t ignored;
                uint64_t address = REG_BASE + alias * 256 + accesses[i].reg;

                g_assert_cmpint(memtx_store(qts, address,
                    0x1122334455667788ULL, accesses[i].size), !=, MEMTX_OK);
                g_assert_cmpint(memtx_load(qts, address, &ignored,
                                           accesses[i].size), !=, MEMTX_OK);
                manual_unchanged(qts, &before);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f), ==, 0xe2);
                for (unsigned byte = 0; byte < sizeof(after); byte++) {
                    after[byte] = qtest_readb(qts, REG_BASE + 0x20 + byte);
                }
                g_assert_cmpmem(after, sizeof(after), scratch, sizeof(scratch));
                g_assert_cmphex(config_readl(qts, 4), ==, pci_command);
                check_selection_identity(qts, "none");
            }
        }
        if (external) {
            for (unsigned size = 4; size <= 8; size *= 2) {
                qtest_writeb(qts, REG_BASE + 0x9a, 0x80);
                g_assert_cmpint(memtx_write(qts, REG_BASE + 0xa0,
                    0x1716151413121110ULL, size), ==, MEMTX_OK);
                qtest_writeb(qts, REG_BASE + 0x9a, 0);
                for (unsigned byte = 0; byte < size; byte++) {
                    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0 + byte),
                                    ==, 0x10 + byte);
                }
            }
        } else {
            /* Retained byte placement, not measured physical bus widths. */
            g_assert_cmpint(memtx_write(qts, REG_BASE + 0x120,
                0x1716151413121110ULL, 8), ==, MEMTX_OK);
            for (unsigned byte = 0; byte < sizeof(after); byte++) {
                after[byte] = 0x10 + byte;
            }
            qtest_memread(qts, REG_BASE + 0x20, scratch, sizeof(scratch));
            g_assert_cmpmem(scratch, sizeof(scratch), after, sizeof(after));
            config_writel(qts, 0x10, 0x1001);
            config_writew(qts, 4, 7);
            g_assert_cmphex(qtest_inw(qts, 0x1004), ==, 0);
            qtest_outw(qts, 0x1004, 0x0000);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 4), ==, 0x33);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 5), ==, 0xf7);
        }
        qtest_quit(qts);
    }
}

static void command_dma_count(QTestState *qts, unsigned reg, uint32_t count)
{
    for (unsigned i = 0; i < 3; i++) {
        qtest_writeb(qts, REG_BASE + reg + i, count >> (8 * i));
    }
}

static void command_dma_prepare(QTestState *qts, const uint8_t *cdb,
                                unsigned bytes, unsigned host_count,
                                unsigned scsi_count, unsigned offset)
{
    uint8_t guarded[288];

    memset(guarded, 0xa5, sizeof(guarded));
    memcpy(guarded + 8 + offset, cdb, bytes);
    manual_command(qts, cdb, 0);
    qtest_writeb(qts, REG_BASE + 1, 8);
    qtest_memwrite(qts, 0x18000, guarded, sizeof(guarded));
    host_dma_program(qts, 0x18008 + offset, host_count);
    command_dma_count(qts, 8, scsi_count);
    qtest_writeb(qts, REG_BASE + 0x93, 5);
}

static void command_dma_no_completion(QTestState *qts)
{
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 3, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
}

static void command_dma_finish(QTestState *qts, bool inquiry)
{
    uint8_t data[52];

    command_dma_no_completion(qts);
    qtest_writeb(qts, REG_BASE + 0x93, 0);
    if (inquiry) {
        qtest_memset(qts, 0x20000, 0x5a, sizeof(data));
        host_dma_program(qts, 0x20008, 36);
        command_dma_count(qts, 8, 36);
        qtest_writeb(qts, REG_BASE + 0x93, 1);
        qtest_writeb(qts, REG_BASE + 3, 0x40);
        qtest_writeb(qts, REG_BASE + 0x93, 0x38);
        manual_wait_phase(qts, 0xc0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
        qtest_memread(qts, 0x20000, data, sizeof(data));
        for (unsigned i = 0; i < 8; i++) {
            g_assert_cmphex(data[i], ==, 0x5a);
            g_assert_cmphex(data[44 + i], ==, 0x5a);
        }
        g_assert_cmphex(data[8], ==, 0);
        g_assert_cmpmem(data + 16, 7, "LABTEST", 7);
        g_assert_cmpmem(data + 24, 14, "SEGMENTED-DISK", 14);
        qtest_writeb(qts, REG_BASE + 0x93, 0);
    }
    qtest_writeb(qts, REG_BASE + 1, 8);
    manual_wait_phase(qts, 0xc0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, inquiry ? 0 : 2);
    qtest_writeb(qts, REG_BASE + 3, 0xe0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
    command_dma_no_completion(qts);
}

static void command_dma_trace(const char *path, const uint8_t *cdb,
                              unsigned len, unsigned offset,
                              unsigned fetched, unsigned requests)
{
    g_autofree char *trace = NULL;
    g_autoptr(GString) command = g_string_new("data=");
    g_auto(GStrv) lines = NULL;
    unsigned seen = 0, credited = 0;

    g_assert_true(g_file_get_contents(path, &trace, NULL, NULL));
    for (unsigned i = 0; i < len; i++) {
        g_string_append_printf(command, "%s%02x", i ? " " : "", cdb[i]);
    }
    lines = g_strsplit(trace, "\n", -1);
    for (unsigned i = 0; lines[i]; i++) {
        const char *event = strstr(lines[i], "aic7880_dma ");
        unsigned devfn, address, count, direction;
        int result;

        if (strstr(lines[i], "scsi_disk_new_request")) {
            seen++;
            g_assert_nonnull(strstr(lines[i], command->str));
        }
        if (!event || sscanf(event,
            "aic7880_dma devfn 0x%x addr 0x%x len %u to_memory %u result %d",
            &devfn, &address, &count, &direction, &result) != 5 ||
            address < 0x18000 || address >= 0x18120) {
            continue;
        }
        g_assert_cmphex(devfn, ==, PCI_DEVFN);
        g_assert_cmpuint(direction, ==, 0);
        g_assert_cmpint(result, ==, MEMTX_OK);
        g_assert_cmphex(address, ==, 0x18008 + offset + credited);
        credited += count;
        g_assert_cmpuint(credited, <=, fetched);
    }
    g_assert_cmpuint(seen, ==, requests);
    g_assert_cmpuint(credited, ==, fetched);
}

static void test_command_dma_offsets(void)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };

    for (unsigned offset = 0; offset < 8; offset++) {
        g_autofree char *dir =
            g_dir_make_tmp("aic-command-offset-XXXXXX", NULL);
        g_autofree char *trace = g_build_filename(dir, "trace", NULL);
        QTestState *qts = start_selected_disk(trace);
        uint8_t guard[288];

        command_dma_prepare(qts, cdb, sizeof(cdb), 6, 6, offset);
        qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
        check_host_fifo_status(qts, (8 + offset) & 7, 6, true);
        g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, 6);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 5, ==, 0);
        qtest_writeb(qts, REG_BASE + 0x93, 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93), ==, 4);
        qtest_writeb(qts, REG_BASE + 0x93, 0x34);
        g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 5, ==, 5);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 2, ==, 0);
        qtest_memread(qts, 0x18000, guard, sizeof(guard));
        for (unsigned i = 0; i < sizeof(guard); i++) {
            g_assert_cmphex(guard[i], ==,
                i >= 8 + offset && i < 14 + offset ?
                cdb[i - 8 - offset] : 0xa5);
        }
        command_dma_finish(qts, true);
        qtest_quit(qts);
        command_dma_trace(trace, cdb, sizeof(cdb), offset, 6, 1);
        unlink(trace);
        g_rmdir(dir);
    }
}

static void test_command_dma_short_counts(void)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };

    for (unsigned short_host = 0; short_host < 2; short_host++) {
        g_autofree char *dir = g_dir_make_tmp("aic-command-short-XXXXXX", NULL);
        g_autofree char *trace = g_build_filename(dir, "trace", NULL);
        QTestState *qts = start_selected_disk(trace);

        command_dma_prepare(qts, cdb, 6, short_host ? 2 : 6,
                            short_host ? 6 : 2, 4);
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe0, ==, 0x80);
        g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, short_host ? 4 : 0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x1800e);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 5, ==,
                        short_host ? 0 : 5);
        command_dma_no_completion(qts);
        if (short_host) {
            qtest_writeb(qts, REG_BASE + 0x93, 0x34);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93), ==, 0x34);
            command_dma_count(qts, 0x8c, 4);
        } else {
            check_host_fifo_status(qts, 6, 4, true);
            qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 5, ==, 0);
            command_dma_count(qts, 8, 4);
        }
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, 0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x18012);
        command_dma_finish(qts, true);
        qtest_quit(qts);
        command_dma_trace(trace, cdb, 6, 4, 6, 1);
        unlink(trace);
        g_rmdir(dir);
    }
}

static void test_command_dma_lengths(void)
{
    static const uint8_t cdbs[][16] = {
        { 0, 0, 0, 0, 0, 0 },
        { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0xa8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0x88, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    static const unsigned lengths[] = { 6, 10, 12, 16 };

    for (unsigned i = 0; i < ARRAY_SIZE(lengths); i++) {
        g_autofree char *dir =
            g_dir_make_tmp("aic-command-length-XXXXXX", NULL);
        g_autofree char *trace = g_build_filename(dir, "trace", NULL);
        QTestState *qts = start_selected_disk(trace);

        /* Consume the disk's initial unit attention before tracing its CDB. */
        manual_command(qts, cdbs[0], 6);
        command_dma_finish(qts, false);
        command_dma_prepare(qts, cdbs[i], lengths[i],
                            lengths[i], lengths[i], 0);
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, 0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe0, !=, 0x80);
        command_dma_no_completion(qts);
        qtest_quit(qts);
        command_dma_trace(trace, cdbs[i], lengths[i], 0, lengths[i], 1);
        unlink(trace);
        g_rmdir(dir);
    }
}

static void test_command_dma_excess_counts(void)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };

    for (unsigned excess_host = 0; excess_host < 2; excess_host++) {
        QTestState *qts = start_selected_disk(NULL);

        command_dma_prepare(qts, cdb, 6, excess_host ? 10 : 6,
                            excess_host ? 6 : 10, 0);
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==,
                        excess_host ? 0 : 4);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x1800e);
        check_host_fifo_status(qts, 6, excess_host ? 4 : 0, true);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe0, ==, 0x40);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xc) & 0x11, ==, 0x11);
        command_dma_no_completion(qts);
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==,
                        excess_host ? 0 : 4);
        command_dma_finish(qts, true);
        qtest_quit(qts);
    }
    {
        static const uint8_t ready[6] = { 0 };
        QTestState *qts = start_selected_disk(NULL);

        command_dma_prepare(qts, ready, 6, 6, 10, 0);
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe0, ==, 0xc0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 5, ==, 0);
        command_dma_finish(qts, false);
        qtest_quit(qts);
    }
}

static void test_command_dma_full_and_empty(void)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };

    for (unsigned full = 0; full < 2; full++) {
        QTestState *qts = start_selected_disk(NULL);

        command_dma_prepare(qts, cdb, 6, full ? 270 : 6, 6, 0);
        if (full) {
            qtest_writeb(qts, REG_BASE + 3, 0xc0);
        } else {
            config_writew(qts, 4, 2);
        }
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        check_host_fifo_status(qts, 0, full ? 256 : 0, false);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, full ? 14 : 6);
        g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, 6);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 7, ==, 0);
        command_dma_no_completion(qts);
        qtest_quit(qts);
    }
}

static void test_command_dma_pio_ownership(void)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };

    for (unsigned refuse = 0; refuse < 2; refuse++) {
        g_autofree char *dir = g_dir_make_tmp("aic-command-pio-XXXXXX", NULL);
        g_autofree char *trace = g_build_filename(dir, "trace", NULL);
        QTestState *qts = start_selected_disk(trace);

        command_dma_prepare(qts, cdb, 6, 2, 6, 0);
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 1) & 8, ==, 8);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 6, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 2, ==, 2);
        if (refuse) {
            attention_write_raw(qts, 6, 0xff);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
            g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, 4);
            command_dma_no_completion(qts);
        } else {
            qtest_writeb(qts, REG_BASE + 0x93, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 2, ==, 2);
            for (unsigned i = 2; i < sizeof(cdb); i++) {
                attention_write(qts, 6, cdb[i]);
                qtest_writeb(qts, REG_BASE + 0x92, 1);
            }
            g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, 4);
            command_dma_finish(qts, true);
        }
        qtest_quit(qts);
        command_dma_trace(trace, cdb, 6, 0, 2, refuse ? 0 : 1);
        unlink(trace);
        g_rmdir(dir);
    }
}

static void test_command_dma_phase_and_rate(void)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };

    for (unsigned mismatch = 0; mismatch < 2; mismatch++) {
        QTestState *qts = start_selected_disk(NULL);

        command_dma_prepare(qts, cdb, 6, 6, 6, 0);
        qtest_writeb(qts, REG_BASE + 4, 0x8f);
        if (mismatch) {
            qtest_writeb(qts, REG_BASE + 3, 0xc0);
        }
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        if (mismatch) {
            g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, 6);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xc) & 0x11, ==, 0x11);
            qtest_writeb(qts, REG_BASE + 3, 0x80);
        }
        g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe0, ==, 0x40);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 4), ==, 0x8f);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x1800e);
        qtest_writeb(qts, REG_BASE + 4, 0);
        command_dma_finish(qts, true);
        qtest_quit(qts);
    }
}

static void test_command_dma_zero_and_invalid(void)
{
    static const uint8_t inquiry[] = { 0x12, 0, 0, 0, 36, 0 };
    static const uint8_t variable[] = { 0x7f, 0, 0, 0, 0, 0 };

    for (unsigned mode = 0; mode < 4; mode++) {
        QTestState *qts = start_selected_disk(NULL);

        command_dma_prepare(qts, mode == 3 ? variable : inquiry, 6,
                            mode == 0 ? 0 : 6, mode == 1 ? 0 : 6, 0);
        if (mode == 2) {
            qtest_writeb(qts, REG_BASE + 0x93, 0);
        }
        qtest_writeb(qts, REG_BASE + 0x93, mode == 2 ? 0x38 : 0x3c);
        g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, mode == 1 ? 0 : 6);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe0, ==, 0x80);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 5, ==,
                        mode == 1 ? 5 : 0);
        if (mode == 0 || mode == 2) {
            g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x18008);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
        }
        command_dma_no_completion(qts);
        if (mode >= 2) {
            attention_write_raw(qts, 0x40, 0xa5);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 0);
        }
        qtest_quit(qts);
    }
}

static void test_command_dma_admission(void)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };

    for (unsigned mastering = 0; mastering < 2; mastering++) {
        QTestState *qts = start_selected_disk(NULL);

        command_dma_prepare(qts, cdb, 6, 2, 6, 0);
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        qtest_writeb(qts, REG_BASE + 0x93, 0x34);
        command_dma_count(qts, 0x8c, 4);
        config_writew(qts, 4, 2);
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        if (!mastering) {
            config_writew(qts, 4, 4);
        }
        config_writew(qts, 4, 2);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 4);
        g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, 4);
        g_assert_cmphex(config_readl(qts, 4) & 0x30000000, ==, 0);
        config_writew(qts, 4, 6);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
        command_dma_finish(qts, true);
        qtest_quit(qts);
    }
    {
        g_autofree char *dir = g_dir_make_tmp("aic-command-fault-XXXXXX", NULL);
        g_autofree char *path = g_build_filename(dir, "trace", NULL);
        g_autofree char *trace = NULL;
        g_auto(GStrv) lines = NULL;
        unsigned failures = 0;
        QTestState *qts = start_selected_disk(path);

        command_dma_prepare(qts, cdb, 6, 2, 6, 0);
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        qtest_writeb(qts, REG_BASE + 0x93, 4);
        host_dma_program(qts, 0x70000000, 4);
        qtest_writeb(qts, REG_BASE + 0x93, 5);
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 4);
        g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, 4);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x70000000);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 8, ==, 0);
        qtest_writeb(qts, REG_BASE + 0x93, 4);
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 4);
        command_dma_no_completion(qts);
        qtest_quit(qts);
        command_dma_trace(path, cdb, 6, 0, 2, 0);
        g_assert_true(g_file_get_contents(path, &trace, NULL, NULL));
        lines = g_strsplit(trace, "\n", -1);
        for (unsigned i = 0; lines[i]; i++) {
            const char *event = strstr(lines[i], "aic7880_dma ");
            unsigned devfn, address, count, direction;
            int result;

            if (!event || sscanf(event,
                "aic7880_dma devfn 0x%x addr 0x%x len %u to_memory %u result %d",
                &devfn, &address, &count, &direction, &result) != 5 ||
                address != 0x70000000) {
                continue;
            }
            failures++;
            g_assert_cmphex(devfn, ==, PCI_DEVFN);
            g_assert_cmpuint(count, ==, 4);
            g_assert_cmpuint(direction, ==, 0);
            g_assert_cmpint(result, ==, MEMTX_DECODE_ERROR);
        }
        g_assert_cmpuint(failures, ==, 1);
        unlink(path);
        g_rmdir(dir);
    }
}

static void test_command_dma_reset(void)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };

    for (unsigned reset = 0; reset < 3; reset++) {
        QTestState *qts = start_selected_disk(NULL);

        command_dma_prepare(qts, cdb, 6, 6, 2, 0);
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        if (reset == 0) {
            qtest_writeb(qts, REG_BASE, 1);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93), ==, 0x3c);
            g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x1800e);
            g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
            check_host_fifo_status(qts, 2, 4, true);
            qtest_writeb(qts, REG_BASE + 0x93, 0);
            qtest_writeb(qts, REG_BASE + 0x93, 1);
            qtest_writeb(qts, REG_BASE, 0);
        } else {
            if (reset == 1) {
                qtest_writeb(qts, REG_BASE + 0x87, 1);
            } else {
                qtest_system_reset(qts);
                config_writel(qts, 0x14, REG_BASE);
                config_writew(qts, 4, 6);
            }
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93), ==, 0);
            check_host_fifo_status(qts, 0, 0, false);
        }
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 3), ==, 0);
        command_dma_no_completion(qts);
        command_dma_prepare(qts, cdb, 6, 6, 6, 4);
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        command_dma_finish(qts, true);
        qtest_quit(qts);
    }
}

static void test_command_dma_migration(void)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };
    g_autofree char *dir = g_dir_make_tmp("aic-command-migrate-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    g_autofree char *uri = g_strdup_printf("file:%s", path);
    QTestState *from = start_selected_disk(NULL);
    QTestState *to;

    command_dma_prepare(from, cdb, 6, 2, 6, 0);
    qtest_writeb(from, REG_BASE + 0x93, 0x3c);
    sgi_migrate_stopped(from, uri);
    to = start_disk_mode("-S -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(host_dma_read_bytes(to, 8, 3), ==, 4);
    g_assert_cmphex(host_dma_read_bytes(to, 0x88, 4), ==, 0x1800a);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 3) & 0xe0, ==, 0x80);
    qtest_writeb(to, REG_BASE + 0x93, 0x34);
    command_dma_count(to, 0x8c, 4);
    qtest_writeb(to, REG_BASE + 0x93, 0x3c);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 3) & 0xe0, ==, 0x40);
    g_assert_cmphex(host_dma_read_bytes(to, 8, 3), ==, 0);
    qtest_quit(to);
    qtest_quit(from);
    for (unsigned field = 0; field < 2; field++) {
        g_autofree char *invalid = g_build_filename(dir, "invalid", NULL);
        g_autofree char *invalid_uri = g_strdup_printf("file:%s", invalid);
        QTestState *bad;

        corrupt_selection_stream(path, invalid,
                                  field ? "cdb_need" : "cdb_len",
                                  field ? 10 : 7);
        bad = start_disk_mode("-S -incoming defer");
        qtest_set_expected_status(bad, EXIT_FAILURE);
        qtest_qmp_assert_success(bad,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}",
            invalid_uri);
        qtest_wait_qemu(bad);
        qtest_quit(bad);
        unlink(invalid);
    }
    unlink(path);
    g_rmdir(dir);
    for (unsigned fifo = 0; fifo < 2; fifo++) {
        QTestState *qts = start_selected_disk(NULL);

        command_dma_prepare(qts, cdb, 6, 6, fifo ? 2 : 6, 0);
        if (!fifo) {
            config_writew(qts, 4, 2);
        }
        qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
        assert_peer_migration_refused(qts);
        qtest_quit(qts);
    }
}

static void test_command_dma_done(void)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };
    QTestState *qts = start_selected_disk(NULL);

    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    command_dma_prepare(qts, cdb, 6, 6, 2, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 0x0c);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 5, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 0x34);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 5, ==, 5);
    qtest_writeb(qts, REG_BASE + 0x93, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 5, ==, 0);
    command_dma_count(qts, 0x8c, 4);
    qtest_writeb(qts, REG_BASE + 0x93, 0x34);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 5, ==, 4);
    command_dma_count(qts, 8, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 5, ==, 0);
    command_dma_no_completion(qts);
    g_assert_false(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 3, 0xc0);
    qtest_writeb(qts, REG_BASE + 0x11, 0x10);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 7, ==, 4);
    qtest_writeb(qts, REG_BASE + 0x87, 6);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0xc, 0x11);
    qtest_writeb(qts, REG_BASE + 0x92, 4);
    g_assert_false(qtest_get_irq(qts, 11));
    qtest_quit(qts);
}

static void test_command_dma_zero_done_irq(void)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };
    QTestState *qts = start_selected_disk(NULL);

    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    command_dma_prepare(qts, cdb, 6, 0, 0, 0);
    qtest_writeb(qts, REG_BASE + 0x10, 4);
    qtest_writeb(qts, REG_BASE + 0x93, 0x14);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 5, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 7, ==, 4);
    qtest_writeb(qts, REG_BASE + 0x87, 6);
    g_assert_true(qtest_get_irq(qts, 11));
    qtest_writeb(qts, REG_BASE + 0x93, 4);
    qtest_writeb(qts, REG_BASE + 0x92, 4);
    g_assert_false(qtest_get_irq(qts, 11));
    command_dma_no_completion(qts);
    qtest_quit(qts);
}

static void test_command_dma_preserves_pio_clears(void)
{
    static const uint8_t ready[6] = { 0 };

    for (unsigned phase = 0; phase < 4; phase++) {
        QTestState *qts = start_disk();

        if (phase == 1) {
            attention_select(qts, 0x48);
        } else {
            manual_command(qts, ready, phase >= 2 ? 6 : 0);
            qtest_writeb(qts, REG_BASE + 1, 8);
            if (phase >= 2) {
                manual_wait_phase(qts, 0xc0);
            }
            if (phase == 3) {
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 6), ==, 2);
                qtest_writeb(qts, REG_BASE + 3, 0xe0);
            }
        }
        for (unsigned clear = 0; clear < 3; clear++) {
            qtest_writeb(qts, REG_BASE + 1, 0);
            qtest_writeb(qts, REG_BASE + 1, 8);
            if (clear != 1) {
                qtest_writeb(qts, REG_BASE + 0xb, 2);
            }
            if (clear != 0) {
                qtest_writeb(qts, REG_BASE + 0xc, 1);
            }
            for (unsigned write = 0; write < 4; write++) {
                qtest_writeb(qts, REG_BASE + 0x93, write & 1 ? 4 : 0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 2, ==,
                                clear == 1 ? 2 : 0);
                g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xc) & 1, ==,
                                clear == 0 ? 1 : 0);
            }
        }
        qtest_quit(qts);
    }
    {
        static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };
        QTestState *qts = start_selected_disk(NULL);

        command_dma_prepare(qts, cdb, 6, 0, 6, 0);
        qtest_writeb(qts, REG_BASE + 0x93, 0x34);
        qtest_writeb(qts, REG_BASE + 0xc, 1);
        for (unsigned write = 0; write < 4; write++) {
            qtest_writeb(qts, REG_BASE + 0x93, write & 1 ? 0x3c : 0x34);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 2, ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xc) & 1, ==, 0);
        }
        qtest_writeb(qts, REG_BASE + 0x93, 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 2, ==, 2);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xc) & 1, ==, 1);
        qtest_quit(qts);
    }
}

static void test_command_dma_inquiry(void)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };
    QTestState *qts = start_selected_disk(NULL);

    manual_command(qts, cdb, 0);
    qtest_memwrite(qts, 0x18004, cdb, sizeof(cdb));
    host_dma_program(qts, 0x18004, sizeof(cdb));
    command_dma_count(qts, 8, sizeof(cdb));
    qtest_writeb(qts, REG_BASE + 0x93, 5);
    qtest_writeb(qts, REG_BASE + 0x93, 0x3c);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x8c, 3), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 8, 3), ==, 0);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==, 0x1800a);
    g_assert_cmphex(host_dma_read_bytes(qts, 0x14, 4), ==, 0x1800a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xb) & 5, ==, 5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 3) & 0xe0, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 3, ==, 0);
    command_dma_finish(qts, true);
    {
        uint8_t program[12];
        size_t n = 0;

        emit_or(program, &n, 0x0d, 0x9d);
        emit_or(program, &n, 2, 0x91);
        emit_or(program, &n, 1, 0x91);
        start_program(qts, program, n);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9d), ==, 0x0d);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 3, ==, 3);
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("aic7880/command-dma/inquiry", test_command_dma_inquiry);
    qtest_add_func("aic7880/command-dma/preserves-pio-clears",
                   test_command_dma_preserves_pio_clears);
    qtest_add_func("aic7880/command-dma/done", test_command_dma_done);
    qtest_add_func("aic7880/command-dma/zero-done-irq",
                   test_command_dma_zero_done_irq);
    qtest_add_func("aic7880/command-dma/offsets", test_command_dma_offsets);
    qtest_add_func("aic7880/command-dma/short-counts",
                   test_command_dma_short_counts);
    qtest_add_func("aic7880/command-dma/lengths", test_command_dma_lengths);
    qtest_add_func("aic7880/command-dma/migration",
                   test_command_dma_migration);
    qtest_add_func("aic7880/command-dma/reset",
                   test_command_dma_reset);
    qtest_add_func("aic7880/command-dma/admission",
                   test_command_dma_admission);
    qtest_add_func("aic7880/command-dma/zero-and-invalid",
                   test_command_dma_zero_and_invalid);
    qtest_add_func("aic7880/command-dma/phase-and-rate",
                   test_command_dma_phase_and_rate);
    qtest_add_func("aic7880/command-dma/pio-ownership",
                   test_command_dma_pio_ownership);
    qtest_add_func("aic7880/command-dma/full-and-empty",
                   test_command_dma_full_and_empty);
    qtest_add_func("aic7880/command-dma/excess-counts",
                   test_command_dma_excess_counts);
    qtest_add_func("aic7880/function-registers/constants",
                   test_function_constants);
    qtest_add_func("aic7880/function-registers/selectors",
                   test_function1_selectors);
    qtest_add_func("aic7880/function-registers/alu", test_function1_alu);
    qtest_add_func("aic7880/function-registers/none-return",
                   test_function_none_return);
    qtest_add_func("aic7880/function-registers/access", test_function_access);
    qtest_add_func("aic7880/function-registers/refusals",
                   test_function_refusals);
    qtest_add_func("aic7880/function-registers/state", test_function1_state);
    qtest_add_func("aic7880/internal-scb-pointer/migrates",
                   test_internal_scb_pointer_migrates);
    qtest_add_func("aic7880/internal-scb-pointer/reset",
                   test_internal_scb_pointer_reset);
    qtest_add_func("aic7880/internal-scb-alias/host",
                   test_internal_scb_alias_host);
    qtest_add_func("aic7880/internal-scb-alias/sequencer",
                   test_internal_scb_alias_sequencer);
    qtest_add_func("aic7880/internal-scb-alias/auto",
                   test_internal_scb_alias_auto);
    qtest_add_func("aic7880/internal-scb-alias/bounds",
                   test_internal_scb_alias_bounds);
    qtest_add_func("aic7880/internal-scb-alias/queue-limits",
                   test_internal_scb_alias_queue_limits);
    qtest_add_func("aic7880/external-scb-capacity", test_external_scb_capacity);
    qtest_add_func("aic7880/external-scb-queues", test_external_scb_queues);
    qtest_add_func("aic7880/external-scb-auto", test_external_scb_auto);
    qtest_add_func("aic7880/external-scb-parity", test_external_scb_parity);
    qtest_add_func("aic7880/external-scb-parity-rmw",
                   test_external_scb_parity_rmw);
    qtest_add_func("aic7880/external-scb-parity-holds-pause",
                   test_external_scb_parity_holds_pause);
    qtest_add_func("aic7880/external-scb-parity-other-errors",
                   test_external_scb_parity_other_errors);
    qtest_add_func("aic7880/external-scb-widths", test_external_scb_widths);
    qtest_add_func("aic7880/external-scb-reset", test_external_scb_reset);
    qtest_add_data_func("aic7880/external-scb-migrates/1k",
                        GUINT_TO_POINTER(1024), test_external_scb_migrates);
    qtest_add_data_func("aic7880/external-scb-migrates/8k",
                        GUINT_TO_POINTER(8192), test_external_scb_migrates);
    qtest_add_func("aic7880/external-scb-configuration",
                   test_external_scb_configuration);
    qtest_add_func("aic7880/external-scb-migration-mismatch",
                   test_external_scb_migration_mismatch);
    qtest_add_func("aic7880/pci-identity", test_identity);
    qtest_add_func("aic7880/wide/registers", test_wide_registers);
    qtest_add_func("aic7880/wide/width-atomicity", test_wide_width_atomicity);
    qtest_add_func("aic7880/wide/target-routing", test_wide_target_routing);
    qtest_add_func("aic7880/wide/own-id", test_wide_own_id);
    qtest_add_func("aic7880/wide/selection-intervals",
                   test_wide_selection_intervals);
    qtest_add_func("aic7880/wide/timer-controls", test_wide_timer_controls);
    qtest_add_func("aic7880/wide/connection", test_wide_connection);
    qtest_add_func("aic7880/wide/resets", test_wide_resets);
    qtest_add_func("aic7880/wide/pending-migration",
                   test_wide_pending_migration);
    qtest_add_func("aic7880/wide/topology-limits", test_wide_topology_limits);
    qtest_add_func("aic7880/wide/selected-reprogramming",
                   test_wide_selected_reprogramming);
    qtest_add_func("aic7880/wide/lun-routing", test_wide_lun_routing);
    qtest_add_func("aic7880/wide/file-data", test_wide_file_data);
    qtest_add_func("aic7880/wide/negotiation", test_wide_negotiation);
    qtest_add_func("aic7880/wide/data-refusal", test_wide_data_refusal);
    qtest_add_func("aic7880/wide/status-migration", test_wide_status_migration);
    qtest_add_func("aic7880/wide/invalid-migration",
                   test_wide_invalid_migration);
    qtest_add_func("aic7880/wide/unselected-and-incoming",
                   test_wide_unselected_and_incoming);
    qtest_add_func("aic7880/wide/controller-isolation",
                   test_wide_controller_isolation);
    qtest_add_func("aic7880/dspcistatus-selector",
                   test_dspcistatus_selector);
    qtest_add_func("aic7880/dspcistatus-resets", test_dspcistatus_resets);
    qtest_add_func("aic7880/dspcistatus-migrates", test_dspcistatus_migrates);
    qtest_add_func("aic7880/dspcistatus-sequencer-rejected",
                   test_dspcistatus_rejects_sequencer_access);
    qtest_add_func("aic7880/dspcistatus-host-words-rejected",
                   test_dspcistatus_rejects_host_words);
    qtest_add_func("aic7880/host-dfdat-byte-port", test_host_dfdat_byte_port);
    qtest_add_func("aic7880/host-address-loads-scsi-address",
                   test_host_address_loads_scsi_address);
    qtest_add_func("aic7880/reserved-host-count-byte",
                   test_reserved_host_count_byte);
    qtest_add_func("aic7880/reserved-host-count-sequencer-reads",
                   test_reserved_host_count_sequencer_reads);
    qtest_add_func("aic7880/reserved-host-count-sequencer-writes",
                   test_reserved_host_count_sequencer_writes);
    qtest_add_func("aic7880/host-dma-memory-to-fifo",
                   test_host_dma_memory_to_fifo);
    qtest_add_func("aic7880/host-dma-four-byte-offset",
                   test_host_dma_four_byte_offset);
    qtest_add_func("aic7880/host-dma-offset-four-28",
                   test_host_dma_offset_four_28);
    qtest_add_func("aic7880/host-fifo-partial-words",
                   test_host_fifo_partial_words);
    qtest_add_func("aic7880/fifo-start-thresholds", test_fifo_start_thresholds);
    qtest_add_func("aic7880/fifo-threshold-pending-admission",
                   test_fifo_threshold_pending_admission);
    qtest_add_func("aic7880/fifo-threshold-migrates",
                   test_fifo_threshold_migrates);
    qtest_add_func("aic7880/fifo-threshold-selector-change",
                   test_fifo_threshold_selector_change);
    qtest_add_func("aic7880/host-dma-fifo-to-memory",
                   test_host_dma_fifo_to_memory);
    qtest_add_func("aic7880/host-dma-short-word-flush",
                   test_host_dma_short_word_flush);
    qtest_add_func("aic7880/host-dma-short-word-fault",
                   test_host_dma_short_word_fault);
    qtest_add_func("aic7880/host-dma-short-word-master-enable",
                   test_host_dma_short_word_master_enable);
    qtest_add_func("aic7880/host-dma-write-offsets",
                   test_host_dma_write_offsets);
    qtest_add_func("aic7880/host-dma-write-capacity",
                   test_host_dma_write_capacity);
    qtest_add_func("aic7880/host-dma-write-failed-prefix",
                   test_host_dma_write_failed_prefix);
    qtest_add_func("aic7880/host-dma-short-reads", test_host_dma_short_reads);
    qtest_add_func("aic7880/host-dma-address-endpoint",
                   test_host_dma_address_endpoint);
    qtest_add_func("aic7880/host-dma-zero-count", test_host_dma_zero_count);
    qtest_add_func("aic7880/host-dma-pci-command-inhibits",
                   test_host_dma_pci_command_inhibits);
    qtest_add_func("aic7880/host-dma-fault", test_host_dma_fault);
    qtest_add_func("aic7880/host-dma-reset-pending",
                   test_host_dma_reset_pending);
    qtest_add_func("aic7880/host-dma-pci-reset-pending",
                   test_host_dma_pci_reset_pending);
    qtest_add_func("aic7880/host-dma-read-offsets", test_host_dma_read_offsets);
    qtest_add_func("aic7880/host-dma-read-capacity",
                   test_host_dma_read_capacity);
    qtest_add_func("aic7880/host-dfdat-rejects-mixed-scsi",
                   test_host_dfdat_rejects_mixed_scsi);
    qtest_add_func("aic7880/host-dfdat-rejects-underrun",
                   test_host_dfdat_rejects_underrun);
    qtest_add_func("aic7880/host-dfdat-rejects-overrun",
                   test_host_dfdat_rejects_overrun);
    qtest_add_func("aic7880/host-dma-done-clears", test_host_dma_done_clears);
    qtest_add_func("aic7880/host-dma-rejects-high-address-selection",
                   test_host_dma_rejects_high_address_selection);
    qtest_add_func("aic7880/host-dfdat-rejects-sfunct",
                   test_host_dfdat_rejects_sfunct);
    qtest_add_func("aic7880/host-dfdat-migrates", test_host_dfdat_migrates);
    qtest_add_func("aic7880/host-dma-read-migrates",
                   test_host_dma_read_migrates);
    qtest_add_func("aic7880/host-dma-active-migration-refused",
                   test_host_dma_active_migration_refused);
    qtest_add_func("aic7880/host-dfdat-sequencer-port",
                   test_host_dfdat_sequencer_port);
    qtest_add_func("aic7880/host-fifo-rejects-scsi-selection",
                   test_host_fifo_rejects_scsi_selection);
    qtest_add_func("aic7880/o2-sibling-policy-default",
                   test_o2_sibling_policy_default);
    qtest_add_func("aic7880/o2-sibling-bar-repeats-last-slice",
                   test_o2_sibling_bar_repeats_last_slice);
    qtest_add_func("aic7880/o2-sibling-command-parity-readback",
                   test_o2_sibling_command_parity_readback);
    qtest_add_func("aic7880/o2-sibling-policy-requires-qtest",
                   test_o2_sibling_policy_requires_qtest);
    qtest_add_func("aic7880/o2-storage-requires-machine",
                   test_o2_storage_diagnostic_requires_machine);
    qtest_add_func("aic7880/o2-diagnostics-exclusive",
                   test_o2_diagnostics_are_exclusive);
    qtest_add_func("aic7880/sequencer-upload", test_sequencer_upload);
    qtest_add_func("aic7880/workload-upload-1696",
                   test_workload_upload_1696);
    qtest_add_func("aic7880/workload-upload-1708",
                   test_workload_upload_1708);
    qtest_add_func("aic7880/workload-dma-scb-branch-clear",
                   test_workload_dma_scb_branch_clear);
    qtest_add_func("aic7880/workload-dma-scb-branch-set",
                   test_workload_dma_scb_branch_set);
    qtest_add_func("aic7880/internal-refusal-pci-status-clear",
                   test_internal_refusal_keeps_pci_status_clear);
    qtest_add_func("aic7880/pci-error-status-survives-chip-reset",
                   test_pci_error_status_survives_chip_reset);
    qtest_add_func("aic7880/host-faildis-readback", test_host_faildis_readback);
    qtest_add_func("aic7880/pci-error-faildis", test_pci_error_faildis);
    qtest_add_func("aic7880/host-pausedis-refusal", test_host_pausedis_refusal);
    qtest_add_func("aic7880/internal-completion-ids",
                   test_internal_completion_ids);
    qtest_add_func("aic7880/deferred-dma-preserves-registers",
                   test_deferred_dma_preserves_registers);
    qtest_add_func("aic7880/diagnostic-unpause-cannot-complete",
                   test_diagnostic_unpause_cannot_complete);
    qtest_add_func("aic7880/sequencer-host-completion-memory",
                   test_sequencer_host_completion_memory);
    qtest_add_func("aic7880/workload-host-only-negative",
                   test_workload_host_only_negative);
    qtest_add_func("aic7880/sequencer-exec", test_sequencer_exec);
    qtest_add_func("aic7880/old-sequencer-format",
                   test_old_sequencer_format);
    qtest_add_func("aic7880/old-shift-one", test_old_shift_one);
    qtest_add_func("aic7880/old-rol-two", test_old_rol_two);
    qtest_add_func("aic7880/old-shift-eight", test_old_shift_eight);
    qtest_add_func("aic7880/old-shift-return", test_old_shift_return);
    qtest_add_func("aic7880/old-shift-unsupported-control",
                   test_old_shift_unsupported_control);
    qtest_add_func("aic7880/old-shift-rejected-source",
                   test_old_shift_rejected_source);
    qtest_add_func("aic7880/old-alu-bytes", test_old_alu_bytes);
    qtest_add_func("aic7880/old-alu-accumulator",
                   test_old_alu_accumulator);
    qtest_add_func("aic7880/old-xor-return", test_old_xor_return);
    qtest_add_func("aic7880/old-alu-flags", test_old_alu_flags);
    qtest_add_func("aic7880/old-adc-carry", test_old_adc_carry);
    qtest_add_func("aic7880/old-flags-reset-host-write",
                   test_old_flags_reset_and_host_write);
    qtest_add_func("aic7880/old-flags-sequencer-write",
                   test_old_flags_sequencer_write);
    qtest_add_func("aic7880/old-flags-migrate", test_old_flags_migrate);
    qtest_add_func("aic7880/old-alu-none-flags",
                   test_old_alu_none_flags);
    qtest_add_func("aic7880/old-alu-host-only-source",
                   test_old_alu_host_only_source);
    qtest_add_func("aic7880/old-alu-readonly-destination",
                   test_old_alu_readonly_destination);
    qtest_add_func("aic7880/public-sequencer-format",
                   test_public_sequencer_format);
    qtest_add_func("aic7880/sequencer-clocked-resume",
                   test_sequencer_clocked_resume);
    qtest_add_func("aic7880/sequencer-self-branch-resumes",
                   test_sequencer_self_branch_resumes);
    qtest_add_func("aic7880/sequencer-self-branch-cancel",
                   test_sequencer_self_branch_cancel);
    qtest_add_func("aic7880/sequencer-queue-wakes",
                   test_sequencer_queue_wakes);
    qtest_add_func("aic7880/sequencer-clocked-cancel",
                   test_sequencer_clocked_cancel);
    qtest_add_func("aic7880/sequencer-reset-restarts-after-pause",
                   test_sequencer_reset_restarts_after_pause);
    qtest_add_func("aic7880/sequencer-clocked-keeps-pc",
                   test_sequencer_clocked_keeps_pc);
    qtest_add_func("aic7880/sequencer-clocked-migrates",
                   test_sequencer_clocked_migrates);
    qtest_add_func("aic7880/old-branch-high-address",
                   test_old_branch_high_address);
    qtest_add_func("aic7880/sequencer-interrupt-code",
                   test_sequencer_interrupt_code);
    qtest_add_func("aic7880/or-return", test_or_return);
    qtest_add_func("aic7880/or-return-pauses", test_or_return_pauses_at_caller);
    qtest_add_func("aic7880/or-return-without-call",
                   test_or_return_without_call);
    qtest_add_func("aic7880/scb-pages", test_scb_pages_and_sequencer_access);
    qtest_add_func("aic7880/scb-auto", test_scb_auto_addressing);
    qtest_add_func("aic7880/queue-in-trace", test_queue_in_trace);
    qtest_add_func("aic7880/queue-in", test_queue_in_requires_sequencer);
    qtest_add_func("aic7880/indirect-source",
                   test_indirect_source_advances_index);
    qtest_add_func("aic7880/indirect-sequencer-sources",
                   test_indirect_sequencer_sources);
    qtest_add_func("aic7880/sequencer-fault-trace",
                   test_sequencer_fault_trace);
    qtest_add_func("aic7880/dindex-latch", test_dindex_latch);
    qtest_add_func("aic7880/dindir-scratch-writes",
                   test_dindir_scratch_writes);
    qtest_add_func("aic7880/dindir-independent-pointers",
                   test_dindir_independent_pointers);
    qtest_add_func("aic7880/dindir-equal-scratch-indexes",
                   test_dindir_equal_scratch_indexes);
    qtest_add_func("aic7880/dindir-equal-haddr-indexes",
                   test_dindir_equal_haddr_indexes);
    qtest_add_func("aic7880/dindir-equal-sblkctl-unsupported",
                   test_dindir_equal_sblkctl_unsupported);
    qtest_add_func("aic7880/dindir-equal-hcnt-unsupported",
                   test_dindir_equal_hcnt_unsupported);
    qtest_add_func("aic7880/dindir-special-destinations",
                   test_dindir_special_destinations);
    qtest_add_func("aic7880/dindir-access-restrictions",
                   test_dindir_access_restrictions);
    qtest_add_func("aic7880/dindir-rejects-sindex-target",
                   test_dindir_rejects_sindex_target);
    qtest_add_func("aic7880/dindir-rejects-dindex-target",
                   test_dindir_rejects_dindex_target);
    qtest_add_func("aic7880/dindir-unmodeled-target",
                   test_dindir_unmodeled_target);
    qtest_add_func("aic7880/dindex-resets", test_dindex_resets);
    qtest_add_func("aic7880/dindir-migrates", test_dindir_migrates);
    qtest_add_func("aic7880/queue-reset", test_queue_reset_and_reuse);
    qtest_add_func("aic7880/conditional-branches",
                   test_conditional_branches);
    qtest_add_func("aic7880/jc-branch-transfer",
                   test_jc_branch_transfer);
    qtest_add_func("aic7880/jnc-branch-transfer",
                   test_jnc_branch_transfer);
    qtest_add_func("aic7880/accumulator-immediate",
                   test_accumulator_immediate_source);
    qtest_add_func("aic7880/rejected-branch-source",
                   test_rejected_branch_does_not_read_source);
    qtest_add_func("aic7880/return-write-pauses",
                   test_return_write_pauses_at_caller);
    qtest_add_func("aic7880/failed-return-stays",
                   test_failed_return_keeps_subroutine_pc);
    qtest_add_func("aic7880/return-cannot-reset-host-control",
                   test_return_cannot_reset_host_control);
    qtest_add_func("aic7880/host-completion-ownership",
                   test_sequencer_cannot_pop_host_completion);
    qtest_add_func("aic7880/other-host-only-sources",
                   test_other_host_only_sequencer_sources);
    qtest_add_func("aic7880/host-interrupt-ownership",
                   test_sequencer_cannot_clear_host_interrupt);
    qtest_add_func("aic7880/assembler-return-none",
                   test_assembler_return_to_none);
    qtest_add_func("aic7880/jmp-transfer", test_jmp_transfer);
    qtest_add_func("aic7880/call-transfer", test_call_transfer);
    qtest_add_func("aic7880/branch-host-only-source",
                   test_branch_host_only_source);
    qtest_add_func("aic7880/call-pointer-arithmetic",
                   test_call_pointer_arithmetic);
    qtest_add_func("aic7880/four-nested-calls", test_four_nested_calls);
    qtest_add_func("aic7880/circular-call-stack", test_circular_call_stack);
    qtest_add_func("aic7880/paused-call-migrates",
                   test_paused_call_migrates);
    qtest_add_func("aic7880/breakpoint",
                   test_breakpoint_pause_and_interrupt);
    qtest_add_func("aic7880/breakpoint-mask-after-error",
                   test_breakpoint_mask_after_error);
    qtest_add_func("aic7880/breakpoint-address-bits",
                   test_breakpoint_address_bits);
    qtest_add_func("aic7880/breakpoint-mask-migrates",
                   test_breakpoint_mask_migrates);
    qtest_add_func("aic7880/pci-error-mask-migrates",
                   test_pci_error_mask_migrates);
    qtest_add_func("aic7880/status-message-phases",
                   test_status_message_phases);
    qtest_add_func("aic7880/checkpoint-expires-at-backend-buffer",
                   test_checkpoint_expires_at_backend_buffer);
    qtest_add_func("aic7880/rate-change-refuses-pending-data",
                   test_rate_change_refuses_pending_data);
    qtest_add_func("aic7880/expected-phase-reset-and-bus-free",
                   test_expected_phase_clears_on_reset_and_bus_free);
    qtest_add_func("aic7880/pio-wrong-direction", test_pio_wrong_direction);
    qtest_add_func("aic7880/nonzero-rate-refuses-data",
                   test_nonzero_rate_refuses_data);
    qtest_add_func("aic7880/peer-disconnect-boundaries",
                   test_peer_disconnect_boundaries);
    qtest_add_func("aic7880/message-refused-lengths",
                   test_message_refused_lengths);
    qtest_add_func("aic7880/peer-disconnected-nexus",
                   test_peer_disconnected_nexus);
    qtest_add_func("aic7880/pending-reject-migrates",
                   test_pending_reject_migrates);
    qtest_add_func("aic7880/pio-incoming-phase-qualification",
                   test_pio_incoming_phase_qualification);
    qtest_add_func("aic7880/software-attention-precommand",
                   test_software_attention_precommand);
    qtest_add_func("aic7880/message-negotiation-reject",
                   test_message_negotiation_reject);
    qtest_add_func("aic7880/pio-request-phase-qualification",
                   test_pio_request_phase_qualification);
    qtest_add_func("aic7880/attention-selection-phase",
                   test_attention_selection_phase);
    qtest_add_func("aic7880/attention-identify-clear-order",
                   test_attention_identify_clear_order);
    qtest_add_func("aic7880/attention-identify-notifies-pio-ready",
                   test_attention_identify_notifies_pio_ready);
    qtest_add_func("aic7880/attention-rearm-after-bus-free",
                   test_attention_rearm_after_bus_free);
    qtest_add_func("aic7880/attention-full-template-without-peer",
                   test_attention_full_template_without_peer);
    qtest_add_func("aic7880/peer-reselection-status-and-irq",
                   test_peer_reselection_status_and_irq);
    qtest_add_func("aic7880/peer-data-in-parity",
                   test_peer_data_in_parity);
    qtest_add_func("aic7880/peer-parity-latches-across-bus-free",
                   test_peer_parity_latches_across_bus_free);
    qtest_add_func("aic7880/peer-channel-clear-preserves-held-data",
                   test_peer_channel_clear_preserves_held_data);
    qtest_add_func("aic7880/peer-bad-sample-survives-bus-free",
                   test_peer_bad_sample_survives_bus_free);
    qtest_add_func("aic7880/peer-reselection-rejects-selection-and-wide-id",
                   test_peer_reselection_rejects_selection_and_wide_id);
    qtest_add_func("aic7880/peer-reselection-mask-and-busy",
                   test_peer_reselection_mask_and_busy);
    qtest_add_func("aic7880/peer-parity-irq-and-resets",
                   test_peer_parity_irq_and_resets);
    qtest_add_func("aic7880/peer-status-fault-stops-completion",
                   test_peer_status_fault_stops_completion);
    qtest_add_func("aic7880/peer-message-in-parity",
                   test_peer_message_in_parity);
    qtest_add_func("aic7880/peer-migration-boundary",
                   test_peer_migration_boundary);
    qtest_add_func("aic7880/attention-rejects-messages",
                   test_attention_rejects_messages);
    qtest_add_func("aic7880/attention-rejects-controls",
                   test_attention_rejects_controls);
    qtest_add_func("aic7880/attention-no-target-and-resets",
                   test_attention_no_target_and_resets);
    qtest_add_func("aic7880/attention-pending-migration-refused",
                   test_attention_pending_migration_refused);
    qtest_add_func("aic7880/spioen-gates-input",
                   test_spioen_gates_input_phases);
    qtest_add_func("aic7880/dfon-latch-and-reset",
                   test_dfon_latch_and_reset);
    qtest_add_func("aic7880/dfon-rejects-other-controls",
                   test_dfon_rejects_other_controls);
    qtest_add_func("aic7880/direction-latch",
                   test_direction_latches_while_dma_enabled);
    qtest_add_func("aic7880/transfer-control-resets",
                   test_transfer_control_resets);
    qtest_add_func("aic7880/partial-fifo-flush", test_partial_fifo_flush);
    qtest_add_func("aic7880/auto-fifo-flush",
                   test_auto_fifo_flush_after_host_dma_resumes);
    qtest_add_func("aic7880/auto-flush-disabled",
                   test_auto_flush_disable_keeps_manual_flush);
    qtest_add_func("aic7880/auto-flush-pci-error",
                   test_auto_flush_failed_pci_write);
    qtest_add_func("aic7880/auto-flush-phase-mismatch",
                   test_phase_mismatch_requests_fifo_flush);
    qtest_add_func("aic7880/auto-flush-phase-disabled",
                   test_phase_mismatch_disable_requires_manual_flush);
    qtest_add_func("aic7880/auto-flush-phase-host-active",
                   test_phase_mismatch_flushes_with_host_dma_active);
    qtest_add_func("aic7880/auto-flush-reset",
                   test_auto_flush_disable_reset);
    qtest_add_func("aic7880/phase-change-without-mismatch",
                   test_phase_change_without_mismatch_is_unsupported);
    qtest_add_func("aic7880/partial-fifo-unaligned-host-address",
                   test_partial_fifo_unaligned_host_address);
    qtest_add_func("aic7880/partial-fifo-flush-while-dma-disabled",
                   test_partial_fifo_flush_while_dma_disabled);
    qtest_add_func("aic7880/partial-fifo-flush-after-direction-change",
                   test_partial_fifo_flush_after_direction_change);
    qtest_add_func("aic7880/partial-fifo-flush-without-bus-master",
                   test_partial_fifo_flush_without_bus_master);
    qtest_add_func("aic7880/partial-fifo-reset", test_partial_fifo_reset);
    qtest_add_func("aic7880/pending-fifo-chip-reset",
                   test_pending_fifo_chip_reset);
    qtest_add_func("aic7880/partial-fifo-clear-channel",
                   test_partial_fifo_clear_channel);
    qtest_add_func("aic7880/setup-control-readback-and-reset",
                   test_setup_control_readback_and_reset);
    qtest_add_func("aic7880/setup-selection-timeout-mask",
                   test_setup_selection_timeout_mask);
    qtest_add_func("aic7880/selection-timeout-scsiint-latch",
                   test_selection_timeout_scsiint_latch);
    qtest_add_func("aic7880/bus-free-scsiint-latch",
                   test_bus_free_scsiint_latch);
    qtest_add_func("aic7880/selection-timeout-scsiint-latch-migrates",
                   test_selection_timeout_scsiint_latch_migrates);
    qtest_add_func("aic7880/selection-timer-intervals",
                   test_selection_timer_intervals);
    qtest_add_func("aic7880/selection-timer-late-enable",
                   test_selection_timer_late_enable);
    qtest_add_func("aic7880/selection-timer-reconfigure",
                   test_selection_timer_reconfigure);
    qtest_add_func("aic7880/selection-timer-migrates",
                   test_selection_timer_migrates);
    qtest_add_func("aic7880/selection-timer-cancel",
                   test_selection_timer_cancel);
    qtest_add_func("aic7880/unmasked-selection-timeout",
                   test_unmasked_selection_timeout);
    qtest_add_func("aic7880/unsupported-scsi-controls",
                   test_unsupported_scsi_controls);
    qtest_add_func("aic7880/manual-command-unsupported",
                   test_manual_command_path_is_unsupported);
    qtest_add_func("aic7880/reset-and-interrupt",
                   test_reset_and_interrupt_level);
    qtest_add_func("aic7880/reset-posted-completion",
                   test_reset_discards_posted_completion);
    qtest_add_func("aic7880/missing-target", test_missing_target);
    qtest_add_func("aic7880/bad-opcode", test_bad_sequencer_opcode);
    qtest_add_func("aic7880/pci-admission-keeps-abort-pending",
                   test_pci_admission_keeps_abort_pending);
    qtest_add_func("aic7880/coupled-pci-admission",
                   test_coupled_pci_admission);
    qtest_add_func("aic7880/dma-requires-bus-master",
                   test_dma_requires_bus_master);
    qtest_add_func("aic7880/identify-lun-routing", test_identify_lun_routing);
    qtest_add_func("aic7880/narrow-connection-id-eight-times-out",
                   test_narrow_connection_id_eight_times_out);
    qtest_add_func("aic7880/lun-nexus-migrates-before-cdb",
                   test_lun_nexus_migrates_before_cdb);
    qtest_add_func("aic7880/inquiry-via-sequencer", test_inquiry_via_sequencer);
    qtest_add_func("aic7880/selected-inquiry-first-segment",
                   test_selected_inquiry_first_segment);
    qtest_add_func("aic7880/selected-inquiry-buffer-migration-refused",
                   test_selected_inquiry_buffer_migration_refused);
    qtest_add_func("aic7880/selected-inquiry-second-segment-fault",
                   test_selected_inquiry_second_segment_fault);
    qtest_add_func("aic7880/selected-inquiry-two-segments",
                   test_selected_inquiry_two_segments);
    qtest_add_func("aic7880/short-inquiry-phase-mismatch-flush",
                   test_short_inquiry_phase_mismatch_flush);
    qtest_add_func("aic7880/short-inquiry-reset-reuses-target",
                   test_short_inquiry_reset_reuses_target);
    qtest_add_func("aic7880/short-inquiry-target-requires-qtest",
                   test_short_inquiry_target_requires_qtest);
    qtest_add_func("aic7880/read-write-via-sequencer",
                   test_read_write_via_sequencer);
    qtest_add_func("aic7880/read-callback-buffers-stream",
                   test_read_callback_buffers_stream);
    qtest_add_func("aic7880/read-write-two-segments",
                   test_read_write_two_segments);
    qtest_add_func("aic7880/empty-fifo-flush-during-request",
                   test_empty_fifo_flush_during_request);
    qtest_add_func("aic7880/coupled-read-host-stall",
                   test_coupled_read_host_stall);
    qtest_add_func("aic7880/coupled-cd-host-stall",
                   test_coupled_cd_host_stall);
    qtest_add_func("aic7880/coupled-fifo-reset-pending",
                   test_coupled_fifo_reset_pending);
    qtest_add_func("aic7880/coupled-write-prefetch",
                   test_coupled_write_prefetch);
    qtest_add_func("aic7880/coupled-read-failed-prefix",
                   test_coupled_read_failed_prefix);
    qtest_add_func("aic7880/coupled-write-failed-read",
                   test_coupled_write_failed_read);
    qtest_add_func("aic7880/coupled-zero-data-keeps-host-count",
                   test_coupled_zero_data_keeps_host_count);
    qtest_add_func("aic7880/scsi-reset-out", test_scsi_reset_out);
    qtest_add_func("aic7880/independent-functions", test_independent_functions);
    qtest_add_func("aic7880/reset-cancels-busy",
                   test_reset_cancels_busy_request);
    qtest_add_func("aic7880/pci-reset-cancels-busy",
                   test_pci_reset_cancels_busy_request);
    qtest_add_func("aic7880/paused-state-migrates", test_paused_state_migrates);
    qtest_add_func("aic7880/manual/status-ack", test_manual_status_ack);
    qtest_add_func("aic7880/manual/signals", test_manual_signals);
    qtest_add_func("aic7880/manual/inquiry", test_manual_inquiry);
    qtest_add_func("aic7880/manual/modes", test_manual_modes);
    qtest_add_func("aic7880/manual/mode-exclusion", test_manual_mode_exclusion);
    qtest_add_func("aic7880/manual/sample-channel-clear",
                   test_manual_sample_channel_clear);
    qtest_add_func("aic7880/manual/file-data", test_manual_file_data);
    qtest_add_func("aic7880/manual/phase-ack-order",
                   test_manual_phase_ack_order);
    qtest_add_func("aic7880/manual/interrupts", test_manual_interrupts);
    qtest_add_func("aic7880/manual/channel-counters",
                   test_manual_channel_counters);
    qtest_add_func("aic7880/manual/automatic-channel-clear",
                   test_automatic_channel_clear);
    qtest_add_func("aic7880/manual/automatic-channel-mode-change",
                   test_automatic_channel_mode_change);
    qtest_add_func("aic7880/manual/refusals", test_manual_refusals);
    qtest_add_func("aic7880/manual/rate-refusals", test_manual_rate_refusals);
    qtest_add_func("aic7880/manual/unpaused-host", test_manual_unpaused_host);
    qtest_add_func("aic7880/manual/target-delay", test_manual_target_delay);
    qtest_add_func("aic7880/manual/resets", test_manual_resets);
    qtest_add_func("aic7880/manual/pending-migration",
                   test_manual_pending_migration);
    qtest_add_func("aic7880/manual/host-command-refused",
                   test_manual_host_command_refused);
    qtest_add_func("aic7880/manual/dma-owned-refused",
                   test_manual_dma_owned_refused);
    qtest_add_func("aic7880/manual/rate-before-callback",
                   test_manual_rate_before_callback);
    qtest_add_func("aic7880/manual/unrelated-interrupt",
                   test_manual_unrelated_interrupt);
    qtest_add_func("aic7880/manual/disconnected-refused",
                   test_manual_disconnected_refused);
    qtest_add_func("aic7880/manual/nonempty-fifo-refused",
                   test_manual_nonempty_fifo_refused);
    qtest_add_func("aic7880/manual/selection-bus-free",
                   test_manual_selection_bus_free);
    qtest_add_func("aic7880/manual/attention-atomic",
                   test_manual_attention_atomic);
    qtest_add_func("aic7880/manual/attention-precommand",
                   test_manual_attention_precommand);
    qtest_add_func("aic7880/manual/attention-incoming",
                   test_manual_attention_incoming);
    qtest_add_func("aic7880/manual/attention-outgoing",
                   test_manual_attention_outgoing);
    qtest_add_func("aic7880/manual/attention-late-cdb",
                   test_manual_attention_late_cdb);
    qtest_add_func("aic7880/manual/attention-disconnected",
                   test_manual_attention_disconnected);
    qtest_add_func("aic7880/manual/attention-ack-refused",
                   test_manual_attention_ack_refused);
    qtest_add_func("aic7880/manual/attention-resets",
                   test_manual_attention_resets);
    return g_test_run();
}
