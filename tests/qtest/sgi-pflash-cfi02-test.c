/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest testcases for the parallel flash corrections carried out of tree.
 *
 * Copyright (c) 2019 Stephen Checkoway
 *
 * The command-sequence helpers below are copied from pflash-cfi02-test.c so
 * that this file can be split off without changing that upstream test. The
 * test cases exercise behaviour we added to hw/block/pflash_cfi02.c: a reset
 * that returns the chip to read-array mode, and a read-only backend that
 * refuses and reports every mutation. They live here, rather than appended to
 * the upstream file, so the upstream test stays byte-for-byte identical to the
 * QEMU project's copy.
 *
 * The upstream musicpal machine these cases first used has since been removed,
 * so they run on the sh4 r2d machine, the only in-tree machine that still wires
 * up a pflash_cfi02 device. r2d's SH4 CPU is non-migratable, so the companion
 * migration test cannot run here; pflash_cfi02's migrated command state is a
 * VMStateDescription upstream already carries.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/*
 * These need to match the flash size and address in r2d.c. The r2d flash is a
 * single 16-bit device with uniform 64 KiB sectors.
 */
#define BASE_ADDR 0x00000000

#define UNIFORM_FLASH_SIZE (16 * 1024 * 1024)
#define UNIFORM_FLASH_SECTOR_SIZE (64 * 1024)

/* This must match the id0 arg to pflash_cfi02_register() in r2d.c. */
#define FLASH_ID0 0x0001

/* Use a newtype to keep flash addresses separate from byte addresses. */
typedef struct {
    uint64_t addr;
} faddr;
#define FLASH_ADDR(x) ((faddr) { .addr = (x) })

#define CFI_ADDR FLASH_ADDR(0x55)
#define UNLOCK0_ADDR FLASH_ADDR(0x555)
#define UNLOCK1_ADDR FLASH_ADDR(0x2AA)

#define CFI_CMD 0x98
#define UNLOCK0_CMD 0xAA
#define UNLOCK1_CMD 0x55
#define SECOND_UNLOCK_CMD 0x80
#define AUTOSELECT_CMD 0x90
#define RESET_CMD 0xF0
#define PROGRAM_CMD 0xA0
#define SECTOR_ERASE_CMD 0x30
#define CHIP_ERASE_CMD 0x10

typedef struct {
    int bank_width;

    /* Nonuniform block size. */
    int nb_blocs[4];
    int sector_len[4];

    QTestState *qtest;
} FlashConfig;

static char *image_path;

/*
 * The pflash implementation allows some parameters to be unspecified. We want
 * to test those configurations but we also need to know the real values in
 * our testing code. So after we launch qemu, we'll need a new FlashConfig
 * with the correct values filled in.
 */
static FlashConfig expand_config_defaults(const FlashConfig *c)
{
    FlashConfig ret = *c;

    if (ret.bank_width == 0) {
        ret.bank_width = 2;
    }
    if (ret.nb_blocs[0] == 0 && ret.sector_len[0] == 0) {
        ret.sector_len[0] = UNIFORM_FLASH_SECTOR_SIZE;
        ret.nb_blocs[0] = UNIFORM_FLASH_SIZE / UNIFORM_FLASH_SECTOR_SIZE;
    }

    /* XXX: Limitations of test harness. */
    assert(ret.bank_width == 2);
    return ret;
}

/*
 * Return a bit mask suitable for extracting the least significant
 * status/query response from an interleaved response.
 */
static inline uint64_t device_mask(const FlashConfig *c)
{
    return (uint64_t)-1;
}

/*
 * Return a bit mask exactly as long as the bank_width.
 */
static inline uint64_t bank_mask(const FlashConfig *c)
{
    if (c->bank_width == 8) {
        return (uint64_t)-1;
    }
    return (1ULL << (c->bank_width * 8)) - 1ULL;
}

static inline void flash_write(const FlashConfig *c, uint64_t byte_addr,
                               uint64_t data)
{
    /* Sanity check our tests. */
    assert((data & ~bank_mask(c)) == 0);
    uint64_t addr = BASE_ADDR + byte_addr;
    switch (c->bank_width) {
    case 1:
        qtest_writeb(c->qtest, addr, data);
        break;
    case 2:
        qtest_writew(c->qtest, addr, data);
        break;
    case 4:
        qtest_writel(c->qtest, addr, data);
        break;
    case 8:
        qtest_writeq(c->qtest, addr, data);
        break;
    default:
        abort();
    }
}

