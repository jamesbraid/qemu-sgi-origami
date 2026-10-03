/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA qtests for what the X server sends after kon_InitHW: the video
 * command, token 0x1a6, with its command number, data words and checksum,
 * and the replies its display generator probe waits for on the return
 * page; the gamma load, 0x12f, the probe makes between them; and the X
 * tokens after the probe, up to the root-window fill. Each case is a
 * discriminating test for the behaviour its comment names.
 *
 * The fixture is sn0-origin2000-kona-bringup-test.c's: KONA as a -device
 * at the empty slot IO4, crossbow0's widget 11, with fabric-hub-widget=9
 * so the board's own fabric writes reach node 0's Hub and its memory.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

#define SMALL_WINDOW_SIZE UINT64_C(0x1000000)
#define KONA_WIDGET 11
#define KONA_BASE   (SPACE_IO + (uint64_t)KONA_WIDGET * SMALL_WINDOW_SIZE)
#define KONA_REG(offset) (KONA_BASE + (offset))

#define REG_INT_CAUSE       KONA_REG(0x800238)
#define REG_MAILBOX         KONA_REG(0x800240)
#define REG_KICK            KONA_REG(0xc00108)
#define REG_COMMAND         KONA_REG(0xc10000)
#define REG_COMMAND_DATA    KONA_REG(0xc1000c)
#define REG_MAPRAM(i)       KONA_REG(0x100000 + 4 * (uint64_t)(i))
/* The slot store that opens token t: offset 0xc + 0x20 * t. */
#define REG_COMMAND_SLOT(t) KONA_REG(0xc1000c + 0x20 * (uint64_t)(t))

#define INT_CAUSE_MAILBOX   0x10

#define TOKEN_VIDEO         0x1a6

/* Video command numbers, from the X server's video command table. */
#define GET_BOARD_CONFIG    0x12
#define GET_CHANNEL_CONFIG  0x13
#define X_SERVER_VIDEO_INIT 0x53

/* A node 0 guest RAM page for the X server context's return page. */
#define RETURN_PAGE         UINT64_C(0x320000)

/* The gamma table: three components of 0x1000 entries, c at c * 0x1000. */
#define GAMMA_COMPONENT     0x1000
#define GAMMA_ENTRIES       (3 * GAMMA_COMPONENT)

#define KONA_QOM_PATH "/machine/module0/kona0"

#define MACHINE_ARGS \
    "-M origin2000,nodes=1 -smp 1 -m 64M " \
    "-device sgi-kona,id=kona0,slot=io4," \
    "fabric-hub-widget=9"

/* The one-node machine is small enough to migrate quickly as it is. */
#define MIGRATION_ARGS MACHINE_ARGS

/*
 * The instance the cases that need no trace of their own share; each
 * resets it first.
 */
static QTestState *qts;

static void kona_reset(void)
{
    qtest_system_reset(qts);
}

/*
 * A fresh instance tracing every sgi_kona event and logging the -d
 * categories in `debug` to its own file. The caller quits it and reads
 * the log with kona_log_read(), which also removes the file.
 */
static QTestState *kona_init_logged(const char *base_args, const char *debug,
                                    char **log)
{
    int fd;

    *log = g_strdup_printf("%s/sn0-kona-video-XXXXXX.log", g_get_tmp_dir());
    fd = g_mkstemp(*log);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    return qtest_initf("%s -trace enable=sgi_kona_* -d %s -D %s", base_args,
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

static void guest_fill(QTestState *s, uint64_t addr, uint8_t byte, size_t len)
{
    g_autofree uint8_t *bytes = g_malloc(len);

    memset(bytes, byte, len);
    qtest_memwrite(s, addr, bytes, len);
}

/* The X server context's 0x1f2 for a host physical return page. */
static void kona_register_return_page(QTestState *s, uint64_t page)
{
    qtest_writel(s, REG_KICK, 0x30000);
    qtest_writel(s, REG_COMMAND, 0x1f2);
    qtest_writel(s, REG_KICK, 0x10000);
    qtest_writel(s, REG_COMMAND, 0x10000000 | (uint32_t)(page >> 12));
    qtest_writel(s, REG_COMMAND, 0x9000 | (uint32_t)(page & 0xfff));
    qtest_writel(s, REG_COMMAND, 0x4000);
    qtest_writel(s, REG_KICK, 0);
}

/* One command on the +0xc10000 port, as plain words, token first. */
static void kona_command(QTestState *s, uint32_t token, const uint32_t *args,
                         unsigned argc)
{
    qtest_writel(s, REG_COMMAND, token);
    for (unsigned i = 0; i < argc; i++) {
        qtest_writel(s, REG_COMMAND, args[i]);
    }
}

/*
 * sn0-origin2000-kona-test.c's own kona_qom_get_uint(): no shared header
 * carries it, and libqtest.h has no generic uint getter.
 */
static uint64_t kona_qom_get_uint(QTestState *s, const char *property)
{
    QDict *resp = qtest_qmp(s,
        "{'execute': 'qom-get', 'arguments': {'path': %s, "
        "'property': %s}}", KONA_QOM_PATH, property);
    uint64_t value;

    if (!qdict_haskey(resp, "return")) {
        g_autoptr(GString) str = qobject_to_json(QOBJECT(resp));
        g_error("qom-get %s failed: %s", property, str->str);
    }
    value = qdict_get_int(resp, "return");
    qobject_unref(resp);
    return value;
}

/* The gamma table entry at gamma-RAM address `addr`. */
static uint32_t kona_gamma(QTestState *s, uint32_t addr)
{
    qtest_qmp_assert_success(s,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-gamma-addr', 'value': %u}}", KONA_QOM_PATH,
        addr);
    return kona_qom_get_uint(s, "x-test-gamma");
}

/*
 * One video command as kona.so's sender stores it: the number at the
 * token's slot, then each data word and the checksum at the data slot.
 */
static void video_command(QTestState *s, uint32_t command,
                          const uint32_t *data, unsigned count,
                          uint32_t checksum)
{
    qtest_writel(s, REG_COMMAND_SLOT(TOKEN_VIDEO), command);
    for (unsigned i = 0; i < count; i++) {
        qtest_writel(s, REG_COMMAND_DATA, data[i]);
    }
    qtest_writel(s, REG_COMMAND_DATA, checksum);
}

/*
 * The PROM's sync probe, 1 9 0: its mailbox reply shows the command
 * page's framing is back in step after whatever came before it.
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

/*
 * Framing: 0x12
 * at +0xc134cc, then 0 and 0x12 at +0xc1000c, frame as one command 0x12
 * with one data word and a checksum that verifies. The same command sent
 * as plain words at +0xc10000 frames the same way. A wrong checksum is
 * traced, and the words after it still frame from a clean start.
 */
static void test_framing_and_checksum(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "guest_errors,unimp",
                                     &log);

    qtest_writel(s, KONA_REG(0xc134cc), GET_BOARD_CONFIG);
    qtest_writel(s, REG_COMMAND_DATA, 0);
    qtest_writel(s, REG_COMMAND_DATA, GET_BOARD_CONFIG);
    assert_textport_in_step(s);

    qtest_writel(s, REG_COMMAND, TOKEN_VIDEO);
    qtest_writel(s, REG_COMMAND, GET_CHANNEL_CONFIG);
    qtest_writel(s, REG_COMMAND, 2);
    qtest_writel(s, REG_COMMAND, GET_CHANNEL_CONFIG + 2);
    assert_textport_in_step(s);

    video_command(s, X_SERVER_VIDEO_INIT, (uint32_t[]){ 0 }, 1, 0x99);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "command 0x12 GetBoardConfig data 0x0 "
                                   "checksum 0x12"));
    g_assert_nonnull(strstr(found, "command 0x13 GetChannelConfig data 0x2 "
                                   "checksum 0x15"));
    g_assert_nonnull(strstr(found, "command 0x53 XServerVideoInit data 0x0 "
                                   "checksum 0x99"));
    g_assert_cmpuint(count_occurrences(found,
                                       "sgi_kona_video_checksum_mismatch "),
                     ==, 1);
    g_assert_nonnull(strstr(found, "command 0x53 checksum 0x99 expected "
                                   "0x53; acted on anyway"));
    g_assert_null(strstr(found, "sync word"));
    g_assert_null(strstr(found, "unknown token"));
}

