/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA firmware-load and post-load bring-up qtests.
 *
 * Covers the indirect port a user-space loader writes the pipe's firmware
 * through (+0x8003e8 address, +0x8003f0 data, +0x8003f8 advancing write)
 * and the load record the model traces when internal address 0x200050
 * goes from 1 to 0. Each case is a discriminating test: it fails on a
 * model without the behaviour for the reason its comment gives.
 *
 * The fixture attaches KONA the way sn0-origin2000-kona-test.c does, as a
 * -device at the empty slot IO4, crossbow0's widget 11, but with
 * fabric-hub-widget=9 as sn0-origin2000-kona-dma-test.c sets it, so the
 * board's own fabric writes reach node 0's Hub and its memory.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

#define SMALL_WINDOW_SIZE UINT64_C(0x1000000)
#define KONA_WIDGET 11
#define KONA_BASE   (SPACE_IO + (uint64_t)KONA_WIDGET * SMALL_WINDOW_SIZE)
#define KONA_REG(offset) (KONA_BASE + (offset))

#define REG_PORT_ADDR       KONA_REG(0x8003e8)
#define REG_PORT_DATA       KONA_REG(0x8003f0)
#define REG_PORT_WRITE_NEXT KONA_REG(0x8003f8)
#define REG_LOADER_3B0      KONA_REG(0x8003b0)
#define REG_LOADER_328      KONA_REG(0x800328)
#define REG_MAPRAM(i)       KONA_REG(0x100000 + 4 * (uint64_t)(i))
#define REG_ACTIVITY        KONA_REG(0x8003a8)
#define REG_DMA_FIFO_PUSH   KONA_REG(0x80007c)
#define REG_INT_CAUSE       KONA_REG(0x800238)
#define REG_MAILBOX         KONA_REG(0x800240)
#define REG_KICK            KONA_REG(0xc00108)
#define REG_COMMAND         KONA_REG(0xc10000)
#define REG_COMMAND_DATA    KONA_REG(0xc1000c)
/* The slot store that opens token t: offset 0xc + 0x20 * t. */
#define REG_COMMAND_SLOT(t) KONA_REG(0xc1000c + 0x20 * (uint64_t)(t))

#define REG_INT_DEST_UPPER  KONA_REG(0x000034)
#define REG_INT_DEST_LOWER  KONA_REG(0x00003c)
#define REG_INT_VECTORS     KONA_REG(0x00005c)

#define ACTIVITY_BUS        0x4
#define INT_CAUSE_MAILBOX   0x10
#define INT_CAUSE_GE        0x20
#define INT_CAUSE_KONASTART_DIES 0x100

/*
 * What the IRIX 6.5.30 driver's kona_reset_adaptor wrote in a captured
 * run: vectors 0xf (xg), 0x11 (geometry engine) and 0x10 (general), and
 * node 0's Hub interrupt-modify register at widget 9 as the destination.
 */
#define KERNEL_INT_VECTORS    0x3e2210
#define KERNEL_INT_DEST_UPPER 0x98000
#define KERNEL_INT_DEST_LOWER 0x1800090
#define GE_VECTOR             0x11
#define GENERAL_VECTOR        0x10
#define PI_INT_MODIFY         0x90

/* A node 0 guest RAM page for the reason page, and a second for a check. */
#define REASON_PAGE           UINT64_C(0x300000)
#define OTHER_PAGE            UINT64_C(0x310000)

/*
 * The return-data page: the configuration block starts at byte 8 and is
 * 0x778 bytes long, with the fields "The configuration request, 0x1e9"
 * names at these offsets from its start.
 */
#define RETURN_PAGE           UINT64_C(0x320000)
#define CONFIG_OFFSET         8
#define CONFIG_SIZE           0x778

/*
 * The pool descriptors in that block: seven words for each of the four
 * classes at 0x90 + 0x1c * class. The words are the model's chosen
 * answer, repeated here so a silent change fails.
 */
#define POOL_OFFSET           0x90
#define POOL_STRIDE           0x1c
#define POOL_CLASSES          4
#define POOL_WORDS            7
#define POOL_MAX_KEY          0
#define POOL_MAX_ALIGN        1
#define POOL_SHARED           3
#define POOL_GRANULE          5
#define POOL_CAPACITY         6
#define TEXTURE_BYTES         (64 * UINT64_C(1024) * 1024)

/* Largest key, largest alignment, copies, shared, copy-down, granule, size */
static const uint32_t pool_words[POOL_CLASSES][POOL_WORDS] = {
    { 0, 0x4000, 1, 0, 1, 0x400, 0x100 },
    { 0, 0x4000, 1, 1, 1, 0x400, 0x3c00 },
    { 10, 0x4000, 4, 0, 1, 0x400, 0x400 },
    { 0, 0x4000, 1, 0, 1, 0x400, 0x10000 },
};

#define ICU_CONTROL         0x200050
#define ARM_HIGH_BASE       0x4000000
#define EEPROM_CHECKED_ADDR 0x87ff1c

#define MACHINE_ARGS \
    "-M origin2000,nodes=1 -smp 1 -m 64M " \
    "-device sgi-kona,id=kona0,slot=io4," \
    "fabric-hub-widget=9"

/* The one-node machine is small enough to migrate quickly as it is. */
#define MIGRATION_ARGS MACHINE_ARGS

static QTestState *qts;

static void kona_reset(void)
{
    qtest_system_reset(qts);
}

static void port_select(QTestState *s, uint32_t addr)
{
    qtest_writel(s, REG_PORT_ADDR, addr);
}

static void port_load(QTestState *s, uint32_t addr, const uint32_t *words,
                      unsigned count)
{
    port_select(s, addr);
    for (unsigned i = 0; i < count; i++) {
        qtest_writel(s, REG_PORT_WRITE_NEXT, words[i]);
    }
}

static void port_poke(QTestState *s, uint32_t addr, uint32_t value)
{
    port_select(s, addr);
    qtest_writel(s, REG_PORT_DATA, value);
}

static uint32_t port_peek(QTestState *s, uint32_t addr)
{
    port_select(s, addr);
    return qtest_readl(s, REG_PORT_DATA);
}

/*
 * A fresh instance tracing the events matching `events` and logging the
 * -d categories in `debug` to its own file. The caller quits it and reads
 * the log with kona_log_read(), which also removes the file.
 */
static QTestState *kona_init_logged(const char *base_args, const char *events,
                                    const char *debug, char **log)
{
    int fd;

    *log = g_strdup_printf("%s/sn0-kona-bringup-XXXXXX.log", g_get_tmp_dir());
    fd = g_mkstemp(*log);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    return qtest_initf("%s -trace enable=%s -d %s -D %s", base_args, events,
                       debug, *log);
}

static char *kona_log_read(const char *log)
{
    char *found = NULL;

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_cmpint(g_unlink(log), ==, 0);
    return found;
}

static unsigned count_occurrences(const char *haystack, const char *needle)
{
    unsigned n = 0;

    for (const char *p = strstr(haystack, needle); p;
         p = strstr(p + 1, needle)) {
        n++;
    }
    return n;
}

/* The trace line the model prints for one section of the load record. */
static char *expected_section(unsigned index, uint32_t addr,
                              const uint32_t *words, unsigned count)
{
    g_autofree uint8_t *bytes = g_new(uint8_t, count * 4);
    g_autofree char *sha = NULL;

    for (unsigned i = 0; i < count; i++) {
        stl_be_p(bytes + 4 * i, words[i]);
    }
    sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes, count * 4);
    return g_strdup_printf("section %u addr 0x%08x words %u sha256 %s",
                           index, addr, count, sha);
}

/* One command on the +0xc10000 port, with no kicks and no sync word. */
static void kona_command(QTestState *s, uint32_t token, const uint32_t *args,
                         unsigned argc)
{
    qtest_writel(s, REG_COMMAND, token);
    for (unsigned i = 0; i < argc; i++) {
        qtest_writel(s, REG_COMMAND, args[i]);
    }
}