static inline uint64_t flash_read(const FlashConfig *c, uint64_t byte_addr)
{
    uint64_t addr = BASE_ADDR + byte_addr;
    switch (c->bank_width) {
    case 1:
        return qtest_readb(c->qtest, addr);
    case 2:
        return qtest_readw(c->qtest, addr);
    case 4:
        return qtest_readl(c->qtest, addr);
    case 8:
        return qtest_readq(c->qtest, addr);
    default:
        abort();
    }
}

/*
 * Convert a flash address expressed in the maximum width of the device as a
 * byte address.
 */
static inline uint64_t as_byte_addr(const FlashConfig *c, faddr flash_addr)
{
    /*
     * Command addresses are always given as addresses in the maximum
     * supported bus size for the flash chip. So an x8/x16 chip in x8 mode
     * uses addresses 0xAAA and 0x555 to unlock because the least significant
     * bit is ignored. (0x555 rather than 0x554 is traditional.)
     *
     * In general we need to multiply by the maximum device width.
     */
    return flash_addr.addr * c->bank_width;
}

/*
 * Return the command value or expected status replicated across all devices.
 */
static inline uint64_t replicate(const FlashConfig *c, uint64_t data)
{
    /* Sanity check our tests. */
    assert((data & ~device_mask(c)) == 0);
    return data;
}

static inline void flash_cmd(const FlashConfig *c, faddr cmd_addr,
                             uint8_t cmd)
{
    flash_write(c, as_byte_addr(c, cmd_addr), replicate(c, cmd));
}

static inline uint64_t flash_query(const FlashConfig *c, faddr query_addr)
{
    return flash_read(c, as_byte_addr(c, query_addr));
}

static void unlock(const FlashConfig *c)
{
    flash_cmd(c, UNLOCK0_ADDR, UNLOCK0_CMD);
    flash_cmd(c, UNLOCK1_ADDR, UNLOCK1_CMD);
}

static void reset(const FlashConfig *c)
{
    flash_cmd(c, FLASH_ADDR(0), RESET_CMD);
}

static void sector_erase(const FlashConfig *c, uint64_t byte_addr)
{
    unlock(c);
    flash_cmd(c, UNLOCK0_ADDR, SECOND_UNLOCK_CMD);
    unlock(c);
    flash_write(c, byte_addr, replicate(c, SECTOR_ERASE_CMD));
}

static void wait_for_completion(const FlashConfig *c, uint64_t byte_addr)
{
    /* If DQ6 is toggling, step the clock and ensure the toggle stops. */
    const uint64_t dq6 = replicate(c, 0x40);
    if ((flash_read(c, byte_addr) & dq6) ^ (flash_read(c, byte_addr) & dq6)) {
        /* Wait for erase or program to finish. */
        qtest_clock_step_next(c->qtest);
        /* Ensure that DQ6 has stopped toggling. */
        g_assert_cmphex(flash_read(c, byte_addr), ==, flash_read(c, byte_addr));
    }
}

static void bypass_program(const FlashConfig *c, uint64_t byte_addr,
                           uint16_t data)
{
    flash_cmd(c, UNLOCK0_ADDR, PROGRAM_CMD);
    flash_write(c, byte_addr, data);
    /*
     * Data isn't valid until DQ6 stops toggling. We don't model this as
     * writes are immediate, but if this changes in the future, we can wait
     * until the program is complete.
     */
    wait_for_completion(c, byte_addr);
}

static void program(const FlashConfig *c, uint64_t byte_addr, uint16_t data)
{
    unlock(c);
    bypass_program(c, byte_addr, data);
}

static void chip_erase(const FlashConfig *c)
{
    unlock(c);
    flash_cmd(c, UNLOCK0_ADDR, SECOND_UNLOCK_CMD);
    unlock(c);
    flash_cmd(c, UNLOCK0_ADDR, CHIP_ERASE_CMD);
}

/*
 * A system reset must return the chip to read-array (ROMD) mode. Data reads
 * answer identically through the MMIO path, so the observable difference is
 * the mapping mode itself: read it from the machine's own flat view rather
 * than inferring it from data.
 */