/*
 * XServerVideoInit is consumed whole and posts nothing: the return page
 * is untouched, no host write is made, and the next command frames. So
 * is konBlankScreen's TempBlankAllChannels (0x3c), sent right after the
 * probe with data 0 and no reply, and it is named
 * rather than logged as unmodelled.
 */
static void test_x_server_video_init_no_reply(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    g_autofree uint8_t *before = g_malloc(0x40);
    g_autofree uint8_t *after = g_malloc(0x40);
    QTestState *s = kona_init_logged(MACHINE_ARGS, "guest_errors,unimp",
                                     &log);

    guest_fill(s, RETURN_PAGE, 0x5a, 0x40);
    kona_register_return_page(s, RETURN_PAGE);
    qtest_memread(s, RETURN_PAGE, before, 0x40);
    video_command(s, X_SERVER_VIDEO_INIT, (uint32_t[]){ 0 }, 1,
                  X_SERVER_VIDEO_INIT);
    qtest_writel(s, REG_COMMAND, TOKEN_VIDEO);
    qtest_writel(s, REG_COMMAND, 0x3c);
    qtest_writel(s, REG_COMMAND, 0);
    qtest_writel(s, REG_COMMAND, 0x3c);
    qtest_memread(s, RETURN_PAGE, after, 0x40);
    g_assert_cmpmem(before, 0x40, after, 0x40);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "command 0x53 XServerVideoInit data 0x0 "
                                   "checksum 0x53"));
    g_assert_nonnull(strstr(found, "command 0x3c TempBlankAllChannels data "
                                   "0x0 checksum 0x3c"));
    g_assert_null(strstr(found, "unmodelled video command"));
    g_assert_null(strstr(found, "sgi_kona_host_write "));
    g_assert_null(strstr(found, "sgi_kona_video_checksum_mismatch "));
}

/*
 * A command number past the table (0x5d; the table ends at 0x5c) has no
 * known length, so it is framed by its checksum, is logged as unmodelled
 * and posts nothing. The data word is shaped like a token, so a model
 * that consumed fewer words would frame it as a command of its own.
 */
static void test_unknown_video_command(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    g_autofree uint8_t *before = g_malloc(0x40);
    g_autofree uint8_t *after = g_malloc(0x40);
    QTestState *s = kona_init_logged(MACHINE_ARGS, "guest_errors,unimp",
                                     &log);

    guest_fill(s, RETURN_PAGE, 0x5a, 0x40);
    kona_register_return_page(s, RETURN_PAGE);
    qtest_memread(s, RETURN_PAGE, before, 0x40);
    video_command(s, 0x5d, (uint32_t[]){ 0x1e9 }, 1, 0x5d + 0x1e9);
    qtest_writel(s, REG_COMMAND_DATA, 0);
    qtest_memread(s, RETURN_PAGE, after, 0x40);
    g_assert_cmpmem(before, 0x40, after, 0x40);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "unmodelled video command 0x5d (data "
                                   "0x1e9); consumed, nothing posted"));
    g_assert_nonnull(strstr(found, "command 0x5d unknown data 0x1e9 "
                                   "checksum 0x246"));
    g_assert_null(strstr(found, "sgi_kona_host_write "));
    g_assert_null(strstr(found, "sgi_kona_video_checksum_mismatch "));
    /* The trailing 0 was an unexpected word, not a 0x1e9's argument. */
    g_assert_nonnull(strstr(found, "textport expected a sync word, got "
                                   "0x0\n"));
}

/*
 * An address-encoded store while a video command is still open is the
 * one framing error the stores make detectable: it is logged, the
 * partial command is dropped and the new one framed.
 */
static void test_slot_store_while_open(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "guest_errors,unimp",
                                     &log);

    qtest_writel(s, REG_COMMAND_SLOT(TOKEN_VIDEO), GET_BOARD_CONFIG);
    qtest_writel(s, REG_COMMAND_SLOT(0x1ff), 0xdeadbeef);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "token 0x1ff stored at command page "
                                   "offset 0x3fec while video command 0x12 "
                                   "still needs 2 words; partial command "
                                   "dropped"));
    g_assert_nonnull(strstr(found, "token 0x1ff args 0xdeadbeef"));
    g_assert_null(strstr(found, "sgi_kona_video_command "));
}

