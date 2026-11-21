/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Altix 350 reset address-space tests. Test bytes are never executed.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qemu/units.h"
#include "hw/mips/sgi/initial-flash.h"

#define FLASH_BASE 0xffa00000ULL
#define FLASH_SIZE (6 * MiB)

static char *image_path;
static char *tmp_dir;
static char *unimp_log;
static unsigned unimp_serial;
static unsigned unimp_seen;

/* Each machine logs what it refuses to a file; assert_unmodelled() reads it. */
static QTestState *altix350_start_bios(const char *bios, const char *extra)
{
    g_free(unimp_log);
    unimp_log = g_strdup_printf("%s/unimp-%u.log", tmp_dir, unimp_serial++);
    unimp_seen = 0;
    return qtest_initf("-machine altix350 -bios %s -d unimp -D %s %s", bios,
                       unimp_log, extra);
}

static QTestState *altix350_start_with(const char *extra)
{
    return altix350_start_bios(image_path, extra);
}

static QTestState *altix350_start(void)
{
    return altix350_start_with("");
}

/* SHub 1 local MMR window; register offsets are from the SHub register header. */
#define SHUB_LOCAL_MMR 0x8000000000ULL
#define SHUB_MD_DQLP 0x100030300ULL
#define SHUB_SCRATCH2 0x1101d0100ULL
#define SHUB_IPI_INT 0x110000380ULL
#define SHUB_RTC_INT_CONFIG 0x110001480ULL
#define SHUB_RTC_INT_ENABLE 0x110001500ULL
#define SHUB_EVENT_OCCURRED 0x110010000ULL
#define SHUB_SHUB_ID 0x110060580ULL
#define SHUB_MD_REG_0000_0000 0x100000000ULL
#define SHUB_LB_1002_0000 0x110020000ULL
#define SHUB_SLICE_REC 0x110030000ULL
#define SHUB_RTC_CTL 0x110060280ULL
#define SHUB_ELECT 0x1101d0080ULL
#define SHUB_ELECT_SET 0x1101d0088ULL
#define SHUB_L1_STATE 0x1101d0100ULL
#define SHUB_LOCK 0x1101d0200ULL
#define SHUB_LOCK_STATUS 0x1101d0208ULL
#define SHUB_LOCK2 0x1101d0180ULL
#define SHUB_LOCK2_STATUS 0x1101d0188ULL
#define SHUB_MD_0003_0400 0x100030400ULL
#define SHUB_MD_0005_0400 0x100050400ULL
#define SHUB_MD_0001_0068 0x100010068ULL
#define SHUB_Y_DIMM_CFG 0x100010008ULL
#define SHUB_JNR_DIMM_CFG 0x100010010ULL
#define SHUB_Y_CFG 0x100010040ULL
#define SHUB_Y_REFRESH_CONTROL 0x100010050ULL
#define SHUB_MMRBIST_ERR 0x100000080ULL
#define SHUB_MMRBIST_BASE 0x100020080ULL
#define SHUB_MMRBIST_CTL 0x100020088ULL
#define SHUB_JTAG_CONFIG 0x110060500ULL
#define SHUB_BTE0 0x1c10000ULL
#define SHUB_RTC0_INT_CONFIG 0x110001380ULL
#define SHUB_RTC0_INT_ENABLE 0x110001400ULL
#define SHUB_INT_CMPA 0x1101b0000ULL
#define SHUB_INT_NODE_ID_CONFIG 0x110000300ULL
#define SHUB_IPI_INT_ENABLE 0x110000400ULL
#define SHUB_LOCAL_INT_CONFIG 0x110000480ULL
#define SHUB_LOCAL_INT0_ENABLE 0x110000500ULL
#define SHUB_EVENT_OVERFLOW 0x110010080ULL
#define SHUB_PTC_PARMS 0x1101a0100ULL
#define SHUB_PI_2001_0100 0x120010100ULL
#define SHUB_FSB_VGA_REMAP 0x120010180ULL
#define SHUB_SLICE_PRESENT 0x120020080ULL
#define SHUB_CPU_HIST 0x150031500ULL
#define SHUB_CPU_BITS 0x150031550ULL
#define SHUB_GLOBAL_MMR 0x800000000ULL
#define SHUB_PTC 0x1101a0000ULL
#define SHUB_INT_CMP 0x1101b0080ULL
#define SHUB_RTC 0x1101c0000ULL
#define SHUB_PI_CAM_CONTROL 0x120050300ULL
#define SHUB_PIO_WRITE_STATUS 0x120070200ULL
#define SHUB_PIO_WRITE_STATUS_ALIAS 0x120070208ULL
#define SHUB_JUNK_BUS_LED 0x7fed00000ULL
#define SHUB_PIO_WRITE_STATUS_IDLE 0x3f00000000000000ULL
#define SHUB_LB_HOLE_0608 0x110050608ULL
#define SHUB_PI_ERROR_DETAIL_1 0x120060500ULL

static void assert_running(QTestState *qts)
{
    QDict *reply = qtest_qmp(qts, "{'execute': 'query-status'}");
    QDict *status = qdict_get_qdict(reply, "return");

    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "running");
    qobject_unref(reply);
}

static void test_flash(void)
{
    QTestState *qts = altix350_start();
    uint8_t data[16];

    qtest_memread(qts, FLASH_BASE, data, sizeof(data));
    for (unsigned i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, i + 1);
    }
    qtest_memread(qts, 0xfffffff0, data, sizeof(data));
    for (unsigned i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa0 + i);
    }
    qtest_quit(qts);
}

static void test_early_ram(void)
{
    QTestState *qts = altix350_start();
    const uint8_t data[] = { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0 };
    uint8_t result[sizeof(data)];

    qtest_memwrite(qts, 0x1000038, data, sizeof(data));
    qtest_memread(qts, 0x1000038, result, sizeof(result));
    g_assert_cmpmem(result, sizeof(result), data, sizeof(data));
    qtest_quit(qts);
}