static void test_reset_to_read_array(const void *opaque)
{
    const FlashConfig *config = opaque;
    QTestState *qtest = qtest_initf(
        "-M r2d"
        " -drive if=pflash,file=%s,format=raw,copy-on-read=on",
        image_path);
    FlashConfig explicit_config = expand_config_defaults(config);
    explicit_config.qtest = qtest;
    const FlashConfig *c = &explicit_config;

    /* Leave ROMD mode by entering autoselect. */
    unlock(c);
    flash_cmd(c, UNLOCK0_ADDR, AUTOSELECT_CMD);
    g_assert_cmphex(flash_query(c, FLASH_ADDR(0)), ==, replicate(c, FLASH_ID0));

    qtest_system_reset(qtest);

    g_autofree char *mtree = qtest_hmp(qtest, "info mtree -f");
    const char *hit = strstr(mtree, "r2d.flash");
    const char *line_start;
    const char *line_end;
    g_autofree char *line = NULL;

    g_assert_nonnull(hit);
    for (line_start = hit; line_start > mtree && line_start[-1] != '\n';
         line_start--) {
        continue;
    }
    line_end = strchr(line_start, '\n');
    line = g_strndup(line_start, line_end ? (size_t)(line_end - line_start)
                                          : strlen(line_start));
    g_assert_nonnull(strstr(line, "romd"));

    qtest_quit(qtest);
}

/* Fill a fresh image with a pattern no erase or program could leave behind. */
static char *make_patterned_image(void)
{
    GError *err = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp("qtest-ro.XXXXXX", &path, &err);
    g_autofree uint8_t *pattern = g_malloc(UNIFORM_FLASH_SIZE);

    g_assert_no_error(err);
    for (size_t i = 0; i < UNIFORM_FLASH_SIZE; ++i) {
        pattern[i] = (uint8_t)(0x5A + i);
    }
    g_assert_cmpint(write(fd, pattern, UNIFORM_FLASH_SIZE), ==,
                    UNIFORM_FLASH_SIZE);
    close(fd);
    return path;
}

static char *make_log_path(const char *stem)
{
    g_autofree char *path = g_strdup_printf("%s/%s-XXXXXX.log",
                                            g_get_tmp_dir(), stem);
    int fd = g_mkstemp(path);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    return g_steal_pointer(&path);
}

/*
 * A read-only backing store makes every mutation a no-op. The device used to
 * swallow them without a word, which reads from the guest side exactly like a
 * chip that programmed fine, so the diagnostic is the only way to tell an
 * ignored write from a successful one. Attempt each of the three mutations and
 * require both the complaint and the untouched bytes.
 */
static void test_readonly_mutations_diagnosed(const void *opaque)
{
    const FlashConfig *config = opaque;
    g_autofree char *ro_image = make_patterned_image();
    g_autofree char *log = make_log_path("pflash-cfi02-readonly");
    g_autofree char *found = NULL;
    g_autofree uint8_t *before = g_malloc(UNIFORM_FLASH_SIZE);
    g_autofree uint8_t *after = g_malloc(UNIFORM_FLASH_SIZE);
    QTestState *qtest;
    FILE *f;

    f = fopen(ro_image, "rb");
    g_assert_nonnull(f);
    g_assert_cmpuint(fread(before, 1, UNIFORM_FLASH_SIZE, f), ==,
                     UNIFORM_FLASH_SIZE);
    fclose(f);

    qtest = qtest_initf("-M r2d"
                        " -drive if=pflash,file=%s,format=raw,readonly=on"
                        " -d guest_errors -D %s",
                        ro_image, log);
    FlashConfig explicit_config = expand_config_defaults(config);
    explicit_config.qtest = qtest;
    const FlashConfig *c = &explicit_config;

    const uint64_t word0 = flash_read(c, 0);
    const uint64_t word1 = flash_read(c, UNIFORM_FLASH_SECTOR_SIZE);

    /* The pattern, not an erased chip: an unchanged read must mean something */
    g_assert_cmphex(word0, !=, bank_mask(c));

    /* Word program. */
    program(c, 0, 0x1234);
    g_assert_cmphex(flash_read(c, 0), ==, word0);

    /* Sector erase, run to completion so reads return data again. */
    sector_erase(c, 0);
    qtest_clock_step_next(c->qtest); /* Step over the 50 us timeout. */
    wait_for_completion(c, 0);
    g_assert_cmphex(flash_read(c, 0), ==, word0);

    /* Chip erase. */
    chip_erase(c);
    qtest_clock_step_next(c->qtest);
    wait_for_completion(c, 0);
    g_assert_cmphex(flash_read(c, 0), ==, word0);
    g_assert_cmphex(flash_read(c, UNIFORM_FLASH_SECTOR_SIZE), ==, word1);

    qtest_quit(qtest);

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_nonnull(strstr(found,
                            "word program ignored: backing store is "
                            "read-only"));
    g_assert_nonnull(strstr(found,
                            "sector erase ignored: backing store is "
                            "read-only"));
    g_assert_nonnull(strstr(found,
                            "chip erase ignored: backing store is read-only"));
    g_assert_cmpint(unlink(log), ==, 0);

    /* The backing file itself is byte-for-byte what we wrote. */
    f = fopen(ro_image, "rb");
    g_assert_nonnull(f);
    g_assert_cmpuint(fread(after, 1, UNIFORM_FLASH_SIZE, f), ==,
                     UNIFORM_FLASH_SIZE);
    fclose(f);
    g_assert_cmpint(memcmp(before, after, UNIFORM_FLASH_SIZE), ==, 0);
    g_assert_cmpint(unlink(ro_image), ==, 0);
}