/* KONAstart's framing of the same command: kick 0x30000, token, 0x10000. */
static void kona_command_kicked(QTestState *s, uint32_t token,
                                const uint32_t *args, unsigned argc)
{
    qtest_writel(s, REG_KICK, 0x30000);
    qtest_writel(s, REG_COMMAND, token);
    qtest_writel(s, REG_KICK, 0x10000);
    for (unsigned i = 0; i < argc; i++) {
        qtest_writel(s, REG_COMMAND, args[i]);
    }
    qtest_writel(s, REG_KICK, 0);
}

/*
 * The PROM's sync probe, 1 9 0: its mailbox reply shows the textport's
 * own framing is still in step after whatever came before it.
 */
static void assert_textport_in_step(QTestState *s)
{
    qtest_writel(s, REG_COMMAND, 1);
    qtest_writel(s, REG_COMMAND, 9);
    qtest_writel(s, REG_COMMAND, 0);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_MAILBOX, ==,
                    INT_CAUSE_MAILBOX);
    g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0x1234abcd);
    qtest_writel(s, REG_MAILBOX, 0);
    qtest_writel(s, REG_INT_CAUSE, INT_CAUSE_MAILBOX);
}

static uint64_t hub_read(QTestState *s, uint64_t offset)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(s, 0, HUB_LOCAL + offset, bytes,
                                    sizeof(bytes)));
    return ldq_be_p(bytes);
}

static void hub_write(QTestState *s, uint64_t offset, uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(s, 0, HUB_LOCAL + offset, bytes,
                                     sizeof(bytes)));
}

static bool hub_pending(QTestState *s, unsigned int vector)
{
    return hub_read(s, PI_INT_PEND0) & (UINT64_C(1) << vector);
}

/* Program the interrupt registers as kona_reset_adaptor does. */
static void kona_program_interrupts(QTestState *s)
{
    qtest_writel(s, REG_INT_DEST_UPPER, KERNEL_INT_DEST_UPPER);
    qtest_writel(s, REG_INT_DEST_LOWER, KERNEL_INT_DEST_LOWER);
    qtest_writel(s, REG_INT_VECTORS, KERNEL_INT_VECTORS);
    hub_write(s, PI_INT_MODIFY, GE_VECTOR);
    hub_write(s, PI_INT_MODIFY, GENERAL_VECTOR);
}

static uint32_t guest_word(QTestState *s, uint64_t addr)
{
    uint8_t bytes[4];

    qtest_memread(s, addr, bytes, sizeof(bytes));
    return ldl_be_p(bytes);
}

static void guest_fill_word(QTestState *s, uint64_t addr, uint32_t value)
{
    uint8_t bytes[4];

    stl_be_p(bytes, value);
    qtest_memwrite(s, addr, bytes, sizeof(bytes));
}

/* KONAstart's 0x1f1 for a host physical page. */
static void kona_register_page(QTestState *s, uint64_t page)
{
    kona_command_kicked(s, 0x1f1,
                        (uint32_t[]){ 0x10000000 | (uint32_t)(page >> 12),
                                      0x9000 | (uint32_t)(page & 0xfff),
                                      0x4000 }, 3);
}

/* The X server context's 0x1f2 for a host physical return page. */
static void kona_register_return_page(QTestState *s, uint64_t page)
{
    kona_command_kicked(s, 0x1f2,
                        (uint32_t[]){ 0x10000000 | (uint32_t)(page >> 12),
                                      0x9000 | (uint32_t)(page & 0xfff),
                                      0x4000 }, 3);
}

static void guest_fill(QTestState *s, uint64_t addr, uint8_t byte, size_t len)
{
    g_autofree uint8_t *bytes = g_malloc(len);

    memset(bytes, byte, len);
    qtest_memwrite(s, addr, bytes, len);
}

/*
 * The configuration block at `page` + 8: the named fields and the pool
 * descriptors hold the model's values and every other byte of the 0x778
 * is zero.
 */
static void assert_config_block(QTestState *s, uint64_t page)
{
    static const struct {
        uint32_t offset;
        uint32_t value;
    } fields[] = {
        { 0x00, 0 }, { 0x14, 1 }, { 0x58, 0 }, { 0x5c, 0xf },
        { 0x84, 1280 }, { 0x88, 1024 }, { 0x8c, 0 },
    };
    g_autofree uint8_t *block = g_malloc(CONFIG_SIZE);
    g_autofree uint8_t *expected = g_malloc0(CONFIG_SIZE);

    for (size_t i = 0; i < ARRAY_SIZE(fields); i++) {
        stl_be_p(expected + fields[i].offset, fields[i].value);
    }
    for (size_t c = 0; c < POOL_CLASSES; c++) {
        for (size_t w = 0; w < POOL_WORDS; w++) {
            stl_be_p(expected + POOL_OFFSET + POOL_STRIDE * c + 4 * w,
                     pool_words[c][w]);
        }
    }
    qtest_memread(s, page + CONFIG_OFFSET, block, CONFIG_SIZE);
    for (size_t i = 0; i < ARRAY_SIZE(fields); i++) {
        g_assert_cmphex(ldl_be_p(block + fields[i].offset), ==,
                        fields[i].value);
    }
    g_assert_cmpmem(block, CONFIG_SIZE, expected, CONFIG_SIZE);
}

/*
 * Port stride: three advancing writes from 0x4000000, then a select of
 * 0x4000004, read the second word. Discriminates an advancing port from a
 * fixed one and a word stride from a byte stride.
 */
static void test_port_stride(void)
{
    static const uint32_t words[] = { 0x11111111, 0x22222222, 0x33333333 };

    kona_reset();
    port_load(qts, ARM_HIGH_BASE, words, ARRAY_SIZE(words));
    g_assert_cmphex(port_peek(qts, ARM_HIGH_BASE + 4), ==, 0x22222222);
    g_assert_cmphex(port_peek(qts, ARM_HIGH_BASE), ==, 0x11111111);
    g_assert_cmphex(port_peek(qts, ARM_HIGH_BASE + 8), ==, 0x33333333);
    /* The advancing writes moved the address register itself. */
    port_load(qts, ARM_HIGH_BASE + 0x100, words, 2);
    g_assert_cmphex(qtest_readl(qts, REG_PORT_ADDR), ==,
                    ARM_HIGH_BASE + 0x108);
}

/*
 * Two reads of +0x8003f0 without a new select return the same word. This
 * checks the model's chosen behaviour, not a hardware fact: whether the
 * data port advances is unknown.
 */
static void test_port_read_does_not_advance(void)
{
    static const uint32_t words[] = { 0xa0a0a0a0, 0xb0b0b0b0 };

    kona_reset();
    port_load(qts, ARM_HIGH_BASE, words, ARRAY_SIZE(words));
    port_select(qts, ARM_HIGH_BASE);
    g_assert_cmphex(qtest_readl(qts, REG_PORT_DATA), ==, 0xa0a0a0a0);
    g_assert_cmphex(qtest_readl(qts, REG_PORT_DATA), ==, 0xa0a0a0a0);
    g_assert_cmphex(qtest_readl(qts, REG_PORT_ADDR), ==, ARM_HIGH_BASE);

    /* A write through the data port does not advance either. */
    qtest_writel(qts, REG_PORT_DATA, 0xc0c0c0c0);
    g_assert_cmphex(qtest_readl(qts, REG_PORT_ADDR), ==, ARM_HIGH_BASE);
    g_assert_cmphex(port_peek(qts, ARM_HIGH_BASE), ==, 0xc0c0c0c0);
    g_assert_cmphex(port_peek(qts, ARM_HIGH_BASE + 4), ==, 0xb0b0b0b0);
}

/*
 * The established EEPROM word keeps answering through the same port once
 * the loader's ranges are backed, and a load does not disturb it. A guard
 * rather than a discriminating case: the model before this change also
 * answered it.
 */
static void test_eeprom_word_still_answers(void)
{
    static const uint32_t words[] = { 1, 2, 3, 4 };

    kona_reset();
    g_assert_cmphex(port_peek(qts, EEPROM_CHECKED_ADDR), ==, 2);
    port_load(qts, 0, words, 3);
    port_load(qts, EEPROM_CHECKED_ADDR - 4, words, ARRAY_SIZE(words));
    g_assert_cmphex(port_peek(qts, EEPROM_CHECKED_ADDR), ==, 2);
    g_assert_cmphex(port_peek(qts, EEPROM_CHECKED_ADDR - 4), ==, 0);
    g_assert_cmphex(port_peek(qts, EEPROM_CHECKED_ADDR + 4), ==, 0);
}

