/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Register tests for a standalone AIC-7880 PCI function. */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include <gio/gio.h>
#include "libqtest.h"
#include "memtx.h"
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

static QTestState *start_aic(void)
{
    QTestState *qts = qtest_init("-M pc -m 128M -nodefaults "
                                 "-device aic7880,id=hba,bus=pci.0,addr=04.0");

    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    return qts;
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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
                            ==, 0x40);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0x08);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
    qtest_quit(qts);
}

static void test_host_dfdat_rejects_underrun(void)
{
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x93, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x99), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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
    qtest_writeb(qts, REG_BASE + 0x87, 0x02);
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
        g_assert_cmphex(qtest_readb(qts, REG_BASE + cases[i].dest), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 0x04);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x63), ==, 0);
        qtest_quit(qts);
    }
}

static QTestState *start_disk(void)
{
    QTestState *qts = qtest_init("-M pc -m 128M -nodefaults "
                                 "-drive if=none,id=disk0,file=null-co://,"
                                 "format=raw "
                                 "-device aic7880,id=hba,bus=pci.0,addr=04.0 "
                                 "-device scsi-hd,drive=disk0,bus=hba.0,"
                                 "scsi-id=0,lun=0");

    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    return qts;
}

static QTestState *start_selected_disk(const char *trace_path)
{
    g_autofree char *trace = trace_path ?
        g_strdup_printf("-trace enable=scsi_disk_new_request,file=%s",
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

static size_t build_command_program_mode(uint8_t *program, const uint8_t *cdb,
                                         size_t cdb_len, bool write,
                                         bool host_dma)
{
    size_t n = 0;
    unsigned wait_at = 7 + cdb_len;
    unsigned idle_at = wait_at + 6;

    g_assert_cmpuint(cdb_len, <=, 16);
    emit_or(program, &n, 0x08, 0x01); /* automatic PIO */
    emit_or(program, &n, 0x07, 0x05); /* initiator 7, target 0 */
    emit_or(program, &n, 0, 0x93); /* disable the old data path */
    emit_or(program, &n, (write ? 0x34 : 0x30) | (host_dma ? 8 : 0), 0x93);
    emit(program, &n, alu_word(1, 0xff, 0x6a, 0x00, false));
    emit_or(program, &n, 0x40, 0x00); /* start selection */
    for (size_t i = 0; i < cdb_len; i++) {
        emit_or(program, &n, cdb[i], 0x06);
    }
    emit_or(program, &n, (write ? 0x34 : 0x30) | (host_dma ? 10 : 0), 0x93);
    emit(program, &n, branch_word(0xf, 2, 0x0b, wait_at));
    emit(program, &n, alu_word(0, 0, 0x06, 0x22, false)); /* status */
    emit(program, &n, alu_word(0, 0, 0x06, 0x23, false)); /* message */
    emit(program, &n, branch_word(0xf, 8, 0x0c, wait_at + 3));
    emit_or(program, &n, 1, 0x9d); /* completion identifier */
    emit_or(program, &n, 2, 0x91); /* completion interrupt */
    emit(program, &n, branch_word(8, 0, 0x65, idle_at));
    return n;
}

static size_t build_command_program(uint8_t *program, const uint8_t *cdb,
                                    size_t cdb_len, bool write)
{
    return build_command_program_mode(program, cdb, cdb_len, write, true);
}

static void start_program(QTestState *qts, const uint8_t *program, size_t len)
{
    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x63, 0);
    for (size_t i = 0; i < len; i++) {
        qtest_writeb(qts, REG_BASE + 0x61, program[i]);
    }
    qtest_writeb(qts, REG_BASE + 0x60, 0x90);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x63, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 0x02);
}

