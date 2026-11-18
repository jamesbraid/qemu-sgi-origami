/*
 * SPDX-License-Identifier: BSD-3-Clause
 * SHub 1 junk bus device mapping, on a fixture machine with a 16550.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define LOCAL 0x8000000000ULL
#define GLOBAL 0x800000000ULL       /* node 0 */
#define UART 0x7fed40000ULL
#define LINE_STATUS (UART + 0x28)   /* register 5, shifted left by 3 */
#define SCRATCH (UART + 0x38)       /* register 7 */

static char *tmp_dir;
static char *unimp_log;
static unsigned unimp_serial;
static unsigned unimp_seen;

/* One log file per machine; assert_unmodelled() reads it. */
static const char *unimp_log_next(void)
{
    g_free(unimp_log);
    unimp_log = g_strdup_printf("%s/unimp-%u.log", tmp_dir, unimp_serial++);
    unimp_seen = 0;
    return unimp_log;
}

static QTestState *start(void)
{
    QTestState *qts = qtest_initf("-machine x-shub1-test -d unimp -D %s",
                                  unimp_log_next());

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    return qts;
}

/* Wait up to ten seconds for the stop, so a missing one fails the test. */
static unsigned unimp_count(void)
{
    g_autofree char *contents = NULL;
    const char *p;
    unsigned n = 0;

    if (!g_file_get_contents(unimp_log, &contents, NULL, NULL)) {
        return 0;
    }
    for (p = contents; (p = strstr(p, "unsupported")); p++) {
        n++;
    }
    return n;
}

/* A refused access is logged and the VM keeps running. */
static void assert_unmodelled(QTestState *qts)
{
    QDict *reply;

    for (unsigned i = 0; i < 100 && unimp_count() <= unimp_seen; i++) {
        g_usleep(100000);
    }
    g_assert_cmpuint(unimp_count(), >, unimp_seen);
    unimp_seen = unimp_count();
    reply = qtest_qmp(qts, "{'execute': 'query-status'}");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(reply, "return"), "status"),
                    ==, "running");
    qobject_unref(reply);
}

/* Both windows reach the UART: transmitter empty, scratch register kept. */
static void test_uart_in_both_windows(void)
{
    QTestState *qts = start();

    g_assert_cmphex(qtest_readb(qts, LOCAL + LINE_STATUS), ==, 0x60);
    g_assert_cmphex(qtest_readq(qts, GLOBAL + LINE_STATUS) & 0xff, ==, 0x60);
    qtest_writeb(qts, LOCAL + SCRATCH, 0xa5);
    g_assert_cmphex(qtest_readb(qts, GLOBAL + SCRATCH), ==, 0xa5);
    qtest_quit(qts);
}

/* Past the UART's registers, and the next junk bus slot, are refused. */
static void test_other_junk_bus_offsets_are_refused(void)
{
    static const uint64_t offsets[] = { UART + 0x40, UART + 0x10000 };

    for (unsigned i = 0; i < ARRAY_SIZE(offsets); i++) {
        QTestState *qts = start();

        qtest_readq(qts, LOCAL + offsets[i]);
        assert_unmodelled(qts);
        qtest_quit(qts);
    }
}

#define FLASH 0x7ff800000ULL
#define WORD(w) ((w) * 2)           /* word address to byte offset */

static void flash_wr(QTestState *qts, uint64_t byte, uint16_t v)
{
    qtest_writew(qts, LOCAL + FLASH + byte, v);
}

static uint16_t flash_rd(QTestState *qts, uint64_t byte)
{
    return qtest_readw(qts, LOCAL + FLASH + byte);
}

static void flash_unlock(QTestState *qts)
{
    flash_wr(qts, WORD(0x555), 0xaa);
    flash_wr(qts, WORD(0x2aa), 0x55);
}