/*
 * The complaint belongs to the mutation, not to the command decoder. Query,
 * autoselect and reset never reach a mutation, so a read-only backend must
 * leave the log empty for them. Every message is flushed as it is written, so
 * the log can be read back while the machine is still up: program afterwards
 * and the same instance proves the backend really was read-only, rather than
 * leaving the silence to be explained by a writable drive.
 */
static void test_readonly_queries_silent(const void *opaque)
{
    const FlashConfig *config = opaque;
    g_autofree char *ro_image = make_patterned_image();
    g_autofree char *log = make_log_path("pflash-cfi02-readonly-quiet");
    g_autofree char *quiet = NULL;
    g_autofree char *noisy = NULL;
    gsize len = 0;
    QTestState *qtest;

    qtest = qtest_initf("-M r2d"
                        " -drive if=pflash,file=%s,format=raw,readonly=on"
                        " -d guest_errors -D %s",
                        ro_image, log);
    FlashConfig explicit_config = expand_config_defaults(config);
    explicit_config.qtest = qtest;
    const FlashConfig *c = &explicit_config;

    unlock(c);
    flash_cmd(c, UNLOCK0_ADDR, AUTOSELECT_CMD);
    g_assert_cmphex(flash_query(c, FLASH_ADDR(0)), ==, replicate(c, FLASH_ID0));
    flash_cmd(c, CFI_ADDR, CFI_CMD);
    g_assert_cmphex(flash_query(c, FLASH_ADDR(0x10)), ==, replicate(c, 'Q'));
    reset(c);
    reset(c);

    g_assert_true(g_file_get_contents(log, &quiet, &len, NULL));
    g_assert_cmpuint(len, ==, 0);

    program(c, 0, 0x1234);
    g_assert_true(g_file_get_contents(log, &noisy, NULL, NULL));
    g_assert_nonnull(strstr(noisy,
                            "word program ignored: backing store is "
                            "read-only"));

    qtest_quit(qtest);
    g_assert_cmpint(unlink(log), ==, 0);
    g_assert_cmpint(unlink(ro_image), ==, 0);
}

static void cleanup(void *opaque)
{
    unlink(image_path);
    g_free(image_path);
}

/* r2d wires up one x16 device with uniform 64 KiB sectors. */
static const FlashConfig configuration = {
    .bank_width = 2,
};

int main(int argc, char **argv)
{
    GError *err = NULL;
    int fd = g_file_open_tmp("qtest.XXXXXX", &image_path, &err);
    g_assert_no_error(err);

    if (ftruncate(fd, UNIFORM_FLASH_SIZE) < 0) {
        int error_code = errno;
        close(fd);
        cleanup(NULL);
        g_printerr("Failed to truncate file %s to %u MB: %s\n", image_path,
                   UNIFORM_FLASH_SIZE, strerror(error_code));
        exit(EXIT_FAILURE);
    }
    close(fd);

    qtest_add_abrt_handler(cleanup, NULL);
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sgi-pflash-cfi02/reset-to-read-array",
                        &configuration, test_reset_to_read_array);
    qtest_add_data_func("sgi-pflash-cfi02/readonly-mutations-diagnosed",
                        &configuration, test_readonly_mutations_diagnosed);
    qtest_add_data_func("sgi-pflash-cfi02/readonly-queries-silent",
                        &configuration, test_readonly_queries_silent);
    int result = g_test_run();
    cleanup(NULL);
    return result;
}
