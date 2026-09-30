/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Odyssey (VPro) graphics board plugged into the Origin 300's XIO
 * connector. No SGI product shipped that composition; it is the host the
 * emulator can build today, and the register contract under test is the
 * board's, not the machine's.
 *
 * Each test pins one step of the ladder the stock IP35 firmware and the
 * IRIX IP35 graphics driver walk before they will claim the board, plus
 * the rule that everything past that ladder stays visibly unmodeled.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-test.h"

#define MACHINE "-M origin300,topology=origin300-v12-direct " \
                "-cpu R14000 -smp 2 -S"

/*
 * The connector's crosstalk position, repeated from the machine's own
 * selection rather than borrowed from it. No source binds the connector
 * to a crossbar port; see the chip's SGI_XBRIDGE_XIO_PORT_DEFAULT.
 */
#define SLOT 11
#define WIDGET (SPACE_IO + (uint64_t)SLOT * UINT64_C(0x1000000))
#define BOARD "/machine/module0/rear-panel"

#define ODSY_IDENTITY 0x0004
#define ODSY_STATUS 0x000c
#define ODSY_CONTROL 0x0024
#define ODSY_ERROR_COMMAND 0x0044
#define ODSY_INTR_FLAGS 0x1014
#define ODSY_INTR_ENABLE 0x1034
#define ODSY_INTR_ACK 0x103c
#define ODSY_MEMORY_CONFIG 0x106c
#define ODSY_SW_SYNC 0x107c
#define ODSY_NIC 0x10d8
#define ODSY_CMD_FIFO 0x110000

/* Crosstalk part 0xc013, manufacturer 0x023, revision nibble 1. */
#define IDENTITY_REVISION_1 0x1c013047

#define NIC_LEVEL (1u << 0)
#define NIC_DONE (1u << 1)
#define NIC_PULSE(n) ((uint32_t)(n) << 10)
#define NIC_SAMPLE(n) ((uint32_t)(n) << 2)

/*
 * The machine refuses to build without a firmware payload. Nothing here
 * executes it -- every test starts halted and drives the board over the
 * test protocol -- so an erased image is enough.
 */
static char *prom_path;

static char *create_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static QTestState *board_start(const char *extra)
{
    g_autofree char *quoted = g_shell_quote(prom_path);

    return qtest_initf(MACHINE " -bios %s%s", quoted, extra);
}

static uint32_t reg_read(QTestState *qts, uint64_t off)
{
    return qtest_readl(qts, WIDGET + off);
}

static void reg_write(QTestState *qts, uint64_t off, uint32_t value)
{
    qtest_writel(qts, WIDGET + off, value);
}

/*
 * One bit slot on the board-identity access register, with the widths
 * the stock IP35 PROM was captured composing: a long low drive is a bus
 * reset, a short one a write-one or a read, and one in between a
 * write-zero.
 */
static bool nic_slot(QTestState *qts, unsigned int pulse, unsigned int sample)
{
    uint32_t nic;

    reg_write(qts, ODSY_NIC, NIC_PULSE(pulse) | NIC_SAMPLE(sample));
    nic = reg_read(qts, ODSY_NIC);
    g_assert_cmphex(nic & NIC_DONE, ==, NIC_DONE);
    return nic & NIC_LEVEL;
}

#define NIC_RESET(q) nic_slot((q), 520, 65)
#define NIC_READ(q) nic_slot((q), 6, 13)
#define NIC_WRITE_ONE(q) nic_slot((q), 6, 13)
#define NIC_WRITE_ZERO(q) nic_slot((q), 80, 30)

static void nic_write_byte(QTestState *qts, uint8_t byte)
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        if ((byte >> i) & 1) {
            NIC_WRITE_ONE(qts);
        } else {
            NIC_WRITE_ZERO(qts);
        }
    }
}

static uint8_t nic_read_byte(QTestState *qts)
{
    unsigned int i;
    uint8_t byte = 0;

    for (i = 0; i < 8; i++) {
        if (NIC_READ(qts)) {
            byte |= 1u << i;
        }
    }
    return byte;
}