static size_t build_selected_inquiry_program(uint8_t *program)
{
    static const uint8_t cdb[] = { 0x12, 0, 0, 0, 36, 0 };
    size_t n = 0;

    emit_or(program, &n, 0x08, 0x01);
    emit_or(program, &n, 0x07, 0x05);
    emit_or(program, &n, 0x30, 0x93);
    emit_or(program, &n, 0x5a, 0x00);
    emit_or(program, &n, 0x80, 0x06);
    emit_or(program, &n, 0x40, 0x0c);
    for (size_t i = 0; i < sizeof(cdb); i++) {
        if (cdb[i]) {
            emit_or(program, &n, cdb[i], 0x06);
        } else {
            emit(program, &n, alu_word(1, 0xff, 0x6a, 0x06, false));
        }
    }
    emit_or(program, &n, 1, 0x91);
    return n;
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
    qtest_writeb(qts, REG_BASE + 0x87, 0x02);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, first);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x21), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x63), ==, 0);

    qtest_writeb(qts, REG_BASE + 0x62, (slots - 1) & 0xff);
    qtest_writeb(qts, REG_BASE + 0x63, (slots - 1) >> 8);
    qtest_writeb(qts, REG_BASE + 0x87, 0x02);
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
    qtest_writeb(qts, REG_BASE + 0x87, 0x02);
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
    qtest_writeb(qts, REG_BASE + 0x87, 0x02);
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

static void test_pci_error_status_survives_chip_reset(void)
{
    QTestState *qts = start_aic();

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/hba','property':'x-test-pci-error',"
        "'value':'received-master-abort'}}");
    g_assert_cmphex(config_readl(qts, 0x04) >> 16 & 0x2000, ==, 0x2000);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0x40);
    qtest_writeb(qts, REG_BASE + 0x92, 0x1f);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0x40);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(config_readl(qts, 0x04) >> 16 & 0x2000, ==, 0x2000);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0x40);
    config_writew(qts, 0x06, 0x2000);
    g_assert_cmphex(config_readl(qts, 0x04) >> 16 & 0x2000, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
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
            qtest_writeb(qts, REG_BASE + 0x87, 2);
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
            config_writew(qts, 0x06, 0x2000);
            g_assert_cmphex(config_readl(qts, 0x04) >> 16 & 0x2000, ==, 0);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
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
            uint8_t program[4];
            uint8_t control = 0x90 | (faildis << 5);
            size_t n = 0;

            emit(program, &n, branch_word(8, 0, 0x65, 0));
            start_program(qts, program, n);
            qtest_writeb(qts, REG_BASE + 0x87, 4);
            qtest_writeb(qts, REG_BASE + 0x60, control);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x60), ==, control);
            qtest_writeb(qts, REG_BASE + 0x60, control | controls[i] | 4);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x60), ==, control);
            qtest_writeb(qts, REG_BASE + 0x87, 2);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 0);
            qtest_writeb(qts, REG_BASE + 0x87, 4);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
            g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
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
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x66), ==, 0x9d);
        qtest_quit(qts);
    }
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
    qtest_writeb(qts, REG_BASE + 0x87, 0x02);
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
    uint8_t program[12] = { 0 };
    size_t n = 0;

    emit_or(program, &n, 0x07, 0x05);
    emit_or(program, &n, control, 0x00);
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
}

static void attention_write(QTestState *qts, uint8_t reg, uint8_t value)
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

static void attention_complete_test_unit_ready(QTestState *qts)
{
    uint8_t command[7 * 4] = { 0 };
    uint8_t finish[5 * 4] = { 0 };
    size_t n = 0;

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
    emit(finish, &n, alu_word(0, 0, 0x06, 0x22, false));
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
    qtest_quit(qts);

    qts = start_aic();
    n = 0;
    qtest_writeb(qts, REG_BASE + 0x66, 0x8f);
    emit(program, &n, alu_word(1, 0xa5, 0x69, 0x6d, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x4b), ==, 0xcc);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 0x81);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0x08);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x30);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 0x81);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x4c), ==, 0xdd);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0x08);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 8);
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
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
        qtest_writeb(qts, REG_BASE + 0x46, 0xa5);
        qtest_writeb(qts, REG_BASE + 0x87, 0x04);
        qtest_writeb(qts, REG_BASE + 0x92, 1);
        n = 0;
        emit(rejected, &n, alu_word(op, 2, 0x9d, 0x46, false));
        emit_or(rejected, &n, 1, 0x91);
        start_program(qts, rejected, n);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x46), ==, 0xa5);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 8);
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
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x30);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x30), ==, 0x81);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6b), ==, 3);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 8);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_sequencer_quantum_resume(void)
{
    uint8_t program[3 * 4];
    QTestState *qts = start_aic();
    size_t n = 0;

    emit(program, &n, branch_word(0xc, 1, 0x9c, 2));
    emit(program, &n, alu_word(1, 1, 0x69, 0x91, false));
    emit(program, &n, branch_word(0xc, 1, 0x6a, 0));
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x09, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);

    int64_t first = qtest_clock_step_next(qts);

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x09, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);
    g_assert_cmpint(qtest_clock_step_next(qts) - first, ==, 500000);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x09, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);

    qtest_writeb(qts, REG_BASE + 0x87, 0x06);
    qtest_writeb(qts, REG_BASE + 0x9b, 3);
    qtest_writeb(qts, REG_BASE + 0x87, 0x02);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x09, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 4);
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
    start_program(qts, program, n);
}