/*
 * Reads outside the backed ranges return zero, and a word written just
 * past either range is dropped rather than aliased into it.
 */
static void test_port_unbacked_addresses_read_zero(void)
{
    static const uint32_t word = 0x5a5a5a5a;

    kona_reset();
    port_load(qts, 0x2c, &word, 1);
    port_load(qts, 0x414e630, &word, 1);
    port_load(qts, ARM_HIGH_BASE - 4, &word, 1);
    g_assert_cmphex(port_peek(qts, 0x2c), ==, 0);
    g_assert_cmphex(port_peek(qts, 0x414e630), ==, 0);
    g_assert_cmphex(port_peek(qts, ARM_HIGH_BASE - 4), ==, 0);
    g_assert_cmphex(port_peek(qts, 0), ==, 0);

    port_load(qts, 0x28, &word, 1);
    port_load(qts, 0x414e62c, &word, 1);
    g_assert_cmphex(port_peek(qts, 0x28), ==, word);
    g_assert_cmphex(port_peek(qts, 0x414e62c), ==, word);
}

/*
 * The load record: the loader's shape in small -- 0x200050 <- 1, two
 * sections, 0x200050 <- 0 -- traces each section's address, word count
 * and the SHA-256 of its big-endian bytes, once, at the 1-to-0 edge.
 */
static void test_load_record_traced_at_arm_release(void)
{
    static const uint32_t image[] = {
        0xe59ff018, 0xe59ff018, 0xe59ff018, 0xe59ff018, 0xdeadbeef,
    };
    static const uint32_t reset_code[] = { 0xe3a00000, 0xe3a01aa0, 0xeafffffe };
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    g_autofree char *first = expected_section(0, ARM_HIGH_BASE, image,
                                              ARRAY_SIZE(image));
    g_autofree char *second = expected_section(1, 0, reset_code,
                                               ARRAY_SIZE(reset_code));
    QTestState *s = kona_init_logged(MACHINE_ARGS, "sgi_kona_load_*",
                                     "unimp", &log);

    port_poke(s, ICU_CONTROL, 1);
    /* A select with no advancing write after it opens no section. */
    port_select(s, 0x2000bc);
    port_load(s, ARM_HIGH_BASE, image, 2);
    /* Continuing without a new select extends the same section. */
    for (unsigned i = 2; i < ARRAY_SIZE(image); i++) {
        qtest_writel(s, REG_PORT_WRITE_NEXT, image[i]);
    }
    port_load(s, 0, reset_code, ARRAY_SIZE(reset_code));
    port_poke(s, ICU_CONTROL, 0);
    /* A second 1-to-0 edge with no new sections records nothing more. */
    port_poke(s, ICU_CONTROL, 0);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, first));
    g_assert_nonnull(strstr(found, second));
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_load_section "), ==,
                     2);
    g_assert_cmpuint(count_occurrences(found,
                                       "sections 2 recorded 2"), ==, 1);
}

/*
 * The two registers the loader writes after the load are stored and
 * traced, not reported as unimplemented.
 */
static void test_loader_registers_are_stored(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "sgi_kona_loader_register",
                                     "unimp", &log);

    qtest_writel(s, REG_LOADER_3B0, 5);
    qtest_writel(s, REG_LOADER_328, 1);
    g_assert_cmphex(qtest_readl(s, REG_LOADER_3B0), ==, 5);
    g_assert_cmphex(qtest_readl(s, REG_LOADER_328), ==, 1);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_null(strstr(found, "unimplemented write of offset 0x8003b0"));
    g_assert_null(strstr(found, "unimplemented write of offset 0x800328"));
    g_assert_nonnull(strstr(found, "offset 0x8003b0 value 0x5"));
    g_assert_nonnull(strstr(found, "offset 0x800328 value 0x1"));
}

/* Reset clears the loaded words, the reset hold and the loader registers. */
static void test_reset_clears_load_state(void)
{
    static const uint32_t words[] = { 0x12345678, 0x9abcdef0 };

    kona_reset();
    port_poke(qts, ICU_CONTROL, 1);
    port_load(qts, 0, words, ARRAY_SIZE(words));
    port_load(qts, ARM_HIGH_BASE, words, ARRAY_SIZE(words));
    qtest_writel(qts, REG_LOADER_3B0, 5);
    qtest_writel(qts, REG_LOADER_328, 1);

    kona_reset();
    g_assert_cmphex(qtest_readl(qts, REG_PORT_ADDR), ==, 0);
    g_assert_cmphex(port_peek(qts, 0), ==, 0);
    g_assert_cmphex(port_peek(qts, ARM_HIGH_BASE + 4), ==, 0);
    g_assert_cmphex(port_peek(qts, ICU_CONTROL), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_LOADER_3B0), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_LOADER_328), ==, 0);
}

/*
 * The loaded words, the reset hold, the loader registers and a section
 * still open at the migration point all cross it: the destination
 * finishes the section, releases the ARM and traces a record whose count
 * and hash cover words written on both sides.
 */
static void test_migration_of_bringup_state(void)
{
    static const uint32_t words[] = { 0x01020304, 0x05060708, 0x090a0b0c };
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    g_autofree char *low = expected_section(0, 0x20, words, 2);
    g_autofree char *high = expected_section(1, ARM_HIGH_BASE, words,
                                             ARRAY_SIZE(words));
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-bringup-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = kona_init_logged(dest_args, "sgi_kona_load_section", "unimp", &log);

    port_load(from, 0x20, words, 2);
    port_poke(from, ICU_CONTROL, 1);
    port_load(from, ARM_HIGH_BASE, words, 2);
    qtest_writel(from, REG_LOADER_3B0, 5);
    qtest_writel(from, REG_LOADER_328, 1);
    kona_program_interrupts(from);
    kona_register_page(from, REASON_PAGE);
    kona_register_return_page(from, RETURN_PAGE);
    /* A command half framed at the migration point. */
    qtest_writel(from, REG_COMMAND, 0x1f8);

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(qtest_readl(to, REG_ACTIVITY), ==, ACTIVITY_BUS);

    /*
     * The destination finishes the command: the reason page, the
     * interrupt registers and the framing all crossed.
     */
    hub_write(to, PI_INT_MODIFY, GE_VECTOR);
    guest_fill_word(to, REASON_PAGE, 0);
    qtest_writel(to, REG_COMMAND, 0x103);
    g_assert_cmphex(guest_word(to, REASON_PAGE), ==, 0x103);
    g_assert_cmphex(qtest_readl(to, REG_INT_CAUSE), ==, INT_CAUSE_GE);
    g_assert_true(hub_pending(to, GE_VECTOR));

    /* The return page crossed too: 0x1e9 answers there. */
    guest_fill_word(to, RETURN_PAGE, 0);
    kona_command(to, 0x1e9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(to, RETURN_PAGE), ==, 1);
    assert_config_block(to, RETURN_PAGE);

    qtest_writel(to, REG_PORT_WRITE_NEXT, words[2]);
    g_assert_cmphex(qtest_readl(to, REG_PORT_ADDR), ==, ARM_HIGH_BASE + 12);
    g_assert_cmphex(port_peek(to, 0x24), ==, words[1]);
    g_assert_cmphex(port_peek(to, ARM_HIGH_BASE + 8), ==, words[2]);
    g_assert_cmphex(qtest_readl(to, REG_LOADER_3B0), ==, 5);
    g_assert_cmphex(qtest_readl(to, REG_LOADER_328), ==, 1);
    g_assert_cmphex(port_peek(to, ICU_CONTROL), ==, 1);
    port_poke(to, ICU_CONTROL, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, low));
    g_assert_nonnull(strstr(found, high));
}

/*
 * Framing: the X server process's 0x1f2 0 0x9000 0x4000 and 0x1e9 0, and
 * a context activation's 0x1fd whose arguments look like the textport's
 * sync probe, are each consumed whole. A model that dropped the tokens
 * as unexpected words would read 0x1fd's arguments 1 9 0 as a sync probe
 * and queue a mailbox reply; this one queues nothing, logs no unexpected
 * word, and the textport stays in step afterwards. An unknown token still
 * takes the unexpected-word path.
 */