static void test_cacheable_alias(void)
{
    QTestState *qts = altix350_start();
    const uint8_t data[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t result[sizeof(data)];

    /* Node memory is also at AS 3: the PROM's own-memory test uses it. */
    qtest_memwrite(qts, 0x3000000000ULL + 0x2000, data, sizeof(data));
    qtest_memread(qts, 0x2000, result, sizeof(result));
    g_assert_cmpmem(result, sizeof(result), data, sizeof(data));
    qtest_quit(qts);
}

static uint64_t shub_rd(QTestState *qts, uint64_t offset)
{
    return qtest_readq(qts, SHUB_LOCAL_MMR + offset);
}

static void shub_wr(QTestState *qts, uint64_t offset, uint64_t value)
{
    qtest_writeq(qts, SHUB_LOCAL_MMR + offset, value);
}

static unsigned log_count(const char *needle)
{
    g_autofree char *contents = NULL;
    const char *p;
    unsigned n = 0;

    if (!g_file_get_contents(unimp_log, &contents, NULL, NULL)) {
        return 0;
    }
    for (p = contents; (p = strstr(p, needle)); p++) {
        n++;
    }
    return n;
}

static unsigned unimp_count(void)
{
    return log_count("unsupported");
}

/* A refused access is logged and the VM keeps running. */
static void assert_unmodelled(QTestState *qts)
{
    for (unsigned i = 0; i < 100 && unimp_count() <= unimp_seen; i++) {
        g_usleep(100000);
    }
    g_assert_cmpuint(unimp_count(), >, unimp_seen);
    unimp_seen = unimp_count();
    assert_running(qts);
}

/* An erase through the SHub's flash window shows at the reset vector alias. */
static void test_flash_write(void)
{
    const uint64_t flash = SHUB_LOCAL_MMR + 0x7ff800000ULL;
    const uint64_t sector = 0x200000;           /* the image's first sector */
    QTestState *qts = altix350_start();

    g_assert_cmphex(qtest_readb(qts, FLASH_BASE), ==, 1);
    qtest_writew(qts, flash + 2 * 0x555, 0xaa);
    qtest_writew(qts, flash + 2 * 0x2aa, 0x55);
    qtest_writew(qts, flash + 2 * 0x555, 0x80);
    qtest_writew(qts, flash + 2 * 0x555, 0xaa);
    qtest_writew(qts, flash + 2 * 0x2aa, 0x55);
    qtest_writew(qts, flash + sector, 0x30);
    qtest_clock_step(qts, 4000000000ULL);
    qtest_writew(qts, flash, 0xf0);
    g_assert_cmphex(qtest_readq(qts, FLASH_BASE), ==, UINT64_MAX);
    qtest_quit(qts);
}

#define NOP (UINT64_C(1) << 27)

static void put_bundle(uint8_t *at, unsigned template, uint64_t a, uint64_t b,
                       uint64_t c)
{
    stq_le_p(at, template | (a << 5) | (b << 46));
    stq_le_p(at + 8, (b >> 18) | (c << 23));
}

static uint64_t adds(unsigned dest, int imm)
{
    unsigned v = (unsigned)imm & 0x3fff;

    return (UINT64_C(8) << 37) | (UINT64_C(2) << 34) |
           ((uint64_t)(v >> 13) << 36) | ((uint64_t)((v >> 7) & 0x3f) << 27) |
           ((uint64_t)(v & 0x7f) << 13) | (dest << 6);
}

static uint64_t st8(unsigned base, unsigned source)
{
    return (UINT64_C(4) << 37) | (UINT64_C(0x33) << 30) |
           ((uint64_t)base << 20) | (source << 13);
}

/*
 * The PROM probes the flash through the SHub window while it still runs from
 * the reset window, so a fetch must work with the chip out of read-array mode.
 */
static void test_fetch_with_flash_in_command_mode(void)
{
    const uint64_t flash = SHUB_LOCAL_MMR + 0x7ff800000ULL;
    g_autofree uint8_t *data = g_malloc0(FLASH_SIZE);
    g_autofree char *dir = g_path_get_dirname(image_path);
    g_autofree char *image = g_build_filename(dir, "fetch.bin", NULL);
    uint8_t *entry = data + FLASH_SIZE - 0x50;      /* the CPU's reset ip */
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    QTestState *qts;

    /* r10 = 4096, r13 = 0x42, store r13 at r10, then loop on the bundle. */
    put_bundle(entry, 1, adds(10, 4096), adds(13, 0x42), NOP);
    put_bundle(entry + 16, 1, st8(10, 13), NOP, NOP);
    put_bundle(entry + 32, 0x11, NOP, NOP, UINT64_C(4) << 37);
    g_assert_true(g_file_set_contents(image, (char *)data, FLASH_SIZE, NULL));
    qts = altix350_start_bios(image, "-accel tcg -S");
    qtest_writew(qts, flash + 2 * 0x555, 0xaa);
    qtest_writew(qts, flash + 2 * 0x2aa, 0x55);
    qtest_writew(qts, flash + 2 * 0x555, 0x90);     /* autoselect */
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    while (qtest_readq(qts, 0x1000) != 0x42 &&
           g_get_monotonic_time() < deadline) {
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readq(qts, 0x1000), ==, 0x42);
    qtest_quit(qts);
    unlink(image);
}

static void poll_ram_word(QTestState *qts, uint64_t addr, uint64_t want)
{
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

    while (qtest_readq(qts, addr) != want &&
           g_get_monotonic_time() < deadline) {
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readq(qts, addr), ==, want);
}

/* A program only clears bits: the spin bundle's immediate goes 0x7f to 0x43. */
static void test_flash_program_reaches_running_code(void)
{
    const uint64_t flash = SHUB_LOCAL_MMR + 0x7ff800000ULL;
    g_autofree uint8_t *data = g_malloc0(FLASH_SIZE);
    g_autofree char *dir = g_path_get_dirname(image_path);
    g_autofree char *image = g_build_filename(dir, "program.bin", NULL);
    uint8_t *entry = data + FLASH_SIZE - 0x50;      /* the CPU's reset ip */
    uint8_t spin[16];
    QTestState *qts;

    /* Spin on a bundle that stores r14 at 4096 and reloads it with 0x7f. */
    put_bundle(entry, 1, adds(11, 4096), NOP, NOP);
    put_bundle(entry + 16, 0x11, st8(11, 14), adds(14, 0x7f),
               UINT64_C(4) << 37);
    g_assert_true(g_file_set_contents(image, (char *)data, FLASH_SIZE, NULL));
    qts = altix350_start_bios(image, "-accel tcg -S");
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    poll_ram_word(qts, 0x1000, 0x7f);
    qtest_qmp_assert_success(qts, "{'execute': 'stop'}");

    put_bundle(spin, 0x11, st8(11, 14), adds(14, 0x43), UINT64_C(4) << 37);
    for (unsigned i = 0; i < sizeof(spin); i += 2) {
        uint16_t old = lduw_le_p(entry + 16 + i);
        uint16_t new = lduw_le_p(spin + i);

        g_assert_cmphex(old & new, ==, new);
        if (old == new) {
            continue;
        }
        qtest_writew(qts, flash + 2 * 0x555, 0xaa);
        qtest_writew(qts, flash + 2 * 0x2aa, 0x55);
        qtest_writew(qts, flash + 2 * 0x555, 0xa0);
        qtest_writew(qts, flash + 0x200000 + FLASH_SIZE - 0x40 + i, new);
    }
    g_assert_cmphex(qtest_readq(qts, FLASH_BASE + FLASH_SIZE - 0x40), ==,
                    ldq_le_p(spin));

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    poll_ram_word(qts, 0x1000, 0x43);
    qtest_quit(qts);
    unlink(image);
}

/* A -drive if=pflash file is what boots: it shows at the reset vector. */
static void test_flash_drive_boots(void)
{
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    g_autofree uint8_t *file = g_malloc(8 * MiB);
    QTestState *qts;
    int fd = g_file_open_tmp("altix350-flash-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    memset(file, 0x5a, 8 * MiB);
    g_assert_true(g_file_set_contents(path, (char *)file, 8 * MiB, NULL));
    args = g_strdup_printf("-drive if=pflash,format=raw,file=%s", path);
    qts = altix350_start_with(args);
    g_assert_cmphex(qtest_readq(qts, FLASH_BASE), ==, 0x5a5a5a5a5a5a5a5aULL);
    qtest_quit(qts);
    g_unlink(path);
}

/*
 * A flash file the initial-flash builder makes shows its variable area
 * through the SHub window and the firmware at the reset window.
 */
static void test_flash_drive_variables(void)
{
    const uint64_t flash = SHUB_LOCAL_MMR + 0x7ff800000ULL;
    static const SGIAltix350PromVariable variables[] = {
        { "NoACPIIO", "1" },
    };
    g_autofree char *firmware = NULL;
    g_autofree uint8_t *file = g_malloc(8 * MiB);
    g_autofree uint8_t *seen = g_malloc(0x200);
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    QTestState *qts;
    gsize length;
    int fd = g_file_open_tmp("altix350-vars-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_true(g_file_get_contents(image_path, &firmware, &length, NULL));
    g_assert_cmpuint(length, ==, FLASH_SIZE);
    g_assert_true(sgi_altix350_initial_flash(file, 8 * MiB,
                                             (uint8_t *)firmware, length,
                                             variables, 1, &error_abort));
    g_assert_true(g_file_set_contents(path, (char *)file, 8 * MiB, NULL));
    args = g_strdup_printf("-drive if=pflash,format=raw,file=%s", path);
    qts = altix350_start_with(args);

    qtest_memread(qts, flash, seen, 0x200);
    g_assert_cmpmem(seen, 16, "\xff\xff\xff\xff\xff\xff\xff\xff"
                    "\xff\xff\xff\xff\xff\xff\xff\xff", 16);
    g_assert_cmpmem(seen + 0x10, 12, "IOLP\x01\0\0\0\xff\xff\xff\xff", 12);
    g_assert_cmphex(seen[0x100], ==, 1);
    g_assert_cmpstr((char *)seen + 0x101, ==, "NoACPIIO");
    g_assert_cmpstr((char *)seen + 0x110, ==, "1");
    g_assert_cmphex(seen[0x140], ==, 0xff);
    g_assert_cmphex(qtest_readq(qts, flash + 0x200000), ==,
                    qtest_readq(qts, FLASH_BASE));
    g_assert_cmphex(qtest_readq(qts, FLASH_BASE), ==, ldq_le_p(firmware));
    qtest_quit(qts);
    g_unlink(path);
}

static void test_unknown_read(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* Unnamed offset in the memory controller block. */
    qtest_readq(qts, 0x8100010080ULL);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_unknown_write(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* Unnamed offset in the memory controller block. */
    qtest_writeq(qts, 0x8100010088ULL, 1);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

/*
 * Expect a startup failure with this stderr text. A machine that starts
 * gets "quit" and exits 0, so a regression fails instead of hanging.
 */
static void expect_startup_error(const char *args, const char *text)
{
    g_autofree char *cmd = g_strdup_printf("%s -machine altix350 -display none "
                                           "-S -monitor stdio %s",
                                           getenv("QTEST_QEMU_BINARY"), args);
    static const char quit[] = "quit\n";
    g_auto(GStrv) argv = NULL;
    GSpawnFlags flags = G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL;
    g_autoptr(GString) err = g_string_new(NULL);
    char buf[512];
    GPid pid;
    int in_fd, err_fd, status;
    ssize_t n;

    g_assert_true(g_shell_parse_argv(cmd, NULL, &argv, NULL));
    g_assert_true(g_spawn_async_with_pipes(NULL, argv, NULL, flags, NULL, NULL,
                                           &pid, &in_fd, NULL, &err_fd, NULL));
    /* A refused start exits first, so a short write is no failure. */
    signal(SIGPIPE, SIG_IGN);
    n = qemu_write_full(in_fd, quit, strlen(quit));
    g_assert_cmpint(n, <=, strlen(quit));
    close(in_fd);
    while ((n = read(err_fd, buf, sizeof(buf))) > 0) {
        g_string_append_len(err, buf, n);
    }
    close(err_fd);
    g_assert_cmpint(waitpid(pid, &status, 0), ==, pid);
    g_spawn_close_pid(pid);
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), !=, 0);
    g_assert_nonnull(strstr(err->str, text));
}

static void test_missing_bios(void)
{
    expect_startup_error("-bios /nonexistent/altix350.bin",
                         "cannot find -bios file");
}

static void test_wrong_cpu(void)
{
    g_autofree char *args = g_strdup_printf("-cpu merced-800 -bios %s",
                                            image_path);

    expect_startup_error(args, "Invalid CPU model: merced-800");
}

static void test_shub_pio_write_status(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmphex(qtest_readq(qts, SHUB_LOCAL_MMR +
                                    SHUB_PIO_WRITE_STATUS + i * 0x80),
                        ==, SHUB_PIO_WRITE_STATUS_IDLE);
        qtest_writeq(qts, SHUB_LOCAL_MMR + SHUB_PIO_WRITE_STATUS_ALIAS +
                     i * 0x80, 7);
    }
    assert_running(qts);
    qtest_quit(qts);
}

static void test_shub_pio_write_error_self_test(void)
{
    QTestState *qts = altix350_start();
    uint64_t status = SHUB_LOCAL_MMR + SHUB_PIO_WRITE_STATUS;
    uint64_t alias = SHUB_LOCAL_MMR + SHUB_PIO_WRITE_STATUS_ALIAS;
    uint64_t addr = SHUB_LB_HOLE_0608 & 0x3fffffffffff8ULL;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writeq(qts, SHUB_LOCAL_MMR + SHUB_LB_HOLE_0608, 0);
    /* The error and its address latch in the status and the detail words. */
    g_assert_cmphex(qtest_readq(qts, status), ==,
                    SHUB_PIO_WRITE_STATUS_IDLE | addr | 4);
    g_assert_cmphex(shub_rd(qts, 0x110050200ULL), ==, (1ULL << 63) | 0xea);
    g_assert_cmphex(shub_rd(qts, 0x110050280ULL), ==, SHUB_LB_HOLE_0608 >> 3);
    g_assert_cmphex(shub_rd(qts, SHUB_PI_ERROR_DETAIL_1), ==,
                    SHUB_LB_HOLE_0608);
    /* A second error sets the multiple bit and keeps the first address. */
    qtest_writeq(qts, SHUB_LOCAL_MMR + SHUB_LB_HOLE_0608, 0);
    g_assert_cmphex(qtest_readq(qts, status), ==,
                    SHUB_PIO_WRITE_STATUS_IDLE | addr | 5);
    g_assert_cmphex(qtest_readq(qts, status + 0x80), ==,
                    SHUB_PIO_WRITE_STATUS_IDLE);
    /* The PROM's alias write clears the errors and the address. */
    qtest_writeq(qts, alias, 0x3fffffffffffdULL);
    g_assert_cmphex(qtest_readq(qts, status), ==,
                    SHUB_PIO_WRITE_STATUS_IDLE);
    assert_running(qts);
    qtest_readq(qts, SHUB_LOCAL_MMR + SHUB_LB_HOLE_0608);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_pio_write_status_alias_other_value(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writeq(qts, SHUB_LOCAL_MMR + SHUB_PIO_WRITE_STATUS_ALIAS, 1);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_pio_write_status_instance_limit(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_readq(qts, SHUB_LOCAL_MMR + SHUB_PIO_WRITE_STATUS + 2 * 0x80);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_led(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    for (unsigned i = 0; i < 4; i++) {
        qtest_writew(qts, SHUB_LOCAL_MMR + SHUB_JUNK_BUS_LED + i * 0x10000,
                     0x0001);
    }
    assert_running(qts);
    /* The LED register is write-only in the kernel's use. */
    qtest_readw(qts, SHUB_LOCAL_MMR + SHUB_JUNK_BUS_LED);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_led_wrong_width(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writeq(qts, SHUB_LOCAL_MMR + SHUB_JUNK_BUS_LED, 1);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_privilege_vector(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writeq(qts, SHUB_LOCAL_MMR + SHUB_MD_DQLP, 0x123456789abcdef0ULL);
    g_assert_cmphex(qtest_readq(qts, SHUB_LOCAL_MMR + SHUB_MD_DQLP), ==,
                    0x123456789abcdef0ULL);
    assert_running(qts);
    qtest_quit(qts);
}

/* The L1 puts rack 1, bay 1, slab 0, partition 0 in SH_SCRATCH2 at reset. */
static void test_shub_scratch2_identity(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    g_assert_cmphex(shub_rd(qts, SHUB_SCRATCH2), ==, 0x0410006c);
    assert_running(qts);
    qtest_quit(qts);
}

static void test_shub_privilege_vector_reset_unknown(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_readq(qts, SHUB_LOCAL_MMR + SHUB_MD_DQLP);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_comparators_reset_and_storage(void)
{
    QTestState *qts = altix350_start();
    const uint64_t config = 0x0000000000000f10ULL;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readq(qts, SHUB_LOCAL_MMR + SHUB_RTC_INT_CONFIG +
                                    i * 0x100), ==, 0);
        g_assert_cmphex(qtest_readq(qts, SHUB_LOCAL_MMR + SHUB_RTC_INT_ENABLE +
                                    i * 0x100), ==, 0);
        g_assert_cmphex(qtest_readq(qts, SHUB_LOCAL_MMR + SHUB_INT_CMP +
                                    i * 0x80), ==, 0);
        qtest_writeq(qts, SHUB_LOCAL_MMR + SHUB_INT_CMP + i * 0x80,
                     0x7fffffffffffffULL);
        qtest_writeq(qts, SHUB_LOCAL_MMR + SHUB_RTC_INT_CONFIG + i * 0x100,
                     config + i);
        g_assert_cmphex(qtest_readq(qts, SHUB_LOCAL_MMR + SHUB_INT_CMP +
                                    i * 0x80), ==, 0x7fffffffffffffULL);
        g_assert_cmphex(qtest_readq(qts, SHUB_LOCAL_MMR + SHUB_RTC_INT_CONFIG +
                                    i * 0x100), ==, config + i);
    }
    assert_running(qts);
    qtest_quit(qts);
}

static void test_shub_comparator_enable_storage(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* Enabling with nothing to compare against is accepted and reads back. */
    shub_wr(qts, SHUB_RTC_INT_ENABLE, 1);
    g_assert_cmphex(shub_rd(qts, SHUB_RTC_INT_ENABLE), ==, 1);
    shub_wr(qts, SHUB_RTC_INT_ENABLE, 0);
    assert_running(qts);
    shub_wr(qts, SHUB_RTC_INT_ENABLE, 2);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_comparator_bits_outside_fields(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writeq(qts, SHUB_LOCAL_MMR + SHUB_INT_CMP, 1ULL << 55);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_cam_control(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writeq(qts, SHUB_LOCAL_MMR + SHUB_PI_CAM_CONTROL,
                 0x8000000000000010ULL);
    assert_running(qts);
    qtest_writeq(qts, SHUB_LOCAL_MMR + SHUB_PI_CAM_CONTROL, 0);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_unmodelled_is_refused(void)
{
    static const struct {
        uint64_t offset;
        bool write;
    } cases[] = {
        { SHUB_IPI_INT, true },         /* send bit clear */
        { SHUB_PTC, true },             /* purge effect unknown */
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        QTestState *qts = altix350_start();

        qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
        if (cases[i].write) {
            qtest_writeq(qts, SHUB_LOCAL_MMR + cases[i].offset, 1);
        } else {
            qtest_readq(qts, SHUB_LOCAL_MMR + cases[i].offset);
        }
        assert_unmodelled(qts);
        qtest_quit(qts);
    }
}

static void test_shub_register_width(void)
{
    static const struct {
        uint64_t offset;
        bool write;
    } cases[] = {
        { SHUB_PIO_WRITE_STATUS, false },
        { SHUB_MD_DQLP, true },
        { SHUB_INT_CMP, true },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        QTestState *qts = altix350_start();

        qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
        if (cases[i].write) {
            qtest_writel(qts, SHUB_LOCAL_MMR + cases[i].offset, 0);
        } else {
            qtest_readl(qts, SHUB_LOCAL_MMR + cases[i].offset);
        }
        assert_unmodelled(qts);
        qtest_quit(qts);
    }
}

static void test_shub_ipi_access_and_dqrp(void)
{
    QTestState *qts = altix350_start();
    const uint64_t dqrp = 0x100050300ULL;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writeq(qts, SHUB_LOCAL_MMR + dqrp, 0x55aa);
    g_assert_cmphex(qtest_readq(qts, SHUB_LOCAL_MMR + dqrp), ==, 0x55aa);
    /* DQLP was not written, so it stays unreadable. */
    assert_running(qts);
    qtest_readq(qts, SHUB_LOCAL_MMR + SHUB_MD_DQLP);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_ipi_access_reset_unknown(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writeq(qts, SHUB_LOCAL_MMR + 0x110060480ULL, 3);
    g_assert_cmphex(qtest_readq(qts, SHUB_LOCAL_MMR + 0x110060480ULL), ==, 3);
    assert_running(qts);
    qtest_system_reset(qts);
    qtest_readq(qts, SHUB_LOCAL_MMR + 0x110060480ULL);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_config_bits_outside_fields(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* Bit 20 is not a field of the IPI-message layout. */
    qtest_writeq(qts, SHUB_LOCAL_MMR + SHUB_RTC_INT_CONFIG, 1ULL << 20);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_id_fields(void)
{
    QTestState *qts = altix350_start();
    /* NASID 0, revision 3 from reset; the PROM writes size and node fields. */
    const uint64_t written = 0x30000000ULL | (1ULL << 44) | (4ULL << 48);

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    g_assert_cmphex(shub_rd(qts, SHUB_SHUB_ID), ==, 0x30000000ULL);
    shub_wr(qts, SHUB_SHUB_ID, written);
    g_assert_cmphex(shub_rd(qts, SHUB_SHUB_ID), ==, written);
    assert_running(qts);
    /* Other bits are stored as read: a changed revision is refused. */
    shub_wr(qts, SHUB_SHUB_ID, written ^ (1ULL << 28));
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_local_memory_space(void)
{
    QTestState *qts = altix350_start();
    const uint8_t data[] = { 9, 8, 7, 6, 5, 4, 3, 2 };
    uint8_t result[sizeof(data)];

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_memwrite(qts, 0xa00000, data, sizeof(data));
    /* AS 0, space id 4: the node's memory, whatever its NASID. */
    qtest_memread(qts, (1ULL << 40) + 0xa00000, result, sizeof(result));
    g_assert_cmpmem(result, sizeof(result), data, sizeof(data));
    shub_wr(qts, SHUB_SHUB_ID, 0x30000000ULL | (6ULL << 32));
    qtest_memread(qts, (1ULL << 40) + 0xa00000, result, sizeof(result));
    g_assert_cmpmem(result, sizeof(result), data, sizeof(data));
    qtest_memwrite(qts, (1ULL << 40) + 0xb00000, data, sizeof(data));
    qtest_memread(qts, 0xb00000, result, sizeof(result));
    g_assert_cmpmem(result, sizeof(result), data, sizeof(data));
    assert_running(qts);
    qtest_quit(qts);
}

static void test_local_memory_space_ends(void)
{
    QTestState *qts = altix350_start();
    uint8_t result[8];
    const uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    qtest_memwrite(qts, 0x1000038, data, sizeof(data));
    /* The memory ends with the machine's RAM; space id 5 is not memory. */
    qtest_memread(qts, (5ULL << 38) + 0x1000038, result, sizeof(result));
    for (unsigned i = 0; i < sizeof(result); i++) {
        g_assert_cmpuint(result[i], ==, 0);
    }
    qtest_quit(qts);
}

static void test_shub_global_window_follows_nasid(void)
{
    QTestState *qts = altix350_start();
    const uint64_t moved = SHUB_GLOBAL_MMR + (1ULL << 38);

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* NASID 0: the global window is at 0x8_0000_0000. */
    g_assert_cmphex(qtest_readq(qts, SHUB_GLOBAL_MMR + SHUB_SLICE_PRESENT), ==,
                    1);
    shub_wr(qts, SHUB_SHUB_ID, 0x30000000ULL | (1ULL << 32));
    g_assert_cmphex(qtest_readq(qts, moved + SHUB_SLICE_PRESENT), ==, 1);
    assert_running(qts);
    /* The old window no longer decodes. */
    g_assert_cmphex(qtest_readq(qts, SHUB_GLOBAL_MMR + SHUB_SLICE_PRESENT), ==,
                    0);
    qtest_quit(qts);
}

static void test_shub_chip_id(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* Part 0x4363 in bits 27:12, revision 3 in bits 31:28. */
    g_assert_cmphex(shub_rd(qts, SHUB_MD_REG_0000_0000), ==, 0x34363000ULL);
    g_assert_cmphex(qtest_readq(qts, SHUB_GLOBAL_MMR + SHUB_MD_REG_0000_0000), ==,
                    0x34363000ULL);
    assert_running(qts);
    shub_wr(qts, SHUB_MD_REG_0000_0000, 0);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_election_and_lock(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    g_assert_cmphex(shub_rd(qts, SHUB_ELECT), ==, 0);
    shub_wr(qts, SHUB_ELECT_SET, 3);
    g_assert_cmphex(shub_rd(qts, SHUB_ELECT), ==, 3);
    g_assert_cmphex(shub_rd(qts, SHUB_ELECT), ==, 3);
    /* The write side is write-only. */
    assert_running(qts);
    shub_rd(qts, SHUB_ELECT_SET);
    assert_unmodelled(qts);
    qtest_quit(qts);

    qts = altix350_start();
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* A read takes the lock, a second sees it held, a write of 0 frees it. */
    g_assert_cmphex(shub_rd(qts, SHUB_LOCK_STATUS), ==, 0);
    g_assert_cmphex(shub_rd(qts, SHUB_LOCK_STATUS), ==, 1);
    shub_wr(qts, SHUB_LOCK, 0);
    g_assert_cmphex(shub_rd(qts, SHUB_LOCK_STATUS), ==, 0);
    shub_wr(qts, SHUB_LOCK, 0);
    assert_running(qts);
    shub_wr(qts, SHUB_LOCK, 1);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

/* The first write to the alias wins; a later one is ignored (approximation). */
static void test_shub_election_has_one_winner(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, SHUB_ELECT_SET, 1);
    g_assert_cmphex(shub_rd(qts, SHUB_ELECT), ==, 1);
    shub_wr(qts, SHUB_ELECT_SET, 3);
    g_assert_cmphex(shub_rd(qts, SHUB_ELECT), ==, 1);
    shub_wr(qts, SHUB_ELECT, 0);
    shub_wr(qts, SHUB_ELECT_SET, 3);
    g_assert_cmphex(shub_rd(qts, SHUB_ELECT), ==, 3);
    qtest_quit(qts);
}

static void test_shub_second_lock(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    g_assert_cmphex(shub_rd(qts, SHUB_LOCK2_STATUS), ==, 0);
    /* The global window reaches the same lock, which is now held. */
    g_assert_cmphex(qtest_readq(qts, SHUB_GLOBAL_MMR + SHUB_LOCK2_STATUS), ==,
                    1);
    /* The first lock is separate. */
    g_assert_cmphex(shub_rd(qts, SHUB_LOCK_STATUS), ==, 0);
    shub_wr(qts, SHUB_LOCK2, 0);
    g_assert_cmphex(shub_rd(qts, SHUB_LOCK2_STATUS), ==, 0);
    assert_running(qts);
    shub_wr(qts, SHUB_LOCK2, 1);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_memory_controller_words(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* Two write-only words, written 0x1000 by the PROM. */
    shub_wr(qts, SHUB_MD_0003_0400, 0x1000);
    shub_wr(qts, SHUB_MD_0005_0400, 0x1000);
    assert_running(qts);
    /* The credit word the PROM compares against a bank size. */
    shub_wr(qts, SHUB_MD_0001_0068, 0x1234);
    g_assert_cmphex(shub_rd(qts, SHUB_MD_0001_0068), ==, 0x1234);
    assert_running(qts);
    shub_rd(qts, SHUB_MD_0003_0400);
    assert_unmodelled(qts);
    qtest_quit(qts);

    qts = altix350_start();
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* The credit word reads 0 at reset, as in the PROM's traced run. */
    g_assert_cmphex(shub_rd(qts, SHUB_MD_0001_0068), ==, 0);
    assert_running(qts);
    qtest_quit(qts);
}

static void test_shub_memory_bring_up_words(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* The PROM reads these before writing and merges the old value in. */
    g_assert_cmphex(shub_rd(qts, SHUB_Y_DIMM_CFG), ==, 0);
    g_assert_cmphex(shub_rd(qts, SHUB_Y_CFG), ==, 0);
    g_assert_cmphex(shub_rd(qts, SHUB_Y_REFRESH_CONTROL), ==, 0);
    g_assert_cmphex(shub_rd(qts, SHUB_JTAG_CONFIG), ==, 0);
    /* Bank configuration for one 2 GiB bank: it must read back as written. */
    shub_wr(qts, SHUB_Y_DIMM_CFG, 0x867472703ULL);
    shub_wr(qts, SHUB_JNR_DIMM_CFG, 0x867472703ULL);
    g_assert_cmphex(shub_rd(qts, SHUB_Y_DIMM_CFG), ==, 0x867472703ULL);
    g_assert_cmphex(shub_rd(qts, SHUB_JNR_DIMM_CFG), ==, 0x867472703ULL);
    /* Written-only words, with the global window as the PROM uses it. */
    qtest_writeq(qts, SHUB_GLOBAL_MMR + 0x100030200ULL + 0x70, ~0ULL);
    qtest_writeq(qts, SHUB_GLOBAL_MMR + 0x100038810ULL + 3 * 0x10000, 1);
    assert_running(qts);
    shub_rd(qts, 0x100010020ULL);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_memory_test_engine(void)
{
    QTestState *qts = altix350_start();
    /* Start: command word, status 0, then control with go and op 48. */
    const uint64_t start = (1ULL << 40) | (48ULL << 32) | 0x10000000;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, SHUB_MMRBIST_BASE, 0);
    shub_wr(qts, SHUB_MMRBIST_ERR, 0);
    shub_wr(qts, SHUB_MMRBIST_CTL, start);
    /* Done and passed at once: go and fail clear, the rest kept. */
    g_assert_cmphex(shub_rd(qts, SHUB_MMRBIST_CTL), ==,
                    (48ULL << 32) | 0x10000000);
    g_assert_cmphex(shub_rd(qts, SHUB_MMRBIST_ERR), ==, 0);
    for (unsigned op = 0; op < 3; op++) {
        shub_wr(qts, SHUB_MMRBIST_CTL, (1ULL << 40) | ((uint64_t)op << 32));
    }
    shub_wr(qts, SHUB_MMRBIST_CTL, (1ULL << 40) | (7ULL << 32));
    shub_wr(qts, SHUB_MMRBIST_CTL, (1ULL << 40) | (16ULL << 32));
    shub_wr(qts, SHUB_MMRBIST_CTL, (1ULL << 40) | (64ULL << 32));
    /* The PROM clears go with a plain write when it finds it still set. */
    shub_wr(qts, SHUB_MMRBIST_CTL, 0);
    assert_running(qts);
    /* The directory read completes; its result registers read 0. */
    shub_wr(qts, SHUB_MMRBIST_CTL, (1ULL << 40) | (80ULL << 32));
    g_assert_cmphex(shub_rd(qts, SHUB_MMRBIST_CTL), ==, 80ULL << 32);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(shub_rd(qts, 0x100032000ULL + (i & 1) * 0x800 +
                                     (i >> 1) * 0x20000), ==, 0);
    }
    assert_running(qts);
    /* An operation outside the PROM's list has no contract. */
    shub_wr(qts, SHUB_MMRBIST_CTL, (1ULL << 40) | (96ULL << 32));
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_ii_init_accesses(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* The PROM reads the idle control and length, then clears the engine. */
    g_assert_cmphex(shub_rd(qts, SHUB_BTE0 + 0x18), ==, 0);
    shub_wr(qts, SHUB_BTE0, 0);
    g_assert_cmphex(shub_rd(qts, SHUB_BTE0), ==, 0);
    /* The error interrupt mask is a plain cell the PROM reads and ORs. */
    shub_wr(qts, 0x1c00260, shub_rd(qts, 0x1c00260) | 0x8000);
    g_assert_cmphex(shub_rd(qts, 0x1c00260), ==, 0x8000);
    assert_running(qts);
    qtest_quit(qts);
}

static void test_shub_ni_ports_down(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* A port is up only when the low nibble reads 9; no link is modelled. */
    for (int p = 0; p < 2; p++) {
        g_assert_cmphex(shub_rd(qts, 0x150000000ULL + p * 0x2000) & 0xf, !=, 9);
        g_assert_cmphex(shub_rd(qts, 0x150000048ULL + p * 0x2000) & (1 << 27),
                        ==, 0);
    }
    /* The link set-up reads the configuration, rewrites it and resets. */
    for (int p = 0; p < 2; p++) {
        uint64_t cfg = 0x150000020ULL + p * 0x2000;

        g_assert_cmphex(shub_rd(qts, cfg), ==, 0x7fc6ffd00);
        shub_wr(qts, cfg, (shub_rd(qts, cfg) & ~0x3ffULL) | 0x190);
        g_assert_cmphex(shub_rd(qts, cfg), ==, 0x7fc6ffd90);
        shub_wr(qts, 0x150000008ULL + p * 0x2000, 1);
        g_assert_cmphex(shub_rd(qts, 0x150000000ULL + p * 0x2000) & 0xf, !=, 9);
        shub_wr(qts, 0x1500303f0ULL + p * 0x200,
                shub_rd(qts, 0x1500303f0ULL + p * 0x200) &
                0xfff0fff0fff0fff0ULL);
    }
    assert_running(qts);
    qtest_quit(qts);
}

static void test_shub_interrupt_pairs(void)
{
    QTestState *qts = altix350_start();
    /* CONFIG, ENABLE offsets in the PROM's walk order after the II pairs. */
    static const uint64_t pairs[][2] = {
        { 0x110000080, 0x110000200 }, { 0x110000180, 0x110000280 },
        { 0x110000480, 0x110000500 }, { 0x110000980, 0x110000a00 },
        { 0x110000c80, 0x110001080 }, { 0x110000e00, 0x110001200 },
        { 0x110000a80, 0x110000e80 }, { 0x110000c00, 0x110001000 },
        { 0x110001280, 0x110001300 }, { 0x110001380, 0x110001400 },
        { 0x110001680, 0x110001700 },
    };

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* The walk reads ENABLE and skips the pair when it is 0. */
    for (int i = 0; i < ARRAY_SIZE(pairs); i++) {
        g_assert_cmphex(shub_rd(qts, pairs[i][1]), ==, 0);
        g_assert_cmphex(shub_rd(qts, pairs[i][0]), ==, 0);
    }
    assert_running(qts);
    qtest_quit(qts);
}

static void test_shub_routing_tables(void)
{
    QTestState *qts = altix350_start();
    static const uint64_t cells[] = {
        0x150022000, 0x1500223f8, 0x150023000, 0x150022400, 0x150022480,
        0x150024000, 0x150020000, 0x150021400, 0x150021480, 0x1500305a0,
    };

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* Direction bits above the header's mask are kept. */
    for (int i = 0; i < ARRAY_SIZE(cells); i++) {
        /* The override words reset with the valid bit set, the rest to 0. */
        g_assert_cmphex(shub_rd(qts, cells[i]) >> 63, ==,
                        (cells[i] & 0xfff) == 0x480);
        shub_wr(qts, cells[i], 0x8000000000003f3fULL);
        g_assert_cmphex(shub_rd(qts, cells[i]), ==, 0x8000000000003f3fULL);
    }
    /* The flow word is read, XORed and written back. */
    shub_wr(qts, 0x1500303a0, shub_rd(qts, 0x1500303a0) ^ 0x1000000);
    g_assert_cmphex(shub_rd(qts, 0x1500303a0), ==, 0x1000000);
    assert_running(qts);
    qtest_quit(qts);
}

/*
 * The kernel's sequence: busy marker in memory, addresses, length, control.
 * With the notify bit the engine writes the completion word; without, not.
 */
static void test_shub_bte_notifies(void)
{
    static const uint64_t mode[] = { 0x11, 0x10, 0x01 };
    const uint64_t busy = ~(1ULL << 21);
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    for (unsigned i = 0; i < G_N_ELEMENTS(mode); i++) {
        qtest_writeq(qts, 0x4000, busy);
        shub_wr(qts, SHUB_BTE0 + 0x08, 0);
        shub_wr(qts, SHUB_BTE0 + 0x10, 0x3000);
        shub_wr(qts, SHUB_BTE0 + 0x20, 0x4000);
        shub_wr(qts, SHUB_BTE0, 1);
        shub_wr(qts, SHUB_BTE0 + 0x18, mode[i]);
        g_assert_cmphex(qtest_readq(qts, 0x4000), ==,
                        (mode[i] & 0x10) ? 0 : busy);
        g_assert_cmphex(shub_rd(qts, SHUB_BTE0) & (1 << 20), ==, 0);
    }
    assert_running(qts);
    qtest_quit(qts);
}

/* A transfer started before its addresses are written is refused. */
static void test_shub_bte_needs_its_addresses(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, SHUB_BTE0, 1);
    shub_wr(qts, SHUB_BTE0 + 0x18, 0x10);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

/* The compatibility hole runs from 4 GiB to the SHub's global window. */
static void test_compat_hole_edges(void)
{
    static const uint64_t inside[] = { 0x100000000ULL, 0x7fffffff8ULL };
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    for (unsigned i = 0; i < G_N_ELEMENTS(inside); i++) {
        qtest_readq(qts, inside[i]);
        g_assert_cmpuint(log_count("compatibility-space hole"), ==, i + 1);
    }
    qtest_readq(qts, 0xff8ULL);                    /* RAM: no note */
    g_assert_cmpuint(log_count("compatibility-space hole"), ==,
                     G_N_ELEMENTS(inside));
    qtest_quit(qts);
}

static void test_shub_bte_zero_fill(void)
{
    QTestState *qts = altix350_start();
    uint8_t dirty[1088], got[1088];

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* The target is dirty: a fill must clear its 1024 bytes and no more. */
    memset(dirty, 0xa5, sizeof(dirty));
    qtest_memwrite(qts, 0x3000, dirty, sizeof(dirty));
    /* The credit fill: length with busy, addresses, then control 0x11. */
    shub_wr(qts, SHUB_BTE0, 0x100008);
    g_assert_cmphex(shub_rd(qts, SHUB_BTE0), ==, 0x100008);
    shub_wr(qts, SHUB_BTE0 + 0x08, 0x3000);
    shub_wr(qts, SHUB_BTE0 + 0x10, 0x3000);
    shub_wr(qts, SHUB_BTE0 + 0x20, 0x3000);
    shub_wr(qts, SHUB_BTE0 + 0x18, 0x11);
    g_assert_cmphex(shub_rd(qts, SHUB_BTE0) & (1 << 20), ==, 0);
    qtest_memread(qts, 0x3000, got, sizeof(got));
    memset(dirty, 0, 1024);
    g_assert(!memcmp(got, dirty, sizeof(got)));
    assert_running(qts);
    /* A mode that is neither a copy nor a zero fill is refused. */
    shub_wr(qts, SHUB_BTE0 + 0x18, 0x100);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_xn_port_words(void)
{
    static const uint64_t words[] = { 0x1500303a0, 0x150030400 };
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
        for (unsigned p = 0; p < 2; p++) {
            g_assert_cmphex(shub_rd(qts, words[i] + p * 0x200), ==, 0);
        }
        /* The PROM's read-modify-write: XOR bit 24, or set the age. */
        shub_wr(qts, words[i], 0x1000000 + i);
        g_assert_cmphex(shub_rd(qts, words[i]), ==, 0x1000000 + i);
        g_assert_cmphex(shub_rd(qts, words[i] + 0x200), ==, 0);
    }
    assert_running(qts);
    shub_rd(qts, 0x1500303a0 + 2 * 0x200);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_pic_config_is_byte_addressed(void)
{
    QTestState *qts = altix350_start();
    /* Widget 15, head 0 configuration window of the IOC4 at device 1. */
    uint64_t cfg = 0x800000000ULL + (15ULL << 24) + 0x21000;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* The ID dword is at +0, with no lane swap, whatever the width. */
    g_assert_cmphex(qtest_readl(qts, cfg), ==, 0x100a10a9);
    g_assert_cmphex(qtest_readw(qts, cfg), ==, 0x10a9);
    g_assert_cmphex(qtest_readw(qts, cfg + 2), ==, 0x100a);
    g_assert_cmphex(qtest_readb(qts, cfg + 1), ==, 0x10);
    /* A byte store lands at its own offset: the cache line size at 0xc. */
    qtest_writeb(qts, cfg + 0xc, 0x20);
    g_assert_cmphex(qtest_readl(qts, cfg + 0xc) & 0xff, ==, 0x20);
    /* The widget's 64-bit registers keep the mirrored lane: id at +0. */
    g_assert_cmphex((qtest_readl(qts, 0x800000000ULL + (15ULL << 24)) >> 12) &
                    0xffff, ==, 0xd102);
    assert_running(qts);
    qtest_quit(qts);
}

static unsigned bcd(uint8_t value)
{
    g_assert_cmpuint(value & 0xf, <=, 9);
    g_assert_cmpuint(value >> 4, <=, 9);
    return (value >> 4) * 10 + (value & 0xf);
}

/* The IOC4's byte bus holds the time-of-day chip the PROM's GetTime reads. */
static void test_ioc4_time_of_day(void)
{
    QTestState *qts =
        altix350_start_with("-rtc base=2026-11-17T12:34:10");
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    const uint64_t cfg = w15 + 0x21000;
    /* Device 1's window, BAR 0 at 1 MiB, the chip 0x80000 above it. */
    const uint64_t chip = w15 + 0x200000 + 0x100000 + 0x80000;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writel(qts, cfg + 0x10, 0x100000);
    qtest_writel(qts, cfg + 4, 2);
    /* Registers are lane swapped within each 32-bit word. */
#define CHIP(reg) (chip + ((reg) ^ 3))
    /* The PROM's probe: logical register 6 reads back 0xff, so layout 0. */
    qtest_writeb(qts, CHIP(0xb), 0);
    qtest_writeb(qts, CHIP(6), 0xff);
    qtest_writeb(qts, CHIP(0xb), 0x80);
    g_assert_cmphex(qtest_readb(qts, CHIP(6)), ==, 0xff);
    /* Year is stored as years since 1970, which the PROM adds back. */
    g_assert_cmpuint(bcd(qtest_readb(qts, CHIP(0x7fff))), ==, 56);
    g_assert_cmpuint(bcd(qtest_readb(qts, CHIP(0x7ffe))), ==, 11);
    g_assert_cmpuint(bcd(qtest_readb(qts, CHIP(0x7ffd))), ==, 17);
    g_assert_cmpuint(bcd(qtest_readb(qts, CHIP(0x7ffb))), ==, 12);
    g_assert_cmpuint(bcd(qtest_readb(qts, CHIP(0x7ffa))), ==, 34);
    g_assert_cmpuint(bcd(qtest_readb(qts, CHIP(0x7ff9))), >=, 10);
    /* The rest of the chip's 32 KiB is RAM. */
    qtest_writeb(qts, CHIP(0x1234), 0x5a);
    g_assert_cmphex(qtest_readb(qts, CHIP(0x1234)), ==, 0x5a);
#undef CHIP
    assert_running(qts);
    qtest_quit(qts);
}

/* The IOC4's INTA is wired to PIC input 0: a serial interrupt sets it. */
static void test_ioc4_interrupt_reaches_pic(void)
{
    QTestState *qts = altix350_start();
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    const uint64_t cfg = w15 + 0x21000;
    const uint64_t bar = w15 + 0x200000 + 0x100000;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writel(qts, cfg + 0x10, 0x100000);
    qtest_writel(qts, cfg + 4, 2);
    g_assert_cmphex(qtest_readq(qts, w15 + 0x100) & 1, ==, 0);
    /* Enable every serial source, then the first UART's transmit interrupt. */
    qtest_writel(qts, bar + 0x10, 0xffffffff);
    qtest_writeb(qts, bar + 0x380 + (1 ^ 3), 0x02);
    g_assert_cmphex(qtest_readq(qts, w15 + 0x100) & 1, ==, 1);
    assert_running(qts);
    qtest_quit(qts);
}

/* The ISP12160's INTA is wired to PIC input 2: a mailbox command sets it. */
static void test_isp12160_interrupt_reaches_pic(void)
{
    QTestState *qts = altix350_start();
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    const uint64_t big = 0x800000000ULL + (6ULL << 29);

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, 0x1c00188ULL, 0xf02);
    /* Device 3, the machine's ISP: its register window on big window 6. */
    qtest_writel(qts, w15 + 0x23000 + 0x14, 0x800000);
    qtest_writel(qts, w15 + 0x23000 + 4, 2);
    g_assert_cmphex(qtest_readq(qts, w15 + 0x100) & 4, ==, 0);
    /* Deliver interrupts, then run a mailbox register test. */
    qtest_writew(qts, big + 0x800008, 0x0006);
    qtest_writew(qts, big + 0x800072, 0x1357);
    qtest_writew(qts, big + 0x800070, 0x0006);
    qtest_writew(qts, big + 0x8000c0, 0x5000);
    g_assert_cmphex(qtest_readq(qts, w15 + 0x100) & 4, ==, 4);
    assert_running(qts);
    qtest_quit(qts);
}

/* The NIC's INTA is wired to PIC input 3: a posted status block sets it. */
static void test_nic_interrupt_reaches_pic(void)
{
    QTestState *qts = altix350_start_with("-nic user,model=bcm570x");
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    const uint64_t cfg = w15 + 0x24000;
    const uint64_t big = 0x800000000ULL + (6ULL << 29);
    const uint64_t regs = big + 0xa00000;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, 0x1c00188ULL, 0xf02);
    shub_wr(qts, 0x1c00118ULL, 0xff01);
    qtest_writeq(qts, w15 + 0x20, 15);
    /* PCI 0x40001000 reaches node RAM at 0x300000 through table entry 1. */
    qtest_writeq(qts, w15 + 0x10000 + 8, 0x300000 | 1);
    qtest_writel(qts, cfg + 0x10, 0xa00000);
    qtest_writel(qts, cfg + 4, 6);              /* memory space, bus master */
    qtest_writel(qts, cfg + 0x68, 0);           /* unmask the line */
    g_assert_cmphex(qtest_readq(qts, w15 + 0x100) & 8, ==, 0);
    /* A four-entry send ring, so that producer 0 is in range. */
    qtest_writel(qts, cfg + 0x7c, 0x100 + 8);
    qtest_writel(qts, cfg + 0x84, 4u << 16);
    qtest_writel(qts, regs + 0x3c38, 0);
    qtest_writel(qts, regs + 0x3c3c, 0x40001000);
    qtest_writel(qts, regs + 0x3c00, 2);        /* host coalescing enable */
    qtest_writel(qts, regs + 0x468, 2);         /* receive enable */
    qtest_writel(qts, regs + 0x204, 0);         /* unmask the mailbox */
    qtest_writel(qts, regs + 0x304, 0);         /* transmit producer: a post */
    g_assert_cmphex(qtest_readq(qts, w15 + 0x100) & 8, ==, 8);
    assert_running(qts);
    qtest_quit(qts);
}

/* A card in slot 2 has its INTA on PIC input 1 (device - 1). */
static void test_slot2_interrupt_reaches_pic(void)
{
    QTestState *qts = altix350_start_with(
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=2,id=dma0");
    const uint64_t status = 0x800000000ULL + (15ULL << 24) + 0x100;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    g_assert_cmphex(qtest_readq(qts, status) & 0xff, ==, 0);
    qtest_set_irq_in(qts, "dma0", "test-irq", 0, 1);
    g_assert_cmphex(qtest_readq(qts, status) & 0xff, ==, 2);
    qtest_set_irq_in(qts, "dma0", "test-irq", 0, 0);
    assert_running(qts);
    qtest_quit(qts);
}

/* The kernel's PIC attach sets control bit 21 (16 KiB translation pages). */
static void test_pic_control_page_size_bit(void)
{
    QTestState *qts = altix350_start();
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writeq(qts, w15 + 0x20, 0x0f20400f);
    g_assert_cmphex(qtest_readq(qts, w15 + 0x20), ==, 0x0f20400f);
    assert_running(qts);
    qtest_quit(qts);
}

/* The IO9's gigabit NIC is a BCM5701 at device 4, with the NIC's own MAC. */
static void test_bcm5701_present(void)
{
    QTestState *qts = altix350_start_with("-nic user,model=bcm570x");
    const uint64_t cfg = 0x800000000ULL + (15ULL << 24) + 0x20000 + 4 * 0x1000;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* Vendor Broadcom 14e4, device 1645. */
    g_assert_cmphex(qtest_readl(qts, cfg), ==, 0x164514e4);
    /*
     * The status word lists capabilities: PCI-X (the PIC is a PCI-X bus) and
     * power management, which the Linux tg3 driver requires.
     */
    g_assert_cmphex(qtest_readw(qts, cfg + 6) & 0x10, ==, 0x10);
    {
        uint8_t ptr = qtest_readb(qts, cfg + 0x34);
        bool pcix = false, pm = false;

        for (int n = 0; ptr && n < 8; n++) {
            uint8_t id = qtest_readb(qts, cfg + ptr);

            pcix |= id == 0x07;
            pm |= id == 0x01;
            ptr = qtest_readb(qts, cfg + ptr + 1);
        }
        g_assert_true(pcix);
        g_assert_true(pm);
    }
    assert_running(qts);
    qtest_quit(qts);
}

/* Without a "-nic" the IO9's NIC slot is empty. */
static void test_bcm5701_absent_without_nic(void)
{
    QTestState *qts = altix350_start();
    const uint64_t cfg = 0x800000000ULL + (15ULL << 24) + 0x20000 + 4 * 0x1000;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    g_assert_cmphex(qtest_readl(qts, cfg), ==, 0xffffffff);
    assert_running(qts);
    qtest_quit(qts);
}

/* The NIC's station address as "info network" prints it. */
static void assert_nic_mac(const char *machine_opts, const char *nic_opts,
                           const char *mac)
{
    g_autofree char *extra = g_strdup_printf("%s -nic user,model=bcm570x%s",
                                             machine_opts, nic_opts);
    g_autofree char *want = g_strdup_printf("macaddr=%s", mac);
    QTestState *qts = altix350_start_with(extra);
    g_autofree char *info = qtest_hmp(qts, "info network");

    g_assert_nonnull(strstr(info, want));
    qtest_quit(qts);
}

/* emu-id picks the address by the project's rule; mac= overrides it. */
static void test_nic_address_identity(void)
{
    assert_nic_mac("", "", "08:00:69:e1:00:00");
    assert_nic_mac("-machine altix350,emu-id=42", "", "08:00:69:e1:02:a0");
    assert_nic_mac("-machine altix350,emu-id=42,mac=08:00:69:aa:bb:cc", "",
                   "08:00:69:aa:bb:cc");
    assert_nic_mac("-machine altix350,emu-id=42", ",mac=08:00:69:e1:02:a0",
                   "08:00:69:e1:02:a0");
}

/* The machine models no serial or hostid, and emu-id stops at 4095. */
static void test_nic_identity_refusals(void)
{
    expect_startup_error("-machine altix350,serial=X", "serial");
    expect_startup_error("-machine altix350,hostid=69000000", "hostid");
    expect_startup_error("-machine altix350,emu-id=4096",
                         "emu-id must be 0 to 4095");
}

/* A -nic mac= other than the machine's address is refused, as on the IO9. */
static void test_nic_mac_conflict(void)
{
    g_autofree char *args = g_strdup_printf(
        "-machine altix350,emu-id=42 -bios %s -nic "
        "user,model=bcm570x,mac=52:54:00:12:34:56", image_path);

    expect_startup_error(args, "conflicts with the machine's address");
}

/* A disk on the IOC4's ATA channel: IDENTIFY by programmed I/O. */
static void test_ioc4_ata_identify(void)
{
    QTestState *qts = altix350_start_with(
        "-drive if=ide,index=0,driver=null-co,size=1048576");
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    const uint64_t cfg = w15 + 0x21000;
    const uint64_t bar = w15 + 0x200000 + 0x100000;
    /* Software addresses a word's base; the PIC moves the lane. */
    const uint64_t status = bar + 0x11c, command = status;
    const uint64_t device = bar + 0x118, data = bar + 0x100;
    uint16_t words[256];

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writel(qts, cfg + 0x10, 0x100000);
    qtest_writel(qts, cfg + 4, 2);
    qtest_writeb(qts, device, 0xa0);
    g_assert_cmphex(qtest_readb(qts, status) & 0xc0, ==, 0x40);   /* ready */
    qtest_writeb(qts, command, 0xec);                         /* IDENTIFY */
    g_assert_cmphex(qtest_readb(qts, status) & 0x08, ==, 0x08);    /* DRQ */
    for (unsigned i = 0; i < 256; i++) {
        words[i] = qtest_readw(qts, data);
    }
    /* Word 49: LBA supported, in the drive's own word order. */
    g_assert_cmphex(words[49] & 0x0200, ==, 0x0200);
    /* Word 60 and 61: 2048 sectors of 512 bytes. */
    g_assert_cmpuint(words[60] | words[61] << 16, ==, 2048);
    assert_running(qts);
    qtest_quit(qts);
}

/* A CD on the channel's second position answers IDENTIFY PACKET DEVICE. */
static void test_ioc4_atapi_identify(void)
{
    QTestState *qts = altix350_start_with(
        "-drive if=ide,index=1,media=cdrom,driver=null-co,size=1048576");
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    const uint64_t cfg = w15 + 0x21000;
    const uint64_t bar = w15 + 0x200000 + 0x100000;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writel(qts, cfg + 0x10, 0x100000);
    qtest_writel(qts, cfg + 4, 2);
    qtest_writeb(qts, bar + 0x118, 0xb0);                  /* the slave */
    qtest_writeb(qts, bar + 0x11c, 0xa1);                  /* IDENTIFY PACKET */
    g_assert_cmphex(qtest_readb(qts, bar + 0x11c) & 0x08, ==, 0x08);
    /* Word 0: an ATAPI device, a CD-ROM (type 5), in the drive's order. */
    g_assert_cmphex(qtest_readw(qts, bar + 0x100) & 0xdf00, ==, 0x8500);
    assert_running(qts);
    qtest_quit(qts);
}

/* The drive's interrupt shows in the other status word at 0xc. */
static void test_ioc4_ata_interrupt(void)
{
    QTestState *qts = altix350_start_with(
        "-drive if=ide,index=0,driver=null-co,size=1048576");
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    const uint64_t cfg = w15 + 0x21000;
    const uint64_t bar = w15 + 0x200000 + 0x100000;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writel(qts, cfg + 0x10, 0x100000);
    qtest_writel(qts, cfg + 4, 2);
    qtest_writeb(qts, bar + 0x118, 0xa0);
    g_assert_cmphex(qtest_readl(qts, bar + 0xc) & 1, ==, 0);
    qtest_writeb(qts, bar + 0x11c, 0xec);
    g_assert_cmphex(qtest_readl(qts, bar + 0xc) & 1, ==, 1);
    /* Reading the status register acknowledges the drive's request. */
    qtest_readb(qts, bar + 0x11c);
    g_assert_cmphex(qtest_readl(qts, bar + 0xc) & 1, ==, 0);
    qtest_quit(qts);
}

/* With no drive attached the channel reads as a floating bus. */
static void test_ioc4_ata_empty_channel(void)
{
    QTestState *qts = altix350_start();
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    const uint64_t cfg = w15 + 0x21000;
    const uint64_t bar = w15 + 0x200000 + 0x100000;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writel(qts, cfg + 0x10, 0x100000);
    qtest_writel(qts, cfg + 4, 2);
    qtest_writeb(qts, bar + 0x118, 0xa0);
    g_assert_cmphex(qtest_readb(qts, bar + 0x11c), ==, 0x7f);
    g_assert_cmphex(qtest_readb(qts, bar + 0x120), ==, 0x7f);
    g_assert_cmphex(qtest_readw(qts, bar + 0x100), ==, 0xff7f);
    /* A pattern written to sector count does not read back. */
    qtest_writeb(qts, bar + 0x108, 0x55);
    g_assert_cmphex(qtest_readb(qts, bar + 0x108), ==, 0x7f);
    assert_running(qts);
    qtest_quit(qts);
}

/* The kernel stores the strobe width in SIO_CR and polls bit 7 until set. */
static void test_ioc4_sio_cr(void)
{
    QTestState *qts = altix350_start();
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    const uint64_t cfg = w15 + 0x21000;
    const uint64_t sio_cr = w15 + 0x200000 + 0x100000 + 0x20;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writel(qts, cfg + 0x10, 0x100000);
    qtest_writel(qts, cfg + 4, 2);
    /* No serial or ATA request is active: bits 7 and 8 read set. */
    g_assert_cmphex(qtest_readl(qts, sio_cr), ==, 0x180);
    qtest_writel(qts, sio_cr, 0xf);
    g_assert_cmphex(qtest_readl(qts, sio_cr), ==, 0x18f);
    /* Only the four strobe bits are writable. */
    qtest_writel(qts, sio_cr, 0xffff);
    g_assert_cmphex(qtest_readl(qts, sio_cr), ==, 0x18f);
    assert_running(qts);
    qtest_quit(qts);
}

/* The kernel times the PCI clock with the IOC4's INT_OUT square wave. */
static void test_ioc4_int_out_square_wave(void)
{
    QTestState *qts = altix350_start();
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    const uint64_t cfg = w15 + 0x21000;
    const uint64_t int_out = w15 + 0x200000 + 0x100000 + 0x28;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writel(qts, cfg + 0x10, 0x100000);
    qtest_writel(qts, cfg + 4, 2);
    g_assert_cmphex(qtest_readl(qts, int_out), ==, 0);
    /* Toggle mode, count 63: 64 ticks of 520 clocks, about 1 ms at 33 MHz. */
    qtest_writel(qts, int_out, 0x70000 | 63);
    g_assert_cmphex(qtest_readl(qts, int_out) >> 31, ==, 0);
    qtest_clock_step(qts, 1500000);
    g_assert_cmphex(qtest_readl(qts, int_out) >> 31, ==, 1);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(qtest_readl(qts, int_out) >> 31, ==, 0);
    /* The control bits read back; writing 0 returns to the power-on state. */
    g_assert_cmphex(qtest_readl(qts, int_out) & 0x7ffffff, ==, 0x7003f);
    qtest_writel(qts, int_out, 0);
    qtest_clock_step(qts, 5000000);
    g_assert_cmphex(qtest_readl(qts, int_out), ==, 0);
    assert_running(qts);
    qtest_quit(qts);
}

/*
 * A PIC pin aimed at a RAM word, the way the kernel's DMA flush uses one:
 * forcing it writes the packet word, 0x10f for level 0xf, in the node's
 * byte order.
 */
static void test_pic_interrupt_to_memory(void)
{
    QTestState *qts = altix350_start();
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* Widget 15 may write node memory. */
    shub_wr(qts, 0x1800000ULL + 0x400118, 1ULL << 15);
    /* The flush pin is never enabled, as in the kernel's flush. */
    qtest_writeq(qts, w15 + 0x130, (0xfULL << 48) | 0x3102108ULL);
    qtest_writeq(qts, 0x3102108ULL, 0);
    qtest_writeq(qts, w15 + 0x180, 1);
    /* The packet is in flight for a microsecond, not delivered at once. */
    g_assert_cmphex(qtest_readl(qts, 0x3102108ULL), ==, 0);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readl(qts, 0x3102108ULL), ==, 0x10f);
    assert_running(qts);
    qtest_quit(qts);
}

static void test_pic_empty_slot_reads_all_ones(void)
{
    QTestState *qts = altix350_start();
    const uint64_t cfg = 0x800000000ULL + (15ULL << 24) + 0x20000;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* Slot 1 of head 0 is empty: the read completes with all ones. */
    g_assert_cmphex(qtest_readl(qts, cfg + 0x2000), ==, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, cfg + 0x2004), ==, 0xffffffff);
    assert_running(qts);
    qtest_quit(qts);
}

static void test_shub_pi_error_mask_reset(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* The header's INIT: every valid bit set, so every source is masked. */
    g_assert_cmphex(shub_rd(qts, 0x120050080ULL), ==, 0x7ffe00000ULL);
    g_assert_cmphex(shub_rd(qts, 0x120050500ULL), ==, 0x1fffffULL);
    g_assert_cmphex(shub_rd(qts, 0x120050a80ULL), ==, 0x7ffffffffULL);
    shub_wr(qts, 0x120050a80ULL, 0);
    g_assert_cmphex(shub_rd(qts, 0x120050a80ULL), ==, 0);
    assert_running(qts);
    qtest_quit(qts);
}

static void test_shub_override_table_reset(void)
{
    static const struct {
        uint64_t offset;
        uint64_t reset;
    } tables[] = {
        { 0x150022480ULL, 0x8000000000000000ULL },  /* NI0 */
        { 0x150023480ULL, 0x8000000000000000ULL },  /* NI1 */
        { 0x150024480ULL, 0x8000000000002000ULL },  /* MD */
        { 0x150020480ULL, 0x8000000000000000ULL },  /* IILB */
        { 0x150021480ULL, 0x8000000000002000ULL },  /* PI */
    };
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* The PROM writes 0 to disable the override; before that it is set. */
    for (unsigned i = 0; i < ARRAY_SIZE(tables); i++) {
        g_assert_cmphex(shub_rd(qts, tables[i].offset), ==, tables[i].reset);
        shub_wr(qts, tables[i].offset, 0);
        g_assert_cmphex(shub_rd(qts, tables[i].offset), ==, 0);
    }
    assert_running(qts);
    qtest_quit(qts);
}

static void dma_prop_set(QTestState *qts, const char *name, uint64_t value)
{
    QDict *reply = qtest_qmp(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': '/machine/peripheral/dma0', "
                             "'property': %s, 'value': %" PRIu64 "}}",
                             name, value);

    g_assert(qdict_haskey(reply, "return"));
    qobject_unref(reply);
}

/* A PCI bus master reads and writes node memory through the direct window. */
static void test_pic_inbound_dma(void)
{
    QTestState *qts = altix350_start_with(
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=2,id=dma0");
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    uint8_t buf[64], expect[64];

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* The PROM's set-up: the PIC's widget may use the II, and the direct */
    /* window starts at node offset 0. */
    shub_wr(qts, 0x1c00118ULL, 0xff01);
    qtest_writeq(qts, w15 + 0x20, 15);
    qtest_writeq(qts, w15 + 0x80, 0);
    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': '/machine/peripheral/dma0', "
                             "'property': 'test-master-enable', "
                             "'value': true}}");

    /* A write: the seeded bytes land in RAM at the window's offset. */
    dma_prop_set(qts, "test-address", 0x80200000ULL);
    dma_prop_set(qts, "test-length", sizeof(buf));
    dma_prop_set(qts, "test-seed", 0x30);
    dma_prop_set(qts, "test-command", 1);
    qtest_memread(qts, 0x200000, buf, sizeof(buf));
    for (unsigned i = 0; i < sizeof(buf); i++) {
        expect[i] = 0x30 + i;
    }
    g_assert(!memcmp(buf, expect, sizeof(buf)));

    /* A read: the device fetches RAM and writes it elsewhere. */
    for (unsigned i = 0; i < sizeof(buf); i++) {
        buf[i] = 0xa0 ^ i;
    }
    qtest_memwrite(qts, 0x300000, buf, sizeof(buf));
    dma_prop_set(qts, "test-source", 0x80300000ULL);
    dma_prop_set(qts, "test-address", 0x80400000ULL);
    dma_prop_set(qts, "test-command", 2);
    qtest_memread(qts, 0x400000, expect, sizeof(expect));
    g_assert(!memcmp(buf, expect, sizeof(buf)));
    assert_running(qts);
    qtest_quit(qts);
}

/* A requester whose widget has no inbound access is refused. */
static void test_pic_inbound_dma_refused(void)
{
    QTestState *qts = altix350_start_with(
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=2,id=dma0");
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writeq(qts, w15 + 0x20, 15);
    qtest_writeq(qts, w15 + 0x80, 0);
    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': '/machine/peripheral/dma0', "
                             "'property': 'test-master-enable', "
                             "'value': true}}");
    dma_prop_set(qts, "test-address", 0x80200000ULL);
    dma_prop_set(qts, "test-length", 8);
    dma_prop_set(qts, "test-command", 1);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

/* The translated window: an entry per page, valid bit, address in 47:12. */
static void test_pic_ate_dma(void)
{
    QTestState *qts = altix350_start_with(
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=2,id=dma0");
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    uint8_t buf[32], expect[32] = { 0 };
    QDict *reply;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, 0x1c00118ULL, 0xff01);
    qtest_writeq(qts, w15 + 0x20, 15);
    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': '/machine/peripheral/dma0', "
                             "'property': 'test-master-enable', "
                             "'value': true}}");
    /* The plain words keep what is written. */
    qtest_writeq(qts, w15 + 0x280, 0x88889999);
    g_assert_cmphex(qtest_readq(qts, w15 + 0x280), ==, 0x88889999);
    /* Entry 1 maps PCI 0x40001000 to node offset 0x300000; entry 2 is off. */
    qtest_writeq(qts, w15 + 0x10000 + 8, 0x300000 | 1);
    dma_prop_set(qts, "test-address", 0x40001010ULL);
    dma_prop_set(qts, "test-length", sizeof(buf));
    dma_prop_set(qts, "test-seed", 0x50);
    dma_prop_set(qts, "test-command", 1);
    qtest_memread(qts, 0x300010, buf, sizeof(buf));
    for (unsigned i = 0; i < sizeof(buf); i++) {
        expect[i] = 0x50 + i;
    }
    g_assert(!memcmp(buf, expect, sizeof(buf)));
    /* An entry that is not valid delivers nothing. */
    dma_prop_set(qts, "test-address", 0x40002000ULL);
    dma_prop_set(qts, "test-seed", 0x70);
    dma_prop_set(qts, "test-command", 1);
    reply = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                      "{'path': '/machine/peripheral/dma0', "
                      "'property': 'test-result'}}");
    g_assert_cmpint(qdict_get_int(reply, "return"), !=, 0);
    qobject_unref(reply);
    assert_running(qts);
    qtest_quit(qts);
}

/* Control bit 21 makes the table's pages 16 KiB: entry 1 is at 0x40004000. */
static void test_pic_ate_dma_16k_pages(void)
{
    QTestState *qts = altix350_start_with(
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=2,id=dma0");
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    uint8_t buf[32], expect[32];

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, 0x1c00118ULL, 0xff01);
    qtest_writeq(qts, w15 + 0x20, 15 | (1u << 21));
    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': '/machine/peripheral/dma0', "
                             "'property': 'test-master-enable', "
                             "'value': true}}");
    qtest_writeq(qts, w15 + 0x10000 + 8, 0x300000 | 1);
    dma_prop_set(qts, "test-address", 0x40005010ULL);
    dma_prop_set(qts, "test-length", sizeof(buf));
    dma_prop_set(qts, "test-seed", 0x50);
    dma_prop_set(qts, "test-command", 1);
    qtest_memread(qts, 0x301010, buf, sizeof(buf));
    for (unsigned i = 0; i < sizeof(buf); i++) {
        expect[i] = 0x50 + i;
    }
    g_assert(!memcmp(buf, expect, sizeof(buf)));
    assert_running(qts);
    qtest_quit(qts);
}

/* PCI address bit 29 of the translated window byte-swaps the transfer. */
static void test_pic_ate_dma_swap(void)
{
    QTestState *qts = altix350_start_with(
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=2,id=dma0");
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    uint8_t buf[32], expect[32];

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, 0x1c00118ULL, 0xff01);
    qtest_writeq(qts, w15 + 0x20, 15);
    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': '/machine/peripheral/dma0', "
                             "'property': 'test-master-enable', "
                             "'value': true}}");
    /* Entry 1 maps PCI 0x40001000, and 0x60001000 with the swap bit set. */
    qtest_writeq(qts, w15 + 0x10000 + 8, 0x300000 | 1);
    dma_prop_set(qts, "test-address", 0x60001010ULL);
    dma_prop_set(qts, "test-length", sizeof(buf));
    dma_prop_set(qts, "test-seed", 0x50);
    dma_prop_set(qts, "test-command", 1);
    qtest_memread(qts, 0x300010, buf, sizeof(buf));
    for (unsigned i = 0; i < sizeof(buf); i++) {
        /* Each four-byte word arrives reversed. */
        expect[i] = 0x50 + (i & ~3u) + (3 - (i & 3));
    }
    g_assert(!memcmp(buf, expect, sizeof(buf)));
    assert_running(qts);
    qtest_quit(qts);
}

/* A bus master's request naming another node's NASID is refused. */
static void test_pic_dma_other_node_is_refused(void)
{
    QTestState *qts = altix350_start_with(
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=2,id=dma0");
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, 0x1c00118ULL, 0xff01);
    qtest_writeq(qts, w15 + 0x20, 15);
    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': '/machine/peripheral/dma0', "
                             "'property': 'test-master-enable', "
                             "'value': true}}");
    /* Entry 1 maps PCI 0x40001000 to node 1 (bits 46:36), not this node. */
    qtest_writeq(qts, w15 + 0x10000 + 8, (1ULL << 36) | 0x300000 | 1);
    dma_prop_set(qts, "test-address", 0x40001010ULL);
    dma_prop_set(qts, "test-length", 32);
    dma_prop_set(qts, "test-seed", 0x50);
    dma_prop_set(qts, "test-command", 1);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

/* Big window 6 reaches the PIC's PCI memory through its table entry. */
static void test_shub_big_window(void)
{
    QTestState *qts = altix350_start_with(
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=2,id=dma0");
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    const uint64_t cfg = w15 + 0x22000;
    const uint64_t big = 0x800000000ULL + (6ULL << 29);

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* The PROM's entry: widget 15, Crosstalk address 0x4000_0000 (head 0). */
    shub_wr(qts, 0x1c00188ULL, 0xf02);
    g_assert_cmphex(shub_rd(qts, 0x1c00188ULL), ==, 0xf02);
    /* The card's first BAR at PCI memory 0x700000, memory space on. */
    qtest_writel(qts, cfg + 0x10, 0x700000);
    qtest_writel(qts, cfg + 4, 2);
    /* Its scratch word keeps what a 32-bit store through the window wrote. */
    qtest_writel(qts, big + 0x700024, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, big + 0x700024), ==, 0x12345678);
    /* The second BAR serves the little-endian slice at any width. */
    qtest_writel(qts, cfg + 0x14, 0x800000);
    g_assert_cmphex(qtest_readq(qts, big + 0x800000), ==,
                    0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readl(qts, big + 0x800000), ==, 0x89abcdef);
    g_assert_cmphex(qtest_readl(qts, big + 0x800004), ==, 0x01234567);
    g_assert_cmphex(qtest_readw(qts, big + 0x800002), ==, 0x89ab);
    g_assert_cmphex(qtest_readb(qts, big + 0x800000), ==, 0xef);
    g_assert_cmphex(qtest_readb(qts, big + 0x800001), ==, 0xcd);
    qtest_writeq(qts, big + 0x800008, 0xaaaabbbbccccddddULL);
    g_assert_cmphex(qtest_readl(qts, big + 0x800008), ==, 0xccccdddd);
    g_assert_cmphex(qtest_readq(qts, big + 0x800008), ==,
                    0xaaaabbbbccccddddULL);
    assert_running(qts);
    /* An entry for a widget nothing answers is refused. */
    shub_wr(qts, 0x1c00188ULL, 0x302);
    qtest_readl(qts, big + 0x700024);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

/* Bus configuration 1 bit 3 stores on an ISP12160 only when asked. */
static void test_isp12160_cfg1_sxp_select(void)
{
    QTestState *qts = altix350_start_with(
        "-device qlogic-isp12160,bus=pic-head-0.0,addr=2");
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    const uint64_t big = 0x800000000ULL + (6ULL << 29);
    uint16_t cfg1;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, 0x1c00188ULL, 0xf02);
    /* Device 3, the machine's ISP, keeps the bit; the one at 2 does not. */
    qtest_writel(qts, w15 + 0x23000 + 0x14, 0x800000);
    qtest_writel(qts, w15 + 0x23000 + 4, 2);
    qtest_writel(qts, w15 + 0x22000 + 0x14, 0x900000);
    qtest_writel(qts, w15 + 0x22000 + 4, 2);
    /* Bus configuration 1 is at offset 6 of the register window. */
    cfg1 = qtest_readw(qts, big + 0x800006);
    qtest_writew(qts, big + 0x800006, cfg1 | 0x0008);
    g_assert_cmphex(qtest_readw(qts, big + 0x800006), ==, cfg1 | 0x0008);
    qtest_writew(qts, big + 0x800006, cfg1);
    g_assert_cmphex(qtest_readw(qts, big + 0x800006), ==, cfg1);
    /* Without the property the bit is not kept. */
    qtest_writew(qts, big + 0x900006, cfg1 | 0x0008);
    g_assert_cmphex(qtest_readw(qts, big + 0x900006), ==, cfg1);
    assert_running(qts);
    qtest_quit(qts);
}

/* A 64-bit direct DMA lands in order unless the swap attribute is set. */
static void test_pic_direct64_dma_lanes(void)
{
    QTestState *qts = altix350_start_with(
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=2,id=dma0");
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    const uint64_t direct = 0x1ULL << 60;
    uint8_t buf[8];

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, 0x1c00118ULL, 0xff01);
    qtest_writeq(qts, w15 + 0x20, 15);
    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': '/machine/peripheral/dma0', "
                             "'property': 'test-master-enable', "
                             "'value': true}}");
    dma_prop_set(qts, "test-length", sizeof(buf));
    dma_prop_set(qts, "test-seed", 0x10);
    dma_prop_set(qts, "test-address", direct | 0x400000);
    dma_prop_set(qts, "test-command", 1);
    qtest_memread(qts, 0x400000, buf, sizeof(buf));
    for (unsigned i = 0; i < sizeof(buf); i++) {
        g_assert_cmphex(buf[i], ==, 0x10 + i);
    }
    /* Swap set: each 32-bit data phase arrives reversed. */
    dma_prop_set(qts, "test-address", direct | (1ULL << 55) | 0x500000);
    dma_prop_set(qts, "test-command", 1);
    qtest_memread(qts, 0x500000, buf, sizeof(buf));
    for (unsigned i = 0; i < sizeof(buf); i++) {
        g_assert_cmphex(buf[i], ==, 0x10 + (i ^ 3));
    }
    assert_running(qts);
    qtest_quit(qts);
}

static void test_pic_head0_isp12160(void)
{
    QTestState *qts = altix350_start();
    /* Widget 15, head 0 configuration window of device 3. */
    uint64_t cfg = 0x800000000ULL + (15ULL << 24) + 0x23000;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* Vendor 0x1077, device 0x1216. */
    g_assert_cmphex(qtest_readl(qts, cfg), ==, 0x12161077);
    /* Device 2 stays empty: all ones. */
    g_assert_cmphex(qtest_readl(qts, cfg - 0x1000), ==, 0xffffffff);
    assert_running(qts);
    qtest_quit(qts);
}

/* The wired ISP's second BAR is reached through big window 6. */
static void test_pic_isp12160_cfg1(void)
{
    QTestState *qts = altix350_start();
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    const uint64_t cfg = w15 + 0x23000;
    const uint64_t big = 0x800000000ULL + (6ULL << 29);
    uint16_t cfg1;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, 0x1c00188ULL, 0xf02);
    /* Memory BAR 1 at PCI 0x800000, memory space on. */
    qtest_writel(qts, cfg + 0x14, 0x800000);
    qtest_writel(qts, cfg + 4, 2);
    /* Bus configuration 1, offset 6: the EFI driver's bit 3 reads back. */
    cfg1 = qtest_readw(qts, big + 0x800006);

    qtest_writew(qts, big + 0x800006, cfg1 | 0x0008);
    g_assert_cmphex(qtest_readw(qts, big + 0x800006), ==, cfg1 | 0x0008);
    qtest_writew(qts, big + 0x800006, cfg1);
    g_assert_cmphex(qtest_readw(qts, big + 0x800006), ==, cfg1);
    assert_running(qts);
    qtest_quit(qts);
}

static void test_shub_llp_reset_en(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* Reset 1 on both ports; the PROM's partition fence writes 0. */
    for (unsigned p = 0; p < 2; p++) {
        g_assert_cmphex(shub_rd(qts, 0x150000010ULL + p * 0x2000), ==, 1);
    }
    shub_wr(qts, 0x150000010ULL, 0);
    g_assert_cmphex(shub_rd(qts, 0x150000010ULL), ==, 0);
    g_assert_cmphex(shub_rd(qts, 0x150002010ULL), ==, 1);
    assert_running(qts);
    qtest_quit(qts);
}

static void test_shub_pi_credits(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* The PROM's credit adjust: write 0x88c, read the count, 8 is enough. */
    shub_wr(qts, 0x120050d80ULL, 0x88c);
    g_assert_cmphex(shub_rd(qts, 0x120060980ULL) & 0x3f, ==, 8);
    assert_running(qts);
    shub_wr(qts, 0x120060980ULL, 0);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_bte_copy(void)
{
    QTestState *qts = altix350_start();
    uint8_t src[300], dst[300], got[300];

    memset(src, 0xc3, sizeof(src));
    memset(dst, 0xad, sizeof(dst));
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_memwrite(qts, 0x100000, src, sizeof(src));
    qtest_memwrite(qts, 0x200000, dst, sizeof(dst));
    /* The PROM's self-test: two 128-byte lines, busy set, control 0x10. */
    shub_wr(qts, SHUB_BTE0, 0x100002);
    shub_wr(qts, SHUB_BTE0 + 0x08, 0x100000);
    shub_wr(qts, SHUB_BTE0 + 0x10, 0x200000);
    shub_wr(qts, SHUB_BTE0 + 0x20, 0x300000);
    shub_wr(qts, SHUB_BTE0 + 0x18, 0x10);
    g_assert_cmphex(shub_rd(qts, SHUB_BTE0) & (1 << 20), ==, 0);
    qtest_memread(qts, 0x200000, got, sizeof(got));
    memset(dst, 0xc3, 256);
    g_assert(!memcmp(got, dst, sizeof(got)));
    assert_running(qts);
    /* A source in another node is refused. */
    shub_wr(qts, SHUB_BTE0, 0x100002);
    shub_wr(qts, SHUB_BTE0 + 0x08, 1ULL << 38);
    shub_wr(qts, SHUB_BTE0 + 0x18, 0x10);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

/*
 * The PROM's SHub accesses from SAL_A to the console start, in its order,
 * with the values the PROM reset-path notes say each read must return.
 */
static void test_shub_prom_reset_path(void)
{
    QTestState *qts = altix350_start();
    const uint64_t g = SHUB_GLOBAL_MMR;
    uint64_t hist, id, ctl;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* SAL_A: node set-up, then the election. */
    shub_wr(qts, SHUB_LB_1002_0000, 0x15031010);
    shub_wr(qts, SHUB_PI_2001_0100, shub_rd(qts, SHUB_PI_2001_0100) | 1 << 24);
    qtest_writew(qts, SHUB_LOCAL_MMR + 0x7fed10000ULL,
                 shub_rd(qts, SHUB_CPU_HIST + 0x10) | 0x60);
    shub_wr(qts, SHUB_ELECT_SET, 1);
    g_assert_cmpuint(shub_rd(qts, SHUB_ELECT), ==, 1);
    shub_wr(qts, SHUB_CPU_BITS, 0);
    shub_wr(qts, SHUB_CPU_BITS - 0x10, 0);
    shub_wr(qts, 0x110060480ULL, 1);
    g_assert_cmpuint(shub_rd(qts, SHUB_ELECT), ==, 1);
    shub_wr(qts, SHUB_L1_STATE, shub_rd(qts, SHUB_L1_STATE) | 7ULL << 39);
    /* A progress write: read the history, shift in the code, store. */
    hist = shub_rd(qts, SHUB_CPU_HIST);
    shub_wr(qts, SHUB_CPU_HIST, hist << 8 | 48);
    qtest_writew(qts, SHUB_LOCAL_MMR + 0x7fed00000ULL, 48);
    /* SAL_B: the clock restarts and takes its period from rtc_ctl. */
    shub_wr(qts, SHUB_RTC_CTL, 0x100000000ULL);
    shub_wr(qts, SHUB_RTC, 0);
    shub_wr(qts, SHUB_RTC_CTL, 0x100000041ULL);
    qtest_clock_step(qts, 500000);
    g_assert_cmpuint(shub_rd(qts, SHUB_RTC), ==, 10000);
    /* One lock cycle: slice record, clock, period, status, word, release. */
    shub_rd(qts, SHUB_SLICE_REC);
    ctl = shub_rd(qts, SHUB_RTC_CTL);
    g_assert_cmpuint((ctl >> 4 & 0x3ff) * 10 + 10, ==, 50);
    g_assert_cmpuint(shub_rd(qts, SHUB_LOCK_STATUS), ==, 0);
    shub_wr(qts, SHUB_CPU_BITS, shub_rd(qts, SHUB_CPU_BITS) | 1 << 21);
    shub_wr(qts, SHUB_LOCK, 0);
    g_assert_cmphex(shub_rd(qts, SHUB_CPU_BITS), ==, 1 << 21);
    /* main(): the node id, then the global window at that NASID. */
    id = shub_rd(qts, SHUB_SHUB_ID);
    g_assert_cmphex(id >> 32 & 0x7ff, ==, 0);
    g_assert_cmphex(qtest_readq(qts, g + SHUB_SLICE_PRESENT) & 1, ==, 1);
    g_assert_cmphex(qtest_readq(qts, g + SHUB_MD_REG_0000_0000) >> 12 & 0xffff, ==,
                    0x4363);
    assert_running(qts);
    qtest_quit(qts);
}

/* One 64-vector word of the SAPIC IRR, from "info registers". */
static uint64_t sapic_irr(QTestState *qts, unsigned word)
{
    g_autofree char *regs = qtest_hmp(qts, "info registers");
    const char *line = strstr(regs, "SAPIC IRR: ");

    g_assert_nonnull(line);
    line += strlen("SAPIC IRR: ");
    return g_ascii_strtoull(line + word * 17, NULL, 16);
}

/*
 * A device raises a vector on the vCPU thread through queued work, so the
 * register read can come first. Wait for the work to run, then assert.
 */
static void sapic_irr_is(QTestState *qts, unsigned word, uint64_t expected)
{
    for (unsigned i = 0; i < 500 && sapic_irr(qts, word) != expected; i++) {
        g_usleep(10000);
    }
    g_assert_cmphex(sapic_irr(qts, word), ==, expected);
}

static void test_shub_ipi(void)
{
    QTestState *qts = altix350_start();
    const uint64_t send = (1ULL << 63) | (0x31ULL << 52);

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    g_assert_cmphex(sapic_irr(qts, 0), ==, 0);
    /* Nothing is pending from reset. */
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 0);
    shub_wr(qts, SHUB_IPI_INT, send);
    sapic_irr_is(qts, 0, 1ULL << 0x31);
    /* The IPI shows as pending in the event register until cleared. */
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 1 << 28);
    shub_wr(qts, SHUB_EVENT_OCCURRED + 8, 1 << 28);
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 0);
    /* A write to the register itself sets the bits it names. */
    shub_wr(qts, SHUB_EVENT_OCCURRED, 0x100);
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 0x100);
    shub_wr(qts, SHUB_EVENT_OCCURRED + 8, 0x100);
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 0);
    assert_running(qts);
    qtest_quit(qts);
}

/* Both processors exist, with slices 0 and 2, and an IPI reaches the second. */
static void test_shub_two_processors(void)
{
    QTestState *qts = altix350_start_with("-smp 2");
    g_autofree char *regs = NULL;
    const char *cpu1;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    g_assert_cmphex(qtest_readq(qts, SHUB_GLOBAL_MMR + SHUB_SLICE_PRESENT), ==,
                    3);
    shub_wr(qts, SHUB_IPI_INT, (1ULL << 63) | (0x31ULL << 52) | (0x100ULL << 4));
    for (unsigned i = 0; i < 500; i++) {
        g_free(regs);
        regs = qtest_hmp(qts, "info registers -a");
        cpu1 = strstr(regs, "CPU#1");
        g_assert_nonnull(cpu1);
        cpu1 = strstr(cpu1, "SAPIC IRR: ");
        if (g_ascii_strtoull(cpu1 + strlen("SAPIC IRR: "), NULL, 16)) {
            break;
        }
        g_usleep(10000);
    }
    g_assert_cmphex(g_ascii_strtoull(cpu1 + strlen("SAPIC IRR: "), NULL, 16), ==,
                    1ULL << 0x31);
    assert_running(qts);
    qtest_quit(qts);
}

/* A load from the compatibility-space hole completes with zeros. */
static void test_compat_hole_reads_zero(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    g_assert_cmphex(qtest_readb(qts, 0x1008e1610ULL), ==, 0);
    g_assert_cmphex(qtest_readq(qts, 0x1008e1610ULL), ==, 0);
    assert_running(qts);
    /* A store there and a load below 4 GiB with nothing mapped are refused. */
    qtest_writeb(qts, 0x1008e1610ULL, 1);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_ipi_unmodelled(void)
{
    static const uint64_t cases[] = {
        (1ULL << 63) | (0x31ULL << 52) | 2, /* PMI */
        (1ULL << 63) | (0x31ULL << 52) | 4, /* NMI */
        (1ULL << 63) | (0x31ULL << 52) | 5, /* INIT */
        (1ULL << 63) | (5ULL << 52),        /* reserved vector */
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        QTestState *qts = altix350_start();

        qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
        shub_wr(qts, SHUB_IPI_INT, cases[i]);
        assert_unmodelled(qts);
        qtest_quit(qts);
    }
}

static void test_shub_comparator_fires(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, SHUB_RTC_CTL, 0x41);    /* 50 ns a tick */
    shub_wr(qts, SHUB_RTC, 0);
    /* The driver's order: compare, configuration, enable. */
    shub_wr(qts, SHUB_INT_CMP, 100);
    shub_wr(qts, SHUB_RTC_INT_CONFIG, 0x40ULL << 52);
    shub_wr(qts, SHUB_RTC_INT_ENABLE, 1);
    qtest_clock_step(qts, 4950);
    /* Not yet. */
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 0);
    g_assert_cmphex(sapic_irr(qts, 1), ==, 0);
    qtest_clock_step(qts, 100);
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 1 << 24);
    /* Vector 0x40 is bit 0 of the second IRR word. */
    sapic_irr_is(qts, 1, 1);
    shub_wr(qts, SHUB_EVENT_OCCURRED + 8, 1 << 24);
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 0);
    /* A compare the counter has already passed fires at once. */
    shub_wr(qts, SHUB_INT_CMP, 10);
    shub_wr(qts, SHUB_RTC_INT_ENABLE, 0);
    shub_wr(qts, SHUB_RTC_INT_ENABLE, 1);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 1 << 24);
    shub_wr(qts, SHUB_EVENT_OCCURRED + 8, 1 << 24);
    /* A compare far ahead never fires, and disabling cancels a pending one. */
    shub_wr(qts, SHUB_INT_CMP, 0x7fffffffffffffULL);
    qtest_clock_step(qts, 1000000);
    shub_wr(qts, SHUB_INT_CMP, 100000);
    shub_wr(qts, SHUB_RTC_INT_ENABLE, 0);
    qtest_clock_step(qts, 100000000);
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 0);
    assert_running(qts);
    qtest_quit(qts);
}