static void test_sequencer_self_branch_resumes(void)
{
    QTestState *qts = start_aic();
    int64_t first;

    start_self_branch_program(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x09, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 0);

    first = qtest_clock_step_next(qts);
    g_assert_cmpint(first, >, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x09, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 0);

    g_assert_cmpint(qtest_clock_step_next(qts), >, first);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x09, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x23);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x63), ==, 0);
    qtest_quit(qts);
}

static void test_sequencer_self_branch_cancel(void)
{
    for (unsigned mode = 0; mode < 2; mode++) {
        QTestState *qts = start_aic();
        uint8_t index;

        start_self_branch_program(qts);
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

static void test_sequencer_quantum_queue_wakes(void)
{
    uint8_t program[3 * 4];
    QTestState *qts = start_aic();
    size_t n = 0;

    emit(program, &n, branch_word(0xc, 1, 0x9c, 2));
    emit(program, &n, alu_word(1, 1, 0x69, 0x91, false));
    emit(program, &n, branch_word(0xc, 1, 0x6a, 0));
    start_program(qts, program, n);
    qtest_writeb(qts, REG_BASE + 0x9b, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x09, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 2);
    qtest_quit(qts);
}

static void test_sequencer_quantum_cancel(void)
{
    uint8_t program[3 * 4];
    size_t n = 0;

    emit(program, &n, branch_word(0xc, 1, 0x9c, 2));
    emit(program, &n, alu_word(1, 1, 0x69, 0x91, false));
    emit(program, &n, branch_word(0xc, 1, 0x6a, 0));
    for (unsigned mode = 0; mode < 4; mode++) {
        QTestState *qts = start_aic();
        int64_t before;

        start_program(qts, program, n);
        if (mode == 0) {
            qtest_writeb(qts, REG_BASE + 0x87, 0x06);
        } else if (mode == 1) {
            qtest_writeb(qts, REG_BASE + 0x60, 0x91);
        } else if (mode == 2) {
            qtest_writeb(qts, REG_BASE + 0x87, 0x01);
        } else {
            qtest_system_reset(qts);
            config_writel(qts, 0x14, REG_BASE);
            config_writew(qts, 0x04, 0x0006);
        }
        before = qtest_clock_step(qts, 1);
        if (!qtest_command_expect_fail(qts, "clock_step")) {
            g_assert_cmpint(qtest_clock_step(qts, 1) - before, >, 500000);
        }
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x09, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);
        qtest_quit(qts);
    }
}

static void test_sequencer_reset_keeps_continuation(void)
{
    uint8_t program[3 * 4];
    QTestState *qts = start_aic();
    size_t n = 0;
    int64_t before;

    emit(program, &n, branch_word(0xc, 1, 0x9c, 2));
    emit(program, &n, alu_word(1, 1, 0x69, 0x91, false));
    emit(program, &n, branch_word(0xc, 1, 0x6a, 0));
    start_program(qts, program, n);
    qtest_writeb(qts, REG_BASE + 0x60, 0x92);
    before = qtest_clock_step(qts, 1);
    g_assert_cmpint(qtest_clock_step_next(qts) - before, ==, 499999);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x09, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 0x04, ==, 0);
    qtest_quit(qts);
}