static void test_kernel_command_framing(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "sgi_kona_kernel_command",
                                     "guest_errors", &log);

    kona_command(s, 0x1f2, (uint32_t[]){ 0, 0x9000, 0x4000 }, 3);
    kona_command(s, 0x1e9, (uint32_t[]){ 0 }, 1);
    kona_command_kicked(s, 0x1fd, (uint32_t[]){ 1, 9, 0 }, 3);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_MAILBOX, ==, 0);
    assert_textport_in_step(s);
    /* kona_checkpipe's variant with only a 0x10000 kick before 0x1dd. */
    qtest_writel(s, REG_KICK, 0x10000);
    kona_command(s, 0x1dd, (uint32_t[]){ 0x105 }, 1);
    qtest_writel(s, REG_KICK, 0);
    assert_textport_in_step(s);
    qtest_writel(s, REG_COMMAND, 0x1fe);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, "textport expected a sync word"),
                     ==, 1);
    g_assert_nonnull(strstr(found, "got 0x1fe\n"));
    g_assert_nonnull(strstr(found, "token 0x1f2 args 0x0 0x9000 0x4000"));
    g_assert_nonnull(strstr(found, "token 0x1e9 args 0x0 0x0 0x0 argc 1"));
    g_assert_nonnull(strstr(found, "token 0x1fd args 0x1 0x9 0x0 argc 3"));
    g_assert_nonnull(strstr(found, "token 0x1dd args 0x105"));
}

/*
 * The kernel's pool packets. kona_mem_alloc's first
 * allocation for the minimal GLX program sends 0x1e4 2 9 0 1 inside the
 * +0xc00108 bracket, then 0x172 0 with none; a free sends 0x1e4 with
 * offset and size 0. Pool pressure adds copy-down, 0x1e1 and four words,
 * and swap-out, 0x1e3 and eight. Each is consumed whole: the trace carries
 * each region's four words in order, the write-gather flush 0x1ff sent
 * straight after the first 0x172 0 frames as its own packet, and 0x1e1's
 * words here are shaped like the sync probe, which a model that dropped
 * the token would answer on the mailbox. No word takes the unexpected-word
 * path, and the textport stays in step.
 */
static void test_kernel_pool_packets(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    const char *alloc, *sync, *flush, *freed;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "sgi_kona_kernel_*",
                                     "guest_errors", &log);

    kona_command_kicked(s, 0x1e4, (uint32_t[]){ 2, 9, 0, 1 }, 4);
    kona_command(s, 0x172, (uint32_t[]){ 0 }, 1);
    kona_command(s, 0x1ff, (uint32_t[]){ 0xdeadbeef }, 1);
    kona_command_kicked(s, 0x1e4, (uint32_t[]){ 2, 9, 0, 0 }, 4);
    kona_command(s, 0x172, (uint32_t[]){ 0 }, 1);
    kona_command_kicked(s, 0x1e1, (uint32_t[]){ 1, 9, 0, 1 }, 4);
    kona_command_kicked(s, 0x1e3,
                        (uint32_t[]){ 3, 0x10, 0x20, 0x400, 1, 0x3e80,
                                      0x9000, 0x102 }, 8);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, 0);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_null(strstr(found, "textport expected a sync word"));
    g_assert_null(strstr(found, "dropped"));
    g_assert_nonnull(strstr(found, "token 0x1e4 args 0x2 0x9 0x0 argc 4"));
    alloc = strstr(found, "class 2 key 9 offset 0x0 size 0x1\n");
    sync = strstr(found, "token 0x172 args 0x0 0x0 0x0 argc 1");
    flush = strstr(found, "token 0x1ff args 0xdeadbeef 0x0 0x0 argc 1");
    freed = strstr(found, "class 2 key 9 offset 0x0 size 0x0\n");
    g_assert_nonnull(alloc);
    g_assert_nonnull(sync);
    g_assert_nonnull(flush);
    g_assert_nonnull(freed);
    g_assert_true(alloc < sync && sync < flush && flush < freed);
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_kernel_mem_region "),
                     ==, 2);
    g_assert_cmpuint(count_occurrences(found, "token 0x172 args 0x0 0x0 0x0"
                                              " argc 1"), ==, 2);
    g_assert_nonnull(strstr(found, "token 0x1e1 args 0x1 0x9 0x0 argc 4"));
    g_assert_nonnull(strstr(found, "token 0x1e3 args 0x3 0x10 0x20 argc 8"));
}

/*
 * 0x1f1 registers the reason page: physical when bit 28 of the page word
 * is set (KONAstart's form), otherwise through the map RAM entry for the
 * device address's 16 KiB page. An unmapped device address registers
 * nothing and says so.
 */
static void test_reason_page_registration(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "sgi_kona_reason_page",
                                     "guest_errors", &log);

    kona_command_kicked(s, 0x1f1,
                        (uint32_t[]){ 0x10000ecc, 0x9000, 0x4000 }, 3);
    /* Device page 1 (0x4000-0x7fff) maps the 16 KiB page at 0x123c000. */
    qtest_writel(s, REG_MAPRAM(1), 0x20000000 | ((0x123c000 >> 14) << 2));
    kona_command(s, 0x1f1, (uint32_t[]){ 4, 0x9010, 0x4000 }, 3);
    /* kona_mapram_init()'s test pattern is not a mapping. */
    qtest_writel(s, REG_MAPRAM(2), 0x10000002);
    kona_command(s, 0x1f1, (uint32_t[]){ 8, 0x9000, 0x4000 }, 3);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "addr 0xecc000 mapped=0"));
    g_assert_nonnull(strstr(found, "addr 0x123c010 mapped=1"));
    g_assert_nonnull(strstr(found, "reason page at device address 0x8000 is"
                                   " not mapped (map RAM entry 0x10000002)"));
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_reason_page "), ==,
                     2);
}

/*
 * Reason posting: with KONAstart's page registered, KONAdestroyddrn's
 * 0x1f8 0x103 writes 0x103 big-endian to word 0 of the page, sets cause
 * bit 0x20 and sends exactly one interrupt, on the geometry-engine vector
 * 0x11; the handler's write of 0x20 clears the bit and sends nothing
 * more. Discriminates a model that posts to the page and the GE vector
 * from one that replies through the mailbox or the general vector.
 */
static void test_reason_posting(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "sgi_kona_ge_interrupt",
                                     "guest_errors", &log);

    kona_program_interrupts(s);
    guest_fill_word(s, REASON_PAGE, 0xffffffff);
    guest_fill_word(s, REASON_PAGE + 4, 0x5a5a5a5a);
    kona_register_page(s, REASON_PAGE);
    g_assert_false(hub_pending(s, GE_VECTOR));

    kona_command_kicked(s, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x00000103);
    g_assert_cmphex(guest_word(s, REASON_PAGE + 4), ==, 0x5a5a5a5a);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, INT_CAUSE_GE);
    g_assert_true(hub_pending(s, GE_VECTOR));
    g_assert_false(hub_pending(s, GENERAL_VECTOR));

    qtest_writel(s, REG_INT_CAUSE, INT_CAUSE_GE);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, 0);
    g_assert_true(hub_pending(s, GE_VECTOR));

    /* kona_checkpipe's 0x1dd posts its reason through the same page. */
    hub_write(s, PI_INT_MODIFY, GE_VECTOR);
    qtest_writel(s, REG_KICK, 0x10000);
    kona_command(s, 0x1dd, (uint32_t[]){ 0x105 }, 1);
    qtest_writel(s, REG_KICK, 0);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x00000105);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, INT_CAUSE_GE);
    g_assert_true(hub_pending(s, GE_VECTOR));
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_ge_interrupt "), ==,
                     2);
    g_assert_nonnull(strstr(found, "vector 0x11"));
}

/*
 * No page, no post: 0x1f8 0x103 before any 0x1f1 writes no guest memory,
 * sets no cause bit, raises nothing, and is logged as a guest error as
 * well as traced.
 */