/*
 * Reset drops a video command still collecting its words: the next
 * command frames from its own number, and the one left open never
 * completes.
 */
static void test_reset_clears_open_command(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "guest_errors,unimp",
                                     &log);

    qtest_writel(s, REG_COMMAND, TOKEN_VIDEO);
    qtest_writel(s, REG_COMMAND, GET_BOARD_CONFIG);
    qtest_system_reset(s);
    video_command(s, GET_CHANNEL_CONFIG, (uint32_t[]){ 1 }, 1,
                  GET_CHANNEL_CONFIG + 1);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "command 0x13 GetChannelConfig data 0x1 "
                                   "checksum 0x14"));
    g_assert_null(strstr(found, "command 0x12 "));
    g_assert_null(strstr(found, "sgi_kona_video_checksum_mismatch "));
}

/*
 * A video command still collecting its words at the migration point
 * completes on the destination.
 */
static void test_migration_mid_video_command(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-video-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = kona_init_logged(dest_args, "guest_errors,unimp", &log);

    qtest_writel(from, REG_COMMAND_SLOT(TOKEN_VIDEO), GET_CHANNEL_CONFIG);
    qtest_writel(from, REG_COMMAND_DATA, 4);

    sgi_migrate_stopped(from, uri);

    qtest_writel(to, REG_COMMAND_DATA, GET_CHANNEL_CONFIG + 4);
    assert_textport_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "command 0x13 GetChannelConfig data 0x4 "
                                   "checksum 0x17"));
    g_assert_null(strstr(found, "sgi_kona_video_checksum_mismatch "));
    g_assert_null(strstr(found, "sync word"));
}

/*
 * The video reply placement test: after a physical
 * 0x1f2 page, GetBoardConfig leaves page words 2 to 8 as 6, the five
 * data words (version 1, board mask 1, option mask 0, options code 0,
 * flag 0) and their sum plus 6, and writes word 0 last. Word 1 and the
 * word after the checksum are left alone. A wrong checksum on the
 * command still gets the reply. With no return page, nothing is posted.
 */
static void test_board_config_reply(void)
{
    static const uint32_t expected[] = { 6, 1, 1, 0, 0, 0, 8 };
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    const char *reply_write, *ready_write;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "guest_errors,unimp",
                                     &log);

    guest_fill(s, RETURN_PAGE, 0x5a, 0x40);
    video_command(s, GET_BOARD_CONFIG, (uint32_t[]){ 0 }, 1,
                  GET_BOARD_CONFIG);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 0x5a5a5a5a);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, 0x5a5a5a5a);

    guest_fill_word(s, RETURN_PAGE, 0);
    kona_register_return_page(s, RETURN_PAGE);
    video_command(s, GET_BOARD_CONFIG, (uint32_t[]){ 0 }, 1,
                  GET_BOARD_CONFIG);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 1);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 4), ==, 0x5a5a5a5a);
    for (size_t i = 0; i < ARRAY_SIZE(expected); i++) {
        g_assert_cmphex(guest_word(s, RETURN_PAGE + 8 + 4 * i), ==,
                        expected[i]);
    }
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 36), ==, 0x5a5a5a5a);

    /* The reply wait's own flush frames and changes nothing. */
    guest_fill_word(s, RETURN_PAGE, 0);
    qtest_writel(s, REG_COMMAND_SLOT(0x1ff), 0xdeadbeef);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 0);

    /* A bad checksum still gets its reply. */
    guest_fill(s, RETURN_PAGE, 0x5a, 0x40);
    video_command(s, GET_BOARD_CONFIG, (uint32_t[]){ 0 }, 1, 0x1234);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 1);
    for (size_t i = 0; i < ARRAY_SIZE(expected); i++) {
        g_assert_cmphex(guest_word(s, RETURN_PAGE + 8 + 4 * i), ==,
                        expected[i]);
    }
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "token 0x1a6 with no return page "
                                   "registered"));
    g_assert_cmpuint(count_occurrences(found, "command 0x12 reply id 6 data "
                                       "bytes 20"), ==, 3);
    g_assert_nonnull(strstr(found, "command 0x12 checksum 0x1234 expected "
                                   "0x12; acted on anyway"));
    reply_write = strstr(found, "addr 0x320008 len 0x1c");
    g_assert_nonnull(reply_write);
    ready_write = strstr(reply_write, "addr 0x320000 len 0x4");
    g_assert_nonnull(ready_write);
}

/*
 * GetChannelConfig answers id 7, a first data word of 1 (channel
 * present), eighteen zero words and the checksum 8, overwriting what an
 * earlier answer left in words 2 onward.
 */
static void test_channel_config_reply(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);

    guest_fill(s, RETURN_PAGE, 0x5a, 0x80);
    guest_fill_word(s, RETURN_PAGE, 0);
    kona_register_return_page(s, RETURN_PAGE);
    video_command(s, GET_CHANNEL_CONFIG, (uint32_t[]){ 0 }, 1,
                  GET_CHANNEL_CONFIG);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 1);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, 7);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 12), ==, 1);
    for (unsigned w = 1; w < 19; w++) {
        g_assert_cmphex(guest_word(s, RETURN_PAGE + 12 + 4 * w), ==, 0);
    }
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 12 + 4 * 19), ==, 8);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 12 + 4 * 20), ==,
                    0x5a5a5a5a);
    qtest_quit(s);
}

/*
 * Every other Get answers its reply id, zero data at its reply size and
 * a checksum equal to the id, then word 0. GetOutputPedestal is the
 * smallest (one word); the rest follow the model's video command table.
 */