static void test_sequencer_quantum_keeps_pc(void)
{
    uint8_t program[4 * 4];
    QTestState *qts = start_aic();
    size_t n = 0;
    int64_t first;

    emit_or(program, &n, 0x0a, 0x9d);
    emit(program, &n, branch_word(0xc, 1, 0x9c, 3));
    emit(program, &n, alu_word(1, 1, 0x69, 0x91, false));
    emit(program, &n, branch_word(0xc, 1, 0x6a, 1));
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x09, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);

    first = qtest_clock_step_next(qts);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmpint(qtest_clock_step_next(qts) - first, ==, 500000);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 3);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x09, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_sequencer_quantum_migrates(void)
{
    uint8_t program[4 * 4];
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-quantum-XXXXXX", &error);
    g_autofree char *socket_path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;
    size_t n = 0;
    int64_t before;

    g_assert_no_error(error);
    socket_path = g_build_filename(dir, "migrate.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket_path);
    from = start_aic();
    to = qtest_init("-M pc -m 128M -nodefaults -S -incoming defer "
                    "-device aic7880,id=hba,bus=pci.0,addr=04.0");
    emit_or(program, &n, 0x0a, 0x9d);
    emit(program, &n, branch_word(0xc, 1, 0x9c, 3));
    emit(program, &n, alu_word(1, 1, 0x69, 0x91, false));
    emit(program, &n, branch_word(0xc, 1, 0x6a, 1));
    start_program(from, program, n);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x62), ==, 3);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x9e), ==, 1);
    qtest_clock_step(from, 100000);
    qtest_qmp_assert_success(from, "{'execute':'stop'}");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_migrate_resume(to);
    g_assert_cmphex(config_readl(to, 0x14) & ~0x0fu, ==, REG_BASE);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x62), ==, 3);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x91) & 0x09, ==, 0);
    before = qtest_clock_step(to, 1);
    /* QTest starts the destination clock at zero; the timer stores expiry. */
    g_assert_cmpint(qtest_clock_step_next(to) - before, ==, 499999);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x62), ==, 3);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x91) & 0x09, ==, 0);
    qtest_writeb(to, REG_BASE + 0x9b, 3);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x9c), ==, 1);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x91) & 0x09, ==, 1);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_unsupported_or_return(void)
{
    static const uint8_t program[] = { 0x5a, 0x6a, 0x20, 0x01 };
    QTestState *qts = start_aic();

    start_program(qts, program, sizeof(program));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0x08);
    qtest_quit(qts);
}