static void test_shub_rtc0_bus_error_message(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, SHUB_RTC_CTL, 0x41);    /* 50 ns a tick */
    shub_wr(qts, SHUB_RTC, 0);
    /* The PROM's order: configuration (agent bit), compare, enable. */
    shub_wr(qts, SHUB_RTC0_INT_CONFIG, 8);
    shub_wr(qts, SHUB_INT_CMPA, 100);
    shub_wr(qts, SHUB_RTC0_INT_ENABLE, 1);
    g_assert_cmphex(shub_rd(qts, SHUB_RTC0_INT_CONFIG), ==, 8);
    qtest_clock_step(qts, 4950);
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 0);
    qtest_clock_step(qts, 100);
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 1 << 23);
    sapic_irr_is(qts, 0, 1);
    /* The undo: disable, then clear the event through the alias. */
    shub_wr(qts, SHUB_RTC0_INT_ENABLE, 0);
    shub_wr(qts, SHUB_EVENT_OCCURRED + 8, 1 << 23);
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 0);
    assert_running(qts);
    qtest_quit(qts);
}

static void test_shub_interrupt_destinations(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* Plain registers: stable values the PROM writes and never reads. */
    shub_wr(qts, SHUB_INT_NODE_ID_CONFIG, 0x800);
    shub_wr(qts, SHUB_IPI_INT_ENABLE, 1);
    for (unsigned i = 0; i < 6; i++) {
        g_assert_cmphex(shub_rd(qts, SHUB_LOCAL_INT_CONFIG + i * 0x100), ==, 0);
        shub_wr(qts, SHUB_LOCAL_INT_CONFIG + i * 0x100, 0x100000000002ULL + i);
        shub_wr(qts, SHUB_LOCAL_INT0_ENABLE + i * 0x100, 0x7ef);
        g_assert_cmphex(shub_rd(qts, SHUB_LOCAL_INT_CONFIG + i * 0x100), ==,
                        0x100000000002ULL + i);
    }
    shub_wr(qts, SHUB_PTC_PARMS, 0x3e8000186a0ULL);
    g_assert_cmphex(shub_rd(qts, SHUB_PTC_PARMS), ==, 0x3e8000186a0ULL);
    /* The overflow word's alias takes the PROM's write of 8. */
    shub_wr(qts, SHUB_EVENT_OVERFLOW + 8, 8);
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OVERFLOW), ==, 0);
    assert_running(qts);
    /* A destination does not deliver anything by itself. */
    g_assert_cmphex(sapic_irr(qts, 0), ==, 0);
    qtest_quit(qts);
}