/* Erased flash reads all ones at 16 and 64 bits; the PROM log header too. */
static void test_flash_erased(void)
{
    QTestState *qts = start();

    g_assert_cmphex(flash_rd(qts, 0), ==, 0xffff);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readq(qts, LOCAL + FLASH + 8 * i), ==,
                        UINT64_MAX);
    }
    g_assert_cmphex(qtest_readq(qts, GLOBAL + FLASH), ==, UINT64_MAX);
    qtest_quit(qts);
}

/* Autoselect and the CFI query, as the PROM's probe issues them. */
static void test_flash_probe(void)
{
    QTestState *qts = start();

    flash_wr(qts, 0, 0xf0);
    flash_unlock(qts);
    flash_wr(qts, WORD(0x555), 0x90);
    g_assert_cmphex(flash_rd(qts, WORD(0)), ==, 0x0001);
    g_assert_cmphex(flash_rd(qts, WORD(1)), ==, 0x227e);
    flash_wr(qts, 0, 0xf0);
    flash_wr(qts, WORD(0x55), 0x98);
    g_assert_cmphex(flash_rd(qts, WORD(0x10)), ==, 0x51);
    g_assert_cmphex(flash_rd(qts, WORD(0x11)), ==, 0x52);
    g_assert_cmphex(flash_rd(qts, WORD(0x12)), ==, 0x59);
    g_assert_cmphex(flash_rd(qts, WORD(0x13)), ==, 2);
    g_assert_cmphex(flash_rd(qts, WORD(0x27)), ==, 23);
    g_assert_cmphex(flash_rd(qts, WORD(0x28)), ==, 2);
    g_assert_cmphex(flash_rd(qts, WORD(0x2c)), ==, 1);
    g_assert_cmphex(flash_rd(qts, WORD(0x2d)), ==, 127);
    flash_wr(qts, 0, 0xf0);
    g_assert_cmphex(flash_rd(qts, 0), ==, 0xffff);
    qtest_quit(qts);
}

/* Program and erase through the command cycles, seen through both windows. */
static void test_flash_program_erase(void)
{
    QTestState *qts = start();
    const uint64_t sector = 16 * 0x10000;       /* the variable store */

    flash_unlock(qts);
    flash_wr(qts, WORD(0x555), 0xa0);
    flash_wr(qts, sector + 0x20, 0x1234);
    qtest_clock_step(qts, 1000000000);
    flash_wr(qts, 0, 0xf0);
    g_assert_cmphex(flash_rd(qts, sector + 0x20), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, GLOBAL + FLASH + sector + 0x20), ==,
                    0x1234);
    flash_unlock(qts);
    flash_wr(qts, WORD(0x555), 0x80);
    flash_unlock(qts);
    flash_wr(qts, sector, 0x30);
    qtest_clock_step(qts, 4000000000ULL);
    flash_wr(qts, 0, 0xf0);
    g_assert_cmphex(flash_rd(qts, sector + 0x20), ==, 0xffff);
    qtest_quit(qts);
}

/* The flash takes 16-bit stores and 16 or 64-bit reads, nothing else. */
static void test_flash_other_widths_are_refused(void)
{
    for (unsigned i = 0; i < 3; i++) {
        QTestState *qts = start();

        switch (i) {
        case 0:
            qtest_writeq(qts, LOCAL + FLASH, 0xf0);
            break;
        case 1:
            qtest_readb(qts, LOCAL + FLASH);
            break;
        default:
            qtest_writel(qts, LOCAL + FLASH, 0xf0);
            break;
        }
        assert_unmodelled(qts);
        qtest_quit(qts);
    }
}