static QTestState *start_pending_status(void)
{
    static const uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t program[128] = { 0 };
    size_t wait_at = 7 + sizeof(cdb);
    uint32_t jump = branch_word(8, 0, 0x65, wait_at);
    QTestState *qts = start_disk();

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

static void test_peer_channel_clear_aborts_held_data(void)
{
    QTestState *qts = start_peer_data_in(true, true, true);

    qtest_writeb(qts, REG_BASE + 0x01, 0x0a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0);
    peer_event_rejected(qts, "release");
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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
    start_program(qts, finish, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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
    start_program(qts, read_status, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe0, ==, 0xe0);
    peer_event(qts, "bad:msg-in:0:00");
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x14, ==, 0x04);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0xf0);
    peer_event(qts, "good:msg-in:0:00");
    qtest_writeb(qts, REG_BASE + 0x0c, 0x44);
    n = 0;
    emit(read_message, &n, alu_word(0, 0, 0x06, 0x23, false));
    emit_or(read_message, &n, 1, 0x9d);
    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    start_program(qts, read_message, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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
    static const uint8_t messages[] = { 0x81, 0xc0, 0x01, 0x12 };

    for (unsigned i = 0; i < ARRAY_SIZE(messages); i++) {
        QTestState *qts = start_disk();

        attention_select(qts, 0x48);
        qtest_writeb(qts, REG_BASE + 0x92, 1);
        attention_write(qts, 0x06, messages[i]);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==,
                        0x40);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==,
                        0xb0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
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
                        0x40);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00), ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 0);
        qtest_quit(qts);
    }
    {
        QTestState *qts = start_disk();

        qtest_writeb(qts, REG_BASE + 0x00, 0x48);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==,
                        0x40);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xf0, ==, 0);
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
    qtest_writeb(qts, REG_BASE + 0x01, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0x80);
    start_program(qts, status_read, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe6, ==, 0xc6);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x02, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x08, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0x08);

    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x92, 0x08);
    qtest_writeb(qts, REG_BASE + 0x01, 0x88);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0x88);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x02, ==, 0x02);
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
    start_program(qts, message_read, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x03) & 0xe6, ==, 0xe6);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0b) & 0x02, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x0c) & 0x08, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0x08);

    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x92, 0x08);
    qtest_writeb(qts, REG_BASE + 0x01, 0x88);
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
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0x08);
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
    qtest_writeb(qts, REG_BASE + 0x87, 0x02);
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
    size_t wait_at = 6 + sizeof(cdb);
    QTestState *qts = start_disk();
    uint32_t pause = alu_word(0, 1, 0x6a, 0x91, false);

    memset(bytes, 0xa5, sizeof(bytes));
    qtest_memwrite(qts, 0x10000, bytes, sizeof(bytes));
    qtest_writeb(qts, REG_BASE + 0x1f, 0x20);
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
    uint8_t finish[8 * 4];
    uint8_t bytes[36];
    QTestState *qts = start_pending_fifo(0x10000);
    size_t n = 0;

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 0x21, ==, 0);

    emit_or(finish, &n, 0x3a, 0x93);
    emit(finish, &n, branch_word(0xf, 2, 0x0b, 1));
    emit(finish, &n, alu_word(0, 0, 0x06, 0x22, false));
    emit(finish, &n, alu_word(0, 0, 0x06, 0x23, false));
    emit(finish, &n, branch_word(0xf, 8, 0x0c, 4));
    emit_or(finish, &n, 1, 0x9d);
    emit_or(finish, &n, 2, 0x91);
    emit(finish, &n, branch_word(8, 0, 0x65, 7));
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
    unsigned wait_at = 4 + sizeof(cdb) + (expected_phase != 0xff);

    emit_or(program, &n, 0x08, 0x01);
    emit_or(program, &n, 0x07, 0x05);
    emit_or(program, &n, host_dma ? 0x38 : 0x30, 0x93);
    if (expected_phase != 0xff) {
        emit_or(program, &n, expected_phase, 0x03);
    }
    emit_or(program, &n, 0x40, 0x00);
    for (size_t i = 0; i < sizeof(cdb); i++) {
        emit_or(program, &n, cdb[i], 0x06);
    }
    emit(program, &n, branch_word(0xf, 0x08, 0x94, wait_at));
    emit(program, &n, alu_word(0, 0, 0x06, 0x22, false));
    emit(program, &n, alu_word(0, 0, 0x06, 0x23, false));
    emit(program, &n, branch_word(0xf, 0x08, 0x0c, wait_at + 3));
    emit_or(program, &n, 1, 0x9d);
    emit_or(program, &n, 2, 0x91);
    emit(program, &n, branch_word(8, 0, 0x65, wait_at + 6));
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
    qtest_writeb(qts, REG_BASE + 0x1f, disabled ? 0x20 : 0);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 2);
    qtest_quit(qts);
}

static void test_auto_flush_disable_reset(void)
{
    QTestState *qts = start_aic();

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f) & 0x20, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x1f, 0x20);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f) & 0x20, ==, 0x20);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f) & 0x20, ==, 0);
    qtest_writeb(qts, REG_BASE + 0x1f, 0x20);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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

    qtest_writeb(qts, REG_BASE + 0x01, 0x0a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x01), ==, 0x08);
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

static void test_unsupported_scsi_controls(void)
{
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x00, 0x20);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x00) & 0x20, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    qtest_writeb(qts, REG_BASE + 0x93, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 0x40, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    qtest_writeb(qts, REG_BASE + 0x93, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 0x80, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    qtest_writeb(qts, REG_BASE + 0x10, 0x10);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x10), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    qtest_writeb(qts, REG_BASE + 0x11, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x11), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    qtest_writeb(qts, REG_BASE + 0x02, 0x08);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x02), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x04), ==, 0xff);
    qtest_quit(qts);
}