static void test_no_reason_page_no_post(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS,
                                     "sgi_kona_reason_no_page",
                                     "guest_errors,unimp", &log);

    kona_program_interrupts(s);
    guest_fill_word(s, REASON_PAGE, 0xffffffff);
    guest_fill_word(s, 0, 0xeeeeeeee);
    kona_command_kicked(s, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0xffffffff);
    g_assert_cmphex(guest_word(s, 0), ==, 0xeeeeeeee);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, 0);
    g_assert_false(hub_pending(s, GE_VECTOR));
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "sgi_kona_reason_no_page token 0x1f8 "
                                   "reason 0x103 with no reason page "
                                   "registered"));
    g_assert_nonnull(strstr(found, "sgi-kona: token 0x1f8 reason 0x103 with "
                                   "no reason page registered"));
}

/*
 * The X server process's 0x1f2 0 0x9000 0x4000 and 0x1e9 0 between
 * KONAstart and the teardown leave the stream in step: 0x1e9 answers on
 * 0x1f2's page and 0x1f8 0x103 still posts to the page 0x1f1 registered.
 */
static void test_process_commands_then_post(void)
{
    kona_reset();
    kona_program_interrupts(qts);
    qtest_writel(qts, REG_MAPRAM(0), 0x20000000 | ((OTHER_PAGE >> 14) << 2));
    guest_fill_word(qts, REASON_PAGE, 0);
    guest_fill_word(qts, OTHER_PAGE, 0);
    kona_register_page(qts, REASON_PAGE);
    kona_command(qts, 0x1f2, (uint32_t[]){ 0, 0x9000, 0x4000 }, 3);
    kona_command(qts, 0x1e9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(qts, OTHER_PAGE), ==, 1);
    g_assert_cmphex(guest_word(qts, REASON_PAGE), ==, 0);
    kona_command_kicked(qts, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    g_assert_cmphex(guest_word(qts, REASON_PAGE), ==, 0x103);
    g_assert_cmphex(guest_word(qts, OTHER_PAGE), ==, 1);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE) & INT_CAUSE_GE, ==,
                    INT_CAUSE_GE);
    g_assert_true(hub_pending(qts, GE_VECTOR));
}

/*
 * KONApcxswap's context deactivate, as a run traced it: kick 0x30000,
 * 0x1fc, kick 0x10000, the outgoing context's index, and no closing kick
 * before the driver sleeps.
 */
static void kona_context_deactivate(QTestState *s, uint32_t index)
{
    qtest_writel(s, REG_KICK, 0x30000);
    qtest_writel(s, REG_COMMAND, 0x1fc);
    qtest_writel(s, REG_KICK, 0x10000);
    qtest_writel(s, REG_COMMAND, index);
}

/*
 * The deactivate's completion: reason 0x101 in word 0 of the reason page,
 * the geometry-engine cause bit and one interrupt on its vector, which
 * kona_ge_intr takes as the wakeup for the sleeping swap. The handler's
 * write of 0x20 clears the bit. Discriminates a model that answers 0x1fc
 * from one that drops it, which leaves the swap to its 90 s timeout.
 */
static void test_context_deactivate_posts(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS,
                                     "sgi_kona_context_deactivate",
                                     "guest_errors", &log);

    kona_program_interrupts(s);
    guest_fill_word(s, REASON_PAGE, 0xffffffff);
    guest_fill_word(s, REASON_PAGE + 4, 0x5a5a5a5a);
    kona_register_page(s, REASON_PAGE);
    g_assert_false(hub_pending(s, GE_VECTOR));

    kona_context_deactivate(s, 1);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x00000101);
    g_assert_cmphex(guest_word(s, REASON_PAGE + 4), ==, 0x5a5a5a5a);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, INT_CAUSE_GE);
    g_assert_true(hub_pending(s, GE_VECTOR));
    g_assert_false(hub_pending(s, GENERAL_VECTOR));

    qtest_writel(s, REG_INT_CAUSE, INT_CAUSE_GE);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, 0);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found,
                                       "sgi_kona_context_deactivate "), ==, 1);
    g_assert_nonnull(strstr(found, "outgoing context 1"));
    g_assert_null(strstr(found, "textport expected a sync word"));
}

/*
 * The word after 0x1fc is its argument: an index of 1 must not open a
 * textport packet, so a 0x1f8 0x103 sent next still posts 0x103 and the
 * textport's own sync probe still answers afterwards.
 */
static void test_context_deactivate_argument_framed(void)
{
    kona_reset();
    kona_program_interrupts(qts);
    guest_fill_word(qts, REASON_PAGE, 0);
    kona_register_page(qts, REASON_PAGE);

    kona_context_deactivate(qts, 1);
    qtest_writel(qts, REG_INT_CAUSE, INT_CAUSE_GE);
    kona_command_kicked(qts, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    g_assert_cmphex(guest_word(qts, REASON_PAGE), ==, 0x103);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE) & INT_CAUSE_GE, ==,
                    INT_CAUSE_GE);
    assert_textport_in_step(qts);
}

/*
 * KONAstart kills the pipe if +0x800238 shows bit 0x100, so the cause must
 * come out of reset without it; and reset forgets the reason page and the
 * interrupt registers, so a post afterwards writes nothing. A guard: the
 * model before this change also read zero here.
 */
static void test_reset_clears_cause_and_reason_page(void)
{
    kona_reset();
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE) &
                    INT_CAUSE_KONASTART_DIES, ==, 0);

    kona_program_interrupts(qts);
    kona_register_page(qts, REASON_PAGE);
    kona_command_kicked(qts, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, INT_CAUSE_GE);

    kona_reset();
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_INT_VECTORS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_INT_DEST_UPPER), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_INT_DEST_LOWER), ==, 0);
    guest_fill_word(qts, REASON_PAGE, 0);
    kona_command_kicked(qts, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    g_assert_cmphex(guest_word(qts, REASON_PAGE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, 0);
}

/*
 * The activity latch: kona_callout's read-then-write-4 round sees bit 0x4
 * only when a command-port, DMA FIFO or load word arrived since the
 * previous round. kona_die's write of 8 neither sets nor clears it and
 * stays on the unimplemented-write log.
 */
static void test_activity_latch(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "sgi_kona_kernel_command",
                                     "unimp", &log);

    g_assert_cmphex(qtest_readl(s, REG_ACTIVITY), ==, 0);
    kona_command(s, 0x1e9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(qtest_readl(s, REG_ACTIVITY), ==, ACTIVITY_BUS);
    qtest_writel(s, REG_ACTIVITY, ACTIVITY_BUS);
    g_assert_cmphex(qtest_readl(s, REG_ACTIVITY), ==, 0);
    /* An idle round stays idle. */
    qtest_writel(s, REG_ACTIVITY, ACTIVITY_BUS);
    g_assert_cmphex(qtest_readl(s, REG_ACTIVITY), ==, 0);

    port_load(s, ARM_HIGH_BASE, (uint32_t[]){ 1 }, 1);
    g_assert_cmphex(qtest_readl(s, REG_ACTIVITY), ==, ACTIVITY_BUS);
    qtest_writel(s, REG_ACTIVITY, 8);
    g_assert_cmphex(qtest_readl(s, REG_ACTIVITY), ==, ACTIVITY_BUS);
    qtest_writel(s, REG_ACTIVITY, ACTIVITY_BUS);

    /* Address selects and reads are not consumed words. */
    port_peek(s, ARM_HIGH_BASE);
    g_assert_cmphex(qtest_readl(s, REG_ACTIVITY), ==, 0);
    qtest_writel(s, REG_DMA_FIFO_PUSH, 0x1eb);
    g_assert_cmphex(qtest_readl(s, REG_ACTIVITY), ==, ACTIVITY_BUS);

    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, REG_ACTIVITY), ==, 0);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_null(strstr(found, "unimplemented read of offset 0x8003a8"));
    g_assert_nonnull(strstr(found, "unimplemented write of offset 0x8003a8 "
                                   "(value 0x8)"));
    g_assert_null(strstr(found, "unimplemented write of offset 0x8003a8 "
                                "(value 0x4)"));
}

/*
 * The 0x1e9 answer placement: map RAM entry 0 = 0x20003e80, then
 * gfxinit's 0x1f2 0 0x9000 0x4000 and 0x1e9 0. Host page 0x3e80000 holds
 * the configuration block from byte 8 and a non-zero word 0, word 1 and
 * the bytes past the block are untouched, and the trace shows the block
 * written before word 0.
 */