static void test_zero_data_replies(void)
{
    static const struct {
        uint32_t command;
        uint32_t id;
        uint32_t bytes;
    } gets[] = {
        { 0x26, 12, 4 }, { 0x15, 8, 72 }, { 0x20, 9, 4 }, { 0x3a, 16, 24 },
        { 0x45, 20, 4 }, { 0x4a, 23, 20 }, { 0x4c, 24, 16 }, { 0x55, 28, 4 },
    };
    QTestState *s = qtest_init(MACHINE_ARGS);

    kona_register_return_page(s, RETURN_PAGE);
    for (size_t i = 0; i < ARRAY_SIZE(gets); i++) {
        uint32_t words = gets[i].bytes / 4;

        guest_fill(s, RETURN_PAGE, 0x5a, 0x80);
        guest_fill_word(s, RETURN_PAGE, 0);
        video_command(s, gets[i].command, (uint32_t[]){ 0xffffffff }, 1,
                      gets[i].command - 1);
        g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 1);
        g_assert_cmphex(guest_word(s, RETURN_PAGE + 4), ==, 0x5a5a5a5a);
        g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, gets[i].id);
        for (unsigned w = 0; w < words; w++) {
            g_assert_cmphex(guest_word(s, RETURN_PAGE + 12 + 4 * w), ==, 0);
        }
        g_assert_cmphex(guest_word(s, RETURN_PAGE + 12 + 4 * words), ==,
                        gets[i].id);
        g_assert_cmphex(guest_word(s, RETURN_PAGE + 16 + 4 * words), ==,
                        0x5a5a5a5a);
    }
    assert_textport_in_step(s);
    qtest_quit(s);
}

/*
 * A version 4 stream predates the video framing state. Loading one
 * leaves no video command open and still carries the version 4 state (a
 * map RAM entry and the return page here). Skipped unless
 * QTEST_QEMU_BINARY_KONA_V4 names a qemu-system-mips64 built before the
 * version 5 fields existed, the pattern sn0-origin2000-kona-bringup-test.c
 * uses for version 3.
 */
static void test_migration_from_v4(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    if (!g_getenv("QTEST_QEMU_BINARY_KONA_V4")) {
        g_test_skip("QTEST_QEMU_BINARY_KONA_V4 is not set");
        return;
    }

    tmpfs = g_dir_make_tmp("sn0-kona-v4-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init_ext("QTEST_QEMU_BINARY_KONA_V4", MIGRATION_ARGS, NULL,
                          true);
    qtest_writel(from, REG_MAPRAM(7), 0x20001234);
    kona_register_return_page(from, RETURN_PAGE);

    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);
    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(qtest_readl(to, REG_MAPRAM(7)), ==, 0x20001234);
    /* No video command is open: a sync probe frames at once. */
    assert_textport_in_step(to);
    /* The return page crossed: 0x1e9 answers there. */
    guest_fill_word(to, RETURN_PAGE, 0);
    qtest_writel(to, REG_COMMAND, 0x1e9);
    qtest_writel(to, REG_COMMAND, 0);
    g_assert_cmphex(guest_word(to, RETURN_PAGE), ==, 1);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * The gamma load, 0x12f: a start address, a count, then that many
 * entries. Sent as plain words, start 0x1000 (the second component) and
 * count 3, whose entries are shaped like 0x1e9 and its argument: a model
 * that did not frame the load would answer a 0x1e9 on the return page.
 * The entries land at 0x1000 to 0x1002 and nowhere else. Sent again with
 * the start in the token's slot at 0x2ffe, the last two entries of the
 * table take the first two words, and the third, a 1 past the table, is
 * consumed rather than read as the sync word of a probe that the next
 * two words, 9 and 0, would complete.
 */
static void test_gamma_load_small(void)
{
    kona_reset();
    kona_register_return_page(qts, RETURN_PAGE);
    guest_fill_word(qts, RETURN_PAGE, 0);
    kona_command(qts, 0x12f, (uint32_t[]){ 0x1000, 3, 0x1e9, 0, 0xfff }, 5);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE), ==, 0);
    g_assert_cmphex(kona_gamma(qts, 0x0fff), ==, 0);
    g_assert_cmphex(kona_gamma(qts, 0x1000), ==, 0x1e9);
    g_assert_cmphex(kona_gamma(qts, 0x1001), ==, 0);
    g_assert_cmphex(kona_gamma(qts, 0x1002), ==, 0xfff);
    g_assert_cmphex(kona_gamma(qts, 0x1003), ==, 0);
    g_assert_cmphex(kona_gamma(qts, 0x0000), ==, 0);
    g_assert_cmphex(kona_gamma(qts, 0x2000), ==, 0);
    assert_textport_in_step(qts);

    qtest_writel(qts, REG_COMMAND_SLOT(0x12f), GAMMA_ENTRIES - 2);
    qtest_writel(qts, REG_COMMAND_DATA, 3);
    qtest_writel(qts, REG_COMMAND_DATA, 0x111);
    qtest_writel(qts, REG_COMMAND_DATA, 0x222);
    qtest_writel(qts, REG_COMMAND_DATA, 1);
    qtest_writel(qts, REG_COMMAND_DATA, 9);
    qtest_writel(qts, REG_COMMAND_DATA, 0);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, 0);
    g_assert_cmphex(kona_gamma(qts, GAMMA_ENTRIES - 2), ==, 0x111);
    g_assert_cmphex(kona_gamma(qts, GAMMA_ENTRIES - 1), ==, 0x222);
    kona_command(qts, 0x1e9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE), ==, 1);
    assert_textport_in_step(qts);
}

/*
 * A whole component, as the stock X server sends it: start 0, count
 * 0x1000, and entries rising from 0 to 0xfff, which the grammar records
 * from a model run. Among them are 0x128, 0x1a6, 0x1dd, 0x1e9 and 0x1f2,
 * which that run misread as commands. Here every entry lands in the
 * first component, nothing answers on the return page, and the command
 * page is in step afterwards.
 */
static void test_gamma_load_full(void)
{
    static const uint32_t probes[] = {
        0, 1, 0x128, 0x1a6, 0x1dd, 0x1e9, 0x1f2, 0xffe, 0xfff,
    };

    kona_reset();
    kona_register_return_page(qts, RETURN_PAGE);
    guest_fill_word(qts, RETURN_PAGE, 0);
    qtest_writel(qts, REG_COMMAND, 0x12f);
    qtest_writel(qts, REG_COMMAND, 0);
    qtest_writel(qts, REG_COMMAND, GAMMA_COMPONENT);
    for (uint32_t i = 0; i < GAMMA_COMPONENT; i++) {
        qtest_writel(qts, REG_COMMAND, i);
    }
    g_assert_cmphex(guest_word(qts, RETURN_PAGE), ==, 0);
    for (size_t i = 0; i < ARRAY_SIZE(probes); i++) {
        g_assert_cmphex(kona_gamma(qts, probes[i]), ==, probes[i]);
    }
    g_assert_cmphex(kona_gamma(qts, GAMMA_COMPONENT), ==, 0);
    assert_textport_in_step(qts);
    kona_command(qts, 0x1e9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE), ==, 1);
}