/* The XN compare words the PROM zeroes read 0 and keep a stored value. */
static void test_xn_zero_cells(void)
{
    static const uint64_t cells[] = {
        0x150031220ULL, 0x150031230ULL, 0x1500313c0ULL,
        0x1500313e0ULL, 0x1500315b0ULL, 0x1500315c0ULL,
        0x1500315a0ULL, 0x150031360ULL, 0x150031370ULL,
        0x150031270ULL, 0x150031110ULL, 0x150031610ULL, 0x150031580ULL,
        0x150031600ULL,
    };
    QTestState *qts = start();

    for (unsigned i = 0; i < ARRAY_SIZE(cells); i++) {
        g_assert_cmphex(qtest_readq(qts, LOCAL + cells[i]), ==, 0);
        qtest_writeq(qts, LOCAL + cells[i], 0);
        qtest_writeq(qts, LOCAL + cells[i], 0x5a + i);
        g_assert_cmphex(qtest_readq(qts, GLOBAL + cells[i]), ==, 0x5a + i);
    }
    qtest_quit(qts);
}

/* The error and mask words the PROM writes are stored; widths are refused. */
static void test_blind_cells(void)
{
    static const uint64_t cells[] = {
        0x150040508ULL, 0x1000000d8ULL, 0x100033040ULL, 0x110050480ULL,
        0x150040550ULL, 0x110040000ULL, 0x110040300ULL,
        0x110060380ULL, 0x110060100ULL, 0x100030110ULL, 0x100050370ULL,
        0x110060780ULL,
    };
    QTestState *qts = start();

    for (unsigned i = 0; i < ARRAY_SIZE(cells); i++) {
        g_assert_cmphex(qtest_readq(qts, LOCAL + cells[i]), ==, 0);
        qtest_writeq(qts, GLOBAL + cells[i], 0x7f77777ffULL + i);
        g_assert_cmphex(qtest_readq(qts, LOCAL + cells[i]), ==,
                        0x7f77777ffULL + i);
    }
    /* The PI error masks reset with every valid bit set. */
    g_assert_cmphex(qtest_readq(qts, LOCAL + 0x120050a80ULL), ==,
                    0x7ffffffffULL);
    g_assert_cmphex(qtest_readq(qts, LOCAL + 0x120050080ULL), ==,
                    0x7ffe00000ULL);
    g_assert_cmphex(qtest_readq(qts, LOCAL + 0x120050500ULL), ==, 0x1fffffULL);
    qtest_writeq(qts, GLOBAL + 0x120050a80ULL, 0);
    g_assert_cmphex(qtest_readq(qts, LOCAL + 0x120050a80ULL), ==, 0);
    /* The credit counts reset to 0x1f and the access words to all ones. */
    g_assert_cmphex(qtest_readq(qts, LOCAL + 0x110060000ULL), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts, LOCAL + 0x110060180ULL), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts, LOCAL + 0x110060400ULL), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts, LOCAL + 0x110040100ULL), ==, 0x1f);
    g_assert_cmphex(qtest_readq(qts, GLOBAL + 0x110040180ULL), ==, 0x1f);
    qtest_readl(qts, LOCAL + cells[0]);
    assert_unmodelled(qts);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    tmp_dir = g_dir_make_tmp("sgi-shub-test-XXXXXX", NULL);
    g_assert_nonnull(tmp_dir);
    qtest_add_func("/sgi-shub/junk-bus/uart", test_uart_in_both_windows);
    qtest_add_func("/sgi-shub/junk-bus/other-offsets-are-refused",
                   test_other_junk_bus_offsets_are_refused);
    qtest_add_func("/sgi-shub/prom-flash/erased", test_flash_erased);
    qtest_add_func("/sgi-shub/prom-flash/probe", test_flash_probe);
    qtest_add_func("/sgi-shub/prom-flash/program-erase",
                   test_flash_program_erase);
    qtest_add_func("/sgi-shub/prom-flash/other-widths-are-refused",
                   test_flash_other_widths_are_refused);
    qtest_add_func("/sgi-shub/xn-zero-cells", test_xn_zero_cells);
    qtest_add_func("/sgi-shub/blind-cells", test_blind_cells);
    ret = g_test_run();
    for (unsigned i = 0; i < unimp_serial; i++) {
        g_autofree char *log = g_strdup_printf("%s/unimp-%u.log", tmp_dir, i);

        unlink(log);
    }
    rmdir(tmp_dir);
    return ret;
}