static void test_config_answer_placement(void)
{
    const uint64_t page = UINT64_C(0x3e80000);
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    const char *block_write, *ready_write;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "sgi_kona_host_write",
                                     "guest_errors", &log);

    guest_fill(s, page, 0x5a, 0x1000);
    guest_fill_word(s, page, 0);
    qtest_writel(s, REG_MAPRAM(0), 0x20003e80);
    kona_command(s, 0x1f2, (uint32_t[]){ 0, 0x9000, 0x4000 }, 3);
    g_assert_cmphex(guest_word(s, page), ==, 0);
    kona_command(s, 0x1e9, (uint32_t[]){ 0 }, 1);

    g_assert_cmphex(guest_word(s, page), ==, 1);
    g_assert_cmphex(guest_word(s, page + 4), ==, 0x5a5a5a5a);
    assert_config_block(s, page);
    g_assert_cmphex(guest_word(s, page + CONFIG_OFFSET + CONFIG_SIZE), ==,
                    0x5a5a5a5a);
    /* The answer raises nothing: the process polls word 0. */
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, 0);
    qtest_quit(s);

    found = kona_log_read(log);
    block_write = strstr(found, "addr 0x3e80008 len 0x778");
    ready_write = strstr(found, "addr 0x3e80000 len 0x4");
    g_assert_nonnull(block_write);
    g_assert_nonnull(ready_write);
    g_assert_true(block_write < ready_write);
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_host_write "), ==, 2);
}

/*
 * The pool descriptors in the 0x1e9 answer, against the limits the
 * kernel's allocator puts on them ("Memory-pool descriptors in the
 * configuration block"): each granule is non-zero and divides 0x400, the
 * library's own display-list block size; each largest
 * alignment is a multiple of the granule and at most 255 granules, since
 * the kernel keeps the quotient in a byte; class 2, which the library's
 * first make-current allocates with key 9, takes that key and is not
 * shared; class 1, the display lists, is shared; and class 3's granule
 * times capacity is the 64 MiB of texture memory the library reads back.
 * A block with zero descriptors fails the first check: the kernel divides
 * by that granule on the class's first allocation.
 */
static void test_config_pool_descriptors(void)
{
    kona_reset();
    guest_fill(qts, RETURN_PAGE, 0x5a, 0x1000);
    guest_fill_word(qts, RETURN_PAGE, 0);
    kona_register_return_page(qts, RETURN_PAGE);
    kona_command(qts, 0x1e9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE), ==, 1);

    for (unsigned c = 0; c < POOL_CLASSES; c++) {
        uint64_t base = RETURN_PAGE + CONFIG_OFFSET + POOL_OFFSET +
                        POOL_STRIDE * c;
        uint32_t word[POOL_WORDS];

        for (unsigned w = 0; w < POOL_WORDS; w++) {
            word[w] = guest_word(qts, base + 4 * w);
        }
        g_assert_cmphex(word[POOL_GRANULE], !=, 0);
        g_assert_cmphex(0x400 % word[POOL_GRANULE], ==, 0);
        g_assert_cmphex(word[POOL_MAX_ALIGN] % word[POOL_GRANULE], ==, 0);
        g_assert_cmpuint(word[POOL_MAX_ALIGN] / word[POOL_GRANULE], <=, 255);
        for (unsigned w = 0; w < POOL_WORDS; w++) {
            g_assert_cmphex(word[w], ==, pool_words[c][w]);
        }
        if (c == 1) {
            g_assert_cmphex(word[POOL_SHARED], ==, 1);
        }
        if (c == 2) {
            g_assert_cmphex(word[POOL_SHARED], ==, 0);
            g_assert_cmpuint(word[POOL_MAX_KEY], >=, 9);
        }
        if (c == 3) {
            g_assert_cmpuint((uint64_t)word[POOL_GRANULE] *
                             word[POOL_CAPACITY], ==, TEXTURE_BYTES);
        }
    }
}

/*
 * KONApcxswap's physical form of 0x1f2: bit 28 of the page word set, the
 * page frame below it and the byte offset in the second word. A second
 * 0x1f2 replaces the first: the model keeps one return page.
 */
static void test_return_page_physical(void)
{
    kona_reset();
    guest_fill_word(qts, RETURN_PAGE, 0);
    guest_fill_word(qts, OTHER_PAGE, 0);
    kona_register_return_page(qts, RETURN_PAGE);
    kona_command(qts, 0x1e9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE), ==, 1);
    assert_config_block(qts, RETURN_PAGE);

    guest_fill_word(qts, RETURN_PAGE, 0);
    kona_register_return_page(qts, OTHER_PAGE);
    kona_command(qts, 0x1e9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(qts, OTHER_PAGE), ==, 1);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE), ==, 0);
}

/*
 * No return page, no answer: 0x1e9 before any 0x1f2 writes no guest
 * memory, raises nothing and says so; so does 0x1f2 naming an unmapped
 * device page, which registers nothing.
 */
static void test_no_return_page_no_post(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "sgi_kona_host_write",
                                     "guest_errors", &log);

    guest_fill_word(s, 0, 0xeeeeeeee);
    guest_fill_word(s, 8, 0xeeeeeeee);
    kona_command(s, 0x1e9, (uint32_t[]){ 0 }, 1);
    kona_command(s, 0x1f2, (uint32_t[]){ 0, 0x9000, 0x4000 }, 3);
    kona_command(s, 0x1e9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(s, 0), ==, 0xeeeeeeee);
    g_assert_cmphex(guest_word(s, 8), ==, 0xeeeeeeee);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, 0);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, "token 0x1e9 with no return "
                                       "page registered"), ==, 2);
    g_assert_nonnull(strstr(found, "return page at device address 0x0 is not "
                                   "mapped"));
    g_assert_null(strstr(found, "sgi_kona_host_write "));
}

/* Reset forgets the return page: a later 0x1e9 writes nothing. */
static void test_reset_clears_return_page(void)
{
    kona_reset();
    guest_fill_word(qts, RETURN_PAGE, 0);
    kona_register_return_page(qts, RETURN_PAGE);
    kona_command(qts, 0x1e9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE), ==, 1);
    kona_reset();
    guest_fill_word(qts, RETURN_PAGE, 0);
    guest_fill_word(qts, RETURN_PAGE + CONFIG_OFFSET + 0x14, 0x77777777);
    kona_command(qts, 0x1e9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE), ==, 0);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + CONFIG_OFFSET + 0x14), ==,
                    0x77777777);
}

/*
 * X initialise, sent as plain words: after a physical 0x1f2, 0x128 W H D
 * writes W, H and D to words 2 to 4 of that page, then 1 to word 0, and
 * leaves words 1 and 5 alone. With no return page it logs and writes
 * nothing.
 */
static void test_x_initialise_echo(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    const char *echo_write, *ready_write;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "sgi_kona_host_write",
                                     "guest_errors", &log);

    guest_fill(s, RETURN_PAGE, 0x5a, 0x40);
    kona_command(s, 0x128, (uint32_t[]){ 1280, 1024, 3 }, 3);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 0x5a5a5a5a);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, 0x5a5a5a5a);

    guest_fill_word(s, RETURN_PAGE, 0);
    kona_register_return_page(s, RETURN_PAGE);
    kona_command(s, 0x128, (uint32_t[]){ 1280, 1024, 3 }, 3);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 1);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 4), ==, 0x5a5a5a5a);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, 1280);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 12), ==, 1024);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 16), ==, 3);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 20), ==, 0x5a5a5a5a);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "token 0x128 with no return page "
                                   "registered"));
    echo_write = strstr(found, "addr 0x320008 len 0xc");
    ready_write = strstr(found, "addr 0x320000 len 0x4");
    g_assert_nonnull(echo_write);
    g_assert_nonnull(ready_write);
    g_assert_true(echo_write < ready_write);
}

/*
 * The X server's display tokens and the kernel's 0x1e6 are consumed
 * whole with a trace each and change nothing else: arguments shaped like
 * the textport's sync probe (1 9 0) queue no mailbox reply, the return
 * page is untouched and the textport stays in step afterwards.
 */