/*
 * The misread this framing ends: an entry equal to 0x1a6 inside a gamma
 * load, followed by entries shaped like GetBoardConfig's number, data
 * and checksum, is stored as entries and opens no video command, so
 * nothing answers on the return page.
 */
static void test_gamma_entry_not_video(void)
{
    kona_reset();
    kona_register_return_page(qts, RETURN_PAGE);
    guest_fill_word(qts, RETURN_PAGE, 0);
    kona_command(qts, 0x12f,
                 (uint32_t[]){ 0x2000, 4, 0x1a6, 0x12, 0, 0x12 }, 6);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE), ==, 0);
    g_assert_cmphex(kona_gamma(qts, 0x2000), ==, 0x1a6);
    g_assert_cmphex(kona_gamma(qts, 0x2001), ==, 0x12);
    g_assert_cmphex(kona_gamma(qts, 0x2003), ==, 0x12);
    assert_textport_in_step(qts);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE), ==, 0);
}

/*
 * Reset empties the gamma table and drops a load still taking its
 * entries: the words after the reset frame from a clean start.
 */
static void test_reset_clears_gamma(void)
{
    kona_reset();
    kona_command(qts, 0x12f, (uint32_t[]){ 0, 4, 0x55, 0x66 }, 4);
    g_assert_cmphex(kona_gamma(qts, 1), ==, 0x66);
    kona_reset();
    g_assert_cmphex(kona_gamma(qts, 0), ==, 0);
    g_assert_cmphex(kona_gamma(qts, 1), ==, 0);
    assert_textport_in_step(qts);
    g_assert_cmphex(kona_gamma(qts, 2), ==, 0);
}

/*
 * A gamma load still taking its entries at the migration point completes
 * on the destination, and the entries stored before it crossed.
 */
static void test_migration_mid_gamma_load(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-gamma-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    kona_register_return_page(from, RETURN_PAGE);
    guest_fill_word(from, RETURN_PAGE, 0);
    kona_command(from, 0x12f, (uint32_t[]){ 0x1000, 4, 7, 8 }, 4);

    sgi_migrate_stopped(from, uri);

    qtest_writel(to, REG_COMMAND, 0x1e9);
    qtest_writel(to, REG_COMMAND, 0);
    g_assert_cmphex(guest_word(to, RETURN_PAGE), ==, 0);
    g_assert_cmphex(kona_gamma(to, 0x1000), ==, 7);
    g_assert_cmphex(kona_gamma(to, 0x1001), ==, 8);
    g_assert_cmphex(kona_gamma(to, 0x1002), ==, 0x1e9);
    g_assert_cmphex(kona_gamma(to, 0x1003), ==, 0);
    assert_textport_in_step(to);
    kona_command(to, 0x1e9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(to, RETURN_PAGE), ==, 1);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * A version 5 stream predates the gamma table. Loading one leaves the
 * table empty and no load open, and still carries the version 5 state
 * (the return page here). Skipped unless QTEST_QEMU_BINARY_KONA_V5 names
 * a qemu-system-mips64 built before the version 6 fields existed.
 */
static void test_migration_from_v5(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    if (!g_getenv("QTEST_QEMU_BINARY_KONA_V5")) {
        g_test_skip("QTEST_QEMU_BINARY_KONA_V5 is not set");
        return;
    }

    tmpfs = g_dir_make_tmp("sn0-kona-v5-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init_ext("QTEST_QEMU_BINARY_KONA_V5", MIGRATION_ARGS, NULL,
                          true);
    kona_register_return_page(from, RETURN_PAGE);
    qtest_writel(from, REG_MAPRAM(7), 0x20001234);

    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);
    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(qtest_readl(to, REG_MAPRAM(7)), ==, 0x20001234);
    g_assert_cmphex(kona_gamma(to, 0), ==, 0);
    /* No gamma load is open: a sync probe frames at once. */
    assert_textport_in_step(to);
    guest_fill_word(to, RETURN_PAGE, 0);
    kona_command(to, 0x1e9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(to, RETURN_PAGE), ==, 1);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * Words shaped to catch a misframed X token: 1 9 0 is the textport's
 * sync probe, which queues a mailbox reply, and 0x1a6 and 0x1e9 open a
 * video command and a configuration request.
 */
static uint32_t x_token_filler(unsigned i)
{
    static const uint32_t pattern[] = { 1, 9, 0, 0x1a6, 0x1e9, 0 };

    return pattern[i % ARRAY_SIZE(pattern)];
}

/*
 * The X tokens the server sends between its video probe and the first
 * drawing command, and the root-window fill's, each with the word count
 * its sender uses: load GC 0x129 (13), load clip 0x12b (5), load
 * display-ID configuration 0x133 (1), X box 0x154 (2), X box6 0x155 (12)
 * and the cursor load 0x130 (261), sent as plain words and with its slot
 * word in the token's slot. Each is consumed whole: the filler words
 * queue no mailbox reply, post nothing on the return page and open no
 * video command, and the page is in step afterwards.
 */
static void test_x_tokens_consumed(void)
{
    static const struct {
        uint32_t token;
        unsigned words;
    } tokens[] = {
        { 0x129, 13 }, { 0x12b, 5 }, { 0x133, 1 }, { 0x154, 2 },
        { 0x155, 12 }, { 0x130, 261 },
    };
    g_autofree uint8_t *before = g_malloc(0x40);
    g_autofree uint8_t *after = g_malloc(0x40);

    kona_reset();
    guest_fill(qts, RETURN_PAGE, 0x5a, 0x40);
    kona_register_return_page(qts, RETURN_PAGE);
    qtest_memread(qts, RETURN_PAGE, before, 0x40);
    for (size_t i = 0; i < ARRAY_SIZE(tokens); i++) {
        qtest_writel(qts, REG_COMMAND, tokens[i].token);
        for (unsigned w = 0; w < tokens[i].words; w++) {
            qtest_writel(qts, REG_COMMAND, x_token_filler(w));
        }
        g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, 0);
    }
    qtest_writel(qts, REG_COMMAND_SLOT(0x130), 0);
    for (unsigned w = 1; w < 261; w++) {
        qtest_writel(qts, REG_COMMAND_DATA, x_token_filler(w));
    }
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, 0);
    qtest_memread(qts, RETURN_PAGE, after, 0x40);
    g_assert_cmpmem(before, 0x40, after, 0x40);
    assert_textport_in_step(qts);
}