static void test_manual_command_path_is_unsupported(void)
{
    QTestState *qts = start_disk();

    qtest_writeb(qts, REG_BASE + 0x05, 0x07);
    qtest_writeb(qts, REG_BASE + 0x00, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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
    qtest_writeb(qts, REG_BASE + 0x90, 16);
    qtest_writeb(qts, REG_BASE + 0xa0, 0xee);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0), ==, 0xff);
    qtest_writeb(qts, REG_BASE + 0x90, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0xa0), ==, 0x35);

    /* Copy an SCB byte through the sequencer's register bus. */
    emit(program, &n, alu_word(0, 0, 0xa0, 0x20, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x35);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x6c), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x30);
    emit(program, &n, alu_word(0, 0, 0x6c, 0x40, false));
    emit(program, &n, alu_word(0, 0, 0x6c, 0x41, false));
    emit(program, &n, alu_word(0, 1, 0x6a, 0x91, false));
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0x12);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0x34);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x32);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
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
    qtest_writeb(qts, REG_BASE + 0x65, 0x6c);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x6d);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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

    qtest_writeb(qts, REG_BASE + 0x1f, 0x20);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f) & 0x20, ==, 0x20);
    qtest_writeb(qts, REG_BASE + 0x65, 0x1f);
    qtest_writeb(qts, REG_BASE + 0x66, 0x1f);
    emit(program, &n, alu_word(1, 0x0f, 0x6c, 0x6d, false));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x1f) & 0x20, ==, 0x20);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 8);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 8);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 8);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 8);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 9, ==, 8);
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
            qtest_writeb(qts, REG_BASE + 0x87, 2);
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

static void test_unconditional_branch_host_only_source(void)
{
    static const uint8_t ops[] = { 8, 0x0b };

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
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
        g_assert_cmphex(host_dma_read_bytes(qts, 0x88, 4), ==,
                        bases[i] + id * 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, id);
        qtest_writeb(qts, REG_BASE + 0x68, 0x80);
        qtest_writeb(qts, REG_BASE + 0x92, 8);
        qtest_writeb(qts, REG_BASE + 0x87, 2);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x64), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0x0a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_rejected_branch_does_not_read_source(void)
{
    for (unsigned op = 9; op <= 0x0a; op++) {
        uint8_t program[4];
        QTestState *qts = start_aic();
        uint32_t word = branch_word(op, 1,
                                    op == 9 ? 0x9b : 0x6c, 0);

        for (unsigned byte = 0; byte < sizeof(program); byte++) {
            program[byte] = word >> (byte * 8);
        }
        qtest_writeb(qts, REG_BASE + 0x9b, 2);
        qtest_writeb(qts, REG_BASE + 0x65, 0x30);
        start_program(qts, program, sizeof(program));
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9c), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x65), ==, 0x30);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 4, ==, 4);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
        qtest_quit(qts);
    }
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x92, 0x0f);
    qtest_writeb(qts, REG_BASE + 0x87, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0x11);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_quit(qts);
}

static void test_failed_return_keeps_subroutine_pc(void)
{
    uint8_t program[5 * 4];
    QTestState *qts = start_aic();
    size_t n = 0;

    qtest_writeb(qts, REG_BASE + 0x11, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    qtest_writeb(qts, REG_BASE + 0x92, 8);
    emit(program, &n, branch_word(0xb, 0, 0x65, 3));
    emit_or(program, &n, 0x11, 0x40);
    emit_or(program, &n, 1, 0x91);
    emit(program, &n, alu_word(1, 0x40, 0x69, 0x93, true));
    emit_or(program, &n, 0x55, 0x41);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    qtest_quit(qts);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
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
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x9e), ==, 1);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0xa5);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==,
                        0x40);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
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
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0xa5);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==,
                        sources[i] == 0x91 ? 0 : 0x40);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
    g_assert_true(qtest_get_irq(qts, 11));

    n = 0;
    emit_or(clear_program, &n, 1, 0x92);
    emit(clear_program, &n, branch_word(8, 0, 0x65, 1));
    start_program(qts, clear_program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x40), ==, 0x11);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x41), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x04, 0x12);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "unsupported register 0x04"));
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
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x62), ==, 4);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x91) & 1, ==, 1);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x62), ==, 4);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x20), ==, 0);
    qtest_writeb(to, REG_BASE + 0x92, 1);
    qtest_writeb(to, REG_BASE + 0x87, 2);
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
    qtest_writeb(qts, REG_BASE + 0x87, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x62), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x87) & 4, ==, 4);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    g_assert_false(qtest_get_irq(qts, 11));

    qtest_writeb(qts, REG_BASE + 0x92, 8);
    qtest_writeb(qts, REG_BASE + 0x60, 0x98);
    qtest_writeb(qts, REG_BASE + 0x87, 2);
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
    qtest_writeb(qts, REG_BASE + 0x87, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x68), ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    qtest_writeb(qts, REG_BASE + 0x68, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x68), ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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
    qtest_writeb(qts, REG_BASE + 0x68, 2);
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
    qtest_writeb(qts, REG_BASE + 0x87, 2);
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
        qtest_writeb(qts, REG_BASE + 0x87, 2);
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
        qtest_writeb(qts, REG_BASE + 0x87, 2);
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
    qtest_writeb(qts, REG_BASE + 0x1f, 0);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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
    qtest_writeb(qts, REG_BASE + 0x1f, 0);
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
    qtest_writeb(qts, REG_BASE + 0x1f, 0);
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
    qtest_writeb(qts, REG_BASE + 0x87, 0x02);
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