static void test_display_tokens_consumed(void)
{
    static const struct {
        uint32_t token;
        unsigned words;
    } tokens[] = {
        { 0x1e6, 3 }, { 0x132, 2 }, { 0x1ff, 1 },
    };
    static const uint32_t probe_shaped[] = { 1, 9, 0, 1, 9, 0, 1 };
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    g_autofree uint8_t *before = g_malloc(0x40);
    g_autofree uint8_t *after = g_malloc(0x40);
    QTestState *s = kona_init_logged(MACHINE_ARGS,
                                     "sgi_kona_command_unmodelled",
                                     "guest_errors", &log);

    guest_fill(s, RETURN_PAGE, 0x5a, 0x40);
    kona_register_return_page(s, RETURN_PAGE);
    qtest_memread(s, RETURN_PAGE, before, 0x40);
    for (size_t i = 0; i < ARRAY_SIZE(tokens); i++) {
        kona_command(s, tokens[i].token, probe_shaped, tokens[i].words);
    }
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, 0);
    qtest_memread(s, RETURN_PAGE, after, 0x40);
    g_assert_cmpmem(before, 0x40, after, 0x40);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    for (size_t i = 0; i < ARRAY_SIZE(tokens); i++) {
        g_autofree char *line = g_strdup_printf(
            "token 0x%x argc %u consumed", tokens[i].token, tokens[i].words);

        g_assert_nonnull(strstr(found, line));
    }
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_command_unmodelled "),
                     ==, ARRAY_SIZE(tokens));
    g_assert_null(strstr(found, "textport expected a sync word"));
}

/*
 * Address decode: W at +0xc1250c, then H and D at +0xc1000c, frame
 * as 0x128 W H D, observed through the X initialise echo. kon_InitHW's
 * write-gather flush at +0xc13fec frames as 0x1ff 0xdeadbeef.
 */
static void test_address_decode(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "sgi_kona_*command*",
                                     "guest_errors,unimp", &log);

    guest_fill(s, RETURN_PAGE, 0x5a, 0x20);
    guest_fill_word(s, RETURN_PAGE, 0);
    kona_register_return_page(s, RETURN_PAGE);
    qtest_writel(s, KONA_REG(0xc1250c), 1280);
    qtest_writel(s, REG_COMMAND_DATA, 1024);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 0);
    qtest_writel(s, REG_COMMAND_DATA, 3);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 1);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, 1280);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 12), ==, 1024);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 16), ==, 3);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 20), ==, 0x5a5a5a5a);
    qtest_writel(s, KONA_REG(0xc13fec), 0xdeadbeef);
    g_assert_cmphex(qtest_readl(s, REG_ACTIVITY), ==, ACTIVITY_BUS);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "token 0x128 args 0x500 0x400 0x3 "
                                   "argc 3"));
    g_assert_nonnull(strstr(found, "token 0x1ff args 0xdeadbeef 0x0 0x0 "
                                   "argc 1"));
    g_assert_nonnull(strstr(found, "token 0x1ff argc 1 consumed"));
    g_assert_null(strstr(found, "unimplemented write"));
    g_assert_null(strstr(found, "command page"));
    g_assert_null(strstr(found, "sync word"));
}

/*
 * The data slot carries plain words: gfxinit's 0x1f2 and 0x1e9 sent as
 * words at +0xc1000c frame exactly as they do at +0xc10000, and a data
 * word opening an unknown token takes the same unexpected-word path.
 */
static void test_data_slot_tokens(void)
{
    const uint32_t words[] = {
        0x1f2, 0x10000000 | (uint32_t)(RETURN_PAGE >> 12), 0x9000, 0x4000,
        0x1e9, 0,
    };
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "sgi_kona_return_post",
                                     "guest_errors,unimp", &log);

    guest_fill_word(s, RETURN_PAGE, 0);
    for (size_t i = 0; i < ARRAY_SIZE(words); i++) {
        qtest_writel(s, REG_COMMAND_DATA, words[i]);
    }
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 1);
    assert_config_block(s, RETURN_PAGE);
    qtest_writel(s, REG_COMMAND_DATA, 0x1fe);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "token 0x1e9 page 0x320000"));
    g_assert_nonnull(strstr(found, "textport expected a sync word, got "
                                   "0x1fe\n"));
    g_assert_null(strstr(found, "unimplemented write"));
}

/*
 * Stores the model cannot place are logged and dropped without
 * disturbing later framing: a slot naming a token with no word count, a
 * store elsewhere in a slot or in the first slot, and a slot store while
 * a command is still open, whose partial command is dropped rather than
 * completed by the next words.
 */
static void test_slot_stores_logged(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "sgi_kona_kernel_command",
                                     "guest_errors,unimp", &log);

    guest_fill(s, RETURN_PAGE, 0x5a, 0x20);
    guest_fill_word(s, RETURN_PAGE, 0);
    kona_register_return_page(s, RETURN_PAGE);
    qtest_writel(s, REG_COMMAND_SLOT(0x100), 0x11111111);
    qtest_writel(s, KONA_REG(0xc12500), 0x22222222);
    qtest_writel(s, KONA_REG(0xc12510), 0x33333333);
    qtest_writel(s, KONA_REG(0xc10004), 0x44444444);
    assert_textport_in_step(s);

    /* 0x128 left one word short, then the flush's slot store. */
    qtest_writel(s, REG_COMMAND_SLOT(0x128), 640);
    qtest_writel(s, REG_COMMAND_DATA, 480);
    qtest_writel(s, REG_COMMAND_SLOT(0x1ff), 0xdeadbeef);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 0);
    qtest_writel(s, REG_COMMAND_SLOT(0x128), 1280);
    qtest_writel(s, REG_COMMAND_DATA, 1024);
    qtest_writel(s, REG_COMMAND_DATA, 1);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 1);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, 1280);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 12), ==, 1024);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 16), ==, 1);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "unknown token 0x100 stored at command "
                                   "page offset 0x200c; value 0x11111111 "
                                   "dropped"));
    g_assert_nonnull(strstr(found, "unexpected store of 0x22222222 at "
                                   "command page offset 0x2500; dropped"));
    g_assert_nonnull(strstr(found, "unexpected store of 0x33333333 at "
                                   "command page offset 0x2510; dropped"));
    g_assert_nonnull(strstr(found, "unexpected store of 0x44444444 at "
                                   "command page offset 0x4; dropped"));
    g_assert_nonnull(strstr(found, "token 0x1ff stored at command page "
                                   "offset 0x3fec while token 0x128 still "
                                   "needs 1 words; partial command "
                                   "dropped"));
    g_assert_null(strstr(found, "args 0x280"));
    g_assert_null(strstr(found, "unimplemented write"));
}

/*
 * The variable colour-map length test: 0x12e with start 0
 * and count 3, then three entries, frames as one command and the next
 * word opens a new one. The entries are shaped like 0x1e9 and its
 * argument, so a model that took fewer words would answer a 0x1e9. Sent
 * both as plain words and with the start index in the token's slot.
 */
static void test_color_map_variable_length(void)
{
    static const uint32_t entries[] = { 0x1e9, 0, 0x1e9 };
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "sgi_kona_*",
                                     "guest_errors,unimp", &log);

    kona_register_return_page(s, RETURN_PAGE);
    guest_fill_word(s, RETURN_PAGE, 0);
    kona_command(s, 0x12e, (uint32_t[]){ 0, 3, 0x1e9, 0, 0x1e9 }, 5);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 0);
    assert_textport_in_step(s);

    qtest_writel(s, REG_COMMAND_SLOT(0x12e), 0);
    qtest_writel(s, REG_COMMAND_DATA, 3);
    for (size_t i = 0; i < ARRAY_SIZE(entries); i++) {
        qtest_writel(s, REG_COMMAND_DATA, entries[i]);
    }
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 0);
    kona_command(s, 0x1e9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 1);
    assert_textport_in_step(s);

    /* A slot store before the entries are in drops the partial load. */
    qtest_writel(s, REG_COMMAND_SLOT(0x12e), 0);
    qtest_writel(s, REG_COMMAND_DATA, 3);
    qtest_writel(s, REG_COMMAND_DATA, 7);
    qtest_writel(s, REG_COMMAND_SLOT(0x1ff), 0xdeadbeef);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_color_map start 0 "
                                       "count 3"), ==, 3);
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_color_map_done "
                                       "start 0x0 count 3 outside 0"),
                     ==, 2);
    g_assert_nonnull(strstr(found, "token 0x1ff stored at command page "
                                   "offset 0x3fec while token 0x12e still "
                                   "needs 2 entries; partial command "
                                   "dropped"));
    g_assert_null(strstr(found, "sync word"));
}