/*
 * The second cursor sender's load, slot 0x2698 (38 times 260) in the
 * run, frames as 261 words like kon_cursorLogo's slot 0 and traces its
 * slot: the 260 data words, all sync probes, queue no mailbox reply, and
 * the page is in step afterwards.
 */
static void test_cursor_load_nonzero_slot(void)
{
    static const uint32_t probe[] = { 1, 9, 0 };
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "guest_errors,unimp",
                                     &log);

    qtest_writel(s, REG_COMMAND_SLOT(0x130), 0x2698);
    for (unsigned w = 1; w < 261; w++) {
        qtest_writel(s, REG_COMMAND_DATA, probe[w % 3]);
    }
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, 0);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "sgi_kona_x_cursor slot 0x2698"));
    g_assert_nonnull(strstr(found, "token 0x130 argc 261 consumed"));
    g_assert_null(strstr(found, "sync word"));
}

/*
 * Reset drops a cursor load still taking its data: the sync probe after
 * it frames at once rather than being consumed as cursor data.
 */
static void test_reset_clears_cursor_load(void)
{
    kona_reset();
    qtest_writel(qts, REG_COMMAND, 0x130);
    qtest_writel(qts, REG_COMMAND, 0);
    for (unsigned w = 0; w < 10; w++) {
        qtest_writel(qts, REG_COMMAND, w);
    }
    kona_reset();
    assert_textport_in_step(qts);
}

/*
 * A cursor load still taking its data at the migration point completes
 * on the destination: its data, all sync probes, queues no mailbox reply
 * on either side.
 */
static void test_migration_mid_cursor_load(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    static const uint32_t probe[] = { 1, 9, 0 };
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-cursor-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    qtest_writel(from, REG_COMMAND_SLOT(0x130), 0);
    for (unsigned w = 1; w < 100; w++) {
        qtest_writel(from, REG_COMMAND_DATA, probe[w % 3]);
    }

    g_assert_cmphex(qtest_readl(from, REG_INT_CAUSE), ==, 0);
    sgi_migrate_stopped(from, uri);

    for (unsigned w = 100; w < 261; w++) {
        qtest_writel(to, REG_COMMAND_DATA, probe[w % 3]);
    }
    g_assert_cmphex(qtest_readl(to, REG_INT_CAUSE), ==, 0);
    assert_textport_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * The probe's last wait, GetDGInterruptState (0x3d), answers reply id 17
 * with three zero data words and the checksum 17, which ends that wait.
 */
static void test_dg_interrupt_state_reply(void)
{
    kona_reset();
    guest_fill(qts, RETURN_PAGE, 0x5a, 0x40);
    guest_fill_word(qts, RETURN_PAGE, 0);
    kona_register_return_page(qts, RETURN_PAGE);
    video_command(qts, 0x3d, (uint32_t[]){ 0 }, 1, 0x3d);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE), ==, 1);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 4), ==, 0x5a5a5a5a);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8), ==, 17);
    for (unsigned w = 0; w < 3; w++) {
        g_assert_cmphex(guest_word(qts, RETURN_PAGE + 12 + 4 * w), ==, 0);
    }
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 24), ==, 17);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 28), ==, 0x5a5a5a5a);
    assert_textport_in_step(qts);
}

/*
 * An 8-byte store of 0x1e9:0 at +0xc10000 with no command open frames as
 * 0x1e9 then 0, which answers on the return page; the reverse order, or
 * the low word landing at +0xc10004, would leave 0x1e9 waiting for its
 * word. The trace records the store at its raw width. Then the grammar's
 * doubleword data test: an 8-byte store of A:B inside an open load
 * counts as two entries, A then B, here a gamma load of two entries.
 *
 * Every other 8-byte access splits into two 4-byte accesses, high word
 * at the lower address, as the core split a wide qtest access before the
 * widget took 8 bytes, and is logged: two map RAM entries take the two
 * halves, a store at +0xc10008 drops its high word as an unexpected
 * store and frames its low word, 0x1e9, from the data slot, and a load
 * splits too.
 */
static void test_doubleword_stores(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "guest_errors,unimp",
                                     &log);

    kona_register_return_page(s, RETURN_PAGE);
    guest_fill_word(s, RETURN_PAGE, 0);
    qtest_writeq(s, REG_COMMAND, UINT64_C(0x000001e900000000));
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 1);
    assert_textport_in_step(s);

    guest_fill_word(s, RETURN_PAGE, 0);
    kona_command(s, 0x12f, (uint32_t[]){ 0x2000, 2 }, 2);
    qtest_writeq(s, REG_COMMAND, UINT64_C(0x0000012300000456));
    g_assert_cmphex(kona_gamma(s, 0x2000), ==, 0x123);
    g_assert_cmphex(kona_gamma(s, 0x2001), ==, 0x456);
    assert_textport_in_step(s);

    qtest_writeq(s, REG_MAPRAM(0), UINT64_C(0x2000123420005678));
    g_assert_cmphex(qtest_readl(s, REG_MAPRAM(0)), ==, 0x20001234);
    g_assert_cmphex(qtest_readl(s, REG_MAPRAM(1)), ==, 0x20005678);
    g_assert_cmphex(qtest_readq(s, REG_MAPRAM(0)), ==,
                    UINT64_C(0x2000123420005678));
    qtest_writeq(s, KONA_REG(0xc10008), UINT64_C(0x00000333000001e9));
    qtest_writel(s, REG_COMMAND_DATA, 0);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 1);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "offset 0xc10000 value 0x1e900000000 "
                                   "size 8"));
    g_assert_nonnull(strstr(found, "sgi_kona_gamma_load_done start 0x2000 "
                                   "count 2 outside 0"));
    g_assert_cmpuint(count_occurrences(found, "split into two 4-byte "
                                       "accesses"), ==, 3);
    g_assert_nonnull(strstr(found, "8-byte store at offset 0x100000 "
                                   "split"));
    g_assert_nonnull(strstr(found, "8-byte load at offset 0x100000 "
                                   "split"));
    g_assert_nonnull(strstr(found, "8-byte store at offset 0xc10008 "
                                   "split"));
    g_assert_nonnull(strstr(found, "unexpected store of 0x333 at command "
                                   "page offset 0x8; dropped"));
    g_assert_null(strstr(found, "sync word"));
}