/* The identity the driver reads, and the revision nibble it dispatches on. */
static void test_identity_carries_part_and_revision(void)
{
    QTestState *qts = board_start("");

    g_assert_cmphex(reg_read(qts, ODSY_IDENTITY), ==, IDENTITY_REVISION_1);
    qtest_quit(qts);

    qts = board_start(" -global sgi-odyssey.revision=3");
    g_assert_cmphex(reg_read(qts, ODSY_IDENTITY), ==, 0x3c013047);
    qtest_quit(qts);
}

/*
 * The driver recovers its own widget number from bits 3:0 of the control
 * register before anything writes it, so the reset value carries the
 * slot. The register itself is plain read/write, the way the Bridge's is:
 * nothing forces the nibble back.
 */
static void test_widget_control_reads_back(void)
{
    QTestState *qts = board_start("");

    g_assert_cmphex(reg_read(qts, ODSY_CONTROL) & 0xf, ==, SLOT);

    reg_write(qts, ODSY_CONTROL, 0x0f003200 | SLOT);
    g_assert_cmphex(reg_read(qts, ODSY_CONTROL), ==, 0x0f003200 | SLOT);
    reg_write(qts, ODSY_CONTROL, 0);
    g_assert_cmphex(reg_read(qts, ODSY_CONTROL), ==, 0);

    /* The error class the crosstalk error handler tests stays clear. */
    g_assert_cmphex(reg_read(qts, ODSY_STATUS) & 0x1c0, ==, 0);
    reg_write(qts, ODSY_ERROR_COMMAND, 0xffffffff);
    g_assert_cmphex(reg_read(qts, ODSY_ERROR_COMMAND), ==, 0);

    qtest_quit(qts);
}

/*
 * Memory size, bank count and column-address latency reach the driver
 * through one register, and IRIX publishes the marketed board name from
 * the size field alone. The zero low bits are the outstanding-operation
 * count and the data-FIFO level the driver waits on at nearly every step.
 */
static void test_memory_configuration_follows_properties(void)
{
    QTestState *qts = board_start("");

    g_assert_cmphex(reg_read(qts, ODSY_MEMORY_CONFIG), ==, 0x80000000);
    qtest_quit(qts);

    qts = board_start(" -global sgi-odyssey.memory-size=32");
    g_assert_cmphex(reg_read(qts, ODSY_MEMORY_CONFIG), ==, 0);
    qtest_quit(qts);

    qts = board_start(" -global sgi-odyssey.memory-size=64"
                      " -global sgi-odyssey.memory-banks=2"
                      " -global sgi-odyssey.cas-latency=2");
    g_assert_cmphex(reg_read(qts, ODSY_MEMORY_CONFIG), ==, 0x70000000);
    qtest_quit(qts);
}

/*
 * A real one-wire bus behind the access register, not a status bit: the
 * stock firmware runs presence, selection and a memory read, and stops
 * clocking after one 32-byte page. Reading that page back is what proves
 * the register drives a bus at all.
 */
static void test_board_identity_part_answers(void)
{
    QTestState *qts = board_start("");
    uint8_t page[32];
    unsigned int i;

    g_assert_false(NIC_RESET(qts));         /* pulled low: the part answers */
    nic_write_byte(qts, 0xcc);              /* one part on the bus: skip ROM */
    nic_write_byte(qts, 0xf0);              /* read memory, address 0 */
    nic_write_byte(qts, 0x00);
    nic_write_byte(qts, 0x00);
    for (i = 0; i < sizeof(page); i++) {
        page[i] = nic_read_byte(qts);
    }

    /*
     * A format byte, four spaces and the delimiter a scanning reader
     * needs before the first field name, then the record itself.
     */
    g_assert_cmphex(page[0], ==, 0x01);
    g_assert_cmphex(page[5], ==, ';');
    g_assert_cmpmem(&page[6], 20, "Name:ODY128VERSIONB;", 20);

    qtest_quit(qts);
}

/*
 * The flag, enable and acknowledge trio, and the rule that an enable on
 * its own raises nothing. No interrupt source is modeled yet, so the flag
 * register stays zero; what this pins is that the line follows the
 * intersection rather than the enable.
 */