/* The PROM's II set-up (PROM I/O interface notes, rows A and B), in part. */
static void test_shub_ii_setup(void)
{
    QTestState *qts = altix350_start();
    const uint64_t ii = 0x1800000;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* The non-II word it writes first. */
    shub_wr(qts, 0x110071080ULL, 0);
    /* The link is up whatever was written: an I/O brick is attached. */
    g_assert_cmphex(shub_rd(qts, ii + 0x400128) & (1 << 13), ==, 1 << 13);
    shub_wr(qts, ii + 0x400128, 0);
    g_assert_cmphex(shub_rd(qts, ii + 0x400128) & (1 << 13), ==, 1 << 13);
    /* Entries read, then written zero. */
    for (unsigned i = 0; i < 8; i++) {
        shub_rd(qts, ii + 0x400308 + 8 * i);
        shub_wr(qts, ii + 0x400308 + 8 * i, 0);
        shub_wr(qts, ii + 0x400348 + 8 * i, 0);
    }
    /* Error clear takes the PROM's all-ones write. */
    shub_wr(qts, ii + 0x4001f8, 0x7ffffffffffULL);
    /* Graphics maps and widget control read back what was written. */
    shub_wr(qts, ii + 0x400140, 2);
    shub_wr(qts, ii + 0x400148, 3);
    g_assert_cmphex(shub_rd(qts, ii + 0x400140), ==, 2);
    g_assert_cmphex(shub_rd(qts, ii + 0x400148), ==, 3);
    shub_wr(qts, ii + 0x400020, 0x1000a);
    g_assert_cmphex(shub_rd(qts, ii + 0x400020), ==, 0x1000a);
    /* A read-modify-write of one bit, and the last CRB entry's last word. */
    shub_wr(qts, ii + 0x4001a0, shub_rd(qts, ii + 0x4001a0) | (1ULL << 42));
    g_assert_cmphex(shub_rd(qts, ii + 0x4001a0), ==, 1ULL << 42);
    shub_wr(qts, ii + 0x400420 + 14 * 0x30, 0);
    shub_wr(qts, ii + 0x4003c0, 0x1fffff);
    assert_running(qts);
    /* The local window reaches the same block at 0x1000000 plus the offset. */
    g_assert_cmphex(shub_rd(qts, 0x1000000 + 0x400140), ==, 2);
    /* Nothing reads the error clear register back. */
    shub_rd(qts, ii + 0x4001f8);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_crb_message_control(void)
{
    QTestState *qts = altix350_start();
    const uint64_t crb = 0x120000000ULL;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    g_assert_cmphex(shub_rd(qts, crb), ==, 6);
    /* Normal boot ORs enables into it. */
    shub_wr(qts, crb, shub_rd(qts, crb) | 0x3b03f7);
    g_assert_cmphex(shub_rd(qts, crb), ==, 0x3b03f7 | 6);
    /* Bits 21 to 16 are written by the PROM: they are kept, not masked. */
    shub_wr(qts, crb, 0x3b03f7ULL | 6);
    g_assert_cmphex(shub_rd(qts, crb), ==, 0x3b03f7ULL | 6);
    shub_wr(qts, crb, 0x3f1ULL | 6);
    g_assert_cmphex(shub_rd(qts, crb), ==, 0x3f1ULL | 6);
    /* The neighbours reset to the header's values, readable before a write. */
    g_assert_cmphex(shub_rd(qts, crb + 0x80), ==, 0);
    g_assert_cmphex(shub_rd(qts, crb + 0x100), ==, 0);
    g_assert_cmphex(shub_rd(qts, crb + 0x180), ==, 7);
    g_assert_cmphex(shub_rd(qts, SHUB_PTC_PARMS), ==, 0x7ffffffffULL);
    shub_wr(qts, crb + 0x80, 0x10);
    g_assert_cmphex(shub_rd(qts, crb + 0x80), ==, 0x10);
    shub_wr(qts, crb + 0x100, 3);
    shub_wr(qts, crb + 0x180, 1ULL << 63);
    assert_running(qts);
    /* The die path's quiesce store is the PAL fault exit: a refused store. */
    shub_wr(qts, crb, 0x1000100000000ULL);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_pic_interrupt_target(void)
{
    QTestState *qts = altix350_start();
    const uint64_t ii_int = 0x110000000ULL;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* The PROM's destination and enable writes, both slices. */
    shub_wr(qts, ii_int + 0x80, 0x4a0);
    shub_wr(qts, ii_int + 0x180, 0x4a1);
    shub_wr(qts, ii_int + 0x200, 1);
    shub_wr(qts, ii_int + 0x280, 1);
    g_assert_cmphex(shub_rd(qts, ii_int + 0x80), ==, 0x4a0);
    g_assert_cmphex(shub_rd(qts, ii_int + 0x280), ==, 1);
    assert_running(qts);
    /* A packet is the send bit and a vector, at either target. */
    shub_wr(qts, ii_int, 0x100 | 0x51);
    shub_wr(qts, ii_int + 0x100, 0x100 | 0x52);
    /* Vectors 0x51 and 0x52 are bits 17 and 18 of the second IRR word. */
    sapic_irr_is(qts, 1, (1ULL << 17) | (1ULL << 18));
    assert_running(qts);
    /* Any other layout is not known. */
    shub_wr(qts, ii_int, 0x51);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

/* The PROM's I/O discovery reads of the crossbow, from the PIC's own model. */
static void test_pic_crossbow(void)
{
    QTestState *qts = altix350_start();
    const uint64_t w0 = 0x800000000ULL;
    const uint64_t w15 = w0 + (15ULL << 24);

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* Id: part 0xd100 in 27:12, revision 3, a 4-byte read at +0. */
    g_assert_cmphex((qtest_readl(qts, w0) >> 12) & 0xffff, ==, 0xd100);
    g_assert_cmphex(qtest_readl(qts, w0) >> 28, ==, 3);
    /* Status bits 9:6 name the hub's link, which must be 8 to 15. */
    g_assert_cmphex((qtest_readl(qts, w0 + 8) >> 6) & 0xf, ==, 10);
    /* The handshake word keeps four bits. */
    qtest_writel(qts, w0 + 0xc0, 10);
    g_assert_cmphex(qtest_readl(qts, w0 + 0xc0), ==, 10);
    qtest_writel(qts, w0 + 0xc8, 1);
    /* The hub's link (10) and the PIC's (15) are alive and present. */
    g_assert_cmphex(qtest_readl(qts, w0 + 0x100 + 2 * 0x40 + 0x10) >> 31,
                    ==, 1);
    g_assert_cmphex(qtest_readl(qts, w0 + 0x100 + 2 * 0x40 + 0x38) & 0x20, ==,
                    0x20);
    g_assert_cmphex(qtest_readl(qts, w0 + 0x100 + 7 * 0x40 + 0x10) >> 31,
                    ==, 1);
    g_assert_cmphex(qtest_readl(qts, w0 + 0x100 + 7 * 0x40 + 0x38) & 0x20, ==,
                    0x20);
    /* The PIC behind window 15 is a PIC: part 0xd102. */
    g_assert_cmphex((qtest_readl(qts, w15) >> 12) & 0xffff, ==, 0xd102);
    assert_running(qts);
    qtest_quit(qts);
}

/* A PIC interrupt pin reaches the SHub's I/O interrupt target as a packet. */
static void test_pic_interrupt_reaches_the_processor(void)
{
    QTestState *qts = altix350_start();
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    /* The target address: bit 47, node 0, SH_II_INT0; the vector in 63:48. */
    const uint64_t target = (1ULL << 47) | 0x110000000ULL;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    g_assert_cmphex(sapic_irr(qts, 1), ==, 0);
    qtest_writeq(qts, w15 + 0x130, (0x51ULL << 48) | target);
    qtest_writeq(qts, w15 + 0x108, 1);
    qtest_writeq(qts, w15 + 0x180, 1);
    qtest_clock_step(qts, 2000);
    /* Vector 0x51 is bit 17 of the second IRR word. */
    sapic_irr_is(qts, 1, 1ULL << 17);
    assert_running(qts);
    qtest_quit(qts);
}

/* The II interrupt packet goes to the processor its INT0 config's pid names. */
static void test_pic_interrupt_follows_the_target_pid(void)
{
    QTestState *qts = altix350_start_with("-smp 2");
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);
    const uint64_t target = (1ULL << 47) | 0x110000000ULL;
    g_autofree char *regs = NULL;
    const char *cpu1;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* Slice 2 is the second processor, pid 0x100. */
    shub_wr(qts, 0x110000080ULL, 0x100ULL << 4);
    qtest_writeq(qts, w15 + 0x130, (0x51ULL << 48) | target);
    qtest_writeq(qts, w15 + 0x108, 1);
    qtest_writeq(qts, w15 + 0x180, 1);
    qtest_clock_step(qts, 2000);
    for (unsigned i = 0; i < 500; i++) {
        g_free(regs);
        regs = qtest_hmp(qts, "info registers -a");
        cpu1 = strstr(regs, "CPU#1");
        g_assert_nonnull(cpu1);
        cpu1 = strstr(cpu1, "SAPIC IRR: ");
        if (g_ascii_strtoull(cpu1 + strlen("SAPIC IRR: ") + 17, NULL, 16)) {
            break;
        }
        g_usleep(10000);
    }
    g_assert_cmphex(g_ascii_strtoull(cpu1 + strlen("SAPIC IRR: ") + 17, NULL,
                                     16), ==, 1ULL << 17);
    g_assert_cmphex(sapic_irr(qts, 1), ==, 0);
    assert_running(qts);
    qtest_quit(qts);
}

/* The packet's node field must match the SHub's NASID, wherever it moves. */
static void test_pic_interrupt_node_field(void)
{
    const uint64_t target = (1ULL << 47) | 0x110000000ULL;
    const uint64_t node1 = 1ULL << 36;
    QTestState *qts = altix350_start();
    uint64_t w15 = 0x800000000ULL + (15ULL << 24);

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_writeq(qts, w15 + 0x130, (0x51ULL << 48) | target | node1);
    qtest_writeq(qts, w15 + 0x108, 1);
    qtest_writeq(qts, w15 + 0x180, 1);
    qtest_clock_step(qts, 2000);
    assert_unmodelled(qts);
    qtest_quit(qts);

    qts = altix350_start();
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, SHUB_SHUB_ID, 0x30000000ULL | (1ULL << 32));
    w15 += 1ULL << 38;
    qtest_writeq(qts, w15 + 0x130, (0x51ULL << 48) | target | node1);
    qtest_writeq(qts, w15 + 0x108, 1);
    qtest_writeq(qts, w15 + 0x180, 1);
    qtest_clock_step(qts, 2000);
    sapic_irr_is(qts, 1, 1ULL << 17);
    assert_running(qts);
    qtest_quit(qts);
}

static unsigned widget_spaces(QTestState *qts)
{
    g_autofree char *all = qtest_hmp(qts, "info mtree");
    unsigned n = 0;

    for (const char *p = all; (p = strstr(p, "address-space: sgi-shub.widget"));
         p++) {
        n++;
    }
    return n;
}

/* Probing the type's properties builds and drops a SHub: no spaces linger. */
static void test_shub_probe_leaves_no_address_spaces(void)
{
    QTestState *qts = altix350_start();
    unsigned before = widget_spaces(qts);
    QDict *reply;

    reply = qtest_qmp(qts, "{'execute': 'qom-list-properties',"
                           " 'arguments': {'typename': 'sgi-shub'}}");
    qobject_unref(reply);
    g_assert_cmpuint(before, ==, 15);
    g_assert_cmpuint(widget_spaces(qts), ==, before);
    qtest_quit(qts);
}

static void test_shub_firmware_words(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(shub_rd(qts, SHUB_CPU_HIST + i * 0x10), ==, 0);
        shub_wr(qts, SHUB_CPU_HIST + i * 0x10, 0xab00 + i);
        g_assert_cmphex(shub_rd(qts, SHUB_CPU_HIST + i * 0x10), ==, 0xab00 + i);
    }
    g_assert_cmphex(shub_rd(qts, SHUB_CPU_BITS), ==, 0);
    shub_wr(qts, SHUB_CPU_BITS, 1ULL << 47 | 0x3c000000);
    g_assert_cmphex(shub_rd(qts, SHUB_CPU_BITS), ==, 1ULL << 47 | 0x3c000000);
    /* Bit 24 of the PI word must stick; bits 41:39 of the L1 word too. */
    g_assert_cmphex(shub_rd(qts, SHUB_PI_2001_0100), ==, 0);
    shub_wr(qts, SHUB_PI_2001_0100, 1ULL << 24);
    g_assert_cmphex(shub_rd(qts, SHUB_PI_2001_0100), ==, 1ULL << 24);
    shub_wr(qts, SHUB_L1_STATE, 7ULL << 39);
    g_assert_cmphex(shub_rd(qts, SHUB_L1_STATE), ==, 7ULL << 39);
    /* The VGA remap word resets to 0 and keeps what the PROM deposits. */
    g_assert_cmphex(shub_rd(qts, SHUB_FSB_VGA_REMAP), ==, 0);
    shub_wr(qts, SHUB_FSB_VGA_REMAP, 0x1fffULL << 49);
    g_assert_cmphex(shub_rd(qts, SHUB_FSB_VGA_REMAP), ==, 0x1fffULL << 49);
    assert_running(qts);
    qtest_quit(qts);
}