/*
 * The return page after a video reply with zero data: word 0 = 1, word 1
 * untouched, word 2 the reply id, `bytes` bytes of zeros, a checksum equal
 * to the id, and the word after it untouched. The caller filled the page
 * with 0x5a and cleared word 0 first.
 */
static void assert_zero_reply(QTestState *s, uint32_t id, uint32_t bytes)
{
    uint32_t words = bytes / 4;

    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 1);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 4), ==, 0x5a5a5a5a);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, id);
    for (unsigned w = 0; w < words; w++) {
        g_assert_cmphex(guest_word(s, RETURN_PAGE + 12 + 4 * w), ==, 0);
    }
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 12 + 4 * words), ==, id);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 16 + 4 * words), ==,
                    0x5a5a5a5a);
}

/* A fresh return page: 0x5a throughout, word 0 clear, registered. */
static void fresh_return_page(QTestState *s)
{
    guest_fill(s, RETURN_PAGE, 0x5a, 0x100);
    guest_fill_word(s, RETURN_PAGE, 0);
    kona_register_return_page(s, RETURN_PAGE);
}

/*
 * QueryMonitorName (0x50), which the X server sends once per channel at
 * extension initialisation and whose wait stopped the stock guest: one
 * data word, the channel's physical number, answered with
 * reply id 25, 68 zero bytes (a zero word 0 and an empty 64-byte name)
 * and the checksum 25, over what an earlier reply left in words 2 onward.
 * This is the generic-reply test.
 */
static void test_query_monitor_name_reply(void)
{
    kona_reset();
    fresh_return_page(qts);
    video_command(qts, 0x3d, (uint32_t[]){ 0 }, 1, 0x3d);
    guest_fill_word(qts, RETURN_PAGE, 0);
    video_command(qts, 0x50, (uint32_t[]){ 0 }, 1, 0x50);
    assert_zero_reply(qts, 25, 68);
    assert_textport_in_step(qts);
}

/*
 * The rest of the table is answered generically, at its own reply size,
 * whatever the grammar's basis for the row. GetVBOBReply (0x5c) is a
 * "tables only" row, its data length unknown and its 164-byte reply the
 * largest; here it carries two data words shaped like a video command's
 * number and data, framed by its checksum. LoadXilinx (0x52) was found
 * with six data words at its senders and waits for reply 27 of 4 bytes;
 * its data words are shaped like commands, so a model that took fewer
 * would open them. GetMasterClockSelect (0x2c), tables only, with no data
 * word before its checksum.
 */
static void test_generic_replies(void)
{
    kona_reset();
    fresh_return_page(qts);
    video_command(qts, 0x5c, (uint32_t[]){ 0x1a6, 0x12 }, 2,
                  0x5c + 0x1a6 + 0x12);
    assert_zero_reply(qts, 31, 164);
    assert_textport_in_step(qts);

    fresh_return_page(qts);
    video_command(qts, 0x52, (uint32_t[]){ 0x1e9, 0, 1, 9, 0, 0x128 }, 6,
                  0x52 + 0x1e9 + 1 + 9 + 0x128);
    assert_zero_reply(qts, 27, 4);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, 0);
    assert_textport_in_step(qts);

    fresh_return_page(qts);
    qtest_writel(qts, REG_COMMAND, TOKEN_VIDEO);
    qtest_writel(qts, REG_COMMAND, 0x2c);
    qtest_writel(qts, REG_COMMAND, 0x2c);
    assert_zero_reply(qts, 13, 4);
    assert_textport_in_step(qts);
}

/*
 * A command the table gives no length and no reply, VCWrite (0x1), framed
 * by the grammar's checksum search: every word that is not the number
 * plus the data words before it is data. Its five data words would be a
 * configuration request and a sync probe if read as commands; none acts,
 * nothing is posted, the search reports five words, and the page is in
 * step after the checksum.
 */
static void test_checksum_search_framing(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "guest_errors,unimp",
                                     &log);

    fresh_return_page(s);
    video_command(s, 0x1, (uint32_t[]){ 0x1e9, 0, 1, 9, 0 }, 5,
                  0x1 + 0x1e9 + 1 + 9);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 0);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, 0);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "command 0x1 VCWrite data 0x1e9 "
                                   "checksum 0x1f4"));
    g_assert_nonnull(strstr(found, "command 0x1 framed by its checksum "
                                   "after 5 data words"));
    g_assert_null(strstr(found, "sgi_kona_video_checksum_mismatch "));
    g_assert_null(strstr(found, "sgi_kona_host_write "));
    g_assert_null(strstr(found, "unmodelled"));
    g_assert_null(strstr(found, "sync word"));
}

/*
 * A counted command whose checksum is wrong still acts: GetSyncSelects
 * (0x24), one data word, is answered with reply 11 of 24 zero bytes, and
 * the mismatch is traced.
 */
static void test_bad_checksum_still_acts(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "guest_errors,unimp",
                                     &log);

    fresh_return_page(s);
    video_command(s, 0x24, (uint32_t[]){ 3 }, 1, 0xbad);
    assert_zero_reply(s, 11, 24);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "command 0x24 checksum 0xbad expected "
                                   "0x27; acted on anyway"));
    g_assert_nonnull(strstr(found, "command 0x24 reply id 11 data bytes "
                                   "24"));
}

/*
 * A search that never finds its checksum gives up after 1,024 data words,
 * the model's bound, rather than taking every later word as data: the
 * sync probe after it frames, and the give-up is logged.
 */
static void test_checksum_search_abandoned(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, "guest_errors,unimp",
                                     &log);

    qtest_writel(s, REG_COMMAND, TOKEN_VIDEO);
    qtest_writel(s, REG_COMMAND, 0x1);
    for (unsigned w = 0; w < 1024; w++) {
        qtest_writel(s, REG_COMMAND, 0);
    }
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "video command 0x1 has no checksum within "
                                   "1024 data words; closed, nothing "
                                   "posted"));
    g_assert_null(strstr(found, "sgi_kona_video_command "));
}