static void test_interrupt_registers(void)
{
    QTestState *qts = board_start("");

    qtest_irq_intercept_out_named(qts, BOARD, "sysbus-irq");

    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_ENABLE), ==, 0);

    reg_write(qts, ODSY_INTR_ENABLE, 0xffffffff);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_ENABLE), ==, 0xffffffff);
    g_assert_false(qtest_get_irq(qts, 0));

    /* Write-1-to-clear over the flags, and nothing readable behind it. */
    reg_write(qts, ODSY_INTR_ACK, 0xffffffff);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_ACK), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    /* The software-sync pair: a token written, then read back. */
    reg_write(qts, ODSY_SW_SYNC, 0x5a5a5a5a);
    g_assert_cmphex(reg_read(qts, ODSY_SW_SYNC), ==, 0x5a5a5a5a);

    qtest_quit(qts);
}

/*
 * Everything past the modeled ladder -- the command FIFO, the indirect
 * write port, the display back end -- reads zero, swallows writes and
 * says so once per offset in the log.
 */
static void test_unmodeled_offsets_are_visible(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    const char *at;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-unimp-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);

    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    g_assert_cmphex(reg_read(qts, ODSY_CMD_FIFO), ==, 0);
    reg_write(qts, ODSY_CMD_FIFO, 0x00010046);
    g_assert_cmphex(reg_read(qts, ODSY_CMD_FIFO), ==, 0);
    /* The command-FIFO self-test word, which only a later increment answers. */
    g_assert_cmphex(reg_read(qts, 0x201594), ==, 0);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    at = strstr(log, "4-byte read at 0x110000 is not modeled");
    g_assert_nonnull(at);
    /* Once per offset, however many times the guest touches it. */
    g_assert_null(strstr(at + 1, "4-byte read at 0x110000 is not modeled"));
    g_assert_nonnull(strstr(log, "4-byte write at 0x110000 is not modeled"));
    g_assert_nonnull(strstr(log, "4-byte read at 0x201594 is not modeled"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

static void test_reset_restores_the_documented_values(void)
{
    QTestState *qts = board_start("");

    reg_write(qts, ODSY_CONTROL, 0xdeadbeef);
    reg_write(qts, ODSY_INTR_ENABLE, 0xffffffff);
    reg_write(qts, ODSY_SW_SYNC, 0x12345678);
    reg_write(qts, ODSY_NIC, NIC_PULSE(520) | NIC_SAMPLE(65));

    qtest_system_reset(qts);

    g_assert_cmphex(reg_read(qts, ODSY_CONTROL), ==, SLOT);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_ENABLE), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_SW_SYNC), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_NIC), ==, 0);
    /* Reset does not disturb what the properties compose. */
    g_assert_cmphex(reg_read(qts, ODSY_IDENTITY), ==, IDENTITY_REVISION_1);
    g_assert_cmphex(reg_read(qts, ODSY_MEMORY_CONFIG), ==, 0x80000000);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int result;

    g_test_init(&argc, &argv, NULL);
    prom_path = create_prom();
    qtest_add_func("sn1/odyssey/identity-carries-part-and-revision",
                   test_identity_carries_part_and_revision);
    qtest_add_func("sn1/odyssey/widget-control-reads-back",
                   test_widget_control_reads_back);
    qtest_add_func("sn1/odyssey/memory-configuration-follows-properties",
                   test_memory_configuration_follows_properties);
    qtest_add_func("sn1/odyssey/board-identity-part-answers",
                   test_board_identity_part_answers);
    qtest_add_func("sn1/odyssey/interrupt-registers",
                   test_interrupt_registers);
    qtest_add_func("sn1/odyssey/unmodeled-offsets-are-visible",
                   test_unmodeled_offsets_are_visible);
    qtest_add_func("sn1/odyssey/reset-restores-documented-values",
                   test_reset_restores_the_documented_values);

    result = g_test_run();
    g_assert_cmpint(g_unlink(prom_path), ==, 0);
    return result;
}