static void test_shub_slice_registers(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    g_assert_cmphex(shub_rd(qts, SHUB_SLICE_PRESENT), ==, 1);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(shub_rd(qts, SHUB_SLICE_REC + i * 0x80), ==, 0);
    }
    assert_running(qts);
    shub_rd(qts, SHUB_SLICE_REC + 5 * 0x80);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_write_only_register(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, SHUB_LB_1002_0000, 0x15031010);
    assert_running(qts);
    shub_rd(qts, SHUB_LB_1002_0000);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_rtc_stopped_at_reset(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* The header's initial control word has the counter disabled. */
    g_assert_cmphex(shub_rd(qts, SHUB_RTC_CTL), ==, 0x100000000ULL);
    g_assert_cmpuint(shub_rd(qts, SHUB_RTC), ==, 0);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(shub_rd(qts, SHUB_RTC), ==, 0);
    /* Enabling it starts the count from where it stood. */
    shub_wr(qts, SHUB_RTC_CTL, 0x100000041ULL);
    qtest_clock_step(qts, 1000);
    g_assert_cmpuint(shub_rd(qts, SHUB_RTC), ==, 20);
    /* Disabling it freezes the count. */
    shub_wr(qts, SHUB_RTC_CTL, 0x100000040ULL);
    qtest_clock_step(qts, 1000);
    g_assert_cmpuint(shub_rd(qts, SHUB_RTC), ==, 20);
    assert_running(qts);
    qtest_quit(qts);
}