/*
 * A command framed by its checksum still searching at the migration
 * point: the destination must know the data words' sum to find the
 * checksum. GetVBOBReply with 5, 6 and 7 before the migration, then its
 * checksum 0x5c + 18, is answered on the destination.
 */
static void test_migration_mid_search(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-search-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    fresh_return_page(from);
    qtest_writel(from, REG_COMMAND_SLOT(TOKEN_VIDEO), 0x5c);
    qtest_writel(from, REG_COMMAND_DATA, 5);
    qtest_writel(from, REG_COMMAND_DATA, 6);
    qtest_writel(from, REG_COMMAND_DATA, 7);

    sgi_migrate_stopped(from, uri);

    qtest_writel(to, REG_COMMAND_DATA, 0x5c + 18);
    assert_zero_reply(to, 31, 164);
    assert_textport_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * A version 6 stream from before the X server's subsection: a video
 * command open with its one data word still completes, and the return
 * page crossed. Skipped unless QTEST_QEMU_BINARY_KONA_V6_PLANES names a
 * qemu-system-mips64 built with the board memory but before the
 * subsection, the pattern the version 4 and 5 cases use.
 */
static void test_migration_from_v6_planes(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    if (!g_getenv("QTEST_QEMU_BINARY_KONA_V6_PLANES")) {
        g_test_skip("QTEST_QEMU_BINARY_KONA_V6_PLANES is not set");
        return;
    }

    tmpfs = g_dir_make_tmp("sn0-kona-v6-planes-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init_ext("QTEST_QEMU_BINARY_KONA_V6_PLANES", MIGRATION_ARGS,
                          NULL, true);
    fresh_return_page(from);
    qtest_writel(from, REG_COMMAND_SLOT(TOKEN_VIDEO), GET_CHANNEL_CONFIG);
    qtest_writel(from, REG_COMMAND_DATA, 4);

    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);
    sgi_migrate_stopped(from, uri);

    qtest_writel(to, REG_COMMAND_DATA, GET_CHANNEL_CONFIG + 4);
    g_assert_cmphex(guest_word(to, RETURN_PAGE), ==, 1);
    g_assert_cmphex(guest_word(to, RETURN_PAGE + 8), ==, 7);
    g_assert_cmphex(guest_word(to, RETURN_PAGE + 12), ==, 1);
    g_assert_cmphex(guest_word(to, RETURN_PAGE + 12 + 4 * 19), ==, 8);
    assert_textport_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qts = qtest_init(MACHINE_ARGS);

    qtest_add_func("sn0/origin2000/kona-video/framing-and-checksum",
                   test_framing_and_checksum);
    qtest_add_func("sn0/origin2000/kona-video/x-server-video-init-no-reply",
                   test_x_server_video_init_no_reply);
    qtest_add_func("sn0/origin2000/kona-video/unknown-video-command",
                   test_unknown_video_command);
    qtest_add_func("sn0/origin2000/kona-video/slot-store-while-open",
                   test_slot_store_while_open);
    qtest_add_func("sn0/origin2000/kona-video/cursor-load-nonzero-slot",
                   test_cursor_load_nonzero_slot);
    qtest_add_func("sn0/origin2000/kona-video/reset-clears-open-command",
                   test_reset_clears_open_command);
    qtest_add_func("sn0/origin2000/kona-video/board-config-reply",
                   test_board_config_reply);
    qtest_add_func("sn0/origin2000/kona-video/channel-config-reply",
                   test_channel_config_reply);
    qtest_add_func("sn0/origin2000/kona-video/zero-data-replies",
                   test_zero_data_replies);
    qtest_add_func("sn0/origin2000/kona-video/migration-mid-video-command",
                   test_migration_mid_video_command);
    qtest_add_func("sn0/origin2000/kona-video/migration-from-v4",
                   test_migration_from_v4);
    qtest_add_func("sn0/origin2000/kona-video/gamma-load-small",
                   test_gamma_load_small);
    qtest_add_func("sn0/origin2000/kona-video/gamma-load-full",
                   test_gamma_load_full);
    qtest_add_func("sn0/origin2000/kona-video/gamma-entry-not-video",
                   test_gamma_entry_not_video);
    qtest_add_func("sn0/origin2000/kona-video/reset-clears-gamma",
                   test_reset_clears_gamma);
    qtest_add_func("sn0/origin2000/kona-video/migration-mid-gamma-load",
                   test_migration_mid_gamma_load);
    qtest_add_func("sn0/origin2000/kona-video/migration-from-v5",
                   test_migration_from_v5);
    qtest_add_func("sn0/origin2000/kona-video/x-tokens-consumed",
                   test_x_tokens_consumed);
    qtest_add_func("sn0/origin2000/kona-video/reset-clears-cursor-load",
                   test_reset_clears_cursor_load);
    qtest_add_func("sn0/origin2000/kona-video/migration-mid-cursor-load",
                   test_migration_mid_cursor_load);
    qtest_add_func("sn0/origin2000/kona-video/dg-interrupt-state-reply",
                   test_dg_interrupt_state_reply);
    qtest_add_func("sn0/origin2000/kona-video/doubleword-stores",
                   test_doubleword_stores);
    qtest_add_func("sn0/origin2000/kona-video/query-monitor-name-reply",
                   test_query_monitor_name_reply);
    qtest_add_func("sn0/origin2000/kona-video/generic-replies",
                   test_generic_replies);
    qtest_add_func("sn0/origin2000/kona-video/checksum-search-framing",
                   test_checksum_search_framing);
    qtest_add_func("sn0/origin2000/kona-video/bad-checksum-still-acts",
                   test_bad_checksum_still_acts);
    qtest_add_func("sn0/origin2000/kona-video/checksum-search-abandoned",
                   test_checksum_search_abandoned);
    qtest_add_func("sn0/origin2000/kona-video/migration-mid-search",
                   test_migration_mid_search);
    qtest_add_func("sn0/origin2000/kona-video/migration-from-v6-planes",
                   test_migration_from_v6_planes);

    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