/*
 * Words the model cannot frame are logged with their values and traced:
 * an unknown token's argument words, arriving as fresh words, and an
 * unknown token's slot store.
 */
static void test_dropped_words_traced(void)
{
    static const uint32_t dropped[] = { 0x1fe, 5, 6 };
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "sgi_kona_word_dropped",
                                     "guest_errors,unimp", &log);

    kona_command(s, 0x1fe, (uint32_t[]){ 5, 6 }, 2);
    qtest_writel(s, REG_COMMAND_SLOT(0x100), 0x11111111);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    for (size_t i = 0; i < ARRAY_SIZE(dropped); i++) {
        g_autofree char *line = g_strdup_printf(
            "offset 0x0 value 0x%x not a token or sync word", dropped[i]);
        g_autofree char *logged = g_strdup_printf(
            "textport expected a sync word, got 0x%x\n", dropped[i]);

        g_assert_nonnull(strstr(found, line));
        g_assert_nonnull(strstr(found, logged));
    }
    g_assert_nonnull(strstr(found, "offset 0x200c value 0x11111111 unknown "
                                   "token"));
    g_assert_nonnull(strstr(found, "unknown token 0x100 stored at command "
                                   "page offset 0x200c; value 0x11111111 "
                                   "dropped"));
}

/*
 * A command opened by a slot store and still collecting at the migration
 * point completes on the destination.
 */
static void test_migration_mid_slot_command(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-slot-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    kona_register_return_page(from, RETURN_PAGE);
    guest_fill_word(from, RETURN_PAGE, 0);
    qtest_writel(from, REG_COMMAND_SLOT(0x128), 1280);
    qtest_writel(from, REG_COMMAND_DATA, 1024);

    sgi_migrate_stopped(from, uri);

    qtest_writel(to, REG_COMMAND_DATA, 2);
    g_assert_cmphex(guest_word(to, RETURN_PAGE), ==, 1);
    g_assert_cmphex(guest_word(to, RETURN_PAGE + 8), ==, 1280);
    g_assert_cmphex(guest_word(to, RETURN_PAGE + 12), ==, 1024);
    g_assert_cmphex(guest_word(to, RETURN_PAGE + 16), ==, 2);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * A version 3 stream predates every field above. Loading one leaves them
 * at their reset default and still carries the version 3 state (a map RAM
 * entry here). Skipped unless QTEST_QEMU_BINARY_KONA_V3 names a
 * qemu-system-mips64 built before the version 4 fields existed, the same
 * pattern sn0-origin2000-kona-test.c's QTEST_QEMU_BINARY_KONA_V1 uses.
 */
static void test_migration_from_v3(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    if (!g_getenv("QTEST_QEMU_BINARY_KONA_V3")) {
        g_test_skip("QTEST_QEMU_BINARY_KONA_V3 is not set");
        return;
    }

    tmpfs = g_dir_make_tmp("sn0-kona-v3-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init_ext("QTEST_QEMU_BINARY_KONA_V3", MIGRATION_ARGS, NULL,
                          true);
    qtest_writel(from, REG_MAPRAM(7), 0x20001234);
    port_select(from, ARM_HIGH_BASE);

    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);
    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(qtest_readl(to, REG_MAPRAM(7)), ==, 0x20001234);
    g_assert_cmphex(qtest_readl(to, REG_PORT_ADDR), ==, ARM_HIGH_BASE);
    g_assert_cmphex(qtest_readl(to, REG_PORT_DATA), ==, 0);
    g_assert_cmphex(qtest_readl(to, REG_LOADER_3B0), ==, 0);
    /* No return page came with the stream: 0x1e9 answers nowhere. */
    guest_fill_word(to, RETURN_PAGE, 0);
    kona_command(to, 0x1e9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(to, RETURN_PAGE), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qts = qtest_init(MACHINE_ARGS);

    qtest_add_func("sn0/origin2000/kona-bringup/port-stride",
                   test_port_stride);
    qtest_add_func("sn0/origin2000/kona-bringup/port-read-does-not-advance",
                   test_port_read_does_not_advance);
    qtest_add_func("sn0/origin2000/kona-bringup/eeprom-word-still-answers",
                   test_eeprom_word_still_answers);
    qtest_add_func("sn0/origin2000/kona-bringup/"
                   "port-unbacked-addresses-read-zero",
                   test_port_unbacked_addresses_read_zero);
    qtest_add_func("sn0/origin2000/kona-bringup/"
                   "load-record-traced-at-arm-release",
                   test_load_record_traced_at_arm_release);
    qtest_add_func("sn0/origin2000/kona-bringup/loader-registers-are-stored",
                   test_loader_registers_are_stored);
    qtest_add_func("sn0/origin2000/kona-bringup/reset-clears-load-state",
                   test_reset_clears_load_state);
    qtest_add_func("sn0/origin2000/kona-bringup/kernel-command-framing",
                   test_kernel_command_framing);
    qtest_add_func("sn0/origin2000/kona-bringup/kernel-pool-packets",
                   test_kernel_pool_packets);
    qtest_add_func("sn0/origin2000/kona-bringup/reason-page-registration",
                   test_reason_page_registration);
    qtest_add_func("sn0/origin2000/kona-bringup/reason-posting",
                   test_reason_posting);
    qtest_add_func("sn0/origin2000/kona-bringup/no-reason-page-no-post",
                   test_no_reason_page_no_post);
    qtest_add_func("sn0/origin2000/kona-bringup/process-commands-then-post",
                   test_process_commands_then_post);
    qtest_add_func("sn0/origin2000/kona-bringup/context-deactivate-posts",
                   test_context_deactivate_posts);
    qtest_add_func("sn0/origin2000/kona-bringup/"
                   "context-deactivate-argument-framed",
                   test_context_deactivate_argument_framed);
    qtest_add_func("sn0/origin2000/kona-bringup/"
                   "reset-clears-cause-and-reason-page",
                   test_reset_clears_cause_and_reason_page);
    qtest_add_func("sn0/origin2000/kona-bringup/activity-latch",
                   test_activity_latch);
    qtest_add_func("sn0/origin2000/kona-bringup/config-answer-placement",
                   test_config_answer_placement);
    qtest_add_func("sn0/origin2000/kona-bringup/config-pool-descriptors",
                   test_config_pool_descriptors);
    qtest_add_func("sn0/origin2000/kona-bringup/return-page-physical",
                   test_return_page_physical);
    qtest_add_func("sn0/origin2000/kona-bringup/no-return-page-no-post",
                   test_no_return_page_no_post);
    qtest_add_func("sn0/origin2000/kona-bringup/reset-clears-return-page",
                   test_reset_clears_return_page);
    qtest_add_func("sn0/origin2000/kona-bringup/x-initialise-echo",
                   test_x_initialise_echo);
    qtest_add_func("sn0/origin2000/kona-bringup/display-tokens-consumed",
                   test_display_tokens_consumed);
    qtest_add_func("sn0/origin2000/kona-bringup/address-decode",
                   test_address_decode);
    qtest_add_func("sn0/origin2000/kona-bringup/data-slot-tokens",
                   test_data_slot_tokens);
    qtest_add_func("sn0/origin2000/kona-bringup/slot-stores-logged",
                   test_slot_stores_logged);
    qtest_add_func("sn0/origin2000/kona-bringup/color-map-variable-length",
                   test_color_map_variable_length);
    qtest_add_func("sn0/origin2000/kona-bringup/dropped-words-traced",
                   test_dropped_words_traced);
    qtest_add_func("sn0/origin2000/kona-bringup/migration-of-bringup-state",
                   test_migration_of_bringup_state);
    qtest_add_func("sn0/origin2000/kona-bringup/migration-mid-slot-command",
                   test_migration_mid_slot_command);
    qtest_add_func("sn0/origin2000/kona-bringup/migration-from-v3",
                   test_migration_from_v3);

    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