static void test_shub_rtc_prom_sequence(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* The PROM's order: control, counter zero, control with period field 4. */
    shub_wr(qts, SHUB_RTC_CTL, 0x100000000ULL);
    shub_wr(qts, SHUB_RTC, 0);
    shub_wr(qts, SHUB_RTC_CTL, 0x100000041ULL);
    g_assert_cmphex(shub_rd(qts, SHUB_RTC_CTL), ==, 0x100000041ULL);
    g_assert_cmpuint(shub_rd(qts, SHUB_RTC), ==, 0);
    /* Period 10 * (4 + 1) = 50 ns, so 1 ms is 20,000 ticks at 20 MHz. */
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(shub_rd(qts, SHUB_RTC), ==, 20000);
    assert_running(qts);
    qtest_quit(qts);
}

static void test_shub_rtc_period_change_keeps_count(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, SHUB_RTC_CTL, 0x100000041ULL);
    shub_wr(qts, SHUB_RTC, 0);
    qtest_clock_step(qts, 1000);
    g_assert_cmpuint(shub_rd(qts, SHUB_RTC), ==, 20);
    /* Field 0 is a 10 ns period; the count continues from 20. */
    shub_wr(qts, SHUB_RTC_CTL, 0x100000001ULL);
    g_assert_cmpuint(shub_rd(qts, SHUB_RTC), ==, 20);
    qtest_clock_step(qts, 1000);
    g_assert_cmpuint(shub_rd(qts, SHUB_RTC), ==, 120);
    qtest_quit(qts);
}