static void test_missing_target(void)
{
    uint8_t program[12] = { 0 };
    size_t n = 0;
    QTestState *qts = start_aic();

    emit_or(program, &n, 0x07, 0x05);
    emit_or(program, &n, 0x40, 0x00);
    emit_or(program, &n, 0x02, 0x91);
    qtest_writeb(qts, REG_BASE + 0x02, 0x04);
    qtest_writeb(qts, REG_BASE + 0x11, 0x80);
    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    for (size_t i = 0; i < n; i++) {
        qtest_writeb(qts, REG_BASE + 0x61, program[i]);
    }
    qtest_writeb(qts, REG_BASE + 0x60, 0x90);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 0x02);
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
    /* Opcode 7 belongs to later 16-bit sequencer formats. */
    static const uint8_t program[] = { 0, 0, 0, 0x0e };
    QTestState *qts = start_aic();

    qtest_writeb(qts, REG_BASE + 0x60, 0x91);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    for (size_t i = 0; i < sizeof(program); i++) {
        qtest_writeb(qts, REG_BASE + 0x61, program[i]);
    }
    qtest_writeb(qts, REG_BASE + 0x60, 0x90);
    qtest_writeb(qts, REG_BASE + 0x62, 0);
    qtest_writeb(qts, REG_BASE + 0x87, 0x02);
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
    host_dma_program(qts, 0x20008, count);
    qtest_writeb(qts, REG_BASE + 0x08, 0);
    qtest_writeb(qts, REG_BASE + 0x09, count >> 8);
    qtest_writeb(qts, REG_BASE + 0x0a, 0);
    qtest_writeb(qts, REG_BASE + 0x93, 1);
    start_program(qts, program,
        build_command_program_mode(program, read_cdb, sizeof(read_cdb),
                                   false, false));
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
                                       write, false));
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
                                           sizeof(write_cdb), true, false));
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
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
    g_autofree char *path = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("aic7880-empty-flush-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 4096), ==, 0);
    close(fd);
    qts = start_throttled_disk(path);
    g_assert_cmphex(run_disk_cdb(qts, read_cdb, sizeof(read_cdb),
                                  0x10000, 512, false), ==, 2);
    g_assert_cmphex(run_disk_cdb(qts, sense_cdb, sizeof(sense_cdb),
                                  0x11000, 18, false), ==, 0);

    qtest_writeb(qts, REG_BASE + 0x87, 0x04);
    qtest_writeb(qts, REG_BASE + 0x88, 0x03);
    qtest_writeb(qts, REG_BASE + 0x89, 0x20);
    qtest_writeb(qts, REG_BASE + 0x8a, 0x01);
    qtest_writeb(qts, REG_BASE + 0x8c, 0);
    qtest_writeb(qts, REG_BASE + 0x8d, 2);
    qtest_writeb(qts, REG_BASE + 0x08, 0);
    qtest_writeb(qts, REG_BASE + 0x09, 2);
    start_program(qts, program,
                  build_command_program(program, read_cdb,
                                        sizeof(read_cdb), false));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 2, ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x08), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x09), ==, 2);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x94) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x93) & 2, ==, 0);
    qtest_quit(qts);
    unlink(path);
}

