/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register tests for a standalone AIC-7880 PCI function. */

#include "qemu/osdep.h"
#include "libqtest.h"
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

static void emit(uint8_t *program, size_t *n, uint32_t word)
{
    for (unsigned byte = 0; byte < 4; byte++) {
        program[(*n)++] = word >> (byte * 8);
    }
}

static void emit_or(uint8_t *program, size_t *n, uint8_t value, uint8_t dest)
{
    emit(program, n, value | (0x6au << 8) | ((uint32_t)dest << 16));
}

static size_t build_command_program(uint8_t *program, const uint8_t *cdb,
                                    size_t cdb_len, bool write)
{
    size_t n = 0;
    unsigned wait_at = 4 + cdb_len;
    unsigned idle_at = wait_at + 3;

    g_assert_cmpuint(cdb_len, <=, 16);
    emit_or(program, &n, 0x08, 0x01); /* automatic PIO */
    emit_or(program, &n, 0x07, 0x05); /* initiator 7, target 0 */
    emit_or(program, &n, write ? 0x3c : 0x38, 0x93);
    emit_or(program, &n, 0x40, 0x00); /* start selection */
    for (size_t i = 0; i < cdb_len; i++) {
        emit_or(program, &n, cdb[i], 0x06);
    }
    emit(program, &n, 0x08u | (0x0cu << 8) |
                      (wait_at << 16) | (0xfu << 25));
    emit_or(program, &n, 1, 0x9d); /* completion identifier */
    emit_or(program, &n, 2, 0x91); /* completion interrupt */
    emit(program, &n, (idle_at << 16) | (0x8u << 25));
    return n;
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
    emit(program, &n, (0xa0u << 8) | (0x20u << 16));
    emit_or(program, &n, 1, 0x91);
    start_program(qts, program, n);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x35);
    qtest_quit(qts);
}

static void test_inquiry_via_sequencer(void)
{
    uint8_t program[64] = { 0 };
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
    uint8_t program[64] = { 0 };
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

    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x92) & 0x40, ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x08, ==, 0x08);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x91) & 0x02, ==, 0);
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

static QTestState *start_file_disk(const char *path)
{
    QTestState *qts = qtest_initf(
        "-M pc -m 128M -nodefaults "
        "-drive if=none,id=disk0,file=%s,format=raw "
        "-device aic7880,id=hba,bus=pci.0,addr=04.0 "
        "-device scsi-hd,drive=disk0,bus=hba.0,scsi-id=0,lun=0", path);

    config_writel(qts, 0x14, REG_BASE);
    config_writew(qts, 0x04, 0x0006);
    return qts;
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
    uint8_t program[96] = { 0 };
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
    status = qtest_readb(qts, REG_BASE + 0x06);
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

    qts = start_file_disk(path);
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

static void test_reset_cancels_busy_request(void)
{
    static const uint8_t read_cdb[10] = {
        0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0
    };
    static const uint8_t sense_cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    uint8_t program[96] = { 0 };
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
    qtest_outl(qts, PCI_CONFIG_DATA, REG_BASE + 0x100);
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80002804u);
    qtest_outw(qts, PCI_CONFIG_DATA, 0x0006);
    qtest_writeb(qts, REG_BASE + 0x20, 0x5a);
    qtest_writeb(qts, REG_BASE + 0x100 + 0x20, 0xa5);
    qtest_irq_intercept_in(qts, "ioapic");
    qtest_outl(qts, PCI_CONFIG_ADDR, 0x80000860u);
    qtest_outb(qts, PCI_CONFIG_DATA, 10);
    qtest_outb(qts, PCI_CONFIG_DATA + 3, 11);
    qtest_writeb(qts, REG_BASE + 0x87, 0x16);
    qtest_writeb(qts, REG_BASE + 0x100 + 0x87, 0x16);
    g_assert_true(qtest_get_irq(qts, 11));
    g_assert_true(qtest_get_irq(qts, 10));
    qtest_writeb(qts, REG_BASE + 0x87, 0x06);
    g_assert_false(qtest_get_irq(qts, 11));
    g_assert_true(qtest_get_irq(qts, 10));
    qtest_writeb(qts, REG_BASE + 0x100 + 0x87, 0x06);
    g_assert_false(qtest_get_irq(qts, 10));
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x20), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x100 + 0x20), ==, 0xa5);
    qtest_writeb(qts, REG_BASE + 0x87, 1);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x100 + 0x20), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, REG_BASE + 0x100 + 0x87), ==, 0x06);
    qtest_quit(qts);
}

static void test_paused_state_migrates(void)
{
    static const uint8_t program[] = { 0x01, 0x6a, 0x91, 0x00 };
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("aic7880-migrate-XXXXXX", &error);
    g_autofree char *uri = NULL;
    g_autofree char *socket_path = NULL;
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
    qtest_writeb(from, REG_BASE + 0x20, 0x5a);
    qtest_writeb(from, REG_BASE + 0x90, 2);
    qtest_writeb(from, REG_BASE + 0xa0, 0x35);
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
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x91) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(to, REG_BASE + 0x87) & 4, ==, 4);
    qtest_writeb(to, REG_BASE + 0x60, 0x91);
    qtest_writeb(to, REG_BASE + 0x62, 0);
    for (size_t i = 0; i < sizeof(program); i++) {
        g_assert_cmphex(qtest_readb(to, REG_BASE + 0x61), ==, program[i]);
    }
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket_path);
    g_rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("aic7880/pci-identity", test_identity);
    qtest_add_func("aic7880/sequencer-upload", test_sequencer_upload);
    qtest_add_func("aic7880/sequencer-exec", test_sequencer_exec);
    qtest_add_func("aic7880/scb-pages", test_scb_pages_and_sequencer_access);
    qtest_add_func("aic7880/reset-and-interrupt",
                   test_reset_and_interrupt_level);
    qtest_add_func("aic7880/missing-target", test_missing_target);
    qtest_add_func("aic7880/bad-opcode", test_bad_sequencer_opcode);
    qtest_add_func("aic7880/dma-requires-bus-master",
                   test_dma_requires_bus_master);
    qtest_add_func("aic7880/inquiry-via-sequencer", test_inquiry_via_sequencer);
    qtest_add_func("aic7880/read-write-via-sequencer",
                   test_read_write_via_sequencer);
    qtest_add_func("aic7880/independent-functions", test_independent_functions);
    qtest_add_func("aic7880/reset-cancels-busy",
                   test_reset_cancels_busy_request);
    qtest_add_func("aic7880/paused-state-migrates", test_paused_state_migrates);
    return g_test_run();
}