static void test_shub_rtc_wraps_at_55_bits(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, SHUB_RTC_CTL, 0x41);
    shub_wr(qts, SHUB_RTC, (1ULL << 55) - 1);
    qtest_clock_step(qts, 100);
    g_assert_cmpuint(shub_rd(qts, SHUB_RTC), ==, 1);
    assert_running(qts);
    shub_wr(qts, SHUB_RTC, 1ULL << 55);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

static void test_shub_reset_restores_state(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    shub_wr(qts, SHUB_RTC_CTL, 0x41);
    shub_wr(qts, SHUB_INT_CMP, 9);
    shub_wr(qts, SHUB_CPU_BITS, 5);
    shub_wr(qts, SHUB_SHUB_ID, 0x30000000ULL | (1ULL << 32));
    qtest_system_reset(qts);
    g_assert_cmpuint(shub_rd(qts, SHUB_INT_CMP), ==, 0);
    g_assert_cmpuint(shub_rd(qts, SHUB_CPU_BITS), ==, 0);
    g_assert_cmphex(shub_rd(qts, SHUB_SHUB_ID), ==, 0x30000000ULL);
    /* The window moved back with the NASID. */
    g_assert_cmphex(qtest_readq(qts, SHUB_GLOBAL_MMR + SHUB_SLICE_PRESENT), ==,
                    1);
    assert_running(qts);
    /* The control word is back at its initial value. */
    g_assert_cmphex(shub_rd(qts, SHUB_RTC_CTL), ==, 0x100000000ULL);
    qtest_quit(qts);
}

static void put_fit_entry(uint8_t *p, uint64_t addr, uint32_t units,
                          uint8_t type)
{
    stq_le_p(p, addr);
    stl_le_p(p + 8, units);
    p[14] = type;
}

/* FIT at the flash base; header, PAL_B, PAL_A, a SAL_A entry, the end. */
static void fit_image_data(uint8_t *data)
{
    stl_le_p(data + FLASH_SIZE - 0x20, 0xffa00000);
    memcpy(data, "_FIT_   ", 8);
    stl_le_p(data + 8, 5);
    put_fit_entry(data + 16, 0x80000000ffe78000ULL, 0xaa14, 0x01);
    put_fit_entry(data + 32, 0x80000000fffc9ac0ULL, 0x67c, 0x8e);
    put_fit_entry(data + 48, 0x80000000ffc4e000ULL, 0x400, 0x90);
    put_fit_entry(data + 64, 0, 0, 0x7f);
    put_fit_entry(data + FLASH_SIZE - 0x30, 0x80000000ffff8000ULL, 0x7fc, 0x8f);
}

/*
 * A 2.6.5 kernel finds the FIT by reading its pointer at 0x6ffffffe0, then
 * the table at the pointer's low 36 bits plus 0x700000000. Both reads reach
 * the PROM flash (the first through a window that ignores bit 32).
 */
static void test_fit_pointer_through_the_flash_alias(void)
{
    g_autofree uint8_t *data = g_malloc0(FLASH_SIZE);
    g_autofree char *dir = g_path_get_dirname(image_path);
    g_autofree char *fit_image = g_build_filename(dir, "fit-alias.bin", NULL);
    QTestState *qts;
    uint64_t pointer;
    uint64_t fit;

    fit_image_data(data);
    g_assert_true(g_file_set_contents(fit_image, (char *)data, FLASH_SIZE,
                                     NULL));
    qts = altix350_start_bios(fit_image, "");
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* The kernel's own address is the global window: 0xe_ffff_ffe0. */
    pointer = qtest_readq(qts, 0x800000000ULL + 0x6ffffffe0ULL);
    g_assert_cmphex(pointer, ==, 0xffa00000);
    g_assert_cmphex(qtest_readq(qts, SHUB_LOCAL_MMR + 0x6ffffffe0ULL), ==,
                    pointer);
    fit = (pointer & 0xfffffffffULL) + 0x700000000ULL;
    g_assert_cmphex(qtest_readq(qts, SHUB_LOCAL_MMR + fit), ==,
                    0x2020205f5449465fULL);
    g_assert_cmphex(qtest_readq(qts, SHUB_LOCAL_MMR + fit + 8) & 0xffffffffULL,
                    ==, 5);
    assert_running(qts);
    /* The alias is read-only; a write's effect is unknown, so it is refused. */
    qtest_writew(qts, SHUB_LOCAL_MMR + 0x6ff800000ULL, 0x1234);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

/* The node's 16550: one register per 8 bytes, global nasid 0 at 0xffed40000 */
#define UART_GLOBAL 0xffed40000ULL
#define UART_LOCAL (SHUB_LOCAL_MMR + 0x7fed40000ULL)
#define UART_LSR 0x28

static uint8_t reverse_byte(uint8_t v)
{
    v = (v >> 4) | (v << 4);
    v = ((v & 0xcc) >> 2) | ((v & 0x33) << 2);
    return ((v & 0xaa) >> 1) | ((v & 0x55) << 1);
}

static void put_escaped(uint8_t *wire, unsigned *n, uint8_t byte)
{
    if (byte == 0x7e || byte == 0x7d) {
        wire[(*n)++] = 0x7d;
        byte ^= 0x20;
    }
    wire[(*n)++] = byte;
}

/* HDLC-style frame with the L1's CRC, as the PROM sends it. */
static unsigned encode_frame(uint8_t *wire, const uint8_t *payload,
                             unsigned len)
{
    uint16_t crc = 0xffff;
    unsigned n = 0;

    wire[n++] = 0x7e;
    for (unsigned i = 0; i < len; i++) {
        crc ^= reverse_byte(payload[i]) << 8;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
        }
        put_escaped(wire, &n, payload[i]);
    }
    put_escaped(wire, &n, reverse_byte(crc >> 8) ^ 0xff);
    put_escaped(wire, &n, reverse_byte(crc) ^ 0xff);
    wire[n++] = 0x7e;
    return n;
}

/* The UART's interrupt: bit 20 latches its rising edge; interrupt 5 sends. */
static void test_uart_interrupt(void)
{
    QTestState *qts = altix350_start();
    const uint64_t cfg5 = SHUB_LOCAL_INT_CONFIG + 5 * 0x100;
    const uint64_t en5 = SHUB_LOCAL_INT0_ENABLE + 5 * 0x100;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    /* The PROM's routing: vector 0xe9, enable bit 13. */
    shub_wr(qts, cfg5, 0xe9ULL << 52);
    shub_wr(qts, en5, 1 << 13);
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 0);
    /* The transmit-empty interrupt, enabled with the holding register empty. */
    qtest_writeb(qts, UART_LOCAL + 8, 0x02);
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 1 << 20);
    sapic_irr_is(qts, 3, 1ULL << (0xe9 - 192));
    /* The bit latches the rising edge: the alias clears it at once. */
    shub_wr(qts, SHUB_EVENT_OCCURRED + 8, 1 << 20);
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 0);
    /* The line stays high and sets nothing more until it falls and rises. */
    qtest_writeb(qts, UART_LOCAL + 8, 0x00);
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 0);
    qtest_writeb(qts, UART_LOCAL + 8, 0x02);
    g_assert_cmphex(shub_rd(qts, SHUB_EVENT_OCCURRED), ==, 1 << 20);
    assert_running(qts);
    qtest_quit(qts);
}