static void test_reset_cancels_busy_request(void)
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
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_usleep(1500000);
    qtest_clock_step(qts, 1000000000);
    qtest_memread(qts, 0x12000, after, sizeof(after));
    g_assert_cmpint(memcmp(guard, after, sizeof(guard)), ==, 0);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91), ==, 0);
    qtest_quit(qts);
    unlink(path);
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
    static const uint8_t program[] = { 0x01, 0x6a, 0x91, 0x00 };
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
    qtest_writeb(from, REG_BASE + 0x87, 0x02);
    g_assert_cmphex(qtest_readb(from, REG_BASE + 0x91) & 1, ==, 1);

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
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x91) & 1, ==, 1);
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
        /* A controller error may raise PCI STATUS.INTERRUPT via INTx. */
        g_assert_cmphex((config_readl(qts, 0x04) >> 16) & ~0x08u, ==,
                        pci_status & ~0x08u);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92), !=, 0);
        g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 8, ==, 8);
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
    qts = qtest_initf("-M pc -m 128M -nodefaults -d invalid_mem -D %s "
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
    g_assert_nonnull(strstr(log, "region 'aic7880-dspc-mmio-0'"));
    g_assert_true(g_regex_match_simple(
        "Invalid read[^\\n]*region 'aic7880-dspc-io'", log, 0, 0));
    g_assert_true(g_regex_match_simple(
        "Invalid write[^\\n]*region 'aic7880-dspc-io'", log, 0, 0));
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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("aic7880/pci-identity", test_identity);
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
    qtest_add_func("aic7880/pci-error-status-survives-chip-reset",
                   test_pci_error_status_survives_chip_reset);
    qtest_add_func("aic7880/host-faildis-readback", test_host_faildis_readback);
    qtest_add_func("aic7880/pci-error-faildis", test_pci_error_faildis);
    qtest_add_func("aic7880/host-pausedis-refusal", test_host_pausedis_refusal);
    qtest_add_func("aic7880/internal-completion-ids",
                   test_internal_completion_ids);
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
    qtest_add_func("aic7880/sequencer-quantum-resume",
                   test_sequencer_quantum_resume);
    qtest_add_func("aic7880/sequencer-self-branch-resumes",
                   test_sequencer_self_branch_resumes);
    qtest_add_func("aic7880/sequencer-self-branch-cancel",
                   test_sequencer_self_branch_cancel);
    qtest_add_func("aic7880/sequencer-quantum-queue-wakes",
                   test_sequencer_quantum_queue_wakes);
    qtest_add_func("aic7880/sequencer-quantum-cancel",
                   test_sequencer_quantum_cancel);
    qtest_add_func("aic7880/sequencer-reset-keeps-continuation",
                   test_sequencer_reset_keeps_continuation);
    qtest_add_func("aic7880/sequencer-quantum-keeps-pc",
                   test_sequencer_quantum_keeps_pc);
    qtest_add_func("aic7880/sequencer-quantum-migrates",
                   test_sequencer_quantum_migrates);
    qtest_add_func("aic7880/old-branch-high-address",
                   test_old_branch_high_address);
    qtest_add_func("aic7880/unsupported-or-return",
                   test_unsupported_or_return);
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
    qtest_add_func("aic7880/unconditional-branch-host-only-source",
                   test_unconditional_branch_host_only_source);
    qtest_add_func("aic7880/call-pointer-arithmetic",
                   test_call_pointer_arithmetic);
    qtest_add_func("aic7880/four-nested-calls", test_four_nested_calls);
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
    qtest_add_func("aic7880/peer-channel-clear-aborts-held-data",
                   test_peer_channel_clear_aborts_held_data);
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
    qtest_add_func("aic7880/unmasked-selection-timeout",
                   test_unmasked_selection_timeout);
    qtest_add_func("aic7880/unsupported-scsi-controls",
                   test_unsupported_scsi_controls);
    qtest_add_func("aic7880/manual-command-unsupported",
                   test_manual_command_path_is_unsupported);
    qtest_add_func("aic7880/reset-and-interrupt",
                   test_reset_and_interrupt_level);
    qtest_add_func("aic7880/missing-target", test_missing_target);
    qtest_add_func("aic7880/bad-opcode", test_bad_sequencer_opcode);
    qtest_add_func("aic7880/pci-admission-keeps-abort-pending",
                   test_pci_admission_keeps_abort_pending);
    qtest_add_func("aic7880/coupled-pci-admission",
                   test_coupled_pci_admission);
    qtest_add_func("aic7880/dma-requires-bus-master",
                   test_dma_requires_bus_master);
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
    qtest_add_func("aic7880/paused-state-migrates", test_paused_state_migrates);
    return g_test_run();
}