/* After reset the transmitter is empty (LSR bit 6) and nothing is received. */
static void test_uart_line_status(void)
{
    QTestState *qts = altix350_start();

    g_assert_cmphex(qtest_readq(qts, UART_GLOBAL + UART_LSR), ==, 0x60);
    g_assert_cmphex(qtest_readq(qts, UART_LOCAL + UART_LSR), ==, 0x60);
    qtest_quit(qts);
}

/* The PROM flash holds the image on top, is erased below and answers CFI. */
static void test_prom_flash(void)
{
    const uint64_t flash = 0x7ff800000ULL;
    QTestState *qts = altix350_start();

    g_assert_cmphex(qtest_readq(qts, SHUB_LOCAL_MMR + flash), ==, UINT64_MAX);
    /* The firmware image is the flash's top 6 MiB. */
    g_assert_cmphex(qtest_readq(qts, SHUB_LOCAL_MMR + flash + 0x200000), ==,
                    qtest_readq(qts, FLASH_BASE));
    qtest_writew(qts, SHUB_LOCAL_MMR + flash + 2 * 0x55, 0x98);
    g_assert_cmphex(qtest_readw(qts, SHUB_LOCAL_MMR + flash + 2 * 0x10), ==,
                    0x51);
    qtest_writew(qts, SHUB_LOCAL_MMR + flash, 0xf0);
    g_assert_cmphex(qtest_readw(qts, SHUB_LOCAL_MMR + flash), ==, 0xffff);
    qtest_quit(qts);
}

/* A reset clears the translation entries and the plain DMA words. */
static void test_pic_ate_reset(void)
{
    QTestState *qts = altix350_start();
    const uint64_t w15 = 0x800000000ULL + (15ULL << 24);

    qtest_writeq(qts, w15 + 0x10000 + 8, 0x300000 | 1);
    qtest_writeq(qts, w15 + 0x280, 0x88889999);
    g_assert_cmphex(qtest_readq(qts, w15 + 0x10000 + 8), ==, 0x300001);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, w15 + 0x10000 + 8), ==, 0);
    g_assert_cmphex(qtest_readq(qts, w15 + 0x280), ==, 0);
    qtest_quit(qts);
}

/* A request sent to the 16550 reaches the L1; its reply returns in RBR. */
static void test_uart_reaches_the_l1(void)
{
    /* Request 0x1005 (display text) at index 5 with "Prom starting...". */
    static const uint8_t ack[] = { 0x7e, 0x25, 0, 0, 0, 0, 0, 0x68, 0x66,
                                   0x7e };
    uint8_t payload[32] = { 5, 0x0f, 0xff, 0xf0, 0x06, 0x10, 0x05, 1, 1 };
    uint8_t wire[64];
    unsigned len = 9;
    unsigned n;
    QTestState *qts = altix350_start();

    strcpy((char *)payload + len, "Prom starting...");
    len += strlen("Prom starting...") + 1;
    n = encode_frame(wire, payload, len);
    for (unsigned i = 0; i < n; i++) {
        qtest_writeb(qts, UART_GLOBAL, wire[i]);
    }
    for (unsigned i = 0; i < sizeof(ack); i++) {
        g_assert_cmphex(qtest_readb(qts, UART_GLOBAL + UART_LSR) & 1, ==, 1);
        g_assert_cmphex(qtest_readb(qts, UART_GLOBAL), ==, ack[i]);
    }
    g_assert_cmphex(qtest_readb(qts, UART_GLOBAL + UART_LSR) & 1, ==, 0);
    qtest_quit(qts);
}

/* The rest of the junk bus slot is still unmodelled and is refused. */
static void test_uart_slot_beyond_the_registers_is_refused(void)
{
    QTestState *qts = altix350_start();

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_readq(qts, UART_GLOBAL + 0x40);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_autofree char *directory = NULL;
    g_autofree uint8_t *data = g_malloc0(FLASH_SIZE);
    int ret;

    g_test_init(&argc, &argv, NULL);
    directory = g_dir_make_tmp("sgi-altix350-test-XXXXXX", NULL);
    g_assert_nonnull(directory);
    tmp_dir = directory;
    image_path = g_build_filename(directory, "address-pattern.bin", NULL);
    for (unsigned i = 0; i < 16; i++) {
        data[i] = i + 1;
        data[FLASH_SIZE - 16 + i] = 0xa0 + i;
    }
    g_assert_true(g_file_set_contents(image_path, (char *)data, FLASH_SIZE,
                                     NULL));

    qtest_add_func("/sgi-altix350/flash", test_flash);
    qtest_add_func("/sgi-altix350/fit-pointer-alias",
                   test_fit_pointer_through_the_flash_alias);
    qtest_add_func("/sgi-altix350/missing-bios", test_missing_bios);
    qtest_add_func("/sgi-altix350/wrong-cpu", test_wrong_cpu);
    qtest_add_func("/sgi-altix350/early-ram", test_early_ram);
    qtest_add_func("/sgi-altix350/cacheable-alias", test_cacheable_alias);
    qtest_add_func("/sgi-altix350/flash-write", test_flash_write);
    qtest_add_func("/sgi-altix350/flash-drive-boots", test_flash_drive_boots);
    qtest_add_func("/sgi-altix350/flash-drive-variables",
                   test_flash_drive_variables);
    qtest_add_func("/sgi-altix350/fetch-in-flash-command-mode",
                   test_fetch_with_flash_in_command_mode);
    qtest_add_func("/sgi-altix350/flash-program-reaches-running-code",
                   test_flash_program_reaches_running_code);
    qtest_add_func("/sgi-altix350/unknown-read", test_unknown_read);
    qtest_add_func("/sgi-altix350/prom-flash", test_prom_flash);
    qtest_add_func("/sgi-altix350/pic-ate-reset", test_pic_ate_reset);
    qtest_add_func("/sgi-altix350/uart/line-status", test_uart_line_status);
    qtest_add_func("/sgi-altix350/uart/reaches-l1", test_uart_reaches_the_l1);
    qtest_add_func("/sgi-altix350/uart/beyond-registers-is-refused",
                   test_uart_slot_beyond_the_registers_is_refused);
    qtest_add_func("/sgi-altix350/unknown-write", test_unknown_write);
    qtest_add_func("/sgi-altix350/shub/pio-write-status",
                   test_shub_pio_write_status);
    qtest_add_func("/sgi-altix350/shub/pio-write-error-self-test",
                   test_shub_pio_write_error_self_test);
    qtest_add_func("/sgi-altix350/shub/pio-write-status-alias-other",
                   test_shub_pio_write_status_alias_other_value);
    qtest_add_func("/sgi-altix350/shub/pio-write-status-instances",
                   test_shub_pio_write_status_instance_limit);
    qtest_add_func("/sgi-altix350/shub/led", test_shub_led);
    qtest_add_func("/sgi-altix350/shub/led-width", test_shub_led_wrong_width);
    qtest_add_func("/sgi-altix350/pic/interrupt-to-memory",
                   test_pic_interrupt_to_memory);
    qtest_add_func("/sgi-altix350/ioc4/time-of-day", test_ioc4_time_of_day);
    qtest_add_func("/sgi-altix350/ioc4/interrupt-reaches-pic",
                   test_ioc4_interrupt_reaches_pic);
    qtest_add_func("/sgi-altix350/pic/control-page-size-bit",
                   test_pic_control_page_size_bit);
    qtest_add_func("/sgi-altix350/nic/bcm5701", test_bcm5701_present);
    qtest_add_func("/sgi-altix350/nic/absent-without-nic",
                   test_bcm5701_absent_without_nic);
    qtest_add_func("/sgi-altix350/nic/address-identity",
                   test_nic_address_identity);
    qtest_add_func("/sgi-altix350/nic/identity-refusals",
                   test_nic_identity_refusals);
    qtest_add_func("/sgi-altix350/nic/mac-conflict", test_nic_mac_conflict);
    qtest_add_func("/sgi-altix350/ioc4/ata-identify", test_ioc4_ata_identify);
    qtest_add_func("/sgi-altix350/ioc4/atapi-identify",
                   test_ioc4_atapi_identify);
    qtest_add_func("/sgi-altix350/ioc4/ata-interrupt", test_ioc4_ata_interrupt);
    qtest_add_func("/sgi-altix350/ioc4/ata-empty-channel",
                   test_ioc4_ata_empty_channel);
    qtest_add_func("/sgi-altix350/ioc4/sio-cr", test_ioc4_sio_cr);
    qtest_add_func("/sgi-altix350/ioc4/int-out-square-wave",
                   test_ioc4_int_out_square_wave);
    qtest_add_func("/sgi-altix350/shub/scratch2-identity",
                   test_shub_scratch2_identity);
    qtest_add_func("/sgi-altix350/shub/privilege-vector",
                   test_shub_privilege_vector);
    qtest_add_func("/sgi-altix350/shub/privilege-vector-reset",
                   test_shub_privilege_vector_reset_unknown);
    qtest_add_func("/sgi-altix350/shub/comparators",
                   test_shub_comparators_reset_and_storage);
    qtest_add_func("/sgi-altix350/shub/comparator-enable",
                   test_shub_comparator_enable_storage);
    qtest_add_func("/sgi-altix350/shub/ipi", test_shub_ipi);
    qtest_add_func("/sgi-altix350/shub/ipi-type", test_shub_ipi_unmodelled);
    qtest_add_func("/sgi-altix350/shub/comparator-fires",
                   test_shub_comparator_fires);
    qtest_add_func("/sgi-altix350/shub/comparator-field-bits",
                   test_shub_comparator_bits_outside_fields);
    qtest_add_func("/sgi-altix350/shub/cam-control", test_shub_cam_control);
    qtest_add_func("/sgi-altix350/shub/unmodelled",
                   test_shub_unmodelled_is_refused);
    qtest_add_func("/sgi-altix350/shub/register-width",
                   test_shub_register_width);
    qtest_add_func("/sgi-altix350/shub/dqrp", test_shub_ipi_access_and_dqrp);
    qtest_add_func("/sgi-altix350/shub/ipi-access",
                   test_shub_ipi_access_reset_unknown);
    qtest_add_func("/sgi-altix350/shub/config-field-bits",
                   test_shub_config_bits_outside_fields);
    qtest_add_func("/sgi-altix350/shub/id-fields", test_shub_id_fields);
    qtest_add_func("/sgi-altix350/shub/global-window",
                   test_shub_global_window_follows_nasid);
    qtest_add_func("/sgi-altix350/local-memory-space",
                   test_local_memory_space);
    qtest_add_func("/sgi-altix350/local-memory-space-ends",
                   test_local_memory_space_ends);
    qtest_add_func("/sgi-altix350/shub/chip-id", test_shub_chip_id);
    qtest_add_func("/sgi-altix350/shub/election-lock",
                   test_shub_election_and_lock);
    qtest_add_func("/sgi-altix350/shub/election-has-one-winner",
                   test_shub_election_has_one_winner);
    qtest_add_func("/sgi-altix350/shub/second-lock", test_shub_second_lock);
    qtest_add_func("/sgi-altix350/shub/memory-controller-words",
                   test_shub_memory_controller_words);
    qtest_add_func("/sgi-altix350/shub/memory-bring-up",
                   test_shub_memory_bring_up_words);
    qtest_add_func("/sgi-altix350/shub/memory-test-engine",
                   test_shub_memory_test_engine);
    qtest_add_func("/sgi-altix350/shub/bte-notifies", test_shub_bte_notifies);
    qtest_add_func("/sgi-altix350/shub/bte-needs-its-addresses",
                   test_shub_bte_needs_its_addresses);
    qtest_add_func("/sgi-altix350/compat-hole-edges", test_compat_hole_edges);
    qtest_add_func("/sgi-altix350/shub/bte-zero-fill", test_shub_bte_zero_fill);
    qtest_add_func("/sgi-altix350/shub/bte-copy", test_shub_bte_copy);
    qtest_add_func("/sgi-altix350/compat-hole/reads-zero",
                   test_compat_hole_reads_zero);
    qtest_add_func("/sgi-altix350/shub/two-processors",
                   test_shub_two_processors);
    qtest_add_func("/sgi-altix350/shub/pi-credits", test_shub_pi_credits);
    qtest_add_func("/sgi-altix350/shub/llp-reset-en", test_shub_llp_reset_en);
    qtest_add_func("/sgi-altix350/uart/interrupt", test_uart_interrupt);
    qtest_add_func("/sgi-altix350/pic/inbound-dma", test_pic_inbound_dma);
    qtest_add_func("/sgi-altix350/pic/head0-isp12160",
                   test_pic_head0_isp12160);
    qtest_add_func("/sgi-altix350/pic/isp12160-cfg1", test_pic_isp12160_cfg1);
    qtest_add_func("/sgi-altix350/pic/direct64-lanes",
                   test_pic_direct64_dma_lanes);
    qtest_add_func("/sgi-altix350/isp12160/cfg1-sxp-select",
                   test_isp12160_cfg1_sxp_select);
    qtest_add_func("/sgi-altix350/shub/big-window", test_shub_big_window);
    qtest_add_func("/sgi-altix350/pic/ate-dma", test_pic_ate_dma);
    qtest_add_func("/sgi-altix350/pic/ate-dma-swap", test_pic_ate_dma_swap);
    qtest_add_func("/sgi-altix350/pic/ate-dma-16k-pages",
                   test_pic_ate_dma_16k_pages);
    qtest_add_func("/sgi-altix350/pic/dma-other-node-is-refused",
                   test_pic_dma_other_node_is_refused);
    qtest_add_func("/sgi-altix350/isp12160/interrupt",
                   test_isp12160_interrupt_reaches_pic);
    qtest_add_func("/sgi-altix350/nic/interrupt",
                   test_nic_interrupt_reaches_pic);
    qtest_add_func("/sgi-altix350/slot2/interrupt",
                   test_slot2_interrupt_reaches_pic);
    qtest_add_func("/sgi-altix350/pic/inbound-dma-refused",
                   test_pic_inbound_dma_refused);
    qtest_add_func("/sgi-altix350/shub/override-table-reset",
                   test_shub_override_table_reset);
    qtest_add_func("/sgi-altix350/shub/pi-error-mask-reset",
                   test_shub_pi_error_mask_reset);
    qtest_add_func("/sgi-altix350/pic/empty-slot-all-ones",
                   test_pic_empty_slot_reads_all_ones);
    qtest_add_func("/sgi-altix350/pic/config-byte-addressed",
                   test_pic_config_is_byte_addressed);
    qtest_add_func("/sgi-altix350/shub/xn-port-words", test_shub_xn_port_words);
    qtest_add_func("/sgi-altix350/shub/ii-init-accesses",
                   test_shub_ii_init_accesses);
    qtest_add_func("/sgi-altix350/shub/ni-ports-down", test_shub_ni_ports_down);
    qtest_add_func("/sgi-altix350/shub/interrupt-pairs",
                   test_shub_interrupt_pairs);
    qtest_add_func("/sgi-altix350/shub/routing-tables",
                   test_shub_routing_tables);
    qtest_add_func("/sgi-altix350/shub/prom-reset-path",
                   test_shub_prom_reset_path);
    qtest_add_func("/sgi-altix350/shub/rtc0",
                   test_shub_rtc0_bus_error_message);
    qtest_add_func("/sgi-altix350/shub/destinations",
                   test_shub_interrupt_destinations);
    qtest_add_func("/sgi-altix350/shub/ii-setup", test_shub_ii_setup);
    qtest_add_func("/sgi-altix350/shub/crb-message-control",
                   test_shub_crb_message_control);
    qtest_add_func("/sgi-altix350/shub/pic-interrupt-target",
                   test_shub_pic_interrupt_target);
    qtest_add_func("/sgi-altix350/pic/crossbow", test_pic_crossbow);
    qtest_add_func("/sgi-altix350/pic/interrupt",
                   test_pic_interrupt_reaches_the_processor);
    qtest_add_func("/sgi-altix350/pic/interrupt-target-pid",
                   test_pic_interrupt_follows_the_target_pid);
    qtest_add_func("/sgi-altix350/pic/interrupt-node",
                   test_pic_interrupt_node_field);
    qtest_add_func("/sgi-altix350/shub/probe-leaves-no-spaces",
                   test_shub_probe_leaves_no_address_spaces);
    qtest_add_func("/sgi-altix350/shub/firmware-words",
                   test_shub_firmware_words);
    qtest_add_func("/sgi-altix350/shub/slice", test_shub_slice_registers);
    qtest_add_func("/sgi-altix350/shub/write-only",
                   test_shub_write_only_register);
    qtest_add_func("/sgi-altix350/shub/rtc-stopped-at-reset",
                   test_shub_rtc_stopped_at_reset);
    qtest_add_func("/sgi-altix350/shub/rtc", test_shub_rtc_prom_sequence);
    qtest_add_func("/sgi-altix350/shub/rtc-period-change",
                   test_shub_rtc_period_change_keeps_count);
    qtest_add_func("/sgi-altix350/shub/rtc-wrap",
                   test_shub_rtc_wraps_at_55_bits);
    qtest_add_func("/sgi-altix350/shub/reset", test_shub_reset_restores_state);
    ret = g_test_run();
    unlink(image_path);
    for (unsigned i = 0; i < unimp_serial; i++) {
        g_autofree char *log = g_strdup_printf("%s/unimp-%u.log", directory, i);

        unlink(log);
    }
    rmdir(directory);
    g_free(image_path);
    return ret;
}
